# RIFE TensorRT Frame Interpolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the midpoint-only FRUC path with optional RIFE 4.6 TensorRT interpolation supporting arbitrary timesteps, >2x output, SVP-like controls, scene-cut handling, and graceful fallback.

**Architecture:** The renderer stays free of compile-time CUDA/TensorRT dependencies. It loads an optional `MPCVRRifeRuntime64.dll` through a small C ABI. MPCVR owns timing, scene policy, source-pair lifetime, worker scheduling, and presentation; the runtime bridge owns CUDA/D3D11 interop, RIFE 4.6 tensor packing, TensorRT engine build/cache, and inference. A pure timestamp scheduler is independently testable.

**Tech Stack:** C++20, DirectShow/reference clock, D3D11, NVIDIA Optical Flow D3D11 driver API, CUDA D3D11 interop, TensorRT 11.x, RIFE 4.6 ONNX, Win32 property pages, PowerShell installer.

**Spec:** `docs/superpowers/specs/2026-09-12-rife-tensorrt-frame-interpolation-design.md`

## Global Constraints

- RIFE is the only production synthesis backend; NvOFFRUC/native-NVOF dense warp do not synthesize production frames.
- Steady-state inference is GPU-resident; no normal playback frame round-trips through system RAM.
- Inference contract is arbitrary timestep: `Frame(A, B, t)`, `0 < t < 1`.
- Initial/default model is RIFE 4.6; default execution contexts is 2.
- Default scene detector is NVOF; Image comparison and Disabled remain selectable.
- Default scene handling is Repeat frame; default duplicate policy is Do not remove.
- Audio timing/rate is untouched.
- Missing runtime/model, unsupported GPU, allocation/build failure, or missed deadlines degrades to ordinary playback.
- Do not add disposable GitHub Actions workflows.

---

## File map

Renderer-side new files:

- `Source/RifeRuntimeApi.h` — stable renderer/runtime C ABI; contains no CUDA/TensorRT headers.
- `Source/RifeFrameInterpolation.h/.cpp` — runtime DLL loader, capability probe, context/output-surface ownership.
- `Source/FrameInterpolationScheduler.h/.cpp` — pure timestamp/rate arithmetic.
- `Source/FrameSceneChangeDetector.h/.cpp` — NVOF and GPU image-comparison scene classification.
- `Source/FrameInterpolationPipeline.h/.cpp` — source-pair queue, duplicate policy, scene policy, inference workers, deadline/drop policy.

Optional runtime/tool files:

- `tools/RifeTensorRTRuntime/CMakeLists.txt`
- `tools/RifeTensorRTRuntime/RifeTensorRTRuntime.cpp`
- `tools/RifeTensorRTRuntime/README.md`
- `tools/RifeRuntimeInstaller/Install-MPCVR-RIFE.ps1/.cmd`
- `tools/RifeRuntimeInstaller/README.md`

Tests:

- `tools/rife-scheduler-test/RifeSchedulerTest.cpp`
- `tools/rife-runtime-abi-test/RifeRuntimeAbiTest.cpp`

Existing integration files:

- `Source/IVideoRenderer.h`
- `Source/PropPage.cpp/.h`
- `Source/resource.h`
- `Source/MpcVideoRenderer.rc`
- `Source/VideoRenderer.cpp/.h`
- `Source/DX11VideoProcessor.cpp/.h`
- `Source/MpcVideoRenderer.vcxproj/.filters`
- `.github/workflows/main.yml`
- `docs/NvidiaFrameInterpolation.md`

---

### Task 1: RIFE settings and SVP-like property page

**Files:** Modify `Source/IVideoRenderer.h`, `Source/PropPage.cpp`, `Source/resource.h`, `Source/MpcVideoRenderer.rc`, `Source/VideoRenderer.cpp`.

**Produces:**

```cpp
enum : int {
    RIFE_MODE_Disabled = 0,
    RIFE_MODE_ToScreen,
    RIFE_MODE_Movie2x,
    RIFE_MODE_Movie2_5x,
    RIFE_MODE_Movie3x,
    RIFE_MODE_Movie4x,
    RIFE_MODE_Movie5x,
    RIFE_MODE_Fixed60,
    RIFE_MODE_Fixed72,
    RIFE_MODE_Fixed90,
    RIFE_MODE_Fixed120,
    RIFE_MODE_Custom,
};
enum : int { RIFE_SCENE_NVOF = 0, RIFE_SCENE_Image, RIFE_SCENE_Disabled };
enum : int { RIFE_SCENE_PROCESS_Blend = 0, RIFE_SCENE_PROCESS_Repeat };
enum : int { RIFE_DUPLICATES_Keep = 0, RIFE_DUPLICATES_RemoveEveryOther };
```

`Settings_t` adds `iRifeMode`, `iRifeCustomFps`, `iRifeGpuThreads`, `iRifeGPU`, `bRifePerformanceBoost`, `iRifeSceneDetection`, `iRifeSceneProcessing`, `iRifeDuplicateRemoval`.

- [ ] Add enums/fields/defaults: Disabled, custom 60, threads 2, GPU Auto, boost off, NVOF, Repeat, Keep.
- [ ] Persist new registry keys. If only legacy FRUC mode is enabled, migrate to `Movie x2`; otherwise default disabled.
- [ ] Replace FRUC dialog choices with approved RIFE choices: Disabled/To screen/Movie x2/x2.5/x3/x4/x5/60/72/90/120/Custom; TensorRT; threads 1/2/3; model 4.6; GPU; Performance boost; scene detector; scene processing; duplicate policy.
- [ ] Ensure Defaults restores approved values and dependent controls disable when mode is Disabled.
- [ ] Run `.\build_mpcvr.cmd NoWait`; expect successful renderer build without CUDA/TensorRT installed.
- [ ] Commit `feat: add RIFE interpolation settings`.

### Task 2: Timestamp scheduler with deterministic tests

**Files:** Create `Source/FrameInterpolationScheduler.h/.cpp`, `tools/rife-scheduler-test/RifeSchedulerTest.cpp`; add source files to the project.

**Produces:**

```cpp
struct FrameInterpolationRate {
    int mode = RIFE_MODE_Disabled;
    uint32_t customFpsNum = 60;
    uint32_t customFpsDen = 1;
};
struct FrameInterpolationTarget {
    REFERENCE_TIME presentationTime = 0;
    double timestep = 0.0;
    bool exactSource = false;
};
class CFrameInterpolationScheduler {
public:
    void Reset();
    void Configure(const FrameInterpolationRate&, uint32_t displayNum, uint32_t displayDen);
    std::vector<FrameInterpolationTarget> Schedule(REFERENCE_TIME firstTime,
        REFERENCE_TIME secondTime, uint32_t sourceNum, uint32_t sourceDen);
};
```

- [ ] Write tests first for 24->120 (`.2/.4/.6/.8`), 24000/1001 x5 (=120000/1001 target), 24->60 noninteger phase cadence, 24 x2.5, display 60000/1001, exact endpoint exclusion, and `Reset()` after discontinuity.
- [ ] Build the test and verify failures before implementation.
- [ ] Implement rational target-period accumulation using integer/reference-time arithmetic with carried phase; do not repeatedly add rounded double durations.
- [ ] Run all scheduler tests; expect zero failures.
- [ ] Build MPCVR.
- [ ] Commit `feat: add arbitrary-rate interpolation scheduler`.

### Task 3: Stable optional runtime ABI and loader

**Files:** Create `Source/RifeRuntimeApi.h`, `Source/RifeFrameInterpolation.h/.cpp`, `tools/rife-runtime-abi-test/RifeRuntimeAbiTest.cpp`; update project files.

**ABI:**

```cpp
#define MPCVR_RIFE_RUNTIME_ABI 1u
struct MpcvrRifeCreateParams { uint32_t size, abiVersion; ID3D11Device* device; uint32_t width, height, gpuIndex, contextCount, performanceBoost; const wchar_t* modelPath; const wchar_t* cachePath; };
struct MpcvrRifeRequest { uint32_t size, contextIndex; ID3D11Texture2D* first; ID3D11Texture2D* second; ID3D11Texture2D* output; float timestep; };
struct MpcvrRifeStats { uint32_t size; double inferenceMs; uint64_t engineBytes; };
extern "C" __declspec(dllexport) uint32_t WINAPI MpcvrRifeGetAbiVersion();
extern "C" __declspec(dllexport) int WINAPI MpcvrRifeCreate(const MpcvrRifeCreateParams*, void** handle);
extern "C" __declspec(dllexport) int WINAPI MpcvrRifeInterpolate(void* handle, const MpcvrRifeRequest*, MpcvrRifeStats*);
extern "C" __declspec(dllexport) void WINAPI MpcvrRifeDestroy(void* handle);
```

- [ ] Write ABI smoke test first: missing DLL reports unavailable without crashing; fake ABI mismatch is rejected; correct fake ABI loads.
- [ ] Implement loader search order: renderer directory `RIFE\\runtime`, `%LOCALAPPDATA%\\MPCVideoRenderer\\RIFE\\runtime`, then explicit environment override `MPCVR_RIFE_RUNTIME_DIR`.
- [ ] Validate all required exports and exact ABI version before create.
- [ ] Implement output texture pool and one runtime handle per configured pipeline; do not include TensorRT/CUDA headers in renderer project.
- [ ] Run ABI tests and normal renderer build.
- [ ] Commit `feat: add optional RIFE runtime bridge`.

### Task 4: TensorRT 11 RIFE 4.6 runtime

**Files:** Create `tools/RifeTensorRTRuntime/CMakeLists.txt`, `RifeTensorRTRuntime.cpp`, `README.md`.

**Model contract:** use the standard vs-mlrt RIFE 4.6 v1 ONNX single input `[1,11,H,W]`: A RGB (3), B RGB (3), timestep plane (1), normalized X (1), normalized Y (1), `2/(W-1)` plane (1), `2/(H-1)` plane (1); RGB values are normalized float 0..1. Output is RGB `[1,3,H,W]`. Pad W/H to multiples of 32 and crop output back to source dimensions.

- [ ] Add CMake discovery for `TENSORRT_ROOT`, `CUDA_PATH`, `nvinfer`, `nvonnxparser`, `cudart`; x64 only.
- [ ] Implement TensorRT logger, ONNX parse/build, dynamic profile min `1x11x128x128`, opt around 1080p padded dimensions, max at least padded 2160p when builder/GPU permits.
- [ ] Build with FP16 flag when hardware supports it; keep TensorRT free to retain FP32 layers.
- [ ] Key serialized plan cache by SHA-256 model hash, TensorRT version, GPU name/compute capability, dimensions/profile, and boost flag.
- [ ] Implement D3D11/CUDA interop with `cudaGraphicsD3D11RegisterResource`, map source/output resources, CUDA preprocessing/postprocessing kernels, and no CPU pixel copy.
- [ ] Allocate one TensorRT execution context + CUDA stream per requested GPU thread.
- [ ] For Performance boost, build a static exact padded H/W profile; otherwise use reusable dynamic profile.
- [ ] Verify with `trtexec`/runtime smoke test on a TensorRT-equipped machine: engine builds, deserializes, and produces output for 1080p pair at `t=.5`.
- [ ] Commit `feat: add TensorRT RIFE 4.6 runtime`.

### Task 5: Scene-change detector

**Files:** Create `Source/FrameSceneChangeDetector.h/.cpp`; reuse only the NVOF driver API/session knowledge from `research/svp4-interpolation-archaeology`, not its synthesizer.

**Produces:**

```cpp
enum class FrameSceneResult { SameShot, Cut, Unavailable };
class CFrameSceneChangeDetector {
public:
    bool Initialize(ID3D11Device*, UINT width, UINT height, int mode);
    FrameSceneResult Classify(ID3D11Texture2D* first, ID3D11Texture2D* second);
    void Reset();
};
```

- [ ] Port minimal driver-only D3D11 NVOF loader/session/resource registration with 4x4 bidirectional flow, temporal hints off, cost output off.
- [ ] Use PerfFast initially and aggregate bidirectional flow disagreement plus coarse image statistics; keep classification thresholds internal.
- [ ] Implement Image comparison with existing validated 32x18 coarse sample, spatial correlation threshold `0.15`, normalized MAD threshold `0.055`.
- [ ] If NVOF init/execute fails, automatically use Image comparison for the session; if that fails, return Unavailable and effectively disable cut detection.
- [ ] Add debug/status text exposing selected detector and fallback state.
- [ ] Build MPCVR.
- [ ] Commit `feat: add RIFE scene change detection`.

### Task 6: Asynchronous interpolation pipeline

**Files:** Create `Source/FrameInterpolationPipeline.h/.cpp`; modify `Source/DX11VideoProcessor.h/.cpp`.

**Produces:** timestamped outputs tagged with generation and kind (`Source`, `Rife`, `SceneRepeat`, `SceneBlend`).

- [ ] Write pure queue-policy tests where possible: duplicate removal phase, generation invalidation, ordering, synthetic deadline drop.
- [ ] Implement source-pair ownership with COM-safe D3D11 texture references.
- [ ] Run scene classification once per pair before dispatching RIFE jobs.
- [ ] For normal pairs, schedule arbitrary `t` jobs across configured runtime contexts; queue completed outputs by presentation timestamp.
- [ ] For Repeat cut handling, select nearest endpoint for each scheduled timestamp; for Blend, issue GPU linear blend without RIFE.
- [ ] Implement literal Remove every other frame before pair construction and reset phase on discontinuity.
- [ ] Enforce bounded queue/backpressure and skip synthetic jobs whose presentation deadline cannot be met; never delay audio/graph clock.
- [ ] Build MPCVR.
- [ ] Commit `feat: add asynchronous RIFE interpolation pipeline`.

### Task 7: Generalize presenter from midpoint to arbitrary output queue

**Files:** Modify `Source/VideoRenderer.h/.cpp`, `Source/DX11VideoProcessor.h/.cpp`.

- [ ] Replace singular midpoint state with `InterpolatedPresentationFrame` queue records containing texture, presentationTime, generation, kind, timestep.
- [ ] Keep existing reference-clock presenter thread but make it consume an arbitrary number of ordered outputs per source interval.
- [ ] On seek/NewSegment/flush/media-type change increment generation, clear source pairs/jobs/presentation queue, reset duplicate phase and scene detector; stale worker completions are discarded by generation.
- [ ] Ensure pause stops timed presentation without destroying a valid TensorRT engine; resume continues only current generation.
- [ ] Add stats: target fps, generated synthetic fps, synthetic drops, average inference ms, queue depth, runtime/model status.
- [ ] Build and run existing CI.
- [ ] Commit `feat: present arbitrary RIFE output cadence`.

### Task 8: Remove production NvOFFRUC assumptions and wire RIFE lifecycle

**Files:** Modify `Source/DX11VideoProcessor.cpp/.h`, `Source/VideoRenderer.cpp`, project files; stop compiling/using `Source/NvidiaFrameInterpolation.cpp/.h` for production path if no longer referenced.

- [ ] Remove midpoint-only FRUC initialization/submission/acquire logic.
- [ ] Initialize RIFE lazily only when enabled and source dimensions/rate pass basic validation.
- [ ] Preserve ordinary rendering path exactly when RIFE is disabled/unavailable.
- [ ] Ensure resize/device reset safely destroys/recreates pipeline/runtime resources.
- [ ] Keep old research branch code untouched; only master-derived production sources change.
- [ ] Build MPCVR and inspect linker for no required TensorRT/CUDA import dependency.
- [ ] Commit `refactor: replace production FRUC path with RIFE pipeline`.

### Task 9: Runtime installer and model verification

**Files:** Create `tools/RifeRuntimeInstaller/Install-MPCVR-RIFE.ps1`, `.cmd`, `README.md`.

- [ ] Installer accepts a pinned runtime bundle URL/path and RIFE 4.6 ONNX URL/path plus expected SHA-256 values; refuse installation on checksum mismatch.
- [ ] Install to `%LOCALAPPDATA%\\MPCVideoRenderer\\RIFE\\runtime` and `...\\models`; create cache directory separately.
- [ ] Never silently download arbitrary latest versions; pinned hashes/versions are explicit inputs/constants.
- [ ] Add `-VerifyOnly` to probe files and report missing/mismatched components without modifying system state.
- [ ] Document `MPCVR_RIFE_RUNTIME_DIR` override and cache removal procedure.
- [ ] Commit `tools: add RIFE runtime installer`.

### Task 10: Durable CI, documentation, and final verification

**Files:** Modify `.github/workflows/main.yml`, `docs/NvidiaFrameInterpolation.md`; add `docs/RifeFrameInterpolation.md` if needed.

- [ ] Add scheduler and ABI tests to the existing Windows job before `build_mpcvr.cmd`; do not create a temporary workflow.
- [ ] Document optional runtime behavior, defaults, supported target modes, scene settings, cache/runtime locations, and fallback semantics.
- [ ] Push feature branch and wait for GitHub Actions build/test result.
- [ ] If CI fails, inspect exact failing job/log, fix root cause, and rerun via another commit or existing `workflow_dispatch`.
- [ ] Verify final branch contains no bundled NVIDIA DLLs, ONNX binaries, generated TensorRT engines, or temporary workflows.
- [ ] Verify renderer package still works without RIFE runtime installed.
- [ ] Verify on NVIDIA/TensorRT machine: 23.976->59.94/119.88, 24->60/72/120, 30->60/120, seeking, pause/resume, hard cuts, runtime missing, and overload/drop behavior.
- [ ] Commit `docs: document RIFE frame interpolation`.
