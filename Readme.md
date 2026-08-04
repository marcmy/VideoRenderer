# MPC Video Renderer

MPC Video Renderer is a free and open-source video renderer for DirectShow. The renderer can potentially work with any DirectShow player, but full support is available only in the MPC-BE. Recommended MPC-BE 1.8.9.106 or newer.

## Custom NVIDIA Maxine build

This fork includes NVIDIA Maxine Video Super Resolution support and a one-click setup package for K-Lite Codec Pack users.

### Quick setup with K-Lite

1. Install [K-Lite Codec Pack](https://codecguide.com/download_kl.htm) with MPC-HC and MPC Video Renderer.
2. Download **`MPCVR-Maxine-Setup.zip`** from the [latest release](https://github.com/marcmy/VideoRenderer/releases/latest).
3. Extract the ZIP completely.
4. Close MPC-HC.
5. Run **`Install-MPCVR-Maxine.cmd`** and approve the administrator prompt.
6. Reopen MPC-HC and enable NVIDIA Maxine in the MPC Video Renderer settings.
7. While a video is playing, press **Ctrl+J** to confirm that Maxine is active and that the runtime path is shown.

The setup package installs the custom renderer, the required five-file Maxine runtime, and the `NV_VIDEO_EFFECTS_PATH` user variable. It also creates a **Restore MPC-VR Maxine** desktop shortcut.

K-Lite updates may replace the custom renderer. When that happens, close MPC-HC and run the **Restore MPC-VR Maxine** shortcut to download and reinstall the latest custom build.

### Manual or portable installation

Advanced users can download **`MpcVideoRenderer-Maxine.zip`** from the same release page and install the renderer files manually. The one-click setup ZIP is recommended for normal K-Lite installations.

`SHA256SUMS.txt` contains the SHA-256 hashes for both public ZIP files.

## Key features

* Can work with DXVA2 and Direct3D 11 hardware decoder.
* DVXA2 and Direct3D11 Video Processor with hardware de-interlacing for NV12, YUY2, P010 formats.
* Shader video processor for various YUV, RGB and grayscale formats.
* Various frame resizing algorithms, including Super Resolution.
* Subtitle and OSD display.
* Rotation and flip of the video frame.
* Dithering when the final color depth is reduced from 10/16 bits to 8 bits.
* HDR video support (HDR10, HLG and partially Dolby Vision).
* Automatic HDR to SDR conversion.
* Transferring HDR10 data to the display.

## Minimum system requirements

* An SSE2-capable CPU
* Windows 7¹ or newer
* DirectX 9.0c (PS 3.0) video card

¹For Windows 7, you must have D3DCompiler_47.dll file. It can be installed via update KB4019990.

## Recommended system requirements

* An SSE2-capable CPU
* Windows 10 or newer
* DirectX 10/11 video card

## License

MPC Video Renderer's code is licensed under [GPL v3].

## Download

### This fork

[Latest NVIDIA Maxine release](https://github.com/marcmy/VideoRenderer/releases/latest)

### Upstream project

[Official releases](https://github.com/Aleksoid1978/VideoRenderer/releases)

[Nightly builds](https://github.com/Aleksoid1978/VideoRenderer/wiki/Nightly-builds)

## Links

[Topic in MPC-BE forum (Russian)](https://mpc-be.org/forum/index.php?topic=381)

[MPC-BE](https://github.com/Aleksoid1978/MPC-BE)

## Donate

<https://mpc-be.org/forum/index.php?topic=240>
