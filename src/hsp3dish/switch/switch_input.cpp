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

static const SW_KEYMAP_ENTRY sw_keymap[] = {
	{ SDL_CONTROLLER_BUTTON_DPAD_LEFT,		SW_NO_AXIS,		SDL_SCANCODE_LEFT },
	{ SDL_CONTROLLER_BUTTON_DPAD_UP,		SW_NO_AXIS,		SDL_SCANCODE_UP },
	{ SDL_CONTROLLER_BUTTON_DPAD_RIGHT,		SW_NO_AXIS,		SDL_SCANCODE_RIGHT },
	{ SDL_CONTROLLER_BUTTON_DPAD_DOWN,		SW_NO_AXIS,		SDL_SCANCODE_DOWN },

	/*	The face buttons are paired with scancodes by PHYSICAL POSITION, not by
		name.  SDL names them the Xbox way - its logical A is the bottom
		button and its logical B the right-hand one - while Nintendo labels
		the Switch the other way round (A right, B bottom).  The mapping
		string from this port's own startup line says which is which:
		a:b1,b:b0,x:b3,y:b2.

		Hardware confirmed both consequences of that:
		  * the pad's B reported as SDL A (RETURN) and the pad's A as SDL B
		    (ESCAPE), i.e. the confirm/cancel pair was crossed;
		  * the pad's X reported as SDL X but sits right, its Y as SDL Y but
		    sits left, crossing the Z/X pair on a keyboard where Z is the
		    left-hand key.

		So each entry below is the SDL name for the button in that *position*:
		SDL B is the top-right button (Nintendo's A), SDL A the bottom one
		(Nintendo's B), SDL X the top one, SDL Y the left one.				*/
	{ SDL_CONTROLLER_BUTTON_B,				SW_NO_AXIS,		SDL_SCANCODE_RETURN },	/* confirm: pad A, right	*/
	{ SDL_CONTROLLER_BUTTON_A,				SW_NO_AXIS,		SDL_SCANCODE_ESCAPE },	/* cancel:  pad B, bottom	*/
	{ SDL_CONTROLLER_BUTTON_Y,				SW_NO_AXIS,		SDL_SCANCODE_Z },		/* left of pair:  pad Y	*/
	{ SDL_CONTROLLER_BUTTON_X,				SW_NO_AXIS,		SDL_SCANCODE_X },		/* right of pair: pad X	*/

	{ SDL_CONTROLLER_BUTTON_LEFTSHOULDER,	SW_NO_AXIS,		SDL_SCANCODE_SPACE },
	{ SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,	SW_NO_AXIS,		SDL_SCANCODE_TAB },
	{ SW_NO_BUTTON,							SDL_CONTROLLER_AXIS_TRIGGERLEFT,	SDL_SCANCODE_LCTRL },
	{ SW_NO_BUTTON,							SDL_CONTROLLER_AXIS_TRIGGERRIGHT,	SDL_SCANCODE_C },

	{ SDL_CONTROLLER_BUTTON_START,			SW_NO_AXIS,		SDL_SCANCODE_F1 },
	{ SDL_CONTROLLER_BUTTON_BACK,			SW_NO_AXIS,		SDL_SCANCODE_F2 },
};

#define SW_KEYMAP_N	((int)( sizeof( sw_keymap ) / sizeof( sw_keymap[0] ) ))

/*	Analog stick acts as a d-pad so `stick` works without touching it.	*/
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

static int sw_entry_down( int i )
{
	const SW_KEYMAP_ENTRY *e = &sw_keymap[i];
	Sint16 lx, ly;

	if ( e->btn != SW_NO_BUTTON && SDL_GameControllerGetButton( sw_pad, e->btn ) ) return 1;
	if ( e->axis != SW_NO_AXIS && SDL_GameControllerGetAxis( sw_pad, e->axis ) > SW_TRIGGER_ON ) return 1;

	lx = SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTX );
	ly = SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTY );
	switch ( e->sc ) {
	case SDL_SCANCODE_LEFT:		return lx < -SW_STICK_DEADZONE;
	case SDL_SCANCODE_RIGHT:	return lx >  SW_STICK_DEADZONE;
	case SDL_SCANCODE_UP:		return ly < -SW_STICK_DEADZONE;
	case SDL_SCANCODE_DOWN:		return ly >  SW_STICK_DEADZONE;
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