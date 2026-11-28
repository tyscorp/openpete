/* core/runtime/host_bugknobs.c - the knob registry's values for the
 * bug-report bundle: each row's value at the session's first committed
 * tick (its boot value), every change of a live row after it with the tick
 * it was seen at, and knobs.txt, the member that carries both. See
 * openpete_bugreport.h. */
#include "openpete_bugreport.h"
#include "openpete_thread_roles.h"

#include "openpete.h"
#include "openpete_config.h"
#include "openpete_fiber.h"
#include "openpete_knobs.h"
#include "openpete_log.h"
#include "rhi.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Changes kept: the newest BK_EVENTS; an older one is counted as dropped. */
#define BK_EVENTS 256
/* Harness schedule entries (OPENPETE_BUGREPORT_KNOB_AT). */
#define BK_SCHED_MAX 8

/* One registry row's recorded values. */
typedef struct {
    const char* cls;           /* openpete_bugknobs_class, decided at the first sample */
    char   boot[KNOB_STR_MAX];
    char   cur[KNOB_STR_MAX];
    double boot_raw, cur_raw;
    unsigned changes;          /* changes seen after boot */
    unsigned last_frame;       /* the tick of the newest change */
    double   last_t;           /* its time, seconds since SDL init */
} bk_row_t;

/* One change of a live row, seen at a committed tick. */
typedef struct {
    unsigned frame, key;
    int      vbl;
    double   t_s;
    int      row;
    double   raw, old_raw;
    char     value[KNOB_STR_MAX];
    char     old[KNOB_STR_MAX];
} bk_event_t;

/* One harness schedule entry: set row `id` to `value` once the feed key reaches `key`. */
typedef struct {
    long key;
    char id[64];
    char value[KNOB_STR_MAX];
    int  done;
} bk_sched_t;

static struct {
    int         inited;
    size_t      n;              /* rows recorded (the registry's count at the first sample) */
    bk_row_t*   rows;
    unsigned    first_frame;    /* the tick of the first sample */
    unsigned    env_epoch;      /* the registry env epoch at the last env sample */
    bk_event_t  ev[BK_EVENTS];
    uint64_t    ev_n;           /* changes seen; the newest BK_EVENTS are kept */
    bk_sched_t  sched[BK_SCHED_MAX];
    int         sched_n;
} s_bk;

/** @brief Replace tabs and line breaks in a value with spaces, so one value is one field. */
static void bk_flatten(char* s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

/** @brief Read a row's value: the text in the CLI's vocabulary where the row has one (knob_config_value_str), else its display text, and the number its setter takes.
 *
 * An OPENPETE_* row's getter reads the environment, so it runs inside the
 * read side of the environment guard.
 * @param k The row (not an action).
 * @param buf Receives the text.
 * @param cap Capacity of @p buf.
 * @return The number (the stored value for a row with no getter), or NAN for a string row. */
static double bk_read(const knob_desc_t* k, char* buf, size_t cap)
{
    const int env = openpete_bugknobs_is_env(k);
    if (env) rhi_env_read_guard(1);
    if (knob_config_value_str(k, buf, cap) != 0) knob_value_str(k, buf, cap);
    if (env) rhi_env_read_guard(0);
    bk_flatten(buf);
    if (k->type == KNOB_STRING) return NAN;
    return k->get_num ? k->get_num(k) : k->stored_num;
}

/** @brief Read the harness schedule OPENPETE_BUGREPORT_KNOB_AT: comma-separated KEY:id=value entries. */
static void bk_sched_read(void)
{
    const char* e = getenv("OPENPETE_BUGREPORT_KNOB_AT");
    char list[1024];
    snprintf(list, sizeof list, "%s", e ? e : "");
    for (char* tok = strtok(list, ","); tok && s_bk.sched_n < BK_SCHED_MAX; tok = strtok(NULL, ",")) {
        char* colon = strchr(tok, ':');
        char* eq = colon ? strchr(colon, '=') : NULL;
        if (!eq) {
            OPL_WARN_F("bugreport", "OPENPETE_BUGREPORT_KNOB_AT: \"%s\" is not KEY:id=value; ignored", tok);
            continue;
        }
        *colon = *eq = '\0';
        bk_sched_t* s = &s_bk.sched[s_bk.sched_n++];
        s->key = strtol(tok, NULL, 10);
        snprintf(s->id, sizeof s->id, "%s", colon + 1);
        snprintf(s->value, sizeof s->value, "%s", eq + 1);
    }
}

/** @brief Take the first sample: run knob_registry_init when no window ran it, then record every row's value as its boot value.
 * @param opts The run options. */
static void bk_init(const openpete_options_t* opts)
{
    if (!knob_registry_inited()) knob_registry_init(opts);
    size_t n = 0;
    knob_desc_t* ks = knob_registry(&n);
    s_bk.rows = (bk_row_t*)calloc(n ? n : 1, sizeof *s_bk.rows);
    if (!s_bk.rows) return;
    s_bk.n = n;
    for (size_t i = 0; i < n; i++) {
        bk_row_t* r = &s_bk.rows[i];
        r->cls = openpete_bugknobs_class(&ks[i]);
        if (ks[i].type == KNOB_ACTION) continue;
        r->boot_raw = r->cur_raw = bk_read(&ks[i], r->boot, sizeof r->boot);
        memcpy(r->cur, r->boot, sizeof r->cur);
    }
    s_bk.first_frame = openpete_frame;
    s_bk.env_epoch = knob_registry_env_epoch();
    bk_sched_read();
    s_bk.inited = 1;
}

/** @brief Report whether a row's value moved since its last sample: the number for a numeric row (NaN equals NaN), the text for a string row.
 * @param k The row.
 * @param r Its record.
 * @param raw Receives the number (NAN for a string row).
 * @param buf Receives the text when it is read.
 * @param cap Capacity of @p buf.
 * @return 1 when it moved; @p buf then holds the new text. */
static int bk_moved(const knob_desc_t* k, const bk_row_t* r, double* raw, char* buf, size_t cap)
{
    if (k->type != KNOB_STRING) {
        const double v = k->get_num ? k->get_num(k) : k->stored_num;
        *raw = v;
        if (v == r->cur_raw || (isnan(v) && isnan(r->cur_raw))) return 0;
        (void)bk_read(k, buf, cap);
        return 1;
    }
    *raw = bk_read(k, buf, cap);
    return strcmp(buf, r->cur) != 0;
}

/** @brief Record one row's change: an event in the ring and the row's new value.
 * @param i The row index.
 * @param raw The new number.
 * @param val The new text. */
static void bk_record(size_t i, double raw, const char* val)
{
    bk_row_t* r = &s_bk.rows[i];
    bk_event_t* e = &s_bk.ev[s_bk.ev_n % BK_EVENTS];
    const double t = (double)SDL_GetTicksNS() / 1e9;   /* clock-guard: wall: a report's timestamp */
    *e = (bk_event_t){.frame = openpete_frame, .key = openpete_feed_key_now(),
                      .vbl = SDL_GetAtomicInt(&openpete_vbl), .t_s = t, .row = (int)i,
                      .raw = raw, .old_raw = r->cur_raw};
    snprintf(e->value, sizeof e->value, "%s", val);
    snprintf(e->old, sizeof e->old, "%s", r->cur);
    s_bk.ev_n++;
    snprintf(r->cur, sizeof r->cur, "%s", val);
    r->cur_raw = raw;
    r->changes++;
    r->last_frame = openpete_frame;
    r->last_t = t;
}

/** @brief Sample the live rows: every numeric and string live row but a `follow` one, and the OPENPETE_* rows when the registry env epoch moved. */
static void bk_sample(void)
{
    size_t n = 0;
    const knob_desc_t* ks = knob_registry(&n);
    if (n > s_bk.n) n = s_bk.n;
    const unsigned ep = knob_registry_env_epoch();
    const int env_moved = ep != s_bk.env_epoch;
    s_bk.env_epoch = ep;
    char buf[KNOB_STR_MAX];
    for (size_t i = 0; i < n; i++) {
        const knob_desc_t* k = &ks[i];
        const char* cls = s_bk.rows[i].cls;
        if (k->mut != KNOB_LIVE || k->type == KNOB_ACTION || !strcmp(cls, "follow") ||
            (openpete_bugknobs_is_env(k) && !env_moved))
            continue;
        double raw = 0.0;
        if (bk_moved(k, &s_bk.rows[i], &raw, buf, sizeof buf)) bk_record(i, raw, buf);
    }
}

/** @brief Parse a value word for a numeric row: an enum row's value name, a bool word, else a number.
 * @param k The row.
 * @param v The word.
 * @param out Receives the number.
 * @return 0, or -1 when the word names nothing the row takes. */
static int bk_parse_num(const knob_desc_t* k, const char* v, double* out)
{
    for (int i = 0; k->type == KNOB_ENUM && k->enum_values && i < k->enum_count; i++)
        if (!strcmp(v, k->enum_values[i])) { *out = i; return 0; }
    if (k->type == KNOB_BOOL && (!strcmp(v, "on") || !strcmp(v, "true"))) { *out = 1; return 0; }
    if (k->type == KNOB_BOOL && (!strcmp(v, "off") || !strcmp(v, "false"))) { *out = 0; return 0; }
    char* end = NULL;
    *out = strtod(v, &end);
    return (end != v && *end == '\0') ? 0 : -1;
}

/** @brief Apply the harness schedule entries whose feed key is reached, each once, through the row's setter. */
static void bk_sched_run(void)
{
    const unsigned key = openpete_feed_key_now();
    for (int i = 0; i < s_bk.sched_n; i++) {
        bk_sched_t* s = &s_bk.sched[i];
        if (s->done || (long)key < s->key) continue;
        s->done = 1;
        knob_desc_t* k = knob_registry_find(s->id);
        double v = 0.0;
        int rc = -1;
        if (k && k->type == KNOB_STRING && k->set_str) rc = k->set_str(k, s->value);
        else if (k && k->set_num && bk_parse_num(k, s->value, &v) == 0) rc = k->set_num(k, v);
        if (rc == 0)
            OPL_INFO_F("bugreport", "harness knob %s = %s at feed key %u (frame %u)", s->id, s->value, key,
                       openpete_frame);
        else
            OPL_WARN_F("bugreport", "harness knob %s = %s refused (no live row, or a value it does not take)",
                       s->id, s->value);
    }
}

void openpete_bugknobs_poll(const openpete_options_t* opts) OP_REQUIRES(sim)
{
    size_t n = 0;
    (void)knob_registry(&n);
    if (s_bk.inited && s_bk.rows && n != s_bk.n) {
        /* A row added after the first sample moved the indices: start over. */
        OPL_WARN_F("bugreport", "knob registry grew from %zu to %zu rows after the first sample; "
                   "the knob record restarts at frame %u", s_bk.n, n, openpete_frame);
        free(s_bk.rows);
        s_bk.rows = NULL;
        s_bk.inited = 0;
        s_bk.ev_n = 0;
        s_bk.sched_n = 0;
    }
    if (!s_bk.inited) {
        bk_init(opts);
        return;
    }
    if (!s_bk.rows) return;
    bk_sched_run();
    bk_sample();
}

/* A growable text buffer for knobs.txt. */
typedef struct { char* b; size_t n, cap; int oom; } bk_buf_t;

/** @brief Append formatted text; an allocation failure marks the buffer and drops the rest. */
static void bk_printf(bk_buf_t* t, const char* fmt, ...)
{
    char line[3 * KNOB_STR_MAX + 512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0 || t->oom) return;
    const size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    if (t->n + len + 1 > t->cap) {
        const size_t cap = (t->cap ? t->cap * 2 : 16384) + len;
        char* g = (char*)realloc(t->b, cap);
        if (!g) { t->oom = 1; return; }
        t->b = g;
        t->cap = cap;
    }
    memcpy(t->b + t->n, line, len);
    t->n += len;
    t->b[t->n] = '\0';
}

/** @brief Write a row's flags: L when its value differs from its boot value, D when it differs from its declared default, C when it changed after boot.
 * @param k The row.
 * @param r Its record.
 * @param out Receives the flags, "-" for none (4 bytes). */
static void bk_flags(const knob_desc_t* k, const bk_row_t* r, char out[4])
{
    int n = 0;
    if (strcmp(r->cur, r->boot) != 0) out[n++] = 'L';
    if (k->def_str && *k->def_str && r->cur[0] != '(' && !knob_config_values_equal(k, r->cur, k->def_str))
        out[n++] = 'D';
    if (r->changes) out[n++] = 'C';
    if (!n) out[n++] = '-';
    out[n] = '\0';
}

/** @brief Write a number field: %.17g, or "-" for NaN (a string row). */
static void bk_num(char* out, size_t cap, double v)
{
    if (isnan(v)) snprintf(out, cap, "-");
    else snprintf(out, cap, "%.17g", v);
}

/** @brief Write one `knob` line.
 * @param t The buffer.
 * @param k The row.
 * @param r Its record. */
static void bk_knob_line(bk_buf_t* t, const knob_desc_t* k, const bk_row_t* r)
{
    char flags[4], raw[40], def[KNOB_STR_MAX];
    bk_flags(k, r, flags);
    bk_num(raw, sizeof raw, r->cur_raw);
    if (k->def_str && *k->def_str) snprintf(def, sizeof def, "%s", k->def_str);
    else if (k->default_note) snprintf(def, sizeof def, "(decided: %s)", k->default_note);
    else snprintf(def, sizeof def, "-");
    bk_flatten(def);
    char last[64] = "-";
    if (r->changes) snprintf(last, sizeof last, "%u@%.3f", r->last_frame, r->last_t);
    bk_printf(t, "knob\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%u\t%s\n", k->id, r->cls, r->cur, r->boot, def,
              openpete_config_knob_layer(k->id), flags, raw, r->changes, last);
}

/** @brief Write one `change` line.
 * @param t The buffer.
 * @param ks The registry rows.
 * @param e The change. */
static void bk_change_line(bk_buf_t* t, const knob_desc_t* ks, const bk_event_t* e)
{
    char raw[40], old_raw[40];
    bk_num(raw, sizeof raw, e->raw);
    bk_num(old_raw, sizeof old_raw, e->old_raw);
    bk_printf(t, "change\t%u\t%d\t%u\t%.3f\t%s\t%s\t%s\t%s\t%s\n", e->frame, e->vbl, e->key, e->t_s, ks[e->row].id,
              e->old, e->value, old_raw, raw);
}

char* openpete_bugknobs_text(void)
{
    if (!s_bk.inited || !s_bk.rows) return NULL;
    bk_buf_t t = {0};
    bk_printf(&t, "# knobs.txt v1: every knob registry row at the press, and each change of a live row after boot.\n"
                  "# knob\tid\tclass\tvalue\tboot\tdefault\tsource\tflags\traw\tchanges\tlast_change(frame@s)\n"
                  "# change\tframe\tvbl\tfeed_key\tt_s\tid\told\tvalue\told_raw\traw\n"
                  "# value, boot and default use the CLI's words where the row has a flag; boot is the value at\n"
                  "# the first committed tick (session first_frame); source is the config layer the boot value\n"
                  "# won at (config.txt's \"won at\"); flags: L value differs from boot, D differs from the\n"
                  "# declared default, C changed after boot; raw is the number the row's setter takes (- for a\n"
                  "# string row); t_s counts seconds from the process's SDL init. class: render (renderer state:\n"
                  "# --bug-report-replay applies it), host (window, pacing, audio, input or logging: the replay's\n"
                  "# own options decide it), sim (acts on the sim's timeline: never applied), latched (read at\n"
                  "# boot: the replay boots on the tester's argv, config and env), env (an OPENPETE_* variable:\n"
                  "# env.txt holds its press value, set from the replay's start), follow (a selection that\n"
                  "# tracks game state: its boot value only).\n");
    const uint64_t kept = s_bk.ev_n < BK_EVENTS ? s_bk.ev_n : BK_EVENTS;
    bk_printf(&t, "session\tfirst_frame=%u\tchanges=%llu\tkept=%llu\tdropped=%llu\n", s_bk.first_frame,
              (unsigned long long)s_bk.ev_n, (unsigned long long)kept,
              (unsigned long long)(s_bk.ev_n - kept));
    size_t n = 0;
    const knob_desc_t* ks = knob_registry(&n);
    if (n > s_bk.n) n = s_bk.n;
    for (size_t i = 0; i < n; i++)
        if (ks[i].type != KNOB_ACTION) bk_knob_line(&t, &ks[i], &s_bk.rows[i]);
    for (uint64_t i = s_bk.ev_n - kept; i < s_bk.ev_n; i++) bk_change_line(&t, ks, &s_bk.ev[i % BK_EVENTS]);
    if (t.oom) {
        free(t.b);
        return NULL;
    }
    return t.b;
}
