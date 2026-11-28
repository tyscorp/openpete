/* core/record/rewind.c - see openpete_rewind.h. */

#include "openpete_rewind.h"
#include "openpete_discontinuity.h"
#include "openpete_input_log.h"
#include "openpete_log.h"
#include "openpete_mods.h"   /* arena immutable check */
#include "openpete_rewind_pages.h"
#include "openpete.h"        /* openpete_rewind_window_set */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

static openpete_rewind_kf_t* s_ring = NULL;
static rwp_table_t* s_tab = NULL;  /* per slot: the keyframe's arena pages */
static int s_ring_n   = 0;   /* capacity (window seconds + 1) */
static int s_head     = 0;   /* next slot to write */
static unsigned s_next_due = 0;  /* frame at/after which the next kf fires */
static int s_registered = 0;

/** @brief Invalidate every keyframe when a savestate load replaces the timeline.
 *
 * Registered with openpete_on_guest_discontinuity by the first enabling
 * openpete_rewind_configure. The keyframes and the input-log suffix they
 * anchor describe the replaced timeline. */
static void rewind_on_guest_discontinuity(void) {
    /* A real savestate load replaced the timeline; every keyframe (and
     * the input-log suffix they anchor) describes the OLD one. */
    openpete_rewind_clear();
}

/* Cost samples (us) of the keyframe copy and of each compare slice, for
 * the exit report's p99 and max. */
typedef struct { uint32_t* v; uint32_t n, cap; } us_samples_t;
static us_samples_t s_us_kf, s_us_arena, s_us_slice;
static uint64_t s_kf_dropped;      /* keyframes dropped by a refused page store */
static uint64_t s_pending_target;  /* targets that were pending: must stay 0 */
static uint64_t s_bad_target;      /* targets invalid or not older than the new keyframe: must stay 0 */
static unsigned s_last_kf;         /* frame of the newest keyframe taken since the clear */
static int      s_have_kf;

/** @brief Append one microsecond sample; a failed growth drops the sample. */
static void us_note(us_samples_t* s, uint64_t ns) {
    if (s->n == s->cap) {
        const uint32_t cap = s->cap ? s->cap * 2u : 1024u;
        uint32_t* v = (uint32_t*)realloc(s->v, cap * sizeof *v);
        if (!v) return;
        s->v = v;
        s->cap = cap;
    }
    s->v[s->n++] = (uint32_t)(ns / 1000u);
}

/** @brief Read the process's resident set in KiB from /proc/self/statm.
 * @return KiB, or 0 where the file does not exist (Windows). */
static unsigned long rss_kib(void) {
    FILE* f = fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, res = 0;
    const int n = fscanf(f, "%lu %lu", &size, &res);
    fclose(f);
    return n == 2 ? res * 4ul : 0ul;
}

/** @brief Invalidate one slot and drop its arena page references. */
static void slot_drop(int i) {
    s_ring[i].valid = 0;
    rwp_release(&s_tab[i]);
}

void openpete_rewind_configure(int window_seconds) {
    const int was_on = s_ring_n > 0;
    openpete_rewind_clear();
    /* Keyframes own their snapshot blobs (~3 MiB each); clear() only
     * invalidates them and releases their arena pages, so a reconfigure
     * frees the blobs and the page store before dropping the ring, or
     * every live resize leaks the whole window. */
    for (int i = 0; i < s_ring_n; i++) {
        if (s_ring[i].mem) openpete_state_mem_free(s_ring[i].mem);
        rwp_table_free(&s_tab[i]);
    }
    free(s_ring);
    free(s_tab);
    s_ring = NULL;
    s_tab = NULL;
    s_ring_n = 0;
    if (was_on) {
        const unsigned long before = rss_kib();
        const uint32_t slabs = rwp_store_trim();
        OPL_INFO_F("rewind", "ring released: page store %u slab(s) (%u MiB) unmapped; "
                   "RSS %lu -> %lu KiB", slabs, slabs * 2u, before, rss_kib());
    }
    if (window_seconds <= 0) return;
    s_ring_n = window_seconds + 1;
    s_ring = (openpete_rewind_kf_t*)calloc((size_t)s_ring_n, sizeof(*s_ring));
    s_tab = (rwp_table_t*)calloc((size_t)s_ring_n, sizeof(*s_tab));
    if (!s_ring || !s_tab) {
        free(s_ring); free(s_tab);
        s_ring = NULL; s_tab = NULL;
        s_ring_n = 0;
        return;
    }
    if (!s_registered) {
        s_registered = 1;
        (void)openpete_on_guest_discontinuity(rewind_on_guest_discontinuity);
    }
    OPL_INFO_F("rewind", "window %d s (%d keyframes x ~3 MiB + shared arena pages)",
               window_seconds, s_ring_n);
}

int openpete_rewind_enabled(void) { return s_ring_n > 0; }

void openpete_rewind_clear(void) {
    for (int i = 0; i < s_ring_n; i++) slot_drop(i);
    s_head = 0;
    s_next_due = 0;
    s_have_kf = 0;
}

/* The intervention epoch and the last intervention's name: written from
 * whichever thread applies an intervention, read by the host loop. */
static OP_ROLE_ATOMIC(unsigned)    s_intervention_epoch;
static OP_ROLE_ATOMIC(const char*) s_intervention_what;

void openpete_rewind_note_intervention(const char* what) {
    atomic_store_explicit(&s_intervention_what, what, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_intervention_epoch, 1u, memory_order_release);
}

unsigned openpete_rewind_intervention_epoch(const char** what) {
    const unsigned ep = atomic_load_explicit(&s_intervention_epoch, memory_order_acquire);
    if (what) *what = atomic_load_explicit(&s_intervention_what, memory_order_relaxed);
    return ep;
}

int openpete_rewind_hold_phase_ready(unsigned frame) {
    static int phase = -2;   /* -2 unread, -1 unset */
    if (phase == -2) {
        const char* e = getenv("OPENPETE_REWIND_HOLD_PHASE");
        phase = e && *e ? atoi(e) : -1;
        if (phase >= 0)
            OPL_INFO_F("rewind", "OPENPETE_REWIND_HOLD_PHASE: the hold starts %d VBL(s) after a keyframe", phase);
    }
    if (phase < 0) return 1;
    if (!s_have_kf || frame - s_last_kf != (unsigned)phase) return 0;
    OPL_INFO_F("rewind", "hold phase met at f=%u (keyframe f=%u, pending compare %u page(s))",
               frame, s_last_kf, rwp_compare_remaining());
    return 1;
}

/** @brief Turn rewind off once at the VBL OPENPETE_REWIND_OFF_AT names, through the live knob path (harness for the page-store release). */
static void off_at_harness(unsigned frame) {
    static int state = -1;   /* -1 unread, 0 off or done, 1 armed */
    static unsigned at;
    if (state < 0) {
        const char* e = getenv("OPENPETE_REWIND_OFF_AT");
        state = e && *e ? 1 : 0;
        if (state) at = (unsigned)strtoul(e, NULL, 10);
    }
    if (state != 1 || frame < at) return;
    state = 0;
    OPL_INFO_F("rewind", "OPENPETE_REWIND_OFF_AT: rewind off at f=%u", frame);
    openpete_rewind_window_set(0);
}

/** @brief Index of the newest valid keyframe other than slot @p self, or -1. */
static int newest_other(int self) {
    int best = -1;
    for (int i = 0; i < s_ring_n; i++) {
        if (i == self || !s_ring[i].valid) continue;
        if (best < 0 || s_ring[i].frame > s_ring[best].frame) best = i;
    }
    return best;
}

/** @brief Run one compare slice: the pages left over the calls left before the next keyframe, about one per tick. */
static void compare_slice(unsigned frame) {
    const uint32_t left = rwp_compare_remaining();
    if (!left) return;
    const unsigned span = s_next_due > frame ? (s_next_due - frame) / 2u : 0u;
    const uint32_t calls = span ? span : 1u;
    const uint64_t t0 = SDL_GetTicksNS();
    (void)rwp_compare_step((left + calls - 1u) / calls);
    us_note(&s_us_slice, SDL_GetTicksNS() - t0);
}

/** @brief Take the keyframe due now into slot s_head: finish the previous compare, release the slot, copy, then start the compare against the newest other valid keyframe. */
static void take_keyframe(const CPUState* cpu, unsigned frame) {
    (void)rwp_compare_step(UINT32_MAX);
    const int h = s_head;
    openpete_rewind_kf_t* kf = &s_ring[h];
    slot_drop(h);
    if (!kf->mem) {
        kf->mem = openpete_state_mem_new();
        if (!kf->mem) return;   /* OOM: ring shrinks by one, logged inside */
        openpete_state_mem_arena_external(kf->mem);
    }
    openpete_mods_arena_immutable_check("rewind keyframe");
    const uint64_t t0 = SDL_GetTicksNS();
    if (openpete_state_mem_save(kf->mem, cpu, frame) != 0) return;
    const uint64_t t1 = SDL_GetTicksNS();
    if (rwp_capture(&s_tab[h]) != 0) {
        /* Dropped whole; the ring keeps the previous keyframes and the
         * next attempt waits a full interval rather than retrying a full
         * copy every tick against a refusing store. */
        if (s_kf_dropped++ == 0)
            OPL_WARN_F("rewind", "keyframe at f=%u dropped: the page store cannot grow", frame);
        s_next_due = frame + OPENPETE_REWIND_KF_VBLS;
        return;
    }
    const uint64_t t2 = SDL_GetTicksNS();
    us_note(&s_us_kf, t2 - t0);
    us_note(&s_us_arena, t2 - t1);
    kf->frame   = frame;
    kf->log_idx = input_log_count();
    kf->valid   = 1;
    s_head = (s_head + 1) % s_ring_n;
    s_next_due = frame + OPENPETE_REWIND_KF_VBLS;
    s_last_kf = frame;
    s_have_kf = 1;
    const int t = newest_other(h);
    if (t >= 0 && s_tab[t].flat) s_pending_target++;
    if (t >= 0 && (!s_ring[t].valid || s_ring[t].frame >= frame)) s_bad_target++;
    rwp_compare_begin(&s_tab[h], t >= 0 ? &s_tab[t] : NULL);
}

void openpete_rewind_on_iter(const CPUState* cpu, unsigned frame, int live) {
    if (!s_ring_n) return;
    off_at_harness(frame);
    if (!live) {
        /* Level transition / loading: the mem snapshot excludes the
         * sections that mutate here - rewinding across is undefined. */
        openpete_rewind_clear();
        return;
    }
    if (s_next_due && frame < s_next_due) {
        compare_slice(frame);
        return;
    }
    take_keyframe(cpu, frame);
}

const openpete_rewind_kf_t* openpete_rewind_pick(unsigned target, const openpete_park_site_t* site) {
    const openpete_rewind_kf_t* best = NULL;
    for (int i = 0; i < s_ring_n; i++) {
        const openpete_rewind_kf_t* kf = &s_ring[i];
        if (!kf->valid || kf->frame > target) continue;
        if (site) {
            const openpete_park_site_t ks = openpete_state_mem_site(kf->mem);
            if (ks.ra != 0u && (ks.ra != site->ra || ks.sp != site->sp)) continue;
        }
        if (!best || kf->frame > best->frame) best = kf;
    }
    return best;
}

int openpete_rewind_restore(const openpete_rewind_kf_t* kf, CPUState* cpu, unsigned* out_frame) OP_REQUIRES(sim) {
    if (!kf || kf < s_ring || kf >= s_ring + s_ring_n || !kf->valid) return -1;
    const int rc = openpete_state_mem_load(kf->mem, cpu, out_frame, 1);
    if (rc == 0) rwp_restore(&s_tab[kf - s_ring]);
    return rc;
}

void openpete_rewind_drop_after(unsigned frame) {
    for (int i = 0; i < s_ring_n; i++)
        if (s_ring[i].valid && s_ring[i].frame > frame) slot_drop(i);
}

/** @brief Sort comparator for uint32_t. */
static int cmp_u32(const void* a, const void* b) {
    const uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return (x > y) - (x < y);
}

/** @brief Return the median, p99 and max of a sample set, sorting it in place. */
static void us_stats(us_samples_t* s, uint32_t* p50, uint32_t* p99, uint32_t* max) {
    *p50 = *p99 = *max = 0;
    if (!s->n) return;
    qsort(s->v, s->n, sizeof *s->v, cmp_u32);
    *p50 = s->v[s->n / 2u];
    *p99 = s->v[(uint32_t)(((uint64_t)s->n * 99u) / 100u)];
    *max = s->v[s->n - 1u];
}

void openpete_rewind_report(void) {
    const rwp_stats_t st = rwp_stats();
    if (!st.captures && !s_kf_dropped) return;
    uint64_t logical = 0;
    int valid = 0;
    for (int i = 0; i < s_ring_n; i++) {
        if (!s_ring[i].valid) continue;
        valid++;
        logical += (uint64_t)s_tab[i].npages * RWP_PAGE_BYTES;
    }
    uint32_t k50, kp99, kmax, a50, ap99, amax, s50, sp99, smax;
    us_stats(&s_us_kf, &k50, &kp99, &kmax);
    us_stats(&s_us_arena, &a50, &ap99, &amax);
    us_stats(&s_us_slice, &s50, &sp99, &smax);
    OPL_INFO_F("rewind", "arena pages: keyframes=%llu dropped=%llu copied=%llu slices=%llu "
               "compared=%llu shared=%llu compares=%llu done=%llu no-target=%llu "
               "pending-target=%llu bad-target=%llu discarded=%llu restores=%llu pending-restores=%llu",
               (unsigned long long)st.captures, (unsigned long long)s_kf_dropped,
               (unsigned long long)st.pages_copied, (unsigned long long)st.slices,
               (unsigned long long)st.pages_compared, (unsigned long long)st.pages_shared,
               (unsigned long long)st.compares_begun, (unsigned long long)st.compares_done,
               (unsigned long long)st.compares_no_target, (unsigned long long)s_pending_target,
               (unsigned long long)s_bad_target,
               (unsigned long long)st.discards, (unsigned long long)st.restores,
               (unsigned long long)st.restores_pending);
    OPL_INFO_F("rewind", "arena memory: %d valid keyframe(s) name %llu KiB, store holds %u KiB "
               "live (peak %u KiB) in %u slab(s) (peak %u); keyframe copy us p50=%u p99=%u "
               "max=%u (n=%u), arena part p50=%u p99=%u max=%u; compare slice us p50=%u p99=%u "
               "max=%u (n=%u)",
               valid, (unsigned long long)(logical / 1024u), st.pages_live * 4u,
               st.pages_live_peak * 4u, st.slabs, st.slabs_peak, k50, kp99, kmax, s_us_kf.n,
               a50, ap99, amax, s50, sp99, smax, s_us_slice.n);
}
