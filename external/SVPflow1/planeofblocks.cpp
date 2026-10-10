/**************************************************************
 SVPFlow1 - motion vectors search for SmoothVideo Project (SVP)
 Based on MVTools by Manao
 Copyright(c)2011 Chainik - http://www.svp-team.com
 Copyright(c)2006 A.G.Balakhnin aka Fizick - http://avisynth.org.ru

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

#include <math.h>
#include <stdlib.h>

#include <algorithm>

#include "x264_pixel.h"
#include "blockmath.h"

#include "planeofblocks.h"

/* returns the biggest integer x such as 2^x <= i */
inline static int ilog2(int i)
{
    int result = 0;
    while ( i > 1 ) { i /= 2; result++; }
    return result;
}

PlaneOfBlocks::PlaneOfBlocks(int _nBlkX, int _nBlkY, int _nBlkSizeX, int _nBlkSizeY,
                             int _nOverlapX, int _nOverlapY,
                             int nPel, int nLevel, int levelsCount, bool smallestPlane,
                             bool useSATD)
{
    this->nPel = nPel;
    this->nLevel = nLevel;
    this->levelsCount=levelsCount;
    this->smallestPlane = smallestPlane;

    nBlkSizeX = _nBlkSizeX;
    nBlkSizeY = _nBlkSizeY;
    nOverlapX = _nOverlapX;
    nOverlapY = _nOverlapY;

    isHalfSized = false;

    nBlkX = _nBlkX;
    nBlkY = _nBlkY;
    nBlkCount = nBlkX * nBlkY;
    int nBlkCountExt=(nBlkX+1)*(nBlkY+1);

    /* arrays memory allocation */
    vectors = new VECTOR[nBlkCount];
    memset(vectors, 0, nBlkCount*sizeof(VECTOR));

    reverseVectors.resize(nBlkCount);

    sortedBlocks.resize(nLevel>0 ? nBlkCountExt:nBlkCount);
    processedBlocks.resize(nBlkCountExt);
    vectorsBorder.resize(nBlkX+nBlkY+1);

    int shift=ilog2(nBlkSizeX*nBlkSizeY*256/2048);
    sortBuf.resize((nBlkSizeX*nBlkSizeY*255)>>shift);

    predictors.resize(9);
    globalMVPredictor = VECTOR();

    InitSADFunctions(useSATD);
    InitAsmFunctions();

    nBlkSize_UV=nBlkSizeX/2;

    int blocksize=nBlkSizeX*nBlkSizeY;
    pSrcTmp[0]=(uint8_t*)_aligned_alloc(blocksize+blocksize/2);
    pSrcTmp[1]=pSrcTmp[0]+blocksize;
    pSrcTmp[2]=pSrcTmp[1]+blocksize/4;
    pSrcTmp[3]=(uint8_t*)_aligned_alloc(blocksize+blocksize/2);
    pSrcTmp[4]=pSrcTmp[3]+blocksize;
    pSrcTmp[5]=pSrcTmp[4]+blocksize/4;
    SetActiveSrc(0);

    pCmp=(uint8_t*)_aligned_alloc(blocksize);
    pTmp=(uint8_t*)_aligned_alloc(blocksize);

    freqSize = 8192*nPel*2;// half must be more than max vector length, which is (framewidth + Padding) * nPel
    freqArray = new int[freqSize];
}

PlaneOfBlocks::~PlaneOfBlocks()
{
    delete[] vectors;
    delete[] freqArray;

    _aligned_free(pSrcTmp[0]);
    _aligned_free(pSrcTmp[3]);
    _aligned_free(pCmp);
    _aligned_free(pTmp);
}

void PlaneOfBlocks::InitSADFunctions(bool satd)
{
    /* function's pointers initialization */
#define SET_FUNCPTR(blksizex, blksizey, blksizex2, blksizey2) \
    AVERAGE[F_LUMA_C] = Luma##blksizex##x##blksizey; \
    AVERAGE[F_CHROMA_C] = Luma##blksizex2##x##blksizey2; \
    BLIT[F_LUMA_C] = Copy##blksizex##x##blksizey; \
    BLIT[F_CHROMA_C] = Copy##blksizex2##x##blksizey2;

    const x264_pixel_cmp_t* fcmp = (satd ? x264functions()->satd : x264functions()->sad);

    switch(nBlkSizeX)
    {
    case 32:
        switch(nBlkSizeY)
        {
        case 32: SET_FUNCPTR(32,32,16,16);
            SAD[F_LUMA_C] = satd ? Satd32x32:Sad32x32;
            SAD[F_CHROMA_C]=fcmp[PIXEL_16x16];
            break;
        case 16: SET_FUNCPTR(32,16,16,8);
            SAD[F_LUMA_C] = satd ? Satd32x16:Sad32x16;
            SAD[F_CHROMA_C]=fcmp[PIXEL_16x8];
            break;
        } break;
    case 16:
        switch(nBlkSizeY)
        {
        case 16: SET_FUNCPTR(16,16,8,8);
            SAD[F_LUMA_C]=fcmp[PIXEL_16x16];
            SAD[F_CHROMA_C]=fcmp[PIXEL_8x8];
            break;
        case 8: SET_FUNCPTR(16,8,8,4);
            SAD[F_LUMA_C]=fcmp[PIXEL_16x8];
            SAD[F_CHROMA_C]=fcmp[PIXEL_8x4];
            break;
        } break;
    case 8:
        switch(nBlkSizeY)
        {
        case 8: SET_FUNCPTR(8,8,4,4);
            SAD[F_LUMA_C]=fcmp[PIXEL_8x8];
            SAD[F_CHROMA_C]=fcmp[PIXEL_4x4];
            break;
        case 4: SET_FUNCPTR(8,4,4,2);
            SAD[F_LUMA_C]=fcmp[PIXEL_8x4];
            SAD[F_CHROMA_C] = Sad4x2;
            break;
        } break;
    case 4:
        SET_FUNCPTR(4,4,2,2);
        SAD[F_LUMA_C]=fcmp[PIXEL_4x4];
        SAD[F_CHROMA_C] = Sad2x2;
        break;
    }
}

void PlaneOfBlocks::InitAsmFunctions(bool fullBlock)
{
    if(fullBlock)
    {
        SAD[F_LUMA]=SAD[F_LUMA_C];
        SAD[F_CHROMA]=SAD[F_CHROMA_C];
        BLIT[F_LUMA]=BLIT[F_LUMA_C];
        BLIT[F_CHROMA]=BLIT[F_CHROMA_C];
        AVERAGE[F_LUMA]=AVERAGE[F_LUMA_C];
        AVERAGE[F_CHROMA]=AVERAGE[F_CHROMA_C];

        if (isHalfSized)
        {
            nBlkSizeX *= 2;
            nBlkSizeY *= 2;
            nBlkSize_UV *= 2;
        }
    }
    else
    {
        SAD[F_LUMA]=SAD[F_CHROMA_C];
        SAD[F_CHROMA]=SAD_null;
        BLIT[F_LUMA]=BLIT[F_CHROMA_C];
        BLIT[F_CHROMA]=BLIT_null;
        AVERAGE[F_LUMA]=AVERAGE[F_CHROMA_C];
        AVERAGE[F_CHROMA]=AVERAGE_null;

        if (!isHalfSized)
        {
            nBlkSizeX /= 2;
            nBlkSizeY /= 2;
            nBlkSize_UV /= 2;
        }
    }

    isHalfSized = !fullBlock;
}

void PlaneOfBlocks::SearchMVs(const MVFrame *pSrcFrame, const MVFrame *pRefFrame,
                              SearchType st, int stp, int lambda, int lsad, int pnew,
                              double plevel, VECTOR * globalMVec,
                              int pzero, int pglobal, int pnbour, int preverse, int badSAD, int _badrange,
                              bool _tryMany, int flags, int *lambdaOut, int *out)
{
    globalMVPredictor.Init(nPel*globalMVec->x,nPel*globalMVec->y,globalMVec->sad);

    penaltyZero = pzero;
    penaltyGlobal = pglobal;
    penaltyNew = pnew;
    penaltyNeighbour = pnbour;
    penaltyReverse = preverse;
    this->flags=flags;

    int m=(flags&0x60)>>5;
    if(m==0) m=3;
    tryMany = (_tryMany && nLevel>=m);

    m=(flags&0x0C)>>2;
    if(m==0) m=3;
    badrange = (_badrange && nLevel>=m);

    if(smallestPlane)
    {
        memset(vectors, 0, nBlkCount*sizeof(VECTOR));
        SetOrder();

        penaltyNew=0;
        lambda=0;
    }

    SearchMVs_common(true,pSrcFrame,pRefFrame,st,stp,lambda,lsad,plevel,badSAD,lambdaOut,out);
}

void PlaneOfBlocks::RecalculateMVs(const MVFrame *pSrcFrame, const MVFrame *pRefFrame,
                                   SearchType st, int stp, int lambda, int lsad, int pnew,
                                   int thSAD, int flags, int *lambdaOut, int *out)
{

    penaltyNew = pnew;
    this->flags=flags;

    SearchMVs_common(false,pSrcFrame,pRefFrame,st,stp,lambda,lsad,0,thSAD,lambdaOut,out);
}

void PlaneOfBlocks::SearchMVs_common(bool mode,const MVFrame *pSrcFrame, const MVFrame *pRefFrame,
                              SearchType st, int stp, int lambda, int lsad,
                              double plevel, int badSAD, int *lambdaOut, int *pBlkData)
{
    std::fill(processedBlocks.begin(),processedBlocks.end(),false);
    blockLuma=0;

    refYPlane=pRefFrame->GetPlane(YPLANE);
    refUPlane=pRefFrame->GetPlane(UPLANE);
    refVPlane=pRefFrame->GetPlane(VPLANE);

    srcYPlane=pSrcFrame->GetPlane(YPLANE);
    srcUPlane=pSrcFrame->GetPlane(UPLANE);
    srcVPlane=pSrcFrame->GetPlane(VPLANE);

    nRefPitchY = refYPlane->GetPitch();
    nRefPitchUV = refUPlane->GetPitch();

    searchType = st;
    int globalSearchParam = (stp ? abs(stp) : (nLevel ? 10:0));
    adaptiveRadius=(globalSearchParam>0 && stp<=0);// && !smallestPlane);

    if (mode) lambda = int(_fmin(lambda / pow(plevel, levelsCount - nLevel - 1) / (nPel*nPel), INT_MAX / 100.0));
    else lambda>>=2;
    if(lambdaOut) *lambdaOut=lambda;

	bool borderNegOverlap = (smallestPlane && nBlkSizeX > 4);
    int chromaMul=CHROMA_SHIFT?1:2,chromaDiv=(CHROMA_SHIFT==1?2:3); //shift=0 -> *2/3, 1 -> *1/2, 2 -> *1/3

    InitAsmFunctions(true);

    int diagLen=nPel*int(sqrt(pow(srcYPlane->GetWidth(),2)+pow(srcYPlane->GetHeight(),2)));

    for(std::vector<BLOCK_DEF>::iterator blk_ref=sortedBlocks.begin();blk_ref!=sortedBlocks.end();++blk_ref)
    {
        blky=blk_ref->y;
        blkx=blk_ref->x;
        blkIdx = blky*nBlkX + blkx;

        if(blkx==nBlkX || blky==nBlkY) blkIdx=-1;
        bool negativeOverlap=(borderNegOverlap && (blkx==0 || blkx>=nBlkX-1 || blky==0 || blky>=nBlkY-1));

        int add=(blkx<nBlkX ? blkx*(nBlkSizeX - nOverlapX) : srcYPlane->GetWidth()-nBlkSizeX);
        if(negativeOverlap) add+=nBlkSizeX>>2;
        x[0]=add;
        add>>=1;
        x[1]=x[2]=add;

        add=(blky<nBlkY ? blky*(nBlkSizeY - nOverlapY) : srcYPlane->GetHeight()-nBlkSizeY);
        if(negativeOverlap) add+=nBlkSizeY>>2;
        y[0]=add;
        add>>=1;
        y[1]=y[2]=add;

        x0[0]=x[0];y0[0]=y[0];
        x0[1]=x[1];y0[1]=y[1];
        x0[2]=x[2];y0[2]=y[2];

        /* computes search boundaries */
        nDxMin = -nPel * x[0];
        nDyMin = -nPel * y[0];
        nDxMax = nPel * (srcYPlane->GetWidth() - x[0] - (negativeOverlap ? nBlkSizeX>>1 : nBlkSizeX));
        nDyMax = nPel * (srcYPlane->GetHeight() - y[0] - (negativeOverlap ? nBlkSizeY>>1 : nBlkSizeY));

        nDxMin0=nDxMin;
        nDyMin0=nDyMin;
        nDxMax0=nDxMax;
        nDyMax0=nDyMax;

        localLambda=lambda;
        localBadSAD=badSAD;
        localSadLimit=lsad;
        nSearchParam=globalSearchParam;

        int idx=blkIdx;
        if(idx<0) idx=(blkx==nBlkX ? blkx-1:blkx) + (blky==nBlkY ? blky-1:blky)*nBlkX;
        predictor = vectors[idx];

        if(borderNegOverlap)
        {
            if(negativeOverlap)
            {
                localLambda=localLambda*chromaMul/chromaDiv;
                localBadSAD=localBadSAD*chromaMul/chromaDiv;
                localSadLimit=localSadLimit*chromaMul/chromaDiv;
            }
            InitAsmFunctions(!negativeOverlap);
        }

        if(mode) SearchMVs_kernel(negativeOverlap);
        else RecalculateMVs_kernel(pBlkData!=0);

        processedBlocks[blky*(nBlkX+1)+blkx]=true;

        if(negativeOverlap)
            bestMV.sad=bestMV.sad*chromaDiv/chromaMul;

        if(blkIdx>=0)
        {
            vectors[blkIdx] = bestMV;

            if(pBlkData)
            {
                int *dst=pBlkData+blkIdx*2;
                dst[0] = (bestMV.x<<16) + bestMV.y;

                int sad = bestMV.sad;
                int relative=(abs(bestMV.x)+abs(bestMV.y))*100/diagLen;
                if(relative>50)
                    sad*=20;//10
                else if(relative>15)
                    sad+=bestMV.sad*(relative-15)/35*9;

                dst[1] = (sad&0x00ffffff) + (blockLuma<<24);
            }
        }
        else vectorsBorder[blkx+nBlkY-blky] = bestMV;

	if (borderNegOverlap)
		InitAsmFunctions(true);
    }
    _clear_sse();
}

void PlaneOfBlocks::CopySrc(int n,int x0,int y0,int x1,int y1,int x2,int y2)
{
    BLIT[F_LUMA](pSrcTmp[n*3],srcYPlane->GetAbsolutePointerPel1(x0,y0),nRefPitchY);
    BLIT[F_CHROMA](pSrcTmp[n*3+1],srcUPlane->GetAbsolutePointerPel1(x1,y1),nRefPitchUV);
    BLIT[F_CHROMA](pSrcTmp[n*3+2],srcVPlane->GetAbsolutePointerPel1(x2,y2),nRefPitchUV);
    SetActiveSrc(n);
}

void PlaneOfBlocks::SetActiveSrc(int n)
{
    pSrc[0]=pSrcTmp[n*3];
    pSrc[1]=pSrcTmp[n*3+1];
    pSrc[2]=pSrcTmp[n*3+2];
}

bool PlaneOfBlocks::MoveBlock(int vx,int vy,int margin)
{
    int dx=0,dy=0;

    if(margin>0)
    {
        int ax=vx,ay=vy,sx=1,sy=1;
        if(vx<0) {ax=-vx;sx=-1;}
        if(vy<0) {ay=-vy;sy=-1;}

        vx=sx*(ax+margin);
        vy=sy*(ay+margin);
    }

    if(vx<nDxMin0) dx=vx-nDxMin0;
    else if(vx>=nDxMax0) dx=vx-nDxMax0+1;
    if(vy<nDyMin0) dy=vy-nDyMin0;
    else if(vy>=nDyMax0) dy=vy-nDyMax0+1;

    if(dx||dy)
    {
        x[0] = _fmin(_fmax(0, x0[0] - dx), srcYPlane->GetWidth() - nBlkSizeX);
        y[0] = _fmin(_fmax(0, y0[0] - dy), srcYPlane->GetHeight() - nBlkSizeY);
        dx=x0[0]-x[0];
        dy=y0[0]-y[0];

        int dx2=(dx>>1),dy2=(dy>>1);
        x[1]=x0[1]-dx2;y[1]=y0[1]-dy2;
        x[2]=x0[2]-dx2;y[2]=y0[2]-dy2;

        nDxMax=nDxMax0-(dx>0?dx:0);
        nDyMax=nDyMax0-(dy>0?dy:0);
        nDxMin=nDxMin0-(dx<0?dx:0);
        nDyMin=nDyMin0-(dy<0?dy:0);

        CopySrc(1,x[0],y[0],x[1],y[1],x[2],y[2]);
        return true;
    }

    SetActiveSrc(0);
    x[0]=x0[0];x[1]=x0[1];x[2]=x0[2];
    y[0]=y0[0];y[1]=y0[1];y[2]=y0[2];
    return false;
}

bool PlaneOfBlocks::CheckPredictor(const VECTOR& v,int penalty,bool absolute,VECTOR *pBest,int *pCost)
{
    bool res=false;
    MoveBlock(v.x,v.y);

    int sad = GetSAD(v.x,v.y);
    int cost = sad + (absolute ? penalty : (penalty*sad)>>8);

    if(cost<nMinCost || tryMany)
    {
        bestMV.Init(v.x,v.y,sad);
        nMinCost = cost;
        res=true;
    }

    if(tryMany)
    {
        Refine();

        *pBest=bestMV;
        *pCost=bestMV.sad + (absolute ? penalty : (penalty*bestMV.sad)>>8);
    }
    return res;
}

int PlaneOfBlocks::GetContrast(int function,const uint8_t* src,int row,int size,uint8_t *buf,int *average)
{
    int avg=AVERAGE[function](src,row);
    if(average) *average=avg;
    if(!buf) return 0;

    memset(buf,avg,size);
    return SAD[function](src,row,buf,row)/size;
}

void PlaneOfBlocks::SearchMVs_kernel(bool negativeOverlap)
{
    double k=pow((float)localSadLimit/(localSadLimit+(predictor.sad>>1)),2);
    localLambda = int(_fmin(localLambda*k, INT_MAX / 100.0));

    bool dontSearch=(nSearchParam==0 && nLevel==0);

    CopySrc(0,x[0],y[0],x[1],y[1],x[2],y[2]);

    int sz=nBlkSizeX*nBlkSizeY;
    //if(negativeOverlap) sz>>=2;

    bool needContrast=((!dontSearch) && (adaptiveRadius || ((flags&2) && nLevel>0)));

    uint8_t* tmp=pSrc[0];
    if(needContrast && nLevel>0 && (flags&0x10)==0)
    {
        int shift=(nBlkSizeX+1);
        memcpy(pTmp,pSrc[0]+shift,sz-shift);
        memcpy(pTmp+sz-shift,pSrc[0],shift);
        for(int i=0;i<sz;i++)
            pTmp[i]=(pTmp[i]+pSrc[0][i])>>1;
        tmp=pTmp;
    }
    int contrast = GetContrast(F_LUMA, tmp, nBlkSizeX, sz, needContrast ? pCmp : 0, &blockLuma);

    VECTOR bestMVMany[4+8];
    int nMinCostMany[4+8];

    nMinCost=INT_MAX;

    if(needContrast)
    {
        if(!negativeOverlap)
        {
            sz>>=2;
            contrast+=GetContrast(F_CHROMA,pSrc[1],nBlkSize_UV,sz,pCmp);
            contrast+=GetContrast(F_CHROMA,pSrc[2],nBlkSize_UV,sz,pCmp);
        }
        else contrast*=2;
        if(contrast>255) contrast=255;
    }
    if(adaptiveRadius)
    {
        if(contrast<2) nSearchParam=0;
        //else nSearchParam=max(1,(nSearchParam*contrast)>>8);
        else nSearchParam = 1 + ((nSearchParam*_fmin(255, contrast * 255 / 150)) >> 8);
    }

    //predictor from prev. level
    if(predictor.sad>=0)
        CheckPredictor(predictor,0,true,bestMVMany,nMinCostMany);

    if(dontSearch) //do not search at all
        return;

    if(blkIdx>=0 && reverseVectors[blkIdx].sad>0)
    {
        if(CheckPredictor(reverseVectors[blkIdx],penaltyReverse,false,bestMVMany+1,nMinCostMany+1))
            predictor=reverseVectors[blkIdx];
    }

    // Global MV predictor  - added by Fizick
    CheckPredictor(globalMVPredictor,penaltyGlobal,false,bestMVMany+2,nMinCostMany+2);

    //zero vector
    CheckPredictor(VECTOR(),penaltyZero,false,bestMVMany+3,nMinCostMany+3);

    // then all the other predictors
    int num=FetchPredictors();
    for(int i=0;i<num;i++)
    {
        const VECTOR& p=predictors[i];
        CheckPredictor(p,penaltyNeighbour,false,bestMVMany+i+4,nMinCostMany+i+4);
    }

    nMinCost=bestMV.sad;

    if((flags&2) && nLevel>0)
        nMinCost = nMinCost*(60 + _fmin(contrast, 120) / 2) / 120;

    if(tryMany) // select best of multi best
    {
        nMinCost = INT_MAX;
        for(int i=0;i<4+num;i++)
        {
            if(bestMVMany[i].sad>=0 && nMinCostMany[i]<nMinCost)
            {
                bestMV = bestMVMany[i];
                nMinCost = nMinCostMany[i];
            }
        }
    }
    else Refine();

    int foundSAD=bestMV.sad;
    if(badrange && foundSAD>localBadSAD && blkIdx>1)
    {
        //localLambda=0;
        MoveBlock(bestMV.x,bestMV.y,2);

        int x0=bestMV.x,y0=bestMV.y;
        if (badrange > 0) // UMH
            UMHSearch(badrange,x0,y0);
        else if (badrange<0) // ESA
        {
            for(int i=1;i<-badrange;i++)
            {
                ExpandingSearch(i,x0,y0);
                if (bestMV.sad < foundSAD/4) break; // stop search if rather good is found
            }
        }
        ExhaustiveSearch(1);
    }

    _clear_sse();
}

void PlaneOfBlocks::RecalculateMVs_kernel(bool needLuma)
{
    bestMV=predictor;

    bool bad=(bestMV.sad>localBadSAD);

    if(bad || needLuma)
    {
        BLIT[F_LUMA](pSrc[0],srcYPlane->GetAbsolutePointerPel1(x[0],y[0]),nRefPitchY);
        GetContrast(F_LUMA,pSrc[0],nBlkSizeX,nBlkSizeX*nBlkSizeY,0,&blockLuma);

        if(!bad) return;

        BLIT[F_CHROMA](pSrc[1],srcUPlane->GetAbsolutePointerPel1(x[1],y[1]),nRefPitchUV);
        BLIT[F_CHROMA](pSrc[2],srcVPlane->GetAbsolutePointerPel1(x[2],y[2]),nRefPitchUV);

        MoveBlock(predictor.x, predictor.y,2);
        nMinCost = bestMV.sad = GetSAD(predictor.x, predictor.y);
        Refine(true);
    }

    _clear_sse();
}

void PlaneOfBlocks::InterpolatePrediction(const PlaneOfBlocks &pob,bool forRecalc)
{
    int normFactor = (forRecalc ? 4 : 3) - ilog2(nPel) + ilog2(pob.nPel);

    if(!forRecalc)
        std::fill(reverseVectors.begin(),reverseVectors.end(),VECTOR());

    int normov = (nBlkSizeX - nOverlapX)*(nBlkSizeY - nOverlapY);
    int aoddx= (nBlkSizeX*3 - nOverlapX*2);
    int aevenx = (nBlkSizeX*3 - nOverlapX*4);
    int aoddy= (nBlkSizeY*3 - nOverlapY*2);
    int aeveny = (nBlkSizeY*3 - nOverlapY*4);
    // note: overlapping is still (v2.5.7) not processed properly
    for ( int l = 0, index = 0; l < nBlkY; l++ )
    {
        for ( int k = 0; k < nBlkX; k++, index++ )
        {
            VECTOR v1, v2, v3, v4;
            int i = k;
            int j = l;

            if(forRecalc)
            {
                if ( i >= 2 * pob.nBlkX ) i= 2 * pob.nBlkX-1;
                if ( j >= 2 * pob.nBlkY ) j= 2 * pob.nBlkY-1;
            }
            else if(i >= 2*pob.nBlkX || j>= 2*pob.nBlkY)
            {
                vectors[index] = pob.vectorsBorder[_fmin(i / 2, pob.nBlkX) + pob.nBlkY - _fmin(j / 2, pob.nBlkY)];
                vectors[index].x <<= (4-normFactor);
                vectors[index].y <<= (4-normFactor);
                //vectors[index].sad >>= 2;
                continue;
            }

            int offy = -1 + 2 * ( j % 2);
            int offx = -1 + 2 * ( i % 2);

            if (( i == 0 ) || (i >= 2 * pob.nBlkX - 1))
            {
                if (( j == 0 ) || ( j >= 2 * pob.nBlkY - 1))
                {
                    v1 = v2 = v3 = v4 = pob.vectors[i / 2 + (j / 2) * pob.nBlkX];
                }
                else
                {
                    v1 = v2 = pob.vectors[i / 2 + (j / 2) * pob.nBlkX];
                    v3 = v4 = pob.vectors[i / 2 + (j / 2 + offy) * pob.nBlkX];
                }
            }
            else if (( j == 0 ) || ( j >= 2 * pob.nBlkY - 1))
            {
                v1 = v2 = pob.vectors[i / 2 + (j / 2) * pob.nBlkX];
                v3 = v4 = pob.vectors[i / 2 + offx + (j / 2) * pob.nBlkX];
            }
            else
            {
                v1 = pob.vectors[i / 2 + (j / 2) * pob.nBlkX];
                v2 = pob.vectors[i / 2 + offx + (j / 2) * pob.nBlkX];
                v3 = pob.vectors[i / 2 + (j / 2 + offy) * pob.nBlkX];
                v4 = pob.vectors[i / 2 + offx + (j / 2 + offy) * pob.nBlkX];
            }

            if(!forRecalc)
                v2=v3=v4=v1; //for MAG

            VECTOR& target=vectors[index];

            if (nOverlapX == 0 && nOverlapY == 0)
            {
                target.x = 9 * v1.x + 3 * v2.x + 3 * v3.x + v4.x;
                target.y = 9 * v1.y + 3 * v2.y + 3 * v3.y + v4.y;
                target.sad = 9 * v1.sad + 3 * v2.sad + 3 * v3.sad + v4.sad + 8;
            }
            else
            {
                int	ax1 = (offx > 0) ? aoddx : aevenx;
                int ax2 = (nBlkSizeX - nOverlapX)*4 - ax1;
                int ay1 = (offy > 0) ? aoddy : aeveny;
                int ay2 = (nBlkSizeY - nOverlapY)*4 - ay1;
                int a11 = ax1*ay1, a12 = ax1*ay2, a21 = ax2*ay1, a22 = ax2*ay2;
                target.x = (a11*v1.x + a21*v2.x + a12*v3.x + a22*v4.x) /normov;
                target.y = (a11*v1.y + a21*v2.y + a12*v3.y + a22*v4.y) /normov;
                target.sad = (a11*v1.sad + a21*v2.sad + a12*v3.sad + a22*v4.sad) /normov;
            }
            target.x >>= normFactor;// << mulFactor;
            target.y >>= normFactor;// << mulFactor;
            if(!forRecalc) target.sad >>= 4;
            else target.sad >>= 6;

            if(!forRecalc)
            {
                int w=nBlkSizeX-nOverlapX,h=nBlkSizeY-nOverlapY;
                int rbx = _fmin(_fmax((i*w + (nBlkSizeX >> 1) + target.x) / w, 0), nBlkX - 1);
                int rby = _fmin(_fmax((j*h + (nBlkSizeY >> 1) + target.y) / h, 0), nBlkY - 1);
                int rev_index=rby*nBlkX+rbx;

                VECTOR& rv=reverseVectors[rev_index];
                if(rv.sad<0 || target.sad<rv.sad)
                {
                    rv.x=-target.x;
                    rv.y=-target.y;
                    rv.sad=target.sad;
                }
            }
        }
    }
}

int PlaneOfBlocks::FetchPredictors()
{
    int x1 = _fmax(0, blkx - 1), x2 = (blkx<nBlkX ? _fmin(nBlkX - 1, blkx + 1) : blkx - 1);
    int y1 = _fmax(0, blky - 1), y2 = (blky<nBlkY ? _fmin(nBlkY - 1, blky + 1) : blky - 1);

    int c=0;
    //return 0;
    for(int y=y1;y<=y2;y++)
        for(int x=x1;x<=x2;x++)
        {
            int idx=y*nBlkX+x;
            if(processedBlocks[idx+y])
                predictors[c++]=vectors[idx];
        }

    return c;
}

void PlaneOfBlocks::Refine(bool blockMoved)
{
    if(!blockMoved)
        MoveBlock(bestMV.x,bestMV.y,2);

    switch(searchType)
    {
    case EXHAUSTIVE:
        ExhaustiveSearch(nSearchParam);
        break;
    case HEX2SEARCH:
        Hex2Search(nSearchParam);
        break;
    case UMHSEARCH:
        UMHSearch(nSearchParam, bestMV.x, bestMV.y);
        break;
    }
}

void PlaneOfBlocks::EstimateGlobalMVDoubled(VECTOR *globalMVec)
{
    // estimate global motion from current plane vectors data for using on next plane - added by Fizick
    // on input globalMVec is prev estimation
    // on output globalMVec is doubled for next scale plane using

    // use very simple but robust method
    // more advanced method (like MVDepan) can be implemented later

    // find most frequent x
    memset(&freqArray[0], 0, freqSize*sizeof(int)); // reset
    int indmin = freqSize-1;
    int indmax = 0;
    for (int i=0; i<nBlkCount; i++)
    {
        int ind = (freqSize>>1)+vectors[i].x;
        if (ind>=0 && ind<freqSize)
        {
            freqArray[ind] += 1 ;
            if (ind > indmax)
                indmax = ind;
            if (ind < indmin)
                indmin = ind;
        }
    }
    int count = freqArray[indmin];
    int index = indmin;
    for (int i=indmin+1; i<=indmax; i++)
    {
        if (freqArray[i] > count)
        {
            count = freqArray[i];
            index = i;
        }
    }
    int medianx = (index-(freqSize>>1)); // most frequent value

    // find most frequent y
    memset(&freqArray[0], 0, freqSize*sizeof(int)); // reset
    indmin = freqSize-1;
    indmax = 0;
    for (int i=0; i<nBlkCount; i++)
    {
        int ind = (freqSize>>1)+vectors[i].y;
        if (ind>=0 && ind<freqSize)
        {
            freqArray[ind] += 1 ;
            if (ind > indmax)
                indmax = ind;
            if (ind < indmin)
                indmin = ind;
        }
    }
    count = freqArray[indmin];
    index = indmin;
    for (int i=indmin+1; i<=indmax; i++)
    {
        if (freqArray[i] > count)
        {
            count = freqArray[i];
            index = i;
        }
    }
    int mediany = (index-(freqSize>>1)); // most frequent value

    // iteration to increase precision
    int meanvx = 0;
    int meanvy = 0;
    int num = 0;
    for ( int i=0; i < nBlkCount; i++ )
    {
        if (abs(vectors[i].x - medianx) < 6 && abs(vectors[i].y - mediany) < 6 )
        {
            meanvx += vectors[i].x;
            meanvy += vectors[i].y;
            num += 1;
        }
    }

    // output vectors must be doubled for next (finer) scale level
    if (num >0)
    {
        globalMVec->x = 2*meanvx / num;
        globalMVec->y = 2*meanvy / num;
    }
    else
    {
        globalMVec->x = 2*medianx;
        globalMVec->y = 2*mediany;
    }
}

void PlaneOfBlocks::SortBlocks()
{
	const int shift=ilog2(nBlkSizeX*nBlkSizeY*256/2048)+1;

    BLOCK_DEF bd;

  //  double div=log(1.5);
    for(int blky=0,blkIdx=0;blky<nBlkY;blky++)
        for(int blkx=0;blkx<nBlkX;blkx++,blkIdx++)
        {
            bd.x=blkx;
            bd.y=blky;
            int n = (vectors[blkIdx].sad>>shift);
//			int n=int(log(vectors[blkIdx].sad+1.0)/div);
//			int n=int(sqrt((float)vectors[blkIdx].sad));
            sortBuf[n].push_back(bd);
        }
    int pos=0;
    int sz=(int)sortBuf.size();
    for(int n=0;n<sz;n++)
    {
        std::vector<BLOCK_DEF>& vec=sortBuf[n];
        if(vec.size()==0) continue;

        memcpy(&sortedBlocks[pos],&vec[0],vec.size()*sizeof(BLOCK_DEF));
        pos+=(int)vec.size();

        vec.clear();
    }
}

//void PlaneOfBlocks::SortBlocks()
//{
//	for(std::vector<BLOCK_DEF>::iterator blk_ref=sortedBlocks.begin();blk_ref!=sortedBlocks.end();++blk_ref)
//		blk_ref->sad=vectors[blk_ref->y*nBlkX+blk_ref->x].sad;

//	std::sort(sortedBlocks.begin(),sortedBlocks.end(),BLOCK_DEF());
//}

static inline void _wb(std::vector<BLOCK_DEF>::iterator& it,int x,int y)
{ it->x=x; it->y=y; ++it; }

void PlaneOfBlocks::SetOrder(const PlaneOfBlocks* old)
{
    std::vector<BLOCK_DEF>::iterator blk_dst=sortedBlocks.begin();
    if(!old)
    {
        for(int blky=1;blky<nBlkY-1;blky++)
        {
            int blkScanDir = (blky%2 == 0) ? 1 : -1;
            int blkxStart = (blky%2 == 0) ? 0 : nBlkX-1;

            for(int iblkx=1;iblkx<nBlkX-1;iblkx++)
                _wb(blk_dst,blkxStart+iblkx*blkScanDir,blky);
        }
        for(int blkx=0;blkx<nBlkX;blkx++)
        {
            _wb(blk_dst,blkx,0);
            if(nBlkY-1>0) _wb(blk_dst,blkx,nBlkY-1);
        }
        for(int blky=1;blky<nBlkY-1;blky++)
        {
            _wb(blk_dst,0,blky);
            if(nBlkX-1>0) _wb(blk_dst,nBlkX-1,blky);
        }
    }
    else
    {
        int add_x=nBlkX-old->nBlkX*2;
        int add_y=nBlkY-old->nBlkY*2;

        std::vector<BLOCK_DEF> border(nBlkX*2+nBlkY*2-4);
        std::vector<BLOCK_DEF>::iterator brd_dst=border.begin();

        for(std::vector<BLOCK_DEF>::const_iterator blk_ref=old->sortedBlocks.begin();
                                            blk_ref!=old->sortedBlocks.end();++blk_ref)
        {
            if(blk_ref->x==old->nBlkX || blk_ref->y==old->nBlkY) continue;

            int x=blk_ref->x*2;
            int y=blk_ref->y*2;

            bool b=(x==0 || y==0 || blk_ref->x==old->nBlkX-1 || blk_ref->y==old->nBlkY-1);

            if(!b)
            {
                _wb(blk_dst,x,y);
                _wb(blk_dst,x+1,y);
                _wb(blk_dst,x+1,y+1);
                _wb(blk_dst,x,y+1);
            }
            else
            {
                _wb(x>0 && y>0 ? blk_dst:brd_dst,x,y);
                _wb(x+1<nBlkX-1 && y>0 ? blk_dst:brd_dst,x+1,y);
                _wb(x+1<nBlkX-1 && y+1<nBlkY-1 ? blk_dst:brd_dst,x+1,y+1);
                _wb(x>0 && y+1<nBlkY-1 ? blk_dst:brd_dst,x,y+1);

                bool last_x=(add_x && blk_ref->x==old->nBlkX-1);
                bool last_y=(add_y && blk_ref->y==old->nBlkY-1);
                if(last_x)
                    for(int i=0;i<add_x;i++)
                    {
                        _wb(i<add_x-1 && y>0 ? blk_dst:brd_dst,x+2+i,y);
                        _wb(i<add_x-1 && y+1<nBlkY-1 ? blk_dst:brd_dst,x+2+i,y+1);

                        if(last_y)
                            for(int k=0;k<add_y;k++)
                                _wb(i<add_x-1 && k<add_y-1 ? blk_dst:brd_dst,x+2+i,y+2+k);
                    }

                if(last_y)
                    for(int k=0;k<add_y;k++)
                    {
                        _wb(x>0 && k<add_y-1 ? blk_dst:brd_dst,x,y+2+k);
                        _wb(x+1<nBlkX-1 && k<add_y-1 ? blk_dst:brd_dst,x+1,y+2+k);
                    }
            }
        }
        std::copy(border.begin(),border.end(),blk_dst);
        blk_dst+=border.size();
    }

    if(nLevel>0)
    {
        for(int blkx=0;blkx<=nBlkX;blkx++)
            _wb(blk_dst,blkx,nBlkY);
        for(int blky=0;blky<nBlkY;blky++)
            _wb(blk_dst,nBlkX,blky);
    }
}
