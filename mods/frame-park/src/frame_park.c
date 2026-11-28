/* frame-park - holds its GamestateDraw override frame across the fiber's
 * parks (see mod.toml). Logs the burst, a reentry count and the call
 * counts from its shutdown hook. */

#include <openpete_mod_api.h>

#include <stdio.h>
#include <string.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static unsigned g_calls;      /* override entries */
static unsigned g_bases;      /* base() calls made */
static unsigned g_reentered;  /* entries while an earlier entry is live */
static int      g_depth;      /* live entries of the override */
static int64_t  g_park_at, g_park_bases, g_touch_at;

/** @brief Rewrite this mod's config.toml with gen = 1, keeping the burst schedule.
 *
 * The directory is the parent of data_dir. The hot-reload poll compares the
 * config file's key, so the write requests a reload. */
static void touch_config(void) {
    char path[1024];
    const char* data = g_api->data_dir(g_self);
    if (!data) return;
    snprintf(path, sizeof path, "%s", data);
    char* slash = strrchr(path, '/');
    if (!slash) return;
    snprintf(slash, sizeof path - (size_t)(slash - path), "/config.toml");
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "park_at = %lld\npark_bases = %lld\ngen = 1\n",
            (long long)g_park_at, (long long)g_park_bases);
    fclose(f);
    g_api->log(g_self, OP_MOD_LOG_INFO, "frame-park: config.toml edited call=%u",
               g_calls);
}

/** @brief GamestateDraw override: one base(), or park_bases of them on the burst call. */
static void on_draw(CPUState* cpu) {
    g_calls++;
    if (g_depth > 0) g_reentered++;
    g_depth++;
    if (g_touch_at > 0 && (int64_t)g_calls == g_touch_at) touch_config();
    const int burst = g_park_at > 0 && (int64_t)g_calls == g_park_at;
    const int64_t n = burst ? g_park_bases : 1;
    if (burst)
        g_api->log(g_self, OP_MOD_LOG_INFO, "frame-park: burst begins call=%u",
                   g_calls);
    for (int64_t i = 0; i < n; i++) {
        g_bases++;
        g_api->base(cpu);
    }
    if (burst)
        g_api->log(g_self, OP_MOD_LOG_INFO, "frame-park: burst ends call=%u",
                   g_calls);
    g_depth--;
}

/** @brief Log the counts; live=1 means the hook ran while a call of the override was on the stack. */
static void on_shutdown(void) {
    g_api->log(g_self, OP_MOD_LOG_INFO,
               "frame-park: shutdown calls=%u bases=%u reentered=%u live=%d",
               g_calls, g_bases, g_reentered, g_depth > 0);
}

/**
 * @brief Mod entry: reads the burst schedule and attaches the override.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0, or the first registration failure.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    g_api = api; g_self = self;
    g_park_at    = api->config_int(self, "park_at", 0);
    g_park_bases = api->config_int(self, "park_bases", 1);
    g_touch_at   = api->config_int(self, "touch_config", 0);
    api->log(self, OP_MOD_LOG_INFO, "frame-park: entry park_at=%lld gen=%lld",
             (long long)g_park_at, (long long)api->config_int(self, "gen", 0));
    if (api->register_shutdown_hook(self, on_shutdown) != 0) return 1;
    return api->override_name(self, "GamestateDraw", on_draw);
}
