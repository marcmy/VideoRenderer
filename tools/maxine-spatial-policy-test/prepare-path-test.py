"""Exercise production Maxine target calculations without a D3D/CUDA device.

--source can point to an older DX11VideoProcessor.cpp to demonstrate the
paused/source versus RIFE anamorphic target discrepancy before the repair.
"""
import argparse
from pathlib import Path


def section(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


def prepare(source, output):
    root = Path(__file__).resolve().parents[2]
    text = source.read_text(encoding="utf-8")
    enums = section((root / "Source/IVideoRenderer.h").read_text(encoding="utf-8"),
                    "enum :int {\n\tSUPERRES_Disable", "enum :int {\n\tMAXINE_PIPELINE_")
    helpers = section(text, "static int NormalizeMaxineOversample(",
                      "static const wchar_t* MaxineOversampleToString(")
    helpers += section(text, "static bool SourceMatchesSuperResLimit(", "const UINT dither_size")
    methods = section(text, "bool CDX11VideoProcessor::GetMaxineVSRTargetSize(",
                      "bool CDX11VideoProcessor::ApplyMaxine(")
    cpp = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include "MAXINE_POLICY"
#include "RIFE_POLICY"
using LONG = int;
using UINT = unsigned int;
constexpr UINT PCIV_NVIDIA = 0x10de;
constexpr uint32_t D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION = 16384;
struct CSize {
    LONG cx=0, cy=0;
    CSize()=default;
    CSize(LONG x, LONG y): cx(x), cy(y) {}
    bool operator==(const CSize&) const = default;
};
struct CRect {
    LONG width, height;
    LONG Width() const { return width; }
    LONG Height() const { return height; }
};
ENUMS
HELPERS
class CDX11VideoProcessor {
public:
    UINT m_srcRectWidth=720, m_srcRectHeight=480;
    bool m_srcAnamorphic=true;
    UINT m_srcAspectRatioX=180, m_srcAspectRatioY=311;
    int m_iRotation=0;
    int m_iMaxineOperation=MAXINE_OPERATION_Upscale;
    int m_iMaxineSourceMode=MAXINE_SOURCE_Standard, m_iMaxineAmount=100;
    int m_iMaxineSourceLimit=SUPERRES_1440p;
    int m_iMaxineDenoise=MAXINE_FILTER_Off, m_iMaxineDeblur=MAXINE_FILTER_Off;
    int m_iMaxineScale=MAXINE_SCALE_MatchOutput, m_iMaxineOversample=MAXINE_OVERSAMPLE_Off;
    UINT m_VendorId=PCIV_NVIDIA;
    bool m_bMaxineOversampleClamped=false, m_bVPUseRTXVideoHDR=false;
    CSize m_DisplaySize{1920,1080};
    std::wstring m_strMaxineVSRStatus;
    bool SourceIsHDR() const { return false; }
    CSize GetRifeSourceContentSize() const {
        const auto size=ResolveRifeContentSize(m_srcRectWidth,m_srcRectHeight,
            m_srcAnamorphic,m_srcAspectRatioX,m_srcAspectRatioY,m_iRotation);
        return {static_cast<int>(size.width),static_cast<int>(size.height)};
    }
    bool GetMaxineVSRTargetSize(const CRect&, CSize&, bool&);
    bool GetMaxineVSRTargetSizeForInput(const CRect&, const CSize&, bool, CSize&, bool&);
};
METHODS
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
int main() {
    try {
        int cases=0;
        // User's exact discrepancy: the same fitted player rectangle should
        // select the portrait output class for raw anamorphic and RIFE input.
        for (int rotation : {0,180}) {
            CDX11VideoProcessor renderer; renderer.m_iRotation=rotation;
            CSize paused, playing; bool up=false;
            require(renderer.GetMaxineVSRTargetSize({630,1080},paused,up) && up,
                    "Raw anamorphic Maxine path was ineligible");
            require(renderer.GetMaxineVSRTargetSizeForInput({630,1080},
                    renderer.GetRifeSourceContentSize(),true,playing,up) && up,
                    "RIFE anamorphic Maxine path was ineligible");
            std::cout << "Anamorphic targets: raw " << paused.cx << 'x' << paused.cy
                      << ", RIFE " << playing.cx << 'x' << playing.cy << '\n';
            require(paused==CSize{1080,1865} && paused==playing,
                    "Paused/source and RIFE choose different portrait output classes");
            ++cases;
        }
        // Quarter turns require targets in each actual input's orientation.
        // Preserve the existing portrait sizing policy for square-pixel video.
        for (int rotation : {90,270}) {
            CDX11VideoProcessor renderer;
            renderer.m_srcRectWidth=1280; renderer.m_srcRectHeight=720;
            renderer.m_srcAnamorphic=false; renderer.m_iRotation=rotation;
            CSize raw, oriented; bool up=false;
            require(renderer.GetMaxineVSRTargetSize({608,1080},raw,up)
                    && raw==CSize{1920,1080}, "Raw rotation target changed");
            require(renderer.GetMaxineVSRTargetSizeForInput({608,1080},
                    renderer.GetRifeSourceContentSize(),true,oriented,up)
                    && oriented==CSize{1080,1920}, "Oriented rotation target changed");
            ++cases;
        }
        // Ordinary landscape and already-portrait paths retain their targets.
        for (bool portrait : {false,true}) for (int oversample : {100,133,150,200}) {
            CDX11VideoProcessor renderer;
            renderer.m_srcRectWidth=portrait?720:1280;
            renderer.m_srcRectHeight=portrait?1280:720;
            renderer.m_srcAnamorphic=false; renderer.m_iMaxineOversample=oversample;
            CSize raw, oriented; bool up=false;
            const CRect fitted=portrait?CRect{608,1080}:CRect{1920,1080};
            require(renderer.GetMaxineVSRTargetSize(fitted,raw,up)
                    && renderer.GetMaxineVSRTargetSizeForInput(fitted,
                        renderer.GetRifeSourceContentSize(),true,oriented,up)
                    && raw==oriented, "Square-pixel target paths diverged");
            require(raw==(portrait?CSize{(1080*oversample+50)/100,(1920*oversample+50)/100}
                                  :CSize{(1920*oversample+50)/100,(1080*oversample+50)/100}),
                    "Existing square-pixel oversampling policy changed");
            ++cases;
        }
        // Explicit scale is intentionally relative to the actual Maxine input.
        CDX11VideoProcessor renderer;
        renderer.m_iMaxineScale=MAXINE_SCALE_2X;
        CSize target; bool up=false;
        require(renderer.GetMaxineVSRTargetSize({630,1080},target,up)
                && target==CSize{1440,960}, "Explicit raw scale changed"); ++cases;
        require(renderer.GetMaxineVSRTargetSizeForInput({630,1080},
                renderer.GetRifeSourceContentSize(),true,target,up)
                && target==CSize{556,960}, "Explicit RIFE scale changed"); ++cases;
        renderer.m_iMaxineScale=MAXINE_SCALE_MatchOutput;
        renderer.m_iMaxineAmount=0;
        require(!renderer.GetMaxineVSRTargetSize({630,1080},target,up),
                "Zero AI amount must bypass Maxine"); ++cases;
        std::cout << "PASS " << cases << " production Maxine target cases (CPU only)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
'''
    replacements = {
        "MAXINE_POLICY": (root / "Source/MaxineSpatialPolicy.h").as_posix(),
        "RIFE_POLICY": (root / "Source/RifeSpatialPolicy.h").as_posix(),
        "ENUMS": enums, "HELPERS": helpers, "METHODS": methods,
    }
    for key, value in replacements.items():
        cpp = cpp.replace(key, value)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(cpp, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[2] / "Source/DX11VideoProcessor.cpp")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
