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

#include <stdlib.h>
#include "templates.h"
#include "doomdef.h"
#include "sbar.h"
#include "r_data/r_translate.h"
#include "r_model.h"
#include "r_data/r_vanillatrans.h"
#include "actorinlines.h"
#include "i_time.h"
#include "swrenderer/r_memory.h"
#include "swrenderer/r_swcolormaps.h"
#include "swrenderer/viewport/r_viewport.h"
#include "swrenderer/scene/r_light.h"

namespace swrenderer
{
	void RenderModel::Project(RenderThread *thread, float x, float y, float z, FSpriteModelFrame *smf, AActor *actor)
	{
		// transform the origin point
		double tr_x = x - thread->Viewport->viewpoint.Pos.X;
		double tr_y = y - thread->Viewport->viewpoint.Pos.Y;
		double tz = tr_x * thread->Viewport->viewpoint.TanCos + tr_y * thread->Viewport->viewpoint.TanSin;

		// thing is behind view plane?
		if (tz < MINZ)
			return;

		double tx = tr_x * thread->Viewport->viewpoint.Sin - tr_y * thread->Viewport->viewpoint.Cos;

		// Flip for mirrors
		if (thread->Portal->MirrorFlags & RF_XFLIP)
		{
			tx = viewwidth - tx - 1;
		}

		// too far off the side?
		if (fabs(tx / 64) > fabs(tz))
			return;

		RenderModel *vis = thread->FrameMemory->NewObject<RenderModel>(x, y, z, smf, actor, float(1 / tz));
		vis->CurrentPortalUniq = thread->Portal->CurrentPortalUniq;
		vis->WorldToClip = thread->Viewport->WorldToClip;
		vis->MirrorWorldToClip = !!(thread->Portal->MirrorFlags & RF_XFLIP);
		thread->SpriteList->Push(vis);
	}

	RenderModel::RenderModel(float x, float y, float z, FSpriteModelFrame *smf, AActor *actor, float idepth) : x(x), y(y), z(z), smf(smf), actor(actor)
	{
		gpos = { x, y, z };
		this->idepth = idepth;
	}

	void RenderModel::Render(RenderThread *thread, short *cliptop, short *clipbottom, int minZ, int maxZ, Fake3DTranslucent clip3DFloor)
	{
		SWModelRenderer renderer(thread, clip3DFloor, &WorldToClip, MirrorWorldToClip);
		renderer.AddLights(actor);
		renderer.RenderModel(x, y, z, smf, actor);
	}

	/////////////////////////////////////////////////////////////////////////////

	void RenderHUDModel(RenderThread *thread, DPSprite *psp, float ofsx, float ofsy)
	{
		SWModelRenderer renderer(thread, Fake3DTranslucent(), &thread->Viewport->WorldToClip, false);
		renderer.RenderHUDModel(psp, ofsx, ofsy);
	}

	/////////////////////////////////////////////////////////////////////////////

	SWModelRenderer::SWModelRenderer(RenderThread *thread, Fake3DTranslucent clip3DFloor, Mat4f *worldToClip, bool mirrorWorldToClip)
		: Thread(thread), Clip3DFloor(clip3DFloor), WorldToClip(worldToClip), MirrorWorldToClip(mirrorWorldToClip)
	{
	}

	void SWModelRenderer::AddLights(AActor *actor)
	{
		if (gl_lights && actor)
		{
			auto &addedLights = Thread->AddedLightsArray;

			addedLights.Clear();

			float x = (float)actor->X();
			float y = (float)actor->Y();
			float z = (float)actor->Center();
			float radiusSquared = (float)(actor->renderradius * actor->renderradius);

			BSPWalkCircle(x, y, radiusSquared, [&](subsector_t *subsector) // Iterate through all subsectors potentially touched by actor
			{
				FLightNode * node = subsector->lighthead;
				while (node) // check all lights touching a subsector
				{
					ADynamicLight *light = node->lightsource;
					if (light->visibletoplayer && !(light->flags2&MF2_DORMANT) && (!(light->lightflags&LF_DONTLIGHTSELF) || light->target != actor) && !(light->lightflags&LF_DONTLIGHTACTORS))
					{
						int group = subsector->sector->PortalGroup;
						DVector3 pos = light->PosRelative(group);
						float radius = (float)(light->GetRadius() + actor->renderradius);
						double dx = pos.X - x;
						double dy = pos.Y - y;
						double dz = pos.Z - z;
						double distSquared = dx * dx + dy * dy + dz * dz;
						if (distSquared < radius * radius) // Light and actor touches
						{
							if (std::find(addedLights.begin(), addedLights.end(), light) == addedLights.end()) // Check if we already added this light from a different subsector
							{
								addedLights.Push(light);
							}
						}
					}
					node = node->nextLight;
				}
			});

			NumLights = addedLights.Size();
			Lights = Thread->FrameMemory->AllocMemory<PolyLight>(NumLights);
			for (int i = 0; i < NumLights; i++)
			{
				ADynamicLight *lightsource = addedLights[i];

				bool is_point_light = (lightsource->lightflags & LF_ATTENUATE) != 0;

				uint32_t red = lightsource->GetRed();
				uint32_t green = lightsource->GetGreen();
				uint32_t blue = lightsource->GetBlue();

				PolyLight &light = Lights[i];
				light.x = (float)lightsource->X();
				light.y = (float)lightsource->Y();
				light.z = (float)lightsource->Z();
				light.radius = 256.0f / lightsource->GetRadius();
				light.color = (red << 16) | (green << 8) | blue;
				if (is_point_light)
					light.radius = -light.radius;
			}
		}
	}

	void SWModelRenderer::BeginDrawModel(AActor *actor, FSpriteModelFrame *smf, const VSMatrix &objectToWorldMatrix, bool mirrored)
	{
		ModelActor = actor;
		const_cast<VSMatrix &>(objectToWorldMatrix).copy(ObjectToWorld.Matrix);

		ClipTop = {};
		ClipBottom = {};
		if (Clip3DFloor.clipTop || Clip3DFloor.clipBottom)
		{
			// Convert 3d floor clipping planes from world to object space

			VSMatrix inverseMat;
			const_cast<VSMatrix &>(objectToWorldMatrix).inverseMatrix(inverseMat);
			Mat4f worldToObject;
			inverseMat.copy(worldToObject.Matrix);

			// Note: Y and Z is swapped here

			Vec4f one = worldToObject * Vec4f(0.0f, 1.0f, 0.0f, 1.0f);
			Vec4f zero = worldToObject * Vec4f(0.0f, 0.0f, 0.0f, 1.0f);
			Vec4f up = { one.X - zero.X, one.Y - zero.Y, one.Z - zero.Z };

			if (Clip3DFloor.clipTop)
			{
				Vec4f p = worldToObject * Vec4f(0.0f, Clip3DFloor.sclipTop, 0.0f, 1.0f);
				float d = up.X * p.X + up.Y * p.Y + up.Z * p.Z;
				ClipTop = { -up.X, -up.Y, -up.Z, d };
			}

			if (Clip3DFloor.clipBottom)
			{
				Vec4f p = worldToObject * Vec4f(0.0f, Clip3DFloor.sclipBottom, 0.0f, 1.0f);
				float d = up.X * p.X + up.Y * p.Y + up.Z * p.Z;
				ClipBottom = { up.X, up.Y, up.Z, -d };
			}
		}

		SetTransform();

		if (actor->RenderStyle == LegacyRenderStyles[STYLE_Normal] || !!(smf->flags & MDL_DONTCULLBACKFACES))
			PolyTriangleDrawer::SetTwoSided(Thread->DrawQueue, true);
		PolyTriangleDrawer::SetCullCCW(Thread->DrawQueue, !(mirrored ^ MirrorWorldToClip));
	}

	void SWModelRenderer::EndDrawModel(AActor *actor, FSpriteModelFrame *smf)
	{
		if (actor->RenderStyle == LegacyRenderStyles[STYLE_Normal] || !!(smf->flags & MDL_DONTCULLBACKFACES))
			PolyTriangleDrawer::SetTwoSided(Thread->DrawQueue, false);
		PolyTriangleDrawer::SetCullCCW(Thread->DrawQueue, true);

		ModelActor = nullptr;
	}

	IModelVertexBuffer *SWModelRenderer::CreateVertexBuffer(bool needindex, bool singleframe)
	{
		return new SWModelVertexBuffer(needindex, singleframe);
	}

	void SWModelRenderer::SetVertexBuffer(IModelVertexBuffer *buffer)
	{
	}

	void SWModelRenderer::ResetVertexBuffer()
	{
	}

	VSMatrix SWModelRenderer::GetViewToWorldMatrix()
	{
		// Calculate the WorldToView matrix as it would have looked like without yshearing:
		const auto &Viewpoint = Thread->Viewport->viewpoint;
		const auto &Viewwindow = Thread->Viewport->viewwindow;
		double radPitch = Viewpoint.Angles.Pitch.Normalized180().Radians();
		double angx = cos(radPitch);
		double angy = sin(radPitch) * level.info->pixelstretch;
		double alen = sqrt(angx*angx + angy*angy);
		float adjustedPitch = (float)asin(angy / alen);
		float adjustedViewAngle = (float)(Viewpoint.Angles.Yaw - 90).Radians();
		float ratio = Viewwindow.WidescreenRatio;
		float fovratio = (Viewwindow.WidescreenRatio >= 1.3f) ? 1.333333f : ratio;
		float fovy = (float)(2 * DAngle::ToDegrees(atan(tan(Viewpoint.FieldOfView.Radians() / 2) / fovratio)).Degrees);
		Mat4f altWorldToView =
			Mat4f::Rotate(adjustedPitch, 1.0f, 0.0f, 0.0f) *
			Mat4f::Rotate(adjustedViewAngle, 0.0f, -1.0f, 0.0f) *
			Mat4f::Scale(1.0f, level.info->pixelstretch, 1.0f) *
			Mat4f::SwapYZ() *
			Mat4f::Translate((float)-Viewpoint.Pos.X, (float)-Viewpoint.Pos.Y, (float)-Viewpoint.Pos.Z);

		Mat4f swapYZ = Mat4f::Null();
		swapYZ.Matrix[0 + 0 * 4] = 1.0f;
		swapYZ.Matrix[1 + 2 * 4] = 1.0f;
		swapYZ.Matrix[2 + 1 * 4] = 1.0f;
		swapYZ.Matrix[3 + 3 * 4] = 1.0f;

		VSMatrix worldToView;
		worldToView.loadMatrix((altWorldToView * swapYZ).Matrix);

		VSMatrix objectToWorld;
		worldToView.inverseMatrix(objectToWorld);
		return objectToWorld;
	}

	void SWModelRenderer::BeginDrawHUDModel(AActor *actor, const VSMatrix &objectToWorldMatrix, bool mirrored)
	{
		ModelActor = actor;
		const_cast<VSMatrix &>(objectToWorldMatrix).copy(ObjectToWorld.Matrix);
		ClipTop = {};
		ClipBottom = {};
		SetTransform();
		PolyTriangleDrawer::SetWeaponScene(Thread->DrawQueue, true);

		if (actor->RenderStyle == LegacyRenderStyles[STYLE_Normal])
			PolyTriangleDrawer::SetTwoSided(Thread->DrawQueue, true);
		PolyTriangleDrawer::SetCullCCW(Thread->DrawQueue, !(mirrored ^ MirrorWorldToClip));
	}

	void SWModelRenderer::EndDrawHUDModel(AActor *actor)
	{
		ModelActor = nullptr;
		PolyTriangleDrawer::SetWeaponScene(Thread->DrawQueue, false);

		if (actor->RenderStyle == LegacyRenderStyles[STYLE_Normal])
			PolyTriangleDrawer::SetTwoSided(Thread->DrawQueue, false);
		PolyTriangleDrawer::SetCullCCW(Thread->DrawQueue, true);
	}

	void SWModelRenderer::SetInterpolation(double interpolation)
	{
		InterpolationFactor = (float)interpolation;
	}

	void SWModelRenderer::SetMaterial(FTexture *skin, bool clampNoFilter, int translation)
	{
		SkinTexture = skin;
	}

	void SWModelRenderer::SetTransform()
	{
		Mat4f swapYZ = Mat4f::Null();
		swapYZ.Matrix[0 + 0 * 4] = 1.0f;
		swapYZ.Matrix[1 + 2 * 4] = 1.0f;
		swapYZ.Matrix[2 + 1 * 4] = 1.0f;
		swapYZ.Matrix[3 + 3 * 4] = 1.0f;
		ObjectToWorld = swapYZ * ObjectToWorld;

		PolyTriangleDrawer::SetTransform(Thread->DrawQueue, Thread->FrameMemory->NewObject<Mat4f>((*WorldToClip) * ObjectToWorld), Thread->FrameMemory->NewObject<Mat4f>(ObjectToWorld));
	}

	void SWModelRenderer::DrawArrays(int start, int count)
	{
		const auto &viewpoint = Thread->Viewport->viewpoint;

		bool foggy = false;
		int actualextralight = foggy ? 0 : viewpoint.extralight << 4;
		sector_t *sector = ModelActor->Sector;

		bool fullbrightSprite = ((ModelActor->renderflags & RF_FULLBRIGHT) || (ModelActor->flags5 & MF5_BRIGHT));
		int lightlevel = fullbrightSprite ? 255 : ModelActor->Sector->lightlevel + actualextralight;

		PolyDrawArgs args;
		args.SetLight(GetColorTable(sector->Colormap, sector->SpecialColors[sector_t::sprites], true), lightlevel, Thread->Light->SpriteGlobVis(foggy), fullbrightSprite);
		args.SetLights(Lights, NumLights);
		args.SetNormal(FVector3(0.0f, 0.0f, 0.0f));
		args.SetStyle(ModelActor->RenderStyle, ModelActor->Alpha, ModelActor->fillcolor, ModelActor->Translation, SkinTexture, fullbrightSprite);
		args.SetDepthTest(true);
		args.SetWriteDepth(true);
		args.SetWriteStencil(false);
		args.SetClipPlane(0, PolyClipPlane());
		args.SetClipPlane(1, ClipTop);
		args.SetClipPlane(2, ClipBottom);

		args.DrawArray(Thread->DrawQueue, VertexBuffer + start, count);
	}

	void SWModelRenderer::DrawElements(int numIndices, size_t offset)
	{
		const auto &viewpoint = Thread->Viewport->viewpoint;

		bool foggy = false;
		int actualextralight = foggy ? 0 : viewpoint.extralight << 4;
		sector_t *sector = ModelActor->Sector;

		bool fullbrightSprite = ((ModelActor->renderflags & RF_FULLBRIGHT) || (ModelActor->flags5 & MF5_BRIGHT));
		int lightlevel = fullbrightSprite ? 255 : ModelActor->Sector->lightlevel + actualextralight;

		PolyDrawArgs args;
		args.SetLight(GetColorTable(sector->Colormap, sector->SpecialColors[sector_t::sprites], true), lightlevel, Thread->Light->SpriteGlobVis(foggy), fullbrightSprite);
		args.SetLights(Lights, NumLights);
		args.SetNormal(FVector3(0.0f, 0.0f, 0.0f));
		args.SetStyle(ModelActor->RenderStyle, ModelActor->Alpha, ModelActor->fillcolor, ModelActor->Translation, SkinTexture, fullbrightSprite);
		args.SetDepthTest(true);
		args.SetWriteDepth(true);
		args.SetWriteStencil(false);
		args.SetClipPlane(0, PolyClipPlane());
		args.SetClipPlane(1, ClipTop);
		args.SetClipPlane(2, ClipBottom);

		args.DrawElements(Thread->DrawQueue, VertexBuffer, IndexBuffer + offset / sizeof(unsigned int), numIndices);
	}

	/////////////////////////////////////////////////////////////////////////////

	SWModelVertexBuffer::SWModelVertexBuffer(bool needindex, bool singleframe)
	{
	}

	SWModelVertexBuffer::~SWModelVertexBuffer()
	{
	}

	FModelVertex *SWModelVertexBuffer::LockVertexBuffer(unsigned int size)
	{
		mVertexBuffer.Resize(size);
		return &mVertexBuffer[0];
	}

	void SWModelVertexBuffer::UnlockVertexBuffer()
	{
	}

	unsigned int *SWModelVertexBuffer::LockIndexBuffer(unsigned int size)
	{
		mIndexBuffer.Resize(size);
		return &mIndexBuffer[0];
	}

	void SWModelVertexBuffer::UnlockIndexBuffer()
	{
	}

	void SWModelVertexBuffer::SetupFrame(FModelRenderer *renderer, unsigned int frame1, unsigned int frame2, unsigned int size)
	{
		SWModelRenderer *swrenderer = (SWModelRenderer *)renderer;

		if (frame1 == frame2 || size == 0 || swrenderer->InterpolationFactor == 0.f)
		{
			TriVertex *vertices = swrenderer->Thread->FrameMemory->AllocMemory<TriVertex>(size);

			for (unsigned int i = 0; i < size; i++)
			{
				vertices[i] =
				{
					mVertexBuffer[frame1 + i].x,
					mVertexBuffer[frame1 + i].y,
					mVertexBuffer[frame1 + i].z,
					1.0f,
					mVertexBuffer[frame1 + i].u,
					mVertexBuffer[frame1 + i].v
				};
			}

			swrenderer->VertexBuffer = vertices;
			swrenderer->IndexBuffer = &mIndexBuffer[0];
		}
		else
		{
			TriVertex *vertices = swrenderer->Thread->FrameMemory->AllocMemory<TriVertex>(size);

			float frac = swrenderer->InterpolationFactor;
			float inv_frac = 1.0f - frac;
			for (unsigned int i = 0; i < size; i++)
			{
				vertices[i].x = mVertexBuffer[frame1 + i].x * inv_frac + mVertexBuffer[frame2 + i].x * frac;
				vertices[i].y = mVertexBuffer[frame1 + i].y * inv_frac + mVertexBuffer[frame2 + i].y * frac;
				vertices[i].z = mVertexBuffer[frame1 + i].z * inv_frac + mVertexBuffer[frame2 + i].z * frac;
				vertices[i].w = 1.0f;
				vertices[i].u = mVertexBuffer[frame1 + i].u;
				vertices[i].v = mVertexBuffer[frame1 + i].v;
			}

			swrenderer->VertexBuffer = vertices;
			swrenderer->IndexBuffer = &mIndexBuffer[0];
		}
	}
}
