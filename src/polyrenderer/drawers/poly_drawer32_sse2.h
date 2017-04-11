/*
**  Projected triangle drawer
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

#pragma once

#include "screen_triangle.h"

namespace TriScreenDrawerModes
{
	template<typename SamplerT, typename FilterModeT>
	FORCEINLINE unsigned int VECTORCALL Sample32(int32_t u, int32_t v, const uint32_t *texPixels, int texWidth, int texHeight, uint32_t oneU, uint32_t oneV, uint32_t color, const uint32_t *translation)
	{
		uint32_t texel;
		if (SamplerT::Mode == (int)Samplers::Shaded || SamplerT::Mode == (int)Samplers::Stencil || SamplerT::Mode == (int)Samplers::Fill || SamplerT::Mode == (int)Samplers::Fuzz)
		{
			return color;
		}
		else if (SamplerT::Mode == (int)Samplers::Translated)
		{
			const uint8_t *texpal = (const uint8_t *)texPixels;
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			return translation[texpal[texelX * texHeight + texelY]];
		}
		else if (FilterModeT::Mode == (int)FilterModes::Nearest)
		{
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			texel = texPixels[texelX * texHeight + texelY];
		}
		else
		{
			u -= oneU >> 1;
			v -= oneV >> 1;

			unsigned int frac_x0 = (((uint32_t)u << 8) >> FRACBITS) * texWidth;
			unsigned int frac_x1 = ((((uint32_t)u << 8) + oneU) >> FRACBITS) * texWidth;
			unsigned int frac_y0 = (((uint32_t)v << 8) >> FRACBITS) * texHeight;
			unsigned int frac_y1 = ((((uint32_t)v << 8) + oneV) >> FRACBITS) * texHeight;
			unsigned int x0 = frac_x0 >> FRACBITS;
			unsigned int x1 = frac_x1 >> FRACBITS;
			unsigned int y0 = frac_y0 >> FRACBITS;
			unsigned int y1 = frac_y1 >> FRACBITS;

			unsigned int p00 = texPixels[x0 * texHeight + y0];
			unsigned int p01 = texPixels[x0 * texHeight + y1];
			unsigned int p10 = texPixels[x1 * texHeight + y0];
			unsigned int p11 = texPixels[x1 * texHeight + y1];

			unsigned int inv_a = (frac_x1 >> (FRACBITS - 4)) & 15;
			unsigned int inv_b = (frac_y1 >> (FRACBITS - 4)) & 15;
			unsigned int a = 16 - inv_a;
			unsigned int b = 16 - inv_b;

			unsigned int sred = (RPART(p00) * (a * b) + RPART(p01) * (inv_a * b) + RPART(p10) * (a * inv_b) + RPART(p11) * (inv_a * inv_b) + 127) >> 8;
			unsigned int sgreen = (GPART(p00) * (a * b) + GPART(p01) * (inv_a * b) + GPART(p10) * (a * inv_b) + GPART(p11) * (inv_a * inv_b) + 127) >> 8;
			unsigned int sblue = (BPART(p00) * (a * b) + BPART(p01) * (inv_a * b) + BPART(p10) * (a * inv_b) + BPART(p11) * (inv_a * inv_b) + 127) >> 8;
			unsigned int salpha = (APART(p00) * (a * b) + APART(p01) * (inv_a * b) + APART(p10) * (a * inv_b) + APART(p11) * (inv_a * inv_b) + 127) >> 8;

			texel = (salpha << 24) | (sred << 16) | (sgreen << 8) | sblue;
		}

		if (SamplerT::Mode == (int)Samplers::Skycap)
		{
			int start_fade = 2; // How fast it should fade out

			int alpha_top = clamp(v >> (16 - start_fade), 0, 256);
			int alpha_bottom = clamp(((2 << 24) - v) >> (16 - start_fade), 0, 256);
			int a = MIN(alpha_top, alpha_bottom);
			int inv_a = 256 - a;

			uint32_t r = RPART(texel);
			uint32_t g = GPART(texel);
			uint32_t b = BPART(texel);
			uint32_t fg_a = APART(texel);
			uint32_t bg_red = RPART(color);
			uint32_t bg_green = GPART(color);
			uint32_t bg_blue = BPART(color);
			r = (r * a + bg_red * inv_a + 127) >> 8;
			g = (g * a + bg_green * inv_a + 127) >> 8;
			b = (b * a + bg_blue * inv_a + 127) >> 8;
			return MAKEARGB(fg_a, r, g, b);
		}
		else
		{
			return texel;
		}
	}

	template<typename SamplerT>
	FORCEINLINE unsigned int VECTORCALL SampleShade32(int32_t u, int32_t v, const uint32_t *texPixels, int texWidth, int texHeight, int &fuzzpos)
	{
		if (SamplerT::Mode == (int)Samplers::Shaded)
		{
			const uint8_t *texpal = (const uint8_t *)texPixels;
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			unsigned int sampleshadeout = texpal[texelX * texHeight + texelY];
			sampleshadeout += sampleshadeout >> 7; // 255 -> 256
			return sampleshadeout;
		}
		else if (SamplerT::Mode == (int)Samplers::Stencil)
		{
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			unsigned int sampleshadeout = APART(texPixels[texelX * texHeight + texelY]);
			sampleshadeout += sampleshadeout >> 7; // 255 -> 256
			return sampleshadeout;
		}
		else if (SamplerT::Mode == (int)Samplers::Fuzz)
		{
			uint32_t texelX = ((((uint32_t)u << 8) >> 16) * texWidth) >> 16;
			uint32_t texelY = ((((uint32_t)v << 8) >> 16) * texHeight) >> 16;
			unsigned int sampleshadeout = APART(texPixels[texelX * texHeight + texelY]);
			sampleshadeout += sampleshadeout >> 7; // 255 -> 256
			sampleshadeout = (sampleshadeout * fuzzcolormap[fuzzpos++]) >> 5;
			if (fuzzpos >= FUZZTABLE) fuzzpos = 0;
			return sampleshadeout;
		}
		else
		{
			return 0;
		}
	}

	template<typename ShadeModeT>
	FORCEINLINE __m128i VECTORCALL Shade32(__m128i fgcolor, __m128i mlight, unsigned int ifgcolor0, unsigned int ifgcolor1, int desaturate, __m128i inv_desaturate, __m128i shade_fade, __m128i shade_light)
	{
		if (ShadeModeT::Mode == (int)ShadeMode::Simple)
		{
			fgcolor = _mm_srli_epi16(_mm_mullo_epi16(fgcolor, mlight), 8);
		}
		else if (ShadeModeT::Mode == (int)ShadeMode::Advanced)
		{
			int blue0 = BPART(ifgcolor0);
			int green0 = GPART(ifgcolor0);
			int red0 = RPART(ifgcolor0);
			int intensity0 = ((red0 * 77 + green0 * 143 + blue0 * 37) >> 8) * desaturate;

			int blue1 = BPART(ifgcolor1);
			int green1 = GPART(ifgcolor1);
			int red1 = RPART(ifgcolor1);
			int intensity1 = ((red1 * 77 + green1 * 143 + blue1 * 37) >> 8) * desaturate;

			__m128i intensity = _mm_set_epi16(0, intensity1, intensity1, intensity1, 0, intensity0, intensity0, intensity0);

			fgcolor = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(fgcolor, inv_desaturate), intensity), 8);
			fgcolor = _mm_mullo_epi16(fgcolor, mlight);
			fgcolor = _mm_srli_epi16(_mm_add_epi16(shade_fade, fgcolor), 8);
			fgcolor = _mm_srli_epi16(_mm_mullo_epi16(fgcolor, shade_light), 8);
		}
		return fgcolor;
	}

	template<typename BlendT>
	FORCEINLINE __m128i VECTORCALL Blend32(__m128i fgcolor, __m128i bgcolor, unsigned int ifgcolor0, unsigned int ifgcolor1, unsigned int ifgshade0, unsigned int ifgshade1, uint32_t srcalpha, uint32_t destalpha)
	{
		if (BlendT::Mode == (int)BlendModes::Opaque)
		{
			__m128i outcolor = fgcolor;
			outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
			return outcolor;
		}
		else if (BlendT::Mode == (int)BlendModes::Masked)
		{
			__m128i mask = _mm_cmpeq_epi32(_mm_packus_epi16(fgcolor, _mm_setzero_si128()), _mm_setzero_si128());
			mask = _mm_unpacklo_epi8(mask, _mm_setzero_si128());
			__m128i outcolor = _mm_or_si128(_mm_and_si128(mask, bgcolor), _mm_andnot_si128(mask, fgcolor));
			outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
			outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
			return outcolor;
		}
		else if (BlendT::Mode == (int)BlendModes::AddSrcColorOneMinusSrcColor)
		{
			__m128i inv_srccolor = _mm_sub_epi16(_mm_set1_epi16(256), _mm_add_epi16(fgcolor, _mm_srli_epi16(fgcolor, 7)));
			__m128i outcolor = _mm_add_epi16(fgcolor, _mm_srli_epi16(_mm_mullo_epi16(bgcolor, inv_srccolor), 8));
			outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
			return outcolor;
		}
		else if (BlendT::Mode == (int)BlendModes::Shaded)
		{
			ifgshade0 = (ifgshade0 * srcalpha + 128) >> 8;
			ifgshade1 = (ifgshade1 * srcalpha + 128) >> 8;
			__m128i alpha = _mm_set_epi16(ifgshade1, ifgshade1, ifgshade1, ifgshade1, ifgshade0, ifgshade0, ifgshade0, ifgshade0);
			__m128i inv_alpha = _mm_sub_epi16(_mm_set1_epi16(256), alpha);

			fgcolor = _mm_mullo_epi16(fgcolor, alpha);
			bgcolor = _mm_mullo_epi16(bgcolor, inv_alpha);
			__m128i outcolor = _mm_srli_epi16(_mm_add_epi16(fgcolor, bgcolor), 8);
			outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
			outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
			return outcolor;
		}
		else if (BlendT::Mode == (int)BlendModes::AddClampShaded)
		{
			ifgshade0 = (ifgshade0 * srcalpha + 128) >> 8;
			ifgshade1 = (ifgshade1 * srcalpha + 128) >> 8;
			__m128i alpha = _mm_set_epi16(ifgshade1, ifgshade1, ifgshade1, ifgshade1, ifgshade0, ifgshade0, ifgshade0, ifgshade0);

			fgcolor = _mm_srli_epi16(_mm_mullo_epi16(fgcolor, alpha), 8);
			__m128i outcolor = _mm_add_epi16(fgcolor, bgcolor);
			outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
			outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
			return outcolor;
		}
		else
		{
			uint32_t alpha0 = APART(ifgcolor0);
			uint32_t alpha1 = APART(ifgcolor1);
			alpha0 += alpha0 >> 7; // 255->256
			alpha1 += alpha1 >> 7; // 255->256
			uint32_t inv_alpha0 = 256 - alpha0;
			uint32_t inv_alpha1 = 256 - alpha1;

			uint32_t bgalpha0 = (destalpha * alpha0 + (inv_alpha0 << 8) + 128) >> 8;
			uint32_t bgalpha1 = (destalpha * alpha1 + (inv_alpha1 << 8) + 128) >> 8;
			uint32_t fgalpha0 = (srcalpha * alpha0 + 128) >> 8;
			uint32_t fgalpha1 = (srcalpha * alpha1 + 128) >> 8;

			__m128i bgalpha = _mm_set_epi16(bgalpha1, bgalpha1, bgalpha1, bgalpha1, bgalpha0, bgalpha0, bgalpha0, bgalpha0);
			__m128i fgalpha = _mm_set_epi16(fgalpha1, fgalpha1, fgalpha1, fgalpha1, fgalpha0, fgalpha0, fgalpha0, fgalpha0);

			fgcolor = _mm_mullo_epi16(fgcolor, fgalpha);
			bgcolor = _mm_mullo_epi16(bgcolor, bgalpha);

			__m128i fg_lo = _mm_unpacklo_epi16(fgcolor, _mm_setzero_si128());
			__m128i bg_lo = _mm_unpacklo_epi16(bgcolor, _mm_setzero_si128());
			__m128i fg_hi = _mm_unpackhi_epi16(fgcolor, _mm_setzero_si128());
			__m128i bg_hi = _mm_unpackhi_epi16(bgcolor, _mm_setzero_si128());

			__m128i out_lo, out_hi;
			if (BlendT::Mode == (int)BlendModes::AddClamp)
			{
				out_lo = _mm_add_epi32(fg_lo, bg_lo);
				out_hi = _mm_add_epi32(fg_hi, bg_hi);
			}
			else if (BlendT::Mode == (int)BlendModes::SubClamp)
			{
				out_lo = _mm_sub_epi32(fg_lo, bg_lo);
				out_hi = _mm_sub_epi32(fg_hi, bg_hi);
			}
			else if (BlendT::Mode == (int)BlendModes::RevSubClamp)
			{
				out_lo = _mm_sub_epi32(bg_lo, fg_lo);
				out_hi = _mm_sub_epi32(bg_hi, fg_hi);
			}

			out_lo = _mm_srai_epi32(out_lo, 8);
			out_hi = _mm_srai_epi32(out_hi, 8);
			__m128i outcolor = _mm_packs_epi32(out_lo, out_hi);
			outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
			outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
			return outcolor;
		}
	}
}

template<typename BlendT, typename SamplerT>
class TriScreenDrawer32
{
public:
	static void Execute(int x, int y, uint32_t mask0, uint32_t mask1, const TriDrawTriangleArgs *args)
	{
		using namespace TriScreenDrawerModes;

		bool is_simple_shade = args->uniforms->SimpleShade();

		if (SamplerT::Mode == (int)Samplers::Texture)
		{
			bool is_nearest_filter = args->uniforms->NearestFilter();

			if (is_simple_shade)
			{
				if (is_nearest_filter)
					DrawBlock<SimpleShade, NearestFilter>(x, y, mask0, mask1, args);
				else
					DrawBlock<SimpleShade, LinearFilter>(x, y, mask0, mask1, args);
			}
			else
			{
				if (is_nearest_filter)
					DrawBlock<AdvancedShade, NearestFilter>(x, y, mask0, mask1, args);
				else
					DrawBlock<AdvancedShade, LinearFilter>(x, y, mask0, mask1, args);
			}
		}
		else if (SamplerT::Mode == (int)Samplers::Fuzz)
		{
			DrawBlock<NoShade, NearestFilter>(x, y, mask0, mask1, args);
		}
		else // no linear filtering for translated, shaded, stencil, fill or skycap
		{
			if (is_simple_shade)
			{
				DrawBlock<SimpleShade, NearestFilter>(x, y, mask0, mask1, args);
			}
			else
			{
				DrawBlock<AdvancedShade, NearestFilter>(x, y, mask0, mask1, args);
			}
		}
	}

private:
	template<typename ShadeModeT, typename FilterModeT>
	FORCEINLINE static void VECTORCALL DrawBlock(int destX, int destY, uint32_t mask0, uint32_t mask1, const TriDrawTriangleArgs *args)
	{
		using namespace TriScreenDrawerModes;

		bool is_fixed_light = args->uniforms->FixedLight();
		uint32_t lightmask = is_fixed_light ? 0 : 0xffffffff;
		uint32_t srcalpha = args->uniforms->SrcAlpha();
		uint32_t destalpha = args->uniforms->DestAlpha();

		int fuzzpos = (ScreenTriangle::FuzzStart + destX * 123 + destY) % FUZZTABLE;

		// Calculate gradients
		const TriVertex &v1 = *args->v1;
		ScreenTriangleStepVariables gradientX = args->gradientX;
		ScreenTriangleStepVariables gradientY = args->gradientY;
		ScreenTriangleStepVariables blockPosY;
		blockPosY.W = v1.w + gradientX.W * (destX - v1.x) + gradientY.W * (destY - v1.y);
		blockPosY.U = v1.u * v1.w + gradientX.U * (destX - v1.x) + gradientY.U * (destY - v1.y);
		blockPosY.V = v1.v * v1.w + gradientX.V * (destX - v1.x) + gradientY.V * (destY - v1.y);
		gradientX.W *= 8.0f;
		gradientX.U *= 8.0f;
		gradientX.V *= 8.0f;

		// Output
		uint32_t * RESTRICT destOrg = (uint32_t*)args->dest;
		int pitch = args->pitch;
		uint32_t *dest = destOrg + destX + destY * pitch;

		// Light
		uint32_t light = args->uniforms->Light();
		float shade = 2.0f - (light + 12.0f) / 128.0f;
		float globVis = args->uniforms->GlobVis() * (1.0f / 32.0f);
		light += (light >> 7); // 255 -> 256

		// Sampling stuff
		uint32_t color = args->uniforms->Color();
		const uint32_t * RESTRICT translation = (const uint32_t *)args->uniforms->Translation();
		const uint32_t * RESTRICT texPixels = (const uint32_t *)args->uniforms->TexturePixels();
		uint32_t texWidth = args->uniforms->TextureWidth();
		uint32_t texHeight = args->uniforms->TextureHeight();
		uint32_t oneU, oneV;
		if (SamplerT::Mode != (int)Samplers::Fill)
		{
			oneU = ((0x800000 + texWidth - 1) / texWidth) * 2 + 1;
			oneV = ((0x800000 + texHeight - 1) / texHeight) * 2 + 1;
		}
		else
		{
			oneU = 0;
			oneV = 0;
		}

		// Shade constants
		__m128i inv_desaturate, shade_fade, shade_light;
		int desaturate;
		if (ShadeModeT::Mode == (int)ShadeMode::Advanced)
		{
			inv_desaturate = _mm_setr_epi16(256, 256 - args->uniforms->ShadeDesaturate(), 256 - args->uniforms->ShadeDesaturate(), 256 - args->uniforms->ShadeDesaturate(), 256, 256 - args->uniforms->ShadeDesaturate(), 256 - args->uniforms->ShadeDesaturate(), 256 - args->uniforms->ShadeDesaturate());
			shade_fade = _mm_set_epi16(args->uniforms->ShadeFadeAlpha(), args->uniforms->ShadeFadeRed(), args->uniforms->ShadeFadeGreen(), args->uniforms->ShadeFadeBlue(), args->uniforms->ShadeFadeAlpha(), args->uniforms->ShadeFadeRed(), args->uniforms->ShadeFadeGreen(), args->uniforms->ShadeFadeBlue());
			shade_light = _mm_set_epi16(args->uniforms->ShadeLightAlpha(), args->uniforms->ShadeLightRed(), args->uniforms->ShadeLightGreen(), args->uniforms->ShadeLightBlue(), args->uniforms->ShadeLightAlpha(), args->uniforms->ShadeLightRed(), args->uniforms->ShadeLightGreen(), args->uniforms->ShadeLightBlue());
			desaturate = args->uniforms->ShadeDesaturate();
		}
		else
		{
			inv_desaturate = _mm_setzero_si128();
			shade_fade = _mm_setzero_si128();
			shade_fade = _mm_setzero_si128();
			shade_light = _mm_setzero_si128();
			desaturate = 0;
		}

		if (mask0 == 0xffffffff && mask1 == 0xffffffff)
		{
			for (int y = 0; y < 8; y++)
			{
				float rcpW = 0x01000000 / blockPosY.W;
				int32_t posU = (int32_t)(blockPosY.U * rcpW);
				int32_t posV = (int32_t)(blockPosY.V * rcpW);

				fixed_t lightpos = FRACUNIT - (int)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosY.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				lightpos = (lightpos & lightmask) | ((light << 8) & ~lightmask);

				ScreenTriangleStepVariables blockPosX = blockPosY;
				blockPosX.W += gradientX.W;
				blockPosX.U += gradientX.U;
				blockPosX.V += gradientX.V;

				rcpW = 0x01000000 / blockPosX.W;
				int32_t nextU = (int32_t)(blockPosX.U * rcpW);
				int32_t nextV = (int32_t)(blockPosX.V * rcpW);
				int32_t stepU = (nextU - posU) / 8;
				int32_t stepV = (nextV - posV) / 8;

				fixed_t lightnext = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosX.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				fixed_t lightstep = (lightnext - lightpos) / 8;
				lightstep = lightstep & lightmask;

				for (int ix = 0; ix < 4; ix++)
				{
					// Load bgcolor
					__m128i bgcolor;
					if (BlendT::Mode != (int)BlendModes::Opaque)
						bgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)(dest + ix * 2)), _mm_setzero_si128());
					else
						bgcolor = _mm_setzero_si128();

					// Sample fgcolor
					unsigned int ifgcolor[2], ifgshade[2];
					ifgcolor[0] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
					ifgshade[0] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
					posU += stepU;
					posV += stepV;

					ifgcolor[1] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
					ifgshade[1] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
					posU += stepU;
					posV += stepV;

					// Setup light
					int lightpos0 = lightpos >> 8;
					lightpos += lightstep;
					int lightpos1 = lightpos >> 8;
					lightpos += lightstep;
					__m128i mlight = _mm_set_epi16(256, lightpos1, lightpos1, lightpos1, 256, lightpos0, lightpos0, lightpos0);

					__m128i shade_fade_lit;
					if (ShadeModeT::Mode == (int)ShadeMode::Advanced)
					{
						__m128i inv_light = _mm_sub_epi16(_mm_set_epi16(0, 256, 256, 256, 0, 256, 256, 256), mlight);
						shade_fade_lit = _mm_mullo_epi16(shade_fade, inv_light);
					}
					else
					{
						shade_fade_lit = _mm_setzero_si128();
					}

					// Shade and blend
					__m128i fgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)ifgcolor), _mm_setzero_si128());
					fgcolor = Shade32<ShadeModeT>(fgcolor, mlight, ifgcolor[0], ifgcolor[1], desaturate, inv_desaturate, shade_fade_lit, shade_light);
					__m128i outcolor = Blend32<BlendT>(fgcolor, bgcolor, ifgcolor[0], ifgcolor[1], ifgshade[0], ifgshade[1], srcalpha, destalpha);

					// Store result
					_mm_storel_epi64((__m128i*)(dest + ix * 2), outcolor);
				}

				blockPosY.W += gradientY.W;
				blockPosY.U += gradientY.U;
				blockPosY.V += gradientY.V;

				dest += pitch;
			}
		}
		else
		{
			// mask0 loop:
			for (int y = 0; y < 4; y++)
			{
				float rcpW = 0x01000000 / blockPosY.W;
				int32_t posU = (int32_t)(blockPosY.U * rcpW);
				int32_t posV = (int32_t)(blockPosY.V * rcpW);

				fixed_t lightpos = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosY.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				lightpos = (lightpos & lightmask) | ((light << 8) & ~lightmask);

				ScreenTriangleStepVariables blockPosX = blockPosY;
				blockPosX.W += gradientX.W;
				blockPosX.U += gradientX.U;
				blockPosX.V += gradientX.V;

				rcpW = 0x01000000 / blockPosX.W;
				int32_t nextU = (int32_t)(blockPosX.U * rcpW);
				int32_t nextV = (int32_t)(blockPosX.V * rcpW);
				int32_t stepU = (nextU - posU) / 8;
				int32_t stepV = (nextV - posV) / 8;

				fixed_t lightnext = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosX.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				fixed_t lightstep = (lightnext - lightpos) / 8;
				lightstep = lightstep & lightmask;

				for (int x = 0; x < 4; x++)
				{
					// Load bgcolor
					uint32_t desttmp[2];
					__m128i bgcolor;
					if (BlendT::Mode != (int)BlendModes::Opaque)
					{
						if (mask0 & (1 << 31)) desttmp[0] = dest[x * 2];
						if (mask0 & (1 << 30)) desttmp[1] = dest[x * 2 + 1];
						bgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)desttmp), _mm_setzero_si128());
					}
					else
						bgcolor = _mm_setzero_si128();

					// Sample fgcolor
					unsigned int ifgcolor[2], ifgshade[2];
					ifgcolor[0] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
					ifgshade[0] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
					posU += stepU;
					posV += stepV;

					ifgcolor[1] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
					ifgshade[1] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
					posU += stepU;
					posV += stepV;

					// Setup light
					int lightpos0 = lightpos >> 8;
					lightpos += lightstep;
					int lightpos1 = lightpos >> 8;
					lightpos += lightstep;
					__m128i mlight = _mm_set_epi16(256, lightpos1, lightpos1, lightpos1, 256, lightpos0, lightpos0, lightpos0);

					__m128i shade_fade_lit;
					if (ShadeModeT::Mode == (int)ShadeMode::Advanced)
					{
						__m128i inv_light = _mm_sub_epi16(_mm_set_epi16(0, 256, 256, 256, 0, 256, 256, 256), mlight);
						shade_fade_lit = _mm_mullo_epi16(shade_fade, inv_light);
					}
					else
					{
						shade_fade_lit = _mm_setzero_si128();
					}

					// Shade and blend
					__m128i fgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)ifgcolor), _mm_setzero_si128());
					fgcolor = Shade32<ShadeModeT>(fgcolor, mlight, ifgcolor[0], ifgcolor[1], desaturate, inv_desaturate, shade_fade_lit, shade_light);
					__m128i outcolor = Blend32<BlendT>(fgcolor, bgcolor, ifgcolor[0], ifgcolor[1], ifgshade[0], ifgshade[1], srcalpha, destalpha);

					// Store result
					_mm_storel_epi64((__m128i*)desttmp, outcolor);
					if (mask0 & (1 << 31)) dest[x * 2] = desttmp[0];
					if (mask0 & (1 << 30)) dest[x * 2 + 1] = desttmp[1];

					mask0 <<= 2;
				}

				blockPosY.W += gradientY.W;
				blockPosY.U += gradientY.U;
				blockPosY.V += gradientY.V;

				dest += pitch;
			}

			// mask1 loop:
			for (int y = 0; y < 4; y++)
			{
				float rcpW = 0x01000000 / blockPosY.W;
				int32_t posU = (int32_t)(blockPosY.U * rcpW);
				int32_t posV = (int32_t)(blockPosY.V * rcpW);

				fixed_t lightpos = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosY.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				lightpos = (lightpos & lightmask) | ((light << 8) & ~lightmask);

				ScreenTriangleStepVariables blockPosX = blockPosY;
				blockPosX.W += gradientX.W;
				blockPosX.U += gradientX.U;
				blockPosX.V += gradientX.V;

				rcpW = 0x01000000 / blockPosX.W;
				int32_t nextU = (int32_t)(blockPosX.U * rcpW);
				int32_t nextV = (int32_t)(blockPosX.V * rcpW);
				int32_t stepU = (nextU - posU) / 8;
				int32_t stepV = (nextV - posV) / 8;

				fixed_t lightnext = FRACUNIT - (fixed_t)(clamp(shade - MIN(24.0f / 32.0f, globVis * blockPosX.W), 0.0f, 31.0f / 32.0f) * (float)FRACUNIT);
				fixed_t lightstep = (lightnext - lightpos) / 8;
				lightstep = lightstep & lightmask;

				for (int x = 0; x < 4; x++)
				{
					// Load bgcolor
					uint32_t desttmp[2];
					__m128i bgcolor;
					if (BlendT::Mode != (int)BlendModes::Opaque)
					{
						if (mask1 & (1 << 31)) desttmp[0] = dest[x * 2];
						if (mask1 & (1 << 30)) desttmp[1] = dest[x * 2 + 1];
						bgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)desttmp), _mm_setzero_si128());
					}
					else
						bgcolor = _mm_setzero_si128();

					// Sample fgcolor
					unsigned int ifgcolor[2], ifgshade[2];
					ifgcolor[0] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
					ifgshade[0] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
					posU += stepU;
					posV += stepV;

					ifgcolor[1] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
					ifgshade[1] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
					posU += stepU;
					posV += stepV;

					// Setup light
					int lightpos0 = lightpos >> 8;
					lightpos += lightstep;
					int lightpos1 = lightpos >> 8;
					lightpos += lightstep;
					__m128i mlight = _mm_set_epi16(256, lightpos1, lightpos1, lightpos1, 256, lightpos0, lightpos0, lightpos0);

					__m128i shade_fade_lit;
					if (ShadeModeT::Mode == (int)ShadeMode::Advanced)
					{
						__m128i inv_light = _mm_sub_epi16(_mm_set_epi16(0, 256, 256, 256, 0, 256, 256, 256), mlight);
						shade_fade_lit = _mm_mullo_epi16(shade_fade, inv_light);
					}
					else
					{
						shade_fade_lit = _mm_setzero_si128();
					}

					// Shade and blend
					__m128i fgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)ifgcolor), _mm_setzero_si128());
					fgcolor = Shade32<ShadeModeT>(fgcolor, mlight, ifgcolor[0], ifgcolor[1], desaturate, inv_desaturate, shade_fade_lit, shade_light);
					__m128i outcolor = Blend32<BlendT>(fgcolor, bgcolor, ifgcolor[0], ifgcolor[1], ifgshade[0], ifgshade[1], srcalpha, destalpha);

					// Store result
					_mm_storel_epi64((__m128i*)desttmp, outcolor);
					if (mask1 & (1 << 31)) dest[x * 2] = desttmp[0];
					if (mask1 & (1 << 30)) dest[x * 2 + 1] = desttmp[1];

					mask1 <<= 2;
				}

				blockPosY.W += gradientY.W;
				blockPosY.U += gradientY.U;
				blockPosY.V += gradientY.V;

				dest += pitch;
			}
		}
	}
};

template<typename BlendT, typename SamplerT>
class RectScreenDrawer32
{
public:
	static void Execute(const void *destOrg, int destWidth, int destHeight, int destPitch, const RectDrawArgs *args, WorkerThreadData *thread)
	{
		using namespace TriScreenDrawerModes;

		if (args->SimpleShade())
		{
			Loop<SimpleShade, NearestFilter>(destOrg, destWidth, destHeight, destPitch, args, thread);
		}
		else
		{
			Loop<AdvancedShade, NearestFilter>(destOrg, destWidth, destHeight, destPitch, args, thread);
		}
	}

private:
	template<typename ShadeModeT, typename FilterModeT>
	FORCEINLINE static void VECTORCALL Loop(const void *destOrg, int destWidth, int destHeight, int destPitch, const RectDrawArgs *args, WorkerThreadData *thread)
	{
		using namespace TriScreenDrawerModes;

		int x0 = clamp((int)(args->X0() + 0.5f), 0, destWidth);
		int x1 = clamp((int)(args->X1() + 0.5f), 0, destWidth);
		int y0 = clamp((int)(args->Y0() + 0.5f), 0, destHeight);
		int y1 = clamp((int)(args->Y1() + 0.5f), 0, destHeight);

		if (x1 <= x0 || y1 <= y0)
			return;

		uint32_t srcalpha = args->SrcAlpha();
		uint32_t destalpha = args->DestAlpha();

		// Setup step variables
		float fstepU = (args->U1() - args->U0()) / (args->X1() - args->X0());
		float fstepV = (args->V1() - args->V0()) / (args->Y1() - args->Y0());
		uint32_t startU = (int32_t)((args->U0() + (x0 + 0.5f - args->X0()) * fstepU) * 0x1000000);
		uint32_t startV = (int32_t)((args->V0() + (y0 + 0.5f - args->Y0()) * fstepV) * 0x1000000);
		uint32_t stepU = (int32_t)(fstepU * 0x1000000);
		uint32_t stepV = (int32_t)(fstepV * 0x1000000);

		// Sampling stuff
		uint32_t color = args->Color();
		const uint32_t * RESTRICT translation = (const uint32_t *)args->Translation();
		const uint32_t * RESTRICT texPixels = (const uint32_t *)args->TexturePixels();
		uint32_t texWidth = args->TextureWidth();
		uint32_t texHeight = args->TextureHeight();
		uint32_t oneU, oneV;
		if (SamplerT::Mode != (int)Samplers::Fill)
		{
			oneU = ((0x800000 + texWidth - 1) / texWidth) * 2 + 1;
			oneV = ((0x800000 + texHeight - 1) / texHeight) * 2 + 1;
		}
		else
		{
			oneU = 0;
			oneV = 0;
		}

		// Shade constants
		__m128i inv_desaturate, shade_fade, shade_light;
		int desaturate;
		if (ShadeModeT::Mode == (int)ShadeMode::Advanced)
		{
			inv_desaturate = _mm_setr_epi16(256, 256 - args->ShadeDesaturate(), 256 - args->ShadeDesaturate(), 256 - args->ShadeDesaturate(), 256, 256 - args->ShadeDesaturate(), 256 - args->ShadeDesaturate(), 256 - args->ShadeDesaturate());
			shade_fade = _mm_set_epi16(args->ShadeFadeAlpha(), args->ShadeFadeRed(), args->ShadeFadeGreen(), args->ShadeFadeBlue(), args->ShadeFadeAlpha(), args->ShadeFadeRed(), args->ShadeFadeGreen(), args->ShadeFadeBlue());
			shade_light = _mm_set_epi16(args->ShadeLightAlpha(), args->ShadeLightRed(), args->ShadeLightGreen(), args->ShadeLightBlue(), args->ShadeLightAlpha(), args->ShadeLightRed(), args->ShadeLightGreen(), args->ShadeLightBlue());
			desaturate = args->ShadeDesaturate();
		}
		else
		{
			inv_desaturate = _mm_setzero_si128();
			shade_fade = _mm_setzero_si128();
			shade_light = _mm_setzero_si128();
			desaturate = 0;
		}

		// Setup light
		uint32_t lightpos = args->Light();
		lightpos += lightpos >> 7; // 255 -> 256
		__m128i mlight = _mm_set_epi16(256, lightpos, lightpos, lightpos, 256, lightpos, lightpos, lightpos);
		__m128i shade_fade_lit;
		if (ShadeModeT::Mode == (int)ShadeMode::Advanced)
		{
			__m128i inv_light = _mm_sub_epi16(_mm_set_epi16(0, 256, 256, 256, 0, 256, 256, 256), mlight);
			shade_fade_lit = _mm_mullo_epi16(shade_fade, inv_light);
		}
		else
		{
			shade_fade_lit = _mm_setzero_si128();
		}

		int count = x1 - x0;
		int sseCount = count / 2;

		int fuzzpos = (ScreenTriangle::FuzzStart + x0 * 123 + y0) % FUZZTABLE;

		uint32_t posV = startV;
		for (int y = y0; y < y1; y++, posV += stepV)
		{
			int coreBlock = y / 8;
			if (coreBlock % thread->num_cores != thread->core)
			{
				fuzzpos = (fuzzpos + count) % FUZZTABLE;
				continue;
			}

			uint32_t *dest = ((uint32_t*)destOrg) + y * destPitch + x0;

			uint32_t posU = startU;
			for (int i = 0; i < sseCount; i++)
			{
				// Load bgcolor
				__m128i bgcolor;
				if (BlendT::Mode != (int)BlendModes::Opaque)
					bgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)dest), _mm_setzero_si128());
				else
					bgcolor = _mm_setzero_si128();

				// Sample fgcolor
				unsigned int ifgcolor[2], ifgshade[2];
				ifgcolor[0] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
				ifgshade[0] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
				posU += stepU;

				ifgcolor[1] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
				ifgshade[1] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
				posU += stepU;

				// Shade and blend
				__m128i fgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)ifgcolor), _mm_setzero_si128());
				fgcolor = Shade32<ShadeModeT>(fgcolor, mlight, ifgcolor[0], ifgcolor[1], desaturate, inv_desaturate, shade_fade_lit, shade_light);
				__m128i outcolor = Blend32<BlendT>(fgcolor, bgcolor, ifgcolor[0], ifgcolor[1], ifgshade[0], ifgshade[1], srcalpha, destalpha);

				// Store result
				_mm_storel_epi64((__m128i*)dest, outcolor);
				dest += 2;
			}

			if (sseCount * 2 != count)
			{
				// Load bgcolor
				__m128i bgcolor;
				if (BlendT::Mode != (int)BlendModes::Opaque)
					bgcolor = _mm_unpacklo_epi8(_mm_cvtsi32_si128(*dest), _mm_setzero_si128());
				else
					bgcolor = _mm_setzero_si128();

				// Sample fgcolor
				unsigned int ifgcolor[2], ifgshade[2];
				ifgcolor[0] = Sample32<SamplerT, FilterModeT>(posU, posV, texPixels, texWidth, texHeight, oneU, oneV, color, translation);
				ifgshade[0] = SampleShade32<SamplerT>(posU, posV, texPixels, texWidth, texHeight, fuzzpos);
				ifgcolor[1] = 0;
				ifgshade[1] = 0;
				posU += stepU;

				// Shade and blend
				__m128i fgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)ifgcolor), _mm_setzero_si128());
				fgcolor = Shade32<ShadeModeT>(fgcolor, mlight, ifgcolor[0], ifgcolor[1], desaturate, inv_desaturate, shade_fade_lit, shade_light);
				__m128i outcolor = Blend32<BlendT>(fgcolor, bgcolor, ifgcolor[0], ifgcolor[1], ifgshade[0], ifgshade[1], srcalpha, destalpha);

				// Store result
				*dest = _mm_cvtsi128_si32(outcolor);
			}
		}
	}
};
