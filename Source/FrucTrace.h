#pragma once

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace FrucTrace
{
inline std::atomic_uint64_t& Sequence() noexcept
{
	static std::atomic_uint64_t sequence = 0;
	return sequence;
}

inline void Write(const char* format, ...) noexcept
{
	char body[1536] = {};
	va_list args;
	va_start(args, format);
	_vsnprintf_s(body, sizeof(body), _TRUNCATE, format, args);
	va_end(args);

	const unsigned long long sequence = Sequence().fetch_add(1, std::memory_order_relaxed) + 1;
	const unsigned long long ticks = GetTickCount64();
	char line[1792] = {};
	const int lineLength = _snprintf_s(
		line, sizeof(line), _TRUNCATE,
		"%010llu tick=%llu pid=%lu tid=%lu %s\r\n",
		sequence, ticks, GetCurrentProcessId(), GetCurrentThreadId(), body);
	if (lineLength <= 0) {
		return;
	}

	wchar_t base[MAX_PATH] = {};
	wchar_t directory[MAX_PATH] = {};
	wchar_t path[MAX_PATH] = {};
	const DWORD baseLength = GetEnvironmentVariableW(L"LOCALAPPDATA", base, _countof(base));
	if (baseLength > 0 && baseLength < _countof(base)) {
		_snwprintf_s(directory, _countof(directory), _TRUNCATE, L"%s\\MPCVR", base);
	}
	else {
		const DWORD tempLength = GetTempPathW(_countof(base), base);
		if (!tempLength || tempLength >= _countof(base)) {
			return;
		}
		_snwprintf_s(directory, _countof(directory), _TRUNCATE, L"%sMPCVR", base);
	}
	CreateDirectoryW(directory, nullptr);
	_snwprintf_s(path, _countof(path), _TRUNCATE, L"%s\\NvOFFRUC-Diagnostic.log", directory);

	const HANDLE file = CreateFileW(
		path,
		FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr,
		OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);
	if (file != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(file, line, static_cast<DWORD>(lineLength), &written, nullptr);
		CloseHandle(file);
	}
	OutputDebugStringA(line);
}
} // namespace FrucTrace

#define FRUC_TRACE(...) FrucTrace::Write(__VA_ARGS__)
