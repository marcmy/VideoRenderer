![MPC Video Renderer](docs/Logo.svg)

# MPC Video Renderer

MPC Video Renderer is a DirectShow video renderer based on Direct3D 11. This fork adds NVIDIA Maxine video enhancement and NVIDIA NvOFFRUC frame interpolation.

## Unified enhanced setup

The test installer for the custom renderer stack is under development in PR #24.

Normal installation is intentionally simple:

1. Extract the unified setup ZIP.
2. Close MPC-HC.
3. Run `MPCVR-Unified-Setup.cmd`.
4. Click **Install / Update**.

Setup installs or updates:

- MPC Video Renderer
- NVIDIA Maxine runtime
- NVIDIA frame-interpolation runtime

When NVIDIA's Optical Flow SDK is required, setup opens the official NVIDIA page and detects the completed download automatically after the user signs in and accepts NVIDIA's license.

Advanced installer controls, telemetry, rollback, calibration, and profile tools remain available without cluttering the normal install screen.

## Project documentation

See the `docs` directory and `tools/UnifiedSetup/README.md` for development, build, runtime, calibration, and installer details.

## License

See [LICENSE](LICENSE).
