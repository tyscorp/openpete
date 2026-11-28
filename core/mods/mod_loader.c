// core/mods/mod_loader.c - discover, build, and load mods.
//
// The mod layout:
//   mods/<id>/mod.toml     manifest ([mod] id/version/api)
//   mods/<id>/src/*.c      mod source, compiled on load
//   mods/enabled.txt       one mod id per line; order IS priority
//                          (later lines override earlier - S3AIR
//                          semantics). Missing file = all discovered
//                          mods, lexicographic order. --mods (env
//                          OPENPETE_MODS) overrides the file with a
//                          comma-separated id list, same semantics.
//
// Mods compile against <exe>/sdk/games/<game-id>/ (the running game's
// generated headers and openpete_game.h) before <exe>/sdk/ (the
// game-neutral openpete_mod_api.h, psx_runtime.h, ...), staged there by
// the build, into mods/<id>/.build/<game-id>/<id>.so, rebuilt when the
// CONTENT-HASH build key (api version + compiler identity + game id +
// compile flags + SDK files + sources) differs from the stamped one - mtime churn never
// forces a rebuild. An object records the game it was built for
// (openpete_mod_game); the loader refuses one that does not fit the
// running game (mod_check_game). Mods may also ship as <id>.zip
// (unpacked to .unpacked/<id>/, content-stamped; "folder or zip,
// identical"). The shared object is dlopen'd and its
// openpete_mod_entry(api, self) is called to register overrides.
//
// Failure posture: a mod that fails to parse/build/load is disabled with
// an error log (compile log at mods/<id>/.build/<game-id>/compile.log) - mods
// never brick a launch. Zero mods loaded leaves the engine byte-
// identical to a modless build (openpete_mods_active == 0).
//
// Platform (openpete-mod-compile-on-boot): every platform compiles
// src/ mods on load - Windows resolves the bundled llvm-mingw the
// distribution package ships (recomp/tools/llvm-mingw), falling back to
// a prebuilt object only when no compiler exists. A mod with NO src/ but
// a prebuilt object is a binary-only mod: loaded as-is, never hashed or
// compiled. Prebuilt objects are looked up in .build/<game-id>/, then
// .build/any/, then the legacy .build/<id>.{so,dll} (mod_find_prebuilt).
// openpete_mods_prepare() lets the game compile stale mods behind its
// splash (progress UI) before openpete_mods_init() loads them from the
// warmed cache.

#include "openpete_mods.h"
#include "openpete_modsave.h"
#include "mod_vec.h"
#include "openpete_osd.h"
#include "../host/texture_pack.h"   /* texpack_fog_shift */
#include "openpete_input_log.h"
#include "openpete_psx.h"   /* openpete_psx_resolve_disc_bin */
#include "openpete_disc_image.h"   /* read_sector */
#include "openpete_disc_trace.h"   /* OPENPETE_DISC_TAG_NONE */
#include "openpete_mod_api.h"
#include "openpete_mod_ui.h"
#include "openpete_log.h"
#include "openpete_config.h"      /* openpete_config_env: OPENPETE_CC */
#include "openpete_psxtrace.h" /* openpete_psxtrace_gate_host_enter: a host-initiated guest call */
#include "openpete_fiber.h"      /* openpete_speculative / openpete_replaying */
#include "openpete_host_input.h"
#include "openpete_host_mouse.h" /* mouse_read / mouse_capture */
#include "openpete_bugrec.h"
#include "openpete_rumble.h"   /* pad_rumble: the one byte-to-motor path */
#include "openpete_keymap.h"     /* [keys.mod.<id>] binding overrides */
#include "game_extras.h"      /* game_get_id: [mod] game targeting */
#include "mod_games.h"        /* requires_games / optional_games */
#include "openpete_identify.h" /* registered game ids */
#include "openpete_launcher.h" /* the library's disc of another game */
#include "../ui/openpete_imgui_guard.h" /* ImGui table bind */
#include "../host/sha256.h"     /* content-hash build cache key */
#include "../host/host_spawn.h" /* silent compiler spawn + cc identity */
#include "openpete_atomic_file.h" /* the unpacked zip's stamp */
#include "miniz.h"              /* zip mods (folder or zip, identical) */
#include <SDL3/SDL.h>            /* SDL_OpenURL, SDL_CreateDirectory: the mods folder */

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include <dirent.h> /* MinGW provides it too */
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MOD_DL_EXT ".dll"
#define mod_mkdir(p) _mkdir(p)
/** @brief Load a mod's shared object with LoadLibraryA.
 *
 * @param path Path of the .dll.
 * @return The module handle, or NULL on failure; mod_dlerror describes it. */
static void* mod_dlopen(const char* path) {
    return (void*)LoadLibraryA(path);
}
/** @brief Look up an exported symbol in a loaded mod object with GetProcAddress.
 *
 * The function pointer converts to void* through a union.
 * @param h Handle from mod_dlopen.
 * @param name Exported symbol name.
 * @return The symbol address, or NULL when the object does not export it. */
static void* mod_dlsym(void* h, const char* name) {
    union { FARPROC fp; void* p; } u;
    u.fp = GetProcAddress((HMODULE)h, name);
    return u.p;
}
/** @brief Describe the last LoadLibrary/GetProcAddress failure.
 *
 * Formats GetLastError() into a static buffer, so the text is overwritten
 * by the next call and the function is not thread-safe.
 * @return The message, never NULL. */
static const char* mod_dlerror(void) {
    static char buf[64];
    snprintf(buf, sizeof buf, "LoadLibrary/GetProcAddress error %lu",
             (unsigned long)GetLastError());
    return buf;
}
#else
#include <dlfcn.h>
#include <unistd.h> /* rmdir */
#define MOD_DL_EXT ".so"
#define mod_mkdir(p) mkdir((p), 0755)
/** @brief Load a mod's shared object with dlopen, binding every symbol now and keeping them local.
 *
 * @param path Path of the .so.
 * @return The handle, or NULL on failure; mod_dlerror describes it. */
static void* mod_dlopen(const char* path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
/** @brief Look up an exported symbol in a loaded mod object with dlsym.
 *
 * @param h Handle from mod_dlopen.
 * @param name Exported symbol name.
 * @return The symbol address, or NULL when the object does not export it. */
static void* mod_dlsym(void* h, const char* name) { return dlsym(h, name); }
/** @brief Return dlerror()'s text for the last failed dlopen or dlsym on this thread.
 *
 * @return The libc-owned message, or NULL when no error is pending. */
static const char* mod_dlerror(void) { return dlerror(); }
#endif

#include <SDL3/SDL.h> /* SDL_GetBasePath - shipped sdk/ + toolchain */
#include "openpete_paths.h" /* mods/ lives with the writable user data */

#include "openpete_psx.h"   /* openpete_psx_get_ram - api->guest() */
#include "openpete_state.h" /* savestate slots - engine services */

#include "../../third_party/tomlc99/toml.h"

/* mod_chain.c internals shared within core/mods. */
int  openpete_mods_chain_add(uint32_t addr, openpete_mod_override_fn fn,
                             const char* owner_id);
int  openpete_mods_chain_add_ovl(uint32_t addr, uint32_t overlay,
                                 openpete_mod_override_fn fn,
                                 const char* owner_id);

/* The first api whose manifests are read on the overlay-scoped rules: the
 * api that adds override_ovl. A manifest pinned below it, or with no pin,
 * was written against the Spyro 1 SDK. */
#define MOD_API_SCOPED 13
/* The first api whose materials compose and whose [[material]] rows read
 * the exclusive key; mod_material_resolve.h's MOD_MAT_API_COMPOSE. */
#define MOD_API_COMPOSE 13
/* The first api whose [[material]] env_anim = false selects the static
 * terrain faces (OP_SEL_ENV_STATIC); below it false narrows nothing. */
#define MOD_API_ENV_STATIC 13
void openpete_mods_base(CPUState* cpu);

/* Mods the live table's first block holds; it grows by whole blocks. */
#define MODS_BLOCK        64
#define MOD_ID_MAX        64
_Static_assert(MOD_ID_MAX == OPENPETE_ARENA_OWNER_MAX,
               "arena ledger owner field must hold a full mod id");
#define MOD_BINDINGS_INIT 8

typedef struct {
    char name[24];    /* binding_down() key within the mod's namespace */
    char key[24];     /* host key name (host_key_down vocabulary) */
    char purpose[64]; /* human text for conflict messages / mod menu */
} mod_binding_t;

/* One [[config]] schema row from mod.toml - what the engine renders in
 * the Mods panel for this mod. Values themselves live in config.toml. */
/* Rows a [[config]] table declares, and the choices a string row offers.
 * Both are the mod author's content, so both grow; the constants are the
 * initial allocation. */
#define MOD_CFG_ROWS_INIT  32
#define MOD_CFG_ITEMS_INIT 8
/* One combo choice. A fixed-width element keeps the row's storage one
 * allocation rather than one per string. */
typedef struct { char s[24]; } mod_cfg_item_t;
typedef struct {
    char    key[96];   /* dotted config path */
    char    type;      /* 'b' bool, 'i' int, 'd' float, 's' string */
    char    label[64];
    char    group[32];
    char    help[128];
    double  min, max;  /* numeric range; min >= max = free input */
    mod_fvec items;    /* mod_cfg_item_t; the combo choices, in order */
    int64_t def_i;     /* default: bool/int */
    double  def_d;     /* default: float */
    char    def_s[64]; /* default: string */
    int     secret;    /* `secret = true`: the value never leaves the machine in a bug report */
} mod_cfg_row_t;
/* One [[material]] row from mod.toml - the zero-C material tier. Holds
 * exactly what `material_register` takes, in manifest spelling, so a mod
 * that only wants "reshade this channel with this frag" needs no C at
 * all. Rows register BEFORE openpete_mod_entry, so a mixed mod's C code
 * registers later and therefore wins (last match takes the pixels): the
 * manifest states the default, code refines it. */
#define MOD_MATERIAL_ROWS_INIT 16
#define MOD_MATERIAL_PARAMS_MAX 64   /* floats; capped again by the ABI's
                                      * OPENPETE_MATERIAL_UNIFORMS_MAX */
/** @brief Map a manifest channel spelling to its OP_CHAN_* value.
 *
 * "any" maps to -1, and the caller leaves an absent key at -1. An
 * unrecognised name returns -2 so the caller refuses the row instead of
 * widening it to every channel. The match is exact and case-sensitive.
 * @param s the channel spelling.
 * @return OP_CHAN_*, -1 for "any", or -2 for an unknown name. */
static int mod_channel_from_name(const char* s) {
    static const struct { const char* name; int chan; } k[] = {
        { "any",      -1 },              { "moby",     OP_CHAN_MOBY },
        { "terrain",  OP_CHAN_TERRAIN }, { "player",   OP_CHAN_PLAYER },
        { "flame",    OP_CHAN_FLAME },   { "particle", OP_CHAN_PARTICLE },
        { "sky",      OP_CHAN_SKY },     { "shadow",   OP_CHAN_SHADOW },
        { "tracer",   OP_CHAN_TRACER },  { "portal",   OP_CHAN_PORTAL },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (strcmp(s, k[i].name) == 0) return k[i].chan;
    return -2;
}

typedef struct {
    char  frag[128];   /* "shaders/<name>.frag" - relative to the mod dir */
    char  vert[128];   /* optional "shaders/<name>.vert" */
    int   channel;     /* OP_CHAN_*; -1 = any */
    int   ident, moby_class, texture, level;   /* -1 = any */
    unsigned flags;    /* OP_SEL_* narrowing bits */
    float params[MOD_MATERIAL_PARAMS_MAX];
    int   n_params;
    int   enabled;     /* [[material]] enabled = false starts it off */
} mod_material_row_t;
/* [[level]] blocks a mod declares. The process-global claimable-slot
 * count is a separate bound (LEVEL_SLOTS_MAX in mod_registry.c). */
#define MOD_LEVELS_INIT 4
/* One [[level]] block: a declarative custom level. */
typedef struct {
    char name[64]; char files[64]; char donor[24]; int replace;
    char music[64]; char title[40];
    /* Target totals; -1 = the block declared none, leaving that table on
     * its stock row. */
    int dragons; int gems; int eggs;
} mod_level_row_t;

#define MOD_PATH_MAX      1024
/* One src/<n>.c path. The list is heap-held and has no fixed count: the
 * sources reach the compiler through a response file, so the cmd.exe
 * line-length limit does not bound it. */
typedef struct { char path[MOD_PATH_MAX]; } mod_src_t;
#define MOD_SRCS_INIT     64
#define MOD_DEPS_INIT     8
/* One declared dependency id. */
typedef struct { char id[MOD_ID_MAX]; } mod_dep_t;

/* One override-order event of a mod (openpete_modset_event_t without the
 * id), stamped with its place in the session's sequence. */
typedef struct {
    int      seq;     /* 1-based, unique across every mod */
    int      kind;    /* openpete_modset_event_kind_t */
    uint32_t addr;    /* LINK: the chain's guest vaddr */
    uint32_t overlay; /* LINK: the link's overlay binding */
} mod_order_ev_t;

struct openpete_mod;
static void mod_order_note(struct openpete_mod* mod, int kind, uint32_t addr,
                           uint32_t overlay);

struct openpete_mod {
    int   load_index;    /* position in the live table, set at the copy
                          * into it; the table is chunked, so the index
                          * cannot be recovered by pointer subtraction */
    char  id[MOD_ID_MAX];
    char  version[32];
    char  name[64];      /* manifest display name (fallback: id) */
    char  dir[MOD_PATH_MAX];
    char  data_dir[MOD_PATH_MAX + 8]; /* "<dir>/data", made on demand */
    char  assets_dir[MOD_PATH_MAX + 48]; /* "<dir>/assets/<game id>" when
                                          * that directory exists, else
                                          * "<dir>/assets"; set at load */
    char  log_tag[MOD_ID_MAX + 8]; /* "mod:<id>" */
    mod_fvec deps;       /* mod_dep_t */
    mod_game_list_t games; /* [mod] requires_games and optional_games */
    int   disc_refused_logged; /* a disc call for an unlisted game was
                                * logged (once per mod) */
    int   raw_warned;  /* raw-tier warning issued (once per mod) */
    uint32_t api_pin;  /* [mod] api, the minimum api the mod's calls need;
                        * 0 when the key is absent. Below MOD_API_SCOPED
                        * it selects the unscoped overlay-window path. */
    int   unscoped_warned; /* unscoped overlay-window deprecation issued
                            * (once per mod, apart from raw_warned) */
    char  state_decl;  /* manifest state: 0 undeclared, 'n' none,
                        * 'r' rebuildable (host state re-derives from
                        * guest RAM) - gates savestate/rewind arming */
    char  ra_decl;     /* manifest runahead: 0 undeclared, 'p' pure,
                        * 'h' host-fx - gates speculative re-execution */
    int   sim_mutative; /* [mod] sim_mutative: -1 undeclared (the engine
                        * derives it), 0 the mod never writes guest RAM,
                        * 1 it does. Drives the save modset tag. */
    mod_fvec bindings;   /* mod_binding_t; declared host keys */
    /* mouse_read markers, one set per hook kind (0 tick/entry, 1 present):
     * the totals this mod last consumed from that cadence. */
    struct {
        uint64_t press[OPENPETE_MOUSE_BUTTONS];
        int64_t  mx, my, wheel;
        int      valid;
    } mouse_mark[2];
    /* pad_read reports its two quiet outcomes once per mod, not per tick:
     * bit per slot for an absent slot, and one flag for a present-hook
     * call, which an always-on UI section would otherwise repeat every
     * present. */
    unsigned pad_absent_logged;
    int      pad_present_logged;
    /* Slots this mod has published a rumble pair for, so disabling or
     * unloading it silences exactly those and nothing else. */
    unsigned pad_rumble_slots;
    int      pad_rumble_slot0_logged;
    mod_fvec cfg_rows;   /* mod_cfg_row_t; [[config]] schema */
    mod_fvec mat_rows;   /* mod_material_row_t; [[material]] */
    int   cfg_reload_pending; /* Apply wrote config.toml; reload at the
                               * next tick boundary re-reads it */
    int   reload_deferred; /* a hot reload is requested and waits: the
                            * mod's code is on the frame stack, or a
                            * level-ready hook walk is parked */
    /* [[level]] blocks: declarative custom levels (registered with the
     * content registry at load; the engine owns slot/TOC/warp). */
    mod_fvec levels;     /* mod_level_row_t */
    char  build_key[65]; /* content-hash build key (hot-reload watch);
                          * empty for a prebuilt object loaded without
                          * the SDK */
    char  obj_path[MOD_PATH_MAX]; /* the object dl was opened from; a
                          * reload with an unchanged build key copies
                          * it instead of compiling */
    int   obj_kind;      /* mod_obj_kind_t of obj_path: the lookup path
                          * the object came from, which decides the game
                          * identity it must carry */
    int   game_any;      /* the object recorded openpete_mod_game "*":
                          * built for every game, so a raw address or a
                          * func_ name may mean another function here */
    char  cfg_key[65];   /* config.toml content hash (hot-reload watch;
                          * a config edit re-enters entry without a
                          * recompile - the cached object is reused) */
    char  shader_key[65];/* <mod>/shaders/ content hash (hot-reload watch;
                          * a shader-only edit reloads against the cached
                          * C object - the per-shader .spv stamps decide
                          * what actually recompiles) */
    int   has_src;       /* 0 = binary-only/assets-only: never compiled,
                          * skipped by the hot-reload watch */
    /* Live-toggle state. Atomic because an always section makes the
     * compose thread read it on every present while the tick boundary
     * writes it. */
    _Atomic int enabled;
    int   boot_on;       /* in the boot enabled set (vs a deferred-entry
                          * load-all row, which starts disabled) */
    int   entered;       /* openpete_mod_entry has run; load-all rows
                          * defer it to their first enable so a
                          * boot-disabled mod runs zero code */
    int   in_entry;      /* its entry is running: links it registers are
                          * implied by the entry's order event */
    mod_fvec order_ev;   /* mod_order_ev_t; its override-order events
                          * (mod_order_note), in session order */
    int   has_assets;    /* assets/ overlay root exists (boot-scoped) */
    /* Set by a deferred entry on the guest thread, taken by the renderer's
     * next compose (openpete_mods_take_entered), which queues the mod's
     * pipelines. */
    _Atomic int late_entered;
    /* Atomic for the same reason as enabled, plus hot reload: the swap
     * happens on the sim thread while the compose thread may be about to
     * call through the pointer. The old object stays resident, so a body
     * already running finishes against valid code. */
    _Atomic openpete_mod_ui_body_fn ui_body; /* UI section descriptor body
                                      * (openpete_mod_ui_section symbol,
                                      * resolved at dlopen, ABI-checked;
                                      * NULL = none) */
    int   ui_always;     /* OPENPETE_MOD_UI_ALWAYS: the body runs from the
                          * overlay's top-level always pass on every
                          * present instead of inside the mod's node */
    /* ui_status line block: accumulates within a tick, lazily reset on
     * the mod's first call in a NEW tick - a one-shot line printed at
     * entry or on a level event persists until repainted. */
    char     status[24][120];
    int      status_n;
    int      status_trunc;
    unsigned status_tick;
    openpete_mod_toggle_fn toggle_fn; /* live-toggle notification */
    openpete_mod_shutdown_fn shutdown_fn; /* exit pass and hot reload */
    /* Set when the exit pass has run this mod's shutdown hook (or found
     * none): from then on every api entry the mod calls is refused. Read
     * by the mod's own threads, hence atomic. */
    _Atomic int api_closed;
    _Atomic int api_refuse_logged; /* the first refusal has been logged */
    uint64_t imgui_calls;  /* ImGui calls the last body made */
    int      imgui_engaged;/* the first non-zero count has been logged */
    void* dl;
};

/** @brief Count a mod's dependency rows.
 *
 * This and the accessors below reach each per-mod table, a mod_fvec owned
 * by the struct, by index, never by a held address (the getter that hands
 * a row out copies what the caller needs). */
static int   mod_dep_count(const struct openpete_mod* m) {
    return (int)mod_fvec_count(&m->deps);
}
/// @brief Return the mod's dependency row at index i, or NULL when i is out of range.
static mod_dep_t* mod_dep_at(const struct openpete_mod* m, int i) {
    return (mod_dep_t*)mod_fvec_at(&m->deps, (size_t)i);
}
/// @brief Count a mod's [[binding]] rows.
static int   mod_binding_count(const struct openpete_mod* m) {
    return (int)mod_fvec_count(&m->bindings);
}
/// @brief Return the mod's binding row at index i, or NULL when i is out of range.
static mod_binding_t* mod_binding_at(const struct openpete_mod* m, int i) {
    return (mod_binding_t*)mod_fvec_at(&m->bindings, (size_t)i);
}
/// @brief Count a mod's config rows.
static int   mod_cfg_count(const struct openpete_mod* m) {
    return (int)mod_fvec_count(&m->cfg_rows);
}
/// @brief Return the mod's config row at index i, or NULL when i is out of range.
static mod_cfg_row_t* mod_cfg_at(const struct openpete_mod* m, int i) {
    return (mod_cfg_row_t*)mod_fvec_at(&m->cfg_rows, (size_t)i);
}
/** @brief Count a mod's declarative [[material]] rows. */
static int   mod_mat_count(const struct openpete_mod* m) {
    return (int)mod_fvec_count(&m->mat_rows);
}
/** @brief Return row i of a mod's material table, or NULL at or past the count. */
static mod_material_row_t* mod_mat_at(const struct openpete_mod* m, int i) {
    return (mod_material_row_t*)mod_fvec_at(&m->mat_rows, (size_t)i);
}
/** @brief Count a mod's [[level]] rows. */
static int   mod_level_count(const struct openpete_mod* m) {
    return (int)mod_fvec_count(&m->levels);
}
/** @brief Return row i of a mod's level table, or NULL at or past the count. */
static mod_level_row_t* mod_level_at(const struct openpete_mod* m, int i) {
    return (mod_level_row_t*)mod_fvec_at(&m->levels, (size_t)i);
}
/// @brief Count the enumeration items of a config row.
static int   mod_cfg_item_count(const mod_cfg_row_t* r) {
    return (int)mod_fvec_count(&r->items);
}
/// @brief Return item i of a config row, or the empty string when i is out of range.
static const char* mod_cfg_item_at(const mod_cfg_row_t* r, int i) {
    const mod_cfg_item_t* it =
        (const mod_cfg_item_t*)mod_fvec_at(&r->items, (size_t)i);
    return it ? it->s : "";
}

/** @brief Release every heap row a mod owns: config rows and their items, deps, bindings, material rows and level rows.
 *
 * There is no unload path, so this runs only on a slot that is being
 * dropped or reused: the key-defaults scratch slot, a pending slot after a
 * failed parse or a failed dependency, a live row whose load_mod failed,
 * and the sweep over the pending rows after the transfer. A disowned
 * slot's empty vec headers make it a no-op. */
static void mod_rows_free(struct openpete_mod* m) {
    for (int i = 0; i < mod_cfg_count(m); i++)
        mod_fvec_free(&mod_cfg_at(m, i)->items);
    mod_fvec_free(&m->cfg_rows);
    mod_fvec_free(&m->deps);
    mod_fvec_free(&m->bindings);
    mod_fvec_free(&m->mat_rows);
    mod_fvec_free(&m->levels);
    mod_fvec_free(&m->order_ev);
}

/** @brief Give up a mod's row ownership without freeing, after the rows moved to another struct.
 *
 * Zeroes the vec headers so a later mod_rows_free over this slot is a
 * no-op rather than a double free. */
static void mod_rows_disown(struct openpete_mod* m) {
    memset(&m->deps, 0, sizeof m->deps);
    memset(&m->bindings, 0, sizeof m->bindings);
    memset(&m->cfg_rows, 0, sizeof m->cfg_rows);
    memset(&m->mat_rows, 0, sizeof m->mat_rows);
    memset(&m->levels, 0, sizeof m->levels);
    memset(&m->order_ev, 0, sizeof m->order_ev);
}

/** @brief Drop every row of a mod, keeping the table storage, for a re-parse in place.
 *
 * Config rows' item lists are freed; the tables themselves are cleared.
 * mod_hot_reload calls this before parse_manifest so the re-parse does
 * not append duplicates. */
static void mod_rows_clear(struct openpete_mod* m) {
    for (int i = 0; i < mod_cfg_count(m); i++)
        mod_fvec_free(&mod_cfg_at(m, i)->items);
    mod_fvec_clear(&m->cfg_rows);
    mod_fvec_clear(&m->deps);
    mod_fvec_clear(&m->bindings);
    mod_fvec_clear(&m->mat_rows);
    mod_fvec_clear(&m->levels);
}

/* Chunked: a mod holds its own openpete_mod_t* for the session and the
 * registries keep mod->id pointers, so an element address must outlive
 * every later load. */
static mod_cvec s_mods;   /* struct openpete_mod */

/// @brief Return the loaded mod at index i in load order, or NULL when i is out of range.
static struct openpete_mod* mod_at(int i) {
    return (struct openpete_mod*)mod_cvec_at(&s_mods, (size_t)i);
}
/// @brief Count the mods in the live table.
static int mod_count(void) { return (int)mod_cvec_count(&s_mods); }

/* Rows of the loader's working lists. Each list is a mod_fvec sized by
 * the mods directory, so nothing about the boot path is bounded by a
 * constant. */
typedef struct { char id[MOD_ID_MAX]; } mod_id_row_t;
typedef struct { char k[65]; } mod_key_row_t;

/// @brief Return the id text of row i of an id list, which must be in range.
///
/// The list holds mod_id_row_t rows. An out-of-range i dereferences NULL, so
/// every caller bounds i by the list count first.
static char* id_row(const mod_fvec* v, int i) {
    return ((mod_id_row_t*)mod_fvec_at(v, (size_t)i))->id;
}
/// @brief Return row i of an int list, or 0 when i is out of range.
static int int_row(const mod_fvec* v, int i) {
    const int* p = (const int*)mod_fvec_at(v, (size_t)i);
    return p ? *p : 0;
}
/// @brief Store x in row i of an int list; an out-of-range i is ignored.
static void int_row_set(mod_fvec* v, int i, int x) {
    int* p = (int*)mod_fvec_at(v, (size_t)i);
    if (p) *p = x;
}
/** @brief Return row i of the pending-mod table, or NULL at or past the count.
 *
 * The load-order callers index only below their own row count and
 * dereference the result without a check. */
static struct openpete_mod* pend_at(const mod_fvec* v, int i) {
    return (struct openpete_mod*)mod_fvec_at(v, (size_t)i);
}
static unsigned            s_toggle_epoch; /* defined with the queue below */

/* Where the enabled set came from, so "fix your load order" messages
 * name the list the user actually controls: the mods dir's enabled.txt,
 * or the --mods override that replaced it. Set by mods_discover. */
static const char* s_order_src = "enabled.txt";

/* First api whose struct layout every later api preserves. */
#define MOD_ABI_LAYOUT_FLOOR 11u

/** @brief Read a loaded object's ABI descriptor and refuse one the engine cannot host.
 *
 * Refused: no descriptor (an object from before the layout freeze), an api
 * older than MOD_ABI_LAYOUT_FLOOR (entries before it moved, so its calls
 * would land on the wrong slots), a newer api than the engine's, and a
 * larger api struct (calls would index past the engine's table). Older
 * descriptored objects load, since the struct is append-only from the
 * floor. It runs on the engine-compiled path, which the SDK stub always
 * satisfies, and on the prebuilt-object path, which compile-on-load cannot
 * rebuild and which is the path it exists for. Each refusal logs an error.
 * @param dl The dlopen handle.
 * @param id Mod id for the log lines.
 * @param path Object path for the log lines.
 * @return 0 when loadable, -1 when refused. */
static int mod_check_abi(void* dl, const char* id, const char* path) {
    const openpete_mod_abi_t* d =
        (const openpete_mod_abi_t*)mod_dlsym(dl, "openpete_mod_abi");
    if (!d || d->magic != OPENPETE_MOD_ABI_MAGIC) {
        OPL_ERROR_F("mods", "%s: %s carries no ABI descriptor - it was "
                    "built without the SDK's openpete_mod_abi.c (add "
                    "<sdk>/openpete_mod_abi.c to the compile line, or let "
                    "the engine compile the mod from src/) - disabled",
                    id, path);
        return -1;
    }
    if (d->api_version < MOD_ABI_LAYOUT_FLOOR) {
        OPL_ERROR_F("mods", "%s: %s was built for api %u, but the api "
                    "struct layout changed at api %u (entries before it "
                    "moved); rebuild the mod against this SDK - disabled",
                    id, path, d->api_version, MOD_ABI_LAYOUT_FLOOR);
        return -1;
    }
    if (d->api_version > OPENPETE_MOD_API_VERSION ||
        d->api_size > (uint32_t)sizeof(openpete_mod_api_t)) {
        OPL_ERROR_F("mods", "%s: %s was built for api %u (struct %u bytes) "
                    "but this engine provides api %u (%u bytes) - a newer "
                    "object cannot load into an older engine; update "
                    "openpete or rebuild the mod against this SDK - "
                    "disabled", id, path, d->api_version, d->api_size,
                    OPENPETE_MOD_API_VERSION,
                    (unsigned)sizeof(openpete_mod_api_t));
        return -1;
    }
    return 0;
}

/** @brief Remove every registration a mod owns and reset its host-side input state.
 *
 * Releases its mouse capture request, clears its mouse markers and pad log
 * latches, stops the rumble slots it drove, removes its chain links,
 * present hooks, registry rows, postfx passes, materials and shaders (postfx
 * before shaders, which retires the pipelines), and clears its UI body and
 * toggle hook, and records a STRIP override-order event. Runs before a
 * reload re-enters the mod, and after a failed entry: an entry that
 * registered and then returned nonzero must not leave its registrations
 * live under an id the loader has declared disabled. */
static void mod_strip_registrations(struct openpete_mod* mod) {
    mod_order_note(mod, OPENPETE_MODSET_EV_STRIP, 0, 0);
    openpete_host_mouse_request_capture(
        OPENPETE_MOUSE_CAP_MOD0 + mod->load_index, 0);
    memset(mod->mouse_mark, 0, sizeof mod->mouse_mark);
    mod->pad_absent_logged  = 0;
    mod->pad_present_logged = 0;
    /* A mod that goes away stops its motors. The pair is level-triggered, so
     * without this the last one it published would keep being re-driven. */
    for (int sl = 1; sl < OPENPETE_MOD_PAD_SLOTS; sl++)
        if (mod->pad_rumble_slots & (1u << sl))
            openpete_rumble_mod_clear(sl);
    mod->pad_rumble_slots = 0;
    mod->pad_rumble_slot0_logged = 0;
    openpete_mods_chain_remove_owner(mod->id);
    openpete_mods_draw_remove_owner(mod->id);
    openpete_mods_registry_remove_owner(mod->id);
    openpete_mods_postfx_remove_owner(mod->id);  /* before shaders: retires pipelines */
    openpete_mods_material_remove_owner(mod->id);
    openpete_mods_shader_remove_owner(mod->id);
    mod->ui_body   = NULL;
    mod->ui_always = 0;
    mod->toggle_fn = NULL;
    mod->shutdown_fn = NULL;
    openpete_modsave_remove_owner(mod->id);
}

/** @brief Resolve a mod object's UI section descriptor into its ui_body and ui_always fields.
 *
 * See openpete_mod_ui.h. Clears both fields first; an object without the
 * descriptor leaves them clear. The magic and abi check runs at load so a
 * prebuilt object, which compile-on-load cannot rebuild, fails loudly
 * instead of calling through a stale vtable shape: on a mismatch or a NULL
 * body the section is refused with an error line and the mod itself keeps
 * running. */
static void mod_resolve_ui_section(struct openpete_mod* mod, void* dl) {
    mod->ui_body = NULL;
    mod->ui_always = 0;
    const openpete_mod_ui_section_t* d = (const openpete_mod_ui_section_t*)
        mod_dlsym(dl, "openpete_mod_ui_section");
    if (!d) return;
    if (d->magic != 0x4F505549u || d->abi != OPENPETE_MOD_UI_ABI ||
        !d->body) {
        OPL_ERROR_F("mods", "%s: UI section ABI mismatch (mod built for "
                    "abi %u, engine has %u) - section refused; the mod "
                    "itself still runs. Rebuild the mod against the "
                    "current SDK.", mod->id,
                    d->magic == 0x4F505549u ? d->abi : 0u,
                    (unsigned)OPENPETE_MOD_UI_ABI);
        return;
    }
    mod->ui_body = d->body;
    mod->ui_always = (d->flags & OPENPETE_MOD_UI_ALWAYS) ? 1 : 0;
}

/** @brief Bind a mod object's ImGui table (openpete_imgui.h) before any of its code runs.
 *
 * No descriptor: the mod did not opt in and nothing is bound. A descriptor
 * that openpete_imgui_bind rejects disables the mod the way a failed abi
 * check does; a section-only refusal would leave a live mod holding a null
 * table, and its first ImGui_* call from any hook would dereference it.
 * Each hot-reload object binds its own copy, since the pointer lives in
 * the object.
 * @return 0 when bound or not opted in, -1 when the descriptor is refused. */
static int mod_resolve_imgui(struct openpete_mod* mod, void* dl,
                                   const char* path) {
    const void* d = mod_dlsym(dl, "openpete_imgui_desc");
    if (!d) return 0;
    return openpete_imgui_bind(d, mod->id, path) ? 0 : -1;
}

/* The registered game descriptor; s_desc_none (every member NULL) stands
 * in until one is registered. s_mods_init_ran closes registration once the
 * loader has compiled or loaded anything. */
static const game_mod_desc_t  s_desc_none;
static const game_mod_desc_t* s_desc = NULL;
static int                    s_mods_init_ran = 0;

int openpete_mods_set_game_desc(const game_mod_desc_t* desc) {
    if (!desc) {
        OPL_ERROR_F("mods", "game descriptor for %s: NULL - refused",
                    game_get_id());
        return -1;
    }
    if (s_desc) {
        OPL_ERROR_F("mods", "game descriptor for %s: already registered - "
                    "second registration refused", game_get_id());
        return -1;
    }
    if (s_mods_init_ran) {
        OPL_ERROR_F("mods", "game descriptor for %s: registered after mod "
                    "init - refused", game_get_id());
        return -1;
    }
    s_desc = desc;
    return 0;
}

const game_mod_desc_t* openpete_mods_game_desc(void) {
    return s_desc ? s_desc : &s_desc_none;
}

/* The game id --compile-mods is compiling for; empty outside that mode. */
static char s_build_game[32];

/** @brief Return the game mods are discovered and compiled for.
 *
 * The staged game --compile-mods is working through, else the running
 * game. Object, stamp and compile-log paths, the SDK resolution and build
 * key, and the [mod] game filter all read it; loading an object checks it
 * against the running game (mod_check_game).
 * @return The game id. */
static const char* mods_build_game(void) {
    return s_build_game[0] ? s_build_game : game_get_id();
}

/* Bound on addr minus the symbol openpete_mods_symbol_for_addr names. */
#define SYM_ADDR_SPAN_MAX 0x10000u

const char* openpete_mods_symbol_for_addr(uint32_t addr, uint32_t* off_out) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    const openpete_sdk_symbol_t* best = NULL;
    if (!addr || !gd->symbols) return NULL;
    for (size_t i = 0; i < gd->symbol_count; i++) {
        const openpete_sdk_symbol_t* c = &gd->symbols[i];
        if (c->addr > addr) continue;
        if (!best || c->addr > best->addr ||
            (c->addr == best->addr && !c->legacy && best->legacy))
            best = c;
    }
    if (!best || addr - best->addr >= SYM_ADDR_SPAN_MAX) return NULL;
    if (off_out) *off_out = addr - best->addr;
    return best->name;
}

/* ---- mod-facing API impl ------------------------------------------- */

/** @brief Find the SDK symbol rows of an exact name with a binary search.
 *
 * The generator sorts the table by strcmp on name, then overlay id, and
 * keeps one row per (name, overlay), so a name whose rows lie in several
 * overlays occupies adjacent rows; the search finds the first.
 * @param name Symbol name; NULL returns NULL.
 * @param count Receives the number of rows; may be NULL.
 * @return The first row, or NULL (count 0) when absent or no table is
 *         registered. */
static const openpete_sdk_symbol_t* sym_find_range(const char* name,
                                                   size_t* count) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    if (count) *count = 0;
    if (!name || !gd->symbols) return NULL;
    size_t lo = 0, hi = gd->symbol_count;
    while (lo < hi) { /* lower bound: first row not below name */
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(gd->symbols[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    size_t n = 0;
    while (lo + n < gd->symbol_count && strcmp(gd->symbols[lo + n].name, name) == 0)
        n++;
    if (!n) return NULL;
    if (count) *count = n;
    return &gd->symbols[lo];
}

/** @brief Find an SDK symbol by exact name: the first of its rows (sym_find_range).
 *
 * @param name Symbol name; NULL returns NULL.
 * @return The table row, or NULL when absent or no table is registered. */
static const openpete_sdk_symbol_t* sym_find(const char* name) {
    return sym_find_range(name, NULL);
}

/* ---- overlay-scoped overrides ----------------------------------------
 * Every overlay of a game loads at one vaddr window, so an address inside
 * it names one function per overlay that has an entry there. A link binds
 * (address, overlay): override_ovl names the overlay, a name resolves to
 * its row's overlay, and only a mod pinned below MOD_API_SCOPED on a game
 * that allows it (game_mod_desc_t.legacy_window_links) binds an unscoped
 * link that fires in every overlay. */

/** @brief Return 1 when addr lies in the registered game's overlay window. */
static int in_overlay_window(uint32_t addr) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    return addr >= gd->window_lo && addr < gd->window_hi;
}

/** @brief Find an overlay of the registered game by its overlay table id.
 *
 * @param id The overlay id.
 * @return The overlay, or NULL when the game has none with that id. */
static const game_mod_overlay_t* overlay_by_id(uint32_t id) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    for (size_t i = 0; i < gd->overlay_count; i++)
        if (gd->overlays[i].id == id) return &gd->overlays[i];
    return NULL;
}

/** @brief Report whether addr is a function entry of an overlay, by binary search of its sorted entry array.
 *
 * @param o The overlay.
 * @param addr The guest vaddr.
 * @return 1 when addr is an entry, else 0. */
static int overlay_has_entry(const game_mod_overlay_t* o, uint32_t addr) {
    size_t lo = 0, hi = o->entry_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (o->entries[mid] == addr) return 1;
        if (o->entries[mid] < addr) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/** @brief Rank a symbol row for listing: the stable name, then func_<overlay>_<addr>, then legacy raw aliases. */
static int sym_rank(const openpete_sdk_symbol_t* r) {
    if (r->legacy) return 2;
    return strncmp(r->name, "func_", 5) == 0 ? 1 : 0;
}

/** @brief Append to buf every SDK name of (addr, overlay), stable name first.
 *
 * @param buf The text being built.
 * @param cap Size of buf.
 * @param used Bytes already in buf.
 * @param addr The guest vaddr.
 * @param overlay The overlay id.
 * @return The new length, at most cap - 1. */
static size_t append_names_at(char* buf, size_t cap, size_t used,
                              uint32_t addr, uint32_t overlay) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    int first = 1;
    for (int rank = 0; rank < 3; rank++)
        for (size_t i = 0; gd->symbols && i < gd->symbol_count; i++) {
            const openpete_sdk_symbol_t* r = &gd->symbols[i];
            if (r->addr != addr || r->overlay != overlay || sym_rank(r) != rank)
                continue;
            int w = snprintf(buf + used, cap - used, "%s%s",
                             first ? " (" : ", ", r->name);
            if (w < 0 || (size_t)w >= cap - used) return cap - 1;
            used += (size_t)w;
            first = 0;
        }
    if (!first) {
        int w = snprintf(buf + used, cap - used, ")");
        if (w < 0 || (size_t)w >= cap - used) return cap - 1;
        used += (size_t)w;
    }
    return used;
}

/** @brief List every overlay where addr is a function entry or holds an SDK row, with that overlay's names for it.
 *
 * Form: "level_15 (Flight1_level_15, func_level_15_8007AE08); level_25
 * (...)", in overlay table order, truncated to the buffer.
 * @param buf Receives the text.
 * @param cap Size of buf.
 * @param addr The guest vaddr. */
static void list_overlays_at(char* buf, size_t cap, uint32_t addr) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    size_t used = 0;
    buf[0] = 0;
    for (size_t k = 0; k < gd->overlay_count && used < cap - 1; k++) {
        const game_mod_overlay_t* o = &gd->overlays[k];
        int has = overlay_has_entry(o, addr);
        for (size_t i = 0; !has && gd->symbols && i < gd->symbol_count; i++)
            has = gd->symbols[i].addr == addr && gd->symbols[i].overlay == o->id;
        if (!has) continue;
        int w = snprintf(buf + used, cap - used, "%s%s", used ? "; " : "",
                         o->name);
        if (w < 0 || (size_t)w >= cap - used) { used = cap - 1; break; }
        used = append_names_at(buf, cap, used + (size_t)w, addr, o->id);
    }
    if (!buf[0]) snprintf(buf, cap, "no overlay");
}

/** @brief Refuse an overlay-window binding that names no single overlay, logging every overlay it occurs in.
 *
 * Names the mod (log tag), the call and target, the game and the reason:
 * the pin at or above MOD_API_SCOPED, or a game that binds such targets
 * scoped only. Points to override_ovl and OP_OVLID_<name>.
 * @param self The calling mod.
 * @param call "override_addr" or "override_name".
 * @param target The address or name as the mod passed it.
 * @param addr The guest vaddr the target resolves to.
 * @return -1, the api's refusal value. */
static int refuse_unscoped(const openpete_mod_t* self, const char* call,
                           const char* target, uint32_t addr) {
    char where[2048];
    list_overlays_at(where, sizeof where, addr);
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    char why[96];
    if (!gd->legacy_window_links)
        snprintf(why, sizeof why, "%s binds overlay-window overrides to one "
                 "overlay at every api pin", game_get_id());
    else
        snprintf(why, sizeof why, "the mod pins api %u (overlay-scoped from "
                 "api %d)", self->api_pin, MOD_API_SCOPED);
    OPL_ERROR_F(self->log_tag,
                "%s(%s) refused on %s: 0x%08X lies in the overlay window, "
                "where it occurs in %s; %s. Bind one overlay with "
                "override_ovl(self, OP_OVLID_<name>, OP_FNADDR_<name>, fn) or "
                "override_name on a name of one overlay",
                call, target, game_get_id(), addr, where, why);
    return -1;
}

/** @brief Report whether a mod may bind an unscoped overlay-window link: a pin below MOD_API_SCOPED (or none) on a game with legacy_window_links. */
static int unscoped_allowed(const openpete_mod_t* self) {
    return self->api_pin < MOD_API_SCOPED &&
           openpete_mods_game_desc()->legacy_window_links;
}

/** @brief Log the unscoped overlay-window deprecation once per mod.
 *
 * Its own flag (unscoped_warned), apart from the raw-tier warning's, so a
 * mod whose first raw call targets the EXE still gets it.
 * @param self The calling mod.
 * @param addr The guest vaddr bound unscoped. */
static void warn_unscoped(openpete_mod_t* self, uint32_t addr) {
    if (self->unscoped_warned) return;
    self->unscoped_warned = 1;
    OPL_WARN_F(self->log_tag,
               "override at 0x%08X in the overlay window binds every overlay "
               "with a function there (api pin %u, below %d): deprecated; "
               "bind one overlay with override_ovl", addr, self->api_pin,
               MOD_API_SCOPED);
}

/** @brief Refuse a sim-affecting mod API call made from inside a present hook.
 *
 * Present hooks may only draw. Presents vary in count and timing between
 * runs, so guest access, game calls, overrides, hook registration, rumble
 * or guest allocation from one would desync replay. Each such API entry
 * calls this first and returns its failure value on 1.
 * @param what API name for the error line.
 * @return 1 (after logging an error) while openpete_mods_in_present is set,
 *         0 otherwise. */
static int purity_violation(const char* what) {
    if (!openpete_mods_in_present) return 0;
    OPL_ERROR_F("mods", "%s refused inside a present hook (render-pure "
                "context) - move sim-affecting work to a tick hook", what);
    return 1;
}

/* ---- the api gate: refusal after the exit pass ---------------------------
 * The exit pass (openpete_mods_shutdown) closes each mod when its shutdown
 * hook returns and then closes the whole api. Every entry but the four
 * named below brackets its work in mod_api_enter/mod_api_leave, which counts
 * the call in s_api_inflight; the close stores the flag and then waits
 * for the count to drain, so with sequentially consistent atomics a call
 * either sees the flag and is refused or is counted before the wait reads
 * the count, and no call is inside the engine when teardown starts. base()
 * and call() run guest code, which may park the fiber mid-call, so they
 * test the flags without being counted; nothing runs guest code once the
 * loop has ended. guest() and guest_addr() translate addresses into
 * memory that outlives teardown and are uncounted for their per-access
 * cost. */
static _Atomic int s_api_closed;    /* every mod closed: entries refuse */
static _Atomic int s_api_inflight;  /* counted entries between enter and leave */
static _Atomic int s_api_anon_refuse_logged; /* first refusal of a self-less entry */

/** @brief Log the first api refusal of a mod after its shutdown hook returned.
 *
 * @param self The calling mod, or NULL for an entry that takes none.
 * @param what The entry name. */
static void mod_api_refused(openpete_mod_t* self, const char* what) {
    _Atomic int* logged = self ? &self->api_refuse_logged
                               : &s_api_anon_refuse_logged;
    if (atomic_exchange(logged, 1)) return;
    OPL_WARN_F(self ? self->log_tag : "mods",
               "%s refused: the exit pass has closed %s; later refusals "
               "are not logged", what, self ? "this mod" : "the mod api");
}

/** @brief Report whether an api entry is closed for a mod.
 *
 * @param self The calling mod, or NULL.
 * @return 1 once the mod or the whole api is closed, else 0. */
static int mod_api_is_closed(openpete_mod_t* self) {
    return atomic_load(&s_api_closed) ||
           (self && atomic_load(&self->api_closed));
}

/** @brief Enter a counted api entry, or refuse it once the mod is closed.
 *
 * A 1 return obliges the caller to call mod_api_leave on every path out.
 * @param self The calling mod, or NULL for an entry that takes none.
 * @param what The entry name for the refusal line.
 * @return 1 to proceed, 0 refused (logged once per mod). */
static int mod_api_enter(openpete_mod_t* self, const char* what) {
    atomic_fetch_add(&s_api_inflight, 1);
    if (!mod_api_is_closed(self)) return 1;
    atomic_fetch_sub(&s_api_inflight, 1);
    mod_api_refused(self, what);
    return 0;
}

/** @brief Leave a counted api entry that mod_api_enter admitted. */
static void mod_api_leave(void) {
    atomic_fetch_sub(&s_api_inflight, 1);
}

int openpete_mods_api_closed(void) {
    return atomic_load(&s_api_closed);
}

/* ---- native-renderer visual-bypass advisory ------------------------------
 * The default --renderer vk draws the 3D world from gamestate and does NOT
 * rasterize the PSX ordering table, so overriding one of the original PSX
 * render / GPU-submission functions to change the picture has NO visible effect
 * under vk (it shows only under --renderer psycross). Warn so the author isn't
 * left wondering. True only when native is the sole display (pure vk, windowed);
 * under psycross / ab / diff / headless the PSX path still rasterizes.
 *
 * Scope: the *visual* no-op only. Freezing these can also drop the render->
 * gameplay channel writes (m_WasDrawn etc.) and desync - a separate, sharper
 * concern this advisory deliberately does not cover. */
static int s_native_hides_psx = 0;   /* set in openpete_mods_init from opts */

/* --scan-guard: 0 turns the guest-byte guard's compares off; set in
 * openpete_mods_init from opts. */
static int s_guest_guard_compares = 1;

int openpete_mods_guest_guard_compares(void) { return s_guest_guard_compares; }

/** @brief Install the game's guest-byte guard and latch whether it compares.
 *
 * --scan-guard 0 keeps the guard's brackets and turns its compares off.
 * @param opts Parsed options, or NULL for the defaults. */
static void mods_guard_install(const openpete_options_t* opts) {
    s_guest_guard_compares = !(opts && opts->scan_guard == 0);
    openpete_mods_set_guest_guard(openpete_mods_game_desc()->guest_guard);
}

/** @brief Name the native-bypassed PSX render function at a guest address.
 *
 * Resolves each name of the game descriptor's psx_render_fns list (the
 * original PSX render and GPU-submission functions whose rasterized
 * output the native renderer replaces) against the SDK symbol table and
 * compares addresses, so override_addr and override_name are both
 * covered. warn_render_bypassed uses it to warn a mod whose override
 * targets a function the native renderer does not rasterize.
 * @param addr Guest address; 0 returns NULL.
 * @return The function name, or NULL when addr is none of them, or the
 *         game registers no list or no symbol table. */
static const char* psx_render_fn_name(uint32_t addr) {
    const game_mod_desc_t* gd = openpete_mods_game_desc();
    if (!addr || !gd->symbols || !gd->psx_render_fns) return NULL;
    for (const char* const* n = gd->psx_render_fns; *n; n++) {
        const openpete_sdk_symbol_t* s = sym_find(*n);
        if (s && s->addr == addr) return *n;
    }
    return NULL;
}

/** @brief Warn that an override targets a PSX render function the native renderer never rasterizes.
 *
 * Fires on every registration at such an address, and only when
 * s_native_hides_psx is set (pure vk windowed). The warning names the
 * function and the alternatives; the registration itself is not refused.
 * @param self The registering mod; its log tag labels the line.
 * @param addr Guest address of the override target. */
static void warn_render_bypassed(openpete_mod_t* self, uint32_t addr) {
    if (!s_native_hides_psx) return;
    const char* rn = psx_render_fn_name(addr);
    if (!rn) return;
    OPL_WARN_F(self->log_tag,
               "override targets PSX render function %s (0x%08X): the native "
               "renderer (--renderer vk) draws the world from gamestate and does "
               "not rasterize the PSX ordering table, so visual changes here "
               "won't appear under vk. Draw via the mod draw API for native "
               "visuals, or run --renderer psycross to see this path.", rn, addr);
}

/** @brief Warn that an object built for every game binds an override by a raw address or a func_ name.
 *
 * A "*" object (mod->game_any) runs on every game, and such a target
 * resolves through the running game's address space or symbol table: on
 * another game it names a different function. Fires on every call.
 * @param self The registering mod.
 * @param call The api entry, for the log line.
 * @param target The address or name as the mod passed it. */
static void warn_any_game_target(const openpete_mod_t* self,
                                 const char* call, const char* target) {
    if (!self->game_any) return;
    OPL_WARN_F(self->log_tag,
               "%s(%s) from a mod built for every game (openpete_mod_game "
               "\"*\"): on %s it binds what this game has there, a "
               "different function on another game", call, target,
               game_get_id());
}

/** @brief Register a mod's override at a raw guest address on the chain.
 *
 * Refused inside a present hook. An address outside the overlay window
 * binds an EXE link. One inside it names a function per overlay, so it is
 * refused (refuse_unscoped) unless unscoped_allowed, where it binds an
 * unscoped link with a once-per-mod deprecation warning. A first success
 * also warns once per mod that raw addresses are unstable across engine
 * and decomp updates, and warns when the address is a PSX render function
 * the native renderer does not rasterize. Every call from a "*" object
 * warns (warn_any_game_target).
 * @param self The calling mod; NULL returns -1.
 * @param addr Guest function address; 0 fails in the chain.
 * @param fn The override; NULL fails in the chain.
 * @return 0, or -1 for a NULL self, a present-hook call, a refused
 *         window address or a chain failure. */
static int api_override_addr(openpete_mod_t* self, uint32_t addr,
                             openpete_mod_override_fn fn) {
    if (!self) return -1;
    if (purity_violation("override_addr")) return -1;
    uint32_t overlay = 0;
    {
        char a[16];
        snprintf(a, sizeof a, "0x%08X", addr);
        warn_any_game_target(self, "override_addr", a);
        if (in_overlay_window(addr)) {
            if (!unscoped_allowed(self))
                return refuse_unscoped(self, "override_addr", a, addr);
            overlay = OPENPETE_MODS_OVERLAY_ANY;
        }
    }
    int rc = openpete_mods_chain_add_ovl(addr, overlay, fn, self->id);
    if (rc == 0) {
        if (overlay == OPENPETE_MODS_OVERLAY_ANY) warn_unscoped(self, addr);
        OPL_INFO_F(self->log_tag, "override registered at 0x%08X", addr);
        /* Raw addresses are region-pinned and drift across decomp
         * progress - warn from the actual use, once per mod (the old
         * manifest tier="raw" declaration said the same thing but could
         * lie in either direction). */
        if (!self->raw_warned) {
            self->raw_warned = 1;
            OPL_WARN_F("mods", "%s uses raw-tier addresses: unstable "
                       "across engine/decomp updates and region-pinned",
                       self->id);
        }
        warn_render_bypassed(self, addr);
    }
    return rc;
}

/** @brief Register a mod's override on a function named in the SDK symbol table.
 *
 * Refused inside a present hook. A name with one row binds that row's
 * (address, overlay), so an overlay function fires in its own overlay
 * only. A name with rows in several overlays (a raw func_XXXXXXXX alias)
 * is refused (refuse_unscoped) unless unscoped_allowed, where it binds an
 * unscoped link with the once-per-mod deprecation warning. A legacy alias
 * resolves to its address with a warning naming the stable spelling; a
 * name absent from the table is an error. Warns when the target is a PSX render function the native
 * renderer does not rasterize, and on every func_ name from a "*" object
 * (warn_any_game_target).
 * @param self The calling mod; NULL returns -1.
 * @param name Symbol name; NULL is treated as absent.
 * @param fn The override.
 * @return 0, or -1 for a NULL self, a present-hook call, an unknown or
 *         refused name or a chain failure. */
static int api_override_name(openpete_mod_t* self, const char* name,
                             openpete_mod_override_fn fn) {
    if (!self) return -1;
    if (purity_violation("override_name")) return -1;
    if (name && strncmp(name, "func_", 5) == 0)
        warn_any_game_target(self, "override_name", name);
    size_t nrows = 0;
    const openpete_sdk_symbol_t* sym = sym_find_range(name, &nrows);
    if (!sym) {
        OPL_ERROR_F(self->log_tag,
                    "override target \"%s\" not in the %s SDK symbol table",
                    name ? name : "(null)", game_get_id());
        return -1;
    }
    /* One row per (name, overlay): one row binds (address, overlay) from
     * the row; several rows are a raw name shared by several overlays. */
    uint32_t overlay = sym->overlay;
    if (nrows > 1) {
        if (!unscoped_allowed(self))
            return refuse_unscoped(self, "override_name", name, sym->addr);
        overlay = OPENPETE_MODS_OVERLAY_ANY;
    }
    if (sym->legacy) {
        /* Decomp promotion contract: historical raw names resolve
         * forever, but say so - the stable name is the durable one. */
        OPL_WARN_F(self->log_tag,
                   "\"%s\" is a legacy raw alias (function has been "
                   "renamed) - resolved to 0x%08X; update the mod to the "
                   "stable name", name, sym->addr);
    }
    int rc = openpete_mods_chain_add_ovl(sym->addr, overlay, fn, self->id);
    if (rc == 0) {
        if (overlay == OPENPETE_MODS_OVERLAY_ANY) warn_unscoped(self, sym->addr);
        OPL_INFO_F(self->log_tag, "override registered: %s (0x%08X)",
                   name, sym->addr);
        warn_render_bypassed(self, sym->addr);
    }
    return rc;
}

/** @brief Register a mod's override on the function at an address in one overlay, or in the EXE for id 0.
 *
 * Refused inside a present hook. Refused with an error naming the mod, the
 * id, the address and the game, binding nothing: an id that is neither 0
 * nor in the game's overlay table; id 0 with an address inside the overlay
 * window; a nonzero id with an address outside it; an address that is not
 * a function entry of that overlay (its generated dispatch). The link fires
 * only while that overlay is resident. Warns when the target is a PSX
 * render function the native renderer does not rasterize.
 * @param self The calling mod; NULL returns -1.
 * @param overlay_id The overlay table id, or 0 for the EXE.
 * @param addr Guest function address; 0 fails in the chain.
 * @param fn The override; NULL fails in the chain.
 * @return 0, or -1 for a NULL self, a present-hook call, a refused
 *         argument or a chain failure. */
static int api_override_ovl(openpete_mod_t* self, uint32_t overlay_id,
                            uint32_t addr, openpete_mod_override_fn fn) {
    if (!self) return -1;
    if (purity_violation("override_ovl")) return -1;
    const game_mod_overlay_t* o = overlay_id ? overlay_by_id(overlay_id) : NULL;
    const char* why = NULL;
    if (overlay_id && !o)
        why = "the id is not in this game's overlay table";
    else if (!overlay_id && in_overlay_window(addr))
        why = "id 0 is the EXE and the address lies in the overlay window";
    else if (overlay_id && !in_overlay_window(addr))
        why = "the address lies outside the overlay window";
    else if (o && !overlay_has_entry(o, addr))
        why = "the address is not a function entry of that overlay";
    if (why) {
        OPL_ERROR_F(self->log_tag,
                    "override_ovl(%u%s%s, 0x%08X) refused on %s: %s",
                    overlay_id, o ? " = " : "", o ? o->name : "", addr,
                    game_get_id(), why);
        return -1;
    }
    int rc = openpete_mods_chain_add_ovl(addr, overlay_id, fn, self->id);
    if (rc == 0) {
        if (o)
            OPL_INFO_F(self->log_tag, "override registered at 0x%08X in "
                       "overlay %s (id %u)", addr, o->name, overlay_id);
        else
            OPL_INFO_F(self->log_tag, "override registered at 0x%08X", addr);
        warn_render_bypassed(self, addr);
    }
    return rc;
}

/** @brief Format a printf-style message and log it under the mod's tag.
 *
 * The text is formatted into a 512-byte buffer and truncated to fit. A
 * level outside OPL_TRACE..OPL_ERROR logs as OPL_INFO, a NULL fmt logs an
 * empty message and a NULL self logs under the tag "mod:?". Refused, with
 * nothing logged, once the mod's shutdown hook has returned.
 * @param self The calling mod, or NULL.
 * @param level An OPL_* level. */
static void api_log(openpete_mod_t* self, int level, const char* fmt, ...) {
    if (!mod_api_enter(self, "log")) return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt ? fmt : "", ap);
    va_end(ap);
    if (level < OPL_TRACE || level > OPL_ERROR) level = OPL_INFO;
    openpete_logf((openpete_log_level_t)level,
                  self ? self->log_tag : "mod:?", "%s", msg);
    mod_api_leave();
}

/* Region-table inverse (psx_runtime_internal.h; declared here for the same
 * reason call_by_address is - this file does not pull the runner header in). */
int psx_runtime_region_host_to_vaddr(const void* host, uint32_t* out_vaddr);

/** @brief Translate a guest vaddr to a host pointer for a mod.
 *
 * Delegates to openpete_psx_to_host_ptr so the two never disagree: it
 * strips the segment bits with the full 0x1FFFFFFF mask (no 16 MB ceiling
 * from a 24-bit offset), covers the scratchpad, and consults the region
 * table for anything above canonical RAM, the arena included at any size.
 *
 * Returns NULL inside a present hook, for a segment other than kuseg,
 * kseg0 (0x8, 0x9) or kseg1 (0xA, 0xB), and for a nonzero address in the
 * kernel area (physical below 0x10000, warned once). Guest address 0 is
 * PSX NULL and resolves to NULL, not to the base of RAM: a mod that missed
 * a failed allocation and writes through the returned pointer faults in
 * its own code rather than overwriting the kernel area (which is also
 * where the low park lives). */
static void* api_guest(uint32_t vaddr) {
    if (purity_violation("guest")) return NULL;
    uint32_t seg = vaddr >> 28;
    if (seg != 0x0u && seg != 0x8u && seg != 0x9u && seg != 0xAu &&
        seg != 0xBu)
        return NULL;
    /* The kernel area (physical [0, 0x10000)) is the stream-handle range:
     * a frame word whose 21-bit field addresses it decodes as a handle,
     * never as bytes. No allocator lands there, so this raw translation
     * is the one route left for a mod to park data under the bound; it
     * is refused (NULL), loudly once. Not fatal: a mod chasing a null
     * model row lands here on a READ, which is harmless. */
    if (vaddr != 0u && (vaddr & 0x1FFFFFFFu) < 0x10000u) {
        static int s_warned = 0;
        if (!s_warned) {
            s_warned = 1;
            OPL_WARN_F("mods", "guest(0x%08X): the kernel area is the "
                       "stream-handle range - no mod data may live below "
                       "0x10000; refused (NULL)", vaddr);
        }
        return NULL;
    }
    return openpete_psx_to_host_ptr(vaddr);
}

/* MIPS kseg0 base - architectural, not a game address (the purity check
 * permits #define'd constants). */
#define OP_KSEG0_BASE 0x80000000u
#define OP_KSEG0_SCRATCH 0x9F800000u /* scratchpad's KSEG0 spelling */

/** @brief Translate a host pointer back to a guest vaddr, the inverse of api_guest.
 *
 * Covers every address api_guest can hand out: canonical RAM (returned in
 * kseg0), the scratchpad (0x9F800000 base) and any registered region,
 * guest_alloc's arena included; a mod that walks a host_view and needs the
 * vaddr of an interior byte relies on the last case.
 * @return the vaddr, or 0 for NULL or a pointer outside all three. */
static uint32_t api_guest_addr(const void* host) {
    if (!host) return 0;
    const uint8_t* p = (const uint8_t*)host;
    uint8_t* ram = openpete_psx_get_ram();
    if (ram && p >= ram && p < ram + 2u * 1024u * 1024u)
        return OP_KSEG0_BASE | (uint32_t)(p - ram);
    uint8_t* scratch = openpete_psx_get_scratch();
    if (scratch && p >= scratch &&
        p < scratch + OPENPETE_PSX_SCRATCH_SIZE)
        return OP_KSEG0_SCRATCH | (uint32_t)(p - scratch);
    uint32_t va = 0;
    if (psx_runtime_region_host_to_vaddr(host, &va)) return va;
    return 0;
}

void call_by_address(CPUState* cpu, uint32_t addr); /* psx_runtime_internal.h */

/** @brief Run the guest function at an address on the given CPU state.
 *
 * Refused inside a present hook, where it returns without running.
 * @param cpu The guest CPU state the call runs on.
 * @param addr Guest function address. */
static void api_call_addr(CPUState* cpu, uint32_t addr) {
    if (purity_violation("call_addr")) return;
    openpete_psxtrace_gate_host_enter();
    openpete_mods_guard_suspend();
    call_by_address(cpu, addr);
    openpete_mods_guard_resume();
    openpete_psxtrace_gate_host_leave();
}

/** @brief Register the mod's per-present hook.
 *
 * Refused inside a present hook.
 * @param self The calling mod; NULL returns -1.
 * @param fn The hook.
 * @return 0, or -1 for a NULL self, a present-hook call or a registry
 *         failure. */
static int api_register_present_hook(openpete_mod_t* self,
                                     openpete_mod_present_fn fn) {
    if (!self) return -1;
    if (purity_violation("register_present_hook")) return -1;
    int rc = openpete_mods_register_present_hook(fn, self->id);
    if (rc == 0)
        OPL_INFO_F(self->log_tag, "present hook registered");
    return rc;
}

/* Per-mod UI section (api v3): the callback is stored on the mod and
 * invoked by the dev overlay's Mods panel while the mod is enabled.
 * Registration is entry/tick context (the callback itself runs under
 * the render-purity guard - see openpete_mods_ui_draw). */

/* Canonical tick counter mirror for the ui_status lazy reset; advanced
 * by openpete_mods_tick_boundary. */
static unsigned s_current_tick;

/** @brief Append one printf-style status line to the mod's Mods-panel section.
 *
 * The line list resets on the first call of each tick, holds 24 lines of
 * 120 bytes, and truncates a longer line; lines past the 24th are dropped
 * and status_trunc is set. A NULL self or fmt does nothing, and so does a
 * call after the mod's shutdown hook returned.
 * @param self The calling mod. */
static void api_ui_status(openpete_mod_t* self, const char* fmt, ...) {
    if (!self || !fmt || !mod_api_enter(self, "ui_status")) return;
    struct openpete_mod* m = self;
    if (m->status_tick != s_current_tick) {
        m->status_n = 0;
        m->status_trunc = 0;
        m->status_tick = s_current_tick;
    }
    if (m->status_n >= (int)(sizeof m->status / sizeof m->status[0])) {
        m->status_trunc = 1;
    } else {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(m->status[m->status_n], sizeof m->status[0], fmt, ap);
        va_end(ap);
        m->status_n++;
    }
    mod_api_leave();
}

/** @brief Post an OSD toast attributed to the calling mod.
 *
 * The toast is attributed to the mod on two axes: a non-empty key is
 * namespaced as `<id>/<key>` so two mods cannot repaint each other's toast,
 * and the mod id is the openpete_osd_notify source, whose rate-limit bucket
 * is per source, so one mod in a tick loop cannot starve another's
 * notifications or the engine's. A NULL or empty key posts an uncoalesced
 * toast. The text is formatted by openpete_osd_vnotify. A timeout over
 * OPENPETE_OSD_TIMEOUT_MAX_MS, OPENPETE_OSD_TIMEOUT_HOLD included, is
 * clamped to it, so a mod cannot pin a toast. Refused once the mod's
 * shutdown hook has returned. */
static void api_notify(openpete_mod_t* self, const char* key,
                       uint32_t timeout_ms, const char* fmt, ...) {
    if (!self || !fmt || !mod_api_enter(self, "notify")) return;
    struct openpete_mod* m = self;
    if (timeout_ms > OPENPETE_OSD_TIMEOUT_MAX_MS) timeout_ms = OPENPETE_OSD_TIMEOUT_MAX_MS;
    /* A namespaced key that does not fit the OSD's key field posts without
     * coalescing rather than sharing a truncated key with another one. */
    char nskey[OPENPETE_OSD_KEY_MAX];
    const int keyed = key && *key &&
                      snprintf(nskey, sizeof nskey, "%s/%s", m->id, key) < (int)sizeof nskey;
    va_list ap;
    va_start(ap, fmt);
    openpete_osd_vnotify(m->id, keyed ? nskey : NULL, timeout_ms, fmt, ap);
    va_end(ap);
    mod_api_leave();
}

/** @brief Set the mod's callback for live enable and disable notifications.
 *
 * Replaces any earlier callback; refused inside a present hook.
 * @param self The calling mod; NULL returns -1.
 * @param fn The callback, or NULL to clear it.
 * @return 0, or -1 for a NULL self or a present-hook call. */
static int api_register_toggle_hook(openpete_mod_t* self,
                                    openpete_mod_toggle_fn fn) {
    if (!self) return -1;
    if (purity_violation("register_toggle_hook")) return -1;
    self->toggle_fn = fn;
    return 0;
}

/** @brief Set the mod's exit callback, run by the exit pass and by a hot reload.
 *
 * Replaces any earlier callback; refused inside a present hook.
 * @param self The calling mod; NULL returns -1.
 * @param fn The callback, or NULL to clear it.
 * @return 0, or -1 for a NULL self or a present-hook call. */
static int api_register_shutdown_hook(openpete_mod_t* self,
                                      openpete_mod_shutdown_fn fn) {
    if (!self) return -1;
    if (purity_violation("register_shutdown_hook")) return -1;
    self->shutdown_fn = fn;
    return 0;
}

/* ---- engine services ------------------------------------------------- */




/** @brief Return the mod's persistent data directory, `<mod dir>/data`.
 *
 * The path is built and the directory created on the first call, then
 * cached in the mod's data_dir field.
 * @return the cached path, or NULL for a NULL self. */
static const char* api_data_dir(openpete_mod_t* self) {
    if (!self) return NULL;
    if (!self->data_dir[0]) {
        snprintf(self->data_dir, sizeof self->data_dir, "%s/data",
                 self->dir);
        mod_mkdir(self->data_dir); /* EEXIST fine */
    }
    return self->data_dir;
}

/* The disc image the engine opened, captured once the library has had its
 * chance to fill opts->disc_path. The sector image behind a cue is resolved
 * on first demand and kept, since the resolver touches the filesystem. */
static char s_disc_path[MOD_PATH_MAX];
static char s_disc_bin[MOD_PATH_MAX];
static int  s_disc_bin_state; /* 0 unresolved, 1 resolved, -1 unresolvable */

/** @brief Record the boot disc path for api_disc_path and mark its sector image unresolved.
 *
 * openpete_mods_init calls it; the library, the splash drop and --ingest
 * have all written opts->disc_path by then, so the boot path is final. A
 * NULL opts or disc path records an empty path, which api_disc_path
 * reports as NULL. */
static void mods_capture_disc_path(const openpete_options_t* opts) {
    const char* p = (opts && opts->disc_path) ? opts->disc_path : "";
    snprintf(s_disc_path, sizeof s_disc_path, "%s", p);
    s_disc_bin_state = 0;
}

/** @brief Return the booted disc's path, or its sector image when asked.
 *
 * A cue sheet resolves to its data image on the first request; the answer,
 * including failure, is cached.
 * @param self The calling mod; NULL returns NULL.
 * @param want_bin Zero for the path the engine opened, nonzero for the
 *        sector image behind it.
 * @return A string owned by the loader, or NULL when no disc path was
 *         recorded or the image cannot be resolved. */
static const char* api_disc_path(openpete_mod_t* self, int want_bin) {
    if (!self || !s_disc_path[0]) return NULL;
    if (!want_bin) return s_disc_path;
    if (s_disc_bin_state == 0)
        s_disc_bin_state = openpete_psx_resolve_disc_bin(
                               s_disc_path, s_disc_bin, sizeof s_disc_bin,
                               NULL, 0) == 0 ? 1 : -1;
    return s_disc_bin_state > 0 ? s_disc_bin : NULL;
}

/* Handles of other games' discs, by installed-disc table slot, opened on
 * first read. Direct mode: the reads go to the image file and never reach
 * the disc cache, which holds only the booted disc. */
static _Atomic(disc_image_t*) s_other_disc[MOD_GAMES_TABLE_MAX];

/** @brief Route a mod's disc call for a game, logging a refusal once per mod.
 *
 * @param self The calling mod.
 * @param game The game id the call names.
 * @param call The api entry's name, for the log line.
 * @return The route. */
static mod_disc_route_t mod_disc_route(openpete_mod_t* self, const char* game,
                                       const char* call) {
    const mod_disc_route_t r = mod_games_route(&self->games, game_get_id(), game);
    if (r == MOD_DISC_UNLISTED && !self->disc_refused_logged) {
        self->disc_refused_logged = 1;
        OPL_ERROR_F(self->log_tag, "%s(\"%s\") refused: the manifest names "
                    "\"%s\" in neither requires_games nor optional_games",
                    call, game, game);
    }
    return r;
}

/** @brief Return the open handle of another game's disc, opening it on first use.
 *
 * Two threads opening at once both open; the first to publish keeps its
 * handle and the other closes its own.
 * @param slot The game's installed-disc table slot.
 * @return The handle, or NULL when the image does not open. */
static disc_image_t* other_disc(int slot) {
    disc_image_t* img = atomic_load(&s_other_disc[slot]);
    if (img) return img;
    disc_image_t* fresh = disc_image_open(mod_games_slot_path(slot, 0),
                                          DISC_READ_DIRECT, OPENPETE_DISC_TAG_NONE);
    if (!fresh) return NULL;
    disc_image_t* expect = NULL;
    if (atomic_compare_exchange_strong(&s_other_disc[slot], &expect, fresh))
        return fresh;
    disc_image_close(fresh);
    return expect;
}

/** @brief Read one raw sector of a game's disc: the booted disc through disc_image's game reader, another game's through its own direct handle.
 *
 * @param self The calling mod; NULL fails.
 * @param game The game id.
 * @param lba The sector.
 * @param buf Receives 2352 bytes.
 * @return 0, or -1 when there is no caller, no buffer, no disc, a refused
 *         game, a sector past the end, or a failed read. */
static int api_read_sector(openpete_mod_t* self, const char* game, uint32_t lba,
                           uint8_t* buf) {
    if (!self || !buf) return -1;
    const mod_disc_route_t r = mod_disc_route(self, game, "read_sector");
    if (r == MOD_DISC_BOOTED) return disc_image_game_read_sector(lba, buf);
    if (r != MOD_DISC_OTHER) return -1;
    disc_image_t* img = other_disc(mod_games_slot(game));
    if (!img) return -1;
    const uint64_t off = (uint64_t)lba * DISC_IMAGE_RAW_SECTOR;
    if (off + DISC_IMAGE_RAW_SECTOR > disc_image_size(img)) return -1;
    return disc_image_read_bytes(img, off, buf, DISC_IMAGE_RAW_SECTOR,
                                 OPENPETE_DISC_TAG_NONE) < 0 ? -1 : 0;
}

/** @brief Return a game's disc path: api_disc_path for the running game, the library entry's for another.
 *
 * @param self The calling mod; NULL returns NULL.
 * @param game The game id.
 * @param want_bin Nonzero for the sector image.
 * @return The path, or NULL for a refused game, no disc or an unresolved image. */
static const char* api_game_disc_path(openpete_mod_t* self, const char* game,
                                      int want_bin) {
    if (!self) return NULL;
    const mod_disc_route_t r = mod_disc_route(self, game, "game_disc_path");
    if (r == MOD_DISC_BOOTED) return api_disc_path(self, want_bin);
    if (r != MOD_DISC_OTHER) return NULL;
    const char* p = mod_games_slot_path(mod_games_slot(game), want_bin);
    return p && *p ? p : NULL;
}

/** @brief Report whether a game is installed, for the game_installed api entry.
 *
 * @param self The calling mod; unused, since the answer is the session's.
 * @param game The game id.
 * @return 1 for the running game or a game with a library disc, else 0. */
static int api_game_installed(openpete_mod_t* self, const char* game) {
    (void)self;
    return mod_games_installed(game_get_id(), game);
}

/** @brief Library lookup for the installed-disc table: the entry's cue and the sector image behind it.
 *
 * @param game The game id.
 * @param cue Receives the cue path.
 * @param cue_cap Its capacity.
 * @param bin Receives the sector image; "" when it does not resolve.
 * @param bin_cap Its capacity.
 * @return 0 when the library holds a disc of @p game, -1 otherwise. */
static int mods_library_lookup(const char* game, char* cue, size_t cue_cap,
                               char* bin, size_t bin_cap) {
    if (openpete_library_game_disc(game, cue, cue_cap) != 0) return -1;
    if (openpete_psx_resolve_disc_bin(cue, bin, bin_cap, NULL, 0) != 0) bin[0] = 0;
    OPL_INFO_F("mods", "library disc of %s: %s", game, cue);
    return 0;
}

/** @brief Fill the installed-disc table with every registered game but the running one. */
static void mods_games_snapshot(void) {
    const char* ids[MOD_GAMES_TABLE_MAX];
    int n = 0;
    while (n < MOD_GAMES_TABLE_MAX && (ids[n] = openpete_game_id_at(n)) != NULL) n++;
    mod_games_snapshot(game_get_id(), ids, n, mods_library_lookup);
}

/** @brief Return the running game's id for the game_id api entry.
 *
 * @param self The calling mod; unused, since the id is the session's.
 * @return The registered game's id string. */
static const char* api_game_id(openpete_mod_t* self) {
    (void)self;
    return game_get_id();
}

/** @brief Fill the game-neutral state a game-service call reads.
 *
 * A non-NULL @p cpu outside a present hook or UI section reads guest RAM
 * at the call; otherwise the record sampled at the last canonical tick
 * boundary is returned, since off tick context the guest is mid-tick or
 * on another thread.
 * @param cpu The calling tick hook's CPU, or NULL.
 * @param out Receives the state. */
static void api_game_state(CPUState* cpu, openpete_mods_game_state_t* out) {
    if (cpu && !openpete_mods_in_present) openpete_mods_game_state_read(cpu, out);
    else openpete_mods_game_state_sampled(out);
}

/** @brief Report whether gameplay is live, for the gameplay_live api entry.
 *
 * @param self The calling mod; NULL reads as not live.
 * @param cpu The calling tick hook's CPU, or NULL for the sampled record.
 * @return 1 while gameplay is live, else 0. */
static int api_gameplay_live(openpete_mod_t* self, CPUState* cpu) {
    if (!self) return 0;
    openpete_mods_game_state_t st;
    api_game_state(cpu, &st);
    return st.live;
}

/** @brief Fill an openpete_mod_pose_t for the player_pose api entry.
 *
 * @param self The calling mod; NULL fails.
 * @param cpu The calling tick hook's CPU, or NULL for the sampled record.
 * @param out The caller's record; its struct_size must match the engine's.
 * @return 0 when filled; -1, writing nothing, for a NULL argument, a wrong
 *         struct_size or while no level is live. */
static int api_player_pose(openpete_mod_t* self, CPUState* cpu,
                           openpete_mod_pose_t* out) {
    if (!self || !out) return -1;
    if (out->struct_size != (uint32_t)sizeof(openpete_mod_pose_t)) {
        OPL_ERROR_F(self->log_tag, "player_pose: struct_size %u, engine expects %u "
                    "(rebuild against this SDK)", out->struct_size,
                    (unsigned)sizeof(openpete_mod_pose_t));
        return -1;
    }
    openpete_mods_game_state_t st;
    api_game_state(cpu, &st);
    if (st.pose_rc < 0) return -1;
    out->x = st.x; out->y = st.y; out->z = st.z;
    out->yaw = st.yaw;
    out->level = st.level;
    out->placeable = st.pose_rc > 0 ? 1u : 0u;
    out->tick = st.tick;
    return 0;
}

/** @brief Return the level being played, for the level_id api entry.
 *
 * @param self The calling mod; NULL reads as no level.
 * @param cpu The calling tick hook's CPU, or NULL for the sampled record.
 * @return The game's level id, or -1 while no level is live. */
static int api_level_id(openpete_mod_t* self, CPUState* cpu) {
    if (!self) return -1;
    openpete_mods_game_state_t st;
    api_game_state(cpu, &st);
    return st.level;
}

static char s_sdk_dir[MOD_PATH_MAX];

/* ---- per-game object paths and identity ------------------------------ */

/* Where a loaded object was found; decides the openpete_mod_game value it
 * must carry (mod_check_game). */
typedef enum {
    MOD_OBJ_GAME = 0,   /* .build/<game-id>/<id>: compiled for the running game */
    MOD_OBJ_ANY,        /* .build/any/<id>: a prebuilt "*" object */
    MOD_OBJ_LEGACY,     /* .build/<id>: written before per-game objects */
} mod_obj_kind_t;

/** @brief Format <mod_dir>/.build/<game-id>, the running game's object directory.
 *
 * The object, its build-key stamp, the compiler response file, the
 * compile log and the hot-reload copies live there.
 * @param mod_dir Mod directory.
 * @param out Output buffer; the path is truncated to @p cap.
 * @param cap Size of @p out. */
static void mod_game_build_dir(const char* mod_dir, char* out, size_t cap) {
    snprintf(out, cap, "%s/.build/%s", mod_dir, mods_build_game());
}

/** @brief Format the path of a mod's object for the running game, <mod_dir>/.build/<game-id>/<id> plus the platform extension.
 *
 * A path that does not fit is logged; @p out then holds it truncated.
 * @param mod_dir Mod directory.
 * @param id Mod id.
 * @param out Output buffer.
 * @param cap Size of @p out.
 * @return 0 when the whole path fits, else -1. */
static int mod_obj_path(const char* mod_dir, const char* id, char* out,
                        size_t cap) {
    const int n = snprintf(out, cap, "%s/.build/%s/%s" MOD_DL_EXT, mod_dir,
                           mods_build_game(), id);
    if (n >= 0 && (size_t)n < cap) return 0;
    OPL_ERROR_F("mods", "%s: object path under %s exceeds %zu bytes", id,
                mod_dir, cap - 1);
    return -1;
}

/** @brief Create <mod_dir>/.build and the running game's directory below it.
 *
 * An existing directory is not an error.
 * @param mod_dir Mod directory. */
static void mod_make_build_dir(const char* mod_dir) {
    char d[MOD_PATH_MAX + 8];
    snprintf(d, sizeof d, "%s/.build", mod_dir);
    mod_mkdir(d);
    mod_game_build_dir(mod_dir, d, sizeof d);
    mod_mkdir(d);
}

/** @brief Find a prebuilt object of a mod for the running game, in lookup order.
 *
 * Probes .build/<game-id>/<id>, then .build/any/<id>, then the legacy
 * .build/<id>, each with the platform extension; the first existing file
 * is the object, and its kind decides which game identity it must carry.
 * @param mod_dir Mod directory.
 * @param id Mod id.
 * @param out Receives the found path, or the .build/<game-id>/ path when
 *        none exists; truncated to @p cap.
 * @param cap Size of @p out.
 * @return The mod_obj_kind_t of the found object, or -1 when none exists. */
static int mod_find_prebuilt(const char* mod_dir, const char* id, char* out,
                             size_t cap) {
    static const char* const k_dirs[] = { NULL, "any/", "" };
    struct stat st;
    for (int k = MOD_OBJ_GAME; k <= MOD_OBJ_LEGACY; k++) {
        if (k == MOD_OBJ_GAME) {
            if (mod_obj_path(mod_dir, id, out, cap) != 0) return -1;
        } else {
            snprintf(out, cap, "%s/.build/%s%s" MOD_DL_EXT, mod_dir,
                     k_dirs[k], id);
        }
        if (stat(out, &st) == 0 && S_ISREG(st.st_mode)) return k;
    }
    mod_obj_path(mod_dir, id, out, cap);
    return -1;
}

/** @brief Copy a loaded object's openpete_mod_game string into a buffer.
 *
 * Reads at most @p cap - 1 bytes and requires a NUL within them, so an
 * object exporting a malformed symbol reads as "(malformed)".
 * @param dl The dlopen handle.
 * @param out Output buffer, always NUL-terminated.
 * @param cap Size of @p out.
 * @return 1 when the object exports openpete_mod_game, else 0 (@p out
 *         then reads "(none)"). */
static int mod_object_game(void* dl, char* out, size_t cap) {
    const char* g = (const char*)mod_dlsym(dl, "openpete_mod_game");
    if (!g) { snprintf(out, cap, "(none)"); return 0; }
    const char* nul = memchr(g, '\0', cap - 1);
    if (!nul) snprintf(out, cap, "(malformed)");
    else      memcpy(out, g, (size_t)(nul - g) + 1);
    return 1;
}

/** @brief Refuse a loaded object whose recorded game does not fit its lookup path and the running game.
 *
 * An object from .build/<game-id>/ must record the running game; one
 * from .build/any/ must record "*"; a legacy .build/<id> object loads
 * when it records the running game or "*", and when it records nothing
 * only on spyro-1, since every object built before per-game SDKs was
 * built against the Spyro 1 SDK. Sets mod->game_any for a "*" object.
 * Each refusal logs an error naming both games.
 * @param mod The mod; its id labels the log line.
 * @param dl The dlopen handle.
 * @param path Object path for the log line.
 * @param kind The mod_obj_kind_t the object was found as.
 * @return 0 when loadable, -1 when refused. */
static int mod_check_game(struct openpete_mod* mod, void* dl,
                          const char* path, int kind) {
    char g[32];
    const int has = mod_object_game(dl, g, sizeof g);
    const char* run = game_get_id();
    const int is_run = has && strcmp(g, run) == 0;
    const int is_any = has && strcmp(g, "*") == 0;
    int ok;
    if (kind == MOD_OBJ_GAME)     ok = is_run;
    else if (kind == MOD_OBJ_ANY) ok = is_any;
    else ok = has ? (is_run || is_any) : strcmp(run, "spyro-1") == 0;
    mod->game_any = ok && is_any;
    if (ok) return 0;
    if (kind == MOD_OBJ_LEGACY && !has)
        OPL_ERROR_F("mods", "%s: %s was built before per-game SDKs (built "
                    "for spyro-1, running %s) - rebuild it against the %s "
                    "SDK - disabled", mod->id, path, run, run);
    else if (kind == MOD_OBJ_ANY)
        OPL_ERROR_F("mods", "%s: %s records game %s, but .build/any/ holds "
                    "objects built for every game (\"*\"; running %s) - "
                    "disabled", mod->id, path, g, run);
    else
        OPL_ERROR_F("mods", "%s: %s records game %s, not the running %s - "
                    "disabled", mod->id, path, g, run);
    return -1;
}

/* defined later in this file */
static int parse_manifest(const char* mod_dir, struct openpete_mod* mod);
static void mod_register_key_defaults(const struct openpete_mod* mod, int enabled);
static int mod_enter_deferred(struct openpete_mod* mod);

static int build_mod(struct openpete_mod* mod, const char* sdk_dir,
                     char* so_path, size_t so_path_len);
static int mod_compute_key(const char* mod_dir, const char* sdk_dir,
                           char key_out[65]);
static void sdk_prefix_recheck(const char* sdk_dir);
static int mods_sdk_present(const char* sdk_dir);
static void mods_sdk_missing_notice(const char* sdk_dir);
static int config_compute_key(const char* mod_dir, char key_out[65]);
static int mod_compile(const char* mod_dir, const char* id,
                       const char* sdk_dir, const char* key,
                       const char* out_path, int stamp);
typedef int (*mod_entry_fn)(const openpete_mod_api_t*, openpete_mod_t*);
static const openpete_mod_api_t s_api;

/** @brief Open a hot-reload object and run every check a load runs, without touching the running object.
 *
 * dlopens @p path, then checks the abi descriptor, the ImGui descriptor,
 * the entry symbol and the game identity (mod_check_game for @p kind).
 * Each failure logs and leaves the mod's running object, registrations
 * and game_any as they were.
 * @param mod The reloading mod.
 * @param path The fresh object.
 * @param kind The mod_obj_kind_t the object stands for.
 * @param dl_out Receives the dlopen handle on success.
 * @return The entry point, or NULL when the object is refused. */
static mod_entry_fn mod_reload_open(struct openpete_mod* mod,
                                    const char* path, int kind,
                                    void** dl_out) {
    void* dl = mod_dlopen(path);
    if (!dl) {
        OPL_ERROR_F("mods", "%s: reload dlopen failed: %s - keeping the "
                    "running object", mod->id, mod_dlerror());
        return NULL;
    }
    if (mod_check_abi(dl, mod->id, path) != 0) return NULL;
    if (mod_resolve_imgui(mod, dl, path) != 0) return NULL;
    mod_entry_fn entry = (mod_entry_fn)mod_dlsym(dl, "openpete_mod_entry");
    if (!entry) {
        OPL_ERROR_F("mods", "%s: reload object has no openpete_mod_entry "
                    "- keeping the running object", mod->id);
        return NULL;
    }
    const int was_any = mod->game_any;
    if (mod_check_game(mod, dl, path, kind) != 0) {
        mod->game_any = was_any;   /* the running object stays */
        return NULL;
    }
    *dl_out = dl;
    return entry;
}

/* Per-hook budget of the exit pass, and the bound on the wait for api
 * calls other threads have in progress when the api closes. */
enum { MOD_SHUTDOWN_BUDGET_MS = 500, MOD_SHUTDOWN_DRAIN_MS = 1000 };

/** @brief Run a mod's shutdown hook under the guest guard, timing it against the budget.
 *
 * A no-op for a mod without one or whose entry never ran.
 * @param mod The mod.
 * @param why "exit" or "reload", for the log lines.
 * @return 1 when a hook ran, else 0. */
static int mod_run_shutdown_hook(struct openpete_mod* mod, const char* why) {
    openpete_mod_shutdown_fn fn = mod->shutdown_fn;
    if (!fn || !mod->entered) return 0;
    const Uint64 t0 = SDL_GetTicks(); /* clock-guard: wall */
    const openpete_mods_site_t site = openpete_mods_site_open(OPENPETE_MODS_CTX_SHUTDOWN);
    openpete_mods_guard_enter(mod->id);
    fn();
    openpete_mods_guard_leave();
    openpete_mods_site_close(site);
    const Uint64 ms = SDL_GetTicks() - t0; /* clock-guard: wall */
    if (ms > MOD_SHUTDOWN_BUDGET_MS)
        OPL_WARN_F(mod->log_tag, "shutdown hook (%s) took %llu ms, over the "
                   "%d ms budget: signal the mod's threads and join them "
                   "without waiting on more work", why,
                   (unsigned long long)ms, (int)MOD_SHUTDOWN_BUDGET_MS);
    else
        OPL_INFO_F(mod->log_tag, "shutdown hook (%s) returned in %llu ms",
                   why, (unsigned long long)ms);
    return 1;
}

/** @brief Reload one mod in place at a tick boundary under a fresh object.
 *
 * First produces a loadable object under a unique
 * `.build/<game-id>/<id>.hotN` name and dlopens it, checking the abi, the
 * game identity, the ImGui descriptor and the entry symbol; any failure up to there logs and keeps the running object and
 * its registrations. Only a changed source key compiles (on Windows
 * straight to the .hotN name, since the boot DLL is file-locked); a
 * reload for a config.toml or shader edit copies the object already
 * loaded to the fresh name, so it needs neither the SDK nor a compiler.
 * Then strips every registration, clears and re-parses the manifest,
 * re-registers key defaults, reloads config, recomputes the config and
 * shader keys, and re-runs entry; a failure from there leaves the mod
 * inert. The old object stays resident. The mod's own host state does not
 * cross the reload: entry re-derives it. On success the registration
 * surfaces take the mod's enabled state again and the toggle epoch is
 * bumped, since the reload changes what the active registration set
 * means to rewind and runahead. Clears reload_deferred: the request is
 * served, whatever its outcome. Callers test reload_must_defer first. */
static void mod_hot_reload(struct openpete_mod* mod) {
    char key[65];
    mod->reload_deferred = 0; /* the request is served from here */
    int need_compile = mod->build_key[0] != '\0' &&
                       mod_compute_key(mod->dir, s_sdk_dir, key) == 0 &&
                       strcmp(key, mod->build_key) != 0;
    OPL_INFO_F("mods", "hot reload: %s (%s)", mod->id,
               need_compile ? "source change" : "config or shader change");

    /* Build and open the fresh object BEFORE touching anything live: a
     * reload that cannot produce a loadable object keeps the running one
     * (registrations and all) instead of leaving the mod inert. The
     * Mods-panel Apply reaches this path, so a stray syntax error must
     * cost a log line, not the session's mod. */
    static unsigned s_hot_n = 0;
    char hot_path[sizeof mod->obj_path];
    if (snprintf(hot_path, sizeof hot_path, "%s/.build/%s/%s.hot%u" MOD_DL_EXT,
                 mod->dir, mods_build_game(), mod->id, ++s_hot_n)
        >= (int)sizeof hot_path) {
        OPL_ERROR_F("mods", "%s: reload object path exceeds %zu bytes - keeping "
                    "the running object", mod->id, sizeof hot_path - 1);
        return;
    }
    const char* src_obj = mod->obj_path;
    int obj_kind = mod->obj_kind;   /* a compile below makes it MOD_OBJ_GAME */
    if (need_compile) {
        if (!mods_sdk_present(s_sdk_dir)) {
            mods_sdk_missing_notice(s_sdk_dir);
            OPL_ERROR_F("mods", "%s: reload needs the SDK - keeping the "
                        "running object", mod->id);
            return;
        }
#ifdef _WIN32
        /* The boot-loaded base DLL is file-locked on Windows: compile
         * straight to the unique .hotN name and skip the stamp - the
         * base DLL goes stale, so the next boot recompiles it once and
         * re-stamps. */
        if (mod_compile(mod->dir, mod->id, s_sdk_dir, key, hot_path, 0) != 0) {
            OPL_ERROR_F("mods", "%s: reload compile failed - keeping the "
                        "running object", mod->id);
            return;
        }
        snprintf(mod->build_key, sizeof mod->build_key, "%s", key);
        src_obj = hot_path;
        obj_kind = MOD_OBJ_GAME;
#else
        static char so_path[MOD_PATH_MAX];
        if (build_mod(mod, s_sdk_dir, so_path, sizeof so_path) != 0) {
            OPL_ERROR_F("mods", "%s: reload compile failed - keeping the "
                        "running object", mod->id);
            return;
        }
        src_obj = so_path;
        obj_kind = MOD_OBJ_GAME;
#endif
    }
    if (strcmp(src_obj, hot_path) != 0) {
        /* copy: a fresh inode (a fresh path on Windows) guarantees the
         * loader opens a fresh object rather than handing back the one
         * already resident */
        if (op_atomic_copy_file(src_obj, hot_path) != 0) {
            OPL_ERROR_F("mods", "%s: reload stage copy failed - keeping "
                        "the running object", mod->id);
            return;
        }
    }
    void* dl = NULL;
    mod_entry_fn entry = mod_reload_open(mod, hot_path, obj_kind, &dl);
    if (!entry) return;

    /* A loadable object exists: now retire the running one. From here a
     * failure leaves the mod INERT - the old registrations are gone and
     * the new entry never completed - which the log says in as many
     * words. The outgoing object's shutdown hook runs first, so threads
     * it started stop before its registrations go. */
    (void)mod_run_shutdown_hook(mod, "reload");
    mod_strip_registrations(mod);
    /* Drop every manifest row in place, keeping the storage: the rows
     * are re-parsed below, and appending to them would duplicate every
     * binding, config row, material row and level block. */
    mod_rows_clear(mod);
    mod->state_decl = mod->ra_decl = 0;
    char dir_copy[MOD_PATH_MAX];
    snprintf(dir_copy, sizeof dir_copy, "%s", mod->dir);
    if (parse_manifest(dir_copy, mod) != 0) {
        OPL_ERROR_F("mods", "%s: reload manifest parse failed - mod is "
                    "now INERT until the next successful reload", mod->id);
        return;
    }
    mod_register_key_defaults(mod, mod->enabled);   /* bindings re-parsed */
    openpete_mods_config_load(mod, mod->dir, mod->log_tag);
    (void)config_compute_key(mod->dir, mod->cfg_key);
    (void)openpete_mods_shader_watch_key(mod->dir, mod->shader_key);
    mod_order_note(mod, OPENPETE_MODSET_EV_ENTRY, 0, 0);
    openpete_mods_arena_entry_begin(mod->id); /* replay its allocations */
    const openpete_mods_site_t site = openpete_mods_site_open(OPENPETE_MODS_CTX_RELOAD_ENTRY);
    openpete_mods_guard_enter(mod->id);
    mod->in_entry = 1;
    const int entry_rc = entry(&s_api, mod);
    mod->in_entry = 0;
    openpete_mods_guard_leave();
    openpete_mods_site_close(site);
    openpete_mods_arena_entry_end(mod->id);
    if (entry_rc != 0) {
        OPL_ERROR_F("mods", "%s: reload entry failed - mod is now INERT",
                    mod->id);
        mod_strip_registrations(mod);
        return;
    }
    mod->dl = dl; /* old object deliberately stays resident */
    snprintf(mod->obj_path, sizeof mod->obj_path, "%s", hot_path);
    mod->obj_kind = obj_kind;
    mod_resolve_ui_section(mod, dl);
    /* Re-entry registered every surface in its default (enabled) state;
     * a live-disabled mod must come back disabled, not resurrected. And
     * a reload changes what the active registration set means, so the
     * timeline-derived state (rewind ring, runahead snapshot) is stale
     * either way - bump the modset epoch. */
    openpete_mods_chain_set_owner_enabled(mod->id, mod->enabled);
    openpete_mods_draw_set_owner_enabled(mod->id, mod->enabled);
    openpete_mods_registry_set_owner_enabled(mod->id, mod->enabled);
    s_toggle_epoch++;
    OPL_INFO_F("mods", "hot reload: %s live%s", mod->id,
               mod->enabled ? "" : " (still disabled)");
}

/** @brief Decide whether a requested hot reload of a mod waits, setting reload_deferred when it does.
 *
 * A reload runs the outgoing object's shutdown hook and strips its
 * registrations, so it waits while the mod's code is on the frame stack (a
 * game fiber parked inside its override or bus handler, whose frames stay
 * on the sim thread's stack across the tick boundary) and while a
 * level-ready hook walk is parked, which may hold the mod's own hook with
 * no frame to show it. Nothing bounds the wait: an override of a function
 * that is on the stack at every park waits until it is toggled off, or for
 * the session. The first deferral of a request is logged, naming the
 * parked override's address. Both callers retry: the config loop at every
 * tick boundary, the hot-reload poll at every poll, which re-detects the
 * key mismatch.
 * @param mod The mod a reload is requested for.
 * @return 1 when the reload waits, 0 when it may run now. */
static int reload_must_defer(struct openpete_mod* mod) {
    uint32_t addr = 0;
    const int frame = openpete_mods_frame_owner_live(mod->id, &addr);
    if (!frame && !openpete_mods_registry_level_walk_active()) return 0;
    if (!mod->reload_deferred) {
        if (frame)
            OPL_INFO_F("mods", "hot reload of %s deferred: override of 0x%08X "
                       "on the stack of the parked game fiber; it applies at "
                       "the first tick boundary without it", mod->id, addr);
        else
            OPL_INFO_F("mods", "hot reload of %s deferred: a level-ready hook "
                       "walk is parked; it applies after the walk", mod->id);
    }
    mod->reload_deferred = 1;
    return 1;
}

/* --mod-hot-reload (OPENPETE_MOD_HOT_RELOAD): 1 arms hot_reload_poll; set
 * once in openpete_mods_init from opts. */
static int s_hot_reload = 0;

/** @brief Latch --mod-hot-reload into s_hot_reload and log once when it is armed.
 * @param opts Parsed options, or NULL for the default (off). */
static void mods_hot_reload_arm(const openpete_options_t* opts) {
    s_hot_reload = opts && opts->mod_hot_reload > 0;
    if (s_hot_reload) OPL_INFO_F("mods", "hot reload armed (dev mode)");
}

/** @brief Poll the entered source mods for edits every 64th tick boundary and hot-reload the changed ones.
 *
 * Active only under --mod-hot-reload (or OPENPETE_MOD_HOT_RELOAD), which
 * openpete_mods_init latches into s_hot_reload. Each poll rechecks the
 * SDK prefix, then recomputes, for every entered mod with an object, a src/
 * directory and a build key,
 * the source build key (sources and SDK), the config.toml key and the
 * shader key; any change calls mod_hot_reload unless reload_must_defer
 * holds it, in which case the next poll sees the same change. */
static void hot_reload_poll(void) {
    if (!s_hot_reload) return;
    static unsigned s_cnt = 0;
    if (++s_cnt % 64u) return;
    sdk_prefix_recheck(s_sdk_dir); /* an SDK edit must show up in
                                    * this round's keys */
    for (int i = 0; i < mod_count(); i++) {
        struct openpete_mod* mod = mod_at(i);
        /* Deferred-entry rows (never entered) reload nothing: no code
         * of theirs is live; their first enable uses the boot object. */
        if (!mod->dl || !mod->has_src || !mod->entered) continue;
        /* A prebuilt object loaded without the SDK has no build key and
         * no source watch. */
        if (!mod->build_key[0]) continue;
        /* Key-only recompute (pure hashing, ms); the compile happens
         * inside mod_hot_reload on a real change. config.toml is watched
         * too - a config-only edit reloads against the cached object, so
         * entry re-reads the fresh values. */
        char key[65], ckey[65], skey[65];
        if (mod_compute_key(mod->dir, s_sdk_dir, key) != 0)
            continue;
        (void)config_compute_key(mod->dir, ckey);
        (void)openpete_mods_shader_watch_key(mod->dir, skey);
        if ((strcmp(key, mod->build_key) != 0 ||
             strcmp(ckey, mod->cfg_key) != 0 ||
             strcmp(skey, mod->shader_key) != 0) &&
            !reload_must_defer(mod))
            mod_hot_reload(mod);
    }
}

/* ── Live modset toggle ──────────────────────────────────────────────
 * enable/disable apply at the NEXT TICK BOUNDARY (the canonical present
 * commit), never mid-tick: requests queue here and the apply flips
 * owner-keyed disabled flags across every registration surface - chain
 * links (skipped transparently, base() nesting preserved), present
 * hooks, and registry rows. Nothing is torn down, so re-enable restores
 * the mod exactly where it was. Asset overlays stay boot-scoped (a toggle
 * takes effect on restart): their consumers scan at init.
 *
 * The save modset tag deliberately does NOT rebuild on toggle: a
 * divergent mod that already ran this session has already diverged it;
 * the session tag is the loaded set, not the instantaneous one.
 *
 * Drivers: the dev overlay's Mods panel and the pause menu's mod row; and
 * OPENPETE_MODS_TOGGLE="tick:id:0|1[,...]" (a canonical-tick schedule)
 * for CI/dev. */


typedef struct {
    char id[MOD_ID_MAX];
    int  on;
    int  defer_logged; /* one deferral notice per request, not per tick */
} toggle_req_t;
/* Requests the queue allocates up front; it grows past this. */
#define TOGGLE_Q_INIT 16
static mod_fvec s_toggle_q;   /* toggle_req_t, drained per tick boundary */
static unsigned     s_toggle_epoch = 0; /* bumped per applied change */

unsigned openpete_mods_toggle_epoch(void) { return s_toggle_epoch; }

typedef struct { unsigned tick; char id[MOD_ID_MAX]; int on; } toggle_sched_t;
#define TOGGLE_SCHED_INIT 16
static mod_fvec s_toggle_sched;   /* toggle_sched_t, parsed once at boot */
static int mod_is_mutative(const struct openpete_mod* m);

int         openpete_mods_count(void) { return mod_count(); }
const char* openpete_mods_id(int i) {
    return (i >= 0 && i < mod_count()) ? mod_at(i)->id : NULL;
}
int openpete_mods_is_enabled(const char* id) {
    for (int i = 0; i < mod_count(); i++)
        if (strcmp(mod_at(i)->id, id) == 0) return mod_at(i)->enabled;
    return 0;
}

int openpete_mods_menu_info(int i, openpete_mods_menu_info_t* out) {
    if (i < 0 || i >= mod_count() || !out) return 0;
    const struct openpete_mod* m = mod_at(i);
    out->id          = m->id;
    out->name        = m->name[0] ? m->name : m->id;
    out->version     = m->version;
    out->enabled     = m->enabled;
    out->entered     = m->entered;
    out->sim_mutative = mod_is_mutative(m);
    out->has_ui      = m->ui_body != NULL || m->status_n > 0;
    out->ui_always   = m->ui_always;
    out->has_assets  = m->has_assets;
    out->has_src     = m->has_src;
    out->has_cfg     = mod_cfg_count(m);
    out->imgui_calls = m->imgui_calls;
    return 1;
}

int openpete_mods_bundle_paths(int i, const char** dir, const char** obj) {
    if (i < 0 || i >= mod_count()) return 0;
    const struct openpete_mod* m = mod_at(i);
    if (dir) *dir = m->dir;
    if (obj) *obj = m->obj_path;
    return 1;
}

/* ── Config-schema accessors (the Mods panel's data source) ────────── */

int openpete_mods_cfg_row_count(int mod_index) {
    if (mod_index < 0 || mod_index >= mod_count()) return 0;
    return mod_cfg_count(mod_at(mod_index));
}

int openpete_mods_cfg_row(int mod_index, int row,
                          openpete_mods_cfg_row_t* out) {
    if (mod_index < 0 || mod_index >= mod_count() || !out) return 0;
    struct openpete_mod* m = mod_at(mod_index);
    if (row < 0 || row >= mod_cfg_count(m)) return 0;
    const mod_cfg_row_t* r = mod_cfg_at(m, row);
    out->key     = r->key;
    out->type    = r->type;
    out->label   = r->label[0] ? r->label : r->key;
    out->group   = r->group;
    out->help    = r->help;
    out->min     = r->min;
    out->max     = r->max;
    out->secret  = r->secret;
    /* The combo view. A row stores its choices as fixed-width elements,
     * so a pointer into one would move with the row's storage; the
     * pointers are rebuilt here instead, and like `sbuf` below they are
     * valid until the next call. */
    static mod_fvec view;   /* const char* */
    int n_items = mod_cfg_item_count(r);
    if (!view.elem_size)
        mod_fvec_init(&view, sizeof(const char*), MOD_CFG_ITEMS_INIT,
                      NULL, 0);
    if (mod_fvec_resize(&view, (size_t)n_items) != 0) n_items = 0;
    for (int i = 0; i < n_items; i++)
        *(const char**)mod_fvec_at(&view, (size_t)i) = mod_cfg_item_at(r, i);
    out->items   = n_items ? (const char**)mod_fvec_data(&view) : NULL;
    out->n_items = n_items;
    out->ival    = 0;
    out->fval    = 0.0;
    out->sval    = "";
    static char sbuf[128];
    switch (r->type) {
    case 'b':
        out->ival = openpete_mods_config_bool(m, r->key, r->def_i != 0);
        break;
    case 'i':
        out->ival = openpete_mods_config_int(m, r->key, r->def_i);
        break;
    case 'd':
        out->fval = openpete_mods_config_float(m, r->key, r->def_d);
        break;
    case 's':
        (void)openpete_mods_config_str(m, r->key, r->def_s, sbuf,
                                       (int)sizeof sbuf);
        out->sval = sbuf;
        break;
    }
    return 1;
}

int openpete_mods_cfg_apply(int mod_index, int n,
                            const openpete_mods_cfg_edit_t* edits) {
    if (mod_index < 0 || mod_index >= mod_count() || n <= 0 || !edits)
        return -1;
    struct openpete_mod* m = mod_at(mod_index);
    if (openpete_mods_config_write(m->dir, m->log_tag, n, edits) != 0)
        return -1;
    m->cfg_reload_pending = 1;
    openpete_bugrec_taint(BUGREC_TAINT_MOD_CONFIG);
    return 0;
}

/** @brief Run a mod's UI section body from the overlay under the render-purity guard.
 *
 * Sets openpete_mods_in_present around the call, so guest access, game
 * calls and registration refuse inside it as in present hooks: widgets
 * mutate host-side mod state, tick hooks consume it. The ImGui body depth
 * is entered for mod index i, which makes the mod's ImGui calls legal.
 * Afterwards the call count is stored in imgui_calls and the first
 * non-zero count is logged once.
 * @param no_input nonzero marks a body the always pass runs with the dev
 *        overlay closed. */
static void mod_ui_run_body(struct openpete_mod* m, int i,
                            openpete_mod_ui_body_fn body,
                            const struct openpete_mod_ui* ui,
                            int no_input) {
    openpete_mods_in_present = 1;
    const openpete_mods_site_t site = openpete_mods_site_open(OPENPETE_MODS_CTX_UI);
    /* The body depth is what makes an ImGui call legal: the guard
     * wrappers drop a call from anywhere else. */
    openpete_imgui_body_enter_always(i, no_input);
    body(ui);
    openpete_imgui_body_leave();
    openpete_mods_site_close(site);
    openpete_mods_in_present = 0;
    m->imgui_calls = openpete_imgui_calls_take();
    if (m->imgui_calls && !m->imgui_engaged) {
        m->imgui_engaged = 1;
        OPL_INFO_F("mods", "%s: ImGui engaged (%llu calls)",
                   m->id, (unsigned long long)m->imgui_calls);
    }
}

void openpete_mods_ui_draw(int i, const struct openpete_mod_ui* ui) {
    if (i < 0 || i >= mod_count() || !ui || atomic_load(&s_api_closed)) return;
    struct openpete_mod* m = mod_at(i);
    if (!m->enabled) return;
    for (int l = 0; l < m->status_n; l++)
        ui->text("%s", m->status[l]);
    if (m->status_trunc)
        ui->text_disabled("... (ui_status block truncated)");
    /* An always section draws from the top-level pass instead: a body
     * inside the mod's tree node would vanish when the node is collapsed,
     * and drawing it in both places would run it twice per present. */
    openpete_mod_ui_body_fn body = m->ui_body;
    if (!body || m->ui_always) return;
    mod_ui_run_body(m, i, body, ui, 0);
}

void openpete_mods_ui_draw_always(int i, const struct openpete_mod_ui* ui,
                                  int no_input) {
    if (i < 0 || i >= mod_count() || !ui || atomic_load(&s_api_closed)) return;
    struct openpete_mod* m = mod_at(i);
    if (!m->enabled || !m->ui_always) return;
    openpete_mod_ui_body_fn body = m->ui_body;
    if (!body) return;
    mod_ui_run_body(m, i, body, ui, no_input);
}

int openpete_mods_ui_always_count(void) {
    int n = 0;
    for (int i = 0; i < mod_count(); i++) {
        struct openpete_mod* m = mod_at(i);
        if (m->ui_always && m->enabled && m->ui_body) n++;
    }
    return n;
}

int openpete_mods_ui_refuse_section(int i, const char* why) {
    if (i < 0 || i >= mod_count()) return -1;
    struct openpete_mod* m = mod_at(i);
    if (!m->ui_body) return 0;
    m->ui_body = NULL;
    m->imgui_calls = 0;  /* the panel's "last present" count is gone too */
    OPL_ERROR_F("mods", "%s: UI section refused for this session - %s",
                m->id, why ? why : "");
    return 0;
}

int openpete_mods_set_mod_enabled(const char* id, int on) {
    if (!id || !id[0]) return -1;
    if (!s_toggle_q.elem_size)
        mod_fvec_init(&s_toggle_q, sizeof(toggle_req_t), TOGGLE_Q_INIT,
                      "toggle_queue", TOGGLE_Q_INIT);
    toggle_req_t* r = (toggle_req_t*)mod_fvec_push(&s_toggle_q);
    if (!r) {
        OPL_ERROR_F("mods", "live toggle: request queue allocation failed "
                    "- \"%s\" not queued", id);
        return -1;
    }
    snprintf(r->id, MOD_ID_MAX, "%s", id);
    r->on = on ? 1 : 0;
    return 0;
}

/** @brief Return a malloc'd mutable copy of an environment variable.
 *
 * The callers tokenize it with strtok_r, which writes into its subject; a
 * fixed stack buffer would drop the tail of a long schedule. The caller
 * frees the copy.
 * @return the copy, or NULL when the variable is unset, empty, or the
 *         allocation fails. */
static char* env_dup(const char* name) {
    const char* e = getenv(name);
    if (!e || !*e) return NULL;
    size_t n = strlen(e) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, e, n);
    return d;
}

/** @brief Parse OPENPETE_MODS_TOGGLE into the toggle schedule.
 *
 * The variable is a comma-separated list of tick:id:0|1 tokens, each
 * appended to s_toggle_sched, with the id read up to 63 characters and any
 * nonzero flag stored as 1. A token that does not parse, or one whose row
 * cannot be allocated, logs one error naming the token and is skipped. An
 * unset or empty variable does nothing. Called once from
 * openpete_mods_init. */
static void toggle_parse_env(void) {
    char* buf = env_dup("OPENPETE_MODS_TOGGLE");
    if (!buf) return;
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        unsigned tick;
        char id[MOD_ID_MAX];
        int on;
        toggle_sched_t* t = NULL;
        if (sscanf(tok, "%u:%63[^:]:%d", &tick, id, &on) == 3) {
            if (!s_toggle_sched.elem_size)
                mod_fvec_init(&s_toggle_sched, sizeof(toggle_sched_t),
                              TOGGLE_SCHED_INIT, "toggle_sched",
                              TOGGLE_SCHED_INIT);
            t = (toggle_sched_t*)mod_fvec_push(&s_toggle_sched);
        }
        if (t) {
            t->tick = tick;
            snprintf(t->id, MOD_ID_MAX, "%s", id);
            t->on = on ? 1 : 0;
        } else {
            OPL_ERROR_F("mods", "OPENPETE_MODS_TOGGLE: bad token \"%s\" "
                        "(want tick:id:0|1)", tok);
        }
    }
    free(buf);
    size_t n_sched = mod_fvec_count(&s_toggle_sched);
    if (n_sched)
        OPL_INFO_F("mods", "toggle schedule armed (%zu entr%s)",
                   n_sched, n_sched == 1 ? "y" : "ies");
}

/* Scheduled config edits (harness): OPENPETE_MODS_CFG_APPLY=
 * "tick:id:key=int[,...]" commits an integer config edit through the
 * same path the Mods panel's Apply uses - config.toml rewritten, reload
 * queued, entry re-run at the next tick boundary - so a headless run can
 * exercise re-entry deterministically. */
typedef struct {
    unsigned tick;
    char     id[MOD_ID_MAX];
    char     key[64];
    int64_t  val;
} cfg_apply_sched_t;
#define CFG_SCHED_INIT 8
static mod_fvec s_cfg_sched;   /* cfg_apply_sched_t, parsed once at boot */

/** @brief Parse OPENPETE_MODS_CFG_APPLY into the config-apply schedule.
 *
 * The variable holds comma-separated `tick:id:key=int` entries; a token
 * that does not scan, or that cannot be stored, is logged and skipped.
 * Runs once at init. */
static void cfg_apply_schedule_parse(void) {
    char* buf = env_dup("OPENPETE_MODS_CFG_APPLY");
    if (!buf) return;
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok;
         tok = strtok_r(NULL, ",", &save)) {
        cfg_apply_sched_t t;
        long long v;
        cfg_apply_sched_t* slot = NULL;
        if (sscanf(tok, "%u:%63[^:]:%63[^=]=%lld", &t.tick, t.id, t.key,
                   &v) == 4) {
            if (!s_cfg_sched.elem_size)
                mod_fvec_init(&s_cfg_sched, sizeof(cfg_apply_sched_t),
                              CFG_SCHED_INIT, "cfg_sched", CFG_SCHED_INIT);
            slot = (cfg_apply_sched_t*)mod_fvec_push(&s_cfg_sched);
        }
        if (slot) {
            t.val = (int64_t)v;
            *slot = t;
        } else {
            OPL_ERROR_F("mods", "OPENPETE_MODS_CFG_APPLY: bad token \"%s\" "
                        "(want tick:id:key=int)", tok);
        }
    }
    free(buf);
    size_t n_cfg = mod_fvec_count(&s_cfg_sched);
    if (n_cfg)
        OPL_INFO_F("mods", "config-apply schedule armed (%zu entr%s)",
                   n_cfg, n_cfg == 1 ? "y" : "ies");
}

/** @brief Commit every scheduled integer config edit due at a tick.
 *
 * Each due entry names a mod by id and goes through openpete_mods_cfg_apply
 * as a one-edit integer set, the path the Mods panel's Apply takes; an
 * unknown id is logged and skipped.
 * @param canonical_tick The tick now starting. */
static void cfg_apply_schedule_run(unsigned canonical_tick) {
    for (size_t i = 0; i < mod_fvec_count(&s_cfg_sched); i++) {
        cfg_apply_sched_t* t = (cfg_apply_sched_t*)mod_fvec_at(&s_cfg_sched, i);
        if (t->tick != canonical_tick) continue;
        int idx = -1;
        for (int m = 0; m < mod_count(); m++)
            if (strcmp(mod_at(m)->id, t->id) == 0) { idx = m; break; }
        if (idx < 0) {
            OPL_ERROR_F("mods", "config-apply schedule: no mod \"%s\"", t->id);
            continue;
        }
        openpete_mods_cfg_edit_t e;
        memset(&e, 0, sizeof e);
        e.key = t->key; e.type = 'i'; e.ival = t->val;
        OPL_INFO_F("mods", "config-apply schedule: %s %s=%lld at tick %u",
                   t->id, t->key, (long long)t->val, canonical_tick);
        (void)openpete_mods_cfg_apply(idx, 1, &e);
    }
}

/** @brief Serve the Apply-committed config changes at a tick boundary.
 *
 * An entered mod with an object and sources is reloaded so its entry
 * re-reads the fresh values (build key unchanged, so the compile is a
 * cache hit); while reload_must_defer holds the reload, cfg_reload_pending
 * stays set and the next boundary retries. A mod that never entered
 * (deferred row, assets-only) re-parses the file: entry reads it on first
 * enable. */
static void cfg_reload_drain(void) {
    for (int i = 0; i < mod_count(); i++) {
        struct openpete_mod* mod = mod_at(i);
        if (!mod->cfg_reload_pending) continue;
        if (!(mod->dl && mod->has_src && mod->entered)) {
            mod->cfg_reload_pending = 0;
            openpete_mods_config_load(mod, mod->dir, mod->log_tag);
        } else if (!reload_must_defer(mod)) {
            mod->cfg_reload_pending = 0;
            mod_hot_reload(mod);
        }
    }
}

/** @brief Run a mod's toggle hook after a flip, in a toggle-hook dispatch site and its guest-guard stretch.
 *
 * Tick context: the mod may undo (or re-stage) its guest-side effects with
 * the surfaces already in the new state.
 * @param mod The flipped mod; one with no hook does nothing.
 * @param cpu Live game CPU.
 * @param on The new enabled state. */
static void toggle_hook_run(struct openpete_mod* mod, CPUState* cpu, int on) {
    if (!mod->toggle_fn) return;
    const openpete_mods_site_t site = openpete_mods_site_open(OPENPETE_MODS_CTX_TOGGLE);
    openpete_mods_guard_enter(mod->id);
    mod->toggle_fn(cpu, on);
    openpete_mods_guard_leave();
    openpete_mods_site_close(site);
}

/** @brief Find a loaded mod by id.
 * @param id The mod id.
 * @return The mod, or NULL. */
static struct openpete_mod* mod_by_id(const char* id) {
    for (int i = 0; i < mod_count(); i++)
        if (strcmp(mod_at(i)->id, id) == 0) return mod_at(i);
    return NULL;
}

/** @brief Drain the live-toggle queue at a tick boundary, one request flipped and its hook run before the next.
 *
 * The queue is drained in place: a deferred request (a disable of the mod
 * that owns the level the player is in) is compacted down to `kept` and the
 * tail is dropped at the end. The bound is re-read per iteration, so a
 * request queued from a toggle body is drained by this same pass. The
 * request is copied out first: that body's queue push may move the run
 * under a held element pointer. An applied flip bumps the toggle epoch.
 * @param cpu Live game CPU.
 * @param canonical_tick The tick now starting, for the log line. */
static void toggle_queue_drain(CPUState* cpu, unsigned canonical_tick) {
    size_t kept = 0;
    for (size_t q = 0; q < mod_fvec_count(&s_toggle_q); q++) {
        toggle_req_t cur = *(toggle_req_t*)mod_fvec_at(&s_toggle_q, q);
        const char* id = cur.id;
        int on = cur.on;
        struct openpete_mod* mod = mod_by_id(id);
        if (!mod) {
            OPL_ERROR_F("mods", "live toggle: no loaded mod \"%s\"", id);
            continue;
        }
        if (mod->enabled == on) continue; /* no-op: don't bump the epoch */
        /* A disable that would strand the player (the mod owns the level
         * they are inside) DEFERS: the request stays queued and lands
         * the moment they leave the slot. The epoch must not bump until
         * it actually applies. */
        if (!on && openpete_mods_registry_owner_busy(id)) {
            if (!cur.defer_logged) {
                OPL_INFO_F("mods", "live toggle: \"%s\" deferred - can't "
                           "be turned off while inside its level", id);
                cur.defer_logged = 1;
            }
            *(toggle_req_t*)mod_fvec_at(&s_toggle_q, kept++) = cur;
            continue;
        }
        if (on && !mod->entered && mod_enter_deferred(mod) != 0) {
            OPL_ERROR_F("mods", "live toggle: \"%s\" failed to start - "
                        "staying disabled", id);
            continue;
        }
        openpete_mods_chain_set_owner_enabled(id, on);
        openpete_mods_draw_set_owner_enabled(id, on);
        openpete_mods_registry_set_owner_enabled(id, on);
        openpete_keymap_set_mod_enabled(id, on);
        if (!on) openpete_host_mouse_request_capture(
                     OPENPETE_MOUSE_CAP_MOD0 + mod->load_index, 0);
        mod->enabled = on;
        s_toggle_epoch++;
        OPL_INFO_F("mods", "live toggle: %s %s (tick %u)",
                   id, on ? "ENABLED" : "DISABLED", canonical_tick);
        /* Notify AFTER the flip. */
        toggle_hook_run(mod, cpu, on);
    }
    mod_fvec_truncate(&s_toggle_q, kept);
}

void openpete_mods_tick_boundary(CPUState* cpu, unsigned canonical_tick) {
    s_current_tick = canonical_tick;
    openpete_mods_game_state_sample(cpu, canonical_tick);
    for (size_t i = 0; i < mod_fvec_count(&s_toggle_sched); i++) {
        const toggle_sched_t* t =
            (const toggle_sched_t*)mod_fvec_at(&s_toggle_sched, i);
        if (t->tick == canonical_tick)
            (void)openpete_mods_set_mod_enabled(t->id, t->on);
    }
    cfg_apply_schedule_run(canonical_tick);
    hot_reload_poll();

    /* Both of these are things a UI section may have asked for from inside a
     * present, where they could not be honoured: a retuned selector would
     * have split the frame between two keys, and the class census reads
     * guest RAM. The tick boundary is where they become legal. */
    openpete_mods_material_apply_pending();
    openpete_mods_material_overlap_tick();
    if (mod_count()) openpete_mods_moby_class_census(cpu);

    cfg_reload_drain();
    toggle_queue_drain(cpu, canonical_tick);
    /* A registration, toggle or hot reload above can change the member
     * set of the combined material modules; unchanged, this returns at once. */
    openpete_mods_material_chain_sync();
}

void openpete_mods_shutdown(void (*progress)(void)) {
    static int s_ran;
    if (s_ran) return;
    s_ran = 1;
    int hooks = 0;
    for (int i = mod_count() - 1; i >= 0; i--) {
        struct openpete_mod* mod = mod_at(i);
        hooks += mod_run_shutdown_hook(mod, "exit");
        atomic_store(&mod->api_closed, 1);
        if (progress) progress();
    }
    atomic_store(&s_api_closed, 1);
    const Uint64 t0 = SDL_GetTicks(); /* clock-guard: wall */
    int left;
    while ((left = atomic_load(&s_api_inflight)) > 0 &&
           SDL_GetTicks() - t0 < MOD_SHUTDOWN_DRAIN_MS) /* clock-guard: wall */
        SDL_Delay(1);
    if (left > 0)
        OPL_WARN_F("mods", "shutdown: %d api call(s) still in progress "
                   "after %d ms; tearing down anyway", left,
                   (int)MOD_SHUTDOWN_DRAIN_MS);
    if (mod_count() > 0)
        OPL_INFO_F("mods", "shutdown: %d hook(s) ran over %d mod(s); the api "
                   "is closed", hooks, mod_count());
}

const char* openpete_mods_build_key(const char* id) {
    if (!id) return NULL;
    for (int i = 0; i < mod_count(); i++)
        if (strcmp(mod_at(i)->id, id) == 0) return mod_at(i)->build_key;
    return NULL;
}

/* A code mod is one with a loaded object (mod->dl), source or prebuilt. */
int openpete_mods_runahead_capable(const char** offender) {
    for (int i = 0; i < mod_count(); i++) {
        if (!mod_at(i)->enabled || !mod_at(i)->dl) continue;
        if (!mod_at(i)->ra_decl) {
            if (offender) *offender = mod_at(i)->id;
            return 0;
        }
    }
    if (offender) *offender = NULL;
    return 1;
}

/* Same scan as openpete_mods_runahead_capable over state_decl. */
int openpete_mods_state_safe(const char** offender) {
    for (int i = 0; i < mod_count(); i++) {
        if (!mod_at(i)->enabled || !mod_at(i)->dl) continue;
        if (!mod_at(i)->state_decl) {
            if (offender) *offender = mod_at(i)->id;
            return 0;
        }
    }
    if (offender) *offender = NULL;
    return 1;
}

/** @brief Decide whether a mod changes gameplay state.
 *
 * An explicit sim_mutative (0 or 1) wins. With none, a mod that claims a
 * level slot is mutative, since it changes what the player progresses
 * through; a mod that loads no code object and claims no level has no
 * path to guest RAM and is not; every other mod with an object is assumed
 * to mutate, so a forgotten declaration produces a spurious save warning
 * rather than a mistagged card.
 * @return 1 when mutative, 0 otherwise. */
static int mod_is_mutative(const struct openpete_mod* m) {
    if (m->sim_mutative >= 0) return m->sim_mutative;
    return (m->dl != NULL) || (mod_level_count(m) > 0);
}

/* The cached modset tags: one heap block holding the set tag, its NUL,
 * the ordered tag and its NUL; NULL when unbuilt or when the allocation
 * failed. mod_enter_deferred clears s_tag_built. */
static char* s_tag;
static int   s_tag_built = 0;

/* Returned by both accessors when the tag block cannot be allocated: no
 * stored tag equals it, so a load is refused rather than admitted under
 * the empty (vanilla) tag. */
#define MODSET_TAG_FAILED "(modset tag allocation failed)"

/** @brief Return a mod's override-order event @p i, or NULL past the end. */
static const mod_order_ev_t* mod_order_at(const struct openpete_mod* m,
                                          size_t i) {
    return (const mod_order_ev_t*)mod_fvec_at(&m->order_ev, i);
}

/** @brief Record an override-order event of @p mod with the session's next sequence number, and drop the cached modset tags.
 *
 * The next number is one above the newest any mod holds, so the sequence
 * needs no counter beside the mod table.
 * @param mod The mod.
 * @param kind An openpete_modset_event_kind_t.
 * @param addr LINK: the chain's guest vaddr; else 0.
 * @param overlay LINK: the link's overlay binding; else 0. */
static void mod_order_note(struct openpete_mod* mod, int kind, uint32_t addr,
                           uint32_t overlay) {
    int seq = 0;
    for (int i = 0; i < mod_count(); i++) {
        const size_t n = mod_fvec_count(&mod_at(i)->order_ev);
        if (n && mod_order_at(mod_at(i), n - 1)->seq > seq)
            seq = mod_order_at(mod_at(i), n - 1)->seq;
    }
    if (!mod->order_ev.elem_size)
        mod_fvec_init(&mod->order_ev, sizeof(mod_order_ev_t), 8, NULL, 0);
    mod_order_ev_t* e = (mod_order_ev_t*)mod_fvec_push(&mod->order_ev);
    if (!e) {
        OPL_ERROR_F("mods", "%s: override-order event allocation failed",
                    mod->id);
    } else {
        e->seq = seq + 1;
        e->kind = kind;
        e->addr = addr;
        e->overlay = overlay;
    }
    s_tag_built = 0;
}

void openpete_mods_link_registered(const char* owner_id, uint32_t addr,
                                   uint32_t overlay) {
    if (!owner_id) return;
    for (int i = 0; i < mod_count(); i++) {
        struct openpete_mod* m = mod_at(i);
        if (strcmp(m->id, owner_id) != 0) continue;
        if (!m->in_entry)
            mod_order_note(m, OPENPETE_MODSET_EV_LINK, addr, overlay);
        return;
    }
}

/** @brief Hash the override-order events of the entered mutative mods, merged in session order. */
static uint64_t modset_order_hash(void) {
    mod_fvec ev;
    mod_fvec_init(&ev, sizeof(openpete_modset_event_t), 64, NULL, 0);
    mod_fvec seqs;
    mod_fvec_init(&seqs, sizeof(int), 64, NULL, 0);
    for (int i = 0; i < mod_count(); i++) {
        const struct openpete_mod* m = mod_at(i);
        if (!m->entered || !mod_is_mutative(m)) continue;
        for (size_t k = 0; k < mod_fvec_count(&m->order_ev); k++) {
            const mod_order_ev_t* o = mod_order_at(m, k);
            openpete_modset_event_t* e =
                (openpete_modset_event_t*)mod_fvec_push(&ev);
            int* sq = (int*)mod_fvec_push(&seqs);
            if (!e || !sq) break;
            e->kind = (openpete_modset_event_kind_t)o->kind;
            e->id = m->id;
            e->addr = o->addr;
            e->overlay = o->overlay;
            *sq = o->seq;
        }
    }
    /* Insertion sort by sequence number: each mod's run is already in
     * order, and the merged count is the session's event count. */
    openpete_modset_event_t* e = (openpete_modset_event_t*)mod_fvec_data(&ev);
    int* sq = (int*)mod_fvec_data(&seqs);
    const int n = (int)mod_fvec_count(&seqs);
    for (int i = 1; i < n; i++) {
        const openpete_modset_event_t ke = e[i];
        const int ks = sq[i];
        int j = i - 1;
        while (j >= 0 && sq[j] > ks) {
            e[j + 1] = e[j];
            sq[j + 1] = sq[j];
            j--;
        }
        e[j + 1] = ke;
        sq[j + 1] = ks;
    }
    const uint64_t h = openpete_modset_order_hash(e, n);
    mod_fvec_free(&ev);
    mod_fvec_free(&seqs);
    return h;
}

/** @brief Build the cached tag block from the entered mutative mods and their order events. */
static void modset_tags_build(void) {
    mod_fvec pick;
    int n = 0;
    s_tag_built = 1;
    free(s_tag);
    s_tag = NULL;
    mod_fvec_init(&pick, sizeof(openpete_modset_member_t), 64, NULL, 0);
    for (int i = 0; i < mod_count(); i++) {
        const struct openpete_mod* m = mod_at(i);
        if (!m->entered || !mod_is_mutative(m)) continue;
        openpete_modset_member_t* slot =
            (openpete_modset_member_t*)mod_fvec_push(&pick);
        if (!slot) break;
        slot->id = m->id;
        slot->version = m->version;
        n++;
    }
    const openpete_modset_member_t* ms =
        (const openpete_modset_member_t*)mod_fvec_data(&pick);
    const size_t set_len = openpete_modset_format_set(NULL, 0, ms, n);
    const uint64_t oh = modset_order_hash();
    const size_t ord_len = openpete_modset_format_order(NULL, 0, ms, n, oh);
    s_tag = (char*)malloc(set_len + ord_len + 2);
    if (s_tag) {
        openpete_modset_format_set(s_tag, set_len + 1, ms, n);
        openpete_modset_format_order(s_tag + set_len + 1, ord_len + 1, ms, n,
                                     oh);
    } else {
        OPL_ERROR_F("mods", "modset tag: allocation of %zu bytes failed",
                    set_len + ord_len + 2);
    }
    mod_fvec_free(&pick);
}

/* Only mods whose entry has run can have diverged the session; a
 * deferred-entry row joins at its first enable, which clears s_tag_built,
 * and memcard reads the tag per check and write, so later writes carry the
 * grown tag. */
const char* openpete_mods_modset_tag(void) {
    if (!s_tag_built) modset_tags_build();
    return s_tag ? s_tag : MODSET_TAG_FAILED;
}

const char* openpete_mods_modset_order_tag(void) {
    if (!s_tag_built) modset_tags_build();
    return s_tag ? s_tag + strlen(s_tag) + 1 : MODSET_TAG_FAILED;
}







/** @brief Report whether any source of a mod's declared binding is down.
 *
 * The sources come from openpete_keymap_mod_binding: the mod's own
 * [[binding]] key at rank 0 and a [keys.mod.<id>] config row above it.
 * The config override lets a user resolve a clash between two mods, or
 * between a mod and a host hotkey, without editing either mod, and it
 * reaches gamepad sources, which the mod.toml `key` spelling cannot.
 * An undeclared name logs an error and reads 0.
 * @return 1 when a source is down, 0 otherwise. */
static int api_binding_down(openpete_mod_t* self, const char* name) {
    if (!self || !name) return 0;
    if (!openpete_mods_in_present) openpete_bugrec_taint(BUGREC_TAINT_MOD_INPUT);

    /* Every declared [[binding]] is registered with the keymap at load
     * (the mod's own key at rank 0, a [keys.mod.<id>] row above it), so
     * the read is a keymap source read: it honours the keyboard suspend
     * and the consumed-key latch like every host action, and a key that
     * did not parse reads 0. */
    km_source_t src[KM_MAX_SOURCES];
    int n = 0;
    if (openpete_keymap_mod_binding(self->id, name, src, KM_MAX_SOURCES, &n)) {
        for (int i = 0; i < n; i++)
            if (openpete_keymap_source_down(&src[i]))
                return 1;
        return 0;
    }
    OPL_ERROR_F(self->log_tag, "binding \"%s\" not declared in mod.toml",
                name);
    return 0;
}

/** @brief Fill a mod's mouse snapshot: position, buttons and the motion and wheel since its last read.
 *
 * Deltas are measured against a marker per hook kind: tick hooks run on
 * the sim worker and present hooks on the present thread, so one marker
 * would split the deltas between the two. The first read of a kind
 * reports zero deltas. A tick hook under speculation or rewind replay
 * reads without advancing its marker, so the committed tick that follows
 * sees the whole motion. While the overlay owns the mouse, deltas, wheel
 * and buttons read zero and owned_by_overlay is set.
 * @return 0, or -1 for a struct_size mismatch (logged) or no host mouse. */
static int api_mouse_read(openpete_mod_t* self, openpete_mod_mouse_t* out) {
    if (!self || !out) return -1;
    if (out->struct_size != (uint32_t)sizeof(openpete_mod_mouse_t)) {
        OPL_ERROR_F(self->log_tag, "mouse_read: struct_size %u, engine expects %u "
                    "(rebuild against this SDK)", out->struct_size,
                    (unsigned)sizeof(openpete_mod_mouse_t));
        return -1;
    }
    if (!openpete_host_mouse_initialised()) return -1;
    const int kind = openpete_mods_in_present ? 1 : 0;
    if (kind == 0) openpete_bugrec_taint(BUGREC_TAINT_MOD_INPUT);
    /* A tick hook under speculation or rewind replay reads the deltas
     * without consuming them: the committed tick that follows sees the
     * whole motion. */
    const int advance = kind == 1 || (!openpete_speculative && !openpete_replaying);

    openpete_mouse_state_t st;
    openpete_host_input_mouse(&st);
    uint64_t press[OPENPETE_MOUSE_BUTTONS];
    for (int b = 0; b < OPENPETE_MOUSE_BUTTONS; b++)
        press[b] = openpete_host_mouse_press_total(b);
    const int64_t mx = openpete_host_mouse_motion_total(0);
    const int64_t my = openpete_host_mouse_motion_total(1);
    const int64_t wh = openpete_host_mouse_wheel_total();
    const int owned = openpete_host_mouse_owned() != OPENPETE_MOUSE_OWNED_NONE;

    struct openpete_mod* m = self;
    if (!m->mouse_mark[kind].valid) {
        /* First read from this cadence: nothing before it is a delta. */
        memcpy(m->mouse_mark[kind].press, press, sizeof press);
        m->mouse_mark[kind].mx = mx; m->mouse_mark[kind].my = my;
        m->mouse_mark[kind].wheel = wh;
        m->mouse_mark[kind].valid = 1;
    }
    const int64_t dx = mx - m->mouse_mark[kind].mx;
    const int64_t dy = my - m->mouse_mark[kind].my;
    const int64_t dw = wh - m->mouse_mark[kind].wheel;
    if (advance) {
        memcpy(m->mouse_mark[kind].press, press, sizeof press);
        m->mouse_mark[kind].mx = mx; m->mouse_mark[kind].my = my;
        m->mouse_mark[kind].wheel = wh;
    }

    memset(out, 0, sizeof *out);
    out->struct_size = (uint32_t)sizeof(openpete_mod_mouse_t);
    out->px = st.px; out->py = st.py;
    out->x  = st.win_h > 0.0f ? st.px / st.win_h : 0.0f;
    out->y  = st.win_h > 0.0f ? st.py / st.win_h : 0.0f;
    out->in_window = st.in_window;
    out->captured  = (uint8_t)openpete_host_mouse_captured();
    out->owned_by_overlay = (uint8_t)owned;
    if (!owned) {
        out->dx = (float)dx; out->dy = (float)dy;
        out->wheel = (float)dw / (float)OPENPETE_MOUSE_WHEEL_UNIT;
        out->buttons = st.buttons;
    }
    return 0;
}

/** @brief Request or release the host mouse capture on the mod's behalf.
 *
 * The request is keyed to the mod's load index, so it does not disturb the
 * camera, the harness or another mod, and mod_strip_registrations releases
 * it.
 * @param self The calling mod; NULL returns -1.
 * @param on Nonzero to request capture, zero to release.
 * @return 0, or -1 for a NULL self. */
static int api_mouse_capture(openpete_mod_t* self, int on) {
    if (!self) return -1;
    openpete_host_mouse_request_capture(
        OPENPETE_MOUSE_CAP_MOD0 + self->load_index, on);
    return 0;
}

/** @brief Fill a mod's pad snapshot for one slot from the input-log record staged for the running tick.
 *
 * The bytes are the ones the log stores for that tick, so the read
 * reproduces wherever the record does. Slots 1-3 never reach guest RAM.
 * libpad holds the PADRAW form: status 0x00, type 0x73, the button word
 * low byte first, then the right and left stick axes; an absent port reads
 * status 0xFF and logs once per slot.
 * @return 0, or -1 for a struct_size mismatch or an out-of-range slot
 *         (logged), or a call from a present hook (logged once). */
static int api_pad_read(openpete_mod_t* self, uint32_t slot,
                        openpete_mod_pad_t* out) {
    if (!self || !out) return -1;
    if (out->struct_size != (uint32_t)sizeof(openpete_mod_pad_t)) {
        OPL_ERROR_F(self->log_tag, "pad_read: struct_size %u, engine expects %u "
                    "(rebuild against this SDK)", out->struct_size,
                    (unsigned)sizeof(openpete_mod_pad_t));
        return -1;
    }
    if (slot >= (uint32_t)OPENPETE_MOD_PAD_SLOTS) {
        OPL_ERROR_F(self->log_tag, "pad_read: slot %u out of range (0..%d)",
                    slot, OPENPETE_MOD_PAD_SLOTS - 1);
        return -1;
    }
    if (openpete_mods_in_present) {
        if (!self->pad_present_logged) {
            self->pad_present_logged = 1;
            OPL_ERROR_F(self->log_tag, "pad_read refused inside a present hook: "
                        "the staged pad record belongs to the tick - read it in "
                        "a tick hook and display from your own copy");
        }
        return -1;
    }

    openpete_input_port_t p;
    input_log_read_slot((int)slot, &p);
    memset(out, 0, sizeof *out);
    out->struct_size = (uint32_t)sizeof(openpete_mod_pad_t);
    out->slot    = slot;
    out->vbl     = input_log_staged_vbl();
    out->present = p.present ? 1u : 0u;
    out->buttons = p.bits;
    memcpy(out->axes, p.axes, sizeof out->axes);
    if (p.present) {
        /* PADRAW: no error, type 7 length 3 (digital+analog), the button
         * word low byte first, then right stick, then left. */
        out->libpad[0] = 0x00;
        out->libpad[1] = 0x73;
        out->libpad[2] = (uint8_t)(p.bits & 0xFFu);
        out->libpad[3] = (uint8_t)(p.bits >> 8);
        out->libpad[4] = p.axes[2];
        out->libpad[5] = p.axes[3];
        out->libpad[6] = p.axes[0];
        out->libpad[7] = p.axes[1];
    } else {
        /* A port with nothing in it: status FF, the rest zero. */
        out->libpad[0] = 0xFF;
        if (!(self->pad_absent_logged & (1u << slot))) {
            self->pad_absent_logged |= (1u << slot);
            OPL_INFO_F(self->log_tag,
                       "pad_read: slot %u absent - no device assigned", slot);
        }
    }
    return 0;
}

/** @brief Drive the rumble motors of pad slot 1 to 3 for the mod.
 *
 * Slot 0 is the game's own actuator table and is refused, with an error
 * logged once; a slot at or above OPENPETE_MOD_PAD_SLOTS is refused with an
 * error each call. Inside a present hook it is refused; during speculative
 * or replayed ticks it is accepted and does nothing. The slot is recorded so
 * mod_strip_registrations stops its motors.
 * @param self The calling mod; NULL returns -1.
 * @param slot Pad slot.
 * @param small_byte Small motor strength; only the low 8 bits are used.
 * @param large_byte Large motor strength; only the low 8 bits are used.
 * @return 0, or -1 for a NULL self, a refused slot or a present-hook
 *         call. */
static int api_pad_rumble(openpete_mod_t* self, uint32_t slot,
                          uint32_t small_byte, uint32_t large_byte) {
    if (!self) return -1;
    if (slot == 0) {
        if (!self->pad_rumble_slot0_logged) {
            self->pad_rumble_slot0_logged = 1;
            OPL_ERROR_F(self->log_tag, "pad_rumble: slot 0 is the game's own "
                        "actuator table - a second writer would fight it; "
                        "drive slots 1..%d", OPENPETE_MOD_PAD_SLOTS - 1);
        }
        return -1;
    }
    if (slot >= (uint32_t)OPENPETE_MOD_PAD_SLOTS) {
        OPL_ERROR_F(self->log_tag, "pad_rumble: slot %u out of range (1..%d)",
                    slot, OPENPETE_MOD_PAD_SLOTS - 1);
        return -1;
    }
    if (purity_violation("pad_rumble")) return -1;
    /* A speculative tick's timeline is one the player never lived, and a
     * rewind burst re-executes tens of ticks at burst speed: replaying the
     * motors for either is a side effect escaping the re-execution, exactly
     * like audio. The call is accepted and does nothing, so a mod driving
     * rumble from a tick hook needs no speculation test of its own. */
    if (openpete_speculative || openpete_replaying) return 0;

    self->pad_rumble_slots |= 1u << slot;
    openpete_rumble_mod_set((int)slot, (int)(small_byte & 0xFFu),
                            (int)(large_byte & 0xFFu));
    return 0;
}

/** @brief Register a parsed manifest's [[binding]] keys with the keymap under the mod.<id> domain.
 *
 * Runs for every discovered mod at init, for the --keys-report path (which
 * prints before any mod loads), and after a reload re-parses a manifest.
 * @param mod Mod whose manifest rows are parsed.
 * @param enabled Whether the mod's bindings take part in the keymap scans. */
static void mod_register_key_defaults(const struct openpete_mod* mod, int enabled) {
    char where[MOD_PATH_MAX + 16];
    snprintf(where, sizeof where, "%s/mod.toml", mod->dir);
    for (int i = 0; i < mod_binding_count(mod); i++)
        (void)openpete_keymap_set_mod_default(mod->id,
                                              mod_binding_at(mod, i)->name,
                                              mod_binding_at(mod, i)->key,
                                              where);
    openpete_keymap_set_mod_enabled(mod->id, enabled);
}

/** @brief Allocate from the guest scratch arena for a mod; tick context only.
 *
 * The arena is sim-visible state, so a present-hook call is refused.
 * Flags other than OP_GALLOC_IMMUTABLE and OP_GALLOC_NO_DMA are refused
 * with an error.
 * @return the guest vaddr from openpete_mods_guest_alloc, or 0 on refusal;
 *         *host_view is NULL on refusal. */
static uint32_t api_guest_alloc(openpete_mod_t* self, uint32_t size,
                                uint32_t align, uint32_t flags,
                                void** host_view) {
    if (host_view) *host_view = NULL;
    if (!self) return 0;
    if (purity_violation("guest_alloc")) return 0;
    if (flags & ~(uint32_t)(OP_GALLOC_IMMUTABLE | OP_GALLOC_NO_DMA)) {
        OPL_ERROR_F("mods", "%s: guest_alloc: unknown flags %#x", self->id,
                    flags);
        return 0;
    }
    return openpete_mods_guest_alloc(size, align, flags, host_view,
                                     self->id);
}


/** @brief Play `<asset dir>/sfx/own/<name>.wav` once through openpete_mod_sound_play.
 *
 * Host-side and additive, so it carries no purity gate and is legal from
 * present hooks. A name containing ".." is refused.
 * @return 0 when played or muted, -1 on refusal or failure. */
static int api_sfx_play(openpete_mod_t* self, const char* name,
                          float gain) {
    if (!self || !name || strstr(name, "..")) return -1;
    char path[MOD_PATH_MAX + 96];
    snprintf(path, sizeof path, "%s/sfx/own/%s.wav", self->assets_dir, name);
    return openpete_mod_sound_play(path, gain);
}

const char* openpete_mods_id_of(openpete_mod_t* self) {
    return self ? self->id : NULL;
}

const char* openpete_mods_dir_of(openpete_mod_t* self) {
    return self ? self->dir : NULL;
}

int openpete_mods_load_index_of(openpete_mod_t* self) {
    if (!self) return -1;
    return self->load_index;
}

int openpete_mods_api_pin_of(openpete_mod_t* self) {
    return self ? (int)self->api_pin : 0;
}

const char* openpete_mods_sdk_dir(void) {
    return s_sdk_dir;
}

/* ---- pillar 9: game data + texture packs (api 2) ----------------------- */

/* api entries whose surface a game's descriptor may leave out; each logs
 * that once per mod. */
enum { UNSUP_READ_LEVEL_DATA, UNSUP_MOBY_CLASSES, UNSUP_VSTREAM_HANDLE,
       UNSUP_ANTIPIRACY_SCAN, UNSUP_ENTRIES };

/** @brief Log once per mod that the running game does not serve an api entry, and return the entry's refusal value.
 *
 * Mods are remembered by id, so a reloaded mod stays silent; the first 64
 * ids are remembered per entry and a mod past that logs on every call. A
 * NULL self logs under "mods" on every call.
 * @param self The calling mod, or NULL.
 * @param entry The UNSUP_* index of the entry.
 * @param call The entry's api member name.
 * @param what The surface the game leaves out, for the log line.
 * @param ret The value the entry returns, printed in the log line.
 * @return @p ret. */
static int api_unsupported_ret(const openpete_mod_t* self, int entry,
                               const char* call, const char* what, int ret) {
    static char s_noted[UNSUP_ENTRIES][64][MOD_ID_MAX];
    static int  s_noted_n[UNSUP_ENTRIES];
    if (self) {
        for (int i = 0; i < s_noted_n[entry]; i++)
            if (strcmp(s_noted[entry][i], self->id) == 0)
                return ret;
        if (s_noted_n[entry] < 64)
            snprintf(s_noted[entry][s_noted_n[entry]++], MOD_ID_MAX, "%s",
                     self->id);
    }
    OPL_WARN_F(self ? self->log_tag : "mods", "%s: %s serves no %s - "
               "unsupported (%d)", call, game_get_id(), what, ret);
    return ret;
}

/** @brief Log once per mod that the running game does not serve an api entry, and return OPENPETE_MOD_ERR_UNSUPPORTED.
 *
 * @param self The calling mod, or NULL.
 * @param entry The UNSUP_* index of the entry.
 * @param call The entry's api member name.
 * @param what The surface the game leaves out, for the log line.
 * @return OPENPETE_MOD_ERR_UNSUPPORTED. */
static int api_unsupported(const openpete_mod_t* self, int entry,
                           const char* call, const char* what) {
    return api_unsupported_ret(self, entry, call, what,
                               OPENPETE_MOD_ERR_UNSUPPORTED);
}

/** @brief Return the stream handle for a guest stream address; tick context only.
 *
 * Handle numbering is sim-visible, so a present-hook call is refused. On a
 * game whose descriptor clears vstream_handles, whose frame words carry no
 * vertex-stream field, the call returns 0 and logs once per mod: a mod
 * built for that game is refused at compile time by the member's
 * attribute, so only an any-game object reaches this.
 * @return the handle from openpete_mods_vstream_handle, or 0 on refusal. */
static uint32_t api_vstream_handle(openpete_mod_t* self,
                                   uint32_t stream_vaddr) {
    if (!self) return 0;
    if (!openpete_mods_game_desc()->vstream_handles)
        return (uint32_t)api_unsupported_ret(self, UNSUP_VSTREAM_HANDLE,
                                             "vstream_handle",
                                             "vertex-stream field", 0);
    if (purity_violation("vstream_handle")) return 0;
    return openpete_mods_vstream_handle(stream_vaddr, self->id);
}

/** @brief Copy a level's data blob through openpete_mods_read_level_data.
 *
 * @param self The calling mod; names it in the unsupported log line.
 * @param level_id The level id.
 * @param buf Destination of cap bytes.
 * @param cap Capacity of buf.
 * @return the game layer's result; OPENPETE_MOD_ERR_UNSUPPORTED when the
 *         game descriptor provides no level-data reader; -1 when level_id
 *         is not positive. */
static int api_read_level_data(openpete_mod_t* self, int level_id,
                               void* buf, uint32_t cap) {
    const openpete_level_ops_t* lo = openpete_mods_game_desc()->level_ops;
    if (!lo || !lo->read_level_data)
        return api_unsupported(self, UNSUP_READ_LEVEL_DATA,
                               "read_level_data", "level data");
    return openpete_mods_read_level_data(level_id, buf, cap);
}

/** @brief Set the host's texture-pack fog re-grade.
 *
 * Forwards to texture_pack_fog_shift_set; NULL scale or offset restores the
 * stock fog ladder. The setting is global to the host, not per mod.
 * @param self Unused.
 * @param scale_rgb Per-channel multiplier of the stock colour.
 * @param offset_rgb Per-channel offset in 0..255 units.
 * @return 0. */
static int api_texpack_fog_shift(openpete_mod_t* self, const float scale_rgb[3],
                                 const float offset_rgb[3]) {
    (void)self;
    texture_pack_fog_shift_set(scale_rgb, offset_rgb);
    return 0;
}

/** @brief Test whether the running game's material channel mask serves a selector channel.
 *
 * A named channel needs its bit in game_mod_desc_t.channel_mask; channel
 * -1 ("any") needs a nonempty mask. Declarative [[material]] rows and the
 * C registration entries share this one test. The harness variable
 * OPENPETE_MOD_CHANNEL_MASK (a number, base prefix honoured) is ANDed
 * into the mask, so a test can withhold a channel on a game that serves
 * every one.
 * @param channel An OP_CHAN_* value, or -1 for any channel.
 * @return 1 when served, 0 otherwise. */
static int material_channel_served(int channel) {
    uint32_t mask = openpete_mods_game_desc()->channel_mask;
    const char* env = getenv("OPENPETE_MOD_CHANNEL_MASK");
    if (env && *env) mask &= (uint32_t)strtoul(env, NULL, 0);
    if (channel < 0) return mask != 0;
    return channel < 32 && ((mask >> channel) & 1u);
}

/** @brief Refuse a C material selector whose channel the running game does not serve.
 *
 * Serves material_register, material_register_refine and
 * material_set_selector. Logs one line naming the mod, the entry, the
 * channel and the game.
 * @param self The calling mod.
 * @param call The api entry name, for the log line.
 * @param sel The selector; non-NULL.
 * @return 1 when the channel is served, 0 when the registration is refused. */
static int material_sel_channel_served(const openpete_mod_t* self,
                                       const char* call,
                                       const openpete_mod_material_selector_t* sel) {
    if (material_channel_served(sel->channel)) return 1;
    OPL_WARN_F(self ? self->log_tag : "mods", "%s: channel %d unsupported on "
               "%s - refused", call, sel->channel, game_get_id());
    return 0;
}

/** @brief Register a fragment-only material for a mod.
 *
 * The mod-facing form takes a frag shader only; vertex-stage overrides
 * stay engine-internal, so a material can reshade geometry but never move
 * it. A selector channel the game's channel mask does not serve is
 * refused, as a declarative [[material]] row on it is skipped.
 * @return the material handle, or -1 for a NULL selector, an unserved
 *         channel or a failed registration. */
static int api_material_register(openpete_mod_t* self,
                                 const openpete_mod_material_selector_t* sel,
                                 int shader) {
    if (!sel || !material_sel_channel_served(self, "material_register", sel))
        return -1;
    return openpete_mods_material_register(self, sel, -1, shader, NULL);
}

/** @brief Copy the ids of the moby classes loaded for the current level.
 *
 * Forwards to openpete_mods_moby_classes, which reports the set the last
 * tick-boundary census found and keeps the previous answer while a level is
 * loading.
 * @param self The calling mod; names it in the unsupported log line.
 * @param out Receives up to cap ids; NULL, or a cap of 0 or less, copies
 *        nothing.
 * @param cap Capacity of out in entries.
 * @return The number of ids copied, or the full class count when nothing
 *         is copied; OPENPETE_MOD_ERR_UNSUPPORTED when the game descriptor
 *         provides no moby class census. */
static int api_moby_classes(openpete_mod_t* self, uint16_t* out, int cap) {
    const openpete_moby_ops_t* mo = openpete_mods_game_desc()->moby_ops;
    if (!mo || !mo->classes_loaded)
        return api_unsupported(self, UNSUP_MOBY_CLASSES, "moby_classes",
                               "moby class census");
    return openpete_mods_moby_classes(out, cap);
}

/** @brief Read one of the game's anti-piracy scans through the descriptor's observer.
 *
 * @param self The calling mod.
 * @param key A walk key or a scanner moby's data block.
 * @param out Receives the state; its struct_size must match.
 * @return 0 when filled, -1 for a NULL or wrong-size @p out or an unseen
 *         scanner, OPENPETE_MOD_ERR_UNSUPPORTED on a game with no
 *         observer. */
static int api_antipiracy_scan(openpete_mod_t* self, uint32_t key,
                               openpete_mod_scan_t* out) {
    const game_mod_desc_t* d = openpete_mods_game_desc();
    if (!d->antipiracy_scan)
        return api_unsupported(self, UNSUP_ANTIPIRACY_SCAN, "antipiracy_scan",
                               "anti-piracy scan observer");
    if (!out || out->struct_size != (uint32_t)sizeof(openpete_mod_scan_t)) return -1;
    return d->antipiracy_scan(key, out);
}

/** @brief Register a fragment material for a mod with a per-instance refine callback.
 *
 * Like api_material_register, with the mod's refine callback attached as the
 * material's per-instance hook.
 * @param self The calling mod.
 * @param sel Face selector; NULL returns -1.
 * @param shader Fragment shader handle.
 * @param refine Per-instance callback, or NULL.
 * @return The material handle, or -1 for a NULL selector, a channel the
 *         game's channel mask does not serve, or a failed registration. */
static int api_material_register_refine(openpete_mod_t* self,
                                        const openpete_mod_material_selector_t* sel,
                                        int shader,
                                        openpete_mod_refine_fn refine) {
    if (!sel ||
        !material_sel_channel_served(self, "material_register_refine", sel))
        return -1;
    return openpete_mods_material_register(self, sel, -1, shader, refine);
}

/** @brief Retune a mod's material selector, refusing a channel the running game does not serve.
 *
 * Applies the channel test of the registration entries, so a retune can
 * no more move a material onto an unserved channel than a registration
 * can place one there; a refusal leaves the old selector in force.
 * @param self The calling mod.
 * @param material The material handle.
 * @param sel The new selector; NULL returns -1.
 * @return 0, or -1 for a NULL selector, an unserved channel, or a refusal
 *         by openpete_mods_material_set_selector. */
static int api_material_set_selector(openpete_mod_t* self, int material,
                                     const openpete_mod_material_selector_t* sel) {
    if (!sel || !material_sel_channel_served(self, "material_set_selector", sel))
        return -1;
    return openpete_mods_material_set_selector(self, material, sel);
}

/* Every counted api entry that takes the calling mod, as
 * X(return type, member, implementation, refused value, parameters,
 * arguments). MOD_API_GATED below defines gate_<member> from each row: the
 * implementation runs between mod_api_enter and mod_api_leave, and a
 * refusal returns the entry's failure value (a config read its default).
 * log, ui_status and notify are variadic and gate inside their bodies;
 * guest_alloc and config_str write an out parameter on refusal and are
 * written out below; guest, guest_addr and draw_text take no mod; base,
 * call, guest and guest_addr are not counted (see mod_api_enter). */
#define MOD_API_GATED_ENTRIES(X) \
    X(int, override_addr, api_override_addr, -1, (openpete_mod_t* self, uint32_t addr, openpete_mod_override_fn fn), (self, addr, fn)) \
    X(int, override_name, api_override_name, -1, (openpete_mod_t* self, const char* name, openpete_mod_override_fn fn), (self, name, fn)) \
    X(const char*, data_dir, api_data_dir, NULL, (openpete_mod_t* self), (self)) \
    X(int, register_toggle_hook, api_register_toggle_hook, -1, (openpete_mod_t* self, openpete_mod_toggle_fn fn), (self, fn)) \
    X(int, config_bool, openpete_mods_config_bool, def, (openpete_mod_t* self, const char* key, int def), (self, key, def)) \
    X(int64_t, config_int, openpete_mods_config_int, def, (openpete_mod_t* self, const char* key, int64_t def), (self, key, def)) \
    X(double, config_float, openpete_mods_config_float, def, (openpete_mod_t* self, const char* key, double def), (self, key, def)) \
    X(uint32_t, vstream_handle, api_vstream_handle, 0, (openpete_mod_t* self, uint32_t stream_vaddr), (self, stream_vaddr)) \
    X(int, register_present_hook, api_register_present_hook, -1, (openpete_mod_t* self, openpete_mod_present_fn fn), (self, fn)) \
    X(int, shader_register, openpete_mods_shader_register, -1, (openpete_mod_t* self, const char* path), (self, path)) \
    X(int, postfx_register, openpete_mods_postfx_register, -1, (openpete_mod_t* self, int frag_shader, int point, int order), (self, frag_shader, point, order)) \
    X(int, postfx_enable, openpete_mods_postfx_enable, -1, (openpete_mod_t* self, int fx, int enabled), (self, fx, enabled)) \
    X(int, postfx_set_uniforms, openpete_mods_postfx_set_uniforms, -1, (openpete_mod_t* self, int fx, const void* data, uint32_t size), (self, fx, data, size)) \
    X(int, sfx_replace, openpete_mods_sfx_replace, -1, (openpete_mod_t* self, const char* name, const void* adpcm_body, uint32_t len, uint16_t pitch), (self, name, adpcm_body, len, pitch)) \
    X(int, sfx_play, api_sfx_play, -1, (openpete_mod_t* self, const char* name, float gain), (self, name, gain)) \
    X(int, mouse_read, api_mouse_read, -1, (openpete_mod_t* self, openpete_mod_mouse_t* out), (self, out)) \
    X(int, mouse_capture, api_mouse_capture, -1, (openpete_mod_t* self, int on), (self, on)) \
    X(int, pad_read, api_pad_read, -1, (openpete_mod_t* self, uint32_t slot, openpete_mod_pad_t* out), (self, slot, out)) \
    X(int, pad_rumble, api_pad_rumble, -1, (openpete_mod_t* self, uint32_t slot, uint32_t small_byte, uint32_t large_byte), (self, slot, small_byte, large_byte)) \
    X(const char*, disc_path, api_disc_path, NULL, (openpete_mod_t* self, int want_bin), (self, want_bin)) \
    X(int, read_sector, api_read_sector, -1, (openpete_mod_t* self, const char* game, uint32_t lba, uint8_t* buf), (self, game, lba, buf)) \
    X(int, override_ovl, api_override_ovl, -1, (openpete_mod_t* self, uint32_t overlay_id, uint32_t addr, openpete_mod_override_fn fn), (self, overlay_id, addr, fn)) \
    X(const char*, game_disc_path, api_game_disc_path, NULL, (openpete_mod_t* self, const char* game, int want_bin), (self, game, want_bin)) \
    X(int, game_installed, api_game_installed, 0, (openpete_mod_t* self, const char* game), (self, game)) \
    X(int, texture_register, openpete_mods_texture_register, -1, (openpete_mod_t* self, uint16_t id, const void* rgba, int w, int h), (self, id, rgba, w, h)) \
    X(const char*, game_id, api_game_id, NULL, (openpete_mod_t* self), (self)) \
    X(int, gameplay_live, api_gameplay_live, 0, (openpete_mod_t* self, CPUState* cpu), (self, cpu)) \
    X(int, player_pose, api_player_pose, -1, (openpete_mod_t* self, CPUState* cpu, openpete_mod_pose_t* out), (self, cpu, out)) \
    X(int, level_id, api_level_id, -1, (openpete_mod_t* self, CPUState* cpu), (self, cpu)) \
    X(int, binding_down, api_binding_down, 0, (openpete_mod_t* self, const char* name), (self, name)) \
    X(int, read_level_data, api_read_level_data, -1, (openpete_mod_t* self, int level_id, void* buf, uint32_t cap), (self, level_id, buf, cap)) \
    X(int, texpack_fog_shift, api_texpack_fog_shift, -1, (openpete_mod_t* self, const float scale_rgb[3], const float offset_rgb[3]), (self, scale_rgb, offset_rgb)) \
    X(int, material_register, api_material_register, -1, (openpete_mod_t* self, const openpete_mod_material_selector_t* sel, int shader), (self, sel, shader)) \
    X(int, material_register_refine, api_material_register_refine, -1, (openpete_mod_t* self, const openpete_mod_material_selector_t* sel, int shader, openpete_mod_refine_fn refine), (self, sel, shader, refine)) \
    X(int, material_enable, openpete_mods_material_enable, -1, (openpete_mod_t* self, int material, int on), (self, material, on)) \
    X(int, material_set_params, openpete_mods_material_set_params, -1, (openpete_mod_t* self, int material, const void* block, uint32_t len), (self, material, block, len)) \
    X(int, material_set_selector, api_material_set_selector, -1, (openpete_mod_t* self, int material, const openpete_mod_material_selector_t* sel), (self, material, sel)) \
    X(int, moby_classes, api_moby_classes, -1, (openpete_mod_t* self, uint16_t* out, int cap), (self, out, cap)) \
    X(int, register_shutdown_hook, api_register_shutdown_hook, -1, (openpete_mod_t* self, openpete_mod_shutdown_fn fn), (self, fn)) \
    X(int, save_register, openpete_modsave_register, -1, (openpete_mod_t* self, uint32_t schema, openpete_mod_save_write_fn write, openpete_mod_save_read_fn read), (self, schema, write, read)) \
    X(int, antipiracy_scan, api_antipiracy_scan, -1, (openpete_mod_t* self, uint32_t key, openpete_mod_scan_t* out), (self, key, out))

/* One gate_<member> per MOD_API_GATED_ENTRIES row. */
#define MOD_API_GATED(ret, name, impl, refused, params, args)             \
    static ret gate_##name params {                                       \
        if (!mod_api_enter(self, #name)) return refused;                  \
        ret gate_rc_ = impl args;                                         \
        mod_api_leave();                                                  \
        return gate_rc_;                                                  \
    }
MOD_API_GATED_ENTRIES(MOD_API_GATED)
#undef MOD_API_GATED

/** @brief Gate guest_alloc: a refused call returns 0 and clears @p host_view.
 *
 * @param self The calling mod.
 * @param size Bytes.
 * @param align Alignment.
 * @param flags `OP_GALLOC_*`.
 * @param host_view Receives the host alias, NULL on refusal.
 * @return The guest vaddr, or 0. */
static uint32_t gate_guest_alloc(openpete_mod_t* self, uint32_t size,
                                 uint32_t align, uint32_t flags,
                                 void** host_view) {
    if (!mod_api_enter(self, "guest_alloc")) {
        if (host_view) *host_view = NULL;
        return 0;
    }
    const uint32_t va = api_guest_alloc(self, size, align, flags, host_view);
    mod_api_leave();
    return va;
}

/** @brief Gate config_str: a refused call copies the default into @p buf.
 *
 * @param self The calling mod.
 * @param key Dotted key.
 * @param def Default string.
 * @param buf Output buffer.
 * @param cap Capacity of @p buf.
 * @return 1 when the key was present, 0 when the default was copied. */
static int gate_config_str(openpete_mod_t* self, const char* key,
                           const char* def, char* buf, int cap) {
    if (!mod_api_enter(self, "config_str")) {
        if (buf && cap > 0) snprintf(buf, (size_t)cap, "%s", def ? def : "");
        return 0;
    }
    const int rc = openpete_mods_config_str(self, key, def, buf, cap);
    mod_api_leave();
    return rc;
}

/** @brief Gate guest, which takes no mod: uncounted, refused once every mod is closed.
 *
 * Every typed guest accessor calls it, so it pays one flag load instead
 * of the counted bracket. It only translates an address into guest RAM,
 * the scratchpad or the arena, which live until the process ends, so no
 * teardown can overlap a call in progress.
 * @param vaddr Guest vaddr.
 * @return The host pointer, or NULL. */
static void* gate_guest(uint32_t vaddr) {
    if (mod_api_is_closed(NULL)) { mod_api_refused(NULL, "guest"); return NULL; }
    return api_guest(vaddr);
}

/** @brief Gate guest_addr, which takes no mod: uncounted like gate_guest, refused once every mod is closed.
 *
 * @param host Host pointer.
 * @return The guest vaddr, or 0. */
static uint32_t gate_guest_addr(const void* host) {
    if (mod_api_is_closed(NULL)) { mod_api_refused(NULL, "guest_addr"); return 0; }
    return api_guest_addr(host);
}

/** @brief Gate draw_text, which takes no mod: refused once every mod is closed.
 *
 * @param x Overlay x.
 * @param y Overlay y.
 * @param cap_h Cap height.
 * @param rgb Tint.
 * @param str Text. */
static void gate_draw_text(float x, float y, float cap_h, uint32_t rgb,
                           const char* str) {
    if (!mod_api_enter(NULL, "draw_text")) return;
    openpete_mods_draw_text(x, y, cap_h, rgb, str);
    mod_api_leave();
}

/** @brief Gate base: uncounted, since the continuation may park the fiber; refused once every mod is closed.
 *
 * @param cpu Guest CPU. */
static void gate_base(CPUState* cpu) {
    if (mod_api_is_closed(NULL)) { mod_api_refused(NULL, "base"); return; }
    openpete_mods_base(cpu);
}

/** @brief Gate call: uncounted, since the callee may park the fiber; refused once every mod is closed.
 *
 * @param cpu Guest CPU.
 * @param addr Guest function address. */
static void gate_call(CPUState* cpu, uint32_t addr) {
    if (mod_api_is_closed(NULL)) { mod_api_refused(NULL, "call"); return; }
    api_call_addr(cpu, addr);
}

/* Every entry routes through a gate, so a refusal after the exit pass
 * holds for each one. */
#define MOD_API_ROW(ret, name, impl, refused, params, args) .name = gate_##name,
static const openpete_mod_api_t s_api = {
    .api_version   = OPENPETE_MOD_API_VERSION,
    .base          = gate_base,
    .call          = gate_call,
    .guest         = gate_guest,
    .guest_addr    = gate_guest_addr,
    .guest_alloc   = gate_guest_alloc,
    .log           = api_log,
    .ui_status     = api_ui_status,
    .notify        = api_notify,
    .config_str    = gate_config_str,
    .draw_text     = gate_draw_text,
    MOD_API_GATED_ENTRIES(MOD_API_ROW)
};
#undef MOD_API_ROW

/* ---- discovery / manifest / build / load --------------------------- */

/** @brief Return a path's modification time, the loader's existence probe.
 *
 * @return st_mtime, or (time_t)-1 when stat fails. */
static time_t file_mtime(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return (time_t)-1;
    return st.st_mtime;
}

/** @brief Read an optional [[level]] count key in 0..max.
 *
 * Absent returns -1, which keeps that table on its stock row. A value
 * outside 0..max is refused with an error naming the key and returns -1
 * rather than truncating: a wrapped write would show a number the author
 * never typed.
 * @return the value, or -1 when absent or out of range. */
static int level_count_key(toml_table_t* lt, const char* key, int max,
                           const char* path, const char* name) {
    toml_datum_t d = toml_int_in(lt, key);
    if (!d.ok) return -1;
    if (d.u.i < 0 || d.u.i > max) {
        OPL_ERROR_F("mods", "%s: [[level]] \"%s\": %s = %lld is outside "
                    "0..%d - ignored", path, name, key,
                    (long long)d.u.i, max);
        return -1;
    }
    return (int)d.u.i;
}

/** @brief Report whether an id's stem (the text before its first dot) is a Windows reserved device name.
 *
 * CON, PRN, AUX, NUL, COM1-COM9 and LPT1-LPT9 name devices in every
 * directory on Windows, with or without an extension, in any case.
 * @param id The id.
 * @return 1 when reserved, else 0. */
static int mod_id_reserved_stem(const char* id) {
    static const char* const names[] = {"CON", "PRN", "AUX", "NUL"};
    size_t n = strcspn(id, ".");
    char stem[5];
    if (n < 3 || n > 4) return 0;
    for (size_t i = 0; i < n; i++)
        stem[i] = (char)((id[i] >= 'a' && id[i] <= 'z') ? id[i] - 32 : id[i]);
    stem[n] = '\0';
    if (n == 4)
        return (!strncmp(stem, "COM", 3) || !strncmp(stem, "LPT", 3)) &&
               stem[3] >= '1' && stem[3] <= '9';
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(stem, names[i])) return 1;
    return 0;
}

/** @brief Report whether a manifest id is usable as a single file name component on every host.
 *
 * The id names files and directories (.build/<id>, .unpacked/<id>) and
 * reaches the compiler's output path through an argv, never a shell, so
 * spaces and shell metacharacters are legal. Refused: an empty id, a leading
 * dot, a "..", a path separator (/ or \), a character Windows file names cannot
 * hold (< > : " | ? * and control bytes), a trailing dot or space, and a
 * reserved device stem (mod_id_reserved_stem). Any of these either leaves
 * the mod's own directory or names a file Windows cannot create.
 * @param id The id.
 * @return 1 when safe, else 0. */
static int mod_id_valid(const char* id) {
    size_t n = strlen(id);
    if (!n || id[0] == '.' || strstr(id, "..")) return 0;
    if (id[n - 1] == '.' || id[n - 1] == ' ') return 0;
    for (const char* p = id; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("/\\<>:\"|?*", *p)) return 0;
    return !mod_id_reserved_stem(id);
}

/** @brief Parse the [mod] table: identity, the api floor, and the declarations that gate savestate, rewind and runahead.
 *
 * id and version are required; name defaults to the id; dir and log_tag
 * are filled. api is the minimum mod API the mod needs: source mods
 * recompile against the current SDK and the api struct is append-only, so
 * only an api above OPENPETE_MOD_API_VERSION refuses. It is stored as
 * api_pin, which selects the overlay-window override rules (MOD_API_SCOPED). sim_mutative stays
 * -1 when absent so mod_is_mutative decides; the obsolete sim_neutral key,
 * of inverted sense, is honoured with a warning when sim_mutative is
 * absent. state ("none" or "rebuildable") and runahead ("pure" or
 * "host-fx") set state_decl and ra_decl; an unknown value logs an error
 * and leaves the declaration unset.
 * @param t The [mod] table.
 * @param mod The mod being parsed.
 * @param mod_dir The mod directory.
 * @param path Manifest path for log lines.
 * @return 0, or -1 when id or version is missing, the id cannot be a
 *         file name (mod_id_valid), or the api is newer than the engine's. */
static int parse_mod_header(toml_table_t* t, struct openpete_mod* mod,
                            const char* mod_dir, const char* path) {
    toml_datum_t id = toml_string_in(t, "id");
    toml_datum_t ver = toml_string_in(t, "version");
    toml_datum_t api = toml_int_in(t, "api");
    if (!id.ok || !ver.ok || !mod_id_valid(id.u.s)) {
        if (id.ok && ver.ok)
            OPL_ERROR_F("mods", "%s: [mod] id \"%s\" cannot be a file name "
                        "(no / \\ < > : \" | ? *, control bytes, \"..\", leading "
                        "dot, trailing dot or space, or device name) - disabled",
                        path, id.u.s);
        else
            OPL_ERROR_F("mods", "%s: [mod] requires id and version", path);
        if (id.ok) free(id.u.s);
        if (ver.ok) free(ver.u.s);
        return -1;
    }
    /* The api struct is append-only, and source mods recompile against
     * the CURRENT staged SDK regardless - so the manifest pin is the
     * MINIMUM api the mod's calls need, not an exact build match. Only
     * a mod needing a newer engine refuses. */
    if (api.ok && api.u.i > (int64_t)OPENPETE_MOD_API_VERSION) {
        OPL_ERROR_F("mods", "%s: mod \"%s\" needs api %lld, engine has %u - "
                    "disabled", path, id.u.s, (long long)api.u.i,
                    OPENPETE_MOD_API_VERSION);
        free(id.u.s);
        free(ver.u.s);
        return -1;
    }
    snprintf(mod->id, sizeof mod->id, "%s", id.u.s);
    snprintf(mod->version, sizeof mod->version, "%s", ver.u.s);
    mod->api_pin = (api.ok && api.u.i > 0) ? (uint32_t)api.u.i : 0u;
    snprintf(mod->dir, sizeof mod->dir, "%s", mod_dir);
    snprintf(mod->log_tag, sizeof mod->log_tag, "mod:%s", mod->id);
    free(id.u.s);
    free(ver.u.s);
    {   /* Display name for menus; ids stay the stable identity. */
        toml_datum_t nm = toml_string_in(t, "name");
        if (nm.ok) {
            snprintf(mod->name, sizeof mod->name, "%s", nm.u.s);
            free(nm.u.s);
        }
        if (!mod->name[0])
            snprintf(mod->name, sizeof mod->name, "%s", mod->id);
    }
    /* sim_mutative: does this mod write guest RAM? Undeclared leaves it
     * to mod_is_mutative(), which can prove the answer for a mod that
     * ships no code and claims no level. */
    toml_datum_t sm = toml_bool_in(t, "sim_mutative");
    mod->sim_mutative = sm.ok ? (sm.u.b ? 1 : 0) : -1;
    /* The key was once spelled sim_neutral, with the opposite sense.
     * Honour it with a warning rather than ignoring it: silently
     * dropping the old spelling would move a mod into the save modset
     * tag without anyone noticing. */
    toml_datum_t sn = toml_bool_in(t, "sim_neutral");
    if (sn.ok) {
        OPL_WARN_F("mods", "%s: sim_neutral is obsolete - write "
                   "sim_mutative = %s instead (the sense is inverted)",
                   mod->id, sn.u.b ? "false" : "true");
        if (!sm.ok) mod->sim_mutative = sn.u.b ? 0 : 1;
    }
    /* State + runahead declarations (savestate/rewind/runahead gates).
     * state: "none" (no host state derived from guest history) or
     * "rebuildable" (host state re-derives from guest RAM within a
     * tick). runahead: "pure" (tick hooks touch guest state only) or
     * "host-fx" (host side effects the engine suppresses during
     * speculative ticks). Unknown values are an error: a wrong claim
     * here corrupts timelines, so it must not parse quietly. */
    toml_datum_t st = toml_string_in(t, "state");
    if (st.ok) {
        if (strcmp(st.u.s, "none") == 0)             mod->state_decl = 'n';
        else if (strcmp(st.u.s, "rebuildable") == 0) mod->state_decl = 'r';
        else OPL_ERROR_F("mods", "%s: unknown state = \"%s\" (want "
                         "\"none\" or \"rebuildable\") - ignored",
                         mod->id, st.u.s);
        free(st.u.s);
    }
    toml_datum_t ra = toml_string_in(t, "runahead");
    if (ra.ok) {
        if (strcmp(ra.u.s, "pure") == 0)          mod->ra_decl = 'p';
        else if (strcmp(ra.u.s, "host-fx") == 0)  mod->ra_decl = 'h';
        else OPL_ERROR_F("mods", "%s: unknown runahead = \"%s\" (want "
                         "\"pure\" or \"host-fx\") - ignored",
                         mod->id, ra.u.s);
        free(ra.u.s);
    }
    return 0;
}

/** @brief Parse the [mod] deps array of mod ids into the mod's dependency rows.
 *
 * Non-string elements are ignored. openpete_mods_init topo-sorts on these
 * rows so a dependency's entry runs before its dependents'; a missing or
 * cyclic dependency disables the mod with an error.
 * @param t The [mod] table.
 * @param mod The mod being parsed. */
static void parse_deps(toml_table_t* t, struct openpete_mod* mod) {
    toml_array_t* deps = toml_array_in(t, "deps");
    if (!deps) return;
    if (!mod->deps.elem_size)
        mod_fvec_init(&mod->deps, sizeof(mod_dep_t), MOD_DEPS_INIT,
                      NULL, 0);
    int n = toml_array_nelem(deps);
    for (int i = 0; i < n; i++) {
        toml_datum_t d = toml_string_at(deps, i);
        if (!d.ok) continue;
        mod_dep_t* slot = (mod_dep_t*)mod_fvec_push(&mod->deps);
        if (slot) snprintf(slot->id, MOD_ID_MAX, "%s", d.u.s);
        free(d.u.s);
        if (!slot) break;
    }
}

/** @brief Add the ids one manifest key names to a mod's game list.
 *
 * The key is one id or an array of ids; non-string elements are ignored.
 * An id that names no registered game is kept, with a warning: as a
 * required game it refuses the mod, since no library disc supplies it.
 * @param t The [mod] table.
 * @param key "requires_games" or "optional_games".
 * @param required 1 for requires_games.
 * @param l The list the ids are added to.
 * @param mod_id The mod id, for log lines. */
static void parse_game_key(toml_table_t* t, const char* key, int required,
                           mod_game_list_t* l, const char* mod_id) {
    toml_array_t* a = toml_array_in(t, key);
    toml_datum_t one = a ? (toml_datum_t){0} : toml_string_in(t, key);
    const int n = a ? toml_array_nelem(a) : (one.ok ? 1 : 0);
    for (int i = 0; i < n; i++) {
        toml_datum_t d = a ? toml_string_at(a, i) : one;
        if (!d.ok) continue;
        if (!openpete_game_registered(d.u.s))
            OPL_WARN_F("mods", "%s: %s names \"%s\", which is no game this "
                       "build knows", mod_id, key, d.u.s);
        if (mod_game_list_add(l, d.u.s, required) < 0)
            OPL_ERROR_F("mods", "%s: %s \"%s\" not added (an id is at most "
                        "%d bytes, and a mod names at most %d games)",
                        mod_id, key, d.u.s, MOD_GAME_ID_MAX - 1, MOD_GAMES_MAX);
        free(d.u.s);
    }
}

/** @brief Parse [mod] requires_games and optional_games into a game list, replacing its rows.
 *
 * @param t The [mod] table.
 * @param l The list.
 * @param mod_id The mod id, for log lines. */
static void parse_games(toml_table_t* t, mod_game_list_t* l, const char* mod_id) {
    memset(l, 0, sizeof *l);
    parse_game_key(t, "requires_games", 1, l, mod_id);
    parse_game_key(t, "optional_games", 0, l, mod_id);
}

/** @brief Parse the [[binding]] tables (name, key, optional purpose) into the mod's binding rows.
 *
 * A table missing name or key is skipped with an error. The rows make
 * host-key use visible: mod_register_key_defaults registers them with the
 * keymap, openpete_mods_init warns when two loaded mods resolve to the same
 * effective key (reads are non-exclusive), and a [keys.mod.<id>] config
 * row rebinds one without editing the mod.
 * @param root The manifest root table.
 * @param mod The mod being parsed.
 * @param path Manifest path for log lines. */
static void parse_bindings(toml_table_t* root, struct openpete_mod* mod,
                           const char* path) {
    toml_array_t* binds = toml_array_in(root, "binding");
    if (!binds) return;
    if (!mod->bindings.elem_size)
        mod_fvec_init(&mod->bindings, sizeof(mod_binding_t),
                      MOD_BINDINGS_INIT, NULL, 0);
    int n = toml_array_nelem(binds);
    for (int i = 0; i < n; i++) {
        toml_table_t* b = toml_table_at(binds, i);
        if (!b) continue;
        toml_datum_t bn = toml_string_in(b, "name");
        toml_datum_t bk = toml_string_in(b, "key");
        toml_datum_t bp = toml_string_in(b, "purpose");
        if (bn.ok && bk.ok) {
            mod_binding_t* mb =
                (mod_binding_t*)mod_fvec_push(&mod->bindings);
            if (mb) {
                snprintf(mb->name, sizeof mb->name, "%s", bn.u.s);
                snprintf(mb->key, sizeof mb->key, "%s", bk.u.s);
                snprintf(mb->purpose, sizeof mb->purpose, "%s",
                         bp.ok ? bp.u.s : "");
            }
        } else {
            OPL_ERROR_F("mods", "%s: [[binding]] needs name and key", path);
        }
        if (bn.ok) free(bn.u.s);
        if (bk.ok) free(bk.u.s);
        if (bp.ok) free(bp.u.s);
    }
}

/** @brief Read the default key of a [[config]] table in the row's own type.
 *
 * A float row also accepts an integer default. A missing or mistyped
 * default leaves the row's default unchanged.
 * @param c The [[config]] table.
 * @param r The row, whose type is already set. */
static void parse_config_default(toml_table_t* c, mod_cfg_row_t* r) {
    toml_datum_t d;
    switch (r->type) {
    case 'b':
        if ((d = toml_bool_in(c, "default")).ok) r->def_i = d.u.b ? 1 : 0;
        break;
    case 'i':
        if ((d = toml_int_in(c, "default")).ok) r->def_i = d.u.i;
        break;
    case 'd':
        if ((d = toml_double_in(c, "default")).ok) r->def_d = d.u.d;
        else if ((d = toml_int_in(c, "default")).ok)
            r->def_d = (double)d.u.i;
        break;
    case 's':
        if ((d = toml_string_in(c, "default")).ok) {
            snprintf(r->def_s, sizeof r->def_s, "%s", d.u.s);
            free(d.u.s);
        }
        break;
    default: break;
    }
}

/** @brief Copy an optional string key of a [[config]] table into a fixed buffer, truncating; an absent key leaves the buffer as it was.
 * @param c The table.
 * @param key The key.
 * @param out The buffer.
 * @param cap Its capacity. */
static void cfg_string_in(toml_table_t* c, const char* key, char* out, size_t cap) {
    toml_datum_t d = toml_string_in(c, key);
    if (!d.ok) return;
    snprintf(out, cap, "%s", d.u.s);
    free(d.u.s);
}

/** @brief Parse one [[config]] table into a schema row.
 *
 * key and type are required; type is bool, int, float or string. label,
 * group, help, numeric min and max, the default, and for a string row an
 * items list are optional.
 * @param c The [[config]] table.
 * @param r The row to fill.
 * @param path Manifest path for log lines.
 * @return 0, or -1 after an error line when key or type is missing or the
 *         type is unknown. */
static int parse_config_row(toml_table_t* c, mod_cfg_row_t* r,
                            const char* path) {
    toml_datum_t ck = toml_string_in(c, "key");
    toml_datum_t ct = toml_string_in(c, "type");
    if (!ck.ok || !ct.ok) {
        OPL_ERROR_F("mods", "%s: [[config]] needs key and type", path);
        if (ck.ok) free(ck.u.s);
        if (ct.ok) free(ct.u.s);
        return -1;
    }
    char type = 0;
    if (strcmp(ct.u.s, "bool") == 0)        type = 'b';
    else if (strcmp(ct.u.s, "int") == 0)    type = 'i';
    else if (strcmp(ct.u.s, "float") == 0)  type = 'd';
    else if (strcmp(ct.u.s, "string") == 0) type = 's';
    if (!type) {
        OPL_ERROR_F("mods", "%s: [[config]] \"%s\": unknown type \"%s\"",
                    path, ck.u.s, ct.u.s);
        free(ck.u.s);
        free(ct.u.s);
        return -1;
    }
    snprintf(r->key, sizeof r->key, "%s", ck.u.s);
    r->type = type;
    free(ck.u.s);
    free(ct.u.s);
    toml_datum_t d;
    cfg_string_in(c, "label", r->label, sizeof r->label);
    cfg_string_in(c, "group", r->group, sizeof r->group);
    cfg_string_in(c, "help", r->help, sizeof r->help);
    if ((d = toml_double_in(c, "min")).ok) r->min = d.u.d;
    if ((d = toml_double_in(c, "max")).ok) r->max = d.u.d;
    if ((d = toml_bool_in(c, "secret")).ok) r->secret = d.u.b != 0;
    parse_config_default(c, r);
    toml_array_t* it = toml_array_in(c, "items");
    if (it && type == 's') {
        mod_fvec_init(&r->items, sizeof(mod_cfg_item_t),
                      MOD_CFG_ITEMS_INIT, NULL, 0);
        int ni = toml_array_nelem(it);
        for (int k = 0; k < ni; k++) {
            toml_datum_t iv = toml_string_at(it, k);
            if (!iv.ok) continue;
            mod_cfg_item_t* slot =
                (mod_cfg_item_t*)mod_fvec_push(&r->items);
            if (slot) snprintf(slot->s, sizeof slot->s, "%s", iv.u.s);
            free(iv.u.s);
            if (!slot) break;
        }
    }
    return 0;
}

/** @brief Parse the [[config]] schema rows that the Mods panel renders.
 *
 * Each table goes through parse_config_row; a row it refuses is dropped
 * after its error line rather than guessed at. Logs the row count when
 * nonzero.
 * @param root The manifest root table.
 * @param mod The mod being parsed.
 * @param path Manifest path for log lines. */
static void parse_config(toml_table_t* root, struct openpete_mod* mod,
                         const char* path) {
    toml_array_t* cfgs = toml_array_in(root, "config");
    if (!cfgs) return;
    if (!mod->cfg_rows.elem_size)
        mod_fvec_init(&mod->cfg_rows, sizeof(mod_cfg_row_t),
                      MOD_CFG_ROWS_INIT, NULL, 0);
    int n = toml_array_nelem(cfgs);
    for (int i = 0; i < n; i++) {
        toml_table_t* c = toml_table_at(cfgs, i);
        if (!c) continue;
        mod_cfg_row_t* r = (mod_cfg_row_t*)mod_fvec_push(&mod->cfg_rows);
        if (!r) {
            OPL_ERROR_F("mods", "%s: config row allocation failed", path);
            break;
        }
        if (parse_config_row(c, r, path) != 0)
            mod_fvec_truncate(&mod->cfg_rows,
                              mod_fvec_count(&mod->cfg_rows) - 1);
    }
    if (mod_cfg_count(mod))
        OPL_INFO_F("mods", "%s: %d config schema row(s)", mod->id,
                   mod_cfg_count(mod));
}

/** @brief Parse the [[level]] tables, declarative custom levels, into the mod's level rows.
 *
 * name, files and donor are required; a table missing one is skipped with
 * an error. replace, music and title are optional. dragons, gems and eggs
 * are bounded by the guest tables' element types (u_char, short, u_char)
 * and read -1 when absent or out of range (level_count_key).
 * @param root The manifest root table.
 * @param mod The mod being parsed.
 * @param path Manifest path for log lines. */
static void parse_levels(toml_table_t* root, struct openpete_mod* mod,
                         const char* path) {
    toml_array_t* la = toml_array_in(root, "level");
    if (!la) return;
    if (!mod->levels.elem_size)
        mod_fvec_init(&mod->levels, sizeof(mod_level_row_t),
                      MOD_LEVELS_INIT, NULL, 0);
    int nl = toml_array_nelem(la);
    for (int i = 0; i < nl; i++) {
        toml_table_t* lt = toml_table_at(la, i);
        if (!lt) continue;
        toml_datum_t ln = toml_string_in(lt, "name");
        toml_datum_t lf = toml_string_in(lt, "files");
        toml_datum_t ld = toml_string_in(lt, "donor");
        toml_datum_t lr = toml_bool_in(lt, "replace");
        toml_datum_t lm = toml_string_in(lt, "music");
        toml_datum_t lti = toml_string_in(lt, "title");
        if (ln.ok && lf.ok && ld.ok) {
            mod_level_row_t* L =
                (mod_level_row_t*)mod_fvec_push(&mod->levels);
            if (L) {
                snprintf(L->name, sizeof L->name, "%s", ln.u.s);
                snprintf(L->files, sizeof L->files, "%s", lf.u.s);
                snprintf(L->donor, sizeof L->donor, "%s", ld.u.s);
                L->replace = lr.ok && lr.u.b;
                snprintf(L->music, sizeof L->music, "%s",
                         lm.ok ? lm.u.s : "");
                snprintf(L->title, sizeof L->title, "%s",
                         lti.ok ? lti.u.s : "");
                /* Bounds are the guest tables' own element types:
                 * u_char, short, u_char. */
                L->dragons = level_count_key(lt, "dragons", 255, path,
                                             ln.u.s);
                L->gems    = level_count_key(lt, "gems", 32767, path,
                                             ln.u.s);
                L->eggs    = level_count_key(lt, "eggs", 255, path,
                                             ln.u.s);
            }
        } else {
            OPL_ERROR_F("mods", "%s: [[level]] needs name, files and "
                        "donor - skipped", path);
        }
        if (ln.ok) free(ln.u.s);
        if (lf.ok) free(lf.u.s);
        if (ld.ok) free(ld.u.s);
        if (lm.ok) free(lm.u.s);
        if (lti.ok) free(lti.u.s);
    }
}

/** @brief Parse a [[material]] row's params list, the floats handed to its shader as a uniform block.
 *
 * Integer elements are accepted and converted. An absent list returns 0
 * and leaves the row's params unchanged.
 * @param mt The [[material]] table.
 * @param r The row to fill.
 * @param path Manifest path for log lines.
 * @return 0, or -1 after an error line when the list holds more than
 *         MOD_MATERIAL_PARAMS_MAX elements or an element is not a
 *         number. */
static int parse_material_params(toml_table_t* mt, mod_material_row_t* r,
                                 const char* path) {
    toml_array_t* pa = toml_array_in(mt, "params");
    if (!pa) return 0;
    int np = toml_array_nelem(pa);
    if (np > MOD_MATERIAL_PARAMS_MAX) {
        OPL_ERROR_F("mods", "%s: [[material]] params has %d floats, max "
                    "%d - skipped", path, np, MOD_MATERIAL_PARAMS_MAX);
        return -1;
    }
    for (int k = 0; k < np; k++) {
        toml_datum_t pv = toml_double_at(pa, k);
        if (pv.ok) { r->params[k] = (float)pv.u.d; continue; }
        toml_datum_t iv = toml_int_at(pa, k);   /* TOML keeps 1 and 1.0 distinct */
        if (!iv.ok) {
            OPL_ERROR_F("mods", "%s: [[material]] params[%d] is not a "
                        "number - skipped", path, k);
            return -1;
        }
        r->params[k] = (float)iv.u.i;
    }
    r->n_params = np;
    return 0;
}

/** @brief Parse the flag keys of one [[material]] row into its OP_SEL_* flags.
 *
 * tier ("hp" or "lp"), textured and env_anim narrow and mirror the C
 * flags exactly; an absent tier, textured or env_anim narrows nothing.
 * env_anim = false adds OP_SEL_ENV_STATIC on a mod pinned at
 * MOD_API_ENV_STATIC or later and narrows nothing below it. exclusive =
 * true adds OP_SEL_EXCLUSIVE on a mod pinned at MOD_API_COMPOSE or later
 * and is ignored below it, as an unknown key is.
 * @param mt The [[material]] table.
 * @param r The row to fill.
 * @param api_pin The mod's manifest api pin.
 * @param path Manifest path for log lines.
 * @return 0, or -1 after an error line when tier names a value the engine does not know. */
static int parse_material_flags(toml_table_t* mt, mod_material_row_t* r, int api_pin,
                                const char* path) {
    toml_datum_t d;
    if ((d = toml_string_in(mt, "tier")).ok) {
        int bad = 0;
        if (strcmp(d.u.s, "hp") == 0)      r->flags |= OP_SEL_HP_TIER;
        else if (strcmp(d.u.s, "lp") == 0) r->flags |= OP_SEL_LP_TIER;
        else {
            OPL_ERROR_F("mods", "%s: [[material]] tier must be \"hp\" or "
                        "\"lp\" (got \"%s\") - skipped", path, d.u.s);
            bad = 1;
        }
        free(d.u.s);
        if (bad) return -1;
    }
    if ((d = toml_bool_in(mt, "textured")).ok)
        r->flags |= d.u.b ? OP_SEL_TEXTURED : OP_SEL_UNTEXTURED;
    if ((d = toml_bool_in(mt, "env_anim")).ok) {
        if (d.u.b)                                r->flags |= OP_SEL_ENV_ANIM;
        else if (api_pin >= MOD_API_ENV_STATIC)   r->flags |= OP_SEL_ENV_STATIC;
    }
    if (api_pin >= MOD_API_COMPOSE && (d = toml_bool_in(mt, "exclusive")).ok && d.u.b)
        r->flags |= OP_SEL_EXCLUSIVE;
    return 0;
}

/** @brief Parse the narrowing selector keys of one [[material]] row.
 *
 * channel (by name, through mod_channel_from_name), ident, class,
 * texture, level and enabled set the row's fields; the flag keys go
 * through parse_material_flags. A key of the wrong TOML type is ignored.
 * @param mt The [[material]] table.
 * @param r The row to fill.
 * @param api_pin The mod's manifest api pin.
 * @param path Manifest path for log lines.
 * @return 0, or -1 after an error line when channel or tier names a value
 *         the engine does not know. */
static int parse_material_selector(toml_table_t* mt, mod_material_row_t* r, int api_pin,
                                   const char* path) {
    toml_datum_t d;
    if ((d = toml_string_in(mt, "channel")).ok) {
        r->channel = mod_channel_from_name(d.u.s);
        int bad = r->channel == -2;
        if (bad)
            OPL_ERROR_F("mods", "%s: [[material]] unknown channel \"%s\" "
                        "- skipped", path, d.u.s);
        free(d.u.s);
        if (bad) return -1;
    }
    if ((d = toml_int_in(mt, "ident")).ok)   r->ident      = (int)d.u.i;
    if ((d = toml_int_in(mt, "class")).ok)   r->moby_class = (int)d.u.i;
    if ((d = toml_int_in(mt, "texture")).ok) r->texture    = (int)d.u.i;
    if ((d = toml_int_in(mt, "level")).ok)   r->level      = (int)d.u.i;
    if ((d = toml_bool_in(mt, "enabled")).ok) r->enabled   = d.u.b ? 1 : 0;
    return parse_material_flags(mt, r, api_pin, path);
}

/** @brief Test whether a [[material]] row's game key selects the running game.
 *
 * The key is one id or an array of ids, matched exactly; an absent key
 * selects every game the mod loads on. A row for another game is logged
 * and dropped.
 * @param mt The [[material]] table.
 * @param path Manifest path for log lines.
 * @param row Row index, for log lines.
 * @return 1 when the row applies to this game, 0 otherwise. */
static int material_row_selects_game(toml_table_t* mt, const char* path,
                                     int row) {
    const char* game = mods_build_game();
    toml_array_t* ga = toml_array_in(mt, "game");
    int match = 0;
    if (ga) {
        for (int i = 0; i < toml_array_nelem(ga); i++) {
            toml_datum_t d = toml_string_at(ga, i);
            if (!d.ok) continue;
            if (strcmp(d.u.s, game) == 0) match = 1;
            free(d.u.s);
        }
    } else {
        toml_datum_t d = toml_string_in(mt, "game");
        if (!d.ok) return 1;
        match = strcmp(d.u.s, game) == 0;
        free(d.u.s);
    }
    if (!match)
        OPL_INFO_F("mods", "%s: [[material]] row %d targets other games - "
                   "skipped (this is %s)", path, row, game);
    return match;
}

/** @brief Test whether the running game's material channel mask serves a row's channel.
 *
 * Applies material_channel_served, the test the C registration entries
 * share. A row the game cannot serve is logged, naming the game, and
 * dropped; the rest of the mod loads.
 * @param r The parsed row.
 * @param path Manifest path for log lines.
 * @param row Row index, for log lines.
 * @return 1 when the row's channel is served, 0 otherwise. */
static int material_row_channel_served(const mod_material_row_t* r,
                                       const char* path, int row) {
    const int served = material_channel_served(r->channel);
    if (!served)
        OPL_WARN_F("mods", "%s: [[material]] row %d: channel %d unsupported "
                   "on %s - skipped", path, row, r->channel, game_get_id());
    return served;
}

/** @brief Parse the [[material]] tables, declarative reshade rows that need no C, into the mod's material rows.
 *
 * A row needs frag or vert. Selector fields default to -1 (match any) and
 * enabled to 1. An unknown channel or tier spelling refuses the row
 * instead of defaulting: a typo that widened the selection would repaint
 * geometry the author did not select, and a selector never matches wider
 * than written. A row whose game key excludes the running game, or whose
 * channel the game's channel mask does not serve, is logged and skipped.
 * Logs the row count when nonzero.
 * @param root The manifest root table.
 * @param mod The mod being parsed.
 * @param path Manifest path for log lines. */
static void parse_materials(toml_table_t* root, struct openpete_mod* mod,
                            const char* path) {
    toml_array_t* ma = toml_array_in(root, "material");
    if (!ma) return;
    if (!mod->mat_rows.elem_size)
        mod_fvec_init(&mod->mat_rows, sizeof(mod_material_row_t),
                      MOD_MATERIAL_ROWS_INIT, NULL, 0);
    int nm = toml_array_nelem(ma);
    for (int i = 0; i < nm; i++) {
        toml_table_t* mt = toml_table_at(ma, i);
        if (!mt) continue;
        mod_material_row_t r;
        memset(&r, 0, sizeof r);
        r.channel = r.ident = r.moby_class = r.texture = r.level = -1;
        r.enabled = 1;

        toml_datum_t d;
        if ((d = toml_string_in(mt, "frag")).ok) {
            snprintf(r.frag, sizeof r.frag, "%s", d.u.s); free(d.u.s);
        }
        if ((d = toml_string_in(mt, "vert")).ok) {
            snprintf(r.vert, sizeof r.vert, "%s", d.u.s); free(d.u.s);
        }
        if (!r.frag[0] && !r.vert[0]) {
            OPL_ERROR_F("mods", "%s: [[material]] needs frag (or vert) - "
                        "skipped", path);
            continue;
        }
        if (!material_row_selects_game(mt, path, i)) continue;
        if (parse_material_selector(mt, &r, (int)mod->api_pin, path) != 0) continue;
        if (!material_row_channel_served(&r, path, i)) continue;
        if (parse_material_params(mt, &r, path) != 0) continue;
        mod_material_row_t* slot =
            (mod_material_row_t*)mod_fvec_push(&mod->mat_rows);
        if (!slot) {
            OPL_ERROR_F("mods", "%s: material row allocation failed", path);
            break;
        }
        *slot = r;
    }
    if (mod_mat_count(mod))
        OPL_INFO_F("mods", "%s: %d declarative material row(s)", mod->id,
                   mod_mat_count(mod));
}

/* Per-mod row tables, counted across the population. The census node
 * cannot live in the tables themselves: every mod owns a copy of the
 * struct, and the pending-to-live copy would duplicate the node while
 * the list still pointed at the pending slot. */
static mod_fixed s_census_deps, s_census_bindings, s_census_cfg_rows,
                 s_census_cfg_items, s_census_mat_rows, s_census_levels;

/** @brief Record a parsed mod's row-table sizes in the process-wide census counters.
 *
 * Notes the count and capacity of the dependency, binding, config,
 * material and level tables, and the widest choice list of any config
 * row. The counters are armed on the first call. */
static void mod_rows_census(const struct openpete_mod* mod) {
    static int armed;
    if (!armed) {
        armed = 1;
        mod_fixed_init(&s_census_deps, "mod_deps", MOD_DEPS_INIT, 0);
        mod_fixed_init(&s_census_bindings, "mod_bindings",
                       MOD_BINDINGS_INIT, 0);
        mod_fixed_init(&s_census_cfg_rows, "mod_cfg_rows",
                       MOD_CFG_ROWS_INIT, 0);
        mod_fixed_init(&s_census_cfg_items, "mod_cfg_items",
                       MOD_CFG_ITEMS_INIT, 0);
        mod_fixed_init(&s_census_mat_rows, "mod_material_rows",
                       MOD_MATERIAL_ROWS_INIT, 0);
        mod_fixed_init(&s_census_levels, "mod_levels", MOD_LEVELS_INIT, 0);
    }
    mod_fixed_note(&s_census_deps, mod_fvec_count(&mod->deps),
                   mod->deps.cap);
    mod_fixed_note(&s_census_bindings, mod_fvec_count(&mod->bindings),
                   mod->bindings.cap);
    mod_fixed_note(&s_census_cfg_rows, mod_fvec_count(&mod->cfg_rows),
                   mod->cfg_rows.cap);
    mod_fixed_note(&s_census_mat_rows, mod_fvec_count(&mod->mat_rows),
                   mod->mat_rows.cap);
    mod_fixed_note(&s_census_levels, mod_fvec_count(&mod->levels),
                   mod->levels.cap);
    /* The widest choice list any one row carries. */
    for (int i = 0; i < mod_cfg_count(mod); i++) {
        const mod_cfg_row_t* r = mod_cfg_at(mod, i);
        mod_fixed_note(&s_census_cfg_items, mod_fvec_count(&r->items),
                       r->items.cap);
    }
}

/** @brief Parse <mod_dir>/mod.toml into a mod record.
 *
 * Reads the [mod] header, then the dependency, binding, config, level and
 * material tables, and records their sizes in the census. A failure after
 * the header can leave rows already pushed into @p mod; the caller
 * releases them with mod_rows_free.
 * @param mod_dir Mod directory.
 * @param mod Zeroed record to fill.
 * @return 0 on success, -1 when mod.toml does not open (silently), does
 *         not parse, has no [mod] table, or has an invalid or too-new
 *         header. */
static int parse_manifest(const char* mod_dir, struct openpete_mod* mod) {
    char path[MOD_PATH_MAX];
    snprintf(path, sizeof path, "%s/mod.toml", mod_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;

    char errbuf[256];
    toml_table_t* root = toml_parse_file(fp, errbuf, sizeof errbuf);
    fclose(fp);
    if (!root) {
        OPL_ERROR_F("mods", "%s: TOML parse error: %s", path, errbuf);
        return -1;
    }

    int rc = -1;
    toml_table_t* t = toml_table_in(root, "mod");
    if (!t) {
        OPL_ERROR_F("mods", "%s: missing [mod] table", path);
        goto out;
    }
    if (parse_mod_header(t, mod, mod_dir, path) != 0) goto out;
    parse_deps(t, mod);
    parse_games(t, &mod->games, mod->id);
    parse_bindings(root, mod, path);
    parse_config(root, mod, path);
    parse_levels(root, mod, path);
    parse_materials(root, mod, path);
    mod_rows_census(mod);
    rc = 0;
out:
    toml_free(root);
    return rc;
}

/** @brief Feed a file's contents into a SHA-256 context.
 *
 * @return 0, or -1 when the file does not open. */
static int hash_file(openpete_sha256_ctx* c, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t buf[4096];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        openpete_sha256_update(c, buf, got);
    fclose(f);
    return 0;
}

/** @brief qsort comparator ordering mod_src_t rows by path with strcmp.
 *
 * Sorting the sources makes the build key independent of readdir order,
 * which is filesystem-dependent. */
static int cmp_src(const void* a, const void* b) {
    return strcmp(((const mod_src_t*)a)->path, ((const mod_src_t*)b)->path);
}


/** @brief Resolve the compiler for mod builds into @p cc.
 *
 * Order: env OPENPETE_MOD_CC; the bundled toolchain the distribution
 * package stages next to the exe
 * (recomp/tools/llvm-mingw/bin/x86_64-w64-mingw32-clang.exe on Windows,
 * recomp/tools/clang/bin/clang elsewhere); env OPENPETE_CC; then, on
 * POSIX only, env CC or cc. On Windows with none of the first three,
 * @p cc is the empty string and callers fall back to a prebuilt object.
 * @param cc Output buffer.
 * @param cap Size of @p cc. */
static void mods_resolve_cc(char* cc, size_t cap) {
    const char* env = getenv("OPENPETE_MOD_CC");
    if (env && env[0]) {
        snprintf(cc, cap, "%s", env);
        return;
    }
    {
        const char* base = SDL_GetBasePath(); /* trailing sep, SDL-owned */
#ifdef _WIN32
        static const char* const rel = "recomp/tools/llvm-mingw/bin/"
                                       "x86_64-w64-mingw32-clang.exe";
#else
        static const char* const rel = "recomp/tools/clang/bin/clang";
#endif
        char bundled[MOD_PATH_MAX];
        snprintf(bundled, sizeof bundled, "%s%s", base ? base : "./", rel);
        if (file_mtime(bundled) != (time_t)-1) {
            snprintf(cc, cap, "%s", bundled);
            return;
        }
    }
    env = openpete_config_env("OPENPETE_CC");
    if (env && env[0]) {
        snprintf(cc, cap, "%s", env);
        return;
    }
#ifdef _WIN32
    cc[0] = '\0';
#else
    env = getenv("CC");
    snprintf(cc, cap, "%s", env && env[0] ? env : "cc");
#endif
}

/** @brief Return the first line of the resolved mod compiler's --version output, probed once per process.
 *
 * sdk_prefix folds it into every build key, so a toolchain change, or a
 * stamp written on a machine with a different compiler, invalidates the
 * cache instead of reusing an object another compiler built. With no
 * compiler resolved or a failed probe it is the empty string, and the key
 * covers the API version, the SDK and the sources only.
 * @return The identity string, owned by the function. */
static const char* mods_cc_identity(void) {
    static int  s_probed = 0;
    static char s_identity[256];
    if (s_probed) return s_identity;
    s_probed = 1;
    char cc[MOD_PATH_MAX];
    mods_resolve_cc(cc, sizeof cc);
    if (!cc[0]) return s_identity;
    const char* argv[] = { cc, "--version", NULL };
    char out[1024];
    if (openpete_host_spawn_argv(argv, NULL, out, sizeof out) < 0) return s_identity;
    out[strcspn(out, "\r\n")] = '\0';
    snprintf(s_identity, sizeof s_identity, "%.*s", (int)sizeof s_identity - 1, out);
    return s_identity;
}

/** The bundled toolchain's mod sysroot (scripts/setup_linux_clang.sh),
 *  resolved beside the mod compiler once per process. */
typedef struct {
    int  ok;                    /**< 1 when both inputs exist. */
    char isystem[MOD_PATH_MAX + 32];  /**< <tree>/sysroot/include. */
    char builtins[MOD_PATH_MAX + 64]; /**< <tree>/sysroot/lib/libclang_rt.builtins.a. */
    char stamp[512];            /**< sysroot/STAMP's first line: the pins. */
} mod_cc_sysroot_t;

/** @brief Return the mod sysroot that sits beside the resolved compiler, probed once per process.
 *
 * The bundled Linux toolchain carries glibc and Linux uapi headers in
 * <tree>/sysroot/include and the compiler-rt builtins archive in
 * <tree>/sysroot/lib, where <tree> is the parent of the compiler's bin/
 * directory. When both exist the mod compile line reads no header,
 * crt object or library from the host (mod_compile). A compiler named
 * without a directory, or one whose tree has no sysroot (a system
 * compiler), leaves ok = 0 and the compile line unchanged. Always ok = 0
 * on Windows, whose llvm-mingw toolchain carries its own CRT.
 * @return The cached result, owned by the function; not thread-safe. */
static const mod_cc_sysroot_t* mods_cc_sysroot(void) {
    static int              s_probed = 0;
    static mod_cc_sysroot_t s_sys;
    if (s_probed) return &s_sys;
    s_probed = 1;
#ifndef _WIN32
    char cc[MOD_PATH_MAX];
    mods_resolve_cc(cc, sizeof cc);
    char* slash = strrchr(cc, '/');
    if (!slash) return &s_sys;
    *slash = '\0'; /* cc now names its bin/ directory */
    char probe[MOD_PATH_MAX + 48];
    snprintf(s_sys.isystem, sizeof s_sys.isystem, "%s/../sysroot/include", cc);
    snprintf(s_sys.builtins, sizeof s_sys.builtins,
             "%s/../sysroot/lib/libclang_rt.builtins.a", cc);
    snprintf(probe, sizeof probe, "%s/stdio.h", s_sys.isystem);
    if (file_mtime(probe) == (time_t)-1 ||
        file_mtime(s_sys.builtins) == (time_t)-1)
        return &s_sys;
    snprintf(probe, sizeof probe, "%s/../sysroot/STAMP", cc);
    FILE* f = fopen(probe, "rb");
    if (f) {
        if (!fgets(s_sys.stamp, sizeof s_sys.stamp, f)) s_sys.stamp[0] = '\0';
        fclose(f);
    }
    s_sys.ok = 1;
#endif
    return &s_sys;
}

/* The flags a compiler with a sysroot (mods_cc_sysroot) adds after
 * mod_cc_flags: no host header directory (the sysroot's goes in with
 * -isystem), no crt objects and no default libraries. The object then
 * carries no DT_NEEDED; its libc and libm references bind at dlopen to
 * the process, which already holds both, and the compiler runtime comes
 * from the builtins archive appended after the sources. */
static const char* const mod_cc_sysroot_flags[] = {
    "-nostdlibinc", "-nostdlib", "-nostartfiles",
};
#define MOD_CC_SYSROOT_FLAG_COUNT \
    (sizeof mod_cc_sysroot_flags / sizeof mod_cc_sysroot_flags[0])

/* Census only: the list is freed as soon as it has been used, so a
 * named vec would be gone from the list by the exit report. One
 * process-global counter carries the population's high-water. */
static mod_fixed s_census_srcs;

/** @brief List <mod_dir>/src/<n>.c as a vec of mod_src_t, sorted by path.
 *
 * Non-recursive; the sort makes the build key independent of directory
 * order. A row that cannot be allocated ends the listing early with the
 * rows gathered so far. The result is quiet when src/ is absent, since the
 * callers decide whether that is a binary-only mod, an assets-only mod, or
 * an error.
 * @param mod_dir Mod directory.
 * @param out Vec to fill; initialised only when src/ opens, so the caller
 *        passes a zeroed vec and releases it with mod_fvec_free.
 * @return The number of sources, 0 when src/ holds no .c file, or -1 when
 *         there is no src/ directory. */
static int mod_list_srcs(const char* mod_dir, mod_fvec* out) {
    char src_dir[MOD_PATH_MAX];
    snprintf(src_dir, sizeof src_dir, "%s/src", mod_dir);
    DIR* d = opendir(src_dir);
    if (!d) return -1;
    mod_fvec_init(out, sizeof(mod_src_t), MOD_SRCS_INIT, NULL, 0);
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 3 || strcmp(e->d_name + len - 2, ".c") != 0) continue;
        char path[sizeof ((mod_src_t*)0)->path];
        if (snprintf(path, sizeof path, "%s/%s", src_dir, e->d_name) >= (int)sizeof path) {
            OPL_ERROR_F("mods", "%s/%s: source path exceeds %zu bytes - not compiled",
                        src_dir, e->d_name, sizeof path - 1);
            continue;
        }
        mod_src_t* slot = (mod_src_t*)mod_fvec_push(out);
        if (!slot) break;
        memcpy(slot->path, path, sizeof path);
    }
    closedir(d);
    /* The build key hashes the sources in order, so the order must not
     * depend on the directory's. */
    size_t n = mod_fvec_count(out);
    if (n > 1)
        qsort(mod_fvec_data(out), n, sizeof(mod_src_t), cmp_src);
    static int census_armed;
    if (!census_armed) {
        census_armed = 1;
        mod_fixed_init(&s_census_srcs, "mod_srcs", MOD_SRCS_INIT, 0);
    }
    mod_fixed_note(&s_census_srcs, n, out->cap);
    return (int)n;
}

/** @brief Return the path of source row i of a mod_list_srcs vec.
 *
 * @param v Vec filled by mod_list_srcs.
 * @param i Row index below the count; not range-checked.
 * @return The path, stored in the row. */
static const char* src_path(const mod_fvec* v, int i) {
    return ((const mod_src_t*)mod_fvec_at(v, (size_t)i))->path;
}

/* CONTENT-HASH build key: api version + compiler identity + game id +
 * compile flags + SDK file bytes (each from the directory it resolves in) + sorted source
 * bytes. mtime churn (git checkout, cp, zip
 * re-extract) never changes it; an SDK regen, api bump, toolchain swap,
 * or real source edit always does. Returns 0 with key_out filled, -1 if
 * the mod has no compilable sources. */
/* The SDK half of the build key is the same for every mod and is the
 * large half - a few hundred KB of generated headers against a few KB of
 * mod source - so it is hashed once per round and the digest state is
 * cloned per mod. The byte stream fed to the hash is unchanged, so the
 * keys, and every stamp already on disk, are identical to hashing it
 * per mod. The hot-reload watch drops the prefix before each poll round
 * so an SDK edit is still noticed. */
static openpete_sha256_ctx s_sdk_prefix;
static char                s_sdk_prefix_dir[MOD_PATH_MAX];
static char                s_sdk_prefix_game[32];
static int                 s_sdk_prefix_ok = 0;
static uint64_t            s_sdk_prefix_stamp;

/* The compile line's flags between the compiler and the include paths.
 * sdk_prefix hashes them into every build key, so a flag change rebuilds
 * every stamped mod. Windows: one dllexport in a mod source would turn off
 * the MinGW linker's export of every global, and with it
 * openpete_mod_entry, openpete_mod_abi and openpete_mod_game;
 * --export-all-symbols keeps them all. Elsewhere _GNU_SOURCE makes glibc's
 * headers declare its extensions (RTLD_DEFAULT, dladdr). */
static const char* const mod_cc_flags[] = {
    "-shared",
#ifndef _WIN32
    "-fPIC", "-D_GNU_SOURCE",
#else
    "-Wl,--export-all-symbols",
#endif
    "-O2", "-g", "-fno-strict-aliasing", "-Wall", "-Wundef",
};
#define MOD_CC_FLAG_COUNT (sizeof mod_cc_flags / sizeof mod_cc_flags[0])

/* The path remaps mod_compile adds after mod_cc_flags, in this order, as
 * templates: `<cwd>`, `<toolchain>`, `<sdk>` and `<mod>` stand for the
 * working directory the compiler inherits, the compiler's install tree,
 * the SDK directory and the mod's directory, and `<id>` for the mod id.
 * They keep every absolute path of the machine out of the object (DWARF
 * names and DW_AT_comp_dir, __FILE__), so the module a bug report ships
 * holds no home path. GCC and clang apply the last remap that matches, so
 * the most specific directory comes last. The working directory is
 * remapped rather than named with -fdebug-compilation-dir, which GCC does
 * not accept. sdk_prefix hashes the templates, not the paths: an object
 * built under the remaps does not depend on where the directories are (an
 * AppImage mounts its tree at a new path every launch). */
static const char* const mod_cc_map_templates[] = {
    "-ffile-prefix-map=<cwd>=.",
    "-ffile-prefix-map=<toolchain>=toolchain",
    "-ffile-prefix-map=<sdk>=sdk",
    "-ffile-prefix-map=<mod>=mods/<id>",
};
#define MOD_CC_MAP_COUNT (sizeof mod_cc_map_templates / sizeof mod_cc_map_templates[0])

static const char* const sdk_inputs[] = {
    "openpete_mod_api.h", "openpete_mod_ui.h", "openpete_sdk_symbols.h",
    "openpete_sdk_wrappers.h", "psx_runtime.h",
    "openpete_game_structs.h", "openpete_game.h",
    "openpete_mod_abi.c",
    "openpete_imgui.h", "openpete_imgui_table.h",
    "openpete_imgui_macros.h", "dcimgui.h", "imconfig.h",
};
#define SDK_INPUT_COUNT (sizeof sdk_inputs / sizeof sdk_inputs[0])

/** @brief Resolve an SDK file the way a mod's compile line does: the running game's directory before the SDK root.
 *
 * The compile line passes `-I <sdk>/games/<game-id>` before `-I <sdk>`,
 * so a file present in both resolves to the per-game copy.
 * @param sdk_dir The staged SDK root.
 * @param name File name.
 * @param out Receives the resolved path (the root path when the file is
 *        in neither); truncated to @p cap.
 * @param cap Size of @p out.
 * @return 1 when the file resolved in the per-game directory, else 0. */
static int sdk_input_path(const char* sdk_dir, const char* name, char* out,
                          size_t cap) {
    struct stat st;
    snprintf(out, cap, "%s/games/%s/%s", sdk_dir, mods_build_game(), name);
    if (stat(out, &st) == 0) return 1;
    snprintf(out, cap, "%s/%s", sdk_dir, name);
    return 0;
}

/** @brief Fold the mtime and size of every SDK build-key input into one FNV-1a-style number.
 *
 * The digest decides a build key; this stamp only decides whether
 * sdk_prefix_recheck drops the cached digest. Each input is stat()ed
 * where sdk_input_path resolves it; a missing input folds as 0.
 * @param sdk_dir The staged SDK directory.
 * @return The stamp. */
static uint64_t sdk_prefix_stamp(const char* sdk_dir) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < SDK_INPUT_COUNT; i++) {
        char p[MOD_PATH_MAX];
        uint64_t v = (uint64_t)sdk_input_path(sdk_dir, sdk_inputs[i], p,
                                              sizeof p);
        struct stat st;
        if (stat(p, &st) == 0)
            v += (uint64_t)st.st_mtime * 1000003ull + (uint64_t)st.st_size;
        h = (h ^ v) * 1099511628211ull;
    }
    return h;
}

/** @brief Drop the cached SDK prefix digest when the SDK directory, the game or the stamp of its inputs changed.
 *
 * The hot-reload poll calls it on the sim thread every 64 ticks, where
 * stat()ing the inputs is far cheaper than rehashing their bytes. No-op
 * when no prefix is cached.
 * @param sdk_dir The staged SDK directory. */
static void sdk_prefix_recheck(const char* sdk_dir) {
    if (!s_sdk_prefix_ok) return;
    if (strcmp(s_sdk_prefix_dir, sdk_dir) != 0 ||
        strcmp(s_sdk_prefix_game, mods_build_game()) != 0 ||
        sdk_prefix_stamp(sdk_dir) != s_sdk_prefix_stamp)
        s_sdk_prefix_ok = 0;
}

/** @brief Return the SHA-256 state that every mod's build key starts from.
 *
 * Hashes the API version, the compiler identity, the running game's id,
 * the compile line's fixed flags (mod_cc_flags) and path-remap templates (mod_cc_map_templates), with a compiler sysroot its flags and the pins in its STAMP, then for each file of sdk_inputs in order the directory it resolves in
 * (sdk_input_path: per-game or root) and its bytes; an absent file
 * contributes its resolution byte only. The state is cached per (SDK
 * directory, game id) until sdk_prefix_recheck drops it, and callers
 * copy it before feeding a mod's sources.
 * @param sdk_dir The staged SDK directory.
 * @return The cached state, owned by the function; not thread-safe. */
static const openpete_sha256_ctx* sdk_prefix(const char* sdk_dir) {
    const char* game = mods_build_game();
    if (s_sdk_prefix_ok && strcmp(s_sdk_prefix_dir, sdk_dir) == 0 &&
        strcmp(s_sdk_prefix_game, game) == 0)
        return &s_sdk_prefix;
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    const unsigned api_ver = OPENPETE_MOD_API_VERSION;
    openpete_sha256_update(&hc, &api_ver, sizeof api_ver);
    {
        const char* ident = mods_cc_identity();
        openpete_sha256_update(&hc, ident, strlen(ident));
    }
    openpete_sha256_update(&hc, game, strlen(game) + 1);
    for (size_t i = 0; i < MOD_CC_FLAG_COUNT; i++)
        openpete_sha256_update(&hc, mod_cc_flags[i],
                               strlen(mod_cc_flags[i]) + 1);
    for (size_t i = 0; i < MOD_CC_MAP_COUNT; i++)
        openpete_sha256_update(&hc, mod_cc_map_templates[i],
                               strlen(mod_cc_map_templates[i]) + 1);
    {
        /* The sysroot's pins, not its path: an AppImage mounts at a new
         * path every launch, and the key must not change with it. */
        const mod_cc_sysroot_t* sys = mods_cc_sysroot();
        if (sys->ok) {
            for (size_t i = 0; i < MOD_CC_SYSROOT_FLAG_COUNT; i++)
                openpete_sha256_update(&hc, mod_cc_sysroot_flags[i],
                                       strlen(mod_cc_sysroot_flags[i]) + 1);
            openpete_sha256_update(&hc, sys->stamp, strlen(sys->stamp) + 1);
        }
    }
    for (size_t i = 0; i < SDK_INPUT_COUNT; i++) {
        char p[MOD_PATH_MAX];
        const uint8_t per_game =
            (uint8_t)sdk_input_path(sdk_dir, sdk_inputs[i], p, sizeof p);
        openpete_sha256_update(&hc, &per_game, 1);
        (void)hash_file(&hc, p); /* absent file = stable no-op */
    }
    s_sdk_prefix = hc;
    snprintf(s_sdk_prefix_dir, sizeof s_sdk_prefix_dir, "%s", sdk_dir);
    snprintf(s_sdk_prefix_game, sizeof s_sdk_prefix_game, "%s", game);
    s_sdk_prefix_stamp = sdk_prefix_stamp(sdk_dir);
    s_sdk_prefix_ok = 1;
    return &s_sdk_prefix;
}

/** @brief Hash a mod's sources and the SDK headers into the hex build key of its compiled object.
 *
 * The key covers every source file listed by mod_list_srcs plus the SDK
 * prefix hash of sdk_dir, so a source or SDK header change alters it and
 * a config-only change does not. The result is compared with the stamp
 * beside the cached object to decide whether to recompile. An unreadable
 * source is skipped by the hash.
 * @param mod_dir Mod directory holding src/.
 * @param sdk_dir SDK directory whose headers enter the prefix hash.
 * @param key_out Receives 64 hex digits and a NUL; untouched on failure.
 * @return 0, or -1 when the mod has no source files. */
static int mod_compute_key(const char* mod_dir, const char* sdk_dir,
                           char key_out[65]) {
    mod_fvec srcs;
    memset(&srcs, 0, sizeof srcs);
    int n_srcs = mod_list_srcs(mod_dir, &srcs);
    if (n_srcs <= 0) { mod_fvec_free(&srcs); return -1; }

    openpete_sha256_ctx hc = *sdk_prefix(sdk_dir);
    for (int i = 0; i < n_srcs; i++)
        (void)hash_file(&hc, src_path(&srcs, i));
    mod_fvec_free(&srcs);
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key_out);
    return 0;
}

/** @brief Hash a mod's config.toml into a hex key for the hot-reload watch.
 *
 * Config is not a compile input, so its key is tracked apart from
 * mod_compute_key and a config-only change reloads the mod against the
 * cached object. An absent file yields the key "-".
 * @return 0 always. */
static int config_compute_key(const char* mod_dir, char key_out[65]) {
    char p[MOD_PATH_MAX + 16];
    snprintf(p, sizeof p, "%s/config.toml", mod_dir);
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    if (hash_file(&hc, p) != 0) {
        snprintf(key_out, 65, "-");   /* no config file */
        return 0;
    }
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key_out);
    return 0;
}

/* Stamp suffixes: MOD_STAMP_OK holds the build key of the cached object,
 * MOD_STAMP_FAIL the build key whose compile the compiler rejected. */
#define MOD_STAMP_OK   ".hash"
#define MOD_STAMP_FAIL ".fail"

/** @brief Format the path of a mod's build-key stamp, <mod_dir>/.build/<game-id>/<id> plus @p suffix.
 *
 * @param suffix MOD_STAMP_OK or MOD_STAMP_FAIL.
 * @param out Output buffer; the path is truncated to @p cap.
 * @param cap Size of @p out. */
static void mod_stamp_path(const char* mod_dir, const char* id,
                           const char* suffix, char* out, size_t cap) {
    snprintf(out, cap, "%s/.build/%s/%s%s", mod_dir, mods_build_game(), id,
             suffix);
}

/** @brief Read the build key a mod's stamp file holds.
 *
 * Reads at most 64 bytes and drops one trailing newline.
 * @param suffix MOD_STAMP_OK or MOD_STAMP_FAIL.
 * @param stamped Output buffer of 65 bytes, always NUL-terminated; empty
 *        when the stamp file does not open. */
static void mod_read_stamp(const char* mod_dir, const char* id,
                           const char* suffix, char stamped[65]) {
    stamped[0] = '\0';
    char stamp_path[MOD_PATH_MAX + 16];
    mod_stamp_path(mod_dir, id, suffix, stamp_path, sizeof stamp_path);
    FILE* f = fopen(stamp_path, "rb");
    if (!f) return;
    size_t got = fread(stamped, 1, 64, f);
    stamped[got] = '\0';
    if (got && stamped[got - 1] == '\n') stamped[got - 1] = '\0';
    fclose(f);
}

/** @brief Test whether a mod's cached object is current for a build key.
 *
 * @return 1 when `.build/<game-id>/<id>` plus the platform object
 *         extension exists and `.build/<game-id>/<id>.hash` holds `key`,
 *         0 otherwise. */
static int mod_key_cached(const char* mod_dir, const char* id,
                          const char* key) {
    char so_path[MOD_PATH_MAX];
    if (mod_obj_path(mod_dir, id, so_path, sizeof so_path) != 0) return 0;
    if (file_mtime(so_path) == (time_t)-1) return 0;
    char stamped[65];
    mod_read_stamp(mod_dir, id, MOD_STAMP_OK, stamped);
    return strcmp(stamped, key) == 0;
}

/** @brief Test whether the compiler already rejected a mod's sources under a build key.
 *
 * The key covers the sources, the SDK, the compile flags and the
 * compiler's identity, so an unchanged failing mod is not recompiled on
 * every boot and any edit to those inputs compiles again. Inputs outside
 * the key (system headers and libraries) do not clear it; --compile-mods
 * recompiles regardless.
 * @return 1 when `.build/<game-id>/<id>.fail` holds `key`, 0 otherwise. */
static int mod_key_failed(const char* mod_dir, const char* id,
                          const char* key) {
    char stamped[65];
    mod_read_stamp(mod_dir, id, MOD_STAMP_FAIL, stamped);
    return strcmp(stamped, key) == 0;
}

/** @brief Write a build key, newline-terminated, to a mod's stamp file.
 *
 * @param suffix MOD_STAMP_OK or MOD_STAMP_FAIL. */
static void mod_write_stamp(const char* mod_dir, const char* id,
                            const char* suffix, const char* key) {
    char stamp_path[MOD_PATH_MAX + 16], line[80];
    mod_stamp_path(mod_dir, id, suffix, stamp_path, sizeof stamp_path);
    int ln = snprintf(line, sizeof line, "%s\n", key);
    (void)op_atomic_write_file(stamp_path, line, (size_t)ln);
}

/** @brief Write the compiler response file listing a mod's sources.
 *
 * Writes each of the `n_srcs` source paths, then
 * `<sdk_dir>/openpete_mod_abi.c`, one per line, double-quoted, with every
 * backslash converted to a forward slash. Both tokenizers accept `/`,
 * and neither accepts `\\` the same way: mingw-gcc reads an @file
 * GNU-style, where a backslash escapes the next character, while the
 * bundled llvm-mingw clang reads it Windows-style.
 *
 * @return 0 on success, -1 when the file cannot be opened or a write or
 *         the close fails. */
static int write_sources_rsp(const char* rsp_path, const mod_fvec* srcs,
                             int n_srcs, const char* sdk_dir) {
    FILE* f = fopen(rsp_path, "wb");
    if (!f) return -1;
    for (int i = 0; i <= n_srcs; i++) {
        char p[MOD_PATH_MAX + 32];
        if (i < n_srcs)
            snprintf(p, sizeof p, "%s", src_path(srcs, i));
        else
            snprintf(p, sizeof p, "%s/openpete_mod_abi.c", sdk_dir);
        for (char* q = p; *q; q++)
            if (*q == '\\') *q = '/';
        fprintf(f, "\"%s\"\n", p);
    }
    int ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/** @brief Compile a mod's src/ C files into out_path with the resolved compiler, its output in .build/<game-id>/compile.log.
 *
 * The compiler runs from an argument vector (openpete_host_spawn_argv),
 * the sources and the SDK's ABI descriptor from a response file, with
 * `-I <sdk>/games/<game-id>` before `-I <sdk>` and `-Wundef`, so the
 * running game's generated headers and openpete_game.h shadow any root
 * copy and a misspelled game macro warns; on Windows with
 * `-Wl,--export-all-symbols`, so every global the loader resolves stays
 * exported when a source marks one `dllexport`; elsewhere with
 * `-D_GNU_SOURCE`, so glibc's headers declare its extensions
 * (`RTLD_DEFAULT`, `dladdr`) to the mod's sources. The fixed flags come
 * from mod_cc_flags, which the build key hashes; the path remaps of
 * mod_cc_map_templates follow them. A compiler with a
 * sysroot beside it (mods_cc_sysroot, the bundled Linux clang) adds
 * mod_cc_sysroot_flags, `-isystem <sysroot>/include` and, after the
 * sources, the builtins archive, so the build reads nothing from the
 * host. Log lines carry the
 * label `<id> [<game-id>]`. `stamp`
 * = 1 records the build key on success so an unchanged mod never
 * recompiles (0 = hot-reload .hotN objects on Windows, where the base DLL
 * is file-locked and must go stale until the next boot). A failed compile
 * removes the stamp and keeps any existing object; with `stamp` = 1 and a
 * compiler exit status it records the key in the `.fail` stamp
 * (mod_key_failed), which a later success removes. The compiler runs
 * stoppable (openpete_host_spawn_argv_stoppable): a stop (the player quit
 * the launcher's progress view) kills it, removes the stamp and the object
 * it was writing, and records no `.fail` stamp, so the next pick compiles
 * the mod again.
 * @return 0 on success, -1 when there are no sources, no compiler, the
 *         response file cannot be written, or the compile fails. */
static int mod_compile(const char* mod_dir, const char* id,
                       const char* sdk_dir, const char* key,
                       const char* out_path, int stamp) {
    mod_fvec srcs;
    memset(&srcs, 0, sizeof srcs);
    int n_srcs = mod_list_srcs(mod_dir, &srcs);
    if (n_srcs <= 0) {
        OPL_ERROR_F("mods", "%s: no .c sources in src/", id);
        mod_fvec_free(&srcs);
        return -1;
    }

    char cc[MOD_PATH_MAX];
    mods_resolve_cc(cc, sizeof cc);
    if (!cc[0]) {
        OPL_ERROR_F("mods", "%s: no compiler - expected the bundled "
                    "toolchain at recomp/tools/ next to the exe, or set "
                    "OPENPETE_MOD_CC", id);
        mod_fvec_free(&srcs);
        return -1;
    }

    char build_dir[MOD_PATH_MAX];
    mod_make_build_dir(mod_dir); /* EEXIST is fine */
    mod_game_build_dir(mod_dir, build_dir, sizeof build_dir);
    const char* game = mods_build_game();

    char stamp_path[MOD_PATH_MAX + 16], fail_path[MOD_PATH_MAX + 16];
    mod_stamp_path(mod_dir, id, MOD_STAMP_OK, stamp_path, sizeof stamp_path);
    mod_stamp_path(mod_dir, id, MOD_STAMP_FAIL, fail_path, sizeof fail_path);

    /* The sources go in a response file, not on the command line, whose
     * length the platform limits (32767 characters on Windows). The ABI
     * descriptor rides the same file (openpete_mod_api.h, "ABI
     * descriptor"): compiled against the same SDK as the mod's own
     * sources, it records the api the object is really built for. */
    char rsp_path[MOD_PATH_MAX + 16];
    snprintf(rsp_path, sizeof rsp_path, "%s/sources.rsp", build_dir);
    if (write_sources_rsp(rsp_path, &srcs, n_srcs, sdk_dir) != 0) {
        OPL_ERROR_F("mods", "%s: cannot write %s", id, rsp_path);
        mod_fvec_free(&srcs);
        return -1;
    }
    mod_fvec_free(&srcs);

    /* The compiler is started from an argument vector, never a shell: a
     * mods, SDK or output path holding a quote, a space, `%NAME%` or `$`
     * is one argument, taken literally. */
    char inc_game[MOD_PATH_MAX + 48], inc[MOD_PATH_MAX + 4];
    char rsp_arg[MOD_PATH_MAX + 20], log_path[MOD_PATH_MAX + 16];
    snprintf(inc_game, sizeof inc_game, "-I%s/games/%s", sdk_dir, game);
    snprintf(inc, sizeof inc, "-I%s", sdk_dir);
    snprintf(rsp_arg, sizeof rsp_arg, "@%s", rsp_path);
    snprintf(log_path, sizeof log_path, "%s/compile.log", build_dir);
    const mod_cc_sysroot_t* sys = mods_cc_sysroot();
    /* mod_cc_map_templates, filled in. The toolchain tree is the parent
     * of the compiler's bin/ directory; a compiler named without a
     * directory has none to map. */
    char map_mod[2 * MOD_PATH_MAX + 64], map_sdk[MOD_PATH_MAX + 32];
    char map_tc[MOD_PATH_MAX + 40], tree[MOD_PATH_MAX];
    char map_cwd[MOD_PATH_MAX + 32], cwd[MOD_PATH_MAX];
    snprintf(map_mod, sizeof map_mod, "-ffile-prefix-map=%s=mods/%s", mod_dir, id);
    snprintf(map_sdk, sizeof map_sdk, "-ffile-prefix-map=%s=sdk", sdk_dir);
    snprintf(tree, sizeof tree, "%s", cc);
    for (int up = 0; up < 2; up++) {
        char* sl = strrchr(tree, '/');
#ifdef _WIN32
        char* bs = strrchr(tree, '\\');
        if (bs && (!sl || bs > sl)) sl = bs;
#endif
        if (sl) *sl = '\0'; else tree[0] = '\0';
    }
    snprintf(map_tc, sizeof map_tc, "-ffile-prefix-map=%s=toolchain", tree);
    if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
    snprintf(map_cwd, sizeof map_cwd, "-ffile-prefix-map=%s=.", cwd);
    const char* argv[MOD_CC_FLAG_COUNT + MOD_CC_MAP_COUNT + MOD_CC_SYSROOT_FLAG_COUNT + 11];
    size_t argc = 0;
    argv[argc++] = cc;
    for (size_t i = 0; i < MOD_CC_FLAG_COUNT; i++)
        argv[argc++] = mod_cc_flags[i];
    if (cwd[0]) argv[argc++] = map_cwd;
    if (tree[0]) argv[argc++] = map_tc;
    argv[argc++] = map_sdk;
    argv[argc++] = map_mod;
    if (sys->ok) {
        for (size_t i = 0; i < MOD_CC_SYSROOT_FLAG_COUNT; i++)
            argv[argc++] = mod_cc_sysroot_flags[i];
        argv[argc++] = "-isystem";
        argv[argc++] = sys->isystem;
    }
    argv[argc++] = inc_game;
    argv[argc++] = inc;
    argv[argc++] = "-o";
    argv[argc++] = out_path;
    argv[argc++] = rsp_arg;
    /* After the sources, so the archive members they reference link. */
    if (sys->ok) argv[argc++] = sys->builtins;
    argv[argc] = NULL;

    OPL_INFO_F("mods", "%s [%s]: compiling %d source file%s", id, game,
               n_srcs, n_srcs == 1 ? "" : "s");
    int rc = openpete_host_spawn_argv_stoppable(argv, log_path, NULL, 0);
    if (rc == OPENPETE_HOST_SPAWN_STOPPED) {
        OPL_INFO_F("mods", "%s [%s]: compile stopped; the mod stays stale", id, game);
        remove(stamp_path);
        remove(out_path);
        return -1;
    }
    if (rc != 0) {
        OPL_ERROR_F("mods", "%s [%s]: compile failed (see %s/compile.log) "
                    "- disabled", id, game, build_dir);
        /* Remove the STAMP only: nothing may cache-hit a failed build,
         * but the object file survives - it may be an author-shipped
         * prebuilt (cross-build handoff) that a no-compiler boot can
         * still fall back to. Deleting it here destroyed exactly that
         * artifact once; don't repeat it. */
        remove(stamp_path);
        /* A compiler exit status rejects these inputs; -1 (not started,
         * killed by a signal) says nothing about them and is not
         * recorded. */
        if (stamp && rc > 0)
            mod_write_stamp(mod_dir, id, MOD_STAMP_FAIL, key);
        return -1;
    }
    if (stamp) {
        mod_write_stamp(mod_dir, id, MOD_STAMP_OK, key);
        remove(fail_path);
    }
    return 0;
}

/** @brief Build a mod's src/ C files into `.build/<game-id>/<id>` plus the object extension when the build key differs from the stamped one.
 *
 * Stores the computed key in mod->build_key, fills so_path with the
 * object path and mod->obj_kind with its mod_obj_kind_t. A key the
 * compiler already rejected (mod_key_failed) is not compiled again. With no
 * compiler resolved, the prebuilt object mod_find_prebuilt finds is used,
 * with a warning when it is the per-game object and its stamp exists and
 * mismatches.
 * @return 0 on a cache hit, a fresh compile or a prebuilt fallback; -1
 *         when src/ has no .c files, no compiler and no prebuilt object
 *         exist, the key failed before, or the compile fails. */
static int build_mod(struct openpete_mod* mod, const char* sdk_dir,
                     char* so_path, size_t so_path_len) {
    char key[65];
    if (mod_compute_key(mod->dir, sdk_dir, key) != 0) {
        OPL_ERROR_F("mods", "%s: no .c sources in src/", mod->id);
        return -1;
    }
    snprintf(mod->build_key, sizeof mod->build_key, "%s", key);

    if (mod_obj_path(mod->dir, mod->id, so_path, so_path_len) != 0) return -1;
    mod->obj_kind = MOD_OBJ_GAME;
    if (mod_key_cached(mod->dir, mod->id, key)) {
        OPL_DEBUG_F("mods", "%s: cached %s up to date (key %.12s)",
                    mod->id, so_path, key);
        return 0;
    }
    if (mod_key_failed(mod->dir, mod->id, key)) {
        OPL_ERROR_F("mods", "%s [%s]: these sources failed to compile (see "
                    "%s/.build/%s/compile.log); not recompiled until src/, "
                    "the SDK or the compiler changes, or --compile-mods - "
                    "disabled", mod->id, mods_build_game(), mod->dir,
                    mods_build_game());
        return -1;
    }

    {
        char cc[MOD_PATH_MAX];
        mods_resolve_cc(cc, sizeof cc);
        if (!cc[0]) {
            /* No compiler on this box (Windows without the bundled
             * toolchain): fall back to a prebuilt DLL. Verified-stale (a
             * stamp exists and mismatches) loads loudly; an unstamped
             * DLL (typical for cross-compiled staging) loads silently -
             * there is nothing trustworthy to compare against. */
            const int kind = mod_find_prebuilt(mod->dir, mod->id, so_path,
                                               so_path_len);
            if (kind >= 0) {
                char stamped[65];
                mod->obj_kind = kind;
                mod_read_stamp(mod->dir, mod->id, MOD_STAMP_OK, stamped);
                if (kind == MOD_OBJ_GAME && stamped[0])
                    OPL_WARN_F("mods", "%s: %s was built from DIFFERENT "
                               "sources/SDK than what is on disk - "
                               "loading it anyway (rebuild the DLL to "
                               "update)", mod->id, so_path);
                return 0;
            }
            OPL_ERROR_F("mods", "%s: no compiler and no prebuilt %s - "
                        "install the packaged toolchain (recomp/tools/), "
                        "set OPENPETE_MOD_CC, or build the DLL with e.g. "
                        "x86_64-w64-mingw32-gcc -shared -O2 -I<sdk> - "
                        "disabled", mod->id, so_path);
            return -1;
        }
    }
    return mod_compile(mod->dir, mod->id, sdk_dir, key, so_path, 1);
}

typedef int (*mod_entry_fn)(const openpete_mod_api_t*, openpete_mod_t*);

static void mod_register_materials(struct openpete_mod* mod);

/** @brief Arm a mod's [[material]] rows, run openpete_mod_entry and resolve its UI section.
 *
 * Shared by the boot load and the deferred first-enable path. The arena's
 * per-mod call cursor is reset before entry. A missing entry symbol or a
 * nonzero entry result disables the mod; the latter strips what the entry
 * registered. Sets entered on success.
 * @return 0, or -1 on failure. */
static int mod_run_entry(struct openpete_mod* mod) {
    /* Manifest rows register before the entry's C materials, so on one
     * selector the C registration wins from api pin 13 and the manifest row
     * below it (mod_mat_outranks). */
    mod_register_materials(mod);
    mod_entry_fn entry =
        (mod_entry_fn)mod_dlsym(mod->dl, "openpete_mod_entry");
    if (!entry) {
        OPL_ERROR_F("mods", "%s: no openpete_mod_entry symbol - disabled",
                    mod->id);
        return -1; /* keep the .so resident; it is inert without chains */
    }
    mod_order_note(mod, OPENPETE_MODSET_EV_ENTRY, 0, 0);
    openpete_mods_arena_entry_begin(mod->id);
    const openpete_mods_site_t site = openpete_mods_site_open(OPENPETE_MODS_CTX_ENTRY);
    openpete_mods_guard_enter(mod->id);
    mod->in_entry = 1;
    int rc = entry(&s_api, mod);
    mod->in_entry = 0;
    openpete_mods_guard_leave();
    openpete_mods_site_close(site);
    openpete_mods_arena_entry_end(mod->id);
    if (rc != 0) {
        OPL_ERROR_F("mods", "%s: openpete_mod_entry returned %d - disabled",
                    mod->id, rc);
        mod_strip_registrations(mod); /* whatever it registered before failing */
        return -1;
    }
    mod_resolve_ui_section(mod, mod->dl);
    mod->entered = 1;
    if (s_mods_init_ran) openpete_modsave_on_entered();
    return 0;
}

/** @brief Register a mod's [[material]] rows, the zero-C material tier.
 *
 * Runs before openpete_mod_entry so a mixed mod's C registrations land
 * later and win the pixels (the last match takes them): the manifest is
 * the declared default, C is the refinement. Each row registers its frag
 * and optional vert shader, then the material, then applies its params and
 * its disabled state. A row whose shader fails to build or whose
 * registration fails is dropped with an error and the rest still arm; a
 * row is never downgraded to a wider selector. */
static void mod_register_materials(struct openpete_mod* mod) {
    for (int i = 0; i < mod_mat_count(mod); i++) {
        const mod_material_row_t* r = mod_mat_at(mod, i);
        int frag = -1, vert = -1;
        if (r->frag[0]) {
            frag = openpete_mods_shader_register(mod, r->frag);
            if (frag < 0) {
                OPL_ERROR_F("mods", "%s: [[material]] frag \"%s\" failed to "
                            "build - row skipped", mod->id, r->frag);
                continue;
            }
        }
        if (r->vert[0]) {
            vert = openpete_mods_shader_register(mod, r->vert);
            if (vert < 0) {
                OPL_ERROR_F("mods", "%s: [[material]] vert \"%s\" failed to "
                            "build - row skipped", mod->id, r->vert);
                continue;
            }
        }
        openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
        sel.channel    = r->channel;
        sel.ident      = r->ident;
        sel.moby_class = r->moby_class;
        sel.texture    = r->texture;
        sel.level      = r->level;
        sel.flags      = r->flags;
        int h = openpete_mods_material_register(mod, &sel, vert, frag, NULL);
        if (h < 0) continue;          /* register already logged the reason */
        if (r->n_params)
            (void)openpete_mods_material_set_params(
                mod, h, r->params, (uint32_t)(r->n_params * sizeof(float)));
        if (!r->enabled)
            (void)openpete_mods_material_enable(mod, h, 0);
    }
}

/** @brief Register a mod's selected asset directory as an overlay root when it has one.
 *
 * The directory is mod->assets_dir, chosen by mod_select_assets. Called
 * after entry, so roots register in entry order; the texture and music
 * namespaces consume them. */
static void mod_register_assets(struct openpete_mod* mod) {
    if (!mod->has_assets) return;
    (void)openpete_mods_register_asset_root(mod->assets_dir, mod->id);
}

/** @brief Choose a mod's asset directory for the running game and record whether it exists.
 *
 * `assets/<game id>/` when that directory exists, else `assets/`: a mod
 * shipping content for several games keeps one subtree per game, named in
 * that game's own terms, and a single-game mod keeps its flat tree. Sets
 * mod->assets_dir and mod->has_assets. */
static void mod_select_assets(struct openpete_mod* mod) {
    struct stat st;
    snprintf(mod->assets_dir, sizeof mod->assets_dir, "%s/assets/%s",
             mod->dir, game_get_id());
    if (stat(mod->assets_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        mod->has_assets = 1;
        return;
    }
    snprintf(mod->assets_dir, sizeof mod->assets_dir, "%s/assets", mod->dir);
    mod->has_assets = stat(mod->assets_dir, &st) == 0 && S_ISDIR(st.st_mode);
}

/** @brief Register a mod's [[level]] rows with the content registry from its asset directory.
 *
 * On a game whose descriptor serves no level ops every row is refused with
 * one line naming the game, and the rest of the mod loads. */
static void mod_register_levels(struct openpete_mod* mod) {
    const int n = mod_level_count(mod);
    if (n > 0 && !openpete_mods_game_desc()->level_ops) {
        OPL_WARN_F("mods", "%s: %d [[level]] row(s) refused - %s serves no "
                   "level ops", mod->id, n, game_get_id());
        return;
    }
    for (int i = 0; i < n; i++) {
        const mod_level_row_t* L = mod_level_at(mod, i);
        openpete_level_counts_t counts = {
            L->dragons,
            L->gems,
            L->eggs,
        };
        (void)openpete_mods_custom_register(mod->id,
                                            L->name,
                                            L->files,
                                            mod->assets_dir,
                                            L->donor,
                                            L->replace,
                                            L->music,
                                            L->title,
                                            &counts,
                                            mod->log_tag);
    }
}

/** @brief Load one parsed mod: register its custom levels, obtain and dlopen its object, and optionally enter it.
 *
 * The object comes from build_mod when the mod has src/ and the SDK is
 * present, else from the prebuilt object mod_find_prebuilt finds, loaded
 * as-is. A mod
 * with neither src/ nor an object is declarative: its manifest rows and
 * files are the whole mod. After dlopen the abi descriptor, the game
 * identity (mod_check_game) and the ImGui descriptor are checked and
 * config.toml is loaded.
 *
 * run_entry 0 is the deferred-entry load (the load-all path): no mod code
 * runs and no material or asset root registers, so a boot-disabled mod is
 * inert until its first enable runs mod_enter_deferred.
 * @return 0, or -1 when the mod is disabled. */
static int load_mod(struct openpete_mod* mod, const char* sdk_dir,
                    int run_entry) {
    char so_path[MOD_PATH_MAX];
    {
        char src_dir[MOD_PATH_MAX + 8];
        snprintf(src_dir, sizeof src_dir, "%s/src", mod->dir);
        struct stat st;
        mod->has_src = (stat(src_dir, &st) == 0 && S_ISDIR(st.st_mode));
    }
    mod_select_assets(mod);
    mod_register_levels(mod);
    if (!mod->has_src) {
        const int kind = mod_find_prebuilt(mod->dir, mod->id, so_path,
                                           sizeof so_path);
        mod->obj_kind = kind;
        if (kind < 0) {
            /* Declarative mods are legal: no src/ and no prebuilt
              * object means the manifest and its files are the whole
              * mod. */
            if (!run_entry) {
                OPL_INFO_F("mods", "loaded %s v%s (declarative; disabled)",
                           mod->id, mod->version);
                return 0;
            }
            OPL_INFO_F("mods", "loaded %s v%s (declarative)", mod->id,
                       mod->version);
            mod->entered = 1;
            mod_order_note(mod, OPENPETE_MODSET_EV_ENTRY, 0, 0);
            mod_register_materials(mod);   /* a manifest-only mod IS its rows */
            mod_register_assets(mod);
            return 0;
        }
        /* Binary-only mod: the prebuilt object IS the mod - loaded
         * as-is, never hashed or compiled, skipped by hot reload. */
        OPL_INFO_F("mods", "%s: binary-only mod (no src/), loading %s "
                   "as-is", mod->id, so_path);
    } else if (!mods_sdk_present(sdk_dir)) {
        mods_sdk_missing_notice(sdk_dir);
        const int kind = mod_find_prebuilt(mod->dir, mod->id, so_path,
                                           sizeof so_path);
        mod->obj_kind = kind;
        if (kind < 0) {
            OPL_ERROR_F("mods", "%s: has src/ but no SDK to compile it "
                        "against and no prebuilt %s - disabled",
                        mod->id, so_path);
            return -1;
        }
        OPL_INFO_F("mods", "%s: no SDK, loading prebuilt %s as-is",
                   mod->id, so_path);
    } else {
        if (build_mod(mod, sdk_dir, so_path, sizeof so_path) != 0)
            return -1;
    }

    mod->dl = mod_dlopen(so_path);
    if (!mod->dl) {
        OPL_ERROR_F("mods", "%s: dlopen failed: %s - disabled", mod->id,
                    mod_dlerror());
        return -1;
    }
    snprintf(mod->obj_path, sizeof mod->obj_path, "%s", so_path);
    if (mod_check_abi(mod->dl, mod->id, so_path) != 0 ||
        mod_check_game(mod, mod->dl, so_path, mod->obj_kind) != 0 ||
        mod_resolve_imgui(mod, mod->dl, so_path) != 0) {
        mod->dl = NULL; /* the object stays mapped; nothing in it runs */
        return -1;
    }
    openpete_mods_config_load(mod, mod->dir, mod->log_tag);
    (void)config_compute_key(mod->dir, mod->cfg_key);
    (void)openpete_mods_shader_watch_key(mod->dir, mod->shader_key);
    if (!run_entry) {
        OPL_INFO_F("mods", "loaded %s v%s (%s; entry deferred until "
                   "enabled)", mod->id, mod->version, so_path);
        return 0;
    }
    if (mod_run_entry(mod) != 0) return -1;
    OPL_INFO_F("mods", "loaded %s v%s (%s)", mod->id, mod->version,
               so_path);
    mod_register_assets(mod);
    return 0;
}

int openpete_mods_take_entered(const char** out, int cap) {
    int n = 0;
    for (int i = 0; i < mod_count() && n < cap; i++) {
        struct openpete_mod* m = mod_at(i);
        if (atomic_exchange(&m->late_entered, 0)) out[n++] = m->id;
    }
    return n;
}

/** @brief Enter a deferred-entry mod on its first enable, at a tick boundary.
 *
 * Enters its dependencies first, recursively (a library's entry must have
 * run before its dependent's; a dependency's enabled state is not
 * changed), then runs entry, or for an object-less mod marks it entered
 * and arms its [[material]] rows, then registers the assets/ root and
 * invalidates the modset tag. Boot-scanned asset consumers (texture pack)
 * see the root only after a restart; per-play consumers (music) apply
 * immediately. Late entry registers at the chain tail, the highest
 * priority; a restart slots the mod into enabled.txt order. Each mod it
 * enters gets its late_entered flag set, which the renderer's next compose
 * takes (openpete_mods_take_entered). A no-op for a mod already entered.
 * @return 0, or -1 when a dependency is not loaded or an entry fails. */
static int mod_enter_deferred(struct openpete_mod* mod) {
    if (mod->entered) return 0;
    for (int d = 0; d < mod_dep_count(mod); d++) {
        const char* dep_id = mod_dep_at(mod, d)->id;
        struct openpete_mod* dep = NULL;
        for (int i = 0; i < mod_count(); i++)
            if (strcmp(mod_at(i)->id, dep_id) == 0) {
                dep = mod_at(i);
                break;
            }
        if (!dep) {
            OPL_ERROR_F("mods", "%s: dependency \"%s\" is not loaded",
                        mod->id, dep_id);
            return -1;
        }
        if (mod_enter_deferred(dep) != 0) return -1;
    }
    if (mod->dl) {
        if (mod_run_entry(mod) != 0) return -1;
    } else {
        mod->entered = 1; /* assets-only: the overlay root IS the mod */
        mod_order_note(mod, OPENPETE_MODSET_EV_ENTRY, 0, 0);
        mod_register_materials(mod);
    }
    mod_register_assets(mod);
    s_tag_built = 0; /* the modset tags now include this mod if divergent */
    atomic_store(&mod->late_entered, 1);
    return 0;
}

/* ---- enabled-list resolution ---------------------------------------- */

/* Dependency topo-sort. DFS, stable in enabled order: a dep
 * loads before its dependents, so dependents keep HIGHER chain priority
 * than their libraries, and a library's entry has run by the time its
 * client's does. Missing dep / cycle = actionable error, dependent
 * disabled, run continues. */
enum { TS_WHITE = 0, TS_GRAY, TS_BLACK, TS_FAILED };

typedef struct {
    const mod_fvec*      pending;    /* struct openpete_mod rows */
    int                  n_pending;
    mod_fvec             state;      /* int per pending row */
    mod_fvec             topo;       /* int, the visit order */
    int                  n_topo;
    const char*          mods_dir;
} topo_ctx_t;

/** @brief Find the pending row whose id equals a dependency id.
 *
 * @return The row index, or -1 when no pending mod has the id. */
static int topo_find(const topo_ctx_t* c, const char* id) {
    for (int j = 0; j < c->n_pending; j++)
        if (strcmp(pend_at(c->pending, j)->id, id) == 0) return j;
    return -1;
}

/** @brief Depth-first visit of pending mod i that appends it to the load order after its dependencies.
 *
 * Colours rows in c->state: a row met while GRAY closes a cycle, a
 * dependency id absent from the pending set is a missing dependency, and
 * a failed dependency fails its dependents; each marks the row TS_FAILED
 * and logs one error naming the cause. A row that finishes is marked
 * TS_BLACK and its index is pushed onto c->topo, so dependencies precede
 * dependents in enabled order. A failed topo push returns -1 with the row
 * already BLACK.
 * @param c Sort context.
 * @param i Pending row index.
 * @return 0 when the row is loadable (or already visited), -1 when it
 *         failed. */
static int topo_visit(topo_ctx_t* c, int i) { /* 0 = ok, -1 = failed */
    if (int_row(&c->state, i) == TS_BLACK)  return 0;
    if (int_row(&c->state, i) == TS_FAILED) return -1;
    if (int_row(&c->state, i) == TS_GRAY) {
        OPL_ERROR_F("mods", "dependency cycle through \"%s\" - disabled "
                    "(break the cycle in the mods' mod.toml deps)",
                    pend_at(c->pending, i)->id);
        int_row_set(&c->state, i, TS_FAILED);
        return -1;
    }
    int_row_set(&c->state, i, TS_GRAY);
    for (int d = 0; d < mod_dep_count(pend_at(c->pending, i)); d++) {
        const char* dep_id = mod_dep_at(pend_at(c->pending, i), d)->id;
        int j = topo_find(c, dep_id);
        if (j < 0) {
            char where[MOD_PATH_MAX + 16];
            if (strcmp(s_order_src, "enabled.txt") == 0)
                snprintf(where, sizeof where, "%s/enabled.txt", c->mods_dir);
            else
                snprintf(where, sizeof where, "the --mods list");
            OPL_ERROR_F("mods", "\"%s\" requires \"%s\", which is not "
                        "enabled - add it to %s (ahead of "
                        "\"%s\") or install it; \"%s\" disabled",
                        pend_at(c->pending, i)->id, dep_id,
                        where, pend_at(c->pending, i)->id, pend_at(c->pending, i)->id);
            int_row_set(&c->state, i, TS_FAILED);
            return -1;
        }
        if (topo_visit(c, j) != 0) {
            if (int_row(&c->state, i) != TS_FAILED) {
                OPL_ERROR_F("mods", "\"%s\" disabled: its dependency "
                            "\"%s\" failed", pend_at(c->pending, i)->id,
                            dep_id);
                int_row_set(&c->state, i, TS_FAILED);
            }
            return -1;
        }
    }
    if (int_row(&c->state, i) == TS_FAILED) return -1; /* cycle marked us */
    int_row_set(&c->state, i, TS_BLACK);
    {
        int* slot = (int*)mod_fvec_push(&c->topo);
        if (!slot) return -1;
        *slot = i;
        c->n_topo++;
    }
    return 0;
}

/** @brief Refuse each pending mod a required game of which has no disc in the library.
 *
 * Marks the row TS_FAILED before the topo sort, so it never loads and a
 * mod depending on it is disabled as for a failed dependency. Each refusal
 * logs one error and adds a Mods panel row naming the game.
 * @param c Sort context, its state sized to the pending rows. */
static void mods_refuse_missing_games(topo_ctx_t* c) {
    for (int i = 0; i < c->n_pending; i++) {
        const struct openpete_mod* m = pend_at(c->pending, i);
        const char* g = mod_games_first_missing(&m->games, game_get_id());
        if (!g) continue;
        char why[160];
        if (openpete_game_registered(g))
            snprintf(why, sizeof why, "requires %s, which has no disc in the "
                     "library - add its disc to load this mod", g);
        else
            snprintf(why, sizeof why, "requires %s, which is no game this "
                     "build knows", g);
        OPL_ERROR_F("mods", "\"%s\" %s; \"%s\" disabled", m->id, why, m->id);
        mod_games_refused_add(m->id, why);
        int_row_set(&c->state, i, TS_FAILED);
    }
}

/** @brief Remove a directory tree (or a file) at path, depth first, removing each link itself without following it.
 *
 * A link (a symbolic link, or a junction on Windows; openpete_paths_is_link)
 * is removed as an entry and never entered, so a link in the tree to a
 * directory outside it leaves that directory's contents alone. Entries that
 * cannot be removed (a DLL the process holds on Windows) are left, and the
 * removal of their parents then fails.
 * @param path The tree's root.
 * @return 0 when nothing remains at path, -1 otherwise. */
static int mod_rmtree(const char* path) {
    if (openpete_paths_is_link(path)) {
        if (remove(path) == 0) return 0;
#ifdef _WIN32
        return _rmdir(path) == 0 ? 0 : -1; /* a directory link or junction */
#else
        return -1;
#endif
    }
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0 ? 0 : -1;
    DIR* d = opendir(path);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char child[MOD_PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            (void)mod_rmtree(child);
        }
        closedir(d);
    }
#ifdef _WIN32
    return _rmdir(path) == 0 ? 0 : -1;
#else
    return rmdir(path) == 0 ? 0 : -1;
#endif
}

/** @brief Find the single top-level directory that wraps every entry of a zip with no root-level mod.toml.
 *
 * @param zip The open archive.
 * @param wrap Receives the wrapper with its trailing '/', or "".
 * @param cap Capacity of wrap.
 * @return The wrapper's length, 0 when the archive has none. */
static size_t zip_wrapper_prefix(mz_zip_archive* zip, char* wrap, size_t cap) {
    size_t wrap_len = 0;
    int root_toml = 0, single = 1;
    wrap[0] = '\0';
    mz_uint n = mz_zip_reader_get_num_files(zip);
    for (mz_uint i = 0; i < n && single; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, i, &st)) { single = 0; break; }
        if (strcmp(st.m_filename, "mod.toml") == 0) root_toml = 1;
        const char* slash = strchr(st.m_filename, '/');
        if (!slash) { single = 0; break; } /* root-level file */
        size_t len = (size_t)(slash - st.m_filename) + 1;
        if (wrap_len == 0) {
            if (len >= cap) { single = 0; break; }
            memcpy(wrap, st.m_filename, len);
            wrap[len] = '\0';
            wrap_len = len;
        } else if (len != wrap_len || memcmp(wrap, st.m_filename, len) != 0) {
            single = 0;
        }
    }
    return (root_toml || !single) ? 0 : wrap_len;
}

/** @brief Extract every entry of a zip into dest, with the wrapper prefix stripped from each path.
 *
 * An entry path that is absolute or contains ".." aborts the extraction.
 * Intermediate directories are made as needed (zips may omit them).
 * @param zip The open archive.
 * @param dest Destination directory, existing.
 * @param wrap_len Length of the wrapper prefix to strip, 0 for none.
 * @param id Mod id, for log lines.
 * @param files Receives the number of files written.
 * @return 1 when every entry extracted, 0 otherwise. */
static int zip_extract_all(mz_zip_archive* zip, const char* dest, size_t wrap_len,
                           const char* id, int* files) {
    mz_uint n = mz_zip_reader_get_num_files(zip);
    *files = 0;
    for (mz_uint i = 0; i < n; i++) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(zip, i, &st)) return 0;
        if (st.m_filename[0] == '/' || strstr(st.m_filename, "..")) {
            OPL_ERROR_F("mods", "%s.zip: unsafe entry path \"%s\" - "
                        "skipped archive", id, st.m_filename);
            return 0;
        }
        const char* rel = st.m_filename + wrap_len;
        if (!*rel) continue; /* the wrapper directory entry itself */
        char out[MOD_PATH_MAX * 2];
        snprintf(out, sizeof out, "%s/%s", dest, rel);
        if (mz_zip_reader_is_file_a_directory(zip, i)) {
            mod_mkdir(out);
            continue;
        }
        for (char* p = out + strlen(dest) + 1; *p; p++) {
            if (*p != '/') continue;
            *p = '\0';
            mod_mkdir(out);
            *p = '/';
        }
        if (!mz_zip_reader_extract_to_file(zip, i, out, 0)) {
            OPL_ERROR_F("mods", "%s.zip: extract failed for \"%s\"", id,
                        st.m_filename);
            return 0;
        }
        (*files)++;
    }
    return 1;
}

/** @brief SHA-256 of a file as hex.
 *
 * @param path The file.
 * @param hex Receives the digest.
 * @return 0, or -1 when the file does not open. */
static int file_sha_hex(const char* path, char hex[65]) {
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    if (hash_file(&hc, path) != 0) return -1;
    uint8_t digest[32];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, hex);
    return 0;
}

/* The player's state in an unpacked zip mod's tree: the data directory
 * api->data_dir returns (`<mod dir>/data`) and the config.toml the Mods
 * tab writes. A changed zip replaces the tree; these two move into the new
 * tree first, so data_dir keeps its path and its contents. */
#define ZIP_KEEP_DATA "data"
#define ZIP_KEEP_ZIPDATA ".zip-data"

/** @brief Move every entry of the zip's own data directory that the player's lacks into the player's, then remove the rest.
 *
 * @param staging The staged tree, holding the player's data/ and the zip's under ZIP_KEEP_ZIPDATA. */
static void zip_merge_data(const char* staging) {
    char zd[MOD_PATH_MAX + 16], ud[MOD_PATH_MAX + 16];
    snprintf(zd, sizeof zd, "%s/" ZIP_KEEP_ZIPDATA, staging);
    snprintf(ud, sizeof ud, "%s/" ZIP_KEEP_DATA, staging);
    DIR* d = opendir(zd);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d)) != NULL) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char from[MOD_PATH_MAX * 2], to[MOD_PATH_MAX * 2];
            snprintf(from, sizeof from, "%s/%s", zd, e->d_name);
            snprintf(to, sizeof to, "%s/%s", ud, e->d_name);
            if (file_mtime(to) == (time_t)-1) (void)rename(from, to);
        }
        closedir(d);
    }
    (void)mod_rmtree(zd);
}

/** @brief Move the player's data directory and edited config.toml from the unpacked tree into the staged one.
 *
 * data/ moves whole (one rename); entries the zip ships under data/ that
 * the player's lacks join it. config.toml is copied over the zip's when the
 * player changed it: when its hash differs from the shipped copy's the
 * stamp recorded, or the stamp records none (an older stamp).
 * @param dest The unpacked tree.
 * @param staging The staged tree.
 * @param shipped_cfg The stamp's hash of the config.toml the previous zip shipped, "-" for none, "" when unrecorded.
 * @param id The mod id, for log lines.
 * @return 1 when data/ moved (zip_return_data puts it back if the swap fails), 0 when there was none, -1 when it could not move. */
static int zip_carry_player_state(const char* dest, const char* staging, const char* shipped_cfg,
                                  const char* id) {
    char ucfg[MOD_PATH_MAX + 16], scfg[MOD_PATH_MAX + 16], hex[65];
    snprintf(ucfg, sizeof ucfg, "%s/config.toml", dest);
    snprintf(scfg, sizeof scfg, "%s/config.toml", staging);
    if (file_sha_hex(ucfg, hex) == 0 && (!shipped_cfg[0] || strcmp(hex, shipped_cfg) != 0)) {
        if (op_atomic_copy_file(ucfg, scfg) != 0) {
            OPL_ERROR_F("mods", "%s.zip: cannot carry the player's config.toml into the new tree", id);
            return -1;
        }
        OPL_INFO_F("mods", "%s.zip: kept the player's config.toml", id);
    }
    char udata[MOD_PATH_MAX + 16], sdata[MOD_PATH_MAX + 16], zdata[MOD_PATH_MAX + 16];
    snprintf(udata, sizeof udata, "%s/" ZIP_KEEP_DATA, dest);
    snprintf(sdata, sizeof sdata, "%s/" ZIP_KEEP_DATA, staging);
    snprintf(zdata, sizeof zdata, "%s/" ZIP_KEEP_ZIPDATA, staging);
    if (file_mtime(udata) == (time_t)-1) return 0;
    const int zip_has = file_mtime(sdata) != (time_t)-1;
    if (zip_has && rename(sdata, zdata) != 0) return -1;
    if (rename(udata, sdata) != 0) {
        OPL_ERROR_F("mods", "%s.zip: cannot move the player's data directory %s: %s", id, udata,
                    strerror(errno));
        if (zip_has) (void)rename(zdata, sdata);
        return -1;
    }
    if (zip_has) zip_merge_data(staging);
    OPL_INFO_F("mods", "%s.zip: kept the player's data directory", id);
    return 1;
}

/** @brief Put the player's data directory back into the unpacked tree after a failed swap.
 *
 * @param dest The unpacked tree, still the previous one.
 * @param staging The staged tree holding the player's data/. */
static void zip_return_data(const char* dest, const char* staging) {
    char udata[MOD_PATH_MAX + 16], sdata[MOD_PATH_MAX + 16];
    snprintf(udata, sizeof udata, "%s/" ZIP_KEEP_DATA, dest);
    snprintf(sdata, sizeof sdata, "%s/" ZIP_KEEP_DATA, staging);
    if (rename(sdata, udata) != 0)
        OPL_ERROR_F("mods", "cannot return the player's data directory to %s: %s (it is at %s)", udata,
                    strerror(errno), sdata);
}

/** @brief Unpack `<mods_dir>/<id>.zip` to `<mods_dir>/.unpacked/<id>/` so a zipped mod loads like a folder.
 *
 * The zip's SHA-256 is stamped at `.unpacked/<id>.hash`, so an unchanged
 * zip whose mod.toml is present is not re-extracted. mod.toml sits at the
 * zip root, or inside a single top-level directory that wraps every
 * entry (the layout zipping the folder produces); that wrapper is
 * stripped on extraction. A changed zip is extracted into
 * `.unpacked/<id>.new/`, which then replaces the previous tree whole by
 * renaming it aside (op_atomic_replace_dir), so a file the new version
 * dropped (a source, a texture, a shader) does not survive, and a failed
 * extraction or swap leaves the previous tree in place. The player's
 * data/ and an edited config.toml move into the new tree before the swap
 * (zip_carry_player_state), so they survive it at the same paths. The
 * stamp's second line records the hash of the config.toml the zip ships
 * (`config -` for none), which tells a player's edit from the shipped copy.
 * An entry path that is absolute or contains ".." aborts the archive.
 * @return 0 when unpacked or already current; -1 when the zip is missing
 *         or unreadable, an entry is unsafe or fails to extract, no
 *         mod.toml results, or the previous tree cannot be replaced. */
static int mod_unpack_zip(const char* mods_dir, const char* id) {
    char zip_path[MOD_PATH_MAX];
    snprintf(zip_path, sizeof zip_path, "%s/%s.zip", mods_dir, id);

    /* Content key. */
    openpete_sha256_ctx hc;
    openpete_sha256_init(&hc);
    if (hash_file(&hc, zip_path) != 0) return -1;
    uint8_t digest[32];
    char key[65];
    openpete_sha256_final(&hc, digest);
    openpete_sha256_hex(digest, key);

    char root[MOD_PATH_MAX], dest[MOD_PATH_MAX], stamp[MOD_PATH_MAX + 8];
    snprintf(root, sizeof root, "%s/.unpacked", mods_dir);
    snprintf(dest, sizeof dest, "%s/.unpacked/%s", mods_dir, id);
    snprintf(stamp, sizeof stamp, "%s.hash", dest);

    char stamped[65] = "", shipped_cfg[65] = "";
    FILE* sf = fopen(stamp, "rb");
    if (sf) {
        char text[160] = "";
        const size_t got = fread(text, 1, sizeof text - 1, sf);
        text[got] = '\0';
        fclose(sf);
        char* nl = strchr(text, '\n');
        if (nl) *nl = '\0';
        snprintf(stamped, sizeof stamped, "%.64s", text);
        if (nl && sscanf(nl + 1, "config %64s", shipped_cfg) != 1) shipped_cfg[0] = '\0';
    }
    char probe[MOD_PATH_MAX + 12];
    snprintf(probe, sizeof probe, "%s/mod.toml", dest);
    if (strcmp(stamped, key) == 0 && file_mtime(probe) != (time_t)-1)
        return 0; /* current */

    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_file(&zip, zip_path, 0)) {
        OPL_ERROR_F("mods", "%s.zip: not a readable zip - skipped", id);
        return -1;
    }
    char wrap[MOD_PATH_MAX];
    size_t wrap_len = zip_wrapper_prefix(&zip, wrap, sizeof wrap);
    if (wrap_len)
        OPL_INFO_F("mods", "%s.zip: stripping wrapper directory \"%s\"", id, wrap);

    char staging[MOD_PATH_MAX + 8], staged_toml[MOD_PATH_MAX + 20];
    snprintf(staging, sizeof staging, "%s.new", dest);
    snprintf(staged_toml, sizeof staged_toml, "%s/mod.toml", staging);
    mod_mkdir(root);
    (void)mod_rmtree(staging);
    mod_mkdir(staging);
    int files = 0;
    int ok = zip_extract_all(&zip, staging, wrap_len, id, &files);
    mz_zip_reader_end(&zip);
    if (ok && file_mtime(staged_toml) == (time_t)-1) {
        OPL_ERROR_F("mods", "%s.zip: no mod.toml at the archive root "
                    "(or inside a single wrapper directory) - skipped", id);
        ok = 0;
    }
    if (!ok) {
        (void)mod_rmtree(staging);
        return -1;
    }
    char new_cfg[65] = "-", staged_cfg[MOD_PATH_MAX + 24];
    snprintf(staged_cfg, sizeof staged_cfg, "%s/config.toml", staging);
    if (file_sha_hex(staged_cfg, new_cfg) != 0) snprintf(new_cfg, sizeof new_cfg, "-");
    const int moved = zip_carry_player_state(dest, staging, shipped_cfg, id);
    if (moved < 0) {
        OPL_ERROR_F("mods", "%s.zip: the player's data could not move to the new version - previous "
                    "tree kept, skipped", id);
        (void)mod_rmtree(staging);
        return -1;
    }
    if (op_atomic_replace_dir(staging, dest, mod_rmtree) != 0) {
        OPL_ERROR_F("mods", "%s.zip: cannot replace %s (a file in it is in "
                    "use?) - skipped", id, dest);
        if (moved) zip_return_data(dest, staging);
        (void)mod_rmtree(staging);
        return -1;
    }
    char line[160];
    int ln = snprintf(line, sizeof line, "%s\nconfig %s\n", key, new_cfg);
    (void)op_atomic_write_file(stamp, line, (size_t)ln);
    OPL_INFO_F("mods", "%s: unpacked %d file%s from %s.zip", id, files,
               files == 1 ? "" : "s", id);
    return 0;
}

/* ---- discovery (shared by prepare + init) ---------------------------- */

typedef struct {
    char id[MOD_ID_MAX];
    char dir[MOD_PATH_MAX]; /* folder, or .unpacked/<id> for zip mods */
    int  boot_on;           /* in the enabled set (vs a deferred-entry
                             * load-all row) */
} mod_ref_t;

/** @brief Return row i of a mod_ref_t vec, or NULL at or past the count.
 *
 * Callers index below their own count and dereference without a check. */
static mod_ref_t* ref_at(const mod_fvec* v, int i) {
    return (mod_ref_t*)mod_fvec_at(v, (size_t)i);
}

/* The mods root the last discovery resolved, for openpete_mods_root_dir;
 * empty until then. */
static char s_mods_root_dir[MOD_PATH_MAX];

const char* openpete_mods_root_dir(void) {
    return s_mods_root_dir[0] ? s_mods_root_dir : NULL;
}

/** @brief Resolve the mods root directory.
 *
 * Order: opts->mods_dir, env OPENPETE_MODS_DIR, then the writable data
 * location openpete_paths_data_dir resolves for "mods". Mods are compiled
 * and unpacked in place (.build/, .unpacked/), so the root must be
 * writable: openpete_paths puts it next to the exe in a portable install
 * and under the per-user data directory otherwise, which is what a
 * read-only install medium leaves.
 * @param opts Parsed options, or NULL.
 * @param mods_dir Output buffer.
 * @param mods_dir_cap Size of @p mods_dir.
 * @return 0 with @p mods_dir filled, or -1 when no base path resolves. */
static int mods_root(const openpete_options_t* opts, char* mods_dir,
                     size_t mods_dir_cap) {
    const char* src = opts && opts->mods_dir ? opts->mods_dir
                                             : getenv("OPENPETE_MODS_DIR");
    if (src) {
        snprintf(mods_dir, mods_dir_cap, "%s", src);
        return 0;
    }
    return openpete_paths_data_dir("mods", mods_dir, mods_dir_cap, 0);
}

int openpete_mods_open_folder(const openpete_options_t* opts) {
    char dir[MOD_PATH_MAX];
    if (opts) {
        if (mods_root(opts, dir, sizeof dir) != 0) return -1;
    } else if (s_mods_root_dir[0]) {
        snprintf(dir, sizeof dir, "%s", s_mods_root_dir);
    } else {
        return -1;
    }
    (void)SDL_CreateDirectory(dir);   /* fails harmlessly when it exists */
    char url[MOD_PATH_MAX + 8];
#if defined(__APPLE__)
    snprintf(url, sizeof url, "file://%s", dir);
#else
    snprintf(url, sizeof url, "%s", dir);
#endif
    const char* stub = getenv("OPENPETE_LAUNCHER_OPEN_URL_STUB");
    if (stub && *stub && *stub != '0') {
        OPL_INFO_F("mods", "mods folder: opened %s (stubbed: nothing opened)", dir);
        return 0;
    }
    if (!SDL_OpenURL(url)) {
        OPL_WARN_F("mods", "mods folder: could not open %s: %s", dir, SDL_GetError());
        return -1;
    }
    OPL_INFO_F("mods", "mods folder: opened %s", dir);
    return 0;
}

/** @brief Fill a mod ref with a candidate id and its mod dir.
 *
 * The dir is `<mods_dir>/<id>` when that folder holds a mod.toml, else the
 * zip's `<mods_dir>/.unpacked/<id>`. */
static void mod_ref_resolve(const char* mods_dir, const char* id,
                            mod_ref_t* r) {
    snprintf(r->id, sizeof r->id, "%s", id);
    snprintf(r->dir, sizeof r->dir, "%s/%s", mods_dir, id);
    char probe[MOD_PATH_MAX + 12];
    snprintf(probe, sizeof probe, "%s/mod.toml", r->dir);
    if (file_mtime(probe) == (time_t)-1) /* zip-unpacked mod */
        snprintf(r->dir, sizeof r->dir, "%s/.unpacked/%s", mods_dir, id);
}

/** @brief Append one declared game id to the mismatch text, comma-separated.
 *
 * Saturates at the buffer instead of overrunning it: once full, the
 * length stays at cap - 1.
 * @param want The text buffer.
 * @param cap Size of @p want.
 * @param used Current length of the text.
 * @param id The id to append.
 * @return The new length. */
static size_t want_append(char* want, size_t cap, size_t used,
                          const char* id) {
    if (used + 1 >= cap) return used;
    int wrote = snprintf(want + used, cap - used, "%s%s",
                         used ? ", " : "", id);
    if (wrote < 0) return used;
    used += (size_t)wrote;
    return used >= cap ? cap - 1 : used;
}

/** @brief Test whether a [mod] game array names @p game, collecting the declared ids into @p want.
 *
 * An empty array, or one holding no strings, names no game: nothing
 * matches, which is the same narrowing direction as a misspelling. On a
 * match @p want is cleared.
 * @param ga The game array.
 * @param game The game id.
 * @param want Receives the declared ids, comma-separated.
 * @param want_cap Size of @p want.
 * @return 1 when an element equals @p game, else 0. */
static int game_array_names(toml_array_t* ga, const char* game, char* want,
                            size_t want_cap) {
    const int n = toml_array_nelem(ga);
    size_t used = 0;
    int match = 0;
    for (int i = 0; i < n; i++) {
        toml_datum_t d = toml_string_at(ga, i);
        if (!d.ok) continue;
        if (strcmp(d.u.s, game) == 0) match = 1;
        used = want_append(want, want_cap, used, d.u.s);
        free(d.u.s);
    }
    if (match) want[0] = 0;
    return match;
}

/** @brief Apply the game selection rule of a manifest's [mod] table to @p game.
 *
 * The game key is one id or an array of them, matched exactly. An absent
 * key selects every game when the table pins api MOD_API_SCOPED or later,
 * and spyro-1 alone otherwise (pin below it or absent), since such a mod
 * was written against the Spyro 1 SDK; *implied is then set. A value
 * naming no game the exe knows matches nothing, so a misspelling narrows a
 * mod's reach instead of widening it. On a mismatch @p want receives the
 * selected ids, comma-separated, so the log can name them.
 * @param t The [mod] table.
 * @param game The game id.
 * @param want Receives the selected ids on a mismatch; the caller clears it.
 * @param want_cap Size of @p want.
 * @param implied Set to 1 when the legacy rule selected spyro-1.
 * @return 1 when the table selects @p game, 0 otherwise. */
static int mod_table_selects_game(toml_table_t* t, const char* game,
                                  char* want, size_t want_cap, int* implied) {
    toml_array_t* ga = toml_array_in(t, "game");
    if (ga) return game_array_names(ga, game, want, want_cap);
    toml_datum_t g = toml_string_in(t, "game");
    const char* sel = NULL;
    if (!g.ok) {
        toml_datum_t api = toml_int_in(t, "api");
        if (api.ok && api.u.i >= MOD_API_SCOPED) return 1;
        *implied = 1;
        sel = "spyro-1";
    }
    const int match = strcmp(g.ok ? g.u.s : sel, game) == 0;
    if (!match) (void)want_append(want, want_cap, 0, g.ok ? g.u.s : sel);
    if (g.ok) free(g.u.s);
    return match;
}

/** @brief Test whether a mod's manifest selects the game mods are built for.
 *
 * Read here rather than in parse_manifest because discovery feeds the
 * pre-boot compile pass as well as the loader: a mod for another title
 * has to leave the candidate set before its C is handed to this game's
 * SDK. Applies mod_table_selects_game to the [mod] table with
 * mods_build_game(). A manifest that will not open or parse, or has no
 * [mod] table, stays a candidate; parse_manifest reports that case.
 * @param mod_dir The mod directory.
 * @param want Receives the selected ids on a mismatch; the caller clears it.
 * @param want_cap Size of @p want.
 * @param implied Set to 1 when the legacy rule selected spyro-1.
 * @return 1 when the mod targets this game, 0 otherwise. */
static int mod_targets_this_game(const char* mod_dir, char* want,
                                 size_t want_cap, int* implied) {
    char path[MOD_PATH_MAX + 12];
    snprintf(path, sizeof path, "%s/mod.toml", mod_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) return 1;
    char errbuf[256];
    toml_table_t* root = toml_parse_file(fp, errbuf, sizeof errbuf);
    fclose(fp);
    if (!root) return 1;
    toml_table_t* t = toml_table_in(root, "mod");
    const int match = !t || mod_table_selects_game(t, mods_build_game(),
                                                   want, want_cap, implied);
    toml_free(root);
    return match;
}

/** @brief Keep or drop a discovery candidate by its manifest's game key.
 *
 * Resolves the candidate's dir (folder or .unpacked zip) and asks
 * mod_targets_this_game. A mod for another game is dropped with an info
 * line naming the ids it declares; each scan logs it again.
 * @param mods_dir The mods root.
 * @param id The candidate id.
 * @return 1 to keep the candidate, 0 to drop it. */
static int scan_keep_for_game(const char* mods_dir, const char* id) {
    mod_ref_t r;
    mod_ref_resolve(mods_dir, id, &r);
    char want[96];
    int implied = 0;
    want[0] = 0;
    if (mod_targets_this_game(r.dir, want, sizeof want, &implied)) return 1;
    char shown[160];
    if (implied)
        snprintf(shown, sizeof shown, "game \"%s\" (no [mod] game key, api "
                 "pin below %d or absent)", want, MOD_API_SCOPED);
    else if (want[0]) snprintf(shown, sizeof shown, "game \"%s\"", want);
    else              snprintf(shown, sizeof shown, "no game");
    OPL_INFO_F("mods", "%s targets %s - skipped (this is %s)",
               id, shown, mods_build_game());
    return 0;
}

/** @brief Scan the mods root for candidate ids and append one mod_id_row_t per candidate to @p ids.
 *
 * A candidate is a subdirectory holding mod.toml, or an <id>.zip archive
 * that mod_unpack_zip extracts to .unpacked/<id>/ (content-hash stamped,
 * so it re-extracts only when the zip changes). Names starting with '.'
 * are skipped. A folder and a zip with the same id each append a row, and
 * mod_ref_resolve resolves the id to the folder. A mod whose manifest
 * names another game is dropped by scan_keep_for_game. Rows are in
 * readdir order.
 * @param mods_dir The mods root.
 * @param ids Row vector the ids are appended to.
 * @return The row count of @p ids, or 0 when the directory does not
 *         open. */
static int mods_scan(const char* mods_dir, mod_fvec* ids) {
    DIR* d = opendir(mods_dir);
    if (!d) return 0; /* no mods directory: inert */

    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        char id[MOD_ID_MAX];
        mod_id_row_t* row;
        if (e->d_name[0] == '.') continue;
        size_t nl = strlen(e->d_name);
        if (nl > 4 && strcmp(e->d_name + nl - 4, ".zip") == 0) {
            snprintf(id, sizeof id, "%.*s", (int)(nl - 4), e->d_name);
            if (mod_unpack_zip(mods_dir, id) != 0 ||
                !scan_keep_for_game(mods_dir, id))
                continue;
        } else {
            char manifest[MOD_PATH_MAX];
            snprintf(manifest, sizeof manifest, "%s/%s/mod.toml", mods_dir,
                     e->d_name);
            if (file_mtime(manifest) == (time_t)-1) continue;
            if (!scan_keep_for_game(mods_dir, e->d_name)) continue;
            snprintf(id, sizeof id, "%s", e->d_name);
        }
        row = (mod_id_row_t*)mod_fvec_push(ids);
        if (!row) {
            OPL_ERROR_F("mods", "mods scan: id list allocation failed - "
                        "\"%s\" skipped", id);
            continue;
        }
        snprintf(row->id, sizeof row->id, "%s", id);
    }
    closedir(d);
    return (int)mod_fvec_count(ids);
}

/// @brief Compare two mod_id_row_t rows by id text, the qsort order for id lists.
static int cmp_id_row(const void* a, const void* b) {
    return strcmp((const char*)a, (const char*)b);
}

/** @brief Trim leading and trailing spaces and tabs from an id in place.
 *
 * Ids compare exactly, so an untrimmed "crt-beans " would be skipped as
 * unknown.
 * @param s The id text; trailing blanks are overwritten with NULs.
 * @return The first non-blank character of @p s. */
static char* trim_id(char* s) {
    while (*s == ' ' || *s == '\t') s++;
    char* t = s + strlen(s);
    while (t > s && (t[-1] == ' ' || t[-1] == '\t')) *--t = 0;
    return s;
}

/** @brief Append one id to an order list.
 *
 * @param order The order list of mod_id_row_t.
 * @param id The id; copied, truncated to MOD_ID_MAX - 1 characters.
 * @return 1 when appended, 0 when the push fails. */
static int order_push(mod_fvec* order, const char* id) {
    int n = (int)mod_fvec_count(order);
    if (!mod_fvec_push(order)) return 0;
    snprintf(id_row(order, n), MOD_ID_MAX, "%s", id);
    return 1;
}

/** @brief Build the order list from a --mods or OPENPETE_MODS comma-separated id list.
 *
 * Each item is trimmed; an empty item is ignored and an id of MOD_ID_MAX
 * characters or more is skipped with a warning.
 * @param list The comma-separated list.
 * @param order The order list appended to.
 * @return The number of ids appended. */
static int order_from_mods_flag(const char* list, mod_fvec* order) {
    int n = 0;
    for (const char* p = list; *p; ) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char item[MOD_ID_MAX * 2];
        char* s;
        snprintf(item, sizeof item, "%.*s", (int)len, p);
        s = trim_id(item);
        if (strlen(s) >= MOD_ID_MAX)
            OPL_WARN_F("mods", "--mods: id \"%.32s...\" exceeds %d "
                       "characters - skipped", s, MOD_ID_MAX - 1);
        else if (*s)
            n += order_push(order, s);
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

/** @brief Build the order list from an open enabled.txt, one id per line.
 *
 * Blank lines and lines whose first non-blank character is '#' are
 * skipped. Each iteration consumes one whole line: the rest of a line
 * longer than the 511-character buffer is drained, so its tail never
 * reads as a separate id, and an over-long non-comment line is skipped
 * with a warning. A line counts as truncated only when fgets filled the
 * buffer without a newline; a final line without a newline, which many
 * editors write, is a complete line and loads. An id of MOD_ID_MAX
 * characters or more is skipped with a warning.
 * @param en The open enabled.txt.
 * @param order The order list appended to.
 * @return The number of ids appended. */
static int order_from_enabled_txt(FILE* en, mod_fvec* order) {
    char line[512];
    int n = 0;
    while (fgets(line, sizeof line, en)) {
        size_t got = strlen(line);
        int truncated = (got == sizeof line - 1) && line[got - 1] != '\n';
        char* s;
        if (truncated) { /* drain the rest of the over-long line */
            int c;
            while ((c = fgetc(en)) != EOF && c != '\n') { }
        }
        line[strcspn(line, "\r\n")] = 0;
        s = trim_id(line);
        if (!*s || *s == '#') continue;
        if (truncated)
            OPL_WARN_F("mods", "enabled.txt: line starting \"%.32s\" is "
                       "longer than %d characters - skipped",
                       s, (int)(sizeof line - 1));
        else if (strlen(s) >= MOD_ID_MAX)
            OPL_WARN_F("mods", "enabled.txt: id \"%.32s...\" exceeds "
                       "%d characters - skipped", s, MOD_ID_MAX - 1);
        else
            n += order_push(order, s);
    }
    return n;
}

/** @brief Build the order list from every scanned id in lexicographic order.
 *
 * Used when neither --mods, OPENPETE_MODS nor enabled.txt gives a list.
 * @param ids The scanned ids; sorted in place.
 * @param n_ids Number of rows in @p ids.
 * @param order The order list appended to.
 * @return The number of ids appended. */
static int order_from_scan(mod_fvec* ids, int n_ids, mod_fvec* order) {
    int n = 0;
    qsort(mod_fvec_data(ids), (size_t)n_ids, sizeof(mod_id_row_t),
          cmp_id_row);
    for (int i = 0; i < n_ids; i++) {
        if (!order_push(order, id_row(ids, i))) break;
        n++;
    }
    return n;
}

/** @brief Resolve the mods root and append one mod_ref_t per mod to load to @p out, the enabled set first in priority order.
 *
 * openpete_mods_prepare, openpete_mods_init and
 * openpete_mods_register_key_defaults all run it, so they see the same
 * set; it is idempotent (zip unpack is content-stamped). The enabled set
 * is opts->mods_enabled or env OPENPETE_MODS (a comma list that replaces
 * enabled.txt; an empty list enables nothing), else enabled.txt, else
 * every scanned id in lexicographic order; the order is the priority
 * (later is higher). An enabled id with no scanned candidate is skipped
 * with a warning that says whether the folder or only its mod.toml is
 * missing. Enabled refs get boot_on = 1. When the set came from
 * enabled.txt and the run is not headless, every other scanned mod is
 * appended with boot_on = 0 (loaded, entry deferred to its first enable),
 * so the Mods panel can list and enable it. Sets s_order_src for the
 * warnings.
 * @param opts Parsed options, or NULL.
 * @param mods_dir Receives the mods root.
 * @param mods_dir_cap Size of @p mods_dir.
 * @param out Vector of mod_ref_t appended to.
 * @return The number of refs appended; 0 when no root resolves, no mod is
 *         scanned, or the --mods list names none. */
static int mods_discover(const openpete_options_t* opts,
                         char* mods_dir, size_t mods_dir_cap,
                         mod_fvec* out) {
    if (mods_root(opts, mods_dir, mods_dir_cap) != 0) return 0;
    snprintf(s_mods_root_dir, sizeof s_mods_root_dir, "%s", mods_dir);

    /* --mods (env OPENPETE_MODS) replaces enabled.txt wholesale: a
     * comma-separated id list, same order-IS-priority semantics. An
     * empty list is meaningful - "enable nothing" - so it is checked
     * for presence, not truthiness. */
    const char* enabled_override =
        opts && opts->mods_enabled ? opts->mods_enabled
                                   : getenv("OPENPETE_MODS");
    s_order_src = enabled_override ? "--mods" : "enabled.txt";

    mod_fvec ids, order;
    int n_ids, n_order, n_out = 0;
    mod_fvec_init(&ids, sizeof(mod_id_row_t), 64, NULL, 0);
    mod_fvec_init(&order, sizeof(mod_id_row_t), 64, NULL, 0);
    n_ids = mods_scan(mods_dir, &ids);
    if (n_ids == 0) { mod_fvec_free(&ids); mod_fvec_free(&order); return 0; }

    /* enabled.txt (one id per line) fixes the order; the order IS the
     * priority (later = higher). Fallback: lexicographic. */
    char en_path[MOD_PATH_MAX];
    snprintf(en_path, sizeof en_path, "%s/enabled.txt", mods_dir);
    FILE* en = enabled_override ? NULL : fopen(en_path, "r");
    if (enabled_override)  n_order = order_from_mods_flag(enabled_override,
                                                          &order);
    else if (en)           n_order = order_from_enabled_txt(en, &order);
    else                   n_order = order_from_scan(&ids, n_ids, &order);
    if (en) fclose(en);
    if (enabled_override && n_order == 0) {
        OPL_INFO_F("mods", "--mods lists no mods - none enabled "
                   "(mods dir: %s)", mods_dir);
        mod_fvec_free(&ids); mod_fvec_free(&order);
        return 0;
    }

    for (int i = 0; i < n_order; i++) {
        mod_ref_t* slot;
        int known = 0;
        for (int j = 0; j < n_ids; j++)
            if (strcmp(id_row(&order, i), id_row(&ids, j)) == 0) known = 1;
        if (!known) {
            /* Say which case it is: a folder missing its manifest, or no
             * folder at all (typical after enabling a mod the deployed
             * mods dir doesn't have yet - rebuild/stage or copy it in). */
            char d[MOD_PATH_MAX];
            const char* want = id_row(&order, i);
            snprintf(d, sizeof d, "%s/%s", mods_dir, want);
            if (file_mtime(d) == (time_t)-1)
                OPL_WARN_F("mods", "%s names \"%s\" but there is "
                           "no mods/%s/ folder (or <id>.zip) here - skipped "
                           "(mods dir: %s)", s_order_src, want, want,
                           mods_dir);
            else
                OPL_WARN_F("mods", "%s names \"%s\" but mods/%s/ has "
                           "no mod.toml - skipped", s_order_src, want, want);
            continue;
        }
        slot = (mod_ref_t*)mod_fvec_push(out);
        if (!slot) break;
        mod_ref_resolve(mods_dir, id_row(&order, i), slot);
        slot->boot_on = 1;
        n_out++;
    }

    /* Deferred-entry load-all (interactive path): every other
     * discovered mod loads too - compiled and dlopen'd, entry deferred
     * to its first enable - so the overlay's Mods panel can list and
     * enable the whole directory. Scope guard: --mods/OPENPETE_MODS
     * (every harness/CI case) and headless runs keep exact-set
     * semantics; the no-enabled.txt fallback already enables all. */
    if (!enabled_override && en && opts && !opts->headless) {
        qsort(mod_fvec_data(&ids), (size_t)n_ids, sizeof(mod_id_row_t),
              cmp_id_row);
        for (int i = 0; i < n_ids; i++) {
            mod_ref_t* slot;
            int listed = 0;
            for (int j = 0; j < n_out && !listed; j++)
                if (strcmp(ref_at(out, j)->id, id_row(&ids, i)) == 0)
                    listed = 1;
            if (listed) continue;
            slot = (mod_ref_t*)mod_fvec_push(out);
            if (!slot) break;
            mod_ref_resolve(mods_dir, id_row(&ids, i), slot);
            slot->boot_on = 0;
            n_out++;
        }
    }
    mod_fvec_free(&ids);
    mod_fvec_free(&order);
    return n_out;
}

/** @brief Report one mod's required games with no disc in the library, for openpete_mods_missing_games.
 *
 * @param mod_dir The mod's directory.
 * @param id The mod id.
 * @param running The game about to run, or NULL.
 * @param fn Called once per missing game.
 * @param ud Passed to @p fn.
 * @return The number of calls made. */
static int mod_missing_games(const char* mod_dir, const char* id, const char* running,
                             openpete_mods_missing_game_fn fn, void* ud) {
    char path[MOD_PATH_MAX + 12];
    snprintf(path, sizeof path, "%s/mod.toml", mod_dir);
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;
    char errbuf[256];
    toml_table_t* root = toml_parse_file(fp, errbuf, sizeof errbuf);
    fclose(fp);
    toml_table_t* t = root ? toml_table_in(root, "mod") : NULL;
    char want[96] = "";
    int implied = 0, calls = 0;
    if (t && (!running || mod_table_selects_game(t, running, want, sizeof want,
                                                 &implied))) {
        toml_datum_t own = toml_string_in(t, "game");
        mod_game_list_t games;
        parse_games(t, &games, id);
        for (int i = 0; i < games.n; i++) {
            const char* g = games.rows[i].id;
            const char* skip = running ? running : (own.ok ? own.u.s : "");
            char cue[MOD_PATH_MAX];
            if (!games.rows[i].required || strcmp(g, skip) == 0 ||
                !openpete_game_registered(g) ||
                openpete_library_game_disc(g, cue, sizeof cue) == 0)
                continue;
            fn(ud, id, g);
            calls++;
        }
        if (own.ok) free(own.u.s);
    }
    if (root) toml_free(root);
    return calls;
}

int openpete_mods_missing_games(const openpete_options_t* opts, const char* running,
                                openpete_mods_missing_game_fn fn, void* ud) {
    char mods_dir[MOD_PATH_MAX];
    if (!fn || mods_root(opts, mods_dir, sizeof mods_dir) != 0) return 0;
    const char* list = opts && opts->mods_enabled ? opts->mods_enabled
                                                  : getenv("OPENPETE_MODS");
    mod_fvec order;
    mod_fvec_init(&order, sizeof(mod_id_row_t), 64, NULL, 0);
    char en_path[MOD_PATH_MAX + 16];
    snprintf(en_path, sizeof en_path, "%s/enabled.txt", mods_dir);
    FILE* en = list ? NULL : fopen(en_path, "r");
    const int n = list ? order_from_mods_flag(list, &order)
                       : en ? order_from_enabled_txt(en, &order) : 0;
    if (en) fclose(en);
    int calls = 0;
    for (int i = 0; i < n; i++) {
        mod_ref_t r;
        mod_ref_resolve(mods_dir, id_row(&order, i), &r);
        calls += mod_missing_games(r.dir, r.id, running, fn, ud);
    }
    mod_fvec_free(&order);
    return calls;
}

/** @brief Write the staged SDK directory, <exe dir>/sdk, into @p out.
 *
 * Falls back to ./sdk when SDL reports no base path. The path is
 * truncated to @p cap.
 * @param out Output buffer.
 * @param cap Size of @p out. */
static void mods_sdk_dir(char* out, size_t cap) {
    const char* base = SDL_GetBasePath();
    snprintf(out, cap, "%ssdk", base ? base : "./");
}

/** @brief Test whether @p sdk_dir holds the SDK root's openpete_mod_api.h and games/<game>/openpete_game.h as regular files.
 *
 * @param sdk_dir The SDK directory.
 * @param game The game id.
 * @return 1 when both exist, else 0. */
static int sdk_game_staged(const char* sdk_dir, const char* game) {
    char p[MOD_PATH_MAX + 64];
    struct stat st;
    snprintf(p, sizeof p, "%s/openpete_mod_api.h", sdk_dir);
    if (stat(p, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    snprintf(p, sizeof p, "%s/games/%s/openpete_game.h", sdk_dir, game);
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/** @brief Test whether the staged SDK serves the running game, probing for its openpete_mod_api.h and games/<game-id>/openpete_game.h.
 *
 * An install packaged without --with-mod-sdk has no <exe>/sdk, and a
 * compile would fail inside the compiler with a header-not-found in
 * compile.log that names nothing. Callers check this first, skip the
 * compile, and name the cause once through mods_sdk_missing_notice. The
 * api header is the probe because every compile line includes it; the
 * game header, because without it an object records no game and the
 * loader refuses it.
 * @param sdk_dir The staged SDK directory.
 * @return 1 when both exist as regular files, else 0. */
static int mods_sdk_present(const char* sdk_dir) {
    return sdk_game_staged(sdk_dir, mods_build_game());
}

/** @brief Write the SDK directory --compile-mods compiles against into @p out.
 *
 * OPENPETE_MOD_SDK_DIR, when set and nonempty, replaces <exe dir>/sdk
 * (a harness variable: it points one game's binary at the build tree's
 * mod-sdk-test/sdk/); compile-on-boot and the loader never read it.
 * @param out Output buffer.
 * @param cap Size of @p out. */
static void mods_compile_sdk_dir(char* out, size_t cap) {
    const char* e = getenv("OPENPETE_MOD_SDK_DIR");
    if (e && *e) {
        snprintf(out, cap, "%s", e);
        OPL_INFO_F("mods", "--compile-mods: SDK %s (OPENPETE_MOD_SDK_DIR)",
                   out);
        return;
    }
    mods_sdk_dir(out, cap);
}

/** @brief Log once per process that this build ships without the mod SDK.
 *
 * States that source mods are not compiled and that a prebuilt object
 * still loads. Later calls do nothing.
 * @param sdk_dir The SDK path the probe looked in, named in the line. */
static void mods_sdk_missing_notice(const char* sdk_dir) {
    static int s_said = 0;
    if (s_said) return;
    s_said = 1;
    OPL_WARN_F("mods", "no mod SDK for %s at %s (openpete_mod_api.h and "
               "games/%s/): this build was packaged without it - source "
               "mods are not compiled; a prebuilt object in .build/%s/, "
               ".build/any/ or .build/ still loads", mods_build_game(), sdk_dir,
               mods_build_game(), mods_build_game());
}

/* ---- compile-on-boot prepare (openpete-mod-compile-on-boot) ---------- */

struct mods_prepare_ctx {
    mod_fvec                  refs;   /* mod_ref_t */
    mod_fvec                  keys;   /* mod_key_row_t, one per ref */
    mod_fvec                  stale;  /* int, indices into refs */
    int                       n_stale;
    char                      sdk_dir[MOD_PATH_MAX];
    openpete_mods_progress_fn progress;
};

/** @brief Compile every stale mod of a prepare context, reporting progress.
 *
 * Runs as one task, on a worker thread when the caller supplies a runner.
 * Calls the progress callback with "COMPILING MODS" before each compile
 * and once more with the full count. A failed compile is logged by
 * mod_compile and left for openpete_mods_init to report; it does not stop
 * the loop.
 * @param ud The struct mods_prepare_ctx to work through.
 * @return 0 always. */
static int mods_prepare_task(void* ud) {
    struct mods_prepare_ctx* c = (struct mods_prepare_ctx*)ud;
    for (int i = 0; i < c->n_stale; i++) {
        int si = int_row(&c->stale, i);
        const mod_ref_t* r = ref_at(&c->refs, si);
        if (c->progress) c->progress("COMPILING MODS", i, c->n_stale);
        char so_path[MOD_PATH_MAX];
        if (mod_obj_path(r->dir, r->id, so_path, sizeof so_path) != 0) continue;
        /* A failure is logged (+ compile.log) and left for init to
         * report as disabled - prepare never aborts the boot. */
        (void)mod_compile(r->dir, r->id, c->sdk_dir,
                          ((mod_key_row_t*)mod_fvec_at(&c->keys, (size_t)si))->k,
                          so_path, 1);
    }
    if (c->progress) c->progress("COMPILING MODS", c->n_stale, c->n_stale);
    return 0;
}

int openpete_mods_prepare(const openpete_options_t* opts,
                          openpete_mods_progress_fn progress,
                          openpete_mods_run_task_fn run_task) {
    {
        char cc[MOD_PATH_MAX];
        mods_resolve_cc(cc, sizeof cc);
        if (!cc[0]) return 0; /* no compiler on this box: init's
                               * prebuilt-DLL fallback owns messaging */
    }
    /* Boot-once, main-thread. The lists are heap-backed; releasing them
     * before the memset keeps a second call from orphaning the first
     * call's rows (free on a zeroed vec is a no-op). */
    static struct mods_prepare_ctx c;
    mod_fvec_free(&c.refs);
    mod_fvec_free(&c.keys);
    mod_fvec_free(&c.stale);
    memset(&c, 0, sizeof c);
    mod_fvec_init(&c.refs, sizeof(mod_ref_t), 64, NULL, 0);
    mod_fvec_init(&c.keys, sizeof(mod_key_row_t), 64, NULL, 0);
    mod_fvec_init(&c.stale, sizeof(int), 64, NULL, 0);
    char mods_dir[MOD_PATH_MAX];
    int n_refs = mods_discover(opts, mods_dir, sizeof mods_dir, &c.refs);
    if (n_refs <= 0) return 0;
    if (mod_fvec_resize(&c.keys, (size_t)n_refs) != 0) return 0;
    mods_sdk_dir(c.sdk_dir, sizeof c.sdk_dir);
    if (!mods_sdk_present(c.sdk_dir)) {
        for (int i = 0; i < n_refs; i++) {
            char key[65];
            if (mod_compute_key(ref_at(&c.refs, i)->dir, c.sdk_dir, key) == 0) {
                mods_sdk_missing_notice(c.sdk_dir); /* a src/ mod exists */
                break;
            }
        }
        return 0;
    }
    c.progress = progress;

    /* Pass 1: pure hashing - milliseconds; no window, no compile. */
    for (int i = 0; i < n_refs; i++) {
        char key[65];
        int* slot;
        const mod_ref_t* r = ref_at(&c.refs, i);
        if (mod_compute_key(r->dir, c.sdk_dir, key) != 0)
            continue; /* assets-only or binary-only mod */
        if (mod_key_cached(r->dir, r->id, key) ||
            mod_key_failed(r->dir, r->id, key))
            continue; /* current, or rejected before: init reports it */
        memcpy(((mod_key_row_t*)mod_fvec_at(&c.keys, (size_t)i))->k, key,
               sizeof key);
        slot = (int*)mod_fvec_push(&c.stale);
        if (!slot) break;
        *slot = i;
        c.n_stale++;
    }
    if (c.n_stale == 0) return 0;

    /* Pass 2: compile the stale set as ONE worker task (the splash
     * runner is not reentrant); progress from inside the worker
     * lazy-opens the UI. */
    OPL_INFO_F("mods", "%d mod%s stale - compiling before boot",
               c.n_stale, c.n_stale == 1 ? "" : "s");
    if (run_task)
        (void)run_task(mods_prepare_task, &c);
    else
        (void)mods_prepare_task(&c);
    return c.n_stale;
}

/** @brief Compile every mod in @p mods_dir whose manifest targets mods_build_game(), for --compile-mods.
 *
 * Scans every candidate, ignoring enabled.txt so a disabled mod's breakage
 * still surfaces; the scan's game filter drops, with one log line each,
 * the mods for other games. A mod whose cached build matches its key is
 * counted up to date; assets-only and binary-only mods are skipped. A
 * key the compiler rejected before (mod_key_failed) compiles again, so
 * this mode retries a failure whose cause lies outside the build key.
 * @param mods_dir The mods root.
 * @param sdk_dir The SDK directory holding games/<mods_build_game()>/.
 * @return The number of failed compiles. */
static int compile_all_for_game(const char* mods_dir, const char* sdk_dir) {
    mod_fvec ids;
    mod_fvec_init(&ids, sizeof(mod_id_row_t), 64, NULL, 0);
    int n_ids = mods_scan(mods_dir, &ids);
    if (n_ids > 1)
        qsort(mod_fvec_data(&ids), (size_t)n_ids, sizeof(mod_id_row_t),
              cmp_id_row);
    int n_compiled = 0, n_cached = 0, n_failed = 0;
    for (int i = 0; i < n_ids; i++) {
        mod_ref_t r;
        mod_ref_resolve(mods_dir, id_row(&ids, i), &r);
        char key[65];
        if (mod_compute_key(r.dir, sdk_dir, key) != 0)
            continue; /* assets-only or binary-only mod: nothing to do */
        if (mod_key_cached(r.dir, r.id, key)) {
            n_cached++;
            continue;
        }
        char so_path[MOD_PATH_MAX];
        if (mod_obj_path(r.dir, r.id, so_path, sizeof so_path) != 0) {
            n_failed++;
            continue;
        }
        if (mod_compile(r.dir, r.id, sdk_dir, key, so_path, 1) == 0)
            n_compiled++;
        else
            n_failed++; /* mod_compile logged + pointed at compile.log */
    }
    mod_fvec_free(&ids);
    OPL_INFO_F("mods", "--compile-mods [%s]: %d compiled, %d up to date, "
               "%d failed (%s)", mods_build_game(), n_compiled, n_cached,
               n_failed, mods_dir);
    return n_failed;
}

/** @brief Collect the ids of the games staged under <sdk_dir>/games/, sorted.
 *
 * A staged game is a subdirectory holding openpete_game.h beside the SDK
 * root's openpete_mod_api.h (sdk_game_staged).
 * @param sdk_dir The SDK directory.
 * @param ids Row vector of mod_id_row_t the ids are appended to.
 * @return The number of staged games. */
static int sdk_staged_games(const char* sdk_dir, mod_fvec* ids) {
    char games_dir[MOD_PATH_MAX];
    if (snprintf(games_dir, sizeof games_dir, "%s/games", sdk_dir) >= (int)sizeof games_dir)
        return 0;
    DIR* d = opendir(games_dir);
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof s_build_game ||
            !sdk_game_staged(sdk_dir, e->d_name))
            continue;
        mod_id_row_t* row = (mod_id_row_t*)mod_fvec_push(ids);
        if (row) snprintf(row->id, sizeof row->id, "%s", e->d_name);
    }
    closedir(d);
    const size_t n = mod_fvec_count(ids);
    if (n > 1) qsort(mod_fvec_data(ids), n, sizeof(mod_id_row_t), cmp_id_row);
    return (int)n;
}

int openpete_mods_compile_all(const openpete_options_t* opts) {
    {
        char cc[MOD_PATH_MAX];
        mods_resolve_cc(cc, sizeof cc);
        if (!cc[0]) {
            OPL_ERROR_F("mods", "--compile-mods: no compiler - expected "
                        "the bundled toolchain at recomp/tools/ next to "
                        "the exe, or set OPENPETE_MOD_CC");
            return -1;
        }
    }
    char mods_dir[MOD_PATH_MAX];
    if (mods_root(opts, mods_dir, sizeof mods_dir) != 0) {
        OPL_ERROR_F("mods", "--compile-mods: no mods dir resolvable");
        return -1;
    }
    char sdk_dir[MOD_PATH_MAX];
    mods_compile_sdk_dir(sdk_dir, sizeof sdk_dir);

    /* One pass per staged game: the per-game headers, the [mod] game
     * filter, the build key and the object directory all follow the
     * staged id, not the game this executable links. */
    mod_fvec games;
    mod_fvec_init(&games, sizeof(mod_id_row_t), 4, NULL, 0);
    const int n_games = sdk_staged_games(sdk_dir, &games);
    if (n_games == 0) {
        mod_fvec_free(&games);
        mods_sdk_missing_notice(sdk_dir);
        OPL_ERROR_F("mods", "--compile-mods: no SDK at %s", sdk_dir);
        return -1;
    }
    int n_failed = 0;
    for (int g = 0; g < n_games; g++) {
        snprintf(s_build_game, sizeof s_build_game, "%.*s",
                 (int)sizeof s_build_game - 1, id_row(&games, g));
        OPL_INFO_F("mods", "--compile-mods: staged %s (%s/games/%s)",
                   s_build_game, sdk_dir, s_build_game);
        n_failed += compile_all_for_game(mods_dir, sdk_dir);
    }
    s_build_game[0] = 0;
    mod_fvec_free(&games);
    return n_failed;
}

void openpete_mods_register_key_defaults(const openpete_options_t* opts) {
    char mods_dir[MOD_PATH_MAX];
    mod_fvec refs;
    mod_fvec_init(&refs, sizeof(mod_ref_t), 64, NULL, 0);
    int n_refs = mods_discover(opts, mods_dir, sizeof mods_dir, &refs);
    static struct openpete_mod scratch;   /* ~7 KB; see the note in init */
    for (int i = 0; i < n_refs; i++) {
        /* The slot is reused per ref: release the previous manifest's
         * rows before the memset drops the pointers to them. */
        mod_rows_free(&scratch);
        memset(&scratch, 0, sizeof scratch);
        if (parse_manifest(ref_at(&refs, i)->dir, &scratch) != 0) continue;
        mod_register_key_defaults(&scratch, ref_at(&refs, i)->boot_on);
    }
    mod_rows_free(&scratch);   /* the last ref's rows */
    mod_fvec_free(&refs);
}

int openpete_mods_init(const openpete_options_t* opts) {
    /* The game descriptor is fixed from here on (openpete_mods_set_game_desc). */
    s_mods_init_ran = 1;

    /* The ImGui guard attributes a refused call by mod index; the loader
     * owns the index-to-id mapping. */
    openpete_imgui_guard_set_mod_id_fn(openpete_mods_id);

    /* Before the no-mods early return below, so a run that loads nothing
     * still reports the census. */
    mod_vec_census_arm();

    /* Pure vk windowed = native is the sole display and the PSX ordering table
     * is not rasterized, so render-function overrides are visual no-ops. Under
     * psycross / ab / diff / headless the PSX path still rasterizes. */
    s_native_hides_psx = opts && opts->renderer == OPENPETE_RENDERER_VK
                              && !opts->headless;

    /* Before the first mod entry below, which the guard brackets. */
    mods_guard_install(opts);

    mods_hot_reload_arm(opts);

    mods_capture_disc_path(opts);

    char mods_dir[MOD_PATH_MAX];
    mod_fvec refs;
    mod_fvec_init(&refs, sizeof(mod_ref_t), 64, NULL, 0);
    int n_refs = mods_discover(opts, mods_dir, sizeof mods_dir, &refs);
    if (n_refs <= 0) {
        mod_fvec_free(&refs);
        openpete_modsave_on_mods_init();
        return 0;
    }

    char sdk_dir[MOD_PATH_MAX];
    mods_sdk_dir(sdk_dir, sizeof sdk_dir);
    snprintf(s_sdk_dir, sizeof s_sdk_dir, "%s", sdk_dir);

    /* Phase A: parse all discovered manifests (no loading yet).
     *
     * Heap, not a local array: struct openpete_mod is ~7 KB (it carries
     * several PATH_MAX buffers plus the [[material]] rows), so one per
     * discovered mod is hundreds of kilobytes - as a local it overflowed
     * the Windows main-thread stack (boot died with
     * EXCEPTION_STACK_OVERFLOW right after the PSX EXE loaded, while
     * Linux's 8 MB stack absorbed it and showed nothing). */
    mod_fvec pending;
    int n_pending = 0;
    mod_fvec_init(&pending, sizeof(struct openpete_mod), 64, NULL, 0);
    for (int i = 0; i < n_refs; i++) {
        struct openpete_mod* mod =
            (struct openpete_mod*)mod_fvec_push(&pending);
        if (!mod) break;
        if (parse_manifest(ref_at(&refs, i)->dir, mod) != 0) {
            /* A partial parse can already have pushed rows; the slot is
             * about to be reused by the next ref. */
            mod_rows_free(mod);
            mod_fvec_truncate(&pending, (size_t)n_pending);
            continue;
        }
        mod->boot_on = ref_at(&refs, i)->boot_on;
        mod_register_key_defaults(mod, mod->boot_on);
        n_pending++;
    }

    /* Phase B: topo-sort by hard deps (see topo_visit above). */
    topo_ctx_t tc;
    memset(&tc, 0, sizeof tc);
    tc.pending   = &pending;
    tc.n_pending = n_pending;
    tc.mods_dir  = mods_dir;
    mod_fvec_init(&tc.state, sizeof(int), 64, NULL, 0);
    mod_fvec_init(&tc.topo, sizeof(int), 64, NULL, 0);
    (void)mod_fvec_resize(&tc.state, (size_t)n_pending);
    mods_games_snapshot();
    mods_refuse_missing_games(&tc);
    for (int i = 0; i < n_pending; i++) (void)topo_visit(&tc, i);

    /* Entry policy: boot-enabled mods enter at load, and so do their
     * deps (a library's entry must run before its client's - the dep
     * enters but stays disabled if it wasn't in the enabled set
     * itself). Every other load-all row defers entry to its first
     * enable. */
    mod_fvec must_enter;   /* int per pending row */
    mod_fvec_init(&must_enter, sizeof(int), 64, NULL, 0);
    (void)mod_fvec_resize(&must_enter, (size_t)n_pending);
    {
        int changed = 1;
        while (changed) {
            changed = 0;
            for (int i = 0; i < n_pending; i++) {
                if (!pend_at(&pending, i)->boot_on && !int_row(&must_enter, i))
                    continue;
                for (int d = 0;
                     d < mod_dep_count(pend_at(&pending, i)); d++) {
                    int j = topo_find(
                        &tc, mod_dep_at(pend_at(&pending, i), d)->id);
                    if (j >= 0 && !pend_at(&pending, j)->boot_on &&
                        !int_row(&must_enter, j)) {
                        int_row_set(&must_enter, j, 1);
                        changed = 1;
                    }
                }
            }
        }
    }

    /* Phase C: build + load in topo order; a load failure cascades to
     * dependents (their imports would dangle). */
    int loaded = 0, enabled = 0;
    mod_fvec load_ok;   /* int per pending row */
    mod_fvec_init(&load_ok, sizeof(int), 64, NULL, 0);
    (void)mod_fvec_resize(&load_ok, (size_t)n_pending);
    for (int t = 0; t < tc.n_topo; t++) {
        int i = int_row(&tc.topo, t);
        int deps_ok = 1;
        for (int d = 0; d < mod_dep_count(pend_at(&pending, i)); d++) {
            int j = topo_find(&tc,
                              mod_dep_at(pend_at(&pending, i), d)->id);
            if (j < 0 || !int_row(&load_ok, j)) deps_ok = 0;
        }
        if (!deps_ok) {
            OPL_ERROR_F("mods", "\"%s\" disabled: a dependency failed to "
                        "load", pend_at(&pending, i)->id);
            mod_rows_free(pend_at(&pending, i));
            continue;
        }
        if (!s_mods.elem_size)
            mod_cvec_init(&s_mods, sizeof(struct openpete_mod), MODS_BLOCK,
                          "mods", MODS_BLOCK);
        struct openpete_mod* mod =
            (struct openpete_mod*)mod_cvec_push(&s_mods);
        if (!mod) {
            OPL_ERROR_F("mods", "mod table allocation failed - \"%s\" not "
                        "loaded", pend_at(&pending, i)->id);
            break;
        }
        *mod = *pend_at(&pending, i);
        /* The copy took the row storage with it; the pending slot must
         * not free what the live row now owns. */
        mod_rows_disown(pend_at(&pending, i));
        /* Both set before load_mod, which runs the entry: the row is in
         * the live table from the push on, so anything the entry
         * registers asks the table about this mod and must read its
         * final state. A registration surface records whether its owner
         * starts disabled, so an enabled mod whose row still read
         * "disabled" would register a hook that never runs. */
        mod->load_index = mod_count() - 1;
        mod->enabled = mod->boot_on;
        if (load_mod(mod, sdk_dir,
                     pend_at(&pending, i)->boot_on ||
                     int_row(&must_enter, i)) != 0) {
            openpete_keymap_set_mod_enabled(mod->id, 0);
            /* Drop the dead tail element, the way the fixed table left
             * the slot for the next copy to overwrite. */
            mod_rows_free(mod);
            mod_cvec_remove_at(&s_mods, (size_t)mod_count() - 1);
            continue;
        }
        int_row_set(&load_ok, i, 1);
        loaded++;
        if (mod->enabled) enabled++;
        /* Boot-disabled rows: make every registration surface agree.
         * Matters for entered-but-disabled deps: their entry registered
         * chains/hooks, which default to enabled. */
        if (!mod->enabled) {
            openpete_mods_chain_set_owner_enabled(mod->id, 0);
            openpete_mods_draw_set_owner_enabled(mod->id, 0);
            openpete_mods_registry_set_owner_enabled(mod->id, 0);
        }
    }

    /* Every pending row the transfer did not take. topo_visit marks a
     * mod TS_FAILED on a dependency cycle or a missing dependency and
     * returns before adding it to tc.topo, and phase C iterates tc.topo,
     * so such a row is never reached by the copy or by either failure
     * path above and would hold its manifest rows for the session. The
     * sweep is the catch-all; the earlier frees stay because they
     * release at the point of failure, which holds the peak allocation
     * to the live set rather than to the discovered set. A row whose
     * storage moved was disowned, so this is a no-op for it. */
    for (int i = 0; i < n_pending; i++)
        mod_rows_free(pend_at(&pending, i));

    /* Declared-bindings conflict scan: two enabled mods claiming the
     * same host key is legal (reads are non-exclusive) but surprising -
     * name both, with purposes, so the user can re-order or rebind.
     *
     * Compares the EFFECTIVE binding, not the declared one: rebinding a
     * mod's key in [keys.mod.<id>] is the documented way to resolve one
     * of these, so a scan blind to the override would keep warning about
     * a clash the user has already fixed. */
    for (int i = 0; i < mod_count(); i++)
        for (int bi = 0; bi < mod_binding_count(mod_at(i)); bi++) {
            const mod_binding_t* bind_i = mod_binding_at(mod_at(i), bi);
            char key_i[80];
            snprintf(key_i, sizeof key_i, "%s",
                     openpete_keymap_mod_effective_key(
                         mod_at(i)->id, bind_i->name,
                         bind_i->key));
            if (strcmp(key_i, "(unbound)") == 0) continue;
            for (int j = i + 1; j < mod_count(); j++)
                for (int bj = 0; bj < mod_binding_count(mod_at(j)); bj++) {
                    const mod_binding_t* bind_j =
                        mod_binding_at(mod_at(j), bj);
                    const char* key_j = openpete_keymap_mod_effective_key(
                        mod_at(j)->id, bind_j->name,
                        bind_j->key);
                    if (strcmp(key_i, key_j) == 0)
                        OPL_WARN_F("mods", "key \"%s\" bound by both %s "
                                   "(%s) and %s (%s)",
                                   key_i,
                                   mod_at(i)->id,
                                   bind_i->purpose,
                                   mod_at(j)->id,
                                   bind_j->purpose);
                }
        }

    if (enabled > 0)
        OPL_INFO_F("mods", "%d mod%s active (dir: %s)", enabled,
                   enabled == 1 ? "" : "s", mods_dir);
    if (loaded > enabled)
        OPL_INFO_F("mods", "%d more mod%s loaded disabled (entry "
                   "deferred) - enable via the M-overlay Mods panel or "
                   "the O-menu", loaded - enabled,
                   loaded - enabled == 1 ? "" : "s");
    toggle_parse_env();
    cfg_apply_schedule_parse();

    /* The working lists die with the boot; the live table owns the rows
     * copied into it. */
    mod_fvec_free(&load_ok);
    mod_fvec_free(&must_enter);
    mod_fvec_free(&tc.state);
    mod_fvec_free(&tc.topo);
    mod_fvec_free(&pending);
    mod_fvec_free(&refs);
    /* After every boot entry: arms the save records, sizes the key's block
     * by the parsed mod table and runs the duplicate save-override check. */
    openpete_modsave_on_mods_init();
    return loaded;
}

