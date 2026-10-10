#pragma once

#include <d3d11.h>

// RIFE's completed BGRA presentation surface still needs a separate resource
// for NvCV registration. A one-to-one crop needs no shader conversion. Keep
// producer completion before CUDA mapping in CNvidiaMaxineVSR::Process.
inline bool TryCopyMaxineBgraInput(ID3D11DeviceContext* context,
		ID3D11Texture2D* source, ID3D11Texture2D* destination, const RECT& rect)
{
	if (!context || !source || !destination || source == destination) {
		return false;
	}
	D3D11_TEXTURE2D_DESC src = {}, dst = {};
	source->GetDesc(&src);
	destination->GetDesc(&dst);
	if (src.Format != DXGI_FORMAT_B8G8R8A8_UNORM || dst.Format != src.Format
			|| src.SampleDesc.Count != 1 || dst.SampleDesc.Count != 1
			|| src.MipLevels != 1 || dst.MipLevels != 1
			|| src.ArraySize != 1 || dst.ArraySize != 1
			|| src.Usage != D3D11_USAGE_DEFAULT || dst.Usage != D3D11_USAGE_DEFAULT
			|| rect.left < 0 || rect.top < 0
			|| rect.right <= rect.left || rect.bottom <= rect.top
			|| static_cast<UINT>(rect.right) > src.Width
			|| static_cast<UINT>(rect.bottom) > src.Height
			|| static_cast<UINT>(rect.right - rect.left) != dst.Width
			|| static_cast<UINT>(rect.bottom - rect.top) != dst.Height) {
		return false;
	}

	ID3D11ShaderResourceView* empty[3] = {};
	context->PSSetShaderResources(0, 3, empty);
	context->OMSetRenderTargets(0, nullptr, nullptr);
	const D3D11_BOX box = {static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
		static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1};
	context->CopySubresourceRegion(destination, 0, 0, 0, 0, source, 0, &box);
	return true;
}
