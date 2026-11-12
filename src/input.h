/* Video frame sources: raw frames (VirtualDub external encoder), YUV4MPEG2. */
#ifndef INPUT_H
#define INPUT_H

#include <stdio.h>
#include <stdint.h>

typedef struct vsrc vsrc;

/* fmt: yuv420p (default), yuv422p, yuv444p, gray8, bgr24, bgra */
vsrc *vsrc_open_raw(FILE *fp, int w, int h, const char *fmt, int fullrange);
vsrc *vsrc_open_y4m(FILE *fp);

int    vsrc_width(const vsrc *s);
int    vsrc_height(const vsrc *s);
/* 0 if the source carries no rate */
double vsrc_fps(const vsrc *s);

/* rgb: width*height*3; returns 1 for a frame, 0 at end, -1 on error */
int  vsrc_read(vsrc *s, uint8_t *rgb);
void vsrc_close(vsrc *s);

#endif
