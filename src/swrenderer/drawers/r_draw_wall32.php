#!/usr/bin/php
/*
**  Drawer commands for walls
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

/*
	Warning: this C++ source file has been auto-generated. Please modify the original php script that generated it.
*/

#pragma once

#include "swrenderer/drawers/r_draw_rgba.h"
#include "swrenderer/viewport/r_walldrawer.h"

namespace swrenderer
{
<?
	GenerateDrawerCommand("DrawWall32Command", "opaque");
	GenerateDrawerCommand("DrawWallMasked32Command", "masked");
	GenerateDrawerCommand("DrawWallAddClamp32Command", "addclamp");
	GenerateDrawerCommand("DrawWallSubClamp32Command", "subclamp");
	GenerateDrawerCommand("DrawWallRevSubClamp32Command", "revsubclamp");
	
	function GenerateDrawerCommand($className, $blendVariant)
	{
?>
	class <?=$className?> : public DrawerCommand
	{
	protected:
		WallDrawerArgs args;

	public:
		<?=$className?>(const WallDrawerArgs &drawerargs) : args(drawerargs) { }

		void Execute(DrawerThread *thread) override
		{
			auto shade_constants = args.ColormapConstants();
			if (shade_constants.simple_shade)
			{
<?				LoopShade($blendVariant, true);?>
			}
			else
			{
<?				LoopShade($blendVariant, false);?>
			}
		}
		
		FString DebugInfo() override { return "<?=$className?>"; }
	};
	
<?
	}

	function LoopShade($blendVariant, $isSimpleShade)
	{ ?>
				const uint32_t *source = (const uint32_t*)args.TexturePixels();
				const uint32_t *source2 = (const uint32_t*)args.TexturePixels2();
				bool is_nearest_filter = (source2 == nullptr);
				if (is_nearest_filter)
				{
<?					Loop($blendVariant, $isSimpleShade, true);?>
				}
				else
				{
<?					Loop($blendVariant, $isSimpleShade, false);?>
				}
<?	}

	function Loop($blendVariant, $isSimpleShade, $isNearestFilter)
	{ ?>
					int textureheight = args.TextureHeight();
					uint32_t one = ((0x80000000 + textureheight - 1) / textureheight) * 2 + 1;
					
					// Shade constants
					int light = 256 - (args.Light() >> (FRACBITS - 8));
					__m128i mlight = _mm_set_epi16(256, light, light, light, 256, light, light, light);
					__m128i inv_light = _mm_set_epi16(0, 256 - light, 256 - light, 256 - light, 0, 256 - light, 256 - light, 256 - light);
<?					if ($isSimpleShade == false)
					{ ?>
					__m128i inv_desaturate = _mm_setr_epi16(256, 256 - shade_constants.desaturate, 256 - shade_constants.desaturate, 256 - shade_constants.desaturate, 256, 256 - shade_constants.desaturate, 256 - shade_constants.desaturate, 256 - shade_constants.desaturate);
					__m128i shade_fade = _mm_set_epi16(shade_constants.fade_alpha, shade_constants.fade_red, shade_constants.fade_green, shade_constants.fade_blue, shade_constants.fade_alpha, shade_constants.fade_red, shade_constants.fade_green, shade_constants.fade_blue);
					shade_fade = _mm_mullo_epi16(shade_fade, inv_light);
					__m128i shade_light = _mm_set_epi16(shade_constants.light_alpha, shade_constants.light_red, shade_constants.light_green, shade_constants.light_blue, shade_constants.light_alpha, shade_constants.light_red, shade_constants.light_green, shade_constants.light_blue);
					int desaturate = shade_constants.desaturate;
<?					} ?>
					
					int count = args.Count();
					int pitch = RenderViewport::Instance()->RenderTarget->GetPitch();
					uint32_t fracstep = args.TextureVStep();
					uint32_t frac = args.TextureVPos();
					uint32_t texturefracx = args.TextureUPos();
					uint32_t *dest = (uint32_t*)args.Dest();
					int dest_y = args.DestY();

					count = thread->count_for_thread(dest_y, count);
					if (count <= 0) return;
					frac += thread->skipped_by_thread(dest_y) * fracstep;
					dest = thread->dest_for_thread(dest_y, pitch, dest);
					fracstep *= thread->num_cores;
					pitch *= thread->num_cores;
<?					if ($isNearestFilter == false)
					{ ?>
					frac -= one / 2;
<?					} ?>
					__m128i srcalpha = _mm_set1_epi16(args.SrcAlpha());
					__m128i destalpha = _mm_set1_epi16(args.DestAlpha());
					
					int ssecount = count / 2;
					for (int index = 0; index < ssecount; index++)
					{
						int offset = index * pitch * 2;
						uint32_t desttmp[2];
						desttmp[0] = dest[offset];
						desttmp[1] = dest[offset + pitch];
<?						if ($blendVariant != "opaque") { ?>
						__m128i bgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)desttmp), _mm_setzero_si128());
<?						} ?>
						
						// Sample
						unsigned int ifgcolor[2];
						{
<?						Sample($isNearestFilter);?>
						ifgcolor[0] = sampleout;
						frac += fracstep;
						}
						{
<?						Sample($isNearestFilter);?>
						ifgcolor[1] = sampleout;
						frac += fracstep;
						}
						__m128i fgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)ifgcolor), _mm_setzero_si128());

						// Shade
<?						Shade($isSimpleShade);?>

						// Blend
<?						Blend($blendVariant);?>

						_mm_storel_epi64((__m128i*)desttmp, outcolor);
						dest[offset] = desttmp[0];
						dest[offset + pitch] = desttmp[1];
					}
					
					if (ssecount * 2 != count)
					{
						int index = ssecount * 2;
						int offset = index * pitch;
<?						if ($blendVariant != "opaque") { ?>
						__m128i bgcolor = _mm_unpacklo_epi8(_mm_cvtsi32_si128(dest[offset]), _mm_setzero_si128());
<?						} ?>
						
						// Sample
						unsigned int ifgcolor[2];
<?						Sample($isNearestFilter);?>
						ifgcolor[0] = sampleout;
						ifgcolor[1] = 0;
						__m128i fgcolor = _mm_unpacklo_epi8(_mm_loadl_epi64((__m128i*)ifgcolor), _mm_setzero_si128());

						// Shade
<?						Shade($isSimpleShade);?>

						// Blend
<?						Blend($blendVariant);?>

						dest[offset] = _mm_cvtsi128_si32(outcolor);
					}
<?	}

	function Sample($isNearestFilter)
	{
		if ($isNearestFilter == true)
		{ ?>
						int sample_index = ((frac >> FRACBITS) * textureheight) >> FRACBITS;
						unsigned int sampleout = source[sample_index];
<?		}
		else
		{ ?>
						unsigned int frac_y0 = (frac >> FRACBITS) * textureheight;
						unsigned int frac_y1 = ((frac + one) >> FRACBITS) * textureheight;
						unsigned int y0 = frac_y0 >> FRACBITS;
						unsigned int y1 = frac_y1 >> FRACBITS;

						unsigned int p00 = source[y0];
						unsigned int p01 = source[y1];
						unsigned int p10 = source2[y0];
						unsigned int p11 = source2[y1];

						unsigned int inv_b = texturefracx;
						unsigned int inv_a = (frac_y1 >> (FRACBITS - 4)) & 15;
						unsigned int a = 16 - inv_a;
						unsigned int b = 16 - inv_b;
						
						unsigned int sred = (RPART(p00) * (a * b) + RPART(p01) * (inv_a * b) + RPART(p10) * (a * inv_b) + RPART(p11) * (inv_a * inv_b) + 127) >> 8;
						unsigned int sgreen = (GPART(p00) * (a * b) + GPART(p01) * (inv_a * b) + GPART(p10) * (a * inv_b) + GPART(p11) * (inv_a * inv_b) + 127) >> 8;
						unsigned int sblue = (BPART(p00) * (a * b) + BPART(p01) * (inv_a * b) + BPART(p10) * (a * inv_b) + BPART(p11) * (inv_a * inv_b) + 127) >> 8;
						unsigned int salpha = (APART(p00) * (a * b) + APART(p01) * (inv_a * b) + APART(p10) * (a * inv_b) + APART(p11) * (inv_a * inv_b) + 127) >> 8;

						unsigned int sampleout = (salpha << 24) | (sred << 16) | (sgreen << 8) | sblue;
<?		}
	}
		
	function Shade($isSimpleShade)
	{
		if ($isSimpleShade == true)
		{ ?>
						fgcolor = _mm_srli_epi16(_mm_mullo_epi16(fgcolor, mlight), 8);
<?		}
		else
		{ ?>
						int blue0 = BPART(ifgcolor[0]);
						int green0 = GPART(ifgcolor[0]);
						int red0 = RPART(ifgcolor[0]);
						int intensity0 = ((red0 * 77 + green0 * 143 + blue0 * 37) >> 8) * desaturate;

						int blue1 = BPART(ifgcolor[1]);
						int green1 = GPART(ifgcolor[1]);
						int red1 = RPART(ifgcolor[1]);
						int intensity1 = ((red1 * 77 + green1 * 143 + blue1 * 37) >> 8) * desaturate;

						__m128i intensity = _mm_set_epi16(0, intensity1, intensity1, intensity1, 0, intensity0, intensity0, intensity0);

						fgcolor = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(fgcolor, inv_desaturate), intensity), 8);
						fgcolor = _mm_mullo_epi16(fgcolor, mlight);
						fgcolor = _mm_srli_epi16(_mm_add_epi16(shade_fade, fgcolor), 8);
						fgcolor = _mm_srli_epi16(_mm_mullo_epi16(fgcolor, shade_light), 8);
<?		}
	}
		
	function Blend($blendVariant)
	{
		if ($blendVariant == "opaque")
		{ ?>
						__m128i outcolor = fgcolor;
						outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
<?		}
		else if ($blendVariant == "masked")
		{
						CalcAlpha(); ?>
						fgcolor = _mm_mullo_epi16(fgcolor, alpha);
						bgcolor = _mm_mullo_epi16(bgcolor, inv_alpha);
						__m128i outcolor = _mm_srli_epi16(_mm_add_epi16(fgcolor, bgcolor), 8);
						outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
						outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
<?		}
		else if ($blendVariant == "add" || $blendVariant == "addclamp")
		{
						CalcBlendColor(); ?>

						__m128i out_lo = _mm_srai_epi16(_mm_add_epi32(fg_lo, bg_lo), 8);
						__m128i out_hi = _mm_srai_epi16(_mm_add_epi32(fg_hi, bg_hi), 8);
						__m128i outcolor = _mm_packs_epi32(fg_lo, fg_hi);
						outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
						outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
<?		}
		else if ($blendVariant == "subclamp")
		{
						CalcBlendColor(); ?>

						__m128i out_lo = _mm_srai_epi16(_mm_sub_epi32(fg_lo, bg_lo), 8);
						__m128i out_hi = _mm_srai_epi16(_mm_sub_epi32(fg_hi, bg_hi), 8);
						__m128i outcolor = _mm_packs_epi32(fg_lo, fg_hi);
						outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
						outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
<?		}
		else if ($blendVariant == "revsubclamp")
		{
						CalcBlendColor(); ?>

						__m128i out_lo = _mm_srai_epi16(_mm_sub_epi32(bg_lo, fg_lo), 8);
						__m128i out_hi = _mm_srai_epi16(_mm_sub_epi32(bg_hi, fg_hi), 8);
						__m128i outcolor = _mm_packs_epi32(fg_lo, fg_hi);
						outcolor = _mm_packus_epi16(outcolor, _mm_setzero_si128());
						outcolor = _mm_or_si128(outcolor, _mm_set1_epi32(0xff000000));
<?		}
	}
	
	function CalcAlpha()
	{ ?>
						__m128i alpha = _mm_shufflelo_epi16(fgcolor, _MM_SHUFFLE(3,3,3,3));
						alpha = _mm_shufflehi_epi16(fgcolor, _MM_SHUFFLE(3,3,3,3));
						alpha = _mm_add_epi16(alpha, _mm_srli_epi16(alpha, 7)); // 255 -> 256
						__m128i inv_alpha = _mm_sub_epi16(_mm_set1_epi16(256), alpha);
<?	}
	
	function CalcBlendColor()
	{
						CalcAlpha();?>

						__m128i bgalpha = _mm_mullo_epi16(destalpha, alpha);
						bgalpha = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(bgalpha, _mm_slli_epi16(inv_alpha, 8)), _mm_set1_epi16(128)), 8);
						
						__m128i fgalpha = _mm_mullo_epi16(srcalpha, alpha);
						fgalpha = _mm_srli_epi16(_mm_add_epi16(fgalpha, _mm_set1_epi16(128)), 8);
						
						fgcolor = _mm_mullo_epi16(fgcolor, fgalpha);
						bgcolor = _mm_mullo_epi16(bgcolor, bgalpha);
						
						__m128i fg_lo = _mm_unpacklo_epi16(fgcolor, _mm_setzero_si128());
						__m128i bg_lo = _mm_unpacklo_epi16(bgcolor, _mm_setzero_si128());
						__m128i fg_hi = _mm_unpackhi_epi16(fgcolor, _mm_setzero_si128());
						__m128i bg_hi = _mm_unpackhi_epi16(bgcolor, _mm_setzero_si128());
<?	}
	
?>
}
