/* scdcpk: Sega CD Cinepak (FILM/STAB) encoder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "cin.h"
#include "audio.h"
#include "input.h"
#include "vq.h"
#include "tpq.h"
#include "par.h"

#define MAX_TW    32
#define MAX_TH    28
#define STOCK_MAX_TILES 767

typedef struct {
    int         npal;
    double      rate;        /* video bytes per second */
    double      fps;
    double      lambda;      /* > 0: constant lambda, no rate control */
    double      repal;       /* palette re-quantize threshold, 0 = every frame */
    int         dither;
    int         pattern;
    double      dweight;     /* tilepalquant dither weight, 0 = its default */
    double      fraction;    /* tilepalquant fraction of pixels, 0 = its default */
    int         iters;
    int         maxframes;
    unsigned    seed;
    int         rawsize[2];
    int         fullrange;
    const char *pixfmt;
    int         quiet;
    const char *input;
    const char *output;
    const char *wav;
    const char *preview;
    audio_opts  aud;
} options;

static void usage(void)
{
    fprintf(stderr,
        "scdcpk - Sega CD Cinepak encoder\n"
        "\n"
        "  scdcpk encode -i INPUT -o OUT.CIN [--wav AUDIO.WAV] [video options]\n"
        "      INPUT: file.y4m or '-' (Y4M on stdin)\n"
        "  scdcpk video --size WxH --fps N -o OUT.CIN [video options]\n"
        "      raw frames on stdin (VirtualDub external encoder), I420 unless --pixfmt\n"
        "  scdcpk audio [-i IN.WAV|-] -o OUT.PCM [--gain G] [--normalize]\n"
        "  scdcpk mux VIDEO.CIN AUDIO.PCM -o OUT.CIN\n"
        "  scdcpk info FILE.CIN\n"
        "\n"
        "video options:\n"
        "  --palettes N     palettes per frame, 1-4 (default 4)\n"
        "  --rate B         video bytes per second (default 120000)\n"
        "  --fps N or A/B   frame rate (default from input, else 15)\n"
        "  --lambda L       constant quality instead of rate control\n"
        "  --repal T        re-quantize palettes when error grows by factor T (default 1.5, 0 = every frame)\n"
        "  --dither M       off | fast | slow (default off)\n"
        "  --dither-pattern P  diag4 (default), horiz4, vert4, diag2, horiz2, vert2\n"
        "  --dither-weight W   dither strength 0.01-1.0 (default 0.5)\n"
        "  --quant-fraction F  fraction of pixels the palette quantizer samples per pass,\n"
        "                      0.01-10 (default 0.1; higher = slower, better palettes)\n"
        "  --iterations N   codebook refinement passes (default 4)\n"
        "  --seed N         palette quantizer seed (default 0)\n"
        "  --threads N      worker threads (default 0 = all CPUs; output is the same)\n"
        "  --frames N       stop after N frames\n"
        "  --raw WxH        INPUT is raw video of this size\n"
        "  --pixfmt F       raw format: yuv420p (default), yuv422p, yuv444p, gray8, bgr24, bgra\n"
        "  --full-range     YUV input is full range (default limited / TV range)\n"
        "  --preview PAT    write decoded frames as BMP, e.g. dec%%05d.bmp\n"
        "  -q               no progress output\n");
}

static int parse_size(const char *s, int *w, int *h)
{
    return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

static double parse_fps(const char *s)
{
    double a, b;
    if (sscanf(s, "%lf/%lf", &a, &b) == 2 && b > 0) return a / b;
    return atof(s);
}

static int parse_args(int argc, char **argv, options *o, const char **pos, int maxpos)
{
    int i, npos = 0;
    for (i = 0; i < argc; i++) {
        const char *a = argv[i], *n = i + 1 < argc ? argv[i + 1] : NULL;
#define NEXT() do { if (!n) { fprintf(stderr, "%s needs a value\n", a); return -1; } i++; } while (0)
        if (!strcmp(a, "-i")) { NEXT(); o->input = n; }
        else if (!strcmp(a, "-o")) { NEXT(); o->output = n; }
        else if (!strcmp(a, "--wav")) { NEXT(); o->wav = n; }
        else if (!strcmp(a, "--palettes")) { NEXT(); o->npal = atoi(n); }
        else if (!strcmp(a, "--rate")) { NEXT(); o->rate = atof(n); }
        else if (!strcmp(a, "--fps")) { NEXT(); o->fps = parse_fps(n); }
        else if (!strcmp(a, "--lambda")) { NEXT(); o->lambda = atof(n); }
        else if (!strcmp(a, "--repal")) { NEXT(); o->repal = atof(n); }
        else if (!strcmp(a, "--dither")) {
            NEXT();
            if (!strcmp(n, "off")) o->dither = 0;
            else if (!strcmp(n, "fast")) o->dither = 1;
            else if (!strcmp(n, "slow")) o->dither = 2;
            else { fprintf(stderr, "bad --dither %s\n", n); return -1; }
        }
        else if (!strcmp(a, "--dither-pattern") || !strcmp(a, "--pattern")) {
            static const char *names[] = { "diag4", "horiz4", "vert4", "diag2", "horiz2", "vert2" };
            int k;
            NEXT();
            for (k = 0; k < 6 && strcmp(n, names[k]); k++) {}
            if (k == 6 && n[0] >= '0' && n[0] <= '5' && !n[1]) k = n[0] - '0';
            if (k == 6) { fprintf(stderr, "bad %s %s (diag4 horiz4 vert4 diag2 horiz2 vert2)\n", a, n); return -1; }
            o->pattern = k;
        }
        else if (!strcmp(a, "--dither-weight")) {
            NEXT();
            o->dweight = atof(n);
            if (o->dweight < 0.01 || o->dweight > 1.0) { fprintf(stderr, "--dither-weight must be 0.01-1.0\n"); return -1; }
        }
        else if (!strcmp(a, "--quant-fraction")) {
            NEXT();
            o->fraction = atof(n);
            if (o->fraction < 0.01 || o->fraction > 10.0) { fprintf(stderr, "--quant-fraction must be 0.01-10\n"); return -1; }
        }
        else if (!strcmp(a, "--iterations")) { NEXT(); o->iters = atoi(n); }
        else if (!strcmp(a, "--frames")) { NEXT(); o->maxframes = atoi(n); }
        else if (!strcmp(a, "--seed")) { NEXT(); o->seed = (unsigned)strtoul(n, NULL, 0); }
        else if (!strcmp(a, "--threads")) { NEXT(); par_set_threads(atoi(n)); }
        else if (!strcmp(a, "--raw") || !strcmp(a, "--size")) {
            NEXT();
            if (!parse_size(n, &o->rawsize[0], &o->rawsize[1])) { fprintf(stderr, "bad size %s\n", n); return -1; }
        }
        else if (!strcmp(a, "--full-range")) o->fullrange = 1;
        else if (!strcmp(a, "--pixfmt")) { NEXT(); o->pixfmt = n; }
        else if (!strcmp(a, "--preview")) { NEXT(); o->preview = n; }
        else if (!strcmp(a, "--gain")) { NEXT(); o->aud.gain = atof(n); }
        else if (!strcmp(a, "--normalize")) o->aud.normalize = 1;
        else if (!strcmp(a, "-q")) o->quiet = 1;
        else if (a[0] == '-' && a[1]) { fprintf(stderr, "unknown option %s\n", a); return -1; }
        else if (npos < maxpos) pos[npos++] = a;
        else { fprintf(stderr, "unexpected argument %s\n", a); return -1; }
#undef NEXT
    }
    return npos;
}

static uint8_t *read_all(FILE *fp, size_t *len)
{
    size_t cap = 1 << 20, n = 0, got;
    uint8_t *buf = malloc(cap);
    while (buf && (got = fread(buf + n, 1, cap - n, fp)) > 0) {
        n += got;
        if (n == cap) buf = realloc(buf, cap *= 2);
    }
    *len = n;
    return buf;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *fp;
    uint8_t *b;
    if (!strcmp(path, "-")) return read_all(stdin, len);
    if (!(fp = fopen(path, "rb"))) return NULL;
    b = read_all(fp, len);
    fclose(fp);
    return b;
}

static double image_mse(const uint8_t *rgb, const uint8_t *idx, const uint8_t *pal, size_t n)
{
    double s = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        const uint8_t *c = pal + idx[i] * 3, *r = rgb + i * 3;
        int a = r[0] - c[0], b = r[1] - c[1], d = r[2] - c[2];
        s += a * a + b * b + d * d;
    }
    return s / (double)n;
}

static int convert_wav(const char *path, const audio_opts *ao, uint8_t **pcm, size_t *len)
{
    size_t wl;
    uint8_t *wav = read_file(path, &wl);
    int r;
    if (!wav) { fprintf(stderr, "cannot read %s\n", path); return -1; }
    r = audio_convert(wav, wl, ao, pcm, len);
    free(wav);
    return r;
}

/* 24-bit BMP: rows are stored bottom-up as BGR, padded to 4 bytes */
static int write_bmp(const char *name, const uint8_t *rgb, int w, int h)
{
    static const uint8_t pad[3];
    const uint32_t rowpad = (4 - (uint32_t)w * 3 % 4) % 4, stride = (uint32_t)w * 3 + rowpad;
    const uint32_t image = stride * h;
    const uint32_t fields[] = { 54 + image, 0, 54, 40, (uint32_t)w, (uint32_t)h };
    uint8_t hdr[54] = { 'B', 'M' }, *row;
    FILE *fp = fopen(name, "wb");
    int i, x, y, r = 0;

    if (!fp) return -1;
    for (i = 0; i < 6; i++) {
        hdr[2 + i * 4] = (uint8_t)fields[i];
        hdr[3 + i * 4] = (uint8_t)(fields[i] >> 8);
        hdr[4 + i * 4] = (uint8_t)(fields[i] >> 16);
        hdr[5 + i * 4] = (uint8_t)(fields[i] >> 24);
    }
    hdr[26] = 1;   /* planes */
    hdr[28] = 24;  /* bits per pixel */
    hdr[34] = (uint8_t)image;
    hdr[35] = (uint8_t)(image >> 8);
    hdr[36] = (uint8_t)(image >> 16);
    hdr[37] = (uint8_t)(image >> 24);
    if (fwrite(hdr, 1, sizeof hdr, fp) != sizeof hdr) r = -1;
    row = malloc((size_t)w * 3);
    for (y = h - 1; y >= 0 && !r; y--) {
        const uint8_t *s = rgb + (size_t)y * w * 3;
        for (x = 0; x < w; x++) {
            row[x * 3] = s[x * 3 + 2];
            row[x * 3 + 1] = s[x * 3 + 1];
            row[x * 3 + 2] = s[x * 3];
        }
        if (fwrite(row, 1, (size_t)w * 3, fp) != (size_t)w * 3 || fwrite(pad, 1, rowpad, fp) != rowpad) r = -1;
    }
    free(row);
    if (fclose(fp)) r = -1;
    return r;
}

/* encodes every frame of src into a silent CIN */
static int encode_video(vsrc *src, const options *o, cin_file *out)
{
    const int sw = vsrc_width(src), sh = vsrc_height(src);
    const int w = (sw + 7) & ~7, h = (sh + 7) & ~7;
    const int npal = o->npal < 1 ? 1 : o->npal > 4 ? 4 : o->npal;
    const int npal_out = npal < 2 ? 2 : npal;
    const double fps = o->fps > 0 ? o->fps : vsrc_fps(src) > 0 ? vsrc_fps(src) : 15.0;
    const double fdur = cin_frame_duration(fps), perframe = o->rate / fps;
    const double cap = o->rate;
    double bucket = o->rate * 0.5, lambda = o->lambda > 0 ? o->lambda : 64.0;
    double imgtime = 0, keymse = 0, total = 0, mse_sum = 0;
    uint8_t *frame, *rgb, *ref, *idx, *pal, *buf, *dec = NULL;
    vq_enc *vq;
    tpq_opts tq;
    int n = 0, r = 0, y;

    if (w / 8 > MAX_TW || h / 8 > MAX_TH) {
        fprintf(stderr, "video is %dx%d; the player supports up to %dx%d\n", sw, sh, MAX_TW * 8, MAX_TH * 8);
        return -1;
    }
    if (!o->quiet && (w / 8) * (h / 8) > STOCK_MAX_TILES)
        fprintf(stderr, "note: %d tiles per frame needs a player with tiles at VRAM $0000 (Sega's demo player max %d)\n",
                (w / 8) * (h / 8), STOCK_MAX_TILES);

    memset(&tq, 0, sizeof tq);
    tq.npal = npal;
    tq.dither = o->dither;
    tq.pattern = o->pattern;
    tq.weight = (float)o->dweight;
    tq.fraction = (float)o->fraction;
    tq.seed = o->seed;

    frame = malloc((size_t)sw * sh * 3);
    rgb = calloc((size_t)w * h, 3);
    ref = malloc((size_t)w * h * 3);
    idx = malloc((size_t)w * h);
    pal = calloc(4 * 16, 3);
    vq = vq_create(w, h, npal_out, o->iters);
    buf = malloc(vq_max_size(vq));
    if (o->preview) dec = malloc((size_t)w * h * 3);
    cin_init(out, w, h);

    while ((o->maxframes <= 0 || n < o->maxframes) && (r = vsrc_read(src, frame)) == 1) {
        vq_stats st;
        size_t size = 0;
        double target = perframe, mse;
        int pass, requant = 0;

        for (y = 0; y < sh; y++)
            memcpy(rgb + (size_t)y * w * 3, frame + (size_t)y * sw * 3, (size_t)sw * 3);

        if (n == 0 || o->repal <= 0) {
            requant = 1;
        } else {
            tpq_remap(&tq, rgb, w, h, pal, idx);
            mse = image_mse(rgb, idx, pal, (size_t)w * h);
            if (mse > keymse * o->repal + 8.0) requant = 1;
        }
        if (requant) {
            memset(pal, 0, 4 * 16 * 3);
            if (tpq_quantize(&tq, rgb, w, h, pal, idx)) { fprintf(stderr, "palette quantization failed\n"); return -1; }
            keymse = image_mse(rgb, idx, pal, (size_t)w * h);
        }
        if (o->dither) {
            size_t i;
            for (i = 0; i < (size_t)w * h; i++) memcpy(ref + i * 3, pal + idx[i] * 3, 3);
        } else {
            memcpy(ref, rgb, (size_t)w * h * 3);
        }

        bucket += perframe;
        if (bucket > cap) bucket = cap;
        if (o->lambda <= 0) {
            target = perframe + 0.3 * (bucket - perframe);
            if (target < 0.2 * perframe) target = 0.2 * perframe;
            if (target > bucket) target = bucket;
        }
        for (pass = 0; pass < 10; pass++) {
            double ratio;
            size = vq_encode(vq, ref, idx, pal, lambda, buf, &st) + 16;
            if (o->lambda > 0) break;
            ratio = size / target;
            if (size > bucket && pass < 9) { lambda *= 4; continue; }
            if (pass >= 2 || (ratio > 0.7 && ratio < 1.15)) break;
            if (ratio < 1 && (st.skip == (w / 8) * (h / 8) || lambda <= 0.01)) break;
            lambda *= pow(ratio, 1.5) < 0.25 ? 0.25 : pow(ratio, 1.5) > 4 ? 4 : pow(ratio, 1.5);
            if (lambda < 0.01) lambda = 0.01;
        }
        vq_commit(vq);
        if (o->lambda <= 0) {
            double ratio = size / target, f = pow(ratio, 0.7);
            bucket -= size;
            lambda *= f < 0.5 ? 0.5 : f > 2 ? 2 : f;
            if (lambda < 0.01) lambda = 0.01;
        }
        {
            uint32_t t0 = (uint32_t)imgtime, t1 = (uint32_t)(imgtime + fdur);
            cin_add(out, buf, size - 16, t0, t1 - t0);
            imgtime += fdur;
        }
        if (dec) {
            char name[4096];
            vq_decoded_rgb(vq, dec);
            snprintf(name, sizeof name, o->preview, n);
            if (write_bmp(name, dec, w, h)) fprintf(stderr, "cannot write %s\n", name);
        }
        total += size;
        mse_sum += st.mse;
        n++;
        if (!o->quiet)
            fprintf(stderr, "frame %5d: %6zu B  skip %3d raw %3d c32 %3d c16 %3d  k %3d/%3d  mse %7.1f  lambda %8.2f%s\n",
                    n - 1, size, st.skip, st.raw, st.cb32, st.cb16, st.k32, st.k16, st.mse, lambda,
                    requant ? "  palette" : "");
    }
    if (!o->quiet && n)
        fprintf(stderr, "%d frames, %.0f video bytes/s, mean mse %.1f\n", n, total / n * fps, mse_sum / n);
    free(frame); free(rgb); free(ref); free(idx); free(pal); free(buf); free(dec);
    vq_destroy(vq);
    return r < 0 ? -1 : n;
}

static void set_binary_stdio(void)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

static int finish(cin_file *video, const options *o, const uint8_t *pcm, size_t pcmlen)
{
    cin_file final;
    int r;
    cin_mux(&final, video, pcm, pcmlen);
    r = cin_write(&final, o->output);
    if (r) fprintf(stderr, "cannot write %s\n", o->output);
    else if (!o->quiet) fprintf(stderr, "%s: %d entries, %zu bytes\n", o->output, final.nents, final.len);
    cin_free(&final);
    return r ? 1 : 0;
}

static int cmd_encode(int argc, char **argv, options *o, int rawstdin)
{
    const char *pos[1];
    vsrc *src;
    FILE *fp = NULL;
    cin_file video;
    uint8_t *pcm = NULL;
    size_t pcmlen = 0;
    int r;

    if (parse_args(argc, argv, o, pos, 0) < 0) return 2;
    if (!o->output || (!rawstdin && !o->input)) { usage(); return 2; }
    if (rawstdin) {
        if (!o->rawsize[0]) { fprintf(stderr, "video: --size WxH required\n"); return 2; }
        src = vsrc_open_raw(stdin, o->rawsize[0], o->rawsize[1], o->pixfmt, o->fullrange);
    } else {
        fp = strcmp(o->input, "-") ? fopen(o->input, "rb") : stdin;
        if (!fp) { fprintf(stderr, "cannot read %s\n", o->input); return 1; }
        src = o->rawsize[0] ? vsrc_open_raw(fp, o->rawsize[0], o->rawsize[1], o->pixfmt, o->fullrange) : vsrc_open_y4m(fp);
    }
    if (!src) return 1;
    if (o->wav && convert_wav(o->wav, &o->aud, &pcm, &pcmlen)) return 1;
    r = encode_video(src, o, &video);
    vsrc_close(src);
    if (fp && fp != stdin) fclose(fp);
    if (r <= 0) { fprintf(stderr, "no frames encoded\n"); return 1; }
    r = finish(&video, o, pcm, pcmlen);
    cin_free(&video);
    free(pcm);
    return r;
}

static int cmd_audio(int argc, char **argv, options *o)
{
    const char *pos[1];
    uint8_t *pcm;
    size_t len;
    FILE *fp;
    if (parse_args(argc, argv, o, pos, 0) < 0 || !o->output) { usage(); return 2; }
    if (convert_wav(o->input ? o->input : "-", &o->aud, &pcm, &len)) return 1;
    if (!(fp = fopen(o->output, "wb")) || fwrite(pcm, 1, len, fp) != len) {
        fprintf(stderr, "cannot write %s\n", o->output);
        return 1;
    }
    fclose(fp);
    if (!o->quiet) fprintf(stderr, "%s: %zu samples (%.2f s)\n", o->output, len - 2, (len - 2) / CIN_PCM_RATE);
    free(pcm);
    return 0;
}

static int cmd_mux(int argc, char **argv, options *o)
{
    const char *pos[2];
    cin_file video;
    uint8_t *pcm = NULL;
    size_t len = 0;
    int r, n = parse_args(argc, argv, o, pos, 2);
    if (n < 1 || !o->output) { usage(); return 2; }
    if (cin_read(&video, pos[0])) { fprintf(stderr, "cannot read %s\n", pos[0]); return 1; }
    if (n > 1) {
        FILE *fp = fopen(pos[1], "rb");
        /* VirtualDub skips the audio encoder for sources without audio */
        if (!fp) {
            fprintf(stderr, "no audio file %s, writing a silent movie\n", pos[1]);
        } else {
            pcm = read_all(fp, &len);
            fclose(fp);
        }
    }
    r = finish(&video, o, pcm, len);
    cin_free(&video);
    free(pcm);
    return r;
}

static int cmd_info(int argc, char **argv, options *o)
{
    const char *pos[1];
    cin_file c;
    int i, nv = 0, na = 0;
    size_t vb = 0, ab = 0, maxv = 0;
    uint32_t end = 0;
    if (parse_args(argc, argv, o, pos, 1) != 1) { usage(); return 2; }
    if (cin_read(&c, pos[0])) { fprintf(stderr, "cannot read %s\n", pos[0]); return 1; }
    for (i = 0; i < c.nents; i++) {
        const cin_entry *e = &c.ents[i];
        if (e->time == CIN_AUDIO_TIME) { na++; ab += e->size; continue; }
        nv++;
        vb += e->size;
        if (e->size > maxv) maxv = e->size;
        if (e->time + e->dur > end) end = e->time + e->dur;
    }
    printf("%s: %dx%d, %d video frames (%zu bytes, max %zu), %d audio chunks (%zu bytes)\n",
           pos[0], c.width, c.height, nv, vb, maxv, na, ab);
    if (end) {
        double secs = end / (double)CIN_BASE_RATE * CIN_PCM_NOMINAL / CIN_PCM_RATE;
        printf("duration %.2f s, %.2f fps, video %.0f B/s, audio %.0f B/s\n",
               secs, nv / secs, vb / secs, ab / secs);
    }
    cin_free(&c);
    return 0;
}

int main(int argc, char **argv)
{
    options o;
    memset(&o, 0, sizeof o);
    o.npal = 4;
    o.rate = 120000;
    o.repal = 1.5;
    o.iters = 4;
    o.aud.gain = 1.0;

    if (argc < 2) { usage(); return 2; }
    set_binary_stdio();
    if (!strcmp(argv[1], "encode")) return cmd_encode(argc - 2, argv + 2, &o, 0);
    if (!strcmp(argv[1], "video")) return cmd_encode(argc - 2, argv + 2, &o, 1);
    if (!strcmp(argv[1], "audio")) return cmd_audio(argc - 2, argv + 2, &o);
    if (!strcmp(argv[1], "mux")) return cmd_mux(argc - 2, argv + 2, &o);
    if (!strcmp(argv[1], "info")) return cmd_info(argc - 2, argv + 2, &o);
    usage();
    return 2;
}
