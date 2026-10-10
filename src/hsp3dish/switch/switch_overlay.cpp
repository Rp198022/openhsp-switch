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

/*	Party HUD reads the interpreter's script variables directly, so it can
	track the player and one pet without touching the script.  The shim
	hands us the context; the names resolve through the debug name table.	*/
#include "../../hsp3/switch/dllshim_switch.h"
#include "../../hsp3/hsp3code.h"
#include "../../hsp3/hspvar_core.h"

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
	{ NULL,			"— 第二层（按住 R）—" },
	{ "左摇杆↑",	"跳跃" },
	{ "左摇杆↓",	"选择目标" },
	{ "左摇杆←",	"给予" },
	{ "左摇杆→",	"关闭" },
	{ "左摇杆按",	"使用道具" },
	{ "十字键↑",	"外观/祈祷" },
	{ "十字键↓",	"供奉" },
	{ "十字键←",	"蘸取" },
	{ "十字键→",	"特性" },
	{ "ZL",			"物品信息" },
	{ "ZR",			"素材列表" },
	{ "加号",		"翻页上" },
	{ "减号",		"翻页下" },
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
	{ "右摇杆→",	"改变地形" },
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

/*	Display mode: R+stick click cycles key hints <-> party HUD.			*/
#define SW_OVL_MODE_KEY		0
#define SW_OVL_MODE_PARTY	1
static int		sw_ovl_mode = SW_OVL_MODE_KEY;

/*	Party HUD state.  The interpreter context is the cache key: when it
	changes (new game, load) the variable indices are resolved again.
	The face atlas and the 1x1 bar swatches are lazily uploaded on first
	use and rebuilt after a GL context reset.						*/
static HSPCTX	*sw_ovl_ctx = NULL;
static PVal		*sw_ovl_pv_cdata = NULL;
static PVal		*sw_ovl_pv_cdatan = NULL;
static PVal		*sw_ovl_pv_sdata = NULL;
static PVal		*sw_ovl_pv_msg = NULL;
static PVal		*sw_ovl_pv_msgline = NULL;
static PVal		*sw_ovl_pv_inf_maxlog = NULL;
static GLuint	sw_ovl_face_tex = 0;
static GLuint	sw_ovl_bar_tex[4] = { 0, 0, 0, 0 };	/* white,hp,mp,sp	*/
static int		sw_ovl_face_w = 0;
static int		sw_ovl_face_h = 0;

/*	Party sheet textures: one per strip, rebuilt only when the tracked
	content (name, bars, abilities, chat) actually changes.				*/
static GLuint	sw_ovl_party_sheet[2] = { 0, 0 };
static int		sw_ovl_party_sheet_w[2] = { 0, 0 };
static int		sw_ovl_party_sheet_h[2] = { 0, 0 };
static int		sw_ovl_party_slot[2] = { 0, -1 };
static char		sw_ovl_party_name[2][ 64 ] = { "", "" };
static int		sw_ovl_party_hp[2], sw_ovl_party_mhp[2];
static int		sw_ovl_party_sp[2], sw_ovl_party_msp[2];
static int		sw_ovl_party_mp[2], sw_ovl_party_mmp[2];
static int		sw_ovl_party_abil[2][ 8 ];
static char		sw_ovl_party_chat[2][ 200 ] = { "", "" };

void switch_overlay_ctx_reset( void )
{
	/*	The context that issued these names is gone; dropping the numbers
		without glDeleteTextures() is deliberate (see the header).		*/
	sw_ovl_tex[0] = 0;
	sw_ovl_tex[1] = 0;
	sw_ovl_origin_x = -1;
	sw_ovl_face_tex = 0;
	sw_ovl_bar_tex[0] = 0;
	sw_ovl_bar_tex[1] = 0;
	sw_ovl_bar_tex[2] = 0;
	sw_ovl_bar_tex[3] = 0;
	sw_ovl_ctx = NULL;
	sw_ovl_pv_cdata = NULL;
	sw_ovl_pv_cdatan = NULL;
	sw_ovl_pv_sdata = NULL;
	sw_ovl_pv_msg = NULL;
	sw_ovl_pv_msgline = NULL;
	sw_ovl_pv_inf_maxlog = NULL;
	sw_ovl_party_sheet[0] = 0;
	sw_ovl_party_sheet[1] = 0;
	sw_ovl_party_slot[0] = 0;
	sw_ovl_party_slot[1] = -1;
}


void switch_overlay_toggle( void )
{
	/*	r185f: the party HUD joins the toggle cycle.  R+stick now walks
		key hints -> party HUD -> key hints; there is no "hidden" state,
		the player asked for the strips to carry information.			*/
	sw_ovl_mode = ( sw_ovl_mode == SW_OVL_MODE_PARTY ) ? SW_OVL_MODE_KEY : SW_OVL_MODE_PARTY;
	printf( "hsp3switch: overlay %s\n", sw_ovl_mode == SW_OVL_MODE_PARTY ? "party" : "keys" );
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
			/*	r173: the hints are drawn over the game's own artwork and
				the regular weight was hard to make out on the small screen. */
			TTF_SetFontStyle( f, TTF_STYLE_BOLD );
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

	/*	Party HUD replaces the key hints entirely: same window pass, its
		own build/draw path below.										*/
	if ( sw_ovl_mode == SW_OVL_MODE_PARTY ) {
		switch_overlay_draw_party( win_w, win_h, origin_x, game_w, right_x, right_w );
		return;
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
/*	==================================================================
	Party HUD (r185f)
	------------------------------------------------------------------
	Reads the interpreter's live variables (cdata/cdatan/sdata/msg) via
	the shim's context getter and paints the player on the left strip and
	the current pet on the right one: portrait, name, HP/MP/SP bars with
	their numbers, the eight base abilities and the pet's latest chat.
	==================================================================*/

#define SW_OVL_PARTY_CD_EXIST		0
#define SW_OVL_PARTY_CD_SEX		8
#define SW_OVL_PARTY_CD_PORTRAIT		13
#define SW_OVL_PARTY_CD_HP		50
#define SW_OVL_PARTY_CD_MAX_HP		51
#define SW_OVL_PARTY_CD_SP		52
#define SW_OVL_PARTY_CD_MAX_SP		53
#define SW_OVL_PARTY_CD_MP		55
#define SW_OVL_PARTY_CD_MAX_MP		56
#define SW_OVL_PARTY_CD_ALLIED		58
#define SW_OVL_PARTY_CD_TAGTEAM		167
#define SW_OVL_PARTY_CD_FACE		431

#define SW_OVL_PARTY_ATTR_FIRST		10	/* SKILL_ATTR_STR */
#define SW_OVL_PARTY_ATTR_COUNT		8	/* STR..CHA */
#define SW_OVL_PARTY_SLOTS		16	/* MAX_CHARA_FOLLOWER */

#define SW_OVL_PARTY_AVA_W		96
#define SW_OVL_PARTY_AVA_H		134
#define SW_OVL_PARTY_AVA_X		8
#define SW_OVL_PARTY_AVA_Y		8

/*	face1.bmp is a 16-column atlas of 48x72 cells (800x744 bitmap).	*/
#define SW_OVL_PARTY_CELL_W		48
#define SW_OVL_PARTY_CELL_H		72
#define SW_OVL_PARTY_CELL_COLS		16

static int sw_ovl_party_resolve( void )
{
	HSPCTX *ctx;
	int id;

	if ( sw_ovl_ctx != NULL ) return 0;
	ctx = switch_runtime_hspctx();
	if ( ctx == NULL ) return -1;
	sw_ovl_ctx = ctx;

	id = code_getdebug_seekvar( "cdata" );
	if ( id < 0 || id >= ctx->hsphed->max_val ) return -1;
	sw_ovl_pv_cdata = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "cdatan" );
	if ( id < 0 || id >= ctx->hsphed->max_val ) return -1;
	sw_ovl_pv_cdatan = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "sdata" );
	if ( id < 0 || id >= ctx->hsphed->max_val ) return -1;
	sw_ovl_pv_sdata = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "msg" );
	if ( id < 0 || id >= ctx->hsphed->max_val ) return -1;
	sw_ovl_pv_msg = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "msgline" );
	if ( id < 0 || id >= ctx->hsphed->max_val ) return -1;
	sw_ovl_pv_msgline = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "inf_maxlog" );
	if ( id < 0 || id >= ctx->hsphed->max_val ) return -1;
	sw_ovl_pv_inf_maxlog = &ctx->mem_var[ id ];
	return 0;
}

static int sw_ovl_cdata( int field, int slot )
{
	if ( sw_ovl_pv_cdata == NULL ) return 0;
	if ( field < 0 || field >= sw_ovl_pv_cdata->len[1] ) return 0;
	if ( slot < 0 || slot >= sw_ovl_pv_cdata->len[2] ) return 0;
	return ((int*)sw_ovl_pv_cdata->pt)[ field + slot * sw_ovl_pv_cdata->len[1] ];
}

static int sw_ovl_sdata( int ability, int slot )
{
	if ( sw_ovl_pv_sdata == NULL ) return 0;
	if ( ability < 0 || ability >= sw_ovl_pv_sdata->len[1] ) return 0;
	if ( slot < 0 || slot >= sw_ovl_pv_sdata->len[2] ) return 0;
	return ((int*)sw_ovl_pv_sdata->pt)[ ability + slot * sw_ovl_pv_sdata->len[1] ];
}

static const char *sw_ovl_cdatan( int field, int slot )
{
	if ( sw_ovl_pv_cdatan == NULL ) return "";
	if ( field < 0 || field >= sw_ovl_pv_cdatan->len[1] ) return "";
	if ( slot < 0 || slot >= sw_ovl_pv_cdatan->len[2] ) return "";
	/*	HspVarCorePtrAPTR writes pv->offset; the pointer is only valid
		until the next call on the same PVal, so callers copy at once.	*/
	return (const char*)HspVarCorePtrAPTR( sw_ovl_pv_cdatan, field + slot * sw_ovl_pv_cdatan->len[1] );
}

static int sw_ovl_pick_pet( int *out )
{
	int s, partner, best = -1;

	if ( sw_ovl_pv_cdata == NULL ) return 0;
	partner = sw_ovl_cdata( SW_OVL_PARTY_CD_TAGTEAM, 0 );
	for ( s = 1; s < SW_OVL_PARTY_SLOTS && s < sw_ovl_pv_cdata->len[2]; s++ ) {
		if ( sw_ovl_cdata( SW_OVL_PARTY_CD_EXIST, s ) != 1 ) continue;
		if ( sw_ovl_cdata( SW_OVL_PARTY_CD_ALLIED, s ) != 100 ) continue;
		if ( best < 0 ) best = s;
		if ( s == partner ) { *out = s; return 1; }
	}
	if ( best >= 0 ) { *out = best; return 1; }
	return 0;
}

static const char *sw_ovl_msg_peek( const char *pet_name )
{
	int ml, n, i, idx;
	const char *s;

	if ( sw_ovl_pv_msg == NULL || sw_ovl_pv_msgline == NULL ) return "";
	ml = *(int*)sw_ovl_pv_msgline->pt;
	if ( ml < 0 ) return "";
	n = sw_ovl_pv_inf_maxlog ? *(int*)sw_ovl_pv_inf_maxlog->pt : 0;
	if ( n <= 0 ) n = 3;
	if ( n > sw_ovl_pv_msg->len[1] ) n = sw_ovl_pv_msg->len[1];
	/*	Walk the ring buffer backwards from the newest line; the newest
		line that starts with the pet's name is its latest chat.		*/
	for ( i = 0; i < n && i < 6; i++ ) {
		idx = ( ml - i ) % n;
		if ( idx < 0 ) idx += n;
		s = (const char*)HspVarCorePtrAPTR( sw_ovl_pv_msg, idx );
		if ( s != NULL && pet_name != NULL && pet_name[0] != '\0' &&
			 strncmp( s, pet_name, strlen( pet_name ) ) == 0 ) {
			return s;
		}
	}
	return "";
}

static const char *sw_ovl_attr_name( int i )
{
	static const char *nm[8] = {
		"力量", "体质", "灵巧", "感知",
		"学习", "意志", "魔力", "魅力",
	};
	if ( i < 0 || i > 7 ) return "";
	return nm[ i ];
}

static GLuint sw_ovl_upload_rgba( SDL_Surface *surf, int *out_w, int *out_h )
{
	GLuint tex;

	glGenTextures( 1, &tex );
	glBindTexture( GL_TEXTURE_2D, tex );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, surf->w, surf->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, surf->pixels );
	if ( out_w ) *out_w = surf->w;
	if ( out_h ) *out_h = surf->h;
	return tex;
}

static int sw_ovl_face_load( void )
{
	SDL_Surface *bmp, *conv;
	static const char *cand[] = {
		"sdmc:/switch/openhsp/graphic/face1.bmp",
		"sdmc:/switch/openhsp/user/graphic/face1.bmp",
	};
	int i;

	if ( sw_ovl_face_tex != 0 ) return 0;
	for ( i = 0; i < (int)( sizeof( cand ) / sizeof( cand[0] ) ); i++ ) {
		bmp = SDL_LoadBMP( cand[i] );
		if ( bmp == NULL ) continue;
		conv = SDL_ConvertSurfaceFormat( bmp, SDL_PIXELFORMAT_ABGR8888, 0 );
		SDL_FreeSurface( bmp );
		if ( conv == NULL ) continue;
		sw_ovl_face_tex = sw_ovl_upload_rgba( conv, &sw_ovl_face_w, &sw_ovl_face_h );
		SDL_FreeSurface( conv );
		if ( sw_ovl_face_tex == 0 ) continue;
		printf( "hsp3switch: overlay face %s (%dx%d)\n", cand[i], sw_ovl_face_w, sw_ovl_face_h );
		fflush( stdout );
		return 0;
	}
	printf( "hsp3switch: overlay: no face atlas, portraits disabled\n" );
	fflush( stdout );
	return -1;
}

static int sw_ovl_bar_upload( void )
{
	/*	ABGR8888 Uint32: 0xAABBGGRR */
	Uint32 rgb[4] = { 0xFFFFFFFF, 0xFF0000FF, 0xFFFF0000, 0xFF00FFFF };
	/*			white		hp(red)	mp(blue)	sp(yellow)	*/
	int k;
	SDL_Surface *s;

	if ( sw_ovl_bar_tex[0] != 0 ) return 0;
	for ( k = 0; k < 4; k++ ) {
		s = SDL_CreateRGBSurfaceWithFormat( 0, 1, 1, 32, SDL_PIXELFORMAT_ABGR8888 );
		if ( s == NULL ) return -1;
		SDL_FillRect( s, NULL, rgb[k] );
		sw_ovl_bar_tex[k] = sw_ovl_upload_rgba( s, NULL, NULL );
		SDL_FreeSurface( s );
		if ( sw_ovl_bar_tex[k] == 0 ) return -1;
	}
	return 0;
}

static void sw_ovl_quad_uv( GLuint tex, float x0, float y0, float w, float h,
						float u0, float v0, float u1, float v1 )
{
	GLfloat vert[8];
	GLfloat uv[8];

	vert[0] = x0;		vert[1] = y0;
	vert[2] = x0 + w;	vert[3] = y0;
	vert[4] = x0;		vert[5] = y0 - h;
	vert[6] = x0 + w;	vert[7] = y0 - h;

	uv[0] = u0;	uv[1] = v0;
	uv[2] = u1;	uv[3] = v0;
	uv[4] = u0;	uv[5] = v1;
	uv[6] = u1;	uv[7] = v1;

	glEnableClientState( GL_VERTEX_ARRAY );
	glVertexPointer( 2, GL_FLOAT, 0, vert );
	glEnableClientState( GL_TEXTURE_COORD_ARRAY );
	glTexCoordPointer( 2, GL_FLOAT, 0, uv );
	glBindTexture( GL_TEXTURE_2D, tex );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
}

static void sw_ovl_chat_lines( SDL_Surface *panel, const char *msg, int x, int y, int maxl )
{
	char buf[ 40 ];
	int len = (int)strlen( msg );
	int row, per = 12, s, n;

	if ( len > per * maxl ) len = per * maxl;
	if ( len <= 0 ) return;
	for ( row = 0; row < maxl && row * per < len; row++ ) {
		s = row * per;
		n = per;
		if ( s + n > len ) n = len - s;
		/*	Walk back so the chunk starts and ends on UTF-8 lead bytes. */
		while ( n > 1 && ( (unsigned char)msg[s+n-1] & 0xC0 ) == 0x80 ) n--;
		while ( n > 1 && ( (unsigned char)msg[s] & 0xC0 ) == 0x80 ) { s++; n--; }
		if ( n <= 0 ) n = 1;
		memcpy( buf, msg + s, n );
		buf[ n ] = '\0';
		sw_ovl_blit_text( panel, buf, 210, x, y + row * 18 );
	}
}

static int sw_ovl_party_build( int ix, int strip_w, int sheet_h )
{
	SDL_Surface *panel;
	SDL_Rect r;
	int y, i;
	char buf[ 96 ];

	if ( strip_w < 48 ) return -1;
	if ( sw_ovl_party_sheet[ ix ] != 0 ) {
		glDeleteTextures( 1, &sw_ovl_party_sheet[ ix ] );
		sw_ovl_party_sheet[ ix ] = 0;
	}
	panel = SDL_CreateRGBSurfaceWithFormat( 0, strip_w, sheet_h, 32, SDL_PIXELFORMAT_ABGR8888 );
	if ( panel == NULL ) return -1;
	SDL_FillRect( panel, NULL, SDL_MapRGBA( panel->format, 0, 0, 0, 0 ) );

	y = 0;
	sw_ovl_blit_text( panel, sw_ovl_party_name[ ix ], 255, 8, y );
	y += 20;

	if ( ix == 1 ) {
		static const char *lbl[3] = { "HP", "MP", "SP" };
		int cur[3] = { sw_ovl_party_hp[1], sw_ovl_party_mp[1], sw_ovl_party_sp[1] };
		int mxs[3] = { sw_ovl_party_mhp[1], sw_ovl_party_mmp[1], sw_ovl_party_msp[1] };
		for ( i = 0; i < 3; i++ ) {
			if ( mxs[ i ] <= 0 ) continue;
			r.x = 8; r.y = y; r.w = strip_w - 44; r.h = 8;
			SDL_FillRect( panel, &r, SDL_MapRGBA( panel->format, 50, 50, 50, 210 ) );
			snprintf( buf, sizeof( buf ), "%s %d/%d", lbl[ i ], cur[ i ], mxs[ i ] );
			sw_ovl_blit_text( panel, buf, 230, 8, y - 3 );
			y += 18;
		}
	}

	for ( i = 0; i < SW_OVL_PARTY_ATTR_COUNT; i++ ) {
		snprintf( buf, sizeof( buf ), "%s %d", sw_ovl_attr_name( i ), sw_ovl_party_abil[ ix ][ i ] );
		sw_ovl_blit_text( panel, buf, 225, 8, y );
		y += 20;
	}

	if ( ix == 1 ) {
		y += 8;
		sw_ovl_chat_lines( panel, sw_ovl_party_chat[ 1 ], 8, y, 3 );
	}

	sw_ovl_party_sheet[ ix ] = sw_ovl_upload_rgba( panel, &sw_ovl_party_sheet_w[ ix ], &sw_ovl_party_sheet_h[ ix ] );
	SDL_FreeSurface( panel );
	return sw_ovl_party_sheet[ ix ] ? 0 : -1;
}

static int sw_ovl_party_refresh( int *slot )
{
	int dirty = 0, k, i;

	slot[0] = 0;
	slot[1] = -1;
	if ( sw_ovl_cdata( SW_OVL_PARTY_CD_EXIST, 0 ) != 1 ) return -1;
	if ( sw_ovl_pick_pet( &slot[1] ) == 0 ) slot[1] = -1;

	for ( k = 0; k < 2; k++ ) {
		int s = slot[k];
		const char *nm = sw_ovl_cdatan( 0, s < 0 ? 0 : s );
		char nb[ 64 ];

		snprintf( nb, sizeof( nb ), "%s", nm );
		if ( strcmp( nb, sw_ovl_party_name[k] ) != 0 ) {
			dirty = 1;
			strncpy( sw_ovl_party_name[k], nb, sizeof( sw_ovl_party_name[k] ) - 1 );
			sw_ovl_party_name[k][ sizeof( sw_ovl_party_name[k] ) - 1 ] = '\0';
		}
		if ( s < 0 ) {
			for ( i = 0; i < SW_OVL_PARTY_ATTR_COUNT; i++ ) sw_ovl_party_abil[k][i] = 0;
			sw_ovl_party_hp[k] = sw_ovl_party_mhp[k] = 0;
			sw_ovl_party_sp[k] = sw_ovl_party_msp[k] = 0;
			sw_ovl_party_mp[k] = sw_ovl_party_mmp[k] = 0;
			continue;
		}
		if ( sw_ovl_party_hp[k] != sw_ovl_cdata( SW_OVL_PARTY_CD_HP, s ) ) dirty = 1;
		if ( sw_ovl_party_mhp[k] != sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_HP, s ) ) dirty = 1;
		if ( sw_ovl_party_sp[k] != sw_ovl_cdata( SW_OVL_PARTY_CD_SP, s ) ) dirty = 1;
		if ( sw_ovl_party_msp[k] != sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_SP, s ) ) dirty = 1;
		if ( sw_ovl_party_mp[k] != sw_ovl_cdata( SW_OVL_PARTY_CD_MP, s ) ) dirty = 1;
		if ( sw_ovl_party_mmp[k] != sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_MP, s ) ) dirty = 1;
		sw_ovl_party_hp[k] = sw_ovl_cdata( SW_OVL_PARTY_CD_HP, s );
		sw_ovl_party_mhp[k] = sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_HP, s );
		sw_ovl_party_sp[k] = sw_ovl_cdata( SW_OVL_PARTY_CD_SP, s );
		sw_ovl_party_msp[k] = sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_SP, s );
		sw_ovl_party_mp[k] = sw_ovl_cdata( SW_OVL_PARTY_CD_MP, s );
		sw_ovl_party_mmp[k] = sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_MP, s );
		for ( i = 0; i < SW_OVL_PARTY_ATTR_COUNT; i++ ) {
			int v = sw_ovl_sdata( SW_OVL_PARTY_ATTR_FIRST + i, s ) / 10000;
			if ( v != sw_ovl_party_abil[k][i] ) dirty = 1;
			sw_ovl_party_abil[k][i] = v;
		}
		if ( k == 1 ) {
			const char *m = sw_ovl_msg_peek( sw_ovl_party_name[1] );
			char mb[ 200 ];
			snprintf( mb, sizeof( mb ), "%s", m );
			if ( strcmp( mb, sw_ovl_party_chat[1] ) != 0 ) {
				dirty = 1;
				strncpy( sw_ovl_party_chat[1], mb, sizeof( sw_ovl_party_chat[1] ) - 1 );
				sw_ovl_party_chat[1][ sizeof( sw_ovl_party_chat[1] ) - 1 ] = '\0';
			}
		}
	}
	return dirty;
}

void switch_overlay_draw_party( int win_w, int win_h, int origin_x, int game_w, int right_x, int right_w )
{
	int dirty, slot[2], k, i, tex2d_was;
	int sheet_h;
	float ly;

	if ( origin_x < 48 || right_w < 48 ) return;
	if ( sw_ovl_party_resolve() != 0 ) return;
	if ( sw_ovl_font == NULL ) sw_ovl_font = sw_ovl_open_font();
	if ( sw_ovl_font == NULL ) return;
	if ( sw_ovl_face_tex == 0 && sw_ovl_face_load() != 0 ) return;
	if ( sw_ovl_bar_tex[0] == 0 && sw_ovl_bar_upload() != 0 ) return;

	dirty = sw_ovl_party_refresh( slot );
	if ( dirty < 0 ) return;

	sheet_h = win_h - 16;
	if ( sheet_h < 120 ) sheet_h = 120;
	if ( sheet_h > sw_ovl_party_sheet_h[0] + 1 || sheet_h > sw_ovl_party_sheet_h[1] + 1 ) dirty = 1;
	if ( dirty || sw_ovl_party_sheet[0] == 0 || sw_ovl_party_sheet[1] == 0 ) {
		if ( sw_ovl_party_build( 0, origin_x, sheet_h ) != 0 ) return;
		if ( sw_ovl_party_build( 1, right_w, sheet_h ) != 0 ) return;
	}

	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	glOrtho( 0, (GLdouble)win_w, -(GLdouble)win_h, 0, -100, 100 );
	glViewport( 0, 0, win_w, win_h );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	glDisableClientState( GL_COLOR_ARRAY );
	sw_glColorKey( 0, 0 );
	tex2d_was = sw_texture2d_on();
	glEnable( GL_TEXTURE_2D );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );

	ly = (float)( -( win_h - sheet_h ) / 2 );
	if ( sw_ovl_party_sheet[0] != 0 ) {
		sw_ovl_quad( sw_ovl_party_sheet[0], 0.0f, ly, (float)sw_ovl_party_sheet_w[0], (float)sheet_h );
		if ( sw_ovl_face_tex != 0 && sw_ovl_face_w > 0 && sw_ovl_face_h > 0 ) {
			int p = sw_ovl_cdata( SW_OVL_PARTY_CD_SEX, 0 ) * 80 + sw_ovl_cdata( SW_OVL_PARTY_CD_PORTRAIT, 0 );
			int col = p % SW_OVL_PARTY_CELL_COLS;
			int row = p / SW_OVL_PARTY_CELL_COLS;
			float u0 = (float)( col * SW_OVL_PARTY_CELL_W ) / (float)sw_ovl_face_w;
			float v0 = (float)( row * SW_OVL_PARTY_CELL_H ) / (float)sw_ovl_face_h;
			float u1 = (float)( col * SW_OVL_PARTY_CELL_W + SW_OVL_PARTY_CELL_W ) / (float)sw_ovl_face_w;
			float v1 = (float)( row * SW_OVL_PARTY_CELL_H + SW_OVL_PARTY_CELL_H ) / (float)sw_ovl_face_h;
			sw_ovl_quad_uv( sw_ovl_face_tex, (float)SW_OVL_PARTY_AVA_X, ly + (float)SW_OVL_PARTY_AVA_Y,
				(float)SW_OVL_PARTY_AVA_W, (float)SW_OVL_PARTY_AVA_H, u0, v0, u1, v1 );
		}
	}
	if ( sw_ovl_party_sheet[1] != 0 ) {
		sw_ovl_quad( sw_ovl_party_sheet[1], (float)right_x, ly, (float)sw_ovl_party_sheet_w[1], (float)sheet_h );
		if ( slot[1] >= 0 ) {
			int cur[3] = { sw_ovl_party_hp[1], sw_ovl_party_mp[1], sw_ovl_party_sp[1] };
			int mxs[3] = { sw_ovl_party_mhp[1], sw_ovl_party_mmp[1], sw_ovl_party_msp[1] };
			for ( i = 0; i < 3; i++ ) {
				int fw;
				if ( mxs[ i ] <= 0 || cur[ i ] <= 0 ) continue;
				if ( cur[ i ] > mxs[ i ] ) cur[ i ] = mxs[ i ];
				fw = (int)( ( right_w - 44 ) * cur[ i ] / mxs[ i ] );
				if ( fw < 1 ) fw = 1;
				sw_ovl_quad_uv( sw_ovl_bar_tex[ i + 1 ], (float)right_x + 8.0f,
					ly + 20.0f + (float)( i * 18 ), (float)fw, 8.0f, 0.0f, 0.0f, 1.0f, 1.0f );
			}
		}
	}

	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_BLEND );
	if ( !tex2d_was ) glDisable( GL_TEXTURE_2D );
}
