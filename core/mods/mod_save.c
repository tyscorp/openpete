/* mod_save.c - per-mod save records. See openpete_modsave.h.
 *
 * State split:
 *   the key's block (s_blk, s_rows): the live key (unknown, cleared or set
 *     to a card slot, save index, lineage and record hash), the generation,
 *     the legacy copy, the pending load, the delivery epoch and the
 *     binding dedupe, and per parsed mod id the held identity. It travels
 *     as a trailer of the savestate game-extra section, so savestates,
 *     rewind, runahead and handoff restore it with the arena;
 *   host state: the registrations, the per-card sidecar tables and the
 *     speculative queue. Host effects of a speculative tick are queued and
 *     committed when runahead adopts the led ticks, discarded on rollback.
 *
 * Sidecar file: every write takes "<sidecar>.lock", re-reads the file and
 * merges it with the table before replacing it (sidecar_write), so two
 * processes holding one card directory keep each other's snapshots. A
 * sidecar that exists but cannot be read is never written over.
 *
 * A snapshot is one save's records: the record hash, the lineage, the
 * session modset tag and per mod (id, schema, gap, bytes). Each save index
 * holds up to MS_CAP snapshots, newest first.
 */
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
#endif
#include "openpete_modsave.h"

#include "openpete_atomic_file.h"
#include "openpete_log.h"
#include "openpete_memcard.h"
#include "openpete_mods.h"
#include "openpete_osd.h"
#include "openpete_psx.h"
#include "../host/sha256.h"

#include "openpete_paths.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

extern int openpete_speculative; /* openpete_fiber.h - runahead lookahead */
extern unsigned openpete_frame;  /* openpete_fiber.h - the VBL frame counter */

#define MS_CAP        4
#define MS_SLOTS      2
#define MS_MAX_IDX    16
#define MS_ID_LEN     64
#define MS_MAX_REGS   64
#define MS_QUEUE_MAX  64
#define MS_BLOCK_MAGIC 0x314B534Du  /* "MSK1" */
#define MS_BLOCK_VER   1u
static const char k_file_magic[8] = { 'O', 'P', 'M', 'O', 'D', 'S', 'V', '1' };
#define MS_FILE_VER    1u

int openpete_modsave_armed = 0;

static const char* const k_status[] = { "OK", "ABSENT", "GAP", "STALE", "NEWER", "LEGACY_PAD" };

/* ── the key's block ─────────────────────────────────────────────────── */

enum { KEY_UNKNOWN = 0, KEY_CLEARED, KEY_SET };
enum { HELD_NONE = 0, HELD_LINEAGE, HELD_CLEARED };

typedef struct {
    uint32_t version;
    uint8_t  key, key_slot, key_idx, pload;
    uint8_t  lineage[16];
    uint8_t  hash[32];
    uint32_t gen;
    uint32_t epoch;          /* delivery-function first entries */
    uint32_t legacy_len;
    uint8_t  legacy[OPENPETE_MODSAVE_LEGACY_MAX];
    uint32_t pload_cands;    /* bit slot * record_count + index */
    uint8_t  pload_hash[32];
    uint32_t pload_legacy_len;
    uint8_t  pload_legacy[OPENPETE_MODSAVE_LEGACY_MAX];
    uint32_t dedupe_epoch;   /* epoch + 1 of the last binding acted on, 0 none */
    uint8_t  dedupe_hash[32];
    uint32_t n_rows;
} ms_block_t;

typedef struct {
    char     id[MS_ID_LEN];
    uint32_t held;           /* HELD_* */
    uint8_t  lineage[16];
    uint32_t gen;
} ms_row_t;

static ms_block_t s_blk;
static ms_row_t*  s_rows;
static uint32_t   s_n_rows;
static int        s_frozen;

/* Pending asynchronous writes (a descriptor with async_write): one entry
 * per write call in flight, serialized back to back. The bytes travel as a
 * second trailer after the key's block, grown on demand, so savestates,
 * rewind and runahead restore them with the block. */
#define MS_PEND_MAGIC 0x31504D4Du  /* "MPP1" */
static uint8_t* s_pend;
static uint32_t s_pend_len, s_pend_cap;

/* ── registrations ───────────────────────────────────────────────────── */

typedef struct {
    char                       id[MS_ID_LEN];
    openpete_mod_t*            self;
    uint32_t                   schema;
    openpete_mod_save_write_fn write;
    openpete_mod_save_read_fn  read;
} ms_reg_t;

static ms_reg_t s_regs[MS_MAX_REGS];
static int      s_n_regs;

/* ── snapshots, sidecars and the speculative queue ───────────────────── */

typedef struct {
    char     id[MS_ID_LEN];
    uint32_t schema, gap, size;
    uint8_t* data;
} ms_rec_t;

typedef struct {
    uint8_t   hash[32];
    uint8_t   lineage[16];
    char*     tag;
    uint32_t  n_rec;
    ms_rec_t* rec;
} ms_snap_t;

typedef struct { uint32_t n; ms_snap_t* s[MS_CAP]; } ms_list_t;

/* SC_NONE: no sidecar file. SC_UNREADABLE: the file exists but could not
 * be read; the table holds only this process's changes and is not written. */
enum { SC_NONE = 0, SC_PRESENT, SC_UNREADABLE };

/* A snapshot's identity in a list: (hash, lineage), the key list_put replaces by. */
typedef struct { uint8_t hash[32]; uint8_t lineage[16]; } ms_key_t;

typedef struct {
    int       state;
    int       warned;        /* the unreadable refusal was logged since the mount */
    char      path[1300];
    ms_list_t idx[MS_MAX_IDX];
    /* This process's changes the file does not hold yet, merged at the next
     * write: per index the keys it put (oldest first) and whether it
     * cleared the index (a drop or a format) first. */
    ms_key_t  put[MS_MAX_IDX][MS_CAP];
    uint32_t  n_put[MS_MAX_IDX];
    uint8_t   cleared[MS_MAX_IDX];
} ms_table_t;

static ms_table_t s_sc[MS_SLOTS];

enum { Q_SNAP = 1, Q_DROP, Q_FORMAT };
typedef struct { int kind, slot, idx; ms_snap_t* snap; } ms_qop_t;
static ms_qop_t s_queue[MS_QUEUE_MAX];
static int      s_nq;

/** @brief The running game's save descriptor, NULL when it has none. */
static const openpete_save_desc_t* desc(void) {
    return openpete_mods_game_desc()->save_desc;
}

/* ── small helpers ───────────────────────────────────────────────────── */

/** @brief Fill @p out with 16 bytes of OS entropy (a fresh lineage). */
static void lineage_fresh(uint8_t out[16]) {
    int ok = 0;
#ifdef _WIN32
    for (int i = 0; i < 4; i++) {
        unsigned int v = 0;
        if (rand_s(&v) != 0) break;
        memcpy(out + 4 * i, &v, 4);
        ok = i == 3;
    }
#else
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ok = read(fd, out, 16) == 16;
        close(fd);
    }
#endif
    if (!ok) {
        /* No entropy source: a process-unique counter keeps lineages distinct within the process. */
        static uint64_t s_ctr = 0;
        const uint64_t a = ++s_ctr, b = (uint64_t)(uintptr_t)&s_ctr ^ 0x9E3779B97F4A7C15ull;
        memcpy(out, &a, 8);
        memcpy(out + 8, &b, 8);
        OPL_WARN_F("modsave", "no OS entropy for a fresh lineage; using a process counter");
    }
}

/** @brief SHA-256 of @p n bytes. */
static void sha(const uint8_t* p, size_t n, uint8_t out[32]) {
    openpete_sha256_ctx c;
    openpete_sha256_init(&c);
    openpete_sha256_update(&c, p, n);
    openpete_sha256_final(&c, out);
}

/** @brief First 8 hex digits of a 32-byte hash or 16-byte lineage, for log lines. */
static const char* hex8(const uint8_t* p, char buf[9]) {
    snprintf(buf, 9, "%02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
    return buf;
}

/** @brief 1 when every byte of the span is zero. */
static int all_zero(const uint8_t* p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (p[i]) return 0;
    return 1;
}

/** @brief 1 when the tick is a committed one, where host-effect log lines are emitted. */
static int committed(void) { return !openpete_speculative; }

/** @brief Free a snapshot and its records; NULL is ignored. */
static void snap_free(ms_snap_t* s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->n_rec; i++) free(s->rec[i].data);
    free(s->rec);
    free(s->tag);
    free(s);
}

/** @brief The record of mod @p id in a snapshot, or NULL. */
static const ms_rec_t* snap_rec(const ms_snap_t* s, const char* id) {
    for (uint32_t i = 0; s && i < s->n_rec; i++)
        if (!strcmp(s->rec[i].id, id)) return &s->rec[i];
    return NULL;
}

/** @brief Append a copy of a record to a snapshot. */
static void snap_add(ms_snap_t* s, const char* id, uint32_t schema, uint32_t gap,
                     const void* data, uint32_t size) {
    ms_rec_t* r = (ms_rec_t*)realloc(s->rec, (s->n_rec + 1u) * sizeof *r);
    if (!r) return;
    s->rec = r;
    r = &s->rec[s->n_rec];
    memset(r, 0, sizeof *r);
    snprintf(r->id, sizeof r->id, "%s", id);
    r->schema = schema; r->gap = gap; r->size = size;
    r->data = size ? (uint8_t*)malloc(size) : NULL;
    if (size && !r->data) return;
    if (size) memcpy(r->data, data, size);
    s->n_rec++;
}

/** @brief A new empty snapshot with the given hash, lineage and tag. */
static ms_snap_t* snap_new(const uint8_t hash[32], const uint8_t lin[16], const char* tag) {
    ms_snap_t* s = (ms_snap_t*)calloc(1, sizeof *s);
    if (!s) return NULL;
    memcpy(s->hash, hash, 32);
    memcpy(s->lineage, lin, 16);
    s->tag = strdup(tag ? tag : "");
    return s;
}

/** @brief The registration of a mod id, or NULL. */
static ms_reg_t* reg_of(const char* id) {
    for (int i = 0; i < s_n_regs; i++) if (!strcmp(s_regs[i].id, id)) return &s_regs[i];
    return NULL;
}

/** @brief The block row of a mod id, or NULL. */
static ms_row_t* row_of(const char* id) {
    for (uint32_t i = 0; i < s_n_rows; i++) if (!strcmp(s_rows[i].id, id)) return &s_rows[i];
    return NULL;
}

/** @brief 1 when a registered mod is enabled (receives callbacks). */
static int reg_active(const ms_reg_t* r) { return openpete_mods_is_enabled(r->id); }

/** @brief Record count of the descriptor, clamped to the sidecar table. */
static uint32_t n_idx(void) {
    const openpete_save_desc_t* d = desc();
    return d ? (d->record_count < MS_MAX_IDX ? d->record_count : MS_MAX_IDX) : 0u;
}

/* ── tables, view and host ops ───────────────────────────────────────── */

/** @brief Insert a snapshot pointer at the front of a list; an equal (hash, lineage) entry is removed first, an entry past the cap evicted; returns the evicted or replaced snapshot. */
static ms_snap_t* list_put(ms_list_t* l, ms_snap_t* sn) {
    ms_snap_t* out = NULL;
    uint32_t k = 0;
    for (; k < l->n; k++)
        if (!memcmp(l->s[k]->hash, sn->hash, 32) && !memcmp(l->s[k]->lineage, sn->lineage, 16)) break;
    if (k < l->n) {
        out = l->s[k];
        memmove(&l->s[1], &l->s[0], k * sizeof l->s[0]);
    } else {
        if (l->n == MS_CAP) out = l->s[MS_CAP - 1];
        const uint32_t keep = l->n < MS_CAP ? l->n : MS_CAP - 1u;
        memmove(&l->s[1], &l->s[0], keep * sizeof l->s[0]);
        l->n = keep + 1u;
    }
    l->s[0] = sn;
    return out;
}

/** @brief The snapshots a lookup sees at (slot, idx): the sidecar with the speculative queue applied, newest first. */
static ms_list_t view(int slot, int idx) {
    ms_list_t v;
    memset(&v, 0, sizeof v);
    if (slot < 0 || slot >= MS_SLOTS || idx < 0 || idx >= MS_MAX_IDX) return v;
    if (s_sc[slot].state != SC_NONE) v = s_sc[slot].idx[idx];
    for (int q = 0; q < s_nq; q++) {
        const ms_qop_t* op = &s_queue[q];
        if (op->slot != slot) continue;
        if (op->kind == Q_FORMAT || (op->kind == Q_DROP && op->idx == idx)) v.n = 0;
        else if (op->kind == Q_SNAP && op->idx == idx) (void)list_put(&v, op->snap);
    }
    return v;
}

static void sidecar_write(int slot);

/** @brief 1 when snapshot @p s has key @p k. */
static int snap_is(const ms_snap_t* s, const ms_key_t* k) {
    return !memcmp(s->hash, k->hash, 32) && !memcmp(s->lineage, k->lineage, 16);
}

/** @brief 1 when this process put snapshot @p s's key at index @p i since the last write. */
static int put_has(const ms_table_t* t, int i, const ms_snap_t* s) {
    for (uint32_t k = 0; k < t->n_put[i]; k++) if (snap_is(s, &t->put[i][k])) return 1;
    return 0;
}

/** @brief Note that this process put snapshot @p s at index @p i; past MS_CAP keys the oldest is forgotten, as the list evicts it. */
static void mark_put(ms_table_t* t, int i, const ms_snap_t* s) {
    if (put_has(t, i, s)) return;
    if (t->n_put[i] == MS_CAP) {
        memmove(&t->put[i][0], &t->put[i][1], (MS_CAP - 1) * sizeof t->put[i][0]);
        t->n_put[i]--;
    }
    ms_key_t* k = &t->put[i][t->n_put[i]++];
    memcpy(k->hash, s->hash, 32);
    memcpy(k->lineage, s->lineage, 16);
}

/** @brief Note that this process cleared index @p i: the file's snapshots there are dropped at the next write. */
static void mark_cleared(ms_table_t* t, int i) {
    t->cleared[i] = 1;
    t->n_put[i] = 0;
}

/** @brief Forget the changes noted since the last write: the file now holds them. */
static void pending_reset(ms_table_t* t) {
    memset(t->put, 0, sizeof t->put);
    memset(t->n_put, 0, sizeof t->n_put);
    memset(t->cleared, 0, sizeof t->cleared);
}

/** @brief The first `.modsave` of a card: one snapshot per non-empty record except @p skip, fresh lineage, no records, the card's `.modset` tag. */
static void seed_card(int slot, int skip) {
    const openpete_save_desc_t* d = desc();
    ms_table_t* t = &s_sc[slot];
    t->state = SC_PRESENT;
    const uint8_t* file = NULL;
    size_t len = 0;
    if (!d || openpete_memcard_file_view(slot, d->file_names[0], &file, &len) != 0) return;
    const char* tag = openpete_memcard_modset_tag(slot);
    for (uint32_t i = 0; i < n_idx(); i++) {
        const size_t at = d->record_base + (size_t)i * d->record_size;
        if ((int)i == skip || at + d->record_size > len) continue;
        const uint8_t* rec = file + at;
        if (d->is_empty(rec) || !d->checksum_ok(rec)) continue;
        uint8_t h[32], lin[16];
        sha(rec + d->hash_ofs, d->hash_len, h);
        lineage_fresh(lin);
        ms_snap_t* sn = snap_new(h, lin, tag);
        if (sn) { t->idx[i].s[0] = sn; t->idx[i].n = 1; }
        OPL_INFO_F("modsave", "card %d: seeded save %u (tag \"%s\")", slot, i, tag);
    }
}

/** @brief Commit one host op to the sidecar: seed the card's first `.modsave`, apply the op, note it for the merge, write the file.
 *
 * Takes ownership of the op's snapshot; a snapshot it replaces or evicts
 * is freed. No seed while the file exists but is unreadable.
 * @param op The op. */
static void commit_op(const ms_qop_t* op) {
    ms_table_t* t = &s_sc[op->slot];
    if (t->state == SC_NONE) seed_card(op->slot, op->kind == Q_FORMAT ? -1 : op->idx);
    if (op->kind == Q_FORMAT) {
        for (int i = 0; i < MS_MAX_IDX; i++) {
            for (uint32_t k = 0; k < t->idx[i].n; k++) snap_free(t->idx[i].s[k]);
            t->idx[i].n = 0;
            mark_cleared(t, i);
        }
    } else if (op->kind == Q_DROP) {
        ms_list_t* l = &t->idx[op->idx];
        for (uint32_t k = 0; k < l->n; k++) snap_free(l->s[k]);
        l->n = 0;
        mark_cleared(t, op->idx);
    } else {
        mark_put(t, op->idx, op->snap);
        ms_snap_t* gone = list_put(&t->idx[op->idx], op->snap);
        if (gone && gone != op->snap) {
            char a[9];
            if (memcmp(gone->hash, op->snap->hash, 32) || memcmp(gone->lineage, op->snap->lineage, 16))
                OPL_INFO_F("modsave", "card %d save %d: oldest snapshot %s evicted", op->slot, op->idx,
                           hex8(gone->hash, a));
            snap_free(gone);
        }
    }
    sidecar_write(op->slot);
}

/** @brief A host op: committed now, or queued on a speculative tick. */
static void host_op(int kind, int slot, int idx, ms_snap_t* sn) {
    ms_qop_t op = { kind, slot, idx, sn };
    if (!openpete_speculative) { commit_op(&op); return; }
    if (s_nq == MS_QUEUE_MAX) {
        OPL_WARN_F("modsave", "speculative queue full; save record dropped");
        snap_free(sn);
        return;
    }
    s_queue[s_nq++] = op;
}

void openpete_modsave_spec_discard(void) {
    if (s_nq) OPL_INFO_F("modsave", "rolled back speculative ticks: %d save-record op%s discarded", s_nq, s_nq == 1 ? "" : "s");
    for (int q = 0; q < s_nq; q++) snap_free(s_queue[q].snap);
    s_nq = 0;
}

void openpete_modsave_spec_commit(int flushed) {
    const int n = s_nq;
    s_nq = 0;
    if (!flushed && n) {
        OPL_WARN_F("modsave", "adoption card flush failed; %d queued save-record op%s dropped",
                   n, n == 1 ? "" : "s");
        for (int q = 0; q < n; q++) snap_free(s_queue[q].snap);
        return;
    }
    if (n) OPL_INFO_F("modsave", "adopted speculative ticks: %d save-record op%s committed", n, n == 1 ? "" : "s");
    for (int q = 0; q < n; q++) commit_op(&s_queue[q]);
}

/* ── sidecar file ────────────────────────────────────────────────────── */

typedef struct { uint8_t* p; size_t len, cap; int bad; } ms_buf_t;

/** @brief Append bytes to a growing buffer; an allocation failure marks it bad. */
static void buf_put(ms_buf_t* b, const void* p, size_t n) {
    if (b->bad) return;
    if (b->len + n > b->cap) {
        size_t c = b->cap ? b->cap * 2 : 4096;
        while (c < b->len + n) c *= 2;
        uint8_t* q = (uint8_t*)realloc(b->p, c);
        if (!q) { b->bad = 1; return; }
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->len, p, n);
    b->len += n;
}
/** @brief Append a little-endian u32. */
static void buf_u32(ms_buf_t* b, uint32_t v) {
    const uint8_t x[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    buf_put(b, x, 4);
}
/** @brief Append a length-prefixed string. */
static void buf_str(ms_buf_t* b, const char* s) {
    const uint32_t n = (uint32_t)strlen(s);
    buf_u32(b, n);
    buf_put(b, s, n);
}

/** @brief Serialize one snapshot. */
static void put_snap(ms_buf_t* b, const ms_snap_t* s) {
    buf_put(b, s->hash, 32);
    buf_put(b, s->lineage, 16);
    buf_str(b, s->tag ? s->tag : "");
    buf_u32(b, s->n_rec);
    for (uint32_t r = 0; r < s->n_rec; r++) {
        buf_str(b, s->rec[r].id);
        buf_u32(b, s->rec[r].schema);
        buf_u32(b, s->rec[r].gap);
        buf_u32(b, s->rec[r].size);
        buf_put(b, s->rec[r].data, s->rec[r].size);
    }
}

/** @brief Serialise a slot's table as the sidecar's bytes: magic, version, per index its snapshots, then the SHA-256 of the rest.
 * @param t The slot's table.
 * @param b Receives the bytes; marked bad when allocation failed. */
static void sidecar_serialise(const ms_table_t* t, ms_buf_t* b) {
    buf_put(b, k_file_magic, 8);
    buf_u32(b, MS_FILE_VER);
    buf_u32(b, MS_MAX_IDX);
    for (int i = 0; i < MS_MAX_IDX; i++) {
        buf_u32(b, t->idx[i].n);
        for (uint32_t k = 0; k < t->idx[i].n; k++) put_snap(b, t->idx[i].s[k]);
    }
    uint8_t h[32];
    if (!b->bad) { sha(b->p, b->len, h); buf_put(b, h, 32); }
}

int openpete_modsave_sidecar_bytes(int slot, uint8_t** out, size_t* len) {
    *out = NULL;
    *len = 0;
    if (slot < 0 || slot >= MS_SLOTS || s_sc[slot].state != SC_PRESENT) return 0;
    ms_buf_t b = { 0 };
    sidecar_serialise(&s_sc[slot], &b);
    if (b.bad) { free(b.p); return -1; }
    *out = b.p;
    *len = b.len;
    return 1;
}

typedef struct { const uint8_t* p; size_t len, at; int bad; } ms_rd_t;

/** @brief Read bytes; a read past the end marks the reader bad and yields zeros. */
static void rd_get(ms_rd_t* r, void* out, size_t n) {
    if (r->bad || r->at + n > r->len) { r->bad = 1; memset(out, 0, n); return; }
    memcpy(out, r->p + r->at, n);
    r->at += n;
}
/** @brief Read a little-endian u32. */
static uint32_t rd_u32(ms_rd_t* r) {
    uint8_t x[4];
    rd_get(r, x, 4);
    return (uint32_t)x[0] | (uint32_t)x[1] << 8 | (uint32_t)x[2] << 16 | (uint32_t)x[3] << 24;
}
/** @brief Read a length-prefixed string into a fresh allocation, at most @p max bytes. */
static char* rd_str(ms_rd_t* r, uint32_t max) {
    const uint32_t n = rd_u32(r);
    if (r->bad || n > max || r->at + n > r->len) { r->bad = 1; return NULL; }
    char* s = (char*)malloc(n + 1u);
    if (!s) { r->bad = 1; return NULL; }
    memcpy(s, r->p + r->at, n);
    s[n] = '\0';
    r->at += n;
    return s;
}

/** @brief Parse one snapshot, or NULL on a malformed one. */
static ms_snap_t* get_snap(ms_rd_t* r) {
    uint8_t h[32], lin[16];
    rd_get(r, h, 32);
    rd_get(r, lin, 16);
    char* tag = rd_str(r, 4096);
    ms_snap_t* s = r->bad ? NULL : snap_new(h, lin, tag);
    free(tag);
    const uint32_t n = s ? rd_u32(r) : 0u;
    for (uint32_t i = 0; s && !r->bad && i < n; i++) {
        char* id = rd_str(r, MS_ID_LEN - 1);
        const uint32_t schema = rd_u32(r), gap = rd_u32(r), size = rd_u32(r);
        if (!id || r->bad || r->at + size > r->len) { free(id); r->bad = 1; break; }
        snap_add(s, id, schema, gap, r->p + r->at, size);
        r->at += size;
        free(id);
    }
    if (r->bad) { snap_free(s); return NULL; }
    return s;
}

/** @brief Parse a whole sidecar file into @p t; 0 on success. */
static int sidecar_parse(ms_table_t* t, const uint8_t* p, size_t len) {
    uint8_t h[32];
    if (len < 8 + 8 + 32 || memcmp(p, k_file_magic, 8)) return -1;
    sha(p, len - 32, h);
    if (memcmp(h, p + len - 32, 32)) return -1;
    ms_rd_t r = { p, len - 32, 8, 0 };
    if (rd_u32(&r) != MS_FILE_VER) return -1;
    const uint32_t n = rd_u32(&r);
    for (uint32_t i = 0; i < n && !r.bad; i++) {
        const uint32_t k = rd_u32(&r);
        for (uint32_t j = 0; j < k && !r.bad; j++) {
            ms_snap_t* s = get_snap(&r);
            if (!s) break;
            if (i < MS_MAX_IDX && t->idx[i].n < MS_CAP) t->idx[i].s[t->idx[i].n++] = s;
            else snap_free(s);
        }
    }
    return r.bad ? -1 : 0;
}

/** @brief Read a sidecar file whole, telling an absent file from one that exists but cannot be read.
 *
 * OPENPETE_MODSAVE_FAULT_READ=1 makes an existing file read as unreadable.
 * @param path The file.
 * @param out Receives the malloc'd bytes on a read, else NULL.
 * @param len Receives their length, 0 for none.
 * @return 1 read, 0 absent (no such file or directory), -1 present but the
 *         open, a read or the allocation failed or the read came up short. */
static int sidecar_read(const char* path, uint8_t** out, size_t* len) {
    *out = NULL;
    *len = 0;
    errno = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return errno == ENOENT || errno == ENOTDIR ? 0 : -1;
    const char* fault = getenv("OPENPETE_MODSAVE_FAULT_READ");
    uint8_t* p = NULL;
    const long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    const int ok = !(fault && *fault == '1') && n >= 0 && fseek(f, 0, SEEK_SET) == 0 &&
                   (p = (uint8_t*)malloc((size_t)n + 1u)) != NULL &&
                   fread(p, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) { free(p); return -1; }
    *out = p;
    *len = (size_t)n;
    return 1;
}

/** @brief Rename an unparseable sidecar to the first free "<path>.bad-<n>". */
static void sidecar_set_aside(const char* path) {
    char aside[1400];
    for (int n = 1; n < 1000; n++) {
        snprintf(aside, sizeof aside, "%s.bad-%d", path, n);
        FILE* probe = fopen(aside, "rb");
        if (probe) { fclose(probe); continue; }
        if (rename(path, aside) == 0)
            OPL_WARN_F("modsave", "%s is unparseable; moved to %s and treated as absent", path, aside);
        return;
    }
}

/** @brief Drop every snapshot of a table and the changes noted for its next write. */
static void table_clear(ms_table_t* t) {
    for (int i = 0; i < MS_MAX_IDX; i++) {
        for (uint32_t k = 0; k < t->idx[i].n; k++) snap_free(t->idx[i].s[k]);
        t->idx[i].n = 0;
    }
    pending_reset(t);
    t->state = SC_NONE;
    t->warned = 0;
}

/** @brief Log, once per mount, that the slot's sidecar exists but cannot be read and is not written while so. */
static void unreadable_note(int slot, ms_table_t* t) {
    if (t->warned) return;
    t->warned = 1;
    OPL_ERROR_F("modsave", "card %d: %s exists but cannot be read; it is not written while it stays "
                "unreadable, and save records made meanwhile stay in memory (mods read their records "
                "as absent)", slot, t->path);
}

/** @brief Merge the file's table into the slot's: each index this process changed keeps its own puts, newest first, then (unless it cleared the index) the file's other snapshots, up to MS_CAP; every other index takes the file's list.
 *
 * A snapshot both hold under one key is this process's copy (the later
 * writer's). Every snapshot of @p d is moved into @p t or freed, and every
 * snapshot of @p t not kept is freed.
 * @param t The slot's table.
 * @param d The table read from the file; left empty. */
static void table_merge(ms_table_t* t, ms_table_t* d) {
    for (int i = 0; i < MS_MAX_IDX; i++) {
        ms_list_t* m = &t->idx[i];
        ms_list_t* f = &d->idx[i];
        ms_list_t out;
        memset(&out, 0, sizeof out);
        const int changed = t->cleared[i] || t->n_put[i];
        for (uint32_t k = 0; changed && k < m->n; k++)
            if (put_has(t, i, m->s[k]) && out.n < MS_CAP) { out.s[out.n++] = m->s[k]; m->s[k] = NULL; }
        for (uint32_t k = 0; !t->cleared[i] && k < f->n; k++)
            if (!(changed && put_has(t, i, f->s[k])) && out.n < MS_CAP) { out.s[out.n++] = f->s[k]; f->s[k] = NULL; }
        for (uint32_t k = 0; k < m->n; k++) snap_free(m->s[k]);
        for (uint32_t k = 0; k < f->n; k++) snap_free(f->s[k]);
        f->n = 0;
        *m = out;
    }
}

/* The longest wait for the sidecar lock, in milliseconds: another process
 * holds it for one read, merge and file write. */
#define MS_LOCK_WAIT_MS 1000
/* The retry interval while another process holds the sidecar lock, in
 * milliseconds. */
#define MS_LOCK_POLL_MS 5u

/** @brief Write the slot's sidecar under the lock "<sidecar>.lock": re-read the file, merge it into the table (table_merge), replace the file atomically.
 *
 * Merge rule: an index this process changed since its last write keeps
 * this process's snapshots ahead of the file's; a drop or a format here
 * drops the file's snapshots of the index (the later writer wins); every
 * other index takes the file's list, so another process's saves survive
 * and become visible here. A file that exists but cannot be read is not
 * written (logged once; the table and its noted changes stay in memory). A
 * file that does not parse is renamed aside and replaced by the table. A
 * failed or locked-out write is logged and the noted changes stay, so the
 * next write merges them again.
 * @param slot Card slot. */
static void sidecar_write(int slot) {
    ms_table_t* t = &s_sc[slot];
    if (!t->path[0]) return;
    intptr_t lk = 0;
    char lp[sizeof t->path + 8];
    snprintf(lp, sizeof lp, "%s.lock", t->path);
    if (openpete_paths_lock_wait(lp, &lk, MS_LOCK_WAIT_MS, MS_LOCK_POLL_MS, NULL, NULL, NULL) < 0) {
        OPL_ERROR_F("modsave", "card %d: %s.lock could not be taken; the save records stay in memory "
                    "and are written at the next save", slot, t->path);
        return;
    }
    uint8_t* p = NULL;
    size_t len = 0;
    const int r = sidecar_read(t->path, &p, &len);
    if (r < 0) {
        t->state = SC_UNREADABLE;
        unreadable_note(slot, t);
        openpete_paths_unlock(lk);
        return;
    }
    if (r > 0) {
        ms_table_t* d = (ms_table_t*)calloc(1, sizeof *d);
        if (!d) {
            free(p);
            openpete_paths_unlock(lk);
            OPL_ERROR_F("modsave", "card %d: no memory to merge %s; the save records stay in memory",
                        slot, t->path);
            return;
        }
        if (sidecar_parse(d, p, len) != 0) sidecar_set_aside(t->path);
        else table_merge(t, d);
        table_clear(d);
        free(d);
        free(p);
    }
    t->state = SC_PRESENT;
    t->warned = 0;
    ms_buf_t b = { 0 };
    sidecar_serialise(t, &b);
    const char* fault = getenv("OPENPETE_MODSAVE_FAULT_WRITE");
    const int rc = b.bad ? -1 : (fault && *fault == '1') ? -1 : op_atomic_write_file(t->path, b.p, b.len);
    free(b.p);
    openpete_paths_unlock(lk);
    if (rc != 0) {
        OPL_ERROR_F("modsave", "card %d: writing %s failed; the save records stay in memory "
                    "and are written at the next save", slot, t->path);
        return;
    }
    pending_reset(t);
    OPL_INFO_F("modsave", "card %d: wrote %s", slot, t->path);
}

/** @brief --adopt-saves: the newest snapshot of every non-empty save on the card takes the running modset tag; the sidecar is rewritten when one changed. */
static void adopt_card(int slot) {
    const openpete_save_desc_t* d = desc();
    ms_table_t* t = &s_sc[slot];
    const uint8_t* file = NULL;
    size_t len = 0;
    if (t->state != SC_PRESENT || openpete_memcard_file_view(slot, d->file_names[0], &file, &len) != 0)
        return;
    const char* cur = openpete_mods_modset_tag();
    int changed = 0;
    for (uint32_t i = 0; i < n_idx(); i++) {
        const size_t lo = d->record_base + (size_t)i * d->record_size;
        if (!t->idx[i].n || lo + d->record_size > len || d->is_empty(file + lo)) continue;
        ms_snap_t* sn = t->idx[i].s[0];
        if (sn->tag && !strcmp(sn->tag, cur)) continue;
        free(sn->tag);
        sn->tag = strdup(cur);
        mark_put(t, (int)i, sn);
        changed = 1;
    }
    if (!changed) return;
    OPL_INFO_F("modsave", "card %d: saves adopted under modset \"%s\"", slot, cur[0] ? cur : "vanilla");
    sidecar_write(slot);
}

void openpete_modsave_card_mounted(int slot, const char* image_path, int adopt) {
    if (slot < 0 || slot >= MS_SLOTS || !desc()) return;
    ms_table_t* t = &s_sc[slot];
    table_clear(t);
    snprintf(t->path, sizeof t->path, "%s.modsave", image_path);
    size_t len = 0;
    uint8_t* p = NULL;
    const int r = sidecar_read(t->path, &p, &len);
    if (r < 0) {
        t->state = SC_UNREADABLE;
        openpete_modsave_armed = 1;
        unreadable_note(slot, t);
        return;
    }
    if (r == 0) return;
    if (sidecar_parse(t, p, len) != 0) {
        table_clear(t);
        sidecar_set_aside(t->path);
    } else {
        t->state = SC_PRESENT;
        if (!openpete_modsave_armed)
            OPL_INFO_F("modsave", "card %d: %s present; save records armed", slot, t->path);
        openpete_modsave_armed = 1;
        if (adopt) adopt_card(slot);
    }
    free(p);
}

/* ── block ───────────────────────────────────────────────────────────── */

/** @brief Key unknown, held identities empty, pending load and pending write entries none; generation and epoch kept. */
static void block_unknown(void) {
    s_blk.key = KEY_UNKNOWN;
    s_blk.pload = 0;
    s_blk.legacy_len = 0;
    s_pend_len = 0;
    for (uint32_t i = 0; i < s_n_rows; i++) s_rows[i].held = HELD_NONE;
}

/** @brief 1 when the running game's writes complete asynchronously, so the trailer carries the pending entries. */
static int async_game(void) {
    const openpete_save_desc_t* d = desc();
    return d && d->async_write;
}

uint32_t openpete_modsave_block_size(void) {
    const uint32_t pend = async_game() ? s_pend_len + 8u : 0u;
    return (uint32_t)(sizeof s_blk + s_n_rows * sizeof(ms_row_t) + 8u) + pend;
}

void openpete_modsave_block_save(void* dst) {
    uint8_t* p = (uint8_t*)dst;
    const uint32_t len = (uint32_t)(sizeof s_blk + s_n_rows * sizeof(ms_row_t));
    if (async_game()) {
        /* The pending entries follow the key's trailer: bytes, length, magic. */
        uint8_t* q = p + len + 8u;
        const uint32_t magic = MS_PEND_MAGIC;
        if (s_pend_len) memcpy(q, s_pend, s_pend_len);
        memcpy(q + s_pend_len, &s_pend_len, 4);
        memcpy(q + s_pend_len + 4u, &magic, 4);
    }
    s_blk.version = MS_BLOCK_VER;
    s_blk.n_rows = s_n_rows;
    memcpy(p, &s_blk, sizeof s_blk);
    if (s_n_rows) memcpy(p + sizeof s_blk, s_rows, s_n_rows * sizeof(ms_row_t));
    /* OPENPETE_MODSAVE_FAULT_NO_TRAILER=1 writes no magic, so a load reads
     * the section as a blob that predates the block. */
    const char* nt = getenv("OPENPETE_MODSAVE_FAULT_NO_TRAILER");
    const uint32_t magic = nt && *nt == '1' ? 0u : MS_BLOCK_MAGIC;
    memcpy(p + len, &len, 4);
    memcpy(p + len + 4, &magic, 4);
}

/** @brief Restore held identities by mod id from a block's rows; ids absent from the state hold none. */
static void rows_restore(const ms_row_t* rows, uint32_t n) {
    for (uint32_t i = 0; i < s_n_rows; i++) {
        s_rows[i].held = HELD_NONE;
        for (uint32_t k = 0; k < n; k++) {
            if (strncmp(rows[k].id, s_rows[i].id, MS_ID_LEN)) continue;
            s_rows[i].held = rows[k].held;
            memcpy(s_rows[i].lineage, rows[k].lineage, 16);
            s_rows[i].gen = rows[k].gen;
        }
    }
}

/** @brief Locate the pending-entry trailer at a section's tail, without reading it into s_pend.
 * @param p The section.
 * @param size Its length.
 * @param len Receives the pending entries' length, 0 when the trailer is absent.
 * @return The length before the pending trailer, @p size when it is absent. */
static uint32_t pend_split(const uint8_t* p, uint32_t size, uint32_t* len) {
    uint32_t magic = 0;
    *len = 0;
    if (size < 8u) return size;
    memcpy(len, p + size - 8, 4);
    memcpy(&magic, p + size - 4, 4);
    if (magic != MS_PEND_MAGIC || *len > size - 8u) { *len = 0; return size; }
    return size - 8u - *len;
}

/** @brief Strip the pending-entry trailer from a section's tail into s_pend; with none, the entries are dropped.
 * @param p The section.
 * @param size Its length.
 * @return The length before the pending trailer. */
static uint32_t pend_load(const uint8_t* p, uint32_t size) {
    uint32_t len = 0;
    const uint32_t head = pend_split(p, size, &len);
    s_pend_len = 0;
    if (len > s_pend_cap) {
        uint8_t* g = (uint8_t*)realloc(s_pend, len);
        if (!g) return head;
        s_pend = g;
        s_pend_cap = len;
    }
    if (len) memcpy(s_pend, p + head, len);
    s_pend_len = len;
    return head;
}

/** @brief 1 when a block's key, card slot, save index, pending flag and legacy lengths lie in the ranges this module writes.
 *
 * The legacy lengths index the fixed legacy arrays and reach a mod as a
 * read size, and the slot and index select sidecar tables, so a block
 * read from a savestate is refused rather than clamped when any is out
 * of range.
 * @param in The block header as read from the section.
 * @return 1 when every field is in range, 0 otherwise. */
static int block_fields_ok(const ms_block_t* in) {
    return in->key <= KEY_SET && in->key_slot < MS_SLOTS && in->key_idx < MS_MAX_IDX && in->pload <= 1u &&
           in->legacy_len <= OPENPETE_MODSAVE_LEGACY_MAX && in->pload_legacy_len <= OPENPETE_MODSAVE_LEGACY_MAX;
}

/** @brief Locate the key's block at the tail of a section stripped of its pending trailer.
 * @param p The section.
 * @param size Its length without the pending trailer.
 * @param own Receives the game's own payload length: @p size without a block
 *        trailer, the bytes before the trailer otherwise.
 * @return The block (header then rows), or NULL when no trailer is present,
 *         its version or row count does not match its length, or a field
 *         lies outside the range this module writes (block_fields_ok). */
static const uint8_t* block_split(const uint8_t* p, uint32_t size, uint32_t* own) {
    uint32_t len = 0, magic = 0;
    *own = size;
    if (size < 8u) return NULL;
    memcpy(&len, p + size - 8, 4);
    memcpy(&magic, p + size - 4, 4);
    if (magic != MS_BLOCK_MAGIC || len > size - 8u || len < sizeof(ms_block_t)) return NULL;
    *own = size - 8u - len;
    const uint8_t* b = p + *own;
    ms_block_t in;
    memcpy(&in, b, sizeof in);
    if (in.version != MS_BLOCK_VER || len != sizeof in + in.n_rows * sizeof(ms_row_t)) return NULL;
    if (!block_fields_ok(&in)) return NULL;
    return b;
}

uint32_t openpete_modsave_block_load(const void* src, uint32_t size) {
    const uint8_t* p = (const uint8_t*)src;
    size = pend_load(p, size);
    uint32_t own = size;
    const uint8_t* b = block_split(p, size, &own);
    if (!b) {
        block_unknown();
        return own;
    }
    ms_block_t in;
    memcpy(&in, b, sizeof in);
    const uint32_t keep_rows = s_blk.n_rows;
    s_blk = in;
    s_blk.n_rows = keep_rows;
    rows_restore((const ms_row_t*)(const void*)(b + sizeof in), in.n_rows);
    return own;
}

/** @brief qsort order of rows by mod id. */
static int row_id_cmp(const void* a, const void* b) {
    return strncmp(((const ms_row_t*)a)->id, ((const ms_row_t*)b)->id, MS_ID_LEN);
}

uint32_t openpete_modsave_block_normalise(const void* src, uint32_t size, uint8_t** out, uint32_t* out_len) {
    const uint8_t* p = (const uint8_t*)src;
    uint32_t pend = 0, own = size;
    const uint32_t head = pend_split(p, size, &pend);
    const uint8_t* b = block_split(p, head, &own);
    const uint32_t tail = size - head;
    ms_block_t in;
    memset(&in, 0, sizeof in);
    if (b) memcpy(&in, b, sizeof in);
    /* Without a block, the bytes after the payload are kept as they are. */
    const uint32_t body = b ? (uint32_t)(sizeof in + in.n_rows * sizeof(ms_row_t)) : head - own;
    *out = (uint8_t*)malloc((size_t)body + tail + 1u);
    *out_len = 0;
    if (!*out) return own;
    if (!b) {
        memcpy(*out, p + own, (size_t)size - own);
        *out_len = size - own;
        return own;
    }
    ms_row_t* rows = (ms_row_t*)(void*)(*out + sizeof in);
    uint32_t kept = 0;
    for (uint32_t i = 0; i < in.n_rows; i++) {
        ms_row_t r;
        memcpy(&r, b + sizeof in + i * sizeof r, sizeof r);
        if (r.held != HELD_NONE) memcpy(&rows[kept++], &r, sizeof r);
    }
    if (kept) qsort(rows, kept, sizeof *rows, row_id_cmp);
    in.n_rows = kept;
    memcpy(*out, &in, sizeof in);
    const uint32_t n = (uint32_t)(sizeof in + kept * sizeof(ms_row_t));
    memcpy(*out + n, p + head, tail);
    *out_len = n + tail;
    return own;
}

void openpete_modsave_block_absent(void) { block_unknown(); }

void openpete_modsave_rebind(const char* const state_paths[2]) {
    if (s_blk.key != KEY_SET || s_blk.key_slot >= MS_SLOTS) return;
    const char* want = state_paths[s_blk.key_slot];
    if (!want || !want[0]) return;
    for (int q = 0; q < MS_SLOTS; q++) {
        char have[1300];
        if (!openpete_memcard_held_path(q, have, sizeof have) || strcmp(have, want)) continue;
        if (q != s_blk.key_slot)
            OPL_INFO_F("modsave", "savestate key rebound from card slot %d to %d", s_blk.key_slot, q);
        s_blk.key_slot = (uint8_t)q;
        return;
    }
}

/* ── status and delivery ─────────────────────────────────────────────── */

/** @brief The status source: 2 = the snapshot matching the key's (hash, lineage), 1 = the lineage's newest at the key's index, 0 = none. */
static int status_source(const ms_snap_t** out) {
    if (s_blk.key != KEY_SET) return 0;
    const ms_list_t v = view(s_blk.key_slot, s_blk.key_idx);
    for (uint32_t k = 0; k < v.n; k++)
        if (!memcmp(v.s[k]->hash, s_blk.hash, 32) && !memcmp(v.s[k]->lineage, s_blk.lineage, 16)) {
            *out = v.s[k];
            return 2;
        }
    for (uint32_t k = 0; k < v.n; k++)
        if (!memcmp(v.s[k]->lineage, s_blk.lineage, 16)) { *out = v.s[k]; return 1; }
    return 0;
}

/** @brief Fill @p r with mod @p g's status, by the precedence LEGACY_PAD, ABSENT, NEWER, STALE, GAP, OK. */
static void status_for(const ms_reg_t* g, openpete_mod_save_read_t* r) {
    const ms_snap_t* src = NULL;
    const int kind = status_source(&src);
    const ms_rec_t* rec = kind ? snap_rec(src, g->id) : NULL;
    memset(r, 0, sizeof *r);
    r->struct_size = (uint32_t)sizeof *r;
    r->save_index = s_blk.key == KEY_SET ? s_blk.key_idx : 0u;
    r->status = OP_MOD_SAVE_ABSENT;
    if (!rec) {
        if (s_blk.legacy_len && !all_zero(s_blk.legacy, s_blk.legacy_len)) {
            r->status = OP_MOD_SAVE_LEGACY_PAD;
            r->data = s_blk.legacy;
            r->size = s_blk.legacy_len;
        }
        return;
    }
    r->schema = rec->schema;
    if (rec->schema > g->schema) { r->status = OP_MOD_SAVE_NEWER; return; }
    r->data = rec->data;
    r->size = rec->size;
    r->status = kind == 1 ? OP_MOD_SAVE_STALE : rec->gap ? OP_MOD_SAVE_GAP : OP_MOD_SAVE_OK;
}

/** @brief The current identity: 0 unknown, else HELD_LINEAGE or HELD_CLEARED with the generation. */
static uint32_t identity(void) {
    return s_blk.key == KEY_SET ? HELD_LINEAGE : s_blk.key == KEY_CLEARED ? HELD_CLEARED : 0u;
}

/** @brief 1 when a row holds the current identity. */
static int row_holds_current(const ms_row_t* w) {
    const uint32_t id = identity();
    if (w->held != id || w->gen != s_blk.gen) return 0;
    return id != HELD_LINEAGE || !memcmp(w->lineage, s_blk.lineage, 16);
}

/** @brief Set a row's held identity to the current one. */
static void row_hold(ms_row_t* w) {
    w->held = identity();
    w->gen = s_blk.gen;
    if (w->held == HELD_LINEAGE) memcpy(w->lineage, s_blk.lineage, 16);
    else memset(w->lineage, 0, 16);
}

/** @brief Deliver a read to every enabled registered mod, in load order, whose held identity differs from the current one. */
static void deliver(void) {
    if (identity() == 0u) return;
    for (int i = 0; i < openpete_mods_count(); i++) {
        const char* id = openpete_mods_id(i);
        ms_reg_t* g = id ? reg_of(id) : NULL;
        ms_row_t* w = id ? row_of(id) : NULL;
        if (!g || !w || !reg_active(g) || row_holds_current(w)) continue;
        openpete_mod_save_read_t r;
        status_for(g, &r);
        row_hold(w);
        if (committed())
            OPL_INFO_F("modsave", "%s: read %s save %u", g->id, k_status[r.status], r.save_index);
        const openpete_mods_site_t site = openpete_mods_site_open(OPENPETE_MODS_CTX_SAVE_RECORD);
        openpete_mods_guard_enter(g->id);
        g->read(g->self, &r);
        openpete_mods_guard_leave();
        openpete_mods_site_close(site);
    }
}

/** @brief Pick the resolved candidate of a pending load.
 *
 * The only candidate; among several, the game's choice when it names one
 * of them, else the lowest (card slot, save index), logged.
 * @param cands Candidate bits, slot * record_count + index.
 * @param chosen_bit The game's chosen save as a bit index, -1 for none.
 * @return The picked bit index, or -1 when there is no candidate. */
static int pick_candidate(uint32_t cands, int chosen_bit) {
    if (!cands) return -1;
    if (!(cands & (cands - 1u))) {
        int b = 0;
        while (!(cands >> b & 1u)) b++;
        return b;
    }
    if (chosen_bit >= 0 && (cands >> chosen_bit & 1u)) return chosen_bit;
    int low = 0;
    while (!(cands >> low & 1u)) low++;
    if (committed())
        OPL_WARN_F("modsave", "load matches several saves (mask 0x%x) and the game's selection "
                   "names none of them; taking the lowest (card slot, save index)", cands);
    return low;
}

/** @brief A new-playthrough reset: key cleared, generation advanced, legacy copy emptied, pending load dropped. */
static void reset_key(void) {
    s_blk.key = KEY_CLEARED;
    s_blk.gen++;
    s_blk.legacy_len = 0;
    s_blk.pload = 0;
}

/** @brief The mismatch check at a resolved load: the snapshot's modset tag against the session's, a log line and, with the knob on, a toast. */
static void mismatch_check(const ms_snap_t* sn, int slot, int idx) {
    const char* cur = openpete_mods_modset_tag();
    if (!sn || !strcmp(sn->tag ? sn->tag : "", cur) || !committed()) return;
    OPL_WARN_F("modsave", "card %d save %d was written under modset \"%s\"; running \"%s\"",
               slot, idx, sn->tag && sn->tag[0] ? sn->tag : "vanilla", cur[0] ? cur : "vanilla");
    if (openpete_memcard_mismatch_toast())
        openpete_osd_notify("host", "modsave-mismatch", 0,
                            "SAVE WRITTEN WITH OTHER MODS");
}

/** @brief Resolve the pending load at the delivery arm: the key names the picked save with the matching snapshot's lineage, else the index's newest, else a fresh one. */
static void resolve_load(struct CPUState* cpu) {
    const openpete_save_desc_t* d = desc();
    int cs = -1, ci = -1, chosen = -1;
    if (d->chosen && d->chosen(cpu, &cs, &ci) && cs >= 0 && cs < MS_SLOTS && ci >= 0 &&
        (uint32_t)ci < n_idx())
        chosen = cs * (int)n_idx() + ci;
    const int bit = pick_candidate(s_blk.pload_cands, chosen);
    int cand_count = 0;
    for (uint32_t c = s_blk.pload_cands; c; c &= c - 1u) cand_count++;
    if (bit < 0) {
        reset_key();
        if (committed()) OPL_INFO_F("modsave", "loaded save matches no card record; new playthrough");
        return;
    }
    const int slot = bit / (int)n_idx(), idx = bit % (int)n_idx();
    const ms_list_t v = view(slot, idx);
    const ms_snap_t* m = NULL;
    for (uint32_t k = 0; k < v.n && !m; k++) if (!memcmp(v.s[k]->hash, s_blk.pload_hash, 32)) m = v.s[k];
    const ms_snap_t* src = m ? m : v.n ? v.s[0] : NULL;
    if (src) memcpy(s_blk.lineage, src->lineage, 16);
    else lineage_fresh(s_blk.lineage);
    s_blk.key = KEY_SET;
    s_blk.key_slot = (uint8_t)slot;
    s_blk.key_idx = (uint8_t)idx;
    memcpy(s_blk.hash, s_blk.pload_hash, 32);
    s_blk.gen++;
    s_blk.legacy_len = s_blk.pload_legacy_len;
    memcpy(s_blk.legacy, s_blk.pload_legacy, sizeof s_blk.legacy);
    s_blk.pload = 0;
    if (committed()) {
        char a[9], b[9];
        OPL_INFO_F("modsave", "load bound to card %d save %d of %d candidate%s (hash %s, lineage %s, %s)",
                   slot, idx, cand_count, cand_count == 1 ? "" : "s", hex8(s_blk.hash, a),
                   hex8(s_blk.lineage, b), m ? "matched" : src ? "newest of the index" : "fresh");
    }
    mismatch_check(src, slot, idx);
}

/** @brief The delivery arm: count the entry, resolve a pending load, deliver. */
static void on_delivery(struct CPUState* cpu) {
    s_blk.epoch++;
    if (s_blk.pload) resolve_load(cpu);
    deliver();
}

/** @brief The load binding's first entry: on a passing checksum verdict, record the candidate set (every loaded card's equal record) and the legacy copy. */
static void on_load_binding(struct CPUState* cpu) {
    const openpete_save_desc_t* d = desc();
    const uint32_t va = d->load_record ? d->load_record(cpu) : 0u;
    const uint8_t* rec = va ? (const uint8_t*)openpete_psx_to_host_ptr(va) : NULL;
    if (!rec || !d->checksum_ok(rec)) return;
    uint8_t h[32];
    sha(rec + d->hash_ofs, d->hash_len, h);
    if (s_blk.dedupe_epoch == s_blk.epoch + 1u && !memcmp(s_blk.dedupe_hash, h, 32)) return;
    s_blk.dedupe_epoch = s_blk.epoch + 1u;
    memcpy(s_blk.dedupe_hash, h, 32);
    uint32_t cands = 0;
    for (int slot = 0; slot < MS_SLOTS; slot++) {
        const uint8_t* file = NULL;
        size_t len = 0;
        if (openpete_memcard_file_view(slot, d->file_names[0], &file, &len) != 0) continue;
        for (uint32_t i = 0; i < n_idx(); i++) {
            const size_t at = d->record_base + (size_t)i * d->record_size;
            if (at + d->record_size <= len && !memcmp(file + at, rec, d->record_size))
                cands |= 1u << (slot * (int)n_idx() + (int)i);
        }
    }
    s_blk.pload = 1;
    s_blk.pload_cands = cands;
    memcpy(s_blk.pload_hash, h, 32);
    s_blk.pload_legacy_len = d->legacy_len;
    memset(s_blk.pload_legacy, 0, sizeof s_blk.pload_legacy);
    if (d->legacy_len) memcpy(s_blk.pload_legacy, rec + d->legacy_ofs, d->legacy_len);
}

/** @brief 1 when @p addr (in overlay @p ovl, 0 = executable) is the entered function. */
static int at(const openpete_save_desc_t* d, uint32_t addr, uint32_t fn, uint32_t ovl) {
    if (!fn || addr != fn) return 0;
    return !d->resident_overlay || !ovl || d->resident_overlay() == ovl;
}

void openpete_modsave_dispatch(struct CPUState* cpu, uint32_t addr) {
    const openpete_save_desc_t* d = desc();
    if (!d || openpete_mods_bypass_pending(addr)) return;
    if (at(d, addr, d->delivery_fn, d->delivery_ovl)) { on_delivery(cpu); return; }
    if (at(d, addr, d->load_fn, d->load_ovl)) { on_load_binding(cpu); return; }
    for (size_t i = 0; i < d->reset_count; i++)
        if (at(d, addr, d->reset_fns[i], d->reset_ovls ? d->reset_ovls[i] : 0u)) { reset_key(); return; }
}

/* ── write capture ───────────────────────────────────────────────────── */

/** @brief Carry-forward source: the snapshot at the key's own (slot, index) whose (hash, lineage) equal the key's, else the lineage's newest there (logged); NULL for none. */
static const ms_snap_t* carry_source(void) {
    const ms_snap_t* src = NULL;
    const int kind = status_source(&src);
    if (kind == 1 && committed())
        OPL_INFO_F("modsave", "carry-forward from the playthrough's newest snapshot (no exact match)");
    return kind ? src : NULL;
}

/** @brief Add one registered mod's record to a new snapshot: a NEWER refusal keeps the stored record; a -1 write keeps the previous one; toggled off carries it with gap. */
static void add_reg_record(ms_snap_t* sn, ms_reg_t* g, const ms_rec_t* prev, uint32_t index) {
    if (!reg_active(g)) {
        if (prev) snap_add(sn, prev->id, prev->schema, 1u, prev->data, prev->size);
        return;
    }
    if (prev && prev->schema > g->schema) {
        if (committed())
            OPL_WARN_F("modsave", "%s: save %u holds its schema %u record; schema %u refused to overwrite it",
                       g->id, index, prev->schema, g->schema);
        snap_add(sn, prev->id, prev->schema, prev->gap, prev->data, prev->size);
        return;
    }
    const openpete_mod_save_id_t sid = { (uint32_t)sizeof sid, index };
    const void* data = NULL;
    uint32_t size = 0;
    const openpete_mods_site_t site = openpete_mods_site_open(OPENPETE_MODS_CTX_SAVE_RECORD);
    openpete_mods_guard_enter(g->id);
    const int rc = g->write(g->self, &sid, &data, &size);
    openpete_mods_guard_leave();
    openpete_mods_site_close(site);
    if (rc == 0 && (data || !size)) snap_add(sn, g->id, g->schema, 0u, data, size);
    else if (prev) snap_add(sn, prev->id, prev->schema, prev->gap, prev->data, prev->size);
}

/** @brief Build the snapshot's records: each registered mod's, then every carried record of a mod not registered in this process (gap set). */
static void build_records(ms_snap_t* sn, const ms_snap_t* src, uint32_t index) {
    for (int i = 0; i < openpete_mods_count(); i++) {
        ms_reg_t* g = reg_of(openpete_mods_id(i));
        if (g) add_reg_record(sn, g, snap_rec(src, g->id), index);
    }
    for (uint32_t k = 0; src && k < src->n_rec; k++)
        if (!reg_of(src->rec[k].id))
            snap_add(sn, src->rec[k].id, src->rec[k].schema, 1u, src->rec[k].data, src->rec[k].size);
}

/** @brief Capture one covered non-empty record after its write. */
static void capture_record(int slot, uint32_t index, const uint8_t* rec, int flushed) {
    const openpete_save_desc_t* d = desc();
    uint8_t lin[16];
    const ms_snap_t* src = NULL;
    if (s_blk.key == KEY_SET) {
        memcpy(lin, s_blk.lineage, 16);
        src = carry_source();
    } else {
        if (s_blk.key == KEY_UNKNOWN && committed())
            OPL_INFO_F("modsave", "card %d save %u written with the key unknown; new playthrough", slot, index);
        lineage_fresh(lin);
    }
    uint8_t h[32];
    sha(rec + d->hash_ofs, d->hash_len, h);
    ms_snap_t* sn = snap_new(h, lin, openpete_mods_modset_tag());
    if (!sn) return;
    build_records(sn, src, index);
    if (!flushed) { snap_free(sn); return; }
    s_blk.key = KEY_SET;
    s_blk.key_slot = (uint8_t)slot;
    s_blk.key_idx = (uint8_t)index;
    memcpy(s_blk.lineage, lin, 16);
    memcpy(s_blk.hash, h, 32);
    s_blk.legacy_len = d->legacy_len;
    if (d->legacy_len) memcpy(s_blk.legacy, rec + d->legacy_ofs, d->legacy_len);
    for (int i = 0; i < s_n_regs; i++) {
        ms_row_t* w = row_of(s_regs[i].id);
        if (w && reg_active(&s_regs[i])) row_hold(w);
    }
    if (committed()) {
        char a[9], b[9];
        OPL_INFO_F("modsave", "card %d save %u: snapshot %s lineage %s, %u record%s", slot, index,
                   hex8(h, a), hex8(lin, b), sn->n_rec, sn->n_rec == 1 ? "" : "s");
    }
    host_op(Q_SNAP, slot, (int)index, sn);
}

/** @brief 1 when @p name is one of the descriptor's save file names. */
static int is_save_file(const openpete_save_desc_t* d, const char* name) {
    for (const char* const* n = d->file_names; n && *n; n++)
        if (!strncmp(*n, name, 20)) return 1;
    return 0;
}

void openpete_modsave_card_write(int slot, const char* name, const uint8_t* file,
                                 size_t file_len, int ofs, int bytes, int flushed) {
    const openpete_save_desc_t* d = desc();
    if (!openpete_modsave_armed || !d || d->async_write || slot < 0 || slot >= MS_SLOTS ||
        !name || !is_save_file(d, name))
        return;
    for (uint32_t i = 0; i < n_idx(); i++) {
        const size_t lo = d->record_base + (size_t)i * d->record_size, hi = lo + d->record_size;
        if (hi > file_len || (size_t)ofs >= hi || (size_t)ofs + (size_t)bytes <= lo) continue;
        const uint8_t* rec = file + lo;
        if (d->is_empty(rec)) {
            if (flushed) host_op(Q_DROP, slot, (int)i, NULL);
            continue;
        }
        capture_record(slot, i, rec, flushed);
    }
}

/* ── asynchronous writes: issue, completion, failure ─────────────────── */

/** @brief Find the pending entry of a port.
 * @param port Card port.
 * @param at Receives the entry's offset in s_pend.
 * @return The entry's length, 0 when the port has none. */
static uint32_t pend_find(int port, uint32_t* at) {
    for (uint32_t o = 0; o + 8u <= s_pend_len;) {
        uint32_t len, p;
        memcpy(&len, s_pend + o, 4);
        memcpy(&p, s_pend + o + 4, 4);
        if (len < 8u || o + len > s_pend_len) return 0;
        if ((int)p == port) { *at = o; return len; }
        o += len;
    }
    return 0;
}

/** @brief Remove the bytes [at, at + len) from s_pend. */
static void pend_cut(uint32_t at, uint32_t len) {
    memmove(s_pend + at, s_pend + at + len, s_pend_len - at - len);
    s_pend_len -= len;
}

/** @brief Append a built entry to s_pend, growing it on demand; an allocation failure drops the entry, logged. */
static void pend_append(const ms_buf_t* b) {
    if (b->bad) return;
    if (s_pend_len + b->len > s_pend_cap) {
        uint32_t c = s_pend_cap ? s_pend_cap : 4096u;
        while (c < s_pend_len + b->len) c *= 2u;
        uint8_t* g = (uint8_t*)realloc(s_pend, c);
        if (!g) { OPL_ERROR_F("modsave", "no memory for a pending save write; its records are dropped"); return; }
        s_pend = g;
        s_pend_cap = c;
    }
    memcpy(s_pend + s_pend_len, b->p, b->len);
    s_pend_len += (uint32_t)b->len;
}

/** @brief The record bytes a write leaves: the card image's record with the written span overlaid.
 *
 * @param out Receives record_size bytes.
 * @param file The file in the card image, or NULL when it is not on the card yet.
 * @param file_len Bytes from @p file to the end of the image.
 * @param lo The record's offset in the file.
 * @param src The written bytes, starting at file offset @p ofs.
 * @return 0, or -1 when a byte the write does not cover is unknown. */
static int record_after(uint8_t* out, const uint8_t* file, size_t file_len, size_t lo,
                        const uint8_t* src, size_t ofs, size_t bytes) {
    const size_t n = desc()->record_size;
    const size_t a = ofs > lo ? ofs : lo, e = ofs + bytes < lo + n ? ofs + bytes : lo + n;
    if (a > lo || e < lo + n) {
        if (!file || lo + n > file_len) return -1;
        memcpy(out, file + lo, n);
    }
    memcpy(out + (a - lo), src + (a - ofs), e - a);
    return 0;
}

/** @brief Serialize one covered record into an issuing entry: its index, the empty flag and, when non-empty, the snapshot the completion stores (lineage, carried and written records; hash filled at completion). */
static void issue_record(ms_buf_t* b, int slot, uint32_t index, const uint8_t* rec) {
    const int empty = desc()->is_empty(rec);
    buf_u32(b, index);
    buf_u32(b, (uint32_t)empty);
    if (empty) return;
    uint8_t lin[16], zero[32] = { 0 };
    const ms_snap_t* src = NULL;
    if (s_blk.key == KEY_SET) {
        memcpy(lin, s_blk.lineage, 16);
        src = carry_source();
    } else {
        if (s_blk.key == KEY_UNKNOWN && committed())
            OPL_INFO_F("modsave", "card %d save %u written with the key unknown; new playthrough", slot, index);
        lineage_fresh(lin);
    }
    ms_snap_t* sn = snap_new(zero, lin, openpete_mods_modset_tag());
    if (!sn) { b->bad = 1; return; }
    build_records(sn, src, index);
    put_snap(b, sn);
    snap_free(sn);
}

void openpete_modsave_bu_write_issue(int port, const char* name, const uint8_t* src, int ofs, int bytes) {
    const openpete_save_desc_t* d = desc();
    if (!openpete_modsave_armed || !d || !d->async_write || port < 0 || port >= MS_SLOTS || !name ||
        !is_save_file(d, name) || !src || ofs < 0 || bytes <= 0)
        return;
    uint32_t at = 0;
    const uint32_t stale = pend_find(port, &at);
    if (stale) pend_cut(at, stale);
    const uint8_t* file = NULL;
    size_t file_len = 0;
    if (openpete_memcard_file_view(port, d->file_names[0], &file, &file_len) != 0) file = NULL;
    ms_buf_t b = { 0 };
    uint32_t n_cov = 0;
    buf_u32(&b, 0u); /* entry length, patched below */
    buf_u32(&b, (uint32_t)port);
    buf_u32(&b, s_blk.gen);
    buf_u32(&b, (uint32_t)ofs);
    buf_u32(&b, (uint32_t)bytes);
    uint32_t n_on = 0;
    for (int i = 0; i < s_n_regs; i++) n_on += reg_active(&s_regs[i]) ? 1u : 0u;
    buf_u32(&b, n_on);
    for (int i = 0; i < s_n_regs; i++) if (reg_active(&s_regs[i])) buf_str(&b, s_regs[i].id);
    const size_t cov_at = b.len;
    buf_u32(&b, 0u); /* covered record count, patched below */
    uint8_t* rec = (uint8_t*)malloc(d->record_size);
    for (uint32_t i = 0; rec && i < n_idx(); i++) {
        const size_t lo = d->record_base + (size_t)i * d->record_size, hi = lo + d->record_size;
        if ((size_t)ofs >= hi || (size_t)ofs + (size_t)bytes <= lo) continue;
        if (record_after(rec, file, file_len, lo, src, (size_t)ofs, (size_t)bytes) != 0) continue;
        issue_record(&b, port, i, rec);
        n_cov++;
    }
    free(rec);
    if (!b.bad) {
        const uint32_t len = (uint32_t)b.len;
        memcpy(b.p, &len, 4);
        memcpy(b.p + cov_at, &n_cov, 4);
        pend_append(&b);
    }
    free(b.p);
    if (committed())
        OPL_INFO_F("modsave", "card %d: save write issued at frame %u, 0x%X bytes at 0x%X, %u record%s covered",
                   port, openpete_frame, (unsigned)bytes, (unsigned)ofs, n_cov, n_cov == 1 ? "" : "s");
}

typedef struct { uint32_t index, empty; ms_snap_t* sn; } ms_cov_t;

/** @brief Parse a removed entry's covered records; returns their count, -1 on a malformed entry (every parsed snapshot freed). */
static int pend_parse(ms_rd_t* r, ms_cov_t* cov, uint32_t cap, char ids[][MS_ID_LEN], uint32_t* n_on, uint32_t* gen) {
    (void)rd_u32(r); (void)rd_u32(r);  /* length, port */
    *gen = rd_u32(r);
    (void)rd_u32(r); (void)rd_u32(r);  /* offset, bytes */
    *n_on = rd_u32(r);
    for (uint32_t i = 0; i < *n_on && !r->bad; i++) {
        char* s = rd_str(r, MS_ID_LEN - 1);
        if (s && i < MS_MAX_REGS) snprintf(ids[i], MS_ID_LEN, "%s", s);
        free(s);
    }
    if (*n_on > MS_MAX_REGS) *n_on = MS_MAX_REGS;
    const uint32_t n = rd_u32(r);
    uint32_t k = 0;
    for (; k < n && k < cap && !r->bad; k++) {
        cov[k].index = rd_u32(r);
        cov[k].empty = rd_u32(r);
        cov[k].sn = cov[k].empty ? NULL : get_snap(r);
        if (!cov[k].empty && !cov[k].sn) r->bad = 1;
    }
    if (!r->bad && k == n) return (int)k;
    for (uint32_t j = 0; j < k; j++) snap_free(cov[j].sn);
    return -1;
}

/** @brief 1 when every covered non-empty record passes the checksum verdict in the card image. */
static int covered_intact(const uint8_t* file, size_t file_len, const ms_cov_t* cov, int n) {
    const openpete_save_desc_t* d = desc();
    for (int k = 0; k < n; k++) {
        const size_t lo = d->record_base + (size_t)cov[k].index * d->record_size;
        if (cov[k].empty) continue;
        if (!file || cov[k].index >= n_idx() || lo + d->record_size > file_len || !d->checksum_ok(file + lo)) return 0;
    }
    return 1;
}

/** @brief Store a completed write's snapshots and drops; with the generation unchanged since issue, set the key to its last non-empty record and the held identities of the mods enabled at issue. */
static void complete_records(int port, const uint8_t* file, ms_cov_t* cov, int n,
                             char ids[][MS_ID_LEN], uint32_t n_on, uint32_t gen) {
    const openpete_save_desc_t* d = desc();
    int last = -1;
    for (int k = 0; k < n; k++) {
        if (cov[k].empty) { host_op(Q_DROP, port, (int)cov[k].index, NULL); continue; }
        const uint8_t* rec = file + d->record_base + (size_t)cov[k].index * d->record_size;
        sha(rec + d->hash_ofs, d->hash_len, cov[k].sn->hash);
        if (committed()) {
            char a[9], b[9];
            OPL_INFO_F("modsave", "card %d save %u: snapshot %s lineage %s, %u record%s", port, cov[k].index,
                       hex8(cov[k].sn->hash, a), hex8(cov[k].sn->lineage, b), cov[k].sn->n_rec,
                       cov[k].sn->n_rec == 1 ? "" : "s");
        }
        last = k;
    }
    if (last >= 0 && gen == s_blk.gen) {
        const uint8_t* rec = file + d->record_base + (size_t)cov[last].index * d->record_size;
        s_blk.key = KEY_SET;
        s_blk.key_slot = (uint8_t)port;
        s_blk.key_idx = (uint8_t)cov[last].index;
        memcpy(s_blk.lineage, cov[last].sn->lineage, 16);
        memcpy(s_blk.hash, cov[last].sn->hash, 32);
        s_blk.legacy_len = d->legacy_len;
        if (d->legacy_len) memcpy(s_blk.legacy, rec + d->legacy_ofs, d->legacy_len);
        for (uint32_t i = 0; i < n_on; i++) {
            ms_row_t* w = row_of(ids[i]);
            if (w) row_hold(w);
        }
    } else if (last >= 0 && committed()) {
        OPL_INFO_F("modsave", "card %d: a load or new playthrough came between the write's issue and "
                   "completion; the key stays on it", port);
    }
    for (int k = 0; k < n; k++)
        if (!cov[k].empty) host_op(Q_SNAP, port, (int)cov[k].index, cov[k].sn);
}

void openpete_modsave_bu_write_done(int port, const char* name, int ok) {
    const openpete_save_desc_t* d = desc();
    if (!openpete_modsave_armed || !d || !d->async_write || port < 0 || port >= MS_SLOTS) return;
    uint32_t at = 0;
    const uint32_t len = pend_find(port, &at);
    if (!len) {
        if (ok && name && is_save_file(d, name) && committed())
            OPL_WARN_F("modsave", "card %d: a save write completed with no pending entry (a savestate "
                       "taken before its issue); a save it changed reads STALE at its next load", port);
        return;
    }
    uint8_t* e = (uint8_t*)malloc(len);
    if (!e) { pend_cut(at, len); return; }
    memcpy(e, s_pend + at, len);
    pend_cut(at, len);
    ms_rd_t r = { e, len, 0, 0 };
    ms_cov_t cov[MS_MAX_IDX];
    static char ids[MS_MAX_REGS][MS_ID_LEN];
    uint32_t n_on = 0, gen = 0;
    const int n = pend_parse(&r, cov, MS_MAX_IDX, ids, &n_on, &gen);
    free(e);
    if (n < 0) { OPL_ERROR_F("modsave", "card %d: malformed pending save write dropped", port); return; }
    const uint8_t* file = NULL;
    size_t file_len = 0;
    if (openpete_memcard_file_view(port, d->file_names[0], &file, &file_len) != 0) file = NULL;
    const char* why = !ok ? "the write failed" : !covered_intact(file, file_len, cov, n)
                    ? "a written record fails the game's checksum in the card image" : NULL;
    if (why) {
        if (committed()) OPL_WARN_F("modsave", "card %d: save write completion: %s; no save record kept", port, why);
        for (int k = 0; k < n; k++) snap_free(cov[k].sn);
        return;
    }
    if (committed())
        OPL_INFO_F("modsave", "card %d: save write completed at frame %u, %d record%s covered", port,
                   openpete_frame, n, n == 1 ? "" : "s");
    if (n) complete_records(port, file, cov, n, ids, n_on, gen);
}

/** @brief 1 when a frame of the slot's card image lies inside the descriptor file's records. */
static int frame_in_records(int slot, int frame) {
    const openpete_save_desc_t* d = desc();
    if (!d) return 0;
    const int base = openpete_memcard_file_offset(slot, d->file_names[0]);
    if (base < 0) return 0;
    const long lo = (long)base + (long)d->record_base, hi = lo + (long)d->record_size * (long)n_idx();
    const long f = (long)frame * 128L;
    return f + 128L > lo && f < hi;
}

void openpete_modsave_card_frame_write(int slot, int frame) {
    static int s_warned[MS_SLOTS];
    uint32_t at = 0;
    if (!openpete_modsave_armed || slot < 0 || slot >= MS_SLOTS || s_warned[slot] || !committed()) return;
    if (pend_find(slot, &at) || !frame_in_records(slot, frame)) return;
    s_warned[slot] = 1;
    OPL_WARN_F("modsave", "card %d: frame %d written outside the file-level entry; a save it "
               "changed reads STALE at its next load", slot, frame);
}

void openpete_modsave_card_format(int slot) {
    if (!openpete_modsave_armed || !desc() || slot < 0 || slot >= MS_SLOTS) return;
    host_op(Q_FORMAT, slot, 0, NULL);
}

/* ── registration and init ───────────────────────────────────────────── */

int openpete_modsave_register(openpete_mod_t* self, uint32_t schema,
                              openpete_mod_save_write_fn write, openpete_mod_save_read_fn read) {
    const char* id = openpete_mods_id_of(self);
    if (!id || !write || !read || schema == 0u) return -1;
    if (!desc()) {
        OPL_WARN_F("modsave", "%s: save_register refused; this game has no save descriptor", id);
        return -1;
    }
    ms_reg_t* g = reg_of(id);
    if (!g) {
        if (s_n_regs == MS_MAX_REGS) return -1;
        g = &s_regs[s_n_regs++];
        snprintf(g->id, sizeof g->id, "%s", id);
    }
    g->self = self; g->schema = schema; g->write = write; g->read = read;
    OPL_INFO_F("modsave", "%s: save records registered (schema %u)", id, schema);
    return 0;
}

void openpete_modsave_remove_owner(const char* mod_id) {
    for (int i = 0; i < s_n_regs; i++) {
        if (strcmp(s_regs[i].id, mod_id)) continue;
        memmove(&s_regs[i], &s_regs[i + 1], (size_t)(s_n_regs - i - 1) * sizeof s_regs[0]);
        s_n_regs--;
        return;
    }
}

/** @brief Warn once per function when two or more mods override a save function without save_register. */
static void dup_override_check(void) {
    const openpete_save_desc_t* d = desc();
    if (!d) return;
    static uint32_t s_warned[8];
    uint32_t fns[8];
    size_t nf = 0;
    if (d->load_fn) fns[nf++] = d->load_fn;
    if (d->save_create_fn) fns[nf++] = d->save_create_fn;
    for (size_t f = 0; f < nf; f++) {
        const char* ids[2] = { NULL, NULL };
        int n = 0;
        for (int i = 0; i < openpete_mods_count(); i++) {
            const char* id = openpete_mods_id(i);
            if (!id || reg_of(id) || !openpete_mods_chain_has_owner(fns[f], id)) continue;
            if (n < 2) ids[n] = id;
            n++;
        }
        if (n < 2 || s_warned[f] == fns[f]) continue;
        s_warned[f] = fns[f];
        OPL_WARN_F("modsave", "%d mods override the save function at 0x%08X without save "
                   "records (%s, %s%s); they collide only if more than one stores data in the "
                   "save's spare bytes", n, fns[f], ids[0], ids[1], n > 2 ? ", ..." : "");
    }
}

void openpete_modsave_on_entered(void) { dup_override_check(); }

void openpete_modsave_on_mods_init(void) {
    if (!s_frozen) {
        s_frozen = 1;
        s_n_rows = (uint32_t)openpete_mods_count();
        s_rows = s_n_rows ? (ms_row_t*)calloc(s_n_rows, sizeof *s_rows) : NULL;
        if (!s_rows) s_n_rows = 0;
        for (uint32_t i = 0; i < s_n_rows; i++)
            snprintf(s_rows[i].id, sizeof s_rows[i].id, "%s", openpete_mods_id((int)i));
        s_blk.n_rows = s_n_rows;
    }
    if (openpete_mods_count() > 0 && desc()) openpete_modsave_armed = 1;
    dup_override_check();
}
