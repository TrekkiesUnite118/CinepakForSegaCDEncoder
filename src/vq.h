/* Sega CD Cinepak frame encoder: per-tile mode decision (skip/raw/cb32/cb16)
   with rate-distortion optimized codebooks. */
#ifndef VQ_H
#define VQ_H

#include <stdint.h>
#include <stddef.h>

typedef struct vq_enc vq_enc;

typedef struct {
    int    skip, raw, cb32, cb16;  /* tile counts per mode */
    int    k32, k16;               /* codebook sizes */
    double mse;                    /* per pixel, summed over RGB */
} vq_stats;

/* w, h: multiples of 8; npal: palettes emitted per frame (2..4) */
vq_enc *vq_create(int w, int h, int npal, int iterations);
void    vq_destroy(vq_enc *v);
size_t  vq_max_size(const vq_enc *v);

/* ref: w*h*3 reference RGB; idx: w*h target indices (palette*16 + color, all pixels
   of a tile in one palette); pal: npal*16*3 RGB. Writes one 'SM' sample to out.
   Does not change encoder state until vq_commit(). */
size_t vq_encode(vq_enc *v, const uint8_t *ref, const uint8_t *idx, const uint8_t *pal,
                 double lambda, uint8_t *out, vq_stats *st);

/* accept the last vq_encode() result as decoded frame */
void vq_commit(vq_enc *v);

/* decoded RGB of the last vq_encode() (w*h*3) */
void vq_decoded_rgb(const vq_enc *v, uint8_t *rgb);

#endif
