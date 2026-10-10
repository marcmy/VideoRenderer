#include "../../Source/RifeFrameInterpolation.h"
#include <iostream>

int wmain()
{
    int failures = 0;
    auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    auto* first = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{1});
    auto* second = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{2});
    auto* output = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{3});
    CRifeFrameInterpolation current, legacy;
    check(current.Initialize(L"pair-runtime", nullptr, 320, 576, 320, 570, UINT32_MAX, 2,
        false, L"model.onnx", L"cache"), "new fake runtime must load without GPU work");
    MpcvrRifeStats stats;
    check(current.Interpolate(1, first, second, output, 0.25f, stats, 987654321),
        "renderer must call the current ABI-2 runtime");
    check(stats.engineBytes == 987654321 && stats.inputPairReuse == 1,
        "pair generation and new stats tail must cross the real renderer loader");
    check(current.DrainContext(1), "optional drain export must remain available");
    check(legacy.Initialize(L"legacy-runtime", nullptr, 320, 576, 320, 570, UINT32_MAX, 2,
        false, L"model.onnx", L"cache"), "old ABI-2 runtime must load without a drain export");
    check(legacy.Interpolate(1, first, second, output, 0.75f, stats, 987654322),
        "new renderer must remain compatible with old runtime");
    check(stats.engineBytes == 1 && stats.inputPairReuse == 0,
        "old runtime must leave unsupported optional stats zero");
    check(legacy.DrainContext(1), "old runtime must retain synchronous drain fallback");
    check(!legacy.Initialize(L"legacy-runtime", nullptr, 320, 576, 320, 570, UINT32_MAX, 2,
        false, L"model.onnx", L"cache", true, false), "old runtime must reject precision without its capability export");
    check(legacy.GetStatus().find(L"Update the RIFE runtime") != std::wstring::npos, "missing capability must give actionable update status");
    std::cout << "RIFE pair ABI loader checks: " << failures << " failures\n";
    return failures ? 1 : 0;
}
