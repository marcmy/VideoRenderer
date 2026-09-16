/*
 * (C) 2018-2026 see Authors.txt
 *
 * Optional RIFE/TensorRT frame interpolation runtime loader.
 */

#pragma once

#include "RifeRuntimeApi.h"

#include <string>

struct RifeRuntimeProbeResult {
    bool available = false;
    uint32_t abiVersion = 0;
    std::wstring modulePath;
    std::wstring status;
};

class CRifeFrameInterpolation {
public:
    CRifeFrameInterpolation() = default;
    ~CRifeFrameInterpolation();

    CRifeFrameInterpolation(const CRifeFrameInterpolation&) = delete;
    CRifeFrameInterpolation& operator=(const CRifeFrameInterpolation&) = delete;

    static RifeRuntimeProbeResult Probe(const std::wstring& overrideDirectory = {});

    bool Initialize(
        const std::wstring& overrideDirectory,
        ID3D11Device* device,
        uint32_t width,
        uint32_t height,
        uint32_t gpuIndex,
        uint32_t contextCount,
        bool performanceBoost,
        const std::wstring& modelPath,
        const std::wstring& cachePath);

    bool Interpolate(
        uint32_t contextIndex,
        ID3D11Texture2D* first,
        ID3D11Texture2D* second,
        ID3D11Texture2D* output,
        float timestep,
        MpcvrRifeStats& stats);

    void Reset() noexcept;

    [[nodiscard]] bool IsReady() const noexcept { return m_module && m_handle && m_interpolate; }
    [[nodiscard]] const std::wstring& GetStatus() const noexcept { return m_status; }
    [[nodiscard]] const std::wstring& GetModulePath() const noexcept { return m_modulePath; }

private:
    bool LoadAndCreate(
        const std::wstring& modulePath,
        ID3D11Device* device,
        uint32_t width,
        uint32_t height,
        uint32_t gpuIndex,
        uint32_t contextCount,
        bool performanceBoost,
        const std::wstring& modelPath,
        const std::wstring& cachePath,
        bool* abiCompatibleRuntime = nullptr);

    HMODULE m_module = nullptr;
    void* m_handle = nullptr;
    MpcvrRifeInterpolateFn m_interpolate = nullptr;
    MpcvrRifeDestroyFn m_destroy = nullptr;
    std::wstring m_modulePath;
    std::wstring m_status;
};
