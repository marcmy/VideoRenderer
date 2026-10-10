#include "../RifeTensorRTRuntime/EngineCacheFile.h"
#include "../RifeTensorRTRuntime/EngineProfile.h"
#include "../RifeTensorRTRuntime/RuntimeCreation.h"
#include <array>
#include <barrier>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
unsigned checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
std::vector<uint8_t> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void NoStagingFiles(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        Check(entry.path().filename().wstring().find(L".tmp-") == std::wstring::npos,
            "an initializer left its staging file behind");
    }
}
struct FakeRuntime {
    inline static int result = MPCVR_RIFE_OK;
    inline static unsigned throwing = 0, live = 0;
    FakeRuntime() { ++live; }
    ~FakeRuntime() { --live; }
    int Initialize(const MpcvrRifeCreateParams&) {
        if (throwing == 1) throw std::filesystem::filesystem_error("model unavailable", std::make_error_code(std::errc::permission_denied));
        if (throwing == 2) throw std::bad_alloc();
        if (throwing == 3) throw 7;
        return result;
    }
};
void CreationCases() {
    MpcvrRifeCreateParams params{};
    void* handle = reinterpret_cast<void*>(1);
    Check(CreateRifeRuntime<FakeRuntime>(nullptr, &handle) == MPCVR_RIFE_INVALID_ARGUMENT && !handle, "null params retained a stale handle");
    Check(CreateRifeRuntime<FakeRuntime>(&params, nullptr) == MPCVR_RIFE_INVALID_ARGUMENT, "null output accepted");
    params.abiVersion = 999;
    Check(CreateRifeRuntime<FakeRuntime>(&params, &handle) == MPCVR_RIFE_INVALID_ARGUMENT && !handle, "bad ABI accepted");
    params.abiVersion = MPCVR_RIFE_RUNTIME_ABI;
    params.size = offsetof(MpcvrRifeCreateParams, flags) - 1;
    Check(CreateRifeRuntime<FakeRuntime>(&params, &handle) == MPCVR_RIFE_INVALID_ARGUMENT && !handle, "short prefix accepted");
    for (auto size : {static_cast<uint32_t>(offsetof(MpcvrRifeCreateParams, flags)), static_cast<uint32_t>(sizeof(params))}) {
        params.size = size;
        Check(CreateRifeRuntime<FakeRuntime>(&params, &handle) == MPCVR_RIFE_OK && handle && FakeRuntime::live == 1, "compatible ABI failed");
        delete static_cast<FakeRuntime*>(handle);
        handle = nullptr;
        Check(FakeRuntime::live == 0, "successful runtime leaked");
    }
    for (int failure : {MPCVR_RIFE_CUDA_FAILURE, MPCVR_RIFE_UNSUPPORTED_COMPUTE_CAPABILITY, MPCVR_RIFE_TENSORRT_FAILURE}) {
        FakeRuntime::result = failure;
        Check(CreateRifeRuntime<FakeRuntime>(&params, &handle) == failure && !handle && !FakeRuntime::live, "error code changed or failed runtime leaked");
    }
    FakeRuntime::result = MPCVR_RIFE_OK;
    for (unsigned throwing = 1; throwing <= 3; ++throwing) {
        FakeRuntime::throwing = throwing;
        Check(CreateRifeRuntime<FakeRuntime>(&params, &handle) == MPCVR_RIFE_TENSORRT_FAILURE && !handle && !FakeRuntime::live, "initialization exception escaped or leaked");
    }
    FakeRuntime::throwing = 0;
}
void ProfileCases() {
    Check(RifeEngineProfileCacheSuffix(1920, 1088, false) == "_dynamic_max3840x2176", "normal cache key changed");
    Check(RifeEngineProfileCacheSuffix(1920, 1088, true) == "_1920x1088_static", "boost cache key changed");
    Check(RifeEngineProfileCacheSuffix(64, 128, false) == "_dynamic_max3840x2176_min64x128", "small profile reused an incompatible cache");
    Check(RifeEngineProfileCacheSuffix(64, 128, true) == "_64x128_static", "small boost key changed");
    for (uint32_t width : {32u, 64u, 96u, 128u, 256u, 1920u, 3840u, 4320u}) {
        for (uint32_t height : {32u, 64u, 96u, 128u, 256u, 1088u, 2176u, 4320u}) {
            for (bool boost : {false, true}) {
                const auto profile = GetRifeEngineProfile(width, height, boost);
                Check(profile.minWidth <= width && profile.minHeight <= height
                    && profile.maxWidth >= width && profile.maxHeight >= height,
                    "current dimensions lie outside the optimization profile");
                if (boost) Check(profile.minWidth == width && profile.maxWidth == width
                    && profile.minHeight == height && profile.maxHeight == height, "boost stopped using fixed dimensions");
            }
        }
    }
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "pass a workspace fixture directory");
        const auto root = std::filesystem::absolute(argv[1]) / (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
        Check(std::filesystem::create_directories(root), "fixture already exists");
        std::vector<uint8_t> first((1u << 20) + 357, 0xA5), second(8193, 0x3C);
        const auto plan = root / L"engine.plan";
        Check(WriteFileAtomically(plan, first.data(), first.size()) && Read(plan) == first, "initial/chunked cache save failed");
        Check(WriteFileAtomically(plan, second.data(), second.size()) && Read(plan) == second, "replacement or truncation failed");
        Check(!WriteFileAtomically(plan, nullptr, 10) && Read(plan) == second, "null payload altered the cache");
        Check(!WriteFileAtomically(plan, first.data(), 0) && Read(plan) == second, "empty payload altered the cache");

        HANDLE held = CreateFileW(plan.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "could not lock cache fixture");
        const bool savedLocked = WriteFileAtomically(plan, first.data(), first.size());
        CloseHandle(held);
        Check(!savedLocked && Read(plan) == second, "failed replacement lost the previous cache");
        NoStagingFiles(root);

        const auto directory = root / L"directory.plan";
        std::filesystem::create_directory(directory);
        Check(!WriteFileAtomically(directory, first.data(), first.size()) && std::filesystem::is_directory(directory), "writer deleted a destination directory");
        Check(!WriteFileAtomically(plan / L"child.plan", first.data(), first.size()) && Read(plan) == second, "failed parent creation altered existing file");
        const auto unicode = root / L"cache-\u00e9-\u6d4b\u8bd5.plan";
        Check(WriteFileAtomically(unicode, second.data(), second.size()) && Read(unicode) == second, "Unicode path failed");

        const auto obsolete = std::filesystem::path(plan.wstring() + L".tmp");
        held = CreateFileW(obsolete.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(held != INVALID_HANDLE_VALUE, "could not lock old shared staging name");
        const bool savedWithOtherWriter = WriteFileAtomically(plan, first.data(), first.size());
        CloseHandle(held);
        Check(savedWithOtherWriter && Read(plan) == first, "a different initializer's staging name blocked saving");

        constexpr unsigned writers = 12;
        std::array<bool, writers> saved{};
        std::array<std::vector<uint8_t>, writers> payloads;
        std::barrier start(static_cast<std::ptrdiff_t>(writers));
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < writers; ++i) {
            payloads[i].assign(65536 + i * 17, static_cast<uint8_t>(i + 1));
            threads.emplace_back([&, i] { start.arrive_and_wait(); saved[i] = WriteFileAtomically(plan, payloads[i].data(), payloads[i].size()); });
        }
        for (auto& thread : threads) thread.join();
        const auto published = Read(plan);
        unsigned successes = 0;
        bool complete = false;
        for (unsigned i = 0; i < writers; ++i) {
            successes += saved[i] ? 1 : 0;
            complete |= saved[i] && published == payloads[i];
        }
        Check(successes > 0 && complete, "parallel publication produced a missing, partial or mixed cache");
        NoStagingFiles(root);
        CreationCases();
        ProfileCases();
        std::cout << "PASS " << checks << " cache/creation checks; " << successes
            << '/' << writers << " concurrent saves; CPU only\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
