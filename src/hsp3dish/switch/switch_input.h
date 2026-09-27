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

#endif