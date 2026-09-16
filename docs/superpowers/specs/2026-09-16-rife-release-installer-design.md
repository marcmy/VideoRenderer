# MPCVR Maxine + RIFE Release and Installer Design

**Date:** 2026-09-16  
**Status:** Approved design direction  
**Repository:** `marcmy/VideoRenderer`  
**Branch baseline:** `feature/rife-tensorrt-interpolation` at `4f76664a79510555f24d5c7f5d8a688ad1030238`

## 1. Goal

Ship the working MPC Video Renderer RIFE 4.6 implementation as a practical Windows release that a K-Lite/MPC-HC user can install in one pass, including:

- the current custom MPC Video Renderer;
- NVIDIA Maxine Video Super Resolution support;
- RIFE 4.6 TensorRT interpolation;
- the MPCVR RIFE runtime DLL;
- the required CUDA/TensorRT runtime files;
- the RIFE 4.6 ONNX model;
- architecture-appropriate TensorRT builder resources;
- local TensorRT engine-cache setup;
- backup, replacement, verification, and rollback for an existing K-Lite MPCVR installation.

The first external validation target is an RTX 5070/Blackwell system. The release architecture must not depend on a hand-maintained list of individual NVIDIA GPU product names.

## 2. Scope and non-goals

This release milestone covers the NVIDIA TensorRT backend only.

It does not yet implement:

- the ncnn/Vulkan backend;
- AMD or Intel neural interpolation;
- automatic download or selection of multiple RIFE model families;
- RIFE 4.9 through 4.16-lite model selection;
- SVP compatibility or SVP configuration import;
- distribution of prebuilt TensorRT `.plan` engines;
- a GPU benchmark/calibration wizard.

Those remain follow-on work after the NVIDIA package is stable.

## 3. Compatibility policy

### 3.1 Capability-based support

GPU compatibility is defined by CUDA compute capability and the pinned CUDA/TensorRT release, not by a list of GeForce/RTX SKU names.

The installer/runtime must:

1. identify the NVIDIA adapter used by MPCVR;
2. determine the CUDA compute capability exposed by the installed CUDA runtime/driver path;
3. verify that the shipped MPCVR CUDA code contains executable code or compatible PTX for that architecture;
4. verify that the matching TensorRT builder resource is present when an engine must be built;
5. fail with an explicit compatibility diagnostic when the architecture is unsupported.

The initial desktop architecture set is:

- Turing: SM 7.5;
- Ampere: SM 8.6;
- Ada: SM 8.9;
- Blackwell desktop: SM 12.0.

Other TensorRT-capable architectures can be added without changing installer structure.

### 3.2 RTX 5070 requirement

The RTX 5070 is the first Blackwell validation target. The shipped runtime must therefore include an `sm_120` CUDA target and the TensorRT builder resource required to construct an engine on compute capability 12.0 hardware.

The current runtime build's `CUDA_ARCHITECTURES "75;80;86;89"` is insufficient for this release and must be expanded as part of implementation.

### 3.3 Representative hardware validation

The project does not require testing every TensorRT-capable GPU SKU. Release confidence is built from:

- CI build/packaging coverage for every supported compute architecture;
- installer tests that do not require physical hardware;
- real playback validation on representative architecture families as hardware is available;
- runtime capability checks and actionable failure diagnostics on unknown/unsupported systems.

The existing RTX 2070 SUPER is the Turing validation machine. The RTX 5070 is the immediate Blackwell validation machine.

## 4. Runtime and engine strategy

### 4.1 No precompiled TensorRT plans

TensorRT `.plan` files remain machine-local. They are never included in release assets.

The runtime continues to build an engine on first use and store it under:

`%LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache`

The existing cache identity already includes model hash, TensorRT version, compute capability, GPU name, and performance-profile information. That is the correct foundation for per-machine engine reuse.

### 4.2 Runtime location

The default installed RIFE tree is:

```text
%LOCALAPPDATA%\MPCVideoRenderer\RIFE\
    runtime\
        MPCVRRifeRuntime64.dll
        cudart64_12.dll
        nvinfer_11.dll
        nvonnxparser_11.dll
        nvinfer_builder_resource_ptx_11.dll
        nvinfer_builder_resource_<architecture>_11.dll
        ...other required pinned TensorRT runtime files...
    models\
        rife_v4.6.onnx
    cache\
        *.plan
```

The renderer's existing module-local and environment-override search paths remain valid for advanced/manual installs. The one-click installer uses the LocalAppData path above.

### 4.3 Architecture-specific builder resources

TensorRT builder resources are treated as architecture payloads rather than forcing every user to install every builder DLL.

The release system produces a common RIFE runtime payload plus architecture-specific builder packs. At install time, the setup selects only the builder resource required for the detected GPU.

The common payload contains files needed regardless of architecture, including the MPCVR runtime, CUDA runtime, TensorRT runtime/parser, PTX builder resource where required, model, provenance, and checksums.

The architecture pack contains the TensorRT builder resource for the selected compute capability.

## 5. Packaging approach

The approved packaging model is a hybrid, architecture-aware installer.

### 5.1 Common package

One setup bundle contains:

- installer scripts;
- custom MPCVR renderer package;
- Maxine runtime package;
- RIFE 4.6 model;
- MPCVR RIFE runtime;
- common CUDA/TensorRT runtime components;
- checksums and provenance metadata;
- architecture-selection logic;
- backup/rollback logic.

### 5.2 Architecture packs

Architecture-specific TensorRT builder resources are packaged independently so the installer can install the minimum required payload.

The first required packs are:

- `sm75`;
- `sm86`;
- `sm89`;
- `sm120`.

Where redistribution terms permit bundling, the corresponding pack can be embedded in an offline installer variant. Where redistribution requires external acquisition, the setup fetches the pinned official NVIDIA asset, verifies its checksum, and extracts only the required files.

The implementation must keep acquisition policy separate from detection/installation logic so licensing changes do not require rewriting the installer.

### 5.3 Public release assets

The intended public UX is:

1. a recommended K-Lite/MPC-HC setup bundle;
2. a manual/portable MPCVR package;
3. checksums for every public artifact;
4. architecture/runtime payloads only when they are not fully embedded in the recommended setup.

The existing Maxine release assets and updater compatibility should be migrated rather than discarded abruptly.

## 6. Installer behavior

The existing `tools/MaxineSetup`, `tools/MaxineRuntimeInstaller`, and `tools/KLiteMaxineUpdater` flows are the base. They should be generalized into a Maxine + RIFE setup rather than replaced with an unrelated installer system.

The setup performs these operations in order:

1. Verify Windows/x64 prerequisites.
2. Require MPC-HC to be closed before mutating renderer/runtime files.
3. Detect K-Lite/MPC-HC installation paths.
4. Detect the active NVIDIA GPU and compute capability.
5. Verify the GPU architecture is supported by the packaged runtime policy.
6. Verify all payload hashes before installation.
7. Back up the currently installed custom or stock MPCVR files that will be replaced.
8. Install/update the Maxine runtime and `NV_VIDEO_EFFECTS_PATH` using the existing transactional mechanism.
9. Stage the complete RIFE runtime/model tree in a temporary directory.
10. Select and stage the architecture-appropriate TensorRT builder resource.
11. Atomically replace the installed RIFE runtime tree.
12. Install the x64 renderer to:

   `C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax`

13. Preserve any supported 32-bit K-Lite renderer update behavior already used by the existing updater.
14. Verify installed hashes after every copy/move.
15. Run a non-playback preflight that checks renderer/runtime/model presence and runtime ABI compatibility.
16. Create/update the existing restore/updater shortcut with the new Maxine + RIFE package identity.
17. Print a concise verification checklist for the first MPC-HC launch.

An older custom renderer installation is treated as an upgrade candidate, not as an error.

## 7. Transactional installation and rollback

The installer must preserve a working pre-install state until the new state is verified.

For user-local runtime trees, use staging plus atomic directory replacement, following the current Maxine runtime installer pattern.

For K-Lite renderer files:

- capture the original destination file and hash before replacement;
- keep a rollback copy until post-install verification succeeds;
- if installation fails after replacement, restore the original file;
- never delete unrelated K-Lite files or folders;
- never treat an old Marc custom build differently from stock solely by filename.

The setup must be safe to run repeatedly. Reinstalling the same release should validate and converge on the same state.

## 8. Release build pipeline

### 8.1 Renderer

The release renderer is built from Marc's current fork mainline after the RIFE feature work is integrated. The release process must verify that the intended RIFE/Maxine commit ancestry is present before publishing.

The current feature branch is based directly on `origin/master` at `0895e03b`; there is no separate fork-main delta that must be manually reconciled before integration.

### 8.2 RIFE runtime

The existing `.github/workflows/rife-runtime.yml` remains the durable runtime build workflow and is extended rather than replaced.

It must:

- build with the pinned CUDA/TensorRT versions selected for the release;
- compile the MPCVR CUDA code for the supported architecture set, including `sm_120`;
- package the custom runtime DLL;
- identify the exact NVIDIA runtime dependencies required by the built runtime;
- produce architecture-pack metadata;
- write provenance and SHA-256 files;
- fail when a declared architecture pack is missing.

No disposable one-shot workflow files are introduced.

### 8.3 RIFE model

The existing `.github/workflows/rife-model.yml` remains the source of the RIFE 4.6 ONNX artifact.

The release assembler consumes the exact checksum-verified model artifact produced by that workflow. The model file is not rebuilt ad hoc inside the installer workflow.

### 8.4 Release assembly

The existing main release workflow is generalized from Maxine-only setup assembly to Maxine + RIFE assembly.

Release assembly consumes independently verified payloads:

- renderer artifact;
- Maxine runtime artifact;
- RIFE runtime/common payload;
- RIFE model artifact;
- TensorRT architecture pack(s).

The assembler verifies every component before creating the final setup archive and then writes top-level checksums for the public assets.

## 9. Licensing and redistribution gate

The release is not published until redistribution terms are verified for the exact NVIDIA files included in the package.

The packaging code must support both of these acquisition modes without changing installed layout:

- bundled redistributable payload;
- checksum-pinned download/extraction from an official NVIDIA source.

The release package must include the required NVIDIA notices and the applicable RIFE/model license notices.

Licensing is a release gate, not a runtime responsibility.

## 10. Diagnostics and failure behavior

Installation/runtime errors must identify the actual failing layer.

Examples of useful diagnostics:

```text
GPU: NVIDIA GeForce RTX 5070
CUDA compute capability: 12.0
MPCVR CUDA target: supported (sm_120)
TensorRT runtime: 11.2.x
TensorRT builder resource: sm120 present
RIFE model: rife_v4.6.onnx verified
Runtime ABI: compatible
Engine cache: %LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache
```

Unsupported systems should report a specific reason such as:

- unsupported compute capability;
- required architecture builder resource missing;
- CUDA runtime/driver incompatibility;
- TensorRT runtime load failure;
- model missing or checksum mismatch;
- MPCVR RIFE runtime ABI mismatch.

The renderer must still load and play normally when RIFE is unavailable; RIFE remains an optional subsystem.

## 11. Verification strategy

### 11.1 CI

CI must verify:

- CMake accepts and builds the declared CUDA architecture set;
- `sm_120` is part of the shipping runtime build;
- expected runtime exports and ABI are present;
- all common and architecture-specific payloads have manifests/checksums;
- installer `-ValidateOnly` paths succeed;
- setup assembly fails when a required component or checksum is missing;
- K-Lite target-path handling includes the x64 MPC-HC `MPCVR` directory;
- backup/rollback operations restore the original test fixture after an injected install failure;
- repeated installation is idempotent on test fixtures.

### 11.2 Real hardware

Turing validation on the RTX 2070 SUPER must cover:

- 30 -> 60;
- 60 -> 120 where GPU throughput permits;
- file switching;
- pause/resume;
- seek/restart behavior;
- Maxine + RIFE combined playback;
- first-run engine build and subsequent cached startup.

Blackwell validation on the RTX 5070 must cover the same lifecycle plus proof that:

- the runtime identifies compute capability 12.0;
- the `sm120` builder resource is loaded successfully;
- a TensorRT plan is built locally;
- subsequent playback reuses the plan;
- no manual CUDA/TensorRT installation is required beyond the setup package.

### 11.3 Release gate

The first NVIDIA release is ready when:

1. installer validation passes in CI;
2. the shipping runtime includes Blackwell support;
3. the renderer package is built from the intended integrated fork mainline;
4. Maxine and RIFE payload checksums/provenance are complete;
5. redistribution/licensing review is complete;
6. RTX 2070 SUPER playback remains regression-free;
7. RTX 5070 can install from a clean/old-custom K-Lite state and successfully build/use RIFE 4.6.

## 12. Migration from the current Maxine setup

The current Maxine one-click system is evolved in place:

- `MPCVR-Maxine-Setup.zip` becomes a Maxine + RIFE setup identity in the release workflow;
- existing Maxine runtime install code remains responsible for Maxine;
- a new RIFE runtime installer module owns the RIFE tree and architecture pack;
- the K-Lite updater installs the current renderer package and retains backward-compatible checksum behavior where practical;
- the desktop restore shortcut is renamed/reworded to reflect the full custom MPCVR stack;
- old Maxine-only installs can be upgraded without manual cleanup.

The release workflow may retain compatibility assets temporarily for older updater shortcuts, but new releases should direct users to the unified setup.

## 13. Follow-on architecture

Once the NVIDIA release is stable, frame interpolation should become a backend/model matrix instead of a TensorRT-only feature.

The next architectural milestone is:

```text
Frame interpolation UI/pipeline
    -> backend
        -> NVIDIA TensorRT
        -> ncnn/Vulkan
    -> model
        -> RIFE 4.6
        -> RIFE 4.9
        -> RIFE 4.16-lite
        -> additional validated models
```

The scheduler, presentation path, rule system, scene detection, and diagnostics remain shared. Backend/model selection should not duplicate the playback pipeline.

This follow-on work gets its own design and implementation plan after the NVIDIA installer/release milestone is complete.
