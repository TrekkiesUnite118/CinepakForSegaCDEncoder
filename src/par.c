#include "par.h"

#ifdef _WIN32
#include <windows.h>

static int nthreads;

void par_set_threads(int n)
{
    SYSTEM_INFO si;
    if (n <= 0) {
        GetSystemInfo(&si);
        n = (int)si.dwNumberOfProcessors;
    }
    nthreads = n < 1 ? 1 : n > 64 ? 64 : n;
}

int par_threads(void)
{
    if (!nthreads) par_set_threads(0);
    return nthreads;
}

typedef struct {
    void (*fn)(void *, int, int);
    void         *ctx;
    int           n, chunk;
    volatile LONG next;
} job;

static void run(job *j)
{
    for (;;) {
        int b = (int)InterlockedExchangeAdd(&j->next, j->chunk);
        if (b >= j->n) break;
        j->fn(j->ctx, b, b + j->chunk < j->n ? b + j->chunk : j->n);
    }
}

static VOID CALLBACK worker(PTP_CALLBACK_INSTANCE inst, PVOID ctx, PTP_WORK work)
{
    (void)inst; (void)work;
    run((job *)ctx);
}

void par_for(int n, int chunk, void (*fn)(void *ctx, int begin, int end), void *ctx)
{
    job j;
    int nchunks, helpers, i;
    PTP_WORK work;

    if (n <= 0) return;
    if (chunk < 1) chunk = 1;
    nchunks = (n + chunk - 1) / chunk;
    helpers = (par_threads() < nchunks ? par_threads() : nchunks) - 1;
    if (helpers <= 0 || !(work = CreateThreadpoolWork(worker, &j, NULL))) {
        fn(ctx, 0, n);
        return;
    }
    j.fn = fn;
    j.ctx = ctx;
    j.n = n;
    j.chunk = chunk;
    j.next = 0;
    for (i = 0; i < helpers; i++) SubmitThreadpoolWork(work);
    run(&j);
    WaitForThreadpoolWorkCallbacks(work, FALSE);
    CloseThreadpoolWork(work);
}

#else

void par_set_threads(int n) { (void)n; }
int  par_threads(void) { return 1; }

void par_for(int n, int chunk, void (*fn)(void *ctx, int begin, int end), void *ctx)
{
    (void)chunk;
    if (n > 0) fn(ctx, 0, n);
}

#endif
