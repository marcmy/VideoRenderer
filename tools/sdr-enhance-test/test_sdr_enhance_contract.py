from pathlib import Path
import re


repo = Path(__file__).resolve().parents[2]

processor = (repo / "Source" / "DX11VideoProcessor.cpp").read_text(encoding="utf-8")
header = (repo / "Source" / "DX11VideoProcessor.h").read_text(encoding="utf-8")
settings = (repo / "Source" / "IVideoRenderer.h").read_text(encoding="utf-8")
shader = (repo / "Shaders" / "d3d11" / "ps_sdr_enhance.hlsl").read_text(encoding="utf-8")


def body(source, start, end):
    first = source.find(start)
    assert first != -1, f"{start!r} was not found"
    last = source.find(end, first)
    assert last != -1, f"end of {start!r} was not found"
    return source[first:last]


# One call site: the enhancement is a post-scale step, nothing else runs it.
assert processor.count("SdrEnhancePass(*") == 1, (
    "the SDR enhancement must have exactly one call site in the post-scale chain"
)

process = body(processor, "HRESULT CDX11VideoProcessor::Process(", "\nvoid CDX11VideoProcessor::SetVideoRect")
prepared = body(process, "if (m_pFrameInterpolationTexture && m_pFrameInterpolationView)", "\n\tHRESULT hr = S_OK;")
assert "SdrEnhance" not in prepared, (
    "interpolated/prepared presentation frames were already enhanced as RIFE/NvOFFRUC input; "
    "the presentation branch must not apply the enhancement again"
)

# Order inside the chain: colour correction and HDR tone mapping, then the
# enhancement, then user shaders, half-OU subtitles and the dithering pass.
chain = body(process, "if (m_pPSCorrection) {", "\n\telse {\n\t\thr = ResizeShaderPass")
positions = [chain.find(marker) for marker in (
    "if (m_pPSCorrection)", "if (m_pPSHDR10ToneMapping)", "if (SdrEnhanceActive())",
    "if (m_pPostScaleShaders.size())", "if (m_pPSHalfOUtoInterlace)", "if (m_bFinalPass)")]
assert -1 not in positions and positions == sorted(positions), (
    "the enhancement must follow colour correction/tone mapping and precede user shaders and dithering"
)
assert re.search(r"if \(SdrEnhanceActive\(\)\) \{\s*StepSetting\(\);", chain), (
    "the enhancement must consume one post-scale step like every other chain pass"
)

steps = body(processor, "UINT CDX11VideoProcessor::GetPostScaleSteps()", "\nHRESULT CDX11VideoProcessor::CreatePShaderFromResource")
assert "SdrEnhanceActive()" in steps, "the post-scale step count must include the enhancement"

# HDR passthrough, HDR-to-SDR conversion and RTX Video HDR stay untouched.
active = re.search(r"bool SdrEnhanceActive\(\) const \{([^}]*)\}", header)
assert active and "!SourceIsHDR()" in active.group(1) and "!m_bVPUseRTXVideoHDR" in active.group(1), (
    "the enhancement must be inactive for HDR sources and RTX Video HDR"
)
update = body(processor, "void CDX11VideoProcessor::UpdateSdrEnhanceShader()", "\nHRESULT CDX11VideoProcessor::SdrEnhancePass")
assert "SDR_ENHANCE_Off" in update and "SourceIsHDR()" in update and "m_bVPUseRTXVideoHDR" in update, (
    "the shader must only exist for enabled SDR sources"
)

# Precision: a float ring and dithering while active, unchanged otherwise.
post_scale = body(processor, "void CDX11VideoProcessor::UpdatePostScaleTexures()", "\nvoid CDX11VideoProcessor::UpdateUpscalingShaders")
assert "GetPostScaleTexFormat(m_InternalTexFmt)" in post_scale and "postScaleFmt != DXGI_FORMAT_B8G8R8A8_UNORM" in post_scale, (
    "the presentation chain must use the enhancement-aware format for textures and dithering"
)
assert "GetPostScaleTexFormat(GetRifeSurfaceFormat())" in process, (
    "RIFE source preparation must also process the enhancement at float precision"
)
assert "SdrEnhanceActive() ? DXGI_FORMAT_R16G16B16A16_FLOAT : format" in header, (
    "the post-scale format must be unchanged when the enhancement is off"
)

# Settings: appended after the existing layout and off by default.
layout = body(settings, "bool bRifeFeatureReuse;", "Settings_t() {")
assert "int iSdrEnhanceMode;" in layout, "SDR enhancement settings must be appended to Settings_t"
assert "iSdrEnhanceMode                 = SDR_ENHANCE_Off;" in settings, "the enhancement must default to Off"

# A new renderer (every new file) must pass the saved settings to the processor
# it creates; they are not applied again before the first frame is processed.
renderer = (repo / "Source" / "VideoRenderer.cpp").read_text(encoding="utf-8")
constructor = body(renderer, "CMpcVideoRenderer::CMpcVideoRenderer(", "\nCMpcVideoRenderer::~CMpcVideoRenderer")
loaded = constructor.find("EnsureRifeSettingsLoaded(this, m_Sets);")
created = constructor.find("new CDX11VideoProcessor(this, m_Sets")
assert loaded != -1 and created != -1 and loaded < created, (
    "SDR enhancement settings must be loaded before the video processor is created"
)
processor_ctor = body(processor, "CDX11VideoProcessor::CDX11VideoProcessor(", "\nCDX11VideoProcessor::~CDX11VideoProcessor")
for field in ("Mode", "Contrast", "Pivot", "Vibrance", "SkinProtection"):
    assert f"m_iSdrEnhance{field}" in processor_ctor and f"config.iSdrEnhance{field}" in processor_ctor, (
        f"the video processor must take iSdrEnhance{field} from its initial settings"
    )

# Shader: one decode/encode pair, no sRGB views, exact neutral path.
assert shader.count("pow(saturate(pixel.rgb), DisplayGamma)") == 1 and shader.count("1.0 / DisplayGamma") == 1, (
    "the shader must decode and encode the display gamma exactly once"
)
assert "_SRGB" not in processor[processor.find("SdrEnhancePass(const Tex2D_t&"):processor.find("void CDX11VideoProcessor::DrawSubtitles")], (
    "hardware sRGB views would add a second gamma transform"
)

print("SDR enhancement contract test passed")
