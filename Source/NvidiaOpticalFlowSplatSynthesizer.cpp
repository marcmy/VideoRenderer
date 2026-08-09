#include "stdafx.h"
#include "NvidiaOpticalFlowSplatSynthesizer.h"
#include "NvidiaOpticalFlowSplatBytecode.h"
#include "NvidiaOpticalFlowResolveBytecode.h"
#include "Helper.h"

#include <algorithm>
#include <array>
#include <format>

namespace {

constexpr char SplatShaderSource[] = R"hlsl(
Texture2D<int2> ForwardFlow : register(t0);   // second -> first
Texture2D<int2> BackwardFlow : register(t1);  // first -> second
RWTexture2D<uint> Winner : register(u0);

cbuffer SplatParameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    float MidpointTime;
    float GridSize;
    float2 Padding;
};

int2 LoadFlowVector(bool backward, int2 coordinate)
{
    coordinate = clamp(coordinate, int2(0, 0), int2(FlowSize) - 1);
    return backward
        ? BackwardFlow.Load(int3(coordinate, 0))
        : ForwardFlow.Load(int3(coordinate, 0));
}

float2 LoadNearestFlow(bool backward, float2 pixel)
{
    int2 cell = int2(pixel / GridSize);
    return float2(LoadFlowVector(backward, cell)) / 32.0;
}

float2 SampleRawFlow(bool backward, float2 pixel)
{
    float2 grid = clamp(pixel / GridSize, 0.0, float2(FlowSize - 1));
    int2 p0 = int2(floor(grid));
    int2 p1 = min(p0 + 1, int2(FlowSize) - 1);
    float2 fraction = grid - float2(p0);

    float2 f00 = float2(LoadFlowVector(backward, int2(p0.x, p0.y))) / 32.0;
    float2 f10 = float2(LoadFlowVector(backward, int2(p1.x, p0.y))) / 32.0;
    float2 f01 = float2(LoadFlowVector(backward, int2(p0.x, p1.y))) / 32.0;
    float2 f11 = float2(LoadFlowVector(backward, int2(p1.x, p1.y))) / 32.0;
    return lerp(lerp(f00, f10, fraction.x), lerp(f01, f11, fraction.x), fraction.y);
}

bool IsValid(float2 position)
{
    return all(position >= 0.0) && all(position <= float2(FrameSize - 1));
}

void SubmitWinner(uint sourceId, int2 destination, float priority)
{
    if (any(destination < 0) || any(destination >= int2(FrameSize))) return;
    uint quantizedPriority = (uint)clamp(round(priority * 127.0), 1.0, 127.0);
    uint packed = (quantizedPriority << 25) | (sourceId & 0x01ffffffu);
    InterlockedMax(Winner[destination], packed);
}

void SplatSource(uint2 sourcePixel, bool backward, uint sourceOffset, float timeFactor)
{
    float2 pixel = float2(sourcePixel);
    int2 cell = int2(pixel / GridSize);
    float2 flow = float2(LoadFlowVector(backward, cell)) / 32.0;
    float motion = length(flow);
    if (motion >= 256.0) return;

    float2 cellPixel = float2(cell) * GridSize;
    float2 match = cellPixel + flow;
    if (!IsValid(match)) return;
    float2 reverseFlow = SampleRawFlow(!backward, match);
    float consistency = length(flow + reverseFlow);
    if (consistency >= 6.0) return;

    float confidence = exp(-max(consistency - 0.5, 0.0) / 4.0);
    float2 destination = pixel + timeFactor * flow;
    if (!IsValid(destination)) return;

    int2 p0 = int2(floor(destination));
    float2 fraction = destination - float2(p0);
    float4 weights = float4(
        (1.0 - fraction.x) * (1.0 - fraction.y),
        fraction.x * (1.0 - fraction.y),
        (1.0 - fraction.x) * fraction.y,
        fraction.x * fraction.y);

    uint sourceId = sourceOffset + sourcePixel.y * FrameSize.x + sourcePixel.x;
    SubmitWinner(sourceId, p0, confidence * weights.x);
    SubmitWinner(sourceId, p0 + int2(1, 0), confidence * weights.y);
    SubmitWinner(sourceId, p0 + int2(0, 1), confidence * weights.z);
    SubmitWinner(sourceId, p0 + int2(1, 1), confidence * weights.w);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    uint framePixels = FrameSize.x * FrameSize.y;
    SplatSource(id.xy, true, 0, MidpointTime);
    SplatSource(id.xy, false, framePixels, 1.0 - MidpointTime);
}
)hlsl";

constexpr char ResolveShaderSource[] = R"hlsl(
Texture2D<float4> FirstFrame : register(t0);
Texture2D<float4> SecondFrame : register(t1);
Texture2D<uint> Winner : register(t2);
RWTexture2D<float4> OutputFrame : register(u0);

cbuffer SplatParameters : register(b0)
{
    uint2 FrameSize;
    uint2 FlowSize;
    float MidpointTime;
    float GridSize;
    float2 Padding;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FrameSize)) return;

    uint packed = Winner.Load(int3(id.xy, 0));
    if (packed == 0) {
        OutputFrame[id.xy] = FirstFrame.Load(int3(id.xy, 0));
        return;
    }

    uint sourceId = packed & 0x01ffffffu;
    uint framePixels = FrameSize.x * FrameSize.y;
    bool second = sourceId >= framePixels;
    uint localId = second ? sourceId - framePixels : sourceId;
    uint2 sourcePixel = uint2(localId % FrameSize.x, localId / FrameSize.x);
    OutputFrame[id.xy] = second
        ? SecondFrame.Load(int3(sourcePixel, 0))
        : FirstFrame.Load(int3(sourcePixel, 0));
}
)hlsl";

struct SplatParameters {
	UINT frameWidth;
	UINT frameHeight;
	UINT flowWidth;
	UINT flowHeight;
	float midpointTime;
	float gridSize;
	float padding[2];
};
static_assert(sizeof(SplatParameters) == 32);

} // namespace

struct CNvidiaOpticalFlowSplatSynthesizer::Impl
{
	CComPtr<ID3D11Texture2D> winnerTexture;
	CComPtr<ID3D11ShaderResourceView> winnerView;
	CComPtr<ID3D11UnorderedAccessView> winnerUav;
	CComPtr<ID3D11ComputeShader> splatShader;
	CComPtr<ID3D11ComputeShader> resolveShader;
	CComPtr<ID3D11Buffer> parameters;
	UINT frameWidth = 0;
	UINT frameHeight = 0;
	UINT flowWidth = 0;
	UINT flowHeight = 0;

	void Reset()
	{
		parameters.Release();
		resolveShader.Release();
		splatShader.Release();
		winnerUav.Release();
		winnerView.Release();
		winnerTexture.Release();
		frameWidth = frameHeight = flowWidth = flowHeight = 0;
	}
};

CNvidiaOpticalFlowSplatSynthesizer::CNvidiaOpticalFlowSplatSynthesizer()
	: m_impl(std::make_unique<Impl>())
{
}

CNvidiaOpticalFlowSplatSynthesizer::~CNvidiaOpticalFlowSplatSynthesizer() = default;

void CNvidiaOpticalFlowSplatSynthesizer::Reset()
{
	m_impl->Reset();
}

bool CNvidiaOpticalFlowSplatSynthesizer::Initialize(ID3D11Device* device,
	const UINT frameWidth, const UINT frameHeight,
	const UINT flowWidth, const UINT flowHeight, std::wstring& status)
{
	m_impl->Reset();
	if (!device || !frameWidth || !frameHeight || !flowWidth || !flowHeight) {
		status = L"Invalid native NVOF splat synthesizer dimensions";
		return false;
	}
	if (static_cast<uint64_t>(frameWidth) * frameHeight * 2ull > (1ull << 25)) {
		status = L"Native NVOF splat source-ID packing exceeded 25 bits";
		return false;
	}

	D3D11_TEXTURE2D_DESC winnerDesc = {};
	winnerDesc.Width = frameWidth;
	winnerDesc.Height = frameHeight;
	winnerDesc.MipLevels = 1;
	winnerDesc.ArraySize = 1;
	winnerDesc.Format = DXGI_FORMAT_R32_UINT;
	winnerDesc.SampleDesc.Count = 1;
	winnerDesc.Usage = D3D11_USAGE_DEFAULT;
	winnerDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	HRESULT hr = device->CreateTexture2D(&winnerDesc, nullptr, &m_impl->winnerTexture);
	if (FAILED(hr)) {
		status = std::format(L"CreateTexture2D(native NVOF splat winner) failed ({})", HR2Str(hr));
		return false;
	}
	hr = device->CreateShaderResourceView(m_impl->winnerTexture, nullptr, &m_impl->winnerView);
	if (FAILED(hr)) {
		status = std::format(L"CreateShaderResourceView(native NVOF splat winner) failed ({})", HR2Str(hr));
		return false;
	}
	hr = device->CreateUnorderedAccessView(m_impl->winnerTexture, nullptr, &m_impl->winnerUav);
	if (FAILED(hr)) {
		status = std::format(L"CreateUnorderedAccessView(native NVOF splat winner) failed ({})", HR2Str(hr));
		return false;
	}

	hr = device->CreateComputeShader(g_NativeNvofSplatBytecode,
		sizeof(g_NativeNvofSplatBytecode), nullptr, &m_impl->splatShader);
	if (FAILED(hr)) {
		status = std::format(L"CreateComputeShader(native NVOF splat) failed ({})", HR2Str(hr));
		return false;
	}
	hr = device->CreateComputeShader(g_NativeNvofResolveBytecode,
		sizeof(g_NativeNvofResolveBytecode), nullptr, &m_impl->resolveShader);
	if (FAILED(hr)) {
		status = std::format(L"CreateComputeShader(native NVOF splat resolve) failed ({})", HR2Str(hr));
		return false;
	}

	D3D11_BUFFER_DESC bufferDesc = {};
	bufferDesc.ByteWidth = sizeof(SplatParameters);
	bufferDesc.Usage = D3D11_USAGE_DEFAULT;
	bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	hr = device->CreateBuffer(&bufferDesc, nullptr, &m_impl->parameters);
	if (FAILED(hr)) {
		status = std::format(L"CreateBuffer(native NVOF splat params) failed ({})", HR2Str(hr));
		return false;
	}

	m_impl->frameWidth = frameWidth;
	m_impl->frameHeight = frameHeight;
	m_impl->flowWidth = flowWidth;
	m_impl->flowHeight = flowHeight;
	return true;
}

bool CNvidiaOpticalFlowSplatSynthesizer::Dispatch(ID3D11DeviceContext* context,
	ID3D11ShaderResourceView* firstFrame,
	ID3D11ShaderResourceView* secondFrame,
	ID3D11ShaderResourceView* forwardFlow,
	ID3D11ShaderResourceView* backwardFlow,
	ID3D11UnorderedAccessView* output,
	const float midpointTime,
	std::wstring& status)
{
	if (!context || !firstFrame || !secondFrame || !forwardFlow || !backwardFlow ||
			!output || !m_impl->winnerUav || !m_impl->winnerView ||
			!m_impl->splatShader || !m_impl->resolveShader || !m_impl->parameters) {
		status = L"Native NVOF splat synthesizer is not initialized";
		return false;
	}

	const SplatParameters values = {
		m_impl->frameWidth, m_impl->frameHeight,
		m_impl->flowWidth, m_impl->flowHeight,
		midpointTime, 4.0f, {0.0f, 0.0f},
	};
	context->UpdateSubresource(m_impl->parameters, 0, nullptr, &values, 0, 0);
	const UINT clear[4] = {};
	context->ClearUnorderedAccessViewUint(m_impl->winnerUav, clear);

	ID3D11Buffer* constantBuffer = m_impl->parameters;
	const std::array<ID3D11ShaderResourceView*, 2> flowViews = { forwardFlow, backwardFlow };
	ID3D11UnorderedAccessView* winnerOutput = m_impl->winnerUav;
	context->CSSetShader(m_impl->splatShader, nullptr, 0);
	context->CSSetConstantBuffers(0, 1, &constantBuffer);
	context->CSSetShaderResources(0, static_cast<UINT>(flowViews.size()), flowViews.data());
	context->CSSetUnorderedAccessViews(0, 1, &winnerOutput, nullptr);
	context->Dispatch((m_impl->frameWidth + 7) / 8, (m_impl->frameHeight + 7) / 8, 1);

	const std::array<ID3D11ShaderResourceView*, 2> nullFlowViews = {};
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetShaderResources(0, static_cast<UINT>(nullFlowViews.size()), nullFlowViews.data());

	const std::array<ID3D11ShaderResourceView*, 3> resolveViews = {
		firstFrame, secondFrame, m_impl->winnerView,
	};
	ID3D11UnorderedAccessView* outputUav = output;
	context->CSSetShader(m_impl->resolveShader, nullptr, 0);
	context->CSSetShaderResources(0, static_cast<UINT>(resolveViews.size()), resolveViews.data());
	context->CSSetUnorderedAccessViews(0, 1, &outputUav, nullptr);
	context->Dispatch((m_impl->frameWidth + 7) / 8, (m_impl->frameHeight + 7) / 8, 1);

	const std::array<ID3D11ShaderResourceView*, 3> nullResolveViews = {};
	ID3D11Buffer* nullBuffer = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetShaderResources(0, static_cast<UINT>(nullResolveViews.size()), nullResolveViews.data());
	context->CSSetConstantBuffers(0, 1, &nullBuffer);
	context->CSSetShader(nullptr, nullptr, 0);
	return true;
}
