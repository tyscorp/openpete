/* frame-inner - counts its GamestateDraw override calls (see mod.toml). */

#include <openpete_mod_api.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned g_calls;

/** @brief GamestateDraw override: count the call and run the original. */
static void on_draw(CPUState* cpu) {
    g_calls++;
    g_api->base(cpu);
}

/** @brief Log the call count. */
static void on_shutdown(void) {
    g_api->log(g_self, OP_MOD_LOG_INFO, "frame-inner: shutdown calls=%u",
               g_calls);
}

/**
 * @brief Mod entry: attaches the counting override.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0, or the first registration failure.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    api->log(self, OP_MOD_LOG_INFO, "frame-inner: entry gen=%lld",
             (long long)api->config_int(self, "gen", 0));
    if (api->register_shutdown_hook(self, on_shutdown) != 0) return 1;
    return api->override_name(self, "GamestateDraw", on_draw);
}
