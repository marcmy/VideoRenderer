/*
 * Driver-only NVIDIA Optical Flow scene-change analysis for RIFE.
 *
 * This module uses the NVOF API installed by the NVIDIA display driver.  It
 * never synthesizes video frames; RIFE/TensorRT remains the only production
 * frame-generation backend.
 */

#pragma once

#include <d3d11.h>
#include <memory>
#include <string>

class CNvidiaSceneChangeDetector
{
public:
    struct Metrics {
        bool valid = false;
        bool likelyCut = false;
        float meanMotionPixels = 0.0f;
        float p90MotionPixels = 0.0f;
        float meanBidirectionalResidualPixels = 0.0f;
        float residualRatio = 0.0f;
        float incoherentFraction = 0.0f;
        float directionCoherence = 1.0f;
    };

    CNvidiaSceneChangeDetector();
    ~CNvidiaSceneChangeDetector();

    CNvidiaSceneChangeDetector(const CNvidiaSceneChangeDetector&) = delete;
    CNvidiaSceneChangeDetector& operator=(const CNvidiaSceneChangeDetector&) = delete;

    bool Initialize(ID3D11Device* device, UINT width, UINT height);
    bool BeginAnalyze(ID3D11Texture2D* first, ID3D11Texture2D* second);
    bool FinishAnalyze(Metrics& metrics);
    bool Analyze(ID3D11Texture2D* first, ID3D11Texture2D* second, Metrics& metrics);
    void Reset();

    const std::wstring& GetStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
