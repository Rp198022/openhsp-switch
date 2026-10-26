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
#include "switch_overlay.h"

#define AI_MAX_TEXT		20480	/* request body / answer scratch; three
								   replayed turns ride along from r283	*/
#define AI_MAX_LINE		512		/* one spoken line					*/
#define AI_MAX_PERSONA	3072	/* r382: room for the event-picture rule */
#define AI_MAX_MEMORY	4096
#define AI_EVENT_KINDS	8		/* recent event sentences kept for dedup */
#define AI_DIR			"sdmc:/switch/openhsp/data/"
#define AI_CONFIG		AI_DIR "ai_config.txt"
#define AI_LOG			AI_DIR "npc_log.txt"
#define AI_INPUT		AI_DIR "ai_input.txt"	/* debug chat channel	*/

static int			ai_enabled = 0;			/* config says on			*/
static int			ai_interval = 40;		/* seconds between idle lines */
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
/*	r272: the panel keeps the last three lines on screen, so the module
	remembers them instead of only the newest one.							*/
static char			ai_hist[ AI_HISTORY ][ AI_MAX_LINE ];
static int			ai_hist_n = 0;
static int			ai_seq = 0;							/* bumps on every shown line */
/*	r283: the last few question/answer pairs are sent back with the next
	question, so the model can follow a conversation instead of treating
	every line as the first one.  Scene remarks are not kept - they have no
	user half and their block is long - only real questions are.		*/
#define AI_TURNS	3
static char			ai_turn_q[ AI_TURNS ][ AI_MAX_LINE ];
static char			ai_turn_a[ AI_TURNS ][ AI_MAX_LINE ];
static int			ai_turn_n = 0;
static char			ai_pending[ AI_MAX_LINE ] = "";		/* question waiting its turn */
static int			ai_has_pending = 0;
static int			ai_state = AI_STATE_OFF;
static char			ai_status[ 96 ] = "AI 未配置";
static int			ai_busy = 0;
static int			ai_idle = 0;		/* player stopped acting		*/
static double		ai_busy_at = 0;		/* when the running request started */
static double		ai_last_request = 0;
static double		ai_last_line_at = 0;
static double		ai_event_seen[ AI_EVENT_KINDS ];		/* 最近的同类事件时间	*/
static char			ai_event_kind[ AI_EVENT_KINDS ][ 96 ];

/*	Persona and memory, read once per follower.							*/
static char			ai_persona_name[ 64 ] = "";
static char			ai_persona[ AI_MAX_PERSONA ] = "";
static char			ai_tone[ 1024 ] = "";		/* 关系语气				*/
static char			ai_example[ 2048 ] = "";	/* 示例对话				*/
static char			ai_memory[ AI_MAX_MEMORY ] = "";
static double		ai_last_compress = 0;
static int			ai_compress_tried = 0;

/*	r303: the expression set.  A follower may ship
	  data/npc/<名字>/<情绪>.png  plus an emotions.txt listing the names.
	The model appends [[emo:词]] to a line, the tag is stripped before the
	line is shown, and the panel swaps the portrait for that expression.	*/
static char			ai_emotion[ 64 ] = "";			/* current expression	*/
static char			ai_emotion_default[ 64 ] = "";	/* first name in list	*/
static char			ai_emotion_calm[ 64 ] = "";		/* 「平静」 if present	*/
static char			ai_emotion_list[ 2048 ] = "";	/* "平静、微笑、…"		*/
static int			ai_emotion_n = 0;
/*	r305: the md/chat/expression folder a display name maps to.  Renaming a
	pet changes cdatan, so alias.txt keeps the old files in use.			*/
static char			ai_base_name[ 64 ] = "";

static void ai_load_emotions( const char *npc );	/* defined below		*/

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

/*	Read a whole small file.  The port's fopen wrapper caches "this path is
	missing" answers (glue_switch.cpp, glue_miss_seen) and only re-probes a
	cached path after the filesystem is reported as changed.  A file that
	appears later - ai_input.txt, or a follower's markdown written by /设定 -
	stayed invisible for that reason, so every read here announces a change
	first.  glue_fs_mutated() is the wrapper's own invalidation hook.

	Returns the byte count, or -1 when the file cannot be opened.		*/
extern "C" void glue_fs_mutated( void );

static int ai_read_file( const char *path, char *buf, int n )
{
	FILE *f;
	size_t r;

	if ( path == NULL || buf == NULL || n < 2 ) return -1;
	glue_fs_mutated();
	f = fopen( path, "rb" );
	if ( f == NULL ) return -1;
	r = fread( buf, 1, (size_t)n - 1, f );
	fclose( f );
	buf[ r ] = '\0';
	return (int)r;
}

/*	Copy the next line out of *pp, advancing it past the terminator.		*/
static int ai_next_line( const char **pp, char *out, int n )
{
	const char *p;
	int i = 0;

	if ( pp == NULL || *pp == NULL || **pp == '\0' ) return 0;
	p = *pp;
	while ( *p && *p != '\n' && i < n - 1 ) out[ i++ ] = *p++;
	if ( *p == '\n' ) p++;
	else if ( *p == '\r' ) p++;
	out[ i ] = '\0';
	*pp = p;
	return 1;
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
	static char data[ 4096 ];
	char line[ 512 ];
	const char *p;

	if ( ai_read_file( AI_CONFIG, data, sizeof( data ) ) < 0 ) {
		ai_enabled = 0;
		ai_state = AI_STATE_OFF;
		ai_status_set( "AI 未配置" );
		return;
	}
	p = data;
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
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

/*	Pull the four sections out of the follower's markdown:
		## 人设       permanent facts
		## 关系语气   how the two relationship scales colour the speech
		## 示例       example exchanges - the strongest style anchor
		## 记忆       things learned during play
	Anything else is ignored, so the file stays hand-editable.			*/
static void ai_append( char *dst, size_t cap, const char *line )
{
	size_t l = strlen( dst );
	while ( *line == ' ' || *line == '\t' ) line++;
	if ( l + strlen( line ) + 2 >= cap ) return;
	memcpy( dst + l, line, strlen( line ) );
	dst[ l + strlen( line ) ] = '\n';
	dst[ l + strlen( line ) + 1 ] = '\0';
}

static int ai_has_file( const char *path )
{
	FILE *f;

	glue_fs_mutated();
	f = fopen( path, "rb" );
	if ( f == NULL ) return 0;
	fclose( f );
	return 1;
}

/*	r305: map a display name to the folder that holds her files.  Renaming a
	pet in game only changes cdatan, so data/npc/alias.txt ("新名=旧名", one
	per line) keeps the persona, the history and the expression folder in
	use without rewriting anything.											*/
static void ai_resolve_base( const char *npc, char *out, size_t n )
{
	char path[ 256 ], data[ 2048 ], line[ 200 ];
	const char *p;

	snprintf( out, n, "%s", npc );
	snprintf( path, sizeof( path ), AI_DIR "npc/%s.md", npc );
	if ( ai_has_file( path ) ) return;
	if ( ai_read_file( AI_DIR "npc/alias.txt", data, sizeof( data ) ) < 0 ) return;
	p = data;
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
		char *eq;

		ai_trim( line );
		if ( line[ 0 ] == '#' || line[ 0 ] == '\0' ) continue;
		eq = strchr( line, '=' );
		if ( eq == NULL ) continue;
		*eq = '\0';
		ai_trim( line );
		ai_trim( eq + 1 );
		if ( strcmp( line, npc ) == 0 && ( eq + 1 )[ 0 ] != '\0' ) {
			snprintf( out, n, "%s", eq + 1 );
			return;
		}
	}
}

static void ai_load_persona( const char *npc )
{
	static char data[ 8192 ];
	char path[ 256 ];
	char line[ 512 ];
	const char *p;
	int section = 0;	/* 1 persona, 2 memory, 3 tone, 4 examples	*/
	size_t ml;

	if ( npc == NULL || npc[ 0 ] == '\0' ) return;
	if ( strcmp( npc, ai_persona_name ) == 0 ) return;
	strncpy( ai_persona_name, npc, sizeof( ai_persona_name ) - 1 );
	ai_persona_name[ sizeof( ai_persona_name ) - 1 ] = '\0';
	ai_persona[ 0 ] = '\0';
	ai_memory[ 0 ] = '\0';
	ai_tone[ 0 ] = '\0';
	ai_example[ 0 ] = '\0';
	ai_resolve_base( npc, ai_base_name, sizeof( ai_base_name ) );

	snprintf( path, sizeof( path ), AI_DIR "npc/%s.md", ai_base_name );
	if ( ai_read_file( path, data, sizeof( data ) ) < 0 ) {
		/*	A save whose follower has no file of its own still gets the shared
			default, so one persona can be tried before writing everyone's.	*/
		snprintf( path, sizeof( path ), AI_DIR "npc/default.md" );
		if ( ai_read_file( path, data, sizeof( data ) ) < 0 ) {
			printf( "aichat: no persona for %s\n", npc );
			fflush( stdout );
			return;
		}
	}
	p = data;
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
		if ( strncmp( line, "##", 2 ) == 0 ) {
			if ( strstr( line, "人设" ) != NULL ) section = 1;
			else if ( strstr( line, "记忆" ) != NULL ) section = 2;
			else if ( strstr( line, "关系语气" ) != NULL ) section = 3;
			else if ( strstr( line, "示例" ) != NULL ) section = 4;
			else if ( strstr( line, "事件图" ) != NULL ) section = 1;
			else section = 0;
			continue;
		}
		if ( section == 0 || line[ 0 ] == '\0' ) continue;
		ml = strlen( ai_memory );
		if ( section == 1 ) ai_append( ai_persona, sizeof( ai_persona ), line );
		else if ( section == 2 && ml + strlen( line ) + 2 < sizeof( ai_memory ) ) {
			/*	Memory keeps its own line breaks: each entry is one fact and
				the order carries meaning (later overrides earlier).		*/
			strcat( ai_memory, line );
			strcat( ai_memory, "\n" );
		}
		else if ( section == 3 ) ai_append( ai_tone, sizeof( ai_tone ), line );
		else if ( section == 4 ) ai_append( ai_example, sizeof( ai_example ), line );
	}
	ai_trim( ai_persona );
	ai_trim( ai_memory );
	ai_trim( ai_tone );
	ai_trim( ai_example );
	ai_load_emotions( ai_base_name );
	printf( "aichat: persona %s (base %s, %d bytes, memory %d, tone %d, example %d)\n",
		npc, ai_base_name,
		(int)strlen( ai_persona ), (int)strlen( ai_memory ),
		(int)strlen( ai_tone ), (int)strlen( ai_example ) );
	fflush( stdout );
}

/*	Read data/npc/<名字>/emotions.txt.  One name per line, the first one is
	the fallback the prompt tells the model to use when it cannot decide.
	Rejects anything that could walk out of the folder, because the name
	becomes a file name later.											*/
static void ai_load_emotions( const char *npc )
{
	static char data[ 8192 ], line[ 80 ], path[ 256 ];
	const char *p;
	size_t o = 0;
	int n = 0, calm = 0;

	ai_emotion[ 0 ] = '\0';
	ai_emotion_default[ 0 ] = '\0';
	ai_emotion_calm[ 0 ] = '\0';
	ai_emotion_list[ 0 ] = '\0';
	ai_emotion_n = 0;
	if ( npc == NULL || npc[ 0 ] == '\0' ) return;
	snprintf( path, sizeof( path ), AI_DIR "npc/%s/emotions.txt", npc );
	if ( ai_read_file( path, data, sizeof( data ) ) < 0 ) return;
	p = data;
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
		ai_trim( line );
		if ( line[ 0 ] == '\0' ) continue;
		if ( strchr( line, '/' ) != NULL || strchr( line, '\\' ) != NULL ||
				strchr( line, ':' ) != NULL ) continue;
		if ( o + strlen( line ) + 4 >= sizeof( ai_emotion_list ) ) break;
		o += (size_t)snprintf( ai_emotion_list + o, sizeof( ai_emotion_list ) - o,
			"%s%s", n > 0 ? "、" : "", line );
		if ( n == 0 ) snprintf( ai_emotion_default, sizeof( ai_emotion_default ), "%s", line );
		if ( strcmp( line, "平静" ) == 0 ) calm = 1;
		n++;
	}
	ai_emotion_n = n;
	if ( n > 0 ) {
		/*	r305: 「平静」 is the resting face when the set has one.			*/
		snprintf( ai_emotion_calm, sizeof( ai_emotion_calm ), "%s",
			calm ? "平静" : ai_emotion_default );
		snprintf( ai_emotion, sizeof( ai_emotion ), "%s", ai_emotion_calm );
		printf( "aichat: %d expressions for %s, calm=%s\n", n, npc, ai_emotion_calm );
		fflush( stdout );
	}
}

static int ai_emotion_ok( const char *name )
{
	const char *p = ai_emotion_list;

	if ( name == NULL || name[ 0 ] == '\0' ) return 0;
	while ( *p ) {
		const char *e = strstr( p, "、" );
		size_t l = e != NULL ? (size_t)( e - p ) : strlen( p );
		if ( strlen( name ) == l && strncmp( p, name, l ) == 0 ) return 1;
		if ( e == NULL ) break;
		p = e + 3;						/* "、" is three bytes in UTF-8	*/
	}
	return 0;
}

/*	r309: the model writes [emo:词] as often as [[emo:词]], and sometimes
	with a full-width colon.  The old parser only knew the double bracket,
	so most tags stayed inside the line and the portrait never changed.
	Returns a pointer at the "emo" word; the caller walks back over the
	brackets so it can cut the whole tag out.							*/
static char *ai_emo_open( char *t )
{
	char *p = t;

	while ( ( p = strstr( p, "emo" ) ) != NULL ) {
		if ( p > t && p[ -1 ] == '[' ) {
			int ok = 0;

			if ( p[ 3 ] == ':' ) ok = 1;
			else if ( (unsigned char)p[ 3 ] == 0xEF && (unsigned char)p[ 4 ] == 0xBC &&
					(unsigned char)p[ 5 ] == 0x9A ) ok = 1;		/* ：	*/
			if ( ok ) return p;
		}
		p += 3;
	}
	return NULL;
}

/*	Pull "[[emo:词]]" (or "[emo:词]", "[[emo：词]]") out of the answer.
	An unknown word is ignored so the portrait does not flicker; the tag
	itself never reaches the panel, the history or the log.				*/
static void ai_take_emotion( char *text )
{
	char name[ 64 ];

	if ( text == NULL ) return;
	for ( ;; ) {
		char *em = ai_emo_open( text );
		char *a, *e, *half, *full, *ns;
		size_t len, cl;

		if ( em == NULL ) break;
		a = em;
		while ( a > text && a[ -1 ] == '[' ) a--;
		half = strchr( em, ']' );
		full = strstr( em, "\xEF\xBC\xBD" );		/* ］				*/
		e = half;
		cl = 1;
		if ( full != NULL && ( e == NULL || full < e ) ) { e = full; cl = 3; }
		if ( e == NULL ) {
			/*	Unclosed tag: cut it off rather than show it to the player.	*/
			*a = '\0';
			ai_trim( text );
			break;
		}
		ns = em + 3;
		if ( ns[ 0 ] == ':' ) ns += 1;
		else if ( (unsigned char)ns[ 0 ] == 0xEF && (unsigned char)ns[ 1 ] == 0xBC &&
				(unsigned char)ns[ 2 ] == 0x9A ) ns += 3;
		len = (size_t)( e - ns );
		if ( len < sizeof( name ) ) {
			memcpy( name, ns, len );
			name[ len ] = '\0';
			ai_trim( name );
			if ( ai_emotion_ok( name ) )
				snprintf( ai_emotion, sizeof( ai_emotion ), "%s", name );
		}
		memmove( a, e + cl, strlen( e + cl ) + 1 );
		ai_trim( text );
	}
	/*	r311: a stray closing bracket survives when the model writes
		"[emo:平静]]" or drops one on its own; strip whatever is left.		*/
	for ( ;; ) {
		size_t tl = strlen( text );
		char *q = text + tl;

		while ( q > text && ( q[ -1 ] == ' ' || q[ -1 ] == '\t' ) ) q--;
		if ( q > text && q[ -1 ] == ']' ) { q[ -1 ] = '\0'; ai_trim( text ); continue; }
		if ( q - text >= 3 && (unsigned char)q[ -3 ] == 0xEF &&
				(unsigned char)q[ -2 ] == 0xBC && (unsigned char)q[ -1 ] == 0xBD ) {
			q[ -3 ] = '\0';
			ai_trim( text );
			continue;
		}
		break;
	}
}

/*	r379: the model stages a scene with [[cg:标签]].  Like the expression
	tag it is metadata: it is cut out of the line, the picture it names is
	held on screen until the player presses confirm.  Only a request is
	recorded here - this runs on the worker thread, where the GL context
	is not current.														*/
static void ai_take_cg( char *text )
{
	printf( "aichat: cg scan in\n" );
	fflush( stdout );
	if ( text == NULL ) return;
	for ( ;; ) {
		char *p = text, *a, *e, *ns;
		char name[ 32 ];
		size_t len, cl;
		int found = 0;

		while ( ( p = strstr( p, "cg" ) ) != NULL ) {
			if ( p > text && p[ -1 ] == '[' ) {
				int colon = 0;

				if ( p[ 2 ] == ':' ) colon = 1;
				else if ( (unsigned char)p[ 2 ] == 0xEF && (unsigned char)p[ 3 ] == 0xBC &&
						(unsigned char)p[ 4 ] == 0x9A ) colon = 1;
				if ( colon ) { found = 1; break; }
			}
			p += 2;
		}
		if ( !found ) break;
		a = p;
		while ( a > text && a[ -1 ] == '[' ) a--;
		{
			char *half = strchr( p, ']' );
			char *full = strstr( p, "\xEF\xBC\xBD" );		/* ］				*/

			e = half;
			cl = 1;
			if ( full != NULL && ( e == NULL || full < e ) ) { e = full; cl = 3; }
		}
		if ( e == NULL ) {
			*a = '\0';
			ai_trim( text );
			break;
		}
		ns = p + 2;
		if ( ns[ 0 ] == ':' ) ns += 1;
		else if ( (unsigned char)ns[ 0 ] == 0xEF && (unsigned char)ns[ 1 ] == 0xBC &&
				(unsigned char)ns[ 2 ] == 0x9A ) ns += 3;
		len = (size_t)( e - ns );
		if ( len > 0 && len < sizeof( name ) ) {
			memcpy( name, ns, len );
			name[ len ] = '\0';
			ai_trim( name );
			printf( "aichat: cg tag [%s] len=%d\n", name, (int)len );
			fflush( stdout );
			if ( name[ 0 ] ) switch_overlay_cg_request( name );
			printf( "aichat: cg request returned\n" );
			fflush( stdout );
		}
		memmove( a, e + cl, strlen( e + cl ) + 1 );
		ai_trim( text );
	}
}

const char *ai_chat_emotion( void )
{
	return ai_emotion;
}

void ai_chat_emotion_calm( void )
{
	if ( ai_emotion_calm[ 0 ] == '\0' ) return;
	snprintf( ai_emotion, sizeof( ai_emotion ), "%s", ai_emotion_calm );
}

const char *ai_chat_face_dir( void )
{
	return ai_base_name[ 0 ] ? ai_base_name : ai_npc;
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
	quotes are the only ones that need it; the text itself stays UTF-8.
	A byte that is not part of a well-formed sequence becomes '?': the game
	stores log lines in GBK and one stray byte anywhere in the body makes the
	API reject the whole request with 400.								*/
static void ai_json_escape( const char *in, char *out, size_t n )
{
	size_t o = 0;
	const unsigned char *p = (const unsigned char*)in;

	while ( *p && o + 8 < n ) {
		if ( *p < 0x80 ) {
			if ( *p == '"' || *p == '\\' ) { out[ o++ ] = '\\'; out[ o++ ] = (char)*p++; }
			else if ( *p == '\n' ) { out[ o++ ] = '\\'; out[ o++ ] = 'n'; p++; }
			else if ( *p == '\r' ) { p++; }
			else if ( *p == '\t' ) { out[ o++ ] = ' '; p++; }
			else if ( *p < 0x20 || *p == 0x7F ) { p++; }
			else out[ o++ ] = (char)*p++;
			continue;
		}
		/*	Multi-byte sequence: the lead byte says how long it is, and every
			continuation byte must carry the 10xxxxxx prefix.			*/
		{
			int len = ( *p & 0xE0 ) == 0xC0 ? 2 :
					( *p & 0xF0 ) == 0xE0 ? 3 :
					( *p & 0xF8 ) == 0xF0 ? 4 : 0;
			int k, ok = len > 0;
			for ( k = 1; ok && k < len; k++ )
				if ( ( p[ k ] & 0xC0 ) != 0x80 ) ok = 0;
			if ( ok && o + len < n ) {
				for ( k = 0; k < len; k++ ) out[ o++ ] = (char)p[ k ];
				p += len;
			} else {
				out[ o++ ] = '?';
				p++;
			}
		}
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

/*	World book ------------------------------------------------------------
	data/npc/world/*.md, generated on the PC side and shipped inside the SD
	folder.  One entry per block:

		## 韦尔尼斯
		韦尔尼斯，Vernis，矿镇
		<正文>

	Nothing is downloaded here: a matched entry is copied into the system
	prompt, so a follower can answer "韦尔尼斯是什么样的" without paying for
	a search.  The files are read once per run - an edited entry only shows
	up after a restart, same as the persona markdown.						*/
#define AI_WORLD_FILES	5
#define AI_WORLD_MAX	96
#define AI_WORLD_NAME	48
#define AI_WORLD_KEYS	128
#define AI_WORLD_BODY	1024
#define AI_WORLD_SHOW	6
#define AI_WORLD_BUDGET	2600	/* bytes of entry text, Chinese is 3/char	*/
#define AI_WORLD_RECENT	3

typedef struct {
	char	name[ AI_WORLD_NAME ];
	char	keys[ AI_WORLD_KEYS ];
	char	body[ AI_WORLD_BODY ];
} AI_WORLD_ENTRY;

static AI_WORLD_ENTRY	ai_world[ AI_WORLD_MAX ];
static int				ai_world_n = -1;				/* -1 = not read yet */
static char				ai_world_recent[ AI_WORLD_RECENT ][ AI_WORLD_NAME ];
static int				ai_world_recent_pos = 0;

static void ai_world_push( const char *name, const char *keys, const char *body )
{
	AI_WORLD_ENTRY *e;

	if ( ai_world_n < 0 || ai_world_n >= AI_WORLD_MAX ) return;
	if ( name == NULL || name[ 0 ] == '\0' || body == NULL || body[ 0 ] == '\0' ) return;
	e = &ai_world[ ai_world_n++ ];
	snprintf( e->name, sizeof( e->name ), "%s", name );
	snprintf( e->keys, sizeof( e->keys ), "%s", keys != NULL ? keys : "" );
	snprintf( e->body, sizeof( e->body ), "%s", body );
}

static void ai_world_load( void )
{
	static const char *files[ AI_WORLD_FILES ] = {
		"places.md", "dungeons.md", "landmarks.md", "systems.md", "npcs.md" };
	static char data[ 65536 ];
	static char line[ 1024 ];
	int i;

	ai_world_n = 0;
	ai_world_recent_pos = 0;
	for ( i = 0; i < AI_WORLD_FILES; i++ ) {
		char path[ 256 ], name[ AI_WORLD_NAME ] = "", keys[ AI_WORLD_KEYS ] = "";
		char body[ AI_WORLD_BODY ] = "";
		size_t bl = 0;
		int stage = 0;			/* 0 idle, 1 saw "##", 2 read the key line */
		const char *p;

		snprintf( path, sizeof( path ), AI_DIR "npc/world/%s", files[ i ] );
		if ( ai_read_file( path, data, sizeof( data ) ) < 0 ) continue;
		p = data;
		while ( ai_next_line( &p, line, sizeof( line ) ) ) {
			ai_trim( line );
			if ( line[ 0 ] == '#' && line[ 1 ] == '#' ) {
				if ( stage >= 2 ) ai_world_push( name, keys, body );
				snprintf( name, sizeof( name ), "%s", line + 2 );
				ai_trim( name );
				keys[ 0 ] = '\0';
				body[ 0 ] = '\0';
				bl = 0;
				stage = 1;
				continue;
			}
			if ( stage == 1 ) {
				snprintf( keys, sizeof( keys ), "%s", line );
				stage = 2;
				continue;
			}
			if ( stage == 2 && line[ 0 ] != '\0' ) {
				/*	Clip instead of dropping: a body longer than the buffer
					used to leave `body` empty, which silently threw the whole
					entry away - half the book went missing that way.	*/
				size_t ll = strlen( line );
				if ( bl + ll + 1 >= sizeof( body ) ) ll = sizeof( body ) - 1 - bl;
				if ( ll > 0 ) {
					memcpy( body + bl, line, ll );
					bl += ll;
					body[ bl ] = '\0';
				}
			}
		}
		if ( stage >= 2 ) ai_world_push( name, keys, body );
	}
}

/*	Longest keyword of this entry found inside `hay`; 0 when none.
	Keys are comma separated - both the ASCII and the full-width comma are
	accepted because the generator writes whichever the model produced.		*/
static int ai_world_score( const AI_WORLD_ENTRY *e, const char *hay )
{
	char kw[ 64 ];
	int i, k = 0, best = 0;

	if ( hay == NULL || hay[ 0 ] == '\0' ) return 0;
	for ( i = 0; ; i++ ) {
		unsigned char c = (unsigned char)e->keys[ i ];
		int sep = 0;

		if ( c == '\0' || c == ',' || c == ' ' ) sep = 1;
		else if ( c == 0xEF && (unsigned char)e->keys[ i + 1 ] == 0xBC &&
				  (unsigned char)e->keys[ i + 2 ] == 0x8C ) sep = 1;	/* ，	*/
		else if ( c == 0xE3 && (unsigned char)e->keys[ i + 1 ] == 0x80 &&
				  (unsigned char)e->keys[ i + 2 ] == 0x81 ) sep = 1;	/* 、	*/
		if ( sep ) {
			kw[ k ] = '\0';
			ai_trim( kw );
			/*	Six bytes is two Chinese characters: one character alone
				("城", "家") would match half the questions.			*/
			if ( strlen( kw ) >= 6 && strstr( hay, kw ) != NULL &&
				 (int)strlen( kw ) > best ) best = (int)strlen( kw );
			k = 0;
			if ( c == '\0' ) break;
			if ( c >= 0xE0 ) i += 2;
			continue;
		}
		if ( k < (int)sizeof( kw ) - 1 ) kw[ k++ ] = e->keys[ i ];
	}
	return best;
}

static int ai_world_recent_has( const char *name )
{
	int i;
	for ( i = 0; i < AI_WORLD_RECENT; i++ )
		if ( ai_world_recent[ i ][ 0 ] && strcmp( ai_world_recent[ i ], name ) == 0 ) return 1;
	return 0;
}

static void ai_world_recent_add( const char *name )
{
	snprintf( ai_world_recent[ ai_world_recent_pos ], AI_WORLD_NAME, "%s", name );
	ai_world_recent_pos = ( ai_world_recent_pos + 1 ) % AI_WORLD_RECENT;
}

/*	Write the matching entries into `out`.  Returns the byte count, or 0
	when nothing matched - the caller then adds nothing to the prompt.
	The player's own words outrank the scene snapshot, and an entry that
	was already injected in the last three requests is skipped unless the
	player just named it again.											*/
static int ai_world_block( const char *question, const char *scene, char *out, size_t n )
{
	int score[ AI_WORLD_MAX ], idx[ AI_WORLD_MAX ];
	int m = 0, i, j, used = 0;
	size_t o = 0;

	if ( ai_world_n < 0 ) ai_world_load();
	if ( ai_world_n <= 0 || out == NULL || n < 32 ) return 0;
	out[ 0 ] = '\0';

	for ( i = 0; i < ai_world_n; i++ ) {
		int s = ai_world_score( &ai_world[ i ], question );
		if ( s > 0 ) s += 1000;
		else s = ai_world_score( &ai_world[ i ], scene );
		if ( s > 0 ) {
			score[ m ] = s;
			idx[ m ] = i;
			m++;
		}
	}
	if ( m == 0 ) return 0;

	for ( i = 1; i < m; i++ ) {
		int s = score[ i ], id = idx[ i ];
		for ( j = i - 1; j >= 0 && score[ j ] < s; j-- ) {
			score[ j + 1 ] = score[ j ];
			idx[ j + 1 ] = idx[ j ];
		}
		score[ j + 1 ] = s;
		idx[ j + 1 ] = id;
	}

	for ( i = 0; i < m && used < AI_WORLD_SHOW; i++ ) {
		AI_WORLD_ENTRY *e = &ai_world[ idx[ i ] ];
		size_t len = strlen( e->body );
		int r;

		if ( ai_world_recent_has( e->name ) && score[ i ] < 1000 ) continue;
		if ( o > 0 && o + len + strlen( e->name ) + 8 > AI_WORLD_BUDGET ) continue;
		r = snprintf( out + o, n - o, "- %s：%s\n", e->name, e->body );
		if ( r <= 0 || (size_t)r >= n - o ) break;
		o += (size_t)r;
		ai_world_recent_add( e->name );
		used++;
	}
	out[ o ] = '\0';
	return (int)o;
}

/*	Build the request body.  `question` is NULL for a scene remark.			*/
static void ai_build_body( char *out, size_t n, const char *scene,
				const char *persona, const char *tone, const char *example,
				const char *memory, const char *question )
{
	/*	Same reason as ai_start: these buffers do not belong on a worker
		thread's stack.  The worker runs one request at a time.  r283: the
		system block now has room for the whole persona + memory - at 8 KB
		a long memory pushed the rules at the end out of the prompt.	*/
	static char sys[ AI_MAX_TEXT ], sys_esc[ AI_MAX_TEXT + 1024 ];
	static char usr[ 4096 ], usr_esc[ 8192 ];
	size_t l;
	int world_hit = 0;
	int shape_v = -1;			/* r312: reply shape for a scene remark		*/
	int battle;					/* r322: an enemy is engaged right now		*/

	battle = ( scene != NULL && strstr( scene, "战斗：正在交战" ) != NULL );

#define AI_CAT( ... ) do { \
		l = strlen( sys ); \
		snprintf( sys + l, sizeof( sys ) - l, __VA_ARGS__ ); \
	} while ( 0 )

	sys[ 0 ] = '\0';
	AI_CAT( "你是《Elona》中主人的随从「%s」。\n", ai_npc );
	if ( persona[ 0 ] ) AI_CAT( "人设：\n%s\n", persona );
	if ( tone[ 0 ] )    AI_CAT( "关系语气：\n%s\n", tone );
	if ( example[ 0 ] )
		AI_CAT( "说话示例（照这些示例的语气和长度，不要照抄内容）：\n%s\n", example );
	if ( memory[ 0 ] )
		AI_CAT( "记忆（越靠后越新；前后矛盾时以靠后的为准）：\n%s\n", memory );
	{
		/*	r286: a few world-book entries, but only the ones this request
			touches.  On a miss nothing is added at all.					*/
		static char world[ 3072 ];
		world_hit = ai_world_block( question, scene, world, sizeof( world ) ) > 0;
		if ( world_hit )
			AI_CAT( "世界设定（下面这些是你本来就知道的事。主人问到时就按它回答，"
				"用你自己的口气说，别照读原文，别说不知道）：\n%s\n", world );
	}
	AI_CAT( "只输出中文台词，第一人称。"
		/*	r311: short by default.  "as long as the content needs" made every
			line three or four clauses long, which reads like a monologue.	*/
		"长度：大多数时候只说一两句，10~25 字就够；"
		"只有讲世界知识、劝他做某件事、或情绪上来时才用长一点，最多三四句。"
		"每句都要说完整，不要用省略号断在半截。" );
	/*	r322: in a fight she watches the fight.  The scene block says whether
		an enemy is engaged, and the whole remark is then tied to it - no
		weather, no lore, no idle chatter while something is swinging.		*/
	if ( battle ) {
		AI_CAT( "现在是战斗中。这一句只能和眼前的战斗有关："
			"提醒主人、报出敌人的方位或动作、喊一句短促的战术、或报自己的伤。"
			"一句就够，不要闲聊，不要讲世界知识，不要提天气、肚子和往事。" );
		shape_v = 4;
	}
	/*	r312: not every line is speech.  Days alone with the player make a
		constant narrator read like a visual novel; sometimes she just moves,
		or says one syllable.  The shape is picked here rather than left to
		the model, because "feel free" was ignored in practice.				*/
	if ( question == NULL && !battle ) {
		int shape = ( ai_seq + ai_hist_n * 3 + (int)( ai_now() / 9.0 ) ) % 10;

		shape_v = shape;
		if ( shape < 5 ) {
			AI_CAT( "这一句正常说台词。" );
		} else if ( shape == 9 ) {
			AI_CAT( "这一句只写极短的反应，六个字以内，"
				"例如「……」「嗯。」「啧。」，不要完整句子。" );
		} else {
			/*	r313: the non-speech shapes.  A follower alone with the player
				mostly watches, mutters and remembers; letting her narrate every
				change is what made her read like a visual novel.			*/
			switch ( ( shape + ai_seq ) % 6 ) {
			case 0:
				AI_CAT( "这一句只写一个括号动作，不要任何台词，"
					"例如「（摸了摸扁下去的肚子）」。短，只写一个。" );
				break;
			case 1:
				AI_CAT( "这一句只写你看到的东西，陈述一句，"
					"既不提醒也不发问，例如「那边的草在动。」" );
				break;
			case 2:
				AI_CAT( "这一句是自言自语，不是对主人说的，"
					"例如「这味道……熟。」可以说半句。" );
				break;
			case 3:
				AI_CAT( "这一句只是一个想不起来的小片段，只一句，"
					"例如「这条街，我好像走过。」不要解释。" );
				break;
			case 4:
				AI_CAT( "这一句只报一个事实，不带建议，"
					"例如「剑钝了。」「靴子进水了。」" );
				break;
			default:
				AI_CAT( "这一句只写两个连续的小动作，不要台词，"
					"例如「（停步，侧耳）」。两个动作要有先后。" );
				break;
			}
		}
	}
	AI_CAT( "不要解释、不要旁白。"
		/*	r307: the game is flat 2D pixels, so a written gesture is the only
			body language the player can see.  Kept sparse - one in every few
			lines - and limited to things that do not move the scene.		*/
		"可以带肢体动作，写在句首或句尾的全角括号里，"
		"例如「（凑近一点）主人，肚子饿了吧」；但不要每句都带，"
		"大约三句里一句就够了，其余句子直接说台词。"
		"括号里只放不改变场景的小动作：歪头、凑近、别开视线、拉衣角、蹭一下、攥紧手。"
		/*	r306: she cannot touch the world - the game moves nobody on her
			word.  A promise like 「我去给你做饭」 never happens on screen,
			so she only talks: remind, ask, worry.  Announcing an action
			read as a broken scene, which is exactly what the player saw.	*/
		"你没法替主人做事：不要承诺或预告行动，禁止「我去…」「我这就…」「我去帮你…」"
		"「等我…就…」「我给你…」这类句子；括号里也不要写走过去、掏出、递上、做饭。"
		"关心要落在提醒、询问、劝嘴上，"
		"比如「主人，肚子饿了吧，先吃点东西」，而不是「我去给你做饭」。"
		"这些只是你的说话准则，不必向主人解释。"
		/*	r308: the master-servant number is no longer in the scene, and the
			persona tiers are written against affection alone, so the model is
			told to read one scale instead of balancing two.				*/
		"关系好坏只看好感，以及你们一起经历过多少事：好感高了才亲近，"
		"别因为别的原因提早熟络。"
		/*	r308: one topic per line.  The weather sits in every scene block,
			so without this she says the same thing four times in a row.	*/
		"同一件事不要连着说两遍：上一句提了天气，这一句就换别的；没什么可说就短一点。" );
	/*	r308: she never heard her own last lines, so she kept repeating the
		weather.  Two remembered sentences plus an explicit "don't repeat"
		give the model negative feedback instead of a blank slate.			*/
	{
		static char said[ 2 ][ AI_MAX_LINE ];
		int sn = 0, i;

		pthread_mutex_lock( &ai_lock );
		for ( i = 0; i < 2 && i < ai_hist_n; i++ ) {
			memcpy( said[ i ], ai_hist[ i ], AI_MAX_LINE );
			sn++;
		}
		pthread_mutex_unlock( &ai_lock );
		if ( sn > 0 ) {
			AI_CAT( "你刚才说过的话（不要重复这些说法，也不要连着两次提同一件事）：\n" );
			for ( i = sn - 1; i >= 0; i-- )
				if ( said[ i ][ 0 ] ) AI_CAT( "- 「%s」\n", said[ i ] );
		}
	}
	/*	r311: only block an action when the last three lines already used two
		- a strict one-in-three rotation felt mechanical.  Otherwise the model
		decides, and the wording stays a suggestion rather than an order.
		r312: skipped when this turn was already told to be an action only.	*/
	if ( shape_v < 5 ) {
		int cnt = 0, i;

		pthread_mutex_lock( &ai_lock );
		for ( i = 0; i < 3 && i < ai_hist_n; i++ )
			if ( strstr( ai_hist[ i ], "（" ) != NULL || strchr( ai_hist[ i ], '(' ) != NULL )
				cnt++;
		pthread_mutex_unlock( &ai_lock );
		if ( cnt >= 2 )
			AI_CAT( "最近几句动作写得太多了。这一句直接说台词，不要加动作。\n" );
		else
			AI_CAT( "动作可有可无，不必每句都带。\n" );
	}
	/*	r303: expression tag.  Chooses the portrait the panel draws.
		r309: the exact shape is spelled out - the model kept writing a single
		bracket, which the old parser did not recognise.					*/
	if ( ai_emotion_n > 0 )
		AI_CAT( "情绪：台词最后另起一个标记，格式必须是双中括号加半角冒号 "
			"[[emo:词]]，例如 [[emo:平静]]；它不算台词，但必须写。"
			"词只能从下面选一个，拿不准就用「%s」。可用：%s\n",
			ai_emotion_default, ai_emotion_list );
	AI_CAT( "%s",
		question != NULL ? "主人正在跟你说话，必须用一句话回应，绝对不要输出 SKIP。"
			: "若此刻不适合说话，只输出 SKIP。" );
	if ( question != NULL ) {
		/*	Said twice on purpose - once in the system prompt, once in the
			user turn - because this model otherwise drifts back to whatever
			the persona was complaining about.								*/
		AI_CAT( "主人刚刚问你：「%s」。回答必须针对这句话。", question );
		if ( world_hit )		/*	r290: the data is right above it	*/
			AI_CAT( "这句话问的是世界设定里的东西，按上面给的资料回答，"
				"不要推说没去过、不知道。" );
	}
	if ( question != NULL ) {
		/*	Only the first scene line (where and when) goes with a question:
			the full snapshot carries the newest log row, and the model kept
			answering that instead of what the player actually typed.		*/
		/*	r365: one rule for both channels - a question turn now carries the
			same scene block a scene remark does, gear lines included.  The
			trailing sentence still keeps the answer on the question.		*/
		snprintf( usr, sizeof( usr ),
			"%s\n主人对你说：「%s」\n只回答这句话，一句话，不要描述四周、不要提日志。",
			scene, question );
	} else {
		snprintf( usr, sizeof( usr ), "%s", scene );
	}
	ai_json_escape( sys, sys_esc, sizeof( sys_esc ) );
	ai_json_escape( usr, usr_esc, sizeof( usr_esc ) );

	/*	r283: replay the last few question/answer pairs before this one, so
		"那刚才那个呢" has something to point at.  The copy is taken under
		the lock because the worker writes that history when an answer
		lands.																*/
	{
		static char q_esc[ 2048 ], a_esc[ 2048 ];
		static char hq[ AI_TURNS ][ AI_MAX_LINE ], ha[ AI_TURNS ][ AI_MAX_LINE ];
		size_t len;
		int hn, i, r;

		pthread_mutex_lock( &ai_lock );
		hn = ai_turn_n;
		for ( i = 0; i < hn; i++ ) {
			memcpy( hq[ i ], ai_turn_q[ i ], AI_MAX_LINE );
			memcpy( ha[ i ], ai_turn_a[ i ], AI_MAX_LINE );
		}
		pthread_mutex_unlock( &ai_lock );

		snprintf( out, n,
			"{\"model\":\"%s\",\"messages\":[{\"role\":\"system\",\"content\":\"%s\"}",
			ai_model, sys_esc );
		len = strlen( out );
		for ( i = 0; i < hn && len < n; i++ ) {
			ai_json_escape( hq[ i ], q_esc, sizeof( q_esc ) );
			ai_json_escape( ha[ i ], a_esc, sizeof( a_esc ) );
			r = snprintf( out + len, n - len,
				",{\"role\":\"user\",\"content\":\"%s\"},"
				"{\"role\":\"assistant\",\"content\":\"%s\"}",
				q_esc, a_esc );
			if ( r <= 0 || (size_t)r >= n - len ) break;
			len += (size_t)r;
		}
		if ( len < n ) {
			snprintf( out + len, n - len,
				",{\"role\":\"user\",\"content\":\"%s\"}],"
				"\"temperature\":1.0,\"max_tokens\":200}",
				usr_esc );
		}
	}
#undef AI_CAT
}

/*	Save / restore the question/answer pairs next to the persona, so a
	conversation survives a restart.  One file per follower:
	  data/npc/<name>.chat, two lines per turn ("Q", "A", tab-separated).
	The writer runs with ai_lock held (worker side).						*/
#define AI_CHAT_BUF	( AI_TURNS * ( AI_MAX_LINE * 2 + 8 ) )

static void ai_turn_save_locked( void )
{
	static char buf[ AI_CHAT_BUF ];
	char path[ 256 ];
	size_t o = 0;
	int i;
	FILE *f;

	if ( ai_npc[ 0 ] == '\0' ) return;
	snprintf( path, sizeof( path ), AI_DIR "npc/%s.chat",
		ai_base_name[ 0 ] ? ai_base_name : ai_npc );
	if ( ai_turn_n <= 0 ) {
		remove( path );
		return;
	}
	buf[ 0 ] = '\0';
	for ( i = 0; i < ai_turn_n && o < sizeof( buf ); i++ ) {
		int r = snprintf( buf + o, sizeof( buf ) - o, "Q\t%s\nA\t%s\n",
			ai_turn_q[ i ], ai_turn_a[ i ] );
		if ( r <= 0 || (size_t)r >= sizeof( buf ) - o ) break;
		o += (size_t)r;
	}
	f = fopen( path, "wb" );
	if ( f == NULL ) return;
	fwrite( buf, 1, o, f );
	fclose( f );
}

static void ai_turn_load( const char *npc )
{
	static char data[ AI_CHAT_BUF ];
	char path[ 256 ], line[ AI_MAX_LINE ], q[ AI_MAX_LINE ], base[ 64 ];
	const char *p;

	pthread_mutex_lock( &ai_lock );
	ai_turn_n = 0;
	pthread_mutex_unlock( &ai_lock );
	if ( npc == NULL || npc[ 0 ] == '\0' ) return;
	ai_resolve_base( npc, base, sizeof( base ) );
	snprintf( path, sizeof( path ), AI_DIR "npc/%s.chat", base );
	if ( ai_read_file( path, data, sizeof( data ) ) < 0 ) return;

	q[ 0 ] = '\0';
	p = data;
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
		if ( line[ 0 ] == 'Q' && line[ 1 ] == '\t' ) {
			snprintf( q, sizeof( q ), "%s", line + 2 );
		} else if ( line[ 0 ] == 'A' && line[ 1 ] == '\t' && q[ 0 ] ) {
			pthread_mutex_lock( &ai_lock );
			if ( ai_turn_n < AI_TURNS ) {
				snprintf( ai_turn_q[ ai_turn_n ], AI_MAX_LINE, "%s", q );
				snprintf( ai_turn_a[ ai_turn_n ], AI_MAX_LINE, "%s", line + 2 );
				ai_turn_n++;
			}
			pthread_mutex_unlock( &ai_lock );
			q[ 0 ] = '\0';
		}
	}
}

/*	Put a sentence on screen and into the three-line history.  Called with
	ai_lock held by the worker, and through ai_push_line() by the panel.	*/
static void ai_push_line_locked( const char *text )
{
	int hi;

	strncpy( ai_line, text, sizeof( ai_line ) - 1 );
	ai_line[ sizeof( ai_line ) - 1 ] = '\0';
	strncpy( ai_last_line, text, sizeof( ai_last_line ) - 1 );
	ai_last_line[ sizeof( ai_last_line ) - 1 ] = '\0';
	for ( hi = AI_HISTORY - 1; hi > 0; hi-- )
		memcpy( ai_hist[ hi ], ai_hist[ hi - 1 ], AI_MAX_LINE );
	strncpy( ai_hist[ 0 ], text, AI_MAX_LINE - 1 );
	ai_hist[ 0 ][ AI_MAX_LINE - 1 ] = '\0';
	if ( ai_hist_n < AI_HISTORY ) ai_hist_n++;
	ai_last_line_at = ai_now();
	ai_seq++;
}

static void ai_push_line( const char *text )
{
	pthread_mutex_lock( &ai_lock );
	ai_push_line_locked( text );
	pthread_mutex_unlock( &ai_lock );
	ai_log_line( "say", text );
}

/*	Remember one question and the answer it got.  Called with ai_lock held,
	from the worker.  Older pairs fall off the front.					*/
static void ai_turn_add( const char *q, const char *a )
{
	int i;

	if ( q == NULL || q[ 0 ] == '\0' || a == NULL || a[ 0 ] == '\0' ) return;
	if ( ai_turn_n < AI_TURNS ) {
		snprintf( ai_turn_q[ ai_turn_n ], AI_MAX_LINE, "%s", q );
		snprintf( ai_turn_a[ ai_turn_n ], AI_MAX_LINE, "%s", a );
		ai_turn_n++;
		ai_turn_save_locked();
		return;
	}
	for ( i = 1; i < AI_TURNS; i++ ) {
		memcpy( ai_turn_q[ i - 1 ], ai_turn_q[ i ], AI_MAX_LINE );
		memcpy( ai_turn_a[ i - 1 ], ai_turn_a[ i ], AI_MAX_LINE );
	}
	snprintf( ai_turn_q[ AI_TURNS - 1 ], AI_MAX_LINE, "%s", q );
	snprintf( ai_turn_a[ AI_TURNS - 1 ], AI_MAX_LINE, "%s", a );
	ai_turn_save_locked();
}

/*	Count the "- " entries the memory section currently holds.			*/
static int ai_memory_lines( void )
{
	const char *p = ai_memory;
	int n = 0;

	while ( *p ) {
		if ( *p == '-' && ( p == ai_memory || p[ -1 ] == '\n' ) ) n++;
		p++;
	}
	return n;
}

/*	Keep only the "- " lines the compactor produced, at most six of them.
	Models like to wrap a list in a sentence; that sentence must not end up
	in the follower's file, where every later read would see it as memory. */
static void ai_memory_clean( const char *in, char *out, size_t n )
{
	const char *p = in;
	char line[ 512 ];
	size_t o = 0;
	int keep = 0;

	out[ 0 ] = '\0';
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
		size_t l;
		ai_trim( line );
		if ( line[ 0 ] != '-' ) continue;
		if ( keep >= 6 ) break;
		l = strlen( line );
		if ( o + l + 2 >= n ) break;
		memcpy( out + o, line, l );
		o += l;
		out[ o++ ] = '\n';
		keep++;
	}
	if ( o > 0 ) out[ o - 1 ] = '\0';		/* drop the trailing newline	*/
}

/*	Rewrite the followers's markdown so that the "## 记忆" section holds
	`list` instead of what it had.  Every other line is copied verbatim, so
	hand-written sections and comments survive the compaction.			*/
static int ai_replace_memory( const char *list )
{
	static char data[ 8192 ], out[ 8192 ];
	char path[ 256 ], tmp[ 256 ], line[ 512 ];
	const char *p;
	FILE *f;
	size_t o = 0;
	int in_mem = 0, seen = 0, r;

	if ( list == NULL || list[ 0 ] == '\0' || ai_npc[ 0 ] == '\0' ) return 0;
	snprintf( path, sizeof( path ), AI_DIR "npc/%s.md", ai_npc );
	if ( ai_read_file( path, data, sizeof( data ) ) < 0 ) {
		/*	Seed from the shared default on first write, so a follower can
			gain a memory before the player has copied anything.		*/
		char base[ 256 ];
		snprintf( base, sizeof( base ), AI_DIR "npc/default.md" );
		if ( ai_read_file( base, data, sizeof( data ) ) < 0 ) return 0;
	}

	p = data;
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
		if ( line[ 0 ] == '#' && line[ 1 ] == '#' ) {
			in_mem = strstr( line, "记忆" ) != NULL;
			if ( in_mem ) {
				r = snprintf( out + o, sizeof( out ) - o, "%s\n%s\n", line, list );
				if ( r > 0 && (size_t)r < sizeof( out ) - o ) o += (size_t)r;
				seen = 1;
				continue;
			}
		}
		if ( in_mem ) continue;			/* old entries are dropped		*/
		r = snprintf( out + o, sizeof( out ) - o, "%s\n", line );
		if ( r > 0 && (size_t)r < sizeof( out ) - o ) o += (size_t)r;
	}
	if ( !seen ) {
		r = snprintf( out + o, sizeof( out ) - o, "\n## 记忆\n%s\n", list );
		if ( r > 0 && (size_t)r < sizeof( out ) - o ) o += (size_t)r;
	}
	snprintf( tmp, sizeof( tmp ), AI_DIR "npc/%s.md.new", ai_npc );
	f = fopen( tmp, "wb" );
	if ( f == NULL ) return 0;
	fwrite( out, 1, strlen( out ), f );
	fclose( f );
	remove( path );
	rename( tmp, path );
	ai_persona_name[ 0 ] = '\0';		/* force a reload next context	*/
	return 1;
}

static void ai_request( char *body, int mode, const char *question )
{
	CURL *h;
	struct curl_slist *hdrs;
	struct AI_BUF buf;
	char auth[ 320 ];
	char result[ AI_MAX_LINE ];
	long code = 0;
	CURLcode rc = CURLE_OK;
	int attempt;

	buf.p = (char*)calloc( 1, AI_MAX_TEXT );
	result[ 0 ] = '\0';
	if ( buf.p == NULL ) { ai_busy = 0; return; }

	snprintf( auth, sizeof( auth ), "Authorization: Bearer %s", ai_key );
	/*	Two attempts.  The console's TLS handshake to a fixed host fails now
		and then (SSL connect error) and a retry almost always succeeds; the
		request is a few hundred tokens, so the second try is free.		*/
	for ( attempt = 0; attempt < 2; attempt++ ) {
		buf.n = 0;
		buf.p[ 0 ] = '\0';
		code = 0;
		h = curl_easy_init();
		if ( h == NULL ) break;
		hdrs = NULL;
		hdrs = curl_slist_append( hdrs, "Content-Type: application/json" );
		hdrs = curl_slist_append( hdrs, auth );
		curl_easy_setopt( h, CURLOPT_URL, "https://api.deepseek.com/chat/completions" );
		curl_easy_setopt( h, CURLOPT_HTTPHEADER, hdrs );
		curl_easy_setopt( h, CURLOPT_POSTFIELDS, body );
		curl_easy_setopt( h, CURLOPT_WRITEFUNCTION, ai_write_cb );
		curl_easy_setopt( h, CURLOPT_WRITEDATA, &buf );
		curl_easy_setopt( h, CURLOPT_TIMEOUT, 15L );
		curl_easy_setopt( h, CURLOPT_CONNECTTIMEOUT, 8L );
		curl_easy_setopt( h, CURLOPT_NOSIGNAL, 1L );
		/*	HTTP/1.1 only: ALPN on this mbedtls build negotiates h2 and then
			stalls, which shows up as that same SSL connect error.			*/
		curl_easy_setopt( h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1 );
		/*	Switch homebrew has no CA store shipped with it; the request is to
			a single fixed host and the payload is disposable, so the
			certificate chain is not verified.							*/
		curl_easy_setopt( h, CURLOPT_SSL_VERIFYPEER, 0L );
		curl_easy_setopt( h, CURLOPT_SSL_VERIFYHOST, 0L );

		rc = curl_easy_perform( h );
		curl_easy_getinfo( h, CURLINFO_RESPONSE_CODE, &code );
		curl_slist_free_all( hdrs );
		curl_easy_cleanup( h );
		if ( rc == CURLE_OK ) break;
		printf( "aichat: attempt %d failed rc=%d, retrying\n", attempt + 1, (int)rc );
		fflush( stdout );
	}

	pthread_mutex_lock( &ai_lock );
	if ( mode == 1 ) {
		/*	Memory compaction: the answer is a list, not a line, and it
			never goes on screen.  A failed compaction is harmless - the
			old memory stays and the next attempt waits for the timer.	*/
		static char cleaned[ AI_MAX_MEMORY ];	/* 4 KB, not on the stack */

		if ( rc == CURLE_OK && code == 200 &&
				ai_parse_content( buf.p, result, sizeof( result ) ) ) {
			ai_memory_clean( result, cleaned, sizeof( cleaned ) );
			if ( cleaned[ 0 ] && ai_replace_memory( cleaned ) ) {
				/*	The panel's copy follows at once; without this the old
					count kept the timer firing every ten minutes.		*/
				snprintf( ai_memory, sizeof( ai_memory ), "%s", cleaned );
				ai_log_line( "mem", cleaned );
			} else {
				ai_log_line( "mem-fail", result );
			}
			ai_last_compress = ai_now();
		}
		ai_state = AI_STATE_READY;
		ai_status_set( "AI 就绪" );
	} else if ( rc == CURLE_OK && code == 200 &&
			ai_parse_content( buf.p, result, sizeof( result ) ) ) {
		if ( strcmp( result, "SKIP" ) == 0 || strcmp( result, "skip" ) == 0 ) {
			ai_state = AI_STATE_READY;
			ai_status_set( "AI 就绪" );
		} else {
			/*	r303: the expression tag is metadata, not part of the line.
				r311: the raw answer is logged first - without it there is no
				way to tell whether a missing expression came from the parser
				or from a name outside the whitelist.						*/
			ai_log_line( "raw", result );
			ai_take_emotion( result );
			ai_take_cg( result );
			/*	r312: an answer that was only a tag (or only brackets) leaves
				nothing to say.  Push it as an action line instead of an empty
				row: the panel paints a fully bracketed line green.			*/
			if ( result[ 0 ] != '\0' ) {
				ai_push_line_locked( result );
				/*	r283: keep the pair so the next question can build on it.
					Scene remarks have no question and are not stored.		*/
				ai_turn_add( question, result );
				ai_log_line( "say", result );
			}
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
		/*	Temporary diagnostic: the last rejected request and the server's
			reply, so a 400 can be explained without guessing.				*/
		{
			FILE *df = fopen( AI_DIR "ai_debug.txt", "wb" );
			if ( df != NULL ) {
				fprintf( df, "rc=%d http=%ld\n--- body ---\n%.1400s\n--- reply ---\n%.600s\n",
					(int)rc, code, body, buf.p );
				fclose( df );
			}
		}
	}
	pthread_mutex_unlock( &ai_lock );

	printf( "aichat: rc=%d http=%ld answer=[%s]\n", (int)rc, code, result );
	fflush( stdout );

	free( buf.p );
	ai_busy = 0;
}

struct AI_JOB { char body[ AI_MAX_TEXT ]; char question[ AI_MAX_LINE ]; int mode; };

static void *ai_thread( void *arg )
{
	struct AI_JOB *job = (struct AI_JOB*)arg;
	ai_request( job->body, job->mode, job->question );
	free( job );
	return NULL;
}

/*	r315: the console's default pthread stack is far too small for a TLS
	handshake - libcurl plus mbedtls wants well over a hundred kilobytes and
	a miss shows up as a hard crash in the middle of play, not as a failed
	request.  512 KB is affordable because ai_busy keeps exactly one request
	thread alive at any time.											*/
#define AI_THREAD_STACK		( 512 * 1024 )

static int ai_spawn( struct AI_JOB *job )
{
	pthread_attr_t at;
	pthread_t th;
	int rc;

	if ( pthread_attr_init( &at ) != 0 ) return -1;
	if ( pthread_attr_setstacksize( &at, AI_THREAD_STACK ) != 0 ) {
		pthread_attr_destroy( &at );
		return -1;
	}
	rc = pthread_create( &th, &at, ai_thread, job );
	pthread_attr_destroy( &at );
	if ( rc == 0 ) pthread_detach( th );
	return rc;
}

static void ai_start( const char *question, const char *event )
{
	struct AI_JOB *job;
	/*	These used to be plain locals, which put ~11 KB on the render thread's
		stack every time the player asked something.  On the Switch that stack
		is small: the question arrived at the model as "???jo??&oAl??P?" - the
		text had been overwritten by the call below it.  Static is safe here
		because every caller is the same thread (the panel's tick).			*/
	static char scene[ AI_MAX_TEXT ], persona[ AI_MAX_PERSONA ], memory[ AI_MAX_MEMORY ];
	static char tone[ 1024 ], example[ 2048 ];

	if ( ai_busy || !ai_enabled || ai_key[ 0 ] == '\0' ) return;
	/*	No follower in the party means nobody to speak: the panel only feeds
		the context while one is present, so this is the final guard.		*/
	if ( ai_npc[ 0 ] == '\0' ) return;

	pthread_mutex_lock( &ai_lock );
	if ( event != NULL && event[ 0 ] ) {
		snprintf( scene, sizeof( scene ), "%s\n事件：%s", ai_scene, event );
	} else {
		snprintf( scene, sizeof( scene ), "%s", ai_scene );
	}
	snprintf( persona, sizeof( persona ), "%s", ai_persona );
	snprintf( memory, sizeof( memory ), "%s", ai_memory );
	snprintf( tone, sizeof( tone ), "%s", ai_tone );
	snprintf( example, sizeof( example ), "%s", ai_example );
	ai_busy = 1;
	ai_busy_at = ai_now();
	ai_state = AI_STATE_BUSY;
	ai_status_set( "AI 思考中" );
	ai_last_request = ai_now();
	pthread_mutex_unlock( &ai_lock );

	job = (struct AI_JOB*)malloc( sizeof( struct AI_JOB ) );
	if ( job == NULL ) { ai_busy = 0; return; }
	job->mode = 0;
	snprintf( job->question, sizeof( job->question ), "%s",
		question != NULL ? question : "" );
	ai_build_body( job->body, sizeof( job->body ), scene, persona, tone, example, memory, question );
	if ( ai_spawn( job ) != 0 ) {
		free( job );
		ai_busy = 0;
		pthread_mutex_lock( &ai_lock );
		ai_state = AI_STATE_ERROR;
		ai_status_set( "AI 线程失败" );
		pthread_mutex_unlock( &ai_lock );
		return;
	}
}

/*	Compact the memory list once it has grown past a couple of dozen facts.
	The answer is written back into the markdown (mode 1), never shown.	*/
#define AI_MEMORY_MAX_LINES	18

static void ai_start_compress( void )
{
	struct AI_JOB *job;
	static char sys[ 1024 ], sys_esc[ 4096 ];
	static char usr[ AI_MAX_MEMORY + 256 ], usr_esc[ ( AI_MAX_MEMORY + 256 ) * 3 ];
	static char list[ AI_MAX_MEMORY ];

	if ( ai_busy || !ai_enabled || ai_key[ 0 ] == '\0' ) return;
	/*	No follower in the party means nobody to speak: the panel only feeds
		the context while one is present, so this is the final guard.		*/
	if ( ai_npc[ 0 ] == '\0' ) return;
	pthread_mutex_lock( &ai_lock );
	if ( ai_memory[ 0 ] == '\0' ) { pthread_mutex_unlock( &ai_lock ); return; }
	snprintf( list, sizeof( list ), "%s", ai_memory );
	ai_busy = 1;
	ai_busy_at = ai_now();
	ai_state = AI_STATE_BUSY;
	ai_status_set( "AI 整理记忆" );
	ai_last_compress = ai_now();
	pthread_mutex_unlock( &ai_lock );

	snprintf( sys, sizeof( sys ),
		"你是记忆整理器。把随从的记忆合并成不超过 6 条，每条一行，以「- 」开头。"
		"保留具体事实，去掉重复，前后矛盾时保留靠后的那条。只输出列表，不要解释。" );
	snprintf( usr, sizeof( usr ), "现有记忆：\n%s", list );
	ai_json_escape( sys, sys_esc, sizeof( sys_esc ) );
	ai_json_escape( usr, usr_esc, sizeof( usr_esc ) );

	job = (struct AI_JOB*)malloc( sizeof( struct AI_JOB ) );
	if ( job == NULL ) { ai_busy = 0; return; }
	job->mode = 1;
	job->question[ 0 ] = '\0';			/* compaction is not a conversation */
	snprintf( job->body, sizeof( job->body ),
		"{\"model\":\"%s\",\"messages\":["
		"{\"role\":\"system\",\"content\":\"%s\"},"
		"{\"role\":\"user\",\"content\":\"%s\"}],"
		"\"temperature\":0.3,\"max_tokens\":400}",
		ai_model, sys_esc, usr_esc );
	if ( ai_spawn( job ) != 0 ) {
		free( job );
		ai_busy = 0;
		return;
	}
}

void ai_chat_init( void )
{
	static int inited = 0;
	static double retry_at = 0;
	double now;

	if ( inited && ai_enabled ) return;
	now = ai_now();
	if ( inited && now < retry_at ) return;		/* one fopen every 10 seconds */
	if ( !inited ) {
		curl_global_init( CURL_GLOBAL_DEFAULT );
		inited = 1;
	}
	/*	The config file usually only appears after the first launch, so a
		missing one is retried instead of locking the module off for the
		whole run.  Until it shows up nothing is spent.						*/
	ai_load_config();
	retry_at = now + 10.0;
}

/*	r302: the party no longer has a follower.  Everything that belonged to
	her is dropped, so a later question cannot be answered - or paid for -
	by a character who is not there any more.							*/
void ai_chat_drop_context( void )
{
	int i;

	if ( !ai_enabled ) return;
	pthread_mutex_lock( &ai_lock );
	if ( ai_npc[ 0 ] == '\0' ) {
		pthread_mutex_unlock( &ai_lock );
		return;
	}
	ai_npc[ 0 ] = '\0';
	ai_base_name[ 0 ] = '\0';
	ai_persona_name[ 0 ] = '\0';
	ai_persona[ 0 ] = '\0';
	ai_tone[ 0 ] = '\0';
	ai_example[ 0 ] = '\0';
	ai_memory[ 0 ] = '\0';
	ai_emotion[ 0 ] = '\0';
	ai_emotion_default[ 0 ] = '\0';
	ai_emotion_calm[ 0 ] = '\0';
	ai_emotion_list[ 0 ] = '\0';
	ai_emotion_n = 0;
	ai_scene[ 0 ] = '\0';
	ai_last_line[ 0 ] = '\0';
	ai_line[ 0 ] = '\0';
	ai_hist_n = 0;
	for ( i = 0; i < AI_HISTORY; i++ ) ai_hist[ i ][ 0 ] = '\0';
	ai_turn_n = 0;
	ai_event_prio = -1;
	ai_event[ 0 ] = '\0';
	ai_has_pending = 0;
	ai_pending[ 0 ] = '\0';
	ai_seq++;
	pthread_mutex_unlock( &ai_lock );
	printf( "aichat: follower left, context dropped\n" );
	fflush( stdout );
}

void ai_chat_set_context( const char *npc_name, const char *scene )
{
	int changed = 0;

	if ( !ai_enabled || npc_name == NULL || scene == NULL ) return;
	pthread_mutex_lock( &ai_lock );
	if ( strcmp( npc_name, ai_npc ) != 0 ) {
		int i;
		strncpy( ai_npc, npc_name, sizeof( ai_npc ) - 1 );
		ai_npc[ sizeof( ai_npc ) - 1 ] = '\0';
		ai_last_line[ 0 ] = '\0';
		/*	A different speaker means a different conversation: keeping the
			old pairs would put somebody else's words in her mouth.  The
			panel's three-line box is cleared too, for the same reason.	*/
		ai_turn_n = 0;
		ai_line[ 0 ] = '\0';
		ai_hist_n = 0;
		for ( i = 0; i < AI_HISTORY; i++ ) ai_hist[ i ][ 0 ] = '\0';
		ai_seq++;
		changed = 1;
	}
	if ( strcmp( scene, ai_scene ) != 0 ) {
		strncpy( ai_scene, scene, sizeof( ai_scene ) - 1 );
		ai_scene[ sizeof( ai_scene ) - 1 ] = '\0';
	}
	pthread_mutex_unlock( &ai_lock );
	ai_load_persona( npc_name );
	/*	Only on a change - this runs every frame, the file read does not. */
	if ( changed ) ai_turn_load( npc_name );
}

void ai_chat_notify_event( const char *event, int priority )
{
	double now = ai_now();
	int i, slot = 0;

	if ( !ai_enabled || event == NULL || event[ 0 ] == '\0' ) return;
	pthread_mutex_lock( &ai_lock );
	/*	Same sentence inside 8 seconds is the same event seen twice: ignore.
		Kept short on request - the follower should feel present, and one
		request costs a fraction of a fen.									*/
	for ( i = 0; i < AI_EVENT_KINDS; i++ ) {
		if ( strcmp( ai_event_kind[ i ], event ) == 0 && now - ai_event_seen[ i ] < 8 ) {
			pthread_mutex_unlock( &ai_lock );
			return;
		}
	}
	for ( i = 1; i < AI_EVENT_KINDS; i++ ) if ( ai_event_seen[ i ] < ai_event_seen[ slot ] ) slot = i;
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

static int ai_ends_with( const char *s, const char *suf )
{
	size_t a = strlen( s ), b = strlen( suf );
	return a >= b && strcmp( s + a - b, suf ) == 0;
}

int ai_chat_remember( const char *instruction );	/* defined below		*/

int ai_chat_ask( const char *question )
{
	if ( question == NULL || question[ 0 ] == '\0' ) return 0;
	/*	The keyboard works on any layer, so the module may never have been
		initialised by the panel yet - do it here as well.					*/
	ai_chat_init();
	if ( !ai_enabled || ai_key[ 0 ] == '\0' ) {
		printf( "aichat: ask ignored, %s\n", ai_status );
		fflush( stdout );
		return 0;
	}
	/*	Typing with nobody in the party must not spend anything.			*/
	if ( ai_npc[ 0 ] == '\0' ) {
		printf( "aichat: ask ignored, no follower\n" );
		fflush( stdout );
		return 0;
	}

	/*	"（设定）" at the end of a line turns it into a memory entry instead
		of a question: the text is appended to the follower's markdown and a
		short acknowledgement is shown.  No request is made, so it is free
		and works even while another line is still on its way.			*/
	{
		const char *suf = ai_ends_with( question, "（设定）" ) ? "（设定）"
			: ai_ends_with( question, "(设定)" ) ? "(设定)" : NULL;
		if ( suf != NULL ) {
			char body[ AI_MAX_LINE ];
			int cut = (int)( strlen( question ) - strlen( suf ) );
			snprintf( body, sizeof( body ), "%.*s", cut, question );
			ai_trim( body );
			if ( body[ 0 ] != '\0' && ai_chat_remember( body ) ) {
				ai_push_line( "记住了。" );
				ai_log_line( "set", body );
				printf( "aichat: memory set [%s]\n", body );
				fflush( stdout );
				return 1;
			}
		}
	}
	/*	A question outranks whatever the game just did: drop the queued
		event so the reply is about what the player typed.					*/
	pthread_mutex_lock( &ai_lock );
	ai_event_prio = -1;
	ai_event[ 0 ] = '\0';
	pthread_mutex_unlock( &ai_lock );

	/*	A question interrupts the idle timer; the answer is a real reply.
		A request already in flight used to swallow the question outright,
		which is why some presses of ZR+ZL produced no answer at all - the
		text is now parked and sent as soon as the worker is free.			*/
	ai_log_line( "ask", question );
	if ( ai_busy ) {
		pthread_mutex_lock( &ai_lock );
		strncpy( ai_pending, question, sizeof( ai_pending ) - 1 );
		ai_pending[ sizeof( ai_pending ) - 1 ] = '\0';
		ai_has_pending = 1;
		pthread_mutex_unlock( &ai_lock );
		printf( "aichat: question queued\n" );
		fflush( stdout );
		return 1;
	}
	ai_start( question, NULL );
	return 1;
}

int ai_chat_seq( void )
{
	return ai_seq;
}

int ai_chat_remember( const char *instruction )
{
	static char data[ 8192 ], list[ AI_MAX_MEMORY ];
	char path[ 256 ], line[ 512 ];
	const char *p;
	int in_mem = 0;

	if ( instruction == NULL ) return 0;
	while ( *instruction == ' ' || *instruction == '\t' ) instruction++;
	if ( instruction[ 0 ] == '\0' || ai_npc[ 0 ] == '\0' ) return 0;

	/*	Read the current list rather than blind-appending at the end of the
		file: the memory section is not always last, and a line dropped into
		the examples section would poison the style anchor.				*/
	snprintf( path, sizeof( path ), AI_DIR "npc/%s.md", ai_npc );
	if ( ai_read_file( path, data, sizeof( data ) ) < 0 ) {
		char base[ 256 ];
		snprintf( base, sizeof( base ), AI_DIR "npc/default.md" );
		if ( ai_read_file( base, data, sizeof( data ) ) < 0 ) return 0;
	}
	list[ 0 ] = '\0';
	p = data;
	while ( ai_next_line( &p, line, sizeof( line ) ) ) {
		if ( strncmp( line, "##", 2 ) == 0 ) {
			in_mem = strstr( line, "记忆" ) != NULL;
			continue;
		}
		if ( !in_mem || line[ 0 ] == '\0' ) continue;
		if ( strlen( list ) + strlen( line ) + 2 < sizeof( list ) ) {
			strcat( list, line );
			strcat( list, "\n" );
		}
	}
	/*	Newest last: the prompt tells the model that later entries win.	*/
	if ( strlen( list ) + strlen( instruction ) + 4 < sizeof( list ) ) {
		strcat( list, "- " );
		strcat( list, instruction );
		strcat( list, "\n" );
	}
	return ai_replace_memory( list );
}

/*	Debug chat channel: the keyboard is unavailable in the emulator, so a text
	file dropped on the SD card stands in for typed input.  Removed once read. */
static void ai_poll_input( void )
{
	char data[ AI_MAX_LINE ];
	char line[ AI_MAX_LINE ];
	const char *p;

	if ( ai_read_file( AI_INPUT, data, sizeof( data ) ) < 0 ) return;
	p = data;
	if ( ai_next_line( &p, line, sizeof( line ) ) ) {
		ai_trim( line );
		if ( strncmp( line, "/设定", strlen( "/设定" ) ) == 0 ) {
			ai_chat_remember( line + strlen( "/设定" ) );
		} else if ( line[ 0 ] ) {
			ai_chat_ask( line );
		}
	}
	remove( AI_INPUT );
}

void ai_chat_tick( void )
{
	static double next_poll = 0;
	double now;
	char pending[ AI_MAX_LINE ];

	/*	Must start empty: the block below only fills it when a queued question
		exists, and the check after it reads the first byte.  Uninitialised,
		this buffer sent whatever the stack happened to hold to the API once a
		second - which is what produced the 400s and the "I don't understand". */
	pending[ 0 ] = '\0';

	if ( !ai_enabled ) return;
	now = ai_now();

	/*	The input file is read even while a request is in flight: the text is
		parked by ai_chat_ask() and sent the moment the worker is free.  Not
		polling here made a typed line wait out the whole timeout.			*/
	if ( now >= next_poll ) {
		next_poll = now + 1.0;		/* the input file is checked once a second */
		ai_poll_input();
	}

	/*	Watchdog: a worker that died before clearing the flag would leave the
		follower mute for the rest of the run.								*/
	if ( ai_busy ) {
		if ( ai_busy_at > 0 && now - ai_busy_at > 45.0 ) {
			ai_busy = 0;
			ai_state = AI_STATE_ERROR;
			ai_status_set( "AI 超时" );
			printf( "aichat: worker watchdog fired\n" );
			fflush( stdout );
		}
		return;
	}

	/*	A question the player typed while a request was running goes first:
		it is the one thing the player is waiting for.						*/
	pthread_mutex_lock( &ai_lock );
	if ( ai_has_pending ) {
		snprintf( pending, sizeof( pending ), "%s", ai_pending );
		ai_pending[ 0 ] = '\0';
		ai_has_pending = 0;
	}
	pthread_mutex_unlock( &ai_lock );
	if ( pending[ 0 ] ) {
		ai_start( pending, NULL );
		return;
	}

	/*	Events no longer expire: whatever happened while a request was in
		flight is still worth a line.  r333: the gap is raised to 12 s and a
		fresh line is never followed within 5 s, because a battle plus a
		floor change plus a level-up used to arrive as three sentences in a
		row and read as nagging.										*/
	pthread_mutex_lock( &ai_lock );
	if ( ai_event_prio >= 0 && now - ai_last_request >= 12.0 &&
			now - ai_last_line_at >= 5.0 ) {
		char event[ 512 ];
		snprintf( event, sizeof( event ), "%s", ai_event );
		ai_event_prio = -1;
		ai_event[ 0 ] = '\0';
		pthread_mutex_unlock( &ai_lock );
		{
			static int dbg = 0;
			if ( dbg < 50 ) {
				dbg++;
				printf( "aichat: send event [%s]\n", event );
				fflush( stdout );
			}
		}
		ai_start( NULL, event );
		return;
	}
	pthread_mutex_unlock( &ai_lock );

	/*	Housekeeping: a memory list longer than this stops being an asset.
		Runs only when nothing else is pending, and at most every ten
		minutes, so it never competes with a line the player is waiting for. */
	if ( ai_memory_lines() >= AI_MEMORY_MAX_LINES &&
			now - ai_last_compress > 600.0 ) {
		ai_start_compress();
		return;
	}

	/*	r309: even the idle line waits a while after the previous one, so a
		queued event that just fired is not immediately followed by scenery
		commentary.  The interval stays the user's setting.				*/
	if ( !ai_idle && now - ai_last_request >= (double)ai_interval &&
			now - ai_last_line_at >= 15.0 && ai_scene[ 0 ] != '\0' ) {
		ai_start( NULL, "无事发生，安静地待着" );
	}
}

void ai_chat_set_idle( int idle )
{
	/*	An unattended console must not keep paying for scenery remarks.  The
		panel decides this - it is the only place that sees the game move.	*/
	ai_idle = idle ? 1 : 0;
}

const char *ai_chat_text( void )
{
	return ai_line;
}

const char *ai_chat_history( int index )
{
	if ( index < 0 || index >= AI_HISTORY ) return "";
	return ai_hist[ index ];
}

int ai_chat_history_count( void )
{
	return ai_hist_n;
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
