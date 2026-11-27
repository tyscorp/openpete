/* games/common/renderer/interp_regions_gen.c - the interp region table
 * generator. See renderer/interp_regions_gen.h. */

#include "renderer/interp_regions_gen.h"
#include "openpete_thread_roles.h"

#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "openpete_psx.h"          /* openpete_psx_to_host_span */
#include "openpete_str.h"          /* op_appendf */
#include "game_layout.h"
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"     /* cut_jump_census_capture */
#include "renderer/credits_key.h"   /* game_credits_track, game_credits_letter_key */
#include "renderer/tracers.h"      /* the mesh vertex lists the tick drew */
#include "renderer/particles.h"    /* particles_slot_serial */
#include "renderer/moby_serial.h"  /* moby_slot_serial */
#include "renderer/orchestrator.h" /* the canonical-render epoch the moby stash carries */
#include "renderer/moby.h"         /* the native view's drawn sets */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define census_write(fd, buf, n) _write((fd), (buf), (unsigned)(n))
#else
#include <unistd.h>
#define census_write(fd, buf, n) write((fd), (buf), (size_t)(n))
#endif

/* The ring captures the table by pointer, so the built table is the storage it
 * reads from for the process's life. */
static openpete_interp_region_t s_regions[OPENPETE_INTERP_MAX_REGIONS];
static unsigned                 s_count;

/* OPENPETE_INTERP_REGION_CENSUS=1: per region, how many ticks the capture ran
 * and on how many of them the region's bytes differed from the previous tick's.
 * A row that never changes is capturing the wrong span (or a span the game does
 * not advance); a row one word off a struct base changes on a different set of
 * ticks than the base does. Sampled from guest RAM independently of the ring, so
 * the ring's whole-snapshot dedupe cannot hide a per-region result. Its hook is
 * the per-object capture, which the ring runs only while interpolation capture
 * is on, so a headless census run passes --render-fps above the tick rate. */
static struct {
    unsigned char* prev;
    int            have_prev;
    unsigned long  ticks;
    unsigned long  changed;
} s_census[OPENPETE_INTERP_MAX_REGIONS];

static openpete_interp_obj_capture_fn s_game_obj_capture;

/** @brief Whether OPENPETE_INTERP_REGION_CENSUS is set, non-empty and not "0"; read once and cached.
 *
 * @return 1 when the region census is armed, else 0. */
static int census_on(void) {
    static int on = -1;
    ENV_FLAG_LATCH(on, "OPENPETE_INTERP_REGION_CENSUS");
    return on;
}

int game_interp_rows_add_one(const char* name, uint32_t addr, uint32_t length,
                             int ra_commit) {
    if (!name || !addr || !length) return -1;
    if (s_count >= OPENPETE_INTERP_MAX_REGIONS) return -1;
    unsigned i = s_count++;
    s_regions[i].name      = name;
    s_regions[i].psx_addr  = addr;
    s_regions[i].length    = length;
    s_regions[i].ra_commit = ra_commit;
    return (int)i;
}

int game_interp_rows_add(const game_interp_row_t* rows, unsigned count) {
    int first = -1;
    for (unsigned r = 0; rows && r < count; ++r) {
        int i = game_interp_rows_add_one(rows[r].name, rows[r].addr,
                                         rows[r].length, rows[r].ra_commit);
        if (first < 0) first = i;
    }
    return first;
}

int game_interp_row_index(const char* name) {
    if (!name) return -1;
    for (unsigned i = 0; i < s_count; ++i)
        if (s_regions[i].name && strcmp(s_regions[i].name, name) == 0) return (int)i;
    return -1;
}

/** @brief Print the region census table, one row per registered region, at exit.
 *
 * Each row carries the region's name, address, length, timeline flag, the
 * ticks sampled and the ticks whose bytes changed. The table goes out in one
 * raw write to the stderr descriptor: the process mirrors stderr to a log
 * file and tears that stream down in its own exit hook, which can run before
 * this one and swallow anything written through the FILE*. A row that does
 * not fit is dropped whole with the rows after it, and a closing line names
 * how many rows were printed; the write covers only the bytes formatted. */
static void census_report(void) {
    OP_ROLE_ASSERT(sim);
    enum { TAIL = 96 };   /* room kept for the truncation line */
    char buf[128 * OPENPETE_INTERP_MAX_REGIONS + 128];
    size_t n = 0;
    unsigned rows = 0;
    (void)op_appendf(buf, sizeof buf - TAIL, &n,
                     "[interp-region-census] %-14s %-10s %6s %3s %10s %10s\n",
                     "region", "addr", "len", "rac", "ticks", "changed");
    for (; rows < s_count; ++rows) {
        const size_t row_start = n;
        if (op_appendf(buf, sizeof buf - TAIL, &n,
                       "[interp-region-census] %-14s 0x%08X %6u %3d %10lu %10lu\n",
                       s_regions[rows].name ? s_regions[rows].name : "?",
                       s_regions[rows].psx_addr, s_regions[rows].length,
                       s_regions[rows].ra_commit, s_census[rows].ticks,
                       s_census[rows].changed) != 0) {
            n = row_start;
            break;
        }
    }
    if (rows < s_count)
        (void)op_appendf(buf, sizeof buf, &n,
                         "[interp-region-census] truncated: %u of %u rows\n",
                         rows, s_count);
    fflush(stderr);
    const long written = (long)census_write(2, buf, n);
    (void)written;   /* a failed write to stderr has nowhere to be reported */
}

/** @brief Sample each registered region: count the tick and whether its bytes differ from the previous sample. */
static void census_sample(void) {
    for (unsigned i = 0; i < s_count; ++i) {
        uint32_t len = s_regions[i].length;
        const void* host = openpete_psx_to_host_span(s_regions[i].psx_addr, len);
        if (!host) continue;
        if (!s_census[i].prev) {
            s_census[i].prev = (unsigned char*)malloc(len);
            if (!s_census[i].prev) continue;
        }
        s_census[i].ticks++;
        if (s_census[i].have_prev && memcmp(s_census[i].prev, host, len) != 0)
            s_census[i].changed++;
        memcpy(s_census[i].prev, host, len);
        s_census[i].have_prev = 1;
    }
}

/** @brief The per-object capture installed while the region census is on: sample every region, then run the game's own capture.
 *
 * The ring's own capture (openpete_interp_capture) runs immediately before the
 * per-object capture, so sampling here reads the same tick-end RAM the ring
 * just captured. */
static void census_obj_capture(void) {
    census_sample();
    if (s_game_obj_capture) s_game_obj_capture();
}

void game_interp_rows_obj_capture(openpete_interp_obj_capture_fn fn,
                                  uint32_t blob_bytes, uint32_t slots) {
    s_game_obj_capture = fn;
    openpete_interp_register_obj_capture(census_on() ? census_obj_capture : fn,
                                         blob_bytes, slots);
}

extern unsigned openpete_frame;

/** @brief Read a guest word through a bounds-checked span; 0 when it does not map. */
static uint32_t rd32(uint32_t addr) {
    const void* p = openpete_psx_to_host_span(addr, 4u);
    uint32_t v = 0;
    if (p) memcpy(&v, p, 4);
    return v;
}

/** @brief The moby record bytes the per-object capture takes: one record and no bytes of the next.
 *
 * The lesser of the store's blob size and game_layout.moby.array_stride,
 * since game_interp_register_common may size the blob for the tracer ribbon
 * record, which is longer than a moby's.
 * @return The byte count. */
static uint32_t moby_blob_bytes(void) {
    const uint32_t stride = game_layout.moby.array_stride;
    return openpete_interp_objs_blob_bytes() < stride
               ? openpete_interp_objs_blob_bytes() : stride;
}

/* Draw-time moby records. A game that writes a moby's state after the tick's
 * draw read it (Spyro 2's HUD update, called from the render routine after the
 * moby draw, advances a HUD moby's angle) leaves the tick-end record one tick
 * ahead of the pose the tick drew. The draw hook stashes each queued moby's
 * record as the draw left it; every capture until the next draw
 * substitutes the stash for those mobys, so the object ring's pair holds drawn
 * states. The stash carries the canonical-render epoch of its draw: a draw in
 * a later epoch replaces it, draws in one epoch add to it, and a capture takes
 * it only while that epoch is in flight or is the last committed one, so a
 * present without the draw (a load, a menu) retires it. A speculative draw
 * blocks the stash until the next committed draw: the capture then runs at the
 * speculation head, whose draw is not the committed one, and takes live RAM.
 * Host memory only; guest RAM is read. */
#define MOBY_STASH_CAP 1024u
static struct {
    uint32_t key;
    uint32_t blob;     /* blob index, in stash order: the later stash of a moby wins */
} s_stash_ent[MOBY_STASH_CAP];
static unsigned char* s_stash_blob;
static uint32_t       s_stash_len;
static unsigned       s_stash_n;        /* entries */
static unsigned       s_stash_nblob;    /* blobs written since the replace */
static int            s_stash_sorted;
static int            s_stash_blocked;
static uint32_t       s_stash_epoch;

/** @brief Whether OPENPETE_MOBY_STASH_CENSUS is set, non-empty and not "0"; read once and cached.
 *
 * @return 1 when the stash census is armed, else 0. */
static int stash_census_on(void) {
    static int on = -1;
    ENV_FLAG_LATCH(on, "OPENPETE_MOBY_STASH_CENSUS");
    return on;
}

void game_interp_moby_stash_begin(void) OP_REQUIRES(sim) {
    extern int openpete_speculative;
    if (openpete_speculative) { s_stash_blocked = 1; return; }
    const uint32_t epoch = spyro_native_canon_epoch_stamp();
    if (s_stash_blocked || epoch != s_stash_epoch) {
        s_stash_n = s_stash_nblob = 0;
        s_stash_sorted = 1;
    }
    s_stash_blocked = 0;
    s_stash_epoch = epoch;
}

void game_interp_moby_stash_put(uint32_t moby) {
    if (s_stash_blocked || s_stash_nblob >= MOBY_STASH_CAP) return;
    const uint32_t len = moby_blob_bytes();
    if (!len) return;
    if (!s_stash_blob || s_stash_len != len) {
        unsigned char* b = (unsigned char*)realloc(s_stash_blob, (size_t)MOBY_STASH_CAP * len);
        if (!b) return;
        s_stash_blob = b;
        s_stash_len  = len;
        s_stash_n = s_stash_nblob = 0;
    }
    const void* host = openpete_psx_to_host_span(moby, len);
    if (!host) return;
    memcpy(s_stash_blob + (size_t)s_stash_nblob * len, host, len);
    s_stash_ent[s_stash_n].key  = moby;
    s_stash_ent[s_stash_n].blob = s_stash_nblob;
    s_stash_n++;
    s_stash_nblob++;
    s_stash_sorted = 0;
}

/** @brief qsort comparator over the stash entries: ascending moby address, then ascending stash order.
 *
 * Reads each entry as its two leading uint32_t fields (key, blob index).
 * @param a First entry.
 * @param b Second entry.
 * @return Negative, zero or positive as a sorts before, equal to or after b. */
static int stash_cmp(const void* a, const void* b) {
    const uint32_t ka = ((const uint32_t*)a)[0], kb = ((const uint32_t*)b)[0];
    if (ka != kb) return ka < kb ? -1 : 1;
    const uint32_t sa = ((const uint32_t*)a)[1], sb = ((const uint32_t*)b)[1];
    return sa < sb ? -1 : (sa > sb ? 1 : 0);
}

/** @brief Order the stash by moby address, keeping the last stash of each moby, and return the count the capture may use.
 *
 * Sorting on (address, stash order) and keeping the last entry per address
 * lets the ascending array walk consume the stash with one cursor. The sort
 * runs once per batch of puts.
 * @return The entry count, or 0 while the stash is blocked or its epoch is
 *         neither the current canonical-render epoch nor fresh. */
static unsigned stash_prepare(void) OP_REQUIRES(sim) {
    if (s_stash_blocked) return 0;
    if (s_stash_epoch != spyro_native_canon_epoch_stamp() &&
        !spyro_native_canon_epoch_fresh(s_stash_epoch)) return 0;
    if (!s_stash_sorted) {
        qsort(s_stash_ent, s_stash_n, sizeof s_stash_ent[0], stash_cmp);
        unsigned w = 0;
        for (unsigned r = 0; r < s_stash_n; ++r) {
            if (r + 1 < s_stash_n && s_stash_ent[r + 1].key == s_stash_ent[r].key) continue;
            s_stash_ent[w++] = s_stash_ent[r];
        }
        s_stash_n = w;
        s_stash_sorted = 1;
    }
    return s_stash_n;
}

/* The capture's cursor over the prepared stash, and its census counts:
 * mobys served from the stash, and of those the ones whose stashed record
 * differs from the tick-end record (OPENPETE_MOBY_STASH_CENSUS=1). */
typedef struct {
    unsigned n, cur, served, differ;
    int      census;
} stash_walk_t;

/** @brief Print the first byte at which a stashed moby record differs from the tick-end record.
 *
 * Prints nothing when the first `blob` bytes are equal. The line carries the
 * moby address, its class read from the tick-end record, the offset of the
 * first differing byte, and both values there.
 * @param m The moby's PSX address.
 * @param drawn The stashed record.
 * @param end The tick-end record.
 * @param blob Record bytes compared. */
static void stash_census_differ(uint32_t m, const uint8_t* drawn, const uint8_t* end,
                                uint32_t blob) {
    unsigned o = 0;
    while (o < blob && drawn[o] == end[o]) ++o;
    if (o >= blob) return;
    uint16_t c2;
    memcpy(&c2, end + game_layout.moby.class_off, 2);
    fprintf(stderr, "[moby-stash f=%u] differ m=%08x cls=%03x first=+0x%02x drawn=%02x end=%02x\n",
            openpete_frame, m, c2, o, drawn[o], end[o]);
}

/** @brief The record the capture takes for moby `m`: the stashed one when the stash holds `m`, else the live one.
 *
 * Advances the walk's cursor past smaller addresses, so `m` ascends across
 * calls. A stash whose record length differs from `blob` serves nothing.
 * Counts each served moby; with the census on, also counts and prints each
 * one whose stashed record differs from the live one.
 * @param w The walk's cursor and census counts.
 * @param m The moby's PSX address.
 * @param host The live record.
 * @param blob Record bytes the capture takes.
 * @return The record to capture. */
static const void* stash_source(stash_walk_t* w, uint32_t m, const void* host, uint32_t blob) {
    while (w->cur < w->n && s_stash_ent[w->cur].key < m) ++w->cur;
    if (w->cur >= w->n || s_stash_ent[w->cur].key != m || s_stash_len != blob) return host;
    const uint8_t* src = s_stash_blob + (size_t)s_stash_ent[w->cur].blob * blob;
    ++w->served;
    if (w->census && memcmp(src, host, blob) != 0) {
        ++w->differ;
        stash_census_differ(m, src, (const uint8_t*)host, blob);
    }
    return src;
}

/** @brief Print one moby's tick-end position when its class is the OPENPETE_CAPTRACE_CLS class (0xFFFF: every class).
 *
 * @param m The moby's PSX address.
 * @param host The live record.
 * @param cls The traced class. */
static void captrace_moby(uint32_t m, const uint8_t* host, long cls) {
    uint16_t c2;
    memcpy(&c2, host + game_layout.moby.class_off, 2);
    if (cls != 0xFFFF && c2 != (uint16_t)cls) return;
    int32_t pos[3];
    memcpy(pos, host + game_layout.moby.pos_off, 12);
    fprintf(stderr, "[cap-tick f=%u] m=%08x cls=%03x pos=(%d,%d,%d)\n",
            openpete_frame, m, c2, pos[0], pos[1], pos[2]);
}

/* The mobys the native view drew on the tick, as an open-addressed set of
 * guest addresses (0 marks an empty slot; no moby address is 0). */
#define VIEW_SET_SLOTS 4096u
static uint32_t s_view_set[VIEW_SET_SLOTS];

/** @brief Add each address of a moby list to the native view set, up to half the set's capacity.
 *
 * @param list The moby addresses.
 * @param n The list's length.
 * @param fill The set's population, updated. */
static void view_set_add(const uint32_t* list, int n, uint32_t* fill) {
    for (int i = 0; list && i < n && *fill < VIEW_SET_SLOTS / 2u; i++) {
        const uint32_t m = list[i];
        if (!m) continue;
        uint32_t h = (m * 2654435761u) >> 20;
        while (s_view_set[h & (VIEW_SET_SLOTS - 1u)] && s_view_set[h & (VIEW_SET_SLOTS - 1u)] != m) ++h;
        if (!s_view_set[h & (VIEW_SET_SLOTS - 1u)]) { s_view_set[h & (VIEW_SET_SLOTS - 1u)] = m; ++*fill; }
    }
}

/** @brief Rebuild the native view set from the tick's cull-record mobys and the canonical reveal admissions.
 *
 * Both lists are written by the canonical render passes of the tick the
 * capture follows, so the set is the tick's own; a sub-tick present cannot
 * change it.
 * @return The set's population. */
static uint32_t view_set_build(void) {
    memset(s_view_set, 0, sizeof s_view_set);
    uint32_t fill = 0;
    const uint32_t* list = NULL;
    int n = moby_native_canon_drawn_mobys(&list);
    view_set_add(list, n, &fill);
    n = moby_native_view_revealed_mobys(&list);
    view_set_add(list, n, &fill);
    return fill;
}

/** @brief Whether the native view drew moby @p m on the tick (view_set_build).
 *
 * @param m The moby's PSX address.
 * @return 1 when the set holds @p m, else 0. */
static int view_set_has(uint32_t m) {
    for (uint32_t h = (m * 2654435761u) >> 20;; ++h) {
        const uint32_t v = s_view_set[h & (VIEW_SET_SLOTS - 1u)];
        if (v == m) return 1;
        if (!v) return 0;
    }
}

/** @brief Whether a changed moby slot serial makes the capture a despawn and a spawn: game_layout.moby.alloc_respawn and OPENPETE_MOBY_REUSE_SPAWN (default on, read once).
 *
 * Off, the serial is still appended to the blob and declared to the store,
 * and the pair spans the two records a reused slot held (moby_pair_reborn
 * names it).
 * @return 1 when on, else 0. */
static int moby_reuse_spawn_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_REUSE_SPAWN"); v = (e && *e) ? (*e != '0') : 1; }
    return v && game_layout.moby.alloc_respawn;
}

/** @brief Capture one moby record under @p key with whether it was shown on the tick.
 *
 * Shown when the game's cull drew the record (its drawn byte) or the native
 * view drew it (a cull record or a reveal admission of the tick: the widened
 * view and extended LOD draw mobys the 4:3 cull leaves undrawn), so a spawn
 * parked unshown by both and placed into view on a later tick snaps on its
 * first shown tick. When the layout names a moby allocator and the store's
 * blob has room, the slot's allocation serial (moby_slot_serial) follows the
 * record's bytes and is declared to the store at that offset, and, where the
 * layout sets alloc_respawn, a serial other
 * than the tracked one's makes the capture a despawn of the tracked record and
 * a spawn of this one. With
 * OPENPETE_INTERP_OBJS_KEYLOG a replacement prints the moby, its class and
 * whether the native view drew it.
 * @param m The moby's PSX address.
 * @param host The live record, for the drawn byte and the class.
 * @param src The record captured (stashed or live).
 * @param blob Record bytes captured.
 * @param key The record's interp key.
 * @param view Nonzero when the native view set has entries this tick. */
static void capture_moby_shown(uint32_t m, const uint8_t* host, const void* src, uint32_t blob,
                               uint32_t key, int view) {
    const int native = view && view_set_has(m);
    const int shown  = host[game_layout.moby.drawn_off] != 0 || native;
    uint8_t buf[256];
    uint32_t len = blob, serial_off = 0u;
    if (moby_serial_enabled() && blob + 4u <= openpete_interp_objs_blob_bytes() && blob + 4u <= sizeof buf) {
        const uint32_t serial = moby_slot_serial(m);
        memcpy(buf, src, blob);
        memcpy(buf + blob, &serial, 4u);
        src = buf;
        len = blob + 4u;
        serial_off = blob;
    }
    if (!openpete_interp_objs_capture_serial(key, src, len, shown, serial_off, moby_reuse_spawn_on()) ||
        !openpete_interp_objs_keylog())
        return;
    uint16_t cls;
    memcpy(&cls, host + game_layout.moby.class_off, 2);
    fprintf(stderr, "[interp-objs] f=%u unshown-replace m=%08x cls=%03x native=%d\n",
            openpete_frame, m, cls, native);
}

const uint8_t* game_interp_capture_moby_record(uint32_t key, uint32_t m) {
    const uint32_t len = moby_blob_bytes();
    const uint8_t* host = (const uint8_t*)openpete_psx_to_host_span(m, len);
    if (host && len) openpete_interp_objs_capture(key, host, len);
    return host;
}

/** @brief Move the record of each continued host allocation that took another slot (moby_serial_take_hops) to its new slot's key, before the walk captures either slot.
 *
 * A move applies only when the record tracked under the old slot's key is the
 * one that moved: its curr declared the slot serial and carries the move's
 * serial. The record the new slot's key held before goes to the evicted set
 * (openpete_interp_objs_rekey). */
static void moby_apply_hops(void) OP_REQUIRES(sim) {
    uint32_t from[64], to[64], serial[64], vf[64], vt[64];
    const uint32_t n = moby_serial_take_hops(from, to, serial, 64u);
    const uint32_t so = game_layout.moby.array_stride;
    uint32_t v = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const openpete_interp_obj_t* o = openpete_interp_objs_find(game_credits_letter_key(from[k]));
        if (!o || o->curr_serial_off != so || memcmp(o->curr + so, &serial[k], 4u) != 0) continue;
        vf[v] = game_credits_letter_key(from[k]);
        vt[v] = game_credits_letter_key(to[k]);
        v++;
    }
    if (v) openpete_interp_objs_rekey(vf, vt, v);
}

void game_interp_capture_moby_array(void) OP_REQUIRES(sim) {
    stash_walk_t sw = { stash_prepare(), 0, 0, 0, stash_census_on() };
    if (moby_serial_enabled()) moby_apply_hops();

    const uint32_t head = game_layout.moby.array_head;
    if (!head) return;
    uint32_t arr = game_layout.moby.array_direct ? head : rd32(head);
    if (arr < 0x80000000u) return;

    const uint32_t stride = game_layout.moby.array_stride;
    const uint32_t blob   = moby_blob_bytes();
    /* OPENPETE_CAPTRACE_CLS=<class|0xFFFF>: the per-TICK canonical position of
     * every moby of that class, the capture side of the staircase-vs-smooth
     * comparison against the sub-tick trace. */
    static long s_ctc = -2;
    if (s_ctc == -2) { const char* e = getenv("OPENPETE_CAPTRACE_CLS"); s_ctc = e ? strtol(e, NULL, 0) : -1; }

    /* Bound the walk below the store capacity, leaving room for the pools
     * captured after it so a full array can never crowd them out; the array's
     * sentinel stops the walk after the live prefix anyway. */
    const long bound = (long)openpete_interp_objs_slots() - 512;
    /* The census runs after the walk, so a record whose marker reads another
     * record (an attachment's parent later in the array) sees that record's
     * capture of this tick. */
    static uint32_t s_live[4096];
    uint32_t n_live = 0;
    const uint32_t view_n = view_set_build();
    uint32_t m = arr;
    for (long guard = 0; guard < bound; ++guard) {
        const void* host = openpete_psx_to_host_span(m, stride);
        if (!host) break;
        uint8_t st = ((const uint8_t*)host)[game_layout.moby.state_off];
        if (st == (uint8_t)game_layout.moby.state_term) break;   /* end of the array */
        if (st < 0x80) {                                         /* 0x80 and above: dormant, skipped */
            const void* src = stash_source(&sw, m, host, blob);
            const uint32_t key = game_credits_letter_key(m);
            capture_moby_shown(m, (const uint8_t*)host, src, blob, key, view_n != 0);
            cut_props_capture((const uint8_t*)src);
            if (n_live < sizeof s_live / sizeof s_live[0]) s_live[n_live++] = key;
            if (s_ctc >= 0) captrace_moby(m, (const uint8_t*)host, s_ctc);
        }
        m += stride;
    }
    for (uint32_t i = 0; i < n_live; ++i) cut_jump_census_capture(s_live[i]);
    if (sw.census)
        fprintf(stderr, "[moby-stash f=%u] stashed=%u served=%u differ=%u\n",
                openpete_frame, sw.n, sw.served, sw.differ);
}

/* Particle pool walk; see interp_regions_gen.h. */
void game_interp_capture_particle_pool(void) {
    if (!game_layout.particles.array_ptr || !game_layout.particles.rec_stride) return;
    const uint32_t base = rd32(game_layout.particles.array_ptr);
    if (base < 0x80000000u) return;
    const uint32_t stride = game_layout.particles.rec_stride;
    /* The record, then the slot's allocation serial (particles_slot_serial)
     * when the blob has room for both: the pairing reads a changed serial as
     * the slot having taken a new record. */
    uint8_t buf[256];
    const uint32_t blob = openpete_interp_objs_blob_bytes();
    const int with_serial = stride + 4u <= blob && stride + 4u <= sizeof buf;
    const uint32_t len = with_serial ? stride + 4u : (blob < stride ? blob : stride);
    /* The pool is walked inside the room the moby walk leaves free above its
     * own bound, so a full array can never crowd it out. */
    for (uint32_t i = 0, rec = base; i < 512u; ++i, rec += stride) {
        const void* host = openpete_psx_to_host_span(rec, stride);
        if (!host) break;
        int16_t sel;
        memcpy(&sel, (const uint8_t*)host + game_layout.particles.sel_off, 2);
        if (sel < 0) {
            if ((sel >> 8) == -1) break;              /* end of the pool */
            continue;                                 /* inactive slot */
        }
        if (!with_serial) {
            openpete_interp_objs_capture(rec, host, len);
            continue;
        }
        const uint32_t serial = particles_slot_serial(rec);
        memcpy(buf, host, stride);
        memcpy(buf + stride, &serial, 4);
        openpete_interp_objs_capture(rec, buf, len);
    }
}

/* Gem-glint pool walk; see interp_regions_gen.h. */
void game_interp_capture_glint_pool(void) {
    if (!game_layout.glint.pool || !game_layout.glint.stride) return;
    const uint32_t stride = game_layout.glint.stride;
    const uint32_t len = openpete_interp_objs_blob_bytes() < stride
                             ? openpete_interp_objs_blob_bytes() : stride;
    for (uint32_t s = 0; s < game_layout.glint.slots; ++s) {
        const uint32_t rec = game_layout.glint.pool + s * stride;
        const void* host = openpete_psx_to_host_span(rec, stride);
        if (!host) break;
        uint32_t packed;
        memcpy(&packed, (const uint8_t*)host + game_layout.glint.packed_off, 4);
        if ((packed & 0xFFu) == 0u) continue;             /* free slot */
        openpete_interp_objs_capture(rec, host, len);
    }
}

/* The glow-ring pool (game_layout.glow): `slots` records of `stride` bytes,
 * world XYZ at `pos_off` and a packed word at `packed_off` whose byte 0 is the
 * shape index and 0 on a free slot. The glow routine projects each record
 * under the tick's camera and emits the ring's screen corners itself, so a
 * sub-tick present that replays those corners needs the record's previous
 * world position to rebase them onto the pose it shows. The pool is a fixed
 * array outside the moby array, so the shared walk never reaches it and the
 * record's address is its slot. Read-only against guest RAM. */
void game_interp_capture_glow_pool(void) {
    if (!game_layout.glow.pool || !game_layout.glow.stride) return;
    const uint32_t stride = game_layout.glow.stride;
    const uint32_t len = openpete_interp_objs_blob_bytes() < stride
                             ? openpete_interp_objs_blob_bytes() : stride;
    for (uint32_t s = 0; s < game_layout.glow.slots; ++s) {
        const uint32_t rec = game_layout.glow.pool + s * stride;
        const void* host = openpete_psx_to_host_span(rec, stride);
        if (!host) break;
        uint32_t packed;
        memcpy(&packed, (const uint8_t*)host + game_layout.glow.packed_off, 4);
        if ((packed & 0xFFu) == 0u) continue;             /* free slot */
        openpete_interp_objs_capture(rec, host, len);
    }
}

/* Zap pool walk; see interp_regions_gen.h. */
void game_interp_capture_zap_pool(void) {
    if (!game_layout.zap.pool || !game_layout.zap.stride) return;
    const uint32_t stride = game_layout.zap.stride;
    const uint32_t len = openpete_interp_objs_blob_bytes() < stride
                             ? openpete_interp_objs_blob_bytes() : stride;
    for (uint32_t s = 0; s < game_layout.zap.slots; ++s) {
        const uint32_t rec = game_layout.zap.pool + s * stride;
        const void* host = openpete_psx_to_host_span(rec, stride);
        if (!host) break;
        uint32_t packed;
        memcpy(&packed, (const uint8_t*)host + game_layout.zap.packed_off, 4);
        if ((packed & 0xFFu) == 0u) continue;             /* free slot */
        openpete_interp_objs_capture(rec, host, len);
    }
}

/* Tracer ribbon and mesh vertex walk; see interp_regions_gen.h. */
void game_interp_capture_tracer_pool(void) OP_REQUIRES(sim) {
    const uint32_t stride = game_layout.tracers.stride;
    if (game_layout.tracers.ring && stride) {
        const uint32_t len = openpete_interp_objs_blob_bytes() < stride
                                 ? openpete_interp_objs_blob_bytes() : stride;
        for (uint32_t s = 0; s < game_layout.tracers.slots; ++s) {
            const uint32_t rec = game_layout.tracers.ring + s * stride;
            const void* host = openpete_psx_to_host_span(rec, stride);
            if (!host) break;
            uint32_t live;
            memcpy(&live, host, 4);
            if (live == 0u) continue;                     /* free slot */
            openpete_interp_objs_capture(rec, host, len);
        }
    }
    uint32_t addr[8], nv[8];
    const int lists = tracers_mesh_lists(addr, nv, 8);
    for (int l = 0; l < lists; ++l) {
        for (uint32_t i = 0; i < nv[l]; ++i) {
            const uint32_t va = addr[l] + i * 12u;
            const void* host = openpete_psx_to_host_span(va, 12u);
            if (host) openpete_interp_objs_capture(tracers_mesh_vertex_key(va), host, 12u);
        }
    }
}

/* Every feeder pool walk; see interp_regions_gen.h. */
void game_interp_capture_feeder_pools(void) OP_REQUIRES(sim) {
    game_interp_capture_particle_pool();
    game_interp_capture_glint_pool();
    game_interp_capture_glow_pool();
    game_interp_capture_zap_pool();
    game_interp_capture_tracer_pool();
}

/** @brief Capture the (tile, accumulator) pair of every scrolling-texture env-anim descriptor.
 *
 * The descriptors animate flowing water and waterfalls. The table at
 * env_anim.scroll holds a count and a pointer to the descriptor pointer list;
 * a zero pointer, a zero count or a count above 1024 captures nothing, and a
 * null descriptor is skipped. Each descriptor's 4 bytes at desc+4 are keyed by
 * the descriptor's address. The update keeps each scroller's whole animation state in a 7-bit accumulator at
 * desc+6 and commits it to the tiledef V bytes as whole texels on a keyframe
 * advance, so a scrolled texture steps at the sim rate while the world around
 * it interpolates. Capture just the (tile, accumulator) pair at desc+4, not the
 * descriptor head, whose countdown byte at +3 decrements every tick: including
 * it would make the blob differ every tick and defeat the table's distinct-roll
 * dedupe, collapsing a slow scroller's real advance interval to a single tick.
 * terrain.c (tsc_scroll_refresh) reads the pair back and interpolates the
 * accumulator at the slot's own alpha, so the scroll stays smooth even when the
 * camera is still, where the camera ring dedupes and the shared bracket alpha
 * never engages. */
static void capture_env_anim_scroll(void) {
    if (!game_layout.env_anim.scroll) return;
    uint32_t cnt  = rd32(game_layout.env_anim.scroll);
    uint32_t base = rd32(game_layout.env_anim.scroll + 4u);
    if (!base || !cnt || cnt > 1024u) return;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t desc = rd32(base + i * 4u);
        if (!desc) continue;
        const void* host = openpete_psx_to_host_span(desc + 4u, 4u);
        if (host) openpete_interp_objs_capture(desc, host, 4u);
    }
}

static void (*s_extra_pools)(void) OP_SLOT(sim);

/** @brief The per-object capture the game registers: one tick's keyed object snapshots.
 *
 * Between openpete_interp_objs_begin_tick and end_tick it updates the credits
 * letter generations every key lookup of the capture reads, then captures the
 * moby array, the game's extra feeder pools when registered, and the scrolling
 * env-anim descriptors. With OPENPETE_INTERP_OBJDBG set it prints the keyed
 * table's population when openpete_frame is a multiple of 120. Reads guest RAM
 * only. */
static void capture_objs_common(void) {
    OP_ROLE_ASSERT(sim);
    openpete_interp_objs_begin_tick();
    game_credits_track();
    game_interp_capture_moby_array();
    if (s_extra_pools) s_extra_pools();
    capture_env_anim_scroll();
    openpete_interp_objs_end_tick();

    /* OPENPETE_INTERP_OBJDBG=1: the keyed table's population when openpete_frame
     * is a multiple of 120.
     * A spawned/despawned pair that never settles is a key that is not stable
     * across ticks, which shows on screen as objects snapping at the tick rate. */
    static int dbg = -1;
    ENV_FLAG_LATCH(dbg, "OPENPETE_INTERP_OBJDBG");
    if (dbg && (openpete_frame % 120u) == 0) {
        int tracked, paired, spawned, despawned;
        openpete_interp_objs_stats(&tracked, &paired, &spawned, &despawned);
        fprintf(stderr, "[interp-objs f=%u] tracked=%d paired=%d spawned=%d despawned=%d\n",
                openpete_frame, tracked, paired, spawned, despawned);
    }
}

void game_interp_register_common(const game_interp_row_t* rows, unsigned count,
                                 uint32_t cam_length, void (*extra_pools)(void) OP_SLOT(sim)) {
    /* ra_commit = 1 on the camera: it presents from the committed timeline under
     * runahead, so a misprediction never shows as a pan that un-shows. The
     * follow camera is where misprediction is catastrophic (it is the view
     * transform) and where the extra ticks of latency are invisible (it trails
     * by design); the player and the mobys stay led, keeping the input-lag win
     * where it is felt. This picks which timeline the presentation lerps; the
     * game's own camera state is untouched. */
    game_interp_rows_add_one(GAME_INTERP_CAM_ROW, game_layout.cam.base, cam_length, 1);
    game_interp_rows_add(rows, count);
    game_interp_rows_commit();
    s_extra_pools = extra_pools;
    /* One whole record per object, so every consumer reads its fields at their
     * natural offsets, and the store's default capacity. The blob is the
     * longest record a walk captures whole: the Moby with its slot's
     * allocation serial when the layout names a moby allocator, or the tracer
     * ribbon record, whose point ring runs to its end. The particle record and
     * the allocation serial appended to it fit under either on every game. */
    const uint32_t moby_len = game_layout.moby.array_stride + (moby_serial_enabled() ? 4u : 0u);
    const uint32_t blob = game_layout.tracers.stride > moby_len ? game_layout.tracers.stride : moby_len;
    game_interp_rows_obj_capture(capture_objs_common, blob, OPENPETE_INTERP_OBJ_SLOTS);
}

void game_interp_rows_commit(void) {
    openpete_interp_register_regions(s_regions, s_count);
    if (census_on()) {
        static int armed = 0;
        if (!armed) { armed = 1; atexit(census_report); }
    }
}
