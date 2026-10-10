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
struct SvpPyramidLayout {
    static int planeSize(int src_size, int level);
    static unsigned planeSuperOffset(bool chroma, int src_height, int level, int pel, int plane_pitch, bool hasFinest);
};
inline int SvpPyramidLayout::planeSize(int src_size, int level)
{
    int sz = src_size;

    for (int i=1; i<=level; i++)
        sz = ((sz/2) / 2) * 2;
    return sz;
}

unsigned inline int SvpPyramidLayout::planeSuperOffset(bool chroma, int src_height, int level, int pel, int plane_pitch, bool hasFinestLevel)
{
    int height = src_height;
    unsigned int offset;

    if (level==0)
        offset = 0;
    else
    {
        offset = (hasFinestLevel ? pel*pel*plane_pitch*src_height : 0);

        for (int i=1; i<level; i++)
        {
            height = planeSize(src_height*(chroma?2:1), i);
            if(chroma) height/=2;

            offset += plane_pitch*height;
        }
    }
    return offset;
}