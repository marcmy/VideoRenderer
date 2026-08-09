#pragma once

#include <d3d11.h>

#include <memory>
#include <string>

class CNvidiaOpticalFlowSplatSynthesizer
{
public:
	CNvidiaOpticalFlowSplatSynthesizer();
	~CNvidiaOpticalFlowSplatSynthesizer();

	bool Initialize(ID3D11Device* device, UINT frameWidth, UINT frameHeight,
		UINT flowWidth, UINT flowHeight, std::wstring& status);
	void Reset();

	bool Dispatch(ID3D11DeviceContext* context,
		ID3D11ShaderResourceView* firstFrame,
		ID3D11ShaderResourceView* secondFrame,
		ID3D11ShaderResourceView* forwardFlow,
		ID3D11ShaderResourceView* backwardFlow,
		ID3D11UnorderedAccessView* output,
		float midpointTime,
		std::wstring& status);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
