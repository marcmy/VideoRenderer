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

enum MVPlaneSet
{
    YPLANE = 0,
    UPLANE,
    VPLANE
};

class MVPlane
{
    std::vector<uint8_t*> pPlane;
    int nWidth;
    int nHeight;
    int nPitch;
    int nPel;

    bool isRefined;

    bool hardCopy;

public:
    MVPlane(int nWidth, int nHeight, int nPel, bool hardCopy=true);
    ~MVPlane();

    void Update(const uint8_t* pSrc, int nPitch);
    void ChangePlane(const uint8_t *pNewPlane, int nNewPitch);
    void Refine(int interType);
    void ReduceTo(MVPlane *pReducedPlane, int rfilter);

    inline const uint8_t *GetAbsolutePointer(int nX, int nY) const
    {
        if (nPel == 1) return GetAbsolutePointerPel1(nX,nY);
        else if (nPel == 2)	return GetAbsolutePointerPel2(nX,nY);
        else return GetAbsolutePointerPel4(nX,nY);
    }

    inline const uint8_t *GetAbsolutePointerPel1(int nX, int nY) const
    { return pPlane[0] + nX + nY * nPitch;	}

    inline const uint8_t *GetAbsolutePointerPel2(int nX, int nY) const
    {
        int idx = (nX&1) | ((nY&1)<<1);

        nX >>= 1;
        nY >>= 1;

        return pPlane[idx] + nX + nY * nPitch;
    }

    inline const uint8_t *GetAbsolutePointerPel4(int nX, int nY) const
    {
        int idx = (nX&3) | ((nY&3)<<2);

        nX >>= 2;
        nY >>= 2;

        return pPlane[idx] + nX + nY*nPitch;
    }

    inline int GetPitch() const { return nPitch; }
    inline int GetWidth() const { return nWidth; }
    inline int GetHeight() const { return nHeight; }

    inline void ResetState() { isRefined = false; }
};

class MVFrame
{
    std::vector<MVPlane*> planes;

public:
    MVFrame(int nWidth, int nHeight, int nPel, bool hardCopy=true);
    ~MVFrame();

    void Update(uint8_t * pSrcY, int pitchY, uint8_t * pSrcU, uint8_t *pSrcV, int pitchUV);
    void ChangePlane(const uint8_t * pSrcY, int pitchY,const uint8_t * pSrcU,const uint8_t *pSrcV, int pitchUV);

    void Refine(int interType);
    void ReduceTo(MVFrame *pFrame, int rfilter);

    inline const MVPlane* GetPlane(MVPlaneSet nMode) const
    { return planes[nMode]; }
    inline MVPlane* GetPlaneW(MVPlaneSet nMode) const
    { return planes[nMode]; }
};

class MVGroupOfFrames
{
    MVFrame **pFrames;

    int nLevelCount;
    int nWidth;
    int nHeight;
    int nPel;
    bool needFinestLevel;

public:
    MVGroupOfFrames(int _nLevelCount, int nWidth, int nHeight, int nPel, bool needFinestLevel=true);
    ~MVGroupOfFrames();
    void Update(uint8_t *pSrcY, int pitchY, uint8_t *pSrcU, uint8_t *pSrcV, int pitchUV);
    void SetPlanes(const uint8_t * pSrcY, int pitchY, const uint8_t * pSrcU, const uint8_t *pSrcV, int pitchUV);

    MVFrame *GetFrame(int nLevel);

    void Refine(int interType);
    void Reduce(int rfilter);
};

