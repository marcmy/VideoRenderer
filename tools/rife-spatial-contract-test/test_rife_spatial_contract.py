from pathlib import Path


repo = Path(__file__).resolve().parents[2]

dx11_header = (repo / "Source" / "DX11VideoProcessor.h").read_text(encoding="utf-8")
rife_bridge = (repo / "Source" / "RifeDX11Bridge.cpp").read_text(encoding="utf-8")
dx11_processor = (repo / "Source" / "DX11VideoProcessor.cpp").read_text(encoding="utf-8")


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

print("RIFE spatial contract test passed")
