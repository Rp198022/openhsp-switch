//
//	src/hsp3dish/switch/glcompat/GL/gl.h
//	GLES1 fixed-function -> GLES2 compatibility layer (Nintendo Switch, T2.3)
//
//	WHY THIS FILE EXISTS
//	--------------------
//	OpenHSP's shared SDL2 drawing backend (src/hsp3dish/emscripten/hgiox.cpp and
//	hgtex.cpp) is written entirely against the *fixed-function* OpenGL pipeline:
//
//	    glMatrixMode/glLoadIdentity/glOrtho/glLoadMatrixf
//	    glVertexPointer/glColorPointer/glTexCoordPointer + glEnableClientState
//	    glDrawArrays with client-side arrays, glPointSize, ...
//
//	The Linux build gets away with it because desktop GL still ships a
//	compatibility profile, and the raspbian branch gets away with it because
//	GLES1 (GLES/gl.h) *is* a fixed-function API.  The Switch exposes neither:
//	its mesa port only offers GLES2/GLES3 (verified on real hardware for
//	SDL_GL_CreateContext in docs/R11_report.md).  ES 2.0 has no matrix stack, no
//	client-state arrays and no fixed-function vertex submission, so hgiox.cpp
//	cannot be compiled against it unchanged.
//
//	Rather than edit those two upstream files, this directory *shadows* the
//	headers their non-raspbian branch includes:
//
//	    <GL/gl.h>, <GL/glext.h>, "SDL2/SDL_opengl.h"
//
//	Building with -Isrc/hsp3dish/switch/glcompat ahead of the portlibs include
//	path therefore makes upstream compile as-is, against a header that declares
//	the same GLES1-flavoured API, backed by an ES2 implementation
//	(../gles1_shim.cpp).  Upstream source stays byte-identical.
//
//	The header is deliberately self-contained (no <GLES2/gl2.h>) so that the
//	only external dependency of the shim is SDL2 itself, whose presence on the
//	Switch is already proven (P1/T1.5).
//
//	Every entry point is macro-renamed to a sw_* symbol so the shim can never
//	collide with the real GL symbols that SDL2's static EGL/glapi link drags in.
//
#ifndef __HSP3_SWITCH_GLCOMPAT_GL_H
#define __HSP3_SWITCH_GLCOMPAT_GL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*----------------------------------------------------------------*/
/*	GL types (subset actually used by the OpenHSP SDL2 backend)	  */
/*----------------------------------------------------------------*/

typedef unsigned int	GLenum;
typedef unsigned char	GLboolean;
typedef unsigned int	GLbitfield;
typedef signed char		GLbyte;
typedef short			GLshort;
typedef int				GLint;
typedef int				GLsizei;
typedef unsigned char	GLubyte;
typedef unsigned short	GLushort;
typedef unsigned int	GLuint;
typedef float			GLfloat;
typedef float			GLclampf;
typedef double			GLdouble;
typedef double			GLclampd;
typedef char			GLchar;
typedef void			GLvoid;
typedef ptrdiff_t		GLintptr;
typedef ptrdiff_t		GLsizeiptr;

/*----------------------------------------------------------------*/
/*	GL enums/constants used by hgiox.cpp / hgtex.cpp			  */
/*	(values are the standard Khronos numbers)					  */
/*----------------------------------------------------------------*/

#ifndef GL_FALSE
#define GL_FALSE						0
#define GL_TRUE							1
#endif

#define GL_NO_ERROR						0

/*	primitive types	*/
#define GL_POINTS						0x0000
#define GL_LINES						0x0001
#define GL_LINE_LOOP					0x0002
#define GL_LINE_STRIP					0x0003
#define GL_TRIANGLES					0x0004
#define GL_TRIANGLE_STRIP				0x0005
#define GL_TRIANGLE_FAN					0x0006

/*	clear / buffer bits	*/
#define GL_DEPTH_BUFFER_BIT				0x00000100
#define GL_COLOR_BUFFER_BIT				0x00004000

/*	blend factors	*/
#define GL_ZERO							0
#define GL_ONE							1
#define GL_SRC_ALPHA					0x0302
#define GL_ONE_MINUS_SRC_ALPHA			0x0303
#define GL_SRC_COLOR					0x0300
#define GL_ONE_MINUS_SRC_COLOR			0x0301
#define GL_DST_ALPHA					0x0304
#define GL_ONE_MINUS_DST_ALPHA			0x0305

/*	blend equations (GLES2 core; gmode 6 needs the subtract one)	*/
#define GL_FUNC_ADD						0x8006
#define GL_FUNC_SUBTRACT				0x800A
#define GL_FUNC_REVERSE_SUBTRACT		0x800B

/*	read buffer	*/
#define GL_FRONT						0x0404
#define GL_BACK							0x0405

/*	capabilities	*/
#define GL_LIGHTING						0x0B50
#define GL_POINT_SMOOTH					0x0B10
#define GL_CULL_FACE					0x0B44
#define GL_DEPTH_TEST					0x0B71
#define GL_BLEND						0x0BE2
#define GL_TEXTURE_2D					0x0DE1
#define GL_SCISSOR_TEST					0x0C11

/*	shade model (legacy, accepted and ignored)	*/
#define GL_FLAT							0x1D00
#define GL_SMOOTH						0x1D01

/*	pixel / texture formats	*/
#define GL_ALPHA						0x1906
#define GL_RGB							0x1907
#define GL_RGBA							0x1908
#define GL_LUMINANCE					0x1909
#define GL_LUMINANCE_ALPHA				0x190A
#define GL_UNSIGNED_BYTE				0x1401
#define GL_UNSIGNED_SHORT				0x1403
#define GL_FLOAT						0x1406
#define GL_UNSIGNED_SHORT_4_4_4_4		0x8033
#define GL_UNSIGNED_SHORT_5_5_5_1		0x8034
#define GL_UNSIGNED_SHORT_5_6_5			0x8363

/*	matrix modes (legacy)	*/
#define GL_MODELVIEW					0x1700
#define GL_PROJECTION					0x1701
#define GL_TEXTURE						0x1702

/*	client state arrays (legacy)	*/
#define GL_VERTEX_ARRAY					0x8074
#define GL_COLOR_ARRAY					0x8076
#define GL_TEXTURE_COORD_ARRAY			0x8078

/*	texture parameters	*/
#define GL_TEXTURE_MAG_FILTER			0x2800
#define GL_TEXTURE_MIN_FILTER			0x2801
#define GL_TEXTURE_WRAP_S				0x2802
#define GL_TEXTURE_WRAP_T				0x2803
#define GL_NEAREST						0x2600
#define GL_LINEAR						0x2601
#define GL_NEAREST_MIPMAP_NEAREST		0x2700
#define GL_LINEAR_MIPMAP_NEAREST		0x2701
#define GL_NEAREST_MIPMAP_LINEAR		0x2702
#define GL_LINEAR_MIPMAP_LINEAR			0x2703
#define GL_REPEAT						0x2901
#define GL_CLAMP_TO_EDGE				0x812F

/*	framebuffer objects (GLES2 core; used by hgiox_switch.cpp to make a	*/
/*	`buffer` screen a real render target)								*/
#define GL_OUT_OF_MEMORY 0x0505
#define GL_NONE							0
#define GL_FRAMEBUFFER					0x8D40
#define GL_RENDERBUFFER					0x8D41
#define GL_COLOR_ATTACHMENT0			0x8CE0
#define GL_DEPTH_ATTACHMENT				0x8D00
#define GL_FRAMEBUFFER_COMPLETE			0x8CD5
#define GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT			0x8CD6
#define GL_FRAMEBUFFER_UNSUPPORTED						0x8CDD

/*	shader plumbing	*/
#define GL_FRAGMENT_SHADER				0x8B30
#define GL_VERTEX_SHADER				0x8B31
#define GL_COMPILE_STATUS				0x8B81
#define GL_LINK_STATUS					0x8B82
#define GL_ARRAY_BUFFER					0x8892

/*----------------------------------------------------------------*/
/*	The API, reimplemented on top of GLES2 in ../gles1_shim.cpp	  */
/*	Macro-renamed so no real GL symbol can clash at link time.	  */
/*----------------------------------------------------------------*/

#define glEnable					sw_glEnable
#define glDisable					sw_glDisable
#define glEnableClientState			sw_glEnableClientState
#define glDisableClientState		sw_glDisableClientState

#define glMatrixMode				sw_glMatrixMode
#define glLoadIdentity				sw_glLoadIdentity
#define glLoadMatrixf				sw_glLoadMatrixf
#define glOrtho						sw_glOrtho
#define glOrthof					sw_glOrthof
#define glFrustum					sw_glFrustum

#define glVertexPointer				sw_glVertexPointer
#define glColorPointer				sw_glColorPointer
#define glTexCoordPointer			sw_glTexCoordPointer
#define glDrawArrays				sw_glDrawArrays

#define glViewport					sw_glViewport
#define glClear						sw_glClear
#define glClearColor				sw_glClearColor
#define glBlendFunc					sw_glBlendFunc
#define glBlendEquation				sw_glBlendEquation
#define glFinish					sw_glFinish
#define glPointSize					sw_glPointSize
#define glLineWidth					sw_glLineWidth
#define glShadeModel				sw_glShadeModel
#define glReadBuffer				sw_glReadBuffer
#define glReadPixels				sw_glReadPixels

#define glGenFramebuffers			sw_glGenFramebuffers
#define glDeleteFramebuffers		sw_glDeleteFramebuffers
#define glBindFramebuffer			sw_glBindFramebuffer
#define glFramebufferTexture2D		sw_glFramebufferTexture2D
#define glCheckFramebufferStatus	sw_glCheckFramebufferStatus
#define glIsTexture					sw_glIsTexture

#define glGenTextures				sw_glGenTextures
#define glDeleteTextures			sw_glDeleteTextures
#define glBindTexture				sw_glBindTexture
#define glTexImage2D				sw_glTexImage2D
#define glTexSubImage2D				sw_glTexSubImage2D

#define glCopyTexImage2D				sw_glCopyTexImage2D
#define glTexParameteri				sw_glTexParameteri

void sw_glEnable( GLenum cap );
void sw_glDisable( GLenum cap );
void sw_glEnableClientState( GLenum array );
void sw_glDisableClientState( GLenum array );

void sw_glMatrixMode( GLenum mode );
void sw_glLoadIdentity( void );
void sw_glLoadMatrixf( const GLfloat *m );
void sw_glOrtho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar );
void sw_glOrthof( GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat zNear, GLfloat zFar );
void sw_glFrustum( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar );

void sw_glVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer );
void sw_glColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer );
void sw_glTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer );
void sw_glDrawArrays( GLenum mode, GLint first, GLsizei count );

void sw_glViewport( GLint x, GLint y, GLsizei width, GLsizei height );
void sw_glClear( GLbitfield mask );
void sw_glClearColor( GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha );
void sw_glBlendFunc( GLenum sfactor, GLenum dfactor );
void sw_glBlendEquation( GLenum mode );
void sw_glFinish( void );
void sw_glColorKey( int on, unsigned int rgb );
void sw_glPointSize( GLfloat size );
void sw_glLineWidth( GLfloat width );
void sw_glShadeModel( GLenum mode );
void sw_glReadBuffer( GLenum mode );
void sw_glReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels );

void sw_glGenTextures( GLsizei n, GLuint *textures );
void sw_glDeleteTextures( GLsizei n, const GLuint *textures );
void sw_glBindTexture( GLenum target, GLuint texture );
void sw_glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
					  GLint border, GLenum format, GLenum type, const GLvoid *pixels );
void sw_glTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
						 GLenum format, GLenum type, const GLvoid *pixels );
void sw_glCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border );
int sw_glCopyTexImage2DAvailable( void );
void sw_frame_tick( void );
void sw_glTexParameteri( GLenum target, GLenum pname, GLint param );

void sw_glGenFramebuffers( GLsizei n, GLuint *framebuffers );
void sw_glDeleteFramebuffers( GLsizei n, const GLuint *framebuffers );
void sw_glBindFramebuffer( GLenum target, GLuint framebuffer );
void sw_glFramebufferTexture2D( GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level );
GLenum sw_glCheckFramebufferStatus( GLenum target );
GLboolean sw_glIsTexture( GLuint texture );
GLenum sw_glGetError( void );
GLenum sw_glDrainErrors( const char *stage, GLuint texture );
GLboolean sw_glOutOfMemory( void );
GLboolean sw_glTextureReady( GLuint texture, GLsizei width, GLsizei height );

#ifdef __cplusplus
}
#endif

#endif