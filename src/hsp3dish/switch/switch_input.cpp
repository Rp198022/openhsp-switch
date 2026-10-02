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
#include <EGL/egl.h>

#include "switch_input.h"

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
	{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,	SW_NO_AXIS,		SDL_SCANCODE_TAB },		/* next tab: pad R			*/
};

#define SW_KEYMAP_N	((int)( sizeof( sw_keymap ) / sizeof( sw_keymap[0] ) ))

/*	Analog stick acts as a d-pad so `stick` works without touching it.  The
	RIGHT stick is the one spent on that (it is the lock-on cursor); the left
	one types characters, see switch_input_take_keys().						*/
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

/*	True while L is held: the pad is on its second layer then.				*/
static int sw_layer_second( void )
{
	return SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER ) ? 1 : 0;
}

static int sw_entry_down( int i )
{
	const SW_KEYMAP_ENTRY *e = &sw_keymap[i];
	Sint16 rx, ry;

	/*	On the second layer the bottom button types 'q' and must stop being
		Escape.  key_check() reads the keylog first and then lets a non-zero
		`stick p,15` overwrite the result, so an Escape arriving at the same
		moment would throw the letter away.								*/
	if ( e->sc == SDL_SCANCODE_ESCAPE && sw_layer_second() ) return 0;

	if ( e->btn != SW_NO_BUTTON && SDL_GameControllerGetButton( sw_pad, e->btn ) ) return 1;
	if ( e->axis != SW_NO_AXIS && SDL_GameControllerGetAxis( sw_pad, e->axis ) > SW_TRIGGER_ON ) return 1;

	/*	The right stick doubles as the cursor keys (Elona's lock-on cursor).	*/
	rx = SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_RIGHTX );
	ry = SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_RIGHTY );
	switch ( e->sc ) {
	case SDL_SCANCODE_LEFT:		return rx < -SW_STICK_DEADZONE;
	case SDL_SCANCODE_RIGHT:	return rx >  SW_STICK_DEADZONE;
	case SDL_SCANCODE_UP:		return ry < -SW_STICK_DEADZONE;
	case SDL_SCANCODE_DOWN:		return ry >  SW_STICK_DEADZONE;
	default:					break;
	}
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
		sw_push_key( sc, now );
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
	sw_poll_no++;
	if ( ( sw_poll_no % 120 ) == 0 ) {
		printf( "hsp3switch: pad poll=%u pushed=%u down[", sw_poll_no, sw_push_no );
		for ( i = 0; i < SW_KEYMAP_N; i++ ) {
			if ( sw_state[i] ) printf( " %d", (int)sw_keymap[i].sc );
		}
		printf( " ]\n" );
		fflush( stdout );
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

	`alt` is the character while L is held - the pad's second layer.			*/
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
	{ SDL_CONTROLLER_BUTTON_BACK,		SW_NO_AXIS,							'l',  0 },
	{ SDL_CONTROLLER_BUTTON_START,		SW_NO_AXIS,							'S',  0 },
	/*	Stick presses: pick up; rest in place (Shift+R) / drop.			*/
	{ SDL_CONTROLLER_BUTTON_LEFTSTICK,	SW_NO_AXIS,							'g',  0 },
	{ SDL_CONTROLLER_BUTTON_RIGHTSTICK,	SW_NO_AXIS,							'R', 'd' },
};

#define SW_CHARKEY_N	((int)( sizeof( sw_charkeys ) / sizeof( sw_charkeys[0] ) ))

/*	Left stick: the four "walk into it" keys, which have no scancode Elona
	reads, so they are typed: up descends/enters, down climbs, left asks for
	the character sheet, right throws something (Shift+T).					*/
#define SW_LSTICK_UP	'>'
#define SW_LSTICK_DOWN	'<'
#define SW_LSTICK_LEFT	'c'
#define SW_LSTICK_RIGHT	'T'

/*	Right stick, second layer only: the letters the first layer gives to the
	cursor keys.															*/
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
	c = sw_stick_key( SDL_CONTROLLER_AXIS_RIGHTX, &sw_rstick_x,
						( layer ? SW_RSTICK_LEFT : 0 ), ( layer ? SW_RSTICK_RIGHT : 0 ) );
	if ( c != 0 && n < max ) out[n++] = c;
	c = sw_stick_key( SDL_CONTROLLER_AXIS_RIGHTY, &sw_rstick_y,
						( layer ? SW_RSTICK_UP : 0 ), ( layer ? SW_RSTICK_DOWN : 0 ) );
	if ( c != 0 && n < max ) out[n++] = c;

	return n;
}
/*	The DirectInput-shaped joypad state Elona's own gamepad path wants.  Its
	config maps buttons to keys by index - `key_cancel. "\" ,"0"`,
	`key_enter. " " ,"2"` - and it reads the state with DIGETJOYSTATE, so the
	layout here decides which Switch button becomes which action.

	It has to agree with the keyboard map above, which is the Nintendo way round:
	the right-hand button confirms and the bottom one cancels.  SDL names those
	two the other way round on this mapping (`a:b1,b:b0` - SDL A is the bottom
	button), so SDL A takes index 0 (cancel) and SDL B index 2 (enter).  With the
	two layers disagreeing, one press produced both key_enter and key_cancel.	*/
static const SDL_GameControllerButton sw_pad_button[] = {
	SDL_CONTROLLER_BUTTON_A,			/* 0: cancel, bottom (Nintendo B)	*/
	SDL_CONTROLLER_BUTTON_X,			/* 1: top					*/
	SDL_CONTROLLER_BUTTON_B,			/* 2: enter, right (Nintendo A)	*/
	SDL_CONTROLLER_BUTTON_Y,			/* 3: left					*/
	SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
	SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
	SDL_CONTROLLER_BUTTON_BACK,
	SDL_CONTROLLER_BUTTON_START,
	SDL_CONTROLLER_BUTTON_LEFTSTICK,
	SDL_CONTROLLER_BUTTON_RIGHTSTICK,
};

#define SW_PAD_BUTTON_N	((int)( sizeof( sw_pad_button ) / sizeof( sw_pad_button[0] ) ))

unsigned int switch_input_pad_bits( void )
{
	unsigned int bits = 0;
	Sint16 lx, ly;
	int j;

	if ( sw_pad == NULL ) return 0;

	SDL_PumpEvents();
	SDL_JoystickUpdate();

	/*	Bits 0-3 are deliberately left clear.  They are the D-pad and the left
		stick in the real DirectInput layout, and Elona does read them as
		directions - but it reads the keyboard's directions into that same
		variable first and then ADDS these onto them (init.hsp: `stick p,15`,
		then `HMMBITCHECK j,0..3` doing `p += 2, 8, 1, 4`).  A D-pad press
		reaches the script twice in this port - once as these bits, and once as
		the arrow scancodes pushed below, which hsp3 turns into the very same
		stick bits - so the two add up: down became 8 + 8 = 16, which is not a
		direction, and no menu cursor in the game would move.  The scancodes
		alone carry it correctly, so the bits are left clear to keep one source.
	*/
	(void)lx;
	(void)ly;

	for ( j = 0; j < SW_PAD_BUTTON_N; j++ ) {
		if ( SDL_GameControllerGetButton( sw_pad, sw_pad_button[j] ) ) bits |= 1u << ( 4 + j );
	}
	if ( SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT ) > SW_TRIGGER_ON )		bits |= 1u << ( 4 + 10 );
	if ( SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT ) > SW_TRIGGER_ON )	bits |= 1u << ( 4 + 11 );

	return bits;
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