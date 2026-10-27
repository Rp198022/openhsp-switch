//
//	src/hsp3dish/switch/switch_input.h
//	Switch gamepad -> HSP input bridge (T2.3)
//
#ifndef __HSP3_SWITCH_INPUT_H
#define __HSP3_SWITCH_INPUT_H

/*	Install the Switch input bridge.
	hspctx is the interpreter's HSPCTX* (passed as void* so the header stays
	free of HSP types).  Called from the sock-slot type init, which is the
	one place in the reused Linux hsp3dish.cpp that receives it.	*/
void switch_input_install( void *hspctx );

/*	Sample the pad and inject the resulting key events.  MUST be called once
	per rendered frame; see the header comment in switch_input.cpp for why
	ctx->msgfunc cannot be used for this.								*/
void switch_input_poll( void );

/*	Characters the pad is asking to be typed this frame, up to max of them.
	Returns how many were written.  Elona reads letters and Return out of its
	hidden keylog box only, so buttons that stand for a letter must reach the
	game as a character, not as a scancode: hsp3gr_dish_switch.cpp's
	sw_key_tick() spends one of its paced slots on each character returned
	here, through the same keybuf + func_notice path keys.txt uses.			*/
int switch_input_take_keys( char *out, int max );

/*	State of the pad's own key table, indexed by SDL scancode exactly like the
	reused Linux glue indexes its keys[] table.  hgiox_switch.cpp consults this in
	addition to get_key_state() so the pad still reaches getkey/stick when the
	synthetic SDL events are not consumed by the event loop.				*/
/*	Harness hook: type a scancode instead of a character.  Elona's birth
	screen waits on getkey(13), which a typed CR cannot satisfy. */
void switch_input_inject_scancode( int sc );

int switch_input_key_state( int scancode );
/*	r397: quick-phrase panel for the companion chat.  ZR+ZL opens it when
	data/chat_phrases.txt exists; the system keyboard stays the fallback.
	While it is open every game key is held off.							*/
int  switch_input_phrase_start( void );
int  switch_input_phrase_active( void );
/*	1 while the panel owns the input, including the press that closes it.
	The linux glue asks this before letting a keystroke reach the script. */
int  switch_input_panel_blocks( void );
int  switch_input_phrase_cursor( void );
int  switch_input_phrase_count( void );
const char *switch_input_phrase_text( int i );


/*	DirectInput-shaped pad state for Elona's own gamepad path: bits 0-3 are
	the hat (up, down, left, right) and bits 4..15 the twelve buttons, which its
	config maps to key_cancel / key_enter / ... by index.					*/
unsigned int switch_input_pad_bits( void );

/*	SDL_GetTicks() of the last time any button, stick or trigger moved, 0
	before the first press.  The companion module uses this to tell a player
	who is standing still from a console nobody is holding.				*/
unsigned int switch_input_last_activity( void );

#endif