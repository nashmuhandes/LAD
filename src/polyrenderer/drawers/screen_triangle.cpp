/*
**  Polygon Doom software renderer
**  Copyright (c) 2016 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include <stddef.h>
#include "templates.h"
#include "doomdef.h"
#include "i_system.h"
#include "w_wad.h"
#include "v_video.h"
#include "doomstat.h"
#include "st_stuff.h"
#include "g_game.h"
#include "g_level.h"
#include "r_data/r_translate.h"
#include "v_palette.h"
#include "r_data/colormaps.h"
#include "poly_triangle.h"
#include "swrenderer/drawers/r_draw_rgba.h"
#include "screen_triangle.h"
#ifndef NO_SSE
#include "poly_drawer32_sse2.h"
#else
#include "poly_drawer32.h"
#endif
#include "poly_drawer8.h"
#include "x86.h"

static void SortVertices(const TriDrawTriangleArgs *args, ShadedTriVertex **sortedVertices)
{
	sortedVertices[0] = args->v1;
	sortedVertices[1] = args->v2;
	sortedVertices[2] = args->v3;

	if (sortedVertices[1]->y < sortedVertices[0]->y)
		std::swap(sortedVertices[0], sortedVertices[1]);
	if (sortedVertices[2]->y < sortedVertices[0]->y)
		std::swap(sortedVertices[0], sortedVertices[2]);
	if (sortedVertices[2]->y < sortedVertices[1]->y)
		std::swap(sortedVertices[1], sortedVertices[2]);
}

void ScreenTriangle::Draw(const TriDrawTriangleArgs *args, PolyTriangleThreadData *thread)
{
	// Sort vertices by Y position
	ShadedTriVertex *sortedVertices[3];
	SortVertices(args, sortedVertices);

	int clipright = args->clipright;
	int clipbottom = args->clipbottom;

	// Ranges that different triangles edges are active
	int topY = (int)(sortedVertices[0]->y + 0.5f);
	int midY = (int)(sortedVertices[1]->y + 0.5f);
	int bottomY = (int)(sortedVertices[2]->y + 0.5f);

	topY = MAX(topY, 0);
	midY = clamp(midY, 0, clipbottom);
	bottomY = MIN(bottomY, clipbottom);

	if (topY >= bottomY)
		return;

	// Find start/end X positions for each line covered by the triangle:

	int leftEdge[MAXHEIGHT];
	int rightEdge[MAXHEIGHT];

	float longDX = sortedVertices[2]->x - sortedVertices[0]->x;
	float longDY = sortedVertices[2]->y - sortedVertices[0]->y;
	float longStep = longDX / longDY;
	float longPos = sortedVertices[0]->x + longStep * (topY + 0.5f - sortedVertices[0]->y) + 0.5f;

	if (topY < midY)
	{
		float shortDX = sortedVertices[1]->x - sortedVertices[0]->x;
		float shortDY = sortedVertices[1]->y - sortedVertices[0]->y;
		float shortStep = shortDX / shortDY;
		float shortPos = sortedVertices[0]->x + shortStep * (topY + 0.5f - sortedVertices[0]->y) + 0.5f;

		for (int y = topY; y < midY; y++)
		{
			int x0 = (int)shortPos;
			int x1 = (int)longPos;
			if (x1 < x0) std::swap(x0, x1);
			x0 = clamp(x0, 0, clipright);
			x1 = clamp(x1, 0, clipright);

			leftEdge[y] = x0;
			rightEdge[y] = x1;

			shortPos += shortStep;
			longPos += longStep;
		}
	}

	if (midY < bottomY)
	{
		float shortDX = sortedVertices[2]->x - sortedVertices[1]->x;
		float shortDY = sortedVertices[2]->y - sortedVertices[1]->y;
		float shortStep = shortDX / shortDY;
		float shortPos = sortedVertices[1]->x + shortStep * (midY + 0.5f - sortedVertices[1]->y) + 0.5f;

		for (int y = midY; y < bottomY; y++)
		{
			int x0 = (int)shortPos;
			int x1 = (int)longPos;
			if (x1 < x0) std::swap(x0, x1);
			x0 = clamp(x0, 0, clipright);
			x1 = clamp(x1, 0, clipright);

			leftEdge[y] = x0;
			rightEdge[y] = x1;

			shortPos += shortStep;
			longPos += longStep;
		}
	}

	// Draw the triangle:

	int bmode = (int)args->uniforms->BlendMode();
	auto drawfunc = args->destBgra ? ScreenTriangle::SpanDrawers32[bmode] : ScreenTriangle::SpanDrawers8[bmode];

	float stepXW = args->gradientX.W;
	float v1X = args->v1->x;
	float v1Y = args->v1->y;
	float v1W = args->v1->w;

	bool depthTest = args->uniforms->DepthTest();
	bool stencilTest = true;
	bool writeColor = args->uniforms->WriteColor();
	bool writeStencil = args->uniforms->WriteStencil();
	bool writeDepth = args->uniforms->WriteDepth();
	uint8_t stencilTestValue = args->uniforms->StencilTestValue();
	uint8_t stencilWriteValue = args->uniforms->StencilWriteValue();

	int num_cores = thread->num_cores;
	for (int y = topY + thread->skipped_by_thread(topY); y < bottomY; y += num_cores)
	{
		int x = leftEdge[y];
		int xend = rightEdge[y];

		float *zbufferLine = args->zbuffer + args->pitch * y;
		uint8_t *stencilLine = args->stencilbuffer + args->pitch * y;

		float startX = x + (0.5f - v1X);
		float startY = y + (0.5f - v1Y);
		float posXW = v1W + stepXW * startX + args->gradientY.W * startY + args->depthOffset;

#ifndef NO_SSE
		__m128 mstepXW = _mm_set1_ps(stepXW * 4.0f);
		__m128 mfirstStepXW = _mm_setr_ps(0.0f, stepXW, stepXW + stepXW, stepXW + stepXW + stepXW);
		while (x < xend)
		{
			int xstart = x;

			if (depthTest && stencilTest)
			{
				int xendsse = x + ((xend - x) / 4);
				__m128 mposXW = _mm_add_ps(_mm_set1_ps(posXW), mfirstStepXW);
				while (_mm_movemask_ps(_mm_cmple_ps(_mm_loadu_ps(zbufferLine + x), mposXW)) == 15 &&
					stencilLine[x] == stencilTestValue &&
					stencilLine[x + 1] == stencilTestValue &&
					stencilLine[x + 2] == stencilTestValue &&
					stencilLine[x + 3] == stencilTestValue &&
					x < xendsse)
				{
					if (writeDepth)
						_mm_storeu_ps(zbufferLine + x, mposXW);
					mposXW = _mm_add_ps(mposXW, mstepXW);
					x += 4;
				}
				posXW = _mm_cvtss_f32(mposXW);

				while (zbufferLine[x] <= posXW && stencilLine[x] == stencilTestValue && x < xend)
				{
					if (writeDepth)
						zbufferLine[x] = posXW;
					posXW += stepXW;
					x++;
				}
			}
			else if (depthTest)
			{
				int xendsse = x + ((xend - x) / 4);
				__m128 mposXW = _mm_add_ps(_mm_set1_ps(posXW), mfirstStepXW);
				while (_mm_movemask_ps(_mm_cmple_ps(_mm_loadu_ps(zbufferLine + x), mposXW)) == 15 && x < xendsse)
				{
					if (writeDepth)
						_mm_storeu_ps(zbufferLine + x, mposXW);
					mposXW = _mm_add_ps(mposXW, mstepXW);
					x += 4;
				}
				posXW = _mm_cvtss_f32(mposXW);

				while (zbufferLine[x] <= posXW && x < xend)
				{
					if (writeDepth)
						zbufferLine[x] = posXW;
					posXW += stepXW;
					x++;
				}
			}
			else if (stencilTest)
			{
				while (stencilLine[x] == stencilTestValue && x < xend)
					x++;
			}
			else
			{
				x = xend;
			}

			if (x > xstart)
			{
				if (writeColor)
					drawfunc(y, xstart, x, args);

				if (writeStencil)
				{
					for (int i = xstart; i < x; i++)
						stencilLine[i] = stencilWriteValue;
				}

				if (!depthTest && writeDepth)
				{
					for (int i = xstart; i < x; i++)
					{
						zbufferLine[i] = posXW;
						posXW += stepXW;
					}
				}
			}

			if (depthTest && stencilTest)
			{
				int xendsse = x + ((xend - x) / 4);
				__m128 mposXW = _mm_add_ps(_mm_set1_ps(posXW), mfirstStepXW);
				while ((_mm_movemask_ps(_mm_cmple_ps(_mm_loadu_ps(zbufferLine + x), mposXW)) == 0 ||
					stencilLine[x] != stencilTestValue ||
					stencilLine[x + 1] != stencilTestValue ||
					stencilLine[x + 2] != stencilTestValue ||
					stencilLine[x + 3] != stencilTestValue) &&
					x < xendsse)
				{
					mposXW = _mm_add_ps(mposXW, mstepXW);
					x += 4;
				}
				posXW = _mm_cvtss_f32(mposXW);

				while ((zbufferLine[x] > posXW || stencilLine[x] != stencilTestValue) && x < xend)
				{
					posXW += stepXW;
					x++;
				}
			}
			else if (depthTest)
			{
				int xendsse = x + ((xend - x) / 4);
				__m128 mposXW = _mm_add_ps(_mm_set1_ps(posXW), mfirstStepXW);
				while (_mm_movemask_ps(_mm_cmple_ps(_mm_loadu_ps(zbufferLine + x), mposXW)) == 0 && x < xendsse)
				{
					mposXW = _mm_add_ps(mposXW, mstepXW);
					x += 4;
				}
				posXW = _mm_cvtss_f32(mposXW);

				while (zbufferLine[x] > posXW && x < xend)
				{
					posXW += stepXW;
					x++;
				}
			}
			else if (stencilTest)
			{
				while (stencilLine[x] != stencilTestValue && x < xend)
				{
					posXW += stepXW;
					x++;
				}
			}
		}
#else
		while (x < xend)
		{
			int xstart = x;

			if (depthTest && stencilTest)
			{
				while (zbufferLine[x] <= posXW && stencilLine[x] == stencilTestValue && x < xend)
				{
					if (writeDepth)
						zbufferLine[x] = posXW;
					posXW += stepXW;
					x++;
				}
			}
			else if (depthTest)
			{
				while (zbufferLine[x] <= posXW && x < xend)
				{
					if (writeDepth)
						zbufferLine[x] = posXW;
					posXW += stepXW;
					x++;
				}
			}
			else if (stencilTest)
			{
				while (stencilLine[x] == stencilTestValue && x < xend)
					x++;
			}
			else
			{
				x = xend;
			}

			if (x > xstart)
			{
				if (writeColor)
					drawfunc(y, xstart, x, args);

				if (writeStencil)
				{
					for (int i = xstart; i < x; i++)
						stencilLine[i] = stencilWriteValue;
				}

				if (!depthTest && writeDepth)
				{
					for (int i = xstart; i < x; i++)
					{
						zbufferLine[i] = posXW;
						posXW += stepXW;
					}
				}
			}

			if (depthTest && stencilTest)
			{
				while ((zbufferLine[x] > posXW || stencilLine[x] != stencilTestValue) && x < xend)
				{
					posXW += stepXW;
					x++;
				}
			}
			else if (depthTest)
			{
				while (zbufferLine[x] > posXW && x < xend)
				{
					posXW += stepXW;
					x++;
				}
			}
			else if (stencilTest)
			{
				while (stencilLine[x] != stencilTestValue && x < xend)
				{
					posXW += stepXW;
					x++;
				}
			}
		}
#endif
	}
}

template<typename ModeT, typename OptT>
void DrawSpanOpt32(int y, int x0, int x1, const TriDrawTriangleArgs *args)
{
	using namespace TriScreenDrawerModes;

	float v1X, v1Y, v1W, v1U, v1V, v1WorldX, v1WorldY, v1WorldZ;
	float startX, startY;
	float stepXW, stepXU, stepXV, stepWorldX, stepWorldY, stepWorldZ;
	float posXW, posXU, posXV, posWorldX, posWorldY, posWorldZ;

	PolyLight *lights;
	int num_lights;
	float worldnormalX, worldnormalY, worldnormalZ;
	uint32_t dynlightcolor;
	const uint32_t *texPixels, *translation;
	int texWidth, texHeight;
	uint32_t fillcolor;
	int alpha;
	uint32_t light;
	fixed_t shade, lightpos, lightstep;
	uint32_t shade_fade_r, shade_fade_g, shade_fade_b, shade_light_r, shade_light_g, shade_light_b, desaturate, inv_desaturate;

	v1X = args->v1->x;
	v1Y = args->v1->y;
	v1W = args->v1->w;
	v1U = args->v1->u * v1W;
	v1V = args->v1->v * v1W;
	startX = x0 + (0.5f - v1X);
	startY = y + (0.5f - v1Y);
	stepXW = args->gradientX.W;
	stepXU = args->gradientX.U;
	stepXV = args->gradientX.V;
	posXW = v1W + stepXW * startX + args->gradientY.W * startY;
	posXU = v1U + stepXU * startX + args->gradientY.U * startY;
	posXV = v1V + stepXV * startX + args->gradientY.V * startY;

	texPixels = (const uint32_t*)args->uniforms->TexturePixels();
	translation = (const uint32_t*)args->uniforms->Translation();
	texWidth = args->uniforms->TextureWidth();
	texHeight = args->uniforms->TextureHeight();
	fillcolor = args->uniforms->Color();
	alpha = args->uniforms->Alpha();
	light = args->uniforms->Light();

	if (OptT::Flags & SWOPT_FixedLight)
	{
		light += light >> 7; // 255 -> 256
	}
	else
	{
		float globVis = args->uniforms->GlobVis() * (1.0f / 32.0f);

		shade = (fixed_t)((2.0f - (light + 12.0f) / 128.0f) * (float)FRACUNIT);
		lightpos = (fixed_t)(globVis * posXW * (float)FRACUNIT);
		lightstep = (fixed_t)(globVis * stepXW * (float)FRACUNIT);
	}

	if (OptT::Flags & SWOPT_DynLights)
	{
		v1WorldX = args->v1->worldX * v1W;
		v1WorldY = args->v1->worldY * v1W;
		v1WorldZ = args->v1->worldZ * v1W;
		stepWorldX = args->gradientX.WorldX;
		stepWorldY = args->gradientX.WorldY;
		stepWorldZ = args->gradientX.WorldZ;
		posWorldX = v1WorldX + stepWorldX * startX + args->gradientY.WorldX * startY;
		posWorldY = v1WorldY + stepWorldY * startX + args->gradientY.WorldY * startY;
		posWorldZ = v1WorldZ + stepWorldZ * startX + args->gradientY.WorldZ * startY;

		lights = args->uniforms->Lights();
		num_lights = args->uniforms->NumLights();
		worldnormalX = args->uniforms->Normal().X;
		worldnormalY = args->uniforms->Normal().Y;
		worldnormalZ = args->uniforms->Normal().Z;
		dynlightcolor = args->uniforms->DynLightColor();
	}

	if (OptT::Flags & SWOPT_ColoredFog)
	{
		shade_fade_r = args->uniforms->ShadeFadeRed();
		shade_fade_g = args->uniforms->ShadeFadeGreen();
		shade_fade_b = args->uniforms->ShadeFadeBlue();
		shade_light_r = args->uniforms->ShadeLightRed();
		shade_light_g = args->uniforms->ShadeLightGreen();
		shade_light_b = args->uniforms->ShadeLightBlue();
		desaturate = args->uniforms->ShadeDesaturate();
		inv_desaturate = 256 - desaturate;
	}

	uint32_t *dest = (uint32_t*)args->dest;
	uint32_t *destLine = dest + args->pitch * y;

	int x = x0;
	while (x < x1)
	{
		uint32_t fg = 0;

		if (ModeT::SWFlags & SWSTYLEF_Fill)
		{
			fg = fillcolor;
		}
		else if (ModeT::SWFlags & SWSTYLEF_FogBoundary)
		{
			fg = destLine[x];
		}
		else if (ModeT::BlendOp != STYLEOP_Fuzz)
		{
			float rcpW = 0x01000000 / posXW;
			int32_t u = (int32_t)(posXU * rcpW);
			int32_t v = (int32_t)(posXV * rcpW);
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;

			if (ModeT::SWFlags & SWSTYLEF_Translated)
			{
				fg = translation[((const uint8_t*)texPixels)[texelX * texHeight + texelY]];
			}
			else if (ModeT::Flags & STYLEF_RedIsAlpha)
			{
				fg = ((const uint8_t*)texPixels)[texelX * texHeight + texelY];
			}
			else
			{
				fg = texPixels[texelX * texHeight + texelY];
			}
		}

		if (ModeT::BlendOp == STYLEOP_Fuzz)
		{
			using namespace swrenderer;

			float rcpW = 0x01000000 / posXW;
			int32_t u = (int32_t)(posXU * rcpW);
			int32_t v = (int32_t)(posXV * rcpW);
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			unsigned int sampleshadeout = APART(texPixels[texelX * texHeight + texelY]);
			sampleshadeout += sampleshadeout >> 7; // 255 -> 256

			fixed_t fuzzscale = (200 << FRACBITS) / viewheight;

			int scaled_x = (x * fuzzscale) >> FRACBITS;
			int fuzz_x = fuzz_random_x_offset[scaled_x % FUZZ_RANDOM_X_SIZE] + fuzzpos;

			fixed_t fuzzcount = FUZZTABLE << FRACBITS;
			fixed_t fuzz = ((fuzz_x << FRACBITS) + y * fuzzscale) % fuzzcount;
			unsigned int alpha = fuzzoffset[fuzz >> FRACBITS];

			sampleshadeout = (sampleshadeout * alpha) >> 5;

			uint32_t a = 256 - sampleshadeout;

			uint32_t dest = destLine[x];
			uint32_t out_r = (RPART(dest) * a) >> 8;
			uint32_t out_g = (GPART(dest) * a) >> 8;
			uint32_t out_b = (BPART(dest) * a) >> 8;
			destLine[x] = MAKEARGB(255, out_r, out_g, out_b);
		}
		else if (ModeT::SWFlags & SWSTYLEF_Skycap)
		{
			float rcpW = 0x01000000 / posXW;
			int32_t v = (int32_t)(posXV * rcpW);

			int start_fade = 2; // How fast it should fade out
			int alpha_top = clamp(v >> (16 - start_fade), 0, 256);
			int alpha_bottom = clamp(((2 << 24) - v) >> (16 - start_fade), 0, 256);
			int a = MIN(alpha_top, alpha_bottom);
			int inv_a = 256 - a;

			if (a == 256)
			{
				destLine[x] = fg;
			}
			else
			{
				uint32_t r = RPART(fg);
				uint32_t g = GPART(fg);
				uint32_t b = BPART(fg);
				uint32_t fg_a = APART(fg);
				uint32_t bg_red = RPART(fillcolor);
				uint32_t bg_green = GPART(fillcolor);
				uint32_t bg_blue = BPART(fillcolor);
				r = (r * a + bg_red * inv_a + 127) >> 8;
				g = (g * a + bg_green * inv_a + 127) >> 8;
				b = (b * a + bg_blue * inv_a + 127) >> 8;

				destLine[x] = MAKEARGB(255, r, g, b);
			}
		}
		else
		{
			if ((ModeT::Flags & STYLEF_ColorIsFixed) && !(ModeT::SWFlags & SWSTYLEF_Fill))
			{
				if (ModeT::Flags & STYLEF_RedIsAlpha)
					fg = (fg << 24) | (fillcolor & 0x00ffffff);
				else
					fg = (fg & 0xff000000) | (fillcolor & 0x00ffffff);
			}

			uint32_t fgalpha = fg >> 24;

			if (!(ModeT::Flags & STYLEF_Alpha1))
			{
				fgalpha = (fgalpha * alpha) >> 8;
			}

			int lightshade;
			if (OptT::Flags & SWOPT_FixedLight)
			{
				lightshade = light;
			}
			else
			{
				fixed_t maxvis = 24 * FRACUNIT / 32;
				fixed_t maxlight = 31 * FRACUNIT / 32;
				lightshade = (FRACUNIT - clamp<fixed_t>(shade - MIN(maxvis, lightpos), 0, maxlight)) >> 8;
			}

			uint32_t lit_r = 0, lit_g = 0, lit_b = 0;
			if (OptT::Flags & SWOPT_DynLights)
			{
				lit_r = RPART(dynlightcolor);
				lit_g = GPART(dynlightcolor);
				lit_b = BPART(dynlightcolor);

				float rcp_posXW = 1.0f / posXW;
				float worldposX = posWorldX * rcp_posXW;
				float worldposY = posWorldY * rcp_posXW;
				float worldposZ = posWorldZ * rcp_posXW;
				for (int i = 0; i < num_lights; i++)
				{
					float lightposX = lights[i].x;
					float lightposY = lights[i].y;
					float lightposZ = lights[i].z;
					float light_radius = lights[i].radius;
					uint32_t light_color = lights[i].color;

					bool is_attenuated = light_radius < 0.0f;
					if (is_attenuated)
						light_radius = -light_radius;

					// L = light-pos
					// dist = sqrt(dot(L, L))
					// distance_attenuation = 1 - MIN(dist * (1/radius), 1)
					float Lx = lightposX - worldposX;
					float Ly = lightposY - worldposY;
					float Lz = lightposZ - worldposZ;
					float dist2 = Lx * Lx + Ly * Ly + Lz * Lz;
#ifdef NO_SSE
					//float rcp_dist = 1.0f / sqrt(dist2);
					float rcp_dist = 1.0f / (dist2 * 0.01f);
#else
					float rcp_dist = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(dist2)));
#endif
					float dist = dist2 * rcp_dist;
					float distance_attenuation = 256.0f - MIN(dist * light_radius, 256.0f);

					// The simple light type
					float simple_attenuation = distance_attenuation;

					// The point light type
					// diffuse = max(dot(N,normalize(L)),0) * attenuation
					Lx *= rcp_dist;
					Ly *= rcp_dist;
					Lz *= rcp_dist;
					float dotNL = worldnormalX * Lx + worldnormalY * Ly + worldnormalZ * Lz;
					float point_attenuation = MAX(dotNL, 0.0f) * distance_attenuation;

					uint32_t attenuation = (uint32_t)(is_attenuated ? (int32_t)point_attenuation : (int32_t)simple_attenuation);

					lit_r += (RPART(light_color) * attenuation) >> 8;
					lit_g += (GPART(light_color) * attenuation) >> 8;
					lit_b += (BPART(light_color) * attenuation) >> 8;
				}
			}

			uint32_t shadedfg_r, shadedfg_g, shadedfg_b;
			if (OptT::Flags & SWOPT_ColoredFog)
			{
				uint32_t fg_r = RPART(fg);
				uint32_t fg_g = GPART(fg);
				uint32_t fg_b = BPART(fg);
				uint32_t intensity = ((fg_r * 77 + fg_g * 143 + fg_b * 37) >> 8) * desaturate;
				shadedfg_r = (((shade_fade_r + ((fg_r * inv_desaturate + intensity) >> 8) * lightshade) >> 8) * shade_light_r) >> 8;
				shadedfg_g = (((shade_fade_g + ((fg_g * inv_desaturate + intensity) >> 8) * lightshade) >> 8) * shade_light_g) >> 8;
				shadedfg_b = (((shade_fade_b + ((fg_b * inv_desaturate + intensity) >> 8) * lightshade) >> 8) * shade_light_b) >> 8;

				lit_r = MIN(lit_r, (uint32_t)256);
				lit_g = MIN(lit_g, (uint32_t)256);
				lit_b = MIN(lit_b, (uint32_t)256);

				shadedfg_r = MIN(shadedfg_r + ((fg_r * lit_r) >> 8), (uint32_t)255);
				shadedfg_g = MIN(shadedfg_g + ((fg_g * lit_g) >> 8), (uint32_t)255);
				shadedfg_b = MIN(shadedfg_b + ((fg_b * lit_b) >> 8), (uint32_t)255);
			}
			else
			{
				lit_r = MIN(lightshade + lit_r, (uint32_t)256);
				lit_g = MIN(lightshade + lit_g, (uint32_t)256);
				lit_b = MIN(lightshade + lit_b, (uint32_t)256);

				shadedfg_r = (RPART(fg) * lit_r) >> 8;
				shadedfg_g = (GPART(fg) * lit_g) >> 8;
				shadedfg_b = (BPART(fg) * lit_b) >> 8;
			}

			if (ModeT::BlendSrc == STYLEALPHA_One && ModeT::BlendDest == STYLEALPHA_Zero)
			{
				destLine[x] = MAKEARGB(255, shadedfg_r, shadedfg_g, shadedfg_b);
			}
			else if (ModeT::BlendSrc == STYLEALPHA_One && ModeT::BlendDest == STYLEALPHA_One)
			{
				uint32_t dest = destLine[x];

				if (ModeT::BlendOp == STYLEOP_Add)
				{
					uint32_t out_r = MIN<uint32_t>(RPART(dest) + shadedfg_r, 255);
					uint32_t out_g = MIN<uint32_t>(GPART(dest) + shadedfg_g, 255);
					uint32_t out_b = MIN<uint32_t>(BPART(dest) + shadedfg_b, 255);
					destLine[x] = MAKEARGB(255, out_r, out_g, out_b);
				}
				else if (ModeT::BlendOp == STYLEOP_RevSub)
				{
					uint32_t out_r = MAX<uint32_t>(RPART(dest) - shadedfg_r, 0);
					uint32_t out_g = MAX<uint32_t>(GPART(dest) - shadedfg_g, 0);
					uint32_t out_b = MAX<uint32_t>(BPART(dest) - shadedfg_b, 0);
					destLine[x] = MAKEARGB(255, out_r, out_g, out_b);
				}
				else //if (ModeT::BlendOp == STYLEOP_Sub)
				{
					uint32_t out_r = MAX<uint32_t>(shadedfg_r - RPART(dest), 0);
					uint32_t out_g = MAX<uint32_t>(shadedfg_g - GPART(dest), 0);
					uint32_t out_b = MAX<uint32_t>(shadedfg_b - BPART(dest), 0);
					destLine[x] = MAKEARGB(255, out_r, out_g, out_b);
				}
			}
			else if (ModeT::SWFlags & SWSTYLEF_SrcColorOneMinusSrcColor)
			{
				uint32_t dest = destLine[x];

				uint32_t sfactor_r = shadedfg_r; sfactor_r += sfactor_r >> 7; // 255 -> 256
				uint32_t sfactor_g = shadedfg_g; sfactor_g += sfactor_g >> 7; // 255 -> 256
				uint32_t sfactor_b = shadedfg_b; sfactor_b += sfactor_b >> 7; // 255 -> 256
				uint32_t sfactor_a = fgalpha; sfactor_a += sfactor_a >> 7; // 255 -> 256
				uint32_t dfactor_r = 256 - sfactor_r;
				uint32_t dfactor_g = 256 - sfactor_g;
				uint32_t dfactor_b = 256 - sfactor_b;
				uint32_t out_r = (RPART(dest) * dfactor_r + shadedfg_r * sfactor_r + 128) >> 8;
				uint32_t out_g = (GPART(dest) * dfactor_g + shadedfg_g * sfactor_g + 128) >> 8;
				uint32_t out_b = (BPART(dest) * dfactor_b + shadedfg_b * sfactor_b + 128) >> 8;

				destLine[x] = MAKEARGB(255, out_r, out_g, out_b);
			}
			else if (ModeT::BlendSrc == STYLEALPHA_Src && ModeT::BlendDest == STYLEALPHA_InvSrc && fgalpha == 255)
			{
				destLine[x] = MAKEARGB(255, shadedfg_r, shadedfg_g, shadedfg_b);
			}
			else if (ModeT::BlendSrc != STYLEALPHA_Src || ModeT::BlendDest != STYLEALPHA_InvSrc || fgalpha != 0)
			{
				uint32_t dest = destLine[x];

				uint32_t sfactor = fgalpha; sfactor += sfactor >> 7; // 255 -> 256
				uint32_t src_r = shadedfg_r * sfactor;
				uint32_t src_g = shadedfg_g * sfactor;
				uint32_t src_b = shadedfg_b * sfactor;
				uint32_t dest_r = RPART(dest);
				uint32_t dest_g = GPART(dest);
				uint32_t dest_b = BPART(dest);
				if (ModeT::BlendDest == STYLEALPHA_One)
				{
					dest_r <<= 8;
					dest_g <<= 8;
					dest_b <<= 8;
				}
				else
				{
					uint32_t dfactor = 256 - sfactor;
					dest_r *= dfactor;
					dest_g *= dfactor;
					dest_b *= dfactor;
				}

				uint32_t out_r, out_g, out_b;
				if (ModeT::BlendOp == STYLEOP_Add)
				{
					if (ModeT::BlendDest == STYLEALPHA_One)
					{
						out_r = MIN<int32_t>((dest_r + src_r + 128) >> 8, 255);
						out_g = MIN<int32_t>((dest_g + src_g + 128) >> 8, 255);
						out_b = MIN<int32_t>((dest_b + src_b + 128) >> 8, 255);
					}
					else
					{
						out_r = (dest_r + src_r + 128) >> 8;
						out_g = (dest_g + src_g + 128) >> 8;
						out_b = (dest_b + src_b + 128) >> 8;
					}
				}
				else if (ModeT::BlendOp == STYLEOP_RevSub)
				{
					out_r = MAX<int32_t>(static_cast<int32_t>(dest_r - src_r + 128) >> 8, 0);
					out_g = MAX<int32_t>(static_cast<int32_t>(dest_g - src_g + 128) >> 8, 0);
					out_b = MAX<int32_t>(static_cast<int32_t>(dest_b - src_b + 128) >> 8, 0);
				}
				else //if (ModeT::BlendOp == STYLEOP_Sub)
				{
					out_r = MAX<int32_t>(static_cast<int32_t>(src_r - dest_r + 128) >> 8, 0);
					out_g = MAX<int32_t>(static_cast<int32_t>(src_g - dest_g + 128) >> 8, 0);
					out_b = MAX<int32_t>(static_cast<int32_t>(src_b - dest_b + 128) >> 8, 0);
				}

				destLine[x] = MAKEARGB(255, out_r, out_g, out_b);
			}
		}

		posXW += stepXW;
		posXU += stepXU;
		posXV += stepXV;
		if (OptT::Flags & SWOPT_DynLights)
		{
			posWorldX += stepWorldX;
			posWorldY += stepWorldY;
			posWorldZ += stepWorldZ;
		}
		if (!(OptT::Flags & SWOPT_FixedLight))
			lightpos += lightstep;
		x++;
	}
}

template<typename ModeT>
void DrawSpan32(int y, int x0, int x1, const TriDrawTriangleArgs *args)
{
	using namespace TriScreenDrawerModes;

	if (args->uniforms->NumLights() == 0)
	{
		if (!args->uniforms->FixedLight())
		{
			if (args->uniforms->SimpleShade())
				DrawSpanOpt32<ModeT, DrawerOpt>(y, x0, x1, args);
			else
				DrawSpanOpt32<ModeT, DrawerOptC>(y, x0, x1, args);
		}
		else
		{
			if (args->uniforms->SimpleShade())
				DrawSpanOpt32<ModeT, DrawerOptF>(y, x0, x1, args);
			else
				DrawSpanOpt32<ModeT, DrawerOptCF>(y, x0, x1, args);
		}
	}
	else
	{
		if (!args->uniforms->FixedLight())
		{
			if (args->uniforms->SimpleShade())
				DrawSpanOpt32<ModeT, DrawerOptL>(y, x0, x1, args);
			else
				DrawSpanOpt32<ModeT, DrawerOptLC>(y, x0, x1, args);
		}
		else
		{
			if (args->uniforms->SimpleShade())
				DrawSpanOpt32<ModeT, DrawerOptLF>(y, x0, x1, args);
			else
				DrawSpanOpt32<ModeT, DrawerOptLCF>(y, x0, x1, args);
		}
	}
}

template<typename ModeT, typename OptT>
void DrawSpanOpt8(int y, int x0, int x1, const TriDrawTriangleArgs *args)
{
	using namespace TriScreenDrawerModes;

	float v1X, v1Y, v1W, v1U, v1V, v1WorldX, v1WorldY, v1WorldZ;
	float startX, startY;
	float stepXW, stepXU, stepXV, stepWorldX, stepWorldY, stepWorldZ;
	float posXW, posXU, posXV, posWorldX, posWorldY, posWorldZ;

	PolyLight *lights;
	int num_lights;
	float worldnormalX, worldnormalY, worldnormalZ;
	uint32_t dynlightcolor;
	const uint8_t *colormaps, *texPixels, *translation;
	int texWidth, texHeight;
	uint32_t fillcolor, capcolor;
	int alpha;
	uint32_t light;
	fixed_t shade, lightpos, lightstep;

	v1X = args->v1->x;
	v1Y = args->v1->y;
	v1W = args->v1->w;
	v1U = args->v1->u * v1W;
	v1V = args->v1->v * v1W;
	startX = x0 + (0.5f - v1X);
	startY = y + (0.5f - v1Y);
	stepXW = args->gradientX.W;
	stepXU = args->gradientX.U;
	stepXV = args->gradientX.V;
	posXW = v1W + stepXW * startX + args->gradientY.W * startY;
	posXU = v1U + stepXU * startX + args->gradientY.U * startY;
	posXV = v1V + stepXV * startX + args->gradientY.V * startY;

	texPixels = args->uniforms->TexturePixels();
	translation = args->uniforms->Translation();
	texWidth = args->uniforms->TextureWidth();
	texHeight = args->uniforms->TextureHeight();
	fillcolor = args->uniforms->Color();
	alpha = args->uniforms->Alpha();
	colormaps = args->uniforms->BaseColormap();
	light = args->uniforms->Light();

	if (ModeT::SWFlags & SWSTYLEF_Skycap)
		capcolor = GPalette.BaseColors[fillcolor].d;

	if (OptT::Flags & SWOPT_FixedLight)
	{
		light += light >> 7; // 255 -> 256
		light = ((256 - light) * NUMCOLORMAPS) & 0xffffff00;
	}
	else
	{
		float globVis = args->uniforms->GlobVis() * (1.0f / 32.0f);

		shade = (fixed_t)((2.0f - (light + 12.0f) / 128.0f) * (float)FRACUNIT);
		lightpos = (fixed_t)(globVis * posXW * (float)FRACUNIT);
		lightstep = (fixed_t)(globVis * stepXW * (float)FRACUNIT);
	}

	if (OptT::Flags & SWOPT_DynLights)
	{
		v1WorldX = args->v1->worldX * v1W;
		v1WorldY = args->v1->worldY * v1W;
		v1WorldZ = args->v1->worldZ * v1W;
		stepWorldX = args->gradientX.WorldX;
		stepWorldY = args->gradientX.WorldY;
		stepWorldZ = args->gradientX.WorldZ;
		posWorldX = v1WorldX + stepWorldX * startX + args->gradientY.WorldX * startY;
		posWorldY = v1WorldY + stepWorldY * startX + args->gradientY.WorldY * startY;
		posWorldZ = v1WorldZ + stepWorldZ * startX + args->gradientY.WorldZ * startY;

		lights = args->uniforms->Lights();
		num_lights = args->uniforms->NumLights();
		worldnormalX = args->uniforms->Normal().X;
		worldnormalY = args->uniforms->Normal().Y;
		worldnormalZ = args->uniforms->Normal().Z;
		dynlightcolor = args->uniforms->DynLightColor();
	}

	uint8_t *dest = (uint8_t*)args->dest;
	uint8_t *destLine = dest + args->pitch * y;

	int x = x0;
	while (x < x1)
	{
		int fg = 0;
		int fgalpha = 255;

		if (ModeT::SWFlags & SWSTYLEF_Fill)
		{
			fg = fillcolor;
		}
		else if (ModeT::SWFlags & SWSTYLEF_FogBoundary)
		{
			fg = destLine[x];
		}
		else if (ModeT::BlendOp != STYLEOP_Fuzz)
		{
			float rcpW = 0x01000000 / posXW;
			int32_t u = (int32_t)(posXU * rcpW);
			int32_t v = (int32_t)(posXV * rcpW);
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			fg = texPixels[texelX * texHeight + texelY];

			if (ModeT::SWFlags & SWSTYLEF_Translated)
				fg = translation[fg];

			fgalpha = (fg != 0) ? 255 : 0;
		}

		if (ModeT::BlendOp == STYLEOP_Fuzz)
		{
			using namespace swrenderer;

			float rcpW = 0x01000000 / posXW;
			int32_t u = (int32_t)(posXU * rcpW);
			int32_t v = (int32_t)(posXV * rcpW);
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			unsigned int sampleshadeout = (texPixels[texelX * texHeight + texelY] != 0) ? 256 : 0;

			fixed_t fuzzscale = (200 << FRACBITS) / viewheight;

			int scaled_x = (x * fuzzscale) >> FRACBITS;
			int fuzz_x = fuzz_random_x_offset[scaled_x % FUZZ_RANDOM_X_SIZE] + fuzzpos;

			fixed_t fuzzcount = FUZZTABLE << FRACBITS;
			fixed_t fuzz = ((fuzz_x << FRACBITS) + y * fuzzscale) % fuzzcount;
			unsigned int alpha = fuzzoffset[fuzz >> FRACBITS];

			sampleshadeout = (sampleshadeout * alpha) >> 5;

			uint32_t a = 256 - sampleshadeout;

			uint32_t dest = GPalette.BaseColors[destLine[x]].d;
			uint32_t r = (RPART(dest) * a) >> 8;
			uint32_t g = (GPART(dest) * a) >> 8;
			uint32_t b = (BPART(dest) * a) >> 8;
			destLine[x] = RGB256k.All[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)];
		}
		else if (ModeT::SWFlags & SWSTYLEF_Skycap)
		{
			float rcpW = 0x01000000 / posXW;
			int32_t v = (int32_t)(posXV * rcpW);

			int start_fade = 2; // How fast it should fade out
			int alpha_top = clamp(v >> (16 - start_fade), 0, 256);
			int alpha_bottom = clamp(((2 << 24) - v) >> (16 - start_fade), 0, 256);
			int a = MIN(alpha_top, alpha_bottom);
			int inv_a = 256 - a;

			if (a == 256)
			{
				destLine[x] = fg;
			}
			else
			{
				uint32_t texelrgb = GPalette.BaseColors[fg].d;

				uint32_t r = RPART(texelrgb);
				uint32_t g = GPART(texelrgb);
				uint32_t b = BPART(texelrgb);
				uint32_t fg_a = APART(texelrgb);
				uint32_t bg_red = RPART(capcolor);
				uint32_t bg_green = GPART(capcolor);
				uint32_t bg_blue = BPART(capcolor);
				r = (r * a + bg_red * inv_a + 127) >> 8;
				g = (g * a + bg_green * inv_a + 127) >> 8;
				b = (b * a + bg_blue * inv_a + 127) >> 8;

				destLine[x] = RGB256k.All[((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2)];
			}
		}
		else
		{
			if ((ModeT::Flags & STYLEF_ColorIsFixed) && !(ModeT::SWFlags & SWSTYLEF_Fill))
			{
				if (ModeT::Flags & STYLEF_RedIsAlpha)
					fgalpha = fg;
				fg = fillcolor;
			}

			if (!(ModeT::Flags & STYLEF_Alpha1))
			{
				fgalpha = (fgalpha * alpha) >> 8;
			}

			uint8_t shadedfg;
			if (OptT::Flags & SWOPT_FixedLight)
			{
				shadedfg = colormaps[light + fg];
			}
			else
			{
				fixed_t maxvis = 24 * FRACUNIT / 32;
				fixed_t maxlight = 31 * FRACUNIT / 32;
				int lightshade = (FRACUNIT - clamp<fixed_t>(shade - MIN(maxvis, lightpos), 0, maxlight)) >> 8;
				lightshade = ((256 - lightshade) << 5) & 0xffffff00;
				shadedfg = colormaps[lightshade + fg];
			}

			if (OptT::Flags & SWOPT_DynLights)
			{
				uint32_t lit_r = RPART(dynlightcolor);
				uint32_t lit_g = GPART(dynlightcolor);
				uint32_t lit_b = BPART(dynlightcolor);

#ifdef NO_SSE
				float rcp_posXW = 1.0f / posXW;
#else
				float rcp_posXW = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(posXW)));
#endif
				float worldposX = posWorldX * rcp_posXW;
				float worldposY = posWorldY * rcp_posXW;
				float worldposZ = posWorldZ * rcp_posXW;
				for (int i = 0; i < num_lights; i++)
				{
					float lightposX = lights[i].x;
					float lightposY = lights[i].y;
					float lightposZ = lights[i].z;
					float light_radius = lights[i].radius;
					uint32_t light_color = lights[i].color;

					bool is_attenuated = light_radius < 0.0f;
					if (is_attenuated)
						light_radius = -light_radius;

					// L = light-pos
					// dist = sqrt(dot(L, L))
					// distance_attenuation = 1 - MIN(dist * (1/radius), 1)
					float Lx = lightposX - worldposX;
					float Ly = lightposY - worldposY;
					float Lz = lightposZ - worldposZ;
					float dist2 = Lx * Lx + Ly * Ly + Lz * Lz;
#ifdef NO_SSE
					//float rcp_dist = 1.0f / sqrt(dist2);
					float rcp_dist = 1.0f / (dist2 * 0.01f);
#else
					float rcp_dist = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(dist2)));
#endif
					float dist = dist2 * rcp_dist;
					float distance_attenuation = 256.0f - MIN(dist * light_radius, 256.0f);

					// The simple light type
					float simple_attenuation = distance_attenuation;

					// The point light type
					// diffuse = max(dot(N,normalize(L)),0) * attenuation
					Lx *= rcp_dist;
					Ly *= rcp_dist;
					Lz *= rcp_dist;
					float dotNL = worldnormalX * Lx + worldnormalY * Ly + worldnormalZ * Lz;
					float point_attenuation = MAX(dotNL, 0.0f) * distance_attenuation;

					uint32_t attenuation = (uint32_t)(is_attenuated ? (int32_t)point_attenuation : (int32_t)simple_attenuation);

					lit_r += (RPART(light_color) * attenuation) >> 8;
					lit_g += (GPART(light_color) * attenuation) >> 8;
					lit_b += (BPART(light_color) * attenuation) >> 8;
				}

				if (lit_r || lit_g || lit_b)
				{
					lit_r = MIN(lit_r, (uint32_t)256);
					lit_g = MIN(lit_g, (uint32_t)256);
					lit_b = MIN(lit_b, (uint32_t)256);

					uint32_t fgrgb = GPalette.BaseColors[fg];
					uint32_t shadedfgrgb = GPalette.BaseColors[shadedfg];

					uint32_t out_r = MIN(((RPART(fgrgb) * lit_r) >> 8) + RPART(shadedfgrgb), (uint32_t)255);
					uint32_t out_g = MIN(((GPART(fgrgb) * lit_g) >> 8) + GPART(shadedfgrgb), (uint32_t)255);
					uint32_t out_b = MIN(((BPART(fgrgb) * lit_b) >> 8) + BPART(shadedfgrgb), (uint32_t)255);
					shadedfg = RGB256k.All[((out_r >> 2) << 12) | ((out_g >> 2) << 6) | (out_b >> 2)];
				}
			}

			if (ModeT::BlendSrc == STYLEALPHA_One && ModeT::BlendDest == STYLEALPHA_Zero)
			{
				destLine[x] = shadedfg;
			}
			else if (ModeT::BlendSrc == STYLEALPHA_One && ModeT::BlendDest == STYLEALPHA_One)
			{
				uint32_t src = GPalette.BaseColors[shadedfg];
				uint32_t dest = GPalette.BaseColors[destLine[x]];

				if (ModeT::BlendOp == STYLEOP_Add)
				{
					uint32_t out_r = MIN<uint32_t>(RPART(dest) + RPART(src), 255);
					uint32_t out_g = MIN<uint32_t>(GPART(dest) + GPART(src), 255);
					uint32_t out_b = MIN<uint32_t>(BPART(dest) + BPART(src), 255);
					destLine[x] = RGB256k.All[((out_r >> 2) << 12) | ((out_g >> 2) << 6) | (out_b >> 2)];
				}
				else if (ModeT::BlendOp == STYLEOP_RevSub)
				{
					uint32_t out_r = MAX<uint32_t>(RPART(dest) - RPART(src), 0);
					uint32_t out_g = MAX<uint32_t>(GPART(dest) - GPART(src), 0);
					uint32_t out_b = MAX<uint32_t>(BPART(dest) - BPART(src), 0);
					destLine[x] = RGB256k.All[((out_r >> 2) << 12) | ((out_g >> 2) << 6) | (out_b >> 2)];
				}
				else //if (ModeT::BlendOp == STYLEOP_Sub)
				{
					uint32_t out_r = MAX<uint32_t>(RPART(src) - RPART(dest), 0);
					uint32_t out_g = MAX<uint32_t>(GPART(src) - GPART(dest), 0);
					uint32_t out_b = MAX<uint32_t>(BPART(src) - BPART(dest), 0);
					destLine[x] = RGB256k.All[((out_r >> 2) << 12) | ((out_g >> 2) << 6) | (out_b >> 2)];
				}
			}
			else if (ModeT::SWFlags & SWSTYLEF_SrcColorOneMinusSrcColor)
			{
				uint32_t src = GPalette.BaseColors[shadedfg];
				uint32_t dest = GPalette.BaseColors[destLine[x]];

				uint32_t sfactor_r = RPART(src); sfactor_r += sfactor_r >> 7; // 255 -> 256
				uint32_t sfactor_g = GPART(src); sfactor_g += sfactor_g >> 7; // 255 -> 256
				uint32_t sfactor_b = BPART(src); sfactor_b += sfactor_b >> 7; // 255 -> 256
				uint32_t sfactor_a = fgalpha; sfactor_a += sfactor_a >> 7; // 255 -> 256
				uint32_t dfactor_r = 256 - sfactor_r;
				uint32_t dfactor_g = 256 - sfactor_g;
				uint32_t dfactor_b = 256 - sfactor_b;
				uint32_t out_r = (RPART(dest) * dfactor_r + RPART(src) * sfactor_r + 128) >> 8;
				uint32_t out_g = (GPART(dest) * dfactor_g + GPART(src) * sfactor_g + 128) >> 8;
				uint32_t out_b = (BPART(dest) * dfactor_b + BPART(src) * sfactor_b + 128) >> 8;

				destLine[x] = RGB256k.All[((out_r >> 2) << 12) | ((out_g >> 2) << 6) | (out_b >> 2)];
			}
			else if (ModeT::BlendSrc == STYLEALPHA_Src && ModeT::BlendDest == STYLEALPHA_InvSrc && fgalpha == 255)
			{
				destLine[x] = shadedfg;
			}
			else if (ModeT::BlendSrc != STYLEALPHA_Src || ModeT::BlendDest != STYLEALPHA_InvSrc || fgalpha != 0)
			{
				uint32_t src = GPalette.BaseColors[shadedfg];
				uint32_t dest = GPalette.BaseColors[destLine[x]];

				uint32_t sfactor = fgalpha; sfactor += sfactor >> 7; // 255 -> 256
				uint32_t dfactor = 256 - sfactor;
				uint32_t src_r = RPART(src) * sfactor;
				uint32_t src_g = GPART(src) * sfactor;
				uint32_t src_b = BPART(src) * sfactor;
				uint32_t dest_r = RPART(dest);
				uint32_t dest_g = GPART(dest);
				uint32_t dest_b = BPART(dest);
				if (ModeT::BlendDest == STYLEALPHA_One)
				{
					dest_r <<= 8;
					dest_g <<= 8;
					dest_b <<= 8;
				}
				else
				{
					uint32_t dfactor = 256 - sfactor;
					dest_r *= dfactor;
					dest_g *= dfactor;
					dest_b *= dfactor;
				}

				uint32_t out_r, out_g, out_b;
				if (ModeT::BlendOp == STYLEOP_Add)
				{
					if (ModeT::BlendDest == STYLEALPHA_One)
					{
						out_r = MIN<int32_t>((dest_r + src_r + 128) >> 8, 255);
						out_g = MIN<int32_t>((dest_g + src_g + 128) >> 8, 255);
						out_b = MIN<int32_t>((dest_b + src_b + 128) >> 8, 255);
					}
					else
					{
						out_r = (dest_r + src_r + 128) >> 8;
						out_g = (dest_g + src_g + 128) >> 8;
						out_b = (dest_b + src_b + 128) >> 8;
					}
				}
				else if (ModeT::BlendOp == STYLEOP_RevSub)
				{
					out_r = MAX<int32_t>(static_cast<int32_t>(dest_r - src_r + 128) >> 8, 0);
					out_g = MAX<int32_t>(static_cast<int32_t>(dest_g - src_g + 128) >> 8, 0);
					out_b = MAX<int32_t>(static_cast<int32_t>(dest_b - src_b + 128) >> 8, 0);
				}
				else //if (ModeT::BlendOp == STYLEOP_Sub)
				{
					out_r = MAX<int32_t>(static_cast<int32_t>(src_r - dest_r + 128) >> 8, 0);
					out_g = MAX<int32_t>(static_cast<int32_t>(src_g - dest_g + 128) >> 8, 0);
					out_b = MAX<int32_t>(static_cast<int32_t>(src_b - dest_b + 128) >> 8, 0);
				}

				destLine[x] = RGB256k.All[((out_r >> 2) << 12) | ((out_g >> 2) << 6) | (out_b >> 2)];
			}
		}

		posXW += stepXW;
		posXU += stepXU;
		posXV += stepXV;
		if (OptT::Flags & SWOPT_DynLights)
		{
			posWorldX += stepWorldX;
			posWorldY += stepWorldY;
			posWorldZ += stepWorldZ;
		}
		if (!(OptT::Flags & SWOPT_FixedLight))
			lightpos += lightstep;
		x++;
	}
}

template<typename ModeT>
void DrawSpan8(int y, int x0, int x1, const TriDrawTriangleArgs *args)
{
	using namespace TriScreenDrawerModes;

	if (args->uniforms->NumLights() == 0)
	{
		if (!args->uniforms->FixedLight())
			DrawSpanOpt8<ModeT, DrawerOptC>(y, x0, x1, args);
		else
			DrawSpanOpt8<ModeT, DrawerOptCF>(y, x0, x1, args);
	}
	else
	{
		if (!args->uniforms->FixedLight())
			DrawSpanOpt8<ModeT, DrawerOptLC>(y, x0, x1, args);
		else
			DrawSpanOpt8<ModeT, DrawerOptLCF>(y, x0, x1, args);
	}
}

void(*ScreenTriangle::SpanDrawers8[])(int, int, int, const TriDrawTriangleArgs *) =
{
	&DrawSpan8<TriScreenDrawerModes::StyleOpaque>,
	&DrawSpan8<TriScreenDrawerModes::StyleSkycap>,
	&DrawSpan8<TriScreenDrawerModes::StyleFogBoundary>,
	&DrawSpan8<TriScreenDrawerModes::StyleSrcColor>,
	&DrawSpan8<TriScreenDrawerModes::StyleFill>,
	&DrawSpan8<TriScreenDrawerModes::StyleNormal>,
	&DrawSpan8<TriScreenDrawerModes::StyleFuzzy>,
	&DrawSpan8<TriScreenDrawerModes::StyleStencil>,
	&DrawSpan8<TriScreenDrawerModes::StyleTranslucent>,
	&DrawSpan8<TriScreenDrawerModes::StyleAdd>,
	&DrawSpan8<TriScreenDrawerModes::StyleShaded>,
	&DrawSpan8<TriScreenDrawerModes::StyleTranslucentStencil>,
	&DrawSpan8<TriScreenDrawerModes::StyleShadow>,
	&DrawSpan8<TriScreenDrawerModes::StyleSubtract>,
	&DrawSpan8<TriScreenDrawerModes::StyleAddStencil>,
	&DrawSpan8<TriScreenDrawerModes::StyleAddShaded>,
	&DrawSpan8<TriScreenDrawerModes::StyleOpaqueTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleSrcColorTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleNormalTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleStencilTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleTranslucentTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleAddTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleShadedTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleTranslucentStencilTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleShadowTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleSubtractTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleAddStencilTranslated>,
	&DrawSpan8<TriScreenDrawerModes::StyleAddShadedTranslated>
};

void(*ScreenTriangle::SpanDrawers32[])(int, int, int, const TriDrawTriangleArgs *) =
{
	&DrawSpan32<TriScreenDrawerModes::StyleOpaque>,
	&DrawSpan32<TriScreenDrawerModes::StyleSkycap>,
	&DrawSpan32<TriScreenDrawerModes::StyleFogBoundary>,
	&DrawSpan32<TriScreenDrawerModes::StyleSrcColor>,
	&DrawSpan32<TriScreenDrawerModes::StyleFill>,
	&DrawSpan32<TriScreenDrawerModes::StyleNormal>,
	&DrawSpan32<TriScreenDrawerModes::StyleFuzzy>,
	&DrawSpan32<TriScreenDrawerModes::StyleStencil>,
	&DrawSpan32<TriScreenDrawerModes::StyleTranslucent>,
	&DrawSpan32<TriScreenDrawerModes::StyleAdd>,
	&DrawSpan32<TriScreenDrawerModes::StyleShaded>,
	&DrawSpan32<TriScreenDrawerModes::StyleTranslucentStencil>,
	&DrawSpan32<TriScreenDrawerModes::StyleShadow>,
	&DrawSpan32<TriScreenDrawerModes::StyleSubtract>,
	&DrawSpan32<TriScreenDrawerModes::StyleAddStencil>,
	&DrawSpan32<TriScreenDrawerModes::StyleAddShaded>,
	&DrawSpan32<TriScreenDrawerModes::StyleOpaqueTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleSrcColorTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleNormalTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleStencilTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleTranslucentTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleAddTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleShadedTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleTranslucentStencilTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleShadowTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleSubtractTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleAddStencilTranslated>,
	&DrawSpan32<TriScreenDrawerModes::StyleAddShadedTranslated>
};

void(*ScreenTriangle::RectDrawers8[])(const void *, int, int, int, const RectDrawArgs *, PolyTriangleThreadData *) =
{
	&RectScreenDrawer8<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::TextureSampler>::Execute,         // TextureOpaque
	&RectScreenDrawer8<TriScreenDrawerModes::MaskedBlend, TriScreenDrawerModes::TextureSampler>::Execute,         // TextureMasked
	&RectScreenDrawer8<TriScreenDrawerModes::AddClampBlend, TriScreenDrawerModes::TextureSampler>::Execute,       // TextureAdd
	&RectScreenDrawer8<TriScreenDrawerModes::SubClampBlend, TriScreenDrawerModes::TextureSampler>::Execute,       // TextureSub
	&RectScreenDrawer8<TriScreenDrawerModes::RevSubClampBlend, TriScreenDrawerModes::TextureSampler>::Execute,    // TextureRevSub
	&RectScreenDrawer8<TriScreenDrawerModes::AddSrcColorBlend, TriScreenDrawerModes::TextureSampler>::Execute,    // TextureAddSrcColor
	&RectScreenDrawer8<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,      // TranslatedOpaque
	&RectScreenDrawer8<TriScreenDrawerModes::MaskedBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,      // TranslatedMasked
	&RectScreenDrawer8<TriScreenDrawerModes::AddClampBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,    // TranslatedAdd
	&RectScreenDrawer8<TriScreenDrawerModes::SubClampBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,    // TranslatedSub
	&RectScreenDrawer8<TriScreenDrawerModes::RevSubClampBlend, TriScreenDrawerModes::TranslatedSampler>::Execute, // TranslatedRevSub
	&RectScreenDrawer8<TriScreenDrawerModes::AddSrcColorBlend, TriScreenDrawerModes::TranslatedSampler>::Execute, // TranslatedAddSrcColor
	&RectScreenDrawer8<TriScreenDrawerModes::ShadedBlend, TriScreenDrawerModes::ShadedSampler>::Execute,          // Shaded
	&RectScreenDrawer8<TriScreenDrawerModes::AddClampShadedBlend, TriScreenDrawerModes::ShadedSampler>::Execute,  // AddShaded
	&RectScreenDrawer8<TriScreenDrawerModes::ShadedBlend, TriScreenDrawerModes::StencilSampler>::Execute,         // Stencil
	&RectScreenDrawer8<TriScreenDrawerModes::AddClampShadedBlend, TriScreenDrawerModes::StencilSampler>::Execute, // AddStencil
	&RectScreenDrawer8<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::FillSampler>::Execute,            // FillOpaque
	&RectScreenDrawer8<TriScreenDrawerModes::AddClampBlend, TriScreenDrawerModes::FillSampler>::Execute,          // FillAdd
	&RectScreenDrawer8<TriScreenDrawerModes::SubClampBlend, TriScreenDrawerModes::FillSampler>::Execute,          // FillSub
	&RectScreenDrawer8<TriScreenDrawerModes::RevSubClampBlend, TriScreenDrawerModes::FillSampler>::Execute,       // FillRevSub
	&RectScreenDrawer8<TriScreenDrawerModes::AddSrcColorBlend, TriScreenDrawerModes::FillSampler>::Execute,       // FillAddSrcColor
	&RectScreenDrawer8<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::SkycapSampler>::Execute,          // Skycap
	&RectScreenDrawer8<TriScreenDrawerModes::ShadedBlend, TriScreenDrawerModes::FuzzSampler>::Execute,            // Fuzz
	&RectScreenDrawer8<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::FogBoundarySampler>::Execute      // FogBoundary
};

void(*ScreenTriangle::RectDrawers32[])(const void *, int, int, int, const RectDrawArgs *, PolyTriangleThreadData *) =
{
	&RectScreenDrawer32<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::TextureSampler>::Execute,         // TextureOpaque
	&RectScreenDrawer32<TriScreenDrawerModes::MaskedBlend, TriScreenDrawerModes::TextureSampler>::Execute,         // TextureMasked
	&RectScreenDrawer32<TriScreenDrawerModes::AddClampBlend, TriScreenDrawerModes::TextureSampler>::Execute,       // TextureAdd
	&RectScreenDrawer32<TriScreenDrawerModes::SubClampBlend, TriScreenDrawerModes::TextureSampler>::Execute,       // TextureSub
	&RectScreenDrawer32<TriScreenDrawerModes::RevSubClampBlend, TriScreenDrawerModes::TextureSampler>::Execute,    // TextureRevSub
	&RectScreenDrawer32<TriScreenDrawerModes::AddSrcColorBlend, TriScreenDrawerModes::TextureSampler>::Execute,    // TextureAddSrcColor
	&RectScreenDrawer32<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,      // TranslatedOpaque
	&RectScreenDrawer32<TriScreenDrawerModes::MaskedBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,      // TranslatedMasked
	&RectScreenDrawer32<TriScreenDrawerModes::AddClampBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,    // TranslatedAdd
	&RectScreenDrawer32<TriScreenDrawerModes::SubClampBlend, TriScreenDrawerModes::TranslatedSampler>::Execute,    // TranslatedSub
	&RectScreenDrawer32<TriScreenDrawerModes::RevSubClampBlend, TriScreenDrawerModes::TranslatedSampler>::Execute, // TranslatedRevSub
	&RectScreenDrawer32<TriScreenDrawerModes::AddSrcColorBlend, TriScreenDrawerModes::TranslatedSampler>::Execute, // TranslatedAddSrcColor
	&RectScreenDrawer32<TriScreenDrawerModes::ShadedBlend, TriScreenDrawerModes::ShadedSampler>::Execute,          // Shaded
	&RectScreenDrawer32<TriScreenDrawerModes::AddClampShadedBlend, TriScreenDrawerModes::ShadedSampler>::Execute,  // AddShaded
	&RectScreenDrawer32<TriScreenDrawerModes::ShadedBlend, TriScreenDrawerModes::StencilSampler>::Execute,         // Stencil
	&RectScreenDrawer32<TriScreenDrawerModes::AddClampShadedBlend, TriScreenDrawerModes::StencilSampler>::Execute, // AddStencil
	&RectScreenDrawer32<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::FillSampler>::Execute,            // FillOpaque
	&RectScreenDrawer32<TriScreenDrawerModes::AddClampBlend, TriScreenDrawerModes::FillSampler>::Execute,          // FillAdd
	&RectScreenDrawer32<TriScreenDrawerModes::SubClampBlend, TriScreenDrawerModes::FillSampler>::Execute,          // FillSub
	&RectScreenDrawer32<TriScreenDrawerModes::RevSubClampBlend, TriScreenDrawerModes::FillSampler>::Execute,       // FillRevSub
	&RectScreenDrawer32<TriScreenDrawerModes::AddSrcColorBlend, TriScreenDrawerModes::FillSampler>::Execute,       // FillAddSrcColor
	&RectScreenDrawer32<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::SkycapSampler>::Execute,          // Skycap
	&RectScreenDrawer32<TriScreenDrawerModes::ShadedBlend, TriScreenDrawerModes::FuzzSampler>::Execute,            // Fuzz
	&RectScreenDrawer32<TriScreenDrawerModes::OpaqueBlend, TriScreenDrawerModes::FogBoundarySampler>::Execute,     // FogBoundary
};

int ScreenTriangle::FuzzStart = 0;
