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
		unsigned quality);

	void Reset();
	const std::wstring& GetStatus() const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
