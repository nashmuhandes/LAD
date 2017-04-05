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

#include <cstdint>
#include <vector>

class FString;
class PolyDrawArgs;

struct WorkerThreadData
{
	int32_t core;
	int32_t num_cores;
};

struct TriVertex
{
	TriVertex() { }
	TriVertex(float x, float y, float z, float w, float u, float v) : x(x), y(y), z(z), w(w), u(u), v(v) { }

	float x, y, z, w;
	float u, v;
};

struct ScreenTriangleStepVariables
{
	float W, U, V;
};

struct TriDrawTriangleArgs
{
	uint8_t *dest;
	int32_t pitch;
	TriVertex *v1;
	TriVertex *v2;
	TriVertex *v3;
	int32_t clipright;
	int32_t clipbottom;
	uint8_t *stencilValues;
	uint32_t *stencilMasks;
	int32_t stencilPitch;
	uint32_t *subsectorGBuffer;
	const PolyDrawArgs *uniforms;
	bool destBgra;
	ScreenTriangleStepVariables gradientX;
	ScreenTriangleStepVariables gradientY;

	void CalculateGradients()
	{
		gradientX.W = FindGradientX(v1->x, v1->y, v2->x, v2->y, v3->x, v3->y, v1->w, v2->w, v3->w);
		gradientY.W = FindGradientY(v1->x, v1->y, v2->x, v2->y, v3->x, v3->y, v1->w, v2->w, v3->w);
		gradientX.U = FindGradientX(v1->x, v1->y, v2->x, v2->y, v3->x, v3->y, v1->u * v1->w, v2->u * v2->w, v3->u * v3->w);
		gradientY.U = FindGradientY(v1->x, v1->y, v2->x, v2->y, v3->x, v3->y, v1->u * v1->w, v2->u * v2->w, v3->u * v3->w);
		gradientX.V = FindGradientX(v1->x, v1->y, v2->x, v2->y, v3->x, v3->y, v1->v * v1->w, v2->v * v2->w, v3->v * v3->w);
		gradientY.V = FindGradientY(v1->x, v1->y, v2->x, v2->y, v3->x, v3->y, v1->v * v1->w, v2->v * v2->w, v3->v * v3->w);
	}

private:
	static float FindGradientX(float x0, float y0, float x1, float y1, float x2, float y2, float c0, float c1, float c2)
	{
		float top = (c1 - c2) * (y0 - y2) - (c0 - c2) * (y1 - y2);
		float bottom = (x1 - x2) * (y0 - y2) - (x0 - x2) * (y1 - y2);
		return top / bottom;
	}

	static float FindGradientY(float x0, float y0, float x1, float y1, float x2, float y2, float c0, float c1, float c2)
	{
		float top = (c1 - c2) * (x0 - x2) - (c0 - c2) * (x1 - x2);
		float bottom = (x0 - x2) * (y1 - y2) - (x1 - x2) * (y0 - y2);
		return top / bottom;
	}
};

class RectDrawArgs;

enum class TriBlendMode
{
	TextureOpaque,
	TextureMasked,
	TextureAdd,
	TextureSub,
	TextureRevSub,
	TextureAddSrcColor,
	TranslatedOpaque,
	TranslatedMasked,
	TranslatedAdd,
	TranslatedSub,
	TranslatedRevSub,
	TranslatedAddSrcColor,
	Shaded,
	AddShaded,
	Stencil,
	AddStencil,
	FillOpaque,
	FillAdd,
	FillSub,
	FillRevSub,
	FillAddSrcColor,
	Skycap,
	Fuzz
};

class ScreenTriangle
{
public:
	static void Draw(const TriDrawTriangleArgs *args, WorkerThreadData *thread);

	static void(*TriDrawers8[])(int, int, uint32_t, uint32_t, const TriDrawTriangleArgs *);
	static void(*TriDrawers32[])(int, int, uint32_t, uint32_t, const TriDrawTriangleArgs *);
	static void(*RectDrawers8[])(const void *, int, int, int, const RectDrawArgs *, WorkerThreadData *);
	static void(*RectDrawers32[])(const void *, int, int, int, const RectDrawArgs *, WorkerThreadData *);

	static int FuzzStart;
};

namespace TriScreenDrawerModes
{
	enum class BlendModes { Opaque, Masked, AddClamp, SubClamp, RevSubClamp, AddSrcColorOneMinusSrcColor, Shaded, AddClampShaded };
	struct OpaqueBlend { static const int Mode = (int)BlendModes::Opaque; };
	struct MaskedBlend { static const int Mode = (int)BlendModes::Masked; };
	struct AddClampBlend { static const int Mode = (int)BlendModes::AddClamp; };
	struct SubClampBlend { static const int Mode = (int)BlendModes::SubClamp; };
	struct RevSubClampBlend { static const int Mode = (int)BlendModes::RevSubClamp; };
	struct AddSrcColorBlend { static const int Mode = (int)BlendModes::AddSrcColorOneMinusSrcColor; };
	struct ShadedBlend { static const int Mode = (int)BlendModes::Shaded; };
	struct AddClampShadedBlend { static const int Mode = (int)BlendModes::AddClampShaded; };

	enum class FilterModes { Nearest, Linear };
	struct NearestFilter { static const int Mode = (int)FilterModes::Nearest; };
	struct LinearFilter { static const int Mode = (int)FilterModes::Linear; };

	enum class ShadeMode { None, Simple, Advanced };
	struct NoShade { static const int Mode = (int)ShadeMode::None; };
	struct SimpleShade { static const int Mode = (int)ShadeMode::Simple; };
	struct AdvancedShade { static const int Mode = (int)ShadeMode::Advanced; };

	enum class Samplers { Texture, Fill, Shaded, Stencil, Translated, Skycap, Fuzz };
	struct TextureSampler { static const int Mode = (int)Samplers::Texture; };
	struct FillSampler { static const int Mode = (int)Samplers::Fill; };
	struct ShadedSampler { static const int Mode = (int)Samplers::Shaded; };
	struct StencilSampler { static const int Mode = (int)Samplers::Stencil; };
	struct TranslatedSampler { static const int Mode = (int)Samplers::Translated; };
	struct SkycapSampler { static const int Mode = (int)Samplers::Skycap; };
	struct FuzzSampler { static const int Mode = (int)Samplers::Fuzz; };

	static const int fuzzcolormap[FUZZTABLE] =
	{
		 6, 11,  6, 11,  6,  6, 11,  6,  6, 11, 
		 6,  6,  6, 11,  6,  6,  6, 11, 15, 18, 
		21,  6, 11, 15,  6,  6,  6,  6, 11,  6, 
		11,  6,  6, 11, 15,  6,  6, 11, 15, 18, 
		21,  6,  6,  6,  6, 11,  6,  6, 11,  6, 
	};
}
