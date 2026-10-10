/*
 * Optional NVIDIA Maxine Video Super Resolution runtime integration.
 *
 * The implementation loads the NVIDIA Video Effects runtime dynamically, so
 * building MPC Video Renderer does not require the proprietary SDK binaries.
 */

#pragma once

#include <d3d11.h>
#include <memory>
#include <string>

struct MpcvrRifeCudaOutput;

class CNvidiaMaxineVSR
{
public:
	CNvidiaMaxineVSR();
	~CNvidiaMaxineVSR();

	CNvidiaMaxineVSR(const CNvidiaMaxineVSR&) = delete;
	CNvidiaMaxineVSR& operator=(const CNvidiaMaxineVSR&) = delete;

	bool Process(
		ID3D11DeviceContext* pDeviceContext,
		ID3D11Texture2D* pInputTexture,
		ID3D11Texture2D* pOutputTexture,
		unsigned mode,
		int gpuIndex = -1,
		bool releaseD3DImagesAfterRun = true,
		bool throttleGpuQueue = false,
		bool waitForD3DInput = false,
		const MpcvrRifeCudaOutput* cudaInput = nullptr,
		int strengthPercent = 100);

	void Reset();
	bool CanUseCudaInput() const noexcept;
	const std::wstring& GetStatus() const;
	const std::wstring& GetRuntimeInfo() const;
	double GetLastProcessTimeMs() const;
	double GetAverageGpuProcessTimeMs() const;
	std::wstring GetGpuTimingDiagnostics() const;
	std::wstring GetGraphicsInputWaitDiagnostics() const;
	std::wstring GetStrengthDiagnostics() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
