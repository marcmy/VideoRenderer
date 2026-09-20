#pragma once

#include "RifeSpatialPolicy.h"

/*
 * RIFE migration wrapper around the existing DX11 processor declaration.
 * The original declaration is preserved in DX11VideoProcessorLegacyBody.h.
 */

// Inject the RIFE bridge at a private declaration that has no override suffix.
// The wrapper returns to private visibility immediately afterward so the
// original class layout/encapsulation stays unchanged outside these helpers.
#define UpdateTexures() UpdateTexures(); \
public: \
	bool PrepareRifeSource(IMediaSample* pSample, ID3D11Texture2D* target, REFERENCE_TIME& sourceTime); \
	bool AcquireRifePresentationSurface(UINT width, UINT height, ID3D11Texture2D** target, UINT& sourceSurface); \
	bool ReserveRifePresentationSurface(ID3D11Texture2D* source, UINT& sourceSurface); \
	ID3D11Device* GetRifeDevice() const { return m_pDevice; } \
	static UINT RifeAlignedDimension(int value) { return value > 0 ? AlignRifeDimension(static_cast<UINT>(value)) : 0u; } \
	CSize GetRifeContentSize(); \
	CSize GetRifeFrameSize(); \
private:
#include "DX11VideoProcessorLegacyBody.h"
#undef UpdateTexures
