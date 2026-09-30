//
//	src/hsp3dish/switch/gles1_shim.cpp
//	GLES1 fixed-function -> GLES2 implementation for the Nintendo Switch (T2.3)
//
//	See ../glcompat/GL/gl.h for why this exists and how upstream is kept
//	untouched.  In short: OpenHSP's shared SDL2 drawing backend
//	(src/hsp3dish/emscripten/hgiox.cpp, hgtex.cpp) submits geometry through the
//	fixed-function pipeline - a matrix stack, client-side vertex/colour/texcoord
//	arrays addressed by glVertexPointer/glColorPointer/glTexCoordPointer,
//	glEnableClientState, and glDrawArrays.  GLES2 has none of that; the Switch
//	only offers GLES2/GLES3 (docs/R11_report.md).
//
//	This file reimplements exactly the slice of that API the backend uses:
//
//	  * a projection/model-view matrix pair, with glOrtho/glOrthof/glFrustum/
//	    glLoadMatrixf/glLoadIdentity/glMatrixMode
//	  * a single GLSL ES 1.00 program that consumes the recorded client arrays
//	    as generic vertex attributes, so glDrawArrays keeps working unchanged
//	  * the state bits the backend toggles (GL_TEXTURE_2D, GL_BLEND, point size)
//	  * a texture pass-through that repairs the one real ES2 gap the backend
//	    walks into: NPOT textures with the default GL_REPEAT wrap are incomplete
//	    in ES2 (and would sample black), so wrap is forced to CLAMP_TO_EDGE for
//	    NPOT textures - matching what desktop GL silently allows.
//
//	All real GL entry points are resolved through SDL_GL_GetProcAddress() rather
//	than linked.  That keeps the link line identical to the verified R11 probe
//	(-lEGL -lglapi -ldrm_nouveau, all of which SDL2's static EGL/GLES binding
//	needs anyway) and avoids any chance of colliding with the real gl* symbols.
//
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <SDL2/SDL.h>

#include <GL/gl.h>

#include "switch_input.h"		/* switch_input_poll(), driven from hgio_render_start */

/*----------------------------------------------------------------*/
/*	Real GLES2 entry points										  */
/*----------------------------------------------------------------*/

typedef void (*PFN_glClear)( GLbitfield );
typedef void (*PFN_glClearColor)( GLclampf, GLclampf, GLclampf, GLclampf );
typedef void (*PFN_glViewport)( GLint, GLint, GLsizei, GLsizei );
typedef void (*PFN_glEnable)( GLenum );
typedef void (*PFN_glDisable)( GLenum );
typedef void (*PFN_glBlendFunc)( GLenum, GLenum );
typedef void (*PFN_glBlendEquation)( GLenum );
typedef void (*PFN_glGenTextures)( GLsizei, GLuint * );
typedef void (*PFN_glDeleteTextures)( GLsizei, const GLuint * );
typedef void (*PFN_glBindTexture)( GLenum, GLuint );
typedef void (*PFN_glTexImage2D)( GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void * );
typedef void (*PFN_glTexSubImage2D)( GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void * );
typedef void (*PFN_glCopyTexImage2D)( GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint );
typedef void (*PFN_glTexParameteri)( GLenum, GLenum, GLint );
typedef void (*PFN_glDrawArrays)( GLenum, GLint, GLsizei );
typedef void (*PFN_glReadPixels)( GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void * );
typedef void (*PFN_glLineWidth)( GLfloat );
typedef GLenum (*PFN_glGetError)( void );
typedef void (*PFN_glGenFramebuffers)( GLsizei, GLuint * );
typedef void (*PFN_glDeleteFramebuffers)( GLsizei, const GLuint * );
typedef void (*PFN_glBindFramebuffer)( GLenum, GLuint );
typedef void (*PFN_glFramebufferTexture2D)( GLenum, GLenum, GLenum, GLuint, GLint );
typedef GLenum (*PFN_glCheckFramebufferStatus)( GLenum );
typedef GLboolean (*PFN_glIsTexture)( GLuint );

typedef GLuint (*PFN_glCreateShader)( GLenum );
typedef void (*PFN_glShaderSource)( GLuint, GLsizei, const char *const *, const GLint * );
typedef void (*PFN_glCompileShader)( GLuint );
typedef void (*PFN_glGetShaderiv)( GLuint, GLenum, GLint * );
typedef void (*PFN_glGetShaderInfoLog)( GLuint, GLsizei, GLsizei *, char * );
typedef GLuint (*PFN_glCreateProgram)( void );
typedef void (*PFN_glAttachShader)( GLuint, GLuint );
typedef void (*PFN_glBindAttribLocation)( GLuint, GLuint, const char * );
typedef void (*PFN_glLinkProgram)( GLuint );
typedef void (*PFN_glGetProgramiv)( GLuint, GLenum, GLint * );
typedef void (*PFN_glGetProgramInfoLog)( GLuint, GLsizei, GLsizei *, char * );
typedef void (*PFN_glDeleteShader)( GLuint );
typedef void (*PFN_glUseProgram)( GLuint );
typedef GLint (*PFN_glGetUniformLocation)( GLuint, const char * );
typedef void (*PFN_glUniform1i)( GLint, GLint );
typedef void (*PFN_glUniform1f)( GLint, GLfloat );
typedef void (*PFN_glUniform4f)( GLint, GLfloat, GLfloat, GLfloat, GLfloat );
typedef void (*PFN_glUniformMatrix4fv)( GLint, GLsizei, GLboolean, const GLfloat * );
typedef void (*PFN_glEnableVertexAttribArray)( GLuint );
typedef void (*PFN_glDisableVertexAttribArray)( GLuint );
typedef void (*PFN_glVertexAttribPointer)( GLuint, GLint, GLenum, GLboolean, GLsizei, const void * );
typedef GLboolean (*PFN_glIsProgram)( GLuint );

static PFN_glClear						gl_clear;
static PFN_glClearColor					gl_clearcolor;
static PFN_glViewport					gl_viewport;
static PFN_glEnable						gl_enable;
static PFN_glDisable					gl_disable;
static PFN_glBlendFunc					gl_blendfunc;
static PFN_glBlendEquation				gl_blendequation;
static PFN_glGenTextures				gl_gentextures;
static PFN_glDeleteTextures				gl_deletetextures;
static PFN_glBindTexture				gl_bindtexture;
static PFN_glTexImage2D					gl_teximage2d;
static PFN_glTexSubImage2D				gl_texsubimage2d;
static PFN_glCopyTexImage2D				gl_copyteximage2d;
static PFN_glTexParameteri				gl_texparameteri;
static PFN_glDrawArrays					gl_drawarrays;
static PFN_glReadPixels					gl_readpixels;
static PFN_glLineWidth					gl_linewidth;
static PFN_glGetError					gl_geterror;

static PFN_glGenFramebuffers			gl_genframebuffers;
static PFN_glDeleteFramebuffers			gl_deleteframebuffers;
static PFN_glBindFramebuffer			gl_bindframebuffer;
static PFN_glFramebufferTexture2D		gl_framebuffertexture2d;
static PFN_glCheckFramebufferStatus		gl_checkframebufferstatus;
static PFN_glIsTexture					gl_istexture;

static PFN_glCreateShader				gl_createshader;
static PFN_glShaderSource				gl_shadersource;
static PFN_glCompileShader				gl_compileshader;
static PFN_glGetShaderiv				gl_getshaderiv;
static PFN_glGetShaderInfoLog			gl_getshaderinfolog;
static PFN_glCreateProgram				gl_createprogram;
static PFN_glAttachShader				gl_attacheshader;
static PFN_glBindAttribLocation			gl_bindattriblocation;
static PFN_glLinkProgram				gl_linkprogram;
static PFN_glGetProgramiv				gl_getprogramiv;
static PFN_glGetProgramInfoLog			gl_getprograminfolog;
static PFN_glDeleteShader				gl_deleteshader;
static PFN_glUseProgram					gl_useprogram;
static PFN_glGetUniformLocation			gl_getuniformlocation;
static PFN_glUniform1i					gl_uniform1i;
static PFN_glUniform1f					gl_uniform1f;
static PFN_glUniform4f					gl_uniform4f;
static PFN_glUniformMatrix4fv			gl_uniformmatrix4fv;
static PFN_glEnableVertexAttribArray	gl_enablevertexattribarray;
static PFN_glDisableVertexAttribArray	gl_disablevertexattribarray;
static PFN_glVertexAttribPointer		gl_vertexattribpointer;
static PFN_glIsProgram					gl_isprogram;

/*----------------------------------------------------------------*/
/*	State carried over from the fixed-function API				  */
/*----------------------------------------------------------------*/

#define SW_ATTR_POS		0
#define SW_ATTR_TEX		1
#define SW_ATTR_COL		2

typedef struct {
	GLint			size;
	GLenum			type;
	GLsizei			stride;
	const GLvoid	*ptr;
	GLboolean		enabled;
} sw_array;

static sw_array		sw_vtx;
static sw_array		sw_col;
static sw_array		sw_tex;
static GLboolean	sw_col_client_enabled;	/* glEnableClientState(GL_COLOR_ARRAY)	*/
static GLboolean	sw_tex_client_enabled;	/* glEnableClientState(GL_TEXTURE_COORD_ARRAY) */

static GLboolean	sw_texture2d;			/* glEnable(GL_TEXTURE_2D)				*/
static GLuint		sw_bound_tex;

#define SW_TEXTURE_STORAGE_MAX 4096

typedef struct {
	GLuint texture;
	GLsizei width, height;
	GLint internalformat;
	GLenum format, type;
	GLboolean allocated;
} sw_texture_storage;

static sw_texture_storage sw_texture_storage_table[SW_TEXTURE_STORAGE_MAX];
static GLboolean sw_oom;
static unsigned sw_alloc_report;

static sw_texture_storage *sw_find_texture_storage( GLuint texture, int create )
{
	sw_texture_storage *empty = NULL;
	if ( texture == 0 ) return NULL;
	for ( int i = 0; i < SW_TEXTURE_STORAGE_MAX; i++ ) {
		sw_texture_storage *s = &sw_texture_storage_table[i];
		if ( s->texture == texture ) return s;
		if ( s->texture == 0 && empty == NULL ) empty = s;
	}
	if ( create && empty != NULL ) {
		memset( empty, 0, sizeof( *empty ) );
		empty->texture = texture;
		return empty;
	}
	return NULL;
}
static GLfloat		sw_point_size;

/*	Matrix state (column-major, same layout glLoadMatrixf expects)		*/
static GLenum		sw_matrix_mode = GL_MODELVIEW;
static GLfloat		sw_mat_proj[16];
static GLfloat		sw_mat_model[16];

static GLuint		sw_prog;
static GLint		sw_u_mvp, sw_u_tex, sw_u_usetex, sw_u_usecol, sw_u_color, sw_u_pointsize;
static GLint		sw_u_colkey, sw_u_colkeycol;
static int		sw_colkey_on = 0;
static GLfloat	sw_colkey_r = 0.f, sw_colkey_g = 0.f, sw_colkey_b = 0.f;

static int			sw_ready;				/* entry points resolved			*/
static int			sw_init_failed;
static int			sw_frames_reported;
static int			sw_frame_no;			/* glClear calls = frames begun		*/
static int			sw_draw_no;
static unsigned	sw_skip_no = 0;				/* glDrawArrays calls				*/

/*----------------------------------------------------------------*/
/*	Helpers														  */
/*----------------------------------------------------------------*/

static void sw_say( const char *fmt, ... )
{
	va_list ap;
	char buf[512];
	FILE *fp;

	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );

	fputs( buf, stdout );
	fflush( stdout );		/* nxlink socket output is fully buffered */

	/*	A copy on the card, so a run started from the menu is readable too. */
	fp = fopen( "hsp3dish_diag.log", "ab" );
	if ( fp != NULL ) {
		fputs( buf, fp );
		fclose( fp );
	}
}

static void sw_identity( GLfloat *m )
{
	memset( m, 0, sizeof( GLfloat ) * 16 );
	m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void sw_matmul( const GLfloat *a, const GLfloat *b, GLfloat *out )
{
	int c, r, k;
	for ( c = 0; c < 4; c++ ) {
		for ( r = 0; r < 4; r++ ) {
			GLfloat s = 0.0f;
			for ( k = 0; k < 4; k++ ) {
				s += a[k * 4 + r] * b[c * 4 + k];
			}
			out[c * 4 + r] = s;
		}
	}
}

static void sw_ortho( GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f, GLfloat *m )
{
	GLfloat rl = r - l;
	GLfloat tb = t - b;
	GLfloat fn = f - n;

	sw_identity( m );
	m[0] = 2.0f / rl;
	m[5] = 2.0f / tb;
	m[10] = -2.0f / fn;
	m[12] = -( r + l ) / rl;
	m[13] = -( t + b ) / tb;
	m[14] = -( f + n ) / fn;
}

static void sw_frustum( GLfloat l, GLfloat r, GLfloat b, GLfloat t, GLfloat n, GLfloat f, GLfloat *m )
{
	GLfloat rl = r - l;
	GLfloat tb = t - b;
	GLfloat fn = f - n;

	memset( m, 0, sizeof( GLfloat ) * 16 );
	m[0] = 2.0f * n / rl;
	m[5] = 2.0f * n / tb;
	m[8] = ( r + l ) / rl;
	m[9] = ( t + b ) / tb;
	m[10] = -( f + n ) / fn;
	m[11] = -1.0f;
	m[14] = -2.0f * f * n / fn;
}

static GLfloat *sw_current_matrix( void )
{
	return ( sw_matrix_mode == GL_PROJECTION ) ? sw_mat_proj : sw_mat_model;
}

static int sw_is_pot( GLsizei v )
{
	return v > 0 && ( v & ( v - 1 ) ) == 0;
}

/*----------------------------------------------------------------*/
/*	Shader program												  */
/*----------------------------------------------------------------*/

static const char *SW_VS =
	"uniform mat4 u_mvp;\n"
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_tex;\n"
	"attribute vec4 a_col;\n"
	"uniform float u_pointsize;\n"
	"varying vec2 v_tex;\n"
	"varying vec4 v_col;\n"
	"void main() {\n"
	"    v_tex = a_tex;\n"
	"    v_col = a_col;\n"
	"    gl_Position = u_mvp * vec4( a_pos, 0.0, 1.0 );\n"
	"    gl_PointSize = u_pointsize;\n"
	"}\n";

static const char *SW_FS =
	"precision mediump float;\n"
	"uniform sampler2D u_tex;\n"
	"uniform int u_usetex;\n"
	"uniform int u_usecol;\n"
	"uniform vec4 u_color;\n"
	"uniform int u_colkey;\n"
	"uniform vec4 u_colkeycol;\n"
	"varying vec2 v_tex;\n"
	"varying vec4 v_col;\n"
	"void main() {\n"
	"    vec4 tc = vec4( 1.0 );\n"
	"    if ( u_usetex == 1 ) {\n"
	"        tc = texture2D( u_tex, v_tex );\n"
	"    }\n"
	"    if ( u_colkey == 1 ) {\n"
	"        vec3 d = abs( tc.rgb - u_colkeycol.rgb );\n"
	"        if ( d.r < 0.004 && d.g < 0.004 && d.b < 0.004 ) discard;\n"
	"    }\n"
	"    gl_FragColor = ( ( u_usecol == 1 ) ? v_col : u_color ) * tc;\n"
	"}\n";

static GLuint sw_compile( GLenum type, const char *src, const char *tag )
{
	GLuint sh;
	GLint ok = 0;
	char log[1024];

	sh = gl_createshader( type );
	if ( !sh ) {
		sw_say( "gles1shim: glCreateShader(%s) failed\n", tag );
		return 0;
	}
	gl_shadersource( sh, 1, &src, NULL );
	gl_compileshader( sh );
	gl_getshaderiv( sh, GL_COMPILE_STATUS, &ok );
	if ( !ok ) {
		log[0] = 0;
		gl_getshaderinfolog( sh, sizeof( log ) - 1, NULL, log );
		sw_say( "gles1shim: %s shader compile FAILED: %s\n", tag, log );
		gl_deleteshader( sh );
		return 0;
	}
	return sh;
}

static int sw_build_program( void )
{
	GLuint vs, fs;
	GLint ok = 0;
	char log[1024];

	vs = sw_compile( GL_VERTEX_SHADER, SW_VS, "vertex" );
	if ( !vs ) return -1;
	fs = sw_compile( GL_FRAGMENT_SHADER, SW_FS, "fragment" );
	if ( !fs ) {
		gl_deleteshader( vs );
		return -1;
	}

	sw_prog = gl_createprogram();
	if ( !sw_prog ) {
		sw_say( "gles1shim: glCreateProgram failed\n" );
		return -1;
	}
	gl_attacheshader( sw_prog, vs );
	gl_attacheshader( sw_prog, fs );

	/*	Fix the attribute slots so the shim never has to query them.	*/
	gl_bindattriblocation( sw_prog, SW_ATTR_POS, "a_pos" );
	gl_bindattriblocation( sw_prog, SW_ATTR_TEX, "a_tex" );
	gl_bindattriblocation( sw_prog, SW_ATTR_COL, "a_col" );

	gl_linkprogram( sw_prog );
	gl_getprogramiv( sw_prog, GL_LINK_STATUS, &ok );
	if ( !ok ) {
		log[0] = 0;
		gl_getprograminfolog( sw_prog, sizeof( log ) - 1, NULL, log );
		sw_say( "gles1shim: program link FAILED: %s\n", log );
		return -1;
	}
	gl_deleteshader( vs );
	gl_deleteshader( fs );

	sw_u_mvp = gl_getuniformlocation( sw_prog, "u_mvp" );
	sw_u_tex = gl_getuniformlocation( sw_prog, "u_tex" );
	sw_u_usetex = gl_getuniformlocation( sw_prog, "u_usetex" );
	sw_u_usecol = gl_getuniformlocation( sw_prog, "u_usecol" );
	sw_u_color = gl_getuniformlocation( sw_prog, "u_color" );
	sw_u_pointsize = gl_getuniformlocation( sw_prog, "u_pointsize" );
	sw_u_colkey = gl_getuniformlocation( sw_prog, "u_colkey" );
	sw_u_colkeycol = gl_getuniformlocation( sw_prog, "u_colkeycol" );

	sw_say( "gles1shim: program ok (mjvp=%d usetex=%d usecol=%d)\n",
		(int)sw_u_mvp, (int)sw_u_usetex, (int)sw_u_usecol );

	gl_useprogram( sw_prog );
	gl_uniform1i( sw_u_tex, 0 );				/* sampler -> texture unit 0	*/
	gl_uniform4f( sw_u_color, 1.f, 1.f, 1.f, 1.f );
	gl_uniform1f( sw_u_pointsize, 1.f );
	return 0;
}

/*----------------------------------------------------------------*/
/*	Lazy initialisation											  */
/*----------------------------------------------------------------*/

#define SW_LOAD( fn, name )	do { *(void **)( &fn ) = SDL_GL_GetProcAddress( name ); \
		if ( !fn ) missing++; } while ( 0 )

static void sw_init( void )
{
	int missing = 0;

	if ( sw_ready || sw_init_failed ) return;

	SW_LOAD( gl_clear, "glClear" );
	SW_LOAD( gl_clearcolor, "glClearColor" );
	SW_LOAD( gl_viewport, "glViewport" );
	SW_LOAD( gl_enable, "glEnable" );
	SW_LOAD( gl_disable, "glDisable" );
	SW_LOAD( gl_blendfunc, "glBlendFunc" );
	SW_LOAD( gl_gentextures, "glGenTextures" );
	SW_LOAD( gl_deletetextures, "glDeleteTextures" );
	SW_LOAD( gl_bindtexture, "glBindTexture" );
	SW_LOAD( gl_teximage2d, "glTexImage2D" );
	SW_LOAD( gl_texsubimage2d, "glTexSubImage2D" );
	SW_LOAD( gl_copyteximage2d, "glCopyTexImage2D" );
	SW_LOAD( gl_texparameteri, "glTexParameteri" );
	SW_LOAD( gl_drawarrays, "glDrawArrays" );
	SW_LOAD( gl_readpixels, "glReadPixels" );
	SW_LOAD( gl_linewidth, "glLineWidth" );
	SW_LOAD( gl_geterror, "glGetError" );

	/*	Framebuffer objects are GLES2 core, but the single-screen backend never
		used them; they are only needed by the offscreen-target fork
		(hgiox_switch.cpp).  Loaded outside SW_LOAD's missing counter so that a
		driver without them cannot disable *all* rendering.					*/
	*(void **)( &gl_genframebuffers ) = SDL_GL_GetProcAddress( "glGenFramebuffers" );
	*(void **)( &gl_deleteframebuffers ) = SDL_GL_GetProcAddress( "glDeleteFramebuffers" );
	*(void **)( &gl_bindframebuffer ) = SDL_GL_GetProcAddress( "glBindFramebuffer" );
	*(void **)( &gl_framebuffertexture2d ) = SDL_GL_GetProcAddress( "glFramebufferTexture2D" );
	*(void **)( &gl_checkframebufferstatus ) = SDL_GL_GetProcAddress( "glCheckFramebufferStatus" );
	*(void **)( &gl_istexture ) = SDL_GL_GetProcAddress( "glIsTexture" );
	/*	same reasoning: a driver without glBlendEquation must not take the
		whole renderer down, it only loses gmode 6's subtract.			*/
	*(void **)( &gl_blendequation ) = SDL_GL_GetProcAddress( "glBlendEquation" );
	SW_LOAD( gl_createshader, "glCreateShader" );
	SW_LOAD( gl_shadersource, "glShaderSource" );
	SW_LOAD( gl_compileshader, "glCompileShader" );
	SW_LOAD( gl_getshaderiv, "glGetShaderiv" );
	SW_LOAD( gl_getshaderinfolog, "glGetShaderInfoLog" );
	SW_LOAD( gl_createprogram, "glCreateProgram" );
	SW_LOAD( gl_attacheshader, "glAttachShader" );
	SW_LOAD( gl_bindattriblocation, "glBindAttribLocation" );
	SW_LOAD( gl_linkprogram, "glLinkProgram" );
	SW_LOAD( gl_getprogramiv, "glGetProgramiv" );
	SW_LOAD( gl_getprograminfolog, "glGetProgramInfoLog" );
	SW_LOAD( gl_deleteshader, "glDeleteShader" );
	SW_LOAD( gl_useprogram, "glUseProgram" );
	SW_LOAD( gl_getuniformlocation, "glGetUniformLocation" );
	SW_LOAD( gl_uniform1i, "glUniform1i" );
	SW_LOAD( gl_uniform1f, "glUniform1f" );
	SW_LOAD( gl_uniform4f, "glUniform4f" );
	SW_LOAD( gl_uniformmatrix4fv, "glUniformMatrix4fv" );
	SW_LOAD( gl_enablevertexattribarray, "glEnableVertexAttribArray" );
	SW_LOAD( gl_disablevertexattribarray, "glDisableVertexAttribArray" );
	SW_LOAD( gl_vertexattribpointer, "glVertexAttribPointer" );
	SW_LOAD( gl_isprogram, "glIsProgram" );

	if ( missing != 0 ) {
		/*	Almost always means "called before an SDL GL context existed" -
			keep sw_init_failed clear so the next call retries.			*/
		sw_say( "gles1shim: %d GL entry point(s) unavailable (no current context yet?)\n", missing );
		return;
	}

	sw_identity( sw_mat_proj );
	sw_identity( sw_mat_model );
	sw_texture2d = GL_FALSE;
	sw_bound_tex = 0;
	memset( sw_texture_storage_table, 0, sizeof( sw_texture_storage_table ) );
	sw_point_size = 1.0f;
	sw_vtx.enabled = GL_TRUE;			/* glVertexPointer is always live here	*/
	sw_col.enabled = GL_FALSE;
	sw_tex.enabled = GL_TRUE;
	sw_col_client_enabled = GL_FALSE;
	sw_tex_client_enabled = GL_FALSE;

	if ( sw_build_program() != 0 ) {
		sw_init_failed = 1;
		sw_say( "gles1shim: *** shader setup failed - rendering disabled\n" );
		return;
	}

	sw_ready = 1;
	sw_say( "gles1shim: ready\n" );
}

/*	The reused Linux platform glue rebuilds the window/GL context whenever the
	script issues a `screen` command - src/hsp3dish/hsp3gr_dish.cpp:937-940 sets
	RUNMODE_RESTART unconditionally for HSPLINUX targets, and hsp3excmd_rebuild_window()
	then recreates the window and the context.  Every GL object the shim owns
	becomes invalid at that point, so the shader program has to be built again.

	Detecting it through glIsProgram() is deliberate: it does not depend on the
	context handle changing (a freed/reallocated handle can reuse the same
	address), and it is exactly the question that matters - "is our program still
	a live object in the current context?".										*/
static int sw_ensure_gl( void )
{
	if ( sw_init_failed ) return 0;
	if ( !sw_ready ) sw_init();
	if ( !sw_ready ) return 0;

	if ( gl_isprogram != NULL && gl_isprogram( sw_prog ) == GL_FALSE ) {
		sw_say( "gles1shim: program %u is gone (GL context was rebuilt) - recreating\n",
			(unsigned)sw_prog );
		sw_prog = 0;
		sw_ready = 0;
		sw_init();
	}
	return sw_ready;
}

/*----------------------------------------------------------------*/
/*	Capability classification									  */
/*----------------------------------------------------------------*/

static int sw_cap_is_es2_valid( GLenum cap )
{
	switch ( cap ) {
	case GL_BLEND:
	case GL_CULL_FACE:
	case GL_DEPTH_TEST:
	case GL_SCISSOR_TEST:
		return 1;
	default:
		return 0;
	}
}

/*----------------------------------------------------------------*/
/*	API: state													  */
/*----------------------------------------------------------------*/

void sw_glEnable( GLenum cap )
{
	sw_init();
	switch ( cap ) {
	case GL_TEXTURE_2D:
		sw_texture2d = GL_TRUE;
		return;
	case GL_POINT_SMOOTH:
	case GL_LIGHTING:
	case GL_DEPTH_BUFFER_BIT:			/* hgiox.cpp disables this by mistake	*/
		return;							/* accepted and ignored, like GLES1		*/
	default:
		break;
	}
	if ( sw_ready && sw_cap_is_es2_valid( cap ) ) gl_enable( cap );
}

void sw_glDisable( GLenum cap )
{
	sw_init();
	switch ( cap ) {
	case GL_TEXTURE_2D:
		sw_texture2d = GL_FALSE;
		return;
	case GL_POINT_SMOOTH:
	case GL_LIGHTING:
	case GL_DEPTH_BUFFER_BIT:
		return;
	default:
		break;
	}
	if ( sw_ready && sw_cap_is_es2_valid( cap ) ) gl_disable( cap );
}

void sw_glEnableClientState( GLenum array )
{
	sw_init();
	switch ( array ) {
	case GL_VERTEX_ARRAY:
		sw_vtx.enabled = GL_TRUE;
		break;
	case GL_COLOR_ARRAY:
		sw_col_client_enabled = GL_TRUE;
		break;
	case GL_TEXTURE_COORD_ARRAY:
		sw_tex_client_enabled = GL_TRUE;
		break;
	default:
		break;
	}
}

void sw_glDisableClientState( GLenum array )
{
	sw_init();
	switch ( array ) {
	case GL_VERTEX_ARRAY:
		sw_vtx.enabled = GL_FALSE;
		break;
	case GL_COLOR_ARRAY:
		sw_col_client_enabled = GL_FALSE;
		break;
	case GL_TEXTURE_COORD_ARRAY:
		sw_tex_client_enabled = GL_FALSE;
		break;
	default:
		break;
	}
}

/*----------------------------------------------------------------*/
/*	API: matrices												  */
/*----------------------------------------------------------------*/

void sw_glMatrixMode( GLenum mode )
{
	sw_init();
	if ( mode == GL_PROJECTION || mode == GL_MODELVIEW ) {
		sw_matrix_mode = mode;
	}
}

void sw_glLoadIdentity( void )
{
	sw_init();
	sw_identity( sw_current_matrix() );
}

void sw_glLoadMatrixf( const GLfloat *m )
{
	sw_init();
	if ( m == NULL ) return;
	memcpy( sw_current_matrix(), m, sizeof( GLfloat ) * 16 );
}

void sw_glOrtho( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar )
{
	sw_init();
	sw_ortho( (GLfloat)left, (GLfloat)right, (GLfloat)bottom, (GLfloat)top, (GLfloat)zNear, (GLfloat)zFar,
		sw_current_matrix() );
}

void sw_glOrthof( GLfloat left, GLfloat right, GLfloat bottom, GLfloat top, GLfloat zNear, GLfloat zFar )
{
	sw_init();
	sw_ortho( left, right, bottom, top, zNear, zFar, sw_current_matrix() );
}

void sw_glFrustum( GLdouble left, GLdouble right, GLdouble bottom, GLdouble top, GLdouble zNear, GLdouble zFar )
{
	sw_init();
	sw_frustum( (GLfloat)left, (GLfloat)right, (GLfloat)bottom, (GLfloat)top, (GLfloat)zNear, (GLfloat)zFar,
		sw_current_matrix() );
}

/*----------------------------------------------------------------*/
/*	API: client arrays + draw									  */
/*----------------------------------------------------------------*/

static void sw_set_pointer( sw_array *dst, GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	dst->size = size;
	dst->type = type;
	dst->stride = stride;
	dst->ptr = pointer;
}

void sw_glVertexPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	sw_init();
	sw_set_pointer( &sw_vtx, size, type, stride, pointer );
}

void sw_glColorPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	sw_init();
	sw_set_pointer( &sw_col, size, type, stride, pointer );
}

void sw_glTexCoordPointer( GLint size, GLenum type, GLsizei stride, const GLvoid *pointer )
{
	sw_init();
	sw_set_pointer( &sw_tex, size, type, stride, pointer );
}

void sw_glDrawArrays( GLenum mode, GLint first, GLsizei count )
{
	GLfloat mvp[16];

	if ( !sw_ensure_gl() ) return;
	if ( sw_vtx.ptr == NULL || sw_vtx.type != GL_FLOAT || count <= 0 ) return;

	if ( sw_texture2d && sw_bound_tex != 0 && sw_tex_client_enabled && sw_tex.ptr != NULL ) {
		/*	A texture whose upload failed has no content, and sampling it
			returns black - which paints over whatever is underneath rather
			than simply not appearing.  The frame is repainted with 122 million
			worth of content at its start and reads zero by the time the mesh is
			flushed, and nothing clears in between, so something draws over it.
			Nothing to draw means nothing drawn.							*/
		sw_texture_storage *st = sw_find_texture_storage( sw_bound_tex, 0 );
		if ( st != NULL && !st->allocated ) {
			sw_skip_no++;
			if ( sw_skip_no <= 8 || ( sw_skip_no % 200 ) == 0 ) {
				sw_say( "gles1shim: skipped draw with tex %u, no storage (total %u)\n",
					(unsigned)sw_bound_tex, (unsigned)sw_skip_no );
			}
			return;
		}
	}
	sw_draw_no++;
	gl_useprogram( sw_prog );

	sw_matmul( sw_mat_proj, sw_mat_model, mvp );
	gl_uniformmatrix4fv( sw_u_mvp, 1, GL_FALSE, mvp );

	gl_uniform1i( sw_u_usetex,
		( sw_texture2d && sw_bound_tex != 0 && sw_tex_client_enabled && sw_tex.ptr != NULL ) ? 1 : 0 );
	gl_uniform1i( sw_u_usecol, sw_col_client_enabled ? 1 : 0 );
	gl_uniform4f( sw_u_color, 1.f, 1.f, 1.f, 1.f );		/* fixed-function current colour */
	gl_uniform1f( sw_u_pointsize, sw_point_size > 0.f ? sw_point_size : 1.f );
	gl_uniform1i( sw_u_colkey, sw_colkey_on );
	gl_uniform4f( sw_u_colkeycol, sw_colkey_r, sw_colkey_g, sw_colkey_b, 1.f );

	gl_enablevertexattribarray( SW_ATTR_POS );
	gl_vertexattribpointer( SW_ATTR_POS, sw_vtx.size, GL_FLOAT, GL_FALSE, sw_vtx.stride, sw_vtx.ptr );

	if ( sw_tex_client_enabled && sw_tex.ptr != NULL && sw_tex.type == GL_FLOAT ) {
		gl_enablevertexattribarray( SW_ATTR_TEX );
		gl_vertexattribpointer( SW_ATTR_TEX, sw_tex.size, GL_FLOAT, GL_FALSE, sw_tex.stride, sw_tex.ptr );
	} else {
		gl_disablevertexattribarray( SW_ATTR_TEX );
	}

	if ( sw_col_client_enabled && sw_col.ptr != NULL && sw_col.type == GL_FLOAT ) {
		gl_enablevertexattribarray( SW_ATTR_COL );
		gl_vertexattribpointer( SW_ATTR_COL, sw_col.size, GL_FLOAT, GL_FALSE, sw_col.stride, sw_col.ptr );
	} else {
		gl_disablevertexattribarray( SW_ATTR_COL );
	}

	gl_drawarrays( mode, first, count );
}

/*----------------------------------------------------------------*/
/*	API: framebuffer											  */
/*----------------------------------------------------------------*/

void sw_glViewport( GLint x, GLint y, GLsizei width, GLsizei height )
{
	sw_init();
	if ( sw_ready ) gl_viewport( x, y, width, height );
}

void sw_glClear( GLbitfield mask )
{
	sw_init();
	if ( sw_ready ) {
		gl_clear( mask );
		/*	Report a GL error once per run so a silently broken frame is at
			least visible over nxlink.										*/
		if ( gl_geterror != NULL && sw_frames_reported < 3 ) {
			GLenum e = sw_glGetError();
			if ( e != GL_NO_ERROR ) {
				sw_say( "gles1shim: glClear left glGetError 0x%x\n", (unsigned)e );
				sw_frames_reported++;
			}
		}
	}
}

/*	The once-per-frame tick.  glClear() used to be the point this port owned
	for that, but the clear now happens once inside the main screen's own
	framebuffer rather than on every frame, so the backend calls this from
	hgio_render_start() instead.  The pad is sampled here for the same
	reason: the script reads it at the top of its own loop, so pumping at the
	frame boundary keeps that read fresh.									*/
void sw_frame_tick( void )
{
	switch_input_poll();

	sw_frame_no++;
	if ( sw_frame_no <= 5 || ( sw_frame_no % 30 ) == 0 ) {
		sw_say( "gles1shim: frame %d, %d draws, t=%u ms\n",
			sw_frame_no, sw_draw_no, (unsigned)SDL_GetTicks() );
	}
}

void sw_glClearColor( GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha )
{
	sw_init();
	if ( sw_ready ) gl_clearcolor( red, green, blue, alpha );
}

/*	The classic (GDI) runtime Elona was written against does not alpha-blend
	pictures loaded from a file: `gmode 2` drops every pixel that equals the
	image's colour key, and `gmode 4` every pixel that equals `color` - see
	GetAttrOperation() in src/hsp3/win32gui/hsp3gr_wingui.cpp.  A BMP carries
	no alpha and the loader here hands every pixel alpha 255, so those key
	pixels reached the screen opaque: the solid black window frames, key
	boxes and chips.  The colour goes to the fragment shader, which discards
	matching fragments.  Cleared by setBlendMode() so nothing else inherits it. */
void sw_glColorKey( int on, unsigned int rgb )
{
	static int logged = 0;
	sw_colkey_on = on ? 1 : 0;
	if ( on && ( logged < 2 ) ) {
		logged++;
		sw_say( "gles1shim: colkey on rgb=%06x loc=%d colloc=%d\n",
			rgb, (int)sw_u_colkey, (int)sw_u_colkeycol );
	}
	sw_colkey_r = (GLfloat)( ( rgb >> 16 ) & 0xffu ) * ( 1.0f / 255.0f );
	sw_colkey_g = (GLfloat)( ( rgb >> 8 ) & 0xffu ) * ( 1.0f / 255.0f );
	sw_colkey_b = (GLfloat)( rgb & 0xffu ) * ( 1.0f / 255.0f );
}


void sw_glBlendFunc( GLenum sfactor, GLenum dfactor )
{
	sw_init();
	if ( sw_ready ) gl_blendfunc( sfactor, dfactor );
}

/*	gmode 6 (減算) is dst*1 - src*alpha; the equation defaults to
	GL_FUNC_ADD, so it has to be reset on every path.				*/
void sw_glBlendEquation( GLenum mode )
{
	sw_init();
	if ( sw_ready && gl_blendequation != NULL ) gl_blendequation( mode );
}

void sw_glPointSize( GLfloat size )
{
	sw_init();
	sw_point_size = size;
}

void sw_glLineWidth( GLfloat width )
{
	sw_init();
	/*	GLES2 clamps line width to 1.0; the call is kept for parity.	*/
	if ( sw_ready ) gl_linewidth( width );
}

void sw_glShadeModel( GLenum mode )
{
	(void)mode;		/* legacy, no ES2 equivalent - accepted and ignored	*/
}

void sw_glReadBuffer( GLenum mode )
{
	(void)mode;		/* ES2 has no glReadBuffer - accepted and ignored	*/
}

void sw_glReadPixels( GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, GLvoid *pixels )
{
	sw_init();
	if ( sw_ready ) gl_readpixels( x, y, width, height, format, type, pixels );
}

/*----------------------------------------------------------------*/
/*	API: textures												  */
/*----------------------------------------------------------------*/

void sw_glGenTextures( GLsizei n, GLuint *textures )
{
	sw_init();
	if ( sw_ready ) gl_gentextures( n, textures );
}

void sw_glDeleteTextures( GLsizei n, const GLuint *textures )
{
	sw_init();
	if ( sw_ready ) {
		gl_deletetextures( n, textures );
		for ( GLsizei i = 0; i < n; i++ ) {
			sw_texture_storage *s = sw_find_texture_storage( textures[i], 0 );
			if ( s != NULL ) memset( s, 0, sizeof( *s ) );
			if ( sw_bound_tex == textures[i] ) sw_bound_tex = 0;
		}
	}
}

void sw_glBindTexture( GLenum target, GLuint texture )
{
	sw_init();
	if ( !sw_ready ) return;
	sw_glDrainErrors( "bind-before", texture );
	gl_bindtexture( target, texture );
	GLenum error = sw_glDrainErrors( "bind-after", texture );
	if ( target == GL_TEXTURE_2D ) sw_bound_tex = error == GL_NO_ERROR ? texture : 0;
}

void sw_glTexParameteri( GLenum target, GLenum pname, GLint param )
{
	sw_init();
	if ( sw_ready ) gl_texparameteri( target, pname, param );
}

/*----------------------------------------------------------------*/
/*	Framebuffer objects											  */
/*	Only used by hgiox_switch.cpp, which turns a screen that owns a	  */
/*	texture into an offscreen render target (upstream's hgio_buffer	  */
/*	is 未実装 and every drawing entry point refused anything but the	  */
/*	main screen, so Elona's `buffer` canvases could not be drawn).	  */
/*----------------------------------------------------------------*/

void sw_glGenFramebuffers( GLsizei n, GLuint *framebuffers )
{
	sw_init();
	if ( sw_ready && gl_genframebuffers ) gl_genframebuffers( n, framebuffers );
}

void sw_glDeleteFramebuffers( GLsizei n, const GLuint *framebuffers )
{
	sw_init();
	if ( sw_ready && gl_deleteframebuffers ) gl_deleteframebuffers( n, framebuffers );
}

void sw_glBindFramebuffer( GLenum target, GLuint framebuffer )
{
	sw_init();
	if ( sw_ready && gl_bindframebuffer ) gl_bindframebuffer( target, framebuffer );
}

void sw_glFramebufferTexture2D( GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level )
{
	sw_init();
	if ( sw_ready && gl_framebuffertexture2d )
		gl_framebuffertexture2d( target, attachment, textarget, texture, level );
}

GLenum sw_glCheckFramebufferStatus( GLenum target )
{
	sw_init();
	if ( sw_ready && gl_checkframebufferstatus ) return gl_checkframebufferstatus( target );
	return GL_FRAMEBUFFER_UNSUPPORTED;
}

GLboolean sw_glIsTexture( GLuint texture )
{
	sw_init();
	if ( sw_ready && gl_istexture ) return gl_istexture( texture );
	return GL_FALSE;
}

GLenum sw_glGetError( void )
{
	sw_init();
	if ( sw_ready && gl_geterror ) {
		GLenum error = gl_geterror();
		if ( error == GL_OUT_OF_MEMORY ) sw_oom = GL_TRUE;
		return error;
	}
	return GL_NO_ERROR;
}

GLenum sw_glDrainErrors( const char *stage, GLuint texture )
{
	GLenum first = GL_NO_ERROR;
	GLenum error;
	while ( ( error = sw_glGetError() ) != GL_NO_ERROR ) {
		if ( first == GL_NO_ERROR ) first = error;
		sw_say( "gles1shim: %s glid=%u err=0x%x oom=%d\n",
			stage, (unsigned)texture, (unsigned)error, (int)sw_oom );
	}
	return first;
}

GLboolean sw_glOutOfMemory( void )
{
	return sw_oom;
}

GLboolean sw_glTextureReady( GLuint texture, GLsizei width, GLsizei height )
{
	sw_texture_storage *s = sw_find_texture_storage( texture, 0 );
	return s != NULL && s->allocated && width > 0 && height > 0 &&
		s->width == width && s->height == height && s->internalformat == GL_RGBA &&
		s->format == GL_RGBA && s->type == GL_UNSIGNED_BYTE;
}

/*	ES2 requires CLAMP_TO_EDGE for non-power-of-two textures: with the default
	GL_REPEAT wrap such a texture is "incomplete" and samples as black, while
	desktop GL (and GLES1) happily allows it.  Elona/HSP 2D assets are NPOT, so
	this is not a corner case.											*/
static void sw_fix_npot_wrap( GLsizei width, GLsizei height )
{
	if ( sw_is_pot( width ) && sw_is_pot( height ) ) return;
	gl_texparameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	gl_texparameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
}

void sw_glTexImage2D( GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height,
					  GLint border, GLenum format, GLenum type, const GLvoid *pixels )
{
	sw_init();
	if ( !sw_ready ) return;
	sw_texture_storage *s = NULL;
	if ( target == GL_TEXTURE_2D && level == 0 ) {
		s = sw_find_texture_storage( sw_bound_tex, 1 );
		if ( s != NULL ) {
			s->allocated = GL_FALSE;
			s->width = width;
			s->height = height;
			s->internalformat = internalformat;
			s->format = format;
			s->type = type;
		}
	}
	sw_glDrainErrors( "teximage-before", sw_bound_tex );
	gl_teximage2d( target, level, internalformat, width, height, border, format, type, pixels );
	GLenum error = sw_glDrainErrors( "teximage-after", sw_bound_tex );
	if ( s != NULL ) s->allocated = error == GL_NO_ERROR && width > 0 && height > 0;
	if ( sw_alloc_report < 96 || error != GL_NO_ERROR || s == NULL ) {
		if ( sw_alloc_report < 96 ) sw_alloc_report++;
		sw_say( "gles1shim: teximage glid=%u target=0x%x level=%d size=%dx%d internal=0x%x format=0x%x type=0x%x tracked=%d allocated=%d err=0x%x oom=%d\n",
			(unsigned)sw_bound_tex, (unsigned)target, (int)level, (int)width, (int)height,
			(unsigned)internalformat, (unsigned)format, (unsigned)type, s != NULL,
			s != NULL && s->allocated, (unsigned)error, (int)sw_oom );
	}
	if ( target == GL_TEXTURE_2D && level == 0 && error == GL_NO_ERROR ) {
		sw_fix_npot_wrap( width, height );
		sw_glDrainErrors( "teximage-wrap", sw_bound_tex );
	}
}

void sw_glTexSubImage2D( GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height,
						 GLenum format, GLenum type, const GLvoid *pixels )
{
	sw_init();
	if ( sw_ready ) gl_texsubimage2d( target, level, xoffset, yoffset, width, height, format, type, pixels );
}
void sw_glCopyTexImage2D( GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border )
{
	static int reported = 0;

	sw_init();
	if ( !sw_ready ) return;
	if ( gl_copyteximage2d == NULL ) {
		if ( !reported ) {
			reported = 1;
			sw_say( "gles1shim: glCopyTexImage2D not resolvable - window copy cannot happen\n" );
		}
		return;
	}
	gl_copyteximage2d( target, level, internalformat, x, y, width, height, border );
	if ( !reported ) {
		reported = 1;
		sw_say( "gles1shim: glCopyTexImage2D %dx%d err=0x%x\n", (int)width, (int)height, (unsigned)sw_glGetError() );
	}
}

int sw_glCopyTexImage2DAvailable( void )
{
	sw_init();
	return ( sw_ready && gl_copyteximage2d != NULL ) ? 1 : 0;
}