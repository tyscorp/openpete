/* options_validate_test - the option parser's shared value validator.
 *
 * A value refused by type or range fails the parse when it is typed, and
 * warns and keeps the option's default when it comes from openpete.toml (a
 * bare key, an [args] entry) or from an env spelling (the environment or an
 * [env] table). Strict mode, which the surface dump's selfcheck uses, makes
 * every refusal fail. The env spellings options_init used to read itself
 * (OPENPETE_LOAD_DELAY and its siblings) are applied after the flags, so an
 * [env] table reaches them. openpete_options_effective resolves the
 * sentinels the selfcheck compares.
 *
 * options.c, the config resolver and the keymap compile straight in; the
 * game hooks the parser calls and the knob registry are stubbed below. */

#include "openpete.h"
#include "openpete_config.h"
#include "game_extras.h"
#include "openpete_keymap.h"
#include "openpete_knobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define setenv_portable(k, v) _putenv_s((k), (v))
  #define unsetenv_portable(k)  _putenv_s((k), "")
#else
  #define setenv_portable(k, v) setenv((k), (v), 1)
  #define unsetenv_portable(k)  unsetenv(k)
#endif

/* ---- game hooks the parser calls ---------------------------------- */

/** @brief Stub level resolver: "artisans" is level 10, digits pass through, anything else is unknown. */
int game_resolve_level_arg(const char* arg) {
    if (!strcmp(arg, "artisans")) return 10;
    return (arg[0] >= '0' && arg[0] <= '9') ? atoi(arg) : -1;
}
/** @brief Stub cutscene resolver: every name is unknown. */
int game_resolve_cutscene_arg(const char* arg) { (void)arg; return -1; }
/** @brief Stub area resolver: every target is unknown. */
int game_resolve_area_arg(const char* arg, int* out_area) { (void)arg; (void)out_area; return -1; }
/** @brief Stub cheat validator: every spec is refused. */
int game_validate_cheats_arg(const char* spec) { (void)spec; return -1; }
/** @brief Stub game load delay: 66 VBLs, a value no option defaults to. */
uint32_t game_get_load_delay_default(void) { return 66u; }

/* ---- host seams options.c reaches ---------------------------------- */

/** @brief Whether name is one of the six SDL gamepad axis names. */
static int is_axis_name(const char* name) {
    static const char* const axes[] = { "leftx", "lefty", "rightx", "righty",
                                        "lefttrigger", "righttrigger" };
    for (size_t i = 0; i < sizeof axes / sizeof axes[0]; i++)
        if (!strcmp(name, axes[i])) return 1;
    return 0;
}

/** @brief Stub name lookup: every key and mouse name resolves to code 1, a pad name to code 1 in the table SDL holds it in.
 *
 * The keymap parser tries the button table before the axis table, as SDL
 * names buttons and axes from disjoint tables; a stub that resolved an axis
 * name as a button would refuse the built-in "pad:-lefty" defaults.
 * @param kind The source kind the parser looks the name up in.
 * @param name The token name.
 * @return 1, or -1 when the name is an axis looked up as a button or a non-axis looked up as an axis. */
static int any_name(int kind, const char* name) {
    if (kind == KM_SRC_PADBTN)  return is_axis_name(name) ? -1 : 1;
    if (kind == KM_SRC_PADAXIS) return is_axis_name(name) ? 1 : -1;
    return 1;
}
/** @brief Stub reverse lookup: every code is named "k". */
static const char* any_code(int kind, int code) { (void)kind; (void)code; return "k"; }
/** @brief Stub for the SDL key-name install: the keymap's built-in defaults need a lookup to parse, and the options do not care which. */
void openpete_keymap_install_sdl(void) { openpete_keymap_set_lookup(any_name, any_code, NULL); }

/** @brief Stub knob registry: no rows; only the --help generator reads it, and no case prints help.
 * @param count Receives 0; may be NULL.
 * @return NULL. */
knob_desc_t* knob_registry(size_t* count) {
    if (count) *count = 0;
    return NULL;
}

/** @brief Stub anchor parser: the six anchor words, as overlay_anchor.c spells them. */
int openpete_anchor_parse(const char* s) {
    static const char* const names[] = { "top-left", "top-center", "top-right",
                                         "bottom-left", "bottom-center", "bottom-right" };
    for (int i = 0; s && i < 6; i++)
        if (!strcmp(s, names[i])) return i;
    return -1;
}

/* ---- harness ------------------------------------------------------ */

static int g_fail;
static const char* g_case = "";
static char g_toml[512];

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL [%s] ", g_case);                          \
            fprintf(stderr, __VA_ARGS__);                                   \
            fprintf(stderr, "\n         at %s:%d\n", __FILE__, __LINE__);   \
            g_fail++;                                                       \
        }                                                                   \
    } while (0)

/** @brief Write body to the test's config file. */
static void write_toml(const char* body) {
    FILE* f = fopen(g_toml, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", g_toml); exit(2); }
    fputs(body, f);
    fclose(f);
}

/** @brief Parse argv (argv[0] supplied) into fresh options after a resolver reset.
 *
 * @param o Receives the options.
 * @param n Number of arguments after argv[0].
 * @param args The arguments.
 * @return The parser's exit code. */
static int parse(openpete_options_t* o, int n, const char* const* args) {
    char* argv[16];
    argv[0] = (char*)"options_validate_test";
    for (int i = 0; i < n; i++) argv[i + 1] = (char*)args[i];
    argv[n + 1] = NULL;
    openpete_options_init(o);
    openpete_config_reset();
    return openpete_options_parse(o, n + 1, argv);
}

/** @brief Parse with the test's config file and the given typed arguments. */
static int parse_cfg(openpete_options_t* o, const char* body, int n, const char* const* typed) {
    const char* args[12] = { "--config", g_toml };
    for (int i = 0; i < n; i++) args[2 + i] = typed[i];
    write_toml(body);
    return parse(o, n + 2, args);
}

/* ---- cases -------------------------------------------------------- */

/** @brief A typed value refused by range or type fails; the same value from the config file keeps the default. */
static void typed_refuses_config_falls_back(void) {
    openpete_options_t o;
    static const char* const ssaa0[] = { "--no-config", "--ssaa", "0" };
    g_case = "typed ssaa 0";
    CHECK(parse(&o, 3, ssaa0) == OPENPETE_EXIT_SETUP_ERROR, "typed --ssaa 0 parsed");

    g_case = "config ssaa 0";
    CHECK(parse_cfg(&o, "ssaa = 0\n", 0, NULL) == OPENPETE_EXIT_OK, "config ssaa = 0 failed the parse");
    CHECK(o.ssaa == 1, "ssaa = %d, want the default 1", o.ssaa);

    g_case = "config fov abc";
    CHECK(parse_cfg(&o, "fov = \"abc\"\n", 0, NULL) == OPENPETE_EXIT_OK, "config fov failed the parse");
    CHECK(o.fov_deg == 0.0f, "fov = %g, want the default 0", (double)o.fov_deg);

    g_case = "config args perf_stats";
    CHECK(parse_cfg(&o, "[args]\n\"--perf-stats\" = \"lots\"\n", 0, NULL) == OPENPETE_EXIT_OK,
          "[args] --perf-stats failed the parse");
    CHECK(o.perf_stats == OPENPETE_PERF_FPS, "perf_stats = %d, want the default", (int)o.perf_stats);

    g_case = "config skip_to_level";
    CHECK(parse_cfg(&o, "skip_to_level = \"nowhere\"\n", 0, NULL) == OPENPETE_EXIT_OK,
          "config skip_to_level failed the parse");
    CHECK(o.skip_to_level == 0, "skip_to_level = %d, want 0", o.skip_to_level);

    g_case = "config good value still applies";
    CHECK(parse_cfg(&o, "ssaa = 3\n", 0, NULL) == OPENPETE_EXIT_OK, "config ssaa = 3 failed");
    CHECK(o.ssaa == 3, "ssaa = %d, want 3", o.ssaa);
}

/** @brief A refused config value does not shield a refused typed value, an unknown flag from the config file still fails, and strict mode fails a refused config value. */
static void refusals_that_stand(void) {
    openpete_options_t o;
    static const char* const fovabc[] = { "--fov", "abc" };
    g_case = "config bad + typed bad";
    CHECK(parse_cfg(&o, "fog_scale = \"abc\"\n", 2, fovabc) == OPENPETE_EXIT_SETUP_ERROR,
          "a typed --fov abc parsed beside a refused config value");

    g_case = "config unknown flag";
    CHECK(parse_cfg(&o, "ssaa = 2\n[args]\n\"--no-such-flag\" = \"1\"\n", 0, NULL) == OPENPETE_EXIT_OK,
          "an unknown [args] flag failed the boot parse");
    CHECK(o.ssaa == 2, "ssaa = %d, want 2 beside the ignored flag", o.ssaa);
    static const char* const rep[] = { "--config-report" };
    CHECK(parse_cfg(&o, "[args]\n\"--no-such-flag\" = \"1\"\n", 1, rep) == OPENPETE_EXIT_SETUP_ERROR,
          "--config-report kept an unknown [args] flag");
    static const char* const typed_unknown[] = { "--no-config", "--no-such-flag" };
    CHECK(parse(&o, 2, typed_unknown) == OPENPETE_EXIT_SETUP_ERROR, "a typed unknown flag parsed");

    g_case = "strict config";
    openpete_options_set_strict(1);
    CHECK(parse_cfg(&o, "ssaa = 0\n", 0, NULL) == OPENPETE_EXIT_SETUP_ERROR,
          "strict mode kept a refused config value");
    openpete_options_set_strict(0);

    g_case = "config-report refuses";
    static const char* const report[] = { "--config-report" };
    CHECK(parse_cfg(&o, "ssaa = 0\n", 1, report) == OPENPETE_EXIT_SETUP_ERROR,
          "--config-report kept a refused flag value");
    CHECK(parse_cfg(&o, "lod = \"bogus\"\n", 1, report) == OPENPETE_EXIT_SETUP_ERROR,
          "--config-report kept a refused enum word");

    g_case = "config enum word";
    CHECK(parse_cfg(&o, "lod = \"bogus\"\nvsync = \"maybe\"\n", 0, NULL) == OPENPETE_EXIT_OK,
          "a refused enum word failed the boot parse");
    CHECK(o.lod == OPENPETE_LOD_ORIGINAL && o.vsync == -1, "lod %d vsync %d", (int)o.lod, o.vsync);

    g_case = "run-recomp flag removed";
    static const char* const rr[] = { "--no-config", "--run-recomp", "0x80012204" };
    CHECK(parse(&o, 3, rr) == OPENPETE_EXIT_SETUP_ERROR, "--run-recomp still parses");
}

/** @brief An env spelling's refused value warns and keeps the default; strict mode fails it; a valid one applies. */
static void env_spellings(void) {
    openpete_options_t o;
    static const char* const none[] = { "--no-config" };
    g_case = "env mouse bogus";
    setenv_portable("OPENPETE_MOUSE", "bogus");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK, "OPENPETE_MOUSE=bogus failed the parse");
    CHECK(o.mouse == -1, "mouse = %d, want unset -1", o.mouse);
    openpete_options_set_strict(1);
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_SETUP_ERROR, "strict kept OPENPETE_MOUSE=bogus");
    openpete_options_set_strict(0);
    unsetenv_portable("OPENPETE_MOUSE");

    g_case = "env vibration bogus";
    setenv_portable("OPENPETE_VIBRATION", "11");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK, "OPENPETE_VIBRATION=11 failed the parse");
    CHECK(o.vibration == -1, "vibration = %d, want unset -1", o.vibration);
    unsetenv_portable("OPENPETE_VIBRATION");

    g_case = "env run recomp";
    setenv_portable("OPENPETE_RUN_RECOMP", "0x80012204");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK, "OPENPETE_RUN_RECOMP failed the parse");
    CHECK(o.run_recomp_addr == 0x80012204u, "run_recomp_addr = 0x%08X", o.run_recomp_addr);
    unsetenv_portable("OPENPETE_RUN_RECOMP");
}

/** @brief --disc-cache and OPENPETE_DISC_CACHE: 0, 1 and auto parse, other text is refused when typed and is auto from the env, and the flag beats the env. */
static void disc_cache_modes(void) {
    openpete_options_t o;
    static const char* const none[] = { "--no-config" };
    static const char* const dc_auto[] = { "--no-config", "--disc-cache", "auto" };
    static const char* const dc0[] = { "--no-config", "--disc-cache", "0" };
    static const char* const dc1[] = { "--no-config", "--disc-cache", "1" };
    static const char* const dc_bad[] = { "--no-config", "--disc-cache", "yes" };
    g_case = "--disc-cache 0|1|auto";
    CHECK(parse(&o, 3, dc_auto) == OPENPETE_EXIT_OK && o.disc_cache == OPENPETE_DISC_CACHE_AUTO,
          "disc_cache = %d, want auto", o.disc_cache);
    CHECK(parse(&o, 3, dc0) == OPENPETE_EXIT_OK && o.disc_cache == OPENPETE_DISC_CACHE_OFF,
          "disc_cache = %d, want 0", o.disc_cache);
    CHECK(parse(&o, 3, dc1) == OPENPETE_EXIT_OK && o.disc_cache == OPENPETE_DISC_CACHE_ON,
          "disc_cache = %d, want 1", o.disc_cache);
    CHECK(parse(&o, 3, dc_bad) == OPENPETE_EXIT_SETUP_ERROR, "--disc-cache yes was accepted");

    g_case = "OPENPETE_DISC_CACHE";
    setenv_portable("OPENPETE_DISC_CACHE", "0");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.disc_cache == OPENPETE_DISC_CACHE_OFF,
          "disc_cache = %d, want 0 from the env", o.disc_cache);
    CHECK(parse(&o, 3, dc_auto) == OPENPETE_EXIT_OK && o.disc_cache == OPENPETE_DISC_CACHE_AUTO,
          "disc_cache = %d, want the typed auto over OPENPETE_DISC_CACHE=0", o.disc_cache);
    setenv_portable("OPENPETE_DISC_CACHE", "1");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.disc_cache == OPENPETE_DISC_CACHE_ON,
          "disc_cache = %d, want 1 from the env", o.disc_cache);
    setenv_portable("OPENPETE_DISC_CACHE", "yes");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.disc_cache == OPENPETE_DISC_CACHE_AUTO,
          "disc_cache = %d, want auto for an unparsed env value", o.disc_cache);
    unsetenv_portable("OPENPETE_DISC_CACHE");
}

/** @brief The env spellings resolved after the flags: defaults, env values, the [env] table, and a typed flag beating the env. */
static void resolved_env_fields(void) {
    openpete_options_t o;
    static const char* const none[] = { "--no-config" };
    g_case = "defaults";
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK, "the default parse failed");
    CHECK(o.notex == 0 && o.wireframe == 0 && o.terrain_cache == 1 &&
          o.disc_cache == OPENPETE_DISC_CACHE_AUTO,
          "notex %d wireframe %d terrain_cache %d disc_cache %d", o.notex, o.wireframe,
          o.terrain_cache, o.disc_cache);
    CHECK(o.load_delay == -1, "load_delay = %d, want -1 for run.c", o.load_delay);
    CHECK(o.sector_visibility == OPENPETE_SECTOR_VIS_AUTO, "sector_visibility %d, want auto",
          (int)o.sector_visibility);

    g_case = "--sector-visibility auto and filled";
    static const char* const sv_auto[] = { "--no-config", "--sector-visibility", "auto" };
    CHECK(parse(&o, 3, sv_auto) == OPENPETE_EXIT_OK, "the auto parse failed");
    CHECK(o.sector_visibility == OPENPETE_SECTOR_VIS_AUTO, "sector_visibility %d, want auto",
          (int)o.sector_visibility);
    static const char* const sv_filled[] = { "--no-config", "--sector-visibility", "filled" };
    CHECK(parse(&o, 3, sv_filled) == OPENPETE_EXIT_OK, "the filled parse failed");
    CHECK(o.sector_visibility == OPENPETE_SECTOR_VIS_FILLED, "sector_visibility %d, want filled",
          (int)o.sector_visibility);

    g_case = "[env] table";
    CHECK(parse_cfg(&o, "[env]\nOPENPETE_LOAD_DELAY = \"7\"\n"
                        "OPENPETE_SECTOR_VISIBILITY = \"game\"\n"
                        "OPENPETE_TERRAIN_CACHE = \"0\"\n", 0, NULL) == OPENPETE_EXIT_OK,
          "the [env] parse failed");
    CHECK(o.load_delay == 7, "load_delay = %d, want 7 from [env]", o.load_delay);
    CHECK(o.sector_visibility == OPENPETE_SECTOR_VIS_GAME, "sector_visibility %d",
          (int)o.sector_visibility);
    CHECK(o.terrain_cache == 0, "terrain_cache = %d, want 0 from [env]", o.terrain_cache);
    unsetenv_portable("OPENPETE_LOAD_DELAY");
    unsetenv_portable("OPENPETE_SECTOR_VISIBILITY");
    unsetenv_portable("OPENPETE_TERRAIN_CACHE");

    g_case = "typed beats env";
    setenv_portable("OPENPETE_WIREFRAME", "1");
    static const char* const wf0[] = { "--no-config", "--wireframe", "0" };
    CHECK(parse(&o, 3, wf0) == OPENPETE_EXIT_OK && o.wireframe == 0,
          "wireframe = %d, want the typed 0", o.wireframe);
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.wireframe == 1,
          "wireframe = %d, want 1 from the env", o.wireframe);
    unsetenv_portable("OPENPETE_WIREFRAME");

    g_case = "typed notex beats env";
    setenv_portable("OPENPETE_NOTEX", "0");
    static const char* const nt1[] = { "--no-config", "--notex", "1" };
    CHECK(parse(&o, 3, nt1) == OPENPETE_EXIT_OK && o.notex == 1,
          "notex = %d, want the typed 1 over OPENPETE_NOTEX=0", o.notex);
    setenv_portable("OPENPETE_NOTEX", "1");
    static const char* const nt0[] = { "--no-config", "--notex", "0" };
    CHECK(parse(&o, 3, nt0) == OPENPETE_EXIT_OK && o.notex == 0,
          "notex = %d, want the typed 0 over OPENPETE_NOTEX=1", o.notex);
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.notex == 1,
          "notex = %d, want 1 from the env", o.notex);
    unsetenv_portable("OPENPETE_NOTEX");
}

/** @brief openpete_options_effective resolves each admitted sentinel and leaves typed values alone. */
static void effective_resolves_sentinels(void) {
    openpete_options_t o, e;
    static const char* const none[] = { "--no-config" };
    static const char* const typed[] = { "--no-config", "--mouse", "0", "--load-delay", "5" };
    g_case = "effective defaults";
    (void)parse(&o, 1, none);
    memset(&e, 0, sizeof e);
    openpete_options_effective(&o, &e);
    CHECK(e.mouse == 1 && e.input_lock == 0 && e.background == 0,
          "mouse %d input_lock %d background %d", e.mouse, e.input_lock, e.background);
    CHECK(e.mouse_sensitivity == 1.0f, "mouse_sensitivity %g", (double)e.mouse_sensitivity);
    CHECK(e.load_delay == 66, "load_delay = %d, want the game's 66", e.load_delay);
    CHECK(o.mouse == -1 && o.load_delay == -1, "effective wrote its input");

    g_case = "effective typed";
    (void)parse(&o, 5, typed);
    openpete_options_effective(&o, &e);
    CHECK(e.mouse == 0 && e.load_delay == 5, "mouse %d load_delay %d", e.mouse, e.load_delay);
}

/** @brief --window and --launcher-window take WxH or maximized from the CLI, the config file and the env twin; the typed flag beats OPENPETE_LAUNCHER_WINDOW and OPENPETE_LAUNCHER_FULLSCREEN. */
static void window_values(void) {
    openpete_options_t o;
    static const char* const none[] = { "--no-config" };
    static const char* const wmax[] = { "--no-config", "--window", "maximized" };
    static const char* const wbad[] = { "--no-config", "--window", "huge" };
    static const char* const lw[] = { "--no-config", "--launcher-window", "700x500",
                                      "--launcher-fullscreen", "0" };
    unsetenv_portable("OPENPETE_LAUNCHER_WINDOW");
    unsetenv_portable("OPENPETE_LAUNCHER_FULLSCREEN");

    g_case = "window maximized";
    CHECK(parse(&o, 3, wmax) == OPENPETE_EXIT_OK && o.window_maximized == 1 &&
          o.window_w == 0 && o.window_h == 0,
          "window_maximized %d %dx%d", o.window_maximized, o.window_w, o.window_h);
    CHECK(parse(&o, 3, wbad) == OPENPETE_EXIT_SETUP_ERROR, "typed --window huge parsed");
    CHECK(parse_cfg(&o, "window = \"maximized\"\n", 0, NULL) == OPENPETE_EXIT_OK &&
          o.window_maximized == 1, "config window = maximized: %d", o.window_maximized);

    g_case = "launcher knobs unset";
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.launcher_window_maximized == -1 &&
          o.launcher_fullscreen == -1,
          "launcher_window_maximized %d launcher_fullscreen %d",
          o.launcher_window_maximized, o.launcher_fullscreen);

    g_case = "launcher knobs from the env";
    setenv_portable("OPENPETE_LAUNCHER_WINDOW", "maximized");
    setenv_portable("OPENPETE_LAUNCHER_FULLSCREEN", "1");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.launcher_window_maximized == 1 &&
          o.launcher_fullscreen == 1,
          "launcher_window_maximized %d launcher_fullscreen %d",
          o.launcher_window_maximized, o.launcher_fullscreen);

    g_case = "typed launcher knobs beat the env";
    CHECK(parse(&o, 5, lw) == OPENPETE_EXIT_OK && o.launcher_window_maximized == 0 &&
          o.launcher_window_w == 700 && o.launcher_window_h == 500 && o.launcher_fullscreen == 0,
          "launcher_window %d %dx%d launcher_fullscreen %d", o.launcher_window_maximized,
          o.launcher_window_w, o.launcher_window_h, o.launcher_fullscreen);
    unsetenv_portable("OPENPETE_LAUNCHER_WINDOW");
    unsetenv_portable("OPENPETE_LAUNCHER_FULLSCREEN");
}

/** @brief Run every case; exit 1 on any failure. */
#ifdef _WIN32
  #define environ_portable _environ
#else
extern char** environ;
  #define environ_portable environ
#endif

/** @brief Unset every OPENPETE_* variable the process inherited, so each case sees only the env values it sets.
 *
 * The names are copied before any is unset, since unsetting edits the
 * environment array being walked. */
static void scrub_inherited_env(void) {
    static char names[64][64];
    int n = 0;
    for (char** e = environ_portable; e && *e && n < 64; e++) {
        const char* eq = strchr(*e, '=');
        size_t len = eq ? (size_t)(eq - *e) : strlen(*e);
        if (strncmp(*e, "OPENPETE_", 9) != 0 || len >= sizeof names[0]) continue;
        memcpy(names[n], *e, len);
        names[n++][len] = '\0';
    }
    for (int i = 0; i < n; i++) unsetenv_portable(names[i]);
}

/** @brief --tick-scale and the toml tick_scale key: a positive value outside [0.05, 8] is clamped into it, a value that is not a finite positive number is refused. */
static void tick_scale_range(void) {
    openpete_options_t o;
    static const char* const huge[] = { "--no-config", "--tick-scale", "1e9" };
    g_case = "typed tick-scale 1e9";
    CHECK(parse(&o, 3, huge) == OPENPETE_EXIT_OK, "typed --tick-scale 1e9 failed the parse");
    CHECK(o.tick_scale == OPENPETE_TICK_SCALE_MAX, "tick_scale = %g, want %g", o.tick_scale,
          OPENPETE_TICK_SCALE_MAX);

    static const char* const tiny[] = { "--no-config", "--tick-scale", "0.001" };
    g_case = "typed tick-scale 0.001";
    CHECK(parse(&o, 3, tiny) == OPENPETE_EXIT_OK, "typed --tick-scale 0.001 failed the parse");
    CHECK(o.tick_scale == OPENPETE_TICK_SCALE_MIN, "tick_scale = %g, want %g", o.tick_scale,
          OPENPETE_TICK_SCALE_MIN);

    static const char* const half[] = { "--no-config", "--tick-scale", "0.5" };
    g_case = "typed tick-scale 0.5";
    CHECK(parse(&o, 3, half) == OPENPETE_EXIT_OK && o.tick_scale == 0.5, "tick_scale = %g, want 0.5",
          o.tick_scale);

    static const char* const refused[][3] = {
        { "--no-config", "--tick-scale", "inf" },
        { "--no-config", "--tick-scale", "nan" },
        { "--no-config", "--tick-scale", "0" },
        { "--no-config", "--tick-scale", "-2" },
        { "--no-config", "--tick-scale", "0.5x" },
    };
    for (size_t k = 0; k < sizeof refused / sizeof refused[0]; k++) {
        g_case = refused[k][2];
        CHECK(parse(&o, 3, refused[k]) == OPENPETE_EXIT_SETUP_ERROR, "typed --tick-scale %s parsed",
              refused[k][2]);
    }

    g_case = "config tick_scale 1e9";
    CHECK(parse_cfg(&o, "tick_scale = 1e9\n", 0, NULL) == OPENPETE_EXIT_OK, "config tick_scale failed the parse");
    CHECK(o.tick_scale == OPENPETE_TICK_SCALE_MAX, "tick_scale = %g, want %g", o.tick_scale,
          OPENPETE_TICK_SCALE_MAX);
}

/** @brief --mod-hot-reload over OPENPETE_MOD_HOT_RELOAD: off by default, the env and the config key arm it, and a typed value wins over the env either way. */
static void mod_hot_reload_spellings(void) {
    openpete_options_t o, e;
    static const char* const none[] = { "--no-config" };
    static const char* const bare[] = { "--no-config", "--mod-hot-reload" };
    static const char* const off[]  = { "--no-config", "--mod-hot-reload", "0" };
    unsetenv_portable("OPENPETE_MOD_HOT_RELOAD");

    g_case = "mod_hot_reload default";
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.mod_hot_reload == 0,
          "mod_hot_reload = %d, want 0", o.mod_hot_reload);
    openpete_options_effective(&o, &e);
    CHECK(e.mod_hot_reload == 0, "effective mod_hot_reload = %d, want 0", e.mod_hot_reload);
    CHECK(parse(&o, 2, bare) == OPENPETE_EXIT_OK && o.mod_hot_reload == 1,
          "mod_hot_reload = %d, want 1 from the bare flag", o.mod_hot_reload);

    g_case = "mod_hot_reload env";
    setenv_portable("OPENPETE_MOD_HOT_RELOAD", "1");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.mod_hot_reload == 1,
          "mod_hot_reload = %d, want 1 from the env", o.mod_hot_reload);
    CHECK(parse(&o, 3, off) == OPENPETE_EXIT_OK && o.mod_hot_reload == 0,
          "mod_hot_reload = %d, want the typed 0 over OPENPETE_MOD_HOT_RELOAD=1", o.mod_hot_reload);
    setenv_portable("OPENPETE_MOD_HOT_RELOAD", "0");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK && o.mod_hot_reload == 0,
          "mod_hot_reload = %d, want 0 from OPENPETE_MOD_HOT_RELOAD=0", o.mod_hot_reload);
    CHECK(parse(&o, 2, bare) == OPENPETE_EXIT_OK && o.mod_hot_reload == 1,
          "mod_hot_reload = %d, want the typed flag over OPENPETE_MOD_HOT_RELOAD=0", o.mod_hot_reload);
    unsetenv_portable("OPENPETE_MOD_HOT_RELOAD");

    g_case = "mod_hot_reload config";
    CHECK(parse_cfg(&o, "mod_hot_reload = true\n", 0, NULL) == OPENPETE_EXIT_OK &&
          o.mod_hot_reload == 1, "mod_hot_reload = %d, want 1 from the key", o.mod_hot_reload);
    CHECK(parse_cfg(&o, "[env]\nOPENPETE_MOD_HOT_RELOAD = \"1\"\n", 0, NULL) == OPENPETE_EXIT_OK &&
          o.mod_hot_reload == 1, "mod_hot_reload = %d, want 1 from [env]", o.mod_hot_reload);
    unsetenv_portable("OPENPETE_MOD_HOT_RELOAD");
}

/** @brief OPENPETE_CONFIG names the config file when --config is absent, and a typed --config wins over it. */
static void config_flag_beats_env(void) {
    openpete_options_t o;
    char other[600];
    snprintf(other, sizeof other, "%s.env.toml", g_toml);
    FILE* f = fopen(other, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", other); exit(2); }
    fputs("tick_scale = 2\n", f);
    fclose(f);
    setenv_portable("OPENPETE_CONFIG", other);

    g_case = "OPENPETE_CONFIG alone";
    CHECK(parse(&o, 0, NULL) == OPENPETE_EXIT_OK && o.tick_scale == 2.0,
          "tick_scale = %g, want 2 from the OPENPETE_CONFIG file", o.tick_scale);

    g_case = "--config over OPENPETE_CONFIG";
    CHECK(parse_cfg(&o, "tick_scale = 0.5\n", 0, NULL) == OPENPETE_EXIT_OK && o.tick_scale == 0.5,
          "tick_scale = %g, want 0.5 from the --config file", o.tick_scale);

    unsetenv_portable("OPENPETE_CONFIG");
    remove(other);
}

/** @brief --freecam-pose and OPENPETE_FREECAM_POSE: a malformed pose fails when typed, falls back from the config file, and is warned and unset from the env; strict mode fails the env. */
static void freecam_pose_values(void) {
    openpete_options_t o;
    static const char* const good[] = { "--no-config", "--freecam-pose",
                                        "pos=(84992, 45582, 9948) yaw=1024 pitch=160" };
    static const char* const bad[][3] = {
        { "--no-config", "--freecam-pose", "pos=(bogus" },
        { "--no-config", "--freecam-pose", "pos=(1, 2" },
        { "--no-config", "--freecam-pose", "pos=(1, 2, 3) pitch=5" },
        { "--no-config", "--freecam-pose", "1.5, 2, 3" },
        { "--no-config", "--freecam-pose", "bogus" },
        { "--no-config", "--freecam-pose", "" },
    };
    unsetenv_portable("OPENPETE_FREECAM_POSE");

    g_case = "typed freecam-pose";
    CHECK(parse(&o, 3, good) == OPENPETE_EXIT_OK, "a well-formed --freecam-pose failed the parse");
    const char* e = getenv("OPENPETE_FREECAM_POSE");
    CHECK(e && !strcmp(e, good[2]), "OPENPETE_FREECAM_POSE = \"%s\", want the typed pose", e ? e : "");
    unsetenv_portable("OPENPETE_FREECAM_POSE");
    for (size_t k = 0; k < sizeof bad / sizeof bad[0]; k++) {
        g_case = bad[k][2];
        CHECK(parse(&o, 3, bad[k]) == OPENPETE_EXIT_SETUP_ERROR, "typed --freecam-pose \"%s\" parsed",
              bad[k][2]);
        e = getenv("OPENPETE_FREECAM_POSE");
        CHECK(!e || !*e, "a refused --freecam-pose set OPENPETE_FREECAM_POSE = \"%s\"", e ? e : "");
    }

    g_case = "config freecam_pose";
    CHECK(parse_cfg(&o, "freecam_pose = \"pos=(bogus\"\n", 0, NULL) == OPENPETE_EXIT_OK,
          "a refused config freecam_pose failed the boot parse");
    e = getenv("OPENPETE_FREECAM_POSE");
    CHECK(!e || !*e, "a refused config freecam_pose set OPENPETE_FREECAM_POSE = \"%s\"", e ? e : "");

    static const char* const none[] = { "--no-config" };
    g_case = "env freecam pose";
    setenv_portable("OPENPETE_FREECAM_POSE", "pos=(bogus");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK, "a refused OPENPETE_FREECAM_POSE failed the parse");
    e = getenv("OPENPETE_FREECAM_POSE");
    CHECK(!e || !*e, "a refused OPENPETE_FREECAM_POSE stayed set: \"%s\"", e ? e : "");
    setenv_portable("OPENPETE_FREECAM_POSE", "1,2,3");
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_OK, "a well-formed OPENPETE_FREECAM_POSE failed the parse");
    e = getenv("OPENPETE_FREECAM_POSE");
    CHECK(e && !strcmp(e, "1,2,3"), "a well-formed OPENPETE_FREECAM_POSE was dropped");

    g_case = "strict env freecam pose";
    setenv_portable("OPENPETE_FREECAM_POSE", "pos=(bogus");
    openpete_options_set_strict(1);
    CHECK(parse(&o, 1, none) == OPENPETE_EXIT_SETUP_ERROR, "strict mode kept a refused OPENPETE_FREECAM_POSE");
    openpete_options_set_strict(0);
    unsetenv_portable("OPENPETE_FREECAM_POSE");
}

int main(void) {
    scrub_inherited_env();
    const char* tmp = getenv("TMPDIR");
    snprintf(g_toml, sizeof g_toml, "%s/options_validate_test.toml",
             (tmp && *tmp) ? tmp : "/tmp");
    openpete_config_set_game_id("spyro-1");
    typed_refuses_config_falls_back();
    refusals_that_stand();
    env_spellings();
    window_values();
    resolved_env_fields();
    disc_cache_modes();
    effective_resolves_sentinels();
    tick_scale_range();
    mod_hot_reload_spellings();
    config_flag_beats_env();
    freecam_pose_values();
    remove(g_toml);
    if (g_fail) printf("options_validate_test: FAILED (%d)\n", g_fail);
    else        printf("options_validate_test: ok\n");
    return g_fail ? 1 : 0;
}
