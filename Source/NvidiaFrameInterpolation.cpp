/*
 * NVIDIA frame interpolation facade.
 *
 * The implementation is renderer-owned and uses the NVIDIA Optical Flow API
 * exposed by the display driver. NvOFFRUC.dll and the CUDA runtime are no
 * longer required by this branch.
 */

#include "stdafx.h"
#include "NvidiaFrameInterpolation.h"
#include "NvidiaOpticalFlowNative.h"

struct CNvidiaFrameInterpolation::Impl
{
	CNvidiaOpticalFlowNative native;
};

CNvidiaFrameInterpolation::CNvidiaFrameInterpolation()
	: m_impl(std::make_unique<Impl>())
{
}

CNvidiaFrameInterpolation::~CNvidiaFrameInterpolation() = default;

bool CNvidiaFrameInterpolation::Initialize(ID3D11Device* device, const UINT width, const UINT height)
{
	return m_impl->native.Initialize(device, width, height);
}

void CNvidiaFrameInterpolation::Reset()
{
	m_impl->native.Reset();
}

bool CNvidiaFrameInterpolation::BeginInputFrame(ID3D11Texture2D** texture)
{
	return m_impl->native.BeginInputFrame(texture);
}

void CNvidiaFrameInterpolation::CancelInputFrame()
{
	m_impl->native.CancelInputFrame();
}

bool CNvidiaFrameInterpolation::SubmitInputFrame(const double inputTimestamp, const double outputTimestamp,
	bool& outputReady, bool& repeated)
{
	return m_impl->native.SubmitInputFrame(inputTimestamp, outputTimestamp, outputReady, repeated);
}

bool CNvidiaFrameInterpolation::AcquireCurrentFrame(
	ID3D11Texture2D** texture, ID3D11ShaderResourceView** view)
{
	return m_impl->native.AcquireCurrentFrame(texture, view);
}

void CNvidiaFrameInterpolation::ReleaseCurrentFrame()
{
	m_impl->native.ReleaseCurrentFrame();
}

bool CNvidiaFrameInterpolation::AcquireInterpolatedFrame(
	ID3D11Texture2D** texture, ID3D11ShaderResourceView** view)
{
	return m_impl->native.AcquireInterpolatedFrame(texture, view);
}

void CNvidiaFrameInterpolation::ReleaseInterpolatedFrame()
{
	m_impl->native.ReleaseInterpolatedFrame();
}

const std::wstring& CNvidiaFrameInterpolation::GetStatus() const
{
	return m_impl->native.GetStatus();
}

const std::wstring& CNvidiaFrameInterpolation::GetRuntimeInfo() const
{
	return m_impl->native.GetRuntimeInfo();
}

double CNvidiaFrameInterpolation::GetLastProcessTimeMs() const
{
	return m_impl->native.GetLastProcessTimeMs();
}
