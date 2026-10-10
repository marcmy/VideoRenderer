#include "../../Source/SvpSceneDetector.h"
#include <d3d11sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <thread>

unsigned checks=0;
void Check(bool success,const char* label) {++checks;if(!success){fprintf(stderr,"FAIL %s\n",label);exit(1);}}
HRESULT Compile(const std::string& source,const D3D_SHADER_MACRO* defines,LPCSTR target,ID3DBlob** result) {
    CComPtr<ID3DBlob> errors;
    HRESULT hr=D3DCompile(source.data(),source.size(),nullptr,defines,nullptr,"main",target,D3DCOMPILE_ENABLE_STRICTNESS,0,result,&errors);
    if(FAILED(hr)&&errors) fprintf(stderr,"%s\n",static_cast<const char*>(errors->GetBufferPointer()));
    return hr;
}
HRESULT FailCompile(const std::string&,const D3D_SHADER_MACRO*,LPCSTR,ID3DBlob**) {return E_FAIL;}
using Image=std::vector<uint8_t>;
CComPtr<ID3D11Texture2D> Texture(ID3D11Device* device,UINT width,UINT height,const Image& pixels,bool srv=true) {
    D3D11_TEXTURE2D_DESC desc={};desc.Width=width;desc.Height=height;
    desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags=srv?D3D11_BIND_SHADER_RESOURCE:0;
    desc.Usage=D3D11_USAGE_DEFAULT;
    D3D11_SUBRESOURCE_DATA initial={pixels.data(),width*4,0};
    CComPtr<ID3D11Texture2D> texture;
    Check(SUCCEEDED(device->CreateTexture2D(&desc,&initial,&texture)),"create source");return texture;
}
Image Pixels(UINT pw,UINT ph,UINT w,UINT h,int brightness,int padding=0) {
    Image pixels(pw*ph*4,padding);
    for(UINT y=0;y<h;++y) for(UINT x=0;x<w;++x) {
        auto p=pixels.data()+(size_t(y)*pw+x)*4;
        p[0]=p[1]=p[2]=brightness;p[3]=255;
    }
    return pixels;
}
CComPtr<ID3D11Device> CreateDevice() {
    CComPtr<ID3D11Device> device;
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr);
    if(FAILED(hr)) hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,nullptr);
    Check(SUCCEEDED(hr),"CPU WARP device");return device;
}
int main() {
    auto device=CreateDevice();
    CComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    CComPtr<ID3D11InfoQueue> messages;device->QueryInterface(IID_PPV_ARGS(&messages));
    CSvpSceneDetector detector(Compile);SvpSceneResult result;
    for(auto [w,h] : {std::pair{854u,480u},{1920u,1080u},{320u,570u},{544u,960u},{65u,97u}}) {
        UINT pw=(w+31)&~31u,ph=(h+31)&~31u;
        auto a=Texture(device,pw,ph,Pixels(pw,ph,w,h,32,255));
        auto same=Texture(device,pw,ph,Pixels(pw,ph,w,h,32,0));
        auto b=Texture(device,pw,ph,Pixels(pw,ph,w,h,220,0));
        // Deliberately retain graphics state; the analysis recorder must restore it.
        auto target=Texture(device,64,64,Pixels(64,64,64,64,0));
        D3D11_VIEWPORT viewport={11,13,29,31,0,1};context->RSSetViewports(1,&viewport);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        Check(detector.Analyze(device,a,same,w,h,result)&&!result.cut,"exclude model padding");
        Check(result.meanError==0,"identical visible pixels");
        D3D11_PRIMITIVE_TOPOLOGY topology;context->IAGetPrimitiveTopology(&topology);
        UINT count=1;D3D11_VIEWPORT actual={};context->RSGetViewports(&count,&actual);
        Check(topology==D3D11_PRIMITIVE_TOPOLOGY_LINELIST&&actual.TopLeftX==11&&actual.Width==29,"restore renderer state");
        Check(detector.Analyze(device,a,b,w,h,result)&&result.cut,"hard cut through GPU readback");
        printf("%ux%u -> %ux%u cut coverage %.3f\n",w,h,detector.Size().width,detector.Size().height,result.badFraction);
        for(unsigned i=0;i<14;++i) {
            auto fresh=Texture(device,pw,ph,Pixels(pw,ph,w,h,32+i,255-i));
            Check(detector.Analyze(device,a,fresh,w,h,result)&&!result.cut,"cache eviction fresh source");
        }
        // Reuse exact texture pointer with new data; no pointer-only decision cache.
        auto changed=Pixels(pw,ph,w,h,220,255);
        context->UpdateSubresource(same,0,nullptr,changed.data(),pw*4,0);
        Check(detector.Analyze(device,a,same,w,h,result)&&result.cut,"mutable texture new pair");
        context->UpdateSubresource(same,0,nullptr,Pixels(pw,ph,w,h,32,255).data(),pw*4,0);
        Check(detector.Analyze(device,a,same,w,h,result)&&!result.cut,"mutable texture restored pair");
    }
    auto a=Texture(device,1280,736,Pixels(1280,736,1280,720,0));
    auto b=Texture(device,1280,736,Pixels(1280,736,1280,720,255));
    CSvpSceneDetector failing(FailCompile);
    Check(!failing.Analyze(device,a,b,1280,720,result),"shader failure returns fallback");
    auto noSrv=Texture(device,1280,736,Pixels(1280,736,1280,720,0),false);
    Check(!detector.Analyze(device,noSrv,b,1280,720,result),"unsupported source returns fallback");
    Check(!detector.Analyze(device,a,b,0,720,result),"empty content returns fallback");
    Check(!detector.Analyze(device,a,b,1281,720,result),"out-of-bounds content returns fallback");
    auto otherDevice=CreateDevice();
    Check(!detector.Analyze(otherDevice,a,b,1280,720,result),"cross-device rejection");
    auto c=Texture(otherDevice,320,180,Pixels(320,180,320,180,0));
    auto d=Texture(otherDevice,320,180,Pixels(320,180,320,180,255));
    Check(detector.Analyze(otherDevice,c,d,320,180,result)&&result.cut,"device transition");
    detector.Reset();
    Check(detector.Analyze(device,a,b,1280,720,result)&&result.cut,"reset and device return");
    // Parallel adapters share only the protected immediate context.
    std::thread one([&]{CSvpSceneDetector own(Compile);SvpSceneResult r;for(int i=0;i<12;++i)if(!own.Analyze(device,a,a,1280,720,r)||r.cut)abort();});
    std::thread two([&]{CSvpSceneDetector own(Compile);SvpSceneResult r;for(int i=0;i<12;++i)if(!own.Analyze(device,a,b,1280,720,r)||!r.cut)abort();});
    one.join();two.join();
    if(messages) for(UINT64 i=0;i<messages->GetNumStoredMessages();++i) {
        SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> data(size);
        auto* message=reinterpret_cast<D3D11_MESSAGE*>(data.data());messages->GetMessage(i,message,&size);
        if(message->Severity==D3D11_MESSAGE_SEVERITY_ERROR||message->Severity==D3D11_MESSAGE_SEVERITY_CORRUPTION) {
            fprintf(stderr,"D3D validation: %s\n",message->pDescription);return 2;
        }
    }
    printf("PASS %u WARP assertions plus 24 concurrent analyses; debug layer %s\n",checks,messages?"on":"unavailable");
}
