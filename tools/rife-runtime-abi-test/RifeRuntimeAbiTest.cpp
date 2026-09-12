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

    CRifeFrameInterpolation runtime;
    Check(runtime.Initialize(
        L"good-runtime", nullptr, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache"),
        "matching fake runtime must initialize");
    Check(runtime.IsReady(), "initialized runtime must report ready");

    MpcvrRifeStats stats = {};
    auto* first = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(1));
    auto* second = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(2));
    auto* output = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(3));
    Check(runtime.Interpolate(1, first, second, output, 0.25f, stats),
        "fake runtime interpolation call must succeed");
    Check(stats.inferenceMs == 1.0, "runtime stats must propagate through ABI");
    Check(stats.engineBytes == 1, "engine size stats must propagate through ABI");

    runtime.Reset();
    Check(!runtime.IsReady(), "Reset must unload runtime and clear handle");

    CRifeFrameInterpolation badRuntime;
    Check(!badRuntime.Initialize(
        L"bad-runtime", nullptr, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache"),
        "ABI mismatch must fail persistent initialization");
    Check(!badRuntime.IsReady(), "failed initialization must not leave a ready runtime");

    if (g_failures) {
        std::cerr << g_failures << " RIFE runtime ABI test(s) failed\n";
        return 1;
    }

    std::cout << "All RIFE runtime ABI tests passed\n";
    return 0;
}
