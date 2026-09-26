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
	//	External DLL / plugin loading is not supported on the Switch (T2.2).
	//
	return 0;
}

int cmdfunc_dllcmd( int cmd )
{
	return -1;
}

int exec_dllcmd( int cmd, int mask )
{
	return -1;
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
}