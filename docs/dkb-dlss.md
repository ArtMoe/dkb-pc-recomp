# DLSS (experimental)

`upscaler = dlss` in `totk.ini` (or `DKB_UPSCALER=dlss`) replaces the game's own upscaler, the compute dispatch that
scales the rendered frame to the output resolution, with NVIDIA DLSS Super Resolution (`runtime/vk_dlss.c`). FSR still
scales the output to the window when the window is larger. The setting is read at start.

Needs:

- An NVIDIA RTX GPU on Windows, with a driver recent enough for DLSS. The driver installs the NGX core
  (`_nvngx.dll`); the runtime finds it through the driver's registry keys.
- `nvngx_dlss.dll`, the DLSS model, next to `dkb.exe`. DKB Recomp does not ship it. Take it from a game or from
  NVIDIA's DLSS SDK release (310.5 or later for preset M, which the runtime asks for).

What slice 1 does: DLSS runs on the game's colour input alone, without jitter, with zero motion vectors and a zero
depth image, and drops its history on every frame. That makes it a spatial upscaler, without DLSS's temporal
reconstruction. The quality mode follows the ratio of output to input (DLAA at 1:1). Each change of the dynamic
resolution recreates the DLSS feature, which can hitch.

When anything is missing or fails (no NVIDIA GPU, no NGX core, no `nvngx_dlss.dll`, NGX initialisation or feature
creation fails, an input larger than the output), `dkb.log` gets one line starting with `dlss:` that says why, and
the game's own upscaler runs for the rest of the run.
