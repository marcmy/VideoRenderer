#include "../../Source/FrameInterpolationScheduler.h"
#include "../../Source/RifeInferenceTiming.h"

#include <algorithm>
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

void TestVariableFrameRateUsesStableAverageAndRealPairTimes()
{
    CFrameInterpolationSourceRateStabilizer sourceRate;
    CFrameInterpolationScheduler scheduler;
    scheduler.Configure(FrameInterpolationRateMode::Movie2x, {}, {});

    int64_t firstTime = 0;
    (void)sourceRate.Observe(Rate(30), firstTime, true);
    int64_t previousTarget = -1;
    int steadyTargetCount = 0;
    for (int i = 0; i < 100; ++i) {
        const int64_t secondTime = firstTime + (i % 2 ? 333333 : 416667);
        const FrameRate stableRate = sourceRate.Observe(
            Rate(i % 2 ? 30 : 24), secondTime, true);
        if (i >= 70) {
            CheckNear(static_cast<double>(stableRate.numerator) / stableRate.denominator,
                26.6667, 0.2, "VFR policy rate tracks the sustained average without following each sample");
        }
        const auto targets = scheduler.Schedule(firstTime, secondTime, stableRate);
        for (const auto& target : targets) {
            Check(target.presentationTime > previousTarget,
                "VFR output timestamps stay ordered across unequal source intervals");
            previousTarget = target.presentationTime;
            if (!target.exactSource) {
                Check(target.timestep > 0.0 && target.timestep < 1.0,
                    "VFR inference uses the real enclosing source timestamps");
            }
            if (i >= 70) ++steadyTargetCount;
        }
        firstTime = secondTime;
    }
    Check(steadyTargetCount >= 50 && steadyTargetCount <= 70,
        "VFR x2 produces a steady output count near twice the average source rate");
    Check(sourceRate.HasVariableTiming(),
        "uneven source timestamps are identified as VFR for workload planning");

    const FrameRate beforeSeek = sourceRate.Observe(Rate(30), firstTime + 333333, true);
    sourceRate.ResetSegment();
    const FrameRate afterSeek = sourceRate.Observe(Rate(30), 0, true);
    Check(beforeSeek.numerator == afterSeek.numerator
            && beforeSeek.denominator == afterSeek.denominator,
        "seek or repeat preserves the stabilized source rate");
    Check(sourceRate.HasVariableTiming(),
        "seek preserves VFR workload classification for the same clip");
    sourceRate.Reset();
    const FrameRate afterStop = sourceRate.Observe(Rate(60), 0, true);
    Check(afterStop.numerator == 60 && afterStop.denominator == 1,
        "Stop clears the stabilized source rate for a new clip");
    Check(!sourceRate.HasVariableTiming(),
        "Stop clears VFR classification for a new clip");
}

void TestVariableRateModesAndCaps()
{
    const FrameRate average = Rate(80, 3); // Alternating 24/30 fps intervals.
    CFrameInterpolationScheduler scheduler;
    const struct {
        FrameInterpolationRateMode mode;
        double expected;
    } movieModes[] = {
        {FrameInterpolationRateMode::Movie2x, 160.0 / 3},
        {FrameInterpolationRateMode::Movie2_5x, 200.0 / 3},
        {FrameInterpolationRateMode::Movie3x, 80.0},
        {FrameInterpolationRateMode::Movie4x, 320.0 / 3},
        {FrameInterpolationRateMode::Movie5x, 400.0 / 3},
    };
    for (const auto& [mode, expected] : movieModes) {
        scheduler.Configure(mode, {}, {});
        scheduler.SetVariableSourceTiming(true);
        const FrameRate target = scheduler.ResolveTargetRate(average);
        CheckNear(static_cast<double>(target.numerator) / target.denominator,
            expected, 0.001, "every Movie multiplier uses the stable VFR average");
    }

    scheduler.Configure(FrameInterpolationRateMode::ToScreen, {}, Rate(144));
    scheduler.SetVariableSourceTiming(true);
    Check(scheduler.ResolveTargetRate(average).numerator == 144,
        "To screen targets the display rate independently of source timing");

    scheduler.Configure(FrameInterpolationRateMode::Fixed90, {}, {});
    scheduler.SetVariableSourceTiming(true);
    Check(scheduler.ResolveTargetRate(average).numerator == 90,
        "fixed 90 fps remains a fixed target on VFR input");

    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(190), {}, 4000, 100'000);
    scheduler.SetVariableSourceTiming(true);
    Check(scheduler.ResolveConfiguredTargetRate(average).numerator == 100,
        "a per-video 100 fps ceiling limits a 190 fps custom request on VFR input");
    scheduler.SetRuntimeOutputFpsCap(77'000);
    Check(scheduler.ResolveTargetRate(average).numerator == 77,
        "measured overload caps VFR output below both the request and rule ceiling");

    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(190), {});
    scheduler.SetVariableSourceTiming(true);
    Check(scheduler.ResolveTargetRate(average).numerator == 190,
        "without a rule or measured overload, a high custom request remains the target");

    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(120), {});
    scheduler.SetVariableSourceTiming(true);
    Check(scheduler.ResolveTargetRate(Rate(121, 2)).numerator == 120,
        "VFR custom 120 is not raised to 121 for a nonexistent endpoint-reuse benefit");

    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(190), {});
    scheduler.SetVariableSourceTiming(true);
    scheduler.SetRuntimeOutputFpsCap(59'940);
    const FrameRate runtimeCap = scheduler.ResolveTargetRate(Rate(30'000, 1001));
    Check(runtimeCap.numerator == 2997 && runtimeCap.denominator == 50,
        "VFR runtime cap stays at the measured rate instead of snapping to a source multiple");
}

void TestVariableTimingIgnoresMillisecondQuantization()
{
    CFrameInterpolationSourceRateStabilizer quantized;
    CFrameInterpolationSourceRateStabilizer mildlyVariable;
    int64_t quantizedTime = 0;
    int64_t variableTime = 0;
    (void)quantized.Observe(Rate(30), quantizedTime, true);
    (void)mildlyVariable.Observe(Rate(24), variableTime, true);
    for (int i = 0; i < 40; ++i) {
        quantizedTime += i % 2 ? 330'000 : 340'000;
        variableTime += i % 2 ? 400'000 : 416'667;
        (void)quantized.Observe(Rate(30), quantizedTime, true);
        (void)mildlyVariable.Observe(Rate(24), variableTime, true);
    }
    Check(!quantized.HasVariableTiming(),
        "ordinary one-millisecond timestamp quantization does not disable CFR endpoint reuse");
    Check(mildlyVariable.HasVariableTiming(),
        "a smaller 24/25 fps timing mixture still uses VFR workload planning");
}

void TestDuplicateRemovalUsesRetainedCadence()
{
    CFrameInterpolationScheduler scheduler;
    for (const FrameRate decoded : {Rate(30), Rate(60), Rate(30000, 1001), Rate(80, 3)}) {
        const FrameRate retained = RetainedSourceRate(decoded, 2);
        for (const bool variable : {false, true}) {
            for (const auto [mode, factor] : std::vector<std::pair<FrameInterpolationRateMode, double>>{
                    {FrameInterpolationRateMode::Movie2x, 2.0},
                    {FrameInterpolationRateMode::Movie2_5x, 2.5},
                    {FrameInterpolationRateMode::Movie3x, 3.0},
                    {FrameInterpolationRateMode::Movie4x, 4.0},
                    {FrameInterpolationRateMode::Movie5x, 5.0}}) {
                scheduler.Configure(mode, {}, {}, 0, 0, 2);
                scheduler.SetVariableSourceTiming(variable);
                const FrameRate target = scheduler.ResolveTargetRate(retained);
                CheckNear(static_cast<double>(target.numerator) / target.denominator,
                    factor * decoded.numerator / decoded.denominator, 0.001,
                    "Movie target follows original FPS despite every-other removal, including VFR");
            }
        }
    }
    scheduler.Configure(FrameInterpolationRateMode::Movie4x, {}, {}, 0, 0, 2);
    const auto targets = scheduler.Schedule(0, 666667, Rate(15));
    Check(targets.size() == 8, "30fps x4 with alternate removal schedules 120fps over retained 15fps pairs");
    if (targets.size() == 8) {
        Check(targets.back().exactSource && targets.back().presentationTime == 666667,
            "decimation preserves real endpoint timing");
        CheckNear(targets[6].timestep, 0.875, 0.00002,
            "seven interior targets span the retained pair");
    }
    scheduler.Configure(FrameInterpolationRateMode::Fixed120, {}, {}, 2000, 90'000, 2);
    Check(scheduler.ResolveTargetRate(Rate(15)).numerator == 60,
        "per-video multiplier ceiling also follows original 30fps");
    scheduler.Configure(FrameInterpolationRateMode::Custom, Rate(190), {}, 0, 90'000, 2);
    Check(scheduler.ResolveTargetRate(Rate(15)).numerator == 90,
        "absolute FPS rule remains independent of decimation");
    scheduler.SetRuntimeOutputFpsCap(60'000);
    Check(scheduler.ResolveTargetRate(Rate(15)).numerator == 60,
        "runtime pressure cap remains effective with alternate removal");
    scheduler.Configure(FrameInterpolationRateMode::ToScreen, {}, Rate(240), 0, 0, 2);
    Check(scheduler.ResolveTargetRate(Rate(15)).numerator == 240,
        "screen output target is not multiplied by frame stride");
    scheduler.Configure(FrameInterpolationRateMode::Movie2x, {}, {}, 0, 0, 1);
    Check(scheduler.ResolveTargetRate(Rate(30)).numerator == 60,
        "switching removal off restores normal multiplier behavior");
    Check(RetainedSourceRate(Rate(60), 2).denominator == 2,
        "inference and pressure still see retained half cadence");
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
        source, fourTimes, 2000, 23'918, 80);
    Check(overloaded.requestedOutputFpsMilli > 119'000,
        "load estimate retains the requested NTSC output rate");
    Check(overloaded.sustainableSyntheticFpsMilli > 80'000,
        "two actually overlapping calls contribute aggregate synthetic throughput");
    Check(overloaded.outputCapFpsMilli == 89'910,
        "an unattainable NTSC 4x request falls back to the exact 3x source-aligned rate");

    const auto sustainable = EstimateFrameInterpolationLoad(
        source, Rate(90000, 1001), 2000, 23'918, 80);
    Check(sustainable.outputCapFpsMilli == 0,
        "a sustainable 3x request remains uncapped");

    const auto warming = EstimateFrameInterpolationLoad(
        source, fourTimes, 2000, 23'918, 7);
    Check(warming.outputCapFpsMilli == 0,
        "the controller waits for enough measurements before applying a cap");

    const auto aligned120 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(120), 2000, 25'000, 80);
    Check(aligned120.outputCapFpsMilli == 0,
        "60 -> 120 remains sustainable because every source endpoint is reused");

    const auto unaligned121 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(121), 2000, 25'000, 80);
    Check(unaligned121.outputCapFpsMilli == 120'000,
        "60 -> 121 detects the doubled synthetic load and falls back to aligned 120 fps");

    const auto sustainable71 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(71), 2000, 25'000, 80);
    Check(sustainable71.outputCapFpsMilli == 0,
        "60 -> 71 stays unchanged when its actual synthetic workload is sustainable");

    const auto overloaded71 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(71), 2000, 30'000, 80);
    Check(overloaded71.outputCapFpsMilli == 70'000,
        "an overloaded 60 -> 71 request falls only to the highest sustainable grid, 70 fps");

    const auto sustainable140 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(140), 2000, 10'000, 80);
    Check(sustainable140.outputCapFpsMilli == 0,
        "RIFE-only 60 -> 140 remains uncapped when inference capacity is sufficient");

    const auto sustainable150 = EstimateFrameInterpolationLoad(
        Rate(60), Rate(150), 2000, 10'000, 80);
    Check(sustainable150.outputCapFpsMilli == 0,
        "RIFE-only 60 -> 150 remains uncapped when inference capacity is sufficient");

    const auto limitedFourTimes = EstimateFrameInterpolationLoad(
        Rate(30), Rate(120), 2000, 24'250, 80);
    Check(limitedFourTimes.outputCapFpsMilli == 90'000,
        "30 fps 4x request with roughly 110 fps total capacity falls back to sustainable 3x/90");
}

void TestInferenceCapacityMeasuresOverlapInsteadOfConfiguredThreads()
{
    CRifeInferenceTimingWindow<16> sequential;
    CRifeInferenceTimingWindow<16> contended;
    CRifeInferenceTimingWindow<16> parallel;
    for (uint64_t i = 0; i < 16; ++i) {
        // Idle gaps and alternating context identities do not double capacity.
        sequential.AddMicroseconds(i * 30'000, i * 30'000 + 10'000);
        // Two calls that overlap but take twice as long have the same capacity.
        contended.AddMicroseconds((i / 2) * 30'000, (i / 2) * 30'000 + 20'000);
        // Preserve the gain when the GPU genuinely sustains parallel work.
        parallel.AddMicroseconds((i / 2) * 30'000, (i / 2) * 30'000 + 10'000);
    }
    const auto estimate = [](const auto& window) {
        const auto timing = window.GetSummary();
        return EstimateFrameInterpolationLoad(Rate(30), Rate(120),
            timing.parallelismPermille, timing.averageWallUs,
            static_cast<uint32_t>(timing.count));
    };
    Check(estimate(sequential).sustainableSyntheticFpsMilli == 97'000,
        "sparse calls measure one lane's capacity without counting idle gaps");
    Check(estimate(contended).sustainableSyntheticFpsMilli == 97'000,
        "overlapping calls with doubled latency do not invent doubled capacity");
    Check(estimate(parallel).sustainableSyntheticFpsMilli == 194'000,
        "real parallel throughput is retained rather than universally halving the estimate");

    CRifeInferenceTimingWindow<4> unordered;
    unordered.AddMicroseconds(15'000, 25'000);
    unordered.AddMicroseconds(0, 20'000); // A slower earlier call completes last.
    auto timing = unordered.GetSummary();
    Check(timing.busyUs == 25'000 && timing.parallelismPermille == 1200,
        "out-of-order completion merges partially overlapping calls exactly once");
    unordered.AddMicroseconds(30'000, 40'000);
    unordered.AddMicroseconds(40'000, 50'000);
    unordered.AddMicroseconds(60'000, 70'000);
    timing = unordered.GetSummary();
    Check(timing.count == 4 && timing.busyUs == 50'000 && timing.completedCalls == 5,
        "the rolling window evicts old calls while merging adjacent busy intervals");
    unordered.Clear();
    unordered.AddMicroseconds(10, 10);
    unordered.AddMicroseconds(20, 10);
    Check(unordered.GetSummary().count == 0 && unordered.GetSummary().completedCalls == 0,
        "reset and invalid intervals leave no stale inference capacity");
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
    // Existing delivery tests model an otherwise idle GPU. Tests below supply
    // constrained, missing or stale measurements to exercise the recovery gate.
    snapshot.sustainableSyntheticFpsMilli = 1'000'000;
    snapshot.inferenceTimingSamples = 16;
    snapshot.inferenceCompletedCalls = renderedFrames;
    snapshot.presenterAverageRenderUs = 500;
    snapshot.presenterTimingSamples = 16;
    return snapshot;
}

void TestVariableRateDoesNotAssumeAlignedEndpointReuse()
{
    const auto constantRate = EstimateFrameInterpolationLoad(
        Rate(30), Rate(90), 2000, 25'000, 80, 970);
    const auto variableRate = EstimateFrameInterpolationLoad(
        Rate(30), Rate(90), 2000, 25'000, 80, 970, true);
    Check(constantRate.outputCapFpsMilli == 0,
        "CFR 30 to 90 reuses the source endpoints and fits measured inference capacity");
    Check(variableRate.outputCapFpsMilli > 30'000
            && variableRate.outputCapFpsMilli < 90'000,
        "VFR 30 to 90 does not claim the same endpoint-reuse discount");

    CFrameInterpolationPressureController controller;
    (void)controller.Update(190'000, Rate(30), 0, PressureSnapshot(), true);
    (void)controller.Update(190'000, Rate(30), 100, PressureSnapshot(0, 1), true);
    const auto decision = controller.Update(190'000, Rate(30), 200,
        PressureSnapshot(0, 2), true);
    Check(decision.pressureDetected && decision.outputCapFpsMilli == 188'000,
        "VFR overload begins a measured backoff without assuming 180 fps is cheaper");
}

void TestHighRefreshVariableRateConvergesWithoutTinyProbes()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 240'000;
    constexpr uint32_t Capacity = 165'000;
    uint32_t target = Request;
    uint64_t frameMilli = 0;
    uint32_t previousCap = Request;
    uint32_t settledTarget = 0;
    bool tinyProbe = false;
    bool changedDuringHold = false;
    (void)controller.Update(Request, Rate(30), 0, DeliverySnapshot(0), true);
    for (uint64_t now = 100; now <= 180'000; now += 100) {
        // An overloaded pipeline loses throughput; a lower feasible request
        // delivers every target. Simulated stage headroom permits initial
        // exploration, but only real delivery can validate the target.
        const uint32_t delivered = target <= Capacity ? target : 130'000;
        frameMilli += delivered / 10;
        const auto decision = controller.Update(Request, Rate(30), now,
            DeliverySnapshot(frameMilli / 1000), true);
        const uint32_t cap = decision.outputCapFpsMilli ? decision.outputCapFpsMilli : Request;
        if (decision.changed && cap > previousCap) {
            tinyProbe |= cap - previousCap < std::max(2'000u, previousCap / 50u);
        }
        if (now == 90'000) {
            settledTarget = cap;
        } else if (now > 90'000) {
            changedDuringHold |= cap != settledTarget;
        }
        previousCap = cap;
        target = cap;
    }
    Check(settledTarget >= 155'000 && settledTarget <= Capacity,
        "a VFR 240 Hz request finds a useful lower delivered rate");
    Check(!tinyProbe,
        "high-refresh VFR recovery does not keep reanchoring for negligible FPS gains");
    Check(!changedDuringHold,
        "a narrowed VFR capacity bracket holds the verified target instead of continuously probing");
}

void TestVariableRateDeliveryFailureHasRecoveryCooldown()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 190'000;
    uint64_t frameMilli = 0;
    uint32_t target = Request;
    bool returnedToStable = false;
    uint64_t failedProbeAt = 0;
    uint32_t knownGood = 0;
    bool heldDuringCooldown = true;
    (void)controller.Update(Request, Rate(30), 0, DeliverySnapshot(0), true);
    for (uint64_t now = 100; now <= 360'000; now += 100) {
        const uint32_t delivered = target <= 180'000 ? target : 160'000;
        frameMilli += delivered / 10;
        const auto decision = controller.Update(Request, Rate(30), now,
            DeliverySnapshot(frameMilli / 1000), true);
        if (decision.pressureDetected && decision.lastKnownGoodFpsMilli
                && target > decision.lastKnownGoodFpsMilli && !failedProbeAt) {
            failedProbeAt = now;
            knownGood = decision.lastKnownGoodFpsMilli;
        } else if (failedProbeAt && now < failedProbeAt + 30'000) {
            returnedToStable |= decision.phase == FrameInterpolationPressurePhase::Stable;
            heldDuringCooldown &= decision.outputCapFpsMilli == knownGood;
        }
        target = decision.outputCapFpsMilli ? decision.outputCapFpsMilli : Request;
    }
    Check(failedProbeAt && returnedToStable,
        "the VFR cooldown regression exercises a failed measured probe and stable fallback");
    Check(heldDuringCooldown,
        "a failed VFR recovery probe keeps its verified fallback during the retry cooldown");
}

void TestConvergedVariableRateStillAdaptsToLoadChanges()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 240'000;
    uint32_t target = Request;
    uint64_t frameMilli = 0;
    bool backedOff = false;
    bool recovered = false;
    (void)controller.Update(Request, Rate(30), 0, DeliverySnapshot(0), true);
    for (uint64_t now = 100; now <= 750'000; now += 100) {
        // Maxine/another GPU job gets heavier after settling, then releases
        // its capacity. A convergence hold must allow both directions.
        const uint32_t capacity = now < 90'000 ? 165'000u
            : now < 180'000 ? 100'000u : Request;
        const uint32_t delivered = target <= capacity ? target : capacity * 3 / 4;
        frameMilli += delivered / 10;
        auto snapshot = DeliverySnapshot(frameMilli / 1000);
        snapshot.sustainableSyntheticFpsMilli = capacity * 2;
        const auto decision = controller.Update(Request, Rate(30), now,
            snapshot, true);
        target = decision.outputCapFpsMilli ? decision.outputCapFpsMilli : Request;
        if (now == 150'000) {
            backedOff = target <= 100'000 && target >= 90'000
                && decision.measuredOutputHealthy;
        }
        recovered |= now > 180'000 && target == Request
            && decision.measuredOutputHealthy;
    }
    Check(backedOff,
        "a converged VFR target still backs off when actual delivered FPS deteriorates");
    Check(recovered,
        "bounded recovery retries can reclaim the requested refresh rate after GPU load drops");
}

void TestNarrowRefreshBracketCanRetryRequestedRate()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 240'000;
    uint32_t target = Request;
    uint64_t frameMilli = 0;
    bool heldNearRequest = false;
    bool recovered = false;
    (void)controller.Update(Request, Rate(30), 0, DeliverySnapshot(0), true);
    for (uint64_t now = 100; now <= 180'000; now += 100) {
        const uint32_t delivered = now < 15'000 && target == Request ? 220'000 : target;
        frameMilli += delivered / 10;
        const auto decision = controller.Update(Request, Rate(30), now,
            DeliverySnapshot(frameMilli / 1000), true);
        target = decision.outputCapFpsMilli ? decision.outputCapFpsMilli : Request;
        if (now == 60'000) {
            heldNearRequest = target == 238'000 && decision.measuredOutputHealthy;
        }
        recovered |= now > 15'000 && target == Request && decision.measuredOutputHealthy;
    }
    Check(heldNearRequest,
        "a near-refresh overload holds 238 fps instead of repeatedly searching tenths of an FPS");
    Check(recovered,
        "a deliberate cooldown retry can restore 240 Hz even when the final gain is below the probe threshold");
}

struct VerifiedRateFixture {
    CFrameInterpolationPressureController controller;
    FrameRate source = Rate(30);
    uint32_t request = 102'000;
    uint32_t good = 100'000;
    bool variable = true;
    uint64_t now = 0;
    uint64_t frameMilli = 0;
    FrameInterpolationPressureDecision decision;

    void Step(uint32_t deliveredMilli)
    {
        now += 100;
        frameMilli += deliveredMilli / 10;
        decision = controller.Update(request, source, now,
            DeliverySnapshot(frameMilli / 1000), variable);
    }

    uint32_t Target() const
    {
        return decision.outputCapFpsMilli ? decision.outputCapFpsMilli : request;
    }

    void Learn()
    {
        (void)controller.Update(request, source, 0, DeliverySnapshot(0), variable);
        for (int tick = 0; tick < 30; ++tick) Step(request / 2);
        Check(Target() == good, "recovery fixture backs off from an unattainable request");
        for (int tick = 0; tick < 30; ++tick) Step(good);
        Check(decision.phase == FrameInterpolationPressurePhase::Stable
                && decision.lastKnownGoodFpsMilli == good && decision.measuredOutputHealthy,
            "recovery fixture has a delivered and verified lower target");
    }

    void Interrupt()
    {
        bool pressure = false;
        for (int tick = 0; tick < 20; ++tick) {
            Step(good / 2);
            pressure |= decision.pressureDetected;
        }
        Check(Target() < good && pressure,
            "an interruption still promptly reduces a verified target");
    }
};

void TestVerifiedRateRecoversAfterAnInterruption()
{
    for (const auto& config : std::vector<VerifiedRateFixture>{
            {}, { {}, Rate(30), 120'000, 90'000, false },
            { {}, Rate(30000, 1001), 119'880, 89'910, false },
            { {}, Rate(121, 2), 181'500, 121'000, false },
            { {}, Rate(24), 360'000, 336'000, false } }) {
        auto fixture = config;
        fixture.Learn();
        fixture.Interrupt();
        bool restored = false;
        uint32_t highestRetry = 0;
        for (int tick = 0; tick < 150; ++tick) {
            fixture.Step(fixture.Target());
            if (fixture.decision.phase == FrameInterpolationPressurePhase::VerifiedRecovery) {
                highestRetry = std::max(highestRetry, fixture.Target());
            }
            restored |= fixture.Target() == fixture.good
                && fixture.decision.measuredOutputHealthy
                && fixture.decision.phase == FrameInterpolationPressurePhase::Stable;
            Check(fixture.Target() <= fixture.good,
                "verified recovery does not reintroduce the never-sustainable requested rate");
            if (restored) break;
        }
        Check(restored && highestRetry == fixture.good,
            "healthy fallback promptly retries and restores the precise previously verified target");
        Check(fixture.decision.lastKnownBadFpsMilli == fixture.request,
            "successful recovery retains the original failed upper bound");
    }
}

void TestVerifiedRecoveryFailsOnceUnderSustainedLoad()
{
    VerifiedRateFixture fixture;
    fixture.Learn();
    fixture.Interrupt();
    uint32_t recoveryStarts = 0;
    bool wasRecovering = false;
    bool fallbackVerified = false;
    for (int tick = 0; tick < 700; ++tick) {
        fixture.Step(fixture.Target() <= 98'000 ? fixture.Target() : 70'000);
        const bool recovering = fixture.decision.phase == FrameInterpolationPressurePhase::VerifiedRecovery;
        recoveryStarts += recovering && !wasRecovering;
        wasRecovering = recovering;
        if (fixture.now >= 25'000) {
            fallbackVerified |= fixture.Target() == 98'000 && fixture.decision.measuredOutputHealthy;
            Check(fixture.Target() == 98'000,
                "failed verified recovery holds the sustainable fallback during normal cooldown");
        }
    }
    Check(recoveryStarts == 1 && fallbackVerified,
        "sustained overload allows only one quick retry of the formerly verified rate");
}

void TestVerifiedRecoverySurvivesSeekButStopAndSettingsClearIt()
{
    for (int mode = 0; mode < 3; ++mode) {
        VerifiedRateFixture fixture;
        fixture.Learn();
        fixture.Interrupt();
        if (mode == 0) {
            fixture.controller.Resume(fixture.now);
        } else if (mode == 1) {
            fixture.controller.Reset();
        } else {
            fixture.request = 90'000;
        }
        bool recovered = false;
        for (int tick = 0; tick < 100; ++tick) {
            fixture.Step(mode == 0 ? fixture.Target() : fixture.request);
            if (mode == 0) {
                recovered |= fixture.Target() == fixture.good && fixture.decision.measuredOutputHealthy;
            } else {
                Check(fixture.decision.outputCapFpsMilli == 0
                        && fixture.decision.phase != FrameInterpolationPressurePhase::VerifiedRecovery,
                    "stop/reset and requested-rate changes clear the previous recovery candidate");
            }
        }
        if (mode == 0) Check(recovered, "seek/resume retains the verified recovery candidate with fresh counters");
    }
}

void TestRequestedRateCanRecoverWhenAlreadyVerified()
{
    VerifiedRateFixture fixture;
    fixture.request = fixture.good;
    (void)fixture.controller.Update(fixture.request, fixture.source, 0, DeliverySnapshot(0), true);
    for (int tick = 0; tick < 60; ++tick) fixture.Step(fixture.request);
    Check(fixture.decision.measuredOutputHealthy, "unlimited requested-rate fixture is actually verified");
    fixture.Interrupt();
    bool recovered = false;
    for (int tick = 0; tick < 100; ++tick) {
        fixture.Step(fixture.Target());
        recovered |= fixture.decision.outputCapFpsMilli == 0 && fixture.decision.measuredOutputHealthy;
    }
    Check(recovered, "a verified full requested rate also recovers without a multi-minute cooldown");
}

void TestPauseDuringVerifiedRecoveryPreservesFallbackAndCandidate()
{
    VerifiedRateFixture fixture;
    fixture.Learn();
    fixture.Interrupt();
    for (int tick = 0; tick < 30; ++tick) fixture.Step(fixture.Target());
    Check(fixture.decision.phase == FrameInterpolationPressurePhase::VerifiedRecovery,
        "resume test pauses during the previously verified rate retry");
    fixture.controller.Resume(fixture.now);
    fixture.frameMilli = 0; // A new presentation generation has new counters.
    fixture.Step(98'000);
    Check(fixture.Target() == 98'000 && !fixture.decision.pressureDetected,
        "pause during recovery restores the freshly verified fallback and discards old counters");
    bool recovered = false;
    for (int tick = 0; tick < 100; ++tick) {
        fixture.Step(fixture.Target());
        recovered |= fixture.Target() == 100'000 && fixture.decision.measuredOutputHealthy;
    }
    Check(recovered, "resume can finish the interrupted recovery with fresh delivery verification");
}

void TestVerifiedRecoveryStillRejectsResourceExhaustion()
{
    VerifiedRateFixture fixture;
    fixture.Learn();
    fixture.Interrupt();
    for (int tick = 0; tick < 40; ++tick) fixture.Step(fixture.Target());
    Check(fixture.decision.phase == FrameInterpolationPressurePhase::VerifiedRecovery,
        "resource-exhaustion test reaches the recovery probe after transition grace");
    for (uint64_t misses = 1; misses <= 2; ++misses) {
        fixture.now += 100;
        fixture.frameMilli += fixture.Target() / 10;
        auto snapshot = DeliverySnapshot(fixture.frameMilli / 1000);
        snapshot.sourcePoolMisses = misses;
        fixture.decision = fixture.controller.Update(fixture.request, fixture.source,
            fixture.now, snapshot, true);
    }
    Check(fixture.Target() == 98'000 && fixture.decision.pressureDetected,
        "a recovery probe still falls back promptly on confirmed resource exhaustion");
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

    for (uint64_t now = 7'000; now <= 16'000; now += 1'000) {
        decision = controller.Update(Request, Source, now,
            DeliverySnapshot(450 + (now - 6'000) * 90 / 1000, 2));
    }
    Check(decision.phase == FrameInterpolationPressurePhase::Probe
            && decision.outputCapFpsMilli == 0,
        "validated 90 fps eventually probes the requested 120 fps");
    (void)controller.Update(Request, Source, 17'000, DeliverySnapshot(1'440, 2));
    decision = controller.Update(Request, Source, 18'000, DeliverySnapshot(1'500, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe,
        "a quiet probe cannot become known-good before its output window completes");
    decision = controller.Update(Request, Source, 19'000, DeliverySnapshot(1'560, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Settling
            && decision.outputCapFpsMilli == 90'000,
        "120 fps silently delivering 60 returns to measured-good 90 fps");
}

void TestHealthyProbeSurvivesBriefDrops()
{
    CFrameInterpolationPressureController controller;
    constexpr uint32_t Request = 120'000;
    constexpr uint32_t Source = 30'000;
    (void)controller.Update(Request, Source, 0, DeliverySnapshot(0));
    (void)controller.Update(Request, Source, 1'000, DeliverySnapshot(60));
    (void)controller.Update(Request, Source, 3'000, DeliverySnapshot(180));
    (void)controller.Update(Request, Source, 4'000, DeliverySnapshot(270));
    (void)controller.Update(Request, Source, 6'000, DeliverySnapshot(450));
    FrameInterpolationPressureDecision decision;
    for (uint64_t now = 7'000; now <= 16'000; now += 1'000) {
        decision = controller.Update(Request, Source, now,
            DeliverySnapshot(450 + (now - 6'000) * 90 / 1000));
    }
    Check(decision.phase == FrameInterpolationPressurePhase::Probe,
        "the brief-drop regression starts with a real recovery probe");

    (void)controller.Update(Request, Source, 17'000, DeliverySnapshot(1'470));
    (void)controller.Update(Request, Source, 17'066, DeliverySnapshot(1'478, 1));
    decision = controller.Update(Request, Source, 17'132, DeliverySnapshot(1'486, 2));
    Check(decision.phase == FrameInterpolationPressurePhase::Probe
            && !decision.pressureDetected,
        "two brief late drops cannot reject a probe before measuring its output");
    Check((decision.observedPressureReasons & RIFE_PRESSURE_LATE_SYNTHETIC_DROP) != 0,
        "brief probe losses remain visible even when they do not decide the cap");
    decision = controller.Update(Request, Source, 19'000, DeliverySnapshot(1'708, 2));
    Check(decision.outputCapFpsMilli == 0
            && decision.phase == FrameInterpolationPressurePhase::Open
            && decision.measuredOutputHealthy,
        "a probe delivering 119 of 120 fps is accepted despite two brief losses");
}

void TestHealthySettlingRateSurvivesBriefDrops()
{
    CFrameInterpolationPressureController controller;
    (void)controller.Update(120'000, 30'000, 0, DeliverySnapshot(0));
    (void)controller.Update(120'000, 30'000, 1'000, DeliverySnapshot(60));
    (void)controller.Update(120'000, 30'000, 3'000, DeliverySnapshot(180));
    (void)controller.Update(120'000, 30'000, 4'000, DeliverySnapshot(270));
    (void)controller.Update(120'000, 30'000, 4'066, DeliverySnapshot(276, 1));
    auto decision = controller.Update(120'000, 30'000, 4'132, DeliverySnapshot(282, 2));
    Check(decision.outputCapFpsMilli == 90'000 && !decision.pressureDetected,
        "isolated settling losses cannot cascade an otherwise healthy 90 fps cap to 60");
    decision = controller.Update(120'000, 30'000, 6'000, DeliverySnapshot(448, 2));
    Check(decision.outputCapFpsMilli == 90'000
            && decision.lastKnownGoodFpsMilli == 90'000,
        "settling establishes the cadence from delivered FPS instead of two error updates");
}

void TestRuntimeWarmupDoesNotBecomeOverload()
{
    CFrameInterpolationPressureController controller;
    constexpr uint64_t StartMs = 1'000'000;
    auto snapshot = DeliverySnapshot(0);
    snapshot.interpolationReady = false;
    (void)controller.Update(120'000, 30'000, StartMs, snapshot);
    for (uint64_t second = 1; second <= 30; ++second) {
        snapshot.presenterRenderedFrames = second * 30;
        const auto decision = controller.Update(120'000, 30'000,
            StartMs + second * 1'000, snapshot);
        Check(decision.outputCapFpsMilli == 0 && !decision.pressureDetected
                && !decision.measuredOutputReady,
            "source-only playback during engine compilation cannot teach a lower interpolation cap");
    }
    snapshot.interpolationReady = true;
    snapshot.presenterRenderedFrames = 930;
    (void)controller.Update(120'000, 30'000, StartMs + 31'000, snapshot);
    snapshot.presenterRenderedFrames = 1'050;
    (void)controller.Update(120'000, 30'000, StartMs + 32'000, snapshot);
    snapshot.presenterRenderedFrames = 1'290;
    const auto decision = controller.Update(120'000, 30'000, StartMs + 34'000, snapshot);
    Check(decision.outputCapFpsMilli == 0 && decision.measuredOutputHealthy,
        "engine readiness starts a fresh delivery window with the original target");
}

void TestDeliveryWarmupUsesRequestStartTime()
{
    CFrameInterpolationPressureController controller;
    constexpr uint64_t StartMs = 1'000'000;
    (void)controller.Update(120'000, 30'000, StartMs, DeliverySnapshot(0));
    (void)controller.Update(120'000, 30'000, StartMs + 100, DeliverySnapshot(12));
    (void)controller.Update(120'000, 30'000, StartMs + 1'000, DeliverySnapshot(120));
    auto decision = controller.Update(120'000, 30'000, StartMs + 2'100, DeliverySnapshot(252));
    Check(!decision.measuredOutputReady,
        "machine uptime cannot skip the initial one-second delivery warmup");
    decision = controller.Update(120'000, 30'000, StartMs + 3'000, DeliverySnapshot(360));
    Check(decision.measuredOutputReady && decision.measuredOutputHealthy,
        "delivery validates after a complete post-warmup two-second window");
}

void TestProbeStillBacksOffOnResourceExhaustion()
{
    CFrameInterpolationPressureController controller;
    (void)controller.Update(120'000, 30'000, 0, DeliverySnapshot(0));
    (void)controller.Update(120'000, 30'000, 1'000, DeliverySnapshot(60));
    (void)controller.Update(120'000, 30'000, 3'000, DeliverySnapshot(180));
    (void)controller.Update(120'000, 30'000, 4'000, DeliverySnapshot(270));
    (void)controller.Update(120'000, 30'000, 6'000, DeliverySnapshot(450));
    for (uint64_t now = 7'000; now <= 16'000; now += 1'000) {
        (void)controller.Update(120'000, 30'000, now,
            DeliverySnapshot(450 + (now - 6'000) * 90 / 1000));
    }
    auto snapshot = DeliverySnapshot(1'470);
    snapshot.sourcePoolMisses = 1;
    (void)controller.Update(120'000, 30'000, 17'000, snapshot);
    snapshot.sourcePoolMisses = 2;
    const auto decision = controller.Update(120'000, 30'000, 17'100, snapshot);
    Check(decision.pressureDetected && decision.outputCapFpsMilli == 90'000,
        "repeated pool exhaustion still rejects a probe promptly without waiting for FPS sampling");
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
struct RecoveryHeadroomFixture {
    CFrameInterpolationPressureController controller;
    FrameRate source = Rate(50);
    uint32_t request = 200'000;
    bool variable = false;
    uint32_t capacity = 74'000;
    uint64_t renderUs = 1'000;
    uint32_t timingSamples = 16;
    bool freshInference = true;
    uint64_t now = 0;
    uint64_t frameMilli = 0;
    uint64_t calls = 0;
    uint32_t probes = 0;
    FrameInterpolationPressureDecision decision;

    uint32_t Target() const { return decision.outputCapFpsMilli ? decision.outputCapFpsMilli : request; }

    void Step(uint32_t deliveredCeiling = 100'000) {
        const uint32_t sourceMilli = static_cast<uint32_t>(
            static_cast<uint64_t>(source.numerator) * 1000 / source.denominator);
        const uint32_t delivered = Target() <= deliveredCeiling ? Target() : sourceMilli * 3 / 2;
        frameMilli += delivered / 10;
        if (freshInference) calls += std::max(1u, delivered > sourceMilli ? (delivered-sourceMilli)/10'000 : 1u);
        auto snapshot = DeliverySnapshot(frameMilli / 1000);
        snapshot.sustainableSyntheticFpsMilli = capacity;
        snapshot.presenterAverageRenderUs = renderUs;
        snapshot.inferenceTimingSamples = timingSamples;
        snapshot.presenterTimingSamples = timingSamples;
        snapshot.inferenceCompletedCalls = calls;
        const bool wasProbing = decision.phase == FrameInterpolationPressurePhase::Probe;
        decision = controller.Update(request, source, now += 100, snapshot, variable);
        probes += !wasProbing && decision.phase == FrameInterpolationPressurePhase::Probe;
    }

    void Run(uint32_t seconds, uint32_t deliveredCeiling = 100'000) {
        for (uint32_t i = 0; i < seconds * 10; ++i) Step(deliveredCeiling);
    }
};

void TestRecoveryHolds50To100WithoutPeriodic150Probes()
{
    RecoveryHeadroomFixture f;
    f.Run(20);
    Check(f.Target() == 100'000 && f.decision.measuredOutputHealthy,
        "50 fps content requested at 200 settles to verified 100 fps");
    bool unchanged = true;
    for (unsigned i = 0; i < 6'000; ++i) {
        f.Step();
        unchanged &= f.Target() == 100'000 && !f.decision.changed;
    }
    Check(unchanged && f.probes == 0 && f.decision.recoveryWaitingForHeadroom,
        "synth-cap 74 cannot trigger a 150 fps probe over ten minutes of smooth playback");
    f.controller.Resume(f.now);
    f.Run(30);
    Check(f.Target() == 100'000 && f.probes == 0,
        "seek/resume retains the verified cap without timer-driven recovery probes");
}

void TestRecoveryIgnoresBriefHeadroomAndPresenterBottlenecks()
{
    RecoveryHeadroomFixture f;
    f.Run(20);
    f.capacity = 400'000;
    f.Run(1);
    f.capacity = 74'000;
    f.Run(20);
    Check(f.Target() == 100'000 && f.probes == 0,
        "a one-second inference capacity spike cannot interrupt smooth playback");
    f.capacity = 400'000;
    f.renderUs = 6'000;
    f.Run(120);
    Check(f.Target() == 100'000 && f.probes == 0,
        "fast RIFE alone cannot authorize 150 fps while presenter/Maxine work fills its budget");
    f.renderUs = 500;
    f.Run(4, 150'000);
    Check(f.Target() == 100'000 && f.probes == 0,
        "recovery waits for five seconds of sustained headroom");
    f.Run(20, 150'000);
    Check(f.decision.lastKnownGoodFpsMilli == 150'000 && f.probes > 0,
        "fresh sustained inference and presenter headroom permits a measured higher cadence");
}

void TestFailedRecoveryNeedsImprovementNotJustCooldown()
{
    RecoveryHeadroomFixture f;
    f.capacity = 200'000; // A deliberately optimistic estimate misses another bottleneck.
    f.renderUs = 500;
    f.Run(35);
    Check(f.probes == 1 && f.Target() == 100'000 && f.decision.lastKnownBadFpsMilli == 150'000,
        "an initially optimistic 150 fps probe fails back to verified 100 fps");
    f.Run(600);
    Check(f.probes == 1 && f.Target() == 100'000 && f.decision.measuredOutputHealthy,
        "unchanged pre-probe measurements do not retry a rejected cadence when cooldown expires");
    f.capacity = 249'000;
    f.Run(30);
    Check(f.probes == 1,
        "a marginal capacity change below the meaningful-improvement threshold cannot reopen a failure");
    f.capacity = 300'000;
    f.Run(30, 150'000);
    Check(f.probes >= 2 && f.decision.lastKnownGoodFpsMilli == 150'000 && f.Target() == 150'000,
        "a real capacity increase recovers the previously rejected rate while delivery still rejects excessive output");
}

void TestRecoveryRejectsMissingAndStaleTiming()
{
    RecoveryHeadroomFixture f;
    f.Run(20);
    f.capacity = 400'000;
    f.timingSamples = 0;
    f.Run(120);
    Check(f.probes == 0 && f.Target() == 100'000,
        "unknown timing is not evidence of spare capacity");
    f.timingSamples = 16;
    f.freshInference = false;
    f.Run(120);
    Check(f.probes == 0 && f.Target() == 100'000,
        "a stale rolling inference estimate cannot authorize periodic recovery");
    f.freshInference = true;
    f.Run(20, 150'000);
    Check(f.decision.lastKnownGoodFpsMilli == 150'000,
        "recovery resumes once enough new inference and presentation samples prove headroom");
}

void TestRecoveryHeadroomUsesVfrDemandAndFractionalSourceRates()
{
    RecoveryHeadroomFixture vfr;
    vfr.source = Rate(30);
    vfr.request = 102'000;
    vfr.variable = true;
    vfr.capacity = 120'000;
    vfr.Run(200);
    Check(vfr.Target() == 100'000 && vfr.probes == 0 && vfr.decision.measuredOutputHealthy,
        "VFR recovery budgets all output frames rather than assuming cheap aligned source endpoints");
    vfr.capacity = 200'000;
    vfr.Run(30, 102'000);
    Check(vfr.Target() == 102'000 && vfr.decision.measuredOutputHealthy,
        "VFR can recover an arbitrary requested rate with sufficient sustained headroom");

    RecoveryHeadroomFixture ntsc;
    ntsc.source = Rate(30'000,1'001);
    ntsc.request = 179'820;
    ntsc.capacity = 61'000;
    ntsc.Run(120, 89'910);
    Check(ntsc.Target() == 89'910 && ntsc.probes == 0,
        "fractional-source recovery holds the exact verified 3x cadence with inadequate headroom");
    ntsc.capacity = 150'000;
    ntsc.Run(30,119'880);
    Check(ntsc.decision.lastKnownGoodFpsMilli == 119'880,
        "fractional-source recovery still supports its exact 4x cadence after capacity improves");
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
    TestVariableFrameRateUsesStableAverageAndRealPairTimes();
    TestVariableRateModesAndCaps();
    TestVariableTimingIgnoresMillisecondQuantization();
    TestDuplicateRemovalUsesRetainedCadence();
    TestPerVideoCapsNeverBoostRequestedRate();
    TestPerVideoCapsPreserveNtscRationals();
    TestMeasuredLoadCapsOnlyUnsustainableRequests();
    TestInferenceCapacityMeasuresOverlapInsteadOfConfiguredThreads();
    TestVariableRateDoesNotAssumeAlignedEndpointReuse();
    TestHighRefreshVariableRateConvergesWithoutTinyProbes();
    TestVariableRateDeliveryFailureHasRecoveryCooldown();
    TestConvergedVariableRateStillAdaptsToLoadChanges();
    TestNarrowRefreshBracketCanRetryRequestedRate();
    TestFixedAndCustomRatesRaiseToNearbySourceMultiplier();
    TestRuntimeCapComposesWithUserCaps();
    TestAdaptiveCapPreservesExactNtscMultiplier();
    TestSilentOutputShortfallBacksOff();
    TestVerifiedRateRecoversAfterAnInterruption();
    TestVerifiedRecoveryFailsOnceUnderSustainedLoad();
    TestVerifiedRecoverySurvivesSeekButStopAndSettingsClearIt();
    TestRequestedRateCanRecoverWhenAlreadyVerified();
    TestPauseDuringVerifiedRecoveryPreservesFallbackAndCandidate();
    TestVerifiedRecoveryStillRejectsResourceExhaustion();
    TestOutputValidatedProbeReturnsToKnownGood();
    TestHealthyProbeSurvivesBriefDrops();
    TestHealthySettlingRateSurvivesBriefDrops();
    TestRuntimeWarmupDoesNotBecomeOverload();
    TestDeliveryWarmupUsesRequestStartTime();
    TestProbeStillBacksOffOnResourceExhaustion();
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
    TestRecoveryHolds50To100WithoutPeriodic150Probes();
    TestRecoveryIgnoresBriefHeadroomAndPresenterBottlenecks();
    TestFailedRecoveryNeedsImprovementNotJustCooldown();
    TestRecoveryRejectsMissingAndStaleTiming();
    TestRecoveryHeadroomUsesVfrDemandAndFractionalSourceRates();

    if (g_failures) {
        std::cerr << g_failures << " scheduler test(s) failed\n";
        return 1;
    }

    std::cout << "All RIFE scheduler tests passed\n";
    return 0;
}
