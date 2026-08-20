#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

enum NvOFFRUCResourceType : int {
    DirectX11Resource = 1,
};

enum NvOFFRUCSurfaceFormat : int {
    ARGBSurface = 1,
};

enum NvOFFRUCCUDAResourceType : int {
    CudaResourceTypeUndefined = -1,
};

enum NvOFFRUCStatus : int {
    NvOFFRUC_SUCCESS = 0,
};

struct CUctx_st;
using CUcontext = CUctx_st*;
using CUresult = int;
using PFN_cuCtxGetCurrent = CUresult (WINAPI*)(CUcontext*);
using PFN_cuCtxSetCurrent = CUresult (WINAPI*)(CUcontext);
constexpr CUresult CUDA_SUCCESS = 0;

union NvOFFRUCSyncWait {
    struct {
        uint64_t uiFenceValueToWaitOn;
    } FenceWaitValue;
    struct {
        uint64_t uiKeyForRenderTextureAcquire;
        uint64_t uiKeyForInterpTextureAcquire;
    } MutexAcquireKey;
};

union NvOFFRUCSyncSignal {
    struct {
        uint64_t uiFenceValueToSignalOn;
    } FenceSignalValue;
    struct {
        uint64_t uiKeyForRenderTextureRelease;
        uint64_t uiKeyForInterpolateRelease;
    } MutexReleaseKey;
};

struct NvOFFRUCCreateParam {
    uint32_t uiWidth;
    uint32_t uiHeight;
    void* pDevice;
    NvOFFRUCResourceType eResourceType;
    NvOFFRUCSurfaceFormat eSurfaceFormat;
    NvOFFRUCCUDAResourceType eCUDAResourceType;
    uint32_t uiReserved[32];
};

struct NvOFFRUCFrameData {
    void* pFrame;
    double nTimeStamp;
    size_t nCuSurfacePitch;
    bool* bHasFrameRepetitionOccurred;
    uint32_t uiReserved[32];
};

struct NvOFFRUCProcessInParams {
    NvOFFRUCFrameData stFrameDataInput;
    uint32_t bSkipWarp : 1;
    NvOFFRUCSyncWait uSyncWait;
    uint32_t uiReserved[32];
};

struct NvOFFRUCProcessOutParams {
    NvOFFRUCFrameData stFrameDataOutput;
    NvOFFRUCSyncSignal uSyncSignal;
    uint32_t uiReserved[32];
};

struct NvOFFRUCRegisterResourceParam {
    void* pArrResource[10];
    void* pD3D11FenceObj;
    uint32_t uiCount;
};

struct NvOFFRUCUnregisterResourceParam {
    void* pArrResource[10];
    uint32_t uiCount;
};

struct NvOFFRUCHandleObject;
using NvOFFRUCHandle = NvOFFRUCHandleObject*;
using PFN_NvOFFRUCCreate = NvOFFRUCStatus (CALLBACK*)(const NvOFFRUCCreateParam*, NvOFFRUCHandle*);
using PFN_NvOFFRUCRegisterResource = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle, const NvOFFRUCRegisterResourceParam*);
using PFN_NvOFFRUCUnregisterResource = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle, const NvOFFRUCUnregisterResourceParam*);
using PFN_NvOFFRUCProcess = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle, const NvOFFRUCProcessInParams*, const NvOFFRUCProcessOutParams*);
using PFN_NvOFFRUCDestroy = NvOFFRUCStatus (CALLBACK*)(NvOFFRUCHandle);

struct BmpImage {
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> rgba;
};

struct SharedTexture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<IDXGIKeyedMutex> mutex;
    uint64_t key = 0;
};

const wchar_t* StatusName(const NvOFFRUCStatus status)
{
    switch (static_cast<int>(status)) {
    case 0: return L"success";
    case 1: return L"not supported";
    case 2: return L"invalid pointer";
    case 3: return L"invalid parameter";
    case 4: return L"invalid handle";
    case 5: return L"out of system memory";
    case 6: return L"out of video memory";
    case 9: return L"optical-flow failure";
    case 12: return L"incorrect API sequence";
    case 14: return L"pipeline execution failure";
    case 15: return L"synchronization failure";
    default: return L"runtime error";
    }
}

std::filesystem::path ExecutableDirectory()
{
    std::array<wchar_t, 32768> path = {};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        return {};
    }
    return std::filesystem::path(path.data()).parent_path();
}

void AddUniqueDirectory(std::vector<std::filesystem::path>& directories, std::filesystem::path directory)
{
    if (directory.empty()) {
        return;
    }
    directory = directory.lexically_normal();
    if (std::find(directories.begin(), directories.end(), directory) == directories.end()) {
        directories.push_back(std::move(directory));
    }
}

std::vector<std::filesystem::path> RuntimeSearchDirectories()
{
    std::vector<std::filesystem::path> directories;
    std::array<wchar_t, 32768> value = {};
    for (const wchar_t* variable : {L"NV_OFFRUC_PATH", L"NVIDIA_OPTICAL_FLOW_SDK_PATH"}) {
        value.fill(0);
        const DWORD length = GetEnvironmentVariableW(variable, value.data(), static_cast<DWORD>(value.size()));
        if (length && length < value.size()) {
            const std::filesystem::path root(value.data());
            AddUniqueDirectory(directories, root);
            AddUniqueDirectory(directories, root / L"NvOFFRUC" / L"NvOFFRUCSample" / L"bin" / L"win64");
        }
    }

    const auto exeDir = ExecutableDirectory();
    AddUniqueDirectory(directories, exeDir);
    AddUniqueDirectory(directories, exeDir / L"NvOFFRUC");
    AddUniqueDirectory(directories, std::filesystem::current_path());
    AddUniqueDirectory(directories, std::filesystem::current_path() / L"NvOFFRUC");
    return directories;
}

bool LoadBmp32(const std::filesystem::path& path, BmpImage& image, std::wstring& error)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = L"Could not open " + path.wstring();
        return false;
    }

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    stream.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader));
    stream.read(reinterpret_cast<char*>(&infoHeader), sizeof(infoHeader));
    if (!stream || fileHeader.bfType != 0x4d42 || infoHeader.biSize < sizeof(BITMAPINFOHEADER)
            || infoHeader.biWidth <= 0 || infoHeader.biHeight == 0 || infoHeader.biBitCount != 32
            || (infoHeader.biCompression != BI_RGB && infoHeader.biCompression != BI_BITFIELDS)) {
        error = L"Expected an uncompressed 32-bit BMP: " + path.wstring();
        return false;
    }

    const UINT width = static_cast<UINT>(infoHeader.biWidth);
    const LONG signedHeight = infoHeader.biHeight;
    const UINT height = static_cast<UINT>(signedHeight < 0 ? -signedHeight : signedHeight);
    const size_t rowBytes = static_cast<size_t>(width) * 4u;
    if (rowBytes > static_cast<size_t>(std::numeric_limits<DWORD>::max())) {
        error = L"BMP row is too wide";
        return false;
    }

    stream.seekg(fileHeader.bfOffBits, std::ios::beg);
    std::vector<uint8_t> bgra(rowBytes * height);
    stream.read(reinterpret_cast<char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    if (!stream) {
        error = L"BMP pixel data is truncated: " + path.wstring();
        return false;
    }

    image.width = width;
    image.height = height;
    image.rgba.resize(bgra.size());
    for (UINT y = 0; y < height; ++y) {
        const UINT sourceY = signedHeight > 0 ? (height - 1u - y) : y;
        const uint8_t* source = bgra.data() + static_cast<size_t>(sourceY) * rowBytes;
        uint8_t* target = image.rgba.data() + static_cast<size_t>(y) * rowBytes;
        for (UINT x = 0; x < width; ++x) {
            target[x * 4u + 0u] = source[x * 4u + 2u];
            target[x * 4u + 1u] = source[x * 4u + 1u];
            target[x * 4u + 2u] = source[x * 4u + 0u];
            target[x * 4u + 3u] = source[x * 4u + 3u];
        }
    }
    return true;
}

bool SaveTextureBmp(ID3D11Device* device, ID3D11DeviceContext* context,
    ID3D11Texture2D* texture, const std::filesystem::path& path, std::wstring& error)
{
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    texture->GetDesc(&sourceDesc);
    if (sourceDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
        error = L"FRUC output texture has an unexpected format";
        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(hr)) {
        error = L"Could not create FRUC readback texture";
        return false;
    }

    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        error = L"Could not map FRUC output texture";
        return false;
    }

    const size_t rowBytes = static_cast<size_t>(sourceDesc.Width) * 4u;
    std::vector<uint8_t> bgra(rowBytes * sourceDesc.Height);
    for (UINT y = 0; y < sourceDesc.Height; ++y) {
        const auto* source = static_cast<const uint8_t*>(mapped.pData)
            + static_cast<size_t>(sourceDesc.Height - 1u - y) * mapped.RowPitch;
        uint8_t* target = bgra.data() + static_cast<size_t>(y) * rowBytes;
        for (UINT x = 0; x < sourceDesc.Width; ++x) {
            target[x * 4u + 0u] = source[x * 4u + 2u];
            target[x * 4u + 1u] = source[x * 4u + 1u];
            target[x * 4u + 2u] = source[x * 4u + 0u];
            target[x * 4u + 3u] = source[x * 4u + 3u];
        }
    }
    context->Unmap(staging.Get(), 0);

    BITMAPFILEHEADER fileHeader = {};
    BITMAPINFOHEADER infoHeader = {};
    fileHeader.bfType = 0x4d42;
    fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(bgra.size());
    infoHeader.biSize = sizeof(BITMAPINFOHEADER);
    infoHeader.biWidth = static_cast<LONG>(sourceDesc.Width);
    infoHeader.biHeight = static_cast<LONG>(sourceDesc.Height);
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = BI_RGB;
    infoHeader.biSizeImage = static_cast<DWORD>(bgra.size());

    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        error = L"Could not create " + path.wstring();
        return false;
    }
    stream.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
    stream.write(reinterpret_cast<const char*>(&infoHeader), sizeof(infoHeader));
    stream.write(reinterpret_cast<const char*>(bgra.data()), static_cast<std::streamsize>(bgra.size()));
    if (!stream) {
        error = L"Could not write " + path.wstring();
        return false;
    }
    return true;
}

bool CreateNvidiaDevice(ComPtr<IDXGIAdapter1>& adapter, ComPtr<ID3D11Device>& device,
    ComPtr<ID3D11DeviceContext>& context, std::wstring& adapterName)
{
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        return false;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(candidate->GetDesc1(&desc)) || desc.VendorId != 0x10de) {
            continue;
        }

        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(candidate.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &device, &featureLevel, &context);
        if (SUCCEEDED(hr)) {
            adapter = candidate;
            adapterName = desc.Description;
            return true;
        }
    }
    return false;
}

bool CreateSharedTexture(ID3D11Device* device, UINT width, UINT height,
    SharedTexture& target, std::wstring& error)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, &target.texture);
    if (FAILED(hr)) {
        error = L"CreateTexture2D(shared FRUC surface) failed";
        return false;
    }
    hr = target.texture.As(&target.mutex);
    if (FAILED(hr)) {
        error = L"IDXGIKeyedMutex is unavailable on the FRUC surface";
        return false;
    }
    return true;
}

bool UploadFrame(ID3D11DeviceContext* context, SharedTexture& target,
    const BmpImage& image, std::wstring& error)
{
    constexpr DWORD timeoutMs = 5000;
    const HRESULT acquire = target.mutex->AcquireSync(target.key, timeoutMs);
    if (acquire != S_OK) {
        error = L"Could not acquire the FRUC input keyed mutex";
        return false;
    }
    ++target.key;
    context->UpdateSubresource(target.texture.Get(), 0, nullptr,
        image.rgba.data(), image.width * 4u, 0);
    context->Flush();
    const HRESULT release = target.mutex->ReleaseSync(target.key);
    if (FAILED(release)) {
        error = L"Could not release the FRUC input keyed mutex";
        return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    const std::filesystem::path captureDir = argc >= 2
        ? std::filesystem::path(argv[1])
        : std::filesystem::current_path();
    const auto frameAPath = captureDir / L"frame-A.bmp";
    const auto frameBPath = captureDir / L"frame-B.bmp";
    const auto outputPath = captureDir / L"fruc-midpoint.bmp";

    std::wcout << L"NVIDIA NvOFFRUC same-pair replay test\n"
               << L"====================================\n";

    BmpImage frameA;
    BmpImage frameB;
    std::wstring error;
    if (!LoadBmp32(frameAPath, frameA, error) || !LoadBmp32(frameBPath, frameB, error)) {
        std::wcerr << L"ERROR: " << error << L"\n";
        return 2;
    }
    if (frameA.width != frameB.width || frameA.height != frameB.height) {
        std::wcerr << L"ERROR: input frame dimensions do not match.\n";
        return 2;
    }
    std::wcout << L"Capture: " << frameA.width << L"x" << frameA.height << L"\n";

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::wstring adapterName;
    if (!CreateNvidiaDevice(adapter, device, context, adapterName)) {
        std::wcerr << L"ERROR: could not create a D3D11 device on an NVIDIA adapter.\n";
        return 3;
    }
    std::wcout << L"Adapter: " << adapterName << L"\n";

    HMODULE cudaDriver = LoadLibraryW(L"nvcuda.dll");
    const auto ctxGetCurrent = cudaDriver
        ? reinterpret_cast<PFN_cuCtxGetCurrent>(GetProcAddress(cudaDriver, "cuCtxGetCurrent"))
        : nullptr;
    const auto ctxSetCurrent = cudaDriver
        ? reinterpret_cast<PFN_cuCtxSetCurrent>(GetProcAddress(cudaDriver, "cuCtxSetCurrent"))
        : nullptr;
    if (!ctxGetCurrent || !ctxSetCurrent) {
        std::wcerr << L"ERROR: CUDA driver context APIs are unavailable.\n";
        if (cudaDriver) FreeLibrary(cudaDriver);
        return 4;
    }

    HMODULE cudaRuntime = nullptr;
    HMODULE frucModule = nullptr;
    std::filesystem::path runtimeDirectory;
    const auto searchDirectories = RuntimeSearchDirectories();
    for (const auto& directory : searchDirectories) {
        const auto frucPath = directory / L"NvOFFRUC.dll";
        if (!std::filesystem::is_regular_file(frucPath)) {
            continue;
        }
        const auto cudaPath = directory / L"cudart64_110.dll";
        if (std::filesystem::is_regular_file(cudaPath)) {
            cudaRuntime = LoadLibraryExW(cudaPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        frucModule = LoadLibraryExW(frucPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (frucModule) {
            runtimeDirectory = directory;
            break;
        }
        if (cudaRuntime) {
            FreeLibrary(cudaRuntime);
            cudaRuntime = nullptr;
        }
    }
    if (!frucModule) {
        std::wcerr << L"ERROR: NvOFFRUC.dll was not found. Searched:\n";
        for (const auto& directory : searchDirectories) {
            std::wcerr << L"  " << directory.wstring() << L"\n";
        }
        FreeLibrary(cudaDriver);
        return 5;
    }
    std::wcout << L"NvOFFRUC runtime: " << runtimeDirectory.wstring() << L"\n";

    const auto Create = reinterpret_cast<PFN_NvOFFRUCCreate>(GetProcAddress(frucModule, "NvOFFRUCCreate"));
    const auto RegisterResource = reinterpret_cast<PFN_NvOFFRUCRegisterResource>(GetProcAddress(frucModule, "NvOFFRUCRegisterResource"));
    const auto UnregisterResource = reinterpret_cast<PFN_NvOFFRUCUnregisterResource>(GetProcAddress(frucModule, "NvOFFRUCUnregisterResource"));
    const auto Process = reinterpret_cast<PFN_NvOFFRUCProcess>(GetProcAddress(frucModule, "NvOFFRUCProcess"));
    const auto Destroy = reinterpret_cast<PFN_NvOFFRUCDestroy>(GetProcAddress(frucModule, "NvOFFRUCDestroy"));
    if (!Create || !RegisterResource || !UnregisterResource || !Process || !Destroy) {
        std::wcerr << L"ERROR: NvOFFRUC.dll is missing required exports.\n";
        FreeLibrary(frucModule);
        if (cudaRuntime) FreeLibrary(cudaRuntime);
        FreeLibrary(cudaDriver);
        return 6;
    }

    SharedTexture inputA;
    SharedTexture inputB;
    SharedTexture output;
    if (!CreateSharedTexture(device.Get(), frameA.width, frameA.height, inputA, error)
            || !CreateSharedTexture(device.Get(), frameA.width, frameA.height, inputB, error)
            || !CreateSharedTexture(device.Get(), frameA.width, frameA.height, output, error)) {
        std::wcerr << L"ERROR: " << error << L"\n";
        FreeLibrary(frucModule);
        if (cudaRuntime) FreeLibrary(cudaRuntime);
        FreeLibrary(cudaDriver);
        return 7;
    }

    CUcontext callerContext = nullptr;
    const bool haveCallerContext = ctxGetCurrent(&callerContext) == CUDA_SUCCESS;
    NvOFFRUCHandle handle = nullptr;
    CUcontext frucContext = nullptr;

    NvOFFRUCCreateParam createParams = {};
    createParams.uiWidth = frameA.width;
    createParams.uiHeight = frameA.height;
    createParams.pDevice = device.Get();
    createParams.eResourceType = DirectX11Resource;
    createParams.eSurfaceFormat = ARGBSurface;
    createParams.eCUDAResourceType = CudaResourceTypeUndefined;
    NvOFFRUCStatus code = Create(&createParams, &handle);
    if (ctxGetCurrent(&frucContext) != CUDA_SUCCESS) {
        frucContext = nullptr;
    }
    if (code != NvOFFRUC_SUCCESS || !handle) {
        std::wcerr << L"ERROR: NvOFFRUCCreate failed: " << StatusName(code)
                   << L" (" << static_cast<int>(code) << L")\n";
        if (haveCallerContext) ctxSetCurrent(callerContext);
        FreeLibrary(frucModule);
        if (cudaRuntime) FreeLibrary(cudaRuntime);
        FreeLibrary(cudaDriver);
        return 8;
    }

    NvOFFRUCRegisterResourceParam registerParams = {};
    registerParams.pArrResource[0] = output.texture.Get();
    registerParams.pArrResource[1] = inputA.texture.Get();
    registerParams.pArrResource[2] = inputB.texture.Get();
    registerParams.uiCount = 3;
    code = RegisterResource(handle, &registerParams);
    if (ctxGetCurrent(&frucContext) != CUDA_SUCCESS) {
        frucContext = nullptr;
    }
    if (code != NvOFFRUC_SUCCESS || !frucContext) {
        std::wcerr << L"ERROR: NvOFFRUCRegisterResource failed: " << StatusName(code)
                   << L" (" << static_cast<int>(code) << L")\n";
        Destroy(handle);
        if (haveCallerContext) ctxSetCurrent(callerContext);
        FreeLibrary(frucModule);
        if (cudaRuntime) FreeLibrary(cudaRuntime);
        FreeLibrary(cudaDriver);
        return 9;
    }
    if (haveCallerContext) {
        ctxSetCurrent(callerContext);
    }

    auto ActivateFruc = [&]() -> CUcontext {
        CUcontext previous = nullptr;
        if (ctxGetCurrent(&previous) != CUDA_SUCCESS) {
            return nullptr;
        }
        if (previous != frucContext && ctxSetCurrent(frucContext) != CUDA_SUCCESS) {
            return nullptr;
        }
        return previous;
    };

    auto Submit = [&](SharedTexture& input, const BmpImage& frame,
        double inputTime, double outputTime, bool& repeated, double& processMs) -> bool {
        if (!UploadFrame(context.Get(), input, frame, error)) {
            return false;
        }

        NvOFFRUCProcessInParams inParams = {};
        NvOFFRUCProcessOutParams outParams = {};
        inParams.stFrameDataInput.pFrame = input.texture.Get();
        inParams.stFrameDataInput.nTimeStamp = inputTime;
        inParams.uSyncWait.MutexAcquireKey.uiKeyForRenderTextureAcquire = input.key;
        inParams.uSyncWait.MutexAcquireKey.uiKeyForInterpTextureAcquire = output.key;
        outParams.stFrameDataOutput.pFrame = output.texture.Get();
        outParams.stFrameDataOutput.nTimeStamp = outputTime;
        outParams.stFrameDataOutput.bHasFrameRepetitionOccurred = &repeated;
        outParams.uSyncSignal.MutexReleaseKey.uiKeyForRenderTextureRelease = ++input.key;
        outParams.uSyncSignal.MutexReleaseKey.uiKeyForInterpolateRelease = ++output.key;

        CUcontext previous = ActivateFruc();
        if (!previous && callerContext != nullptr) {
            error = L"Could not activate the NvOFFRUC CUDA context";
            return false;
        }
        const auto started = std::chrono::steady_clock::now();
        const NvOFFRUCStatus processStatus = Process(handle, &inParams, &outParams);
        processMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        if (previous || callerContext == nullptr) {
            ctxSetCurrent(previous);
        }
        if (processStatus != NvOFFRUC_SUCCESS) {
            error = L"NvOFFRUCProcess failed: " + std::wstring(StatusName(processStatus));
            return false;
        }
        return true;
    };

    bool repeatedA = false;
    bool repeatedMidpoint = false;
    double processA = 0.0;
    double processMidpoint = 0.0;
    if (!Submit(inputA, frameA, 0.0, 0.0, repeatedA, processA)
            || !Submit(inputB, frameB, 1.0, 0.5, repeatedMidpoint, processMidpoint)) {
        std::wcerr << L"ERROR: " << error << L"\n";
        CUcontext previous = ActivateFruc();
        NvOFFRUCUnregisterResourceParam unregisterParams = {};
        unregisterParams.pArrResource[0] = output.texture.Get();
        unregisterParams.pArrResource[1] = inputA.texture.Get();
        unregisterParams.pArrResource[2] = inputB.texture.Get();
        unregisterParams.uiCount = 3;
        UnregisterResource(handle, &unregisterParams);
        Destroy(handle);
        ctxSetCurrent(previous);
        FreeLibrary(frucModule);
        if (cudaRuntime) FreeLibrary(cudaRuntime);
        FreeLibrary(cudaDriver);
        return 10;
    }

    constexpr DWORD outputTimeoutMs = 5000;
    if (output.mutex->AcquireSync(output.key, outputTimeoutMs) != S_OK) {
        std::wcerr << L"ERROR: timed out acquiring the FRUC midpoint output.\n";
        return 11;
    }
    ++output.key;
    const bool saved = SaveTextureBmp(device.Get(), context.Get(), output.texture.Get(), outputPath, error);
    const HRESULT releaseOutput = output.mutex->ReleaseSync(output.key);
    if (!saved || FAILED(releaseOutput)) {
        std::wcerr << L"ERROR: " << (saved ? L"Could not release output keyed mutex" : error) << L"\n";
        return 12;
    }

    CUcontext previous = ActivateFruc();
    NvOFFRUCUnregisterResourceParam unregisterParams = {};
    unregisterParams.pArrResource[0] = output.texture.Get();
    unregisterParams.pArrResource[1] = inputA.texture.Get();
    unregisterParams.pArrResource[2] = inputB.texture.Get();
    unregisterParams.uiCount = 3;
    UnregisterResource(handle, &unregisterParams);
    Destroy(handle);
    ctxSetCurrent(previous);

    std::wcout << L"Prime process: " << processA << L" ms, repeated=" << (repeatedA ? L"yes" : L"no") << L"\n"
               << L"Midpoint process: " << processMidpoint << L" ms, repeated=" << (repeatedMidpoint ? L"yes" : L"no") << L"\n"
               << L"Saved: " << outputPath.wstring() << L"\n"
               << L"FRUC REPLAY RESULT: PASS\n";

    FreeLibrary(frucModule);
    if (cudaRuntime) FreeLibrary(cudaRuntime);
    FreeLibrary(cudaDriver);
    return 0;
}
