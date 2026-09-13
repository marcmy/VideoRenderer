from pathlib import Path


repo = Path(__file__).resolve().parents[2]

dx11_header = (repo / "Source" / "DX11VideoProcessor.h").read_text(encoding="utf-8")
dx11_legacy_header = (repo / "Source" / "DX11VideoProcessorLegacyBody.h").read_text(encoding="utf-8")
rife_bridge = (repo / "Source" / "RifeDX11Bridge.cpp").read_text(encoding="utf-8")
dx11_processor = (repo / "Source" / "DX11VideoProcessor.cpp").read_text(encoding="utf-8")
renderer_legacy = (repo / "Source" / "VideoRendererLegacy.inl").read_text(encoding="utf-8")


assert "GetRifeContentSize()" in dx11_header, (
    "RIFE needs a stable source-derived content size separate from the window size"
)

frame_size_start = dx11_header.find("CSize GetRifeFrameSize() const")
assert frame_size_start != -1, "GetRifeFrameSize() was not found"
frame_size_end = dx11_header.find("\\\nprivate:", frame_size_start)
assert frame_size_end != -1, "GetRifeFrameSize() declaration block end was not found"
frame_size_block = dx11_header[frame_size_start:frame_size_end]
assert "m_windowRect" not in frame_size_block, (
    "RIFE working dimensions must not change when only the player window changes"
)
assert "GetRifeContentSize()" in frame_size_block, (
    "RIFE aligned working dimensions must be derived from stable video content dimensions"
)

prepare_start = rife_bridge.find("bool CDX11VideoProcessor::PrepareRifeSource(")
assert prepare_start != -1, "PrepareRifeSource() was not found"
prepare_end = rife_bridge.find("bool CDX11VideoProcessor::ReserveRifePresentationSurface(", prepare_start)
assert prepare_end != -1, "PrepareRifeSource() end was not found"
prepare_body = rife_bridge[prepare_start:prepare_end]
assert "GetRifeContentSize()" in prepare_body, (
    "RIFE source preparation must use stable video content dimensions"
)
assert "m_videoRect" not in prepare_body, (
    "RIFE source preparation must not render into presentation-space coordinates"
)
assert "Process(target, m_srcRect, contentRect, false, true)" in prepare_body, (
    "RIFE source preparation must explicitly select the source-sized processing path"
)

assert "m_TexRifeConvertOutput" in dx11_legacy_header, (
    "RIFE must have a dedicated conversion intermediate that cannot inherit window-sized allocation"
)
assert "m_TexsRifePostScale" in dx11_legacy_header, (
    "RIFE must have dedicated post-scale intermediates that cannot inherit m_windowRect dimensions"
)

process_start = dx11_processor.find("HRESULT CDX11VideoProcessor::Process(")
assert process_start != -1, "Process() was not found"
process_end = dx11_processor.find("\nvoid CDX11VideoProcessor::SetVideoRect", process_start)
assert process_end != -1, "Process() end was not found"
process_body = dx11_processor[process_start:process_end]
assert "rifeSourcePreparation" in process_body, (
    "Process() must distinguish RIFE source preparation from presentation rendering"
)
assert "m_TexRifeConvertOutput" in process_body and "m_TexsRifePostScale" in process_body, (
    "RIFE source preparation must route both conversion and post-scale work through source-sized intermediates"
)
assert "rifeSourcePreparation ||" in process_body, (
    "RIFE source preparation must force the intermediate path instead of taking a window-sensitive direct VP shortcut"
)

convert_start = dx11_processor.find("HRESULT CDX11VideoProcessor::ConvertColorPass(")
assert convert_start != -1, "ConvertColorPass() was not found"
convert_end = dx11_processor.find("\nHRESULT CDX11VideoProcessor::ResizeShaderPass", convert_start)
assert convert_end != -1, "ConvertColorPass() end was not found"
convert_body = dx11_processor[convert_start:convert_end]
assert "pRenderTarget->GetDesc" in convert_body, (
    "color conversion viewport must come from the selected render target rather than m_TexConvertOutput"
)
assert "m_TexConvertOutput.desc.Width" not in convert_body and "m_TexConvertOutput.desc.Height" not in convert_body, (
    "color conversion must not silently reintroduce presentation-sized dimensions into RIFE preprocessing"
)

prepared_start = dx11_processor.find("if (m_pFrameInterpolationTexture && m_pFrameInterpolationView)")
assert prepared_start != -1, "prepared RIFE presentation branch was not found"
prepared_end = dx11_processor.find("\n\tHRESULT hr = S_OK;", prepared_start)
assert prepared_end != -1, "prepared RIFE presentation branch end was not found"
prepared_branch = dx11_processor[prepared_start:prepared_end]
assert "GetRifeContentSize()" in prepared_branch, (
    "prepared RIFE output must crop aligned padding back to the actual content dimensions"
)
assert "dstRect" in prepared_branch, (
    "prepared RIFE output must be scaled into the current presentation video rectangle"
)
assert "ResizeShaderPass" in prepared_branch, (
    "prepared RIFE output must use the renderer scaling path instead of a top-left point copy"
)

assert "RIFE input" in dx11_processor and "GetRifeFrameSize()" in dx11_processor, (
    "Ctrl+J diagnostics must expose the stable RIFE working dimensions for fullscreen verification"
)

rotation_start = renderer_legacy.find('if (!strcmp(field, "rotation"))')
assert rotation_start != -1, "rotation settings branch was not found"
rotation_end = renderer_legacy.find('\n\tif (!strcmp(field, "stereo3dTransform"))', rotation_start)
assert rotation_end != -1, "rotation settings branch end was not found"
rotation_branch = renderer_legacy[rotation_start:rotation_end]
assert "m_RifePipeline->Reset()" in rotation_branch and "ResetFrameInterpolationPresenterQueue()" in rotation_branch, (
    "rotation changes must invalidate queued RIFE work before new geometry is used for presentation"
)

print("RIFE spatial contract test passed")
