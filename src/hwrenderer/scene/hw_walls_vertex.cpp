// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2006-2016 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//


#include "r_defs.h"
#include "hwrenderer/data/flatvertices.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_drawstructs.h"

EXTERN_CVAR(Bool, gl_seamless)

//==========================================================================
//
// Split upper edge of wall
//
//==========================================================================

void GLWall::SplitUpperEdge(FFlatVertex *&ptr)
{
	side_t *sidedef = seg->sidedef;
	float polyw = glseg.fracright - glseg.fracleft;
	float facu = (tcs[UPRGT].u - tcs[UPLFT].u) / polyw;
	float facv = (tcs[UPRGT].v - tcs[UPLFT].v) / polyw;
	float fact = (ztop[1] - ztop[0]) / polyw;
	float facc = (zceil[1] - zceil[0]) / polyw;
	float facf = (zfloor[1] - zfloor[0]) / polyw;

	for (int i = 0; i < sidedef->numsegs - 1; i++)
	{
		seg_t *cseg = sidedef->segs[i];
		float sidefrac = cseg->sidefrac;
		if (sidefrac <= glseg.fracleft) continue;
		if (sidefrac >= glseg.fracright) return;

		float fracfac = sidefrac - glseg.fracleft;

		ptr->x = cseg->v2->fX();
		ptr->y = cseg->v2->fY();
		ptr->z = ztop[0] + fact * fracfac;
		ptr->u = tcs[UPLFT].u + facu * fracfac;
		ptr->v = tcs[UPLFT].v + facv * fracfac;
		ptr++;
	}
}

//==========================================================================
//
// Split upper edge of wall
//
//==========================================================================

void GLWall::SplitLowerEdge(FFlatVertex *&ptr)
{
	side_t *sidedef = seg->sidedef;
	float polyw = glseg.fracright - glseg.fracleft;
	float facu = (tcs[LORGT].u - tcs[LOLFT].u) / polyw;
	float facv = (tcs[LORGT].v - tcs[LOLFT].v) / polyw;
	float facb = (zbottom[1] - zbottom[0]) / polyw;
	float facc = (zceil[1] - zceil[0]) / polyw;
	float facf = (zfloor[1] - zfloor[0]) / polyw;

	for (int i = sidedef->numsegs - 2; i >= 0; i--)
	{
		seg_t *cseg = sidedef->segs[i];
		float sidefrac = cseg->sidefrac;
		if (sidefrac >= glseg.fracright) continue;
		if (sidefrac <= glseg.fracleft) return;

		float fracfac = sidefrac - glseg.fracleft;

		ptr->x = cseg->v2->fX();
		ptr->y = cseg->v2->fY();
		ptr->z = zbottom[0] + facb * fracfac;
		ptr->u = tcs[LOLFT].u + facu * fracfac;
		ptr->v = tcs[LOLFT].v + facv * fracfac;
		ptr++;
	}
}

//==========================================================================
//
// Split left edge of wall
//
//==========================================================================

void GLWall::SplitLeftEdge(FFlatVertex *&ptr)
{
	if (vertexes[0] == NULL) return;

	vertex_t * vi = vertexes[0];

	if (vi->numheights)
	{
		int i = 0;

		float polyh1 = ztop[0] - zbottom[0];
		float factv1 = polyh1 ? (tcs[UPLFT].v - tcs[LOLFT].v) / polyh1 : 0;
		float factu1 = polyh1 ? (tcs[UPLFT].u - tcs[LOLFT].u) / polyh1 : 0;

		while (i<vi->numheights && vi->heightlist[i] <= zbottom[0]) i++;
		while (i<vi->numheights && vi->heightlist[i] < ztop[0])
		{
			ptr->x = glseg.x1;
			ptr->y = glseg.y1;
			ptr->z = vi->heightlist[i];
			ptr->u = factu1*(vi->heightlist[i] - ztop[0]) + tcs[UPLFT].u;
			ptr->v = factv1*(vi->heightlist[i] - ztop[0]) + tcs[UPLFT].v;
			ptr++;
			i++;
		}
	}
}

//==========================================================================
//
// Split right edge of wall
//
//==========================================================================

void GLWall::SplitRightEdge(FFlatVertex *&ptr)
{
	if (vertexes[1] == NULL) return;

	vertex_t * vi = vertexes[1];

	if (vi->numheights)
	{
		int i = vi->numheights - 1;

		float polyh2 = ztop[1] - zbottom[1];
		float factv2 = polyh2 ? (tcs[UPRGT].v - tcs[LORGT].v) / polyh2 : 0;
		float factu2 = polyh2 ? (tcs[UPRGT].u - tcs[LORGT].u) / polyh2 : 0;

		while (i>0 && vi->heightlist[i] >= ztop[1]) i--;
		while (i>0 && vi->heightlist[i] > zbottom[1])
		{
			ptr->x = glseg.x2;
			ptr->y = glseg.y2;
			ptr->z = vi->heightlist[i];
			ptr->u = factu2*(vi->heightlist[i] - ztop[1]) + tcs[UPRGT].u;
			ptr->v = factv2*(vi->heightlist[i] - ztop[1]) + tcs[UPRGT].v;
			ptr++;
			i--;
		}
	}
}

//==========================================================================
//
// Create vertices for one wall
//
//==========================================================================

void GLWall::CreateVertices(FFlatVertex *&ptr, bool split)
{
	ptr->Set(glseg.x1, zbottom[0], glseg.y1, tcs[LOLFT].u, tcs[LOLFT].v);
	ptr++;
	if (split && glseg.fracleft == 0) SplitLeftEdge(ptr);
	ptr->Set(glseg.x1, ztop[0], glseg.y1, tcs[UPLFT].u, tcs[UPLFT].v);
	ptr++;
	if (split && !(flags & GLWF_NOSPLITUPPER && seg->sidedef->numsegs > 1)) SplitUpperEdge(ptr);
	ptr->Set(glseg.x2, ztop[1], glseg.y2, tcs[UPRGT].u, tcs[UPRGT].v);
	ptr++;
	if (split && glseg.fracright == 1) SplitRightEdge(ptr);
	ptr->Set(glseg.x2, zbottom[1], glseg.y2, tcs[LORGT].u, tcs[LORGT].v);
	ptr++;
	if (split && !(flags & GLWF_NOSPLITLOWER) && seg->sidedef->numsegs > 1) SplitLowerEdge(ptr);
}

//==========================================================================
//
// 
//
//==========================================================================

int GLWall::CountVertices()
{
	int cnt = 4;
	vertex_t * vi = vertexes[0];
	if (glseg.fracleft == 0 && vi != nullptr && vi->numheights)
	{
		int i = 0;

		while (i<vi->numheights && vi->heightlist[i] <= zbottom[0]) i++;
		while (i<vi->numheights && vi->heightlist[i] < ztop[0])
		{
			i++;
			cnt++;
		}
	}
	auto sidedef = seg->sidedef;
	constexpr auto NOSPLIT_LOWER_UPPER = GLWF_NOSPLITLOWER | GLWF_NOSPLITUPPER;
	const bool canSplit = (flags & NOSPLIT_LOWER_UPPER) != NOSPLIT_LOWER_UPPER;
	if (canSplit && sidedef->numsegs > 1)
	{
		int cnt2 = 0;
		for (int i = sidedef->numsegs - 2; i >= 0; i--)
		{
			float sidefrac = sidedef->segs[i]->sidefrac;
			if (sidefrac >= glseg.fracright) continue;
			if (sidefrac <= glseg.fracleft) break;
			cnt2++;
		}
		const bool splitBoth = !(flags & NOSPLIT_LOWER_UPPER);
		if (splitBoth) cnt2 <<= 1;
		cnt += cnt2;
	}
	vi = vertexes[1];
	if (glseg.fracright == 1 && vi != nullptr && vi->numheights)
	{
		int i = 0;

		while (i<vi->numheights && vi->heightlist[i] <= zbottom[1]) i++;
		while (i<vi->numheights && vi->heightlist[i] < ztop[1])
		{
			i++;
			cnt++;
		}
	}
	return cnt;
}

//==========================================================================
//
// build the vertices for this wall
//
//==========================================================================

void GLWall::MakeVertices(HWDrawInfo *di, bool nosplit)
{
	if (vertcount == 0)
	{
		bool split = (gl_seamless && !nosplit && seg->sidedef != nullptr && !(seg->sidedef->Flags & WALLF_POLYOBJ) && !(flags & GLWF_NOSPLIT));
		vertcount = split ? CountVertices() : 4;

		auto ret = di->AllocVertices(vertcount);
		vertindex = ret.second;
		CreateVertices(ret.first, split);
	}
}

