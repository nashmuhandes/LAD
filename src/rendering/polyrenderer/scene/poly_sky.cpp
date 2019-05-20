/*
**  Sky dome rendering
**  Copyright(C) 2003-2016 Christoph Oelckers
**  All rights reserved.
**
**  This program is free software: you can redistribute it and/or modify
**  it under the terms of the GNU Lesser General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  This program is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU Lesser General Public License for more details.
**
**  You should have received a copy of the GNU Lesser General Public License
**  along with this program.  If not, see http:**www.gnu.org/licenses/
**
**  Loosely based on the JDoom sky and the ZDoomGL 0.66.2 sky.
*/

#include <stdlib.h>
#include "templates.h"
#include "doomdef.h"
#include "sbar.h"
#include "r_data/r_translate.h"
#include "poly_sky.h"
#include "poly_portal.h"
#include "r_sky.h" // for skyflatnum
#include "g_levellocals.h"
#include "polyrenderer/scene/poly_light.h"

EXTERN_CVAR(Float, skyoffset)
EXTERN_CVAR(Int, r_skymode)

PolySkyDome::PolySkyDome()
{
	CreateDome();
}

void PolySkyDome::Render(PolyRenderThread *thread, const Mat4f &worldToView, const Mat4f &worldToClip)
{
#ifdef USE_GL_DOME_MATH
	Mat4f modelMatrix = GLSkyMath();
#else
	Mat4f modelMatrix = Mat4f::Identity();

	PolySkySetup frameSetup;
	frameSetup.Update();

	if (frameSetup != mCurrentSetup)
	{
		// frontcyl = pixels for full 360 degrees, front texture
		// backcyl = pixels for full 360 degrees, back texture
		// skymid = Y scaled pixel offset
		// sky1pos = unscaled X offset, front
		// sky2pos = unscaled X offset, back
		// frontpos = scaled X pixel offset (fixed point)
		// backpos = scaled X pixel offset (fixed point)
		// skyflip = flip X direction

		float scaleBaseV = 1.42f;
		float offsetBaseV = 0.25f;

		float scaleFrontU = frameSetup.frontcyl / (float)frameSetup.frontskytex->GetWidth();
		float scaleFrontV = (float)frameSetup.frontskytex->GetScale().Y * scaleBaseV;
		float offsetFrontU = (float)((frameSetup.frontpos / 65536.0 + frameSetup.frontcyl / 2) / frameSetup.frontskytex->GetWidth());
		float offsetFrontV = (float)((frameSetup.skymid / frameSetup.frontskytex->GetHeight() + offsetBaseV) * scaleBaseV);

		unsigned int count = mVertices.Size();
		for (unsigned int i = 0; i < count; i++)
		{
			mVertices[i].u = offsetFrontU + mInitialUV[i].X * scaleFrontU;
			mVertices[i].v = offsetFrontV + mInitialUV[i].Y * scaleFrontV;
		}

		mCurrentSetup = frameSetup;
	}
#endif

	const auto &viewpoint = PolyRenderer::Instance()->Viewpoint;
	Mat4f objectToWorld = Mat4f::Translate((float)viewpoint.Pos.X, (float)viewpoint.Pos.Y, (float)viewpoint.Pos.Z) * modelMatrix;

	int rc = mRows + 1;

	PolyTriangleDrawer::SetTransform(thread->DrawQueue, thread->FrameMemory->NewObject<Mat4f>(worldToClip * objectToWorld), nullptr);

	PolyDrawArgs args;
	args.SetLight(&NormalLight, 255, PolyRenderer::Instance()->Light.WallGlobVis(false), true);
	args.SetStencilTestValue(255);
	args.SetWriteStencil(true, 1);
	args.SetClipPlane(0, PolyClipPlane(0.0f, 0.0f, 0.0f, 1.0f));

	RenderCapColorRow(thread, args, mCurrentSetup.frontskytex, 0, false);
	RenderCapColorRow(thread, args, mCurrentSetup.frontskytex, rc, true);

	args.SetTexture(mCurrentSetup.frontskytex, DefaultRenderStyle());

	uint32_t topcapcolor = mCurrentSetup.frontskytex->GetSkyCapColor(false);
	uint32_t bottomcapcolor = mCurrentSetup.frontskytex->GetSkyCapColor(true);
	uint8_t topcapindex = RGB256k.All[((RPART(topcapcolor) >> 2) << 12) | ((GPART(topcapcolor) >> 2) << 6) | (BPART(topcapcolor) >> 2)];
	uint8_t bottomcapindex = RGB256k.All[((RPART(bottomcapcolor) >> 2) << 12) | ((GPART(bottomcapcolor) >> 2) << 6) | (BPART(bottomcapcolor) >> 2)];

	for (int i = 1; i <= mRows; i++)
	{
		RenderRow(thread, args, i, topcapcolor, topcapindex);
		RenderRow(thread, args, rc + i, bottomcapcolor, bottomcapindex);
	}
}

void PolySkyDome::RenderRow(PolyRenderThread *thread, PolyDrawArgs &args, int row, uint32_t capcolor, uint8_t capcolorindex)
{
	args.SetColor(capcolor, capcolorindex);
	args.SetStyle(TriBlendMode::Skycap);
	PolyTriangleDrawer::DrawArray(thread->DrawQueue, args, &mVertices[mPrimStart[row]], mPrimStart[row + 1] - mPrimStart[row], PolyDrawMode::TriangleStrip);
}

void PolySkyDome::RenderCapColorRow(PolyRenderThread *thread, PolyDrawArgs &args, FSoftwareTexture *skytex, int row, bool bottomCap)
{
	uint32_t solid = skytex->GetSkyCapColor(bottomCap);
	uint8_t palsolid = RGB32k.RGB[(RPART(solid) >> 3)][(GPART(solid) >> 3)][(BPART(solid) >> 3)];

	args.SetColor(solid, palsolid);
	args.SetStyle(TriBlendMode::Fill);
	PolyTriangleDrawer::DrawArray(thread->DrawQueue, args, &mVertices[mPrimStart[row]], mPrimStart[row + 1] - mPrimStart[row], PolyDrawMode::TriangleFan);
}

void PolySkyDome::CreateDome()
{
	mColumns = 16;// 128;
	mRows = 4;
	CreateSkyHemisphere(false);
	CreateSkyHemisphere(true);
	mPrimStart.Push(mVertices.Size());
}

void PolySkyDome::CreateSkyHemisphere(bool zflip)
{
	int r, c;

	mPrimStart.Push(mVertices.Size());

	for (c = 0; c < mColumns; c++)
	{
		SkyVertex(1, zflip ? c : (mColumns - 1 - c), zflip);
	}

	// The total number of triangles per hemisphere can be calculated
	// as follows: rows * columns * 2 + 2 (for the top cap).
	for (r = 0; r < mRows; r++)
	{
		mPrimStart.Push(mVertices.Size());
		for (c = 0; c <= mColumns; c++)
		{
			SkyVertex(r + 1 - zflip, c, zflip);
			SkyVertex(r + zflip, c, zflip);
		}
	}
}

TriVertex PolySkyDome::SetVertexXYZ(float xx, float yy, float zz, float uu, float vv)
{
	TriVertex v;
	v.x = xx;
	v.y = zz;
	v.z = yy;
	v.w = 1.0f;
	v.u = uu;
	v.v = vv;
	return v;
}

void PolySkyDome::SkyVertex(int r, int c, bool zflip)
{
	static const FAngle maxSideAngle = 60.f;
	static const float scale = 10000.;

	FAngle topAngle = (c / (float)mColumns * 360.f);
	FAngle sideAngle = maxSideAngle * (float)(mRows - r) / (float)mRows;
	float height = sideAngle.Sin();
	float realRadius = scale * sideAngle.Cos();
	FVector2 pos = topAngle.ToVector(realRadius);
	float z = (!zflip) ? scale * height : -scale * height;

	float u, v;

	// And the texture coordinates.
	if (!zflip)	// Flipped Y is for the lower hemisphere.
	{
		u = (-c / (float)mColumns);
		v = (r / (float)mRows);
	}
	else
	{
		u = (-c / (float)mColumns);
		v = 1.0f + ((mRows - r) / (float)mRows);
	}

	if (r != 4) z += 300;

	// And finally the vertex.
	TriVertex vert;
	vert = SetVertexXYZ(-pos.X, z - 1.f, pos.Y, u, v - 0.5f);
	mVertices.Push(vert);
	mInitialUV.Push({ vert.u, vert.v });
}

Mat4f PolySkyDome::GLSkyMath()
{
	PolySkySetup frameSetup;
	frameSetup.Update();
	mCurrentSetup = frameSetup;

	float x_offset = 0.0f;
	float y_offset = 0.0f;
	bool mirror = false;
	FSoftwareTexture *tex = mCurrentSetup.frontskytex;

	int texh = 0;
	int texw = 0;

	Mat4f modelMatrix = Mat4f::Identity();
	if (tex)
	{
		texw = tex->GetWidth();
		texh = tex->GetHeight();

		modelMatrix = Mat4f::Rotate(-180.0f + x_offset, 0.f, 0.f, 1.f);

		float xscale = texw < 1024.f ? floor(1024.f / float(texw)) : 1.f;
		float yscale = 1.f;
		if (texh <= 128 && (PolyRenderer::Instance()->Level->flags & LEVEL_FORCETILEDSKY))
		{
			modelMatrix = modelMatrix * Mat4f::Translate(0.f, 0.f, (-40 + tex->GetSkyOffset() + skyoffset)*skyoffsetfactor);
			modelMatrix = modelMatrix * Mat4f::Scale(1.f, 1.f, 1.2f * 1.17f);
			yscale = 240.f / texh;
		}
		else if (texh < 128)
		{
			// smaller sky textures must be tiled. We restrict it to 128 sky pixels, though
			modelMatrix = modelMatrix * Mat4f::Translate(0.f, 0.f, -1250.f);
			modelMatrix = modelMatrix * Mat4f::Scale(1.f, 1.f, 128 / 230.f);
			yscale = (float)(128 / texh); // intentionally left as integer.
		}
		else if (texh < 200)
		{
			modelMatrix = modelMatrix * Mat4f::Translate(0.f, 0.f, -1250.f);
			modelMatrix = modelMatrix * Mat4f::Scale(1.f, 1.f, texh / 230.f);
		}
		else if (texh <= 240)
		{
			modelMatrix = modelMatrix * Mat4f::Translate(0.f, 0.f, (200 - texh + tex->GetSkyOffset() + skyoffset)*skyoffsetfactor);
			modelMatrix = modelMatrix * Mat4f::Scale(1.f, 1.f, 1.f + ((texh - 200.f) / 200.f) * 1.17f);
		}
		else
		{
			modelMatrix = modelMatrix * Mat4f::Translate(0.f, 0.f, (-40 + tex->GetSkyOffset() + skyoffset)*skyoffsetfactor);
			modelMatrix = modelMatrix * Mat4f::Scale(1.f, 1.f, 1.2f * 1.17f);
			yscale = 240.f / texh;
		}

		float offsetU = 1.0f;
		float offsetV = y_offset / texh;
		float scaleU = mirror ? -xscale : xscale;
		float scaleV = yscale;

		unsigned int count = mVertices.Size();
		for (unsigned int i = 0; i < count; i++)
		{
			mVertices[i].u = offsetU + mInitialUV[i].X * scaleU;
			mVertices[i].v = offsetV + mInitialUV[i].Y * scaleV;
		}
	}

	return modelMatrix;
}

/////////////////////////////////////////////////////////////////////////////

static FSoftwareTexture *GetSWTex(FTextureID texid, bool allownull = true)
{
	auto tex = TexMan.GetPalettedTexture(texid, true);
	if (tex == nullptr) return nullptr;
	if (!allownull && !tex->isValid()) return nullptr;
	return tex->GetSoftwareTexture();
}

void PolySkySetup::Update()
{
	double skytexturemid = 0.0;
	double skyscale = 0.0;
	float skyiscale = 0.0f;
	fixed_t sky1cyl = 0, sky2cyl = 0;
	auto Level = PolyRenderer::Instance()->Level;

	auto skytex1 = TexMan.GetPalettedTexture(Level->skytexture1, true);
	auto skytex2 = TexMan.GetPalettedTexture(Level->skytexture2, true);

	if (skytex1)
	{
		FSoftwareTexture *sskytex1 = skytex1->GetSoftwareTexture();
		FSoftwareTexture *sskytex2 = skytex2->GetSoftwareTexture();
		skytexturemid = 0;
		int skyheight = skytex1->GetDisplayHeight();
		if (skyheight >= 128 && skyheight < 200)
		{
			skytexturemid = -28;
		}
		else if (skyheight > 200)
		{
			skytexturemid = (200 - skyheight) * sskytex1->GetScale().Y + ((r_skymode == 2 && !(Level->flags & LEVEL_FORCETILEDSKY)) ? skytex1->GetSkyOffset() : 0);
		}

		if (viewwidth != 0 && viewheight != 0)
		{
			skyiscale = float(r_Yaspect / freelookviewheight);
			skyscale = freelookviewheight / r_Yaspect;

			skyiscale *= float(PolyRenderer::Instance()->Viewpoint.FieldOfView.Degrees / 90.);
			skyscale *= float(90. / PolyRenderer::Instance()->Viewpoint.FieldOfView.Degrees);
		}

		if (Level->skystretch)
		{
			skyscale *= (double)SKYSTRETCH_HEIGHT / skyheight;
			skyiscale *= skyheight / (float)SKYSTRETCH_HEIGHT;
			skytexturemid *= skyheight / (double)SKYSTRETCH_HEIGHT;
		}

		// The standard Doom sky texture is 256 pixels wide, repeated 4 times over 360 degrees,
		// giving a total sky width of 1024 pixels. So if the sky texture is no wider than 1024,
		// we map it to a cylinder with circumfrence 1024. For larger ones, we use the width of
		// the texture as the cylinder's circumfrence.
		sky1cyl = MAX(sskytex1->GetWidth(), fixed_t(sskytex1->GetScale().X * 1024));
		sky2cyl = MAX(sskytex2->GetWidth(), fixed_t(sskytex2->GetScale().Y * 1024));
	}

	FTextureID sky1tex, sky2tex;
	double frontdpos = 0, backdpos = 0;

	if ((PolyRenderer::Instance()->Level->flags & LEVEL_SWAPSKIES) && !(PolyRenderer::Instance()->Level->flags & LEVEL_DOUBLESKY))
	{
		sky1tex = Level->skytexture2;
	}
	else
	{
		sky1tex = Level->skytexture1;
	}
	sky2tex = Level->skytexture2;
	skymid = skytexturemid;
	skyangle = 0;

	int sectorSky = 0;// sector->sky;

	if (!(sectorSky & PL_SKYFLAT))
	{	// use sky1
	sky1:
		frontskytex = GetSWTex(sky1tex);
		if (PolyRenderer::Instance()->Level->flags & LEVEL_DOUBLESKY)
			backskytex = GetSWTex(sky2tex);
		else
			backskytex = nullptr;
		skyflip = false;
		frontdpos = Level->sky1pos;
		backdpos = Level->sky2pos;
		frontcyl = sky1cyl;
		backcyl = sky2cyl;
	}
	else if (sectorSky == PL_SKYFLAT)
	{	// use sky2
		frontskytex = GetSWTex(sky2tex);
		backskytex = nullptr;
		frontcyl = sky2cyl;
		skyflip = false;
		frontdpos = Level->sky2pos;
	}
	else
	{	// MBF's linedef-controlled skies
		// Sky Linedef
		const line_t *l = &PolyRenderer::Instance()->Level->lines[(sectorSky & ~PL_SKYFLAT) - 1];

		// Sky transferred from first sidedef
		const side_t *s = l->sidedef[0];
		int pos;

		// Texture comes from upper texture of reference sidedef
		// [RH] If swapping skies, then use the lower sidedef
		if (PolyRenderer::Instance()->Level->flags & LEVEL_SWAPSKIES && s->GetTexture(side_t::bottom).isValid())
		{
			pos = side_t::bottom;
		}
		else
		{
			pos = side_t::top;
		}

		frontskytex = GetSWTex(s->GetTexture(pos), false);
		if (frontskytex == nullptr)
		{ // [RH] The blank texture: Use normal sky instead.
			goto sky1;
		}
		backskytex = nullptr;

		// Horizontal offset is turned into an angle offset,
		// to allow sky rotation as well as careful positioning.
		// However, the offset is scaled very small, so that it
		// allows a long-period of sky rotation.
		skyangle += FLOAT2FIXED(s->GetTextureXOffset(pos));

		// Vertical offset allows careful sky positioning.
		skymid = s->GetTextureYOffset(pos);

		// We sometimes flip the picture horizontally.
		//
		// Doom always flipped the picture, so we make it optional,
		// to make it easier to use the new feature, while to still
		// allow old sky textures to be used.
		skyflip = l->args[2] ? false : true;

		int frontxscale = int(frontskytex->GetScale().X * 1024);
		frontcyl = MAX(frontskytex->GetWidth(), frontxscale);
	}

	frontpos = int(fmod(frontdpos, sky1cyl * 65536.0));
	if (backskytex != nullptr)
	{
		backpos = int(fmod(backdpos, sky2cyl * 65536.0));
	}
}
