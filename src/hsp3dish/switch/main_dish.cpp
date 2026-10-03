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
#include <SDL2/SDL.h>
/*	Declared where each one lives upstream.  Kept local so this file does
	not have to pull in the emscripten graphics header on a Switch build. */
int hgio_file_exist( char *fname );
char *hgio_getstorage( char *fname );
int hsp3_flength( char *name );
char *dpm_readalloc( char *fname );

#include "../linux/hsp3dish.h"

/*	The .ax to execute and any data files are resolved relative to this
	directory, the same way the PC build resolves them relative to the cwd.	*/
#define HSP3SWITCH_APPDIR	"sdmc:/switch/openhsp"
#define HSP3SWITCH_STARTAX	"start.ax"
/*	Project-local convention: the graphical runtime prefers its own script name
	so a small T2.3/T2.4 test can sit next to Elona's start.ax on the card
	instead of overwriting it.  Falls back to the conventional name.		*/
#define HSP3SWITCH_DISHAX	"hsp3dish.ax"
#define HSP3SWITCH_LOG		"hsp3dish_boot.log"
/*	What hsp3dish_modname() must be given: the module FILE, not its directory.
	hsp3ext_linux.cpp's InitSystemInformation() derives the directory by cutting
	the last path component off it, and hgio_init() then reads that back through
	hsp3ext_getdir(1) to build the TTF font path as <dir> + "/ipaexg.ttf".	*/
#define HSP3SWITCH_NROPATH	HSP3SWITCH_APPDIR "/hsp3dish.nro"

static FILE *sw_log = NULL;

/*  Point stdout at the boot log rather than holding a second handle to
    it.  There is no console in this build (see the header comment) and
    nxlink only exists when a host is listening, so a plain printf() -
    which is what every diagnostic outside this file uses, including the
    DLL shim's reason for refusing a call - wrote nowhere at all.  A
    failure inside the shim could therefore only be seen on hardware
    attached to nxlink; a card run, or an emulator run, lost it.  */
static void sw_log_open( void )
{
	sw_log = freopen( HSP3SWITCH_LOG, "w", stdout );
}

static void sw_say( const char *fmt, ... )
{
	va_list ap;

	va_start( ap, fmt );
	vprintf( fmt, ap );
	va_end( ap );
	fflush( stdout );			/* stdout is the boot log file now */
}

static const char *sw_pick_startfile( void )
{
	const char *cand[2];
	FILE *fp;
	int i;

	cand[0] = HSP3SWITCH_DISHAX;
	cand[1] = HSP3SWITCH_STARTAX;
	for ( i = 0; i < 2; i++ ) {
		fp = fopen( cand[i], "rb" );
		if ( fp != NULL ) {
			fclose( fp );
			return cand[i];
		}
	}
	return HSP3SWITCH_STARTAX;
}

static void sw_probe_open( const char *path )
{
	FILE *fp = fopen( path, "rb" );

	sw_say( "hsp3dish: probe %-40s -> %s\n", path, ( fp != NULL ) ? "ok" : "FAILED" );
	if ( fp != NULL ) {
		fclose( fp );
	}
}

/*	Called from hsp3dish_init()'s own failure paths.  They report through
	printf, which on the Switch is a debug device nobody is listening to, so
	the value of each stage would otherwise be invisible.  */
void sw_boot_trace( const char *stage, int value )
{
	sw_say( "hsp3dish: init stage %-8s -> %d\n", stage, value );
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

	/*	A host on the other end of nxlink wants the diagnostics live, and
		freopen() would close the very socket it is listening on - that is what
		made an earlier hardware run go silent the moment it booted.  Only fall
		back to the on-card log when there is nobody listening.			*/
	if ( nxlink_fd < 0 ) sw_log_open();
	sw_say( "hsp3dish: boot (nxlink fd = %d)\n", nxlink_fd );
	sw_say( "hsp3dish: HOME=%s\n", getenv( "HOME" ) ? getenv( "HOME" ) : "(null)" );

	startfile = (char *)sw_pick_startfile();
	if ( ( argc > 1 ) && ( argv[1] != NULL ) && ( argv[1][0] != 0 ) ) {
		startfile = argv[1];
	}
	sw_say( "hsp3dish: start file = %s\n", startfile );

	hsp3dish_cmdline( "" );
	/*	This is the module file path, and it must look like one.  dirinfo(1)
		("the directory the executable lives in") is what hgio_init() appends
		"/ipaexg.ttf" to, but hsp3ext_linux.cpp derives it from this value by
		cutting the last path component off - so a bare directory does not
		survive it.  Both earlier attempts failed that way on hardware:

			"sdmc:/switch/openhsp"  ->  "sdmc:/switch"        (wrong dir)
			"."                     ->  ""                    (no separator)

		and the font path became sdmc:/switch/ipaexg.ttf and /ipaexg.ttf, which
		do not exist - TTF_OpenFont failed with "Couldn't open".  The .nro name
		makes the cut yield exactly the app directory.						*/
	hsp3dish_modname( (char *)HSP3SWITCH_NROPATH );

	sw_probe_open( HSP3SWITCH_DISHAX );
	sw_probe_open( "./ipaexg.ttf" );
	sw_probe_open( HSP3SWITCH_APPDIR "/ipaexg.ttf" );

	sw_probe_open( HSP3SWITCH_STARTAX );
		/*	fopen("start.ax") working says nothing about the file layer the
			engine actually uses, so ask that one directly. */
		{
			const char *sf = HSP3SWITCH_STARTAX;
			sw_say( "hsp3dish: probe hgio_file_exist('%s')  -> %d\n",
				sf, hgio_file_exist( (char *)sf ) );
			sw_say( "hsp3dish: probe hgio_getstorage('%s') -> '%s'\n",
				sf, hgio_getstorage( (char *)sf ) );
			sw_say( "hsp3dish: probe hsp3_flength('%s')     -> %d\n",
				sf, hsp3_flength( (char *)sf ) );
			sw_say( "hsp3dish: probe SDL_Init(VIDEO)        -> %d [%s]\n",
				SDL_Init( SDL_INIT_VIDEO ), SDL_GetError() );
		}


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
		fflush( sw_log );
		fclose( sw_log );
		sw_log = NULL;
	}

	socketExit();
	return res;
}