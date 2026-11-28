/* svc-member - one provider or consumer of the service rungs (see mod.toml).
 * Each probe logs one line, "svc-member <id>: <name> <value>...", that
 * tests/harness/mods_svc_rungs.sh reads. */

#include <openpete_mod_api.h>
#include <psx_runtime.h> /* CPUState */

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PING_OK = 0, PING_RETIRED = 2 };

/* The table a provider exports. */
typedef struct {
    uint32_t size;
    uint32_t marker;  /* PROBE_MARKER in the exported copy */
    uint32_t variant; /* 1, or 2 for the second export of `twice` */
    int (*ping)(uint32_t* gen_out); /* any thread */
} probe_tab_t;

#define PROBE_MARKER 0x5EC0u

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static const char*               g_id;
static int64_t   g_probe_a, g_probe_b, g_probe_c;
static int       g_keyless_probe;
static char      g_consume[256];
static uint32_t  g_draws;
static const probe_tab_t* g_kept; /* the first provider's table at probe_a */

/* Provider: the admission gate and the state its shutdown hook frees. */
typedef struct { uint32_t gen; } prov_state_t;
static atomic_int   g_live;
static atomic_uint  g_inflight;
static prov_state_t* g_state;
static atomic_uint  g_refused; /* pings refused by the gate */

/* Reader thread. */
static pthread_t   g_reader_thread;
static int         g_reader_started;
static atomic_int  g_reader_stop;
static atomic_uint g_reader_polls, g_reader_hits, g_reader_pings, g_reader_retired;

/** @brief Log one probe line under this copy's id. */
#define SAY(...) do { char say_buf_[600]; snprintf(say_buf_, sizeof say_buf_, __VA_ARGS__); \
    g_api->log(g_self, OP_MOD_LOG_INFO, "svc-member %s: %s", g_id, say_buf_); } while (0)

/** @brief The exported function: report the provider's generation, or PING_RETIRED once the gate is closed.
 *
 * @param gen_out receives the generation.
 * @return PING_OK or PING_RETIRED. */
static int prov_ping(uint32_t* gen_out) {
    atomic_fetch_add(&g_inflight, 1);
    if (!atomic_load(&g_live)) {
        atomic_fetch_sub(&g_inflight, 1);
        atomic_fetch_add(&g_refused, 1);
        return PING_RETIRED;
    }
    *gen_out = g_state->gen; /* freed by the shutdown hook after the drain */
    atomic_fetch_sub(&g_inflight, 1);
    return PING_OK;
}

/** @brief Format a key as its mod's id (mod_info), or "?". */
static const char* key_id(uint32_t key) {
    openpete_mod_bus_sender_t rec = { sizeof rec, NULL, 0 };
    return g_api->mod_info(g_self, key, &rec) == 0 && rec.id ? rec.id : "?";
}

/** @brief Write svc_list of a service as "n id,id," into a buffer. */
static void list_ids(const char* name, uint32_t major, char* out, size_t cap) {
    uint32_t keys[16];
    const int n = g_api->svc_list(g_self, name, major, keys, 16);
    snprintf(out, cap, "%d ", n);
    for (int i = 0; i < n && i < 16; i++) {
        strncat(out, key_id(keys[i]), cap - strlen(out) - 2);
        strncat(out, ",", cap - strlen(out) - 1);
    }
}

/** @brief Look one provider's probe/1 up by id. */
static const probe_tab_t* get_tab(const char* id) {
    return (const probe_tab_t*)g_api->svc_get(g_self, openpete_mod_key_of(id), "probe", 1,
                                              sizeof(probe_tab_t));
}

/** @brief One probe: svc_list, svc_get and ping per consumed provider, the kept pointer's ping; probe A adds the lookup refusals. */
static void probe(const char* ph) {
    char buf[400];
    list_ids("probe", 1, buf, sizeof buf);
    SAY("R list_%s=%s", ph, buf);
    char ids[256];
    snprintf(ids, sizeof ids, "%s", g_consume);
    char gets[400] = "";
    const probe_tab_t* first = NULL;
    int first_done = 0;
    for (char* tok = strtok(ids, ","); tok; tok = strtok(NULL, ",")) {
        const probe_tab_t* t = get_tab(tok);
        char one[64];
        uint32_t gen = 0;
        if (!t) snprintf(one, sizeof one, "%s:-,", tok);
        else {
            const int rc = t->ping(&gen);
            snprintf(one, sizeof one, "%s:%d.%u%s,", tok, rc == PING_OK ? (int)gen : -rc,
                     t->variant, t->marker == PROBE_MARKER ? "" : "!marker");
        }
        strncat(gets, one, sizeof gets - strlen(gets) - 1);
        if (!first_done) { first = t; first_done = 1; }
    }
    SAY("R gets_%s=%s", ph, gets);
    if (!g_kept) {
        g_kept = first;
        SAY("R kept_%s=%d", ph, first != NULL);
    } else {
        uint32_t gen = 0;
        const int rc = g_kept->ping(&gen);
        SAY("R kept_%s=%d moved=%d", ph, rc, first != g_kept);
    }
    if (strcmp(ph, "a") != 0 || !first) return;
    char first_id[sizeof g_consume];
    snprintf(first_id, sizeof first_id, "%s", g_consume);
    char* comma = strchr(first_id, ',');
    if (comma) *comma = 0;
    const uint32_t k = openpete_mod_key_of(first_id);
    const probe_tab_t* again = get_tab(first_id);
    list_ids("probe", 2, buf, sizeof buf);
    SAY("R checks_a=other_major=%d min_size=%d key0=%d unknown=%d other_name=%d stable=%d major2=%s",
        g_api->svc_get(g_self, k, "probe", 3, 4) != NULL,
        g_api->svc_get(g_self, k, "probe", 1, sizeof(probe_tab_t) + 1) != NULL,
        g_api->svc_get(g_self, 0, "probe", 1, 4) != NULL,
        g_api->svc_get(g_self, openpete_mod_key_of("no-such-mod"), "probe", 1, 4) != NULL,
        g_api->svc_get(g_self, k, "probes", 1, 4) != NULL, again == first, buf);
    probe_tab_t x = { sizeof x, PROBE_MARKER, 9, prov_ping };
    SAY("R late_export=%d", g_api->svc_export(g_self, "late", 1, &x, sizeof x));
}

/** @brief The keyless probe: svc_get and svc_list, both refused for a keyless caller. */
static void keyless_probe(void) {
    uint32_t keys[4];
    SAY("R keyless_get=%d list=%d", g_api->svc_get(g_self, openpete_mod_key_of("p1"), "probe", 1, 4) != NULL,
        g_api->svc_list(g_self, "probe", 1, keys, 4));
}

/** @brief GamestateDraw override: the probes the config schedules, then base(). */
static void on_draw(CPUState* cpu) {
    g_draws++;
    if (g_probe_a && g_draws == (uint32_t)g_probe_a) {
        probe("a");
        if (g_keyless_probe) keyless_probe();
    }
    if (g_probe_b && g_draws == (uint32_t)g_probe_b) probe("b");
    if (g_probe_c && g_draws == (uint32_t)g_probe_c) probe("c");
    g_api->base(cpu);
}

/** @brief The reader thread: list probe/1, look every provider up and ping it, until the shutdown hook stops it. */
static void* reader_main(void* arg) {
    (void)arg;
    while (!atomic_load(&g_reader_stop)) {
        uint32_t keys[16];
        const int n = g_api->svc_list(g_self, "probe", 1, keys, 16);
        for (int i = 0; i < n && i < 16; i++) {
            const probe_tab_t* t = (const probe_tab_t*)g_api->svc_get(
                g_self, keys[i], "probe", 1, sizeof(probe_tab_t));
            if (!t || t->marker != PROBE_MARKER) continue;
            atomic_fetch_add(&g_reader_hits, 1);
            uint32_t gen = 0;
            if (t->ping(&gen) == PING_OK) atomic_fetch_add(&g_reader_pings, 1);
            else atomic_fetch_add(&g_reader_retired, 1);
        }
        atomic_fetch_add(&g_reader_polls, 1);
        usleep(100);
    }
    return NULL;
}

/** @brief Toggle hook: a provider's live flag follows its enabled state, with no drain. */
static void on_toggle(CPUState* cpu, int on) {
    (void)cpu;
    if (g_state) atomic_store(&g_live, on);
}

/** @brief Shutdown hook: stop the reader; a provider closes its gate, drains it, then frees its state. */
static void on_shutdown(void) {
    if (g_reader_started) {
        atomic_store(&g_reader_stop, 1);
        pthread_join(g_reader_thread, NULL);
        g_reader_started = 0;
        SAY("reader polls=%u hits=%u pings=%u retired=%u", atomic_load(&g_reader_polls),
            atomic_load(&g_reader_hits), atomic_load(&g_reader_pings), atomic_load(&g_reader_retired));
    }
    if (g_state) {
        atomic_store(&g_live, 0);
        while (atomic_load(&g_inflight)) usleep(50);
        SAY("shutdown gen=%u", g_state->gen);
        free(g_state);
        g_state = NULL;
    }
}

/** @brief Export probe/1 (and probe/2 with major2) from a local table the entry overwrites afterwards.
 *
 * @param api The engine table.
 * @param self This mod.
 * @return the OR of the export results. */
static int provide(const openpete_mod_api_t* api, openpete_mod_t* self) {
    probe_tab_t t = { sizeof t, PROBE_MARKER, 1, prov_ping };
    int rc = api->svc_export(self, "probe", 1, &t, sizeof t);
    if (api->config_bool(self, "twice", 0)) {
        t.variant = 2;
        rc |= api->svc_export(self, "probe", 1, &t, sizeof t);
    }
    if (api->config_bool(self, "major2", 0)) rc |= api->svc_export(self, "probe", 2, &t, sizeof t);
    memset(&t, 0, sizeof t); /* the engine holds its own copy */
    return rc;
}

/**
 * @brief Mod entry: reads the config; a provider exports its table, a consumer overrides GamestateDraw.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0, or 1 on a failed registration or with fail_entry.
 */
int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
    static char id[96];
    g_api = api; g_self = self;
    /* The id is the data_dir's parent directory name, which the rungs set
     * equal to the manifest id. */
    const char* data = api->data_dir(self);
    snprintf(id, sizeof id, "?");
    if (data) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "%s", data);
        char* slash = strrchr(tmp, '/');
        if (slash) { *slash = 0; slash = strrchr(tmp, '/'); }
        if (slash) snprintf(id, sizeof id, "%s", slash + 1);
    }
    g_id = id;
    g_probe_a = api->config_int(self, "probe_a", 0);
    g_probe_b = api->config_int(self, "probe_b", 0);
    g_probe_c = api->config_int(self, "probe_c", 0);
    g_keyless_probe = api->config_bool(self, "keyless_probe", 0);
    (void)api->config_str(self, "consume", "", g_consume, sizeof g_consume);
    if (api->register_shutdown_hook(self, on_shutdown) != 0) return 1;
    if (api->register_toggle_hook(self, on_toggle) != 0) return 1;
    if (api->config_bool(self, "provide", 0)) {
        g_state = calloc(1, sizeof *g_state);
        if (!g_state) return 1;
        g_state->gen = (uint32_t)api->config_int(self, "gen", 0);
        atomic_store(&g_live, 1);
        const int rc = provide(api, self);
        SAY("entry export=%d gen=%u", rc, g_state->gen);
        if (api->config_bool(self, "fail_entry", 0)) { SAY("entry failing"); return 1; }
        return 0;
    }
    char peer[64];
    if (api->config_str(self, "entry_get", "", peer, sizeof peer) && peer[0])
        SAY("R entry_get=%d", get_tab(peer) != NULL);
    if (g_keyless_probe) {
        probe_tab_t t = { sizeof t, PROBE_MARKER, 1, prov_ping };
        SAY("R keyless_export=%d", api->svc_export(self, "probe", 1, &t, sizeof t));
    } else if (g_consume[0]) {
        probe_tab_t t = { sizeof t, PROBE_MARKER, 1, prov_ping };
        SAY("R entry_refusals=%d,%d,%d,%d", api->svc_export(self, "", 1, &t, sizeof t),
            api->svc_export(self, "probe", 0, &t, sizeof t),
            api->svc_export(self, "probe", 1, NULL, sizeof t),
            api->svc_export(self, "probe", 1, &t, 0));
    }
    if (api->config_bool(self, "reader", 0))
        g_reader_started = pthread_create(&g_reader_thread, NULL, reader_main, NULL) == 0;
    return api->override_name(self, "GamestateDraw", on_draw);
}
