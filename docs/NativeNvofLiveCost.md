# Native NVOF live cost output

Hardware cost output is disabled in the live renderer path.

The real-frame diagnostic capture established that the RTX 2070 SUPER exposes `R8_UINT` cost output, but enabling forward and backward cost generation on every NVOF execution correlated with severe startup/seek stalls and clock catch-up playback. Cost surfaces should therefore be generated only by dedicated diagnostic builds when explicitly required.
