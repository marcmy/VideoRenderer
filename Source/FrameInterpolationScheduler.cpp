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

#include "FrameInterpolationScheduler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {

constexpr uint64_t ReferenceTicksPerSecond = 10'000'000ULL;

FrameRate ReduceRate(FrameRate rate) noexcept
{
    if (!rate.IsValid()) {
        return {};
    }
    const uint32_t divisor = std::gcd(rate.numerator, rate.denominator);
    rate.numerator /= divisor;
    rate.denominator /= divisor;
    return rate;
}

FrameRate SnapSourceRate(FrameRate rate) noexcept
{
    rate = ReduceRate(rate);
    if (!rate.IsValid()) {
        return {};
    }

    constexpr FrameRate commonRates[] = {
        {24, 1}, {25, 1}, {30, 1}, {50, 1}, {60, 1}, {120, 1},
        {24'000, 1'001},
        {30'000, 1'001},
        {60'000, 1'001},
    };
    constexpr long double relativeTolerance = 10.0e-6L; // 10 ppm

    const long double value = static_cast<long double>(rate.numerator) / rate.denominator;
    for (const auto candidate : commonRates) {
        const long double canonical = static_cast<long double>(candidate.numerator) / candidate.denominator;
        if (std::abs(value - canonical) / canonical <= relativeTolerance) {
            return candidate;
        }
    }
    return rate;
}

FrameRate ScaleRate(FrameRate rate, uint32_t numerator, uint32_t denominator = 1) noexcept
{
    if (!rate.IsValid() || numerator == 0 || denominator == 0) {
        return {};
    }

    uint64_t scaledNumerator = static_cast<uint64_t>(rate.numerator) * numerator;
    uint64_t scaledDenominator = static_cast<uint64_t>(rate.denominator) * denominator;
    const uint64_t divisor = std::gcd(scaledNumerator, scaledDenominator);
    scaledNumerator /= divisor;
    scaledDenominator /= divisor;
    if (scaledNumerator > std::numeric_limits<uint32_t>::max()
            || scaledDenominator > std::numeric_limits<uint32_t>::max()) {
        return {};
    }

    return ReduceRate({
        static_cast<uint32_t>(scaledNumerator),
        static_cast<uint32_t>(scaledDenominator),
    });
}

FrameRate RestoreSourceAlignedRate(const FrameRate sourceRate, const uint32_t fpsMilli) noexcept
{
    FrameRate rate = ReduceRate({fpsMilli, 1000});
    const FrameRate source = SnapSourceRate(sourceRate);
    if (!rate.IsValid() || !source.IsValid()) {
        return rate;
    }

    const long double sourceFps = static_cast<long double>(source.numerator)
        / source.denominator;
    const long long nearestMultiplier = std::llround((fpsMilli / 1000.0L) / sourceFps);
    if (nearestMultiplier < 1
            || nearestMultiplier > std::numeric_limits<uint32_t>::max()) {
        return rate;
    }
    const FrameRate aligned = ScaleRate(source, static_cast<uint32_t>(nearestMultiplier));
    if (aligned.IsValid()) {
        const long double alignedMilli = static_cast<long double>(aligned.numerator)
            * 1000.0L / aligned.denominator;
        // The controller stores milli-fps, but the scheduler must recover the
        // exact rational for uncommon fractional source rates as well as NTSC.
        if (std::abs(alignedMilli - fpsMilli) <= 0.5L) {
            rate = aligned;
        }
    }
    return rate;
}

bool IsSourceAlignedRateMilli(const FrameRate sourceRate, const uint32_t fpsMilli) noexcept
{
    const FrameRate source = SnapSourceRate(sourceRate);
    if (!source.IsValid() || !fpsMilli) {
        return false;
    }
    const long double sourceFps = static_cast<long double>(source.numerator)
        / source.denominator;
    const long long multiplier = std::llround((fpsMilli / 1000.0L) / sourceFps);
    if (multiplier < 1 || multiplier > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    const FrameRate aligned = ScaleRate(source, static_cast<uint32_t>(multiplier));
    if (!aligned.IsValid()) {
        return false;
    }
    const long double alignedMilli = static_cast<long double>(aligned.numerator)
        * 1000.0L / aligned.denominator;
    return std::abs(alignedMilli - fpsMilli) <= 0.5L;
}

FrameRate RaiseRequestedRateToNearbyWholeMultiplier(
    const FrameRate sourceRate, const FrameRate requestedRate) noexcept
{
    const FrameRate source = SnapSourceRate(sourceRate);
    const FrameRate requested = ReduceRate(requestedRate);
    if (!source.IsValid() || !requested.IsValid()) {
        return requested;
    }

    // Do not turn a request at/below the source rate into an interpolation
    // request. Above source rate, allow a small upward correction to an exact
    // source multiplier for nominal-rate mismatches such as 60.5 -> 120.
    const uint64_t ratioNumerator = static_cast<uint64_t>(requested.numerator) * source.denominator;
    const uint64_t ratioDenominator = static_cast<uint64_t>(requested.denominator) * source.numerator;
    if (ratioNumerator <= ratioDenominator) {
        return requested;
    }

    const uint64_t roundedMultiplier = (ratioNumerator + ratioDenominator / 2) / ratioDenominator;
    if (roundedMultiplier <= 1 || roundedMultiplier > std::numeric_limits<uint32_t>::max()) {
        return requested;
    }

    const FrameRate aligned = ScaleRate(source, static_cast<uint32_t>(roundedMultiplier));
    if (!aligned.IsValid()) {
        return requested;
    }

    // Normalize small nominal-rate mismatches without silently lowering a
    // deliberately requested rate. In particular, 60 -> 121 must remain 121
    // until measured load proves that it is unsustainable. One percent is wide
    // enough for normal nominal-rate rounding while preserving intentional
    // fractional targets such as 24 -> 60 (2.5x).
    const long double requestedFps = static_cast<long double>(requested.numerator)
        / requested.denominator;
    const long double alignedFps = static_cast<long double>(aligned.numerator)
        / aligned.denominator;
    if (alignedFps > requestedFps
            && (alignedFps - requestedFps) / requestedFps <= 0.01L) {
        return aligned;
    }
    return requested;
}

long double EstimateSyntheticFps(const FrameRate sourceRate, const FrameRate outputRate) noexcept
{
    const FrameRate source = SnapSourceRate(sourceRate);
    const FrameRate output = ReduceRate(outputRate);
    if (!source.IsValid() || !output.IsValid()) {
        return 0.0L;
    }

    const long double sourceFps = static_cast<long double>(source.numerator) / source.denominator;
    const long double outputFps = static_cast<long double>(output.numerator) / output.denominator;
    if (outputFps <= sourceFps) {
        return 0.0L;
    }

    const uint64_t ratioNumerator = static_cast<uint64_t>(output.numerator) * source.denominator;
    const uint64_t ratioDenominator = static_cast<uint64_t>(output.denominator) * source.numerator;
    const uint64_t ratioDivisor = std::gcd(ratioNumerator, ratioDenominator);
    const uint64_t sourceReusePeriod = ratioDivisor ? ratioDenominator / ratioDivisor : 1;
    const long double reusableSourceFps = sourceFps / std::max<uint64_t>(1, sourceReusePeriod);
    return std::max<long double>(0.0L, outputFps - reusableSourceFps);
}

bool SameRate(FrameRate first, FrameRate second) noexcept
{
    first = ReduceRate(first);
    second = ReduceRate(second);
    return first.numerator == second.numerator && first.denominator == second.denominator;
}

} // namespace

void CFrameInterpolationPressureController::ResetDeliveryMeasurement() noexcept
{
    m_deliveryWindowStartMs = 0;
    m_deliveryWindowStartFrames = 0;
    m_measuredOutputFpsMilli = 0;
    m_deliveryWindowActive = false;
    m_measuredOutputReady = false;
    m_measuredOutputHealthy = false;
}

void CFrameInterpolationPressureController::Reset() noexcept
{
    m_previousSnapshot = {};
    m_havePreviousSnapshot = false;
    m_requestedOutputFpsMilli = 0;
    m_sourceRate = {};
    m_outputCapFpsMilli = 0;
    m_lastKnownGoodFpsMilli = 0;
    m_lastKnownBadFpsMilli = 0;
    m_failedAlignedFpsMilli = 0;
    m_backoffStepFpsMilli = 0;
    m_probingAligned = false;
    m_failedAlignedRetryAfterMs = 0;
    m_phaseStartMs = 0;
    m_pressureEvidenceStartMs = 0;
    m_pressureEvidenceCount = 0;
    m_pressureEvidenceReasons = RIFE_PRESSURE_NONE;
    ResetDeliveryMeasurement();
    m_phase = FrameInterpolationPressurePhase::Open;
}

void CFrameInterpolationPressureController::Resume(const uint64_t nowMs) noexcept
{
    m_previousSnapshot = {};
    m_havePreviousSnapshot = false;
    m_pressureEvidenceStartMs = 0;
    m_pressureEvidenceCount = 0;
    m_pressureEvidenceReasons = RIFE_PRESSURE_NONE;
    ResetDeliveryMeasurement();
    m_probingAligned = false;
    // A probe was never proven sustainable if playback was paused midway.
    if (m_phase == FrameInterpolationPressurePhase::Probe && m_lastKnownGoodFpsMilli) {
        m_outputCapFpsMilli = m_lastKnownGoodFpsMilli;
    }
    m_phase = m_outputCapFpsMilli
        ? FrameInterpolationPressurePhase::Settling : FrameInterpolationPressurePhase::Open;
    m_phaseStartMs = nowMs;
}

FrameInterpolationPressureDecision CFrameInterpolationPressureController::Update(
    const uint32_t requestedOutputFpsMilli,
    const uint32_t sourceFpsMilli,
    const uint64_t nowMs,
    const FrameInterpolationPressureSnapshot& snapshot) noexcept
{
    return Update(requestedOutputFpsMilli, FrameRate{sourceFpsMilli, 1000}, nowMs, snapshot);
}

FrameInterpolationPressureDecision CFrameInterpolationPressureController::Update(
    const uint32_t requestedOutputFpsMilli,
    const FrameRate inputSourceRate,
    const uint64_t nowMs,
    const FrameInterpolationPressureSnapshot& snapshot) noexcept
{
    FrameInterpolationPressureDecision decision;
    const FrameRate sourceRate = SnapSourceRate(inputSourceRate);
    const uint32_t sourceFpsMilli = sourceRate.IsValid()
        ? static_cast<uint32_t>((static_cast<uint64_t>(sourceRate.numerator) * 1000
            + sourceRate.denominator / 2) / sourceRate.denominator)
        : 0;
    if (!requestedOutputFpsMilli || !sourceFpsMilli
            || requestedOutputFpsMilli <= sourceFpsMilli) {
        Reset();
        return decision;
    }

    if (requestedOutputFpsMilli != m_requestedOutputFpsMilli
            || !SameRate(sourceRate, m_sourceRate)) {
        Reset();
        m_previousSnapshot = snapshot;
        m_havePreviousSnapshot = true;
        m_requestedOutputFpsMilli = requestedOutputFpsMilli;
        m_sourceRate = sourceRate;
        return decision;
    }
    if (!m_havePreviousSnapshot) {
        m_previousSnapshot = snapshot;
        m_havePreviousSnapshot = true;
        decision.outputCapFpsMilli = m_outputCapFpsMilli;
        decision.phase = m_phase;
        decision.lastKnownGoodFpsMilli = m_lastKnownGoodFpsMilli;
        decision.lastKnownBadFpsMilli = m_lastKnownBadFpsMilli;
        decision.backoffStepFpsMilli = m_backoffStepFpsMilli;
        return decision;
    }

    const auto delta = [](const uint64_t current, const uint64_t previous) noexcept {
        return current >= previous ? current - previous : current;
    };
    const uint64_t sourcePoolMissDelta = delta(
        snapshot.sourcePoolMisses, m_previousSnapshot.sourcePoolMisses);
    const uint64_t lateDropDelta = delta(
        snapshot.lateSyntheticDrops, m_previousSnapshot.lateSyntheticDrops);
    const uint64_t presentationDropDelta = delta(
        snapshot.presentationDrops, m_previousSnapshot.presentationDrops);
    const uint64_t presentationReclaimDelta = delta(
        snapshot.presentationReclaims, m_previousSnapshot.presentationReclaims);
    const uint64_t presenterStaleDropDelta = delta(
        snapshot.presenterStaleDrops, m_previousSnapshot.presenterStaleDrops);
    const uint64_t surfaceWaitUsDelta = delta(
        snapshot.presentationSurfaceWaitUs, m_previousSnapshot.presentationSurfaceWaitUs);
    const uint64_t surfaceWaitCountDelta = delta(
        snapshot.presentationSurfaceWaitCount, m_previousSnapshot.presentationSurfaceWaitCount);
    m_previousSnapshot = snapshot;

    constexpr uint64_t DeliveryWarmupMs = 1'000;
    constexpr uint64_t DeliveryWindowMs = 2'000;
    constexpr uint32_t DeliverySuccessPercent = 97;
    const uint32_t currentTargetFpsMilli = m_outputCapFpsMilli
        ? m_outputCapFpsMilli : requestedOutputFpsMilli;
    bool deliveryShortfall = false;
    if (snapshot.presenterDeliveryAvailable
            && nowMs >= m_phaseStartMs
            && nowMs - m_phaseStartMs >= DeliveryWarmupMs) {
        if (!m_deliveryWindowActive
                || snapshot.presenterRenderedFrames < m_deliveryWindowStartFrames) {
            m_deliveryWindowStartMs = nowMs;
            m_deliveryWindowStartFrames = snapshot.presenterRenderedFrames;
            m_deliveryWindowActive = true;
        } else if (nowMs - m_deliveryWindowStartMs >= DeliveryWindowMs) {
            const uint64_t elapsedMs = nowMs - m_deliveryWindowStartMs;
            const uint64_t frames = snapshot.presenterRenderedFrames - m_deliveryWindowStartFrames;
            const uint64_t measuredMilli = frames > std::numeric_limits<uint64_t>::max() / 1'000'000ULL
                ? std::numeric_limits<uint64_t>::max() : frames * 1'000'000ULL / elapsedMs;
            m_measuredOutputFpsMilli = static_cast<uint32_t>(std::min<uint64_t>(
                measuredMilli, std::numeric_limits<uint32_t>::max()));
            m_measuredOutputReady = true;
            m_measuredOutputHealthy = static_cast<uint64_t>(m_measuredOutputFpsMilli) * 100
                >= static_cast<uint64_t>(currentTargetFpsMilli) * DeliverySuccessPercent;
            deliveryShortfall = !m_measuredOutputHealthy;
            m_deliveryWindowStartMs = nowMs;
            m_deliveryWindowStartFrames = snapshot.presenterRenderedFrames;
        }
    }

    constexpr uint64_t SurfaceWaitPressureUs = 2'000;
    const bool surfaceWaitPressure = surfaceWaitCountDelta
        && surfaceWaitUsDelta / surfaceWaitCountDelta >= SurfaceWaitPressureUs;
    const uint32_t contexts = std::max(1u, snapshot.inferenceContexts);
    const bool backlogPressure = snapshot.sourceQueueDepth >= 4
        || (snapshot.sourceQueueDepth >= 2 && snapshot.pendingPairDepth >= contexts - 1);

    uint32_t observedPressureReasons = RIFE_PRESSURE_NONE;
    if (sourcePoolMissDelta) {
        observedPressureReasons |= RIFE_PRESSURE_SOURCE_POOL_MISS;
    }
    if (lateDropDelta) {
        observedPressureReasons |= RIFE_PRESSURE_LATE_SYNTHETIC_DROP;
    }
    if (presentationDropDelta) {
        observedPressureReasons |= RIFE_PRESSURE_PRESENTATION_DROP;
    }
    if (presentationReclaimDelta) {
        observedPressureReasons |= RIFE_PRESSURE_PRESENTATION_RECLAIM;
    }
    if (presenterStaleDropDelta) {
        observedPressureReasons |= RIFE_PRESSURE_PRESENTER_STALE_DROP;
    }
    if (backlogPressure) {
        observedPressureReasons |= RIFE_PRESSURE_BACKLOG;
    }
    if (surfaceWaitPressure) {
        observedPressureReasons |= RIFE_PRESSURE_PRESENTATION_SURFACE_WAIT;
    }
    if (deliveryShortfall) {
        observedPressureReasons |= RIFE_PRESSURE_DELIVERY_SHORTFALL;
    }
    decision.observedPressureReasons = observedPressureReasons;

    constexpr uint64_t PressureConfirmWindowMs = 750;
    constexpr uint32_t PressureConfirmSamples = 2;
    constexpr uint64_t SettleWindowMs = 1'000;

    // A presentation-surface wait means the producer got ahead of the presenter,
    // not that interpolation throughput is unsustainable. Real cap changes need
    // repeated end-to-end distress. Two observations inside this window reject
    // isolated timer/OS scheduling hiccups while confirming sustained overload
    // within a couple of source-frame updates.
    uint32_t capDrivingReasons = observedPressureReasons
        & ~RIFE_PRESSURE_PRESENTATION_SURFACE_WAIT;
    if (snapshot.presenterDeliveryAvailable
            && m_phase == FrameInterpolationPressurePhase::Stable
            && m_lastKnownGoodFpsMilli == currentTargetFpsMilli) {
        // A verified cadence can survive isolated drops. Demote it only when
        // the presenter actually falls short over a complete delivery window.
        capDrivingReasons &= RIFE_PRESSURE_DELIVERY_SHORTFALL;
    }
    bool pressure = false;
    const bool transitionGrace = (m_phase == FrameInterpolationPressurePhase::Settling
            || m_phase == FrameInterpolationPressurePhase::Probe)
        && nowMs >= m_phaseStartMs
        && nowMs - m_phaseStartMs < SettleWindowMs;
    if (transitionGrace) {
        // Drops and queue depth collected while a changed output rate is taking
        // effect belong to the old schedule. Re-baseline them before judging
        // the new cap so a recovery probe is not rejected by stale work.
        m_pressureEvidenceStartMs = 0;
        m_pressureEvidenceCount = 0;
        m_pressureEvidenceReasons = RIFE_PRESSURE_NONE;
    } else if (capDrivingReasons) {
        if (!m_pressureEvidenceStartMs
                || nowMs - m_pressureEvidenceStartMs > PressureConfirmWindowMs) {
            m_pressureEvidenceStartMs = nowMs;
            m_pressureEvidenceCount = 1;
            m_pressureEvidenceReasons = capDrivingReasons;
        } else {
            ++m_pressureEvidenceCount;
            m_pressureEvidenceReasons |= capDrivingReasons;
        }
        if (m_pressureEvidenceCount >= PressureConfirmSamples) {
            pressure = true;
            decision.confirmedPressureReasons = m_pressureEvidenceReasons;
        }
    } else if (m_pressureEvidenceStartMs
            && nowMs - m_pressureEvidenceStartMs > PressureConfirmWindowMs) {
        m_pressureEvidenceStartMs = 0;
        m_pressureEvidenceCount = 0;
        m_pressureEvidenceReasons = RIFE_PRESSURE_NONE;
    }
    if (deliveryShortfall && !transitionGrace) {
        // One full output window is stronger evidence than two error-counter
        // observations. It also catches silent under-delivery with zero drops.
        pressure = true;
        decision.confirmedPressureReasons |= RIFE_PRESSURE_DELIVERY_SHORTFALL;
    }
    decision.pressureDetected = pressure;

    constexpr uint32_t InitialBackoffFpsMilli = 2'000;
    constexpr uint32_t MaximumBackoffFpsMilli = 32'000;
    constexpr uint64_t RecoveryDelayMs = 500;
    constexpr uint64_t FailedAlignedRetryDelayMs = 30'000;

    const auto effectiveCap = [&]() noexcept {
        return m_outputCapFpsMilli ? m_outputCapFpsMilli : requestedOutputFpsMilli;
    };
    const auto finish = [&]() noexcept {
        decision.outputCapFpsMilli = m_outputCapFpsMilli;
        decision.phase = m_phase;
        decision.lastKnownGoodFpsMilli = m_lastKnownGoodFpsMilli;
        decision.lastKnownBadFpsMilli = m_lastKnownBadFpsMilli;
        decision.backoffStepFpsMilli = m_backoffStepFpsMilli;
        decision.measuredOutputFpsMilli = m_measuredOutputFpsMilli;
        decision.measuredOutputReady = m_measuredOutputReady;
        decision.measuredOutputHealthy = m_measuredOutputHealthy;
        return decision;
    };
    const auto clearPressureEvidence = [&]() noexcept {
        m_pressureEvidenceStartMs = 0;
        m_pressureEvidenceCount = 0;
        m_pressureEvidenceReasons = RIFE_PRESSURE_NONE;
    };
    const auto beginBackoff = [&](const uint32_t stepFpsMilli) noexcept {
        const uint32_t current = effectiveCap();
        if (current > sourceFpsMilli) {
            m_lastKnownBadFpsMilli = m_lastKnownBadFpsMilli
                ? std::min(m_lastKnownBadFpsMilli, current) : current;
        }
        uint32_t nextCap = current > sourceFpsMilli + stepFpsMilli
            ? current - stepFpsMilli : sourceFpsMilli;
        const long double stepSyntheticFps = EstimateSyntheticFps(
            sourceRate, RestoreSourceAlignedRate(sourceRate, nextCap));
        {
            // A small cut can increase inference work by losing source
            // endpoints. Prefer a cheaper exact source multiple whenever it
            // reduces synthetic work, even when a larger step would land just
            // below that multiple.
            uint64_t multiplier = (static_cast<uint64_t>(current) * sourceRate.denominator)
                / (static_cast<uint64_t>(sourceRate.numerator) * 1000);
            while (multiplier >= 1) {
                const FrameRate aligned = ScaleRate(sourceRate, static_cast<uint32_t>(multiplier));
                if (aligned.IsValid()) {
                    const uint64_t alignedMilli = (static_cast<uint64_t>(aligned.numerator) * 1000
                        + aligned.denominator / 2) / aligned.denominator;
                    if (alignedMilli < current && alignedMilli >= sourceFpsMilli
                            && (multiplier > 1 || nextCap == sourceFpsMilli)) {
                        const long double alignedSyntheticFps = EstimateSyntheticFps(
                            sourceRate, aligned);
                        if (alignedSyntheticFps + 1.0e-9L < stepSyntheticFps) {
                            nextCap = static_cast<uint32_t>(alignedMilli);
                        }
                        break;
                    }
                }
                --multiplier;
            }
        }
        if (nextCap != m_outputCapFpsMilli) {
            m_outputCapFpsMilli = nextCap;
            decision.changed = true;
        }
        ResetDeliveryMeasurement();
        m_backoffStepFpsMilli = std::min(MaximumBackoffFpsMilli,
            std::max(InitialBackoffFpsMilli, stepFpsMilli) * 2u);
        m_phase = FrameInterpolationPressurePhase::Settling;
        m_phaseStartMs = nowMs;
        clearPressureEvidence();
    };

    if (m_phase == FrameInterpolationPressurePhase::Open) {
        if (pressure) {
            m_backoffStepFpsMilli = InitialBackoffFpsMilli;
            beginBackoff(InitialBackoffFpsMilli);
        }
        return finish();
    }

    if (m_phase == FrameInterpolationPressurePhase::Probe) {
        if (pressure) {
            const uint32_t failedProbe = effectiveCap();
            if (m_probingAligned) {
                m_failedAlignedFpsMilli = m_failedAlignedFpsMilli
                    ? std::min(m_failedAlignedFpsMilli, failedProbe) : failedProbe;
                m_failedAlignedRetryAfterMs = nowMs + FailedAlignedRetryDelayMs;
            }
            m_probingAligned = false;
            m_lastKnownBadFpsMilli = m_lastKnownBadFpsMilli
                ? std::min(m_lastKnownBadFpsMilli, failedProbe) : failedProbe;
            const uint32_t fallback = std::max(sourceFpsMilli,
                m_lastKnownGoodFpsMilli ? m_lastKnownGoodFpsMilli : sourceFpsMilli);
            if (m_outputCapFpsMilli != fallback) {
                m_outputCapFpsMilli = fallback;
                decision.changed = true;
            }
            ResetDeliveryMeasurement();
            m_backoffStepFpsMilli = InitialBackoffFpsMilli;
            m_phase = FrameInterpolationPressurePhase::Settling;
            m_phaseStartMs = nowMs;
            clearPressureEvidence();
            return finish();
        }
        if (nowMs - m_phaseStartMs < SettleWindowMs) {
            return finish();
        }
        if (m_pressureEvidenceCount
                && nowMs - m_pressureEvidenceStartMs <= PressureConfirmWindowMs) {
            // Let post-settle evidence either confirm as sustained pressure or
            // age out before accepting or rejecting the probe.
            return finish();
        }

        if (snapshot.presenterDeliveryAvailable && !m_measuredOutputReady) {
            return finish();
        }
        m_lastKnownGoodFpsMilli = effectiveCap();
        m_probingAligned = false;
        if (m_failedAlignedFpsMilli
                && m_lastKnownGoodFpsMilli >= m_failedAlignedFpsMilli) {
            m_failedAlignedFpsMilli = 0;
            m_failedAlignedRetryAfterMs = 0;
        }
        if (m_lastKnownBadFpsMilli
                && m_lastKnownBadFpsMilli <= m_lastKnownGoodFpsMilli) {
            // A cheaper aligned rate can succeed above an earlier failed
            // arbitrary rate. That old FPS bound no longer brackets recovery.
            m_lastKnownBadFpsMilli = requestedOutputFpsMilli;
        }
        m_backoffStepFpsMilli = InitialBackoffFpsMilli;
        if (m_lastKnownGoodFpsMilli >= requestedOutputFpsMilli) {
            m_outputCapFpsMilli = 0;
            m_lastKnownBadFpsMilli = 0;
            m_phase = FrameInterpolationPressurePhase::Open;
        } else {
            m_phase = FrameInterpolationPressurePhase::Stable;
        }
        m_phaseStartMs = nowMs;
        return finish();
    }

    if (m_phase == FrameInterpolationPressurePhase::Settling) {
        if (nowMs - m_phaseStartMs < SettleWindowMs) {
            return finish();
        }

        if (pressure) {
            beginBackoff(m_backoffStepFpsMilli
                ? m_backoffStepFpsMilli : InitialBackoffFpsMilli);
            return finish();
        }
        if (m_pressureEvidenceCount
                && nowMs - m_pressureEvidenceStartMs <= PressureConfirmWindowMs) {
            // One cap-driving observation is pending confirmation. Do not call
            // this rate stable until the second sample arrives or the evidence
            // ages out; otherwise repeated overload gets misclassified as a
            // sequence of unrelated 2 fps events.
            return finish();
        }
        if (snapshot.presenterDeliveryAvailable && !m_measuredOutputReady) {
            return finish();
        }
        m_lastKnownGoodFpsMilli = effectiveCap();
        m_backoffStepFpsMilli = InitialBackoffFpsMilli;
        m_phase = FrameInterpolationPressurePhase::Stable;
        m_phaseStartMs = nowMs;
        return finish();
    }

    // Stable: a fresh pressure event establishes a new upper bound. Recovery
    // probes exact source multiples first because they reuse every source
    // endpoint. When an aligned probe has failed, keep the last known-good cap
    // until its retry cooldown ends instead of probing arbitrary rates between
    // the good and failed multiples.
    if (pressure) {
        const uint32_t current = effectiveCap();
        if (m_lastKnownGoodFpsMilli == current
                && IsSourceAlignedRateMilli(sourceRate, current)) {
            // This exact cadence was known-good, but sustained live pressure
            // has disproved it. Avoid immediately probing it again; retry after
            // the cooldown in case shared GPU load later changes.
            m_failedAlignedFpsMilli = m_failedAlignedFpsMilli
                ? std::min(m_failedAlignedFpsMilli, current) : current;
            m_failedAlignedRetryAfterMs = nowMs + FailedAlignedRetryDelayMs;
        }
        m_lastKnownBadFpsMilli = current;
        m_backoffStepFpsMilli = InitialBackoffFpsMilli;
        beginBackoff(InitialBackoffFpsMilli);
        return finish();
    }

    if (nowMs - m_phaseStartMs >= (snapshot.presenterDeliveryAvailable ? 5'000 : RecoveryDelayMs)
            && m_lastKnownGoodFpsMilli < requestedOutputFpsMilli) {
        if (m_failedAlignedFpsMilli && nowMs >= m_failedAlignedRetryAfterMs) {
            // Load can change without a source or setting change (for example,
            // when another GPU job ends). Retry an aligned failure occasionally.
            m_failedAlignedFpsMilli = 0;
            m_failedAlignedRetryAfterMs = 0;
        }
        uint32_t alignedProbe = 0;
        // Recover an exact rational for a stored milli-fps cap before finding
        // the next source multiple. A rounded 89.910 fps cap for 30000/1001
        // content is microscopically below 3x; integer division on the rounded
        // value would otherwise select 3x again and leave the controller in a
        // no-op probe forever.
        const FrameRate knownGoodRate = RestoreSourceAlignedRate(
            sourceRate, m_lastKnownGoodFpsMilli);
        const uint64_t goodRateNumerator = static_cast<uint64_t>(knownGoodRate.numerator)
            * sourceRate.denominator;
        const uint64_t goodRateDenominator = static_cast<uint64_t>(knownGoodRate.denominator)
            * sourceRate.numerator;
        const uint64_t nextMultiplier = std::max<uint64_t>(2,
            goodRateDenominator ? goodRateNumerator / goodRateDenominator + 1 : 2);
        const FrameRate alignedRate = nextMultiplier <= std::numeric_limits<uint32_t>::max()
            ? ScaleRate(sourceRate, static_cast<uint32_t>(nextMultiplier)) : FrameRate{};
        const uint64_t nextAligned = alignedRate.IsValid()
            ? (static_cast<uint64_t>(alignedRate.numerator) * 1000
                + alignedRate.denominator / 2) / alignedRate.denominator
            : std::numeric_limits<uint64_t>::max();
        if (nextAligned <= requestedOutputFpsMilli
                && (!m_failedAlignedFpsMilli || nextAligned < m_failedAlignedFpsMilli)) {
            alignedProbe = static_cast<uint32_t>(nextAligned);
            if (m_lastKnownBadFpsMilli
                    && alignedProbe > m_lastKnownBadFpsMilli) {
                // A failed lower output rate only rules out this candidate
                // when it also needed strictly fewer synthetic frames.
                const long double badSyntheticFps = EstimateSyntheticFps(
                    sourceRate, RestoreSourceAlignedRate(sourceRate, m_lastKnownBadFpsMilli));
                const long double alignedSyntheticFps = EstimateSyntheticFps(
                    sourceRate, alignedRate);
                if (badSyntheticFps + 1.0e-9L < alignedSyntheticFps) {
                    alignedProbe = 0;
                }
            }
        }
        uint32_t probe = alignedProbe;
        if (!probe) {
            if (m_failedAlignedFpsMilli
                    && nowMs < m_failedAlignedRetryAfterMs) {
                // An intermediate cap can schedule a non-integral source
                // cadence and perform worse than the known-good aligned rate.
                // Hold the known-good cadence until the failed aligned rate is
                // eligible for a deliberate retry.
                m_phaseStartMs = nowMs;
                return finish();
            }
            probe = requestedOutputFpsMilli;
            if (m_lastKnownBadFpsMilli > m_lastKnownGoodFpsMilli) {
                const uint32_t gap = m_lastKnownBadFpsMilli - m_lastKnownGoodFpsMilli;
                probe = m_lastKnownGoodFpsMilli + (gap + 1u) / 2u;
            }
        }
        probe = std::clamp(probe, sourceFpsMilli, requestedOutputFpsMilli);
        const uint32_t nextCap = probe >= requestedOutputFpsMilli ? 0 : probe;
        if (nextCap != m_outputCapFpsMilli) {
            m_outputCapFpsMilli = nextCap;
            decision.changed = true;
        }
        ResetDeliveryMeasurement();
        m_phase = FrameInterpolationPressurePhase::Probe;
        m_probingAligned = alignedProbe != 0;
        m_phaseStartMs = nowMs;
        clearPressureEvidence();
    }

    return finish();
}

FrameInterpolationLoadEstimate EstimateFrameInterpolationLoad(
    const FrameRate sourceRate,
    const FrameRate requestedOutputRate,
    const uint32_t contextCount,
    const uint64_t averageInferenceWallUs,
    const uint32_t sampleCount,
    const uint32_t headroomPermille) noexcept
{
    FrameInterpolationLoadEstimate estimate;
    if (!sourceRate.IsValid() || !requestedOutputRate.IsValid()) {
        return estimate;
    }

    const FrameRate snappedSourceRate = SnapSourceRate(sourceRate);
    const FrameRate reducedRequestedRate = ReduceRate(requestedOutputRate);
    const long double sourceFps = static_cast<long double>(snappedSourceRate.numerator)
        / snappedSourceRate.denominator;
    const long double requestedFps = static_cast<long double>(reducedRequestedRate.numerator)
        / reducedRequestedRate.denominator;
    estimate.requestedOutputFpsMilli = static_cast<uint32_t>(std::clamp(
        std::llround(requestedFps * 1000.0L), 0LL,
        static_cast<long long>(std::numeric_limits<uint32_t>::max())));

    constexpr uint32_t MinimumSamples = 8;
    if (requestedFps <= sourceFps || contextCount == 0 || averageInferenceWallUs == 0
            || sampleCount < MinimumSamples || headroomPermille == 0) {
        return estimate;
    }

    const long double sustainableSyntheticFps =
        static_cast<long double>(contextCount) * 1'000'000.0L / averageInferenceWallUs
        * std::min(headroomPermille, 1000u) / 1000.0L;
    estimate.sustainableSyntheticFpsMilli = static_cast<uint32_t>(std::clamp(
        std::llround(sustainableSyntheticFps * 1000.0L), 0LL,
        static_cast<long long>(std::numeric_limits<uint32_t>::max())));

    // The old estimate used requestedFps - sourceFps, which is only correct for
    // whole multipliers. Example: 60 -> 120 needs about 60 inferences/s, while
    // 60 -> 121 needs about 120 because almost none of the source endpoints land
    // on the 121 fps grid.
    const long double requestedSyntheticFps = EstimateSyntheticFps(
        snappedSourceRate, reducedRequestedRate);
    if (requestedSyntheticFps <= sustainableSyntheticFps) {
        return estimate;
    }

    // Find the highest lower output rate whose actual synthetic workload fits
    // measured capacity. Arbitrary rates are allowed when affordable; aligned
    // source multiples simply emerge as especially cheap candidates because
    // they can reuse every real source endpoint.
    FrameRate bestRate = snappedSourceRate;
    long double bestFps = sourceFps;
    const auto considerCandidate = [&](FrameRate candidate) {
        candidate = ReduceRate(candidate);
        if (!candidate.IsValid()) {
            return;
        }
        const long double candidateFps = static_cast<long double>(candidate.numerator)
            / candidate.denominator;
        constexpr long double Epsilon = 1.0e-9L;
        if (candidateFps <= sourceFps || candidateFps >= requestedFps - Epsilon
                || candidateFps <= bestFps + Epsilon) {
            return;
        }
        if (EstimateSyntheticFps(snappedSourceRate, candidate)
                <= sustainableSyntheticFps + Epsilon) {
            bestRate = candidate;
            bestFps = candidateFps;
        }
    };

    // Any candidate above source+sustainableSynthetic cannot fit even with
    // perfect endpoint reuse, so begin the integer search at that bound.
    const long double highestPossibleFps = std::min(
        requestedFps - 1.0e-9L, sourceFps + sustainableSyntheticFps);
    const uint32_t highestIntegerFps = static_cast<uint32_t>(std::clamp<long double>(
        std::floor(highestPossibleFps), 0.0L,
        static_cast<long double>(std::numeric_limits<uint32_t>::max())));
    for (uint32_t fps = highestIntegerFps; fps > 0; --fps) {
        if (static_cast<long double>(fps) <= sourceFps) {
            break;
        }
        considerCandidate({fps, 1});
        if (bestFps == static_cast<long double>(fps)) {
            break;
        }
    }

    // Integer custom/fixed targets cannot represent NTSC-aligned caps exactly,
    // so also test exact whole source multiples and keep whichever fitting rate
    // is highest.
    const uint32_t highestWholeMultiplier = static_cast<uint32_t>(std::clamp<long double>(
        std::floor(requestedFps / sourceFps + 1.0e-9L), 1.0L,
        static_cast<long double>(std::numeric_limits<uint32_t>::max())));
    for (uint32_t multiplier = 2; multiplier <= highestWholeMultiplier; ++multiplier) {
        considerCandidate(ScaleRate(snappedSourceRate, multiplier));
    }

    const long long roundedCapMilli = std::llround(bestFps * 1000.0L);
    uint64_t capMilli = static_cast<uint64_t>(std::max<long long>(0LL, roundedCapMilli));
    capMilli = std::min<uint64_t>(capMilli, std::numeric_limits<uint32_t>::max());
    if (capMilli != estimate.requestedOutputFpsMilli) {
        estimate.outputCapFpsMilli = static_cast<uint32_t>(capMilli);
    }
    return estimate;
}

void CFrameInterpolationScheduler::Reset() noexcept
{
    m_activeTargetRate = {};
    m_anchorTime = 0;
    m_nextTargetIndex = 1;
    m_anchored = false;
}

void CFrameInterpolationScheduler::Configure(
    const FrameInterpolationRateMode mode,
    const FrameRate customRate,
    const FrameRate displayRate,
    const uint32_t maxMultiplierMilli,
    const uint32_t maxOutputFpsMilli) noexcept
{
    m_mode = mode;
    m_customRate = ReduceRate(customRate);
    m_displayRate = ReduceRate(displayRate);
    m_maxMultiplierMilli = maxMultiplierMilli;
    m_maxOutputFpsMilli = maxOutputFpsMilli;
    m_runtimeMaxOutputFpsMilli = 0;
    Reset();
}

void CFrameInterpolationScheduler::SetRuntimeOutputFpsCap(const uint32_t maxOutputFpsMilli) noexcept
{
    m_runtimeMaxOutputFpsMilli = maxOutputFpsMilli;
}

FrameRate CFrameInterpolationScheduler::ResolveRequestedRate(const FrameRate sourceRate) const noexcept
{
    const FrameRate multiplierSourceRate = SnapSourceRate(sourceRate);
    switch (m_mode) {
    case FrameInterpolationRateMode::ToScreen:
        return ReduceRate(m_displayRate);
    case FrameInterpolationRateMode::Movie2x:
        return ScaleRate(multiplierSourceRate, 2);
    case FrameInterpolationRateMode::Movie2_5x:
        return ScaleRate(multiplierSourceRate, 5, 2);
    case FrameInterpolationRateMode::Movie3x:
        return ScaleRate(multiplierSourceRate, 3);
    case FrameInterpolationRateMode::Movie4x:
        return ScaleRate(multiplierSourceRate, 4);
    case FrameInterpolationRateMode::Movie5x:
        return ScaleRate(multiplierSourceRate, 5);
    case FrameInterpolationRateMode::Fixed60:
        return RaiseRequestedRateToNearbyWholeMultiplier(multiplierSourceRate, {60, 1});
    case FrameInterpolationRateMode::Fixed72:
        return RaiseRequestedRateToNearbyWholeMultiplier(multiplierSourceRate, {72, 1});
    case FrameInterpolationRateMode::Fixed90:
        return RaiseRequestedRateToNearbyWholeMultiplier(multiplierSourceRate, {90, 1});
    case FrameInterpolationRateMode::Fixed120:
        return RaiseRequestedRateToNearbyWholeMultiplier(multiplierSourceRate, {120, 1});
    case FrameInterpolationRateMode::Custom:
        return RaiseRequestedRateToNearbyWholeMultiplier(multiplierSourceRate, m_customRate);
    case FrameInterpolationRateMode::Disabled:
    default:
        return {};
    }
}

FrameRate CFrameInterpolationScheduler::ResolveConfiguredTargetRate(const FrameRate sourceRate) const noexcept
{
    FrameRate rate = ResolveRequestedRate(sourceRate);
    const auto capRate = [&](FrameRate cap) {
        if (cap.IsValid() && static_cast<uint64_t>(cap.numerator) * rate.denominator
                < static_cast<uint64_t>(rate.numerator) * cap.denominator) {
            rate = cap;
        }
    };
    if (m_maxMultiplierMilli) {
        // Reduce the multiplier before scaling to preserve rational NTSC rates
        // and avoid unnecessary overflow with 100ns source durations.
        const FrameRate multiplier = ReduceRate({m_maxMultiplierMilli, 1000});
        capRate(ScaleRate(SnapSourceRate(sourceRate), multiplier.numerator, multiplier.denominator));
    }
    if (m_maxOutputFpsMilli) {
        capRate(ReduceRate({m_maxOutputFpsMilli, 1000}));
    }
    return rate;
}

FrameRate CFrameInterpolationScheduler::ResolveTargetRate(const FrameRate sourceRate) const noexcept
{
    FrameRate rate = ResolveConfiguredTargetRate(sourceRate);
    if (rate.IsValid() && m_runtimeMaxOutputFpsMilli) {
        const FrameRate cap = RestoreSourceAlignedRate(sourceRate, m_runtimeMaxOutputFpsMilli);
        if (cap.IsValid() && static_cast<uint64_t>(cap.numerator) * rate.denominator
                < static_cast<uint64_t>(rate.numerator) * cap.denominator) {
            rate = cap;
        }
    }
    return rate;
}

int64_t CFrameInterpolationScheduler::TargetTime(const uint64_t index, const FrameRate rate) const noexcept
{
    if (!m_anchored || !rate.IsValid()) {
        return m_anchorTime;
    }

    const uint64_t periodNumerator = ReferenceTicksPerSecond * rate.denominator;
    if (index > std::numeric_limits<uint64_t>::max() / periodNumerator) {
        return std::numeric_limits<int64_t>::max();
    }

    const uint64_t scaled = index * periodNumerator;
    const uint64_t roundedDelta = (scaled + rate.numerator / 2ULL) / rate.numerator;
    if (roundedDelta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::max();
    }
    if (m_anchorTime > std::numeric_limits<int64_t>::max() - static_cast<int64_t>(roundedDelta)) {
        return std::numeric_limits<int64_t>::max();
    }

    return m_anchorTime + static_cast<int64_t>(roundedDelta);
}

std::vector<FrameInterpolationTarget> CFrameInterpolationScheduler::Schedule(
    const int64_t firstTime,
    const int64_t secondTime,
    const FrameRate sourceRate)
{
    std::vector<FrameInterpolationTarget> targets;
    if (secondTime <= firstTime) {
        return targets;
    }

    const FrameRate targetRate = ResolveTargetRate(sourceRate);
    if (!targetRate.IsValid()) {
        return targets;
    }

    if (!m_anchored || !SameRate(targetRate, m_activeTargetRate)) {
        m_anchorTime = firstTime;
        m_nextTargetIndex = 1;
        m_activeTargetRate = targetRate;
        m_anchored = true;
    }

    // Container timestamps can be quantized to milliseconds even when the
    // nominal rate is rational. Treat a target within that precision of B as
    // the real frame, keeping its presentation on the uniform target grid.
    // Otherwise 2x needlessly infers both the midpoint and an almost-identical
    // endpoint. Include targets just beyond B too, so they are not inferred
    // again near t=0 in the following pair. Bound tolerance to a quarter of
    // either interval so genuine interior targets cannot collapse together.
    const int64_t targetPeriod = static_cast<int64_t>(
        ReferenceTicksPerSecond * targetRate.denominator / targetRate.numerator);
    const int64_t endpointTolerance = std::min({int64_t{10'000},
        (secondTime - firstTime) / 4, targetPeriod / 4});

    while (true) {
        const int64_t presentationTime = TargetTime(m_nextTargetIndex, targetRate);
        if (presentationTime == std::numeric_limits<int64_t>::max()) {
            break;
        }

        if (presentationTime <= firstTime) {
            ++m_nextTargetIndex;
            continue;
        }
        if (presentationTime > secondTime && presentationTime - secondTime > endpointTolerance) {
            break;
        }
        if (std::abs(presentationTime - secondTime) <= endpointTolerance) {
            targets.push_back({presentationTime, 1.0, true});
            ++m_nextTargetIndex;
            break;
        }

        const double timestep = static_cast<double>(presentationTime - firstTime)
            / static_cast<double>(secondTime - firstTime);
        targets.push_back({presentationTime, std::clamp(timestep, 0.0, 1.0), false});
        ++m_nextTargetIndex;
    }

    return targets;
}
