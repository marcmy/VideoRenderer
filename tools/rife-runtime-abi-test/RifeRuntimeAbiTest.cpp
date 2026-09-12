#include "../../Source/RifeFrameInterpolation.h"

#include <iostream>
#include <string_view>

namespace {
int g_failures = 0;

void Check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++g_failures;
    }
}
}

int wmain()
{
    const auto missing = CRifeFrameInterpolation::Probe(L"missing-runtime");
    Check(!missing.available, "missing runtime must be reported as unavailable");

    const auto bad = CRifeFrameInterpolation::Probe(L"bad-runtime");
    Check(!bad.available, "ABI mismatch must be rejected");
    Check(bad.abiVersion == 999, "ABI mismatch must report the discovered version");

    const auto good = CRifeFrameInterpolation::Probe(L"good-runtime");
    Check(good.available, "matching runtime ABI must be accepted");
    Check(good.abiVersion == MPCVR_RIFE_RUNTIME_ABI, "matching runtime must report ABI 1");

    if (g_failures) {
        std::cerr << g_failures << " RIFE runtime ABI test(s) failed\n";
        return 1;
    }

    std::cout << "All RIFE runtime ABI tests passed\n";
    return 0;
}
