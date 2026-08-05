# Native NVIDIA Optical Flow frame interpolation

## Goal

Replace the optional proprietary `NvOFFRUC.dll` runtime with an MPC Video Renderer-owned interpolation backend built on NVIDIA's driver-provided Optical Flow API.

The resulting public runtime path should require only:

- an NVIDIA Turing-or-newer GPU with NVOFA support
- a compatible NVIDIA display driver
- MPC Video Renderer itself

It must not require the Optical Flow SDK ZIP, an NVIDIA Developer Program profile, `NvOFFRUC.dll`, or `cudart64_110.dll` on the user's machine.

## Keep the working backend during development

The current dynamically loaded NvOFFRUC backend remains intact as a quality and performance reference until the native backend is proven. Native work is isolated on `feature/native-nvof-interpolation`.

## Milestones

### 1. Driver API probe

- load `nvofapi.dll`/`nvofapi64.dll` strictly from the Windows system directory
- resolve `NvOFGetMaxSupportedApiVersion`
- resolve `NvOFAPICreateInstanceD3D11`
- report the supported API version without requiring an SDK runtime

### 2. Direct3D 11 optical-flow session

- create the NVOF D3D11 API function table
- create an optical-flow session on MPCVR's existing D3D11 device/context
- query supported output grid sizes and input formats
- prefer forward and backward flow in one execute operation when supported
- keep the feature optional and fail back to ordinary playback

### 3. Flow-vector debug output

- register two processed source textures
- allocate forward/backward flow buffers
- execute NVOFA for consecutive frames
- visualize vector direction and magnitude in a debug shader
- verify reset, seek, file-switch, device-loss, and shutdown behavior

### 4. Baseline midpoint synthesis

- upsample the hardware flow field
- backward-warp both source frames toward `t = 0.5`
- blend valid samples
- detect out-of-bounds samples and scene cuts
- fall back to the nearest real frame when interpolation is unsafe

### 5. Quality pipeline

- forward/backward consistency validation
- motion-edge-aware vector upsampling
- occlusion/disocclusion masks
- invalid-vector infilling
- image-space hole repair
- duplicate-frame and hard-cut rejection
- subtitle/overlay protection where practical

### 6. Product integration

- backend selector: Native NVOF / NvOFFRUC reference / Off
- native backend becomes the default only after visual and timing validation
- remove SDK acquisition from the normal installer after the native backend is ready

## Official API references

- NVOFA Programming Guide: https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html
- Public NVIDIA Optical Flow API common header: https://github.com/NVIDIA/NVIDIAOpticalFlowSDK/blob/master/nvOpticalFlowCommon.h

The public NVIDIA header carries a permissive redistribution notice. No proprietary SDK DLL is added to this repository.
