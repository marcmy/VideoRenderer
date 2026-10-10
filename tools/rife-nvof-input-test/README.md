# NVOF input preparation correctness

Run `build-test.cmd` from a Visual Studio Build Tools x64 environment on Windows.
It builds the actual production `Source/NvofAnalysisInput.h` component and uses
CPU-only D3D11 WARP. No NVOF driver, NVIDIA GPU, TensorRT, model/cache or player
is invoked. Pixel results are compared with an independent weighted-area
reference, including padded/odd content, portraits, resource reuse and state
restoration. This verifies input preparation, not optical-flow scene accuracy
or hardware throughput.
