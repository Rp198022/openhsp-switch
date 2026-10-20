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
#include "switch_gbk.h"
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
	{ "ZL",			"射击" },
	{ "减号",		"帮助" },
	{ "L",			"切换标签" },
	{ NULL,			"— 第二层（按住 R）—" },
	{ "左摇杆↑",	"跳跃" },
	{ "左摇杆↓",	"挖掘" },
	{ "左摇杆←",	"给予" },
	{ "左摇杆→",	"关门" },
	{ "十字键↑",	"外观/祈祷" },
	{ "十字键↓",	"供奉" },
	{ "十字键←",	"蘸取" },
	{ "十字键→",	"特性" },
	{ "ZL",			"阅读" },
	{ "加号",		"素材列表" },
	{ "减号",		"观察/锁定" },
	{ "L",			"等待" },
	{ NULL,			"— 第三层（按住 ZR）—" },
	{ "十字键↑",	"数字 8" },
	{ "十字键↓",	"数字 2" },
	{ "十字键←",	"数字 4" },
	{ "十字键→",	"数字 6" },
	{ "左摇杆↑",	"丢弃" },
	{ "左摇杆↓",	"使用道具" },
	{ "左摇杆←",	"擦洗" },
	{ "左摇杆→",	"堆叠" },
	{ "ZL",			"数字 5" },
	{ "L",			"删除存档" },
	{ "减号",		"翻页上" },
};

/*	Right Joy-Con, all three layers.										*/
static const SW_OVL_ROW sw_ovl_right[] = {
	{ "A",			"确认/攻击" },
	{ "B",			"取消/关闭" },
	{ "X",			"行动菜单" },
	{ "Y",			"道具菜单" },
	{ "右摇杆↑",	"咏唱魔法" },
	{ "右摇杆↓",	"搜索周围" },
	{ "右摇杆←",	"使用特技" },
	{ "右摇杆→",	"对话" },
	{ "加号",		"存档/设置" },
	{ NULL,			"— 第二层（按住 R）—" },
	{ "A",			"吃" },
	{ "B",			"喝" },
	{ "X",			"拾取" },
	{ "Y",			"装备" },
	{ "右摇杆↑",	"挥动魔杖" },
	{ "右摇杆↓",	"打开" },
	{ "右摇杆←",	"日志" },
	{ "右摇杆→",	"改变地形" },
	{ "加号",		"素材列表" },
	{ NULL,			"— 第三层（按住 ZR）—" },
	{ "A",			"数字 3" },
	{ "B",			"数字 1" },
	{ "X",			"数字 9" },
	{ "Y",			"数字 7" },
	{ "右摇杆↑",	"消息日志" },
	{ "右摇杆↓",	"砸开" },
	{ "右摇杆←",	"切换面板" },
	{ "右摇杆→",	"换弹" },
	{ "加号",		"翻页下" },
};

#define SW_OVL_LEFT_N	((int)( sizeof( sw_ovl_left ) / sizeof( sw_ovl_left[0] ) ))
#define SW_OVL_RIGHT_N	((int)( sizeof( sw_ovl_right ) / sizeof( sw_ovl_right[0] ) ))

static int		sw_ovl_off = 0;			/* hidden when 1 (shown by default) */
static int		sw_ovl_failed = 0;		/* font/build failure: gives up		*/
static int		sw_ovl_tried = 0;		/* the one-time font open has run	*/
static TTF_Font	*sw_ovl_font = NULL;
static TTF_Font	*sw_ovl_font_small = NULL;
/*	The pet's chatter is the text the panel is read for, so it gets its own
	face - a touch larger than the other small labels and without the bold,
	which at this size closes the strokes of a glyph into a blob.			*/
static TTF_Font	*sw_ovl_font_chat = NULL;
static GLuint	sw_ovl_tex[2] = { 0, 0 };	/* 0 = left strip, 1 = right strip	*/
static int		sw_ovl_tex_w[2] = { 0, 0 };
static int		sw_ovl_tex_h[2] = { 0, 0 };
static int		sw_ovl_origin_x = -1;	/* layout the textures were built for */

/*	Display mode (r249).  Three are under the player's control and cycle on
	ZR+stick-left (or R+stick click); the fourth is automatic.  The cycle is
	key hints -> party -> party detail: the party sheet carries what is worth
	a glance at any time (both portraits, the bars, the pet's chatter), the
	detail sheet the figures consulted now and then (abilities, gear).  The
	battle sheet hangs off the same point - an engaged enemy overrides the
	player's pick and the pick comes back once the fight is over - and is
	never reached by hand.												*/
#define SW_OVL_MODE_KEY		0
#define SW_OVL_MODE_PARTY	1		/* portraits, bars, pet talk	*/
#define SW_OVL_MODE_DETAIL	2		/* portraits, abilities, gear	*/
#define SW_OVL_MODE_BATTLE	3		/* hidden until a fight is on	*/
static int		sw_ovl_manual = SW_OVL_MODE_KEY;	/* the player's pick	*/
static int		sw_ovl_mode = SW_OVL_MODE_KEY;		/* what gets drawn	*/
static int		sw_ovl_battle_on = 0;				/* an enemy engaged	*/
static int		sw_ovl_battle_slot = -1;			/* that enemy's slot */
static int		sw_ovl_party_built_mode = -1;		/* mode the sheets hold */

/*	Battle sheet (r247): the nearest enemy that is actually fighting us,
	together with its live condition counters.  The field numbers are the
	game's own CDATA_CONDITION_* block (defines/mod.hsp).					*/
#define SW_OVL_BAT_COND_N	9
static const struct { int field; const char *name; } sw_ovl_bat_cond[ SW_OVL_BAT_COND_N ] = {
	{ 250, "中毒" }, { 254, "混乱" }, { 252, "麻痹" }, { 251, "睡眠" },
	{ 253, "盲目" }, { 255, "恐惧" }, { 258, "流血" }, { 256, "暗影" },
	{ 261, "生病" },
};
static char		sw_ovl_bat_name[ 64 ] = "";
static int		sw_ovl_bat_hp = 0, sw_ovl_bat_mhp = 0;
static int		sw_ovl_bat_condv[ SW_OVL_BAT_COND_N ];

/*	r254: the player's own always-checked numbers, drawn under the bars on
	the party sheet's left strip.  All of them are plain cdata/gdata fields;
	the weight pair uses the game's cnvweight() scale (value / 1000, one
	decimal).  There is no fatigue field in Elona+ - HUNGER and the sleep
	condition are the only state the script keeps - so fame takes that slot. */
#define SW_OVL_PLY_LINE_H		14
#define SW_OVL_PLY_CD_HUNGER	22
#define SW_OVL_PLY_CD_GOLD		30
#define SW_OVL_PLY_CD_PLATINUM	31
#define SW_OVL_PLY_CD_FAME		34
#define SW_OVL_PLY_CD_WEIGHT	43
#define SW_OVL_PLY_CD_CARRY		44
#define SW_OVL_PLY_CD_KARMA		49
#define SW_OVL_PLY_CD_GOD		61
#define SW_OVL_PLY_CD_PIETY		62
#define SW_OVL_PLY_CD_SANITY	86
#define SW_OVL_GD_YEAR			10
#define SW_OVL_GD_MONTH			11
#define SW_OVL_GD_DAY			12
#define SW_OVL_GD_HOUR			13
#define SW_OVL_GD_WEATHER		17
static int		sw_ovl_ply_cond[ SW_OVL_BAT_COND_N ];
static int		sw_ovl_ply_hunger = -1, sw_ovl_ply_gold = -1, sw_ovl_ply_plat = -1;
static int		sw_ovl_ply_year = -1, sw_ovl_ply_month = -1, sw_ovl_ply_day = -1;
static int		sw_ovl_ply_hour = -1, sw_ovl_ply_weather = -1;
static int		sw_ovl_ply_karma = 0, sw_ovl_ply_god = -1, sw_ovl_ply_piety = -1;
static int		sw_ovl_ply_sanity = -1, sw_ovl_ply_fame = -1;
static int		sw_ovl_ply_weight = -1, sw_ovl_ply_carry = -1;

/*	Party HUD state.  The interpreter context is the cache key: when it
	changes (new game, load) the variable indices are resolved again.
	The face atlas and the 1x1 bar swatches are lazily uploaded on first
	use and rebuilt after a GL context reset.						*/
static HSPCTX	*sw_ovl_ctx = NULL;
static PVal		*sw_ovl_pv_cdata = NULL;
static PVal		*sw_ovl_pv_gdata = NULL;
static PVal		*sw_ovl_pv_cdatan = NULL;
static PVal		*sw_ovl_pv_sdata = NULL;
static PVal		*sw_ovl_pv_inv = NULL;	/* item data: dim 70,5480 INT	*/
static PVal		*sw_ovl_pv_msg = NULL;
static PVal		*sw_ovl_pv_msgline = NULL;
static PVal		*sw_ovl_pv_inf_maxlog = NULL;

/*	r203: the most recent custom-talk speaker.  chara_func.hsp:207 sets
	`chatc = customtalk_charid` before dispatching any custom line and
	nothing clears it, so it stays valid for every trigger.  Reaching it
	needs the .ax variable-name table: present on the 2.33 build (hspcmp
	emits the debug segment, max_dinfo=22568) but empty on the shipped
	2.30 .ax (max_dinfo=2), where this stays NULL and the caller must
	fall back to matching the speaker name written into the talk file. */
static PVal		*sw_ovl_pv_chatc = NULL;
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
static int		sw_ovl_party_rank[2][ 8 ];
static int		sw_ovl_party_fix[2][ 5 ];
static GLuint	sw_ovl_face_u_tex[2] = { 0, 0 };
static int		sw_ovl_party_rel[2] = { 0, 0 };
static int		sw_ovl_wep[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
static int		sw_ovl_face_u_id[2] = { 0, 0 };
/*	Pet chat: the latest pet-related log lines joined by '\n', newest
	last.  sw_ovl_msg_peek() collects up to SW_OVL_PET_ROWS lines from the
	game's log ring; sw_ovl_chat_lines() renders one line each, older
	lines scrolling up as new ones arrive like the game's own log.		*/
#define SW_OVL_PET_ROWS	6
static char		sw_ovl_party_chat[2][ 1200 ] = { "", "" };

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
	sw_ovl_pv_gdata = NULL;
	sw_ovl_pv_cdatan = NULL;
	sw_ovl_pv_sdata = NULL;
	sw_ovl_pv_msg = NULL;
	sw_ovl_pv_msgline = NULL;
	sw_ovl_pv_inf_maxlog = NULL;
	sw_ovl_pv_chatc = NULL;
	sw_ovl_pv_inv = NULL;
	sw_ovl_party_sheet[0] = 0;
	sw_ovl_party_sheet[1] = 0;
	sw_ovl_party_slot[0] = 0;
	sw_ovl_party_slot[1] = -1;
	sw_ovl_party_built_mode = -1;
	sw_ovl_bat_name[0] = '\0';
	sw_ovl_face_u_tex[0] = 0;
	sw_ovl_face_u_tex[1] = 0;
	sw_ovl_face_u_id[0] = 0;
	sw_ovl_face_u_id[1] = 0;
}


void switch_overlay_draw_party( int win_w, int win_h, int origin_x, int game_w, int right_x, int right_w );
static void sw_ovl_battle_update( void );

void switch_overlay_toggle( void )
{
	/*	r249: the cycle carries the player's pick - key hints -> party ->
		party detail -> key hints.  The battle sheet is not on it; an engaged
		enemy brings it up by itself and it cannot be reached by hand.	*/
	static const char *name[3] = { "keys", "party", "detail" };
	sw_ovl_manual = ( sw_ovl_manual + 1 ) % 3;
	printf( "hsp3switch: overlay %s\n", name[ sw_ovl_manual ] );
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
			if ( sw_ovl_font_small == NULL ) {
				sw_ovl_font_small = TTF_OpenFont( cand[i], 11 );
				if ( sw_ovl_font_small != NULL ) TTF_SetFontStyle( sw_ovl_font_small, TTF_STYLE_BOLD );
			}
			if ( sw_ovl_font_chat == NULL ) {
				/*	r252: the chatter is meant to be read, and the bold the other
					small labels carry closed its counters into blobs at that
					size; a touch larger and without the bold keeps the strokes
					apart on the handheld screen.							*/
				sw_ovl_font_chat = TTF_OpenFont( cand[i], 13 );
			}
			printf( "hsp3switch: overlay font %s\n", cand[i] );
			fflush( stdout );
			return f;
		}
	}
	printf( "hsp3switch: overlay: no usable font, hints disabled\n" );
	fflush( stdout );
	return NULL;
}

/*	Same as sw_ovl_blit_text but with the smaller rating font.			*/
static void sw_ovl_blit_small( SDL_Surface *dst, const char *text, Uint8 v, int x, int y )
{
	SDL_Color col;
	SDL_Surface *s, *c;
	SDL_Rect at;

	if ( sw_ovl_font_small == NULL ) return;
	col.r = col.g = col.b = v;
	col.a = 255;
	s = TTF_RenderUTF8_Blended( sw_ovl_font_small, text, col );
	if ( s == NULL ) return;
	c = SDL_ConvertSurfaceFormat( s, SDL_PIXELFORMAT_ABGR8888, 0 );
	SDL_FreeSurface( s );
	if ( c == NULL ) return;
	at.x = x;
	at.y = y;
	SDL_BlitSurface( c, NULL, dst, &at );
	SDL_FreeSurface( c );
}
static void sw_ovl_blit_small_rgb( SDL_Surface *dst, const char *text, Uint8 r, Uint8 g, Uint8 b, int x, int y )
{
	SDL_Color col;
	SDL_Surface *s, *c;
	SDL_Rect at;

	if ( sw_ovl_font_small == NULL ) return;
	col.r = r; col.g = g; col.b = b;
	col.a = 255;
	s = TTF_RenderUTF8_Blended( sw_ovl_font_small, text, col );
	if ( s == NULL ) return;
	c = SDL_ConvertSurfaceFormat( s, SDL_PIXELFORMAT_ABGR8888, 0 );
	SDL_FreeSurface( s );
	if ( c == NULL ) return;
	at.x = x;
	at.y = y;
	SDL_BlitSurface( c, NULL, dst, &at );
	SDL_FreeSurface( c );
}

/*	One line of the pet's chatter, in the smaller face; falls back to the
	ordinary small font if that one never opened.							*/
static void sw_ovl_blit_chat( SDL_Surface *dst, const char *text, Uint8 r, Uint8 g, Uint8 b, int x, int y )
{
	SDL_Color col;
	SDL_Surface *s, *c;
	SDL_Rect at;

	if ( sw_ovl_font_chat == NULL ) { sw_ovl_blit_small_rgb( dst, text, r, g, b, x, y ); return; }
	col.r = r; col.g = g; col.b = b;
	col.a = 255;
	s = TTF_RenderUTF8_Blended( sw_ovl_font_chat, text, col );
	if ( s == NULL ) return;
	c = SDL_ConvertSurfaceFormat( s, SDL_PIXELFORMAT_ABGR8888, 0 );
	SDL_FreeSurface( s );
	if ( c == NULL ) return;
	at.x = x;
	at.y = y;
	SDL_BlitSurface( c, NULL, dst, &at );
	SDL_FreeSurface( c );
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

	/*	r247: pick what to show first.  An engaged enemy overrides the
		player's choice with the battle sheet; the choice comes back once the
		fight is over.  The battle sheet is never on the manual cycle.		*/
	sw_ovl_battle_update();
	sw_ovl_mode = sw_ovl_battle_on ? SW_OVL_MODE_BATTLE : sw_ovl_manual;

	/*	Party HUD / battle sheet replace the key hints entirely: same window
		pass, their own build/draw path below.							*/
	if ( sw_ovl_mode != SW_OVL_MODE_KEY ) {
		switch_overlay_draw_party( win_w, win_h, origin_x, game_w, right_x, right_w );
		return;
	}

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
#define SW_OVL_PARTY_CD_X		1
#define SW_OVL_PARTY_CD_Y		2
#define SW_OVL_PARTY_CD_RELATION	9
#define SW_OVL_PARTY_CD_TARGET	29
#define SW_OVL_PARTY_CD_AI_AGGRO	201

#define SW_OVL_PARTY_ATTR_FIRST		10	/* SKILL_ATTR_STR */
#define SW_OVL_PARTY_ATTR_COUNT		8	/* STR..CHA */
#define SW_OVL_PARTY_SKILL_OFF	600	/* STARTING_SKILL_SPACT: sdata base for skills */
#define SW_OVL_PARTY_SLOTS		16	/* MAX_CHARA_FOLLOWER */

#define SW_OVL_PARTY_BOX_X		4	/* portrait box inset from the strip edge */
#define SW_OVL_PARTY_BOX_Y		24	/* box top: below the name line */
#define SW_OVL_PARTY_BOX_PAD		12	/* frame thickness kept around the face */
#define SW_OVL_PARTY_NAME_Y		4	/* small top margin */
#define SW_OVL_PARTY_STAT_PITCH		30	/* label row + bar row per stat */

/*	face1.bmp is a 16-column atlas of 48x72 cells (800x744 bitmap).	*/
#define SW_OVL_PARTY_CELL_W		48
#define SW_OVL_PARTY_CELL_H		72
#define SW_OVL_PARTY_CELL_COLS		16

static PVal *sw_ovl_find_dim( HSPCTX *ctx, int flag, int l1, int l2, int l3 )
{
	int i, n = (int)ctx->hsphed->max_val;
	for ( i = 0; i < n; i++ ) {
		PVal *pv = &ctx->mem_var[ i ];
		if ( (int)pv->flag != flag ) continue;
		if ( l1 >= 0 && pv->len[1] != l1 ) continue;
		if ( l2 >= 0 && pv->len[2] != l2 ) continue;
		if ( l3 >= 0 && pv->len[3] != l3 ) continue;
		return pv;
	}
	return NULL;
}

static int sw_ovl_party_resolve( void )
{
	HSPCTX *ctx;
	int id;

	if ( sw_ovl_ctx != NULL ) return 0;
	ctx = switch_runtime_hspctx();
	if ( ctx == NULL ) return -1;

	/*	Elona ships its .ax without the debug variable-name table, so
		code_getdebug_seekvar() cannot resolve anything at all: mem_di_val
		stays NULL and every lookup returns -1 even though max_val is set.
		The arrays this HUD needs are therefore found by their exact
		dimensions, which Elona fixes: cdata dim 500,245, sdata dim 1200,245,
		cdatan sdim 40,10,245 -> PVal 10x245 (sdim's first argument is the
		string length, stored in len[0]; the axes land in len[1],len[2]).
		The name route is still tried first in case a debug build appears. */
	sw_ovl_pv_cdata = NULL;
	sw_ovl_pv_gdata = NULL;
	sw_ovl_pv_sdata = NULL;
	sw_ovl_pv_cdatan = NULL;
	sw_ovl_pv_msg = NULL;
	sw_ovl_pv_msgline = NULL;
	sw_ovl_pv_inf_maxlog = NULL;
	sw_ovl_pv_chatc = NULL;

	id = code_getdebug_seekvar( "cdata" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_cdata = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "sdata" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_sdata = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "cdatan" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_cdatan = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "msg" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_msg = &ctx->mem_var[ id ];	printf( "ovldiag: seek msg id=%d max=%d\n", id, (int)ctx->hsphed->max_val ); fflush( stdout );
	id = code_getdebug_seekvar( "msgline" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_msgline = &ctx->mem_var[ id ];
	id = code_getdebug_seekvar( "inf_maxlog" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_inf_maxlog = &ctx->mem_var[ id ];

	id = code_getdebug_seekvar( "chatc" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_chatc = &ctx->mem_var[ id ];

	id = code_getdebug_seekvar( "gdata" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_gdata = &ctx->mem_var[ id ];
	if ( sw_ovl_pv_gdata == NULL )
		sw_ovl_pv_gdata = sw_ovl_find_dim( ctx, HSPVAR_FLAG_INT, 1000, 1, -1 );

	if ( sw_ovl_pv_cdata == NULL )  sw_ovl_pv_cdata  = sw_ovl_find_dim( ctx, HSPVAR_FLAG_INT, 500, 245, -1 );
	if ( sw_ovl_pv_sdata == NULL )  sw_ovl_pv_sdata  = sw_ovl_find_dim( ctx, HSPVAR_FLAG_INT, 1200, 245, -1 );
	if ( sw_ovl_pv_cdatan == NULL ) sw_ovl_pv_cdatan = sw_ovl_find_dim( ctx, HSPVAR_FLAG_STR, 10, 245, -1 );
	if ( sw_ovl_pv_inv == NULL )    sw_ovl_pv_inv    = sw_ovl_find_dim( ctx, HSPVAR_FLAG_INT, 70, 5480, -1 );
	if ( sw_ovl_pv_msg == NULL ) {
		/* msg = sdim msg,120,inf_maxlog : a one-dim STR array.  len[1] holds
		   the runtime inf_maxlog and len[0] is always 1 in openhsp, so
		   neither can identify it; its index in the Elona+ 2.32 Chinese .ax
		   variable table (743, seen in the Eden boot log) does.  The guard
		   len[1]>=10 keeps a wrong index from being mistaken for msg. */
		/*	743 was the book-title array, not the log ring; the real msg
			is the STR array whose log lines carry a leading space.  The .ax
			has no debug name table, so this scan is the way in.			*/
		{
			int ii;
			for ( ii = 0; ii < (int)ctx->hsphed->max_val; ii++ ) {
				PVal *pv = &ctx->mem_var[ ii ];
				const char *sc;
				if ( pv->flag != HSPVAR_FLAG_STR ) continue;
				if ( pv->len[0] * pv->len[1] < 2 ) continue;
				sc = (const char*)HspVarCorePtrAPTR( pv, 0 );
				if ( sc != NULL && sc[0] == ' ' ) {
					sw_ovl_pv_msg = pv;
					printf( "ovldiag: msg idx=%d len1=%d first=%s\n", ii, pv->len[1], sc ); fflush( stdout );
					break;
				}
			}
		}
		}


	if ( sw_ovl_pv_cdata == NULL || sw_ovl_pv_sdata == NULL || sw_ovl_pv_cdatan == NULL ) return -1;

	sw_ovl_ctx = ctx;
	return 0;
}

static int sw_ovl_cdata( int field, int slot )
{
	if ( sw_ovl_pv_cdata == NULL ) return 0;
	if ( field < 0 || field >= sw_ovl_pv_cdata->len[1] ) return 0;
	if ( slot < 0 || slot >= sw_ovl_pv_cdata->len[2] ) return 0;
	return ((int*)sw_ovl_pv_cdata->pt)[ field + slot * sw_ovl_pv_cdata->len[1] ];
}

/*	Global counters (dim gdata,1000 in init.hsp) - the clock, the weather. */
static int sw_ovl_gdata( int field )
{
	if ( sw_ovl_pv_gdata == NULL ) return 0;
	if ( field < 0 || field >= sw_ovl_pv_gdata->len[1] ) return 0;
	return ((int*)sw_ovl_pv_gdata->pt)[ field ];
}

/*	r247: is somebody fighting us right now, and who?  A character counts
	when it is alive, hostile and actually engaged:
	  relation < 0 - how Elona marks an enemy (action.hsp's duel setup sets
	    relation -3 + aggro 80), while the player's own side sits at 10;
	  aggro > 0   - the AI's "I am after somebody" counter.  It is raised
	    when a monster notices or is hit and reset to 0 once it calms down
	    (ai.hsp), so an unaware monster wandering past never counts.
	TARGET is deliberately not consulted: cdata(CDATA_TARGET) is 0 both for
	"the player" and for the default of a character that has picked nobody.
	The nearest engaged enemy is the one the battle sheet shows.
	Resolving the arrays is heavy (the shipped .ax has no debug name table,
	so they are found by a full mem_var scan), so a failed attempt is retried
	only twice a second - the script's own arrays exist from program start,
	so this normally succeeds on the first frame the interpreter is up.		*/
static void sw_ovl_battle_update( void )
{
	static unsigned int tick = 0;
	int s, n, px, py, best = -1, bestd = 0;

	sw_ovl_battle_on = 0;
	sw_ovl_battle_slot = -1;
	if ( sw_ovl_pv_cdata == NULL && sw_ovl_ctx == NULL ) {
		if ( ( tick++ % 30 ) != 0 ) return;
		if ( sw_ovl_party_resolve() != 0 ) return;
	}
	if ( sw_ovl_pv_cdata == NULL ) return;

	n = sw_ovl_pv_cdata->len[2];
	px = sw_ovl_cdata( SW_OVL_PARTY_CD_X, 0 );
	py = sw_ovl_cdata( SW_OVL_PARTY_CD_Y, 0 );
	for ( s = 1; s < n; s++ ) {
		int dx, dy, d;
		if ( sw_ovl_cdata( SW_OVL_PARTY_CD_EXIST, s ) != 1 ) continue;
		if ( sw_ovl_cdata( SW_OVL_PARTY_CD_RELATION, s ) > -1 ) continue;
		if ( sw_ovl_cdata( SW_OVL_PARTY_CD_AI_AGGRO, s ) <= 0 ) continue;
		dx = sw_ovl_cdata( SW_OVL_PARTY_CD_X, s ) - px;
		dy = sw_ovl_cdata( SW_OVL_PARTY_CD_Y, s ) - py;
		if ( dx < 0 ) dx = -dx;
		if ( dy < 0 ) dy = -dy;
		d = ( dx > dy ) ? dx : dy;
		if ( best < 0 || d < bestd ) { best = s; bestd = d; }
	}
	if ( best < 0 ) return;
	sw_ovl_battle_on = 1;
	sw_ovl_battle_slot = best;
}

/*	r203: charid of the last custom-talk speaker, or -1 when the name
	table is unavailable (2.30) or the value is out of range. */
static int sw_ovl_speaker( void )
{
	int id;
	if ( sw_ovl_pv_chatc == NULL ) return -1;
	id = ( (int *)sw_ovl_pv_chatc->pt )[ 0 ];
	if ( id < 0 || id >= 245 ) return -1;
	return id;
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

static int sw_ovl_inv( int field, int idx )
{
	if ( sw_ovl_pv_inv == NULL ) return 0;
	if ( field < 0 || field >= sw_ovl_pv_inv->len[1] ) return 0;
	if ( idx < 0 || idx >= sw_ovl_pv_inv->len[2] ) return 0;
	return ((int*)sw_ovl_pv_inv->pt)[ field + idx * sw_ovl_pv_inv->len[1] ];
}

static int sw_ovl_lim( int v, int lo, int hi )
{
	return v < lo ? lo : ( v > hi ? hi : v );
}

/*	Reproduce the character sheet's weapon lines (command.hsp
	*com_skill_calcAttack, note mode): mode 0 = melee (skip the ammo/extra
	slots 10/11), mode 1 = ranged (the shoot slot 5).  out = dice1, dice2,
	dmgfix, hit%.  Equip slot value = slot*10000 + (item index + 1).			*/
static void sw_ovl_calc_wep( int mode, int *out )
{
	int cnt, cw = -1, st, v, sk, d1, d2, fix, hit;

	out[0] = out[1] = out[2] = out[3] = 0;
	for ( cnt = 0; cnt < 30; cnt++ ) {
		v = sw_ovl_cdata( 100 + cnt, 0 );
		if ( v <= 0 ) continue;
		st = v / 10000;
		if ( mode == 0 ) {
			if ( st == 10 || st == 11 ) continue;
		} else {
			if ( st != 5 ) continue;
		}
		cw = v % 10000 - 1;
		if ( cw < 0 ) { cw = -1; continue; }
		if ( sw_ovl_inv( 10, cw ) <= 0 ) { cw = -1; continue; }
		break;
	}
	if ( cw < 0 ) return;

	sk = sw_ovl_inv( 16, cw );
	if ( sk <= 0 ) sk = 106;
	d1 = sw_ovl_inv( 10, cw ); if ( d1 <= 0 ) d1 = 4;
	d2 = sw_ovl_inv( 11, cw ); if ( d2 <= 0 ) d2 = 4;
	fix = sw_ovl_cdata( 73, 0 ) + sw_ovl_inv( 12, cw ) + sw_ovl_inv( 20, cw )
		+ ( sw_ovl_inv( 17, cw ) == 1 )
		+ sw_ovl_sdata( 15, 0 ) / 10 + sw_ovl_sdata( 10, 0 ) / 30 + sw_ovl_sdata( 13, 0 ) / 30
		+ sw_ovl_lim( ( sw_ovl_inv( 7, cw ) + 1 ) / 5000, 0, 40 )
		+ sw_ovl_sdata( sk, 0 ) / 50 * 5;
	hit = sw_ovl_sdata( 12, 0 ) / 4 + sw_ovl_sdata( sw_ovl_inv( 16, cw ), 0 ) / 3
		+ sw_ovl_sdata( sk, 0 ) + 50
		+ sw_ovl_cdata( 72, 0 ) + sw_ovl_inv( 13, cw ) + sw_ovl_inv( 20, cw )
		+ ( sw_ovl_inv( 17, cw ) == 1 );

	out[0] = d1; out[1] = d2; out[2] = fix; out[3] = hit;
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

/*	GBK bytes -> UTF-8 in a static scratch buffer.  The Chinese Elona
	overlay font is SimHei (has the Han glyphs): the tofu boxes were an
	encoding problem, not a missing-glyph one.  ASCII passes through,
	every GBK lead byte is looked up and emitted as UTF-8.			*/
static const char *sw_ovl_utf8( const char *in )
{
	static char out[ 2048 ];
	const unsigned char *p = (const unsigned char*)in;
	unsigned char *q = (unsigned char*)out;
	unsigned short u;

	while ( *p && ( q - (unsigned char*)out ) < (int)sizeof( out ) - 4 ) {
		if ( *p < 0x80 ) { *q++ = *p++; continue; }
		if ( *p >= 0x81 && *p <= 0xFE && p[1] >= 0x40 && p[1] <= 0xFE ) {
			u = sw_gbk_uni[ *p - 0x81 ][ p[1] - 0x40 ];
			p += 2;
			if ( u == 0 ) { *q++ = '?'; continue; }
		} else { *q++ = '?'; p++; continue; }
		if ( u < 0x800 ) {
			*q++ = (unsigned char)( 0xC0 | ( u >> 6 ) );
			*q++ = (unsigned char)( 0x80 | ( u & 0x3F ) );
		} else {
			*q++ = (unsigned char)( 0xE0 | ( u >> 12 ) );
			*q++ = (unsigned char)( 0x80 | ( ( u >> 6 ) & 0x3F ) );
			*q++ = (unsigned char)( 0x80 | ( u & 0x3F ) );
		}
	}
	*q = 0;
	return out;
}

/*	Latest log lines belonging to the pet, joined oldest-first.  Two routes,
	in this order: the game names the talker itself (chara_func.hsp:207 sets
	chatc = customtalk_charid before dispatching a custom line), so the
	NEWEST log line is credited to the pet only when chatc says it is the
	pet's and is taken off the name route otherwise - a line that merely
	mentions the pet's name is not the pet's line.  Where chatc cannot be
	read (the 2.30 .ax carries no variable-name table) the raw (GBK) bytes
	are matched against the pet's name tokens instead, byte-wise, no
	transcoding.  The ring is walked from the newest cell backward (msgline
	itself has no reliable decode).										*/
static const char *sw_ovl_msg_peek( const char *pet_name, int speaker, int pet_slot )
{
	int i, got = 0, kk, newest = -1;
	const char *hits[ SW_OVL_PET_ROWS ];
	const char *s;
	char keys[4][ 64 ]; int nk = 0;
	static int once = 0;
	static int dbg = 0;
	static char out[ 1200 ];

	if ( sw_ovl_pv_msg == NULL ) return "";

	/*	The log quotes a pet by any of its name tokens (title, given name or
		full "title name" string), so accept the line if it contains ANY one
		of the tokens - a line can name the pet without the title prefix.  */
	if ( pet_name != NULL ) {
		const char *p = pet_name;
		const char *p0 = pet_name;
		int sl;
		while ( *p != '\0' && nk < 4 ) {
			sl = 0;
			while ( p[ sl ] != '\0' && p[ sl ] != ' ' ) sl++;
			if ( sl == 0 ) { if ( *p == '\0' ) break; p++; continue; }
			if ( sl > 63 ) sl = 63;
			memcpy( keys[ nk ], p, sl ); keys[ nk ][ sl ] = '\0'; nk++;
			p += sl;
			while ( *p == ' ' ) p++;
		}
		if ( nk > 0 ) {
			/* keep the full string too - some lines quote the whole name */
			sl = 0; while ( pet_name[ sl ] != '\0' && sl < 63 ) sl++;
			memcpy( keys[ nk ], pet_name, sl ); keys[ nk ][ sl ] = '\0'; nk++;
		}
	}
	if ( !once ) { once = 1;
		printf( "ovlmsg: nk=%d \"%s\"\n", nk, nk ? keys[0] : "(none)" ); fflush( stdout );
		for ( i = 0; i < (int)sw_ovl_pv_msg->len[1]; i++ ) {
			s = (const char*)HspVarCorePtrAPTR( sw_ovl_pv_msg, i );
			if ( s != NULL && s[0] != '\0' )
				printf( "ovlmsg[%d]: %s\n", i, s ); fflush( stdout );
		}
	}
	/*	Collect the latest pet lines (newest first).  The newest line goes by
		chatc where that is readable; the older ones count when they carry
		any of the name tokens.  Then join them oldest-first.			*/
	for ( i = (int)sw_ovl_pv_msg->len[1] - 1; i >= 0 && got < SW_OVL_PET_ROWS; i-- ) {
		s = (const char*)HspVarCorePtrAPTR( sw_ovl_pv_msg, i );
		if ( s == NULL || s[ 0 ] == '\0' ) continue;
		if ( newest < 0 ) newest = i;	/* first filled cell = newest line */
		if ( i == newest && speaker >= 0 && pet_slot >= 0 ) {
			/*	chatc names the talker of the newest line: keep it for the pet
				only when it is the pet, and take it off the name route when
				it is not - being mentioned is not talking.  A quoted line is
				the pet's even when the file writes no name into it.		*/
			if ( speaker != pet_slot ) continue;
			if ( strchr( s, '"' ) != NULL ) { hits[ got++ ] = s; continue; }
		}
		if ( nk == 0 ) continue;
		for ( kk = 0; kk < nk; kk++ )
			if ( keys[ kk ][ 0 ] != '\0' && strstr( s, keys[ kk ] ) != NULL ) {
				hits[ got++ ] = s; break;
			}
	}
	out[ 0 ] = '\0';
	while ( got > 0 ) {
		size_t l = strlen( hits[ got - 1 ] );
		size_t o = strlen( out );
		if ( o + l + 1 >= sizeof( out ) ) break;
		memcpy( out + o, hits[ got - 1 ], l );
		o += l;
		if ( --got > 0 ) out[ o++ ] = '\n';
		out[ o ] = '\0';
	}
	return out;
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

/*	Ability rating words, exactly as the game's status screen draws them
	(command.hsp): p = sdata(skill + STARTING_SKILL_SPACT) \ 1000 with
	thresholds 400/300/200/150/100/50 -> Supreme..Hopeless.  HSP '\' is
	integer division, so a C '/' on the non-negative value does the same. */
static const char *sw_ovl_rank_name( int p )
{
	if ( p >= 400 ) return "Supreme";
	if ( p >= 300 ) return "Amazing";
	if ( p >= 200 ) return "Superb";
	if ( p >= 150 ) return "Great";
	if ( p >= 100 ) return "Good";
	if ( p >= 50 ) return "Bad";
	return "Hopeless";
}

/*	u-series portrait: CDATA_PORTRAIT < 0 picks user/graphic/faceN.bmp as
	a whole 80x112 image, N = abs(portrait + 1); portrait == -1 means "none"
	and is skipped, mirroring command.hsp.  One texture per strip, replaced
	when the tracked character's portrait index changes.			*/
static void sw_ovl_face_u_load( int ix, int n )
{
	char path[ 128 ];
	SDL_Surface *bmp, *conv;
	GLuint tex;

	if ( n == sw_ovl_face_u_id[ ix ] && sw_ovl_face_u_tex[ ix ] != 0 ) return;
	if ( sw_ovl_face_u_tex[ ix ] != 0 ) {
		glDeleteTextures( 1, &sw_ovl_face_u_tex[ ix ] );
		sw_ovl_face_u_tex[ ix ] = 0;
	}
	sw_ovl_face_u_id[ ix ] = 0;
	if ( n <= 0 ) return;
	snprintf( path, sizeof( path ), "sdmc:/switch/openhsp/user/graphic/face%d.bmp", n );
	bmp = SDL_LoadBMP( path );
	if ( bmp == NULL ) {
		printf( "hsp3switch: overlay: u-face %d missing, atlas fallback\n", n );
		fflush( stdout );
		return;
	}
	conv = SDL_ConvertSurfaceFormat( bmp, SDL_PIXELFORMAT_ABGR8888, 0 );
	SDL_FreeSurface( bmp );
	if ( conv == NULL ) return;
	tex = sw_ovl_upload_rgba( conv, NULL, NULL );
	SDL_FreeSurface( conv );
	if ( tex == 0 ) return;
	sw_ovl_face_u_tex[ ix ] = tex;
	sw_ovl_face_u_id[ ix ] = n;
	printf( "hsp3switch: overlay u-face %d loaded\n", n );
	fflush( stdout );
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

/*	Portrait box geometry.  The box fills the letterbox strip and the face is
	scaled to the 48x72 cell's aspect inside it; the even margin left over is
	painted as the frame, so the portrait can fill the strip without being
	stretched.  Both the sheet builder (which paints the frame) and the
	frame-time face quad call this, so the two always line up.			*/
static void sw_ovl_party_ava_geom( int strip_w, int *bx, int *by, int *bw, int *bh,
					int *ax, int *ay, int *aw, int *ah )
{
	*bx = SW_OVL_PARTY_BOX_X;
	*by = SW_OVL_PARTY_BOX_Y;
	*bw = strip_w - SW_OVL_PARTY_BOX_X * 2;
	if ( *bw < 32 ) *bw = 32;
	*aw = *bw - SW_OVL_PARTY_BOX_PAD * 2;
	if ( *aw < 24 ) *aw = 24;
	*ah = *aw * SW_OVL_PARTY_CELL_H / SW_OVL_PARTY_CELL_W;
	*bh = *ah + SW_OVL_PARTY_BOX_PAD * 2;
	*ax = *bx + SW_OVL_PARTY_BOX_PAD;
	*ay = *by + SW_OVL_PARTY_BOX_PAD;
}

/*	Top of the row block that sits under the portrait box.				*/
static int sw_ovl_party_rows_y( int strip_w )
{
	int bx, by, bw, bh, ax, ay, aw, ah;
	sw_ovl_party_ava_geom( strip_w, &bx, &by, &bw, &bh, &ax, &ay, &aw, &ah );
	return by + bh + 6;
}

/*	Paint the box into the sheet: a dark fill with a 2px light edge.  The
	face quad is drawn over its centre at frame time, so only this margin
	shows - that margin IS the frame the strip leaves around the face.	*/
static void sw_ovl_party_draw_box( SDL_Surface *panel, int strip_w )
{
	int bx, by, bw, bh, ax, ay, aw, ah;
	SDL_Rect r;
	Uint32 fill = SDL_MapRGBA( panel->format, 32, 34, 40, 255 );
	Uint32 edge = SDL_MapRGBA( panel->format, 150, 152, 160, 255 );

	sw_ovl_party_ava_geom( strip_w, &bx, &by, &bw, &bh, &ax, &ay, &aw, &ah );
	r.x = bx; r.y = by; r.w = bw; r.h = bh;
	SDL_FillRect( panel, &r, fill );
	r.x = bx; r.y = by; r.w = bw; r.h = 2;
	SDL_FillRect( panel, &r, edge );
	r.x = bx; r.y = by + bh - 2; r.w = bw; r.h = 2;
	SDL_FillRect( panel, &r, edge );
	r.x = bx; r.y = by; r.w = 2; r.h = bh;
	SDL_FillRect( panel, &r, edge );
	r.x = bx + bw - 2; r.y = by; r.w = 2; r.h = bh;
	SDL_FillRect( panel, &r, edge );
}

/*	Paint one face cell of face1.bmp (16 columns of 48x72) for a character
	slot, scaled into the centre of its portrait box.					*/
static void sw_ovl_draw_face( int slot, float strip_x, int strip_w, float top_y )
{
	int p, col, row, bx, by, bw, bh, ax, ay, aw, ah;
	float u0, v0, u1, v1;

	if ( sw_ovl_face_tex == 0 || sw_ovl_face_w <= 0 || sw_ovl_face_h <= 0 ) return;
	sw_ovl_party_ava_geom( strip_w, &bx, &by, &bw, &bh, &ax, &ay, &aw, &ah );
	{
		int pt = sw_ovl_cdata( SW_OVL_PARTY_CD_PORTRAIT, slot );
		if ( pt < 0 ) {
			int nu = -( pt + 1 );
			if ( nu > 0 && sw_ovl_face_u_tex[ slot ] != 0 ) {
				sw_ovl_quad_uv( sw_ovl_face_u_tex[ slot ], strip_x + (float)ax,
					top_y - (float)ay, (float)aw, (float)ah, 0.0f, 0.0f, 1.0f, 1.0f );
				return;
			}
		}
	}

	p = sw_ovl_cdata( SW_OVL_PARTY_CD_SEX, slot ) * 80 + sw_ovl_cdata( SW_OVL_PARTY_CD_PORTRAIT, slot );
	col = p % SW_OVL_PARTY_CELL_COLS;
	row = p / SW_OVL_PARTY_CELL_COLS;
	u0 = (float)( col * SW_OVL_PARTY_CELL_W ) / (float)sw_ovl_face_w;
	v0 = (float)( row * SW_OVL_PARTY_CELL_H ) / (float)sw_ovl_face_h;
	u1 = (float)( col * SW_OVL_PARTY_CELL_W + SW_OVL_PARTY_CELL_W ) / (float)sw_ovl_face_w;
	v1 = (float)( row * SW_OVL_PARTY_CELL_H + SW_OVL_PARTY_CELL_H ) / (float)sw_ovl_face_h;
	sw_ovl_quad_uv( sw_ovl_face_tex, strip_x + (float)ax, top_y - (float)ay,
		(float)aw, (float)ah, u0, v0, u1, v1 );
}

static void sw_ovl_chat_lines( SDL_Surface *panel, const char *msg, int x, int y, int maxl )
{
	char rows[ 128 ][ 256 ];
	char buf[ 256 ];
	int len, nr = 0, start, rrow, row, s, maxw, n, cw, w, h;

	if ( msg == NULL ) return;
	len = (int)strlen( msg );
	if ( len <= 0 ) return;
	maxw = panel->w - x - 8;
	/*	Split the whole buffer into display rows first (each '\n' segment plus
		width wraps).  If a line would overflow the scratch buffer the oldest row
		is dropped so the newest rows are kept.  */
	s = 0;
	while ( s < len ) {
		n = 0;
		while ( s + n < len && msg[ s + n ] != '\n' ) {
			unsigned char b = (unsigned char)msg[ s + n ];
			cw = 1;
			if ( b >= 0xC2 && b <= 0xDF ) cw = 2;
			else if ( b >= 0xE0 && b <= 0xEF ) cw = 3;
			else if ( b >= 0xF0 && b <= 0xF4 ) cw = 4;
			if ( n + cw > 254 ) break;
			memcpy( buf, msg + s, n + cw ); buf[ n + cw ] = '\0';
			if ( TTF_SizeUTF8( sw_ovl_font_chat, buf, &w, &h ) != 0 || w > maxw ) break;
			n += cw;
		}
		if ( n <= 0 ) n = 1;
		if ( n > 255 ) n = 255;
		memcpy( buf, msg + s, n ); buf[ n ] = '\0';
		if ( nr >= 128 ) {
			memmove( rows[ 0 ], rows[ 1 ], ( 128 - 1 ) * 256 );
			nr = 127;
		}
		strncpy( rows[ nr ], buf, 255 ); rows[ nr ][ 255 ] = '\0'; nr++;
		s += n;
		if ( s < len && msg[ s ] == '\n' ) s++;
	}
	/*	Bottom-aligned: render only the newest maxl rows, old rows scroll off
		the top; the newest message always shows in full at the bottom.  */
	start = ( nr > maxl ) ? ( nr - maxl ) : 0;
	for ( row = start, rrow = 0; row < nr; row++, rrow++ ) {
		/*	r187v: colour like the game's own log below the HP bars - a quoted
			line is dialogue (init.hsp msg_write paints those 210,250,160),
			anything else is the plain log white.  */
		if ( strchr( rows[ row ], '"' ) != NULL ) {
			sw_ovl_blit_chat( panel, rows[ row ], 210, 250, 160, x, y + rrow * 18 );
		} else {
			sw_ovl_blit_chat( panel, rows[ row ], 255, 255, 255, x, y + rrow * 18 );
		}
	}
}

/*	r254: the left strip's "always worth a glance" block.  God names are the
	game's own Chinese short forms (god.hsp's godname table) minus the "X之"
	prefix, so the longest - 雅卡泰克特 - still fits the ~99px strip.		*/
static const char *sw_ovl_ply_god_name( int g )
{
	static const char *nm[ 9 ] = {
		"无", "玛尼", "露露薇", "伊兹帕鲁特", "艾赫卡托尔",
		"奥帕托斯", "朱亚", "库米罗妮", "雅卡泰克特"
	};
	if ( g < 0 || g > 8 ) return "无";
	return nm[ g ];
}

/*	init.hsp's _time table: GDATA_HOUR / 4 picks one of six words.		*/
static const char *sw_ovl_ply_time_name( int h )
{
	static const char *nm[ 6 ] = { "深夜", "黎明", "上午", "中午", "傍晚", "夜晚" };
	if ( h < 0 || h > 5 ) return "";
	return nm[ h ];
}

static const char *sw_ovl_ply_weather_name( int w )
{
	static const char *nm[ 5 ] = { "晴", "以太风", "雪", "雨", "大雨" };
	if ( w < 0 || w > 4 ) return "";
	return nm[ w ];
}

static const char *sw_ovl_ply_season_name( int m )
{
	if ( m >= 3 && m <= 5 ) return "春";
	if ( m >= 6 && m <= 8 ) return "夏";
	if ( m >= 9 && m <= 11 ) return "秋";
	return "冬";
}

/*	cnvweight() from text.hsp: value / 1000 with one truncated decimal.	*/
static void sw_ovl_ply_wfmt( int v, char *out, size_t n )
{
	int a = v < 0 ? -v : v;
	snprintf( out, n, "%s%d.%d", v < 0 ? "-" : "", a / 1000, ( a % 1000 ) / 100 );
}

/*	One row of that block: label in grey, value bright.  When the pair does
	not fit across the strip the value moves to the next row - the strip is
	only ~99px wide, so the condition list is the row that usually wraps.
	Returns the number of rows used.									*/
static int sw_ovl_ply_line( SDL_Surface *panel, const char *label, const char *value,
				int y, int strip_w, int hot )
{
	char full[ 200 ];
	Uint8 vr = 235, vg = 235, vb = 240;
	int avail = strip_w - 16, w = 0, lw = 0;

	if ( sw_ovl_font_small == NULL ) return 1;
	if ( avail < 32 ) avail = 32;
	snprintf( full, sizeof( full ), "%s %s", label, value );
	if ( hot ) { vr = 255; vg = 150; vb = 140; }
	if ( TTF_SizeUTF8( sw_ovl_font_small, full, &w, NULL ) != 0 ) w = 0;
	sw_ovl_blit_small_rgb( panel, label, 170, 175, 185, 8, y );
	if ( w <= avail ) {
		if ( TTF_SizeUTF8( sw_ovl_font_small, label, &lw, NULL ) != 0 ) lw = 0;
		sw_ovl_blit_small_rgb( panel, value, vr, vg, vb, 8 + lw + 6, y );
		return 1;
	}
	sw_ovl_blit_small_rgb( panel, value, vr, vg, vb, 8, y + SW_OVL_PLY_LINE_H );
	return 2;
}

static int sw_ovl_party_build( int ix, int strip_w, int sheet_h )
{
	SDL_Surface *panel;
	SDL_Rect r;
	int y, i;
	char buf[ 160 ];

	if ( strip_w < 48 ) return -1;
	if ( sw_ovl_party_sheet[ ix ] != 0 ) {
		glDeleteTextures( 1, &sw_ovl_party_sheet[ ix ] );
		sw_ovl_party_sheet[ ix ] = 0;
	}
	panel = SDL_CreateRGBSurfaceWithFormat( 0, strip_w, sheet_h, 32, SDL_PIXELFORMAT_ABGR8888 );
	if ( panel == NULL ) return -1;
	SDL_FillRect( panel, NULL, SDL_MapRGBA( panel->format, 0, 0, 0, 0 ) );

	y = SW_OVL_PARTY_NAME_Y;
	sw_ovl_blit_text( panel, sw_ovl_utf8( sw_ovl_party_name[ ix ] ), 255, 8, y );
	if ( ix == 1 ) {
		int nmw; char lv[ 48 ];
		static const char gbk_lv[] = { 0xBA, 0xC3, 0xB8, 0xD0, 0 };
		if ( TTF_SizeUTF8( sw_ovl_font, sw_ovl_utf8( sw_ovl_party_name[ ix ] ), &nmw, NULL ) == 0 ) {
			snprintf( lv, sizeof( lv ), "%s %d", gbk_lv, sw_ovl_party_rel[ ix ] );
			sw_ovl_blit_small( panel, sw_ovl_utf8( lv ), 225, 8 + nmw + 8, y + 3 );
		}
	}
	sw_ovl_party_draw_box( panel, strip_w );
	y = sw_ovl_party_rows_y( strip_w );

		if ( sw_ovl_mode == SW_OVL_MODE_PARTY || sw_ovl_mode == SW_OVL_MODE_BATTLE ) {
			static const char *lbl[3] = { "HP", "MP", "SP" };
			int cur[3] = { sw_ovl_party_hp[ix], sw_ovl_party_mp[ix], sw_ovl_party_sp[ix] };
			int mxs[3] = { sw_ovl_party_mhp[ix], sw_ovl_party_mmp[ix], sw_ovl_party_msp[ix] };
			for ( i = 0; i < 3; i++ ) {
				if ( mxs[ i ] <= 0 ) continue;
				snprintf( buf, sizeof( buf ), "%s %d/%d", lbl[ i ], cur[ i ], mxs[ i ] );
				sw_ovl_blit_text( panel, buf, 230, 8, y + i * SW_OVL_PARTY_STAT_PITCH );
				r.x = 8; r.y = y + i * SW_OVL_PARTY_STAT_PITCH + 17; r.w = strip_w - 16; r.h = 8;
				SDL_FillRect( panel, &r, SDL_MapRGBA( panel->format, 50, 50, 50, 210 ) );
			}
			y += 3 * SW_OVL_PARTY_STAT_PITCH;
		}
		/*	r254: the player's always-checked numbers, under the bars on the
			left strip only.  The battle sheet keeps its strip for the enemy
			block, so this stays out of that mode.						*/
		if ( ix == 0 && sw_ovl_mode == SW_OVL_MODE_PARTY ) {
			char v[ 200 ];
			int first = 1, c;

			y += 4;
			v[ 0 ] = '\0';
			for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
				if ( sw_ovl_ply_cond[ c ] <= 0 ) continue;
				if ( !first ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
				strncat( v, sw_ovl_bat_cond[ c ].name, sizeof( v ) - 1 - strlen( v ) );
				first = 0;
			}
			if ( first ) snprintf( v, sizeof( v ), "正常" );
			y += sw_ovl_ply_line( panel, "状态", v, y, strip_w, !first ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d%%", sw_ovl_lim( sw_ovl_ply_hunger / 100, 0, 100 ) );
			y += sw_ovl_ply_line( panel, "饱腹", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%dgp %dpp", sw_ovl_ply_gold, sw_ovl_ply_plat );
			y += sw_ovl_ply_line( panel, "金钱", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			if ( sw_ovl_ply_month > 0 ) {
				snprintf( v, sizeof( v ), "%d/%d/%d %s", sw_ovl_ply_year, sw_ovl_ply_month,
					sw_ovl_ply_day, sw_ovl_ply_time_name( sw_ovl_ply_hour / 4 ) );
				y += sw_ovl_ply_line( panel, "日期", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

				snprintf( v, sizeof( v ), "%s·%s",
					sw_ovl_ply_weather_name( sw_ovl_ply_weather ),
					sw_ovl_ply_season_name( sw_ovl_ply_month ) );
				y += sw_ovl_ply_line( panel, "天气", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
			}

			snprintf( v, sizeof( v ), "%d  名声 %d", sw_ovl_ply_karma, sw_ovl_ply_fame );
			y += sw_ovl_ply_line( panel, "善恶", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			y += sw_ovl_ply_line( panel, "信仰", sw_ovl_ply_god_name( sw_ovl_ply_god ),
				y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d  狂气 %d", sw_ovl_ply_piety, sw_ovl_ply_sanity );
			y += sw_ovl_ply_line( panel, "虔诚", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			{
				char a[ 32 ], b[ 32 ];
				sw_ovl_ply_wfmt( sw_ovl_ply_weight, a, sizeof( a ) );
				sw_ovl_ply_wfmt( sw_ovl_ply_carry, b, sizeof( b ) );
				snprintf( v, sizeof( v ), "%s/%s", a, b );
			}
			y += sw_ovl_ply_line( panel, "负重", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
		}
		if ( sw_ovl_mode == SW_OVL_MODE_DETAIL )
		for ( i = 0; i < SW_OVL_PARTY_ATTR_COUNT; i++ ) {
			int nw;
		snprintf( buf, sizeof( buf ), "%s %d", sw_ovl_attr_name( i ), sw_ovl_party_abil[ ix ][ i ] );
		sw_ovl_blit_text( panel, buf, 225, 8, y );
		if ( TTF_SizeUTF8( sw_ovl_font, buf, &nw, NULL ) == 0 )
			sw_ovl_blit_small( panel, sw_ovl_rank_name( sw_ovl_party_rank[ ix ][ i ] ), 210, 8 + nw + 8, y + 3 );
		y += 20;
	}
	if ( sw_ovl_mode == SW_OVL_MODE_DETAIL && ix == 0 ) {
		int prot, eva, spellpow;
		int pv = sw_ovl_party_fix[ 0 ][ 0 ]; if ( pv < 0 ) pv = 0;
		int dv = sw_ovl_party_fix[ 0 ][ 1 ]; if ( dv < 0 ) dv = 0;
		prot = 100 - 10000 / ( pv + 100 );
		eva  = 100 - 10000 / ( dv + 100 );
		spellpow = sw_ovl_party_fix[ 0 ][ 4 ];
		y += 6;
		{
			static const char *wl[2] = { "\xe6\xad\xa6\xe5\x99\xa8", "\xe5\xb0\x84\xe5\x87\xbb" };
			for ( i = 0; i < 2; i++ ) {
				int d1 = sw_ovl_wep[ i ][ 0 ];
				if ( d1 <= 0 ) continue;
				if ( sw_ovl_wep[ i ][ 2 ] > 0 )
					snprintf( buf, sizeof( buf ), "%s %dd%d+%d %d%%", wl[i], d1, sw_ovl_wep[i][1], sw_ovl_wep[i][2], sw_ovl_wep[i][3] );
				else
					snprintf( buf, sizeof( buf ), "%s %dd%d %d%%", wl[i], d1, sw_ovl_wep[i][1], sw_ovl_wep[i][3] );
				sw_ovl_blit_text( panel, buf, 200, 8, y );
				y += 20;
			}
			snprintf( buf, sizeof( buf ), "%s %d%%", "\xe5\x87\x8f\xe8\xbd\xbb", prot );
			sw_ovl_blit_text( panel, buf, 200, 8, y ); y += 20;
			snprintf( buf, sizeof( buf ), "%s %d%%", "\xe5\x9b\x9e\xe9\x81\xbf", eva );
			sw_ovl_blit_text( panel, buf, 200, 8, y ); y += 20;
			snprintf( buf, sizeof( buf ), "%s %d%%", "\xe9\xad\x94\xe6\xb3\x95\xe5\xa8\x81\xe5\x8a\x9b", spellpow );
			sw_ovl_blit_text( panel, buf, 200, 8, y ); y += 20;
		}
	}

	if ( sw_ovl_mode == SW_OVL_MODE_PARTY && ix == 1 ) {
		y += 8;
		sw_ovl_chat_lines( panel, sw_ovl_utf8( sw_ovl_party_chat[ 1 ] ), 8, y, SW_OVL_PET_ROWS * 2 );
	}

	/*	r247: the battle sheet hangs the current enemy off the bottom of the
	player's strip - the party rows above stay exactly as the party layer
	draws them, so both portraits are still up.  The block is clipped by
	the surface if the rows above have already used the height.			*/
	if ( ix == 0 && sw_ovl_mode == SW_OVL_MODE_BATTLE && sw_ovl_bat_name[ 0 ] != '\0' ) {
		int mhp = sw_ovl_bat_mhp, c, first = 1;
		SDL_Rect hr;
		if ( mhp < 1 ) mhp = 1;
		y += 10;
		hr.x = 8; hr.y = y - 4; hr.w = strip_w - 16; hr.h = 2;
		SDL_FillRect( panel, &hr, SDL_MapRGBA( panel->format, 150, 152, 160, 255 ) );
		sw_ovl_blit_text( panel, "— 战斗 —", 235, 8, y ); y += SW_OVL_LINE_H;
		snprintf( buf, sizeof( buf ), "%s", sw_ovl_utf8( sw_ovl_bat_name ) );
		sw_ovl_blit_text( panel, buf, 255, 8, y ); y += SW_OVL_LINE_H;
		snprintf( buf, sizeof( buf ), "HP %d/%d (%d%%)", sw_ovl_bat_hp, mhp,
			sw_ovl_lim( sw_ovl_bat_hp * 100 / mhp, 0, 100 ) );
		sw_ovl_blit_text( panel, buf, 230, 8, y ); y += SW_OVL_LINE_H;
		buf[ 0 ] = '\0';
		for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
			if ( sw_ovl_bat_condv[ c ] <= 0 ) continue;
			if ( !first ) strncat( buf, " ", sizeof( buf ) - 1 - strlen( buf ) );
			strncat( buf, sw_ovl_bat_cond[ c ].name, sizeof( buf ) - 1 - strlen( buf ) );
			first = 0;
		}
		if ( first ) snprintf( buf, sizeof( buf ), "状态正常" );
		sw_ovl_blit_small( panel, buf, 225, 8, y + 2 );
	}

	sw_ovl_party_sheet[ ix ] = sw_ovl_upload_rgba( panel, &sw_ovl_party_sheet_w[ ix ], &sw_ovl_party_sheet_h[ ix ] );
	SDL_FreeSurface( panel );
	return sw_ovl_party_sheet[ ix ] ? 0 : -1;
}

static int sw_ovl_party_refresh( int *slot )
{
	int dirty = 0, k, i, c;

	slot[0] = 0;
	slot[1] = -1;
	{ int e0 = sw_ovl_cdata( SW_OVL_PARTY_CD_EXIST, 0 );
	  if ( e0 != 1 ) return -1; }
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
			for ( i = 0; i < SW_OVL_PARTY_ATTR_COUNT; i++ ) {
				sw_ovl_party_abil[k][i] = 0; sw_ovl_party_rank[k][i] = 0;
			}
			sw_ovl_face_u_load( k, 0 );
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
			int v = sw_ovl_sdata( SW_OVL_PARTY_ATTR_FIRST + i, s );
			int r = sw_ovl_sdata( SW_OVL_PARTY_SKILL_OFF + SW_OVL_PARTY_ATTR_FIRST + i, s ) % 1000;
			if ( v != sw_ovl_party_abil[k][i] || r != sw_ovl_party_rank[k][i] ) dirty = 1;
			sw_ovl_party_abil[k][i] = v;
			sw_ovl_party_rank[k][i] = r;
		}
		{
			int pt = sw_ovl_cdata( SW_OVL_PARTY_CD_PORTRAIT, s );
			int nu = ( pt < 0 ) ? -( pt + 1 ) : 0;
			if ( nu != sw_ovl_face_u_id[k] ) {
				dirty = 1;
				sw_ovl_face_u_load( k, nu );
			}
		}
		if ( k == 0 ) {
			int fv[5], mag;
				int w[4], r[4], wi;
				sw_ovl_calc_wep( 0, w );
				sw_ovl_calc_wep( 1, r );
				for ( wi = 0; wi < 4; wi++ ) {
					if ( w[wi] != sw_ovl_wep[0][wi] ) dirty = 1;
					if ( r[wi] != sw_ovl_wep[1][wi] ) dirty = 1;
					sw_ovl_wep[0][wi] = w[wi];
					sw_ovl_wep[1][wi] = r[wi];
				}
			fv[0] = sw_ovl_cdata( 70, s );
			fv[1] = sw_ovl_cdata( 71, s );
			fv[2] = sw_ovl_cdata( 72, s );
			fv[3] = sw_ovl_cdata( 11, s );
			mag = sw_ovl_sdata( 16, s );
			if ( mag < 0 ) mag = 0; if ( mag > 1500 ) mag = 1500;
			fv[4] = sw_ovl_cdata( 98, s ) * 2 / 3;
			if ( fv[4] < 0 ) fv[4] = 0; if ( fv[4] > 1000 ) fv[4] = 1000;
			fv[4] = fv[4] / 10 + mag / 10 + 100;
			{ static int once = 0;
			  if ( !once ) { once = 1;
				for ( i = 0; i < SW_OVL_PARTY_ATTR_COUNT; i++ )
					printf( "ovlrank[%d] cur=%d raw=%d r=%d %s\n", i,
						sw_ovl_party_abil[0][i],
						sw_ovl_sdata( SW_OVL_PARTY_SKILL_OFF + SW_OVL_PARTY_ATTR_FIRST + i, s ),
						sw_ovl_party_rank[0][i],
						sw_ovl_rank_name( sw_ovl_party_rank[0][i] ) );
				fflush( stdout );
			  } }
			for ( i = 0; i < 5; i++ ) {
				if ( fv[i] != sw_ovl_party_fix[k][i] ) dirty = 1;
				sw_ovl_party_fix[k][i] = fv[i];
			}

			/*	r254: the left strip's numbers, all off the player's own row. */
			for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
				int cv = sw_ovl_cdata( sw_ovl_bat_cond[ c ].field, 0 );
				if ( cv != sw_ovl_ply_cond[ c ] ) { dirty = 1; sw_ovl_ply_cond[ c ] = cv; }
			}
#define SW_OVL_PLY_SYNC( dst, src ) \
			do { int nv_ = ( src ); if ( ( dst ) != nv_ ) { dirty = 1; ( dst ) = nv_; } } while ( 0 )
			SW_OVL_PLY_SYNC( sw_ovl_ply_hunger, sw_ovl_cdata( SW_OVL_PLY_CD_HUNGER, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_gold,   sw_ovl_cdata( SW_OVL_PLY_CD_GOLD, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_plat,   sw_ovl_cdata( SW_OVL_PLY_CD_PLATINUM, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_fame,   sw_ovl_cdata( SW_OVL_PLY_CD_FAME, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_weight, sw_ovl_cdata( SW_OVL_PLY_CD_WEIGHT, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_carry,  sw_ovl_cdata( SW_OVL_PLY_CD_CARRY, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_karma,  sw_ovl_cdata( SW_OVL_PLY_CD_KARMA, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_god,    sw_ovl_cdata( SW_OVL_PLY_CD_GOD, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_piety,  sw_ovl_cdata( SW_OVL_PLY_CD_PIETY, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_sanity, sw_ovl_cdata( SW_OVL_PLY_CD_SANITY, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_year,   sw_ovl_gdata( SW_OVL_GD_YEAR ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_month,  sw_ovl_gdata( SW_OVL_GD_MONTH ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_day,    sw_ovl_gdata( SW_OVL_GD_DAY ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_hour,   sw_ovl_gdata( SW_OVL_GD_HOUR ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_weather, sw_ovl_gdata( SW_OVL_GD_WEATHER ) );
#undef SW_OVL_PLY_SYNC
		}

			/*	Affection is the impression plus the master-servant axis: the
				game's own thresholds are 200 / 250 / 300 / 350 (action.hsp).
				The HUD read cdata(9), which is the hostile-relation band
				(-3..10), so the number beside the name was never the real
				one.														*/
			{ int rr = sw_ovl_cdata( 17, s ) + sw_ovl_cdata( 169, s );
			  if ( rr != sw_ovl_party_rel[k] ) { dirty = 1; sw_ovl_party_rel[k] = rr; } }
		/*	r203 probe: does the name-table route agree with the HUD's
			dimension route, and is chatc actually pointing at a follower?
			Prints once per run.  Remove with the rest of r203's probe. */
		{
			static int r203_once = 0;
			if ( !r203_once ) {
				int sp;
				r203_once = 1;
				sp = sw_ovl_speaker();
				printf( "r203diag: cdata_by_name=%s chatc_var=%s speaker=%d "
					"name=[%s] imp=%d master=%d affection=%d\n",
					code_getdebug_seekvar( "cdata" ) >= 0 ? "yes" : "no",
					sw_ovl_pv_chatc ? "found" : "NULL", sp,
					sp >= 0 ? sw_ovl_cdatan( 0, sp ) : "-",
					sp >= 0 ? sw_ovl_cdata( 17, sp ) : -1,
					sp >= 0 ? sw_ovl_cdata( 169, sp ) : -1,
					sp >= 0 ? sw_ovl_cdata( 17, sp ) + sw_ovl_cdata( 169, sp ) : -1 );
				fflush( stdout );
			}
		}

		if ( k == 1 ) {
			const char *m = sw_ovl_msg_peek( sw_ovl_party_name[1], sw_ovl_speaker(), sw_ovl_party_slot[1] );
			char mb[ 1200 ];
			snprintf( mb, sizeof( mb ), "%s", m );
			if ( strcmp( mb, sw_ovl_party_chat[1] ) != 0 ) {
				dirty = 1;
				strncpy( sw_ovl_party_chat[1], mb, sizeof( sw_ovl_party_chat[1] ) - 1 );
				sw_ovl_party_chat[1][ sizeof( sw_ovl_party_chat[1] ) - 1 ] = '\0';
			}
		}
	}

	/*	r247: while the battle sheet is up, track the enemy it names (name,
	HP and the condition counters) so the sheet is rebuilt when they move. */
	if ( sw_ovl_mode == SW_OVL_MODE_BATTLE && sw_ovl_battle_slot >= 0 ) {
		int s = sw_ovl_battle_slot, c;
		char nb[ 64 ];
		snprintf( nb, sizeof( nb ), "%s", sw_ovl_cdatan( 0, s ) );
		if ( strcmp( nb, sw_ovl_bat_name ) != 0 ) {
			dirty = 1;
			strncpy( sw_ovl_bat_name, nb, sizeof( sw_ovl_bat_name ) - 1 );
			sw_ovl_bat_name[ sizeof( sw_ovl_bat_name ) - 1 ] = '\0';
		}
		if ( sw_ovl_bat_hp != sw_ovl_cdata( SW_OVL_PARTY_CD_HP, s ) ) dirty = 1;
		if ( sw_ovl_bat_mhp != sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_HP, s ) ) dirty = 1;
		sw_ovl_bat_hp = sw_ovl_cdata( SW_OVL_PARTY_CD_HP, s );
		sw_ovl_bat_mhp = sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_HP, s );
		for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
			int v = sw_ovl_cdata( sw_ovl_bat_cond[ c ].field, s );
			if ( v != sw_ovl_bat_condv[ c ] ) { dirty = 1; sw_ovl_bat_condv[ c ] = v; }
		}
	}
	return dirty;
}

void switch_overlay_draw_party( int win_w, int win_h, int origin_x, int game_w, int right_x, int right_w )
{
	int dirty, slot[2], k, i, tex2d_was;
	int sheet_h;
	float ly;

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
	/*	r247: the battle block is part of the sheets, so a mode change has
		to rebuild them even when the tracked party values did not move.	*/
	if ( sw_ovl_party_built_mode != sw_ovl_mode ) dirty = 1;
	if ( dirty || sw_ovl_party_sheet[0] == 0 || sw_ovl_party_sheet[1] == 0 ) {
		if ( sw_ovl_party_build( 0, origin_x, sheet_h ) != 0 ) return;
		if ( sw_ovl_party_build( 1, right_w, sheet_h ) != 0 ) return;
		sw_ovl_party_built_mode = sw_ovl_mode;
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

	ly = 0.0f;	/* sheets start at the top edge so the name row is not clipped */
	if ( sw_ovl_party_sheet[0] != 0 ) {
		sw_ovl_quad( sw_ovl_party_sheet[0], 0.0f, ly, (float)sw_ovl_party_sheet_w[0], (float)sheet_h );
		sw_ovl_draw_face( 0, 0.0f, origin_x, ly );
		/*	r187v: the player's grey bar slots sit in the same rows as the
			pet's on the right strip (sw_ovl_party_build draws 3 *
			SW_OVL_PARTY_STAT_PITCH); paint the colour strips over them here at
			present time, mirroring the pet's block below.  */
		if ( sw_ovl_mode == SW_OVL_MODE_PARTY || sw_ovl_mode == SW_OVL_MODE_BATTLE ) {
			int rows_y = sw_ovl_party_rows_y( origin_x );
			int cur[3] = { sw_ovl_party_hp[0], sw_ovl_party_mp[0], sw_ovl_party_sp[0] };
			int mxs[3] = { sw_ovl_party_mhp[0], sw_ovl_party_mmp[0], sw_ovl_party_msp[0] };
			for ( i = 0; i < 3; i++ ) {
				int fw;
				if ( mxs[ i ] <= 0 || cur[ i ] <= 0 ) continue;
				if ( cur[ i ] > mxs[ i ] ) cur[ i ] = mxs[ i ];
				fw = (int)( ( origin_x - 16 ) * cur[ i ] / mxs[ i ] );
				if ( fw < 1 ) fw = 1;
				sw_ovl_quad_uv( sw_ovl_bar_tex[ i + 1 ], 8.0f,
					ly - (float)( rows_y + i * SW_OVL_PARTY_STAT_PITCH + 17 ),
					(float)fw, 8.0f, 0.0f, 0.0f, 1.0f, 1.0f );
			}
		}
	}
	if ( sw_ovl_party_sheet[1] != 0 && slot[1] >= 0 ) {
			int rows_y = sw_ovl_party_rows_y( right_w );
		sw_ovl_quad( sw_ovl_party_sheet[1], (float)right_x, ly, (float)sw_ovl_party_sheet_w[1], (float)sheet_h );
		sw_ovl_draw_face( 1, (float)right_x, right_w, ly );
		if ( sw_ovl_mode == SW_OVL_MODE_PARTY || sw_ovl_mode == SW_OVL_MODE_BATTLE ) {
			int cur[3] = { sw_ovl_party_hp[1], sw_ovl_party_mp[1], sw_ovl_party_sp[1] };
			int mxs[3] = { sw_ovl_party_mhp[1], sw_ovl_party_mmp[1], sw_ovl_party_msp[1] };
			for ( i = 0; i < 3; i++ ) {
				int fw;
				if ( mxs[ i ] <= 0 || cur[ i ] <= 0 ) continue;
				if ( cur[ i ] > mxs[ i ] ) cur[ i ] = mxs[ i ];
				fw = (int)( ( right_w - 16 ) * cur[ i ] / mxs[ i ] );
				if ( fw < 1 ) fw = 1;
				sw_ovl_quad_uv( sw_ovl_bar_tex[ i + 1 ], (float)right_x + 8.0f,
					ly - (float)( rows_y + i * SW_OVL_PARTY_STAT_PITCH + 17 ),
					(float)fw, 8.0f, 0.0f, 0.0f, 1.0f, 1.0f );
			}
		}
	}

	glBindTexture( GL_TEXTURE_2D, 0 );
	glDisable( GL_BLEND );
	if ( !tex2d_was ) glDisable( GL_TEXTURE_2D );
}
