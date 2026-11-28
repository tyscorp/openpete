/* bus-ident - rewrites its own manifest identity before a hot reload (see
 * mod.toml). Logs its entry, the edit and, from its shutdown hook, the
 * override's call count. */

#include <openpete_mod_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned g_calls;
static int64_t  g_touch_ident, g_touch_at, g_gen;

/** @brief Rewrite mod.toml's id (touch_ident 1) or api pin (2), and config.toml's gen, so the poll requests a reload. */
static void touch(void) {
    char dir[1024], cmd[2400];
    const char* data = g_api->data_dir(g_self);
    if (!data) return;
    snprintf(dir, sizeof dir, "%s", data);
    char* slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = 0;
    if (g_touch_ident == 1)
        snprintf(cmd, sizeof cmd,
                 "sed -i 's/^id *= *\"bus-ident\"/id      = \"bus-ident-renamed\"/' '%s/mod.toml'", dir);
    else
        snprintf(cmd, sizeof cmd, "sed -i 's/^api *= *13/api     = 12/' '%s/mod.toml'", dir);
    const int rc = system(cmd);
    snprintf(cmd, sizeof cmd, "printf 'gen = 1\\n' > '%s/config.toml'", dir);
    const int rc2 = system(cmd);
    g_api->log(g_self, OP_MOD_LOG_INFO, "bus-ident: edit %lld call=%u rc=%d,%d",
               (long long)g_touch_ident, g_calls, rc, rc2);
}

/** @brief GamestateDraw override: count, edit on touch_at, then base(). */
static void on_draw(CPUState* cpu) {
    g_calls++;
    if (g_touch_ident && (int64_t)g_calls == g_touch_at) touch();
    g_api->base(cpu);
}

/** @brief Shutdown hook: the call count and the generation this object entered with. */
static void on_shutdown(void) {
    g_api->log(g_self, OP_MOD_LOG_INFO, "bus-ident: shutdown calls=%u gen=%lld",
               g_calls, (long long)g_gen);
}

/**
 * @brief Mod entry: reads the schedule and attaches the override.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0, or 1 on a failed registration.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    g_touch_ident = api->config_int(self, "touch_ident", 0);
    g_touch_at    = api->config_int(self, "touch_at", 0);
    g_gen         = api->config_int(self, "gen", 0);
    api->log(self, OP_MOD_LOG_INFO, "bus-ident: entry gen=%lld", (long long)g_gen);
    if (api->register_shutdown_hook(self, on_shutdown) != 0) return 1;
    return api->override_name(self, "GamestateDraw", on_draw);
}
