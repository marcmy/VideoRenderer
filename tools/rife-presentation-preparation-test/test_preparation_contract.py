"""CPU-only structural checks for the presenter's ownership/cancellation boundary."""
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "rife-lifecycle-test"))
from test_rife_lifecycle import function_body

repo = Path(__file__).resolve().parents[2]
bridge = (repo / "Source/RifeDX11Bridge.cpp").read_text(encoding="utf-8")
dx11 = (repo / "Source/DX11VideoProcessor.cpp").read_text(encoding="utf-8")
renderer = (repo / "Source/VideoRendererLegacy.inl").read_text(encoding="utf-8")

wait = function_body(renderer, "bool CMpcVideoRenderer::WaitForFrameInterpolationTime(")
assert "m_RendererLock" not in wait and "m_InterfaceLock" not in wait
assert "frame.generation != m_FrameInterpolationPresenterGeneration.load()" in wait
assert "m_bStopFrameInterpolationPresenter.load()" in wait
assert "frame.clock->GetTime" in wait and "WaitForMultipleObjects" in wait

present = function_body(renderer, "void CMpcVideoRenderer::FrameInterpolationPresenter(")
early = present.index("WaitForFrameInterpolationTime(frame, timer, preparationLead)")
prepare = present.index("PrepareRifePresentationSource(")
deadline = present.index("const bool due = WaitForFrameInterpolationTime(frame, timer);")
draw = present.index("RenderFrameInterpolationSource(")
assert early < prepare < deadline < draw
assert "frame.generation == m_FrameInterpolationPresenterGeneration.load()" in present[early:prepare]
assert "if (!obsolete)" in present[early:prepare]
assert "+ preparationTicks" in present

prepare = function_body(bridge, "bool CDX11VideoProcessor::PrepareRifePresentationSource(")
assert "m_RifePreparedMaxineKey.Clear();" in prepare
assert "m_pFilter->m_filterState != State_Running" in prepare
assert "!surface.inUse" in prepare
assert "ApplyMaxine(input, inputRect, contentSize, target, upscale, true)" in prepare
assert "CopyResource(m_TexRifePreparedMaxine.pTexture, input->pTexture)" in prepare
assert "End(surface.retireQuery)" in prepare and "surface.retirePending = true" in prepare
assert "if (!enhanced) return Finish(false)" in prepare
assert "return Finish(true)" in prepare
# The early operation cannot replace the displayed frame, OSD or subtitle time.
assert "Render(" not in prepare and "Present(" not in prepare
assert "m_rtStart =" not in prepare

process = function_body(dx11, "HRESULT CDX11VideoProcessor::Process(")
assert "m_RifePreparedMaxineKey.Matches(m_FrameInterpolationGeneration.load()," in process
assert "m_rtStart, m_RifeRenderingSurface" in process
assert "m_RifePreparedMaxineDestRect == dstRect" in process
assert "ApplyMaxine(pInputTexture, inputRect, contentRect.Size()" in process
for marker in ("void CDX11VideoProcessor::ResetFrameInterpolation(",
               "void CDX11VideoProcessor::UpdateTexures(",
               "void CDX11VideoProcessor::ReleaseFrameInterpolationSource("):
    assert "m_RifePreparedMaxineKey.Clear()" in function_body(dx11, marker)
print("RIFE preparation ownership/deadline/cancellation source checks passed")
