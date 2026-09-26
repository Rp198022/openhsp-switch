//
//	hsp3/switch/main_switch.cpp
//	Entry point for the Nintendo Switch build of the OpenHSP runtime (T2.2).
//
//	This replaces src/hsp3/linux/main.cpp, which is a command-line front end and
//	cannot be used on the Switch.  The interpreter driver itself (hsp3cl.cpp) is
//	reused unmodified.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>
#include <switch/runtime/nxlink.h>

#include "../hsp3config.h"
#include "../linux/hsp3cl.h"

//	The .ax to execute and any data files are resolved relative to this
//	directory, the same way the PC build resolves them relative to the cwd.
//
#define HSP3SWITCH_APPDIR	"sdmc:/switch/openhsp"
#define HSP3SWITCH_STARTAX	"start.ax"

int main( int argc, char *argv[] )
{
	int res;
	int nxlink_fd;

	//	libnx does not redirect stdout on its own: nxlinkStdio() is what makes
	//	printf() reach the nxlink host.  (Same pattern the P1 SDL2 demo used.)
	//
	socketInitializeDefault();
	nxlink_fd = nxlinkStdio();

	if ( nxlink_fd < 0 ) {
		//	No nxlink host - fall back to the on-screen console so the run is
		//	still observable when the .nro is started from hbmenu.
		//
		consoleInit( NULL );
	}

	printf( "hsp3switch: boot\n" );
	fflush( stdout );

	if ( chdir( HSP3SWITCH_APPDIR ) != 0 ) {
		printf( "hsp3switch: cannot enter %s\n", HSP3SWITCH_APPDIR );
		fflush( stdout );
	}

	char *startfile = (char *)HSP3SWITCH_STARTAX;
	if ( ( argc > 1 ) && ( argv[1] != NULL ) && ( argv[1][0] != 0 ) ) {
		startfile = argv[1];
	}
	printf( "hsp3switch: start file = %s\n", startfile );
	fflush( stdout );

	hsp3cl_cmdline( "" );
	hsp3cl_modname( (char *)( HSP3SWITCH_APPDIR "/hsp3switch.nro" ) );

	printf( "hsp3switch: calling hsp3cl_init\n" );
	fflush( stdout );
	res = hsp3cl_init( startfile );
	printf( "hsp3switch: hsp3cl_init -> %d\n", res );
	fflush( stdout );
	if ( res ) {
		printf( "hsp3switch: startup failed (%d)\n", res );
		fflush( stdout );
	} else {
		hsp3cl_option( 0 );
		printf( "hsp3switch: calling hsp3cl_exec\n" );
		fflush( stdout );
		res = hsp3cl_exec();
		printf( "hsp3switch: exit code %d\n", res );
		fflush( stdout );
	}

	//	When running from hbmenu there is no host to quit for us, so keep the
	//	finished screen up until the user closes the applet.
	//
	if ( nxlink_fd < 0 ) {
		while ( appletMainLoop() ) {
			consoleUpdate( NULL );
			svcSleepThread( 20000000L );		// 20 ms
		}
		consoleExit( NULL );
	}

	socketExit();
	return res;
}