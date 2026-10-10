#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <atlbase.h>
#include <cuda_runtime.h>
#include <cuda_d3d11_interop.h>
#include <cuda_fp16.h>
#include "../../Source/RifeRuntimeApi.h"
#include "../RifeTensorRTRuntime/RifeKernels.h"
#include <filesystem>
#include <iostream>
#include <vector>
#include <set>
#include <array>
#include <algorithm>
#include <stdexcept>
#include <fstream>

void Check(bool ok, const char* label) { if(!ok) throw std::runtime_error(label); }
void Cuda(cudaError_t e, const char* label) { if(e!=cudaSuccess) { std::cerr<<label<<": "<<cudaGetErrorString(e)<<'\n'; throw std::runtime_error(label); } }
CComPtr<ID3D11Texture2D> Texture(ID3D11Device* device, UINT w, UINT h, bool precise, const void* pixels=nullptr) {
    D3D11_TEXTURE2D_DESC d={};d.Width=w;d.Height=h;d.ArraySize=d.MipLevels=d.SampleDesc.Count=1;
    d.Format=precise?DXGI_FORMAT_R16G16B16A16_FLOAT:DXGI_FORMAT_B8G8R8A8_UNORM; d.Usage=D3D11_USAGE_DEFAULT;
    d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    D3D11_SUBRESOURCE_DATA data={pixels,w*(precise?8u:4u),0}; CComPtr<ID3D11Texture2D> result;
    Check(SUCCEEDED(device->CreateTexture2D(&d,pixels?&data:nullptr,&result)),"D3D texture");return result;
}
std::vector<uint8_t> Read(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d={};texture->GetDesc(&d);const unsigned pixel=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CComPtr<ID3D11Texture2D> staging;Check(SUCCEEDED(device->CreateTexture2D(&d,nullptr,&staging)),"staging");
    context->CopyResource(staging,texture);D3D11_MAPPED_SUBRESOURCE m={};Check(SUCCEEDED(context->Map(staging,0,D3D11_MAP_READ,0,&m)),"map");
    std::vector<uint8_t> result(size_t(d.Width)*d.Height*pixel);
    for(UINT y=0;y<d.Height;++y) memcpy(result.data()+size_t(y)*d.Width*pixel,static_cast<uint8_t*>(m.pData)+size_t(y)*m.RowPitch,d.Width*pixel);
    context->Unmap(staging,0);return result;
}
void Gradient(ID3D11Device* device,ID3D11DeviceContext* context) {
    constexpr int w=1024,h=32; std::vector<ushort4> pixels(w*h);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) pixels[y*w+x]=make_ushort4(__half_as_ushort(__float2half(float(x)/1023)),0x3400,0x3800,0x3c00);
    auto source=Texture(device,w,h,true,pixels.data()), output=Texture(device,w,h,true);
    cudaGraphicsResource_t resources[2]={};
    Cuda(cudaGraphicsD3D11RegisterResource(&resources[0],source,0),"register float source");
    Cuda(cudaGraphicsD3D11RegisterResource(&resources[1],output,cudaGraphicsRegisterFlagsSurfaceLoadStore),"register float output");
    Cuda(cudaGraphicsMapResources(2,resources),"map float");cudaArray_t a=nullptr,b=nullptr;
    Cuda(cudaGraphicsSubResourceGetMappedArray(&a,resources[0],0,0),"source array");Cuda(cudaGraphicsSubResourceGetMappedArray(&b,resources[1],0,0),"output array");
    __half* tensor=nullptr;Cuda(cudaMalloc(&tensor,size_t(w)*h*11*sizeof(__half)),"tensor");
    cudaTextureObject_t ta=0,tb=0;cudaSurfaceObject_t surface=0;
    Cuda(MpcvrRifePackInput(a,a,tensor,true,w,h,w,h,.5f,nullptr,&ta,&tb,true),"float pack");
    Cuda(MpcvrRifeWriteOutput(tensor,true,b,w,h,w,h,nullptr,&surface,true),"float write");Cuda(cudaDeviceSynchronize(),"gradient finish");
    cudaDestroyTextureObject(ta);cudaDestroyTextureObject(tb);cudaDestroySurfaceObject(surface);cudaFree(tensor);
    Cuda(cudaGraphicsUnmapResources(2,resources),"unmap");for(auto r:resources) Cuda(cudaGraphicsUnregisterResource(r),"unregister");
    auto bytes=Read(device,context,output);Check(bytes.size()==pixels.size()*sizeof(ushort4),"gradient size");
    Check(memcmp(bytes.data(),pixels.data(),bytes.size())==0,"1024-level RGBA16F round trip exact");
    std::set<uint16_t> levels;for(int x=0;x<w;++x) levels.insert(reinterpret_cast<uint16_t*>(bytes.data())[x*4]);
    Check(levels.size()==1024,"10-bit gradient retains all 1024 levels");std::cout<<"CUDA/D3D 16F gradient: exact 1024-level round trip\n";
}
int wmain(int argc,wchar_t** argv) {
    try {
    Check(argc==4,"usage: runtime model-folder private-cache");
    CComPtr<ID3D11Device> device;CComPtr<ID3D11DeviceContext> context;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context)),"GPU device");
    CComPtr<ID3D11Multithread> threading;Check(SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&threading))),"thread protection");threading->SetMultithreadProtected(TRUE);
    unsigned count=0;int ordinal=-1;Cuda(cudaD3D11GetDevices(&count,&ordinal,1,device,cudaD3D11DeviceListAll),"CUDA D3D device");Cuda(cudaSetDevice(ordinal),"set device");
    Gradient(device,context);
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) std::cerr << "LoadLibrary error " << GetLastError() << '\n';
    Check(module!=nullptr,"load runtime");
    auto capabilities=reinterpret_cast<MpcvrRifeGetCapabilitiesFn>(GetProcAddress(module,"MpcvrRifeGetCapabilities"));
    Check(capabilities && capabilities()==3,"runtime capabilities");
    auto create=reinterpret_cast<MpcvrRifeCreateFn>(GetProcAddress(module,"MpcvrRifeCreate"));
    auto infer=reinterpret_cast<MpcvrRifeInterpolateFn>(GetProcAddress(module,"MpcvrRifeInterpolate"));
    auto destroy=reinterpret_cast<MpcvrRifeDestroyFn>(GetProcAddress(module,"MpcvrRifeDestroy"));Check(create&&infer&&destroy,"exports");
    std::filesystem::create_directories(argv[3]);
    for(const auto name:{L"rife_v4.15_lite.onnx",L"rife_v4.25.onnx",L"rife_v4.25_lite.onnx"}) {
        const auto model=(std::filesystem::path(argv[2])/name).wstring();
        const bool supported = std::wstring(name) != L"rife_v4.25_lite.onnx";
        for (const bool precise : {false, true}) {
        std::array<std::vector<std::vector<uint8_t>>,2> results;
        for(int reuse=0;reuse<2;++reuse) {
            MpcvrRifeCreateParams p;p.device=device;p.width=p.height=p.contentWidth=p.contentHeight=128;p.contextCount=2;p.performanceBoost=1;
            p.modelPath=model.c_str();p.cachePath=argv[3];p.flags=(precise?MPCVR_RIFE_CREATE_HIGH_PRECISION:0u) | (reuse?MPCVR_RIFE_CREATE_FEATURE_REUSE:0u);
            void* handle=nullptr;const auto status=create(&p,&handle);std::wcout<<name<<L" reuse="<<reuse<<L" create="<<status<<std::endl;Check(status==0&&handle,"runtime create");
            std::vector<ushort4> pixels(128*128);for(size_t i=0;i<pixels.size();++i) pixels[i]=make_ushort4(__half_as_ushort(__float2half(float(i%128)/127)),0x3400,0x3800,0x3c00);
            std::vector<uchar4> bytes(pixels.size());
            for(size_t i=0;i<bytes.size();++i) bytes[i]=make_uchar4(128,64,static_cast<unsigned char>((i%128)*255/127),255);
            auto a=Texture(device,128,128,precise,precise?static_cast<void*>(pixels.data()):static_cast<void*>(bytes.data()));
            std::reverse(pixels.begin(),pixels.end());std::reverse(bytes.begin(),bytes.end());
            auto b=Texture(device,128,128,precise,precise?static_cast<void*>(pixels.data()):static_cast<void*>(bytes.data()));
            auto output=Texture(device,128,128,precise);
            double fresh=0,cached=0;unsigned freshN=0,cachedN=0;
            for(int pass=0;pass<9;++pass) {
                MpcvrRifeRequest q;q.contextIndex=0;q.first=a;q.second=b;q.output=output;q.timestep=(pass%3+1)*.25f;q.inputPairId=1;
                MpcvrRifeStats s;Check(infer(handle,&q,&s)==0,"inference");
                Check(s.featureReuse==uint64_t(reuse&&supported?(pass?2:1):0),"encoder fresh/reused or safe fallback status");
                if(pass<3) {
                    results[reuse].push_back(Read(device,context,output));
                    const auto file = std::filesystem::path(argv[3]) / (std::wstring(name) + (precise?L"_":L"_bgra_")
                        + std::to_wstring(reuse) + L"_" + std::to_wstring(pass) + L".rgba16");
                    std::ofstream dump(file, std::ios::binary);
                    const auto& bytes = results[reuse].back();
                    dump.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                } else if (reuse && supported) {
                    Check(Read(device,context,output)==results[reuse][pass%3], "reused features reproduce fresh output exactly");
                }
                if(pass) {cached+=s.tensorRtMs;++cachedN;} else {fresh+=s.tensorRtMs;++freshN;}
            }
            std::cout<<"TRT ms fresh="<<fresh/freshN<<" reused="<<cached/cachedN<<"\n";
            // A new generation must recompute; failures must invalidate previous reuse.
            MpcvrRifeRequest q;q.first=a;q.second=b;q.output=output;q.inputPairId=2;MpcvrRifeStats s;
            Check(infer(handle,&q,&s)==0 && s.featureReuse==uint64_t(reuse&&supported?1:0),"new pair invalidates encoder");
            q.timestep=1;Check(infer(handle,&q,&s)==MPCVR_RIFE_INVALID_ARGUMENT,"invalid request rejected");q.timestep=.5f;
            Check(infer(handle,&q,&s)==0 && s.featureReuse==uint64_t(reuse&&supported?1:0),"failure invalidates encoder");
            q.contextIndex=1;Check(infer(handle,&q,&s)==0 && s.featureReuse==uint64_t(reuse&&supported?1:0),"contexts own independent features");
            destroy(handle);
        }
        double maximum=0, total=0; size_t n=0; std::vector<double> errors;
        for(size_t t=0;t<3;++t) for(size_t i=0;i<results[0][t].size()/(precise?2:1);++i) {
            const auto* a=reinterpret_cast<const __half*>(results[0][t].data());const auto* b=reinterpret_cast<const __half*>(results[1][t].data());
            const double error=precise ? std::abs(__half2float(a[i])-__half2float(b[i]))
                : std::abs(int(results[0][t][i])-int(results[1][t][i]))/255.0;
            maximum=std::max(maximum,error); total+=error; ++n; errors.push_back(error);
        }
        std::sort(errors.begin(),errors.end());
        std::cout<<(precise?"16F":"BGRA8")<<" whole/split GPU max="<<maximum<<" mean="<<total/n<<" p99="<<errors[errors.size()*99/100]<<std::endl;
        Check(maximum<.02,"GPU precision parity within FP16 tactic tolerance");
        if (!supported) Check(maximum==0, "unvalidated model keeps original output exactly");
        }
    }
    FreeLibrary(module);std::cout<<"Native GPU: models, precision, pair invalidation and context isolation passed\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
