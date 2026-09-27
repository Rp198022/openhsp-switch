//
//	hsp3/switch/dllshim_switch.h
//	Nintendo Switch DLL/plugin shim	(P3, step 1)
//
//	Why this exists
//	---------------
//	Upstream implements external DLL calls in src/hsp3/linux/hsp3extlib_ffi.cpp
//	with libffi plus dlopen/dlsym.  libnx has neither a dynamic loader nor
//	libffi, so that translation unit is excluded from makefile.switch and its
//	symbols are supplied by src/hsp3/switch/glue_switch.cpp instead.
//
//	What was missing there is the half that actually *performs* a call: mapping
//	(library name, function name) to an implementation, and turning the operands
//	that follow a TYPE_DLLCMD token into C arguments.  This file is that half.
//
//	Scope: only the functions the script actually reaches are implemented; an
//	unknown library/function is reported with its full name and still fails
//	loudly, so the port grows one dependency at a time.
//
#ifndef __dllshim_switch_h
#define __dllshim_switch_h

#include "../hsp3config.h"
#include "../hsp3struct.h"

//	Hands the shim the interpreter context.  HSP passes it to the platform glue
//	exactly once, from Hsp3ExtLibInit() (see linux/hsp3ext_linux.cpp:136).
//
void dllshim_install( HSP3TYPEINFO *info );

//	Runs one DLL command.
//
//	Returns the run mode the caller must return (RUNMODE_RUN or RUNMODE_AWAIT)
//	when the call was resolved and performed - hspctx->stat is set.  Returns -1
//	when the shim has no implementation for this library/function, in which case
//	nothing has been consumed from the bytecode stream and the caller must
//	report the failure.
//
//	`desc` always receives "library!function" for diagnostics (may be NULL).
//
//	Mirrors linux/hsp3extlib_ffi.cpp:711-754, including the HLA/hspext-style
//	subid handling and the "not a statement/function form" syntax check.
//
int dllshim_exec( int cmd, int mask, char *desc, int descsize );

//	P3 diagnostics: prints the error the script left behind.  It must run during
//	teardown, from a term function, because hspctx is destroyed before
//	hsp3dish_exec() returns - and because a script that catches its own errors
//	with ONERROR otherwise leaves hspctx->err as the only trace of them.
//
void dllshim_report_exit( void );

#endif