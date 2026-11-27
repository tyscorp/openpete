// core/runtime/work_pool.c - persistent data-parallel worker pool.
//
// See openpete_workpool.h for the contracts. Shape: N-1 persistent SDL
// threads park on a start semaphore; a run publishes the job, wakes them,
// and the calling thread works the same item stream as worker 0. Items are
// pulled from one atomic counter (dynamic load balance - sector costs vary
// by 100x, a static split would leave workers idle behind one heavy chunk).
// The done semaphore both joins the run and publishes the workers' writes
// to the caller.

#include "openpete_workpool.h"
#include "openpete_crash.h"
#include "openpete_profile.h"       /* the join's wait region */

#include <SDL3/SDL.h>
#include "openpete_thread_roles.h"

#define WP_MAX_THREADS 8

/* The posted run. Every worker increments next once per item and reads the
 * rest once per item, so the job sits alone on one cache line: a variable
 * of another file placed beside it would be invalidated per item. */
typedef struct {
    _Alignas(64) openpete_work_fn fn;
    void*            arg;
    int              nitems;
    SDL_AtomicInt    next;      /* next item to pull */
} wp_job;

static SDL_Semaphore* s_start[WP_MAX_THREADS];  /* one per helper: wake for a run; worker-line: written at spawn, read once per run */
static SDL_Semaphore* s_done;                    /* helpers post once per run */
static SDL_Thread*    s_thread[WP_MAX_THREADS];  /* worker-line: written at spawn only */
static wp_job         s_job;
static int            s_threads   = 0;   /* resolved count, 0 = not yet resolved */
static int            s_requested = 0;   /* configure() value, 0 = auto */
static int            s_spawned   = 0;   /* helper threads actually created */

/** @brief Pull items from the shared counter and run the job function on each until none remain.
 *
 * Each pull is one atomic increment of the job's next counter, so items are
 * distributed dynamically; a worker that finds the counter at or past the
 * item count returns. The counter keeps incrementing past the count, once
 * per caller of this function per run. */
static void wp_work(int worker) {
    for (;;) {
        int item = SDL_AddAtomicInt(&s_job.next, 1);
        if (item >= s_job.nitems) return;
        s_job.fn(item, worker, s_job.arg);
    }
}

/** @brief Body of one work-pool helper thread: run its share of each posted job, forever.
 *
 * The loop has no exit; helpers are never joined and end with the process. */
static int SDLCALL wp_thread_main(void* p) {
    OP_ROLE_BIND(workpool);
    int worker = (int)(intptr_t)p;          /* 1..threads-1; 0 is the caller */
    openpete_crash_thread_attach();
    for (;;) {
        SDL_WaitSemaphore(s_start[worker]);
        wp_work(worker);
        SDL_SignalSemaphore(s_done);
    }
    return 0;
}

void openpete_workpool_configure(int threads) {
    s_requested = threads;
}

int openpete_workpool_threads(void) {
    if (s_threads > 0) return s_threads;
    int n = s_requested;
    if (n <= 0) {
        n = SDL_GetNumLogicalCPUCores() / 2;
        if (n < 1) n = 1;
    }
    if (n > WP_MAX_THREADS) n = WP_MAX_THREADS;
    s_threads = n;
    return s_threads;
}

/** @brief Create the done semaphore and the helper threads on the first parallel run.
 *
 * Does nothing when the pool was already spawned or resolves to one thread.
 * Helper w gets its own start semaphore and a thread named
 * "openpete-extractN". When a thread fails to create, its semaphore is
 * destroyed and the resolved count drops to w, so the pool runs with the
 * helpers created so far; the pool counts as spawned either way. */
static void wp_spawn(void) {
    int n = openpete_workpool_threads();
    if (s_spawned || n <= 1) return;
    s_done = SDL_CreateSemaphore(0);
    for (int w = 1; w < n; w++) {
        s_start[w] = SDL_CreateSemaphore(0);
        char name[32];
        SDL_snprintf(name, sizeof name, "openpete-extract%d", w);
        s_thread[w] = SDL_CreateThread(wp_thread_main, name, (void*)(intptr_t)w);
        if (!s_thread[w]) {                /* thread creation failed: run without it */
            SDL_DestroySemaphore(s_start[w]);
            s_start[w] = NULL;
            s_threads = w;
            break;
        }
    }
    s_spawned = 1;
}

/** @brief Do nothing: the per-item function of wp_selftest's empty runs.
 *
 * @param item Unused.
 * @param worker Unused.
 * @param arg Unused. */
static void wp_noop(int item, int worker, void* arg) {
    (void)item; (void)worker; (void)arg;
}

/** @brief Log what a pool run costs with no work in it, per item count, at the resolved worker count.
 *
 * Runs under OPENPETE_WORKPOOL_SELFTEST=1. The sweep over item counts at a
 * fixed worker count separates the two parts of the fan-out floor: the
 * constant part is the semaphore wake and join (n-1 signals, n-1 waits),
 * and the slope in item count is contention on the one pull counter, which
 * every worker increments once per item. A caller deciding whether a scene
 * is large enough to fan out needs this number, and it varies most between
 * machines. Unlike openpete_workpool_run, every run wakes all helpers. */
static void wp_selftest(void) {
    static const int ITEMS[] = { 1, 2, 8, 32, 128, 512 };
    const int reps = 200;
    int n = openpete_workpool_threads();
    SDL_Log("[workpool] selftest: %d worker(s), %d reps, empty items", n, reps);
    for (size_t k = 0; k < sizeof ITEMS / sizeof ITEMS[0]; k++) {
        int items = ITEMS[k];
        double best = 1e30, sum = 0.0;
        Uint64 freq = SDL_GetPerformanceFrequency();
        for (int r = 0; r < reps; r++) {
            Uint64 t0 = SDL_GetPerformanceCounter();
            s_job.fn = wp_noop; s_job.arg = NULL; s_job.nitems = items;
            SDL_SetAtomicInt(&s_job.next, 0);
            for (int w = 1; w < n; w++) SDL_SignalSemaphore(s_start[w]);
            wp_work(0);
            for (int w = 1; w < n; w++) SDL_WaitSemaphore(s_done);
            double us = (double)(SDL_GetPerformanceCounter() - t0) * 1e6 / (double)freq;
            sum += us;
            if (us < best) best = us;
        }
        SDL_Log("[workpool]   items=%-4d mean %7.2f us   best %7.2f us", 
                items, sum / reps, best);
    }
}

void openpete_workpool_run(int nitems, openpete_work_fn fn OP_SYNC_FN, void* arg) {
    if (nitems <= 0) return;
    wp_spawn();
    {
        static int st = -1;
        if (st < 0) {
            const char* e = SDL_getenv("OPENPETE_WORKPOOL_SELFTEST");
            st = (e && *e && *e != '0');
            if (st && openpete_workpool_threads() > 1) wp_selftest();
        }
    }
    int n = openpete_workpool_threads();
    if (n <= 1 || nitems <= 1) {
        for (int i = 0; i < nitems; i++) fn(i, 0, arg);
        return;
    }
    /* Wake at most one helper per item beyond the caller's own. The join has
     * to wait for every helper it signalled to come back, and a helper with no
     * item left still costs its full wake latency before it can post done - so
     * signalling more helpers than there is work for buys nothing and sets the
     * run's floor at the slowest of them. */
    int wake = nitems < n ? nitems : n;
    s_job.fn = fn; s_job.arg = arg; s_job.nitems = nitems;
    SDL_SetAtomicInt(&s_job.next, 0);
    for (int w = 1; w < wake; w++) SDL_SignalSemaphore(s_start[w]);
    wp_work(0);
    /* The join: the caller has run out of items and waits for the helpers.
     * A wait region, so a profile shows the fan-out's wall time on the
     * caller as the items the caller ran plus this wait. */
    { OP_PROF_FLAGS_ACCUM_SCOPE("pool.wait", OP_PROF_F_WAIT);
      for (int w = 1; w < wake; w++) SDL_WaitSemaphore(s_done); }
}
