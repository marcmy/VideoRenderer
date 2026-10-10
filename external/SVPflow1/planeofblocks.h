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

#pragma once

#include "common.h"
#include "mvframe.h"
#include "x264_pixel.h"

struct BLOCK_DEF
{
    short int x;
    short int y;

    //int sad;
    //inline bool operator() (const BLOCK_DEF& b1,const BLOCK_DEF& b2) { return b1.sad<b2.sad;}
};

enum SearchType
{
//	LOGARITHMIC = 1,
    HEX2SEARCH = 2,
    UMHSEARCH,
    EXHAUSTIVE
};

struct VECTOR
{
    short x;
    short y;
    int sad;

    VECTOR() {x=0; y=0; sad=-1;}
    VECTOR(short x,short y,int sad) {Init(x,y,sad);}
    VECTOR(const VECTOR& src) {x=src.x; y=src.y; sad=src.sad;}

//	inline void Init(const short *src)
//		{x = src[0]; y = src[1]; sad = *((int*)(src+2));}
    inline void Init(short x,short y,int sad)
        {this->x=x; this->y=y; this->sad=sad;}

    VECTOR& operator=(const VECTOR& src)
        {x = src.x; y = src.y; sad = src.sad; return *this;}
};

class PlaneOfBlocks
{
public:
    std::vector<VECTOR> reverseVectors;

#ifdef _PRINT_RADIUS
    int _SUM,_C;
    int _AR[7];
#endif

protected:
    int nBlkX;                  /* width in number of blocks */
    int nBlkY;                  /* height in number of blocks */
    int nBlkSizeX;               /* size of a block */
    int nBlkSize_UV;
    int nBlkSizeY;               /* size of a block */
    int nOverlapX; // overlap size
    int nOverlapY; // overlap size

    int nBlkCount;              /* number of blocks in the plane */
    int nPel;                   /* pel refinement accuracy */
    int nLevel;              /* level number */
    int levelsCount;

    bool isHalfSized;

#define F_LUMA		0
#define F_CHROMA	1
#define F_LUMA_C	2
#define F_CHROMA_C	3

    x264_pixel_cmp_t SAD[4];
    copy_function_t BLIT[4];
    luma_function_t AVERAGE[4];
//	x264_pixel_cmp_t SSD;

    VECTOR *vectors;            /* motion vectors of the blocks */
    /* before the search, contains the hierachal predictor */
    /* after the search, contains the best motion vector */

    bool smallestPlane;         /* say whether vectors can used predictors from a smaller plane */

    const MVPlane *srcYPlane,*srcUPlane,*srcVPlane;
    const MVPlane *refYPlane,*refUPlane,*refVPlane;

    uint8_t *pSrc[3],*pSrcTmp[6];
    int nRefPitchY;
    int nRefPitchUV;

    VECTOR bestMV;              /* best vector found so far during the search */
    int nBestSad;               /* sad linked to the best vector */
    int nMinCost;               /* minimum cost ( sad + mv cost ) found so far */
    VECTOR predictor;           /* best predictor for the current vector */

    std::vector<VECTOR> predictors;   /* set of predictors for the current block */

    int nDxMin;                 /* minimum x coordinate for the vector */
    int nDyMin;                 /* minimum y coordinate for the vector */
    int nDxMax;                 /* maximum x corrdinate for the vector */
    int nDyMax;                 /* maximum y coordinate for the vector */
    int nDxMin0,nDyMin0,nDxMax0,nDyMax0;

    int x[3];                   /* absolute x coordinate of the origin of the block in the reference frame */
    int y[3];                   /* absolute y coordinate of the origin of the block in the reference frame */
    int x0[3],y0[3];

    int blkx;                   /* x coordinate in blocks */
    int blky;                   /* y coordinate in blocks */
    int blkIdx;                 /* index of the block */

    /* search parameters */

    SearchType searchType;      /* search type used */
    int nSearchParam;           /* additionnal parameter for this search */
    bool adaptiveRadius;

    int localLambda;                /* vector cost factor */
    int localSadLimit; // SAD limit for lambda using - Fizick
    int penaltyNew; // cost penalty factor for new candidates
    int penaltyZero; // cost penalty factor for zero vector
    int penaltyGlobal; // cost penalty factor for global predictor
    int penaltyNeighbour; // cost penalty factor for neighbour vectors
    int penaltyReverse;

    int localBadSAD; // SAD threshold for more wide search
    int badrange; // wide search radius
    bool tryMany; // try refine around many predictors

    int flags;
    uint8_t *pCmp,*pTmp;
    int blockLuma;

    VECTOR globalMVPredictor; // predictor of global motion vector

    int *freqArray; // temporary array for global motion estimaton
    int freqSize;// size of freqArray

    std::vector<BLOCK_DEF> sortedBlocks;
    std::vector<bool> processedBlocks;
    std::vector<std::vector<BLOCK_DEF> > sortBuf;
    std::vector<VECTOR> vectorsBorder;

    /* fetch the block in the reference frame, which is pointed by the vector (vx, vy) */
    inline const uint8_t *GetRefBlock(int nVx, int nVy)
    {
        return (nPel==2) ? refYPlane->GetAbsolutePointerPel2((x[0]<<1) + nVx, (y[0]<<1) + nVy) :
                           (nPel==1) ? refYPlane->GetAbsolutePointerPel1(x[0] + nVx, y[0] + nVy) :
                                       refYPlane->GetAbsolutePointerPel4((x[0]<<2) + nVx, (y[0]<<2) + nVy);
    }

    inline const uint8_t *GetRefBlockU(int nVx, int nVy)
    {
        return (nPel==2) ? refUPlane->GetAbsolutePointerPel2((x[1]<<1) + (nVx >> 1), (y[1]<<1) + (nVy>>1) ) :
                           (nPel==1) ? refUPlane->GetAbsolutePointerPel1(x[1] + (nVx >> 1), y[1] + (nVy>>1) ) :
                                       refUPlane->GetAbsolutePointerPel4((x[1]<<2) + (nVx >> 1), (y[1]<<2) + (nVy>>1) );
    }

    inline const uint8_t *GetRefBlockV(int nVx, int nVy)
    {
        return (nPel==2) ? refVPlane->GetAbsolutePointerPel2((x[2]<<1) + (nVx >> 1), (y[2]<<1) + (nVy>>1) ) :
                           (nPel==1) ? refVPlane->GetAbsolutePointerPel1(x[2] + (nVx >> 1), y[2] + (nVy>>1) ) :
                                       refVPlane->GetAbsolutePointerPel4((x[2]<<2) + (nVx >> 1), (y[2]<<2) + (nVy>>1) );
    }

    /* computes the cost of a vector (vx, vy) */
    inline int MotionDistorsion(int vx, int vy) const
    {
        int dx=predictor.x-vx, dy=predictor.y-vy;
        return (localLambda*(dx*dx+dy*dy)) >> 8;
    }

    inline int GetSAD(int x,int y)
    {
        return SAD[F_LUMA](pSrc[0], nBlkSizeX, GetRefBlock(x, y), nRefPitchY)
                + ((SAD[F_CHROMA](pSrc[1], nBlkSize_UV, GetRefBlockU(x, y), nRefPitchUV)
                    + SAD[F_CHROMA](pSrc[2], nBlkSize_UV, GetRefBlockV(x, y), nRefPitchUV))<<CHROMA_SHIFT);
    }

    /* check if the vector (vx, vy) is better than the best vector found so far */
    void CheckMV(int vx, int vy, int *dir=0, int val=0);

    bool MoveBlock(int vx,int vy,int margin=0);

    inline bool IsVectorOK(int vx,int vy)
    { return !(vx<nDxMin || vy<nDyMin || vx>=nDxMax || vy>=nDyMax); }

    void CopySrc(int n,int x0,int y0,int x1,int y1,int x2,int y2);
    void SetActiveSrc(int n);

public:
    PlaneOfBlocks(int _nBlkX, int _nBlkY, int _nBlkSizeX, int _nBlkSizeY, int _nOverlapX, int _nOverlapY,
                  int nPel, int nLevel, int levelsCount, bool smallestPlane,
                  bool useSATD);

    ~PlaneOfBlocks();

    /* compute the predictors from the upper plane */
    void InterpolatePrediction(const PlaneOfBlocks &pob, bool forRecalc=false);

    int GetArraySize() const { return nBlkCount*2; }

    void EstimateGlobalMVDoubled(VECTOR *globalMVDoubled); // Fizick

    void SortBlocks();
    void SetOrder(const PlaneOfBlocks* old=0);

    /* search the vectors for the whole plane */
    void SearchMVs(const MVFrame *pSrcFrame, const MVFrame *pRefFrame, SearchType st,
                   int stp, int lambda, int lSAD, int pnew, double plevel,
                   VECTOR *globalMVec, int pzero, int pglobal, int pnbour, int preverse, int badSAD, int badrange,
                   bool tryMany, int flags, int *lambdaOut, int *out);

    void RecalculateMVs(const MVFrame *pSrcFrame, const MVFrame *pRefFrame, SearchType st,
                        int stp, int _lambda, int _lSAD, int _pennew,
                        int thSAD, int flags, int *lambdaOut, int *out);

    void InitSADFunctions(bool satd);

protected:
    void SearchMVs_common(bool mode,const MVFrame *pSrcFrame, const MVFrame *pRefFrame,
                                  SearchType st, int stp, int lambda, int lsad,
                                  double plevel, int badSAD, int *lambdaOut, int *out);

    void SearchMVs_kernel(bool negativeOverlap=false);
    void RecalculateMVs_kernel(bool needLuma);

    bool CheckPredictor(const VECTOR& v,int penalty,bool absolute,VECTOR *pBest,int *pCost);
    int FetchPredictors();

    void Refine(bool blockMoved=false);

    void InitAsmFunctions(bool fullBlock=true);
    int GetContrast(int function,const uint8_t* src,int row,int size,uint8_t *buf,int *average=0);

    void ExhaustiveSearch(int radius);
    void ExhaustiveSearch_pel2(int minX, int maxX, int minY, int maxY);
    void ExhaustiveSearch_pel4(int minX, int maxX, int minY, int maxY);
    void ExpandingSearch(int radius, int mvx, int mvy);

    void Hex2Search(int i_me_range);
    void CrossSearch(int start, int x_max, int y_max, int mvx, int mvy);
    void UMHSearch(int i_me_range, int omx, int omy);
};

