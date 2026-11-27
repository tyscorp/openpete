// core/mods/mod_draw.c - the mod-facing screen-space draw service.
//
// The native draw/overlay service, vk-renderer-first: mods
// draw HUD text through the native renderer, never guest-side, so
// an observer/HUD mod stays sim-neutral by construction (zero guest-RAM
// writes; guest prim injection provably leaks - the 16:9
// particle_drawn_flag regression).
//
// Two submission contexts:
//   - sim-tick hooks (inside overrides): commands land in a staging
//     buffer; the canonical present commits it for display across ALL of
//     that tick's presents (immediate mode per tick - draw every tick or
//     it disappears).
//   - present hooks (OPENPETE mod api register_present_hook): run once
//     per present on the host fiber with {tick, alpha, dt, aspect};
//     commands live for that present only. Mods evaluate their own
//     curves at alpha - the engine never interpolates mod visuals.
//
// Render-purity is enforced by API shape: while a present hook runs,
// openpete_mods_in_present is set and the loader's guest/call/override
// entry points refuse with an error (presents are non-deterministic in
// count/timing; sim-affecting work belongs in tick hooks).
//
// Text is rasterized by a GAME-registered emitter (Spyro: the 3D gold
// text-moby glyphs via text_moby_native_emit - digits, A-Z, limited
// punctuation). Coordinates: overlay space = window-height fractions,
// origin top-left, +y down, x spans [0, aspect].

#include "openpete_mods.h"
#include "openpete_mod_api.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"   /* g_openpete_tick_period_ns */
#include "openpete_time.h"
#include "openpete_fiber.h" /* openpete_frame */
#include "../ui/openpete_imgui_guard.h" /* hook-owner attribution */
#include "mod_vec.h"
#include "game_extras.h"      /* game_get_id: unsupported-surface lines */
#include "openpete_psx.h"         /* openpete_psx_get_ram: the fault injection */
#include "openpete_subtick_view.h" /* the RAM guard window */
#include "openpete_thread_roles.h"  /* OP_ONCE_INT */

#include <SDL3/SDL.h>   /* SDL_GetCurrentThreadID - the single-owner check */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Texts a command buffer allocates up front; it grows past this. */
#define DRAW_TEXTS_INIT 64
#define DRAW_STR_MAX    96
/* Hooks the table allocates up front; it grows past this. */
#define PRESENT_HOOKS_INIT 16

typedef struct {
    float       x, y, cap_h;
    uint32_t    rgb;
    const char* owner;   /* mod id of the hook that submitted this text */
    char        str[DRAW_STR_MAX];
} cmd_text_t;

typedef struct {
    mod_fvec texts;      /* cmd_text_t */
    int      inited;
} cmd_buf_t;

/* The tick double buffer. Staging takes this tick's draws; committed is
 * what the presents of this tick display. The canonical commit exchanges
 * the two by pointer, so neither buffer's heap block is ever aliased or
 * dropped, and clears the new staging. */
static cmd_buf_t  s_tick_bufs[2];
static cmd_buf_t* s_tick_staging   = &s_tick_bufs[0];
static cmd_buf_t* s_tick_committed = &s_tick_bufs[1];
static cmd_buf_t  s_present;       /* rebuilt inside each present hook  */

/** @brief Initialize a command buffer's text list once.
 *
 * @param b the buffer; a second call does nothing.
 * @param name the list's name for the allocation census. */
static void buf_init(cmd_buf_t* b, const char* name) {
    if (b->inited) return;
    b->inited = 1;
    mod_fvec_init(&b->texts, sizeof(cmd_text_t), DRAW_TEXTS_INIT, name,
                  DRAW_TEXTS_INIT);
}

/** @brief Return the number of texts a command buffer holds. */
static int buf_count(const cmd_buf_t* b) {
    return (int)mod_fvec_count(&b->texts);
}

/** @brief Drop a buffer's text commands and keep its storage.
 *
 * The buffers refill every tick or present, so the allocation is paid once
 * per run.
 * @param b Buffer to clear. */
static void buf_clear(cmd_buf_t* b) { mod_fvec_clear(&b->texts); }

/** @brief Latch the first thread to swap the tick buffers and log an error once if a later swap runs on another thread.
 *
 * The draw service has one owner thread: registration, removal,
 * submission and the flush run on the worker thread that owns the game
 * fiber. The tick buffers are exchanged by pointer and a mod pushes into
 * staging from a hook, so a submission on a second thread could be in
 * flight across the swap; this check is the only signal of that. Called
 * at each canonical commit. */
static void assert_one_owner(void) {
    static SDL_ThreadID s_owner_tid = 0;
    static int s_reported = 0;
    SDL_ThreadID tid = SDL_GetCurrentThreadID();
    if (!s_owner_tid) { s_owner_tid = tid; return; }
    if (tid != s_owner_tid && !s_reported) {
        s_reported = 1;
        OPL_ERROR_F("mods", "draw service: tick buffers exchanged from a "
                    "second thread (owner %llu, now %llu) - a mod's text "
                    "can be lost or double-freed",
                    (unsigned long long)s_owner_tid,
                    (unsigned long long)tid);
    }
}

/* Per thread: present hooks run on the thread that extracts while a mod UI
 * section can run inside a compose on a present thread; one shared flag
 * would let either clear the other's purity guard. */
OPENPETE_THREAD_LOCAL int openpete_mods_in_present = 0;


typedef struct {
    openpete_mod_present_fn fn;
    const char*             owner_id;
    int                     disabled; /* live toggle */
} present_hook_t;

/* Flat, and safe to move: registration, removal and the flush below all
 * run on the worker thread that owns the game fiber, so the table has
 * one owner and no reader can be inside it across a growth. The swap's
 * assert_one_owner() above latches that thread. */
static mod_fvec s_phooks;   /* present_hook_t */

/** @brief Return the number of registered present hooks, disabled ones included. */
static int phook_count(void) { return (int)mod_fvec_count(&s_phooks); }
/** @brief Return present hook i in registration order.
 *
 * The pointer is valid until the next registration, which can move the
 * table.
 * @param i the hook index, below phook_count(). */
static present_hook_t* phook_at(int i) {
    return (present_hook_t*)mod_fvec_at(&s_phooks, (size_t)i);
}
static int            s_any_draw = 0; /* fast bail for modless runs */

/* Last present's ctx (tick/alpha/canonical) - the postfx engine UBO
 * reads these through openpete_mods_present_ctx (same source as the
 * present-hook ctx, so mods and shaders can never disagree on phase). */
static uint32_t s_pctx_tick;
static float    s_pctx_alpha;
static int      s_pctx_canonical = 1;

void openpete_mods_present_ctx(uint32_t* tick, float* alpha, int* canonical) {
    if (tick) *tick = s_pctx_tick;
    if (alpha) *alpha = s_pctx_alpha;
    if (canonical) *canonical = s_pctx_canonical;
}

void openpete_mods_draw_remove_owner(const char* owner_id) {
    if (!owner_id) return;
    int w = 0;
    for (int i = 0; i < phook_count(); i++) {
        present_hook_t* h = phook_at(i);
        if (h->owner_id && strcmp(h->owner_id, owner_id) == 0) continue;
        *phook_at(w++) = *h;
    }
    mod_fvec_truncate(&s_phooks, (size_t)w);
}

void openpete_mods_draw_set_owner_enabled(const char* owner_id, int on) {
    if (!owner_id) return;
    for (int i = 0; i < phook_count(); i++) {
        present_hook_t* h = phook_at(i);
        if (h->owner_id && strcmp(h->owner_id, owner_id) == 0)
            h->disabled = !on;
    }
}

/** @brief Return whether a present hook registered now by an owner starts disabled.
 *
 * An owner the loader lists starts in its current live-toggle state. Hot
 * reload sweeps a mod's registrations and re-runs its entry without
 * consulting the toggle state, so without this a disabled mod's
 * re-registered hook would present again. At boot the loader pushes a
 * mod's row, with its boot enabled state, before running its entry, so
 * boot registrations read their final state. A deferred-entry mod's first
 * enable runs its entry while its row still reads disabled: the hook
 * starts disabled and the apply's openpete_mods_draw_set_owner_enabled(id,
 * 1) after the entry enables it. A NULL owner or one the loader does not
 * list starts enabled.
 * @param owner_id the registering mod's id, or NULL.
 * @return 1 when the hook starts disabled, 0 otherwise. */
static int owner_starts_disabled(const char* owner_id) {
    if (!owner_id) return 0;
    const int n = openpete_mods_count();
    for (int i = 0; i < n; i++) {
        const char* id = openpete_mods_id(i);
        if (id && strcmp(id, owner_id) == 0)
            return !openpete_mods_is_enabled(owner_id);
    }
    return 0;
}

int openpete_mods_register_present_hook(openpete_mod_present_fn fn,
                                        const char* owner_id) {
    if (!fn) return -1;
    present_hook_t* h;
    if (!s_phooks.elem_size)
        mod_fvec_init(&s_phooks, sizeof(present_hook_t), PRESENT_HOOKS_INIT,
                      "present_hooks", PRESENT_HOOKS_INIT);
    h = (present_hook_t*)mod_fvec_push(&s_phooks);
    if (!h) {
        OPL_ERROR_F("mods", "present-hook table allocation failed - cannot "
                    "add %s", owner_id ? owner_id : "?");
        return -1;
    }
    h->fn = fn;
    h->owner_id = owner_id;
    h->disabled = owner_starts_disabled(owner_id);
    s_any_draw = 1;
    return 0;
}

/* ---- command submission (mod-facing, via the api struct) ------------ */

/** @brief Return the buffer a text submitted now goes to.
 *
 * @return this present's buffer while a present hook runs on this thread,
 *         else the tick staging buffer. */
static cmd_buf_t* target_buf(void) {
    return openpete_mods_in_present ? &s_present : s_tick_staging;
}

/** @brief Log once per owner that the running game serves no text rasterizer.
 *
 * Owners are matched by pointer (the loader's session-lifetime id); the
 * first 32 are remembered, and an owner past that logs on every call. A
 * draw with no published owner is reported under "?".
 * @param owner The submitting mod's id, or NULL. */
static void note_text_unsupported(const char* owner) {
    static const char* s_noted[32];
    static int s_noted_n = 0;
    for (int i = 0; i < s_noted_n; i++)
        if (s_noted[i] == owner) return;
    if (s_noted_n < (int)(sizeof s_noted / sizeof s_noted[0]))
        s_noted[s_noted_n++] = owner;
    OPL_WARN_F("mods", "%s: draw_text unsupported on %s (no text "
               "rasterizer) - text dropped", owner ? owner : "?",
               game_get_id());
}

void openpete_mods_draw_text(float x, float y, float cap_h, uint32_t rgb,
                             const char* str) {
    if (!str || !str[0]) return;
    if (!openpete_mods_game_desc()->text_emit) {
        note_text_unsupported(openpete_imgui_guard_hook_owner());
        return;
    }
    cmd_buf_t* b = target_buf();
    cmd_text_t* t;
    buf_init(b, b == &s_present ? "draw_text_present"
                                : (b == &s_tick_bufs[0] ? "draw_text_tick_a"
                                                        : "draw_text_tick_b"));
    t = (cmd_text_t*)mod_fvec_push(&b->texts);
    if (!t) {
        OPL_ERROR_F("mods", "draw service: text list allocation failed - "
                    "\"%.32s\" dropped", str);
        return;
    }
    t->x = x; t->y = y; t->cap_h = cap_h; t->rgb = rgb;
    /* The hook dispatch publishes the running mod for attribution; a
     * draw from anywhere else has no owner to name. */
    t->owner = openpete_imgui_guard_hook_owner();
    snprintf(t->str, sizeof t->str, "%s", str);
    s_any_draw = 1;
}

/* ---- per-present flush (called from run.c's two present legs) -------- */

#define MOD_DRAW_VERTS_MAX 16384
static openpete_hud_vert s_verts[MOD_DRAW_VERTS_MAX];

/** @brief Warn once per run that a mod's text overflowed the overlay vertex budget.
 *
 * The vertex stream is a real bound (the renderer's buffer is sized to
 * MOD_DRAW_VERTS_MAX), so the remedy is the mod drawing less, and repeating
 * the line every present would bury it. Owners are matched by pointer; the
 * first 32 are remembered, and an owner past that warns on every overflow.
 * @param owner Mod id of the overflowing text; NULL logs as "?". */
static void warn_glyph_overflow(const char* owner) {
    static const char* s_warned[32];
    static int s_warned_n = 0;
    const char* id = owner ? owner : "?";
    for (int i = 0; i < s_warned_n; i++)
        if (s_warned[i] == owner) return;
    if (s_warned_n < (int)(sizeof s_warned / sizeof s_warned[0]))
        s_warned[s_warned_n++] = owner;
    OPL_WARN_F("mods", "draw service: %s filled the overlay vertex budget "
               "(%d verts) - the rest of its text this present is dropped",
               id, MOD_DRAW_VERTS_MAX);
}

/** @brief Lay out a command buffer's texts into the overlay vertex array after the first n vertices.
 *
 * Each text goes through the registered emitter with the space left in
 * the array. A text that writes no vertices after an earlier text in this
 * call has written some is reported as an overflow drop.
 * @param b the buffer to lay out.
 * @param n the vertices already used.
 * @return the new vertex count; n when no emitter is registered. */
static int emit_buf(const cmd_buf_t* b, int n) {
    /* A text that laid out no glyphs at all is the drop this reports. The
     * emitter refuses a glyph only when the remaining budget cannot hold
     * it, so once the budget is full every later text writes nothing -
     * but it writes nothing for a missing font too, which is why the
     * report waits until some text in this flush has laid out glyphs.
     * The one text straddling the boundary keeps the glyphs that fit and
     * is not reported: it drew. */
    int emitted_any = 0;
    const openpete_mods_text_emit_fn text_fn =
        openpete_mods_game_desc()->text_emit;
    if (!text_fn) return n;
    for (int i = 0; i < buf_count(b); i++) {
        const cmd_text_t* t = (const cmd_text_t*)mod_fvec_at(&b->texts,
                                                             (size_t)i);
        int avail = MOD_DRAW_VERTS_MAX - n;
        int wrote = avail > 0
                  ? text_fn(t->str, t->x, t->y, t->cap_h, t->rgb,
                              &s_verts[n], avail)
                  : 0;
        n += wrote;
        if (wrote > 0) { emitted_any = 1; continue; }
        if (emitted_any) warn_glyph_overflow(t->owner);
    }
    return n;
}

/** @brief Run the draw half of a present flush: the tick-buffer commit, the present hooks and the overlay layout.
 *
 * Everything openpete_mods_present_flush does after the tick boundary; its
 * caller brackets it in the RAM guard window.
 * @param canonical 1 for a tick's canonical present, 0 for a sub-tick
 *        present. */
static void present_flush_draw(int canonical) OP_REQUIRES(sim) {
    if (!s_any_draw) return;

    /* Commit the tick buffer once per canonical present: the sim tick
     * that just ran wrote staging; it becomes the displayed set for this
     * tick's presents (and clears - immediate mode per tick).
     *
     * ABOVE the renderer gate, so staging drains even when nothing
     * consumes it (--renderer og, or any run without a native renderer).
     * An undrained staging buffer pins every command ever submitted: the
     * text cap fills for the rest of the session (further draw
     * calls fail) and the whole backlog would replay at once if the
     * renderer came up.
     *
     * Live toggle: the caller's tick boundary applies before this commit,
     * so a mod disabled at this boundary still displays the tick it drew
     * while enabled - then goes quiet. Its hooks no longer run, staging stays
     * empty, and the next canonical commit clears the display set;
     * nothing of a disabled mod survives past that one tick. */
    if (canonical) {
        cmd_buf_t* prev = s_tick_committed;
        assert_one_owner();
        s_tick_committed = s_tick_staging;
        s_tick_staging = prev;
        buf_clear(s_tick_staging);
    }

    if (!openpete_native_render_enabled()) return;

    /* Present hooks: render-pure by API shape (see header comment). The
     * ctx is computed unconditionally - the postfx engine UBO reads the
     * same values through openpete_mods_present_ctx, hooks or not. */
    buf_clear(&s_present);
    {
        static _Atomic(op_time_t) s_prev_present_ns = 0;
        const op_time_t now = openpete_interp_present_now();
        openpete_present_ctx_t ctx;
        ctx.tick   = openpete_frame;
        ctx.alpha  = 0.0f;
        const op_time_t prev_ns = s_prev_present_ns;
        ctx.dt     = (prev_ns && now > prev_ns)
                       ? (float)((double)(now - prev_ns) / 1e9)
                       : 0.0f;
        ctx.aspect = openpete_native_overlay_aspect();
        s_prev_present_ns = now;

        /* Sub-tick phase between the last two sim snapshots - the same
         * query the high-fps spine uses. 0 on canonical presents / when
         * interpolation isn't running. */
        double a = 0.0;
        const openpete_interp_snapshot_t *prev, *curr;
        /* The canonical tick period, matching what run.c paces to - never a
         * local literal, or a mod's alpha drifts against the engine's. */
        const op_time_t sim_period = g_openpete_tick_period_ns;
        if (!canonical &&
            openpete_interp_query(now, sim_period, &a, &prev, &curr))
            ctx.alpha = (float)a;

        s_pctx_tick = ctx.tick;
        s_pctx_alpha = ctx.alpha;
        s_pctx_canonical = canonical ? 1 : 0;

        /* After the exit pass no mod code runs: a present the teardown
         * still makes calls no hook. */
        if (phook_count() > 0 && !openpete_mods_api_closed()) {
        openpete_mods_in_present = 1;
        for (int i = 0; i < phook_count(); i++) {
            const present_hook_t* h = phook_at(i);
            if (h->disabled) continue;
            const op_time_t t0 = op_now(); /* clock-guard: wall */ /* hook cost span */
            /* Names the mod in an ImGui refusal: a present hook runs
             * outside any UI section body, so the guard has no mod index.
             * The owner is read out before the call: a hook body that
             * registers another hook moves the table. */
            const char* owner = h->owner_id;
            openpete_mod_present_fn fn = h->fn;
            openpete_imgui_guard_set_hook_owner(owner);
            fn(&ctx);
            openpete_imgui_guard_set_hook_owner(NULL);
            openpete_mods_time_add(owner, op_now() - t0, 1); /* clock-guard: wall */
        }
        openpete_mods_in_present = 0;
        }
    }

    int n = 0;
    n = emit_buf(s_tick_committed, n);
    n = emit_buf(&s_present, n);
    openpete_native_set_mod_overlay(s_verts, n);

    /* One-shot diagnostics: confirm the pipeline end-to-end on first use. */
    static atomic_int s_logged_first;
    if (!s_logged_first && n > 0) {
        s_logged_first = 1;
        OPL_INFO_F("mods", "draw service live: %d overlay verts "
                   "(%d tick texts, %d present texts)",
                   n, buf_count(s_tick_committed), buf_count(&s_present));
    }
}

/** @brief Fault injection: rewrite one guest word through a host pointer inside the flush's guard window.
 *
 * Reads OPENPETE_MODS_FLUSH_FAULT_STORE on the first call and does nothing
 * unless it is set. The word at RAM offset 0x10000 is stored with the value
 * it holds, the shape of a mod hook that writes gameplay memory and restores
 * it: no hashed byte changes, so the oracle passes, and the RAM guard must
 * report the store. */
static void present_flush_fault_store(void) {
    static OP_ONCE_INT s_fault = -1;
    int on = op_once_int_get(&s_fault);
    if (on < 0) {
        const char* e = getenv("OPENPETE_MODS_FLUSH_FAULT_STORE");
        on = op_once_int_fill(&s_fault, (e && *e && *e != '0') ? 1 : 0);
        if (on)
            OPL_WARN_F("mods", "%s", "fault injection: every present flush rewrites guest word [80010000]");
    }
    if (!on) return;
    uint8_t* ram = openpete_psx_get_ram();
    if (!ram) return;
    volatile uint32_t* w = (volatile uint32_t*)(void*)(ram + 0x10000u);
    *w = *w;
}

void openpete_mods_present_flush(CPUState* cpu, int canonical) OP_REQUIRES(sim) {
    /* Per-mod time report window: every 512 sim ticks (~17 s at 29.913),
     * counted at the canonical commit. ABOVE the draw gates - tick-hook
     * timing must report in headless runs too.
     *
     * Gate on mods LOADED, not openpete_mods_active (= a chain is
     * registered): the tick boundary drains the live-toggle queue, and
     * under deferred-entry load-all the very first enable can happen
     * while no mod owns a chain yet - an active-gated boundary would
     * never drain the queue, bricking every toggle. Zero mods loaded
     * still skips everything (pristine modless run). After the exit
     * pass no boundary runs, so no toggle or reload calls mod code. */
    if (canonical && openpete_mods_count() > 0 && !openpete_mods_api_closed()) {
        static unsigned s_canonical_ticks = 0;
        static unsigned s_report_ticks = 0;
        openpete_mods_tick_boundary(cpu, ++s_canonical_ticks);
        if (++s_report_ticks >= 512) {
            openpete_mods_time_report(s_report_ticks);
            s_report_ticks = 0;
        }
    }

    /* The draw half runs mod present hooks and the game's text emitter,
     * neither of which may store into guest RAM: under
     * OPENPETE_SUBTICK_RAM_GUARD the window maps guest RAM read-only for its
     * duration. The tick boundary above stays outside it: a live toggle may
     * run a mod's enable or disable, which writes gameplay state by design. */
    openpete_subtick_guard_window_begin("the mods' present flush");
    present_flush_draw(canonical);
    present_flush_fault_store();
    openpete_subtick_guard_window_end();
}
