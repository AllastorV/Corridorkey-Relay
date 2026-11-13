# CorridorKey Relay

A free, unofficial OpenFX plugin that relays your DaVinci Resolve clips to
[CorridorKey 2.0](https://github.com/nikopueringer/CorridorKey2.0) and brings the keyed result back into
the Edit, Fusion and Color pages, on Windows.

> **Credit.** All of the keying is done by **CorridorKey 2.0**, made by Niko Pueringer and
> [Corridor Digital](https://www.youtube.com/@CorridorCrew). This plugin only moves frames between Resolve
> and their model. It is not affiliated with or endorsed by Corridor Digital. For the official, supported
> plugin see [corridorkey.com](https://corridorkey.com).

CorridorKey 2.0 is a temporal video model: it looks at many frames at once. A per-frame live effect
cannot run it, so this plugin works in two steps:

1. **Key Clip** reads the clip's frames once and starts CorridorKey 2.0 in the background (Python +
   PyTorch, your NVIDIA GPU). A small progress window opens in the corner of Resolve's monitor with the
   current stage, a progress bar and a Cancel button; it closes when the job ends.
2. When it is done, press **Refresh**. The keyed result plays from a disk cache. View, despill and matte
   levels are applied live, without keying again.

## Requirements

- Windows 10/11, DaVinci Resolve (Studio or free) with OpenFX
- NVIDIA RTX 30 series or newer, 4 GB VRAM (1440p) / 6 GB (4K)
- Python 3.12 with the `py` launcher, Git
- ~10 GB disk for the runtime (PyTorch + weights) and space for the cache

## Install

**One click:** download `CorridorKey-Relay-<version>-Setup.exe` from the Releases page and run it (close
Resolve first). It installs the plugin and, if the task is ticked, downloads CorridorKey 2.0 from its
official repository (needs Git and Python 3.12; the setup tells you if they are missing). Uninstall from
Windows Settings > Apps; the downloaded CorridorKey 2.0 runtime in `%LOCALAPPDATA%\CorridorKey2OFX` is
kept so a reinstall does not download it again.

**Manual:**

1. Install the runtime (CorridorKey 2.0 code at a pinned commit, PyTorch cu128, weights from Hugging Face):

   ```powershell
   powershell -ExecutionPolicy Bypass -File scripts\install_runtime.ps1
   ```

   It goes to `%LOCALAPPDATA%\CorridorKey2OFX\runtime`. Use `-RuntimeDir <path>` for another folder and
   `-Latest` for the newest CorridorKey 2.0 commit.
   The script clones the original repository, `github.com/nikopueringer/CorridorKey2.0`, and its weight
   downloader; nothing from CorridorKey is mirrored here.
2. Install the plugin: run `scripts\install_plugin.bat` (asks for administrator rights). It copies
   `CorridorKey2.ofx.bundle` to `C:\Program Files\Common Files\OFX\Plugins`.
3. Restart Resolve. The effect is under **OpenFX > Keying > CorridorKey Relay**.

## Build from source

Visual Studio 2022 (or Build Tools) with the C++ workload, CMake 3.24+, Ninja:

```bat
build.bat
```

The OpenFX headers are downloaded by CMake. Output: `build\CorridorKey2.ofx.bundle`.

Setup EXE: build first, then compile `installer\CorridorKeyRelay.iss` with Inno Setup 6
(`ISCC.exe installer\CorridorKeyRelay.iss`). Output: `build\dist\CorridorKey-Relay-<version>-Setup.exe`.

## Controls

| Group | Control | When it applies |
|---|---|---|
| Process | Key Clip, Refresh, Cancel, Open Cache Folder, Clear Cache (to Recycle Bin), Status | |
| Controls | Linear input, Resolution (Full/Half/Quarter), GPU memory (Auto/Manual/VRAM limit), VRAM limit (GB), Temporal window, Low VRAM | next Key Clip |
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

CorridorKey Relay builds on CorridorKey 2.0, so, as the CorridorKey licence requires for derivative works,
it is released under the **same CorridorKey licence** (a variation of CC BY-NC-SA 4.0 with additional
terms), reproduced in full in `LICENSE`. It is free of charge and keeps the CorridorKey name.

CorridorKey 2.0 itself is not included; `install_runtime.ps1` downloads it from its own repository. In short,
per the licence: what you key may be used commercially; this plugin may not be sold, repackaged or offered
as a paid service; anything you release that builds on it must stay free, under this licence and keep the
CorridorKey name; organisations with more than US $20 million annual revenue must buy a seat of the
official CorridorKey plugin for each user ([corridorkey.com](https://corridorkey.com)). The foundation models it downloads have
their own licences (C-RADIOv4: NVIDIA Open Model License, MoGe: MIT, RVM: Apache 2.0 / CC-BY 4.0). See
`THIRD_PARTY.md`.

## Workflow notes

- **Reference frame hint**: the hint is taken at the reference frame and keyed forward to the end and
  backward to the start, so the best-looking frame can guide the whole shot.
- **Hint input**: in general context (Color page node, Fusion) the effect has a second input. Connect a
  matte (Magic Mask, Qualifier, window, a roto) and set *Hint from* to *Hint input*. On the Color page,
  connect both the RGB and the key output of the matte node to the effect's second input pair: Resolve
  treats the Hint input as unconnected when only the key link is made. Use *Hint channel: Alpha* for a key
  link, *Luminance* for a black-and-white image.
- **Several shots**: press Key Clip on as many clips as you like. Jobs wait in a queue and use the GPU one
  at a time.
- **VRAM limit**: set *GPU memory* to *VRAM limit* and move the slider (3.5 GB up to your card's memory) to
  cap how much GPU memory the keyer may use in total, CUDA included, for example to leave room for Resolve. The keyer picks the
  largest temporal window that fits and never goes over the limit.
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
