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

void TestLazyOutputFallback()
{
    auto runtime = std::make_shared<CRifeFrameInterpolation>();
    const bool initialized = runtime->Initialize(
        L"good-runtime", nullptr, 1920, 1088, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache");
    Check(initialized && runtime->SupportsCudaOutput(), "fake runtime must expose CUDA output leases");
    if (!initialized || !runtime->SupportsCudaOutput()) return;

    const auto module = GetModuleHandleW(runtime->GetModulePath().c_str());
    const auto setResults = reinterpret_cast<void(WINAPI*)(int, int)>(
        GetProcAddress(module, "FakeRifeSetOutputResults"));
    const auto d3dCalls = reinterpret_cast<uint32_t(WINAPI*)()>(
        GetProcAddress(module, "FakeRifeGetD3DCalls"));
    Check(setResults && d3dCalls, "fake output controls must be available");
    if (!setResults || !d3dCalls) return;

    auto* first = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(1));
    auto* second = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(2));
    auto* fallback = reinterpret_cast<ID3D11Texture2D*>(static_cast<uintptr_t>(3));
    ID3D11Texture2D* output = fallback;
    bool runtimeInvoked = false;
    MpcvrRifeStats stats{};
    uint32_t allocations = 0;
    auto acquireOutput = [&]() { ++allocations; return fallback; };
    auto allocationFailure = [&]() -> ID3D11Texture2D* { ++allocations; return nullptr; };
    auto interpolate = [&](std::shared_ptr<RifeCudaOutputLease>& lease, auto&& acquire) {
        return InterpolateRifeOutput(*runtime, 1, first, second, 0.25f, stats, 42, lease, output, runtimeInvoked, acquire);
    };

    const uint32_t initialD3DCalls = d3dCalls();
    auto lease = runtime->AcquireCudaOutput();
    Check(lease != nullptr, "CUDA lease must be acquired");
    if (!lease) return;
    Check(interpolate(lease, allocationFailure), "CUDA success must not depend on D3D allocation");
    Check(allocations == 0 && output == nullptr && lease && stats.inferenceMs == 2.0 && runtimeInvoked,
        "direct output must retain its lease and acquire no D3D texture");
    Check(d3dCalls() == initialD3DCalls, "direct output must not submit D3D inference");
    lease.reset();

    Check(interpolate(lease, acquireOutput), "missing CUDA output must use the D3D fallback");
    Check(allocations == 1 && output == fallback && !lease && stats.inferenceMs == 1.0,
        "D3D fallback must return its texture and final inference stats");

    setResults(MPCVR_RIFE_CUDA_FAILURE, MPCVR_RIFE_OK);
    lease = runtime->AcquireCudaOutput();
    Check(lease != nullptr, "CUDA lease must be reusable after direct output release");
    Check(interpolate(lease, acquireOutput), "failed CUDA inference must retry D3D in the same request");
    Check(allocations == 2 && output == fallback && !lease && stats.inferenceMs == 1.0,
        "failed CUDA output must release its lease before D3D fallback and replace failed stats");

    lease = runtime->AcquireCudaOutput();
    Check(lease != nullptr, "CUDA failure must release its output slot");
    const uint32_t callsBeforeAllocationFailure = d3dCalls();
    Check(!interpolate(lease, allocationFailure), "CUDA failure plus D3D allocation failure must fail cleanly");
    Check(allocations == 3 && output == nullptr && !lease && runtimeInvoked && d3dCalls() == callsBeforeAllocationFailure,
        "allocation failure must neither retain a failed lease nor submit a null D3D output");
    Check(!interpolate(lease, allocationFailure), "D3D-only allocation failure must fail cleanly");
    Check(allocations == 4 && output == nullptr && !runtimeInvoked && d3dCalls() == callsBeforeAllocationFailure,
        "D3D-only allocation failure must preserve the previous pair's deferred input ownership");

    setResults(MPCVR_RIFE_CUDA_FAILURE, MPCVR_RIFE_TENSORRT_FAILURE);
    lease = runtime->AcquireCudaOutput();
    Check(!interpolate(lease, acquireOutput), "both inference paths failing must report failure");
    Check(!lease && output == fallback, "failed CUDA lease must not escape a failed fallback");

    setResults(MPCVR_RIFE_OK, MPCVR_RIFE_OK);
    lease = runtime->AcquireCudaOutput();
    const uint32_t allocationsBeforeRecovery = allocations;
    Check(lease && interpolate(lease, allocationFailure), "CUDA output must recover on the next request");
    Check(allocations == allocationsBeforeRecovery && output == nullptr,
        "recovered CUDA output must still avoid D3D acquisition");
}
}

int wmain()
{
    static_assert(MPCVR_RIFE_OK == 0);
    static_assert(MPCVR_RIFE_INVALID_ARGUMENT == -1);
    static_assert(MPCVR_RIFE_UNSUPPORTED == -2);
    static_assert(MPCVR_RIFE_CUDA_FAILURE == -3);
    static_assert(MPCVR_RIFE_TENSORRT_FAILURE == -4);
    static_assert(MPCVR_RIFE_BUILDER_RESOURCE_MISSING == -5);
    static_assert(MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY == -6);
    static_assert(MPCVR_RIFE_UNSUPPORTED_TENSOR_FORMAT == -7);

    const auto missing = CRifeFrameInterpolation::Probe(L"missing-runtime");
    Check(!missing.available, "missing runtime must be reported as unavailable");

    const auto bad = CRifeFrameInterpolation::Probe(L"bad-runtime");
    Check(!bad.available, "ABI mismatch must be rejected");
    Check(bad.abiVersion == 999, "ABI mismatch must report the discovered version");

    const auto good = CRifeFrameInterpolation::Probe(L"good-runtime");
    Check(good.available, "matching runtime ABI must be accepted");
    Check(good.abiVersion == MPCVR_RIFE_RUNTIME_ABI, "matching runtime must report ABI 2");

    CRifeFrameInterpolation runtime;
    Check(runtime.Initialize(
        L"good-runtime", nullptr, 1920, 1088, 1920, 1080, UINT32_MAX, 2, false,
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

    TestLazyOutputFallback();

    CRifeFrameInterpolation badRuntime;
    Check(!badRuntime.Initialize(
        L"bad-runtime", nullptr, 1920, 1088, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache"),
        "ABI mismatch must fail persistent initialization");
    Check(!badRuntime.IsReady(), "failed initialization must not leave a ready runtime");

    CRifeFrameInterpolation unsupportedArchitecture;
    Check(!unsupportedArchitecture.Initialize(
        L"unsupported-cc-runtime", nullptr, 1920, 1088, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache"),
        "unsupported compute capability must fail initialization");
    Check(unsupportedArchitecture.GetStatus() == L"RIFE runtime does not support this CUDA compute capability",
        "unsupported architecture must identify compute capability");

    CRifeFrameInterpolation missingBuilderResource;
    Check(!missingBuilderResource.Initialize(
        L"builder-missing-runtime", nullptr, 1920, 1088, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache"),
        "missing TensorRT builder resource must fail initialization");
    Check(missingBuilderResource.GetStatus() == L"RIFE TensorRT builder resource for this GPU architecture is missing",
        "missing TensorRT builder resource must be explicit");

    CRifeFrameInterpolation futureFailure;
    Check(!futureFailure.Initialize(
        L"future-failure-runtime", nullptr, 1920, 1088, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache"),
        "unknown future runtime failure must fail initialization");
    Check(futureFailure.GetStatus().find(L"-99") != std::wstring::npos,
        "unknown future runtime failure must preserve its numeric code");

    // Production normally probes multiple candidate directories. A useful error
    // from a loadable/ABI-compatible runtime must survive later missing paths.
    SetEnvironmentVariableW(L"LOCALAPPDATA", L"missing-localappdata");
    SetEnvironmentVariableW(L"MPCVR_RIFE_RUNTIME_DIR", L"missing-env-runtime");
    CRifeFrameInterpolation candidateFallback;
    Check(!candidateFallback.Initialize(
        L"", nullptr, 1920, 1088, 1920, 1080, UINT32_MAX, 2, false,
        L"rife_v4.6.onnx", L"cache"),
        "multi-directory runtime lookup must fail for unsupported compute capability");
    Check(candidateFallback.GetStatus() == L"RIFE runtime does not support this CUDA compute capability",
        "actionable runtime initialization failure must survive later missing candidates");

    if (g_failures) {
        std::cerr << g_failures << " RIFE runtime ABI test(s) failed\n";
        return 1;
    }

    std::cout << "All RIFE runtime ABI tests passed\n";
    return 0;
}
