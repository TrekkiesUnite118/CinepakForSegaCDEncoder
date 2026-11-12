#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cin.h"

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { fprintf(stderr, "out of memory\n"); exit(1); }
    return q;
}

void cin_init(cin_file *c, int width, int height)
{
    memset(c, 0, sizeof *c);
    c->width = width;
    c->height = height;
}

void cin_free(cin_file *c)
{
    free(c->ents);
    free(c->data);
    memset(c, 0, sizeof *c);
}

void cin_add(cin_file *c, const void *p, size_t n, uint32_t time, uint32_t dur)
{
    size_t padded = (n + 1) & ~(size_t)1;
    if (c->nents == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 256;
        c->ents = xrealloc(c->ents, c->cap * sizeof *c->ents);
    }
    if (c->len + padded > c->dcap) {
        c->dcap = (c->len + padded) * 2;
        c->data = xrealloc(c->data, c->dcap);
    }
    c->ents[c->nents].off = (uint32_t)c->len;
    c->ents[c->nents].size = (uint32_t)padded;
    c->ents[c->nents].time = time;
    c->ents[c->nents].dur = dur;
    c->nents++;
    memcpy(c->data + c->len, p, n);
    if (padded != n) c->data[c->len + n] = 0;
    c->len += padded;
}

int cin_write(const cin_file *c, const char *path)
{
    size_t stablen = 16 + 16 * (size_t)c->nents + 16, filmlen = 16 + 20 + stablen;
    uint8_t *hdr = calloc(1, filmlen), *p = hdr;
    FILE *fp;
    int i, ok;

    if (!hdr) return -1;
    memcpy(p, "FILM", 4); cin_put32(p + 4, (uint32_t)filmlen); p += 16;
    memcpy(p, "FDSC", 4); cin_put32(p + 4, 20); memcpy(p + 8, "SEGA", 4);
    cin_put32(p + 12, (uint32_t)c->height); cin_put32(p + 16, (uint32_t)c->width); p += 20;
    memcpy(p, "STAB", 4); cin_put32(p + 4, (uint32_t)stablen);
    cin_put32(p + 8, CIN_BASE_RATE); cin_put32(p + 12, (uint32_t)c->nents); p += 16;
    for (i = 0; i < c->nents; i++, p += 16) {
        cin_put32(p, c->ents[i].off);
        cin_put32(p + 4, c->ents[i].size);
        cin_put32(p + 8, c->ents[i].time);
        cin_put32(p + 12, c->ents[i].dur);
    }
    if (!(fp = fopen(path, "wb"))) { free(hdr); return -1; }
    ok = fwrite(hdr, 1, filmlen, fp) == filmlen && fwrite(c->data, 1, c->len, fp) == c->len;
    ok = (fclose(fp) == 0) && ok;
    free(hdr);
    return ok ? 0 : -1;
}

int cin_read(cin_file *c, const char *path)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *buf;
    long size;
    uint32_t filmlen, stablen, n, i;
    const uint8_t *p;

    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || size < 52 || fread(buf, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp); free(buf); return -1;
    }
    fclose(fp);
    filmlen = cin_get32(buf + 4);
    p = buf + 16;
    if (memcmp(buf, "FILM", 4) || memcmp(p, "FDSC", 4) || filmlen > (uint32_t)size) { free(buf); return -1; }
    cin_init(c, (int)cin_get32(p + 16), (int)cin_get32(p + 12));
    p += cin_get32(p + 4);
    if (memcmp(p, "STAB", 4)) { free(buf); return -1; }
    stablen = cin_get32(p + 4);
    n = cin_get32(p + 12);
    if (16 + 16 * (uint64_t)n > stablen) { free(buf); return -1; }
    for (i = 0; i < n; i++) {
        const uint8_t *e = p + 16 + 16 * i;
        uint32_t off = cin_get32(e), sz = cin_get32(e + 4);
        if ((uint64_t)filmlen + off + sz > (uint64_t)size) { cin_free(c); free(buf); return -1; }
        cin_add(c, buf + filmlen + off, sz, cin_get32(e + 8), cin_get32(e + 12));
    }
    free(buf);
    return 0;
}

double cin_frame_duration(double fps)
{
    return CIN_BASE_RATE / fps * CIN_PCM_RATE / CIN_PCM_NOMINAL;
}

void cin_mux(cin_file *dst, const cin_file *src, const uint8_t *pcm, size_t pcmlen)
{
    const double chunktime = CIN_BASE_RATE * CIN_PCM_CHUNK / CIN_PCM_NOMINAL;
    double pcmtime = -0.4 * chunktime;
    size_t pos = 0;
    int i;

    cin_init(dst, src->width, src->height);
    for (i = 0; i < src->nents; i++) {
        const cin_entry *e = &src->ents[i];
        if (e->time == CIN_AUDIO_TIME) continue;
        while (pos < pcmlen && (double)e->time > pcmtime) {
            size_t n = pcmlen - pos < CIN_PCM_CHUNK ? pcmlen - pos : CIN_PCM_CHUNK;
            cin_add(dst, pcm + pos, n, CIN_AUDIO_TIME, 1);
            pos += n;
            pcmtime += chunktime;
        }
        cin_add(dst, src->data + e->off, e->size, e->time, e->dur);
    }
    while (pos < pcmlen) {
        size_t n = pcmlen - pos < CIN_PCM_CHUNK ? pcmlen - pos : CIN_PCM_CHUNK;
        cin_add(dst, pcm + pos, n, CIN_AUDIO_TIME, 1);
        pos += n;
    }
}
