/* core/record/bugreport_replay_knobs.c - --bug-report-replay's application
 * of the live knob values a bundle's knobs.txt records; see
 * bugreport_internal.h. The member's layout is written by
 * core/runtime/host_bugknobs.c. */
#include "bugreport_internal.h"

#include "openpete_bugreport.h"
#include "openpete_knobs.h"
#include "openpete_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RK_FIELDS 12

/* A `knob` line's fields used here. The line's class field is not read:
 * the replay classifies this build's row of the id (rk_class). */
typedef struct {
    const char* id;
    const char* value;
    const char* boot;
    const char* flags;
    const char* raw;
} rk_row_t;

/* A `change` line's fields used here. */
typedef struct {
    unsigned    frame;
    const char* t_s;
    const char* id;
    const char* old;
    const char* value;
    const char* old_raw;
    const char* raw;
} rk_change_t;

static struct {
    int          loaded;       /* br_knobs_replay_load ran */
    int          present;      /* the bundle holds knobs.txt */
    int          off;          /* OPENPETE_BUGREPORT_REPLAY_KNOBS=0 */
    char*        text;         /* the member, split in place */
    rk_row_t*    rows;
    size_t       nrows;
    rk_change_t* ch;
    size_t       nch, next;
    unsigned     changed, applied_load, applied_tick, not_applied, off_skipped;
} K;

/** @brief Split a line on tabs in place.
 * @param line The line (modified).
 * @param f Receives the fields.
 * @param max Capacity of @p f.
 * @return The field count. */
static int rk_split(char* line, char** f, int max)
{
    int n = 0;
    f[n++] = line;
    for (char* p = line; *p && n < max; p++)
        if (*p == '\t') { *p = '\0'; f[n++] = p + 1; }
    return n;
}

/** @brief Parse one line into a row or a change; other lines are skipped. */
static void rk_line(char* line)
{
    char* f[RK_FIELDS];
    const int n = rk_split(line, f, RK_FIELDS);
    if (!strcmp(f[0], "knob") && n >= 9) {
        rk_row_t* g = (rk_row_t*)realloc(K.rows, (K.nrows + 1) * sizeof *g);
        if (!g) return;
        K.rows = g;
        K.rows[K.nrows++] = (rk_row_t){f[1], f[3], f[4], f[7], f[8]};
    } else if (!strcmp(f[0], "change") && n >= 10) {
        rk_change_t* g = (rk_change_t*)realloc(K.ch, (K.nch + 1) * sizeof *g);
        if (!g) return;
        K.ch = g;
        K.ch[K.nch++] = (rk_change_t){(unsigned)strtoul(f[1], NULL, 10), f[4], f[5], f[6], f[7], f[8], f[9]};
    }
}

/** @brief Find a parsed row by id, or NULL. */
static const rk_row_t* rk_row(const char* id)
{
    for (size_t i = 0; i < K.nrows; i++)
        if (!strcmp(K.rows[i].id, id)) return &K.rows[i];
    return NULL;
}

/** @brief This build's replay class of a row id (openpete_bugknobs_class), or NULL when this build has no row of that id. */
static const char* rk_class(const char* id)
{
    const knob_desc_t* k = knob_registry_find(id);
    return k ? openpete_bugknobs_class(k) : NULL;
}

/** @brief Set a row to a recorded value through this build's setter: the number for a numeric row, the text for a string row.
 * @param id The row id.
 * @param value The text.
 * @param raw The number, "-" for a string row.
 * @return 0, or -1 when this build has no live row of that id or its setter refuses the value. */
static int rk_apply(const char* id, const char* value, const char* raw)
{
    knob_desc_t* k = knob_registry_find(id);
    if (!k || k->mut != KNOB_LIVE) return -1;
    if (k->type == KNOB_STRING) return k->set_str ? k->set_str(k, value) : -1;
    if (!k->set_num || !strcmp(raw, "-")) return -1;
    return k->set_num(k, strtod(raw, NULL));
}

/** @brief Name why a class is not applied; NULL is a row this build lacks. */
static const char* rk_why(const char* cls)
{
    if (!cls) return "this build has no row of that id";
    if (!strcmp(cls, "host")) return "host: window, pacing, audio, input or logging state the replay's own options decide";
    if (!strcmp(cls, "sim")) return "sim: acts on the sim's timeline; the movie and the hash compare cover the run";
    if (!strcmp(cls, "env")) return "env: env.txt holds its press value, set from the replay's start";
    if (!strcmp(cls, "latched")) return "latched: read at boot; the replay boots on the tester's argv, config and env";
    if (!strcmp(cls, "follow")) return "follow: a selection that tracks game state";
    return "unknown class";
}

/** @brief The first change of a row after the anchor, or NULL. */
static const rk_change_t* rk_first_after(const char* id, unsigned anchor_frame)
{
    for (size_t i = 0; i < K.nch; i++)
        if (K.ch[i].frame > anchor_frame && !strcmp(K.ch[i].id, id)) return &K.ch[i];
    return NULL;
}

/** @brief Decide one row that changed after boot: apply its anchor value when this build classifies the row render, or print why not.
 * @param r The row.
 * @param anchor_frame The anchor's tick. */
static void rk_row_at_anchor(const rk_row_t* r, unsigned anchor_frame)
{
    K.changed++;
    const char* cls = rk_class(r->id);
    if (!cls || strcmp(cls, "render") != 0) {
        K.not_applied++;
        fprintf(stderr, "bug-report-replay: knob %s = %s not applied (%s)\n", r->id, r->value, rk_why(cls));
        return;
    }
    const rk_change_t* c = rk_first_after(r->id, anchor_frame);
    const char* v = c ? c->old : r->value;
    const char* raw = c ? c->old_raw : r->raw;
    if (!strcmp(v, r->boot)) return;   /* the boot configuration already holds it */
    if (K.off) {
        K.off_skipped++;
        fprintf(stderr, "bug-report-replay: knob %s = %s not applied at the anchor load "
                "(OPENPETE_BUGREPORT_REPLAY_KNOBS=0)\n", r->id, v);
        return;
    }
    if (rk_apply(r->id, v, raw) != 0) {
        K.not_applied++;
        fprintf(stderr, "bug-report-replay: knob %s = %s not applied (this build has no live row that takes it)\n",
                r->id, v);
        return;
    }
    K.applied_load++;
    fprintf(stderr, "bug-report-replay: knob %s = %s applied at the anchor load (frame %u; set live before "
            "the anchor; boot value %s)\n", r->id, v, anchor_frame, r->boot);
}

/** @brief Report whether the bundle's change ring dropped changes after the anchor: its session line counts drops and the oldest kept change is past the anchor. */
static int rk_partial(const char* session, unsigned anchor_frame)
{
    const char* d = session ? strstr(session, "dropped=") : NULL;
    if (!d || strtoul(d + 8, NULL, 10) == 0) return 0;
    return K.nch == 0 || K.ch[0].frame > anchor_frame;
}

/** @brief Split the member into lines and parse each; return the session line's text, or NULL. */
static const char* rk_parse(void)
{
    const char* session = NULL;
    for (char* line = K.text; line && *line;) {
        char* nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!strncmp(line, "session\t", 8)) session = line;
        else if (line[0] != '#') rk_line(line);
        line = nl ? nl + 1 : NULL;
        while (line && (*line == '\n' || *line == '\r')) line++;
    }
    return session;
}

int br_knobs_replay_load(const char* scratch, unsigned anchor_frame)
{
    K.loaded = 1;
    const char* e = getenv("OPENPETE_BUGREPORT_REPLAY_KNOBS");
    K.off = e && !strcmp(e, "0");
    char p[1024];
    snprintf(p, sizeof p, "%s/knobs.txt", scratch);
    size_t n = 0;
    uint8_t* d = NULL;
    if (br_read_file(p, &d, &n) != 0) {
        fprintf(stderr, "bug-report-replay: knobs: the bundle has no knobs.txt (written before the member): "
                "the live values at the press are unknown; the replay runs on the tester's boot configuration\n");
        return 0;
    }
    K.present = 1;
    K.text = (char*)d;
    const char* session = rk_parse();
    for (size_t i = 0; i < K.nrows; i++)
        if (strchr(K.rows[i].flags, 'C') || strchr(K.rows[i].flags, 'L')) rk_row_at_anchor(&K.rows[i], anchor_frame);
    while (K.next < K.nch && K.ch[K.next].frame <= anchor_frame) K.next++;
    const int partial = rk_partial(session, anchor_frame);
    if (partial)
        fprintf(stderr, "bug-report-replay: knobs: the bundle's change ring dropped changes after the anchor; "
                "the values at the anchor load are the press values of the rows whose first kept change is "
                "later (%s)\n", session);
    return partial;
}

void br_knobs_replay_tick(unsigned frame)
{
    for (; K.next < K.nch && K.ch[K.next].frame <= frame; K.next++) {
        const rk_change_t* c = &K.ch[K.next];
        const rk_row_t* r = rk_row(c->id);
        const char* cls = r ? rk_class(c->id) : NULL;
        if (!cls || strcmp(cls, "render") != 0) continue;   /* reported at the anchor load */
        if (K.off) {
            K.off_skipped++;
            fprintf(stderr, "bug-report-replay: knob %s = %s not applied at frame %u "
                    "(OPENPETE_BUGREPORT_REPLAY_KNOBS=0)\n", c->id, c->value, c->frame);
            continue;
        }
        if (rk_apply(c->id, c->value, c->raw) != 0) {
            K.not_applied++;
            fprintf(stderr, "bug-report-replay: knob %s = %s not applied at frame %u (this build has no live "
                    "row that takes it)\n", c->id, c->value, c->frame);
            continue;
        }
        K.applied_tick++;
        fprintf(stderr, "bug-report-replay: knob %s = %s applied at frame %u (changed live at frame %u, %s s, "
                "from %s)\n", c->id, c->value, frame, c->frame, c->t_s, c->old);
    }
}

void br_knobs_replay_report(void)
{
    if (!K.loaded || !K.present) return;
    fprintf(stderr, "bug-report-replay: knobs: %u changed after boot, %u applied (%u at the anchor load, %u at "
            "their tick), %u not applied%s\n", K.changed, K.applied_load + K.applied_tick, K.applied_load,
            K.applied_tick, K.not_applied + K.off_skipped, K.off ? " (OPENPETE_BUGREPORT_REPLAY_KNOBS=0)" : "");
}
