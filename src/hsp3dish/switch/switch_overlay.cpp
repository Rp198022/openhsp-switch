//
//	src/hsp3dish/switch/switch_overlay.cpp
//	Side-panel key hints for the Switch port.
//
//	The Switch panel is 16:9 while Elona's canvas is 4:3, so the picture sits
//	centred with a letterbox strip on either side - about 160px at 1280x720,
//	240px at 1920x1080.  Those strips are dead space, and this module fills
//	them with the pad's key map: the left strip for the left Joy-Con, the
//	right strip for the right one, including the second layer that L opens.
//
//	Everything is rendered once with SDL_ttf - the font the game itself ships,
//	so the strips read Chinese - into one RGBA texture per side; a frame then
//	costs two textured quads.  The drawing happens at present time, between
//	sw_unbind_window() and SDL_GL_SwapWindow(), where the window framebuffer
//	is bound; switch_overlay_draw() takes the layout as parameters instead of
//	reading the engine's globals from here.
//
//	Toggling (R + right-stick click) is handled in switch_input.cpp; this
//	module only owns the flag.
//
//	GL note: the texture goes through the shadowed GL entry points the rest
//	of the port uses (glcompat -> gles1_shim), so it lands in the shim's
//	texture storage like any other texture and no game texture-table entry
//	is touched.  glTexImage2D data keeps its top row at v=0 (the convention
//	sw_apply_target() documents for loaded images), which the quad below
//	assumes.
//
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <GL/gl.h>			/* shadowed by glcompat: maps to gles1_shim	*/

#include "switch_overlay.h"

/*	Texture on/off is part of the fixed-function state the shim tracks,
	and the present pass leaves GL_TEXTURE_2D *disabled*: ChangeTex(-1)
	disables it on the way out (hgtex.cpp).  A draw whose texture never
	gets sampled falls back to flat white in the shim's shader, so the
	panels enable it themselves and put the state back.  sw_texture2d_on
	is the shim's own view of that switch.  */
extern "C" int sw_texture2d_on( void );

#define SW_OVL_FONT_SIZE	16
#define SW_OVL_LINE_H		22
#define SW_OVL_PAD_Y		8
#define SW_OVL_KEY_X		8		/* key column					*/
#define SW_OVL_DESC_X		80		/* description column			*/
#define SW_OVL_TITLE_X		8		/* group titles start at the key column */
#define SW_OVL_MARGIN		2		/* distance kept from the screen edge */
#define SW_OVL_COL_GAP		4		/* gap between the measured columns */

typedef struct {
	const char			*key;	/* NULL: the row is a group title		*/
	const char			*desc;
} SW_OVL_ROW;

/*	Left Joy-Con.  The functions are the original keyboard actions the pad
	was mapped to (switch_keymap_design).									*/
static const SW_OVL_ROW sw_ovl_left[] = {
	{ "十字键",		"移动/光标" },
	{ "左摇杆↑",	"下楼/进入" },
	{ "左摇杆↓",	"上楼" },
	{ "左摇杆←",	"角色情报" },
	{ "左摇杆→",	"投掷" },
	{ "左摇杆按",	"阅读" },
	{ "ZL",			"射击" },
	{ "减号",		"锁定目标" },
	{ "L",			"切换标签" },
};

/*	Right Joy-Con, both layers.											*/
static const SW_OVL_ROW sw_ovl_right[] = {
	{ "A",			"确认/攻击" },
	{ "B",			"取消/关闭" },
	{ "X",			"行动菜单" },
	{ "Y",			"道具菜单" },
	{ "ZR",			"咏唱魔法" },
	{ "右摇杆↑",	"挥动魔杖" },
	{ "右摇杆↓",	"搜索周围" },
	{ "右摇杆←",	"使用特技" },
	{ "右摇杆→",	"对话" },
	{ "右摇杆按",	"原地休息" },
	{ "加号",		"存档/设置" },
	{ NULL,			"— 第二层（按住 R）—" },
	{ "A",			"吃" },
	{ "B",			"喝" },
	{ "X",			"拾取" },
	{ "Y",			"装备" },
	{ "右摇杆↑",	"丢弃" },
	{ "右摇杆↓",	"打开" },
	{ "右摇杆←",	"日志" },
	{ "右摇杆→",	"砸开" },
	{ "L",			"删除存档" },
	{ "R+摇杆按",	"显示/隐藏" },
};

#define SW_OVL_LEFT_N	((int)( sizeof( sw_ovl_left ) / sizeof( sw_ovl_left[0] ) ))
#define SW_OVL_RIGHT_N	((int)( sizeof( sw_ovl_right ) / sizeof( sw_ovl_right[0] ) ))

static int		sw_ovl_off = 0;			/* hidden when 1 (shown by default) */
static int		sw_ovl_failed = 0;		/* font/build failure: gives up		*/
static int		sw_ovl_tried = 0;		/* the one-time font open has run	*/
static TTF_Font	*sw_ovl_font = NULL;
static GLuint	sw_ovl_tex[2] = { 0, 0 };	/* 0 = left strip, 1 = right strip	*/
static int		sw_ovl_tex_w[2] = { 0, 0 };
static int		sw_ovl_tex_h[2] = { 0, 0 };
static int		sw_ovl_origin_x = -1;	/* layout the textures were built for */

void switch_overlay_toggle( void )
{
	sw_ovl_off = sw_ovl_off ? 0 : 1;
	printf( "hsp3switch: overlay %s\n", sw_ovl_off ? "hidden" : "shown" );
	fflush( stdout );
}

int switch_overlay_hidden( void )
{
	return sw_ovl_off;
}

/*	The game draws its Chinese with the font shipped in its own directory
	(the same file is used for every text the game shows), so the strips pull
	the very same glyphs.  The backups are tried in the order the port's
	deployments left them on the SD card.									*/
static TTF_Font *sw_ovl_open_font( void )
{
	static const char *cand[] = {
		"sdmc:/switch/openhsp/ipaexg.ttf",
		"sdmc:/switch/openhsp/ipaexg.ttf.simhei",
		"sdmc:/switch/openhsp/ipaexg.ttf.notovf",
	};
	int i;

	if ( TTF_Init() != 0 ) {
		printf( "hsp3switch: overlay: TTF_Init failed (%s)\n", TTF_GetError() );
		fflush( stdout );
		return NULL;
	}
	for ( i = 0; i < (int)( sizeof( cand ) / sizeof( cand[0] ) ); i++ ) {
		TTF_Font *f = TTF_OpenFont( cand[i], SW_OVL_FONT_SIZE );
		if ( f != NULL ) {
			printf( "hsp3switch: overlay font %s\n", cand[i] );
			fflush( stdout );
			return f;
		}
	}
	printf( "hsp3switch: overlay: no usable font, hints disabled\n" );
	fflush( stdout );
	return NULL;
}

static void sw_ovl_blit_text( SDL_Surface *dst, const char *text, Uint8 v, int x, int y )
{
	SDL_Color col;
	SDL_Surface *s, *c;
	SDL_Rect at;

	col.r = col.g = col.b = v;
	col.a = 255;
	s = TTF_RenderUTF8_Blended( sw_ovl_font, text, col );
	if ( s == NULL ) return;
	c = SDL_ConvertSurfaceFormat( s, SDL_PIXELFORMAT_ABGR8888, 0 );
	SDL_FreeSurface( s );
	if ( c == NULL ) return;
	at.x = x;
	at.y = y;
	SDL_BlitSurface( c, NULL, dst, &at );
	SDL_FreeSurface( c );
}

/*	One strip: all rows onto a transparent RGBA sheet, then upload.			*/
static int sw_ovl_build_side( const SW_OVL_ROW *rows, int n, int strip_w, int ix )
{
	SDL_Surface *panel;
	int h = n * SW_OVL_LINE_H + SW_OVL_PAD_Y * 2;
	int i;
	int key_x, desc_x, title_x;
	int max_key = 0, max_desc = 0, tmp_w, tmp_h, k;

	/*	Pin the block to its screen edge: measure the two column widths, then
		start the left panel at the left margin and end the right panel's
		description at the right margin, so the block rides the edge whatever
		the font's real advance is.		*/
	for ( k = 0; k < n; k++ ) {
		if ( rows[k].key == NULL ) continue;
		if ( TTF_SizeUTF8( sw_ovl_font, rows[k].key, &tmp_w, &tmp_h ) == 0 && tmp_w > max_key ) max_key = tmp_w;
		if ( TTF_SizeUTF8( sw_ovl_font, rows[k].desc, &tmp_w, &tmp_h ) == 0 && tmp_w > max_desc ) max_desc = tmp_w;
	}
	if ( ix == 0 ) {
		key_x  = SW_OVL_MARGIN;
		desc_x = key_x + max_key + SW_OVL_COL_GAP;
	} else {
		desc_x = strip_w - SW_OVL_MARGIN - max_desc;
		key_x  = desc_x - SW_OVL_COL_GAP - max_key;
		if ( key_x < SW_OVL_MARGIN ) key_x = SW_OVL_MARGIN;
		if ( desc_x < SW_OVL_MARGIN ) desc_x = SW_OVL_MARGIN;
	}
	/*	Group titles hug the outer margin: they are the widest rows of
		the panel and would otherwise squeeze the key column back towards
		the middle of the strip.										*/
	title_x = SW_OVL_MARGIN;
	printf( "hsp3switch: overlay cols ix=%d k=%d d=%d at %d,%d\n", ix, max_key, max_desc, key_x, desc_x );
	fflush( stdout );

	panel = SDL_CreateRGBSurfaceWithFormat( 0, strip_w, h, 32, SDL_PIXELFORMAT_ABGR8888 );
	if ( panel == NULL ) return -1;
	SDL_FillRect( panel, NULL, SDL_MapRGBA( panel->format, 0, 0, 0, 0 ) );

	for ( i = 0; i < n; i++ ) {
		const SW_OVL_ROW *r = &rows[i];
		int y = SW_OVL_PAD_Y + i * SW_OVL_LINE_H;
		if ( r->key == NULL ) {
			sw_ovl_blit_text( panel, r->desc, 205, title_x, y );
		} else {
			sw_ovl_blit_text( panel, r->key, 255, key_x, y );
			sw_ovl_blit_text( panel, r->desc, 230, desc_x, y );
		}
	}

	if ( sw_ovl_tex[ix] != 0 ) {
		glDeleteTextures( 1, &sw_ovl_tex[ix] );
		sw_ovl_tex[ix] = 0;
	}
	glGenTextures( 1, &sw_ovl_tex[ix] );
	glBindTexture( GL_TEXTURE_2D, sw_ovl_tex[ix] );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, strip_w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, panel->pixels );

	sw_ovl_tex_w[ix] = strip_w;
	sw_ovl_tex_h[ix] = h;
	SDL_FreeSurface( panel );
	return 0;
}

static int sw_ovl_build( int left_w, int right_w )
{
	if ( !sw_ovl_tried ) {
		sw_ovl_tried = 1;
		sw_ovl_font = sw_ovl_open_font();
		if ( sw_ovl_font == NULL ) {
			sw_ovl_failed = 1;
			return -1;
		}
	}
	if ( sw_ovl_font == NULL ) return -1;
	if ( sw_ovl_build_side( sw_ovl_left, SW_OVL_LEFT_N, left_w, 0 ) != 0 ) { sw_ovl_failed = 1; return -1; }
	if ( sw_ovl_build_side( sw_ovl_right, SW_OVL_RIGHT_N, right_w, 1 ) != 0 ) { sw_ovl_failed = 1; return -1; }
	printf( "hsp3switch: overlay built L%dx%d R%dx%d\n",
		sw_ovl_tex_w[0], sw_ovl_tex_h[0], sw_ovl_tex_w[1], sw_ovl_tex_h[1] );
	fflush( stdout );
	return 0;
}

static void sw_ovl_quad( GLuint tex, float x0, float y0, float w, float h )
{
	GLfloat vert[8];
	GLfloat uv[8];

	/*	Window-space pass: y grows downward from 0, so the block spans
		y0 .. y0-h and v runs top (0) to bottom (1).						*/
	vert[0] = x0;		vert[1] = y0;
	vert[2] = x0 + w;	vert[3] = y0;
	vert[4] = x0;		vert[5] = y0 - h;
	vert[6] = x0 + w;	vert[7] = y0 - h;

	uv[0] = 0.0f;	uv[1] = 0.0f;
	uv[2] = 1.0f;	uv[3] = 0.0f;
	uv[4] = 0.0f;	uv[5] = 1.0f;
	uv[6] = 1.0f;	uv[7] = 1.0f;

	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, vert );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );
	glBindTexture( GL_TEXTURE_2D, tex );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
}

void switch_overlay_draw( int win_w, int win_h, int origin_x, int game_w )
{
	int right_x, right_w, ly0, ry0, tex2d_was;

	if ( sw_ovl_off || sw_ovl_failed ) return;
	if ( win_w <= 0 || win_h <= 0 ) return;
	if ( origin_x < 48 ) return;					/* no usable strip		*/
	right_x = origin_x + game_w;
	right_w = win_w - right_x;
	if ( right_w < 48 ) return;

	/*	Rebuild when the layout changed (docked/undocked switches the strip
		width); the content itself is static.								*/
	if ( !sw_ovl_tried || origin_x != sw_ovl_origin_x ) {
		if ( sw_ovl_build( origin_x, right_w ) != 0 ) return;
		sw_ovl_origin_x = origin_x;
	}

	/*	Window-space coordinates: x across the whole panel, y downward.		*/
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0, (GLdouble)win_w, -(GLdouble)win_h, 0, -100, 100 );
	glViewport( 0, 0, win_w, win_h );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	/*	Same colour setup the present pass leaves behind: vertex colours off,
		colour-keying off; the fragments come from the texture's own alpha.	*/
	glDisableClientState( GL_COLOR_ARRAY );
	sw_glColorKey( 0, 0 );
	tex2d_was = sw_texture2d_on();
	glEnable( GL_TEXTURE_2D );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

	ly0 = -( ( win_h - sw_ovl_tex_h[0] ) / 2 );
	ry0 = -( ( win_h - sw_ovl_tex_h[1] ) / 2 );
	sw_ovl_quad( sw_ovl_tex[0], 0.0f, (float)ly0,
		(float)sw_ovl_tex_w[0], (float)sw_ovl_tex_h[0] );
	sw_ovl_quad( sw_ovl_tex[1], (float)right_x, (float)ry0,
		(float)sw_ovl_tex_w[1], (float)sw_ovl_tex_h[1] );

	/*	Back to the state sw_main_present() left the frame in: nothing bound,
		blending off.  The projection/viewport are re-set by every draw pass
		at the top of the next frame (sw_apply_target), so they stay.		*/
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_BLEND );
	if ( !tex2d_was ) glDisable( GL_TEXTURE_2D );
}