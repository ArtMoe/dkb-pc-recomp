# Testing DKB Recomp

This guide is for running DKB Recomp on your own PC and reporting what happens. You build it from source with your own
copy of the game. Nothing from the game is in this repository, and nothing you build from it may be shared: not the
executable, not the generated code, not the shader cache.

## What you need

- Windows 10 or 11 on an x86-64 CPU with AVX2.
- 16 GB of RAM. The game uses about 13 GB, so close other large programs first.
- A GPU with Vulkan 1.3 and a current driver.
- About 15 GB free for the build (the packed RomFS is about 9 GB), besides the space your dump takes.
- *Donkey Kong Bananza* base version 1.0.0, dumped from your own Switch 2: the ExeFS (`main`, `sdk`, `main.npdm`) and
  the RomFS as a folder. Update 458752 is not supported yet. This project does not explain how to dump the game.
  If your dump is a single `.nup` file (the container Eglantine and Eden load), `tools/extract_nup.py` writes both
  folders from its base package: `python tools\extract_nup.py <game>.nup --list` shows the packages, and
  `python tools\extract_nup.py <game>.nup --out build\extracted\dkb100-dump` writes `exefs` and `romfs` there, checking
  every file's SHA-256. Pass those two folders to the commands in the README.

## Build

Install the toolchain from [building.md](building.md) (MSYS2 UCRT64 Clang, Ninja, Python 3.12 in `.venv`, .NET SDK),
then run the commands in the README, "Building from source". The executable ends up in `build\dkb100\dkb.exe`.

The generation command must include `--patches recomp\patches-dkb100.txt`. This enables the controller setup patch
that bypasses the game's HID server proxy, which the host does not implement. If you generated the code without
that argument, rerun the README's generation command with it and then rebuild with `tools\build_game.py`.
Rebuilding alone leaves the unpatched generated code in place and the game stops during controller setup.

## Run

```
build\dkb100\dkb.exe
```

Run it from inside the repository: it finds the shader translator (`build\xlate\totk_xlate.exe`) by looking up from
its own folder. The first run translates every shader it meets and is slow; later runs use the cache.

Prepare the shaders once (the Bananza installer does this as its `dkb_shaders` step): the game's own shader files hold
about 18,800 shader stages, and translating one the first time a draw needs it takes about 35 ms, with the draw waiting.
On a first visit to an area that adds up to seconds. These two steps translate them all up front, from your own game
files:

```
.venv\Scripts\python tools\dkb_shader_census.py --version dkb100 --out build\dkb100
build\dkb100\dkb.exe --prepare-shaders
```

The first reads the shader folders of the extracted RomFS (4 seconds) and writes `seed-nvn2.bin` (about 187 MiB) next to
`dkb.exe`; the second lowers and translates every stage into the shader cache. Each stage is translated with the default
answers (every texture a 2D texture, float vertex inputs and colour outputs), and each vertex stage again for the integer
vertex layouts met so far (Locations 6, 6 and 7, 3 and 4) plus any layout your own `shader-answers.bin` recorded. That
takes about 150 seconds on 20 threads and about 2.9 GB of cache. The pack is deleted afterwards by the installer.

In a cold run of the first area this cut the shaders translated during play from 714 to 62, the worst translation stall
from 6.6 s to 0.34 s, and the 1% low of the last window from 24 to 39 fps. What is left is about 1.4% of the stages the
game loads: 38 embedded in `main` and about 50 whose code is in no file the census reads. They translate on first use, and
play records them. `DKB_ANSWER_STATS=1` logs each translation made in play with what made it necessary.

Useful settings, as environment variables (`set NAME=value` in the same Command Prompt):

| Setting | Effect |
| --- | --- |
| `TOTK_PROFILE_DIR=<folder>` | Use this folder for the save and settings. A new, empty folder gives a fresh start, which is what reports should use. |
| `TOTK_GPU_NAME=<text>` | Use the first GPU whose name contains this text, on machines with more than one. |
| `TOTK_GPU=null` | Run without a picture (no Vulkan). Useful to tell CPU and OS bugs from graphics bugs. |
| `TOTK_XLATE_PROCS=<n>` | Shader translator processes at run time (default 2). Lower it if RAM runs out. |
| `TOTK_TRACE=perf` | Log the frame time, what holds frames back and the GPU time every 60 frames. For stalls and low frame rates. |
| `TOTK_PERF_SLOW_GPU_MS=<ms>` | With `TOTK_TRACE=perf`, a replay part that runs this long on the GPU (default 100) gets a line with its draw and dispatch counts and the NVN 2 names of its stages, a second line with the arguments of its indirect draws as read back afterwards, and a third saying whether a shader in that part wrote the largest one's arguments, and which. |
| `TOTK_FIX_INDIRECT_RESET=0` | Draws the terrain's indirect-record reset program as a shader instead of the host doing its stores (#48). The shader's stores are lost, so the records keep zero or stale contents; for comparisons only. |
| `TOTK_EXPERIMENTAL_GEOMETRY=1` | Replays draws with an ordinary geometry stage through it (#52). Off by default; other geometry draws are rejected once in the log. |
| `TOTK_NATIVE_GS=1` (or `DKB_NATIVE_GS=1`) | Replays the game's native passthrough geometry stages on the portable path, which draws the destructible terrain (#45). Off by default. The varyings are inferred, not read from the hardware skip mask. |
| `DKB_GEOMETRY_CONTEXT=1` | Logs the first 12 draws with a geometry stage and their buffers (#58). `DKB_GEOMETRY_CONTEXT_ORIGIN=<hex>` keeps only draws with that stage origin. |
| `TOTK_TRACE=codec` | Log each call of the video decoder that moves a buffer or a picture. For guide movies that stop or show a flat colour. |

The settings keep TotK Recomp's `TOTK_` names, so its fixes apply without a rename. Each one can also be written with
`DKB_` in its place (`DKB_GPU=null`); when both are set, the `DKB_` one wins.

Command-line options: `--seconds S` stops the run after S seconds of game time, and `--shot frame.png` with it saves
that frame. A shot is for your own debugging; do not post it if it shows game content.

## What to report

Open an issue with the issue form, or comment on [#3](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues/3)
(GPU testing) or [#7](https://github.com/Lucas-blip-png/dkb-pc-recomp/issues/7) (playing further):

- the commit you built (`git rev-parse --short HEAD`);
- GPU, driver version and Windows version;
- how far the game got and what you did just before it stopped;
- the log, `logs\totk.log` next to `dkb.exe`, as an attachment. After a crash, copy it before you start the game
  again: the next run renames it to `totk.1.log`.

Never attach game files, keys, builds or the shader cache, and never paste key values into a log or an issue.
