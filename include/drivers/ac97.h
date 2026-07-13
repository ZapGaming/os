#ifndef DRIVERS_AC97_H
#define DRIVERS_AC97_H

#include <stdint.h>

/* Detects and initializes an Intel ICH AC97 audio codec (vendor 0x8086
 * device 0x2415 -- what QEMU's `-device AC97` emulates). Returns 1 if
 * found, 0 otherwise (the GUI just shows "no audio device" -- this is
 * not a fatal error). */
int ac97_init(void);
int ac97_is_present(void);

/* Starts playing 16-bit signed PCM `data` (interleaved if stereo) via
 * bus-master DMA and returns immediately -- fire-and-forget; poll
 * ac97_is_playing() for completion. `sample_count` is the total number
 * of 16-bit words (frames * channels), and a new call stops whatever
 * was already playing first. Fixed at 48000 Hz -- no resampling, so
 * the source audio must already be at that rate. Returns 0 if the clip
 * is longer than the driver's fixed 32-descriptor buffer can address
 * (~21.8 seconds of 48kHz stereo audio) or no codec was found. */
int ac97_play_pcm(const int16_t *data, uint32_t sample_count, int stereo);
int ac97_is_playing(void);
void ac97_stop(void);

#endif
