# SDR-to-SDR perceptual enhancement (prototype)

An optional global tone curve and vibrance pass for SDR video on SDR displays.
Direct3D 11 only, off by default. HDR passthrough, HDR-to-SDR conversion and
RTX Video HDR are not changed: the step never exists for those sources.

## Where it runs

The D3D11 path for one source image is:

1. Colour conversion: D3D11 video processor (optionally RTX VSR) or the
   shader conversion pass, into `m_TexConvertOutput`.
2. Maxine VSR/denoise/deblur (`ApplyMaxine`), when enabled.
3. Resize into the post-scale chain.
4. Post-scale chain: colour correction (`m_pPSCorrection`), HDR tone mapping,
   **SDR enhancement**, user post-resize shaders, half-OU, dithering (`FinalPass`).
5. `Render()`: subtitles, statistics, OSD bitmap and `Present`.

The enhancement is one step of that chain (`SdrEnhancePass`), after every
colour-management step and before user shaders and dithering.

With frame interpolation the same chain runs once per source frame inside
RIFE source preparation (`PrepareRifeSource` → `Process(..., true)`) and
for NvOFFRUC input. Interpolated and original RIFE frames are then
presented by the prepared branch of `Process()`, which only does Maxine or
RTX VSR and scaling and never enters the post-scale chain. So the pass:

- runs exactly once in the lineage of every presented frame, and never twice;
- costs once per source frame at the RIFE working size (720p short edge by
  default), whatever the output rate;
- precedes Maxine/RTX VSR in RIFE mode but follows them in normal playback.
  For a per-pixel operation this order has little effect; a later spatial
  stage would need to revisit it.

The alternative was enhancing after interpolation, per presented frame at
output size. It keeps RIFE's input untouched, but costs up to 10–20 times
more (for example 1440p at 144 Hz instead of 720p at 24 Hz). It would also need its
own cache next to the prepared-Maxine cache so repeated presentations of a
surface are not processed again. Pointwise colour operations commute exactly
with RIFE's warping and differ from enhance-after-blend only by a
second-order term in blended regions.

## Colour handling

- Input is display-referred BT.709 R′G′B′ from UNORM/float views (never
  sRGB views). It is decoded with gamma 2.2 once and encoded once.
- Tone curve: a monotonic S-curve on OKLab lightness with black, white and the
  pivot fixed; slopes stay within [0.5, 1.5]. It is applied by scaling linear
  light by `(L′/L)³`, which in OKLab sets lightness to exactly `L′` and scales
  chroma with it (hue and saturation kept). This is also exact for neutral
  settings; a full OKLab round trip in float32 is not (half an 8-bit code
  near black after encoding).
- Vibrance scales OKLab chroma, favouring muted colours, sparing near-neutral
  pixels, and optionally skin hues. It is applied as a linear delta so the
  conversion rounding cancels.
- Gamut: out-of-range colours lose OKLab chroma at constant lightness and hue
  (8-step bisection) instead of having channels clipped.
- Precision: while active, the post-scale textures are 16-bit float and the
  dithering pass is enabled for 8-bit and 10-bit swap chains.

## Controls

Main page → **SDR enhancement...**: Off, On, or **A/B split** (original left
of a grey divider, enhanced right; the split position is in render-target
pixels, so it stays centred before RIFE and after scaling). Contrast −50..50%,
pivot 5..95% lightness, vibrance −50..100%, skin protection 0..100%;
the defaults are 15%, 20%, 25% and 50%, chosen in playback. Ctrl+J
shows the state, processed size, whether it ran before interpolation, and the
measured GPU time of the pass (timestamp queries, read without stalling).

## Measurements

`tools/sdr-enhance-test/build-test.cmd` checks the production HLSL on WARP:
neutral identity (every 8-bit code), tone-curve monotonicity and fixed points,
gamut and hue preservation, vibrance weighting and A/B exactness.
`build-test.cmd --bench` times the pass on the hardware adapter.

RTX 2070 SUPER, default settings, 16F → 16F, median of 7×400 draws:

| Size      | Copy pass | Enhancement, natural content | Enhancement, random noise (worst case) |
|-----------|-----------|------------------------------|----------------------------------------|
| 1280×720  | 0.024 ms  | 0.043 ms                     | 0.067 ms                               |
| 1920×1080 | 0.045 ms  | 0.076 ms                     | 0.152 ms                               |
| 2560×1440 | 0.081 ms  | 0.147 ms                     | 0.270 ms                               |
| 3840×2160 | 0.167 ms  | 0.305 ms                     | 0.631 ms                               |

In playback, Ctrl+J reported 0.063 ms for 1920×1080 per frame (A/B split, so
only half the image was enhanced).

Settings: the renderer loads its saved settings, including these, before it
creates the video processor, so every new file starts with them.

Not implemented yet, pending validation of this prototype: multipass local
contrast and scene-adaptive (histogram) processing.
