/* render_paths.c - per-path render isolation registry. See render_paths.h. */

#include "renderer/render_paths.h"
#include "renderer/ui_game.h"      /* retained freeze backdrop commit */
#include "feature_registry.h"
#include "psx_runtime_internal.h"   /* CPUState, read_word - for the path brackets */
#include "openpete.h"
#include "openpete_knobs.h"          /* dev-overlay row bind */
#include "openpete_native_render.h"  /* isolation toggle */
#include "openpete_tile_census.h"    /* the tile census names paths through tag_kind_path */
#include "renderer/orchestrator.h"  /* canonical-render epoch commit */
#include "renderer/terrain.h"       /* deferred terrain feed fallback */
#include "renderer/moby.h"          /* held overlay-pass feed */
#include "psyq/psyq_game_hooks.h"   /* the OT walk's view of this filter */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_fiber.h"   /* openpete_frame: interval and mark VBLs, the ivdump probe */
#include "openpete_psx.h"     /* openpete_loop_anchor_ticks: interval and mark ticks */
#include "renderer/env_knob.h"
#include "game_layout.h"             /* level_id: the level push */
#include "openpete_mods.h"           /* openpete_mods_set_level */
#include "openpete_thread_roles.h"
#include "openpete_log.h"            /* table-full warning */

static const char* const k_names[RP_COUNT] = {
    "cyclorama", "spyro", "moby", "terrain", "shadow", "flame", "particles", "tracer",
    "transition", "portal", "glow", "sparkle", "moby_shadow", "ui",
};

static uint32_t s_mask   = 0;      /* bit per selected path */
static bool     s_active = false;  /* a selection is in effect */

void render_paths_init(const char* spec) {
    s_mask = 0;
    s_active = false;
    if (!spec || !*spec) return;   /* unset = all paths, no filtering */
    s_active = true;

    const char* p = spec;
    char tok[32];
    while (*p) {
        while (*p == ',' || *p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ',' && *p != ' ' && n < (int)sizeof(tok) - 1) tok[n++] = *p++;
        tok[n] = '\0';
        int matched = -1;
        for (int i = 0; i < RP_COUNT; i++)
            if (strcmp(tok, k_names[i]) == 0) { matched = i; break; }
        if (matched >= 0) s_mask |= (1u << matched);
        else if (strcmp(tok, "none") == 0) { /* selects no path */ }
        else fprintf(stderr, "[render-paths] unknown path '%s' (known: "
                             "cyclorama,spyro,moby,terrain,shadow,flame,particles,"
                             "tracer,transition,portal,glow,sparkle,moby_shadow,ui)\n", tok);
    }

    fprintf(stderr, "[render-paths] isolating:");
    for (int i = 0; i < RP_COUNT; i++)
        if (s_mask & (1u << i)) fprintf(stderr, " %s", k_names[i]);
    fprintf(stderr, "%s\n", s_mask ? "" : " (none matched - nothing will render)");
}

bool render_paths_active(void) { return s_active; }

void render_paths_set(const char* spec) {
    render_paths_init(spec);
    /* In isolation an empty native scene must show the background, not the
     * unfiltered PSX VRAM framebuffer - same sync as boot. */
    openpete_native_set_isolation(render_paths_active());
}

void render_paths_current(char* buf, size_t cap) {
    if (!s_active) { snprintf(buf, cap, "(all)"); return; }
    if (!s_mask)   { snprintf(buf, cap, "none"); return; }
    size_t off = 0;
    if (cap) buf[0] = '\0';
    for (int i = 0; i < RP_COUNT; i++) {
        if (!(s_mask & (1u << i))) continue;
        off += (size_t)snprintf(buf + off, cap > off ? cap - off : 0,
                                "%s%s", off ? "," : "", k_names[i]);
    }
}

bool render_path_enabled(render_path_t p) {
    if (!s_active) return true;                 /* no selection = all on */
    if (p < 0 || p >= RP_COUNT) return false;
    return (s_mask & (1u << (unsigned)p)) != 0;
}

/* Per-frame intervals. Sized generously: mobys can emit many separate dispatch
 * intervals per frame.
 *
 * Two banks: the game submits DrawOTag at the START of a VBL, before that
 * VBL's render functions record any intervals - so at walk time the current
 * table describes the OT being BUILT, not the one being DRAWN. The commit
 * (once per canonical present) rolls current -> previous instead of
 * discarding, and the OT-walk filter consults current first, then previous.
 * The polybuf double-buffers per frame, so the two tables cover disjoint
 * address banks and cannot false-match each other. */
#define MAX_INTERVALS 1024
/* vbl and tick: openpete_frame and openpete_loop_anchor_ticks() at the record. */
typedef struct { uint32_t start, end; render_path_t path; unsigned vbl; uint32_t tick; } interval_t;
static interval_t s_iv[MAX_INTERVALS];
static int        s_iv_n = 0;
static interval_t s_iv_prev[MAX_INTERVALS];
static int        s_iv_prev_n = 0;
/* Ranges render_path_record refused because the table was full, since the
 * last frame reset. The first refusal of each such run prints a warning and
 * the reset prints the run's total. */
static unsigned   s_iv_dropped = 0;

/* Interval-less canon-drawn marks (render_path_mark_drawn). Kept separate from
 * the intervals so they can never leak into the OT-walk addr filter. */
static uint32_t s_pending_marks = 0;
static unsigned s_mark_vbl[RP_COUNT];   /* VBL of each path's pending mark */
static uint32_t s_mark_tick[RP_COUNT];  /* loop-anchor count at each path's pending mark */

void render_paths_frame_reset(void) {
    if (s_iv_dropped) {
        OPL_WARN_F("render-paths", "interval table full: %u ranges dropped since the last "
                   "present commit (vbl %u)", s_iv_dropped, openpete_frame);
        s_iv_dropped = 0;
    }
    if (s_iv_n) {   /* roll into the drawn-OT bank; keep it across idle VBLs */
        memcpy(s_iv_prev, s_iv, (size_t)s_iv_n * sizeof(s_iv[0]));
        s_iv_prev_n = s_iv_n;
    }
    s_iv_n = 0; s_pending_marks = 0;
}

void render_path_mark_drawn(render_path_t p) {
    if (p < 0 || p >= RP_COUNT) return;
    s_pending_marks |= (1u << (unsigned)p);
    s_mark_vbl[p] = openpete_frame;
    s_mark_tick[p] = openpete_loop_anchor_ticks();
}

void render_path_record(render_path_t p, uint32_t start, uint32_t end) {
    if (end <= start) return;
    /* OPENPETE_RENDER_PATH_IVDUMP=frame: print every recorded polybuf interval
     * on that openpete_frame with its path name (OT-prim producer attribution). */
    {
        static int s_ivdump = -2;
        ENV_INT_LATCH(s_ivdump, "OPENPETE_RENDER_PATH_IVDUMP", -1);
        if (s_ivdump >= 0 && (int)openpete_frame == s_ivdump)
            fprintf(stderr, "[render-paths] ivdump f=%u path=%s [0x%08X,0x%08X)\n",
                    openpete_frame, k_names[p], start, end);
    }
    /* A range that continues the previous one of the same path extends it:
     * the 2D layer records one range per primitive, and the polybuf allocates
     * them back to back. */
    const uint32_t tick = openpete_loop_anchor_ticks();
    if (s_iv_n > 0 && s_iv[s_iv_n - 1].path == p && s_iv[s_iv_n - 1].end == start &&
        s_iv[s_iv_n - 1].vbl == openpete_frame && s_iv[s_iv_n - 1].tick == tick) {
        s_iv[s_iv_n - 1].end = end;
        return;
    }
    if (s_iv_n >= MAX_INTERVALS) {
        if (!s_iv_dropped++)
            OPL_WARN_F("render-paths", "interval table full (%d ranges) at vbl %u: dropping "
                       "%s [0x%08X,0x%08X) and later ranges until the next present commit; "
                       "the OT-walk filter hides their prims", MAX_INTERVALS, openpete_frame,
                       k_names[p], start, end);
        return;
    }
    s_iv[s_iv_n].start = start;
    s_iv[s_iv_n].end   = end;
    s_iv[s_iv_n].path  = p;
    s_iv[s_iv_n].vbl   = openpete_frame;
    s_iv[s_iv_n].tick  = tick;
    s_iv_n++;
}

/* "Canon drawn" mask - latched from the per-frame intervals once per canonical
 * render (before frame_reset clears them). A path is "drawn" iff it recorded at
 * least one non-empty polybuf interval this frame; render_path_record already
 * drops empty ranges, so a path that ran its render fn but emitted nothing (fully
 * culled / suppressed during a transition) reads as NOT drawn - which is what the
 * spine wants (reproduce only what the canonical frame actually painted). */
static uint32_t s_canon_mask = 0;
static uint32_t s_canon_mask_prev = 0;   /* the render before it */

void render_paths_canon_snapshot(void) {
    uint32_t m = s_pending_marks;
    for (int i = 0; i < s_iv_n; i++) m |= (1u << (unsigned)s_iv[i].path);
    s_canon_mask_prev = s_canon_mask;
    s_canon_mask = m;
}

/** @brief Drops what a render superseded before any canonical present recorded, when the game's next render first captures or opens its scene.
 *
 * Registered with openpete_native_scene_register_superseded. The canonical
 * present that follows shows only the newest scene, so the polybuf
 * intervals and drawn marks recorded on VBLs before that scene's VBL leave
 * the pending set, and the canonical-render epoch moves past the captures
 * the superseded render stamped (spyro_native_canon_epoch_supersede). A
 * render never spans a VBL boundary or a loop-anchor entry, so a path the
 * new scene's render recorded before its first feed opened the scene stays:
 * an interval or mark of the new scene's VBL stays when its loop-anchor
 * count is the current one, and one of an earlier tick rendered at the same
 * VBL (a render that skipped its VSync wait) leaves. Its producer
 * captures stay with it because the callback runs at the render's first
 * capture, before any is stamped (openpete_native_scene_note_game_capture).
 *
 * A scene a runahead speculation opens drops the intervals and marks but
 * keeps the epoch: the producers gated to the committed tick's render
 * (spyro_native_canon_capture_open) stamp no speculative capture, so a new
 * epoch would leave their committed captures stale at the canonical present
 * that follows the rollback, and the re-extract would drop every moby and
 * the player. Producers that capture on every render replace their set
 * within the epoch.
 * @param frame The VBL stamp of the scene being opened. */
static void render_paths_superseded(unsigned frame) {
    int w = 0;
    const uint32_t tick = openpete_loop_anchor_ticks();
    for (int i = 0; i < s_iv_n; i++)
        if (openpete_native_render_stamp_current(s_iv[i].vbl, s_iv[i].tick, frame, tick)) s_iv[w++] = s_iv[i];
    s_iv_n = w;
    for (int p = 0; p < RP_COUNT; p++)
        if (!openpete_native_render_stamp_current(s_mark_vbl[p], s_mark_tick[p], frame, tick))
            s_pending_marks &= ~(1u << (unsigned)p);
    if (spyro_native_canon_capture_open()) spyro_native_canon_epoch_supersede();
}

bool render_path_canon_drawn_prev(render_path_t p) {
    if (p < 0 || p >= RP_COUNT) return false;
    return (s_canon_mask_prev & (1u << (unsigned)p)) != 0;
}

bool render_path_canon_drawn(render_path_t p) {
    if (p < 0 || p >= RP_COUNT) return false;
    return (s_canon_mask & (1u << (unsigned)p)) != 0;
}

/* Per-canonical-present commit, registered with the core host present loop
 * (openpete_native_register_commit). Runs once per canonical render, after the
 * game fiber's render + DrawOTag, before the sub-tick re-extract: latch the
 * canon-drawn mask, then clear the intervals for the next frame. */
/* Feed audit: every native tri carries a provenance tag whose family (bits
 * 31:28) and kind within the family (openpete_tag_kind) name the render path
 * that emitted it. A path that fed geometry this canonical frame but recorded
 * no polybuf interval and no mark_drawn reads as not canon-drawn, so the
 * sub-tick spine drops it on every intermediate present: geometry on the
 * canonical present, gone between ticks. Detected here, at the commit,
 * instead of by watching the screen at a high present rate. Untagged tris
 * (family 0) and families without a path are not auditable. Reported per
 * path on first occurrence and in the exit census;
 * OPENPETE_RENDER_PATH_FEED_ASSERT=1 aborts on the first one. */
/* The path that emits each tag family's tris, -1 for none. The particle and
 * shadow families hold kinds of more than one path (tag_kind_path). */
static const int k_tag_family_path[16] = {
    -1, RP_MOBY, RP_TERRAIN, RP_SPYRO, RP_FLAME, RP_PARTICLES, RP_CYCLORAMA,
    RP_SHADOW, RP_TRACER, RP_PORTAL, -1, -1, -1, -1, -1, -1
};
/** @brief The render path that emits the tris of one tag family and kind.
 *
 * The particle family's glow rings are RP_GLOW's and its sparkle flares and
 * lines RP_SPARKLE's (each its own .s renderer); the shadow family's moby
 * casters are RP_MOBY_SHADOW's. Every other kind is its family's path.
 * @param f The tag family, tag bits 31:28.
 * @param k The kind, openpete_tag_kind.
 * @return The path index, or -1 when the family has no path. */
static int tag_kind_path(int f, int k) {
    if (f == 5 && k == (int)OPENPETE_TAG_PART_GLOW) return RP_GLOW;
    if (f == 5 && (k == (int)OPENPETE_TAG_PART_FLARE || k == (int)OPENPETE_TAG_PART_SPARK)) return RP_SPARKLE;
    if (f == 7 && k == (int)OPENPETE_TAG_SHADOW_MOBY) return RP_MOBY_SHADOW;
    return k_tag_family_path[f];
}
/** @brief The render path of a tag family and kind, or of the family itself, for the tile census.
 *
 * @param f The tag family, tag bits 31:28; outside 0 to 15 maps to no path.
 * @param k The kind (openpete_tag_kind), or -1 for the family's own path.
 * @return The path index, or -1 when there is none. */
static int tag_census_path(int f, int k) {
    if (f < 0 || f > 15) return -1;
    return k < 0 ? k_tag_family_path[f] : tag_kind_path(f, k);
}
/** @brief Sums the scene's tagged tris per render path.
 *
 * Reads openpete_native_scene_tag_kind_counts and adds each family kind's
 * count to its path (tag_kind_path); untagged tris and families without a
 * path are left out.
 * @param out Receives RP_COUNT counts. */
static void scene_path_tris(uint32_t out[RP_COUNT]) {
    uint32_t n[16][OPENPETE_TAG_KINDS];
    openpete_native_scene_tag_kind_counts(n);
    memset(out, 0, RP_COUNT * sizeof out[0]);
    for (int f = 1; f < 16; f++)
        for (int k = 0; k < (int)OPENPETE_TAG_KINDS; k++) {
            const int p = tag_kind_path(f, k);
            if (p >= 0) out[p] += n[f][k];
        }
}
static uint32_t s_feed_audit_frames[RP_COUNT];   /* frames with unmarked feed, per path */
static uint32_t s_feed_audit_tris[RP_COUNT];     /* tris fed on those frames, per path */
static uint32_t s_feed_audit_fed_frames[RP_COUNT]; /* frames the path fed at all (engagement) */
static int      s_feed_audit_registered = 0;

/** @brief Prints the exit summary of the feed audit to stderr.
 *
 * One line: the path-frames that fed at all, the path-frames that fed with no
 * polybuf interval and no mark, then each path that fed with its fed frames
 * and, when nonzero, its unmarked frames and their triangle total.
 * Registered with atexit by the first audited scene. */
static void render_paths_feed_audit_census(void) {
    OP_ROLE_ASSERT(sim);
    uint32_t fed = 0, bad = 0;
    for (int p = 0; p < RP_COUNT; p++) { fed += s_feed_audit_fed_frames[p]; bad += s_feed_audit_frames[p]; }
    fprintf(stderr, "render-paths: feed audit: fed_path_frames=%u unmarked_path_frames=%u",
            fed, bad);
    for (int p = 0; p < RP_COUNT; p++) {
        if (!s_feed_audit_fed_frames[p]) continue;
        fprintf(stderr, " %s=%u", k_names[p], s_feed_audit_fed_frames[p]);
        if (s_feed_audit_frames[p])
            fprintf(stderr, "/unmarked=%u(%u tris)", s_feed_audit_frames[p], s_feed_audit_tris[p]);
    }
    fprintf(stderr, "\n");
}

/** @brief Audits the canonical scene's tagged triangles per render path against the canon-drawn mask.
 *
 * Runs once per built scene. For each path with tagged triangles
 * (scene_path_tris), counts the path as fed, and when the path is not
 * canon-drawn counts an unmarked frame and its triangles, printing a DEFECT
 * line on the path's first occurrence. With OPENPETE_RENDER_PATH_FEED_ASSERT
 * set it prints every occurrence and aborts on the first. */
static void render_paths_feed_audit(void) {
    /* One audit per canonical scene. The commit runs on every canonical
     * present; when the game stops rendering (a level-exit freeze, a
     * transition) the presents continue and the scene keeps the last frame's
     * counts while the intervals were already cleared, so a scene is audited
     * only on the first commit after it was built. */
    static unsigned s_last_scene = 0xFFFFFFFFu;
    const unsigned sf = openpete_native_scene_frame();
    if (sf == 0xFFFFFFFFu || sf == s_last_scene) return;
    s_last_scene = sf;
    if (!s_feed_audit_registered) { s_feed_audit_registered = 1; atexit(render_paths_feed_audit_census); }
    static int s_assert = -1;
    ENV_FLAG_LATCH(s_assert, "OPENPETE_RENDER_PATH_FEED_ASSERT");
    uint32_t n[RP_COUNT];
    scene_path_tris(n);
    for (int p = 0; p < RP_COUNT; p++) {
        if (n[p] == 0) continue;
        s_feed_audit_fed_frames[p]++;
        if (render_path_canon_drawn((render_path_t)p)) continue;
        if (s_feed_audit_frames[p]++ == 0 || s_assert)
            fprintf(stderr, "[render-paths] DEFECT f=%u path=%s fed %u native tris with no polybuf "
                    "interval and no render_path_mark_drawn; sub-tick presents drop it\n",
                    openpete_frame, k_names[p], n[p]);
        s_feed_audit_tris[p] += n[p];
        if (s_assert) abort();
    }
}

/** @brief Prints the canon-drawn paths and the native scene's per-path triangle counts under OPENPETE_RP_CENSUS.
 *
 * OPENPETE_RP_CENSUS=LO-HI, or a single value, selects an openpete_frame
 * range, read on the first call. The per-canonical-present commit calls this
 * after latching the canon-drawn mask, so each line separates a producer that
 * fed the canonical scene from one that did not; the tris are attributed to
 * paths as the feed audit attributes them (scene_path_tris), and untagged
 * tris are counted apart. */
static void render_paths_census(void) {
    static env_frame_range_t s_range = ENV_FRAME_RANGE_INIT;
    if (!env_frame_range_active(&s_range, "OPENPETE_RP_CENSUS", openpete_frame)) return;
    uint32_t fam[16], n[RP_COUNT];
    openpete_native_scene_tag_family_counts(fam);
    scene_path_tris(n);
    fprintf(stderr, "[rp-census f=%u CANON] drawn:", openpete_frame);
    for (int i = 0; i < RP_COUNT; i++)
        if (s_canon_mask & (1u << (unsigned)i)) fprintf(stderr, " %s", k_names[i]);
    fprintf(stderr, " | tris:");
    for (int p = 0; p < RP_COUNT; p++)
        if (n[p]) fprintf(stderr, " %s=%u", k_names[p], n[p]);
    if (fam[0]) fprintf(stderr, " untagged=%u", fam[0]);
    fprintf(stderr, " scene=%u\n", openpete_native_scene_frame());
}

/** @brief The per-canonical-present commit: latches the canon-drawn mask and rolls the frame state.
 *
 * In order: runs a terrain feed still held for a portal draw that did not
 * release it, feeds the held menu overlay moby pass rebuilt from the moby's
 * current state, turns a repeat present the game cleared into an empty one
 * (cyclorama_native_repeat_resolve), latches the canon-drawn mask, prints the census, audits the feed,
 * rolls the intervals, commits the canonical-render epoch (none of these five on
 * a repeat present, openpete_native_scene_canon_repeat), then syncs the
 * native background clear and the retained freeze backdrop from the draw
 * environment the next DrawOTag applies. */
static void render_paths_commit(void) OP_REQUIRES(sim) {
    terrain_native_flush_deferred_late();   /* a feed held past its portal draw */
    moby_native_overlay_commit();           /* the menu overlay pass, at the moby's present state */
    /* A present with no game render where the game cleared the screen shows
     * an empty frame: it becomes a fresh present of an empty scene. */
    cyclorama_native_repeat_resolve();
    /* A present with no game render since the last one shows the previous
     * scene: the drawn mask and the fresh epoch stay those of the render that
     * built it, so the sub-tick re-extracts keep replaying it. */
    if (!openpete_native_scene_canon_repeat()) {
        render_paths_canon_snapshot();
        render_paths_census();
        render_paths_feed_audit();
        render_paths_frame_reset();
        /* Same anchor drives the canonical-render epoch: captures taken during
         * the present that just committed become the (only) fresh set for the
         * sub-tick re-extracts that follow. */
        spyro_native_canon_epoch_commit();
    }
    /* Sync the native background clear from g_CurDB->m_DrawEnv (the env the
     * game's DrawOTag will apply to the scene being presented) - the PutDrawEnv
     * thunk latch trails the OT build by one VSync and flashed the stale
     * homeworld clear at the portal-entry transition boundary. */
    cyclorama_native_env_clear_commit();
    /* Same DRAWENV, same instant: isbg decides whether that clear happens at
     * all, and a suppressed clear is how some games hold the last rendered
     * frame behind a menu screen. */
    ui_freeze_retain_commit();
}

bool render_path_addr_visible(uint32_t addr) {
    for (int i = 0; i < s_iv_n; i++) {
        if (addr >= s_iv[i].start && addr < s_iv[i].end)
            return render_path_enabled(s_iv[i].path);
    }
    /* The walked OT was built on the PREVIOUS render VBL (DrawOTag submits at
     * the start of a VBL, before this VBL's records) - its intervals live in
     * the rolled bank. */
    for (int i = 0; i < s_iv_prev_n; i++) {
        if (addr >= s_iv_prev[i].start && addr < s_iv_prev[i].end)
            return render_path_enabled(s_iv_prev[i].path);
    }
    return false;   /* untracked geometry: dropped while a selection is active */
}

/* The OT walk's filter hooks (psyq_game_hooks.h), defined once for every game. */
int psyq_game_render_paths_active(void) { return render_paths_active(); }

/* Documented at its declaration in psyq_game_hooks.h. */
int psyq_game_render_path_addr_visible(uint32_t addr) {
    return render_path_addr_visible(addr);
}

/* This module holds no polybuf bracket of its own: each path's feature module
 * records its RP_* range through render_path_record (directly, or through
 * hook_bracket_run). This module is the --render-paths parser, the OT-walk
 * filter above and the per-canonical-present commit below. */

static const feature_dispatch_entry_t render_paths_dispatch[] = {
    { 0u, NULL, 0u },
};

/* Dev-overlay adapters: the registry row lives in core, which cannot link
 * against this game module, so apply-options binds the live channel. */
/** @brief The render_paths row's getter: writes render_paths_current() into `buf`.
 *
 * @param k Unused.
 * @param buf Receives the text.
 * @param cap The capacity of `buf`. */
static void rp_knob_get(const knob_desc_t* k, char* buf, size_t cap) {
    (void)k;
    render_paths_current(buf, cap);
}
/** @brief The render_paths row's setter: applies the text through render_paths_set().
 *
 * A value starting with '(' is the getter's "(all)" placeholder and clears
 * the selection; "none" selects no path.
 * @param k Unused.
 * @param v The new selection text, or NULL.
 * @return 0. */
static int rp_knob_set(knob_desc_t* k, const char* v) {
    (void)k;
    if (v && *v == '(') v = NULL;   /* "(all)" placeholder = clear */
    render_paths_set(v);
    return 0;
}

/** @brief Parses --render-paths, syncs the native isolation background, and registers the per-canonical-present commit.
 *
 * Also binds the dev overlay's render_paths row and its path vocabulary,
 * and hands the tile census the tag-to-path mapping.
 * The module hooks no guest address: each path's own feature module records
 * its polybuf interval.
 * @param opts The parsed options; NULL leaves every path rendering. */
static void render_paths_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    render_paths_init(opts ? opts->render_paths : NULL);
    /* In isolation, an empty native scene must show the background, not the
     * unfiltered PSX VRAM framebuffer (which would show every path). */
    openpete_native_set_isolation(render_paths_active());
    /* Drive the per-frame canon-mask latch + interval reset from the host present
     * loop (renderer-agnostic), not psyq_DrawOTag (future-removed). */
    openpete_native_register_commit(render_paths_commit);
    openpete_native_scene_register_superseded(render_paths_superseded);
    openpete_native_register_tag_paths(tag_census_path, k_names, RP_COUNT);
    /* Attach the live channel to the dev overlay's row. */
    (void)openpete_knob_bind_str("render_paths", rp_knob_get, rp_knob_set);
    (void)openpete_knob_bind_vocab("render_paths", k_names, RP_COUNT);
}

const feature_module_t render_paths_module = {
    .name           = "render_paths",
    .apply_options  = render_paths_apply_options,
    .dispatch_table = render_paths_dispatch,
};

void render_material_level_push(struct CPUState* cpu) {
    if (cpu) openpete_mods_set_level((int)cpu->read_word(game_layout.level_id));
}
