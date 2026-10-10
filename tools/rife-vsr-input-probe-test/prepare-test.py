"""Run the production VSR comparison and owned snapshot against CPU-only VP spies.

No D3D device, CUDA runtime, video file or driver activity query is used.
"""
from pathlib import Path
import argparse


def section(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


def prepare(output):
    root = Path(__file__).resolve().parents[2]
    bridge = (root / 'Source/RifeDX11Bridge.cpp').read_text(encoding='utf-8')
    vp = (root / 'Source/D3D11VP.cpp').read_text(encoding='utf-8')
    header = (root / 'Source/D3D11VP.h').read_text(encoding='utf-8')
    cpp = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <deque>
#include <format>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include "POLICY"
#include "PROBE_POLICY"
using UINT=unsigned int; using DWORD=unsigned long; using ULONGLONG=unsigned long long;
using LONG=int; using HRESULT=int;
constexpr HRESULT S_OK=0, E_FAIL=-1;
bool SUCCEEDED(HRESULT hr) { return hr>=0; } bool FAILED(HRESULT hr) { return hr<0; }
#define ASSERT assert
constexpr UINT PCIV_NVIDIA=0x10de;
constexpr int SUPERRES_Disable=0, MAXINE_OPERATION_Disabled=0, DEINT_Disable=0;
constexpr int DXGI_FORMAT_NV12=1, DXGI_FORMAT_B8G8R8A8_UNORM=2;
constexpr int D3D11_USAGE_DEFAULT=0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE=0;
constexpr int DXVA2_NominalRange_16_235=1, DXVA2_VideoTransferMatrix_BT709=1;
constexpr int DXVA2_VideoTransFunc_22=1, DXVA2_VideoTransFunc_709=2;
using DXGI_FORMAT=int;
constexpr int Tex2D_DefaultRTarget=0, Tex2D_DefaultShaderRTarget=1;
constexpr int HKEY_CURRENT_USER=0, RRF_RT_REG_DWORD=0, ERROR_SUCCESS=0;
DWORD registryMode=0; ULONGLONG tick=2000;
ULONGLONG GetTickCount64() { return tick; }
int RegGetValueW(int, const wchar_t*, const wchar_t*, int, void*, DWORD* value, DWORD*) {
    *value=registryMode; return ERROR_SUCCESS;
}
template<class... T> void DLog(const wchar_t*, T...) {}
std::wstring HR2Str(HRESULT) { return L"failure"; }
struct CSize {
    LONG cx=0,cy=0;
    CSize()=default; CSize(LONG x,LONG y):cx(x),cy(y){}
    bool operator==(const CSize&) const=default;
};
struct CRect {
    LONG left=0,top=0,right=0,bottom=0;
    CRect()=default; CRect(LONG l,LONG t,LONG r,LONG b):left(l),top(t),right(r),bottom(b){}
    LONG Width() const { return right-left; } LONG Height() const { return bottom-top; }
    CSize Size() const { return {Width(),Height()}; }
    bool operator==(const CRect&) const=default;
};
struct D3D11_TEXTURE2D_DESC {
    UINT Width=0,Height=0,MipLevels=1,ArraySize=1;
    int Format=DXGI_FORMAT_NV12,Usage=D3D11_USAGE_DEFAULT;
    UINT CPUAccessFlags=0;
    struct { UINT Count=1; } SampleDesc;
};
struct Ref { int refs=0; void AddRef(){++refs;} void Release(){assert(refs>0);--refs;} };
struct ID3D11Texture2D:Ref {
    D3D11_TEXTURE2D_DESC desc;
    void GetDesc(D3D11_TEXTURE2D_DESC* out){*out=desc;}
};
struct IMediaSample:Ref {};
struct ID3D11VideoProcessorInputView:Ref {};
struct ID3D11ShaderResourceView {};
template<class T> struct CComPtr {
    T* p=nullptr;
    CComPtr()=default; CComPtr(T* raw):p(raw){if(p)p->AddRef();}
    CComPtr(const CComPtr& other):CComPtr(other.p){}
    ~CComPtr(){Release();}
    void Release(){if(p){p->Release();p=nullptr;}}
    CComPtr& operator=(T* raw){if(raw)raw->AddRef();Release();p=raw;return *this;}
    CComPtr& operator=(const CComPtr& other){return *this=other.p;}
    operator T*() const {return p;} T* operator->() const {return p;}
};
INPUT_DATA
struct DXVA2_ExtendedFormat { int NominalRange=0,VideoTransferMatrix=0,VideoTransferFunction=0; };
struct CD3D11VP {
    bool ready=true,device=true,failProcess=false,failRequest=false;
    int initCount=0,convertCount=0,blitCount=0,preset=0;
    UINT m_InputArraySlice=0; bool m_bDecoderInputValid=false;
    struct { UINT count=0; UINT Size(){return count;} } m_VideoTextures;
    VideoInputData m_VideoInputData;
    ID3D11VideoProcessorInputView view;
    ID3D11Texture2D* bound=nullptr; IMediaSample* owner=nullptr; UINT slice=0;
    CRect srcRect,dstRect; CSize inputStorage,outputStorage;
    bool IsReady()const{return ready;} bool IsVideoDeviceOk(){return device;}
    HRESULT InitVideoDevice(void*,void*,UINT){device=true;return S_OK;}
    void ReleaseVideoProcessor(){ready=false;m_VideoInputData.Clear();m_bDecoderInputValid=false;}
    HRESULT InitVideoProcessor(int,UINT w,UINT h,DXVA2_ExtendedFormat,int,bool,
            int&,CSize out,bool=false){++initCount;inputStorage={static_cast<int>(w),static_cast<int>(h)};
        outputStorage=out;ready=true;return S_OK;}
    HRESULT InitInputTextures(void*,bool){return S_OK;}
    HRESULT SetSuperRes(int value,CSize={}){preset=value;return failRequest?E_FAIL:S_OK;}
    HRESULT SetRectangles(CRect a,CRect b){srcRect=a;dstRect=b;return S_OK;}
    HRESULT Process(ID3D11Texture2D* target,int,bool){
        if(target->desc.Format==DXGI_FORMAT_NV12)++convertCount;else ++blitCount;
        return failProcess?E_FAIL:S_OK;
    }
    void SetInputVideoData(ID3D11Texture2D* tex,IMediaSample* sample,UINT index,int){
        bound=tex;owner=sample;slice=index;m_InputArraySlice=index;
        m_VideoInputData.Resize(1);m_VideoInputData.GetTexture()=tex;
        m_VideoInputData.SetInputView(&view);m_VideoInputData.PushSample(sample);
        m_bDecoderInputValid=sample!=nullptr;
    }
    bool GetLatestDecoderInput(CComPtr<ID3D11Texture2D>&,CComPtr<IMediaSample>&,UINT&);
};
SNAPSHOT
struct Tex2D_t {
    ID3D11Texture2D* pTexture=nullptr; D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D storage;
    void Release(){pTexture=nullptr;}
    HRESULT CheckCreate(void*,int format,UINT width,UINT height,int){
        desc.Width=width;desc.Height=height;desc.Format=format;storage.desc=desc;
        pTexture=&storage;return S_OK;
    }
};
struct Context {
    void PSSetShaderResources(UINT,size_t,ID3D11ShaderResourceView**){}
    void OMSetRenderTargets(UINT,void*,void*){}
};
class CDX11VideoProcessor {
public:
    UINT m_VendorId=PCIV_NVIDIA,m_srcRectWidth=720,m_srcRectHeight=480;
    bool m_bVPScaling=true,m_bVPUseSuperRes=true,m_srcAnamorphic=true;
    bool m_bVPUseRTXVideoHDR=false,hdr=false;
    int m_iMaxineOperation=0,m_iRotation=0,m_iVPSuperRes=4,m_SampleFormat=0;
    DWORD m_RifeVsrInputProbeMode=0; ULONGLONG m_RifeVsrInputProbeReadTick=0;
    bool m_bRifeVsrProbeSourceDisabled=false;
    CRect m_srcRect{0,0,720,480};
    DXVA2_ExtendedFormat m_srcExFmt{1,1,2};
    CD3D11VP m_D3D11VP,m_RifeVsrConvertVP,m_RifeUpscaleVP;
    Tex2D_t m_TexRifeVsrNV12,m_TexRifeUpscaleOutput;
    D3D11_TEXTURE2D_DESC m_RifeUpscaleInputDesc;
    CRect m_RifeUpscaleContentRect;
    CSize m_RifeVsrContentSize,m_RifeVsrStorageSize,m_RifeUpscaleOutputSize;
    int m_RifeUpscaleSuperRes=0;
    bool m_bRifeUpscaleAttempted=false,m_bRifeUpscaleUsed=false;
    std::wstring m_strRifeUpscaleStatus;
    Context context; Context* m_pDeviceContext=&context; void* m_pDevice=nullptr;
    bool SourceIsHDR(){return hdr;}
    HRESULT ResizeShaderPass(Tex2D_t&,ID3D11Texture2D*,CRect,CRect,int,bool){return S_OK;}
    DWORD GetRifeVsrInputProbeMode();
    void UpdateRifeVsrProbeSourceRequest(bool);
    bool GetRifeVsrProbeDecoderInput(CComPtr<ID3D11Texture2D>&,CComPtr<IMediaSample>&,UINT&);
    bool TryRifeVideoProcessorUpscale(Tex2D_t&,ID3D11Texture2D*,const CRect&,const CRect&);
};
METHODS
void mode(CDX11VideoProcessor& renderer,DWORD value) {
    registryMode=value;tick+=1100;renderer.UpdateRifeVsrProbeSourceRequest(true);
}
int main(){
    ID3D11Texture2D decoded,target,rgb;IMediaSample sample;
    decoded.desc.Width=720;decoded.desc.Height=512;decoded.desc.ArraySize=8;
    target.desc.Width=1920;target.desc.Height=1080;target.desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    rgb.desc.Width=288;rgb.desc.Height=480;rgb.desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    Tex2D_t input;input.pTexture=&rgb;input.desc=rgb.desc;
    const CRect content{0,0,278,480},output{0,0,626,1080};
#if !MPCVR_TEST_VSR_INPUT_PROBE
    CDX11VideoProcessor normal;registryMode=1;
    normal.UpdateRifeVsrProbeSourceRequest(true);
    assert(normal.GetRifeVsrInputProbeMode()==0 && !normal.m_bRifeVsrProbeSourceDisabled);
    assert(normal.TryRifeVideoProcessorUpscale(input,&target,content,output));
    assert(normal.m_RifeUpscaleVP.owner==nullptr
        && normal.m_RifeUpscaleVP.inputStorage==CSize(720,480));
    std::cout<<"PASS normal build ignores diagnostic registry selection (CPU only)\n";
    return 0;
#else
    int cases=0;
    {
        CDX11VideoProcessor r;
        r.m_D3D11VP.SetInputVideoData(&decoded,&sample,3,0);
        mode(r,1);assert(r.m_bVPUseSuperRes && r.m_D3D11VP.preset==0);
        assert(r.TryRifeVideoProcessorUpscale(input,&target,content,output));
        assert(r.m_RifeUpscaleVP.bound==&decoded && r.m_RifeUpscaleVP.owner==&sample
            && r.m_RifeUpscaleVP.slice==3);
        assert(r.m_RifeUpscaleVP.inputStorage==CSize(720,512));
        assert(r.m_RifeUpscaleVP.srcRect==CRect(0,0,720,480)
            && r.m_RifeUpscaleVP.dstRect==output);
        assert(r.m_RifeVsrConvertVP.convertCount==1 && r.m_RifeUpscaleVP.blitCount==1);
        assert(r.m_strRifeUpscaleStatus.find(L"probe A: decoded")!=std::wstring::npos);
        assert(sample.refs>=2 && decoded.refs>=2);++cases;
        mode(r,2);
        assert(r.TryRifeVideoProcessorUpscale(input,&target,content,output));
        assert(r.m_RifeUpscaleVP.bound==r.m_TexRifeVsrNV12.pTexture
            && r.m_RifeUpscaleVP.owner==nullptr && r.m_RifeUpscaleVP.slice==0);
        assert(r.m_RifeUpscaleVP.initCount==1 && r.m_RifeVsrConvertVP.initCount==1);
        assert(r.m_RifeVsrConvertVP.convertCount==2 && r.m_RifeUpscaleVP.blitCount==2);
        assert(r.m_RifeUpscaleVP.srcRect==CRect(0,0,720,480)
            && r.m_RifeUpscaleVP.dstRect==output && r.m_RifeUpscaleVP.inputStorage==CSize(720,512));
        assert(r.m_strRifeUpscaleStatus.find(L"probe B: RIFE")!=std::wstring::npos);++cases;
        mode(r,1);assert(r.TryRifeVideoProcessorUpscale(input,&target,content,output));
        assert(r.m_RifeUpscaleVP.initCount==1);++cases;
        mode(r,0);assert(!r.m_bRifeVsrProbeSourceDisabled && r.m_D3D11VP.preset==4);
        assert(r.TryRifeVideoProcessorUpscale(input,&target,content,output));
        assert(r.m_RifeUpscaleVP.inputStorage==CSize(720,480)
            && r.m_RifeUpscaleVP.owner==nullptr && r.m_RifeUpscaleVP.initCount==2);
        assert(r.m_strRifeUpscaleStatus.find(L"requested per frame after RIFE")!=std::wstring::npos);++cases;
        mode(r,99);assert(r.GetRifeVsrInputProbeMode()==0);++cases;
        mode(r,1);r.UpdateRifeVsrProbeSourceRequest(false);
        assert(!r.m_bRifeVsrProbeSourceDisabled && r.m_D3D11VP.preset==4);++cases;
        mode(r,1);r.m_iMaxineOperation=1;r.UpdateRifeVsrProbeSourceRequest(true);
        assert(!r.m_bRifeVsrProbeSourceDisabled && r.m_D3D11VP.preset==0);++cases;
    }
    assert(sample.refs==0 && decoded.refs==0);
    { CD3D11VP original;CComPtr<ID3D11Texture2D> tex;CComPtr<IMediaSample> held;UINT slice=0;
      original.SetInputVideoData(&decoded,&sample,7,0);
      assert(original.GetLatestDecoderInput(tex,held,slice) && slice==7);
      original.ReleaseVideoProcessor();
      assert(tex.p==&decoded && held.p==&sample && sample.refs==1 && decoded.refs==1);
      tex.Release();held.Release();++cases; }
    for(int invalid=0;invalid<10;++invalid){
        CDX11VideoProcessor r;
        r.m_D3D11VP.SetInputVideoData(&decoded,&sample,3,0);mode(r,2);
        auto saved=decoded.desc;
        switch(invalid){
        case 0:r.m_D3D11VP.m_VideoTextures.count=1;break; // software upload
        case 1:r.m_D3D11VP.m_bDecoderInputValid=false;break; // after reset
        case 2:r.m_iRotation=90;break;
        case 3:r.m_SampleFormat=1;break;
        case 4:r.m_srcExFmt.VideoTransferMatrix=2;break;
        case 5:r.m_srcRect.left=2;break;
        case 6:decoded.desc.Height=481;break;
        case 7:decoded.desc.ArraySize=2;break;
        case 8:decoded.desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;break;
        case 9:r.m_RifeVsrConvertVP.failProcess=true;break;
        }
        assert(!r.TryRifeVideoProcessorUpscale(input,&target,content,output));
        assert(r.m_RifeUpscaleVP.blitCount==0);decoded.desc=saved;++cases;
    }
    { CDX11VideoProcessor r;r.m_D3D11VP.SetInputVideoData(&decoded,&sample,3,0);mode(r,1);
      assert(!r.TryRifeVideoProcessorUpscale(input,&target,{0,0,139,240},output));++cases; }
    { CDX11VideoProcessor r;r.m_D3D11VP.SetInputVideoData(&decoded,&sample,3,0);mode(r,1);
      r.m_RifeUpscaleVP.failRequest=true;
      assert(!r.TryRifeVideoProcessorUpscale(input,&target,content,output));
      assert(r.m_RifeUpscaleVP.blitCount==0);++cases; }
    assert(sample.refs==0 && decoded.refs==0);
    std::cout<<"PASS "<<cases<<" production VSR input-probe cases (CPU only)\n";
#endif
}
'''
    cpp = cpp.replace('INPUT_DATA', section(header, 'class VideoInputData', '// D3D11 Video Processor'))
    cpp = cpp.replace('SNAPSHOT', section(vp, 'bool CD3D11VP::GetLatestDecoderInput(', 'void CD3D11VP::ResetFrameOrder()'))
    cpp = cpp.replace('METHODS', section(bridge, 'DWORD CDX11VideoProcessor::GetRifeVsrInputProbeMode()',
                                       'DXGI_FORMAT CDX11VideoProcessor::GetRifeSurfaceFormat()'))
    cpp = cpp.replace('PROBE_POLICY', (root / 'Source/RifeVsrInputProbe.h').as_posix())
    cpp = cpp.replace('POLICY', (root / 'Source/RifeVideoProcessorPolicy.h').as_posix())
    output.write_text(cpp, encoding='utf-8')


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    prepare(parser.parse_args().output)
