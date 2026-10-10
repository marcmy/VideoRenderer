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

#include "planeofblocks.h"

void PlaneOfBlocks::ExhaustiveSearch(int radius)
{
	int mvx = bestMV.x;
	int mvy = bestMV.y;

	const int minX = _fmax(mvx - radius, nDxMin), maxX = _fmin(mvx + radius, nDxMax - 1);
	const int minY = _fmax(mvy - radius, nDyMin), maxY = _fmin(mvy + radius, nDyMax - 1);

	if(nPel>1)
	{
		if(nPel==4) ExhaustiveSearch_pel4(minX,maxX,minY,maxY);
		else ExhaustiveSearch_pel2(minX,maxX,minY,maxY);
		return;
	}

	const uint8_t* srcY=pSrc[0],*srcU=pSrc[1],*srcV=pSrc[2];

	const uint8_t *refY0=refYPlane->GetAbsolutePointerPel1(x[0],y[0]);
	const uint8_t *refU0=refUPlane->GetAbsolutePointerPel1(x[1],y[1]);
	const uint8_t *refV0=refVPlane->GetAbsolutePointerPel1(x[2],y[2]);

	int vx2=-1;
	for(int vy=minY;vy<=maxY;vy++)
	{
		const uint8_t* refY=refY0+vy*nRefPitchY+minX;
		const uint8_t* refU=refU0+(vy>>1)*nRefPitchUV;
		const uint8_t* refV=refV0+(vy>>1)*nRefPitchUV;

		for(int vx=minX;vx<=maxX;vx++)
		{
			int cost = MotionDistorsion(vx, vy);
			if(cost >= nMinCost) continue;

			int sad = SAD[F_LUMA](srcY, nBlkSizeX, refY++, nRefPitchY);
			cost += sad + ((penaltyNew*sad)>>8);
			if(cost >= nMinCost) continue;

			vx2=(vx>>1);
			int saduv=(SAD[F_CHROMA](srcU, nBlkSize_UV, refU+vx2, nRefPitchUV)
					   + SAD[F_CHROMA](srcV, nBlkSize_UV, refV+vx2, nRefPitchUV))<<CHROMA_SHIFT;
			cost+=saduv+((penaltyNew*saduv)>>8);

			if(cost < nMinCost)
			{
				nMinCost = cost;
				bestMV.Init(vx,vy,sad+saduv);
			}
		}
	}
}

void PlaneOfBlocks::ExhaustiveSearch_pel2(int minX, int maxX, int minY, int maxY)
{
	const uint8_t* srcY=pSrc[0],*srcU=pSrc[1],*srcV=pSrc[2];

	int x0y=(x[0]<<1), y0y=(y[0]<<1);
	int x0u=(x[1]<<1), y0u=(y[1]<<1);
	int x0v=(x[2]<<1), y0v=(y[2]<<1);

	int vx2=-1,vy2=-1;
	for(int vy=minY;vy<=maxY;vy++)
	{
		vy2=(vy>>1);
		for(int vx=minX;vx<=maxX;vx++)
		{
			int cost = MotionDistorsion(vx, vy);
			if(cost >= nMinCost) continue;

			int sad = SAD[F_LUMA](srcY, nBlkSizeX, refYPlane->GetAbsolutePointerPel2(x0y+vx,y0y+vy),nRefPitchY);
			cost += sad + ((penaltyNew*sad)>>8);
			if(cost >= nMinCost) continue;

			vx2=(vx>>1);
			int saduv = (SAD[F_CHROMA](srcU, nBlkSize_UV, refUPlane->GetAbsolutePointerPel2(x0u+vx2,y0u+vy2), nRefPitchUV)
					 + SAD[F_CHROMA](srcV, nBlkSize_UV, refVPlane->GetAbsolutePointerPel2(x0v+vx2,y0v+vy2), nRefPitchUV))
					<<CHROMA_SHIFT;

			cost+=saduv+((penaltyNew*saduv)>>8);

			if(cost < nMinCost)
			{
				nMinCost = cost;
				bestMV.Init(vx,vy,sad+saduv);
			}
		}
	}
}

void PlaneOfBlocks::ExhaustiveSearch_pel4(int minX, int maxX, int minY, int maxY)
{
	const uint8_t* srcY=pSrc[0],*srcU=pSrc[1],*srcV=pSrc[2];

	int x0y=(x[0]<<2), y0y=(y[0]<<2);
	int x0u=(x[1]<<2), y0u=(y[1]<<2);
	int x0v=(x[2]<<2), y0v=(y[2]<<2);

	int vx2=-1,vy2=-1;
	for(int vy=minY;vy<=maxY;vy++)
	{
		vy2=(vy>>1);
		for(int vx=minX;vx<=maxX;vx++)
		{
			int cost = MotionDistorsion(vx, vy);
			if(cost >= nMinCost) continue;

			int sad = SAD[F_LUMA](srcY, nBlkSizeX, refYPlane->GetAbsolutePointerPel4(x0y+vx,y0y+vy),nRefPitchY);
			cost += sad + ((penaltyNew*sad)>>8);
			if(cost >= nMinCost) continue;

			vx2=(vx>>1);
			int saduv = (SAD[F_CHROMA](srcU, nBlkSize_UV, refUPlane->GetAbsolutePointerPel4(x0u+vx2,y0u+vy2), nRefPitchUV)
					 + SAD[F_CHROMA](srcV, nBlkSize_UV, refVPlane->GetAbsolutePointerPel4(x0v+vx2,y0v+vy2), nRefPitchUV))
					<<CHROMA_SHIFT;

			cost+=saduv+((penaltyNew*saduv)>>8);

			if(cost < nMinCost)
			{
				nMinCost = cost;
				bestMV.Init(vx,vy,sad+saduv);
			}
		}
	}
}

void PlaneOfBlocks::CheckMV(int vx, int vy, int *dir, int val)
{
	if(!IsVectorOK(vx,vy)) return;

	int cost=MotionDistorsion(vx, vy);
	if(cost >= nMinCost) return;

	int sad=SAD[F_LUMA](pSrc[0], nBlkSizeX, GetRefBlock(vx, vy), nRefPitchY);
	cost += sad+((penaltyNew*sad)>>8);
	if(cost >= nMinCost) return;

	int saduv=(SAD[F_CHROMA](pSrc[1], nBlkSize_UV, GetRefBlockU(vx, vy), nRefPitchUV)
			   + SAD[F_CHROMA](pSrc[2], nBlkSize_UV, GetRefBlockV(vx, vy), nRefPitchUV))
			<<CHROMA_SHIFT;
	cost += saduv+((penaltyNew*saduv)>>8);
	if(cost < nMinCost)
	{
		bestMV.Init(vx,vy,sad+saduv);
		nMinCost = cost;
		if(dir) *dir=val;
	}
}

void PlaneOfBlocks::ExpandingSearch(int r, int mvx, int mvy)
{
	for(int i=-r;i<=r;i++)
	{
		CheckMV(mvx + i, mvy - r);
		CheckMV(mvx + i, mvy + r);
		if(i!=-r && i!=r)
		{
			CheckMV(mvx - r, mvy + i);
			CheckMV(mvx + r, mvy + i);
		}
	}
}

/* (x-1)%6 */
static const int mod6m1[8] = {5,0,1,2,3,4,5,0};
/* radius 2 hexagon. repeated entries are to avoid having to compute mod6 every time. */
static const int hex2[8][2] = {{-1,-2}, {-2,0}, {-1,2}, {1,2}, {2,0}, {1,-2}, {-1,-2}, {-2,0}};

void PlaneOfBlocks::Hex2Search(int i_me_range)
{ //adopted from x264
	int dir = -2;
	int bmx = bestMV.x;
	int bmy = bestMV.y;

	if (i_me_range > 1)
	{
		/* hexagon */
		CheckMV(bmx-2, bmy, &dir, 0);
		CheckMV(bmx-1, bmy+2, &dir, 1);
		CheckMV(bmx+1, bmy+2, &dir, 2);
		CheckMV(bmx+2, bmy, &dir, 3);
		CheckMV(bmx+1, bmy-2, &dir, 4);
		CheckMV(bmx-1, bmy-2, &dir, 5);


		if( dir != -2 )
		{
			bmx += hex2[dir+1][0];
			bmy += hex2[dir+1][1];
			/* half hexagon, not overlapping the previous iteration */
			for( int i = 1; i < i_me_range/2 && IsVectorOK(bmx, bmy); i++ )
			{
				const int odir = mod6m1[dir+1];

				dir = -2;

				CheckMV(bmx + hex2[odir+0][0], bmy + hex2[odir+0][1], &dir, odir-1);
				CheckMV(bmx + hex2[odir+1][0], bmy + hex2[odir+1][1], &dir, odir);
				CheckMV(bmx + hex2[odir+2][0], bmy + hex2[odir+2][1], &dir, odir+1);
				if( dir == -2 )
					break;
				bmx += hex2[dir+1][0];
				bmy += hex2[dir+1][1];
			}
		}

		bestMV.x = bmx;
		bestMV.y = bmy;
	}
	/* square refine */
	ExhaustiveSearch(1);
}


void PlaneOfBlocks::CrossSearch(int start, int x_max, int y_max, int mvx, int mvy)
{ // part of umh  search

	for ( int i = start; i < x_max; i+=2 )
	{
		CheckMV(mvx - i, mvy);
		CheckMV(mvx + i, mvy);
	}

	for ( int j = start; j < y_max; j+=2 )
	{
		CheckMV(mvx, mvy - j);
		CheckMV(mvx, mvy + j);
	}
}


void PlaneOfBlocks::UMHSearch(int i_me_range, int omx, int omy) // radius
{
	// Uneven-cross Multi-Hexagon-grid Search (see x264)
	/* hexagon grid */

	// my mod: do not shift the center after Cross
	CrossSearch(1, i_me_range, i_me_range,  omx,  omy);

	int i = 1;
	do
	{
		static const int hex4[16][2] = {
			{-4, 2}, {-4, 1}, {-4, 0}, {-4,-1}, {-4,-2},
			{ 4,-2}, { 4,-1}, { 4, 0}, { 4, 1}, { 4, 2},
			{ 2, 3}, { 0, 4}, {-2, 3},
			{-2,-3}, { 0,-4}, { 2,-3},
		};

		for( int j = 0; j < 16; j++ )
		{
			int mx = omx + hex4[j][0]*i;
			int my = omy + hex4[j][1]*i;
			CheckMV( mx, my );
		}
	} while( ++i <= i_me_range/4 );

	Hex2Search(i_me_range);
}
