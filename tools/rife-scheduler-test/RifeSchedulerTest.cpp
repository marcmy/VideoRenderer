#include "../../Source/FrameInterpolationScheduler.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}

void CheckNear(double actual, double expected, double epsilon, std::string_view message)
{
    if (std::abs(actual - expected) > epsilon) {
        std::cerr << "FAIL: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
        ++g_failures;
    }
}

constexpr FrameRate Rate(uint32_t numerator, uint32_t denominator = 1)
{
    return {numerator, denominator};
}

void Test24To120()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Fixed120, {}, {});

    const auto targets = scheduler.Schedule(0, 416667, Rate(24));
    Check(targets.size() == 5, "24 -> 120 must return four synthetic frames plus the exact source endpoint");
    if (targets.size() == 5) {
        CheckNear(targets[0].timestep, 0.2, 2.0e-5, "24 -> 120 t0");
        CheckNear(targets[1].timestep, 0.4, 2.0e-5, "24 -> 120 t1");
        CheckNear(targets[2].timestep, 0.6, 2.0e-5, "24 -> 120 t2");
        CheckNear(targets[3].timestep, 0.8, 2.0e-5, "24 -> 120 t3");
        Check(!targets[0].exactSource && !targets[1].exactSource
                && !targets[2].exactSource && !targets[3].exactSource,
            "24 -> 120 interior targets are synthetic");
        Check(targets[4].presentationTime == 416667 && targets[4].exactSource,
            "24 -> 120 source endpoint lands on the 120 Hz grid");
    }
}

void Test23976TimesFiveUses11988Grid()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Movie5x, {}, {});

    constexpr int64_t first = 0;
    constexpr int64_t second = 417083; // rounded 1001/24000 s in 100 ns units
    const auto targets = scheduler.Schedule(first, second, Rate(24000, 1001));

    Check(targets.size() == 5, "24000/1001 x5 returns four synthetic frames plus exact source endpoint");
    if (targets.size() == 5) {
        Check(targets[0].presentationTime == 83417, "119.88 grid target 1");
        Check(targets[1].presentationTime == 166833, "119.88 grid target 2");
        Check(targets[2].presentationTime == 250250, "119.88 grid target 3");
        Check(targets[3].presentationTime == 333667, "119.88 grid target 4");
        Check(targets[4].presentationTime == second && targets[4].exactSource,
            "119.88 grid target 5 is the real source endpoint");
    }
}

void TestRoundedNtscRatesUseCanonicalMultiplierGrids()
{
    // DirectShow frame durations are integer 100-ns ticks. A 23.976 or 29.97
    // source therefore commonly reaches the renderer as these rounded rational
    // rates rather than the exact 24000/1001 or 30000/1001 fractions.
    {
        CFrameInterpolationScheduler scheduler;
        scheduler.Configure(FrameInterpolationRateMode::Movie5x, {}, {});
        const auto targets = scheduler.Schedule(0, 417083, Rate(10'000'000, 417083));
        Check(targets.size() == 5, "rounded 23.976 x5 target count");
        if (targets.size() == 5) {
            Check(targets[3].presentationTime == 333667,
                "rounded 23.976 source snaps to canonical 119.88 Hz grid");
        }
    }

    {
        CFrameInterpolationScheduler scheduler;
        scheduler.Configure(FrameInterpolationRateMode::Movie2x, {}, {});
        const auto targets = scheduler.Schedule(0, 333667, Rate(10'000'000, 333667));
        Check(targets.size() == 2, "rounded 29.97 x2 target count");
        if (targets.size() == 2) {
            Check(targets[0].presentationTime == 166833,
                "rounded 29.97 source snaps to canonical 59.94 Hz grid");
        }
    }
}

void Test24To60KeepsUniformGridAcrossPairs()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Fixed60, {}, {});

    const auto firstPair = scheduler.Schedule(0, 416667, Rate(24));
    const auto secondPair = scheduler.Schedule(416667, 833333, Rate(24));

    Check(firstPair.size() == 2, "24 -> 60 first pair target count");
    Check(secondPair.size() == 3, "24 -> 60 second pair includes the coincident source endpoint");
    if (firstPair.size() == 2 && secondPair.size() == 3) {
        Check(firstPair[0].presentationTime == 166667, "60 Hz grid 1");
        Check(firstPair[1].presentationTime == 333333, "60 Hz grid 2");
        Check(secondPair[0].presentationTime == 500000, "60 Hz grid 3");
        Check(secondPair[1].presentationTime == 666667, "60 Hz grid 4");
        Check(secondPair[2].presentationTime == 833333 && secondPair[2].exactSource,
            "60 Hz grid 5 uses the coincident real source frame");
        CheckNear(secondPair[0].timestep, 0.2, 2.0e-5, "24 -> 60 phase wraps to .2");
        CheckNear(secondPair[1].timestep, 0.6, 2.0e-5, "24 -> 60 phase wraps to .6");
    }
}

void TestMovieTwoAndHalfTimes()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Movie2_5x, {}, {});

    const auto firstPair = scheduler.Schedule(0, 416667, Rate(24));
    const auto secondPair = scheduler.Schedule(416667, 833333, Rate(24));

    Check(firstPair.size() == 2, "24 x2.5 first interval has two synthetic targets");
    Check(secondPair.size() == 3, "24 x2.5 second interval ends on the target grid");
    if (firstPair.size() == 2 && secondPair.size() == 3) {
        Check(firstPair[0].presentationTime == 166667, "x2.5 target 1");
        Check(firstPair[1].presentationTime == 333333, "x2.5 target 2");
        Check(secondPair[0].presentationTime == 500000, "x2.5 target 3");
        Check(secondPair[1].presentationTime == 666667, "x2.5 target 4");
        Check(secondPair[2].presentationTime == 833333 && secondPair[2].exactSource,
            "x2.5 target 5 is the real endpoint");
    }
}

void TestToScreenUsesPreciseDisplayRate()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::ToScreen, {}, Rate(60000, 1001));

    const auto targets = scheduler.Schedule(0, 417083, Rate(24000, 1001));
    Check(targets.size() == 2, "23.976 -> 59.94 first interval target count");
    if (targets.size() == 2) {
        Check(targets[0].presentationTime == 166833, "59.94 target 1");
        Check(targets[1].presentationTime == 333667, "59.94 target 2");
    }
}

void TestExactEndpointIsReturnedAsSource()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Fixed60, {}, {});

    const auto pair1 = scheduler.Schedule(0, 500000, Rate(20));
    Check(pair1.size() == 3, "20 -> 60 returns two synthetic frames plus the exact real endpoint");
    if (pair1.size() == 3) {
        Check(pair1[0].presentationTime == 166667 && !pair1[0].exactSource,
            "first 20 -> 60 target is synthetic");
        Check(pair1[1].presentationTime == 333333 && !pair1[1].exactSource,
            "second 20 -> 60 target is synthetic");
        Check(pair1[2].presentationTime == 500000, "exact target-grid endpoint is returned");
        Check(pair1[2].exactSource, "endpoint is tagged as an exact source frame");
        CheckNear(pair1[2].timestep, 1.0, 1.0e-12, "exact source endpoint has t=1");
    }
}

void TestNonGridEndpointRemainsExcluded()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Fixed60, {}, {});

    const auto pair = scheduler.Schedule(0, 416667, Rate(24));
    Check(pair.size() == 2, "non-grid 24 fps source endpoint is not forced onto a 60 Hz grid");
    if (!pair.empty()) {
        Check(pair.back().presentationTime == 333333 && !pair.back().exactSource,
            "last target before a non-grid endpoint remains synthetic");
    }
}

void TestResetReanchorsTimeline()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Fixed60, {}, {});
    (void)scheduler.Schedule(0, 416667, Rate(24));

    scheduler.Reset();
    const auto afterSeek = scheduler.Schedule(10000000, 10416667, Rate(24));
    Check(afterSeek.size() == 2, "reset reanchors target grid after seek");
    if (afterSeek.size() == 2) {
        Check(afterSeek[0].presentationTime == 10166667, "post-seek target 1 is segment-relative");
        Check(afterSeek[1].presentationTime == 10333333, "post-seek target 2 is segment-relative");
    }
}

void TestLongRunDoesNotAccumulateRoundedPeriodDrift()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::ToScreen, {}, Rate(60000, 1001));

    int64_t a = 0;
    for (int i = 0; i < 24000; ++i) {
        const int64_t b = static_cast<int64_t>(std::llround((i + 1) * (10000000.0 * 1001.0 / 24000.0)));
        (void)scheduler.Schedule(a, b, Rate(24000, 1001));
        a = b;
    }

    const auto next = scheduler.Schedule(a,
        static_cast<int64_t>(std::llround(24001.0 * (10000000.0 * 1001.0 / 24000.0))),
        Rate(24000, 1001));

    if (!next.empty()) {
        const auto& target = next.front();
        const double exactIndex = (target.presentationTime / 10000000.0) * (60000.0 / 1001.0);
        CheckNear(exactIndex, std::round(exactIndex), 3.0e-5,
            "long-run target remains on exact rational display grid");
    }
}

void TestTwoTimesWithQuantizedSourceTimestamps()
{
    for (const auto rate : {Rate(60), Rate(60000, 1001), Rate(30000, 1001)}) {
        for (const int64_t quantum : {1LL, 10000LL}) { // 100 ns and millisecond container timebases
            CFrameInterpolationScheduler scheduler;
            scheduler.Configure(FrameInterpolationRateMode::Movie2x, {}, {});
            const double duration = 10'000'000.0 * rate.denominator / rate.numerator;
            int synthetic = 0, source = 0;
            int64_t previousTarget = 0;
            const auto timestamp = [&](int index) {
                return static_cast<int64_t>(std::llround(index * duration / quantum)) * quantum;
            };
            for (int i = 0; i < 600; ++i) {
                // A running average expressed in integer ticks can alternate by
                // one tick even though the video's rational frame rate is fixed.
                const uint32_t measuredDuration = static_cast<uint32_t>(
                    i % 2 ? std::ceil(duration) : std::floor(duration));
                const auto targets = scheduler.Schedule(timestamp(i), timestamp(i + 1),
                    Rate(10'000'000, measuredDuration));
                for (const auto& target : targets) {
                    Check(target.presentationTime > previousTarget, "quantized 2x target timestamps increase");
                    previousTarget = target.presentationTime;
                    if (target.exactSource) ++source;
                    else {
                        ++synthetic;
                        Check(target.timestep > 0.4 && target.timestep < 0.6,
                            "2x inference is the midpoint, never a rounded source endpoint");
                    }
                }
            }
            std::cout << "2x " << rate.numerator << '/' << rate.denominator
                << " quantum=" << quantum << " source=" << source << " synthetic=" << synthetic << '\n';
            Check(source == 600, "2x reuses every real source endpoint despite timestamp rounding");
            Check(synthetic == 600, "2x needs exactly one inference per source pair");
            CheckNear(static_cast<double>(previousTarget), 600.0 * duration, 1.0,
                "2x keeps the rational target grid despite quantized source timestamps");
        }
    }
}

void TestPerVideoCapsNeverBoostRequestedRate()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Fixed120, {}, {}, 2000, 90'000);
    const FrameRate capped = scheduler.ResolveTargetRate(Rate(24));
    Check(capped.numerator == 48 && capped.denominator == 1,
        "2x cap wins over fixed 120 and 90 fps cap for 24 fps source");

    scheduler.Configure(FrameInterpolationRateMode::Movie2x, {}, {}, 4000, 120'000);
    const FrameRate unchanged = scheduler.ResolveTargetRate(Rate(24));
    Check(unchanged.numerator == 48 && unchanged.denominator == 1,
        "rule caps never boost a lower requested mode");
}

void TestPerVideoCapsPreserveNtscRationals()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Movie5x, {}, {}, 2000, 0);
    const FrameRate capped = scheduler.ResolveTargetRate(Rate(24000, 1001));
    Check(capped.numerator == 48000 && capped.denominator == 1001,
        "2x cap preserves canonical 23.976 rational rate");
}

void TestMeasuredLoadCapsOnlyUnsustainableRequests()
{
    const FrameRate source = Rate(30000, 1001);
    const FrameRate fourTimes = Rate(120000, 1001);
    const auto overloaded = EstimateFrameInterpolationLoad(
        source, fourTimes, 2, 23'918, 80);
    Check(overloaded.requestedOutputFpsMilli > 119'000,
        "load estimate retains the requested NTSC output rate");
    Check(overloaded.sustainableSyntheticFpsMilli > 80'000,
        "two measured contexts contribute aggregate synthetic throughput");
    Check(overloaded.outputCapFpsMilli == 89'910,
        "an unattainable NTSC 4x request falls back to the exact 3x source-aligned rate");

    const auto sustainable = EstimateFrameInterpolationLoad(
        source, Rate(90000, 1001), 2, 23'918, 80);
    Check(sustainable.outputCapFpsMilli == 0,
        "a sustainable 3x request remains uncapped");

    const auto warming = EstimateFrameInterpolationLoad(
        source, fourTimes, 2, 23'918, 7);
    Check(warming.outputCapFpsMilli == 0,
        "the controller waits for enough measurements before applying a cap");

    const auto aligned120 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(120), 2, 25'000, 80);
    Check(aligned120.outputCapFpsMilli == 0,
        "60 -> 120 remains sustainable because every source endpoint is reused");

    const auto unaligned121 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(121), 2, 25'000, 80);
    Check(unaligned121.outputCapFpsMilli == 120'000,
        "60 -> 121 detects the doubled synthetic load and falls back to aligned 120 fps");

    const auto sustainable71 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(71), 2, 25'000, 80);
    Check(sustainable71.outputCapFpsMilli == 0,
        "60 -> 71 stays unchanged when its actual synthetic workload is sustainable");

    const auto overloaded71 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(71), 2, 30'000, 80);
    Check(overloaded71.outputCapFpsMilli == 70'000,
        "an overloaded 60 -> 71 request falls only to the highest sustainable grid, 70 fps");

    const auto sustainable140 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(140), 2, 10'000, 80);
    Check(sustainable140.outputCapFpsMilli == 0,
        "RIFE-only 60 -> 140 remains uncapped when inference capacity is sufficient");

    const auto sustainable150 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(150), 2, 10'000, 80);
    Check(sustainable150.outputCapFpsMilli == 0,
        "RIFE-only 60 -> 150 remains uncapped when inference capacity is sufficient");

    const auto limitedFourTimes = EstimateFrameInterpolationLoad(
        Rate(30), Rate(120), 2, 24'250, 80);
    Check(limitedFourTimes.outputCapFpsMilli == 90'000,
        "30 fps 4x request with roughly 110 fps total capacity falls back to sustainable 3x/90");
}

void TestFixedAndCustomRatesRaiseToNearbySourceMultiplier()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Fixed120, {}, {});
    const FrameRate fixed = scheduler.ResolveConfiguredTargetRate(Rate(121, 2));
    Check(fixed.numerator == 121 && fixed.denominator == 1,
        "60.5 fps source with fixed 120 rounds to nearest 2x rate, 121 fps");

    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(120), {});
    const FrameRate custom = scheduler.ResolveConfiguredTargetRate(Rate(121, 2));
    Check(custom.numerator == 121 && custom.denominator == 1,
        "60.5 fps source with custom 120 also rounds to nearest 2x rate, 121 fps");

    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(121), {});
    const FrameRate explicit121 = scheduler.ResolveConfiguredTargetRate(Rate(60));
    Check(explicit121.numerator == 121 && explicit121.denominator == 1,
        "explicit 121 fps on a 60 fps source is preserved until measured load requires a cap");
}

void TestRuntimeCapComposesWithUserCaps()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Movie4x, {}, {}, 0, 110'000);
    Check(scheduler.ResolveConfiguredTargetRate(Rate(30)).numerator == 110,
        "configured rule cap is visible before runtime load control");

    scheduler.SetRuntimeOutputFpsCap(95'000);
    const FrameRate runtimeCapped = scheduler.ResolveTargetRate(Rate(30));
    Check(runtimeCapped.numerator == 95 && runtimeCapped.denominator == 1,
        "runtime load cap composes with the configured user cap");

    scheduler.SetRuntimeOutputFpsCap(115'000);
    const FrameRate userCapped = scheduler.ResolveTargetRate(Rate(30));
    Check(userCapped.numerator == 110 && userCapped.denominator == 1,
        "runtime load control never raises a user-selected cap");
}

void TestAdaptiveCapPreservesExactNtscMultiplier()
{
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Movie4x, {}, {});
    scheduler.SetRuntimeOutputFpsCap(89'910);
    const FrameRate capped = scheduler.ResolveTargetRate(Rate(30000, 1001));
    Check(capped.numerator == 90000 && capped.denominator == 1001,
        "rounded adaptive milli-fps cap snaps back to the exact NTSC 3x source rate");
}

FrameInterpolationPressureSnapshot PressureSnapshot(
    uint64_t sourcePoolMisses = 0,
    uint64_t lateSyntheticDrops = 0,
    uint64_t presentationDrops = 0,
    uint64_t presentationReclaims = 0,
    uint64_t presentationSurfaceWaitUs = 0,
    uint64_t presentationSurfaceWaitCount = 0,
    uint32_t sourceQueueDepth = 0,
    uint32_t pendingPairDepth = 0,
    uint32_t inferenceContexts = 2,
    uint64_t presenterStaleDrops = 0)
{
    return {
        sourcePoolMisses,
        lateSyntheticDrops,
        presentationDrops,
        presentationReclaims,
        presentationSurfaceWaitUs,
        presentationSurfaceWaitCount,
        sourceQueueDepth,
        pendingPairDepth,
        inferenceContexts,
        presenterStaleDrops,
    };
}

FrameInterpolationPressureSnapshot DeliverySnapshot(
    uint64_t renderedFrames, uint64_t lateDrops = 0)
{
    auto snapshot = PressureSnapshot(0, lateDrops);
    snapshot.presenterDeliveryAvailable = true;
    snapshot.presenterRenderedFrames = renderedFrames;
    return snapshot;
}

void TestSilentOutputShortfallBacksOff()
{
    CFrameInterpolationPressureController controller;
    (void)controller.Update(120'000, 30'000, 0, DeliverySnapshot(0));
    (void)controller.Update(120'000, 30'000, 1'000, DeliverySnapshot(60));
    const auto decision = controller.Update(120'000, 30'000, 3'000, DeliverySnapshot(180));
    Check(decision.pressureDetected && decision.outputCapFpsMilli == 90'000,
        "a quiet 120 fps request delivering only 60 fps backs off to aligned 90");
    Check((decision.confirmedPressureReasons & RIFE_PRESSURE_DELIVERY_SHORTFALL) != 0,
        "silent output shortfall is visible as the cause of backoff");
}

void TestOutputValidatedProbeReturnsToKnownGood()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 120'000;
    constexpr uint32_t Source = 30'000;
    (void)controller.Update(Request, Source, 0, DeliverySnapshot(0));
    (void)controller.Update(Request, Source, 1'000, DeliverySnapshot(60));
    (void)controller.Update(Request, Source, 3'000, DeliverySnapshot(180));
    (void)controller.Update(Request, Source, 4'000, DeliverySnapshot(270));
    auto decision = controller.Update(Request, Source, 6'000, DeliverySnapshot(450));
    Check(decision.phase == FrameInterpolationPressurePhase::Stable
            && decision.lastKnownGoodFpsMilli == 90'000,
        "90 fps becomes known-good only after the presenter delivers it");

    decision = controller.Update(Request, Source, 6'066, DeliverySnapshot(456, 1));
    decision = controller.Update(Request, Source, 6'132, DeliverySnapshot(462, 2));
    Check(decision.outputCapFpsMilli == 90'000 && !decision.pressureDetected,
        "two isolated late-drop samples cannot demote verified 90 fps to 60");

    decision = controller.Update(Request, Source, 11'000, DeliverySnapshot(900, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe
            && decision.outputCapFpsMilli == 0,
        "validated 90 fps eventually probes the requested 120 fps");
    (void)controller.Update(Request, Source, 12'000, DeliverySnapshot(990, 2));
    decision = controller.Update(Request, Source, 13'000, DeliverySnapshot(1'050, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe,
        "a quiet probe cannot become known-good before its output window completes");
    decision = controller.Update(Request, Source, 14'000, DeliverySnapshot(1'110, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Settling
            && decision.outputCapFpsMilli == 90'000,
        "120 fps silently delivering 60 returns to measured-good 90 fps");
}

void TestPressureControllerLeavesHealthyArbitraryRateUncapped()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(140'000, 60'000, 0, PressureSnapshot());
    const auto decision = controller.Update(140'000, 60'000, 1'000, PressureSnapshot());

    Check(!decision.pressureDetected,
        "healthy pipeline reports no end-to-end pressure");
    Check(decision.outputCapFpsMilli == 0,
        "healthy 140 fps request remains uncapped regardless of source multiples");
}

void TestPressureControllerBacksOffToNearbyCheaperMultiple()
{
    CFrameInterpolationPressureController controller;
    (void)controller.Update(121'000, Rate(60), 0, PressureSnapshot());
    (void)controller.Update(121'000, Rate(60), 100, PressureSnapshot(0, 1));
    const auto decision = controller.Update(121'000, Rate(60), 200, PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli == 120'000,
        "overloaded 60 -> 121 uses cheaper 120 fps instead of lowering to costly 119 fps");
}

void TestPressureControllerSettlesBeforeAdditionalBackoff()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(190'000, 60'000, 0, PressureSnapshot());

    auto decision = controller.Update(190'000, 60'000, 100,
        PressureSnapshot(0, 1));
    Check(!decision.pressureDetected && decision.outputCapFpsMilli == 0,
        "one late synthetic output is observed but cannot cap the pipeline by itself");

    decision = controller.Update(190'000, 60'000, 200,
        PressureSnapshot(0, 2));
    Check(decision.pressureDetected && decision.outputCapFpsMilli == 180'000,
        "confirmed overload avoids a 2 fps cut that would increase synthetic work");

    decision = controller.Update(190'000, 60'000, 400,
        PressureSnapshot(0, 3));
    Check(decision.outputCapFpsMilli == 180'000,
        "confirmed pressure while the pipeline is settling cannot stack another cut");

    decision = controller.Update(190'000, 60'000, 1'200,
        PressureSnapshot(0, 4));
    Check(decision.outputCapFpsMilli == 180'000,
        "the first fresh post-settle distress observation starts confirmation without cutting again");

    decision = controller.Update(190'000, 60'000, 1'300,
        PressureSnapshot(1, 4));
    Check(decision.outputCapFpsMilli == 120'000,
        "continued distress drops to the next cheaper whole source multiple");
}

void TestPressureControllerDetectsBacklogAndPresentationWaits()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(150'000, 60'000, 0, PressureSnapshot());

    auto decision = controller.Update(150'000, 60'000, 100,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 2, 1, 2));
    Check(!decision.pressureDetected && decision.outputCapFpsMilli == 0,
        "one backlog observation is not enough to reduce the output rate");
    decision = controller.Update(150'000, 60'000, 200,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 2, 1, 2));
    Check(decision.pressureDetected && decision.outputCapFpsMilli == 120'000,
        "persistent backlog selects a cheaper aligned rate instead of increasing inference work");

    controller.Reset();
    (void)controller.Update(150'000, 60'000, 0, PressureSnapshot());
    decision = controller.Update(150'000, 60'000, 100,
        PressureSnapshot(0, 0, 0, 0, 3'000, 1));
    Check(!decision.pressureDetected && decision.outputCapFpsMilli == 0,
        "presentation-surface waits are telemetry-only producer backpressure");
    Check((decision.observedPressureReasons & RIFE_PRESSURE_PRESENTATION_SURFACE_WAIT) != 0,
        "presentation-surface waits remain visible in pressure diagnostics");
}

void TestPressureControllerDetectsPresenterStaleDrops()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(190'000, 15'000, 0, PressureSnapshot());

    auto decision = controller.Update(190'000, 15'000, 100,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 1));
    Check(!decision.pressureDetected && decision.outputCapFpsMilli == 0,
        "one presenter stale drop is not enough to lower the output cap");
    Check((decision.observedPressureReasons & RIFE_PRESSURE_PRESENTER_STALE_DROP) != 0,
        "presenter stale drops are visible as an observed pressure reason");

    decision = controller.Update(190'000, 15'000, 200,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 2));
    Check(decision.pressureDetected && decision.outputCapFpsMilli == 180'000,
        "repeated presenter stale drops select a cheaper aligned backoff");
    Check((decision.confirmedPressureReasons & RIFE_PRESSURE_PRESENTER_STALE_DROP) != 0,
        "confirmed pressure diagnostics identify presenter stale drops");
}

void TestPressureControllerAcceleratesPersistentPresenterOverload()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 190'000;
    constexpr uint32_t Source = 15'000;
    (void)controller.Update(Request, Source, 0, PressureSnapshot());
    (void)controller.Update(Request, Source, 100,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 1));
    auto decision = controller.Update(Request, Source, 200,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 2));
    Check(decision.outputCapFpsMilli == 180'000,
        "15 fps overload first selects the cheaper 180 fps multiple");

    (void)controller.Update(Request, Source, 300,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 3));
    decision = controller.Update(Request, Source, 400,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 4));
    Check(decision.outputCapFpsMilli == 180'000,
        "old queued losses cannot trigger another cut immediately");
    decision = controller.Update(Request, Source, 550,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 5));
    Check(decision.outputCapFpsMilli == 180'000,
        "losses during the transition grace period do not trigger another cut");

    decision = controller.Update(Request, Source, 1'200,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 6));
    decision = controller.Update(Request, Source, 1'300,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 7));
    Check(decision.outputCapFpsMilli == 165'000,
        "confirmed presenter losses after the grace period lower the cap");

    decision = controller.Update(Request, Source, 2'400,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 7));
    Check(decision.phase == FrameInterpolationPressurePhase::Stable,
        "quiet playback establishes a stable lower cap");
    decision = controller.Update(Request, Source, 2'800,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 7));
    Check(decision.phase == FrameInterpolationPressurePhase::Stable,
        "a short quiet spell does not immediately reintroduce an overloaded rate");
    decision = controller.Update(Request, Source, 2'900,
        PressureSnapshot(0, 0, 0, 0, 0, 0, 0, 0, 2, 7));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe,
        "recovery still checks for renewed GPU headroom after sustained quiet playback");
}

void TestPressureControllerResumeKeepsGoodCapWithoutOldLosses()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 140'000;
    constexpr uint32_t Source = 60'000;
    (void)controller.Update(Request, Source, 0, PressureSnapshot());
    (void)controller.Update(Request, Source, 100, PressureSnapshot(0, 1));
    auto decision = controller.Update(Request, Source, 200, PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli == 120'000,
        "overload establishes the cap before pausing");
    decision = controller.Update(Request, Source, 1'300, PressureSnapshot(0, 2));
    decision = controller.Update(Request, Source, 3'400, PressureSnapshot(0, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe,
        "recovery probe is active when playback pauses");

    controller.Resume(3'500);
    decision = controller.Update(Request, Source, 3'500,
        PressureSnapshot(0, 200, 0, 0, 0, 0, 0, 0, 2, 100));
    Check(decision.outputCapFpsMilli == 120'000
            && !decision.pressureDetected
            && decision.observedPressureReasons == RIFE_PRESSURE_NONE,
        "resume restores the proven cap and ignores drops from the old generation");
    decision = controller.Update(Request, Source, 3'600,
        PressureSnapshot(0, 200, 0, 0, 0, 0, 0, 0, 2, 100));
    Check(decision.outputCapFpsMilli == 120'000,
        "resumed playback does not restart at the overloaded request");

    decision = controller.Update(Request, 30'000, 3'700,
        PressureSnapshot(0, 200, 0, 0, 0, 0, 0, 0, 2, 100));
    Check(decision.outputCapFpsMilli == 0,
        "a different source frame rate still invalidates the learned cap");
}

void TestPressureControllerRecoversByBracketAndRevertsFailedProbe()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(140'000, 60'000, 0, PressureSnapshot());
    auto decision = controller.Update(140'000, 60'000, 100,
        PressureSnapshot(0, 1));
    Check(decision.outputCapFpsMilli == 0,
        "one transient late output does not establish a lower cap");
    decision = controller.Update(140'000, 60'000, 200,
        PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli == 120'000,
        "confirmed repeated pressure selects a cheaper aligned cap before recovery");

    decision = controller.Update(140'000, 60'000, 1'300,
        PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli == 120'000,
        "quiet after settling establishes the reduced rate as known-good");

    decision = controller.Update(140'000, 60'000, 3'400,
        PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli == 130'000,
        "recovery probes the midpoint of the known-good/known-bad bracket");

    decision = controller.Update(140'000, 60'000, 3'500,
        PressureSnapshot(0, 3));
    Check(decision.outputCapFpsMilli == 130'000,
        "drops during a recovery probe transition do not reject the probe");
    decision = controller.Update(140'000, 60'000, 3'600,
        PressureSnapshot(0, 4));
    Check(decision.outputCapFpsMilli == 130'000,
        "repeated transition-window drops remain excluded from probe pressure");
    decision = controller.Update(140'000, 60'000, 4'500,
        PressureSnapshot(0, 5));
    decision = controller.Update(140'000, 60'000, 4'600,
        PressureSnapshot(0, 6));
    Check(decision.outputCapFpsMilli == 120'000,
        "confirmed post-transition pressure during a recovery probe returns to the last stable cap");
}

void TestPressureControllerRecoversQuicklyAfterAnOvershoot()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(190'000, 60'000, 0, PressureSnapshot());

    uint64_t lateDrops = 0;
    uint64_t now = 100;
    auto decision = controller.Update(190'000, 60'000, now, PressureSnapshot(0, ++lateDrops));
    now += 100;
    decision = controller.Update(190'000, 60'000, now, PressureSnapshot(0, ++lateDrops));
    for (int i = 0; i < 4; ++i) {
        now += 1'000;
        decision = controller.Update(190'000, 60'000, now, PressureSnapshot(0, ++lateDrops));
        now += 100;
        decision = controller.Update(190'000, 60'000, now, PressureSnapshot(0, ++lateDrops));
    }
    Check(decision.outputCapFpsMilli < 150'000,
        "escalating backoff can move a badly overloaded 190 fps request down quickly");

    const uint32_t lowCap = decision.outputCapFpsMilli;
    now += 1'100;
    decision = controller.Update(190'000, 60'000, now, PressureSnapshot(0, lateDrops));
    Check(decision.outputCapFpsMilli == lowCap,
        "first quiet settled sample records the low cap as known-good");

    now += 2'100;
    decision = controller.Update(190'000, 60'000, now, PressureSnapshot(0, lateDrops));
    Check(decision.outputCapFpsMilli > lowCap + 1'000,
        "recovery jumps toward the known-bad boundary instead of crawling upward at 1 fps every few seconds");
}

void TestPressureControllerRecoversThroughHigherWholeMultiples()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    constexpr uint32_t Request = 190'000;
    constexpr uint32_t Source = 30'000;
    uint64_t now = 0;
    uint64_t lateDrops = 0;
    (void)controller.Update(Request, Source, now, PressureSnapshot());
    (void)controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    auto decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));

    // Backoff from 190 visits the cheaper aligned 180, 150, 120, and 90
    // instead of stepping through arbitrary rates that need more inference.
    for (int i = 0; i < 3; ++i) {
        (void)controller.Update(Request, Source, now += 1'100,
            PressureSnapshot(0, ++lateDrops));
        decision = controller.Update(Request, Source, now += 100,
            PressureSnapshot(0, ++lateDrops));
    }
    Check(decision.outputCapFpsMilli == 90'000,
        "sustained pressure reaches 90 fps without skipping the aligned rates");
    decision = controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.phase == FrameInterpolationPressurePhase::Stable,
        "quiet playback establishes a stable cap after backoff");

    for (const uint32_t aligned : {120'000u, 150'000u, 180'000u}) {
        decision = controller.Update(Request, Source, now += aligned == 120'000 ? 30'000 : 2'100,
            PressureSnapshot(0, lateDrops));
        Check(decision.phase == FrameInterpolationPressurePhase::Probe
                && decision.outputCapFpsMilli == aligned,
            "recovery tests each higher source-aligned rate after overload subsides");
        decision = controller.Update(Request, Source, now += 1'100,
            PressureSnapshot(0, lateDrops));
        Check(decision.phase == FrameInterpolationPressurePhase::Stable
                && decision.lastKnownGoodFpsMilli == aligned,
            "a healthy aligned probe becomes the next known-good cap");
    }
}

void TestPressureControllerRecoversFromNtscThreeTimesToFourTimes()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 190'000;
    constexpr FrameRate Source{30'000, 1'001};
    constexpr uint32_t ThreeTimes = 89'910;
    constexpr uint32_t FourTimes = 119'880;
    uint64_t now = 0;
    uint64_t lateDrops = 0;
    (void)controller.Update(Request, Source, now, PressureSnapshot());

    // Drive sustained overload down to the 3x NTSC cadence, matching a clip
    // that settles near 89.91 fps after the requested 190 fps proves too high.
    (void)controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    auto decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    for (int i = 0; i < 8 && decision.outputCapFpsMilli > ThreeTimes; ++i) {
        (void)controller.Update(Request, Source, now += 1'100,
            PressureSnapshot(0, ++lateDrops));
        decision = controller.Update(Request, Source, now += 100,
            PressureSnapshot(0, ++lateDrops));
    }
    Check(decision.outputCapFpsMilli == ThreeTimes,
        "sustained overload reaches the exact 3x cadence for 29.97 fps content");

    // Once playback is quiet, the controller must probe 4x (119.88) rather
    // than remain parked indefinitely at the previous known-good 3x cap.
    decision = controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.phase == FrameInterpolationPressurePhase::Stable
            && decision.lastKnownGoodFpsMilli == ThreeTimes,
        "quiet playback establishes the 3x NTSC cap as known-good");
    decision = controller.Update(Request, Source, now += 2'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe
            && decision.outputCapFpsMilli == FourTimes,
        "quiet 3x NTSC playback advances to a 4x recovery probe");
}

void TestPressureControllerStopsEscalatingAfterAlignedRateFails()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    constexpr uint32_t Request = 94'000;
    constexpr uint32_t Source = 30'000;
    uint64_t now = 0;
    uint64_t lateDrops = 0;
    (void)controller.Update(Request, Source, now, PressureSnapshot());
    (void)controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    auto decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    Check(decision.outputCapFpsMilli == 90'000,
        "a nearby 90 fps source multiple is chosen over a costly 92 fps backoff");
    decision = controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.phase == FrameInterpolationPressurePhase::Stable
            && decision.lastKnownGoodFpsMilli == 90'000,
        "quiet playback establishes 90 fps as known-good before its load changes");
    (void)controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    Check(decision.outputCapFpsMilli == 60'000
            && decision.phase == FrameInterpolationPressurePhase::Settling,
        "sustained overload at 90 fps drops to the next cheaper aligned rate");
    (void)controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, lateDrops));
    decision = controller.Update(Request, Source, now += 2'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.outputCapFpsMilli == 60'000
            && decision.phase == FrameInterpolationPressurePhase::Stable,
        "the known-good aligned rate is held instead of probing an intermediate cadence");
    decision = controller.Update(Request, Source, now += 30'000,
        PressureSnapshot(0, lateDrops));
    Check(decision.outputCapFpsMilli == 90'000
            && decision.phase == FrameInterpolationPressurePhase::Probe,
        "the previously failed aligned rate is retried after sustained quiet playback");
}

void TestPressureControllerHolds90UntilFailed120CanBeRetried()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 190'000;
    constexpr uint32_t Source = 30'000;
    constexpr uint32_t KnownGood = 90'000;
    constexpr uint32_t FailedProbe = 120'000;
    uint64_t now = 0;
    uint64_t lateDrops = 0;
    (void)controller.Update(Request, Source, now, PressureSnapshot());
    (void)controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    auto decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));

    for (int i = 0; i < 3; ++i) {
        (void)controller.Update(Request, Source, now += 1'100,
            PressureSnapshot(0, ++lateDrops));
        decision = controller.Update(Request, Source, now += 100,
            PressureSnapshot(0, ++lateDrops));
    }
    Check(decision.outputCapFpsMilli == KnownGood,
        "pressure backs the 30 fps source down to its sustainable 90 fps cadence");
    decision = controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.phase == FrameInterpolationPressurePhase::Stable,
        "90 fps becomes the stable known-good cap before recovery");

    decision = controller.Update(Request, Source, now += 2'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe
            && decision.outputCapFpsMilli == FailedProbe,
        "recovery probes the next exact cadence at 120 fps");

    // Ignore transition-time drops, then confirm that the 120 fps probe is
    // unsustainable. The controller should return to 90 and remember the
    // failed aligned rate for its retry cooldown.
    (void)controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, ++lateDrops));
    decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    Check(decision.outputCapFpsMilli == KnownGood
            && decision.phase == FrameInterpolationPressurePhase::Settling,
        "an overloaded 120 fps probe falls directly back to the known-good 90 fps cap");
    (void)controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, lateDrops));
    decision = controller.Update(Request, Source, now += 2'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.outputCapFpsMilli == KnownGood
            && decision.phase == FrameInterpolationPressurePhase::Stable,
        "90 fps stays selected instead of trying an unstable intermediate cap near 105 fps");

    decision = controller.Update(Request, Source, now += 30'000,
        PressureSnapshot(0, lateDrops));
    Check(decision.outputCapFpsMilli == FailedProbe
            && decision.phase == FrameInterpolationPressurePhase::Probe,
        "120 fps is retried after its cooldown while preserving the stable 90 fps fallback");
}

void TestPressureControllerSupportsRatesAbove190AndFractionalSources()
{
    CFrameInterpolationPressureController controller;
    constexpr FrameRate ntscSource = {24'000, 1'001};
    (void)controller.Update(240'000, ntscSource, 0, PressureSnapshot());
    (void)controller.Update(240'000, ntscSource, 100, PressureSnapshot(0, 1));
    auto decision = controller.Update(240'000, ntscSource, 200, PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli == 239'760,
        "a 240 fps request can back off to exact 10x NTSC cadence above 190 fps");

    controller.Reset();
    constexpr FrameRate uncommonSource = {100'000, 1'691};
    constexpr uint32_t Request = 200'000;
    uint64_t now = 0;
    uint64_t lateDrops = 0;
    (void)controller.Update(Request, uncommonSource, now, PressureSnapshot());
    (void)controller.Update(Request, uncommonSource, now += 100,
        PressureSnapshot(0, ++lateDrops));
    decision = controller.Update(Request, uncommonSource, now += 100,
        PressureSnapshot(0, ++lateDrops));
    Check(decision.outputCapFpsMilli == 177'410,
        "aligned backoff uses the exact source rational instead of multiplying rounded milli-fps");

    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(200), {});
    scheduler.SetRuntimeOutputFpsCap(decision.outputCapFpsMilli);
    const FrameRate output = scheduler.ResolveTargetRate(uncommonSource);
    Check(output.numerator == 300'000 && output.denominator == 1'691,
        "the rounded controller cap restores an exact uncommon source multiple in the scheduler");

    controller.Reset();
    (void)controller.Update(300'000, Rate(60), 0, PressureSnapshot());
    (void)controller.Update(300'000, Rate(60), 100, PressureSnapshot(0, 1));
    (void)controller.Update(300'000, Rate(60), 200, PressureSnapshot(0, 2));
    (void)controller.Update(300'000, Rate(60), 1'300, PressureSnapshot(0, 2));
    decision = controller.Update(300'000, Rate(60), 31'900, PressureSnapshot(0, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe
            && decision.outputCapFpsMilli == 0,
        "a 5x 60 fps request can probe 300 fps rather than stopping at 190 or 240");
}

void TestPressureControllerDoesNotTreatOneDrainingSurfaceWaitAsFreshOverload()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(190'000, 60'000, 0, PressureSnapshot());
    uint64_t waitUs = 0;
    uint64_t waitCount = 0;
    uint64_t lateDrops = 0;
    FrameInterpolationPressureDecision decision;
    for (uint64_t now = 100; now <= 5'000; now += 100) {
        waitUs += 4'000;
        ++waitCount;
        if (now == 1'000 || now == 3'000 || now == 5'000) {
            ++lateDrops;
        }
        decision = controller.Update(190'000, 60'000, now,
            PressureSnapshot(0, lateDrops, 0, 0, waitUs, waitCount));
        Check(decision.outputCapFpsMilli == 0,
            "continuous presentation backpressure plus isolated late drops cannot ratchet the cap downward");
    }
    Check(!decision.pressureDetected
            && (decision.observedPressureReasons & RIFE_PRESSURE_PRESENTATION_SURFACE_WAIT),
        "surface waits remain observable without becoming cap-driving pressure");
}

void TestPressureControllerResetsWhenRequestedRateChanges()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(190'000, 60'000, 0, PressureSnapshot());
    auto decision = controller.Update(190'000, 60'000, 100, PressureSnapshot(0, 1));
    decision = controller.Update(190'000, 60'000, 200, PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli != 0,
        "pressure establishes an adaptive cap before a user target change");

    decision = controller.Update(120'000, 60'000, 300, PressureSnapshot(0, 2));
    Check(decision.outputCapFpsMilli == 0 && !decision.pressureDetected,
        "changing the requested output rate starts a fresh pressure measurement instead of inheriting a stale cap");
}

void TestPressureControllerNeverCapsBelowSourceRate()
{
    CFrameInterpolationPressureController controller;
    controller.Reset();
    (void)controller.Update(62'000, 60'000, 0, PressureSnapshot());
    (void)controller.Update(62'000, 60'000, 100,
        PressureSnapshot(1));
    const auto decision = controller.Update(62'000, 60'000, 200,
        PressureSnapshot(2));
    Check(decision.outputCapFpsMilli == 60'000,
        "pressure control never drives the output rate below the real source rate");
}

void TestPressureAtSourceRateDoesNotEraseKnownBadRate()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 190'000;
    constexpr uint32_t Source = 60'000;
    uint64_t now = 0;
    uint64_t lateDrops = 0;
    (void)controller.Update(Request, Source, now, PressureSnapshot());
    (void)controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    auto decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    for (int i = 0; i < 5; ++i) {
        (void)controller.Update(Request, Source, now += 1'100,
            PressureSnapshot(0, ++lateDrops));
        decision = controller.Update(Request, Source, now += 100,
            PressureSnapshot(0, ++lateDrops));
    }
    Check(decision.outputCapFpsMilli == Source,
        "backoff stops at the real source rate after sustained pressure");
    (void)controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, ++lateDrops));
    decision = controller.Update(Request, Source, now += 100,
        PressureSnapshot(0, ++lateDrops));
    Check(decision.outputCapFpsMilli == Source
            && decision.lastKnownBadFpsMilli > Source,
        "pressure at source rate retains the last meaningful bad rate");
    (void)controller.Update(Request, Source, now += 1'100,
        PressureSnapshot(0, lateDrops));
    decision = controller.Update(Request, Source, now += 2'100,
        PressureSnapshot(0, lateDrops));
    Check(decision.outputCapFpsMilli > Source
            && decision.outputCapFpsMilli < Request,
        "recovery probes cautiously instead of reopening the original overloaded target");
}
} // namespace

int main()
{
    Test24To120();
    Test23976TimesFiveUses11988Grid();
    TestRoundedNtscRatesUseCanonicalMultiplierGrids();
    Test24To60KeepsUniformGridAcrossPairs();
    TestMovieTwoAndHalfTimes();
    TestToScreenUsesPreciseDisplayRate();
    TestExactEndpointIsReturnedAsSource();
    TestNonGridEndpointRemainsExcluded();
    TestResetReanchorsTimeline();
    TestLongRunDoesNotAccumulateRoundedPeriodDrift();
    TestTwoTimesWithQuantizedSourceTimestamps();
    TestPerVideoCapsNeverBoostRequestedRate();
    TestPerVideoCapsPreserveNtscRationals();
    TestMeasuredLoadCapsOnlyUnsustainableRequests();
    TestFixedAndCustomRatesRaiseToNearbySourceMultiplier();
    TestRuntimeCapComposesWithUserCaps();
    TestAdaptiveCapPreservesExactNtscMultiplier();
    TestSilentOutputShortfallBacksOff();
    TestOutputValidatedProbeReturnsToKnownGood();
    TestPressureControllerLeavesHealthyArbitraryRateUncapped();
    TestPressureControllerBacksOffToNearbyCheaperMultiple();
    TestPressureControllerSettlesBeforeAdditionalBackoff();
    TestPressureControllerDetectsBacklogAndPresentationWaits();
    TestPressureControllerDetectsPresenterStaleDrops();
    TestPressureControllerAcceleratesPersistentPresenterOverload();
    TestPressureControllerResumeKeepsGoodCapWithoutOldLosses();
    TestPressureControllerRecoversByBracketAndRevertsFailedProbe();
    TestPressureControllerRecoversQuicklyAfterAnOvershoot();
    TestPressureControllerRecoversThroughHigherWholeMultiples();
    TestPressureControllerRecoversFromNtscThreeTimesToFourTimes();
    TestPressureControllerStopsEscalatingAfterAlignedRateFails();
    TestPressureControllerHolds90UntilFailed120CanBeRetried();
    TestPressureControllerSupportsRatesAbove190AndFractionalSources();
    TestPressureControllerDoesNotTreatOneDrainingSurfaceWaitAsFreshOverload();
    TestPressureControllerResetsWhenRequestedRateChanges();
    TestPressureControllerNeverCapsBelowSourceRate();
    TestPressureAtSourceRateDoesNotEraseKnownBadRate();

    if (g_failures) {
        std::cerr << g_failures << " scheduler test(s) failed\n";
        return 1;
    }

    std::cout << "All RIFE scheduler tests passed\n";
    return 0;
}
