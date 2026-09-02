from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "Source"


def read(name: str) -> str:
    return (SOURCE / name).read_text(encoding="utf-8").replace("\r\n", "\n")


def write(name: str, text: str) -> None:
    (SOURCE / name).write_text(text.replace("\r\n", "\n"), encoding="utf-8", newline="\n")


def replace_exact(text: str, old: str, new: str, label: str) -> str:
    old = old.replace("\r\n", "\n")
    new = new.replace("\r\n", "\n")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


dx = read("DX11VideoProcessor.cpp")

old = r'''void CDX11VideoProcessor::SetVideoRect(const CRect& videoRect)
{
	m_videoRect = videoRect;
	UpdateRenderRect();

	// Fixed Maxine scales do not need texture recreation for presentation-only
	// zoom changes. Match-output mode intentionally follows the player size.
	CSize maxineTargetSize;
	bool maxineUpscaleNeeded = false;
	if (m_iMaxineScale == MAXINE_SCALE_MatchOutput
			|| !GetMaxineVSRTargetSize(m_videoRect, maxineTargetSize, maxineUpscaleNeeded)) {
		UpdateTexures();
	}
}
'''

new = r'''void CDX11VideoProcessor::SetVideoRect(const CRect& videoRect)
{
	m_videoRect = videoRect;
	UpdateRenderRect();

	// Fixed Maxine scales do not need texture recreation for presentation-only
	// zoom changes. Match-output mode intentionally follows the player size.
	CSize maxineTargetSize;
	bool maxineUpscaleNeeded = false;
	if (m_iMaxineScale == MAXINE_SCALE_MatchOutput
			|| !GetMaxineVSRTargetSize(m_videoRect, maxineTargetSize, maxineUpscaleNeeded)) {
		UpdateTexures();

#ifdef _WIN64
		// InitMediaType() can run before the player's final video rectangle is known.
		// In Match Output mode SetVideoRect() may therefore change Maxine's real
		// output dimensions after the initial cold-start warmup. Warm the final
		// geometry here as well; CNvidiaMaxineVSR::WarmUp() is idempotent for an
		// already-warm adapter/mode/dimension tuple.
		if (m_iMaxineScale == MAXINE_SCALE_MatchOutput && m_pDeviceContext
				&& m_srcRectWidth > 0 && m_srcRectHeight > 0) {
			CSize warmTargetSize;
			bool warmUpscaleNeeded = false;
			if (GetMaxineVSRTargetSize(m_videoRect, warmTargetSize, warmUpscaleNeeded)) {
				CSize warmCurrentSize(m_srcRectWidth, m_srcRectHeight);

				auto WarmPass = [&](const MaxinePass pass) -> bool {
					CNvidiaMaxineVSR* effect = nullptr;
					CSize outputSize = warmCurrentSize;
					unsigned mode = 0;
					const wchar_t* passName = nullptr;

					switch (pass) {
					case MaxinePass::Upscale:
						if (m_iMaxineOperation != MAXINE_OPERATION_Upscale || !warmUpscaleNeeded) {
							return true;
						}
						effect = &m_MaxineVSR;
						outputSize = warmTargetSize;
						mode = ResolveMaxineUpscaleMode();
						passName = L"Upscale";
						break;
					case MaxinePass::Denoise:
						if (m_iMaxineDenoise == MAXINE_FILTER_Off
								|| (m_iMaxineOperation != MAXINE_OPERATION_Upscale
									&& m_iMaxineOperation != MAXINE_OPERATION_Denoise)) {
							return true;
						}
						effect = &m_MaxineDenoise;
						mode = 7u + static_cast<unsigned>(m_iMaxineDenoise);
						passName = L"Denoise";
						break;
					case MaxinePass::Deblur:
						if (m_iMaxineDeblur == MAXINE_FILTER_Off
								|| (m_iMaxineOperation != MAXINE_OPERATION_Upscale
									&& m_iMaxineOperation != MAXINE_OPERATION_Deblur)) {
							return true;
						}
						effect = &m_MaxineDeblur;
						mode = 11u + static_cast<unsigned>(m_iMaxineDeblur);
						passName = L"Deblur";
						break;
					}

					const bool ok = effect->WarmUp(
						m_pDeviceContext,
						static_cast<UINT>(warmCurrentSize.cx),
						static_cast<UINT>(warmCurrentSize.cy),
						static_cast<UINT>(outputSize.cx),
						static_cast<UINT>(outputSize.cy),
						mode,
						m_iMaxineGPU);
					DLog(L"NVIDIA Maxine {} final-geometry warm-up: {} ({}x{} -> {}x{}, {})",
						passName, ok ? L"ready" : L"failed",
						warmCurrentSize.cx, warmCurrentSize.cy,
						outputSize.cx, outputSize.cy, effect->GetStatus());
					if (ok) {
						warmCurrentSize = outputSize;
					}
					return ok;
				};

				if (m_iMaxineOperation == MAXINE_OPERATION_Upscale) {
					for (const MaxinePass pass : GetMaxinePassOrder(m_iMaxinePipeline)) {
						if (!WarmPass(pass)) {
							break;
						}
					}
				}
				else if (m_iMaxineOperation == MAXINE_OPERATION_Denoise) {
					WarmPass(MaxinePass::Denoise);
				}
				else if (m_iMaxineOperation == MAXINE_OPERATION_Deblur) {
					WarmPass(MaxinePass::Deblur);
				}
			}
		}
#endif
	}
}
'''

dx = replace_exact(dx, old, new, "SetVideoRect final Maxine geometry warmup")
write("DX11VideoProcessor.cpp", dx)
print("Applied Maxine final-geometry warmup patch.")
