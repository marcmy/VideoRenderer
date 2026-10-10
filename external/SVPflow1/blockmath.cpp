#include <stdio.h>
#include <mutex>

#include "common.h"
#include "blockmath.h"

#ifndef Q_PROCESSOR_ARM64
#   define USE_SSE 1
#endif

static x264_pixel_function_t *globalPfn=0;

static uint8_t *zeroBlock = 0;
static std::mutex mtxLock;
static x264_pixel_cmp_t sad84 = 0;
static x264_pixel_cmp_t satd16 = 0;
static x264_pixel_cmp_t satd84 = 0;

const x264_pixel_function_t* x264functions()
{
    std::lock_guard<std::mutex> lock(mtxLock);

    if (!globalPfn)
    {        
        globalPfn = new x264_pixel_function_t;
        x264_pixel_init(x264_cpu_detect(), globalPfn);

        sad84 = globalPfn->sad[PIXEL_8x4];
        satd16 = globalPfn->satd[PIXEL_16x16];
        satd84 = globalPfn->satd[PIXEL_8x4];

        zeroBlock = (uint8_t*)_aligned_alloc(32*32);
        memset(zeroBlock, 0, 32 * 32);
    }
    return globalPfn;
}

int Sad32x32(const uint8_t* c, int, const uint8_t* r, int rp)
{
#if USE_SSE
    __m128i res = _mm_setzero_si128();
    const __m128i *mc = (const __m128i*)c;

    for (int i = 0; i < 32; i++,r+=rp,mc+=2)
    {
        res = _mm_add_epi64(res, _mm_sad_epu8(_mm_load_si128(mc), _mm_loadu_si128((const __m128i*)r)));
        res = _mm_add_epi64(res, _mm_sad_epu8(_mm_load_si128(mc + 1), _mm_loadu_si128(((const __m128i*)r) + 1)));
    }
    return 	_mm_extract_epi16(res, 0) + (_mm_extract_epi16(res, 1) << 16) +
            _mm_extract_epi16(res, 4) + (_mm_extract_epi16(res, 5) << 16);
#else
    return Sad32x16(c, 32, r, rp) + Sad32x16(c+32*16, 32, r+rp*16, rp);
#endif
}

int Sad32x16(const uint8_t* c, int, const uint8_t* r, int rp)
{
#if USE_SSE
    __m128i res = _mm_setzero_si128();
    const __m128i *mc = (const __m128i*)c;
    for (int i = 0; i < 16; i++, r += rp, mc += 2)
    {
        res = _mm_add_epi64(res, _mm_sad_epu8(_mm_load_si128(mc), _mm_loadu_si128((const __m128i*)r)));
        res = _mm_add_epi64(res, _mm_sad_epu8(_mm_load_si128(mc + 1), _mm_loadu_si128(((const __m128i*)r) + 1)));
    }
    return _mm_extract_epi16(res, 0) + _mm_extract_epi16(res, 4);
#else
    int res = 0;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    res += sad84(c, 8, r, 8); c += 32; r += rp;
    return res;
#endif
}

int Sad2x2(const uint8_t* c, int, const uint8_t* r, int rp)
{
#if USE_SSE
    __m128i tmp = _mm_set_epi16(0, 0, 0, 0, 0, 0, *((short*)r), *((short*)(r + rp)));
    return _mm_extract_epi16(_mm_sad_epu8(_mm_set_epi32(0, 0, 0, *((int*)c)), tmp), 0);
#else
#   define _ad(a,b) (a>b ? a-b:b-a)
    return _ad(c[0],r[0]) + _ad(c[1],r[1]) + _ad(c[2],r[rp]) + _ad(c[3],r[rp+1]);
#endif
}

int Sad4x2(const uint8_t* c, int, const uint8_t* r, int rp)
{
#if USE_SSE
    __m128i tmp = _mm_set_epi32(0, 0, *((int*)r), *((int*)(r+rp)));
    return _mm_extract_epi16(_mm_sad_epu8(_mm_loadl_epi64((const __m128i*)c), tmp), 0);
#else
    return Sad2x2(c,2,r,2) + Sad2x2(c+4,2,r+2*rp,2);
#endif
}

int Satd32x32(const uint8_t* c, int, const uint8_t* r, int rp)
{
    return Satd32x16(c, 32, r, rp) + Satd32x16(c+32*16,32,r+rp*16,rp);
}

int Satd32x16(const uint8_t* c, int, const uint8_t* r, int rp)
{
    int res = 0;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    res += satd84(c, 8, r, 8); c += 32; r += rp;
    return res;
}

void Copy32x32(uint8_t* c, const uint8_t *r, int rp)
{
    Copy32x16(c,r,rp); c+=32*16; r+=rp*16;
    Copy32x16(c,r,rp);
}

void Copy32x16(uint8_t *c, const uint8_t *r, int rp)
{
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32); c += 32; r += rp;
    memcpy(c, r, 32);
}

void Copy16x16(uint8_t *c, const uint8_t *r, int rp)
{
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16);
}

void Copy16x8(uint8_t *c, const uint8_t *r, int rp)
{
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16); c+=16; r+=rp;
    memcpy(c, r, 16);
}

void Copy8x8(uint8_t *c, const uint8_t *r, int rp)
{
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8);
}

void Copy8x4(uint8_t *c, const uint8_t *r, int rp)
{
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8); c+=8; r+=rp;
    memcpy(c, r, 8);
}

void Copy4x2(uint8_t *c, const uint8_t *r, int rp)
{
    *((uint32_t*)c) = *((uint32_t*)r);
    *((uint32_t*)(c+4)) = *((uint32_t*)(r+rp));
}

void Copy4x4(uint8_t *c,const uint8_t *r, int rp)
{
    Copy4x2(c, r, rp); c+=8; r+=rp+rp;
    Copy4x2(c, r, rp);
}

void Copy2x2(uint8_t *c, const uint8_t *r, int rp)
{
    c[0] = r[0]; c[1] = r[1]; r+=rp;
    c[2] = r[0]; c[3] = r[1];
}

int Luma32x32(const uint8_t* c, int cp)
{
    return Sad32x32(zeroBlock, cp, c, cp) >> 10;
}

int Luma32x16(const uint8_t* c, int cp)
{
    return Sad32x16(zeroBlock, cp, c, cp) >> 9;
}

int Luma16x16(const uint8_t* c, int cp)
{
    return globalPfn->sad[PIXEL_16x16](zeroBlock, cp, c, cp) >> 8;
}

int Luma16x8(const uint8_t* c, int cp)
{
    return globalPfn->sad[PIXEL_16x8](zeroBlock, cp, c, cp) >> 7;
}

int Luma8x8(const uint8_t* c, int cp)
{
    return globalPfn->sad[PIXEL_8x8](zeroBlock, cp, c, cp) >> 6;
}

int Luma8x4(const uint8_t* c, int cp)
{
    return globalPfn->sad[PIXEL_8x4](zeroBlock, cp, c, cp) >> 5;
}

int Luma4x4(const uint8_t* c, int cp)
{
    return globalPfn->sad[PIXEL_4x4](zeroBlock, cp, c, cp) >> 4;
}
