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
#include <SDL2/SDL_ttf.h>

#include "../linux/hsp3dish.h"

/*	Declared here rather than by including hsp3ext_linux.h: the signature is the
	one that header publishes, and this keeps hsp3code.h out of this file.
	Used to show what dirinfo(1) actually yields at the moment hgio_init()
	builds the font path from it.											*/
extern char *hsp3ext_getdir( int id );

/*	The .ax to execute and any data files are resolved relative to this
	directory, the same way the PC build resolves them relative to the cwd.	*/
#define HSP3SWITCH_APPDIR	"sdmc:/switch/openhsp"
#define HSP3SWITCH_STARTAX	"start.ax"
/*	Project-local convention: the graphical runtime prefers its own script name
	so a small T2.3/T2.4 test can sit next to Elona's start.ax on the card
	instead of overwriting it.  Falls back to the conventional name.		*/
#define HSP3SWITCH_DISHAX	"hsp3dish.ax"
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

/*	A path being readable is not the same as its bytes being a font that
	freetype will accept, and "Init:TTF_OpenFont error" on its own says neither
	which one failed nor why.  So the size and the sfnt signature are reported
	first, then the exact call fontsystem.cpp makes is reproduced here and its
	TTF_GetError() printed - the log then carries the library's own reason.
	Both are done before hsp3dish_init(), so they cannot disturb the run.	*/
static void sw_probe_font( const char *path )
{
	unsigned char hdr[4];
	FILE *fp;
	long sz;

	fp = fopen( path, "rb" );
	if ( fp == NULL ) {
		sw_say( "hsp3dish: file %-38s -> open FAILED\n", path );
		return;
	}
	memset( hdr, 0, sizeof( hdr ) );
	(void)fread( hdr, 1, 4, fp );
	fseek( fp, 0, SEEK_END );
	sz = ftell( fp );
	fclose( fp );
	sw_say( "hsp3dish: file %-38s -> %ld bytes, magic %02X%02X%02X%02X\n",
		path, sz, hdr[0], hdr[1], hdr[2], hdr[3] );
}

static void sw_probe_ttf( const char *path )
{
	TTF_Font *f;

	f = TTF_OpenFont( path, 18 );
	sw_say( "hsp3dish: TTF_OpenFont(\"%s\",18) -> %p : %s\n",
		path, (void *)f, TTF_GetError() );
	if ( f != NULL ) {
		TTF_CloseFont( f );
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

	startfile = (char *)sw_pick_startfile();
	if ( ( argc > 1 ) && ( argv[1] != NULL ) && ( argv[1][0] != 0 ) ) {
		startfile = argv[1];
	}
	sw_say( "hsp3dish: start file = %s\n", startfile );

	hsp3dish_cmdline( "" );
	/*	dirinfo(1) means "the directory the executable lives in", and hgio_init()
		feeds it straight into the TTF font path by appending "/ipaexg.ttf".  It
		is kept relative (".") so it resolves through the process cwd - which the
		chdir above already set to the app directory, i.e. the very same form the
		.ax lookup uses.												*/
	hsp3dish_modname( (char *)"." );

	sw_probe_font( HSP3SWITCH_DISHAX );
	sw_probe_font( "./ipaexg.ttf" );
	sw_probe_font( HSP3SWITCH_APPDIR "/ipaexg.ttf" );

	sw_say( "hsp3dish: TTF_Init() -> %d\n", TTF_Init() );
	sw_probe_ttf( "./ipaexg.ttf" );
	sw_probe_ttf( HSP3SWITCH_APPDIR "/ipaexg.ttf" );

	sw_say( "hsp3dish: calling hsp3dish_init\n" );
	sw_say( "hsp3dish: before init, getdir(1) = \"%s\"\n", hsp3ext_getdir( 1 ) );
	res = hsp3dish_init( startfile );
	sw_say( "hsp3dish: hsp3dish_init -> %d\n", res );

	/*	The two probes above open the font fine, while the call hgio_init() makes
		on the very same path fails - and the only thing that happens in between
		is SDL_Init(SDL_INIT_VIDEO).  These three lines tie the failure down: the
		string dirinfo(1) really handed to hgio_init(), SDL_ttf's own reason for
		the last failure (the error text survives until the next failing call),
		and the identical probe re-run now that the video subsystem is up.	*/
	sw_say( "hsp3dish: after init, getdir(1) = \"%s\"\n", hsp3ext_getdir( 1 ) );
	sw_say( "hsp3dish: after init, TTF_GetError() = \"%s\"\n", TTF_GetError() );
	sw_probe_ttf( "./ipaexg.ttf" );
	sw_probe_ttf( "/ipaexg.ttf" );

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