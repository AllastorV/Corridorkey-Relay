import importlib
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from pathlib import Path

os.environ["OPENCV_IO_ENABLE_OPENEXR"] = "1"

import cv2
import numpy as np

HERE = Path(__file__).resolve().parent
SERVER = HERE.parent / "server"
GPU = os.environ.get("CK2_GPU_TESTS") == "1"
RUNTIME = Path(os.environ.get("CORRIDORKEY2_RUNTIME",
                              Path(os.environ.get("LOCALAPPDATA", "")) / "CorridorKey2OFX" / "runtime"))


class FakeRuntime:
    def __init__(self):
        self.calls = []
        self.fail_first = 0

    def install(self):
        rt = types.ModuleType("corridorkey.runtime")
        vio = types.ModuleType("corridorkey.video_io")
        pkg = types.ModuleType("corridorkey")
        fake = self

        class InferenceConfig:
            def __init__(self, **kw):
                self.auto_vram = False
                self.hann_chunk = 80
                self.hann_stride = 40
                self.low_vram = False
                self.__dict__.update(kw)

        class RuntimePaths:
            @classmethod
            def from_package(cls, root):
                return cls()

            def validate(self):
                pass

        def run_inference(model, frames, out, cfg, writer, initial_hint_path=None, hint_video_path=None):
            fake.calls.append({"frames": frames, "initial": initial_hint_path, "video": hint_video_path,
                               "video_pngs": len(list(Path(hint_video_path).glob("*.png"))) if hint_video_path else 0,
                               "low_vram": cfg.low_vram, "chunk": cfg.hann_chunk, "auto": cfg.auto_vram})
            if fake.fail_first > 0:
                fake.fail_first -= 1
                raise RuntimeError("CUDA out of memory. Tried to allocate 2.00 GiB")
            clip = Path(frames).parent.name
            a_dir, f_dir = Path(out) / f"{clip}_alpha_exr", Path(out) / f"{clip}_fg_exr"
            a_dir.mkdir(parents=True, exist_ok=True)
            f_dir.mkdir(parents=True, exist_ok=True)
            for j, p in enumerate(sorted(Path(frames).glob("*.exr"))):
                img = cv2.imread(str(p), cv2.IMREAD_UNCHANGED).astype(np.float32)
                cv2.imwrite(str(a_dir / f"alpha_{j:06d}.exr"), np.ascontiguousarray(img[:, :, 0]))
                cv2.imwrite(str(f_dir / f"fg_{j:06d}.exr"), img)

        rt.InferenceConfig = InferenceConfig
        rt.RuntimePaths = RuntimePaths
        rt.load_model = lambda paths, device="cuda": object()
        rt.prepare_runtime_environment = lambda paths: None
        rt.run_inference = run_inference
        vio.WriterConfig = lambda: None
        torch = types.ModuleType("torch")
        torch.cuda = types.SimpleNamespace(is_available=lambda: True, empty_cache=lambda: None)
        sys.modules.update({"corridorkey": pkg, "corridorkey.runtime": rt, "corridorkey.video_io": vio,
                            "torch": torch})


def load_job_module():
    if str(SERVER) not in sys.path:
        sys.path.insert(0, str(SERVER))
    return importlib.import_module("ck2_job")


def make_job(root: Path, count=6, w=32, h=18, hint=None, linear=False, frames=None):
    folder = root / "job"
    (folder / "in").mkdir(parents=True)
    for i in range(count):
        rgb = frames(i) if frames else np.full((h, w, 3), (i + 1) / 64.0, np.float32)
        rgb.astype(np.float16).tofile(folder / "in" / f"{i:05d}.raw")
    spec = {"width": w, "height": h, "full_w": w * 2, "full_h": h * 2, "x0": 0, "y0": 0, "count": count,
            "start": 0, "fps": 25, "linear": linear, "scale": 2,
            "memory": {"auto": True, "chunk": 8, "low_vram": False},
            "hint": hint or {"mode": "none"}, "carry_hint": True, "carry_quality": 0.95, "settings": "x"}
    (folder / "job.json").write_text(json.dumps(spec), encoding="utf-8")
    return folder


def read_out(folder: Path, i, w=32, h=18):
    return np.fromfile(folder / "out" / f"{i:05d}.rgba", dtype=np.float16).reshape(h, w, 4).astype(np.float32)


class PureFunctions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        FakeRuntime().install()
        cls.m = load_job_module()

    def test_srgb_encode_reference_points(self):
        v = self.m.srgb_encode(np.array([0.0, 0.0031308, 0.18, 1.0, -1.0], np.float32))
        np.testing.assert_allclose(v, [0.0, 0.04045, 0.46135, 1.0, 0.0], atol=2e-4)

    def test_chroma_alpha_screen_is_zero_far_is_one(self):
        key = [0.0, 0.694, 0.251]
        img = np.array([[key, [0.9, 0.2, 0.6], [0.5, 0.5, 0.5]]], np.float32)
        a = self.m.chroma_alpha(img, key, 0.12, 0.08)
        self.assertAlmostEqual(float(a[0, 0]), 0.0, places=5)
        self.assertEqual(float(a[0, 1]), 1.0)
        self.assertGreater(float(a[0, 2]), 0.5)

    def test_chroma_alpha_zero_softness_is_binary_and_finite(self):
        img = np.random.default_rng(1).uniform(-2, 3, (8, 8, 3)).astype(np.float32)
        a = self.m.chroma_alpha(img, [0, 1, 0], 0.1, 0.0)
        self.assertTrue(set(np.unique(a)).issubset({0.0, 1.0}))
        self.assertTrue(np.isfinite(a).all())

    def test_hint_frames_needed(self):
        f = self.m.hint_frames_needed
        self.assertEqual(f({"mode": "none"}, 10), set())
        self.assertEqual(f({"mode": "video"}, 3), {0, 1, 2})
        self.assertEqual(f({"mode": "first", "reference": 4}, 10), {4})
        self.assertEqual(f({"mode": "first", "reference": 99}, 10), {9})
        self.assertEqual(f({"mode": "first", "reference": -5}, 10), {0})

    def test_progress_text_is_monotonic_across_stages(self):
        f = self.m.progress_text
        steps = [("starting", 0, 30), ("preparing", 15, 30), ("preparing", 30, 30), ("queued", 0, 30),
                 ("loading", 0, 30), ("keying", 0, 30), ("keying", 29, 30), ("finishing", 30, 30),
                 ("done", 30, 30)]
        fracs = [f({"stage": s, "done": d, "total": t})[1] for s, d, t in steps]
        self.assertEqual(fracs, sorted(fracs))
        self.assertEqual(fracs[0], 0.0)
        self.assertEqual(fracs[-1], 1.0)
        self.assertEqual(f({"stage": "keying", "done": 12, "total": 30})[0], "Keying 12/30 frames")
        self.assertEqual(f({"stage": "keying", "done": 99, "total": 0})[1], 0.95)
        self.assertEqual(f({"stage": "error", "error": "Cancelled."}), ("Error: Cancelled.", None))

    def test_is_oom_follows_cause_chain_and_survives_cycles(self):
        inner = RuntimeError("CUDA out of memory")
        outer = ValueError("wrapped")
        outer.__cause__ = inner
        self.assertTrue(self.m.is_oom(outer))
        a, b = RuntimeError("a"), RuntimeError("b")
        a.__context__, b.__context__ = b, a
        self.assertFalse(self.m.is_oom(a))

    def test_subsequence_reverses(self):
        with tempfile.TemporaryDirectory() as d:
            src, dst = Path(d) / "s", Path(d) / "t"
            src.mkdir()
            for i in range(5):
                (src / f"{i:05d}.exr").write_text(str(i))
            self.m.subsequence(src, dst, [3, 2, 1, 0], ".exr")
            self.assertEqual([(dst / f"{j:05d}.exr").read_text() for j in range(4)], ["3", "2", "1", "0"])


class JobScenarios(unittest.TestCase):
    def setUp(self):
        self.fake = FakeRuntime()
        self.fake.install()
        self.m = load_job_module()
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def run_job(self, folder):
        job = self.m.Job(folder)
        self.m.run(job)
        return json.loads((folder / "progress.json").read_text())

    def assert_frames_in_order(self, folder, count):
        for i in range(count):
            out = read_out(folder, i)
            np.testing.assert_allclose(out[..., 3], (i + 1) / 64.0, atol=2e-3, err_msg=f"frame {i}")

    def test_no_hint_single_pass(self):
        folder = make_job(self.root)
        state = self.run_job(folder)
        self.assertEqual(state["stage"], "done")
        self.assertEqual(len(self.fake.calls), 1)
        self.assert_frames_in_order(folder, 6)
        self.assertTrue((folder / "done").exists())
        self.assertFalse((folder / "in").exists())

    def test_reference_frame_in_middle_maps_both_passes_back(self):
        folder = make_job(self.root, count=9, hint={"mode": "first", "source": "chroma", "reference": 4,
                                                     "color": [0, 1, 0], "similarity": 0.1, "softness": 0.1})
        self.run_job(folder)
        self.assertEqual(len(self.fake.calls), 2)
        self.assertTrue(all(c["initial"] and c["initial"].endswith("00000.png") for c in self.fake.calls))
        self.assert_frames_in_order(folder, 9)

    def test_reference_on_last_frame(self):
        folder = make_job(self.root, count=5, hint={"mode": "first", "reference": 4})
        self.run_job(folder)
        self.assert_frames_in_order(folder, 5)

    def test_single_frame_clip_with_out_of_range_reference(self):
        folder = make_job(self.root, count=1, hint={"mode": "first", "reference": 7})
        self.run_job(folder)
        self.assertEqual(len(self.fake.calls), 1)
        self.assert_frames_in_order(folder, 1)

    def test_video_hint(self):
        folder = make_job(self.root, count=4, hint={"mode": "video"})
        self.run_job(folder)
        self.assertTrue(self.fake.calls[0]["video"])
        self.assertEqual(self.fake.calls[0]["video_pngs"], 4)
        self.assert_frames_in_order(folder, 4)

    def test_hint_input_missing_frame_is_clear_error(self):
        folder = make_job(self.root, count=3, hint={"mode": "first", "source": "input", "reference": 1})
        with self.assertRaisesRegex(RuntimeError, "Hint input frame 1 is missing"):
            self.run_job(folder)

    def test_hint_input_used_when_present(self):
        folder = make_job(self.root, count=3, hint={"mode": "first", "source": "input", "reference": 0})
        np.full((18, 32, 1), 0.5, np.float16).tofile(folder / "in" / "hint_00000.raw")
        self.run_job(folder)
        self.assertEqual(len(self.fake.calls), 1)

    def test_oom_retries_with_low_vram_then_succeeds(self):
        self.fake.fail_first = 2
        folder = make_job(self.root)
        state = self.run_job(folder)
        self.assertEqual(state["stage"], "done")
        self.assertEqual([c["low_vram"] for c in self.fake.calls], [False, True, True])
        self.assertEqual([c["chunk"] for c in self.fake.calls][1:], [4, 1])
        self.assertIn("Low VRAM", state["note"])
        self.assert_frames_in_order(folder, 6)

    def test_oom_three_times_fails(self):
        self.fake.fail_first = 3
        folder = make_job(self.root)
        with self.assertRaises(RuntimeError):
            self.run_job(folder)
        self.assertFalse((folder / "done").exists())

    def test_cancel_marker_stops_before_keying(self):
        folder = make_job(self.root)
        (folder / "cancel").write_text("1")
        with self.assertRaises(self.m.Cancelled):
            self.run_job(folder)
        self.assertEqual(self.fake.calls, [])

    def test_missing_output_frames_fail(self):
        folder = make_job(self.root, count=4)
        real = sys.modules["corridorkey.runtime"].run_inference

        def short(model, frames, out, cfg, writer, **kw):
            real(model, frames, out, cfg, writer, **kw)
            clip = Path(frames).parent.name
            last = sorted((Path(out) / f"{clip}_alpha_exr").glob("*.exr"))[-1]
            last.rename(last.with_suffix(".gone"))

        sys.modules["corridorkey.runtime"].run_inference = short
        with self.assertRaisesRegex(RuntimeError, "Expected 4 frames"):
            self.run_job(folder)

    def test_linear_job_keeps_values_above_one(self):
        folder = make_job(self.root, count=2, linear=True, frames=lambda i: np.full((18, 32, 3), 4.0, np.float32))
        self.run_job(folder)
        self.assertGreater(float(read_out(folder, 0)[..., 0].max()), 3.9)

    def test_display_job_is_srgb_encoded(self):
        folder = make_job(self.root, count=1, frames=lambda i: np.full((18, 32, 3), 0.18, np.float32))
        self.run_job(folder)
        self.assertAlmostEqual(float(read_out(folder, 0)[0, 0, 0]), 0.4613, places=2)

    def test_nan_and_inf_input_do_not_crash(self):
        def bad(i):
            a = np.full((18, 32, 3), 0.5, np.float32)
            a[0, 0] = np.nan
            a[1, 1] = np.inf
            return a

        folder = make_job(self.root, count=2, hint={"mode": "video"}, frames=bad)
        self.assertEqual(self.run_job(folder)["stage"], "done")

    def test_gpu_queue_waits_for_other_job(self):
        shared = self.root / "shared"
        a_dir, b_dir = make_job(shared / "x"), make_job(shared / "y")
        a_job, b_job = self.m.Job(a_dir), self.m.Job(b_dir)
        b_job.folder = a_dir.parent / "job_b"
        b_job.folder.mkdir()
        slot = self.m.gpu_slot(a_job)
        got = []
        t = threading.Thread(target=lambda: got.append(self.m.gpu_slot(b_job)))
        t.start()
        time.sleep(1.5)
        self.assertEqual(got, [])
        self.assertEqual(json.loads((b_job.folder / "progress.json").read_text())["stage"], "queued")
        slot.close()
        t.join(5)
        self.assertEqual(len(got), 1)
        got[0].close()

    def test_main_reports_error_and_exit_code(self):
        folder = make_job(self.root, count=3, hint={"mode": "first", "source": "input", "reference": 0})
        argv = sys.argv
        sys.argv = ["ck2_job.py", str(folder)]
        try:
            code = self.m.main()
        finally:
            sys.argv = argv
        state = json.loads((folder / "progress.json").read_text())
        self.assertEqual(code, 1)
        self.assertEqual(state["stage"], "error")
        self.assertIn("Hint input", state["error"])
        self.assertTrue((folder / "pid").exists())


@unittest.skipUnless(GPU, "set CK2_GPU_TESTS=1 to run the real CorridorKey 2.0 model")
class RealModel(unittest.TestCase):
    def run_real(self, count, w, h, hint=None):
        with tempfile.TemporaryDirectory() as d:
            def frame(i):
                img = np.zeros((h, w, 3), np.float32)
                img[..., 1] = 0.7
                img[h // 4: 3 * h // 4, w // 3: 2 * w // 3] = (0.8, 0.5, 0.3)
                return img

            folder = make_job(Path(d), count=count, w=w, h=h, hint=hint, frames=frame)
            python = RUNTIME / "venv" / "Scripts" / "python.exe"
            code = subprocess.run([str(python), str(SERVER / "ck2_job.py"), str(folder), str(RUNTIME)],
                                  capture_output=True, text=True, timeout=1800).returncode
            state = json.loads((folder / "progress.json").read_text())
            self.assertEqual(code, 0, state)
            out = np.fromfile(folder / "out" / f"{count - 1:05d}.rgba", np.float16).reshape(h, w, 4)
            self.assertGreater(float(out[h // 2, w // 2, 3]), 0.5)
            self.assertLess(float(out[2, 2, 3]), 0.5)

    def test_single_frame(self):
        self.run_real(1, 256, 144)

    def test_odd_size_not_multiple_of_128(self):
        self.run_real(5, 333, 187)

    def test_reference_frame_chroma_hint(self):
        self.run_real(6, 256, 144, {"mode": "first", "source": "chroma", "reference": 3,
                                    "color": [0, 0.7, 0], "similarity": 0.1, "softness": 0.05})


if __name__ == "__main__":
    unittest.main(verbosity=2)
