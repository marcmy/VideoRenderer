/**************************************************************
 Functions that interpolates a frame
 Author: Manao
 Copyright(c) 2006 A.G.Balakhnin aka Fizick - bicubic, Wiener, separable
 Copyright(c) 2014 SVP-Team - SSE2 intrinsics version

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
 http://www.gnu.org/copyleft/gpl.html
*****************************************************************/

#include "common.h"

#include "mvframe_interpolation.h"

#ifndef Q_PROCESSOR_ARM64
#   define USE_SSE 1
#   ifndef Q_PROCESSOR_X86_64
#       include <mmintrin.h>
#       define OLD_ASM 1
#   endif
#endif

void FrameInterpolation::Copy(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight)
{
    for(int h=0;h<nHeight;h++,pDst+=nDstPitch,pSrc+=nSrcPitch)
        memcpy(pDst,pSrc,nWidth);
}

#if OLD_ASM
extern "C" void RB2CubicHorizontalInplaceLine_SSE(uint8_t *pSrc, int nWidthMMX);
extern "C" void RB2QuadraticHorizontalInplaceLine_SSE(uint8_t *pSrc, int nWidthMMX);
extern "C" void RB2FilteredHorizontalInplaceLine_SSE(uint8_t *pSrc, int nWidthMMX);
extern "C" void RB2BilinearFilteredHorizontalInplaceLine_SSE(uint8_t *pSrc, int nWidthMMX);

//extern "C" void HorizontalBilin_iSSE(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight);
//extern "C" void VerticalBilin_iSSE(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight);
//extern "C" void DiagonalBilin_iSSE(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight);
extern "C" void VerticalWiener_iSSE(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight);
extern "C" void HorizontalWiener_iSSE(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight);
extern "C" void VerticalBicubic_iSSE(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight);
extern "C" void HorizontalBicubic_iSSE(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch, int nSrcPitch, int nWidth, int nHeight);
#endif


static void RB2FilteredVertical(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
                         int nSrcPitch, int nWidth, int nHeight)
{
#if USE_SSE
    __m128i k1 = _mm_set1_epi16(2);
    __m128i ks = _mm_cvtsi32_si128(2);
    __m128i z = _mm_setzero_si128();
    int nWidthSSE = (nWidth/8)*8;
#endif

    for(int y = 0; y < nHeight; y++)
    {
        if(y == 0)
            for(int x = 0; x < nWidth; x++)
                pDst[x] = (pSrc[x] + pSrc[x+nSrcPitch] + 1) / 2;
        else
        {
#if USE_SSE
            if (nWidthSSE >= 8)
                for (int x = 0; x < nWidthSSE; x += 8)
                {
                    const uint8_t *p = pSrc + x - nSrcPitch;
                    __m128i s1 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    __m128i s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k1));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z);
                    s1 = _mm_add_epi16(s1, _mm_add_epi16(s2, k1));

                    s1 = _mm_srl_epi16(s1, ks);
                    _mm_storel_epi64((__m128i*)(pDst + x), _mm_packus_epi16(s1, z));
                }

            for (int x = (nWidthSSE >= 8 ? nWidthSSE : 0); x < nWidth; x++)
#else
            for (int x = 0; x < nWidth; x++)
#endif
                pDst[x] = (pSrc[x - nSrcPitch] + pSrc[x] * 2 + pSrc[x + nSrcPitch] + 2) >> 2;
        }

        pDst += nDstPitch; pSrc += nSrcPitch * 2;
    }
}

static void RB2FilteredHorizontalInplace(uint8_t *pSrc, int nSrcPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    int nWidthMMX = 1 + ((nWidth-2)/4)*4;
#endif
    for ( int y = 0; y < nHeight; y++ )
    {
        pSrc[0] = (pSrc[0] + pSrc[1] + 1) >>1;

        int x2 = 2;
#if OLD_ASM
        RB2FilteredHorizontalInplaceLine_SSE(pSrc, nWidthMMX); // very first is skipped
        for (int x = nWidthMMX; x < nWidth; x++)
#else
        for (int x = 1; x < nWidth; x++,x2+=2)
#endif
            pSrc[x] = (pSrc[x2 - 1] + pSrc[x2] * 2 + pSrc[x2 + 1] + 2) >>2;

        pSrc += nSrcPitch;
    }
}

void FrameInterpolation::RB2Filtered(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
                 int nSrcPitch, int nWidth, int nHeight)
{ // separable Filtered with 1/4, 1/2, 1/4 filter for smoothing and anti-aliasing - Fizick v.2.5.2
    // assume he have enough horizontal dimension for intermediate results (double as final)
    RB2FilteredVertical(pDst, pSrc, nDstPitch, nSrcPitch, nWidth*2, nHeight); // intermediate half height
    RB2FilteredHorizontalInplace(pDst, nDstPitch, nWidth, nHeight); // inpace width reduction

#if OLD_ASM
    _clear_sse();
#endif
}

static void RB2BilinearFilteredVertical(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
                                 int nSrcPitch, int nWidth, int nHeight)
{
#if USE_SSE
    __m128i k1 = _mm_set1_epi16(3);
    __m128i k3 = _mm_set1_epi16(4);
    __m128i ks = _mm_cvtsi32_si128(3);
    __m128i z = _mm_setzero_si128();

    int nWidthSSE = (nWidth/8)*8;
#endif

    for(int y = 0; y < nHeight; y++)
    {
        if(y == 0 || y == nHeight-1)
            for(int x = 0; x < nWidth; x++ )
                pDst[x] = (pSrc[x] + pSrc[x+nSrcPitch] + 1) / 2;
        else
        {
#if USE_SSE
            if (nWidthSSE >= 8)
                for(int x = 0; x < nWidthSSE; x += 8)
                {
                    const uint8_t *p = pSrc + x - nSrcPitch;
                    __m128i s1 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    __m128i s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k1));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k1));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z);
                    s1 = _mm_add_epi16(s1, _mm_add_epi16(s2, k3));

                    s1 = _mm_srl_epi16(s1, ks);
                    _mm_storel_epi64((__m128i*)(pDst + x), _mm_packus_epi16(s1, z));
                }

            for(int x = (nWidthSSE>=8 ? nWidthSSE : 0); x < nWidth; x++)
#else
            for(int x = 0; x < nWidth; x++)
#endif
                pDst[x] = (pSrc[x-nSrcPitch] + pSrc[x]*3 + pSrc[x+nSrcPitch]*3 + pSrc[x+nSrcPitch*2] + 4) >>3;
        }

        pDst += nDstPitch; pSrc += nSrcPitch * 2;
    }
}

static void RB2BilinearFilteredHorizontalInplace(uint8_t *pSrc, int nSrcPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    int nWidthMMX = 1 + ((nWidth-2)/4)*4;
#endif
    for ( int y = 0; y < nHeight; y++ )
    {
        pSrc[0] = (pSrc[0] + pSrc[1] + 1) >>1;

        int x2 = 2;
#if OLD_ASM
        RB2BilinearFilteredHorizontalInplaceLine_SSE(pSrc, nWidthMMX); // very first is skipped
        for ( int x = nWidthMMX; x < nWidth-1; x++ )
#else
        for(int x = 1; x < nWidth-1; x++,x2+=2)
#endif
            pSrc[x] = (pSrc[x2 - 1] + pSrc[x2] * 3 + pSrc[x2 + 1] * 3 + pSrc[x2 + 2] + 4) >>3;

        for(int x = _fmax(nWidth-1,1); x < nWidth; x++)
            pSrc[x] = (pSrc[x*2] + pSrc[x*2+1] + 1) / 2;

        pSrc += nSrcPitch;
    }
}

void FrameInterpolation::RB2BilinearFiltered(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
                         int nSrcPitch, int nWidth, int nHeight)
{ // separable BilinearFiltered with 1/8, 3/8, 3/8, 1/8 filter for smoothing and anti-aliasing - Fizick v.2.5.2
    // assume he have enough horizontal dimension for intermediate results (double as final)
    RB2BilinearFilteredVertical(pDst, pSrc, nDstPitch, nSrcPitch, nWidth*2, nHeight); // intermediate half height
    RB2BilinearFilteredHorizontalInplace(pDst, nDstPitch, nWidth, nHeight); // inpace width reduction

#if OLD_ASM
    _clear_sse();
#endif
}

static void RB2QuadraticVertical(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
                          int nSrcPitch, int nWidth, int nHeight)
{
#if USE_SSE
    __m128i k1 = _mm_set1_epi16(9);
    __m128i k2 = _mm_set1_epi16(22);
    __m128i k3 = _mm_set1_epi16(32);
    __m128i ks = _mm_cvtsi32_si128(6);
    __m128i z = _mm_setzero_si128();

    int nWidthSSE = (nWidth / 8) * 8;
#endif

    for (int y = 0; y < nHeight; y++)
    {
        if(y == 0 || y == nHeight-1)
            for(int x = 0; x < nWidth; x++)
                pDst[x] = (pSrc[x] + pSrc[x+nSrcPitch] + 1) / 2;
        else
        {
#if USE_SSE
            if (nWidthSSE >= 8)
                for (int x = 0; x < nWidthSSE; x += 8)
                {
                    const uint8_t *p = pSrc + x - nSrcPitch * 2;
                    __m128i s1 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    __m128i s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k1));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k2));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k2));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k1));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z);
                    s1 = _mm_add_epi16(s1, _mm_add_epi16(s2, k3));

                    s1 = _mm_srl_epi16(s1, ks);
                    _mm_storel_epi64((__m128i*)(pDst + x),_mm_packus_epi16(s1, z));
                }

            for (int x = (nWidthSSE >= 8 ? nWidthSSE : 0); x < nWidth; x++)
#else
            for (int x = 0; x < nWidth; x++)
#endif
                pDst[x] = (pSrc[x-nSrcPitch*2] + pSrc[x-nSrcPitch]*9 + pSrc[x]*22 +
                        pSrc[x+nSrcPitch]*22 + pSrc[x+nSrcPitch*2]*9 + pSrc[x+nSrcPitch*3] + 32) >>6;

            pDst += nDstPitch;
            pSrc += nSrcPitch * 2;
        }
    }
}

static void RB2QuadraticHorizontalInplace(uint8_t *pSrc, int nSrcPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    int nWidthMMX = 1 + ((nWidth-2)/4)*4;
#endif
    for(int y = 0; y < nHeight; y++)
    {
        pSrc[0] = (pSrc[0] + pSrc[1] + 1) >>1;

        int x2 = 2;
#if OLD_ASM
        RB2QuadraticHorizontalInplaceLine_SSE(pSrc, nWidthMMX);
        for ( int x = nWidthMMX; x < nWidth-1; x++ )
#else
        for (int x = 1; x < nWidth - 1; x++,x2+=2)
#endif
            pSrc[x] = (pSrc[x2-2] + pSrc[x2-1]*9 + pSrc[x2]*22 + pSrc[x2+1]*22 + pSrc[x2+2]*9 + pSrc[x2+3] + 32) >>6;

        for ( int x = _fmax(nWidth-1,1); x < nWidth; x++ )
            pSrc[x] = (pSrc[x*2] + pSrc[x*2+1] + 1) / 2;

        pSrc += nSrcPitch;
    }
}

void FrameInterpolation::RB2Quadratic(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
                  int nSrcPitch, int nWidth, int nHeight)
{ // separable filtered Quadratic with 1/64, 9/64, 22/64, 22/64, 9/64, 1/64 filter for smoothing and anti-aliasing - Fizick v.2.5.2
    // assume he have enough horizontal dimension for intermediate results (double as final)
    RB2QuadraticVertical(pDst, pSrc, nDstPitch, nSrcPitch, nWidth*2, nHeight); // intermediate half height
    RB2QuadraticHorizontalInplace(pDst, nDstPitch, nWidth, nHeight); // inpace width reduction

#if OLD_ASM
    _clear_sse();
#endif
}

static void RB2CubicVertical(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
                      int nSrcPitch, int nWidth, int nHeight)
{
#if USE_SSE
    __m128i k1 = _mm_set1_epi16(5);
    __m128i k2 = _mm_set1_epi16(10);
    __m128i k3 = _mm_set1_epi16(16);
    __m128i ks = _mm_cvtsi32_si128(5);
    __m128i z = _mm_setzero_si128();

    int nWidthSSE = (nWidth/8)*8;
#endif

    for(int y = 0; y < nHeight; y++)
    {
        if(y == 0 || y == nHeight-1)
            for(int x = 0; x < nWidth; x++)
                pDst[x] = (pSrc[x] + pSrc[x+nSrcPitch] + 1) / 2;
        else
        {
#if USE_SSE
            if (nWidthSSE >= 8)
                for (int x = 0; x < nWidthSSE; x += 8)
                {
                    const uint8_t *p = pSrc + x - nSrcPitch * 2;
                    __m128i s1 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    __m128i s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k1));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k2));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k2));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z); p += nSrcPitch;
                    s1 = _mm_add_epi16(s1, _mm_mullo_epi16(s2, k1));
                    s2 = _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i*)p), z);
                    s1 = _mm_add_epi16(s1, _mm_add_epi16(s2, k3));

                    s1 = _mm_srl_epi16(s1, ks);
                    _mm_storel_epi64((__m128i*)(pDst + x), _mm_packus_epi16(s1, z));
                }

            for (int x = (nWidthSSE >= 8 ? nWidthSSE : 0); x < nWidth; x++)
#else
            for (int x = 0; x < nWidth; x++)
#endif
                pDst[x] = (pSrc[x - nSrcPitch*2] + pSrc[x - nSrcPitch] * 5 + pSrc[x] * 10 +
                        pSrc[x + nSrcPitch] * 10 + pSrc[x + nSrcPitch*2] * 5 + pSrc[x + nSrcPitch*3] + 16) >> 5;
        }

        pDst += nDstPitch; pSrc += nSrcPitch * 2;
    }
}

static void RB2CubicHorizontalInplace(uint8_t *pSrc, int nSrcPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    int nWidthMMX = 1 + ((nWidth-2)/4)*4;
#endif
    for ( int y = 0; y < nHeight; y++ )
    {
        pSrc[0] = (pSrc[0] + pSrc[1] + 1) / 2;

        int x2 = 2;
#if OLD_ASM
        RB2CubicHorizontalInplaceLine_SSE(pSrc, nWidthMMX);
        for (int x = nWidthMMX; x < nWidth - 1; x++)
#else
        for (int x = 1; x < nWidth - 1; x++,x2+=2)
#endif
                pSrc[x] = (pSrc[x2 - 2] + pSrc[x2 - 1] * 5 + pSrc[x2] * 10 + pSrc[x2 + 1] * 10 + pSrc[x2 + 2] * 5 + pSrc[x2 + 3] + 16) >>5;

        for (int x = _fmax(nWidth-1,1); x < nWidth; x++ )
            pSrc[x] = (pSrc[x*2] + pSrc[x*2+1] + 1) / 2;

        pSrc += nSrcPitch;
    }
}

void FrameInterpolation::RB2Cubic(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,
              int nSrcPitch, int nWidth, int nHeight)
{ // separable filtered cubic with 1/32, 5/32, 10/32, 10/32, 5/32, 1/32 filter for smoothing and anti-aliasing - Fizick v.2.5.2
    // assume he have enough horizontal dimension for intermediate results (double as final)
    RB2CubicVertical(pDst, pSrc, nDstPitch, nSrcPitch, nWidth*2, nHeight); // intermediate half height
    RB2CubicHorizontalInplace(pDst, nDstPitch, nWidth, nHeight); // inpace width reduction

#if OLD_ASM
    _clear_sse();
#endif
}

void FrameInterpolation::Average2(uint8_t *pDst, const uint8_t *pSrc1, const uint8_t *pSrc2, int nPitch, int nWidth, int nHeight)
{
    for (int y = 0; y < nHeight; y++, pDst += nPitch, pSrc1 += nPitch, pSrc2 += nPitch)
    {
#if USE_SSE
        __m128i *pd = (__m128i*)pDst;
        const __m128i *ps1 = (const __m128i*)pSrc1;
        const __m128i *ps2 = (const __m128i*)pSrc2;
        for (int x = 0; x < nWidth; x += 16, pd++, ps1++, ps2++)
            _mm_storeu_si128(pd, _mm_avg_epu8(_mm_loadu_si128(ps1), _mm_loadu_si128(ps2)));
#else
        for(int x = 0; x < nWidth; x++)
            pDst[x] = (pSrc1[x] + pSrc2[x] + 1) >> 1;
#endif
    }
}

static void _bilin(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight, int shift)
{
    for (int y = 0; y < nHeight; y++, pDst += nPitch, pSrc += nPitch)
    {
#if USE_SSE
        __m128i *pd = (__m128i*)pDst;
        for (int x = 0; x < nWidth; x += 16, pd++)
            _mm_storeu_si128(pd, _mm_avg_epu8(_mm_loadu_si128((const __m128i*)(pSrc + x)), _mm_loadu_si128((const __m128i*)(pSrc + x + shift))));
#else
        for(int x = 0; x < nWidth; x++)
            pDst[x] = (pSrc[x] + pSrc[x + nPitch + shift] + 1) >> 1;
#endif
    }
}

void FrameInterpolation::HorizontalBilin(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight)
{
    //HorizontalBilin_iSSE(pDst, pSrc, nPitch, nPitch, nWidth, nHeight);
    _bilin(pDst, pSrc, nPitch, nWidth, nHeight, 1);
}

void FrameInterpolation::VerticalBilin(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight)
{
    //VerticalBilin_iSSE(pDst, pSrc, nPitch, nPitch, nWidth, nHeight);
    _bilin(pDst, pSrc, nPitch, nWidth, nHeight, nPitch);
    memcpy(pDst, pSrc, nWidth);
}

void FrameInterpolation::DiagonalBilin(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight)
{
    //DiagonalBilin_iSSE(pDst, pSrc, nPitch, nPitch, nWidth, nHeight);
    _bilin(pDst, pSrc, nPitch, nWidth, nHeight, nPitch+1);
    memcpy(pDst, pSrc+1, nWidth);
}

// so called Wiener interpolation. (sharp, similar to Lanczos ?)
// invarint simplified, 6 taps. Weights: (1, -5, 20, 20, -5, 1)/32 - added by Fizick
void FrameInterpolation::VerticalWiener(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    VerticalWiener_iSSE(pDst, pSrc, nPitch, nPitch, nWidth, nHeight);
#else
    for (int j = 0; j < 2; j++)
    {
        for (int i = 0; i < nWidth; i++)
            pDst[i] = (pSrc[i] + pSrc[i + nPitch] + 1) >> 1;
        pDst += nPitch;
        pSrc += nPitch;
    }
    for (int j = 2; j < nHeight - 4; j++)
    {
        for (int i = 0; i < nWidth; i++)
        {
            pDst[i] = _fmin(255, _fmax(0,
                ((pSrc[i - nPitch * 2])
                + (-(pSrc[i - nPitch]) + (pSrc[i] << 2) + (pSrc[i + nPitch] << 2) - (pSrc[i + nPitch * 2])) * 5
                + (pSrc[i + nPitch * 3]) + 16) >> 5));
        }
        pDst += nPitch;
        pSrc += nPitch;
    }
    for (int j = nHeight - 4; j < nHeight - 1; j++)
    {
        for (int i = 0; i < nWidth; i++)
        {
            pDst[i] = (pSrc[i] + pSrc[i + nPitch] + 1) >> 1;
        }

        pDst += nPitch;
        pSrc += nPitch;
    }
    // last row
    for (int i = 0; i < nWidth; i++)
        pDst[i] = pSrc[i];
#endif
}

void FrameInterpolation::HorizontalWiener(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    HorizontalWiener_iSSE(pDst, pSrc, nPitch, nPitch, nWidth, nHeight);
#else
    for (int j = 0; j < nHeight; j++)
    {
        pDst[0] = (pSrc[0] + pSrc[1] + 1) >> 1;
        pDst[1] = (pSrc[1] + pSrc[2] + 1) >> 1;
        for (int i = 2; i < nWidth - 4; i++)
        {
            pDst[i] = _fmin(255, _fmax(0, ((pSrc[i - 2]) + (-(pSrc[i - 1]) + (pSrc[i] << 2)
                + (pSrc[i + 1] << 2) - (pSrc[i + 2])) * 5 + (pSrc[i + 3]) + 16) >> 5));
        }
        for (int i = nWidth - 4; i < nWidth - 1; i++)
            pDst[i] = (pSrc[i] + pSrc[i + 1] + 1) >> 1;

        pDst[nWidth - 1] = pSrc[nWidth - 1];
        pDst += nPitch;
        pSrc += nPitch;
    }
#endif
}

// bicubic (Catmull-Rom 4 taps interpolation)
void FrameInterpolation::VerticalBicubic(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    VerticalBicubic_iSSE(pDst, pSrc, nPitch, nPitch, nWidth, nHeight);
#else
    for (int j = 0; j < 1; j++)
    {
        for (int i = 0; i < nWidth; i++)
            pDst[i] = (pSrc[i] + pSrc[i + nPitch] + 1) >> 1;
        pDst += nPitch;
        pSrc += nPitch;
    }
    for (int j = 1; j < nHeight - 3; j++)
    {
        for (int i = 0; i < nWidth; i++)
        {
            pDst[i] = _fmin(255, _fmax(0,
                (-pSrc[i - nPitch] - pSrc[i + nPitch * 2] + (pSrc[i] + pSrc[i + nPitch]) * 9 + 8) >> 4));
        }
        pDst += nPitch;
        pSrc += nPitch;
    }
    for (int j = nHeight - 3; j < nHeight - 1; j++)
    {
        for (int i = 0; i < nWidth; i++)
        {
            pDst[i] = (pSrc[i] + pSrc[i + nPitch] + 1) >> 1;
        }

        pDst += nPitch;
        pSrc += nPitch;
    }
    // last row
    for (int i = 0; i < nWidth; i++)
        pDst[i] = pSrc[i];
#endif
}

void FrameInterpolation::HorizontalBicubic(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight)
{
#if OLD_ASM
    HorizontalBicubic_iSSE(pDst, pSrc, nPitch, nPitch, nWidth, nHeight);
#else
    for (int j = 0; j < nHeight; j++)
    {
        pDst[0] = (pSrc[0] + pSrc[1] + 1) >> 1;
        for (int i = 1; i < nWidth - 3; i++)
        {
            pDst[i] = _fmin(255, _fmax(0,
                (-(pSrc[i - 1] + pSrc[i + 2]) + (pSrc[i] + pSrc[i + 1]) * 9 + 8) >> 4));
        }
        for (int i = nWidth - 3; i < nWidth - 1; i++)
            pDst[i] = (pSrc[i] + pSrc[i + 1] + 1) >> 1;

        pDst[nWidth - 1] = pSrc[nWidth - 1];
        pDst += nPitch;
        pSrc += nPitch;
    }
#endif
}
