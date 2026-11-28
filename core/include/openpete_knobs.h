#ifndef OPENPETE_KNOBS_H
#define OPENPETE_KNOBS_H

/*
 * Knob registry - the table behind the M-key dev config overlay.
 *
 * One row per config knob (CLI flag and/or OPENPETE_* env var): what it is,
 * where its boot value came from (CLI beats env beats default - the same
 * per-knob sentinel-unset precedence the consumers implement), and, for the
 * knobs proven runtime-safe, how to read/write the live value.
 *
 * Deliberately NOT a reflection system: rows are added by editing
 * knob_registry.c (a compile-time diff), values flow through two tiny
 * function pointers per row, and everything imgui-specific stays in
 * devui.cpp. The registry itself is plain C and UI-free.
 *
 * Mutability contract (enforced here):
 *  - KNOB_LIVE rows route every write through a public setter that touches
 *    renderer/host state only - never gameplay-readable RAM (north-star).
 *  - KNOB_RESTART_ONLY rows have set_num/set_str == NULL and render disabled;
 *    they still display the resolved boot value + source. Promoting one to
 *    LIVE requires auditing every consumer for boot-time latching first
 *    (a single latched consumer means LIVE_FIXUP, not LIVE).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct openpete_options;   /* openpete.h */

typedef enum {
    KNOB_BOOL,     /* value_num 0/1 */
    KNOB_INT,      /* value_num integral */
    KNOB_FLOAT,    /* value_num */
    KNOB_ENUM,     /* value_num = index into enum_values */
    KNOB_STRING,   /* get_str/set_str */
    KNOB_ACTION    /* one-shot: a button, not a value (see below) */
} knob_type_t;

/* KNOB_ACTION rows carry no value - they fire something once (apply a cheat,
 * reset a counter). The row renders as a button captioned `action_label`
 * ("Apply" when NULL), and:
 *   set_num(k, 1)  fires it; returns 0 when the request was accepted.
 *   get_num        optional gate: 0 => the action can't run right now and the
 *                  button renders disabled (e.g. "not in a level yet").
 *   get_str        optional short status shown beside the button.
 * An action that touches gameplay state must not write it from here - the UI
 * runs outside the sim. Queue a request and let the game's own per-tick hook
 * perform the write at a tick boundary (north-star: gameplay state changes
 * only on the sim's schedule). */

typedef enum {
    KNOB_LIVE,          /* editable; writes go through the row's setter */
    KNOB_RESTART_ONLY   /* display-only: value applies at next launch */
} knob_mut_t;

typedef enum {
    KNOB_SRC_DEFAULT,   /* built-in default (neither CLI nor env supplied) */
    KNOB_SRC_ENV,       /* env var (incl. an openpete.toml [env] table) */
    KNOB_SRC_CLI        /* explicit flag (incl. openpete.toml / bk2 splices) */
} knob_src_t;

#define KNOB_STR_MAX 256

/* What a row's value can reach. Gameplay is the default: a row is
 * presentation only when an audit of every consumer of its value, and of
 * every other row or subsystem its setter writes, finds no write to guest
 * RAM (and, for a value the recompiled render pass reads, its spelling
 * clears the oracle once per non-default value); pace rows change when or
 * how fast ticks run, never what a tick computes. */
typedef enum {
    KNOB_EFFECT_GAMEPLAY = 0,
    KNOB_EFFECT_PRESENTATION,
    KNOB_EFFECT_PACE
} knob_effect_t;

/* What --bug-report-replay does with a live value row
 * (openpete_bugknobs_class): `host` rows set window, pacing, audio, input,
 * logging or menu state the replay's own options decide; `sim` rows act on
 * the sim's timeline; `follow` rows are selections that track game state
 * and are not sampled after boot; `render` rows write renderer state and
 * are the only class the replay applies. A live, non-action row that is
 * not an OPENPETE_* variable row declares one: openpete_knob_add refuses
 * such a row with KNOB_REPLAY_UNSET. Action, restart-only and variable rows
 * derive their class and leave it unset. */
typedef enum {
    KNOB_REPLAY_UNSET = 0,
    KNOB_REPLAY_HOST,
    KNOB_REPLAY_SIM,
    KNOB_REPLAY_FOLLOW,
    KNOB_REPLAY_RENDER
} knob_replay_t;

/* knob_desc_t.flags bits. HOLD: the value is held while a source holds it
 * (the key, a row write) and drops when none does. CONTENT_TOKENS: the enum
 * values are mod content claimed at run time, so a vocabulary that loses a
 * token between builds is not a removed control. NO_MOD: the row is outside
 * the mod surface. */
#define KNOB_FLAG_HOLD           0x01u
#define KNOB_FLAG_CONTENT_TOKENS 0x02u
#define KNOB_FLAG_NO_MOD         0x04u

typedef struct knob_desc {
    const char *id;        /* stable slug; imgui ID scope */
    const char *label;     /* display name */
    const char *section;   /* collapsing-header group */
    const char *cli;       /* "--flag" or NULL (env-only knob) */
    const char *env;       /* "OPENPETE_*" or NULL (CLI-only knob) */
    const char *help;      /* tooltip: what it does + apply-cost caveats */

    knob_type_t type;
    knob_mut_t  mut;

    const char *const *enum_values;  /* KNOB_ENUM: display strings; KNOB_STRING:
                                      * the tokens of a comma-list value, one
                                      * overlay toggle each (bind_vocab) */
    int    enum_count;
    float  num_min, num_max;         /* numeric widget range (both 0 = free) */

    /* Live value plumbing. Numeric types use get_num/set_num (ENUM = index);
     * strings use get_str/set_str. set_* NULL == not editable (RESTART_ONLY).
     * get_num should read LIVE state where a getter exists (a knob can also
     * change via its host hotkey - L, F, Alt+Enter...), else the registry's
     * stored copy. set_* returns 0 on success. */
    double (*get_num)(const struct knob_desc *k);
    int    (*set_num)(struct knob_desc *k, double v);
    void   (*get_str)(const struct knob_desc *k, char *buf, size_t cap);
    int    (*set_str)(struct knob_desc *k, const char *v);

    /* Stamped once by knob_registry_init (resolution-order replay). */
    knob_src_t boot_src;
    double     boot_num;
    char       boot_str[KNOB_STR_MAX];

    /* Registry-owned current value for rows without a live getter. */
    double stored_num;

    /* The declared default in the vocabulary a user types: the CLI's for a
     * row with a flag, the overlay's value word ("on", "off") for a flagless
     * typed row, the variable's value for an env row (shown by the overlay as
     * "(unset - <def>)"). NULL/empty = none declared. */
    const char *def_str;

    /* KNOB_ACTION only: button caption ("Apply" when NULL). */
    const char *action_label;

    /* Optional: 0 = leave this row out of the overlay for this run. For knobs
     * that act on something the run does not have - a capture that was never
     * started, a device that is absent - where a disabled row would just be a
     * question the user has to answer twice. The row still exists everywhere
     * else (--config-report, the config schema, saving), so this is display
     * only. NULL = always shown. A section whose rows are all hidden emits no
     * header. */
    int (*visible)(const struct knob_desc *k);

    /* Numeric rows whose 0 is a distinct mode rather than a magnitude, so the
     * setter accepts 0 and [num_min, num_max] but refuses the span between:
     * the caption of the checkbox that selects 0, beside a slider over the
     * range. NULL = the range runs to num_min with nothing below it. */
    const char *zero_label;

    /* The declared default, for a row whose default the program decides from
     * the machine, the game, another flag or an env var: how it is decided.
     * A row declares def_str (in the CLI's vocabulary, or the overlay's value
     * word for a row with no flag) or this, never both. NULL = none. */
    const char *default_note;

    /* The preprocessor symbol the row's feature needs at build time
     * ("OPENPETE_HAS_LIBAV"), or NULL when every build carries it. */
    const char *build_cond;

    /* How the `cli` flag takes its value (knob_flag_arg_t). */
    unsigned char flag_arg;

    /* 1 for a one-shot action flag (--ingest) that openpete.toml cannot
     * name: the row has no config schema entry and no bare key. */
    unsigned char cli_only;

    /* 1 for a built-in row compiled inside the OPENPETE_HIDE_DEBUG_FLAGS
     * block; set by the registry when it copies the built-ins in. */
    unsigned char debug_block;

    /* Device-creation class (config_schema.h cfg_device_t): 0 none, 1 a
     * knob read while the window, instance or device is created, resolved
     * only from tiers without a game or disc scope, whose scoped value is
     * reported not applied; 2 the same with a live path that applies a
     * scoped value after creation. The config schema's device set carries
     * the same class for each of the row's spellings. */
    unsigned char device_creation;

    /* --help line of the `cli` flag. help_arg is the value placeholder
     * printed after the flag ("0|1", "PATH"), NULL for none. help_text is
     * the description, one '\n' per printed line break; "{def}" expands to
     * def_str, and "{default_mark:NAME}" to " - default" when NAME is the
     * game's default renderer. help_order places the line inside its block
     * (debug_block, else the OPENPETE_HAS_LIBAV build_cond, else release),
     * ascending. A flag row with no help_text prints nothing. */
    const char *help_arg;
    const char *help_text;
    unsigned short help_order;

    /* The [keys.host] action that changes this row's setting ("pause"):
     * the overlay's tooltip shows its resolved binding and --help its
     * built-in one, as a Key field. NULL for a setting no action changes. */
    const char *key_action;

    /* A value of the `cli` flag other than def_str that the parser accepts,
     * the surface selfcheck's negative control for a string flag whose
     * grammar refuses def_str with "x" appended. NULL takes that default
     * control. */
    const char *control_str;

    /* knob_effect_t: what the value can reach; gameplay when undeclared. */
    unsigned char effect;
    /* knob_replay_t: the bug-report replay class of a live value row. */
    unsigned char replay;
    /* KNOB_FLAG_* bits. */
    unsigned char flags;
} knob_desc_t;

/* Arity of a row's flag: the default, a flag that takes no value
 * ("--no-card"), and one whose value is optional ("--input-lock [0|1]"). */
typedef enum {
    KNOB_FLAG_ARG_REQUIRED = 0,
    KNOB_FLAG_ARG_NONE,
    KNOB_FLAG_ARG_OPTIONAL
} knob_flag_arg_t;

/** @brief The knob table; mutable, since boot stamps and stored values live in the rows.
 *
 * The first call copies the built-in rows in; openpete_knob_add inserts
 * rows after the last row of their section.
 * @param count Receives the row count; may be NULL.
 * @return The first row. */
knob_desc_t *knob_registry(size_t *count);

/** @brief Stamp every row's boot source and boot value.
 *
 * The source comes from the config resolver (openpete_config_knob_layer).
 * The stored value is seeded from the row's openpete_options field; vsync,
 * devui_open and record_fps_overlay read their env var when the option is
 * unset, and freecam and camlock start at 0. Each non-action row's boot
 * value is then stamped from its getter, or from the stored value when it
 * has none. The dev UI calls it at init, after openpete_options_parse.
 * @param opts Parsed options; the pointer is kept and must outlive the registry. */
void knob_registry_init(const struct openpete_options *opts);
/** @brief Report whether knob_registry_init has run in this process.
 *
 * The overlay's init runs it only when a window exists; a headless run's
 * other readers of boot values (the bug-report knob record) run it
 * themselves when this returns 0.
 * @return 1 after the first knob_registry_init, else 0. */
int knob_registry_inited(void);

/** @brief Counter bumped on every env var write by a live registry row.
 *
 * A consumer that caches an env knob re-reads it when the epoch changes,
 * paying one getenv per edit instead of one per poll.
 * @return The current epoch; it starts at 1. */
unsigned knob_registry_env_epoch(void);

/** @brief Install the guard a live env row's write runs inside: guard(1) before the write, guard(0) after it.
 *
 * Threads that read the environment while a row may be edited (the
 * renderer's compile workers, inside which the Vulkan driver calls getenv)
 * hold the guard's other side, so the write never moves the environment
 * under them.
 * @param guard The guard, or NULL for none. */
void knob_registry_set_env_guard(void (*guard)(int begin));

/** @brief Whether a cache keyed on the registry env epoch must re-read its knob.
 *
 * The pattern for a live knob read on a hot path: keep the parsed value and
 * the epoch it was read at in statics, and call getenv only when this
 * returns 1. A cache seeded with 0 reads on its first call (the epoch starts
 * at 1).
 * @param seen The epoch the cache last read at; updated when stale.
 * @return 1 when the knob must be re-read. */
static inline int knob_env_epoch_stale(unsigned* seen) {
    const unsigned ep = knob_registry_env_epoch();
    if (*seen == ep) return 0;
    *seen = ep;
    return 1;
}

/** @brief Write a row's current value as a display string.
 *
 * Action rows show their get_str output, else action_label ("Apply" when
 * NULL); string rows their get_str output, else "?". Numeric rows read
 * get_num, else the stored value: bool as "on"/"off", int as %d, float as
 * %.3g, enum as its value name, or the index when out of range.
 * @param k Row.
 * @param buf Output buffer.
 * @param cap Size of `buf`. */
void knob_value_str(const knob_desc_t *k, char *buf, size_t cap);

/** @brief Write a row's current value in the vocabulary openpete.toml stores, the CLI's.
 *
 * The string is knob_value_str's except for the SSAA row, which displays
 * "4x" where the flag takes "4", and the fog row, whose follow-LOD entry has
 * no value. A value starting with '(' is a get_str placeholder
 * ("(default)", "(none)") and is refused, so it is never saved as a literal.
 * @param k Row.
 * @param buf Output buffer.
 * @param cap Size of `buf`.
 * @return 0 when the value can be persisted; 1 when the row is unset (the
 *         fog row following the LOD mode), so a saved key is removed; -1 for
 *         a NULL or action row, a row with no config schema entry, or a
 *         placeholder. */
int knob_config_value_str(const knob_desc_t *k, char *buf, size_t cap);

/** @brief Whether two config-vocabulary values of a row are the same setting.
 *
 * Bools compare by truth ("off" equals "0"), two values that both parse
 * whole as numbers compare numerically ("90" equals "90.0"), anything else
 * by exact text. An unparseable bool is unequal to everything.
 * @param k The row.
 * @param a One value.
 * @param b The other.
 * @return 1 when equal, else 0. */
int knob_config_values_equal(const knob_desc_t *k, const char *a, const char *b);

/** @brief Settle the named rows in openpete.toml's [saved] zone and write the file once.
 *
 * Each row's live value, in the CLI's vocabulary (knob_config_value_str),
 * is compared with the value the global tier gives it
 * (openpete_config_knob_value_below_saved), or its def_str when no global
 * layer sets it: an equal value, or an unset row, removes the row's
 * [saved] entry, so the knob follows the file and the default again; any
 * other value, or a
 * comparison that cannot be made, writes the entry. A row pinned by a
 * launch tier (a typed flag or an exported variable) is saved like any
 * other; the pin keeps winning while it is present. Unknown ids, action
 * rows and rows knob_config_value_str cannot persist are skipped. Called
 * only by the settings writer (openpete_settings_save.h) on the host loop.
 * @param ids Knob registry ids.
 * @param n Number of ids.
 * @return The number of rows settled (0 writes nothing), or -1 when the
 *         write failed. */
int knob_registry_save_ids(const char *const *ids, int n);

/** @brief Settle knob ids into the in-memory [saved] zone as knob_registry_save_ids does, without writing the file.
 *
 * The settings writer settles knobs and bindings and then writes once.
 * @param ids Knob registry ids.
 * @param n Number of ids.
 * @return The number of rows settled. */
int knob_registry_settle_ids(const char *const *ids, int n);

/** @brief Re-stamp the boot values of the rows that settle after knob_registry_init.
 *
 * The pacer resolves the render_fps target, tick_scale and the gated
 * runahead depth at host-loop start, and the session seeds the cadence
 * policy there (host_eager_start). Each of those rows adopts its getter's
 * value as both boot and stored value, so it does not read as edited. The
 * dev UI calls it once, on the overlay's first open. */
void knob_registry_late_stamp(void);

/* Late-bind a live string channel onto an existing row. Some knobs' setters
 * live in game feature modules the core library cannot link against; the
 * module attaches them here (apply-options time) and the row flips to
 * KNOB_LIVE. Either pointer may be NULL to keep the row's own. Returns 0 if
 * the id was found (it won't be when the row is compiled out, e.g. Debug
 * rows under OPENPETE_HIDE_DEBUG_FLAGS - callers just ignore that). */
/** @brief Append a row the core table cannot name, such as a game module's cheats or toggles.
 *
 * The row is copied, so the caller's knob_desc_t may be a stack temporary,
 * but every string it points at must outlive the process (literals or module
 * statics). The row lands after the last existing row of its section, so it
 * joins that collapsing header, or starts a new section at the end. Call at
 * feature apply-options time, before the overlay first draws and before
 * knob_registry_init stamps boot values.
 * @param row The row to copy in.
 * @return 0 on success, -1 when row is NULL, when its id, flag or env var
 *         names a registered row, when it is a live value row that needs
 *         a replay class and declares none (knob_needs_replay_class), or
 *         when the extra-row capacity (KNOB_EXTRA_MAX rows) is exhausted. */
int openpete_knob_add(const knob_desc_t *row);

/** @brief Report whether a row must declare its replay class: a live row that is neither an action nor an OPENPETE_* variable row.
 *
 * A variable row has an env spelling, no flag and a string value.
 * @param k The row.
 * @return 1 when the row needs a declared knob_replay_t, else 0. */
int knob_needs_replay_class(const knob_desc_t *k);

/** @brief Record the game's default window size for the --window row's "(default)" text.
 *
 * The host loop calls it once the game layout is known; the registry links
 * no game layout, so console tests that compile it need no game. Until it is
 * called, and while either size is 0, the row shows "(default)" with no size.
 * @param w Default window width in pixels.
 * @param h Default window height in pixels. */
void openpete_knob_default_window_set(int w, int h);

/** @brief Attach string accessors to the registry row `id`.
 *
 * A non-NULL `get_str` replaces the row's getter; a non-NULL `set_str` replaces
 * its setter and marks the row KNOB_LIVE. Lets a game module own the value
 * behind a row the registry builds.
 * @param id Row id to bind.
 * @param get_str Display getter, or NULL to keep the current one.
 * @param set_str Setter, or NULL to keep the current one.
 * @return 0 when the row exists, -1 when no row has that id (the row is
 *   compiled out). */
int openpete_knob_bind_str(const char *id,
                           void (*get_str)(const struct knob_desc *k,
                                           char *buf, size_t cap),
                           int (*set_str)(struct knob_desc *k, const char *v));

/** @brief Attach the token vocabulary of a comma-list string row.
 *
 * The overlay then draws one toggle per token in place of a text field: the
 * row's value lists the tokens switched on, "(all)" or an empty value reads
 * as every token on, and "none" is the value with every token off. A game
 * module owns the vocabulary of a row the registry builds, as with
 * openpete_knob_bind_str.
 * @param id Row id to bind.
 * @param tokens The token strings; they must outlive the process.
 * @param count The token count.
 * @return 0 when the row exists, -1 when no row has that id. */
int openpete_knob_bind_vocab(const char *id, const char *const *tokens,
                             int count);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_KNOBS_H */
