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
#include <string.h>

#include <SDL2/SDL.h>
#if defined(__SWITCH__)
#include <switch.h>
#endif
#include <EGL/egl.h>

#include "switch_input.h"
#include "switch_overlay.h"
/*	Switch diagnostics.  0 = shipping build.						*/
#ifndef SWITCH_DIAG
#define SWITCH_DIAG 1
#endif


/*----------------------------------------------------------------*/
/*	Mapping														  */
/*----------------------------------------------------------------*/

typedef struct {
	SDL_GameControllerButton	btn;	/* or SW_NO_BUTTON					*/
	SDL_GameControllerAxis		axis;	/* -1 when unused					*/
	SDL_Scancode				sc;
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
		POSITION: SDL B is the top-right button (Nintendo's A), SDL A the
		bottom one (Nintendo's B), SDL X the top one, SDL Y the left one.
		That is what this port's own startup line prints: a:b1,b:b0,x:b3,y:b2.

		Return stays here even though the pad's A also *types* a CR: the two
		serve different readers.  key_check() wants the character, but a few
		routines take Enter straight from the key with `getkey p,13` - the
		item marker in command.hsp, the help viewer - and those need the
		scancode.  Only Nintendo's B has a scancode key_check() reads, and
		only while it is not on the second layer; on that layer the bottom
		button drinks instead.  X and Y are typed, not scancoded.			*/
	{ SDL_CONTROLLER_BUTTON_B,				SW_NO_AXIS,		SDL_SCANCODE_RETURN },	/* confirm: pad A, right	*/
	{ SDL_CONTROLLER_BUTTON_A,				SW_NO_AXIS,		SDL_SCANCODE_ESCAPE },	/* cancel:  pad B, bottom	*/
	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,	SW_NO_AXIS,		SDL_SCANCODE_TAB },		/* next tab: pad L			*/
	{ SDL_CONTROLLER_BUTTON_RIGHTSTICK,		SW_NO_AXIS,		SDL_SCANCODE_SPACE },	/* wait: right stick click	*/
	/*	Elona's save list deletes a save with BackSpace - system.hsp's
		*game_title_selectID reads `getkey a, 8` and its hint bar says
		"BackSpace [Delete]".  The pad has no button left over for it, so it is
		the R + L combination (R holding the layer), read in sw_entry_down() below.				*/
	{ SW_NO_BUTTON,							SW_NO_AXIS,		SDL_SCANCODE_BACKSPACE },
};

#define SW_KEYMAP_N	((int)( sizeof( sw_keymap ) / sizeof( sw_keymap[0] ) ))

/*	Both sticks type characters - a stick has no scancode Elona reads, so
		each direction is a key press on the character channel instead (see
		switch_input_take_keys()).  The d-pad is the only thing that moves the
		cursor keys, and it does so through the scancode table above.			*/
#define SW_STICK_DEADZONE	12000

static SDL_GameController	*sw_pad;
static Uint8				sw_state[SW_KEYMAP_N];
static int					sw_installed;
#define SW_KEY_STATE_MAX	512
static Uint8				sw_keys[SW_KEY_STATE_MAX];
static unsigned int			sw_poll_no;
static unsigned int			sw_push_no;

/*----------------------------------------------------------------*/
/*	Polling														  */
/*----------------------------------------------------------------*/

/*	True while R is held: the pad is on its second layer then.				*/
static int sw_layer_second( void )
{
	return SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ) ? 1 : 0;
}

static int sw_entry_down( int i )
{
	const SW_KEYMAP_ENTRY *e = &sw_keymap[i];

	/*	On the second layer the bottom button types 'q' and must stop being
		Escape.  key_check() reads the keylog first and then lets a non-zero
		`stick p,15` overwrite the result, so an Escape arriving at the same
		moment would throw the letter away.								*/
	if ( e->sc == SDL_SCANCODE_ESCAPE && sw_layer_second() ) return 0;

	/*	Delete is R + L: R is the second-layer shift now, L has taken the
		Tab role, and every other button has a first-layer job.  While the
		pair is held the Tab entry (L) goes quiet - otherwise one press
		reports two keys, which is the mistake R48/R49 are about.										*/
	if ( ( e->sc == SDL_SCANCODE_BACKSPACE ) || ( e->sc == SDL_SCANCODE_TAB ) ) {
		int del = sw_layer_second()
			&& SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER );
		if ( e->sc == SDL_SCANCODE_BACKSPACE ) return del ? 1 : 0;
		if ( del ) return 0;
	}

	/*	R + right-stick click toggles the side-panel key hints.  The stick
		click alone is Space (wait), so while the pair is held it goes
		quiet - otherwise the same press that flips the hints would also
		make the character rest.  R itself types nothing (it is the second
		layer's shift now), so only the click needs silencing.  */
	if ( e->sc == SDL_SCANCODE_SPACE ) {
		if ( SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER )
			&& SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK ) ) return 0;
	}

	if ( e->btn != SW_NO_BUTTON && SDL_GameControllerGetButton( sw_pad, e->btn ) ) return 1;
	if ( e->axis != SW_NO_AXIS && SDL_GameControllerGetAxis( sw_pad, e->axis ) > SW_TRIGGER_ON ) return 1;

	/*	The right stick used to be spent here, doubling as the cursor keys for
		Elona's lock-on cursor.  It types its own four characters now (see
		switch_input_take_keys() below), so nothing in this table reads it and
		the cursor keys come from the d-pad alone.							*/
	return 0;
}

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

void switch_input_poll( void )
{
	int i;

	if ( sw_pad == NULL ) return;

	/*	SDL refreshes the joystick/controller state from inside the event
		pump, so this must come before the reads below.					*/
	SDL_PumpEvents();
	SDL_JoystickUpdate();
	for ( i = 0; i < SW_KEYMAP_N; i++ ) {
		SDL_Scancode sc = sw_keymap[i].sc;
		Uint8 now = (Uint8)sw_entry_down( i );
		if ( sc < SW_KEY_STATE_MAX ) sw_keys[sc] = now;
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
		if ( sc != SDL_SCANCODE_RETURN ) sw_push_key( sc, now );
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

	/*	The hints toggle is edge-triggered: the pair must be released and
		pressed again for the next flip.  */
	{
		static int sw_ovl_combo = 0;
		int now = SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER )
			&& SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK );
		if ( now && !sw_ovl_combo ) switch_overlay_toggle();
		sw_ovl_combo = now;
	}
}

/*	Read by hgiox_switch.cpp's hgio_getkey/hgio_stick, so the pad stays visible
	even if the synthetic SDL events never make it through the event loop.	*/
int switch_input_key_state( int scancode )
{
	if ( scancode < 0 || scancode >= SW_KEY_STATE_MAX ) return 0;
	return sw_keys[scancode] ? 1 : 0;
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
	char						base;
	char						alt;	/* 0 = nothing on that layer	*/
} SW_CHARKEY;

static const SW_CHARKEY sw_charkeys[] = {
	/*	Nintendo A (right): confirm, which in the field is also "attack what
		you face"; second layer: eat.									*/
	{ SDL_CONTROLLER_BUTTON_B,			SW_NO_AXIS,							13,  'e' },
	/*	Nintendo B (bottom): cancel is the Escape scancode above; second
		layer: drink.													*/
	{ SDL_CONTROLLER_BUTTON_A,			SW_NO_AXIS,							 0,  'q' },
	/*	Nintendo X (top): action menu; second layer: read.				*/
	{ SDL_CONTROLLER_BUTTON_X,			SW_NO_AXIS,							'z', 'r' },
	/*	Nintendo Y (left): item menu; second layer: wear.				*/
	{ SDL_CONTROLLER_BUTTON_Y,			SW_NO_AXIS,							'x', 'w' },
	/*	ZL fires, ZR casts.												*/
	{ SW_NO_BUTTON,	SDL_CONTROLLER_AXIS_TRIGGERLEFT,					'f',  0 },
	{ SW_NO_BUTTON,	SDL_CONTROLLER_AXIS_TRIGGERRIGHT,					'v',  0 },
	/*	- locks the target on; + opens the save/settings menu (Shift+S).	*/
	/*	The minus button's second layer is the appearance editor: Elona opens
		it from the character sheet with a literal 'p' key
		(command.hsp:10312, `*com_charainfo` page 0), and every other button
		already has a first-layer job.									*/
	{ SDL_CONTROLLER_BUTTON_BACK,		SW_NO_AXIS,							'l', 'p' },
	{ SDL_CONTROLLER_BUTTON_START,		SW_NO_AXIS,							'S',  0 },
	/*	Left stick press: pick up.  The right stick's press is the Space key
		now - it is in sw_keymap() above, so it is not typed here.			*/
	{ SDL_CONTROLLER_BUTTON_LEFTSTICK,	SW_NO_AXIS,							'g',  0 },
};

#define SW_CHARKEY_N	((int)( sizeof( sw_charkeys ) / sizeof( sw_charkeys[0] ) ))

/*	Left stick: the four "walk into it" keys, which have no scancode Elona
	reads, so they are typed: up descends/enters, down climbs, left asks for
	the character sheet, right throws something (Shift+T).					*/
#define SW_LSTICK_UP	'>'
#define SW_LSTICK_DOWN	'<'
#define SW_LSTICK_LEFT	'c'
#define SW_LSTICK_RIGHT	'T'

/*	Right stick: the four field actions that have no scancode.  They fire on
		either layer - the stick is not the cursor any more, so there is no
		first-layer job left for it to keep.								*/
#define SW_RSTICK_UP	'Z'		/* whirl a wand (Shift+Z)	*/
#define SW_RSTICK_DOWN	's'		/* search the ground		*/
#define SW_RSTICK_LEFT	'a'		/* use a special ability	*/
#define SW_RSTICK_RIGHT	'i'		/* talk to someone			*/

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

	if ( sw_pad == NULL || out == NULL || max < 1 ) return 0;

	layer = sw_layer_second();

	for ( i = 0; i < SW_CHARKEY_N; i++ ) {
		const SW_CHARKEY *e = &sw_charkeys[i];
		Uint8 now;

		if ( e->axis != SW_NO_AXIS ) {
			now = (Uint8)( SDL_GameControllerGetAxis( sw_pad, e->axis ) > SW_TRIGGER_ON );
		} else {
			now = (Uint8)SDL_GameControllerGetButton( sw_pad, e->btn );
		}
		if ( now && !sw_char_state[i] ) {
			c = layer ? e->alt : e->base;
			if ( c != 0 && n < max ) out[n++] = c;
		}
		sw_char_state[i] = now;
	}

	c = sw_stick_key( SDL_CONTROLLER_AXIS_LEFTX, &sw_lstick_x, SW_LSTICK_LEFT, SW_LSTICK_RIGHT );
	if ( c != 0 && n < max ) out[n++] = c;
	c = sw_stick_key( SDL_CONTROLLER_AXIS_LEFTY, &sw_lstick_y, SW_LSTICK_UP, SW_LSTICK_DOWN );
	if ( c != 0 && n < max ) out[n++] = c;
	c = sw_stick_key( SDL_CONTROLLER_AXIS_RIGHTX, &sw_rstick_x, SW_RSTICK_LEFT, SW_RSTICK_RIGHT );
	if ( c != 0 && n < max ) out[n++] = c;
	c = sw_stick_key( SDL_CONTROLLER_AXIS_RIGHTY, &sw_rstick_y, SW_RSTICK_UP, SW_RSTICK_DOWN );
	if ( c != 0 && n < max ) out[n++] = c;

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
/*----------------------------------------------------------------*/
/*	Install														  */
/*----------------------------------------------------------------*/

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
				if ( map != NULL ) SDL_free( map );
			}
		}
		fflush( stdout );
	}
}