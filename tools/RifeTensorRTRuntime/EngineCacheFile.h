#pragma once

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

// Each initializer owns its staging file. A failed publication must never
// delete the last usable plan (or a directory occupying the destination).
inline bool WriteFileAtomically(const std::filesystem::path& path, const void* data, size_t size)
{
    if (!data || !size) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    static std::atomic_uint64_t nextFile = 0;
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        temporary = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId())
            + L"-" + std::to_wstring(nextFile.fetch_add(1, std::memory_order_relaxed));
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;

    struct StagingFile final {
        HANDLE handle;
        const std::filesystem::path& path;
        ~StagingFile() {
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
            DeleteFileW(path.c_str());
        }
    } staging{file, temporary};

    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(staging.handle, bytes + offset, chunk, &written, nullptr) || !written) return false;
        offset += written;
    }
    if (!FlushFileBuffers(staging.handle)) return false;
    const BOOL closed = CloseHandle(staging.handle);
    staging.handle = INVALID_HANDLE_VALUE;
    if (!closed) return false;

    return MoveFileExW(temporary.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

inline void ReportEngineCacheSaveFailure(const std::filesystem::path& path)
{
    const auto message = L"MPCVR RIFE: engine cache save failed: " + path.wstring()
        + L"; using the in-memory engine, next initialization may rebuild it\n";
    OutputDebugStringW(message.c_str());
}
