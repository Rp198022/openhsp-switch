//
//	src/hsp3dish/switch/ai_chat.h
//	Companion chatter through the DeepSeek API.
//
//	The panel already shows what the pet says in the game; this module adds a
//	second line written by a model instead - a remark about what just happened
//	or an answer to something the player typed.  Everything stays on the panel:
//	the game's own text, its log and its savedata are never touched.
//
//	The split of work is deliberate.  This port decides *whether* something is
//	worth saying and *what happened* (in plain words); the model only chooses
//	the wording in character.  That keeps the requests short and predictable.
//
//	Files on the SD card (all optional; without them the module stays asleep):
//	  data/ai_config.txt     key/model/interval/enable
//	  data/npc/<name>.md     persona + rolling memory, one file per follower
//	  data/npc_log.txt       every line ever shown, one per row
//
#ifndef SWITCH_AI_CHAT_H
#define SWITCH_AI_CHAT_H

/*	What the panel shows next to the AI line.							*/
enum {
	AI_STATE_OFF = 0,		/* no config, no key, or disabled		*/
	AI_STATE_READY,			/* armed, waiting for a trigger			*/
	AI_STATE_BUSY,			/* a request is in flight				*/
	AI_STATE_ERROR			/* last request failed					*/
};

/*	Read the config and load the persona of the first follower.  Safe to call
	more than once; the first call wins.									*/
void ai_chat_init( void );

/*	Refresh the scene the next request will describe.  Called every frame by
	the overlay with a ready-made, human-readable block (already UTF-8).		*/
void ai_chat_set_context( const char *npc_name, const char *scene );

/*	The party has no follower any more: drop her persona, history, pending
	question and queued event, so nothing is spent with nobody to speak.	*/
void ai_chat_drop_context( void );

/*	Queue a line because something happened.  `event` is the plain sentence
	the model is told (e.g. "刚刚的战斗结束了，你们赢了"); priority decides
	which event wins when two arrive inside one cooldown window.			*/
void ai_chat_notify_event( const char *event, int priority );

/*	Drive the worker: start a request when one is due, collect the answer.
	Call once per frame from the present pass.								*/
void ai_chat_tick( void );

/*	Nothing in the game has moved for a while.  While this is set the idle
	timer stays asleep, so an unattended console spends nothing; a question
	or a real event still goes through immediately.						*/
void ai_chat_set_idle( int idle );

/*	The newest line to draw, or "" when there is nothing new.  UTF-8.		*/
const char *ai_chat_text( void );

/*	The panel shows the last three lines; index 0 is the newest, and the
	oldest is pushed out when a fourth arrives.								*/
#define AI_HISTORY	3
const char *ai_chat_history( int index );
int ai_chat_history_count( void );

/*	Bumps every time a new line is shown, including a line that happens to
	repeat the previous one.  The panel watches this instead of the text so a
	repeated answer still redraws.											*/
int ai_chat_seq( void );

/*	State for the status row, plus the last error in words.				*/
int ai_chat_state( void );
const char *ai_chat_status_text( void );

/*	Ask the model a direct question typed by the player.  Returns 1 when the
	question was queued, 0 when the module is not ready.					*/
int ai_chat_ask( const char *question );

/*	Append a line to the persona's memory block ("/设定" from the keyboard).	*/
int ai_chat_remember( const char *instruction );

void ai_chat_shutdown( void );

#endif
