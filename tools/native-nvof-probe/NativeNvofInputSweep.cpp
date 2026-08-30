/*
 * Standalone NVOF input-format / effective-grid sweep.
 *
 * Compares BGRA8 and NV12 input representations at effective grids
 * 4/8/16/24/32 without enabling NVOF output cost. This never touches MPC-HC.
 */
#include "NativeNvofApi.h"
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT NvidiaVendorId=0x10DE;
constexpr std::array<uint32_t,5> EffectiveGrids{4,8,16,24,32};
struct ModuleGuard{HMODULE m=nullptr;~ModuleGuard(){if(m)FreeLibrary(m);}};
struct SessionGuard{nvof::Handle h=nullptr;nvof::DestroyFn d=nullptr;~SessionGuard(){if(h&&d)d(h);}};
struct ResourceGuard{nvof::GpuBufferHandle h=nullptr;nvof::UnregisterResourceD3D11Fn u=nullptr;~ResourceGuard(){if(h&&u)u(h);}};
struct Image{uint32_t w=0,h=0;std::vector<uint8_t> bgra;};
struct Surface{ComPtr<ID3D11Texture2D> gpu,stage;ResourceGuard reg;};
struct FlowResult{bool supported=false,ok=false;std::wstring error;double ms=0;std::vector<uint8_t> fwd,bwd;};

std::wstring Hr(HRESULT hr){std::wostringstream s;s<<L"0x"<<std::hex<<std::uppercase<<(unsigned long)hr;return s.str();}
HMODULE LoadNvof(){wchar_t d[MAX_PATH]={};UINT n=GetSystemDirectoryW(d,(UINT)std::size(d));if(!n||n>=std::size(d))return nullptr;return LoadLibraryW((std::filesystem::path(d)/L"nvofapi64.dll").c_str());}
bool Device(ComPtr<ID3D11Device>&dev,ComPtr<ID3D11DeviceContext>&ctx,std::wstring&name){ComPtr<IDXGIFactory1> f;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f))))return false;for(UINT i=0;;++i){ComPtr<IDXGIAdapter1>a;HRESULT hr=f->EnumAdapters1(i,&a);if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return false;DXGI_ADAPTER_DESC1 d={};a->GetDesc1(&d);if(d.VendorId!=NvidiaVendorId||(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0},made{};hr=D3D11CreateDevice(a.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,(UINT)std::size(levels),D3D11_SDK_VERSION,&dev,&made,&ctx);if(FAILED(hr)){std::wcerr<<L"D3D11CreateDevice "<<Hr(hr)<<L'\n';return false;}name=d.Description;return true;}return false;}
bool ReadBmp(const std::filesystem::path&p,Image&i){std::ifstream f(p,std::ios::binary);if(!f)return false;BITMAPFILEHEADER fh={};BITMAPINFOHEADER ih={};f.read((char*)&fh,sizeof(fh));f.read((char*)&ih,sizeof(ih));if(!f||fh.bfType!=0x4d42||ih.biWidth<=0||ih.biHeight==0||ih.biBitCount!=32||ih.biCompression!=BI_RGB)return false;i.w=(uint32_t)ih.biWidth;bool top=ih.biHeight<0;i.h=(uint32_t)(top?-(int64_t)ih.biHeight:(int64_t)ih.biHeight);size_t rb=(size_t)i.w*4;i.bgra.resize(rb*i.h);f.seekg(fh.bfOffBits);std::vector<uint8_t>row(rb);for(uint32_t y=0;y<i.h;++y){f.read((char*)row.data(),(std::streamsize)rb);if(!f)return false;uint32_t dy=top?y:i.h-1-y;std::copy(row.begin(),row.end(),i.bgra.begin()+(size_t)dy*rb);}return true;}

double Cubic(double x){constexpr double B=1.0/3.0,C=1.0/3.0;x=std::abs(x);if(x<1.0)return ((12-9*B-6*C)*x*x*x+(-18+12*B+6*C)*x*x+(6-2*B))/6.0;if(x<2.0)return ((-B-6*C)*x*x*x+(6*B+30*C)*x*x+(-12*B-48*C)*x+(8*B+24*C))/6.0;return 0.0;}
Image ResizeCrop(const Image&src,uint32_t cropW,uint32_t cropH,uint32_t dstW,uint32_t dstH){Image out;out.w=dstW;out.h=dstH;out.bgra.resize((size_t)dstW*dstH*4);if(cropW==dstW&&cropH==dstH){for(uint32_t y=0;y<dstH;++y)std::copy_n(src.bgra.data()+(size_t)y*src.w*4,(size_t)dstW*4,out.bgra.data()+(size_t)y*dstW*4);return out;}const double sx=(double)cropW/dstW,sy=(double)cropH/dstH;for(uint32_t y=0;y<dstH;++y){double fy=(y+.5)*sy-.5;int iy=(int)std::floor(fy);for(uint32_t x=0;x<dstW;++x){double fx=(x+.5)*sx-.5;int ix=(int)std::floor(fx);double sum[4]={},ws=0;for(int oy=-1;oy<=2;++oy){int yy=std::clamp(iy+oy,0,(int)cropH-1);double wy=Cubic(fy-(iy+oy));for(int ox=-1;ox<=2;++ox){int xx=std::clamp(ix+ox,0,(int)cropW-1);double w=wy*Cubic(fx-(ix+ox));const uint8_t*p=src.bgra.data()+((size_t)yy*src.w+xx)*4;for(int c=0;c<4;++c)sum[c]+=w*p[c];ws+=w;}}uint8_t*d=out.bgra.data()+((size_t)y*dstW+x)*4;for(int c=0;c<4;++c)d[c]=(uint8_t)std::clamp((int)std::lround(sum[c]/ws),0,255);}}return out;}
std::vector<uint8_t> ToNv12(const Image&i){const uint32_t w=i.w,h=i.h;std::vector<uint8_t>o((size_t)w*h*3/2);auto clip=[](double v){return(uint8_t)std::clamp((int)std::lround(v),0,255);};for(uint32_t y=0;y<h;++y)for(uint32_t x=0;x<w;++x){const uint8_t*p=i.bgra.data()+((size_t)y*w+x)*4;double B=p[0],G=p[1],R=p[2];o[(size_t)y*w+x]=clip(16.0+.182586*R+.614231*G+.062007*B);}size_t uv=(size_t)w*h;for(uint32_t y=0;y<h;y+=2)for(uint32_t x=0;x<w;x+=2){double U=0,V=0;int n=0;for(uint32_t oy=0;oy<2&&y+oy<h;++oy)for(uint32_t ox=0;ox<2&&x+ox<w;++ox){const uint8_t*p=i.bgra.data()+((size_t)(y+oy)*w+x+ox)*4;double B=p[0],G=p[1],R=p[2];U+=128.0-.100644*R-.338572*G+.439216*B;V+=128.0+.439216*R-.398942*G-.040274*B;++n;}size_t q=uv+(size_t)(y/2)*w+x;o[q]=clip(U/n);o[q+1]=clip(V/n);}return o;}

bool Formats(const nvof::D3D11FunctionList&a,nvof::Handle h,nvof::BufferUsage u,std::vector<DXGI_FORMAT>&v){uint32_t n=0;if(a.getSurfaceFormatCountD3D11(h,u,nvof::ModeOpticalFlow,&n)!=nvof::Success)return false;v.assign(n,DXGI_FORMAT_UNKNOWN);return !n||a.getSurfaceFormatD3D11(h,u,nvof::ModeOpticalFlow,v.data())==nvof::Success;}
bool Has(const std::vector<DXGI_FORMAT>&v,DXGI_FORMAT f){return std::find(v.begin(),v.end(),f)!=v.end();}
bool Reg(const nvof::D3D11FunctionList&a,nvof::Handle h,ID3D11Resource*r,ResourceGuard&g){g.u=a.unregisterResourceD3D11;return a.registerResourceD3D11(h,r,&g.h)==nvof::Success;}
bool InputTex(ID3D11Device*d,const Image&i,bool nv12,ComPtr<ID3D11Texture2D>&t){D3D11_TEXTURE2D_DESC q={};q.Width=i.w;q.Height=i.h;q.MipLevels=1;q.ArraySize=1;q.Format=nv12?DXGI_FORMAT_NV12:DXGI_FORMAT_B8G8R8A8_UNORM;q.SampleDesc.Count=1;q.Usage=D3D11_USAGE_DEFAULT;q.BindFlags=D3D11_BIND_SHADER_RESOURCE;std::vector<uint8_t>n;D3D11_SUBRESOURCE_DATA sd={};if(nv12){n=ToNv12(i);sd.pSysMem=n.data();sd.SysMemPitch=i.w;sd.SysMemSlicePitch=(UINT)n.size();}else{sd.pSysMem=i.bgra.data();sd.SysMemPitch=i.w*4;}return SUCCEEDED(d->CreateTexture2D(&q,&sd,&t));}
bool Output(ID3D11Device*d,uint32_t w,uint32_t h,Surface&s){D3D11_TEXTURE2D_DESC q={};q.Width=w;q.Height=h;q.MipLevels=1;q.ArraySize=1;q.Format=DXGI_FORMAT_R16G16_SINT;q.SampleDesc.Count=1;q.Usage=D3D11_USAGE_DEFAULT;q.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;HRESULT hr=d->CreateTexture2D(&q,nullptr,&s.gpu);if(FAILED(hr))return false;q.Usage=D3D11_USAGE_STAGING;q.BindFlags=0;q.CPUAccessFlags=D3D11_CPU_ACCESS_READ;return SUCCEEDED(d->CreateTexture2D(&q,nullptr,&s.stage));}
bool Read(ID3D11DeviceContext*c,Surface&s,uint32_t w,uint32_t h,std::vector<uint8_t>&b){c->CopyResource(s.stage.Get(),s.gpu.Get());D3D11_MAPPED_SUBRESOURCE m={};if(FAILED(c->Map(s.stage.Get(),0,D3D11_MAP_READ,0,&m)))return false;b.resize((size_t)w*h*4);for(uint32_t y=0;y<h;++y)std::copy_n((uint8_t*)m.pData+(size_t)y*m.RowPitch,(size_t)w*4,b.data()+(size_t)y*w*4);c->Unmap(s.stage.Get(),0);return true;}

FlowResult Run(ID3D11Device*d,ID3D11DeviceContext*c,const nvof::D3D11FunctionList&a,const Image&A,const Image&B,bool nv12){FlowResult r;SessionGuard s;s.d=a.destroy;nvof::Status st=a.createOpticalFlowD3D11(d,c,&s.h);if(st!=nvof::Success||!s.h){r.error=L"session create failed";return r;}std::vector<DXGI_FORMAT>in,out;if(!Formats(a,s.h,nvof::BufferUsageInput,in)||!Formats(a,s.h,nvof::BufferUsageOutput,out)){r.error=L"format query failed";return r;}DXGI_FORMAT inf=nv12?DXGI_FORMAT_NV12:DXGI_FORMAT_B8G8R8A8_UNORM;if(!Has(in,inf)||!Has(out,DXGI_FORMAT_R16G16_SINT)){r.error=L"input/output format unsupported";return r;}r.supported=true;nvof::InitParams p={};p.width=A.w;p.height=A.h;p.outputGridSize=nvof::OutputGrid4;p.hintGridSize=nvof::HintGridUndefined;p.mode=nvof::ModeOpticalFlow;p.performance=nvof::PerfSlow;p.enableExternalHints=nvof::False;p.enableOutputCost=nvof::False;p.disparityRange=nvof::StereoRangeUndefined;p.enableRoi=nvof::False;p.predictionDirection=nvof::PredictionBoth;p.enableGlobalFlow=nvof::False;p.inputBufferFormat=nv12?nvof::BufferFormatNv12:nvof::BufferFormatAbgr8;st=a.initialize(s.h,&p);if(st!=nvof::Success){r.error=L"NvOFInit failed";return r;}ComPtr<ID3D11Texture2D>at,bt;if(!InputTex(d,A,nv12,at)||!InputTex(d,B,nv12,bt)){r.error=L"input texture failed";return r;}ResourceGuard ar,br;if(!Reg(a,s.h,at.Get(),ar)||!Reg(a,s.h,bt.Get(),br)){r.error=L"input registration failed";return r;}uint32_t gw=A.w/4,gh=A.h/4;Surface f,b;if(!Output(d,gw,gh,f)||!Output(d,gw,gh,b)||!Reg(a,s.h,f.gpu.Get(),f.reg)||!Reg(a,s.h,b.gpu.Get(),b.reg)){r.error=L"output setup failed";return r;}nvof::ExecuteInputParams ei={};ei.inputFrame=br.h;ei.referenceFrame=ar.h;ei.disableTemporalHints=nvof::True;nvof::ExecuteOutputParams eo={};eo.outputBuffer=f.reg.h;eo.backwardOutputBuffer=b.reg.h;auto t=std::chrono::steady_clock::now();st=a.execute(s.h,&ei,&eo);if(st!=nvof::Success){r.error=L"NvOFExecute failed";return r;}c->Flush();if(!Read(c,f,gw,gh,r.fwd)||!Read(c,b,gw,gh,r.bwd)){r.error=L"readback failed";return r;}r.ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();r.ok=true;return r;}
bool Write(const std::filesystem::path&p,const std::vector<uint8_t>&b){std::ofstream f(p,std::ios::binary);if(!f)return false;f.write((const char*)b.data(),(std::streamsize)b.size());return f.good();}
std::filesystem::path OutDir(const std::filesystem::path&c){SYSTEMTIME t={};GetLocalTime(&t);wchar_t n[96]={};swprintf_s(n,L"nvof-input-sweep-%04u%02u%02u-%02u%02u%02u",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);auto p=c/n;std::filesystem::create_directories(p);return p;}
}
int wmain(int argc,wchar_t**argv){
#ifndef _WIN64
return 2;
#else
if(argc!=2){std::wcerr<<L"Usage: NativeNvofInputSweep.exe <capture-directory>\n";return 2;}std::filesystem::path cap=std::filesystem::absolute(argv[1]);Image A0,B0;if(!ReadBmp(cap/L"frame-A.bmp",A0)||!ReadBmp(cap/L"frame-B.bmp",B0)||A0.w!=B0.w||A0.h!=B0.h)return 3;ComPtr<ID3D11Device>d;ComPtr<ID3D11DeviceContext>c;std::wstring adapter;if(!Device(d,c,adapter))return 4;ModuleGuard mod{LoadNvof()};if(!mod.m)return 5;auto gv=(nvof::GetMaxSupportedApiVersionFn)GetProcAddress(mod.m,"NvOFGetMaxSupportedApiVersion");auto ci=(nvof::CreateInstanceD3D11Fn)GetProcAddress(mod.m,"NvOFAPICreateInstanceD3D11");if(!gv||!ci)return 6;uint32_t ver=0;if(gv(&ver)!=nvof::Success||ver<nvof::ApiVersion50)return 7;nvof::D3D11FunctionList api={};if(ci(nvof::ApiVersion50,&api)!=nvof::Success||!api.createOpticalFlowD3D11||!api.initialize||!api.registerResourceD3D11||!api.unregisterResourceD3D11||!api.execute||!api.destroy||!api.getSurfaceFormatCountD3D11||!api.getSurfaceFormatD3D11)return 8;auto out=OutDir(cap);std::ofstream sum(out/L"input-sweep-summary.txt");sum<<"NVOF input-format/effective-grid sweep\n"<<"adapter="<<std::filesystem::path(adapter).string()<<'\n'<<"source="<<A0.w<<'x'<<A0.h<<'\n';sum<<std::fixed<<std::setprecision(4);for(uint32_t grid:EffectiveGrids){uint32_t cw=A0.w-(A0.w%grid),ch=A0.h-(A0.h%grid),tw=(cw/grid)*4,th=(ch/grid)*4;if(tw<160||th<128){sum<<"grid"<<grid<<"_skipped=too_small\n";continue;}Image A=ResizeCrop(A0,cw,ch,tw,th),B=ResizeCrop(B0,cw,ch,tw,th);sum<<"grid"<<grid<<"_source="<<tw<<'x'<<th<<"\n";sum<<"grid"<<grid<<"_full_pixel_scale="<<(double)grid/4.0<<"\n";for(int f=0;f<2;++f){bool nv=f==1;const char*name=nv?"nv12":"bgra";FlowResult r=Run(d.Get(),c.Get(),api,A,B,nv);sum<<"grid"<<grid<<'_'<<name<<"_supported="<<(r.supported?1:0)<<'\n';sum<<"grid"<<grid<<'_'<<name<<"_success="<<(r.ok?1:0)<<'\n';if(r.ok){sum<<"grid"<<grid<<'_'<<name<<"_execute_plus_readback_ms="<<r.ms<<'\n';std::wstring pre=L"grid"+std::to_wstring(grid)+L"-"+(nv?L"nv12":L"bgra");Write(out/(pre+L"-flow-forward-B-to-A-s10.5.bin"),r.fwd);Write(out/(pre+L"-flow-backward-A-to-B-s10.5.bin"),r.bwd);}else if(!r.error.empty())sum<<"grid"<<grid<<'_'<<name<<"_error="<<std::filesystem::path(r.error).string()<<'\n';}}
}sum.close();std::wcout<<L"Output: "<<out.wstring()<<L'\n';return 0;
#endif
}
