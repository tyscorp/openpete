// core/ui/openpete_menu.c - in-game settings menu state + logic.
// See openpete_menu.h.
//
// Every value row writes through the setter of the knob registry row it names
// (ROW_SURFACE), the setter the dev overlay's row writes through, so the menu
// and the overlay change a setting by one path. Those setters are
// presentation and pacing state, never gameplay-readable RAM, with one
// deliberate exception: FREECAM's row raises the free camera's engage request
// (the F key's), an opt-in gameplay-state divergence by design
// (openpete_freecam.h). This file is state + apply; the
// panel is drawn by the native renderer and the row glyphs by the text-moby
// builder, both reading the layout and row strings defined here.

#include "openpete_menu.h"
#include "openpete.h"                /* openpete_options_t, openpete_set_render_fps */
#include "openpete_native_render.h"  /* set_aspect / set_ssaa / set_vsync */
#include "openpete_log.h"
#include "openpete_settings_save.h" /* autosave of the persisted rows */
#include "openpete_osd.h"           /* announce every change the player makes */
#include "openpete_freecam.h"        /* freecam active (FREECAM row re-sync) */
#include "openpete_knobs.h"          /* the registry rows the value rows write */
#include "openpete_mods.h"           /* loaded-mod enumeration (MOD rows) */
#include "openpete_launcher.h"        /* EXIT GAME back to the launcher */
#include "no_lod.h"         /* openpete_no_lod_wanted, openpete_fog_mode_pinned */
#include "openpete_host_input.h"     /* the published mouse snapshot */
#include "openpete_host_mouse.h"     /* totals, ownership, probe */
#include "openpete_devui.h"          /* ADVANCED opens the overlay */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* --- rows ---------------------------------------------------------------- */
enum { ROW_DISPLAY, ROW_ASPECT, ROW_FPS, ROW_FOV, ROW_SHOWFPS, ROW_VSYNC, ROW_SSAA,
       ROW_WOBBLE, ROW_PIXEL, ROW_TEXMAP, ROW_DITHER, ROW_LOD, ROW_FOG,
       ROW_FREECAM, ROW_ADVANCED, ROW_QUIT,
       ROW_COUNT,
       /* MODSEL/MODEN are parked past ROW_COUNT: hidden from display +
        * navigation, but their plumbing stays live and compiled - the case
        * labels below and the mod enumeration. Move a row back before
        * ROW_COUNT (and restore its ROW_LABEL entry) to re-show it. */
       ROW_MODSEL, ROW_MODEN };

static const char* const ROW_LABEL[ROW_COUNT] = {
    "DISPLAY", "ASPECT", "FPS", "FOV", "SHOW FPS", "VSYNC", "SSAA", "WOBBLE",
    "PIXELATION", "TEXTURES", "DITHER", "LOD", "FOG", "FREECAM",
    "ADVANCED", "EXIT GAME",
};


/* Per row: the id, which is also the OSD coalescing key, the knob registry
 * row the row moves (NULL for the action rows and the mod rows), a hint
 * naming what the row changes, written in the glyph set notify_row
 * documents, and whether a change is saved to openpete.toml. FREECAM is a
 * session toggle: saving it would start the next launch with Spyro frozen
 * under the free camera. The key is
 * NAMED, not indexed, because the hotkeys that front the same settings (L,
 * F, Alt+Enter) raise their toasts from run.c and native_render.c and must
 * land on the SAME toast - a player who flips LOD with the key and then with
 * the row should see one toast repaint, not two stack. Indexed by row through ROW_MODEN, the parked rows included; the
 * surface dump reads it through openpete_menu_surface_row. */
static const struct { const char* id; const char* knob; const char* hint; int persist; }
ROW_SURFACE[ROW_MODEN + 1] = {
    [ROW_DISPLAY] = { "menu.display",    "fullscreen",       "WINDOW OR FULLSCREEN",                    1 },
    [ROW_ASPECT]  = { "menu.aspect",     "aspect",           "WIDTH TO HEIGHT OF THE PICTURE",          1 },
    [ROW_FPS]     = { "menu.fps",        "render_fps",       "PICTURES DRAWN PER SECOND",               1 },
    [ROW_FOV]     = { "menu.fov",        "fov",              "HOW WIDE THE CAMERA SEES",                1 },
    [ROW_SHOWFPS] = { "menu.showfps",    "perf_stats",       "FPS READOUT IN THE CORNER",               1 },
    [ROW_VSYNC]   = { "menu.vsync",      "vsync",            "WAIT FOR THE DISPLAY REFRESH",            1 },
    [ROW_SSAA]    = { "menu.ssaa",       "ssaa",             "RENDER AT A HIGHER RESOLUTION",           1 },
    [ROW_WOBBLE]  = { "menu.wobble",     "vertex_precision", "SNAP CORNERS TO THE PSX PIXEL GRID",      1 },
    [ROW_PIXEL]   = { "menu.pixelation", "raster_quantize",  "DRAW THE SCENE ON THE PSX PIXEL GRID",    1 },
    [ROW_TEXMAP]  = { "menu.texmap",     "texture_mapping",  "AFFINE TEXTURES AS ON THE PSX",           1 },
    [ROW_DITHER]  = { "menu.dither",     "dither",           "PSX 15 BIT COLOUR AND DITHER",            1 },
    [ROW_LOD]     = { "menu.lod",        "lod",              "ORIGINAL OR FULL DETAIL AT ANY DISTANCE", 1 },
    [ROW_FOG]     = { "menu.fog",        "fog",              "DISTANCE FOG",                            1 },
    [ROW_FREECAM] = { "menu.freecam",    "freecam",          "FLY THE CAMERA FREELY",                   0 },
    [ROW_ADVANCED] = { "menu.advanced",  NULL,               "MORE SETTINGS AND CONTROLS",              0 },
    [ROW_QUIT]    = { "menu.quit",       NULL,               "QUIT THE GAME",                           0 },
    [ROW_COUNT]   = { NULL,              NULL,               NULL,                                      0 },
    [ROW_MODSEL]  = { "menu.modsel",     NULL,               "CHOOSE A MOD",                            0 },
    [ROW_MODEN]   = { "menu.moden",      NULL,               "TURN THE MOD ON OR OFF",                  0 },
};

int openpete_menu_surface_row(int i, openpete_menu_surface_t* out) {
    const int row = (i < ROW_COUNT) ? i : i + 1;   /* skip the ROW_COUNT gap */
    if (i < 0 || row > ROW_MODEN) return -1;
    out->id        = ROW_SURFACE[row].id;
    out->knob      = ROW_SURFACE[row].knob;
    out->hint      = ROW_SURFACE[row].hint;
    out->displayed = row < ROW_COUNT;
    return 0;
}

/* EXIT is a CENTERED row: no value column, its label spans the panel centre
 * (openpete_menu_row_centered), and it sits below a gap so it reads as an
 * action separate from the settings above it.
 *
 * It arms on the first press and quits on the second. "Increase value" is the
 * same input the player uses on every other row (Right / d-pad / cross) and the
 * cursor wraps, so a one-press exit sits one stray keypress away from throwing
 * out the progress since their last save. Moving the cursor disarms. */
static int s_quit_armed;

/* Aspect presets (label + width/height). */
static const struct { const char* label; float wh; } s_aspect[] = {
    { "AUTO",  0.0f          },   /* follow the window/fullscreen shape (default) */
    { "4:3",   4.0f  / 3.0f  },
    { "16:10", 16.0f / 10.0f },
    { "16:9",  16.0f / 9.0f  },
    { "21:9",  21.0f / 9.0f  },
    { "32:9",  32.0f / 9.0f  },
};
#define N_ASPECT ((int)(sizeof(s_aspect) / sizeof(s_aspect[0])))

/* FOV ladder: index 0 = the game's projection (setter 0), then vertical
 * degrees in 5° steps over the flag's [30,120] range. */
#define FOV_MIN 30
#define FOV_MAX 120
#define FOV_STEP 5
#define N_FOV (1 + (FOV_MAX - FOV_MIN) / FOV_STEP)
/** @brief Map an FOV ladder index to the vertical field of view in degrees.
 *
 * @param idx The ladder index; index 0 or below selects the game's projection.
 * @return 0 for the game's projection, else FOV_MIN + (idx - 1) * FOV_STEP. */
static float fov_val(int idx) { return idx <= 0 ? 0.0f : (float)(FOV_MIN + (idx - 1) * FOV_STEP); }

/* Ladder stops at OPENPETE_SSAA_MAX - 8X/16X ask for more render-target VRAM
 * than an 8GB card has (see openpete.h). 3X is a genuine rung, not a
 * power-of-two gap-filler: the resolve pass box-filters the exact NxN source
 * block, so an odd factor is as well defined as an even one, and 3X costs
 * ~44% less render-target area than 4X.
 *
 * Factor 1 reads OFF, not "1X": it is not a supersampling amount but the
 * absence of one - no supersampled target, no resolve pass. (The PIXELATION
 * ladder below spells its own no-op step the same way, where 1X IS a
 * setting.) */
static const int   s_ssaa_vals[]   = { 1, 2, 3, 4 };
static const char* s_ssaa_labels[] = { "OFF", "2X", "3X", "4X" };
#define N_SSAA ((int)(sizeof(s_ssaa_vals) / sizeof(s_ssaa_vals[0])))

/* The PS1-look pair is two independent rows, each driving one setter, so the
 * menu offers the whole space the CLI knobs do:
 *   WOBBLE     - the integer-grid vertex snap (--vertex-precision), ON/OFF.
 *   PIXELATION - the scene point-resample (--raster-quantize N), OFF or a
 *                multiplier of the 512x240 PSX raster.
 * Snapping without quantizing leaves the snap's residual hairline cracks
 * visible; quantizing without snapping is a pixelated image with smooth
 * vertex motion. Both are reachable, neither row writes the other's setter.
 *
 * The ladder stops at 16X, the maximum --raster-quantize accepts. */
static const int   s_pix_vals[]   = { 0, 1, 2, 4, 8, 16 };
static const char* s_pix_labels[] = { "OFF", "1X", "2X", "4X", "8X", "16X" };
#define N_PIX ((int)(sizeof(s_pix_vals) / sizeof(s_pix_vals[0])))

/* FPS ladder, built from the monitor refresh at init. The final entry is an
 * UNLIMITED sentinel: a render-fps so high the wall-clock pacer never sleeps on
 * the render deadline, so presenting is GPU-bound (uncapped). It's a normal
 * render-fps value to the pacer - the sim still ticks at its fixed ~29.913 Hz;
 * only presentation is uncapped (subject to vsync). Displayed as "UNLIMITED". */
#define FPS_UNLIMITED 1000000
static int s_fps_vals[16];
static int s_fps_count = 0;

/* --- live selection ------------------------------------------------------ */
static int s_open       = 0;
static int s_cursor     = 0;
static int s_aspect_idx = 0;   /* AUTO     */
static int s_fov_idx    = 0;   /* default  */
static int s_fps_idx    = 0;   /* first ladder entry (30) */
static int s_showfps_idx = 1;  /* ON - mirrors the live vk FPS overlay */
static int s_vsync_idx  = 1;   /* ON       */
static int s_ssaa_idx   = 0;   /* 1X       */
static int s_wobble_idx = 0;   /* OFF - re-synced from the live snap on open */
static int s_pixel_idx  = 0;   /* OFF - re-synced from the live quantize on open */
static int s_texmap_idx = 0;   /* CORRECT (perspective) - re-synced from the live mode on open */
static int s_dither_idx = 0;   /* OFF - re-synced from the live flag on open */
static int s_lod_idx    = 0;   /* ORIGINAL */
static int s_freecam_idx = 0;  /* OFF - re-synced from the live state on open */
static unsigned s_tick  = 0;   /* wobble/shimmer phase (draw step) */

/* --- helpers ------------------------------------------------------------- */
/** @brief Reduce an index into [0, n), wrapping negatives from the top.
 *
 * @param i The index, any sign.
 * @param n The range length.
 * @return i modulo n in [0, n); 0 when n is 0 or less. */
static int wrap_idx(int i, int n) {
    if (n <= 0) return 0;
    i %= n;
    if (i < 0) i += n;
    return i;
}

/** @brief Spell a two-state row's value.
 *
 * @param v The row's state.
 * @return "ON" for non-zero, else "OFF". */
static const char* on_off(int v) { return v ? "ON" : "OFF"; }

/** @brief Find the entry of a ladder nearest to a value, for seeding a row.
 *
 * Ties go to the earlier entry.
 * @param vals The ladder.
 * @param n The ladder's length.
 * @param want The value to match.
 * @return The index of the nearest entry; 0 when n is 0 or less. */
static int nearest_idx(const int* vals, int n, int want) {
    int best = 0, bestd = -1;
    for (int i = 0; i < n; i++) {
        int d = vals[i] - want; if (d < 0) d = -d;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    return best;
}

/** @brief Read the WOBBLE row's index from the chosen vertex precision.
 *
 * Each row re-seeds from its own live setting when the menu opens.
 * @return 1 while integer vertex precision is chosen, else 0. */
static int wobble_idx_from_live(void) {
    return openpete_native_get_vertex_precision() ? 1 : 0;
}

/** @brief Read the PIXELATION row's index from the live raster quantize multiplier.
 *
 * A multiplier off the ladder (a CLI or dev-overlay 3x) shows its nearest
 * entry and is rewritten only when the row is stepped.
 * @return The index of the nearest ladder entry. */
static int pixel_idx_from_live(void) {
    return nearest_idx(s_pix_vals, N_PIX, openpete_native_get_raster_quantize());
}

/** @brief Fill the FPS row's ladder from the monitor refresh rate.
 *
 * The ladder is 30, 60, then every multiple of 60 below the refresh, then the
 * refresh itself when it is above 60 and not already the last entry, then the
 * FPS_UNLIMITED sentinel. A refresh below 60 is treated as 60. Entries stop at
 * the capacity of s_fps_vals, so the sentinel is dropped when the array is
 * full. Sets s_fps_count.
 * @param refresh The monitor refresh in Hz. */
static void build_fps_list(int refresh) {
    const int cap = (int)(sizeof(s_fps_vals) / sizeof(s_fps_vals[0]));
    if (refresh < 60) refresh = 60;   /* sane floor / unknown */
    int n = 0;
    s_fps_vals[n++] = 30;
    s_fps_vals[n++] = 60;
    for (int m = 120; m < refresh && n < cap; m += 60)
        s_fps_vals[n++] = m;
    if (refresh > 60 && n < cap && s_fps_vals[n - 1] != refresh)
        s_fps_vals[n++] = refresh;
    if (n < cap)
        s_fps_vals[n++] = FPS_UNLIMITED;   /* uncapped presenting, after refresh */
    s_fps_count = n;
}

/* Mod menu rows: MOD cycles the loaded-mod selection;
 * MOD ENABLED shows + live-toggles the selected mod (applied at the
 * next tick boundary by the loader). Data comes straight from the
 * loader enumeration - zero mods shows NONE and the rows no-op. */
static int s_modsel_idx = 0;

/* FOG row entries, in the knob registry fog row's order: AUTO follows the
 * LOD mode (openpete_fog_mode_pinned() == OPENPETE_FOG_AUTO), the rest pin
 * openpete_fog_mode_t 0..2. Entry = pinned mode + 1. */
static const char* const s_fog_labels[] = { "AUTO", "ORIGINAL", "PER-PIXEL", "OFF" };
#define N_FOG ((int)(sizeof(s_fog_labels) / sizeof(s_fog_labels[0])))

/** @brief Read the FOG row's entry from the live fog pin.
 *
 * The row keeps no shadow index, so the --fog flag, the dev overlay's Fog row
 * and a config load all show here without a re-seed.
 * @return 0 while the fog form follows the LOD mode, else the pinned mode + 1. */
static int fog_idx_from_live(void) {
    return wrap_idx(openpete_fog_mode_pinned() + 1, N_FOG);
}

/** @brief Spell a row's current value for the glyph builder, the OSD, and the log.
 *
 * DISPLAY and MOD ENABLED read live state; FOV shows DEFAULT or both angles
 * for the live aspect; EXIT is empty. An unknown row reads "?".
 * @param row The row index.
 * @param buf Scratch space for formatted values.
 * @param buflen Size of @p buf in bytes.
 * @return A static string or @p buf. */
static const char* value_str(int row, char* buf, int buflen) {
    switch (row) {
    /* DISPLAY reads the LIVE window state each frame (no shadow index) - like
     * MOD ENABLED - so Alt+Enter / a double-click can never leave it stale. */
    case ROW_DISPLAY:
        return openpete_native_fullscreen_active() ? "FULLSCREEN" : "WINDOWED";
    case ROW_ASPECT: return s_aspect[s_aspect_idx].label;
    case ROW_FOV: {
        /* Both angles for the live aspect: the vertical is the setting, the
         * horizontal follows Hor+. The default reads DEFAULT, the value column
         * being too narrow for the game's own pair as well. */
        if (s_fov_idx <= 0) return "DEFAULT";
        double v = 0.0, h = 0.0;
        openpete_native_fov_angles(&v, &h);
        snprintf(buf, (size_t)buflen, "%.0fV %.0fH", v, h);
        return buf;
    }
    case ROW_FPS: {
        int v = (s_fps_idx < s_fps_count) ? s_fps_vals[s_fps_idx] : 0;
        if (v >= FPS_UNLIMITED) return "UNLIMITED";
        /* tiny itoa (no snprintf dependency needed, but it's available) */
        snprintf(buf, (size_t)buflen, "%d", v);
        return buf;
    }
    case ROW_SHOWFPS:  return on_off(s_showfps_idx);
    case ROW_VSYNC:    return on_off(s_vsync_idx);
    case ROW_SSAA:     return s_ssaa_labels[s_ssaa_idx];
    case ROW_WOBBLE:   return on_off(s_wobble_idx);
    case ROW_PIXEL:    return s_pix_labels[s_pixel_idx];
    case ROW_TEXMAP:   return s_texmap_idx ? "AFFINE" : "CORRECT";
    case ROW_DITHER:   return s_dither_idx ? "15-BIT" : "OFF";
    case ROW_LOD: return s_lod_idx ? "EXTENDED" : "ORIGINAL";
    case ROW_FOG:      return s_fog_labels[fog_idx_from_live()];
    case ROW_FREECAM:  return on_off(s_freecam_idx);
    case ROW_ADVANCED: return "";   /* centered: the label IS the row */
    case ROW_QUIT:     return "";   /* centered: the label IS the row */
    case ROW_MODSEL: {
        const char* id = openpete_mods_id(s_modsel_idx);
        return id ? id : "NONE";
    }
    case ROW_MODEN: {
        const char* id = openpete_mods_id(s_modsel_idx);
        if (!id) return "-";
        return on_off(openpete_mods_is_enabled(id));
    }
    default:           return "?";
    }
}

/** @brief Announce a row's current value on the OSD as "LABEL: VALUE", or the label alone when the value is empty.
 *
 * Coalesced per row through the row's ROW_SURFACE id, so holding Right through
 * the FPS ladder repaints one toast instead of stacking a dozen, and the
 * hotkeys that front the same settings repaint the same toast. Every string
 * is authored inside the glyph set the game's own text mobys cover (0-9 A-Z
 * % / ? + ^ . ' and a faked colon); anything outside it renders as a space.
 * A row outside [0, ROW_COUNT) is ignored.
 * @param row The row index. */
static void notify_row(int row) {
    char vb[16];
    if (row < 0 || row >= ROW_COUNT) return;
    const char* kb = ROW_SURFACE[row].id;
    const char* label = openpete_menu_row_label(row);
    const char* value = value_str(row, vb, sizeof vb);
    if (value && *value) openpete_osd_notify("host", kb, 0, "%s: %s", label, value);
    else                 openpete_osd_notify("host", kb, 0, "%s", label);
}

/** @brief Log the cursor row's label and value under the "menu" channel.
 *
 * @param what The event name printed before the row, such as "opened". */
static void log_state(const char* what) {
    char vb[16];
    OPL_INFO_F("menu", "%s: [%s] = %s", what,
               ROW_LABEL[s_cursor], value_str(s_cursor, vb, sizeof vb));
}

/** @brief Write a value through the setter of the knob registry row a menu row names.
 *
 * A row the build compiles out, a restart-only row, or a value the setter
 * refuses is logged and changes nothing.
 * @param row The menu row index; its ROW_SURFACE knob names the registry row.
 * @param v The value in the registry row's vocabulary (an enum row's index).
 * @return 0 when the setter accepted the value, else -1. */
static int menu_row_write(int row, double v) {
    const char* id = ROW_SURFACE[row].knob;
    knob_desc_t* k = knob_registry_find(id);
    if (!k || k->mut != KNOB_LIVE || !k->set_num || k->set_num(k, v) != 0) {
        OPL_WARN_F("menu", "%s: row %s refused %g", ROW_SURFACE[row].id, id ? id : "-", v);
        return -1;
    }
    return 0;
}

/** @brief Perform the ADVANCED row: close the menu and open the dev overlay scrolled to its top.
 *
 * Toasts NOT AVAILABLE when the build or the run has no overlay. */
static void open_advanced(void) {
    if (!openpete_devui_available()) {
        openpete_osd_notify("host", ROW_SURFACE[ROW_ADVANCED].id, 0, "NOT AVAILABLE");
        return;
    }
    if (s_open) openpete_menu_toggle();
    openpete_devui_open_top();   /* host-control: exempt: UI_NAV, the overlay is navigation */
}

/** @brief Perform the EXIT row once it is armed twice: return to the launcher and request a cooperative shutdown.
 *
 * The host-fiber loop drains and tears down on its next iteration, so the
 * memory card flushes like any other exit. A session picked on the
 * launcher's game page then starts the launcher again
 * (openpete_launcher_return_request). The quit row's fire cancels that
 * return, so this row does not write it. */
static void exit_game(void) {
    if (s_quit_armed < 2) return;
    OPL_INFO_F("menu", "%s", "exit requested");
    (void)openpete_launcher_return_request();   /* host-control: exempt: the launcher return */
    openpete_request_shutdown();                /* host-control: exempt: the launcher return */
}

/** @brief Write a row's selected value through its registry row, or perform the row's action.
 *
 * DISPLAY writes the opposite of the live window state, which the fullscreen
 * row's setter turns into the toggle request Alt+Enter raises; FOG writes the
 * entry @p step away from the live pin; FREECAM writes the engage request,
 * which the camera site applies only when it differs from the live state.
 * The other value rows write the entry their shadow index selects. ADVANCED
 * and EXIT act (open_advanced, exit_game); the mod rows apply nothing here:
 * their adjust step sets the live state directly. A row whose ROW_SURFACE
 * persist flag is set marks its knob for the settings writer, which saves it
 * after the debounce interval, on menu close or at shutdown; DISPLAY is
 * marked too, since the writer reads the window state the request has
 * settled by then.
 * @param row The row index.
 * @param step The adjust direction, +1 or -1; FOG reads it. */
static void apply_row(int row, int step) {
    double v;
    switch (row) {
    case ROW_DISPLAY:  v = openpete_native_fullscreen_active() ? 0.0 : 1.0;   break;
    case ROW_ASPECT:   v = s_aspect_idx;                                     break;
    case ROW_FOV:      v = fov_val(s_fov_idx);                               break;
    case ROW_FPS:      v = s_fps_vals[s_fps_idx];                            break;
    /* SHOW FPS is the two-state view of the stats level: on restores the
     * last non-off level, so a toggle never demotes a detailed readout. */
    case ROW_SHOWFPS:
        v = s_showfps_idx ? openpete_native_perf_stats_last() : OPENPETE_PERF_OFF;
        break;
    case ROW_VSYNC:    v = s_vsync_idx;                                      break;
    case ROW_SSAA:     v = s_ssaa_idx;    /* the registry ladder is s_ssaa_vals */ break;
    case ROW_WOBBLE:   v = s_wobble_idx;                                     break;
    case ROW_PIXEL:    v = s_pix_vals[s_pixel_idx];                          break;
    /* Queued: the renderer re-bakes its pipelines at a later present (a
     * one-time device-idle hitch); the value string reads the local index. */
    case ROW_TEXMAP:   v = s_texmap_idx;                                     break;
    case ROW_DITHER:   v = s_dither_idx;                                     break;
    case ROW_LOD:      v = s_lod_idx;                                        break;
    /* Entry 0 unpins (OPENPETE_FOG_AUTO): the fog form follows LOD again. */
    case ROW_FOG:      v = wrap_idx(fog_idx_from_live() + step, N_FOG);      break;
    case ROW_FREECAM:  v = s_freecam_idx;                                    break;
    case ROW_ADVANCED: open_advanced(); return;
    case ROW_QUIT:     exit_game();     return;
    default:           return;
    }
    (void)menu_row_write(row, v);
    if (ROW_SURFACE[row].persist)
        openpete_settings_save_mark(ROW_SURFACE[row].knob);
}

/* --- public -------------------------------------------------------------- */
void openpete_menu_init(const openpete_options_t* opts, int refresh_hz) {
    build_fps_list(refresh_hz);

    if (!opts) return;

    /* Aspect: nearest FIXED preset to an explicit launch --aspect (the i=1
     * start skips the AUTO entry - its 0.0 sentinel isn't a ratio); unset /
     * `--aspect auto` -> AUTO (follow the window, the default). */
    if (opts->aspect_w_over_h > 0.0f) {
        int best = 1; float bestd = -1.0f;
        for (int i = 1; i < N_ASPECT; i++) {
            float d = s_aspect[i].wh - opts->aspect_w_over_h;
            if (d < 0.0f) d = -d;
            if (bestd < 0.0f || d < bestd) { bestd = d; best = i; }
        }
        s_aspect_idx = best;
    } else {
        s_aspect_idx = 0;   /* AUTO */
    }
    /* FOV: nearest ladder step to the launch --fov (0 = the game's projection). */
    if (opts->fov_deg > 0.0f) {
        int i = (int)((opts->fov_deg - (float)FOV_MIN) / (float)FOV_STEP + 0.5f) + 1;
        s_fov_idx = i < 1 ? 1 : (i >= N_FOV ? N_FOV - 1 : i);
    } else {
        s_fov_idx = 0;
    }

    /* FPS: exact/nearest in the ladder, seeded from the LIVE pacer target -
     * the host pacer resolves the --render-fps default (display refresh when
     * unset, windowed) and publishes it before this init, so opts->render_fps may
     * still hold the -1 sentinel. 0 = legacy match-sim -> the 30 entry. */
    {
        int live_fps = openpete_get_render_fps();
        s_fps_idx = nearest_idx(s_fps_vals, s_fps_count,
                                live_fps > 0 ? live_fps : 30);
    }

    /* VSync: -1 (unset) defaults to ON (FIFO); 0/1 as given. */
    s_vsync_idx = (opts->vsync == 0) ? 0 : 1;

    /* SHOW FPS is the two-state view of the three-level stats overlay: ON
     * whenever anything is drawn. The M overlay's Performance stats row picks
     * WHICH level, and toggling this row off and on restores it. */
    s_showfps_idx = (opts->perf_stats != OPENPETE_PERF_OFF) ? 1 : 0;

    /* SSAA: nearest supported factor. */
    s_ssaa_idx = nearest_idx(s_ssaa_vals, N_SSAA, opts->ssaa > 0 ? opts->ssaa : 1);

    /* WOBBLE / PIXELATION: the native renderer has already resolved
     * --vertex-precision and the --raster-quantize auto default (which pairs
     * with the snap when unset) - read each live value. */
    s_wobble_idx = wobble_idx_from_live();
    s_pixel_idx  = pixel_idx_from_live();

    /* TEXTURE: the renderer resolved --texture-mapping at its init. */
    s_texmap_idx = openpete_native_get_texture_mapping();

    /* DITHER (PS1 15-bit colour). */
    s_dither_idx = opts->dither ? 1 : 0;

    /* LOD (draw distance / detail policy). */
    s_lod_idx = (opts->lod == OPENPETE_LOD_EXTENDED) ? 1 : 0;

    s_cursor = 0;
}

/* --- mouse consumer ------------------------------------------------------ */

/* Markers: the press and wheel totals this consumer last consumed. Snapped
 * to the current totals on the open edge and on every poll where the dev
 * overlay owns the mouse, so a press made before the menu opened or on the
 * fill-tag inspector's panel is never delivered as a row click. */
static uint64_t s_m_press[OPENPETE_MOUSE_BUTTONS];
static int64_t  s_m_wheel;
static float    s_m_px = -1.0f, s_m_py = -1.0f;

/** @brief Set the press, wheel, and position markers to the current mouse state.
 *
 * Presses and wheel motion that happened before the call are then not counted
 * by the next openpete_menu_mouse_poll, and a pointer at rest is not a move. */
static void menu_mouse_snap(void) {
    for (int b = 0; b < OPENPETE_MOUSE_BUTTONS; b++)
        s_m_press[b] = openpete_host_mouse_press_total(b);
    s_m_wheel = openpete_host_mouse_wheel_total();
    /* The position too: a pointer resting on a row when the menu opens or
     * the overlay closes is not a hover until it moves. */
    openpete_mouse_state_t st;
    openpete_host_input_mouse(&st);
    s_m_px = st.px; s_m_py = st.py;
}

/** @brief Convert a wheel total to whole notches, truncating toward zero.
 *
 * A notch is OPENPETE_MOUSE_WHEEL_UNIT wheel units.
 * @param total The signed wheel total.
 * @return The notch count, negative for a negative total. */
static int64_t wheel_notches(int64_t total) {
    const int64_t u = OPENPETE_MOUSE_WHEEL_UNIT;
    return total >= 0 ? total / u : -((-total + u - 1) / u);
}

/** @brief Report whether a row performs an action rather than setting a value.
 *
 * The mouse poll gives these rows a left click only; a right click or a
 * wheel notch on them is ignored.
 * @param row The row index.
 * @return Non-zero for DISPLAY, ADVANCED and EXIT. */
static int row_action_only(int row) {
    return row == ROW_DISPLAY || row == ROW_ADVANCED || row == ROW_QUIT;
}

/* The close glyph's drawn ink, in overlay units, as the text builder emitted
 * it. Zero width means no build has run yet. */
static float s_close_rect[4];

/* The row cap height the text builder last drew at. Zero means no build yet. */
static float s_row_cap_drawn;

/* Pointer inside the close glyph's ink, as of the last poll. */
static int s_close_hover;

int openpete_menu_close_hovered(void) { return s_open && s_close_hover; }

void openpete_menu_note_row_cap(float cap_h) {
    s_row_cap_drawn = cap_h > 0.0f ? cap_h : 0.0f;
}

void openpete_menu_note_close_rect(float x0, float y0, float x1, float y1) {
    if (x0 != s_close_rect[0] || y0 != s_close_rect[1] ||
        x1 != s_close_rect[2] || y1 != s_close_rect[3])
        openpete_host_mouse_probe_log("menu", "close rect [%g,%g]x[%g,%g]",
                                      x0, x1, y0, y1);
    s_close_rect[0] = x0; s_close_rect[1] = y0;
    s_close_rect[2] = x1; s_close_rect[3] = y1;
}

/** @brief Test whether an overlay-space point lies on the close glyph.
 *
 * Tests the ink rect the text builder published; before its first build the
 * rect has zero width and the layout's close square stands in. Edges are
 * inclusive.
 * @param L The current menu layout.
 * @param x Point x in units of window height.
 * @param y Point y in units of window height.
 * @return Non-zero when the point is on the glyph. */
static int menu_in_close(const openpete_menu_layout_t* L, float x, float y) {
    if (s_close_rect[2] > s_close_rect[0])
        return x >= s_close_rect[0] && x <= s_close_rect[2] &&
               y >= s_close_rect[1] && y <= s_close_rect[3];
    return x >= L->close_x && x <= L->close_x + L->close_cap_h &&
           y >= L->close_y && y <= L->close_y + L->close_cap_h;
}

void openpete_menu_mouse_poll(void) {
    if (!s_open) return;
    if (openpete_host_mouse_owned() == OPENPETE_MOUSE_OWNED_DEVUI) {
        s_close_hover = 0;
        menu_mouse_snap();
        return;
    }
    openpete_mouse_state_t st;
    openpete_host_input_mouse(&st);

    uint64_t press[OPENPETE_MOUSE_BUTTONS];
    for (int b = 0; b < OPENPETE_MOUSE_BUTTONS; b++)
        press[b] = openpete_host_mouse_press_total(b);
    const int64_t wheel = openpete_host_mouse_wheel_total();
    const int64_t left  = (int64_t)(press[OPENPETE_MOUSE_BTN_LEFT]  - s_m_press[OPENPETE_MOUSE_BTN_LEFT]);
    const int64_t right = (int64_t)(press[OPENPETE_MOUSE_BTN_RIGHT] - s_m_press[OPENPETE_MOUSE_BTN_RIGHT]);
    const int64_t notch = wheel_notches(wheel) - wheel_notches(s_m_wheel);
    memcpy(s_m_press, press, sizeof press);
    s_m_wheel = wheel;

    if (st.win_h <= 0.0f) { s_close_hover = 0; return; }
    /* Overlay space: units of window height, origin top-left. The divisor is
     * the point height, so a display scale cancels. */
    const float x = st.px / st.win_h, y = st.py / st.win_h;
    openpete_menu_layout_t L;
    openpete_menu_get_layout(&L);

    const int in_panel = x >= L.panel_x0 && x <= L.panel_x1 &&
                         y >= L.panel_y0 && y <= L.panel_y1;
    int hit = -1;
    if (in_panel)
        for (int i = 0; i < L.row_count; i++)
            if (fabsf(y - L.row_y[i]) <= L.row_h * 0.5f) { hit = i; break; }
    const int in_close = menu_in_close(&L, x, y);
    s_close_hover = in_close;

    /* Hover: only a pointer move can take the cursor, and only to a row that
     * differs, so a resting pointer never overrides keyboard nav and a move
     * within the row never disarms EXIT. */
    if (st.px != s_m_px || st.py != s_m_py) {
        s_m_px = st.px; s_m_py = st.py;
        if (hit >= 0 && hit != s_cursor) {
            openpete_menu_set_cursor(hit);
            openpete_host_mouse_probe_log("menu", "hover row=%d (%g,%g)", hit, x, y);
        }
    }

    for (int64_t n = 0; n < left; n++) {
        if (in_close) {
            openpete_host_mouse_probe_log("menu", "click X -> close");
            openpete_menu_toggle();
            return;
        }
        if (hit < 0) {
            openpete_host_mouse_probe_log("menu", "click outside rows (%g,%g) ignored", x, y);
            continue;
        }
        if (hit != s_cursor) openpete_menu_set_cursor(hit);
        if (openpete_menu_row_centered(hit)) {
            openpete_host_mouse_probe_log("menu", "click row=%d -> activate", hit);
            openpete_menu_activate();
        } else {
            openpete_host_mouse_probe_log("menu", "click row=%d -> adjust +1", hit);
            openpete_menu_adjust(+1);
        }
    }
    for (int64_t n = 0; n < right; n++) {
        if (hit < 0 || row_action_only(hit)) {
            openpete_host_mouse_probe_log("menu", "right click row=%d inert", hit);
            continue;
        }
        if (hit != s_cursor) openpete_menu_set_cursor(hit);
        openpete_host_mouse_probe_log("menu", "right click row=%d -> adjust -1", hit);
        openpete_menu_adjust(-1);
    }
    if (notch) {
        if (hit < 0 || row_action_only(hit)) {
            openpete_host_mouse_probe_log("menu", "wheel %lld row=%d inert",
                                          (long long)notch, hit);
        } else {
            if (hit != s_cursor) openpete_menu_set_cursor(hit);
            for (int64_t n = notch > 0 ? notch : -notch; n > 0; n--) {
                openpete_host_mouse_probe_log("menu", "wheel row=%d -> adjust %+d",
                                              hit, notch > 0 ? 1 : -1);
                openpete_menu_adjust(notch > 0 ? +1 : -1);
            }
        }
    }
}

int openpete_menu_toggle(void) {
    s_open = !s_open;
    if (s_open) {
        /* Re-sync the rows a hotkey can change while the menu is closed, so the
         * menu never shows a stale value: SHOW FPS (--perf-stats / the M
         * overlay's Performance stats row), LOD (the "L" toggle flips
         * the wanted LOD mode), and FREECAM (the "F" key / left-stick
         * click). FOG reads the live pin and needs no re-sync. */
        s_showfps_idx = openpete_native_fps_overlay_enabled();
        s_lod_idx     = openpete_no_lod_wanted() ? 1 : 0;
        s_freecam_idx = openpete_freecam_active() ? 1 : 0;
        s_wobble_idx  = wobble_idx_from_live();   /* M-overlay knob moves the snap */
        s_pixel_idx   = pixel_idx_from_live();    /* M-overlay knob moves the quantize */
        s_texmap_idx  = openpete_native_get_texture_mapping();   /* M-overlay row moves it */
        s_dither_idx  = openpete_native_get_dither();            /* M-overlay row moves it */
        s_quit_armed  = 0;   /* never re-open onto a primed EXIT */
        menu_mouse_snap();   /* a press before this open is not a click */
        log_state("opened");
    } else {
        OPL_INFO_F("menu", "%s", "closed");
        openpete_settings_save_flush("menu-close");
    }
    return s_open;
}

int openpete_menu_is_open(void) { return s_open; }

int openpete_menu_cursor(void) { return s_cursor; }

void openpete_menu_get_layout(openpete_menu_layout_t* out) {
    if (!out) return;
    /* Full-height, flush-left sidebar. Overlay space: x = window-height
     * fractions (0 = left window edge), y = [0,1] = the FULL window height
     * (window-relative, not the letterboxed game viewport). */
    out->panel_x0 = 0.0f;  out->panel_x1 = 0.62f;
    out->panel_y0 = 0.0f;  out->panel_y1 = 1.0f;
    /* Fill a y for EVERY row the draw loop will visit (row_count), capped only
     * by the row_y array's real capacity. */
    const int cap = (int)(sizeof(out->row_y) / sizeof(out->row_y[0]));
    const int N = (ROW_COUNT < cap) ? ROW_COUNT : cap;
    out->row_count = N;
    /* Styled after the game's pause menu: a compact block (title + rows) with
     * tight line spacing, TOP-ALIGNED in the panel (a small margin from the top).
     * The title is a bold header, noticeably larger than the rows. */
    const float title_cap = 0.080f, row_cap = 0.0296f;   /* header > rows */
    /* row_cap is the ceiling the glyph builder shrinks from, to fit the widest
     * value ("FULLSCREEN") in the value column. It sits below the width the
     * column can take, so the clamp is a safety net for a future wider value
     * rather than what sizes the rows: while it is slack the rows draw at
     * row_cap exactly. Spacing is the same multiple of the cap the
     * pause-menu-tuned pitch was, so the list shortens with the glyphs instead
     * of thinning them. */
    const float spacing   = 0.0442f;   /* row-to-row, 1.49 x row_cap */
    const float top_pad   = 0.055f;   /* panel top -> title top, panel bottom -> EXIT bottom */
    const float title_gap = 0.070f;   /* title top -> first row (larger header needs more) */
    out->title_cap_h = title_cap;
    out->title_y     = out->panel_y0 + top_pad + title_cap * 0.5f;
    out->row_cap_h   = row_cap;
    out->row_h       = spacing;        /* (selection bar removed; kept ≈ line pitch) */
    const float first = out->panel_y0 + top_pad + title_cap + title_gap + row_cap * 0.5f;
    for (int i = 0; i < N; i++)
        out->row_y[i] = first + spacing * (float)i;
    /* EXIT acts, it doesn't set: it leaves the settings block and anchors to
     * the panel's bottom edge at the same inset the title keeps from the top,
     * so the block reads as framed rather than trailing off. Its ink bottom,
     * not its nominal box, is what lands on the inset, hence the drawn cap.
     * The draw and the hit test both read this y. */
    if (ROW_QUIT < N) {
        const float drawn = s_row_cap_drawn > 0.0f ? s_row_cap_drawn : row_cap;
        out->row_y[ROW_QUIT] = out->panel_y1 - top_pad - drawn * 0.5f;
    }
    /* Hint line: centred in the gap between the row above EXIT and EXIT's
     * ink, two row pitches tall, so it never sits inside a row's hit band. */
    out->hint_cap_h = 0.020f;
    out->hint_y = out->row_y[N - 1];
    if (ROW_QUIT < N && ROW_QUIT > 0) {
        const float drawn = s_row_cap_drawn > 0.0f ? s_row_cap_drawn : row_cap;
        const float above = out->row_y[ROW_QUIT - 1] + drawn * 0.5f;
        const float below = out->row_y[ROW_QUIT] - drawn * 0.5f;
        out->hint_y = 0.5f * (above + below);
    }
    /* Split point (labels right-align to label_x, values left-align at value_x).
     * Kept left of centre - reduces the left padding and gives the widest row's
     * value ("EXTENDED") room before the right border. */
    out->label_x = 0.300f;   /* label RIGHT-edge column */
    out->value_x = 0.335f;   /* value LEFT column        */
    /* Close glyph: a square in the panel's top-right corner, above the
     * title's line and clear of its right end, so it reads as a corner
     * button rather than a superscript on the title. */
    const float close_pad = 0.020f, close_cap = 0.050f;
    out->close_cap_h = close_cap;
    out->close_x     = out->panel_x1 - close_pad - close_cap;
    out->close_y     = out->panel_y0 + close_pad;
}

const char* openpete_menu_title(void) { return "SETTINGS"; }

const char* openpete_menu_row_label(int row) {
    if (row == ROW_QUIT && s_quit_armed) return "REALLY EXIT?";
    return (row >= 0 && row < ROW_COUNT) ? ROW_LABEL[row] : "";
}

const char* openpete_menu_row_hint(int row) {
    if (row < 0 || row >= ROW_COUNT || !ROW_SURFACE[row].hint) return "";
    return ROW_SURFACE[row].hint;
}

int openpete_menu_row_centered(int row) {
    return row == ROW_ADVANCED || row == ROW_QUIT;
}

const char* openpete_menu_row_value(int row, char* buf, int buflen) {
    if (row < 0 || row >= ROW_COUNT) return "";
    return value_str(row, buf, buflen);
}

unsigned openpete_menu_wobble_phase(void) { return s_tick; }

void openpete_menu_nav(int dy) {
    if (!s_open) return;
    s_cursor = wrap_idx(s_cursor + (dy > 0 ? 1 : -1), ROW_COUNT);
    s_quit_armed = 0;   /* leaving the row (or arriving) always re-arms safe */
    log_state("cursor");
}

void openpete_menu_set_cursor(int row) {
    if (!s_open || row < 0 || row >= ROW_COUNT || row == s_cursor) return;
    s_cursor = row;
    s_quit_armed = 0;
    log_state("cursor");
}

void openpete_menu_adjust(int dx) {
    if (!s_open) return;
    const int step = (dx > 0 ? 1 : -1);
    switch (s_cursor) {
    case ROW_DISPLAY:
        /* Two-state cycle: either direction flips windowed <-> fullscreen
         * (apply_row); the SDL call runs on the window-owning thread, not
         * here. The value string reads the live state back.
         *
         * The toast notify_row raises below therefore shows the state BEFORE
         * the flip - it is a request, not a change. It self-corrects: the
         * toggle announces on the same key when it actually runs, a present
         * or two later, and repaints this toast with the truth. */
        break;
    case ROW_ASPECT:   s_aspect_idx = wrap_idx(s_aspect_idx + step, N_ASPECT);   break;
    case ROW_FOV:      s_fov_idx    = wrap_idx(s_fov_idx    + step, N_FOV);       break;
    case ROW_FPS:      s_fps_idx    = wrap_idx(s_fps_idx    + step, s_fps_count); break;
    case ROW_SHOWFPS:  s_showfps_idx = wrap_idx(s_showfps_idx + step, 2);         break;
    case ROW_VSYNC:    s_vsync_idx  = wrap_idx(s_vsync_idx  + step, 2);           break;
    case ROW_SSAA:     s_ssaa_idx   = wrap_idx(s_ssaa_idx   + step, N_SSAA);      break;
    case ROW_WOBBLE:   s_wobble_idx = wrap_idx(s_wobble_idx + step, 2);           break;
    case ROW_PIXEL:    s_pixel_idx  = wrap_idx(s_pixel_idx  + step, N_PIX);      break;
    case ROW_TEXMAP:   s_texmap_idx = wrap_idx(s_texmap_idx + step, 2);           break;
    case ROW_DITHER:   s_dither_idx = wrap_idx(s_dither_idx + step, 2);           break;
    case ROW_LOD:      s_lod_idx    = wrap_idx(s_lod_idx    + step, 2);           break;
    case ROW_FOG:      break;   /* apply_row steps from the live pin */
    case ROW_FREECAM:  s_freecam_idx = wrap_idx(s_freecam_idx + step, 2);         break;
    case ROW_MODSEL:
        if (openpete_mods_count() > 0)
            s_modsel_idx = wrap_idx(s_modsel_idx + step,
                                    openpete_mods_count());
        break;
    case ROW_MODEN: {
        /* Queue the flip of the SELECTED mod; the loader applies it at
         * the next tick boundary and records the state the value_str
         * reads back - no local shadow index to go stale. */
        const char* id = openpete_mods_id(s_modsel_idx);
        if (id)
            (void)openpete_mods_set_mod_enabled(   /* host-control: exempt: the mod set */
                id, !openpete_mods_is_enabled(id));
        break;
    }
    /* ADVANCED acts on any step and announces nothing: the menu closes
     * under it and the overlay opens. */
    case ROW_ADVANCED:
        log_state("set");
        apply_row(ROW_ADVANCED, step);
        return;
    /* Action rows have no value to cycle - left/right are inert on them.
     * openpete_menu_activate() is the only way in, so a stray Right on the
     * way down the list does not arm EXIT. */
    case ROW_QUIT: return;
    default: break;
    }
    apply_row(s_cursor, step);
    log_state("set");
    notify_row(s_cursor);
}

void openpete_menu_activate(void) {
    if (!s_open) return;
    if (!openpete_menu_row_centered(s_cursor)) return;   /* value rows: no-op */
    if (s_cursor == ROW_QUIT && s_quit_armed < 2) s_quit_armed++;   /* first press arms, second fires */
    apply_row(s_cursor, +1);
    log_state("activate");
    /* EXIT raises no toast: arming already changes the row's own label to
     * REALLY EXIT?. */
}

void openpete_menu_tick(void) { s_tick++; }

void openpete_menu_draw(void) {
    /* Intentionally empty: the native renderer draws the panel and the
     * text-moby builder draws the glyph rows. */
}
