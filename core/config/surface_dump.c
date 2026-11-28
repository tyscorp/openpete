/* core/config/surface_dump.c - OPENPETE_SURFACE_DUMP: the game binary's
 * user-facing surface as JSON, and the selfcheck of its declared defaults.
 * See openpete_surface.h. */

#include "openpete_surface.h"
#include "openpete.h"
#include "openpete_config.h"
#include "openpete_keymap.h"
#include "openpete_knobs.h"
#include "openpete_menu.h"
#include "config_schema.h"
#include "game_extras.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define surface_environ _environ
#else
extern char** environ;
#define surface_environ environ
#endif

/* ------------------------------------------------------------------ */
/* the game's contributions                                            */
/* ------------------------------------------------------------------ */

#define SURFACE_ROWS_MAX  32
#define SURFACE_VOCAB_MAX 512
#define SURFACE_SLUG_MAX  64

typedef struct {
    const char* table;
    const char* flag;
    char        slug[SURFACE_SLUG_MAX];
} surface_vocab_t;

struct openpete_surface_sink {
    knob_desc_t     rows[SURFACE_ROWS_MAX];
    int             row_n;
    surface_vocab_t vocab[SURFACE_VOCAB_MAX];
    int             vocab_n;
    int             dropped;   /* entries past a capacity; fails the dump */
};

void openpete_surface_vocab(openpete_surface_sink_t* sink, const char* table,
                            const char* flag, const char* slug) {
    if (sink->vocab_n >= SURFACE_VOCAB_MAX) { sink->dropped++; return; }
    surface_vocab_t* v = &sink->vocab[sink->vocab_n++];
    v->table = table;
    v->flag  = flag;
    snprintf(v->slug, sizeof v->slug, "%s", slug);
}

void openpete_surface_row(openpete_surface_sink_t* sink, const knob_desc_t* row) {
    if (sink->row_n >= SURFACE_ROWS_MAX) { sink->dropped++; return; }
    sink->rows[sink->row_n++] = *row;
}

/* ------------------------------------------------------------------ */
/* JSON                                                                */
/* ------------------------------------------------------------------ */

/** @brief Write s as a JSON string literal, or null when s is NULL.
 *
 * Escapes the quote, the backslash and every control byte; other bytes pass
 * through, so UTF-8 text stays UTF-8.
 * @param f Output.
 * @param s The string, or NULL. */
static void json_str(FILE* f, const char* s) {
    if (!s) { fputs("null", f); return; }
    fputc('"', f);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"' || *p == '\\') fprintf(f, "\\%c", *p);
        else if (*p < 0x20)          fprintf(f, "\\u%04x", *p);
        else                         fputc(*p, f);
    }
    fputc('"', f);
}

/** @brief Write `, "key": <string or null>` inside an open JSON object. */
static void json_kv(FILE* f, const char* key, const char* val) {
    fprintf(f, ", \"%s\": ", key);
    json_str(f, val);
}

/** @brief Open one entry object of the entries array with its id `<ns>:<name>`; the first entry takes no leading comma.
 *
 * @param f Output.
 * @param first Cleared after the first entry.
 * @param ns The id namespace ("flag", "knob", ...).
 * @param name The id within the namespace. */
static void json_entry_open(FILE* f, int* first, const char* ns, const char* name) {
    char id[160];
    snprintf(id, sizeof id, "%s:%s", ns, name);
    fputs(*first ? "\n    {\"id\": " : ",\n    {\"id\": ", f);
    *first = 0;
    json_str(f, id);
}

/** @brief The JSON spelling of a flag arity (knob_flag_arg_t). */
static const char* arity_name(int a) {
    return a == KNOB_FLAG_ARG_NONE ? "none"
         : a == KNOB_FLAG_ARG_OPTIONAL ? "optional" : "required";
}

/** @brief Write a row's declared default: `def` or `default_note`, else both null. */
static void json_default(FILE* f, const knob_desc_t* k) {
    json_kv(f, "def", (k->def_str && *k->def_str) ? k->def_str : NULL);
    json_kv(f, "default_note", k->default_note);
}

/* ------------------------------------------------------------------ */
/* populations                                                         */
/* ------------------------------------------------------------------ */

static const char* const k_knob_type_names[] = {
    "bool", "int", "float", "enum", "string", "action"
};
static const char* const k_cfg_type_names[] = {
    "bool", "int", "float", "enum", "string"
};

/** @brief Write the schema facts of row id: its config key, schema type, CLI vocabulary and alias, or nulls when the schema has no row. */
static void json_schema(FILE* f, const char* id) {
    int si = cfg_schema_find_id(id);
    const cfg_knob_t* c = si >= 0 ? &cfg_schema(NULL)[si] : NULL;
    json_kv(f, "config_key", c ? c->id : NULL);
    json_kv(f, "schema_type", c ? k_cfg_type_names[c->type] : NULL);
    fputs(", \"vocab\": [", f);
    for (int i = 0; c && c->vocab && c->vocab[i]; i++) {
        if (i) fputs(", ", f);
        json_str(f, c->vocab[i]);
    }
    fputs("], \"aliases\": [", f);
    if (c && c->alias) json_str(f, c->alias);
    fputc(']', f);
}

/** @brief Write the knob entry of one registry row.
 *
 * @param f Output.
 * @param first The entries array's first-entry flag.
 * @param k The row.
 * @param game The registering game's id, or "core" for a built-in row. */
static void emit_knob(FILE* f, int* first, const knob_desc_t* k, const char* game) {
    json_entry_open(f, first, "knob", k->id);
    json_kv(f, "flag", k->cli);
    json_kv(f, "env", k->env);
    json_schema(f, k->id);
    json_kv(f, "type", k_knob_type_names[k->type]);
    json_kv(f, "mut", k->mut == KNOB_LIVE ? "live" : "restart");
    if (k->num_min != 0.0f || k->num_max != 0.0f)
        fprintf(f, ", \"range\": [%g, %g]", (double)k->num_min, (double)k->num_max);
    json_kv(f, "section", k->section);
    json_kv(f, "block", k->debug_block ? "debug" : "release");
    json_kv(f, "build_cond", k->build_cond);
    fprintf(f, ", \"cli_only\": %s", k->cli_only ? "true" : "false");
    if (k->type != KNOB_ACTION) json_default(f, k);
    json_kv(f, "game", game);
    fputc('}', f);
}

/** @brief Write the flag entry of a registry row's flag.
 *
 * The flag's --help block is not recorded here: audience is computed from
 * the --help output itself. */
static void emit_row_flag(FILE* f, int* first, const knob_desc_t* k) {
    json_entry_open(f, first, "flag", k->cli);
    json_kv(f, "flag", k->cli);
    json_kv(f, "takes_arg", arity_name(k->flag_arg));
    json_kv(f, "knob", k->id);
    json_kv(f, "build_cond", k->build_cond);
    json_default(f, k);
    fputc('}', f);
}

/** @brief Write the flag entries of the schema aliases (--no-lod), each naming the knob and value it sets. */
static void emit_alias_flags(FILE* f, int* first) {
    size_t n;
    const cfg_knob_t* sch = cfg_schema(&n);
    for (size_t i = 0; i < n; i++) {
        if (!sch[i].alias) continue;
        json_entry_open(f, first, "flag", sch[i].alias);
        json_kv(f, "flag", sch[i].alias);
        json_kv(f, "takes_arg", "none");
        json_kv(f, "knob", sch[i].id);
        json_kv(f, "alias_value", sch[i].alias_value);
        fputc('}', f);
    }
}

/** @brief Write the flag entries of the help-only table. */
static void emit_help_only(FILE* f, int* first) {
    size_t n;
    const openpete_help_only_t* h = openpete_options_help_only(&n);
    for (size_t i = 0; i < n; i++) {
        json_entry_open(f, first, "flag", h[i].flag);
        json_kv(f, "flag", h[i].flag);
        json_kv(f, "takes_arg", arity_name(h[i].takes_arg));
        json_kv(f, "block", h[i].block);
        json_kv(f, "build_cond", h[i].build_cond);
        json_kv(f, "def", NULL);
        json_kv(f, "default_note", h[i].default_note);
        fputc('}', f);
    }
}

/** @brief Write the menu entries: every settings-menu row with its knob, hint and whether the menu displays it. */
static void emit_menu(FILE* f, int* first) {
    openpete_menu_surface_t m;
    for (int i = 0; openpete_menu_surface_row(i, &m) == 0; i++) {
        json_entry_open(f, first, "menu", m.id);
        json_kv(f, "knob", m.knob);
        json_kv(f, "hint", m.hint);
        fprintf(f, ", \"displayed\": %s}", m.displayed ? "true" : "false");
    }
}

/* One key action: its id, flags and source default binding. */
typedef struct {
    const char* id;
    unsigned    flags;
    int         axis;
    const char* suggested;
} surface_key_t;

#define SURFACE_KEY_ROW(sym, id, kind, ctx, flags, def, purpose) \
    { id, (flags), (kind) == KM_AXIS, def },
static const surface_key_t k_host_keys[] = {
    OPENPETE_KEYMAP_HOST_ACTIONS(SURFACE_KEY_ROW)
};
static const surface_key_t k_pad_keys[] = {
    OPENPETE_KEYMAP_PAD_ACTIONS(SURFACE_KEY_ROW)
};
#undef SURFACE_KEY_ROW

/** @brief Write one key entry; `def` is the shipped-build binding: none for a KM_DEV row and for the second to fourth pads.
 *
 * @param f Output.
 * @param first The entries array's first-entry flag.
 * @param domain "host", "pad", "pad2".."pad4".
 * @param k The action.
 * @param unbound 1 when the domain starts with every action unbound. */
static void emit_key(FILE* f, int* first, const char* domain,
                     const surface_key_t* k, int unbound) {
    char name[96];
    snprintf(name, sizeof name, "%s.%s", domain, k->id);
    json_entry_open(f, first, "key", name);
    json_kv(f, "domain", domain);
    json_kv(f, "kind", k->axis ? "axis" : "digital");
    fprintf(f, ", \"dev\": %s, \"devbuild\": %s",
            (k->flags & KM_DEV) ? "true" : "false",
            (k->flags & KM_DEVBUILD) ? "true" : "false");
    json_kv(f, "def", (unbound || (k->flags & KM_DEV)) ? "none" : k->suggested);
    json_kv(f, "suggested", unbound ? "none" : k->suggested);
    fputc('}', f);
}

/** @brief Write every key action: the host domain, then the pad block once per pad domain. */
static void emit_keys(FILE* f, int* first) {
    static const char* const pads[] = { "pad", "pad2", "pad3", "pad4" };
    for (size_t i = 0; i < sizeof k_host_keys / sizeof k_host_keys[0]; i++)
        emit_key(f, first, "host", &k_host_keys[i], 0);
    for (int p = 0; p < 4; p++)
        for (size_t i = 0; i < sizeof k_pad_keys / sizeof k_pad_keys[0]; i++)
            emit_key(f, first, pads[p], &k_pad_keys[i], p > 0);
}

/** @brief Write the vocabulary entries the game reported, each id the table name and the slug joined by a dot. */
static void emit_vocab(FILE* f, int* first, const openpete_surface_sink_t* sink) {
    char name[SURFACE_SLUG_MAX + 32];
    for (int i = 0; i < sink->vocab_n; i++) {
        const surface_vocab_t* v = &sink->vocab[i];
        snprintf(name, sizeof name, "%s.%s", v->table, v->slug);
        json_entry_open(f, first, "vocab", name);
        json_kv(f, "table", v->table);
        json_kv(f, "slug", v->slug);
        json_kv(f, "flag", v->flag);
        fputc('}', f);
    }
}

/** @brief Write every entry: the registry's flags and rows, the game's rows, the schema aliases, the help-only flags, the menu, the keys, the config directive and the vocabularies. */
static void emit_entries(FILE* f, const openpete_surface_sink_t* sink) {
    int first = 1;
    size_t n;
    const knob_desc_t* ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) {
        emit_knob(f, &first, &ks[i], "core");
        if (ks[i].cli) emit_row_flag(f, &first, &ks[i]);
    }
    for (int i = 0; i < sink->row_n; i++)
        emit_knob(f, &first, &sink->rows[i], game_get_id());
    emit_alias_flags(f, &first);
    emit_help_only(f, &first);
    emit_menu(f, &first);
    emit_keys(f, &first);
    json_entry_open(f, &first, "directive", "portable");
    fputc('}', f);
    emit_vocab(f, &first, sink);
}

/* ------------------------------------------------------------------ */
/* selfcheck                                                           */
/* ------------------------------------------------------------------ */

/* The populations the selfcheck counts. FLAG: a row's flag parsed at its
 * def. SWITCH: a value-less flag, whose row value must read def unset and
 * whose presence must change the parse. ROW: a flagless typed row read
 * after knob_registry_init. ENV: env rows, whose def the selfcheck cannot
 * check. */
enum { POP_FLAG, POP_SWITCH, POP_ROW, POP_ENV, POP_COUNT };
static const char* const k_pop_names[POP_COUNT] = { "flag", "switch", "row", "env" };

#define SELFCHECK_FAIL_MAX 32

typedef struct {
    const char*         argv0;
    openpete_options_t  raw;           /* baseline parse, raw fields */
    openpete_options_t  base;          /* baseline after openpete_options_effective */
    int                 checked[POP_COUNT];
    int                 unchecked[POP_COUNT];
    int                 failed[POP_COUNT];
    char                fail[SELFCHECK_FAIL_MAX][160];
    int                 fail_n;
} selfcheck_t;

/** @brief Record a failure of population pop with a printf-style reason. */
static void sc_fail(selfcheck_t* sc, int pop, const char* fmt, const char* a,
                    const char* b) {
    sc->failed[pop]++;
    if (sc->fail_n < SELFCHECK_FAIL_MAX)
        snprintf(sc->fail[sc->fail_n++], sizeof sc->fail[0], fmt, a, b ? b : "");
}

/* Every pointer field of openpete_options_t. The comparison reads strings
 * by value, NULL-aware; a pointer field missing here compares by address
 * and makes a correct def fail, never pass. */
static const size_t k_str_fields[] = {
    offsetof(openpete_options_t, display_name),
    offsetof(openpete_options_t, screenshot_output),
    offsetof(openpete_options_t, ram_dump_output),
    offsetof(openpete_options_t, state_save_output),
    offsetof(openpete_options_t, state_load_input),
    offsetof(openpete_options_t, seed_ram_path),
    offsetof(openpete_options_t, seed_cpu_path),
    offsetof(openpete_options_t, halt_cpu_dump_output),
    offsetof(openpete_options_t, log_level_spec),
    offsetof(openpete_options_t, log_file),
    offsetof(openpete_options_t, disc_path),
    offsetof(openpete_options_t, exe_path),
    offsetof(openpete_options_t, ingest_path),
    offsetof(openpete_options_t, extract_recomp_inputs_dir),
    offsetof(openpete_options_t, recomp_module_path),
    offsetof(openpete_options_t, render_paths),
    offsetof(openpete_options_t, bk2_path),
    offsetof(openpete_options_t, bk2_record_args),
    offsetof(openpete_options_t, bk2_record_path),
    offsetof(openpete_options_t, unlock_frames_spec),
    offsetof(openpete_options_t, position_log_path),
    offsetof(openpete_options_t, gameplay_hash_log_path),
    offsetof(openpete_options_t, psxtrace_verify_path),
    offsetof(openpete_options_t, frame_time_log_path),
    offsetof(openpete_options_t, pace_log_path),
    offsetof(openpete_options_t, profile_out_path),
    offsetof(openpete_options_t, record_path),
    offsetof(openpete_options_t, record_encoder),
    offsetof(openpete_options_t, record_pixfmt),
    offsetof(openpete_options_t, record_audio_encoder),
    offsetof(openpete_options_t, texpack_dir),
    offsetof(openpete_options_t, sfxpack_dir),
    offsetof(openpete_options_t, controller_db),
    offsetof(openpete_options_t, mods_dir),
    offsetof(openpete_options_t, mods_enabled),
    offsetof(openpete_options_t, dump_sfx_pack_dir),
    offsetof(openpete_options_t, dump_sfx_wads),
    offsetof(openpete_options_t, card_dir),
    offsetof(openpete_options_t, card1_path),
    offsetof(openpete_options_t, card2_path),
    offsetof(openpete_options_t, resume_handoff),
    offsetof(openpete_options_t, bug_report_replay),
    offsetof(openpete_options_t, cheats),
};

/** @brief Whether two option blocks are equal field by field.
 *
 * The handoff fields (every parse writes them) and native_unavailable
 * (the launcher's hand-off writes it) are skipped; strings compare by value, NULL equal only to NULL; every other
 * byte compares exactly. Both blocks come from openpete_options_init,
 * whose memset zeroes the padding.
 * @return 1 when equal. */
static int options_equal(const openpete_options_t* a, const openpete_options_t* b) {
    openpete_options_t x, y;
    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    x.handoff_argc = y.handoff_argc = 0;
    x.handoff_argv = y.handoff_argv = NULL;
    x.native_unavailable = y.native_unavailable = 0;
    for (size_t i = 0; i < sizeof k_str_fields / sizeof k_str_fields[0]; i++) {
        const char** px = (const char**)((char*)&x + k_str_fields[i]);
        const char** py = (const char**)((char*)&y + k_str_fields[i]);
        int same = (!*px && !*py) || (*px && *py && !strcmp(*px, *py));
        if (!same) return 0;
        *px = *py = NULL;
    }
    return memcmp(&x, &y, sizeof x) == 0;
}

/** @brief Parse argv the way a boot does, into fresh options, with no config file, then resolve them through openpete_options_effective.
 *
 * The resolver is reset first, so no earlier parse leaks into this one.
 * @param argc Argument count.
 * @param argv Arguments; argv[0] is the program.
 * @param raw Receives the raw parse; may be NULL.
 * @param eff Receives the resolved parse.
 * @return The parser's exit code. */
static int sc_parse(int argc, char** argv, openpete_options_t* raw,
                    openpete_options_t* eff) {
    openpete_options_t o;
    openpete_options_init(&o);
    o.display_name = game_get_display_name();
    openpete_config_reset();
    int rc = openpete_options_parse(&o, argc, argv);
    if (raw) memcpy(raw, &o, sizeof o);
    memset(eff, 0, sizeof *eff);
    openpete_options_effective(&o, eff);
    return rc;
}

/** @brief Parse `--no-config <flag> [value]` and report whether the result differs from the baseline.
 *
 * @param sc The selfcheck.
 * @param flag The flag.
 * @param value Its value, or NULL for a value-less flag.
 * @param rc Receives the parser's exit code.
 * @return 1 when the parse differs from the baseline. */
static int sc_differs(selfcheck_t* sc, const char* flag, const char* value, int* rc) {
    char* argv[5] = { (char*)sc->argv0, (char*)"--no-config", (char*)flag,
                      (char*)value, NULL };
    openpete_options_t eff;
    *rc = sc_parse(value ? 4 : 3, argv, NULL, &eff);
    return !options_equal(&eff, &sc->base);
}

/** @brief The entry after `def` in a NULL-terminated or counted word list, wrapping to the first.
 *
 * @param words The list.
 * @param n Its length, or -1 for a NULL-terminated list.
 * @param def The word to step past; an absent word steps to the first.
 * @return The next word. */
static const char* next_word(const char* const* words, int n, const char* def) {
    int i = 0;
    while ((n < 0 ? words[i] != NULL : i < n) && strcmp(words[i], def) != 0) i++;
    int next = i + 1;
    if (n < 0 ? (words[i] == NULL || words[next] == NULL) : next >= n) next = 0;
    return words[next];
}

/** @brief A value of row k's flag other than its def, for the negative control.
 *
 * The next word of the flag's schema vocabulary, or of the row's enum when
 * the schema spells the flag as a free string (--aspect); the other state
 * of a bool; def + 1 for a number, def - 1 past num_max and num_min below
 * it (a 0 sentinel def under a [num_min, num_max] range, as --fov's), so
 * the parser never clamps the control; the row's control_str, else def
 * with "x" appended, for any other string.
 * @param k The row.
 * @param buf Scratch for a formatted value.
 * @param cap Size of buf.
 * @return The control value. */
static const char* control_value(const knob_desc_t* k, char* buf, size_t cap) {
    int si = cfg_schema_find_id(k->id);
    const cfg_knob_t* c = si >= 0 ? &cfg_schema(NULL)[si] : NULL;
    int ctype = c ? (int)c->type : (k->type == KNOB_STRING ? CFG_STRING : CFG_INT);
    if (ctype == CFG_ENUM) return next_word(c->vocab, -1, k->def_str);
    if (ctype == CFG_STRING && k->type == KNOB_ENUM)
        return next_word(k->enum_values, k->enum_count, k->def_str);
    if (ctype == CFG_BOOL) {
        int on = !strcmp(k->def_str, "1") || !strcmp(k->def_str, "on");
        return on ? "0" : "1";
    }
    if (ctype == CFG_STRING && k->control_str) return k->control_str;
    if (ctype == CFG_STRING) {
        snprintf(buf, cap, "%sx", k->def_str);
        return buf;
    }
    double d = atof(k->def_str) + 1.0;
    if (k->num_max != 0.0f && d > (double)k->num_max) d -= 2.0;
    if (k->num_max != 0.0f && d < (double)k->num_min) d = (double)k->num_min;
    snprintf(buf, cap, "%g", d);
    return buf;
}

/** @brief Check a row whose flag takes a value: its def parses to the baseline, and its control value parses to something else. */
static void sc_check_flag(selfcheck_t* sc, const knob_desc_t* k) {
    char ctl[64];
    int rc = 0;
    if (sc_differs(sc, k->cli, k->def_str, &rc) || rc != OPENPETE_EXIT_OK) {
        sc_fail(sc, POP_FLAG, "%s %s: the parse differs from the default parse",
                k->cli, k->def_str);
        return;
    }
    const char* v = control_value(k, ctl, sizeof ctl);
    if (!sc_differs(sc, k->cli, v, &rc) || rc != OPENPETE_EXIT_OK) {
        sc_fail(sc, POP_FLAG, "%s %s: the control value does not change the parse",
                k->cli, v);
        return;
    }
    sc->checked[POP_FLAG]++;
}

/** @brief Check a row read after knob_registry_init over the baseline: its value reads def. */
static int sc_row_reads_def(const knob_desc_t* k) {
    char buf[KNOB_STR_MAX];
    knob_value_str(k, buf, sizeof buf);
    return strcmp(buf, k->def_str) == 0;
}

/** @brief Check a row whose flag takes no value: it reads def over the baseline, and the flag changes the parse. */
static void sc_check_switch(selfcheck_t* sc, const knob_desc_t* k) {
    int rc = 0;
    if (!sc_row_reads_def(k)) {
        sc_fail(sc, POP_SWITCH, "%s: the row does not read %s by default", k->id, k->def_str);
        return;
    }
    if (!sc_differs(sc, k->cli, NULL, &rc) || rc != OPENPETE_EXIT_OK) {
        sc_fail(sc, POP_SWITCH, "%s: the flag does not change the parse", k->cli, NULL);
        return;
    }
    sc->checked[POP_SWITCH]++;
}

/* Flagless rows whose getter reads live state the dump never starts (the
 * free camera, the camera lock): comparing them checks nothing about the
 * declaration, so they count as unchecked. */
static const char* const k_live_rows[] = { "freecam", "camlock" };

/** @brief 1 when row id is in k_live_rows. */
static int is_live_row(const char* id) {
    for (size_t i = 0; i < sizeof k_live_rows / sizeof k_live_rows[0]; i++)
        if (!strcmp(id, k_live_rows[i])) return 1;
    return 0;
}

/** @brief Check a flagless typed row: its value after knob_registry_init over the baseline reads def. */
static void sc_check_row(selfcheck_t* sc, const knob_desc_t* k) {
    if (is_live_row(k->id)) { sc->unchecked[POP_ROW]++; return; }
    if (!sc_row_reads_def(k)) {
        sc_fail(sc, POP_ROW, "%s: the row does not read %s by default", k->id, k->def_str);
        return;
    }
    sc->checked[POP_ROW]++;
}

/** @brief The population of a non-action row: env for a pass-through env row, else by its flag. */
static int row_population(const knob_desc_t* k) {
    if (!k->cli && k->env && !k->get_num && k->type == KNOB_STRING) return POP_ENV;
    if (!k->cli) return POP_ROW;
    return k->flag_arg == KNOB_FLAG_ARG_NONE ? POP_SWITCH : POP_FLAG;
}

/** @brief Check one registry row's declared default, or count it unchecked.
 *
 * A row is unchecked when it declares a default_note (the pose flags,
 * whose parse writes the environment, among them), belongs to the env
 * population, or is a debug-block row with no def. A release row with
 * neither def nor note fails. */
static void sc_check(selfcheck_t* sc, const knob_desc_t* k) {
    const int pop = row_population(k);
    const int has_def = k->def_str && *k->def_str;
    if (k->type == KNOB_ACTION) return;
    if (pop == POP_ENV || k->default_note || (!has_def && k->debug_block)) {
        sc->unchecked[pop]++;
        return;
    }
    if (!has_def) {
        sc_fail(sc, pop, "%s: declares neither def nor default_note", k->id, NULL);
        return;
    }
    if (pop == POP_FLAG)        sc_check_flag(sc, k);
    else if (pop == POP_SWITCH) sc_check_switch(sc, k);
    else                        sc_check_row(sc, k);
}

/** @brief Unset every OPENPETE_* variable, so the selfcheck's parses see no env spelling. */
static void scrub_environment(void) {
    char name[256];
    for (;;) {
        char** e = surface_environ;
        while (*e && strncmp(*e, "OPENPETE_", 9) != 0) e++;
        if (!*e) return;
        size_t n = strcspn(*e, "=");
        if (n >= sizeof name) n = sizeof name - 1;
        memcpy(name, *e, n);
        name[n] = '\0';
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }
}

/** @brief Replace row `id`'s def with its control value, so a run can show the selfcheck failing; the surface test's negative control. */
static void break_def(const char* id) {
    static char ctl[64];
    size_t n;
    knob_desc_t* ks = knob_registry(&n);
    for (size_t i = 0; id && i < n; i++) {
        if (strcmp(ks[i].id, id) != 0 || !ks[i].def_str) continue;
        ks[i].def_str = control_value(&ks[i], ctl, sizeof ctl);
        if (ks[i].def_str != ctl) {
            snprintf(ctl, sizeof ctl, "%s", ks[i].def_str);
            ks[i].def_str = ctl;
        }
        fprintf(stderr, "openpete: surface dump: def of %s broken to \"%s\"\n", id, ctl);
    }
}

/** @brief Run the selfcheck over every registry row; the baseline parse is `--no-config` under a scrubbed environment and strict value checking. */
static void selfcheck_run(selfcheck_t* sc, const openpete_surface_sink_t* sink) {
    char* argv[3] = { (char*)sc->argv0, (char*)"--no-config", NULL };
    openpete_options_set_strict(1);
    if (sc_parse(2, argv, &sc->raw, &sc->base) != OPENPETE_EXIT_OK)
        sc_fail(sc, POP_FLAG, "the baseline parse failed%s%s", "", NULL);
    knob_registry_init(&sc->raw);
    size_t n;
    const knob_desc_t* ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++) sc_check(sc, &ks[i]);
    for (int i = 0; i < sink->row_n; i++) sc_check(sc, &sink->rows[i]);
    openpete_options_set_strict(0);
}

/** @brief Write the selfcheck's counts and failures as the dump's "selfcheck" object and to stderr.
 *
 * @return 0 when nothing failed and every checkable population (flag,
 *         switch, row) checked at least one entry, else 1. */
static int selfcheck_report(FILE* f, const selfcheck_t* sc) {
    int bad = 0;
    fputs(",\n  \"selfcheck\": {", f);
    for (int p = 0; p < POP_COUNT; p++) {
        fprintf(f, "%s\"%s\": {\"checked\": %d, \"unchecked\": %d, \"failed\": %d}",
                p ? ", " : "", k_pop_names[p], sc->checked[p], sc->unchecked[p],
                sc->failed[p]);
        fprintf(stderr, "openpete: surface selfcheck: %-6s checked %d, unchecked %d, failed %d\n",
                k_pop_names[p], sc->checked[p], sc->unchecked[p], sc->failed[p]);
        bad |= sc->failed[p] != 0;
        if (p != POP_ENV && sc->checked[p] == 0) {
            fprintf(stderr, "openpete: surface selfcheck: VACUOUS: no %s entry checked\n",
                    k_pop_names[p]);
            bad = 1;
        }
    }
    fputs(", \"failures\": [", f);
    for (int i = 0; i < sc->fail_n; i++) {
        if (i) fputs(", ", f);
        json_str(f, sc->fail[i]);
        fprintf(stderr, "openpete: surface selfcheck: FAIL: %s\n", sc->fail[i]);
    }
    fputs("]}", f);
    return bad;
}

/* ------------------------------------------------------------------ */
/* entry                                                               */
/* ------------------------------------------------------------------ */

/** @brief Give the load_delay row its def from the game's own default, which run.c applies when the flag is unset. */
static void set_game_defaults(void) {
    static char load_delay[16];
    size_t n;
    knob_desc_t* ks = knob_registry(&n);
    snprintf(load_delay, sizeof load_delay, "%u", (unsigned)game_get_load_delay_default());
    for (size_t i = 0; i < n; i++)
        if (!strcmp(ks[i].id, "load_delay")) ks[i].def_str = load_delay;
}

/** @brief Write the dump to path and run the selfcheck into it.
 *
 * @return The process exit code: 0 on success, 1 on a write failure, a
 *         dropped entry or a selfcheck failure. */
static int surface_dump(const char* path, const char* argv0, const char* broken) {
    static openpete_surface_sink_t sink;
    static selfcheck_t sc;
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "openpete: surface dump: cannot write %s\n", path); return 1; }
    set_game_defaults();
    break_def(broken);
    game_surface(&sink);
    sc.argv0 = argv0;
    selfcheck_run(&sc, &sink);
    fputs("{\n  \"format\": 1", f);
    json_kv(f, "game", game_get_id());
    fputs(",\n  \"entries\": [", f);
    emit_entries(f, &sink);
    fputs("\n  ]", f);
    int bad = selfcheck_report(f, &sc);
    fputs("\n}\n", f);
    if (sink.dropped)
        fprintf(stderr, "openpete: surface dump: %d game entries past capacity\n", sink.dropped);
    if (fclose(f) != 0) bad = 1;
    return (bad || sink.dropped) ? 1 : 0;
}

/** @brief Run the tooltip vocabulary check over the registry and the game's rows and exit when OPENPETE_TOOLTIP_VOCAB is set. */
static void tooltip_vocab_run_if_requested(void) {
    const char* v = getenv("OPENPETE_TOOLTIP_VOCAB");
    if (!v || !*v || *v == '0') return;
    static openpete_surface_sink_t sink;
    game_surface(&sink);
    exit(openpete_tooltip_vocab_check(sink.rows, sink.row_n,
                                      getenv("OPENPETE_TOOLTIP_VOCAB_BREAK"),
                                      stdout));
}

void openpete_surface_dump_run_if_requested(const char* argv0) {
    tooltip_vocab_run_if_requested();
    const char* p = getenv("OPENPETE_SURFACE_DUMP");
    if (!p || !*p) return;
    static char path[1024];
    static char broken[64];
    snprintf(path, sizeof path, "%s", p);
    const char* b = getenv("OPENPETE_SURFACE_SELFCHECK_BREAK");
    snprintf(broken, sizeof broken, "%s", b ? b : "");
    fprintf(stderr, "openpete: surface dump to %s\n", path);
    scrub_environment();
    exit(surface_dump(path, argv0, broken[0] ? broken : NULL));
}
