/* core/psx/disc_cache.c - see openpete_disc_cache.h. */
#include "openpete_disc_cache.h"
#include "openpete_disc_image.h"
#include "openpete_disc_trace.h"
#include "openpete_crash.h"
#include "openpete_log.h"
#include "openpete_time.h"
#include "openpete_pace_log.h"
#include "psx/libcd.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

/* The image is tracked per 2352-byte sector and filled by one loader thread
 * in runs of missing sectors. Run order: the chunk a game read last missed
 * on, the chunk an XA read last missed on, the game's boot-order profile
 * ranges in order, then every chunk in file order. A chunk is a whole number
 * of sectors, 8 MB rounded down; a chunk run never crosses its chunk and a
 * profile run never leaves its range. A read never waits on the loader; the
 * reader serves a miss from the file. */
#define DC_SECTOR        2352u
#define DC_CHUNK_SECTORS ((size_t)((1u << 23) / DC_SECTOR))   /* 3566 sectors */
#define DC_CHUNK         (DC_CHUNK_SECTORS * DC_SECTOR)

/* Steer slots, in the loader's priority order. Host paths (the woah NSF,
 * the library scan, the ingest copy, the sfx census, mods) have none: a
 * host read never reorders the loader ahead of the game. */
enum { DC_STEER_GAME = 0, DC_STEER_XA, DC_STEER_SLOTS };

/* The VBL rate the throttle's budget accrues on: 2250000/37609 Hz. */
#define DC_VBL_NUM 2250000.0
#define DC_VBL_DEN 37609.0

/* s_state: whether the image is attached and how auto is resolving.
 * DC_DETACHED: never attached (--disc-cache 0, or before boot).
 * DC_PROBING: attached under auto; the loader times its uncached reads.
 * DC_ATTACHED: attached; the loader fills the image to the end.
 * DC_WATCHING: auto measured the file fast and freed the image; demand
 * reads served from the file are timed, and a burst over a quarter tick
 * attaches the cache again (state DC_ATTACHED). */
enum { DC_DETACHED = 0, DC_PROBING, DC_ATTACHED, DC_WATCHING };

/* The module lock as the analysis sees it: dc_lock and dc_unlock take and
 * release s_mx. Every mutable global below except s_state is read and
 * written under it. */
static const op_lock_cap_t s_dc_lock;
static SDL_Mutex*     s_mx;            /* created by the first set_boot_order or attach, on the boot thread */
static SDL_AtomicInt  s_state;         /* DC_* ; the reader fast path checks it before locking */
static uint8_t*       s_img OP_GUARDED_BY_CAP(s_dc_lock);   /* NULL while detached or watching */
static size_t         s_size OP_GUARDED_BY_CAP(s_dc_lock);
static size_t         s_nchunks OP_GUARDED_BY_CAP(s_dc_lock);
static size_t         s_nsec OP_GUARDED_BY_CAP(s_dc_lock);
static uint8_t*       s_have OP_GUARDED_BY_CAP(s_dc_lock);  /* per-chunk DC_MISSING / DC_RESIDENT / DC_UNREAD */
static uint8_t*       s_sec OP_GUARDED_BY_CAP(s_dc_lock);   /* per-sector DC_MISSING / DC_RESIDENT / DC_UNREAD */
static uint8_t*       s_logged OP_GUARDED_BY_CAP(s_dc_lock); /* per-chunk: a miss on it was logged */
static size_t         s_have_n OP_GUARDED_BY_CAP(s_dc_lock);  /* chunks resident */
static long           s_want[DC_STEER_SLOTS] OP_GUARDED_BY_CAP(s_dc_lock); /* chunk each slot steers to, -1 = none */
static size_t         s_seq OP_GUARDED_BY_CAP(s_dc_lock);     /* next chunk in file order */
static const openpete_disc_range_t* s_order OP_GUARDED_BY_CAP(s_dc_lock); /* boot-order profile, NULL = none */
static size_t         s_order_n OP_GUARDED_BY_CAP(s_dc_lock);
static size_t         s_order_at OP_GUARDED_BY_CAP(s_dc_lock); /* next profile range */
static uint64_t       s_bps OP_GUARDED_BY_CAP(s_dc_lock);      /* OPENPETE_DISC_CACHE_BPS, 0 = unthrottled */
static uint64_t       s_acct OP_GUARDED_BY_CAP(s_dc_lock);     /* bytes charged to the throttle */
static int            s_abort OP_GUARDED_BY_CAP(s_dc_lock);    /* shutdown asked; nothing attaches after it */
static SDL_Thread*    s_thr OP_GUARDED_BY_CAP(s_dc_lock);      /* the loader, joined by shutdown or the next attach */
static char*          s_path OP_GUARDED_BY_CAP(s_dc_lock);     /* the image path; a reader of the same file is served here */
static int64_t        s_load_ns OP_GUARDED_BY_CAP(s_dc_lock);
static uint64_t       s_misses OP_GUARDED_BY_CAP(s_dc_lock);
static uint64_t       s_misses_by_tag[OPENPETE_DISC_TAG_COUNT] OP_GUARDED_BY_CAP(s_dc_lock);
static int64_t        s_direct_ns OP_GUARDED_BY_CAP(s_dc_lock);
static openpete_disc_burst_t s_burst OP_GUARDED_BY_CAP(s_dc_lock); /* the watchdog's per-tick sum */

/** @brief Take the module lock. */
static void dc_lock(void) OP_ACQUIRE_CAP(s_dc_lock) OP_NO_THREAD_SAFETY_ANALYSIS {
    SDL_LockMutex(s_mx);
}
/** @brief Release the module lock. */
static void dc_unlock(void) OP_RELEASE_CAP(s_dc_lock) OP_NO_THREAD_SAFETY_ANALYSIS {
    SDL_UnlockMutex(s_mx);
}
/** @brief Create the module lock on first use; called only on the boot thread, before any loader exists. */
static void dc_mutex_init(void) {
    if (!s_mx) s_mx = SDL_CreateMutex();
}

/* s_have and s_sec states. A DC_UNREAD sector came up short in the loader
 * after DC_READ_TRIES attempts; it is never served from memory, so every
 * read of it goes to the file. A chunk is DC_MISSING while any of its
 * sectors is, else DC_UNREAD while any is unread, else DC_RESIDENT. */
#define DC_MISSING   0u
#define DC_RESIDENT  1u
#define DC_UNREAD    2u
#define DC_READ_TRIES 3

/* Sectors [s0, s1) the loader reads in one go, into img (s_img as the run
 * was chosen; the loader is the only thread that frees it). */
typedef struct { size_t s0, s1; uint8_t* img; size_t size; } dc_run_t;

/* The loader thread's own handles, owned by the thread once started. */
typedef struct {
    FILE*                     fp;      /* buffered handle; every run without a probe, and retries */
    openpete_disc_uncached_t* unc;     /* uncached handle while auto measures, else NULL */
    char                      how[48]; /* the uncached method, for the log */
    int64_t                   t0;      /* op_now() at attach */
    openpete_disc_probe_t     probe;   /* timed uncached reads */
} dc_loader_t;

void openpete_disc_cache_set_boot_order(const openpete_disc_range_t* ranges, size_t n) {
    dc_mutex_init();
    dc_lock();
    s_order = n ? ranges : NULL;
    s_order_n = ranges ? n : 0;
    dc_unlock();
}

/** @brief Read the sectors of run r from fp into the run's image; bytes the read does not supply are zeroed.
 *
 * @param fp An open handle on the image.
 * @param r The run; its last sector may pass the end of the image.
 * @return 1 when the whole run was read, 0 on a seek failure or a short read. */
static int read_run_from(FILE* fp, dc_run_t r) {
    size_t at = r.s0 * DC_SECTOR;
    size_t end = r.s1 * DC_SECTOR; if (end > r.size) end = r.size;
    size_t want = end - at;
    size_t got = 0;
    if (fseek(fp, (long)at, SEEK_SET) == 0) got = fread(r.img + at, 1, want, fp);
    openpete_disc_trace_bytes(OPENPETE_DISC_TAG_CACHE_LOAD, at, got, OPENPETE_DISC_DIRECT);
    if (got < want) memset(r.img + at + got, 0, want - got);
    return got == want;
}

/** @brief Read run r through the buffered handle, retrying a short read up to DC_READ_TRIES times.
 *
 * @param fp The loader's buffered handle.
 * @param r The run.
 * @return 1 when the run was read whole. */
static int read_run(FILE* fp, dc_run_t r) {
    for (int t = 0; t < DC_READ_TRIES; t++)
        if (read_run_from(fp, r)) return 1;
    return 0;
}

/** @brief Read run r through the uncached handle in OPENPETE_DISC_PROBE_BLOCK reads, timing each into the probe.
 *
 * A short read falls back to read_run on the buffered handle, untimed.
 * @param L The loader.
 * @param r The run.
 * @return 1 when the run was read whole. */
static int read_run_probed(dc_loader_t* L, dc_run_t r) {
    size_t at = r.s0 * DC_SECTOR;
    size_t end = r.s1 * DC_SECTOR; if (end > r.size) end = r.size;
    while (at < end) {
        size_t n = end - at < OPENPETE_DISC_PROBE_BLOCK ? end - at : OPENPETE_DISC_PROBE_BLOCK;
        int64_t t0 = op_now();
        size_t got = openpete_disc_uncached_read(L->unc, at, r.img + at, n);
        int64_t ns = op_now() - t0;
        openpete_disc_trace_bytes(OPENPETE_DISC_TAG_CACHE_LOAD, at, got, OPENPETE_DISC_DIRECT);
        if (got < n) return read_run(L->fp, r);
        openpete_disc_probe_add(&L->probe, got, ns);
        at += n;
    }
    return 1;
}

/** @brief Find the first run of DC_MISSING sectors in [lo, hi), under the lock.
 *
 * @param lo First sector.
 * @param hi One past the last sector; clipped to the image.
 * @param r Receives the run's sectors.
 * @return 1 when a run was found. */
static int missing_run(size_t lo, size_t hi, dc_run_t* r) OP_REQUIRES_CAP(s_dc_lock) {
    if (hi > s_nsec) hi = s_nsec;
    size_t s = lo;
    while (s < hi && s_sec[s] != DC_MISSING) s++;
    if (s >= hi) return 0;
    size_t e = s;
    while (e < hi && s_sec[e] == DC_MISSING) e++;
    r->s0 = s; r->s1 = e;
    return 1;
}

/** @brief Find the first run of missing sectors in chunk c, under the lock.
 *
 * @param c Chunk index.
 * @param r Receives the run's sectors.
 * @return 1 when the chunk has a missing sector. */
static int chunk_run(size_t c, dc_run_t* r) OP_REQUIRES_CAP(s_dc_lock) {
    return s_have[c] == DC_MISSING &&
           missing_run(c * DC_CHUNK_SECTORS, (c + 1u) * DC_CHUNK_SECTORS, r);
}

/** @brief Pick the loader's next run under the lock: each steer slot's chunk in slot order, the profile ranges in order, then file order.
 *
 * @param r Receives the run and the image it fills.
 * @return 1 when a run was chosen, 0 when no sector is missing. */
static int next_run(dc_run_t* r) OP_REQUIRES_CAP(s_dc_lock) {
    r->img = s_img; r->size = s_size;
    for (int k = 0; k < DC_STEER_SLOTS; k++) {
        long w = s_want[k];
        if (w >= 0 && chunk_run((size_t)w, r)) return 1;
        s_want[k] = -1;
    }
    for (; s_order_at < s_order_n; s_order_at++) {
        const openpete_disc_range_t* g = &s_order[s_order_at];
        if (missing_run(g->lba, (size_t)g->lba + g->count, r)) return 1;
    }
    for (; s_seq < s_nchunks; s_seq++)
        if (chunk_run(s_seq, r)) return 1;
    return 0;
}

/** @brief Mark run r's sectors resident or unread and recompute the state of every chunk it touches, under the lock.
 *
 * @param r The run.
 * @param ok 1 when the loader read it whole. */
static void mark_run(dc_run_t r, int ok) OP_REQUIRES_CAP(s_dc_lock) {
    memset(s_sec + r.s0, ok ? DC_RESIDENT : DC_UNREAD, r.s1 - r.s0);
    for (size_t c = r.s0 / DC_CHUNK_SECTORS; c <= (r.s1 - 1u) / DC_CHUNK_SECTORS; c++) {
        size_t lo = c * DC_CHUNK_SECTORS, hi = lo + DC_CHUNK_SECTORS;
        if (hi > s_nsec) hi = s_nsec;
        uint8_t st = DC_RESIDENT;
        for (size_t s = lo; s < hi && st != DC_MISSING; s++)
            if (s_sec[s] != DC_RESIDENT) st = s_sec[s];
        if (st == DC_RESIDENT && s_have[c] != DC_RESIDENT) s_have_n++;
        s_have[c] = st;
    }
}

/** @brief Hold the loader until the throttle's VBL-clock budget covers every byte charged so far.
 *
 * The budget is OPENPETE_DISC_CACHE_BPS bytes per second of VBL time
 * (2250000/37609 Hz) since VBL 0; the charge is every byte the loader has
 * read plus every byte a miss read from the file. Returns at once when
 * unthrottled or on shutdown.
 * @param bytes The run's byte count, added to the charge first. */
static void throttle(size_t bytes) {
    dc_lock();
    uint64_t bps = s_bps;
    s_acct += bytes;
    dc_unlock();
    if (!bps) return;
    for (;;) {
        dc_lock();
        uint64_t acct = s_acct;
        int abort_now = s_abort;
        dc_unlock();
        int vbl = openpete_disc_trace_vbl();
        double budget = (double)bps * (vbl > 0 ? vbl : 0) * DC_VBL_DEN / DC_VBL_NUM;
        if (abort_now || budget >= (double)acct) return;
        SDL_Delay(1);
    }
}

/** @brief Free the image and its residency maps and arm the watchdog, under the lock.
 *
 * Readers then get -1 from openpete_disc_cache_copy and read the file. */
static void detach_locked(void) OP_REQUIRES_CAP(s_dc_lock) {
    free(s_img); s_img = NULL;
    free(s_sec); s_sec = NULL;
    free(s_have); s_have = NULL;
    free(s_logged); s_logged = NULL;
    s_nsec = s_nchunks = s_have_n = 0;
    s_burst.tick = -2; s_burst.ns = 0;
    SDL_SetAtomicInt(&s_state, DC_WATCHING);
}

/** @brief Take auto's decision from the loader's probe and log it; a fast file detaches the cache.
 *
 * Closes the uncached handle. An image already wholly resident stays
 * attached whatever the verdict, since freeing it saves nothing read.
 * @param L The loader.
 * @param done 1 when the loader has read every sector.
 * @return 1 when the cache was detached and the loader stops. */
static int auto_decide(dc_loader_t* L, int done) {
    char why[160];
    const int slow = openpete_disc_probe_slow(&L->probe, why, sizeof why);
    openpete_disc_uncached_close(L->unc);
    L->unc = NULL;
    const int off = !slow && !done;
    OPL_INFO_F("boot", "disc cache: auto measured %s (%s) -> %s", why, L->how,
               off ? "off, watching demand reads" : (slow ? "on" : "on, already resident"));
    dc_lock();
    if (off && !s_abort) detach_locked();
    else SDL_SetAtomicInt(&s_state, DC_ATTACHED);
    int stop = off && !s_abort;
    dc_unlock();
    return stop;
}

/** @brief Report whether the probe has measured enough: the profile read and OPENPETE_DISC_PROBE_MIN bytes, or OPENPETE_DISC_PROBE_BUDGET bytes.
 *
 * @param L The loader.
 * @param profile_done 1 when every profile range has been read.
 * @return 1 when auto decides now. */
static int probe_due(const dc_loader_t* L, int profile_done) {
    return L->unc && ((profile_done && L->probe.bytes >= OPENPETE_DISC_PROBE_MIN) ||
                      L->probe.bytes >= OPENPETE_DISC_PROBE_BUDGET);
}

/** @brief Loader thread: read every missing sector once, run by run in next_run order, then close the image.
 *
 * Under auto the runs are read uncached and timed until probe_due, when
 * auto_decide either detaches the cache (the thread ends) or the loader
 * fills the rest through the buffered handle. A run that stays short after
 * DC_READ_TRIES attempts is marked DC_UNREAD with its missing bytes zeroed;
 * reads of it go to the file.
 * @param arg The dc_loader_t, freed here.
 * @return 0. */
static int loader_main(void* arg) {
    OP_ROLE_BIND(disc);
    dc_loader_t* L = (dc_loader_t*)arg;
    openpete_crash_thread_attach();
    int detached = 0;
    for (;;) {
        dc_run_t r;
        dc_lock();
        int more = !s_abort && next_run(&r);
        int profile_done = s_order_at >= s_order_n;
        dc_unlock();
        if (!more) break;                       /* everything resident, or shutting down */
        int ok = L->unc ? read_run_probed(L, r) : read_run(L->fp, r);
        throttle((r.s1 - r.s0) * DC_SECTOR);
        dc_lock();
        mark_run(r, ok);
        dc_unlock();
        if (!ok)
            OPL_WARN_F("cd", "disc cache: short read of sectors [%zu,%zu) after %d attempts; "
                       "reads of them go to the file", r.s0, r.s1, DC_READ_TRIES);
        if (probe_due(L, profile_done) && auto_decide(L, 0)) { detached = 1; break; }
    }
    dc_lock();
    int aborted = s_abort;
    dc_unlock();
    if (L->unc && !aborted) (void)auto_decide(L, 1);
    openpete_disc_uncached_close(L->unc);
    fclose(L->fp);
    int64_t ns = op_now() - L->t0;
    free(L);
    dc_lock();
    s_load_ns = ns;
    size_t mb = s_size >> 20;
    dc_unlock();
    if (!detached && !aborted)
        OPL_INFO_F("cd", "disc cache: %zu MB resident in %.2f s", mb, (double)ns / 1e9);
    return 0;
}

/** @brief Read the loader's harness knobs, under the lock: OPENPETE_DISC_CACHE_BPS (VBL-clock throttle) and OPENPETE_DISC_BOOT_ORDER=0 (profile off). */
static void read_loader_env(void) OP_REQUIRES_CAP(s_dc_lock) {
    const char* bps = getenv("OPENPETE_DISC_CACHE_BPS");
    s_bps = bps && bps[0] ? strtoull(bps, NULL, 10) : 0;
    const char* order = getenv("OPENPETE_DISC_BOOT_ORDER");
    if (order && strcmp(order, "0") == 0) s_order_n = 0;
    s_order_at = 0; s_acct = 0;
}

/** @brief Open the image, allocate it and its maps, and start the loader.
 *
 * Installs the state under the lock and creates the thread there; when
 * the thread cannot be created the image loads inline before the call
 * returns. Refuses after shutdown.
 * @param bin_path The image path.
 * @param unc The uncached handle auto measures with, or NULL; owned here.
 * @param how The uncached method, for the decision line.
 * @param first_chunk A chunk the loader reads first, or -1.
 * @return 1 when the cache is attached. */
static int dc_start(const char* bin_path, openpete_disc_uncached_t* unc, const char* how,
                    long first_chunk) {
    FILE* fp = fopen(bin_path, "rb");
    if (!fp) {
        OPL_WARN_F("cd", "disc cache: cannot open %s", bin_path);
        openpete_disc_uncached_close(unc);
        return 0;
    }
    long long sz = -1;
    if (fseek(fp, 0, SEEK_END) == 0) sz = ftell(fp);
    if (sz <= 0 || sz > 0x7FFFFFFFLL) {   /* PsyX_CDFS_Init_Mem takes an int size */
        OPL_WARN_F("cd", "disc cache: image size %lld unusable", sz);
        fclose(fp); openpete_disc_uncached_close(unc); return 0;
    }
    fseek(fp, 0, SEEK_SET);
    size_t size = (size_t)sz;
    size_t nsec = (size + DC_SECTOR - 1u) / DC_SECTOR;
    size_t nchunks = (nsec + DC_CHUNK_SECTORS - 1u) / DC_CHUNK_SECTORS;
    uint8_t* img = (uint8_t*)malloc(size);
    uint8_t* have = (uint8_t*)calloc(nchunks, 1);
    uint8_t* sec = (uint8_t*)calloc(nsec, 1);
    uint8_t* logged = (uint8_t*)calloc(nchunks, 1);
    dc_loader_t* L = (dc_loader_t*)calloc(1, sizeof *L);
    char* path = (char*)malloc(strlen(bin_path) + 1u);
    if (!img || !have || !sec || !logged || !L || !path) {
        OPL_WARN_F("cd", "disc cache: cannot allocate %lld bytes", sz);
        free(img); free(have); free(sec); free(logged); free(L); free(path);
        fclose(fp); openpete_disc_uncached_close(unc); return 0;
    }
    memcpy(path, bin_path, strlen(bin_path) + 1u);
    L->fp = fp; L->unc = unc; L->t0 = op_now();
    snprintf(L->how, sizeof L->how, "%s", how ? how : "");
    dc_lock();
    if (s_abort) {
        dc_unlock();
        free(img); free(have); free(sec); free(logged); free(L); free(path);
        fclose(fp); openpete_disc_uncached_close(unc); return 0;
    }
    s_img = img; s_size = size; s_nsec = nsec; s_nchunks = nchunks;
    s_have = have; s_sec = sec; s_logged = logged; s_have_n = 0; s_seq = 0;
    free(s_path); s_path = path;
    for (int k = 0; k < DC_STEER_SLOTS; k++) s_want[k] = -1;
    if (first_chunk >= 0 && (size_t)first_chunk < nchunks) s_want[DC_STEER_GAME] = first_chunk;
    read_loader_env();
    size_t order_n = s_order_n;
    uint64_t bps = s_bps;
    /* Readers consult s_state only after the loader state above exists. */
    SDL_SetAtomicInt(&s_state, unc ? DC_PROBING : DC_ATTACHED);
    SDL_Thread* thr = SDL_CreateThread(loader_main, "openpete-disc", L);
    s_thr = thr;
    dc_unlock();
    OPL_INFO_F("boot", "disc cache: %s (%zu MB) loading in the background, boot order %zu "
               "range(s)%s, throttle %llu B/s%s", bin_path, size >> 20, order_n,
               order_n ? "" : " (file order)", (unsigned long long)bps,
               unc ? ", measuring" : "");
    if (!thr) {
        /* No thread: load synchronously so the image is complete before use. */
        OPL_WARN_F("cd", "disc cache: SDL_CreateThread failed (%s); loading inline", SDL_GetError());
        loader_main(L);
    }
    return 1;
}

int openpete_disc_cache_attach(const char* bin_path) {
    if (openpete_disc_cache_active() || !bin_path) return openpete_disc_cache_active();
    dc_mutex_init();
    return dc_start(bin_path, NULL, NULL, -1);
}

int openpete_disc_cache_attach_auto(const char* bin_path) {
    if (SDL_GetAtomicInt(&s_state) != DC_DETACHED || !bin_path) return openpete_disc_cache_active();
    dc_mutex_init();
    char how[48];
    openpete_disc_uncached_t* unc = openpete_disc_uncached_open(bin_path, how, sizeof how);
    if (unc) return dc_start(bin_path, unc, how, -1);
    /* No way to read past the OS file cache: the file path serves, watched. */
    char* path = (char*)malloc(strlen(bin_path) + 1u);
    if (!path) return 0;
    memcpy(path, bin_path, strlen(bin_path) + 1u);
    OPL_INFO_F("boot", "disc cache: auto cannot measure (%s) -> off, watching demand reads", how);
    dc_lock();
    free(s_path); s_path = path;
    detach_locked();
    dc_unlock();
    return 0;
}

int openpete_disc_cache_active(void) {
    const int st = SDL_GetAtomicInt(&s_state);
    return st == DC_PROBING || st == DC_ATTACHED;
}

/** @brief Map a read path to its steer slot.
 *
 * The EXE read (with its volume descriptor and directory sectors) runs once
 * as the cache attaches and is never re-read, so it steers nothing.
 * @param tag An OPENPETE_DISC_TAG_* value.
 * @return DC_STEER_GAME for libcd, the extract and the WAD overlay;
 *         DC_STEER_XA for XA; -1 (no steer) for the EXE and every host
 *         path. */
static int steer_slot(int tag) {
    switch (tag) {
    case OPENPETE_DISC_TAG_LIBCD:
    case OPENPETE_DISC_TAG_EXTRACT:
    case OPENPETE_DISC_TAG_WAD:     return DC_STEER_GAME;
    case OPENPETE_DISC_TAG_XA:      return DC_STEER_XA;
    default:                        return -1;
    }
}

/** @brief Find the first sector of [s0, s1] not resident, under the lock.
 *
 * Chunks wholly resident are skipped without a sector scan.
 * @param s0 First sector.
 * @param s1 Last sector (inclusive).
 * @return The sector, or -1 when every sector is resident. */
static long first_absent(size_t s0, size_t s1) OP_REQUIRES_CAP(s_dc_lock) {
    for (size_t s = s0; s <= s1;) {
        size_t c = s / DC_CHUNK_SECTORS;
        size_t c_end = (c + 1u) * DC_CHUNK_SECTORS;
        if (s_have[c] == DC_RESIDENT) { s = c_end; continue; }
        for (; s <= s1 && s < c_end; s++)
            if (s_sec[s] != DC_RESIDENT) return (long)s;
    }
    return -1;
}

int openpete_disc_cache_copy(const char* bin_path, uint64_t offset, void* dst,
                             size_t len, int tag) {
    if (!openpete_disc_cache_active() || !bin_path) return -1;
    dc_lock();
    if (!s_img || !s_path || strcmp(bin_path, s_path) != 0 ||
        offset >= s_size || len > s_size - offset) {
        dc_unlock();
        return -1;
    }
    if (len == 0) { dc_unlock(); return 1; }
    long miss = first_absent((size_t)(offset / DC_SECTOR), (size_t)((offset + len - 1u) / DC_SECTOR));
    if (miss >= 0) {
        size_t c = (size_t)miss / DC_CHUNK_SECTORS;
        int slot = steer_slot(tag);
        if (slot >= 0 && s_have[c] == DC_MISSING && !s_abort) s_want[slot] = (long)c;
        dc_unlock();
        return 0;
    }
    /* The copy runs under the lock: the loader frees the image under it
     * when auto detaches. */
    memcpy(dst, s_img + offset, len);
    dc_unlock();
    return 1;
}

void openpete_disc_cache_miss_served(int tag, uint64_t offset, size_t len, int64_t ns) {
    if (!openpete_disc_cache_active()) return;
    size_t c = (size_t)(offset / DC_CHUNK);
    uint64_t s0 = offset / DC_SECTOR, s1 = (offset + len + DC_SECTOR - 1u) / DC_SECTOR;
    dc_lock();
    if (!s_logged || c >= s_nchunks) { dc_unlock(); return; }
    s_misses++;
    if (tag >= 0 && tag < OPENPETE_DISC_TAG_COUNT) s_misses_by_tag[tag]++;
    s_direct_ns += ns;
    s_acct += (s1 - s0) * DC_SECTOR;           /* a direct read shares the throttled device */
    int first = !s_logged[c];
    s_logged[c] = 1;
    size_t have = s_have_n, nchunks = s_nchunks;
    dc_unlock();
    openpete_pace_cd_miss(ns, offset + len);
    if (first)
        OPL_WARN_F("cd", "disc cache miss: %s read [%llu,%llu) in chunk %zu served from the file "
                   "in %.2f ms (%zu/%zu chunks resident)", openpete_disc_trace_tag_name(tag),
                   (unsigned long long)offset, (unsigned long long)(offset + len), c,
                   (double)ns / 1e6, have, nchunks);
}

void openpete_disc_cache_direct_served(const char* bin_path, int tag, uint64_t offset,
                                       size_t len, int64_t ns) {
    if (SDL_GetAtomicInt(&s_state) != DC_WATCHING || steer_slot(tag) < 0 || !bin_path) return;
    const int vbl = openpete_disc_trace_vbl();
    if (vbl < 0) return;   /* before the first VBL no tick runs, so none can be late */
    const int64_t tick = vbl / 2;   /* two VBLs per tick */
    dc_lock();
    int flip = SDL_GetAtomicInt(&s_state) == DC_WATCHING && !s_abort && s_path &&
               strcmp(bin_path, s_path) == 0 && openpete_disc_burst_add(&s_burst, tick, ns);
    int64_t burst = s_burst.ns;
    SDL_Thread* old = NULL;
    char* path = NULL;
    if (flip) {
        SDL_SetAtomicInt(&s_state, DC_ATTACHED);   /* no second flip; readers see no image yet */
        old = s_thr; s_thr = NULL;
        path = s_path; s_path = NULL;
    }
    dc_unlock();
    if (!flip) return;
    OPL_INFO_F("cd", "disc cache: demand reads took %.2f ms of tick %lld (watchdog ceiling "
               "%.2f ms); the cache attaches from chunk %llu", (double)burst / 1e6,
               (long long)tick, (double)OPENPETE_DISC_BURST_NS / 1e6,
               (unsigned long long)(offset / DC_CHUNK));
    (void)len;
    if (old) SDL_WaitThread(old, NULL);   /* the measuring loader ended at its decision */
    if (!dc_start(path, NULL, NULL, (long)(offset / DC_CHUNK)))
        SDL_SetAtomicInt(&s_state, DC_DETACHED);
    free(path);
}

void openpete_disc_cache_shutdown(void) {
    if (SDL_GetAtomicInt(&s_state) == DC_DETACHED) return;
    dc_lock();
    s_abort = 1;
    SDL_Thread* t = s_thr;
    s_thr = NULL;
    dc_unlock();
    if (t) SDL_WaitThread(t, NULL);
    /* The image stays: a resident sector still serves a late read, and the OS
     * reclaims it at exit. */
}

/* PsyCross's image reads, bound to one disc_image handle. The result of
 * the last read is kept for the read observer, which runs right after it
 * on the same thread. */
static disc_image_t* s_psx_img;
static int           s_psx_result = OPENPETE_DISC_DIRECT;

/** @brief PsyCross image read: serve [b, e) into dst through the disc_image handle.
 *
 * @param dst Receives e - b bytes.
 * @param b First byte offset.
 * @param e One past the last byte offset.
 * @param source PSYX_CDFS_READ_DATA or PSYX_CDFS_READ_XA.
 * @return 1: the bytes are always served here. */
static int psycross_read(void* dst, size_t b, size_t e, int source) {
    int tag = source == PSYX_CDFS_READ_XA ? OPENPETE_DISC_TAG_XA : OPENPETE_DISC_TAG_LIBCD;
    int r = disc_image_read_bytes(s_psx_img, b, dst, e > b ? e - b : 0, tag);
    s_psx_result = r < 0 ? OPENPETE_DISC_DIRECT : r;
    return 1;
}

int openpete_disc_psycross_bind(disc_image_t* img) {
    uint64_t size = img ? disc_image_size(img) : 0;
    if (!size || size > 0x7FFFFFFFu) return 0;   /* PsyX_CDFS_Init_Mem takes an int size */
    s_psx_img = img;
    PsyX_CDFS_SetMemRead(psycross_read);
    PsyX_CDFS_Init_Mem(NULL, (int)size, /*track=*/0, /*sectorSize=*/0);
    return 1;
}

/** @brief PsyCross read observer: trace one image read as libcd or xa with the result psycross_read recorded.
 *
 * @param b First byte offset.
 * @param e One past the last byte offset.
 * @param source PSYX_CDFS_READ_DATA or PSYX_CDFS_READ_XA. */
static void trace_psycross_read(size_t b, size_t e, int source) {
    int tag = source == PSYX_CDFS_READ_XA ? OPENPETE_DISC_TAG_XA : OPENPETE_DISC_TAG_LIBCD;
    openpete_disc_trace_bytes(tag, b, e > b ? e - b : 0, s_psx_result);
}

void openpete_disc_cache_trace_install(void) {
    if (openpete_disc_trace_enabled()) PsyX_CDFS_SetReadObserver(trace_psycross_read);
}

uint64_t openpete_disc_cache_tag_misses(int tag) {
    if (tag < 0 || tag >= OPENPETE_DISC_TAG_COUNT) return 0;
    dc_lock();
    uint64_t n = s_misses_by_tag[tag];
    dc_unlock();
    return n;
}

void openpete_disc_cache_census(uint64_t* misses, int64_t* direct_ns,
                                size_t* bytes, int64_t* load_ns) {
    dc_lock();
    if (misses)    *misses    = s_misses;
    if (direct_ns) *direct_ns = s_direct_ns;
    if (bytes)     *bytes     = s_size;
    if (load_ns)   *load_ns   = s_load_ns;
    dc_unlock();
}
