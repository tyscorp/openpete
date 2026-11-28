/* knob_registry_test - the M-overlay knob table's non-obvious mechanics:
 * module-contributed rows (openpete_knob_add) landing inside their section,
 * capacity refusal, one row per id, flag and env var (a contributed row
 * reusing one is refused), and KNOB_ACTION rows (a button, no value)
 * surviving boot stamping. The overlay itself needs a window, so these are the parts that
 * would otherwise only be testable by eye.
 *
 * knob_registry.c is compiled straight in, with the host/renderer setters it
 * calls stubbed below - linking openpete_core would drag SDL into a console
 * test (same reason as psx_region_test). */

#include "openpete.h"
#include "openpete_knobs.h"
#include "openpete_config.h"
#include "openpete_fillpick.h"
#include "openpete_mods.h"
#include "../../core/config/config_schema.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stubs for the live-value channels knob_registry.c writes through ---- */

int   openpete_no_lod    = 0;
float openpete_fog_scale = 1.0f;

int    openpete_fog_mode_pinned(void)          { return -1; }
void   openpete_fog_mode_set(int mode)         { (void)mode; }
void   openpete_fog_scale_set(float v)         { (void)v; }
void   openpete_no_lod_set(int on)             { (void)on; }
int    openpete_no_lod_wanted(void)            { return openpete_no_lod; }
float  openpete_fog_scale_wanted(void)         { return openpete_fog_scale; }
int    openpete_freecam_active(void)           { return 0; }
void   openpete_freecam_toggle(void)           { }
unsigned openpete_state_active_slot(void)        { return 0u; }
void   openpete_state_set_active_slot(unsigned s) { (void)s; }
void   openpete_state_request(int load)        { (void)load; }
int    openpete_state_active_slot_full(void)  { return 0; }
int    openpete_camlock_active(void)           { return 0; }
void   openpete_camlock_toggle(void)           { }
int    openpete_fillpick_active(void)          { return 0; }
void   openpete_fillpick_set_active(int on)    { (void)on; }
int    openpete_fillpick_preview(void)         { return 0; }
void   openpete_fillpick_set_preview(int on)   { (void)on; }
int    openpete_get_mute(void)                 { return 0; }
void   openpete_set_mute(int on)               { (void)on; }
static int s_mismatch_toast;
void   openpete_memcard_set_mismatch_toast(int on) { s_mismatch_toast = on ? 1 : 0; }
int    openpete_memcard_mismatch_toast(void)   { return s_mismatch_toast; }
static int s_mouse_enabled = 1;
int    openpete_host_mouse_enabled(void)       { return s_mouse_enabled; }
void   openpete_host_mouse_set_enabled(int on) { s_mouse_enabled = on ? 1 : 0; }
static int s_pad_unfocused;
int    openpete_host_input_pad_unfocused(void)        { return s_pad_unfocused; }
void   openpete_host_input_set_pad_unfocused(int on)  { s_pad_unfocused = on ? 1 : 0; }
static int s_record_supported = 1;
int    openpete_record_supported(void)         { return s_record_supported; }
int    openpete_get_sfx_volume(void)           { return -1; }
void   openpete_set_sfx_volume(int v)          { (void)v; }
int    openpete_get_music_volume(void)         { return -1; }
void   openpete_set_music_volume(int v)        { (void)v; }
int    openpete_get_vibration(void)            { return -1; }
void   openpete_set_vibration(int v)           { (void)v; }
float  openpete_freecam_mouse_sensitivity(void)     { return 1.0f; }
void   openpete_freecam_set_mouse_sensitivity(float s) { (void)s; }
int    openpete_get_render_fps(void)           { return 60; }
void   openpete_set_render_fps(int fps)        { (void)fps; }
static int s_cadence_stub;
int    openpete_get_cadence(void)              { return s_cadence_stub; }
void   openpete_set_cadence(int c)             { s_cadence_stub = c; }
double openpete_get_tick_scale(void)           { return 1.0; }
void   openpete_set_tick_scale(double s)       { (void)s; }
int    openpete_runahead_get(void)             { return 0; }
void   openpete_runahead_set(int n)            { (void)n; }
void   openpete_rewind_window_set(int w)       { (void)w; }
void   openpete_sim_pause_request(int on)      { (void)on; }
int    openpete_interp_toggle_on(void)         { return 1; }
void   openpete_interp_toggle_set(int on)      { (void)on; }
void   openpete_sim_step_request(void)         { }
int    openpete_sim_paused(void)               { return 0; }
int    openpete_native_fullscreen_active(void) { return 0; }
void   openpete_native_toggle_fullscreen(void) { }
void   openpete_native_request_fullscreen_toggle(void) { }

/* The renderer's live display state. Display rows read it back rather than
 * shadowing their value, because the O menu drives these setters directly
 * (never the rows'), so the stub keeps real state for those rows to find. */
static bool  s_native_up     = false;
static float s_native_aspect = 0.0f;   /* AUTO */
static int   s_native_ssaa   = 1;
static int   s_native_vsync  = -1;     /* unset */
static int   s_native_showfps = 1;
static int   s_native_vsnap  = 0;
static int   s_native_rquant = 0;
static int   s_native_dither = 0;
static int   s_native_texmap = 0;

bool   openpete_native_render_enabled(void)    { return s_native_up; }
void   openpete_native_set_aspect(float a)     { s_native_aspect = (a > 0.0f) ? a : 0.0f; }
float  openpete_native_get_aspect(void)        { return s_native_aspect; }
void   openpete_native_set_ssaa(int n)         { s_native_ssaa = n; }
int    openpete_native_get_ssaa(void)          { return s_native_ssaa; }
static int s_native_resolution;
void   openpete_native_set_resolution(int packed) { s_native_resolution = packed; }
int    openpete_native_get_resolution(void)    { return s_native_resolution; }
void   openpete_native_set_vsync(int on)       { s_native_vsync = on ? 1 : 0; }
int    openpete_native_get_vsync(void)         { return s_native_vsync; }
void   openpete_native_set_perf_stats(int level) { s_native_showfps = level; }
static int s_native_perf_anchor = 0;
void   openpete_native_set_perf_anchor(int a) { s_native_perf_anchor = a; }
int    openpete_native_perf_anchor(void) { return s_native_perf_anchor; }
static int      s_osd_on = 1, s_osd_anchor = 0;
static uint32_t s_osd_timeout_ms = 3000;
void     openpete_osd_set_enabled(int on) { s_osd_on = on ? 1 : 0; }
int      openpete_osd_enabled(void) { return s_osd_on; }
void     openpete_osd_set_anchor(int a) { s_osd_anchor = a; }
int      openpete_osd_anchor(void) { return s_osd_anchor; }
void     openpete_osd_set_default_timeout_ms(uint32_t ms) { s_osd_timeout_ms = ms; }
uint32_t openpete_osd_default_timeout_ms(void) { return s_osd_timeout_ms; }
int    openpete_native_perf_stats_level(void)    { return s_native_showfps; }
void   openpete_native_set_fps_overlay(int on)   { s_native_showfps = on ? OPENPETE_PERF_FPS
                                                                         : OPENPETE_PERF_OFF; }
int    openpete_native_fps_overlay_enabled(void) { return s_native_showfps != OPENPETE_PERF_OFF; }
void   openpete_native_set_fps_in_record(int on) { (void)on; }
void   openpete_native_set_vertex_precision(int m) { s_native_vsnap = m ? 1 : 0; }
int    openpete_native_get_vertex_precision(void)  { return s_native_vsnap; }
void   openpete_native_set_raster_quantize(int n)  { s_native_rquant = n; }
int    openpete_native_get_raster_quantize(void)   { return s_native_rquant; }
int    openpete_workpool_threads(void)             { return 1; }
int    openpete_tercache_ready(void)               { return 0; }
int    openpete_disc_cache_active(void)            { return 0; }
int    openpete_cd_load_delay = 0;
int    openpete_native_get_dither(void)            { return s_native_dither; }
void   openpete_native_set_dither(int on)          { s_native_dither = on ? 1 : 0; }
static int s_native_hide_player;
int    openpete_native_get_hide_player(void)       { return s_native_hide_player; }
void   openpete_native_set_hide_player(int on)     { s_native_hide_player = on ? 1 : 0; }
static int s_native_hide_hud;
int    openpete_native_get_hide_hud(void)          { return s_native_hide_hud; }
void   openpete_native_set_hide_hud(int on)        { s_native_hide_hud = on ? 1 : 0; }
static int s_native_tervis;
void   openpete_native_set_sector_visibility(int mode) { s_native_tervis = mode; }
int    openpete_native_get_sector_visibility(void)     { return s_native_tervis; }
static int s_native_drain_legacy;
void   openpete_native_set_terrain_drain_legacy(int legacy) { s_native_drain_legacy = legacy ? 1 : 0; }
int    openpete_native_terrain_drain_legacy(void)           { return s_native_drain_legacy; }
static float s_native_fov;
void   openpete_native_set_fov(float deg)      { s_native_fov = deg; }
float  openpete_native_fov_deg(void)           { return s_native_fov; }
static int s_native_version_osd = 1;
void   openpete_native_set_version_osd(int on) { s_native_version_osd = on ? 1 : 0; }
int    openpete_native_version_osd(void)       { return s_native_version_osd; }
int    openpete_native_get_texture_mapping(void)   { return s_native_texmap; }
void   openpete_native_set_texture_mapping(int m)  { s_native_texmap = m ? 1 : 0; }
int    openpete_native_get_notex(void)             { return 0; }
void   openpete_native_set_notex(int on)           { (void)on; }
int    openpete_native_get_wireframe(void)         { return 0; }
void   openpete_native_set_wireframe(int on)       { (void)on; }
int    openpete_log_get_format(void)           { return 0; }
void   openpete_log_set_format(int f)          { (void)f; }
const char *openpete_log_get_spec(void)        { return ""; }
const char *openpete_log_file_path(void)       { return ""; }
void   openpete_log_set_spec(const char *s)    { (void)s; }
void   openpete_logf(int lvl, const char *sub, const char *fmt, ...) {
    (void)lvl; (void)sub; (void)fmt;
}

/* The host-action, fill-tag verdict and custom-level rows' channels. */
void   openpete_screenshot_request(void)       { }
void   openpete_bugreport_request(void)        { }
void   openpete_request_shutdown(void)         { }
void   openpete_launcher_return_cancel(void)   { }
void   openpete_quit(void)                     { }
void   openpete_freecam_request_teleport(void) { }
void   openpete_freecam_request_active(bool on) { (void)on; }
int    openpete_freecam_pending_active(void)   { return -1; }
int    openpete_freecam_speed_index(void)      { return 2; }
void   openpete_freecam_request_speed(int s)   { (void)s; }
void   openpete_camlock_request(bool on)       { (void)on; }
void   openpete_turbo_hold(int on)             { (void)on; }
int    openpete_turbo_held(void)               { return 0; }
int    openpete_rewind_hold_request(int t)     { (void)t; return 0; }
int    openpete_rewind_hold_value(void)        { return 0; }
void   openpete_fillpick_mark(int verdict)     { (void)verdict; }
void   openpete_fillpick_undo(void)            { }
int    openpete_fillpick_get(openpete_fillpick_hit *out) { (void)out; return 0; }
int    openpete_fillpick_count(void)           { return 0; }
int    openpete_mods_custom_count(void)        { return 0; }
int    openpete_mods_custom_info(int i, openpete_mods_custom_info_t *out) { (void)i; (void)out; return 0; }
int    openpete_mods_custom_warp(int i)        { (void)i; return 0; }
void   openpete_mods_custom_set_mode(int i, int r) { (void)i; (void)r; }
int    openpete_mods_is_enabled(const char *id) { (void)id; return 0; }

/* ---- the action row under test ---- */

static int    s_fired  = 0;
static double s_ready  = 1.0;

static double act_ready(const knob_desc_t *k) { (void)k; return s_ready; }
static int    act_fire(knob_desc_t *k, double v) { (void)k; (void)v; s_fired++; return 0; }
static void   act_status(const knob_desc_t *k, char *buf, size_t cap) {
    (void)k; snprintf(buf, cap, s_fired ? "applied" : "");
}

static int fail(const char *msg) { printf("FAIL: %s\n", msg); return 1; }

static const knob_desc_t *find_row(const char *id, size_t *idx_out) {
    size_t n;
    const knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(ks[i].id, id)) { if (idx_out) *idx_out = i; return &ks[i]; }
    return NULL;
}

/* Every section must occupy ONE contiguous run - the overlay emits a header
 * per run, so a split section would render its name twice. */
static int sections_contiguous(void) {
    size_t n;
    const knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (strcmp(ks[i].section, ks[j].section) == 0 &&
                strcmp(ks[j - 1].section, ks[j].section) != 0)
                return 0;
        }
    }
    return 1;
}

/** @brief Check that the float rows' setters refuse NaN and infinity without calling through.
 *
 * NaN fails every range comparison, so a bounds test alone passes it on.
 * @return 0 when every float row refuses both, 1 otherwise. */
static int float_rows_refuse_non_finite(void) {
    static const char *const float_rows[] = { "fov", "fog_scale", NULL };
    int rc = 0;
    for (int r = 0; float_rows[r]; r++) {
        const knob_desc_t *fk = find_row(float_rows[r], NULL);
        if (!fk || !fk->set_num) {
            rc |= fail("float row missing");
            continue;
        }
        float fov_before = s_native_fov;
        if (fk->set_num((knob_desc_t *)fk, (double)NAN) == 0)
            rc |= fail("a float row accepted NaN");
        if (fk->set_num((knob_desc_t *)fk, (double)INFINITY) == 0)
            rc |= fail("a float row accepted infinity");
        if (memcmp(&s_native_fov, &fov_before, sizeof fov_before) != 0)
            rc |= fail("a refused value reached the setter");
    }
    return rc;
}

/** @brief Check the registry's device_creation marks against the config schema's device set, both ways.
 *
 * Every spelling of a marked row has the row's class in the schema set;
 * every schema spelling a row names marks that row with the same class.
 * Spellings with no row (harness env vars) are the schema set's alone. The
 * set must be non-empty, so a check over nothing fails.
 * @return 1 on a mismatch, else 0. */
static int device_marks_agree(void) {
    int rc = 0, marked = 0;
    size_t n;
    const knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        const char *sp[2] = { ks[i].cli, ks[i].env };
        for (int j = 0; j < 2; j++) {
            if (!sp[j]) continue;
            const int cls = (int)cfg_schema_device_class(sp[j]);
            if (cls == (int)ks[i].device_creation) { marked += cls != 0; continue; }
            rc |= fail("device-creation mark differs between registry and schema");
            fprintf(stderr, "       %s (%s): registry %d, schema %d\n",
                    ks[i].id, sp[j], ks[i].device_creation, cls);
        }
    }
    size_t dn;
    (void)cfg_schema_device_spellings(&dn);
    if (dn == 0 || marked == 0) rc |= fail("device-creation set is empty");
    printf("knob_registry_test: device marks: %d row spellings, %zu schema spellings\n",
           marked, dn);
    return rc;
}

/** @brief The value the resolver splices for `flag` from a config file holding `body`, or NULL when it splices none.
 * @param path The config file to write.
 * @param body The file's text.
 * @param flag The flag to look for.
 * @param rc Receives openpete_config_apply's return.
 * @return The spliced value, or NULL. */
static const char *config_flag_value(const char *path, const char *body,
                                     const char *flag, int *rc) {
    FILE *f = fopen(path, "wb");
    if (!f) { *rc = -1; return NULL; }
    fputs(body, f);
    fclose(f);
    openpete_config_reset();
    openpete_config_set_game_id("spyro-1");
    static char *argv[3];
    argv[0] = (char *)"openpete";
    argv[1] = (char *)"--config";
    argv[2] = (char *)path;
    int oc = 0;
    char **ov = NULL;
    *rc = openpete_config_apply(3, argv, 3, &oc, &ov);
    if (*rc != 1) return NULL;
    for (int i = 0; i + 1 < oc; i++)
        if (!strcmp(ov[i], flag)) return ov[i + 1];
    return NULL;
}

/** @brief The former bool `fog` row's saved values read as the fog mode's: a true spelling as unset, a false one as off.
 * @return 0 on success, 1 on a failure. */
static int legacy_fog_values_read(void) {
    const char *tmp = getenv("TMPDIR");
    char path[512];
    snprintf(path, sizeof path, "%s/openpete-knob-fog-test.toml", (tmp && *tmp) ? tmp : "/tmp");
    static const struct { const char *body; const char *want; } cases[] = {
        { "fog = \"on\"\n",              NULL },
        { "fog = true\n",                NULL },
        { "fog = \"1\"\n",               NULL },
        { "[saved]\nfog = \"on\"\n",      NULL },
        { "fog = \"off\"\n",             "off" },
        { "fog = false\n",               "off" },
        { "fog = \"0\"\n",               "off" },
        { "[saved]\nfog = \"off\"\n",     "off" },
        { "fog = \"per-pixel\"\n",       "per-pixel" },
    };
    int rc = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int arc = 0;
        const char *got = config_flag_value(path, cases[i].body, "--fog", &arc);
        const int ok = arc >= 0 && (cases[i].want ? (got && !strcmp(got, cases[i].want)) : !got);
        if (!ok) {
            rc |= fail("a legacy fog value read wrong");
            fprintf(stderr, "       %s -> %s (rc %d), want %s\n", cases[i].body,
                    got ? got : "(unset)", arc, cases[i].want ? cases[i].want : "(unset)");
        }
    }
    remove(path);
    openpete_config_reset();
    return rc;
}

/** @brief Check that the settings writer never settles a row that has no setter.
 *
 * load_delay has no setter; its getter reports the resolved delay, which
 * becomes the game's own default after the boot snapshot. Writing it would
 * pin one game's delay for every game.
 * @return 0 on pass, nonzero on failure. */
static int check_no_setter_never_saved(void) {
    const knob_desc_t *ld = find_row("load_delay", NULL);
    if (!ld) return fail("load_delay row missing from the registry");
    if (ld->set_num || ld->set_str)
        return fail("load_delay gained a setter; revisit the no-setter save rule");
    const char *ids[] = { "load_delay" };
    if (knob_registry_settle_ids(ids, 1) != 0)
        return fail("the settings writer settled a row with no setter");
    return 0;
}

/** @brief Check that the TERRAIN_DRAIN_SKIP row edits the renderer's packing through its setter and leaves the environment alone.
 *
 * A live row whose setter wrote the variable would race the walk thread's
 * read of it; the row's value is renderer state.
 * @return 0 on pass, nonzero on failure. */
static int check_drain_skip_row_setter(void) {
    const knob_desc_t *k = find_row("TERRAIN_DRAIN_SKIP", NULL);
    if (!k) return fail("TERRAIN_DRAIN_SKIP row missing from the registry");
    if (k->mut != KNOB_LIVE || !k->set_num || !k->get_num || k->set_str)
        return fail("TERRAIN_DRAIN_SKIP is not a live row with a numeric setter");
    knob_desc_t *w = (knob_desc_t *)k;
    if (w->set_num(w, 0.0) != 0 || s_native_drain_legacy != 1 || w->get_num(w) != 0.0)
        return fail("TERRAIN_DRAIN_SKIP 0 did not select the keyed packing");
    if (w->set_num(w, 1.0) != 0 || s_native_drain_legacy != 0 || w->get_num(w) != 1.0)
        return fail("TERRAIN_DRAIN_SKIP 1 did not select the sentinel");
    if (getenv("OPENPETE_TERRAIN_DRAIN_SKIP"))
        return fail("the TERRAIN_DRAIN_SKIP row wrote the environment");
    return 0;
}

/** @brief Report the first two rows of the table that share an id, a flag or an env var.
 *
 * Two rows under one name collide in the overlay's imgui ID scope, in the
 * config resolver and in the surface dump; a module moved into shared code
 * that keeps its own registration is the case this catches.
 * @return 0 when every name is unique, 1 after printing the clash. */
static int names_unique(void) {
    size_t n;
    const knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            const char *what = NULL;
            if (ks[i].id && ks[j].id && !strcmp(ks[i].id, ks[j].id)) what = ks[i].id;
            else if (ks[i].cli && ks[j].cli && !strcmp(ks[i].cli, ks[j].cli)) what = ks[i].cli;
            else if (ks[i].env && ks[j].env && !strcmp(ks[i].env, ks[j].env)) what = ks[i].env;
            if (what) {
                printf("FAIL: rows %zu (%s) and %zu (%s) share the name %s\n", i,
                       ks[i].id ? ks[i].id : "?", j, ks[j].id ? ks[j].id : "?", what);
                return 1;
            }
        }
    }
    return 0;
}

/** @brief A contributed row reusing a registered id, flag or env var is refused and the table keeps one row per name.
 *
 * @return 0 on pass, 1 on the first failure. */
static int duplicate_add_refused(void) {
    size_t n0, n1;
    const knob_desc_t *ks = knob_registry(&n0);
    const knob_desc_t *with_cli = NULL, *with_env = NULL;
    for (size_t i = 0; i < n0; i++) {
        if (!with_cli && ks[i].cli) with_cli = &ks[i];
        if (!with_env && ks[i].env) with_env = &ks[i];
    }
    if (!with_cli || !with_env) return fail("no registry row carries a flag and an env var");
    knob_desc_t row = {0};
    row.label = "Duplicate";
    row.section = "Gameplay";
    row.type = KNOB_ACTION;
    row.mut = KNOB_LIVE;
    row.id = "test_action";
    if (openpete_knob_add(&row) != -1) return fail("a second row with id test_action was added");
    row.id = "test_dup_cli";
    row.cli = with_cli->cli;
    if (openpete_knob_add(&row) != -1) return fail("a row reusing a registered flag was added");
    row.id = "test_dup_env";
    row.cli = NULL;
    row.env = with_env->env;
    if (openpete_knob_add(&row) != -1) return fail("a row reusing a registered env var was added");
    (void)knob_registry(&n1);
    if (n1 != n0) return fail("a refused duplicate changed the row count");
    return names_unique();
}

/** @brief Check the two class fields across the table: every live value row that is not a variable row declares its replay class, every replay-render row is presentation, and a contributed row without a class is refused.
 * @return 0 on pass, else 1. */
static int classes_declared(void) {
    int rc = 0;
    size_t n0;
    const knob_desc_t *ks = knob_registry(&n0);
    for (size_t i = 0; i < n0; i++) {
        if (knob_needs_replay_class(&ks[i]) && ks[i].replay == KNOB_REPLAY_UNSET) {
            rc |= fail("a live value row declares no replay class");
            fprintf(stderr, "       %s\n", ks[i].id);
        }
        if (ks[i].replay == KNOB_REPLAY_RENDER && ks[i].effect != KNOB_EFFECT_PRESENTATION) {
            rc |= fail("a replay-render row is not presentation");
            fprintf(stderr, "       %s\n", ks[i].id);
        }
    }
    knob_desc_t row = {0};
    row.id = "test_unclassified";
    row.label = "Unclassified";
    row.section = "Gameplay";
    row.type = KNOB_BOOL;
    row.mut = KNOB_LIVE;
    if (openpete_knob_add(&row) != -1) rc |= fail("a live row with no replay class was added");
    row.env = "OPENPETE_TEST_UNCLASSIFIED";
    row.type = KNOB_STRING;
    if (openpete_knob_add(&row) != 0) rc |= fail("a variable row was refused for its derived class");
    size_t n1;
    (void)knob_registry(&n1);
    if (n1 != n0 + 1) rc |= fail("the class refusal changed the row count");
    return rc;
}

int main(void) {
    int rc = 0;

    size_t before = 0;
    (void)knob_registry(&before);

    /* A contributed row joins its named section rather than landing at the
     * end of the table. */
    knob_desc_t row = {0};
    row.id           = "test_action";
    row.label        = "Test action";
    row.section      = "Gameplay";
    row.help         = "unit test";
    row.type         = KNOB_ACTION;
    row.mut          = KNOB_LIVE;
    row.get_num      = act_ready;
    row.set_num      = act_fire;
    row.get_str      = act_status;
    row.action_label = "Apply";
    if (openpete_knob_add(&row) != 0) rc |= fail("openpete_knob_add rejected a row");

    size_t after = 0, idx = 0;
    (void)knob_registry(&after);
    if (after != before + 1) rc |= fail("row count did not grow by one");

    const knob_desc_t *k = find_row("test_action", &idx);
    if (!k) {
        rc |= fail("contributed row missing from the registry");
    } else {
        size_t n;
        const knob_desc_t *ks = knob_registry(&n);
        if (strcmp(ks[idx - 1].section, "Gameplay") != 0)
            rc |= fail("row did not land inside its section");
        if (idx + 1 < n && strcmp(ks[idx + 1].section, "Gameplay") == 0)
            rc |= fail("row landed before the section's last built-in");
    }
    if (!sections_contiguous()) rc |= fail("insertion split a section run");
    rc |= classes_declared();

    /* Boot stamping must leave an action row alone: it has no value, and a
     * stamped boot_num would make the overlay render it as "edited". */
    openpete_options_t opts;
    memset(&opts, 0, sizeof opts);
    opts.vsync = -1;
    opts.render_fps = -1;
    opts.tick_scale = 1.0;
    knob_registry_init(&opts);
    k = find_row("test_action", NULL);
    if (!k) return fail("row vanished across knob_registry_init");
    if (k->boot_num != 0.0) rc |= fail("action row got a boot value stamped");

    /* The value column of an action row shows its status, not a number. */
    char buf[KNOB_STR_MAX];
    knob_value_str(k, buf, sizeof buf);
    if (strcmp(buf, "") != 0) rc |= fail("un-fired action should report no status");
    if (k->set_num((knob_desc_t *)k, 1.0) != 0) rc |= fail("action setter refused");
    if (s_fired != 1) rc |= fail("action did not fire");
    knob_value_str(k, buf, sizeof buf);
    if (strcmp(buf, "applied") != 0) rc |= fail("status string not reported");

    /* Rows the run has no use for report themselves hidden, so the overlay
     * drops them - and, since every Recording row is one of them, drops the
     * section header with them. The predicate reads two things: whether the
     * build can encode at all, and whether this run was launched recording. */
    {
        static const char *const rec_rows[] = {
            "record", "record_cfr", "record_frames", "record_fps_overlay", NULL
        };
        openpete_options_t ropts;
        memset(&ropts, 0, sizeof ropts);
        ropts.vsync = -1; ropts.render_fps = -1; ropts.tick_scale = 1.0;

        for (int pass = 0; pass < 3; pass++) {
            /* pass 0: no --record. 1: recording. 2: recording, no encoder. */
            ropts.record_path  = pass ? "clip.mkv" : NULL;
            s_record_supported = (pass != 2);
            knob_registry_init(&ropts);
            for (int r = 0; rec_rows[r]; r++) {
                const knob_desc_t *rk = find_row(rec_rows[r], NULL);
                if (!rk || !rk->visible) {
                    rc |= fail("a Recording row carries no visibility rule");
                    continue;
                }
                if (!!rk->visible(rk) != (pass == 1)) {
                    rc |= fail("Recording row visibility ignored the run");
                    fprintf(stderr, "       %s, pass %d\n", rec_rows[r], pass);
                }
            }
        }
        s_record_supported = 1;

        /* bk2 replay drives the pad, so it is not one of them: it stays
         * listed whether or not the run records. */
        const knob_desc_t *bk2 = find_row("bk2", NULL);
        if (!bk2 || bk2->visible) rc |= fail("bk2 row hides with the recording rows");
        if (bk2 && strcmp(bk2->section, "Gameplay") != 0)
            rc |= fail("bk2 row left the Gameplay section");
    }

    /* A pick made in the O menu must register as an edit. That menu keeps its
     * own row state and calls the renderer's setters directly, so a row that
     * reported a registry-side shadow instead of the renderer's live value
     * would stay at its boot value forever - and "save edited settings" skips
     * unedited rows, so the pick would silently never persist. */
    {
        static const struct { const char *id; const char *want; } picks[] = {
            { "ssaa",             "4"     },
            { "aspect",           "16:9"  },
            { "vsync",            "off"   },
            { "perf_stats",       "off"   },
            { "vertex_precision", "integer" },
            { "raster_quantize",  "1"     },
        };
        openpete_options_t mopts;
        memset(&mopts, 0, sizeof mopts);
        mopts.vsync = -1; mopts.render_fps = -1; mopts.tick_scale = 1.0;
        mopts.ssaa = 1; mopts.perf_stats = OPENPETE_PERF_FPS; mopts.raster_quantize = -1;
        knob_registry_init(&mopts);
        s_native_up = true;   /* the renderer comes up after the boot stamp */

        /* the setter calls the O menu makes on its own rows */
        openpete_native_set_ssaa(4);
        openpete_native_set_aspect(16.0f / 9.0f);
        openpete_native_set_vsync(0);
        openpete_native_set_perf_stats(OPENPETE_PERF_OFF);
        openpete_native_set_vertex_precision(1);
        openpete_native_set_raster_quantize(1);

        for (size_t i = 0; i < sizeof picks / sizeof picks[0]; i++) {
            const knob_desc_t *pk = find_row(picks[i].id, NULL);
            if (!pk) { rc |= fail("display row missing from the registry"); continue; }
            if (pk->get_num(pk) == pk->boot_num) {
                rc |= fail("an O-menu pick did not register as an edit");
                fprintf(stderr, "       %s still reads its boot value\n", picks[i].id);
                continue;
            }
            char got[KNOB_STR_MAX];
            if (knob_config_value_str(pk, got, sizeof got) != 0) {
                rc |= fail("an O-menu pick has no persistable value");
                fprintf(stderr, "       %s\n", picks[i].id);
            } else if (strcmp(got, picks[i].want) != 0) {
                rc |= fail("an O-menu pick persists the wrong value");
                fprintf(stderr, "       %s: %s, want %s\n", picks[i].id, got, picks[i].want);
            }
        }

        /* The SSAA ladder is a factor TABLE, not a 1<<index mapping: an odd
         * rung has to survive the round trip to the flag's vocabulary. */
        {
            openpete_native_set_ssaa(3);
            const knob_desc_t *sk = find_row("ssaa", NULL);
            char got[KNOB_STR_MAX];
            if (!sk || knob_config_value_str(sk, got, sizeof got) != 0 ||
                strcmp(got, "3") != 0)
                rc |= fail("3x SSAA did not round-trip to --ssaa 3");
            openpete_native_set_ssaa(4);
        }

        /* Before the renderer is up (and in headless / PsyCross-only runs, where
         * it never comes up) the rows fall back to their stamped boot value
         * instead of reading a renderer that has no state yet. */
        s_native_up = false;
        for (size_t i = 0; i < sizeof picks / sizeof picks[0]; i++) {
            const knob_desc_t *pk = find_row(picks[i].id, NULL);
            if (pk && pk->get_num(pk) != pk->boot_num) {
                rc |= fail("a display row read the renderer before it was up");
                fprintf(stderr, "       %s\n", picks[i].id);
            }
        }
    }

    /* Before the capacity fill below, so a refusal here is the name's. */
    rc |= names_unique();
    rc |= duplicate_add_refused();

    /* Capacity is finite and refuses politely rather than corrupting. Each
     * filler has its own id, since a repeated id is refused first. */
    int added = 0;
    static char filler_ids[64][24];
    for (int i = 0; i < 64; i++) {
        knob_desc_t filler = {0};
        snprintf(filler_ids[i], sizeof filler_ids[i], "test_filler_%d", i);
        filler.id      = filler_ids[i];
        filler.label   = "filler";
        filler.section = "ZZZ test";
        filler.help    = "unit test";
        filler.type    = KNOB_ACTION;
        filler.mut     = KNOB_LIVE;
        if (openpete_knob_add(&filler) != 0) break;
        added++;
    }
    if (added >= 64) rc |= fail("capacity never refused an add");
    if (!find_row("test_action", NULL)) rc |= fail("overflow clobbered a row");

    rc |= check_drain_skip_row_setter();

    /* The registry and the config schema are two tables describing the same
     * knobs, and they have to agree or a flag silently loses its
     * openpete.toml bare key - and, when it also has an env twin, loses the
     * flattening that keeps the two spellings from fighting.
     *
     * Checked in the direction that catches the mistake people actually
     * make: adding a CLI flag with a registry row and forgetting the schema.
     * The reverse (a schema row with no registry row) is checked too, so the
     * schema can't grow knobs the M overlay never lists.
     *
     * cli_only rows (one-shot action flags such as --ingest) are exempt from
     * the first direction: openpete.toml must not name them.
     *
     * ENVROW_* rows are exempt: they are the ~150 debug-only OPENPETE_* vars,
     * which have no flag to conflict with and no bare key by design. Their
     * ids are the SHOUTING var name minus the prefix, which is what tells
     * them apart from the hand-written env-only rows. */
    {
        size_t n;
        const knob_desc_t *ks = knob_registry(&n);
        for (size_t i = 0; i < n; i++) {
            if (!ks[i].cli || ks[i].cli_only) continue;
            int si = cfg_schema_find_cli(ks[i].cli);
            if (si < 0) {
                rc |= fail("knob has no config schema row");
                fprintf(stderr, "       %s (%s) - add it to config_schema.c\n",
                        ks[i].id, ks[i].cli);
            } else if (strcmp(cfg_schema(NULL)[si].id, ks[i].id) != 0) {
                rc |= fail("schema/registry id mismatch for a flag");
                fprintf(stderr, "       %s vs %s for %s\n",
                        cfg_schema(NULL)[si].id, ks[i].id, ks[i].cli);
            }
        }
        size_t sn;
        const cfg_knob_t *sch = cfg_schema(&sn);
        for (size_t i = 0; i < sn; i++) {
            if (!find_row(sch[i].id, NULL)) {
                rc |= fail("config schema names a knob the registry lacks");
                fprintf(stderr, "       %s - add its row to knob_registry.c\n",
                        sch[i].id);
            }
        }
    }

    rc |= float_rows_refuse_non_finite();
    rc |= device_marks_agree();
    rc |= legacy_fog_values_read();
    rc |= check_no_setter_never_saved();
    rc |= names_unique();

    printf(rc ? "knob_registry_test: FAILED\n" : "knob_registry_test: ok\n");
    return rc;
}
