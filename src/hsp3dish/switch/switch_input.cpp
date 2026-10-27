//
//	src/hsp3dish/switch/switch_input.cpp
//	Switch gamepad -> HSP input bridge (T2.3)
//
//	WHY A BRIDGE AT ALL
//	-------------------
//	The Switch port reuses src/hsp3dish/linux/hsp3dish.cpp unmodified as its
//	platform glue.  That file already contains the whole event loop: it keeps a
//	`keys[]` table indexed by SDL scancode out of SDL_PollEvent, exposes it as
//	get_key_state() (used by hgio_stick / hgio_getkey in hgiox.cpp), and turns
//	key-down events into HSP object notices.  What it has no notion of is a
//	gamepad - on Linux a Joy-Con simply does not exist.
//
//	So instead of forking the glue, this file *translates* the gamepad into the
//	keyboard events that glue already understands, and leaves the rest alone:
//
//	  * the pad is polled once per rendered frame;
//	  * a change in any mapped button is pushed into the SDL event queue as a
//	    synthetic SDL_KEYDOWN/SDL_KEYUP with the scancode the Linux glue would
//	    have seen for that key;
//	  * the glue's own handleEvent() then consumes it exactly like a real
//	    keypress, so `stick`, `getkey` and `onkey` all keep working with no
//	    further changes and no new HSP key codes.
//
//	WHERE THE PER-FRAME TICK COMES FROM (this is the part that bit us)
//	-----------------------------------------------------------------
//	The obvious hook is ctx->msgfunc, which linux/hsp3dish.cpp installs during
//	hsp3dish_init_sub() and which this file can still wrap from the one place
//	the reused glue hands over the HSPCTX (hsp3typeinit_sock_extcmd).
//
//	That hook does not work.  msgfunc is not a per-frame callback: it is
//	entered once and then *loops internally* while the script waits
//	(hsp3dish_msgfunc's own `while(1)` drives RUNMODE_WAIT/RUNMODE_AWAIT and
//	calls handleEvent() from inside that loop).  Wrapping it therefore sampled
//	the pad exactly once per program run.  On hardware that looked like a
//	totally dead pad: the hook was verifiably installed, the controller was
//	open and correctly mapped, and yet no key value ever moved, because the
//	state was read once at startup and never again.
//
//	The frame tick that does work is hgio_render_start() in this port's
//	graphics backend, entered once per frame.  Through gles1_shim.cpp's
//	sw_frame_tick() it calls switch_input_poll() there.  (It first lived in
//	sw_glClear(), which stopped being a per-frame hook once the frame clear
//	moved into the main screen's own framebuffer.)
//
//	Elona's full key set is P4 work (PLAN.md R5); this is the minimal mapping
//	the P2 gate needs (official sample scripts driving stick/getkey).
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#if defined(__SWITCH__)
#include <switch.h>
#endif
#include <EGL/egl.h>
#include "switch_input.h"
#include "switch_overlay.h"
#include "ai_chat.h"
/*	Switch diagnostics.  0 = shipping build.						*/
#ifndef SWITCH_DIAG
#define SWITCH_DIAG 0
#endif
/*----------------------------------------------------------------*/
/*	Mapping														  */
/*----------------------------------------------------------------*/
typedef struct {
	SDL_GameControllerButton	btn;	/* or SW_NO_BUTTON					*/
	SDL_GameControllerAxis		axis;	/* SW_NO_AXIS when btn is used		*/
	SDL_Scancode				sc;		/* the base layer					*/
	SDL_Scancode				alt;	/* the R layer (UNKNOWN = nothing)	*/
	SDL_Scancode				zr;		/* the ZR layer (UNKNOWN = nothing)	*/
} SW_KEYMAP_ENTRY;
/*	SDL's controller API has no ZL/ZR buttons; on the Switch those two arrive
	as trigger axes, so the map carries an optional analog source as well.	*/
#define SW_NO_BUTTON	((SDL_GameControllerButton)-1)
#define SW_NO_AXIS		((SDL_GameControllerAxis)-1)
#define SW_TRIGGER_ON	8000
/*	ONLY THE KEYS ELONA READS AS A SCANCODE ARE LISTED HERE: the four
	directions (movement, and the lock-on cursor on the right stick), the two
	values key_check() tests with `stick p,15` - Escape (128) and Tab (1024) -
	and Return, which a few routines read with `getkey p,13` instead of going
	through key_check().  Everything Elona has to receive as a *letter* is
	typed as a character instead by switch_input_take_keys() below, because
	key_check() (elona232src/init.hsp:7325) takes letters from its hidden
	keylog box and never from a scancode.								*/
static const SW_KEYMAP_ENTRY sw_keymap[] = {
	{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,		SW_NO_AXIS,		SDL_SCANCODE_LEFT },
	{ SDL_CONTROLLER_BUTTON_DPAD_UP,		SW_NO_AXIS,		SDL_SCANCODE_UP },
	{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,		SW_NO_AXIS,		SDL_SCANCODE_RIGHT },
	{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,		SW_NO_AXIS,		SDL_SCANCODE_DOWN },
	/*	SDL names the face buttons the Xbox way - its logical A is the bottom
		button and its logical B the right-hand one - while Nintendo labels
		the Switch the other way round, so the entries below are named by
		POSITION: SDL B is the right-hand button (Nintendo's A), SDL A the
		bottom one (Nintendo's B), SDL Y the top one (Nintendo's X) and
		SDL X the left one (Nintendo's Y) - SDL's X/Y stand for the positions
		Xbox's X/Y sit in, which on a Switch pad are Nintendo's Y/X.  Verified
		on Eden 2026-10-06: its switch-X key arrives as SDL's Y, its switch-Y
		as SDL's X.  That is what this port's own startup line prints:
		a:b1,b:b0,x:b3,y:b2.
		Return stays here even though the pad's A also *types* a CR: the two
		serve different readers.  key_check() wants the character, but a few
		routines take Enter straight from the key with `getkey p,13` - the
		item marker in command.hsp, the help viewer - and those need the
		scancode.  Only Nintendo's B has a scancode key_check() reads, and
		only while it is not on the second layer; on that layer the bottom
		button drinks instead.  X and Y are typed, not scancoded.			*/
	{ SDL_CONTROLLER_BUTTON_B,				SW_NO_AXIS,		SDL_SCANCODE_RETURN },	/* confirm: pad A, right	*/
	{ SDL_CONTROLLER_BUTTON_A,				SW_NO_AXIS,		SDL_SCANCODE_ESCAPE },	/* cancel:  pad B, bottom	*/
	/*	L carries one scancode per layer - the three Elona reads that need a
		real key.  Tab on the base layer, Space (wait) while R is held, and
		BackSpace (delete a save; system.hsp's *game_title_selectID reads
		`getkey a, 8`) on the ZR layer.  The stick clicks were these keys'
		only other home, and the player asked for those to be dropped.		*/
	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,	SW_NO_AXIS,		SDL_SCANCODE_TAB,	SDL_SCANCODE_SPACE,	SDL_SCANCODE_BACKSPACE },
	/*	The stick clicks no longer inject a scancode.						*/
	{ SDL_CONTROLLER_BUTTON_RIGHTSTICK,		SW_NO_AXIS,		SDL_SCANCODE_UNKNOWN },
	{ SDL_CONTROLLER_BUTTON_LEFTSTICK,		SW_NO_AXIS,		SDL_SCANCODE_UNKNOWN },
};
#define SW_KEYMAP_N	((int)( sizeof( sw_keymap ) / sizeof( sw_keymap[0] ) ))
/*	Both sticks type characters - a stick has no scancode Elona reads, so
		each direction is a key press on the character channel instead (see
		switch_input_take_keys()).  The d-pad is the only thing that moves the
		cursor keys, and it does so through the scancode table above.			*/
#define SW_STICK_DEADZONE	12000
static SDL_GameController	*sw_pad;
static Uint8				sw_state[SW_KEYMAP_N];
static SDL_Scancode			sw_inject_sc[SW_KEYMAP_N];	/* scancode that entry owns now */
static int					sw_installed;
#define SW_KEY_STATE_MAX	512
static Uint8				sw_keys[SW_KEY_STATE_MAX];
static int					sw_cg_swallow = 0;	/* r379: confirm press closes the picture */
static unsigned int			sw_poll_no;
static unsigned int			sw_push_no;
/*----------------------------------------------------------------*/
/*	Polling														  */
/*----------------------------------------------------------------*/
/*	r248: ZL / ZR delivery.  SDL's own Switch layout binds both to *buttons*
	- lefttrigger:b8, righttrigger:b9 (the string the pad prints at startup) -
	and its game-controller layer promotes those to the trigger axes only when
	the pad feeds that button.  The console does, so the axis read worked
	there; an emulator that leaves ZL/ZR as plain buttons does not, and the
	axis stays at zero however hard the key is held.  The raw button each
	trigger maps to is taken from the pad's own mapping string at startup, and
	a trigger counts as held when either the axis reads high or that raw
	button is down - so the three layers behave the same on both.			*/
static int sw_zl_raw = -1;			/* raw joystick button, or -1	*/
static int sw_zr_raw = -1;
static int sw_ai_key_held;			/* ZR+ZL already opened the keyboard */
static unsigned int sw_activity_tick;	/* last pad movement, SDL ticks */
/*	"righttrigger:b9" -> 9; "righttrigger:a5" -> -1 (that pad already hands
	the trigger over as an axis, so no raw button is needed).			*/
static int sw_map_trigger_button( const char *map, const char *name )
{
	const char *p = map;
	size_t nl;
	if ( map == NULL ) return -1;
	nl = strlen( name );
	while ( ( p = strstr( p, name ) ) != NULL ) {
		if ( p[ nl ] == ':' && p[ nl + 1 ] == 'b' ) return atoi( p + nl + 2 );
		p += nl;
	}
	return -1;
}
static int sw_trigger_held( SDL_GameControllerAxis axis, int raw )
{
	SDL_Joystick *js;
	if ( sw_pad == NULL ) return 0;
	if ( SDL_GameControllerGetAxis( sw_pad, axis ) > SW_TRIGGER_ON ) return 1;
	if ( raw >= 0 ) {
		js = SDL_GameControllerGetJoystick( sw_pad );
		if ( js != NULL && SDL_JoystickGetButton( js, raw ) ) return 1;
	}
	return 0;
}
static int sw_zr_held( void )
{
	return sw_trigger_held( SDL_CONTROLLER_AXIS_TRIGGERRIGHT, sw_zr_raw );
}
/*	The pad has three layers: 0 = base, 1 = R held (second), 2 = ZR held
	(third).  ZR wins while both are down, so a slip never lands on R's map.
	sw_layer_second() stays as the "not on the base layer" test that the
	scancode table and the overlay already ask for.							*/
static int sw_layer( void )
{
	if ( sw_pad == NULL ) return 0;
	if ( sw_zr_held() ) return 2;
	if ( SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ) ) return 1;
	return 0;
}
static int sw_layer_second( void )
{
	return sw_layer() != 0;
}
/*	The scancode this entry injects on the layer that is active right now, or
	SDL_SCANCODE_UNKNOWN when it injects nothing there.  L hands out one
	scancode per layer (Tab / Space / BackSpace); every other entry keeps its
	base key and goes quiet on the upper layers, because each of those keys
	types a character there - and a matching scancode arriving at the same
	moment is a *second* key.  key_check() reads its keylog box first and
	lets a non-zero `stick p,15` overwrite the result, so Escape or a cursor
	key would throw the letter away (the R48/R49 lesson).				*/
static SDL_Scancode sw_entry_sc( int i )
{
	const SW_KEYMAP_ENTRY *e = &sw_keymap[i];
	int layer = sw_layer();
	SDL_Scancode sc = ( layer == 2 ) ? e->zr : ( ( layer == 1 ) ? e->alt : e->sc );
	if ( sc == SDL_SCANCODE_UNKNOWN ) return sc;
	if ( layer != 0 && ( sc == SDL_SCANCODE_ESCAPE ||
		 sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT ||
		 sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN ) ) return SDL_SCANCODE_UNKNOWN;
	return sc;
}
/*	True while that entry's physical key is held.						*/
static int sw_entry_pressed( int i )
{
	const SW_KEYMAP_ENTRY *e = &sw_keymap[i];
	if ( e->btn != SW_NO_BUTTON && SDL_GameControllerGetButton( sw_pad, e->btn ) ) return 1;
	if ( e->axis != SW_NO_AXIS && SDL_GameControllerGetAxis( sw_pad, e->axis ) > SW_TRIGGER_ON ) return 1;
	return 0;
}
static int sw_inject_release = -1;
static void sw_push_key( SDL_Scancode sc, int down )
{
	SDL_Event ev;
	memset( &ev, 0, sizeof( ev ) );
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.type = ev.type;
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat = 0;
	ev.key.keysym.scancode = sc;
	ev.key.keysym.sym = SDL_GetKeyFromScancode( sc );
	ev.key.keysym.mod = KMOD_NONE;
	SDL_PushEvent( &ev );
}
/*	r416: the PC keyboard, for Eden.
	Elona on a PC reads its commands as *characters* out of the hidden keylog
	box - the letters, space and punctuation config.txt names in its (keyboard)
	section - and its movement as direction bits out of stick(), which is why
	the keypad's 8/2/4/6/7/9/1/3 are translated into the arrow scancodes it
	stands for instead of being typed as digits.  The emulator hands the
	keyboard over to SDL, but the reused event loop only fills the keylog from
	SDL_TEXTINPUT, which a plain key press never produces, so those characters
	are collected here and handed over by switch_input_take_keys().  Hardware
	has no keyboard: the SDL state stays all-zero and none of this ever runs. */
/*	r426: whether the game is waiting for text in one of its prompt boxes.
	Digits are movement keys in the field and typed characters in a box, and
	typing them in the field would only make the game answer "not assigned"
	and hold the press until it is released - the direction is lost.  The
	answer lives in hsp3gr_dish_switch.cpp, next to the objsel bookkeeping.	*/
extern int switch_input_prompt_active( void );
#define SW_PC_BUF_MAX	32
static char		sw_pc_buf[ SW_PC_BUF_MAX ];
static int		sw_pc_head, sw_pc_tail;
static Uint8	sw_pc_last[ SDL_NUM_SCANCODES ];
static Uint8	sw_pc_keys[ SW_KEY_STATE_MAX ];	/* the scancode channel		*/
static void sw_pc_push( char c )
{
	int next = ( sw_pc_tail + 1 ) % SW_PC_BUF_MAX;
	if ( next == sw_pc_head ) return;			/* full: drop the newest		*/
	sw_pc_buf[ sw_pc_tail ] = c;
	sw_pc_tail = next;
}
/*	The character one press types on a US layout, 0 for a key Elona has no
	use for.  Shift selects the symbol row; Caps Lock applies to letters, as
	it does on the PC.													*/
static char sw_pc_char( SDL_Scancode sc, int shift, int caps )
{
	static const char digits[] = "123456789";
	static const char shifted[] = "!@#$%^&*(";
	char c = 0;
	int letter = 0;
	if ( sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z ) {
		c = (char)( 'a' + ( sc - SDL_SCANCODE_A ) );
		letter = 1;
	} else if ( sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9 ) {
		c = shift ? shifted[ sc - SDL_SCANCODE_1 ] : digits[ sc - SDL_SCANCODE_1 ];
	} else if ( sc == SDL_SCANCODE_0 ) {
		c = shift ? ')' : '0';
	} else if ( sc == SDL_SCANCODE_SPACE ) {
		c = ' ';				/* key_enter							*/
	} else if ( sc == SDL_SCANCODE_MINUS ) {
		c = shift ? '_' : '-';	/* key_pagedown						*/
	} else if ( sc == SDL_SCANCODE_EQUALS ) {
		/*	Eden's keyboard has no keypad scancodes - it reports the
			keypad's + as '=' - and config.txt pages lists with '+', which is
			not a key Elona binds any other way, so '=' types the plus.		*/
		c = '+';
	} else if ( sc == SDL_SCANCODE_LEFTBRACKET ) {
		c = shift ? '{' : '[';	/* key_alter							*/
	} else if ( sc == SDL_SCANCODE_RIGHTBRACKET ) {
		c = shift ? '}' : ']';
	} else if ( sc == SDL_SCANCODE_BACKSLASH ) {
		c = shift ? '|' : '\\';	/* key_cancel							*/
	} else if ( sc == SDL_SCANCODE_SEMICOLON ) {
		c = shift ? ':' : ';';
	} else if ( sc == SDL_SCANCODE_APOSTROPHE ) {
		c = shift ? '"' : '\'';
	} else if ( sc == SDL_SCANCODE_COMMA ) {
		c = shift ? '<' : ',';	/* key_goUp							*/
	} else if ( sc == SDL_SCANCODE_PERIOD ) {
		c = shift ? '>' : '.';	/* key_goDown						*/
	} else if ( sc == SDL_SCANCODE_SLASH ) {
		c = shift ? '?' : '/';	/* key_help, key_msglog			*/
	} else if ( sc == SDL_SCANCODE_GRAVE ) {
		c = shift ? '~' : '`';
	} else if ( sc == SDL_SCANCODE_KP_DIVIDE ) {
		c = '/';
	} else if ( sc == SDL_SCANCODE_KP_MULTIPLY ) {
		c = '*';				/* key_target						*/
	} else if ( sc == SDL_SCANCODE_KP_MINUS ) {
		c = '-';
	} else if ( sc == SDL_SCANCODE_KP_PLUS ) {
		c = '+';				/* key_pageup						*/
	} else if ( sc == SDL_SCANCODE_KP_PERIOD ) {
		c = '.';
	}
	if ( letter && ( shift != 0 ) != ( caps != 0 ) )
		c = (char)( c - 'a' + 'A' );
	return c;
}
/*	The four movement directions as the keyboard spells them: the arrow keys
	and, exactly as config.txt's key_north..key_southeast do, the keypad -
	8/2/4/6 straight and 7/9/1/3 diagonal.									*/
static int sw_pc_dir( const Uint8 *ks, int a )
{
	/*	The keypad is part of this on purpose: Elona moves with its digits
		(config.txt key_north..key_southeast), and Eden never reports the
		keypad's own scancodes - a press arrives as the matching digit - so
		the top-row digit keys stand in for the keypad here.  In a text box
		the digit is typed as well and nothing reads stick(), so prompts keep
		working; in the field key_check() lets the direction override it.	*/
	if ( ks == NULL ) return 0;
	if ( a == 0 ) return ( ks[ SDL_SCANCODE_UP ] || ks[ SDL_SCANCODE_KP_8 ] ||
		ks[ SDL_SCANCODE_KP_7 ] || ks[ SDL_SCANCODE_KP_9 ] ||
		ks[ SDL_SCANCODE_8 ] || ks[ SDL_SCANCODE_7 ] ||
		ks[ SDL_SCANCODE_9 ] ) ? 1 : 0;
	if ( a == 1 ) return ( ks[ SDL_SCANCODE_LEFT ] || ks[ SDL_SCANCODE_KP_4 ] ||
		ks[ SDL_SCANCODE_KP_7 ] || ks[ SDL_SCANCODE_KP_1 ] ||
		ks[ SDL_SCANCODE_4 ] || ks[ SDL_SCANCODE_7 ] ||
		ks[ SDL_SCANCODE_1 ] ) ? 1 : 0;
	if ( a == 2 ) return ( ks[ SDL_SCANCODE_DOWN ] || ks[ SDL_SCANCODE_KP_2 ] ||
		ks[ SDL_SCANCODE_KP_1 ] || ks[ SDL_SCANCODE_KP_3 ] ||
		ks[ SDL_SCANCODE_2 ] || ks[ SDL_SCANCODE_1 ] ||
		ks[ SDL_SCANCODE_3 ] ) ? 1 : 0;
	return ( ks[ SDL_SCANCODE_RIGHT ] || ks[ SDL_SCANCODE_KP_6 ] ||
		ks[ SDL_SCANCODE_KP_9 ] || ks[ SDL_SCANCODE_KP_3 ] ||
		ks[ SDL_SCANCODE_6 ] || ks[ SDL_SCANCODE_9 ] ||
		ks[ SDL_SCANCODE_3 ] ) ? 1 : 0;
}
/*	Once per poll: hand over what the physical keyboard just pressed.  The
	keys Elona reads as scancodes - Escape (key_cancel), Tab (key_next), the
	keypad's 5 (key_wait) and Return (the getkey 13 readers) - go onto the
	scancode channel; everything else is typed into the character queue.	*/
static void sw_pc_tick( void )
{
	const Uint8 *ks;
	int shift, caps, sc;
	ks = SDL_GetKeyboardState( NULL );
	if ( ks == NULL ) return;
	shift = ks[ SDL_SCANCODE_LSHIFT ] || ks[ SDL_SCANCODE_RSHIFT ];
	caps = ( SDL_GetModState() & KMOD_CAPS ) ? 1 : 0;
	for ( sc = 0; sc < SDL_NUM_SCANCODES; sc++ ) {
		Uint8 now = ks[ sc ];
		char c;
		if ( now == sw_pc_last[ sc ] ) continue;
		sw_pc_last[ sc ] = now;
		/*	r427: the emulator door into the phrase panel.  Eden keeps F9
			for its own hotkeys and never passes it on, and ';' is the one key
			Elona binds nothing to that it does deliver - so that opens the
			panel and its character is dropped, leaving the game nothing to
			answer as an unassigned key.  A prompt keeps the semicolon.		*/
		if ( sc == SDL_SCANCODE_SEMICOLON && !switch_input_prompt_active() ) {
			if ( now ) switch_input_phrase_start();
			continue;
		}
		if ( sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_TAB ||
			 sc == SDL_SCANCODE_KP_5 || sc == SDL_SCANCODE_RETURN ) {
			if ( sc < SW_KEY_STATE_MAX ) sw_pc_keys[ sc ] = now;
			continue;
		}
		/*	The keypad's 5 is the wait key, and getkey 12 reads it there
			(key_map in hgiox_switch.cpp).  Eden sends the plain digit, so
			that one stands in for it too.										*/
		if ( sc == SDL_SCANCODE_5 ) sw_pc_keys[ SDL_SCANCODE_KP_5 ] = now;
		if ( !now ) continue;
		c = sw_pc_char( (SDL_Scancode)sc, shift, caps );
		if ( c >= '0' && c <= '9' && !switch_input_prompt_active() ) c = 0;
		if ( c != 0 ) sw_pc_push( c );
	}
}
/*	The next character the keyboard typed, 0 when there is none.			*/
static int sw_pc_pop( void )
{
	int c;
	if ( sw_pc_head == sw_pc_tail ) return 0;
	c = (unsigned char)sw_pc_buf[ sw_pc_head ];
	sw_pc_head = ( sw_pc_head + 1 ) % SW_PC_BUF_MAX;
	return c;
}
/*	The console's own software keyboard.  Elona's prompts are edit objects
	that only ever receive what the pad can type - X=z, Y=x, ZL=f, ZR=v, ... -
	so anything outside ASCII is impossible without this.  swkbdShow blocks
	until the player confirms or cancels, exactly like the input boxes do on
	the platforms this game was written for, and hands the text back as UTF-8,
	which is what the injection queue already speaks.
	Returns 1 with text, 0 when the player closed it without typing, and -2
	when the keyboard cannot be used at all (an applet-mode launch, for
	instance) - the caller then leaves the pad's letters as the only path.
	A cancel must stay distinct from "no keyboard": the caller re-raises the
	keyboard on a cancel, but must not retry one that cannot open.			*/
int switch_input_ask_text( char *out, int outsize )
{
#if defined(__SWITCH__)
	/*	SwkbdConfig is several KB (a union of every swkbd arg version) and
		this call sits inside the interpreter's call chain - keep it off the
		stack.  The function is not reentrant.							*/
	static SwkbdConfig kbd;
	Result rc;
	if ( out == NULL || outsize < 2 ) return -2;
	out[0] = 0;
	rc = swkbdCreate( &kbd, (size_t)outsize );
	if ( R_FAILED( rc ) ) return -2;
	swkbdConfigMakePresetDefault( &kbd );
	/*	The preset leaves the type at SwkbdType_Normal, which only offers the
		Latin layout - Chinese, Japanese and Korean would be unreachable.  The
		"all language keyboards" type turns every layout on; the keyboard's own
		globe key then switches between them.							*/
	swkbdConfigSetType( &kbd, SwkbdType_All );
	swkbdConfigSetGuideText( &kbd, "Elona" );
	rc = swkbdShow( &kbd, out, (size_t)outsize );
	swkbdClose( &kbd );
	if ( R_FAILED( rc ) ) return 0;			/* the player closed it empty	*/
	return ( out[0] != 0 ) ? 1 : 0;
#else
	(void)out;
	(void)outsize;
	return -2;
#endif
}
/*	r397: the quick-phrase panel.  The player picks a prepared line with the
	d-pad and sends it with A, so the system keyboard is only needed for free
	text.  Lines live in data/chat_phrases.txt, one per line, '#' comments.	*/
#define SW_PH_MAX	8
#define SW_PH_LEN	120
static char		sw_ph[ SW_PH_MAX ][ SW_PH_LEN ];
static int		sw_ph_n = 0, sw_ph_open = 0, sw_ph_cur = 0, sw_ph_loaded = 0;
static Uint8	sw_ph_a = 0, sw_ph_b = 0, sw_ph_u = 0, sw_ph_d = 0;
/*	r409: the press that closed the panel is still held for a few frames;
	this keeps it out of the game until every one of those keys is up.	*/
static int	sw_ph_swallow = 0;
/*	r400: the emulator's own door into the panel.  Eden feeds a pad-shaped
	view of the keyboard to the emulated console internally; SDL never sees a
	controller there, so ZR+ZL cannot reach this port.  F9/B are free in
	Elona's key set, and no console pad has a keyboard, so this stays inert
	on hardware.  B rides along because an emulator hotkey may eat F9 first.	*/
static int sw_ph_kb_combo( void )
{
	const Uint8 *k = SDL_GetKeyboardState( NULL );
	if ( k == NULL ) return 0;
	return k[ SDL_SCANCODE_F9 ];
}
static void sw_ph_trim( char *s )
{
	size_t l;
	while ( *s == ' ' || *s == '\t' ) memmove( s, s + 1, strlen( s ) );
	l = strlen( s );
	while ( l > 0 && ( s[ l - 1 ] == ' ' || s[ l - 1 ] == '\t' ||
			s[ l - 1 ] == '\r' || s[ l - 1 ] == '\n' ) )
		s[ --l ] = '\0';
}
static void sw_ph_load( void )
{
	FILE *f;
	if ( sw_ph_loaded ) return;
	sw_ph_loaded = 1;
	f = fopen( "sdmc:/switch/openhsp/data/chat_phrases.txt", "rb" );
	if ( f == NULL ) return;
	while ( sw_ph_n < SW_PH_MAX && fgets( sw_ph[ sw_ph_n ], SW_PH_LEN, f ) != NULL ) {
		sw_ph_trim( sw_ph[ sw_ph_n ] );
		if ( sw_ph[ sw_ph_n ][ 0 ] == '\0' || sw_ph[ sw_ph_n ][ 0 ] == '#' ) continue;
		sw_ph_n++;
	}
	fclose( f );
}
#if defined(__SWITCH__)
extern void hsp3dish_clear_keys( void );	/* linux/hsp3dish.cpp */
#endif
int switch_input_phrase_start( void )
{
	sw_ph_load();
	if ( sw_ph_n <= 0 ) return 0;
	sw_ph_open = 1;
	sw_ph_cur = 0;
	/*	r413: treat the confirm key as already down, so a stick that is
		still pushed while the panel opens does not send the first line. */
	sw_ph_a = 1;
	sw_ph_b = sw_ph_u = sw_ph_d = 0;
#if defined(__SWITCH__)
	/*	r414: a key held when the panel appeared must not still read as
		pressed inside the story - clear the glue's own table. */
	hsp3dish_clear_keys();
#endif
	return 1;
}
int switch_input_phrase_active( void ) { return sw_ph_open; }
/*	r414: the glue-level keys and the text buffer are separate from this
	module's own tables, so it needs one question it can ask.			*/
int switch_input_panel_blocks( void ) { return sw_ph_open || sw_ph_swallow; }
int switch_input_phrase_cursor( void ) { return sw_ph_cur; }
int switch_input_phrase_count( void ) { return sw_ph_n; }
const char *switch_input_phrase_text( int i )
{
	if ( i < 0 || i >= sw_ph_n ) return "";
	return sw_ph[ i ];
}
void switch_input_poll( void )
{
	int i;
	/*	r270: remember when the pad last moved.  Elona is turn based, so the
		game data changes only while somebody is playing - but a player who
		walks around without touching anything the panel tracks still counts
		as present, and this is the cheapest honest signal for it.  The
		physical keyboard counts too: Eden plays with one and has no pad.	*/
	{
		static Uint32 sig = 0;
		Uint32 s = 0;
		int b, a;
		if ( sw_pad != NULL ) {
			for ( b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++ )
				s = s * 31u + (Uint32)SDL_GameControllerGetButton( sw_pad, (SDL_GameControllerButton)b );
			for ( a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++ )
				s = s * 31u + (Uint32)( SDL_GameControllerGetAxis( sw_pad, (SDL_GameControllerAxis)a ) / 8000 );
		}
		{
			const Uint8 *k = SDL_GetKeyboardState( NULL );
			for ( b = 0; b < 128; b++ ) s = s * 31u + (Uint32)k[ b ];
		}
		if ( s != sig ) {
			sig = s;
			sw_activity_tick = SDL_GetTicks();
		}
	}
	if ( sw_inject_release >= 0 ) {
		sw_push_key( (SDL_Scancode)sw_inject_release, 0 );
		sw_inject_release = -1;
	}
	/*	Desktop / Eden fallback: the physical keyboard drives movement straight
		from SDL, so no gamepad layer is needed to walk the map.  The arrow keys
		and the keypad both count - config.txt moves with 8/2/4/6/7/9/1/3 - and
		sw_pc_dir() folds the two together.  The events go through the same
		sw_push_key() path as the pad ones, so is_pressed() / getkey()/stick all
		see them.  On the real console there is no physical keyboard, so the SDL
		keyboard state stays all-zero and nothing changes.  */
	{
		const Uint8 *ks;
		SDL_Scancode arrows[4];
		static int last[4] = { 0, 0, 0, 0 };
		int a;
		arrows[0] = SDL_SCANCODE_UP;
		arrows[1] = SDL_SCANCODE_LEFT;
		arrows[2] = SDL_SCANCODE_DOWN;
		arrows[3] = SDL_SCANCODE_RIGHT;
		/*	r187x: pump unconditionally.  The old guarded call only ran when a
			gamepad was attached, so on a desktop with no pad the SDL keyboard
			state (read below) never got refreshed and arrow keys stayed dead
			even though the events reached the window.  The pump is harmless on
			the console - no keyboard there, so the state stays all-zero.	*/
		SDL_PumpEvents();
		ks = SDL_GetKeyboardState( NULL );
		for ( a = 0; a < 4; a++ ) {
			int nw = sw_pc_dir( ks, a );
			if ( nw != last[a] ) { sw_push_key( arrows[a], nw ); last[a] = nw; }
			/*	r187v: the stick channel reads sw_keys[], not the synthetic
				events - hgio_stick()'s sw_input_key() is get_key_state(sc) ||
				switch_input_key_state(sc), and that reads this table.  A pushed
				SDL event never reached it, so a physical arrow key moved nobody
				on Eden; write the state on top of the push.
				r422: into sw_pc_keys, not sw_keys - the pad pass below rewrites
				every scancode sw_keymap owns (the d-pad's four directions among
				them) on each poll, so a direction that only the keyboard holds
				was cleared again before the script ever read it.  The keypad's
				digits move through the same table, and they have no synthetic
				event behind them at all.  */
			if ( arrows[a] < SW_KEY_STATE_MAX ) sw_pc_keys[ arrows[a] ] = (Uint8)nw;
		}
	}
	/*	r379: an event picture holds the screen until the confirm key is
		pressed.  Physical A arrives as SDL's B button (the mapping above),
		the desktop as Enter.  That whole press belongs to the picture: the
		swallow flag keeps it out of the game until the key is released.	*/
	{
		static int last_ok = 0;
		const Uint8 *ks;
		int ok = 0;
		SDL_PumpEvents();
		ks = SDL_GetKeyboardState( NULL );
		if ( ks != NULL && ( ks[ SDL_SCANCODE_RETURN ] || ks[ SDL_SCANCODE_KP_ENTER ] ) ) ok = 1;
		if ( sw_pad != NULL &&
			SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_B ) ) ok = 1;
		if ( ok && !last_ok && switch_overlay_cg_active() ) {
			switch_overlay_cg_dismiss();
			sw_cg_swallow = 1;
		}
		if ( !ok ) sw_cg_swallow = 0;
		last_ok = ok;
	}
	/*	r400: the desktop door into the panel.  Eden exposes no controller to
		SDL, so ZR+ZL (read in take_keys) never fires there; F9/B do the job
		and are inert on the console, which has no keyboard.  The log line is
		the only way to see inside a shipped emulator build - remove it with
		the other diagnostics before the release.								*/
	{
		static int last_f9 = 0;
		int f9 = sw_ph_kb_combo();
		if ( f9 && !last_f9 && !sw_ph_open ) switch_input_phrase_start();
		last_f9 = f9;
	}
	/*	r406: the emulator door.  Eden turns its keyboard into pad buttons
		inside its own input layer, so SDL sees a pad but never a keyboard and
		F9/B stay unreachable there.  A stick click is free - its scancode is
		UNKNOWN - and exists on the console too, so it opens the panel on both.*/
	{
		static Uint8 last_r3 = 0;

		if ( sw_pad != NULL ) {
			Uint8 r3;

			SDL_JoystickUpdate();
			r3 = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK );
			if ( r3 && !last_r3 && !sw_ph_open ) switch_input_phrase_start();
			last_r3 = r3;
		}
	}
	/*	r397: the phrase panel owns the pad while it is open.  The game keys
		are zeroed and the poll returns, so choosing a line never also walks
		or opens a menu.  r400: a desktop that exposes no pad at all (Eden
		turns the keyboard into the emulated pad inside its own input layer,
		where SDL's controller API cannot see it) drives the panel from the
		keyboard instead - arrows move, C/Enter send, X/Esc close.			*/
	if ( sw_ph_open ) {
		Uint8 u = 0, d = 0, a = 0, b = 0;
		const Uint8 *kb;
		SDL_PumpEvents();
		if ( sw_pad != NULL ) {
			SDL_JoystickUpdate();
			u = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_UP );
			d = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN );
			a = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_B );	/* confirm */
			/*	r413: Eden binds the keyboard's A to the left stick (WASD), so
				A is that direction and nothing else while the panel is up.		*/
			if ( SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTX ) <
					-SW_STICK_DEADZONE ) a = 1;
			b = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_A );	/* cancel  */
		}
		kb = SDL_GetKeyboardState( NULL );
		if ( kb != NULL ) {
			if ( kb[ SDL_SCANCODE_UP ] ) u = 1;
			if ( kb[ SDL_SCANCODE_DOWN ] ) d = 1;
			if ( kb[ SDL_SCANCODE_A ] || kb[ SDL_SCANCODE_C ] || kb[ SDL_SCANCODE_RETURN ] ) a = 1;
			if ( kb[ SDL_SCANCODE_B ] || kb[ SDL_SCANCODE_X ] || kb[ SDL_SCANCODE_ESCAPE ] ) b = 1;
		}
		if ( u && !sw_ph_u && sw_ph_cur > 0 ) sw_ph_cur--;
		if ( d && !sw_ph_d && sw_ph_cur + 1 < sw_ph_n ) sw_ph_cur++;
		if ( b && !sw_ph_b ) { sw_ph_open = 0; sw_ph_swallow = 1; }
		if ( a && !sw_ph_a && sw_ph_open ) {
			char text[ SW_PH_LEN ];
			snprintf( text, sizeof( text ), "%s", sw_ph[ sw_ph_cur ] );
			sw_ph_open = 0;
			sw_ph_swallow = 1;
			ai_chat_ask( text );
		}
		sw_ph_u = u; sw_ph_d = d; sw_ph_a = a; sw_ph_b = b;
		memset( sw_keys, 0, sizeof( sw_keys ) );
		return;
	}
	/*	r409: see sw_ph_swallow - consume the close press until it is up.*/
	if ( sw_ph_swallow ) {
		Uint8 a = 0, b = 0;
		const Uint8 *kb;
		if ( sw_pad != NULL ) {
			SDL_JoystickUpdate();
			a = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_B );
			b = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_A );
			if ( SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_UP ) ||
					SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN ) ||
					SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT ) ||
					SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT ) ) a = 1;
			/*	r415: the sticks type letters too - the left stick's 'c' opens
				the character sheet - so they stay swallowed as well.			*/
			if ( SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTX ) <=
					-SW_STICK_DEADZONE ||
					SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTX ) >=
					SW_STICK_DEADZONE ||
					SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTY ) <=
					-SW_STICK_DEADZONE ||
					SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTY ) >=
					SW_STICK_DEADZONE ) a = 1;
		}
		kb = SDL_GetKeyboardState( NULL );
		if ( kb != NULL && ( kb[ SDL_SCANCODE_A ] || kb[ SDL_SCANCODE_B ] ||
				kb[ SDL_SCANCODE_C ] || kb[ SDL_SCANCODE_RETURN ] ||
				kb[ SDL_SCANCODE_X ] || kb[ SDL_SCANCODE_ESCAPE ] ||
				kb[ SDL_SCANCODE_UP ] || kb[ SDL_SCANCODE_DOWN ] ||
				kb[ SDL_SCANCODE_LEFT ] || kb[ SDL_SCANCODE_RIGHT ] ) ) a = 1;
		memset( sw_keys, 0, sizeof( sw_keys ) );
		if ( !a && !b ) sw_ph_swallow = 0;
		return;
	}
	/*	r416: the physical keyboard, for Eden.  Runs after both panel
		branches so the press that drove the panel never also reaches the
		game, and before the pad checks so it works even on an emulator
		that exposes no controller to SDL.							*/
	sw_pc_tick();
	if ( sw_pad == NULL ) return;
	/*	SDL refreshes the joystick/controller state from inside the event
		pump, so this must come before the reads below.					*/
	SDL_PumpEvents();
	SDL_JoystickUpdate();
	for ( i = 0; i < SW_KEYMAP_N; i++ ) {
		SDL_Scancode sc = sw_entry_sc( i );
		Uint8 now = (Uint8)( ( sc != SDL_SCANCODE_UNKNOWN ) && sw_entry_pressed( i ) );
		/*	A layer flip re-maps an entry's scancode (L above all), so a key
			that is still held must hand the old one back before taking the
			new - otherwise the previous key would stay latched.			*/
		if ( sw_inject_sc[i] != SDL_SCANCODE_UNKNOWN && sw_inject_sc[i] != sc ) {
			if ( sw_inject_sc[i] < SW_KEY_STATE_MAX ) sw_keys[ sw_inject_sc[i] ] = 0;
			sw_push_key( sw_inject_sc[i], 0 );
			sw_inject_sc[i] = SDL_SCANCODE_UNKNOWN;
			sw_state[i] = 0;
		}
		if ( sc != SDL_SCANCODE_UNKNOWN && sc < SW_KEY_STATE_MAX &&
			!( sw_cg_swallow && sc == SDL_SCANCODE_RETURN ) ) sw_keys[sc] = now;
		if ( now == sw_state[i] ) continue;
		sw_state[i] = now;
		/*	A synthesised Return is not pushed.  It would only serve
			getkey(13), and sw_keys[] below already carries that - hsp3dish's
			sw_input_key() is get_key_state(sc) || switch_input_key_state(sc).
			What the event *did* add was a second Enter: the linux glue turns a
			keydown for Return into SendHSPObjectNotice( HSPOBJ_NOTICE_KEY_CR ),
			= 13, and the pad's A also types a CR that sw_key_tick() hands to the
			prompt's own box.  One press put two newlines in the box's bound
			variable, so every prompt in the character-creation chain advanced
			twice: the leftover newline completed the next one on the spot.
			Buttons whose scancode carries no notice (B, R) are pushed as before.
			The pad's letter and CR keys are unaffected - switch_input_take_keys()
			and sw_key_tick() deliver those straight to the objects.		*/
		if ( sc != SDL_SCANCODE_UNKNOWN ) {
			if ( sc != SDL_SCANCODE_RETURN ) sw_push_key( sc, now );
			sw_inject_sc[i] = now ? sc : SDL_SCANCODE_UNKNOWN;
		}
		sw_push_no++;
	}
	/*	P3 DIAGNOSTIC: the DirectInput-shaped value Elona's own gamepad path
		reads with DIGETJOYSTATE.  Printed on change only, so a run that never
		touches the pad stays quiet.										*/
	{
		static unsigned int last_bits = 0xffffffffu;
		unsigned int bits = switch_input_pad_bits();
		if ( bits != last_bits ) {
			last_bits = bits;
			printf( "hsp3switch: pad bits=0x%03x\n", bits );
			fflush( stdout );
		}
	}
/*	P3 diagnostic: the raw SDL view of the pad, once a second.  The
	failure this was added for is the pad going quiet after a save is
	loaded - pushed stops moving while the game still renders - and the
	question is whether SDL stopped reporting the buttons or the port
	stopped reading them.												*/
#if SWITCH_DIAG
	{
		static unsigned int dbg_last = 0;
		unsigned int dbg_now = (unsigned int)SDL_GetTicks();
		if ( ( dbg_now - dbg_last ) >= 1000u ) {
			int dl = 0, du = 0, dr = 0, dd = 0, nb = 0, na = 0;
			int lx = 0, ly = 0, rx = 0, ry = 0, att = -9;
			dbg_last = dbg_now;
			if ( sw_pad != NULL ) {
				dl = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT );
				du = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_UP );
				dr = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT );
				dd = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN );
				nb = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_B );
				na = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_A );
				att = SDL_GameControllerGetAttached( sw_pad );
				lx = (int)SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTX );
				ly = (int)SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTY );
				rx = (int)SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_RIGHTX );
				ry = (int)SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_RIGHTY );
			}
#if defined(__SWITCH__)
			/*	The raw libnx reading, so a frozen SDL gamecontroller can be
				told apart from a frozen HID layer.							*/
			{
				static PadState sw_rawpad;
				static int sw_rawpad_init = 0;
				u64 rawbtn = 0;
				if ( !sw_rawpad_init ) {
					sw_rawpad_init = 1;
					padInitializeDefault( &sw_rawpad );
				}
				padUpdate( &sw_rawpad );
				rawbtn = padGetButtons( &sw_rawpad );
				printf( "hsp3switch: PADRAW pad=%p njoy=%d isgc=%d att=%d "
					"d(L%dU%dR%dD%d) nb=%d na=%d ls(%d,%d) rs(%d,%d) pushed=%u raw=%llx\n",
					(void *)sw_pad, SDL_NumJoysticks(), SDL_IsGameController( 0 ), att,
					dl, du, dr, dd, nb, na, lx, ly, rx, ry, sw_push_no,
					(unsigned long long)rawbtn );
			}
#else
			printf( "hsp3switch: PADRAW pad=%p njoy=%d isgc=%d att=%d "
				"d(L%dU%dR%dD%d) nb=%d na=%d ls(%d,%d) rs(%d,%d) pushed=%u\n",
				(void *)sw_pad, SDL_NumJoysticks(), SDL_IsGameController( 0 ), att,
				dl, du, dr, dd, nb, na, lx, ly, rx, ry, sw_push_no );
#endif
			fflush( stdout );
		}
	}
#endif
	sw_poll_no++;
	if ( ( sw_poll_no % 120 ) == 0 ) {
		printf( "hsp3switch: pad poll=%u pushed=%u down[", sw_poll_no, sw_push_no );
		for ( i = 0; i < SW_KEYMAP_N; i++ ) {
			if ( sw_state[i] ) printf( " %d", (int)sw_keymap[i].sc );
		}
		printf( " ]\n" );
		fflush( stdout );
	}
	/*	The panel toggle (key hints <-> party HUD).  Two combinations walk it,
		each edge-triggered so the pair must be released and pressed again for
		the next flip:
		  ZR + right stick left - the pad's own binding, and the comfortable
		    one on the console;
		  R + right stick click - what this port used up to the three-layer
		    rework.  It is back because both keys are ordinary buttons that
		    every pad reports, while ZR is a trigger: a pad that hands ZR over
		    as a plain button (the emulator case r248 investigated) or not at
		    all leaves the ZR form unreachable, and then there is no way to
		    walk the three sheets at all.  The player found the stick click
		    hard to press, which is why it is no longer the only route.		*/
	{
		static int sw_ovl_combo = 0;
		static int sw_ovl_combo_alt = 0;
		static int sw_ovl_combo_zl = 0;
		/*	ZL on its own (Y on Eden) flips the panels, so the keyboard has a
			single-key route.  While ZR is held the pair belongs to the AI
			keyboard, so this stays quiet then.									*/
		int zl = sw_trigger_held( SDL_CONTROLLER_AXIS_TRIGGERLEFT, sw_zl_raw ) && !sw_zr_held();
		int now = ( sw_layer() == 2 )
			&& SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_RIGHTX ) < -SW_STICK_DEADZONE;
		int alt = ( sw_layer() == 1 )
			&& SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK );
		if ( now && !sw_ovl_combo ) switch_overlay_toggle();
		if ( zl && !sw_ovl_combo_zl ) switch_overlay_toggle();
		if ( alt && !sw_ovl_combo_alt ) switch_overlay_toggle();
		sw_ovl_combo = now;
		sw_ovl_combo_alt = alt;
		sw_ovl_combo_zl = zl;
	}
}
/*	Read by hgiox_switch.cpp's hgio_getkey/hgio_stick, so the pad stays visible
	even if the synthetic SDL events never make it through the event loop.	*/
int switch_input_key_state( int scancode )
{
	if ( scancode < 0 || scancode >= SW_KEY_STATE_MAX ) return 0;
	return ( sw_keys[scancode] || sw_pc_keys[scancode] ) ? 1 : 0;
}
/*----------------------------------------------------------------*/
/*	Character keys: the ones Elona only takes from its keylog box  */
/*----------------------------------------------------------------*/
/*	key_check() empties the hidden input box bound to `keylog`, takes its first
	character and reads a CR as Return.  Every letter the game acts on - the
	one-key menu shortcuts, and Shift+letter for everything else - plus Return
	itself therefore have to arrive as a *character*.  A synthesised SDL_KEYDOWN
	cannot do that: the reused event loop only fills that box from SDL_TEXTINPUT.
	So these buttons are typed through the route the keys.txt harness already
	proved: hsp3gr_dish_switch.cpp's sw_key_tick() writes the characters into
	keybuf and hands them to the object with func_notice, one every SW_KEY_GAP
	frames.  This module only says *which* character each press produces.
	`alt` is the character while R is held - the pad's second layer.			*/
typedef struct {
	SDL_GameControllerButton	btn;	/* or SW_NO_BUTTON				*/
	SDL_GameControllerAxis		axis;	/* SW_NO_AXIS when btn is used	*/
	char						base;	/* the base layer				*/
	char						alt;	/* the R layer (0 = nothing)	*/
	char						zr;		/* the ZR layer (0 = nothing)	*/
} SW_CHARKEY;
static const SW_CHARKEY sw_charkeys[] = {
	/*	Nintendo A (right): confirm, which in the field is also "attack what
		you face".  R layer: eat.  ZR layer: keypad 3.						*/
	{ SDL_CONTROLLER_BUTTON_B,			SW_NO_AXIS,							13,  'e', '3' },
	/*	Nintendo B (bottom): cancel is the Escape scancode above.  R layer:
		drink.  ZR layer: keypad 1.										*/
	{ SDL_CONTROLLER_BUTTON_A,			SW_NO_AXIS,							 0,  'q', '1' },
	/*	Nintendo X (top): action menu; R layer: pick up; ZR layer: keypad 9.
		SDL's Y is the top button (positional naming), so BUTTON_Y is used.	*/
	{ SDL_CONTROLLER_BUTTON_Y,			SW_NO_AXIS,							'z', 'g', '9' },
	/*	Nintendo Y (left): item menu; R layer: wear; ZR layer: keypad 7.
		SDL's X is the left button.											*/
	{ SDL_CONTROLLER_BUTTON_X,			SW_NO_AXIS,							'x', 'w', '7' },
	/*	ZL fires.  R layer: the read key, which the left-stick click used to
		carry.  ZR layer: r261 gave the keypad's 5 up - ZR+ZL now opens the
		companion chat instead, so it types nothing here.					*/
	{ SW_NO_BUTTON,	SDL_CONTROLLER_AXIS_TRIGGERLEFT,					'f', 'r',  0 },
	/*	ZR is the third layer's shift itself, so it types nothing anywhere.	*/
	{ SW_NO_BUTTON,	SDL_CONTROLLER_AXIS_TRIGGERRIGHT,					 0,   0,  0 },
	/*	- opens the help (the player picked it for this unobtrusive key).
		R layer: examine - the lock-on the base layer used to carry.
		ZR layer: page up.													*/
	{ SDL_CONTROLLER_BUTTON_BACK,		SW_NO_AXIS,							'?', 'l', '+' },
	/*	+ opens the save/settings menu (Shift+S).  R layer: the materials
		list.  ZR layer: page down.  Elona pages its lists with '+'/'-'
		(config.txt key_pageup/key_pagedown) and without them anything past
		the first page is unreachable.										*/
	{ SDL_CONTROLLER_BUTTON_START,		SW_NO_AXIS,							'S', 'm', '-' },
	/*	L carries scancodes only (see sw_keymap), so it types nothing here.	*/
	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,	SW_NO_AXIS,						 0,   0,  0 },
	/*	The d-pad feeds four utility keys on the R layer and the keypad
		digits 8/2/4/6 on the ZR layer - the same arrangement a PC keypad
		has, so that grid needs no memorising.  Its movement scancodes go
		quiet on both upper layers in sw_entry_down().						*/
	{ SDL_CONTROLLER_BUTTON_DPAD_UP,	SW_NO_AXIS,							 0, 'p', '8' },
	{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,	SW_NO_AXIS,							 0, 'O', '2' },
	{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,	SW_NO_AXIS,							 0, 'B', '4' },
	{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,	SW_NO_AXIS,							 0, 'F', '6' },
};
#define SW_CHARKEY_N	((int)( sizeof( sw_charkeys ) / sizeof( sw_charkeys[0] ) ))
/*	Left stick: the four "walk into it" keys, which have no scancode Elona
	reads, so they are typed: up descends/enters, down climbs, left asks for
	the character sheet, right throws something (Shift+T).					*/
#define SW_LSTICK_UP	'>'
#define SW_LSTICK_DOWN	'<'
#define SW_LSTICK_LEFT	'c'
#define SW_LSTICK_RIGHT	'T'
/*	Left stick, R layer: four utility actions.							*/
#define SW_LSTICK_UP_ALT	'h'		/* jump over things			*/
#define SW_LSTICK_DOWN_ALT	'D'		/* dig a wall				*/
#define SW_LSTICK_LEFT_ALT	'G'		/* give an item				*/
#define SW_LSTICK_RIGHT_ALT	'C'		/* close a door				*/
/*	Left stick, ZR layer: the two actions that sat on the stick clicks, moved
	here because the player found those hard to press.					*/
#define SW_LSTICK_UP_ZR		'd'		/* drop an item				*/
#define SW_LSTICK_DOWN_ZR	't'		/* use a tool				*/
#define SW_LSTICK_LEFT_ZR	'W'		/* wipe/polish				*/
#define SW_LSTICK_RIGHT_ZR	'I'		/* stack items				*/
/*	Right stick base: the four field actions with no scancode.  Up casts a
	spell now - ZR gave that up to become the third layer's shift, and the
	spell is the more-used of the two - so the wand moved to the R layer.	*/
#define SW_RSTICK_UP	'v'		/* cast a spell				*/
#define SW_RSTICK_DOWN	's'		/* search the ground		*/
#define SW_RSTICK_LEFT	'a'		/* use a special ability	*/
#define SW_RSTICK_RIGHT	'i'		/* talk to someone			*/
/*	Right stick, R layer: four more field actions.						*/
#define SW_RSTICK_UP_ALT	'Z'		/* whirl a wand (Shift+Z)	*/
#define SW_RSTICK_DOWN_ALT	'o'		/* open a container/door	*/
#define SW_RSTICK_LEFT_ALT	'j'		/* open the journal/log		*/
#define SW_RSTICK_RIGHT_ALT	'['		/* alter the terrain		*/
/*	Right stick, ZR layer.  The left direction is not typed - it toggles the
	side panels instead (see switch_input_poll).						*/
#define SW_RSTICK_UP_ZR		'/'		/* message log				*/
#define SW_RSTICK_DOWN_ZR	'b'		/* bash a wall				*/
#define SW_RSTICK_LEFT_ZR	0		/* the panel toggle			*/
#define SW_RSTICK_RIGHT_ZR	'A'		/* switch ammo				*/
static Uint8	sw_char_state[SW_CHARKEY_N];
static int		sw_lstick_x, sw_lstick_y, sw_rstick_x, sw_rstick_y;
/*	Direction of one stick axis, 0 inside the dead-zone.					*/
static int sw_stick_dir( SDL_GameControllerAxis axis )
{
	Sint16 v = SDL_GameControllerGetAxis( sw_pad, axis );
	if ( v < -SW_STICK_DEADZONE ) return -1;
	if ( v >  SW_STICK_DEADZONE ) return  1;
	return 0;
}
/*	Fire once when an axis leaves the dead-zone, and re-arm when it comes back
	to the middle.  A direction held while L is pressed or released never
	re-fires, because the tracker keeps following the axis on both layers.	*/
static char sw_stick_key( SDL_GameControllerAxis axis, int *last, char neg, char pos )
{
	int d = sw_stick_dir( axis );
	char c = 0;
	if ( d == *last ) return 0;
	*last = d;
	if ( d < 0 ) c = neg;
	else if ( d > 0 ) c = pos;
	return c;
}
int switch_input_take_keys( char *out, int max )
{
	int n = 0;
	int i, layer;
	char c;
	if ( out == NULL || max < 1 ) return 0;
	/*	r410: while the panel is up the game receives nothing at all -
		no letters from the pad and no characters from the keyboard bridge,
		so the confirm press cannot also open the game's own menu.  The
		press that closed the panel stays blocked until it is released.	*/
	if ( switch_input_phrase_active() || sw_ph_swallow ) return 0;
	/*	r416: the physical keyboard's characters come first - on Eden the
		keyboard is the way in, and the pad may not even be exposed there. */
	for ( i = 0; i < max; i++ ) {
		c = (char)sw_pc_pop();
		if ( c == 0 ) break;
		out[ n++ ] = c;
	}
	if ( n > 0 ) return n;
	if ( sw_pad == NULL ) return 0;
	layer = sw_layer();
	/*	r261: ZR+ZL opens the companion chat.  The keyboard blocks like any
		other Elona prompt, so the caller simply sees a longer frame; the
		typed line goes to the chatter module, prefixed /设定 for a persona
		note.  Emulators without applet support are skipped - launching the
		keyboard there crashes, which is why the marker exists.				*/
	if ( layer == 2 && sw_trigger_held( SDL_CONTROLLER_AXIS_TRIGGERLEFT, sw_zl_raw ) &&
			!sw_ai_key_held ) {
		sw_ai_key_held = 1;
		/*	r397: the prepared phrases cover the everyday lines; the system
			keyboard is opened only when no phrase file is present.			*/
		if ( !switch_input_phrase_start() ) {
			FILE *f = fopen( "sdmc:/switch/openhsp/noswkbd", "rb" );
			if ( f != NULL ) fclose( f );
			else {
				char text[ 512 ];
				if ( switch_input_ask_text( text, sizeof( text ) ) == 1 ) {
					if ( strncmp( text, "/设定", strlen( "/设定" ) ) == 0 )
						ai_chat_remember( text + strlen( "/设定" ) );
					else
						ai_chat_ask( text );
				}
			}
		}
	} else if ( !sw_trigger_held( SDL_CONTROLLER_AXIS_TRIGGERLEFT, sw_zl_raw ) ) {
		sw_ai_key_held = 0;
	}
	for ( i = 0; i < SW_CHARKEY_N; i++ ) {
		const SW_CHARKEY *e = &sw_charkeys[i];
		Uint8 now;
		if ( e->axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ) {
			/*	ZL may arrive as the raw button the mapping names (r248).	*/
			now = (Uint8)sw_trigger_held( SDL_CONTROLLER_AXIS_TRIGGERLEFT, sw_zl_raw );
		} else if ( e->axis != SW_NO_AXIS ) {
			now = (Uint8)( SDL_GameControllerGetAxis( sw_pad, e->axis ) > SW_TRIGGER_ON );
		} else {
			now = (Uint8)SDL_GameControllerGetButton( sw_pad, e->btn );
		}
		if ( now && !sw_char_state[i] ) {
			c = ( layer == 2 ) ? e->zr : ( ( layer == 1 ) ? e->alt : e->base );
			if ( c != 0 && n < max ) out[n++] = c;
		}
		sw_char_state[i] = now;
	}
	/*	Left stick picks its character from the active layer too: the base
		layer keeps descend/ascend/sheet/throw, the R layer the utilities,
		the ZR layer what is left over.									*/
	{
		char lup = ( layer == 2 ) ? SW_LSTICK_UP_ZR    : ( ( layer == 1 ) ? SW_LSTICK_UP_ALT    : SW_LSTICK_UP );
		char ldn = ( layer == 2 ) ? SW_LSTICK_DOWN_ZR  : ( ( layer == 1 ) ? SW_LSTICK_DOWN_ALT  : SW_LSTICK_DOWN );
		char llf = ( layer == 2 ) ? SW_LSTICK_LEFT_ZR  : ( ( layer == 1 ) ? SW_LSTICK_LEFT_ALT  : SW_LSTICK_LEFT );
		char lrt = ( layer == 2 ) ? SW_LSTICK_RIGHT_ZR : ( ( layer == 1 ) ? SW_LSTICK_RIGHT_ALT : SW_LSTICK_RIGHT );
		c = sw_stick_key( SDL_CONTROLLER_AXIS_LEFTX, &sw_lstick_x, llf, lrt );
		if ( c != 0 && n < max ) out[n++] = c;
		c = sw_stick_key( SDL_CONTROLLER_AXIS_LEFTY, &sw_lstick_y, lup, ldn );
		if ( c != 0 && n < max ) out[n++] = c;
	}
	/*	Right stick, same three layers: base keeps cast/search/ability/talk,
		R layer adds wand/open/journal/blend, ZR finishes the list.		*/
	{
		char rup = ( layer == 2 ) ? SW_RSTICK_UP_ZR    : ( ( layer == 1 ) ? SW_RSTICK_UP_ALT    : SW_RSTICK_UP );
		char rdn = ( layer == 2 ) ? SW_RSTICK_DOWN_ZR  : ( ( layer == 1 ) ? SW_RSTICK_DOWN_ALT  : SW_RSTICK_DOWN );
		char rlf = ( layer == 2 ) ? SW_RSTICK_LEFT_ZR  : ( ( layer == 1 ) ? SW_RSTICK_LEFT_ALT  : SW_RSTICK_LEFT );
		char rrt = ( layer == 2 ) ? SW_RSTICK_RIGHT_ZR : ( ( layer == 1 ) ? SW_RSTICK_RIGHT_ALT : SW_RSTICK_RIGHT );
		c = sw_stick_key( SDL_CONTROLLER_AXIS_RIGHTX, &sw_rstick_x, rlf, rrt );
		if ( c != 0 && n < max ) out[n++] = c;
		c = sw_stick_key( SDL_CONTROLLER_AXIS_RIGHTY, &sw_rstick_y, rup, rdn );
		if ( c != 0 && n < max ) out[n++] = c;
	}
	return n;
}
/*	Elona reads this DirectInput-shaped word with DIGETJOYSTATE and turns its
	bits into its own actions, the button index coming from its config
	(`key_cancel. "\" ,"0"`, `key_enter. " " ,"2"`).  Nothing is reported any
	more, and for the same reason the direction bits were cleared long ago:
	every button already reaches the script through the keyboard emulation
	above, so a pad bit here was a *second* source.  With both live, one press
	of Nintendo A confirmed twice - the log shows bit 6 (button index 2, the
	pad's enter) going high while the same press also typed a CR through
	key_check().  The keyboard map is the richer of the two - it carries the
	letters and the typed Return as well - so it is the one that stays.
	The pad is still polled by switch_input_poll(), which is what feeds
	sw_keys[] and the synthetic scancode events.							*/
unsigned int switch_input_pad_bits( void )
{
	return 0;
}
unsigned int switch_input_last_activity( void )
{
	return sw_activity_tick;
}
/*----------------------------------------------------------------*/
/*	Install														  */
/*----------------------------------------------------------------*/
void switch_input_inject_scancode( int sc )
{
	sw_push_key( (SDL_Scancode)sc, 1 );
	sw_inject_release = sc;
}
void switch_input_install( void *hspctx )
{
	(void)hspctx;		/* kept for the call site's shape; the pad is opened
						   here and polled from the GL clear, not from msgfunc */
	if ( !sw_installed ) {
		sw_installed = 1;
		memset( sw_state, 0, sizeof( sw_state ) );
		memset( sw_keys, 0, sizeof( sw_keys ) );
		/*	The window is double buffered, and HSP's redraw model expects the
			frame to persist between passes - Elona's title menu paints its
			background once and then repaints only the cursor region.  Without
			preservation that one-off paint lands in a single back buffer and
			the two buffers stay permanently different, so the menu alternates
			between the background and a black frame.  Ask EGL to keep the
			buffer contents across the swap, which makes the two converge.	*/
		{
			EGLDisplay dpy = eglGetCurrentDisplay();
			EGLSurface surf = eglGetCurrentSurface( EGL_DRAW );
			if ( dpy != EGL_NO_DISPLAY && surf != EGL_NO_SURFACE &&
				 eglSurfaceAttrib( dpy, surf, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED ) ) {
				printf( "hsp3switch: swap behaviour = EGL_BUFFER_PRESERVED\n" );
			} else {
				printf( "hsp3switch: EGL_BUFFER_PRESERVED refused (0x%x)\n", (unsigned)eglGetError() );
			}
		}
		/*	The frame is balanced - one clear, one render_start and one
			render_end per frame - and yet the text flickers, which is what
			an unsynchronised present looks like: the panel samples the
			buffer while it is still being drawn.  Ask for a vsynced swap.	*/
		if ( SDL_GL_SetSwapInterval( 1 ) != 0 ) {
			printf( "hsp3switch: SDL_GL_SetSwapInterval(1) failed: %s\n", SDL_GetError() );
		} else {
			printf( "hsp3switch: vsync swap interval = %d\n", SDL_GL_GetSwapInterval() );
		}
		/*	GAMECONTROLLER only: enabling JOYSTICK as well makes the Switch
			SDL port deliver every pad event twice (P1/T1.3 lesson).		*/
		if ( SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER ) != 0 ) {
			printf( "hsp3switch: gamecontroller init failed: %s\n", SDL_GetError() );
		} else {
			int n = SDL_NumJoysticks();
			int i;
			for ( i = 0; i < n; i++ ) {
				if ( SDL_IsGameController( i ) ) {
					sw_pad = SDL_GameControllerOpen( i );
					break;
				}
			}
			if ( sw_pad == NULL ) {
				printf( "hsp3switch: no game controller among %d joystick(s)\n", n );
			} else {
				char *map = SDL_GameControllerMapping( sw_pad );
				printf( "hsp3switch: game controller = %s (%d joystick(s))\n",
					SDL_GameControllerName( sw_pad ), n );
				/*	The mapping string is the only place the pad's button
					numbering is visible; it is what showed that SDL's
					Switch layout is a:b1,b:b0,x:b3,y:b2.				*/
				printf( "hsp3switch: mapping = %s\n", map ? map : "(none)" );
				/*	r248: pick ZL/ZR's raw button out of that string, so a pad
					that keeps the triggers as buttons still reaches the three
					layers.  The console's own mapping names them too, but there
					the axis is already live, so this is a no-op for it.	*/
				sw_zl_raw = sw_map_trigger_button( map, "lefttrigger" );
				sw_zr_raw = sw_map_trigger_button( map, "righttrigger" );
				if ( map != NULL ) SDL_free( map );
			}
		}
		fflush( stdout );
	}
}
