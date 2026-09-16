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
cudart64_12.dll
nvinfer_11.dll
nvonnxparser_11.dll
nvinfer_builder_resource_ptx_11.dll
BUILD-INFO.txt
```

Each architecture archive contains exactly one matching `nvinfer_builder_resource_<architecture>_11.dll`. TensorRT engines are generated locally on the target machine; prebuilt `.plan` engines are not distributed.

The release stack is pinned to CUDA toolkit 12.9.1, CUDA runtime 12.9.79, and TensorRT 11.2.1.2. `runtime-manifest.json` records that contract and runtime ABI 1.

Validate an assembled bundle with either Windows PowerShell 5.1 or PowerShell 7:

```powershell
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File `
  tools\RifeRuntimeInstaller\Test-RifeRuntimePackage.ps1 -BundleRoot <bundle-directory>

pwsh.exe -NoLogo -NoProfile -File `
  tools\RifeRuntimeInstaller\Test-RifeRuntimePackage.ps1 -BundleRoot <bundle-directory>
```

The validator rejects missing architecture packs, manifest drift, unexpected ZIP contents, missing checksums, extra checksum entries, and checksum mismatches.
