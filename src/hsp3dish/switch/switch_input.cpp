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
//	  * the pad is polled once per interpreter frame;
//	  * a change in any mapped button is pushed into the SDL event queue as a
//	    synthetic SDL_KEYDOWN/SDL_KEYUP with the scancode the Linux glue would
//	    have seen for that key;
//	  * the glue's own handleEvent() then consumes it exactly like a real
//	    keypress, so `stick`, `getkey` and `onkey` all keep working with no
//	    further changes and no new HSP key codes.
//
//	The per-frame hook is ctx->msgfunc.  linux/hsp3dish.cpp installs
//	hsp3dish_msgfunc there during hsp3dish_init_sub(), and then calls
//	hsp3typeinit_sock_extcmd() with the context attached - the one moment the
//	reused glue hands us the HSPCTX.  We wrap msgfunc from there.
//
//	Elona's full key set is P4 work (PLAN.md R5); this is the minimal mapping
//	the P2 gate needs (official sample scripts driving stick/getkey).
//
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "../../hsp3/hsp3config.h"
#include "../../hsp3/hsp3code.h"

#include "switch_input.h"

/*	Defined in the reused src/hsp3dish/linux/hsp3dish.cpp.	*/
void hsp3dish_msgfunc( HSPCTX *hspctx );

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

static HSPCTX				*sw_ctx;
static SDL_GameController	*sw_pad;
static Uint8				sw_state[SW_KEYMAP_N];
static int					sw_installed;

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

static void sw_poll( void )
{
	int i;

	if ( sw_pad == NULL ) return;

	SDL_PumpEvents();
	for ( i = 0; i < SW_KEYMAP_N; i++ ) {
		Uint8 now = (Uint8)sw_entry_down( i );
		if ( now == sw_state[i] ) continue;
		sw_state[i] = now;
		sw_push_key( sw_keymap[i].sc, now );
	}
}

/*----------------------------------------------------------------*/
/*	msgfunc hook												  */
/*----------------------------------------------------------------*/

static void switch_msgfunc( HSPCTX *ctx )
{
	sw_poll();
	hsp3dish_msgfunc( ctx );
}

/*----------------------------------------------------------------*/
/*	Install														  */
/*----------------------------------------------------------------*/

void switch_input_install( void *hspctx )
{
	sw_ctx = (HSPCTX *)hspctx;

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
				if ( SDL_IsGameController( i ) ) {
					sw_pad = SDL_GameControllerOpen( i );
					break;
				}
			}
			if ( sw_pad == NULL ) {
				printf( "hsp3switch: no game controller among %d joystick(s)\n", n );
			} else {
				printf( "hsp3switch: game controller = %s\n", SDL_GameControllerName( sw_pad ) );
			}
		}
		fflush( stdout );
	}

	if ( sw_ctx != NULL ) {
		sw_ctx->msgfunc = switch_msgfunc;
		printf( "hsp3switch: gamepad bridge installed (msgfunc=%p)\n", (void *)sw_ctx->msgfunc );
		fflush( stdout );
	}
}