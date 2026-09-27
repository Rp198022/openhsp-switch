//
//	src/hsp3dish/switch/glcompat/GL/glext.h
//	Stub for the <GL/glext.h> include in hgiox.cpp / hgtex.cpp (Switch, T2.3).
//
//	The upstream sources are built with `-DGL_GLEXT_PROTOTYPES` and then include
//	this header expecting the desktop-GL extension entry points.  Everything
//	those files actually use is already declared by our GL/gl.h, so the stub is
//	intentionally empty: it exists only so the include resolves inside the
//	Switch build without dragging in desktop GL.
//
#ifndef __HSP3_SWITCH_GLCOMPAT_GLEXT_H
#define __HSP3_SWITCH_GLCOMPAT_GLEXT_H

/* intentionally empty - see the header comment */

#endif