#pragma once

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
	bool ReserveRifePresentationSurface(ID3D11Texture2D* source, UINT& sourceSurface); \
	ID3D11Device* GetRifeDevice() const { return m_pDevice; } \
	static UINT RifeAlignedDimension(int value) { return value > 0 ? (static_cast<UINT>(value) + 31u) & ~31u : 0u; } \
	CSize GetRifeContentSize() const { \
		UINT width = m_srcRectWidth; \
		UINT height = m_srcRectHeight; \
		if (!width || !height) { return CSize(0, 0); } \
		if (m_srcAnamorphic && m_srcAspectRatioX && m_srcAspectRatioY) { \
			width = static_cast<UINT>(MulDiv(height, m_srcAspectRatioX, m_srcAspectRatioY)); \
		} \
		return (m_iRotation == 90 || m_iRotation == 270) \
			? CSize(static_cast<int>(height), static_cast<int>(width)) \
			: CSize(static_cast<int>(width), static_cast<int>(height)); \
	} \
	CSize GetRifeFrameSize() const { \
		const CSize size = GetRifeContentSize(); \
		return CSize(RifeAlignedDimension(size.cx), RifeAlignedDimension(size.cy)); \
	} \
private:
#include "DX11VideoProcessorLegacyBody.h"
#undef UpdateTexures
