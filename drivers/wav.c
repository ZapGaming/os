#include <drivers/wav.h>
#include <string.h>

int wav_parse(const uint8_t *data, uint32_t len, struct wav_info *out) {
    if (len < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return 0;

    memset(out, 0, sizeof(*out));
    int have_fmt = 0;
    uint16_t audio_format = 0;

    uint32_t pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *id = data + pos;
        uint32_t size;
        memcpy(&size, data + pos + 4, 4);
        uint32_t body = pos + 8;

        if (memcmp(id, "fmt ", 4) == 0 && body + 16 <= len) {
            uint16_t channels, bits;
            uint32_t rate;
            memcpy(&audio_format, data + body + 0, 2);
            memcpy(&channels, data + body + 2, 2);
            memcpy(&rate, data + body + 4, 4);
            memcpy(&bits, data + body + 14, 2);
            out->channels = channels;
            out->sample_rate = rate;
            out->bits_per_sample = bits;
            have_fmt = 1;
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt || body > len) return 0;
            uint32_t avail = size;
            if (body + avail > len) avail = len - body; /* truncated file safety */
            out->pcm = (const int16_t *)(data + body);
            out->sample_count = avail / 2;
            return audio_format == 1 && out->bits_per_sample == 16;
        }

        pos = body + size + (size & 1); /* chunks are word-aligned */
    }
    return 0;
}
