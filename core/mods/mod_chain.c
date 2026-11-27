// core/mods/mod_chain.c - per-address override chains + base() fallthrough.
//
// The ONE override primitive of the modding framework: N mods stack on a guest
// function; the highest-priority override runs first; base() calls the
// next link down and bottoms out at the recompiled original.
//
// Mechanism: every recompiled function's prologue calls
// psx_override_dispatch(cpu, addr) (emitted by the recompiler for all
// functions - the freecam freeze rides the same chokepoint), which
// reaches openpete_mods_dispatch() below. To fall through to the real
// body, base() re-calls the function through call_by_address() with a
// one-shot bypass armed for that addr, so the prologue's mod arm returns
// 0 and the recompiled body runs - the generalization of the in-tree
// re-entrancy-guard pattern in game_extras_impl.c (func_80023AC4 trace).
//
// Chain contents are runtime data: registering and
// removing links needs no re-link or code patching. All chain execution
// happens on the sim thread (guest-call semantics); the TLS frame stack
// is belt-and-braces.
//
// Overlay scoping: every overlay of a game loads at one vaddr window, so
// one chain per address holds links for several overlays. Each link
// carries the overlay it is bound to (0 for an EXE target,
// OPENPETE_MODS_OVERLAY_ANY for an unscoped link); inside the game's
// window the dispatch reads the resident overlay once through the feature
// registry's probe and treats a link bound to another overlay as it treats
// a disabled link.

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include <SDL3/SDL.h> /* SDL_GetCurrentThreadID - the guard thread claim */
#include "openpete_time.h" /* per-mod time instrument */
#include "feature_registry.h" /* resident-overlay probe */
#include "mod_vec.h"
#include "../ui/openpete_imgui_guard.h" /* hook-owner attribution */

#include <stdio.h>  /* snprintf - owner-state table */
#include <stdlib.h> /* getenv - timer dump toggle */
#include <string.h>
#include "openpete_thread_roles.h"

/* Recompiler runtime: dispatch-by-address (psx_runtime.h). */
void call_by_address(CPUState* cpu, uint32_t addr);

int openpete_mods_active = 0;

/* Addresses the chain table's first block holds, and links a chain
 * allocates up front; both grow past these. */
#define MOD_CHAIN_BLOCK      64
#define MOD_CHAIN_LINKS_INIT 16
#define MOD_FRAME_STACK_MAX  32   /* nested chain invocations: a real
                                   * bound, the recursion guard */

typedef struct {
    openpete_mod_override_fn fn;
    const char*              owner_id; /* for diagnostics/attribution */
    int                      disabled; /* live toggle: skipped, not removed
                                        * - re-enable restores in place */
    uint32_t                 overlay;  /* bound overlay id: 0 = EXE target,
                                        * OPENPETE_MODS_OVERLAY_ANY =
                                        * every overlay */
    int                      stat;     /* call-counter row last charged,
                                        * -1 before the first call */
} mod_link_t;

typedef struct {
    uint32_t addr;
    mod_fvec links;   /* mod_link_t, [0] = highest priority */
} mod_chain_t;

/* Chunked: a live frame holds its chain's address across the override
 * body it is running, and that body may register another chain. */
static mod_cvec s_chains;   /* mod_chain_t */

/** @brief Return chain i in registration order.
 *
 * Chains never move, so the pointer stays valid across later registrations.
 * @param i the chain index, below chain_count(). */
static mod_chain_t* chain_at(int i) {
    return (mod_chain_t*)mod_cvec_at(&s_chains, (size_t)i);
}
/** @brief Return the number of registered chains. */
static int chain_count(void) { return (int)mod_cvec_count(&s_chains); }
/** @brief Return the number of links in a chain, disabled ones included. */
static int link_count(const mod_chain_t* c) {
    return (int)mod_fvec_count(&c->links);
}
/** @brief Return link i of a chain, where 0 is the highest priority.
 *
 * The pointer is valid until the next link is added to the chain, which
 * can move the link list.
 * @param c the chain.
 * @param i the link index, below link_count(c). */
static mod_link_t* link_at(const mod_chain_t* c, int i) {
    return (mod_link_t*)mod_fvec_at(&c->links, (size_t)i);
}

/* Number of links across all chains that are NOT disabled. Zero = every
 * registered chain is toggled off, and dispatch must behave exactly like
 * a build with no chains at all: one branch, no table scan, no re-entry
 * through call_by_address. openpete_mods_active deliberately STAYS set
 * (it is also the gate other subsystems read for "a mod could be live
 * this session", and a re-enable must be seen) - this counter is what
 * makes the all-disabled hot path free. */
static int s_live_links = 0;

/* Owner enable state, held OUTSIDE the links so it survives their
 * removal and re-registration. Two paths depend on it:
 *   - hot reload drops a mod's links and re-runs its entry, which
 *     re-adds them; a mod that is toggled OFF must come back dormant.
 *   - a link is inserted at the FRONT of its chain, so its `disabled`
 *     flag must be seeded from its owner, never inherited from whatever
 *     link previously sat at the head (which may belong to an unrelated,
 *     toggled-off mod).
 * Owners never toggled - including engine-internal ones like the content
 * registry's driver link - default to enabled. */
/* Owners the state table allocates up front; it grows past this. */
#define MOD_OWNER_STATE_INIT 32
#define MOD_OWNER_ID_MAX    64 /* mirrors the loader's MOD_ID_MAX */

typedef struct {
    char owner[MOD_OWNER_ID_MAX];
    int  disabled;
} owner_state_t;
static mod_fvec s_owner_state;   /* owner_state_t */

/** @brief Find an owner's recorded enable state.
 *
 * The pointer is valid until the next owner_state_set adds an owner.
 * @param owner_id the mod id.
 * @return the state row, or NULL when the owner was never toggled. */
static owner_state_t* owner_state_find(const char* owner_id) {
    for (size_t i = 0; i < mod_fvec_count(&s_owner_state); i++) {
        owner_state_t* o = (owner_state_t*)mod_fvec_at(&s_owner_state, i);
        if (strcmp(o->owner, owner_id) == 0) return o;
    }
    return NULL;
}

/** @brief Return 1 when an owner's links are toggled off; a NULL or never-toggled owner reads as enabled. */
static int owner_disabled(const char* owner_id) {
    const owner_state_t* o;
    if (!owner_id) return 0;
    o = owner_state_find(owner_id);
    return o ? o->disabled : 0;
}

/** @brief Record an owner's disabled state, adding the owner on first use.
 *
 * The id is stored truncated to MOD_OWNER_ID_MAX - 1 characters. An
 * allocation failure is logged and leaves the owner unrecorded, so its
 * links read as enabled after a reload.
 * @param owner_id the mod id.
 * @param disabled nonzero to record the owner as toggled off. */
static void owner_state_set(const char* owner_id, int disabled) {
    owner_state_t* o = owner_state_find(owner_id);
    if (o) { o->disabled = disabled; return; }
    owner_state_t* slot;
    if (!s_owner_state.elem_size)
        mod_fvec_init(&s_owner_state, sizeof(owner_state_t),
                      MOD_OWNER_STATE_INIT, "chain_owner_state",
                      MOD_OWNER_STATE_INIT);
    slot = (owner_state_t*)mod_fvec_push(&s_owner_state);
    if (!slot) {
        OPL_ERROR_F("mods", "override owner-state allocation failed - "
                    "\"%s\" will re-register its links ENABLED after a "
                    "reload", owner_id);
        return;
    }
    snprintf(slot->owner, MOD_OWNER_ID_MAX, "%s", owner_id);
    slot->disabled = disabled;
}

/* Per-call frame: which link of which chain is currently executing, so
 * base() knows its continuation. Sim-thread only; TLS for safety. */
typedef struct {
    mod_chain_t* chain;
    int          link;
    uint32_t     resident; /* overlay resident at dispatch, or
                            * OPENPETE_MODS_OVERLAY_ANY for an address
                            * outside the overlay window (no filter) */
    op_time_t    child_ns; /* time spent in base() descent - subtracted
                            * so the instrument reports SELF time */
} mod_frame_t;

static _Thread_local mod_frame_t s_frames[MOD_FRAME_STACK_MAX];
static _Thread_local int         s_frame_depth = 0;

/* One-shot bypass: armed by base() bottoming out, consumed by the very
 * next prologue hit on that addr so the recompiled body runs. */
static _Thread_local uint32_t s_bypass_addr = 0;
static _Thread_local int      s_bypass_armed = 0;

/** @brief Find the chain registered for a guest function address.
 *
 * @param addr the function's vaddr.
 * @return the chain, or NULL when none is registered. */
static mod_chain_t* chain_find(uint32_t addr) {
    for (int i = 0; i < chain_count(); i++) {
        mod_chain_t* c = chain_at(i);
        if (c->addr == addr) return c;
    }
    return NULL;
}

/** @brief Return 1 when a link is skipped for a call made with the given resident overlay.
 *
 * A disabled link is skipped everywhere. With a resident filter (an
 * address inside the overlay window), a link bound to neither
 * OPENPETE_MODS_OVERLAY_ANY nor the resident overlay is skipped too.
 * @param l the link.
 * @param resident the frame's resident overlay, or OPENPETE_MODS_OVERLAY_ANY
 *        for no filter. */
static int link_skipped(const mod_link_t* l, uint32_t resident) {
    if (l->disabled) return 1;
    if (resident == OPENPETE_MODS_OVERLAY_ANY) return 0;
    return l->overlay != OPENPETE_MODS_OVERLAY_ANY && l->overlay != resident;
}

/** @brief Return the overlay filter for a dispatch at addr.
 *
 * Outside the registered game's overlay window there is no filter. Inside
 * it the feature registry's resident-overlay probe names the overlay; with
 * no probe registered the resident id reads as 0, so only unscoped links
 * run.
 * @param addr the entered function's vaddr.
 * @return the resident overlay id, or OPENPETE_MODS_OVERLAY_ANY. */
static uint32_t resident_filter(uint32_t addr) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    if (addr < gd->window_lo || addr >= gd->window_hi)
        return OPENPETE_MODS_OVERLAY_ANY;
    uint32_t (*probe)(void) = feature_registry_overlay_probe();
    return probe ? probe() : 0u;
}

/* ── Per-link call counter ──────────────────────────────────────────────
 * One row per (owner, addr, link overlay, resident overlay), so a test can
 * read whether an unscoped link ran while each overlay was resident. Rows
 * outlive their links (hot reload, unload); a link caches the row it last
 * charged, so a call while the resident overlay is unchanged costs one
 * compare. */
#define MOD_STAT_INIT 32

typedef struct {
    char     owner[MOD_OWNER_ID_MAX];
    uint32_t addr;
    uint32_t overlay;
    uint32_t resident;
    uint64_t calls;
} mod_link_stat_t;
static mod_fvec s_stats;   /* mod_link_stat_t */

/** @brief Find or add the counter row for a link run with a resident overlay.
 *
 * @param chain the link's chain.
 * @param l the link.
 * @param resident the frame's resident overlay.
 * @return the row index, or -1 on an allocation failure (the call goes
 *         uncounted). */
static int stat_row(const mod_chain_t* chain, const mod_link_t* l,
                    uint32_t resident) {
    const char* owner = l->owner_id ? l->owner_id : "?";
    for (size_t i = 0; i < mod_fvec_count(&s_stats); i++) {
        const mod_link_stat_t* r = (const mod_link_stat_t*)mod_fvec_at(&s_stats, i);
        if (r->addr == chain->addr && r->overlay == l->overlay &&
            r->resident == resident && strcmp(r->owner, owner) == 0)
            return (int)i;
    }
    if (!s_stats.elem_size)
        mod_fvec_init(&s_stats, sizeof(mod_link_stat_t), MOD_STAT_INIT,
                      "chain_link_stats", MOD_STAT_INIT);
    mod_link_stat_t* r = (mod_link_stat_t*)mod_fvec_push(&s_stats);
    if (!r) return -1;
    snprintf(r->owner, MOD_OWNER_ID_MAX, "%s", owner);
    r->addr     = chain->addr;
    r->overlay  = l->overlay;
    r->resident = resident;
    r->calls    = 0;
    return (int)(mod_fvec_count(&s_stats) - 1);
}

/** @brief Count one call of a link under a resident overlay.
 *
 * @param chain the link's chain.
 * @param l the link; its cached row is updated.
 * @param resident the frame's resident overlay. */
static void stat_count(const mod_chain_t* chain, mod_link_t* l,
                       uint32_t resident) {
    if (l->stat >= 0) {
        mod_link_stat_t* r = (mod_link_stat_t*)mod_fvec_at(&s_stats, (size_t)l->stat);
        if (r->resident == resident) { r->calls++; return; }
    }
    l->stat = stat_row(chain, l, resident);
    if (l->stat >= 0)
        ((mod_link_stat_t*)mod_fvec_at(&s_stats, (size_t)l->stat))->calls++;
}

int openpete_mods_link_calls(openpete_mods_link_calls_fn fn, void* ctx) {
    int n = (int)mod_fvec_count(&s_stats);
    if (!fn) return n;
    for (int i = 0; i < n; i++) {
        const mod_link_stat_t* r = (const mod_link_stat_t*)mod_fvec_at(&s_stats, (size_t)i);
        fn(ctx, r->owner, r->addr, r->overlay, r->resident, r->calls);
    }
    return n;
}

/** @brief Print one counter row to stderr in the link-calls dump format.
 *
 * @param ctx unused.
 * @param owner the link's owner.
 * @param addr the chain address.
 * @param overlay the link's bound overlay.
 * @param resident the resident overlay of the counted calls.
 * @param calls the call count. */
static void link_calls_print(void* ctx, const char* owner, uint32_t addr,
                             uint32_t overlay, uint32_t resident,
                             uint64_t calls) {
    (void)ctx;
    char ov[16], rs[16];
    if (overlay == OPENPETE_MODS_OVERLAY_ANY) snprintf(ov, sizeof ov, "any");
    else snprintf(ov, sizeof ov, "%u", overlay);
    if (resident == OPENPETE_MODS_OVERLAY_ANY) snprintf(rs, sizeof rs, "-");
    else snprintf(rs, sizeof rs, "%u", resident);
    fprintf(stderr, "[mods] link-calls owner=%s addr=0x%08X overlay=%s "
            "resident=%s calls=%llu\n", owner, addr, ov, rs,
            (unsigned long long)calls);
}

/** @brief Print every counter row at process exit (OPENPETE_MOD_LINK_CALLS). */
static void link_calls_dump(void) {
    OP_ROLE_ASSERT(sim);
    openpete_mods_link_calls(link_calls_print, NULL);
}

int openpete_mods_bypass_pending(uint32_t addr) {
    return s_bypass_armed && s_bypass_addr == addr;
}

/* The installed guest-byte guard; NULL on a game that installs none. The
 * stack holds each open stretch's owner and state: open, closed by a
 * suspend (base(), call()), or closed by a nested enter, which its leave
 * reopens. A stretch nested deeper than the stack is not guarded. */
#define MOD_GUARD_STACK_MAX 64
enum { GUARD_OPEN, GUARD_SUSPENDED, GUARD_NESTED };
static const openpete_mods_guest_guard_t* s_guest_guard = NULL;
static const char*  s_guard_owner[MOD_GUARD_STACK_MAX];
static signed char  s_guard_state[MOD_GUARD_STACK_MAX];
static int          s_guard_depth = 0;
static SDL_ThreadID s_guard_thread = 0; /* 0: unclaimed, every thread */

void openpete_mods_set_guest_guard(const openpete_mods_guest_guard_t* guard) {
    s_guest_guard = guard;
}

void openpete_mods_guard_claim_thread(void) {
    s_guard_thread = SDL_GetCurrentThreadID();
}

/** @brief Report whether a bracket on the calling thread reaches the installed guard.
 *
 * @return 1 with a guard installed and the thread unclaimed or claimed by the caller, else 0. */
static int guard_live(void) {
    return s_guest_guard &&
           (s_guard_thread == 0 || s_guard_thread == SDL_GetCurrentThreadID());
}

void openpete_mods_guard_enter(const char* mod_id) {
    if (!guard_live()) return;
    const int top = s_guard_depth - 1;
    s_guard_depth++;
    if (s_guard_depth > MOD_GUARD_STACK_MAX) return;
    if (top >= 0 && s_guard_state[top] == GUARD_OPEN) {
        s_guest_guard->end(s_guard_owner[top]);
        s_guard_state[top] = GUARD_NESTED;
    }
    s_guard_owner[s_guard_depth - 1] = mod_id;
    s_guard_state[s_guard_depth - 1] = GUARD_OPEN;
    s_guest_guard->begin();
}

void openpete_mods_guard_leave(void) {
    if (!guard_live() || s_guard_depth <= 0) return;
    const int top = --s_guard_depth;
    if (top >= MOD_GUARD_STACK_MAX) return;
    if (s_guard_state[top] == GUARD_OPEN) s_guest_guard->end(s_guard_owner[top]);
    if (top > 0 && s_guard_state[top - 1] == GUARD_NESTED) {
        s_guard_state[top - 1] = GUARD_OPEN;
        s_guest_guard->begin();
    }
}

void openpete_mods_guard_suspend(void) {
    if (!guard_live() || s_guard_depth <= 0 || s_guard_depth > MOD_GUARD_STACK_MAX) return;
    const int top = s_guard_depth - 1;
    if (s_guard_state[top] != GUARD_OPEN) return;
    s_guest_guard->end(s_guard_owner[top]);
    s_guard_state[top] = GUARD_SUSPENDED;
}

void openpete_mods_guard_resume(void) {
    if (!guard_live() || s_guard_depth <= 0 || s_guard_depth > MOD_GUARD_STACK_MAX) return;
    const int top = s_guard_depth - 1;
    if (s_guard_state[top] != GUARD_SUSPENDED) return;
    s_guard_state[top] = GUARD_OPEN;
    s_guest_guard->begin();
}

/** @brief Run the first enabled link of a chain at or after idx, or the recompiled original when none is left.
 *
 * base() is run_link(current + 1) with the frame's resident overlay. A
 * link bound to an overlay other than the resident one is skipped
 * (link_skipped), and every run is counted per resident overlay. A link runs inside a pushed frame with
 * the ImGui guard's hook owner set to the link's owner. Its self time
 * (span minus the time credited to its frame's child_ns by its base()
 * descent and nested overrides) is charged to the owner through
 * openpete_mods_time_add, and its full span is credited to the parent
 * frame's child_ns, so an override that wraps an expensive original is
 * billed only for its own code. With no link left, or when the frame stack
 * holds MOD_FRAME_STACK_MAX frames (logged), it arms the one-shot bypass
 * for the chain's addr and calls the function through call_by_address, so
 * the prologue's dispatch returns 0 and the recompiled body runs.
 * @param cpu the guest CPU.
 * @param chain the chain.
 * @param idx the first link index to consider.
 * @param resident the overlay filter (resident_filter) the frame carries. */
static void run_link(CPUState* cpu, mod_chain_t* chain, int idx,
                     uint32_t resident) {
    /* Live toggle and overlay scope: a skipped link is transparent -
     * base() descends straight past it. */
    while (idx < link_count(chain) && link_skipped(link_at(chain, idx), resident))
        idx++;
    const op_time_t t0 = op_now();
    if (idx < link_count(chain)) {
        if (s_frame_depth >= MOD_FRAME_STACK_MAX) {
            OPL_ERROR_F("mods", "override frame stack overflow at 0x%08X "
                        "(recursion through %d chained calls?) - running "
                        "original body", chain->addr, s_frame_depth);
        } else {
            mod_frame_t* f = &s_frames[s_frame_depth++];
            f->chain = chain;
            f->link  = idx;
            f->resident = resident;
            f->child_ns = 0;
            stat_count(chain, link_at(chain, idx), resident);
            /* Names the mod in an ImGui refusal: an override runs on
             * the sim thread, outside any UI section body, so the guard has
             * no mod index. Restored rather than cleared, since a link's
             * base() descent re-enters here for the next owner. */
            const char* owner_save = openpete_imgui_guard_hook_owner();
            /* Read out before the call: the body may register a link on
             * this same chain, which moves the link list and shifts this
             * link's index. */
            const char* owner = link_at(chain, idx)->owner_id;
            openpete_mod_override_fn fn = link_at(chain, idx)->fn;
            openpete_imgui_guard_set_hook_owner(owner);
            openpete_mods_guard_enter(owner);
            fn(cpu);
            openpete_mods_guard_leave();
            openpete_imgui_guard_set_hook_owner(owner_save);
            const op_time_t span = op_now() - t0;
            s_frame_depth--;
            openpete_mods_time_add(owner, span - f->child_ns, 0);
            if (s_frame_depth > 0)
                s_frames[s_frame_depth - 1].child_ns += span;
            return;
        }
    }
    /* Chain exhausted (or overflow fallback): call the original through
     * the dispatch switch. The callee's prologue re-enters
     * openpete_mods_dispatch, sees the armed bypass, and returns 0 so
     * the recompiled body executes. */
    s_bypass_addr  = chain->addr;
    s_bypass_armed = 1;
    call_by_address(cpu, chain->addr);
    /* Normally consumed at the prologue; clear defensively in case
     * another dispatch arm (freecam freeze) swallowed the call before our
     * arm saw it. */
    s_bypass_armed = 0;
    if (s_frame_depth > 0)
        s_frames[s_frame_depth - 1].child_ns += op_now() - t0;
}

/* Removes and compacts; documented in openpete_mods.h. */
void openpete_mods_chain_remove_owner(const char* owner_id) {
    if (!owner_id) return;
    for (int i = 0; i < chain_count(); i++) {
        mod_chain_t* c = chain_at(i);
        int w = 0;
        for (int j = 0; j < link_count(c); j++) {
            mod_link_t* l = link_at(c, j);
            if (l->owner_id && strcmp(l->owner_id, owner_id) == 0) {
                if (!l->disabled) s_live_links--;
                continue;
            }
            *link_at(c, w++) = *l;
        }
        mod_fvec_truncate(&c->links, (size_t)w);
    }
}

int openpete_mods_chain_has_owner(uint32_t addr, const char* owner_id) {
    if (!owner_id) return 0;
    for (int i = 0; i < chain_count(); i++) {
        mod_chain_t* c = chain_at(i);
        if (c->addr != addr) continue;
        for (int j = 0; j < link_count(c); j++) {
            const mod_link_t* l = link_at(c, j);
            if (l->owner_id && strcmp(l->owner_id, owner_id) == 0) return 1;
        }
    }
    return 0;
}

/* Live toggle: flip every link owned by owner_id. Chain STRUCTURE is
 * untouched (order, base() nesting) - a re-enabled mod is exactly where
 * it was. Call only at a tick boundary (the loader's apply point). */
void openpete_mods_chain_set_owner_enabled(const char* owner_id, int on) {
    if (!owner_id) return;
    const int want = !on;
    owner_state_set(owner_id, want); /* outlives the links themselves */
    for (int i = 0; i < chain_count(); i++)
        for (int j = 0; j < link_count(chain_at(i)); j++) {
            mod_link_t* l = link_at(chain_at(i), j);
            if (!l->owner_id || strcmp(l->owner_id, owner_id) != 0) continue;
            if (l->disabled == want) continue;
            l->disabled = want;
            s_live_links += want ? -1 : 1;
        }
}

/* ── Per-mod time instrument ────────────────────────────────────────────
 * Accumulates SELF time per owner across tick hooks (chain links, fed by
 * run_link above) and present hooks (mod_draw.c). Reported + reset every
 * ~17 s of sim by the present flush: always WARNS when a mod's average
 * tick cost exceeds the budget (names the slow mod - the sim-thread
 * contract made socially enforceable); full dump under
 * OPENPETE_MOD_TIMERS=1. */

/* Slots the report table allocates up front; it grows past this. */
#define MOD_TIME_INIT      24
#define MOD_TICK_BUDGET_MS 4.0

typedef struct {
    const char* owner;
    op_time_t   tick_ns, present_ns;
    uint32_t    tick_calls, present_calls;
} mod_time_slot_t;

static mod_fvec s_times;   /* mod_time_slot_t, one per owner seen */

void openpete_mods_time_add(const char* owner, op_time_t ns, int is_present) {
    if (!owner || ns < 0) return;
    mod_time_slot_t* t = NULL;
    for (size_t i = 0; i < mod_fvec_count(&s_times); i++) {
        mod_time_slot_t* c = (mod_time_slot_t*)mod_fvec_at(&s_times, i);
        if (c->owner == owner || strcmp(c->owner, owner) == 0) { t = c; break; }
    }
    if (!t) {
        if (!s_times.elem_size)
            mod_fvec_init(&s_times, sizeof(mod_time_slot_t), MOD_TIME_INIT,
                          "mod_time", MOD_TIME_INIT);
        t = (mod_time_slot_t*)mod_fvec_push(&s_times);
        if (!t) {
            OPL_ERROR_F("mods", "timer table allocation failed - %s is "
                        "absent from the time report", owner);
            return;
        }
        t->owner = owner;
    }
    if (is_present) { t->present_ns += ns; t->present_calls++; }
    else            { t->tick_ns    += ns; t->tick_calls++;    }
}

void openpete_mods_time_report(unsigned ticks_elapsed) {
    /* Re-read at every report (every 512 ticks): setenv is the live channel. */
    const char* mt_env = getenv("OPENPETE_MOD_TIMERS");
    int s_dump = (mt_env && *mt_env && *mt_env != '0') ? 1 : 0;
    if (!ticks_elapsed) ticks_elapsed = 1;
    for (size_t i = 0; i < mod_fvec_count(&s_times); i++) {
        mod_time_slot_t* t = (mod_time_slot_t*)mod_fvec_at(&s_times, i);
        double tick_ms = (double)t->tick_ns / OP_NS_PER_MS
                       / (double)ticks_elapsed;
        if (tick_ms > MOD_TICK_BUDGET_MS)
            OPL_WARN_F("mods", "%s: %.2f ms/tick average over the last "
                       "window (budget %.1f) - this mod is slowing the "
                       "sim thread", t->owner, tick_ms, MOD_TICK_BUDGET_MS);
        else if (s_dump) /* dump 0-call windows too: a live-toggled-off
                          * mod's silence is exactly the signal */
            OPL_INFO_F("mods", "timer %s: %.3f ms/tick (%u calls), "
                       "%.3f ms total present (%u hooks)",
                       t->owner, tick_ms, t->tick_calls,
                       (double)t->present_ns / OP_NS_PER_MS,
                       t->present_calls);
        t->tick_ns = t->present_ns = 0;
        t->tick_calls = t->present_calls = 0;
    }
}

int openpete_mods_dispatch(CPUState* cpu, uint32_t addr) {
    if (s_bypass_armed && s_bypass_addr == addr) {
        s_bypass_armed = 0; /* base() bottoming out: run the real body */
        return 0;
    }
    if (s_live_links == 0) return 0;
    mod_chain_t* chain = chain_find(addr);
    if (!chain) return 0;
    const uint32_t resident = resident_filter(addr);
    int idx = 0;
    while (idx < link_count(chain) && link_skipped(link_at(chain, idx), resident))
        idx++;
    /* Every link toggled off or bound to another overlay: run the
     * recompiled body IN PLACE. Entering
     * run_link here would still produce the right result (it bottoms out
     * on the original), but only by re-calling the function through
     * call_by_address - a second pass over every dispatch arm of the
     * prologue (freecam freeze, feature registry) for a
     * call no mod is servicing. */
    if (idx >= link_count(chain)) return 0;
    run_link(cpu, chain, idx, resident);
    return 1;
}

/** @brief Run the next override below the running one, bottoming out at the recompiled original.
 *
 * The mod api's base(). It continues from the link the innermost live
 * frame is executing with that frame's resident overlay, skipping
 * disabled links and links bound to another overlay. Refused with an error log
 * and no effect inside a present hook (openpete_mods_in_present set) and
 * when no override is running on this thread.
 * @param cpu the guest CPU passed to the running override. */
void openpete_mods_base(CPUState* cpu) {
    if (openpete_mods_in_present) {
        OPL_ERROR_F("mods", "base() refused inside a present hook "
                    "(render-pure context)");
        return;
    }
    if (s_frame_depth <= 0) {
        OPL_ERROR_F("mods", "base() called outside an override - ignored");
        return;
    }
    mod_frame_t f = s_frames[s_frame_depth - 1];
    /* The calling override's code stops here and resumes after the
     * continuation returns: its stretch is closed and reopened around the
     * guest code below it, which may store into guarded bytes itself. */
    openpete_mods_guard_suspend();
    run_link(cpu, f.chain, f.link + 1, f.resident);
    openpete_mods_guard_resume();
}

/* Reads this thread's frame stack; documented in openpete_mods.h. */
const char* openpete_mods_crash_attribution(uint32_t* addr_out) {
    if (s_frame_depth <= 0) return NULL;
    const mod_frame_t* f = &s_frames[s_frame_depth - 1];
    if (addr_out) *addr_out = f->chain->addr;
    if (f->link < link_count(f->chain) &&
        link_at(f->chain, f->link)->owner_id)
        return link_at(f->chain, f->link)->owner_id;
    return "?";
}

/** @brief Register an override link at the front of the chain for a guest function.
 *
 * The loader loads mods in enabled-list order and each new link goes to
 * the front, so the last-enabled mod runs first and base() walks toward
 * earlier-enabled mods, then the original: list order is priority. A mod
 * entered late (deferred entry) therefore runs first until a restart. The
 * link's disabled flag is taken from its owner's live-toggle state. Live
 * frames on the chain have their link index advanced past the insert, so
 * registration from inside a running chain keeps base() pointing at the
 * next link. Sets openpete_mods_active and reports the link to
 * openpete_mods_link_registered. With OPENPETE_MOD_LINK_CALLS set,
 * the first registration arms a stderr dump of every call-counter row at
 * process exit.
 * @param addr the guest function's vaddr; 0 fails.
 * @param overlay the overlay the link is bound to: 0 for an EXE target,
 *        an overlay id, or OPENPETE_MODS_OVERLAY_ANY.
 * @param fn the override; NULL fails.
 * @param owner_id the registering mod's id, or NULL.
 * @return 0, or -1 for a 0 addr, a NULL fn, or an allocation failure. */
int openpete_mods_chain_add_ovl(uint32_t addr, uint32_t overlay,
                                openpete_mod_override_fn fn,
                                const char* owner_id) {
    if (addr == 0 || !fn) return -1;
    if (getenv("OPENPETE_MOD_LINK_CALLS")) {
        static int s_dump_armed = 0;
        if (!s_dump_armed) { s_dump_armed = 1; atexit(link_calls_dump); }
    }
    mod_chain_t* chain = chain_find(addr);
    if (!chain) {
        if (!s_chains.elem_size)
            mod_cvec_init(&s_chains, sizeof(mod_chain_t), MOD_CHAIN_BLOCK,
                          "chains", MOD_CHAIN_BLOCK);
        chain = (mod_chain_t*)mod_cvec_push(&s_chains);
        if (!chain) {
            OPL_ERROR_F("mods", "override chain table allocation failed - "
                        "cannot hook 0x%08X for %s", addr,
                        owner_id ? owner_id : "?");
            return -1;
        }
        chain->addr = addr;
        mod_fvec_init(&chain->links, sizeof(mod_link_t),
                      MOD_CHAIN_LINKS_INIT, NULL, 0);
    }
    /* Append, then rotate the new link to the front: the list is in
     * priority order and the last registration is the highest. */
    if (!mod_fvec_push(&chain->links)) {
        OPL_ERROR_F("mods", "override chain at 0x%08X: link allocation "
                    "failed - cannot add %s", addr,
                    owner_id ? owner_id : "?");
        return -1;
    }
    for (int j = link_count(chain) - 1; j > 0; j--)
        *link_at(chain, j) = *link_at(chain, j - 1);
    link_at(chain, 0)->fn       = fn;
    link_at(chain, 0)->owner_id = owner_id;
    link_at(chain, 0)->overlay  = overlay;
    link_at(chain, 0)->stat     = -1;
    /* Seed from the OWNER, never from the slot: the memmove leaves the
     * previous head link's bytes in [0], so an inherited flag would make
     * a fresh link dead (registering behind a toggled-off mod) or live
     * (a toggled-off mod re-registering on hot reload). */
    link_at(chain, 0)->disabled = owner_disabled(owner_id);
    if (!link_at(chain, 0)->disabled) s_live_links++;
    /* The insert shifted every existing link up one slot, but live
     * frames hold link INDICES. Registration from inside a running chain
     * is a normal path (the content registry's on_level hooks fire from
     * its own driver link), and without this re-anchor base() would
     * re-enter the link that is currently executing. */
    for (int f = 0; f < s_frame_depth; f++)
        if (s_frames[f].chain == chain) s_frames[f].link++;
    openpete_mods_active = 1;
    openpete_mods_link_registered(owner_id, addr, overlay);
    return 0;
}

/** @brief Register an unscoped override link (OPENPETE_MODS_OVERLAY_ANY) at the front of the chain for a guest function.
 *
 * The engine's own links (the content registry's driver, the level ops'
 * aliases) use it; a mod's links bind through openpete_mods_chain_add_ovl.
 * @param addr the guest function's vaddr; 0 fails.
 * @param fn the override; NULL fails.
 * @param owner_id the registering owner's id, or NULL.
 * @return 0, or -1 as openpete_mods_chain_add_ovl. */
int openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                            const char* owner_id) {
    return openpete_mods_chain_add_ovl(addr, OPENPETE_MODS_OVERLAY_ANY, fn,
                                       owner_id);
}
