#pragma once

#include "actor.h"
#include "r_defs.h"
#include "g_levellocals.h"
// These depend on both actor.h and r_defs.h so they cannot be in either file without creating a circular dependency.

inline DVector3 AActor::PosRelative(int portalgroup) const
{
	return Pos() + level.Displacements.getOffset(Sector->PortalGroup, portalgroup);
}

inline DVector3 AActor::PosRelative(const AActor *other) const
{
	return Pos() + level.Displacements.getOffset(Sector->PortalGroup, other->Sector->PortalGroup);
}

inline DVector3 AActor::PosRelative(sector_t *sec) const
{
	return Pos() + level.Displacements.getOffset(Sector->PortalGroup, sec->PortalGroup);
}

inline DVector3 AActor::PosRelative(const line_t *line) const
{
	return Pos() + level.Displacements.getOffset(Sector->PortalGroup, line->frontsector->PortalGroup);
}

inline DVector3 PosRelative(const DVector3 &pos, line_t *line, sector_t *refsec = NULL)
{
	return pos + level.Displacements.getOffset(refsec->PortalGroup, line->frontsector->PortalGroup);
}


inline void AActor::ClearInterpolation()
{
	Prev = Pos();
	PrevAngles = Angles;
	if (Sector) PrevPortalGroup = Sector->PortalGroup;
	else PrevPortalGroup = 0;
}

inline double secplane_t::ZatPoint(const AActor *ac) const
{
	return (D + normal.X*ac->X() + normal.Y*ac->Y()) * negiC;
}

inline double sector_t::HighestCeilingAt(AActor *a, sector_t **resultsec)
{
	return ::HighestCeilingAt(this, a->X(), a->Y(), resultsec);
}

inline double sector_t::LowestFloorAt(AActor *a, sector_t **resultsec)
{
	return ::LowestFloorAt(this, a->X(), a->Y(), resultsec);
}

inline double AActor::GetBobOffset(double ticfrac) const
{
	if (!(flags2 & MF2_FLOATBOB))
	{
		return 0;
	}
	return BobSin(FloatBobPhase + level.maptime + ticfrac) * FloatBobStrength;
}

inline double AActor::GetCameraHeight() const
{
	return CameraHeight == INT_MIN ? Height / 2 : CameraHeight;
}


inline FDropItem *AActor::GetDropItems() const
{
	return GetInfo()->DropItems;
}

inline double AActor::GetGravity() const
{
	if (flags & MF_NOGRAVITY) return 0;
	return level.gravity * Sector->gravity * Gravity * 0.00125;
}
