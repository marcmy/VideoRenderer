# MPC-VR RIFE TensorRT runtime bundle

The release runtime is split into one common archive plus one TensorRT builder-resource pack per supported NVIDIA compute capability. The installer can therefore install the shared runtime once and add only the builder resource that matches the detected GPU.

The shipping bundle contains:

- `MPCVR-RIFE-Common.zip`
- `MPCVR-RIFE-sm75.zip` for compute capability 7.5 (Turing)
- `MPCVR-RIFE-sm86.zip` for compute capability 8.6 (Ampere)
- `MPCVR-RIFE-sm89.zip` for compute capability 8.9 (Ada)
- `MPCVR-RIFE-sm120.zip` for compute capability 12.0 (Blackwell)
- `runtime-manifest.json`
- `SHA256SUMS.txt`
- build provenance plus NVIDIA license/notice files carried from the pinned SDK archives when present

`MPCVR-RIFE-Common.zip` contains exactly:

```text
MPCVRRifeRuntime64.dll
nvinfer_11.dll
nvonnxparser_11.dll
nvinfer_builder_resource_ptx_11.dll
BUILD-INFO.txt
```

Each architecture archive contains exactly one matching `nvinfer_builder_resource_<architecture>_11.dll`. TensorRT engines are generated locally on the target machine; prebuilt `.plan` engines are not distributed.

The release stack is pinned to CUDA toolkit 13.4.1, CUDA runtime 13.4.49, and TensorRT 11.3.0.99. CUDA 13 links the runtime through the NVIDIA display driver's hybrid CUDA loader, so the common archive does not carry a standalone `cudart64_13.dll`. `runtime-manifest.json` records the version contract and runtime ABI 2.

Validate an assembled bundle with either Windows PowerShell 5.1 or PowerShell 7:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File `
  tools\RifeRuntimeInstaller\Test-RifeRuntimePackage.ps1 -BundleRoot <bundle-directory>

pwsh.exe -NoLogo -NoProfile -File `
  tools\RifeRuntimeInstaller\Test-RifeRuntimePackage.ps1 -BundleRoot <bundle-directory>
```

The validator rejects missing architecture packs, manifest drift, unexpected ZIP contents, missing checksums, extra checksum entries, and checksum mismatches.

## Installing the runtime

`Install-MPCVRRifeRuntime.cmd` launches the PowerShell 5.1 installer. The installer detects every NVIDIA GPU with `nvidia-smi`, maps each compute capability to a supported architecture pack, verifies the selected runtime archives and the model package, stages a complete replacement tree, runs native ABI preflight, and only then removes the previous backup.

The default install root is:

```text
%LOCALAPPDATA%\MPCVideoRenderer\RIFE
```

The installed tree is:

```text
runtime\
  MPCVRRifeRuntime64.dll
  nvinfer_11.dll
  nvonnxparser_11.dll
  nvinfer_builder_resource_ptx_11.dll
  nvinfer_builder_resource_sm*_11.dll
models\
  rife_v4.4.onnx
  rife_v4.6.onnx
  rife_v4.15_lite.onnx
cache\
installed-manifest.json
```

For a local release payload, run:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File `
  .\Install-MPCVRRifeRuntime.ps1 `
  -RuntimeBundleRoot .\payload `
  -ModelArchive .\payload\MPCVR-RIFE-Models.zip `
  -NoPause
```

The installer verifies archive/model hashes before moving the existing install. Existing TensorRT engine plans are preserved across runtime upgrades because every cache filename is isolated by TensorRT major/minor, runtime ABI, model SHA-256, GPU, dimensions, and builder mode. Incompatible plans remain available for rollback but cannot be selected by a different runtime key. Upgrading from TensorRT 11.2 to 11.3 therefore requires a one-time 11.3 engine build for each selected model and builder mode. Any post-swap preflight failure removes the failed tree and restores the previous install.

`Test-MPCVRRifePreflight.ps1` checks the common runtime DLLs, every builder resource required by the detected GPUs, the installed model hash, cache writability, and the native `MpcvrRifeGetAbiVersion` export. The expected ABI is 2.

For CI or diagnostics, `-GpuInventoryJson` accepts either raw JSON or a JSON file containing entries such as:

```json
[
  {"name":"NVIDIA GeForce RTX 2070 SUPER","computeCapability":"7.5"},
  {"name":"NVIDIA GeForce RTX 5070","computeCapability":"12.0"}
]
```

`-ValidateOnly` performs GPU-to-pack selection without requiring a release payload. Unsupported capabilities fail with an explicit `Unsupported CUDA compute capability: <major.minor>` message.
