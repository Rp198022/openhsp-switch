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

#define AI_MAX_TEXT		16384	/* request body / answer scratch		*/
#define AI_MAX_LINE		512		/* one spoken line					*/
#define AI_MAX_PERSONA	2048
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

	snprintf( path, sizeof( path ), AI_DIR "npc/%s.md", npc );
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
			else section = 0;
			continue;
		}
		if ( section == 0 || line[ 0 ] == '\0' ) continue;
		pl = strlen( ai_persona ) + strlen( ai_tone ) + strlen( ai_example ) + 3;
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
	printf( "aichat: persona %s (%d bytes, memory %d, tone %d, example %d)\n", npc,
		(int)strlen( ai_persona ), (int)strlen( ai_memory ),
		(int)strlen( ai_tone ), (int)strlen( ai_example ) );
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

/*	Build the request body.  `question` is NULL for a scene remark.			*/
static void ai_build_body( char *out, size_t n, const char *scene,
				const char *persona, const char *tone, const char *example,
				const char *memory, const char *question )
{
	/*	Same reason as ai_start: these buffers do not belong on a worker
		thread's stack.  The worker runs one request at a time.			*/
	static char sys[ 8192 ], sys_esc[ 24576 ];
	static char usr[ 4096 ], usr_esc[ 12288 ];
	size_t l;

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
	AI_CAT( "只输出中文台词，50字以内，可以用两三个短句，第一人称；"
		"不要解释、不要动作描写、不要用括号。"
		"好感管亲近（越高越主动关心、越会示弱），主从管服从（越高越听话，越低越会顶嘴）；"
		"两项独立判断，不要因为一项高就推断另一项。" );
	AI_CAT( "%s",
		question != NULL ? "主人正在跟你说话，必须用一句话回应，绝对不要输出 SKIP。"
			: "若此刻不适合说话，只输出 SKIP。" );
	if ( question != NULL ) {
		/*	Said twice on purpose - once in the system prompt, once in the
			user turn - because this model otherwise drifts back to whatever
			the persona was complaining about.								*/
		AI_CAT( "主人刚刚问你：「%s」。回答必须针对这句话。", question );
	}
	if ( question != NULL ) {
		/*	Only the first scene line (where and when) goes with a question:
			the full snapshot carries the newest log row, and the model kept
			answering that instead of what the player actually typed.		*/
		char where[ 160 ];
		size_t wl = 0;
		while ( scene[ wl ] && scene[ wl ] != '\n' && wl < sizeof( where ) - 1 ) wl++;
		memcpy( where, scene, wl );
		where[ wl ] = '\0';
		snprintf( usr, sizeof( usr ),
			"%s\n主人对你说：「%s」\n只回答这句话，一句话，不要描述四周、不要提日志。",
			where, question );
	} else {
		snprintf( usr, sizeof( usr ), "%s", scene );
	}
	ai_json_escape( sys, sys_esc, sizeof( sys_esc ) );
	ai_json_escape( usr, usr_esc, sizeof( usr_esc ) );

	snprintf( out, n,
		"{\"model\":\"%s\",\"messages\":["
		"{\"role\":\"system\",\"content\":\"%s\"},"
		"{\"role\":\"user\",\"content\":\"%s\"}],"
		"\"temperature\":1.0,\"max_tokens\":200}",
		ai_model, sys_esc, usr_esc );
#undef AI_CAT
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

static void ai_request( char *body, int mode )
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
		if ( rc == CURLE_OK && code == 200 &&
				ai_parse_content( buf.p, result, sizeof( result ) ) ) {
			ai_log_line( ai_replace_memory( result ) ? "mem" : "mem-fail", result );
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
			ai_push_line_locked( result );
			ai_state = AI_STATE_READY;
			ai_status_set( "AI 就绪" );
			ai_log_line( "say", result );
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

struct AI_JOB { char body[ AI_MAX_TEXT ]; int mode; };

static void *ai_thread( void *arg )
{
	struct AI_JOB *job = (struct AI_JOB*)arg;
	ai_request( job->body, job->mode );
	free( job );
	return NULL;
}

static void ai_start( const char *question, const char *event )
{
	struct AI_JOB *job;
	pthread_t th;
	/*	These used to be plain locals, which put ~11 KB on the render thread's
		stack every time the player asked something.  On the Switch that stack
		is small: the question arrived at the model as "???jo??&oAl??P?" - the
		text had been overwritten by the call below it.  Static is safe here
		because every caller is the same thread (the panel's tick).			*/
	static char scene[ AI_MAX_TEXT ], persona[ AI_MAX_PERSONA ], memory[ AI_MAX_MEMORY ];
	static char tone[ 1024 ], example[ 2048 ];

	if ( ai_busy || !ai_enabled || ai_key[ 0 ] == '\0' ) return;

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
	ai_build_body( job->body, sizeof( job->body ), scene, persona, tone, example, memory, question );
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

/*	Compact the memory list once it has grown past a couple of dozen facts.
	The answer is written back into the markdown (mode 1), never shown.	*/
#define AI_MEMORY_MAX_LINES	18

static void ai_start_compress( void )
{
	struct AI_JOB *job;
	pthread_t th;
	static char sys[ 1024 ], sys_esc[ 4096 ];
	static char usr[ AI_MAX_MEMORY + 256 ], usr_esc[ ( AI_MAX_MEMORY + 256 ) * 3 ];
	static char list[ AI_MAX_MEMORY ];

	if ( ai_busy || !ai_enabled || ai_key[ 0 ] == '\0' ) return;
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
	snprintf( job->body, sizeof( job->body ),
		"{\"model\":\"%s\",\"messages\":["
		"{\"role\":\"system\",\"content\":\"%s\"},"
		"{\"role\":\"user\",\"content\":\"%s\"}],"
		"\"temperature\":0.3,\"max_tokens\":400}",
		ai_model, sys_esc, usr_esc );
	if ( pthread_create( &th, NULL, ai_thread, job ) != 0 ) {
		free( job );
		ai_busy = 0;
		return;
	}
	pthread_detach( th );
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
		flight is still worth a line.  A short gap keeps a burst of game
		events from turning into a burst of paid requests.					*/
	pthread_mutex_lock( &ai_lock );
	if ( ai_event_prio >= 0 && now - ai_last_request >= 3.0 ) {
		char event[ 512 ];
		snprintf( event, sizeof( event ), "%s", ai_event );
		ai_event_prio = -1;
		ai_event[ 0 ] = '\0';
		pthread_mutex_unlock( &ai_lock );
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

	if ( !ai_idle && now - ai_last_request >= (double)ai_interval && ai_scene[ 0 ] != '\0' ) {
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
