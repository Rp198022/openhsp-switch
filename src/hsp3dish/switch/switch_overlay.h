//
//	src/hsp3dish/switch/switch_overlay.h
//	Side-panel key hints for the Switch port (see switch_overlay.cpp).
//
//	The game picture is 4:3 on a 16:9 panel, so a letterbox strip is left on
//	either side.  These strips carry the pad's key map; the module owns both
//	the hint textures and the visible/hidden flag.
//
#ifndef SWITCH_OVERLAY_H
#define SWITCH_OVERLAY_H

/*	Flip the hints on/off (called on R + right-stick click from
	switch_input.cpp).  Visible by default; the state is per-run only.		*/
void switch_overlay_toggle( void );
int  switch_overlay_hidden( void );

/*	Draw the two hint panels over the letterbox strips.  Called once per
	presented frame with the window (drawable) size, the x of the game
	picture and its width - i.e. the layout sw_apply_window() has just
	computed.  Draws nothing when the strips are too narrow to hold text.	*/
void switch_overlay_draw( int win_w, int win_h, int origin_x, int game_w );

/*	GL context teardown invalidates every texture name; hgio_resume()
	calls this so the next draw rebuilds the strips instead of sampling
	names the new context never issued.  The old names are NOT deleted
	here - they mean nothing to the new context.						*/
void switch_overlay_ctx_reset( void );

#endif