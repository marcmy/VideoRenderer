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


# Expose an idempotent warm-up entry point. It uses throwaway BGRA textures
# and performs one invisible NvVFX run at the exact configured dimensions.
# This moves NvVFX_Load plus first-run CUDA/JIT/interop work out of the first
# displayed video frames while preserving the loaded effect afterwards.
header = read("NvidiaMaxineVSR.h")
header = replace_exact(
    header,
    """\tbool Process(\n\t\tID3D11DeviceContext* pDeviceContext,\n\t\tID3D11Texture2D* pInputTexture,\n\t\tID3D11Texture2D* pOutputTexture,\n\t\tunsigned mode,\n\t\tint gpuIndex = -1);\n""",
    """\tbool WarmUp(\n\t\tID3D11DeviceContext* pDeviceContext,\n\t\tUINT inputWidth,\n\t\tUINT inputHeight,\n\t\tUINT outputWidth,\n\t\tUINT outputHeight,\n\t\tunsigned mode,\n\t\tint gpuIndex = -1);\n\n\tbool Process(\n\t\tID3D11DeviceContext* pDeviceContext,\n\t\tID3D11Texture2D* pInputTexture,\n\t\tID3D11Texture2D* pOutputTexture,\n\t\tunsigned mode,\n\t\tint gpuIndex = -1);\n""",
    "Maxine header WarmUp declaration",
)
write("NvidiaMaxineVSR.h", header)

cpp = read("NvidiaMaxineVSR.cpp")
warmup_impl = r'''bool CNvidiaMaxineVSR::WarmUp(
	ID3D11DeviceContext* pDeviceContext,
	UINT inputWidth,
	UINT inputHeight,
	UINT outputWidth,
	UINT outputHeight,
	unsigned mode,
	int gpuIndex)
{
#ifdef _WIN64
	if (!pDeviceContext || !inputWidth || !inputHeight || !outputWidth || !outputHeight) {
		m_impl->status = L"Invalid Maxine warm-up parameters";
		return false;
	}

	CComPtr<ID3D11Device> device;
	pDeviceContext->GetDevice(&device);
	if (!device) {
		m_impl->status = L"Maxine warm-up could not get the D3D11 device";
		return false;
	}

	auto CreateTexture = [&](const UINT width, const UINT height,
		CComPtr<ID3D11Texture2D>& texture, CComPtr<ID3D11RenderTargetView>& rtv) -> bool {
		D3D11_TEXTURE2D_DESC desc = {};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, &texture);
		if (FAILED(hr)) {
			m_impl->status = std::format(L"Maxine warm-up CreateTexture2D failed ({})", HR2Str(hr));
			return false;
		}
		hr = device->CreateRenderTargetView(texture, nullptr, &rtv);
		if (FAILED(hr)) {
			m_impl->status = std::format(L"Maxine warm-up CreateRenderTargetView failed ({})", HR2Str(hr));
			return false;
		}
		return true;
	};

	CComPtr<ID3D11Texture2D> input;
	CComPtr<ID3D11Texture2D> output;
	CComPtr<ID3D11RenderTargetView> inputRtv;
	CComPtr<ID3D11RenderTargetView> outputRtv;
	if (!CreateTexture(inputWidth, inputHeight, input, inputRtv)
			|| !CreateTexture(outputWidth, outputHeight, output, outputRtv)) {
		return false;
	}

	LUID adapterLuid = {};
	const bool adapterLuidValid = m_impl->GetAdapterLuid(input, adapterLuid);
	const bool adapterMatches = m_impl->effect && m_impl->effectAdapterLuidValid && adapterLuidValid
		&& m_impl->effectAdapterLuid.HighPart == adapterLuid.HighPart
		&& m_impl->effectAdapterLuid.LowPart == adapterLuid.LowPart;
	const bool alreadyWarm = m_impl->effect
		&& m_impl->selectedGPU == gpuIndex
		&& adapterMatches
		&& m_impl->quality == mode
		&& m_impl->gpuInput.width == inputWidth
		&& m_impl->gpuInput.height == inputHeight
		&& m_impl->gpuOutput.width == outputWidth
		&& m_impl->gpuOutput.height == outputHeight;
	if (alreadyWarm) {
		m_impl->status = std::format(L"Ready, mode {} (warm)", mode);
		m_impl->lastProcessTimeMs = 0.0;
		return true;
	}

	const FLOAT clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
	pDeviceContext->ClearRenderTargetView(inputRtv, clear);
	pDeviceContext->ClearRenderTargetView(outputRtv, clear);
	pDeviceContext->Flush();

	const bool ok = Process(pDeviceContext, input, output, mode, gpuIndex);
	if (ok) {
		m_impl->status = std::format(L"Ready, mode {} (cold-start warmed)", mode);
		m_impl->lastProcessTimeMs = 0.0;
	}
	else {
		// A warm-up failure must not permanently suppress the ordinary first-frame
		// retry path. Process() already tore down any partially-created effect.
		m_impl->failed = false;
	}
	return ok;
#else
	UNREFERENCED_PARAMETER(pDeviceContext);
	UNREFERENCED_PARAMETER(inputWidth);
	UNREFERENCED_PARAMETER(inputHeight);
	UNREFERENCED_PARAMETER(outputWidth);
	UNREFERENCED_PARAMETER(outputHeight);
	UNREFERENCED_PARAMETER(mode);
	UNREFERENCED_PARAMETER(gpuIndex);
	return false;
#endif
}

'''
cpp = replace_exact(
    cpp,
    "bool CNvidiaMaxineVSR::Process(\n",
    warmup_impl + "bool CNvidiaMaxineVSR::Process(\n",
    "Maxine WarmUp implementation",
)
write("NvidiaMaxineVSR.cpp", cpp)

# Warm the exact configured Maxine pass chain once media/device/output geometry
# is established, before the first sample reaches the playback/render path.
dx = read("DX11VideoProcessor.cpp")n