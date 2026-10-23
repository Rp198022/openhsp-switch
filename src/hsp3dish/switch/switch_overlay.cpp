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
#include "switch_input.h"
#include "ai_chat.h"

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
	{ "ZL",			"呼出对话" },
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
static int		sw_ovl_tex_win_h = -1;	/* window height they were built for	*/

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

/*	r285: gdata(GDATA_AREA) is a number; the game turns it into a name in
	mapname().  The port cannot call that HSP function, so the names are
	copied from the 2.33 Chinese build's own table (start.ax, GBK) - the
	same words the player sees when the area message is printed.			*/
static const struct { int id; const char *name; } sw_ovl_area_tbl[] = {
	{ 1, "世界地图" }, { 2, "野外" }, { 3, "莱希墨苏" }, { 4, "北提里斯" },
	{ 5, "韦尔尼斯" }, { 6, "斗技场" }, { 7, "自家" }, { 8, "地城" },
	{ 9, "实验场" }, { 10, "鲁米艾斯特墓地" }, { 11, "卡普尔港" }, { 12, "约恩村" },
	{ 13, "任务" }, { 14, "特尔斐" }, { 15, "帕罗米亚" }, { 16, "灼热之塔" },
	{ 17, "死者的洞窟" }, { 18, "古城" }, { 19, "龙窟" }, { 20, "众神休战之地" },
	{ 21, "电子巨蛋" }, { 22, "混沌之城·兽" }, { 23, "混沌之城·机甲" },
	{ 24, "混沌之城·畸形" }, { 25, "莱娜" }, { 26, "山道" }, { 27, "小狗之洞窟" },
	{ 28, "小鬼之洞窟" }, { 29, "妹之馆" }, { 30, "避难所" }, { 31, "牧场" },
	{ 32, "帕罗米亚大使馆" }, { 33, "诺耶尔" }, { 34, "米拉鲁·盖罗克工房" },
	{ 35, "家居巨蛋" }, { 36, "鲁米艾斯特" }, { 37, "金字塔" }, { 38, "牛头人之巢穴" },
	{ 39, "地牢" }, { 40, "宠物斗技场" }, { 41, "监狱" }, { 42, "巢穴" },
	{ 43, "提里斯关所北侧" }, { 44, "南提里斯" }, { 45, "提里斯关所南侧" },
	{ 46, "烟与烟斗之亭" }, { 47, "海" }, { 48, "提里斯关所南侧" },
	{ 49, "莱米伊德遗迹" }, { 50, "埃雷鲁" }, { 51, "鲁奥萨" }, { 52, "梅尔卡恩" },
	{ 53, "克莱阿尔玛" }, { 54, "瓦卢姆" }, { 55, "梅洛卡斯" }, { 56, "阿克·贝尔库" },
	{ 57, "路多斯" }, { 58, "异形的神殿" }, { 59, "黄泉之谷" }, { 60, "机械要塞" },
	{ 61, "女仆馆" }, { 62, "混沌之城·破坏" }, { 63, "混沌之城·机皇" },
	{ 64, "混沌之城·地狱" }, { 65, "古之庭院" }, { 66, "监狱" }, { 67, "道场" },
	{ 68, "风之山道" }, { 69, "犬神之森" }, { 70, "失落伊尔瓦" },
	{ 71, "乌鲁姆·莱纵" }, { 72, "伊尔瓦神立图书馆" }, { 73, "艾沃达纳魔导船" },
	{ 74, "阿摩利的暗牢" }, { 75, "阳牢之塔" }, { 76, "混沌巨城" },
	{ 77, "路斯特尔仙窟" }, { 78, "塞尔特火山" }, { 79, "中央神殿" },
	{ 80, "沉睡的神舰·船坞" }, { 81, "欧鲁德朗" }, { 82, "隐蔽的工房" },
	{ 83, "星渡之收获船" }, { 84, "魔女之家" }, { 85, "丧失狂宫" },
	{ 86, "众神的休战地" }, { 87, "冒险课堂" }, { 88, "梅尔卡雨林" },
	{ 89, "重力谷" }, { 90, "山麓遗迹" }, { 91, "伊姆瓦尔" }, { 92, "扎伊尔" },
	{ 93, "恶魔岬" }, { 95, "拉斯基利斯山谷·南" }, { 96, "拉斯基利斯山谷·西" },
	{ 100, "废弃的怪物牧场" }, { 101, "博物馆" }, { 102, "商店" },
	{ 103, "田地" }, { 104, "仓库" }, { 105, "野营地" },
	{ 0, NULL }
};

static const char *sw_ovl_area_name( int id )
{
	int i;

	for ( i = 0; sw_ovl_area_tbl[ i ].name != NULL; i++ )
		if ( sw_ovl_area_tbl[ i ].id == id ) return sw_ovl_area_tbl[ i ].name;
	return "";
}

/*	Areas that are a single place rather than a depth: a town does not get
	"地下第 1 层" appended, a dungeon does.								*/
static int sw_ovl_area_is_dungeon( int id )
{
	if ( id == 3 || id == 8 || id == 16 || id == 17 || id == 18 || id == 19 ||
			id == 26 || id == 27 || id == 28 || id == 29 || id == 37 || id == 38 ||
			id == 39 || id == 41 || id == 42 ) return 1;
	if ( id >= 58 && id <= 70 ) return 1;
	if ( id >= 74 && id <= 80 ) return 1;
	if ( id >= 89 && id <= 90 ) return 1;
	return 0;
}

/*	r277: the battle sheet wants the numbers a player checks before choosing
	a move - the target's level, speed, PV/DV and whatever is riding on it.
	Indices are from Elona+'s defines (2.32 _reference/defines_2.32.hsp):
	CDATA_SPEED 11, CDATA_TARGET 29, CDATA_LEVEL 38, CDATA_PV 70,
	CDATA_DV 71, buffs start at 280 with three cells each (id, power,
	remaining turns).  Every fighter is read the same way.				*/
#define SW_OVL_CD_SPEED			11
#define SW_OVL_CD_TARGET		29
#define SW_OVL_CD_LEVEL			38
#define SW_OVL_CD_PV			70
#define SW_OVL_CD_DV			71
#define SW_OVL_CD_BUFF_BASE		280
#define SW_OVL_CD_BUFF_STRIDE	3
#define SW_OVL_CD_BUFF_N		72
#define SW_OVL_CD_BUFF_TURNS	2
#define SW_OVL_FOE_BUFF_SHOWN	3

typedef struct {
	int		slot;					/* chara index, -1 = nothing		*/
	int		level, speed, pv, dv, hp, mhp;
	int		cond[ SW_OVL_BAT_COND_N ];
	int		nbuff, ndebuff;
	int		buffid[ SW_OVL_FOE_BUFF_SHOWN ];
	int		debuffid[ SW_OVL_FOE_BUFF_SHOWN ];
	char	name[ 64 ];				/* still GBK						*/
} SW_OVL_FOE;

static SW_OVL_FOE	sw_ovl_foe[ 2 ];	/* 0 = player's target, 1 = pet's */
static int			sw_ovl_foe_sig = -1;
static int			sw_ovl_pet_speed = -1, sw_ovl_pet_pv = -1, sw_ovl_pet_dv = -1;
static int			sw_ovl_pet_nbuff = 0, sw_ovl_pet_ndebuff = 0;
static int			sw_ovl_pet_buffid[ SW_OVL_FOE_BUFF_SHOWN ];
static int			sw_ovl_pet_debuffid[ SW_OVL_FOE_BUFF_SHOWN ];
static int			sw_ovl_ply_speed = -1, sw_ovl_ply_pv = -1, sw_ovl_ply_dv = -1;
static int			sw_ovl_ply_nbuff = 0, sw_ovl_ply_ndebuff = 0;
static int			sw_ovl_ply_buffid[ SW_OVL_FOE_BUFF_SHOWN ];
static int			sw_ovl_ply_debuffid[ SW_OVL_FOE_BUFF_SHOWN ];

/*	r254: the player's own always-checked numbers, drawn under the bars on
	the party sheet's left strip.  All of them are plain cdata/gdata fields;
	the weight pair uses the game's cnvweight() scale (value / 1000, one
	decimal).  There is no fatigue field in Elona+ - HUNGER and the sleep
	condition are the only state the script keeps - so fame takes that slot. */
#define SW_OVL_PLY_LINE_H		19
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
#define SW_OVL_PLY_CD_EQUIPW	64	/* equipped gear weight			*/
#define SW_OVL_PLY_CD_BARRIER	78	/* shield points, battle sheet only	*/
#define SW_OVL_GD_YEAR			10
#define SW_OVL_GD_MONTH			11
#define SW_OVL_GD_DAY			12
#define SW_OVL_GD_HOUR			13
#define SW_OVL_GD_WEATHER		17
#define SW_OVL_GD_CARGO			80	/* cart / luggage weight		*/
#define SW_OVL_GD_CARGO_LIMIT	82
#define SW_OVL_GD_LEVEL			22	/* current dungeon depth		*/
#define SW_OVL_GD_AREA			20	/* AREA_*, see sw_ovl_area_tbl	*/
/*	Ether disease: global trait() is dim trait,500 and the ether traits are
	201..228 (defines/2.32.hsp).  A negative value means the symptom is
	present; its magnitude is the stage, so the count and the worst stage
	are all the panel has to show.										*/
#define SW_OVL_ETHER_FIRST		201
#define SW_OVL_ETHER_LAST		228
static int		sw_ovl_ply_cond[2][ SW_OVL_BAT_COND_N ];
static int		sw_ovl_ply_hunger[2] = { -1, -1 };
static int		sw_ovl_ply_gold[2] = { -1, -1 }, sw_ovl_ply_plat[2] = { -1, -1 };
static int		sw_ovl_ply_weight[2] = { -1, -1 }, sw_ovl_ply_carry[2] = { -1, -1 };
static int		sw_ovl_ply_equipw[2] = { -1, -1 }, sw_ovl_ply_barrier[2] = { -1, -1 };
static int		sw_ovl_ply_cargo = -1, sw_ovl_ply_cargol = -1;
static int		sw_ovl_ply_level = -1;
static int		sw_ovl_ply_area = -1;
static int		sw_ovl_ply_ether = -1, sw_ovl_ply_ether_worst = 0;
static int		sw_ovl_ply_year = -1, sw_ovl_ply_month = -1, sw_ovl_ply_day = -1;
static int		sw_ovl_ply_hour = -1, sw_ovl_ply_weather = -1;
static int		sw_ovl_ply_karma = 0, sw_ovl_ply_god = -1, sw_ovl_ply_piety = -1;
static int		sw_ovl_ply_sanity = -1, sw_ovl_ply_fame = -1;

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
/*	r277: the game's own buff table (sdim buffname,20,72) and its type
	column (dim bdataref,7,72; field 0 is BUFF_TYPE_BUFF/HEX/FOOD).  Reading
	them keeps the battle sheet's words in Chinese without a hand-kept
	list, and tells a buff from a debuff without guessing.				*/
static PVal		*sw_ovl_pv_buffname = NULL;
static PVal		*sw_ovl_pv_bdataref = NULL;

/*	r203: the most recent custom-talk speaker.  chara_func.hsp:207 sets
	`chatc = customtalk_charid` before dispatching any custom line and
	nothing clears it, so it stays valid for every trigger.  Reaching it
	needs the .ax variable-name table: present on the 2.33 build (hspcmp
	emits the debug segment, max_dinfo=22568) but empty on the shipped
	2.30 .ax (max_dinfo=2), where this stays NULL and the caller must
	fall back to matching the speaker name written into the talk file. */
static PVal		*sw_ovl_pv_chatc = NULL;
/*	r260: trait (dim trait,500) carries the ether-disease symptoms.  It is
	resolved by name only - spell/spact are also 500-long, so the dimension
	scan cannot tell them apart, and a wrong array would fake a disease.	*/
static PVal		*sw_ovl_pv_trait = NULL;
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
static int		sw_ovl_party_imp[2] = { 0, 0 };	/* cdata 17: impression	*/
static int		sw_ovl_party_ms[2] = { 0, 0 };	/* cdata 169: master-servant */
static int		sw_ovl_wep[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
static int		sw_ovl_face_u_id[2] = { 0, 0 };
/*	Pet chat: the latest pet-related log lines joined by '\n', newest
	last.  sw_ovl_msg_peek() collects up to SW_OVL_PET_ROWS lines from the
	game's log ring; sw_ovl_chat_lines() renders one line each, older
	lines scrolling up as new ones arrive like the game's own log.		*/
#define SW_OVL_PET_ROWS	6
static char		sw_ovl_party_chat[2][ 1200 ] = { "", "" };

/*	r261: the companion's model-written line, drawn under the game's own
	chatter.  The port only tracks what changed; the sentence itself comes
	from ai_chat.cpp, off a worker thread.
	r269: the game's chatter row is gone - the whole lower half of the pet's
	strip is the answer box, so a line may now run to four or five rows. */
#define SW_OVL_AI_ROWS	15
static char		sw_ovl_ai_shown[ 512 ] = "";
static int		sw_ovl_ai_seq_seen = 0;
static int		sw_ovl_ai_last_battle = 0;
static int		sw_ovl_ai_last_level = -1;
static int		sw_ovl_ai_was_hurt = 0;
static int		sw_ovl_ai_was_hungry = 0;

/*	r262: what the follower is told about the game.  The panel already reads
	the interpreter's arrays every frame, so the same reads feed a watcher
	that turns a change into a plain sentence for the model.  A log line is
	the most informative of them - Elona writes "你击败了…" / "你捡到了…"
	there - and the numbers cover what the log does not say out loud.		*/
static char		sw_ovl_ai_w_log[ 1200 ] = "";
static unsigned int	sw_ovl_ai_w_log_tick = 0;
static int		sw_ovl_ai_w_hp_pct = -1;
static int		sw_ovl_ai_w_cargo = -1;
static int		sw_ovl_ai_w_ether = -1;
static int		sw_ovl_ai_w_imp = -1;		/* follower's affection		*/
static int		sw_ovl_ai_w_ms = -1;		/* follower's master-servant	*/
static int		sw_ovl_ai_w_weather = -1;
static int		sw_ovl_ai_w_part = -1;		/* hour / 4					*/
static int		sw_ovl_ai_w_hunger = -1;	/* satiety, 0-100			*/
static int		sw_ovl_ai_w_dying = 0;		/* already reported < 25%	*/
static int		sw_ovl_ai_w_battle = 0;
static int		sw_ovl_ai_w_cond = 0;
static int		sw_ovl_ai_w_cond_ready = 0;
static unsigned int	sw_ovl_ai_w_tick = 0;
/*	r292: the events that are worth a line the moment they happen - a
	welcome after a load, a death, a follower called back, and the game's
	own notes about the follower (skill practice, eating, drinking).		*/
static char		sw_ovl_ai_w_greet_name[ 64 ] = "";	/* who we already greeted */
static int		sw_ovl_ai_w_pet_seen = 0;	/* follower is in the party	*/
static int		sw_ovl_ai_w_ply_dead = 0;
static int		sw_ovl_ai_w_pet_dead = 0;
static int		sw_ovl_ai_w_pet_hp = -1;	/* pet hp percent, -1 = unread */
static char		sw_ovl_ai_w_msg[ 300 ] = "";	/* newest log row, UTF-8	*/

/*	r269: unattended consoles.  A fingerprint of everything the panel tracks
	changes on every turn the player takes and stays put while the game is
	left standing, which is exactly the signal needed to stop the idle timer
	from buying scenery remarks all night.									*/
#define SW_OVL_AI_IDLE_MS	75000
static unsigned int	sw_ovl_ai_act_sig = 0;
static unsigned int	sw_ovl_ai_act_tick = 0;
static int		sw_ovl_ai_idle = 0;

void switch_overlay_ctx_reset( void )
{
	/*	The context that issued these names is gone; dropping the numbers
		without glDeleteTextures() is deliberate (see the header).		*/
	sw_ovl_tex[0] = 0;
	sw_ovl_tex[1] = 0;
	sw_ovl_origin_x = -1;
	sw_ovl_tex_win_h = -1;
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
	sw_ovl_pv_trait = NULL;
	sw_ovl_pv_inv = NULL;
	sw_ovl_pv_buffname = NULL;
	sw_ovl_pv_bdataref = NULL;
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

/*	r257: the strips are parchment now, so the text is ink - near-black for
	the values, a warm grey-brown for labels and group titles.  The existing
	call sites still pass a brightness (255/230/205) and this maps it.		*/
static void sw_ovl_ink( Uint8 v, Uint8 *r, Uint8 *g, Uint8 *b )
{
	if ( v >= 245 )      { *r = 32;  *g = 26;  *b = 20; }
	else if ( v >= 215 ) { *r = 64;  *g = 53;  *b = 40; }
	else                 { *r = 106; *g = 92;  *b = 70; }
}

/*	Same as sw_ovl_blit_text but with the smaller rating font.			*/
static void sw_ovl_blit_small( SDL_Surface *dst, const char *text, Uint8 v, int x, int y )
{
	SDL_Color col;
	SDL_Surface *s, *c;
	SDL_Rect at;

	if ( sw_ovl_font_small == NULL ) return;
	sw_ovl_ink( v, &col.r, &col.g, &col.b );
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

/*	Draw one line with a chosen face - the key strips fall back to a smaller
	font when the two columns cannot both fit the strip at 16px.			*/
static void sw_ovl_blit_f( SDL_Surface *dst, TTF_Font *f, const char *text, Uint8 v, int x, int y )
{
	SDL_Color col;
	SDL_Surface *s, *c;
	SDL_Rect at;

	if ( f == NULL ) return;
	sw_ovl_ink( v, &col.r, &col.g, &col.b );
	col.a = 255;
	s = TTF_RenderUTF8_Blended( f, text, col );
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
	sw_ovl_blit_f( dst, sw_ovl_font, text, v, x, y );
}

/*	The same face in a chosen colour - the stat block tints its labels grey
	and its values bright white (or red when something is wrong).			*/
static void sw_ovl_blit_text_rgb( SDL_Surface *dst, const char *text,
					Uint8 r, Uint8 g, Uint8 b, int x, int y )
{
	SDL_Color col;
	SDL_Surface *s, *c;
	SDL_Rect at;

	if ( sw_ovl_font == NULL ) return;
	col.r = r; col.g = g; col.b = b;
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

/*	r259: the strips wear Elona's own background texture, graphic/void.bmp
	(1200x900, clean marble-paper with no drawn border) - a slice of it with
	the panel's aspect fills a strip without any decoration running over the
	text.  The framed chat sheet and paper.bmp remain as fallbacks.  Loaded
	once, never freed (see the header).										*/
#define SW_OVL_PAPER_WHOLE	0	/* any large texture: crop, do not split	*/
#define SW_OVL_PAPER_SPLIT	1	/* two halves are the two panels			*/
#define SW_OVL_PAPER_TORN	2	/* paper.bmp: left sheet, inside its edge	*/
static SDL_Surface	*sw_ovl_paper = NULL;
static int			sw_ovl_paper_tried = 0;
static int			sw_ovl_paper_kind = SW_OVL_PAPER_WHOLE;

static SDL_Surface *sw_ovl_paper_get( void )
{
	static const struct { const char *path; int kind; } cand[] = {
		{ "sdmc:/switch/openhsp/graphic/void.bmp",			SW_OVL_PAPER_WHOLE },
		{ "sdmc:/switch/openhsp/user/graphic/void.bmp",		SW_OVL_PAPER_WHOLE },
		{ "sdmc:/switch/openhsp/graphic/ie_chat2.bmp",		SW_OVL_PAPER_SPLIT },
		{ "sdmc:/switch/openhsp/user/graphic/ie_chat2.bmp",	SW_OVL_PAPER_SPLIT },
		{ "sdmc:/switch/openhsp/graphic/paper.bmp",			SW_OVL_PAPER_TORN },
		{ "sdmc:/switch/openhsp/user/graphic/paper.bmp",		SW_OVL_PAPER_TORN },
	};
	int i;

	if ( sw_ovl_paper != NULL || sw_ovl_paper_tried ) return sw_ovl_paper;
	sw_ovl_paper_tried = 1;
	for ( i = 0; i < (int)( sizeof( cand ) / sizeof( cand[0] ) ); i++ ) {
		SDL_Surface *b = SDL_LoadBMP( cand[ i ].path );
		if ( b == NULL ) continue;
		sw_ovl_paper = SDL_ConvertSurfaceFormat( b, SDL_PIXELFORMAT_ABGR8888, 0 );
		SDL_FreeSurface( b );
		if ( sw_ovl_paper != NULL ) {
			sw_ovl_paper_kind = cand[ i ].kind;
			printf( "hsp3switch: overlay paper %s (%dx%d) kind=%d\n", cand[ i ].path,
				sw_ovl_paper->w, sw_ovl_paper->h, sw_ovl_paper_kind );
			fflush( stdout );
			return sw_ovl_paper;
		}
	}
	printf( "hsp3switch: overlay: no paper image, flat parchment\n" );
	fflush( stdout );
	return NULL;
}

/*	Paint one strip's paper.  side 0 = left panel, 1 = right panel.

	Whole texture (void.bmp): a slice with the panel's aspect is taken from
	the middle and scaled once.  The source is 1200x900, so a 135x594 strip
	uses a 204x900 slice - the grain comes out finer, and no border piece can
	land on the text.

	Split sheet (ie_chat2.bmp): the bitmap's two halves are the panels, border
	included, so each half is scaled to fill the strip and shifted until its
	OUTER edge meets the strip's outer edge - the inner overhang is cropped.

	paper.bmp: two sheets side by side (800x300), the left clean and the right
	covered in ink, so only the left half's inner area is usable.			*/
static void sw_ovl_paper_fill( SDL_Surface *panel, int side )
{
	SDL_Surface *paper = sw_ovl_paper_get();
	SDL_Rect src, dst;
	int w, h, sw, sh, half, x0, y0, inner_w, inner_h, slice_w;

	if ( paper == NULL || paper->w < 64 || paper->h < 64 || panel->h < 16 ) {
		SDL_FillRect( panel, NULL, SDL_MapRGBA( panel->format, 214, 197, 168, 255 ) );
		return;
	}
	if ( sw_ovl_paper_kind == SW_OVL_PAPER_WHOLE ) {
		slice_w = paper->h * panel->w / panel->h;
		if ( slice_w > paper->w ) slice_w = paper->w;
		if ( slice_w < 8 ) slice_w = 8;
		src.x = ( paper->w - slice_w ) / 2;
		src.y = 0; src.w = slice_w; src.h = paper->h;
		dst.x = 0; dst.y = 0; dst.w = panel->w; dst.h = panel->h;
		SDL_BlitScaled( paper, &src, panel, &dst );
		return;
	}
	if ( sw_ovl_paper_kind == SW_OVL_PAPER_SPLIT ) {
		w = paper->w / 2;
		h = paper->h;
		sw = w * panel->h / h;			/* fill the height first		*/
		sh = panel->h;
		if ( sw < panel->w ) {			/* too narrow: fill the width	*/
			sw = panel->w;
			sh = h * panel->w / w;
		}
		src.x = ( side == 0 ) ? 0 : w;
		src.y = 0; src.w = w; src.h = h;
		dst.y = ( panel->h - sh ) / 2;
		dst.w = sw; dst.h = sh;
		dst.x = ( side == 0 ) ? 0 : panel->w - sw;
		SDL_BlitScaled( paper, &src, panel, &dst );
		return;
	}

	half = paper->w / 2;					/* left sheet only			*/
	x0 = half / 10;  y0 = paper->h / 12;	/* inside the torn edge		*/
	inner_w = half - x0 * 2;
	inner_h = paper->h - y0 * 2;
	if ( inner_w < 16 || inner_h < 16 ) {
		SDL_FillRect( panel, NULL, SDL_MapRGBA( panel->format, 214, 197, 168, 255 ) );
		return;
	}
	slice_w = inner_h * panel->w / panel->h;
	if ( slice_w > inner_w ) slice_w = inner_w;
	if ( slice_w < 8 ) slice_w = 8;
	src.x = x0 + ( inner_w - slice_w ) / 2;
	src.y = y0;
	src.w = slice_w; src.h = inner_h;
	dst.x = 0; dst.y = 0; dst.w = panel->w; dst.h = panel->h;
	SDL_BlitScaled( paper, &src, panel, &dst );
}

/*	One strip: all rows onto a transparent RGBA sheet, then upload.			*/
static int sw_ovl_build_side( const SW_OVL_ROW *rows, int n, int strip_w, int ix, int win_h )
{
	SDL_Surface *panel;
	/*	r259b: the sheet is at least the window tall, so its paper reaches both
		edges - a short list used to leave bare black under (and over) it.	*/
	int h = n * SW_OVL_LINE_H + SW_OVL_PAD_Y * 2;
	if ( h < win_h ) h = win_h;
	int i;
	int key_x, desc_x, title_x;
	int max_key = 0, max_desc = 0, tmp_w, tmp_h, k, pass;
	TTF_Font *font = sw_ovl_font;

	/*	Pin the block to its screen edge: measure the two column widths, then
		start the left panel at the left margin and end the right panel's
		description at the right margin, so the block rides the edge whatever
		the font's real advance is.  If the two columns cannot both fit the
		strip at 16px, the whole block steps down to 13px and then 11px.		*/
	for ( pass = 0; pass < 3; pass++ ) {
		if ( font == NULL ) break;
		max_key = max_desc = 0;
		for ( k = 0; k < n; k++ ) {
			if ( rows[k].key == NULL ) continue;
			if ( TTF_SizeUTF8( font, rows[k].key, &tmp_w, &tmp_h ) == 0 && tmp_w > max_key ) max_key = tmp_w;
			if ( TTF_SizeUTF8( font, rows[k].desc, &tmp_w, &tmp_h ) == 0 && tmp_w > max_desc ) max_desc = tmp_w;
		}
		if ( max_key + max_desc + SW_OVL_COL_GAP + SW_OVL_MARGIN * 2 <= strip_w ) break;
		if ( font == sw_ovl_font && sw_ovl_font_chat != NULL ) font = sw_ovl_font_chat;
		else font = sw_ovl_font_small;
		if ( font == NULL ) break;
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
	sw_ovl_paper_fill( panel, ix );

	for ( i = 0; i < n; i++ ) {
		const SW_OVL_ROW *r = &rows[i];
		int y = SW_OVL_PAD_Y + i * SW_OVL_LINE_H + ( font == sw_ovl_font ? 0 : 3 );
		if ( r->key == NULL ) {
			sw_ovl_blit_f( panel, font, r->desc, 205, title_x, y );
		} else {
			sw_ovl_blit_f( panel, font, r->key, 255, key_x, y );
			sw_ovl_blit_f( panel, font, r->desc, 230, desc_x, y );
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

static int sw_ovl_build( int left_w, int right_w, int win_h )
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
	if ( sw_ovl_build_side( sw_ovl_left, SW_OVL_LEFT_N, left_w, 0, win_h ) != 0 ) { sw_ovl_failed = 1; return -1; }
	if ( sw_ovl_build_side( sw_ovl_right, SW_OVL_RIGHT_N, right_w, 1, win_h ) != 0 ) { sw_ovl_failed = 1; return -1; }
	sw_ovl_tex_win_h = win_h;
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
		width, a resized window its height); the content itself is static.	*/
	if ( !sw_ovl_tried || origin_x != sw_ovl_origin_x || win_h != sw_ovl_tex_win_h ) {
		if ( sw_ovl_build( origin_x, right_w, win_h ) != 0 ) return;
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

	/*	r259b: pinned to the top edge - the sheet is at least a window tall, and
		a taller one must lose its surplus at the bottom rather than both ends. */
	ly0 = 0;
	ry0 = 0;
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
	sw_ovl_pv_buffname = NULL;
	sw_ovl_pv_bdataref = NULL;

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

	id = code_getdebug_seekvar( "buffname" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_buffname = &ctx->mem_var[ id ];

	id = code_getdebug_seekvar( "bdataref" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_bdataref = &ctx->mem_var[ id ];
	if ( sw_ovl_pv_bdataref == NULL )
		sw_ovl_pv_bdataref = sw_ovl_find_dim( ctx, HSPVAR_FLAG_INT, 7, 72, -1 );

	id = code_getdebug_seekvar( "trait" );
	if ( id >= 0 && id < ctx->hsphed->max_val ) sw_ovl_pv_trait = &ctx->mem_var[ id ];

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

/*	r260: how many ether-disease symptoms the player carries, and the worst
	stage among them.  Every ether trait holds 0 when healthy and a negative
	stage (-1..-3) once the mutation took hold.							*/
static int sw_ovl_ether_scan( int *worst )
{
	int i, cnt = 0, w = 0;

	*worst = 0;
	if ( sw_ovl_pv_trait == NULL ) return 0;
	if ( sw_ovl_pv_trait->len[1] <= SW_OVL_ETHER_LAST ) return 0;
	for ( i = SW_OVL_ETHER_FIRST; i <= SW_OVL_ETHER_LAST; i++ ) {
		int v = ( (int*)sw_ovl_pv_trait->pt )[ i ];
		if ( v < 0 ) { cnt++; if ( -v > w ) w = -v; }
	}
	*worst = w;
	return cnt;
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

/*	r277: one word from the game's own buff table (sdim buffname,20,72).
	The interpreter keeps its strings in GBK like the rest of the game.	*/
static const char *sw_ovl_utf8( const char *in );	/* defined further down */

static const char *sw_ovl_buff_name( int id )
{
	static char out[ 64 ];
	const char *s;

	out[ 0 ] = '\0';
	if ( sw_ovl_pv_buffname == NULL ) return out;
	if ( id <= 0 ) return out;
	if ( id >= sw_ovl_pv_buffname->len[1] && id >= sw_ovl_pv_buffname->len[0] ) return out;
	s = (const char*)HspVarCorePtrAPTR( sw_ovl_pv_buffname, id );
	if ( s == NULL || s[ 0 ] == '\0' ) return out;
	snprintf( out, sizeof( out ), "%s", sw_ovl_utf8( s ) );
	return out;
}

/*	The buff ids currently riding on one character, split into buffs and
	hexes.  Buff id 0 is "none"; a cell with no turns left has expired.
	The type comes from the game's own table (bdataref field 0): 1 = buff,
	2 = hex.  When the table is unreachable everything counts as a buff so
	nothing silently disappears from the sheet.							*/
static int sw_ovl_buff_is_hex( int id )
{
	if ( sw_ovl_pv_bdataref == NULL ) return 0;
	if ( id < 0 || id >= sw_ovl_pv_bdataref->len[2] ) return 0;
	return ((int*)sw_ovl_pv_bdataref->pt)[ id * sw_ovl_pv_bdataref->len[1] ] == 2;
}

static void sw_ovl_buff_scan( int slot, int *bids, int *nb, int *dids, int *nd )
{
	int i, id, turns;

	*nb = 0;
	*nd = 0;
	if ( slot < 0 ) return;
	for ( i = 0; i < SW_OVL_CD_BUFF_N; i++ ) {
		id = sw_ovl_cdata( SW_OVL_CD_BUFF_BASE + i * SW_OVL_CD_BUFF_STRIDE, slot );
		turns = sw_ovl_cdata( SW_OVL_CD_BUFF_BASE + i * SW_OVL_CD_BUFF_STRIDE
			+ SW_OVL_CD_BUFF_TURNS, slot );
		if ( id <= 0 || turns <= 0 ) continue;
		if ( sw_ovl_buff_is_hex( id ) ) {
			if ( *nd < SW_OVL_FOE_BUFF_SHOWN ) dids[ ( *nd )++ ] = id;
		} else {
			if ( *nb < SW_OVL_FOE_BUFF_SHOWN ) bids[ ( *nb )++ ] = id;
		}
	}
}

/*	Fill one battle-sheet target.  slot 0 is "the player itself" in Elona's
	TARGET field, so it means nobody.  A dead or empty slot reads as none. */
static void sw_ovl_foe_read( int which, int slot )
{
	SW_OVL_FOE *f = &sw_ovl_foe[ which ];
	int i;

	f->slot = -1;
	f->level = f->speed = f->pv = f->dv = f->hp = f->mhp = 0;
	f->nbuff = 0;
	f->ndebuff = 0;
	f->name[ 0 ] = '\0';
	for ( i = 0; i < SW_OVL_BAT_COND_N; i++ ) f->cond[ i ] = 0;
	if ( slot <= 0 ) return;
	if ( sw_ovl_cdata( SW_OVL_PARTY_CD_EXIST, slot ) != 1 ) return;

	f->slot = slot;
	snprintf( f->name, sizeof( f->name ), "%s", sw_ovl_cdatan( 0, slot ) );
	f->level = sw_ovl_cdata( SW_OVL_CD_LEVEL, slot );
	f->speed = sw_ovl_cdata( SW_OVL_CD_SPEED, slot );
	f->pv    = sw_ovl_cdata( SW_OVL_CD_PV, slot );
	f->dv    = sw_ovl_cdata( SW_OVL_CD_DV, slot );
	f->hp    = sw_ovl_cdata( SW_OVL_PARTY_CD_HP, slot );
	f->mhp   = sw_ovl_cdata( SW_OVL_PARTY_CD_MAX_HP, slot );
	for ( i = 0; i < SW_OVL_BAT_COND_N; i++ )
		f->cond[ i ] = sw_ovl_cdata( sw_ovl_bat_cond[ i ].field, slot );
	sw_ovl_buff_scan( slot, f->buffid, &f->nbuff, f->debuffid, &f->ndebuff );
}

/*	Values that move during a fight; the sheet is rebuilt when it changes. */
static unsigned int sw_ovl_foe_hash( void )
{
	unsigned int h = 2166136261u;
	int i, j;

#define FH( v ) h = ( h ^ (unsigned int)( v ) ) * 16777619u
	for ( i = 0; i < 2; i++ ) {
		SW_OVL_FOE *f = &sw_ovl_foe[ i ];
		FH( f->slot ); FH( f->level ); FH( f->speed ); FH( f->pv ); FH( f->dv );
		FH( f->hp ); FH( f->mhp ); FH( f->nbuff ); FH( f->ndebuff );
		for ( j = 0; j < SW_OVL_BAT_COND_N; j++ ) FH( f->cond[ j ] );
		for ( j = 0; j < f->nbuff; j++ ) FH( f->buffid[ j ] );
		for ( j = 0; j < f->ndebuff; j++ ) FH( f->debuffid[ j ] );
		for ( j = 0; j < 8 && f->name[ j ]; j++ ) FH( (unsigned char)f->name[ j ] );
	}
	FH( sw_ovl_pet_speed ); FH( sw_ovl_pet_pv ); FH( sw_ovl_pet_dv );
	FH( sw_ovl_pet_nbuff ); FH( sw_ovl_pet_ndebuff );
	for ( j = 0; j < sw_ovl_pet_nbuff; j++ ) FH( sw_ovl_pet_buffid[ j ] );
	for ( j = 0; j < sw_ovl_pet_ndebuff; j++ ) FH( sw_ovl_pet_debuffid[ j ] );
	FH( sw_ovl_ply_speed ); FH( sw_ovl_ply_pv ); FH( sw_ovl_ply_dv );
	FH( sw_ovl_ply_nbuff ); FH( sw_ovl_ply_ndebuff );
	for ( j = 0; j < sw_ovl_ply_nbuff; j++ ) FH( sw_ovl_ply_buffid[ j ] );
	for ( j = 0; j < sw_ovl_ply_ndebuff; j++ ) FH( sw_ovl_ply_debuffid[ j ] );
#undef FH
	return h;
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

/*	r295/r297: the game's own narration about a character, as opposed to
	something she says.  Three sources, all fixed wording:

	1. "txtskillchange" table in the .ax - attribute and skill growth:

		感到生命力的提升。 / 察觉到生命力在衰退。
		感到玛那变强了。 / 感到玛那衰弱了。
		变强壮了。 / 感觉它长了点赘肉的样子。
		体会到了忍耐的快感。 / 变得没有耐力了。
		变得灵巧了。 / 变得笨拙了。
		它开始觉得世界变得更为亲近了。 / 感到了感觉的偏差。
		忽然变得想学各种东西了。 / 的学习意愿下降了。
		的意志愈发坚定了。 / 变得容易放弃了。
		察觉到魔力有所增长。 / 感觉到魔力在衰退。
		感觉周围的目光很惬意。 / 突然变得不愿在人前露面了。
		觉得周围的动作变慢了。 / 变迟钝了。
		变得幸运了。 / 变得不幸了。
		将技能的进步。 / 将技能的退步。

	2. Inline verbs from proc.hsp - *eat / *drinkPotion build

		npcn(cc) + itemname(ci, 1) + "を食べ終えた。"	→ "<名字>吃完了<食物>。"
		npcn(tc) + itemname(ci, 1) + "を飲み干した。"	→ "<名字>喝光了<饮料>。"
		npcn(cc) + itemname(ci, 1) + "を…送进了嘴中。"	→ 开始吃的提示

	3. The condition table (@11934500) and the interaction results
	   (@12360600) - poison, bleeding, falling asleep, being hugged, and
	   the reactions to a pat or a whisper.

	Every one of them arrives as "<名字>" + one of these, so a name filter
	alone would treat it as something she said - it is not, it is the game
	narrating her.  Matching the fixed fragments is exact, and it keeps
	those rows out of the panel's chatter.									*/
static int sw_ovl_ai_story_line( const char *u )
{
	static const char *frag[] = {
		/*	txtskillchange table											*/
		"生命力的提升", "生命力在衰退", "玛那变强", "玛那衰弱",
		"变强壮了", "长了点赘肉", "忍耐的快感", "没有耐力",
		"变得灵巧了", "变得笨拙了", "世界变得更为亲近", "感觉的偏差",
		"想学各种东西", "学习意愿下降", "意志愈发坚定", "变得容易放弃",
		"魔力有所增长", "魔力在衰退", "目光很惬意", "不愿在人前露面",
		"动作变慢了", "变迟钝了", "变得幸运了", "变得不幸了",
		"技能的进步", "技能的退步",
		/*	food and drink verbs											*/
		"送进了嘴中", "吃完了", "喝光了", "吃掉了", "喝掉了",
		/*	condition table (@11934500)										*/
		"害怕了", "开始颤栗了", "混乱了", "麻痹了", "中毒了", "睡着了",
		"开始流血了", "喝醉了", "发狂了", "生病了", "意识模糊了",
		"感到了恐惧", "失禁了", "遭到冥界寒气的侵蚀", "翻着白眼倒下了",
		/*	recovering from them											*/
		"从混乱中恢复了", "从失明中恢复了", "从恐惧中重新振作",
		"从胆怯中恢复了", "的颤栗停止了",
		/*	interaction results (@12360600)									*/
		"的身体好热", "反倒紧紧抱住", "默不作声，侧耳倾听", "抱紧了你，在耳畔",
		"把你紧紧地搂住", "骨碌骨碌地滚着", "主动凑上前来用身子蹭你",
		"痒得不住地扭动身子", "一脸沉醉地听着你说话", "玩笑般地笑了",
		/*	riding															*/
		"骑上了你",
	};
	int i;

	if ( u == NULL || u[ 0 ] == '\0' ) return 0;
	for ( i = 0; i < (int)( sizeof( frag ) / sizeof( frag[ 0 ] ) ); i++ )
		if ( strstr( u, frag[ i ] ) != NULL ) return 1;
	return 0;
}

/*	r300: what gets fed to the model.  Three groups, all reproduced from the
	code rather than from the pixels - the colour never reaches the log:

	  green  module.hsp:363/645  txtef COLOR_GREEN before txtskillchange(..., 0, ...)
	  red    module.hsp:393/677  txtef COLOR_RED   before txtskillchange(..., 1, ...)
	         plus the condition table (@11934500), which is the red half of a
	         fight: poison, bleeding, confusion, paralysis, falling asleep
	  food   proc.hsp *eat / *drinkPotion - white, but asked for by name:
	         npcn(cc) + itemname + "を食べ終えた。" → "<名字>吃完了<食物>。"
	         npcn(tc) + itemname + "を飲み干した。" → "<名字>喝光了<饮料>。"

	The follower's own spoken lines are never here - those are the panel's
	chatter and feeding them back would make her parrot herself.			*/
static int sw_ovl_ai_feed_line( const char *u )
{
	static const char *frag[] = {
		/*	green: growth													*/
		"生命力的提升", "玛那变强", "变强壮了", "忍耐的快感",
		"变得灵巧了", "世界变得更为亲近", "想学各种东西", "意志愈发坚定",
		"魔力有所增长", "目光很惬意", "动作变慢了", "变得幸运了",
		"技能的进步",
		/*	red: decline													*/
		"生命力在衰退", "玛那衰弱", "长了点赘肉", "没有耐力",
		"变得笨拙了", "感觉的偏差", "学习意愿下降", "变得容易放弃",
		"魔力在衰退", "不愿在人前露面", "变迟钝了", "变得不幸了",
		"技能的退步",
		/*	red: condition table											*/
		"害怕了", "开始颤栗了", "混乱了", "麻痹了", "中毒了", "睡着了",
		"开始流血了", "喝醉了", "发狂了", "生病了", "意识模糊了",
		"感到了恐惧", "失禁了", "遭到冥界寒气的侵蚀", "翻着白眼倒下了",
		/*	food and drink													*/
		"送进了嘴中", "吃完了", "喝光了", "吃掉了", "喝掉了",
	};
	int i;

	if ( u == NULL || u[ 0 ] == '\0' ) return 0;
	for ( i = 0; i < (int)( sizeof( frag ) / sizeof( frag[ 0 ] ) ); i++ )
		if ( strstr( u, frag[ i ] ) != NULL ) return 1;
	return 0;
}

/*	r297: the watcher hands the model a fact about the follower, so her own
	name becomes "你" - "X 中毒了。" turns into "你中毒了。".  Without this
	she answers in the third person about herself.							*/
static void sw_ovl_ai_self_phrase( const char *u, const char *nm, char *out, size_t n )
{
	const char *p;
	size_t pre;

	if ( u == NULL || nm == NULL || nm[ 0 ] == '\0' || out == NULL || n < 8 ) {
		if ( out != NULL && n > 0 ) snprintf( out, n, "%s", u != NULL ? u : "" );
		return;
	}
	p = strstr( u, nm );
	if ( p == NULL ) { snprintf( out, n, "%s", u ); return; }
	pre = (size_t)( p - u );
	if ( pre > n - 4 ) pre = n - 4;
	memcpy( out, u, pre );
	snprintf( out + pre, n - pre, "你%s", p + strlen( nm ) );
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
				/*	r295: "X 将…技能的进步。" names her but is the game
					talking about her stats, not her talking.  Keeping it
					out here is what separates the two kinds of row.	*/
				if ( sw_ovl_ai_story_line( sw_ovl_utf8( s ) ) ) break;
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

/*	r292/r296: the newest rows of the game's log, whatever they say.
	msg_peek() above only returns the follower's own lines; the watcher also
	needs the rows that merely talk ABOUT her ("X 的重武器精通精进了"),
	and one action can leave several rows behind ("…送进了嘴中" then the
	taste line), so it walks back a few.  Raw GBK.						*/
static int sw_ovl_msg_row( int back, char *out, size_t n )
{
	int i, seen = 0;
	const char *s;

	if ( sw_ovl_pv_msg == NULL || out == NULL || n < 2 ) return 0;
	for ( i = (int)sw_ovl_pv_msg->len[1] - 1; i >= 0; i-- ) {
		s = (const char*)HspVarCorePtrAPTR( sw_ovl_pv_msg, i );
		if ( s == NULL || s[ 0 ] == '\0' ) continue;
		if ( seen == back ) {
			snprintf( out, n, "%s", s );
			return 1;
		}
		seen++;
	}
	return 0;
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
	Uint32 fill = SDL_MapRGBA( panel->format, 124, 105, 80, 255 );
	Uint32 edge = SDL_MapRGBA( panel->format, 66, 52, 38, 255 );

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

static void sw_ovl_chat_lines( SDL_Surface *panel, const char *msg, int x, int y, int maxl, int bottom )
{
	/*	r272: 32 KB of rows on the render thread's stack was asking for
		trouble - with three long answers the bottom rows went missing.	*/
	static char rows[ 128 ][ 256 ];
	char buf[ 256 ];
	int len, nr = 0, start, rrow, row, s, maxw, n, cw, w, h, shown;

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
	shown = nr - start;
	/*	r272: bottom mode takes y as the baseline the last row sits on, so a
		two-line answer hugs the bottom instead of floating mid-strip.	*/
	if ( bottom && shown > 0 ) y -= shown * 18;
	for ( row = start, rrow = 0; row < nr; row++, rrow++ ) {
		/*	r187v: colour like the game's own log below the HP bars - a quoted
			line is dialogue (init.hsp msg_write paints those 210,250,160),
			anything else is the plain log white.  */
		if ( strchr( rows[ row ], '"' ) != NULL ) {
			sw_ovl_blit_chat( panel, rows[ row ], 44, 72, 34, x, y + rrow * 18 );
		} else {
			sw_ovl_blit_chat( panel, rows[ row ], 40, 34, 28, x, y + rrow * 18 );
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

/*	cnvweight() from text.hsp: value / 1000 with one truncated decimal.	*/
static void sw_ovl_ply_wfmt( int v, char *out, size_t n )
{
	int a = v < 0 ? -v : v;
	snprintf( out, n, "%s%d.%d", v < 0 ? "-" : "", a / 1000, ( a % 1000 ) / 100 );
}

/*	One row of that block: label in grey, value bright, in the same 16px bold
	face as the HP/MP/SP rows above so the strip reads as one table.  When the
	pair does not fit across the strip the value moves to the next row - the
	strip is only ~91px of usable width.								*/
static int sw_ovl_ply_line( SDL_Surface *panel, const char *label, const char *value,
				int y, int strip_w, int hot )
{
	char full[ 200 ];
	Uint8 vr = 32, vg = 26, vb = 20;		/* ink on parchment	*/
	int avail = strip_w - 16, w = 0, lw = 0;

	if ( sw_ovl_font == NULL ) return 1;
	if ( avail < 32 ) avail = 32;
	if ( hot ) { vr = 158; vg = 32; vb = 24; }
	if ( label[ 0 ] == '\0' ) {			/* no label: the value takes the row */
		sw_ovl_blit_text_rgb( panel, value, vr, vg, vb, 8, y );
		return 1;
	}
	snprintf( full, sizeof( full ), "%s %s", label, value );
	if ( TTF_SizeUTF8( sw_ovl_font, full, &w, NULL ) != 0 ) w = 0;
	sw_ovl_blit_text_rgb( panel, label, 106, 92, 70, 8, y );
	if ( w <= avail ) {
		if ( TTF_SizeUTF8( sw_ovl_font, label, &lw, NULL ) != 0 ) lw = 0;
		sw_ovl_blit_text_rgb( panel, value, vr, vg, vb, 8 + lw + 6, y );
		return 1;
	}
	sw_ovl_blit_text_rgb( panel, value, vr, vg, vb, 8, y + SW_OVL_PLY_LINE_H );
	return 2;
}

/*	r272: the game's own two scales, taken from init.hsp (_impressiona7 and
	_impressionb3) and chara_func.hsp (implevel / implevel2).  They are not
	the same thing: the first is plain affection, the second is the master-
	servant axis a follower also carries.  A follower sitting at 50 reads
	"普通", exactly as the character sheet shows.							*/
static const char *sw_ovl_imp_name( int v )
{
	/*	Names copied verbatim from start.ax (the _impressiona7 table):
		Foe / Hate / Annoying / Normal / Amiable / Family / Dear / Doting /
		MatureLove.  The panel used to say 厌恶-好意-深爱, which never appear
		in the character sheet.											*/
	if ( v < 10 ) return "最讨厌";
	if ( v < 25 ) return "讨厌";
	if ( v < 40 ) return "烦人";
	if ( v < 75 ) return "普通";
	if ( v < 100 ) return "善意的";
	if ( v < 150 ) return "家人";
	if ( v < 200 ) return "亲爱";
	if ( v < 300 ) return "溺爱";
	return "熟爱";
}

static const char *sw_ovl_ms_name( int v )
{
	/*	_impressionb3: Uninhibited / Peer / Docile / Allegiance / Obedience /
		Adoration / Loyalty.												*/
	if ( v < 0 ) return "奔放";
	if ( v < 100 ) return "对等";
	if ( v < 150 ) return "坦诚";
	if ( v < 200 ) return "忠诚";
	if ( v < 250 ) return "恭顺";
	if ( v < 300 ) return "醉心";
	return "忠义";
}

/*	r275: instead of writing one detector per number, the panel keeps the
	snapshot it had when she last spoke and prints the deltas itself.  The
	model is bad at comparing two blocks of numbers, but good at reacting to
	"金币 1200→950".  The baseline moves only when a new line is shown, so a
	long silence accumulates into one honest summary.					*/
#define SW_OVL_AI_DIFF_N	8
static int		sw_ovl_ai_dbase[ SW_OVL_AI_DIFF_N ];
static int		sw_ovl_ai_dbase_ok = 0;
static int		sw_ovl_ai_dbase_seq = -1;

static void sw_ovl_ai_dsnap( int *v )
{
	v[ 0 ] = sw_ovl_party_mhp[0] > 0 ? sw_ovl_party_hp[0] * 100 / sw_ovl_party_mhp[0] : 0;
	v[ 1 ] = sw_ovl_party_mmp[0] > 0 ? sw_ovl_party_mp[0] * 100 / sw_ovl_party_mmp[0] : 0;
	v[ 2 ] = sw_ovl_ply_gold[0];
	v[ 3 ] = sw_ovl_ply_cargo;
	v[ 4 ] = sw_ovl_ply_hunger[0] / 100;
	v[ 5 ] = sw_ovl_ply_level;
	v[ 6 ] = sw_ovl_ply_weather;
	v[ 7 ] = sw_ovl_ply_hour / 4;
}

static void sw_ovl_ai_diff( char *out, size_t n )
{
	int cur[ SW_OVL_AI_DIFF_N ];
	int i, cnt = 0;
	char one[ 80 ], wa[ 32 ], wb[ 32 ];

	out[ 0 ] = '\0';
	sw_ovl_ai_dsnap( cur );
	if ( !sw_ovl_ai_dbase_ok ) { snprintf( out, n, "—" ); return; }
	for ( i = 0; i < SW_OVL_AI_DIFF_N && cnt < 4; i++ ) {
		if ( cur[ i ] == sw_ovl_ai_dbase[ i ] ) continue;
		switch ( i ) {
		case 0: snprintf( one, sizeof( one ), "主人HP %d%%→%d%%",
					sw_ovl_ai_dbase[ i ], cur[ i ] ); break;
		case 1: snprintf( one, sizeof( one ), "主人MP %d%%→%d%%",
					sw_ovl_ai_dbase[ i ], cur[ i ] ); break;
		case 2: snprintf( one, sizeof( one ), "金币 %d→%d",
					sw_ovl_ai_dbase[ i ], cur[ i ] ); break;
		case 3:
			sw_ovl_ply_wfmt( sw_ovl_ai_dbase[ i ], wa, sizeof( wa ) );
			sw_ovl_ply_wfmt( cur[ i ], wb, sizeof( wb ) );
			snprintf( one, sizeof( one ), "负重 %s→%s", wa, wb );
			break;
		case 4: snprintf( one, sizeof( one ), "饱腹 %d%%→%d%%",
					sw_ovl_ai_dbase[ i ], cur[ i ] ); break;
		case 5: snprintf( one, sizeof( one ), "等级 %d→%d",
					sw_ovl_ai_dbase[ i ], cur[ i ] ); break;
		case 6: snprintf( one, sizeof( one ), "天气 %s→%s",
					sw_ovl_ply_weather_name( sw_ovl_ai_dbase[ i ] ),
					sw_ovl_ply_weather_name( cur[ i ] ) ); break;
		default: snprintf( one, sizeof( one ), "时段 %s→%s",
					sw_ovl_ply_time_name( sw_ovl_ai_dbase[ i ] ),
					sw_ovl_ply_time_name( cur[ i ] ) ); break;
		}
		if ( out[ 0 ] ) strncat( out, "；", n - strlen( out ) - 1 );
		strncat( out, one, n - strlen( out ) - 1 );
		cnt++;
	}
	if ( out[ 0 ] == '\0' ) snprintf( out, n, "无" );
}

/*	r261: the block the model is told about the current moment.  Everything
	here is already human-readable - no raw field numbers, no ids - because
	the model would only guess at what "gdata 20 = 12" means.				*/
static void sw_ovl_ai_scene( char *out, size_t n, int pet, const char *where )
{
	char cond[ 128 ], wp[ 64 ], a[ 32 ], b[ 32 ], recent[ 384 ], diff[ 256 ];
	int first = 1, c, php, php2, hunger, weight, carry;
	size_t rl, i;

	cond[ 0 ] = '\0';
	for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
		if ( sw_ovl_ply_cond[0][ c ] <= 0 ) continue;
		if ( !first ) strncat( cond, "、", sizeof( cond ) - 1 - strlen( cond ) );
		strncat( cond, sw_ovl_bat_cond[ c ].name, sizeof( cond ) - 1 - strlen( cond ) );
		first = 0;
	}
	if ( first ) snprintf( cond, sizeof( cond ), "正常" );

	php  = sw_ovl_party_mhp[0] > 0 ? sw_ovl_party_hp[0] * 100 / sw_ovl_party_mhp[0] : 0;
	php2 = sw_ovl_party_mhp[pet] > 0 ? sw_ovl_party_hp[pet] * 100 / sw_ovl_party_mhp[pet] : 0;
	hunger = sw_ovl_lim( sw_ovl_ply_hunger[pet] / 100, 0, 100 );
	weight = sw_ovl_ply_weight[pet];
	carry  = sw_ovl_ply_carry[pet];
	sw_ovl_ply_wfmt( weight, a, sizeof( a ) );
	sw_ovl_ply_wfmt( carry, b, sizeof( b ) );
	snprintf( wp, sizeof( wp ), "%s/%s", a, b );

	/*	The newest game log line, flattened: a newline inside the block would
		break the "最近：" row into two unrelated sentences.  The game stores it
		in GBK, so it is converted first - the request body is UTF-8 and a raw
		GBK byte would make the API reject the whole JSON with a 400.		*/
	snprintf( recent, sizeof( recent ), "%s",
		sw_ovl_party_chat[1][ 0 ] ? sw_ovl_utf8( sw_ovl_party_chat[1] ) : "安静" );
	rl = strlen( recent );
	for ( i = 0; i < rl; i++ ) if ( recent[ i ] == '\n' || recent[ i ] == '\r' ) recent[ i ] = ' ';
	if ( rl > 200 ) {
		/*	Cut on a UTF-8 lead byte, otherwise the first character is sliced
			and the body is invalid again.								*/
		i = rl - 200;
		while ( i < rl && ( (unsigned char)recent[ i ] & 0xC0 ) == 0x80 ) i++;
		memmove( recent, recent + i, rl - i + 1 );
	}

	sw_ovl_ai_diff( diff, sizeof( diff ) );
	snprintf( out, n,
		"地点：%s  时间：%d/%d/%d %s %s\n"
		"主人：HP %d%%，状态 %s\n"
		"你：HP %d%%，饱腹 %d%%，负重 %s\n"
		"关系：主人对你的好感 %d（%s），主从 %d（%s）\n"
		"变化（自你上次开口）：%s\n"
		"最近：%s",
		where, sw_ovl_ply_year, sw_ovl_ply_month, sw_ovl_ply_day,
		sw_ovl_ply_time_name( sw_ovl_ply_hour / 4 ),
		sw_ovl_ply_weather_name( sw_ovl_ply_weather ),
		php, cond, php2, hunger, wp,
		sw_ovl_party_imp[ pet ], sw_ovl_imp_name( sw_ovl_party_imp[ pet ] ),
		sw_ovl_party_ms[ pet ], sw_ovl_ms_name( sw_ovl_party_ms[ pet ] ),
		diff,
		recent );

	/*	The baseline follows the words, not the clock: every sentence she
		says resets it, so the next request reports what happened since.	*/
	if ( ai_chat_seq() != sw_ovl_ai_dbase_seq ) {
		sw_ovl_ai_dsnap( sw_ovl_ai_dbase );
		sw_ovl_ai_dbase_ok = 1;
		sw_ovl_ai_dbase_seq = ai_chat_seq();
	}
}

/*	A cheap hash of the values that move when the player acts.  The log row
	is included because a turn almost always writes one even when the numbers
	happen to stay the same.												*/
static unsigned int sw_ovl_ai_activity( void )
{
	unsigned int h = 2166136261u;
	int i;

#define AI_FP( v ) h = ( h ^ (unsigned int)( v ) ) * 16777619u
	AI_FP( sw_ovl_party_hp[0] );   AI_FP( sw_ovl_party_mhp[0] );
	AI_FP( sw_ovl_party_hp[1] );   AI_FP( sw_ovl_party_mhp[1] );
	AI_FP( sw_ovl_ply_gold[0] );   AI_FP( sw_ovl_ply_hunger[0] );
	AI_FP( sw_ovl_ply_weight[0] );
	AI_FP( sw_ovl_ply_level );     AI_FP( sw_ovl_ply_hour );
	AI_FP( sw_ovl_ply_day );       AI_FP( sw_ovl_ply_month );
	AI_FP( sw_ovl_ply_ether );     AI_FP( sw_ovl_ply_cargo );
	AI_FP( sw_ovl_ply_weather );   AI_FP( sw_ovl_battle_on );
	for ( i = 0; i < SW_OVL_BAT_COND_N; i++ ) AI_FP( sw_ovl_ply_cond[0][ i ] );
	for ( i = 0; i < 64 && sw_ovl_party_chat[1][ i ]; i++ )
		h = ( h ^ (unsigned char)sw_ovl_party_chat[1][ i ] ) * 16777619u;
#undef AI_FP
	return h;
}

/*	Most log rows are footsteps and failed searches; repeating them made her
	sound like a second message window.  Level-ups, loot, quests and deaths
	are the ones a companion would actually remark on.					*/
static int sw_ovl_ai_log_hot( const char *s )
{
	static const char *kw[] = {
		"提升", "学会", "捡到", "拾到", "获得", "任务", "委托",
		"死亡", "倒下", "复活", "击倒", "杀掉", "宝箱", "通缉", NULL
	};
	int i;
	if ( s == NULL ) return 0;
	for ( i = 0; kw[ i ] != NULL; i++ ) if ( strstr( s, kw[ i ] ) != NULL ) return 1;
	return 0;
}

/*	r262: turn a change in the tracked numbers into a sentence the model can
	react to.  Everything here reads the caches sw_ovl_party_refresh already
	filled, so the watcher costs a few comparisons per frame and nothing at
	all when the game is idle.  Thresholds are deliberately coarse: the
	model only needs "something happened", not a telemetry feed.			*/
static void sw_ovl_ai_watch( void )
{
	unsigned int now = SDL_GetTicks();
	char line[ 240 ], note[ 384 ], logbuf[ 240 ];
	const char *p;
	size_t n;
	int hp_pct, i;

	if ( now - sw_ovl_ai_w_tick < 500 ) return;		/* twice a second is plenty */
	sw_ovl_ai_w_tick = now;

	/*	1. The game's own log.  One line per change, at most one every four
		seconds, and never the follower's own words (those are already the
		panel's chatter and repeating them makes her sound like a parrot).	*/
	if ( sw_ovl_party_chat[1][ 0 ] ) {
		/*	sw_ovl_utf8() hands back one shared buffer, so the line is copied
			before the name is converted - otherwise the second call would
			overwrite the string being searched.							*/
		snprintf( logbuf, sizeof( logbuf ), "%s", sw_ovl_utf8( sw_ovl_party_chat[1] ) );
		p = logbuf;
		if ( strcmp( p, sw_ovl_ai_w_log ) != 0 && now - sw_ovl_ai_w_log_tick >= 4000 &&
				strstr( p, sw_ovl_utf8( sw_ovl_party_name[1] ) ) == NULL ) {
			n = strlen( p );
			i = 0;
			while ( i < (int)n && i < 120 ) i++;
			while ( i > 0 && i < (int)n && ( (unsigned char)p[ i ] & 0xC0 ) == 0x80 ) i--;
			snprintf( note, sizeof( note ), "游戏里刚刚发生：%.*s", i, p );
			for ( n = 0; note[ n ]; n++ )
				if ( note[ n ] == '\n' || note[ n ] == '\r' ) note[ n ] = ' ';
			ai_chat_notify_event( note, sw_ovl_ai_log_hot( logbuf ) ? 2 : 1 );
			/*	Only remember the line once it has been sent, so a burst of
				log rows inside the cooldown still produces one report of the
				latest one instead of being swallowed here.				*/
			snprintf( sw_ovl_ai_w_log, sizeof( sw_ovl_ai_w_log ), "%s", p );
			sw_ovl_ai_w_log_tick = now;
		}
	}

	/*	r292: rows that talk ABOUT the follower.  Section 1 drops them on
		purpose - a row carrying her name is usually her own speech, and
		echoing that back makes her parrot herself - but skill practice,
		eating and drinking are the game narrating her, so they get a forced
		line even while the log is otherwise busy.  One action can leave
		several rows, hence the short walk back.							*/
	{
		char newest[ 300 ];
		if ( sw_ovl_msg_row( 0, newest, sizeof( newest ) ) ) {
			char un[ 400 ];
			snprintf( un, sizeof( un ), "%s", sw_ovl_utf8( newest ) );
			if ( strcmp( un, sw_ovl_ai_w_msg ) != 0 ) {
				char nm[ 96 ], prev[ 300 ];
				int back;
				snprintf( prev, sizeof( prev ), "%s", sw_ovl_ai_w_msg );
				snprintf( nm, sizeof( nm ), "%s", sw_ovl_utf8( sw_ovl_party_name[1] ) );
				snprintf( sw_ovl_ai_w_msg, sizeof( sw_ovl_ai_w_msg ), "%s", un );
				for ( back = 0; back < 4 && nm[ 0 ]; back++ ) {
					char raw[ 300 ], u[ 400 ], self[ 400 ];
					int mine, speech;

					if ( !sw_ovl_msg_row( back, raw, sizeof( raw ) ) ) break;
					snprintf( u, sizeof( u ), "%s", sw_ovl_utf8( raw ) );
					/*	The previous newest row was already handled with the
						last poll; stop before walking into it again.		*/
					if ( prev[ 0 ] && strcmp( u, prev ) == 0 ) break;
					/*	Her own spoken line is already the panel's chatter.	*/
					speech = ( back == 0 && sw_ovl_speaker() == sw_ovl_party_slot[1] );
					mine = ( strstr( u, nm ) != NULL || strstr( u, "随从" ) != NULL );
					/*	r300: the coloured notices, plus eating and drinking.	*/
					if ( speech || !mine || !sw_ovl_ai_feed_line( u ) ) continue;
					sw_ovl_ai_self_phrase( u, nm, self, sizeof( self ) );
					snprintf( note, sizeof( note ),
						"游戏日志里出现了关于你的消息：%.*s", 120, self );
					{
						size_t c;
						for ( c = 0; note[ c ]; c++ )
							if ( note[ c ] == '\n' || note[ c ] == '\r' ) note[ c ] = ' ';
					}
					ai_chat_notify_event( note, 4 );
					break;
				}
			}
		}
	}

	/*	r292: death and waking up.  The first read only records what the save
		was loaded with, so a load never announces a corpse.  Priority 5 puts
		both above every other event in the queue.							*/
	if ( sw_ovl_ai_w_hp_pct >= 0 && sw_ovl_party_mhp[0] > 0 ) {
		int dead = sw_ovl_party_hp[0] <= 0;
		if ( dead && !sw_ovl_ai_w_ply_dead )
			ai_chat_notify_event( "主人倒下了，失去了意识", 5 );
		else if ( !dead && sw_ovl_ai_w_ply_dead )
			ai_chat_notify_event( "主人醒过来了", 4 );
		sw_ovl_ai_w_ply_dead = dead;
	}
	if ( sw_ovl_ai_w_pet_hp >= 0 && sw_ovl_party_mhp[1] > 0 ) {
		int pct = sw_ovl_party_hp[1] * 100 / sw_ovl_party_mhp[1];
		int dead = pct <= 0;
		if ( dead && !sw_ovl_ai_w_pet_dead )
			ai_chat_notify_event( "你倒下了，眼前一黑", 5 );
		else if ( !dead && sw_ovl_ai_w_pet_dead )
			ai_chat_notify_event( "你又站了起来，喘着气", 5 );
		sw_ovl_ai_w_pet_dead = dead;
	}
	if ( sw_ovl_party_mhp[1] > 0 )
		sw_ovl_ai_w_pet_hp = sw_ovl_party_hp[1] * 100 / sw_ovl_party_mhp[1];

	/*	2. Only the events that a snapshot cannot carry survive here.  Plain
		number drift (money, weight, level, recovery) is printed as a delta
		line in the scene block instead - one generic diff beats five
		detectors.  A sudden hit and a drop below a quarter are transient:
		they can be over before the next snapshot, so they stay.			*/
	if ( sw_ovl_party_mhp[0] > 0 ) {
		hp_pct = sw_ovl_party_hp[0] * 100 / sw_ovl_party_mhp[0];
		if ( sw_ovl_ai_w_hp_pct >= 0 && sw_ovl_ai_w_hp_pct - hp_pct >= 30 )
			ai_chat_notify_event( "主人刚刚挨了重重的一下", 3 );
		/*	Reported once on the way in, not on every frame spent there.	*/
		if ( hp_pct > 0 && hp_pct < 25 && !sw_ovl_ai_w_dying )
			ai_chat_notify_event( "主人伤得很重，快撑不住了", 3 );
		sw_ovl_ai_w_dying = ( hp_pct > 0 && hp_pct < 25 );
		sw_ovl_ai_w_hp_pct = hp_pct;
	}

	/*	Hunger reads as a state, not a percentage: crossing into the lower
		quarter is what a person would actually mention.					*/
	{
		int hunger = sw_ovl_lim( sw_ovl_ply_hunger[0] / 100, 0, 100 );
		if ( sw_ovl_ai_w_hunger >= 0 && hunger / 25 < sw_ovl_ai_w_hunger / 25 ) {
			if ( hunger < 25 ) ai_chat_notify_event( "主人快饿死了", 3 );
			else if ( hunger < 50 ) ai_chat_notify_event( "主人饿了", 1 );
		}
		sw_ovl_ai_w_hunger = hunger;
	}

	if ( sw_ovl_ai_w_cargo >= 0 && sw_ovl_ply_cargol > 0 &&
			sw_ovl_ply_cargo > sw_ovl_ply_cargol && sw_ovl_ai_w_cargo <= sw_ovl_ply_cargol )
		ai_chat_notify_event( "行李已经超重了", 2 );
	sw_ovl_ai_w_cargo = sw_ovl_ply_cargo;

	if ( sw_ovl_ai_w_ether >= 0 && sw_ovl_ply_ether > sw_ovl_ai_w_ether )
		ai_chat_notify_event( "主人又染上了一种以太病", 2 );
	sw_ovl_ai_w_ether = sw_ovl_ply_ether;

	/*	r272: affection moves rarely and always means something, so it is the
		loudest event there is - priority 5 makes it beat everything else in
		the queue, and a change of tier is worth a line by itself.			*/
	if ( sw_ovl_ai_w_imp >= 0 && sw_ovl_party_imp[1] != sw_ovl_ai_w_imp ) {
		int d = sw_ovl_party_imp[1] - sw_ovl_ai_w_imp;
		snprintf( line, sizeof( line ), "主人对你的好感%s了 %d，现在是「%s」",
			d > 0 ? "提高" : "下降", d > 0 ? d : -d,
			sw_ovl_imp_name( sw_ovl_party_imp[1] ) );
		ai_chat_notify_event( line, 5 );
	}
	sw_ovl_ai_w_imp = sw_ovl_party_imp[1];

	if ( sw_ovl_ai_w_ms >= 0 && sw_ovl_party_ms[1] != sw_ovl_ai_w_ms ) {
		snprintf( line, sizeof( line ), "你和主人的主从关系变成了「%s」",
			sw_ovl_ms_name( sw_ovl_party_ms[1] ) );
		ai_chat_notify_event( line, 5 );
	}
	sw_ovl_ai_w_ms = sw_ovl_party_ms[1];

	/*	Weather and the part of day: cheap, frequent, and they give the
		follower something new to talk about while walking.  Etherwind is
		weather id 1 and deserves its own line - it is the one weather that
		changes what the player can safely do.							*/
	if ( sw_ovl_ai_w_weather >= 0 && sw_ovl_ply_weather != sw_ovl_ai_w_weather ) {
		if ( sw_ovl_ply_weather == 1 ) {
			ai_chat_notify_event( "外面刮起了以太风，出门会染上以太病", 3 );
		} else {
			snprintf( line, sizeof( line ), "天气变了，现在是%s",
				sw_ovl_ply_weather_name( sw_ovl_ply_weather ) );
			ai_chat_notify_event( line, 1 );
		}
	}
	sw_ovl_ai_w_weather = sw_ovl_ply_weather;

	if ( sw_ovl_ai_w_part >= 0 && sw_ovl_ply_hour / 4 != sw_ovl_ai_w_part ) {
		snprintf( line, sizeof( line ), "天色变了，现在是%s",
			sw_ovl_ply_time_name( sw_ovl_ply_hour / 4 ) );
		ai_chat_notify_event( line, 1 );
	}
	sw_ovl_ai_w_part = sw_ovl_ply_hour / 4;

	/*	The start of a fight is reported once; drawing the sheet already keeps
		its own copy of the flag, so the watcher needs its own.				*/
	if ( !sw_ovl_ai_w_battle && sw_ovl_battle_on )
		ai_chat_notify_event( "附近出现了敌人", 3 );
	sw_ovl_ai_w_battle = sw_ovl_battle_on;

	/*	3. Newly gained condition, by name - "你中毒了" reads better than a
		number the model would have to guess at.							*/
	if ( sw_ovl_ai_w_cond_ready ) {
		for ( i = 0; i < SW_OVL_BAT_COND_N; i++ ) {
			if ( sw_ovl_ply_cond[0][ i ] > 0 && !( sw_ovl_ai_w_cond & ( 1 << i ) ) ) {
				snprintf( line, sizeof( line ), "主人身上冒出了「%s」",
					sw_ovl_bat_cond[ i ].name );
				ai_chat_notify_event( line, 2 );
				break;
			}
		}
	}
	sw_ovl_ai_w_cond = 0;
	for ( i = 0; i < SW_OVL_BAT_COND_N; i++ )
		if ( sw_ovl_ply_cond[0][ i ] > 0 ) sw_ovl_ai_w_cond |= ( 1 << i );
	/*	The first pass only records what the save was loaded with; from then
		on a new bit is a real change.										*/
	sw_ovl_ai_w_cond_ready = 1;

	/*	r269: sleep the idle timer while nothing moves.  Two signals feed the
		clock: a change in the tracked values, and anything the pad sees.  The
		second one matters because walking around changes little the panel
		watches - without it the follower would fall asleep mid-session.	*/
	{
		unsigned int sig = sw_ovl_ai_activity();
		unsigned int pad = switch_input_last_activity();

		if ( sig != sw_ovl_ai_act_sig ) {
			sw_ovl_ai_act_sig = sig;
			sw_ovl_ai_act_tick = now;
		}
		if ( pad > sw_ovl_ai_act_tick ) sw_ovl_ai_act_tick = pad;
		if ( sw_ovl_ai_act_tick == 0 ) sw_ovl_ai_act_tick = now;

		if ( !sw_ovl_ai_idle && now - sw_ovl_ai_act_tick > SW_OVL_AI_IDLE_MS ) {
			sw_ovl_ai_idle = 1;
			ai_chat_set_idle( 1 );
			printf( "aichat: idle, scenery remarks paused\n" );
			fflush( stdout );
		} else if ( sw_ovl_ai_idle && now - sw_ovl_ai_act_tick < 2000 ) {
			sw_ovl_ai_idle = 0;
			ai_chat_set_idle( 0 );
			printf( "aichat: awake again\n" );
			fflush( stdout );
		}
	}
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
	sw_ovl_paper_fill( panel, ix );

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
				SDL_FillRect( panel, &r, SDL_MapRGBA( panel->format, 96, 82, 62, 220 ) );
			}
			y += 3 * SW_OVL_PARTY_STAT_PITCH;
		}
		/*	r260: the barrier is a battle-only figure - it exists while a shield
			skill is up and is gone by the time the fight ends.				*/
		if ( ix == 0 && sw_ovl_mode == SW_OVL_MODE_BATTLE && sw_ovl_ply_barrier[0] > 0 ) {
			char v[ 64 ];
			snprintf( v, sizeof( v ), "%d", sw_ovl_ply_barrier[0] );
			y += sw_ovl_ply_line( panel, "屏障", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
		}
	/*	r254: the player's always-checked numbers, under the bars on the
		left strip only.  The battle sheet keeps its strip for the enemy
		block, so this stays out of that mode.  Short labels ("金", "重")
		keep every row on one line at the 13px face.						*/
		if ( ix == 0 && sw_ovl_mode == SW_OVL_MODE_PARTY ) {
			char v[ 200 ];
			int first = 1, c;

			y += 4;
			v[ 0 ] = '\0';
			for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
				if ( sw_ovl_ply_cond[0][ c ] <= 0 ) continue;
				if ( !first ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
				strncat( v, sw_ovl_bat_cond[ c ].name, sizeof( v ) - 1 - strlen( v ) );
				first = 0;
			}
			if ( first ) snprintf( v, sizeof( v ), "正常" );
			y += sw_ovl_ply_line( panel, "状态", v, y, strip_w, !first ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d%%", sw_ovl_lim( sw_ovl_ply_hunger[0] / 100, 0, 100 ) );
			y += sw_ovl_ply_line( panel, "饱腹", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%dgp %dpp", sw_ovl_ply_gold[0], sw_ovl_ply_plat[0] );
			y += sw_ovl_ply_line( panel, "金", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			/*	r260: the clock and weather moved to the bottom of the strip, so
				the top block keeps only what changes with the character.	*/
			snprintf( v, sizeof( v ), "%d", sw_ovl_ply_karma );
			y += sw_ovl_ply_line( panel, "善恶", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d", sw_ovl_ply_fame );
			y += sw_ovl_ply_line( panel, "名声", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			y += sw_ovl_ply_line( panel, "信仰", sw_ovl_ply_god_name( sw_ovl_ply_god ),
				y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d", sw_ovl_ply_piety );
			y += sw_ovl_ply_line( panel, "虔诚", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d", sw_ovl_ply_sanity );
			y += sw_ovl_ply_line( panel, "狂气", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			/*	r260: the three weight figures live on the detail sheet - this
				strip cannot hold four more rows at 16px.					*/
			/*	Ether disease: the count plus the worst stage; red once it has
				taken hold, because every symptom is permanent.				*/
			if ( sw_ovl_ply_ether > 0 ) {
				snprintf( v, sizeof( v ), "%d 症状 %d级", sw_ovl_ply_ether,
					sw_ovl_ply_ether_worst );
				y += sw_ovl_ply_line( panel, "以太", v, y, strip_w, 1 ) * SW_OVL_PLY_LINE_H;
			} else {
				y += sw_ovl_ply_line( panel, "以太", "无", y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
			}

			/*	r260: the clock, the weather and the depth sit at the very bottom
				of the strip, where they are read at a glance while walking.	*/
			{
				int by = panel->h - SW_OVL_PLY_LINE_H * 2 - 4;
				if ( by < y ) by = y;
				if ( sw_ovl_ply_month > 0 ) {
					snprintf( v, sizeof( v ), "%d/%d/%d %s %s",
						sw_ovl_ply_year, sw_ovl_ply_month, sw_ovl_ply_day,
						sw_ovl_ply_time_name( sw_ovl_ply_hour / 4 ),
						sw_ovl_ply_weather_name( sw_ovl_ply_weather ) );
				} else {
					snprintf( v, sizeof( v ), "--/--/-- " );
				}
				by += sw_ovl_ply_line( panel, "", v, by, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

				if ( sw_ovl_ply_level > 0 ) {
					snprintf( v, sizeof( v ), "%d 层", sw_ovl_ply_level );
				} else {
					snprintf( v, sizeof( v ), "地表" );
				}
				by += sw_ovl_ply_line( panel, "", v, by, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
			}
		}
		/*	The follower's strip has the same empty half under its bars: the
			two relation scales - separate numbers, separate names - plus the
			figures a follower actually carries.  Ether is player-only, so it
			never appears here.  In battle the barrier and the enemy's share
			of HP join them; the enemy block on the player's strip is far down
			the screen by then.												*/
		if ( ix == 1 && sw_ovl_party_slot[ ix ] >= 0 && sw_ovl_mode == SW_OVL_MODE_PARTY ) {
			char v[ 200 ];
			int first = 1, c;

			y += 4;
			snprintf( v, sizeof( v ), "%d %s", sw_ovl_party_imp[ ix ],
				sw_ovl_imp_name( sw_ovl_party_imp[ ix ] ) );
			y += sw_ovl_ply_line( panel, "好感", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d %s", sw_ovl_party_ms[ ix ],
				sw_ovl_ms_name( sw_ovl_party_ms[ ix ] ) );
			y += sw_ovl_ply_line( panel, "顺从", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			v[ 0 ] = '\0';
			for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
				if ( sw_ovl_ply_cond[ ix ][ c ] <= 0 ) continue;
				if ( !first ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
				strncat( v, sw_ovl_bat_cond[ c ].name, sizeof( v ) - 1 - strlen( v ) );
				first = 0;
			}
			if ( first ) snprintf( v, sizeof( v ), "正常" );
			y += sw_ovl_ply_line( panel, "状态", v, y, strip_w, !first ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d%%", sw_ovl_lim( sw_ovl_ply_hunger[ ix ] / 100, 0, 100 ) );
			y += sw_ovl_ply_line( panel, "饱腹", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%dgp %dpp", sw_ovl_ply_gold[ ix ], sw_ovl_ply_plat[ ix ] );
			y += sw_ovl_ply_line( panel, "金", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
		}
		/*	Battle flavour: the follower's own fighting numbers and buffs,
			then the target it is actually on - what a player would otherwise
			have to press examine for.  The player's own target is drawn on
			the left strip, so the two never repeat each other.				*/
		if ( ix == 1 && sw_ovl_party_slot[ ix ] >= 0 && sw_ovl_mode == SW_OVL_MODE_BATTLE ) {
			SW_OVL_FOE *f = &sw_ovl_foe[ 1 ];
			char v[ 200 ];
			int i, c, first = 1;

			y += 4;
			snprintf( v, sizeof( v ), "%d", sw_ovl_pet_speed );
			y += sw_ovl_ply_line( panel, "速度", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "PV %d DV %d", sw_ovl_pet_pv, sw_ovl_pet_dv );
			y += sw_ovl_ply_line( panel, "防御", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			if ( sw_ovl_ply_barrier[ ix ] > 0 ) {
				snprintf( v, sizeof( v ), "%d", sw_ovl_ply_barrier[ ix ] );
				y += sw_ovl_ply_line( panel, "屏障", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
			}
			if ( sw_ovl_pet_nbuff > 0 ) {
				v[ 0 ] = '\0';
				for ( i = 0; i < sw_ovl_pet_nbuff; i++ ) {
					if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
					strncat( v, sw_ovl_buff_name( sw_ovl_pet_buffid[ i ] ),
						sizeof( v ) - 1 - strlen( v ) );
				}
				y += sw_ovl_ply_line( panel, "强化", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
			}
			if ( sw_ovl_pet_ndebuff > 0 ) {
				v[ 0 ] = '\0';
				for ( i = 0; i < sw_ovl_pet_ndebuff; i++ ) {
					if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
					strncat( v, sw_ovl_buff_name( sw_ovl_pet_debuffid[ i ] ),
						sizeof( v ) - 1 - strlen( v ) );
				}
				y += sw_ovl_ply_line( panel, "弱化", v, y, strip_w, 1 ) * SW_OVL_PLY_LINE_H;
			}

			{
				SDL_Rect hr;
				y += 6;
				hr.x = 8; hr.y = y - 3; hr.w = strip_w - 16; hr.h = 2;
				SDL_FillRect( panel, &hr, SDL_MapRGBA( panel->format, 110, 94, 70, 255 ) );
				sw_ovl_blit_small( panel, "— 随从目标 —", 110, 8, y ); y += 18;
			}
			if ( f->slot < 0 ) {
				sw_ovl_blit_small( panel, "无", 200, 8, y + 2 );
			} else {
				snprintf( v, sizeof( v ), "%s", sw_ovl_utf8( f->name ) );
				sw_ovl_blit_text( panel, v, 255, 8, y ); y += SW_OVL_LINE_H;

				snprintf( v, sizeof( v ), "Lv%d  HP %d%%", f->level,
					sw_ovl_lim( f->hp * 100 / ( f->mhp > 0 ? f->mhp : 1 ), 0, 100 ) );
				sw_ovl_blit_small( panel, v, 230, 8, y + 2 ); y += 18;

				snprintf( v, sizeof( v ), "速度 %d  PV %d  DV %d", f->speed, f->pv, f->dv );
				sw_ovl_blit_small( panel, v, 225, 8, y + 2 ); y += 18;

				v[ 0 ] = '\0';
				for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
					if ( f->cond[ c ] <= 0 ) continue;
					if ( !first ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
					strncat( v, sw_ovl_bat_cond[ c ].name, sizeof( v ) - 1 - strlen( v ) );
					first = 0;
				}
				if ( first ) snprintf( v, sizeof( v ), "状态正常" );
				sw_ovl_blit_small( panel, v, first ? 225 : 200, 8, y + 2 ); y += 18;

				if ( f->nbuff > 0 ) {
					v[ 0 ] = '\0';
					for ( i = 0; i < f->nbuff; i++ ) {
						if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
						strncat( v, sw_ovl_buff_name( f->buffid[ i ] ),
							sizeof( v ) - 1 - strlen( v ) );
					}
					sw_ovl_blit_small( panel, v, 44, 8, y + 2 ); y += 18;
				}
				if ( f->ndebuff > 0 ) {
					v[ 0 ] = '\0';
					for ( i = 0; i < f->ndebuff; i++ ) {
						if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
						strncat( v, sw_ovl_buff_name( f->debuffid[ i ] ),
							sizeof( v ) - 1 - strlen( v ) );
					}
					sw_ovl_blit_small( panel, v, 200, 8, y + 2 );
				}
			}
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
		/*	r255: on the detail sheet's right strip (pet, ix==1), under the abilities,
			add the pet's own base numbers: condition, hunger, weight, money.
			If there is no pet, the strip was empty already and it stays empty.		*/
		if ( sw_ovl_mode == SW_OVL_MODE_DETAIL && ix == 1 && sw_ovl_party_slot[ix] >= 0 ) {
			char v[ 200 ];
			int first = 1, c;

			y += 10;
			v[ 0 ] = '\0';
			for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
				if ( sw_ovl_ply_cond[ix][ c ] <= 0 ) continue;
				if ( !first ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
				strncat( v, sw_ovl_bat_cond[ c ].name, sizeof( v ) - 1 - strlen( v ) );
				first = 0;
			}
			if ( first ) snprintf( v, sizeof( v ), "正常" );
			y += sw_ovl_ply_line( panel, "状态", v, y, strip_w, !first ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%d%%", sw_ovl_lim( sw_ovl_ply_hunger[ix] / 100, 0, 100 ) );
			y += sw_ovl_ply_line( panel, "饱腹", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%dgp", sw_ovl_ply_gold[ix] );
			y += sw_ovl_ply_line( panel, "金", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			snprintf( v, sizeof( v ), "%dpp", sw_ovl_ply_plat[ix] );
			y += sw_ovl_ply_line( panel, "白金", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

			{
				char a[ 32 ], b[ 32 ];
				sw_ovl_ply_wfmt( sw_ovl_ply_weight[ix], a, sizeof( a ) );
				sw_ovl_ply_wfmt( sw_ovl_ply_carry[ix], b, sizeof( b ) );
				y += sw_ovl_ply_line( panel, "负重", a, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
				y += sw_ovl_ply_line( panel, "上限", b, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
			}
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

			/*	r260: the weight family belongs on this sheet - it is consulted
				now and then, not every step, and the party strip has no room
				for four more rows.  Weights use the game's cnvweight scale.	*/
			{
				char v[ 64 ], a[ 32 ], b[ 32 ];
				y += 6;
				sw_ovl_ply_wfmt( sw_ovl_ply_weight[0], a, sizeof( a ) );
				sw_ovl_ply_wfmt( sw_ovl_ply_carry[0], b, sizeof( b ) );
				snprintf( v, sizeof( v ), "%s/%s", a, b );
				y += sw_ovl_ply_line( panel, "负重", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

				sw_ovl_ply_wfmt( sw_ovl_ply_cargo, a, sizeof( a ) );
				sw_ovl_ply_wfmt( sw_ovl_ply_cargol, b, sizeof( b ) );
				snprintf( v, sizeof( v ), "%s/%s", a, b );
				y += sw_ovl_ply_line( panel, "行李", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

				sw_ovl_ply_wfmt( sw_ovl_ply_equipw[0], a, sizeof( a ) );
				y += sw_ovl_ply_line( panel, "装备", a, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
			}
	}

	if ( ( sw_ovl_mode == SW_OVL_MODE_PARTY || sw_ovl_mode == SW_OVL_MODE_BATTLE ) && ix == 1 ) {
		/*	r272: the whole lower half of the pet's strip is the answer box,
			and it shows the last three lines so a conversation reads as a
			conversation.  The oldest is pushed out by the newest.			*/
		static char box[ AI_HISTORY * 512 + 16 ];
		char one[ 512 ];
		int hi, ci;

		box[ 0 ] = '\0';
		for ( hi = ai_chat_history_count() - 1; hi >= 0; hi-- ) {
			const char *h = ai_chat_history( hi );
			if ( h == NULL || h[ 0 ] == '\0' ) continue;
			/*	A newline inside one answer would eat a row of the box and
				push the whole block up; each line is wrapped by width here
				instead, so three answers take six or seven rows.		*/
			snprintf( one, sizeof( one ), "%s", h );
			for ( ci = 0; one[ ci ]; ci++ )
				if ( one[ ci ] == '\n' || one[ ci ] == '\r' ) one[ ci ] = ' ';
			if ( one[ 0 ] == '\0' ) continue;
			if ( box[ 0 ] )
				strncat( box, "\n", sizeof( box ) - 1 - strlen( box ) );
			strncat( box, one, sizeof( box ) - 1 - strlen( box ) );
		}
		if ( box[ 0 ] ) {
			if ( sw_ovl_mode == SW_OVL_MODE_BATTLE ) {
				/*	The battle strip is already full; keep its answers at
					the bottom where they have always been.			*/
				sw_ovl_chat_lines( panel, box, 8, panel->h - 8, SW_OVL_AI_ROWS, 1 );
			} else {
				/*	r284: the answer box starts right under the rows above
					(the money line), not at the bottom of the strip, so the
					two blocks read as one list.  The room left decides how
					many rows fit.										*/
				int maxl = ( panel->h - y - 12 ) / 18;
				if ( maxl < 1 ) maxl = 1;
				if ( maxl > SW_OVL_AI_ROWS ) maxl = SW_OVL_AI_ROWS;
				sw_ovl_chat_lines( panel, box, 8, y + 6, maxl, 0 );
			}
		}
	}

	/*	r247: the battle sheet hangs the current enemy off the bottom of the
	player's strip - the party rows above stay exactly as the party layer
	draws them, so both portraits are still up.  r277 adds what is actually
	worth checking mid-fight: level, speed, PV/DV, conditions and buffs.	*/
	if ( ix == 0 && sw_ovl_mode == SW_OVL_MODE_BATTLE ) {
		SW_OVL_FOE *f = &sw_ovl_foe[ 0 ];
		SDL_Rect hr;
		char v[ 160 ];
		int c, first = 1;

		/*	The player's own fighting numbers first, then the target.		*/
		y += 4;
		snprintf( v, sizeof( v ), "%d", sw_ovl_ply_speed );
		y += sw_ovl_ply_line( panel, "速度", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

		snprintf( v, sizeof( v ), "PV %d DV %d", sw_ovl_ply_pv, sw_ovl_ply_dv );
		y += sw_ovl_ply_line( panel, "防御", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;

		if ( sw_ovl_ply_nbuff > 0 ) {
			int i;
			v[ 0 ] = '\0';
			for ( i = 0; i < sw_ovl_ply_nbuff; i++ ) {
				if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
				strncat( v, sw_ovl_buff_name( sw_ovl_ply_buffid[ i ] ),
					sizeof( v ) - 1 - strlen( v ) );
			}
			y += sw_ovl_ply_line( panel, "强化", v, y, strip_w, 0 ) * SW_OVL_PLY_LINE_H;
		}
		if ( sw_ovl_ply_ndebuff > 0 ) {
			int i;
			v[ 0 ] = '\0';
			for ( i = 0; i < sw_ovl_ply_ndebuff; i++ ) {
				if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
				strncat( v, sw_ovl_buff_name( sw_ovl_ply_debuffid[ i ] ),
					sizeof( v ) - 1 - strlen( v ) );
			}
			y += sw_ovl_ply_line( panel, "弱化", v, y, strip_w, 1 ) * SW_OVL_PLY_LINE_H;
		}

		y += 10;
		hr.x = 8; hr.y = y - 4; hr.w = strip_w - 16; hr.h = 2;
		SDL_FillRect( panel, &hr, SDL_MapRGBA( panel->format, 110, 94, 70, 255 ) );
		sw_ovl_blit_text( panel, "— 战斗 —", 235, 8, y ); y += SW_OVL_LINE_H;
		if ( f->slot < 0 ) {
			sw_ovl_blit_text( panel, "无目标", 200, 8, y );
		} else {
			snprintf( buf, sizeof( buf ), "%s Lv%d", sw_ovl_utf8( f->name ), f->level );
			sw_ovl_blit_text( panel, buf, 255, 8, y ); y += SW_OVL_LINE_H;

			snprintf( buf, sizeof( buf ), "HP %d/%d (%d%%)", f->hp,
				f->mhp > 0 ? f->mhp : 1,
				sw_ovl_lim( f->hp * 100 / ( f->mhp > 0 ? f->mhp : 1 ), 0, 100 ) );
			sw_ovl_blit_text( panel, buf, 230, 8, y ); y += SW_OVL_LINE_H;

			snprintf( v, sizeof( v ), "速度 %d  PV %d  DV %d", f->speed, f->pv, f->dv );
			sw_ovl_blit_small( panel, v, 225, 8, y + 2 ); y += 18;

			v[ 0 ] = '\0';
			for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
				if ( f->cond[ c ] <= 0 ) continue;
				if ( !first ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
				strncat( v, sw_ovl_bat_cond[ c ].name, sizeof( v ) - 1 - strlen( v ) );
				first = 0;
			}
			if ( first ) snprintf( v, sizeof( v ), "状态正常" );
			sw_ovl_blit_small( panel, v, first ? 225 : 200, 8, y + 2 ); y += 18;

			if ( f->nbuff > 0 ) {
				int i;
				v[ 0 ] = '\0';
				for ( i = 0; i < f->nbuff; i++ ) {
					if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
					strncat( v, sw_ovl_buff_name( f->buffid[ i ] ), sizeof( v ) - 1 - strlen( v ) );
				}
				sw_ovl_blit_small( panel, v, 44, 8, y + 2 ); y += 18;
			}
			if ( f->ndebuff > 0 ) {
				int i;
				v[ 0 ] = '\0';
				for ( i = 0; i < f->ndebuff; i++ ) {
					if ( v[ 0 ] ) strncat( v, " ", sizeof( v ) - 1 - strlen( v ) );
					strncat( v, sw_ovl_buff_name( f->debuffid[ i ] ), sizeof( v ) - 1 - strlen( v ) );
				}
				sw_ovl_blit_small( panel, v, 200, 8, y + 2 );
			}
		}
	}

	sw_ovl_party_sheet[ ix ] = sw_ovl_upload_rgba( panel, &sw_ovl_party_sheet_w[ ix ], &sw_ovl_party_sheet_h[ ix ] );
	SDL_FreeSurface( panel );
	return sw_ovl_party_sheet[ ix ] ? 0 : -1;
}

static int sw_ovl_party_refresh( int *slot )
{
	int dirty = 0, k, i, c;

/*	Cache a freshly read value, marking the sheets dirty when it moved.	*/
#define SW_OVL_PLY_SYNC( dst, src ) \
	do { int nv_ = ( src ); if ( ( dst ) != nv_ ) { dirty = 1; ( dst ) = nv_; } } while ( 0 )

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
			for ( i = 0; i < SW_OVL_BAT_COND_N; i++ ) sw_ovl_ply_cond[k][i] = 0;
			sw_ovl_ply_hunger[k] = sw_ovl_ply_gold[k] = sw_ovl_ply_plat[k] = 0;
			sw_ovl_ply_weight[k] = sw_ovl_ply_carry[k] = 0;
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
		/*	r255: hunger, burden, money and the condition flags exist for any
			character, so the player and the pet each keep a set - the pet's
			goes on the detail sheet's right strip.						*/
		for ( c = 0; c < SW_OVL_BAT_COND_N; c++ ) {
			int cv = sw_ovl_cdata( sw_ovl_bat_cond[ c ].field, s );
			if ( cv != sw_ovl_ply_cond[k][c] ) { dirty = 1; sw_ovl_ply_cond[k][c] = cv; }
		}
		SW_OVL_PLY_SYNC( sw_ovl_ply_hunger[k], sw_ovl_cdata( SW_OVL_PLY_CD_HUNGER, s ) );
		SW_OVL_PLY_SYNC( sw_ovl_ply_gold[k],   sw_ovl_cdata( SW_OVL_PLY_CD_GOLD, s ) );
		SW_OVL_PLY_SYNC( sw_ovl_ply_plat[k],   sw_ovl_cdata( SW_OVL_PLY_CD_PLATINUM, s ) );
		SW_OVL_PLY_SYNC( sw_ovl_ply_weight[k], sw_ovl_cdata( SW_OVL_PLY_CD_WEIGHT, s ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_carry[k],  sw_ovl_cdata( SW_OVL_PLY_CD_CARRY, s ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_barrier[k], sw_ovl_cdata( SW_OVL_PLY_CD_BARRIER, s ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_equipw[k], sw_ovl_cdata( SW_OVL_PLY_CD_EQUIPW, s ) );
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

			/*	The rest of the left strip is player-only: faith, karma and
				the clock do not apply to a pet.							*/
			SW_OVL_PLY_SYNC( sw_ovl_ply_fame,   sw_ovl_cdata( SW_OVL_PLY_CD_FAME, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_karma,  sw_ovl_cdata( SW_OVL_PLY_CD_KARMA, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_god,    sw_ovl_cdata( SW_OVL_PLY_CD_GOD, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_piety,  sw_ovl_cdata( SW_OVL_PLY_CD_PIETY, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_sanity, sw_ovl_cdata( SW_OVL_PLY_CD_SANITY, 0 ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_cargo,  sw_ovl_gdata( SW_OVL_GD_CARGO ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_cargol, sw_ovl_gdata( SW_OVL_GD_CARGO_LIMIT ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_level,  sw_ovl_gdata( SW_OVL_GD_LEVEL ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_area,   sw_ovl_gdata( SW_OVL_GD_AREA ) );
			{
				int worst = 0, n = sw_ovl_ether_scan( &worst );
				if ( n != sw_ovl_ply_ether || worst != sw_ovl_ply_ether_worst ) {
					dirty = 1;
					sw_ovl_ply_ether = n;
					sw_ovl_ply_ether_worst = worst;
				}
			}
			SW_OVL_PLY_SYNC( sw_ovl_ply_year,   sw_ovl_gdata( SW_OVL_GD_YEAR ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_month,  sw_ovl_gdata( SW_OVL_GD_MONTH ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_day,    sw_ovl_gdata( SW_OVL_GD_DAY ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_hour,   sw_ovl_gdata( SW_OVL_GD_HOUR ) );
			SW_OVL_PLY_SYNC( sw_ovl_ply_weather, sw_ovl_gdata( SW_OVL_GD_WEATHER ) );
		}

			/*	r272: affection (cdata 17) and the master-servant axis
				(cdata 169) are separate scales with separate names; the sum
				is only what the name row shows, as before.				*/
			{ int im = sw_ovl_cdata( 17, s ), ms = sw_ovl_cdata( 169, s );
			  if ( im != sw_ovl_party_imp[k] ) { dirty = 1; sw_ovl_party_imp[k] = im; }
			  if ( ms != sw_ovl_party_ms[k] ) { dirty = 1; sw_ovl_party_ms[k] = ms; }
			  if ( im + ms != sw_ovl_party_rel[k] ) { dirty = 1; sw_ovl_party_rel[k] = im + ms; } }
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

	/*	r277: both strips carry their owner's own target this time - the
		player's on the left, the follower's on the right.  The pet's own
		speed/PV/DV ride above them.  One hash covers the lot.			*/
	if ( sw_ovl_mode == SW_OVL_MODE_BATTLE ) {
		int ps = sw_ovl_cdata( SW_OVL_CD_TARGET, 0 );
		int qs = slot[1] >= 0 ? sw_ovl_cdata( SW_OVL_CD_TARGET, slot[1] ) : -1;
		int hash;

		sw_ovl_foe_read( 0, ps > 0 ? ps : sw_ovl_battle_slot );
		/*	A follower's TARGET only exists while it is actually swinging
			(action.hsp sets it for the blow and clears it again), so the
			engaged enemy is the useful fallback - otherwise this block sits
			at "无" through the whole fight.								*/
		sw_ovl_foe_read( 1, qs > 0 ? qs : sw_ovl_battle_slot );
		sw_ovl_pet_speed = slot[1] >= 0 ? sw_ovl_cdata( SW_OVL_CD_SPEED, slot[1] ) : -1;
		sw_ovl_pet_pv    = slot[1] >= 0 ? sw_ovl_cdata( SW_OVL_CD_PV, slot[1] ) : -1;
		sw_ovl_pet_dv    = slot[1] >= 0 ? sw_ovl_cdata( SW_OVL_CD_DV, slot[1] ) : -1;
		sw_ovl_buff_scan( slot[1], sw_ovl_pet_buffid, &sw_ovl_pet_nbuff,
			sw_ovl_pet_debuffid, &sw_ovl_pet_ndebuff );
		sw_ovl_ply_speed = sw_ovl_cdata( SW_OVL_CD_SPEED, 0 );
		sw_ovl_ply_pv    = sw_ovl_cdata( SW_OVL_CD_PV, 0 );
		sw_ovl_ply_dv    = sw_ovl_cdata( SW_OVL_CD_DV, 0 );
		sw_ovl_buff_scan( 0, sw_ovl_ply_buffid, &sw_ovl_ply_nbuff,
			sw_ovl_ply_debuffid, &sw_ovl_ply_ndebuff );
		hash = (int)sw_ovl_foe_hash();
		if ( hash != sw_ovl_foe_sig ) { dirty = 1; sw_ovl_foe_sig = hash; }
	}
#undef SW_OVL_PLY_SYNC
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
	/*	r257: the sheets need the slots as well - the pet's block on the detail
		strip is skipped while slot[1] is -1, and that cache was never filled. */
	sw_ovl_party_slot[0] = slot[0];
	sw_ovl_party_slot[1] = slot[1];

	/*	r292: greet once when the sheet first sees a follower, and again
		whenever she leaves the party and comes back (revive, re-summon, or
		loading another save).  Queued here because this is where the party
		is known; the tick below sends it on the next free worker.  The init
		call must come first: notify_event drops everything while the module
		is still asleep, and this is the first event of a run.				*/
	ai_chat_init();
	{
		int have = ( slot[1] >= 0 && sw_ovl_party_name[1][0] != '\0' );
		if ( have ) {
			char nm[ 64 ];
			snprintf( nm, sizeof( nm ), "%s", sw_ovl_utf8( sw_ovl_party_name[1] ) );
			if ( strcmp( nm, sw_ovl_ai_w_greet_name ) != 0 ) {
				int first = ( sw_ovl_ai_w_greet_name[ 0 ] == '\0' );
				snprintf( sw_ovl_ai_w_greet_name, sizeof( sw_ovl_ai_w_greet_name ),
					"%s", nm );
				ai_chat_notify_event( first ? "主人刚刚回到游戏，你就跟在旁边"
					: "主人又把你叫回队伍里了", first ? 4 : 5 );
			} else if ( !sw_ovl_ai_w_pet_seen ) {
				ai_chat_notify_event( "主人又把你叫回队伍里了", 5 );
			}
		}
		sw_ovl_ai_w_pet_seen = have;
	}

	/*	r261: feed the chatter worker and redraw when its line moves.  Without
		a follower there is nobody to speak, so the module stays untouched.	*/
	if ( slot[1] >= 0 ) {
		char scene[ 1024 ], npc[ 64 ], where[ 48 ];
		const char *ai_line;

		ai_chat_init();
		snprintf( npc, sizeof( npc ), "%s", sw_ovl_utf8( sw_ovl_party_name[1] ) );
		/*	r285: the model gets a real place name instead of "地下第 N 层" -
			"韦尔尼斯" or "莱希墨苏 第 3 层".  Unknown area ids fall back to
			the old wording, so nothing regresses on a modded map.		*/
		{
			const char *an = sw_ovl_area_name( sw_ovl_ply_area );

			if ( an[ 0 ] && sw_ovl_area_is_dungeon( sw_ovl_ply_area ) && sw_ovl_ply_level > 0 )
				snprintf( where, sizeof( where ), "%s 第 %d 层", an, sw_ovl_ply_level );
			else if ( an[ 0 ] )
				snprintf( where, sizeof( where ), "%s", an );
			else if ( sw_ovl_ply_level > 0 )
				snprintf( where, sizeof( where ), "地下第 %d 层", sw_ovl_ply_level );
			else
				snprintf( where, sizeof( where ), "地表" );
		}
		sw_ovl_ai_scene( scene, sizeof( scene ), slot[1] >= 0 ? 1 : 0, where );
		ai_chat_set_context( npc, scene );

		/*	Events: each one is a plain sentence for the model to word. */
		if ( sw_ovl_ai_last_battle && !sw_ovl_battle_on )
			ai_chat_notify_event( "刚刚的战斗结束了，你们赢了", 2 );
		sw_ovl_ai_last_battle = sw_ovl_battle_on;

		if ( sw_ovl_ply_level != sw_ovl_ai_last_level && sw_ovl_ai_last_level >= 0 )
			ai_chat_notify_event( sw_ovl_ply_level > 0 ? "你们来到了新的楼层"
				: "你们回到了地面", 1 );
		sw_ovl_ai_last_level = sw_ovl_ply_level;

		if ( sw_ovl_party_mhp[0] > 0 && sw_ovl_party_hp[0] * 100 < sw_ovl_party_mhp[0] * 30 ) {
			if ( !sw_ovl_ai_was_hurt )
				ai_chat_notify_event( "主人伤得很重，血流不止", 3 );
			sw_ovl_ai_was_hurt = 1;
		} else sw_ovl_ai_was_hurt = 0;

		if ( sw_ovl_ply_hunger[0] / 100 < 20 ) {
			if ( !sw_ovl_ai_was_hungry ) ai_chat_notify_event( "主人已经很饿了", 2 );
			sw_ovl_ai_was_hungry = 1;
		} else sw_ovl_ai_was_hungry = 0;

		ai_chat_tick();

		/*	r262: whatever moved in the tracked numbers or in the game log is
			handed to the model as a plain sentence.  This is what makes her
			react to the fight, the loot and the looters instead of waiting
			for the idle timer.												*/
		sw_ovl_ai_watch();

		ai_line = ai_chat_text();
		if ( ai_chat_seq() != sw_ovl_ai_seq_seen ) {
			sw_ovl_ai_seq_seen = ai_chat_seq();
			snprintf( sw_ovl_ai_shown, sizeof( sw_ovl_ai_shown ), "%s",
				ai_line != NULL ? ai_line : "" );
			dirty = 1;
		}
	}

	/*	r258: the sheet runs the full window height - the old -16 left a bare
		black band under the right strip whenever its rows were short.		*/
	sheet_h = win_h;
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
