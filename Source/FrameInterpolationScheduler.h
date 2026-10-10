/*
 * (C) 2018-2026 see Authors.txt
 *
 * This file is part of MPC-BE.
 *
 * MPC-BE is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

struct FrameRate {
    uint32_t numerator = 0;
    uint32_t denominator = 1;

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return numerator != 0 && denominator != 0;
    }
};

// When the input path retains one frame in each group, interpolation runs at
// the retained cadence. Rule matching may still use the original clip rate.
[[nodiscard]] FrameRate RetainedSourceRate(FrameRate sourceRate,
    uint32_t frameStride) noexcept;

// A source's moving-average duration may vary on every VFR sample. Keep one
// rate for rules, multiplier targets and the load controller, while the
// scheduler still uses each pair's real timestamps for interpolation.
class CFrameInterpolationSourceRateStabilizer {
public:
    void Reset() noexcept;
    void ResetSegment() noexcept;
    [[nodiscard]] FrameRate Observe(FrameRate nominalRate, int64_t sourceTime,
        bool hasSourceTime) noexcept;
    [[nodiscard]] bool HasVariableTiming() const noexcept { return m_variableTiming; }

private:
    FrameRate m_stableRate = {};
    FrameRate m_candidateRate = {};
    std::array<int64_t, 32> m_intervals = {};
    uint64_t m_intervalSum = 0;
    uint32_t m_intervalCount = 0;
    uint32_t m_intervalIndex = 0;
    uint32_t m_candidateSamples = 0;
    int64_t m_previousTime = 0;
    bool m_havePreviousTime = false;
    bool m_variableTiming = false;
};

enum class FrameInterpolationRateMode : uint8_t {
    Disabled = 0,
    ToScreen,
    Movie2x,
    Movie2_5x,
    Movie3x,
    Movie4x,
    Movie5x,
    Fixed60,
    Fixed72,
    Fixed90,
    Fixed120,
    Custom,
};

struct FrameInterpolationTarget {
    int64_t presentationTime = 0;
    double timestep = 0.0;
    bool exactSource = false; // Use the real second frame within source timestamp precision.
};

struct FrameInterpolationLoadEstimate {
    uint32_t requestedOutputFpsMilli = 0;
    uint32_t sustainableSyntheticFpsMilli = 0;
    uint32_t outputCapFpsMilli = 0; // Zero means the requested rate is sustainable.
};

struct FrameInterpolationPressureSnapshot {
    uint64_t sourcePoolMisses = 0;
    uint64_t lateSyntheticDrops = 0;
    uint64_t presentationDrops = 0;
    uint64_t presentationReclaims = 0;
    uint64_t presentationSurfaceWaitUs = 0;
    uint64_t presentationSurfaceWaitCount = 0;
    uint32_t sourceQueueDepth = 0;
    uint32_t pendingPairDepth = 0;
    uint32_t inferenceContexts = 1;
    uint64_t presenterStaleDrops = 0;
    // Count successful RIFE presenter renders, including real source frames.
    // This is independent of the OSD's short, unsynchronized FPS history.
    bool presenterDeliveryAvailable = false;
    uint64_t presenterRenderedFrames = 0;
    // Source-only playback while an engine is compiling cannot establish the
    // interpolation rate's capacity or invalidate a previously verified cap.
    bool interpolationReady = true;
    // Fresh, measured stage capacity only authorizes speculative recovery.
    // It must never lower a rate already proven by actual delivery.
    uint32_t sustainableSyntheticFpsMilli = 0;
    uint32_t inferenceTimingSamples = 0;
    uint64_t inferenceCompletedCalls = 0;
    uint64_t presenterAverageRenderUs = 0;
    uint32_t presenterTimingSamples = 0;
};

enum class FrameInterpolationPressurePhase : uint8_t {
    Open = 0,
    Settling,
    Stable,
    Probe,
    VerifiedRecovery,
};

enum FrameInterpolationPressureReason : uint32_t {
    RIFE_PRESSURE_NONE = 0,
    RIFE_PRESSURE_SOURCE_POOL_MISS = 1u << 0,
    RIFE_PRESSURE_LATE_SYNTHETIC_DROP = 1u << 1,
    RIFE_PRESSURE_PRESENTATION_DROP = 1u << 2,
    RIFE_PRESSURE_PRESENTATION_RECLAIM = 1u << 3,
    RIFE_PRESSURE_BACKLOG = 1u << 4,
    // Presentation-surface waits are expected producer backpressure when RIFE
    // gets ahead of the presenter. Keep them visible in diagnostics, but never
    // use them by themselves to reduce the requested output rate.
    RIFE_PRESSURE_PRESENTATION_SURFACE_WAIT = 1u << 5,
    RIFE_PRESSURE_PRESENTER_STALE_DROP = 1u << 6,
    RIFE_PRESSURE_DELIVERY_SHORTFALL = 1u << 7,
};

struct FrameInterpolationPressureDecision {
    uint32_t outputCapFpsMilli = 0;
    bool pressureDetected = false;
    bool changed = false;
    FrameInterpolationPressurePhase phase = FrameInterpolationPressurePhase::Open;
    uint32_t lastKnownGoodFpsMilli = 0;
    uint32_t lastKnownBadFpsMilli = 0;
    uint32_t backoffStepFpsMilli = 0;
    uint32_t observedPressureReasons = RIFE_PRESSURE_NONE;
    uint32_t confirmedPressureReasons = RIFE_PRESSURE_NONE;
    uint32_t measuredOutputFpsMilli = 0;
    bool measuredOutputReady = false;
    bool measuredOutputHealthy = false;
    bool recoveryWaitingForHeadroom = false;
};

class CFrameInterpolationPressureController {
public:
    void Reset() noexcept;
    // Keep the learned cap across a pause or seek, but discard pressure
    // samples from the old presentation generation before evaluating resumed playback.
    void Resume(uint64_t nowMs) noexcept;
    [[nodiscard]] FrameInterpolationPressureDecision Update(
        uint32_t requestedOutputFpsMilli,
        uint32_t sourceFpsMilli,
        uint64_t nowMs,
        const FrameInterpolationPressureSnapshot& snapshot) noexcept;
    [[nodiscard]] FrameInterpolationPressureDecision Update(
        uint32_t requestedOutputFpsMilli,
        FrameRate sourceRate,
        uint64_t nowMs,
        const FrameInterpolationPressureSnapshot& snapshot,
        bool variableSourceTiming = false) noexcept;

private:
    void ResetDeliveryMeasurement() noexcept;
    FrameInterpolationPressureSnapshot m_previousSnapshot = {};
    bool m_havePreviousSnapshot = false;
    uint32_t m_requestedOutputFpsMilli = 0;
    FrameRate m_sourceRate = {};
    bool m_variableSourceTiming = false;
    uint32_t m_outputCapFpsMilli = 0;
    uint32_t m_lastKnownGoodFpsMilli = 0;
    uint32_t m_lastKnownBadFpsMilli = 0;
    uint32_t m_failedAlignedFpsMilli = 0;
    uint32_t m_backoffStepFpsMilli = 0;
    bool m_probingAligned = false;
    uint64_t m_failedAlignedRetryAfterMs = 0;
    uint64_t m_arbitraryRetryAfterMs = 0;
    uint64_t m_arbitraryRetryDelayMs = 0;
    uint64_t m_phaseStartMs = 0;
    uint64_t m_pressureEvidenceStartMs = 0;
    uint32_t m_pressureEvidenceCount = 0;
    uint32_t m_pressureEvidenceReasons = RIFE_PRESSURE_NONE;
    uint64_t m_deliveryWindowStartMs = 0;
    uint64_t m_deliveryWindowStartFrames = 0;
    uint32_t m_measuredOutputFpsMilli = 0;
    bool m_deliveryWindowActive = false;
    bool m_measuredOutputReady = false;
    bool m_measuredOutputHealthy = false;
    struct ProbeHeadroomEvidence {
        uint32_t targetFpsMilli = 0;
        uint64_t startMs = 0;
        uint64_t lastFreshMs = 0;
        uint64_t startCalls = 0;
        uint64_t lastCalls = 0;
        uint64_t startRenderedFrames = 0;
        uint64_t lastRenderedFrames = 0;
    } m_probeHeadroom;
    // Compare retries against measurements taken BEFORE the failed probe.
    // Returning to a cheaper cap naturally speeds inference up; that alone
    // is not evidence that the GPU gained capacity since the earlier attempt.
    struct ProbeMeasurements {
        uint32_t targetFpsMilli = 0;
        uint32_t syntheticCapacityFpsMilli = 0;
        uint64_t presenterAverageRenderUs = 0;
    } m_probeStartMeasurements, m_rejectedProbeMeasurements;
    // One bounded retry of a rate verified before an interruption. Keep the
    // earlier failed upper bounds so success does not reopen capacity search.
    struct VerifiedRateRecovery {
        uint32_t targetFpsMilli = 0;
        uint32_t knownBadFpsMilli = 0;
        uint32_t failedAlignedFpsMilli = 0;
        uint64_t failedAlignedRetryAfterMs = 0;
        uint64_t arbitraryRetryAfterMs = 0;
        uint64_t arbitraryRetryDelayMs = 0;
    } m_verifiedRecovery;
    FrameInterpolationPressurePhase m_phase = FrameInterpolationPressurePhase::Open;
};

// Convert per-request wall time and measured concurrent-call overlap into an
// inference-only capacity estimate. 1000 means one active call, 2000 means two.
// The configured context count alone cannot establish actual parallel capacity.
// Delivered FPS and pipeline pressure decide backoff. This estimate can only
// authorize speculative recovery when sustained stage headroom is available.
[[nodiscard]] FrameInterpolationLoadEstimate EstimateFrameInterpolationLoad(
    FrameRate sourceRate,
    FrameRate requestedOutputRate,
    uint32_t measuredParallelismPermille,
    uint64_t averageInferenceWallUs,
    uint32_t sampleCount,
    uint32_t headroomPermille = 970,
    bool variableSourceTiming = false) noexcept;

class CFrameInterpolationScheduler {
public:
    void Reset() noexcept;
    void Configure(FrameInterpolationRateMode mode, FrameRate customRate, FrameRate displayRate,
        uint32_t maxMultiplierMilli = 0, uint32_t maxOutputFpsMilli = 0,
        uint32_t sourceFrameStride = 1) noexcept;
    void SetRuntimeOutputFpsCap(uint32_t maxOutputFpsMilli) noexcept;
    void SetVariableSourceTiming(bool variableSourceTiming) noexcept;

    [[nodiscard]] FrameRate ResolveConfiguredTargetRate(FrameRate sourceRate) const noexcept;
    [[nodiscard]] FrameRate ResolveTargetRate(FrameRate sourceRate) const noexcept;

    [[nodiscard]] std::vector<FrameInterpolationTarget> Schedule(
        int64_t firstTime,
        int64_t secondTime,
        FrameRate sourceRate);

private:
    [[nodiscard]] FrameRate ResolveRequestedRate(FrameRate sourceRate) const noexcept;
    [[nodiscard]] int64_t TargetTime(uint64_t index, FrameRate rate) const noexcept;

    FrameInterpolationRateMode m_mode = FrameInterpolationRateMode::Disabled;
    uint32_t m_sourceFrameStride = 1;
    FrameRate m_customRate = {};
    FrameRate m_displayRate = {};
    bool m_variableSourceTiming = false;
    uint32_t m_maxMultiplierMilli = 0;
    uint32_t m_maxOutputFpsMilli = 0;
    uint32_t m_runtimeMaxOutputFpsMilli = 0;
    FrameRate m_activeTargetRate = {};
    int64_t m_anchorTime = 0;
    uint64_t m_nextTargetIndex = 1;
    bool m_anchored = false;
};
