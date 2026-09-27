//
//	src/hsp3dish/switch/webtask_switch.cpp
//	WebTask (HTTP access) for the Nintendo Switch - unsupported stub (T2.3)
//
//	This replaces src/hsp3dish/linux/webtask_linux.cpp, which is built on
//	libcurl.  Network access is out of scope for the P2 gate and the Switch
//	build does not link curl, but src/hsp3dish/hsp3gr_dish.cpp compiles its
//	`#define USE_WEBTASK` block unconditionally, so the class itself has to
//	exist.
//
//	The class layout comes from linux/webtask_linux.h, which is the header
//	webtask.h selects under -DHSPLINUX - so no upstream file is touched.
//	The stubs keep the documented state machine (a request drives the object
//	into CZHTTP_MODE_ERROR instead of silently pretending to succeed), which
//	is the same "fail loudly" policy the T2.2 DLL/socket stubs follow.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../webtask.h"

/*----------------------------------------------------------------*/

WebTask::WebTask( void )
{
	str_agent = NULL;
	mode = CZHTTP_MODE_NONE;
	size = 0;
	errstr[0] = 0;
	req_url.clear();
	proxy_url[0] = 0;
	proxy_local = 0;
	req_header = NULL;
	varstr[0] = 0;
	postdata = NULL;
	vardata = NULL;
	varsize = 0;
	curl = NULL;
	mcurl = NULL;
}

WebTask::~WebTask( void )
{
	Terminate();
}

void WebTask::Reset( void )
{
	/*	Nothing to reset: there is no transport on the Switch.	*/
}

void WebTask::Terminate( void )
{
	errstr[0] = 0;
	req_url.clear();
	ClearVarData();
	ClearPostData();
}

int WebTask::Request( char *url, char *post )
{
	(void)url;
	(void)post;

	SetError( (char *)"http is not supported on this build" );
	mode = CZHTTP_MODE_ERROR;

	printf( "hsp3switch: ### WebTask::Request unsupported\n" );
	fflush( stdout );
	return -1;
}

int WebTask::getStatus( int id )
{
	switch ( id ) {
	case HTTPINFO_MODE:
		return getMode();
	case HTTPINFO_SIZE:
		return getSize();
	default:
		break;
	}
	return 0;
}

char *WebTask::getData( int id )
{
	switch ( id ) {
	case HTTPINFO_DATA: {
		char *p = getVarData();
		if ( p != NULL ) return p;
		break;
	}
	case HTTPINFO_ERROR:
		return getError();
	default:
		break;
	}
	return (char *)"";
}

void WebTask::setData( int id, char *str )
{
	switch ( id ) {
	case HTTPINFO_DATA:
		ClearVarData();
		break;
	case HTTPINFO_ERROR:
		SetError( str );
		break;
	default:
		break;
	}
}

int WebTask::Exec( void )
{
	return 0;
}

void WebTask::ClearVarData( void )
{
	if ( vardata != NULL ) {
		free( vardata );
		vardata = NULL;
	}
	varsize = 0;
	size = 0;
}

void WebTask::ClearPostData( void )
{
	if ( postdata != NULL ) {
		free( postdata );
		postdata = NULL;
	}
}

void WebTask::SetError( char *mes )
{
	if ( mes == NULL ) {
		errstr[0] = 0;
		return;
	}
	strncpy( errstr, mes, sizeof( errstr ) - 1 );
	errstr[sizeof( errstr ) - 1] = 0;
}