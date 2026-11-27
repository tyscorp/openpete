// core/include/openpete_workpool.h - data-parallel extraction worker pool.
//
// A persistent pool of worker threads for fan-out over independent items
// (terrain sectors, compose buckets). Contracts:
//  - Workers run ONLY inside openpete_workpool_run(); between runs they are
//    parked. They must never touch the game fiber or any thread-affine state.
//  - Callers may only hand work that reads guest RAM while the game fiber is
//    parked (the extraction window) and writes exclusively to per-item or
//    per-worker state.
//  - openpete_workpool_run() is itself single-threaded: it is called from the
//    host-loop thread only, and returns only after every item completed, so
//    the caller resumes with all results visible (semaphores order the
//    memory).
//  - Item scheduling is dynamic (atomic pull); callers needing deterministic
//    OUTPUT order must tag results by item index and merge in item order.

#ifndef OPENPETE_WORKPOOL_H
#define OPENPETE_WORKPOOL_H

#include "openpete_thread_roles.h"   /* OP_SYNC_FN */

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*openpete_work_fn)(int item, int worker, void* arg);

/* Per-worker state is an array indexed by worker id whose element fills whole
 * cache lines, so no two workers write one line wherever the linker places
 * the array. The element type's first member carries OPENPETE_WORKER_ALIGN
 * (alignment of the type, and with it a size padded to whole lines), and
 * OPENPETE_WORKER_ELEM(T) beside the type checks both at compile time.
 * scripts/fences/worker_line_guard.py refuses a per-worker array whose
 * element type has no OPENPETE_WORKER_ELEM. */
#define OPENPETE_CACHE_LINE 64
#ifdef __cplusplus
#define OPENPETE_WORKER_ALIGN alignas(OPENPETE_CACHE_LINE)
#define OPENPETE_WORKER_ELEM(T) \
    static_assert(alignof(T) >= OPENPETE_CACHE_LINE && sizeof(T) % OPENPETE_CACHE_LINE == 0, \
                  #T " is a per-worker element: OPENPETE_WORKER_ALIGN on its first member")
#else
#define OPENPETE_WORKER_ALIGN _Alignas(OPENPETE_CACHE_LINE)
#define OPENPETE_WORKER_ELEM(T) \
    _Static_assert(_Alignof(T) >= OPENPETE_CACHE_LINE && sizeof(T) % OPENPETE_CACHE_LINE == 0, \
                   #T " is a per-worker element: OPENPETE_WORKER_ALIGN on its first member")
#endif

/** @brief Set the requested pool size, from --extract-threads, before the pool's first use.
 *
 * The size is resolved on the first openpete_workpool_threads() call and
 * does not change after it.
 * @param threads 0 for auto (logical cores / 2, at least 1); 1 for serial
 *        (no threads created, openpete_workpool_run is a plain loop); any
 *        count is capped at 8. */
void openpete_workpool_configure(int threads);

/** @brief Resolve and return the worker count.
 *
 * Worker ids passed to fn are 0..count-1; id 0 is the calling thread, which
 * works the items alongside the helpers. The first call latches the
 * configured size; a helper thread that fails to spawn lowers the count.
 * @return The worker count, 1 to 8. */
int openpete_workpool_threads(void);

/** @brief Run fn(item, worker, arg) for every item in [0, nitems) and return after all complete.
 *
 * Spawns the helper threads on first use. Serial, in item order, on the
 * calling thread when the pool has one worker or nitems <= 1; otherwise
 * wakes min(nitems, workers) - 1 helpers, and every participant pulls items
 * from one atomic counter. OPENPETE_WORKPOOL_SELFTEST=1 logs the empty-run
 * cost sweep once, on the first call.
 * @param nitems The item count; 0 or less returns at once.
 * @param fn The per-item function.
 * @param arg Passed to every fn call. */
void openpete_workpool_run(int nitems, openpete_work_fn fn OP_SYNC_FN, void* arg);

#ifdef __cplusplus
}
#endif

#endif
