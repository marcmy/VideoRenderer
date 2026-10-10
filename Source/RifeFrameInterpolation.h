/*
 * (C) 2018-2026 see Authors.txt
 *
 * Optional RIFE/TensorRT frame interpolation runtime loader.
 */

#pragma once

#include "RifeRuntimeApi.h"
#include "RifeCudaOutputApi.h"

#include <atomic>
#include <memory>
#include <string>

class RifeCudaOutputLease;

struct RifeRuntimeProbeResult {
    bool available = false;
    uint32_t abiVersion = 0;
    std::wstring modulePath;
    std::wstring status;
};

class CRifeFrameInterpolation : public std::enable_shared_from_this<CRifeFrameInterpolation> {
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
        uint32_t contentWidth,
        uint32_t contentHeight,
        uint32_t gpuIndex,
        uint32_t contextCount,
        bool performanceBoost,
        const std::wstring& modelPath,
        const std::wstring& cachePath, bool highPrecision = false, bool featureReuse = false);

    bool Interpolate(
        uint32_t contextIndex,
        ID3D11Texture2D* first,
        ID3D11Texture2D* second,
        ID3D11Texture2D* output,
        float timestep,
        MpcvrRifeStats& stats,
        uint64_t inputPairId = 0);

    bool DrainContext(uint32_t contextIndex) noexcept;

    std::shared_ptr<RifeCudaOutputLease> AcquireCudaOutput();
    [[nodiscard]] bool SupportsCudaOutput() const noexcept { return m_acquireCudaOutput != nullptr; }

    void Reset() noexcept;

    [[nodiscard]] bool IsReady() const noexcept { return m_module && m_handle && m_interpolate; }
    [[nodiscard]] const std::wstring& GetStatus() const noexcept { return m_status; }
    [[nodiscard]] const std::wstring& GetModulePath() const noexcept { return m_modulePath; }

private:
    friend class RifeCudaOutputLease;
    bool LoadAndCreate(
        const std::wstring& modulePath,
        ID3D11Device* device,
        uint32_t width,
        uint32_t height,
        uint32_t contentWidth,
        uint32_t contentHeight,
        uint32_t gpuIndex,
        uint32_t contextCount,
        bool performanceBoost,
        const std::wstring& modelPath,
        const std::wstring& cachePath,
        bool* abiCompatibleRuntime = nullptr, bool highPrecision = false, bool featureReuse = false);

    HMODULE m_module = nullptr;
    void* m_handle = nullptr;
    MpcvrRifeInterpolateFn m_interpolate = nullptr;
    MpcvrRifeDrainContextFn m_drainContext = nullptr;
    MpcvrRifeDestroyFn m_destroy = nullptr;
    MpcvrRifeAcquireCudaOutputFn m_acquireCudaOutput = nullptr;
    MpcvrRifeInterpolateCudaFn m_interpolateCuda = nullptr;
    MpcvrRifeExportCudaOutputFn m_exportCudaOutput = nullptr;
    MpcvrRifeReleaseCudaOutputFn m_releaseCudaOutput = nullptr;
    std::atomic_uint32_t m_liveCudaLeases = 0;
    uint32_t m_width = 0, m_height = 0, m_contentWidth = 0, m_contentHeight = 0;
    std::wstring m_modulePath;
    std::wstring m_status;
};

// A queued/dropped output pins the producer handle AND DLL until its last owner
// releases it. Maxine must finish reading before that last owner is removed.
class RifeCudaOutputLease final {
public:
    explicit RifeCudaOutputLease(std::shared_ptr<CRifeFrameInterpolation> owner) : m_owner(std::move(owner)) {}
    ~RifeCudaOutputLease();
    RifeCudaOutputLease(const RifeCudaOutputLease&) = delete;
    RifeCudaOutputLease& operator=(const RifeCudaOutputLease&) = delete;
    const MpcvrRifeCudaOutput& GetImage() const noexcept { return m_image; }
    bool Interpolate(uint32_t contextIndex, ID3D11Texture2D* first, ID3D11Texture2D* second,
        float timestep, MpcvrRifeStats& stats, uint64_t inputPairId);
    bool Export(ID3D11Texture2D* output) const;
private:
    friend class CRifeFrameInterpolation;
    std::shared_ptr<CRifeFrameInterpolation> m_owner;
    MpcvrRifeCudaOutput m_image;
};
