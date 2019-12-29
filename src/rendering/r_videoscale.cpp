// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2017 Magnus Norddahl
// Copyright(C) 2018 Rachael Alexanderson
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

#include <math.h>
#include "c_dispatch.h"
#include "c_cvars.h"
#include "v_video.h"
#include "templates.h"

#include "console/c_console.h"
#include "menu/menu.h"

#define NUMSCALEMODES 8

extern bool setsizeneeded;
extern bool generic_ui;

EXTERN_CVAR(Int, vid_aspect)

EXTERN_CVAR(Bool, log_vgafont)
EXTERN_CVAR(Bool, dlg_vgafont)

CUSTOM_CVAR(Int, vid_scale_customwidth, VID_MIN_WIDTH, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < VID_MIN_WIDTH)
		self = VID_MIN_WIDTH;
	setsizeneeded = true;
}
CUSTOM_CVAR(Int, vid_scale_customheight, VID_MIN_HEIGHT, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	if (self < VID_MIN_HEIGHT)
		self = VID_MIN_HEIGHT;
	setsizeneeded = true;
}
CVAR(Bool, vid_scale_customlinear, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
CUSTOM_CVAR(Bool, vid_scale_customstretched, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	setsizeneeded = true;
}

namespace
{
	uint32_t min_width = VID_MIN_WIDTH;
	uint32_t min_height = VID_MIN_HEIGHT;

	struct v_ScaleTable
	{
		bool isValid;
		bool isLinear;
		uint32_t(*GetScaledWidth)(uint32_t Width, uint32_t Height);
		uint32_t(*GetScaledHeight)(uint32_t Width, uint32_t Height);
		bool isScaled43;
		bool isCustom;
	};

	float v_MinimumToFill(uint32_t inwidth, uint32_t inheight)
	{
		// sx = screen x dimension, sy = same for y
		float sx = (float)inwidth, sy = (float)inheight;
		static float lastsx = 0., lastsy = 0., result = 0.;
		if (lastsx != sx || lastsy != sy)
		{
			if (sx <= 0. || sy <= 0.)
				return 1.; // prevent x/0 error
			// set absolute minimum scale to fill the entire screen but get as close to 640x400 as possible
			float ssx = (float)(min_width) / sx, ssy = (float)(min_height) / sy;
			result = (ssx < ssy) ? ssy : ssx;
			lastsx = sx;
			lastsy = sy;
		}
		return result;
	}
	inline uint32_t v_mfillX(uint32_t inwidth, uint32_t inheight)
	{
		return (uint32_t)((float)inwidth * v_MinimumToFill(inwidth, inheight));
	}
	inline uint32_t v_mfillY(uint32_t inwidth, uint32_t inheight)
	{
		return (uint32_t)((float)inheight * v_MinimumToFill(inwidth, inheight));
	}
	inline void refresh_minimums()
	{
		// specialUI is tracking a state where high-res console fonts are actually required, and
		// aren't actually rendered correctly in 320x200. this forces the game to revert to the 640x400
		// minimum set in GZDoom 4.0.0, but only while those fonts are required.

		static bool lastspecialUI = false;
		bool isInActualMenu = false;

		bool specialUI = (generic_ui || !!log_vgafont || !!dlg_vgafont || ConsoleState != c_up || 
			(menuactive == MENU_On && CurrentMenu && !CurrentMenu->IsKindOf("ConversationMenu")));

		if (specialUI == lastspecialUI)
			return;

		lastspecialUI = specialUI;
		setsizeneeded = true;

		if (!specialUI)
		{
			min_width = VID_MIN_WIDTH;
			min_height = VID_MIN_HEIGHT;
		}
		else
		{
			min_width = VID_MIN_UI_WIDTH;
			min_height = VID_MIN_UI_HEIGHT;
		}
	}

	v_ScaleTable vScaleTable[NUMSCALEMODES] =
	{
		//	isValid,	isLinear,	GetScaledWidth(),													            	GetScaledHeight(),										        					isScaled43, isCustom
		{ true,			false,		[](uint32_t Width, uint32_t Height)->uint32_t { return Width; },		        		[](uint32_t Width, uint32_t Height)->uint32_t { return Height; },	        		false,  	false   },	// 0  - Native
		{ true,			true,		[](uint32_t Width, uint32_t Height)->uint32_t { return Width; },			       		[](uint32_t Width, uint32_t Height)->uint32_t { return Height; },	        		false,  	false   },	// 1  - Native (Linear)
		{ true,			false,		[](uint32_t Width, uint32_t Height)->uint32_t { return 640; },		            		[](uint32_t Width, uint32_t Height)->uint32_t { return 400; },			        	true,   	false   },	// 2  - 640x400 (formerly 320x200)
		{ true,			true,		[](uint32_t Width, uint32_t Height)->uint32_t { return 960; },		            		[](uint32_t Width, uint32_t Height)->uint32_t { return 600; },				        true,   	false   },	// 3  - 960x600 (formerly 640x400)
		{ true,			true,		[](uint32_t Width, uint32_t Height)->uint32_t { return 1280; },		           		[](uint32_t Width, uint32_t Height)->uint32_t { return 800; },	        			true,   	false   },	// 4  - 1280x800
		{ true,			true,		[](uint32_t Width, uint32_t Height)->uint32_t { return vid_scale_customwidth; },		[](uint32_t Width, uint32_t Height)->uint32_t { return vid_scale_customheight; },	true,   	true    },	// 5  - Custom
		{ true,			true,		[](uint32_t Width, uint32_t Height)->uint32_t { return v_mfillX(Width, Height); },		[](uint32_t Width, uint32_t Height)->uint32_t { return v_mfillY(Width, Height); },	false,		false   },	// 6  - Minimum Scale to Fill Entire Screen
		{ true,			false,		[](uint32_t Width, uint32_t Height)->uint32_t { return 320; },		            		[](uint32_t Width, uint32_t Height)->uint32_t { return 200; },			        	true,   	false   },	// 7  - 320x200
	};
	bool isOutOfBounds(int x)
	{
		return (x < 0 || x >= NUMSCALEMODES || vScaleTable[x].isValid == false);
	}
}

void R_ShowCurrentScaling();
CUSTOM_CVAR(Float, vid_scalefactor, 1.0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	setsizeneeded = true;
	if (self < 0.05 || self > 2.0)
		self = 1.0;
	if (self != 1.0)
		R_ShowCurrentScaling();
}

CUSTOM_CVAR(Int, vid_scalemode, 0, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	setsizeneeded = true;
	if (isOutOfBounds(self))
		self = 0;
}

CUSTOM_CVAR(Bool, vid_cropaspect, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG)
{
	setsizeneeded = true;
}

bool ViewportLinearScale()
{
	if (isOutOfBounds(vid_scalemode))
		vid_scalemode = 0;
	// hack - use custom scaling if in "custom" mode
	if (vScaleTable[vid_scalemode].isCustom)
		return vid_scale_customlinear;
	// vid_scalefactor > 1 == forced linear scale
	return (vid_scalefactor > 1.0) ? true : vScaleTable[vid_scalemode].isLinear;
}

int ViewportScaledWidth(int width, int height)
{
	if (isOutOfBounds(vid_scalemode))
		vid_scalemode = 0;
	refresh_minimums();
	if (vid_cropaspect && height > 0)
	{
		width = ((float)width/height > ActiveRatio(width, height)) ? (int)(height * ActiveRatio(width, height)) : width;
		height = ((float)width/height < ActiveRatio(width, height)) ? (int)(width / ActiveRatio(width, height)) : height;
	}
	return (int)MAX((int32_t)min_width, (int32_t)(vid_scalefactor * vScaleTable[vid_scalemode].GetScaledWidth(width, height)));
}

int ViewportScaledHeight(int width, int height)
{
	if (isOutOfBounds(vid_scalemode))
		vid_scalemode = 0;
	if (vid_cropaspect && height > 0)
	{
		height = ((float)width/height < ActiveRatio(width, height)) ? (int)(width / ActiveRatio(width, height)) : height;
		width = ((float)width/height > ActiveRatio(width, height)) ? (int)(height * ActiveRatio(width, height)) : width;
	}
	return (int)MAX((int32_t)min_height, (int32_t)(vid_scalefactor * vScaleTable[vid_scalemode].GetScaledHeight(width, height)));
}

bool ViewportIsScaled43()
{
	if (isOutOfBounds(vid_scalemode))
		vid_scalemode = 0;
	// hack - use custom scaling if in "custom" mode
	if (vScaleTable[vid_scalemode].isCustom)
		return vid_scale_customstretched;
	return vScaleTable[vid_scalemode].isScaled43;
}

void R_ShowCurrentScaling()
{
	int x1 = screen->GetClientWidth(), y1 = screen->GetClientHeight(), x2 = ViewportScaledWidth(x1, y1), y2 = ViewportScaledHeight(x1, y1);
	Printf("Current vid_scalefactor: %f\n", (float)(vid_scalefactor));
	Printf("Real resolution: %i x %i\nEmulated resolution: %i x %i\n", x1, y1, x2, y2);
}

CCMD (vid_showcurrentscaling)
{
	R_ShowCurrentScaling();
}

CCMD (vid_scaletowidth)
{
	if (argv.argc() > 1)
	{
		// the following enables the use of ViewportScaledWidth to get the proper dimensions in custom scale modes
		vid_scalefactor = 1;
		vid_scalefactor = (float)((double)atof(argv[1]) / ViewportScaledWidth(screen->GetClientWidth(), screen->GetClientHeight()));
	}
}

CCMD (vid_scaletoheight)
{
	if (argv.argc() > 1)
	{
		vid_scalefactor = 1;
		vid_scalefactor = (float)((double)atof(argv[1]) / ViewportScaledHeight(screen->GetClientWidth(), screen->GetClientHeight()));
	}
}

inline bool atob(char* I)
{
    if (stricmp (I, "true") == 0 || stricmp (I, "1") == 0)
        return true;
    return false;
}

CCMD (vid_setscale)
{
    if (argv.argc() > 2)
    {
        vid_scale_customwidth = atoi(argv[1]);
        vid_scale_customheight = atoi(argv[2]);
        if (argv.argc() > 3)
        {
            vid_scale_customlinear = atob(argv[3]);
            if (argv.argc() > 4)
            {
                vid_scale_customstretched = atob(argv[4]);
            }
        }
        vid_scalemode = 5;
	vid_scalefactor = 1.0;
    }
    else
    {
        Printf("Usage: vid_setscale <x> <y> [bool linear] [bool long-pixel-shape]\nThis command will create a custom viewport scaling mode.\n");
    }
}

CCMD (vid_scaletolowest)
{
	uint32_t method = 0;
	if (argv.argc() > 1)
		method = atoi(argv[1]);
	switch (method)
	{
	case 1:		// Method 1: set a custom video scaling
		vid_scalemode = 5;
		vid_scalefactor = 1.0;
		vid_scale_customlinear = 1;
		vid_scale_customstretched = 0;
		vid_scale_customwidth = v_mfillX(screen->GetClientWidth(), screen->GetClientHeight());
		vid_scale_customheight = v_mfillY(screen->GetClientWidth(), screen->GetClientHeight());
		break;
	case 2:		// Method 2: use the actual downscaling mode directly
		vid_scalemode = 6;
		vid_scalefactor = 1.0;
		break;
	default:	// Default method: use vid_scalefactor to achieve the result on a default scaling mode
		vid_scalemode = 1;
		vid_scalefactor = v_MinimumToFill(screen->GetClientWidth(), screen->GetClientHeight());
		break;
	}
}
