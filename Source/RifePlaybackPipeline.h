#pragma once

#include <cstdint>
#include <memory>

#include <d3d11_4.h>
#include <strmif.h>

#include "Helper.h"
#include "FrameInterpolationScheduler.h"
#include "IVideoRenderer.h"

class CMpcVideoRenderer;
class CDX11VideoProcessor;

// Asynchronous RIFE playback coordinator. Source-frame rendering remains on
// MPCVR's receive/D3D11 thread; scene analysis and TensorRT inference run on a
// private worker so video/audio playback never waits for AI.
class CRifePlaybackPipeline
{
public:
    explicit CRifePlaybackPipeline(CMpcVideoRenderer* owner);
    ~CRifePlaybackPipeline();

    CRifePlaybackPipeline(const CRifePlaybackPipeline&) = delete;
    CRifePlaybackPipeline& operator=(const CRifePlaybackPipeline&) = delete;

    // Called with the renderer lock held. The method only renders/copies the
    // current source frame and enqueues lightweight work; it never performs
    // NVOF analysis or TensorRT inference synchronously.
    bool SubmitSample(
        CDX11VideoProcessor* processor,
        IMediaSample* sample,
        const Settings_t& settings,
        uint64_t presenterGeneration,
        FrameRate displayRate,
        REFERENCE_TIME frameDuration);

    // Non-blocking reset used for seek/flush/configuration changes. In-flight
    // GPU work is invalidated by generation checks and discarded on completion.
    void Reset() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
