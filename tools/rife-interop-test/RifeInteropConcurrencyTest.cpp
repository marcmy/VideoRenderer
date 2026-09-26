#define NOMINMAX
#include <windows.h>
#include "../RifeTensorRTRuntime/D3D11InteropLock.h"
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
using Microsoft::WRL::ComPtr;
using Error=int;
using Resource=void*;
using Stream=void*;
using FnGetDevices=Error(*)(unsigned*,int*,unsigned,ID3D11Device*,int);
using FnSetDevice=Error(*)(int);
using FnRegister=Error(*)(Resource*,ID3D11Resource*,unsigned);
using FnUnregister=Error(*)(Resource);
using FnMap=Error(*)(int,Resource*,Stream);
using FnStreamCreate=Error(*)(Stream*,unsigned);
using FnStreamOp=Error(*)(Stream);
int wmain(int argc,wchar_t** argv) {
    const bool guarded=!(argc>2 && std::wstring(argv[2])==L"--unguarded");
    if(argc<2){std::cerr<<"Usage: RifeInteropConcurrencyTest.exe <cudart64_13.dll> [--unguarded]"<<std::endl;return 2;}
    const auto dll=std::filesystem::absolute(argv[1]);
    auto module=LoadLibraryW(dll.c_str()); if(!module)return 2;
    auto getDevices=reinterpret_cast<FnGetDevices>(GetProcAddress(module,"cudaD3D11GetDevices"));
    auto setDevice=reinterpret_cast<FnSetDevice>(GetProcAddress(module,"cudaSetDevice"));
    auto reg=reinterpret_cast<FnRegister>(GetProcAddress(module,"cudaGraphicsD3D11RegisterResource"));
    auto unreg=reinterpret_cast<FnUnregister>(GetProcAddress(module,"cudaGraphicsUnregisterResource"));
    auto map=reinterpret_cast<FnMap>(GetProcAddress(module,"cudaGraphicsMapResources"));
    auto unmap=reinterpret_cast<FnMap>(GetProcAddress(module,"cudaGraphicsUnmapResources"));
    auto streamCreate=reinterpret_cast<FnStreamCreate>(GetProcAddress(module,"cudaStreamCreateWithFlags"));
    auto streamSync=reinterpret_cast<FnStreamOp>(GetProcAddress(module,"cudaStreamSynchronize"));
    auto streamDestroy=reinterpret_cast<FnStreamOp>(GetProcAddress(module,"cudaStreamDestroy"));
    if(!getDevices||!setDevice||!reg||!unreg||!map||!unmap||!streamCreate||!streamSync||!streamDestroy)return 3;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> ctx;
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx);
    if(FAILED(hr)){std::cout<<"debug device failed "<<std::hex<<hr<<std::endl;return 4;}
    ComPtr<ID3D11Multithread> mt; if(FAILED(ctx.As(&mt)))return 5;
    mt->SetMultithreadProtected(TRUE);
    ComPtr<ID3D11InfoQueue> info; if(FAILED(device.As(&info)))return 6;
    unsigned count=0;int devices[8]{}; if(getDevices(&count,devices,8,device.Get(),1)||!count||setDevice(devices[0]))return 7;
    D3D11_TEXTURE2D_DESC td{};td.Width=640;td.Height=480;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> cudaTexture,drawTexture;
    if(FAILED(device->CreateTexture2D(&td,nullptr,&cudaTexture))||FAILED(device->CreateTexture2D(&td,nullptr,&drawTexture)))return 8;
    ComPtr<ID3D11RenderTargetView> target; if(FAILED(device->CreateRenderTargetView(drawTexture.Get(),nullptr,&target)))return 9;
    const char* shader="float4 vs(uint id:SV_VertexID):SV_Position { return float4(id==2?3:-1,id==1?3:-1,0,1); } float4 ps():SV_Target {return float4(0.1,0.2,0.3,1);}";
    ComPtr<ID3DBlob> vsCode,psCode,errors;
    if(FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"vs","vs_5_0",0,0,&vsCode,&errors))||FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"ps","ps_5_0",0,0,&psCode,&errors)))return 10;
    ComPtr<ID3D11VertexShader> vs; ComPtr<ID3D11PixelShader> ps;
    if(FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs))||FAILED(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps)))return 11;
    Resource resource=nullptr; if(reg(&resource,cudaTexture.Get(),0))return 12;
    Stream stream=nullptr;if(streamCreate(&stream,1))return 13;
    std::atomic<bool> stop=false;std::atomic<unsigned> maps=0;std::atomic<int> cudaError=0;
    info->ClearStoredMessages();
    std::thread worker([&]{setDevice(devices[0]);while(!stop.load()){
        Error e=0;{D3D11InteropLock guard(guarded?mt.Get():nullptr);e=map(1,&resource,stream);}if(e){cudaError=e;break;}
        {D3D11InteropLock guard(guarded?mt.Get():nullptr);e=unmap(1,&resource,stream);}if(e){cudaError=e;break;}
        e=streamSync(stream);if(e){cudaError=e;break;}++maps;
    }});
    D3D11_VIEWPORT viewport{0,0,640,480,0,1}; unsigned draws=0,debugErrors=0;
    auto until=std::chrono::steady_clock::now()+std::chrono::seconds(15);
    while(std::chrono::steady_clock::now()<until&&!cudaError.load()&&!debugErrors){
        // These resources never enter CUDA. The only shared object is the immediate context.
        {auto rtv=target.Get();ctx->OMSetRenderTargets(1,&rtv,nullptr);ctx->RSSetViewports(1,&viewport);ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);ctx->VSSetShader(vs.Get(),nullptr,0);ctx->PSSetShader(ps.Get(),nullptr,0);ctx->Draw(3,0);ctx->Flush();}
        ++draws;
        if((draws%20)==0){
            for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i){SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<char> data(size);auto msg=reinterpret_cast<D3D11_MESSAGE*>(data.data());if(SUCCEEDED(info->GetMessage(i,msg,&size))&&msg->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){++debugErrors;if(debugErrors<=12)std::cout<<msg->pDescription<<std::endl;}}
            info->ClearStoredMessages();
            Sleep(1);
        }
        if(FAILED(device->GetDeviceRemovedReason()))break;
    }
    stop=true;worker.join();
    hr=device->GetDeviceRemovedReason();
    std::cout<<"guarded="<<guarded<<" draws="<<draws<<" maps="<<maps<<" cudaError="<<cudaError<<" debugErrors="<<debugErrors<<" deviceReason="<<std::hex<<hr<<std::dec<<std::endl;
    {D3D11InteropLock guard(guarded?mt.Get():nullptr);unreg(resource);}streamDestroy(stream);
    return cudaError||debugErrors||FAILED(hr)?1:0;
}
