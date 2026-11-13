import json
import os
import shutil
import sys
import tempfile
import threading
import time
import traceback
from pathlib import Path

os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
os.environ.setdefault("HF_HUB_OFFLINE", "1")
os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")

import cv2
import numpy as np

CK2 = Path(sys.argv[2] if len(sys.argv) > 2 else Path(__file__).resolve().parents[1]) / "ck2"
sys.path.insert(0, str(CK2))


class Cancelled(Exception):
    pass


class Job:
    def __init__(self, folder: Path):
        self.folder = folder
        self.spec = json.loads((folder / "job.json").read_text(encoding="utf-8"))
        self.state = {"stage": "starting", "done": 0, "total": int(self.spec["count"]), "error": None,
                      "note": None}
        self.lock = threading.Lock()

    def report(self, **changes):
        with self.lock:
            self.state.update(changes)
            tmp = self.folder / "progress.json.tmp"
            tmp.write_text(json.dumps(self.state), encoding="utf-8")
            for _ in range(40):
                try:
                    os.replace(tmp, self.folder / "progress.json")
                    break
                except PermissionError:
                    time.sleep(0.05)

    def log(self, text):
        with open(self.folder / "job.log", "a", encoding="utf-8") as fh:
            fh.write(text.rstrip() + "\n")

    def check(self):
        if (self.folder / "cancel").exists():
            raise Cancelled()


def srgb_encode(x):
    x = np.maximum(x, 0.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)


def read_raw(path: Path, w: int, h: int, channels: int):
    return np.fromfile(path, dtype=np.float16).reshape(h, w, channels).astype(np.float32)


def chroma_alpha(rgb, key, similarity, softness):
    m = np.array([[-0.1146, -0.3854, 0.5], [0.5, -0.4542, -0.0458]], dtype=np.float32)
    uv = np.clip(rgb, 0.0, 1.0) @ m.T
    kv = np.asarray(key, dtype=np.float32) @ m.T
    d = np.sqrt(((uv - kv) ** 2).sum(axis=2) / 2.0)
    if softness <= 1e-4:
        return (d > similarity).astype(np.float32)
    return np.clip((d - similarity) / softness, 0.0, 1.0)


def hint_frames_needed(hint, count):
    mode = hint.get("mode", "none")
    if mode == "video":
        return set(range(count))
    if mode == "first":
        return {min(max(int(hint.get("reference", 0)), 0), count - 1)}
    return set()


def stage_frames(job: Job, frames_dir: Path, hint_dir: Path):
    spec = job.spec
    w, h, count = int(spec["width"]), int(spec["height"]), int(spec["count"])
    linear = bool(spec.get("linear"))
    hint = spec.get("hint") or {}
    needed = hint_frames_needed(hint, count)
    source = hint.get("source", "chroma")
    frames_dir.mkdir(parents=True, exist_ok=True)
    hint_dir.mkdir(parents=True, exist_ok=True)
    params = [cv2.IMWRITE_EXR_TYPE, cv2.IMWRITE_EXR_TYPE_HALF]
    for i in range(count):
        job.check()
        rgb = read_raw(job.folder / "in" / f"{i:05d}.raw", w, h, 3)
        cv2.imwrite(str(frames_dir / f"{i:05d}.exr"), cv2.cvtColor(rgb, cv2.COLOR_RGB2BGR), params)
        if i in needed:
            if source == "input":
                path = job.folder / "in" / f"hint_{i:05d}.raw"
                if not path.exists():
                    raise RuntimeError(f"Hint input frame {i} is missing. Connect a matte to the Hint input.")
                a = np.clip(read_raw(path, w, h, 1)[..., 0], 0.0, 1.0)
            else:
                display = np.clip(srgb_encode(rgb), 0, 1) if linear else np.clip(rgb, 0, 1)
                a = chroma_alpha(display, hint.get("color", [0, 1, 0]), float(hint.get("similarity", 0.12)),
                                 float(hint.get("softness", 0.08)))
            cv2.imwrite(str(hint_dir / f"{i:05d}.png"), (a * 65535.0 + 0.5).astype(np.uint16))
        job.report(stage="preparing", done=i + 1)


def link(src: Path, dst: Path):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def subsequence(src_dir: Path, dst_dir: Path, order, ext):
    dst_dir.mkdir(parents=True, exist_ok=True)
    for j, i in enumerate(order):
        p = src_dir / f"{i:05d}{ext}"
        if p.exists():
            link(p, dst_dir / f"{j:05d}{ext}")


class Prints:
    def __init__(self, job: Job):
        self.job, self.buffer = job, ""

    def write(self, text):
        self.buffer += text
        while "\n" in self.buffer:
            line, self.buffer = self.buffer.split("\n", 1)
            if line.strip():
                self.job.log(line)
        return len(text)

    def flush(self):
        pass


def watch(job: Job, alpha_dir: Path, offset: int, stop: threading.Event):
    while not stop.wait(0.5):
        if alpha_dir.is_dir():
            job.report(stage="keying", done=offset + len(list(alpha_dir.glob("*.exr"))))


def is_oom(exc):
    seen = set()
    while exc is not None and id(exc) not in seen:
        seen.add(id(exc))
        if "out of memory" in f"{type(exc).__name__} {exc}".lower():
            return True
        exc = exc.__cause__ or exc.__context__
    return False


def gpu_slot(job: Job):
    import msvcrt

    fh = open(job.folder.parent / "gpu.lock", "a+b")
    waited = False
    while True:
        job.check()
        try:
            fh.seek(0)
            msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
            return fh
        except OSError:
            if not waited:
                job.report(stage="queued", done=0)
                waited = True
            time.sleep(1.0)


def make_cfg(spec, work: Path, attempt: int):
    from corridorkey.runtime import InferenceConfig

    hint = spec.get("hint") or {}
    cfg = InferenceConfig(
        hint_quality=float(hint.get("quality", 0.95)),
        carry_hint=bool(spec.get("carry_hint", True)),
        carry_hint_quality=float(spec.get("carry_quality", 0.95)),
        outputs={"alpha", "fg"},
        output_format="exr",
        exr_dtype="float",
        frame_dir_fps=float(spec.get("fps", 24.0)),
        frame_dir_linear=bool(spec.get("linear")),
        despill_strength=0.0,
        temp_dir=work / "tmp",
    )
    memory = spec.get("memory") or {}
    if attempt == 0 and memory.get("auto", True):
        cfg.auto_vram = True
    elif attempt == 0:
        cfg.hann_chunk = int(memory.get("chunk", 8))
        cfg.hann_stride = max(1, cfg.hann_chunk // 2)
        cfg.low_vram = bool(memory.get("low_vram", False))
    else:
        cfg.hann_chunk = 4 if attempt == 1 else 1
        cfg.hann_stride = max(1, cfg.hann_chunk // 2)
        cfg.low_vram = True
    return cfg


def run_pass(job: Job, model, frames: Path, hint_dir: Path, mode: str, out_dir: Path, offset: int):
    import torch
    from corridorkey.runtime import run_inference
    from corridorkey.video_io import WriterConfig

    clip = frames.parent.name
    alpha_dir = out_dir / f"{clip}_alpha_exr"
    for attempt in range(3):
        job.check()
        shutil.rmtree(out_dir, ignore_errors=True)
        cfg = make_cfg(job.spec, frames.parent, attempt)
        initial = str(hint_dir / "00000.png") if mode == "first" else None
        video = str(hint_dir) if mode == "video" else None
        stop = threading.Event()
        watcher = threading.Thread(target=watch, args=(job, alpha_dir, offset, stop), daemon=True)
        watcher.start()
        old = sys.stdout
        sys.stdout = Prints(job)
        try:
            run_inference(model, str(frames), str(out_dir), cfg, WriterConfig(), initial_hint_path=initial,
                          hint_video_path=video)
            return clip
        except Exception as exc:
            if not is_oom(exc) or attempt == 2:
                raise
            job.log(f"out of GPU memory, retrying with low VRAM (attempt {attempt + 2})")
            job.report(note="Out of GPU memory: retried with Low VRAM and a smaller temporal window.")
            torch.cuda.empty_cache()
        finally:
            sys.stdout = old
            stop.set()
    raise RuntimeError("unreachable")


def convert(job: Job, out_dir: Path, clip: str, mapping, done_before: int):
    alphas = sorted((out_dir / f"{clip}_alpha_exr").glob("*.exr"))
    fgs = sorted((out_dir / f"{clip}_fg_exr").glob("*.exr"))
    need = max(j for j, _ in mapping) + 1
    if len(alphas) < need or len(fgs) < need:
        raise RuntimeError(f"Expected {need} frames, got alpha {len(alphas)} / fg {len(fgs)}.")
    target = job.folder / "out"
    target.mkdir(exist_ok=True)
    linear = bool(job.spec.get("linear"))
    w, h = int(job.spec["width"]), int(job.spec["height"])
    for n, (j, i) in enumerate(mapping):
        job.check()
        a = cv2.imread(str(alphas[j]), cv2.IMREAD_UNCHANGED).astype(np.float32)
        a = a if a.ndim == 2 else a[:, :, 0]
        fg = cv2.cvtColor(cv2.imread(str(fgs[j]), cv2.IMREAD_UNCHANGED).astype(np.float32), cv2.COLOR_BGR2RGB)
        if not linear:
            fg = srgb_encode(fg)
        if a.shape != (h, w):
            raise RuntimeError(f"Output size {a.shape[1]}x{a.shape[0]}, expected {w}x{h}.")
        rgba = np.dstack([fg, np.clip(a, 0.0, 1.0)]).astype(np.float16)
        rgba.tofile(target / f"{i:05d}.rgba")
        job.report(stage="finishing", done=done_before + n + 1)


def run(job: Job):
    import torch
    from corridorkey.runtime import RuntimePaths, load_model, prepare_runtime_environment

    spec = job.spec
    count = int(spec["count"])
    hint = spec.get("hint") or {}
    mode = hint.get("mode", "none")
    ref = min(max(int(hint.get("reference", 0)), 0), count - 1) if mode == "first" else 0
    work = Path(tempfile.mkdtemp(prefix="ck2_", dir=job.folder))
    try:
        frames_dir, hint_dir = work / "frames", work / "hint"
        job.report(stage="preparing", done=0)
        stage_frames(job, frames_dir, hint_dir)
        slot = gpu_slot(job)
        try:
            if not torch.cuda.is_available():
                raise RuntimeError("CUDA is not available: CorridorKey 2.0 needs an NVIDIA RTX GPU.")
            job.report(stage="loading", done=0)
            paths = RuntimePaths.from_package(str(CK2))
            paths.validate()
            prepare_runtime_environment(paths)
            model = load_model(paths, device="cuda")
            passes = [list(range(ref, count))]
            if ref > 0:
                passes.append(list(range(ref, -1, -1)))
            job.report(stage="keying", done=0, total=sum(len(p) for p in passes))
            results, offset = [], 0
            for n, order in enumerate(passes):
                seq = work / f"pass{n}"
                subsequence(frames_dir, seq / "frames", order, ".exr")
                if mode != "none":
                    subsequence(hint_dir, seq / "hint", order, ".png")
                clip = run_pass(job, model, seq / "frames", seq / "hint", mode, seq / "out", offset)
                mapping = [(k, i) for k, i in enumerate(order)] if n == 0 else \
                    [(k, i) for k, i in enumerate(order) if k > 0]
                results.append((seq / "out", clip, mapping))
                offset += len(order)
            del model
            torch.cuda.empty_cache()
        finally:
            slot.close()
        done = 0
        job.report(stage="finishing", done=0, total=count)
        for out_dir, clip, mapping in results:
            convert(job, out_dir, clip, mapping, done)
            done += len(mapping)
        missing = [i for i in range(count) if not (job.folder / "out" / f"{i:05d}.rgba").exists()]
        if missing:
            raise RuntimeError(f"{len(missing)} frames were not keyed (first: {missing[0]}).")
        shutil.rmtree(job.folder / "in", ignore_errors=True)
        (job.folder / "done").write_text("ok", encoding="utf-8")
        job.report(stage="done", done=count, total=count)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    folder = Path(sys.argv[1])
    job = Job(folder)
    (folder / "pid").write_text(str(os.getpid()), encoding="utf-8")
    try:
        run(job)
        return 0
    except Cancelled:
        job.report(stage="error", error="Cancelled.")
        return 2
    except Exception as exc:
        text = f"{type(exc).__name__}: {exc}"
        if is_oom(exc):
            text = "Out of GPU memory even in Low VRAM mode. Use a lower Resolution or close other GPU apps. " + text
        job.log(traceback.format_exc())
        job.report(stage="error", error=text[:400])
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
