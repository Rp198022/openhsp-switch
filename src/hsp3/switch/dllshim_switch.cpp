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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>

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

//	user32.dll / COMDLG32.DLL / imm32 - the Windows UI layer Elona links against
//	(window menus, the open/save dialogs, IME).  None of it exists on the Switch
//	and the game does not need it to run, but it must not fail either: a failed
//	call becomes HSP error 38, Elona's ONERROR handler swallows it, and the loop
//	it sits in then spins.  The run that first got past the language screen
//	logged 1,419,940 "user32.dll!keybd_event" failures in 60 seconds.
//
//	1 doubles as Win32's TRUE for the BOOL-returning calls and as a plausible
//	fake handle for the ones that hand one back (CreateMenu, ImmGetContext).
//
static int impl_win_true( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 1;
}

static int impl_win_zero( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 0;
}

//	The rest of kernel32.dll.  CloseHandle was the third failure-in-a-loop the
//	device runs found (about 2.8 million calls in 75 seconds, right after the
//	game opened its own 800x600 screen), so it has to succeed like the others.
//
static int impl_GetUserDefaultLCID( const DllArgValue *args, int argc )
{
	(void)args;
	(void)argc;
	return 0x0409;		//	en-US, matching the language chosen on the title screen
}

//	LCMapStringA( LCID, flags, src, cchSrc, dest, cchDest ) - only the two case
//	mapping modes are approximated (by copying, optionally case-folded); a real
//	sort-key request would need a locale table and is not attempted.  Elona uses
//	this for case-insensitive name handling, where copying is right and the
//	ordering is its own.
//
static int impl_LCMapStringA( const DllArgValue *args, int argc )
{
	const char *src;
	char *dest;
	int room, i;

	if ( argc < 6 ) return 0;
	if ( args[2].ptr == NULL || args[4].ptr == NULL ) return 0;
	src = (const char *)args[2].ptr;
	dest = (char *)args[4].ptr;
	room = args[5].ival;
	if ( room <= 1 ) return 0;
	if ( (size_t)room > strlen( src ) + 1 ) room = (int)strlen( src ) + 1;

	for ( i = 0; i < room - 1 && src[i] != 0; i++ ) {
		char c = src[i];
		if ( args[1].ival & 0x00000100 ) {			//	LCMAP_LOWERCASE
			if ( c >= 'A' && c <= 'Z' ) c = (char)( c + 32 );
		} else if ( args[1].ival & 0x00000200 ) {	//	LCMAP_UPPERCASE
			if ( c >= 'a' && c <= 'z' ) c = (char)( c - 32 );
		}
		dest[i] = c;
	}
	dest[i] = 0;
	return i;
}

/*----------------------------------------------------------------*/
/*	hspda.dll - the legacy "Easy Data Access" plugin				*/
/*----------------------------------------------------------------*/

//	Reference: src/plugins/win32/hspda/Hspda.cpp.
//
//	All four functions the .ax declares are STRUCTPRM_SUBID_OLDDLLINIT entries
//	whose minfo slots are the ABI alone - (pexinfo, nullptr, nullptr, nullptr).
//	The real arguments are not marshalled from those types: they stay on the
//	bytecode stream and the function reads them itself through exinfo, which is
//	what hei->HspFunc_prm_getva() / _getdi() / _gets() do in Hspda.cpp.  Nothing
//	is consumed by the marshaller for such a declaration, so when an
//	implementation below runs, the stream is sitting on the first real argument.
//
//	sortval/sortstr/sortnote/sortget became standard commands in HSP 3.5
//	(doclib/history.txt), but the names stay declared against hspda.dll for
//	backward compatibility - and that is the form Elona's start.ax calls, which
//	is why start-up stopped on `Unsupported DLL call hspda.dll!_sortnote@16`.
//	The implementations below follow the built-in equivalents in hsp3int.cpp
//	(case 0x02d sortval / 0x02f sortnote) so the result is the same either way.
//
//	A non-zero order sorts descending (same convention as the built-ins), and
//	ties keep their original relative position via the recorded index.
//
struct HspdaItem {
	union {
		int ikey;
		double dkey;
		char *skey;
	} as;
	int info;
};

static HspdaItem *hspda_dtmp = NULL;

static PVal *hspda_note_pval = NULL;	//	xnotesel's target variable
static APTR hspda_note_aptr = 0;

static void hspda_probe( const char *who, const char *what );		// TEMPORARY P3 DIAGNOSTIC
static void hspda_probe_args( const char *who, const DllArgValue *args, int argc );

static void hspda_data_bye( void )
{
	if ( hspda_dtmp != NULL ) {
		free( hspda_dtmp );
		hspda_dtmp = NULL;
	}
}

static void hspda_data_ini( int size )
{
	hspda_data_bye();
	if ( size < 1 ) size = 1;
	hspda_dtmp = (HspdaItem *)calloc( (size_t)size, sizeof( HspdaItem ) );
}

struct HspdaLessStr {
	int order;
	explicit HspdaLessStr( int o ) : order( o ) {}
	bool operator()( const HspdaItem &a, const HspdaItem &b ) const {
		int cmp = strcmp( a.as.skey, b.as.skey );
		if ( cmp == 0 ) return a.info < b.info;
		return ( order == 0 ) ? ( cmp < 0 ) : ( cmp > 0 );
	}
};

struct HspdaLessInt {
	int order;
	explicit HspdaLessInt( int o ) : order( o ) {}
	bool operator()( const HspdaItem &a, const HspdaItem &b ) const {
		if ( a.as.ikey == b.as.ikey ) return a.info < b.info;
		return ( order == 0 ) ? ( a.as.ikey < b.as.ikey ) : ( a.as.ikey > b.as.ikey );
	}
};

struct HspdaLessDouble {
	int order;
	explicit HspdaLessDouble( int o ) : order( o ) {}
	bool operator()( const HspdaItem &a, const HspdaItem &b ) const {
		if ( a.as.dkey == b.as.dkey ) return a.info < b.info;
		return ( order == 0 ) ? ( a.as.dkey < b.as.dkey ) : ( a.as.dkey > b.as.dkey );
	}
};

//	Hspda.cpp's skipline / lineeq, verbatim.
//
static char *hspda_skipline( char *s )
{
	while ( *s != 0 ) {
		char c = *s++;
		if ( c == '\n' ) break;
		if ( c == '\r' ) {
			if ( *s == '\n' ) s++;
			break;
		}
	}
	return s;
}

static bool hspda_lineeq( char *a, char *b )
{
	while ( 1 ) {
		char ca = *a++;
		char cb = *b++;
		if ( ca == '\n' || ca == '\r' ) ca = 0;
		if ( ca != cb ) return false;
		if ( ca == 0 ) return true;
	}
}

//	Hspda.cpp's addline(): grow the note variable and append the new line with
//	a CRLF terminator.
//
static void hspda_addline( PVal *pval, APTR aptr, int len, char *add )
{
	int size;
	int addlen = (int)strlen( add );
	char *buf, *p;

	HspVarCoreAllocBlock( pval, HspVarCorePtrAPTR( pval, aptr ), len + addlen + 8 );
	buf = (char *)HspVarCoreGetBlockSize( pval, HspVarCorePtrAPTR( pval, aptr ), &size );
	p = buf + len;
	if ( len > 0 && buf[len-1] != '\r' && buf[len-1] != '\n' ) {
		strcpy( p, "\r\n" );
		p += 2;
	}
	strcpy( p, add );
	p += addlen;
	strcpy( p, "\r\n" );
}

//	xnotesel notedat, maxnum
//
static int impl_hspda_xnotesel( const DllArgValue *args, int argc )
{
	PVal *pval;
	int maxnum;

	hspda_probe_args( "xnotesel", args, argc );		// TEMPORARY P3 DIAGNOSTIC
	return -1;

	(void)args;
	(void)argc;

	hspda_note_aptr = code_getva( &pval );
	maxnum = code_getdi( 0 );
	if ( maxnum == 0 ) maxnum = 256;			// the plugin's default
	if ( pval->flag != HSPVAR_FLAG_STR ) return -1;

	hspda_note_pval = pval;
	hspda_data_ini( maxnum );
	return 0;
}

//	xnoteadd "strings"  ->  stat = index of the line that now holds it
//
static int impl_hspda_xnoteadd( const DllArgValue *args, int argc )
{
	char *add, *buf, *p;
	int size, line;

	hspda_probe_args( "xnoteadd", args, argc );		// TEMPORARY P3 DIAGNOSTIC
	return -1;

	(void)args;
	(void)argc;

	add = code_gets();
	if ( hspda_note_pval == NULL ) return -1;
	if ( hspda_note_pval->flag != HSPVAR_FLAG_STR ) return -1;

	buf = (char *)HspVarCoreGetBlockSize( hspda_note_pval,
		HspVarCorePtrAPTR( hspda_note_pval, hspda_note_aptr ), &size );

	//	An already-present line is reported, not duplicated; a new one is
	//	appended at the first free index.  Negative result because the caller
	//	negates it into stat.
	//
	line = 0;
	for ( p = buf; *p != 0; line++ ) {
		if ( hspda_lineeq( p, add ) ) return -line;
		p = hspda_skipline( p );
	}
	hspda_addline( hspda_note_pval, hspda_note_aptr, (int)( p - buf ), add );
	return -line;
}

//	sortval var, order  -  numeric sort of an int/double array
//
static int impl_hspda_sortval( const DllArgValue *args, int argc )
{
	PVal *pval = NULL;
	APTR aptr;
	int order, i, count;

	hspda_probe_args( "sortval", args, argc );		// TEMPORARY P3 DIAGNOSTIC
	hspda_probe( "sortval", "entry" );
	aptr = code_getva( &pval );
	hspda_probe( "sortval", "arg0" );
	printf( "hsp3switch: ### sortval arg0: flag=%d aptr=%d len1=%d\n",
		( pval == NULL ) ? -1 : (int)pval->flag, aptr,
		( pval == NULL ) ? -1 : (int)pval->len[1] );
	order = code_getdi( -123456 );
	hspda_probe( "sortval", "arg1" );
	printf( "hsp3switch: ### sortval arg1: order=%d\n", order );
	fflush( stdout );

	return 0;		// TEMPORARY: the real sort returns once the shape is known

	(void)args;
	(void)argc;

	aptr = code_getva( &pval );
	order = code_getdi( 0 );
	count = pval->len[1];
	if ( count <= 0 ) return -1;

	if ( pval->flag == HSPVAR_FLAG_INT ) {
		int *p = (int *)HspVarCorePtrAPTR( pval, aptr );
		hspda_data_ini( count );
		if ( hspda_dtmp == NULL ) return -1;
		for ( i = 0; i < count; i++ ) {
			hspda_dtmp[i].as.ikey = p[i];
			hspda_dtmp[i].info = i;
		}
		std::sort( hspda_dtmp, hspda_dtmp + count, HspdaLessInt( order ) );
		for ( i = 0; i < count; i++ ) p[i] = hspda_dtmp[i].as.ikey;
		return 0;
	}

	if ( pval->flag == HSPVAR_FLAG_DOUBLE ) {
		double *p = (double *)HspVarCorePtrAPTR( pval, aptr );
		hspda_data_ini( count );
		if ( hspda_dtmp == NULL ) return -1;
		for ( i = 0; i < count; i++ ) {
			hspda_dtmp[i].as.dkey = p[i];
			hspda_dtmp[i].info = i;
		}
		std::sort( hspda_dtmp, hspda_dtmp + count, HspdaLessDouble( order ) );
		for ( i = 0; i < count; i++ ) p[i] = hspda_dtmp[i].as.dkey;
		return 0;
	}

	return -1;
}

//	sortnote var, order  -  line sort of a note (string) variable
//
static int impl_hspda_sortnote( const DllArgValue *args, int argc )
{
	PVal *pval = NULL;
	APTR aptr;
	char *buf, *p, *dst;
	int order, i, count, size, len;

	hspda_probe_args( "sortnote", args, argc );		// TEMPORARY P3 DIAGNOSTIC
	hspda_probe( "sortnote", "entry" );
	aptr = code_getva( &pval );
	hspda_probe( "sortnote", "arg0" );
	if ( pval != NULL && pval->flag == HSPVAR_FLAG_STR ) {
		char *s = (char *)HspVarCoreGetBlockSize( pval, HspVarCorePtrAPTR( pval, aptr ), &size );
		printf( "hsp3switch: ### sortnote arg0: STR aptr=%d size=%d head='%.60s'\n",
			aptr, size, ( s == NULL ) ? "" : s );
	} else {
		printf( "hsp3switch: ### sortnote arg0: flag=%d aptr=%d\n",
			( pval == NULL ) ? -1 : (int)pval->flag, aptr );
	}
	order = code_getdi( -123456 );
	hspda_probe( "sortnote", "arg1" );
	printf( "hsp3switch: ### sortnote arg1: order=%d\n", order );
	fflush( stdout );

	return 0;		// TEMPORARY: the real sort returns once the shape is known

	(void)args;
	(void)argc;

	aptr = code_getva( &pval );
	order = code_getdi( 0 );
	if ( pval->flag != HSPVAR_FLAG_STR ) return -1;

	buf = (char *)HspVarCoreGetBlockSize( pval, HspVarCorePtrAPTR( pval, aptr ), &size );

	//	Line count first (the built-ins' GetNoteLines), then split in place
	//	(their NoteToData) - a line is terminated by LF, CR or CRLF and the
	//	terminator is overwritten with NUL.
	//
	count = 0;
	for ( p = buf; *p != 0; count++ ) {
		while ( *p != 0 ) {
			char c = *p++;
			if ( c == '\n' ) break;
			if ( c == '\r' ) {
				if ( *p == '\n' ) p++;
				break;
			}
		}
	}
	if ( count <= 0 ) return -1;

	hspda_data_ini( count );
	if ( hspda_dtmp == NULL ) return -1;

	i = 0;
	p = buf;
	while ( *p != 0 && i < count ) {
		hspda_dtmp[i].as.skey = p;
		hspda_dtmp[i].info = i;
		while ( *p != 0 ) {
			char c = *p;
			if ( c == '\n' || c == '\r' ) *p = 0;
			p++;
			if ( c == '\n' ) break;
			if ( c == '\r' ) {
				if ( *p == '\n' ) p++;
				break;
			}
		}
		i++;
	}

	std::sort( hspda_dtmp, hspda_dtmp + count, HspdaLessStr( order ) );

	//	Rejoin with CRLF into the runtime's temp string (DataToNoteLen /
	//	DataToNote) and store it back into the variable.
	//
	len = 0;
	for ( i = 0; i < count; i++ ) {
		len += (int)strlen( hspda_dtmp[i].as.skey ) + 2;
	}
	dst = code_stmp( len + 1 );
	p = dst;
	for ( i = 0; i < count; i++ ) {
		int slen = (int)strlen( hspda_dtmp[i].as.skey );
		memcpy( p, hspda_dtmp[i].as.skey, slen );
		p += slen;
		*p++ = 13;
		*p++ = 10;
	}
	*p = 0;
	code_setva( pval, aptr, HSPVAR_FLAG_STR, dst );

	return 0;
}

//	TEMPORARY P3 DIAGNOSTIC - remove once the hspda argument shapes are known.
//
//	Reading the arguments off the bytecode stream by hand only works if the
//	entry really is the OLDDLL form assumed in the section above; if it is not,
//	the reads desynchronise the stream and the next fetched word is executed as
//	an instruction, which is a hard fault rather than an HSP error (p3s19 died
//	exactly that way, without even reaching the teardown).
//
//	p3s20 tried to print the stream instead and added a worse fault: hspctx->mcs
//	is NOT the interpreter's live code pointer - that is a file-static inside
//	hsp3code.cpp, and the struct field is only ever maintained by hsp3cnv's
//	converter (chsp3.cpp:162) - so the dump dereferenced stale memory and the
//	run died before it named anything.
//
//	code_getpcbak() is the supported view: it returns the token the interpreter
//	has just consumed, which is exactly the operand being reported.  Every read
//	is therefore logged twice - the raw token words, decoded the way __code_next
//	() does, and the value the runtime API returned for that same operand.
//
static void hspda_probe( const char *who, const char *what )
{
	unsigned short *pc = code_getpcbak();

	printf( "hsp3switch: ### %s %s: token@%ld = %#06x %#06x %#06x [type=%d exflg=%x]\n",
		who, what, (long)( pc - hspctx->mem_mcs ),
		(unsigned)pc[0], (unsigned)pc[1], (unsigned)pc[2],
		(int)( pc[0] & 0x0fff ), (int)( pc[0] & 0x6000 ) );
	fflush( stdout );
}

static void hspda_probe_args( const char *who, const DllArgValue *args, int argc )
{
	int i;

	printf( "hsp3switch: ### %s: argc=%d\n", who, argc );
	for ( i = 0; i < argc; i++ ) {
		printf( "   arg%d mptype=%d ival=%d dval=%g ptr=%p str='%.32s'\n",
			i, args[i].type, args[i].ival, args[i].dval, args[i].ptr,
			( args[i].ptr == NULL ) ? "" : (const char *)args[i].ptr );
	}
	fflush( stdout );
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
	{ "kernel32.dll",	"CloseHandle",			impl_win_true },
	{ "kernel32.dll",	"RemoveDirectoryA",		impl_win_true },
	{ "kernel32.dll",	"GetUserDefaultLCID",	impl_GetUserDefaultLCID },
	{ "kernel32.dll",	"LCMapStringA",			impl_LCMapStringA },
	{ "winmm.dll",		"timeBeginPeriod",		impl_timeBeginPeriod },
	{ "winmm.dll",		"timeEndPeriod",		impl_timeEndPeriod },
	{ "winmm.dll",		"timeGetTime",			impl_timeGetTime },

	//	hspda.dll - the legacy note/sort family (see the section above).  These
	//	consume their own arguments off the bytecode stream.
	{ "hspda.dll",		"_sortval@16",			impl_hspda_sortval },
	{ "hspda.dll",		"_sortnote@16",			impl_hspda_sortnote },
	{ "hspda.dll",		"_xnotesel@16",			impl_hspda_xnotesel },
	{ "hspda.dll",		"_xnoteadd@16",			impl_hspda_xnoteadd },

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

	//	Windows UI layer - present but inert (see impl_win_true above).  The
	//	library names are spelled exactly as the .ax declares them, including
	//	"COMDLG32.DLL" in upper case.
	{ "user32.dll",		"AppendMenuA",			impl_win_true },
	{ "user32.dll",		"CheckMenuRadioItem",	impl_win_true },
	{ "user32.dll",		"CreateMenu",			impl_win_true },
	{ "user32.dll",		"CreatePopupMenu",		impl_win_true },
	{ "user32.dll",		"DrawMenuBar",			impl_win_true },
	{ "user32.dll",		"SetMenu",				impl_win_true },
	{ "user32.dll",		"keybd_event",			impl_win_zero },
	{ "user32.dll",		"GetKeyboardState",		impl_win_true },
	{ "COMDLG32.DLL",	"GetOpenFileNameA",		impl_win_zero },	// 0 = cancelled
	{ "COMDLG32.DLL",	"GetSaveFileNameA",		impl_win_zero },
	{ "imm32",			"ImmGetContext",		impl_win_true },
	{ "imm32",			"ImmReleaseContext",	impl_win_true },
	{ "imm32",			"ImmSetOpenStatus",		impl_win_true },
	{ "imm32",			"ImmGetOpenStatus",		impl_win_zero },	// IME closed
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

//	P3 DIAGNOSTIC - bounds the pc trace printed at the end of dllshim_exec().
//
static int dllshim_trace_count = 0;

int dllshim_exec( int cmd, int mask, char *desc, int descsize )
{
	STRUCTDAT *st;
	LIBDAT *lib;
	const DllImplEntry *entry;
	DllArgValue args[DLLSHIM_MAX_ARGS];
	unsigned short *pc_in = code_getpcbak();
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

	//	P3 DIAGNOSTIC - how far the bytecode stream moved across this command.
	//
	//	pc_in is the command token itself (the caller dispatched on it without
	//	advancing; exec_dllcmd's code_next() is what consumes it).  pc_out is the
	//	token left pending, which must be the next statement's first token - so
	//	(pc_out - pc_in) is exactly how many words the parameter reads consumed.
	//
	//	That makes over-consumption directly visible: a command whose declaration
	//	does not match the operands the compiler emitted runs on into the
	//	following statements and leaves the interpreter mid-stream, which faults
	//	as a data abort rather than raising an HSP error.
	//
	if ( dllshim_trace_count < 400 ) {
		printf( "hsp3switch: ### dll %s cmd=%d pc=%ld->%ld result=%d\n",
			desc, cmd,
			(long)( pc_in - hspctx->mem_mcs ),
			(long)( code_getpcbak() - hspctx->mem_mcs ),
			result );
		fflush( stdout );
	}
	dllshim_trace_count++;

	return RUNMODE_RUN;
}