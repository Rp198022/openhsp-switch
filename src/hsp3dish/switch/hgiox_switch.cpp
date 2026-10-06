//
//		Draw lib (iOS/android/opengl/ndk)
//			onion software/onitama 2011/11
//


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <math.h>
#include <string.h>

#include "../../hsp3/hsp3config.h"

#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
#include <unistd.h>
#endif

#ifdef HSPWIN
#define STRICT
#include <windows.h>
#endif

#ifdef HSPNDK
#define USE_JAVA_FONT
#define FONT_TEX_SX 512
#define FONT_TEX_SY 128
#include "../../appengine.h"
#include "../../javafunc.h"
#include "font_data.h"
#endif

#ifdef HSPIOS
//#include <OpenGLES/EAGL.h>
#include <OpenGLES/ES1/gl.h>
#include <OpenGLES/ES1/glext.h>
#include <CoreFoundation/CoreFoundation.h>
#include "iOSBridge.h"
#include "hsp3dish/ios/appengine.h"
#endif


#if defined(HSPLINUX)
#include <SDL2/SDL_ttf.h>
#define USE_TTFFONT
#define USE_JAVA_FONT
#define FONT_TEX_SX 512
#define FONT_TEX_SY 128
//#include "font_data.h"
#endif

#if defined(HSPEMSCRIPTEN)
#include <emscripten.h>
//#ifdef HSPDISHGP
//#include <SDL2/SDL_ttf.h>
//#define USE_TTFFONT
//#endif
#define USE_JAVA_FONT
#define FONT_TEX_SX 512
#define FONT_TEX_SY 128
#endif

#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
#ifdef HSPRASPBIAN
#include "bcm_host.h"
#include "GLES/gl.h"
#include "EGL/egl.h"
#include "EGL/eglext.h"
#include "SDL2/SDL.h"


#else

//#include <GLES2/gl2.h>
//#include <GLES2/gl2ext.h>
//#include <EGL/egl.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

//#include <GL/glut.h>

#ifdef HSPEMSCRIPTEN
#include "SDL2/SDL.h"
#include "SDL2/SDL_image.h"
#include "SDL2/SDL_opengl.h"
#else
#include "SDL2/SDL.h"
#include "SDL2/SDL_image.h"
#include "SDL2/SDL_opengl.h"
#endif

#endif

#ifdef USE_TTFFONT
#include <SDL2/SDL_ttf.h>
#define TTF_FONTFILE "/ipaexg.ttf"
#endif


#include "../emscripten/appengine.h"
extern bool get_key_state(int sym);
extern SDL_Window *window;
#endif

#include "switch_input.h"
#include "switch_overlay.h"

/*	The pad reaches getkey/stick through two tables: the Linux glue's keys[],
	which only the synthetic SDL key events can fill, and the pad's own table
	inside switch_input.cpp, which is written straight from the controller once
	per frame.  Reading both keeps the pad alive even when the events never make
	it through the event loop.											*/
static bool sw_input_key( int scancode )
{
	return get_key_state( scancode ) || ( switch_input_key_state( scancode ) != 0 );
}

#include "../supio.h"
#include "../sysreq.h"
#include "../hgio.h"
#include "../hsp3ext.h"

#include "../texmes.h"

static		HSPREAL infoval[GINFO_EXINFO_MAX];

/*-------------------------------------------------------------------------------*/

#define CIRCLE_DIV 32
#define DEFAULT_FONT_NAME ""
#define DEFAULT_FONT_SIZE 14
#define DEFAULT_FONT_STYLE 0

//色
typedef struct{
    GLfloat r;
    GLfloat g;
    GLfloat b;
    GLfloat a;
} Color;

static float linebasex, linebasey;


//テクスチャ頂点情報
static GLfloat panelVertices[8]={
     0,  0, //左上
     0, -1, //左下
     1,  0, //右上
     1, -1, //右下
};

//テクスチャUV情報
static const GLfloat panelUVs[8]={
    0.0f, 0.0f, //左上
    0.0f, 1.0f, //左下
    1.0f, 0.0f, //右上
    1.0f, 1.0f, //右下
};


#define FVAL_BYTE1 (1.0f/255.0f)
#define RGBA2A(col) (FVAL_BYTE1 * ((col>>24)&0xff))
#define RGBA2R(col) (FVAL_BYTE1 * ((col>>16)&0xff))
#define RGBA2G(col) (FVAL_BYTE1 * ((col>> 8)&0xff))
#define RGBA2B(col) (FVAL_BYTE1 * ((col    )&0xff))

//	グラフィックス設定
static int _bgsx, _bgsy;	//背景サイズ
static int _sizex, _sizey;	//初期サイズ
static Color _color;   	//色
static int   _flipMode;	//フリップ
static int   _originX; 	//原点X
static int   _originY; 	//原点Y
static GLint  _filter;	//フィルタ
static float center_x, center_y;
static float _scaleX;	// スケールX
static float _scaleY;	// スケールY
static float _rateX;	// 1/スケールX
static float _rateY;	// 1/スケールY
static int _uvfix;		// UVFix

static int		drawflag;
static engine	*appengine;
static BMSCR    *mainbm = NULL;

static		BMSCR *backbm;		// 背景消去用のBMSCR(null=NC)

static int		mouse_x;
static int		mouse_y;
static int		mouse_btn;

static	int  font_texid;
static	int  font_sx, font_sy;
static	int  mes_sx, mes_sy;
static	int  font_size;
static	int  font_style;

static		texmesManager tmes;	// テキストメッセージマネージャー

static GLfloat _line_colors[8];
static GLfloat _panelColorsTex[16];

#ifdef HSPIOS
static	double  total_tick;
static	CFAbsoluteTime  lastTime;
#endif

static		MATRIX mat_proj;	// プロジェクションマトリクス
static		MATRIX mat_unproj;	// プロジェクション逆変換マトリクス

/*------------------------------------------------------------*/
/*
		Polygon Draw Routines
*/
/*------------------------------------------------------------*/

//テクスチャ頂点情報
static GLfloat vertf2D[8]={
    0,  0, //左上
    0, -1, //左下
    1,  0, //右上
    1, -1, //右下
};

//テクスチャUV情報
static GLfloat uvf2D[8]={
    0.0f, 0.0f, //左上
    0.0f, 1.0f, //左下
    1.0f, 0.0f, //右上
    1.0f, 1.0f, //右下
};

#if defined(HSPEMSCRIPTEN)
static void gluPerspective(double fovy, double aspect, double zNear, double zFar) {
    GLfloat xmin, xmax, ymin, ymax;
    ymax = zNear * tan(fovy * M_PI / 360.0);
    ymin = -ymax;
    xmin = ymin * aspect;
    xmax = ymax * aspect;
    glFrustum(xmin, xmax, ymin, ymax, zNear, zFar);
}
#endif

/*-------------------------------------------------------------------------------*/
/*
		Offscreen render target support (Switch)

		Upstream hsp3dish backends refuse to draw into a `buffer` screen: every
		draw entry point bails out with HSPERR_UNSUPPORTED_FUNCTION unless the
		target is HSPWND_TYPE_MAIN (or HSPWND_TYPE_OFFSCREEN, in the gameplay
		backend).  The classic GDI runtime (src/hsp3/win32gui) does implement
		`buffer`, and Elona+ was written against that runtime - so a real port
		has to provide offscreen drawing targets here.

		Each drawable screen is backed by a texture (allocated through the usual
		hgio_buffer()/hgio_texload() path) plus a framebuffer object.  The draw
		commands keep working unchanged because they only bind the *source*
		texture; the render target is whatever FBO is bound when they run.

		sw_cur tracks the screen the bound target belongs to (NULL = window).
*/
/*-------------------------------------------------------------------------------*/

/*	Switch diagnostics.  0 = shipping build: the probes, the traces and
	the 1 ms watchdog thread are compiled out.  Build with
	-DSWITCH_DIAG=1 (makefile.switch) when a run needs them back.		*/
#ifndef SWITCH_DIAG
#define SWITCH_DIAG 1			/* P3: on while the fcgraph colour pass is traced */
#endif

/*	The three heavy diagnostics: the SD-card copy of every log line, the
	draw trace and the framebuffer dumps.  All three cost real time on
	hardware - a dump stalls a frame for seconds, the card copy is an
	open/write/close per line, and the trace writes tens of megabytes -
	so they are off unless a capture explicitly asks for them.  The plain
	stdout log stays on: it costs nothing and keeps a run readable.		*/
#ifndef SW_FBO_HEAVY
#define SW_FBO_HEAVY 0
#endif

#define SWTARGET_MAX 64

typedef struct {
	BMSCR	*bm;			// owning screen: texid alone is recycled by GetNextTex()
	int		texid;
	GLuint	fbo;
	int		owned;			// 0 = another entry owns this framebuffer
} SWTARGET;

static SWTARGET	sw_targets[SWTARGET_MAX];

/*	P3 diagnostic: the real GL framebuffer binding, mirrored so the draw trace
	can record what was actually bound rather than what the code believes.	*/
static SWTARGET *sw_find( BMSCR *bm );
static GLuint	sw_real_fbo = 0;
static void sw_bfb( GLuint fbo )
{
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	sw_real_fbo = fbo;
}

/*	P3 - a screen for the window that outlives the frame.
	hsp3dish draws the main screen straight into the window, which is double
	buffered: a one-off paint such as Elona's title background lands in one
	buffer and never reaches the other, so every swap alternates between the
	menu and an empty frame.  The main screen gets a framebuffer of its own and
	is drawn over the window once a frame, so both buffers receive the same
	picture and the content survives between frames.						*/
static GLuint	sw_main_tex = 0;
static GLuint	sw_main_fbo = 0;
static int	sw_main_ok = 0;

static int		sw_target_used = 0;
static BMSCR	*sw_cur = NULL;		// screen currently serving as render target
static BMSCR	*sw_colorbm = NULL;	/* screen whose color/gmode is current */
static int	sw_gsel_report = 0;	/* one-build diagnostic */
static int	sw_texload_report = 0;	/* one-build diagnostic */
static int	sw_copy_report = 0;	/* one-build diagnostic */
static int	sw_dumped = 0;	/* one-build dump */
#define SW_COPY_SIG_MAX 512
static unsigned int	sw_copy_sig[SW_COPY_SIG_MAX];
static int	sw_copy_sig_used = 0;
static int	sw_fbo_report = 0;
static int		sw_fbo_fail = 0;
static int		sw_attach_report = 0;	// P3 diagnostic
static int		sw_buffer_report = 0;	// P3 diagnostic
static int		sw_del_report = 0;		// P3 diagnostic
static int		sw_clear_report = 0;		// P3 diagnostic
static int		sw_copy_skip_report = 0;	// t23 probe: copies dropped for a missing source
static int		sw_copy_big_report = 0;	// t23 probe: picture-buffer restores seen
static int		sw_panel_a = 0;	// t23 probe: window caught right after the sheet copy
static int		sw_panel_b = 0;	// t23 probe: ...and again when that frame is presented
/*	Elona's second screen (`screen 20`, 800x190).  The classic runtime gives it
	a window of its own; this port gives it an offscreen framebuffer instead, and
	sw_main_overlay() draws it back along the bottom of the main screen.  NULL
	until the script asks for such a screen.									*/
static BMSCR	*sw_helpbm = NULL;

static void sw_fbo_log( const char *fmt, ... )
{
	va_list ap;
	char buf[512];
	FILE *fp;

	va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );

	fputs( buf, stdout );
	fflush( stdout );			// nxlink socket output is fully buffered

	/*	Keep a copy on the card as well.  The socket only exists when the
		program is started from the netloader, and the runs that matter are
		the ordinary ones started from the menu.						*/
#if SW_FBO_HEAVY
	fp = fopen( "hsp3dish_diag.log", "ab" );
	if ( fp != NULL ) {
		fputs( buf, fp );
		fclose( fp );
	}
#endif
}

/*	P3 draw trace, one-build diagnostic.  Every primitive that can paint
	pixels records its destination rectangle and the blend parameters in
	force, so the composition of a single frame can be replayed offline.
	Font paths are left out on purpose - they would swamp the budget.		*/
/*	t23: 20000 records stop before the name prompt; a whole run needs more.
	80000 then filled up during the opening scene on 5x5 floor tiles alone
	(the message board repaints every frame), so the budget is 150000 with
	anything 8x8 or smaller dropped - the tiles are noise, the 32x32 map
	copies and the big composites are what a screen bug needs. */
#define SW_TRC_MAX   150000
static FILE	*sw_trc_fp = NULL;
static int	sw_trc_total = 0;

/*	P3 diagnostic: the framebuffer the destination screen is supposed to be
	rendering into, for comparison against the mirrored real binding.		*/
static unsigned sw_exp_fbo( const BMSCR *bm )
{
	SWTARGET *t;
	if ( bm == NULL ) return 0u;
	if ( bm->type == HSPWND_TYPE_MAIN ) {
		return ( sw_main_ok == 1 ) ? (unsigned)sw_main_fbo : 0u;
	}
	t = sw_find( (BMSCR *)bm );
	return ( t != NULL ) ? (unsigned)t->fbo : 0u;
}

static void sw_trc_ex( const char *tag, const BMSCR *bm, float x, float y, float w, float h,
	int srctx, int sx, int sy, int sw_, int sh_ )
{
	if ( !SW_FBO_HEAVY ) return;
	char buf[224];
	/*	5x5 message-board tiles are rebuilt every frame and ate the whole
		budget before the world stage; drop anything that small. */
	if ( w <= 8.0f && h <= 8.0f ) return;
	if ( sw_trc_total >= SW_TRC_MAX ) return;
	if ( sw_trc_fp == NULL ) sw_trc_fp = fopen( "hsp3dish_trace.log", "wb" );
	if ( sw_trc_fp == NULL ) return;
	sw_trc_total++;
	snprintf( buf, sizeof( buf ),
		"hgio: TRC %05d %-8s dst x=%g y=%g w=%g h=%g tx=%d"
		" | src tx=%d %d,%d %dx%d | gm=%d rt=%d col=%06x fbo=%u exp=%u c=%d\n",
		sw_trc_total, tag, (double)x, (double)y, (double)w, (double)h,
		( bm != NULL ) ? bm->texid : -9, srctx, sx, sy, sw_, sh_,
		( bm != NULL ) ? bm->gmode : 0, ( bm != NULL ) ? bm->gfrate : 0,
		( bm != NULL ) ? ( bm->color & 0xffffff ) : 0,
		( unsigned )sw_real_fbo, sw_exp_fbo( bm ),
		( ( bm != NULL ) && ( sw_cur == bm ) ) ? 1 : 0 );
	fputs( buf, sw_trc_fp );
	if ( ( sw_trc_total % 200 ) == 0 ) fflush( sw_trc_fp );
}

static void sw_trc( const char *tag, const BMSCR *bm, float x, float y, float w, float h )
{
	sw_trc_ex( tag, bm, x, y, w, h, -9, 0, 0, 0, 0 );
}

static SWTARGET *sw_find( BMSCR *bm )
{
	int i;
	if ( bm == NULL ) return NULL;
	for ( i = 0; i < sw_target_used; i++ ) {
		if ( ( sw_targets[i].bm == bm ) && ( sw_targets[i].texid == bm->texid ) )
			return &sw_targets[i];
	}
	return NULL;
}

static void sw_forget( BMSCR *bm )
{
	SWTARGET *t;
	GLuint fbo;

	if ( bm == NULL ) return;
	t = sw_find( bm );
	if ( t == NULL ) return;

	fbo = t->fbo;
	if ( sw_cur == bm ) {
		sw_bfb(  0 );
		sw_cur = NULL;
	}
	if ( t->owned ) glDeleteFramebuffers( 1, &fbo );

	sw_target_used--;
	if ( t != &sw_targets[sw_target_used] ) {
		*t = sw_targets[sw_target_used];
	}
	sw_targets[sw_target_used].bm = NULL;
	sw_targets[sw_target_used].texid = -1;
	sw_targets[sw_target_used].fbo = 0;
}

static void sw_forget_all( void )
{
	sw_main_tex = 0;
	sw_main_fbo = 0;
	sw_main_ok = 0;
	sw_target_used = 0;
	sw_cur = NULL;
	sw_bfb(  0 );
}

static int sw_is_window( void )
{
	return ( sw_cur == NULL ) || ( sw_cur->type == HSPWND_TYPE_MAIN );
}

static void sw_apply_target( BMSCR *bm )
{
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();

	if ( ( bm != NULL ) && ( bm->type != HSPWND_TYPE_MAIN ) ) {
		//	オフスクリーン: テクスチャ全体に1:1で描画する
		float ox = (float)bm->sx;
		float oy = (float)bm->sy;
#if defined(HSPRASPBIAN) || defined(HSPNDK) || defined(HSPIOS)
		/*
			Screen y=0 has to land at texture v=0 - the same row a loaded image
			keeps its top in - because hgio_copy() reads a source back with
			v = y / height.  The mirrored projection put a draw at screen row y at
			v = 1 - y/h, so everything Elona composed into a picture buffer came back
			upside down, and the whole-screen gcopy Elona uses to lay the title
			screen down mirrored it a second time.		*/
		glOrthof( 0, ox, 0, -oy, -100, 100 );
#else
		glOrtho( 0, ox, 0, -oy, -100, 100 );
#endif
		glViewport( 0, 0, bm->sx, bm->sy );
	} else {
		/*	Main screen: render 1:1 into its own _bgsx x _bgsy texture.  The
			upscale to the panel happens at present time (sw_apply_window), not
			here.  The window texture doubles as a gcopy source, so
			sw_tex_src() has to hand out 1/800 by 1/600 for those reads to land
			on the region the script asked for - scaling the main target as
			well made every copy that sampled the window read only the left
			62.5% of it.  Row 0 stays at v=1, which hgio_copy() compensates
			for.																*/
		float ox = (float)_bgsx;
		float oy = (float)_bgsy;
#if defined(HSPRASPBIAN) || defined(HSPNDK) || defined(HSPIOS)
		glOrthof( 0, ox, -oy, 0, -100, 100 );
#else
		glOrtho( 0, ox, -oy, 0, -100, 100 );
#endif
		glViewport( 0, 0, (int)ox, (int)oy );
	}

	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	if ( bm != NULL ) hgio_setview( bm );
}

/*	Present the finished frame: the _bgsx x _bgsy main texture centred and
	scaled up over the panel.  Only the window pass knows about
	_originX/_scaleX, so they are set here and left alone by the draw passes
	above; hgio_scale_point() reads them for pointer coordinates.		*/
static void sw_apply_window( void )
{
	float ox = (float)_bgsx;
	float oy = (float)_bgsy;

	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
#if defined(HSPRASPBIAN) || defined(HSPNDK) || defined(HSPIOS)
	glOrthof( 0, ox, -oy, 0, -100, 100 );
#else
	glOrtho( 0, ox, -oy, 0, -100, 100 );
#endif

	_rateX = 1.0f / _scaleX;
	_rateY = 1.0f / _scaleY;
	_originX = ( _sizex - (ox * _scaleX) ) / 2;
	_originY = ( _sizey - (oy * _scaleY) ) / 2;
	glViewport( (float)_originX, (float)_originY, ox * _scaleX, oy * _scaleY );

	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();
}

static int sw_ensure( BMSCR *bm )
{
	SWTARGET *t;
	TEXINF *tex;
	GLuint fbo;
	GLenum st;
	GLboolean live;

	if ( bm == NULL ) return -1;
	t = sw_find( bm );
	if ( t != NULL ) return 0;

	if ( bm->texid < 0 ) {
		hgio_buffer( bm );			// 通常のテクスチャ確保パスを使う
		if ( bm->texid < 0 ) return -1;
	}

	tex = GetTex( bm->texid );
	if ( ( tex == NULL ) || ( tex->mode == TEXMODE_NONE ) ) return -1;
	if ( !sw_glTextureReady( (GLuint)tex->texid, tex->sx, tex->sy ) ) {
		sw_fbo_log( "hgio: attach SKIP storage texid=%d glid=%u size=%dx%d oom=%d\n",
			bm->texid, (unsigned)tex->texid, (int)tex->sx, (int)tex->sy,
			(int)sw_glOutOfMemory() );
		return -1;
	}
	sw_glDrainErrors( "attach-before", (GLuint)tex->texid );

	/*	P3 diagnostic.  The Atmosphere crash report for this build points at a
		NULL dereference inside Mesa's st_update_renderbuffer_surface() while
		glFramebufferTexture2D runs - and this is its only caller.  The last
		line printed below therefore names the screen/texture that Mesa chokes
		on.  glIsTexture() separates "dangling name" (TEXINF survives a
		glDeleteTextures) from "live texture, unsupported as a colour buffer"
		(MakeEmptyTex() hands out GL_ALPHA/TEXMODE_MES8 textures).			*/
	live = glIsTexture( (GLuint)tex->texid );
	GLenum live_error = sw_glDrainErrors( "attach-live", (GLuint)tex->texid );
	if ( sw_attach_report < 96 ) {
		sw_attach_report++;
		sw_fbo_log( "hgio: attach bm=%p type=%d texid=%d mode=%d opt=%d sx=%d sy=%d w=%d h=%d glid=%u live=%d err=0x%x used=%d\n",
			(void *)bm, bm->type, bm->texid, (int)tex->mode, (int)tex->opt,
			(int)tex->sx, (int)tex->sy, (int)tex->width, (int)tex->height,
			(unsigned)tex->texid, (int)live, (unsigned)live_error, sw_target_used );
	}
	if ( !live || live_error != GL_NO_ERROR ) {
		sw_fbo_log( "hgio: attach SKIP dead texture texid=%d glid=%u\n",
			bm->texid, (unsigned)tex->texid );
		return -1;
	}
	if ( ( tex->mode != TEXMODE_BUFFER ) && ( tex->mode != TEXMODE_NORMAL ) ) {
		sw_fbo_log( "hgio: attach SKIP mode=%d texid=%d glid=%u\n",
			(int)tex->mode, bm->texid, (unsigned)tex->texid );
		return -1;
	}

	t = sw_find( bm );
	if ( t != NULL ) return 0;
	/*	A texture can already be someone else's render target: GetNextTex()
		hands out any slot whose mode went back to TEXMODE_NONE, so a screen
		that is still alive may be pointing at a texture another screen now
		owns.  Two framebuffers on one texture is not something GL can serve,
		and the draws into the second one do not land. */
	{
		int k;
		for ( k = 0; k < sw_target_used; k++ ) {
			if ( sw_targets[k].texid == bm->texid ) {
				if ( sw_target_used >= SWTARGET_MAX ) return -1;
				t = &sw_targets[sw_target_used++];
				t->bm = bm;
				t->texid = bm->texid;
				t->fbo = sw_targets[k].fbo;
				t->owned = 0;
				if ( sw_attach_report < 96 ) {
					sw_attach_report++;
					sw_fbo_log( "hgio: share target texid=%d bm=%p -> fbo %u\n",
						bm->texid, (void *)bm, (unsigned)t->fbo );
				}
				return 0;
			}
		}
	}
	if ( sw_target_used >= SWTARGET_MAX ) return -1;
	fbo = 0;
	glGenFramebuffers( 1, &fbo );
	GLenum gen_error = sw_glDrainErrors( "attach-gen", (GLuint)tex->texid );
	if ( fbo == 0 || gen_error != GL_NO_ERROR ) {
		if ( fbo != 0 ) glDeleteFramebuffers( 1, &fbo );
		return -1;
	}
	sw_bfb(  fbo );
	if ( sw_glDrainErrors( "attach-setup", (GLuint)tex->texid ) != GL_NO_ERROR ) {
		sw_bfb(  0 );
		glDeleteFramebuffers( 1, &fbo );
		return -1;
	}
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, (GLuint)tex->texid, 0 );
	GLenum attach_error = sw_glDrainErrors( "attach-after", (GLuint)tex->texid );
	st = attach_error == GL_NO_ERROR
		? glCheckFramebufferStatus( GL_FRAMEBUFFER ) : GL_FRAMEBUFFER_UNSUPPORTED;
	GLenum status_error = sw_glDrainErrors( "attach-status", (GLuint)tex->texid );
	sw_fbo_log( "hgio: attach result glid=%u fbo=%u err=0x%x status=0x%x status_err=0x%x oom=%d\n",
		(unsigned)tex->texid, (unsigned)fbo, (unsigned)attach_error,
		(unsigned)st, (unsigned)status_error, (int)sw_glOutOfMemory() );
	sw_bfb(  0 );

	if ( st != GL_FRAMEBUFFER_COMPLETE || attach_error != GL_NO_ERROR ||
		status_error != GL_NO_ERROR ) {
		glDeleteFramebuffers( 1, &fbo );
		if ( sw_fbo_fail < 8 ) {
			sw_fbo_fail++;
			sw_fbo_log( "hgio: FBO incomplete(0x%x) texid=%d tex=%dx%d\n",
				(unsigned)st, bm->texid, (int)tex->width, (int)tex->height );
		}
		return -1;
	}

	t = &sw_targets[sw_target_used++];
	t->bm = bm;
	t->texid = bm->texid;
	t->fbo = fbo;
	t->owned = 1;

	if ( sw_fbo_report < 48 ) {
		sw_fbo_report++;
		sw_fbo_log( "hgio: offscreen target texid=%d (%dx%d) -> fbo %u (tex %dx%d)\n",
			bm->texid, bm->sx, bm->sy, (unsigned)fbo, (int)tex->width, (int)tex->height );
	}
	return 0;
}

static int sw_bind_target( BMSCR *bm );	/* defined below, re-binds here */

static int sw_drawable( BMSCR *bm )
{
	if ( bm == NULL ) return 0;
	if ( bm->type == HSPWND_TYPE_MAIN ) {
		if ( sw_cur != bm ) sw_bind_target( bm );
		return 1;
	}
	if ( bm->type == HSPWND_TYPE_NONE ) return 0;
	if ( sw_ensure( bm ) != 0 ) return 0;
	/*	A draw command names the screen it draws into, so it has to make that
		screen the render target.  This used to be left to gsel() alone, but a
		screen's framebuffer is keyed on the screen and dropped whenever its
		texture is recreated (buffer/picload), which left the window bound and
		made the drawing land outside the screen it was meant for.			*/
	if ( sw_cur != bm ) sw_bind_target( bm );
	return 1;
}

static void sw_main_ensure( void )
{
	int w = (int)_bgsx;
	int h = (int)_bgsy;

	if ( sw_main_ok != 0 ) return;
	if ( w <= 0 || h <= 0 || w > 1920 || h > 1080 ) { sw_main_ok = -1; return; }

	glGenTextures( 1, &sw_main_tex );
	glGenFramebuffers( 1, &sw_main_fbo );
	if ( sw_main_tex == 0 || sw_main_fbo == 0 ) { sw_main_ok = -1; return; }

	glBindTexture( GL_TEXTURE_2D, sw_main_tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );

	sw_bfb(  sw_main_fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sw_main_tex, 0 );
	if ( ( sw_glGetError() != GL_NO_ERROR ) ||
		 ( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE ) ) {
		sw_main_ok = -1;
		sw_fbo_log( "hgio: no target for the main screen, %dx%d; using the window\n", w, h );
		sw_bfb(  0 );
		sw_cur = NULL;
		return;
	}
	hgio_clear();
	sw_main_ok = 1;
}

/*	Draw the main screen over the window.  The frame was rendered with the
	window's matrices, so only the target changes; the texture is sampled over
	the viewport rectangle it was drawn into.

	Every client array this needs is set here and the colour array is turned
	off explicitly.  The shader picks up whatever the last draw left enabled,
	and a stale colour pointer is read as vertices and colours - which is what
	turned this copy into a red diagonal the first time round.				*/
/*	One-build diagnostic: read a target back in BGRA and write it as a
	32-bit BMP (bottom-up, which is the order glReadPixels hands back).	*/
/*	Blits that sample the texture they are drawing into are undefined in GL,
	and the classic runtime Elona targets performs them as ordinary memory
	copies, so Elona relies on them.  Capture the rectangle 1:1 into a
	scratch texture and draw from that instead.								*/
#define SW_SCRATCH_MAX 1024
static int	sw_selfblit_log = 0;
static int	sw_scratch_dumped = 0;
static void sw_dump_fbo( const char *name, GLuint fbo, int w, int h );
static GLuint	sw_scratch_tex = 0;
static GLuint	sw_scratch_fbo = 0;
static int	sw_scratch_w = 0;
static int	sw_scratch_h = 0;
static int	sw_scratch_ok = 0;
static int	sw_scratch_used = 0;

/*	ChangeTex() caches the last GL name it bound and skips a redundant
	bind.  Several paths here bind a texture behind that cache's back - the
	raw glBindTexture in sw_main_ensure() below, and the shared upload code
	(MakeEmptyTex/MakeEmptyTexBuffer/UpdateTex32) which binds directly - so
	the cache can be left naming a texture that is no longer bound.  The
	next ChangeTex() of that name is then skipped and the draw samples
	whatever happens to be bound: the shape stays right and the colours go,
	which is what turned the title panel black on some runs.				*/
static void sw_bind_tex( int id )
{
	TexReset();
	ChangeTex( id );
}

/*	The window (main screen) has no TEXINF entry - its texture lives in
	sw_main_tex - so GetTex( bmsrc->texid ) with texid -1 read before the
	table and handed back heap garbage: a random mode, rate and GL name.
	Describe the window texture instead.								*/
static TEXINF *sw_tex_src( BMSCR *bmsrc, TEXINF *scratch )
{
	if ( bmsrc == NULL ) return NULL;
	/*	t23: only the window may sit at texid -1.  A screen that has been
		deleted keeps -1 as well, and GetTex() does not range-check: GetTex(-1)
		reads the slot before the table and hands back a random mode, size and
		GL name, which the copy then drew as a garbage tile over the screen.	*/
	if ( ( bmsrc->texid < 0 ) && ( bmsrc->type != HSPWND_TYPE_MAIN ) ) return NULL;
	if ( ( bmsrc->texid < 0 ) && ( bmsrc->type == HSPWND_TYPE_MAIN ) ) {
		if ( sw_main_ok != 1 ) return NULL;
		memset( scratch, 0, sizeof( TEXINF ) );
		scratch->mode = TEXMODE_NORMAL;
		scratch->sx = (short)_bgsx;
		scratch->sy = (short)_bgsy;
		scratch->width = (short)_bgsx;
		scratch->height = (short)_bgsy;
		scratch->texid = (int)sw_main_tex;
		scratch->ratex = ( _bgsx > 0 ) ? 1.0f / (float)_bgsx : 0.0f;
		scratch->ratey = ( _bgsy > 0 ) ? 1.0f / (float)_bgsy : 0.0f;
		return scratch;
	}
	return GetTex( bmsrc->texid );
}


static int sw_scratch_ensure( int w, int h )
{
	int nw = 32, nh = 32;

	if ( w <= 0 || h <= 0 || w > SW_SCRATCH_MAX || h > SW_SCRATCH_MAX ) return -1;
	while ( nw < w ) nw <<= 1;
	while ( nh < h ) nh <<= 1;
	if ( ( sw_scratch_ok == 1 ) && ( nw <= sw_scratch_w ) && ( nh <= sw_scratch_h ) ) return 0;

	if ( sw_scratch_tex != 0 ) glDeleteTextures( 1, &sw_scratch_tex );
	if ( sw_scratch_fbo != 0 ) glDeleteFramebuffers( 1, &sw_scratch_fbo );
	sw_scratch_tex = 0; sw_scratch_fbo = 0; sw_scratch_ok = 0;

	glGenTextures( 1, &sw_scratch_tex );
	glGenFramebuffers( 1, &sw_scratch_fbo );
	if ( sw_scratch_tex == 0 || sw_scratch_fbo == 0 ) return -1;

	/*	This bind bypasses ChangeTex(), so that cache still names whatever was
		bound before while the scratch is now the texture in force.  The next
		thing sw_scratch_capture() does is ChangeTex( srctex ), and when srctex
		is the texture ChangeTex already believes is current it skips the bind -
		leaving the scratch bound *and* sampled, so the capture reads and writes
		one texture at once and the tile it lands is uninitialised memory.
		Drop the cache so the bind really happens.							*/
	glBindTexture( GL_TEXTURE_2D, sw_scratch_tex );
	TexReset();
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, nw, nh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	sw_bfb(  sw_scratch_fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sw_scratch_tex, 0 );
	if ( ( sw_glGetError() != GL_NO_ERROR ) ||
		 ( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE ) ) {
		sw_fbo_log( "hgio: no scratch target %dx%d\n", nw, nh );
		sw_bfb(  0 );
		return -1;
	}
	sw_scratch_w = nw;
	sw_scratch_h = nh;
	sw_scratch_ok = 1;
	return 0;
}

/*	r153: stage a self-copy by reading the live framebuffer back.
	Sampling a texture does not work for the main screen (r150 proved it:
	the band went blank), but glReadPixels always sees what is on screen.
	Both glReadPixels output and the scratch texture are bottom-up, so the
	pixels go in as-is and match the UVs the staging path already uses.  */
/*	r155: write a raw RGBA buffer as a 32-bit BMP (bottom-up, no flip). */
static void sw_write_bmp_rgba( const char *name, const unsigned char *data,
							   int w, int h )
{
	FILE *fp;
	unsigned char hdr[54];
	int i, rowsz, filesize;

	if ( w <= 0 || h <= 0 || data == NULL ) return;
	rowsz = w * 4;
	filesize = 54 + rowsz * h;
	for ( i = 0; i < 54; i++ ) hdr[i] = 0;
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2] = (unsigned char)( filesize & 0xff );
	hdr[3] = (unsigned char)( ( filesize >> 8 ) & 0xff );
	hdr[4] = (unsigned char)( ( filesize >> 16 ) & 0xff );
	hdr[5] = (unsigned char)( ( filesize >> 24 ) & 0xff );
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (unsigned char)( w & 0xff );
	hdr[19] = (unsigned char)( ( w >> 8 ) & 0xff );
	hdr[22] = (unsigned char)( h & 0xff );
	hdr[23] = (unsigned char)( ( h >> 8 ) & 0xff );
	hdr[26] = 1;
	hdr[28] = 32;
	fp = fopen( name, "wb" );
	if ( fp == NULL ) return;
	fwrite( hdr, 1, 54, fp );
	fwrite( data, 1, (size_t)rowsz * h, fp );
	fclose( fp );
}

static int sw_scratch_readback( int xx, int yy, int w, int h, BMSCR *bm )
{
	unsigned char *buf;
	SWTARGET *st;
	GLuint fbo;

	if ( w <= 0 || h <= 0 ) return -1;
	if ( sw_scratch_ensure( w, h ) != 0 ) return -1;

	buf = (unsigned char *)mem_ini( w * h * 4 );
	if ( buf == NULL ) return -1;

	st = sw_find( bm );
	/*	r154: the main screen is not in the target list, so sw_find()
		returns NULL for it and fbo 0 (the 1280x720 window buffer) was
		read instead - the band came out as garbage.  Use the main FBO,
		the same one sw_dump_fbo() reads when it exports the screen.	*/
	if ( st != NULL ) {
		fbo = st->fbo;
	} else if ( ( bm->type == HSPWND_TYPE_MAIN ) && ( sw_main_ok == 1 ) ) {
		fbo = sw_main_fbo;
	} else {
		fbo = 0;
	}
	{
		static int sw_main_rb_log = 0;
		if ( sw_main_rb_log < 40 ) {
			sw_main_rb_log++;
			printf( "t52 readback fbo=%u main_ok=%d st=%d %dx%d at %d,%d\n",
				(unsigned)fbo, sw_main_ok, ( st != NULL ) ? 1 : 0, w, h, xx, yy );
			fflush( stdout );
		}
	}
	sw_bfb(  fbo );
	/*	No glPixelStorei here: the GLES1 shim does not provide it and the
		default pack alignment of 4 is already right for RGBA bytes.	*/
	glReadPixels( xx, yy, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf );

	{
		/*	r155: what did glReadPixels actually give us, and did it make
			it into the scratch texture?								*/
		static int sw_rb_dumped = 0;
		if ( sw_rb_dumped < 3 ) {
			char nm[48];
			sw_rb_dumped++;
			snprintf( nm, sizeof( nm ), "sdmc:/dump/rb%d_raw.bmp", sw_rb_dumped );
			sw_write_bmp_rgba( nm, buf, w, h );
			printf( "t53 dumped %s %dx%d from fbo=%u at %d,%d\n",
				nm, w, h, (unsigned)fbo, xx, yy );
			fflush( stdout );
		}
	}
	ChangeTex( -1 );
	glBindTexture( GL_TEXTURE_2D, sw_scratch_tex );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, w, h,
		GL_RGBA, GL_UNSIGNED_BYTE, buf );
	ChangeTex( -1 );

	mem_bye( buf );

	/*	Leave the target exactly as the caller expects to draw into it. */
	sw_bfb(  fbo );
	if ( st != NULL ) sw_apply_target( bm );
	return 0;
}

static int sw_scratch_capture( GLuint srctex, float ratex, float ratey,
							   int xx, int yy, int w, int h )
{
	GLfloat vert[8];
	GLfloat uv[8];

	if ( sw_scratch_ensure( w, h ) != 0 ) return -1;

	sw_bfb(  sw_scratch_fbo );
	glViewport( 0, 0, sw_scratch_w, sw_scratch_h );
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0, sw_scratch_w, 0, -sw_scratch_h, -100, 100 );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	vert[0] = 0.0f;			vert[1] = 0.0f;
	vert[2] = 0.0f;			vert[3] = (GLfloat)-h;
	vert[4] = (GLfloat)w;	vert[5] = 0.0f;
	vert[6] = (GLfloat)w;	vert[7] = (GLfloat)-h;

	uv[0] = (GLfloat)xx * ratex;		uv[1] = (GLfloat)yy * ratey;
	uv[2] = (GLfloat)xx * ratex;		uv[3] = (GLfloat)( yy + h ) * ratey;
	uv[4] = (GLfloat)( xx + w ) * ratex;	uv[5] = (GLfloat)yy * ratey;
	uv[6] = (GLfloat)( xx + w ) * ratex;	uv[7] = (GLfloat)( yy + h ) * ratey;

	glDisable( GL_BLEND );
	glDisableClientState( GL_COLOR_ARRAY );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, vert );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );
	sw_bind_tex( (int)srctex );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	ChangeTex( -1 );
	if ( ( w >= 256 ) && ( sw_scratch_dumped == 0 ) ) {
		sw_scratch_dumped = 1;
		sw_dump_fbo( "smp_scratch.bmp", sw_scratch_fbo, w, h );
	}
	return 0;
}


#define SW_DUMP_MAX_PIXELS 2000000L


static void sw_dump_fbo( const char *name, GLuint fbo, int w, int h )
{
	if ( !SW_FBO_HEAVY ) return;
	if ( !SWITCH_DIAG ) return;
	unsigned char *p;
	FILE *fp;
	unsigned char hdr[54];
	int i, x, y;
	int rowsz, filesize;

	if ( w <= 0 || h <= 0 ) return;
	if ( (long)w * (long)h > SW_DUMP_MAX_PIXELS ) {
		sw_fbo_log( "hgio: dump %s skipped, %dx%d too big\n", name, w, h );
		return;
	}
	p = (unsigned char *)mem_ini( w * h * 4 );
	if ( p == NULL ) return;
	sw_bfb(  fbo );
	glReadPixels( 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p );

	rowsz = w * 4;
	filesize = 54 + rowsz * h;
	for ( i = 0; i < 54; i++ ) hdr[i] = 0;
	hdr[0] = 'B'; hdr[1] = 'M';
	hdr[2] = (unsigned char)( filesize & 0xff );
	hdr[3] = (unsigned char)( ( filesize >> 8 ) & 0xff );
	hdr[4] = (unsigned char)( ( filesize >> 16 ) & 0xff );
	hdr[5] = (unsigned char)( ( filesize >> 24 ) & 0xff );
	hdr[10] = 54;
	hdr[14] = 40;
	hdr[18] = (unsigned char)( w & 0xff );
	hdr[19] = (unsigned char)( ( w >> 8 ) & 0xff );
	hdr[22] = (unsigned char)( h & 0xff );
	hdr[23] = (unsigned char)( ( h >> 8 ) & 0xff );
	hdr[26] = 1;
	hdr[28] = 32;

	{
		/*	Keep the snapshots out of the game directory: Eden watches it and
			reloads its game list on every file that appears, and the burst of
			readbacks was taking the emulator down. */
		char full[96];
		snprintf( full, sizeof( full ), "sdmc:/dump/%s", name );
		fp = fopen( full, "wb" );
	}
	if ( fp == NULL ) {
		sw_fbo_log( "hgio: dump %s could not be opened\n", name );
		mem_bye( p );
		return;
	}
	fwrite( hdr, 1, 54, fp );
	for ( y = 0; y < h; y++ ) {
		unsigned char *src = p + (size_t)y * rowsz;
		unsigned char px[4];
		for ( x = 0; x < w; x++ ) {
			px[0] = src[x*4+2];
			px[1] = src[x*4+1];
			px[2] = src[x*4+0];
			px[3] = 255;
			fwrite( px, 1, 4, fp );
		}
	}
	fclose( fp );
	mem_bye( p );
	sw_fbo_log( "hgio: dumped %s %dx%d fbo=%u\n", name, w, h, (unsigned)fbo );
}


static void sw_dump_all( const char *pfx )
{
	int i;
	char nm[40];
	snprintf( nm, sizeof( nm ), "%s_main.bmp", pfx );
	sw_dump_fbo( nm, ( sw_main_ok == 1 ) ? sw_main_fbo : 0,
		(int)_bgsx, (int)_bgsy );
	for ( i = 0; i < sw_target_used; i++ ) {
		snprintf( nm, sizeof( nm ), "%s_t%02d.bmp", pfx, (int)sw_targets[i].bm->texid );
		sw_dump_fbo( nm, sw_targets[i].fbo, sw_targets[i].bm->sx, sw_targets[i].bm->sy );
	}
}


/*	Draw the second screen back over the main one, along the bottom.  Its own
	framebuffer is left alone by the main screen's draw commands, so the help
	panel no longer burns into the world map; here it is composited on top of
	the finished frame, once, so both screens are visible where the classic
	runtime put them.

	An offscreen target keeps screen row 0 at texture v=0 - the opposite of the
	window texture, whose row 0 sits at v=1 - so the v range runs the other way
	round from the pass above.												*/
static void sw_main_overlay( void )
{
	SWTARGET *t;
	TEXINF *tex;
	GLfloat vert[8];
	GLfloat uv[8];
	float ox = (float)_bgsx;
	float oy = (float)_bgsy;
	float hy, u1, v1;

	if ( sw_helpbm == NULL ) return;
	if ( sw_helpbm->flag == BMSCR_FLAG_NOUSE ) return;
	t = sw_find( sw_helpbm );
	if ( t == NULL ) return;
	tex = GetTex( sw_helpbm->texid );
	if ( ( tex == NULL ) || ( tex->mode == TEXMODE_NONE ) ) return;

	hy = (float)sw_helpbm->sy;
	if ( ( hy <= 0.0f ) || ( hy > oy ) ) hy = oy;
	u1 = (float)sw_helpbm->sx * tex->ratex;
	v1 = (float)sw_helpbm->sy * tex->ratey;
	if ( u1 <= 0.0f ) u1 = 1.0f;
	if ( v1 <= 0.0f ) v1 = 1.0f;

	vert[0] = 0.0f;	vert[1] = -( oy - hy );
	vert[2] = ox;	vert[3] = -( oy - hy );
	vert[4] = 0.0f;	vert[5] = -oy;
	vert[6] = ox;	vert[7] = -oy;

	uv[0] = 0.0f;	uv[1] = 0.0f;
	uv[2] = u1;		uv[3] = 0.0f;
	uv[4] = 0.0f;	uv[5] = v1;
	uv[6] = u1;		uv[7] = v1;

	glDisableClientState( GL_COLOR_ARRAY );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, vert );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );

	sw_glColorKey( 0, 0 );
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	sw_bind_tex( (int)tex->texid );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	ChangeTex( -1 );
}


unsigned int sw_last_render_tick = 0;	/* tick of the last presented frame	*/
static void sw_main_present( void )
{
	sw_last_render_tick = (unsigned int)hgio_gettick();

/*	P3 diagnostic: the window the port believes it has against the one
	SDL actually opened.  Elona asks for 800x600 (4:3) while the Switch
	panel is 1280x720 (16:9); whichever of the two the viewport is built
	from decides whether the picture is letterboxed or stretched.		*/
#if SWITCH_DIAG
	{
		static int shown = 0;
		if ( !shown && ( hgio_gettick() > 3000 ) ) {
			int ww = 0, wh = 0, dw = 0, dh = 0;
			int mw = 0, mh = 0;
			SDL_DisplayMode dm;
			SDL_Window *win = SDL_GL_GetCurrentWindow();
			shown = 1;
			if ( SDL_GetCurrentDisplayMode( 0, &dm ) == 0 ) { mw = dm.w; mh = dm.h; }
			if ( win != NULL ) {
				SDL_GetWindowSize( win, &ww, &wh );
				SDL_GL_GetDrawableSize( win, &dw, &dh );
			}
			printf( "hsp3switch: SCREEN sizex=%d sizey=%d bgsx=%d bgsy=%d "
				"scale=%.3f,%.3f origin=%d,%d win=%dx%d drawable=%dx%d mode=%dx%d\n",
				(int)_sizex, (int)_sizey, (int)_bgsx, (int)_bgsy,
				(double)_scaleX, (double)_scaleY, _originX, _originY,
				ww, wh, dw, dh, mw, mh );
			fflush( stdout );
		}
	}
#endif
	GLfloat vert[8];
	GLfloat uv[8];
	float ox = (float)_bgsx;
	float oy = (float)_bgsy;
	float u0, v0, u1, v1;

	if ( sw_main_ok != 1 ) return;

	/*	t23 probe: the other half of the sheet-copy frame.  panel_a is the
		window just after the copy; this is the same frame as it leaves.  The
		trace is closed here so it holds exactly those two moments.			*/
	if ( sw_panel_b == 1 ) {
		sw_panel_b = 0;
		sw_dump_fbo( "panel_b.bmp", sw_main_fbo, (int)_bgsx, (int)_bgsy );
		if ( sw_trc_fp != NULL ) fclose( sw_trc_fp );
		sw_trc_fp = NULL;
		sw_trc_total = SW_TRC_MAX;
	}

	if ( !sw_dumped && ( hgio_gettick() > 15000 ) ) {
		sw_dumped = 1;
		sw_dump_all( "smp" );
	}
	/*	t23: the world stage needs its own snapshot - the first dump always
		lands on the title screen.											*/
	if ( ( sw_dumped == 1 ) && ( hgio_gettick() > 120000 ) ) {
		sw_dumped = 2;
		sw_dump_all( "sw" );
	}
	if ( ( sw_dumped == 2 ) && ( hgio_gettick() > 200000 ) ) {
		sw_dumped = 3;
		sw_dump_all( "sw2" );
	}

	/*	The main texture is exactly _bgsx x _bgsy now, so all of it is laid
		over the viewport rectangle.										*/
	u0 = 0.0f;	v0 = 0.0f;
	u1 = 1.0f;	v1 = 1.0f;

	vert[0] = 0.0f;		vert[1] = 0.0f;
	vert[2] = ox;		vert[3] = 0.0f;
	vert[4] = 0.0f;		vert[5] = -oy;
	vert[6] = ox;		vert[7] = -oy;

	uv[0] = u0;	uv[1] = v1;
	uv[2] = u1;	uv[3] = v1;
	uv[4] = u0;	uv[5] = v0;
	uv[6] = u1;	uv[7] = v0;

	sw_bfb(  0 );
	/*	Elona paints only part of the screen (the intro and the character screens
		leave large areas untouched) and the main texture keeps alpha 0 there.  On
		the Switch that never shows - the display is opaque - but in a window the
		untouched areas would be transparent and the desktop would come through,
		which makes an emulator run impossible to judge.  Clearing to opaque black
		is enough: with the usual alpha blend the destination alpha stays 1 where
		the texture is transparent.											*/
	glClearColor( 0.0f, 0.0f, 0.0f, 1.0f );
	glClear( GL_COLOR_BUFFER_BIT );
	/*	The frame may well have ended on an offscreen screen, in which case the
		projection and viewport still belong to that screen and a quad drawn in
		them would land somewhere else on the window.						*/
	sw_apply_window();

	glDisableClientState( GL_COLOR_ARRAY );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, vert );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );

	sw_glColorKey( 0, 0 );
	/*	The frame is complete, so this is a copy and not a blend.  The
		classic runtime blitted the finished screen opaquely; blending here
		pulled the picture towards the black clear wherever a buffer's alpha
		had fallen below 1, which is what made a correctly composed window
		panel vanish. */
	glDisable( GL_BLEND );
	sw_bind_tex( (int)sw_main_tex );
	/*	The window is 1280x720 while the script draws an 800x600 canvas, so
		this one draw scales it up 1.2x to fill the panel's height.  A
		non-integer ratio under GL_NEAREST repeats some source rows and not
		others, which is what made the message text look uneven, so the
		upscale interpolates.  Both parameters go back afterwards: this same
		texture is a gcopy source too, and a 1:1 read wants the exact
		texel.																*/
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	ChangeTex( -1 );

	/*	The frame is on the window; the second screen, if the script asked for
		one, is composited over it.  The backbuffer is not swapped yet, so both
		passes land in the same presented frame.							*/
	/*	Elona's second window (`screen 20`, 800x190 = a third of the main
		screen) is the "basic operation" help panel.  The classic runtime
		gives it a window of its own below the main one; overlaying it here
		buried the bottom third of the game - the message log and the status
		bars - under the panel's black background, which reads as a dead black
		band across the bottom of the map.  The offscreen framebuffer is still
		built (the script's draw commands need a target), but the panel is left
		off the presented frame so the game screen stays whole.				*/
}
static int sw_bind_target( BMSCR *bm )
{
	SWTARGET *t;

	if ( bm == NULL ) return -1;

	if ( bm->type == HSPWND_TYPE_MAIN ) {
		sw_main_ensure();
		sw_bfb(  ( sw_main_ok == 1 ) ? sw_main_fbo : 0 );
		sw_cur = bm;
		sw_apply_target( bm );
		return 0;
	}

	if ( sw_ensure( bm ) != 0 ) {
		if ( sw_fbo_fail <= 8 ) {
			sw_fbo_log( "hgio: NO target for screen type=%d (%dx%d) texid=%d\n",
				bm->type, bm->sx, bm->sy, bm->texid );
		}
		return -1;
	}

	t = sw_find( bm );
	if ( t == NULL ) return -1;
	sw_bfb(  t->fbo );
	sw_cur = bm;
	sw_apply_target( bm );
	return 0;
}

static void sw_unbind_window( void )
{
	sw_bfb(  0 );
	sw_cur = NULL;
}

static void sw_redraw_time( BMSCR *bm )
{
	int curtick = hgio_gettick();
	if ( bm->prevtime ) {
		bm->passed_time = curtick - bm->prevtime;
	}
	bm->prevtime = curtick;
}

/*-------------------------------------------------------------------------------*/
/*
		Draw Service
*/
/*-------------------------------------------------------------------------------*/

void hgio_init( int mode, int sx, int sy, void *hwnd )
{
#ifdef HSPIOS
	gb_init();	//		IOS グラフィック初期化
    gb_reset( sx, sy );
#endif
    
	//テクスチャ初期化
	TexInit();

	//背景サイズ
	_bgsx = sx;
	_bgsy = sy;
	_sizex = sx;
	_sizey = sy;
	_scaleX = 1.0f;
	_scaleY = 1.0f;
	_rateX = 1.0f;
	_rateY = 1.0f;
	_uvfix = 0;

	GeometryInit();

	//グラフィックス設定
	_flipMode = GRAPHICS_FLIP_NONE;
	_originX = 0;
	_originY = 0;
	_filter = GL_NEAREST;
	drawflag = 0;
	mainbm = NULL;
	backbm = NULL;
	appengine = (engine *)hwnd;
	hgio_touch( 0,0,0 );

	//		設定の初期化
	//
	SetSysReq( SYSREQ_RESULT, 0 );
	SetSysReq( SYSREQ_RESVMODE, 0 );
	SetSysReq( SYSREQ_CLSMODE, CLSMODE_SOLID );
	SetSysReq( SYSREQ_CLSCOLOR, 0 );
//	SetSysReq( SYSREQ_CLSCOLOR, 0xffffff );

    //クリア色の設定
	Alertf( "Init:HGIOScreen(%d,%d)",sx,sy );

	//フォント準備
#if defined(HSPNDK) || defined(HSPEMSCRIPTEN)
	#ifdef USE_JAVA_FONT
	//font_texid = MakeEmptyTex( FONT_TEX_SX, FONT_TEX_SY );
	#else
	font_texid = RegistTexMem( font_data, font_data_size );
	font_sx = 16;
	font_sy = 16;
	#endif
#endif

#if defined(HSPLINUX)
	#ifdef USE_TTFFONT

	//TTF初期化
	char fontpath[HSP_MAX_PATH+1];
	strcpy( fontpath, hsp3ext_getdir(1) );
	strcat( fontpath, TTF_FONTFILE );

	if ( TTF_Init() ) {
		Alertf( "Init:TTF_Init error" );
	}
	TexFontInit( fontpath, 18 );

	#else
	font_texid = RegistTexMem( font_data, font_data_size );
	font_sx = 16;
	font_sy = 16;
	#endif
#endif

	//		テキストを初期化
	//
	tmes.texmesInit(SYSREQ_MESCACHE_MAX);

	//		infovalをリセット
	//
	int i;
	for(i=0;i<GINFO_EXINFO_MAX;i++) {
		infoval[i] = 0.0;
	}

    //  timer initalize
#ifdef HSPIOS
    total_tick = 0.0;
    lastTime = CFAbsoluteTimeGetCurrent();
#endif

}


void hgio_size( int sx, int sy )
{
	_sizex = sx;
	_sizey = sy;
}


void hgio_view( int sx, int sy )
{
	_bgsx = sx;
	_bgsy = sy;
    //Alertf( "Size(%d,%d)",_bgsx,_bgsy );
}


void hgio_scale( float xx, float yy )
{
	_scaleX = xx;
	_scaleY = yy;
    //Alertf( "Scale(%f,%f)",_scaleX,_scaleY );
}


void hgio_autoscale( int mode )
{
	int m_mode;
	float x,y;
	float adjx,adjy;
	adjx = (float)_sizex/(float)_bgsx;
	adjy = (float)_sizey/(float)_bgsy;

	m_mode = mode;
	if ( mode == 0 ) {
		x = (float)_bgsx * adjy;
		y = (float)_bgsy * adjx;
		if ( adjx > adjy ) {
			m_mode=1;
			if ( y > (float)_sizey ) { m_mode=2; }
		} else {
			m_mode=2;
			if ( x > (float)_sizex ) { m_mode=1; }
		}
	}

	switch( m_mode ) {
	case 1:
		_scaleX = adjx;
		_scaleY = adjx;
		break;
	case 2:
		_scaleX = adjy;
		_scaleY = adjy;
		break;
	default:
		_scaleX = adjx;
		_scaleY = adjy;
		break;
	}
    //Alertf( "Scale(%f,%f)",_scaleX,_scaleY );
}


void hgio_uvfix( int mode )
{
	_uvfix = mode;

}


void hgio_reset( void )
{
    //投影変換/ビューポート変換 (ウインドウをターゲットに戻す)

    sw_main_ensure();
    sw_bfb(  ( sw_main_ok == 1 ) ? sw_main_fbo : 0 );
	sw_cur = mainbm;
	sw_apply_target( mainbm );

    glDisable(GL_LIGHTING);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
#if defined(HSPIOS) || defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
    glDisable(GL_DEPTH_BUFFER_BIT);
#endif

    //glClearColor(.7f, .7f, .9f, 1.f);
    //glShadeModel(GL_SMOOTH);

    
    //頂点配列の設定
    glVertexPointer(2,GL_FLOAT,0,panelVertices);
    glEnableClientState(GL_VERTEX_ARRAY);
    
    //UVの設定
    glTexCoordPointer(2,GL_FLOAT,0,panelUVs);
        
    //テクスチャの設定
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);


#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
	glDisable(GL_TEXTURE_2D);
#else
    glEnable(GL_TEXTURE_2D);
#endif


#if defined(HSPNDK) || defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
	if ( GetSysReq( SYSREQ_CLSMODE ) == CLSMODE_SOLID ) {
		//指定カラーで消去
		int ccol = GetSysReq( SYSREQ_CLSCOLOR );
		hgio_setClear( (ccol>>16)&0xff, (ccol>>8)&0xff, (ccol)&0xff );
		hgio_clear();
		/*	One shot.  HSP's cls clears the screen when the script asks for it;
			the mode is not meant to stay set.  Leaving it set made every
			redraw wipe the window, which breaks the idiom Elona's title menu
			is built on - draw the background once, then repaint only the
			cursor region each frame.  With it left set the background survived
			three frames and the menu sat on bare white after that.			*/
		SetSysReq( SYSREQ_CLSMODE, CLSMODE_NONE );
	}
#endif

    hgio_setview( mainbm );

    //ブレンドの設定
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);

    //ポイントの設定
    glEnable(GL_POINT_SMOOTH);

#if !defined(HSPEMSCRIPTEN)
    //前処理
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
#endif

	//テクスチャ設定リセット
	TexReset();

}

void hgio_term( void )
{
	tmes.texmesTerm();
	hgio_render_end();
	TexTerm();
	GeometryInit();
}


void hgio_resume( void )
{
	//	画面リソースの再構築
	//
	//	GLコンテキストが作り直されるため、古いFBO名は全て無効になる
	sw_forget_all();

	tmes.texmesInit(SYSREQ_MESCACHE_MAX);

	//テクスチャ初期化
	TexInit();

#if defined(HSPNDK) || defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
	#ifdef USE_JAVA_FONT
	//font_texid = MakeEmptyTex( FONT_TEX_SX, FONT_TEX_SY );
	#else
	font_texid = RegistTexMem( font_data, font_data_size );
	#endif
#endif
}


void hgio_setback( BMSCR *bm )
{
	//		背景画像の設定
	//		(NULL=なし)
	//
	backbm = bm;
}


/*	Name the screen that sw_main_overlay() draws back over the main screen.
	Handed over as soon as `screen` creates the second screen, so its framebuffer
	is built here rather than on the first present that has to show it.			*/
void hgio_set_help( BMSCR *bm )
{
	sw_helpbm = bm;
	if ( bm == NULL ) return;
	sw_bind_target( bm );
	if ( mainbm != NULL ) {
		sw_bind_target( mainbm );		// 呼び出し元はこの後 gsel する
	}
}


int hgio_gsel( BMSCR *bm )
{
	//		gsel(描画先変更)
	//		描画先を切り替えるだけ。フレームの確定はredrawに任せる
	//		(クラシックGDIランタイムと同じ意味論。ここでrender_endを
	//		 呼ぶとdrawflagが落ちてredraw 1でSwapWindowされなくなる)。
	//
	if ( bm == NULL ) return -1;
	sw_colorbm = bm;
	if ( sw_gsel_report < 800 ) {
		sw_gsel_report++;
		sw_fbo_log( "hgio: gsel bm=%p type=%d %dx%d texid=%d\n",
			(void *)bm, bm->type, bm->sx, bm->sy, bm->texid );
	}
	sw_bind_target( bm );
	return 0;
}


int hgio_buffer(BMSCR *bm)
{
	//		buffer(描画用画面作成)
	//		テクスチャを確保する。FBOは実際に描画する時に作る。
	//
	sw_forget( bm );
	int texid = MakeEmptyTexBuffer( bm->sx, bm->sy );
	if (texid >= 0) {
		bm->texid = texid;
	}
	if ( sw_buffer_report < 48 ) {
		sw_buffer_report++;
		TEXINF *t = ( texid >= 0 ) ? GetTex( texid ) : NULL;
		sw_fbo_log( "hgio: buffer bm=%p %dx%d -> texid=%d glid=%u potsx=%d potsy=%d\n",
			(void *)bm, bm->sx, bm->sy, texid,
			t ? (unsigned)t->texid : 0u, t ? (int)t->sx : 0, t ? (int)t->sy : 0 );
	}
	return 0;
}


static int GetSurface(int x, int y, int sx, int sy, int px, int py, void *res, int mode)
{
	//	VRAMの情報を取得する
	//
	int ybase = _bgsy - (sy - y);

#ifdef	GP_USE_ANGLE
	return -1;
#else
#if defined(HSPWIN)||defined(HSPLINUX)
#ifndef HSPRASPBIAN
	glReadBuffer(GL_BACK);
#endif
#endif
#endif

	// OpenGLで画面に描画されている内容をバッファに格納
	glReadPixels(
		x,              //読み取る領域の左下隅のx座標
		ybase,          //読み取る領域の左下隅のy座標
		sx,             //読み取る領域の幅
		sy,             //読み取る領域の高さ
		GL_RGBA,		//取得したい色情報の形式
		GL_UNSIGNED_BYTE,  //読み取ったデータを保存する配列の型
		res                //ビットマップのピクセルデータ（実際にはバイト配列）へのポインタ
	);

	return 0;
}


int hgio_bufferop(BMSCR* bm, int mode, char *ptr)
{
	//		オフスクリーンバッファを操作
	//
	int texid = bm->texid;
	if (texid < 0) return -1;

	if (mode & 0x1000) {
		return UpdateTexStar(texid, mode & 0xfff);
	}

	switch (mode) {
	case 0:
	case 1:
		return UpdateTex32(texid, ptr, mode);
	case 16:
	case 17:
		GetSurface(0, 0, bm->sx, bm->sy, 1, 1, ptr, mode & 15);
		return 0;
	default:
		return -2;
	}

	return 0;
}


/*-------------------------------------------------------------------------------*/

void hgio_clear( void )
{
	glClear(GL_COLOR_BUFFER_BIT); 
}


void hgio_setClear( int rval, int gval ,int bval )
{
	if ( sw_clear_report < 4 ) {
		sw_clear_report++;
		sw_fbo_log( "hgio: clear colour %d,%d,%d\n", rval, gval, bval );
	}
	glClearColor((GLclampf)(FVAL_BYTE1 * (float)rval), (GLclampf)(FVAL_BYTE1 * (float)gval), (GLclampf)(FVAL_BYTE1 * (float)bval), 1 );
}


void hgio_setFilterMode( int mode )
{
    switch( mode ) {
        case 0:
            _filter = GL_NEAREST;
            break;
        default:
            _filter = GL_LINEAR;
            break;
    }
}

//色の指定
/*
static void hgio_setColorTex( int rval, int gval ,int bval )
{
	GLfloat r = FVAL_BYTE1 * rval;
	GLfloat g = FVAL_BYTE1 * gval;
	GLfloat b = FVAL_BYTE1 * bval;
	GLfloat *flp = _panelColorsTex;
	for (int i=0;i<4;i++) {
		*flp++ = r;
		*flp++ = g;
		*flp++ = b;
		*flp++ = 1.0f;
	}
}
*/

static void setColorTex_reset( float alpha )
{
	GLfloat *flp = _panelColorsTex;
	for (int i=0;i<4;i++) {
		*flp++ = 1.0f;
		*flp++ = 1.0f;
		*flp++ = 1.0f;
		*flp++ = alpha;
	}
}

static void setColorTex_color( float alpha )
{
	BMSCR *cbm = ( sw_colorbm != NULL ) ? sw_colorbm : mainbm;
	GLfloat *flp = _panelColorsTex;
	GLfloat r,g,b;
	if ( cbm != NULL ) {
		r = cbm->colorvalue[0];
		g = cbm->colorvalue[1];
		b = cbm->colorvalue[2];
	} else {
		r = 1.0f;
		g = 1.0f;
		b = 1.0f;
	}
	for (int i=0;i<4;i++) {
		*flp++ = r;
		*flp++ = g;
		*flp++ = b;
		*flp++ = alpha;
	}
}

static void setColorTex_mulcolor( float alpha )
{
	BMSCR *cbm = ( sw_colorbm != NULL ) ? sw_colorbm : mainbm;
	GLfloat *flp = _panelColorsTex;
	GLfloat r,g,b;
	if ( cbm != NULL ) {
		r = cbm->mulcolorvalue[0];
		g = cbm->mulcolorvalue[1];
		b = cbm->mulcolorvalue[2];
	} else {
		r = 1.0f;
		g = 1.0f;
		b = 1.0f;
	}
	for (int i=0;i<4;i++) {
		*flp++ = r;
		*flp++ = g;
		*flp++ = b;
		*flp++ = alpha;
	}
}

static void setBlendMode( int mode )
{
	sw_glColorKey( 0, 0 );
	// mode=2 はアルファあり半透明レート無効なのでアルファを 1.0 で埋める
    switch( mode ) {
        case 0:                     //no blend
        case 1:                     //no blend
            glDisable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            break;
        case 5:                     //add
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE);
            break;
        case 6:                     //sub
            /*	The upstream equation call sits inside #ifdef HSPIOS, which
            	this target never defines, so gmode 6 used to fall through
            	to the same glBlendFunc() as gmode 5 and drew as add.	*/
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE);
            break;
        default:                    //normal blend
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
            //glBlendFunc(GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
            break;
    }
}

static void hgio_setTexBlendMode( BMSCR *bm, int mode, int aval )
{
	/*	gmode 2 and gmode 4 carry the picture's colour key in the classic
		runtime, so the key pixels must not be painted.  Every other path
		(setBlendMode() below) turns the key back off. */
	setBlendMode( mode );
	if ( mode == 2 ) {
		/*	Colour-key copy: the classic runtime skips the key-coloured
			source pixels and copies every other one opaquely - there is no
			alpha to blend with.  The shader already discards the key, so
			blending here only mixed the copy with the destination whenever
			the source's alpha had fallen below 1.  That is what let the
			title menu's per-row strip pick up the neighbouring row's text.
																					*/
		glDisable( GL_BLEND );
		sw_glColorKey( 1, 0x000000u );
	} else if ( ( mode == 4 ) && ( bm != NULL ) ) {
		sw_glColorKey( 1, (unsigned)bm->color & 0xffffffu );
	} else {
		sw_glColorKey( 0, 0 );
	}
    //ブレンドモード設定

    if ( mode <= 1 ) {
        glDisableClientState(GL_COLOR_ARRAY);
	} else {
		GLfloat alpha;
	    if ( mode >= 3 ) {
			alpha = FVAL_BYTE1*( aval );
		} else {
			alpha = 1.0f;
		}
		setColorTex_mulcolor(alpha);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4,GL_FLOAT,0,_panelColorsTex);
    }

    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,_filter); 
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,_filter); 
}

//ライン幅の指定
void hgio_setLineWidth( int lineWidth )
{
    glLineWidth(lineWidth);
    glPointSize(lineWidth*0.6f);
}

//フリップモードの指定
void hgio_setFlipMode( int flipMode )
{
    _flipMode=flipMode;
}

//原点の指定
void hgio_setOrigin( int x, int y )
{
    _originX=x;
    _originY=y;
}

/*-------------------------------------------------------------------------------*/

void hgio_clsmode( int mode, int color, int tex )
{
	SetSysReq( SYSREQ_CLSMODE, mode );
	SetSysReq( SYSREQ_CLSCOLOR, color );
	SetSysReq( SYSREQ_CLSTEX, tex );
}


int hgio_getWidth( void )
{
	return _bgsx;
}


int hgio_getHeight( void )
{
	return _bgsy;
}


int hgio_getDesktopWidth( void )
{
#ifdef HSPLINUX
	SDL_DisplayMode dm;
	SDL_GetDesktopDisplayMode(0,&dm);
	return dm.w;
#endif
	return _bgsx;
}


int hgio_getDesktopHeight( void )
{
#ifdef HSPLINUX
	SDL_DisplayMode dm;
	SDL_GetDesktopDisplayMode(0,&dm);
	return dm.h;
#endif
	return _bgsy;
}


void hgio_setfilter( int type, int opt )
{
	hgio_setFilterMode( type );
}


int hgio_title( char *str1 )
{
#if defined(HSPEMSCRIPTEN)
	SDL_SetWindowTitle( window, (const char *)str1 );
	//SDL_WM_SetCaption( (const char *)str1, NULL );
#endif
#if defined(HSPLINUX)
#ifndef HSPRASPBIAN
	SDL_SetWindowTitle( window, (const char *)str1 );
	//SDL_WM_SetCaption( (const char *)str1, NULL );
#endif
#endif
	return 0;
}

/*	switch_input_poll() fills the pad tables, and until now the only caller was
	the once-per-frame tick (gles1_shim's sw_frame_tick).  A script can sit in a
	wait loop that never redraws - Elona's text box does exactly that (the stall
	dump shows objsel + getkey repeating) - and then no frame means no poll, so
	getkey and stick answer from a frozen table forever and the guest hangs with
	the box still waiting for a key.  Refresh on demand instead, at most every
	few milliseconds so a tight polling loop stays cheap. */
static void sw_input_refresh( void )
{
	static unsigned int last = 0;
	unsigned int now = (unsigned int)hgio_gettick();

	if ( ( now - last ) < 4 ) return;
	last = now;
	switch_input_poll();
}

int hgio_stick( int actsw )
{
	int ckey = 0;

	sw_input_refresh();
#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
#ifndef HSPRASPBIAN
	if ( sw_input_key(SDL_SCANCODE_LEFT) )  ckey|=1;		// [left]
	if ( sw_input_key(SDL_SCANCODE_UP) )    ckey|=1<<1;		// [up]
	if ( sw_input_key(SDL_SCANCODE_RIGHT) ) ckey|=1<<2;		// [right]
	if ( sw_input_key(SDL_SCANCODE_DOWN) )  ckey|=1<<3;		// [down]
	if ( sw_input_key(SDL_SCANCODE_SPACE) ) ckey|=1<<4;		// [spc]
	if ( sw_input_key(SDL_SCANCODE_RETURN) )ckey|=1<<5;		// [ent]
	if ( sw_input_key(SDL_SCANCODE_LCTRL) || sw_input_key(SDL_SCANCODE_RCTRL) ) ckey|=1<<6;		// [ctrl]
	if ( sw_input_key(SDL_SCANCODE_ESCAPE) )ckey|=1<<7;	// [esc]
	if ( mouse_btn & SDL_BUTTON_LMASK ) ckey|=1<<8;	// mouse_l
	if ( mouse_btn & SDL_BUTTON_RMASK ) ckey|=1<<9;	// mouse_r
	if ( sw_input_key(SDL_SCANCODE_TAB) )   ckey|=1<<10;	// [tab]
	
	if ( sw_input_key(SDL_SCANCODE_Z) )     ckey|=1<<11;
	if ( sw_input_key(SDL_SCANCODE_X) )     ckey|=1<<12;
	if ( sw_input_key(SDL_SCANCODE_C) )     ckey|=1<<13;
	
	if ( sw_input_key(SDL_SCANCODE_A) )     ckey|=1<<14;
	if ( sw_input_key(SDL_SCANCODE_W) )     ckey|=1<<15;
	if ( sw_input_key(SDL_SCANCODE_D) )     ckey|=1<<16;
	if ( sw_input_key(SDL_SCANCODE_S) )     ckey|=1<<17;
#else
	if ( get_key_state(37) ) ckey|=1;		// [left]
	if ( get_key_state(38) ) ckey|=2;		// [up]
	if ( get_key_state(39) ) ckey|=4;		// [right]
	if ( get_key_state(40) ) ckey|=8;		// [down]
	if ( get_key_state(32) ) ckey|=16;		// [spc]
	if ( get_key_state(13) ) ckey|=32;		// [ent]
	if ( get_key_state(17) ) ckey|=64;		// [ctrl]
	if ( get_key_state(27) ) ckey|=128;		// [esc]
	if ( get_key_state(1) )  ckey|=256;		// mouse_l
	if ( get_key_state(2) )  ckey|=512;		// mouse_r
	if ( get_key_state(9) )  ckey|=1024;	// [tab]
#endif

#else
	if ( mouse_btn ) ckey|=256;	// mouse_l
#endif
	return ckey;
}

#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
#ifndef HSPRASPBIAN
static const unsigned int key_map[256]={
	/* 0- */
	0, 0, 0, 3, 0, 0, 0, 0, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB, 0, 0, 12, SDL_SCANCODE_RETURN, 0, 0,
	0, 0, 0, SDL_SCANCODE_PAUSE, SDL_SCANCODE_CAPSLOCK, 0, 0, 0, 0, 0, 0, SDL_SCANCODE_ESCAPE, 0, 0, 0, 0,
	/* 32- */
	SDL_SCANCODE_SPACE, SDL_SCANCODE_PAGEUP, SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_END, SDL_SCANCODE_HOME,
	SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, 0, SDL_SCANCODE_PRINTSCREEN, 0, 0, SDL_SCANCODE_INSERT, SDL_SCANCODE_DELETE, SDL_SCANCODE_HELP,
	/* 48- */
	SDL_SCANCODE_0, SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9,
	0, 0, 0, 0, 0, 0, 0,
	/* 65- */
	SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I,
	SDL_SCANCODE_J, SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O, SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R,
	SDL_SCANCODE_S, SDL_SCANCODE_T, SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y, SDL_SCANCODE_Z,
	/* 91- */
	SDL_SCANCODE_LGUI, SDL_SCANCODE_RGUI, 0, 0, 0,
	SDL_SCANCODE_KP_0, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3, SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7, SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_9,
	SDL_SCANCODE_KP_MULTIPLY, SDL_SCANCODE_KP_PLUS, 0, SDL_SCANCODE_KP_MINUS, SDL_SCANCODE_KP_PERIOD, SDL_SCANCODE_KP_DIVIDE, 
	/* 112- */
	SDL_SCANCODE_F1, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5, SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10,
	SDL_SCANCODE_F11, SDL_SCANCODE_F12, SDL_SCANCODE_F13, SDL_SCANCODE_F14, SDL_SCANCODE_F15, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* 136- */
	0, 0, 0, 0, 0, 0, 0, 0, SDL_SCANCODE_NUMLOCKCLEAR, 145,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* 160- */
	SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL, SDL_SCANCODE_LALT, SDL_SCANCODE_RALT,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* 186- */
	SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_COMMA, SDL_SCANCODE_MINUS, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, 0, 
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	/* 219- */
	SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_RIGHTBRACKET, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

bool hgio_getkey( int kcode )
{
	bool res = false;

	sw_input_refresh();
	switch( kcode ){
		case 1: res = (mouse_btn & SDL_BUTTON_LMASK) > 0; break;
		case 2: res = (mouse_btn & SDL_BUTTON_RMASK) > 0; break;
		case 4: res = (mouse_btn & SDL_BUTTON_MMASK) > 0; break;
		case 5: res = (mouse_btn & SDL_BUTTON_X1MASK) > 0; break;
		case 6: res = (mouse_btn & SDL_BUTTON_X2MASK) > 0; break;
		case 16: res = sw_input_key(SDL_SCANCODE_LSHIFT) | sw_input_key(SDL_SCANCODE_RSHIFT); break;
		case 17: res = sw_input_key(SDL_SCANCODE_LCTRL) | sw_input_key(SDL_SCANCODE_RCTRL); break;
		case 18: res = sw_input_key(SDL_SCANCODE_LALT) | sw_input_key(SDL_SCANCODE_RALT); break;
		default: res = sw_input_key( key_map[ kcode & 255 ] ); break;
	}
	return res;
}
#else
bool hgio_getkey( int kcode )
{
	return get_key_state( kcode );
}
#endif

#endif



int hgio_texload( BMSCR *bm, char *fname )
{
	TEXINF *t;
	int texid;

	hgio_delscreen( bm );

	texid = RegistTex( fname );
	if ( texid < 0 ) return -1;

	t = GetTex( texid );
	if ( t->mode == TEXMODE_NONE ) return -1;

	bm->sx = t->width;
	bm->sy = t->height;
	bm->texid = texid;
	if ( sw_texload_report < 64 ) {
		sw_texload_report++;
		sw_fbo_log( "hgio: texload bm=%p '%s' -> texid=%d %dx%d\n",
			(void *)bm, fname, texid, bm->sx, bm->sy );
	}

	return texid;
}


/*	Elona stamps every PCC part into its scratch buffer with `picload file, 1`
	(chips.hsp, all sixteen parts of create_pcpic) and recomposes the sprite
	constantly.  RegistTex() decodes the file and allocates a fresh GL texture
	each time; here the picture is only *sampled* by the blit below, so that
	texture was dead the moment the quad finished - yet nothing freed it.  A
	hardware run allocated 15333 of them (128x256, one PCC template each) and hit
	GL_OUT_OF_MEMORY after ~6000.  Eden never does: its host GPU has the room.
	Keep the decoded picture of the files that get reloaded - the SD read and
	the stb decode disappear with it.						*/
#define SW_PIC_CACHE_MAX 48

typedef struct {
	char			name[96];
	int				texid;
	unsigned int	gl;
	unsigned int	used;
} SW_PICCACHE;

static SW_PICCACHE		sw_pic_cache[SW_PIC_CACHE_MAX];
static int				sw_pic_cache_n = 0;
static unsigned int	sw_pic_cache_tick = 0;

static int sw_pic_load( const char *fname )
{
	int i, slot, texid;

	slot = -1;
	for ( i = 0; i < sw_pic_cache_n; i++ ) {
		if ( strcmp( sw_pic_cache[i].name, fname ) == 0 ) { slot = i; break; }
	}
	/*	A hit is trusted only while the TEXINF slot still holds the very
		texture we cached; the table recycles slots by index.		*/
	if ( slot >= 0 ) {
		TEXINF *c = GetTex( sw_pic_cache[slot].texid );
		if ( ( c != NULL ) && ( c->mode != TEXMODE_NONE ) &&
			( c->texid == (GLuint)sw_pic_cache[slot].gl ) ) {
			sw_pic_cache[slot].used = ++sw_pic_cache_tick;
			return sw_pic_cache[slot].texid;
		}
	}
	if ( slot < 0 ) {
		if ( sw_pic_cache_n < SW_PIC_CACHE_MAX ) {
			slot = sw_pic_cache_n++;
		} else {
			slot = 0;
			for ( i = 1; i < SW_PIC_CACHE_MAX; i++ ) {
				if ( sw_pic_cache[i].used < sw_pic_cache[slot].used ) slot = i;
			}
			/*	TexReset() first: the binding cache must not keep an id
				we are about to delete.					*/
			TexReset();
			DeleteTex( sw_pic_cache[slot].texid );
		}
	}
	texid = RegistTex( (char *)fname );
	if ( texid < 0 ) { sw_pic_cache[slot].name[0] = 0; return -1; }
	strncpy( sw_pic_cache[slot].name, fname, sizeof( sw_pic_cache[slot].name ) - 1 );
	sw_pic_cache[slot].name[sizeof( sw_pic_cache[slot].name ) - 1] = 0;
	sw_pic_cache[slot].texid = texid;
	sw_pic_cache[slot].gl = (unsigned int)GetTex( texid )->texid;
	sw_pic_cache[slot].used = ++sw_pic_cache_tick;
	return texid;
}


/*	`picload file, 1` is *overwrite*, not resize: the classic runtime
	writes the decoded picture into the screen's memory at (0,0) and
	leaves the screen's size alone (hspwnd_win.cpp, Picload: only mode 0
	and 2 re-make the screen).  Elona loads 180x300 background tiles into
	its 1584x1632 picture buffer this way, and the dish path - which
	always re-builds the screen from the picture - used to shrink that
	buffer to 180x300: every later read-back of it (the satisfied-menu
	restore, `gcopy BUFFER_MAP, 0, 0, 800, 500`) then came back clipped
	to 180x300 and the old composition stayed on the screen.

	Here the picture is decoded and drawn once into the screen's own
	framebuffer, so the screen keeps its size and its read-back works.
	Returns 0 when it handled the load, -1 to fall back to the classic
	path.																*/
int hgio_picload_overwrite( BMSCR *bm, char *fname )
{
	TEXINF *t;
	int texid;
	GLfloat vert[8];
	GLfloat uv[8];
	float w, h;
	float px, py;

	if ( bm == NULL ) return -1;
	if ( bm->type == HSPWND_TYPE_MAIN ) return -1;
	if ( bm->type == HSPWND_TYPE_NONE ) return -1;

	texid = sw_pic_load( fname );
	if ( texid < 0 ) return -1;
	t = GetTex( texid );
	if ( ( t == NULL ) || ( t->mode == TEXMODE_NONE ) ) return -1;
	w = (float)t->width;
	h = (float)t->height;
	if ( ( w <= 0.0f ) || ( h <= 0.0f ) ) return -1;

	if ( sw_bind_target( bm ) != 0 ) return -1;

	/*	t23 probe: read the picture itself back before the blit.  The blit
		lands as opaque black - which is what a sampler gives back for a
		texture with no usable level - so first find out whether the picture
		RegistTex() just decoded has pixels at all.						*/
	{
		static int sw_srctex_probe = 0;
		if ( SWITCH_DIAG && sw_srctex_probe < 24 ) {
			unsigned char sp[4] = { 7, 7, 7, 7 };
			if ( sw_scratch_ensure( 64, 64 ) == 0 ) {
				sw_srctex_probe++;
				sw_bfb(  sw_scratch_fbo );
				glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					(GLuint)t->texid, 0 );
				glReadPixels( (GLint)( t->width / 2 ), (GLint)( t->height / 2 ), 1, 1,
					GL_RGBA, GL_UNSIGNED_BYTE, sp );
				glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
					sw_scratch_tex, 0 );
				sw_fbo_log( "hgio: srctex '%s' gl=%u pic=%dx%d pot=%dx%d px=%d,%d,%d,%d err=0x%x\n",
					fname, (unsigned)t->texid, (int)t->width, (int)t->height, (int)t->sx, (int)t->sy,
					sp[0], sp[1], sp[2], sp[3], (unsigned)sw_glGetError() );
			}
			sw_bind_target( bm );
		}
	}

	/*	the classic overwrite lands at the current position, not at the
		origin: hspwnd_win.cpp Picload() takes `x = bm->cx; y = bm->cy`
		and renders the picture there (mode 1 never re-makes the screen).
		Elona relies on that everywhere - `pos 960, 96; picload
		graphic\deco_cm.bmp, 1` keeps its decorations clear of the
		interface art sharing BUFFER_INF, and stamping them at (0,0)
		overwrote the window-frame tiles `window` reads back from
		(0..264, 48..264) - so the panel body disappeared.				*/
	px = (float)bm->cx;
	py = (float)-bm->cy;
	vert[0] = px;		vert[1] = py;
	vert[2] = px + w;	vert[3] = py;
	vert[4] = px;		vert[5] = py - h;
	vert[6] = px + w;	vert[7] = py - h;

	uv[0] = 0.0f;		uv[1] = 0.0f;
	uv[2] = w * t->ratex;	uv[3] = 0.0f;
	uv[4] = 0.0f;		uv[5] = h * t->ratey;
	uv[6] = w * t->ratex;	uv[7] = h * t->ratey;

	glDisableClientState( GL_COLOR_ARRAY );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, vert );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );

	sw_bind_tex( (int)t->texid );
	/*	The picture was created by RegistTex() a moment ago and nothing has
		touched its filters yet.  ES2 defaults GL_TEXTURE_MIN_FILTER to
		GL_NEAREST_MIPMAP_LINEAR, so a texture with only level 0 is
		incomplete and samples as opaque black - which is what the screen
		was getting.  Every other draw path sets the filters through
		hgio_setTexBlendMode(); this one has to as well.  gmode 0 = no
		blend, which is what an overwrite wants, and it also turns the
		colour key off and drops the colour array.						*/
	hgio_setTexBlendMode( bm, 0, 0 );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	/*	Real-hardware GPU sync: the next gcopy samples from the same texture
		we just rendered into.  On the Tegra X1 the draw can still be in
		flight when the sample lands, so the copy reads back the uncoloured
		template.  glFinish() stalls until the subtraction quad is out of the
		GPU pipeline; Eden's software path does not need it.					*/
	glFinish();
	{
		/*	t23 probe: read the blit back while the screen's own framebuffer is
			still bound.  If the picture is here and the dumped screen is black
			later, something painted over it; if it is black here, the blit itself
			did not land.														*/
		static int sw_picload_probe = 0;
		if ( SWITCH_DIAG && sw_picload_probe < 48 ) {
			unsigned char mid[4] = { 9, 9, 9, 9 };
			unsigned char cor[4] = { 8, 8, 8, 8 };
			SWTARGET *pr = sw_find( bm );
			glReadPixels( (GLint)( bm->sx / 2 ), (GLint)( bm->sy / 2 ), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, mid );
			glReadPixels( 1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, cor );
			sw_picload_probe++;
			sw_fbo_log( "hgio: picload verify bm=%p txtid=%d sx=%d sy=%d pic=%dx%d gl=%u fbo=%u cur=%p mid=%d,%d,%d,%d cor=%d,%d,%d,%d err=0x%x\n",
				(void *)bm, bm->texid, bm->sx, bm->sy, (int)t->width, (int)t->height,
				(unsigned)t->texid, pr ? (unsigned)pr->fbo : 0u, (void *)sw_cur,
				mid[0], mid[1], mid[2], mid[3], cor[0], cor[1], cor[2], cor[3],
				(unsigned)sw_glGetError() );
		}
	}
	ChangeTex( -1 );

	if ( sw_texload_report < 64 ) {
		sw_texload_report++;
		sw_fbo_log( "hgio: picload overwrite bm=%p '%s' texid=%d %gx%g keep %dx%d at %d,%d\n",
			(void *)bm, fname, texid, (double)w, (double)h, bm->sx, bm->sy,
			(int)bm->cx, (int)bm->cy );
	}

	return 0;
}

/*-------------------------------------------------------------------------------*/

//ポイントカラー設定
void hgio_panelcolor( GLfloat *colors, int color, int aval )
{
	GLfloat *flp;
	GLfloat r = RGBA2R(color);
	GLfloat g = RGBA2G(color);
	GLfloat b = RGBA2B(color);
	GLfloat a = FVAL_BYTE1 * (aval&0xff);
	flp = colors;
	for (int i=0;i<4;i++) {
		*flp++ = r;
		*flp++ = g;
		*flp++ = b;
		*flp++ = a;
	}

    glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4,GL_FLOAT,0,colors);
}


static void setCurrentColor( GLfloat *colors, int vnum )
{
	BMSCR *cbm = ( sw_colorbm != NULL ) ? sw_colorbm : mainbm;
	GLfloat *flp;
	flp = colors;
	GLfloat r,g,b,a;

	if ( cbm ) {
		r = cbm->colorvalue[0];
		g = cbm->colorvalue[1];
		b = cbm->colorvalue[2];
	} else {
		r = 1.0f;
		g = 1.0f;
		b = 1.0f;
	}

	for (int i=0;i<vnum;i++) {
		*flp++ = r;
		*flp++ = g;
		*flp++ = b;
		*flp++ = 1.0f;
	}

	glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4,GL_FLOAT,0,colors);
}

//ポイント描画
void hgio_pset( float x, float y )
{
    //頂点配列情報
	GLfloat colors[1*4];
    GLfloat vert[2]={
		x, -y
	};

	glDisable(GL_BLEND);
    //glBindTexture(GL_TEXTURE_2D,0);
    glVertexPointer(2,GL_FLOAT,0,vert);
	setCurrentColor(colors,1);
    glDrawArrays(GL_POINTS,0,1);
}


//矩形の描画
void hgio_rect( float x, float y, float w, float h )
{
	sw_trc( "rect", NULL, x, y, w, h );
    //頂点配列情報
	GLfloat colors[4*4];
	GLfloat vert[8]={
		x,   -y,
		x,   -y-h,
		x+w, -y-h,
		x+w, -y
	};

	glDisable(GL_BLEND);
    //glBindTexture(GL_TEXTURE_2D,0);
    glVertexPointer(2,GL_FLOAT,0,vert);
	setCurrentColor(colors,4);
    glDrawArrays(GL_LINE_LOOP,0,4);
}


//矩形の塗り潰し
void hgio_boxfill( float x, float y, float w, float h )
{
	sw_trc( "boxfill", NULL, x, y, w, h );
    //頂点配列情報
	GLfloat colors[4*4];
	GLfloat vert[8]={
		x,   -y,
		x,   -y-h,
		x+w, -y,
		x+w, -y-h
	};

	glDisable(GL_BLEND);
	//glBindTexture(GL_TEXTURE_2D,0);
	glVertexPointer(2,GL_FLOAT,0,vert);
	setCurrentColor(colors,4);
	glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}

//円の描画
void hgio_circleLine( float x, float y, float rx, float ry )
{
    int length = CIRCLE_DIV;

	//頂点配列情報
	GLfloat colors[length*4];
    GLfloat vert[length*2], *flp;
	flp = vert;
	for (int i=0;i<length;i++) {
		float angle=2*M_PI*i/length;
		*flp++ =  x+cos(angle)*rx;
		*flp++ = -y+sin(angle)*ry;
	}

	glDisable(GL_BLEND);
    //glBindTexture(GL_TEXTURE_2D,0);
    glVertexPointer(2,GL_FLOAT,0,vert);
 	setCurrentColor(colors,length);
	glDrawArrays(GL_LINE_LOOP,0,length);
}

//円の塗り潰し
void hgio_circleFill( float x, float y, float rx, float ry )
{
    int length = CIRCLE_DIV+2;
    
	//頂点配列情報
	GLfloat colors[length*4];
    GLfloat vert[length*2], *flp;
	flp = vert;
	*flp++ =  x;
	*flp++ = -y;
	for (int i=1;i<length;i++) {
		float angle=2*M_PI*i/(length-2);
		*flp++ =  x+cos(angle)*rx;
		*flp++ = -y+sin(angle)*ry;
	}

	glDisable(GL_BLEND);
    //glBindTexture(GL_TEXTURE_2D,0);
    glVertexPointer(2,GL_FLOAT,0,vert);
  	setCurrentColor(colors,length);
	glDrawArrays(GL_TRIANGLE_FAN,0,length);
}


/*-------------------------------------------------------------------------------*/

void hgio_line( BMSCR *bm, float x, float y )
{
	//		ライン描画
	//		(bm!=NULL の場合、ライン描画開始)
	//		(bm==NULL の場合、ライン描画完了)
	//		(ラインの座標は必要な数だけhgio_line2を呼び出す)
	//
	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

	setCurrentColor(_line_colors,2);

	linebasex = x + 0.375f;
	linebasey = y + 0.375f;

	glDisable(GL_BLEND);
	ChangeTex( -1 );
    //glBindTexture(GL_TEXTURE_2D,0);
}


//ラインの描画
void hgio_line2( float x, float y )
{
	//		ライン描画
	//		(hgio_lineで開始後に必要な回数呼ぶ、hgio_line(NULL)で終了すること)
	//

	//頂点配列情報
	GLfloat vert[4];
	vert[0]= linebasex;
	vert[1]=-linebasey;
	linebasex = x + 0.375f;
	linebasey = y + 0.375f;
	vert[2]= linebasex;
	vert[3]=-linebasey;

    glVertexPointer(2,GL_FLOAT,0,vert);
    glEnableClientState(GL_COLOR_ARRAY);
    glColorPointer(4,GL_FLOAT,0,_line_colors);
    glDrawArrays(GL_LINE_STRIP,0,2);
}


void hgio_boxfAlpha(BMSCR *bm, float x1, float y1, float x2, float y2, int alphamode)
{
	/*	r149: clears that touch the message band.					*/
	{
		float sw_b_lo = ( y1 < y2 ) ? y1 : y2;
		float sw_b_hi = ( y1 < y2 ) ? y2 : y1;
		if ( ( sw_b_hi >= 500.0f ) && ( sw_b_lo <= 610.0f ) ) {
			static int sw_band_B_n = 0;
			if ( sw_band_B_n < 1500 ) {
				sw_band_B_n++;
				printf( "t48 clearA t=%d x=%.0f..%.0f y=%.0f..%.0f a=%d\n",
					(int)hgio_gettick(), x1, x2, y1, y2, alphamode );
				fflush( stdout );
			}
		}
	}
	//		矩形描画
	//
	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

	ChangeTex( -1 );

	/*	A solid fill never carries a picture's colour key; drop any left over
		from the previous copy so the rectangle really lands.				*/
	sw_glColorKey( 0, 0 );

	float x = x1;
	float y = y1;
	float w = (x2-x1);
	float h = (y2-y1);
	sw_trc( alphamode ? "boxfA" : "boxf", bm, x, y, w, h );

    //頂点配列情報
	GLfloat colors[4*4];
	GLfloat vert[8]={
		x,   -y,
		x,   -y-h,
		x+w, -y,
		x+w, -y-h
	};

	if (alphamode) {
		setBlendMode( bm->gmode );
		hgio_panelcolor( colors, bm->color, bm->gmode < 3 ? 255 : bm->gfrate );
	} else {
		glDisable(GL_BLEND);
		setCurrentColor(colors,4);
	}
	glVertexPointer(2,GL_FLOAT,0,vert);
	glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}


void hgio_boxf( BMSCR *bm, float x1, float y1, float x2, float y2 )
{
	hgio_boxfAlpha(bm, x1, y1, x2, y2, 0);
}


void hgio_circle( BMSCR *bm, float x1, float y1, float x2, float y2, int mode )
{
	//		円描画
	//
	float xx,yy,rx,ry;
	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

	rx = ((float)abs(x2-x1))*0.5f;
	ry = ((float)abs(y2-y1))*0.5f;
	xx = ((float)x1) + rx;
	yy = ((float)y1) + ry;

	ChangeTex( -1 );
	//hgio_setColor( bm->color );
	if ( mode ) {
		hgio_circleFill( xx, yy, rx, ry );
	} else {
		hgio_circleLine( xx, yy, rx, ry );
	}
}


//		矩形(回転)描画
//
void hgio_fillrot( BMSCR *bm, float x, float y, float sx, float sy, float ang )
{
	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;
	sw_trc( "fillrot", bm, x, y, sx, sy );
    
	GLfloat colors[16];
    GLfloat *flp;
	GLfloat x0,y0,x1,y1,ofsx,ofsy;
    
	ofsx = sx;
	ofsy = sy;
	x0 = -(float)sin( ang );
	y0 = (float)cos( ang );
	x1 = -y0;
	y1 = x0;
    
	ofsx *= -0.5f;
	ofsy *= -0.5f;
	x0 *= ofsy;
	y0 *= ofsy;
	x1 *= ofsx;
	y1 *= ofsx;
    
    flp = vertf2D;
    
	*flp++ = (-x0-x1) + x;
	*flp++ = -((-y0-y1) + y);

	*flp++ = (-x0+x1) + x;
	*flp++ = -((-y0+y1) + y);
    
	*flp++ = (x0-x1) + x;
	*flp++ = -((y0-y1) + y);
    
	*flp++ = (x0+x1) + x;
	*flp++ = -((y0+y1) + y);

	ChangeTex( -1 );

    glVertexPointer(2,GL_FLOAT,0,vertf2D);

	setBlendMode( bm->gmode );
	hgio_panelcolor( colors, bm->color, bm->gmode < 3 ? 255 : bm->gfrate );

    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}


#if 0
void hgio_fcopy( float distx, float disty, short xx, short yy, short srcsx, short srcsy, int texid, int color )
{
	sw_trc_ex( "fcopy", NULL, distx, disty, (float)srcsx, (float)srcsy, texid, (int)xx, (int)yy, (int)srcsx, (int)srcsy );
	//		画像コピー(フォント用)
	//		texid内の(xx,yy)-(xx+srcsx,yy+srcsy)を現在の画面に等倍でコピー
	//		描画モードは3,100%、転送先はdistx,disty
	//
	TEXINF *tex = GetTex( texid );
	if ( tex->mode == TEXMODE_NONE ) return;

	GLfloat colors[16];
    GLfloat *flp;
    GLfloat x1,y1,x2,y2;
    float ratex,ratey;

    flp = vertf2D;
    x1 = (GLfloat)distx;
    y1 = (GLfloat)-disty;
    x2 = x1+srcsx;
    y2 = y1-srcsy;

    *flp++ = x1;
    *flp++ = y1;
    *flp++ = x1;
    *flp++ = y2;
    *flp++ = x2;
    *flp++ = y1;
    *flp++ = x2;
    *flp++ = y2;

    //ratex = 1.0f / image.width;
    //ratey = 1.0f / image.height;
    ratex = tex->ratex;
    ratey = tex->ratey;

    flp = uvf2D;
	if ( _uvfix ) {
	    x1 = (((GLfloat)xx) + 0.5f) * ratex;
	    y1 = (((GLfloat)yy) + 0.5f) * ratey;
	    x2 = ((GLfloat)(xx+srcsx) - 0.5f) * ratex;
	    y2 = ((GLfloat)(yy+srcsy) - 0.5f) * ratey;
	} else {
	    x1 = ((GLfloat)xx) * ratex;
	    y1 = ((GLfloat)yy) * ratey;
	    x2 = ((GLfloat)(xx+srcsx)) * ratex;
	    y2 = ((GLfloat)(yy+srcsy)) * ratey;
	}

    *flp++ = x1;
    *flp++ = y1;
    *flp++ = x1;
    *flp++ = y2;
    *flp++ = x2;
    *flp++ = y1;
    *flp++ = x2;
    *flp++ = y2;

#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
	glEnable(GL_TEXTURE_2D);
#endif

	sw_bind_tex( tex->texid );
//    glBindTexture( GL_TEXTURE_2D, tex->texid );
    glVertexPointer( 2, GL_FLOAT,0,vertf2D );
    glTexCoordPointer( 2,GL_FLOAT,0,uvf2D );

	setBlendMode( 3 );
	setColorTex_color(255);
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4,GL_FLOAT,0,_panelColorsTex);

//	hgio_panelcolor( colors, color, 255 );
	
//    glDisableClientState(GL_COLOR_ARRAY);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);

#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
	glDisable(GL_TEXTURE_2D);
#endif
}
#endif


void hgio_fontcopy( BMSCR *bm, float distx, float disty, float ratex, float ratey, int srcsx, int srcsy, int texid, int basex, int basey )
{
	sw_trc_ex( "fontcopy", bm, distx, disty, (float)srcsx, (float)srcsy, texid, basex, basey, (int)srcsx, (int)srcsy );
	//		画像コピー(フォント用)
	//		texid内の(xx,yy)-(xx+srcsx,yy+srcsy)を現在の画面に等倍でコピー
	//		描画モードは3,100%、転送先はdistx,disty
	//
    GLfloat *flp;
    GLfloat x1,y1,x2,y2;

    flp = vertf2D;
    x1 = (GLfloat)distx;
    y1 = (GLfloat)-disty;
    x2 = x1+srcsx;
    y2 = y1-srcsy;

    *flp++ = x1;
    *flp++ = y1;
    *flp++ = x1;
    *flp++ = y2;
    *flp++ = x2;
    *flp++ = y1;
    *flp++ = x2;
    *flp++ = y2;

    flp = uvf2D;
	x1 = (GLfloat)basex;
	y1 = (GLfloat)basey;
	x2 = (GLfloat)basex+srcsx;
	y2 = (GLfloat)basey+srcsy;

	x1 *= ratex;
	y1 *= ratey;
	x2 *= ratex;
	y2 *= ratey;

    *flp++ = x1;
    *flp++ = y1;
    *flp++ = x1;
    *flp++ = y2;
    *flp++ = x2;
    *flp++ = y1;
    *flp++ = x2;
    *flp++ = y2;

#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
	glEnable(GL_TEXTURE_2D);
#endif

	/*	texid here is an index into texinf[], not a GL name: the caller takes it
		from texmes::_texture, which hgio_fontsystem_setup() filled in with the
		value MakeEmptyTexBuffer() returned.  Binding it raw put an arbitrary GL
		name in the sampler - whichever texture happened to own that integer - so
		every string sampled a foreign image and was painted as opaque black bars
		even though its quad and UVs were right.  Resolve it the way the picture
		paths do.															*/
	{
		TEXINF *ftex = GetTex( texid );
		sw_bind_tex( ( ftex != NULL ) ? (int)ftex->texid : 0 );
	}
    glVertexPointer( 2, GL_FLOAT,0,vertf2D );
    glTexCoordPointer( 2,GL_FLOAT,0,uvf2D );

    //ブレンドモード設定
    int mode;
	if (GetSysReq(SYSREQ_FIXMESALPHA)) {
		mode = 2;
	} else {
		mode = bm->gmode;
	}
	setBlendMode( mode );

    if ( mode <= 1 ) {
        glDisableClientState(GL_COLOR_ARRAY);
	} else {
		GLfloat alpha;
	    if ( mode >= 3 ) {
			alpha = FVAL_BYTE1*( bm->gfrate );
		} else {
			alpha = 1.0f;
		}
		setColorTex_color(alpha);
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4,GL_FLOAT,0,_panelColorsTex);
    }

    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,_filter); 
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,_filter); 

    glDrawArrays(GL_TRIANGLE_STRIP,0,4);

#if defined(HSPLINUX) || defined(HSPEMSCRIPTEN)
	glDisable(GL_TEXTURE_2D);
#endif
}


/*	Set by gfdec/gfdec2 (src/hsp3/switch/dllshim_switch.cpp) for Elona's
	create_pcpic colour pass; consumed by hgio_copy() below.  Defined in
	src/hsp3dish/switch/gles1_shim.cpp.										*/
extern "C" {
extern int sw_fc_tint_on;
extern float sw_fc_tint_r, sw_fc_tint_g, sw_fc_tint_b;
}

/*	P3 DIAGNOSTIC - how many copies actually consumed an armed colour pass.
	Armed by gfdec/gfdec2, dropped unconditionally after every copy below, so
	a colour pass that reaches no copy at all is invisible without this.		*/
static int sw_tint_consume_trace = 0;

/*	P3 DIAGNOSTIC - create_pcpic lifts every tinted part back with a single
	128x198 copy (chips.hsp:118); no other copy in Elona is that shape.  Its
	presence, its tint state and whether it was skipped answer the whole
	question on their own.													*/
static int sw_pcc_copy_trace = 0;

/*	fcgraph colour pass, done in place.

	Hspext's gfini/gfdec/gfdec2/gfinc (src/plugins/win32/Hspext/fcgraph.cpp:41-160)
	do
	not draw anything: gfini locks an xsize x ysize rectangle whose top-left is
	the current screen position, and gfdec/gfdec2 walk that rectangle's pixels
	subtracting a per-channel amount, saturating at 0.  Elona leans on that for
	every PCC part - create_pcpic draws a greyscale template into a scratch
	strip, locks it, subtracts c_col and copies the strip back, which is what
	turns grey hair brown (chips.hsp:101-179) - and blend.hsp:1293-1295 calls
	gfdec2 with no copy afterwards at all.

	The earlier implementation recorded the colour and let the *next*
	hgio_copy apply it in the fragment shader.  That is wrong on both counts:
	a call site with no following copy lost the colour entirely, and an
	unrelated 16x16 tile copy often consumed the pending colour first - which
	is exactly how the strip came back grey and the eye disappeared.

	The subtraction now happens the moment gfdec is called: a solid quad over
	the locked rectangle, blended with GL_FUNC_REVERSE_SUBTRACT
	(dst = dst*1 - src*1), which the fixed-point colour buffer clamps to
	max(0, dst-src) on its own.  The vertex colour's alpha is 0, so the same
	equation leaves the destination's alpha untouched.

	gfinc (fcgraph.cpp:125-160) is the same walk with each channel ADDED and
	clamped at 255.  GL_FUNC_ADD with those same ( ONE, ONE ) factors is
	exactly that, so both directions share one function and differ only in
	the equation they arm.
							*/

static int		sw_fc_lock_x = 0;			/* gfini xsize (0 = whole width)	*/
static int		sw_fc_lock_y = 0;			/* gfini ysize (0 = whole height)	*/
static int		sw_fc_lock_px = 0;			/* lock top-left = pos at gfini		*/
static int		sw_fc_lock_py = 0;
static BMSCR	*sw_fc_lock_bm = NULL;		/* only meaningful on this screen	*/

extern "C" int sw_glBlendEquationAvailable( void );
extern "C" int sw_texture2d_on( void );

static int sw_fcsub_trace = 0;

extern "C" void sw_fcgraph_lock( int xs, int ys )
{
	/*	gfini: the lock is opened on the screen that is current right now,
		at the position the script set with `pos`.							*/
	BMSCR *bm = sw_cur;
	sw_fc_lock_bm = bm;
	sw_fc_lock_x = xs;
	sw_fc_lock_y = ys;
	sw_fc_lock_px = ( bm != NULL ) ? (int)bm->cx : 0;
	sw_fc_lock_py = ( bm != NULL ) ? (int)bm->cy : 0;
}

//	gfdec/gfdec2 (add == 0) and gfinc (add == 1) - one implementation, two
//	directions.  Only the blend equation and the zero case differ.
//
static void sw_fcgraph_tint( int r, int g, int b, int add )
{
	GLfloat vert[8];
	GLfloat cols[16];
	BMSCR *bm = sw_fc_lock_bm;
	int i, w, h;
	if ( SWITCH_DIAG && sw_fcsub_trace < 40 && ( sw_fc_lock_x >= 100 ) && ( sw_fc_lock_y >= 100 ) ) {
		sw_fcsub_trace++;
		if ( ( r <= 0 ) && ( g <= 0 ) && ( b <= 0 ) ) {
			sw_fbo_log( "hgio: fc%s #%d SKIP rgb<=0\n", add ? "inc" : "sub", sw_fcsub_trace );
		} else if ( ( bm == NULL ) || ( bm != sw_cur ) ) {
			sw_fbo_log( "hgio: fc%s #%d SKIP badbm lockbm=%p cur=%p\n", add ? "inc" : "sub", sw_fcsub_trace, (void *)bm, (void *)sw_cur );
		} else {
			sw_fbo_log( "hgio: fc%s #%d rgb=%d,%d,%d at %d,%d size=%dx%d\n", add ? "inc" : "sub", sw_fcsub_trace, r, g, b, sw_fc_lock_px, sw_fc_lock_py, sw_fc_lock_x, sw_fc_lock_y );
		}
	}
	float x1, y1, x2, y2, cr, cg, cb;
	int eq_ok;

	if ( ( r <= 0 ) && ( g <= 0 ) && ( b <= 0 ) ) {
		/*	Nothing to subtract, but the copy that follows still samples the
			texture the copy before it wrote.  Without the stall the subtracting
			path takes, the two parts whose tint is all zero - the eye and the
			cloth - come out missing from the PCC sheet.  A zero tint is a real
			case, not an error: the body uses it too.  An addition of zero
			changes no pixel and owes the next copy nothing, so only the
			subtraction stalls here. */
		if ( add == 0 ) glFinish();
		return;
	}
	if ( ( bm == NULL ) || ( bm != sw_cur ) ) return;		/* the lock died with its screen	*/
	/*	A subtraction has no fallback - without the equation it would brighten
		instead - while an addition can always lean on blendFunc( ONE, ONE ). */
	eq_ok = sw_glBlendEquationAvailable();
	if ( ( add == 0 ) && ( eq_ok == 0 ) ) return;

	w = sw_fc_lock_x; h = sw_fc_lock_y;
	if ( w <= 0 ) w = bm->sx;								/* gfini 0 = whole width	*/
	if ( h <= 0 ) h = bm->sy;
	if ( ( w <= 0 ) || ( h <= 0 ) ) return;

	cr = (float)r * ( 1.0f / 255.0f );
	cg = (float)g * ( 1.0f / 255.0f );
	cb = (float)b * ( 1.0f / 255.0f );
	x1 = (float)sw_fc_lock_px;
	y1 = (float)-sw_fc_lock_py;
	x2 = x1 + (float)w;
	y2 = y1 - (float)h;

	vert[0] = x1;	vert[1] = y1;
	vert[2] = x1;	vert[3] = y2;
	vert[4] = x2;	vert[5] = y1;
	vert[6] = x2;	vert[7] = y2;
	for ( i = 0; i < 4; i++ ) {
		cols[i*4+0] = cr;	cols[i*4+1] = cg;
		cols[i*4+2] = cb;	cols[i*4+3] = 0.0f;	/* alpha 0 keeps dst alpha	*/
	}

	/*	The texture unit has to be off for this one draw - the shim decides
		u_usetex from GL_TEXTURE_2D plus the client arrays, and nothing in
		Elona's copy path ever turns the array back on.  Leaving either off
		sent every later gcopy through with no texture at all, which painted
		the map solid white.  Save the flag, draw, put it back.			*/
	int tex2d_was = sw_texture2d_on();
	glDisable( GL_TEXTURE_2D );
	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, vert );
	glEnableClientState( GL_COLOR_ARRAY );
	glColorPointer( 4, GL_FLOAT, 0, cols );
	sw_glColorKey( 0, 0 );
	glEnable( GL_BLEND );
	if ( add == 0 ) {
		glBlendEquation( GL_FUNC_REVERSE_SUBTRACT );
	} else if ( eq_ok != 0 ) {
		glBlendEquation( GL_FUNC_ADD );			/* the default anyway	*/
	}
	glBlendFunc( GL_ONE, GL_ONE );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	/*	Restored: the gcopy that follows samples the very texture this quad
		just wrote, so the tint has to be out of the pipeline before
		the sample lands - the failure shows up as un-subtracted or missing
		PCC parts.  Eden renders on the host GPU too, so it needs the stall. */
	glFinish();
	if ( eq_ok != 0 ) glBlendEquation( GL_FUNC_ADD );
	if ( tex2d_was ) glEnable( GL_TEXTURE_2D );

	/*	Put the equation back; the blend function itself is restored by the
		next hgio_setTexBlendMode(), exactly like hgio_copy() does.			*/
	hgio_setTexBlendMode( bm, bm->gmode, bm->gfrate );

	/*	Nothing draws between here and the next command, so the key this
		hgio_setTexBlendMode() re-armed must not be left behind.			*/
	sw_glColorKey( 0, 0 );
}

/*	gfdec r,g,b / gfdec2 r,g,b - subtract from the rectangle gfini locked. */
extern "C" void sw_fcgraph_sub( int r, int g, int b )
{
	sw_fcgraph_tint( r, g, b, 0 );
}

/*	gfinc r,g,b - the saturated per-channel addition.  Elona tints every
	list highlight with gfdec -30,-10,0 followed by gfinc 50,50,50
	(module.hsp:173-265); while this one was a stub every selection in the
	game came out 50 short per channel, i.e. darker than the original. */
extern "C" void sw_fcgraph_add( int r, int g, int b )
{
	sw_fcgraph_tint( r, g, b, 1 );
}

void hgio_copy( BMSCR *bm, short xx, short yy, short srcsx, short srcsy, BMSCR *bmsrc, float s_psx, float s_psy )
{
	/*	r148 probe (log only, nothing is skipped): every copy that lands
		in the message band, with its source identified.  A one-row scroll
		would show up as bmsrc == bm (screen to itself).				*/
	if ( ( bm != NULL ) && ( bm->cy >= 500 ) && ( bm->cy <= 600 ) ) {
		static int sw_band_T_n = 0;
		if ( sw_band_T_n < 3000 ) {
			sw_band_T_n++;
			printf( "t46 copy t=%d dst=%.0f,%.0f size=%.0fx%.0f src=%d,%d srcdim=%dx%d bmsrc=%dx%d self=%d gmode=%d\n",
				(int)hgio_gettick(), (double)bm->cx, (double)bm->cy, s_psx, s_psy,
				(int)xx, (int)yy, (int)srcsx, (int)srcsy,
				( bmsrc != NULL ) ? bmsrc->sx : -1, ( bmsrc != NULL ) ? bmsrc->sy : -1,
				( bmsrc == bm ) ? 1 : 0, (int)bm->gmode );
			fflush( stdout );
		}
	}
	//		画像コピー
	//		texid内の(xx,yy)-(xx+srcsx,yy+srcsy)を現在の画面に(psx,psy)サイズでコピー
	//		カレントポジション、描画モードはBMSCRから取得
	//
	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

	TEXINF mtex;
	TEXINF *tex = sw_tex_src( bmsrc, &mtex );
	if ( SWITCH_DIAG && ( sw_fc_tint_on != 0 ) && ( sw_tint_consume_trace < 64 ) ) {
		sw_tint_consume_trace++;
		sw_fbo_log( "hgio: TINT armed -> src tx=%d %d,%d %dx%d dst tx=%d at %g,%g nostex=%d\n",
			( bmsrc != NULL ) ? bmsrc->texid : -99, (int)xx, (int)yy, (int)srcsx, (int)srcsy,
			bm->texid, (double)bm->cx, (double)bm->cy,
			( ( tex == NULL ) || ( tex->mode == TEXMODE_NONE ) ) ? 1 : 0 );
	}
	if ( SWITCH_DIAG && ( sw_pcc_copy_trace < 48 ) && ( srcsx == 128 ) && ( srcsy == 198 ) ) {
		sw_pcc_copy_trace++;
		sw_fbo_log( "hgio: PCCCOPY src tx=%d %d,%d -> dst tx=%d at %g,%g tint=%d gmode=%d nostex=%d\n",
			( bmsrc != NULL ) ? bmsrc->texid : -99, (int)xx, (int)yy,
			bm->texid, (double)bm->cx, (double)bm->cy, sw_fc_tint_on, bm->gmode,
			( ( tex == NULL ) || ( tex->mode == TEXMODE_NONE ) ) ? 1 : 0 );
	}
	if ( ( tex == NULL ) || ( tex->mode == TEXMODE_NONE ) ) {
		/*	t23 probe: Elona restores whole regions of a screen from picture
			buffers it keeps around (`gcopy BUFFER_MAP, ...`).  A source without
			a texture drops the copy in silence and the region then keeps
			whatever the last composition left on it.							*/
		if ( sw_copy_skip_report < 1200 ) {
			sw_copy_skip_report++;
			sw_fbo_log( "hgio: copy SKIP src bmsrc=%p texid=%d type=%d xy=%d,%d %dx%d -> dst tx=%d at %g,%g tick=%d\n",
				(void *)bmsrc, ( bmsrc != NULL ) ? bmsrc->texid : -99,
				( bmsrc != NULL ) ? bmsrc->type : -1,
				(int)xx, (int)yy, (int)srcsx, (int)srcsy,
				bm->texid, (double)bm->cx, (double)bm->cy, hgio_gettick() );
		}
		return;
	}

	sw_trc_ex( "copy", bm, (float)bm->cx, (float)bm->cy, s_psx, s_psy,
		bmsrc->texid, (int)xx, (int)yy, (int)srcsx, (int)srcsy );
	/*	t23 probe: remember every copy big enough to be a picture-buffer
		restore, and where it landed - the diag log fills up long before
		the trace does.														*/
	{
		long sw_copy_px = (long)srcsx * (long)srcsy;
		if ( ( sw_copy_big_report < 3000 ) && ( sw_copy_px >= 40000L ) ) {
			sw_copy_big_report++;
			sw_fbo_log( "hgio: copy BIG %dx%d src tx=%d %d,%d -> dst tx=%d at %g,%g gm=%d tick=%d\n",
				(int)srcsx, (int)srcsy, bmsrc->texid, (int)xx, (int)yy,
				bm->texid, (double)bm->cx, (double)bm->cy, bm->gmode, hgio_gettick() );
		}
	}

    GLfloat *flp;
    GLfloat x1,y1,x2,y2,tx0,tx1,ty0,ty1;
    float psx,psy,ratex,ratey;

    if ( s_psx < 0.0f ) {
        psx = -s_psx;
        tx1 = ((GLfloat)xx);
        tx0 = ((GLfloat)(xx+srcsx));
    } else {
        psx = s_psx;
        tx0 = ((GLfloat)xx);
        tx1 = ((GLfloat)(xx+srcsx));
    }
    if ( s_psy < 0.0f ) {
        psy = -s_psy;
        ty1 = ((GLfloat)yy);
        ty0 = ((GLfloat)(yy+srcsy));
    } else {
        psy = s_psy;
        ty0 = ((GLfloat)yy);
        ty1 = ((GLfloat)(yy+srcsy));
    }
    /*	t23: the window texture keeps screen row 0 at v=1 while an
    	offscreen screen keeps it at v=0 (see sw_apply_target), so a copy
    	that reads the window has to flip the source rows - otherwise the
    	character sheet stored into the picture buffer at chara.hsp:4183
    	comes back upside down when 4209 restores it.  Same story for
    	every other read-back from the window.                              */
    if ( ( bmsrc != NULL ) && ( bmsrc->type == HSPWND_TYPE_MAIN ) ) {
        float fh = (float)tex->sy;
        ty0 = fh - ty0;
        ty1 = fh - ty1;
    }
    
    flp = vertf2D;
    x1 = (GLfloat)bm->cx;
    y1 = (GLfloat)-bm->cy;
    x2 = x1+psx;
    y2 = y1-psy;
    
    *flp++ = x1;
    *flp++ = y1;
    *flp++ = x1;
    *flp++ = y2;
    *flp++ = x2;
    *flp++ = y1;
    *flp++ = x2;
    *flp++ = y2;

	if ( _uvfix ) {
        tx0 += 0.5f;
        ty0 += 0.5f;
        tx1 -= 0.5f;
        ty1 -= 0.5f;
	}
	sw_scratch_used = 0;
	/*	A gcopy whose source and destination are the same offscreen screen must
		be staged through the scratch texture - GL leaves a texture that is
		both sampled and rendered into undefined, the classic runtime copies
		row by row and has no such restriction.

		The 64x64 rule alone was not enough.  Elona draws its scene pictures
		with `gsel BUFFER_BUF / picload ...640x480 / pos 0,y1 /
		gzoom w,h, BUFFER_BUF, 0,0, 640,480` (scene.hsp) - a 640x480 copy from
		a screen onto itself at an offset.  That is larger than the tile rule,
		so it went through unstaged and the GPU read rows it was still writing:
		on hardware the picture comes out sheared along a diagonal.

		So the test is geometric now - same texture, and the source rectangle
		really does overlap the destination one.  The 64x64 rule is kept as
		well, because create_pcpic's 16x16 tiles are the case it was written
		for.  p3s195's mistake was raising the size limit outright: that also
		routed create_pcpic's 128x198 parts (128,0->256,0 and 256,0->0,0, which
		do NOT overlap) through the single shared scratch, changing how often
		each layer landed on the screen - which whitened the whole map.		*/
	int sw_ov;
	{
		int sx0 = (int)xx, sy0 = (int)yy;
		int dx0 = (int)bm->cx, dy0 = (int)bm->cy;
		/*	A copy that lands exactly where it came from changes nothing on
			the direct path, and Elona uses one to "refresh" a screen - the
			title screen does a full 800x600 onto itself.  Staging that would
			read the screen while it is still being drawn and paint the result
			back; on hardware it came out black.  Only a PARTIAL overlap is a
			real hazard, so an exact self-copy stays on the direct path. */
		int sw_same = ( sx0 == dx0 ) && ( sy0 == dy0 ) &&
				( (int)srcsx == (int)psx ) && ( (int)srcsy == (int)psy );
		sw_ov = ( sw_same == 0 ) &&
				( sx0 < ( dx0 + (int)psx ) ) && ( dx0 < ( sx0 + (int)srcsx ) ) &&
				( sy0 < ( dy0 + (int)psy ) ) && ( dy0 < ( sy0 + (int)srcsy ) );	}
	/*	r152: r150 tried to let the main screen stage its overlapping
		self-copies and wiped the message log instead, so the main screen
		is excluded again.  Whatever fixes the band has to work without
		relying on the main screen's texture holding live pixels.			*/
	/*	r153: the main screen's overlapping self-copy is the message log
		scrolling (src 532 -> dst 517, 664x45).  Stage it by reading the
		live framebuffer back - r150 showed a texture sample comes out
		blank here and erases the band.									*/
	if ( ( bmsrc == bm ) && ( bm->type == HSPWND_TYPE_MAIN ) && ( sw_ov != 0 ) ) {
		int sw_rb = sw_scratch_readback( (int)xx, (int)yy, (int)srcsx, (int)srcsy, bm );
		static int sw_main_rb_n = 0;
		if ( sw_main_rb_n < 200 ) {
			sw_main_rb_n++;
			printf( "t51 main-readback %d,%d %dx%d -> %g,%g ret=%d\n",
				(int)xx, (int)yy, (int)srcsx, (int)srcsy,
				(double)bm->cx, (double)bm->cy, sw_rb );
			fflush( stdout );
		}
		if ( sw_rb == 0 ) {
			sw_scratch_used = 1;
			tx0 = 0.0f;
			ty0 = 0.0f;
			tx1 = (GLfloat)srcsx;
			ty1 = (GLfloat)srcsy;
			ratex = 1.0f / (float)sw_scratch_w;
			ratey = 1.0f / (float)sw_scratch_h;
		}
	} else if ( ( bm->texid == bmsrc->texid ) && ( bm->type != HSPWND_TYPE_MAIN ) &&
		 ( ( ( srcsx <= 64 ) && ( srcsy <= 64 ) ) || sw_ov ) ) {
		int scret = sw_scratch_capture( (GLuint)tex->texid, tex->ratex, tex->ratey,
				(int)xx, (int)yy, (int)srcsx, (int)srcsy );
		if ( sw_selfblit_log < 24 ) {
			sw_selfblit_log++;
			sw_fbo_log( "hgio: selfblit %d,%d %dx%d -> %g,%g gmode=%d tex=%d ret=%d "
				"scratch=%dx%d err=0x%x\n",
				(int)xx, (int)yy, (int)srcsx, (int)srcsy, (double)psx, (double)psy,
				bm->gmode, bm->texid, scret, sw_scratch_w, sw_scratch_h,
				(unsigned)sw_glGetError() );
		}
		if ( scret == 0 ) {
			/*	sw_bind_target() may return early when sw_cur already names this
				screen, which would leave the scratch bound and every later draw
				going into it.  Restore the target outright. */
			{
				SWTARGET *st2 = sw_find( bm );
				if ( st2 != NULL ) {
					sw_bfb(  st2->fbo );
					sw_apply_target( bm );
				}
			}
			sw_scratch_used = 1;
			tx0 = 0.0f;
			ty0 = 0.0f;
			tx1 = (GLfloat)srcsx;
			ty1 = (GLfloat)srcsy;
			ratex = 1.0f / (float)sw_scratch_w;
			ratey = 1.0f / (float)sw_scratch_h;
		}
	}
	if ( !sw_scratch_used ) {
		ratex = tex->ratex;
		ratey = tex->ratey;
	}

    flp = uvf2D;

    tx0 *= ratex;
    ty0 *= ratey;
    tx1 *= ratex;
    ty1 *= ratey;
    
    *flp++ = tx0;
    *flp++ = ty0;
    *flp++ = tx0;
    *flp++ = ty1;
    *flp++ = tx1;
    *flp++ = ty0;
    *flp++ = tx1;
    *flp++ = ty1;

	sw_bind_tex( sw_scratch_used ? (int)sw_scratch_tex : tex->texid );
    glVertexPointer( 2, GL_FLOAT,0,vertf2D );
    glTexCoordPointer( 2,GL_FLOAT,0,uvf2D );

	hgio_setTexBlendMode( bm, bm->gmode, bm->gfrate );
	glDrawArrays(GL_TRIANGLE_STRIP,0,4);

	/*	The colour key hgio_setTexBlendMode() just armed belongs to this one
		draw.  Left set, it also keyed the next draw that never asked for one,
		so a plain boxf clear discarded every pixel it painted - Elona's
		create_pcpic wipes its 384x198 PCC buffer that way and the buffer kept
		a stray cyan rectangle behind the character.						*/
	sw_glColorKey( 0, 0 );

	/*	An fcgraph colour pass armed by gfdec/gfdec2 is consumed by exactly
		this copy - the one that lifts the tinted scratch strip back into
		place.  Drop it so the next draw comes out uncoloured.				*/
	sw_fc_tint_on = 0;

	/*	t23 probe: the p3s155 probe armed on `bm->type == HSPWND_TYPE_MAIN`
		and never fired, although the copy is logged with the right rectangle
		every frame as `700x400 src tx=8 ... dst tx=-1`.  The window is the
		only screen with texid -1, so `dst tx=-1` has to be a *different*
		screen whose texture was released by delscreen and never re-made -
		which would compose the sheet into something nothing shows.  Arm on
		the copy geometry alone and name the destination instead of assuming
		it: screen, type, texid, size, sw_cur, the framebuffer bound and the
		one sw_find() resolves, source and its texture, sw_main_ok, blend
		mode, uv rectangle, plus three pixels read straight back inside the
		sheet's rectangle (50..750 x 86..486).  Four copies are kept, which
		covers the single gmode 6 pass at the panel origin and the repeating
		gmode 2 pass beside it.												*/
	if ( SWITCH_DIAG && ( sw_panel_a < 4 ) && ( srcsx == 700 ) && ( srcsy == 400 ) ) {
		unsigned char pt[3][4];
		int k;
		SWTARGET *ptgt = sw_find( bm );
		unsigned dfo = (unsigned)sw_real_fbo;
		int dw = (int)bm->sx;
		int dh = (int)bm->sy;
		if ( dw <= 0 ) dw = 800;
		if ( dh <= 0 ) dh = 600;
		if ( dh < 301 ) dh = 301;
		sw_panel_a++;
		if ( sw_panel_a == 1 ) sw_panel_b = 1;
		for ( k = 0; k < 3; k++ ) {
			pt[k][0] = pt[k][1] = pt[k][2] = pt[k][3] = 0;
		}
		glReadPixels( 100, dh - 301, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pt[0] );
		glReadPixels( 300, dh - 301, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pt[1] );
		glReadPixels( 600, dh - 301, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pt[2] );
		sw_fbo_log( "hgio: panel probe #%d bm=%p type=%d tx=%d sx=%d sy=%d"
			" cur=%p curtype=%d rfbo=%u tfbo=%u ttx=%d src=%p srctx=%d stex=%u"
			" mainok=%d gm=%d rt=%d uv=%g,%g-%g,%g"
			" p100=%d,%d,%d p300=%d,%d,%d p600=%d,%d,%d tick=%d\n",
			sw_panel_a, (void *)bm, bm->type, bm->texid, (int)bm->sx, (int)bm->sy,
			(void *)sw_cur, ( sw_cur != NULL ) ? sw_cur->type : -1,
			(unsigned)sw_real_fbo, ( ptgt != NULL ) ? (unsigned)ptgt->fbo : 0u,
			( ptgt != NULL ) ? ptgt->texid : -99,
			(void *)bmsrc, bmsrc->texid, (unsigned)tex->texid,
			sw_main_ok, bm->gmode, bm->gfrate,
			(double)uvf2D[0], (double)uvf2D[1],
			(double)uvf2D[4], (double)uvf2D[5],
			pt[0][0], pt[0][1], pt[0][2], pt[1][0], pt[1][1], pt[1][2],
			pt[2][0], pt[2][1], pt[2][2], hgio_gettick() );
		if ( dfo != 0 ) {
			char pnm[32];
			sprintf( pnm, "panel_%d.bmp", sw_panel_a );
			sw_dump_fbo( pnm, dfo, dw, dh );
			sw_bfb(  dfo );
		}
	}
}


void hgio_copyrot( BMSCR *bm, short xx, short yy, short srcsx, short srcsy, float s_ofsx, float s_ofsy, BMSCR *bmsrc, float psx, float psy, float ang )
{
	//		画像コピー
	//		texid内の(xx,yy)-(xx+srcsx,yy+srcsy)を現在の画面に(psx,psy)サイズでコピー
	//		カレントポジション、描画モードはBMSCRから取得
	//
	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;
	sw_trc_ex( "copyrot", bm, (float)bm->cx, (float)bm->cy, psx, psy,
		( bmsrc != NULL ) ? bmsrc->texid : -9, (int)xx, (int)yy, (int)srcsx, (int)srcsy );

	TEXINF mtex;
	TEXINF *tex = sw_tex_src( bmsrc, &mtex );
	if ( ( tex == NULL ) || ( tex->mode == TEXMODE_NONE ) ) return;

    GLfloat *flp;
    float ratex,ratey;

	int texpx,texpy,texid;
	GLfloat x,y,x0,y0,x1,y1,ofsx,ofsy,mx0,mx1,my0,my1;
	GLfloat tx0,ty0,tx1,ty1,sx,sy;

    //Alertf( "(%d,%d)(%d,%d)(%f,%f)",xx,yy,srcsx,srcsy,psx,psy );
    
	mx0=-(float)sin( ang );
	my0=(float)cos( ang );
	mx1 = -my0;
	my1 = mx0;
    
	ofsx = -s_ofsx;
	ofsy = -s_ofsy;
	x0 = mx0 * ofsy;
	y0 = my0 * ofsy;
	x1 = mx1 * ofsx;
	y1 = my1 * ofsx;
    
	//		基点の算出
	x = ( (float)bm->cx - (-x0+x1) );
	y = ( (float)bm->cy - (-y0+y1) );
    
	/*-------------------------------*/
    
	//		回転座標の算出
	ofsx = -psx;
	ofsy = -psy;
	x0 = mx0 * ofsy;
	y0 = my0 * ofsy;
	x1 = mx1 * ofsx;
	y1 = my1 * ofsx;
    
	/*-------------------------------*/
    
	sx = tex->ratex;
	sy = tex->ratey;
    //sx = 1.0f / image.width;
    //sy = 1.0f / image.height;
	texpx = xx + srcsx;
	texpy = yy + srcsy;
    
	tx0 = ((float)xx) * sx;
	ty0 = ((float)yy) * sy;
	tx1 = ((float)(texpx)) * sx;
	ty1 = ((float)(texpy)) * sy;

    flp = uvf2D;
    *flp++ = tx0;
    *flp++ = ty0;
    *flp++ = tx0;
    *flp++ = ty1;
    *flp++ = tx1;
    *flp++ = ty0;
    *flp++ = tx1;
    *flp++ = ty1;

	/*-------------------------------*/

    flp = vertf2D;
    
	*flp++ = (x);
	*flp++ = -(y);
    
	/*-------------------------------*/

	*flp++ = ((-x0) + x);
	*flp++ = -((-y0) + y);
    
	/*-------------------------------*/
    
	*flp++ = ((x1) + x);
	*flp++ = -((y1) + y);
    
	/*-------------------------------*/

	*flp++ = ((-x0+x1) + x);
	*flp++ = -((-y0+y1) + y);
    
	/*-------------------------------*/
    
	sw_bind_tex( tex->texid );
    //glBindTexture(GL_TEXTURE_2D,image.name);

    glVertexPointer(2,GL_FLOAT,0,vertf2D);
    glTexCoordPointer(2,GL_FLOAT,0,uvf2D);

	hgio_setTexBlendMode( bm, bm->gmode, bm->gfrate );
//    glDisableClientState(GL_COLOR_ARRAY);

	/*	The appearance editor's preview is a grotate from the PCC buffer the
		script composed earlier in this very frame (chips.hsp: picload +
		gcopy + gfdec2, of which only gfdec2 had a sync point).  The GPU runs
		that composition asynchronously, so without a wait here the sample
		can still see the buffer as it was before the part was redrawn - the
		"figure never follows the part" symptom on hardware, which the
		emulator's in-order software path hides.  This costs nothing on a
		frame that only moves the character, because a PCC is rebuilt only
		when the appearance changes.									*/
	glFinish();
	glDrawArrays(GL_TRIANGLE_STRIP,0,4);

	/*	The colour key hgio_setTexBlendMode() just armed belongs to this one
		draw.  Left set, it also keyed the next draw that never asked for one,
		so a plain boxf clear discarded every pixel it painted - Elona's
		create_pcpic wipes its 384x198 PCC buffer that way and the buffer kept
		a stray cyan rectangle behind the character.						*/
	sw_glColorKey( 0, 0 );
}


void hgio_square_tex( BMSCR *bm, int *posx, int *posy, BMSCR *bmsrc, int *uvx, int *uvy )
{
	if ( ( posx != NULL ) && ( posy != NULL ) ) sw_trc( "squareT", bm, (float)posx[0], (float)posy[0], (float)posx[2], (float)posy[2] );
	//		四角形(square)テクスチャ描画
	//
	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

	TEXINF mtex;
	TEXINF *tex = sw_tex_src( bmsrc, &mtex );
	if ( ( tex == NULL ) || ( tex->mode == TEXMODE_NONE ) ) return;

    GLfloat *flp;
    float sx,sy;

	sx = tex->ratex;
	sy = tex->ratey;

    flp = uvf2D;
    *flp++ = ((float)uvx[0]) * sx;
    *flp++ = ((float)uvy[0]) * sy;
    *flp++ = ((float)uvx[3]) * sx;
    *flp++ = ((float)uvy[3]) * sy;
    *flp++ = ((float)uvx[1]) * sx;
    *flp++ = ((float)uvy[1]) * sy;
    *flp++ = ((float)uvx[2]) * sx;
    *flp++ = ((float)uvy[2]) * sy;

    flp = vertf2D;
	*flp++ = (float)posx[0];
	*flp++ = (float)-posy[0];
	*flp++ = (float)posx[3];
	*flp++ = (float)-posy[3];
	*flp++ = (float)posx[1];
	*flp++ = (float)-posy[1];
	*flp++ = (float)posx[2];
	*flp++ = (float)-posy[2];

	sw_bind_tex( tex->texid );
    //glBindTexture(GL_TEXTURE_2D,image.name);

    glVertexPointer(2,GL_FLOAT,0,vertf2D);
    glTexCoordPointer(2,GL_FLOAT,0,uvf2D);

	hgio_setTexBlendMode( bm, bm->gmode, bm->gfrate );
    //glDisableClientState(GL_COLOR_ARRAY);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);

	/*	The colour key hgio_setTexBlendMode() just armed belongs to this one
		draw.  Left set, it also keyed the next draw that never asked for one,
		so a plain boxf clear discarded every pixel it painted - Elona's
		create_pcpic wipes its 384x198 PCC buffer that way and the buffer kept
		a stray cyan rectangle behind the character.						*/
	sw_glColorKey( 0, 0 );
}


void hgio_square( BMSCR *bm, int *posx, int *posy, int *color )
{
	if ( ( posx != NULL ) && ( posy != NULL ) ) sw_trc( "square", bm, (float)posx[0], (float)posy[0], (float)posx[2], (float)posy[2] );
	//		四角形(square)単色描画
	//
    GLfloat *flp;

	if ( bm == NULL ) return;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

    flp = vertf2D;

	*flp++ = (float)posx[0];
	*flp++ = (float)-posy[0];
	*flp++ = (float)posx[3];
	*flp++ = (float)-posy[3];
	*flp++ = (float)posx[1];
	*flp++ = (float)-posy[1];
	*flp++ = (float)posx[2];
	*flp++ = (float)-posy[2];

	ChangeTex( -1 );

    glVertexPointer(2,GL_FLOAT,0,vertf2D);

	setBlendMode( bm->gmode );

	GLfloat a = bm->gmode < 3 ? 1.0f : FVAL_BYTE1*(bm->gfrate&0xff);
	GLfloat colors[16]={
		RGBA2R( color[0] ), RGBA2G( color[0] ), RGBA2B( color[0] ), a,
		RGBA2R( color[3] ), RGBA2G( color[3] ), RGBA2B( color[3] ), a,
		RGBA2R( color[1] ), RGBA2G( color[1] ), RGBA2B( color[1] ), a,
		RGBA2R( color[2] ), RGBA2G( color[2] ), RGBA2B( color[2] ), a
	};

	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4,GL_FLOAT,0,colors);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}


int hgio_celputmulti( BMSCR *bm, int *xpos, int *ypos, int *cel, int count, BMSCR *bmsrc )
{
	sw_trc( "celputm", bm, 0.0f, 0.0f, 0.0f, 0.0f );
	//		マルチ画像コピー
	//		int配列内のX,Y,CelIDを元に等倍コピーを行なう(count=個数)
	//		カレントポジション、描画モードはBMSCRから取得
	//
	int psx,psy;
	float f_psx,f_psy;
	int i;
	int id;
	int *p_xpos;
	int *p_ypos;
	int *p_cel;
	int xx,yy;
	int total;

	if ( bm == NULL ) return 0;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

	total =0;

	p_xpos = xpos;
	p_ypos = ypos;
	p_cel = cel;

	psx = bmsrc->divsx;
	psy = bmsrc->divsy;
	f_psx = (float)psx;
	f_psy = (float)psy;

	for(i=0;i<count;i++) {

		id = *p_cel;

		if ( id >= 0 ) {

			xx = ( id % bmsrc->divx ) * psx;
			yy = ( id / bmsrc->divx ) * psy;

			bm->cx = *p_xpos;
			bm->cy = *p_ypos;

			hgio_copy( bm, xx, yy, psx, psy, bmsrc, f_psx, f_psy );

			total++;
		}

		p_xpos++;
		p_ypos++;
		p_cel++;

	}

	return total;
}

/*-------------------------------------------------------------------------------*/

#if defined(HSPLINUX) || defined(HSPNDK) || defined(HSPEMSCRIPTEN)
    static time_t basetick;
    static bool tick_reset = false;
#endif

int hgio_gettick( void )
{
    // 経過時間の計測

#if defined(HSPLINUX) || defined(HSPNDK) || defined(HSPEMSCRIPTEN)
	int i;
	timespec ts;
	double nsec;
    clock_gettime(CLOCK_REALTIME,&ts);
    nsec = (double)(ts.tv_nsec) * 0.001 * 0.001;
    //i = (int)ts.tv_sec * 1000 + (int)nsec;
    time_t sec = ts.tv_sec;
    if ( tick_reset ) {
	sec -= basetick;
    } else {
	tick_reset = true;
	basetick = sec;
	sec = 0;
     }
    i =((int)sec) * 1000 + (int)nsec;
    return i;
#endif

#ifdef HSPIOS
    CFAbsoluteTime now;
    now = CFAbsoluteTimeGetCurrent();
    total_tick += now - lastTime;
    lastTime = now;
    return (int)(total_tick * 1000.0 );
#endif

}

int hgio_dialog( int mode, char *str1, char *str2 )
{
#ifdef HSPNDK
	j_dispDialog( str1, str2, mode );
#endif
#ifdef HSPIOS
    gb_dialog( mode, str1, str2 );
    //Alertf( str1 );
#endif
#ifdef HSPLINUX
	{
	int i = 0;
	/*	The Switch has no window-system message box - SDL_ShowSimpleMessageBox
		lands nowhere here.  Elona announces real decisions through dialog()
		(*world_init refuses a save with "invalid version" or "The adventurer
		is not revived again...", then ends the script), so with the text gone
		an intentional exit is indistinguishable from a crash.  Keep a copy in
		the log. */
	printf( "hsp3switch: dialog mode=%d: %s%s%s\n", mode,
		( str1 != NULL ) ? str1 : "",
		( ( str1 != NULL ) && ( str2 != NULL ) ) ? " / " : "",
		( str2 != NULL ) ? str2 : "" );
	fflush( stdout );
	if (mode>=16) return 0;
	if (mode&1) i|=SDL_MESSAGEBOX_WARNING; else i|=SDL_MESSAGEBOX_INFORMATION;
	SDL_ShowSimpleMessageBox(i, str2, str1, NULL);
	}
#endif
#ifdef HSPEMSCRIPTEN
	EM_ASM_({
		alert(UTF8ToString($0));
	},str1 );
#endif
	return 0;
}

/*-------------------------------------------------------------------------------*/

void hgio_scale_point( int xx, int yy, int &x, int & y )
{
	x = ( xx - _originX ) * _rateX;
	y = ( yy - _originY ) * _rateY;
}

void hgio_touch( int xx, int yy, int button )
{
    Bmscr *bm;
	hgio_scale_point( xx,yy,mouse_x,mouse_y );
	hgio_cnvview( mainbm, &mouse_x, &mouse_y );
	mouse_btn = button;
    if ( mainbm != NULL ) {
        mainbm->savepos[BMSCR_SAVEPOS_MOSUEX] = mouse_x;
        mainbm->savepos[BMSCR_SAVEPOS_MOSUEY] = mouse_y;
        mainbm->tapstat = button!=0;
        bm = (Bmscr *)mainbm;
        bm->UpdateAllObjects();
		HSP3MTOUCH *mt;
		bool notice = false;
		if (button!=0) {
			mt = bm->getMTouchByPointId(-1);
			if (mt==NULL) {
				mt = bm->getMTouch(0);
				if (mt->flag == 0) notice=true;
			} else {
				notice=true;
			}
		} else {
			notice=true;
		}
		if (notice) {
	        bm->setMTouchByPointId( -1, mouse_x, mouse_y, button!=0 );
		}
    }
}

void hgio_mtouch( int old_x, int old_y, int xx, int yy, int button, int opt )
{
    Bmscr *bm;
    int x,y,old_x2,old_y2;
    if ( mainbm == NULL ) return;
    bm = (Bmscr *)mainbm;
    hgio_scale_point( xx,yy,x,y );
    hgio_cnvview( mainbm, &mouse_x, &mouse_y );

    if ( opt == 0) {
        mouse_x = x;
        mouse_y = y;
        mouse_btn = button;
        mainbm->savepos[BMSCR_SAVEPOS_MOSUEX] = mouse_x;
        mainbm->savepos[BMSCR_SAVEPOS_MOSUEY] = mouse_y;
        mainbm->tapstat = button!=0;
        bm->UpdateAllObjects();
    }
    if ( old_x >= 0 ) {
        old_x2 = ( old_x - _originX ) * _rateX;
    } else {
        old_x2 = old_x;
    }
    if ( old_y >= 0 ) {
        old_y2 = ( old_y - _originY ) * _rateY;
    } else {
        old_y2 = old_y;
    }
    bm->setMTouchByPoint( old_x2, old_y2, x, y, button!=0 );
}

void hgio_mtouchid( int pointid, int xx, int yy, int button, int opt )
{
    Bmscr *bm;
    int x,y;
    if ( mainbm == NULL ) return;
    bm = (Bmscr *)mainbm;
	x = ( xx - _originX ) * _rateX;
	y = ( yy - _originY ) * _rateY;
    if ( opt == 0 ) {
        mouse_x = x;
        mouse_y = y;
        mouse_btn = button;
        mainbm->savepos[BMSCR_SAVEPOS_MOSUEX] = mouse_x;
        mainbm->savepos[BMSCR_SAVEPOS_MOSUEY] = mouse_y;
        mainbm->tapstat = button!=0;
        bm->UpdateAllObjects();
    }
    bm->setMTouchByPointId( pointid, x, y, button!=0 );
}

int hgio_getmousex( void )
{
	return mouse_x;
}


int hgio_getmousey( void )
{
	return mouse_y;
}


int hgio_getmousebtn( void )
{
	return mouse_btn;
}

/*-------------------------------------------------------------------------------*/

void hgio_test(void)
{
    // 描画する
    hgio_render_start();
    //hgio_clear();
    
	//hgio_setColor( 0xff00ff );
	hgio_boxfill( 100.1,100.2,200.5,50.2 );
	//hgio_boxfill( 100,200,100,10 );
	//hgio_setColor( 0xffffff );
	//hgio_line( 0,0,400,300 );
	//hgio_setColor( 0xffff00 );
	//hgio_circleFill( 640,400,200,200 );

	//hgio_putTexFont( 0,0, (char *)"This is Android Test." );
	//hgio_fcopy( 0,0,  0, 0, 256, 128, font_texid );

    hgio_render_end();
}


int hgio_font(char *fontname, int size, int style)
{
	//		文字フォント指定
	//
	tmes.setFont(fontname, size, style);
	return 0;
}

static unsigned sw_text_hash( const char *s )
{
	unsigned h = 5381;
	while ( *s != 0 ) h = h * 33u + (unsigned char)*s++;
	return h;
}
/*-------------------------------------------------------------------------------*/
/*
		r104: drop the spurious second copy of a story line

		One line of Elona's story reader arrives twice at hgio_mes: the clean
		copy first, then a copy still carrying its line-ending CR that lands a
		row or two lower - that shifted second copy is what doubles the text
		on screen.  The register cache compares strings with strcmp, so the CR
		copy never matches the clean one, and it gets its own texture and its
		own draw.

		Only a repeat within a few milliseconds of the first can be that copy:
		the ordinary redraw repeats the whole block every frame (tens of ms
		apart), and the outline/shadow passes sit on the same spot (x differing
		by a pixel, y by one).  So a CR-terminated string whose clean text was
		drawn less than 5ms ago at the same x and 6..48px higher is skipped -
		the row advance is still applied so the rest of the block stays put.
*/
/*-------------------------------------------------------------------------------*/
#define SWMES_CLEAN_MAX 512
#define SWMES_DRAWN_KEEP 64

typedef struct {
	void		*bm;
	int		x, y;
	unsigned	tick;
	unsigned	hash;
} SWMESDRAWN;

static SWMESDRAWN sw_mes_drawn[SWMES_DRAWN_KEEP];
static int sw_mes_drawn_ix = 0;

/*	Copy msg, trimming any line-ending CR/LF from the tail.  *had_cr tells
	whether the original ended with one (that is the spurious-copy marker),
	*ok says the string fit the buffer and can be used as the clean text.	*/
static void sw_mes_clean( char *dst, const char *msg, int *had_cr, int *ok )
{
	int n = (int)strlen( msg );
	int i;
	*had_cr = 0;
	*ok = 0;
	if ( n >= SWMES_CLEAN_MAX ) return;
	memcpy( dst, msg, n + 1 );
	*ok = 1;
	for ( i = n; i > 0; i-- ) {
		char c = dst[i-1];
		if ( c != 0x0D && c != 0x0A ) break;
		*had_cr = 1;
		dst[i-1] = 0;
	}
}

static void sw_mes_note_draw( BMSCR *bm, int x, int y, const char *s )
{
	sw_mes_drawn[sw_mes_drawn_ix].bm = (void *)bm;
	sw_mes_drawn[sw_mes_drawn_ix].x = x;
	sw_mes_drawn[sw_mes_drawn_ix].y = y;
	sw_mes_drawn[sw_mes_drawn_ix].tick = (unsigned)hgio_gettick();
	sw_mes_drawn[sw_mes_drawn_ix].hash = sw_text_hash( s );
	sw_mes_drawn_ix = ( sw_mes_drawn_ix + 1 ) % SWMES_DRAWN_KEEP;
}

int hgio_mes(BMSCR* bm, char* msg)
{
	/*	r149: text draws that land in the message band.				*/
	if ( ( bm != NULL ) && ( bm->cy >= 500.0f ) && ( bm->cy <= 610.0f ) ) {
		static int sw_band_M_n = 0;
		if ( sw_band_M_n < 1500 ) {
			sw_band_M_n++;
			printf( "t47 mes t=%d x=%.0f y=%.0f n=%d\n",
				(int)hgio_gettick(), (double)bm->cx, (double)bm->cy,
				( msg != NULL ) ? (int)strlen( msg ) : -1 );
			fflush( stdout );
		}
	}
	//		mes,print 文字表示
	//
	int xsize, ysize;
	char clean[SWMES_CLEAN_MAX];
	int had_cr, clean_ok;
	const char *use;

	if ( !sw_drawable( bm ) ) return -1;
	if (drawflag == 0) hgio_render_start();

	// print per line
	if (bm->vp_flag == BMSCR_VPFLAG_NOUSE) {
		if (bm->cy >= bm->sy) return -1;
	}

	if (*msg == 0) {
		ysize = tmes._fontsize;
		bm->printsizey += ysize;
		bm->cy += ysize;
		return 0;
	}

	/*	fork: a mes can arrive carrying its own line breaks.  Upstream leaves
		the splitting to PrintSub, but on the 2.30 build a wrapped block
		reached this function with an embedded CR/LF (seen as a 126-byte
		segment holding two lines): the renderer then drew only up to the
		break and advanced a single row, so the rest of the panel shifted up
		and read as overlapping text.  Split it here as well - every line in
		the string is drawn and its row advanced, whatever the caller did.	*/
	{
		const char *q = msg;
		while ( *q != 0 ) {
			if ( *q == '\r' || *q == '\n' ) break;
			q++;
		}
		if ( *q != 0 ) {
			char seg[SWMES_CLEAN_MAX];
			const char *s = msg;
			int rc = 0;
			while ( *s != 0 ) {
				const char *e = s;
				int n;
				while ( *e != 0 && *e != '\r' && *e != '\n' ) e++;
				n = (int)( e - s );
				if ( n > SWMES_CLEAN_MAX - 1 ) n = SWMES_CLEAN_MAX - 1;
				memcpy( seg, s, n );
				seg[n] = 0;
				if ( hgio_mes( bm, seg ) < 0 ) rc = -1;
				s = e;
				if ( *s == '\r' ) s++;
				if ( *s == '\n' ) s++;
			}
			return rc;
		}
	}
	sw_mes_clean( clean, msg, &had_cr, &clean_ok );
	use = ( clean_ok && clean[0] != 0 ) ? clean : msg;

	/*	r104: skip the shifted CR-terminated second copy			*/
	if ( had_cr && clean_ok && clean[0] != 0 ) {
		unsigned h = sw_text_hash( use );
		unsigned tick = (unsigned)hgio_gettick();
		int i;
		for ( i = 0; i < SWMES_DRAWN_KEEP; i++ ) {
			int dy;
			if ( sw_mes_drawn[i].bm != (void *)bm ) continue;
			if ( sw_mes_drawn[i].hash != h ) continue;
			if ( sw_mes_drawn[i].x != bm->cx ) continue;
			dy = bm->cy - sw_mes_drawn[i].y;
			if ( dy < 6 || dy > 48 ) continue;
			if ( tick - sw_mes_drawn[i].tick > 5 ) continue;
			ysize = tmes._fontsize;
			bm->printsizey += ysize;
			bm->cy += ysize;
			return 0;
		}
	}

	int id;
	texmes* tex;
	id = tmes.texmesRegist((char *)use);
	if (id < 0) return -1;
	tex = tmes.texmesGet(id);
	if (tex == NULL) return -1;

	xsize = tex->sx;
	ysize = tex->sy;

	if (bm->printoffsetx > 0) {			// センタリングを行う(X)
		int offset = (bm->printoffsetx - xsize) / 2;
		if (offset > 0) {
			bm->cx += offset;
		}
		bm->printoffsetx = 0;
	}
	if (bm->printoffsety > 0) {			// センタリングを行う(Y)
		int offset = (bm->printoffsety - ysize) / 2;
		if (offset > 0) {
			bm->cy += offset;
		}
		bm->printoffsety = 0;
	}

	{
		/*	2026-10-06: the Chinese 2.30 translation draws the tail rows of
			the race description into the skill column of the reference panel.
			The same overlap happens on the PC build of that translation, so it
			is the translation's data, not this port.  A real skill row is
			"<name><padding><description>" and so contains a run of spaces; the
			stray rows are plain sentences.  Only the blit is skipped - every
			step before it still runs, so nothing else changes.			*/
		int sw_hide = 0;
		if ( bm->cx >= 286 && bm->cx <= 298 && bm->cy >= 385 && bm->cy <= 520 ) {
			const char *sp = use;
			int padded = 0;
			while ( sp[0] != 0 && sp[1] != 0 ) {
				if ( sp[0] == ' ' && sp[1] == ' ' ) { padded = 1; break; }
				sp++;
			}
			sw_hide = !padded;
		}
		if ( !sw_hide ) {
			hgio_fontcopy(bm, bm->cx, bm->cy, tex->ratex, tex->ratey, xsize, ysize, tex->_texture, 0, 0);
		}
		sw_mes_note_draw( bm, bm->cx, bm->cy, use );
	}

	if (xsize > bm->printsizex) bm->printsizex = xsize;
	bm->printsizey += ysize;
	bm->cy += ysize;
	return 0;
}


int hgio_mestex(BMSCR *bm, texmesPos *tpos)
{
	//		TEXMESPOSによる文字表示
	//
	int mode, x, y, sx, sy;
	int orgx, orgy;
	int tx, ty;
	int xsize, ysize;
	int esx, esy;
	if ( !sw_drawable( bm ) ) return -1;
	if (drawflag == 0) hgio_render_start();

	// print per line
	orgx = bm->cx;
	orgy = bm->cy;
	mode = tpos->mode;

	sx = tpos->sx;
	if (sx <= 0) {
		sx = bm->sx - orgx;
		if (sx <= 0) return -1;
	}
	sy = tpos->sy;
	if (sy <= 0) {
		sy = bm->sy - orgy;
		if (sy <= 0) return -1;
	}

	int id = tpos->texid;
	if (id < 0) {
		char *str = tpos->getString();
		id = tmes.texmesRegist(str, tpos);
		if (id < 0) {
			ysize = tmes._fontsize;
			x = orgx; y = orgy;
			if (mode & TEXMES_MODE_CENTERX) {
				int px = sx / 2;
				x += px;
			}
			if (mode & TEXMES_MODE_CENTERY) {
				int py = (sy - ysize) / 2;
				if (py < 0) { py = 0; }
				y += py;
			}
			tpos->lastcx = x;
			tpos->lastcy = y;
			tpos->printysize = ysize;
			return -1;
		}
		tpos->texid = id;
	}

	texmes* tex;
	tex = tmes.texmesUpdateLife(id);
	if (tex == NULL) return -1;

	xsize = tex->sx;
	ysize = tex->sy;
	tpos->printysize = ysize;

	x = orgx; y = orgy;
	tx = 0; ty = 0;

	esx = x + sx;
	esy = y + sy;

	if (mode & TEXMES_MODE_CENTERX) {
		int px = (sx - xsize) / 2;
		if (px < 0) { px = 0; }
		x += px;
	}
	if (mode & TEXMES_MODE_CENTERY) {
		int py = (sy - ysize) / 2;
		if (py < 0) { py = 0; }
		y += py;
	}
	tpos->lastcx = x;
	tpos->lastcy = y;

	if ( tpos->attribute == NULL ) {
		if ((x + xsize) >= esx) {
			xsize = esx - x;
			if (xsize <= 0) return -1;
		}
		if ((y + ysize) >= esy) {
			ysize = esy - y;
			if (ysize <= 0) return -1;
		}
		hgio_fontcopy(bm, x, y, tex->ratex, tex->ratey, xsize, ysize, tex->_texture, tx, ty);
	}

	bm->cy += ysize;
	bm->printsizex = xsize;
	bm->printsizey = ysize;
	return 0;
}


/*	The value this returns goes straight back into hgio_fontcopy() as its
	texid argument, and that path resolves it with GetTex() - so it has to be
	an index into the TEXINF table, which is what the classic (win32) backend
	returns from RegistTexEmpty()/UpdateTex32().  This port had inherited the
	emscripten body, which hands out the raw GL name instead: on this driver
	GL names are large sparse integers that move between runs, so GetTex()
	walked far outside texinf[] and every string was drawn from an arbitrary
	texture, differently on each run.										*/
void hgio_fontsystem_delete(int id)
{
	DeleteTex( id );
}


int hgio_fontsystem_setup(int sx, int sy, void *buffer)
{
	int texid = MakeEmptyTexBuffer( sx, sy );

	if ( texid < 0 ) return -1;
	if ( UpdateTex32( texid, (char *)buffer, 0 ) < 0 ) {
		DeleteTex( texid );
		return -1;
	}
	return texid;
}


void hgio_editputclip(BMSCR* bm, char *str)
{
	//		クリップボードコピー
	//
#if (defined(HSPLINUX)||defined(HSPEMSCRIPTEN))
	SDL_SetClipboardText( (const char *)str );
#endif
}


char *hgio_editgetclip(BMSCR* bm)
{
	//		クリップボードペースト文字列取得
	//
#if (defined(HSPLINUX)||defined(HSPEMSCRIPTEN))
	if ( SDL_HasClipboardText() ) {
		return (SDL_GetClipboardText());
	}
#endif
	return NULL;
}

/*-------------------------------------------------------------------------------*/


void hgio_setcenter( float x, float y )
{
	center_x = x;
	center_y = y;
}


HSPREAL hgio_getinfo( int type )
{
	int i;
	i = type - GINFO_EXINFO_BASE;
	if (( i >= 0 )&&( i < GINFO_EXINFO_MAX)) {
		return infoval[i];
	}
	return 0.0;
}

void hgio_setinfo( int type, HSPREAL val )
{
	int i;
	i = type - GINFO_EXINFO_BASE;
	if (( i >= 0 )&&( i < GINFO_EXINFO_MAX)) {
		infoval[i] = val;
	}
}

/*	Start of a frame's drawing: poll the pad, put the main screen back as the
	target and mark the frame open.  No present - the classic runtime updates
	the screen on `redraw 1` alone, and presenting here let the title menu (7
	redraw 0 a frame) show the half-drawn state.						*/
static void sw_frame_begin( BMSCR *keep )
{
	/*	This is what a pad read hangs off, so it still runs on every redraw 0.
		The counters showed several calls a frame, which is what kept input
		alive once the frame clear stopped being the sampling point.		*/
	switch_input_poll();
	sw_frame_tick();

#ifdef HSPIOS
    gb_render_start();
#endif

	hgio_reset();


	//	hgio_reset()はウインドウに戻すので、離屏ターゲットを復元する
	if ( ( keep != NULL ) && ( keep->type != HSPWND_TYPE_MAIN ) ) {
		sw_bind_target( keep );
	}

	drawflag = 1;
}

int hgio_render_start( void )
{
	/*	Capture the offscreen target FIRST: hgio_render_end() unbinds it
		(sw_unbind_window() clears sw_cur), so reading it afterwards always
		yields NULL and the target is never restored.						*/
	BMSCR *keep = sw_cur;

	/*	A frame that was never closed by redraw 1 is presented here, so a
		script that draws outside a redraw cycle still reaches the screen.	*/
	if ( drawflag ) {
		hgio_render_end();
	}
	sw_frame_begin( keep );
	return 0;
}


int hgio_render_end( void )
{
	int res;
	res = 0;
	if ( drawflag == 0 ) return 0;
#ifdef HSPIOS
    gb_render_end();
#endif

		tmes.texmesProc();

	sw_main_present();

	//	ウインドウ(FBO 0)に戻してからスワップする
	sw_unbind_window();

	/*	Side panels: the 800x600 picture sits centred on a 16:9 panel with
		a letterbox strip on either side, and the strips carry the pad's key
		hints (switch_overlay.cpp).  The window framebuffer is bound here and
		the swap has not happened yet, so this is the point to draw them.  */
	switch_overlay_draw( (int)_sizex, (int)_sizey, (int)_originX,
		(int)( _bgsx * _scaleX ) );


#if defined(HSPRASPBIAN) || defined(HSPNDK)

    //後処理
    if (appengine->display == NULL) {
        // displayが無い
        return 0;
    }
	//hgio_setColor( 0xffffff );
	//hgio_fcopy( 0,80,  0, 0, 256, 128, font_texid, 0xffffff );


    eglSwapBuffers(appengine->display, appengine->surface);
#endif

#if defined(HSPEMSCRIPTEN)
	SDL_GL_SwapWindow(window);
	//SDL_GL_SwapBuffers();
#endif
#if defined(HSPLINUX)

	//hgio_makeTexFont( msg );
	//hgio_putTexFont( 0,0, (char *)"This is Test.", -1 );
	//hgio_setColor( 0xffffff );
	//hgio_boxfill( 100,200,100,10 );
	//hgio_fcopy( 0,80,  0, 0, 256, 128, font_texid, 0xffffff );

#ifndef HSPRASPBIAN
	SDL_GL_SwapWindow(window);
	//SDL_GL_SwapBuffers();
#endif
#endif

	drawflag = 0;
	return res;
}


void hgio_screen( BMSCR *bm )
{
    mainbm = bm;
}


void hgio_delscreen( BMSCR *bm )
{
	if ( bm->flag == BMSCR_FLAG_NOUSE ) return;
	if ( sw_del_report < 48 ) {
		sw_del_report++;
		sw_fbo_log( "hgio: delscreen bm=%p texid=%d hasTarget=%d\n",
			(void *)bm, bm->texid, ( sw_find( bm ) != NULL ) ? 1 : 0 );
	}
	sw_forget( bm );			// FBOはbm->texidをキーにしているので先に破棄する
	/*	sw_colorbm names the screen whose color/mulcolor the next draw picks up,
		and only gsel() ever assigns it.  A screen is routinely deleted and
		straight away re-created - Elona tears the title screen down and reloads
		title.bmp the moment it starts composing it - so leaving the pointer
		here left it naming freed memory for the whole composition.  The tiles
		then took a garbage multiply color, and on the runs where that memory
		held zeros every one of them was painted opaque black over the page.	*/
	if ( sw_colorbm == bm ) sw_colorbm = NULL;
	if ( sw_helpbm == bm ) sw_helpbm = NULL;
	if ( bm->texid != -1 ) {
		DeleteTex( bm->texid );
		//gb_delimage( bm->texid );
		bm->texid = -1;
	}
}


int hgio_redraw( BMSCR *bm, int flag )
{
	if ( bm == NULL ) return -1;
	if ( !sw_drawable( bm ) ) throw HSPERR_UNSUPPORTED_FUNCTION;

	if ( bm->type != HSPWND_TYPE_MAIN ) {
		//		離屏スクリーンのredrawは描画先の切り替えとクリアのみ行う
		//		(FBOの内容はフレームをまたいで保持される)
		//
		if ( flag & 1 ) {
			sw_redraw_time( bm );
		} else {
			if ( sw_bind_target( bm ) == 0 ) {
				drawflag = 1;
				if ( GetSysReq( SYSREQ_CLSMODE ) == CLSMODE_SOLID ) {
					int ccol = GetSysReq( SYSREQ_CLSCOLOR );
					hgio_setClear( (ccol>>16)&0xff, (ccol>>8)&0xff, (ccol)&0xff );
					hgio_clear();
				}
			}
		}
		return 0;
	}

	hgio_screen( bm );

	if ( flag & 1 ) {
		hgio_render_end();
		sw_redraw_time( bm );
	} else {
		sw_frame_begin( sw_cur );
	}
	return 0;
}



//
//		FILE I/O Service
//
static char storage_path[256];
static char my_storage_path[256+64];

int hgio_file_exist( char *fname )
{
#ifdef HSPNDK
	int size;
	AAssetManager* mgr = appengine->app->activity->assetManager;
	if (mgr == NULL) return -1;
	AAsset* asset = AAssetManager_open(mgr, (const char *)fname, AASSET_MODE_UNKNOWN);
	if (asset == NULL) return -1;
    size = (int)AAsset_getLength(asset);
    AAsset_close(asset);
	//Alertf( "[EXIST]%s:%d",fname,size );
    return size;
#endif
    return -1;
}


int hgio_file_read( char *fname, void *ptr, int size, int offset )
{
#ifdef HSPNDK
	int readsize;
	AAssetManager* mgr = appengine->app->activity->assetManager;
	if (mgr == NULL) return -1;
	AAsset* asset = AAssetManager_open(mgr, (const char *)fname, AASSET_MODE_UNKNOWN);
	if (asset == NULL) return -1;
    readsize = (int)AAsset_getLength(asset);
	if ( readsize > size ) readsize = size;
	if ( offset>0 ) AAsset_seek( asset, offset, SEEK_SET );
	AAsset_read( asset, ptr, readsize );
    AAsset_close(asset);
    return readsize;
#endif
    return -1;
}


#ifdef HSPNDK
FILE *hgio_android_fopen( char *fname, int offset )
{
	AAssetManager* mgr = appengine->app->activity->assetManager;
	if (mgr == NULL) return NULL;
	AAsset* asset = AAssetManager_open(mgr, (const char *)fname, AASSET_MODE_UNKNOWN);
	if (asset == NULL) return NULL;
	if ( offset>0 ) AAsset_seek( asset, offset, SEEK_SET );
	return (FILE *)asset;
}

void hgio_android_fclose(FILE* ptr)
{
	AAsset* asset = (AAsset*)ptr;
	if (asset == NULL) return;
    AAsset_close(asset);
}

int hgio_android_fread( FILE* ptr, void *mem, int size )
{
	AAsset* asset = (AAsset*)ptr;
	if (asset == NULL) -1;
	return AAsset_read( asset, mem, size );
}

int hgio_android_seek( FILE* ptr, int offset, int whence )
{
	AAsset* asset = (AAsset*)ptr;
	if (asset == NULL) -1;
	return AAsset_seek( asset, offset, whence );
}

#endif


void hgio_setstorage( char *path )
{
	int i;
	*storage_path = 0;
#ifdef HSPNDK
	i = strlen(path);if (( i<=0 )||( i>=255 )) return;
	strcpy( storage_path, path );
	if ( storage_path[i-1]!='/' ) {
		storage_path[i] = '/';
		storage_path[i+1] = 0;
	}
#endif
}


char *hgio_getstorage( char *fname )
{
#ifdef HSPNDK
	strcpy( my_storage_path, storage_path );
	strcat( my_storage_path, fname );
	return my_storage_path;
#endif
	return fname;
}


/*-------------------------------------------------------------------------------*/

void hgio_setview(BMSCR* bm)
{
	// vp_flagに応じたビューポートの設定を行う
	//
	int i;
	MATRIX *vmat;
	MATRIX tmpmat;
	float* vp;
	vmat = &mat_proj;
	vp = (float*)vmat;
	float* mat = (float*)GetCurrentMatrixPtr();

	switch (bm->vp_flag) {
	case BMSCR_VPFLAG_2D:
		//	2D projection mode
		UnitMatrix();
		RotZ(bm->vp_viewrotate[2]);
		GetCurrentMatrix(&tmpmat);
		OrthoMatrix(-bm->vp_viewtrans[0], bm->vp_viewtrans[1], (float)_bgsx / bm->vp_viewscale[0], (float)-_bgsy / bm->vp_viewscale[1], 0.0f, 1.0f);
		MulMatrix(&tmpmat);
		break;
	case BMSCR_VPFLAG_3D:
		//	3D projection mode
		UnitMatrix();
		RotZ(bm->vp_viewrotate[2]);
		RotY(bm->vp_viewrotate[1]);
		RotX(bm->vp_viewrotate[0]);
		Scale(bm->vp_viewscale[0], bm->vp_viewscale[1], bm->vp_viewscale[2]);
		Trans(bm->vp_viewtrans[0], bm->vp_viewtrans[1], bm->vp_viewtrans[2]);
		GetCurrentMatrix(&tmpmat);
		PerspectiveFOV(bm->vp_view3dprm[0], bm->vp_view3dprm[1], bm->vp_view3dprm[2], 0.0f, 0.0f, (float)_bgsx / 10, (float)_bgsy / 10);
		//PerspectiveFOV(45.0f, 1.0f, 500.0f, 0.0f, 0.0f, (float)nDestWidth / 10, (float)nDestHeight / 10);
		//PerspectiveWithZBuffer(10.0f, 0.0f, 60.0f, 1.0f, 0.0f);
		MulMatrix(&tmpmat);
		break;
	case BMSCR_VPFLAG_MATRIX:
		//	user matrix mode
		mat = &bm->vp_viewtrans[0];
		break;
	case BMSCR_VPFLAG_NOUSE:
	default:
		return;
	}

	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(mat);

	//	mat_projに設定する
	for (i = 0; i < 16; i++) {
		*vp++ = *mat++;
	}

	//D3DXMATRIX matrixProj;
	//Mat2D3DMAT(&matrixProj, vmat);
	//d3ddev->SetTransform(D3DTS_PROJECTION, &matrixProj);

	//	投影マトリクスの逆行列を設定する
	//D3DXMatrixInverse(&InvViewport, NULL, &matrixProj);
	SetCurrentMatrix(vmat);
	InverseMatrix(&mat_unproj);

}


void hgio_cnvview(BMSCR* bm, int* xaxis, int* yaxis)
{
	//	ビュー変換後の座標 -> 元の座標に変換する
	//	(タッチ位置再現のため)
	//
	VECTOR v1,v2;
	if (bm==NULL) return;
	if (bm->vp_flag == 0) return;
	v1.x = (float)*xaxis;
	v1.y = (float)(_bgsy-*yaxis);
	v1.z = 1.0f;
	v1.w = 0.0f;

	v1.x -= _bgsx/2;
	v1.y -= _bgsy/2;
	v1.x *= 2.0f / float(_bgsx);
	v1.y *= 2.0f / float(_bgsy);

//	*xaxis = (int)(v1.x);
//	*yaxis = (int)(v1.y);

//	D3DXVECTOR3 a1,a2;
//	D3DXVec3TransformCoord(&a2, &D3DXVECTOR3(v1.x, v1.y, v1.z), &InvViewport);
//	*xaxis = (int)a2.x;
//	*yaxis = (int)a2.y;

	ApplyMatrix(&mat_unproj, &v2, &v1);
	*xaxis = (int)v2.x;
	*yaxis = (int)-v2.y;
}


