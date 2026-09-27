//
//	src/hsp3dish/switch/main_dish.cpp
//	Entry point for the Nintendo Switch build of the OpenHSP *graphical* runtime
//	(hsp3dish: SDL2 + GLES, T2.3).
//
//	This replaces src/hsp3dish/emscripten/main.cpp, which is a command-line
//	front end (it parses argv and resolves the module path with getcwd) and
//	cannot be used on the Switch.  The platform glue itself
//	(src/hsp3dish/linux/hsp3dish.cpp) is reused unmodified, exactly the way T2.2
//	reused hsp3cl.cpp for the console backend.
//
//	Two rules from docs/R11_report.md §5.3 shape this file:
//
//	  * libnx's console and the GPU are mutually exclusive, so unlike the T2.2
//	    console entry point this one never calls consoleInit().  All diagnostics
//	    go out over nxlink, and the visible evidence is the rendered frame.
//	  * because there is then no on-screen channel, a boot log is also written
//	    next to the .ax on the SD card, so a failure that happens without an
//	    nxlink host still leaves a trace.
//
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include <switch.h>
#include <switch/runtime/nxlink.h>

#include "../linux/hsp3dish.h"

/*	The .ax to execute and any data files are resolved relative to this
	directory, the same way the PC build resolves them relative to the cwd.	*/
#define HSP3SWITCH_APPDIR	"sdmc:/switch/openhsp"
#define HSP3SWITCH_STARTAX	"start.ax"
#define HSP3SWITCH_LOG		"hsp3dish_boot.log"

static FILE *sw_log = NULL;

static void sw_log_open( void )
{
	sw_log = fopen( HSP3SWITCH_LOG, "w" );
}

static void sw_say( const char *fmt, ... )
{
	va_list ap;

	va_start( ap, fmt );
	vprintf( fmt, ap );
	va_end( ap );
	fflush( stdout );			/* nxlink socket output is fully buffered */

	if ( sw_log != NULL ) {
		va_start( ap, fmt );
		vfprintf( sw_log, fmt, ap );
		va_end( ap );
		fflush( sw_log );
	}
}

int main( int argc, char *argv[] )
{
	int res;
	int nxlink_fd;
	char *startfile;

	/*	nxlinkStdio() is what makes printf() reach the host; without a host it
		returns < 0 immediately (same pattern as the P1 demo and the R11 probe). */
	//
	socketInitializeDefault();
	nxlink_fd = nxlinkStdio();

	printf( "hsp3dish: boot (nxlink fd = %d)\n", nxlink_fd );
	fflush( stdout );

	if ( chdir( HSP3SWITCH_APPDIR ) != 0 ) {
		printf( "hsp3dish: cannot enter %s\n", HSP3SWITCH_APPDIR );
		fflush( stdout );
	}

	/*	Switch/libnx gives the process no environment block, so getenv("HOME")
		returns NULL and hsp3ext_linux.cpp:63 hands that to sbStrCopy(), whose
		strlen(NULL) aborted the T2.2 run at its very first hsp3cl_init().
		Seat HOME here rather than editing the upstream file.				*/
	//
	setenv( "HOME", HSP3SWITCH_APPDIR, 1 );

	sw_log_open();
	sw_say( "hsp3dish: boot (nxlink fd = %d)\n", nxlink_fd );
	sw_say( "hsp3dish: HOME=%s\n", getenv( "HOME" ) ? getenv( "HOME" ) : "(null)" );

	startfile = (char *)HSP3SWITCH_STARTAX;
	if ( ( argc > 1 ) && ( argv[1] != NULL ) && ( argv[1][0] != 0 ) ) {
		startfile = argv[1];
	}
	sw_say( "hsp3dish: start file = %s\n", startfile );

	hsp3dish_cmdline( "" );
	hsp3dish_modname( (char *)( HSP3SWITCH_APPDIR "/hsp3dish.nro" ) );

	sw_say( "hsp3dish: calling hsp3dish_init\n" );
	res = hsp3dish_init( startfile );
	sw_say( "hsp3dish: hsp3dish_init -> %d\n", res );

	if ( res == 0 ) {
		hsp3dish_option( 0 );
		sw_say( "hsp3dish: calling hsp3dish_exec\n" );
		res = hsp3dish_exec();
		sw_say( "hsp3dish: exit code %d\n", res );
	}

	if ( sw_log != NULL ) {
		fclose( sw_log );
		sw_log = NULL;
	}

	socketExit();
	return res;
}