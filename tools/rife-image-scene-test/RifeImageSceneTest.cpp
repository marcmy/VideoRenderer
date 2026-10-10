#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include "../../Source/RifeImageSceneDetector.h"
#include <iostream>
#include <stdexcept>
#include <vector>

static void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

static HRESULT Compile(const std::string& source, const D3D_SHADER_MACRO* defines,
    LPCSTR target, ID3DBlob** code)
{
    CComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, defines, nullptr,
        "main", target, 0, 0, code, &errors);
    if (FAILED(hr) && errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
    return hr;
}

static HRESULT FailCompile(const std::string&, const D3D_SHADER_MACRO*, LPCSTR, ID3DBlob**)
{
    return E_FAIL;
}

// Independent scalar reference: full-image normalized absolute luma difference.
// Use doubles directly rather than the production integer tile reduction.
static bool Reference(UINT width, UINT height, UINT pitch,
    const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool& cut, double threshold)
{
    double mad = 0;
    uint64_t count = 0;
    for (UINT y = 0; y < height; ++y) {
        for (UINT x = 0; x < width; ++x) {
            const auto index = static_cast<size_t>(y) * pitch + x * 4;
            const double ya = (0.0722 * a[index] + 0.7152 * a[index + 1] + 0.2126 * a[index + 2]) / 255.0;
            const double yb = (0.0722 * b[index] + 0.7152 * b[index + 1] + 0.2126 * b[index + 2]) / 255.0;
            mad += std::abs(ya - yb); ++count;
        }
    }
    cut = false;
    if (!count) return false;
    cut = mad / static_cast<double>(count) > threshold;
    return true;
}

struct Fixture {
    CComPtr<ID3D11Device> device;
    CComPtr<ID3D11DeviceContext> context;
    CComPtr<ID3D11InfoQueue> debug;
    unsigned cases = 0;

    Fixture()
    {
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            D3D11_CREATE_DEVICE_DEBUG, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
        }
        Check(SUCCEEDED(hr), "CPU-only WARP device creation failed");
        device->QueryInterface(IID_PPV_ARGS(&debug));
    }

    CComPtr<ID3D11Texture2D> Texture(UINT width, UINT height, UINT pitch, const std::vector<uint8_t>& pixels)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width; desc.Height = height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        const D3D11_SUBRESOURCE_DATA data = {pixels.data(), pitch, 0};
        CComPtr<ID3D11Texture2D> texture;
        Check(SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)), "Source texture creation failed");
        return texture;
    }

    void Compare(CRifeImageSceneDetector& detector, UINT width, UINT height,
        const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, bool compact = true, double threshold = 0.15)
    {
        const UINT pitch = width * 4 + 12; // Non-tight source rows.
        auto first = Texture(width, height, pitch, a), second = Texture(width, height, pitch, b);
        bool expectedCut = false, actualCut = false;
        const bool expected = Reference(width, height, pitch, a, b, expectedCut, threshold);
        const bool actual = detector.Analyze(device, first, second, actualCut, threshold);
        Check(expected == actual && expectedCut == actualCut, "Cut decision differs from full-image reference");
        if (expected) Check(detector.UsedCompactReadback() == compact, "Unexpected readback path");
        ++cases;
    }

    void CheckDebug()
    {
        if (!debug) return;
        for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
            SIZE_T size = 0;
            debug->GetMessage(i, nullptr, &size);
            std::vector<uint8_t> data(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(data.data());
            if (SUCCEEDED(debug->GetMessage(i, message, &size))
                    && message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::cerr << message->pDescription << '\n';
                throw std::runtime_error("D3D debug layer reported a warning/error");
            }
        }
    }
};

int main()
{
    try {
        // Tile coverage includes edges and tiny images.
        for (UINT width = 0; width <= 4096; ++width) {
            for (const UINT height : {0u, 1u, 15u, 35u, 36u, 71u, 72u, 736u, 1088u, 1920u}) {
                const auto grid = RifeImageSampleGrid::ForSize(width, height);
                Check(grid.columns == (width + 15) / 16 && grid.rows == (height + 15) / 16,
                    "Reduction tiles omit edge pixels");
            }
        }
        Fixture fixture;
        CRifeImageSceneDetector detector(Compile);
        uint32_t random = 1;
        for (const auto [width, height] : std::array<std::pair<UINT, UINT>, 10>{
                {{1, 1}, {3, 5}, {4, 4}, {127, 71}, {320, 570}, {544, 960},
                 {1280, 736}, {1696, 960}, {1920, 1088}, {1088, 1920}}}) {
            const UINT pitch = width * 4 + 12;
            std::vector<uint8_t> a(static_cast<size_t>(pitch) * height), b(a.size());
            for (auto& value : a) { random = random * 1664525u + 1013904223u; value = static_cast<uint8_t>(random >> 24); }
            fixture.Compare(detector, width, height, a, a); // Identical textured frame.
            for (auto& value : b) { random = random * 1664525u + 1013904223u; value = static_cast<uint8_t>(random >> 24); }
            fixture.Compare(detector, width, height, a, b); // Hard cut/unrelated frames.
            b = a;
            for (UINT y = 0; y < height; ++y) {
                for (UINT x = 0; x < width; ++x) {
                    const UINT stepX=std::max(1u,width/64),stepY=std::max(1u,height/36);
                    const bool sampled = x >= stepX / 2 && y >= stepY / 2
                        && (x - stepX / 2) % stepX == 0 && (y - stepY / 2) % stepY == 0;
                    if (!sampled) for (UINT channel = 0; channel < 4; ++channel) b[static_cast<size_t>(y) * pitch + x * 4 + channel] ^= 255;
                }
            }
            fixture.Compare(detector, width, height, a, b); // Pixels outside the former sparse grid now count.
        }
        // Representative color/alpha and threshold extremes; no hardware GPU.
        constexpr UINT width = 64, height = 36, pitch = width * 4 + 12;
        std::vector<uint8_t> a(pitch * height), b(a.size());
        for (UINT channel = 0; channel < 4; ++channel) {
            for (UINT value : {0u,1u,14u,38u,51u,128u,255u}) {
                std::fill(b.begin(), b.end(), uint8_t{0});
                for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) b[y * pitch + x * 4 + channel] = static_cast<uint8_t>(value);
                for(double threshold:{0.0,0.05,0.15,0.2,1.0})
                    fixture.Compare(detector, width, height, a, b,true,threshold);
            }
        }
        // Restore a producer's output binding after executing the worker list.
        auto target = fixture.Texture(width, height, pitch, a);
        CComPtr<ID3D11RenderTargetView> targetView;
        Check(SUCCEEDED(fixture.device->CreateRenderTargetView(target, nullptr, &targetView)), "RTV creation failed");
        ID3D11RenderTargetView* bound = targetView;
        fixture.context->OMSetRenderTargets(1, &bound, nullptr);
        fixture.Compare(detector, width, height, a, b);
        CComPtr<ID3D11RenderTargetView> restored;
        fixture.context->OMGetRenderTargets(1, &restored, nullptr);
        Check(restored == targetView, "Immediate-context producer binding was not restored");
        fixture.context->OMSetRenderTargets(0, nullptr, nullptr);
        detector.Reset();
        fixture.Compare(detector, width, height, a, b); // Reset/reinitialization.
        CRifeImageSceneDetector fallback(FailCompile);
        fixture.Compare(fallback, width, height, a, b, false); // Shader failure preserves the new decision rule.
        for(double threshold:{0.0,0.05,0.15,1.0})
            fixture.Compare(fallback, width, height, a, b, false,threshold);
        // Exact boundary: uniform grayscale 51/255 is precisely 20%.
        std::fill(b.begin(),b.end(),uint8_t{51});
        auto black=fixture.Texture(width,height,pitch,a),gray=fixture.Texture(width,height,pitch,b);
        bool cut=false;
        Check(detector.Analyze(fixture.device,black,gray,cut,0.2)&&!cut,"Exact threshold must not cut");
        Check(detector.Analyze(fixture.device,black,gray,cut,0.199)&&cut,"Lower threshold must cut");
        Check(fallback.Analyze(fixture.device,black,gray,cut,0.2)&&!cut,"Fallback boundary mismatch");
        // Padding must neither dilute the cut nor count as picture content.
        std::fill(b.begin(),b.end(),uint8_t{0});
        for(UINT y=0;y<5;++y)for(UINT x=0;x<3;++x)
            for(UINT c=0;c<3;++c)b[y*pitch+x*4+c]=51;
        gray=fixture.Texture(width,height,pitch,b);
        for(CRifeImageSceneDetector* path:{&detector,&fallback}) {
            Check(path->Analyze(fixture.device,black,gray,cut,0.15,3,5)&&cut,"Padding diluted visible picture difference");
            Check(path->Analyze(fixture.device,black,gray,cut,0.15)&&!cut,"Content size change reused stale reduction constants");
        }
        fixture.CheckDebug();
        std::cout << "CPU-only WARP scene detector: " << fixture.cases
            << " parity cases passed; full-image tiles, thresholds, color/alpha, fallback and context restoration passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
