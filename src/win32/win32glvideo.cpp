/*
** win32video.cpp
** Code to let ZDoom draw to the screen
**
**---------------------------------------------------------------------------
** Copyright 1998-2006 Randy Heit
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#include <windows.h>
#include <GL/gl.h>
#include "wglext.h"

#include "gl_sysfb.h"
#include "hardware.h"
#include "x86.h"
#include "templates.h"
#include "version.h"
#include "c_console.h"
#include "v_video.h"
#include "i_input.h"
#include "i_system.h"
#include "doomstat.h"
#include "v_text.h"
#include "m_argv.h"
#include "doomerrors.h"
#include "win32glvideo.h"

#include "gl/system/gl_framebuffer.h"

EXTERN_CVAR(Int, vid_adapter)
EXTERN_CVAR(Bool, vr_enable_quadbuffered)

CUSTOM_CVAR(Bool, gl_debug, false, CVAR_ARCHIVE | CVAR_GLOBALCONFIG | CVAR_NOINITCALL)
{
	Printf("This won't take effect until " GAMENAME " is restarted.\n");
}

// these get used before GLEW is initialized so we have to use separate pointers with different names
PFNWGLCHOOSEPIXELFORMATARBPROC myWglChoosePixelFormatARB; // = (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");
PFNWGLCREATECONTEXTATTRIBSARBPROC myWglCreateContextAttribsARB;


//==========================================================================
//
// 
//
//==========================================================================

Win32GLVideo::Win32GLVideo()
{
	SetPixelFormat();
}

//==========================================================================
//
// 
//
//==========================================================================

DFrameBuffer *Win32GLVideo::CreateFrameBuffer()
{
	SystemGLFrameBuffer *fb;

	fb = new OpenGLFrameBuffer(m_hMonitor, fullscreen);
	return fb;
}

//==========================================================================
//
// 
//
//==========================================================================

HWND Win32GLVideo::InitDummy()
{
	HMODULE g_hInst = GetModuleHandle(NULL);
	HWND dummy;
	//Create a rect structure for the size/position of the window
	RECT windowRect;
	windowRect.left = 0;
	windowRect.right = 64;
	windowRect.top = 0;
	windowRect.bottom = 64;

	//Window class structure
	WNDCLASS wc;

	//Fill in window class struct
	wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	wc.lpfnWndProc = (WNDPROC)DefWindowProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = g_hInst;
	wc.hIcon = LoadIcon(NULL, IDI_WINLOGO);
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = NULL;
	wc.lpszMenuName = NULL;
	wc.lpszClassName = "GZDoomOpenGLDummyWindow";

	//Register window class
	if (!RegisterClass(&wc))
	{
		return 0;
	}

	//Set window style & extended style
	DWORD style, exStyle;
	exStyle = WS_EX_CLIENTEDGE;
	style = WS_SYSMENU | WS_BORDER | WS_CAPTION;// | WS_VISIBLE;

												//Adjust the window size so that client area is the size requested
	AdjustWindowRectEx(&windowRect, style, false, exStyle);

	//Create Window
	if (!(dummy = CreateWindowEx(exStyle,
		"GZDoomOpenGLDummyWindow",
		"GZDOOM",
		WS_CLIPSIBLINGS | WS_CLIPCHILDREN | style,
		0, 0,
		windowRect.right - windowRect.left,
		windowRect.bottom - windowRect.top,
		NULL, NULL,
		g_hInst,
		NULL)))
	{
		UnregisterClass("GZDoomOpenGLDummyWindow", g_hInst);
		return 0;
	}
	ShowWindow(dummy, SW_HIDE);

	return dummy;
}

//==========================================================================
//
// 
//
//==========================================================================

void Win32GLVideo::ShutdownDummy(HWND dummy)
{
	DestroyWindow(dummy);
	UnregisterClass("GZDoomOpenGLDummyWindow", GetModuleHandle(NULL));
}


//==========================================================================
//
// 
//
//==========================================================================

bool Win32GLVideo::SetPixelFormat()
{
	HDC hDC;
	HGLRC hRC;
	HWND dummy;

	PIXELFORMATDESCRIPTOR pfd = {
		sizeof(PIXELFORMATDESCRIPTOR),
		1,
		PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
		PFD_TYPE_RGBA,
		32, // color depth
		0, 0, 0, 0, 0, 0,
		0,
		0,
		0,
		0, 0, 0, 0,
		16, // z depth
		0, // stencil buffer
		0,
		PFD_MAIN_PLANE,
		0,
		0, 0, 0
	};

	int pixelFormat;

	// we have to create a dummy window to init stuff from or the full init stuff fails
	dummy = InitDummy();

	hDC = GetDC(dummy);
	pixelFormat = ChoosePixelFormat(hDC, &pfd);
	DescribePixelFormat(hDC, pixelFormat, sizeof(pfd), &pfd);

	::SetPixelFormat(hDC, pixelFormat, &pfd);

	hRC = wglCreateContext(hDC);
	wglMakeCurrent(hDC, hRC);

	myWglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)wglGetProcAddress("wglChoosePixelFormatARB");
	myWglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
	// any extra stuff here?

	wglMakeCurrent(NULL, NULL);
	wglDeleteContext(hRC);
	ReleaseDC(dummy, hDC);
	ShutdownDummy(dummy);

	return true;
}

//==========================================================================
//
// 
//
//==========================================================================

bool Win32GLVideo::SetupPixelFormat(int multisample)
{
	int i;
	int colorDepth;
	HDC deskDC;
	int attributes[28];
	int pixelFormat;
	unsigned int numFormats;
	float attribsFloat[] = { 0.0f, 0.0f };

	deskDC = GetDC(GetDesktopWindow());
	colorDepth = GetDeviceCaps(deskDC, BITSPIXEL);
	ReleaseDC(GetDesktopWindow(), deskDC);

	if (myWglChoosePixelFormatARB)
	{
	again:
		attributes[0] = WGL_RED_BITS_ARB; //bits
		attributes[1] = 8;
		attributes[2] = WGL_GREEN_BITS_ARB; //bits
		attributes[3] = 8;
		attributes[4] = WGL_BLUE_BITS_ARB; //bits
		attributes[5] = 8;
		attributes[6] = WGL_ALPHA_BITS_ARB;
		attributes[7] = 8;
		attributes[8] = WGL_DEPTH_BITS_ARB;
		attributes[9] = 24;
		attributes[10] = WGL_STENCIL_BITS_ARB;
		attributes[11] = 8;

		attributes[12] = WGL_DRAW_TO_WINDOW_ARB;	//required to be true
		attributes[13] = true;
		attributes[14] = WGL_SUPPORT_OPENGL_ARB;
		attributes[15] = true;
		attributes[16] = WGL_DOUBLE_BUFFER_ARB;
		attributes[17] = true;

		if (multisample > 0)
		{
			attributes[18] = WGL_SAMPLE_BUFFERS_ARB;
			attributes[19] = true;
			attributes[20] = WGL_SAMPLES_ARB;
			attributes[21] = multisample;
			i = 22;
		}
		else
		{
			i = 18;
		}

		attributes[i++] = WGL_ACCELERATION_ARB;	//required to be FULL_ACCELERATION_ARB
		attributes[i++] = WGL_FULL_ACCELERATION_ARB;

		if (vr_enable_quadbuffered)
		{
			// [BB] Starting with driver version 314.07, NVIDIA GeForce cards support OpenGL quad buffered
			// stereo rendering with 3D Vision hardware. Select the corresponding attribute here.
			attributes[i++] = WGL_STEREO_ARB;
			attributes[i++] = true;
		}

		attributes[i++] = 0;
		attributes[i++] = 0;

		if (!myWglChoosePixelFormatARB(m_hDC, attributes, attribsFloat, 1, &pixelFormat, &numFormats))
		{
			Printf("R_OPENGL: Couldn't choose pixel format. Retrying in compatibility mode\n");
			goto oldmethod;
		}

		if (numFormats == 0)
		{
			if (vr_enable_quadbuffered)
			{
				Printf("R_OPENGL: No valid pixel formats found for VR quadbuffering. Retrying without this feature\n");
				vr_enable_quadbuffered = false;
				goto again;
			}
			Printf("R_OPENGL: No valid pixel formats found. Retrying in compatibility mode\n");
			goto oldmethod;
		}
	}
	else
	{
	oldmethod:
		// If wglChoosePixelFormatARB is not found we have to do it the old fashioned way.
		static PIXELFORMATDESCRIPTOR pfd = {
			sizeof(PIXELFORMATDESCRIPTOR),
			1,
			PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
			PFD_TYPE_RGBA,
			32, // color depth
			0, 0, 0, 0, 0, 0,
			0,
			0,
			0,
			0, 0, 0, 0,
			32, // z depth
			8, // stencil buffer
			0,
			PFD_MAIN_PLANE,
			0,
			0, 0, 0
		};

		pixelFormat = ChoosePixelFormat(m_hDC, &pfd);
		DescribePixelFormat(m_hDC, pixelFormat, sizeof(pfd), &pfd);

		if (pfd.dwFlags & PFD_GENERIC_FORMAT)
		{
			I_Error("R_OPENGL: OpenGL driver not accelerated!");
			return false;
		}
	}

	if (!::SetPixelFormat(m_hDC, pixelFormat, NULL))
	{
		I_Error("R_OPENGL: Couldn't set pixel format.\n");
		return false;
	}
	return true;
}

//==========================================================================
//
// 
//
//==========================================================================

bool Win32GLVideo::InitHardware(HWND Window, int multisample)
{
	m_Window = Window;
	m_hDC = GetDC(Window);

	if (!SetupPixelFormat(multisample))
	{
		return false;
	}

	int prof = WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
	const char *version = Args->CheckValue("-glversion");

	if (version != nullptr && strtod(version, nullptr) < 3.0) prof = WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB;

	for (; prof <= WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB; prof++)
	{
		m_hRC = NULL;
		if (myWglCreateContextAttribsARB != NULL)
		{
			// let's try to get the best version possible. Some drivers only give us the version we request
			// which breaks all version checks for feature support. The highest used features we use are from version 4.4, and 3.0 is a requirement.
			static int versions[] = { 46, 45, 44, 43, 42, 41, 40, 33, 32, 31, 30, -1 };

			for (int i = 0; versions[i] > 0; i++)
			{
				int ctxAttribs[] = {
					WGL_CONTEXT_MAJOR_VERSION_ARB, versions[i] / 10,
					WGL_CONTEXT_MINOR_VERSION_ARB, versions[i] % 10,
					WGL_CONTEXT_FLAGS_ARB, gl_debug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
					WGL_CONTEXT_PROFILE_MASK_ARB, prof,
					0
				};

				m_hRC = myWglCreateContextAttribsARB(m_hDC, 0, ctxAttribs);
				if (m_hRC != NULL) break;
			}
		}

		if (m_hRC == NULL && prof == WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB)
		{
			m_hRC = wglCreateContext(m_hDC);
			if (m_hRC == NULL)
			{
				I_Error("R_OPENGL: Unable to create an OpenGL render context.\n");
				return false;
			}
		}

		if (m_hRC != NULL)
		{
			wglMakeCurrent(m_hDC, m_hRC);
			return true;
		}
	}
	// We get here if the driver doesn't support the modern context creation API which always means an old driver.
	I_Error("R_OPENGL: Unable to create an OpenGL render context. Insufficient driver support for context creation\n");
	return false;
}

//==========================================================================
//
// 
//
//==========================================================================

void Win32GLVideo::Shutdown()
{
	if (m_hRC)
	{
		wglMakeCurrent(0, 0);
		wglDeleteContext(m_hRC);
	}
	if (m_hDC) ReleaseDC(m_Window, m_hDC);
}





