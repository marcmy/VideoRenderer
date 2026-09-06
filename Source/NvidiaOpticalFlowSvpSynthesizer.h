/*
 * SVP4-style native NVIDIA Optical Flow midpoint synthesis research path.
 *
 * This deliberately bypasses the Build 3 dense/JFA repair pipeline.  It uses
 * the native 4x4 NVOF fields directly, reproduces the proprietary modern
 * software-SAD scene classifier, and implements the stock algo21/force13
 * control path for MPCVR's 2x midpoint mode.
 */

#pragma once

#include <atlbase.h>
#include <d3d11.h>
#include <string>

class CNvidiaOpticalFlowSvpSynthesizer
{
public:
    bool Initialize(ID3D11Device* device, UINT frameWidth, UINT frameHeight,
        UINT flowWidth, UINT flowHeight, std::wstring& status);
    void Reset();

    bool Dispatch(ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* previousFrame,
        ID3D11ShaderResourceView* nextFrame,
        ID3D11ShaderResourceView* previousLuma,
        ID3D11ShaderResourceView* nextLuma,
        ID3D11ShaderResourceView* flowBtoA,
        ID3D11ShaderResourceView* flowAtoB,
        ID3D11UnorderedAccessView* output,
        float midpointTime,
        std::wstring& status);

    std::wstring GetTelemetryText() const;

private:
    struct ClassifyParameters {
        UINT flowWidth;
        UINT flowHeight;
        UINT frameWidth;
        UINT frameHeight;
        UINT borderX;
        UINT borderY;
        UINT padding[2];
    };
    static_assert(sizeof(ClassifyParameters) == 32);

    struct ControlParameters {
        UINT flowWidth;
        UINT flowHeight;
        UINT borderX;
        UINT borderY;
    };
    static_assert(sizeof(ControlParameters) == 16);

    struct CoverageScatterParameters {
        UINT flowWidth;
        UINT flowHeight;
        UINT padding[2];
    };
    static_assert(sizeof(CoverageScatterParameters) == 16);

    struct CoverageFinishParameters {
        UINT flowWidth;
        UINT flowHeight;
        UINT coverPercent;
        UINT padding;
    };
    static_assert(sizeof(CoverageFinishParameters) == 16);

    struct WarpParameters {
        UINT frameWidth;
        UINT frameHeight;
        UINT flowWidth;
        UINT flowHeight;
        UINT padding[4];
    };
    static_assert(sizeof(WarpParameters) == 32);

    UINT m_frameWidth = 0;
    UINT m_frameHeight = 0;
    UINT m_flowWidth = 0;
    UINT m_flowHeight = 0;

    CComPtr<ID3D11ComputeShader> m_classifyShader;
    CComPtr<ID3D11ComputeShader> m_controlShader;
    CComPtr<ID3D11ComputeShader> m_coverageScatterShader;
    CComPtr<ID3D11ComputeShader> m_coverageFinishShader;
    CComPtr<ID3D11ComputeShader> m_warpShader;

    CComPtr<ID3D11Buffer> m_classifyParameters;
    CComPtr<ID3D11Buffer> m_controlParameters;
    CComPtr<ID3D11Buffer> m_coverageScatterParameters;
    CComPtr<ID3D11Buffer> m_coverageFinishParameters;
    CComPtr<ID3D11Buffer> m_warpParameters;

    CComPtr<ID3D11Buffer> m_classCounters;
    CComPtr<ID3D11ShaderResourceView> m_classCountersView;
    CComPtr<ID3D11UnorderedAccessView> m_classCountersUav;

    CComPtr<ID3D11Buffer> m_controlState;
    CComPtr<ID3D11ShaderResourceView> m_controlStateView;
    CComPtr<ID3D11UnorderedAccessView> m_controlStateUav;
    static constexpr UINT TelemetrySlotCount = 3;
    CComPtr<ID3D11Buffer> m_controlReadback[TelemetrySlotCount];
    bool m_telemetryPrimed[TelemetrySlotCount] = {};
    UINT m_telemetryWriteIndex = 0;

    CComPtr<ID3D11Buffer> m_coverageAccum[2];
    CComPtr<ID3D11ShaderResourceView> m_coverageAccumView[2];
    CComPtr<ID3D11UnorderedAccessView> m_coverageAccumUav[2];
    CComPtr<ID3D11Buffer> m_coverageMask[2];
    CComPtr<ID3D11ShaderResourceView> m_coverageMaskView[2];
    CComPtr<ID3D11UnorderedAccessView> m_coverageMaskUav[2];

    CComPtr<ID3D11Texture1D> m_pairLumaLut;
    CComPtr<ID3D11ShaderResourceView> m_pairLumaLutView;

    int m_lastClass = 0;
    UINT m_lastPhase = 128;
    UINT m_lastAlgorithm = 21;
    UINT m_lastConsidered = 0;
    UINT m_lastZeroSkipped = 0;
    UINT m_lastM1Plus = 0;
    UINT m_lastM2Plus = 0;
    UINT m_lastScenePlus = 0;
    bool m_haveTelemetry = false;
};
