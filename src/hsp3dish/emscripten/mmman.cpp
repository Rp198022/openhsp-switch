//
//		MMMAN : Multimedia manager source
//				for SDL enviroment
//				onion software/onitama 2012/6
//				zakki 2014/7
//
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#ifdef HSPEMSCRIPTEN
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#else
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#endif

#include "../../hsp3/hsp3config.h"
#include "../../hsp3/dpmread.h"
#include "../../hsp3/strbuf.h"
#include "../supio.h"

#include "mmman.h"

#define sndbank(a) (char *)(mem_snd[a].mempt)

#define MIX_MAX_CHANNEL 16
//#define USE_MODMIX		// Use Modified SDLMix

//---------------------------------------------------------------------------

//	SDL Music Object
int curmusic;
Mix_Music *m_music;

void MusicInit( void )
{
	m_music = NULL;
	curmusic = -1;
}

void MusicTerm( void )
{
	if (m_music) {
		Mix_HaltMusic();
		Mix_FreeMusic(m_music);
		m_music = NULL;
		curmusic = -1;
	}
}

int MusicLoad( char *fname )
{
	MusicTerm();
	m_music=Mix_LoadMUS((const char *)fname);
	if (m_music==NULL) return -1;
	return 0;
}

void MusicPlay( int num, int mode )
{
	if (m_music==NULL) return;
	curmusic = num;
	Mix_HaltMusic();
	/*	F18: HSP's option bit 0 is the loop flag, but it was handed to
		SDL_mixer as a repeat count - mode 1 (loop forever) played twice
		and stopped, and mode 2 (wait) three times.  SDL's infinite loop
		is -1; the wait semantics stay unimplemented here.				*/
	Mix_PlayMusic( m_music, ( mode & 1 ) ? -1 : 0 );
}

void MusicStop( void )
{
	if (m_music==NULL) return;
	curmusic = -1;
	Mix_HaltMusic();
}

void MusicPause( void )
{
	if (m_music==NULL) return;
	if (Mix_PausedMusic()!=0) {
		Mix_ResumeMusic();
	} else {
		Mix_PauseMusic();
	}
}

void MusicVolume( int vol )
{
	Mix_VolumeMusic(vol);
}

int MusicStatus( int num, int type )
{
	if (m_music==NULL) return 0;
	if (num!=curmusic) return 0;

	int res=0;
	switch(type) {
	case 16:
		res = Mix_PlayingMusic();
		break;
	default:
		break;
	}
	return res;
}

//---------------------------------------------------------------------------

//	MMDATA structure
//
typedef struct MMM
{
	//	Multimedia Data structure
	//
	int		flag;			//	bank mode (0=none/1=wav/2=mid/3=cd/4=avi)
	int		opt;			//	option (0=none/1=loop/2=wait/16=fullscr)
	int		num;			//	request number
	short	track;			//	CD track No.
	short	lasttrk;		//	CD last track No.
	void	*mempt;			//	pointer to sound data
	char	*fname;			//	sound filename (sbstr)
	int		vol;
	int		pan;
	int		start;
	int		end;
	
	Mix_Chunk	*chunk;
	int		channel;
	// int	pause_flag;

} MMM;


/*	F17: which bank each SDL_mixer channel belongs to right now.  A bank
	remembers the channel it was played on, but SDL hands a finished
	channel to the next playback - and the first bank's record then
	pointed at the second bank's sound, so StopBank/SetVol/SetPan and
	GetStatus on one of them acted on the other.  -1 = unowned.			*/
static int sw_ch_bank[MIX_MAX_CHANNEL];

/*	True while the bank still owns its recorded channel; lazily drops
	the association once the channel went silent or was taken over.		*/
static int sw_bank_channel_live( MMM *m, int bank )
{
	int ch;
	if ( m == NULL ) return 0;
	ch = m->channel;
	if ( ch < 0 ) return 0;
	if ( sw_ch_bank[ch] != bank ) { m->channel = -1; return 0; }
	if ( !Mix_Playing( ch ) ) { sw_ch_bank[ch] = -1; m->channel = -1; return 0; }
	return 1;
}


//---------------------------------------------------------------------------

MMMan::MMMan()
{
	mm_cur = 0;

	mem_snd = NULL;
	engine_flag = false;
	{
		int ci;
		for ( ci = 0; ci < MIX_MAX_CHANNEL; ci++ ) sw_ch_bank[ci] = -1;
	}
	MusicInit();

	int init_result = Mix_Init(MIX_INIT_OGG|MIX_INIT_MP3);
	if (init_result != (MIX_INIT_OGG|MIX_INIT_MP3)) {
		Alertf("[MMMan] Mix_Init: %d OGG:%d MP3:%d\n", init_result, init_result & MIX_INIT_OGG, init_result & MIX_INIT_MP3);
	}

	int ret = Mix_OpenAudio(0, 0, 0, 0);

	int allocated_channels = Mix_AllocateChannels(MIX_MAX_CHANNEL);
	if (allocated_channels != MIX_MAX_CHANNEL) {
		printf("[MMMan] Mix_AllocateChannels: %d\n", allocated_channels);
	}

	//assert(ret == 0);
	engine_flag = ret == 0;
}


MMMan::~MMMan()
{
	ClearAllBank();
	MusicTerm();

	Mix_CloseAudio();

	while(Mix_Init(0))
		Mix_Quit();
}

void MMMan::DeleteBank( int bank )
{
	MMM *m;
	char *lpSnd;

	m = &(mem_snd[bank]);
	if ( m->flag == MMDATA_INTWAVE ) {
		Mix_FreeChunk( m->chunk );
	}
	if ( m->flag == MMDATA_MUSIC ) {
		if (m->num==curmusic) MusicTerm();
	}
	
	if (m->fname!=NULL) {
		free(m->fname);
		m->fname = NULL;
	}
	
	lpSnd = sndbank( bank );
	if ( lpSnd != NULL ) {
		free( lpSnd );
	}
	{
		int ci;
		for ( ci = 0; ci < MIX_MAX_CHANNEL; ci++ ) {
			if ( sw_ch_bank[ci] == bank ) sw_ch_bank[ci] = -1;
		}
	}
	mem_snd[bank].flag = MMDATA_NONE;
	mem_snd[bank].mempt=NULL;
	mem_snd[bank].num = -1;
	mem_snd[bank].channel = -1;
}


int MMMan::AllocBank( void )
{
	if ( !engine_flag ) return -1;

	int ids = mm_cur++;
	int sz = mm_cur * sizeof(MMM);
	if ( mem_snd == NULL ) {
		mem_snd = (MMM *)sbAlloc( sz );
	} else {
		mem_snd = (MMM *)sbExpand( (char *)mem_snd, sz );
	}
	mem_snd[ids].flag = MMDATA_NONE;
	mem_snd[ids].num = -1;
	mem_snd[ids].channel = -1;
	return ids;
}


int MMMan::SearchBank( int num )
{
	for(int a=0;a<mm_cur;a++) {
		if ( mem_snd[a].num == num ) return a;
	}
	return -1;
}


MMM *MMMan::SetBank( int num, int flag, int opt, void *mempt, char *fname, int start, int end )
{
	int bank;
	MMM *m;

	bank = SearchBank( num );
	if ( bank < 0 ) {
		bank = AllocBank();
	} else {
		StopBank( num );
		DeleteBank( bank );
	}

	if ( bank < 0 ) return NULL;

	m = &(mem_snd[bank]);
	m->flag = flag;
	m->opt = opt;
	m->num = num;
	m->mempt = mempt;
	m->fname = NULL;
	// m->pause_flag = 0;
	m->vol = -1;
	m->pan = 0;
	m->start = start;
	m->end = end;
	m->chunk = NULL;
	m->channel = -1;
	return m;
}


void MMMan::ClearAllBank( void )
{
	int a;
	if ( mem_snd != NULL ) {
		Stop();
		for(a=0;a<mm_cur;a++) {
			DeleteBank( a );
		}
		sbFree( mem_snd );
		mem_snd = NULL;
		mm_cur = 0;
	}
}


void MMMan::Reset( void *hwnd )
{
	ClearAllBank();
}


void MMMan::SetWindow( void *hwnd, int x, int y, int sx, int sy )
{
}


void MMMan::Pause( void )
{
	//		pause all playing sounds
	//
	Mix_Pause( -1 );
	MusicPause();
}


void MMMan::Resume( void )
{
	//		resume all playing sounds
	//
	Mix_Resume( -1 );
	MusicPause();
}


void MMMan::Stop( void )
{
	//		stop all playing sounds
	//
	int ci;
	MusicStop();
	Mix_HaltChannel( -1 );
	for ( ci = 0; ci < MIX_MAX_CHANNEL; ci++ ) sw_ch_bank[ci] = -1;
}


int MMMan::BankLoad( MMM *mmm, char *fname )
{
	bool is_music = false;
	char fext[8];
	if ( mmm == NULL ) return -9;

	getpath(fname,fext,16+2);
	if (!strcmp(fext,".mp3")) is_music = true;
	if (!strcmp(fext,".ogg")) is_music = true;
	if ( is_music ) {
		mmm->fname = (char *)malloc( strlen(fname)+1 );
		strcpy( mmm->fname,fname );
		mmm->flag = MMDATA_MUSIC;
		return 0;
	}

	mmm->chunk = Mix_LoadWAV( fname );
	if (mmm->chunk==NULL) return 1;
	mmm->flag = MMDATA_INTWAVE;
	return 0;
}


int MMMan::Load( char *fname, int num, int opt, int start, int end )
{
	//		Load sound to bank
	//			opt : 0=normal/1=loop/2=wait/3=continuous
	//
	int flag,res;
	MMM *mmm;

	flag = MMDATA_INTWAVE;
	mmm = SetBank( num, flag, opt, NULL, fname, start, end );

	if ( mmm != NULL ) {
		res = BankLoad( mmm, fname );
		if ( res ) {
			mmm->flag = MMDATA_NONE;
			Alertf( "[MMMan] Failed %s on bank #%d (%d)",fname,num,res );
			return -1;
		}
		//if ( opt == 1 ) SetLoopBank( mmm, opt );
	}
	Alertf( "[MMMan] Loaded %s on bank #%d",fname,num );
	return 0;
}


int MMMan::Play( int num, int ch )
{
	//		Play sound
	//
	int bank;
	MMM *m;
	bank = SearchBank(num);
	if ( bank < 0 ) return 1;
	m = &(mem_snd[bank]);
	if ( m == NULL ) {
		return -1;
	}
	bool loop = m->opt & 1;

	switch(m->flag) {
	case MMDATA_INTWAVE:
		if ( ch >= MIX_MAX_CHANNEL ) break;
		if (ch>=0) Mix_HaltChannel( ch );
#ifdef USE_MODMIX
		m->channel = Mix_PlayChannel( ch, m->chunk, loop ? -1 : 0 , m->start, m->end );
#else
		m->channel = Mix_PlayChannel( ch, m->chunk, loop ? -1 : 0 );
#endif
		if ( m->channel >= 0 ) {
			/*	F17: take the channel away from whichever bank played on
				it before, so that bank's later stop/volume/pan/status
				calls no longer reach this playback.					*/
			int prev;
			prev = sw_ch_bank[m->channel];
			if ( prev >= 0 && prev != bank ) mem_snd[prev].channel = -1;
			sw_ch_bank[m->channel] = bank;
			if (m->vol>=0) {
				Mix_Volume( m->channel, m->vol );
			}
		}
		break;
	case MMDATA_MUSIC:
		if ( MusicLoad(m->fname)!=0 ) return -1;
		if (m->vol>=0) {
			MusicVolume( m->vol );
		}
		MusicPlay(num,m->opt);
		break;
	default:
		return 2;
	}

	return 0;
}


void MMMan::Notify( void )
{
}


void MMMan::GetInfo( int bank, char **fname, int *num, int *flag, int *opt )
{
	//		Get MMM info
	//
	MMM *mmm;
	mmm=&mem_snd[bank];
	*fname = mmm->fname;
	*opt=mmm->opt;
	*flag=mmm->flag;
	*num=mmm->num;
}


void MMMan::SetVol( int num, int vol )
{
	MMM *mmm;
	int bank,flg;
	bank = SearchBank(num);
	if (bank<0) return;
	
	mmm=&mem_snd[bank];
	flg=mmm->flag;
	
	if ( vol >     0 ) vol =     0;
	if ( vol < -1000 ) vol = -1000;
	mmm->vol = (int)(MIX_MAX_VOLUME * ((float)(vol + 1000) / 1000.0f));

	if ( flg == MMDATA_INTWAVE ) {
		if ( sw_bank_channel_live( mmm, bank ) ) {
			Mix_Volume( mmm->channel, mmm->vol );
			if ( mmm->pan != 0 ) SetPan(num, mmm->pan);
		}
	}
	if ( flg == MMDATA_MUSIC ) {
		MusicVolume(mmm->vol);
	}
}



void MMMan::SetPan( int num, int p_pan )
{
	int pan = p_pan;

	MMM *mmm;
	int bank,flg;
	bank = SearchBank(num);
	if (bank<0) return;
	
	mmm=&mem_snd[bank];
	flg=mmm->flag;
	
	if ( pan >  1000 ) pan =  1000;
	if ( pan < -1000 ) pan = -1000;
	mmm->pan = pan;

	int l, r;
	int max = 255;
	if ( pan > 0 ) {
		l = (1000 - pan) * max;
		r = 1000 * max;
	} else {
		l = 1000 * max;
		r = (1000 + pan) * max;
	}
	if ( flg == MMDATA_INTWAVE ) {
		if ( sw_bank_channel_live( mmm, bank ) ) {
			Mix_SetPanning( mmm->channel, l / 1000, r / 1000 );
		}
	}
}


int MMMan::GetStatus( int num, int infoid )
{
	MMM *mmm;
	int bank,flg;
	bank = SearchBank(num);
	if (bank<0) return 0;
	
	mmm=&mem_snd[bank];
	flg=mmm->flag;

	int res = 0;
	switch( infoid ) {
	case 0:
		res = mmm->opt;
		break;
	case 1:
		res = mmm->vol;
		break;
	case 2:
		res = mmm->pan;
		break;
	case 16:
		if ( num < 0 ) {
			res = Mix_Playing( -1 );
			break;
		}
		if ( flg == MMDATA_INTWAVE ) {
			if ( sw_bank_channel_live( mmm, bank ) ) res = 1;
		}
		if ( flg == MMDATA_MUSIC ) {
			res = MusicStatus(num,infoid);
		}
		break;
	}
	return res;
}


void MMMan::StopBank( int num )
{
	//		stop playing sound
	//
	if ( num < 0 ) {
		Stop();
		return;
	}

	MMM *mmm;
	int bank,flg;
	bank = SearchBank(num);
	if (bank<0) return;
	
	mmm=&mem_snd[bank];
	flg=mmm->flag;

	if ( flg == MMDATA_INTWAVE ) {
		if ( sw_bank_channel_live( mmm, bank ) ) {
			Mix_HaltChannel( mmm->channel );
			sw_ch_bank[mmm->channel] = -1;
			mmm->channel = -1;
		}
	}
	if ( flg == MMDATA_MUSIC ) {
		if (num==curmusic) MusicStop();
	}
	return;
}
