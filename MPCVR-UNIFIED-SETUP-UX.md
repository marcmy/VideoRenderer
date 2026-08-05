# MPCVR Unified Setup UX

## Normal installation

1. Run `MPCVR-Unified-Setup.cmd`.
2. Review the short system/component status summary.
3. Click **Install / Update**.
4. Complete NVIDIA sign-in/license acceptance only when the Optical Flow SDK is not already available.
5. Setup installs, verifies, and keeps a rollback point.

## Main-window rule

The main window must not ask the user to choose:

- Automatic, Guided, or Advanced modes
- a calibration video
- a profile name
- a recommendation priority
- a 30 -> 60 or 60 -> 120 workload class

Those are implementation and tuning concerns, not installation prerequisites.

## Advanced access

A secondary **Advanced...** dialog may expose installer-specific overrides, telemetry, rollback, and access to technical tools. Renderer settings remain fully customizable through MPC Video Renderer's property pages and the packaged command-line tooling.
