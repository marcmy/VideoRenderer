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

#include "planeofblocks.h"

#define MVANALYSIS_DATA_VERSION 0xA0

class SVAnalysisData
{
public:
    int nVersion;
    int vectorsType; //VECTORS_FORWARD | VECTORS_BACWARD
    int nBlkSizeX; // horizontal block size
    int nBlkSizeY; // vertical block size - v1.7
    int nPel; //pixel refinement of the motion estimation
    int nLvCount; //number of level for the hierarchal search
    int nFlags; //diverse flags to set up the search
    int nWidth; //Width of the frame
    int nHeight; //Height of the frame
    int nOverlapX; // overlap block size - v1.1
    int nOverlapY; // vertical overlap - v1.7
    int nBlkX; // number of blocks along X
    int nBlkY; // number of blocks along Y
    int gpuMode;
    int delta;

public:
    SVAnalysisData()
    {
        memset(this, 0, sizeof(SVAnalysisData));
        nVersion = MVANALYSIS_DATA_VERSION;
    }
};

class GroupOfPlanes
{
    SVAnalysisData ad;
    SearchType searchType;
    int searchParam;
    int nLambda;
    int lsad;
    int pnew;
    int thSAD;
    int flags;

    int minLevel;
    int fullMathLevel;

    std::vector<PlaneOfBlocks*> planes1;
    std::vector<PlaneOfBlocks*> planes2;

public:
    //used by changeParams() only - for SVP's RemoteControl feature
    struct PARAMS_CHANGE
    {
        int vectorsType;
        int searchType,nSearchParam;
        int nLambda,lsad,pnew;
        int thSAD;
        int satd,satdCoarse;
        int flags;

        PARAMS_CHANGE() {vectorsType=searchType=nSearchParam=nLambda=lsad=pnew=
                    thSAD=satd=satdCoarse=flags=IGNORE_VALUE;}
    };

    struct InitData
    {
        SVAnalysisData ad;
        SearchType searchType;
        int nSearchParam;
        int levelsSkipped;
        int nLambda;
        int lsad;
        int pnew;
        int satd;
        int thSAD;
        int fullMathWidth;
        int flags;

        void changeParams(const PARAMS_CHANGE& pc);
    };

public:
    GroupOfPlanes(const InitData& d);
    ~GroupOfPlanes();

    void SearchMVs(MVGroupOfFrames *pFrame1GOF, MVGroupOfFrames *pFrame2GOF,
                   SearchType pelSearchType, int pelSearchParam, double plevel,
                   int pzero, int pglobal, int pnbour, int preverse, int badrange, bool tryMany,bool sorting,
                   int *lambdaOut, int *out1, int *out2);

    void RecalculateMVs(const GroupOfPlanes* vectors, MVGroupOfFrames *pFrame1GOF, MVGroupOfFrames *pFrame2GOF,
                        int *lambdaOut,int *out1, int *out2);

    int GetArraySize() const {return 1 + planes1[minLevel]->GetArraySize();}
};

