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

} // namespace

int main()
{
    Test24To120();
    Test23976TimesFiveUses11988Grid();
    Test24To60KeepsUniformGridAcrossPairs();
    TestMovieTwoAndHalfTimes();
    TestToScreenUsesPreciseDisplayRate();
    TestExactEndpointIsReturnedAsSource();
    TestNonGridEndpointRemainsExcluded();
    TestResetReanchorsTimeline();
    TestLongRunDoesNotAccumulateRoundedPeriodDrift();

    if (g_failures) {
        std::cerr << g_failures << " scheduler test(s) failed\n";
        return 1;
    }

    std::cout << "All RIFE scheduler tests passed\n";
    return 0;
}
