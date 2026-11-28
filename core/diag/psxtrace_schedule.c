/* core/diag/psxtrace_schedule.c - the reference's timing, replayed.
 *
 * A golden captured with the DuckStation fork's recorders carries sidecars
 * beside the trace: `<trace>.vsync` (every libetc VSync call: a waited
 * call with the VBLANK count at entry and return, a query with the
 * counter it returned, runs of equal queries as one row with a count),
 * `<trace>.loads` (every CdRead with its issue and completion VBLANK)
 * and `<trace>.srand` (every srand seed). When the trace is verified
 * they become openpete's schedule: each waited VSync charges the VBLs
 * the reference spent since the previous one, a synchronous read charges
 * its VBLs inside the read, an asynchronous read completes after the
 * reference's delay, each query answers the reference's value for that
 * read of the record (charging the VBLs whose interrupts the reference
 * had seen by then), and srand receives the reference's seed. The thunk
 * layer consumes the rows in order; a row whose shape differs from what
 * openpete issues (mode, read sectors or destination, exhausted rows at
 * the wrong record) is a schedule failure the verifier reports as a hard
 * fail with the record.
 */
#include "openpete_psxtrace.h"
#include "openpete_psx.h"
#include "openpete_log.h"
#include "openpete_rows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* One libetc VSync call of the reference, in call order: a waited call
 * (mode >= 0) with the VBLANK count at entry and return, a run of n equal
 * queries (mode -1) from one call site with the counter they returned, or
 * a callback registration (mode -2) with its VBLANK count. ra is the
 * caller's return address (0 in a sidecar without the column). */
typedef struct { uint32_t rec; int32_t mode; uint32_t entry_vbl, ret_vbl; int32_t counter; uint32_t n, ra; uint64_t entry_tick, ret_tick; } sched_vsync_t;
typedef struct { uint32_t idx, issue_rec, issue_vbl, sectors, dst, mode, done_rec, done_vbl; int done; uint64_t done_tick; } sched_read_t;

static sched_vsync_t* s_vs = NULL;   static uint32_t s_vs_n = 0, s_vs_next = 0, s_vs_left = 0;
static uint32_t s_vs_waits = 0;       /* waited rows the sidecar holds */
static uint32_t s_reg_total = 0, s_reg_charged = 0, s_reg_matched = 0;   /* registration rows */
static uint64_t s_q_total = 0;        /* queries the sidecar holds (runs expanded) */
static sched_read_t*  s_rd = NULL;   static uint32_t s_rd_n = 0, s_rd_next = 0;
static uint32_t*      s_seed = NULL; static uint32_t s_seed_n = 0, s_seed_next = 0;
/* `.rcnt2`: the game's reads of root counter 2's count register, in order. */
typedef struct { uint32_t rec; uint16_t value; } sched_rcnt2_t;
static sched_rcnt2_t* s_rc2 = NULL; static uint32_t s_rc2_n = 0, s_rc2_next = 0;
static uint32_t s_rc2_answered = 0, s_rc2_unmade = 0, s_rc2_skipped = 0;
static int      s_active = 0;
static FILE*    s_log = NULL;        /* OPENPETE_SCHED_LOG=path: every consumed row */
static uint32_t s_query_logged = 0;
static int      s_failed = 0;
static char     s_fail_reason[256];
static uint32_t s_reads_sync = 0, s_reads_async = 0, s_vsync_consumed = 0, s_long_frames = 0, s_boot_waits = 0;
/* Calls made past the trace's last record, which no sidecar row serves:
 * the capture stopped before the reference made them. */
static uint32_t s_vs_past_end = 0, s_rd_past_end = 0;
static int64_t  s_boot_offset = 0;   /* stamp[0] minus the position the boot reached */
static int32_t  s_counter_k = 0;    /* VBLANK count minus the libetc counter, constant per capture */
static int      s_counter_k_set = 0;
static uint64_t s_q_answered = 0;     /* queries answered from the sidecar */
static uint64_t s_q_extra = 0;        /* openpete's queries past the record's rows: answered pos - K */
static uint64_t s_q_unmade = 0;       /* the reference's queries openpete never made (rows dropped) */
static uint32_t s_q_charged = 0;      /* VBLs charged at a query (the interrupt landed inside the tick's work) */
static uint32_t s_q_behind = 0;       /* query rows of the record behind the position (dropped) */
static uint64_t s_wait_tick = 0;      /* exact return clock of the waited row last consumed */
static uint64_t s_query_tick = 0;     /* exact clock of the query row last answered from */
static void clock_open(const char* trace);
static void clock_skip_to(uint32_t rec);

int openpete_sched_active(void) { return s_active; }

/* Bit 0 .vsync, bit 1 .loads, bit 2 .srand: the sidecars of the schedule
 * proper that the last openpete_sched_open() read. */
static unsigned s_core_loaded = 0;
unsigned openpete_sched_core_loaded(void) { return s_core_loaded; }
int openpete_sched_failed(const char** reason) {
    if (reason) *reason = s_fail_reason;
    return s_failed;
}

void openpete_sched_fail(const char* fmt, ...) {
    if (s_failed) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_fail_reason, sizeof(s_fail_reason), fmt, ap);
    va_end(ap);
    s_failed = 1;
    OPL_ERROR_F("psxtrace", "schedule: %s", s_fail_reason);
}

/** @brief Read the sidecar `<trace><ext>` whole into a NUL-terminated heap buffer and check its magic prefix.
 *
 * A missing or unreadable file returns NULL silently; a file whose text
 * does not start with magic logs an error and returns NULL.
 * @param trace Path of the golden trace.
 * @param ext Sidecar suffix, such as ".vsync".
 * @param magic Required first bytes of the file.
 * @return The buffer, which the caller frees, or NULL. */
static char* read_sidecar(const char* trace, const char* ext, const char* magic) {
    char path[4096];
    snprintf(path, sizeof(path), "%s%s", trace, ext);
    FILE* fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char* buf = (char*)malloc((size_t)n + 1u);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { free(buf); fclose(fp); return NULL; }
    fclose(fp);
    buf[n] = 0;
    if (strncmp(buf, magic, strlen(magic)) != 0) {
        OPL_ERROR_F("psxtrace", "schedule: %s is not a %s file", path, magic);
        free(buf);
        return NULL;
    }
    return buf;
}

/** @brief Fail a sidecar load on an allocation failure: log it and release the sidecar's text and its row table.
 *
 * The caller then clears its table pointer and row count, which leaves the
 * sidecar as not loaded.
 * @param buf The sidecar's text.
 * @param table The rows read so far, or NULL.
 * @param ext The sidecar's extension, for the message.
 * @param rows The rows read so far.
 * @return 0, the loaders' failure value. */
static int sidecar_oom(char* buf, void* table, const char* ext, uint32_t rows) {
    OPL_ERROR_F("psxtrace", "schedule: out of memory after %u rows of %s; the sidecar is not loaded", rows, ext);
    free(buf);
    free(table);
    return 0;
}

/** @brief Fail a sidecar load on a row value above its bound: log it and release the sidecar's text and its row table.
 *
 * The caller then clears its table pointer and row count, as after
 * sidecar_oom.
 * @param buf The sidecar's text.
 * @param table The rows read so far, or NULL.
 * @param ext The sidecar's extension, for the message.
 * @param what The value's name, for the message.
 * @param value The row's value.
 * @param bound The largest value the loader accepts.
 * @return 0, the loaders' failure value. */
static int sidecar_refuse(char* buf, void* table, const char* ext, const char* what,
                          unsigned long long value, unsigned long long bound) {
    OPL_ERROR_F("psxtrace", "schedule: %s %llu in %s is above %llu; the sidecar is not loaded", what, value, ext, bound);
    free(buf);
    free(table);
    return 0;
}

/** @brief Load the `.vsync` sidecar into s_vs and count its waited rows, queries and registrations.
 *
 * Rows starting with `#` or `p` are skipped, as are rows with fewer than
 * eight fields. The first query row fixes the counter offset, its return
 * VBLANK minus the counter it returned.
 * @param trace Path of the golden trace.
 * @return 1 when the sidecar was read, else 0. */
static int load_vsync(const char* trace) {
    char* buf = read_sidecar(trace, ".vsync", "psxtrace-vsync");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_vs = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_vs), 4096u);
    if (!s_vs) return sidecar_oom(buf, NULL, ".vsync", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, evbl, rvbl; int mode; unsigned long long et, rt; unsigned v0, n = 1, ra = 0;
        if (sscanf(line, "%u %u %d %u %u %llu %llu %u %u %x", &idx, &rec, &mode, &evbl, &rvbl, &et, &rt, &v0, &n, &ra) < 8)
            continue;
        sched_vsync_t* grown = openpete_rows_reserve(s_vs, &cap, s_vs_n + 1ull, sizeof(*s_vs), 4096u);
        if (!grown) {
            sidecar_oom(buf, s_vs, ".vsync", s_vs_n);
            s_vs = NULL; s_vs_n = 0; s_vs_waits = 0; s_reg_total = 0; s_q_total = 0; s_counter_k = 0; s_counter_k_set = 0;
            return 0;
        }
        s_vs = grown;
        sched_vsync_t* r = &s_vs[s_vs_n++];
        r->rec = rec; r->mode = mode; r->entry_vbl = evbl; r->ret_vbl = rvbl;
        r->counter = (mode == -1) ? (int32_t)v0 : -1; r->n = (mode == -1 && n) ? n : 1u; r->ra = ra;
        r->entry_tick = et; r->ret_tick = rt;
        if (mode == -1) {
            if (!s_counter_k_set) { s_counter_k = (int32_t)rvbl - (int32_t)v0; s_counter_k_set = 1; }
            s_q_total += r->n;
        } else if (mode == -2) {
            s_reg_total++;
        } else {
            s_vs_waits++;
        }
    }
    free(buf);
    return 1;
}

/** @brief Load the `.loads` sidecar into s_rd, one row per CdRead with its issue and completion.
 *
 * A row whose completion record column is `-` is a read with no completion
 * callback. Rows with fewer than ten fields are skipped.
 * @param trace Path of the golden trace.
 * @return 1 when the sidecar was read, else 0. */
static int load_reads(const char* trace) {
    char* buf = read_sidecar(trace, ".loads", "psxtrace-loads");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_rd = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_rd), 256u);
    if (!s_rd) return sidecar_oom(buf, NULL, ".loads", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, irec, ivbl, iline, sectors, dst, mode; unsigned long long itick;
        char d_rec[32], d_vbl[32], d_tick[32] = "-";
        if (sscanf(line, "%u %u %u %llu %u %u %x %x %31s %31s %31s", &idx, &irec, &ivbl, &itick, &iline,
                   &sectors, &dst, &mode, d_rec, d_vbl, d_tick) < 10)
            continue;
        sched_read_t* grown = openpete_rows_reserve(s_rd, &cap, s_rd_n + 1ull, sizeof(*s_rd), 256u);
        if (!grown) { sidecar_oom(buf, s_rd, ".loads", s_rd_n); s_rd = NULL; s_rd_n = 0; return 0; }
        s_rd = grown;
        sched_read_t* r = &s_rd[s_rd_n++];
        r->idx = idx; r->issue_rec = irec; r->issue_vbl = ivbl; r->sectors = sectors; r->dst = dst; r->mode = mode;
        r->done = (d_rec[0] != '-');
        r->done_rec = r->done ? (uint32_t)strtoul(d_rec, NULL, 10) : 0u;
        r->done_vbl = r->done ? (uint32_t)strtoul(d_vbl, NULL, 10) : 0u;
        r->done_tick = (r->done && d_tick[0] != '-') ? (uint64_t)strtoull(d_tick, NULL, 10) : 0u;
    }
    free(buf);
    return 1;
}

/** @brief Load the `.srand` sidecar into s_seed, the reference's srand seeds in call order.
 *
 * @param trace Path of the golden trace.
 * @return 1 when the sidecar was read, else 0. */
static int load_seeds(const char* trace) {
    char* buf = read_sidecar(trace, ".srand", "psxtrace-srand");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_seed = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_seed), 16u);
    if (!s_seed) return sidecar_oom(buf, NULL, ".srand", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, seed;
        if (sscanf(line, "%u %u %u %x", &idx, &rec, &vbl, &seed) != 4) continue;
        uint32_t* grown = openpete_rows_reserve(s_seed, &cap, s_seed_n + 1ull, sizeof(*s_seed), 16u);
        if (!grown) { sidecar_oom(buf, s_seed, ".srand", s_seed_n); s_seed = NULL; s_seed_n = 0; return 0; }
        s_seed = grown;
        s_seed[s_seed_n++] = seed;
    }
    free(buf);
    return 1;
}

/** @brief Load the `.rcnt2` sidecar into s_rc2: the game's reads of root counter 2's count register.
 *
 * The capture records the reads of the instructions its recipe names, the
 * game code's own reads that openpete executes.
 * @param trace Path of the golden trace.
 * @return 1 when the sidecar was read, else 0. */
static int load_rcnt2(const char* trace) {
    char* buf = read_sidecar(trace, ".rcnt2", "psxtrace-rcnt2");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_rc2 = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_rc2), 16u);
    if (!s_rc2) return sidecar_oom(buf, NULL, ".rcnt2", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, pc, value;
        if (sscanf(line, "%u %u %u %x %x", &idx, &rec, &vbl, &pc, &value) != 5) continue;
        (void)pc;
        sched_rcnt2_t* grown = openpete_rows_reserve(s_rc2, &cap, s_rc2_n + 1ull, sizeof(*s_rc2), 16u);
        if (!grown) { sidecar_oom(buf, s_rc2, ".rcnt2", s_rc2_n); s_rc2 = NULL; s_rc2_n = 0; return 0; }
        s_rc2 = grown;
        s_rc2[s_rc2_n].rec = rec;
        s_rc2[s_rc2_n].value = (uint16_t)value;
        s_rc2_n++;
    }
    free(buf);
    return 1;
}

static uint32_t s_pos = 0;          /* the reference VBLANK count openpete has reached */
static int      s_pos_set = 0;
static const uint32_t* s_stamps = NULL; static uint32_t s_stamps_n = 0;
/* VBLANKs the position counted that the CPU has not taken: held by a
 * critical section and not yet flushed. The libetc counter counts only the
 * VBLANKs taken, so counter = position - K - D. Reset at anchor 0's rebase,
 * which absorbs every drop before it. */
static int32_t  s_dropped = 0;
/* Conversions made while D was nonzero, per site (wait return, VSync(-1)
 * answer, replay row skip, charge target), and those whose counter was not
 * position - K - D. */
static uint32_t s_conv_hits[4], s_conv_bad[4];
/* Polling-loop quanta under the schedule: delivered (a VBLANK the
 * reference's rows show due), refused (no row due: counted only), the
 * rows the schedule consumed later in a record a quantum fired in, and on
 * the clock row the reference's `insns` column of each row a quantum
 * delivered against openpete's loop instructions since the anchor. */
static uint32_t s_lq_delivered = 0, s_lq_norow = 0, s_lq_after = 0;
static uint32_t s_lq_rec = 0xFFFFFFFFu;               /* record of the last quantum */
static uint64_t s_lq_ref_insns = 0, s_lq_op_insns = 0;
static uint32_t s_lq_rows = 0;

void openpete_sched_set_stamps(const uint32_t* stamps, uint32_t count) { s_stamps = stamps; s_stamps_n = count; }
static const uint32_t* s_anchor_ticks = NULL; static uint32_t s_anchor_ticks_n = 0;
void openpete_sched_set_anchor_ticks(const uint32_t* ticks, uint32_t count) { s_anchor_ticks = ticks; s_anchor_ticks_n = count; }
int openpete_sched_anchor(uint32_t rec, uint32_t* vbl_out, uint32_t* tick_out) {
    if (!s_stamps || !s_anchor_ticks || rec >= s_stamps_n || rec >= s_anchor_ticks_n) return 0;
    *vbl_out = s_stamps[rec];
    *tick_out = s_anchor_ticks[rec];
    return 1;
}
uint32_t openpete_sched_pos(void) { return s_pos; }
void openpete_sched_advance_pos(uint32_t vbls) { s_pos += vbls; }
int openpete_sched_pos_known(void) { return s_pos_set; }
void openpete_sched_drop_vbl(void) { s_dropped++; }
void openpete_sched_take_dropped(void) { s_dropped--; }
int32_t openpete_sched_dropped(void) { return s_dropped; }
void openpete_sched_set_dropped(int32_t d) { s_dropped = d; }

/** @brief Count a counter-to-VBLANK conversion made while VBLANKs are dropped, and check it.
 * @param site 0 wait return, 1 VSync(-1) answer, 2 replay row skip, 3 charge target.
 * @param counter The counter on the libetc side of the conversion.
 * @param vblank The VBLANK on the position side. */
static void conv_note(int site, int32_t counter, uint32_t vblank) {
    if (!s_dropped) return;
    s_conv_hits[site]++;
    if (counter != (int32_t)vblank - s_counter_k - s_dropped) s_conv_bad[site]++;
}

void openpete_sched_conv_report(uint32_t hits[4], uint32_t bad[4]) {
    for (int i = 0; i < 4; i++) { hits[i] = s_conv_hits[i]; bad[i] = s_conv_bad[i]; }
}


/* The load windows the reference's watched word describes: record indexes
 * the loader was active from and to (the first record hashed with the
 * value that entered the window, the first hashed with the value that
 * left it), by the game's predicate. */
typedef struct { uint32_t enter_rec, leave_rec; } sched_window_t;
static sched_window_t* s_win = NULL; static uint32_t s_win_n = 0, s_win_next = 0;
static uint32_t sched_cur_rec(void);
static int64_t  s_rec_offset = 0;     /* reference record index minus openpete's hit-derived one */
static int      s_in_load = 0, s_in_load_known = 0;
static uint32_t s_records_skipped = 0, s_windows_seen = 0, s_load_waits = 0;
extern int psyq_game_load_window_active(uint32_t value) __attribute__((weak));

/** @brief Load the `.watch` sidecar into s_win, the record ranges during which the loader was active.
 *
 * Each row is a store to the watched word. The store's new value is
 * classified by the game's psyq_game_load_window_active predicate one row
 * later, as the next row's old value, so a window opens or closes at the
 * previous row's record. A window still open at the end keeps a leave
 * record of 0xFFFFFFFF and is not counted in s_win_n.
 * @param trace Path of the golden trace.
 * @return 1 when the sidecar was read and the game supplies the predicate,
 *         else 0. */
static int load_watch(const char* trace) {
    char* buf = read_sidecar(trace, ".watch", "psxtrace-watch");
    if (!buf) return 0;
    if (!psyq_game_load_window_active) { free(buf); return 0; }
    uint32_t cap = 0;
    s_win = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_win), 64u);
    if (!s_win) return sidecar_oom(buf, NULL, ".watch", 0);
    /* Each store's value is the next store's "old" (the breakpoint fires
     * before the store lands). */
    unsigned prev_rec = 0; int have_prev = 0; int active = 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, pc, oldv, newv;
        if (sscanf(line, "%u %u %u %x %x %x", &idx, &rec, &vbl, &pc, &oldv, &newv) != 6) continue;
        if (have_prev) {
            int a = psyq_game_load_window_active(oldv);   /* value the previous store set */
            if (a && !active) {
                sched_window_t* grown = openpete_rows_reserve(s_win, &cap, s_win_n + 1ull, sizeof(*s_win), 64u);
                if (!grown) { sidecar_oom(buf, s_win, ".watch", s_win_n); s_win = NULL; s_win_n = 0; return 0; }
                s_win = grown;
                s_win[s_win_n].enter_rec = prev_rec; s_win[s_win_n].leave_rec = 0xFFFFFFFFu;
            } else if (!a && active && s_win_n < cap) {
                s_win[s_win_n].leave_rec = prev_rec; s_win_n++;
            }
            active = a;
        }
        prev_rec = rec; have_prev = 1;
    }
    free(buf);
    return 1;
}

/* Two bitmaps over the reference's VBLANKs. `s_sio0`: the vblanks the
 * kernel's controller/card handler ran on, from its store to its port-flip
 * word (.sio0); a vblank without one is a vblank the handler did not run
 * (its interrupt was taken by an earlier handler) or the card driver's
 * abort path, which returns before the flip. `s_padcmd`: the vblanks the
 * recorder saw the controller accept a command on (.padcmd), the vsyncs
 * libpad's command sequence advanced on; a served vblank without one is
 * an exchange libpad abandoned. */
typedef struct { uint8_t* bits; uint32_t n, first, served, skipped; } vbl_set_t;
/* The bitmap sidecars' rows name VBLANKs below this bound: 2^26 VBLANKs is
 * about 312 hours at 59.826 Hz and an 8 MiB bitmap. */
#define SCHED_VBL_MAX (1u << 26)
static vbl_set_t s_sio0, s_padcmd;

/** @brief Add reference VBLANK vbl to the bitmap set, growing it by doubling from 4096 bits.
 *
 * Tracks the lowest member in first. New bits start cleared. A failed
 * growth leaves the set unchanged. The loaders refuse a VBLANK at or above
 * SCHED_VBL_MAX first; the growth is computed in 64 bits regardless.
 * @param v The set.
 * @param vbl The VBLANK.
 * @return 1 when vbl was added, 0 when the growth failed. */
static int vbl_set_add(vbl_set_t* v, uint32_t vbl) {
    if (vbl >= v->n) {
        uint64_t n = v->n ? v->n : 4096u;
        while (n <= vbl) n *= 2u;
        if (n > UINT32_MAX) n = UINT32_MAX;
        uint8_t* grown = (uint8_t*)realloc(v->bits, (size_t)(n / 8u + 1u));
        if (!grown) return 0;
        v->bits = grown;
        memset(v->bits + (v->n / 8u), 0, (size_t)((n - v->n) / 8u + 1u));
        v->n = (uint32_t)n;
    }
    v->bits[vbl / 8u] |= (uint8_t)(1u << (vbl % 8u));
    if (!v->first || vbl < v->first) v->first = vbl;
    return 1;
}

/** @brief Empty a bitmap set after its sidecar load failed, which leaves the sidecar as not loaded.
 * @param v The set.
 * @return 0, the loaders' failure value. */
static int vbl_set_drop(vbl_set_t* v) {
    free(v->bits);
    memset(v, 0, sizeof(*v));
    return 0;
}
/** @brief Return 1 when the set holds vbl, counting the answer as served or skipped.
 *
 * Outside the covered range, or with the schedule inactive or the set
 * empty, known is set to 0 and the answer is 1 without counting. known may
 * be NULL. */
static int vbl_set_has(vbl_set_t* v, uint32_t vbl, int* known) {
    if (!s_active || !v->bits || vbl < v->first || vbl >= v->n) { if (known) *known = 0; return 1; }
    if (known) *known = 1;
    int has = (v->bits[vbl / 8u] >> (vbl % 8u)) & 1;
    if (has) v->served++; else v->skipped++;
    return has;
}

/** @brief Load the `.padcmd` sidecar into s_padcmd, the vblanks the controller accepted a command on.
 *
 * The set's range is trimmed to the last vblank listed.
 * @param trace Path of the golden trace.
 * @return 1 when the set holds a range, else 0. */
static int load_padcmd(const char* trace) {
    char* buf = read_sidecar(trace, ".padcmd", "psxtrace-padcmd");
    if (!buf) return 0;
    uint32_t max = 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, port, cmd;
        if (sscanf(line, "%u %u %u %u %x", &idx, &rec, &vbl, &port, &cmd) != 5) continue;
        if (vbl >= SCHED_VBL_MAX) {
            sidecar_refuse(buf, NULL, ".padcmd", "vblank", vbl, SCHED_VBL_MAX - 1u);
            return vbl_set_drop(&s_padcmd);
        }
        if (!vbl_set_add(&s_padcmd, vbl)) { sidecar_oom(buf, NULL, ".padcmd", s_padcmd.n); return vbl_set_drop(&s_padcmd); }
        if (vbl > max) max = vbl;
    }
    free(buf);
    if (max + 1u < s_padcmd.n) s_padcmd.n = max + 1u;
    return s_padcmd.n != 0;
}

/** @brief Load the `.sio0` sidecar into s_sio0, the vblanks the controller and card handler ran on.
 *
 * A row names a first vblank and a run length n; every vblank of the run
 * is added. The set's range is trimmed to the last vblank listed.
 * @param trace Path of the golden trace.
 * @return 1 when the set holds a range, else 0. */
static int load_sio0(const char* trace) {
    char* buf = read_sidecar(trace, ".sio0", "psxtrace-sio0");
    if (!buf) return 0;
    uint32_t max = 0;
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, n;
        if (sscanf(line, "%u %u %u %u", &idx, &rec, &vbl, &n) != 4) continue;
        if (n == 0) continue;
        if (vbl >= SCHED_VBL_MAX || n > SCHED_VBL_MAX - vbl) {
            sidecar_refuse(buf, NULL, ".sio0", "run end", (unsigned long long)vbl + n - 1u, SCHED_VBL_MAX - 1u);
            return vbl_set_drop(&s_sio0);
        }
        for (unsigned i = 0; i < n; i++)
            if (!vbl_set_add(&s_sio0, vbl + i)) { sidecar_oom(buf, NULL, ".sio0", s_sio0.n); return vbl_set_drop(&s_sio0); }
        if (vbl + n - 1u > max) max = vbl + n - 1u;
    }
    free(buf);
    if (max + 1u < s_sio0.n) s_sio0.n = max + 1u;
    return s_sio0.n != 0;
}

/* The reference's CdSync polls: one row per run of calls with the same
 * record, VBLANK, mode, value and call site. A poll of openpete's takes
 * the value of the next row of its record with its mode and site, and a
 * row whose VBLANK is ahead of the position charges the VBLs up to it (a
 * game spinning on the drive spent them in the spin). Rows of earlier
 * records are polls openpete's tick did not make. */
typedef struct { uint32_t rec, vbl, ra, n; int32_t mode, ret; uint64_t tick; } sched_cdsync_t;
/* The reference's clock (CPU cycles) at the row the last waited VSync or
 * CdSync poll was answered from; 0 when the answer came from no row. An
 * event the reference recorded with a clock value (a read's completion
 * callback) precedes that call when its value is lower. */
static uint64_t s_last_tick = 0, s_read_done_tick = 0;
uint64_t openpete_sched_last_tick(void) { return s_last_tick; }
uint64_t openpete_sched_read_done_tick(void) { return s_read_done_tick; }

static sched_cdsync_t* s_cs = NULL; static uint32_t s_cs_n = 0, s_cs_next = 0, s_cs_left = 0;
static uint32_t s_cs_answered = 0, s_cs_unmade = 0, s_cs_charged = 0;

/** @brief Load the `.cdsync` sidecar into s_cs, one row per run of equal CdSync polls.
 *
 * Rows that do not parse as eight fields are skipped.
 * @param trace Path of the golden trace.
 * @return 1 when at least one row loaded, else 0. */
static int load_cdsync(const char* trace) {
    char* buf = read_sidecar(trace, ".cdsync", "psxtrace-cdsync");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_cs = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_cs), 1024u);
    if (!s_cs) return sidecar_oom(buf, NULL, ".cdsync", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, n, ra; int mode, ret; unsigned long long tick;
        if (sscanf(line, "%u %u %u %d %d %u %x %llu", &idx, &rec, &vbl, &mode, &ret, &n, &ra, &tick) != 8) continue;
        sched_cdsync_t* grown = openpete_rows_reserve(s_cs, &cap, s_cs_n + 1ull, sizeof(*s_cs), 1024u);
        if (!grown) { sidecar_oom(buf, s_cs, ".cdsync", s_cs_n); s_cs = NULL; s_cs_n = 0; return 0; }
        s_cs = grown;
        s_cs[s_cs_n].rec = rec; s_cs[s_cs_n].vbl = vbl; s_cs[s_cs_n].ra = ra; s_cs[s_cs_n].n = n;
        s_cs[s_cs_n].mode = mode; s_cs[s_cs_n].ret = ret; s_cs[s_cs_n].tick = tick; s_cs_n++;
    }
    free(buf);
    return s_cs_n != 0;
}

int openpete_sched_cdsync(int mode, uint32_t ra, int32_t* ret_out, uint32_t* target_out) {
    s_last_tick = 0;
    if (!s_active || !s_cs || s_failed) return 0;
    const uint32_t cur_rec = sched_cur_rec();
    while (s_cs_next < s_cs_n && s_cs[s_cs_next].rec < cur_rec) { s_cs_unmade += s_cs_left ? s_cs_left : s_cs[s_cs_next].n; s_cs_next++; s_cs_left = 0; }
    uint32_t k = s_cs_next;
    while (k < s_cs_n && s_cs[k].rec == cur_rec && (s_cs[k].mode != mode || s_cs[k].ra != ra)) k++;
    if (k >= s_cs_n || s_cs[k].rec != cur_rec) return 0;
    if (k != s_cs_next) { s_cs_unmade += s_cs_left ? s_cs_left : 0; s_cs_next = k; s_cs_left = 0; }
    if (!s_cs_left) s_cs_left = s_cs[k].n;
    *ret_out = s_cs[k].ret;
    *target_out = s_cs[k].vbl;
    s_last_tick = s_cs[k].tick;
    if (s_cs[k].vbl > s_pos) {
        s_cs_charged += s_cs[k].vbl - s_pos;
        if (cur_rec == s_lq_rec) s_lq_after++;
    }
    s_cs_answered++;
    if (--s_cs_left == 0) s_cs_next = k + 1;
    return 1;
}

/* Scans from the row cursor without moving it. */
int openpete_sched_cdsync_peek(uint32_t* ra_out, uint64_t* tick_out) {
    if (!s_active || !s_cs || s_failed) return 0;
    const uint32_t cur_rec = sched_cur_rec();
    uint32_t k = s_cs_next;
    while (k < s_cs_n && s_cs[k].rec < cur_rec) k++;
    if (k >= s_cs_n || s_cs[k].rec != cur_rec) return 0;
    *ra_out = s_cs[k].ra; *tick_out = s_cs[k].tick;
    return 1;
}

/* The reference's card transfer starts (.card): per start the VBLANK, the
 * tick offset from it and, when the row carries them, the step ticks from
 * the start (a count, then the ticks; held in s_card_steps from `first`),
 * in VBLANK order. */
typedef struct { uint32_t vbl, off, first, n; } sched_card_t;
static sched_card_t* s_card = NULL; static uint32_t s_card_n = 0;
static uint32_t* s_card_steps = NULL; static uint32_t s_card_steps_n = 0, s_card_steps_cap = 0;
static uint32_t s_card_timed = 0;   /* rows carrying steps */

/* The most step ticks one `.card` row may carry. */
#define SCHED_CARD_STEPS_MAX (1u << 16)

/** @brief Release the card transfer starts and their steps after the `.card` load failed, which leaves the sidecar as not loaded.
 *
 * The caller has logged the failure and released the sidecar's text and
 * s_card (sidecar_oom, sidecar_refuse).
 * @return 0, the loader's failure value. */
static int card_drop(void) {
    free(s_card_steps);
    s_card = NULL; s_card_n = 0; s_card_timed = 0;
    s_card_steps = NULL; s_card_steps_n = 0; s_card_steps_cap = 0;
    return 0;
}

/** @brief Load the reference's card transfer starts and their steps from the `.card` sidecar into s_card.
 *
 * A row is `idx rec vbl off`, then, from version 2, the step count and
 * that many step ticks; a row without them leaves the transfer to the
 * cadence table.
 * @param trace Path of the golden trace.
 * @return 1 when at least one start loaded, else 0. */
static int load_card(const char* trace) {
    char* buf = read_sidecar(trace, ".card", "psxtrace-card");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_card = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_card), 256u);
    if (!s_card) return sidecar_oom(buf, NULL, ".card", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, nsteps = 0; unsigned long long off; int used = 0;
        if (sscanf(line, "%u %u %u %llu%n", &idx, &rec, &vbl, &off, &used) != 4) continue;
        sched_card_t* grown = openpete_rows_reserve(s_card, &cap, s_card_n + 1ull, sizeof(*s_card), 256u);
        if (!grown) { sidecar_oom(buf, s_card, ".card", s_card_n); return card_drop(); }
        s_card = grown;
        sched_card_t* row = &s_card[s_card_n++];
        row->vbl = vbl; row->off = (uint32_t)off; row->first = s_card_steps_n; row->n = 0;
        const char* p = line + used; int adv = 0;
        if (sscanf(p, "%u%n", &nsteps, &adv) != 1) continue;
        p += adv;
        if (nsteps > SCHED_CARD_STEPS_MAX) {
            sidecar_refuse(buf, s_card, ".card", "step count", nsteps, SCHED_CARD_STEPS_MAX);
            return card_drop();
        }
        uint32_t* steps = openpete_rows_reserve(s_card_steps, &s_card_steps_cap, (uint64_t)s_card_steps_n + nsteps + 1u,
                                                sizeof(*s_card_steps), 4096u);
        if (!steps) { sidecar_oom(buf, s_card, ".card", s_card_n); return card_drop(); }
        s_card_steps = steps;
        for (unsigned i = 0; i < nsteps && sscanf(p, "%u%n", &s_card_steps[s_card_steps_n], &adv) == 1; i++, p += adv) {
            s_card_steps_n++; row->n++;
        }
        if (row->n) s_card_timed++;
    }
    free(buf);
    return s_card_n != 0;
}

int openpete_sched_card_start(uint32_t vbl, uint32_t* off_out) {
    if (!s_active || !s_card_n) return 0;
    uint32_t lo = 0, hi = s_card_n;
    while (lo < hi) { const uint32_t mid = (lo + hi) / 2u; if (s_card[mid].vbl < vbl) lo = mid + 1u; else hi = mid; }
    if (lo >= s_card_n || s_card[lo].vbl != vbl) return 0;
    *off_out = s_card[lo].off;
    return 1;
}

const uint32_t* openpete_sched_card_steps(uint32_t vbl, uint32_t* n_out) {
    *n_out = 0;
    if (!s_active || !s_card_n) return NULL;
    uint32_t lo = 0, hi = s_card_n;
    while (lo < hi) { const uint32_t mid = (lo + hi) / 2u; if (s_card[mid].vbl < vbl) lo = mid + 1u; else hi = mid; }
    if (lo >= s_card_n || s_card[lo].vbl != vbl || !s_card[lo].n) return NULL;
    *n_out = s_card[lo].n;
    return s_card_steps + s_card[lo].first;
}

int openpete_sched_sio0_served(uint32_t vbl, int* known) { return vbl_set_has(&s_sio0, vbl, known); }
/* Before the first recorded command the game had not started polling the
 * controller, so the sidecar says nothing about those vblanks. */
int openpete_sched_padcmd_at(uint32_t vbl, int* known) { return vbl_set_has(&s_padcmd, vbl, known); }

int openpete_sched_open(const char* trace_path) {
    int vs = load_vsync(trace_path);
    int rd = load_reads(trace_path);
    int sd = load_seeds(trace_path);
    int r2 = load_rcnt2(trace_path);
    int wt = load_watch(trace_path);
    int pc = load_padcmd(trace_path);
    int so = load_sio0(trace_path);
    int cs = load_cdsync(trace_path);
    int cd = load_card(trace_path);
    s_core_loaded = (vs ? 1u : 0u) | (rd ? 2u : 0u) | (sd ? 4u : 0u);
    if (!vs && !rd && !sd) return 0;
    s_active = 1;
    clock_open(trace_path);
    if (wt) OPL_INFO_F("psxtrace", "schedule: %u load windows from the watched word", s_win_n);
    if (pc) OPL_INFO_F("psxtrace", "schedule: controller commands known from vblank %u to %u", s_padcmd.first, s_padcmd.n - 1u);
    if (so) OPL_INFO_F("psxtrace", "schedule: controller handler vblanks known from %u to %u", s_sio0.first, s_sio0.n - 1u);
    if (cs) OPL_INFO_F("psxtrace", "schedule: %u CdSync poll rows", s_cs_n);
    if (cd) OPL_INFO_F("psxtrace", "schedule: %u card transfer starts, %u with their steps", s_card_n, s_card_timed);
    if (r2) {
        openpete_psx_set_root_counter2_reader(openpete_sched_rcnt2_read);
        OPL_INFO_F("psxtrace", "schedule: %u game reads of root counter 2", s_rc2_n);
    }
    { const char* lp = getenv("OPENPETE_SCHED_LOG"); if (lp && *lp) { s_log = fopen(lp, "w"); if (s_log) setvbuf(s_log, NULL, _IOLBF, 0); } }
    /* The position starts at the first event; the game-visible counter is
     * the VBLANK count minus a constant the first query row gives
     * (libetc's counter starts at its own init, later than the
     * emulator's). */
    {
        uint32_t first = 0xFFFFFFFFu;
        if (s_vs_n) first = s_vs[0].entry_vbl;
        if (s_rd_n && s_rd[0].issue_vbl < first) first = s_rd[0].issue_vbl;
        if (first != 0xFFFFFFFFu) { s_pos = first; s_pos_set = 1; }
    }
    OPL_INFO_F("psxtrace", "schedule: %s%u waited VSync rows and %llu queries, %s%u reads, %s%u seeds",
               vs ? "" : "no .vsync: ", s_vs_waits, (unsigned long long)s_q_total, rd ? "" : "no .loads: ", s_rd_n, sd ? "" : "no .srand: ", s_seed_n);
    return 1;
}

/** @brief Set the position to vbl when it is not yet known.
 *
 * The first consumed waited VSync supplies its entry VBLANK and the first
 * consumed read its issue VBLANK; no VBL passes on openpete before the
 * schedule is first met. */
static void pos_init_from(uint32_t vbl) {
    if (!s_pos_set) { s_pos = vbl; s_pos_set = 1; }
}

/** @brief Return whether OPENPETE_SCHED_BOOT_OWN is present, read once.
 *
 * Set, the rows of record 0 are skipped and the boot's waits and reads take
 * openpete's own count (any value, the empty one included, counts).
 * @return 1 when the variable is present. */
static int sched_boot_own(void) {
    static int v = -1;
    if (v < 0) v = getenv("OPENPETE_SCHED_BOOT_OWN") != NULL;
    return v;
}

/* After a schedule failure this returns 0 and the wait runs under openpete's own count. */
int openpete_sched_next_vsync(int mode, uint32_t* target_out) {
    if (!s_active || !s_vs || s_failed) return 0;
    s_last_tick = 0;
    s_wait_tick = 0;
    extern uint32_t openpete_psxtrace_anchor_hits;
    /* The sidecar's rec column is the records written when the call was
     * made: the record after the hit the call follows. openpete's current
     * record is the same count. */
    const uint32_t cur_rec = sched_cur_rec();
    if (s_stamps && cur_rec >= s_stamps_n) { s_vs_past_end++; return 0; }   /* past the trace's last record */
    if (s_in_load) {
        /* Inside a load window the records do not correspond and neither do
         * the waits: each charges its own count; the window's end sets the
         * position to the reference's. */
        if (!s_pos_set) return 0;
        *target_out = s_pos + (uint32_t)((mode == 0) ? 1 : mode);
        s_load_waits++;
        if (s_log) fprintf(s_log, "load-wait op_hit=%u mode=%d pos=%u\n", openpete_psxtrace_anchor_hits, mode, s_pos);
        return 1;
    }
    if (cur_rec == 0 && sched_boot_own()) {
        /* Before the first anchor hit the boot's call sequence is not the
         * reference's on every game (a card probe, a splash the game
         * times by rand); the rows of record 0 are skipped and a wait
         * charges its own count, the position advancing with it. The
         * first anchor's stamp reconciles the position. */
        while (s_vs_next < s_vs_n && s_vs[s_vs_next].rec == 0) { s_vs_next++; s_vs_left = 0; }
        if (!s_pos_set) return 0;
        *target_out = s_pos + (uint32_t)((mode == 0) ? 1 : mode);
        s_boot_waits++;
        if (s_log && s_boot_waits <= 2000) fprintf(s_log, "boot-wait %u mode=%d pos=%u\n", s_boot_waits, mode, s_pos);
        return 1;
    }
    /* The next waited row of this record or an earlier one; the query
     * rows before it are reads the reference made before waiting that
     * openpete did not (its tick's work spent no VBL). */
    uint32_t w = s_vs_next;
    while (w < s_vs_n && s_vs[w].rec <= cur_rec && s_vs[w].mode < 0) w++;
    if (w >= s_vs_n || s_vs[w].rec > cur_rec) {
        /* The reference made no more waited calls before its next anchor
         * hit: its frame ran long enough that the game's wait loop saw the
         * VBLs pass in its own work. openpete's call charges to that hit's
         * stamp and consumes no row. */
        if (s_stamps && cur_rec < s_stamps_n && s_pos_set) {
            if (s_log) fprintf(s_log, "vsync-long rec=%u target=%u pos=%u\n", cur_rec, s_stamps[cur_rec], s_pos);
            if (s_stamps[cur_rec] <= s_pos) {
                /* Nothing left to charge: the reference's tick reached
                 * its next hit without this wait. A wait charging nothing
                 * would spin the game's wait loop forever, so the schedule
                 * ends here and the wait runs under openpete's count. */
                openpete_sched_fail("VSync(%d) at record %u: the reference made no wait here (position %u at its stamp)",
                                    mode, cur_rec, s_pos);
                return 0;
            }
            *target_out = s_stamps[cur_rec];
            return 1;
        }
        if (w >= s_vs_n)
            openpete_sched_fail("VSync(%d) call beyond the sidecar's %u waited rows", mode, s_vs_waits);
        return 0;
    }
    for (; s_vs_next < w; s_vs_next++) if (s_vs[s_vs_next].mode == -1) s_q_unmade += s_vs_left ? s_vs_left : s_vs[s_vs_next].n;
    s_vs_left = 0;
    const sched_vsync_t* r = &s_vs[s_vs_next++];
    if (s_log) {
        extern uint32_t openpete_psxtrace_anchor_hits;
        fprintf(s_log, "vsync %u ref_rec=%u op_hit=%u entry=%u ret=%u pos=%u\n", s_vs_next - 1, r->rec,
                openpete_psxtrace_anchor_hits, r->entry_vbl, r->ret_vbl, s_pos);
    }
    if (r->mode != mode)
        openpete_sched_fail("VSync row %u: mode %d, openpete called VSync(%d) (record %u)", s_vs_next - 1, r->mode, mode, r->rec);
    pos_init_from(r->entry_vbl);
    if (cur_rec == s_lq_rec && r->entry_vbl > s_pos) s_lq_after++;
    s_last_tick = r->entry_tick;
    s_wait_tick = r->ret_tick;
    s_vsync_consumed++;
    /* VBLs the reference spent in game code after this wait returned (a
     * long frame) are charged at the next anchor hit: the tick's work
     * read its counters before the interrupts landed. */
    *target_out = r->ret_vbl;
    return 1;
}

uint32_t openpete_sched_current_record(void) { return sched_cur_rec(); }

/** @brief Return the reference record index the next anchor hit produces.
 *
 * Anchor hits from the target hit on count from record 0; the load-window
 * re-alignment offset is added and the result is floored at 0. */
static uint32_t sched_cur_rec(void) {
    extern uint32_t openpete_psxtrace_anchor_hits;
    extern uint32_t openpete_psxtrace_anchor_hit_target;
    int64_t r = (openpete_psxtrace_anchor_hits >= openpete_psxtrace_anchor_hit_target)
                    ? (int64_t)openpete_psxtrace_anchor_hits - openpete_psxtrace_anchor_hit_target + 1 : 0;
    r += s_rec_offset;
    return r < 0 ? 0u : (uint32_t)r;
}

/* Leaving a window re-aligns the record index with the reference's window end. */
int openpete_sched_anchor_window(uint32_t record, uint32_t value, uint32_t* skip_to_rec) {
    if (!s_active || s_failed) return 0;
    int a = (s_win && psyq_game_load_window_active) ? psyq_game_load_window_active(value) : 0;
    int was = s_in_load_known ? s_in_load : 0;
    if (s_log && (a || was || getenv("OPENPETE_SCHED_LOG_REC"))) fprintf(s_log, "anchor record=%u word=0x%X pos=%u\n", record, value, s_pos);
    if (!s_win || !psyq_game_load_window_active) return 0;
    s_in_load = a; s_in_load_known = 1;
    if (a && !was) s_windows_seen++;
    if (!a && was) {
        if (s_win_next >= s_win_n) {
            openpete_sched_fail("load window %u ended at record %u; the reference's watched word saw %u windows",
                                s_windows_seen, record, s_win_n);
            return 0;
        }
        const sched_window_t* w = &s_win[s_win_next++];
        if (w->leave_rec == 0xFFFFFFFFu) return 0;  /* the reference's window ran to the trace's end */
        if (w->leave_rec < record) {
            openpete_sched_fail("load window %u: openpete left it at record %u, the reference at %u (openpete's loader took longer)",
                                s_windows_seen, record, w->leave_rec);
            return 0;
        }
        if (w->leave_rec > record) {
            s_records_skipped += w->leave_rec - record;
            s_rec_offset += (int64_t)(w->leave_rec - record);
            /* Rows inside the reference's window openpete never made. */
            while (s_vs && s_vs_next < s_vs_n && s_vs[s_vs_next].rec <= w->leave_rec) s_vs_next++;
            s_vs_left = 0;
            clock_skip_to(w->leave_rec);
            if (s_log) fprintf(s_log, "window %u: openpete record %u -> reference record %u (%u skipped)\n",
                               s_windows_seen, record, w->leave_rec, w->leave_rec - record);
            *skip_to_rec = w->leave_rec;
            return 1;
        }
    }
    return 0;
}

int openpete_sched_in_load(void) { return s_in_load; }

int openpete_sched_read_issued_at(uint32_t rec) {
    for (uint32_t i = 0; i < s_rd_n; i++)
        if (s_rd[i].issue_rec == rec) return 1;
    return 0;
}

/* A callback registration: the next registration row of this record (or
 * an earlier one) gives the VBLANK the reference registered at; the VBLs
 * its tick's work spent before it are charged there, so the callback
 * does not run for them. Rows before it that are reads or waits stay.
 * Returns 1 with the target when the position lies behind it. */
int openpete_sched_registration(uint32_t ra, uint32_t* target_out) {
    if (!s_active || !s_vs || s_failed || !s_pos_set) return 0;
    const uint32_t cur_rec = sched_cur_rec();
    if (s_in_load || (s_stamps && cur_rec >= s_stamps_n)) return 0;
    uint32_t k = s_vs_next;
    while (k < s_vs_n && s_vs[k].rec <= cur_rec && s_vs[k].mode != -2) k++;
    if (k >= s_vs_n || s_vs[k].rec > cur_rec) return 0;
    if (s_vs[k].ra != 0 && s_vs[k].ra != ra) {
        if (s_log) fprintf(s_log, "registration ra=0x%08X: the reference's next is from 0x%08X (record %u)\n", ra, s_vs[k].ra, s_vs[k].rec);
        return 0;
    }
    /* The row is consumed in place (rows before it stay for their own
     * consumers): its mode becomes a no-op marker. */
    s_vs[k].mode = -3;
    s_reg_matched++;
    if (s_log) fprintf(s_log, "registration ra=0x%08X rec=%u vbl=%u pos=%u\n", ra, s_vs[k].rec, s_vs[k].entry_vbl, s_pos);
    if (s_vs[k].entry_vbl > s_pos) { s_reg_charged += s_vs[k].entry_vbl - s_pos; *target_out = s_vs[k].entry_vbl; return 1; }
    return 0;
}

int32_t openpete_sched_counter(void) {
    if (!s_active || !s_pos_set || !s_counter_k_set) return -1;
    const int32_t counter = (int32_t)s_pos - s_counter_k - s_dropped;
    conv_note(0, counter, s_pos);
    return counter;
}

static uint32_t s_query_ra = 0;
/** @brief Write one query line to the schedule log for the first 400 queries and for hits in the OPENPETE_SCHED_LOG_REC range.
 *
 * The range is read once as `lo:hi` anchor hit numbers, hi exclusive. The
 * caller must have opened s_log.
 * @param how Label of the answer source: row, extra or pos.
 * @param counter The counter returned.
 * @param target The VBLANK the position is charged to. */
static void query_log(const char* how, int32_t counter, uint32_t target) {
    extern uint32_t openpete_psxtrace_anchor_hits;
    static long lo = -1, hi = -1;
    if (lo < 0) { const char* w = getenv("OPENPETE_SCHED_LOG_REC"); lo = 0; hi = 0; if (w) sscanf(w, "%ld:%ld", &lo, &hi); }
    if ((s_query_logged < 400) || ((long)openpete_psxtrace_anchor_hits >= lo && (long)openpete_psxtrace_anchor_hits < hi)) {
        s_query_logged++;
        fprintf(s_log, "query %s hit=%u pos=%u counter=%d target=%u ra=0x%08X\n", how, openpete_psxtrace_anchor_hits, s_pos, counter, target, s_query_ra);
    }
}

/** @brief Report whether a query row's counter maps to a VBLANK behind the position.
 * @param counter The row's libetc counter.
 * @return 1 when counter + K + D is below the position. */
static int row_vblank_behind(int32_t counter) {
    const uint32_t vblank = (uint32_t)(counter + s_counter_k + s_dropped);
    conv_note(2, counter, vblank);
    return vblank < s_pos;
}

/* The counter the game's VSync(-1) returns: the reference's answer to the
 * same read of the record, whose VBLANK (counter + K) the position is
 * charged to when it lies ahead (the interrupt landed inside the tick's
 * work before this read). Before the first anchor, inside a load window,
 * or past the record's rows the answer is the position's. -1 when the
 * sidecar carries no counter. */
int32_t openpete_sched_query(uint32_t ra, uint32_t* target_out) {
    if (!s_active || !s_pos_set || !s_counter_k_set) return -1;
    const uint32_t cur_rec = sched_cur_rec();
    int32_t counter = (int32_t)s_pos - s_counter_k - s_dropped;
    conv_note(1, counter, s_pos);
    *target_out = s_pos;
    s_query_ra = ra;
    s_query_tick = 0;
    const int replay = s_vs && !(cur_rec == 0 && sched_boot_own()) && !s_in_load && !s_failed && !(s_stamps && cur_rec >= s_stamps_n);
    if (replay) {
        /* The next query row of this record from the same call site answers
         * the read. Query rows before it are reads openpete never made: an
         * earlier record's, rows behind the position (the reference's reads
         * during a span a read charged at once), the library's own polls
         * (thunked on the host) and a poll loop's extra iterations. A waited
         * row stops the search: the reference waited before its next read
         * from this site. A sidecar without call sites (ra 0) matches any
         * site. */
        uint32_t k = s_vs_next;
        while (k < s_vs_n && s_vs[k].rec <= cur_rec && s_vs[k].mode < 0 &&
               (s_vs[k].mode == -2 || s_vs[k].rec < cur_rec || row_vblank_behind(s_vs[k].counter) ||
                (s_vs[k].ra != 0 && s_vs[k].ra != ra)))
            k++;
        if (k < s_vs_n && s_vs[k].rec == cur_rec && s_vs[k].mode == -1) {
            for (; s_vs_next < k; s_vs_next++) {
                if (s_vs[s_vs_next].mode != -1) { s_vs_left = 0; continue; }
                s_q_unmade += s_vs_left ? s_vs_left : s_vs[s_vs_next].n;
                if (s_vs[s_vs_next].rec == cur_rec) s_q_behind++;
                s_vs_left = 0;
            }
            if (!s_vs_left) s_vs_left = s_vs[k].n;
            counter = s_vs[k].counter;
            s_query_tick = s_vs[k].ret_tick;
            uint32_t target = (uint32_t)(counter + s_counter_k + s_dropped);
            conv_note(3, counter, target);
            if (target > s_pos) {
                s_q_charged += target - s_pos;
                *target_out = target;
                if (cur_rec == s_lq_rec) s_lq_after++;
            }
            s_q_answered++;
            if (--s_vs_left == 0) s_vs_next++;
            if (s_log) query_log("row", counter, *target_out);
            return counter;
        }
        /* The reference waited before its next read, or made no more reads
         * this record: the position's value. */
        s_q_extra++;
    }
    if (s_log) query_log(replay ? "extra" : "pos", counter, *target_out);
    return counter;
}

int openpete_sched_anchor_target(uint32_t record, uint32_t* target_out) {
    if (!s_active || !s_stamps || record >= s_stamps_n || !s_pos_set || s_failed) return 0;
    *target_out = s_stamps[record];
    if (record == 0) {
        /* The boot ran under openpete's own counting; from here the
         * position is the reference's. */
        if (s_log) fprintf(s_log, "anchor 0: boot ended at pos %u, stamp %u (%u boot waits)\n", s_pos, s_stamps[0], s_boot_waits);
        s_boot_offset = (int64_t)s_stamps[0] - (int64_t)s_pos;
        s_pos = s_stamps[0];
        s_dropped = 0;
        return 1;
    }
    if (s_pos > s_stamps[record] && !s_in_load)
        openpete_sched_fail("anchor record %u: openpete at reference VBLANK %u, the reference at %u (ahead by %u)",
                            record, s_pos, s_stamps[record], s_pos - s_stamps[record]);
    else if (s_pos < s_stamps[record])
        s_long_frames++;
    return 1;
}

/** @brief Return the reference VBLANK of its next event at or after a VBLANK: the next unissued read's issue, the first waited VSync or unmatched callback registration at or after it, or the current record's anchor stamp, whichever comes first.
 *
 * The VSync rows are scanned from the first, wherever the row pointer
 * stands (the boot's rows are skipped, not consumed). A registration row
 * (mode -2) bounds like a waited row: the reference's tick reached the
 * registration at that VBLANK, and openpete_sched_registration charges
 * the position to it, so VBLs past it belong to the work after the
 * registration. Queries (mode -1) and matched registrations (mode -3) do
 * not bound.
 * @param from The reference VBLANK the search starts at.
 * @return The VBLANK, or UINT32_MAX when no event bounds it. */
static uint32_t sched_next_event(uint32_t from) {
    uint32_t bound = 0xFFFFFFFFu;
    if (s_rd && s_rd_next < s_rd_n) bound = s_rd[s_rd_next].issue_vbl;
    for (uint32_t i = 0; s_vs && i < s_vs_n; i++) {
        if ((s_vs[i].mode < 0 && s_vs[i].mode != -2) || s_vs[i].entry_vbl < from) continue;
        if (s_vs[i].entry_vbl < bound) bound = s_vs[i].entry_vbl;
        break;
    }
    const uint32_t rec = sched_cur_rec();
    if (s_stamps && rec < s_stamps_n && s_stamps[rec] < bound) bound = s_stamps[rec];
    return bound;
}

int openpete_sched_spin_due(void) {
    /* Inside a load window the records do not correspond; before the
     * position is set or after a failure no row bounds the charge. */
    if (!s_active || s_failed || !s_pos_set || s_in_load) return 1;
    return s_pos < sched_next_event(s_pos);
}

/** @brief Account a CdRead the `.loads` sidecar has no row left for.
 *
 * Past the trace's last record the capture stopped before the reference
 * issued the read: it is counted and runs under openpete's own timing.
 * Inside the records the reference made no such read, a schedule failure.
 * @param sectors The read's sector count.
 * @param dst The read's destination address. */
static void sched_read_beyond(int sectors, uint32_t dst) {
    const uint32_t rec = sched_cur_rec();
    if (s_stamps && rec >= s_stamps_n) {
        s_rd_past_end++;
        if (s_log) fprintf(s_log, "read past the trace end rec=%u sectors=%d dst=0x%08X\n", rec, sectors, dst);
        return;
    }
    openpete_sched_fail("CdRead %u beyond the sidecar's %u reads (%d sectors to 0x%08X)", s_rd_next, s_rd_n, sectors, dst);
}

int openpete_sched_read(int sectors, uint32_t dst, uint32_t* issue_vbl_out, uint32_t* done_vbl_out, uint32_t* done_rec_out, int* sync_out) {
    if (!s_active || !s_rd || s_failed) return 0;
    if (s_rd_next >= s_rd_n) {
        sched_read_beyond(sectors, dst);
        return 0;
    }
    const sched_read_t* r = &s_rd[s_rd_next++];
    s_read_done_tick = r->done ? r->done_tick : 0u;
    if (s_log) {
        extern uint32_t openpete_psxtrace_anchor_hits;
        fprintf(s_log, "read %u ref_rec=%u op_hit=%u issue=%u pos=%u sectors=%u\n", r->idx, r->issue_rec,
                openpete_psxtrace_anchor_hits, r->issue_vbl, s_pos, r->sectors);
    }
    if (r->sectors != (uint32_t)sectors || r->dst != dst) {
        openpete_sched_fail("read %u: sidecar %u sectors to 0x%08X, openpete %d sectors to 0x%08X",
                            r->idx, r->sectors, r->dst, sectors, dst);
        return 0;
    }
    pos_init_from(r->issue_vbl);
    *issue_vbl_out = r->issue_vbl;
    /* A read the game spun on ends by the reference's next event, and the
     * VBLs between a completion and it are the tick's own work after the
     * read (a loader's parse), spent before the game samples its counters
     * again. */
    const uint32_t bound = sched_next_event(r->issue_vbl);
    if (r->done) {
        /* The game waited on VSync while this read was in flight when a
         * waited row lies inside it: then the waits carry the VBLs and the
         * completion is deferred to its VBLANK. A read the game spun on
         * without waiting is charged inside the read (a deferral would
         * never complete), through to the next event. */
        int waited_inside = 0;
        for (uint32_t i = 0; s_vs && i < s_vs_n; i++) {
            if (s_vs[i].mode < 0) continue;
            if (s_vs[i].entry_vbl >= r->done_vbl) break;
            if (s_vs[i].entry_vbl >= r->issue_vbl) { waited_inside = 1; break; }
        }
        *sync_out = !waited_inside;
        *done_vbl_out = (*sync_out && bound != 0xFFFFFFFFu && bound > r->done_vbl) ? bound : r->done_vbl;
        *done_rec_out = r->done_rec;
    } else {
        /* No completion callback on this game: the read ended by the next event. */
        *done_vbl_out = (bound != 0xFFFFFFFFu) ? bound : r->issue_vbl;
        *done_rec_out = r->issue_rec;
        *sync_out = 1;
    }
    if (*sync_out) s_reads_sync++; else s_reads_async++;
    return 1;
}

int openpete_sched_rcnt2_read(uint16_t* value_out) {
    if (!s_active || !s_rc2) return 0;
    const uint32_t rec = sched_cur_rec();
    while (s_rc2_next < s_rc2_n && s_rc2[s_rc2_next].rec < rec) { s_rc2_next++; s_rc2_skipped++; }
    if (s_rc2_next >= s_rc2_n || s_rc2[s_rc2_next].rec != rec) { s_rc2_unmade++; return 0; }
    *value_out = s_rc2[s_rc2_next++].value;
    s_rc2_answered++;
    return 1;
}

int openpete_sched_next_srand(uint32_t* seed_out) {
    if (!s_active || !s_seed) return 0;
    if (s_seed_next >= s_seed_n) {
        openpete_sched_fail("srand call %u beyond the sidecar's %u seeds", s_seed_next, s_seed_n);
        return 0;
    }
    *seed_out = s_seed[s_seed_next++];
    return 1;
}

/* The clock replay (OPENPETE_PSXTRACE_CLOCK_REPLAY=1). `.vblpos` holds
 * every VBLANK interrupt the reference took outside a waited VSync, with
 * the first game-code function entered after its handler (t), that entry's
 * ordinal since the record's anchor (k) and the record it fell in (t_rec),
 * and, when the game loaded a word its VSync callback stored before
 * reaching t, that word (rt) and the load's ordinal among the record's
 * loads of the word (rk): the row is due at whichever comes first;
 * `.spuq` holds every libspu call that reads or moves envelope state with
 * the exact clock. Gate hits are counted per target, so a guest call a
 * presentation flag adds or removes elsewhere does not move a delivery. */
typedef struct { uint32_t rec, vbl, t, t_rec, k, insns, rt, rk; uint8_t has_rt; } sched_vblpos_t;
typedef struct { uint32_t rec, ra, a0, a1, idx, reg_first, reg_n; uint64_t tick; uint8_t kind, used; } sched_spuq_t;
/* A `.spureg` row: one SPU register access inside the libspu call of `.spuq`
 * row `spuq`; unit is the KON half (0 low, 1 high) for a key-on store and
 * the voice for a sample-rate store or an envelope-level load. */
typedef struct { uint32_t spuq; uint64_t tick; uint8_t kind, unit, used; } sched_spureg_t;
#define VBLPOS_TARGETS 32u

static int      s_clock = 0;           /* the knob was set and both sidecars loaded */
static int      s_clock_reseed = 1;    /* the SPU clock seeds from the next charged clock */
static uint64_t s_clock_seed_tick = 0;
static sched_vblpos_t* s_vp = NULL;  static uint32_t s_vp_n = 0, s_vp_next = 0;
static uint32_t s_vp_delivered = 0, s_vp_covered = 0, s_vp_late = 0, s_vp_skipped = 0;
static uint32_t s_vp_rec = 0xFFFFFFFFu;              /* record the target counters belong to */
static uint32_t s_vp_tgt[VBLPOS_TARGETS], s_vp_cnt[VBLPOS_TARGETS], s_vp_ntgt = 0;
static uint8_t  s_vp_tread[VBLPOS_TARGETS];          /* the target is a RAM word (a row's rt) */
/* Every RAM word a row's rt names, one bit per word: the read gate's
 * filter. */
static uint8_t  s_vp_read_bits[0x200000u >> 5];
static uint32_t s_vp_read_rows = 0, s_vp_read_counted = 0, s_vp_read_delivered = 0;
static sched_spuq_t* s_sq = NULL;    static uint32_t s_sq_n = 0, s_sq_next = 0;
static uint32_t s_sq_charged = 0, s_sq_unmatched = 0, s_sq_unmade = 0;
static uint32_t s_sq_last = 0xFFFFFFFFu;  /* s_sq index of the last matched call, or none */
static sched_spureg_t* s_sr = NULL;  static uint32_t s_sr_n = 0;
static uint32_t s_sr_charged = 0, s_sr_unlinked = 0;

int openpete_sched_clock_replay(void) { return s_clock; }
uint64_t openpete_sched_wait_tick(void) { return s_wait_tick; }
uint64_t openpete_sched_query_tick(void) { return s_query_tick; }
int openpete_sched_clock_reseed_take(void) { int r = s_clock_reseed; s_clock_reseed = 0; return r; }
void openpete_sched_clock_seeded(uint64_t tick) { if (!s_clock_seed_tick) s_clock_seed_tick = tick; }

/** @brief Load the `.vblpos` sidecar into s_vp, one row per VBLANK interrupt taken outside a waited VSync.
 *
 * A row whose target column is `-` (the reference reached no game-code
 * entry before the trace ended) keeps target 0, which no gate hit
 * matches, so it is consumed late. A row with rt and rk columns
 * (recorder version 2) also targets that load; the word's bit is set in
 * s_vp_read_bits. A version 1 sidecar has neither column.
 * @param trace Path of the golden trace.
 * @return 1 when at least one row loaded, else 0. */
static int load_vblpos(const char* trace) {
    char* buf = read_sidecar(trace, ".vblpos", "psxtrace-vblpos");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_vp = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_vp), 4096u);
    if (!s_vp) return sidecar_oom(buf, NULL, ".vblpos", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl;
        char via[16], epc[16], entry[16], insns[16], t[16], t_rec[16], k[16], tick[24], rt[16] = "-", rk[16] = "-";
        if (sscanf(line, "%u %u %u %15s %15s %15s %15s %15s %15s %15s %23s %15s %15s", &idx, &rec, &vbl, via, epc,
                   entry, insns, t, t_rec, k, tick, rt, rk) < 10)
            continue;
        sched_vblpos_t* grown = openpete_rows_reserve(s_vp, &cap, s_vp_n + 1ull, sizeof(*s_vp), 4096u);
        if (!grown) {
            sidecar_oom(buf, s_vp, ".vblpos", s_vp_n);
            s_vp = NULL; s_vp_n = 0; s_vp_read_rows = 0; memset(s_vp_read_bits, 0, sizeof(s_vp_read_bits));
            return 0;
        }
        s_vp = grown;
        sched_vblpos_t* r = &s_vp[s_vp_n++];
        r->rec = rec; r->vbl = vbl;
        r->t = (uint32_t)strtoul(t, NULL, 16);
        r->t_rec = (t_rec[0] == '-') ? rec : (uint32_t)strtoul(t_rec, NULL, 10);
        r->k = (uint32_t)strtoul(k, NULL, 10);
        r->insns = (uint32_t)strtoul(insns, NULL, 10);
        r->has_rt = (uint8_t)(rt[0] == '0');
        r->rt = r->has_rt ? (uint32_t)strtoul(rt, NULL, 16) & 0x1FFFFCu : 0u;  /* offset in main RAM */
        r->rk = r->has_rt ? (uint32_t)strtoul(rk, NULL, 10) : 0u;
        if (r->has_rt) {
            const uint32_t w = r->rt >> 2;
            s_vp_read_bits[w >> 3] |= (uint8_t)(1u << (w & 7u));
            s_vp_read_rows++;
        }
    }
    free(buf);
    return s_vp_n != 0;
}

/** @brief Return the `.spuq` kind index of a kind column, -1 for an unknown kind.
 * @param name The kind column.
 * @return 0 setkey, 1 keystatus, 2 allkeys, 3 voiceattr, or -1. */
static int spuq_kind(const char* name) {
    static const char* const kinds[4] = { "setkey", "keystatus", "allkeys", "voiceattr" };
    for (int i = 0; i < 4; i++)
        if (strcmp(name, kinds[i]) == 0) return i;
    return -1;
}

/** @brief Load the `.spuq` sidecar into s_sq, one row per recorded libspu call with its first two arguments.
 *
 * The arguments kept are those the call is matched on: SpuSetKey's on
 * flag and voices, SpuGetKeyStatus's voice, SpuGetAllKeysStatus's buffer,
 * SpuSetVoiceAttr's voice and mask.
 * @param trace Path of the golden trace.
 * @return 1 when at least one row loaded, else 0. */
static int load_spuq(const char* trace) {
    char* buf = read_sidecar(trace, ".spuq", "psxtrace-spuq");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_sq = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_sq), 4096u);
    if (!s_sq) return sidecar_oom(buf, NULL, ".spuq", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, pc, ra; unsigned long long tick; char kind[16], a0[24] = "0", a1[24] = "0";
        if (sscanf(line, "%u %u %u %15s %x %x %llu %23s %23s", &idx, &rec, &vbl, kind, &pc, &ra, &tick, a0, a1) < 8)
            continue;
        const int kd = spuq_kind(kind);
        if (kd < 0) continue;
        sched_spuq_t* grown = openpete_rows_reserve(s_sq, &cap, s_sq_n + 1ull, sizeof(*s_sq), 4096u);
        if (!grown) { sidecar_oom(buf, s_sq, ".spuq", s_sq_n); s_sq = NULL; s_sq_n = 0; return 0; }
        s_sq = grown;
        sched_spuq_t* r = &s_sq[s_sq_n++];
        r->rec = rec; r->ra = ra; r->tick = tick; r->kind = (uint8_t)kd; r->used = 0;
        r->idx = idx; r->reg_first = 0; r->reg_n = 0;
        r->a0 = (uint32_t)strtoul(a0, NULL, 0);
        r->a1 = (kd == 0 || kd == 3) ? (uint32_t)strtoul(a1, NULL, 0) : 0u;
    }
    free(buf);
    return s_sq_n != 0;
}

/** @brief Return the `.spureg` kind index of a kind column, -1 for an unknown kind.
 * @param name The kind column.
 * @return 0 kon, 1 pitch, 2 envx, or -1. */
static int spureg_kind(const char* name) {
    static const char* const kinds[3] = { "kon", "pitch", "envx" };
    for (int i = 0; i < 3; i++)
        if (strcmp(name, kinds[i]) == 0) return i;
    return -1;
}

/** @brief Link each `.spureg` row to its `.spuq` row: the call's reg_first and reg_n index its accesses in s_sr.
 *
 * Both sidecars are in capture order, so one forward pass pairs them; a
 * register row naming no loaded `.spuq` row is counted unlinked. */
static void spureg_link(void) {
    uint32_t q = 0;
    for (uint32_t i = 0; i < s_sr_n; i++) {
        while (q < s_sq_n && s_sq[q].idx < s_sr[i].spuq) q++;
        if (q == s_sq_n || s_sq[q].idx != s_sr[i].spuq) { s_sr_unlinked++; continue; }
        if (s_sq[q].reg_n == 0) s_sq[q].reg_first = i;
        s_sq[q].reg_n++;
    }
}

/** @brief Load the `.spureg` sidecar into s_sr, one row per SPU register access the reference made inside a recorded libspu call.
 *
 * Rows outside any call (spuq column -1) and rows of an unknown kind are
 * skipped. The unit is derived from the access's address: the KON half for
 * 0x1F801D88/0x1F801D8A, else the voice ((addr - 0x1F801C00) / 16).
 * @param trace Path of the golden trace.
 * @return 1 when at least one row loaded, else 0. */
static int load_spureg(const char* trace) {
    char* buf = read_sidecar(trace, ".spureg", "psxtrace-spureg");
    if (!buf) return 0;
    uint32_t cap = 0;
    s_sr = openpete_rows_reserve(NULL, &cap, 1u, sizeof(*s_sr), 4096u);
    if (!s_sr) return sidecar_oom(buf, NULL, ".spureg", 0);
    for (char* line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || line[0] == 'p') continue;
        unsigned idx, rec, vbl, pc, addr; int spuq; unsigned long long tick; char kind[16], value[16];
        if (sscanf(line, "%u %u %u %d %15s %x %x %15s %llu", &idx, &rec, &vbl, &spuq, kind, &pc, &addr, value, &tick) != 9)
            continue;
        const int kd = spureg_kind(kind);
        if (kd < 0 || spuq < 0) continue;
        const uint32_t a = addr & 0x1FFFFFFFu;
        sched_spureg_t* grown = openpete_rows_reserve(s_sr, &cap, s_sr_n + 1ull, sizeof(*s_sr), 4096u);
        if (!grown) { sidecar_oom(buf, s_sr, ".spureg", s_sr_n); s_sr = NULL; s_sr_n = 0; return 0; }
        s_sr = grown;
        sched_spureg_t* r = &s_sr[s_sr_n++];
        r->spuq = (uint32_t)spuq; r->tick = tick; r->kind = (uint8_t)kd; r->used = 0;
        r->unit = (uint8_t)(kd == 0 ? (a & 2u) >> 1 : ((a - 0x1F801C00u) >> 4) & 0x1Fu);
    }
    free(buf);
    spureg_link();
    return s_sr_n != 0;
}

/** @brief Turn the clock replay on when OPENPETE_PSXTRACE_CLOCK_REPLAY is set to a value not starting with '0' and both sidecars load.
 *
 * Read once, at openpete_sched_open(). With the knob set and a sidecar
 * missing the replay stays off and an error is logged.
 * @param trace Path of the golden trace. */
static void clock_open(const char* trace) {
    const char* e = getenv("OPENPETE_PSXTRACE_CLOCK_REPLAY");
    if (!e || !*e || *e == '0') return;
    const int vp = load_vblpos(trace), sq = load_spuq(trace);
    s_clock = vp && sq;
    if (!s_clock) {
        OPL_ERROR_F("psxtrace", "clock replay: %s%s missing; the replay is off", vp ? "" : ".vblpos ", sq ? "" : ".spuq");
        return;
    }
    if (!load_spureg(trace))
        OPL_WARN_F("psxtrace", "clock replay: no .spureg rows; the SPU clock charges at libspu entries only");
    OPL_INFO_F("psxtrace", "clock replay: %u in-work VBLANK rows (%u at a load), %u libspu rows, %u SPU register rows (%u unlinked)",
               s_vp_n, s_vp_read_rows, s_sq_n, s_sr_n, s_sr_unlinked);
}

/** @brief Skip the rows of the records a load window's end jumps over and re-seed the SPU clock.
 * @param rec The reference record the window ended at. */
static void clock_skip_to(uint32_t rec) {
    s_clock_reseed = 1;
    while (s_vp && s_vp_next < s_vp_n && s_vp[s_vp_next].t_rec <= rec) { s_vp_next++; s_vp_skipped++; }
    while (s_sq && s_sq_next < s_sq_n && s_sq[s_sq_next].rec <= rec) s_sq_next++;
}

/** @brief Add target t (an entry, or a load when read is set) to the record's counted targets at 0, unless present or the table is full.
 * @param t The function's entry address, or the word's offset in main RAM.
 * @param read 1 for a load, 0 for an entry. */
static void vblpos_add_target(uint32_t t, uint8_t read) {
    uint32_t j = 0;
    while (j < s_vp_ntgt && (s_vp_tgt[j] != t || s_vp_tread[j] != read)) j++;
    if (j < s_vp_ntgt || s_vp_ntgt == VBLPOS_TARGETS) return;
    s_vp_tgt[j] = t; s_vp_tread[j] = read; s_vp_cnt[j] = 0; s_vp_ntgt++;
}

/** @brief Start counting the entries of rec's `.vblpos` targets: the distinct targets of its pending rows, each at 0.
 *
 * Rows of earlier records still pending are counted skipped (the anchor
 * consumes them first, so these are rows of records openpete never ran).
 * @param rec The current record. */
static void vblpos_begin_record(uint32_t rec) {
    s_vp_rec = rec;
    s_vp_ntgt = 0;
    while (s_vp_next < s_vp_n && s_vp[s_vp_next].t_rec < rec) { s_vp_next++; s_vp_skipped++; }
    for (uint32_t i = s_vp_next; i < s_vp_n && s_vp[i].t_rec == rec; i++) {
        vblpos_add_target(s_vp[i].t, 0);
        if (s_vp[i].has_rt) vblpos_add_target(s_vp[i].rt, 1);
    }
}

/** @brief Return 1 when `.vblpos` row r is due at the k-th hit of target t (an entry, or a load when read is set).
 * @param r The row.
 * @param t The function's entry address, or the word's offset in main RAM.
 * @param read 1 for a load, 0 for an entry.
 * @param k The hit's ordinal in the record.
 * @return 1 when the row names that hit, else 0. */
static int vblpos_row_at(const sched_vblpos_t* r, uint32_t t, uint8_t read, uint32_t k) {
    return read ? (r->has_rt && r->rt == t && r->rk == k) : (r->t == t && r->k == k);
}

/** @brief Count one hit of target t (a function entry, or a load of a RAM word when read is set) and return the in-work VBLANK due at it.
 * @param t The function's entry address, or the word's offset in main RAM.
 * @param read 1 for a load, 0 for an entry.
 * @param target_out The reference VBLANK to charge to.
 * @return 1 with target_out set when a VBLANK is due here, else 0. */
static int vblpos_hit(uint32_t t, uint8_t read, uint32_t* target_out) {
    if (!s_clock || s_failed || s_in_load) return 0;
    const uint32_t rec = sched_cur_rec();
    if (rec != s_vp_rec) vblpos_begin_record(rec);
    uint32_t j = 0;
    while (j < s_vp_ntgt && (s_vp_tgt[j] != t || s_vp_tread[j] != read)) j++;
    if (j == s_vp_ntgt) return 0;
    const uint32_t k = ++s_vp_cnt[j];
    s_vp_read_counted += read;
    uint32_t target = 0, n = 0;
    while (s_vp_next < s_vp_n && s_vp[s_vp_next].t_rec == rec && vblpos_row_at(&s_vp[s_vp_next], t, read, k)) {
        target = s_vp[s_vp_next++].vbl;
        n++;
    }
    if (!n) return 0;
    if (s_log) fprintf(s_log, "vblpos rec=%u t=0x%08X%s k=%u vbl=%u pos=%u rows=%u\n", rec, t, read ? " read" : "", k,
                       target, s_pos, n);
    if (target <= s_pos) { s_vp_covered += n; return 0; }
    s_vp_delivered += n;
    if (read) s_vp_read_delivered += n;
    if (rec == s_lq_rec) s_lq_after += n;
    *target_out = target;
    return 1;
}

int openpete_sched_vblpos_gate(uint32_t addr, uint32_t* target_out) {
    return vblpos_hit(addr, 0, target_out);
}

int openpete_sched_vblpos_read_words(void) {
    return s_clock && s_vp_read_rows != 0;
}

int openpete_sched_vblpos_read_gate(uint32_t addr, uint32_t* target_out) {
    const uint32_t m = addr & 0x1FFFFFFFu;
    if (m >= 0x800000u) return 0;
    const uint32_t w = (m & 0x1FFFFFu) >> 2;
    if (!(s_vp_read_bits[w >> 3] & (1u << (w & 7u)))) return 0;
    return vblpos_hit(w << 2, 1, target_out);
}

uint32_t openpete_sched_vblpos_late(uint32_t bound) {
    if (!s_clock || s_failed || s_in_load) return 0;
    const uint32_t rec = sched_cur_rec();
    uint32_t target = 0;
    while (s_vp_next < s_vp_n && (bound ? s_vp[s_vp_next].vbl <= bound : s_vp[s_vp_next].t_rec < rec)) {
        const uint32_t v = s_vp[s_vp_next++].vbl;
        if (v <= s_pos) { s_vp_covered++; continue; }
        s_vp_late++;
        if (rec == s_lq_rec) s_lq_after++;
        if (v > target) target = v;
        if (s_log) fprintf(s_log, "vblpos-late rec=%u vbl=%u pos=%u at=%s\n", rec, v, s_pos, bound ? "wait" : "anchor");
    }
    return target;
}

/** @brief The bound the default row puts on in-work VBLANKs: the VBLANK before the record's next waited row's return, or a query row's answer.
 *
 * Peeks the current record's `.vsync` rows from the cursor without
 * consuming them: a waited row bounds at its return VBLANK minus one (the
 * waited VBLANK stays with the wait), a query row at its answer mapped to
 * a VBLANK (counter + K + D) when that is ahead of the position; query rows
 * at or behind the position and registration rows are passed over.
 * @param rec The current record.
 * @return The bound, 0 when the record has no such row. */
static uint32_t vsync_bound(uint32_t rec) {
    for (uint32_t w = s_vs_next; w < s_vs_n && s_vs[w].rec <= rec; w++) {
        const sched_vsync_t* r = &s_vs[w];
        if (r->rec < rec) continue;
        if (r->mode >= 0) return r->ret_vbl ? r->ret_vbl - 1u : 0u;
        if (r->mode != -1 || !s_counter_k_set) continue;
        const uint32_t v = (uint32_t)(r->counter + s_counter_k + s_dropped);
        if (v > s_pos) return v;
    }
    return 0u;
}

int openpete_sched_loop_due(uint64_t op_insns, uint32_t* target_out) {
    extern uint32_t openpete_psxtrace_anchor_hits;
    extern uint32_t openpete_psxtrace_anchor_hit_target;
    if (!s_active || !s_pos_set || s_failed || s_in_load ||
        openpete_psxtrace_anchor_hits < openpete_psxtrace_anchor_hit_target) {
        s_lq_norow++;
        return 0;
    }
    const uint32_t rec = sched_cur_rec();
    s_lq_rec = rec;
    if (s_stamps && rec >= s_stamps_n) { s_lq_norow++; return 0; }
    if (s_clock) {
        /* The record's next pending `.vblpos` row, whatever its target: a
         * call-free spin reaches no entry, and the row's target is the
         * entry after the spin exits, whose gate hit then finds it
         * consumed. */
        if (rec != s_vp_rec) vblpos_begin_record(rec);
        while (s_vp_next < s_vp_n && s_vp[s_vp_next].t_rec == rec) {
            const sched_vblpos_t* r = &s_vp[s_vp_next++];
            if (r->vbl <= s_pos) { s_vp_covered++; continue; }
            s_vp_delivered++;
            s_lq_delivered++;
            s_lq_rows++;
            s_lq_ref_insns += r->insns;
            s_lq_op_insns += op_insns;
            if (s_log) fprintf(s_log, "loop-vblpos rec=%u vbl=%u pos=%u ref_insns=%u op_insns=%llu\n",
                               rec, r->vbl, s_pos, r->insns, (unsigned long long)op_insns);
            *target_out = r->vbl;
            return 1;
        }
        s_lq_norow++;
        return 0;
    }
    const uint32_t bound = vsync_bound(rec);
    if (bound < s_pos + 1u) { s_lq_norow++; return 0; }
    s_lq_delivered++;
    if (s_log) fprintf(s_log, "loop-vsync rec=%u bound=%u pos=%u\n", rec, bound, s_pos);
    *target_out = s_pos + 1u;
    return 1;
}

/** @brief Log the polling-loop quanta the schedule saw: delivered, refused, rows consumed after a quantum, and the clock row's instruction counts. */
static void loop_summary(void) {
    OPL_INFO_F("psxtrace", "loop: quanta delivered=%u no row due=%u; rows consumed after a quantum in its record=%u; "
               "clock rows delivered=%u ref insns=%llu loop insns=%llu",
               s_lq_delivered, s_lq_norow, s_lq_after, s_lq_rows,
               (unsigned long long)s_lq_ref_insns, (unsigned long long)s_lq_op_insns);
}

/** @brief Return 1 when `.spuq` row r is unconsumed and is the call of the given kind, site and arguments.
 * @param r The row.
 * @param kind The call's kind index.
 * @param ra The call's return address.
 * @param a0 The call's first matched argument.
 * @param a1 The call's second matched argument.
 * @return 1 on a match, else 0. */
static int spuq_row_is(const sched_spuq_t* r, int kind, uint32_t ra, uint32_t a0, uint32_t a1) {
    return !r->used && r->kind == (uint8_t)kind && r->ra == ra && r->a0 == a0 && r->a1 == a1;
}

int openpete_sched_spuq(int kind, uint32_t ra, uint32_t a0, uint32_t a1, uint64_t* tick_out) {
    s_sq_last = 0xFFFFFFFFu;
    if (!s_clock || s_failed || s_in_load) return 0;
    const uint32_t rec = sched_cur_rec();
    for (; s_sq_next < s_sq_n && (s_sq[s_sq_next].used || s_sq[s_sq_next].rec < rec); s_sq_next++)
        s_sq_unmade += !s_sq[s_sq_next].used;
    uint32_t i = s_sq_next;
    while (i < s_sq_n && s_sq[i].rec == rec && !spuq_row_is(&s_sq[i], kind, ra, a0, a1)) i++;
    if (i >= s_sq_n || s_sq[i].rec != rec) {
        s_sq_unmatched++;
        if (s_log) fprintf(s_log, "spuq-unmatched rec=%u kind=%d ra=0x%08X a0=0x%X a1=0x%X pos=%u\n", rec, kind, ra, a0, a1, s_pos);
        return 0;
    }
    if (s_log) fprintf(s_log, "spuq rec=%u kind=%d ra=0x%08X a0=0x%X a1=0x%X tick=%llu pos=%u\n", rec, kind, ra, a0, a1,
                       (unsigned long long)s_sq[i].tick, s_pos);
    s_sq[i].used = 1;
    s_sq_charged++;
    s_sq_last = i;
    *tick_out = s_sq[i].tick;
    return 1;
}

int openpete_sched_spureg(int kind, int unit, uint64_t* tick_out) {
    if (s_sq_last >= s_sq_n) return 0;
    const sched_spuq_t* q = &s_sq[s_sq_last];
    for (uint32_t i = q->reg_first; i < q->reg_first + q->reg_n; i++) {
        sched_spureg_t* r = &s_sr[i];
        if (r->used || r->kind != (uint8_t)kind || r->unit != (uint8_t)unit) continue;
        r->used = 1;
        s_sr_charged++;
        if (s_log) fprintf(s_log, "spureg kind=%d unit=%d tick=%llu pos=%u\n", kind, unit, (unsigned long long)r->tick, s_pos);
        *tick_out = r->tick;
        return 1;
    }
    return 0;
}

/** @brief Log the clock replay's engagement counters, and the `.spuq` key-on rows before the SPU clock's seed. */
static void clock_summary(void) {
    if (!s_clock) return;
    uint32_t keyons_before = 0;
    for (uint32_t i = 0; i < s_sq_n && s_sq[i].tick < s_clock_seed_tick; i++)
        keyons_before += (s_sq[i].kind == 0 && s_sq[i].a0 == 1u);
    OPL_INFO_F("psxtrace", "clock: vblpos delivered=%u covered=%u late=%u skipped=%u pending=%u of %u; "
               "spuq charged=%u unmatched=%u unmade=%u of %u; key-on rows before the SPU clock's seed=%u (seed tick %llu); "
               "spureg charged=%u of %u; vblpos read rows=%u loads counted=%u delivered=%u",
               s_vp_delivered, s_vp_covered, s_vp_late, s_vp_skipped, s_vp_n - s_vp_next, s_vp_n,
               s_sq_charged, s_sq_unmatched, s_sq_unmade, s_sq_n, keyons_before, (unsigned long long)s_clock_seed_tick,
               s_sr_charged, s_sr_n, s_vp_read_rows, s_vp_read_counted, s_vp_read_delivered);
}

void openpete_sched_summary(void) {
    if (!s_active) return;
    OPL_INFO_F("psxtrace", "schedule: consumed %u/%u waited VSync rows, %u/%u reads (%u sync, %u async), %u/%u seeds, "
               "%u VSync call(s) and %u read(s) past the trace end, "
               "queries: %llu/%llu answered from the reference's rows, %llu past the record's rows, %llu of the reference's unmade, "
               "%u VBL(s) charged at a query, %u rows behind the position dropped; "
               "registrations: %u/%u matched, %u VBL(s) charged; "
               "%u records reached behind the reference's stamp (long frames, charged at the anchor), "
               "boot: %u waits under openpete's count, %lld VBL(s) from the first stamp, "
               "%u load windows (%u/%u on the reference), %u reference records skipped at their ends; "
               "controller handler: %u vblanks served, %u skipped, commands on %u, none on %u; "
               "CdSync polls: %u answered from the reference's rows, %u of the reference's unmade, %u VBL(s) charged%s%s",
               s_vsync_consumed, s_vs_waits, s_rd_next, s_rd_n, s_reads_sync, s_reads_async,
               s_seed_next, s_seed_n, s_vs_past_end, s_rd_past_end,
               (unsigned long long)s_q_answered, (unsigned long long)s_q_total, (unsigned long long)s_q_extra,
               (unsigned long long)s_q_unmade, s_q_charged, s_q_behind,
               s_reg_matched, s_reg_total, s_reg_charged,
               s_long_frames, s_boot_waits, (long long)s_boot_offset,
               s_windows_seen, s_win_next, s_win_n, s_records_skipped,
               s_sio0.served, s_sio0.skipped, s_padcmd.served, s_padcmd.skipped,
               s_cs_answered, s_cs_unmade, s_cs_charged,
               s_failed ? "; FAILED: " : "", s_failed ? s_fail_reason : "");
    if (s_rc2)
        OPL_INFO_F("psxtrace", "schedule: root counter 2: %u/%u game reads answered from the reference, "
                   "%u read(s) with no row in their record, %u row(s) passed unread",
                   s_rc2_answered, s_rc2_n, s_rc2_unmade, s_rc2_skipped + (s_rc2_n - s_rc2_next));
    clock_summary();
    loop_summary();
}
