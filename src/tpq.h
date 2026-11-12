/* C interface to tilepalquant (8x8 tiles, 16 colors per palette, 3 bits per channel). */
#ifndef TPQ_H
#define TPQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int      npal;      /* 1..4 */
    int      dither;    /* 0 off, 1 fast, 2 slow */
    int      pattern;   /* tilepalquant dither pattern 0..5 */
    float    weight;    /* dither weight */
    float    fraction;  /* fraction of pixels sampled per stage */
    unsigned seed;
} tpq_opts;

/* rgb: w*h*3 bytes, w and h multiples of 8.
   pal: npal*16*3 bytes out, 8-bit RGB on the 3-bit grid; entry 0 of each palette is black.
   idx: w*h bytes out, palette*16 + color. */
int tpq_quantize(const tpq_opts *o, const uint8_t *rgb, int w, int h,
                 uint8_t *pal, uint8_t *idx);

/* Map rgb onto existing palettes (pal as returned by tpq_quantize). */
int tpq_remap(const tpq_opts *o, const uint8_t *rgb, int w, int h,
              const uint8_t *pal, uint8_t *idx);

#ifdef __cplusplus
}
#endif

#endif
