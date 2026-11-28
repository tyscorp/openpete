/* tests/unit/mod_bus_test.c - the bus walker, its snapshot and the key table
 * of core/mods/mod_bus.c against stub mods, with the override frame stack
 * of core/mods/mod_chain.c (both included whole). Walk order follows the
 * entry sequence across a re-entry; pass, stop, OP_BUS_NONE, a negative
 * result and bus_call_to by key; the depth cap and a full frame stack give
 * OP_BUS_DEPTH; the saved register set and the 64-byte sp reserve; base()
 * refused in a handler; the context refusals of every site kind, the role
 * mask and the present flag, the entry-only bus_attach, sim_mutative =
 * false and keyless mods; a member disabled, retired, or first entered
 * while a walk is in progress runs at most once in that walk and never
 * after its retirement; bus_members and mod_info record sizes; a reader
 * thread polling bus_members and mod_info across publishes (a
 * ThreadSanitizer build of this test checks the snapshot handoff). The
 * service tables of core/mods/mod_svc.c (included whole) ride the same
 * snapshot: svc_export at entry only, copied and replaced in place,
 * svc_get and svc_list in provider order across a toggle, a strip and a
 * re-export, their refusals, and a reader thread looking tables up across
 * publishes. The host services the modules call are stubs. */

#include "../../core/mods/mod_chain.c"
#include "../../core/mods/mod_bus.c"
#include "../../core/mods/mod_svc.c"
#include "psx_runtime.h" /* CPUState */

#include <pthread.h>
#include <time.h>
#include <sched.h>

/* ---- stubs ------------------------------------------------------------- */

static game_mod_desc_t s_game; /* window [0, 0): no overlay filter */
const game_mod_desc_t* openpete_mods_game_desc(void) { return &s_game; }
uint32_t (*feature_registry_overlay_probe(void))(void) { return NULL; }
static const char* s_hook_owner;
const char* openpete_imgui_guard_hook_owner(void) { return s_hook_owner; }
void openpete_imgui_guard_set_hook_owner(const char* id) { s_hook_owner = id; }
void openpete_mods_link_registered(const char* owner_id, uint32_t addr, uint32_t overlay) {
    (void)owner_id; (void)addr; (void)overlay;
}
op_time_t op_now(void) { return 0; }
SDL_ThreadID SDL_GetCurrentThreadID(void) { return 1; }
OPENPETE_THREAD_LOCAL int openpete_mods_in_present;
int openpete_speculative;
int openpete_replaying;
static int s_no_sim; /* nonzero: the calling thread holds no sim role */
unsigned op_role_mask(void) { return s_no_sim ? 0u : (unsigned)OP_ROLE_BIT_sim; }
void op_role__assert_sim(op_role_site_t* site) { (void)site; }

#define RAM_BASE 0x80010000u
static uint8_t s_ram[0x10000];
void* openpete_psx_to_host_ptr(uint32_t addr) {
    if (addr >= RAM_BASE && addr < RAM_BASE + sizeof s_ram) return &s_ram[addr - RAM_BASE];
    return NULL;
}

static int s_orig;
/** @brief The dispatch layer: the body runs when no override serviced the call. */
void call_by_address(CPUState* cpu, uint32_t addr) {
    if (!openpete_mods_dispatch(cpu, addr)) s_orig++;
}

/* The loader's mod record, as far as the bus reads it. */
struct openpete_mod {
    const char* id;
    _Atomic int enabled;
    _Atomic int closed;
    _Atomic uint32_t retired;
    uint32_t key;
    int neutral;
    char tag[40];
    int runs; /* handler entries */
};
int mod_owner_enabled(const openpete_mod_t* m) { return atomic_load(&m->enabled); }
int mod_owner_closed(const openpete_mod_t* m) { return atomic_load(&m->closed); }
uint32_t mod_owner_retired(const openpete_mod_t* m) { return atomic_load(&m->retired); }
uint32_t mod_owner_key(const openpete_mod_t* m) { return m->key; }
int mod_owner_sim_neutral(const openpete_mod_t* m) { return m->neutral; }
const char* mod_owner_tag(const openpete_mod_t* m) { return m->tag; }

static int s_fail;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_fail = 1; } } while (0)

#define N_MODS 12
static struct openpete_mod s_m[N_MODS];
static const char* const k_ids[N_MODS] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l"};

/** @brief Seal the key table once with every stub mod's key. */
static void keys_once(void) {
    static int done;
    if (done) return;
    done = 1;
    for (int i = 0; i < N_MODS; i++) {
        s_m[i].id = k_ids[i];
        s_m[i].key = openpete_mod_key_of(k_ids[i]);
        snprintf(s_m[i].tag, sizeof s_m[i].tag, "mod:%s", k_ids[i]);
        (void)openpete_mods_keys_add(k_ids[i], s_m[i].key);
    }
    openpete_mods_keys_seal();
}

/* The walk log: owner indices in the order their handlers ran. */
static int s_log[64], s_log_n;
static int s_stop_at = -1;   /* index whose handler returns 9 */
static int s_neg_at = -1;    /* index whose handler returns -5 */

/** @brief Record a handler run of its owner, and answer per the case's settings. */
static int h_log(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                 uint32_t msg, uint32_t size, void* user) {
    (void)cpu; (void)from; (void)msg; (void)size; (void)user;
    const int i = (int)(self - s_m);
    self->runs++;
    if (s_log_n < 64) s_log[s_log_n++] = i;
    if (i == s_stop_at) return 9;
    if (i == s_neg_at) return -5;
    return OP_BUS_PASS;
}

/** @brief Reset the stub mods (all enabled), the bus table, the frames and the site; seal the keys on first use. */
static void reset(void) {
    keys_once();
    for (int i = 0; i < N_MODS; i++) {
        openpete_mods_bus_remove_owner(&s_m[i]);
        openpete_mods_svc_remove_owner(&s_m[i]);
        atomic_store(&s_m[i].enabled, 1);
        atomic_store(&s_m[i].closed, 0);
        s_m[i].neutral = 0;
        s_m[i].runs = 0;
        s_m[i].key = openpete_mod_key_of(k_ids[i]);
    }
    s_frame_depth = 0;
    s_site.ctx = OPENPETE_MODS_CTX_GUEST;
    s_site.depth = 0;
    s_site.tick = 0;
    s_log_n = 0;
    s_stop_at = s_neg_at = -1;
    s_no_sim = 0;
    openpete_mods_in_present = 0;
    openpete_mods_bus_publish();
}

/** @brief Enter one stub mod: open an entry site, attach its handler to each bus, publish. */
static void enter(int i, const char* bus, openpete_mod_bus_fn fn) {
    const openpete_mods_site_t prev = openpete_mods_site_open(OPENPETE_MODS_CTX_ENTRY);
    CHECK(openpete_mods_bus_attach(&s_m[i], bus, fn, NULL) == 0, "attach %s", k_ids[i]);
    openpete_mods_site_close(prev);
    openpete_mods_bus_publish();
}

/** @brief Strip a stub mod as mod_strip_registrations does: remove, retire, publish. */
static void strip(int i) {
    openpete_mods_bus_remove_owner(&s_m[i]);
    openpete_mods_svc_remove_owner(&s_m[i]);
    atomic_fetch_add(&s_m[i].retired, 1u);
    openpete_mods_bus_publish();
}

static CPUState s_cpu;
static int s_in_override; /* the override below is running */
static int (*s_override_body)(CPUState* cpu);
static int s_override_rc;

/** @brief An override link that runs the case's body, so the call has an override frame of GUEST's. */
static void link_body(CPUState* cpu) { s_in_override = 1; s_override_rc = s_override_body(cpu); s_in_override = 0; }

#define ADDR_O 0x80020000u
/** @brief Run a body inside an override frame, as a send from an override does, and return its result. */
static int from_override(int (*body)(CPUState* cpu)) {
    static int linked;
    if (!linked) { (void)openpete_mods_chain_add(ADDR_O, link_body, "o"); linked = 1; }
    s_override_body = body;
    s_override_rc = -99;
    (void)openpete_mods_dispatch(&s_cpu, ADDR_O);
    return s_override_rc;
}

/** @brief Format the walk log as owner ids. */
static const char* log_str(void) {
    static char buf[200];
    buf[0] = 0;
    for (int i = 0; i < s_log_n; i++) strncat(buf, k_ids[s_log[i]], sizeof buf - strlen(buf) - 1);
    return buf;
}

static int body_call_order(CPUState* cpu) { return openpete_mods_bus_call(&s_m[0], cpu, "order/1", 0, 0); }

/** @brief Walk order is the entry sequence, newest first; a re-entry moves the mod to the front; a second attach keeps the position. */
static void case_order(void) {
    reset();
    enter(0, "order/1", h_log);
    enter(1, "order/1", h_log);
    enter(2, "order/1", h_log);
    CHECK(from_override(body_call_order) == OP_BUS_PASS, "pass");
    CHECK(strcmp(log_str(), "cba") == 0, "boot order %s", log_str());
    s_log_n = 0;
    enter(2, "order/1", h_log); /* second attach: replaced in place */
    (void)from_override(body_call_order);
    CHECK(strcmp(log_str(), "cba") == 0, "second attach moved the handler: %s", log_str());
    s_log_n = 0;
    strip(1);                   /* a hot reload of b */
    enter(1, "order/1", h_log);
    (void)from_override(body_call_order);
    CHECK(strcmp(log_str(), "bca") == 0, "reload order %s", log_str());
    openpete_mod_bus_sender_t m[8];
    m[0].struct_size = sizeof m[0];
    CHECK(openpete_mods_bus_members(&s_m[0], "order/1", m, 8) == 3 &&
          strcmp(m[0].id, "b") == 0 && strcmp(m[2].id, "a") == 0 && m[0].key == s_m[1].key,
          "members in walk order");
}

static int body_stop(CPUState* cpu) { return openpete_mods_bus_call(&s_m[0], cpu, "order/1", 0, 0); }
static int body_none(CPUState* cpu) { return openpete_mods_bus_call(&s_m[0], cpu, "nobody/1", 0, 0); }
static int body_misspelt(CPUState* cpu) { return openpete_mods_bus_call(&s_m[0], cpu, "ordr/1", 0, 0); }
static int body_malformed(CPUState* cpu) { return openpete_mods_bus_call(&s_m[0], cpu, "order/01", 0, 0); }
static int body_to_b(CPUState* cpu) {
    return openpete_mods_bus_call_to(&s_m[0], cpu, "order/1", s_m[1].key, 0, 0);
}
static int body_to_unknown(CPUState* cpu) {
    return openpete_mods_bus_call_to(&s_m[0], cpu, "order/1", openpete_mod_key_of("zz"), 0, 0);
}
static int body_range_ok(CPUState* cpu) {
    return openpete_mods_bus_call(&s_m[0], cpu, "order/1", RAM_BASE + 16, 64);
}
static int body_range_bad(CPUState* cpu) {
    return openpete_mods_bus_call(&s_m[0], cpu, "order/1", RAM_BASE + sizeof s_ram - 8, 64);
}

/** @brief Results: stop, negative read as pass, OP_BUS_NONE for unattached and misspelt names, a malformed name refused, bus_call_to by key, the range check. */
static void case_results(void) {
    reset();
    enter(0, "order/1", h_log);
    enter(1, "order/1", h_log);
    enter(2, "order/1", h_log);
    s_stop_at = 1;
    CHECK(from_override(body_stop) == 9, "stop value");
    CHECK(strcmp(log_str(), "cb") == 0, "walk ends at the stopper: %s", log_str());
    s_stop_at = -1; s_neg_at = 2; s_log_n = 0;
    CHECK(from_override(body_stop) == OP_BUS_PASS && s_log_n == 3, "negative read as pass");
    s_neg_at = -1;
    CHECK(from_override(body_none) == OP_BUS_NONE, "unattached bus");
    CHECK(from_override(body_misspelt) == OP_BUS_NONE, "misspelt bus");
    CHECK(from_override(body_malformed) == -1, "malformed name");
    s_log_n = 0;
    CHECK(from_override(body_to_b) == OP_BUS_PASS && strcmp(log_str(), "b") == 0, "call_to b");
    CHECK(from_override(body_to_unknown) == OP_BUS_NONE, "call_to an unloaded key");
    atomic_store(&s_m[1].enabled, 0);
    openpete_mods_bus_publish();
    CHECK(from_override(body_to_b) == OP_BUS_NONE, "call_to a disabled member");
    CHECK(from_override(body_range_ok) == OP_BUS_PASS, "contiguous range");
    CHECK(from_override(body_range_bad) == -1, "range past the end refused");
}

static int s_depth_seen, s_depth_fail;
/** @brief Nest the same bus until a call fails, recording the depth. */
static int h_deep(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                  uint32_t msg, uint32_t size, void* user) {
    (void)from; (void)msg; (void)size; (void)user;
    s_depth_seen++;
    const int rc = openpete_mods_bus_call(self, cpu, "deep/1", 0, 0);
    if (rc < 0 && !s_depth_fail) s_depth_fail = rc;
    return OP_BUS_PASS;
}
static int body_deep(CPUState* cpu) { return openpete_mods_bus_call(&s_m[0], cpu, "deep/1", 0, 0); }
static int body_fill(CPUState* cpu) {
    mod_bracket_t b[MOD_FRAME_STACK_MAX];
    int pushed = 0;
    while (frame_enter(&b[pushed], OPENPETE_MODS_FRAME_OVERRIDE, "o")) pushed++;
    const int rc = openpete_mods_bus_call(&s_m[0], cpu, "order/1", 0, 0);
    while (pushed > 0) frame_leave(&b[--pushed]);
    return rc;
}

/** @brief Eight nested calls run, the ninth gets OP_BUS_DEPTH; a broadcast over more members than the cap runs them all; a full frame stack gives OP_BUS_DEPTH. */
static void case_depth(void) {
    reset();
    enter(0, "deep/1", h_deep);
    s_depth_seen = 0; s_depth_fail = 0;
    CHECK(from_override(body_deep) == OP_BUS_PASS, "outer deep call");
    CHECK(s_depth_seen == 8 && s_depth_fail == OP_BUS_DEPTH, "depth %d fail %d", s_depth_seen,
          s_depth_fail);
    for (int i = 0; i < 11; i++) enter(i, "order/1", h_log);
    s_log_n = 0;
    CHECK(from_override(body_stop) == OP_BUS_PASS && s_log_n == 11, "11-member broadcast ran %d",
          s_log_n);
    CHECK(from_override(body_fill) == OP_BUS_DEPTH, "full frame stack");
    CHECK(s_frame_depth == 0, "frames unwound");
}

static uint32_t s_sp_seen;
static int s_base_ran;
/** @brief Write every saved register, both GTE banks and sp, and try base(). */
static int h_clobber(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                     uint32_t msg, uint32_t size, void* user) {
    (void)self; (void)from; (void)msg; (void)size; (void)user;
    s_sp_seen = cpu->sp;
    uint32_t* r = &cpu->at;
    for (int i = 0; i < 15; i++) r[i] = 0xBAD00000u + (uint32_t)i;
    cpu->t8 = cpu->t9 = cpu->hi = cpu->lo = 0xBAD1u;
    memset(cpu->gte_data, 0x5A, sizeof cpu->gte_data);
    memset(cpu->gte_ctrl, 0xA5, sizeof cpu->gte_ctrl);
    cpu->sp = 0x1234u;
    const int before = s_orig;
    openpete_mods_base(cpu);
    s_base_ran = s_orig != before;
    return OP_BUS_PASS;
}
static int body_regs(CPUState* cpu) {
    cpu->sp = 0x801FFF13u;
    for (int i = 0; i < 32; i++) { cpu->gte_data[i] = (uint32_t)i; cpu->gte_ctrl[i] = 100u + (uint32_t)i; }
    uint32_t* r = &cpu->at;
    for (int i = 0; i < 15; i++) r[i] = (uint32_t)i;
    CPUState before = *cpu;
    const int rc = openpete_mods_bus_call(&s_m[0], cpu, "regs/1", 0, 0);
    if (memcmp(&before.at, &cpu->at, offsetof(CPUState, s0) - offsetof(CPUState, at)) ||
        before.t8 != cpu->t8 || before.t9 != cpu->t9 || before.hi != cpu->hi ||
        before.lo != cpu->lo || before.sp != cpu->sp ||
        memcmp(before.gte_data, cpu->gte_data, sizeof cpu->gte_data) ||
        memcmp(before.gte_ctrl, cpu->gte_ctrl, sizeof cpu->gte_ctrl))
        return -50;
    return rc;
}

/** @brief The saved set is restored after the handler, sp is lowered by the 8-aligned 64-byte reserve inside it, and base() in a handler runs nothing. */
static void case_regs(void) {
    reset();
    enter(0, "regs/1", h_clobber);
    s_base_ran = 0;
    CHECK(from_override(body_regs) == OP_BUS_PASS, "saved set restored");
    CHECK(s_sp_seen == ((0x801FFF13u - 64u) & ~7u), "handler sp %08X", s_sp_seen);
    CHECK(!s_base_ran, "base() in a bus handler ran the original");
}

/* Context refusals. */
static int call_now(void) { return openpete_mods_bus_call(&s_m[0], &s_cpu, "order/1", 0, 0); }

/** @brief A bus call is legal from an override and a tick-class toggle or level hook; refused at every other site kind, under a non-tick enclosing site, off the sim role, and under the present flag. */
static void case_contexts(void) {
    reset();
    enter(0, "order/1", h_log);
    CHECK(call_now() == -1, "GUEST with no override frame");
    static const openpete_mods_ctx refused[] = {
        OPENPETE_MODS_CTX_ENTRY, OPENPETE_MODS_CTX_RELOAD_ENTRY, OPENPETE_MODS_CTX_PRESENT,
        OPENPETE_MODS_CTX_UI, OPENPETE_MODS_CTX_HOST, OPENPETE_MODS_CTX_REFINE,
        OPENPETE_MODS_CTX_SHUTDOWN, OPENPETE_MODS_CTX_SAVE_RECORD,
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        const openpete_mods_site_t prev = openpete_mods_site_open(refused[i]);
        CHECK(call_now() == -1, "site %d admitted a bus call", (int)refused[i]);
        openpete_mods_site_close(prev);
    }
    openpete_mods_site_t prev = openpete_mods_site_open(OPENPETE_MODS_CTX_TOGGLE);
    CHECK(call_now() == OP_BUS_PASS, "toggle hook under GUEST");
    openpete_mods_site_close(prev);
    prev = openpete_mods_site_open(OPENPETE_MODS_CTX_LEVEL);
    CHECK(call_now() == OP_BUS_PASS, "level-ready hook under GUEST");
    openpete_mods_site_close(prev);
    prev = openpete_mods_site_open(OPENPETE_MODS_CTX_SHUTDOWN);
    const openpete_mods_site_t inner = openpete_mods_site_open(OPENPETE_MODS_CTX_TOGGLE);
    CHECK(call_now() == -1, "toggle hook under a shutdown hook");
    openpete_mods_site_close(inner);
    s_override_body = NULL;
    openpete_mods_site_close(prev);
    s_no_sim = 1;
    CHECK(from_override(body_call_order) == -1, "a thread without the sim role");
    s_no_sim = 0;
    openpete_mods_in_present = 1;
    CHECK(from_override(body_call_order) == -1, "present flag");
    openpete_mods_in_present = 0;
    CHECK(from_override(body_call_order) == OP_BUS_PASS, "override");
}

/** @brief An override reached from a shutdown hook keeps the shutdown class: its bus call is refused. */
static void case_override_under_shutdown(void) {
    reset();
    enter(0, "order/1", h_log);
    const openpete_mods_site_t prev = openpete_mods_site_open(OPENPETE_MODS_CTX_SHUTDOWN);
    CHECK(from_override(body_call_order) == -1, "override under a shutdown hook");
    openpete_mods_site_close(prev);
}

/** @brief bus_attach outside an entry, a sim_mutative = false mod and a keyless mod are refused; a keyless mod's mod_key is 0. */
static void case_owner_refusals(void) {
    reset();
    CHECK(openpete_mods_bus_attach(&s_m[0], "order/1", h_log, NULL) == -1, "attach under GUEST");
    s_m[1].neutral = 1;
    const openpete_mods_site_t prev = openpete_mods_site_open(OPENPETE_MODS_CTX_ENTRY);
    CHECK(openpete_mods_bus_attach(&s_m[1], "order/1", h_log, NULL) == -1, "neutral attach");
    CHECK(openpete_mods_bus_attach(&s_m[2], "order", h_log, NULL) == -1, "attach malformed");
    CHECK(openpete_mods_bus_attach(&s_m[2], "order/1", NULL, NULL) == -1, "attach NULL fn");
    s_m[3].key = 0;
    CHECK(openpete_mods_bus_attach(&s_m[3], "order/1", h_log, NULL) == -1, "keyless attach");
    openpete_mods_site_close(prev);
    CHECK(openpete_mods_bus_mod_key(&s_m[3]) == 0, "keyless mod_key");
    CHECK(openpete_mods_bus_members(&s_m[3], "order/1", NULL, 0) == -1, "keyless members");
    enter(0, "order/1", h_log);
    s_m[0].neutral = 1;
    CHECK(from_override(body_call_order) == -1, "neutral send");
    s_m[0].neutral = 0;
    CHECK(openpete_mods_bus_mod_key(&s_m[0]) == openpete_mod_key_of("a"), "mod_key equals the SDK hash");
}

/* Mutation while a walk is in progress: the first handler changes later members. */
static int s_mut;
/** @brief The first handler: disables c, retires b (a reload or a failed reload), enters d (a first enable); every later handler logs. */
static int h_mutate(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                    uint32_t msg, uint32_t size, void* user) {
    const int i = (int)(self - s_m);
    if (i == 4 && !s_mut) {
        s_mut = 1;
        atomic_store(&s_m[2].enabled, 0);
        openpete_mods_bus_publish();
        strip(1);
        enter(1, "order/1", h_mutate);   /* the reloaded object re-attaches */
        enter(3, "order/1", h_mutate);   /* a first enable mid-walk */
    }
    return h_log(self, cpu, from, msg, size, user);
}

/** @brief A member disabled, retired or entered while a walk is in progress is not entered in that walk; every handler runs at most once. */
static void case_mutation(void) {
    reset();
    enter(0, "order/1", h_mutate);
    enter(1, "order/1", h_mutate);
    enter(2, "order/1", h_mutate);
    enter(4, "order/1", h_mutate);
    s_mut = 0;
    s_log_n = 0;
    CHECK(from_override(body_call_order) == OP_BUS_PASS, "mutating walk");
    CHECK(strcmp(log_str(), "ea") == 0, "walk after mutation %s", log_str());
    for (int i = 0; i < N_MODS; i++) CHECK(s_m[i].runs <= 1, "%s ran %d times", k_ids[i], s_m[i].runs);
    s_log_n = 0;
    (void)from_override(body_call_order);
    CHECK(strcmp(log_str(), "dbea") == 0, "next walk %s", log_str());
    atomic_store(&s_m[4].closed, 1);
    s_log_n = 0;
    (void)from_override(body_call_order);
    CHECK(strcmp(log_str(), "dba") == 0, "closed member skipped: %s", log_str());
}

/** @brief A live-disabled mod reloaded (strip, re-attach, publish under enabled = 0) is never listed; record sizes are checked and strided. */
static void case_members_records(void) {
    reset();
    enter(0, "order/1", h_log);
    atomic_store(&s_m[1].enabled, 0);
    strip(1);
    enter(1, "order/1", h_log);
    openpete_mod_bus_sender_t m[4];
    m[0].struct_size = sizeof m[0];
    CHECK(openpete_mods_bus_members(&s_m[0], "order/1", m, 4) == 1 && strcmp(m[0].id, "a") == 0,
          "disabled member listed");
    m[0].struct_size = 4;
    CHECK(openpete_mods_bus_members(&s_m[0], "order/1", m, 4) == -1, "short record refused");
    struct { openpete_mod_bus_sender_t r; uint32_t extra; } big[2];
    memset(big, 0x77, sizeof big);
    big[0].r.struct_size = sizeof big[0];
    atomic_store(&s_m[1].enabled, 1);
    openpete_mods_bus_publish();
    CHECK(openpete_mods_bus_members(&s_m[0], "order/1", &big[0].r, 2) == 2 &&
          strcmp(big[1].r.id, "a") == 0 && big[0].extra == 0x77777777u, "strided records");
    openpete_mod_bus_sender_t rec = { sizeof rec, NULL, 0 };
    CHECK(openpete_mods_bus_mod_info(&s_m[0], s_m[5].key, &rec) == 0 && strcmp(rec.id, "f") == 0,
          "mod_info covers a mod on no bus");
    CHECK(openpete_mods_bus_mod_info(&s_m[0], openpete_mod_key_of("zz"), &rec) == -1, "unknown key");
    CHECK(openpete_mods_bus_mod_info(&s_m[0], 0, &rec) == -1, "key 0");
}

static int h_fault_probe(openpete_mod_t* self, CPUState* cpu, const openpete_mod_bus_sender_t* from,
                         uint32_t msg, uint32_t size, void* user) {
    (void)self; (void)cpu; (void)from; (void)msg; (void)size; (void)user;
    openpete_mods_attribution_t at;
    CHECK(openpete_mods_crash_attribution(&at) && at.kind == OPENPETE_MODS_FRAME_BUS &&
          strcmp(at.bus, "crash/1") == 0 && strcmp(at.owner, "a") == 0,
          "attribution in a handler names the bus and its owner");
    return OP_BUS_PASS;
}
static int body_crash(CPUState* cpu) { return openpete_mods_bus_call(&s_m[0], cpu, "crash/1", 0, 0); }

/** @brief Crash attribution from inside a handler names the bus and the handler's owner. */
static void case_attribution(void) {
    reset();
    enter(0, "crash/1", h_fault_probe);
    CHECK(from_override(body_crash) == OP_BUS_PASS, "crash walk");
}

static _Atomic int s_reader_stop;
static _Atomic long s_reader_reads;
/** @brief A reader thread: bus_members and mod_info in a loop until stopped. */
static void* reader(void* arg) {
    (void)arg;
    openpete_mod_bus_sender_t m[N_MODS];
    while (!atomic_load(&s_reader_stop)) {
        m[0].struct_size = sizeof m[0];
        const int n = openpete_mods_bus_members(&s_m[0], "order/1", m, N_MODS);
        for (int i = 0; i < n && i < N_MODS; i++)
            if (!m[i].id || m[i].key != openpete_mod_key_of(m[i].id)) atomic_store(&s_reader_stop, 2);
        openpete_mod_bus_sender_t rec = { sizeof rec, NULL, 0 };
        (void)openpete_mods_bus_mod_info(&s_m[0], s_m[3].key, &rec);
        atomic_fetch_add(&s_reader_reads, 1);
    }
    return NULL;
}

/** @brief A reader thread sees consistent records while the sim side toggles, strips and re-enters members and publishes. */
static void case_reader_thread(void) {
    reset();
    for (int i = 0; i < 6; i++) enter(i, "order/1", h_log);
    pthread_t t;
    atomic_store(&s_reader_stop, 0);
    CHECK(pthread_create(&t, NULL, reader, NULL) == 0, "reader thread");
    /* Start mutating only once the reader runs: on a loaded machine the 2000
     * rounds can otherwise finish before the reader is first scheduled. */
    for (int spin = 0; spin < 5000 && atomic_load(&s_reader_reads) == 0; spin++) {
        struct timespec ms = {0, 1000000};
        nanosleep(&ms, NULL);
    }
    for (int round = 0; round < 2000; round++) {
        const int i = round % 6;
        atomic_store(&s_m[i].enabled, !atomic_load(&s_m[i].enabled));
        openpete_mods_bus_publish();
        if (round % 7 == 0) { strip(i); enter(i, "order/1", h_log); }
    }
    const int bad = atomic_load(&s_reader_stop) == 2;
    atomic_store(&s_reader_stop, 1);
    pthread_join(t, NULL);
    CHECK(!bad, "reader saw a bad record (%ld reads)", atomic_load(&s_reader_reads));
    CHECK(atomic_load(&s_reader_reads) > 0, "reader never read within 5 s");
}

/* A stub service table: a marker word and the exporter's index. */
typedef struct { uint32_t size, marker, who; } svc_tab_t;

/** @brief Export a stub table from an entry site and publish, as a mod's entry does.
 *
 * @param i the exporter.
 * @param name the service name.
 * @param major its major.
 * @param marker the table's marker word.
 * @return svc_export's result. */
static int export_at_entry(int i, const char* name, uint32_t major, uint32_t marker) {
    svc_tab_t t = { sizeof t, marker, (uint32_t)i };
    const openpete_mods_site_t prev = openpete_mods_site_open(OPENPETE_MODS_CTX_ENTRY);
    const int rc = openpete_mods_svc_export(&s_m[i], name, major, &t, sizeof t);
    openpete_mods_site_close(prev);
    t.marker = 0xDEADu; /* the engine kept its own copy */
    openpete_mods_bus_publish();
    return rc;
}

/** @brief Format svc_list's keys for a service as exporter ids. */
static const char* list_str(const char* name, uint32_t major) {
    static char buf[64];
    uint32_t keys[N_MODS];
    const int n = openpete_mods_svc_list(&s_m[0], name, major, keys, N_MODS);
    buf[0] = 0;
    for (int i = 0; i < n && i < N_MODS; i++)
        for (int j = 0; j < N_MODS; j++)
            if (s_m[j].key == keys[i]) strncat(buf, k_ids[j], sizeof buf - strlen(buf) - 1);
    return buf;
}

/** @brief svc_get by key and major with min_size; svc_list newest entry first; replace in place; a toggle, a strip and a re-export; refusals. */
static void case_svc(void) {
    reset();
    CHECK(export_at_entry(1, "gift", 1, 11) == 0, "export b");
    CHECK(export_at_entry(2, "gift", 1, 12) == 0, "export c");
    CHECK(export_at_entry(3, "gift", 2, 13) == 0, "export d major 2");
    CHECK(strcmp(list_str("gift", 1), "cb") == 0, "providers newest first: %s", list_str("gift", 1));
    const svc_tab_t* t = openpete_mods_svc_get(&s_m[0], s_m[1].key, "gift", 1, sizeof(svc_tab_t));
    CHECK(t && t->marker == 11 && t->who == 1, "svc_get b's copy");
    CHECK(!openpete_mods_svc_get(&s_m[0], s_m[3].key, "gift", 1, 4), "another major");
    CHECK(!openpete_mods_svc_get(&s_m[0], s_m[1].key, "gift", 1, sizeof(svc_tab_t) + 1), "min_size");
    CHECK(!openpete_mods_svc_get(&s_m[0], 0, "gift", 1, 4), "key 0");
    CHECK(!openpete_mods_svc_get(&s_m[0], s_m[1].key, "gifts", 1, 4), "another name");
    /* A second export replaces in place; the first copy stays readable. */
    CHECK(export_at_entry(1, "gift", 1, 21) == 0, "re-export b");
    const svc_tab_t* t2 = openpete_mods_svc_get(&s_m[0], s_m[1].key, "gift", 1, 4);
    CHECK(t2 && t2 != t && t2->marker == 21 && t->marker == 11, "replaced copy, old kept");
    CHECK(strcmp(list_str("gift", 1), "cb") == 0, "replace keeps the position");
    /* Toggle off: gone from both reads; on: back. */
    atomic_store(&s_m[2].enabled, 0);
    openpete_mods_bus_publish();
    CHECK(!openpete_mods_svc_get(&s_m[0], s_m[2].key, "gift", 1, 4) &&
          strcmp(list_str("gift", 1), "b") == 0, "disabled provider");
    atomic_store(&s_m[2].enabled, 1);
    openpete_mods_bus_publish();
    /* Disabled without a republish: the live re-check hides it. */
    atomic_store(&s_m[2].enabled, 0);
    CHECK(!openpete_mods_svc_get(&s_m[0], s_m[2].key, "gift", 1, 4), "live re-check");
    atomic_store(&s_m[2].enabled, 1);
    /* Strip (a reload, or a reload that fails after the strip): no export. */
    strip(1);
    CHECK(!openpete_mods_svc_get(&s_m[0], s_m[1].key, "gift", 1, 4) &&
          strcmp(list_str("gift", 1), "c") == 0, "stripped provider");
    CHECK(export_at_entry(1, "gift", 1, 31) == 0, "reload entry exports again");
    const svc_tab_t* t3 = openpete_mods_svc_get(&s_m[0], s_m[1].key, "gift", 1, 4);
    CHECK(t3 && t3->marker == 31 && strcmp(list_str("gift", 1), "bc") == 0,
          "re-export at the front with the new copy");
    /* Refusals. */
    svc_tab_t x = { sizeof x, 1, 0 };
    CHECK(openpete_mods_svc_export(&s_m[4], "gift", 1, &x, sizeof x) == -1, "export outside entry");
    const openpete_mods_site_t prev = openpete_mods_site_open(OPENPETE_MODS_CTX_ENTRY);
    CHECK(openpete_mods_svc_export(&s_m[4], "", 1, &x, sizeof x) == -1, "empty name");
    CHECK(openpete_mods_svc_export(&s_m[4], "gift", 0, &x, sizeof x) == -1, "major 0");
    CHECK(openpete_mods_svc_export(&s_m[4], "gift", 1, NULL, 4) == -1, "NULL table");
    CHECK(openpete_mods_svc_export(&s_m[4], "gift", 1, &x, 0) == -1, "size 0");
    s_m[4].key = 0;
    CHECK(openpete_mods_svc_export(&s_m[4], "gift", 1, &x, sizeof x) == -1, "keyless export");
    openpete_mods_site_close(prev);
    uint32_t keys[2];
    CHECK(!openpete_mods_svc_get(&s_m[4], s_m[2].key, "gift", 1, 4) &&
          openpete_mods_svc_list(&s_m[4], "gift", 1, keys, 2) == 0, "keyless consumer");
    CHECK(openpete_mods_svc_list(&s_m[0], "gift", 1, NULL, 1) == -1 &&
          openpete_mods_svc_list(&s_m[0], "gift", 1, NULL, 0) == 2, "list arguments");
}

static _Atomic int s_svc_stop;
static _Atomic long s_svc_reads;
/** @brief A reader thread: svc_list and svc_get per key in a loop until stopped; a table's who must name its provider. */
static void* svc_reader(void* arg) {
    (void)arg;
    uint32_t keys[N_MODS];
    while (!atomic_load(&s_svc_stop)) {
        const int n = openpete_mods_svc_list(&s_m[0], "gift", 1, keys, N_MODS);
        for (int i = 0; i < n && i < N_MODS; i++) {
            const svc_tab_t* t = openpete_mods_svc_get(&s_m[0], keys[i], "gift", 1, sizeof *t);
            if (t && (t->who >= N_MODS || s_m[t->who].key != keys[i])) atomic_store(&s_svc_stop, 2);
        }
        atomic_fetch_add(&s_svc_reads, 1);
    }
    return NULL;
}

/** @brief A reader thread sees consistent tables while the sim side toggles, strips and re-exports providers and publishes. */
static void case_svc_reader_thread(void) {
    reset();
    for (int i = 1; i < 7; i++) (void)export_at_entry(i, "gift", 1, (uint32_t)i);
    pthread_t t;
    atomic_store(&s_svc_stop, 0);
    CHECK(pthread_create(&t, NULL, svc_reader, NULL) == 0, "svc reader thread");
    while (atomic_load(&s_svc_reads) == 0) sched_yield(); /* reading before the first publish */
    for (int round = 0; round < 2000; round++) {
        const int i = 1 + round % 6;
        atomic_store(&s_m[i].enabled, !atomic_load(&s_m[i].enabled));
        openpete_mods_bus_publish();
        if (round % 5 == 0) { strip(i); (void)export_at_entry(i, "gift", 1, (uint32_t)round); }
    }
    const int bad = atomic_load(&s_svc_stop) == 2;
    atomic_store(&s_svc_stop, 1);
    pthread_join(t, NULL);
    CHECK(!bad, "svc reader saw a bad table (%ld reads)", atomic_load(&s_svc_reads));
}

/** @brief Run every case; exit 1 on any failed check. */
int main(void) {
    case_order();
    case_results();
    case_depth();
    case_regs();
    case_contexts();
    case_override_under_shutdown();
    case_owner_refusals();
    case_mutation();
    case_members_records();
    case_attribution();
    case_reader_thread();
    case_svc();
    case_svc_reader_thread();
    if (s_fail) return 1;
    printf("mod_bus_test: all cases passed\n");
    return 0;
}
