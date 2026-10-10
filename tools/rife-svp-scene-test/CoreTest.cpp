#include "../../Source/SvpSceneMotion.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using Image = std::vector<uint8_t>;
unsigned checks = 0;
void Check(bool value, const char* text) { ++checks; if (!value) { fprintf(stderr, "FAIL %s\n", text); exit(1); } }
uint32_t Hash(uint32_t v) { v ^= v >> 16; v *= 0x7feb352d; v ^= v >> 15; v *= 0x846ca68b; return v ^ (v >> 16); }
Image Picture(unsigned w, unsigned h, int ox = 0, int oy = 0, int seed = 0, double zoom = 1) {
    Image out(w*h*4);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const int xx = int((x - w * .5) / zoom + w * .5) + ox;
        const int yy = int((y - h * .5) / zoom + h * .5) + oy;
        const auto coarse = Hash((xx/7 + 3000) * 1117 + (yy/7 + 3000) * 7879 + seed*65537) % 75;
        const int value = std::clamp(int(100 + 50*sin(xx*.073 + seed) + 35*cos(yy*.091 - seed) + coarse), 0, 255);
        for (unsigned c = 0; c < 3; ++c) out[(y*w+x)*4+c] = value;
        out[(y*w+x)*4+3] = 255;
    }
    return out;
}
double Difference(const Image& a, const Image& b) {
    uint64_t sum = 0;
    for (size_t i = 0; i < a.size(); i += 4) sum += abs(int(a[i]) - int(b[i]));
    return sum / (255.0 * a.size() / 4);
}
void RunShape(unsigned w, unsigned h) {
    CSvpSceneMotion detector;
    auto a = Picture(w,h);
    SvpSceneResult result;
    auto test = [&](const char* label, const Image& b, bool cut) {
        Check(detector.Analyze(a.data(),w*4,b.data(),w*4,w,h,result), "analysis success");
        printf("%ux%u %-18s cut %d bad %.4f error %.4f image %.4f\n", w,h,label,result.cut,result.badFraction,result.meanError,Difference(a,b));
        Check(result.cut == cut, label);
    };
    test("identical",a,false);
    auto alpha = a;
    for(size_t i=3;i<alpha.size();i+=4) alpha[i]=0;
    test("alpha-only",alpha,false);
    test("pan +8",Picture(w,h,8),false);
    test("pan -8",Picture(w,h,-8),false);
    test("diagonal pan",Picture(w,h,4,-4),false);
    if(w >= 128) {
        auto pan = Picture(w,h,24);
        Check(Difference(a,pan) > .15,"motion exceeds fixed-pixel image threshold");
        test("large pan +24",pan,false);
    }
    test("zoom 1.04",Picture(w,h,0,0,0,1.04),false);
    auto noise = a;
    for(size_t i=0;i<noise.size();i+=4) for(unsigned c=0;c<3;++c) noise[i+c]=std::clamp(int(noise[i+c])+int(Hash(i)%13)-6,0,255);
    test("compression noise",noise,false);
    auto occlusion = a;
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w/10;++x) for(unsigned c=0;c<3;++c) occlusion[(y*w+x)*4+c]=0;
    test("10pct occlusion",occlusion,false);
    auto partialCut = a;
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w/2;++x) for(unsigned c=0;c<3;++c) partialCut[(y*w+x)*4+c]=0;
    test("50pct changed area",partialCut,true);
    Image black(w*h*4,0),white(w*h*4,255);
    Check(detector.Analyze(black.data(),w*4,white.data(),w*4,w,h,result) && result.cut,"hard cut black white");
    // Round-trip shape changes and old pair revisits must not retain decisions.
    test("revisit old pair",a,false);
    SvpSceneResult earlier = result;
    detector.Reset();
    test("reset same pair",a,false);
    Check(result.badFraction == earlier.badFraction,"deterministic reset");
    // Independent pixel field; changing the phases of the sinusoidal fixture
    // alone can be explained by a legitimate translation of the same pattern.
    auto other = a;
    for(size_t i=0;i<other.size();i+=4) for(unsigned c=0;c<3;++c) other[i+c]=Hash(i+99117)%256;
    test("new scene",other,true);
}
int main() {
    for(auto [w,h] : {std::pair{160u,128u}, {320u,180u}, {427u,240u}, {480u,270u}, {272u,480u}, {33u,37u}}) RunShape(w,h);
    CSvpSceneMotion detector;
    SvpSceneResult result;
    Image dummy(4,0);
    Check(!detector.Analyze(nullptr,0,dummy.data(),4,32,32,result),"null input");
    Check(!detector.Analyze(dummy.data(),4,dummy.data(),4,32,32,result),"invalid pitch");
    Check(!detector.Analyze(dummy.data(),4,dummy.data(),4,1,1,result),"tiny fallback");
    Check(!detector.Analyze(dummy.data(),8192,dummy.data(),8192,2048,2048,result),"work bound");
    // Exercise allocator/layout boundary dimensions and pitch independent of width.
    for(unsigned w : {32u,34u,46u,63u,64u,78u,129u,257u}) for(unsigned h : {32u,35u,62u,127u}) {
        auto a=Picture(w,h); Image padded((w*4+20)*h,0);
        for(unsigned y=0;y<h;++y) std::copy_n(a.data()+y*w*4,w*4,padded.data()+y*(w*4+20));
        Check(detector.Analyze(a.data(),w*4,padded.data(),w*4+20,w,h,result) && !result.cut,"shape/pitch transition");
    }
    auto a=Picture(320,180),b=Picture(320,180,8);
    const auto start=std::chrono::steady_clock::now();
    for(unsigned i=0;i<100;++i) Check(detector.Analyze(a.data(),1280,b.data(),1280,320,180,result)&&!result.cut,"repeat pair");
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()/100;
    std::thread one([&]{CSvpSceneMotion own; SvpSceneResult r; for(int i=0;i<40;++i) if(!own.Analyze(a.data(),1280,a.data(),1280,320,180,r)||r.cut) abort();});
    std::thread two([&]{CSvpSceneMotion own; SvpSceneResult r; for(int i=0;i<40;++i) if(!own.Analyze(a.data(),1280,b.data(),1280,320,180,r)||r.cut) abort();});
    one.join();two.join();
    printf("PASS %u assertions, plus 80 concurrent pair analyses; 320x180 CPU motion %.3f ms/pair\n",checks,ms);
}
