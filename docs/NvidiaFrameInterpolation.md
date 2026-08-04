# NVIDIA frame interpolation

This draft introduces a dedicated frame-interpolation settings dialog and persistent configuration for a future NvFRUC-backed playback path.

The first engine milestone is exact 2x conversion: one generated midpoint frame between consecutive source frames. It must buffer one future frame, preserve timestamps, reset cleanly on seeks and media-type changes, and fall back to an original frame when interpolation quality is insufficient.

The NVIDIA Optical Flow SDK and NvFRUC runtime are governed by NVIDIA's SDK license. Do not commit or publish NVIDIA headers, sample source, import libraries, or runtime binaries until redistribution terms have been reviewed for compatibility with this GPL repository. The renderer integration should remain optional and dynamically load the external runtime.
