#pragma once

#include "RifeSpatialPolicy.h"
#include "RifePresentationPreparation.h"
#include "RifeFrameInterpolation.h"

/*
 * RIFE migration wrapper around the existing DX11 processor declaration.
 * The original declaration is preserved in DX11VideoProcessorLegacyBody.h.
 */

// Inject the RIFE bridge at a private declaration that has no override suffix.
// The wrapper returns to private visibility immediately afterward so the
// original class layout/encapsulation stays unchanged outside these helpers.
#define UpdateTexures() UpdateTexures(); \
public: \
	bool PrepareRifeSource(IMediaSample* pSample, ID3D11Texture2D* target, REFERENCE_TIME& sourceTime, UINT alignment = 32u); \
	bool AcquireRifePresentationSurface(UINT width, UINT height, ID3D11Texture2D** target, UINT& sourceSurface); \
	bool ReserveRifePresentationSurface(ID3D11Texture2D* source, UINT& sourceSurface, CSize contentSize); \
	bool ReserveRifeCudaPresentationSurface(const std::shared_ptr<RifeCudaOutputLease>& source, UINT& sourceSurface, CSize contentSize); \
	bool CanUseRifeCudaOutput(); \
	REFERENCE_TIME RifePresentationPreparationLeadTime(REFERENCE_TIME frameInterval) override; \
	bool PrepareRifePresentationSource(UINT sourceSurface, REFERENCE_TIME frameTime) override; \
	ID3D11Device* GetRifeDevice() const { return m_pDevice; } \
	static UINT RifeAlignedDimension(int value, UINT alignment = 32u) { return value > 0 ? AlignRifeDimension(static_cast<UINT>(value), alignment) : 0u; } \
	DXGI_FORMAT GetRifeSurfaceFormat() const; \
	CSize GetRifeContentSize() const; \
	CSize GetRifePresentationContentSize(UINT sourceSurface) const; \
	CSize GetRifeSourceContentSize() const { \
		const auto size = ResolveRifeContentSize(m_srcRectWidth, m_srcRectHeight, \
			m_srcAnamorphic, m_srcAspectRatioX, m_srcAspectRatioY, m_iRotation); \
		return CSize(static_cast<int>(size.width), static_cast<int>(size.height)); \
	} \
	CSize GetRifeFrameSize(UINT alignment = 32u) const { \
		const auto content = GetRifeContentSize(); \
		const auto size = AlignRifeSize({static_cast<uint32_t>(content.cx), static_cast<uint32_t>(content.cy)}, alignment); \
		return CSize(static_cast<int>(size.width), static_cast<int>(size.height)); \
	} \
private: \
	bool TryRifeVideoProcessorUpscale(Tex2D_t& input, ID3D11Texture2D* target, const CRect& contentRect, const CRect& dstRect); \
	Tex2D_t m_TexRifePreparedMaxine; \
	RifePreparedPresentationKey m_RifePreparedMaxineKey; \
	CRect m_RifePreparedMaxineInputRect, m_RifePreparedMaxineDestRect; \
	CSize m_RifePreparedMaxineSize; \
	int m_RifePreparedMaxineMode = -1; \
	bool m_RifePreparedMaxineOversampleClamped = false; \
	std::wstring m_RifePreparedMaxinePipeline, m_RifePreparedMaxineStatus; \
	UINT m_RifeRenderingSurface = UINT_MAX;
#include "DX11VideoProcessorLegacyBody.h"
#undef UpdateTexures
