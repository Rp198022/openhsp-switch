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

#include "../hsp3config.h"
#include "../hsp3code.h"
#include "../hsp3debug.h"
#include "../hsp3struct.h"
#include "../hspvar_core.h"
#include "../strbuf.h"

#include "dllshim_switch.h"

static HSPCTX *hspctx = NULL;		// Current Context
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
	pmpval = info->hspexinfo->mpval;
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