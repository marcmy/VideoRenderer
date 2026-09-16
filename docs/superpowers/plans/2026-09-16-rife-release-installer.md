# MPCVR Maxine + RIFE Release Installer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the existing RIFE 4.6 + Maxine renderer as a one-pass K-Lite/MPC-HC installer that supports Turing, Ampere, Ada, and Blackwell desktop GPUs, including an RTX 5070/SM120 validation path.

**Architecture:** Keep MPCVR itself independent of CUDA/TensorRT, extend the existing optional RIFE runtime to compile for the supported compute capabilities, and split TensorRT distribution into a common runtime payload plus architecture-specific builder packs. Evolve the existing Maxine setup/K-Lite updater in place so the unified installer verifies payloads, detects the NVIDIA compute capability, transactionally installs Maxine/RIFE/renderer files, and can roll back renderer/runtime changes when verification fails.

**Tech Stack:** C++20, D3D11, CUDA 12.9.1, TensorRT 11.2.1.2, CMake/Ninja, PowerShell 5.1+, GitHub Actions, K-Lite Codec Pack/MPC-HC.

**Spec:** `docs/superpowers/specs/2026-09-16-rife-release-installer-design.md`

## Global Constraints

- Windows x64 is the supported RIFE/TensorRT platform; MPCVR must continue loading normally when the optional RIFE runtime is absent.
- Keep CUDA/TensorRT out of the main renderer link; `MPCVRRifeRuntime64.dll` remains dynamically loaded through `Source/RifeFrameInterpolation.cpp`.
- Pin CUDA to `12.9.1` and TensorRT to `11.2.1.2` for this release.
- Shipping desktop compute capabilities are `7.5`, `8.6`, `8.9`, and `12.0`; package keys are `sm75`, `sm86`, `sm89`, and `sm120`.
- Build real CUDA code for all four shipping architectures and retain PTX for the newest architecture so the binary is not restricted to the 2070 SUPER build target.
- Never distribute TensorRT `.plan` files. Plans remain machine-local under `%LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache`.
- Default RIFE install root is `%LOCALAPPDATA%\MPCVideoRenderer\RIFE`; advanced module-local and `MPCVR_RIFE_RUNTIME_DIR` lookup behavior stays intact.
- Install `MpcVideoRenderer64.ax` to `C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax`; preserve the existing supported x86 K-Lite target.
- Reuse the current `tools/MaxineRuntimeInstaller`, `tools/MaxineSetup`, and `tools/KLiteMaxineUpdater` flows rather than introducing a separate installer technology.
- Preserve existing generated/untracked local test artifacts; never stage them with release work.
- Do not add disposable GitHub workflows. Extend the durable workflows already in the repository.
- Publication is blocked until the exact bundled NVIDIA and RIFE/model payloads have the required redistribution/license notices.

---

### Task 1: Make the RIFE runtime explicitly support the shipping GPU architectures

**Files:**
- Modify: `tools/RifeTensorRTRuntime/CMakeLists.txt:81-96`
- Modify: `Source/RifeRuntimeApi.h:18-55`
- Modify: `tools/RifeTensorRTRuntime/RifeTensorRTRuntime.cpp:32-40,252-304,398-483`
- Modify: `Source/RifeFrameInterpolation.h`
- Modify: `Source/RifeFrameInterpolation.cpp:232-281`
- Modify: `tools/rife-runtime-abi-test/FakeRifeRuntime.cpp:1-35`
- Modify: `tools/rife-runtime-abi-test/RifeRuntimeAbiTest.cpp:18-64`
- Modify: `tools/rife-runtime-abi-test/build-test.cmd`
- Modify: `tools/RifeTensorRTRuntime/README.md`

**Interfaces:**
- Consumes: the existing ABI functions `MpcvrRifeGetAbiVersion`, `MpcvrRifeCreate`, `MpcvrRifeInterpolate`, and `MpcvrRifeDestroy`.
- Produces: named `MpcvrRifeResult` values shared by the renderer/runtime; a runtime binary containing `sm75`, `sm86`, `sm89`, and `sm120` machine code plus `compute_120` PTX; explicit result codes for unsupported compute capability and missing architecture builder resources.

- [ ] **Step 1: Add ABI-level result-code assertions to the fake-runtime test**

Extend `RifeRuntimeAbiTest.cpp` so the fake runtime can be configured to return each new failure result and the renderer-side status string contains a stable layer-specific phrase. Cover at minimum:

```cpp
Check(DescribeRifeResult(MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY).find(L"compute capability") != std::wstring::npos,
    "unsupported architecture must identify compute capability");
Check(DescribeRifeResult(MPCVR_RIFE_BUILDER_RESOURCE_MISSING).find(L"builder resource") != std::wstring::npos,
    "missing TensorRT builder resource must be explicit");
```

Expose `DescribeRifeResult(int result)` from `RifeFrameInterpolation.cpp` through `RifeFrameInterpolation.h` only if the test needs a callable seam; otherwise exercise the string through `Initialize()` with fake return codes.

- [ ] **Step 2: Run the ABI test and verify it fails before the contract exists**

Run:

```cmd
tools\rife-runtime-abi-test\build-test.cmd
```

Expected: FAIL because the named result values/status mapping do not exist yet.

- [ ] **Step 3: Define the stable runtime result contract**

In `Source/RifeRuntimeApi.h`, add:

```cpp
enum MpcvrRifeResult : int32_t {
    MPCVR_RIFE_OK = 0,
    MPCVR_RIFE_INVALID_ARGUMENT = -1,
    MPCVR_RIFE_UNSUPPORTED = -2,
    MPCVR_RIFE_CUDA_FAILURE = -3,
    MPCVR_RIFE_TENSORRT_FAILURE = -4,
    MPCVR_RIFE_BUILDER_RESOURCE_MISSING = -5,
    MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY = -6,
};
```

Replace the private numeric constants in `RifeTensorRTRuntime.cpp` with these values. Do not bump `MPCVR_RIFE_RUNTIME_ABI`; function signatures and struct layout remain unchanged.

- [ ] **Step 4: Add runtime architecture and builder-resource validation**

After `cudaGetDeviceProperties()` succeeds, accept exactly `{7.5, 8.6, 8.9, 12.0}` for this release. Derive the expected TensorRT builder filename from the detected capability:

```cpp
const auto builder = runtimeDirectory /
    std::filesystem::path(std::format(
        L"nvinfer_builder_resource_sm{}{}_11.dll", prop.major, prop.minor));
```

Return `MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY` before engine creation for other capabilities, and `MPCVR_RIFE_BUILDER_RESOURCE_MISSING` when the matching file is absent. Keep `nvinfer_builder_resource_ptx_11.dll` in the common runtime payload and let TensorRT load it from the existing `setInternalLibraryPath()` directory.

- [ ] **Step 5: Map runtime failures to useful renderer status text**

Replace the generic `"RIFE runtime initialization failed with code {}"` path in `RifeFrameInterpolation.cpp` with deterministic strings for the new enum values. The two new messages must be:

```text
RIFE runtime does not support this CUDA compute capability
RIFE TensorRT builder resource for this GPU architecture is missing
```

Preserve the numeric code in the text for unknown future failures.

- [ ] **Step 6: Expand CUDA code generation to Blackwell**

Set the CMake property to:

```cmake
CUDA_ARCHITECTURES "75-real;86-real;89-real;120"
```

The unsuffixed `120` intentionally emits both real `sm_120` code and `compute_120` PTX; the older shipping targets remain real-code-only.

- [ ] **Step 7: Run the ABI tests again**

Run:

```cmd
tools\rife-runtime-abi-test\build-test.cmd
```

Expected: `All RIFE runtime ABI tests passed`.

- [ ] **Step 8: Commit the runtime compatibility contract**

```bash
git add Source/RifeRuntimeApi.h Source/RifeFrameInterpolation.cpp Source/RifeFrameInterpolation.h tools/RifeTensorRTRuntime/CMakeLists.txt tools/RifeTensorRTRuntime/RifeTensorRTRuntime.cpp tools/RifeTensorRTRuntime/README.md tools/rife-runtime-abi-test
git commit -m "feat: add RIFE GPU architecture compatibility gate"
```

---

### Task 2: Package a common TensorRT runtime and architecture-specific builder packs

**Files:**
- Modify: `.github/workflows/rife-runtime.yml:3-188`
- Create: `tools/RifeRuntimeInstaller/RifeRuntimeManifest.psm1`
- Create: `tools/RifeRuntimeInstaller/Test-RifeRuntimePackage.ps1`
- Create: `tools/RifeRuntimeInstaller/README.md`
- Create: `.github/workflows/validate-rife-runtime-installer.yml`

**Interfaces:**
- Consumes: TensorRT `11.2.1.2` archive, CUDA `12.9.1` runtime, `MPCVRRifeRuntime64.dll` from Task 1.
- Produces: `MPCVR-RIFE-Common.zip`, `MPCVR-RIFE-sm75.zip`, `MPCVR-RIFE-sm86.zip`, `MPCVR-RIFE-sm89.zip`, `MPCVR-RIFE-sm120.zip`, `runtime-manifest.json`, and `SHA256SUMS.txt` inside one Actions artifact named `mpcvr-rife-runtime-bundle`.

- [ ] **Step 1: Write the package-manifest validator first**

Create `RifeRuntimeManifest.psm1` with pure functions:

```powershell
function ConvertTo-RifeArchitectureKey([int]$Major, [int]$Minor) {
    $key = '{0}.{1}' -f $Major, $Minor
    switch ($key) {
        '7.5'  { 'sm75'; break }
        '8.6'  { 'sm86'; break }
        '8.9'  { 'sm89'; break }
        '12.0' { 'sm120'; break }
        default { throw "Unsupported CUDA compute capability: $key" }
    }
}

function Get-RifeBuilderResourceName([string]$Architecture) {
    if ($Architecture -notin @('sm75','sm86','sm89','sm120')) {
        throw "Unsupported RIFE architecture pack: $Architecture"
    }
    "nvinfer_builder_resource_${Architecture}_11.dll"
}
```

`Test-RifeRuntimePackage.ps1` must accept `-BundleRoot` and fail unless the common ZIP, four architecture ZIPs, manifest, and top-level checksum list all exist and verify.

- [ ] **Step 2: Add a CI fixture that proves a missing pack fails**

In `validate-rife-runtime-installer.yml`, create mock archives named exactly like the shipping files, write a manifest declaring all four architectures, omit `MPCVR-RIFE-sm120.zip`, and run:

```powershell
& tools/RifeRuntimeInstaller/Test-RifeRuntimePackage.ps1 -BundleRoot $fixture
if ($LASTEXITCODE -eq 0) { throw 'Missing sm120 pack was not rejected.' }
```

Expected: validator exits non-zero and mentions `sm120`.

- [ ] **Step 3: Run the validator workflow logic locally against the failing fixture**

Run the PowerShell block from Step 2 with a temporary directory under `$env:TEMP`.

Expected: failure because `MPCVR-RIFE-sm120.zip` is absent.

- [ ] **Step 4: Extend `rife-runtime.yml` to stage exact common files**

After building the runtime, stage this common directory:

```text
common/
  MPCVRRifeRuntime64.dll
  cudart64_12.dll
  nvinfer_11.dll
  nvonnxparser_11.dll
  nvinfer_builder_resource_ptx_11.dll
  BUILD-INFO.txt
```

Do not copy `nvinfer_plugin_11.dll` unless `dumpbin /dependents` or a real RIFE engine build proves it is required. Keep the dependency check authoritative.

- [ ] **Step 5: Stage architecture packs from the pinned TensorRT archive**

For each key in `sm75`, `sm86`, `sm89`, `sm120`, require exactly:

```text
architecture/<key>/nvinfer_builder_resource_<key>_11.dll
```

The workflow must throw if any declared file is absent in the pinned TensorRT package; do not silently omit an architecture.

- [ ] **Step 6: Emit one machine-readable manifest**

Write `runtime-manifest.json` with this schema:

```json
{
  "schemaVersion": 1,
  "cudaVersion": "12.9.1",
  "cudaRuntimeVersion": "12.9.79",
  "tensorRtVersion": "11.2.1.2",
  "runtimeAbi": 1,
  "architectures": [
    {"computeCapability":"7.5","key":"sm75","builderResource":"nvinfer_builder_resource_sm75_11.dll"},
    {"computeCapability":"8.6","key":"sm86","builderResource":"nvinfer_builder_resource_sm86_11.dll"},
    {"computeCapability":"8.9","key":"sm89","builderResource":"nvinfer_builder_resource_sm89_11.dll"},
    {"computeCapability":"12.0","key":"sm120","builderResource":"nvinfer_builder_resource_sm120_11.dll"}
  ]
}
```

- [ ] **Step 7: Zip and checksum the runtime outputs**

Produce the five ZIPs named in `Interfaces`, then write `SHA256SUMS.txt` over all five. The Actions artifact must contain only the five ZIPs, manifest, checksum list, build provenance, and carried license/notice files.

- [ ] **Step 8: Make the runtime workflow reusable by the release workflow**

Add `workflow_call:` alongside `workflow_dispatch`/`push`. Preserve the existing standalone triggers, and keep the artifact name fixed at `mpcvr-rife-runtime-bundle` so the main release workflow can download it from the same run when invoked as a reusable workflow job.

- [ ] **Step 9: Make the package validation fixture pass and run it under both PowerShell hosts**

Add the missing mock `sm120` pack, calculate hashes, then run:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File tools/RifeRuntimeInstaller/Test-RifeRuntimePackage.ps1 -BundleRoot $fixture
pwsh.exe -NoLogo -NoProfile -File tools/RifeRuntimeInstaller/Test-RifeRuntimePackage.ps1 -BundleRoot $fixture
```

Expected: both exit 0.

- [ ] **Step 10: Commit runtime packaging**

```bash
git add .github/workflows/rife-runtime.yml .github/workflows/validate-rife-runtime-installer.yml tools/RifeRuntimeInstaller
git commit -m "build: package RIFE TensorRT architecture runtimes"
```

---

### Task 3: Make the RIFE model artifact release-consumable and license-auditable

**Files:**
- Modify: `.github/workflows/rife-model.yml:3-68`
- Modify: `tools/RifeTensorRTRuntime/prepare_model.py`
- Create: `tools/RifeRuntimeInstaller/model-manifest.schema.json`

**Interfaces:**
- Consumes: the pinned `AmusementClub/vs-mlrt` `model-20220923/rife_v8.7z` source archive.
- Produces: Actions artifact `mpcvr-rife-v4.6-model` containing `rife_v4.6.onnx`, `SOURCE.txt`, `SHA256SUMS.txt`, `model-manifest.json`, and the model/source license notice carried from the pinned upstream source.

- [ ] **Step 1: Add model-manifest validation to `prepare_model.py` tests/CLI path**

Require the output manifest to contain:

```json
{
  "schemaVersion": 1,
  "model": "RIFE 4.6",
  "file": "rife_v4.6.onnx",
  "sourceRelease": "model-20220923",
  "input": [1, 11, "H", "W"],
  "output": [1, 3, "H", "W"],
  "precision": "mixed-fp16-fp32"
}
```

Use a small Python unit that validates the manifest shape without rebuilding the ONNX file.

- [ ] **Step 2: Run the manifest test before workflow changes**

Run the new Python test directly.

Expected: FAIL because `model-manifest.json` is not produced yet.

- [ ] **Step 3: Pin and record the source archive hash**

Download the 195,245,748-byte release asset once and compute its SHA-256 with:

```powershell
curl.exe --fail --location --retry 3 `
  --output rife_v8.7z `
  https://github.com/AmusementClub/vs-mlrt/releases/download/model-20220923/rife_v8.7z
(Get-FileHash -Algorithm SHA256 .\rife_v8.7z).Hash.ToLowerInvariant()
```

Commit that resulting 64-character value as `RIFE_SOURCE_SHA256` in the workflow, verify it immediately after every workflow download, and include it in `SOURCE.txt` and `model-manifest.json`. A changed upstream asset with the same URL must fail before conversion.

- [ ] **Step 4: Carry an explicit model license notice**

Pin the Practical-RIFE license to commit `bbfd2ea90910789a860ea3e2b32a240cd577b75e` and fetch:

```text
https://raw.githubusercontent.com/hzwer/Practical-RIFE/bbfd2ea90910789a860ea3e2b32a240cd577b75e/LICENSE
```

Require SHA-256 `7932fb49341512b959b1744a6d9cbb39e5a1ec89da438a34d0454d5d8df9fecd`, then package the verified bytes as `LICENSES/RIFE-model-license.txt`. Record the pinned commit and license hash in `SOURCE.txt`. Practical-RIFE explicitly states its trained models use the same MIT license as the project, which is the model-license basis for this package.

- [ ] **Step 5: Add `workflow_call` support**

Keep the existing standalone triggers and add `workflow_call:` so `main.yml` can build/download the model artifact in the same release run.

- [ ] **Step 6: Run the lightweight manifest test and workflow YAML parse check**

Run the Python manifest test, then install the same lightweight YAML parser used for this check and parse the workflow:

```powershell
python -m pip install --disable-pip-version-check pyyaml
python -c "import pathlib,yaml; yaml.safe_load(pathlib.Path('.github/workflows/rife-model.yml').read_text(encoding='utf-8')); print('rife-model.yml parsed')"
```

Expected: manifest test passes; YAML parses successfully.

- [ ] **Step 7: Commit model provenance changes**

```bash
git add .github/workflows/rife-model.yml tools/RifeTensorRTRuntime/prepare_model.py tools/RifeRuntimeInstaller/model-manifest.schema.json
git commit -m "build: pin RIFE model provenance for releases"
```

---

### Task 4: Build the transactional RIFE runtime installer and GPU architecture selector

**Files:**
- Modify: `tools/RifeRuntimeInstaller/RifeRuntimeManifest.psm1`
- Create: `tools/RifeRuntimeInstaller/Install-MPCVRRifeRuntime.ps1`
- Create: `tools/RifeRuntimeInstaller/Install-MPCVRRifeRuntime.cmd`
- Create: `tools/RifeRuntimeInstaller/Test-MPCVRRifePreflight.ps1`
- Modify: `tools/RifeRuntimeInstaller/README.md`
- Modify: `.github/workflows/validate-rife-runtime-installer.yml`

**Interfaces:**
- Consumes: common runtime ZIP, one or more architecture ZIPs, model artifact/ZIP, checksum files, `%LOCALAPPDATA%`, and `nvidia-smi` GPU inventory.
- Produces: `%LOCALAPPDATA%\MPCVideoRenderer\RIFE\runtime`, `models\rife_v4.6.onnx`, `cache\`, an installed manifest, and a zero/non-zero preflight result.

- [ ] **Step 1: Write GPU-selection tests with injected inventory**

Add a `-GpuInventoryJson` test seam to the installer. The fixture format is:

```json
[
  {"name":"NVIDIA GeForce RTX 2070 SUPER","computeCapability":"7.5"},
  {"name":"NVIDIA GeForce RTX 5070","computeCapability":"12.0"}
]
```

The test must assert the unique selected packs are `sm75` and `sm120`, and that an injected `9.0` capability exits non-zero with `Unsupported CUDA compute capability: 9.0`.

- [ ] **Step 2: Run the selector tests and verify they fail before installer implementation**

Run the fixture through `Install-MPCVRRifeRuntime.ps1 -ValidateOnly -GpuInventoryJson ...`.

Expected: FAIL because the installer entry point does not exist yet.

- [ ] **Step 3: Implement real GPU inventory detection**

When `-GpuInventoryJson` is absent, invoke:

```powershell
& nvidia-smi.exe --query-gpu=name,compute_cap --format=csv,noheader,nounits
```

Parse every returned NVIDIA GPU. Install the union of required architecture packs so multi-GPU systems remain valid even when MPC-HC later renders on a different NVIDIA adapter. The runtime's D3D11/CUDA interop remains authoritative for the adapter actually used during playback.

- [ ] **Step 4: Implement the transactional staging layout**

Use:

```powershell
$InstallRoot = Join-Path $env:LOCALAPPDATA 'MPCVideoRenderer\RIFE'
$stagingRoot = "$InstallRoot.staging-$([guid]::NewGuid().ToString('N'))"
$backupRoot = "$InstallRoot.backup"
```

Stage common runtime files to `runtime\`, selected builder resources to the same directory, `rife_v4.6.onnx` to `models\`, and create an empty `cache\` directory. Verify every staged file against the supplied SHA-256 manifests before touching the installed tree.

- [ ] **Step 5: Preserve cache only when runtime/model identity remains compatible**

Read the previous installed manifest. If model SHA-256, TensorRT major/minor, and runtime ABI match the new manifest, copy existing `cache\*.plan` into staging. Otherwise create an empty cache. Never move an incompatible plan into a new install.

- [ ] **Step 6: Atomically replace the RIFE tree and roll back on failure**

Follow the existing Maxine installer transaction pattern: remove stale backup, move current install to backup, move staging into place, run preflight, then delete backup. On any exception after moving the old install, remove the failed new tree and move the backup back.

- [ ] **Step 7: Implement preflight checks**

`Test-MPCVRRifePreflight.ps1` must verify:

```text
MPCVRRifeRuntime64.dll exists
cudart64_12.dll exists
nvinfer_11.dll exists
nvonnxparser_11.dll exists
nvinfer_builder_resource_ptx_11.dll exists
each detected GPU has its matching nvinfer_builder_resource_sm*_11.dll
models\rife_v4.6.onnx exists and matches manifest SHA-256
cache directory is writable
MPCVRRifeRuntime64.dll exports MpcvrRifeGetAbiVersion and reports ABI 1
```

Use a tiny bundled PowerShell `Add-Type` P/Invoke helper for `LoadLibraryExW`, `GetProcAddress`, and the ABI call; do not require MPC-HC to launch for preflight.

- [ ] **Step 8: Add idempotence and rollback fixtures**

In `validate-rife-runtime-installer.yml`, install a mock release to a temp root twice and assert all installed hashes are unchanged. Then install a second fixture with a deliberately invalid model hash after the old tree has been moved and assert the original fixture's files/hashes are restored.

- [ ] **Step 9: Run validation with Windows PowerShell 5.1 and PowerShell 7**

Run the installer `-ValidateOnly` and fixture installs under both hosts. Expected: all selection, hash, idempotence, and rollback assertions pass.

- [ ] **Step 10: Commit the RIFE installer**

```bash
git add tools/RifeRuntimeInstaller .github/workflows/validate-rife-runtime-installer.yml
git commit -m "feat: add transactional RIFE runtime installer"
```

---

### Task 5: Make K-Lite renderer replacement transactional and update the restore shortcut

**Files:**
- Create: `tools/KLiteMaxineUpdater/KLiteRendererInstall.psm1`
- Modify: `tools/KLiteMaxineUpdater/Update-KLiteMPCVR.ps1:3-280`
- Modify: `tools/KLiteMaxineUpdater/Install-KLiteMPCVRUpdater.ps1:3-63`
- Modify: `tools/KLiteMaxineUpdater/README.md`
- Modify: `.github/workflows/validate-klite-updater.yml`

**Interfaces:**
- Consumes: renderer ZIP/checksum and the existing x86/x64 K-Lite destinations.
- Produces: transactional renderer replacement with automatic restore on partial failure; desktop shortcut named `Restore MPC-VR Maxine + RIFE`.

- [ ] **Step 1: Add a fixture-level rollback test before refactoring**

Create two temp destination files containing `old-x86`/`old-x64`, a renderer package containing `new-x86`/`new-x64`, and configure the second target to fail after the first target has been replaced. Assert both original destination hashes are restored.

- [ ] **Step 2: Run the rollback fixture and verify the current updater cannot satisfy it**

Expected: FAIL because current `Update-KLiteMPCVR.ps1` copies files directly and has no transaction/rollback seam.

- [ ] **Step 3: Extract transactional replacement into `KLiteRendererInstall.psm1`**

Provide:

```powershell
function Install-KLiteRendererFiles {
    param(
        [Parameter(Mandatory)][hashtable]$Sources,
        [Parameter(Mandatory)][hashtable]$Targets
    )
    # Returns installed-file metadata only after every copy/hash verification succeeds.
}
```

For every target, copy the original to a unique temp backup before replacement, verify source/destination SHA-256 after copy, and restore all touched targets in reverse order if any copy or verification fails.

- [ ] **Step 4: Preserve the production target map exactly**

Keep:

```powershell
$targets = [ordered]@{
    'MpcVideoRenderer.ax'   = 'C:\Program Files (x86)\K-Lite Codec Pack\Filters\MPCVR\MpcVideoRenderer.ax'
    'MpcVideoRenderer64.ax' = 'C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\MPCVR\MpcVideoRenderer64.ax'
}
```

Add a test-only injectable target map parameter or module-level function argument; never redirect production installs away from these locations.

- [ ] **Step 5: Move the updater to the new release identity with legacy fallback**

Primary asset:

```text
MpcVideoRenderer-Maxine-RIFE.zip
```

If the new asset is unavailable when an older shortcut runs, continue supporting the existing `MpcVideoRenderer-Maxine.zip` checksum/download convention. Do not remove the legacy checksum parser.

- [ ] **Step 6: Rename/update the installed restore shortcut**

Install the script under `%LOCALAPPDATA%\MPCVR Custom Updater` and create `Restore MPC-VR Maxine + RIFE.lnk`. Remove the old `Restore MPC-VR Maxine.lnk` only after the new shortcut is successfully created.

- [ ] **Step 7: Run updater validation in both PowerShell hosts**

Parse the two scripts plus `KLiteRendererInstall.psm1` with `[System.Management.Automation.Language.Parser]::ParseFile` under Windows PowerShell 5.1, then run the rollback/idempotence fixture once with `powershell.exe` and once with `pwsh.exe`. Finally run:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File tools/KLiteMaxineUpdater/Update-KLiteMPCVR.ps1 -ValidateOnly -NoPause
pwsh.exe -NoLogo -NoProfile -File tools/KLiteMaxineUpdater/Update-KLiteMPCVR.ps1 -ValidateOnly -NoPause
```

Expected: parsing succeeds, both fixture runs restore the original hashes after injected failure, and both `-ValidateOnly` invocations exit 0.

- [ ] **Step 8: Commit the updater transaction**

```bash
git add tools/KLiteMaxineUpdater .github/workflows/validate-klite-updater.yml
git commit -m "feat: make K-Lite MPCVR updates transactional"
```

---

### Task 6: Turn the existing Maxine setup into the unified Maxine + RIFE setup

**Files:**
- Create: `tools/MaxineSetup/Install-MPCVR-Maxine-RIFE.ps1`
- Create: `tools/MaxineSetup/Install-MPCVR-Maxine-RIFE.cmd`
- Modify: `tools/MaxineSetup/Install-MPCVR-Maxine.ps1`
- Modify: `tools/MaxineSetup/Install-MPCVR-Maxine.cmd`
- Modify: `tools/MaxineSetup/README.md`
- Modify: `.github/workflows/validate-maxine-runtime-installer.yml`
- Modify: `.github/workflows/validate-rife-runtime-installer.yml`

**Interfaces:**
- Consumes: renderer ZIP, Maxine runtime ZIP, RIFE common ZIP, RIFE architecture ZIPs, RIFE model ZIP, manifests/checksums, and the three installer scripts from earlier tasks.
- Produces: one validated setup entry point that installs Maxine, RIFE, restore updater, and the renderer in a deterministic order.

- [ ] **Step 1: Expand the assembled-setup validation fixture first**

Build a mock `payload\` directory containing these exact names:

```text
MpcVideoRenderer-Maxine-RIFE.zip
MPCVR-Maxine-Runtime.zip
MPCVR-RIFE-Common.zip
MPCVR-RIFE-sm75.zip
MPCVR-RIFE-sm86.zip
MPCVR-RIFE-sm89.zip
MPCVR-RIFE-sm120.zip
MPCVR-RIFE-Model-v4.6.zip
RIFE-runtime-manifest.json
PAYLOAD-SHA256SUMS.txt
```

Call `Install-MPCVR-Maxine-RIFE.ps1 -ValidateOnly -NoPause` and expect failure before the unified script exists.

- [ ] **Step 2: Implement the unified setup orchestration**

Retain the current helper functions `Get-ExpectedHash`, `Test-FileHash`, and `Invoke-SetupStep`. Validate all payload hashes before executing any installer. Then run exactly:

```text
1/4 Installing NVIDIA Maxine runtime...
2/4 Installing RIFE 4.6 TensorRT runtime...
3/4 Installing K-Lite restore shortcut...
4/4 Installing custom MPC Video Renderer into K-Lite...
```

Require MPC-HC to be closed before step 1.

- [ ] **Step 3: Keep old setup entry points as compatibility wrappers**

`Install-MPCVR-Maxine.cmd` and `.ps1` should print that the package is now Maxine + RIFE and forward to `Install-MPCVR-Maxine-RIFE.*` when the unified payload is present. This keeps old documentation/bookmarks from dead-ending while the new release uses the new filename directly.

- [ ] **Step 4: Add post-install preflight**

After renderer installation, run `Test-MPCVRRifePreflight.ps1` and verify the Maxine runtime's five required DLLs plus `NV_VIDEO_EFFECTS_PATH`. If preflight fails, the unified setup exits non-zero; component installers are responsible for their own transactional rollback.

- [ ] **Step 5: Print the first-launch verification block**

On success print:

```text
Open MPC-HC and play a video.
Press Ctrl+J and confirm:
  Maxine runtime: <installed path>
  RIFE runtime: ready
  RIFE model: 4.6
First playback may build a TensorRT engine under %LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache.
```

- [ ] **Step 6: Make both installer validation workflows pass**

Run PowerShell 5.1 parsing, PowerShell 7 parsing, `-ValidateOnly`, mock assembly, and hash-failure cases. The unified setup must fail before mutation when any declared payload/checksum is missing.

- [ ] **Step 7: Commit unified setup**

```bash
git add tools/MaxineSetup .github/workflows/validate-maxine-runtime-installer.yml .github/workflows/validate-rife-runtime-installer.yml
git commit -m "feat: unify Maxine and RIFE setup"
```

---

### Task 7: Assemble verified release assets from reusable renderer/runtime/model jobs

**Files:**
- Modify: `.github/workflows/main.yml:19-205`
- Modify: `.github/workflows/rife-runtime.yml`
- Modify: `.github/workflows/rife-model.yml`
- Create: `tools/MaxineSetup/THIRD-PARTY-NOTICES.txt`
- Modify: `tools/MaxineSetup/README.md`

**Interfaces:**
- Consumes: Actions artifacts `MPC Video Renderer`, `mpcvr-rife-runtime-bundle`, `mpcvr-rife-v4.6-model`, plus the verified carried Maxine runtime ZIP.
- Produces: `MPCVR-Maxine-RIFE-Setup.zip`, `MpcVideoRenderer-Maxine-RIFE.zip`, `SHA256SUMS.txt`, and temporary compatibility aliases for the previous Maxine-only renderer/setup asset names.

- [ ] **Step 1: Add reusable runtime/model jobs to `main.yml` without publishing yet**

Define jobs:

```yaml
  rife-runtime:
    uses: ./.github/workflows/rife-runtime.yml

  rife-model:
    uses: ./.github/workflows/rife-model.yml
```

Make the publish job depend on `build`, `rife-runtime`, and `rife-model`. Keep publication restricted to `master` push/workflow-dispatch as it is today.

- [ ] **Step 2: Download and verify all three build artifacts in the publish job**

Use `actions/download-artifact@v8` with fixed artifact names into separate release subdirectories. Verify each runtime/model internal `SHA256SUMS.txt` before assembling anything.

- [ ] **Step 3: Keep Maxine migration behavior but rename the public stack**

Continue extracting the verified Maxine runtime from the previous latest unified setup or legacy standalone Maxine runtime asset. Rename the new renderer artifact to `MpcVideoRenderer-Maxine-RIFE.zip`.

- [ ] **Step 4: Assemble the setup payload**

Copy the unified setup scripts plus:

```text
payload/Install-MPCVRMaxineRuntime.ps1
payload/Install-MPCVRRifeRuntime.ps1
payload/Test-MPCVRRifePreflight.ps1
payload/Install-KLiteMPCVRUpdater.ps1
payload/Update-KLiteMPCVR.ps1
payload/MpcVideoRenderer-Maxine-RIFE.zip
payload/MPCVR-Maxine-Runtime.zip
payload/MPCVR-RIFE-Common.zip
payload/MPCVR-RIFE-sm75.zip
payload/MPCVR-RIFE-sm86.zip
payload/MPCVR-RIFE-sm89.zip
payload/MPCVR-RIFE-sm120.zip
payload/MPCVR-RIFE-Model-v4.6.zip
payload/RIFE-runtime-manifest.json
payload/THIRD-PARTY-NOTICES.txt
payload/PAYLOAD-SHA256SUMS.txt
```

Then create `MPCVR-Maxine-RIFE-Setup.zip`.

- [ ] **Step 5: Add a hard license/notice gate before `gh release create`**

Require the assembled payload to include:

```text
THIRD-PARTY-NOTICES.txt
Maxine runtime license/notice file(s) carried by the existing exporter
TensorRT/CUDA license/notice file(s) carried by the runtime workflow
LICENSES/RIFE-model-license.txt
```

If any required notice is absent, fail the publish job before release creation. `THIRD-PARTY-NOTICES.txt` must identify CUDA, TensorRT, NVIDIA Maxine, the RIFE model source, versions/source URLs, and the corresponding bundled notice filenames; it must not invent or paraphrase license grants.

- [ ] **Step 6: Keep one-release compatibility aliases**

Publish byte-identical aliases:

```text
MpcVideoRenderer-Maxine.zip -> MpcVideoRenderer-Maxine-RIFE.zip
MPCVR-Maxine-Setup.zip      -> MPCVR-Maxine-RIFE-Setup.zip
```

Keep `MpcVideoRenderer-Maxine.zip.sha256` so already-installed updater shortcuts continue functioning. New scripts/readmes use the Maxine + RIFE names.

- [ ] **Step 7: Add an ancestry assertion before release publication**

Before assembly, verify the publishing `master` commit contains the already-tested RIFE/Maxine runtime baseline. Set:

```yaml
env:
  RIFE_MINIMUM_COMMIT: 4f76664a79510555f24d5c7f5d8a688ad1030238
```

and run:

```bash
git merge-base --is-ancestor "$RIFE_MINIMUM_COMMIT" "$GITHUB_SHA"
```

Fail publication if the release commit does not include the integrated RIFE/Maxine work.

- [ ] **Step 8: Update release notes and checksums**

Release notes must name Maxine VSR + RIFE 4.6 TensorRT and list Turing/Ampere/Ada/Blackwell desktop support by compute capability. `SHA256SUMS.txt` covers every public ZIP including compatibility aliases.

- [ ] **Step 9: Validate workflow syntax and dry-run assembly without `gh release create`**

Run the assembly commands against fixture artifacts in a temp directory and assert the final ZIP contains every path from Step 4 and that every top-level SHA-256 verifies.

- [ ] **Step 10: Commit release assembly**

```bash
git add .github/workflows/main.yml .github/workflows/rife-runtime.yml .github/workflows/rife-model.yml tools/MaxineSetup
git commit -m "build: assemble Maxine RIFE release package"
```

---

### Task 8: Run the software release gate and produce the RTX 5070 test installer

**Files:**
- Modify only if verification exposes a defect in earlier tasks.
- Do not add one-off workflows.

**Interfaces:**
- Consumes: completed Tasks 1-7 on `feature/rife-tensorrt-interpolation`.
- Produces: a checksum-verified release-candidate setup artifact suitable for the RTX 5070 machine, without publishing a public GitHub release yet.

- [ ] **Step 1: Run all local deterministic tests**

Run:

```cmd
tools\rife-runtime-abi-test\build-test.cmd
```

Run the existing scheduler/lifecycle tests and the new PowerShell validation scripts under both Windows PowerShell 5.1 and PowerShell 7. Expected: zero failures.

- [ ] **Step 2: Build the full renderer**

Run:

```powershell
.\build_mpcvr.cmd NoWait
```

Expected: x86/x64 renderer build succeeds and produces `_bin\MpcVideoRenderer*.zip`.

- [ ] **Step 3: Run durable CI workflows on the feature branch**

Dispatch the existing durable workflows only:

```powershell
gh workflow run rife-runtime.yml --ref feature/rife-tensorrt-interpolation
gh workflow run rife-model.yml --ref feature/rife-tensorrt-interpolation
gh workflow run validate-rife-runtime-installer.yml --ref feature/rife-tensorrt-interpolation
gh workflow run validate-maxine-runtime-installer.yml --ref feature/rife-tensorrt-interpolation
gh workflow run validate-klite-updater.yml --ref feature/rife-tensorrt-interpolation
```

Wait for each run and require a successful conclusion. Do not call a queued/running workflow verified.

- [ ] **Step 4: Inspect the runtime artifact for Blackwell files**

Download the `mpcvr-rife-runtime-bundle` artifact and verify it contains:

```text
MPCVR-RIFE-Common.zip
MPCVR-RIFE-sm75.zip
MPCVR-RIFE-sm86.zip
MPCVR-RIFE-sm89.zip
MPCVR-RIFE-sm120.zip
runtime-manifest.json
SHA256SUMS.txt
```

Expand `MPCVR-RIFE-sm120.zip` and require `nvinfer_builder_resource_sm120_11.dll`.

- [ ] **Step 5: Assemble the release candidate from verified artifacts**

Use the same assembly script/path as `main.yml`, with publishing disabled, to create `MPCVR-Maxine-RIFE-Setup.zip`. Verify its top-level SHA-256 and payload SHA-256 list.

- [ ] **Step 6: Re-test the release candidate on the RTX 2070 SUPER**

Install over the current K-Lite custom build, then verify the already-established runtime behavior:

```text
30 -> 60 generation
60 -> 120 where throughput permits
pause/resume
seek
file switching
Maxine + RIFE playback
first engine build
cached engine reuse after restart
```

Record Ctrl+J RIFE/Maxine lines and confirm there are no new freezes, persistent runtime-wait, or interop errors.

- [ ] **Step 7: Give the same release-candidate setup to the RTX 5070 test machine**

Starting from its existing K-Lite/MPC-HC installation (including any old Marc custom renderer), run the one-click installer. Require installer/preflight output to report:

```text
GPU: NVIDIA GeForce RTX 5070
CUDA compute capability: 12.0
RIFE architecture pack: sm120
TensorRT builder resource: present
RIFE runtime ABI: 1
RIFE model: rife_v4.6.onnx verified
```

- [ ] **Step 8: Validate Blackwell playback and local plan creation**

On the 5070, verify 30 -> 60 and 60 -> 120, Maxine + RIFE, pause/resume, seek, file switching, first-use engine build, and cached startup. Confirm `%LOCALAPPDATA%\MPCVideoRenderer\RIFE\cache` gains a `.plan` whose filename includes the `cc120`/5070 cache identity, then confirm the next playback reuses it instead of rebuilding.

- [ ] **Step 9: Stop before public publication if either physical-machine gate fails**

Any Turing regression or Blackwell install/runtime failure returns to the task that owns the failing layer. Do not relax the architecture gate, checksum gate, or rollback assertions to make the release pass.

---

### Task 9: Integrate to fork main and publish the first NVIDIA RIFE release

**Files:**
- Git history / release metadata only unless Task 8 exposed a defect.

**Interfaces:**
- Consumes: a fully verified feature head and successful RTX 2070 SUPER + RTX 5070 release-candidate results.
- Produces: fork `master` containing the RIFE feature and a public immutable Maxine + RIFE release generated by the durable main workflow.

- [ ] **Step 1: Verify branch ancestry and intended diff before integration**

Fetch `origin` and `upstream`, then verify:

```bash
git merge-base origin/master feature/rife-tensorrt-interpolation
git diff --check origin/master...feature/rife-tensorrt-interpolation
git log --oneline origin/master..feature/rife-tensorrt-interpolation
```

Confirm the branch still descends from the current fork main and no generated/untracked artifacts are staged.

- [ ] **Step 2: Integrate the reviewed feature history into `master` with a fast-forward when possible**

After fetching, require `origin/master` to be an ancestor of the tested feature head:

```bash
git merge-base --is-ancestor origin/master feature/rife-tensorrt-interpolation
```

If that returns 0, update local `master` and use:

```bash
git switch master
git merge --ff-only feature/rife-tensorrt-interpolation
```

If it returns non-zero because fork main advanced during implementation, merge the new `origin/master` into the feature branch first, rerun Task 8's complete deterministic + hardware release gate, and only then fast-forward `master`. Do not rebase or rewrite the tested feature commits.

- [ ] **Step 3: Watch the master build/publish run to completion**

Require the renderer build, reusable RIFE runtime/model jobs, installer validators, release assembly, and immutable release publication to all conclude successfully.

- [ ] **Step 4: Verify the published release assets**

Download the published `MPCVR-Maxine-RIFE-Setup.zip` and `SHA256SUMS.txt`, verify the checksum locally, and inspect the ZIP for the SM120 pack, RIFE 4.6 model, Maxine runtime, unified installer, restore updater, and required license notices.

- [ ] **Step 5: Verify the latest-release updater path**

Run `Update-KLiteMPCVR.ps1 -ValidateOnly -NoPause`, confirm it resolves the new Maxine + RIFE renderer identity, and confirm the compatibility alias remains downloadable for older installed shortcuts.

- [ ] **Step 6: Record the shipped release boundary**

Update the implementation/release notes with the final tag, master commit SHA, runtime artifact checksum, model checksum, installer checksum, and representative hardware validation results. Do not start ncnn/Vulkan or additional RIFE model work in this release branch; that is the next design milestone.
