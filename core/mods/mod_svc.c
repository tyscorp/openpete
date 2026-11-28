// core/mods/mod_svc.c - service tables: function tables one mod exports and
// other mods call directly, from any thread the provider permits.
//
// svc_export copies a table into an engine-owned block at entry. The export
// table holds, per service (name and major), its providers newest entry
// first, as bus handlers are ordered; it is copy-on-write and written only
// at entry and at a strip, on the sim thread or on the main thread during
// boot. openpete_mods_bus_publish builds the service part of the membership
// snapshot from it (the exports whose owner is enabled), so svc_get and
// svc_list read one immutable snapshot from any thread, with one hash
// probe per lookup.
//
// Lifetime: a table copy is never freed, and the loader never unloads an
// object, so a pointer svc_get returned and the code it points into stay
// valid for the process. A strip (hot reload, failed reload, failed entry)
// removes the owner's exports before the snapshot is republished, so a
// lookup after the strip never returns the superseded copy. The provider's
// own state ends at its shutdown hook; a call that arrives through an old
// pointer meets the provider's admission gate.

#include "mod_bus.h"
#include "openpete_mods.h"
#include "openpete_log.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define SVC_NAME_MAX 95 /* longest accepted service name, NUL excluded */

/* ---- the export table --------------------------------------------------- */

typedef struct {
    openpete_mod_t* owner;
    const void*     block; /* the engine's copy, never freed */
    uint32_t        size;
} export_t;

typedef struct {
    const char* name; /* interned */
    uint32_t    major;
    int         first, n; /* rows [first, first + n), [first] newest entry */
} tab_svc_t;

/* Immutable once swapped in: export and strip build a new table. Only the
 * thread that publishes reads it; a superseded table is freed at the swap
 * (the blocks its rows point at are not). */
typedef struct {
    int              n_svc, n_rows;
    const tab_svc_t* svcs;
    const export_t*  rows;
} svc_table_t;

static _Atomic(const svc_table_t*) s_svc_table;

static _Atomic unsigned long      s_exports; /* successful svc_export calls */
static _Atomic unsigned long long s_gets;    /* svc_get lookups admitted */
static _Atomic unsigned long long s_hits;    /* ... that returned a table */
static _Atomic unsigned long long s_lists;   /* svc_list calls admitted */

/** @brief Allocate an empty export table with room for a number of services and rows.
 *
 * @param n_svc service capacity.
 * @param n_rows row capacity.
 * @return the table, its arrays writable through casts, or NULL. */
static svc_table_t* svc_table_alloc(int n_svc, int n_rows) {
    svc_table_t* t = (svc_table_t*)malloc(sizeof *t + (size_t)n_svc * sizeof(tab_svc_t) +
                                          (size_t)n_rows * sizeof(export_t));
    if (!t) return NULL;
    t->n_svc = 0;
    t->n_rows = 0;
    t->svcs = (const tab_svc_t*)(t + 1);
    t->rows = (const export_t*)(t->svcs + n_svc);
    return t;
}

/** @brief Swap in a new export table and free the one it replaces.
 *
 * @param t the new table. */
static void svc_table_swap(const svc_table_t* t) {
    free((void*)atomic_exchange(&s_svc_table, t));
}

/** @brief Copy one service's rows into a table being built, with an export put in.
 *
 * The export replaces the owner's row in place when it has one, else goes
 * to the front.
 * @param t the table being built.
 * @param old the old table.
 * @param b the service.
 * @param e the export, or NULL to copy the rows unchanged. */
static void svc_copy_run(svc_table_t* t, const svc_table_t* old, const tab_svc_t* b,
                         const export_t* e) {
    export_t* rows = (export_t*)t->rows;
    int replaced = 0;
    for (int j = 0; e && j < b->n; j++)
        replaced |= old->rows[b->first + j].owner == e->owner;
    if (e && !replaced) rows[t->n_rows++] = *e;
    for (int j = 0; j < b->n; j++) {
        const export_t* r = &old->rows[b->first + j];
        rows[t->n_rows++] = (e && r->owner == e->owner) ? *e : *r;
    }
}

/** @brief Publish an export table equal to the current one with an export put in.
 *
 * @param name the interned service name.
 * @param major its major.
 * @param e the export row.
 * @return 0, or -1 on an allocation failure (the table is unchanged). */
static int svc_table_put(const char* name, uint32_t major, const export_t* e) {
    const svc_table_t* old = atomic_load(&s_svc_table);
    const int os = old ? old->n_svc : 0, orows = old ? old->n_rows : 0;
    svc_table_t* t = svc_table_alloc(os + 1, orows + 1);
    if (!t) return -1;
    tab_svc_t* svcs = (tab_svc_t*)t->svcs;
    int found = 0;
    for (int i = 0; i < os; i++) {
        const tab_svc_t* b = &old->svcs[i];
        const int mine = b->name == name && b->major == major;
        tab_svc_t* nb = &svcs[t->n_svc++];
        *nb = (tab_svc_t){ b->name, b->major, t->n_rows, 0 };
        svc_copy_run(t, old, b, mine ? e : NULL);
        nb->n = t->n_rows - nb->first;
        found |= mine;
    }
    if (!found) {
        svcs[t->n_svc++] = (tab_svc_t){ name, major, t->n_rows, 1 };
        ((export_t*)t->rows)[t->n_rows++] = *e;
    }
    svc_table_swap(t);
    return 0;
}

void openpete_mods_svc_remove_owner(const openpete_mod_t* m) {
    const svc_table_t* old = atomic_load(&s_svc_table);
    if (!old) return;
    svc_table_t* t = svc_table_alloc(old->n_svc, old->n_rows);
    if (!t) {
        OPL_ERROR_F("mods", "service table allocation failed - the stripped mod's "
                    "exports stay listed; its retirement count keeps lookups "
                    "from returning them");
        return;
    }
    tab_svc_t* svcs = (tab_svc_t*)t->svcs;
    export_t* rows = (export_t*)t->rows;
    for (int i = 0; i < old->n_svc; i++) {
        const tab_svc_t* b = &old->svcs[i];
        tab_svc_t* nb = &svcs[t->n_svc++];
        *nb = (tab_svc_t){ b->name, b->major, t->n_rows, 0 };
        for (int j = 0; j < b->n; j++)
            if (old->rows[b->first + j].owner != m) rows[t->n_rows++] = old->rows[b->first + j];
        nb->n = t->n_rows - nb->first;
    }
    svc_table_swap(t);
}

/* ---- the service snapshot ------------------------------------------------ */

typedef struct {
    openpete_mod_t* owner;
    uint32_t        key;
    const void*     block;
    uint32_t        size;
    uint32_t        retired; /* the owner's retirement count at the build */
} svc_row_t;

typedef struct {
    const char* name; /* interned */
    uint32_t    major;
    int         first, n;
} svc_run_t;

struct openpete_mods_svc_snap {
    int              n_svc;
    uint32_t         mask;  /* slot count - 1, a power of two minus one */
    const int*       slots; /* run index per hash slot, -1 empty */
    const svc_run_t* runs;
    const svc_row_t* rows;
};

/** @brief Hash a service name and major (FNV-1a over the name's bytes, then the major's four bytes).
 *
 * @param name the name.
 * @param major the major.
 * @return the hash. */
static uint32_t svc_hash(const char* name, uint32_t major) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)name; *p; p++) h = (h ^ *p) * 16777619u;
    for (int i = 0; i < 4; i++) h = (h ^ ((major >> (8 * i)) & 0xFFu)) * 16777619u;
    return h;
}

/** @brief Count the services and rows of the export table whose owner is enabled.
 *
 * @param tab the export table, or NULL.
 * @param n_svc receives the services with at least one enabled provider.
 * @param n_rows receives the enabled providers. */
static void svc_count_live(const svc_table_t* tab, size_t* n_svc, size_t* n_rows) {
    *n_svc = 0;
    *n_rows = 0;
    for (int i = 0; tab && i < tab->n_svc; i++) {
        size_t live = 0;
        for (int j = 0; j < tab->svcs[i].n; j++)
            live += mod_owner_enabled(tab->rows[tab->svcs[i].first + j].owner) != 0;
        if (live) { (*n_svc)++; *n_rows += live; }
    }
}

const openpete_mods_svc_snap_t* openpete_mods_svc_snap_build(void) {
    const svc_table_t* tab = atomic_load(&s_svc_table);
    size_t n_svc, n_rows;
    svc_count_live(tab, &n_svc, &n_rows);
    size_t n_slots = 1;
    while (n_slots < 2 * n_svc) n_slots <<= 1;
    openpete_mods_svc_snap_t* s = (openpete_mods_svc_snap_t*)malloc(
        sizeof *s + n_slots * sizeof(int) + n_svc * sizeof(svc_run_t) + n_rows * sizeof(svc_row_t));
    if (!s) return NULL;
    int* slots = (int*)(s + 1);
    svc_run_t* runs = (svc_run_t*)(slots + n_slots);
    svc_row_t* rows = (svc_row_t*)(runs + n_svc);
    for (size_t i = 0; i < n_slots; i++) slots[i] = -1;
    s->mask = (uint32_t)(n_slots - 1);
    int ri = 0, si = 0;
    for (int i = 0; tab && i < tab->n_svc; i++) {
        const tab_svc_t* b = &tab->svcs[i];
        const int first = ri;
        for (int j = 0; j < b->n; j++) {
            const export_t* e = &tab->rows[b->first + j];
            if (!mod_owner_enabled(e->owner)) continue;
            rows[ri++] = (svc_row_t){ e->owner, mod_owner_key(e->owner), e->block, e->size,
                                      mod_owner_retired(e->owner) };
        }
        if (ri == first) continue;
        runs[si] = (svc_run_t){ b->name, b->major, first, ri - first };
        uint32_t h = svc_hash(b->name, b->major) & s->mask;
        while (slots[h] >= 0) h = (h + 1) & s->mask;
        slots[h] = si++;
    }
    s->n_svc = si;
    s->slots = slots;
    s->runs = runs;
    s->rows = rows;
    return s;
}

/** @brief Find a service's provider run in a snapshot by one hash probe sequence.
 *
 * @param s the service snapshot, or NULL.
 * @param name the name.
 * @param major the major.
 * @return the run, or NULL when no enabled mod exports it. */
static const svc_run_t* svc_find(const openpete_mods_svc_snap_t* s, const char* name,
                                 uint32_t major) {
    if (!s || s->n_svc == 0) return NULL;
    for (uint32_t h = svc_hash(name, major) & s->mask;; h = (h + 1) & s->mask) {
        const int i = s->slots[h];
        if (i < 0) return NULL;
        if (s->runs[i].major == major && strcmp(s->runs[i].name, name) == 0) return &s->runs[i];
    }
}

/** @brief Return 1 while a snapshot row's owner is enabled, open and not retired since the build.
 *
 * Narrows the window between a strip or toggle and the republish that
 * follows it; the provider's admission gate covers the rest.
 * @param r the row. */
static int svc_row_live(const svc_row_t* r) {
    return mod_owner_enabled(r->owner) && !mod_owner_closed(r->owner) &&
           mod_owner_retired(r->owner) == r->retired;
}

/* ---- the api entries ------------------------------------------------------ */

/** @brief Return 1 for a well-formed service name and major: a non-empty name of at most 95 bytes, a major of 1 or more.
 *
 * @param name the name, or NULL.
 * @param major the major. */
static int svc_name_valid(const char* name, uint32_t major) {
    return name && *name && strlen(name) <= SVC_NAME_MAX && major >= 1;
}

int openpete_mods_svc_export(openpete_mod_t* self, const char* name, uint32_t major,
                             const void* table, uint32_t size) {
    if (!self) return -1;
    if (!mod_owner_key(self))
        return openpete_mods_bus_refuse(self, "svc_export", "this mod is keyless (its key is "
                                        "0, collides with another mod's, or its id is 64 "
                                        "bytes or longer)");
    if (!svc_name_valid(name, major) || !table || size == 0)
        return openpete_mods_bus_refuse(self, "svc_export", "an empty or NULL name, a major "
                                        "of 0, a NULL table or a size of 0");
    const openpete_mods_ctx ctx = openpete_mods_site_now().ctx;
    if (ctx != OPENPETE_MODS_CTX_ENTRY && ctx != OPENPETE_MODS_CTX_RELOAD_ENTRY)
        return openpete_mods_bus_refuse(self, "svc_export", "svc_export is legal in the "
                                        "entry point only");
    const char* iname = openpete_mods_bus_intern(name);
    void* block = iname ? malloc(size) : NULL;
    if (!block)
        return openpete_mods_bus_refuse(self, "svc_export", "the table copy could not be "
                                        "allocated");
    memcpy(block, table, size);
    const export_t e = { self, block, size };
    if (svc_table_put(iname, major, &e) != 0) {
        free(block);
        return openpete_mods_bus_refuse(self, "svc_export", "the service table could not grow");
    }
    atomic_fetch_add_explicit(&s_exports, 1, memory_order_relaxed);
    return 0;
}

const void* openpete_mods_svc_get(openpete_mod_t* self, uint32_t provider_key,
                                  const char* name, uint32_t major, uint32_t min_size) {
    if (!self) return NULL;
    if (!mod_owner_key(self)) {
        openpete_mods_bus_refuse(self, "svc_get", "this mod is keyless (its key is 0, "
                                 "collides with another mod's, or its id is 64 bytes or "
                                 "longer)");
        return NULL;
    }
    if (!svc_name_valid(name, major)) {
        openpete_mods_bus_refuse(self, "svc_get", "an empty or NULL name or a major of 0");
        return NULL;
    }
    atomic_fetch_add_explicit(&s_gets, 1, memory_order_relaxed);
    if (provider_key == 0) return NULL;
    const openpete_mods_svc_snap_t* s = openpete_mods_bus_snap_svc(); /* loaded once */
    const svc_run_t* run = svc_find(s, name, major);
    for (int i = 0; run && i < run->n; i++) {
        const svc_row_t* r = &s->rows[run->first + i];
        if (r->key != provider_key) continue;
        if (r->size < min_size || !svc_row_live(r)) return NULL;
        atomic_fetch_add_explicit(&s_hits, 1, memory_order_relaxed);
        return r->block;
    }
    return NULL;
}

int openpete_mods_svc_list(openpete_mod_t* self, const char* name, uint32_t major,
                           uint32_t* keys_out, int cap) {
    if (!self) return -1;
    if (!svc_name_valid(name, major))
        return openpete_mods_bus_refuse(self, "svc_list", "an empty or NULL name or a major of 0");
    if (cap < 0 || (cap > 0 && !keys_out))
        return openpete_mods_bus_refuse(self, "svc_list", "a negative cap or a NULL keys_out "
                                        "with a cap");
    if (!mod_owner_key(self)) {
        openpete_mods_bus_refuse(self, "svc_list", "this mod is keyless (its key is 0, "
                                 "collides with another mod's, or its id is 64 bytes or "
                                 "longer)");
        return 0;
    }
    atomic_fetch_add_explicit(&s_lists, 1, memory_order_relaxed);
    const openpete_mods_svc_snap_t* s = openpete_mods_bus_snap_svc();
    const svc_run_t* run = svc_find(s, name, major);
    int n = 0;
    for (int i = 0; run && i < run->n; i++) {
        const svc_row_t* r = &s->rows[run->first + i];
        if (!svc_row_live(r)) continue;
        if (n < cap) keys_out[n] = r->key;
        n++;
    }
    return n;
}

void openpete_mods_svc_report(void) {
    const unsigned long exports = atomic_load(&s_exports);
    if (!exports) return;
    OPL_INFO_F("mods", "svc: exports=%lu gets=%llu hits=%llu lists=%llu", exports,
               (unsigned long long)atomic_load(&s_gets), (unsigned long long)atomic_load(&s_hits),
               (unsigned long long)atomic_load(&s_lists));
}
