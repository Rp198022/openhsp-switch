//
//	hsp3/switch/glue_switch.cpp
//	Nintendo Switch platform glue for the "interpreter core + empty backend" step (T2.2)
//
//	This file replaces three Linux-only translation units that cannot be compiled
//	for the Switch, while leaving every upstream file untouched:
//
//	  - src/hsp3/linux/devctrl_io.cpp      hardcodes `#define USE_GPIOD` -> needs libgpiod
//	  - src/hsp3/linux/hsp3extlib_ffi.cpp  needs <ffi.h> and <dlfcn.h>
//	  - src/hsp3/linux/hsp3ext_sock.cpp    BSD socket command set (not needed for T2.2)
//
//	Everything below is a deliberate stub.  The point of T2.2 is to prove that the
//	interpreter core executes bytecode on real hardware; any .ax that really calls
//	a DLL/plugin or a socket command must fail loudly, and that failure is exactly
//	the "first missing call" evidence T2.4 asks for.
//
#include <stdio.h>
#include <stdlib.h>

#include "../hsp3config.h"
#include "../hsp3code.h"
#include "../hsp3debug.h"
#include "../supio.h"
#include "../strbuf.h"
#include "../hsp3ext.h"

//	HSP3DEVINFO lives here.  On Linux the definition is pulled in indirectly by
//	hsp3gr_linux.cpp; devctrl_io.h itself does not include it.
#include "../../hsp3dish/hspwnd_dish.h"

#include "../linux/hsp3extlib_ffi.h"
#include "../linux/hsp3ext_sock.h"
#include "../linux/devctrl_io.h"

//	P3: the (library, function) -> implementation table plus the parameter
//	marshaller that the missing FFI would have provided.  See the header.
#include "dllshim_switch.h"

#ifdef HSPDISH
//	T2.3 - graphical build only: gamepad -> keyboard bridge (see below).
#include "../../hsp3dish/switch/switch_input.h"
#endif

/*----------------------------------------------------------------*/
/*	DEVINFO - replaces devctrl_io.cpp							  */
/*----------------------------------------------------------------*/

static int glue_devprm( char *name, char *value )
{
	return -1;
}

static int glue_devcontrol( char *cmd, int p1, int p2, int p3 )
{
	return -1;
}

static int *glue_devinfoi( char *name, int *size )
{
	*size = -1;
	return NULL;
}

static char *glue_devinfo( char *name )
{
	return NULL;
}

void hsp3dish_setdevinfo_io( HSP3DEVINFO *devinfo )
{
	//	Switch has no GPIO/I2C control channel (the Linux build uses libgpiod).
	//
	devinfo->devname = "switch";
	devinfo->error = "";
	devinfo->devprm = glue_devprm;
	devinfo->devcontrol = glue_devcontrol;
	devinfo->devinfo = glue_devinfo;
	devinfo->devinfoi = glue_devinfoi;
}

void hsp3dish_termdevinfo_io( void )
{
}

/*----------------------------------------------------------------*/
/*	Plugin / DLL subsystem - replaces hsp3extlib_ffi.cpp		  */
/*----------------------------------------------------------------*/

int Hsp3ExtLibInit( HSP3TYPEINFO *info )
{
	//	There is no dlopen on the Switch, so instead of the upstream FFI this is
	//	where the shim is handed the interpreter context.  HSP offers the context
	//	here and nowhere else (linux/hsp3ext_linux.cpp:136).
	//
	dllshim_install( info );
	return 0;
}

int exec_dllcmd( int cmd, int mask )
{
	//	Reference failure path, linux/hsp3extlib_ffi.cpp:711-727: a DLL command
	//	handler first advances past the parameter tokens with code_next(), and
	//	when the function cannot be bound it raises error 38 ("DLL call failed").
	//
	//	Returning -1 instead - which this stub used to do - is not a loud failure.
	//	Both callers drop the return value: linux/hsp3ext_linux.cpp:100 for the
	//	function form, and cmdfunc_dllcmd() below for the statement form.  HSP
	//	therefore recorded a *successful* call and carried on, which is how Elona
	//	1.90's start.ax reached its main loop and sat there on a black screen
	//	instead of stopping at its first missing call.  It also skipped
	//	code_next(), which the reference calls unconditionally before it throws.
	//
	char desc[256];
	int runmode;

	code_next();

	runmode = dllshim_exec( cmd, mask, desc, (int)sizeof( desc ) );
	if ( runmode >= 0 ) return runmode;

	//	Still a loud failure, but now it names the exact library and function
	//	instead of only the PRM index - that is what makes the "run, read the
	//	missing call, add the next stub" loop cheap (no lookup against
	//	_scratch/inv232.txt each round).
	//
	printf( "hsp3switch: ### Unsupported DLL call %s (cmd %d)\n", desc, cmd );
	fflush( stdout );

	throw ( HSPERR_DLL_ERROR );
}

int cmdfunc_dllcmd( int cmd )
{
	//	cmdfunc : TYPE_DLLCMD (statement form) - the reference simply forwards.
	//
	return exec_dllcmd( cmd, STRUCTDAT_OT_STATEMENT );
}

namespace hsp3 {

CDllManager::CDllManager() : mError( NULL )
{
}

CDllManager::~CDllManager()
{
}

HANDLE_MODULE CDllManager::load_library( const char *lpFileName )
{
	return NULL;
}

bool CDllManager::free_library( HANDLE_MODULE hModule )
{
	return false;
}

bool CDllManager::free_all_library()
{
	return true;
}

HANDLE_MODULE CDllManager::get_error() const
{
	return mError;
}

}

hsp3::CDllManager & DllManager()
{
	static hsp3::CDllManager manager;
	return manager;
}

/*----------------------------------------------------------------*/
/*	Socket command set - replaces hsp3ext_sock.cpp				  */
/*----------------------------------------------------------------*/

void hsp3typeinit_sock_extcmd( HSP3TYPEINFO *info )
{
	//	Network commands are not part of the T2.2 empty backend: registering
	//	nothing makes such commands report "not found" instead of silently
	//	doing nothing.
	//
	//	Slot bookkeeping - this is where the interpreter's typeinfo table gets
	//	its only uninitialised entry.  code_init() sizes the table to
	//	HSP3_FUNC_MAX (18) entries and default-initialises ids 0..17.  Later
	//	hsp3cl_init() asks code_gettypeinfo() for TYPE_USERDEF+1 (=19), which
	//	grows the table to 20 entries but calls hsp3typeinit_default() for the
	//	*requested* id only - so id 18 (HSP3_TYPE_USER, the gap between the two)
	//	keeps whatever BlockRealloc left in the newly added memory.
	//
	//	At teardown code_termfunc() sweeps tinfo_cur-1 .. 0 and invokes every
	//	non-NULL termfunc, so that stale value was entered and the process died
	//	on exit: fatal 2168-0001, LR inside code_termfunc(), PC taken from the
	//	stale pointer, X0=0 (the termfunc argument).  It is heap-content
	//	dependent, which is why running from hbmenu (consoleInit path) survived
	//	while every nxlink run crashed.
	//
	//	The caller hands us id 19, so id 18 is `info[-1]`.  The sock command set
	//	is stubbed out above, so this slot has no term function to offer.
	//
	printf( "hsp3switch: typeinfo id %d termfunc=%p ; gap id %d termfunc=%p -> cleared\n",
		HSP3_TYPE_USER + 1, (void *)info->termfunc,
		HSP3_TYPE_USER, (void *)info[-1].termfunc );
	fflush( stdout );

	info[-1].termfunc = NULL;

#ifdef HSPDISH
	//	T2.3: this is the one point where the reused Linux platform glue
	//	(src/hsp3dish/linux/hsp3dish.cpp:780-785) hands the interpreter context
	//	back to us, so it is where the gamepad bridge is opened.  The bridge
	//	cannot hang its per-frame work off ctx->msgfunc - that is entered once
	//	and loops internally - so it is sampled from the GL shim's glClear
	//	instead (switch_input.cpp explains both halves).
	//
	switch_input_install( info->hspctx );
#endif
}