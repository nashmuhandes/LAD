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
#include "x86.h"
#include <cmath>

#ifdef NO_SSE
static void WriteW(int y, int x0, int x1, const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread)
{
	float startX = x0 + (0.5f - args->v1->x);
	float startY = y + (0.5f - args->v1->y);

	float posW = args->v1->w + args->gradientX.W * startX + args->gradientY.W * startY;
	float stepW = args->gradientX.W;
	float* w = thread->scanline.W;
	for (int x = x0; x < x1; x++)
	{
		w[x] = 1.0f / posW;
		posW += stepW;
	}
}
#else
static void WriteW(int y, int x0, int x1, const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread)
{
	float startX = x0 + (0.5f - args->v1->x);
	float startY = y + (0.5f - args->v1->y);

	float posW = args->v1->w + args->gradientX.W * startX + args->gradientY.W * startY;
	float stepW = args->gradientX.W;
	float* w = thread->scanline.W;

	int ssecount = ((x1 - x0) & ~3);
	int sseend = x0 + ssecount;

	__m128 mstepW = _mm_set1_ps(stepW * 4.0f);
	__m128 mposW = _mm_setr_ps(posW, posW + stepW, posW + stepW + stepW, posW + stepW + stepW + stepW);

	for (int x = x0; x < sseend; x += 4)
	{
		// One Newton-Raphson iteration for 1/posW
		__m128 res = _mm_rcp_ps(mposW);
		__m128 muls = _mm_mul_ps(mposW, _mm_mul_ps(res, res));
		_mm_storeu_ps(w + x, _mm_sub_ps(_mm_add_ps(res, res), muls));
		mposW = _mm_add_ps(mposW, mstepW);
	}

	posW += ssecount * stepW;
	for (int x = sseend; x < x1; x++)
	{
		w[x] = 1.0f / posW;
		posW += stepW;
	}
}
#endif

static void WriteDynLightArray(int x0, int x1, PolyTriangleThreadData* thread)
{
	int num_lights = thread->numPolyLights;
	PolyLight* lights = thread->polyLights;

	float worldnormalX = thread->mainVertexShader.vWorldNormal.X;
	float worldnormalY = thread->mainVertexShader.vWorldNormal.Y;
	float worldnormalZ = thread->mainVertexShader.vWorldNormal.Z;

	uint32_t* dynlights = thread->scanline.dynlights;
	float* worldposX = thread->scanline.WorldX;
	float* worldposY = thread->scanline.WorldY;
	float* worldposZ = thread->scanline.WorldZ;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~3);
	sseend = x0 + ssecount;

	__m128 mworldnormalX = _mm_set1_ps(worldnormalX);
	__m128 mworldnormalY = _mm_set1_ps(worldnormalY);
	__m128 mworldnormalZ = _mm_set1_ps(worldnormalZ);

	for (int x = x0; x < sseend; x += 4)
	{
		__m128i litlo = _mm_setzero_si128();
		//__m128i litlo = _mm_shuffle_epi32(_mm_unpacklo_epi8(_mm_cvtsi32_si128(dynlightcolor), _mm_setzero_si128()), _MM_SHUFFLE(1, 0, 1, 0));
		__m128i lithi = litlo;

		for (int i = 0; i < num_lights; i++)
		{
			__m128 lightposX = _mm_set1_ps(lights[i].x);
			__m128 lightposY = _mm_set1_ps(lights[i].y);
			__m128 lightposZ = _mm_set1_ps(lights[i].z);
			__m128 light_radius = _mm_set1_ps(lights[i].radius);
			__m128i light_color = _mm_shuffle_epi32(_mm_unpacklo_epi8(_mm_cvtsi32_si128(lights[i].color), _mm_setzero_si128()), _MM_SHUFFLE(1, 0, 1, 0));

			__m128 is_attenuated = _mm_cmplt_ps(light_radius, _mm_setzero_ps());
			light_radius = _mm_andnot_ps(_mm_set1_ps(-0.0f), light_radius); // clear sign bit

			// L = light-pos
			// dist = sqrt(dot(L, L))
			// distance_attenuation = 1 - MIN(dist * (1/radius), 1)
			__m128 Lx = _mm_sub_ps(lightposX, _mm_loadu_ps(&worldposX[x]));
			__m128 Ly = _mm_sub_ps(lightposY, _mm_loadu_ps(&worldposY[x]));
			__m128 Lz = _mm_sub_ps(lightposZ, _mm_loadu_ps(&worldposZ[x]));
			__m128 dist2 = _mm_add_ps(_mm_mul_ps(Lx, Lx), _mm_add_ps(_mm_mul_ps(Ly, Ly), _mm_mul_ps(Lz, Lz)));
			__m128 rcp_dist = _mm_rsqrt_ps(dist2);
			__m128 dist = _mm_mul_ps(dist2, rcp_dist);
			__m128 distance_attenuation = _mm_sub_ps(_mm_set1_ps(256.0f), _mm_min_ps(_mm_mul_ps(dist, light_radius), _mm_set1_ps(256.0f)));

			// The simple light type
			__m128 simple_attenuation = distance_attenuation;

			// The point light type
			// diffuse = max(dot(N,normalize(L)),0) * attenuation
			Lx = _mm_mul_ps(Lx, rcp_dist);
			Ly = _mm_mul_ps(Ly, rcp_dist);
			Lz = _mm_mul_ps(Lz, rcp_dist);
			__m128 dotNL = _mm_add_ps(_mm_add_ps(_mm_mul_ps(mworldnormalX, Lx), _mm_mul_ps(mworldnormalY, Ly)), _mm_mul_ps(mworldnormalZ, Lz));
			__m128 point_attenuation = _mm_mul_ps(_mm_max_ps(dotNL, _mm_setzero_ps()), distance_attenuation);

			__m128i attenuation = _mm_cvtps_epi32(_mm_or_ps(_mm_and_ps(is_attenuated, point_attenuation), _mm_andnot_ps(is_attenuated, simple_attenuation)));

			attenuation = _mm_shufflehi_epi16(_mm_shufflelo_epi16(attenuation, _MM_SHUFFLE(2, 2, 0, 0)), _MM_SHUFFLE(2, 2, 0, 0));
			__m128i attenlo = _mm_shuffle_epi32(attenuation, _MM_SHUFFLE(1, 1, 0, 0));
			__m128i attenhi = _mm_shuffle_epi32(attenuation, _MM_SHUFFLE(3, 3, 2, 2));

			litlo = _mm_add_epi16(litlo, _mm_srli_epi16(_mm_mullo_epi16(light_color, attenlo), 8));
			lithi = _mm_add_epi16(lithi, _mm_srli_epi16(_mm_mullo_epi16(light_color, attenhi), 8));
		}

		_mm_storeu_si128((__m128i*)&dynlights[x], _mm_packus_epi16(litlo, lithi));
	}
#endif

	for (int x = x0; x < x1; x++)
	{
		uint32_t lit_r = 0;
		uint32_t lit_g = 0;
		uint32_t lit_b = 0;

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
			float Lx = lightposX - worldposX[x];
			float Ly = lightposY - worldposY[x];
			float Lz = lightposZ - worldposZ[x];
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

		lit_r = MIN<uint32_t>(lit_r, 255);
		lit_g = MIN<uint32_t>(lit_g, 255);
		lit_b = MIN<uint32_t>(lit_b, 255);
		dynlights[x] = MAKEARGB(255, lit_r, lit_g, lit_b);

		// Palette version:
		// dynlights[x] = RGB256k.All[((lit_r >> 2) << 12) | ((lit_g >> 2) << 6) | (lit_b >> 2)];
	}
}

static void WriteLightArray(int y, int x0, int x1, const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread)
{
	float startX = x0 + (0.5f - args->v1->x);
	float startY = y + (0.5f - args->v1->y);
	float posW = args->v1->w + args->gradientX.W * startX + args->gradientY.W * startY;
	float stepW = args->gradientX.W;

	float globVis = thread->mainVertexShader.Viewpoint->mGlobVis;

	uint32_t light = (int)(thread->PushConstants->uLightLevel * 255.0f);
	fixed_t shade = (fixed_t)((2.0f - (light + 12.0f) / 128.0f) * (float)FRACUNIT);
	fixed_t lightpos = (fixed_t)(globVis * posW * (float)FRACUNIT);
	fixed_t lightstep = (fixed_t)(globVis * stepW * (float)FRACUNIT);

	fixed_t maxvis = 24 * FRACUNIT / 32;
	fixed_t maxlight = 31 * FRACUNIT / 32;

	uint16_t *lightarray = thread->scanline.lightarray;

	fixed_t lightend = lightpos + lightstep * (x1 - x0);
	if (lightpos < maxvis && shade >= lightpos && shade - lightpos <= maxlight &&
		lightend < maxvis && shade >= lightend && shade - lightend <= maxlight)
	{
		//if (BitsPerPixel == 32)
		{
			lightpos += FRACUNIT - shade;
			for (int x = x0; x < x1; x++)
			{
				lightarray[x] = lightpos >> 8;
				lightpos += lightstep;
			}
		}
		/*else
		{
			lightpos = shade - lightpos;
			for (int x = x0; x < x1; x++)
			{
				lightarray[x] = (lightpos >> 3) & 0xffffff00;
				lightpos -= lightstep;
			}
		}*/
	}
	else
	{
		//if (BitsPerPixel == 32)
		{
			for (int x = x0; x < x1; x++)
			{
				lightarray[x] = (FRACUNIT - clamp<fixed_t>(shade - MIN(maxvis, lightpos), 0, maxlight)) >> 8;
				lightpos += lightstep;
			}
		}
		/*else
		{
			for (int x = x0; x < x1; x++)
			{
				lightarray[x] = (clamp<fixed_t>(shade - MIN(maxvis, lightpos), 0, maxlight) >> 3) & 0xffffff00;
				lightpos += lightstep;
			}
		}*/
	}
}

#ifdef NO_SSE
static void WriteVarying(float pos, float step, int x0, int x1, const float* w, float* varying)
{
	for (int x = x0; x < x1; x++)
	{
		varying[x] = pos * w[x];
		pos += step;
	}
}
#else
static void WriteVarying(float pos, float step, int x0, int x1, const float* w, float* varying)
{
	int ssecount = ((x1 - x0) & ~3);
	int sseend = x0 + ssecount;

	__m128 mstep = _mm_set1_ps(step * 4.0f);
	__m128 mpos = _mm_setr_ps(pos, pos + step, pos + step + step, pos + step + step + step);

	for (int x = x0; x < sseend; x += 4)
	{
		_mm_storeu_ps(varying + x, _mm_mul_ps(mpos, _mm_loadu_ps(w + x)));
		mpos = _mm_add_ps(mpos, mstep);
	}

	pos += ssecount * step;
	for (int x = sseend; x < x1; x++)
	{
		varying[x] = pos * w[x];
		pos += step;
	}
}
#endif

#ifdef NO_SSE
static void WriteVaryingWrap(float pos, float step, int x0, int x1, const float* w, uint16_t* varying)
{
	for (int x = x0; x < x1; x++)
	{
		float value = pos * w[x];
		value = value - std::floor(value);
		varying[x] = static_cast<uint32_t>(static_cast<int32_t>(value * static_cast<float>(0x1000'0000)) << 4) >> 16;
		pos += step;
	}
}
#else
static void WriteVaryingWrap(float pos, float step, int x0, int x1, const float* w, uint16_t* varying)
{
	int ssecount = ((x1 - x0) & ~3);
	int sseend = x0 + ssecount;

	__m128 mstep = _mm_set1_ps(step * 4.0f);
	__m128 mpos = _mm_setr_ps(pos, pos + step, pos + step + step, pos + step + step + step);

	for (int x = x0; x < sseend; x += 4)
	{
		__m128 value = _mm_mul_ps(mpos, _mm_loadu_ps(w + x));
		__m128 f = value;
		__m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(f));
		__m128 r = _mm_sub_ps(t, _mm_and_ps(_mm_cmplt_ps(f, t), _mm_set1_ps(1.0f)));
		value = _mm_sub_ps(f, r);

		__m128i ivalue = _mm_srli_epi32(_mm_slli_epi32(_mm_cvttps_epi32(_mm_mul_ps(value, _mm_set1_ps(static_cast<float>(0x1000'0000)))), 4), 17);
		_mm_storel_epi64((__m128i*)(varying + x), _mm_slli_epi16(_mm_packs_epi32(ivalue, ivalue), 1));
		mpos = _mm_add_ps(mpos, mstep);
	}

	pos += ssecount * step;
	for (int x = sseend; x < x1; x++)
	{
		float value = pos * w[x];
		__m128 f = _mm_set_ss(value);
		__m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(f));
		__m128 r = _mm_sub_ss(t, _mm_and_ps(_mm_cmplt_ps(f, t), _mm_set_ss(1.0f)));
		value = _mm_cvtss_f32(_mm_sub_ss(f, r));

		varying[x] = static_cast<uint32_t>(static_cast<int32_t>(value * static_cast<float>(0x1000'0000)) << 4) >> 16;
		pos += step;
	}
}
#endif

#ifdef NO_SSE
static void WriteVaryingColor(float pos, float step, int x0, int x1, const float* w, uint8_t* varying)
{
	for (int x = x0; x < x1; x++)
	{
		varying[x] = clamp(static_cast<int>(pos * w[x] * 255.0f), 0, 255);
		pos += step;
	}
}
#else
static void WriteVaryingColor(float pos, float step, int x0, int x1, const float* w, uint8_t* varying)
{
	int ssecount = ((x1 - x0) & ~3);
	int sseend = x0 + ssecount;

	__m128 mstep = _mm_set1_ps(step * 4.0f);
	__m128 mpos = _mm_setr_ps(pos, pos + step, pos + step + step, pos + step + step + step);

	for (int x = x0; x < sseend; x += 4)
	{
		__m128i value = _mm_cvttps_epi32(_mm_mul_ps(_mm_mul_ps(mpos, _mm_loadu_ps(w + x)), _mm_set1_ps(255.0f)));
		value = _mm_packs_epi32(value, value);
		value = _mm_packus_epi16(value, value);
		*(uint32_t*)(varying + x) = _mm_cvtsi128_si32(value);
		mpos = _mm_add_ps(mpos, mstep);
	}

	pos += ssecount * step;
	for (int x = sseend; x < x1; x++)
	{
		varying[x] = clamp(static_cast<int>(pos * w[x] * 255.0f), 0, 255);
		pos += step;
	}
}
#endif

static void WriteVaryings(int y, int x0, int x1, const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread)
{
	float startX = x0 + (0.5f - args->v1->x);
	float startY = y + (0.5f - args->v1->y);

	WriteVaryingWrap(args->v1->u * args->v1->w + args->gradientX.U * startX + args->gradientY.U * startY, args->gradientX.U, x0, x1, thread->scanline.W, thread->scanline.U);
	WriteVaryingWrap(args->v1->v * args->v1->w + args->gradientX.V * startX + args->gradientY.V * startY, args->gradientX.V, x0, x1, thread->scanline.W, thread->scanline.V);
	WriteVarying(args->v1->worldX * args->v1->w + args->gradientX.WorldX * startX + args->gradientY.WorldX * startY, args->gradientX.WorldX, x0, x1, thread->scanline.W, thread->scanline.WorldX);
	WriteVarying(args->v1->worldY * args->v1->w + args->gradientX.WorldY * startX + args->gradientY.WorldY * startY, args->gradientX.WorldY, x0, x1, thread->scanline.W, thread->scanline.WorldY);
	WriteVarying(args->v1->worldZ * args->v1->w + args->gradientX.WorldZ * startX + args->gradientY.WorldZ * startY, args->gradientX.WorldZ, x0, x1, thread->scanline.W, thread->scanline.WorldZ);
	WriteVarying(args->v1->gradientdistZ * args->v1->w + args->gradientX.GradientdistZ * startX + args->gradientY.GradientdistZ * startY, args->gradientX.GradientdistZ, x0, x1, thread->scanline.W, thread->scanline.GradientdistZ);
	WriteVaryingColor(args->v1->a * args->v1->w + args->gradientX.A * startX + args->gradientY.A * startY, args->gradientX.A, x0, x1, thread->scanline.W, thread->scanline.vColorA);
	WriteVaryingColor(args->v1->r * args->v1->w + args->gradientX.R * startX + args->gradientY.R * startY, args->gradientX.R, x0, x1, thread->scanline.W, thread->scanline.vColorR);
	WriteVaryingColor(args->v1->g * args->v1->w + args->gradientX.G * startX + args->gradientY.G * startY, args->gradientX.G, x0, x1, thread->scanline.W, thread->scanline.vColorG);
	WriteVaryingColor(args->v1->b * args->v1->w + args->gradientX.B * startX + args->gradientY.B * startY, args->gradientX.B, x0, x1, thread->scanline.W, thread->scanline.vColorB);
}

static const int shiftTable[] = {
	0, 0, 0, 0, // STYLEALPHA_Zero
	0, 0, 0, 0, // STYLEALPHA_One
	24, 24, 24, 24, // STYLEALPHA_Src
	24, 24, 24, 24, // STYLEALPHA_InvSrc
	24, 16, 8, 0, // STYLEALPHA_SrcCol
	24, 16, 8, 0, // STYLEALPHA_InvSrcCol
	24, 16, 8, 0, // STYLEALPHA_DstCol
	24, 16, 8, 0 // STYLEALPHA_InvDstCol
};

#if 1 //#ifndef USE_AVX2
template<typename OptT>
static void BlendColor(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	FRenderStyle style = thread->RenderStyle;

	bool invsrc = style.SrcAlpha & 1;
	bool invdst = style.DestAlpha & 1;

	const int* shiftsrc = shiftTable + (style.SrcAlpha << 2);
	const int* shiftdst = shiftTable + (style.DestAlpha << 2);

	uint32_t* dest = (uint32_t*)thread->dest;
	uint32_t* line = dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int srcSelect = style.SrcAlpha <= STYLEALPHA_One ? 0 : (style.SrcAlpha >= STYLEALPHA_DstCol ? 1 : 2);
	int dstSelect = style.DestAlpha <= STYLEALPHA_One ? 0 : (style.DestAlpha >= STYLEALPHA_DstCol ? 1 : 2);

	uint32_t inputs[3];
	inputs[0] = 0;

	for (int x = x0; x < x1; x++)
	{
		inputs[1] = line[x];
		inputs[2] = fragcolor[x];

		uint32_t srcinput = inputs[srcSelect];
		uint32_t dstinput = inputs[dstSelect];

		uint32_t out[4];
		for (int i = 0; i < 4; i++)
		{
			// Grab component for scale factors
			int32_t src = (srcinput >> shiftsrc[i]) & 0xff;
			int32_t dst = (dstinput >> shiftdst[i]) & 0xff;

			// Inverse if needed
			if (invsrc) src = 0xff - src;
			if (invdst) dst = 0xff - dst;

			// Rescale 0-255 to 0-256
			src = src + (src >> 7);
			dst = dst + (dst >> 7);

			// Multiply with input
			src = src * ((inputs[2] >> (24 - (i << 3))) & 0xff);
			dst = dst * ((inputs[1] >> (24 - (i << 3))) & 0xff);

			// Apply blend operator
			int32_t val;
			if (OptT::Flags & SWBLEND_Sub)
			{
				val = src - dst;
			}
			else if (OptT::Flags & SWBLEND_RevSub)
			{
				val = dst - src;
			}
			else
			{
				val = src + dst;
			}
			out[i] = clamp((val + 127) >> 8, 0, 255);
		}

		line[x] = MAKEARGB(out[0], out[1], out[2], out[3]);
	}
}
#else
template<typename OptT>
static void BlendColor(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	FRenderStyle style = thread->RenderStyle;

	bool invsrc = style.SrcAlpha & 1;
	bool invdst = style.DestAlpha & 1;

	__m128i shiftsrc = _mm_loadu_si128((const __m128i*)(shiftTable + (style.SrcAlpha << 2)));
	__m128i shiftdst = _mm_loadu_si128((const __m128i*)(shiftTable + (style.DestAlpha << 2)));

	uint32_t* dest = (uint32_t*)thread->dest;
	uint32_t* line = dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int srcSelect = style.SrcAlpha <= STYLEALPHA_One ? 0 : (style.SrcAlpha >= STYLEALPHA_DstCol ? 1 : 2);
	int dstSelect = style.DestAlpha <= STYLEALPHA_One ? 0 : (style.DestAlpha >= STYLEALPHA_DstCol ? 1 : 2);

	uint32_t inputs[3];
	inputs[0] = 0;

	__m128i shiftmul = _mm_set_epi32(24, 16, 8, 0);

	for (int x = x0; x < x1; x++)
	{
		inputs[1] = line[x];
		inputs[2] = fragcolor[x];

		__m128i srcinput = _mm_set1_epi32(inputs[srcSelect]);
		__m128i dstinput = _mm_set1_epi32(inputs[dstSelect]);

		// Grab component for scale factors
		__m128i src = _mm_and_si128(_mm_srlv_epi32(srcinput, shiftsrc), _mm_set1_epi32(0xff));
		__m128i dst = _mm_and_si128(_mm_srlv_epi32(dstinput, shiftdst), _mm_set1_epi32(0xff));

		// Inverse if needed
		if (invsrc) src = _mm_sub_epi32(_mm_set1_epi32(0xff), src);
		if (invdst) dst = _mm_sub_epi32(_mm_set1_epi32(0xff), dst);

		// Rescale 0-255 to 0-256
		src = _mm_add_epi32(src, _mm_srli_epi32(src, 7));
		dst = _mm_add_epi32(dst, _mm_srli_epi32(dst, 7));

		// Multiply with input
		__m128i mulsrc = _mm_and_si128(_mm_srlv_epi32(_mm_set1_epi32(inputs[2]), shiftmul), _mm_set1_epi32(0xff));
		__m128i muldst = _mm_and_si128(_mm_srlv_epi32(_mm_set1_epi32(inputs[1]), shiftmul), _mm_set1_epi32(0xff));
		__m128i mulresult = _mm_mullo_epi16(_mm_packs_epi32(src, dst), _mm_packs_epi32(mulsrc, muldst));
		src = _mm_unpacklo_epi16(mulresult, _mm_setzero_si128());
		dst = _mm_unpackhi_epi16(mulresult, _mm_setzero_si128());

		// Apply blend operator
		__m128i val;
		if (OptT::Flags & SWBLEND_Sub)
		{
			val = _mm_sub_epi32(src, dst);
		}
		else if (OptT::Flags & SWBLEND_RevSub)
		{
			val = _mm_sub_epi32(dst, src);
		}
		else
		{
			val = _mm_add_epi32(src, dst);
		}

		__m128i out = _mm_srli_epi32(_mm_add_epi32(val, _mm_set1_epi32(127)), 8);
		out = _mm_packs_epi32(out, out);
		out = _mm_packus_epi16(out, out);
		line[x] = _mm_cvtsi128_si32(out);
	}
}
#endif

#ifdef NO_SSE
static void BlendColorOpaque(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* dest = (uint32_t*)thread->dest;
	uint32_t* line = dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	memcpy(line + x0, fragcolor + x0, (x1 - x0) * sizeof(uint32_t));
}
#else
static void BlendColorOpaque(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* dest = (uint32_t*)thread->dest;
	uint32_t* line = dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int ssecount = ((x1 - x0) & ~3);
	int sseend = x0 + ssecount;

	for (int x = x0; x < sseend; x += 4)
	{
		__m128i v = _mm_loadu_si128((__m128i*) & fragcolor[x]);
		_mm_storeu_si128((__m128i*) & line[x], v);
	}

	for (int x = sseend; x < x1; x++)
	{
		line[x] = fragcolor[x];
	}
}
#endif

static void BlendColorAdd_Src_InvSrc(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* line = (uint32_t*)thread->dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~1);
	sseend = x0 + ssecount;
	for (int x = x0; x < sseend; x += 2)
	{
		__m128i dst = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)&line[x]), _mm_setzero_si128());
		__m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)&fragcolor[x]), _mm_setzero_si128());

		__m128i srcscale = _mm_shufflehi_epi16(_mm_shufflelo_epi16(src, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		srcscale = _mm_add_epi16(srcscale, _mm_srli_epi16(srcscale, 7));
		__m128i dstscale = _mm_sub_epi16(_mm_set1_epi16(256), srcscale);

		__m128i out = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(src, srcscale), _mm_mullo_epi16(dst, dstscale)), _mm_set1_epi16(127)), 8);
		_mm_storel_epi64((__m128i*)&line[x], _mm_packus_epi16(out, out));
	}
#endif

	for (int x = sseend; x < x1; x++)
	{
		uint32_t dst = line[x];
		uint32_t src = fragcolor[x];

		uint32_t srcscale = APART(src);
		srcscale += srcscale >> 7;
		uint32_t dstscale = 256 - srcscale;

		uint32_t a = ((APART(src) * srcscale + APART(dst) * dstscale) + 127) >> 8;
		uint32_t r = ((RPART(src) * srcscale + RPART(dst) * dstscale) + 127) >> 8;
		uint32_t g = ((GPART(src) * srcscale + GPART(dst) * dstscale) + 127) >> 8;
		uint32_t b = ((BPART(src) * srcscale + BPART(dst) * dstscale) + 127) >> 8;

		line[x] = MAKEARGB(a, r, g, b);
	}
}

static void BlendColorAdd_SrcCol_InvSrcCol(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* line = (uint32_t*)thread->dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~1);
	sseend = x0 + ssecount;
	for (int x = x0; x < sseend; x += 2)
	{
		__m128i dst = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & line[x]), _mm_setzero_si128());
		__m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & fragcolor[x]), _mm_setzero_si128());

		__m128i srcscale = src;
		srcscale = _mm_add_epi16(srcscale, _mm_srli_epi16(srcscale, 7));
		__m128i dstscale = _mm_sub_epi16(_mm_set1_epi16(256), srcscale);

		__m128i out = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(_mm_mullo_epi16(src, srcscale), _mm_mullo_epi16(dst, dstscale)), _mm_set1_epi16(127)), 8);
		_mm_storel_epi64((__m128i*) & line[x], _mm_packus_epi16(out, out));
	}
#endif

	for (int x = sseend; x < x1; x++)
	{
		uint32_t dst = line[x];
		uint32_t src = fragcolor[x];

		uint32_t srcscale_a = APART(src);
		uint32_t srcscale_r = RPART(src);
		uint32_t srcscale_g = GPART(src);
		uint32_t srcscale_b = BPART(src);
		srcscale_a += srcscale_a >> 7;
		srcscale_r += srcscale_r >> 7;
		srcscale_g += srcscale_g >> 7;
		srcscale_b += srcscale_b >> 7;
		uint32_t dstscale_a = 256 - srcscale_a;
		uint32_t dstscale_r = 256 - srcscale_r;
		uint32_t dstscale_g = 256 - srcscale_g;
		uint32_t dstscale_b = 256 - srcscale_b;

		uint32_t a = ((APART(src) * srcscale_a + APART(dst) * dstscale_a) + 127) >> 8;
		uint32_t r = ((RPART(src) * srcscale_r + RPART(dst) * dstscale_r) + 127) >> 8;
		uint32_t g = ((GPART(src) * srcscale_g + GPART(dst) * dstscale_g) + 127) >> 8;
		uint32_t b = ((BPART(src) * srcscale_b + BPART(dst) * dstscale_b) + 127) >> 8;

		line[x] = MAKEARGB(a, r, g, b);
	}
}

static void BlendColorAdd_Src_One(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* line = (uint32_t*)thread->dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~1);
	sseend = x0 + ssecount;
	for (int x = x0; x < sseend; x += 2)
	{
		__m128i dst = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & line[x]), _mm_setzero_si128());
		__m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & fragcolor[x]), _mm_setzero_si128());

		__m128i srcscale = _mm_shufflehi_epi16(_mm_shufflelo_epi16(src, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		srcscale = _mm_add_epi16(srcscale, _mm_srli_epi16(srcscale, 7));

		__m128i out = _mm_add_epi16(_mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(src, srcscale), _mm_set1_epi16(127)), 8), dst);
		_mm_storel_epi64((__m128i*) & line[x], _mm_packus_epi16(out, out));
	}
#endif

	for (int x = sseend; x < x1; x++)
	{
		uint32_t dst = line[x];
		uint32_t src = fragcolor[x];

		uint32_t srcscale = APART(src);
		srcscale += srcscale >> 7;

		uint32_t a = MIN<int32_t>((((APART(src) * srcscale) + 127) >> 8) + APART(dst), 255);
		uint32_t r = MIN<int32_t>((((RPART(src) * srcscale) + 127) >> 8) + RPART(dst), 255);
		uint32_t g = MIN<int32_t>((((GPART(src) * srcscale) + 127) >> 8) + GPART(dst), 255);
		uint32_t b = MIN<int32_t>((((BPART(src) * srcscale) + 127) >> 8) + BPART(dst), 255);

		line[x] = MAKEARGB(a, r, g, b);
	}
}

static void BlendColorAdd_SrcCol_One(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* line = (uint32_t*)thread->dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~1);
	sseend = x0 + ssecount;
	for (int x = x0; x < sseend; x += 2)
	{
		__m128i dst = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & line[x]), _mm_setzero_si128());
		__m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & fragcolor[x]), _mm_setzero_si128());

		__m128i srcscale = src;
		srcscale = _mm_add_epi16(srcscale, _mm_srli_epi16(srcscale, 7));

		__m128i out = _mm_add_epi16(_mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(src, srcscale), _mm_set1_epi16(127)), 8), dst);
		_mm_storel_epi64((__m128i*) & line[x], _mm_packus_epi16(out, out));
	}
#endif

	for (int x = sseend; x < x1; x++)
	{
		uint32_t dst = line[x];
		uint32_t src = fragcolor[x];

		uint32_t srcscale_a = APART(src);
		uint32_t srcscale_r = RPART(src);
		uint32_t srcscale_g = GPART(src);
		uint32_t srcscale_b = BPART(src);
		srcscale_a += srcscale_a >> 7;
		srcscale_r += srcscale_r >> 7;
		srcscale_g += srcscale_g >> 7;
		srcscale_b += srcscale_b >> 7;

		uint32_t a = MIN<int32_t>((((APART(src) * srcscale_a) + 127) >> 8) + APART(dst), 255);
		uint32_t r = MIN<int32_t>((((RPART(src) * srcscale_r) + 127) >> 8) + RPART(dst), 255);
		uint32_t g = MIN<int32_t>((((GPART(src) * srcscale_g) + 127) >> 8) + GPART(dst), 255);
		uint32_t b = MIN<int32_t>((((BPART(src) * srcscale_b) + 127) >> 8) + BPART(dst), 255);

		line[x] = MAKEARGB(a, r, g, b);
	}
}

static void BlendColorAdd_DstCol_Zero(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* line = (uint32_t*)thread->dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~1);
	sseend = x0 + ssecount;
	for (int x = x0; x < sseend; x += 2)
	{
		__m128i dst = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & line[x]), _mm_setzero_si128());
		__m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & fragcolor[x]), _mm_setzero_si128());

		__m128i srcscale = dst;
		srcscale = _mm_add_epi16(srcscale, _mm_srli_epi16(srcscale, 7));

		__m128i out = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(src, srcscale), _mm_set1_epi16(127)), 8);
		_mm_storel_epi64((__m128i*) & line[x], _mm_packus_epi16(out, out));
	}
#endif

	for (int x = sseend; x < x1; x++)
	{
		uint32_t dst = line[x];
		uint32_t src = fragcolor[x];

		uint32_t srcscale_a = APART(dst);
		uint32_t srcscale_r = RPART(dst);
		uint32_t srcscale_g = GPART(dst);
		uint32_t srcscale_b = BPART(dst);
		srcscale_a += srcscale_a >> 7;
		srcscale_r += srcscale_r >> 7;
		srcscale_g += srcscale_g >> 7;
		srcscale_b += srcscale_b >> 7;

		uint32_t a = (((APART(src) * srcscale_a) + 127) >> 8);
		uint32_t r = (((RPART(src) * srcscale_r) + 127) >> 8);
		uint32_t g = (((GPART(src) * srcscale_g) + 127) >> 8);
		uint32_t b = (((BPART(src) * srcscale_b) + 127) >> 8);

		line[x] = MAKEARGB(a, r, g, b);
	}
}

static void BlendColorAdd_InvDstCol_Zero(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* line = (uint32_t*)thread->dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~1);
	sseend = x0 + ssecount;
	for (int x = x0; x < sseend; x += 2)
	{
		__m128i dst = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & line[x]), _mm_setzero_si128());
		__m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & fragcolor[x]), _mm_setzero_si128());

		__m128i srcscale = _mm_sub_epi16(_mm_set1_epi16(255), dst);
		srcscale = _mm_add_epi16(srcscale, _mm_srli_epi16(srcscale, 7));

		__m128i out = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(src, srcscale), _mm_set1_epi16(127)), 8);
		_mm_storel_epi64((__m128i*) & line[x], _mm_packus_epi16(out, out));
	}
#endif

	for (int x = sseend; x < x1; x++)
	{
		uint32_t dst = line[x];
		uint32_t src = fragcolor[x];

		uint32_t srcscale_a = 255 - APART(dst);
		uint32_t srcscale_r = 255 - RPART(dst);
		uint32_t srcscale_g = 255 - GPART(dst);
		uint32_t srcscale_b = 255 - BPART(dst);
		srcscale_a += srcscale_a >> 7;
		srcscale_r += srcscale_r >> 7;
		srcscale_g += srcscale_g >> 7;
		srcscale_b += srcscale_b >> 7;

		uint32_t a = (((APART(src) * srcscale_a) + 127) >> 8);
		uint32_t r = (((RPART(src) * srcscale_r) + 127) >> 8);
		uint32_t g = (((GPART(src) * srcscale_g) + 127) >> 8);
		uint32_t b = (((BPART(src) * srcscale_b) + 127) >> 8);

		line[x] = MAKEARGB(a, r, g, b);
	}
}

static void BlendColorRevSub_Src_One(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* line = (uint32_t*)thread->dest + y * (ptrdiff_t)thread->dest_pitch;
	uint32_t* fragcolor = thread->scanline.FragColor;

	int sseend = x0;

#ifndef NO_SSE
	int ssecount = ((x1 - x0) & ~1);
	sseend = x0 + ssecount;
	for (int x = x0; x < sseend; x += 2)
	{
		__m128i dst = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & line[x]), _mm_setzero_si128());
		__m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*) & fragcolor[x]), _mm_setzero_si128());

		__m128i srcscale = _mm_shufflehi_epi16(_mm_shufflelo_epi16(src, _MM_SHUFFLE(3, 3, 3, 3)), _MM_SHUFFLE(3, 3, 3, 3));
		srcscale = _mm_add_epi16(srcscale, _mm_srli_epi16(srcscale, 7));

		__m128i out = _mm_sub_epi16(dst, _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(src, srcscale), _mm_set1_epi16(127)), 8));
		_mm_storel_epi64((__m128i*) & line[x], _mm_packus_epi16(out, out));
	}
#endif

	for (int x = sseend; x < x1; x++)
	{
		uint32_t dst = line[x];
		uint32_t src = fragcolor[x];

		uint32_t srcscale = APART(src);
		srcscale += srcscale >> 7;

		uint32_t a = MAX<int32_t>(APART(dst) - (((APART(src) * srcscale) + 127) >> 8), 0);
		uint32_t r = MAX<int32_t>(RPART(dst) - (((RPART(src) * srcscale) + 127) >> 8), 0);
		uint32_t g = MAX<int32_t>(GPART(dst) - (((GPART(src) * srcscale) + 127) >> 8), 0);
		uint32_t b = MAX<int32_t>(BPART(dst) - (((BPART(src) * srcscale) + 127) >> 8), 0);

		line[x] = MAKEARGB(a, r, g, b);
	}
}

static void SelectWriteColorFunc(PolyTriangleThreadData* thread)
{
	FRenderStyle style = thread->RenderStyle;
	if (style.BlendOp == STYLEOP_Add)
	{
		if (style.SrcAlpha == STYLEALPHA_One && style.DestAlpha == STYLEALPHA_Zero)
		{
			thread->WriteColorFunc = &BlendColorOpaque;
		}
		else if (style.SrcAlpha == STYLEALPHA_Src && style.DestAlpha == STYLEALPHA_InvSrc)
		{
			thread->WriteColorFunc = &BlendColorAdd_Src_InvSrc;
		}
		else if (style.SrcAlpha == STYLEALPHA_SrcCol && style.DestAlpha == STYLEALPHA_InvSrcCol)
		{
			thread->WriteColorFunc = &BlendColorAdd_SrcCol_InvSrcCol;
		}
		else if (style.SrcAlpha == STYLEALPHA_Src && style.DestAlpha == STYLEALPHA_One)
		{
			thread->WriteColorFunc = &BlendColorAdd_Src_One;
		}
		else if (style.SrcAlpha == STYLEALPHA_SrcCol && style.DestAlpha == STYLEALPHA_One)
		{
			thread->WriteColorFunc = &BlendColorAdd_SrcCol_One;
		}
		else if (style.SrcAlpha == STYLEALPHA_DstCol && style.DestAlpha == STYLEALPHA_Zero)
		{
			thread->WriteColorFunc = &BlendColorAdd_DstCol_Zero;
		}
		else if (style.SrcAlpha == STYLEALPHA_InvDstCol && style.DestAlpha == STYLEALPHA_Zero)
		{
			thread->WriteColorFunc = &BlendColorAdd_InvDstCol_Zero;
		}
		else
		{
			thread->WriteColorFunc = &BlendColor<BlendColorOpt_Add>;
		}
	}
	else if (style.BlendOp == STYLEOP_Sub)
	{
		thread->WriteColorFunc = &BlendColor<BlendColorOpt_Sub>;
	}
	else // if (style.BlendOp == STYLEOP_RevSub)
	{
		if (style.SrcAlpha == STYLEALPHA_Src && style.DestAlpha == STYLEALPHA_One)
		{
			thread->WriteColorFunc = &BlendColorRevSub_Src_One;
		}
		else
		{
			thread->WriteColorFunc = &BlendColor<BlendColorOpt_RevSub>;
		}
	}
}

static void WriteDepth(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	size_t pitch = thread->depthstencil->Width();
	float* line = thread->depthstencil->DepthValues() + pitch * y;
	float* w = thread->scanline.W;
	for (int x = x0; x < x1; x++)
	{
		line[x] = w[x];
	}
}

static void WriteStencil(int y, int x0, int x1, PolyTriangleThreadData* thread)
{
	size_t pitch = thread->depthstencil->Width();
	uint8_t* line = thread->depthstencil->StencilValues() + pitch * y;
	uint8_t value = thread->StencilWriteValue;
	for (int x = x0; x < x1; x++)
	{
		line[x] = value;
	}
}

static uint32_t SampleTexture(uint32_t u, uint32_t v, const void* texPixels, int texWidth, int texHeight, bool texBgra)
{
	int texelX = (u * texWidth) >> 16;
	int texelY = (v * texHeight) >> 16;
	int texelOffset = texelX + texelY * texWidth;
	if (texBgra)
	{
		return static_cast<const uint32_t*>(texPixels)[texelOffset];
	}
	else
	{
		uint32_t c = static_cast<const uint8_t*>(texPixels)[texelOffset];
		return (c << 16) | 0xff000000;
	}
}

static void EffectFogBoundary(int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* fragcolor = thread->scanline.FragColor;
	for (int x = x0; x < x1; x++)
	{
		/*float fogdist = pixelpos.w;
		float fogfactor = exp2(uFogDensity * fogdist);
		FragColor = vec4(uFogColor.rgb, 1.0 - fogfactor);*/
		fragcolor[x] = 0;
	}
}

static void EffectBurn(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;

	int tex2Width = thread->textures[1].width;
	int tex2Height = thread->textures[1].height;
	const void* tex2Pixels = thread->textures[1].pixels;
	bool tex2Bgra = thread->textures[1].bgra;

	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;
	for (int x = x0; x < x1; x++)
	{
		uint32_t frag_r = thread->scanline.vColorR[x];
		uint32_t frag_g = thread->scanline.vColorG[x];
		uint32_t frag_b = thread->scanline.vColorB[x];
		uint32_t frag_a = thread->scanline.vColorA[x];
		frag_r += frag_r >> 7; // 255 -> 256
		frag_g += frag_g >> 7; // 255 -> 256
		frag_b += frag_b >> 7; // 255 -> 256
		frag_a += frag_a >> 7; // 255 -> 256

		uint32_t t1 = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		uint32_t t2 = SampleTexture(u[x], 0xffff - v[x], tex2Pixels, tex2Width, tex2Height, tex2Bgra);

		uint32_t r = (frag_r * RPART(t1)) >> 8;
		uint32_t g = (frag_g * GPART(t1)) >> 8;
		uint32_t b = (frag_b * BPART(t1)) >> 8;
		uint32_t a = (frag_a * APART(t2)) >> 8;

		fragcolor[x] = MAKEARGB(a, r, g, b);
	}
}

static void EffectStencil(int x0, int x1, PolyTriangleThreadData* thread)
{
	/*for (int x = x0; x < x1; x++)
	{
		fragcolor[x] = 0x00ffffff;
	}*/
}

static void FuncPaletted(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	const uint32_t* lut = (const uint32_t*)thread->textures[1].pixels;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		fragcolor[x] = lut[RPART(SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra))] | 0xff000000;
	}
}

static void FuncNoTexture(int x0, int x1, PolyTriangleThreadData* thread)
{
	auto& streamdata = thread->mainVertexShader.Data;
	uint32_t a = (int)(streamdata.uObjectColor.a * 255.0f);
	uint32_t r = (int)(streamdata.uObjectColor.r * 255.0f);
	uint32_t g = (int)(streamdata.uObjectColor.g * 255.0f);
	uint32_t b = (int)(streamdata.uObjectColor.b * 255.0f);
	uint32_t texel = MAKEARGB(a, r, g, b);

	if (streamdata.uDesaturationFactor > 0.0f)
	{
		uint32_t t = (int)(streamdata.uDesaturationFactor * 256.0f);
		uint32_t inv_t = 256 - t;
		uint32_t gray = (RPART(texel) * 77 + GPART(texel) * 143 + BPART(texel) * 37) >> 8;
		texel = MAKEARGB(
			APART(texel),
			(RPART(texel) * inv_t + gray * t + 127) >> 8,
			(GPART(texel) * inv_t + gray * t + 127) >> 8,
			(BPART(texel) * inv_t + gray * t + 127) >> 8);
	}

	uint32_t* fragcolor = thread->scanline.FragColor;
	for (int x = x0; x < x1; x++)
	{
		fragcolor[x] = texel;
	}
}

static void FuncNormal(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		fragcolor[x] = texel;
	}
}

static void FuncNormal_Stencil(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		fragcolor[x] = texel | 0x00ffffff;
	}
}

static void FuncNormal_Opaque(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		fragcolor[x] = texel | 0xff000000;
	}
}

static void FuncNormal_Inverse(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		fragcolor[x] = MAKEARGB(APART(texel), 0xff - RPART(texel), 0xff - BPART(texel), 0xff - GPART(texel));
	}
}

static void FuncNormal_AlphaTexture(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		uint32_t gray = (RPART(texel) * 77 + GPART(texel) * 143 + BPART(texel) * 37) >> 8;
		uint32_t alpha = APART(texel);
		alpha += alpha >> 7;
		alpha = (alpha * gray + 127) >> 8;
		texel = (alpha << 24) | 0x00ffffff;
		fragcolor[x] = texel;
	}
}

static void FuncNormal_ClampY(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		fragcolor[x] = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		if (v[x] < 0.0 || v[x] > 1.0)
			fragcolor[x] &= 0x00ffffff;
	}
}

static void FuncNormal_InvertOpaque(int x0, int x1, PolyTriangleThreadData* thread)
{
	int texWidth = thread->textures[0].width;
	int texHeight = thread->textures[0].height;
	const void* texPixels = thread->textures[0].pixels;
	bool texBgra = thread->textures[0].bgra;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint16_t* u = thread->scanline.U;
	uint16_t* v = thread->scanline.V;

	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = SampleTexture(u[x], v[x], texPixels, texWidth, texHeight, texBgra);
		fragcolor[x] = MAKEARGB(0xff, 0xff - RPART(texel), 0xff - BPART(texel), 0xff - GPART(texel));
	}
}

static void FuncNormal_AddColor(int x0, int x1, PolyTriangleThreadData* thread)
{
	auto& streamdata = thread->mainVertexShader.Data;
	uint32_t r = (int)(streamdata.uAddColor.r * 255.0f);
	uint32_t g = (int)(streamdata.uAddColor.g * 255.0f);
	uint32_t b = (int)(streamdata.uAddColor.b * 255.0f);
	uint32_t* fragcolor = thread->scanline.FragColor;
	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = fragcolor[x];
		fragcolor[x] = MAKEARGB(
			APART(texel),
			MIN(r + RPART(texel), (uint32_t)255),
			MIN(g + GPART(texel), (uint32_t)255),
			MIN(b + BPART(texel), (uint32_t)255));
	}
}

static void FuncNormal_AddObjectColor(int x0, int x1, PolyTriangleThreadData* thread)
{
	auto& streamdata = thread->mainVertexShader.Data;
	uint32_t r = (int)(streamdata.uObjectColor.r * 256.0f);
	uint32_t g = (int)(streamdata.uObjectColor.g * 256.0f);
	uint32_t b = (int)(streamdata.uObjectColor.b * 256.0f);
	uint32_t* fragcolor = thread->scanline.FragColor;
	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = fragcolor[x];
		fragcolor[x] = MAKEARGB(
			APART(texel),
			MIN((r * RPART(texel)) >> 8, (uint32_t)255),
			MIN((g * GPART(texel)) >> 8, (uint32_t)255),
			MIN((b * BPART(texel)) >> 8, (uint32_t)255));
	}
}

static void FuncNormal_AddObjectColor2(int x0, int x1, PolyTriangleThreadData* thread)
{
	auto& streamdata = thread->mainVertexShader.Data;
	float* gradientdistZ = thread->scanline.GradientdistZ;
	uint32_t* fragcolor = thread->scanline.FragColor;
	for (int x = x0; x < x1; x++)
	{
		float t = gradientdistZ[x];
		float inv_t = 1.0f - t;
		uint32_t r = (int)((streamdata.uObjectColor.r * inv_t + streamdata.uObjectColor2.r * t) * 256.0f);
		uint32_t g = (int)((streamdata.uObjectColor.g * inv_t + streamdata.uObjectColor2.g * t) * 256.0f);
		uint32_t b = (int)((streamdata.uObjectColor.b * inv_t + streamdata.uObjectColor2.b * t) * 256.0f);

		uint32_t texel = fragcolor[x];
		fragcolor[x] = MAKEARGB(
			APART(texel),
			MIN((r * RPART(texel)) >> 8, (uint32_t)255),
			MIN((g * GPART(texel)) >> 8, (uint32_t)255),
			MIN((b * BPART(texel)) >> 8, (uint32_t)255));
	}
}

static void FuncNormal_DesaturationFactor(int x0, int x1, PolyTriangleThreadData* thread)
{
	auto& streamdata = thread->mainVertexShader.Data;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint32_t t = (int)(streamdata.uDesaturationFactor * 256.0f);
	uint32_t inv_t = 256 - t;
	for (int x = x0; x < x1; x++)
	{
		uint32_t texel = fragcolor[x];
		uint32_t gray = (RPART(texel) * 77 + GPART(texel) * 143 + BPART(texel) * 37) >> 8;
		fragcolor[x] = MAKEARGB(
			APART(texel),
			(RPART(texel) * inv_t + gray * t + 127) >> 8,
			(GPART(texel) * inv_t + gray * t + 127) >> 8,
			(BPART(texel) * inv_t + gray * t + 127) >> 8);
	}
}

static void RunAlphaTest(int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t alphaThreshold = thread->AlphaThreshold;
	uint32_t* fragcolor = thread->scanline.FragColor;
	uint8_t* discard = thread->scanline.discard;
	for (int x = x0; x < x1; x++)
	{
		discard[x] = fragcolor[x] <= alphaThreshold;
	}
}

static void ApplyVertexColor(int x0, int x1, PolyTriangleThreadData* thread)
{
	uint32_t* fragcolor = thread->scanline.FragColor;
	for (int x = x0; x < x1; x++)
	{
		uint32_t r = thread->scanline.vColorR[x];
		uint32_t g = thread->scanline.vColorG[x];
		uint32_t b = thread->scanline.vColorB[x];
		uint32_t a = thread->scanline.vColorA[x];

		a += a >> 7;
		r += r >> 7;
		g += g >> 7;
		b += b >> 7;

		uint32_t texel = fragcolor[x];
		fragcolor[x] = MAKEARGB(
			(APART(texel) * a + 127) >> 8,
			(RPART(texel) * r + 127) >> 8,
			(GPART(texel) * g + 127) >> 8,
			(BPART(texel) * b + 127) >> 8);
	}
}

static void MainFP(int x0, int x1, PolyTriangleThreadData* thread)
{
	if (thread->numPolyLights > 0)
		WriteDynLightArray(x0, x1, thread);

	if (thread->EffectState == SHADER_Paletted) // func_paletted
	{
		FuncPaletted(x0, x1, thread);
	}
	else if (thread->EffectState == SHADER_NoTexture) // func_notexture
	{
		FuncNoTexture(x0, x1, thread);
	}
	else // func_normal
	{
		auto constants = thread->PushConstants;

		switch (constants->uTextureMode)
		{
		default:
		case TM_NORMAL:
		case TM_FOGLAYER:     FuncNormal(x0, x1, thread); break;
		case TM_STENCIL:      FuncNormal_Stencil(x0, x1, thread); break;
		case TM_OPAQUE:       FuncNormal_Opaque(x0, x1, thread); break;
		case TM_INVERSE:      FuncNormal_Inverse(x0, x1, thread); break;
		case TM_ALPHATEXTURE: FuncNormal_AlphaTexture(x0, x1, thread); break;
		case TM_CLAMPY:       FuncNormal_ClampY(x0, x1, thread); break;
		case TM_INVERTOPAQUE: FuncNormal_InvertOpaque(x0, x1, thread); break;
		}

		if (constants->uTextureMode != TM_FOGLAYER)
		{
			auto& streamdata = thread->mainVertexShader.Data;

			if (streamdata.uAddColor.r != 0.0f || streamdata.uAddColor.g != 0.0f || streamdata.uAddColor.b != 0.0f)
			{
				FuncNormal_AddColor(x0, x1, thread);
			}

			if (streamdata.uObjectColor2.a == 0.0f)
			{
				if (streamdata.uObjectColor.r != 1.0f || streamdata.uObjectColor.g != 1.0f || streamdata.uObjectColor.b != 1.0f)
				{
					FuncNormal_AddObjectColor(x0, x1, thread);
				}
			}
			else
			{
				FuncNormal_AddObjectColor2(x0, x1, thread);
			}

			if (streamdata.uDesaturationFactor > 0.0f)
			{
				FuncNormal_DesaturationFactor(x0, x1, thread);
			}
		}
	}

	if (thread->AlphaTest)
		RunAlphaTest(x0, x1, thread);

	ApplyVertexColor(x0, x1, thread);

	auto constants = thread->PushConstants;
	uint32_t* fragcolor = thread->scanline.FragColor;
	if (constants->uLightLevel >= 0.0f && thread->numPolyLights > 0)
	{
		uint16_t* lightarray = thread->scanline.lightarray;
		uint32_t* dynlights = thread->scanline.dynlights;
		for (int x = x0; x < x1; x++)
		{
			uint32_t fg = fragcolor[x];
			int lightshade = lightarray[x];
			uint32_t dynlight = dynlights[x];

			uint32_t a = APART(fg);
			uint32_t r = MIN((RPART(fg) * (lightshade + RPART(dynlight))) >> 8, (uint32_t)255);
			uint32_t g = MIN((GPART(fg) * (lightshade + GPART(dynlight))) >> 8, (uint32_t)255);
			uint32_t b = MIN((BPART(fg) * (lightshade + BPART(dynlight))) >> 8, (uint32_t)255);

			fragcolor[x] = MAKEARGB(a, r, g, b);
		}
	}
	else if (constants->uLightLevel >= 0.0f)
	{
		uint16_t* lightarray = thread->scanline.lightarray;
		for (int x = x0; x < x1; x++)
		{
			uint32_t fg = fragcolor[x];
			int lightshade = lightarray[x];

			uint32_t a = APART(fg);
			uint32_t r = (RPART(fg) * lightshade) >> 8;
			uint32_t g = (GPART(fg) * lightshade) >> 8;
			uint32_t b = (BPART(fg) * lightshade) >> 8;

			fragcolor[x] = MAKEARGB(a, r, g, b);
		}

		// To do: apply fog
	}
	else if (thread->numPolyLights > 0)
	{
		uint32_t* dynlights = thread->scanline.dynlights;
		for (int x = x0; x < x1; x++)
		{
			uint32_t fg = fragcolor[x];
			uint32_t dynlight = dynlights[x];

			uint32_t a = APART(fg);
			uint32_t r = MIN((RPART(fg) * RPART(dynlight)) >> 8, (uint32_t)255);
			uint32_t g = MIN((GPART(fg) * GPART(dynlight)) >> 8, (uint32_t)255);
			uint32_t b = MIN((BPART(fg) * BPART(dynlight)) >> 8, (uint32_t)255);

			fragcolor[x] = MAKEARGB(a, r, g, b);
		}
	}
}

static void SelectFragmentShader(PolyTriangleThreadData* thread)
{
	void (*fragshader)(int x0, int x1, PolyTriangleThreadData * thread);

	if (thread->SpecialEffect == EFF_FOGBOUNDARY) // fogboundary.fp
	{
		fragshader = &EffectFogBoundary;
	}
	else if (thread->SpecialEffect == EFF_BURN) // burn.fp
	{
		fragshader = &EffectBurn;
	}
	else if (thread->SpecialEffect == EFF_STENCIL) // stencil.fp
	{
		fragshader = &EffectStencil;
	}
	else
	{
		fragshader = &MainFP;
	}

	thread->FragmentShader = fragshader;
}

static void DrawSpan(int y, int x0, int x1, const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread)
{
	WriteVaryings(y, x0, x1, args, thread);

	if (thread->PushConstants->uLightLevel >= 0.0f)
		WriteLightArray(y, x0, x1, args, thread);

	thread->FragmentShader(x0, x1, thread);

	if (!thread->AlphaTest)
	{
		if (thread->WriteColor)
			thread->WriteColorFunc(y, x0, x1, thread);
		if (thread->WriteDepth)
			WriteDepth(y, x0, x1, thread);
		if (thread->WriteStencil)
			WriteStencil(y, x0, x1, thread);
	}
	else
	{
		uint8_t* discard = thread->scanline.discard;
		while (x0 < x1)
		{
			int xstart = x0;
			while (!discard[x0] && x0 < x1) x0++;

			if (xstart < x0)
			{
				if (thread->WriteColor)
					thread->WriteColorFunc(y, xstart, x0, thread);
				if (thread->WriteDepth)
					WriteDepth(y, xstart, x0, thread);
				if (thread->WriteStencil)
					WriteStencil(y, xstart, x0, thread);
			}

			while (discard[x0] && x0 < x1) x0++;
		}
	}
}

template<typename OptT>
static void TestSpan(int y, int x0, int x1, const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread)
{
	WriteW(y, x0, x1, args, thread);

	if ((OptT::Flags & SWTRI_DepthTest) || (OptT::Flags & SWTRI_StencilTest))
	{
		size_t pitch = thread->depthstencil->Width();

		uint8_t* stencilbuffer;
		uint8_t* stencilLine;
		uint8_t stencilTestValue;
		if (OptT::Flags & SWTRI_StencilTest)
		{
			stencilbuffer = thread->depthstencil->StencilValues();
			stencilLine = stencilbuffer + pitch * y;
			stencilTestValue = thread->StencilTestValue;
		}

		float* zbuffer;
		float* zbufferLine;
		float* w;
		float depthbias;
		if (OptT::Flags & SWTRI_DepthTest)
		{
			zbuffer = thread->depthstencil->DepthValues();
			zbufferLine = zbuffer + pitch * y;
			w = thread->scanline.W;
			depthbias = thread->depthbias;
		}

		int x = x0;
		int xend = x1;
		while (x < xend)
		{
			int xstart = x;

			if ((OptT::Flags & SWTRI_DepthTest) && (OptT::Flags & SWTRI_StencilTest))
			{
				while (zbufferLine[x] >= w[x] + depthbias && stencilLine[x] == stencilTestValue && x < xend)
					x++;
			}
			else if (OptT::Flags & SWTRI_DepthTest)
			{
				while (zbufferLine[x] >= w[x] + depthbias && x < xend)
					x++;
			}
			else if (OptT::Flags & SWTRI_StencilTest)
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
				DrawSpan(y, xstart, x, args, thread);
			}

			if ((OptT::Flags & SWTRI_DepthTest) && (OptT::Flags & SWTRI_StencilTest))
			{
				while ((zbufferLine[x] < w[x] + depthbias || stencilLine[x] != stencilTestValue) && x < xend)
					x++;
			}
			else if (OptT::Flags & SWTRI_DepthTest)
			{
				while (zbufferLine[x] < w[x] + depthbias && x < xend)
					x++;
			}
			else if (OptT::Flags & SWTRI_StencilTest)
			{
				while (stencilLine[x] != stencilTestValue && x < xend)
					x++;
			}
		}
	}
	else
	{
		DrawSpan(y, x0, x1, args, thread);
	}
}

static void SortVertices(const TriDrawTriangleArgs* args, ScreenTriVertex** sortedVertices)
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

void ScreenTriangle::Draw(const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread)
{
	// Sort vertices by Y position
	ScreenTriVertex* sortedVertices[3];
	SortVertices(args, sortedVertices);

	int clipleft = thread->clip.left;
	int cliptop = MAX(thread->clip.top, thread->numa_start_y);
	int clipright = thread->clip.right;
	int clipbottom = MIN(thread->clip.bottom, thread->numa_end_y);

	int topY = (int)(sortedVertices[0]->y + 0.5f);
	int midY = (int)(sortedVertices[1]->y + 0.5f);
	int bottomY = (int)(sortedVertices[2]->y + 0.5f);

	topY = MAX(topY, cliptop);
	midY = MIN(midY, clipbottom);
	bottomY = MIN(bottomY, clipbottom);

	if (topY >= bottomY)
		return;

	SelectFragmentShader(thread);
	SelectWriteColorFunc(thread);

	void(*testfunc)(int y, int x0, int x1, const TriDrawTriangleArgs * args, PolyTriangleThreadData * thread);

	int opt = 0;
	if (thread->DepthTest) opt |= SWTRI_DepthTest;
	if (thread->StencilTest) opt |= SWTRI_StencilTest;
	testfunc = ScreenTriangle::TestSpanOpts[opt];

	topY += thread->skipped_by_thread(topY);
	int num_cores = thread->num_cores;

	// Find start/end X positions for each line covered by the triangle:

	int y = topY;

	float longDX = sortedVertices[2]->x - sortedVertices[0]->x;
	float longDY = sortedVertices[2]->y - sortedVertices[0]->y;
	float longStep = longDX / longDY;
	float longPos = sortedVertices[0]->x + longStep * (y + 0.5f - sortedVertices[0]->y) + 0.5f;
	longStep *= num_cores;

	if (y < midY)
	{
		float shortDX = sortedVertices[1]->x - sortedVertices[0]->x;
		float shortDY = sortedVertices[1]->y - sortedVertices[0]->y;
		float shortStep = shortDX / shortDY;
		float shortPos = sortedVertices[0]->x + shortStep * (y + 0.5f - sortedVertices[0]->y) + 0.5f;
		shortStep *= num_cores;

		while (y < midY)
		{
			int x0 = (int)shortPos;
			int x1 = (int)longPos;
			if (x1 < x0) std::swap(x0, x1);
			x0 = clamp(x0, clipleft, clipright);
			x1 = clamp(x1, clipleft, clipright);

			testfunc(y, x0, x1, args, thread);

			shortPos += shortStep;
			longPos += longStep;
			y += num_cores;
		}
	}

	if (y < bottomY)
	{
		float shortDX = sortedVertices[2]->x - sortedVertices[1]->x;
		float shortDY = sortedVertices[2]->y - sortedVertices[1]->y;
		float shortStep = shortDX / shortDY;
		float shortPos = sortedVertices[1]->x + shortStep * (y + 0.5f - sortedVertices[1]->y) + 0.5f;
		shortStep *= num_cores;

		while (y < bottomY)
		{
			int x0 = (int)shortPos;
			int x1 = (int)longPos;
			if (x1 < x0) std::swap(x0, x1);
			x0 = clamp(x0, clipleft, clipright);
			x1 = clamp(x1, clipleft, clipright);

			testfunc(y, x0, x1, args, thread);

			shortPos += shortStep;
			longPos += longStep;
			y += num_cores;
		}
	}
}

void(*ScreenTriangle::TestSpanOpts[])(int y, int x0, int x1, const TriDrawTriangleArgs* args, PolyTriangleThreadData* thread) =
{
	&TestSpan<TestSpanOpt0>,
	&TestSpan<TestSpanOpt1>,
	&TestSpan<TestSpanOpt2>,
	&TestSpan<TestSpanOpt3>
};
