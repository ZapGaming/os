#ifndef DRIVERS_WAV_H
#define DRIVERS_WAV_H

#include <stdint.h>

struct wav_info {
    const int16_t *pcm;    /* points into the buffer passed to wav_parse */
    uint32_t sample_count; /* total 16-bit words = frames * channels */
    int channels;
    uint32_t sample_rate;
    int bits_per_sample;
};

/* Parses a RIFF/WAVE file already in memory (only the "fmt " and
 * "data" chunks are understood -- others are skipped). Returns 1 if it
 * found valid PCM audio (`out` is filled in either way when a fmt+data
 * pair is found, but only integer PCM sets bits_per_sample == 16
 * *and* returns 1 -- check both `out->bits_per_sample` and the return
 * value before trusting the format is playable). Returns 0 for
 * anything else (not a WAVE file, compressed/float audio, malformed). */
int wav_parse(const uint8_t *data, uint32_t len, struct wav_info *out);

#endif
