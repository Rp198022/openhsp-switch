//
//	hsp3/switch/dllshim_switch.cpp
//	Nintendo Switch DLL/plugin shim	(P3, step 1)
//
//	Replaces the half of src/hsp3/linux/hsp3extlib_ffi.cpp that actually calls
//	into an external library.  The lookup is by (library name, function name)
//	instead of dlopen/dlsym, and the call is a plain C function pointer instead
//	of libffi.  Everything else - which parameter types exist, how they are read
//	off the bytecode stream, and what happens to the return value - follows the
//	reference implementation, so a function behaves the same here as it would on
//	the Linux/Windows builds.
//
//	See dllshim_switch.h for the design rationale.
//
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "../hsp3config.h"
#include "../hsp3code.h"
#include "../hsp3debug.h"
#include "../hsp3struct.h"
#include "../hspvar_core.h"
#include "../strbuf.h"

#include "dllshim_switch.h"

static HSPCTX *hspctx = NULL;		// Current Context
static HSPEXINFO *exinfo = NULL;	// Info for Plugins
static PVal **pmpval = NULL;		// Master PVal (points at code_get's temp var)

//	The reference caps parameter lists at 16 as well (ExitFunc(),
//	hsp3extlib_ffi.cpp:242).  No dependency declared by Elona exceeds 6.
//
#define DLLSHIM_MAX_ARGS 16

/*----------------------------------------------------------------*/
/*	One resolved argument											*/
/*----------------------------------------------------------------*/

struct DllArgValue {
	int		type;		// MPTYPE_*
	int		ival;		// MPTYPE_INUM / MPTYPE_FLEXSPTR holding an int
	double	dval;		// MPTYPE_DNUM
	float	fval;		// MPTYPE_FLOAT
	void	*ptr;		// pointer or string data
	char	*owned;		// local copy to release after the call (or NULL)
};

typedef int (*DllImplFunc)( const DllArgValue *args, int argc );

/*----------------------------------------------------------------*/
/*	Implementations												*/
/*----------------------------------------------------------------*/

//	exrand.dll - the extended RNG plugin Elona uses instead of HSP's built-in
//	randomize/rnd.  randomize() receives four 32-bit seed words from the script
//	and keeps them as the generator state; _exrand_rnd@16 is the function that
//	consumes that state, and it is deliberately not implemented yet - the
//	"unsupported DLL call exrand.dll!_exrand_rnd@16" line it produces on the
//	next run is how its exact calling contract gets pinned down.
//
static unsigned int exrand_seed[4] = { 0, 0, 0, 0 };

static int impl_exrand_randomize( const DllArgValue *args, int argc )
{
	int i;
	for ( i = 0; i < 4; i++ ) {
		exrand_seed[i] = ( i < argc ) ? (unsigned int)args[i].ival : 0u;
	}
	return 0;
}

//	kernel32.dll - GetLastError() -> DWORD.
//
//	This was the second call the device reported (see T2.4_report.md §3.4.2 and
//	§3.4.5: `exec_dllcmd 0` then `48`/`53` twice).  Nothing in this shim sets a
//	thread error, so "no error" - 0, ERROR_SUCCESS - is the only correct answer.
//	Two STRUCTDATs declare it: as a function and as a statement.
//
static int impl_GetLastError( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 0;
}

//	kernel32.dll - CreateMutexA( LPSECURITY_ATTRIBUTES, BOOL, LPCSTR ) -> HANDLE.
//
//	Elona creates a named mutex only to notice a second copy of itself starting;
//	the port is single-instance by construction, so a non-NULL handle is always
//	the right answer - "this is the first instance, carry on".
//
static int impl_CreateMutexA( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 1;		//	non-NULL handle
}

//	winmm.dll - the multimedia timer trio.  This was the third dependency the
//	device reported (`winmm.dll!timeBeginPeriod`, see the P3 report): Elona opens
//	the high-resolution timer before it starts measuring frame times.
//
//	timeBeginPeriod/timeEndPeriod only ask Windows for a finer scheduler tick;
//	the Switch's resolution is fixed by the kernel/vsync, so there is nothing to
//	do but report success (0 == TIMERR_NOERROR).  Their argument is the period in
//	milliseconds and is of no use here.
//
static int impl_timeBeginPeriod( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 0;
}

static int impl_timeEndPeriod( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 0;
}

//	timeGetTime() -> DWORD, milliseconds since it was first called (Windows
//	counts from system start; the origin does not matter, only differences do).
//
//	The clock source is the same one the runtime's own tick already uses on this
//	device - clock_gettime( CLOCK_REALTIME ) truncated to milliseconds, exactly
//	as hgio_gettick() does in src/hsp3dish/emscripten/hgiox.cpp:1808, which the
//	T2.3 frame-rate measurements exercised.  A baseline is subtracted so the
//	result starts near zero and climbs, instead of wrapping the epoch's ~1.7e12
//	ms through a 32-bit int.
//
static int impl_timeGetTime( const DllArgValue *args, int argc )
{
	static long long base = -1;
	timespec ts;
	long long now;

	(void)args;
	(void)argc;

	clock_gettime( CLOCK_REALTIME, &ts );
	now = (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	if ( base < 0 ) base = now;
	return (int)( now - base );
}

//	hmm.dll - the Windows audio/input library Elona links against (DirectSound,
//	DirectMusic, DirectInput).  The Switch has none of those, and this port has
//	no audio yet, so the whole family is a no-op that reports success.
//
//	Reporting failure is NOT an option here.  HSP turns a failed DLL call into
//	error 38, Elona's ONERROR handler swallows it, and its startup then loops on
//	_DMEND@16 forever: the device run that first reached the game produced
//	thousands of "Unsupported DLL call hmm.dll!_DMEND@16" lines in 40 seconds,
//	which is also how this family was identified as the current blocker.
//
static int impl_hmm_ok( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 0;
}

//	The two loaders hand back a handle that the script passes to play/stop
//	later.  0 would mean "nothing loaded", so give them a plausible one.
//
static int impl_hmm_load( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 1;
}

//	_HMMBITON@16 / _HMMBITOFF@16 / _HMMBITCHECK@16 are real bit twiddling on the
//	variable the script passes in - worth doing exactly, because Elona keeps
//	capability/state flags in it.
//
static int impl_hmm_biton( const DllArgValue *args, int argc )
{
	if ( argc < 2 || args[0].ptr == NULL ) return 0;
	*(int *)args[0].ptr |= args[1].ival;
	return 0;
}

static int impl_hmm_bitoff( const DllArgValue *args, int argc )
{
	if ( argc < 2 || args[0].ptr == NULL ) return 0;
	*(int *)args[0].ptr &= ~args[1].ival;
	return 0;
}

static int impl_hmm_bitcheck( const DllArgValue *args, int argc )
{
	if ( argc < 2 || args[0].ptr == NULL ) return 0;
	return ( *(int *)args[0].ptr & args[1].ival ) ? 1 : 0;
}

//	_DIGETJOYNUM@16 / _DIGETJOYSTATE@16 - DirectInput enumeration.  The pad is
//	read through the Switch bridge instead (switch_input.cpp), so report none.
//
static int impl_hmm_joynum( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 0;
}

static int impl_hmm_joystate( const DllArgValue *args, int argc )
{
	if ( argc >= 1 && args[0].ptr != NULL ) *(int *)args[0].ptr = 0;
	return 0;
}

/*----------------------------------------------------------------*/
/*	Dispatch table													*/
/*----------------------------------------------------------------*/

//	Names must match the .ax verbatim (see _scratch/inv232.txt).  Keep the list
//	short and grow it one loud failure at a time.
//
typedef struct {
	const char *lib;
	const char *func;
	DllImplFunc impl;
} DllImplEntry;

static const DllImplEntry impl_table[] = {
	{ "exrand.dll",		"_exrand_randomize@16",	impl_exrand_randomize },
	{ "kernel32.dll",	"GetLastError",			impl_GetLastError },
	{ "kernel32.dll",	"CreateMutexA",			impl_CreateMutexA },
	{ "winmm.dll",		"timeBeginPeriod",		impl_timeBeginPeriod },
	{ "winmm.dll",		"timeEndPeriod",		impl_timeEndPeriod },
	{ "winmm.dll",		"timeGetTime",			impl_timeGetTime },

	//	hmm.dll - silent-but-successful audio/input (see impl_hmm_ok above).
	{ "hmm.dll",		"_DSINIT@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DSEND@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DSRELEASE@16",		impl_hmm_ok },
	{ "hmm.dll",		"_DSLOADFNAME@16",		impl_hmm_load },
	{ "hmm.dll",		"_DSPLAY@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DSSTOP@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DSSETVOLUME@16",		impl_hmm_ok },
	{ "hmm.dll",		"_DSGETMASTERVOLUME@16",impl_hmm_ok },
	{ "hmm.dll",		"_CHECKPLAY@16",		impl_hmm_ok },
	{ "hmm.dll",		"_DMINIT@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DMEND@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DMLOADFNAME@16",		impl_hmm_load },
	{ "hmm.dll",		"_DMPLAY@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DMSTOP@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DIINIT@16",			impl_hmm_ok },
	{ "hmm.dll",		"_DIGETJOYNUM@16",		impl_hmm_joynum },
	{ "hmm.dll",		"_DIGETJOYSTATE@16",	impl_hmm_joystate },
	{ "hmm.dll",		"_HMMBITON@16",			impl_hmm_biton },
	{ "hmm.dll",		"_HMMBITOFF@16",		impl_hmm_bitoff },
	{ "hmm.dll",		"_HMMBITCHECK@16",		impl_hmm_bitcheck },
};

static const DllImplEntry *find_entry( const STRUCTDAT *st )
{
	LIBDAT *lib;
	const char *libname;
	const char *funcname;
	size_t i;

	if ( st->index < 0 ) return NULL;			// not a library function
	lib = &hspctx->mem_linfo[ st->index ];
	if ( lib->nameidx < 0 || st->nameidx < 0 ) return NULL;

	libname = code_strp( lib->nameidx );
	funcname = code_strp( st->nameidx );
	for ( i = 0; i < sizeof(impl_table) / sizeof(impl_table[0]); i++ ) {
		if ( strcmp( impl_table[i].lib, libname ) == 0 &&
			 strcmp( impl_table[i].func, funcname ) == 0 ) {
			return &impl_table[i];
		}
	}
	return NULL;
}

/*----------------------------------------------------------------*/
/*	Parameter marshalling											*/
/*----------------------------------------------------------------*/

static bool mptype_supported( int mptype )
{
	//	The subset the shim can marshal today.  Anything else is refused *before*
	//	the first operand is consumed, which is what keeps a refused call from
	//	desynchronising the bytecode stream.
	//
	switch ( mptype ) {
	case MPTYPE_INUM:		// int
	case MPTYPE_DNUM:		// double
	case MPTYPE_FLOAT:		// float
	case MPTYPE_LOCALSTRING:// string
	case MPTYPE_PVARPTR:	// pointer to a variable's storage
	case MPTYPE_FLEXSPTR:	// 0/NULL or string, decided per call
	case MPTYPE_NULLPTR:	// NULL
	case MPTYPE_PBMSCR:		// screen buffer, supplied by the runtime
	case MPTYPE_PTR_EXINFO:	// pointer to the plugin info block
		return true;
	default:
		return false;
	}
}

static void prepare_localstr( DllArgValue *v, const char *src )
{
	//	DLL 渡しのための文字列を準備する (the ANSI half of hsp3extlib_ffi.cpp:451)
	//
	v->owned = sbAlloc( (int)strlen( src ) + 1 );
	strcpy( v->owned, src );
	v->ptr = v->owned;
}

static void read_arg( DllArgValue *v, const STRUCTPRM *prm )
{
	PVal *pval;

	v->type = prm->mptype;
	v->ival = 0;
	v->dval = 0.0;
	v->fval = 0.0f;
	v->ptr = NULL;
	v->owned = NULL;

	//	mptype branches follow code_expand_next(), hsp3extlib_ffi.cpp:586-690.
	//
	switch ( prm->mptype ) {
	case MPTYPE_INUM:
		v->ival = (int)code_getdi( 0 );
		break;
	case MPTYPE_DNUM:
		v->dval = code_getdd( 0.0 );
		break;
	case MPTYPE_FLOAT:
		v->fval = (float)code_getdd( 0.0 );
		break;
	case MPTYPE_LOCALSTRING:
		prepare_localstr( v, code_gets() );
		break;
	case MPTYPE_PVARPTR: {
		APTR aptr = code_getva( &pval );
		v->ptr = HspVarCorePtrAPTR( pval, aptr );
		break;
	}
	case MPTYPE_NULLPTR:
		v->ptr = NULL;
		break;
	case MPTYPE_PBMSCR:
		//	The screen buffer is handed in by the runtime, not read from the
		//	bytecode: the reference calls GetBMSCR() and consumes no operand.  The
		//	calls that declare it here are the audio stubs, which ignore it, so
		//	NULL keeps the operand stream correct without pulling hgio in.
		//
		v->ptr = NULL;
		break;
	case MPTYPE_PTR_EXINFO:
		v->ptr = exinfo;
		break;
	case MPTYPE_FLEXSPTR: {
		//	Either a literal 0 / NULL or a string, decided at run time.  The
		//	reference reads code_get() and inspects *mpval to tell them apart.
		//
		int chk = code_get();
		if ( chk < 0 ) throw ( HSPERR_NO_DEFAULT );		// -1 == PARAM_END
		pval = *pmpval;
		if ( pval->flag == HSPVAR_FLAG_INT ) {
			v->ival = *(int *)pval->pt;
		} else if ( pval->flag == HSPVAR_FLAG_STR ) {
			prepare_localstr( v, (char *)pval->pt );
		} else {
			throw ( HSPERR_TYPE_MISMATCH );
		}
		break;
	}
	default:
		//	Guarded by the dry scan; kept so the switch stays exhaustive.
		throw ( HSPERR_UNSUPPORTED_FUNCTION );
	}
}

/*----------------------------------------------------------------*/
/*	Interface														*/
/*----------------------------------------------------------------*/

void dllshim_install( HSP3TYPEINFO *info )
{
	hspctx = info->hspctx;
	exinfo = info->hspexinfo;
	pmpval = exinfo->mpval;
}

void dllshim_report_exit( void )
{
	//	code_catcherror() records the code in hspctx->err and never clears it, so
	//	even an error the script swallowed with ONERROR is still here.
	//
	if ( hspctx == NULL ) return;
	printf( "hsp3switch: script end: err=%d (%s) runmode=%d endcode=%d\n",
		(int)hspctx->err, hspd_geterror( hspctx->err ),
		hspctx->runmode, hspctx->endcode );
	fflush( stdout );
}

int dllshim_exec( int cmd, int mask, char *desc, int descsize )
{
	STRUCTDAT *st;
	LIBDAT *lib;
	const DllImplEntry *entry;
	DllArgValue args[DLLSHIM_MAX_ARGS];
	const char *libname = "?";
	const char *funcname = "?";
	int prmmax, argc, i, result;

	if ( desc != NULL && descsize > 0 ) desc[0] = 0;
	if ( hspctx == NULL ) return -1;

	prmmax = hspctx->hsphed->max_finfo / (int)sizeof( STRUCTDAT );
	if ( cmd < 0 || cmd >= prmmax ) return -1;

	st = &hspctx->mem_finfo[cmd];
	if ( st->index >= 0 ) {
		lib = &hspctx->mem_linfo[ st->index ];
		if ( lib->nameidx >= 0 ) libname = code_strp( lib->nameidx );
	}
	if ( st->nameidx >= 0 ) funcname = code_strp( st->nameidx );
	if ( desc != NULL && descsize > 0 ) {
		snprintf( desc, descsize, "%s!%s", libname, funcname );
	}

	//	Reference order (hsp3extlib_ffi.cpp:724-732): resolve the STRUCTDAT, then
	//	reject a statement used as a function (or vice versa).
	//
	if ( st->index < 0 ) return -1;
	if ( ( st->otindex & mask ) == 0 ) throw ( HSPERR_SYNTAX );

	entry = find_entry( st );
	if ( entry == NULL ) return -1;

	if ( st->prmmax > DLLSHIM_MAX_ARGS ) {
		printf( "hsp3switch: ### %s: %d parameters, shim limit is %d\n",
			desc, st->prmmax, DLLSHIM_MAX_ARGS );
		fflush( stdout );
		return -1;
	}

	//	Dry scan: refuse before consuming anything, so the caller's loud failure
	//	leaves the bytecode stream where it was.
	//
	for ( i = 0; i < st->prmmax; i++ ) {
		int mptype = hspctx->mem_minfo[ st->prmindex + i ].mptype;
		if ( !mptype_supported( mptype ) ) {
			printf( "hsp3switch: ### %s: unsupported parameter type %d (arg %d)\n",
				desc, mptype, i );
			fflush( stdout );
			return -1;
		}
	}

	argc = st->prmmax;
	try {
		for ( i = 0; i < argc; i++ ) {
			read_arg( &args[i], &hspctx->mem_minfo[ st->prmindex + i ] );
		}
		result = entry->impl( args, argc );
	}
	catch ( ... ) {
		for ( i = 0; i < argc; i++ ) {
			if ( args[i].owned != NULL ) sbFree( args[i].owned );
		}
		throw;
	}
	for ( i = 0; i < argc; i++ ) {
		if ( args[i].owned != NULL ) sbFree( args[i].owned );
	}

	//	Subid handling copied from hsp3extlib_ffi.cpp:734-753.  _exrand_randomize
	//	is an OLDDLLINIT (subid -6) entry, so a positive return value would mean
	//	"await N ticks" - we return 0 and land in the `stat = -result` branch.
	//
	if ( st->subid == STRUCTPRM_SUBID_OLDDLLINIT ) {
		if ( result > 0 ) {
			if ( result & 0x20000 ) {
				result &= 0x1ffff;
			} else if ( result & 0x10000 ) {
				result = ( result & 0xffff ) * 10;
			} else {
				throw ( HSPERR_DLL_ERROR );
			}
			hspctx->waitcount = result;
			hspctx->waittick = -1;
			hspctx->runmode = RUNMODE_AWAIT;
			return RUNMODE_AWAIT;
		}
		hspctx->stat = -result;
	} else {
		hspctx->stat = result;
	}

	return RUNMODE_RUN;
}