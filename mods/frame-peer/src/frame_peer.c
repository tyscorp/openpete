/* frame-peer - links, toggle hook and level-ready hook around frame-park
 * (see mod.toml). */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* RTLD_DEFAULT; the SDK compile line defines it too */
#endif
#include <openpete_mod_api.h>

#include <stddef.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* Spyro 1 GamestateDraw (src/gamestates/draw.c): waits in VSync(0) for the
 * previous draw and submits a new one, so each call is one tick boundary. */
#define SPYRO1_GAMESTATE_DRAW 0x8001ED5Cu
/* g_CurDB: GamestateDraw flips it between the two DB buffers on every run. */
#define SPYRO1_G_CURDB 0x80075888u

typedef void (*level_fn)(CPUState* cpu, int level_id);
typedef int (*on_level_fn)(const char* mod_id, level_fn fn, const char* log_tag);

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static int      g_toggle_base, g_toggle_crash;
static int64_t  g_level_park;
static int      g_level_parked;
static int      g_level_parking; /* inside the level hook's draw loop */

/** @brief GamestateDraw override: run the chain below. */
static void on_draw(CPUState* cpu) { g_api->base(cpu); }

/** @brief Toggle hook: optionally call base() or fault, per the config. */
static void on_toggle(CPUState* cpu, int on) {
    if (g_toggle_crash) {
        g_api->log(g_self, OP_MOD_LOG_INFO, "frame-peer: toggle hook faulting");
        volatile int* p = NULL;
        *p = 1;
    }
    if (g_toggle_base) {
        const volatile uint32_t* db = (const volatile uint32_t*)g_api->guest(SPYRO1_G_CURDB);
        const uint32_t before = db ? *db : 0;
        g_api->base(cpu);
        const uint32_t after = db ? *db : 0;
        g_api->log(g_self, OP_MOD_LOG_INFO,
                   "frame-peer: toggle hook on=%d base returned ran_draw=%d",
                   on, before != after);
    }
}

/** @brief Level-ready hook: on the first level, call GamestateDraw level_park times, one park each. */
static void on_level(CPUState* cpu, int level_id) {
    if (g_level_parked) return;
    g_level_parked = 1;
    g_level_parking = 1;
    g_api->log(g_self, OP_MOD_LOG_INFO, "frame-peer: level park begins level=%d",
               level_id);
    for (int64_t i = 0; i < g_level_park; i++)
        g_api->call(cpu, SPYRO1_GAMESTATE_DRAW);
    g_level_parking = 0;
    g_api->log(g_self, OP_MOD_LOG_INFO, "frame-peer: level park ends");
}

/** @brief Log whether the level hook's loop was live when the hook ran. */
static void on_shutdown(void) {
    g_api->log(g_self, OP_MOD_LOG_INFO, "frame-peer: shutdown level_parking=%d",
               g_level_parking);
}

/**
 * @brief Mod entry: registers the configured links and hooks.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0, or 1 on a registration failure.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    const int64_t links = api->config_int(self, "links", 1);
    g_toggle_base  = api->config_bool(self, "toggle_base", 0);
    g_toggle_crash = api->config_bool(self, "toggle_crash", 0);
    g_level_park   = api->config_int(self, "level_park", 0);
    api->log(self, OP_MOD_LOG_INFO, "frame-peer: entry links=%lld gen=%lld",
             (long long)links, (long long)api->config_int(self, "gen", 0));
    for (int64_t i = 0; i < links; i++)
        if (api->override_name(self, "GamestateDraw", on_draw) != 0) return 1;
    if (g_toggle_base || g_toggle_crash)
        if (api->register_toggle_hook(self, on_toggle) != 0) return 1;
    if (api->register_shutdown_hook(self, on_shutdown) != 0) return 1;
    if (g_level_park > 0) {
#ifdef _WIN32
        on_level_fn reg = (on_level_fn)(void*)GetProcAddress(GetModuleHandleA(NULL),
                                                             "openpete_mods_on_level");
#else
        on_level_fn reg = (on_level_fn)dlsym(RTLD_DEFAULT, "openpete_mods_on_level");
#endif
        if (!reg || reg("frame-peer", on_level, "frame-peer") != 0) {
            api->log(self, OP_MOD_LOG_ERROR, "frame-peer: no level hook registration");
            return 1;
        }
    }
    return 0;
}
