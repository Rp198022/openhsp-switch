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
//	The frame tick that does work is in this port's own GL shim:
//	hgio_reset() begins every frame with glClear(), so gles1_shim.cpp's
//	sw_glClear() already counts frames there.  It calls switch_input_poll()
//	once per frame, which is the sampling rate the event loop needs anyway.
//
//	Elona's full key set is P4 work (PLAN.md R5); this is the minimal mapping
//	the P2 gate needs (official sample scripts driving stick/getkey).
//
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

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

	{ SDL_CONTROLLER_BUTTON_A,				SW_NO_AXIS,		SDL_SCANCODE_RETURN },	/* confirm	*/
	{ SDL_CONTROLLER_BUTTON_B,				SW_NO_AXIS,		SDL_SCANCODE_ESCAPE },	/* cancel	*/
	{ SDL_CONTROLLER_BUTTON_X,				SW_NO_AXIS,		SDL_SCANCODE_Z },
	{ SDL_CONTROLLER_BUTTON_Y,				SW_NO_AXIS,		SDL_SCANCODE_X },

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

/*----------------------------------------------------------------*/
/*	Hardware probe												  */
/*	--------------------------------------------------------------  */
/*	Temporary instrumentation for the T2.3 gate; it is read-only and is
	removed once the bridge is verified.  The three things it has to
	separate are:

	  * is the msgfunc hook running at all			(poll#)
	  * does the controller API report the presses	(A/DL/lx/ly columns)
	  * do the synthetic key events reach keys[]		(keys* columns,
		which read the glue's own table one frame later)

	There is no on-screen console in a graphics build (R11 §5.3), so all of
	this goes out over nxlink.										*/
extern bool get_key_state( int sym );		/* src/hsp3dish/linux/hsp3dish.cpp */

static int		sw_poll_calls;
static Uint32	sw_probe_next;
static int		sw_trace_left = 40;

static void sw_probe( void )
{
	SDL_Joystick *js;
	Uint32 now = SDL_GetTicks();
	int jb = 0;
	int j;

	if ( (Sint32)( now - sw_probe_next ) < 0 ) return;
	sw_probe_next = now + 1000;

	/*	Raw joystick view: if the API above stays silent but these columns
		move, the pad is talking and only the gamecontroller layer is at
		fault.  If both stay silent, nothing is being scanned at all.	*/
	js = ( sw_pad != NULL ) ? SDL_GameControllerGetJoystick( sw_pad ) : NULL;
	for ( j = 0; js != NULL && j < 16; j++ ) {
		if ( SDL_JoystickGetButton( js, j ) ) jb |= ( 1 << j );
	}

	printf( "swinput: poll#%d pad=%p attached=%d njoy=%d A=%d DL=%d lx=%d ly=%d"
		" | js=%p jb=0x%04x jax0=%d jax1=%d"
		" | evt joy=%d ctrl=%d"
		" | keys L=%d R=%d Z=%d X=%d ESC=%d SPC=%d RET=%d\n",
		sw_poll_calls, (void *)sw_pad,
		sw_pad ? (int)SDL_GameControllerGetAttached( sw_pad ) : -1,
		SDL_NumJoysticks(),
		sw_pad ? (int)SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_A ) : -1,
		sw_pad ? (int)SDL_GameControllerGetButton( sw_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT ) : -1,
		sw_pad ? (int)SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTX ) : -999,
		sw_pad ? (int)SDL_GameControllerGetAxis( sw_pad, SDL_CONTROLLER_AXIS_LEFTY ) : -999,
		(void *)js, jb,
		js ? (int)SDL_JoystickGetAxis( js, 0 ) : -999,
		js ? (int)SDL_JoystickGetAxis( js, 1 ) : -999,
		SDL_JoystickEventState( SDL_QUERY ),
		SDL_GameControllerEventState( SDL_QUERY ),
		(int)get_key_state( SDL_SCANCODE_LEFT ),
		(int)get_key_state( SDL_SCANCODE_RIGHT ),
		(int)get_key_state( SDL_SCANCODE_Z ),
		(int)get_key_state( SDL_SCANCODE_X ),
		(int)get_key_state( SDL_SCANCODE_ESCAPE ),
		(int)get_key_state( SDL_SCANCODE_SPACE ),
		(int)get_key_state( SDL_SCANCODE_RETURN ) );
	fflush( stdout );
}

static void sw_probe_event( SDL_Scancode sc, int down, int rc )
{
	if ( sw_trace_left <= 0 ) return;
	sw_trace_left--;
	printf( "swinput: push sc=%d %s -> %d (%s)\n",
		(int)sc, down ? "down" : "up", rc, rc > 0 ? "queued" : SDL_GetError() );
	fflush( stdout );
}

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
	int rc;

	memset( &ev, 0, sizeof( ev ) );
	ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.type = ev.type;
	ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat = 0;
	ev.key.keysym.scancode = sc;
	ev.key.keysym.sym = SDL_GetKeyFromScancode( sc );
	ev.key.keysym.mod = KMOD_NONE;

	rc = SDL_PushEvent( &ev );
	sw_probe_event( sc, down, rc );
}

void switch_input_poll( void )
{
	int i;

	sw_poll_calls++;
	sw_probe();

	if ( sw_pad == NULL ) return;

	/*	SDL_JoystickUpdate() is the documented manual pump; SDL's own "called
		automatically by the event loop" only holds when joystick events are
		enabled, and the probe reports that state too.  Calling it twice with
		no change in between costs nothing.									*/
	SDL_PumpEvents();
	SDL_JoystickUpdate();
	for ( i = 0; i < SW_KEYMAP_N; i++ ) {
		Uint8 now = (Uint8)sw_entry_down( i );
		if ( now == sw_state[i] ) continue;
		sw_state[i] = now;
		sw_push_key( sw_keymap[i].sc, now );
	}
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

		/*	GAMECONTROLLER only: enabling JOYSTICK as well makes the Switch
			SDL port deliver every pad event twice (P1/T1.3 lesson).		*/
		if ( SDL_InitSubSystem( SDL_INIT_GAMECONTROLLER ) != 0 ) {
			printf( "hsp3switch: gamecontroller init failed: %s\n", SDL_GetError() );
		} else {
			int n = SDL_NumJoysticks();
			int i;
			for ( i = 0; i < n; i++ ) {
				printf( "hsp3switch: joystick %d = %s (gamecontroller=%d)\n",
					i, SDL_JoystickNameForIndex( i ), SDL_IsGameController( i ) );
			}
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
				printf( "hsp3switch: game controller = %s\n", SDL_GameControllerName( sw_pad ) );
				printf( "hsp3switch: mapping = %s\n", map ? map : "(none)" );
				if ( map != NULL ) SDL_free( map );
			}
		}
		fflush( stdout );
	}
}