/*
 * Driver-only NVIDIA Optical Flow frame interpolation backend.
 *
 * Uses nvofapi.dll/nvofapi64.dll installed by the NVIDIA display driver.
 * No NvOFFRUC.dll, CUDA runtime, or Optical Flow SDK binary is required.
 */

#pragma once

#include <d3d11.h>
#include <memory>
#include <string>

class CNvidiaOpticalFlowNativeProbe
{
public:
	struct Result {
		bool available = false;
		unsigned apiMajor = 0;
		unsigned apiMinor = 0;
		std::wstring moduleName;
		std::wstring status;
	};

	static Result Probe();
};

class CNvidiaOpticalFlowNative
{
public:
	CNvidiaOpticalFlowNative();
	~CNvidiaOpticalFlowNative();

	CNvidiaOpticalFlowNative(const CNvidiaOpticalFlowNative&) = delete;
	CNvidiaOpticalFlowNative& operator=(const CNvidiaOpticalFlowNative&) = delete;

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
