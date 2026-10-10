#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include "../../Source/RifeImageSceneDetector.h"
#include "../../Source/NvofAnalysisInput.h"
#include <iostream>
#include <vector>
#include <set>
#include <stdexcept>

void Check(bool ok, const char* label) { if (!ok) throw std::runtime_error(label); }
HRESULT Compile(const std::string& source, const D3D_SHADER_MACRO* defines, LPCSTR target, ID3DBlob** code) {
    CComPtr<ID3DBlob> errors;
    return D3DCompile(source.data(), source.size(), nullptr, defines, nullptr, "main", target, 0, 0, code, &errors);
}
HRESULT FailCompile(const std::string&, const D3D_SHADER_MACRO*, LPCSTR, ID3DBlob**) { return E_FAIL; }
CComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, const void* pixels = nullptr) {
    D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1; d.Format = format;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data = {pixels, w * RifeSurfaceBytes(format), 0};
    CComPtr<ID3D11Texture2D> result;
    Check(SUCCEEDED(device->CreateTexture2D(&d, pixels ? &data : nullptr, &result)), "texture"); return result;
}
int main() {
    CComPtr<ID3D11Device> device; CComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context)), "WARP");
    for (auto [w,h] : {std::pair{128u,128u}, {640u,512u}, {320u,576u}}) {
        std::vector<uint16_t> dark(size_t(w)*h*4, 0), light(dark.size(), 0x3b00);
        for (size_t i = 0; i < dark.size(); i += 4) dark[i+3] = light[i+3] = 0x3c00;
        auto a = Texture(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,dark.data());
        auto b = Texture(device,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,light.data());
        CRifeImageSceneDetector gpu(Compile), cpu(FailCompile);
        for (double threshold : {0.0,0.15,0.5,1.0}) {
            bool cg=false, cc=false;
            Check(gpu.Analyze(device,a,b,cg,threshold) && gpu.UsedCompactReadback(), "16F GPU analysis");
            Check(cpu.Analyze(device,a,b,cc,threshold) && !cpu.UsedCompactReadback(), "16F CPU fallback");
            Check(cg == cc && cg == (threshold < 0.875), "GPU/CPU cut parity");
            Check(gpu.Analyze(device,a,a,cg,threshold) && !cg, "16F unchanged frame");
        }
        auto size=NvofAnalysisSize::ForContent(w,h);
        auto oa=Texture(device,size.width,size.height,DXGI_FORMAT_B8G8R8A8_UNORM);
        auto ob=Texture(device,size.width,size.height,DXGI_FORMAT_B8G8R8A8_UNORM);
        CNvofAnalysisInput input(Compile);
        Check(input.Prepare(device,a,b,oa,ob,w,h), "float input to reduced BGRA scene analysis");
        bool cut=false; Check(gpu.Analyze(device,oa,ob,cut) && cut, "format switch rebuild and reduced hard cut");
        D3D11_TEXTURE2D_DESC d={}; ob->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING; d.BindFlags=0; d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        CComPtr<ID3D11Texture2D> readback; Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&readback)),"readback");
        context->CopyResource(readback,ob); D3D11_MAPPED_SUBRESOURCE mapped={};
        Check(SUCCEEDED(context->Map(readback,0,D3D11_MAP_READ,0,&mapped)),"read pixels");
        const auto* p=static_cast<uint8_t*>(mapped.pData);
        Check(p[0]==223 && p[1]==223 && p[2]==223 && p[3]==255,"RGBA float to BGRA byte order"); context->Unmap(readback,0);
    }
    Check(RifeHalfToFloat(0x3c00)==1.0f && RifeHalfToFloat(1)==std::ldexp(1.0f,-24),"half normal/subnormal");
    std::cout << "16F WARP: image cuts, fallback parity, float scene reduction and format transitions passed\n";
}
