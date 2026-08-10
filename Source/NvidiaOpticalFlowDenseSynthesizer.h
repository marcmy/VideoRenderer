/*
 * Dense native NVIDIA Optical Flow midpoint synthesis.
 *
 * Converts the Turing-class 4x4 NVOF vector grid into a dense, image-guided
 * motion field before interpolation. All work remains on the renderer's D3D11
 * device; there is no CPU readback or CUDA dependency.
 */

#pragma once

#include <d3d11.h>
#include <atlbase.h>
#include <string>

class CNvidiaOpticalFlowDenseSynthesizer
{
public:
    bool Initialize(ID3D11Device* device, UINT frameWidth, UINT frameHeight,
        UINT flowWidth, UINT flowHeight, std::wstring& status);
    void Reset();

    bool Dispatch(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* previousFrame,
        ID3D11ShaderResourceView* nextFrame,
        ID3D11ShaderResourceView* forwardFlowBtoA,
        ID3D11ShaderResourceView* backwardFlowAtoB,
        ID3D11UnorderedAccessView* output,
        float midpointTime,
        std::wstring& status);

private:
    struct SeedParameters {
        UINT flowWidth;
        UINT flowHeight;
        float gridSize;
        float consistencyThreshold;
    };
    static_assert(sizeof(SeedParameters) == 16);

    struct JumpParameters {
        UINT flowWidth;
        UINT flowHeight;
        UINT jumpStep;
        UINT padding;
    };
    static_assert(sizeof(JumpParameters) == 16);

    struct DenseParameters {
        UINT frameWidth;
        UINT frameHeight;
        UINT flowWidth;
        UINT flowHeight;
        float gridSize;
        float spatialSigma;
        float colorSigma;
        float infillSigma;
    };
    static_assert(sizeof(DenseParameters) == 32);

    struct WarpParameters {
        UINT frameWidth;
        UINT frameHeight;
        float midpointTime;
        float padding;
    };
    static_assert(sizeof(WarpParameters) == 16);

    UINT m_frameWidth = 0;
    UINT m_frameHeight = 0;
    UINT m_flowWidth = 0;
    UINT m_flowHeight = 0;

    CComPtr<ID3D11ComputeShader> m_seedShader;
    CComPtr<ID3D11ComputeShader> m_jumpShader;
    CComPtr<ID3D11ComputeShader> m_denseShader;
    CComPtr<ID3D11ComputeShader> m_warpShader;

    CComPtr<ID3D11Texture2D> m_seedTextures[2];
    CComPtr<ID3D11ShaderResourceView> m_seedViews[2];
    CComPtr<ID3D11UnorderedAccessView> m_seedUavs[2];

    CComPtr<ID3D11Texture2D> m_denseFlowTexture;
    CComPtr<ID3D11ShaderResourceView> m_denseFlowView;
    CComPtr<ID3D11UnorderedAccessView> m_denseFlowUav;

    CComPtr<ID3D11Buffer> m_seedParameters;
    CComPtr<ID3D11Buffer> m_jumpParameters;
    CComPtr<ID3D11Buffer> m_denseParameters;
    CComPtr<ID3D11Buffer> m_warpParameters;
    CComPtr<ID3D11SamplerState> m_linearSampler;
};
