
#pragma once

#include "polyrenderer/math/gpu_types.h"
#include "hwrenderer/scene/hw_viewpointuniforms.h"
#include "hwrenderer/scene/hw_renderstate.h"

#ifndef NO_SSE
#include <xmmintrin.h>
#endif

class ShadedTriVertex
{
public:
	Vec4f gl_Position;
	float gl_ClipDistance[5];
	Vec4f vTexCoord;
	Vec4f vColor;
	Vec4f pixelpos;
	//Vec3f glowdist;
	Vec3f gradientdist;
	//Vec4f vEyeNormal;
	Vec4f vWorldNormal;
};

class PolyMainVertexShader : public ShadedTriVertex
{
public:
	// Input
	Vec4f aPosition;
	Vec2f aTexCoord;
	Vec4f aColor;
	Vec4f aVertex2;
	Vec4f aNormal;
	Vec4f aNormal2;

	// Defines
	bool SIMPLE = false;
	bool SPHEREMAP = false;

	// Uniforms
	VSMatrix ModelMatrix;
	VSMatrix NormalModelMatrix;
	VSMatrix TextureMatrix;
	StreamData Data;
	Vec2f uClipSplit;
	const HWViewpointUniforms *Viewpoint = nullptr;

	void main()
	{
		Vec2f parmTexCoord = aTexCoord;
		Vec4f parmPosition = aPosition;

		Vec4f worldcoord;
		if (SIMPLE)
			worldcoord = mul(ModelMatrix, mix(parmPosition, aVertex2, Data.uInterpolationFactor));
		else
			worldcoord = mul(ModelMatrix, parmPosition);

		Vec4f eyeCoordPos = mul(Viewpoint->mViewMatrix, worldcoord);

		vColor = aColor;

		if (!SIMPLE)
		{
			pixelpos.X = worldcoord.X;
			pixelpos.Y = worldcoord.Y;
			pixelpos.Z = worldcoord.Z;
			pixelpos.W = -eyeCoordPos.Z / eyeCoordPos.W;

			/*if (Data.uGlowTopColor.W > 0 || Data.uGlowBottomColor.W > 0)
			{
				float topatpoint = (Data.uGlowTopPlane.W + Data.uGlowTopPlane.X * worldcoord.X + Data.uGlowTopPlane.Y * worldcoord.Z) * Data.uGlowTopPlane.Z;
				float bottomatpoint = (Data.uGlowBottomPlane.W + Data.uGlowBottomPlane.X * worldcoord.X + Data.uGlowBottomPlane.Y * worldcoord.Z) * Data.uGlowBottomPlane.Z;
				glowdist.X = topatpoint - worldcoord.Y;
				glowdist.Y = worldcoord.Y - bottomatpoint;
				glowdist.Z = clamp(glowdist.X / (topatpoint - bottomatpoint), 0.0f, 1.0f);
			}*/

			if (Data.uObjectColor2.a != 0)
			{
				float topatpoint = (Data.uGradientTopPlane.W + Data.uGradientTopPlane.X * worldcoord.X + Data.uGradientTopPlane.Y * worldcoord.Z) * Data.uGradientTopPlane.Z;
				float bottomatpoint = (Data.uGradientBottomPlane.W + Data.uGradientBottomPlane.X * worldcoord.X + Data.uGradientBottomPlane.Y * worldcoord.Z) * Data.uGradientBottomPlane.Z;
				gradientdist.X = topatpoint - worldcoord.Y;
				gradientdist.Y = worldcoord.Y - bottomatpoint;
				gradientdist.Z = clamp(gradientdist.X / (topatpoint - bottomatpoint), 0.0f, 1.0f);
			}

			if (Data.uSplitBottomPlane.Z != 0.0f)
			{
				gl_ClipDistance[3] = ((Data.uSplitTopPlane.W + Data.uSplitTopPlane.X * worldcoord.X + Data.uSplitTopPlane.Y * worldcoord.Z) * Data.uSplitTopPlane.Z) - worldcoord.Y;
				gl_ClipDistance[4] = worldcoord.Y - ((Data.uSplitBottomPlane.W + Data.uSplitBottomPlane.X * worldcoord.X + Data.uSplitBottomPlane.Y * worldcoord.Z) * Data.uSplitBottomPlane.Z);
			}

			vWorldNormal = mul(NormalModelMatrix, Vec4f(normalize(mix3(aNormal, aNormal2, Data.uInterpolationFactor)), 1.0f));
			//vEyeNormal = mul(Viewpoint->mNormalViewMatrix, vWorldNormal);
		}

		if (!SPHEREMAP)
		{
			vTexCoord = mul(TextureMatrix, Vec4f(parmTexCoord, 0.0f, 1.0f));
		}
		else
		{
			Vec3f u = normalize3(eyeCoordPos);
			Vec3f n = normalize3(mul(Viewpoint->mNormalViewMatrix, Vec4f(parmTexCoord.X, 0.0f, parmTexCoord.Y, 0.0f)));
			Vec3f r = reflect(u, n);
			float m = 2.0f * sqrt(r.X*r.X + r.Y*r.Y + (r.Z + 1.0f)*(r.Z + 1.0f));
			vTexCoord.X = r.X / m + 0.5f;
			vTexCoord.Y = r.Y / m + 0.5f;
		}

		gl_Position = mul(Viewpoint->mProjectionMatrix, eyeCoordPos);

		if (Viewpoint->mClipHeightDirection != 0.0f) // clip planes used for reflective flats
		{
			gl_ClipDistance[0] = (worldcoord.Y - Viewpoint->mClipHeight) * Viewpoint->mClipHeightDirection;
		}
		else if (Viewpoint->mClipLine.X > -1000000.0f) // and for line portals - this will never be active at the same time as the reflective planes clipping so it can use the same hardware clip plane.
		{
			gl_ClipDistance[0] = -((worldcoord.Z - Viewpoint->mClipLine.Y) * Viewpoint->mClipLine.Z + (Viewpoint->mClipLine.X - worldcoord.X) * Viewpoint->mClipLine.W) + 1.0f / 32768.0f;	// allow a tiny bit of imprecisions for colinear linedefs.
		}
		else
		{
			gl_ClipDistance[0] = 1.0f;
		}

		// clip planes used for translucency splitting
		gl_ClipDistance[1] = worldcoord.Y - uClipSplit.X;
		gl_ClipDistance[2] = uClipSplit.Y - worldcoord.Y;

		if (Data.uSplitTopPlane == FVector4(0.0f, 0.0f, 0.0f, 0.0f))
		{
			gl_ClipDistance[3] = 1.0f;
			gl_ClipDistance[4] = 1.0f;
		}
	}

private:
	static Vec3f normalize(const Vec3f &a)
	{
		float rcplen = 1.0f / sqrt(a.X * a.X + a.Y * a.Y + a.Z * a.Z);
		return Vec3f(a.X * rcplen, a.Y * rcplen, a.Z * rcplen);
	}

	static Vec3f normalize3(const Vec4f &a)
	{
		float rcplen = 1.0f / sqrt(a.X * a.X + a.Y * a.Y + a.Z * a.Z);
		return Vec3f(a.X * rcplen, a.Y * rcplen, a.Z * rcplen);
	}

	static Vec4f mix(const Vec4f &a, const Vec4f &b, float t)
	{
		float invt = 1.0f - t;
		return Vec4f(a.X * invt + b.X * t, a.Y * invt + b.Y * t, a.Z * invt + b.Z * t, a.W * invt + b.W * t);
	}

	static Vec3f mix3(const Vec4f &a, const Vec4f &b, float t)
	{
		float invt = 1.0f - t;
		return Vec3f(a.X * invt + b.X * t, a.Y * invt + b.Y * t, a.Z * invt + b.Z * t);
	}

	static Vec3f reflect(const Vec3f &u, const Vec3f &n)
	{
		float d = 2.0f * (n.X * u.X + n.Y * u.Y + n.Z * u.Z);
		return Vec3f(u.X - d * n.X, u.Y - d * n.Y, u.Z - d * n.Z);
	}

	static Vec4f mul(const VSMatrix &mat, const Vec4f &v)
	{
		const float *m = mat.get();

		Vec4f result;
#ifdef NO_SSE
		result.X = m[0 * 4 + 0] * v.X + m[1 * 4 + 0] * v.Y + m[2 * 4 + 0] * v.Z + m[3 * 4 + 0] * v.W;
		result.Y = m[0 * 4 + 1] * v.X + m[1 * 4 + 1] * v.Y + m[2 * 4 + 1] * v.Z + m[3 * 4 + 1] * v.W;
		result.Z = m[0 * 4 + 2] * v.X + m[1 * 4 + 2] * v.Y + m[2 * 4 + 2] * v.Z + m[3 * 4 + 2] * v.W;
		result.W = m[0 * 4 + 3] * v.X + m[1 * 4 + 3] * v.Y + m[2 * 4 + 3] * v.Z + m[3 * 4 + 3] * v.W;
#else
		__m128 m0 = _mm_loadu_ps(m);
		__m128 m1 = _mm_loadu_ps(m + 4);
		__m128 m2 = _mm_loadu_ps(m + 8);
		__m128 m3 = _mm_loadu_ps(m + 12);
		__m128 mv = _mm_loadu_ps(&v.X);
		m0 = _mm_mul_ps(m0, _mm_shuffle_ps(mv, mv, _MM_SHUFFLE(0, 0, 0, 0)));
		m1 = _mm_mul_ps(m1, _mm_shuffle_ps(mv, mv, _MM_SHUFFLE(1, 1, 1, 1)));
		m2 = _mm_mul_ps(m2, _mm_shuffle_ps(mv, mv, _MM_SHUFFLE(2, 2, 2, 2)));
		m3 = _mm_mul_ps(m3, _mm_shuffle_ps(mv, mv, _MM_SHUFFLE(3, 3, 3, 3)));
		mv = _mm_add_ps(_mm_add_ps(_mm_add_ps(m0, m1), m2), m3);
		_mm_storeu_ps(&result.X, mv);
#endif
		return result;
	}
};
