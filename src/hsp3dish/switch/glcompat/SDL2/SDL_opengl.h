//
//	src/hsp3dish/switch/glcompat/SDL2/SDL_opengl.h
//	Stub for the "SDL2/SDL_opengl.h" include in hgiox.cpp / hgtex.cpp (Switch, T2.3).
//
//	On every platform SDL_opengl.h eventually pulls in a *desktop GL* header
//	(<GL/gl.h>), which does not exist in the Switch portlibs and would not be
//	usable anyway (no fixed-function pipeline on the Switch).  The two files
//	that include it only need GL types and entry points, and they get those
//	from our GL/gl.h - which is included immediately before this one.
//
//	Shipped only because the include is quoted ("SDL2/SDL_opengl.h") and the
//	compiler searches -I paths in order, so this directory shadows the real
//	SDL2 one.  No such path is used for <SDL2/SDL.h> or <SDL2/SDL_image.h>,
//	which continue to come from the real Switch SDL2 port.
//
#ifndef __HSP3_SWITCH_GLCOMPAT_SDL_OPENGL_H
#define __HSP3_SWITCH_GLCOMPAT_SDL_OPENGL_H

#include <GL/gl.h>

/* intentionally empty beyond the include above */

#endif