#pragma once

class FrameInterpolation
{
public:
    //copy
    static void Copy(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,int nSrcPitch, int nWidth, int nHeight);

    //reduce
    static void RB2Filtered(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,int nSrcPitch, int nWidth, int nHeight);
    static void RB2BilinearFiltered(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,int nSrcPitch, int nWidth, int nHeight);
    static void RB2Quadratic(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,int nSrcPitch, int nWidth, int nHeight);
    static void RB2Cubic(uint8_t *pDst, const uint8_t *pSrc, int nDstPitch,int nSrcPitch, int nWidth, int nHeight);

    //refine
    static void VerticalWiener(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight);
    static void HorizontalWiener(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight);
    static void VerticalBicubic(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight);
    static void HorizontalBicubic(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight);

    static void HorizontalBilin(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight);
    static void VerticalBilin(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight);
    static void DiagonalBilin(uint8_t *pDst, const uint8_t *pSrc, int nPitch, int nWidth, int nHeight);

    static void Average2(uint8_t *pDst, const uint8_t *pSrc1, const uint8_t *pSrc2, int nPitch, int nWidth, int nHeight);
};


