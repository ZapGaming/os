//
// ZapOS replacement for i_sound.c/i_*music.c/i_*sound.c.
//
// No audio at all in this port (v1) -- the AC97 driver only exists in
// the kernel today, and there's no syscall to reach it from an
// isolated ring-3 program. Every I_* sound/music entry point is a
// real, harmless no-op rather than a missing symbol: the game logic
// (s_sound.c) calls these unconditionally throughout play, so they
// have to exist and behave sanely (never "succeed" at playing
// anything, never crash), not just link.

#include "i_sound.h"
#include "doomtype.h"

int snd_sfxdevice = SNDDEVICE_NONE;
int snd_musicdevice = SNDDEVICE_NONE;
int snd_samplerate = 0;
int snd_cachesize = 0;
int snd_maxslicetime_ms = 0;
char *snd_musiccmd = "";
int opl_io_port = 0;
char *timidity_cfg_path = "";

void I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void I_ShutdownSound(void) { }
int I_GetSfxLumpNum(sfxinfo_t *sfxinfo) { (void)sfxinfo; return -1; }
void I_UpdateSound(void) { }
void I_UpdateSoundParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }
int I_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep) {
    (void)sfxinfo; (void)channel; (void)vol; (void)sep;
    return -1;
}
void I_StopSound(int channel) { (void)channel; }
boolean I_SoundIsPlaying(int channel) { (void)channel; return false; }
void I_PrecacheSounds(sfxinfo_t *sounds, int num_sounds) { (void)sounds; (void)num_sounds; }

void I_InitMusic(void) { }
void I_ShutdownMusic(void) { }
void I_SetMusicVolume(int volume) { (void)volume; }
void I_PauseSong(void) { }
void I_ResumeSong(void) { }
void *I_RegisterSong(void *data, int len) { (void)data; (void)len; return NULL; }
void I_UnRegisterSong(void *handle) { (void)handle; }
void I_PlaySong(void *handle, boolean looping) { (void)handle; (void)looping; }
void I_StopSong(void) { }
boolean I_MusicIsPlaying(void) { return false; }

void I_BindSoundVariables(void) { }
void I_InitTimidityConfig(void) { }
