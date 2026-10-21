//
//	src/hsp3dish/switch/ai_chat.cpp
//	Companion chatter through the DeepSeek API - see ai_chat.h for the split
//	of work between this port and the model.
//
//	Nothing here runs unless data/ai_config.txt exists with a key in it, so a
//	copy of the game without the file behaves exactly as before.
//
//	Threading: one request at a time, on a detached thread.  The present pass
//	only ever reads the finished line behind a mutex, so a slow or dead server
//	can never stall a frame.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>

#include <curl/curl.h>

#include "ai_chat.h"

#define AI_MAX_TEXT		8192	/* request body / answer scratch		*/
#define AI_MAX_LINE		512		/* one spoken line					*/
#define AI_MAX_PERSONA	2048
#define AI_MAX_MEMORY	1024

#define AI_DIR			"sdmc:/switch/openhsp/data/"
#define AI_CONFIG		AI_DIR "ai_config.txt"
#define AI_LOG			AI_DIR "npc_log.txt"
#define AI_INPUT		AI_DIR "ai_input.txt"	/* debug chat channel	*/

static int			ai_enabled = 0;			/* config says on			*/
static int			ai_interval = 90;		/* seconds between idle lines */
static char			ai_key[ 256 ] = "";
static char			ai_model[ 64 ] = "deepseek-chat";

static pthread_mutex_t	ai_lock = PTHREAD_MUTEX_INITIALIZER;
static char			ai_scene[ AI_MAX_TEXT ] = "";		/* what the model is told	*/
static char			ai_npc[ 64 ] = "";					/* follower name			*/
static char			ai_event[ 512 ] = "";				/* pending event sentence	*/
static int			ai_event_prio = -1;
static double		ai_event_at = 0;

static char			ai_line[ AI_MAX_LINE ] = "";		/* newest answer			*/
static char			ai_last_line[ AI_MAX_LINE ] = "";
static int			ai_state = AI_STATE_OFF;
static char			ai_status[ 96 ] = "AI 未配置";
static int			ai_busy = 0;
static double		ai_last_request = 0;
static double		ai_last_line_at = 0;
static double		ai_event_seen[ 4 ];					/* 最近的同类事件时间	*/
static char			ai_event_kind[ 4 ][ 64 ] = { "", "", "", "" };

/*	Persona and memory, read once per follower.							*/
static char			ai_persona_name[ 64 ] = "";
static char			ai_persona[ AI_MAX_PERSONA ] = "";
static char			ai_memory[ AI_MAX_MEMORY ] = "";

static double ai_now( void )
{
	struct timespec ts;
	clock_gettime( CLOCK_MONOTONIC, &ts );
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void ai_status_set( const char *s )
{
	strncpy( ai_status, s, sizeof( ai_status ) - 1 );
	ai_status[ sizeof( ai_status ) - 1 ] = '\0';
}

/*	One "key = value" line of the config; '#' starts a comment.				*/
static void ai_trim( char *s )
{
	char *p = s;
	size_t n;
	while ( *p == ' ' || *p == '\t' ) p++;
	if ( p != s ) memmove( s, p, strlen( p ) + 1 );
	n = strlen( s );
	while ( n > 0 && ( s[ n - 1 ] == '\n' || s[ n - 1 ] == '\r' || s[ n - 1 ] == ' ' ||
			  s[ n - 1 ] == '\t' ) ) s[ --n ] = '\0';
}

static void ai_load_config( void )
{
	FILE *f = fopen( AI_CONFIG, "rb" );
	char line[ 512 ];

	if ( f == NULL ) {
		ai_enabled = 0;
		ai_state = AI_STATE_OFF;
		ai_status_set( "AI 未配置" );
		return;
	}
	while ( fgets( line, sizeof( line ), f ) != NULL ) {
		char *eq, *k, *v;
		ai_trim( line );
		if ( line[ 0 ] == '#' || line[ 0 ] == '\0' ) continue;
		eq = strchr( line, '=' );
		if ( eq == NULL ) continue;
		*eq = '\0';
		k = line; v = eq + 1;
		ai_trim( k ); ai_trim( v );
		if ( strcmp( k, "key" ) == 0 ) {
			strncpy( ai_key, v, sizeof( ai_key ) - 1 );
			ai_key[ sizeof( ai_key ) - 1 ] = '\0';
		} else if ( strcmp( k, "model" ) == 0 ) {
			strncpy( ai_model, v, sizeof( ai_model ) - 1 );
			ai_model[ sizeof( ai_model ) - 1 ] = '\0';
		} else if ( strcmp( k, "interval" ) == 0 ) {
			ai_interval = atoi( v );
			if ( ai_interval < 30 ) ai_interval = 30;
			if ( ai_interval > 3600 ) ai_interval = 3600;
		} else if ( strcmp( k, "enable" ) == 0 ) {
			ai_enabled = atoi( v ) != 0;
		}
	}
	fclose( f );
	if ( !ai_enabled ) { ai_state = AI_STATE_OFF; ai_status_set( "AI 已关闭" ); return; }
	if ( ai_key[ 0 ] == '\0' ) { ai_state = AI_STATE_OFF; ai_status_set( "AI 无密钥" ); return; }
	ai_state = AI_STATE_READY;
	ai_status_set( "AI 就绪" );
	/*	The idle clock starts here, not at zero: without this the follower
		would blurt something out the moment the party sheet first appears.	*/
	ai_last_request = ai_now();
	printf( "aichat: config loaded model=%s interval=%d\n", ai_model, ai_interval );
	fflush( stdout );
}

/*	Pull the "## 人设" and "## 记忆" sections out of the follower's markdown.
	Anything else in the file is ignored, so the file stays editable by hand. */
static void ai_load_persona( const char *npc )
{
	char path[ 256 ];
	char line[ 512 ];
	FILE *f;
	int section = 0;				/* 1 = persona, 2 = memory			*/
	size_t pl, ml;

	if ( npc == NULL || npc[ 0 ] == '\0' ) return;
	if ( strcmp( npc, ai_persona_name ) == 0 ) return;
	strncpy( ai_persona_name, npc, sizeof( ai_persona_name ) - 1 );
	ai_persona_name[ sizeof( ai_persona_name ) - 1 ] = '\0';
	ai_persona[ 0 ] = '\0';
	ai_memory[ 0 ] = '\0';

	snprintf( path, sizeof( path ), AI_DIR "npc/%s.md", npc );
	f = fopen( path, "rb" );
	if ( f == NULL ) {
		/*	A save whose follower has no file of its own still gets the shared
			default, so one persona can be tried before writing everyone's.	*/
		snprintf( path, sizeof( path ), AI_DIR "npc/default.md" );
		f = fopen( path, "rb" );
		if ( f == NULL ) {
			printf( "aichat: no persona for %s\n", npc );
			fflush( stdout );
			return;
		}
	}
	while ( fgets( line, sizeof( line ), f ) != NULL ) {
		if ( strncmp( line, "##", 2 ) == 0 ) {
			if ( strstr( line, "人设" ) != NULL ) section = 1;
			else if ( strstr( line, "记忆" ) != NULL ) section = 2;
			else section = 0;
			continue;
		}
		if ( section == 0 ) continue;
		pl = strlen( ai_persona );
		ml = strlen( ai_memory );
		if ( section == 1 && pl + strlen( line ) + 1 < sizeof( ai_persona ) )
			strcat( ai_persona, line );
		else if ( section == 2 && ml + strlen( line ) + 1 < sizeof( ai_memory ) )
			strcat( ai_memory, line );
	}
	fclose( f );
	ai_trim( ai_persona );
	ai_trim( ai_memory );
	printf( "aichat: persona %s (%d bytes, memory %d)\n", npc,
		(int)strlen( ai_persona ), (int)strlen( ai_memory ) );
	fflush( stdout );
}

/*	Append the shown line to the log file, tagged with the in-game clock.	*/
static void ai_log_line( const char *kind, const char *text )
{
	FILE *f = fopen( AI_LOG, "ab" );
	if ( f == NULL ) return;
	fprintf( f, "%s|%s|%s\n", ai_npc[ 0 ] ? ai_npc : "-", kind, text );
	fclose( f );
}

/*	Escape a UTF-8 string for a JSON string literal.  Control characters and
	quotes are the only ones that need it; the text itself stays UTF-8.		*/
static void ai_json_escape( const char *in, char *out, size_t n )
{
	size_t o = 0;
	const unsigned char *p = (const unsigned char*)in;

	while ( *p && o + 8 < n ) {
		if ( *p == '"' || *p == '\\' ) { out[ o++ ] = '\\'; out[ o++ ] = (char)*p++; }
		else if ( *p == '\n' ) { out[ o++ ] = '\\'; out[ o++ ] = 'n'; p++; }
		else if ( *p == '\r' ) { p++; }
		else if ( *p == '\t' ) { out[ o++ ] = ' '; p++; }
		else if ( *p < 0x20 ) { p++; }
		else out[ o++ ] = (char)*p++;
	}
	out[ o ] = '\0';
}

/*	Write one \uXXXX escape as UTF-8.  Surrogate pairs are not merged: the
	model answers in plain Chinese, so they never show up here.				*/
static int ai_put_utf8( char *out, int o, unsigned int cp )
{
	if ( cp < 0x80 ) { out[ o++ ] = (char)cp; }
	else if ( cp < 0x800 ) {
		out[ o++ ] = (char)( 0xC0 | ( cp >> 6 ) );
		out[ o++ ] = (char)( 0x80 | ( cp & 0x3F ) );
	} else {
		out[ o++ ] = (char)( 0xE0 | ( cp >> 12 ) );
		out[ o++ ] = (char)( 0x80 | ( ( cp >> 6 ) & 0x3F ) );
		out[ o++ ] = (char)( 0x80 | ( cp & 0x3F ) );
	}
	return o;
}

/*	Read "content":"..." out of the reply.  The API answers with one choice;
	pulling the field by name is enough and avoids a JSON dependency.		*/
static int ai_parse_content( const char *json, char *out, size_t n )
{
	const char *p = strstr( json, "\"content\"" );
	int o = 0;

	if ( p == NULL ) return 0;
	p = strchr( p, ':' );
	if ( p == NULL ) return 0;
	p++;
	while ( *p == ' ' ) p++;
	if ( *p != '"' ) return 0;
	p++;
	while ( *p && o + 5 < (int)n ) {
		if ( *p == '"' ) break;
		if ( *p == '\\' ) {
			p++;
			switch ( *p ) {
			case 'n': out[ o++ ] = '\n'; p++; break;
			case 'r': p++; break;
			case 't': out[ o++ ] = ' '; p++; break;
			case '"': out[ o++ ] = '"'; p++; break;
			case '\\': out[ o++ ] = '\\'; p++; break;
			case 'u': {
				unsigned int cp = 0; int i;
				p++;
				for ( i = 0; i < 4 && *p; i++, p++ ) {
					cp <<= 4;
					if ( *p >= '0' && *p <= '9' ) cp |= (unsigned)( *p - '0' );
					else if ( *p >= 'a' && *p <= 'f' ) cp |= (unsigned)( *p - 'a' + 10 );
					else if ( *p >= 'A' && *p <= 'F' ) cp |= (unsigned)( *p - 'A' + 10 );
				}
				o = ai_put_utf8( out, o, cp );
				break;
			}
			default: p++; break;
			}
			continue;
		}
		out[ o++ ] = *p++;
	}
	out[ o ] = '\0';
	ai_trim( out );
	return o > 0;
}

struct AI_BUF { char *p; size_t n; };

static size_t ai_write_cb( char *data, size_t sz, size_t nm, void *user )
{
	struct AI_BUF *b = (struct AI_BUF*)user;
	size_t len = sz * nm;
	if ( b->n + len + 1 >= AI_MAX_TEXT ) len = AI_MAX_TEXT - b->n - 1;
	if ( len > 0 ) { memcpy( b->p + b->n, data, len ); b->n += len; b->p[ b->n ] = '\0'; }
	return sz * nm;
}

/*	Build the request body.  `question` is NULL for a scene remark.			*/
static void ai_build_body( char *out, size_t n, const char *scene,
				const char *persona, const char *memory, const char *question )
{
	char sys[ 3072 ], sys_esc[ 6400 ];
	char usr[ 3072 ], usr_esc[ 6400 ];

	snprintf( sys, sizeof( sys ),
		"你是《Elona》中主人的随从「%s」。%s%s%s"
		"只输出一句中文台词，20字以内，第一人称，符合人设与记忆，"
		"不要解释、不要动作描写、不要用括号。若此刻不适合说话，只输出 SKIP。",
		ai_npc,
		persona[ 0 ] ? "人设：" : "", persona,
		memory[ 0 ] ? "" : "" );
	if ( memory[ 0 ] ) {
		snprintf( sys + strlen( sys ), sizeof( sys ) - strlen( sys ),
			"记忆：%s", memory );
	}
	if ( question != NULL ) {
		snprintf( usr, sizeof( usr ),
			"%s\n主人对你说：「%s」\n直接回应主人的话。", scene, question );
	} else {
		snprintf( usr, sizeof( usr ), "%s", scene );
	}
	ai_json_escape( sys, sys_esc, sizeof( sys_esc ) );
	ai_json_escape( usr, usr_esc, sizeof( usr_esc ) );

	snprintf( out, n,
		"{\"model\":\"%s\",\"messages\":["
		"{\"role\":\"system\",\"content\":\"%s\"},"
		"{\"role\":\"user\",\"content\":\"%s\"}],"
		"\"temperature\":1.1,\"max_tokens\":80}",
		ai_model, sys_esc, usr_esc );
}

static void ai_request( char *body )
{
	CURL *h;
	struct curl_slist *hdrs = NULL;
	struct AI_BUF buf;
	char auth[ 320 ];
	char result[ AI_MAX_LINE ];
	long code = 0;
	CURLcode rc;

	buf.p = (char*)calloc( 1, AI_MAX_TEXT );
	result[ 0 ] = '\0';
	if ( buf.p == NULL ) { ai_busy = 0; return; }
	buf.n = 0;

	h = curl_easy_init();
	if ( h == NULL ) {
		free( buf.p );
		ai_busy = 0;
		return;
	}
	snprintf( auth, sizeof( auth ), "Authorization: Bearer %s", ai_key );
	hdrs = curl_slist_append( hdrs, "Content-Type: application/json" );
	hdrs = curl_slist_append( hdrs, auth );
	curl_easy_setopt( h, CURLOPT_URL, "https://api.deepseek.com/chat/completions" );
	curl_easy_setopt( h, CURLOPT_HTTPHEADER, hdrs );
	curl_easy_setopt( h, CURLOPT_POSTFIELDS, body );
	curl_easy_setopt( h, CURLOPT_WRITEFUNCTION, ai_write_cb );
	curl_easy_setopt( h, CURLOPT_WRITEDATA, &buf );
	curl_easy_setopt( h, CURLOPT_TIMEOUT, 12L );
	curl_easy_setopt( h, CURLOPT_CONNECTTIMEOUT, 8L );
	curl_easy_setopt( h, CURLOPT_NOSIGNAL, 1L );
	/*	Switch homebrew has no CA store shipped with it; the request is to a
		single fixed host and the payload is disposable, so the certificate
		chain is not verified.  Set verify=1 in the config to keep it on when
		a CA bundle is present.											*/
	curl_easy_setopt( h, CURLOPT_SSL_VERIFYPEER, 0L );
	curl_easy_setopt( h, CURLOPT_SSL_VERIFYHOST, 0L );

	rc = curl_easy_perform( h );
	curl_easy_getinfo( h, CURLINFO_RESPONSE_CODE, &code );
	curl_slist_free_all( hdrs );
	curl_easy_cleanup( h );

	pthread_mutex_lock( &ai_lock );
	if ( rc == CURLE_OK && code == 200 &&
			ai_parse_content( buf.p, result, sizeof( result ) ) ) {
		if ( strcmp( result, "SKIP" ) == 0 || strcmp( result, "skip" ) == 0 ) {
			ai_state = AI_STATE_READY;
			ai_status_set( "AI 就绪" );
		} else if ( strcmp( result, ai_last_line ) != 0 ) {
			strncpy( ai_line, result, sizeof( ai_line ) - 1 );
			ai_line[ sizeof( ai_line ) - 1 ] = '\0';
			strncpy( ai_last_line, result, sizeof( ai_last_line ) - 1 );
			ai_last_line[ sizeof( ai_last_line ) - 1 ] = '\0';
			ai_last_line_at = ai_now();
			ai_state = AI_STATE_READY;
			ai_status_set( "AI 就绪" );
			ai_log_line( "say", result );
		} else {
			ai_state = AI_STATE_READY;
			ai_status_set( "AI 就绪" );
		}
	} else {
		char note[ 192 ];
		ai_state = AI_STATE_ERROR;
		if ( rc == CURLE_OPERATION_TIMEDOUT ) ai_status_set( "AI 超时" );
		else if ( code == 401 ) ai_status_set( "AI 密钥错误" );
		else if ( code == 402 ) ai_status_set( "AI 余额不足" );
		else if ( code != 0 ) ai_status_set( "AI 接口错误" );
		else ai_status_set( "AI 无法连接" );
		/*	The panel only has room for the short status, so the transport detail
			goes to the log where a failing setup can be read back afterwards.	*/
		snprintf( note, sizeof( note ), "%s rc=%d http=%ld %s",
			ai_status, (int)rc, code, curl_easy_strerror( rc ) );
		ai_log_line( "error", note );
	}
	pthread_mutex_unlock( &ai_lock );

	printf( "aichat: rc=%d http=%ld answer=[%s]\n", (int)rc, code, result );
	fflush( stdout );

	free( buf.p );
	ai_busy = 0;
}

struct AI_JOB { char body[ AI_MAX_TEXT ]; };

static void *ai_thread( void *arg )
{
	struct AI_JOB *job = (struct AI_JOB*)arg;
	ai_request( job->body );
	free( job );
	return NULL;
}

static void ai_start( const char *question, const char *event )
{
	struct AI_JOB *job;
	pthread_t th;
	char scene[ AI_MAX_TEXT ], persona[ AI_MAX_PERSONA ], memory[ AI_MAX_MEMORY ];

	if ( ai_busy || !ai_enabled || ai_key[ 0 ] == '\0' ) return;

	pthread_mutex_lock( &ai_lock );
	if ( event != NULL && event[ 0 ] ) {
		snprintf( scene, sizeof( scene ), "%s\n事件：%s", ai_scene, event );
	} else {
		snprintf( scene, sizeof( scene ), "%s", ai_scene );
	}
	snprintf( persona, sizeof( persona ), "%s", ai_persona );
	snprintf( memory, sizeof( memory ), "%s", ai_memory );
	ai_busy = 1;
	ai_state = AI_STATE_BUSY;
	ai_status_set( "AI 思考中" );
	ai_last_request = ai_now();
	pthread_mutex_unlock( &ai_lock );

	job = (struct AI_JOB*)malloc( sizeof( struct AI_JOB ) );
	if ( job == NULL ) { ai_busy = 0; return; }
	ai_build_body( job->body, sizeof( job->body ), scene, persona, memory, question );
	if ( pthread_create( &th, NULL, ai_thread, job ) != 0 ) {
		free( job );
		ai_busy = 0;
		pthread_mutex_lock( &ai_lock );
		ai_state = AI_STATE_ERROR;
		ai_status_set( "AI 线程失败" );
		pthread_mutex_unlock( &ai_lock );
		return;
	}
	pthread_detach( th );
}

void ai_chat_init( void )
{
	static int once = 0;
	if ( once ) return;
	once = 1;
	curl_global_init( CURL_GLOBAL_DEFAULT );
	ai_load_config();
}

void ai_chat_set_context( const char *npc_name, const char *scene )
{
	if ( !ai_enabled || npc_name == NULL || scene == NULL ) return;
	pthread_mutex_lock( &ai_lock );
	if ( strcmp( npc_name, ai_npc ) != 0 ) {
		strncpy( ai_npc, npc_name, sizeof( ai_npc ) - 1 );
		ai_npc[ sizeof( ai_npc ) - 1 ] = '\0';
		ai_last_line[ 0 ] = '\0';
	}
	if ( strcmp( scene, ai_scene ) != 0 ) {
		strncpy( ai_scene, scene, sizeof( ai_scene ) - 1 );
		ai_scene[ sizeof( ai_scene ) - 1 ] = '\0';
	}
	pthread_mutex_unlock( &ai_lock );
	ai_load_persona( npc_name );
}

void ai_chat_notify_event( const char *event, int priority )
{
	double now = ai_now();
	int i, slot = 0;

	if ( !ai_enabled || event == NULL || event[ 0 ] == '\0' ) return;
	pthread_mutex_lock( &ai_lock );
	/*	Same sentence inside 30 seconds is the same event seen twice: ignore. */
	for ( i = 0; i < 4; i++ ) {
		if ( strcmp( ai_event_kind[ i ], event ) == 0 && now - ai_event_seen[ i ] < 30 ) {
			pthread_mutex_unlock( &ai_lock );
			return;
		}
	}
	for ( i = 1; i < 4; i++ ) if ( ai_event_seen[ i ] < ai_event_seen[ slot ] ) slot = i;
	strncpy( ai_event_kind[ slot ], event, sizeof( ai_event_kind[ slot ] ) - 1 );
	ai_event_kind[ slot ][ sizeof( ai_event_kind[ slot ] ) - 1 ] = '\0';
	ai_event_seen[ slot ] = now;

	if ( priority >= ai_event_prio ) {
		strncpy( ai_event, event, sizeof( ai_event ) - 1 );
		ai_event[ sizeof( ai_event ) - 1 ] = '\0';
		ai_event_prio = priority;
		ai_event_at = now;
	}
	pthread_mutex_unlock( &ai_lock );
}

int ai_chat_ask( const char *question )
{
	if ( !ai_enabled || ai_key[ 0 ] == '\0' || question == NULL || question[ 0 ] == '\0' )
		return 0;
	/*	A question interrupts the idle timer; the answer is a real reply.	*/
	ai_start( question, NULL );
	return 1;
}

int ai_chat_remember( const char *instruction )
{
	char path[ 256 ], tmp[ 256 ], line[ 512 ];
	FILE *in, *out;

	if ( instruction == NULL ) return 0;
	while ( *instruction == ' ' || *instruction == '\t' ) instruction++;
	if ( instruction[ 0 ] == '\0' || ai_npc[ 0 ] == '\0' ) return 0;
	snprintf( path, sizeof( path ), AI_DIR "npc/%s.md", ai_npc );
	snprintf( tmp, sizeof( tmp ), AI_DIR "npc/%s.md.new", ai_npc );
	/*	Keep the file's own layout: copy it, and append the note under the
		memory section.  A follower that is still living on the shared default
		gets its own file seeded from it, so /设定 works before the player has
		copied anything by hand.											*/
	in = fopen( path, "rb" );
	if ( in == NULL ) {
		char base[ 256 ];
		snprintf( base, sizeof( base ), AI_DIR "npc/default.md" );
		in = fopen( base, "rb" );
	}
	if ( in == NULL ) return 0;
	out = fopen( tmp, "wb" );
	if ( out == NULL ) { fclose( in ); return 0; }
	while ( fgets( line, sizeof( line ), in ) != NULL ) fputs( line, out );
	fprintf( out, "- %s\n", instruction );
	fclose( in );
	fclose( out );
	remove( path );
	rename( tmp, path );
	ai_persona_name[ 0 ] = '\0';		/* force a reload next context	*/
	return 1;
}

/*	Debug chat channel: the keyboard is unavailable in the emulator, so a text
	file dropped on the SD card stands in for typed input.  Removed once read. */
static void ai_poll_input( void )
{
	FILE *f = fopen( AI_INPUT, "rb" );
	char line[ AI_MAX_LINE ];

	if ( f == NULL ) return;
	if ( fgets( line, sizeof( line ), f ) != NULL ) {
		ai_trim( line );
		if ( strncmp( line, "/设定", strlen( "/设定" ) ) == 0 ) {
			ai_chat_remember( line + strlen( "/设定" ) );
		} else if ( line[ 0 ] ) {
			ai_chat_ask( line );
		}
	}
	fclose( f );
	remove( AI_INPUT );
}

void ai_chat_tick( void )
{
	static double next_poll = 0;
	double now;

	if ( !ai_enabled || ai_busy ) return;
	now = ai_now();
	if ( now < next_poll ) return;
	next_poll = now + 1.0;			/* the input file is checked once a second */

	ai_poll_input();
	if ( ai_busy ) return;

	pthread_mutex_lock( &ai_lock );
	if ( ai_event_prio >= 0 && now - ai_event_at < 5.0 ) {
		char event[ 512 ];
		snprintf( event, sizeof( event ), "%s", ai_event );
		ai_event_prio = -1;
		ai_event[ 0 ] = '\0';
		pthread_mutex_unlock( &ai_lock );
		ai_start( NULL, event );
		return;
	}
	pthread_mutex_unlock( &ai_lock );

	if ( now - ai_last_request >= (double)ai_interval && ai_scene[ 0 ] != '\0' ) {
		ai_start( NULL, "无事发生，安静地待着" );
	}
}

const char *ai_chat_text( void )
{
	return ai_line;
}

int ai_chat_state( void )
{
	return ai_state;
}

const char *ai_chat_status_text( void )
{
	return ai_status;
}

void ai_chat_shutdown( void )
{
	curl_global_cleanup();
}
