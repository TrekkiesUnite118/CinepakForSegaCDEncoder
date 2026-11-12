#include <stdlib.h>
#include <string.h>

#include "input.h"

enum { SRC_I420, SRC_Y4M };
enum { CS_420, CS_422, CS_444, CS_MONO, CS_BGR24, CS_BGRA };

struct vsrc {
    int      type;
    FILE    *fp;
    int      w, h, cs, fullrange;
    double   fps;
    uint8_t *buf;
    size_t   framesize;
};

static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void yuv_to_rgb(const vsrc *s, uint8_t *rgb)
{
    const uint8_t *y = s->buf, *u, *v;
    int cw, ch, x, yy;

    if (s->cs == CS_BGR24 || s->cs == CS_BGRA) {
        int bpp = s->cs == CS_BGRA ? 4 : 3;
        size_t i, n = (size_t)s->w * s->h;
        for (i = 0; i < n; i++, rgb += 3, y += bpp) {
            rgb[0] = y[2];
            rgb[1] = y[1];
            rgb[2] = y[0];
        }
        return;
    }
    switch (s->cs) {
    case CS_422:  cw = (s->w + 1) / 2; ch = s->h; break;
    case CS_444:  cw = s->w; ch = s->h; break;
    case CS_MONO: cw = 0; ch = 0; break;
    default:      cw = (s->w + 1) / 2; ch = (s->h + 1) / 2; break;
    }
    u = y + (size_t)s->w * s->h;
    v = u + (size_t)cw * ch;
    for (yy = 0; yy < s->h; yy++) {
        for (x = 0; x < s->w; x++) {
            int Y = y[(size_t)yy * s->w + x], U = 128, V = 128, c, d, e, r, g, b;
            if (cw) {
                int cx = s->cs == CS_444 ? x : x / 2;
                int cy = s->cs == CS_420 ? yy / 2 : yy;
                U = u[(size_t)cy * cw + cx];
                V = v[(size_t)cy * cw + cx];
            }
            d = U - 128;
            e = V - 128;
            /* BT.601, 16.16 fixed point */
            if (s->fullrange) {
                c = Y << 16;
                r = c + 91881 * e;
                g = c - 22554 * d - 46802 * e;
                b = c + 116130 * d;
            } else {
                c = 76309 * (Y - 16);
                r = c + 104597 * e;
                g = c - 25675 * d - 53279 * e;
                b = c + 132201 * d;
            }
            rgb[0] = (uint8_t)clamp8((r + 32768) >> 16);
            rgb[1] = (uint8_t)clamp8((g + 32768) >> 16);
            rgb[2] = (uint8_t)clamp8((b + 32768) >> 16);
            rgb += 3;
        }
    }
}

static size_t yuv_size(int w, int h, int cs)
{
    size_t l = (size_t)w * h;
    switch (cs) {
    case CS_422:  return l + 2 * (size_t)((w + 1) / 2) * h;
    case CS_444:  return 3 * l;
    case CS_MONO: return l;
    case CS_BGR24: return 3 * l;
    case CS_BGRA: return 4 * l;
    default:      return l + 2 * (size_t)((w + 1) / 2) * ((h + 1) / 2);
    }
}

static vsrc *alloc_yuv(int type, FILE *fp, int w, int h, int cs, int fullrange, double fps)
{
    vsrc *s = calloc(1, sizeof *s);
    s->type = type;
    s->fp = fp;
    s->w = w;
    s->h = h;
    s->cs = cs;
    s->fullrange = fullrange;
    s->fps = fps;
    s->framesize = yuv_size(w, h, cs);
    s->buf = malloc(s->framesize);
    return s;
}

vsrc *vsrc_open_raw(FILE *fp, int w, int h, const char *fmt, int fullrange)
{
    int cs;
    if (w <= 0 || h <= 0) return NULL;
    if (!fmt || !strcmp(fmt, "yuv420p") || !strcmp(fmt, "i420")) cs = CS_420;
    else if (!strcmp(fmt, "yuv422p")) cs = CS_422;
    else if (!strcmp(fmt, "yuv444p")) cs = CS_444;
    else if (!strcmp(fmt, "gray8")) cs = CS_MONO;
    else if (!strcmp(fmt, "bgr24")) cs = CS_BGR24;
    else if (!strcmp(fmt, "bgra")) cs = CS_BGRA;
    else { fprintf(stderr, "input: unsupported pixel format %s\n", fmt); return NULL; }
    return alloc_yuv(SRC_I420, fp, w, h, cs, fullrange, 0);
}

/* reads one header line; returns length or -1 */
static int read_line(FILE *fp, char *line, int max)
{
    int n = 0, c;
    while ((c = fgetc(fp)) != EOF && c != '\n')
        if (n < max - 1) line[n++] = (char)c;
    line[n] = 0;
    return (c == EOF && n == 0) ? -1 : n;
}

vsrc *vsrc_open_y4m(FILE *fp)
{
    char line[1024], *tok;
    int w = 0, h = 0, cs = CS_420, full = 0;
    double fps = 0;

    if (read_line(fp, line, sizeof line) < 0 || strncmp(line, "YUV4MPEG2", 9)) {
        fprintf(stderr, "input: not a YUV4MPEG2 stream\n");
        return NULL;
    }
    for (tok = strtok(line + 9, " "); tok; tok = strtok(NULL, " ")) {
        switch (tok[0]) {
        case 'W': w = atoi(tok + 1); break;
        case 'H': h = atoi(tok + 1); break;
        case 'F': {
            long num = 0, den = 1;
            if (sscanf(tok + 1, "%ld:%ld", &num, &den) == 2 && den) fps = (double)num / den;
            break;
        }
        case 'C':
            if (!strcmp(tok + 1, "420") || !strcmp(tok + 1, "420jpeg") ||
                !strcmp(tok + 1, "420mpeg2") || !strcmp(tok + 1, "420paldv")) cs = CS_420;
            else if (!strcmp(tok + 1, "422")) cs = CS_422;
            else if (!strcmp(tok + 1, "444")) cs = CS_444;
            else if (!strcmp(tok + 1, "mono")) cs = CS_MONO;
            else { fprintf(stderr, "input: unsupported Y4M colorspace %s (need 8-bit)\n", tok + 1); return NULL; }
            break;
        case 'X':
            if (!strcmp(tok, "XCOLORRANGE=FULL")) full = 1;
            break;
        }
    }
    if (w <= 0 || h <= 0) { fprintf(stderr, "input: bad Y4M size\n"); return NULL; }
    return alloc_yuv(SRC_Y4M, fp, w, h, cs, full, fps);
}

int vsrc_width(const vsrc *s) { return s->w; }
int vsrc_height(const vsrc *s) { return s->h; }
double vsrc_fps(const vsrc *s) { return s->fps; }

int vsrc_read(vsrc *s, uint8_t *rgb)
{
    if (s->type == SRC_Y4M) {
        char line[256];
        if (read_line(s->fp, line, sizeof line) < 0) return 0;
        if (strncmp(line, "FRAME", 5)) { fprintf(stderr, "input: bad Y4M frame header\n"); return -1; }
    }
    {
        size_t got = fread(s->buf, 1, s->framesize, s->fp);
        if (got == 0) return 0;
        if (got != s->framesize) {
            fprintf(stderr, "input: truncated frame (%zu of %zu bytes)\n", got, s->framesize);
            return 0;
        }
    }
    yuv_to_rgb(s, rgb);
    return 1;
}

void vsrc_close(vsrc *s)
{
    if (!s) return;
    free(s->buf);
    free(s);
}
