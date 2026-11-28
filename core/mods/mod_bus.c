// core/mods/mod_bus.c - named synchronous message buses between mods.
//
// A bus is a name "<name>/<major>". Each mod attaches at most one handler
// per bus, at its entry; bus_call walks the bus's enabled handlers in the
// mod entry sequence (newest entry first, as override links are inserted),
// and bus_call_to runs one mod's handler by its key. The message lives in
// guest memory and the engine never writes it.
//
// Three tables:
//   - the bus table: per bus, its handlers in walk order. Written at entry
//     (attach) and at a strip (mod_strip_registrations), on the sim thread
//     or on the main thread during boot; never read by another thread.
//   - the membership snapshot: an immutable copy of the bus table's
//     handlers whose owners are enabled, swapped by an atomic pointer at
//     the loader's publication points (after an entry, in a toggle flip,
//     in a strip, after a reload re-applies the enabled state). Walkers
//     and bus_members read it; superseded snapshots are kept for the
//     process, so a walk that parks the fiber inside a handler keeps a
//     valid member list across the tick boundary that runs meanwhile.
//   - the key table: every discovered mod's key and interned id, built
//     at discovery and sealed before the first entry; immutable after.
//
// The walker re-checks each handler's owner before entering it (enabled,
// api closed, retirement count against the count the snapshot row was
// built under), so a handler whose owner was disabled, closed or retired
// while an earlier handler had the fiber parked is never entered.

#include "mod_bus.h"
#include "openpete_mods.h"
#include "openpete_log.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_ptr */
#include "openpete_fiber.h"        /* openpete_speculative */
#include "openpete_thread_roles.h" /* op_role_mask */
#include "psx_runtime.h"           /* CPUState */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define BUS_DEPTH_MAX   8    /* nested bus_call/bus_call_to per site */
#define BUS_SP_RESERVE  64u  /* bytes below the sender's sp per handler */
#define BUS_NAME_MAX    96   /* longest accepted bus name, NUL excluded */

/* The api-13 size of openpete_mod_bus_sender_t: the smallest record a
 * caller may pass. */
#define BUS_SENDER_SIZE_13 ((uint32_t)sizeof(openpete_mod_bus_sender_t))

/* ---- interned strings --------------------------------------------------
 * Bus names and mod ids a table row, a snapshot and a frame point at:
 * a prepend-only list of nodes never freed, so a reader walks an immutable
 * tail and an insert is one compare-exchange of the head. */
typedef struct str_node {
    const struct str_node* next;
    char                   s[];
} str_node_t;

static _Atomic(const str_node_t*) s_interned;

const char* openpete_mods_bus_intern(const char* s) {
    const str_node_t* head = atomic_load(&s_interned);
    for (;;) {
        for (const str_node_t* n = head; n; n = n->next)
            if (strcmp(n->s, s) == 0) return n->s;
        const size_t len = strlen(s) + 1;
        str_node_t* node = (str_node_t*)malloc(sizeof *node + len);
        if (!node) return NULL;
        memcpy(node->s, s, len);
        node->next = head;
        if (atomic_compare_exchange_strong(&s_interned, &head, node)) return node->s;
        free(node); /* another insert won: re-check the longer list */
    }
}

/* ---- the key table ------------------------------------------------------ */

typedef struct {
    uint32_t    key;
    const char* id; /* interned */
} key_row_t;

typedef struct {
    int              n;
    const key_row_t* rows; /* sorted by key */
} key_table_t;

/* Rows added before the seal: a prepend-only list, as s_interned. */
typedef struct key_node {
    const struct key_node* next;
    key_row_t              row;
} key_node_t;

static _Atomic(const key_node_t*) s_key_build;
static _Atomic(const key_table_t*) s_keys;   /* NULL until the seal */

const char* openpete_mods_keys_add(const char* id, uint32_t key) {
    if (atomic_load(&s_keys) || !id) return NULL;
    const char* iid = openpete_mods_bus_intern(id);
    key_node_t* node = iid ? (key_node_t*)malloc(sizeof *node) : NULL;
    if (!node) return NULL;
    node->row.key = key;
    node->row.id  = iid;
    const key_node_t* head = atomic_load(&s_key_build);
    do node->next = head;
    while (!atomic_compare_exchange_weak(&s_key_build, &head, node));
    return iid;
}

/** @brief qsort comparator ordering key rows by key. */
static int key_cmp(const void* a, const void* b) {
    const uint32_t x = ((const key_row_t*)a)->key, y = ((const key_row_t*)b)->key;
    return x < y ? -1 : x > y;
}

void openpete_mods_keys_seal(void) {
    if (atomic_load(&s_keys)) return;
    int n = 0;
    for (const key_node_t* k = atomic_load(&s_key_build); k; k = k->next) n++;
    key_table_t* t = (key_table_t*)malloc(sizeof *t + (size_t)n * sizeof(key_row_t));
    if (!t) {
        OPL_ERROR_F("mods", "mod key table allocation failed - mod_info "
                    "knows no key this session");
        return;
    }
    key_row_t* rows = (key_row_t*)(t + 1);
    int i = 0;
    for (const key_node_t* k = atomic_load(&s_key_build); k; k = k->next) rows[i++] = k->row;
    qsort(rows, (size_t)n, sizeof(key_row_t), key_cmp);
    t->n = n;
    t->rows = rows;
    atomic_store(&s_keys, t);
}

const char* openpete_mods_key_id(uint32_t key) {
    const key_table_t* t = atomic_load(&s_keys);
    if (!t || key == 0) return NULL;
    int lo = 0, hi = t->n - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (t->rows[mid].key == key) return t->rows[mid].id;
        if (t->rows[mid].key < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

/* ---- one log line per mod and cause --------------------------------------
 * Refusals come from any thread (bus_members, a mod's own thread), so the
 * set is guarded by a spin flag; a refusal is rare and the critical section
 * is a short scan. */

typedef struct once_node {
    const openpete_mod_t*   m;
    const char*             what; /* a literal */
    const char*             bus;  /* interned, or NULL */
    const struct once_node* next;
} once_node_t;

/* Prepend-only list, nodes never freed: a lookup walks an immutable tail,
 * an insert is one compare-exchange of the head. Two threads that log the
 * same triple at once may both insert it, and the line appears twice. */
static _Atomic(const once_node_t*) s_once;

/** @brief Return 1 the first time a (mod, cause, bus) triple is seen, else 0.
 *
 * An allocation failure answers 1, so the line is logged again rather than
 * lost.
 * @param m the mod the line is about.
 * @param what the cause, a string literal.
 * @param bus the interned bus name, or NULL. */
static int once(const openpete_mod_t* m, const char* what, const char* bus) {
    const once_node_t* head = atomic_load(&s_once);
    for (const once_node_t* n = head; n; n = n->next)
        if (n->m == m && n->what == what && n->bus == bus) return 0;
    once_node_t* node = (once_node_t*)malloc(sizeof *node);
    if (!node) return 1;
    node->m = m; node->what = what; node->bus = bus;
    do node->next = head;
    while (!atomic_compare_exchange_weak(&s_once, &head, node));
    return 1;
}

int openpete_mods_bus_refuse(const openpete_mod_t* m, const char* entry, const char* why) {
    if (once(m, why, NULL))
        OPL_ERROR_F(m ? mod_owner_tag(m) : "mods", "%s refused: %s "
                    "(later refusals for this cause are not logged)", entry, why);
    return -1;
}

/* ---- bus names ----------------------------------------------------------- */

/** @brief Return 1 for a well-formed bus name: a non-empty name, '/', and a decimal major of 1 or more.
 *
 * The major has no leading zero, so one bus has one spelling.
 * @param s the name, or NULL. */
static int bus_name_valid(const char* s) {
    if (!s || !*s || *s == '/') return 0;
    const char* slash = strrchr(s, '/');
    if (!slash || strlen(s) > BUS_NAME_MAX) return 0;
    const char* d = slash + 1;
    if (*d < '1' || *d > '9') return 0;
    for (; *d; d++)
        if (*d < '0' || *d > '9') return 0;
    return 1;
}

/* ---- the bus table ---------------------------------------------------- */

typedef struct {
    openpete_mod_t*     owner;
    const char*         id;   /* owner's interned id */
    openpete_mod_bus_fn fn;
    void*               user;
} handler_t;

typedef struct {
    const char* name;     /* interned */
    int         first, n; /* rows [first, first + n), [first] walks first */
} tab_bus_t;

/* The bus table is immutable once published: attach and strip build a new
 * one and swap it in, so the pointer is the only mutable state. Only the
 * sim role (and the main thread at boot, which holds it) reads or swaps
 * it; a superseded table is freed at the swap. */
typedef struct {
    int              n_bus, n_rows;
    const tab_bus_t* buses;
    const handler_t* rows;
} bus_table_t;

static _Atomic(const bus_table_t*) s_table;

/** @brief Allocate an empty table with room for a number of buses and rows.
 *
 * @param n_bus bus capacity.
 * @param n_rows row capacity.
 * @return the table, its arrays writable through the casts below, or NULL. */
static bus_table_t* table_alloc(int n_bus, int n_rows) {
    bus_table_t* t = (bus_table_t*)malloc(sizeof *t + (size_t)n_bus * sizeof(tab_bus_t) +
                                          (size_t)n_rows * sizeof(handler_t));
    if (!t) return NULL;
    t->n_bus = 0;
    t->n_rows = 0;
    t->buses = (const tab_bus_t*)(t + 1);
    t->rows = (const handler_t*)(t->buses + n_bus);
    return t;
}

/** @brief Swap in a new table and free the one it replaces.
 *
 * @param t the new table. */
static void table_swap(const bus_table_t* t) {
    free((void*)atomic_exchange(&s_table, t));
}

/** @brief Return the published table's bus count, 0 before the first attach. */
static int table_buses(void) {
    const bus_table_t* t = atomic_load(&s_table);
    return t ? t->n_bus : 0;
}

/** @brief Publish a table equal to the current one with a handler attached: replaced in place when the owner has one on the bus, else inserted at the bus's front.
 *
 * @param bus the interned bus name.
 * @param h the handler row.
 * @return 0, or -1 on an allocation failure (the table is unchanged). */
static int table_attach(const char* bus, const handler_t* h) {
    const bus_table_t* old = atomic_load(&s_table);
    const int ob = old ? old->n_bus : 0, orows = old ? old->n_rows : 0;
    bus_table_t* t = table_alloc(ob + 1, orows + 1);
    if (!t) return -1;
    tab_bus_t* buses = (tab_bus_t*)t->buses;
    handler_t* rows = (handler_t*)t->rows;
    int found = 0;
    for (int i = 0; i < ob; i++) {
        const tab_bus_t* b = &old->buses[i];
        tab_bus_t* nb = &buses[t->n_bus++];
        nb->name = b->name;
        nb->first = t->n_rows;
        const int mine = b->name == bus;
        int replaced = 0;
        for (int j = 0; mine && j < b->n; j++)
            replaced |= old->rows[b->first + j].owner == h->owner;
        if (mine && !replaced) rows[t->n_rows++] = *h; /* front */
        for (int j = 0; j < b->n; j++) {
            const handler_t* r = &old->rows[b->first + j];
            rows[t->n_rows++] = (mine && r->owner == h->owner) ? *h : *r;
        }
        nb->n = t->n_rows - nb->first;
        found |= mine;
    }
    if (!found) {
        tab_bus_t* nb = &buses[t->n_bus++];
        nb->name = bus;
        nb->first = t->n_rows;
        nb->n = 1;
        rows[t->n_rows++] = *h;
    }
    table_swap(t);
    return 0;
}

void openpete_mods_bus_remove_owner(const openpete_mod_t* m) {
    const bus_table_t* old = atomic_load(&s_table);
    if (!old) return;
    bus_table_t* t = table_alloc(old->n_bus, old->n_rows);
    if (!t) {
        OPL_ERROR_F("mods", "bus table allocation failed - the stripped mod's "
                    "handlers stay attached; its retirement count keeps the "
                    "walker from entering them");
        return;
    }
    tab_bus_t* buses = (tab_bus_t*)t->buses;
    handler_t* rows = (handler_t*)t->rows;
    for (int i = 0; i < old->n_bus; i++) {
        const tab_bus_t* b = &old->buses[i];
        tab_bus_t* nb = &buses[t->n_bus++];
        nb->name = b->name;
        nb->first = t->n_rows;
        for (int j = 0; j < b->n; j++)
            if (old->rows[b->first + j].owner != m) rows[t->n_rows++] = old->rows[b->first + j];
        nb->n = t->n_rows - nb->first;
    }
    table_swap(t);
}

/* ---- the membership snapshot --------------------------------------------- */

typedef struct {
    openpete_mod_t*     owner;
    const char*         id;
    uint32_t            key;
    openpete_mod_bus_fn fn;
    void*               user;
    uint32_t            retired; /* owner's retirement count at the build */
} snap_row_t;

typedef struct {
    const char* name; /* interned */
    int         first, n; /* rows [first, first + n) */
} snap_bus_t;

typedef struct {
    int               n_bus;
    const snap_bus_t* buses;
    const snap_row_t* rows;
    const openpete_mods_svc_snap_t* svc; /* the exports (mod_svc.c) */
} snapshot_t;

static _Atomic(const snapshot_t*) s_snap;
static _Atomic unsigned long s_published; /* snapshots built, for the exit report */

void openpete_mods_bus_publish(void) {
    const bus_table_t* tab = atomic_load(&s_table);
    const int tb = tab ? tab->n_bus : 0;
    size_t n_bus = 0, n_rows = 0;
    for (int i = 0; i < tb; i++) {
        size_t live = 0;
        for (int j = 0; j < tab->buses[i].n; j++)
            live += mod_owner_enabled(tab->rows[tab->buses[i].first + j].owner) != 0;
        if (live) { n_bus++; n_rows += live; }
    }
    const openpete_mods_svc_snap_t* svc = openpete_mods_svc_snap_build();
    snapshot_t* s = svc ? (snapshot_t*)malloc(sizeof *s + n_bus * sizeof(snap_bus_t) +
                                              n_rows * sizeof(snap_row_t)) : NULL;
    if (!s) {
        free((void*)svc);
        OPL_ERROR_F("mods", "bus snapshot allocation failed - bus membership "
                    "and service exports keep their previous state");
        return;
    }
    snap_bus_t* buses = (snap_bus_t*)(s + 1);
    snap_row_t* rows = (snap_row_t*)(buses + n_bus);
    int bi = 0, ri = 0;
    for (int i = 0; i < tb; i++) {
        const tab_bus_t* b = &tab->buses[i];
        const int first = ri;
        for (int j = 0; j < b->n; j++) {
            const handler_t* h = &tab->rows[b->first + j];
            if (!mod_owner_enabled(h->owner)) continue;
            snap_row_t* r = &rows[ri++];
            r->owner   = h->owner;
            r->id      = h->id;
            r->key     = mod_owner_key(h->owner);
            r->fn      = h->fn;
            r->user    = h->user;
            r->retired = mod_owner_retired(h->owner);
        }
        if (ri > first) {
            buses[bi].name  = b->name;
            buses[bi].first = first;
            buses[bi].n     = ri - first;
            bi++;
        }
    }
    s->n_bus = bi;
    s->buses = buses;
    s->rows  = rows;
    s->svc   = svc;
    atomic_store(&s_snap, s);
    atomic_fetch_add_explicit(&s_published, 1, memory_order_relaxed);
}

const openpete_mods_svc_snap_t* openpete_mods_bus_snap_svc(void) {
    const snapshot_t* s = atomic_load(&s_snap);
    return s ? s->svc : NULL;
}

/** @brief Find a bus's member run in a snapshot.
 *
 * @param s the snapshot, or NULL.
 * @param name the bus name.
 * @return the run, or NULL when no enabled handler is attached. */
static const snap_bus_t* snap_find(const snapshot_t* s, const char* name) {
    if (!s) return NULL;
    for (int i = 0; i < s->n_bus; i++)
        if (strcmp(s->buses[i].name, name) == 0) return &s->buses[i];
    return NULL;
}

/* ---- refusal tests -------------------------------------------------------- */

/** @brief Refuse a bus entry for a keyless mod or, for a mutating entry, a sim_mutative = false mod.
 *
 * @param self the caller.
 * @param entry the entry name.
 * @param mutating 1 for bus_attach, bus_call and bus_call_to.
 * @return 0 to proceed, -1 refused (logged once). */
static int owner_refused(const openpete_mod_t* self, const char* entry, int mutating) {
    if (!self) return -1;
    if (!mod_owner_key(self))
        return openpete_mods_bus_refuse(self, entry, "this mod is keyless (its key is 0, collides "
                      "with another mod's, or its id is 64 bytes or longer)");
    if (mutating && mod_owner_sim_neutral(self))
        return openpete_mods_bus_refuse(self, entry, "the manifest declares sim_mutative = false, "
                      "and a bus handler or sender changes gameplay state");
    return 0;
}

/** @brief Return the reason a bus_call or bus_call_to made now is refused, or NULL when it is legal.
 *
 * Legal on a thread holding the sim role, outside a present hook, when the
 * open site is GUEST with at least one override frame of the site's own
 * (a send from an override or from a bus handler reached from one), or a
 * toggle or level-ready hook of tick class. Every other site (entry,
 * shutdown, refine, save record, host, present, UI) and a toggle hook
 * opened under one of them is refused. */
static const char* call_context_refusal(void) {
    if (!(op_role_mask() & OP_ROLE_BIT_sim))
        return "bus calls run on the sim thread only (a thread the mod "
               "started, or a compose thread)";
    if (openpete_mods_in_present)
        return "bus calls are refused inside a present hook or UI section";
    const openpete_mods_site_t site = openpete_mods_site_now();
    switch (site.ctx) {
    case OPENPETE_MODS_CTX_GUEST:
        return openpete_mods_site_frames(OPENPETE_MODS_FRAME_OVERRIDE) > 0 ? NULL
               : "bus calls are legal from an override, a toggle hook or a "
                 "level-ready hook; no override is running";
    case OPENPETE_MODS_CTX_TOGGLE:
    case OPENPETE_MODS_CTX_LEVEL:
        return site.tick ? NULL
               : "bus calls are refused in a hook reached from a shutdown "
                 "hook, save-record callback or other non-tick site";
    case OPENPETE_MODS_CTX_ENTRY:
    case OPENPETE_MODS_CTX_RELOAD_ENTRY:
        return "bus calls are refused in the entry point";
    case OPENPETE_MODS_CTX_REFINE:
        return "bus calls are refused in a material refine callback";
    case OPENPETE_MODS_CTX_SHUTDOWN:
        return "bus calls are refused in a shutdown hook";
    case OPENPETE_MODS_CTX_SAVE_RECORD:
        return "bus calls are refused in a save-record callback";
    case OPENPETE_MODS_CTX_HOST:
        return "bus calls are refused in a host hook";
    case OPENPETE_MODS_CTX_PRESENT:
    case OPENPETE_MODS_CTX_UI:
        return "bus calls are refused inside a present hook or UI section";
    default:
        return "bus calls are legal from an override, a toggle hook or a "
               "level-ready hook";
    }
}

/** @brief Return 1 when [addr, addr + size) is one contiguous host view of guest memory.
 *
 * Size 0 is always accepted. The kernel area below physical 0x10000 is
 * refused, as api->guest refuses it.
 * @param addr the guest address.
 * @param size the length. */
static int range_ok(uint32_t addr, uint32_t size) {
    if (size == 0) return 1;
    const uint32_t last = addr + (size - 1u);
    if (last < addr) return 0;
    const uint32_t seg = addr >> 28;
    if (seg != 0x0u && seg != 0x8u && seg != 0x9u && seg != 0xAu && seg != 0xBu)
        return 0;
    if ((addr & 0x1FFFFFFFu) < 0x10000u) return 0;
    const uint8_t* p0 = (const uint8_t*)openpete_psx_to_host_ptr(addr);
    const uint8_t* p1 = (const uint8_t*)openpete_psx_to_host_ptr(last);
    return p0 && p1 && p1 >= p0 && (uintptr_t)(p1 - p0) == (uintptr_t)(size - 1u);
}

/* ---- the walker -------------------------------------------------------- */

/* The registers a guest callee may legally leave changed, plus sp: saved
 * around each handler so its api->call cannot change what the sender's
 * base() and the game code above it read. */
typedef struct {
    uint32_t at_t7[15]; /* at, v0, v1, a0-a3, t0-t7: consecutive in CPUState */
    uint32_t t8, t9, hi, lo, sp;
    uint32_t gte_data[32], gte_ctrl[32];
} saved_regs_t;

/** @brief Copy the saved set out of the CPU state.
 *
 * @param s the copy.
 * @param cpu the CPU state. */
static void regs_save(saved_regs_t* s, const CPUState* cpu) {
    memcpy(s->at_t7, &cpu->at, sizeof s->at_t7);
    s->t8 = cpu->t8; s->t9 = cpu->t9;
    s->hi = cpu->hi; s->lo = cpu->lo; s->sp = cpu->sp;
    memcpy(s->gte_data, cpu->gte_data, sizeof s->gte_data);
    memcpy(s->gte_ctrl, cpu->gte_ctrl, sizeof s->gte_ctrl);
}

/** @brief Write the saved set back into the CPU state.
 *
 * @param cpu the CPU state.
 * @param s the copy. */
static void regs_restore(CPUState* cpu, const saved_regs_t* s) {
    memcpy(&cpu->at, s->at_t7, sizeof s->at_t7);
    cpu->t8 = s->t8; cpu->t9 = s->t9;
    cpu->hi = s->hi; cpu->lo = s->lo; cpu->sp = s->sp;
    memcpy(cpu->gte_data, s->gte_data, sizeof s->gte_data);
    memcpy(cpu->gte_ctrl, s->gte_ctrl, sizeof s->gte_ctrl);
}

static _Atomic unsigned long long s_runs;      /* handlers entered */
static _Atomic unsigned long long s_runs_spec; /* ... while openpete_speculative was set */
static _Atomic unsigned long long s_runs_replay; /* ... while openpete_replaying was set */
static _Atomic unsigned long long s_rollbacks_bus; /* runahead rollbacks over a live bus frame */
static _Atomic unsigned long long s_boundaries_bus; /* tick boundaries over a live bus frame */
static _Atomic unsigned s_boundary_bus_last; /* the canonical tick of the last of them */

/* Result of one handler: its value, or a walker condition. */
enum { RUN_SKIPPED = -100, RUN_FULL = -101 };

/** @brief Run one snapshot row's handler if its owner is still live, inside a bus frame and the D1 register save.
 *
 * The owner is re-checked first: enabled, api not closed, and its
 * retirement count equal to the row's, so a handler whose owner changed
 * since the snapshot was built (in a tick boundary that ran while an
 * earlier handler had the fiber parked) is skipped. sp is lowered by the
 * 64-byte reserve, 8-aligned, before the call. A negative handler result is
 * logged once per owner and bus and read as OP_BUS_PASS.
 * @param r the row.
 * @param bus the bus's interned name.
 * @param cpu the sender's CPU state.
 * @param from the sender record.
 * @param msg the message address.
 * @param size the message length.
 * @return the handler's value (>= 0), RUN_SKIPPED, or RUN_FULL when the
 *         frame stack is full. */
static int run_row(const snap_row_t* r, const char* bus, CPUState* cpu,
                   const openpete_mod_bus_sender_t* from, uint32_t msg, uint32_t size) {
    if (!mod_owner_enabled(r->owner) || mod_owner_closed(r->owner) ||
        mod_owner_retired(r->owner) != r->retired)
        return RUN_SKIPPED;
    openpete_mods_bracket_t b;
    if (openpete_mods_frame_enter_bus(&b, r->id, bus) != 0) return RUN_FULL;
    saved_regs_t save;
    regs_save(&save, cpu);
    cpu->sp = (cpu->sp - BUS_SP_RESERVE) & ~7u;
    atomic_fetch_add_explicit(&s_runs, 1, memory_order_relaxed);
    if (openpete_speculative)
        atomic_fetch_add_explicit(&s_runs_spec, 1, memory_order_relaxed);
    if (openpete_replaying)
        atomic_fetch_add_explicit(&s_runs_replay, 1, memory_order_relaxed);
    int rc = r->fn(r->owner, cpu, from, msg, size, r->user);
    regs_restore(cpu, &save);
    openpete_mods_frame_leave(&b);
    if (rc < 0) {
        if (once(r->owner, "negative", bus))
            OPL_WARN_F(mod_owner_tag(r->owner), "bus \"%s\": handler returned %d, "
                       "read as OP_BUS_PASS (return 0 to pass, a positive value "
                       "to stop; logged once per bus)", bus, rc);
        rc = OP_BUS_PASS;
    }
    return rc;
}

/** @brief Refuse or admit a bus_call or bus_call_to, and find the bus's member run.
 *
 * @param self the caller.
 * @param entry the entry name.
 * @param cpu the caller's CPU state.
 * @param bus the bus name.
 * @param msg the message address.
 * @param size the message length.
 * @param run receives the run, NULL when no enabled handler is attached.
 * @param snap receives the snapshot the run belongs to.
 * @return 0 to walk, -1 refused, OP_BUS_NONE, or OP_BUS_DEPTH. */
static int call_admit(openpete_mod_t* self, const char* entry, CPUState* cpu,
                      const char* bus, uint32_t msg, uint32_t size,
                      const snap_bus_t** run, const snapshot_t** snap) {
    if (owner_refused(self, entry, 1)) return -1;
    if (!bus_name_valid(bus)) return openpete_mods_bus_refuse(self, entry, "a bus name is <name>/<major>, the major 1 or more");
    const char* why = call_context_refusal();
    if (why) return openpete_mods_bus_refuse(self, entry, why);
    if (!cpu) return openpete_mods_bus_refuse(self, entry, "a NULL CPUState");
    if (!range_ok(msg, size))
        return openpete_mods_bus_refuse(self, entry, "[msg_addr, msg_addr + size) is not one "
                      "contiguous view of guest memory");
    *snap = atomic_load(&s_snap);
    *run = snap_find(*snap, bus);
    if (!*run) return OP_BUS_NONE;
    if (openpete_mods_site_frames(OPENPETE_MODS_FRAME_BUS) >= BUS_DEPTH_MAX) {
        if (once(self, "depth", (*run)->name))
            OPL_ERROR_F(mod_owner_tag(self), "%s on bus \"%s\": %d nested bus "
                        "calls, the limit - OP_BUS_DEPTH (logged once per bus)",
                        entry, (*run)->name, BUS_DEPTH_MAX);
        return OP_BUS_DEPTH;
    }
    return 0;
}

/** @brief Log a frame-stack overflow once per caller and bus, and return OP_BUS_DEPTH.
 *
 * @param self the caller.
 * @param bus the interned bus name. */
static int frames_full(const openpete_mod_t* self, const char* bus) {
    if (once(self, "frames", bus))
        OPL_ERROR_F(mod_owner_tag(self), "bus \"%s\": the override frame stack "
                    "is full - OP_BUS_DEPTH (logged once per bus)", bus);
    return OP_BUS_DEPTH;
}

/** @brief Fill a sender record for a caller.
 *
 * @param from the record.
 * @param self the caller, keyed (owner_refused passed). */
static void sender_fill(openpete_mod_bus_sender_t* from, const openpete_mod_t* self) {
    from->struct_size = BUS_SENDER_SIZE_13;
    from->key = mod_owner_key(self);
    from->id  = openpete_mods_key_id(from->key);
}

int openpete_mods_bus_call(openpete_mod_t* self, CPUState* cpu, const char* bus,
                           uint32_t msg_addr, uint32_t size) {
    const snap_bus_t* run = NULL;
    const snapshot_t* snap = NULL;
    const int adm = call_admit(self, "bus_call", cpu, bus, msg_addr, size, &run, &snap);
    if (adm) return adm;
    openpete_mod_bus_sender_t from;
    sender_fill(&from, self);
    int ran = 0;
    for (int i = 0; i < run->n; i++) {
        const int rc = run_row(&snap->rows[run->first + i], run->name, cpu,
                               &from, msg_addr, size);
        if (rc == RUN_SKIPPED) continue;
        if (rc == RUN_FULL) return frames_full(self, run->name);
        ran = 1;
        if (rc > 0) return rc;
    }
    return ran ? OP_BUS_PASS : OP_BUS_NONE;
}

int openpete_mods_bus_call_to(openpete_mod_t* self, CPUState* cpu,
                              const char* bus, uint32_t key, uint32_t msg_addr,
                              uint32_t size) {
    const snap_bus_t* run = NULL;
    const snapshot_t* snap = NULL;
    const int adm = call_admit(self, "bus_call_to", cpu, bus, msg_addr, size, &run, &snap);
    if (adm) return adm;
    for (int i = 0; i < run->n; i++) {
        const snap_row_t* r = &snap->rows[run->first + i];
        if (r->key != key || key == 0) continue;
        openpete_mod_bus_sender_t from;
        sender_fill(&from, self);
        const int rc = run_row(r, run->name, cpu, &from, msg_addr, size);
        if (rc == RUN_FULL) return frames_full(self, run->name);
        return rc == RUN_SKIPPED ? OP_BUS_NONE : rc;
    }
    return OP_BUS_NONE;
}

int openpete_mods_bus_attach(openpete_mod_t* self, const char* bus,
                             openpete_mod_bus_fn fn, void* user) {
    if (owner_refused(self, "bus_attach", 1)) return -1;
    if (!bus_name_valid(bus) || !fn)
        return openpete_mods_bus_refuse(self, "bus_attach", "a NULL handler or a bus name not of "
                      "the form <name>/<major>");
    const openpete_mods_ctx ctx = openpete_mods_site_now().ctx;
    if (ctx != OPENPETE_MODS_CTX_ENTRY && ctx != OPENPETE_MODS_CTX_RELOAD_ENTRY)
        return openpete_mods_bus_refuse(self, "bus_attach", "bus_attach is legal in the entry point only");
    const char* iname = openpete_mods_bus_intern(bus);
    const handler_t h = { self, openpete_mods_key_id(mod_owner_key(self)), fn, user };
    if (!iname || table_attach(iname, &h) != 0)
        return openpete_mods_bus_refuse(self, "bus_attach", "the bus table could not grow");
    return 0;
}

/** @brief Refuse a caller-sized record below the api-13 sender record.
 *
 * @param self the caller.
 * @param entry the entry name.
 * @param size the caller's struct_size.
 * @return 0, or -1 refused (logged once). */
static int record_size_refused(const openpete_mod_t* self, const char* entry, uint32_t size) {
    if (size >= BUS_SENDER_SIZE_13) return 0;
    return openpete_mods_bus_refuse(self, entry, "struct_size is below sizeof(openpete_mod_bus_sender_t) "
                  "of api 13");
}

/** @brief Write a sender record into a caller-sized slot: at most the caller's size, its struct_size kept.
 *
 * @param out the slot.
 * @param size the caller's struct_size.
 * @param id the interned id.
 * @param key the key. */
static void record_write(void* out, uint32_t size, const char* id, uint32_t key) {
    openpete_mod_bus_sender_t rec;
    memset(&rec, 0, sizeof rec);
    rec.struct_size = size;
    rec.id = id;
    rec.key = key;
    memcpy(out, &rec, size < sizeof rec ? size : sizeof rec);
}

int openpete_mods_bus_members(openpete_mod_t* self, const char* bus,
                              openpete_mod_bus_sender_t* out, int cap) {
    if (owner_refused(self, "bus_members", 0)) return -1;
    if (!bus_name_valid(bus))
        return openpete_mods_bus_refuse(self, "bus_members", "a bus name is <name>/<major>, the major 1 or more");
    if (cap < 0 || (cap > 0 && !out))
        return openpete_mods_bus_refuse(self, "bus_members", "a negative cap or a NULL out with a cap");
    const uint32_t stride = cap > 0 ? out[0].struct_size : BUS_SENDER_SIZE_13;
    if (record_size_refused(self, "bus_members", stride)) return -1;
    const snapshot_t* snap = atomic_load(&s_snap);
    const snap_bus_t* run = snap_find(snap, bus);
    if (!run) return 0;
    for (int i = 0; i < run->n && i < cap; i++) {
        const snap_row_t* r = &snap->rows[run->first + i];
        record_write((unsigned char*)out + (size_t)i * stride, stride, r->id, r->key);
    }
    return run->n;
}

uint32_t openpete_mods_bus_mod_key(openpete_mod_t* self) {
    return self ? mod_owner_key(self) : 0u;
}

int openpete_mods_bus_mod_info(openpete_mod_t* self, uint32_t key,
                               openpete_mod_bus_sender_t* out) {
    if (!self || !out) return -1;
    if (record_size_refused(self, "mod_info", out->struct_size)) return -1;
    const char* id = openpete_mods_key_id(key);
    if (!id) return -1;
    record_write(out, out->struct_size, id, key);
    return 0;
}

void openpete_mods_bus_note_rollback(void) {
    if (openpete_mods_frames_of_kind(OPENPETE_MODS_FRAME_BUS) > 0)
        atomic_fetch_add_explicit(&s_rollbacks_bus, 1, memory_order_relaxed);
}

void openpete_mods_bus_note_boundary(unsigned tick) {
    if (openpete_mods_frames_of_kind(OPENPETE_MODS_FRAME_BUS) <= 0) return;
    atomic_fetch_add_explicit(&s_boundaries_bus, 1, memory_order_relaxed);
    atomic_store_explicit(&s_boundary_bus_last, tick, memory_order_relaxed);
}

void openpete_mods_bus_note_state(const char* what) {
    if (!table_buses()) return;
    OPL_INFO_F("mods", "bus: %d bus frame(s) live at the state %s",
               openpete_mods_frames_of_kind(OPENPETE_MODS_FRAME_BUS), what);
}

void openpete_mods_bus_report(void) {
    const unsigned long long runs = atomic_load(&s_runs);
    if (!runs && !table_buses()) return;
    OPL_INFO_F("mods", "bus: buses=%d handler runs=%llu speculative=%llu "
               "replaying=%llu snapshots=%lu", table_buses(), runs,
               (unsigned long long)atomic_load(&s_runs_spec),
               (unsigned long long)atomic_load(&s_runs_replay),
               (unsigned long)atomic_load(&s_published));
    OPL_INFO_F("mods", "bus: rollbacks over a bus frame=%llu; tick boundaries over a "
               "bus frame=%llu, the last at tick %u",
               (unsigned long long)atomic_load(&s_rollbacks_bus),
               (unsigned long long)atomic_load(&s_boundaries_bus),
               atomic_load(&s_boundary_bus_last));
}
