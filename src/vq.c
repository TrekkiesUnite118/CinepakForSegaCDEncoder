#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vq.h"
#include "par.h"

#define MAXK      256
#define INF       0xFFFFFFFFu
#define BITS_SKIP 2
#define BITS_RAW  (2 + 256)
#define BITS_C32  (2 + 64)
#define BITS_C16  (2 + 128)

enum { M_SKIP, M_RAW, M_C32, M_C16 };

typedef struct {
    int       dim;           /* pixels per block: 8 (4x2) or 4 (2x2) */
    int       per_tile;      /* blocks per tile: 8 or 16 */
    int       entry_bits;
    int       off[16][8];    /* pixel offset of block k, position j, from tile origin */
    int       k;
    uint8_t   cb[MAXK][8];
    uint32_t *e;             /* [block][dim][16] distortion per candidate color */
    uint8_t  *best;          /* per block */
    uint32_t *bestd, *secd;  /* per block */
} book;

struct vq_enc {
    int       w, h, tw, th, nt, npal, iters;
    uint32_t *err;           /* [pixel][16] */
    uint8_t  *tgt;           /* target 4-bit index per pixel */
    uint8_t  *tpal;          /* palette per tile */
    uint8_t  *mode;
    uint32_t *dskip, *draw;
    int      *active;        /* tiles needing a decision */
    int       nactive;
    uint8_t  *bank[2];       /* decoded 4-bit indices in each 1M word RAM bank */
    int       valid[2];
    int       frame;
    uint8_t  *trial;
    uint8_t   pal[4 * 16 * 3];
    book      b32, b16;
};

static void *xcalloc(size_t n, size_t s)
{
    void *p = calloc(n ? n : 1, s);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(1); }
    return p;
}

static void book_init(book *b, int dim, int w, int nt)
{
    int k, j;
    b->dim = dim;
    b->per_tile = dim == 8 ? 8 : 16;
    b->entry_bits = dim * 4;
    for (k = 0; k < b->per_tile; k++) {
        for (j = 0; j < dim; j++) {
            int x, y;
            if (dim == 8) {
                /* 4x2: k = row pair * 2 + side */
                x = (k & 1) * 4 + (j & 3);
                y = (k >> 1) * 2 + (j >> 2);
            } else {
                /* 2x2: k = row pair * 4 + column */
                x = (k & 3) * 2 + (j & 1);
                y = (k >> 2) * 2 + (j >> 1);
            }
            b->off[k][j] = y * w + x;
        }
    }
    b->e = xcalloc((size_t)nt * b->per_tile * dim * 16, sizeof *b->e);
    b->best = xcalloc((size_t)nt * b->per_tile, 1);
    b->bestd = xcalloc((size_t)nt * b->per_tile, sizeof *b->bestd);
    b->secd = xcalloc((size_t)nt * b->per_tile, sizeof *b->secd);
}

vq_enc *vq_create(int w, int h, int npal, int iterations)
{
    vq_enc *v = xcalloc(1, sizeof *v);
    v->w = w;
    v->h = h;
    v->tw = w / 8;
    v->th = h / 8;
    v->nt = v->tw * v->th;
    v->npal = npal;
    v->iters = iterations < 1 ? 1 : iterations;
    v->err = xcalloc((size_t)w * h * 16, sizeof *v->err);
    v->tgt = xcalloc((size_t)w * h, 1);
    v->tpal = xcalloc(v->nt, 1);
    v->mode = xcalloc(v->nt, 1);
    v->dskip = xcalloc(v->nt, sizeof *v->dskip);
    v->draw = xcalloc(v->nt, sizeof *v->draw);
    v->active = xcalloc(v->nt, sizeof *v->active);
    v->bank[0] = xcalloc((size_t)w * h, 1);
    v->bank[1] = xcalloc((size_t)w * h, 1);
    v->trial = xcalloc((size_t)w * h, 1);
    book_init(&v->b32, 8, w, v->nt);
    book_init(&v->b16, 4, w, v->nt);
    return v;
}

static void book_free(book *b)
{
    free(b->e); free(b->best); free(b->bestd); free(b->secd);
}

void vq_destroy(vq_enc *v)
{
    if (!v) return;
    free(v->err); free(v->tgt); free(v->tpal); free(v->mode);
    free(v->dskip); free(v->draw); free(v->active);
    free(v->bank[0]); free(v->bank[1]); free(v->trial);
    book_free(&v->b32);
    book_free(&v->b16);
    free(v);
}

static size_t map_bytes(const vq_enc *v) { return 4 * (size_t)((v->nt + 15) / 16); }

size_t vq_max_size(const vq_enc *v)
{
    return 12 + 32 * (size_t)v->npal + 2 * map_bytes(v) + 8 + 4 * MAXK + 2 * MAXK + 32 * (size_t)v->nt;
}

static size_t tile_origin(const vq_enc *v, int t)
{
    return (size_t)(t / v->tw) * 8 * v->w + (size_t)(t % v->tw) * 8;
}

/* gather per-candidate distortion for every block of the active tiles */
static void book_gather(vq_enc *v, book *b)
{
    int a, k, j;
    for (a = 0; a < v->nactive; a++) {
        int t = v->active[a];
        size_t o = tile_origin(v, t);
        for (k = 0; k < b->per_tile; k++) {
            uint32_t *e = b->e + ((size_t)t * b->per_tile + k) * b->dim * 16;
            for (j = 0; j < b->dim; j++)
                memcpy(e + j * 16, v->err + (o + b->off[k][j]) * 16, 16 * sizeof *e);
        }
    }
}

static int cmp_count(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* seed with the most frequent exact target patterns */
static void book_seed(vq_enc *v, book *b)
{
    size_t nb = (size_t)v->nactive * b->per_tile, cap = 1, i, n = 0;
    uint32_t *keys;
    uint32_t *cnt;
    uint64_t *list;
    int a, k, j;

    while (cap < nb * 2) cap <<= 1;
    keys = xcalloc(cap, sizeof *keys);
    cnt = xcalloc(cap, sizeof *cnt);
    for (a = 0; a < v->nactive; a++) {
        int t = v->active[a];
        size_t o = tile_origin(v, t);
        for (k = 0; k < b->per_tile; k++) {
            uint32_t key = 0, hsh;
            for (j = 0; j < b->dim; j++)
                key = (key << 4) | v->tgt[o + b->off[k][j]];
            hsh = (key * 2654435761u) & (uint32_t)(cap - 1);
            while (cnt[hsh] && keys[hsh] != key) hsh = (hsh + 1) & (uint32_t)(cap - 1);
            keys[hsh] = key;
            cnt[hsh]++;
        }
    }
    list = xcalloc(cap, sizeof *list);
    for (i = 0; i < cap; i++)
        if (cnt[i]) list[n++] = ((uint64_t)cnt[i] << 32) | keys[i];
    qsort(list, n, sizeof *list, cmp_count);
    b->k = n < MAXK ? (int)n : MAXK;
    for (k = 0; k < b->k; k++) {
        uint32_t key = (uint32_t)list[k];
        for (j = b->dim - 1; j >= 0; j--, key >>= 4)
            b->cb[k][j] = (uint8_t)(key & 15);
    }
    free(keys); free(cnt); free(list);
}

typedef struct { vq_enc *v; book *b; } assign_job;

#ifdef _MSC_VER
#define FORCEINLINE __forceinline
#else
#define FORCEINLINE inline __attribute__((always_inline))
#endif

/* dim is a constant at each call site so the inner loop unrolls */
static FORCEINLINE void assign_blocks(const vq_enc *v, book *b, int begin, int end, const int dim)
{
    const int nk = b->k;
    int a, k, e, j;
    for (a = begin; a < end; a++) {
        int t = v->active[a];
        for (k = 0; k < b->per_tile; k++) {
            size_t bi = (size_t)t * b->per_tile + k;
            const uint32_t *E = b->e + bi * dim * 16;
            uint32_t bd = INF, sd = INF;
            int be = 0;
            for (e = 0; e < nk; e++) {
                const uint8_t *c = b->cb[e];
                uint32_t d = 0;
                for (j = 0; j < dim; j++) {
                    d += E[j * 16 + c[j]];
                    if (d >= sd) break;
                }
                if (d < bd) { sd = bd; bd = d; be = e; }
                else if (d < sd) sd = d;
            }
            b->best[bi] = (uint8_t)be;
            b->bestd[bi] = bd;
            b->secd[bi] = sd;
        }
    }
}

static void assign_range(void *ctx, int begin, int end)
{
    const assign_job *j = ctx;
    if (j->b->dim == 8) assign_blocks(j->v, j->b, begin, end, 8);
    else assign_blocks(j->v, j->b, begin, end, 4);
}

static void book_assign(vq_enc *v, book *b)
{
    assign_job j;
    j.v = v;
    j.b = b;
    par_for(v->nactive, 8, assign_range, &j);
}

static uint64_t tile_cost(const book *b, int t)
{
    uint64_t s = 0;
    int k;
    if (b->k == 0) return (uint64_t)-1;
    for (k = 0; k < b->per_tile; k++) s += b->bestd[(size_t)t * b->per_tile + k];
    return s;
}

static void decide(vq_enc *v, double lambda)
{
    int a;
    for (a = 0; a < v->nactive; a++) {
        int t = v->active[a], m = M_RAW;
        double jbest = v->draw[t] + lambda * BITS_RAW, j;
        uint64_t d;
        if (v->valid[v->frame & 1]) {
            j = v->dskip[t] + lambda * BITS_SKIP;
            if (j < jbest) { jbest = j; m = M_SKIP; }
        }
        if ((d = tile_cost(&v->b32, t)) != (uint64_t)-1) {
            j = (double)d + lambda * BITS_C32;
            if (j < jbest) { jbest = j; m = M_C32; }
        }
        if ((d = tile_cost(&v->b16, t)) != (uint64_t)-1) {
            j = (double)d + lambda * BITS_C16;
            if (j < jbest) { jbest = j; m = M_C16; }
        }
        v->mode[t] = (uint8_t)m;
    }
}

/* drop entries whose removal costs less distortion than their own bits;
   returns old->new index map in remap (0xFF = removed) */
static void book_prune(vq_enc *v, book *b, int m, double lambda, int keep_unused, uint8_t *remap)
{
    double gain[MAXK] = {0};
    int used[MAXK] = {0};
    int a, k, e, n = 0;

    memset(remap, 0xFF, MAXK);
    for (a = 0; a < v->nactive; a++) {
        int t = v->active[a];
        if (v->mode[t] != m) continue;
        for (k = 0; k < b->per_tile; k++) {
            size_t bi = (size_t)t * b->per_tile + k;
            e = b->best[bi];
            used[e]++;
            gain[e] += b->secd[bi] == INF ? 1e30 : (double)(b->secd[bi] - b->bestd[bi]);
        }
    }
    for (e = 0; e < b->k; e++) {
        int keep = used[e] ? gain[e] >= lambda * b->entry_bits : keep_unused;
        if (keep) {
            if (n != e) memcpy(b->cb[n], b->cb[e], 8);
            remap[e] = (uint8_t)n++;
        } else {
            remap[e] = 0xFF;
        }
    }
    b->k = n;
}

static void book_update(vq_enc *v, book *b, int m)
{
    static uint64_t acc[MAXK][8][16];
    int used[MAXK] = {0};
    int a, k, e, j, c;

    memset(acc, 0, sizeof acc);
    for (a = 0; a < v->nactive; a++) {
        int t = v->active[a];
        if (v->mode[t] != m) continue;
        for (k = 0; k < b->per_tile; k++) {
            size_t bi = (size_t)t * b->per_tile + k;
            const uint32_t *E = b->e + bi * b->dim * 16;
            e = b->best[bi];
            used[e] = 1;
            for (j = 0; j < b->dim; j++)
                for (c = 0; c < 16; c++)
                    acc[e][j][c] += E[j * 16 + c];
        }
    }
    for (e = 0; e < b->k; e++) {
        if (!used[e]) continue;
        for (j = 0; j < b->dim; j++) {
            int bc = 0;
            for (c = 1; c < 16; c++)
                if (acc[e][j][c] < acc[e][j][bc]) bc = c;
            b->cb[e][j] = (uint8_t)bc;
        }
    }
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v & 0xFFFF); }

static unsigned vdp_level(uint8_t c) { return ((unsigned)c * 7 + 127) / 255; }

size_t vq_encode(vq_enc *v, const uint8_t *ref, const uint8_t *idx, const uint8_t *pal,
                 double lambda, uint8_t *out, vq_stats *st)
{
    const int canskip = v->valid[v->frame & 1];
    const uint8_t *prev = v->bank[v->frame & 1];
    uint8_t remap[MAXK];
    size_t p, mb = map_bytes(v), pix;
    double dtotal = 0;
    int t, i, it, x, y;

    memcpy(v->pal, pal, (size_t)v->npal * 48);
    for (i = 0; i < v->npal; i++)
        v->pal[i * 48] = v->pal[i * 48 + 1] = v->pal[i * 48 + 2] = 0;

    /* per-pixel distortion of each of the tile palette's 16 colors */
    for (t = 0; t < v->nt; t++) {
        size_t o = tile_origin(v, t);
        int tp = idx[o] >> 4;
        uint32_t ds = 0, dr = 0;
        if (tp >= v->npal) tp = 0;
        v->tpal[t] = (uint8_t)tp;
        for (y = 0; y < 8; y++) {
            for (x = 0; x < 8; x++) {
                const uint8_t *r, *pc = v->pal + tp * 48;
                uint32_t *e;
                int c;
                pix = o + (size_t)y * v->w + x;
                r = ref + pix * 3;
                e = v->err + pix * 16;
                for (c = 0; c < 16; c++, pc += 3) {
                    int dr0 = r[0] - pc[0], dg = r[1] - pc[1], db = r[2] - pc[2];
                    e[c] = (uint32_t)(dr0 * dr0 + dg * dg + db * db);
                }
                v->tgt[pix] = idx[pix] & 15;
                dr += e[v->tgt[pix]];
                if (canskip) ds += e[prev[pix]];
            }
        }
        v->draw[t] = dr;
        v->dskip[t] = ds;
    }

    v->nactive = 0;
    for (t = 0; t < v->nt; t++) {
        if (canskip && v->dskip[t] == 0) v->mode[t] = M_SKIP;
        else v->active[v->nactive++] = t;
    }

    book_gather(v, &v->b32);
    book_gather(v, &v->b16);
    book_seed(v, &v->b32);
    book_seed(v, &v->b16);
    for (it = 0; it < v->iters; it++) {
        book_assign(v, &v->b32);
        book_assign(v, &v->b16);
        decide(v, lambda);
        if (it > 0) {
            book_prune(v, &v->b32, M_C32, lambda, 0, remap);
            book_prune(v, &v->b16, M_C16, lambda, 0, remap);
            book_assign(v, &v->b32);
            book_assign(v, &v->b16);
            decide(v, lambda);
        }
        book_update(v, &v->b32, M_C32);
        book_update(v, &v->b16, M_C16);
    }
    book_assign(v, &v->b32);
    book_assign(v, &v->b16);
    decide(v, lambda);
    /* drop entries no tile uses; the rest keep their nearest assignment */
    book_prune(v, &v->b32, M_C32, 0, 0, remap);
    for (t = 0; t < v->nt * 8; t++) v->b32.best[t] = remap[v->b32.best[t]];
    book_prune(v, &v->b16, M_C16, 0, 0, remap);
    for (t = 0; t < v->nt * 16; t++) v->b16.best[t] = remap[v->b16.best[t]];

    /* header */
    memset(st, 0, sizeof *st);
    out[0] = 'S'; out[1] = 'M';
    put16(out + 2, 0x20 | (v->npal - 1));
    put16(out + 8, (unsigned)v->tw);
    put16(out + 10, (unsigned)v->th);
    p = 12;
    for (i = 0; i < v->npal * 16; i++, p += 2) {
        const uint8_t *c = v->pal + i * 3;
        put16(out + p, (vdp_level(c[2]) << 9) | (vdp_level(c[1]) << 5) | (vdp_level(c[0]) << 1));
    }
    memset(out + p, 0, mb);
    for (t = 0; t < v->nt; t++)
        out[p + t / 4] |= (uint8_t)(v->tpal[t] << (6 - 2 * (t & 3)));
    p += mb;

    put32(out + p, 4u * v->b32.k); p += 4;
    for (i = 0; i < v->b32.k; i++, p += 4) {
        const uint8_t *c = v->b32.cb[i];
        out[p] = (uint8_t)(c[0] << 4 | c[1]); out[p + 1] = (uint8_t)(c[2] << 4 | c[3]);
        out[p + 2] = (uint8_t)(c[4] << 4 | c[5]); out[p + 3] = (uint8_t)(c[6] << 4 | c[7]);
    }
    put32(out + p, 2u * v->b16.k); p += 4;
    for (i = 0; i < v->b16.k; i++, p += 2) {
        const uint8_t *c = v->b16.cb[i];
        out[p] = (uint8_t)(c[0] << 4 | c[1]);
        out[p + 1] = (uint8_t)(c[2] << 4 | c[3]);
    }

    /* flags: 00 skip, 01 raw, 10 cb32, 11 cb16, MSB first */
    memset(out + p, 0, mb);
    for (t = 0; t < v->nt; t++)
        out[p + t / 4] |= (uint8_t)(v->mode[t] << (6 - 2 * (t & 3)));
    p += mb;

    for (t = 0; t < v->nt; t++) {
        size_t o = tile_origin(v, t);
        const uint8_t *src;
        int k, j;
        switch (v->mode[t]) {
        case M_SKIP:
            st->skip++;
            for (y = 0; y < 8; y++)
                memcpy(v->trial + o + (size_t)y * v->w, prev + o + (size_t)y * v->w, 8);
            break;
        case M_RAW:
            st->raw++;
            for (y = 0; y < 8; y++) {
                src = v->tgt + o + (size_t)y * v->w;
                memcpy(v->trial + o + (size_t)y * v->w, src, 8);
                for (x = 0; x < 8; x += 2) out[p++] = (uint8_t)(src[x] << 4 | src[x + 1]);
            }
            break;
        case M_C32:
            st->cb32++;
            for (k = 0; k < 8; k++) {
                int e = v->b32.best[(size_t)t * 8 + k];
                out[p++] = (uint8_t)e;
                for (j = 0; j < 8; j++) v->trial[o + v->b32.off[k][j]] = v->b32.cb[e][j];
            }
            break;
        default:
            st->cb16++;
            for (k = 0; k < 16; k++) {
                int e = v->b16.best[(size_t)t * 16 + k];
                out[p++] = (uint8_t)e;
                for (j = 0; j < 4; j++) v->trial[o + v->b16.off[k][j]] = v->b16.cb[e][j];
            }
            break;
        }
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                pix = o + (size_t)y * v->w + x;
                dtotal += v->err[pix * 16 + v->trial[pix]];
            }
    }
    put32(out + 4, (uint32_t)p);
    st->k32 = v->b32.k;
    st->k16 = v->b16.k;
    st->mse = dtotal / ((double)v->w * v->h);
    return p;
}

void vq_commit(vq_enc *v)
{
    int b = v->frame & 1;
    memcpy(v->bank[b], v->trial, (size_t)v->w * v->h);
    v->valid[b] = 1;
    v->frame++;
}

void vq_decoded_rgb(const vq_enc *v, uint8_t *rgb)
{
    int t, x, y;
    for (t = 0; t < v->nt; t++) {
        size_t o = tile_origin(v, t);
        const uint8_t *pc = v->pal + v->tpal[t] * 48;
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++) {
                size_t pix = o + (size_t)y * v->w + x;
                memcpy(rgb + pix * 3, pc + v->trial[pix] * 3, 3);
            }
    }
}
