"""Compile production Configure fragments against CPU-only resource spies.

The regression is loss of the paused source input when only Maxine changes.
No D3D device or Maxine runtime is created. An old-source argument allows the
same behavioral checks to demonstrate the regression before the repair.
"""
import argparse
from pathlib import Path


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


def prepare(source, output):
    text = source.read_text(encoding="utf-8")
    configure = section(text, "void CDX11VideoProcessor::Configure(",
                        "void CDX11VideoProcessor::SetRotation(")
    parts = [
        section(configure, "\tif (config.iTexFormat", "\tif (m_srcParams.cformat"),
        section(configure, "\tif (config.iMaxineOperation", "\tif (config.iFrameInterpolationMode"),
        section(configure, "\tif (changeTextures)", "\tif (changeConvertShader)"),
        section(configure, "\tif (changeSuperRes || changeMaxineVSR)", "\tUpdateStatsStatic();"),
    ]
    # The output-only update used by the spies must not hide a source reset.
    update = section(text, "void CDX11VideoProcessor::UpdateTexures()",
                     "void CDX11VideoProcessor::UpdatePostScaleTexures()")
    for forbidden in ("m_TexSrcVideo.Release", "InitializeD3D11VP(",
                      "m_D3D11VP.Release", "m_D3D11VP.Init"):
        assert forbidden not in update, forbidden

    defaults = {
        "iTexFormat": 0, "iMaxineOperation": 1, "iMaxineSourceMode": 1,
        "iMaxineQuality": 3, "iMaxineScale": 0, "iMaxineOversample": 100,
        "iMaxineSourceLimit": 3, "iMaxineDenoise": 0, "iMaxineDeblur": 0,
        "iMaxinePipeline": 0, "iMaxineGPU": -1, "iMaxineAutoBitrate": 6,
        "iMaxineAmount": 60,
    }
    settings = "\n".join(f"int {key} = {value};" for key, value in defaults.items())
    members = "\n".join(f"int m_{key} = {value};" for key, value in defaults.items())
    edits = ",\n".join("&Settings_t::" + key for key in defaults
                       if key not in ("iTexFormat", "iMaxineAmount"))
    cpp = r'''
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
constexpr int S_OK=0, MAXINE_OPERATION_Disabled=0, SUPERRES_Disable=0;
#define EXECUTE_ASSERT(x) do { if (!(x)) throw std::runtime_error(#x); } while (0)
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Settings_t { SETTINGS };
struct Texture { bool populated=true; void Release() { populated=false; } };
struct Key { bool valid=true; void Clear() { valid=false; } };
struct VideoProcessor {
    bool ready=true, inputPresent=true;
    int superRes=-1;
    bool IsReady() const { return ready; }
    int SetSuperRes(int value) { superRes=value; return S_OK; }
};
class CDX11VideoProcessor {
public:
    MEMBERS
    int m_iMaxineStrength=100, m_iVPSuperRes=3;
    bool m_bVPScaling=true, m_bVPUseSuperRes=false;
    bool m_bMaxineAmountPending=true, m_bMaxineAmountBaselineOriented=true;
    void* m_MaxineAmountExpectedTexture=this;
    Key m_RifePreparedMaxineKey;
    Texture m_MaxineAmountSource, m_TexMaxineAmountEnhanced;
    Texture m_TexMaxineAmountBaseline, m_TexMaxineAmountVPBaseline;
    VideoProcessor m_D3D11VP;
    struct Params { int CDepth=8; } m_srcParams;
    struct Filter { int m_inputMT=0; } filter;
    Filter* m_pFilter=&filter;
    int m_srcWidth=720, m_srcHeight=406;
    int sourceRebuilds=0, maxineUpdates=0, postScaleUpdates=0, formatUpdates=0;
    uint64_t sourcePixels=0x23456789;
    void UpdateTexParams(int) { ++formatUpdates; }
    int InitializeD3D11VP(const Params&, int, int, const int*) {
        ++sourceRebuilds;
        sourcePixels=0;
        m_D3D11VP.inputPresent=false;
        return S_OK;
    }
    void UpdateTexures() { ++maxineUpdates; }
    void UpdatePostScaleTexures() { ++postScaleUpdates; }
    void Configure(const Settings_t& config) {
        bool changeTextures=false, changeMaxineVSR=false;
        bool changeMaxineTextures=false, changeSuperRes=false;
        (void)changeMaxineTextures;
        FRAGMENTS
    }
    void requireSource() const {
        require(sourceRebuilds==0 && sourcePixels==0x23456789
                && m_D3D11VP.inputPresent,
                "Maxine-only setting discarded the paused source input");
        require(formatUpdates==0 && postScaleUpdates==0,
                "Maxine-only setting rebuilt unrelated source/presentation resources");
    }
};
int main() {
    try {
        const int Settings_t::* unused=nullptr; (void)unused;
        int Settings_t::* edits[]={ EDITS };
        int cases=0;
        for (bool vpReady : {true,false}) {
            for (auto field : edits) {
                CDX11VideoProcessor renderer;
                renderer.m_D3D11VP.ready=vpReady;
                Settings_t config; ++(config.*field);
                renderer.Configure(config); renderer.requireSource();
                require(renderer.maxineUpdates==1,
                        "Maxine setting failed to invalidate enhancement resources");
                ++cases;
            }
            for (int initial : {0,1,60,100}) for (int amount : {0,1,60,100}) {
                CDX11VideoProcessor renderer;
                renderer.m_D3D11VP.ready=vpReady;
                renderer.m_iMaxineAmount=initial;
                Settings_t config; config.iMaxineAmount=amount;
                renderer.Configure(config); renderer.requireSource();
                require(renderer.m_iMaxineAmount==amount, "AI amount not applied");
                require(renderer.maxineUpdates==int((initial==0)!=(amount==0)),
                        "Amount changed resource layout unnecessarily or missed bypass change");
                if (initial!=amount) {
                    require(!renderer.m_RifePreparedMaxineKey.valid
                            && !renderer.m_bMaxineAmountPending
                            && !renderer.m_MaxineAmountExpectedTexture
                            && !renderer.m_MaxineAmountSource.populated,
                            "AI amount reused prepared or pending pixels");
                    if (amount==100) require(!renderer.m_TexMaxineAmountEnhanced.populated
                            && !renderer.m_TexMaxineAmountBaseline.populated
                            && !renderer.m_TexMaxineAmountVPBaseline.populated,
                            "Full AI amount retained obsolete blend resources");
                }
                ++cases;
            }
        }
        for (bool changeMaxine : {false,true}) {
            CDX11VideoProcessor renderer;
            Settings_t config; ++config.iTexFormat;
            if (changeMaxine) ++config.iMaxineQuality;
            renderer.Configure(config);
            require(renderer.sourceRebuilds==1 && renderer.formatUpdates==1
                    && renderer.maxineUpdates==1 && renderer.postScaleUpdates==1,
                    "A real texture-format change must retain the original rebuild path");
            ++cases;
        }
        for (int operation : {0,1}) {
            CDX11VideoProcessor renderer; Settings_t config;
            config.iMaxineOperation=operation;
            renderer.Configure(config); renderer.requireSource();
            if (operation==0) require(renderer.m_D3D11VP.superRes==renderer.m_iVPSuperRes,
                                     "Disabling Maxine did not restore saved native VSR request");
            renderer.m_iMaxineOperation=0; config.iMaxineOperation=1;
            renderer.Configure(config); renderer.requireSource();
            require(renderer.m_D3D11VP.superRes==SUPERRES_Disable,
                    "Enabling Maxine did not suppress native VSR");
            ++cases;
        }
        std::cout << "PASS " << cases << " production Maxine settings cases: "
                     "paused input preserved, amount transitions, real format rebuilds, VSR exclusion\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
'''
    cpp = cpp.replace("SETTINGS", settings).replace("MEMBERS", members)
    cpp = cpp.replace("FRAGMENTS", "\n".join(parts)).replace("EDITS", edits)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(cpp, encoding="utf-8", newline="\n")
    print(f"Prepared production settings regression: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    prepare(args.source, args.output)
