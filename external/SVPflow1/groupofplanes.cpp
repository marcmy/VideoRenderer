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

#include "groupofplanes.h"

GroupOfPlanes::GroupOfPlanes(const GroupOfPlanes::InitData &d)
{
    ad = d.ad;
    minLevel = (ad.gpuMode==2 ? 1:0);

    searchType=d.searchType;
    searchParam=d.nSearchParam;
    nLambda=d.nLambda;
    lsad=d.lsad;
    pnew=d.pnew;
    thSAD=d.thSAD;
    flags=d.flags;

    int nWidth_B = (ad.nBlkSizeX - ad.nOverlapX)*ad.nBlkX + ad.nOverlapX;
    int nHeight_B = (ad.nBlkSizeY - ad.nOverlapY)*ad.nBlkY + ad.nOverlapY;

    if(minLevel>0)
    {
        planes1.push_back(0);
        planes2.push_back(0);
    }

    fullMathLevel = 1;//(fullMathWidth>0 ? minLevel : 1);
    for(int i = minLevel; i < ad.nLvCount; i++ )
    {
        ad.nBlkX = ((nWidth_B>>i) - ad.nOverlapX)/(ad.nBlkSizeX-ad.nOverlapX);
        ad.nBlkY = ((nHeight_B>>i) - ad.nOverlapY)/(ad.nBlkSizeY-ad.nOverlapY);

        if (d.fullMathWidth>0 && (nWidth_B >> i)>=d.fullMathWidth)
            fullMathLevel = i+1;

        for(int k=0;k<2;k++)
        {
            std::vector<PlaneOfBlocks*>& planes=(k==0?planes1:planes2);
            planes.push_back(new PlaneOfBlocks(ad.nBlkX, ad.nBlkY, ad.nBlkSizeX, ad.nBlkSizeY, ad.nOverlapX, ad.nOverlapY,
                                            i==0 ? ad.nPel : 1, i, d.levelsSkipped+ad.nLvCount, i==ad.nLvCount-1,
                                            (i < fullMathLevel && (d.satd & 1)) || (i >= fullMathLevel && (d.satd & 2))));
        }
    }
}

GroupOfPlanes::~GroupOfPlanes()
{
    for(int i=0;i<(int)planes1.size();i++)
    {
        if(planes1[i]) delete planes1[i];
        if(planes2[i]) delete planes2[i];
    }
}

void GroupOfPlanes::InitData::changeParams(const GroupOfPlanes::PARAMS_CHANGE& pc)
{
    if(pc.vectorsType!=IGNORE_VALUE) ad.vectorsType=pc.vectorsType;
    if(pc.searchType!=IGNORE_VALUE) searchType=(SearchType)pc.searchType;
    if(pc.nSearchParam!=IGNORE_VALUE) nSearchParam=pc.nSearchParam;
    if(pc.nLambda!=IGNORE_VALUE) nLambda=pc.nLambda;
    if(pc.lsad!=IGNORE_VALUE) lsad=pc.lsad;
    if(pc.pnew!=IGNORE_VALUE) pnew=pc.pnew;
    if(pc.thSAD!=IGNORE_VALUE) thSAD=pc.thSAD;
    if(pc.satd!=IGNORE_VALUE) satd = pc.satd + (satd&2);
    if(pc.satdCoarse!=IGNORE_VALUE) satd = (satd&1) + pc.satdCoarse*2;
    if(pc.flags!=IGNORE_VALUE) flags=pc.flags;
}
void GroupOfPlanes::SearchMVs(MVGroupOfFrames *pFrame1GOF, MVGroupOfFrames *pFrame2GOF,
                              SearchType pelSearchType,int pelSearchParam,
                              double plevel,int pzero, int pglobal, int pnbour, int preverse, int badrange,
                              bool tryMany, bool sorting, int *lambdaOut, int *out1, int *out2)
{
    if(out1) *(out1++) = GetArraySize();
    if(out2) *(out2++) = GetArraySize();

    VECTOR globalMV1, globalMV2; // create and init global motion vector as zero
    // Search the motion vectors, for the low details interpolations first
    planes1[ad.nLvCount - 1]->SearchMVs(pFrame1GOF->GetFrame(ad.nLvCount-1),
                                       pFrame2GOF->GetFrame(ad.nLvCount-1),
                                       searchType, searchParam, nLambda, lsad, pnew, plevel,
                                       &globalMV1, pzero, pglobal, pnbour, preverse, thSAD, badrange,tryMany,
                                       flags, 0,0);

    if (ad.vectorsType == 3)
        planes2[ad.nLvCount - 1]->SearchMVs(pFrame2GOF->GetFrame(ad.nLvCount-1),
                                           pFrame1GOF->GetFrame(ad.nLvCount-1),
                                           searchType, searchParam, nLambda, lsad, pnew, plevel,
                                           &globalMV2, pzero, pglobal, pnbour, preverse, thSAD, badrange,tryMany,
                                           flags, 0,0);

    // Refining the search until we reach the highest detail interpolation.
    for(int i = ad.nLvCount - 2; i >= minLevel; i-- )
    {
        SearchType searchTypeLevel = (i<fullMathLevel) ? pelSearchType : searchType; // full search for coarse planes
        int searchParamLevel = (i<fullMathLevel) ? pelSearchParam : searchParam; // special case for finest level
        bool tryManyLevel = tryMany && i >= fullMathLevel+1;
        bool needSort = sorting && i >= fullMathLevel;

        if (needSort)
        {
            planes1[i+1]->SortBlocks();
            if(ad.vectorsType==3)
                planes2[i+1]->SortBlocks();
        }

        planes1[i]->SetOrder(needSort ? planes1[i + 1] : 0);
        if(ad.vectorsType==3)
            planes2[i]->SetOrder(needSort ? planes2[i + 1] : 0);

        planes1[i + 1]->EstimateGlobalMVDoubled(&globalMV1); // get updated global MV (doubled)
        if(ad.vectorsType==3)
        {
            planes2[i+1]->EstimateGlobalMVDoubled(&globalMV2); // get updated global MV (doubled)

            int vx=(globalMV1.x+globalMV2.x)/2;
            int vy=(globalMV1.y+globalMV2.y)/2;
            globalMV1.x -= vx; globalMV1.y -= vy;
            globalMV2.x -= vx; globalMV1.y -= vy;
        }

        planes1[i]->InterpolatePrediction(*(planes1[i + 1]));
        if(ad.vectorsType==3)
        {
            planes2[i]->InterpolatePrediction(*(planes2[i+1]));

            std::swap(planes1[i]->reverseVectors,planes2[i]->reverseVectors);
        }

        planes1[i]->SearchMVs(pFrame1GOF->GetFrame(i), pFrame2GOF->GetFrame(i),
                         searchTypeLevel, searchParamLevel, nLambda, lsad, pnew, plevel,
                         &globalMV1, pzero, pglobal, pnbour, preverse, thSAD, badrange, tryManyLevel,
                         flags,lambdaOut,i==minLevel ? out1 : 0);

        if (ad.vectorsType == 3)
            planes2[i]->SearchMVs(pFrame2GOF->GetFrame(i), pFrame1GOF->GetFrame(i),
                             searchTypeLevel, searchParamLevel, nLambda, lsad, pnew, plevel,
                             &globalMV2, pzero, pglobal, pnbour, preverse, thSAD, badrange, tryManyLevel,
                             flags,0,i==minLevel ? out2 : 0);
    }
}

void GroupOfPlanes::RecalculateMVs(const GroupOfPlanes* vectors, MVGroupOfFrames *pFrame1GOF,
                                   MVGroupOfFrames *pFrame2GOF,int *lambdaOut,int *out1, int *out2)
{
    if(out1) *(out1++) = GetArraySize();
    if(out2) *(out2++) = GetArraySize();

    bool divide=!(ad.nBlkSizeX==vectors->ad.nBlkSizeX && ad.nBlkSizeY==vectors->ad.nBlkSizeY);
    nLambda=*lambdaOut;

    if(divide)
    {
        planes1[0]->InterpolatePrediction(*(vectors->planes1[0]),true);
        if(ad.vectorsType==3)
            planes2[0]->InterpolatePrediction(*(vectors->planes2[0]),true);

        planes1[0]->SetOrder();
        if(ad.vectorsType==3)
            planes2[0]->SetOrder();

        planes1[0]->RecalculateMVs(pFrame1GOF->GetFrame(0),pFrame2GOF->GetFrame(0),
                              searchType, searchParam, nLambda, lsad, pnew,
                              thSAD,flags,lambdaOut,out1);
        if(ad.vectorsType==3)
            planes2[0]->RecalculateMVs(pFrame2GOF->GetFrame(0),pFrame1GOF->GetFrame(0),
                                  searchType, searchParam, nLambda, lsad, pnew,
                                  thSAD,flags,0,out2);
    }
}
