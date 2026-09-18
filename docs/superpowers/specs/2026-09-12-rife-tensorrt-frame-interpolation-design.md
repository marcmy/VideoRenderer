# RIFE TensorRT Frame Interpolation Design

**Date:** 2026-09-12  
**Status:** Approved design direction  
**Repository:** `marcmy/VideoRenderer`

## 1. Goal

Replace the current production frame-interpolation direction based on NvOFFRUC/native NVOF synthesis with a RIFE-class neural frame-interpolation subsystem integrated directly into MPC Video Renderer.

The initial implementation targets NVIDIA GPUs through mainline TensorRT and CUDA, uses RIFE 4.6 as the first supported model, supports arbitrary interpolation timesteps rather than a single midpoint, and preserves the user-facing behavior of SVP 4 Pro's RIFE panel as closely as practical.

The result must support more than 2x interpolation, including 2.5x, 3x, 4x, 5x, fixed output rates, and output matched to the active display refresh.

## 2. Non-goals

The first implementation will not:

- reimplement RIFE in HLSL;
- retain NvOFFRUC as an interpolation backend;
- retain the experimental native NVOF dense-warp synthesizer as an interpolation backend;
- embed Python, PyTorch, VapourSynth, or SVP itself;
- reproduce SVP's proprietary motion-vector engine;
- expose TensorRT precision/tactic knobs to the user;
- implement adaptive duplicate detection beyond the explicit SVP-style `Remove every other frame` mode;
- require RIFE/TensorRT to be installed for MPCVR to load or play video normally.

The old NVOF/NvOFFRUC research documents and experimental branches remain historical reference material. Production code should stop depending on them for frame synthesis.

## 3. User-facing behavior

### 3.1 Interpolation controls

The frame-interpolation UI should remain close to SVP's RIFE configuration rather than exposing implementation details.

Target-rate controls:

- Disabled
- To screen
- Movie x2
- Movie x2.5
- Movie x3
- Movie x4
- Movie x5
- 60 fps
- 72 fps
- 90 fps
- 120 fps
- Custom fixed FPS

`To screen` targets the actual active-display refresh reported by the renderer, preserving rational/precise refresh information where available instead of rounding to an integer label.

Multiplier modes derive their target rate from actual source timestamps/source rate. For example, 24000/1001 x5 is 120000/1001, not 120.000 fps.

### 3.2 RIFE options

Initial controls:

- Neural network engine: `NVIDIA TensorRT`
- GPU threads: `1`, `2`, `3`; default `2`
- AI model: `4.6`; initial release supports only RIFE 4.6
- GPU device: `Auto` plus enumerated supported NVIDIA adapters
- Performance boost: `Disabled` / `Enabled`; default `Disabled`

`GPU threads` maps internally to TensorRT execution contexts with independent CUDA streams, not to arbitrary CPU worker threads.

Tensor precision is not user-facing. The backend uses FP16-capable TensorRT execution where appropriate while allowing TensorRT to retain higher precision for operations that require it.

### 3.3 Scene-change controls

Scene-change detection options:

- NVOF motion vectors; default
- Image comparison
- Disabled

Scene-change processing options:

- Blend adjacent frames
- Repeat frame; default

Duplicate-frame removal options:

- Do not remove; default
- Remove every other frame

SVP motion vectors are intentionally omitted because MPCVR does not contain SVP's proprietary motion-analysis subsystem. NVOF is the closest native NVIDIA-side analogue and can operate through the display driver's D3D11 Optical Flow API.

## 4. High-level architecture

The production path is split into five independent responsibilities:

1. `CRifeTensorRTBackend` — TensorRT/CUDA inference and D3D11 interop.
2. `CFrameInterpolationScheduler` — converts source timestamps plus target-rate mode into exact output timestamps and normalized RIFE timesteps.
3. `CFrameSceneChangeDetector` — NVOF or image-comparison scene-cut classification.
4. `CFrameInterpolationPipeline` — owns source-frame pairs, duplicate policy, scene-change policy, inference dispatch, deadline handling, and output ordering.
5. Existing MPCVR timed presenter — generalized to present source and synthetic frames from a timestamped queue.

The renderer must not know TensorRT details. The TensorRT backend must not decide presentation timing. The scene detector must not synthesize frames. These boundaries are deliberate so each unit can be tested and replaced independently.

## 5. GPU and resource pipeline

### 5.1 Placement in the renderer pipeline

RIFE operates on the fully processed video image before subtitles, OSD, and final presentation composition.

Conceptual order:

```text
decode
  -> YUV/RGB conversion and color correction
  -> HDR processing as currently required
  -> optional Maxine VSR / denoise / deblur
  -> RIFE interpolation
  -> final resize / post-scale shaders
  -> subtitles and OSD
  -> swap chain
```

This prevents subtitle/OSD glyphs from becoming neural-interpolation input and ensures source and synthetic frames use the same final MPCVR presentation path.

The current `m_TexFrameInterpolationInput` concept is retained as the staging point for the fully processed video frame, but it is generalized away from the old single-midpoint NvOFFRUC contract.

### 5.2 Zero-CPU-copy requirement

Normal inference must remain GPU-resident:

```text
D3D11 source textures
  -> CUDA/D3D11 interop
  -> CUDA preprocessing
  -> TensorRT device tensors
  -> TensorRT RIFE inference
  -> CUDA postprocessing
  -> D3D11 output texture
  -> MPCVR presentation queue
```

No normal playback frame may be copied through system RAM merely to feed RIFE.

The exact synchronization primitive used for D3D11/CUDA ownership may be selected during implementation after measurement on Turing hardware. Acceptable implementations include registered D3D11 graphics resources or external-memory/fence variants, provided they remain GPU-only and encapsulated inside the backend.

### 5.3 Tensor preprocessing/postprocessing

A small CUDA preprocessing stage converts the MPCVR interpolation input surface into the exact RIFE 4.6 tensor layout, including channel ordering, normalization, padding/alignment, and FP16 conversion.

A CUDA postprocessing stage converts the inference result into a D3D11-presentable texture used by the normal MPCVR rendering path.

TensorRT does not consume MPCVR's D3D11 texture format directly; the explicit conversion boundary keeps model layout and renderer layout decoupled.

### 5.4 Two-context default

The default `GPU threads = 2` creates two TensorRT execution contexts backed by independent CUDA streams and per-context scratch/output buffers while sharing immutable engine/model weights.

For a source pair requiring multiple synthetic outputs, jobs are distributed across the two contexts while output order remains timestamp-based.

The implementation must cap outstanding work so decode cannot run arbitrarily far ahead and consume unbounded VRAM.

## 6. RIFE inference contract

The fundamental backend operation is arbitrary-timestep synthesis:

```text
Frame A + Frame B + t -> Frame(t), where 0 < t < 1
```

The public backend abstraction must not encode a midpoint-only assumption.

A representative interface is:

```cpp
struct RifeInferenceRequest {
    ID3D11Texture2D* first;
    ID3D11Texture2D* second;
    float timestep;
    uint64_t generation;
};

struct RifeInferenceResult {
    ID3D11Texture2D* texture;
    uint64_t generation;
    double inferenceMs;
};
```

The concrete API may use owned handles/futures instead of raw pointers, but it must preserve arbitrary `t`, generation tagging, and asynchronous completion.

## 7. Timestamp-driven output scheduler

### 7.1 Core rule

RIFE does not decide timing. The scheduler creates the exact output timeline; RIFE only fills target timestamps that lie between two real source frames.

For source frames A and B at times `TA` and `TB`, every requested output time `T` where `TA < T < TB` yields:

```text
t = (T - TA) / (TB - TA)
```

The scheduler requests RIFE inference at that normalized timestep.

### 7.2 Integer and fractional multipliers

Integer multipliers naturally yield evenly spaced timesteps. For 5x:

```text
0.20, 0.40, 0.60, 0.80
```

Fractional multipliers such as 2.5x are not implemented as a hand-authored alternating cadence. They are generated from an exact target-time grid so the correct cadence falls out naturally without cumulative drift.

### 7.3 Fixed output rates and display matching

For fixed output rates and `To screen`, the scheduler creates a stable target-time grid independent of source-frame boundaries.

A source frame is presented directly only when its timestamp coincides with the output grid. Otherwise the frame shown at a target time is the corresponding RIFE result for the enclosing source pair.

This avoids perturbing a uniform 60/72/90/120/display-refresh cadence merely to force every source frame through presentation.

### 7.4 Clocking and audio

Interpolation never changes playback speed.

- audio timestamps and audio rate remain untouched;
- DirectShow/reference-clock time remains authoritative;
- source timestamps remain authoritative endpoints;
- video interpolation fills temporal gaps only.

The existing interpolation presenter concept is retained and generalized because it already tracks stream time, graph start, reference clock, and generation.

### 7.5 Lookahead and latency

RIFE requires both endpoint frames. The pipeline therefore carries one source-pair of lookahead.

The renderer must exploit normal decoded-sample lead time and may not stall the DirectShow graph merely to wait for RIFE.

When inference cannot complete before a presentation deadline, the system degrades gracefully:

1. drop synthetic outputs that are already too late or least useful;
2. present the nearest valid real/source-derived frame;
3. never block audio or freeze the graph waiting for neural inference.

## 8. Presentation queue

Replace the old singular `midpoint/output-ready` assumptions with a timestamped generated-frame representation.

Representative record:

```cpp
enum class InterpolatedFrameKind {
    Source,
    Rife,
    SceneRepeat,
    SceneBlend,
};

struct InterpolatedPresentationFrame {
    ID3D11Texture2D* texture;
    REFERENCE_TIME presentationTime;
    uint64_t generation;
    InterpolatedFrameKind kind;
    float timestep;
};
```

The final implementation should use owned/reference-counted texture wrappers appropriate to the existing renderer rather than unmanaged raw lifetime.

The presenter consumes ordered timestamps and does not need to understand how a frame was generated.

## 9. Scene-change detection

### 9.1 Default: driver-only NVOF

The default detector uses the NVIDIA display driver's native D3D11 Optical Flow API (`nvofapi64.dll` on x64) rather than NvOFFRUC.

The existing research implementation proves the repository already has working knowledge of:

- D3D11 NVOF session creation;
- driver API discovery;
- 4x4 flow output;
- bidirectional prediction;
- BGRA8 input registration;
- forward/backward flow resources.

The new production detector must extract only the scene-classification responsibility. It must not carry forward the old dense-flow synthesis, repair, jump-flood, warp, or midpoint code.

### 9.2 NVOF detector policy

Use bidirectional 4x4 optical flow with temporal hints disabled. Scene classification uses aggregate flow plausibility/consistency plus lightweight frame-difference evidence.

Do not enable NVOF hardware cost output on Turing in the initial implementation. The prior research path explicitly found live cost capture could severely stall startup, seeks, and sustained playback on Turing. The scene detector therefore starts from bidirectional flow consistency and frame statistics only.

Use a low-overhead NVOF performance preset suitable for classification rather than synthesis. Initial preset: `PerfFast`. If validation shows materially worse scene-cut classification than `PerfMedium`, switch the default to `PerfMedium`; this is an internal tuning decision and not a user-facing setting.

### 9.3 Image-comparison detector

The image-comparison option uses a small GPU-side spatial sample rather than full-resolution CPU readback.

The initial classifier is based on the already validated research heuristic:

- coarse spatial sampling;
- normalized mean absolute intensity difference;
- spatial correlation;
- cut only when both correlation is sufficiently low and absolute change is sufficiently high.

Initial thresholds are inherited from the existing shader research values:

- correlation threshold: `0.15`;
- normalized MAD threshold: `0.055`.

These are initial production defaults, not UI controls.

### 9.4 Detector failure

If the user selects NVOF but the API is unavailable or initialization fails, the renderer automatically falls back to Image comparison for that playback session and reports the fallback in statistics/status text.

If Image comparison also fails, scene-change detection becomes Disabled while playback continues.

## 10. Scene-change processing

When a source pair is classified as a scene cut, RIFE is never invoked across that pair.

### Repeat frame — default

For each target output timestamp between A and B, choose the temporally nearest endpoint frame. This produces an instantaneous cut at the midpoint of the source interval without neural morphing.

### Blend adjacent frames

For target timestamps between A and B, generate a simple linear temporal blend using the same normalized `t` that RIFE would have received.

Scene processing changes the generator, not the scheduler; output timestamps remain identical.

## 11. Duplicate-frame removal

Default: `Do not remove`.

`Remove every other frame` is intentionally literal and deterministic. It removes alternating decoded source frames before source-pair construction, then the normal interpolation scheduler operates on the remaining timestamps.

The first implementation does not add automatic duplicate similarity thresholds or content analysis under this setting.

Any discontinuity, seek, flush, or new segment resets the alternating phase so behavior is deterministic within the new segment.

## 12. TensorRT runtime and model

### 12.1 Runtime choice

Use mainline NVIDIA TensorRT C++ APIs, not TensorRT-RTX, for the initial release.

MPCVR must dynamically load the runtime so the renderer DLL continues to load and play video when the optional RIFE runtime is absent.

### 12.2 Model

Initial and default model: RIFE 4.6 in ONNX form suitable for arbitrary-timestep inference.

The internal model registry is data-driven even though only 4.6 is supported initially. Future model additions must not require redesigning the scheduler or presenter.

### 12.3 Normal engine

With `Performance boost = Disabled`, build and cache a dynamic-shape FP16-capable TensorRT engine covering the supported playback range.

The profile should prioritize common video resolutions and support at least SD through 1080p in the first production milestone. Higher resolutions may be admitted when memory/performance validation succeeds.

### 12.4 Performance boost engine

With `Performance boost = Enabled`, build and cache a static exact-resolution TensorRT engine for the current RIFE input dimensions.

The UI semantics are:

- disabled: broad reusable engine, fewer builds;
- enabled: per-resolution optimization/build, potentially better throughput and lower workspace/runtime memory.

### 12.5 Engine cache

Use an application-local cache such as:

```text
%LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache\
```

Cache identity includes at minimum:

- model content hash;
- TensorRT runtime/build version;
- CUDA/runtime compatibility identity;
- GPU identity/compute capability;
- precision policy;
- dynamic profile or exact input dimensions;
- performance-boost state.

A mismatch invalidates the cached engine and triggers a rebuild rather than attempting unsafe deserialization.

### 12.6 Runtime packaging

Do not commit NVIDIA runtime binaries or the RIFE ONNX model into the main source tree.

Provide a separate optional RIFE runtime installer/tool, following the same general repository philosophy as the existing Maxine/NvOFFRUC setup tools:

- acquire/install the pinned supported TensorRT runtime package;
- acquire/install the approved RIFE 4.6 model;
- verify file hashes;
- place assets in a known MPCVR RIFE runtime directory;
- optionally run a minimal compatibility probe.

The renderer itself only discovers and loads already-installed runtime/model assets.

## 13. Engine building UX

Engine construction may be expensive and must not freeze the player UI.

The renderer/backend exposes build state and progress/status text. Playback may continue without interpolation while an engine is being built if the architecture cannot safely make the build ready before playback starts.

Once a compatible engine is cached, subsequent sessions deserialize it directly.

For `Performance boost`, a new input resolution may trigger another static-engine build; this is expected and should be communicated in status text.

## 14. VRAM and overload policy

Before enabling the pipeline for a media type, estimate or observe enough memory headroom for:

- TensorRT engine/runtime allocations;
- two execution contexts by default;
- input and output tensors;
- CUDA preprocessing/postprocessing buffers;
- D3D11 shared/intermediate textures;
- MPCVR's existing renderer allocations.

If required allocations fail or safe headroom cannot be maintained, disable RIFE for the session and continue ordinary playback.

RIFE must never force device removal, uncontrolled allocation retry loops, or graph-wide stalls merely to satisfy the selected interpolation rate.

## 15. Lifecycle and recovery

The following events invalidate all pending interpolation work:

- seek;
- `NewSegment`;
- begin/end flush as appropriate;
- media-type or resolution change;
- display/device recreation;
- D3D11 device loss;
- RIFE backend reset;
- target-rate mode change requiring timeline rebuild.

On invalidation:

1. increment interpolation generation;
2. discard queued synthetic outputs from older generations;
3. invalidate the current source-frame pair;
4. reset duplicate-removal phase;
5. reset scene-detector history;
6. cancel or ignore outstanding inference completions tagged with an old generation;
7. restart scheduling from the next valid source pair.

TensorRT engine objects may remain cached/in memory across pause/resume when dimensions/device/runtime identity remain valid.

## 16. Failure behavior

All RIFE failures are soft playback failures unless the underlying D3D11 renderer itself has failed.

Examples:

- TensorRT runtime missing -> RIFE disabled, normal playback continues.
- Model missing/hash invalid -> RIFE disabled, normal playback continues.
- Unsupported NVIDIA adapter -> RIFE disabled, normal playback continues.
- Engine deserialize/build failure -> RIFE disabled for session, normal playback continues.
- CUDA/D3D11 interop failure -> RIFE disabled for session, normal playback continues.
- NVOF scene detector unavailable -> fall back to Image comparison.
- Inference misses deadline -> drop that synthetic result and continue clocked presentation.
- Out of VRAM -> tear down RIFE allocations and continue normal playback.

No optional RIFE dependency may make the MPCVR filter DLL fail to register/load.

## 17. Statistics and diagnostics

Expose enough telemetry to debug real-time behavior without requiring a debugger.

Minimum runtime statistics:

- RIFE enabled/disabled/status;
- model name/version;
- TensorRT runtime identity;
- selected GPU;
- execution-context count;
- target output rate;
- current source rate estimate;
- inference time, moving average, and recent maximum;
- synthetic frames generated;
- synthetic frames dropped/late;
- presentation queue depth;
- scene detector mode and fallback state;
- detected scene-cut count;
- engine cache hit/miss/build state;
- performance-boost state.

Diagnostic logging must include generation changes, engine rebuild reasons, scene-detector fallback, and backend teardown causes.

## 18. Production-code migration

The RIFE implementation supersedes the current production interpolation direction.

During implementation:

- replace `CNvidiaFrameInterpolation`/NvOFFRUC interpolation use with the new pipeline;
- remove the midpoint-specific renderer state once no callers depend on it;
- retire production references to NvOFFRUC runtime installation for frame interpolation;
- do not delete historical research docs or experimental branches;
- retain/reuse only narrowly applicable NVOF driver API code for scene detection;
- do not reuse the old dense NVOF synthesizer in the RIFE frame-generation path.

The Maxine VSR/denoise/deblur integration remains independent and is not removed by this project.

## 19. Testing strategy

### 19.1 Pure scheduler tests

Create deterministic tests for:

- 24 -> 48, 72, 96, 120;
- 24000/1001 -> 60000/1001 and 120000/1001;
- 24 -> 60 fixed-rate phase progression;
- 2.5x cadence without cumulative drift;
- target-display rates represented as rational/non-integer values;
- source timestamp jitter within allowed media timing;
- output ordering and no duplicate timestamps;
- generation reset on seek/flush.

Scheduler tests must not require a GPU.

### 19.2 Scene-detector tests

Image comparison:

- identical frames -> no cut;
- small exposure/noise change -> no cut;
- fast translated/panned synthetic content -> no cut where correlation remains coherent;
- unrelated hard-cut pair -> cut;
- dark hard-cut pair -> cut.

NVOF integration:

- capability probe succeeds/fails cleanly;
- bidirectional flow resources initialize on supported hardware;
- detector fallback to image comparison works when NVOF is unavailable;
- no hardware-cost surface is required in the Turing path.

### 19.3 Duplicate policy tests

Verify `Remove every other frame`:

- drops exactly alternating source frames;
- resets phase on generation/segment reset;
- leaves timestamps of retained frames unchanged;
- is bypassed completely in `Do not remove` mode.

### 19.4 TensorRT probe/integration tests

Provide a small standalone RIFE probe tool that can:

- discover runtime/model assets;
- build or deserialize an engine;
- allocate one and two execution contexts;
- run a known frame pair at `t=0.25`, `0.5`, and `0.75`;
- write timing/status output;
- verify cache reuse on a second run;
- fail cleanly when runtime/model/GPU support is absent.

The probe is used before wiring the backend into timed playback.

### 19.5 Renderer integration tests

Validate in-player behavior for:

- 23.976/24/25/30 fps sources;
- 2x, 2.5x, 3x, 4x, 5x;
- 60/72/90/120 fixed output rates;
- `To screen` on non-integer refresh rates;
- pause/resume;
- repeated seeking;
- rapid file close/open;
- fullscreen/window/display transitions;
- scene cuts under Repeat and Blend;
- backend failure while playback continues;
- sustained playback with queue depth bounded.

### 19.6 Performance acceptance

The first milestone is successful real-time 1080p RIFE playback at practical movie-to-60-class output rates on the target RTX 2070 SUPER without CPU readback and without destabilizing MPCVR.

Higher output rates are best-effort and must degrade by dropping synthetic frames rather than blocking playback when inference throughput is insufficient.

Performance tuning is based on measured inference time, presentation lateness, queue depth, and VRAM use rather than theoretical GPU utilization alone.

## 20. Implementation sequence

Implement in this order so each stage is independently testable:

1. timestamp/rational scheduler and unit tests;
2. scene-change detector abstraction plus image-comparison implementation;
3. driver-only NVOF detector extracted from research code;
4. TensorRT runtime/model discovery and standalone RIFE probe;
5. TensorRT engine build/cache and arbitrary-timestep inference;
6. D3D11/CUDA zero-copy interop and preprocessing/postprocessing;
7. generalized generated-frame queue/presenter integration;
8. scene-repeat/scene-blend and duplicate policy integration;
9. property-page controls and registry persistence;
10. statistics, diagnostics, runtime installer, and cleanup of production NvOFFRUC interpolation code;
11. end-to-end performance and seek/device-reset validation.

## 21. Final default profile

The initial out-of-box profile is:

```text
Frame interpolation      Disabled until user enables it
Engine                   NVIDIA TensorRT
GPU threads              2
AI model                 RIFE 4.6
GPU device               Auto
Performance boost        Disabled
Scene change detection   NVOF motion vectors
Scene change processing  Repeat frame
Duplicate frames         Do not remove
```

When enabled, target rate is selected independently from the RIFE options and may be `To screen`, multiplier, or fixed FPS.

## 22. Success criteria

The project is successful when:

- RIFE 4.6 can generate arbitrary-timestep frames directly inside MPCVR;
- 2.5x/3x/4x/5x and fixed/display-matched rates work from a timestamp-driven scheduler;
- normal inference remains GPU-resident with no per-frame CPU round trip;
- scene cuts do not pass through RIFE under the default Repeat policy;
- NVOF scene detection works through the display driver with Image comparison fallback;
- seeking/device reset cannot present stale synthetic frames;
- optional runtime/model absence never breaks ordinary playback;
- overload drops synthetic frames instead of stalling audio/video;
- the user-facing options remain recognizably close to SVP's RIFE panel;
- the old NvOFFRUC/native-warp interpolation direction is no longer part of the production frame-generation path.
