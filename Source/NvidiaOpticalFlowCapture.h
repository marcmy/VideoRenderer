#pragma once

#include <d3d11.h>
#include <string>

// Diagnostic-only frame-pair capture used to replay real NVOF failures offline.
// The normal interpolation path does not depend on capture being armed.
struct NativeNvofCaptureInputs
{
	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	ID3D11Texture2D* firstFrame = nullptr;
	ID3D11Texture2D* secondFrame = nullptr;
	ID3D11Texture2D* midpointFrame = nullptr;
	ID3D11Texture2D* forwardFlow = nullptr;
	ID3D11Texture2D* backwardFlow = nullptr;
	ID3D11Texture2D* forwardCost = nullptr;
	ID3D11Texture2D* backwardCost = nullptr;
	UINT frameWidth = 0;
	UINT frameHeight = 0;
	UINT flowWidth = 0;
	UINT flowHeight = 0;
	float midpointTime = 0.5f;
	double firstTimestamp = 0.0;
	double secondTimestamp = 0.0;
};

bool IsNativeNvofCaptureRequested();
bool CaptureNativeNvofFramePair(
	const NativeNvofCaptureInputs& inputs,
	std::wstring& outputDirectory,
	std::wstring& errorMessage);
