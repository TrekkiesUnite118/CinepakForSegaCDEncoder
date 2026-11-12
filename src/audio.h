/* WAV to Sega CD RF5C164 PCM (8-bit sign-magnitude, mono, ~16276 Hz). */
#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    double gain;       /* linear gain applied before conversion */
    int    normalize;  /* scale peak to full range */
} audio_opts;

/* Reads a whole WAV stream (lengths may be 0/garbage for streamed WAVs).
   Returns 0 on success; *pcm is malloc'd and ends with two 0xFF bytes. */
int audio_convert(const uint8_t *wav, size_t wavlen, const audio_opts *o,
                  uint8_t **pcm, size_t *pcmlen);

#endif
