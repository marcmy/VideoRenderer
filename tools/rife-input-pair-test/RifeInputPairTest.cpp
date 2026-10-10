#include "../RifeTensorRTRuntime/PackedInputReuse.h"
#include "../RifeTensorRTRuntime/RifeInputTimestep.h"
#include "../RifeTensorRTRuntime/RifeRequestCompatibility.h"

#include <array>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

namespace {
int failures = 0;
size_t checks = 0;

void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

void TestCache()
{
    int textures[4] = {};
    PackedInputReuse cache;
    auto* a = &textures[0];
    auto* b = &textures[1];
    auto* c = &textures[2];
    Check(!cache.Begin(1, a, b), "first pair must pack");
    cache.Commit(1, a, b);
    Check(cache.Begin(1, a, b), "same committed pair must reuse");
    Check(!cache.Begin(1, a, b), "failure without commit must invalidate reuse");
    cache.Commit(1, a, b);
    Check(!cache.Begin(2, a, b), "recycled pool textures require a new pack");
    cache.Commit(2, a, b);
    Check(!cache.Begin(2, b, a), "source roles must match");
    cache.Commit(2, a, b);
    Check(!cache.Begin(2, a, c), "different source texture must pack");
    cache.Commit(2, a, b);
    cache.Invalidate();
    Check(!cache.Begin(2, a, b), "drain/reset must invalidate");
    cache.Commit(0, a, b);
    Check(!cache.Begin(0, a, b), "legacy caller must always pack");
    cache.Commit(2, a, b);
    Check(!cache.Begin(2, nullptr, b), "invalid inputs must invalidate");
    Check(!cache.Begin(2, a, b), "invalid request must not retain the old pair");

    std::array<PackedInputReuse, 3> contexts;
    std::array<uint64_t, 3> committedIds{};
    std::array<size_t, 3> committedFirst{}, committedSecond{};
    std::mt19937 rng(2818);
    for (size_t i = 0; i < 20'000; ++i) {
        const size_t context = rng() % contexts.size();
        const uint64_t id = rng() % 6;
        const size_t first = rng() % 4, second = rng() % 4;
        const bool expected = id && committedIds[context] == id
            && committedFirst[context] == first && committedSecond[context] == second;
        Check(contexts[context].Begin(id, &textures[first], &textures[second]) == expected,
            "independent context reuse must follow content generation");
        committedIds[context] = 0;
        // Model a failed inference, a reset/drain, or successful completion.
        if (rng() % 4) {
            contexts[context].Commit(id, &textures[first], &textures[second]);
            committedIds[context] = id;
            committedFirst[context] = first;
            committedSecond[context] = second;
        }
    }
}

template <typename T> T Convert(float value) { return static_cast<T>(value); }
template <> __half Convert(float value) { return __float2half_rn(value); }

template <typename T>
void TestTimestep()
{
    constexpr std::array<std::array<int, 4>, 7> shapes = {{
        {1, 1, 32, 32}, {33, 35, 64, 64}, {320, 570, 320, 576},
        {544, 960, 544, 960}, {1280, 720, 1280, 736},
        {1920, 1080, 1920, 1088}, {1080, 1920, 1152, 1920}
    }};
    constexpr std::array<float, 7> times = {0.0001f, 0.1f, 0.25f, 1.0f / 3.0f, 0.5f, 0.75f, 0.9999f};
    for (const auto& shape : shapes) {
        const auto [sourceWidth, sourceHeight, width, height] = shape;
        const size_t plane = static_cast<size_t>(width) * height;
        std::vector<T> tensor(11 * plane + 16);
        for (size_t i = 0; i < tensor.size(); ++i) {
            tensor[i] = Convert<T>(static_cast<float>(static_cast<int>(i % 1023) - 511) / 511.0f);
        }
        for (float t : times) {
            auto expected = tensor;
            // Independent original full-pack timestep formula.
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    expected[6 * plane + static_cast<size_t>(y) * width + x] =
                        Convert<T>(x < sourceWidth && y < sourceHeight ? t : 0.0f);
                }
            }
            // Execute the exact production kernel element on CPU, including
            // rounded launch dimensions. No CUDA device/runtime is used.
            for (int y = 0; y < ((height + 15) / 16) * 16; ++y) {
                for (int x = 0; x < ((width + 15) / 16) * 16; ++x) {
                    RifeUpdateInputTimestepAt(tensor.data(), x, y,
                        sourceWidth, sourceHeight, width, height, t);
                }
            }
            Check(std::memcmp(tensor.data(), expected.data(), tensor.size() * sizeof(T)) == 0,
                "timestep must match full pack bit-for-bit and preserve all ten other channels/padding/canary");
        }
    }
}

void TestCompatibility()
{
    struct OldRequest {
        uint32_t size = sizeof(OldRequest);
        uint32_t contextIndex = 2;
        ID3D11Texture2D* first = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{1});
        ID3D11Texture2D* second = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{2});
        ID3D11Texture2D* output = reinterpret_cast<ID3D11Texture2D*>(uintptr_t{3});
        float timestep = 0.25f;
    };
    static_assert(sizeof(OldRequest) == offsetof(MpcvrRifeRequest, inputPairId));
    struct GuardedOldRequest { OldRequest request; uint64_t canary = UINT64_MAX; } old;
    MpcvrRifeRequest normalized;
    Check(CopyRifeRuntimeRequest(reinterpret_cast<const MpcvrRifeRequest*>(&old.request), normalized),
        "old ABI-2 request prefix must be accepted");
    Check(normalized.contextIndex == 2 && normalized.first == old.request.first
        && normalized.timestep == 0.25f && normalized.inputPairId == 0,
        "old ABI-2 request must preserve fields and disable reuse");

    MpcvrRifeRequest request;
    request.inputPairId = 123456;
    for (uint32_t size = 0; size <= sizeof(request); ++size) {
        request.size = size;
        const bool accepted = CopyRifeRuntimeRequest(&request, normalized);
        Check(accepted == (size >= sizeof(OldRequest)), "request size compatibility must follow legacy prefix");
        if (accepted) Check(normalized.inputPairId == (size == sizeof(request) ? request.inputPairId : 0),
            "partial generation field must never enable reuse");
    }
    Check(!CopyRifeRuntimeRequest(nullptr, normalized), "null request must be rejected");

    MpcvrRifeStats stats;
    stats.inferenceMs = 3.5;
    stats.inputPairReuse = 2;
    for (uint32_t size = static_cast<uint32_t>(offsetof(MpcvrRifeStats, inputMapMs));
            size <= sizeof(stats); ++size) {
        alignas(MpcvrRifeStats) std::array<unsigned char, sizeof(stats) + 16> guarded;
        guarded.fill(0xa5);
        auto* destination = reinterpret_cast<MpcvrRifeStats*>(guarded.data());
        destination->size = size;
        CopyRifeRuntimeStats(destination, stats);
        Check(destination->size == size && destination->inferenceMs == 3.5,
            "legacy stats must retain caller size and prefix values");
        bool canaryOk = true;
        for (size_t i = size; i < guarded.size(); ++i) canaryOk &= guarded[i] == 0xa5;
        Check(canaryOk, "stats copy must not overwrite any byte beyond caller allocation");
    }
}
}

int main()
{
    TestCache();
    TestTimestep<float>();
    TestTimestep<__half>();
    TestCompatibility();
    std::cout << "RIFE input-pair CPU tests: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
