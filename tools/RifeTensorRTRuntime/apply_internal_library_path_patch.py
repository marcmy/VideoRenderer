#!/usr/bin/env python3
from pathlib import Path

path = Path('tools/RifeTensorRTRuntime/RifeTensorRTRuntime.cpp')
text = path.read_text(encoding='utf-8')

anchor = '''constexpr uint32_t kPadMultiple = 32;\n\n'''
helper = r'''constexpr uint32_t kPadMultiple = 32;

std::filesystem::path ThisModuleDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&ThisModuleDirectory), &module) || !module) {
        return {};
    }

    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size()) return {};
    return std::filesystem::path(buffer.data()).parent_path();
}

'''
if text.count(anchor) != 1:
    raise SystemExit(f'expected one kPadMultiple anchor, found {text.count(anchor)}')
text = text.replace(anchor, helper, 1)

old = '''        m_device = params.device;\n        m_device->AddRef();\n\n        unsigned cudaCount = 0;\n'''
new = '''        m_device = params.device;\n        m_device->AddRef();\n\n        const auto runtimeDirectory = ThisModuleDirectory();\n        if (runtimeDirectory.empty()) return kTensorRtFailure;\n        const std::string internalLibraryPath = runtimeDirectory.string();\n        if (!nvinfer1::setInternalLibraryPath(internalLibraryPath.c_str())) {\n            return kTensorRtFailure;\n        }\n\n        unsigned cudaCount = 0;\n'''
if text.count(old) != 1:
    raise SystemExit(f'expected one Initialize anchor, found {text.count(old)}')
text = text.replace(old, new, 1)

path.write_text(text, encoding='utf-8')
