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

#include "mvframe.h"
#include "mvframe_interpolation.h"
#include "pyramidlayout.h"


/******************************************************************************
*                                                                             *
*  MVPlane : manages a single plane, allowing refining			             *
*                                                                             *
******************************************************************************/

MVPlane::MVPlane(int nWidth, int nHeight, int nPel, bool hardCopy)
{
    ResetState();

    this->nWidth = nWidth;
    this->nHeight = nHeight;
    this->nPel = nPel;
    this->hardCopy=hardCopy;

    pPlane.resize(nPel*nPel);
}

MVPlane::~MVPlane()
{}

void MVPlane::Update(const uint8_t* pSrc, int nPitch)
{
    this->nPitch = nPitch;

    for(int i=0;i<nPel*nPel;i++)
        pPlane[i] = const_cast<uint8_t*>(pSrc + i*nPitch * nHeight);

    ResetState();
}

void MVPlane::ChangePlane(const uint8_t *pNewPlane,int nNewPitch)
{
    if(hardCopy)
        FrameInterpolation::Copy(pPlane[0], pNewPlane, nPitch, nNewPitch, nWidth, nHeight);
    else
    {
        pPlane[0]=const_cast<uint8_t*>(pNewPlane);
        nPitch=nNewPitch;
    }
}

void MVPlane::Refine(int sharp)
{
    if (nPel==2 /* && !isRefined*/)
    {
        if (sharp == 0) // bilinear
        {
            FrameInterpolation::HorizontalBilin(pPlane[1], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::VerticalBilin(pPlane[2], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::DiagonalBilin(pPlane[3], pPlane[0], nPitch, nWidth, nHeight);
        }
        else if(sharp==1) // bicubic
        {
            FrameInterpolation::HorizontalBicubic(pPlane[1], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::VerticalBicubic(pPlane[2], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::HorizontalBicubic(pPlane[3], pPlane[2], nPitch, nWidth, nHeight); // faster from ready-made horizontal
        }
        else // Wiener
        {
            FrameInterpolation::HorizontalWiener(pPlane[1], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::VerticalWiener(pPlane[2], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::HorizontalWiener(pPlane[3], pPlane[2], nPitch, nWidth, nHeight);// faster from ready-made horizontal
        }
    }
    else if (nPel==4 /*&& !isRefined*/) // firstly pel2 interpolation
    {
        if (sharp == 0) // bilinear
        {
            FrameInterpolation::HorizontalBilin(pPlane[2], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::VerticalBilin(pPlane[8], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::DiagonalBilin(pPlane[10], pPlane[0], nPitch, nWidth, nHeight);
        }
        else if(sharp==1) // bicubic
        {
            FrameInterpolation::HorizontalBicubic(pPlane[2], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::VerticalBicubic(pPlane[8], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::HorizontalBicubic(pPlane[10], pPlane[8], nPitch, nWidth, nHeight); // faster from ready-made horizontal
        }
        else // Wiener
        {
            FrameInterpolation::HorizontalWiener(pPlane[2], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::VerticalWiener(pPlane[8], pPlane[0], nPitch, nWidth, nHeight);
            FrameInterpolation::HorizontalWiener(pPlane[10], pPlane[8], nPitch,nWidth, nHeight);// faster from ready-made horizontal
        }
        // now interpolate intermediate
        FrameInterpolation::Average2(pPlane[1], pPlane[0], pPlane[2], nPitch, nWidth, nHeight);
        FrameInterpolation::Average2(pPlane[9], pPlane[8], pPlane[10], nPitch, nWidth, nHeight);
        FrameInterpolation::Average2(pPlane[4], pPlane[0], pPlane[8], nPitch, nWidth, nHeight);
        FrameInterpolation::Average2(pPlane[6], pPlane[2], pPlane[10], nPitch, nWidth, nHeight);
        FrameInterpolation::Average2(pPlane[5], pPlane[4], pPlane[6], nPitch, nWidth, nHeight);

        FrameInterpolation::Average2(pPlane[3], pPlane[0] + 1, pPlane[2], nPitch, nWidth - 1, nHeight);
        FrameInterpolation::Average2(pPlane[11], pPlane[8] + 1, pPlane[10], nPitch, nWidth - 1, nHeight);
        FrameInterpolation::Average2(pPlane[12], pPlane[0] + nPitch, pPlane[8], nPitch, nWidth, nHeight - 1);
        FrameInterpolation::Average2(pPlane[14], pPlane[2] + nPitch, pPlane[10], nPitch, nWidth, nHeight - 1);
        FrameInterpolation::Average2(pPlane[13], pPlane[12], pPlane[14], nPitch, nWidth, nHeight);
        FrameInterpolation::Average2(pPlane[7], pPlane[4] + 1, pPlane[6], nPitch, nWidth - 1, nHeight);
        FrameInterpolation::Average2(pPlane[15], pPlane[12] + 1, pPlane[14], nPitch, nWidth - 1, nHeight);
    }

    isRefined = true;
}

void MVPlane::ReduceTo(MVPlane *pReducedPlane, int rfilter)
{
//	if(pReducedPlane->isFilled) return;

    int dstPitch=(hardCopy ? pReducedPlane->nPitch : pReducedPlane->nPitch*2);

    switch(rfilter)
    {
    case 0: //removed "simple 4 pixels averaging like unfiltered SimpleResize"
    case 1:
        FrameInterpolation::RB2Filtered(pReducedPlane->pPlane[0], pPlane[0], dstPitch, nPitch, pReducedPlane->nWidth, pReducedPlane->nHeight);
        break;
    case 2:
        FrameInterpolation::RB2BilinearFiltered(pReducedPlane->pPlane[0], pPlane[0],dstPitch, nPitch, pReducedPlane->nWidth, pReducedPlane->nHeight);
        break;
    case 3:
        FrameInterpolation::RB2Quadratic(pReducedPlane->pPlane[0], pPlane[0], dstPitch, nPitch, pReducedPlane->nWidth, pReducedPlane->nHeight);
        break;
    case 4:
        FrameInterpolation::RB2Cubic(pReducedPlane->pPlane[0], pPlane[0], dstPitch, nPitch, pReducedPlane->nWidth, pReducedPlane->nHeight);
        break;
    }

    if(!hardCopy)
    {
        uint8_t	*dst=pReducedPlane->pPlane[0]+pReducedPlane->nPitch,
                *src=pReducedPlane->pPlane[0]+pReducedPlane->nPitch*2;
        for(int y=1;y<pReducedPlane->nHeight;y++)
        {
            memcpy(dst,src,pReducedPlane->nPitch);
            dst+=pReducedPlane->nPitch;
            src+=pReducedPlane->nPitch*2;
        }
    }
}

/******************************************************************************
*                                                                             *
*  MVFrame : a MVFrame is a threesome of MVPlane, some undefined, some        *
*  defined, according to the nMode value                                      *
*                                                                             *
******************************************************************************/
MVFrame::MVFrame(int nWidth, int nHeight, int nPel, bool hardCopy)
{
    int yRatioUV = 2;

    planes.push_back(new MVPlane(nWidth, nHeight, nPel, hardCopy));
    planes.push_back(new MVPlane(nWidth / 2, nHeight / yRatioUV, nPel, hardCopy));
    planes.push_back(new MVPlane(nWidth / 2, nHeight / yRatioUV, nPel, hardCopy));
}

void MVFrame::Update(uint8_t * pSrcY, int pitchY, uint8_t * pSrcU, uint8_t *pSrcV, int pitchUV)
{
    planes[0]->Update(pSrcY, pitchY);
    planes[1]->Update(pSrcU, pitchUV);
    planes[2]->Update(pSrcV, pitchUV);
}

MVFrame::~MVFrame()
{
    for(int i=0;i<3;i++)
        delete planes[i];
    planes.clear();
}

void MVFrame::ChangePlane(const uint8_t * pSrcY, int pitchY,const uint8_t * pSrcU,const uint8_t *pSrcV, int pitchUV)
{
    planes[0]->ChangePlane(pSrcY, pitchY);
    planes[1]->ChangePlane(pSrcU, pitchUV);
    planes[2]->ChangePlane(pSrcV, pitchUV);
}

void MVFrame::Refine(int sharp)
{
    for(int i=0;i<3;i++)
        planes[i]->Refine(sharp);
}

void MVFrame::ReduceTo(MVFrame *pFrame, int rfilter)
{
    for(int i=0;i<3;i++)
        planes[i]->ReduceTo(pFrame->GetPlaneW((MVPlaneSet)i), rfilter);
}

/******************************************************************************
*                                                                             *
*  MVGroupOfFrames : manage a hierachal frame structure                       *
*                                                                             *
******************************************************************************/

MVGroupOfFrames::MVGroupOfFrames(int nLevelCount, int nWidth, int nHeight, int nPel, bool needFinestLevel)
{
    this->nLevelCount=nLevelCount;
    this->nWidth=nWidth;
    this->nHeight=nHeight;
    this->nPel=nPel;
    this->needFinestLevel=needFinestLevel;

    pFrames = new MVFrame *[nLevelCount];
    pFrames[0] = new MVFrame(nWidth, nHeight, nPel, needFinestLevel);
    for ( int i = 1; i < nLevelCount; i++ )
    {
        int nWidthi = SvpPyramidLayout::planeSize(nWidth, i);
        int nHeighti = SvpPyramidLayout::planeSize(nHeight, i);
        pFrames[i] = new MVFrame(nWidthi, nHeighti, 1);
    }
}

void MVGroupOfFrames::Update(uint8_t * pSrcY, int pitchY, uint8_t * pSrcU, uint8_t *pSrcV, int pitchUV) // v2.0
{
    for(int i=(needFinestLevel?0:1); i<nLevelCount; i++)
    {
        unsigned int offY = SvpPyramidLayout::planeSuperOffset(false,nHeight, i, nPel, pitchY, needFinestLevel);
        unsigned int offUV = SvpPyramidLayout::planeSuperOffset(true,nHeight/2, i, nPel, pitchUV, needFinestLevel);
        pFrames[i]->Update(pSrcY+offY, pitchY, pSrcU+offUV, pSrcV+offUV, pitchUV);
    }
}

MVGroupOfFrames::~MVGroupOfFrames()
{
    for(int i=0; i<nLevelCount; i++)
        delete pFrames[i];

    delete[] pFrames;
}

MVFrame *MVGroupOfFrames::GetFrame(int nLevel)
{
    if (( nLevel < 0 ) || ( nLevel >= nLevelCount )) return 0;
    return pFrames[nLevel];
}

void MVGroupOfFrames::SetPlanes(const uint8_t * pSrcY, int pitchY,const uint8_t * pSrcU,const uint8_t *pSrcV, int pitchUV)
{
    pFrames[0]->ChangePlane(pSrcY,pitchY,pSrcU,pSrcV,pitchUV);
}

void MVGroupOfFrames::Refine(int sharp)
{
    pFrames[0]->Refine(sharp);
}

void MVGroupOfFrames::Reduce(int rfilter)
{
    for (int i = 0; i < nLevelCount - 1; i++ )
        pFrames[i]->ReduceTo(pFrames[i+1], rfilter);
}

