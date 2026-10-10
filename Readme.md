# MPC Video Renderer

MPC Video Renderer is a free and open-source DirectShow video renderer for Windows. It supports modern hardware decoding paths, high-quality scaling, HDR playback, subtitle and OSD composition, and several NVIDIA-specific video-processing features.

This fork adds a dedicated **NVIDIA Maxine** control panel, a tested five-DLL Maxine Video Super Resolution runtime, output oversampling, and a one-click setup package for **MPC-HC installed through K-Lite Codec Pack**.

## Recommended download

Go to the [latest release](https://github.com/marcmy/VideoRenderer/releases/latest) and download:

- **`MPCVR-Maxine-RIFE-Setup.zip`** for the normal one-click K-Lite installation
- **`MpcVideoRenderer-Maxine.zip`** only for manual or portable installation
- **`SHA256SUMS.txt`** to verify the two public ZIP files

For most people, `MPCVR-Maxine-RIFE-Setup.zip` is the only ZIP they need.

## Requirements

### General MPC Video Renderer

- Windows 10 or Windows 11 is recommended
- A DirectX 10/11-capable GPU
- MPC-HC, MPC-BE, or another compatible DirectShow player

### NVIDIA Maxine processing

- 64-bit MPC-HC is recommended
- A compatible NVIDIA GPU and current NVIDIA display driver
- The one-click setup package from this repository

Maxine processing is implemented in the 64-bit renderer. The setup still updates both K-Lite renderer files so the normal K-Lite installation stays consistent.

## One-click setup with K-Lite

1. Install [K-Lite Codec Pack](https://codecguide.com/download_kl.htm) with **MPC-HC** and **MPC Video Renderer** selected.
2. Download **`MPCVR-Maxine-RIFE-Setup.zip`** from the [latest release](https://github.com/marcmy/VideoRenderer/releases/latest).
3. Extract the ZIP completely. Do not run the installer from inside the compressed archive.
4. Close every open MPC-HC window.
5. Run **`Install-MPCVR-Maxine.cmd`**.
6. Approve the administrator prompt when Windows asks for permission to replace the K-Lite renderer files.
7. Reopen MPC-HC.

The setup package:

- verifies the embedded renderer and Maxine runtime before installing them
- installs the custom 32-bit and 64-bit MPC Video Renderer files into K-Lite
- installs the tested Maxine runtime under `%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs`
- creates the current-user `NV_VIDEO_EFFECTS_PATH` environment variable
- creates a **Restore MPC-VR Maxine** shortcut on the desktop

No additional runtime download is required after the setup ZIP has been downloaded.

## Select MPC Video Renderer in MPC-HC

1. Open MPC-HC.
2. Go to **View > Options**.
3. Open **Playback > Output**.
4. Select **MPC Video Renderer** as the video renderer.
5. Click **Apply**, then **OK**.
6. Close and reopen the video if the renderer does not change immediately.

For best performance on Windows 10/11, use **D3D11** hardware decoding in LAV Video Decoder when MPC Video Renderer is using Direct3D 11.

## Open the MPC Video Renderer settings

Start playing a video, then use either route:

- **View > Renderer Settings**
- right-click the video, then open **Filters > MPC Video Renderer**

The main property page contains the normal MPC Video Renderer controls. Click **NVIDIA Maxine settings...** in the **DXVA2 and D3D11 video processor** section to open the dedicated Maxine panel.

## Recommended starting configuration

These settings are a safe starting point for normal SDR video:

| Setting | Recommended value |
|---|---|
| Operation | Upscale, with optional cleanup passes |
| Source type | Automatic from reported source bitrate |
| Quality | High |
| Output size | Match player output |
| Oversampling | Off |
| Max source | 1080p or lower |
| Denoise | Off |
| Deblur | Off |
| Pipeline | Upscale -> Denoise -> Deblur |
| CUDA GPU | Auto |
| Auto high-bitrate threshold | 20 Mbps |

After applying the settings, play a video and press **Ctrl+J**. The renderer statistics should show the Maxine operation, selected mode, processing size, and loaded runtime path.

## NVIDIA Maxine settings explained

### Operation

Controls the main processing mode.

- **Disabled**: turns Maxine processing off.
- **Upscale, with optional cleanup passes**: enlarges the frame and can additionally apply denoise and deblur.
- **Denoise only, keep source resolution**: removes compression noise without changing resolution. A denoise strength must be selected.
- **Deblur only, keep source resolution**: attempts to reduce blur without changing resolution. A deblur strength must be selected.

### Source type

Selects which Maxine Video Super Resolution model is used for upscaling.

- **Automatic from reported source bitrate**: uses Standard below the configured bitrate threshold and High Bitrate at or above it. If the media type does not report a bitrate, Standard is used.
- **Standard, compressed video**: intended for typical streamed, downloaded, or visibly compressed material.
- **High bitrate / clean source**: intended for cleaner sources with fewer compression artifacts.
- **Bicubic baseline, no AI enhancement**: uses the baseline scaler without AI enhancement. This is useful for comparison and troubleshooting.

### Quality

Selects the Maxine quality level: **Low**, **Medium**, **High**, or **Ultra**.

Higher levels generally use more GPU resources and may produce stronger processing. **High** is the recommended starting point. Ultra is best treated as an optional quality/performance tradeoff rather than an automatic improvement for every source.

### Output size

Controls the resolution produced by the Maxine upscale pass.

- **Match player output**: targets the current video output size inside the player window or fullscreen display.
- **1.33x, 1.5x, 2x, 3x, 4x**: scale directly from the source resolution by the selected factor.

`Match player output` is usually the most practical setting because it avoids generating a much larger frame than the player currently needs.

### Oversampling

Oversampling is available only when **Output size** is set to **Match player output**.

- **Off**: Maxine targets the player output size directly.
- **1.33x, 1.5x, 2x**: Maxine renders above the player output size, after which MPC Video Renderer downsamples the result to the actual display size.

Oversampling can improve fine detail and reduce aliasing, but it increases GPU and VRAM usage substantially. It is also limited to a maximum of 4x the source dimensions. A Ctrl+J message such as **`2x (clamped to 4x source limit)`** means the requested oversampled size was reduced to that maximum. It is informational, not an error.

### Max source

Limits which source resolutions are eligible for Maxine processing.

- **Disabled**: disables Maxine processing through the source-limit control.
- **SD or lower**
- **720p or lower**
- **1080p or lower**
- **1440p or lower**

A source above the selected limit bypasses Maxine. This prevents expensive processing on material that is already high resolution.

### Denoise

Adds a noise and compression-artifact cleanup pass.

- **Off** disables the pass.
- **Low, Medium, High, Ultra** increase its strength.

When **Operation** is set to Denoise only, this setting is required. When upscaling, it is optional. Strong denoise can remove real texture along with noise, so begin with Low or Medium when a source actually needs cleanup.

### Deblur

Adds a blur-reduction pass.

- **Off** disables the pass.
- **Low, Medium, High, Ultra** increase its strength.

When **Operation** is set to Deblur only, this setting is required. Strong deblur may exaggerate edges or halos, so conservative settings are usually better.

### Pipeline

Controls the order in which upscale, denoise, and deblur are applied when more than one pass is enabled.

Available orders are:

- Upscale -> Denoise -> Deblur
- Upscale -> Deblur -> Denoise
- Denoise -> Deblur -> Upscale
- Deblur -> Denoise -> Upscale
- Denoise -> Upscale -> Deblur
- Deblur -> Upscale -> Denoise

Order can change the look of the result. Cleaning before upscaling may prevent artifacts from being enlarged, while cleaning after upscaling may give the filters more pixels to analyze. Leave the default order unless a particular source looks better with another sequence.

### CUDA GPU

Selects the NVIDIA GPU used for Maxine processing.

- **Auto** is recommended for normal single-GPU systems.
- **GPU 0** through **GPU 7** allow manual selection on multi-GPU systems.

The CUDA device number may not always match the number shown in Windows Task Manager or the physical display connection.

### Auto high-bitrate threshold

Used only when **Source type** is Automatic.

- valid range: **1 to 1000 Mbps**
- default: **20 Mbps**

Sources reporting a bitrate at or above the threshold use the High Bitrate model. Lower-bitrate sources use Standard. Sources that report no bitrate also use Standard.

## Main MPC Video Renderer settings relevant to Maxine

### Use Direct3D 11

Recommended on Windows 10/11, especially when LAV Video Decoder is also using D3D11 hardware decoding. Maxine requires the Direct3D 11 processing path in this fork.

### Use for resizing

Lets the DXVA2/D3D11 video processor handle resizing. Maxine has its own output-size controls, but this setting still affects the renderer's normal fallback behavior when Maxine is not active.

### Request Super Resolution

This is the normal driver-level video-processor Super Resolution control. It is separate from NVIDIA Maxine. When Maxine is enabled, the custom Maxine path takes priority for eligible content.

### RTX Video HDR

Converts SDR video to HDR through NVIDIA's driver-level processing. It cannot be used at the same time as Maxine in this build. Disable RTX Video HDR when using Maxine.

### Show statistics

Displays the renderer debug overlay. MPC-HC also toggles this overlay with **Ctrl+J**. The overlay is the fastest way to confirm which processing path is actually active.

## RIFE processing size

RIFE has two independent, opt-in options. **Preserve 10-bit** uses RGBA16F working
and output surfaces when the source and video-conversion path retain more than
8 bits. Explicit 8-bit conversion stays 8-bit. Models still use their existing
mixed FP16/FP32 arithmetic; this option avoids the BGRA8 input/output bottleneck.
Normal rendering retains precision until its existing output conversion/dither.
Maxine's current BGRA8 input remains an 8-bit boundary and the BGRA CUDA handoff
is bypassed for this precision path. Larger surfaces can cost memory/bandwidth.

**Feature reuse** runs the image encoder once per immutable source pair and
execution context for the pinned 4.15 Lite and 4.25 exports. 4.4 and 4.6 lack
that encoder. 4.25 Lite keeps its original engine because the split produced a
reproducible border artifact in native GPU validation. Hash-qualified
encoder/synthesis sidecars are included in the
model package; they leave original models and old plans intact. The first use
builds separate fixed-resolution stage plans. A new pair, seek, drain or failed
request invalidates reuse. Missing/incompatible sidecars fall back to the
original engine; detailed Ctrl+J reports whether features are actually active.
This is experimental: compare delivered FPS and picture quality at identical
settings. Encoder reuse does not guarantee a throughput improvement.

Frame interpolation settings includes an optional processing-size control:

- **Source resolution** (default) keeps the original oriented picture size.
- **Match displayed size** reduces it to fit the displayed video rectangle, without upscaling.
- **Custom limit** caps the short edge, preserving aspect ratio and never upscaling. For example, 720 gives 1280x720 for a 1920x1080 source, or 720x1280 for a portrait 1080x1920 source.

Lower resolution trades picture detail for less inference work. All frames in the active RIFE path use this working size, including source endpoints. Normal resizing and optional Maxine enhancement then produce the displayed image. Maxine's extra work can offset the inference saving, so compare first with Maxine disabled. Source-size playback rules still match the original video.

Ctrl+J's existing **RIFE input** line shows original, working and model-padded dimensions. Match displayed size can reinitialize RIFE when the displayed rectangle changes; a custom limit stays independent of window size. Changed working dimensions invalidate queued frames and the learned capacity. Seeking and pause/resume otherwise retain their existing behavior, and Stop still clears it. Performance Boost may build a new engine for the new working dimensions; existing caches are retained.

**SVPflow1 motion vectors** is the first scene-detector choice and the default for new or reset settings. Existing saved detector selections remain selected.

## When Maxine will not activate

The Ctrl+J overlay normally explains why Maxine was bypassed. Common reasons include:

- Operation is Disabled
- Max source is Disabled
- the video exceeds the selected Max source limit
- the current input is HDR
- RTX Video HDR is enabled
- the Maxine runtime could not be loaded
- Match player output does not require enlargement and no cleanup pass is enabled
- the requested frame would exceed a Direct3D 11 texture-size limit

HDR input is not supported by the current Maxine path. Normal MPC Video Renderer HDR passthrough or HDR-to-SDR processing continues to work when Maxine is bypassed.

## Verify the installation

Play an SDR video and press **Ctrl+J**. Check for:

- an NVIDIA Maxine status line showing the active operation
- the selected Standard, High Bitrate, or Bicubic mode
- the source and Maxine processing resolutions
- the runtime path under `%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs`

The overlay is also the most useful information to capture when reporting a problem.

## K-Lite updates and the restore shortcut

K-Lite updates may replace the custom renderer with the normal bundled MPC Video Renderer.

When that happens:

1. Close MPC-HC.
2. Double-click **Restore MPC-VR Maxine** on the desktop.
3. Approve the administrator prompt.
4. Reopen MPC-HC.

The shortcut downloads the latest custom renderer, verifies its SHA-256 hash, and restores both K-Lite renderer files. The Maxine runtime normally remains installed and does not need to be reinstalled.

## Manual or portable installation

`MpcVideoRenderer-Maxine.zip` contains the renderer files only. It is intended for advanced users who already know where their player loads MPC Video Renderer from.

The manual ZIP does not perform the complete K-Lite setup and does not install the Maxine runtime or environment variable. Normal K-Lite users should use `MPCVR-Maxine-RIFE-Setup.zip` instead.

## Key renderer features

- Direct3D 11 and DXVA2 video-processing paths
- support for D3D11 and DXVA2 hardware decoders
- hardware deinterlacing for NV12, P010/P016, YUY2, and other supported formats
- shader-based chroma upsampling, upscaling, and downscaling
- NVIDIA Maxine upscaling, denoise, deblur, selectable pass ordering, multi-GPU selection, and output oversampling
- NVIDIA driver-level Super Resolution and RTX Video HDR support where available
- HDR10 and HLG playback
- partial Dolby Vision processing
- HDR passthrough and HDR-to-SDR tone mapping
- subtitle and OSD rendering
- frame rotation and flipping
- dithering when reducing 10/16-bit output to 8-bit
- exclusive fullscreen, VBlank, and presentation-timing controls

## Troubleshooting

### The NVIDIA Maxine settings button is missing

Confirm that MPC-HC is actually loading this fork's renderer. Open **View > Renderer Settings**, then check the Information tab or Ctrl+J overlay. A normal upstream K-Lite renderer will not contain this fork's Maxine panel.

### Maxine runtime could not be loaded

Run `MPCVR-Maxine-RIFE-Setup.zip` again, close and reopen MPC-HC, and verify that Ctrl+J reports a runtime path under:

`%LOCALAPPDATA%\MPCVR Maxine Runtime\nvvfx\libs`

The required runtime files are:

- `NVCVImage.dll`
- `NVVideoEffects.dll`
- `nvngxruntime.dll`
- `nvngx_vsr.dll`
- `nvVFXVideoSuperRes.dll`

### K-Lite replaced the custom renderer

Use the **Restore MPC-VR Maxine** desktop shortcut.

### Playback becomes slow or frames are dropped

Reduce Quality, turn Oversampling off, lower the fixed Output size, disable Denoise or Deblur, or lower the Max source limit. Ultra quality plus 2x oversampling can be extremely expensive.

### The image looks overprocessed

Reduce Denoise or Deblur strength first. High and Ultra cleanup levels can remove natural texture or exaggerate edges depending on the source.

## Building from source

The repository includes `build_mpcvr.cmd` and GitHub Actions workflows for x86/x64 builds and release packaging. A normal local build requires the Visual Studio toolchain and the repository submodules.

Release automation produces an immutable Maxine release containing:

- `MPCVR-Maxine-RIFE-Setup.zip`
- `MpcVideoRenderer-Maxine.zip`
- `SHA256SUMS.txt`

A small legacy renderer checksum may remain temporarily so updater shortcuts installed by older releases continue to function.

## Upstream project

This repository is based on [Aleksoid1978/VideoRenderer](https://github.com/Aleksoid1978/VideoRenderer).

Upstream information and bug reports unrelated to this fork's Maxine changes should be checked against the upstream project first.

## License

MPC Video Renderer's code is licensed under the [GNU General Public License v3](https://www.gnu.org/licenses/gpl-3.0.html).

The optional SVPflow1 scene detector includes GPL-2.0-or-later motion-search
code from SVP/MVTools and x264 pixel kernels. Attribution, source provenance,
classifier differences and NASM build instructions are in
[external/SVPflow1/README.md](external/SVPflow1/README.md).
