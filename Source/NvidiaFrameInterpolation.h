/*
 * Optional NVIDIA Optical Flow frame-rate up-conversion integration.
 *
 * The proprietary NvOFFRUC runtime is loaded dynamically and is not linked
 * or redistributed with MPC Video Renderer.
 */

#pragma once

#include <d3d11.h>
#include <memory>
#include <string>

class CNvidiaFrameInterpolation
{
public:
	CNvidiaFrameInterpolation();
	~CNvidiaFrameInterpolation();

	CNvidiaFrameInterpolation(const CNvidiaFrameInterpolation&) = delete;
	CNvidiaFrameInterpolation& operator=(const CNvidiaFrameInterpolation&) = delete;

	bool Initialize(ID3D11Device* device, UINT width, UINT height);
	void Reset();

	bool BeginInputFrame(ID3D11Texture2D** texture);
	void CancelInputFrame();
	bool SubmitInputFrame(double inputTimestamp, double outputTimestamp,
		bool& outputReady, bool& repeated);

	bool AcquireCurrentFrame(ID3D11Texture2D** texture, ID3D11ShaderResourceView** view);
	void ReleaseCurrentFrame();
	bool AcquireInterpolatedFrame(ID3D11Texture2D** texture, ID3D11ShaderResourceView** view);
	void ReleaseInterpolatedFrame();

	const std::wstring& GetStatus() const;
	const std::wstring& GetRuntimeInfo() const;
	double GetLastProcessTimeMs() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
