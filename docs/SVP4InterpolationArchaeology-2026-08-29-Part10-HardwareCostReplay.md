# SVP4 interpolation archaeology — Part 10: hardware-cost replay

Date: 2026-08-29

## Why this exists

The remaining major mismatch between the clean-room V1.2 classifier and SVP/open-svpflow is NVIDIA's native Optical Flow output-cost signal.

`open-svpflow` requests NVOF output cost and packs that value into its vector score. The current VideoRenderer capture corpus contains source frames and forward/backward flow, but no hardware cost, so an exact SVP-like field classifier cannot be reconstructed from those captures alone.

## Do not re-enable cost in normal playback

VideoRenderer already contains complete forward/backward cost-surface and execute-output plumbing, but live cost is deliberately disabled:

```cpp
costCaptureEnabled = false;
init.enableOutputCost = nvof::False;
```

This is not a theoretical precaution. Repository history contains commit:

- `b1be648b6c7ac10fe1890a0016049d2f91b48f8c`
- **Disable live NVOF cost output to prevent stalls**

The accompanying source comment records severe Turing startup/seek and sustained-playback stalls when output cost was enabled.

NVIDIA's own programming guide also makes the relevant constraints explicit:

- output cost is selected through `NV_OF_INIT_PARAMS::enableOutputCost`, therefore it is a session-initialization property rather than a per-Execute switch;
- higher cost means less reliable flow;
- 8-bit cost is recommended;
- NVOF API contexts are not thread-safe;
- the SLOW preset can increase graphics/CUDA engine utilization.

Therefore hardware cost must remain outside the ordinary MPC-HC playback session unless future evidence proves a safe architecture.

## Chosen architecture: standalone offline replay

Rather than create a second NVOF context inside MPC-HC, use a separate x64 process modeled after the existing `NativeNvofFlowTest.cpp` probe.

New source:

- `tools/native-nvof-probe/NativeNvofCostReplay.cpp`

Launcher:

- `tools/native-nvof-probe/Run-NvofCostReplay.cmd`

Build workflow:

- `.github/workflows/nvof-cost-replay-tool.yml`

The tool accepts an existing capture directory containing:

- `frame-A.bmp`
- `frame-B.bmp`

It creates its own NVIDIA D3D11 device/context and NVOF API 5.0 session and initializes that session with:

- BGRA8 input
- 4x4 output grid
- `PerfSlow`
- bidirectional prediction
- disabled temporal hints
- 8-bit output cost enabled

The execute orientation intentionally matches the renderer capture naming:

- input = frame B
- reference = frame A
- forward output = B -> A
- backward output = A -> B

## Replay output

A new timestamped child directory is created under the selected capture:

`nvof-cost-replay-YYYYMMDD-HHMMSS`

It contains:

- `flow-forward-B-to-A-s10.5.bin`
- `flow-backward-A-to-B-s10.5.bin`
- `cost-forward-B-to-A-r8.bin`
- `cost-backward-A-to-B-r8.bin`
- `cost-forward-B-to-A.bmp`
- `cost-backward-A-to-B.bmp`
- `replay-summary.txt`

The utility deliberately performs blocking readback because this is an offline one-pair diagnostic, not a real-time path.

## Critical validity check: replay flow versus captured flow

A cost map from a fresh NVOF session is only useful for calibrating the captured corpus if that session reproduces approximately the same flow solution.

When the original capture flow binaries are present, `NativeNvofCostReplay` compares replayed S10.5 vectors against them and reports, per direction:

- mean absolute component difference in pixels
- p99 absolute component difference in pixels

This is the first gate before using replayed costs for SVP-threshold calibration.

### Interpretation

If replay flow is close to captured flow:

- replayed R8 cost is a plausible missing confidence signal for the existing corpus;
- proceed to correlation studies between hardware cost, clean-room q, forward/backward consistency, catastrophic masks, and known visual failures;
- reconstruct SVP-style adjusted-score/field-class behavior using real hardware cost.

If replay flow differs materially:

- do **not** pretend the cost belongs to the original captured flow;
- investigate why session replay differs (driver state, temporal history, input representation, session warmup, or another initialization detail);
- either reproduce the live execution state more exactly or generate a new paired corpus in the standalone tool where flow and cost are guaranteed to come from the same Execute.

## Relationship to Build #2

Build #2 (`research/nvof-adaptive-splat-v12`) does not depend on hardware cost. It intentionally uses a clean-room q/error classifier and keeps the frozen golden renderer as the primary hypothesis.

Hardware-cost replay is therefore a parallel research track for a later iteration, not a prerequisite for testing V1.2.

## Next experiments after first real replay

1. Run a severe Boromir capture and one clean/control capture through the offline tool.
2. Verify replay-flow similarity before interpreting cost.
3. Measure R8 cost distributions by:
   - golden valid/invalid cell state;
   - catastrophic consistency state;
   - V1.2 q bands;
   - projected support/ownership bands.
4. Decode whether the open-svpflow/SVP score is raw cost, scaled cost, or a transformed composite by comparing observed ranges against the recovered `zero=200`, `m1=1600`, `m2=2800`, `scene=4000` classifier thresholds and its block-area scaling.
5. Only after that decide whether Build #3 should consume hardware cost, improve the clean-room q proxy, or leave hardware cost diagnostic-only.
