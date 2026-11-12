#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "audio.h"
#include "cin.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FMT_PCM        1
#define FMT_FLOAT      3
#define FMT_EXTENSIBLE 0xFFFE
#define SINC_HALF      24
#define WIN_TABLE      4096

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static double sample_at(const uint8_t *p, int fmt, int bits)
{
    switch (bits) {
    case 8:  return (p[0] - 128) / 128.0;
    case 16: return (int16_t)le16(p) / 32768.0;
    case 24: return (double)((int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) >> 8) / 8388608.0;
    case 32:
        if (fmt == FMT_FLOAT) { float f; uint32_t u = le32(p); memcpy(&f, &u, 4); return f; }
        return (int32_t)le32(p) / 2147483648.0;
    case 64:
        if (fmt == FMT_FLOAT) {
            double d; uint64_t u = le32(p) | ((uint64_t)le32(p + 4) << 32);
            memcpy(&d, &u, 8); return d;
        }
    }
    return 0;
}

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    int k;
    for (k = 1; k < 40; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

/* windowed-sinc (Kaiser) resampler */
static double *resample(const double *in, size_t n, double inrate, double outrate, size_t *outn)
{
    const double beta = 8.0;
    double ratio = outrate / inrate, cutoff = ratio < 1 ? ratio * 0.95 : 0.95;
    size_t m = (size_t)floor((double)n * ratio), i;
    double *out = malloc((m ? m : 1) * sizeof *out), i0b = bessel_i0(beta);
    double scale = cutoff, win[WIN_TABLE + 1];
    long half = (long)ceil(SINC_HALF / scale);

    for (i = 0; i <= WIN_TABLE; i++) {
        double r = (double)i / WIN_TABLE;
        win[i] = bessel_i0(beta * sqrt(1 - r * r)) / i0b;
    }
    for (i = 0; i < m; i++) {
        double t = (double)i / ratio, acc = 0, wsum = 0;
        long c = (long)floor(t), k;
        for (k = c - half + 1; k <= c + half; k++) {
            double x = t - (double)k, u = x * scale, w, s, r = fabs(x) / half;
            if (k < 0 || k >= (long)n || r >= 1) continue;
            w = win[(int)(r * WIN_TABLE + 0.5)];
            s = fabs(u) < 1e-9 ? 1 : sin(M_PI * u) / (M_PI * u);
            acc += in[k] * s * w;
            wsum += s * w;
        }
        out[i] = wsum != 0 ? acc / wsum : 0;
    }
    *outn = m;
    return out;
}

int audio_convert(const uint8_t *wav, size_t wavlen, const audio_opts *o,
                  uint8_t **pcm, size_t *pcmlen)
{
    size_t pos = 12, datalen = 0, frames, i, outn;
    const uint8_t *data = NULL;
    int fmt = 0, ch = 0, bits = 0, block;
    uint32_t rate = 0;
    double *mono, *res, peak = 0, gain;
    uint8_t *out;

    if (wavlen < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) {
        fprintf(stderr, "audio: not a WAV file\n");
        return -1;
    }
    while (pos + 8 <= wavlen) {
        uint32_t clen = le32(wav + pos + 4);
        if (!memcmp(wav + pos, "fmt ", 4) && pos + 8 + 16 <= wavlen) {
            const uint8_t *f = wav + pos + 8;
            fmt = le16(f);
            ch = le16(f + 2);
            rate = le32(f + 4);
            bits = le16(f + 14);
            if (fmt == FMT_EXTENSIBLE && clen >= 26 && pos + 8 + 26 <= wavlen)
                fmt = le16(f + 24);
        } else if (!memcmp(wav + pos, "data", 4)) {
            data = wav + pos + 8;
            datalen = wavlen - (pos + 8);
            /* streamed WAVs leave this 0 or 0xFFFFFFFF */
            if (clen != 0 && clen != 0xFFFFFFFFu && clen <= datalen)
                datalen = clen;
            break;
        }
        if (clen > wavlen - pos - 8) break;
        pos += 8 + ((clen + 1) & ~1u);
    }
    if (!data || !ch || !rate || !(fmt == FMT_PCM || fmt == FMT_FLOAT) ||
        !(bits == 8 || bits == 16 || bits == 24 || bits == 32 || (bits == 64 && fmt == FMT_FLOAT))) {
        fprintf(stderr, "audio: unsupported WAV (format %d, %d bits, %d channels)\n", fmt, bits, ch);
        return -1;
    }

    block = ch * bits / 8;
    frames = datalen / block;
    mono = malloc((frames ? frames : 1) * sizeof *mono);
    for (i = 0; i < frames; i++) {
        double s = 0;
        int c;
        for (c = 0; c < ch; c++)
            s += sample_at(data + i * block + c * (bits / 8), fmt, bits);
        mono[i] = s / ch;
    }
    res = resample(mono, frames, rate, CIN_PCM_RATE, &outn);
    free(mono);

    gain = o ? o->gain : 1.0;
    if (o && o->normalize) {
        for (i = 0; i < outn; i++) if (fabs(res[i]) > peak) peak = fabs(res[i]);
        if (peak > 0) gain = 1.0 / peak;
    }

    out = malloc(outn + 2);
    for (i = 0; i < outn; i++) {
        long v = lround(res[i] * gain * 127.0);
        /* 0xFF (negative 127) is the RF5C164 loop/end marker */
        if (v > 127) v = 127;
        if (v < -126) v = -126;
        out[i] = v > 0 ? (uint8_t)v : (uint8_t)(0x80 | -v);
    }
    out[outn] = 0xFF;
    out[outn + 1] = 0xFF;
    free(res);
    *pcm = out;
    *pcmlen = outn + 2;
    return 0;
}
