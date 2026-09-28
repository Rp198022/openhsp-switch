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

/*	State of the pad's own key table, indexed by SDL scancode exactly like the
	reused Linux glue indexes its keys[] table.  hgiox_switch.cpp consults this in
	addition to get_key_state() so the pad still reaches getkey/stick when the
	synthetic SDL events are not consumed by the event loop.				*/
int switch_input_key_state( int scancode );

#endif