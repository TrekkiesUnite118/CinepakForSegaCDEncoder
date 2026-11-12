#ifndef PAR_H
#define PAR_H

#ifdef __cplusplus
extern "C" {
#endif

/* 0 = one thread per logical CPU */
void par_set_threads(int n);
int  par_threads(void);

/* calls fn(ctx, begin, end) over [0, n) in chunks of `chunk`, possibly concurrently */
void par_for(int n, int chunk, void (*fn)(void *ctx, int begin, int end), void *ctx);

#ifdef __cplusplus
}
#endif

#endif
