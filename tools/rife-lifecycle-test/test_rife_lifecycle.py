from pathlib import Path


repo = Path(__file__).resolve().parents[2]

renderer = (repo / "Source" / "VideoRenderer.cpp").read_text(encoding="utf-8")
legacy_renderer = (repo / "Source" / "VideoRendererLegacy.inl").read_text(encoding="utf-8")
dx11_header = (repo / "Source" / "DX11VideoProcessor.h").read_text(encoding="utf-8")
dx11_processor = (repo / "Source" / "DX11VideoProcessor.cpp").read_text(encoding="utf-8")
rife_pipeline = (repo / "Source" / "RifePlaybackPipeline.cpp").read_text(encoding="utf-8")


def function_body(source: str, marker: str) -> str:
    start = source.find(marker)
    assert start != -1, f"{marker} implementation was not found"
    brace = source.find("{", start)
    assert brace != -1, f"{marker} implementation body was not found"
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f"{marker} implementation end was not found")


assert "RifeAlignedDimension" in dx11_header, (
    "RIFE source surfaces must be aligned to the model's 32-pixel spatial requirement"
)

prepared_start = dx11_processor.find("if (m_pFrameInterpolationTexture && m_pFrameInterpolationView)")
assert prepared_start != -1, "prepared-frame presentation branch was not found"
prepared_end = dx11_processor.find("\n\tHRESULT hr = S_OK;", prepared_start)
assert prepared_end != -1, "prepared-frame presentation branch end was not found"
prepared_branch = dx11_processor[prepared_start:prepared_end]
assert "GetRifeContentSize()" in prepared_branch and "ResizeShaderPass" in prepared_branch, (
    "aligned RIFE surfaces must crop padding and scale the content into the presentation rectangle"
)

assert "static bool RifeFramesCompatible" in rife_pipeline and "RifeFramesCompatible(*previous, current)" in rife_pipeline, (
    "the worker must never pair RIFE source textures from different device/size generations"
)

assert "ForgetRifeSettings(this)" in legacy_renderer, (
    "destroying a renderer must remove its address from the RIFE settings-load tracker"
)
assert "g_rifeSettingsLoaded.erase(renderer)" in renderer, (
    "RIFE settings tracker cleanup must actually erase the dead renderer address"
)

destructor = function_body(legacy_renderer, "CMpcVideoRenderer::~CMpcVideoRenderer(")
assert "m_RifePipeline.reset()" in destructor, (
    "renderer teardown must join/destroy the RIFE worker before destroying the video processor"
)
assert destructor.find("m_RifePipeline.reset()") < destructor.find("m_VideoProcessor.reset()"), (
    "the RIFE worker must not retain raw processor references after the processor is destroyed"
)

for transition in ("NewSegment", "BeginFlush", "Stop"):
    marker = f"CMpcVideoRenderer::{transition}("
    body = function_body(legacy_renderer, marker)
    assert "m_RifePipeline->Reset()" in body, (
        f"{transition} must invalidate queued/in-flight RIFE work immediately"
    )

print("RIFE lifecycle/source-contract test passed")
