/*
 * Knob registry - the table behind the M-key dev config overlay.
 *
 * One row per config knob. LIVE rows write through the same public setters
 * the O-menu uses (renderer/host state only - never gameplay-readable RAM);
 * RESTART_ONLY rows are display-only until each is individually audited and
 * given a proper live channel (a single boot-latched consumer is enough to
 * keep a row read-only: cf. the fog-scale latch history in no_lod.h).
 *
 * Adding a permanent CLI flag or OPENPETE_* env var? Add its row here in the
 * same change - the overlay doubles as the living catalog of every knob.
 */

#include "openpete_knobs.h"
#include "openpete_osd.h"
#include "openpete_memcard.h"
#include "openpete_workpool.h"
#include "openpete_terrain_cache.h"
#include "openpete_disc_cache.h"
#include "openpete.h"
#include "openpete_config.h"
#include "config_schema.h"
#include "openpete_log.h"
#include "openpete_native_render.h"
#include "openpete_camlock.h"
#include "openpete_fillpick.h"
#include "openpete_freecam.h"
#include "openpete_host_input.h"
#include "openpete_host_mouse.h"
#include "openpete_record.h"
#include "openpete_rewind.h"
#include "openpete_state.h"
#include "openpete_stick_response.h"
#include "openpete_render_res.h"
#include "no_lod.h"

#include <math.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const openpete_options_t *g_opts;

/* ------------------------------------------------------------------ */
/* value plumbing helpers                                             */
/* ------------------------------------------------------------------ */

/** @brief Numeric getter that returns the row's stored value. */
static double get_stored(const knob_desc_t *k) { return k->stored_num; }

/** @brief Index of the entry of `vals[0..n)` nearest to `v`.
 *
 * The first entry wins a tie. Returns 0 when `n` is 0 or less, or when `v` is
 * NaN. */
static int nearest_enum_idx(const float *vals, int n, float v) {
    int best = 0;
    float bd = 1e30f;
    for (int i = 0; i < n; i++) {
        float d = fabsf(vals[i] - v);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

/* Rows the O menu moves through the renderer's own setters must read the
 * renderer back, not a registry-side shadow: the menu bypasses set_num, so a
 * shadowed row would still report its boot value and "save edited settings"
 * would skip the pick entirely. Until the native renderer is up (its state
 * settles after the registry stamps boot values, and never at all under
 * --headless) the stored boot value is the live value. Keyed on the DISPLAY
 * predicate: these rows are display state, and in psycross mode the native
 * window and its accessors are real even though no native scene is walked. */
#define KNOB_LIVE_OR_STORED(k) \
    do { if (!openpete_native_render_enabled()) return (k)->stored_num; } while (0)

/* ---- Display ---- */

/** @brief 1 when the native window is fullscreen, else 0. */
static double fullscreen_get(const knob_desc_t *k) {
    (void)k; return openpete_native_fullscreen_active() ? 1.0 : 0.0;
}
/** @brief Queue a fullscreen toggle when `v` differs from the window's current state; the window's thread applies it, so the state reads unchanged until then. */
static int fullscreen_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_native_fullscreen_active() != 0))
        openpete_native_request_fullscreen_toggle();   /* SDL call runs on the window's thread */
    return 0;
}

static const char *const k_aspect_names[] = { "auto", "4:3", "16:10", "16:9", "21:9", "32:9" };
static const float       k_aspect_vals[]  = { 0.0f, 4.f/3.f, 16.f/10.f, 16.f/9.f, 21.f/9.f, 32.f/9.f };
/** @brief Index in k_aspect_names of the renderer's live aspect ratio.
 *
 * Returns the stored value until the native renderer is up. Index 0 is
 * auto, whose 0.0 is a sentinel rather than a ratio: a renderer aspect of
 * 0 or less reads as 0, and a positive one as the nearest of the five fixed
 * ratios plus 1 (the same offset knob_registry_init uses). */
static double aspect_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    float wh = openpete_native_get_aspect();
    if (wh <= 0.0f) return 0;
    return nearest_enum_idx(k_aspect_vals + 1, 5, wh) + 1;
}
/** @brief Set the renderer aspect ratio to entry `v` of k_aspect_vals (entry 0 is auto).
 *
 * Stores the index in the row. Returns -1, changing nothing, when the truncated
 * index is outside the table; else 0. */
static int aspect_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i >= (int)(sizeof(k_aspect_vals)/sizeof(k_aspect_vals[0]))) return -1;
    openpete_native_set_aspect(k_aspect_vals[i]);
    k->stored_num = i;
    return 0;
}

/* An explicit factor table, not a power-of-two series: the resolve pass box-
 * filters the exact NxN source block, so every integer factor is well defined
 * and 3x is a real rung (9 samples/pixel, ~44% less render-target area than
 * 4x). The ladder stops at OPENPETE_SSAA_MAX because the supersampled targets
 * outgrow an 8GB card beyond it (see openpete.h). */
static const int         k_ssaa_vals[]  = { 1, 2, 3, 4 };
static const char *const k_ssaa_names[] = { "off", "2x", "3x", "4x" };
#define K_SSAA_N ((int)(sizeof(k_ssaa_vals)/sizeof(k_ssaa_vals[0])))
/** @brief Index of the k_ssaa_vals rung nearest to a supersampling factor.
 *
 * An --ssaa factor the ladder does not list still lands on a row; a tie
 * picks the lower rung. */
static int ssaa_idx_of(int factor) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < K_SSAA_N; i++) {
        int d = k_ssaa_vals[i] - factor;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
/** @brief The supersampling row index: the k_ssaa_vals rung nearest to the renderer's factor.
 *
 * Returns the stored value until the native renderer is up. */
static double ssaa_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return ssaa_idx_of(openpete_native_get_ssaa());
}
/** @brief Set the supersampling factor to entry `v` of k_ssaa_vals.
 *
 * Stores the index in the row. Returns -1, changing nothing, when the truncated
 * index is outside the ladder; else 0. */
static int ssaa_set(knob_desc_t *k, double v) {
    int i = (int)v;
    if (i < 0 || i >= K_SSAA_N) return -1;
    openpete_native_set_ssaa(k_ssaa_vals[i]);
    k->stored_num = i;
    return 0;
}

/** @brief The renderer's vsync preference as 0 or 1.
 *
 * Returns the stored value until the native renderer is up, and also when
 * the renderer reports -1: the device never got an explicit preference (the
 * env or the surface picked the present mode), which the boot value already
 * records. */
static double vsync_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    int pref = openpete_native_get_vsync();
    return (pref < 0) ? k->stored_num : (pref ? 1 : 0);
}
/** @brief Set the renderer's vsync preference from `v` (nonzero enables) and store it in the row. */
static int vsync_set(knob_desc_t *k, double v) {
    openpete_native_set_vsync(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

static const char *const k_anchor_names[] = { "top-left", "top-center", "top-right",
                                              "bottom-left", "bottom-center",
                                              "bottom-right" };
/** @brief The performance-stats block anchor index into k_anchor_names.
 *
 * Returns the stored value until the native renderer is up. */
static double perfpos_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_native_perf_anchor();
}
/** @brief Set the performance-stats block anchor to index `v` of k_anchor_names.
 *
 * The renderer ignores an index outside the anchor range, while the row stores
 * the truncated index regardless. */
static int perfpos_set(knob_desc_t *k, double v) {
    openpete_native_set_perf_anchor((int)v);
    k->stored_num = (int)v;
    return 0;
}

/** @brief 1 when the on-screen notifications are enabled, else 0; the stored value until the native renderer is up. */
static double osd_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_osd_enabled() ? 1.0 : 0.0;
}
/** @brief Enable or disable on-screen notifications from `v` (nonzero enables).
 *
 * Stores the resulting 0 or 1 in the row. */
static int osd_set(knob_desc_t *k, double v) {
    openpete_osd_set_enabled(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}
/** @brief 1 when the version OSD is shown, else 0. */
static double versionosd_get(const knob_desc_t *k) {
    /* No stored fallback: the backing flag is a plain host-side int, seeded
     * from the parsed options at boot whether or not the native renderer came
     * up, so the live read is always the resolved value. */
    (void)k;
    return openpete_native_version_osd() ? 1.0 : 0.0;
}
/** @brief Show or hide the version OSD from `v` (nonzero shows).
 *
 * Stores the resulting 0 or 1 in the row. */
static int versionosd_set(knob_desc_t *k, double v) {
    openpete_native_set_version_osd(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}
/** @brief The notification anchor index into k_anchor_names.
 *
 * Returns the stored value until the native renderer is up. */
static double osdpos_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_osd_anchor();
}
/** @brief Set the notification anchor to index `v` of k_anchor_names.
 *
 * The OSD ignores an index outside the anchor range, while the row stores the
 * truncated index regardless. */
static int osdpos_set(knob_desc_t *k, double v) {
    openpete_osd_set_anchor((int)v);
    k->stored_num = (int)v;
    return 0;
}
/** @brief The default notification timeout in seconds.
 *
 * The OSD holds milliseconds; the row shows seconds, the unit --osd-timeout
 * takes. Returns the stored value until the native renderer is up. */
static double osdtimeout_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_osd_default_timeout_ms() / 1000.0;
}
/** @brief Set the default notification timeout from `v` seconds.
 *
 * Returns -1, changing nothing, when `v` is 0 or less. Otherwise converts to
 * milliseconds rounded to nearest, passes them to the OSD (which clamps to its
 * maximum), stores `v` in the row and returns 0. */
static int osdtimeout_set(knob_desc_t *k, double v) {
    if (v <= 0.0) return -1;
    openpete_osd_set_default_timeout_ms((uint32_t)(v * 1000.0 + 0.5));
    k->stored_num = v;
    return 0;
}

static const char *const k_perf_names[] = { "off", "fps", "detailed" };
/** @brief The performance-stats level as an index into k_perf_names.
 *
 * Returns the stored value until the native renderer is up. */
static double perfstats_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return (double)openpete_native_perf_stats_level();
}
/** @brief Set the performance-stats level to `v`, truncated to int.
 *
 * The renderer clamps the level to its valid range; the row stores the
 * unclamped truncated value. */
static int perfstats_set(knob_desc_t *k, double v) {
    openpete_native_set_perf_stats((int)v);
    k->stored_num = (int)v;
    return 0;
}

static const char *const k_vprec_names[] = { "subpixel", "integer" };
static const char *const k_texmap_names[] = { "perspective", "affine" };
static const char *const k_stick_names[] = { "linear", "console" };
/** @brief The stick response the live pad decode applies: 0 linear, 1 console.
 * @param k The row.
 * @return The response. */
static double stick_get(const knob_desc_t *k) { (void)k; return (double)openpete_stick_response_get(); }
/** @brief Select the stick response from `v`: 0 is linear, any other value console.
 *
 * The decode applies it from the next pad sample.
 * @param k The row.
 * @param v The response.
 * @return 0. */
static int stick_set(knob_desc_t *k, double v) {
    openpete_stick_response_set((int)v == 0 ? OPENPETE_STICK_RESPONSE_LINEAR
                                            : OPENPETE_STICK_RESPONSE_CONSOLE);
    k->stored_num = (int)v != 0;
    return 0;
}
/** @brief 1 when integer-grid vertex precision is chosen, 0 for subpixel positions.
 *
 * Returns the stored value until the native renderer is up. */
static double vprec_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_vertex_precision() ? 1.0 : 0.0;
}
/** @brief Select vertex precision from `v`: 1 is the integer grid, else subpixel.
 *
 * The row stores the truncated `v` unchanged. */
static int vprec_set(knob_desc_t *k, double v) {
    openpete_native_set_vertex_precision((int)v == 1);
    k->stored_num = (int)v;
    return 0;
}

/** @brief The chosen texture mapping: 0 perspective, 1 affine.
 *
 * Returns the stored value until the native renderer is up, the renderer's
 * chosen mode after that: the last queued mode, which the renderer applies
 * with a pipeline rebuild at a later present. The O menu moves the same
 * mode, so the row reads the renderer rather than stored_num. */
static double texmap_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_texture_mapping();
}
/** @brief Queue the texture mapping mode from `v`: 1 is affine, any other value perspective.
 *
 * The renderer applies it with a pipeline rebuild at the next present. The row
 * stores nothing, since texmap_get reads the renderer. */
static int texmap_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_texture_mapping((int)v == 1);
    return 0;
}

/** @brief The raster-quantize multiplier, 0 when off.
 *
 * Returns the stored value until the native renderer is up. */
static double rquant_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_raster_quantize();
}
/** @brief The resolved worker-thread count of the extraction work pool, with auto expanded. */
static double exthreads_get(const knob_desc_t *k) {
    (void)k;
    return openpete_workpool_threads();   /* resolved count (auto expanded) */
}
static const char *const k_disccache_names[] = { "0", "1", "auto" };
/** @brief Whether the disc cache attached, as its k_disccache_names index: 1 when active, else 0. */
static double disccache_get(const knob_desc_t *k) {
    (void)k;
    return openpete_disc_cache_active() ? 1 : 0;
}
/* The resolved level-load delay (core/psx/fiber.c), declared here rather
 * than through openpete_fiber.h, whose SDL atomics the unit test does not
 * link. */
extern int openpete_cd_load_delay;
/** @brief The resolved level-load delay, read from openpete_cd_load_delay. */
static double loaddelay_get(const knob_desc_t *k) {
    (void)k;
    return openpete_cd_load_delay;
}
/** @brief 1 when the terrain cache came up, else 0. */
static double tercache_get(const knob_desc_t *k) {
    (void)k;
    return openpete_tercache_ready() ? 1 : 0;   /* cache actually came up */
}
/** @brief Set the raster-quantize multiplier to `v`, truncated to int.
 *
 * The renderer treats a value of 0 or less as off and applies the change at the
 * next scene split; the row stores the unclamped truncated value. */
static int rquant_set(knob_desc_t *k, double v) {
    openpete_native_set_raster_quantize((int)v);
    k->stored_num = (int)v;
    return 0;
}

/** @brief The renderer's applied untextured-lighting view flag.
 *
 * A change the setter queues reads as the old value until the next present
 * applies it. */
static double notex_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_notex();
}
/** @brief Set the renderer's untextured-lighting flag from `v` (nonzero is on); the renderer applies it at the next present. */
static int notex_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_notex(v != 0.0);
    return 0;
}

/** @brief The renderer's applied wireframe view flag.
 *
 * A change the setter queues reads as the old value until the next present
 * applies it. */
static double wireframe_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_wireframe();
}
/** @brief Queue the renderer's wireframe view flag from `v` (nonzero is on).
 *
 * The renderer applies it at the next present. */
static int wireframe_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_wireframe(v != 0.0);
    return 0;
}

/** @brief The renderer's dither flag, which the O menu's DITHER row also sets.
 *
 * Returns the stored value until the native renderer is up. */
static double dither_get(const knob_desc_t *k) {
    KNOB_LIVE_OR_STORED(k);
    return openpete_native_get_dither();
}
/** @brief Set the renderer's dither flag from `v` (nonzero is on). */
static int dither_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_dither(v != 0.0);
    return 0;
}

/** @brief 1 when the native feeders drop the player's draws, else 0.
 *
 * The renderer seeds the flag from the options at its init, as it does the
 * untextured view's. */
static double hide_player_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_hide_player();
}
/** @brief Set whether the native feeders drop the player's draws from `v` (nonzero is on). */
static int hide_player_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_hide_player(v != 0.0);
    return 0;
}

/** @brief 1 when the compose skips the HUD layers, else 0.
 *
 * The renderer seeds the flag from the options at its init. */
static double hide_hud_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_get_hide_hud();
}
/** @brief Set whether the compose skips the HUD layers from `v` (nonzero is on). */
static int hide_hud_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_hide_hud(v != 0.0);
    return 0;
}

static const char *const k_lod_names[] = { "original", "extended" };
static const char *const k_tervis_names[] = { "game", "frustum", "filled", "auto" };
/** @brief The sector visibility mode as its k_tervis_names index (openpete_sector_visibility_t).
 * @param k The row.
 * @return 0 game, 1 frustum, 2 filled, 3 auto. */
static double tervis_get(const knob_desc_t *k) {
    (void)k;
    return (double)openpete_native_get_sector_visibility();
}
/** @brief Select the sector visibility mode by its k_tervis_names index, which is its openpete_sector_visibility_t value.
 * @param k The row.
 * @param v The index: 0 game, 1 frustum, 2 filled, 3 auto.
 * @return 0. */
static int tervis_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_sector_visibility((int)v);
    return 0;
}
/** @brief 1 when the terrain walk stores the sentinel for a skipped drain sub-cell, 0 when it keys and draws it. */
static double drainskip_get(const knob_desc_t *k) {
    (void)k;
    return openpete_native_terrain_drain_legacy() ? 0.0 : 1.0;
}
/** @brief Select the skipped-sub-cell packing: nonzero `v` is the sentinel, zero keys and draws the sub-cell. */
static int drainskip_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_native_set_terrain_drain_legacy(v == 0.0);
    return 0;
}
/** @brief 1 when the extended level-of-detail mode is chosen (openpete_no_lod_wanted), else 0. */
static double lod_get(const knob_desc_t *k) { (void)k; return openpete_no_lod_wanted() ? 1.0 : 0.0; }
/** @brief Switch level-of-detail mode: nonzero is extended, zero original.
 *
 * Passes no fog scale, so the fog scale follows the mode unless fog was set
 * explicitly. */
static int lod_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_no_lod_set(v != 0.0, -1.0f);   /* fog follows the mode unless touched */
    return 0;
}

/* The fog row's entries: index 0 follows the LOD mode and reads the mode
 * it resolves to (fog_get rewrites it), 1..3 pin openpete_fog_mode_t 0..2.
 * The config vocabulary is k_fog_cfg_names; index 0 has none, since an
 * unset key is what follows the LOD mode. */
static char s_fog_auto_label[40] = "follow LOD (original)";
static const char *const k_fog_names[] = { s_fog_auto_label, "original", "per-pixel", "off" };
static const char *const k_fog_cfg_names[] = { NULL, "original", "per-pixel", "off" };
/** @brief The fog row's entry: 0 while the fog form follows the LOD mode, else the pinned mode + 1.
 *
 * Rewrites entry 0's label with the mode the LOD mode resolves to. */
static double fog_get(const knob_desc_t *k) {
    (void)k;
    static const char *const names[] = { "original", "per-pixel", "off" };
    snprintf(s_fog_auto_label, sizeof s_fog_auto_label, "follow LOD (%s)",
             names[openpete_no_lod_wanted() ? OPENPETE_FOG_PER_PIXEL : OPENPETE_FOG_ORIGINAL]);
    return (double)(openpete_fog_mode_pinned() + 1);
}
/** @brief Pin the fog form to entry `v` - 1, or return it to following the LOD mode for entry 0. */
static int fog_set(knob_desc_t *k, double v) {
    (void)k;
    const int i = (int)v;
    if (i < 0 || i > 3) return -1;
    openpete_fog_mode_set(i - 1);
    return 0;
}

/** @brief The fog-distance multiplier the next latch applies (openpete_fog_scale_wanted). */
static double fogscale_get(const knob_desc_t *k) { (void)k; return openpete_fog_scale_wanted(); }
/** @brief Set the fog-distance multiplier from the fog-scale row; a non-finite value is refused. */
static int fogscale_set(knob_desc_t *k, double v) {
    (void)k;
    if (!isfinite(v)) return -1;
    openpete_fog_scale_set((float)v);
    return 0;
}

static const char *const k_renderer_names[] = { "psycross", "vk" };
/** @brief The renderer selected at boot: 0 psycross, 1 vk.
 *
 * Returns 1 when options are unbound. */
static double renderer_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->renderer : 1.0; }

/* The game's default window size, handed in by the host loop once the game
 * layout is known. The registry has no game layout of its own: it is compiled
 * into console tests that link no game. Zero until set. */
static int s_default_win_w, s_default_win_h;

void openpete_knob_default_window_set(int w, int h) {
    s_default_win_w = w;
    s_default_win_h = h;
}

/** @brief Write the window size into `buf`.
 *
 * Shows "WxH" for a --window size, "WxH (default)" for the game's default size
 * once the host has supplied it, else "(default)". */
static void window_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    if (g_opts && g_opts->window_maximized > 0)
        snprintf(buf, cap, "maximized");
    else if (g_opts && g_opts->window_w > 0)
        snprintf(buf, cap, "%dx%d", g_opts->window_w, g_opts->window_h);
    else if (s_default_win_w > 0 && s_default_win_h > 0)
        snprintf(buf, cap, "%dx%d (default)", s_default_win_w, s_default_win_h);
    else
        snprintf(buf, cap, "(default)");
}

/** @brief Format the launcher window's first size: WxH, "maximized", or "(1280x800, or --window's size)" when unset.
 *
 * @param k The row; unused.
 * @param buf Receives the text.
 * @param cap The capacity of buf. */
static void launcher_window_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    if (g_opts && g_opts->launcher_window_maximized > 0)
        snprintf(buf, cap, "maximized");
    else if (g_opts && g_opts->launcher_window_maximized == 0)
        snprintf(buf, cap, "%dx%d", g_opts->launcher_window_w, g_opts->launcher_window_h);
    else
        snprintf(buf, cap, "(1280x800, or --window's size)");
}

/** @brief Return 1 when the launcher opens fullscreen (--launcher-fullscreen 1), else 0.
 *
 * @param k The row; unused.
 * @return The value. */
static double launcher_fullscreen_get(const knob_desc_t *k) {
    (void)k;
    return (g_opts && g_opts->launcher_fullscreen > 0) ? 1.0 : 0.0;
}

/* ---- Pacing ---- */

static const char *const k_cadence_names[] = { "even", "fill" };
/** @brief The eager planner's cadence policy: 0 even, 1 fill (openpete_get_cadence). */
static double cadence_get(const knob_desc_t *k) {
    (void)k;
    return openpete_get_cadence() == OPENPETE_CADENCE_FILL ? 1.0 : 0.0;
}
/** @brief Set the cadence policy: nonzero `v` is fill, zero even; read by the next span plan. */
static int cadence_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_set_cadence(v != 0.0 ? OPENPETE_CADENCE_FILL : OPENPETE_CADENCE_EVEN);
    return 0;
}

/** @brief The pacer's live render-fps target (0 is the match-sim pacer). */
static double renderfps_get(const knob_desc_t *k) { (void)k; return openpete_get_render_fps(); }
/** @brief Set the pacer's render-fps target to `v`, truncated to int.
 *
 * Values below 0 are clamped to 0 (match-sim pacing) by the pacer, which
 * applies the change at the host loop's next quiescent point. */
static int renderfps_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_set_render_fps((int)v);
    return 0;
}

/** @brief The pacer's live tick-scale factor. */
static double tickscale_get(const knob_desc_t *k) { (void)k; return openpete_get_tick_scale(); }
/** @brief Set the pacer's tick-scale factor to `v`.
 *
 * Returns -1, changing nothing, when `v` is 0 or less. Otherwise the pacer
 * clamps it to [OPENPETE_TICK_SCALE_MIN, OPENPETE_TICK_SCALE_MAX] and applies it at the host loop's next quiescent
 * point; only wall-clock pacing changes, not the ticks executed. */
static int tickscale_set(knob_desc_t *k, double v) {
    (void)k;
    if (v <= 0.0) return -1;
    openpete_set_tick_scale(v);
    return 0;
}

/** @brief The applied runahead depth, which is 0 while the mod-capability gate forces it off. */
static double runahead_get(const knob_desc_t *k) { (void)k; return (double)openpete_runahead_get(); }
/** @brief Request runahead depth `v`, truncated to int.
 *
 * Returns -1, changing nothing, when `v` is negative. Otherwise the request is
 * clamped to 3 by the runtime and applied at the host loop's next quiescent
 * point. */
static int runahead_set(knob_desc_t *k, double v) {
    (void)k;
    if (v < 0) return -1;
    openpete_runahead_set((int)v);
    return 0;
}

/** @brief The native field of view in degrees; 0 selects the game's own. */
static double fov_get(const knob_desc_t *k) { (void)k; return (double)openpete_native_fov_deg(); }
/** @brief Set the native field of view from the fov row: 0 (the game's own) or [30, 120] degrees; anything else, NaN included, is refused. */
static int fov_set(knob_desc_t *k, double v) {
    if (!isfinite(v) || v < 0.0 || (v > 0.0 && v < 30.0) || v > 120.0) return -1;
    openpete_native_set_fov((float)v);
    k->stored_num = v;
    return 0;
}

/** @brief 1 while the host loop has the sim paused, else 0. */
static double pause_get(const knob_desc_t *k) { (void)k; return openpete_sim_paused(); }
/** @brief Request the sim paused (`v` nonzero) or resumed, as the pause key does. */
static int pause_set(knob_desc_t *k, double v) { (void)k; openpete_sim_pause_request(v != 0.0); return 0; }
/** @brief The Step tick button's gate: 1 while the sim is paused, else 0. */
static double step_ready(const knob_desc_t *k) { (void)k; return openpete_sim_paused(); }
/** @brief Request one tick of the paused sim, as the step key does. */
static int step_fire(knob_desc_t *k, double v) { (void)k; (void)v; openpete_sim_step_request(); return 0; }

/** @brief Request a rewind window of `v` seconds, truncated to int.
 *
 * Returns -1, changing nothing, when `v` is negative. Otherwise the host loop
 * resizes the ring at its next iteration, discarding the accumulated rewind
 * history (0 disables rewind); the row stores the value and 0 is returned. */
static int rewind_set(knob_desc_t *k, double v) {
    if (v < 0) return -1;
    openpete_rewind_window_set((int)v);
    k->stored_num = (int)v;
    return 0;
}

/* ---- Audio ---- */

/** @brief 1 when host audio is muted, else 0. */
static double mute_get(const knob_desc_t *k) { (void)k; return openpete_get_mute() ? 1.0 : 0.0; }
/** @brief Mute or unmute host audio from `v` (nonzero mutes). */
static int mute_set(knob_desc_t *k, double v) { (void)k; openpete_set_mute(v != 0.0); return 0; }

/** @brief The requested in-game SFX slider preset.
 *
 * The SFX and music setters below move only a host-side request; the game's
 * tick hook writes the guest slider, so the rows keep the live-row contract
 * although the value ends up in gameplay-readable RAM. */
static double sfxvol_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_sfx_volume(); }
/** @brief Request SFX slider preset `v`, truncated to int; the runtime clamps it. */
static int sfxvol_set(knob_desc_t *k, double v) { (void)k; openpete_set_sfx_volume((int)v); return 0; }
/** @brief The requested music volume, read from the runtime. */
static double musicvol_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_music_volume(); }
/** @brief Request music volume `v`, truncated to int; the runtime clamps it. */
static int musicvol_set(knob_desc_t *k, double v) { (void)k; openpete_set_music_volume((int)v); return 0; }

/* ---- Input ---- */

/** @brief The freecam mouse sensitivity. */
static double mousesens_get(const knob_desc_t *k) { (void)k; return (double)openpete_freecam_mouse_sensitivity(); }
/** @brief Set the freecam mouse sensitivity to `v`.
 *
 * Returns -1, changing nothing, unless `v` lies in [0, 100] (NaN fails); else
 * stores it in the row and returns 0. */
static int mousesens_set(knob_desc_t *k, double v) {
    if (!(v >= 0.0) || v > 100.0) return -1;
    openpete_freecam_set_mouse_sensitivity((float)v);
    k->stored_num = v;
    return 0;
}
/** @brief 1 when host mouse input is enabled, else 0. */
static double mouse_get(const knob_desc_t *k) { (void)k; return openpete_host_mouse_enabled() ? 1.0 : 0.0; }
/** @brief Enable or disable host mouse input from `v` (nonzero enables). */
static int mouse_set(knob_desc_t *k, double v) { (void)k; openpete_host_mouse_set_enabled(v != 0.0); return 0; }
/** @brief 1 when gamepad input is accepted while the window is unfocused, else 0. */
static double padunf_get(const knob_desc_t *k) { (void)k; return openpete_host_input_pad_unfocused() ? 1.0 : 0.0; }
/** @brief Accept or refuse unfocused gamepad input from `v` (nonzero accepts). */
static int padunf_set(knob_desc_t *k, double v) { (void)k; openpete_host_input_set_pad_unfocused(v != 0.0); return 0; }
/** @brief The host rumble strength.
 *
 * The setter moves this host request; the per-game feature writes the guest
 * enable flag on its tick hook. */
static double vibration_get(const knob_desc_t *k) { (void)k; return (double)openpete_get_vibration(); }
/** @brief Request rumble strength `v`, truncated to int; the runtime clamps it. */
static int vibration_set(knob_desc_t *k, double v) { (void)k; openpete_set_vibration((int)v); return 0; }

/* ---- Recording ---- */

/** @brief 1 when the Recording rows are shown: an encoder is built in and --record named a path.
 *
 * --record is latched at boot, so a run with no path never encodes and every
 * row in the section is inert; on a build with no video encoder --record only
 * warns. The rows are hidden rather than disabled because neither case can
 * be changed from the overlay. */
static int recording_visible(const knob_desc_t *k) {
    (void)k;
    if (!openpete_record_supported()) return 0;
    return g_opts && g_opts->record_path && g_opts->record_path[0];
}

/** @brief Enable or disable drawing the fps readout into recorded video from `v` (nonzero enables).
 *
 * Stores the resulting 0 or 1 in the row. */
static int recfps_set(knob_desc_t *k, double v) {
    openpete_native_set_fps_in_record(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}

/* ---- Paths (boot-resolved; display the same precedence the consumer used) */

/* The options string field behind each row whose value is a parsed string
 * with no live channel, by row id. */
static const struct { const char *id; size_t off; } k_optstr_fields[] = {
    { "disc",                 offsetof(openpete_options_t, disc_path) },
    { "texpack_dir",          offsetof(openpete_options_t, texpack_dir) },
    { "sfxpack_dir",          offsetof(openpete_options_t, sfxpack_dir) },
    { "controller_db",        offsetof(openpete_options_t, controller_db) },
    { "mods_dir",             offsetof(openpete_options_t, mods_dir) },
    { "mods",                 offsetof(openpete_options_t, mods_enabled) },
    { "card_dir",             offsetof(openpete_options_t, card_dir) },
    { "card1",                offsetof(openpete_options_t, card1_path) },
    { "card2",                offsetof(openpete_options_t, card2_path) },
    { "ingest",               offsetof(openpete_options_t, ingest_path) },
    { "game",                 offsetof(openpete_options_t, game_selector) },
    { "record",               offsetof(openpete_options_t, record_path) },
    { "record_encoder",       offsetof(openpete_options_t, record_encoder) },
    { "record_pixfmt",        offsetof(openpete_options_t, record_pixfmt) },
    { "record_audio_encoder", offsetof(openpete_options_t, record_audio_encoder) },
    { "bk2",                  offsetof(openpete_options_t, bk2_path) },
    { "bk2_record",           offsetof(openpete_options_t, bk2_record_path) },
    { "hide_classes",         offsetof(openpete_options_t, hide_classes) },
    { "gpu",                  offsetof(openpete_options_t, gpu) },
};

/** @brief The parsed options string behind row `k`, or NULL when it is unset, the options are unbound, or the row has no k_optstr_fields entry. */
static const char *optstr_field(const knob_desc_t *k) {
    if (!g_opts) return NULL;
    for (size_t i = 0; i < sizeof k_optstr_fields / sizeof k_optstr_fields[0]; i++)
        if (!strcmp(k->id, k_optstr_fields[i].id))
            return *(const char *const *)((const char *)g_opts + k_optstr_fields[i].off);
    return NULL;
}

/** @brief Writes a string row's boot value: the parsed value, else the row's env var, else "(default)".
 *
 * The parsed value comes from the options field k_optstr_fields names for
 * the row id; an explicit empty value (such as --mods "") displays as
 * "(none)". */
static void optstr_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *cli = optstr_field(k);
    if (cli && !*cli) { /* explicit empty value, e.g. --mods "" */
        snprintf(buf, cap, "(none)");
        return;
    }
    const char *env = (k->env && !cli) ? getenv(k->env) : NULL;
    snprintf(buf, cap, "%s", cli ? cli : (env ? env : "(default)"));
}

/* ---- Launch-only rows over parsed options ----
 * Flags read once at startup with no live channel: each getter returns the
 * parsed value, 0 before the options are bound. */

/** @brief Writes the config files the launch read: the --config (or OPENPETE_CONFIG) file, else the exe-directory and per-user openpete.toml, comma-separated; "(none)" under --no-config. */
static void configfile_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *a = openpete_config_file_path(0), *b = openpete_config_file_path(1);
    if (a && b) snprintf(buf, cap, "%s, %s", a, b);
    else        snprintf(buf, cap, "%s", a ? a : (b ? b : "(none)"));
}

/** @brief 1 when --no-log-file turned the log file off, else 0. */
static double nologfile_get(const knob_desc_t *k) {
    (void)k;
    return (g_opts && g_opts->log_file && !strcmp(g_opts->log_file, "off")) ? 1.0 : 0.0;
}
/** @brief 1 when --compile-mods was given, else 0. */
static double compilemods_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->compile_mods : 0.0; }
/** @brief 1 when --no-card removed both memory cards, else 0. */
static double nocard_get(const knob_desc_t *k) { (void)k; return (g_opts && !g_opts->card_a_present) ? 1.0 : 0.0; }
/** @brief 0 when the run turned the mod guard's compares off (--scan-guard 0), else 1. */
static double scanguard_get(const knob_desc_t *k) { (void)k; return (g_opts && g_opts->scan_guard == 0) ? 0.0 : 1.0; }
/** @brief 1 when the run arms the mod hot-reload watch (--mod-hot-reload or OPENPETE_MOD_HOT_RELOAD), else 0. */
static double modhot_get(const knob_desc_t *k) { (void)k; return (g_opts && g_opts->mod_hot_reload > 0) ? 1.0 : 0.0; }
/** @brief 1 when port A holds no memory card (--no-card or --no-card1), else 0. */
static double nocard1_get(const knob_desc_t *k) { (void)k; return (g_opts && !g_opts->card_a_present) ? 1.0 : 0.0; }
/** @brief 1 when port B holds no memory card (--no-card or --no-card2), else 0. */
static double nocard2_get(const knob_desc_t *k) { (void)k; return (g_opts && !g_opts->card_b_present) ? 1.0 : 0.0; }
/** @brief 1 when --card-unformatted was given, else 0. */
static double cardunf_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->card_unformatted : 0.0; }
/** @brief 1 when the save-mismatch toast is on, else 0; the backing flag is seeded from the options at boot. */
static double mismatch_toast_get(const knob_desc_t *k) { (void)k; return openpete_memcard_mismatch_toast() ? 1.0 : 0.0; }
/** @brief Turn the save-mismatch toast on or off from `v` (nonzero enables). */
static int mismatch_toast_set(knob_desc_t *k, double v) {
    openpete_memcard_set_mismatch_toast(v != 0.0);
    k->stored_num = (v != 0.0);
    return 0;
}
/** @brief 1 when --adopt-saves was given, else 0. */
static double adopt_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->adopt_saves : 0.0; }
/** @brief The parsed --bk2-frame-offset value. */
static double bk2offset_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->bk2_frame_offset : 0.0; }
/** @brief 1 when --bk2-stop-on-end was given, else 0. */
static double bk2stop_get(const knob_desc_t *k) { (void)k; return (g_opts && g_opts->bk2_stop_on_end) ? 1.0 : 0.0; }
/** @brief The parsed --record-crf value. */
static double reccrf_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->record_crf : 0.0; }
/** @brief The parsed --record-gop value. */
static double recgop_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->record_gop : 0.0; }
/** @brief 1 when --no-record-audio was given, else 0. */
static double norecaudio_get(const knob_desc_t *k) { (void)k; return (g_opts && g_opts->record_no_audio) ? 1.0 : 0.0; }
/** @brief The parsed --psycross-native-raster value. */
static double pcnative_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->psycross_native_raster : 0.0; }

/* ---- CLI debug rows (boot-parsed; display-only) ---- */

/** @brief The row's env var value, or "(off)" when it is unset or empty. */
static void injectmouse_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *env = k->env ? getenv(k->env) : NULL;
    snprintf(buf, cap, "%s", (env && *env) ? env : "(off)");
}

/** @brief Write the --render-paths list into `buf`.
 *
 * Writes "(all)" when options are unbound or the list is empty. */
static void renderpaths_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = (g_opts && g_opts->render_paths && *g_opts->render_paths)
                        ? g_opts->render_paths : NULL;
    snprintf(buf, cap, "%s", v ? v : "(all)");
}

/** @brief Write the active log level spec into `buf`, or "(default: info)" when the spec is empty. */
static void loglevel_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = openpete_log_get_spec();
    snprintf(buf, cap, "%s", (v && *v) ? v : "(default: info)");
}

/** @brief Write the log file path into `buf`, or "(no log file)" when none is open. */
static void logfile_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const char *v = openpete_log_file_path();
    snprintf(buf, cap, "%s", (v && *v) ? v : "(no log file)");
}

/** @brief Apply `v` as the log level spec.
 *
 * A value starting with `(` is the display placeholder and clears the spec, as
 * does NULL. Always returns 0. */
static int loglevel_set_str(knob_desc_t *k, const char *v) {
    (void)k;
    if (v && *v == '(') v = "";   /* "(default: info)" placeholder = clear */
    openpete_log_set_spec(v);
    return 0;
}

/** @brief Write the wanted --resolution value in the flag's spelling: window, N or WxH.
 * @param k The row; unused.
 * @param buf Receives the text.
 * @param cap The capacity of buf. */
static void resolution_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    const openpete_render_res_t v = openpete_render_res_unpack(openpete_native_get_resolution());
    openpete_render_res_format(&v, buf, cap);
}

/** @brief Parse a --resolution value with the flag's parser and hand it to the renderer, which applies it at the next present.
 * @param k The row; unused.
 * @param v The text: window, N or WxH.
 * @return 0 when accepted (N above the maximum is clamped), -1 when refused. */
static int resolution_set_str(knob_desc_t *k, const char *v) {
    (void)k;
    openpete_render_res_t rv;
    char msg[160];
    const int rc = openpete_render_res_parse(v, &rv, msg, sizeof msg);
    if (msg[0]) fprintf(stderr, "openpete: %s\n", msg);
    if (rc < 0) return -1;
    openpete_native_set_resolution(openpete_render_res_pack(&rv));
    return 0;
}

static const char *const k_logfmt_names[] = { "plain", "json" };
/** @brief The active log format as its k_logfmt_names index. */
static double logfmt_get(const knob_desc_t *k) {
    (void)k; return (double)openpete_log_get_format();
}
/** @brief Select the log format: `v` of 1 is JSON, any other value is plain. */
static int logfmt_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_log_set_format((int)v == 1 ? OPENPETE_LOG_JSON : OPENPETE_LOG_PLAIN);
    return 0;
}

/* ---- Debug env rows ----
 *
 * Generic rows over raw OPENPETE_* env strings: display is the live env
 * value ("(unset)" when absent). Editability follows the consumer pattern,
 * proven by grep per var:
 *  - FRESH consumers call getenv per use, so setenv IS their live channel
 *    (editable, KNOB_LIVE);
 *  - CACHED consumers latch the value into a function-local static on first
 *    use - setenv would silently do nothing, so those rows stay display-only
 *    (KNOB_RESTART_ONLY) until the latch is converted to an accessor.
 *    Promoting one is a per-knob audited change, not an overlay tweak. */

/* Bumped on every live env edit. A consumer that re-resolves a knob "once
 * per VBL" pays a getenv per VBL on every thread that reads it; keyed on
 * this epoch it pays one per EDIT. On Windows that matters twice over: the
 * CRT's getenv is ~46x glibc's, and concurrent getenv/putenv from two
 * threads has been observed to wedge the process. */
static atomic_uint s_env_epoch = 1;
unsigned knob_registry_env_epoch(void) {
    return atomic_load_explicit(&s_env_epoch, memory_order_acquire);
}

/* The guard every live env write runs inside (knob_registry_set_env_guard). */
static void (*s_env_guard)(int begin);

void knob_registry_set_env_guard(void (*guard)(int begin)) { s_env_guard = guard; }

/** @brief Write or unset an env var inside the env guard, then bump the registry env epoch.
 *
 * The epoch moves after the write with release order, so a consumer on
 * another thread that loads the new epoch (acquire) and then calls getenv
 * reads the new value; bumped before the write, a consumer could cache the
 * old value under the new epoch and keep it until the next edit.
 * @param name Variable name.
 * @param val New value; NULL or "" unsets the variable.
 * @return 0 on success, else the platform call's failure value. */
static int portable_setenv(const char *name, const char *val) {
    if (s_env_guard) s_env_guard(1);
#ifdef _WIN32
    const int rc = _putenv_s(name, val ? val : "");
#else
    const int rc = (!val || !*val) ? unsetenv(name) : setenv(name, val, 1);
#endif
    if (s_env_guard) s_env_guard(0);
    atomic_fetch_add_explicit(&s_env_epoch, 1u, memory_order_release);
    return rc;
}

/** @brief Write the row's env var value into `buf`.
 *
 * An unset variable shows as "(unset - <default>)" when the row has a non-empty
 * default string, else "(unset)". */
static void env_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    const char *v = k->env ? getenv(k->env) : NULL;
    if (v)
        snprintf(buf, cap, "%s", v);
    else if (k->def_str && *k->def_str)
        snprintf(buf, cap, "(unset - %s)", k->def_str);
    else
        snprintf(buf, cap, "(unset)");
}

/** @brief Set the row's env var to `v`, through portable_setenv.
 *
 * Returns -1 for a row with no env var. A value starting with "(unset" (the
 * display placeholder), an empty value and NULL unset the variable. Otherwise
 * returns the portable_setenv result. */
static int env_set_str(knob_desc_t *k, const char *v) {
    if (!k->env) return -1;
    /* Editing back to the "(unset...)" placeholder (or clearing) unsets. */
    if (v && !strncmp(v, "(unset", 6)) v = "";
    return portable_setenv(k->env, v);
}

/* Compact row builders for the Debug sections. LIVE = FRESH consumer.
 * Designated, so every member they leave out is zero. */
#define ENVROW_LIVE(name, sect, def, helptext) \
    { .id = name, .label = name, .section = sect, .env = "OPENPETE_" name, \
      .help = helptext, .type = KNOB_STRING, .mut = KNOB_LIVE, \
      .get_str = env_get_str, .set_str = env_set_str, \
      .boot_src = KNOB_SRC_DEFAULT, .def_str = def }
#define ENVROW_LATCHED(name, sect, def, helptext) \
    { .id = name, .label = name, .section = sect, .env = "OPENPETE_" name, \
      .help = helptext, .type = KNOB_STRING, .mut = KNOB_RESTART_ONLY, \
      .get_str = env_get_str, \
      .boot_src = KNOB_SRC_DEFAULT, .def_str = def }
/* An ENVROW_LATCHED row read while the device is created: device-creation
 * class 1 (openpete_knobs.h device_creation). */
#define ENVROW_DEVICE(name, sect, def, helptext) \
    { .id = name, .label = name, .section = sect, .env = "OPENPETE_" name, \
      .help = helptext, .type = KNOB_STRING, .mut = KNOB_RESTART_ONLY, \
      .get_str = env_get_str, \
      .boot_src = KNOB_SRC_DEFAULT, .def_str = def, .device_creation = 1 }
/* An ENVROW_LATCHED row whose unset value the program decides: `note` says
 * how, and the overlay shows "(unset)". */
#define ENVROW_LATCHED_NOTE(name, sect, note, helptext) \
    { .id = name, .label = name, .section = sect, .env = "OPENPETE_" name, \
      .help = helptext, .type = KNOB_STRING, .mut = KNOB_RESTART_ONLY, \
      .get_str = env_get_str, \
      .boot_src = KNOB_SRC_DEFAULT, .default_note = note }

/* ---- Gameplay / debug ---- */

/** @brief The parsed --skip-to-level value, 0 before options are bound. */
static double skiplevel_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->skip_to_level : 0.0; }
/** @brief The parsed --skip-to-cutscene value, 0 before options are bound. */
static double skipcutscene_get(const knob_desc_t *k) { (void)k; return g_opts ? (double)g_opts->skip_to_cutscene : 0.0; }

/** @brief Write the parsed --skip-to-area value into `buf`.
 *
 * The form is "<level>:<area>"; "(none)" when options are unbound or no level
 * was given. */
static void skiparea_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    if (g_opts && g_opts->skip_to_area_level > 0)
        snprintf(buf, cap, "%d:%d", g_opts->skip_to_area_level, g_opts->skip_to_area);
    else
        snprintf(buf, cap, "(none)");
}

/** @brief Write the parsed --cheats string into `buf`, or "(none)" when options are unbound or it is empty. */
static void cheats_get_str(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k;
    snprintf(buf, cap, "%s",
             (g_opts && g_opts->cheats && g_opts->cheats[0]) ? g_opts->cheats
                                                             : "(none)");
}

/** @brief 1 when the freecam is active, else 0. */
static double freecam_get(const knob_desc_t *k) { (void)k; return openpete_freecam_active() ? 1.0 : 0.0; }
/** @brief Toggle the freecam when `v` differs from its current state. */
static int freecam_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_freecam_active() != 0))
        openpete_freecam_toggle();
    return 0;
}

static const char *const k_state_slot_names[OPENPETE_STATE_NUM_SLOTS] = {
    "Slot 0", "Slot 1", "Slot 2", "Slot 3", "Slot 4",
    "Slot 5", "Slot 6", "Slot 7", "Slot 8", "Slot 9",
};

/** @brief The active savestate slot's index. */
static double state_slot_get(const knob_desc_t *k) { (void)k; return (double)openpete_state_active_slot(); }
/** @brief Make slot `v` the active one, as the slot-cycle key does. */
static int state_slot_set(knob_desc_t *k, double v) {
    (void)k;
    if (v < 0.0 || v >= (double)OPENPETE_STATE_NUM_SLOTS) return -1;
    openpete_state_set_active_slot((unsigned)v);
    return 0;
}
/** @brief Queue a save to the active slot, as the quick-save key does. */
static int state_save_fire(knob_desc_t *k, double v) { (void)k; (void)v; openpete_state_request(0); return 0; }
/** @brief 1 when the active slot holds a state, so Load is enabled, else 0. */
static double state_load_ready(const knob_desc_t *k) { (void)k; return openpete_state_active_slot_full() ? 1.0 : 0.0; }
/** @brief Queue a load from the active slot, as the quick-load key does. */
static int state_load_fire(knob_desc_t *k, double v) { (void)k; (void)v; openpete_state_request(1); return 0; }

/** @brief 1 when the fill-tag picker is active, else 0. */
static double fillpick_get(const knob_desc_t *k) { (void)k; return openpete_fillpick_active() ? 1.0 : 0.0; }
/** @brief Activate or deactivate the fill-tag picker from `v` (nonzero activates). */
static int fillpick_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_fillpick_set_active(v != 0.0);
    return 0;
}

/** @brief 1 when the fill-tag picker preview is on, else 0. */
static double fillpick_prev_get(const knob_desc_t *k) { (void)k; return openpete_fillpick_preview() ? 1.0 : 0.0; }
/** @brief Turn the fill-tag picker preview on or off from `v` (nonzero is on). */
static int fillpick_prev_set(knob_desc_t *k, double v) {
    (void)k;
    openpete_fillpick_set_preview(v != 0.0);
    return 0;
}

/** @brief 1 when the camera lock is active, else 0. */
static double camlock_get(const knob_desc_t *k) { (void)k; return openpete_camlock_active() ? 1.0 : 0.0; }
/** @brief Toggle the camera lock when `v` differs from its current state.
 *
 * Engaging the lock while the freecam is active first toggles the freecam off,
 * because both drive the fly axes. */
static int camlock_set(knob_desc_t *k, double v) {
    (void)k;
    if ((v != 0.0) != (openpete_camlock_active() != 0)) {
        if (v != 0.0 && openpete_freecam_active())
            openpete_freecam_toggle();   /* the two share the fly axes */
        openpete_camlock_toggle();
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the table                                                          */
/* ------------------------------------------------------------------ */

#define ENUM_ROW(vals) vals, (int)(sizeof(vals)/sizeof((vals)[0]))

/* Built-in rows outside the debug block. The registry hands out g_rows (these,
 * the debug rows, and whatever game modules contributed through
 * openpete_knob_add), never this array directly. */
static const knob_desc_t g_builtin[] = {
    /* -------- Display -------- */
    { "fullscreen", "Fullscreen", "Display", "--fullscreen", NULL,
      "Borderless-desktop fullscreen (Alt+Enter and a double-click toggle it too).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fullscreen_get, fullscreen_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 110,
      .help_text = "launch in borderless-desktop fullscreen (default {def} = windowed;\n"
          "on|off also accepted). Alt+Enter, a double-click and the\n"
          "settings menu's DISPLAY row toggle it live.",
      .key_action = "fullscreen" },

    { "aspect", "Aspect ratio", "Display", "--aspect", NULL,
      "Hor+ render aspect; auto follows the window shape. Picking a fixed "
      "ratio windowed resizes the window to match.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_aspect_names), 0, 0,
      aspect_get, aspect_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "auto",
      .help_arg = "W:H|auto", .help_order = 70,
      .help_text = "Hor+ widescreen. {def} (default) = follow the window/fullscreen\n"
          "shape, filling it live on resize; W:H (e.g. 16:9, 21:9) = fixed\n"
          "render aspect (sizes the window; bars when the window mismatches)" },

    { "fov", "Field of view", "Display", "--fov", NULL,
      "Vertical FOV of the visible band in degrees, 30-120 (0 = the game's "
      "projection, 55). The horizontal FOV follows the Hor+ aspect: 74 at "
      "4:3, 90 at 16:9 for the default. Native renderer only; applies at "
      "the next present.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 30, 120,
      fov_get, fov_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .zero_label = "Original",
      .def_str = "0",
      .help_arg = "DEG", .help_order = 80,
      .help_text = "vertical FOV of the visible band, native renderer (default 55°,\n"
          "range 30-120; the horizontal FOV follows the Hor+ aspect: 74° at\n"
          "4:3, 90° at 16:9 for the default). Inert under --renderer psycross." },

    { "resolution", "Resolution", "Display", "--resolution", NULL,
      "Pixel count the 3D scene renders at: the window's own size, N times "
      "the game's original raster, or exact WxH pixels. The picture keeps "
      "the window's shape; supersampling stacks on top. Applying reallocates "
      "the render targets next present.",
      KNOB_STRING, KNOB_LIVE, NULL, 0, 0, 0,
      NULL, NULL, resolution_get_str, resolution_set_str, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "window", .control_str = "2",
      .help_arg = "window|N|WxH", .help_order = 88,
      .help_text = "pixel count of the native 3D scene (default {def}): window = the\n"
          "window's size; N = N times the game's original raster (1..8; Spyro 1\n"
          "512x224, Spyro 2 and 3 512x216, widened for Hor+); WxH = exact\n"
          "pixels (64..16384 a side). The scene is stretched to the window's\n"
          "picture area; --ssaa supersamples on top of it." },

    { "ssaa", "Supersampling", "Display", "--ssaa", NULL,
      "Native supersampling factor. Applying reallocates the render target "
      "next present (brief hitch at large factors; GPU-max capped).",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_ssaa_names), 0, 0,
      ssaa_get, ssaa_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "N", .help_order = 90,
      .help_text = "native vk supersampling 1..4 (default {def}=off); renders the live\n"
          "window at NxN res and box-downsamples. The 4x ceiling keeps the\n"
          "render targets inside an 8GB card at 1080p; higher factors are\n"
          "clamped, and the extent is further capped to the GPU's max image\n"
          "size." },

    { "vsync", "VSync", "Display", "--vsync", "OPENPETE_PRESENT_MODE",
      "FIFO (on) vs immediate/mailbox (off). Applying rebuilds the swapchain "
      "on the next frame.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      vsync_get, vsync_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "on (FIFO), unless OPENPETE_PRESENT_MODE or the device selects another present mode",
      .device_creation = 2,
      .help_arg = "0|1", .help_order = 100,
      .help_text = "native vk present mode (default 1 = vsync on / FIFO; 0 = vsync off\n"
          "for uncapped fps / low latency; on|off also accepted)." },

    { "gpu", "GPU", "Display", "--gpu", "OPENPETE_VK_DEVICE",
      "The Vulkan device the native renderer uses: its index in the log's "
      "gpu list, or part of its name (case-insensitive). A value that "
      "matches no device, or names one that cannot draw to the window, is "
      "logged and the automatic choice is used.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "unset: chosen automatically, a discrete GPU first, then integrated, virtual, other, then a software rasterizer",
      .device_creation = 1,
      .help_arg = "INDEX|NAME", .help_order = 102,
      .help_text = "Vulkan device to render on: its index in the log's gpu list, or a\n"
          "case-insensitive part of its name (default: automatic, a discrete GPU\n"
          "first; env OPENPETE_VK_DEVICE). No match falls back to automatic" },

    { "perf_stats", "Performance stats", "Display", "--perf-stats", NULL,
      "Corner overlay (native renderer): off, the FPS counter, or the "
      "counter plus a pacing readout - VBL rate, deadline slips, late ticks "
      "per 1000, and the shed / interpolation share of presents.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_perf_names), 0, 0,
      perfstats_get, perfstats_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "fps",
      .help_arg = "LEVEL", .help_order = 130,
      .help_text = "on-screen performance stats: off | {def} (default) | detailed.\n"
          "detailed adds the VBL rate, slips, late ticks per 1000 and the\n"
          "shed / interpolation share. 0|1|2 also accepted." },

    { "perf_position", "Stats position", "Display", "--perf-position", NULL,
      "Which corner or edge the performance-stats block anchors to. Shares "
      "the anchor ledger with the OSD: whichever surface claims a corner "
      "first, the other stacks clear of it.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_anchor_names), 0, 0,
      perfpos_get, perfpos_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "top-right",
      .help_arg = "ANCHOR", .help_order = 150,
      .help_text = "where the stats block sits: {def} (default) | top-left | top-center | bottom-left | bottom-center | bottom-right" },

    { "osd", "Notifications", "Display", "--osd", NULL,
      "On-screen notifications when a setting changes, in the game's own gold "
      "chrome. Mods raise them too.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      osd_get, osd_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "on",
      .help_arg = "on|off", .help_order = 160,
      .help_text = "on-screen notifications when a setting changes (default {def})" },

    { "version_osd", "Version line", "Display", "--version-osd", NULL,
      "A release build's two-second version line at the bottom left. A "
      "distributed build that is not a clean tag keeps its version up for the "
      "whole session and this does not turn it off, so a screenshot always "
      "names the build it came from; a locally built binary shows no line.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      versionosd_get, versionosd_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "on",
      .help_arg = "on|off", .help_order = 190,
      .help_text = "the release build's two-second version line at the bottom\n"
          "left (default {def}). A distributed build that is not a clean\n"
          "tag shows its version for the whole session and this does\n"
          "not suppress it; a locally built binary shows none." },

    { "osd_position", "Notification position", "Display", "--osd-position", NULL,
      "Which corner or edge notifications stack at. Sharing a corner with the "
      "stats block is fine: the stats block claims first and the toasts stack "
      "clear of it.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_anchor_names), 0, 0,
      osdpos_get, osdpos_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "top-left",
      .help_arg = "ANCHOR", .help_order = 170,
      .help_text = "where notifications stack; same six anchors (default {def})" },

    { "osd_timeout", "Notification timeout", "Display", "--osd-timeout", NULL,
      "Seconds a notification stays up before it fades. A caller may ask for "
      "its own; this is what it gets when it does not.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.5, 30.0,
      osdtimeout_get, osdtimeout_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "3.0",
      .help_arg = "SECONDS", .help_order = 180,
      .help_text = "how long a notification stays up (default {def}, max 30)" },

    { "vertex_precision", "Vertex precision", "Display", "--vertex-precision", NULL,
      "PSX integer-grid vertex snap (authentic polygon wobble) vs smooth "
      "subpixel.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_vprec_names), 0, 0,
      vprec_get, vprec_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "subpixel",
      .help_arg = "MODE", .help_order = 280,
      .help_text = "native vk vertex screen-coordinate precision: {def} (default =\n"
          "smooth sub-pixel projection) | integer (snap 3D-world vertices to the\n"
          "PSX 512x240 pixel grid = the original PS1 polygon wobble). 2D HUD stays\n"
          "smooth; depth/UVs untouched. Native renderer only." },

    { "raster_quantize", "Raster quantize", "Display", "--raster-quantize", NULL,
      "Point-resample the 3D scene to N x the PSX raster (1 = the authentic "
      "512x240 grid - 224 lines visible on screen, letterbands trimmed; "
      "0 = off). One sample per cell, as the hardware rasterizer produced, "
      "always aligned with the integer vertex snap.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 16,
      rquant_get, rquant_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "0 (off), or 1 under --vertex-precision integer",
      .help_arg = "N", .help_order = 290,
      .help_text = "point-resample the 3D scene (pre-HUD) to N x the PSX raster (1 =\n"
          "authentic 512x240 grid, of which the 224 visible lines span the\n"
          "screen - the letterband rows are trimmed, so on-screen this is\n"
          "512x224 as on a real TV; every N stays aligned with the\n"
          "--vertex-precision integer grid). 0 = off. Default: 1 under\n"
          "--vertex-precision integer, else off." },

    { "extract_threads", "Extraction threads", "Display", "--extract-threads", NULL,
      "Worker threads for the native render extraction (terrain sector walk). "
      "Shows the resolved count (0 = auto at launch). Output is byte-identical "
      "at any count; 1 = serial.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 1, 8,
      exthreads_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 300,
      .help_text = "worker threads for the native render extraction (terrain sector\n"
          "walk). 0 = auto (logical cores / 2, max 8), 1 = serial (no\n"
          "threads). Output is byte-identical at any count (item-order\n"
          "merge)." },

    { "terrain_cache", "Terrain GPU cache", "Display", "--terrain-cache", "OPENPETE_TERRAIN_CACHE",
      "Retained GPU terrain (default on): unclaimed sectors render from "
      "world-space vertex chunks with the per-face verdicts mirrored in the "
      "vertex stage; claimed sectors (near / blended-over-portal / safety) "
      "stay on the CPU walk. Shows whether the cache came up (needs a "
      "hardware GPU; software vk runs the CPU walk). 0 = kill switch.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      tercache_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "0|1", .help_order = 260,
      .help_text = "retained-terrain GPU cache: static sectors render from persistent\n"
          "GPU meshes with the per-face verdicts mirrored in the vertex\n"
          "stage (default {def}; needs a hardware GPU, else the CPU\n"
          "walk runs as before). 0 = kill switch." },

    { "sector_visibility", "Sector visibility", "Display", "--sector-visibility",
      "OPENPETE_SECTOR_VISIBILITY",
      "Which level sectors are candidates to draw. auto (default) is filled "
      "on Spyro 2 and 3 and frustum on Spyro 1, one setting for every game. "
      "frustum takes "
      "every sector whose bounding sphere is inside the displayed frame, "
      "hidden or not, so the geometry the original hides - the world outside "
      "the room the camera sits in, the side wedges of a frame wider than "
      "4:3 - draws and the depth buffer decides what is seen; a departure "
      "from the original presentation. game takes the game's own visibility "
      "verdict (faithful). filled (Spyro 2 and 3) takes the game's verdict "
      "plus the sectors the game's visibility data lists from both sides of "
      "the camera's cell, or from one side where the other is outside the "
      "data; a departure; frustum where the level has no visibility data. "
      "Every mode still culls by the frustum test, the per-sector tier "
      "verdicts, the retained GPU cache and the occlusion pre-pass.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_tervis_names), 0, 0,
      tervis_get, tervis_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "auto",
      .help_arg = "MODE", .help_order = 250,
      .help_text = "sector candidate set: {def} (default) = filled on Spyro 2/3,\n"
          "frustum on Spyro 1 | frustum = every sector whose\n"
          "bounding sphere is inside the displayed frame, hidden or not, so\n"
          "the geometry the original hides (inside-geometry views, the side\n"
          "wedges wider than 4:3) draws and the depth buffer decides what is\n"
          "seen - a departure from the original presentation | game = the\n"
          "game's own visibility verdict (faithful) | filled = the game's\n"
          "verdict plus the sectors its visibility data lists around the\n"
          "camera's cell (Spyro 2/3; a departure). Native renderer only." },

    ENVROW_LATCHED("TERRAIN_HDFOLD", "Display", "1",
      "HD texture-replacement fold (default on): with a texture pack active, "
      "pack-matched terrain faces bake replacement entries into the retained "
      "GPU cache (atlas + per-entry fog) instead of claiming their sectors "
      "back to the CPU walk; unbakeable faces (blended targets, overflow) "
      "still claim. Inert without a pack. 0 = kill switch (tier-1 per-sector "
      "claims)."),

    { "notex", "Untextured (lighting)", "Display", "--notex", "OPENPETE_NOTEX",
      "Debug view: draw the 3D scene with textures replaced by neutral grey, "
      "showing the raw gouraud vertex lighting. Cutout silhouettes stay real. "
      "Applying re-bakes every scene pipeline (one-time hitch).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      notex_get, notex_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 330,
      .help_text = "1 = draw the 3D scene with textures replaced by neutral grey,\n"
          "showing the raw vertex lighting; cutout silhouettes stay real.\n"
          "Default: env OPENPETE_NOTEX, else {def}. Native renderer only." },

    { "wireframe", "Wireframe (lighting)", "Display", "--wireframe", "OPENPETE_WIREFRAME",
      "Debug view: draw every 3D scene triangle as its three edges, coloured "
      "by the gouraud vertex lighting (textures replaced by neutral grey, as "
      "in the untextured view), so the terrain subdivision shows as drawn. "
      "Depth-tested: a line hides the lines behind it, not the faces. HUD and "
      "menus stay filled. Falls back to the filled untextured view on a GPU "
      "without line rasterization. Applying re-bakes every scene pipeline "
      "(one-time hitch).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      wireframe_get, wireframe_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 340,
      .help_text = "1 = draw every 3D scene triangle as its three edges, coloured by the\n"
          "vertex lighting (textures replaced by neutral grey, as NOTEX); the\n"
          "HUD and menus stay filled. Shows the terrain subdivision as drawn.\n"
          "Default: env OPENPETE_WIREFRAME, else {def}. Native renderer only." },

    { "dither", "Color depth", "Display", "--dither", NULL,
      "PS1 15-bit colour: quantize the 3D scene to RGB555 with the GPU's 4x4 "
      "ordered dither matrix on the PSX framebuffer grid (authentic banding + "
      "crosshatch).",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      dither_get, dither_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 310,
      .help_text = "1 = PS1 15-bit colour: quantize the 3D scene to RGB555 with the\n"
          "GPU's 4x4 ordered dither matrix on the PSX 512x240 framebuffer\n"
          "grid (the authentic banding + crosshatch the smooth float\n"
          "pipeline removed). {def} = off (default)." },

    { "hide_player", "Hide player", "Display", "--hide-player", NULL,
      "Draw the scene without the player: Spyro, his fire breath and ground "
      "shadow and Sparx are not drawn, nor the particles Spyro spawns "
      "(Spyro 2 and 3) and those Sparx spawns and his glow (Spyro 3). Every "
      "other actor, effect and the HUD stay; the game plays on as before. "
      "Native renderer only.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      hide_player_get, hide_player_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 315,
      .help_text = "1 = draw the scene without the player: Spyro, his fire breath\n"
          "and shadow, Sparx, Spyro's particles (Spyro 2 and 3), Sparx's\n"
          "particles and glow (Spyro 3). Other actors, effects and the HUD\n"
          "stay. {def} = off (default). Native renderer only." },

    { "texture_mapping", "Texture mapping", "Display", "--texture-mapping", NULL,
      "perspective = modern perspective-correct interpolation; affine = the "
      "PSX screen-linear texture swim/warp. Applying re-bakes every scene "
      "pipeline at the next present (brief hitch).",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_texmap_names), 0, 0,
      texmap_get, texmap_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "perspective",
      .help_arg = "MODE", .help_order = 320,
      .help_text = "texture/colour interpolation: {def} (default = modern\n"
          "perspective-correct) | affine (PSX screen-linear = the original\n"
          "PS1 texture swim/warp). Native renderer only." },

    { "lod", "LOD", "Display", "--lod", NULL,
      "original = authentic PSX detail bands; extended = full detail at any "
      "distance (triangle-bound in dense vistas). Fog follows the mode "
      "unless explicitly set.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_lod_names), 0, 0,
      lod_get, lod_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "original",
      .help_arg = "MODE", .help_order = 240,
      .help_text = "sector level-of-detail: {def} (default = authentic PSX LOD band) |\n"
          "extended (draw every visible sector at high-poly, out to the cull\n"
          "distance and beyond). Native renderer only. (--no-lod = extended.)",
      .key_action = "lod_toggle" },

    { "fog", "Fog", "Display", "--fog", NULL,
      "How distance fog is drawn: original = the game's per-corner fade and "
      "per-face palette steps; per-pixel = the same curve evaluated at every "
      "pixel; off = no distance fog. Follow LOD picks original under original "
      "LOD and per-pixel under extended, and keeps following LOD switches; "
      "any other entry stays as chosen.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_fog_names), 0, 0,
      fog_get, fog_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "follows --lod: original under original, per-pixel under extended",
      .help_arg = "MODE", .help_order = 245,
      .help_text = "distance fog: original (the game's per-corner fade and per-face\n"
          "palette steps) | per-pixel (the same curve at every pixel) | off.\n"
          "Default: follows --lod (original under original, per-pixel under\n"
          "extended). Native renderer only." },

    { "fog_scale", "Fog scale", "Display", "--fog-scale", NULL,
      "Fog-distance multiplier (1 = authentic curve, 100 = effectively off). "
      "Terrain re-resolves it every walk; a manual value takes fog off the "
      "LOD autopilot.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.25f, 128.0f,
      fogscale_get, fogscale_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "1",
      .help_arg = "N", .help_order = 270,
      .help_text = "terrain fog-distance multiplier: 1 = authentic PSX curve, N = fog\n"
          "saturates N x farther (100 ~= fog off, the far-detail look).\n"
          "Default: 1. Range [0.25,128]." },

    { "renderer", "Renderer", "Display", "--renderer", NULL,
      "Which rasterizer draws the picture: the native Vulkan renderer (vk, "
      "default) or the PsyCross GL reference (psycross). The native window "
      "displays either one.",
      KNOB_ENUM, KNOB_RESTART_ONLY, ENUM_ROW(k_renderer_names), 0, 0,
      renderer_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "vk",
      .help_arg = "MODE", .help_order = 50,
      .help_text = "which rasterizer draws: vk (native Vulkan{default_mark:vk}) | psycross\n"
          "(the GL reference{default_mark:psycross}). The native window displays either." },

    { "window", "Window size", "Display", "--window", NULL,
      "Launch window size (drag the window to resize live), in window "
      "coordinates: the desktop's scaled units on a scaled Wayland or macOS "
      "display, where the game draws at the full pixel density, else pixels. "
      "The default is the game's drawn viewport at two window rows per VRAM row.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, window_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "the game's drawn viewport at two window rows per VRAM row, widened to a fixed --aspect",
      .help_arg = "WxH|maximized", .help_order = 60,
      .help_text = "window size, or maximized to open the window maximized (default:\n"
          "the game's drawn viewport at 2x, 640x448 on Spyro 1, 640x432 on\n"
          "Spyro 2 and 3), in window coordinates: scaled units on a scaled\n"
          "Wayland or macOS display, else pixels. A size, maximize or\n"
          "fullscreen set in the launcher replaces this and --fullscreen for\n"
          "the game it starts" },

    { "background", "Background window", "Display", "--background", "OPENPETE_BACKGROUND",
      "Open the window visible and at its normal size but unfocused, not "
      "activated on show, and behind the other windows where the platform "
      "allows it; clicking it or its taskbar button focuses it. Presents, pacing and "
      "screenshots are unchanged. Wins over Fullscreen. Raising or "
      "focusing the window after launch does not change it.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0", .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .device_creation = 1,
      .help_arg = "[0|1]", .help_order = 120,
      .help_text = "open the window at its normal size and visible, but do not take\n"
          "focus, do not activate on show, and sit behind the other windows\n"
          "where the platform allows it. Presents, pacing and screenshots are\n"
          "unchanged; clicking the window or its taskbar button focuses it. Wins over\n"
          "--fullscreen. For an unattended windowed run alongside\n"
          "--input-lock (default {def} = off; ignored with --headless)." },

    { "launcher_fullscreen", "Launcher fullscreen", "Display", "--launcher-fullscreen",
      "OPENPETE_LAUNCHER_FULLSCREEN",
      "Open the launcher in borderless-desktop fullscreen. The game keeps "
      "its own Fullscreen setting unless fullscreen is toggled in the launcher.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      launcher_fullscreen_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 112,
      .help_text = "open the launcher in borderless-desktop fullscreen (default {def}\n"
          "= windowed; on|off also accepted). Alt+Enter or a double-click\n"
          "toggles it in the launcher; a toggle there carries to the game." },

    { "launcher_window", "Launcher window size", "Display", "--launcher-window",
      "OPENPETE_LAUNCHER_WINDOW",
      "The launcher window's first size, WxH or maximized. Unset, the "
      "launcher opens at --window's size when one is set, else 1280x800, "
      "fitted to the display. A resize or maximize made in the launcher "
      "carries to the game; this setting alone does not.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, launcher_window_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "1280x800, or --window's size and state when set",
      .help_arg = "WxH|maximized", .help_order = 62,
      .help_text = "the launcher window's first size, or maximized (default:\n"
          "--window's when set, else 1280x800). A resize or maximize in the\n"
          "launcher carries to the game" },

    { "render_paths", "Render paths", "Display", "--render-paths", NULL,
      "Draw only the named render paths (cyclorama,spyro,moby,terrain,"
      "shadow,flame,particles,tracer,transition,portal,glow,sparkle,"
      "moby_shadow,ui); none draws no path. Empty clears the selection. "
      "Changes only the picture.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, renderpaths_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "every path",
      .help_arg = "LIST", .help_order = 100,
      .help_text = "draw ONLY these paths, e.g.\n"
          "cyclorama,spyro,moby,terrain,shadow,flame,particles,glow,sparkle,\n"
          "tracer,transition,portal,moby_shadow,ui (default: every path).\n"
          "Lets a path be compared in isolation; the picture only" },

    /* -------- Pacing -------- */
    { "render_fps", "Render FPS", "Pacing", "--render-fps", NULL,
      "Target present rate (0 = legacy match-sim pacer). Above ~30 arms "
      "sub-tick interpolation; vsync clamps to refresh. Shows the live "
      "pacer target (an unset launch value resolves to display refresh).",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 1000,
      renderfps_get, renderfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "the display's refresh rate in a windowed run; 0 (the match-sim pacer) under --headless",
      .help_arg = "N", .help_order = 220,
      .help_text = "target render Hz (default: the display's refresh rate; 0=match\n"
          "sim/30fps legacy pacer, >0=wall-clock dual-pacer + interpolation)" },

    { "cadence", "Cadence", "Pacing", "--cadence", "OPENPETE_CADENCE",
      "How in-between pictures sit on the display's refresh grid (VSync on) "
      "when a tick affords fewer than the grid has refreshes. even shows "
      "every 2nd, 3rd... refresh so every picture is held equally long; fill "
      "draws every picture the tick affords and repeats the refreshes it "
      "misses one at a time, spread apart. Without VSync both are the same. "
      "Applies from the next tick.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_cadence_names), 0, 0,
      cadence_get, cadence_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "even",
      .help_arg = "MODE", .help_order = 225,
      .help_text = "with vsync on, when a tick affords fewer pictures than refreshes:\n"
          "{def} (default) = every Nth refresh, each picture held equally long |\n"
          "fill = every affordable picture, the missed refreshes spread apart.\n"
          "No effect with vsync off (both draw every affordable picture)" },

    { "tick_scale", "Tick scale", "Pacing", "--tick-scale", NULL,
      "Engine speed multiplier (slow-mo / speed-up; sim and presents "
      "together). Applies on the next host iteration; pacing only, the sim "
      "runs identical ticks on a stretched wall clock.",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, (float)OPENPETE_TICK_SCALE_MIN, (float)OPENPETE_TICK_SCALE_MAX,
      tickscale_get, tickscale_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1.0",
      .help_arg = "F", .help_order = 230,
      .help_text = "engine speed x F (default {def}); scales sim AND present cadence\n"
          "together so sub-tick frames-per-tick is invariant. 0.5 = half-\n"
          "speed slow-mo to inspect high-FPS interpolation." },

    { "pause", "Pause sim", "Pacing", NULL, NULL,
      "Pause the game's ticks while presents continue. Audio mutes and "
      "rumble stops while paused.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      pause_get, pause_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off",
      .key_action = "pause" },

    { "frame_step", "Step tick", "Pacing", NULL, NULL,
      "Advance the paused game by one tick.",
      KNOB_ACTION, KNOB_LIVE, NULL, 0, 0, 0,
      step_ready, step_fire, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .action_label = "Step tick",
      .key_action = "frame_step" },

    { "runahead", "Runahead", "Pacing", "--runahead", NULL,
      "Present-timeline lead of N sim ticks (latency cut, ~N extra ticks of "
      "CPU). Applies next host iteration; the mod-capability gate can "
      "refuse (row falls back to 0). Presents snap for one tick on change.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 3,
      runahead_get, runahead_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 210,
      .help_text = "lead the presented timeline by N sim ticks (default {def} = off).\n"
          "1 recommended: cancels the interpolation tick exactly - real-\n"
          "PSX input feel with high-fps motion, every animation frame\n"
          "shown. 2 = sub-console latency but input edges skip one\n"
          "animation onset frame (reads as 'uncanny'). Costs ~N extra\n"
          "sim ticks/tick of CPU." },

    { "rewind", "Rewind window", "Pacing", "--rewind", NULL,
      "Rewind window in seconds (0 = off). APPLYING CLEARS the "
      "accumulated rewind history; ~3 MiB per second, plus what "
      "enabled mods change per second.",
      KNOB_INT, KNOB_LIVE, NULL, 0, 0, 600,
      get_stored, rewind_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 200,
      .help_text = "hold the rewind key to rewind up to N seconds (default {def} = off;\n"
          "~3 MiB RAM per second of window, plus what enabled mods change\n"
          "per second; same-level only)",
      .key_action = "rewind" },

    /* -------- Audio -------- */
    { "mute", "Mute", "Audio", "--mute", NULL,
      "Mute all audio at the OpenAL layer (SPU SFX + XA music + WAV "
      "override). Leaving pause restores this setting.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      mute_get, mute_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 470,
      .help_text = "mute all audio at the OpenAL layer (default {def} = audio on; on|off also accepted)" },

    { "sfx", "SFX volume", "Audio", "--sfx", NULL,
      "Preset the game's own SOUND EFFECTS slider, 0 (OFF) to 10 (max); "
      "-1 leaves it alone. Stamped at a tick boundary and re-stamped over "
      "the title screen / save-file load, until the player moves that "
      "slider in the game's options menu.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      sfxvol_get, sfxvol_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "-1",
      .help_arg = "N", .help_order = 480,
      .help_text = "preset the game's SOUND EFFECTS slider: 0 (OFF) .. 10 (max), or on|off.\n"
          "Survives the title screen and a save-file load; the game's own options\n"
          "menu takes it back the moment you move that slider. -1 = don't preset" },

    { "music", "Music volume", "Audio", "--music", NULL,
      "Preset the game's own MUSIC slider, 0 (OFF) to 10 (max); -1 leaves "
      "it alone. Same stamping rules as the SFX row.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      musicvol_get, musicvol_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "-1",
      .help_arg = "N", .help_order = 490,
      .help_text = "preset the game's MUSIC slider, same scale and rules as --sfx" },

    /* -------- Input -------- */
    { "vibration", "Vibration", "Input", "--vibration", "OPENPETE_VIBRATION",
      "Controller rumble strength, 0 (off) to 10 (max); -1 leaves the game's "
      "own vibration setting alone (the default, which means on). 10 applies "
      "the measured motor response unscaled. The game's enable flag is stamped at a "
      "tick boundary and released if the player toggles vibration in the "
      "options menu; the motors are silent headless, and unfocused unless "
      "Pad while unfocused is on.",
      KNOB_INT, KNOB_LIVE, NULL, 0, -1, 10,
      vibration_get, vibration_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "-1",
      .help_arg = "N", .help_order = 500,
      .help_text = "controller rumble: 0 (off) .. 10 (max), or on|off.\n"
          "Default {def} leaves the game's own setting alone (= on);\n"
          "0 silences the motors without changing that setting.\n"
          "10 is the full measured response. Always silent when\n"
          "headless; silent while the window is not focused unless\n"
          "--pad-unfocused is on." },

    { "mouse", "Mouse", "Input", "--mouse", "OPENPETE_MOUSE",
      "The host mouse channel. Off drops every mouse motion, button and wheel "
      "event from a real device before any consumer sees it and refuses "
      "pointer capture, so the pointer over the window stays an ordinary "
      "desktop cursor and mouse look, the settings menu and the dev overlay "
      "receive nothing. The scripted injection stream is unaffected.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      mouse_get, mouse_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "0|1", .help_order = 510,
      .help_text = "host mouse channel: 0 ignores every mouse movement, button and\n"
          "wheel event and leaves the pointer a free desktop cursor over the\n"
          "window; mouse look, the settings menu pointer and the dev overlay\n"
          "all go quiet (default {def} = on; on|off also accepted)" },

    { "input_lock", "Input lock", "Input", "--input-lock", "OPENPETE_INPUT_LOCK",
      "Ignore every host input device: the keyboard (game keys and every "
      "hotkey, this overlay's included), the mouse (as Mouse off) and "
      "gamepads. A bk2 replay and the scripted injection stream still drive "
      "the run, and closing the window still exits. Set at launch only: "
      "switched on from here it would take away the input needed to switch "
      "it off.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0", .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .help_arg = "[0|1]", .help_order = 520,
      .help_text = "ignore every host input device: keyboard (game keys and every\n"
          "hotkey), mouse (as --mouse 0) and gamepads. A --bk2 replay still\n"
          "drives the game and closing the window still exits. For a window\n"
          "that must not react to host input (default {def} = off)" },

    { "pad_unfocused", "Pad while unfocused", "Input", "--pad-unfocused", "OPENPETE_PAD_UNFOCUSED",
      "Keep reading every controller while another window has focus, so a "
      "pad drives the game behind a timer, a split tool or a stream window; "
      "rumble follows. Off, the game ignores a controller while its window "
      "is in the background. Input lock still ignores every physical pad.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      padunf_get, padunf_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0", .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .help_arg = "[0|1]", .help_order = 525,
      .help_text = "keep reading controllers, and driving their rumble, while the\n"
          "window does not have focus: a pad plays the game behind a timer,\n"
          "a split tool or a stream window (default {def} = off; on|off also\n"
          "accepted). Keyboard and mouse still need focus" },

    { "stick_response", "Stick response", "Input", "--stick-response", NULL,
      "How a controller's analog stick position becomes the stick value the "
      "game reads. console (default) = the stick reaches the end of the "
      "range at about three quarters of its travel, the response the game "
      "is verified against. linear = the stick's travel spread evenly over "
      "the PSX range. Applies from the next pad sample; a keyboard key "
      "bound to the stick gives the full value under either, and movie "
      "replay is unaffected.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_stick_names), 0, 0,
      stick_get, stick_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "console",
      .help_arg = "MODE", .help_order = 526,
      .help_text = "analog stick response: {def} (default: full value at about\n"
          "three quarters of the travel) | linear (stick travel spread\n"
          "evenly)" },

    { "mouse_sensitivity", "Mouse sensitivity", "Input", "--mouse-sensitivity", NULL,
      "Mouse look scale for the free camera and the camlock: angle units per "
      "count = 0.5 * this, so 1.0 is one full turn per 8192 counts. Read at "
      "every apply; a change rebases the mouse term, so it never rotates the "
      "view retroactively. Mouse look itself binds through the look_axis_x/y "
      "keymap actions (mouse:x, mouse:-y).",
      KNOB_FLOAT, KNOB_LIVE, NULL, 0, 0.0, 100.0,
      mousesens_get, mousesens_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1.0",
      .help_arg = "S", .help_order = 530,
      .help_text = "mouse look scale for the free camera and camlock (default {def};\n"
          "0.5 = one full turn per 8192 counts). Mouse look binds through\n"
          "[keys.host] look_axis_x = \"pad:rightx,mouse:x\" and\n"
          "look_axis_y = \"pad:-righty,mouse:-y\"; \"mouse:y\" inverts, \"none\" unbinds" },

    /* -------- Paths -------- */
    { "disc", "Disc image", "Paths", "--disc", NULL,
      "BIN/CUE the game boots from.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the launcher's pick from the game library",
      .help_arg = "PATH", .help_order = 10,
      .help_text = "disc image: the .cue, or the .bin/.img/.iso itself. A cue\n"
          "whose FILE line names a missing bin falls back to the cue's\n"
          "own basename and then to the lone .bin beside it." },

    { "disc_cache", "Disc cache", "Paths", "--disc-cache", "OPENPETE_DISC_CACHE",
      "Disc image resident in memory: a background loader fills a host "
      "array in the game's boot order; a resident sector is a memcpy and a "
      "missing one is read from the file at once (a read over a network "
      "share costs tens of ms per burst and lands as a late tick). auto "
      "(default) turns it on when the image sits on a network, VM-shared "
      "or optical filesystem (9p such as WSL's /mnt/c, NFS, SMB, FUSE; on "
      "Windows a UNC path, a mapped network drive or a CD-ROM drive); on "
      "any other disk it times the boot reads with the OS file cache "
      "bypassed and keeps the cache when the disk reads under 188 MB/s or "
      "a 256 KB read's p99 passes 1.39 ms, else frees it and turns it back "
      "on if the game's reads in one tick ever take over 8.36 ms. The log "
      "states the numbers and each decision. Shows whether the cache is "
      "attached. 0 = read the file per sector, 1 = always resident. The "
      "game reads the same bytes either way.",
      KNOB_ENUM, KNOB_RESTART_ONLY, ENUM_ROW(k_disccache_names), 0, 0,
      disccache_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "auto",
      .help_arg = "0|1|auto", .help_order = 30,
      .help_text = "keep the disc image resident in memory, filled by a background\n"
          "loader in the game's boot order: a resident sector is a copy, a\n"
          "missing one is read from the file at once. auto = on when the image\n"
          "is on a network, VM-shared or CD-ROM filesystem, off on a local\n"
          "disk; 0 = read the file per sector; 1 = always. Default {def}.\n"
          "The game reads the same bytes either way." },

    { "load_delay", "Load delay (VBLs)", "Paths", "--load-delay", "OPENPETE_LOAD_DELAY",
      "VBLs of guest time every level load waits: the load's disc reads land "
      "at once and their completion is deferred, so the game's own wait runs "
      "for the same count on every machine (0 = instant). Omitted = the game's "
      "default. A bk2 records the value and replays only under it. A "
      "load in flight keeps the value it started with.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 3600,
      loaddelay_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "N", .help_order = 40,
      .help_text = "VBLs of guest time every level load waits (the game's default\n"
          "when omitted; 0 = instant). Recorded into a bk2's header and\n"
          "checked on replay." },

    { "texpack_dir", "Texture pack dir", "Paths", "--texpack-dir", "OPENPETE_TEXPACK_DIR",
      "HD texture-replacement PNG directory, scanned when the renderer starts.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_TEXPACK_DIR, else assets/textures next to the executable",
      .help_arg = "PATH", .help_order = 360,
      .help_text = "directory of HD texture-replacement PNGs (default: env OPENPETE_TEXPACK_DIR,\n"
          "else <exe>/assets/textures/). Native renderer only." },

    { "controller_db", "Controller DB", "Paths", "--controller-db", "OPENPETE_CONTROLLER_DB",
      "Extra SDL controller-mapping database, loaded after the shipped "
      "gamecontrollerdb.txt and the per-user one and outranking both. A pad "
      "SDL has no mapping for never reaches the decode path; one line for one "
      "GUID is enough to fix that.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_CONTROLLER_DB, else none",
      .help_arg = "PATH", .help_order = 370,
      .help_text = "extra SDL controller-mapping database, loaded after the shipped one and\n"
          "the per-user gamecontrollerdb.txt, so it outranks both (default: env\n"
          "OPENPETE_CONTROLLER_DB, else neither)." },

    { "sfxpack_dir", "Sound pack dir", "Paths", "--sfxpack-dir", "OPENPETE_SFXPACK_DIR",
      "Drop-in sound-effect WAV directory (rescanned at every level load).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_SFXPACK_DIR, else assets/sfx next to the executable",
      .help_arg = "PATH", .help_order = 350,
      .help_text = "directory of drop-in sound-effect WAVs named by sound-effect id\n"
          "(default: env OPENPETE_SFXPACK_DIR, else assets/sfx next to the exe)" },

    { "mods_dir", "Mods dir", "Paths", "--mods-dir", "OPENPETE_MODS_DIR",
      "Mods root (mod.toml subdirs; load order via enabled.txt).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_MODS_DIR, else mods next to the executable",
      .help_arg = "PATH", .help_order = 380,
      .help_text = "mods root: each subdir with a mod.toml is a mod, compiled on load;\n"
          "enabled.txt fixes order (= priority). Default: env OPENPETE_MODS_DIR,\n"
          "else <exe>/mods. No directory = no mods, byte-identical vanilla." },

    { "mods", "Enabled mods", "Paths", "--mods", "OPENPETE_MODS",
      "Enabled modset override (comma-separated ids; order = priority). "
      "Empty = no mods. Unset = the mods dir's enabled.txt.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_MODS, else the mods directory's enabled.txt",
      .help_arg = "ID[,ID...]", .help_order = 390,
      .help_text = "run exactly these mods, overriding mods/enabled.txt (order IS\n"
          "priority, later = higher). --mods \"\" enables none (vanilla run).\n"
          "Default: env OPENPETE_MODS, else enabled.txt." },

    { "scan_guard", "Mod scan guard", "Paths", "--scan-guard", "OPENPETE_SCAN_GUARD",
      "Spyro 3 only: a mod's store into the code its anti-piracy checks "
      "hash is reverted and logged with the mod's name (default on). 0 "
      "skips the check, so such a store persists and can trip the game's "
      "anti-piracy measures. Accepted and without effect on Spyro the Dragon "
      "and Spyro 2.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      scanguard_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "1",
      .help_arg = "0|1", .help_order = 395,
      .help_text = "Spyro 3: revert and log a mod's store into the code the game's\n"
          "anti-piracy checks hash (default {def}). 0 = let such a store\n"
          "persist. No effect on Spyro 1 and 2." },

    { "mod_hot_reload", "Mod hot reload", "Paths", "--mod-hot-reload", "OPENPETE_MOD_HOT_RELOAD",
      "Watch every running mod with a src/ directory and reload one in "
      "place when its sources, the mod SDK, its config.toml or its shaders/ "
      "change, checked every 64 ticks. For mod authors.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      modhot_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0", .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .help_arg = "[0|1]", .help_order = 397,
      .help_text = "reload a running mod in place when its src/, the SDK, its\n"
          "config.toml or its shaders/ change, checked every 64 ticks\n"
          "(default {def} = off; for mod authors)" },

    { "card_dir", "Memory card dir", "Paths", "--card-dir", "OPENPETE_CARD_DIR",
      "Directory holding the .mcr memory-card images.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_CARD_DIR, else cards next to the executable in a portable install, else the per-user state directory",
      .help_arg = "PATH", .help_order = 430,
      .help_text = "directory holding the memory-card .mcr images (default: env OPENPETE_CARD_DIR,\n"
          "else <exe>/cards for a portable install, or the per-user state dir\n"
          "openpete/cards. Two formatted cards inserted by default." },

    { "log_file", "Log file", "Paths", "--log-file", "OPENPETE_LOG_FILE",
      "Path stderr is mirrored to for bug reports; empty = disabled "
      "(--no-log-file).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, logfile_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_LOG_FILE, else <state dir>/logs/openpete.log",
      .device_creation = 1,
      .help_arg = "PATH", .help_order = 640,
      .help_text = "mirror stderr to PATH instead of the default\n"
          "<state>/logs/openpete.log; env OPENPETE_LOG_FILE=..." },

    { "no_log_file", "No log file", "Paths", "--no-log-file", NULL,
      "Write no log file at all.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      nologfile_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .device_creation = 1,
      .help_arg = NULL, .help_order = 650,
      .help_text = "do not write a log file at all" },

    { "ingest", "Ingest disc image", "Paths", "--ingest", NULL,
      "Disc image copied into the game library next to the executable and "
      "booted; later runs list it in the launcher with no --disc. A bare "
      "disc-image path on the command line means the same.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none",
      .cli_only = 1,
      .help_arg = "PATH", .help_order = 20,
      .help_text = "copy a disc image (.cue/.bin) into the library\n"
          "next to the exe and boot it; later runs need no\n"
          "--disc at all (the original file can be deleted)" },

    { "game", "Game", "Paths", "--game", NULL,
      "The game this session runs (spyro-1, spyro-2, spyro-3), which "
      "selects the [game.<id>] scope of openpete.toml. Without --disc or "
      "--ingest it boots that game's most recently played library entry "
      "without the launcher's selector, or opens the drop screen when the "
      "library holds none. A disc of another game refuses the launch, as "
      "does a game this executable does not run.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "the disc's game, else the game of the entry picked in the launcher",
      .cli_only = 1,
      .help_arg = "ID", .help_order = 25,
      .help_text = "the game to run: spyro-1, spyro-2 or spyro-3 (default: the\n"
          "disc's game); without --disc, boots its most recently played\n"
          "library entry, or shows the drop screen when there is none;\n"
          "a disc of another game refuses the launch" },

    { "config", "Config files", "Paths", "--config", "OPENPETE_CONFIG",
      "The openpete.toml files this launch read. --config PATH (or the "
      "variable OPENPETE_CONFIG) reads that one file alone and saves to it; "
      "the value none reads no file. The flag wins over the variable.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, configfile_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "env OPENPETE_CONFIG, else openpete.toml next to the executable and openpete.toml in the per-user config directory, both read, the per-user file winning at the same scope",
      .cli_only = 1,
      .help_arg = "PATH", .help_order = 27,
      .help_text = "read only this config file, also the save target; none = read\n"
          "no file (default: env OPENPETE_CONFIG, else both openpete.toml)" },

    { "compile_mods", "Compile mods and exit", "Paths", "--compile-mods", NULL,
      "Compile every mod with a src/ directory in the mods directory against "
      "the shipped SDK, then exit; nonzero exit on a failed compile.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      compilemods_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .cli_only = 1,
      .help_arg = NULL, .help_order = 400,
      .help_text = "compile EVERY mod with src/ in the mods dir (see --mods-dir;\n"
          "ignores enabled.txt) against <exe>/sdk and exit; nonzero exit\n"
          "on any failed compile (the cmake build-time mods gate)" },

    { "card1", "Memory card 1", "Paths", "--card1", NULL,
      "The .mcr memory-card image in port A.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "card1.mcr in the memory-card directory",
      .help_arg = "PATH", .help_order = 410,
      .help_text = "use a specific .mcr memory-card image for port A (default: <card-dir>/card1.mcr)" },

    { "card2", "Memory card 2", "Paths", "--card2", NULL,
      "The .mcr memory-card image in port B.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "card2.mcr in the memory-card directory",
      .help_arg = "PATH", .help_order = 420,
      .help_text = "use a specific .mcr memory-card image for port B (default: <card-dir>/card2.mcr)" },

    { "no_card", "No memory cards", "Paths", "--no-card", NULL,
      "Present no memory card in either port, so the game cannot save.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      nocard_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 440,
      .help_text = "present no memory card in either port (saving disabled)" },

    { "no_card1", "No memory card 1", "Paths", "--no-card1", NULL,
      "Present no memory card in port A; port B keeps its card.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      nocard1_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 442,
      .help_text = "present no memory card in port A; port B keeps its card" },

    { "no_card2", "No memory card 2", "Paths", "--no-card2", NULL,
      "Present no memory card in port B; port A keeps its card.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      nocard2_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 445,
      .help_text = "present no memory card in port B; port A keeps its card" },

    { "card_unformatted", "Unformatted card", "Paths", "--card-unformatted", NULL,
      "Provision a blank unformatted card, so the game's own format screen "
      "runs.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      cardunf_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 450,
      .help_text = "provision a blank unformatted card so the in-game format screen runs" },

    { "save_mismatch_toast", "Save mismatch toast", "Paths", "--save-mismatch-toast", NULL,
      "An on-screen notification when a loaded save was written under a "
      "different set of mods. The log line is written either way.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      mismatch_toast_get, mismatch_toast_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off",
      .help_arg = "on|off", .help_order = 455,
      .help_text = "toast when a loaded save was written under different mods (default {def});\n"
          "the log line is written either way" },

    { "adopt_saves", "Adopt saves", "Paths", "--adopt-saves", NULL,
      "Re-tag every save on the memory card to the running modset at the "
      "card's first use.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      adopt_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .cli_only = 1,
      .help_arg = NULL, .help_order = 460,
      .help_text = "re-tag every memory-card save to the running modset at the card's first use" },

    ENVROW_LIVE("CC", "Paths", "",
        "C compiler for the first-boot module build when the package carries no "
        "bundled one, and for mods after OPENPETE_MOD_CC and the bundled one "
        "(unset = cc, except on Windows)."),
    ENVROW_LIVE("MOD_CC", "Paths", "",
        "C compiler for mod compiles; outranks the bundled compiler and OPENPETE_CC."),
    ENVROW_LIVE("PSXRECOMP", "Paths", "",
        "psxrecomp executable for the first-boot module build; outranks the one under recomp/tools/."),
    ENVROW_LIVE("MODULE_MARCH", "Paths", "",
        "-march of the first-boot module build, with -mtune=native; empty, none or off = the compiler's own target."),

    /* -------- Gameplay entry -------- */
    { "skip_to_level", "Skip to level", "Gameplay", "--skip-to-level", NULL,
      "Boot straight into a level id, fast-forwarding logos/title.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 99,
      skiplevel_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the game boots to its title",
      .help_arg = "L", .help_order = 540,
      .help_text = "load directly into level L: the game's numeric level id or its\n"
          "lower_snake_case name; no value or an unknown name prints this\n"
          "game's list. Windowed runs fast-forward the logos/title/loading\n"
          "(black window, audio muted) until the level is playable" },

    { "skip_to_cutscene", "Skip to cutscene", "Gameplay", "--skip-to-cutscene", NULL,
      "Play a named cutscene (Spyro 1: intro from the title; ending, "
      "ending_loot from Gnasty's levels; credits, credits_100 from a live "
      "level; Spyro 2: intro, intro_arrival, summer_forest, crush_intro, "
      "crush_defeated, autumn_plains, gulp_intro, gulp_defeated, "
      "winter_tundra, ripto_intro, ending from Dragon Shores, and "
      "LEVEL_intro / LEVEL_outro for the 17 levels with a level cutscene "
      "(idol_springs_intro, metropolis_outro, ...), from the homeworld "
      "portal / the completed level; Spyro 3: the "
      "19 story cutscenes by their loading captions, an_evil_plot_unfolds "
      "and its chain from the title, the rest from their trigger's level).",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 99,
      skipcutscene_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the game boots to its title",
      .help_arg = "C", .help_order = 550,
      .help_text = "play the named cutscene C. No value or an unknown name prints this\n"
          "game's list. Spyro 1: intro (the opening, from the title through\n"
          "New Game; not with --skip-to-level), ending (Gnasty defeated, from\n"
          "level 63), ending_loot (Gnasty's Loot, from level 64; the credits\n"
          "return to Artisans), credits / credits_100 (the short / full 100%\n"
          "roll, from --skip-to-level's target or Artisans; the roll returns\n"
          "to the level it started from). Spyro 2: intro, intro_arrival,\n"
          "summer_forest, crush_intro, crush_defeated, autumn_plains,\n"
          "gulp_intro, gulp_defeated, winter_tundra, ripto_intro, ending (the\n"
          "Dragon Shores theater's replay, from level 65; playback returns\n"
          "to Dragon Shores); LEVEL_intro (entered from the homeworld\n"
          "portal, first visit) and LEVEL_outro (the level completed and\n"
          "left) for LEVEL idol_springs, colossus, hurricos, aquaria_towers,\n"
          "sunny_beach, skelos_badlands, crystal_glacier, breeze_harbor,\n"
          "zephyr, scorch, shady_oasis, magma_cone, fracture_hills,\n"
          "mystic_marsh, cloud_temples, robotica_farms, metropolis. Spyro 3\n"
          "(the game's loading captions; each from its trigger's level, the\n"
          "chain continues as in the game):\n"
          "an_evil_plot_unfolds, a_powerful_villain_emerges,\n"
          "a_desperate_rescue_begins (from the title through New Game; not\n"
          "with --skip-to-level), bianca_strikes_back, the_second_warning,\n"
          "no_hard_feelings (10), spike_is_born, hunters_tussle,\n"
          "byrd_james_byrd (20), a_monster_to_end_all_monsters,\n"
          "an_apology_and_lunch, a_duplicitous_larcenous_ursine (30),\n"
          "the_escape (37), the_dancing_bear (40), deja_vu,\n"
          "a_familiar_face, billy_in_the_wall, one_less_noble_warrior (47),\n"
          "the_end (50)" },

    { "skip_to_area", "Skip to sub-area", "Gameplay", "--skip-to-area", NULL,
      "Boot into a level's sub-area, level:area (Spyro 3: the skate parks and "
      "the areas behind a level's portals), entered through the game's own "
      "sub-area transition once the level is live.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, skiparea_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the game boots to its title",
      .help_arg = "L:A", .help_order = 560,
      .help_text = "load level L (number or name) and enter its sub-area A through the\n"
          "game's own sub-area transition, as its area-0 portal does. A is\n"
          "an index from 1; no value or an invalid target prints each level's\n"
          "valid indices. Replaces --skip-to-level's level (Spyro 3)" },

    { "freecam", "Freecam", "Gameplay", NULL, "OPENPETE_FREECAM",
      "Fly the camera freely, detached from the player.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      freecam_get, freecam_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off",
      .key_action = "freecam" },

    { "state_slot", "Savestate", "Gameplay", NULL, NULL,
      "The slot the Save and Load buttons and the quick-save, quick-load "
      "and slot-cycle keys use.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_state_slot_names), 0, 0,
      state_slot_get, state_slot_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "Slot 0",
      .key_action = "savestate_cycle" },

    { "state_save", "Save state", "Gameplay", NULL, NULL,
      "Save the game to the selected slot.",
      KNOB_ACTION, KNOB_LIVE, NULL, 0, 0, 0,
      NULL, state_save_fire, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .action_label = "Save",
      .key_action = "savestate_save" },

    { "state_load", "Load state", "Gameplay", NULL, NULL,
      "Load the game from the selected slot; disabled while the slot is empty.",
      KNOB_ACTION, KNOB_LIVE, NULL, 0, 0, 0,
      state_load_ready, state_load_fire, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .action_label = "Load",
      .key_action = "savestate_load" },

    /* Both pose flags set their env spelling from the parser, so the env
     * getter shows either spelling. One-shot at boot. */
    { "freecam_pose", "Freecam pose", "Gameplay", "--freecam-pose", "OPENPETE_FREECAM_POSE",
      "Engage the debug free camera at this pose once the level is live "
      "(\"pos=(x, y, z) yaw=N pitch=N\", optionally \"roll=N\").",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, env_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the camera follows the player",
      .help_arg = "STR", .help_order = 580,
      .help_text = "engage the debug free camera at a pose once the level is\n"
          "live (same format as the freecam log: \"pos=(x, y, z) yaw=N\n"
          "pitch=N\", optionally followed by \"roll=N\", 4096 per turn,\n"
          "absent = 0). CLI twin of OPENPETE_FREECAM_POSE - env vars\n"
          "don't cross the WSL->Windows interop boundary." },

    { "player_pose", "Player pose", "Gameplay", "--player-pose", "OPENPETE_PLAYER_POSE",
      "Place the player at this position and facing once gameplay is live "
      "and under pad control (\"x,y,z[,yaw][@N]\"; @N waits until count N "
      "of the --frame-limit counter). Repeat the flag, or separate entries "
      "with ';', to place the player again at later counts, in order.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, env_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the player starts where the game places him",
      .help_arg = "STR", .help_order = 590,
      .help_text = "place the player once gameplay is live and under pad control:\n"
          "\"x,y,z[,yaw]\" or the exit pose line (\"pos=(x, y, z) yaw=N\",\n"
          "yaw 4096 per turn); the follow camera is re-seated behind him.\n"
          "A trailing \"@N\" waits for the first such tick at or after count\n"
          "N of the counter --frame-limit reads. Repeat the flag (or\n"
          "separate entries with ';') to place him again later: each\n"
          "entry applies in order, at its own @N.\n"
          "CLI twin of OPENPETE_PLAYER_POSE. Every run prints its end\n"
          "state as openpete-pose: after openpete-exit:." },

    { "camlock", "Camera lock (render)", "Gameplay", NULL, "OPENPETE_CAMLOCK",
      "Lock or fly the presented view while the game keeps running, so "
      "cutscenes play on underneath - the close-up inspection tool.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      camlock_get, camlock_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off",
      .key_action = "camlock" },

    /* A movie drives the pad, so it belongs with what shapes the session
     * rather than with the video-capture rows it used to sit among. */
    { "bk2", "bk2 replay", "Gameplay", "--bk2", NULL,
      "Replay a BizHawk movie into the pad (loaded at loop start).",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none: the pad is live",
      .help_arg = "PATH", .help_order = 600,
      .help_text = "replay BizHawk movie (Octoshock P1 pad: buttons + analog sticks)" },

    { "bk2_frame_offset", "bk2 frame offset", "Gameplay", "--bk2-frame-offset", NULL,
      "Map bk2 frame 0 to this value of the movie's row counter (yields on "
      "Spyro 1, VBLs on Spyro 2 and 3).",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      bk2offset_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "0: bk2 frame 0 plays at counter value 0",
      .help_arg = "N", .help_order = 610,
      .help_text = "map bk2 frame 0 to row-counter value N (default 0)" },

    { "bk2_stop_on_end", "Exit at bk2 end", "Gameplay", "--bk2-stop-on-end", NULL,
      "Exit after the last bk2 frame instead of holding its input.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      bk2stop_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 620,
      .help_text = "exit after last bk2 frame instead of holding last input" },

    { "bk2_record", "bk2 recording", "Gameplay", "--bk2-record", NULL,
      "BizHawk movie the live pad input is written to, with a .args sidecar "
      "of the typed launch flags.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none; the flag without a path records to "
                      "<bk2 dir>/<timestamp>.bk2",
      .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .help_arg = "[PATH]", .help_order = 630,
      .help_text = "write live pad input to a BizHawk .bk2 (combinable with --bk2 to extend a movie).\n"
          "With no PATH, records to <exe_dir>/bk2/<timestamp>.bk2. Also writes a\n"
          "<PATH>.args sidecar of the launch flags (minus --bk2*) for replay via --bk2." },

    /* The launch selection; the game module contributes one button row per
     * cheat through openpete_knob_add, in the Cheats section. */
    { "cheats", "Cheats at launch", "Gameplay", "--cheats", NULL,
      "Built-in cheat codes this run starts each session with. The rows below "
      "fire the same cheats on demand; this one only shows the launch "
      "selection.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, cheats_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .default_note = "none",
      .help_arg = "A,B,...", .help_order = 570,
      .help_text = "Spyro 1 only: apply the game's own built-in cheat codes without\n"
          "entering them on the pad: 99_lives, unlock_all_levels (an unknown\n"
          "name prints the list). Applied once a level is live, and again for\n"
          "each new session started from the title screen. Spyro 2 and 3\n"
          "refuse the flag" },

    /* -------- Recording --------
     * Every row here is hidden unless this run is recording; see
     * recording_visible. */
    { "record", "Record video", "Recording", "--record", NULL,
      "True-VFR gameplay capture to a video file.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .default_note = "none: no video is recorded",
      .help_arg = "PATH", .help_order = 10,
      .help_text = "capture true-VFR gameplay video (.mkv recommended; .mp4 ok); reads the present FBO (no letterbox bars)" },

    { "record_cfr", "Constant-rate capture", "Recording", "--record-cfr", NULL,
      "Capture at a fixed frame rate (an integer multiple of the 29.913 Hz "
      "tick rate) on an offline clock, instead of true VFR. 0 = VFR.",
      KNOB_FLOAT, KNOB_RESTART_ONLY, NULL, 0, 0.0f, 480.0f,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .default_note = "none: true variable-rate capture",
      .help_arg = "RATE", .help_order = 90,
      .help_text = "capture at a constant frame rate instead of true VFR. RATE must be an\n"
          "integer multiple of the NTSC tick rate: 29.913, 59.826, 119.652, ...\n"
          "Drives the host clock from the pacer, so the same span of GAME time is\n"
          "captured however slowly the machine renders. Implies --no-record-audio." },

    { "record_frames", "Capture frame budget", "Recording", "--record-frames", NULL,
      "Finish the recording and exit after this many encoded frames. "
      "0 = until the run ends.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 1000000,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "0",
      .help_arg = "N", .help_order = 100,
      .help_text = "finish the recording and exit after exactly N encoded frames (with\n"
          "--record-cfr, a window of exactly N/RATE seconds of game time)" },

    { "record_fps_overlay", "FPS overlay in recording", "Recording",
      "--record-fps-overlay", "OPENPETE_FPS_IN_RECORD",
      "Burn the FPS overlay into --record video output. Live: re-resolved "
      "on the next recorded frame.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      get_stored, recfps_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .default_note = "env OPENPETE_FPS_IN_RECORD, else 0",
      .help_arg = "0|1", .help_order = 80,
      .help_text = "burn the native FPS overlay into --record video (default 0 = clean\n"
          "capture; on|off also accepted)." },

    { "record_encoder", "Video encoder", "Recording", "--record-encoder", NULL,
      "libavcodec video encoder of the recording.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "libx264",
      .help_arg = "NAME", .help_order = 20,
      .help_text = "libavcodec encoder (default {def}; e.g. h264_nvenc, libopenh264)" },

    { "record_crf", "Encoder quality", "Recording", "--record-crf", NULL,
      "Constant-rate-factor quality of libx264 and libx265 (lower is better).",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 51,
      reccrf_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "23",
      .help_arg = "N", .help_order = 30,
      .help_text = "encoder quality for libx264/libx265 (default {def}; lower = better)" },

    { "record_gop", "Keyframe interval", "Recording", "--record-gop", NULL,
      "Encoded frames between keyframes.",
      KNOB_INT, KNOB_RESTART_ONLY, NULL, 0, 0, 1000,
      recgop_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "120",
      .help_arg = "N", .help_order = 40,
      .help_text = "keyframe interval in frames (default {def})" },

    { "record_pixfmt", "Pixel format", "Recording", "--record-pixfmt", NULL,
      "Encoder pixel format.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "yuv420p",
      .help_arg = "NAME", .help_order = 50,
      .help_text = "encoder pixel format (default {def})" },

    { "no_record_audio", "No audio track", "Recording", "--no-record-audio", NULL,
      "Record video only, without the mixed SPU audio.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      norecaudio_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "off", .flag_arg = KNOB_FLAG_ARG_NONE,
      .help_arg = NULL, .help_order = 60,
      .help_text = "record video only (default: also capture mixed SPU audio)" },

    { "record_audio_encoder", "Audio encoder", "Recording", "--record-audio-encoder", NULL,
      "libavcodec audio encoder of the recording.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .visible = recording_visible, .build_cond = "OPENPETE_HAS_LIBAV",
      .def_str = "aac",
      .help_arg = "NAME", .help_order = 70,
      .help_text = "libavcodec audio encoder (default {def})" },
};

#ifndef OPENPETE_HIDE_DEBUG_FLAGS
/* Rows compiled only into builds that carry the debug flags. rows_init marks
 * each copy debug_block, which is how the surface dump tells them apart. */
static const knob_desc_t g_builtin_debug[] = {
    /* -------- Debug (toggles / tunables) -------- */
    { "log_level", "Log level", "Debug", "--log-level", "OPENPETE_LOG",
      "Per-subsystem stderr log levels, e.g. cd:debug,*:info. Applying "
      "REPLACES all boot rules with the typed spec (empty = *:info).",
      KNOB_STRING, KNOB_LIVE, NULL, 0, 0, 0,
      NULL, NULL, loglevel_get_str, loglevel_set_str, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "SPEC", .help_order = 140,
      .help_text = "e.g. cd:debug,*:info; env OPENPETE_LOG=..." },

    { "psycross_native_raster", "PsyCross native raster", "Debug",
      "--psycross-native-raster", NULL,
      "1 renders the PsyCross reference on the console's own 512x240 display "
      "grid instead of a square-pixel 4:3 grid sized from the window. "
      "Reference renderer only.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      pcnative_get, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "N", .help_order = 110,
      .help_text = "1 = render the PsyCross reference on the console's own 512x240\n"
          "display grid (one texel per PSX pixel, authentic aliasing) instead\n"
          "of a square-pixel 4:3 grid sized from the window. The window blit\n"
          "scales it up. Default {def}. Reference renderer only." },

    { "hide_hud", "Hide HUD", "Debug", "--hide-hud", "OPENPETE_HIDE_HUD",
      "Draw the scene without the HUD: the flat 2D HUD layer (panels, "
      "counters, text) and the HUD3D layer (the spinning HUD models) are "
      "not drawn. Letterbox bars, the fade and the game state are "
      "unchanged. Native renderer only.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      hide_hud_get, hide_hud_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0",
      .help_arg = "0|1", .help_order = 112,
      .help_text = "1 = draw the scene without the HUD: the flat 2D HUD layer and\n"
          "the HUD3D models; letterbox bars and the fade stay.\n"
          "Default: env OPENPETE_HIDE_HUD, else {def}. Native renderer only." },

    { "hide_classes", "Hide moby classes", "Debug", "--hide-classes", "OPENPETE_HIDE_CLASSES",
      "Comma list of moby class ids (decimal, or hex with 0x) whose models "
      "and drop shadows are not drawn; at most 32. The game plays on as "
      "before. Native renderer only.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, optstr_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "LIST", .help_order = 114,
      .help_text = "comma list of moby class ids (0x prefix for hex, at most 32)\n"
          "whose models and drop shadows are not drawn.\n"
          "Default: env OPENPETE_HIDE_CLASSES, else none. Native renderer only." },

    ENVROW_LATCHED("CARD_GUARD", "Debug", "0",
        "1 = refuse to start, exit 64 with an error naming the port and "
        "path, when an inserted card's path (from --card1/--card2, "
        "--card-dir, OPENPETE_CARD_DIR or the default) resolves inside the "
        "per-user card directory or the cards directory beside the "
        "executable. For automated runs, which must not touch a "
        "player's cards. Read at launch."),
    ENVROW_LATCHED("DISC_BUILD_REFUSE", "Debug", "0",
        "1 = refuse to start, exit 3 with the disc's message, when the "
        "disc's boot executable is not a supported build (another "
        "revision, region or a modified image). 0 warns and boots. For "
        "automated runs, which must run only the supported discs. Read at "
        "launch."),
    ENVROW_LATCHED("SANDBOX", "Debug", "",
        "Directory every file the run writes goes under: config, state and "
        "data resolve to <root>/config, <root>/state and <root>/data on "
        "every platform, the portable layout is off, and the overlay ini "
        "moves to the config directory. The run refuses to start, exit 66, "
        "when the root, a resolved leaf, or an output path a variable or "
        "flag names lies in a per-user openpete directory or beside the "
        "executable. For automated runs, which must not touch a player's "
        "files. Read at launch."),
    ENVROW_LATCHED("RUN_RECOMP", "Debug", "",
        "Guest address (hex) the game fiber dispatches instead of the EXE "
        "header's entry point."),
    ENVROW_LATCHED_NOTE("SURFACE_DUMP", "Debug", "none: the game runs",
        "Path the binary writes its flags, registry rows, menu rows, key "
        "actions and vocabularies to as JSON, with their declared defaults "
        "and the defaults selfcheck, before exiting; read at the start of "
        "main, before option parsing."),

    { "log_format", "Log format", "Debug", "--log-format", NULL,
      "stderr log shape: plain text vs one JSON object per line. Takes "
      "effect on the next line.",
      KNOB_ENUM, KNOB_LIVE, ENUM_ROW(k_logfmt_names), 0, 0,
      logfmt_get, logfmt_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .device_creation = 2,
      .help_arg = "json|plain", .help_order = 130,
      .help_text = "structured stderr (default plain)" },

    ENVROW_LATCHED("PSXTRACE_CLOCK_REPLAY", "Debug", "0",
        "Oracle clock replay: with --psxtrace-verify and the golden's .vblpos "
        "and .spuq sidecars, each of the reference's in-work VBLANKs is "
        "delivered at the function entry it recorded and the SPU envelope is "
        "charged to the reference's exact clock at every wait, poll, query "
        "and libspu call. "
        "tests/harness/check_psx_oracle.sh sets it under OPENPETE_ORACLE_CLOCK=1."),
    ENVROW_LATCHED("HEADLESS_NATIVE", "Debug", "0",
        "1 = under --headless with the vk renderer, bring the native renderer "
        "up surfaceless with no screenshot requested, so the game-side scene "
        "feed and the per-VBL device services run every tick as under a "
        "window; nothing is composed unless a capture asks. The exit line "
        "`native-feed: captures= fed= tris=` counts the feed. "
        "tests/harness/check_psx_oracle.sh and check_determinism_matrix.sh "
        "set it."),
    ENVROW_LATCHED("D1", "Debug", "0",
        "1 = every live moby of the level updates every tick at any distance "
        "(Spyro 2 and 3): the moby update-list builder is replaced by a "
        "build without its distance test, and an undrawn gem spins at any "
        "range. For the credits fly-through video."),
    ENVROW_LATCHED("VIDEO_NOSPARX", "Debug", "0",
        "1 = with OPENPETE_D1, the companion's class stays off the moby "
        "update list, so Sparx and his sparkles and glow are absent. Without "
        "OPENPETE_D1 it changes nothing and a warning says so."),
    ENVROW_LATCHED("VIDEO_OPEN_PANIM", "Debug", "",
        "Comma list of keyframe animation ids (Spyro 2 and 3): each "
        "animated sector (door) with that id that rests at keyframe 0 is "
        "started once, as the level opens it, and rests at its next stop. "
        "For the credits fly-through video."),
    ENVROW_LATCHED("VIDEO_HOME_PANIM", "Debug", "",
        "Comma list of keyframe animation ids (Spyro 2 and 3): each "
        "animated sector with that id is sent back to keyframe 0 and held "
        "there when the level restarts it. For the credits fly-through "
        "video."),
    ENVROW_LATCHED("VIDEO_ZOE_NEAR", "Debug", "0",
        "1 = the checkpoint fairy reads her distance to the player as near "
        "at any range (Spyro 3), so she is shown idle everywhere and never "
        "triggers the checkpoint. For the credits fly-through video."),
    ENVROW_LATCHED("CRASH_TEST", "Debug", "",
        "Fault injection for the crash handler: segv (a null store) or "
        "overflow (unbounded recursion) on the game fiber before the game's "
        "entry runs; pipe (a null store) inside the first pipeline create, "
        "after its breadcrumb is set and before the driver call. Read at "
        "boot; pipe at native renderer device creation."),
    ENVROW_LIVE("WRITE_FAIL", "Debug", "",
        "Fault injection for persisted files: <substring>[:<bytes>] makes "
        "every atomic save (memory card, save state, config, mod config, "
        "shader cache) whose path contains <substring> fail after <bytes> "
        "bytes, with the previous file kept. Read at each save."),
    ENVROW_DEVICE("RHI_DEPTH_D24", "Debug", "0",
        "1 = take the D24_UNORM_S8 depth format even where D32_SFLOAT_S8 is "
        "available, running the fallback's depth convention. Read at device "
        "creation."),
    ENVROW_DEVICE("RHI_COMPUTE", "Debug", "1",
        "0 = report compute passes unavailable on the device, as a device "
        "without them does, so every compute path takes its CPU fallback. "
        "The device is created the same either way. Read at device "
        "creation."),
    ENVROW_LATCHED_NOTE("PRECOMPILE_FORCE", "Debug",
        "unset: runs in a windowed run on a hardware device",
        "1 runs the pipeline precompile at bring-up in any run mode (headless, "
        "deterministic capture, a software device); 0 skips it in any mode. "
        "Unset, it runs in a windowed run on a hardware device and is skipped "
        "in headless and deterministic runs and on a software device."),
    ENVROW_LATCHED("EAGER", "Debug", "1",
      "Eager production (default on): the span of presents is extracted "
      "AHEAD of each tick and presented from a dedicated thread's scene "
      "ring, so the tick's execution leaves no hole in the display. 0 = "
      "present from the sim thread (the first A/B for any pacing "
      "regression). Windowed native-vk only; a --screenshot-on-frame "
      "target disengages it unless OPENPETE_EAGER_SHOT=1."),
    ENVROW_LATCHED("LOOP_DELIVERY", "Debug", "1",
      "Deliver a VBL inside a guest polling loop once a VBL of the loop's "
      "instructions has run without one, so a driver spinning on a word its "
      "own VBL handler writes can finish. Set 0 to stop the loop deliveries "
      "and keep only their count (the oracle's control run)."),
    ENVROW_LATCHED("HANG_STALL_SECONDS", "Debug", "30",
      "Wall seconds the game may run without a VBL before the hang "
      "watchdog writes hang-trace.bin and hang-ram.bin and exits with code "
      "2. Time the game is paused or parked in the host loop does not "
      "count. 0 measures without tripping; OPENPETE_HANG_THRESHOLD=0 "
      "disarms every watchdog trip."),
    ENVROW_LATCHED("BUGREPORT_THUMBS", "Debug", "1",
      "Keep the bug report's thumbnail ring: four low-resolution copies "
      "of the presented image per second of present clock, the newest 20 "
      "(5 s) held in memory and written to a bug report as thumbs/. Set 0 "
      "to drop the ring and its per-present downscale."),
    ENVROW_LATCHED("BUGREPORT_SUBTICK", "Debug", "1",
      "Keep the bug report's sub-tick present ring: per re-extracted "
      "present its kind, alpha and tick stamps, and per drawn object record "
      "its pairing state and drawn position, the newest 1024 presents held "
      "in memory and written to a bug report as subtick.bin. Set 0 to drop "
      "the ring."),
    ENVROW_DEVICE("BUGREPORT_GPU", "Debug", "0",
      "Arm the GPU timestamp zones and keep one GPU_FRAME record per "
      "rendered frame in the pace ring (the span from the first zone's start "
      "to the last zone's end, and the sum of the top-level zones), written "
      "to a bug report in pace.oppl and folded into pace-coarse.bin. Off by "
      "default: the timestamp writes cost GPU time not yet measured on a "
      "weak GPU. Set 1 to record. Read at device creation."),
    ENVROW_LATCHED("THREAD_SPLIT", "Debug", "1",
      "Run the sim on a worker thread while the main thread owns the window "
      "and the event pump, so dragging or resizing the window cannot stall "
      "the game. On by default; set 0 to put the loop back on the main "
      "thread. Windowed native-vk only."),
    ENVROW_LATCHED("THREAD_ROLE_FATAL", "Debug", "0",
      "1 aborts the process at the first failed thread-role check (a role "
      "assert or check whose thread lacks the role, or a write to a sealed "
      "group after its seal); otherwise a failure is counted and logged once "
      "per site. Read at the first failure. The gate harnesses set 1."),
    ENVROW_LATCHED_NOTE("THREAD_ROLE_REPORT", "Debug", "none: no report",
      "Path of a tab-separated report written at exit: one row per "
      "thread-role check site the run reached, with its evaluation and "
      "failure counts. scripts/fences/thread_roles.py join <path> adds "
      "the sites the run did not reach as zero rows."),
    ENVROW_LATCHED_NOTE("LOOP_TRACE", "Debug", "none: no trace",
      "Path of the host loop's decision trace: one 64-byte record per loop "
      "iteration (sim due, unlock, override claim, rewind, yield, tick, the "
      "present made, the runahead outcome, the exit reason, the VBL pace "
      "flags and the pacer deadlines), a header per process and the "
      "teardown steps in the order they ran; layout in "
      "openpete_host_trace.h. Two builds' traces of one run compare "
      "decision for decision. Read at the loop's start; a handoff successor "
      "appends."),
    ENVROW_LATCHED_NOTE("EAGER_HOLD_S", "Debug", "none: no hold",
      "Seconds eager production stays off from the first canonical present "
      "at which every other engage term holds, so inline sub-tick presents "
      "run and warm the shed predictor first."),
    ENVROW_LATCHED("GLINT", "Debug", "2",
      "What the gem-glint feeder emits: 0 nothing, 1 the near four-point "
      "stars alone, 2 the near stars and the far crosses. The guest draw "
      "runs at every setting, so 0 and 1 price the feed's painted pixels "
      "against 2 from one binary."),
    ENVROW_DEVICE("VK_VALIDATION", "Debug", "0",
        "1 = enable the Khronos Vulkan validation layer and log its warnings "
        "and errors as 'validation:' lines. Needs the Vulkan SDK (the layer "
        "is not part of a driver install); costs CPU time on every Vulkan "
        "call. Read at instance creation."),
    ENVROW_LATCHED("VK_FAULT", "Debug", "",
        "submit:N = the N-th fenced Vulkan queue submit (a frame or present "
        "submit) returns device-lost without submitting, which drives the "
        "device-lost shutdown on a working device. Read at the first "
        "submit."),
    ENVROW_DEVICE("GPU_TIMERS", "Debug", "0",
        "Per-pass GPU timestamp zones (query pools created at device init)."),
    ENVROW_LIVE("PHASE_TIMERS", "Debug", "0",
        "Once-per-second per-phase CPU us report (re-resolved per VBL)."),
    ENVROW_LIVE("MOD_TIMERS", "Debug", "0",
        "Per-mod tick/present ms report (re-read per report window)."),
    ENVROW_LIVE("GPU_TIMERS_VERBOSE", "Debug", "0",
        "GPU-timer stderr line includes nested sub-zones, not just depth-0 "
        "(re-read per report; needs GPU_TIMERS)."),
    ENVROW_LIVE("BACKDROP_DEFER", "Debug", "1",
        "Defer cyclorama/backdrop fills until after WORLD-opaque so early-Z "
        "rejects covered pixels (default 1; 0 = PSX draw-first order)."),
    ENVROW_LATCHED("MATERIAL_STATS", "Debug", "0",
        "Print new peaks of interned material slots."),
    ENVROW_LATCHED("TEXPACK_STATS", "Debug", "0",
        "HD-texpack sub-run disposition counters."),
    ENVROW_LIVE("MOBY_OCCLUSION", "Debug", "1",
        "Moby occlusion culling on/off (default 1; re-resolved per VBL)."),
    ENVROW_LATCHED("MOBY_OCCL_SLACK", "Debug", "1500",
        "Occlusion-grid depth slack before a rect counts hidden (default 1500)."),
    ENVROW_LATCHED("OCCL_CAM_MOVE", "Debug", "24",
        "Occlusion-grid camera bound: world units the querying camera may differ "
        "from the grid's before the depth verdict is refused (default 24)."),
    ENVROW_LATCHED("OCCL_CAM_TURN", "Debug", "0.35",
        "Occlusion-grid camera bound: degrees the querying camera may be rotated "
        "from the grid's before the depth verdict is refused (default 0.35)."),
    ENVROW_LATCHED("MOBY_RANK", "Debug", "1",
        "Same-moby painter select: between faces of one moby that intersect in 3D, "
        "the game's own paint order (its OT key, evaluated natively) overrides "
        "per-pixel depth (default 1; 0 = true depth only)."),
    ENVROW_LATCHED("MOBY_SHELL", "Debug", "1",
        "Front-shell depth: a moby record drawn into one ordering-table bucket with "
        "blended and opaque faces paints its opaque faces over terrain lying between "
        "them and its blended front faces (default 1; 0 = true depth only)."),
    ENVROW_LATCHED("TERRAIN_TJSPLIT", "Debug", "1",
        "Marked-edge terrain split: every HP face edge that borders an HQ-eligible "
        "face carries the subdivide's edge nodes on both of its faces, in the CPU "
        "walk and the retained cache, so the edge has no T-junction pinholes, and LP "
        "vertices within one unit of each other draw at one position "
        "(default 1; 0 = faces draw unsplit and LP vertices as stored)."),
    ENVROW_LATCHED("MOBY_SHADOW_DECAL", "Debug", "1",
        "Depth-aware moby-shadow decal vs legacy subtract pipe (default 1)."),
    ENVROW_LATCHED("SHADOW_PLAYER", "Debug", "1",
        "Player ground-shadow leg feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("SHADOW_INTERP", "Debug", "1",
        "Moby drop-shadow own-motion offsets under an interpolated camera (default 1); "
        "0 leaves every shadow at the newest tick's world pose."),
    ENVROW_LATCHED("PORTAL3_INTERP", "Debug", "1",
        "Spyro 2 and Spyro 3 portal-preview sub-tick interpolation (default 1); "
        "0 replays every preview at the state its tick captured."),
    ENVROW_LATCHED("PORTAL3_SKY", "Debug", "1",
        "Spyro 2 and Spyro 3 portal previews: the destination level's sky, read from its "
        "WAD entry, beneath each picture (default 1); 0 leaves the authored backdrop."),
    ENVROW_LATCHED("PARTICLES", "Debug", "1",
        "Particle feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("TRACERS", "Debug", "1",
        "Tracer ribbon and mesh feed enable (default 1); the guest draws run either way."),
    ENVROW_LATCHED("TRACERS_INTERP", "Debug", "1",
        "Spyro 2 and Spyro 3 tracer sub-tick rebase (default 1); "
        "0 replays every ribbon and mesh at the screen positions its tick drew."),
    ENVROW_LATCHED("GLOW", "Debug", "1",
        "Glow-ring feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("ZAP", "Debug", "1",
        "Zoe's zap feed enable (default 1); the guest draw runs either way."),
    ENVROW_LATCHED("SHADOW_BAND", "Debug", "256",
        "Fallback view-Z band for shadow decal mode 0 (default 256)."),
    ENVROW_LATCHED("SHADOW_HW_BIAS", "Debug", "-64,-2,-0.002",
        "Hardware depth bias c,s,clamp on subtractive shadow pipes."),
    ENVROW_LIVE("SHADOW_DECAL_MODE", "Debug", "",
        "Force shadow-decal shader mode (calibration override)."),
    ENVROW_LATCHED("PORTAL_MASK_BIAS", "Debug", "0,0,0",
        "Portal-mask camera-ward depth pull (3 floats)."),
    ENVROW_LATCHED("HUD6_ZPIPE", "Debug", "0",
        "A/B: HUD glyph bucket via opaque depth-tested pipe."),
    ENVROW_LATCHED("HUD6_NOSORT", "Debug", "0",
        "A/B: keep bucket-6 submission order (skip z sort)."),
    ENVROW_LATCHED("NATIVE_SCENE_DELAY", "Debug", "0",
        "Compose from a past completed frame instead of the current one."),
    ENVROW_LATCHED("TAG_SPLIT", "Debug", "0",
        "Force batch split by mod scene-tag."),
    ENVROW_LATCHED("TAG_TINT", "Debug", "0",
        "Visual tint by scene tag."),
    ENVROW_LIVE("SHADOW_WALL_HEIGHT", "Debug", "400",
        "Decal wall-occlusion height threshold (default 400)."),
    ENVROW_LIVE("SHADOW_STEEP_NY", "Debug", "0.5",
        "Up-normal below which a surface counts as wall (default 0.5)."),
    ENVROW_LIVE("SHADOW_PLAT_EPS", "Debug", "150",
        "Raised-occluder clause epsilon (default 150; 1e5 disables)."),
    ENVROW_LIVE("SHADOW_PLAT_SLACK", "Debug", "48",
        "Platform-clause slack (default 48)."),
    ENVROW_LIVE("SHADOW_FOOTPRINT", "Debug", "1",
        "Player-shadow footprint term: the terrain under the ring below the caster "
        "(default 1); 0 leaves the fan's own surface under the depth rule."),
    ENVROW_LATCHED("SHADOW_BIAS_SPYRO", "Debug", "1.0",
        "Spyro-shadow depth-bias multiplier."),
    ENVROW_LATCHED("SHADOW_BIAS_MOBY", "Debug", "1.0",
        "Moby-shadow depth-bias multiplier."),
    ENVROW_LATCHED("PORTAL_SKY_EPS", "Debug", "24",
        "Portal-sky forward depth margin (default 24; two independent latches)."),
    ENVROW_LATCHED("PORTAL_SKY_DEPTH", "Debug", "plane",
        "Portal sky depth model: ot|plane|ring."),
    ENVROW_LIVE("PORTAL_SKY_OTPUSH", "Debug", "1",
        "Far-portal sky rides the .s +64-slot OT bump as an 8192-world depth push (0 = un-pushed plane)."),
    ENVROW_LATCHED("NOLOD_PORTAL_DOME", "Debug", "1",
        "Kill switch for extended-LOD always-render dome, every game (default 1)."),
    ENVROW_LATCHED("PORTAL_GUARD", "Debug", "0",
        "Experiment: re-feed .s near-band tris as portal guard."),
    ENVROW_LATCHED("PORTAL_APERTURE", "Debug", "1",
        "Stencil the portal sky to the opening polygon (0 = old sky-footprint mask)."),
    ENVROW_LATCHED("PORTAL_APERTURE_MARGIN", "Debug", "40",
        "Outward margin on the portal aperture, in WORLD units (default 40; negative erodes)."),
    ENVROW_LATCHED("PORTAL_RIM_CULL", "Debug", "1",
        "Cull terrain lying in a portal's opening plane (the inner rim). 0 = keep."),
    ENVROW_LATCHED("PORTAL_TRANSP_COVER", "Debug", "1",
        "Drop blended terrain (waterfalls, both tiers) the portal preview covers "
        "(0 = let it blend through the preview)."),
    ENVROW_LATCHED("PORTAL_PLANE_MARGIN", "Debug", "128",
        "Plane-cover near margin M (world units): inside an opening the preview covers "
        "everything at or behind plane - M. Portal-name text floats ~192-256 in front; "
        "frame erosion begins near 512."),
    ENVROW_LATCHED("PORTAL_RIM_NEAR", "Debug", "128",
        "Rim predicate: how far the NEAREST vertex may sit from the opening plane, in "
        "world units - the rim must touch the plane, an arch's front wall must not."),
    ENVROW_LATCHED("PORTAL_RIM_EPS", "Debug", "384",
        "Rim predicate: how far the FARTHEST vertex may stray from the opening plane, "
        "in world units."),
    ENVROW_LATCHED("PORTAL_RIM_REACH", "Debug", "128",
        "Rim predicate: how far OUTSIDE the opening polygon a face may reach and still "
        "count as rim, measured in the plane, in world units."),
    ENVROW_LATCHED("PORTAL_RIM_ANGLE", "Debug", "30",
        "Rim predicate: max angle between a rim face and the opening plane, in degrees."),
    ENVROW_LATCHED("PORTAL_RIM_BROAD", "Debug", "1",
        "Portal-rim sector broadphase: skip rim classification for sectors provably "
        "clear of every opening (1 on, 0 off, 2 = classify live + audit the broadphase)."),
    ENVROW_LATCHED("PORTAL_NUDGE", "Debug", "",
        "Per-portal opening correction \"id:dh,dv[,sh[,sv]];...\" in world units."),
    ENVROW_DEVICE("TERRAIN_SECDELTA", "Debug", "0",
        "Terrain-cache vertex stages: 1 = the per-sector rounding control of the "
        "shared-vertex readback and the hole census, 0 = the shipped stages."),
    ENVROW_LIVE("TERRAIN_CULL", "Debug", "3",
        "bit0 HP NCLIP cull, bit1 LP cull (default 3 = both; each mirrors its walk's exact rule)."),
    ENVROW_LIVE("TERRAIN_FLAT", "Debug", "0",
        "Draw terrain untextured."),
    ENVROW_LATCHED("TERRAIN_HQSCALE", "Debug", "4.0",
        "Near/HQ classification scale (default 4.0)."),
    ENVROW_LATCHED("TERRAIN_NEARZ", "Debug", "1.0",
        "Near clip plane in world/16 view-Z (default 1.0)."),
    ENVROW_LATCHED("TERRAIN_NEARHQ", "Debug", "1",
        "Near/HQ subdivide pass enable (default 1)."),
    ENVROW_LIVE("TERRAIN_TEXTBL", "Debug", "0",
        "0 auto; 1 force LQ; 2 force HQ tiledefs."),
    ENVROW_LATCHED("TERRAIN_HIDETAIL", "Debug", "-1",
        "Force hi-detail flag into sector decode cache."),
    ENVROW_LIVE("TERRAIN_OCCL", "Debug", "1",
        "Sector occlusion cull via occlusion grid (default 1)."),
    ENVROW_LIVE("TERRAIN_HOLEFILL", "Debug", "1",
        "Stencil-gated LP hole fill at LOD boundary (default 1)."),
    ENVROW_LATCHED("TERRAIN_LPKEEP", "Debug", "0",
        "Keep ALL near-LP faces (probe)."),
    ENVROW_LATCHED("TERRAIN_FILL_FOGSLACK", "Debug", "0",
        "ir0 slack admitting not-fully-fogged faces to fill."),
    ENVROW_LATCHED("TERRAIN_FILL_THINPX", "Debug", "16",
        "Screen-thin bbox cap for slack-admitted fill faces (default 16)."),
    ENVROW_LATCHED("TERRAIN_FILL_DILATE", "Debug", "0",
        "Fill-tri screen dilation px."),
    ENVROW_LATCHED("TERRAIN_FILL_UP", "Debug", "0.7",
        "Fill upward growth px (default 0.7)."),
    ENVROW_LATCHED("TERRAIN_FILL_QUANT", "Debug", "1",
        "Floor-quantize fill verts (default 1)."),
    ENVROW_LIVE("TERRAIN_LP_ADMIT", "Debug", "1",
        "Spyro 1 authored per-face LP admission (.s exact; 0 = legacy); Spyro 2/3 ignore it."),
    ENVROW_LIVE("TERRAIN_LP_FARCULL", "Debug", "1",
        "LP far cull at m_CullingDistance (.s exact; original LOD only)."),
    ENVROW_LIVE("TERRAIN_HP_DROP", "Debug", "1",
        "HP LOD drop at m_LodDistance (.s exact; 0 = fog-corner legacy)."),
    ENVROW_LIVE("TERRAIN_OT_BIAS", "Debug", "1",
        "Authored .s OT-slot depth offsets, both tiers (0 = raw depth)."),
    ENVROW_LIVE("TERRAIN_GAPFILL", "Debug", "1",
        "Frozen fill-tag LP faces (default 1)."),
    ENVROW_LATCHED("TERRAIN_ARTFIX", "Debug", "1",
        "Recolour known unpainted level-data faces (0 = authentic)."),
    ENVROW_LATCHED("TERRAIN_LOD_FAITHFUL", "Debug", "1",
        "Faithful .s sector binning (original LOD; 0 = admission-only)."),
    ENVROW_LATCHED("TERRAIN_FOGSCALE", "Debug", "1.0",
        "Debug force-override of the fog curve (wins over --fog-scale)."),
    ENVROW_LATCHED("TERRAIN_FOGDPCS", "Debug", "1",
        "Exact per-corner DPCS vertex fog decode (default 1)."),
    ENVROW_LATCHED("TERRAIN_CLUTFOG", "Debug", "1",
        "CLUT-row texel fog ladder (default 1)."),
    ENVROW_LIVE("TERRAIN_LOD_HANDOFF", "Debug", "1",
        "Drop HP beyond fog saturation, substitute LP (default 1)."),
    ENVROW_LATCHED("TERRAIN_PORTAL_CONFORM", "Debug", "1",
        "Terrain-vs-portal-sky depth conformance clamp (default 1)."),
    ENVROW_LATCHED("TERRAIN_NEARVAR", "Debug", "1",
        "Retain near-plane sectors on the GPU clip-fan path (0 = CPU walk)."),
    ENVROW_LATCHED("TERRAIN_ANIMNEAR", "Debug", "1",
        "Retain near-plane poly-anim sectors on the GPU clip-fan path (default 1 = GPU, 0 = CPU walk)."),
    ENVROW_LATCHED("TERRAIN_CANIMNEAR", "Debug", "1",
        "Retain near-plane env-anim colour sectors on the GPU clip-fan path (default 1 = GPU, 0 = CPU walk)."),
    ENVROW_LATCHED("MOBY_SNAP_CARRY", "Debug", "1",
        "Carry each moby's snap planes from the tick's resolve onto sub-tick presents under the moved camera (default 1; 0 = resolve again on every present)."),
    ENVROW_LATCHED("OCCL_CONFIRM", "Debug", "1",
        "Confirm a moby occlusion admission that was not decided on bounds decoded this VBL against the bounds of the decode that follows (default 1; 0 = keep the admission)."),
    ENVROW_LATCHED("OCCL_HELD_CONFIRM", "Debug", "1",
        "Confirm a held provisional moby occlusion admission again on each sub-tick present's own decode (default 1; 0 = take the held admission; log = also print [occl-held] per present)."),
    ENVROW_LATCHED("MOBY_RANK_ABSENT_SKIP", "Debug", "1",
        "Keep the carried paint rank of a sub-tick moby run whose faces absent from the carry table contest no face (default 1; 0 = re-rank every such run; check = re-rank it and count contested-order mismatches)."),
    ENVROW_LATCHED("SNAP_MIN_PX", "Debug", "16",
        "A moby drawn under this height in pixels at 1440 lines takes no ground snap; one left out at the previous tick stays out until 1.25 times it (default 16; 0 = snap every moby)."),
    ENVROW_LATCHED("SNAP_HP_ONLY", "Debug", "0",
        "1 limits the moby snap's terrain capture to sectors inside the game's HP envelope at the view (default 0 = every visible sector)."),
    ENVROW_LATCHED("SNAP_LISTED", "Debug", "1",
        "Moby snap capture walks a cache-owned sector over only the drawn faces whose corner box meets a moby's search box (default 1; 0 = walk every face)."),
    ENVROW_LATCHED("SNAP_REX_MERGE", "Debug", "1",
        "One moby snap resolve serves a sub-tick present after its walk passes (default 1; 0 = resolve per pass)."),
    ENVROW_LATCHED("SNAP_CORNER_EQ", "Debug", "1",
        "Skip a carried moby's snap corner recompute when its queued corners and view equal the stored ones bit for bit (default 1; 0 = always compare)."),
    ENVROW_LATCHED("SNAP_QUEUE_LEAN", "Debug", "1",
        "Moby snap bounds computed at the chunk resolve and corners gathered straight into the queue (default 1; 0 = bounds at the queue, corners copied)."),
    ENVROW_LATCHED("MOBY_POSE_CACHE", "Debug", "1",
        "Sub-tick moby pose cache: 1 reads back a pose filed under equal key words and the same guest generation, 0 never files or reads, 2 decodes every vertex and compares it with the filed copy (default 1)."),
    ENVROW_LATCHED("MOBY_SNAP_FANOUT", "Debug", "1",
        "The moby snap contact pass may fan out to the worker pool (default 1; 0 = serial)."),
    ENVROW_LATCHED("MOBY_SNAP_FANOUT_MIN", "Debug", "6000",
        "Snap contact pass fan-out bound in work units (triangles plus candidates per moby); a smaller chunk runs serially (default 6000)."),
    ENVROW_LATCHED("MOBY_REC_FANOUT", "Debug", "1",
        "The sub-tick moby record pass may fan its decodes out to the worker pool (default 1; 0 = serial)."),
    ENVROW_LATCHED("MOBY_REC_FANOUT_MIN", "Debug", "8000",
        "Record pass decode fan-out bound in vertices; a smaller pass runs serially (default 8000)."),
    ENVROW_LATCHED("MOBY_RANK_FANOUT", "Debug", "1",
        "The moby paint-rank census and rank may fan out to the worker pool (default 1; 0 = serial)."),
    ENVROW_LATCHED("MOBY_RANK_FANOUT_MIN", "Debug", "3000",
        "Paint-rank fan-out bound in faces; fewer run on the calling thread (default 3000)."),
    ENVROW_LATCHED("MOBY_PUSH_FANOUT", "Debug", "1",
        "The moby face emit may fan out to the worker pool (default 1; 0 = serial)."),
    ENVROW_LATCHED("MOBY_PUSH_FANOUT_MIN", "Debug", "4800",
        "Face emit fan-out bound in faces; a smaller emit runs serially (default 4800)."),
    ENVROW_LATCHED("MOBY_WALK_SERIAL", "Debug", "0",
        "1 forces the animated moby walk and the simple-model fan-out serial, leaving the worker pool up for every other caller (default 0)."),
    ENVROW_LATCHED("MOBY_FANOUT_MIN", "Debug", "400",
        "Animated moby walk fan-out bound in faces; a smaller walk runs serially (default 400)."),
    ENVROW_LATCHED("MOBY_SIMPLE_FANOUT_MIN", "Debug", "3000",
        "Simple-moby decode fan-out bound in faces; a smaller decode runs serially (default 3000)."),
    ENVROW_LATCHED("MOBY_SIM_PROJ_DEFER", "Debug", "1",
        "The simple-moby decode projects each record's vertices in the record's own job, on the walk workers when it fans out (default 1; 0 = in the serial prefix)."),
    ENVROW_LATCHED("RING_GROW_FULL_COPY", "Debug", "0",
        "A growing ring vertex buffer copies every slot whole into the new storage (default 0 = each slot up to the furthest byte written to it)."),
    ENVROW_LATCHED("EAGER_REWARM_EDGE", "Debug", "0",
        "A level edge the terrain walk counts opens the re-warm window while the world stays live, as at a portal entry (default 0 = only the world going live opens it)."),
    ENVROW_LATCHED("EAGER_REWARM", "Debug", "1",
        "At the world going live, the first 32 scenes and renders cheaper than the planner's cost averages replace them (default 1; 0 = an eighth per sample)."),
    ENVROW_LATCHED("TSC_VHASH_MEMO", "Debug", "1",
        "A sub-tick terrain build skips a cached sector's vertex hash that an earlier sub-tick build of the same guest generation found current (default 1; 0 = hash every build)."),
    ENVROW_LATCHED("UI_UNDER_PLAYER", "Debug", "1",
        "Draw 2D the game links behind the player's ordering-table slot before the player (default 1; 0 = all 2D over the player)."),
    ENVROW_LATCHED("TC_RETIRE_SPREAD", "Debug", "0",
        "At a level edge the outgoing level's terrain-cache buffers are destroyed one per present (default 0 = all at the first present past the drain floors)."),
    ENVROW_LATCHED("TC_EDGE_RECYCLE", "Debug", "0",
        "The outgoing level's terrain-cache buffers stay allocated, one per arena, and serve the next level edge's replacement when large enough (default 0 = freed, and the replacement allocated at the edge)."),
    ENVROW_LATCHED("TC_BAKE_BUDGET_US", "Debug", "1000",
        "Terrain-cache first-touch bake budget per tick in us of reference-host bake time, charged in work units counted from each sector (800 per ms), so every host defers the same sectors; the rest draw through the CPU walk (0 = one sector per tick, off = no budget)."),
    ENVROW_LIVE("TERRAIN_VANIM", "Debug", "1",
        "Sub-tick lerp of moving-terrain verts (default 1)."),
    ENVROW_LIVE("TERRAIN_TANIM", "Debug", "1",
        "Sub-tick lerp of scrolling terrain textures (default 1)."),
    ENVROW_LIVE("TERRAIN_CANIM", "Debug", "1",
        "Sub-tick lerp of animated terrain vertex colours (0 = hold the previous tick's)."),
    ENVROW_LATCHED("TERRAIN_ALLVIS", "Debug", "0",
        "Walk ALL sectors, bypass visibility groups (probe)."),
    ENVROW_LATCHED("TERRAIN_NEARQ", "Debug", "0",
        "Read the .s deferred near queue as debug oracle route."),
    ENVROW_LIVE("TERRAIN_NOTEX", "Debug", "0",
        "Feed terrain untextured."),
    ENVROW_LIVE("TERRAIN_TIERCOLOR", "Debug", "0",
        "Colour terrain by LOD tier."),
    ENVROW_LIVE("TERRAIN_TIERONLY", "Debug", "0",
        "Draw only LP (1) or only HP (2) tier."),

    { "devui_open", "Open this overlay at boot", "Debug", "--devui-open", "OPENPETE_DEVUI_OPEN",
      "Open this panel as soon as it initializes instead of waiting for its "
      "toggle key, expanding the Mods list and every mod node once so an "
      "unattended windowed run reaches a mod UI section body. The "
      "overlay's toggle opens and closes the panel at any time.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .help_arg = "0|1", .help_order = 120,
      .help_text = "open the dev config overlay as soon as it initializes\n"
          "instead of waiting for its toggle key, with the Mods\n"
          "list and every mod node expanded once, so an unattended run\n"
          "reaches a mod UI section body (on|off also accepted).\n"
          "Windowed native-vk runs only; inert elsewhere.",
      .key_action = "devui" },

    { "screenshot_overlays", "Screenshot overlays", "Debug", "--screenshot-overlays",
      "OPENPETE_SCREENSHOT_OVERLAYS",
      "Draw the settings menu, notifications, stats readout and version line "
      "into recomposed screenshots (headless and intermediate captures), as "
      "the window shows them. Off, those captures hold the picture without "
      "them. A windowed capture that reads back the presented image carries "
      "them either way.",
      KNOB_BOOL, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      get_stored, NULL, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = "0", .flag_arg = KNOB_FLAG_ARG_OPTIONAL,
      .help_arg = "[0|1]", .help_order = 175,
      .help_text = "draw the settings menu, notifications, stats readout and\n"
          "version line into recomposed screenshots (headless and\n"
          "intermediate captures), as the window shows them (default {def}\n"
          "= off; on|off also accepted)" },

    { "devui_inject_mouse", "Park the overlay mouse", "Debug",
      NULL, "OPENPETE_DEVUI_INJECT_MOUSE",
      "Post a fixed cursor position (window pixels, \"X,Y\") into this "
      "overlay's input every present, so an unattended run can hover a "
      "widget and read the result out of the log instead of needing a hand "
      "on the mouse.",
      KNOB_STRING, KNOB_RESTART_ONLY, NULL, 0, 0, 0,
      NULL, NULL, injectmouse_get_str, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .def_str = NULL },

    ENVROW_LATCHED("MOUSE_INJECT", "Debug", "",
        "Path of a mouse-event script (\"<pump-iteration> move x y | down b | "
        "up b | click2 b | rel dx dy | wheel n | menu 0|1 | capture 0|1 | shot | "
        "keydown k | keyup k\") "
        "pushed through the event pump as SDL events, so an unattended run "
        "drives every mouse consumer."),
    ENVROW_LATCHED("MOUSE_PROBE", "Debug", "0",
        "Log the mouse channel on change and every consumer gesture with its "
        "outcome."),
    ENVROW_DEVICE("WINDOW_STATE", "Debug", "0",
        "Log the window's flags and input focus at bring-up and at exit for a "
        "window created without --background, which prints them anyway. The "
        "comparison of the two is what proves the flag changed the state."),
    ENVROW_LATCHED_NOTE("PSYX_DITHER", "Debug",
        "0 in a run that writes -og screenshots, else 1",
        "1 = the PsyCross raster (the -og screenshot reference, --renderer "
        "psycross, the framebuffer copy-back StoreImage reads) adds the PSX "
        "GPU's 4x4 ordered dither to shaded polygons whose draw state "
        "requests it. 0 = no dither, so the reference compares with the "
        "undithered native render. Read at launch."),

    { "fillpick", "Fill-tag inspector", "Debug", NULL, "OPENPETE_FILLPICK",
      "Pick the terrain face under the crosshair and re-judge its LP fill "
      "tag live. Audit tool for the frozen list in "
      "tests/freecam/fill-tags.verdicts; verdicts land in a session file.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fillpick_get, fillpick_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .key_action = "fillpick" },

    { "fillpick_preview", "Fill-tag preview", "Debug", NULL, "OPENPETE_FILLPICK_PREVIEW",
      "With the inspector engaged, draw every LP face - not just the tagged "
      "ones - tinting the untagged candidates magenta. This is how a "
      "face that should be filling a hole is found before it is tagged.",
      KNOB_BOOL, KNOB_LIVE, NULL, 0, 0, 0,
      fillpick_prev_get, fillpick_prev_set, NULL, NULL, KNOB_SRC_DEFAULT, 0, {0}, 0,
      .key_action = "fillpick_preview" },

    ENVROW_LATCHED("ACTOR_OWN_KEY", "Debug", "1",
        "Actor faces contest only other mobys' keys (0 = own keys too)."),
    ENVROW_LATCHED("MOBY_OTBIAS", "Debug", "1",
        "Master switch for the three ordering-bias mechanisms mirrored from "
        "the game's OT depth pull: 0 turns off MOBY_TIE_PULL, MOBY_FACE_KEY "
        "and PLAYER_SORT_PULL together."),
    ENVROW_LATCHED("MOBY_TIE_PULL", "Debug", "1",
        "Moby walk: feed each vertex 127 world units nearer than its true "
        "view-Z when the record centre is in front of the camera, so a "
        "same-slot moby-versus-terrain tie resolves to the moby (0 = off)."),
    ENVROW_LATCHED("MOBY_FACE_KEY", "Debug", "1",
        "Moby walk: write each opaque actor face's ordering key (mean "
        "perspective W less 128 x the authored depth offset) into the actor "
        "key image for the key contest (0 = off)."),
    ENVROW_LATCHED("MOBY_SNAP", "Debug", "1",
        "Moby walk: a moby with an authored depth offset draws the part sunk "
        "into the opaque terrain it intersects just in front of that terrain, "
        "within its own penetration (0 = true depth cuts it)."),
    ENVROW_LATCHED("PLAYER_SORT_PULL", "Debug", "1",
        "Spyro walk: apply the sortDepth pulls - the front sentinel's "
        "whole-model near pull, otherwise the 127-unit tie pull and the "
        "mirrored copy's negative depth-key bias (0 = off)."),
    ENVROW_LIVE("MOBY_TRUESCOPE", "Debug", "1",
        "Scope moby og-key yields by true depth (0 = og compound painter)."),
    ENVROW_LATCHED("SHADED_OTBIAS", "Debug", "1",
        "Same pull for the SHADED walk."),
    ENVROW_LATCHED("TRUE_W", "Debug", "7",
        "Perspective W from the true view-Z, not the OT-pulled depth "
        "(bitmask 1=player 2=moby 4=world; 0 = legacy derive-from-depth)."),
    ENVROW_LATCHED("MOBY_SNAP_STALE", "Debug", "1",
        "Snap instead of lerp across stale interp pairs (default 1)."),
    ENVROW_LATCHED("MOBY_JUMP_HOLD", "Debug", "1",
        "Hold a replayed moby at its previous tick across a one-tick position "
        "jump (Spyro 2/3; 0 interpolates across it)."),
    ENVROW_LATCHED("LATE_SWITCH", "Debug", "1",
        "Sub-tick presents before alpha 1 draw the previous tick's particle "
        "and moby records, the newest tick's at alpha 1 (Spyro 2/3; 0 draws "
        "the newest tick's at every alpha)."),
    ENVROW_LATCHED("MOBY_REUSE_SPAWN", "Debug", "1",
        "A moby slot the allocator hands to a new record is a despawn of the "
        "old record and a spawn of the new one in the sub-tick pairing "
        "(Spyro 2/3; 0 pairs the two records)."),
    ENVROW_LATCHED("MOBY_MIRROR_HOLD", "Debug", "64",
        "Sub-tick hold threshold for a moby's one-tick turn, in byte angles "
        "(256 = one turn); 0 interpolates every interval."),
    ENVROW_LATCHED("MOBY_IDLE", "Debug", "0",
        "Idle animation for frozen mobys."),
    ENVROW_LATCHED("SHADED_NOHL", "Debug", "0",
        "Disable shaded-walk highlight."),
    ENVROW_LATCHED("ACTOR_FRONT", "Debug", "1",
        "Dragon-rescue actor front pass (default 1)."),
    ENVROW_LATCHED("ENV_C2", "Debug", "14.5",
        "Metal env-reflection calibration constant (default 14.5)."),
    ENVROW_LATCHED("ENV_AZSIGN", "Debug", "1.0",
        "Metal env-reflection azimuth sign."),
    ENVROW_LATCHED("SPYRO_SCRATCH_MAT", "Debug", "0",
        "Force old scratch-block matrix read."),
    ENVROW_LATCHED("SPYRO_UNTEX", "Debug", "0",
        "Draw Spyro feed untextured (bisect)."),
    ENVROW_LATCHED("GEM_FLYOUT_KEY", "Debug", "1",
        "Rotation-fingerprint keying of HUD gem-flyout interp (default 1)."),
    ENVROW_LATCHED("RA_REBASE", "Debug", "1",
        "Runahead camera rebase (default 1)."),
    ENVROW_LATCHED("RA_CAM_COMMIT", "Debug", "1",
        "Committed-timeline camera regions under runahead (default 1)."),
    ENVROW_LATCHED("INTERP_LINEAR", "Debug", "0",
        "Force linear lerp instead of pred1 Hermite."),
    ENVROW_LATCHED("INTERP_PERIOD_UNCAP", "Debug", "0",
        "Diagnostic: uncap interp period."),
    ENVROW_LATCHED("INTERP_PHASE_MS", "Debug", "0",
        "Capture-stamp phase bias vs present grid (0-16)."),
    ENVROW_LATCHED("BRACKET_CHECK", "Debug", "0",
        "Hash gameplay RAM around sub-tick brackets, log leaks."),
    ENVROW_LATCHED("FREECAM_SPIN", "Debug", "0",
        "Scripted continuous yaw sweep (units/tick)."),
    ENVROW_LATCHED("FREECAM_DRIFT", "Debug", "",
        "Scripted per-tick camera drift dx,dy,dz."),
    ENVROW_LATCHED("CAMLOCK_POSE", "Debug", "",
        "Engage the render camera lock at x,y,z,yaw,pitch (a camlock: "
        "log line pastes verbatim)."),
    ENVROW_LATCHED("CAMLOCK_FRAME", "Debug", "",
        "Engage the render camera lock (latch the live view) at VBL N."),
    ENVROW_LATCHED("OT_TRUNCATE", "Debug", "",
        "Submit only first N OT slots (paint-order bisection)."),
    ENVROW_LATCHED("TRANSITION_PROBE", "Debug", "0",
        "One line per (func,frame) during level transitions."),
    ENVROW_LATCHED("CUT_HOLD_ALL", "Debug", "1",
        "A held camera cut holds every moby record with it (default 1; 0 = each record keeps its own clock)."),
    ENVROW_LATCHED("CUT_LOAD_GUARD", "Debug", "1",
        "A camera cut is not held when tick N-1's canonical render drew no world or a level edge lies between the two ticks, and one verdict holds per tick pair (default 1; 0 = neither)."),
    ENVROW_LATCHED_NOTE("EAGER_POLICY", "Debug", "unset: every level-th slot of the present grid",
        "Eager planner target policy: greedy = evenly spaced targets; any other value = every level-th slot of the present grid."),
    ENVROW_LATCHED("EAGER_COUNT_SPARE", "Debug", "1",
        "Scenes a span must leave spare to count as clean for the eager planner's free-span and fill count controllers (default 1)."),
    ENVROW_LATCHED("EAGER_COUNT_RUNS", "Debug", "16",
        "Clean spans in a row after which the eager planner's free-span controller gives back one scene (default 16)."),
    ENVROW_LATCHED("EAGER_REPLAN", "Debug", "1",
        "1 = after the update VBL the eager planner re-spaces a free span's unproduced scenes to the room left (vsync off); 0 = the count controller's plan stands (default 1)."),
    ENVROW_LATCHED("FOG_SAT_CULL", "Debug", "0",
        "1 = a world moby the level's heavy fog saturates is skipped on a one-colour level and drawn from its LP model on a mixed-haze level (default 0)."),
    ENVROW_LATCHED("GLINT_OWN", "Debug", "1",
        "Sparkle records take an owning moby and draw at its depth (default 1; 0 = every star keeps its record's depth)."),
    ENVROW_LATCHED("GLOW_RANGE", "Debug", "1",
        "Under extended LOD, a glow record the routine dropped on its box or far test is fed (default 1; 0 = the routine's drop stands)."),
    ENVROW_LATCHED("GLOW_SYNTH", "Debug", "1",
        "Under extended LOD, a drawn-gated glow record the wider range frees is fed as a native glow (default 1; 0 = none)."),
    ENVROW_LATCHED("HANG_THRESHOLD", "Debug", "50000000",
        "Function entries the game may make without a VBL before the hang watchdog trips (default 50000000; 0 = no trip and no stall monitor)."),
    ENVROW_LATCHED("HD_CREATE_CAP", "Debug", "12",
        "HD replacement textures created and uploaded per present; the rest stay queued for the next present (default 12)."),
    ENVROW_LATCHED("INPUT_LEGACY", "Debug", "0",
        "1 returns pad pumping and decode to PsyCross and leaves the rumble hook uninstalled; refused with --input-lock (default 0)."),
    ENVROW_LATCHED("KEY_ORDER", "Debug", "1",
        "Actor key image: 1 = faces of the key-order class mark it and terrain keyed behind them yields; 0 = no face marks it, true depth decides; 2 = the yielded terrain fragment paints magenta (walked) or cyan (retained) instead of being discarded (default 1)."),
    ENVROW_LATCHED("KEY_TIER_ORDER", "Debug", "1",
        "In-bucket tier order of the key contest shader: 1 = on; 0 = the contest without it; 2 = on, painting the fragments it discards (default 1)."),
    ENVROW_LATCHED_NOTE("MOBY_JUMP_BOUND", "Debug", "unset: the game layout's bound",
        "Distance in world units a moby's step between ticks must exceed to count as a placement jump that is not interpolated (0 = no bound)."),
    ENVROW_LATCHED("MOBY_JUMP_MARKERS", "Debug", "1",
        "A moby step matching a row of the game's placement table counts as a placement jump (default 1; 0 = only the cutscene sampler's rule and the distance bound)."),
    ENVROW_LATCHED_NOTE("MOBY_MODE0_IR0_SUM", "Debug", "unset: the game's own switch",
        "Spyro 2/3 special material, mode 0: 1 = IR0 from the vertex colour sum; 0 = the per-moby IR0."),
    ENVROW_LATCHED("MOBY_POSE_HOLD", "Debug", "1",
        "A moby's two latest decoded poses are held, so a sub-tick present lerps from held vertices (default 1; 0 = re-decode from game memory)."),
    ENVROW_LATCHED("MOBY_PROP_LEGACY", "Debug", "0",
        "1 = moby classes 250 and 332 are the embedded props in every game, instead of the per-game curated list (default 0)."),
    ENVROW_LATCHED("MOBY_RANK_CARRY", "Debug", "1",
        "A sub-tick present takes the moby paint ranks the tick's pass stored (default 1; 0 = rank on every present)."),
    ENVROW_LATCHED("MOBY_RANK_REUSE", "Debug", "1",
        "A moby run whose census inputs equal the previous feed's byte for byte reuses its stored rank (default 1; 0 = census and rank every run; count = match without reusing and print the matched share of the census time)."),
    ENVROW_LATCHED("MOBY_RANK_STACKED", "Debug", "1",
        "Stacked-pair contest of the moby paint rank: a face pair sharing an edge within 5 degrees, a decal on its base (default 1; 0 = off)."),
    ENVROW_LATCHED("MOBY_SIMPLE_OCCL", "Debug", "1",
        "Simple-moby occlusion test (default 1; 0 = off)."),
    ENVROW_LATCHED("MOBY_SNAP_CARRY_REFRESH", "Debug", "1",
        "The moby snap carry's refresh test: a present keeps an endpoint's face set only while the moby stays in its region and every face keeps its verdict (default 1; 0 = off)."),
    ENVROW_LATCHED("MOBY_TERRAIN_ORDER", "Debug", "1",
        "The selected mobys' faces carry the terrain-order sentinel (default 1; 0 = off)."),
    ENVROW_LATCHED("MOBY_TRAIL", "Debug", "1",
        "A sub-tick present adds host-built records for the trailing-edge mobys the game's cull admits under the present's camera (default 1; 0 = none)."),
    ENVROW_LATCHED("MOBY_WORLD_KEY", "Debug", "1",
        "Where the game's layout sorts by one ordering table, a moby face's contest key is its world ordering-table bucket (default 1; 0 = the per-triangle mean-depth key)."),
    ENVROW_LATCHED_NOTE("PACER_SLACK_US", "Debug", "unset: adaptive, starting at the platform cap",
        "Pacer wake-up slack in microseconds, fixed for every thread (negative reads as 0)."),
    ENVROW_LATCHED("PACE_LATE_BUDGET_US", "Debug", "0",
        "Microseconds an intermediate present may overrun the next tick's start when its tail cost bound fits, 0 to 2000 (default 0 = never start a present that could overrun)."),
    ENVROW_LATCHED("PACE_SHED_K", "Debug", "0",
        "Deviation multiplier k of the present-shed cost bound, EMA + k deviations (default 0 = EMA + EMA/4)."),
    ENVROW_LATCHED("PART_GTE_CARRY", "Debug", "1",
        "The particle pairing walk models the GTE state a class-2 key skip carries (default 1; 0 = every record pairs under the camera state)."),
    ENVROW_LATCHED("PORTAL_BAND_BACK", "Debug", "24",
        "Spyro 1 portal: world units behind the opening plane the aperture's correction band is marked (default 24)."),
    ENVROW_LATCHED("PORTAL_RIM_NUDGED", "Debug", "0",
        "Spyro 1 portal: 1 = the inner-rim and preview-cover tests follow the corrected outline instead of the authored opening (default 0)."),
    ENVROW_LATCHED("PRESENT_TARGET_CLOCK", "Debug", "0",
        "1 = each present's content is timed for an evenly spaced display slot from a smoothed present clock, instead of the instant it was generated (default 0)."),
    ENVROW_LATCHED("PT_GUARD_US", "Debug", "900",
        "Present thread: microseconds after the vblank before the target at which the present is issued (default 900)."),
    ENVROW_LATCHED("PT_STAGED", "Debug", "1",
        "Present thread: a scene renders into a stage image ahead of its present instant (default 1; 0 = render at the instant)."),
    ENVROW_LATCHED("RANK_TIE", "Debug", "0",
        "Full-key moby paint order: 1 = the order and the contest; order or contest = that part alone; 0 = none, the bucket order with ties in insertion order and the crossing contest (default 0)."),
    ENVROW_LATCHED("RUMBLE_LEGACY", "Debug", "0",
        "1 leaves the rumble registration hook uninstalled, so PadSetAct keeps PsyCross's one-shot behaviour (default 0)."),
    ENVROW_LATCHED("SEQ_SORT", "Debug", "1",
        "2D triangles order by emission sequence (default 1; 0 = scene order)."),
    ENVROW_LATCHED_NOTE("SEQ_SORT_RUNS", "Debug", "unset: runs of the 2D-layer subsequence",
        "scene = the emission-sequence sort takes runs of the whole scene, bounded by any unnumbered triangle."),
    ENVROW_LATCHED("TC_RANGE_DRAW", "Debug", "1",
        "Terrain cache arenas draw only their used ranges (default 1; 0 = whole-range arena draws)."),
    ENVROW_LATCHED("TERRAIN_CACHE_FORCE", "Debug", "0",
        "1 lets the terrain GPU cache run on a software Vulkan device (default 0)."),
    { .id = "TERRAIN_DRAIN_SKIP", .label = "TERRAIN_DRAIN_SKIP", .section = "Debug",
      .env = "OPENPETE_TERRAIN_DRAIN_SKIP",
      .help = "Terrain sub-cell key packing, taken at each walk: 1 = a skipped sub-cell carries the sentinel key; 0 = it is keyed at the face's least key and drawn (default 1).",
      .type = KNOB_BOOL, .mut = KNOB_LIVE, .get_num = drainskip_get, .set_num = drainskip_set,
      .boot_src = KNOB_SRC_DEFAULT, .def_str = "on" },
    ENVROW_LATCHED("TERRAIN_ENVMODEL", "Debug", "1",
        "The walker model drives a sector's HP banks on a tick the game's walkers skipped it (default 1; 0 = the older pose in memory draws)."),
    ENVROW_LATCHED("TERRAIN_FANOUT_MIN", "Debug", "400",
        "Faces a terrain walk needs before its sector jobs fan out to the work pool (default 400)."),
    ENVROW_LATCHED("TERRAIN_HPIMPORT", "Debug", "1",
        "On a distance-binning game, a scene sector with LP faces borrows HP faces from a source sector holding every one of them (default 1; 0 = borrow nothing)."),
    ENVROW_LATCHED("TERRAIN_HQFOG", "Debug", "1",
        "Heavy-fog fade of the terrain walk (default 1; 0 = off)."),
    ENVROW_LATCHED("TERRAIN_KFMODEL", "Debug", "1",
        "Deferred keyframe overlay of keyframe-animated terrain (default 1; 0 = off)."),
    ENVROW_LATCHED("TERRAIN_POSRING", "Debug", "1",
        "Poly-anim sectors read their positions from a position ring (default 1; 0 = positions bake into the chunks and re-bake on every roll)."),
    ENVROW_LATCHED("TEXPACK_FOG_CONT", "Debug", "1",
        "Texture pack: a ladder-eligible replaced face takes the continuous fog model (default 1; 0 = per-face rungs)."),
    ENVROW_LATCHED("TEXPACK_FOG_FIT", "Debug", "1",
        "Texture pack: each replaced texture's fog offset is retargeted by an affine fit of its art to the stock art (default 1; 0 = the global fog shift only)."),
    ENVROW_LATCHED("TEXPACK_LP_FIT", "Debug", "1",
        "Texture pack: the LP terrain grade folds the previous walk's level mean into sectors that yielded no fit (default 1; 0 = off)."),
    ENVROW_LATCHED("TEXPACK_PREFETCH", "Debug", "1",
        "Texture pack: every entry of an enabled source is queued for decode before its first draw (default 1; 0 = decode on first draw)."),
    ENVROW_LATCHED("UI_PERIOD_DEFER", "Debug", "1",
        "2D of a table the game has not stitched yet is held back to a later scene (default 1; 0 = it stays in the scene)."),
    ENVROW_LATCHED("UI_RANK_WALK", "Debug", "0",
        "1 = the 2D ranks by the DrawOTag walk on a game that ranks it by link (default 0)."),
    ENVROW_LATCHED_NOTE("COMPILE_POOL", "Debug", "unset: every logical core until the first tick, then half, at least 1",
        "Worker count of the pipeline compile pool, the same before and after the first tick."),
    ENVROW_DEVICE("EAGER_LATTICE", "Debug", "0",
        "1 = the presenter runs on its own thread and paces on the display's vblank lattice, fitted from flip times; turns on present timing (default 0)."),
    ENVROW_DEVICE("PRESENT_TIMING", "Debug", "",
        "Vulkan present_id and present_wait where offered: 1 = on; 0 = off even when PACE_FLIP_LOG or EAGER_LATTICE asks; unset = on only when one of them asks. Costs pacing."),
    ENVROW_DEVICE("PACE_FLIP_LOG", "Debug", "0",
        "1 = the pacer's idle wait waits on the queued present, timing the display flip; turns on present timing (default 0)."),
    ENVROW_DEVICE("PIPE_EXTS", "Debug", "1",
        "Pipeline extensions where offered: creation feedback, cache control and the graphics pipeline library (default 1; 0 = none)."),
    ENVROW_DEVICE("PIPELINE_LIBRARY", "Debug", "1",
        "Graphics pipeline library where offered (default 1; 0 = whole pipelines)."),
    ENVROW_DEVICE("PIPELINE_LTO", "Debug", "1",
        "Library-linked pipelines get a link-time-optimized link (default 1; 0 = the fast link only)."),
    ENVROW_DEVICE("PIPELINE_CACHE_DIR", "Debug", "",
        "Directory of the pipeline cache file, in place of the per-user one (unset = the per-user one)."),
    ENVROW_DEVICE("PIPELINE_CACHE_COLD", "Debug", "0",
        "1 removes the pipeline cache file before it is read: a cold boot (default 0)."),
    ENVROW_DEVICE("RHI_BALLOON", "Debug", "",
        "Comma list of <heap>:<MiB>, <heap>:fill or <heap>:fill-<MiB>: device memory held in that heap after device creation, so later allocations there are refused."),

    /* -------- Debug diagnostics (stderr censuses; flip on, watch, flip off) -------- */
    ENVROW_LATCHED("PIPE_LOG", "Debug diagnostics", "1",
        "1 = one log line per graphics pipeline create (whole, library part "
        "or link), written before the driver call and waited into the log "
        "file: the stages' .spv names, SPIR-V sizes and hashes, "
        "specialization constants and fixed-function state. 0 = no line; "
        "the crash report's breadcrumb of the create in progress is kept. "
        "Read at native renderer device creation."),
    ENVROW_LATCHED("REEXTRACT_DBG", "Debug diagnostics", "0",
        "Per-present alpha/bracket summary of the re-extract spine."),
    ENVROW_LATCHED("REEXTRACT_FEEDDBG", "Debug diagnostics", "0",
        "Per-frame feeder freshness/counts (spyro + moby)."),
    ENVROW_LATCHED("CANON_CAPDBG", "Debug diagnostics", "0",
        "1/200-frame canonical-capture row prints."),
    ENVROW_LATCHED("CAP_ROLL_DBG", "Debug diagnostics", "0",
        "One line per interp ring roll."),
    ENVROW_LATCHED("ALPHA_MAP_DBG", "Debug diagnostics", "0",
        "Per-present present-time to interp-time mapping."),
    ENVROW_LATCHED("PART_INTERP_DBG", "Debug diagnostics", "0",
        "1/s particle pool-interp verdict census."),
    ENVROW_LATCHED("ANIMDBG", "Debug diagnostics", "0",
        "Spyro anim segment/blend + sub-tick vertex probe."),
    ENVROW_LATCHED("SPYRO_RECDBG", "Debug diagnostics", "0",
        "Per-epoch player-hook record census (calls per feeder, ring roll, hook-vs-ring bytes, pairing bound)."),
    ENVROW_LATCHED("MOBY_TIER_DBG", "Debug diagnostics", "0",
        "Extraction-tier census (proj/walk us, cache hits)."),
    ENVROW_LATCHED("MOBY_OCCL_DBG", "Debug diagnostics", "0",
        "1/s occlusion cull/seen breakdown."),
    ENVROW_LATCHED("MOBY_ANIMSW_DBG", "Debug diagnostics", "0",
        "Animation-switch cross/snap counters."),
    ENVROW_LATCHED("MOBY_GPF_DBG", "Debug diagnostics", "0",
        "One line per GPF re-extract."),
    ENVROW_LATCHED("MOBY_CLASSDBG", "Debug diagnostics", "0",
        "One line per drawn moby: class + vaddr + world position."),
    ENVROW_LATCHED("MOBY_CLERP_DBG", "Debug diagnostics", "0",
        "Face-0 colour-lerp ramp per present."),
    ENVROW_LATCHED("MOBY_INTERP_MISS", "Debug diagnostics", "0",
        "Record mobys with no interp snapshot."),
    ENVROW_LATCHED("SHADED_INTERP_MISS", "Debug diagnostics", "0",
        "Same for shaded mobys."),
    ENVROW_LATCHED("MOBY_TRIDBG", "Debug diagnostics", "",
        "Log moby walk tris in a screen box x0,y0,x1,y1,fA,fB."),
    ENVROW_LATCHED("SCENE_BOXDBG", "Debug diagnostics", "",
        "Log composed scene tris overlapping a PSX-screen box x0,y0,x1,y1 at VBLs fA..fB, "
        "with a per-present tested/covering count; retained terrain cache off while set."),
    ENVROW_LATCHED("SCENE_CAPDBG", "Debug diagnostics", "0",
        "Print new peaks of scene-tri demand vs caps."),
    ENVROW_LATCHED("TERRAIN_CACHE_DBG", "Debug diagnostics", "0",
        "~1/s sector-cache hit/miss census."),
    ENVROW_LATCHED("OCCL_CAMDBG", "Debug diagnostics", "0",
        "Trace the occlusion grid's camera: one line per grid queue and retrieval, "
        "and one per query the camera bound admits (distance and matrix trace)."),
    ENVROW_LATCHED("TERRAIN_OCCL_DBG", "Debug diagnostics", "0",
        "Sector occlusion-cull census."),
    ENVROW_LATCHED("TERRAIN_OCCL_AUDIT", "Debug diagnostics", "",
        "From this VBL on, check every sector the occlusion cull calls off-screen "
        "against the exact sphere-vs-frame test; prints each false off-screen sector "
        "and cumulative totals at exit."),
    ENVROW_LATCHED("TERRAIN_TIMERS", "Debug diagnostics", "0",
        "~1/s per-sector walk-cost census."),
    ENVROW_LATCHED("TERRAIN_VISDBG", "Debug diagnostics", "0",
        "Visible-sector list diagnostics."),
    ENVROW_LATCHED("TERRAIN_ANIMDBG", "Debug diagnostics", "0",
        "Per-present env-anim change census."),
    ENVROW_LATCHED("TILE_CENSUS", "Debug diagnostics", "0",
        "Write <tile>-vk.census beside every native capture: integer counters "
        "of the scene the capture composed (triangles per render path, tag kind, "
        "category, blend mode, flag and moby class; retained terrain sectors; "
        "the frozen backdrop's counters under backdrop.), named in "
        "core/diag/tile_census.def. A 40-digit hex value is written as the "
        "sidecar's source tree. Counting selects no code path; the presentation "
        "corpus scripts set it on every leg."),
    ENVROW_LATCHED("MOD_TABLE_CENSUS", "Debug diagnostics", "0",
        "Mod-table census at exit: one line per growable mod table with "
        "its element count, the fixed capacity it used to have, its "
        "storage size and how many times it grew. 1 writes the report to "
        "stderr; any other value names a file to write it to, which a "
        "reader needs because stderr rides a pipe whose pump can lose the "
        "tail of a burst at exit."),
    ENVROW_LATCHED("TEXPACK_CATCENSUS", "Debug diagnostics", "0",
        "HD-texpack category census (which categories resolve per level)."),
    ENVROW_LATCHED("TEXPACK_ANIMPROBE", "Debug diagnostics", "",
        "Raw Tiledef words of record N each present (env-anim rewrite shape)."),
    ENVROW_LATCHED("TEXSEL_DBG", "Debug diagnostics", "0",
        "Terrain texture-id selector match census."),
    ENVROW_LATCHED("TEXSEL_DBG2", "Debug diagnostics", "0",
        "Material texture-id selector census (compose side)."),
    ENVROW_LATCHED("TERRAIN_VANIMDBG", "Debug diagnostics", "0",
        "Vert-anim refresh census."),
    ENVROW_LIVE("TERRAIN_FOGDBG", "Debug diagnostics", "0",
        "Fog decode debug prints."),
    ENVROW_LATCHED("TERRAIN_CALDBG", "Debug diagnostics", "0",
        "Near-classification calibration stats vs .s queue."),
    ENVROW_LATCHED("TERRAIN_NEARDBG", "Debug diagnostics", "0",
        "Census of straddling foreground quads by route."),
    ENVROW_LIVE("TERRAIN_UVDBG", "Debug diagnostics", "0",
        "UV/tile A/B vs captured .s prims."),
    ENVROW_LIVE("TERRAIN_UVBOX", "Debug diagnostics", "",
        "Box filter x0,y0,x1,y1 for UVDBG."),
    ENVROW_LIVE("TERRAIN_UVALL", "Debug diagnostics", "0",
        "UVDBG print-all modifier."),
    ENVROW_LIVE("TERRAIN_POINT", "Debug diagnostics", "",
        "Frontmost native tri + .s prim under screen points x,y;x,y."),
    ENVROW_LIVE("PARTICLE_DEBUG", "Debug diagnostics", "0",
        "Native-vs-.s particle selector counts per present."),
    ENVROW_LATCHED("STREAK_DBG", "Debug diagnostics", "0",
        "Log emitted line/fan legs over 300 px."),
    ENVROW_LATCHED("PART_PHASE_DBG", "Debug diagnostics", "0",
        "Per-present phase-lerp trace of first particle."),
    ENVROW_LATCHED("GLOW_ANIM_DBG", "Debug diagnostics", "0",
        "Glow raw-change census + phase trace."),
    ENVROW_LATCHED("SPARKLE_INTERP_DBG", "Debug diagnostics", "0",
        "Per-present sparkle slot-0 phase trace."),
    ENVROW_LATCHED("PORTAL_MAT_DBG", "Debug diagnostics", "0",
        "Per-dome derived-vs-GTE matrix validation print."),
    ENVROW_LATCHED("PORTAL_REEXTRACT_DBG", "Debug diagnostics", "0",
        "1/20-frame portal replay-state print."),
    ENVROW_LIVE("POLYBUF_GUARD", "Debug diagnostics", "0",
        "polygon-buffer overflow detector: logs when the prim cursor "
        "passes the buffer end (Spyro 1, into g_WorldOT) or the draw-scratch "
        "carve at end - 0x3000 (Spyro 2, 3), and the closest approach. "
        "0=off (default), 1=on, verbose=every sample"),
    ENVROW_LATCHED("NOLOD_DOME_DBG", "Debug diagnostics", "0",
        "Per-event forced-dome feed print under extended LOD."),
};
#define KNOB_DEBUG_COUNT (sizeof(g_builtin_debug) / sizeof(g_builtin_debug[0]))
#else
#define KNOB_DEBUG_COUNT 0
#endif /* OPENPETE_HIDE_DEBUG_FLAGS */

/* Working table: the built-ins, copied once, plus room for module-contributed
 * rows. One flat array because the UI walks a single {ptr, count} and groups by
 * section; adds happen at feature apply-options time, long before the overlay
 * exists, so no row pointer the UI holds can be invalidated by an insertion. */
#define KNOB_RELEASE_COUNT (sizeof(g_builtin) / sizeof(g_builtin[0]))
#define KNOB_BUILTIN_COUNT (KNOB_RELEASE_COUNT + KNOB_DEBUG_COUNT)
#define KNOB_EXTRA_MAX     16

static knob_desc_t g_rows[KNOB_BUILTIN_COUNT + KNOB_EXTRA_MAX];
static size_t      g_row_count;

/** @brief Copy the built-in rows into the working table on first use: the release rows, then the debug rows marked debug_block.
 *
 * Later calls do nothing. The copy is unsynchronized: callers are expected to
 * run on one thread during boot. */
static void rows_init(void) {
    if (g_row_count) return;
    memcpy(g_rows, g_builtin, sizeof g_builtin);
#ifndef OPENPETE_HIDE_DEBUG_FLAGS
    memcpy(g_rows + KNOB_RELEASE_COUNT, g_builtin_debug, sizeof g_builtin_debug);
    for (size_t i = KNOB_RELEASE_COUNT; i < KNOB_BUILTIN_COUNT; i++)
        g_rows[i].debug_block = 1;
#endif
    g_row_count = KNOB_BUILTIN_COUNT;
}

knob_desc_t *knob_registry(size_t *count) {
    rows_init();
    if (count) *count = g_row_count;
    return g_rows;
}

int openpete_knob_add(const knob_desc_t *row) {
    if (!row) return -1;
    rows_init();
    if (g_row_count >= KNOB_BUILTIN_COUNT + KNOB_EXTRA_MAX) {
        OPL_WARN_F("knobs", "no room for row \"%s\" (extra capacity %d)",
                   row->id ? row->id : "?", KNOB_EXTRA_MAX);
        return -1;
    }

    /* Sections are rendered as contiguous runs, so a row joins its section by
     * landing right after that section's last row; an unknown section starts
     * its own run at the end. */
    size_t at = g_row_count;
    for (size_t i = 0; i < g_row_count; i++)
        if (row->section && !strcmp(g_rows[i].section, row->section))
            at = i + 1;

    memmove(&g_rows[at + 1], &g_rows[at],
            (g_row_count - at) * sizeof g_rows[0]);
    g_rows[at] = *row;
    g_row_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* boot resolution: value + source stamping                           */
/* ------------------------------------------------------------------ */

/** @brief The overlay source tag of a row, from the config resolver's winning layer.
 *
 * openpete_config_knob_layer holds the layer order; this maps its label to
 * the overlay's three tags: "default" (or NULL) is DEFAULT, "env" or a
 * label ending ".env" is ENV, and every other layer (the command line, the
 * bk2 sidecar, saved, an [args] or bare-key file layer) is CLI. The overlay
 * shows the precise layer and the pinned state separately, from
 * openpete_config_knob_layer and openpete_config_knob_pinned. */
static knob_src_t knob_src_from_config(const knob_desc_t *k) {
    const char *layer = openpete_config_knob_layer(k->id);
    if (!layer || !strcmp(layer, "default")) return KNOB_SRC_DEFAULT;
    if (!strcmp(layer, "env")) return KNOB_SRC_ENV;
    size_t n = strlen(layer);
    if (n >= 4 && !strcmp(layer + n - 4, ".env")) return KNOB_SRC_ENV;
    return KNOB_SRC_CLI;
}

/* Set by the first knob_registry_init (knob_registry_inited). */
static int s_registry_inited;

int knob_registry_inited(void) { return s_registry_inited; }

void knob_registry_init(const struct openpete_options *opts) {
    g_opts = opts;
    s_registry_inited = 1;

    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        knob_desc_t *k = &ks[i];
        k->boot_src = knob_src_from_config(k);

        /* Seed the row's displayed value from the parsed options. WHERE the
         * value came from is not decided here: config.c resolved the whole
         * layer stack at boot, and re-deriving it from each field's unset
         * sentinel would be a second implementation of the precedence rules,
         * free to drift from the first. The post-pass below asks the
         * resolver. */
        if (!strcmp(k->id, "fullscreen")) {
            k->stored_num = opts->fullscreen ? 1 : 0;
        } else if (!strcmp(k->id, "aspect")) {
            if (opts->aspect_w_over_h > 0.0f) {
                k->stored_num = nearest_enum_idx(k_aspect_vals + 1, 5,
                                                 opts->aspect_w_over_h) + 1;
            } else {
                k->stored_num = 0;   /* auto (also the default) */
            }
        } else if (!strcmp(k->id, "ssaa")) {
            k->stored_num = ssaa_idx_of(opts->ssaa > 0 ? opts->ssaa : 1);
        } else if (!strcmp(k->id, "vsync")) {
            if (opts->vsync != -1) {
                k->stored_num = opts->vsync ? 1 : 0;
            } else {
                const char *e = openpete_config_env("OPENPETE_PRESENT_MODE");
                if (e && (!strcmp(e, "immediate") || !strcmp(e, "mailbox"))) {
                    k->stored_num = 0;
                } else {
                    k->stored_num = 1;   /* default FIFO */
                }
            }
        } else if (!strcmp(k->id, "perf_stats")) {
            k->stored_num = opts->perf_stats;
        } else if (!strcmp(k->id, "perf_position")) {
            k->stored_num = opts->perf_position;
        } else if (!strcmp(k->id, "osd")) {
            k->stored_num = opts->osd ? 1 : 0;
        } else if (!strcmp(k->id, "osd_position")) {
            k->stored_num = opts->osd_position;
        } else if (!strcmp(k->id, "osd_timeout")) {
            k->stored_num = (double)opts->osd_timeout_ms / 1000.0;
        } else if (!strcmp(k->id, "vertex_precision")) {
            k->stored_num = (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER);
        } else if (!strcmp(k->id, "raster_quantize")) {
            /* mirror the renderer's auto-pair resolve (-1 = 1x iff integer snap) */
            k->stored_num = (opts->raster_quantize >= 0) ? opts->raster_quantize
                          : (opts->vertex_precision == OPENPETE_VERTEX_PRECISION_INTEGER ? 1 : 0);
        } else if (!strcmp(k->id, "dither")) {
            k->stored_num = opts->dither ? 1 : 0;
        } else if (!strcmp(k->id, "texture_mapping")) {
            k->stored_num = (opts->texture_mapping == OPENPETE_TEXTURE_MAPPING_AFFINE);
        } else if (!strcmp(k->id, "lod")) {
            k->stored_num = (opts->lod != OPENPETE_LOD_ORIGINAL);
        } else if (!strcmp(k->id, "sector_visibility")) {
            k->stored_num = (double)opts->sector_visibility;
        } else if (!strcmp(k->id, "fog")) {
            k->stored_num = opts->fog_mode + 1;
        } else if (!strcmp(k->id, "fog_scale")) {
            k->stored_num = openpete_fog_scale;
        } else if (!strcmp(k->id, "render_fps")) {
            k->stored_num = opts->render_fps;
        } else if (!strcmp(k->id, "rewind")) {
            k->stored_num = opts->rewind_window;
        } else if (!strcmp(k->id, "tick_scale")) {
            k->stored_num = opts->tick_scale;
        } else if (!strcmp(k->id, "runahead")) {
            k->stored_num = opts->runahead;
        } else if (!strcmp(k->id, "fov")) {
            k->stored_num = opts->fov_deg;
        } else if (!strcmp(k->id, "mute")) {
            k->stored_num = opts->mute ? 1 : 0;
        } else if (!strcmp(k->id, "sfx")) {
            k->stored_num = opts->sfx_volume;
        } else if (!strcmp(k->id, "music")) {
            k->stored_num = opts->music_volume;
        } else if (!strcmp(k->id, "vibration")) {
            k->stored_num = opts->vibration;
        } else if (!strcmp(k->id, "mouse")) {
            k->stored_num = opts->mouse != 0 ? 1 : 0;
        } else if (!strcmp(k->id, "input_lock")) {
            k->stored_num = opts->input_lock > 0 ? 1 : 0;
        } else if (!strcmp(k->id, "background")) {
            k->stored_num = opts->background > 0 ? 1 : 0;
        } else if (!strcmp(k->id, "mouse_sensitivity")) {
            k->stored_num = opts->mouse_sensitivity >= 0.0f ? opts->mouse_sensitivity : 1.0;
        } else if (!strcmp(k->id, "record_cfr")) {
            k->stored_num = opts->record_cfr;
        } else if (!strcmp(k->id, "record_frames")) {
            k->stored_num = opts->record_frames;
        } else if (!strcmp(k->id, "devui_open")) {
            if (opts->devui_open >= 0) {
                k->stored_num = opts->devui_open ? 1 : 0;
            } else {
                const char *e = getenv("OPENPETE_DEVUI_OPEN");
                k->stored_num = (e && *e && *e != '0') ? 1 : 0;
            }
        } else if (!strcmp(k->id, "record_fps_overlay")) {
            if (opts->record_fps_overlay >= 0) {
                k->stored_num = opts->record_fps_overlay ? 1 : 0;
            } else {
                const char *e = getenv("OPENPETE_FPS_IN_RECORD");
                k->stored_num = (e && *e && *e != '0') ? 1 : 0;
            }
        } else if (!strcmp(k->id, "renderer")) {
            k->stored_num = opts->renderer;
        } else if (!strcmp(k->id, "skip_to_level")) {
            k->stored_num = opts->skip_to_level;
        } else if (!strcmp(k->id, "skip_to_cutscene")) {
            k->stored_num = opts->skip_to_cutscene;
        } else if (!strcmp(k->id, "log_format")) {
            k->stored_num = opts->log_format;
        } else if (!strcmp(k->id, "freecam") || !strcmp(k->id, "camlock")) {
            /* Both start disengaged whatever armed them: the env var (or
             * OPENPETE_CAMLOCK_POSE) requests engagement later, at a point
             * the feature itself picks. */
            k->stored_num = 0;
        }

        /* Stamp the boot value for the "reset to boot" affordance. Actions
         * carry no value to reset to. */
        if (k->type == KNOB_ACTION) {
            continue;
        } else if (k->type == KNOB_STRING) {
            if (k->get_str) k->get_str(k, k->boot_str, sizeof k->boot_str);
        } else {
            k->boot_num = k->get_num ? k->get_num(k) : k->stored_num;
        }
    }
}

int openpete_knob_bind_str(const char *id,
                           void (*get_str)(const knob_desc_t *k,
                                           char *buf, size_t cap),
                           int (*set_str)(knob_desc_t *k, const char *v)) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ks[i].id, id) != 0) continue;
        if (get_str) ks[i].get_str = get_str;
        if (set_str) { ks[i].set_str = set_str; ks[i].mut = KNOB_LIVE; }
        return 0;
    }
    return -1;   /* row absent (compiled out) - nothing to bind */
}

int openpete_knob_bind_vocab(const char *id, const char *const *tokens,
                             int count) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ks[i].id, id) != 0) continue;
        ks[i].enum_values = tokens;
        ks[i].enum_count  = count;
        return 0;
    }
    return -1;   /* row absent (compiled out) - nothing to bind */
}

int knob_config_value_str(const knob_desc_t *k, char *buf, size_t cap) {
    if (!k || k->type == KNOB_ACTION) return -1;
    if (cfg_schema_find_id(k->id) < 0) return -1;   /* not a config knob */

    /* The SSAA row is an enum of "off"/"2x"/"3x"/"4x" because that is what
     * reads well in a menu; the flag takes the factor (1 = off). Every other
     * enum row already displays the CLI's own words. */
    if (!strcmp(k->id, "ssaa")) {
        int idx = (int)(k->get_num ? k->get_num(k) : k->stored_num);
        if (idx < 0 || idx >= K_SSAA_N) idx = 0;
        snprintf(buf, cap, "%d", k_ssaa_vals[idx]);
        return 0;
    }
    /* The fog row's first entry follows the LOD mode: no key is its value. */
    if (!strcmp(k->id, "fog")) {
        int idx = (int)(k->get_num ? k->get_num(k) : k->stored_num);
        if (idx <= 0 || idx > 3) return 1;
        snprintf(buf, cap, "%s", k_fog_cfg_names[idx]);
        return 0;
    }

    knob_value_str(k, buf, cap);
    /* get_str placeholders ("(default)", "(none)", "(unset - 1)") describe
     * the absence of a value; writing one back as a literal would turn it
     * into a real setting on the next boot. */
    if (buf[0] == '(') return -1;
    return 0;
}

/** @brief Parse a bool spelling: 1 for "1", "on", "true", "yes"; 0 for "0", "off", "false", "no"; -1 otherwise. */
static int knob_bool_word(const char *v) {
    static const char *const on[]  = { "1", "on", "true", "yes" };
    static const char *const off[] = { "0", "off", "false", "no" };
    for (size_t i = 0; i < sizeof on / sizeof on[0]; i++) {
        if (!strcmp(v, on[i]))  return 1;
        if (!strcmp(v, off[i])) return 0;
    }
    return -1;
}

/* Documented at the declaration in openpete_knobs.h. */
int knob_config_values_equal(const knob_desc_t *k, const char *a,
                             const char *b) {
    if (k->type == KNOB_BOOL) {
        const int x = knob_bool_word(a), y = knob_bool_word(b);
        return x >= 0 && x == y;
    }
    char *ea = NULL, *eb = NULL;
    const double da = strtod(a, &ea), db = strtod(b, &eb);
    if (ea != a && *ea == '\0' && eb != b && *eb == '\0') return da == db;
    return !strcmp(a, b);
}

int knob_registry_settle_ids(const char *const *ids, int n) {
    size_t count;
    knob_desc_t *ks = knob_registry(&count);
    int rows = 0;
    for (int i = 0; i < n; i++) {
        knob_desc_t *k = NULL;
        for (size_t j = 0; j < count && !k; j++)
            if (ids[i] && !strcmp(ks[j].id, ids[i])) k = &ks[j];
        /* A row with no setter is not a setting anyone edits in session:
         * its getter reports a resolved value (load_delay reads the game's
         * own default once the disc is identified), which is never written. */
        if (k && !k->set_num && !k->set_str) continue;
        char now[KNOB_STR_MAX];
        const int vs = k ? knob_config_value_str(k, now, sizeof now) : -1;
        if (vs == 1) {   /* the row is unset: its saved key goes */
            (void)openpete_config_unsave_knob(k->id);
            rows++;
            continue;
        }
        if (vs != 0) continue;

        /* A launch-tier pin does not stop the save: the value is recorded
         * and takes effect on a launch without the pin. */
        char below[KNOB_STR_MAX];
        int r = openpete_config_knob_value_below_saved(k->id, below, sizeof below);
        if (r == 0 && k->def_str && *k->def_str) {
            snprintf(below, sizeof below, "%s", k->def_str);
            r = 1;
        }
        if (r == 1 && knob_config_values_equal(k, now, below)) {
            (void)openpete_config_unsave_knob(k->id);
            rows++;
        } else if (openpete_config_save_knob(k->id, now) == 0) {
            rows++;
        }
    }
    return rows;
}

int knob_registry_save_ids(const char *const *ids, int n) {
    const int rows = knob_registry_settle_ids(ids, n);
    /* The file is written whenever a row was settled, changed in memory or
     * not: a removal that a failed write left only in memory reaches disk
     * on the retry. */
    if (rows == 0) return 0;
    if (openpete_config_save_flush() != 0) return -1;
    return rows;
}

void knob_registry_late_stamp(void) {
    size_t n;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(ks[i].id, "render_fps") ||
            !strcmp(ks[i].id, "cadence") ||
            !strcmp(ks[i].id, "tick_scale") ||
            !strcmp(ks[i].id, "runahead")) {
            /* The pacer seeds its live state (resolved render-fps target,
             * tick scale, gated runahead depth) and the session the cadence
             * policy at host-loop start, after
             * knob_registry_init ran; adopt the settled value as the boot
             * value so the row doesn't read as "edited" on first open. */
            ks[i].boot_num   = ks[i].get_num(&ks[i]);
            ks[i].stored_num = ks[i].boot_num;
        }
    }
}

void knob_value_str(const knob_desc_t *k, char *buf, size_t cap) {
    if (k->type == KNOB_ACTION) {
        if (k->get_str) k->get_str(k, buf, cap);
        else snprintf(buf, cap, "%s", k->action_label ? k->action_label : "Apply");
        return;
    }
    if (k->type == KNOB_STRING) {
        if (k->get_str) k->get_str(k, buf, cap);
        else snprintf(buf, cap, "?");
        return;
    }
    double v = k->get_num ? k->get_num(k) : k->stored_num;
    switch (k->type) {
    case KNOB_BOOL:  snprintf(buf, cap, "%s", v != 0.0 ? "on" : "off"); break;
    case KNOB_INT:   snprintf(buf, cap, "%d", (int)v); break;
    case KNOB_FLOAT: snprintf(buf, cap, "%.3g", v); break;
    case KNOB_ENUM: {
        int i = (int)v;
        if (k->enum_values && i >= 0 && i < k->enum_count)
            snprintf(buf, cap, "%s", k->enum_values[i]);
        else
            snprintf(buf, cap, "%d", i);
        break;
    }
    default: snprintf(buf, cap, "?"); break;
    }
}
