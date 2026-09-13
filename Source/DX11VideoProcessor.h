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
	bool ReserveRifePresentationSurface(ID3D11Texture2D* source, UINT& sourceSurface); \
	ID3D11Device* GetRifeDevice() const { return m_pDevice; } \
	static UINT RifeAlignedDimension(int value) { return value > 0 ? AlignRifeDimension(static_cast<UINT>(value)) : 0u; } \
	CSize GetRifeContentSize() const { \
		const auto size = ResolveRifeContentSize(m_srcRectWidth, m_srcRectHeight, \
			m_srcAnamorphic, m_srcAspectRatioX, m_srcAspectRatioY, m_iRotation); \
		return CSize(static_cast<int>(size.width), static_cast<int>(size.height)); \
	} \
	CSize GetRifeFrameSize() const { \
		const auto content = ResolveRifeContentSize(m_srcRectWidth, m_srcRectHeight, \
			m_srcAnamorphic, m_srcAspectRatioX, m_srcAspectRatioY, m_iRotation); \
		const auto size = AlignRifeSize(content); \
		return CSize(static_cast<int>(size.width), static_cast<int>(size.height)); \
	} \
private:
#include "DX11VideoProcessorLegacyBody.h"
#undef UpdateTexures
