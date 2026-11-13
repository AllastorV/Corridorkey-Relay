# CorridorKey 2.0 OFX

An unofficial OpenFX plugin that brings [CorridorKey 2.0](https://github.com/nikopueringer/CorridorKey2.0)
into DaVinci Resolve (Edit, Fusion and Color pages) on Windows.

> Not affiliated with Corridor Digital. For the official, supported plugin see
> [corridorkey.com](https://corridorkey.com).

CorridorKey 2.0 is a temporal video model: it looks at many frames at once. A per-frame live effect
cannot run it, so this plugin works in two steps:

1. **Key Clip** reads the clip's frames once and starts CorridorKey 2.0 in the background (Python +
   PyTorch, your NVIDIA GPU). A progress bar is drawn over the picture while it works.
2. When it is done, press **Refresh**. The keyed result plays from a disk cache. View, despill and matte
   levels are applied live, without keying again.

## Requirements

- Windows 10/11, DaVinci Resolve (Studio or free) with OpenFX
- NVIDIA RTX 30 series or newer, 4 GB VRAM (1440p) / 6 GB (4K)
- Python 3.12 with the `py` launcher, Git
- ~10 GB disk for the runtime (PyTorch + weights) and space for the cache

## Install

1. Install the runtime (CorridorKey 2.0 code at a pinned commit, PyTorch cu128, weights from Hugging Face):

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\install_runtime.ps1
   ```

   It goes to `%LOCALAPPDATA%\CorridorKey2OFX\runtime`. Use `-RuntimeDir <path>` for another folder and
   `-Latest` for the newest CorridorKey 2.0 commit.
2. Install the plugin: run `scripts\install_plugin.bat` (asks for administrator rights). It copies
   `CorridorKey2.ofx.bundle` to `C:\Program Files\Common Files\OFX\Plugins`.
3. Restart Resolve. The effect is under **OpenFX > Keying > CorridorKey 2.0**.

## Build from source

Visual Studio 2022 (or Build Tools) with the C++ workload, CMake 3.24+, Ninja:

```bat
build.bat
```

The OpenFX headers are downloaded by CMake. Output: `build\CorridorKey2.ofx.bundle`.

## Controls

| Group | Control | When it applies |
|---|---|---|
| Process | Key Clip, Refresh, Cancel, Open Cache Folder, Clear Cache (to Recycle Bin), Status | |
| Controls | Linear input, Resolution (Full/Half/Quarter), GPU memory (Auto/Manual), Temporal window, Low VRAM | next Key Clip |
| Hint | Alpha hint (None / Reference frame / Every frame), Hint from (Chroma key / Hint input), Hint channel, Reference frame + Set Reference to Current Frame, Screen colour, Similarity, Softness, Hint quality, Carry hint, Carry quality | next Key Clip |
| Output | View (Cutout premultiplied / straight, Alpha, Foreground, Checker, Source), Clip black, Clip white | live |
| Despill | Screen (green/blue), Algorithm (Average/Max/Min), Despill | live |

If you change a "next Key Clip" setting after keying, the Status says so; the old result stays until you key
again.

## Cache

`%LOCALAPPDATA%\CorridorKey2OFX\cache\<job>` holds one half-float RGBA file per frame at the model
resolution (about 8 bytes per pixel per frame). Delete old job folders to free space. `plugin.log` in the
cache folder and `job.log` / `python.log` in each job folder help with problems.

## Licence

The plugin code in this repository is MIT licensed (see `LICENSE`).

CorridorKey 2.0 itself is **not** included. `install_runtime.ps1` downloads it from its own repository; it is
under the CorridorKey licence (a variation of CC BY-NC-SA 4.0 with additional terms, see its `LICENSE`).
In short, per its README: output may be used commercially; organisations with more than US $20 million
annual revenue must buy a seat of the official plugin per user. The foundation models it downloads have
their own licences (C-RADIOv4: NVIDIA Open Model License, MoGe: MIT, RVM: Apache 2.0 / CC-BY 4.0). See
`THIRD_PARTY.md`.

## Workflow notes

- **Reference frame hint**: the hint is taken at the reference frame and keyed forward to the end and
  backward to the start, so the best-looking frame can guide the whole shot.
- **Hint input**: in general context (Color page node, Fusion) the effect has a second input. Connect a
  matte (Magic Mask, Qualifier, window, a roto) and set *Hint from* to *Hint input*.
- **Several shots**: press Key Clip on as many clips as you like. Jobs wait in a queue and use the GPU one
  at a time.
- **Out of GPU memory**: the job retries on its own with Low VRAM and a smaller temporal window; the Status
  says so.
- **Trimmed or changed clips**: the Status tells you when the clip range or a keying setting changed after
  keying. Press Key Clip again.
- **Rendering / delivery**: the effect reads only the cache while delivering, so key every clip before you
  render.

## Development

The keying job can be tested without Resolve:

```powershell
$rt = "$env:LOCALAPPDATA\CorridorKey2OFX\runtime"
& "$rt\venv\Scripts\python.exe" tests\make_test_job.py clip.mp4 "$env:TEMP\ck2test" 2 first 15
& "$rt\venv\Scripts\python.exe" server\ck2_job.py "$env:TEMP\ck2test" $rt
& "$rt\venv\Scripts\python.exe" tests\view_result.py "$env:TEMP\ck2test" 15 preview.png
```

Arguments of `make_test_job.py`: video, job folder, scale (1/2/4), hint mode (none/first/video), reference
frame.

## Tests

```powershell
build.bat; build\test_plugin.exe
& "$env:LOCALAPPDATA\CorridorKey2OFX\runtime\venv\Scripts\python.exe" -m unittest tests.test_job
$env:CK2_GPU_TESTS = "1"; & "$env:LOCALAPPDATA\CorridorKey2OFX\runtime\venv\Scripts\python.exe" -m unittest tests.test_job.RealModel
```

`test_plugin.exe` drives the plugin through a fake OFX host (half-float round trip of all 65536 values,
orientation, render scale, partial render windows, offset origins, NaN/Inf input, 8-thread renders,
concurrent frame collection, broken caches). `test_job` checks the keying job with a fake runtime
(two-pass reference frames, OOM retries, GPU queue, cancel); `RealModel` runs the real model on the GPU.
