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
#include <atomic>
#include <thread>
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
double MaxDifference(const std::vector<uint8_t>& a,const std::vector<uint8_t>& b,bool precise) {
    Check(a.size()==b.size(),"result size");double maximum=0;
    if(precise) { const auto* x=reinterpret_cast<const __half*>(a.data());const auto* y=reinterpret_cast<const __half*>(b.data());
        for(size_t i=0;i<a.size()/2;++i) maximum=std::max(maximum,double(std::abs(__half2float(x[i])-__half2float(y[i])))); }
    else for(size_t i=0;i<a.size();++i) maximum=std::max(maximum,std::abs(int(a[i])-int(b[i]))/255.0);
    return maximum;
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
    Check(capabilities && capabilities()==(MPCVR_RIFE_CAP_HIGH_PRECISION|MPCVR_RIFE_CAP_FEATURE_REUSE|MPCVR_RIFE_CAP_FLOW_REUSE),"runtime capabilities");
    auto create=reinterpret_cast<MpcvrRifeCreateFn>(GetProcAddress(module,"MpcvrRifeCreate"));
    auto infer=reinterpret_cast<MpcvrRifeInterpolateFn>(GetProcAddress(module,"MpcvrRifeInterpolate"));
    auto destroy=reinterpret_cast<MpcvrRifeDestroyFn>(GetProcAddress(module,"MpcvrRifeDestroy"));Check(create&&infer&&destroy,"exports");
    std::filesystem::create_directories(argv[3]);
    for(const auto name:{L"rife_v4.15_lite.onnx",L"rife_v4.25.onnx",L"rife_v4.25_lite.onnx"}) {
        const auto model=(std::filesystem::path(argv[2])/name).wstring();
        // Every bundled model with an image encoder must keep feature reuse active.
        constexpr bool supported = true;
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
            double fresh=0,cached=0;unsigned freshN=0,cachedN=0;std::vector<std::vector<uint8_t>> reused;
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
                    // Pass 0 alone ran the encoder. TensorRT 11.4's 4.25 Lite plans read
                    // some scratch left by the previous run, so a reused result may differ
                    // from the fresh one within FP16 tolerance, but repeated reuse is exact.
                    auto current=Read(device,context,output);
                    if(pass<6) {
                        Check(MaxDifference(current,results[reuse][pass%3],precise)<.02,"reused features stay within FP16 tolerance of fresh features");
                        reused.push_back(std::move(current));
                    } else {
                        Check(current==reused[pass%3],"repeated feature reuse reproduces its output exactly");
                    }
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
            if(reuse&&supported) {
                // Consecutive pairs may keep the frame they share, across contexts, but
                // only when the IDs promise it. Only 4.25 Lite on SM75 currently does.
                const auto run=[&](uint32_t index,ID3D11Texture2D* first,ID3D11Texture2D* second,ID3D11Texture2D* target,uint64_t id,uint64_t previous){
                    MpcvrRifeRequest r;r.contextIndex=index;r.first=first;r.second=second;r.output=target;r.timestep=.5f;r.inputPairId=id;r.previousPairId=previous;
                    MpcvrRifeStats rs;Check(infer(handle,&r,&rs)==0,"consecutive pair inference");return rs.featureReuse;};
                const uint64_t none=run(0,a,b,output,10,0);Check(none==1,"no promise encodes both images");
                const auto forward=Read(device,context,output);
                Check(run(0,b,a,output,11,0)==1,"no promise encodes both images, even if the frame is shared");
                const auto backward=Read(device,context,output);
                run(0,a,b,output,12,0);
                const uint64_t kept=run(1,b,a,output,13,12);
                Check(kept==1||kept==3,"consecutive pair status");
                const bool keeps=kept==3;
                Check(MaxDifference(Read(device,context,output),backward,precise)<.02,"keeping a frame across contexts matches encoding both");
                Check(run(1,a,b,output,14,13)==(keeps?3u:1u),"consecutive pair on one context keeps its frame");
                Check(MaxDifference(Read(device,context,output),forward,precise)<.02,"keeping a frame on one context matches encoding both");
                Check(run(0,b,a,output,15,999)==1,"a pair that never ran keeps nothing");
                Check(run(1,a,b,output,16,13)==1,"an older pair keeps nothing");
                Check(run(0,b,a,output,17,16)==(keeps?3u:1u),"keeping resumes after a mismatch");
                MpcvrRifeRequest bad;bad.first=a;bad.second=b;bad.output=output;bad.inputPairId=18;bad.previousPairId=17;bad.timestep=1;MpcvrRifeStats badStats;
                Check(infer(handle,&bad,&badStats)==MPCVR_RIFE_INVALID_ARGUMENT,"invalid consecutive request rejected");
                Check(MaxDifference(forward,backward,precise)>.02,"the test images must tell directions apart");
                // Contexts racing unsynchronized, as the renderer's workers do: any request
                // may miss the shared frame, but none may ever use the wrong one.
                std::atomic_uint64_t raced{0},failed{0};
                const auto worker=[&](uint32_t index) {
                    auto target=Texture(device,128,128,precise);
                    for(uint64_t i=0;i<120;++i) {
                        const uint64_t id=1000+i*2+index;
                        const uint64_t status=run(index,index?b:a,index?a:b,target,id,id-1);
                        raced+=status==3;
                        if(MaxDifference(Read(device,context,target),index?backward:forward,precise)>=.02) ++failed;
                    }
                };
                std::exception_ptr error0,error1;
                std::thread first([&]{try{worker(0);}catch(...){error0=std::current_exception();}}),second([&]{try{worker(1);}catch(...){error1=std::current_exception();}});
                first.join();second.join();
                if(error0) std::rethrow_exception(error0);
                if(error1) std::rethrow_exception(error1);
                Check(failed==0,"racing contexts never use the wrong frame");
                std::wcout<<name<<L" frame reuse "<<(keeps?L"active":L"not used")<<L", racing requests kept "<<raced.load()<<L" of 240\n";
                if(keeps) Check(raced>0,"racing contexts still keep frames");
            }
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
        }
    }
    // Content shorter than the padded surface: on SM75 the fused 4.25 Lite plan works on
    // fewer rows than the surface has, and the visible rows must still match the whole model.
    {
        constexpr int size=256,contentHeight=190;
        const auto model=(std::filesystem::path(argv[2])/L"rife_v4.25_lite.onnx").wstring();
        // The renderer clears the rows below the picture to black. Smooth content moving a
        // few pixels keeps flow small, so a row placed wrongly shows and FP16 rounding does not.
        const auto frame=[&](int shift) {
            std::vector<uchar4> pixels(size_t(size)*size,make_uchar4(0,0,0,255));
            for(int y=0;y<contentHeight;++y) for(int x=0;x<size;++x)
                pixels[size_t(y)*size+x]=make_uchar4(128,static_cast<unsigned char>(y*255/(contentHeight-1)),static_cast<unsigned char>((x+shift)*255/(size+7)),255);
            return Texture(device,size,size,false,pixels.data());
        };
        auto a=frame(0);
        auto b=frame(8);
        auto output=Texture(device,size,size,false);
        std::array<std::vector<uint8_t>,2> images;
        for(int reuse=0;reuse<2;++reuse) {
            MpcvrRifeCreateParams p;p.device=device;p.width=p.height=size;p.contentWidth=size;p.contentHeight=contentHeight;p.contextCount=2;p.performanceBoost=1;
            p.modelPath=model.c_str();p.cachePath=argv[3];p.flags=reuse?MPCVR_RIFE_CREATE_FEATURE_REUSE:0u;
            void* handle=nullptr;Check(create(&p,&handle)==0&&handle,"shorter content create");
            MpcvrRifeRequest q;q.first=a;q.second=b;q.output=output;q.timestep=.5f;q.inputPairId=1;
            MpcvrRifeStats s;Check(infer(handle,&q,&s)==0,"shorter content inference");
            Check(s.featureReuse==uint64_t(reuse),"shorter content uses the expected engine");
            images[reuse]=Read(device,context,output);
            destroy(handle);
        }
        double maximum=0;
        for(size_t i=0;i<size_t(size)*contentHeight*4;++i) maximum=std::max(maximum,std::abs(int(images[0][i])-int(images[1][i]))/255.0);
        std::cout<<"Shorter content, visible rows max="<<maximum<<std::endl;
        Check(maximum<.05,"visible rows match the whole model");
    }
    // Shared flow: at the time it was estimated each mode must match the whole model, and at
    // other times gentle motion must land where it truly is. Only the measured path shares it.
    for(const bool precise : {false,true}) {
        constexpr int size=256;constexpr int shift=8;
        const auto model=(std::filesystem::path(argv[2])/L"rife_v4.25_lite.onnx").wstring();
        const auto frame=[&](int offset) {
            std::vector<uchar4> bytes(size_t(size)*size);std::vector<ushort4> floats(bytes.size());
            for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
                const float g=float(y)/(size-1),r=float(x+offset)/(size+shift-1);
                bytes[size_t(y)*size+x]=make_uchar4(128,static_cast<unsigned char>(g*255),static_cast<unsigned char>(r*255),255);
                floats[size_t(y)*size+x]=make_ushort4(__half_as_ushort(__float2half(r)),__half_as_ushort(__float2half(g)),__half_as_ushort(__float2half(128.f/255)),__half_as_ushort(__float2half(1.f)));
            }
            return Texture(device,size,size,precise,precise?static_cast<const void*>(floats.data()):static_cast<const void*>(bytes.data()));
        };
        auto a=frame(0);auto b=frame(shift);
        auto output=Texture(device,size,size,precise);
        std::vector<uint8_t> whole;
        const auto run=[&](void* handle,float t,uint64_t expectedFlow,bool reusedPair) {
            MpcvrRifeRequest q;q.first=a;q.second=b;q.output=output;q.timestep=t;q.inputPairId=1;
            MpcvrRifeStats s;Check(infer(handle,&q,&s)==0,"flow reuse inference");
            Check(s.flowReuse==expectedFlow,"flow reuse reported");
            Check(s.inputPairReuse==(reusedPair?2u:1u),"flow reuse keeps the pair");
            return Read(device,context,output);
        };
        for(uint32_t mode=0;mode<3;++mode) {
            MpcvrRifeCreateParams p;p.device=device;p.width=p.height=p.contentWidth=p.contentHeight=size;p.contextCount=2;p.performanceBoost=1;
            p.modelPath=model.c_str();p.cachePath=argv[3];
            p.flags=(precise?MPCVR_RIFE_CREATE_HIGH_PRECISION:0u)|MPCVR_RIFE_CREATE_FEATURE_REUSE
                |(mode==1?MPCVR_RIFE_CREATE_FLOW_REUSE_COARSE:mode==2?MPCVR_RIFE_CREATE_FLOW_REUSE_RETIME:0u);
            void* handle=nullptr;Check(create(&p,&handle)==0&&handle,"flow reuse create");
            MpcvrRifeRequest probe;probe.first=a;probe.second=b;probe.output=output;probe.timestep=.5f;probe.inputPairId=99;
            MpcvrRifeStats reported;Check(infer(handle,&probe,&reported)==0,"flow reuse probe");
            const bool shared=reported.flowReuse==mode;
            if(mode && !shared) {std::cout<<"Flow reuse mode "<<mode<<" not on this GPU's measured path\n";Check(reported.flowReuse==0,"unavailable flow reuse falls back");destroy(handle);continue;}
            const auto midpoint=run(handle,.5f,mode,false);
            if(!mode) whole=midpoint;
            const auto early=run(handle,.25f,mode,true);
            const auto again=run(handle,.5f,mode,true);
            const auto late=run(handle,.75f,mode,true);
            const double toWhole=MaxDifference(midpoint,whole,precise),repeated=MaxDifference(midpoint,again,precise);
            auto truth=[&](int offset) {auto texture=frame(offset);return Read(device,context,texture);};
            const double toEarly=MaxDifference(early,truth(shift/4),precise),toLate=MaxDifference(late,truth(shift*3/4),precise);
            std::cout<<"Flow reuse mode "<<mode<<(precise?" RGBA16F":" BGRA8")<<": t=.5 vs whole max="<<toWhole<<", repeated max="<<repeated<<", t=.25/.75 vs truth max="<<toEarly<<"/"<<toLate<<std::endl;
            Check(toWhole<.03,"shared flow matches the model where it was estimated");
            Check(repeated<(mode==2?1e-6:.02),"a target doesn't depend on the ones before it");
            Check(toEarly<.06&&toLate<.06,"shared flow lands gentle motion where it truly is");
            destroy(handle);
        }
    }
    FreeLibrary(module);std::cout<<"Native GPU: models, precision, pair invalidation and context isolation passed\n";
    } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
