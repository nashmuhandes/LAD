// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2002-2016 Christoph Oelckers
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
/*
** gl_sprite.cpp
** Sprite/Particle rendering
**
*/

#include "gl/system/gl_system.h"
#include "p_local.h"
#include "p_effect.h"
#include "g_level.h"
#include "doomstat.h"
#include "r_defs.h"
#include "r_sky.h"
#include "r_utility.h"
#include "a_pickups.h"
#include "d_player.h"
#include "g_levellocals.h"
#include "events.h"
#include "actorinlines.h"
#include "r_data/r_vanillatrans.h"
#include "r_data/matrix.h"
#include "r_data/models/models.h"
#include "vectors.h"

#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_fakeflat.h"
#include "hwrenderer/utility/hw_cvars.h"
#include "hwrenderer/utility/hw_clock.h"
#include "hwrenderer/utility/hw_lighting.h"
#include "hwrenderer/textures/hw_material.h"
#include "hwrenderer/dynlights/hw_dynlightdata.h"

extern TArray<spritedef_t> sprites;
extern TArray<spriteframe_t> SpriteFrames;
extern uint32_t r_renderercaps;

const float LARGE_VALUE = 1e19f;

EXTERN_CVAR(Bool, r_debug_disable_vis_filter)
EXTERN_CVAR(Float, transsouls)


//==========================================================================
//
// 
//
//==========================================================================

bool GLSprite::CalculateVertices(HWDrawInfo *di, FVector3 *v)
{
	if (actor != nullptr && (actor->renderflags & RF_SPRITETYPEMASK) == RF_FLATSPRITE)
	{
		Matrix3x4 mat;
		mat.MakeIdentity();

		// [MC] Rotate around the center or offsets given to the sprites.
		// Counteract any existing rotations, then rotate the angle.
		// Tilt the actor up or down based on pitch (increase 'somersaults' forward).
		// Then counteract the roll and DO A BARREL ROLL.

		FAngle pitch = (float)-Angles.Pitch.Degrees;
		pitch.Normalized180();

		mat.Translate(x, z, y);
		mat.Rotate(0, 1, 0, 270. - Angles.Yaw.Degrees);
		mat.Rotate(1, 0, 0, pitch.Degrees);

		if (actor->renderflags & RF_ROLLCENTER)
		{
			float cx = (x1 + x2) * 0.5;
			float cy = (y1 + y2) * 0.5;

			mat.Translate(cx - x, 0, cy - y);
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees);
			mat.Translate(-cx, -z, -cy);
		}
		else
		{
			mat.Rotate(0, 1, 0, - Angles.Roll.Degrees);
			mat.Translate(-x, -z, -y);
		}
		v[0] = mat * FVector3(x2, z, y2);
		v[1] = mat * FVector3(x1, z, y2);
		v[2] = mat * FVector3(x2, z, y1);
		v[3] = mat * FVector3(x1, z, y1);

		return true;
	}
	
	// [BB] Billboard stuff
	const bool drawWithXYBillboard = ((particle && gl_billboard_particles) || (!(actor && actor->renderflags & RF_FORCEYBILLBOARD)
		//&& di->mViewActor != nullptr
		&& (gl_billboard_mode == 1 || (actor && actor->renderflags & RF_FORCEXYBILLBOARD))));

	const bool drawBillboardFacingCamera = gl_billboard_faces_camera;
	// [Nash] has +ROLLSPRITE
	const bool drawRollSpriteActor = (actor != nullptr && actor->renderflags & RF_ROLLSPRITE);


	// [fgsfds] check sprite type mask
	uint32_t spritetype = (uint32_t)-1;
	if (actor != nullptr) spritetype = actor->renderflags & RF_SPRITETYPEMASK;

	// [Nash] is a flat sprite
	const bool isFlatSprite = (actor != nullptr) && (spritetype == RF_WALLSPRITE || spritetype == RF_FLATSPRITE);
	const bool useOffsets = (actor != nullptr) && !(actor->renderflags & RF_ROLLCENTER);

	// [Nash] check for special sprite drawing modes
	if (drawWithXYBillboard || drawBillboardFacingCamera || drawRollSpriteActor || isFlatSprite)
	{
		// Compute center of sprite
		float xcenter = (x1 + x2)*0.5;
		float ycenter = (y1 + y2)*0.5;
		float zcenter = (z1 + z2)*0.5;
		float xx = -xcenter + x;
		float zz = -zcenter + z;
		float yy = -ycenter + y;
		Matrix3x4 mat;
		mat.MakeIdentity();
		mat.Translate(xcenter, zcenter, ycenter); // move to sprite center

												  // Order of rotations matters. Perform yaw rotation (Y, face camera) before pitch (X, tilt up/down).
		if (drawBillboardFacingCamera && !isFlatSprite)
		{
			// [CMB] Rotate relative to camera XY position, not just camera direction,
			// which is nicer in VR
			float xrel = xcenter - r_viewpoint.Pos.X;
			float yrel = ycenter - r_viewpoint.Pos.Y;
			float absAngleDeg = RAD2DEG(atan2(-yrel, xrel));
			float counterRotationDeg = 270. - di->mAngles.Yaw.Degrees; // counteracts existing sprite rotation
			float relAngleDeg = counterRotationDeg + absAngleDeg;

			mat.Rotate(0, 1, 0, relAngleDeg);
		}

		// [fgsfds] calculate yaw vectors
		float yawvecX = 0, yawvecY = 0, rollDegrees = 0;
		float angleRad = (270. - di->mAngles.Yaw).Radians();
		if (actor)	rollDegrees = Angles.Roll.Degrees;
		if (isFlatSprite)
		{
			yawvecX = Angles.Yaw.Cos();
			yawvecY = Angles.Yaw.Sin();
		}

		// [fgsfds] Rotate the sprite about the sight vector (roll) 
		if (spritetype == RF_WALLSPRITE)
		{
			mat.Rotate(0, 1, 0, 0);
			if (drawRollSpriteActor)
			{
				if (useOffsets)	mat.Translate(xx, zz, yy);
				mat.Rotate(yawvecX, 0, yawvecY, rollDegrees);
				if (useOffsets) mat.Translate(-xx, -zz, -yy);
			}
		}
		else if (drawRollSpriteActor)
		{
			if (useOffsets) mat.Translate(xx, zz, yy);
			if (drawWithXYBillboard)
			{
				mat.Rotate(-sin(angleRad), 0, cos(angleRad), -di->mAngles.Pitch.Degrees);
			}
			mat.Rotate(cos(angleRad), 0, sin(angleRad), rollDegrees);
			if (useOffsets) mat.Translate(-xx, -zz, -yy);
		}
		else if (drawWithXYBillboard)
		{
			// Rotate the sprite about the vector starting at the center of the sprite
			// triangle strip and with direction orthogonal to where the player is looking
			// in the x/y plane.
			mat.Rotate(-sin(angleRad), 0, cos(angleRad), -di->mAngles.Pitch.Degrees);
		}

		mat.Translate(-xcenter, -zcenter, -ycenter); // retreat from sprite center
		v[0] = mat * FVector3(x1, z1, y1);
		v[1] = mat * FVector3(x2, z1, y2);
		v[2] = mat * FVector3(x1, z2, y1);
		v[3] = mat * FVector3(x2, z2, y2);
	}
	else // traditional "Y" billboard mode
	{
		v[0] = FVector3(x1, z1, y1);
		v[1] = FVector3(x2, z1, y2);
		v[2] = FVector3(x1, z2, y1);
		v[3] = FVector3(x2, z2, y2);
	}
	return false;
}

//==========================================================================
//
// 
//
//==========================================================================

inline void GLSprite::PutSprite(HWDrawInfo *di, bool translucent)
{
	// That's a lot of checks...
	if (modelframe && RenderStyle.BlendOp != STYLEOP_Shadow && gl_light_sprites && level.HasDynamicLights && di->FixedColormap == CM_DEFAULT && !fullbright && !(screen->hwcaps & RFL_NO_SHADERS))
	{
		hw_GetDynModelLight(actor, lightdata);
		dynlightindex = di->UploadLights(lightdata);
	}
	else
		dynlightindex = -1;


	di->AddSprite(this, translucent);
}

//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::SplitSprite(HWDrawInfo *di, sector_t * frontsector, bool translucent)
{
	GLSprite copySprite;
	double lightbottom;
	unsigned int i;
	bool put=false;
	TArray<lightlist_t> & lightlist=frontsector->e->XFloor.lightlist;

	for(i=0;i<lightlist.Size();i++)
	{
		// Particles don't go through here so we can safely assume that actor is not nullptr
		if (i<lightlist.Size()-1) lightbottom=lightlist[i+1].plane.ZatPoint(actor);
		else lightbottom=frontsector->floorplane.ZatPoint(actor);

		if (lightbottom<z2) lightbottom=z2;

		if (lightbottom<z1)
		{
			copySprite=*this;
			copySprite.lightlevel = hw_ClampLight(*lightlist[i].p_lightlevel);
			copySprite.Colormap.CopyLight(lightlist[i].extra_colormap);

			if (level.flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
			{
				copySprite.Colormap.Decolorize();
			}

			if (!ThingColor.isWhite())
			{
				copySprite.Colormap.LightColor.r = (copySprite.Colormap.LightColor.r*ThingColor.r) >> 8;
				copySprite.Colormap.LightColor.g = (copySprite.Colormap.LightColor.g*ThingColor.g) >> 8;
				copySprite.Colormap.LightColor.b = (copySprite.Colormap.LightColor.b*ThingColor.b) >> 8;
			}

			z1=copySprite.z2=lightbottom;
			vt=copySprite.vb=copySprite.vt+ 
				(lightbottom-copySprite.z1)*(copySprite.vb-copySprite.vt)/(z2-copySprite.z1);
			copySprite.PutSprite(di, translucent);
			put=true;
		}
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::PerformSpriteClipAdjustment(AActor *thing, const DVector2 &thingpos, float spriteheight)
{
	const float NO_VAL = 100000000.0f;
	bool clipthing = (thing->player || thing->flags3&MF3_ISMONSTER || thing->IsKindOf(RUNTIME_CLASS(AInventory))) && (thing->flags&MF_ICECORPSE || !(thing->flags&MF_CORPSE));
	bool smarterclip = !clipthing && gl_spriteclip == 3;
	if (clipthing || gl_spriteclip > 1)
	{

		float btm = NO_VAL;
		float top = -NO_VAL;
		extsector_t::xfloor &x = thing->Sector->e->XFloor;

		if (x.ffloors.Size())
		{
			for (unsigned int i = 0; i < x.ffloors.Size(); i++)
			{
				F3DFloor * ff = x.ffloors[i];
				if (ff->flags & FF_THISINSIDE) continue;	// only relevant for software rendering.
				float floorh = ff->top.plane->ZatPoint(thingpos);
				float ceilingh = ff->bottom.plane->ZatPoint(thingpos);
				if (floorh == thing->floorz)
				{
					btm = floorh;
				}
				if (ceilingh == thing->ceilingz)
				{
					top = ceilingh;
				}
				if (btm != NO_VAL && top != -NO_VAL)
				{
					break;
				}
			}
		}
		else if (thing->Sector->GetHeightSec())
		{
			if (thing->flags2&MF2_ONMOBJ && thing->floorz ==
				thing->Sector->heightsec->floorplane.ZatPoint(thingpos))
			{
				btm = thing->floorz;
				top = thing->ceilingz;
			}
		}
		if (btm == NO_VAL)
			btm = thing->Sector->floorplane.ZatPoint(thing) - thing->Floorclip;
		if (top == NO_VAL)
			top = thing->Sector->ceilingplane.ZatPoint(thingpos);

		// +/-1 to account for the one pixel empty frame around the sprite.
		float diffb = (z2+1) - btm;
		float difft = (z1-1) - top;
		if (diffb >= 0 /*|| !gl_sprite_clip_to_floor*/) diffb = 0;
		// Adjust sprites clipping into ceiling and adjust clipping adjustment for tall graphics
		if (smarterclip)
		{
			// Reduce slightly clipping adjustment of corpses
			if (thing->flags & MF_CORPSE || spriteheight > fabs(diffb))
			{
				float ratio = clamp<float>((fabs(diffb) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
				diffb *= ratio;
			}
			if (!diffb)
			{
				if (difft <= 0) difft = 0;
				if (difft >= (float)gl_sclipthreshold)
				{
					// dumb copy of the above.
					if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || difft > (float)gl_sclipthreshold)
					{
						difft = 0;
					}
				}
				if (spriteheight > fabs(difft))
				{
					float ratio = clamp<float>((fabs(difft) * (float)gl_sclipfactor / (spriteheight + 1)), 0.5, 1.0);
					difft *= ratio;
				}
				z2 -= difft;
				z1 -= difft;
			}
		}
		if (diffb <= (0 - (float)gl_sclipthreshold))	// such a large displacement can't be correct! 
		{
			// for living monsters standing on the floor allow a little more.
			if (!(thing->flags3&MF3_ISMONSTER) || (thing->flags&MF_NOGRAVITY) || (thing->flags&MF_CORPSE) || diffb < (-1.8*(float)gl_sclipthreshold))
			{
				diffb = 0;
			}
		}
		z2 -= diffb;
		z1 -= diffb;
	}
}

//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::Process(HWDrawInfo *di, AActor* thing, sector_t * sector, area_t in_area, int thruportal)
{
	sector_t rs;
	sector_t * rendersector;

	if (thing == nullptr)
		return;

	// [ZZ] allow CustomSprite-style direct picnum specification
	bool isPicnumOverride = thing->picnum.isValid();

	// Don't waste time projecting sprites that are definitely not visible.
	if ((thing->sprite == 0 && !isPicnumOverride) || !thing->IsVisibleToPlayer() || ((thing->renderflags & RF_MASKROTATION) && !thing->IsInsideVisibleAngles()))
	{
		return;
	}

	AActor *camera = r_viewpoint.camera;

	if (thing->renderflags & RF_INVISIBLE || !thing->RenderStyle.IsVisible(thing->Alpha))
	{
		if (!(thing->flags & MF_STEALTH) || !di->FixedColormap || !gl_enhanced_nightvision || thing == camera)
			return;
	}

	// check renderrequired vs ~r_rendercaps, if anything matches we don't support that feature,
	// check renderhidden vs r_rendercaps, if anything matches we do support that feature and should hide it.
	if ((!r_debug_disable_vis_filter && !!(thing->RenderRequired & ~r_renderercaps)) ||
		(!!(thing->RenderHidden & r_renderercaps)))
		return;

	int spritenum = thing->sprite;
	DVector2 sprscale = thing->Scale;
	if (thing->player != nullptr)
	{
		P_CheckPlayerSprite(thing, spritenum, sprscale);
	}

	// [RH] Interpolate the sprite's position to make it look smooth
	DVector3 thingpos = thing->InterpolatedPosition(r_viewpoint.TicFrac);
	if (thruportal == 1) thingpos += level.Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);

	// Some added checks if the camera actor is not supposed to be seen. It can happen that some portal setup has this actor in view in which case it may not be skipped here
	if (thing == camera && !r_viewpoint.showviewer)
	{
		DVector3 thingorigin = thing->Pos();
		if (thruportal == 1) thingorigin += level.Displacements.getOffset(thing->Sector->PortalGroup, sector->PortalGroup);
		if (fabs(thingorigin.X - r_viewpoint.ActorPos.X) < 2 && fabs(thingorigin.Y - r_viewpoint.ActorPos.Y) < 2) return;
	}
	// Thing is invisible if close to the camera.
	if (thing->renderflags & RF_MAYBEINVISIBLE)
	{
		if (fabs(thingpos.X - r_viewpoint.Pos.X) < 32 && fabs(thingpos.Y - r_viewpoint.Pos.Y) < 32) return;
	}

	// Too close to the camera. This doesn't look good if it is a sprite.
	if (fabs(thingpos.X - r_viewpoint.Pos.X) < 2 && fabs(thingpos.Y - r_viewpoint.Pos.Y) < 2)
	{
		if (r_viewpoint.Pos.Z >= thingpos.Z - 2 && r_viewpoint.Pos.Z <= thingpos.Z + thing->Height + 2)
		{
			// exclude vertically moving objects from this check.
			if (!thing->Vel.isZero())
			{
				if (!gl_FindModelFrame(thing->GetClass(), spritenum, thing->frame, false))
				{
					return;
				}
			}
		}
	}

	// don't draw first frame of a player missile
	if (thing->flags&MF_MISSILE)
	{
		if (!(thing->flags7 & MF7_FLYCHEAT) && thing->target == di->mViewActor && di->mViewActor != nullptr)
		{
			double speed = thing->Vel.Length();
			if (speed >= thing->target->radius / 2)
			{
				double clipdist = clamp(thing->Speed, thing->target->radius, thing->target->radius * 2);
				if ((thingpos - r_viewpoint.Pos).LengthSquared() < clipdist * clipdist) return;
			}
		}
		thing->flags7 |= MF7_FLYCHEAT;	// do this only once for the very first frame, but not if it gets into range again.
	}

	if (thruportal != 2 && di->clipPortal)
	{
		int clipres = di->ClipPoint(thingpos);
		if (clipres == PClip_InFront) return;
	}
	// disabled because almost none of the actual game code is even remotely prepared for this. If desired, use the INTERPOLATE flag.
	if (thing->renderflags & RF_INTERPOLATEANGLES)
		Angles = thing->InterpolatedAngles(r_viewpoint.TicFrac);
	else
		Angles = thing->Angles;

	player_t *player = &players[consoleplayer];
	FloatRect r;

	if (sector->sectornum != thing->Sector->sectornum && !thruportal)
	{
		rendersector = hw_FakeFlat(thing->Sector, &rs, in_area, false);
	}
	else
	{
		rendersector = sector;
	}
	topclip = rendersector->PortalBlocksMovement(sector_t::ceiling) ? LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::ceiling);
	bottomclip = rendersector->PortalBlocksMovement(sector_t::floor) ? -LARGE_VALUE : rendersector->GetPortalPlaneZ(sector_t::floor);

	uint32_t spritetype = (thing->renderflags & RF_SPRITETYPEMASK);
	x = thingpos.X;
	z = thingpos.Z;
	y = thingpos.Y;
	if (spritetype == RF_FACESPRITE) z -= thing->Floorclip; // wall and flat sprites are to be considered level geometry so this may not apply.

	// [RH] Make floatbobbing a renderer-only effect.
	if (thing->flags2 & MF2_FLOATBOB)
	{
		float fz = thing->GetBobOffset(r_viewpoint.TicFrac);
		z += fz;
	}

	modelframe = isPicnumOverride ? nullptr : gl_FindModelFrame(thing->GetClass(), spritenum, thing->frame, !!(thing->flags & MF_DROPPED));
	if (!modelframe)
	{
		bool mirror;
		DAngle ang = (thingpos - r_viewpoint.Pos).Angle();
		FTextureID patch;
		// [ZZ] add direct picnum override
		if (isPicnumOverride)
		{
			// Animate picnum overrides.
			auto tex = TexMan(thing->picnum);
			if (tex == nullptr) return;
			patch =  tex->id;
			mirror = false;
		}
		else
		{
			DAngle sprangle;
			int rot;
			if (!(thing->renderflags & RF_FLATSPRITE) || thing->flags7 & MF7_SPRITEANGLE)
			{
				sprangle = thing->GetSpriteAngle(ang, r_viewpoint.TicFrac);
				rot = -1;
			}
			else
			{
				// Flat sprites cannot rotate in a predictable manner.
				sprangle = 0.;
				rot = 0;
			}
			patch = sprites[spritenum].GetSpriteFrame(thing->frame, rot, sprangle, &mirror, !!(thing->renderflags & RF_SPRITEFLIP));
		}

		if (!patch.isValid()) return;
		int type = thing->renderflags & RF_SPRITETYPEMASK;
		gltexture = FMaterial::ValidateTexture(patch, (type == RF_FACESPRITE), false);
		if (!gltexture)
			return;

		vt = gltexture->GetSpriteVT();
		vb = gltexture->GetSpriteVB();
		if (thing->renderflags & RF_YFLIP) std::swap(vt, vb);

		gltexture->GetSpriteRect(&r);

		// [SP] SpriteFlip
		if (thing->renderflags & RF_SPRITEFLIP)
			thing->renderflags ^= RF_XFLIP;

		if (mirror ^ !!(thing->renderflags & RF_XFLIP))
		{
			r.left = -r.width - r.left;	// mirror the sprite's x-offset
			ul = gltexture->GetSpriteUL();
			ur = gltexture->GetSpriteUR();
		}
		else
		{
			ul = gltexture->GetSpriteUR();
			ur = gltexture->GetSpriteUL();
		}

		if (thing->renderflags & RF_SPRITEFLIP) // [SP] Flip back
			thing->renderflags ^= RF_XFLIP;

		r.Scale(sprscale.X, sprscale.Y);

		float rightfac = -r.left;
		float leftfac = rightfac - r.width;
		float bottomfac = -r.top;
		float topfac = bottomfac - r.height;
		z1 = z - r.top;
		z2 = z1 - r.height;

		float spriteheight = sprscale.Y * r.height;

		// Tests show that this doesn't look good for many decorations and corpses
		if (spriteheight > 0 && gl_spriteclip > 0 && (thing->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE)
		{
			PerformSpriteClipAdjustment(thing, thingpos, spriteheight);
		}

		float viewvecX;
		float viewvecY;
		switch (spritetype)
		{
		case RF_FACESPRITE:
			viewvecX = di->mViewVector.X;
			viewvecY = di->mViewVector.Y;

			x1 = x - viewvecY*leftfac;
			x2 = x - viewvecY*rightfac;
			y1 = y + viewvecX*leftfac;
			y2 = y + viewvecX*rightfac;
			break;

		case RF_FLATSPRITE:
		{
			x1 = x + leftfac;
			x2 = x + rightfac;
			y1 = y - topfac;
			y2 = y - bottomfac;
		}
		break;
		case RF_WALLSPRITE:
			viewvecX = Angles.Yaw.Cos();
			viewvecY = Angles.Yaw.Sin();

			x1 = x + viewvecY*leftfac;
			x2 = x + viewvecY*rightfac;
			y1 = y - viewvecX*leftfac;
			y2 = y - viewvecX*rightfac;
			break;
		}
	}
	else
	{
		x1 = x2 = x;
		y1 = y2 = y;
		z1 = z2 = z;
		gltexture = nullptr;
	}

	depth = FloatToFixed((x - r_viewpoint.Pos.X) * r_viewpoint.TanCos + (y - r_viewpoint.Pos.Y) * r_viewpoint.TanSin);

	// light calculation

	bool enhancedvision = false;

	// allow disabling of the fullbright flag by a brightmap definition
	// (e.g. to do the gun flashes of Doom's zombies correctly.
	fullbright = (thing->flags5 & MF5_BRIGHT) ||
		((thing->renderflags & RF_FULLBRIGHT) && (!gltexture || !gltexture->tex->bDisableFullbright));

	lightlevel = fullbright ? 255 :
		hw_ClampLight(rendersector->GetTexture(sector_t::ceiling) == skyflatnum ?
			rendersector->GetCeilingLight() : rendersector->GetFloorLight());
	foglevel = (uint8_t)clamp<short>(rendersector->lightlevel, 0, 255);

	lightlevel = rendersector->CheckSpriteGlow(lightlevel, thingpos);

	ThingColor = (thing->RenderStyle.Flags & STYLEF_ColorIsFixed) ? thing->fillcolor : 0xffffff;
	ThingColor.a = 255;
	RenderStyle = thing->RenderStyle;

	// colormap stuff is a little more complicated here...
	if (di->FixedColormap)
	{
		if ((gl_enhanced_nv_stealth > 0 && di->FixedColormap == CM_LITE)		// Infrared powerup only
			|| (gl_enhanced_nv_stealth == 2 && di->FixedColormap >= CM_TORCH)// Also torches
			|| (gl_enhanced_nv_stealth == 3))								// Any fixed colormap
			enhancedvision = true;

		Colormap.Clear();

		if (di->FixedColormap == CM_LITE)
		{
			if (gl_enhanced_nightvision &&
				(thing->IsKindOf(RUNTIME_CLASS(AInventory)) || thing->flags3&MF3_ISMONSTER || thing->flags&MF_MISSILE || thing->flags&MF_CORPSE))
			{
				RenderStyle.Flags |= STYLEF_InvertSource;
			}
		}
	}
	else
	{
		Colormap = rendersector->Colormap;
		if (fullbright)
		{
			if (rendersector == &level.sectors[rendersector->sectornum] || in_area != area_below)
				// under water areas keep their color for fullbright objects
			{
				// Only make the light white but keep everything else (fog, desaturation and Boom colormap.)
				Colormap.MakeWhite();
			}
			else
			{
				// Keep the color, but brighten things a bit so that a difference can be seen.
				Colormap.LightColor.r = (3 * Colormap.LightColor.r + 0xff) / 4;
				Colormap.LightColor.g = (3 * Colormap.LightColor.g + 0xff) / 4;
				Colormap.LightColor.b = (3 * Colormap.LightColor.b + 0xff) / 4;
			}
		}
		else if (level.flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();
		}
	}

	translation = thing->Translation;

	OverrideShader = -1;
	trans = thing->Alpha;
	hw_styleflags = STYLEHW_Normal;

	if (RenderStyle.BlendOp >= STYLEOP_Fuzz && RenderStyle.BlendOp <= STYLEOP_FuzzOrRevSub)
	{
		RenderStyle.CheckFuzz();
		if (RenderStyle.BlendOp == STYLEOP_Fuzz)
		{
			if (gl_fuzztype != 0 && !(screen->hwcaps & RFL_NO_SHADERS) && !(RenderStyle.Flags & STYLEF_InvertSource))
			{
				RenderStyle = LegacyRenderStyles[STYLE_Translucent];
				OverrideShader = SHADER_NoTexture + gl_fuzztype;
				trans = 0.99f;	// trans may not be 1 here
				hw_styleflags = STYLEHW_NoAlphaTest;
			}
			else
			{
				// Without shaders only the standard effect is available.
				RenderStyle.BlendOp = STYLEOP_Shadow;
			}
		}
	}

	if (RenderStyle.Flags & STYLEF_TransSoulsAlpha)
	{
		trans = transsouls;
	}
	else if (RenderStyle.Flags & STYLEF_Alpha1)
	{
		trans = 1.f;
	}
	if (r_UseVanillaTransparency)
	{
		// [SP] "canonical transparency" - with the flip of a CVar, disable transparency for Doom objects,
		//   and disable 'additive' translucency for certain objects from other games.
		if (thing->renderflags & RF_ZDOOMTRANS)
		{
			trans = 1.f;
			RenderStyle.BlendOp = STYLEOP_Add;
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
		}
	}
	if (trans >= 1.f - FLT_EPSILON && RenderStyle.BlendOp != STYLEOP_Shadow && (
		(RenderStyle.SrcAlpha == STYLEALPHA_One && RenderStyle.DestAlpha == STYLEALPHA_Zero) ||
		(RenderStyle.SrcAlpha == STYLEALPHA_Src && RenderStyle.DestAlpha == STYLEALPHA_InvSrc)
		))
	{
		// This is a non-translucent sprite (i.e. STYLE_Normal or equivalent)
		trans = 1.f;

		if (!gl_sprite_blend || modelframe || (thing->renderflags & (RF_FLATSPRITE | RF_WALLSPRITE)) || gl_billboard_faces_camera)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_One;
			RenderStyle.DestAlpha = STYLEALPHA_Zero;
			hw_styleflags = STYLEHW_Solid;
		}
		else
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
	}
	if ((gltexture && gltexture->tex->GetTranslucency()) || (RenderStyle.Flags & STYLEF_RedIsAlpha))
	{
		if (hw_styleflags == STYLEHW_Solid)
		{
			RenderStyle.SrcAlpha = STYLEALPHA_Src;
			RenderStyle.DestAlpha = STYLEALPHA_InvSrc;
		}
		hw_styleflags = STYLEHW_NoAlphaTest;
	}

	if (enhancedvision && gl_enhanced_nightvision)
	{
		if (RenderStyle.BlendOp == STYLEOP_Shadow)
		{
			// enhanced vision makes them more visible!
			trans = 0.5f;
			FRenderStyle rs = RenderStyle;
			RenderStyle = STYLE_Translucent;
			RenderStyle.Flags = rs.Flags;	// Flags must be preserved, at this point it can only be STYLEF_InvertSource
		}
		else if (thing->flags & MF_STEALTH)
		{
			// enhanced vision overcomes stealth!
			if (trans < 0.5f) trans = 0.5f;
		}
	}

	if (trans == 0.0f) return;

	// end of light calculation

	actor = thing;
	index = di->spriteindex++;	// this assumes that sprites from the same sector are added sequentially, i.e. by the same thread.
	particle = nullptr;

	const bool drawWithXYBillboard = (!(actor->renderflags & RF_FORCEYBILLBOARD)
		&& (actor->renderflags & RF_SPRITETYPEMASK) == RF_FACESPRITE
		&& players[consoleplayer].camera
		&& (gl_billboard_mode == 1 || actor->renderflags & RF_FORCEXYBILLBOARD));


	// no light splitting when:
	// 1. no lightlist
	// 2. any fixed colormap
	// 3. any bright object
	// 4. any with render style shadow (which doesn't use the sector light)
	// 5. anything with render style reverse subtract (light effect is not what would be desired here)
	if (thing->Sector->e->XFloor.lightlist.Size() != 0 && di->FixedColormap == CM_DEFAULT && !fullbright &&
		RenderStyle.BlendOp != STYLEOP_Shadow && RenderStyle.BlendOp != STYLEOP_RevSub)
	{
		if (screen->hwcaps & RFL_NO_CLIP_PLANES)	// on old hardware we are rather limited...
		{
			lightlist = nullptr;
			if (!drawWithXYBillboard && !modelframe)
			{
				SplitSprite(di, thing->Sector, hw_styleflags != STYLEHW_Solid);
			}
		}
		else
		{
			lightlist = &thing->Sector->e->XFloor.lightlist;
		}
	}
	else
	{
		lightlist = nullptr;
	}

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}


//==========================================================================
//
// 
//
//==========================================================================

void GLSprite::ProcessParticle (HWDrawInfo *di, particle_t *particle, sector_t *sector)//, int shade, int fakeside)
{
	player_t *player=&players[consoleplayer];
	
	if (particle->alpha==0) return;

	lightlevel = hw_ClampLight(sector->GetTexture(sector_t::ceiling) == skyflatnum ? 
		sector->GetCeilingLight() : sector->GetFloorLight());
	foglevel = (uint8_t)clamp<short>(sector->lightlevel, 0, 255);

	if (di->FixedColormap) 
	{
		Colormap.Clear();
	}
	else if (!particle->bright)
	{
		TArray<lightlist_t> & lightlist=sector->e->XFloor.lightlist;
		double lightbottom;

		Colormap = sector->Colormap;
		for(unsigned int i=0;i<lightlist.Size();i++)
		{
			if (i<lightlist.Size()-1) lightbottom = lightlist[i+1].plane.ZatPoint(particle->Pos);
			else lightbottom = sector->floorplane.ZatPoint(particle->Pos);

			if (lightbottom < particle->Pos.Z)
			{
				lightlevel = hw_ClampLight(*lightlist[i].p_lightlevel);
				Colormap.CopyLight(lightlist[i].extra_colormap);
				break;
			}
		}
		if (level.flags3 & LEVEL3_NOCOLOREDSPRITELIGHTING)
		{
			Colormap.Decolorize();	// ZDoom never applies colored light to particles.
		}
	}
	else
	{
		lightlevel = 255;
		Colormap = sector->Colormap;
		Colormap.ClearColor();
	}

	trans=particle->alpha;
	RenderStyle = STYLE_Translucent;
	OverrideShader = 0;

	ThingColor = particle->color;
	ThingColor.a = 255;

	modelframe=nullptr;
	gltexture=nullptr;
	topclip = LARGE_VALUE;
	bottomclip = -LARGE_VALUE;

	// [BB] Load the texture for round or smooth particles
	if (gl_particles_style)
	{
		FTextureID lump;
		if (gl_particles_style == 1)
		{
			lump = TexMan.glPart2;
		}
		else if (gl_particles_style == 2)
		{
			lump = TexMan.glPart;
		}
		else lump.SetNull();

		if (lump.isValid())
		{
			gltexture = FMaterial::ValidateTexture(lump, true, false);
			translation = 0;

			ul = gltexture->GetUL();
			ur = gltexture->GetUR();
			vt = gltexture->GetVT();
			vb = gltexture->GetVB();
			FloatRect r;
			gltexture->GetSpriteRect(&r);
		}
	}

	double timefrac = r_viewpoint.TicFrac;
	if (paused || bglobal.freeze || (level.flags2 & LEVEL2_FROZEN))
		timefrac = 0.;
	float xvf = (particle->Vel.X) * timefrac;
	float yvf = (particle->Vel.Y) * timefrac;
	float zvf = (particle->Vel.Z) * timefrac;

	x = float(particle->Pos.X) + xvf;
	y = float(particle->Pos.Y) + yvf;
	z = float(particle->Pos.Z) + zvf;
	
	float factor;
	if (gl_particles_style == 1) factor = 1.3f / 7.f;
	else if (gl_particles_style == 2) factor = 2.5f / 7.f;
	else factor = 1 / 7.f;
	float scalefac=particle->size * factor;

	float viewvecX = di->mViewVector.X;
	float viewvecY = di->mViewVector.Y;

	x1=x+viewvecY*scalefac;
	x2=x-viewvecY*scalefac;
	y1=y-viewvecX*scalefac;
	y2=y+viewvecX*scalefac;
	z1=z-scalefac;
	z2=z+scalefac;

	depth = FloatToFixed((x - r_viewpoint.Pos.X) * r_viewpoint.TanCos + (y - r_viewpoint.Pos.Y) * r_viewpoint.TanSin);

	actor=nullptr;
	this->particle=particle;
	fullbright = !!particle->bright;
	
	// [BB] Translucent particles have to be rendered without the alpha test.
	if (gl_particles_style != 2 && trans>=1.0f-FLT_EPSILON) hw_styleflags = STYLEHW_Solid;
	else hw_styleflags = STYLEHW_NoAlphaTest;

	if (sector->e->XFloor.lightlist.Size() != 0 && di->FixedColormap == CM_DEFAULT && !fullbright)
		lightlist = &sector->e->XFloor.lightlist;
	else
		lightlist = nullptr;

	PutSprite(di, hw_styleflags != STYLEHW_Solid);
	rendered_sprites++;
}

//==========================================================================
//
// 
//
//==========================================================================

void HWDrawInfo::ProcessActorsInPortal(FLinePortalSpan *glport, area_t in_area)
{
	TMap<AActor*, bool> processcheck;
	if (glport->validcount == validcount) return;	// only process once per frame
	glport->validcount = validcount;
	for (auto port : glport->lines)
	{
		line_t *line = port->mOrigin;
		if (line->isLinePortal())	// only crossable ones
		{
			FLinePortal *port2 = port->mDestination->getPortal();
			// process only if the other side links back to this one.
			if (port2 != nullptr && port->mDestination == port2->mOrigin && port->mOrigin == port2->mDestination)
			{
				for (portnode_t *node = port->lineportal_thinglist; node != nullptr; node = node->m_snext)
				{
					AActor *th = node->m_thing;

					// process each actor only once per portal.
					bool *check = processcheck.CheckKey(th);
					if (check && *check) continue;
					processcheck[th] = true;

					DAngle savedangle = th->Angles.Yaw;
					DVector3 savedpos = th->Pos();
					DVector3 newpos = savedpos;
					sector_t fakesector;

					if (!r_viewpoint.showviewer && th == r_viewpoint.camera)
					{
						if (fabs(savedpos.X - r_viewpoint.ActorPos.X) < 2 && fabs(savedpos.Y - r_viewpoint.ActorPos.Y) < 2)
						{
							continue;
						}
					}

					P_TranslatePortalXY(line, newpos.X, newpos.Y);
					P_TranslatePortalZ(line, newpos.Z);
					P_TranslatePortalAngle(line, th->Angles.Yaw);
					th->SetXYZ(newpos);
					th->Prev += newpos - savedpos;

					GLSprite spr;
					spr.Process(this, th, hw_FakeFlat(th->Sector, &fakesector, in_area, false), in_area, 2);
					th->Angles.Yaw = savedangle;
					th->SetXYZ(savedpos);
					th->Prev -= newpos - savedpos;
				}
			}
		}
	}
}
