#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include "../../Source/NvofAnalysisInput.h"
#include "../../Source/NvofInverseFlow.h"
#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>

static unsigned checks = 0, pixelCases = 0;
static void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static HRESULT Compile(const std::string& source, const D3D_SHADER_MACRO* macros, LPCSTR target, ID3DBlob** code) {
    CComPtr<ID3DBlob> errors;
    const auto result = D3DCompile(source.data(), source.size(), nullptr, macros, nullptr,
        "main", target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS, 0, code, &errors);
    if (FAILED(result) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
    return result;
}
static unsigned failedCompiles = 0;
static HRESULT FailCompile(const std::string&, const D3D_SHADER_MACRO*, LPCSTR, ID3DBlob**) { ++failedCompiles; return E_FAIL; }

struct Fixture {
    CComPtr<ID3D11Device> device;
    CComPtr<ID3D11DeviceContext> context;
    CComPtr<ID3D11InfoQueue> debug;
    Fixture() {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_DEBUG,
            &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(result)) result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        Check(SUCCEEDED(result), "CPU-only WARP creation failed");
        device->QueryInterface(IID_PPV_ARGS(&debug));
    }
    CComPtr<ID3D11Texture2D> Texture(UINT width, UINT height, const std::vector<uint8_t>* pixels = nullptr,
        UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = bind;
        D3D11_SUBRESOURCE_DATA data = {pixels ? pixels->data() : nullptr, width * 4, 0};
        CComPtr<ID3D11Texture2D> result;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &result)), "Texture creation failed");
        return result;
    }
    std::vector<uint8_t> Read(ID3D11Texture2D* texture) {
        D3D11_TEXTURE2D_DESC desc = {}; texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CComPtr<ID3D11Texture2D> staging;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Staging creation failed");
        context->CopyResource(staging, texture);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        Check(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)), "WARP readback failed");
        std::vector<uint8_t> pixels(static_cast<size_t>(desc.Width) * desc.Height * 4);
        for (UINT y = 0; y < desc.Height; ++y) memcpy(pixels.data() + static_cast<size_t>(y) * desc.Width * 4,
            static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, desc.Width * 4);
        context->Unmap(staging, 0); return pixels;
    }
    void CheckDebug() {
        if (!debug) return;
        for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0; debug->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> data(size); auto* message = reinterpret_cast<D3D11_MESSAGE*>(data.data());
            if (SUCCEEDED(debug->GetMessage(i, message, &size)) && message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::cerr << message->pDescription << '\n';
                Check(false, "D3D debug warning/error");
            }
        }
    }
};

static std::vector<uint8_t> Pixels(UINT width, UINT height, UINT cw, UINT ch, unsigned pattern, uint8_t padding) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4, padding);
    for (UINT y = 0; y < ch; ++y) for (UINT x = 0; x < cw; ++x) for (UINT c = 0; c < 4; ++c) {
        unsigned value = 0;
        if (pattern == 0) value = 31 + c * 51; // Constant, including alpha.
        if (pattern == 1) value = ((x ^ y) & 1) ? 255 : 0; // Aliasing stress.
        if (pattern == 2) value = (x * 37 + y * 61 + c * 89) % 256;
        if (pattern == 3) value = (x == cw - 1 || y == ch - 1) ? 255 : 0; // Last visible edges.
        pixels[(static_cast<size_t>(y) * width + x) * 4 + c] = static_cast<uint8_t>(value);
    }
    return pixels;
}

// Independent double-precision weighted-box reference at actual output dimensions.
static void Compare(const std::vector<uint8_t>& source, UINT sourceWidth, UINT cw, UINT ch,
    const std::vector<uint8_t>& actual, UINT ow, UINT oh) {
    for (UINT y = 0; y < oh; ++y) for (UINT x = 0; x < ow; ++x) {
        const double left = static_cast<double>(x) * cw / ow, right = static_cast<double>(x + 1) * cw / ow;
        const double top = static_cast<double>(y) * ch / oh, bottom = static_cast<double>(y + 1) * ch / oh;
        std::array<double, 4> sums = {};
        for (UINT sy = static_cast<UINT>(top); sy < std::min(ch, static_cast<UINT>(std::ceil(bottom))); ++sy) {
            const double wy = std::min(bottom, sy + 1.0) - std::max(top, static_cast<double>(sy));
            for (UINT sx = static_cast<UINT>(left); sx < std::min(cw, static_cast<UINT>(std::ceil(right))); ++sx) {
                const double weight = wy * (std::min(right, sx + 1.0) - std::max(left, static_cast<double>(sx)));
                for (UINT c = 0; c < 4; ++c) sums[c] += weight * source[(static_cast<size_t>(sy) * sourceWidth + sx) * 4 + c];
            }
        }
        for (UINT c = 0; c < 4; ++c) {
            const double expected = sums[c] / ((right - left) * (bottom - top));
            Check(std::abs(actual[(static_cast<size_t>(y) * ow + x) * 4 + c] - expected) <= 1.01,
                "Reduced pixel differs from independent area reference");
        }
    }
    ++pixelCases;
}

static void RunGeometry(Fixture& fixture, CNvofAnalysisInput& reducer, UINT cw, UINT ch, UINT padX, UINT padY) {
    const UINT sw = cw + padX, sh = ch + padY;
    const auto size = NvofAnalysisSize::ForContent(cw, ch);
    auto firstOutput = fixture.Texture(size.width, size.height), secondOutput = fixture.Texture(size.width, size.height);
    for (unsigned pattern = 0; pattern < 4; ++pattern) {
        auto a = Pixels(sw, sh, cw, ch, pattern, 0), b = Pixels(sw, sh, cw, ch, pattern, 255);
        auto first = fixture.Texture(sw, sh, &a), second = fixture.Texture(sw, sh, &b);
        Check(reducer.Prepare(fixture.device, first, second, firstOutput, secondOutput, cw, ch), "Content reduction failed");
        auto pa = fixture.Read(firstOutput), pb = fixture.Read(secondOutput);
        Check(pa == pb, "Model padding leaked into NVOF analysis");
        Compare(a, sw, cw, ch, pa, size.width, size.height);
        Compare(b, sw, cw, ch, pb, size.width, size.height);
    }
}

int main() {
    try {
        // Reduction thresholds, odd edges and source-space vector units.
        for (UINT w = 0; w <= 4096; ++w) for (const UINT h : {0u, 1u, 127u, 255u, 256u, 479u, 512u, 1080u, 1920u}) {
            const auto size = NvofAnalysisSize::ForContent(w, h);
            Check(size.width <= w && size.height <= h, "Analysis enlarged a source");
            if (w && h) {
                Check(size.width && size.height, "Empty analysis for valid source");
                const auto vector = size.SourceVector(static_cast<float>(size.width) / w * 11,
                    -static_cast<float>(size.height) / h * 7, w, h);
                Check(std::abs(vector[0] - 11) < 0.00001f && std::abs(vector[1] + 7) < 0.00001f,
                    "Flow magnitude changed units after resizing");
            }
        }
        Check(NvofAnalysisSize::ForContent(1920, 1080).width == 480, "1080p reduction policy wrong");
        Check(NvofAnalysisSize::ForContent(854, 480).width == 427, "480p reduction policy wrong");
        Check(NvofAnalysisSize::ForContent(320, 570).height == 285, "Portrait reduction cropped an edge");
        // Inverse warp must happen before source-unit conversion.
        float bx = 0, by = 0;
        Check(SampleNvofInverseFlow(2, 2, 2, -1, 10, 10, 4,
            [](UINT x, UINT y) { return std::array<float, 2>{-2.0f * x, -3.0f * y}; }, bx, by), "Inverse lookup failed");
        const auto vector = NvofAnalysisSize{427, 240}.SourceVector(bx, by, 854, 480);
        Check(std::abs(vector[0] + 10) < 0.00001f && std::abs(vector[1] + 10.5f) < 0.00001f,
            "Inverse lookup and source units disagree");
        Fixture fixture;
        CNvofAnalysisInput reducer(Compile);
        for (const auto dims : std::array<std::array<UINT, 4>, 11>{{
                {1, 1, 3, 3}, {127, 71, 1, 1}, {319, 255, 1, 1}, {320, 256, 0, 0},
                {320, 570, 0, 6}, {544, 960, 0, 0}, {854, 480, 10, 0}, {853, 479, 11, 1},
                {1280, 720, 0, 16}, {1920, 1080, 0, 8}, {1080, 1920, 8, 0}}}) {
            RunGeometry(fixture, reducer, dims[0], dims[1], dims[2], dims[3]);
        }
        // Renderer bindings must survive, even when the pair is already bound.
        auto sentinel = fixture.Texture(32, 32);
        CComPtr<ID3D11RenderTargetView> sentinelView;
        Check(SUCCEEDED(fixture.device->CreateRenderTargetView(sentinel, nullptr, &sentinelView)), "Sentinel RTV failed");
        ID3D11RenderTargetView* rt = sentinelView; fixture.context->OMSetRenderTargets(1, &rt, nullptr);
        const D3D11_VIEWPORT viewport = {2, 3, 13, 17, 0.1f, 0.9f}; fixture.context->RSSetViewports(1, &viewport);
        fixture.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        RunGeometry(fixture, reducer, 643, 515, 29, 29);
        CComPtr<ID3D11RenderTargetView> afterView; fixture.context->OMGetRenderTargets(1, &afterView, nullptr);
        D3D11_VIEWPORT afterViewport = {}; UINT count = 1; fixture.context->RSGetViewports(&count, &afterViewport);
        D3D11_PRIMITIVE_TOPOLOGY topology; fixture.context->IAGetPrimitiveTopology(&topology);
        Check(afterView == sentinelView && topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST
            && count == 1 && memcmp(&viewport, &afterViewport, sizeof(viewport)) == 0, "Shared renderer state changed");
        fixture.context->ClearState();

        // View-cache eviction: keep the first pair view alive across a second-view miss.
        const auto size = NvofAnalysisSize::ForContent(320, 256);
        auto a = Pixels(320, 256, 320, 256, 2, 0);
        std::vector<CComPtr<ID3D11Texture2D>> sources;
        for (unsigned i = 0; i < 15; ++i) sources.push_back(fixture.Texture(320, 256, &a));
        auto outA = fixture.Texture(size.width, size.height), outB = fixture.Texture(size.width, size.height);
        reducer.Reset();
        for (unsigned i = 0; i < 12; i += 2) Check(reducer.Prepare(fixture.device, sources[i], sources[i + 1], outA, outB, 320, 256), "Cache fill failed");
        Check(reducer.Prepare(fixture.device, sources[0], sources[12], outA, outB, 320, 256), "Cache eviction failed");
        Compare(a, 320, 320, 256, fixture.Read(outA), size.width, size.height);
        Compare(a, 320, 320, 256, fixture.Read(outB), size.width, size.height);
        CNvofAnalysisInput unavailable(FailCompile);
        Check(!unavailable.Prepare(fixture.device, sources[0], sources[1], outA, outB, 320, 256), "Failed shader did not signal fallback");
        Check(!unavailable.Prepare(fixture.device, sources[0], sources[1], outA, outB, 320, 256)
            && failedCompiles == 1, "Unavailable shader was recompiled on every pair");
        Check(!reducer.Prepare(fixture.device, sources[0], sources[1], outA, outA, 320, 256), "Aliased targets accepted");
        Check(!reducer.Prepare(fixture.device, sources[0], sources[1], outA, outB, 321, 256), "Out-of-bounds content accepted");
        Check(!reducer.Prepare(fixture.device, nullptr, sources[1], outA, outB, 320, 256), "Null source accepted");
        auto noView = fixture.Texture(320, 256, &a, 0);
        Check(!reducer.Prepare(fixture.device, noView, sources[1], outA, outB, 320, 256), "Non-SRV input accepted");
        auto wrongFormat = fixture.Texture(320, 256, &a, D3D11_BIND_SHADER_RESOURCE, DXGI_FORMAT_R8G8B8A8_UNORM);
        Check(!reducer.Prepare(fixture.device, wrongFormat, sources[1], outA, outB, 320, 256), "Wrong-format input accepted");
        Fixture other;
        Check(!reducer.Prepare(other.device, sources[0], sources[1], outA, outB, 320, 256), "Foreign-device texture accepted");
        RunGeometry(other, reducer, 854, 480, 10, 0); // Recreate resources for another device.
        reducer.Reset();
        RunGeometry(fixture, reducer, 854, 480, 10, 0);
        fixture.CheckDebug(); other.CheckDebug();
        std::cout << "PASS: " << pixelCases << " WARP image comparisons, " << checks
                  << " assertions; geometry, padding, flow units, cache lifetime, state, fallback and device reset.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
