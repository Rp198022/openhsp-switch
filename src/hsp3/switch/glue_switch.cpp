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
#include <string.h>

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

#ifdef HSPDISH
//	P3: liveness watchdog / statement breadcrumb.
//
//	p3s19..p3s21 all stop printing right after the first hspda call and never
//	reach "hsp3dish: exit code", so the image dies somewhere just past that
//	point - but the last line of the log is only the last thing the *script*
//	happened to print, which is not necessarily where it stopped.  Nothing in
//	the interpreter can report it either: an HSP error would be swallowed by the
//	script's ONERROR handler and would leave error.txt behind, and no error.txt
//	appears.
//
//	p3s22 sampled once every 250 ms and proved the point that mattered: the
//	samples STOP, and the process dies (nxlink had already exited on its own
//	when the 45 s window closed), i.e. a hard fault rather than a hang.  It also
//	showed the run lasts only about two seconds and never begins a third frame.
//
//	p3s23 sampled every 20 ms and printed on change, which is already finer than
//	the statements it is trying to report - it still stopped at the same offset,
//	cs 2821848 (the `extcmd _DMLOADFNAME` command token), because the fault
//	lands within one sampling period of the interpreter leaving it.
//
//	So this version samples every 1 ms and prints the raw words of the pending
//	token as well as its offset, which makes each line self-describing instead of
//	needing an offline decode.  Only one window of the code segment is reported
//	(see GLUE_WATCH_LO): a plain line cap is worthless here - the loading phase
//	and an _HMMBITON burst would consume it long before the interesting part is
//	reached, and the last lines are the ones that matter.
//
//	code_getpcbak() is the supported view of the interpreter's position (see the
//	probe comment in dllshim_switch.cpp: hspctx->mcs is NOT live).  Samples
//	stopping still means the process died at the last offset printed.
//
//	Only the graphics target gets it: the console target does not link pthread
//	and does not run Elona.
//
#include <pthread.h>
#include <time.h>

//	The run reproducibly dies just after the hspda call at cs 2821894, so the
//	neighbourhood of that statement is the only part worth reporting.
//
#define GLUE_WATCH_LO	2800000UL
#define GLUE_WATCH_HI	2900000UL

static HSPCTX *watch_ctx = NULL;
static unsigned long watch_last = (unsigned long)-1;
static int watch_beats = 0;

static void *glue_watchdog( void *arg )
{
	struct timespec ts;
	unsigned long tick = 0;

	(void)arg;
	ts.tv_sec = 0;
	ts.tv_nsec = 1000000L;					// 1 ms
	for (;;) {
		unsigned short *pc;
		unsigned long off;

		nanosleep( &ts, NULL );
		tick++;
		if ( watch_ctx == NULL || watch_ctx->mem_mcs == NULL ) continue;

		pc = code_getpcbak();
		off = (unsigned long)( pc - watch_ctx->mem_mcs );
		if ( off == watch_last ) continue;
		watch_last = off;
		if ( off < GLUE_WATCH_LO || off > GLUE_WATCH_HI ) continue;
		if ( watch_beats >= 1500 ) continue;

		printf( "hsp3switch: ### pc=%lu tok=%#06x/%#06x at %lu ms\n",
			off, (unsigned)pc[0], (unsigned)pc[1], tick );
		fflush( stdout );
		watch_beats++;
	}
	return NULL;
}
#endif

int Hsp3ExtLibInit( HSP3TYPEINFO *info )
{
	//	There is no dlopen on the Switch, so instead of the upstream FFI this is
	//	where the shim is handed the interpreter context.  HSP offers the context
	//	here and nowhere else (linux/hsp3ext_linux.cpp:136).
	//
	dllshim_install( info );

#ifdef HSPDISH
	//	Start the watchdog once the context the interpreter will actually use is
	//	in hand; before this the pointer would be the wrong one on the heap.
	//
	watch_ctx = info->hspctx;
	{
		pthread_t th;
		if ( pthread_create( &th, NULL, glue_watchdog, NULL ) != 0 ) {
			printf( "hsp3switch: ### watchdog thread could not start\n" );
			fflush( stdout );
		}
	}
#endif
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
	//	A script spinning on a missing call issues it hundreds of thousands of
	//	times (the language-screen run logged 1,419,940 of them in 60 seconds).
	//	Printing and flushing every one floods nxlink and slows the run down, so
	//	identical consecutive misses are counted and reported once per 1000.
	//
	{
		static char last[256] = "";
		static unsigned long repeats = 0;

		if ( strcmp( desc, last ) == 0 ) {
			repeats++;
			if ( repeats % 1000 == 0 ) {
				printf( "hsp3switch: ### Unsupported DLL call %s (cmd %d) x%lu\n",
					desc, cmd, repeats );
				fflush( stdout );
			}
		} else {
			strncpy( last, desc, sizeof( last ) - 1 );
			last[sizeof( last ) - 1] = 0;
			repeats = 1;
			printf( "hsp3switch: ### Unsupported DLL call %s (cmd %d)\n", desc, cmd );
			fflush( stdout );
		}
	}

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

/*----------------------------------------------------------------*/
/*	P3 diagnostics													*/
/*----------------------------------------------------------------*/

//	Runs during teardown (code_termfunc sweeps every registered termfunc before
//	the context is destroyed), which is the only place left where the error an
//	ONERROR handler swallowed can still be read.  See dllshim_report_exit().
//
static int glue_exit_report( int option )
{
	(void)option;
#ifdef HSPDISH
	//	Stop sampling before the context is torn down: the watchdog would
	//	otherwise dereference freed memory and turn a clean exit into a crash
	//	that is indistinguishable from the one this build is chasing.
	//
	watch_ctx = NULL;
#endif
	dllshim_report_exit();
	return 0;
}

//	File-access trace.  Elona's startup quits without printing anything, so the
//	files it asks for have to be observed from outside the script.  --wrap=fopen
//	(maintained in makefile.switch) redirects every fopen made by this image,
//	including the ones SDL makes on the runtime's behalf, without touching any
//	upstream file.  Failures are always reported; successes are capped so a
//	long run cannot flood nxlink.
//
//	Error trace.  Elona installs an ONERROR handler, so every error it hits is
//	recorded (hspctx->err), handed to the handler, and then invisible: the script
//	either carries on or ends "normally" with exit code 0.  Its startup now ends
//	with err=7 (Array overflow) and nothing else to go on, while no DLL call and
//	no file open fails, so the only useful next fact is *where* in the script it
//	happened.  Printing at the throw reports that, and the saved PC still points
//	at the command that failed.
//
//	The hook has to be the throw itself.  code_catcherror() would be the obvious
//	place, but its only caller is in the same object file as its definition, and
//	--wrap redirects *undefined* references only (the build with that flag came
//	out byte-identical to the one without it, which is how this was found).  HSP
//	raises every runtime error with `throw HSPERROR`, i.e. through __cxa_throw,
//	which is undefined in our objects and therefore wraps properly.
//
//	HSPERR_INTJUMP and HSPERR_EXITRUN are excluded: the interpreter uses them for
//	control flow (`goto rerun` in code_execcmd), so they fire constantly and are
//	not errors.
//
#define GLUE_MAX_ERRORS 200

extern "C" void __real___cxa_throw( void *thrown, void *tinfo, void (*dest)(void *) );

//	The image runs at a randomised base, so a raw address cannot be looked up in
//	the linker map.  `anchor` is a symbol in this same file: printing the distance
//	from it, plus its own address, makes the map lookup exact.
static int anchor_dummy = 0;

extern "C" void __wrap___cxa_throw( void *thrown, void *tinfo, void (*dest)(void *) )
{
	static int shown = 0;
	void * const anchor = (void *)&anchor_dummy;
	int code = ( thrown != NULL ) ? *(int *)thrown : -1;

	if ( code != HSPERR_NONE && code != HSPERR_INTJUMP && code != HSPERR_EXITRUN ) {
		if ( shown < GLUE_MAX_ERRORS ) {
			shown++;
			//	The thrower's own address is what identifies the site: the
			//	interpreter raises a given error from several places and the
			//	script line is not always available.  Resolve it against
			//	hsp3dish.map, which is built with -Map and shipped inside the
			//	artifact.
			//
			printf( "hsp3switch: throw %d (%s) at line %d of %s ret=%p off=%#lx anchor=%p\n",
				code, hspd_geterror( (HSPERROR)code ),
				code_getdebug_line(), code_getdebug_name(),
				__builtin_return_address( 0 ),
				(unsigned long)( (char *)__builtin_return_address( 0 ) - (char *)anchor ),
				anchor );
			fflush( stdout );
		}
	}
	__real___cxa_throw( thrown, tinfo, dest );
}

extern "C" FILE *__real_fopen( const char *path, const char *mode );

extern "C" FILE *__wrap_fopen( const char *path, const char *mode )
{
	static int shown = 0;
	static char fixed[512];
	const char *use = path;
	FILE *fp;
	size_t i, n;

	if ( path == NULL ) return __real_fopen( path, mode );

	//	The script is a Windows program: it builds paths the Windows way, e.g.
	//	"sdmc:/switch/openhsp\config.txt".  The Switch's devoptab only knows '/',
	//	so every such path looks like a missing file - which is exactly where
	//	Elona gave up: it could not read config.txt, and immediately after came
	//	"err=7 (Array overflow)".  Normalising at the one place every open goes
	//	through fixes all of the script's paths at once, reads and writes alike.
	//
	if ( strchr( path, '\\' ) != NULL ) {
		n = strlen( path );
		if ( n > sizeof( fixed ) - 1 ) n = sizeof( fixed ) - 1;
		for ( i = 0; i < n; i++ ) fixed[i] = ( path[i] == '\\' ) ? '/' : path[i];
		fixed[n] = 0;
		use = fixed;
	}

	fp = __real_fopen( use, mode );
	if ( fp == NULL ) {
		printf( "hsp3file: FAIL '%s' (mode %s)\n", use, mode );
		fflush( stdout );
	} else if ( shown < 150 ) {
		shown++;
		printf( "hsp3file: ok   '%s'\n", use );
		fflush( stdout );
	}
	return fp;
}


#ifdef HSPDISH
/*----------------------------------------------------------------*/
/*	P3 diagnostics: which screen operation the backend refuses	  */
/*----------------------------------------------------------------*/

//	cmdfunc_extcmd() throws HSPERR_UNSUPPORTED_FUNCTION when Bmscr::Copy or
//	Bmscr::Zoom reports a failure, and the compiler merges every identical
//	throw block in that function into one landing pad - so the reported throw
//	address no longer says which command failed.  These two are the only
//	screen operations that can fail that way, they live in hspwnd_dish.swd
//	and are referenced from hsp3gr_dish.swd, so --wrap reaches them without
//	touching upstream.
//
static void glue_screen_op( const char *op, BMSCR *self, BMSCR *src, int r )
{
	static int calls = 0;
	static int fails = 0;

	calls++;
	if ( r != 0 ) fails++;
	if ( ( r != 0 ) || ( calls <= 8 ) ) {
		printf( "hsp3screen: %s -> %d dst type=%d (%dx%d texid=%d) src type=%d (%dx%d texid=%d) [%d calls, %d failed]\n",
			op, r,
			(int)self->type, (int)self->sx, (int)self->sy, self->texid,
			( src != NULL ) ? (int)src->type : -1,
			( src != NULL ) ? (int)src->sx : -1, ( src != NULL ) ? (int)src->sy : -1,
			( src != NULL ) ? src->texid : -1,
			calls, fails );
		fflush( stdout );
	}
}

extern "C" int __real__ZN5Bmscr4CopyEPS_iiii( BMSCR *self, BMSCR *src,
	int xx, int yy, int psx, int psy );

extern "C" int __wrap__ZN5Bmscr4CopyEPS_iiii( BMSCR *self, BMSCR *src,
	int xx, int yy, int psx, int psy )
{
	int r = __real__ZN5Bmscr4CopyEPS_iiii( self, src, xx, yy, psx, psy );
	glue_screen_op( "gcopy", self, src, r );
	return r;
}

extern "C" int __real__ZN5Bmscr4ZoomEiiPS_iiiii( BMSCR *self, int dx, int dy,
	BMSCR *src, int xx, int yy, int psx, int psy, int mode );

extern "C" int __wrap__ZN5Bmscr4ZoomEiiPS_iiiii( BMSCR *self, int dx, int dy,
	BMSCR *src, int xx, int yy, int psx, int psy, int mode )
{
	int r = __real__ZN5Bmscr4ZoomEiiPS_iiiii( self, dx, dy, src, xx, yy, psx, psy, mode );
	glue_screen_op( "gzoom", self, src, r );
	return r;
}
#endif


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
	//	is stubbed out above, so that slot has no term function to offer; id 19
	//	gets the P3 exit report instead, which therefore runs on every teardown
	//	while the context is still alive.
	//
	printf( "hsp3switch: typeinfo id %d termfunc=%p ; gap id %d termfunc=%p -> cleared\n",
		HSP3_TYPE_USER + 1, (void *)info->termfunc,
		HSP3_TYPE_USER, (void *)info[-1].termfunc );
	fflush( stdout );

	info[-1].termfunc = NULL;
	info->termfunc = glue_exit_report;

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