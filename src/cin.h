/* Sega CD Cinepak FILM container (FILM/FDSC/STAB). */
#ifndef CIN_H
#define CIN_H

#include <stdint.h>
#include <stddef.h>

#define CIN_BASE_RATE  600
#define CIN_PCM_CHUNK  0x4000
/* rate the player assumes for its clock vs. the rate it actually plays (FD = 0x400) */
#define CIN_PCM_NOMINAL 16000.0
#define CIN_PCM_RATE    (12500000.0 / 384.0 * 0x400 / 0x800)
#define CIN_AUDIO_TIME  0xFFFFFFFFu

typedef struct {
    uint32_t off, size, time, dur;
} cin_entry;

typedef struct {
    int        width, height;
    cin_entry *ents;
    int        nents, cap;
    uint8_t   *data;
    size_t     len, dcap;
} cin_file;

void cin_init(cin_file *c, int width, int height);
void cin_free(cin_file *c);
void cin_add(cin_file *c, const void *p, size_t n, uint32_t time, uint32_t dur);
int  cin_write(const cin_file *c, const char *path);
int  cin_read(cin_file *c, const char *path);

/* player-clock duration of one video frame in STAB units */
double cin_frame_duration(double fps);

/* Rebuild dst from the video entries of src, interleaving pcm the way
   the Sega encoder did (pcm may be NULL). */
void cin_mux(cin_file *dst, const cin_file *src, const uint8_t *pcm, size_t pcmlen);

static inline void cin_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static inline uint32_t cin_get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

#endif
