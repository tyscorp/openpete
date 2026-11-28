/* bus-member - one member of the bus rungs (see mod.toml). Each probe logs
 * one line, "bus-member <id>: <name> <value>...", that
 * tests/harness/mods_bus_rungs.sh reads. */

#include <openpete_mod_api.h>
#include <openpete_sdk_symbols.h> /* OP_FNADDR_VecCopy */
#include <psx_runtime.h>         /* CPUState */

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const openpete_mod_api_t* g_api;
static openpete_mod_t*           g_self;
static const char*               g_id;
static uint32_t g_draws;          /* GamestateDraw override entries */
static int64_t  g_send_at, g_selftest_at, g_stop, g_crash_at;
static int      g_neg, g_deep, g_fill, g_refusals, g_crash_handler, g_toggle_send;
static int      g_every_tick, g_attach_late, g_sender, g_no_override, g_log_links;
static CPUState* g_cpu;           /* the process's one CPUState, kept for the refusal probes */
static uint32_t g_pkt;            /* arena packet (order/1, regs/1) */
static uint8_t* g_pkt_host;
static uint32_t g_tick_blk;       /* arena block tick/1 advances */
static uint8_t* g_tick_host;
static int      g_deep_level;     /* deep/1 nesting reached */
static int      g_deep_result;    /* the result of the call that failed */
static int      g_reent_level;    /* reent/1 nesting reached */
static int      g_fill_depth;     /* VecCopy recursion depth in the fill probe */
static int      g_fill_mode, g_hcall_mode, g_shut_mode;
static int      g_fill_done, g_fill_pass, g_fill_result;
static int      g_hcall_ok;
static unsigned g_regs_writes;
static unsigned g_handler_runs;   /* handlers of this copy entered */
static int      g_in_handler;     /* a handler of this copy is running */
static pthread_t g_thread;
static int       g_thread_started;
static volatile int g_thread_rc = 99, g_thread_members = -99, g_thread_info = -99;
static int      g_park, g_park_control, g_fail_entry, g_reader, g_plant_race;
static int      g_park_inner;     /* the next GamestateDraw entry is the parking call: base() */
static uint32_t g_park_blk;       /* arena word: parks made, the per-tick register seed */
static uint8_t* g_park_host;
static uint32_t g_park_sp_in, g_park_sp_call; /* last logged sp at entry and at the api->call */
static unsigned g_park_walks, g_park_dups, g_park_second;
static uint32_t g_park_last_n;    /* members that appended to the previous walk's packet */
static uint8_t* g_heap;           /* freed by the shutdown hook; every handler writes it */
static unsigned g_after_shutdown; /* handler entries after the shutdown hook ran */
static int      g_shut_done;
static pthread_t g_reader_thread;
static int       g_reader_started;
static atomic_int g_reader_stop;
static atomic_uint g_reader_polls, g_reader_listed;
static unsigned  g_race;          /* plant_race: written by two threads, no lock */

#define PKT_KEYS 32
typedef struct {
    uint32_t n;
    uint32_t keys[PKT_KEYS];
    uint32_t mix;           /* order-dependent hash of the appenders */
    uint32_t vec_in[3], vec_out[3];
} order_pkt_t;

/** @brief Log one probe line under this copy's id. */
#define SAY(...) do { char say_buf_[400]; snprintf(say_buf_, sizeof say_buf_, __VA_ARGS__); \
    g_api->log(g_self, OP_MOD_LOG_INFO, "bus-member %s: %s", g_id, say_buf_); } while (0)

/** @brief Count a handler entry made after this object's shutdown hook, and write the heap block the hook frees. */
static void handler_enter(void) {
    if (g_shut_done) {
        g_after_shutdown++;
        g_api->log(g_self, OP_MOD_LOG_INFO, "bus-member %s: handler entered after the shutdown hook", g_id);
    }
    if (g_heap) g_heap[0]++;
}

/** @brief order/1: append this copy's key and fold it into the order hash; -7 when neg is set. */
static int h_order(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                   uint32_t msg, uint32_t size, void* user) {
    (void)cpu; (void)user;
    g_handler_runs++;
    handler_enter();
    if (size >= sizeof(order_pkt_t)) {
        order_pkt_t* p = (order_pkt_t*)g_api->guest(msg);
        const uint32_t k = g_api->mod_key(self);
        if (p && p->n < PKT_KEYS) p->keys[p->n++] = k;
        if (p) p->mix = p->mix * 31u + (k & 0xFFu);
    }
    if (g_draws == (uint32_t)g_send_at || g_toggle_send)
        SAY("handler order/1 from=%s", from && from->id ? from->id : "?");
    return g_neg ? -7 : OP_BUS_PASS;
}

/** @brief stop/1: return the configured stop value, logging the run. */
static int h_stop(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                  uint32_t msg, uint32_t size, void* user) {
    (void)self; (void)cpu; (void)from; (void)msg; (void)size; (void)user;
    g_handler_runs++;
    handler_enter();
    SAY("handler stop/1 ran returns=%lld", (long long)g_stop);
    return (int)g_stop;
}

/** @brief tick/1: advance this copy's own arena counter by the sender's tick word. */
static int h_tick(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                  uint32_t msg, uint32_t size, void* user) {
    (void)self; (void)cpu; (void)user; (void)from;
    g_handler_runs++;
    handler_enter();
    const uint32_t* w = size >= 4 ? (const uint32_t*)g_api->guest(msg) : NULL;
    if (g_tick_host && w) {
        uint32_t c; memcpy(&c, g_tick_host, 4);
        c = c * 1664525u + *w + 1013904223u;
        memcpy(g_tick_host, &c, 4);
    }
    return OP_BUS_PASS;
}

/** @brief deep/1: send deep/1 again from inside the handler until a call fails, recording the depth and the failing result. */
static int h_deep(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                  uint32_t msg, uint32_t size, void* user) {
    (void)from; (void)user;
    g_handler_runs++;
    handler_enter();
    if (!g_deep) return OP_BUS_PASS; /* only the nesting copy recurses */
    g_deep_level++;
    const int rc = g_api->bus_call(self, cpu, "deep/1", msg, size);
    if (rc < 0 && !g_deep_result) g_deep_result = rc;
    return OP_BUS_PASS;
}

/** @brief reent/1: one reentry to depth 2, recording the depth reached. */
static int h_reent(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                   uint32_t msg, uint32_t size, void* user) {
    (void)from; (void)user;
    g_handler_runs++;
    handler_enter();
    if (!g_deep) return OP_BUS_PASS;
    g_reent_level++;
    if (g_reent_level == 1) (void)g_api->bus_call(self, cpu, "reent/1", msg, size);
    return OP_BUS_PASS;
}

/** @brief regs/1: write every saved register, GTE, and a stack argument, call VecCopy through api->call (its override calls base()), and try base(). */
static int h_regs(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                  uint32_t msg, uint32_t size, void* user) {
    (void)self; (void)from; (void)size; (void)user;
    g_handler_runs++;
    handler_enter();
    g_in_handler = 1;
    order_pkt_t* p = (order_pkt_t*)g_api->guest(msg);
    uint32_t* r = &cpu->at;
    for (int i = 0; i < 15; i++) { r[i] ^= 0xA5A50000u + (uint32_t)i; g_regs_writes++; }
    cpu->t8 ^= 1u; cpu->t9 ^= 2u; cpu->hi ^= 3u; cpu->lo ^= 4u; g_regs_writes += 4;
    for (int i = 0; i < 32; i++) {
        cpu->gte_data[i] ^= 0x5A000000u + (uint32_t)i;
        cpu->gte_ctrl[i] ^= 0x3C000000u + (uint32_t)i;
        g_regs_writes += 2;
    }
    cpu->write_word(cpu->sp + 16, 0xDEADBEEFu); /* a fifth argument */
    cpu->write_word(cpu->sp + 20, 0xFEEDFACEu);
    g_regs_writes += 2;
    cpu->sp -= 32;                               /* handler's own frame */
    g_regs_writes++;
    if (p) {
        p->vec_in[0] = 11; p->vec_in[1] = 22; p->vec_in[2] = 33;
        memset(p->vec_out, 0, sizeof p->vec_out);
        cpu->a0 = msg + (uint32_t)offsetof(order_pkt_t, vec_out);
        cpu->a1 = msg + (uint32_t)offsetof(order_pkt_t, vec_in);
        g_hcall_mode = 1;
        g_api->call(cpu, OP_FNADDR_VecCopy);
        g_api->base(cpu); /* refused: a bus handler has no chain */
    }
    g_in_handler = 0;
    return OP_BUS_PASS;
}

/** @brief frames/1: pass. */
static int h_frames(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                    uint32_t msg, uint32_t size, void* user) {
    (void)self; (void)cpu; (void)from; (void)msg; (void)size; (void)user;
    g_handler_runs++;
    handler_enter();
    return OP_BUS_PASS;
}

/** @brief crash/1: fault when crash_handler is set. */
static int h_crash(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                   uint32_t msg, uint32_t size, void* user) {
    (void)self; (void)cpu; (void)from; (void)msg; (void)size; (void)user;
    g_handler_runs++;
    handler_enter();
    if (g_crash_handler) {
        SAY("crash handler faulting");
        volatile int* volatile np = NULL;
        *np = 1;
    }
    return OP_BUS_PASS;
}

static const uint32_t g_veccopy = OP_FNADDR_VecCopy;

/** @brief Seed the caller-saved registers from the arena park count, so they differ per tick and roll back with the arena. */
static void park_seed(CPUState* cpu) {
    uint32_t c = 0;
    if (g_park_host) { memcpy(&c, g_park_host, 4); c++; memcpy(g_park_host, &c, 4); }
    uint32_t* r = &cpu->at;
    for (int i = 7; i < 15; i++) r[i] = c * 16u + (uint32_t)i; /* t0-t7 */
}

/** @brief Call GamestateDraw through api->call with the next entry marked as the parking one, logging sp when it changes. */
static void park_call(CPUState* cpu, uint32_t sp_in) {
    park_seed(cpu);
    if (cpu->sp != g_park_sp_call || sp_in != g_park_sp_in) {
        g_park_sp_in = sp_in; g_park_sp_call = cpu->sp;
        SAY("park sp_in=%08X sp_call=%08X", sp_in, cpu->sp);
    }
    g_park_inner = 1;
    g_api->call(cpu, OP_FNADDR_GamestateDraw);
}

/** @brief park/1: append this copy's key; the parking copy then calls GamestateDraw, whose original parks in VSync. */
static int h_park(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                  uint32_t msg, uint32_t size, void* user) {
    (void)from; (void)user;
    g_handler_runs++;
    handler_enter();
    order_pkt_t* p = size >= sizeof(order_pkt_t) ? (order_pkt_t*)g_api->guest(msg) : NULL;
    if (p && p->n < PKT_KEYS) p->keys[p->n++] = g_api->mod_key(self);
    if (g_park) park_call(cpu, p ? p->vec_in[0] : 0);
    return OP_BUS_PASS;
}

/** @brief The sender side of park and park_control: one parking GamestateDraw call per call, then a second send that must pass.
 *
 * park sends park/1 (stack: this override, the bus frame, this override
 * again); park_control makes the same call directly inside the walker's
 * saved set and sp reserve. Each walk's packet is checked for a key
 * appended twice. */
static void park_draw(CPUState* cpu) {
    if (g_park) {
        order_pkt_t* p = (order_pkt_t*)g_pkt_host;
        memset(p, 0, sizeof *p);
        p->vec_in[0] = cpu->sp;
        (void)g_api->bus_call(g_self, cpu, "park/1", g_pkt, sizeof *p);
        g_park_walks++;
        if (p->n != g_park_last_n) {
            SAY("park members %u -> %u at walk %u", g_park_last_n, p->n, g_park_walks);
            g_park_last_n = p->n;
        }
        for (uint32_t i = 0; i < p->n; i++)
            for (uint32_t j = i + 1; j < p->n; j++)
                if (p->keys[i] == p->keys[j]) { g_park_dups++; i = j = p->n; }
    } else {
        /* The packet bytes the bus run's sender and handler write. */
        order_pkt_t* p = (order_pkt_t*)g_pkt_host;
        memset(p, 0, sizeof *p);
        p->vec_in[0] = cpu->sp;
        p->keys[p->n++] = g_api->mod_key(g_self);
        uint32_t save[15 + 5 + 64];
        memcpy(save, &cpu->at, 15 * 4);
        save[15] = cpu->t8; save[16] = cpu->t9; save[17] = cpu->hi; save[18] = cpu->lo;
        save[19] = cpu->sp;
        memcpy(save + 20, cpu->gte_data, 32 * 4);
        memcpy(save + 52, cpu->gte_ctrl, 32 * 4);
        const uint32_t sp_in = cpu->sp;
        cpu->sp = (cpu->sp - 64u) & ~7u;
        park_call(cpu, sp_in);
        memcpy(&cpu->at, save, 15 * 4);
        cpu->t8 = save[15]; cpu->t9 = save[16]; cpu->hi = save[17]; cpu->lo = save[18];
        cpu->sp = save[19];
        memcpy(cpu->gte_data, save + 20, 32 * 4);
        memcpy(cpu->gte_ctrl, save + 52, 32 * 4);
        g_park_walks++;
    }
    if (g_api->bus_call(g_self, cpu, "frames/1", 0, 0) == OP_BUS_PASS) g_park_second++;
}

/** @brief VecCopy override: base() normally; in the fill probe, recurse until bus_call reports the frame stack full; reached from the shutdown hook, try a bus call. */
static void on_veccopy(CPUState* cpu) {
    if (g_shut_mode) {
        g_shut_mode = 0;
        SAY("shutdown-override bus_call=%d", g_api->bus_call(g_self, cpu, "order/1", 0, 0));
        return;
    }
    if (g_hcall_mode) {
        g_hcall_mode = 0;
        g_api->base(cpu);
        g_hcall_ok = 1;
        return;
    }
    if (!g_fill_mode) { g_api->base(cpu); return; }
    g_fill_depth++;
    const int rc = g_api->bus_call(g_self, cpu, "frames/1", 0, 0);
    if (rc == OP_BUS_PASS) g_fill_pass++;
    if (rc != OP_BUS_PASS) { g_fill_done = 1; g_fill_result = rc; return; }
    if (g_fill_depth < 64) g_api->call(cpu, g_veccopy);
}

/** @brief Record a saved-set snapshot of the CPU state and the two stack words at sp+16. */
typedef struct { uint32_t r[15], t8, t9, hi, lo, sp, gd[32], gc[32], s16, s20; } regs_t;
static void regs_take(regs_t* s, CPUState* cpu) {
    memcpy(s->r, &cpu->at, sizeof s->r);
    s->t8 = cpu->t8; s->t9 = cpu->t9; s->hi = cpu->hi; s->lo = cpu->lo; s->sp = cpu->sp;
    memcpy(s->gd, cpu->gte_data, sizeof s->gd);
    memcpy(s->gc, cpu->gte_ctrl, sizeof s->gc);
    s->s16 = cpu->read_word(cpu->sp + 16);
    s->s20 = cpu->read_word(cpu->sp + 20);
}

/** @brief The result battery, from the sender's GamestateDraw override. */
static void selftest(CPUState* cpu) {
    order_pkt_t* p = (order_pkt_t*)g_pkt_host;
    memset(p, 0, sizeof *p);
    SAY("R pass=%d", g_api->bus_call(g_self, cpu, "order/1", g_pkt, sizeof *p));
    SAY("R stop=%d", g_api->bus_call(g_self, cpu, "stop/1", 0, 0));
    SAY("R none_unattached=%d", g_api->bus_call(g_self, cpu, "nobody/1", 0, 0));
    SAY("R none_misspelt=%d", g_api->bus_call(g_self, cpu, "ordr/1", 0, 0));
    SAY("R malformed=%d", g_api->bus_call(g_self, cpu, "order", 0, 0));
    SAY("R range=%d", g_api->bus_call(g_self, cpu, "order/1", 0x80000000u + 0x1FFFF0u, 64));
    SAY("R mod_key=%08X sdk=%08X", g_api->mod_key(g_self), openpete_mod_key_of(g_id));
    openpete_mod_bus_sender_t rec = { sizeof rec, NULL, 0 };
    const int info = g_api->mod_info(g_self, g_api->mod_key(g_self), &rec);
    SAY("R mod_info_self=%d id=%s", info, rec.id ? rec.id : "-");
    openpete_mod_bus_sender_t m[16];
    m[0].struct_size = sizeof m[0];
    const int n = g_api->bus_members(g_self, "order/1", m, 16);
    char list[300] = "";
    for (int i = 0; i < n && i < 16; i++) {
        strncat(list, m[i].id ? m[i].id : "?", sizeof list - strlen(list) - 2);
        strncat(list, ",", sizeof list - strlen(list) - 1);
    }
    SAY("R members=%d %s", n, list);
    for (int i = 0; i < n && i < 16; i++) {
        if (strcmp(m[i].id, g_id) == 0) continue;
        memset(p, 0, sizeof *p);
        const int rc = g_api->bus_call_to(g_self, cpu, "order/1", m[i].key, g_pkt, sizeof *p);
        SAY("R call_to %s=%d appended=%u key_ok=%d", m[i].id, rc, p->n,
            p->n == 1 && p->keys[0] == m[i].key);
        break;
    }
    SAY("R call_to_self=%d", g_api->bus_call_to(g_self, cpu, "order/1", g_api->mod_key(g_self), g_pkt, sizeof *p));
    SAY("R call_to_unloaded=%d", g_api->bus_call_to(g_self, cpu, "order/1", openpete_mod_key_of("no-such-mod"), 0, 0));
    SAY("R call_to_zero=%d", g_api->bus_call_to(g_self, cpu, "order/1", 0, 0, 0));
    char peer[64];
    if (g_api->config_str(g_self, "probe_disabled", "", peer, sizeof peer) && peer[0])
        SAY("R call_to_disabled=%d", g_api->bus_call_to(g_self, cpu, "order/1", openpete_mod_key_of(peer), 0, 0));
    if (g_api->config_str(g_self, "probe_unattached", "", peer, sizeof peer) && peer[0])
        SAY("R call_to_never_attached=%d", g_api->bus_call_to(g_self, cpu, "nobody/1", openpete_mod_key_of(peer), 0, 0));
    if (g_deep) {
        g_deep_level = 0; g_deep_result = 0;
        const int rc = g_api->bus_call(g_self, cpu, "deep/1", 0, 0);
        SAY("R deep levels=%d failed_with=%d outer=%d", g_deep_level, g_deep_result, rc);
    }
    g_reent_level = 0;
    const int reent = g_api->bus_call(g_self, cpu, "reent/1", 0, 0);
    SAY("R reent=%d levels=%d", reent, g_reent_level);
    regs_t before, after;
    regs_take(&before, cpu);
    g_regs_writes = 0; g_hcall_ok = 0;
    const int rr = g_api->bus_call_to(g_self, cpu, "regs/1", g_api->mod_key(g_self), g_pkt, sizeof *p);
    regs_take(&after, cpu);
    SAY("R regs=%d unchanged=%d handler_writes=%u hcall_base_ran=%d vec=%u,%u,%u", rr,
        memcmp(&before, &after, sizeof before) == 0, g_regs_writes, g_hcall_ok,
        p->vec_out[0], p->vec_out[1], p->vec_out[2]);
    if (g_fill) {
        g_fill_mode = 1; g_fill_depth = 0; g_fill_done = 0; g_fill_pass = 0; g_fill_result = 0;
        g_api->call(cpu, g_veccopy);
        g_fill_mode = 0;
        SAY("R fill depth=%d passes=%d result=%d", g_fill_depth, g_fill_pass, g_fill_result);
    }
}

/** @brief Present hook: a bus call with the kept CPUState is refused. */
static void on_present(const openpete_present_ctx_t* ctx) {
    (void)ctx;
    static int done;
    if (done || !g_cpu) return;
    done = 1;
    SAY("present bus_call=%d members=%d", g_api->bus_call(g_self, g_cpu, "order/1", 0, 0),
        g_api->bus_members(g_self, "order/1", NULL, 0));
}

/** @brief A thread of this mod: bus_call refused, bus_members and mod_info answered. */
static void* thread_main(void* arg) {
    (void)arg;
    for (int i = 0; i < 2000 && !g_cpu; i++) usleep(1000);
    if (!g_cpu) return NULL;
    openpete_mod_bus_sender_t rec = { sizeof rec, NULL, 0 };
    g_thread_rc = g_api->bus_call(g_self, g_cpu, "order/1", 0, 0);
    g_thread_members = g_api->bus_members(g_self, "order/1", NULL, 0);
    g_thread_info = g_api->mod_info(g_self, g_api->mod_key(g_self), &rec);
    return NULL;
}

/** @brief The reader thread: poll bus_members and mod_info for every listed key until the shutdown hook stops it. */
static void* reader_main(void* arg) {
    (void)arg;
    while (!atomic_load(&g_reader_stop)) {
        openpete_mod_bus_sender_t m[16];
        m[0].struct_size = sizeof m[0];
        const int n = g_api->bus_members(g_self, "order/1", m, 16);
        for (int i = 0; i < n && i < 16; i++) {
            openpete_mod_bus_sender_t rec = { sizeof rec, NULL, 0 };
            if (g_api->mod_info(g_self, m[i].key, &rec) == 0 && rec.id && strcmp(rec.id, m[i].id) == 0)
                atomic_fetch_add(&g_reader_listed, 1);
        }
        if (g_plant_race) g_race++;
        atomic_fetch_add(&g_reader_polls, 1);
        usleep(200);
    }
    return NULL;
}

/** @brief GamestateDraw override: the probes the config schedules, then base(). */
static void on_draw(CPUState* cpu) {
    if (g_park_inner) { g_park_inner = 0; g_api->base(cpu); return; }
    g_draws++;
    g_cpu = cpu;
    if (g_plant_race) g_race++;
    if (g_park || g_park_control) { park_draw(cpu); return; }
    if (g_attach_late && g_draws == 1)
        SAY("late attach=%d", g_api->bus_attach(g_self, "late/1", h_frames, NULL));
    if (g_log_links) SAY("link GamestateDraw");
    if (g_sender && g_send_at && g_draws == (uint32_t)g_send_at) {
        order_pkt_t* p = (order_pkt_t*)g_pkt_host;
        memset(p, 0, sizeof *p);
        const int rc = g_api->bus_call(g_self, cpu, "order/1", g_pkt, sizeof *p);
        char ids[600] = "";
        for (uint32_t i = 0; i < p->n; i++) {
            openpete_mod_bus_sender_t rec = { sizeof rec, NULL, 0 };
            (void)g_api->mod_info(g_self, p->keys[i], &rec);
            strncat(ids, rec.id ? rec.id : "?", sizeof ids - strlen(ids) - 2);
            strncat(ids, ",", sizeof ids - strlen(ids) - 1);
        }
        SAY("walk rc=%d n=%u mix=%08X ids=%s", rc, p->n, p->mix, ids);
    }
    if (g_selftest_at && g_draws == (uint32_t)g_selftest_at) selftest(cpu);
    if (g_crash_at && g_draws == (uint32_t)g_crash_at)
        (void)g_api->bus_call(g_self, cpu, "crash/1", 0, 0);
    if (g_every_tick && g_pkt_host && g_tick_host) {
        uint32_t w; /* from the arena, so a replayed tick sends the same word */
        memcpy(&w, g_tick_host, 4);
        memcpy(g_pkt_host, &w, 4);
        (void)g_api->bus_call(g_self, cpu, "tick/1", g_pkt, 4);
    }
    g_api->base(cpu);
}

/** @brief Toggle hook: with toggle_send, send order/1 and list the members. */
static void on_toggle(CPUState* cpu, int on) {
    if (!g_toggle_send) return;
    order_pkt_t* p = (order_pkt_t*)g_pkt_host;
    memset(p, 0, sizeof *p);
    const int rc = g_api->bus_call(g_self, cpu, "order/1", g_pkt, sizeof *p);
    openpete_mod_bus_sender_t m[16];
    m[0].struct_size = sizeof m[0];
    const int n = g_api->bus_members(g_self, "order/1", m, 16);
    char list[300] = "";
    int self_listed = 0;
    for (int i = 0; i < n && i < 16; i++) {
        strncat(list, m[i].id, sizeof list - strlen(list) - 2);
        strncat(list, ",", sizeof list - strlen(list) - 1);
        if (strcmp(m[i].id, g_id) == 0) self_listed = 1;
    }
    SAY("toggle on=%d send=%d appended=%u members=%d self_listed=%d list=%s",
        on, rc, p->n, n, self_listed, list);
}

/** @brief Shutdown hook: the counts, then bus calls with the kept CPUState (refused) and through an override reached from here. */
static void on_shutdown(void) {
    SAY("shutdown draws=%u handler_runs=%u gen_live=1", g_draws, g_handler_runs);
    if (g_park || g_park_control)
        SAY("park walks=%u dups=%u second_pass=%u", g_park_walks, g_park_dups, g_park_second);
    if (g_reader_started) {
        atomic_store(&g_reader_stop, 1);
        pthread_join(g_reader_thread, NULL);
        g_reader_started = 0;
        SAY("reader polls=%u listed=%u", atomic_load(&g_reader_polls), atomic_load(&g_reader_listed));
    }
    SAY("after_shutdown=%u", g_after_shutdown);
    g_shut_done = 1;
    free(g_heap); /* left dangling: a handler entry after this is a use after free */
    if (g_refusals && g_cpu) {
        SAY("shutdown bus_call=%d", g_api->bus_call(g_self, g_cpu, "order/1", 0, 0));
        g_shut_mode = 1;
        g_api->call(g_cpu, g_veccopy);
        g_shut_mode = 0;
        if (g_thread_started) {
            pthread_join(g_thread, NULL);
            SAY("thread bus_call=%d members=%d mod_info=%d", g_thread_rc, g_thread_members,
                g_thread_info);
        }
    }
    if (g_tick_host) {
        uint32_t c; memcpy(&c, g_tick_host, 4);
        SAY("tick counter=%08X", c);
    }
}

/**
 * @brief Mod entry: reads the config, allocates the arena packet, attaches the handlers and the overrides.
 * @param api The engine table.
 * @param self This mod's handle.
 * @return 0, or 1 on a failed registration.
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
    g_send_at       = api->config_int(self, "send_at", 0);
    g_selftest_at   = api->config_int(self, "selftest_at", 0);
    g_stop          = api->config_int(self, "stop", 0);
    g_crash_at      = api->config_int(self, "crash_at", 0);
    g_neg           = api->config_bool(self, "neg", 0);
    g_deep          = api->config_bool(self, "deep", 0);
    g_fill          = api->config_bool(self, "fill", 0);
    g_refusals      = api->config_bool(self, "refusals", 0);
    g_crash_handler = api->config_bool(self, "crash_handler", 0);
    g_toggle_send   = api->config_bool(self, "toggle_send", 0);
    g_every_tick    = api->config_bool(self, "every_tick", 0);
    g_attach_late   = api->config_bool(self, "attach_late", 0);
    g_sender        = api->config_bool(self, "sender", 0);
    g_no_override   = api->config_bool(self, "no_override", 0);
    g_log_links     = api->config_bool(self, "log_links", 0);
    g_park          = api->config_bool(self, "park", 0);
    g_park_control  = api->config_bool(self, "park_control", 0);
    g_fail_entry    = api->config_bool(self, "fail_entry", 0);
    g_reader        = api->config_bool(self, "reader", 0);
    g_plant_race    = api->config_bool(self, "plant_race", 0);
    g_heap = calloc(1, 64);
    void* host = NULL;
    g_pkt = api->guest_alloc(self, sizeof(order_pkt_t), 4, 0, &host);
    g_pkt_host = (uint8_t*)host;
    g_tick_blk = api->guest_alloc(self, 4, 4, 0, &host);
    g_tick_host = (uint8_t*)host;
    g_park_blk = api->guest_alloc(self, 4, 4, 0, &host);
    g_park_host = (uint8_t*)host;
    SAY("entry key=%08X gen=%lld entry_bus_call=%d", api->mod_key(self),
        (long long)api->config_int(self, "gen", 0), api->bus_call(self, NULL, "order/1", 0, 0));
    int rc = 0;
    rc |= api->bus_attach(self, "order/1", h_order, NULL);
    rc |= api->bus_attach(self, "order/1", h_order, NULL); /* replaced in place */
    rc |= api->bus_attach(self, "stop/1", h_stop, NULL);
    rc |= api->bus_attach(self, "tick/1", h_tick, NULL);
    rc |= api->bus_attach(self, "deep/1", h_deep, NULL);
    rc |= api->bus_attach(self, "reent/1", h_reent, NULL);
    rc |= api->bus_attach(self, "regs/1", h_regs, NULL);
    rc |= api->bus_attach(self, "frames/1", h_frames, NULL);
    rc |= api->bus_attach(self, "crash/1", h_crash, NULL);
    rc |= api->bus_attach(self, "park/1", h_park, NULL);
    SAY("attach rc=%d", rc);
    if (g_fail_entry) { SAY("entry failing"); return 1; }
    if (api->register_shutdown_hook(self, on_shutdown) != 0) return 1;
    if (api->register_toggle_hook(self, on_toggle) != 0) return 1;
    if (g_refusals) {
        if (api->register_present_hook(self, on_present) != 0) return 1;
        g_thread_started = pthread_create(&g_thread, NULL, thread_main, NULL) == 0;
    }
    if (g_reader) g_reader_started = pthread_create(&g_reader_thread, NULL, reader_main, NULL) == 0;
    if (g_no_override) return 0; /* a bus-only member: handlers and no link */
    if (api->override_name(self, "VecCopy", on_veccopy) != 0) return 1;
    return api->override_name(self, "GamestateDraw", on_draw);
}
