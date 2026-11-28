/* core/input/keymap.c - the action registry, the layered binding store,
 * and the resolver. See core/include/openpete_keymap.h for what this is
 * for and why it is SDL-free.
 *
 * Deliberately free of SDL and of config.c: config.c hands bindings in as
 * (domain, action, tokens, rank, origin) and gets an error back, so the
 * resolver links into console unit tests alongside the config resolver
 * with nothing but tomlc99 in the link line. */

#include "openpete_host_mouse.h"   /* OPENPETE_MOUSE_OWNED_* */
#include "openpete_input_log.h"
#include "openpete_keymap.h"
#include "openpete_log.h"

#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* the registry                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    const char*   id;
    unsigned char domain;
    unsigned char kind;
    unsigned char ctx;
    unsigned      flags;
    const char*   def;
    const char*   purpose;
    const char*   row;   /* host: the knob registry row driven; NULL when exempt */
    signed char   op;    /* host: km_op_t; pad: -1 */
} km_action_def_t;

#define KM_ROW_HOST(sym, id, k, c, f, r, o, d, p) { id, KM_DOM_HOST, k, c, f, d, p, r, o },
#define KM_ROW_PAD(sym, id, k, c, f, d, p)  { id, KM_DOM_PAD,  k, c, f, d, p, NULL, -1 },
/* Players 2-4 default to UNBOUND and carry no flags: giving them P1's
 * defaults would make every row collide with its P1 twin, and KM_REQUIRED on
 * a surface nobody has configured would warn on every stock boot. Their
 * devices are decoded through the shared controller merge, so an extra
 * player has working buttons with nothing bound here; these rows exist to
 * re-map that player: keymap_decode_slot (host_input.c) resolves each bound
 * row for the slot and leaves an unbound row's bit to the shared merge. */
#define KM_ROW_PAD2(sym, id, k, c, f, d, p) { id, KM_DOM_PAD2, k, c, 0u, "none", p, NULL, -1 },
#define KM_ROW_PAD3(sym, id, k, c, f, d, p) { id, KM_DOM_PAD3, k, c, 0u, "none", p, NULL, -1 },
#define KM_ROW_PAD4(sym, id, k, c, f, d, p) { id, KM_DOM_PAD4, k, c, 0u, "none", p, NULL, -1 },

static const km_action_def_t g_actions[] = {
    OPENPETE_KEYMAP_HOST_ACTIONS(KM_ROW_HOST)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD2)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD3)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ROW_PAD4)
};

/* One row per action; the enum and this table are generated from the same
 * X-macro lists, so a mismatch is a compile error rather than a silent
 * off-by-one into the wrong binding. */
typedef char km_assert_registry_len[
    (sizeof g_actions / sizeof g_actions[0]) == OPK_ACTION_COUNT ? 1 : -1];

#define KM_ORIGIN_MAX 48

typedef struct {
    km_source_t src[KM_MAX_SOURCES];
    int         n;
    int         rank;                    /* -1 = still the built-in default */
    char        origin[KM_ORIGIN_MAX];
    char        shadowed[KM_ORIGIN_MAX]; /* previous winner, for the report */
    unsigned char unbound_by_config;     /* "none"/[] rather than KM_DEV */
    /* Per "hold:" source: the poll's clock reading plus one when its input
     * was first seen held in the current hold, 0 while released; and
     * whether it has been held for KM_HOLD_MS. Written by km_hold_update. */
    unsigned long long hold_since[KM_MAX_SOURCES];
    unsigned char      hold_on[KM_MAX_SOURCES];
    /* The layers a binding edit compares against, recorded by
     * openpete_keymap_set against the bounds of
     * openpete_keymap_set_layer_bounds: the best call below the saved
     * layer (what a restore returns to), the best call below the game
     * scopes (the global layer), whether a game or disc scope bound the
     * action, and whether the live binding is the saved layer's. */
    km_source_t   below_src[KM_MAX_SOURCES];
    int           below_n;
    int           below_rank;
    char          below_origin[KM_ORIGIN_MAX];
    km_source_t   glob_src[KM_MAX_SOURCES];
    int           glob_n;
    int           glob_rank;
    unsigned char scoped;
    unsigned char has_saved;
} km_binding_t;

static km_binding_t g_bind[OPK_ACTION_COUNT];

/* Per-action wheel pulses and mouse motion counts of the current poll. */
static unsigned  g_pulses[OPK_ACTION_COUNT];
static long long g_delta[OPK_ACTION_COUNT];

/* ------------------------------------------------------------------ */
/* seams                                                              */
/* ------------------------------------------------------------------ */

static openpete_keymap_name_to_code_fn  s_to_code;
static openpete_keymap_code_to_name_fn  s_to_name;
static openpete_keymap_key_label_fn     s_key_label;
static openpete_keymap_pad_label_fn     s_pad_label;
static openpete_keymap_input_fns        s_in;

void openpete_keymap_set_lookup(openpete_keymap_name_to_code_fn to_code,
                                openpete_keymap_code_to_name_fn to_name,
                                openpete_keymap_key_label_fn label) {
    s_to_code = to_code; s_to_name = to_name; s_key_label = label;
}

void openpete_keymap_set_pad_label(openpete_keymap_pad_label_fn fn) {
    s_pad_label = fn;
}

void openpete_keymap_set_input(const openpete_keymap_input_fns* fns) {
    if (fns) s_in = *fns;
    else memset(&s_in, 0, sizeof s_in);
}

/* The store lock. The host loop is the only writer of the binding store;
 * the readers on other threads (the page and the tooltips on the imgui
 * frame thread, openpete_keymap_key_binds on the pump thread, the pad stamp
 * on the sampling thread) and every write take it. Host-loop reads stay
 * unlocked. A spin lock keeps this file free of SDL: every critical section
 * is a copy of one action's binding. */
static atomic_flag g_km_lock = ATOMIC_FLAG_INIT;

/** @brief Take the store lock, spinning until it is free. */
static void km_lock(void) {
    while (atomic_flag_test_and_set_explicit(&g_km_lock, memory_order_acquire)) {
    }
}

/** @brief Release the store lock. */
static void km_unlock(void) {
    atomic_flag_clear_explicit(&g_km_lock, memory_order_release);
}

/* The consume latch: inputs whose current press was already acted on (an
 * Escape the event filter consumed, every input held when a capture ended).
 * One bitset per kind, held until the poll sees the input released, so no
 * number of held inputs overflows it. Atomic because the pump thread sets
 * key bits. */
#define KM_LAT_WORDS(n) (((n) + 31) / 32)
static _Atomic unsigned g_lat_key[KM_LAT_WORDS(KM_SCAN_KEYS)];
static _Atomic unsigned g_lat_btn[KM_LAT_WORDS(KM_SCAN_PADBTNS)];
static _Atomic unsigned g_lat_axis[KM_LAT_WORDS(KM_SCAN_PADAXES)];
static _Atomic unsigned g_lat_mouse[1];

/** @brief Return the latch bitset of a source kind and its bit count, or NULL for a kind with no latch. */
static _Atomic unsigned* km_lat_set_of(int kind, int* bits) {
    switch (kind) {
    case KM_SRC_KEY:      *bits = KM_SCAN_KEYS;    return g_lat_key;
    case KM_SRC_PADBTN:   *bits = KM_SCAN_PADBTNS; return g_lat_btn;
    case KM_SRC_PADAXIS:  *bits = KM_SCAN_PADAXES; return g_lat_axis;
    case KM_SRC_MOUSEBTN: *bits = KM_MOUSE_X2 + 1; return g_lat_mouse;
    default:              *bits = 0;               return NULL;
    }
}

void openpete_keymap_consume_input(int kind, int code) {
    int bits = 0;
    _Atomic unsigned* set = km_lat_set_of(kind, &bits);
    if (!set || code < 0 || code >= bits) return;
    atomic_fetch_or_explicit(&set[code / 32], 1u << (code % 32), memory_order_relaxed);
}

void openpete_keymap_consume_key(int scancode) {
    openpete_keymap_consume_input(KM_SRC_KEY, scancode);
}

/** @brief Return whether an input is latched consumed.
 *
 * @param kind the source kind.
 * @param code the input's code.
 * @return 1 when latched, 0 otherwise or for a kind with no latch. */
static int km_consumed(int kind, int code) {
    int bits = 0;
    _Atomic unsigned* set = km_lat_set_of(kind, &bits);
    if (!set || code < 0 || code >= bits) return 0;
    return (atomic_load_explicit(&set[code / 32], memory_order_relaxed) >> (code % 32)) & 1u;
}

/** @brief Return whether a scancode is latched consumed (km_read_key reads it as not held). */
static int km_key_consumed(int scancode) {
    return km_consumed(KM_SRC_KEY, scancode);
}

/** @brief Return whether any input of any kind is latched consumed. */
static int km_latch_any(void) {
    for (size_t i = 0; i < KM_LAT_WORDS(KM_SCAN_KEYS); i++)
        if (atomic_load_explicit(&g_lat_key[i], memory_order_relaxed)) return 1;
    for (size_t i = 0; i < KM_LAT_WORDS(KM_SCAN_PADBTNS); i++)
        if (atomic_load_explicit(&g_lat_btn[i], memory_order_relaxed)) return 1;
    for (size_t i = 0; i < KM_LAT_WORDS(KM_SCAN_PADAXES); i++)
        if (atomic_load_explicit(&g_lat_axis[i], memory_order_relaxed)) return 1;
    return atomic_load_explicit(&g_lat_mouse[0], memory_order_relaxed) != 0;
}

/** @brief Return whether an input is physically held, read from the raw input seams.
 *
 * Bypasses the keyboard and pad suspensions, the mouse owner and the latch:
 * the capture and the latch release read the device, not a binding. A pad
 * axis is held past KM_AXIS_PRESS of throw either way.
 * @param kind KM_SRC_KEY, KM_SRC_PADBTN, KM_SRC_PADAXIS or KM_SRC_MOUSEBTN.
 * @param code the input's code.
 * @return 1 when held, 0 otherwise or when the seam is not installed. */
static int km_raw_held(int kind, int code) {
    switch (kind) {
    case KM_SRC_KEY:      return s_in.key_down ? s_in.key_down(code) != 0 : 0;
    case KM_SRC_PADBTN:   return s_in.pad_button ? s_in.pad_button(code) != 0 : 0;
    case KM_SRC_PADAXIS: {
        const int v = s_in.pad_axis ? s_in.pad_axis(code) : 0;
        return v > KM_AXIS_PRESS || v < -KM_AXIS_PRESS;
    }
    case KM_SRC_MOUSEBTN: return s_in.mouse_button ? s_in.mouse_button(code) != 0 : 0;
    default:              return 0;
    }
}

/** @brief Clear the latch bits of one kind whose inputs read released.
 *
 * @param kind the source kind. */
static void km_release_kind(int kind) {
    int bits = 0;
    _Atomic unsigned* set = km_lat_set_of(kind, &bits);
    for (int w = 0; w < KM_LAT_WORDS(bits); w++) {
        unsigned v = atomic_load_explicit(&set[w], memory_order_relaxed);
        for (int b = 0; v && b < 32; b++) {
            if (!((v >> b) & 1u) || km_raw_held(kind, w * 32 + b)) continue;
            atomic_fetch_and_explicit(&set[w], ~(1u << b), memory_order_relaxed);
            v &= ~(1u << b);
        }
    }
}

/** @brief Drop latched inputs that are no longer physically held, so the next press of each is live again.
 *
 * Reads the raw input seams rather than km_src_down, which consults the
 * latch and would never let an entry clear. With no seam installed every
 * entry of that kind clears. Called at the start of every
 * openpete_keymap_poll. */
static void km_release_consumed(void) {
    km_release_kind(KM_SRC_KEY);
    km_release_kind(KM_SRC_PADBTN);
    km_release_kind(KM_SRC_PADAXIS);
    km_release_kind(KM_SRC_MOUSEBTN);
}

/** @brief Latch every input that is physically held, so none reads as a press after a capture ends. */
static void km_latch_held(void) {
    static const int kinds[] = { KM_SRC_KEY, KM_SRC_PADBTN, KM_SRC_PADAXIS, KM_SRC_MOUSEBTN };
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        int bits = 0;
        (void)km_lat_set_of(kinds[k], &bits);
        for (int c = 0; c < bits; c++)
            if (km_raw_held(kinds[k], c)) openpete_keymap_consume_input(kinds[k], c);
    }
}

void openpete_keymap_latch_held_keys(void) {
    for (int c = 0; c < KM_SCAN_KEYS; c++)
        if (km_raw_held(KM_SRC_KEY, c)) openpete_keymap_consume_input(KM_SRC_KEY, c);
}

/** @brief Clear every latch bit. */
static void km_latch_clear(void) {
    for (size_t i = 0; i < KM_LAT_WORDS(KM_SCAN_KEYS); i++) atomic_store(&g_lat_key[i], 0u);
    for (size_t i = 0; i < KM_LAT_WORDS(KM_SCAN_PADBTNS); i++) atomic_store(&g_lat_btn[i], 0u);
    for (size_t i = 0; i < KM_LAT_WORDS(KM_SCAN_PADAXES); i++) atomic_store(&g_lat_axis[i], 0u);
    atomic_store(&g_lat_mouse[0], 0u);
}

/* The last refusal message of the token parser, and whether the parser
 * logs it: the binding editor parses a candidate quietly and shows the
 * message as the refusal reason. */
static char g_parse_err[192];
static int  g_parse_quiet;
#define KM_PARSE_ERR(...)                                                    \
    do {                                                                     \
        snprintf(g_parse_err, sizeof g_parse_err, __VA_ARGS__);              \
        if (!g_parse_quiet) OPL_ERROR_F("keys", "%s", g_parse_err);          \
    } while (0)

/* ------------------------------------------------------------------ */
/* token parsing                                                      */
/* ------------------------------------------------------------------ */

/* SDL has no mouse-button name table, so the `mouse:` vocabulary lives
 * here and the lookup seam is untouched for the two mouse kinds. */
static const struct { const char* name; unsigned char kind; short code; }
g_mouse_names[] = {
    { "left",       KM_SRC_MOUSEBTN,  KM_MOUSE_LEFT },
    { "middle",     KM_SRC_MOUSEBTN,  KM_MOUSE_MIDDLE },
    { "right",      KM_SRC_MOUSEBTN,  KM_MOUSE_RIGHT },
    { "x1",         KM_SRC_MOUSEBTN,  KM_MOUSE_X1 },
    { "x2",         KM_SRC_MOUSEBTN,  KM_MOUSE_X2 },
    { "wheel_up",   KM_SRC_MOUSEBTN,  KM_MOUSE_WHEEL_UP },
    { "wheel_down", KM_SRC_MOUSEBTN,  KM_MOUSE_WHEEL_DOWN },
    { "x",          KM_SRC_MOUSEAXIS, KM_MOUSE_X },
    { "y",          KM_SRC_MOUSEAXIS, KM_MOUSE_Y },
};

/** @brief Return the config name of a mouse source code, for reports.
 *
 * Searches g_mouse_names for the (kind, code) pair.
 * @param kind KM_SRC_MOUSEBTN or KM_SRC_MOUSEAXIS.
 * @param code the KM_MOUSE_* code.
 * @return the name without the "mouse:" prefix or a leading '-', or NULL
 *         when the pair has no entry. */
static const char* km_mouse_name(int kind, int code) {
    for (size_t i = 0; i < sizeof g_mouse_names / sizeof g_mouse_names[0]; i++)
        if (g_mouse_names[i].kind == kind && g_mouse_names[i].code == code)
            return g_mouse_names[i].name;
    return NULL;
}

/** @brief Return whether a source kind is a mouse button or a mouse axis. */
static int km_is_mouse_kind(int kind) {
    return kind == KM_SRC_MOUSEBTN || kind == KM_SRC_MOUSEAXIS;
}

static int km_find(const char* domain, const char* action);
static int km_streq_ci(const char* a, const char* b);
static int km_parse_hold(const char* inner, km_source_t* src, const char* where,
                         const char* domain, const char* action);

/** @brief Compare two strings for equality, ignoring ASCII letter case. */
static int km_streq_ci(const char* a, const char* b) {
    for (; *a && *b; a++, b++) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return 0;
    }
    return *a == *b;
}

/** @brief Strip leading and trailing spaces and tabs from a string in place. */
static void km_trim(char* s) {
    char* p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
}

/** @brief Parse the name after a "pad:" or "padN:" prefix into a button or axis source.
 *
 * Buttons are tried first, then axes; a leading '-' inverts an axis and is
 * an error on a button.
 * @param pad_name the text after the prefix.
 * @param pad_device 0 for an unqualified "pad:", else the player number.
 * @param src receives the source on success; zeroed by the caller.
 * @param where the config file or manifest named in error messages.
 * @param domain the binding domain.
 * @param action the action or mod binding name.
 * @return 1 when src is filled, -1 when the name is no button or axis. */
static int km_parse_pad(const char* pad_name, int pad_device, km_source_t* src,
                        const char* where, const char* domain, const char* action) {
    const char* name = pad_name;
    int invert = 0;
    if (*name == '-') { invert = 1; name++; }
    src->device = (unsigned char)pad_device;

    /* Buttons first, then axes: SDL names them from disjoint tables,
     * and trying the button table first means "pad:start" is never
     * mistaken for an axis. */
    int code = s_to_code ? s_to_code(KM_SRC_PADBTN, name) : -1;
    if (code >= 0) {
        if (invert) {
            KM_PARSE_ERR("%s: %s.%s: '-' inverts an axis, and "
                        "'%s' is a button", where, domain, action, name);
            return -1;
        }
        src->kind = KM_SRC_PADBTN;
        src->code = (short)code;
        return 1;
    }
    code = s_to_code ? s_to_code(KM_SRC_PADAXIS, name) : -1;
    if (code >= 0) {
        src->kind   = KM_SRC_PADAXIS;
        src->code   = (short)code;
        src->invert = (signed char)(invert ? -1 : 1);
        return 1;
    }
    KM_PARSE_ERR("%s: %s.%s: no gamepad button or axis named '%s'",
                where, domain, action, name);
    return -1;
}

/** @brief Parse one binding token into a source.
 *
 * The token is trimmed and truncated to 63 characters. An empty token or
 * "none" (any case) binds nothing. "pad:" is player 1 unqualified (device
 * 0); "pad1:".."pad4:" set device to the player number. A pad name is tried
 * as a button, then as an axis; a leading '-' inverts an axis and is an
 * error on a button. "mouse:" sources are refused in the pad domains; a
 * mouse axis binds only an action flagged KM_DELTA and never a mod
 * binding; a wheel pulse binds neither a mod binding nor an axis or
 * KM_LEVEL action. Any other name is a keyboard scancode. Every refusal is
 * logged under "keys" with where, domain and action.
 * @param tok the token text.
 * @param src receives the source on success.
 * @param where the config file or manifest named in error messages.
 * @param domain the binding domain ("host", "pad".."pad4", "mod.<id>").
 * @param action the action or mod binding name.
 * @return 1 when src is filled, 0 for an empty or "none" token, -1 for a
 *         token that names no source or is refused. */
static int km_parse_token(const char* tok, km_source_t* src, const char* where,
                          const char* domain, const char* action) {
    char buf[64];
    snprintf(buf, sizeof buf, "%s", tok);
    km_trim(buf);
    if (!buf[0] || km_streq_ci(buf, "none")) return 0;

    if (strncmp(buf, "hold:", 5) == 0)
        return km_parse_hold(buf + 5, src, where, domain, action);

    memset(src, 0, sizeof *src);

    /* "pad:" is player 1; "pad2:".."pad4:" address the other host pad slots.
     * The digit is the player number, so the slot index is one less. */
    int pad_device = -1;
    const char* pad_name = NULL;
    if (strncmp(buf, "pad:", 4) == 0) {
        pad_device = 0;                      /* unqualified */
        pad_name = buf + 4;
    } else if (strncmp(buf, "pad", 3) == 0 &&
               buf[3] >= '1' && buf[3] <= '0' + OPENPETE_PAD_SLOTS &&
               buf[4] == ':') {
        pad_device = buf[3] - '0';           /* 1-based player number */
        pad_name = buf + 5;
    }

    if (pad_device >= 0)
        return km_parse_pad(pad_name, pad_device, src, where, domain, action);

    /* "mouse:" sources. Accepted in the host and mod.<id> domains only: the
     * player-1 pad domain is pushed to PsyCross as gamepad codes and the
     * padN reader has no mouse member, so a mouse source there could never
     * be read. */
    if (strncmp(buf, "mouse:", 6) == 0) {
        const char* name = buf + 6;
        int invert = 0;
        if (*name == '-') { invert = 1; name++; }
        if (km_streq_ci(domain, "pad")  || km_streq_ci(domain, "pad2") ||
            km_streq_ci(domain, "pad3") || km_streq_ci(domain, "pad4")) {
            KM_PARSE_ERR("%s: %s.%s: mouse sources bind host and mod "
                        "actions only; the pad domains reach the game as "
                        "gamepad codes", where, domain, action);
            return -1;
        }
        const struct { const char* name; unsigned char kind; short code; }* m = NULL;
        for (size_t i = 0; i < sizeof g_mouse_names / sizeof g_mouse_names[0]; i++)
            if (km_streq_ci(g_mouse_names[i].name, name)) {
                m = (const void*)&g_mouse_names[i];
                break;
            }
        if (!m) {
            KM_PARSE_ERR("%s: %s.%s: no mouse button or axis named '%s' "
                        "(left right middle x1 x2 wheel_up wheel_down x y)",
                        where, domain, action, name);
            return -1;
        }
        const int is_mod = strncmp(domain, "mod.", 4) == 0;
        const int idx = is_mod ? -1 : km_find(domain, action);
        const unsigned flags = idx >= 0 ? g_actions[idx].flags : 0u;
        const int akind = idx >= 0 ? (int)g_actions[idx].kind : KM_DIGITAL;

        if (m->kind == KM_SRC_MOUSEAXIS) {
            /* A displacement, not a level: legal only where a consumer reads
             * openpete_key_delta, or it would bind and do nothing. */
            if (is_mod) {
                KM_PARSE_ERR("%s: %s.%s: 'mouse:%s' is motion; a mod "
                            "reads motion through mouse_read, not a binding",
                            where, domain, action, name);
                return -1;
            }
            if (idx >= 0 && !(flags & KM_DELTA)) {
                KM_PARSE_ERR("%s: %s.%s: 'mouse:%s' is a displacement "
                            "and %s reads a %s; only look_axis_x and "
                            "look_axis_y take mouse motion", where, domain,
                            action, name, action,
                            akind == KM_AXIS ? "level axis" : "button");
                return -1;
            }
            src->kind   = KM_SRC_MOUSEAXIS;
            src->code   = m->code;
            src->invert = (signed char)(invert ? -1 : 1);
            return 1;
        }
        if (invert) {
            KM_PARSE_ERR("%s: %s.%s: '-' inverts an axis, and "
                        "'mouse:%s' is a button", where, domain, action, name);
            return -1;
        }
        if (KM_MOUSE_IS_PULSE(m->code)) {
            /* A wheel notch is a pulse with no level. binding_down is a level
             * read from a game-tick hook, and a KM_LEVEL action is read
             * through openpete_key_down; neither can ever see it. */
            if (is_mod) {
                KM_PARSE_ERR("%s: %s.%s: 'mouse:%s' is a pulse and a "
                            "mod binding is read as a level", where, domain,
                            action, name);
                return -1;
            }
            if (idx >= 0 && (akind == KM_AXIS || (flags & KM_LEVEL))) {
                KM_PARSE_ERR("%s: %s.%s: 'mouse:%s' is a pulse and %s "
                            "is read as a level", where, domain, action, name,
                            action);
                return -1;
            }
        }
        src->kind = KM_SRC_MOUSEBTN;
        src->code = m->code;
        return 1;
    }

    /* Bare name == keyboard scancode. The `pad:` prefix is mandatory
     * precisely so this branch never has to guess: SDL's scancode and
     * gamepad-button name tables both contain "a", "b", "back" and
     * "start". */
    {
        int code = s_to_code ? s_to_code(KM_SRC_KEY, buf) : -1;
        if (code >= 0) {
            src->kind = KM_SRC_KEY;
            src->code = (short)code;
            return 1;
        }
    }
    KM_PARSE_ERR("%s: %s.%s: no key named '%s' (gamepad bindings "
                "need a pad prefix, e.g. \"pad:south\" for player 1 or "
                "\"pad2:south\" for player 2)",
                where, domain, action, buf);
    return -1;
}

/** @brief Parse the source after a "hold:" prefix and mark it a hold source.
 *
 * A hold source is read only by the live host poll, which times it, so it
 * binds a digital host action and nothing else: the pad domains are decoded
 * through a reader and mod bindings are read as levels from a game-tick
 * hook, and neither times a hold. The inner source must have a level: a
 * mouse motion or wheel source, a nested "hold:" and "none" are refused.
 * @param inner the token text after "hold:".
 * @param src receives the source on success.
 * @param where the config file or manifest named in error messages.
 * @param domain the binding domain.
 * @param action the action or mod binding name.
 * @return 1 when src is filled, -1 when refused. */
static int km_parse_hold(const char* inner, km_source_t* src, const char* where,
                         const char* domain, const char* action) {
    const int idx = km_streq_ci(domain, "host") ? km_find(domain, action) : -1;
    if (idx < 0 || g_actions[idx].kind != KM_DIGITAL) {
        KM_PARSE_ERR("%s: %s.%s: a hold: source binds a [keys.host] "
                    "button action only", where, domain, action);
        return -1;
    }
    if (strncmp(inner, "hold:", 5) == 0) {
        KM_PARSE_ERR("%s: %s.%s: hold: is written once", where,
                    domain, action);
        return -1;
    }
    int r = km_parse_token(inner, src, where, domain, action);
    if (r < 0) return -1;
    if (r == 0 || src->kind == KM_SRC_MOUSEAXIS ||
        (src->kind == KM_SRC_MOUSEBTN && KM_MOUSE_IS_PULSE(src->code))) {
        KM_PARSE_ERR("%s: %s.%s: 'hold:%s' needs a key or button "
                    "that stays down", where, domain, action, inner);
        return -1;
    }
    src->hold = 1;
    return 1;
}

/** @brief Split binding values on commas and parse each piece with km_parse_token.
 *
 * A scalar "C,pad:south" and an array ["C", "pad:south"] give the same
 * binding; the registry's defaults use the scalar form, so defaults and
 * config share this path. NULL tokens are skipped, each piece is truncated
 * to 63 characters, and sources past KM_MAX_SOURCES are dropped with a
 * warning.
 * @param tokens the values as written.
 * @param n number of tokens.
 * @param out receives up to KM_MAX_SOURCES sources.
 * @param out_n receives the number of sources parsed, including those
 *        parsed before a failure.
 * @param where the config file or manifest named in error messages.
 * @param domain the binding domain.
 * @param action the action or mod binding name.
 * @return 0, or -1 at the first piece km_parse_token rejects. */
static int km_parse_list(const char* const* tokens, int n,
                         km_source_t* out, int* out_n,
                         const char* where, const char* domain,
                         const char* action) {
    *out_n = 0;
    for (int i = 0; i < n; i++) {
        const char* s = tokens[i];
        if (!s) continue;
        while (*s) {
            const char* comma = strchr(s, ',');
            char piece[64];
            size_t len = comma ? (size_t)(comma - s) : strlen(s);
            if (len >= sizeof piece) len = sizeof piece - 1;
            memcpy(piece, s, len);
            piece[len] = '\0';

            km_source_t src;
            int r = km_parse_token(piece, &src, where, domain, action);
            if (r < 0) return -1;
            if (r > 0) {
                if (*out_n >= KM_MAX_SOURCES) {
                    OPL_WARN_F("keys", "%s: %s.%s: more than %d sources; "
                               "ignoring the rest", where, domain, action,
                               KM_MAX_SOURCES);
                } else {
                    out[(*out_n)++] = src;
                }
            }
            if (!comma) break;
            s = comma + 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* init                                                               */
/* ------------------------------------------------------------------ */

/** @brief Return the config spelling of a binding domain.
 *
 * @param d a KM_DOM_* value.
 * @return "pad", "pad2", "pad3" or "pad4" for the pad domains, "host" for
 *         any other value. */
static const char* km_domain_name(int d) {
    return d == KM_DOM_PAD  ? "pad"  : d == KM_DOM_PAD2 ? "pad2"
         : d == KM_DOM_PAD3 ? "pad3" : d == KM_DOM_PAD4 ? "pad4" : "host";
}

/** @brief Return whether a KM_DEV row's built-in default binds.
 *
 * Only a KM_DEVBUILD row binds, and only where the debug flags are compiled
 * in; the distribution presets define OPENPETE_HIDE_DEBUG_FLAGS and bind no
 * KM_DEV row.
 * @param flags the row's KM_* flags.
 * @return 1 when the default binds, 0 otherwise. */
static int km_devbuild_bound(unsigned flags) {
#ifdef OPENPETE_HIDE_DEBUG_FLAGS
    (void)flags;
    return 0;
#else
    return (flags & KM_DEVBUILD) != 0;
#endif
}

void openpete_keymap_init(void) {
    km_lock();
    memset(g_bind, 0, sizeof g_bind);
    km_latch_clear();
    memset(g_pulses, 0, sizeof g_pulses);
    memset(g_delta, 0, sizeof g_delta);
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_action_def_t* a = &g_actions[i];
        km_binding_t* b = &g_bind[i];
        b->rank = -1;
        snprintf(b->origin, sizeof b->origin, "default");

        /* KM_DEV rows resolve unbound no matter what their default says.
         * The default string is still kept in the registry (and printed
         * by --keys-report) so it reads as documentation of the key to
         * put in your own config, not as a key that is live.
         * KM_DEVBUILD exempts a row from that in a build that compiles
         * the debug flags in: there the default binds like any other, and
         * only the shipped build (OPENPETE_HIDE_DEBUG_FLAGS) withholds it. */
        if ((a->flags & KM_DEV) && !km_devbuild_bound(a->flags)) {
            b->n = 0;
        } else {
            const char* def = a->def;
            if (km_parse_list(&def, 1, b->src, &b->n,
                              "built-in defaults", km_domain_name(a->domain),
                              a->id) != 0) {
                /* A default that does not parse is a build-time mistake in
                 * the registry; say so loudly rather than shipping a dead
                 * key. */
                OPL_ERROR_F("keys", "built-in default for %s.%s is unparseable: %s",
                            km_domain_name(a->domain), a->id, a->def);
                b->n = 0;
            }
        }
        /* The defaults are the bottom of both comparison layers. */
        memcpy(b->below_src, b->src, sizeof b->src);
        b->below_n = b->n;
        b->below_rank = -1;
        snprintf(b->below_origin, sizeof b->below_origin, "default");
        memcpy(b->glob_src, b->src, sizeof b->src);
        b->glob_n = b->n;
        b->glob_rank = -1;
    }
    km_unlock();
}

/* ------------------------------------------------------------------ */
/* layering                                                           */
/* ------------------------------------------------------------------ */

/** @brief Return the registry index of a domain's action.
 *
 * The domain is matched case-insensitively against "host" and "pad".."pad4";
 * the action id is matched exactly.
 * @param domain the binding domain; "mod.<id>" and unknown domains match
 *        nothing.
 * @param action the action id.
 * @return the index into g_actions, or -1. */
static int km_find(const char* domain, const char* action) {
    int dom = -1;
    if (km_streq_ci(domain, "host")) dom = KM_DOM_HOST;
    else if (km_streq_ci(domain, "pad"))  dom = KM_DOM_PAD;
    else if (km_streq_ci(domain, "pad2")) dom = KM_DOM_PAD2;
    else if (km_streq_ci(domain, "pad3")) dom = KM_DOM_PAD3;
    else if (km_streq_ci(domain, "pad4")) dom = KM_DOM_PAD4;
    if (dom < 0) return -1;
    for (int i = 0; i < OPK_ACTION_COUNT; i++)
        if (g_actions[i].domain == (unsigned char)dom &&
            strcmp(g_actions[i].id, action) == 0)
            return i;
    return -1;
}

/** @brief Return the Levenshtein distance between two strings, for km_did_you_mean.
 *
 * Gives a mistyped action id the same "did you mean" hint config.c gives
 * bare keys.
 * @param a first string.
 * @param b second string.
 * @return the edit distance, or 99 when either string is longer than 32
 *         characters. */
static int km_edit_distance(const char* a, const char* b) {
    size_t la = strlen(a), lb = strlen(b);
    if (la > 32 || lb > 32) return 99;
    int prev[33], cur[33];
    for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
    for (size_t i = 1; i <= la; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= lb; j++) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            int d = prev[j] + 1;
            if (cur[j - 1] + 1 < d) d = cur[j - 1] + 1;
            if (prev[j - 1] + cost < d) d = prev[j - 1] + cost;
            cur[j] = d;
        }
        memcpy(prev, cur, sizeof(int) * (lb + 1));
    }
    return prev[lb];
}

/** @brief Return the action id in a domain closest to a mistyped one.
 *
 * Scans the registry rows of the domain and keeps the first row at the
 * smallest km_edit_distance; a distance of 4 or more is not a typo.
 * @param dom a KM_DOM_* value.
 * @param action the id as written.
 * @return the registry's id string, or NULL when no id is within 3 edits. */
static const char* km_did_you_mean(int dom, const char* action) {
    const char* best = NULL;
    int best_d = 4;   /* anything further away is not a typo */
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        if (g_actions[i].domain != (unsigned char)dom) continue;
        int d = km_edit_distance(action, g_actions[i].id);
        if (d < best_d) { best_d = d; best = g_actions[i].id; }
    }
    return best;
}

/* The ranks bounding the saved layer and the game scopes
 * (openpete_keymap_set_layer_bounds); INT_MAX until set, so every call
 * counts as global and nothing as saved. */
static int g_bound_saved = INT_MAX;
static int g_bound_game  = INT_MAX;

void openpete_keymap_set_layer_bounds(int saved_rank, int game_rank) {
    g_bound_saved = saved_rank;
    g_bound_game  = game_rank;
}

/** @brief Log the refusal of a [keys] row whose domain or action names nothing: the namespace, or the action with the nearest id of its domain.
 *
 * @param domain the domain as written.
 * @param action the action as written.
 * @param where the config file named in the message, or NULL. */
static void km_log_unknown(const char* domain, const char* action, const char* where) {
    int dom = km_streq_ci(domain, "pad")  ? KM_DOM_PAD
            : km_streq_ci(domain, "pad2") ? KM_DOM_PAD2
            : km_streq_ci(domain, "pad3") ? KM_DOM_PAD3
            : km_streq_ci(domain, "pad4") ? KM_DOM_PAD4
            : km_streq_ci(domain, "host") ? KM_DOM_HOST : -1;
    if (dom < 0) {
        OPL_ERROR_F("keys", "%s: unknown [keys] namespace '%s' "
                    "(expected pad, pad2, pad3, pad4, host or mod.<id>)",
                    where ? where : "config", domain);
        return;
    }
    const char* hint = km_did_you_mean(dom, action);
    OPL_ERROR_F("keys", "%s: [keys.%s] unknown action '%s'%s%s%s",
                where ? where : "config", domain, action,
                hint ? " (did you mean '" : "", hint ? hint : "",
                hint ? "'?)" : "");
}

/** @brief Record one config call into an action's comparison layers; the caller holds the store lock.
 *
 * The best call below the saved layer (what a restore returns to), the
 * best call below the game scopes (the global layer), whether a game or
 * disc scope called, and whether a saved-layer call did.
 * @param b the action's binding.
 * @param src the call's parsed sources.
 * @param cnt their count.
 * @param rank the call's rank.
 * @param origin the call's origin label, or NULL. */
static void km_record_layers(km_binding_t* b, const km_source_t* src, int cnt,
                             int rank, const char* origin) {
    if (rank < g_bound_saved && rank >= b->below_rank) {
        memcpy(b->below_src, src, sizeof b->below_src);
        b->below_n = cnt;
        b->below_rank = rank;
        snprintf(b->below_origin, sizeof b->below_origin, "%s",
                 origin ? origin : "config");
    }
    if (rank < g_bound_game && rank >= b->glob_rank) {
        memcpy(b->glob_src, src, sizeof b->glob_src);
        b->glob_n = cnt;
        b->glob_rank = rank;
    }
    if (rank >= g_bound_game && rank < g_bound_saved) b->scoped = 1;
    if (rank >= g_bound_saved && rank >= b->rank) b->has_saved = 1;
}

int openpete_keymap_set(const char* domain, const char* action,
                        const char* const* tokens, int n,
                        int rank, const char* origin, const char* where) {
    if (!domain || !action) return -1;

    int idx = km_find(domain, action);
    if (idx < 0) {
        km_log_unknown(domain, action, where);
        return -1;
    }

    km_binding_t* b = &g_bind[idx];
    /* A call outranked by the stored binding still feeds the comparison
     * layers below the saved layer, so it is parsed too, quietly: its
     * tokens never reach the live binding, and a refusal of a shadowed row
     * stays as silent as it always was. */
    const int shadowed = rank < b->rank;
    if (shadowed && rank >= g_bound_saved) return 0;

    km_source_t src[KM_MAX_SOURCES];
    int cnt = 0;
    const int quiet = g_parse_quiet;
    if (shadowed) g_parse_quiet = 1;
    const int perr = km_parse_list(tokens, n, src, &cnt, where ? where : "config",
                                   domain, action);
    g_parse_quiet = quiet;
    if (perr != 0) return shadowed ? 0 : -1;

    km_lock();
    km_record_layers(b, src, cnt, rank, origin);
    if (shadowed) { km_unlock(); return 0; }

    /* Ties go to the later call: config.c harvests files in ascending
     * precedence, so "same rank, seen later" means the user-dir file or a
     * line further down the same table.
     *
     * A scope REPLACES an action's source list rather than appending to
     * it. That is what lets a higher scope unbind (`= "none"`, or `= []`,
     * both of which land here with cnt == 0) something a lower scope
     * bound; append semantics would make unbinding inexpressible. */
    if (b->rank >= 0 || b->n > 0)
        snprintf(b->shadowed, sizeof b->shadowed, "%s", b->origin);
    memcpy(b->src, src, sizeof src);
    memset(b->hold_since, 0, sizeof b->hold_since);
    memset(b->hold_on, 0, sizeof b->hold_on);
    b->n = cnt;
    b->rank = rank;
    b->unbound_by_config = (cnt == 0) ? 1u : 0u;
    snprintf(b->origin, sizeof b->origin, "%s", origin ? origin : "config");
    km_unlock();
    return 0;
}

/* ------------------------------------------------------------------ */
/* resolve: conflict + reachability scans                             */
/* ------------------------------------------------------------------ */

/** @brief Return whether two input contexts can be live at the same instant.
 *
 * ALWAYS overlaps every context and each context overlaps itself. MENU is
 * disjoint from GAMEPLAY and FREECAM because opening the settings menu
 * holds the game pad neutral and zeroes the freecam axes; GAMEPLAY and
 * FREECAM are disjoint because the free camera freezes Spyro. FILLPICK
 * overlaps GAMEPLAY and FREECAM because the inspector runs with the game
 * live underneath it. DEVUI is disjoint from GAMEPLAY, FREECAM, FILLPICK and
 * MENU: while the dev overlay navigates with the pad, the game pad is held
 * neutral, every other live pad source is suspended and the freecam axes are
 * zeroed, and the overlay and the settings menu are never open together.
 * openpete_keymap_resolve reports a shared source only between actions
 * whose contexts overlap.
 * @param a first KM_CTX_* value.
 * @param b second KM_CTX_* value.
 * @return 1 when the contexts overlap, 0 otherwise. */
static int km_ctx_overlap(int a, int b) {
    if (a == KM_CTX_ALWAYS || b == KM_CTX_ALWAYS) return 1;
    if (a == b) return 1;
    if ((a == KM_CTX_GAMEPLAY && b == KM_CTX_FILLPICK) ||
        (b == KM_CTX_GAMEPLAY && a == KM_CTX_FILLPICK)) return 1;
    if ((a == KM_CTX_FREECAM  && b == KM_CTX_FILLPICK) ||
        (b == KM_CTX_FREECAM  && a == KM_CTX_FILLPICK)) return 1;
    return 0;
}

/** @brief Test whether two actions sharing an input is a known, deliberate overlap.
 *
 * Three pairs are deliberate: OPK_FREECAM with OPK_PAD_L3 (Spyro 1 never
 * reads L3), OPK_MENU with OPK_MENU_CLOSE (the host loop ignores the
 * release of a menu_close source held from before the menu opened), and
 * OPK_MENU_RIGHT with OPK_MENU_SELECT (a menu row is either a value row or
 * an action row). A hold: pad source on a host action against the same
 * plain pad input on a pad action is deliberate by km_hold_overlap, not by
 * this table. openpete_keymap_resolve() logs no warning for them, so a
 * stock boot stays silent, but still collects them, and --keys-report
 * prints them with the reason.
 * @param a First action index (OPK_*).
 * @param b Second action index, in either order.
 * @param why Receives the reason when the overlap is deliberate.
 * @return 1 when deliberate, 0 otherwise. */
static int km_overlap_is_intentional(int a, int b, const char** why) {
    int lo = a < b ? a : b, hi = a < b ? b : a;
    if (lo == OPK_FREECAM && hi == OPK_PAD_L3) {
        *why = "Spyro 1 never reads L3, so the stick click is free for the "
               "debug camera";
        return 1;
    }
    if (lo == OPK_MENU && hi == OPK_MENU_CLOSE) {
        *why = "a held Start opens the menu; its release is ignored and "
               "only a new press of Start closes the menu";
        return 1;
    }
    if (lo == OPK_MENU_RIGHT && hi == OPK_MENU_SELECT) {
        *why = "cross is 'increase' on a value row and 'activate' on an "
               "action row; a row is only ever one of the two";
        return 1;
    }
    return 0;
}

/** @brief Return whether two sources contest the same input.
 *
 * Kind and code must match, and pad axes must also share a polarity. The
 * device is not compared, so "pad:south" and "pad2:south" read as the same
 * input.
 * @param a first source.
 * @param b second source.
 * @return 1 when they contest, 0 otherwise. */
static int km_src_same(const km_source_t* a, const km_source_t* b) {
    if (a->kind != b->kind || a->code != b->code) return 0;
    /* Opposite polarities of one axis are the two HALVES of a stick's
     * throw, not a contested source - that is exactly how a bipolar axis
     * drives a pair of directions (menu_up on "pad:-lefty", menu_down on
     * "pad:lefty"). Only the same half of the same axis is a clash. */
    if (a->kind == KM_SRC_PADAXIS) return a->invert == b->invert;
    /* A mouse motion source delivers the whole displacement to every action
     * it is bound to whatever its sign, so mouse:x and mouse:-x contest. */
    return 1;
}

/** @brief Return whether a pad axis code names a trigger, from its config name ("lefttrigger", "righttrigger").
 *
 * A trigger is a button press, not a stick direction; the name lookup
 * keeps this file free of SDL's axis numbering. */
static int km_axis_is_trigger(int code) {
    const char* nm = s_to_name ? s_to_name(KM_SRC_PADAXIS, code) : NULL;
    return nm && strstr(nm, "trigger") != NULL;
}

/** @brief Return whether a source is a controller button: a pad button, or a trigger read as a press. */
static int km_src_is_pad_button(const km_source_t* s) {
    return s->kind == KM_SRC_PADBTN ||
           (s->kind == KM_SRC_PADAXIS && km_axis_is_trigger(s->code));
}

/* km_hold_overlap verdicts. */
enum { KM_HOLD_NONE = 0, KM_HOLD_GAME_TAP, KM_HOLD_OVERLAY_NAV };

/** @brief Return whether two contesting sources are an intended overlap: a hold of a controller button on a host button action against the same plain button on a pad action or an overlay's navigation action.
 *
 * A pad action reads the tap and the host action only a KM_HOLD_MS hold.
 * A KM_CTX_MENU or KM_CTX_DEVUI action reads the button only while its
 * overlay owns the pad, and then the live pad sources of every other host
 * action are suspended (openpete_keymap_suspend_pad with
 * openpete_keymap_menu_pad_exempt or the overlay's own set), so the hold
 * never fires there. A key is never covered: a key shared with another
 * action follows the conflict rule. Either argument order.
 * @param a the first source's action.
 * @param sa a's source.
 * @param b the second source's action.
 * @param sb b's source, contesting sa (km_src_same).
 * @return KM_HOLD_GAME_TAP or KM_HOLD_OVERLAY_NAV when intended,
 *         KM_HOLD_NONE otherwise. */
static int km_hold_overlap(int a, const km_source_t* sa, int b, const km_source_t* sb) {
    if (!km_src_is_pad_button(sa) || !km_src_is_pad_button(sb)) return KM_HOLD_NONE;
    for (int pass = 0; pass < 2; pass++) {
        const int h = pass ? b : a, p = pass ? a : b;
        const km_source_t* hs = pass ? sb : sa;
        const km_source_t* ps = pass ? sa : sb;
        if (!hs->hold || ps->hold || g_actions[h].domain != KM_DOM_HOST ||
            g_actions[h].kind != KM_DIGITAL)
            continue;
        if (g_actions[p].domain != KM_DOM_HOST) return KM_HOLD_GAME_TAP;
        if (g_actions[p].ctx == KM_CTX_MENU || g_actions[p].ctx == KM_CTX_DEVUI)
            return KM_HOLD_OVERLAY_NAV;
    }
    return KM_HOLD_NONE;
}

/* The reasons --keys-report prints for km_hold_overlap pairs. */
#define KM_HOLD_OVERLAP_WHY \
    "a tap reaches the game; the hotkey reads only a half-second hold"
#define KM_HOLD_NAV_WHY \
    "the overlay reads the button while it owns the pad; the hotkey's pad " \
    "sources are suspended then"

/** @brief Return whether OPENPETE_REBIND_MUTATE_NO_V22 disables km_hold_overlap in the conflict rule.
 *
 * The negative control of the hold harness leg; read once. */
static int km_mutate_no_v22(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_REBIND_MUTATE_NO_V22");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

typedef struct { int a, b; const char* why; } km_conflict_t;
#define KM_MAX_CONFLICTS 32
static km_conflict_t g_conflicts[KM_MAX_CONFLICTS];
static int g_conflict_n;

/* Mod bindings are stored apart from the action registry: a mod's
 * [[binding]] names are not knowable at config time. One entry per
 * (mod, name): the mod's own `key` registers at rank 0 (origin "mod") and a
 * [keys.mod.<id>] row at its scope's rank (origin "config") replaces it.
 * The table grows on demand; `enabled` follows the mod's live state so a
 * disabled mod's defaults leave the left_bound scan. */
typedef struct {
    char        mod[40];
    char        name[24];
    km_source_t src[KM_MAX_SOURCES];
    int         n;
    int         rank;
    unsigned char is_default;   /* origin: the mod's own key, not a config row */
    unsigned char enabled;
} km_mod_bind_t;

static km_mod_bind_t* g_modbind;
static int g_modbind_n, g_modbind_cap;

/** @brief Return the stored mod binding for (mod_id, name).
 *
 * Both names are compared in full against the stored copies, which
 * km_modbind_add truncates to 39 and 23 characters.
 * @param mod_id the mod's id.
 * @param name the binding name.
 * @return a pointer into g_modbind, valid until the next km_modbind_add, or
 *         NULL when no entry matches. */
static km_mod_bind_t* km_modbind_find(const char* mod_id, const char* name) {
    for (int i = 0; i < g_modbind_n; i++)
        if (!strcmp(g_modbind[i].mod, mod_id) && !strcmp(g_modbind[i].name, name))
            return &g_modbind[i];
    return NULL;
}

/** @brief Append an enabled, zeroed mod binding for (mod_id, name).
 *
 * The table starts at 32 entries and doubles. The id and name are copied
 * truncated to the stored field sizes (39 and 23 characters). No duplicate
 * check is made; km_set_mod calls km_modbind_find first.
 * @param mod_id the mod's id.
 * @param name the binding name.
 * @return the new entry, or NULL when the table cannot grow. */
static km_mod_bind_t* km_modbind_add(const char* mod_id, const char* name) {
    if (g_modbind_n == g_modbind_cap) {
        int cap = g_modbind_cap ? g_modbind_cap * 2 : 32;
        km_mod_bind_t* grown = realloc(g_modbind, sizeof *grown * (size_t)cap);
        if (!grown) return NULL;
        g_modbind = grown;
        g_modbind_cap = cap;
    }
    km_mod_bind_t* m = &g_modbind[g_modbind_n++];
    memset(m, 0, sizeof *m);
    snprintf(m->mod, sizeof m->mod, "%s", mod_id);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->enabled = 1;
    return m;
}

/* Whether any host or mod binding carries mouse:left. Read on the pump
 * thread by the double-click fullscreen gate, so two presses of a
 * mouse:left-bound action cannot also flip the window. */
static _Atomic int g_left_bound;

/** @brief Rescan the host and enabled mod bindings for mouse:left and publish the result.
 *
 * Host bindings count whether or not their action is live; a mod binding
 * counts only while its mod is enabled. Stores the flag read by
 * openpete_keymap_mouse_left_bound. */
static void km_recompute_left_bound(void) {
    int found = 0;
    for (int i = 0; i < OPK_ACTION_COUNT && !found; i++)
        for (int x = 0; x < g_bind[i].n; x++)
            if (g_bind[i].src[x].kind == KM_SRC_MOUSEBTN &&
                g_bind[i].src[x].code == KM_MOUSE_LEFT) { found = 1; break; }
    for (int i = 0; i < g_modbind_n && !found; i++)
        for (int x = 0; x < g_modbind[i].n && g_modbind[i].enabled; x++)
            if (g_modbind[i].src[x].kind == KM_SRC_MOUSEBTN &&
                g_modbind[i].src[x].code == KM_MOUSE_LEFT) { found = 1; break; }
    atomic_store_explicit(&g_left_bound, found, memory_order_relaxed);
}

int openpete_keymap_mouse_left_bound(void) {
    return atomic_load_explicit(&g_left_bound, memory_order_relaxed);
}

/** @brief Return whether two actions share a source, and whether the sharing is deliberate: a table pair, or every shared source a km_hold_overlap pair.
 *
 * @param i first action.
 * @param j second action.
 * @param intentional receives 1 when deliberate.
 * @param why receives the reason when deliberate.
 * @return 1 when any source is shared, 0 otherwise. */
static int km_pair_shares(int i, int j, int* intentional, const char** why) {
    int shared = 0, held = 0, nav = 0;
    for (int x = 0; x < g_bind[i].n; x++)
        for (int y = 0; y < g_bind[j].n; y++) {
            if (!km_src_same(&g_bind[i].src[x], &g_bind[j].src[y])) continue;
            shared++;
            const int h = km_hold_overlap(i, &g_bind[i].src[x], j, &g_bind[j].src[y]);
            held += h != KM_HOLD_NONE;
            nav += h == KM_HOLD_OVERLAY_NAV;
        }
    if (!shared) return 0;
    *intentional = km_overlap_is_intentional(i, j, why);
    if (!*intentional && held == shared) {
        *intentional = 1;
        *why = nav ? KM_HOLD_NAV_WHY : KM_HOLD_OVERLAP_WHY;
    }
    return 1;
}

int openpete_keymap_resolve(void) {
    int warnings = 0;
    g_conflict_n = 0;

    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        for (int j = i + 1; j < OPK_ACTION_COUNT; j++) {
            if (!km_ctx_overlap(g_actions[i].ctx, g_actions[j].ctx)) continue;
            const char* why = NULL;
            int intentional = 0;
            if (!km_pair_shares(i, j, &intentional, &why)) continue;
            if (g_conflict_n < KM_MAX_CONFLICTS) {
                g_conflicts[g_conflict_n].a = i;
                g_conflicts[g_conflict_n].b = j;
                g_conflicts[g_conflict_n].why = intentional ? why : NULL;
                g_conflict_n++;
            }
            if (!intentional) {
                OPL_WARN_F("keys", "%s.%s and %s.%s are both bound to the "
                           "same input (%s / %s)",
                           km_domain_name(g_actions[i].domain), g_actions[i].id,
                           km_domain_name(g_actions[j].domain), g_actions[j].id,
                           g_actions[i].purpose, g_actions[j].purpose);
                warnings++;
            }
        }
    }

    /* Reachability: an action the UI depends on that ended up with NO
     * source at all. A warning rather than an error - a pad-only player
     * unbinding the keyboard half of `cross` is legitimate - and it only
     * fires when every source is gone. KM_DEV rows are exempt: being
     * unbound is their whole design. */
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        if (!(g_actions[i].flags & KM_REQUIRED)) continue;
        if (g_actions[i].flags & KM_DEV) continue;
        if (g_bind[i].n > 0) continue;
        OPL_WARN_F("keys", "%s.%s is unbound (%s) - that control is now "
                   "unreachable", km_domain_name(g_actions[i].domain),
                   g_actions[i].id, g_actions[i].purpose);
        warnings++;
    }

    /* Mouse reachability. The settings menu owns the mouse while the MENU
     * context is live, so every mouse source on a MENU action reads neutral;
     * menu_close is exempt from that ownership (it may close what a mouse
     * button opened) and so from this clause. The menu's own gestures (left
     * and right click on a row, the wheel over a row) are consumed outside
     * the keymap, so a `menu` or `menu_close` binding on them would make
     * every row click also toggle or close the menu. */
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_action_def_t* a = &g_actions[i];
        const km_binding_t* b = &g_bind[i];
        for (int x = 0; x < b->n; x++) {
            const km_source_t* src = &b->src[x];
            if (!km_is_mouse_kind(src->kind)) continue;
            const char* nm = km_mouse_name(src->kind, src->code);
            if (a->ctx == KM_CTX_MENU && i != OPK_MENU_CLOSE) {
                OPL_WARN_F("keys", "%s.%s binds mouse:%s, which is unreachable: "
                           "the settings menu owns the mouse while it is open",
                           km_domain_name(a->domain), a->id, nm ? nm : "?");
                warnings++;
                continue;
            }
            if ((i == OPK_MENU || i == OPK_MENU_CLOSE) &&
                src->kind == KM_SRC_MOUSEBTN &&
                (src->code == KM_MOUSE_LEFT || src->code == KM_MOUSE_RIGHT ||
                 (i == OPK_MENU && KM_MOUSE_IS_PULSE(src->code)))) {
                OPL_WARN_F("keys", "%s.%s binds mouse:%s, a settings-menu "
                           "gesture: every %s would also %s the menu",
                           km_domain_name(a->domain), a->id, nm ? nm : "?",
                           KM_MOUSE_IS_PULSE(src->code) ? "wheel notch over a row"
                                                        : "row click",
                           i == OPK_MENU ? "toggle" : "close");
                warnings++;
            }
        }
    }
    km_recompute_left_bound();
    return warnings;
}

/* ------------------------------------------------------------------ */
/* live queries                                                       */
/* ------------------------------------------------------------------ */

static unsigned char g_held[OPK_ACTION_COUNT];
static unsigned char g_prev[OPK_ACTION_COUNT];
static int           g_suspend_kbd;
static int           g_suspend_pad;
/* Actions that stay readable through a keyboard or a pad suspension. */
static unsigned char g_kbd_exempt[OPK_ACTION_COUNT];
static unsigned char g_pad_exempt[OPK_ACTION_COUNT];

/* KM_AXIS_PRESS (openpete_keymap.h): past that fraction of full throw an
 * axis counts as a digital press - generous, because the only
 * digital-from-axis bindings are the analog triggers, where a light rest of
 * the finger should not register. */
/* Below this an axis reads as centred. Matches the freecam's own
 * long-standing stick deadzone so remapping a stick to an axis action
 * feels the same as the hardcoded read it replaces. */
#define KM_AXIS_DEAD    8000

/* Where a resolution reads from.
 *
 * `rd` NULL means the installed live reads - the host path, unchanged.
 * A non-NULL reader lets a caller resolve the same bindings against its own
 * snapshot, which is what keeps player 2's pad decode replayable: it is
 * decoded on the producer side from the sample being published, not from
 * whatever SDL happens to hold when a consumer later asks.
 *
 * `default_slot` serves sources with no explicit "padN:" qualifier, so an
 * unqualified "pad:" inside a [keys.pad2] block reads player 2's device. */
typedef struct {
    const openpete_keymap_reader_t* rd;
    int default_slot;
    /* The action is exempt from the settings menu's mouse ownership (OPK_MENU
     * and OPK_MENU_CLOSE, so a mouse-bound `menu` can close what it opened).
     * Never exempt from the dev overlay's. */
    int menu_exempt;
    /* The action is in openpete_keymap_suspend_keyboard's exempt set, so
     * its keyboard sources are read through a suspension. */
    int kbd_exempt;
    /* The action is in openpete_keymap_suspend_pad's exempt set, so its
     * live pad sources are read through a suspension. */
    int pad_exempt;
} km_resolve_t;

static const km_resolve_t KM_RESOLVE_LIVE = { NULL, 0, 0, 0, 0 };

/** @brief Return the current mouse owner from the input seam.
 *
 * @return an OPENPETE_MOUSE_OWNED_* value, OPENPETE_MOUSE_OWNED_NONE when
 *         no mouse_owned read is installed. */
static int km_mouse_owned(void) {
    return s_in.mouse_owned ? s_in.mouse_owned() : OPENPETE_MOUSE_OWNED_NONE;
}

/** @brief Return whether mouse sources read neutral under the current mouse owner.
 *
 * The dev overlay's ownership neutralises every action; the settings
 * menu's neutralises every action except a menu-exempt one.
 * @param owned an OPENPETE_MOUSE_OWNED_* value.
 * @param menu_exempt nonzero for OPK_MENU and OPK_MENU_CLOSE.
 * @return 1 when mouse sources read neutral, 0 otherwise. */
static int km_mouse_neutral(int owned, int menu_exempt) {
    if (owned == OPENPETE_MOUSE_OWNED_DEVUI) return 1;
    if (owned == OPENPETE_MOUSE_OWNED_MENU && !menu_exempt) return 1;
    return 0;
}

/** @brief Read a mouse button source's level.
 *
 * Reads 0 under a caller-supplied reader, for a wheel pulse source (no
 * level), while the button is latched consumed, and while the current mouse
 * owner neutralises the action (km_mouse_neutral); otherwise the
 * mouse_button seam.
 * @param c the resolution context.
 * @param s a KM_SRC_MOUSEBTN source.
 * @return 1 when held, 0 otherwise. */
static int km_read_mouse_btn(const km_resolve_t* c, const km_source_t* s) {
    /* A pad reader resolves the replayable pad decode; the mouse never
     * routes into it. A pulse source has no level. */
    if (c->rd) return 0;
    if (KM_MOUSE_IS_PULSE(s->code)) return 0;
    if (km_consumed(KM_SRC_MOUSEBTN, s->code)) return 0;
    if (km_mouse_neutral(km_mouse_owned(), c->menu_exempt)) return 0;
    return s_in.mouse_button ? s_in.mouse_button(s->code) : 0;
}

/** @brief Return the pad slot a source reads: its device minus one, else the context's default slot. */
static int km_src_slot(const km_resolve_t* c, const km_source_t* s) {
    return s->device ? s->device - 1 : c->default_slot;
}

/** @brief Read a keyboard source's level under a resolution context.
 *
 * Reads 0 while the keyboard is suspended (unless the action is exempt) and
 * while the scancode is latched consumed; otherwise the caller's reader
 * when set, else the key_down seam.
 * @param c the resolution context.
 * @param code the SDL scancode.
 * @return 1 when held, 0 otherwise. */
static int km_read_key(const km_resolve_t* c, int code) {
    if (g_suspend_kbd && !c->kbd_exempt) return 0;
    /* The event filter already acted on this press. */
    if (km_key_consumed(code)) return 0;
    if (c->rd) return c->rd->key_down ? c->rd->key_down(c->rd->ctx, code) : 0;
    return s_in.key_down ? s_in.key_down(code) : 0;
}

/** @brief Read a pad button source's level from the caller's reader or the live seams.
 *
 * Slot 0 reads the pad_button seam; a higher slot reads pad_button_slot and
 * reads 0 when that seam is not installed. A live read reads 0 while the pad
 * is suspended and the action is not exempt, and a live slot-0 read while
 * the button is latched consumed.
 * @param c the resolution context.
 * @param s a KM_SRC_PADBTN source.
 * @return 1 when held, 0 otherwise. */
static int km_read_btn(const km_resolve_t* c, const km_source_t* s) {
    const int slot = km_src_slot(c, s);
    if (!c->rd && g_suspend_pad && !c->pad_exempt) return 0;
    if (c->rd)
        return c->rd->pad_button ? c->rd->pad_button(c->rd->ctx, slot, s->code) : 0;
    if (slot > 0)
        return s_in.pad_button_slot ? s_in.pad_button_slot(slot, s->code) : 0;
    if (km_consumed(KM_SRC_PADBTN, s->code)) return 0;
    return s_in.pad_button ? s_in.pad_button(s->code) : 0;
}

/** @brief Return a pad axis source's raw value, sign-corrected and read from the source's pad slot.
 *
 * The slot is the source's device minus one, or the resolution's default
 * slot for an unqualified source. The caller's reader serves every slot
 * when set; otherwise slot 0 reads s_in.pad_axis and other slots
 * s_in.pad_axis_slot. The digital-press test and the analog reads share
 * this reader, so both honour the slot. A live read reads 0 while the pad is
 * suspended and the action is not exempt, and a live slot-0 read while the
 * axis is latched consumed.
 * @param c the resolution context.
 * @param s a KM_SRC_PADAXIS source; invert < 0 negates the value.
 * @return the read value (nominally [-32768, 32767]), negated for an
 *         inverted source, or 0 when the needed read is not installed. */
static int km_src_axis_raw(const km_resolve_t* c, const km_source_t* s) {
    const int slot = km_src_slot(c, s);
    int v;
    if (!c->rd && g_suspend_pad && !c->pad_exempt) return 0;
    if (c->rd) {
        if (!c->rd->pad_axis) return 0;
        v = c->rd->pad_axis(c->rd->ctx, slot, s->code);
    } else if (slot > 0) {
        if (!s_in.pad_axis_slot) return 0;
        v = s_in.pad_axis_slot(slot, s->code);
    } else {
        if (!s_in.pad_axis || km_consumed(KM_SRC_PADAXIS, s->code)) return 0;
        v = s_in.pad_axis(s->code);
    }
    return v * (s->invert < 0 ? -1 : 1);
}

/** @brief Return whether a single binding source is held.
 *
 * A pad axis is held past KM_AXIS_PRESS of full throw; a mouse motion source
 * has no level and reads 0.
 * @param c the resolution context.
 * @param s the source. */
static int km_src_down(const km_resolve_t* c, const km_source_t* s) {
    switch (s->kind) {
    case KM_SRC_KEY:
        return km_read_key(c, s->code);
    case KM_SRC_PADBTN:
        return km_read_btn(c, s);
    case KM_SRC_PADAXIS:
        return km_src_axis_raw(c, s) > KM_AXIS_PRESS;
    case KM_SRC_MOUSEBTN:
        return km_read_mouse_btn(c, s);
    case KM_SRC_MOUSEAXIS:
        return 0;   /* a displacement has no level */
    default: return 0;
    }
}

/** @brief Return whether an action keeps the mouse while the settings menu owns it (OPK_MENU, OPK_MENU_CLOSE). */
static int km_menu_exempt(openpete_action_t a) {
    return a == OPK_MENU || a == OPK_MENU_CLOSE;
}

/** @brief Return whether any source of an action is held under a resolution context.
 *
 * Derives the menu and keyboard exemptions from the action itself.
 * @param c the resolution context; copied, not modified.
 * @param a the action; out of range reads 0.
 * @return 1 when any source is held, 0 otherwise. */
static int km_action_down(const km_resolve_t* c, openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    const km_binding_t* b = &g_bind[a];
    km_resolve_t cc = *c;
    cc.menu_exempt = km_menu_exempt(a);
    cc.kbd_exempt  = g_kbd_exempt[a];
    cc.pad_exempt  = g_pad_exempt[a];
    for (int i = 0; i < b->n; i++) {
        if (b->src[i].hold) {
            /* Timed by the live poll; a reader never sees a hold. */
            if (!c->rd && b->hold_on[i]) return 1;
            continue;
        }
        if (km_src_down(&cc, &b->src[i])) return 1;
    }
    return 0;
}

/** @brief Advance every "hold:" source's timer from its input's live level.
 *
 * The input is read as a plain source of its action, with the action's
 * keyboard, pad and mouse exemptions, so a suspension stops a hold the same
 * way it stops a press. A held input starts its timer at the first poll
 * that sees it; the source reads held once KM_HOLD_MS has passed, and a
 * release clears both. Without the now_ms seam no hold source reads held.
 * Called by openpete_keymap_poll before the levels are sampled. */
static void km_hold_update(void) {
    const unsigned long long now = s_in.now_ms ? s_in.now_ms() : 0;
    for (int a = 0; a < OPK_ACTION_COUNT; a++) {
        km_binding_t* b = &g_bind[a];
        km_resolve_t cc = KM_RESOLVE_LIVE;
        cc.menu_exempt = km_menu_exempt((openpete_action_t)a);
        cc.kbd_exempt  = g_kbd_exempt[a];
        cc.pad_exempt  = g_pad_exempt[a];
        for (int i = 0; i < b->n; i++) {
            if (!b->src[i].hold) continue;
            km_source_t plain = b->src[i];
            plain.hold = 0;
            if (!s_in.now_ms || !km_src_down(&cc, &plain)) {
                b->hold_since[i] = 0;
                b->hold_on[i] = 0;
                continue;
            }
            if (!b->hold_since[i]) b->hold_since[i] = now + 1;
            b->hold_on[i] = (now + 1 - b->hold_since[i]) >= KM_HOLD_MS;
        }
    }
}

/** @brief Return an action's level in [-1, 1] from its first active source.
 *
 * Sources are tried in binding order. A pad axis past KM_AXIS_DEAD returns
 * its throw over 32767, clamped, and any other held source returns 1. The
 * first active source wins; values are not summed. Mouse motion sources are
 * skipped.
 * @param c the resolution context.
 * @param a the action; out of range reads 0.
 * @return the level, 0 when no source is active. */
static float km_action_axis(const km_resolve_t* c, openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0.0f;
    const km_binding_t* b = &g_bind[a];
    /* First non-neutral source wins rather than summing: with two sticks
     * bound to one axis, summing would let a centred-but-drifting pad
     * bias the other. Deterministic and order-stable. */
    for (int i = 0; i < b->n; i++) {
        const km_source_t* s = &b->src[i];
        if (s->hold) continue;   /* timed by the poll; it has no level here */
        if (s->kind == KM_SRC_PADAXIS) {
            int v = km_src_axis_raw(c, s);
            if (v > KM_AXIS_DEAD || v < -KM_AXIS_DEAD) {
                float f = (float)v / 32767.0f;
                return f > 1.0f ? 1.0f : (f < -1.0f ? -1.0f : f);
            }
        } else if (s->kind == KM_SRC_MOUSEAXIS) {
            continue;   /* a displacement has no level to return */
        } else if (km_src_down(c, s)) {
            return 1.0f;
        }
    }
    return 0.0f;
}

int openpete_keymap_action_down_via(openpete_action_t a, int default_slot,
                                    const openpete_keymap_reader_t* rd) {
    const km_resolve_t c = { .rd = rd, .default_slot = default_slot };
    return km_action_down(&c, a);
}

float openpete_keymap_action_axis_via(openpete_action_t a, int default_slot,
                                      const openpete_keymap_reader_t* rd) {
    const km_resolve_t c = { .rd = rd, .default_slot = default_slot };
    return km_action_axis(&c, a);
}

int openpete_keymap_action_axis_raw_via(openpete_action_t a, int default_slot,
                                        const openpete_keymap_reader_t* rd) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    const km_resolve_t c = { .rd = rd, .default_slot = default_slot };
    const km_binding_t* b = &g_bind[a];
    /* Same first-non-neutral-source rule as the float form, returning the
     * unscaled value: a pad decode has to reproduce the game's own
     * fixed-point axis conversion, and a float round-trip through a deadzone
     * would give the extra players a centre band player 1 does not have. */
    for (int i = 0; i < b->n; i++) {
        const km_source_t* s = &b->src[i];
        if (s->kind == KM_SRC_PADAXIS) {
            int v = km_src_axis_raw(&c, s);
            if (v > KM_AXIS_DEAD || v < -KM_AXIS_DEAD) return v;
        } else if (km_src_down(&c, s)) {
            return 32767;
        }
    }
    return 0;
}

int openpete_keymap_action_bound(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_bind[a].n : 0;
}

/* Wheel pulses and motion deltas: one marker per total, advanced every
 * poll whether or not anything is bound, so motion made while an overlay
 * owned the mouse is dropped rather than delivered late. */
static long long g_wheel_last;
static long long g_motion_last[KM_MOUSE_AXIS_COUNT];

/** @brief Divide a wheel total by OPENPETE_MOUSE_WHEEL_UNIT, rounding toward negative infinity. */
static long long km_floordiv_unit(long long v) {
    const long long u = OPENPETE_MOUSE_WHEEL_UNIT;
    return v >= 0 ? v / u : -((-v + u - 1) / u);
}

/** @brief Advance the wheel and motion markers and count each action's wheel pulses and mouse delta for this poll.
 *
 * The markers advance every poll; an action reads no pulse and no delta
 * while a capture holds input neutral or its mouse sources read neutral
 * under the current owner, so input made then is dropped, not delivered
 * late.
 * @param neutral nonzero while a capture holds input neutral. */
static void km_poll_mouse(int neutral) {
    const long long wheel = s_in.mouse_wheel_total ? s_in.mouse_wheel_total() : 0;
    const long long notches = km_floordiv_unit(wheel) - km_floordiv_unit(g_wheel_last);
    g_wheel_last = wheel;
    long long dm[KM_MOUSE_AXIS_COUNT];
    for (int ax = 0; ax < KM_MOUSE_AXIS_COUNT; ax++) {
        long long t = s_in.mouse_motion_total ? s_in.mouse_motion_total(ax) : 0;
        dm[ax] = t - g_motion_last[ax];
        g_motion_last[ax] = t;
    }
    const int owned = km_mouse_owned();
    memset(g_pulses, 0, sizeof g_pulses);
    memset(g_delta, 0, sizeof g_delta);
    if (neutral) return;

    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_binding_t* b = &g_bind[i];
        if (km_mouse_neutral(owned, km_menu_exempt((openpete_action_t)i))) continue;
        for (int x = 0; x < b->n; x++) {
            const km_source_t* s = &b->src[x];
            if (s->kind == KM_SRC_MOUSEBTN) {
                if (s->code == KM_MOUSE_WHEEL_UP && notches > 0)
                    g_pulses[i] += (unsigned)notches;
                else if (s->code == KM_MOUSE_WHEEL_DOWN && notches < 0)
                    g_pulses[i] += (unsigned)(-notches);
            } else if (s->kind == KM_SRC_MOUSEAXIS) {
                g_delta[i] += dm[s->code] * (s->invert < 0 ? -1 : 1);
            }
        }
    }
}

static void km_capture_step(void);

void openpete_keymap_poll(void) {
    km_release_consumed();
    km_capture_step();
    memcpy(g_prev, g_held, sizeof g_prev);
    /* While a capture runs every action reads neutral, and no hold timer
     * runs, so a held input neither fires its old binding nor completes a
     * hold behind the prompt. */
    const int neutral = openpete_keymap_capture_neutral();
    if (neutral) {
        for (int a = 0; a < OPK_ACTION_COUNT; a++) {
            memset(g_bind[a].hold_since, 0, sizeof g_bind[a].hold_since);
            memset(g_bind[a].hold_on, 0, sizeof g_bind[a].hold_on);
        }
    } else {
        km_hold_update();
    }
    for (int i = 0; i < OPK_ACTION_COUNT; i++)
        g_held[i] = neutral ? 0u
                  : (unsigned char)km_action_down(&KM_RESOLVE_LIVE,
                                                  (openpete_action_t)i);

    km_poll_mouse(neutral);
}

int openpete_key_down(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_held[a] : 0;
}

int openpete_key_edge(openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    return g_held[a] && !g_prev[a];
}

int openpete_key_pulses(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? (int)g_pulses[a] : 0;
}

int openpete_key_pressed(openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    return (g_held[a] && !g_prev[a]) || g_pulses[a] > 0;
}

long long openpete_key_delta(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_delta[a] : 0;
}

void openpete_keymap_mouse_markers(long long* wheel, long long* mx, long long* my) {
    if (wheel) *wheel = g_wheel_last;
    if (mx) *mx = g_motion_last[KM_MOUSE_X];
    if (my) *my = g_motion_last[KM_MOUSE_Y];
}

float openpete_key_axis(openpete_action_t a) {
    if (openpete_keymap_capture_neutral()) return 0.0f;
    return km_action_axis(&KM_RESOLVE_LIVE, a);
}

/** @brief Replace an exemption set with the in-range members of a list.
 *
 * @param set one byte per action, written 1 for a member and 0 otherwise.
 * @param list the members; NULL exempts none.
 * @param n number of members of list. */
static void km_set_exempt(unsigned char* set, const openpete_action_t* list, int n) {
    memset(set, 0, OPK_ACTION_COUNT);
    for (int i = 0; list && i < n; i++)
        if ((unsigned)list[i] < OPK_ACTION_COUNT) set[list[i]] = 1;
}

void openpete_keymap_suspend_keyboard(int on, const openpete_action_t* exempt, int n) {
    g_suspend_kbd = on ? 1 : 0;
    km_set_exempt(g_kbd_exempt, exempt, n);
}

void openpete_keymap_suspend_pad(int on, const openpete_action_t* exempt, int n) {
    g_suspend_pad = on ? 1 : 0;
    km_set_exempt(g_pad_exempt, exempt, n);
}

int openpete_keymap_sources(openpete_action_t a, km_source_t* out, int cap) {
    if ((unsigned)a >= OPK_ACTION_COUNT || !out || cap <= 0) return 0;
    km_lock();
    int n = g_bind[a].n < cap ? g_bind[a].n : cap;
    memcpy(out, g_bind[a].src, sizeof(km_source_t) * (size_t)n);
    km_unlock();
    return n;
}

const char* openpete_keymap_origin(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? g_bind[a].origin : "default";
}

int openpete_keymap_scancode_down(int scancode) {
    if (g_suspend_kbd || openpete_keymap_capture_neutral()) return 0;
    return s_in.key_down ? s_in.key_down(scancode) : 0;
}

int openpete_keymap_source_down(const km_source_t* s) {
    if (!s || openpete_keymap_capture_neutral()) return 0;
    return km_src_down(&KM_RESOLVE_LIVE, s);
}

int openpete_keymap_key_binds(openpete_action_t a, int scancode) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    int hit = 0;
    km_lock();
    const km_binding_t* b = &g_bind[a];
    for (int i = 0; i < b->n && !hit; i++)
        hit = b->src[i].kind == KM_SRC_KEY && b->src[i].code == scancode;
    km_unlock();
    return hit;
}

/* ------------------------------------------------------------------ */
/* the report                                                         */
/* ------------------------------------------------------------------ */

static int s_report_requested;
void openpete_keymap_request_report(void) { s_report_requested = 1; }
int  openpete_keymap_report_requested(void) { return s_report_requested; }

/** @brief Write one source in the config spelling, optionally with the layout's key label.
 *
 * A key prints its scancode name, with the current layout's label in
 * brackets when the two differ and `label` is set; a mouse source prints
 * "mouse:" and its name; a pad source its "padN:" prefix and a '-' for an
 * inverted axis; a hold source is prefixed "hold:". A name the lookup
 * cannot give prints as "?". Without the label the text is a token the
 * [keys] parser reads back.
 * @param s the source.
 * @param label nonzero to append the layout label to a key.
 * @param out receives the text, always NUL-terminated.
 * @param cap capacity of out in bytes; at least 6. */
static void km_config_piece_ex(const km_source_t* s, int label, char* out, size_t cap) {
    const char* nm = s_to_name ? s_to_name(s->kind, s->code) : NULL;
    const char* hold = s->hold ? "hold:" : "";
    if (km_is_mouse_kind(s->kind)) {
        const char* mn = km_mouse_name(s->kind, s->code);
        snprintf(out, cap, "%smouse:%s%s", hold,
                 (s->kind == KM_SRC_MOUSEAXIS && s->invert < 0) ? "-" : "",
                 mn ? mn : "?");
        return;
    }
    if (s->kind == KM_SRC_KEY) {
        /* Print the layout's own label next to the (positional) scancode
         * name when they differ, so a non-QWERTY user can see which
         * physical key to press without guessing. */
        const char* lbl = (label && s_key_label) ? s_key_label(s->code) : NULL;
        if (lbl && nm && strcmp(lbl, nm) != 0)
            snprintf(out, cap, "%s%s [%s]", hold, nm, lbl);
        else
            snprintf(out, cap, "%s%s", hold, nm ? nm : "?");
        return;
    }
    char padpfx[8];
    if (s->device) snprintf(padpfx, sizeof padpfx, "pad%d:", s->device);
    else           snprintf(padpfx, sizeof padpfx, "pad:");
    snprintf(out, cap, "%s%s%s%s", hold, padpfx,
             (s->kind == KM_SRC_PADAXIS && s->invert < 0) ? "-" : "",
             nm ? nm : "?");
}

/** @brief Write one source in the config spelling with the layout label, for km_format_binding. */
static void km_config_piece(const km_source_t* s, char* out, size_t cap) {
    km_config_piece_ex(s, 1, out, cap);
}

void openpete_keymap_source_token(const km_source_t* s, char* out, size_t cap) {
    if (!out || cap == 0) return;
    if (!s) { out[0] = '\0'; return; }
    km_config_piece_ex(s, 0, out, cap);
}

/** @brief Format a binding's sources as a comma-separated list for the report.
 *
 * Each source is spelled by km_config_piece. An unbound binding prints
 * "-". Output longer than the buffer is truncated and no further piece is
 * written.
 * @param b the binding.
 * @param out receives the text, always NUL-terminated.
 * @param cap capacity of out in bytes; must be at least 1. */
static void km_format_binding(const km_binding_t* b, char* out, size_t cap) {
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < b->n; i++) {
        char piece[96];
        km_config_piece(&b->src[i], piece, sizeof piece);
        int wrote = snprintf(out + used, cap - used, "%s%s",
                             used ? ", " : "", piece);
        if (wrote < 0 || (size_t)wrote >= cap - used) break;
        used += (size_t)wrote;
    }
    if (!b->n) snprintf(out, cap, "-");
}

int openpete_keymap_host_action(const char* id) {
    return id ? km_find("host", id) : -1;
}

const char* openpete_keymap_action_row(openpete_action_t a) {
    if ((int)a < 0 || a >= OPK_ACTION_COUNT) return NULL;
    return g_actions[a].row;
}

int openpete_keymap_action_op(openpete_action_t a) {
    if ((int)a < 0 || a >= OPK_ACTION_COUNT) return -1;
    return g_actions[a].op;
}

const char* openpete_keymap_row_key_action(const char* row_id) {
    if (!row_id) return NULL;
    for (int a = 0; a < OPK_ACTION_COUNT; a++)
        if (g_actions[a].row && !strcmp(g_actions[a].row, row_id)) return g_actions[a].id;
    return NULL;
}

/** @brief Write one source in the spelling a player reads, for openpete_keymap_action_label.
 *
 * @param s the source.
 * @param out receives the text, always NUL-terminated.
 * @param cap capacity of out in bytes. */
static void km_player_piece(const km_source_t* s, char* out, size_t cap) {
    const char* nm = s_to_name ? s_to_name(s->kind, s->code) : NULL;
    if (km_is_mouse_kind(s->kind)) {
        const char* mn = km_mouse_name(s->kind, s->code);
        snprintf(out, cap, "Mouse %s%s",
                 (s->kind == KM_SRC_MOUSEAXIS && s->invert < 0) ? "-" : "",
                 mn ? mn : "?");
        return;
    }
    if (s->kind == KM_SRC_KEY) {
        const char* lbl = s_key_label ? s_key_label(s->code) : NULL;
        snprintf(out, cap, "%s", lbl && *lbl ? lbl : nm ? nm : "?");
        return;
    }
    const char* pl = s_pad_label ? s_pad_label(s->kind, s->code, s->invert) : NULL;
    char pfx[12] = "";
    if (s->device) snprintf(pfx, sizeof pfx, "Pad%d ", s->device);
    if (pl)
        snprintf(out, cap, "%s%s", pfx, pl);
    else
        snprintf(out, cap, "%spad:%s%s", pfx,
                 (s->kind == KM_SRC_PADAXIS && s->invert < 0) ? "-" : "",
                 nm ? nm : "?");
}

/** @brief Write one source for openpete_keymap_action_label: km_player_piece, and " (hold)" after a hold source.
 *
 * @param s the source.
 * @param out receives the text, always NUL-terminated.
 * @param cap capacity of out in bytes. */
static void km_player_source(const km_source_t* s, char* out, size_t cap) {
    km_player_piece(s, out, cap);
    if (s->hold) {
        size_t len = strlen(out);
        if (len < cap) snprintf(out + len, cap - len, " (hold)");
    }
}

void openpete_keymap_source_label(const km_source_t* s, char* out, size_t cap) {
    if (!out || cap == 0) return;
    if (!s) { out[0] = '\0'; return; }
    km_player_piece(s, out, cap);
}

void openpete_keymap_action_label(openpete_action_t a, char* out, size_t cap) {
    out[0] = '\0';
    km_source_t src[KM_MAX_SOURCES];
    const int n = (unsigned)a < OPK_ACTION_COUNT ? openpete_keymap_sources(a, src, KM_MAX_SOURCES) : 0;
    if (!n) {
        snprintf(out, cap, "unbound");
        return;
    }
    size_t used = 0;
    for (int i = 0; i < n; i++) {
        char piece[64];
        km_player_source(&src[i], piece, sizeof piece);
        int wrote = snprintf(out + used, cap - used, "%s%s",
                             used ? " / " : "", piece);
        if (wrote < 0 || (size_t)wrote >= cap - used) break;
        used += (size_t)wrote;
    }
}

void openpete_keymap_action_default_label(openpete_action_t a, char* out, size_t cap) {
    if ((unsigned)a >= OPK_ACTION_COUNT) { snprintf(out, cap, "unbound"); return; }
    const km_action_def_t* d = &g_actions[a];
    if (((d->flags & KM_DEV) && !km_devbuild_bound(d->flags)) ||
        km_streq_ci(d->def, "none")) {
        snprintf(out, cap, "unbound");
        return;
    }
    size_t used = 0;
    out[0] = '\0';
    for (const char* p = d->def; *p && used + 3 < cap; p++) {
        out[used++] = *p;
        if (*p == ',') out[used++] = ' ';
    }
    out[used] = '\0';
}

void openpete_keymap_report(FILE* out) {
    if (!out) return;
    fprintf(out, "openpete keymap\n");
    fprintf(out, "  %-22s %-42s %-16s %s\n",
            "action", "binding", "from", "purpose");

    int last_domain = -1;
    for (int i = 0; i < OPK_ACTION_COUNT; i++) {
        const km_action_def_t* a = &g_actions[i];
        const km_binding_t* b = &g_bind[i];
        if ((int)a->domain != last_domain) {
            fprintf(out, "\n  [keys.%s]\n", km_domain_name(a->domain));
            last_domain = a->domain;
        }
        char bind[160];
        km_format_binding(b, bind, sizeof bind);

        /* Why a row is blank matters more than that it is: "dev" means
         * the registry withheld it, "unbound at <scope>" means a config
         * line did. */
        char from[64];
        if (!b->n && (a->flags & KM_DEV) && b->rank < 0)
            snprintf(from, sizeof from, "dev (default %s)", a->def);
        else if (b->unbound_by_config)
            snprintf(from, sizeof from, "unbound at %s", b->origin);
        else
            snprintf(from, sizeof from, "%s", b->origin);

        fprintf(out, "  %-22s %-42s %-16s %s\n",
                a->id, bind, from, a->purpose);
        if (b->shadowed[0] && strcmp(b->shadowed, b->origin) != 0)
            fprintf(out, "  %-22s   (shadowed %s)\n", "", b->shadowed);
    }

    /* Mod overrides live in their own store (mod ids are not knowable at
     * config time), but they are bindings too - leaving them out would
     * make this report an incomplete answer to "what does this install
     * respond to?". Printed after a scan of the store so an override for
     * a mod that is not installed still shows up rather than vanishing. */
    if (g_modbind_n) {
        fprintf(out, "\n  [keys.mod.*]\n");
        for (int i = 0; i < g_modbind_n; i++) {
            km_binding_t tmp;
            memset(&tmp, 0, sizeof tmp);
            memcpy(tmp.src, g_modbind[i].src, sizeof tmp.src);
            tmp.n = g_modbind[i].n;
            char bind[160];
            km_format_binding(&tmp, bind, sizeof bind);
            char id[72];
            snprintf(id, sizeof id, "%s.%s", g_modbind[i].mod, g_modbind[i].name);
            fprintf(out, "  %-22s %-42s %-16s %s\n", id, bind,
                    g_modbind[i].is_default ? "mod default" : "config",
                    g_modbind[i].is_default ? "mod binding (mod.toml key)"
                                            : "mod binding override");
        }
    }

    if (g_conflict_n) {
        fprintf(out, "\n  shared inputs:\n");
        for (int i = 0; i < g_conflict_n; i++) {
            const km_conflict_t* c = &g_conflicts[i];
            fprintf(out, "    %s.%s + %s.%s%s%s\n",
                    km_domain_name(g_actions[c->a].domain), g_actions[c->a].id,
                    km_domain_name(g_actions[c->b].domain), g_actions[c->b].id,
                    c->why ? " - intentional: " : "  <-- unintended",
                    c->why ? c->why : "");
        }
    }
    fprintf(out, "\n  Rebind in openpete.toml, e.g.\n"
                 "    [keys.host]\n"
                 "    fillpick = \"P\"      # bind a dev instrument\n"
                 "    rewind   = \"none\"   # unbind\n");
}

/* ------------------------------------------------------------------ */
/* mod bindings                                                       */
/* ------------------------------------------------------------------ */

/** @brief Store one mod binding at a rank, as a mod.toml default or a [keys.mod.<id>] override.
 *
 * The entry for (mod_id, name) is created on first use. A stored entry of
 * higher rank is kept; an equal rank is replaced by the later call. A
 * default whose key fails to parse is stored unbound, so the mod's
 * binding_down reads 0; an override that fails to parse stores nothing.
 * Every store recomputes the mouse:left scan.
 * @param mod_id the mod's id; NULL fails.
 * @param name the mod's declared binding name; NULL fails.
 * @param tokens the binding values as written.
 * @param n number of tokens.
 * @param rank the scope's precedence; 0 for a mod default.
 * @param is_default nonzero for the mod's own key.
 * @param where the config file or manifest named in error messages.
 * @return 0, or -1 on a parse failure, a NULL id or name, or allocation
 *         failure. */
static int km_set_mod(const char* mod_id, const char* name,
                      const char* const* tokens, int n, int rank,
                      int is_default, const char* where) {
    if (!mod_id || !name) return -1;
    km_source_t src[KM_MAX_SOURCES];
    int cnt = 0;
    char dom[48];
    snprintf(dom, sizeof dom, "mod.%s", mod_id);
    int parsed = km_parse_list(tokens, n, src, &cnt, where ? where : "config",
                               dom, name);
    if (parsed != 0 && !is_default) return -1;
    /* A mod key that fails to parse still registers, unbound, so the mod's
     * binding_down reads 0 the way an unbound config override does. */
    if (parsed != 0) cnt = 0;

    km_mod_bind_t* m = km_modbind_find(mod_id, name);
    if (m) {
        if (rank < m->rank) return parsed == 0 ? 0 : -1;
    } else {
        m = km_modbind_add(mod_id, name);
        if (!m) {
            OPL_WARN_F("keys", "out of memory registering %s.%s", mod_id, name);
            return -1;
        }
    }
    memcpy(m->src, src, sizeof src);
    m->n = cnt;
    m->rank = rank;
    m->is_default = (unsigned char)(is_default ? 1 : 0);
    km_recompute_left_bound();
    return parsed == 0 ? 0 : -1;
}

int openpete_keymap_set_mod(const char* mod_id, const char* name,
                            const char* const* tokens, int n,
                            int rank, const char* where) {
    return km_set_mod(mod_id, name, tokens, n, rank, 0, where);
}

int openpete_keymap_set_mod_default(const char* mod_id, const char* name,
                                    const char* key, const char* where) {
    const char* tok[1] = { key };
    return km_set_mod(mod_id, name, tok, 1, 0, 1, where);
}

void openpete_keymap_set_mod_enabled(const char* mod_id, int on) {
    if (!mod_id) return;
    for (int i = 0; i < g_modbind_n; i++)
        if (!strcmp(g_modbind[i].mod, mod_id))
            g_modbind[i].enabled = (unsigned char)(on ? 1 : 0);
    km_recompute_left_bound();
}

const char* openpete_keymap_mod_effective_key(const char* mod_id,
                                              const char* name,
                                              const char* fallback) {
    static char buf[80];
    for (int i = 0; i < g_modbind_n; i++) {
        if (strcmp(g_modbind[i].mod, mod_id) || strcmp(g_modbind[i].name, name))
            continue;
        if (g_modbind[i].n == 0) return "(unbound)";
        const km_source_t* s = &g_modbind[i].src[0];
        const char* nm = s_to_name ? s_to_name(s->kind, s->code) : NULL;
        if (km_is_mouse_kind(s->kind)) {
            const char* mn = km_mouse_name(s->kind, s->code);
            snprintf(buf, sizeof buf, "mouse:%s%s",
                     (s->kind == KM_SRC_MOUSEAXIS && s->invert < 0) ? "-" : "",
                     mn ? mn : "?");
        } else if (s->kind == KM_SRC_KEY)
            snprintf(buf, sizeof buf, "%s", nm ? nm : "?");
        else {
            char padpfx[8];
            if (s->device) snprintf(padpfx, sizeof padpfx, "pad%d:", s->device);
            else           snprintf(padpfx, sizeof padpfx, "pad:");
            snprintf(buf, sizeof buf, "%s%s%s", padpfx,
                     (s->kind == KM_SRC_PADAXIS && s->invert < 0) ? "-" : "",
                     nm ? nm : "?");
        }
        return buf;
    }
    return fallback;
}

int openpete_keymap_mod_binding(const char* mod_id, const char* name,
                                km_source_t* out, int cap, int* out_n) {
    if (!mod_id || !name) return 0;
    for (int i = 0; i < g_modbind_n; i++) {
        if (strcmp(g_modbind[i].mod, mod_id) || strcmp(g_modbind[i].name, name))
            continue;
        int n = g_modbind[i].n < cap ? g_modbind[i].n : cap;
        if (out) memcpy(out, g_modbind[i].src, sizeof(km_source_t) * (size_t)n);
        if (out_n) *out_n = n;
        return 1;
    }
    return 0;
}

const openpete_action_t* openpete_keymap_menu_pad_exempt(int* n) {
    /* The settings menu's own actions, and the two overlay toggles, which
     * stay live as under the dev overlay's suspension. */
    static const openpete_action_t kExempt[] = {
        OPK_MENU, OPK_DEVUI, OPK_MENU_UP, OPK_MENU_DOWN, OPK_MENU_LEFT,
        OPK_MENU_RIGHT, OPK_MENU_CLOSE, OPK_MENU_SELECT,
    };
    if (n) *n = (int)(sizeof kExempt / sizeof kExempt[0]);
    return kExempt;
}

/* ------------------------------------------------------------------ */
/* binding editor                                                     */
/* ------------------------------------------------------------------ */

/* The PsyCross field shape of every player-1 pad action. The pad stamp
 * (stamp_psx_pad_bindings) writes the fields by this table and the binding
 * editor admits only a binding that fits it, so the two read one rule. */
static const struct { openpete_action_t action; unsigned char shape; } g_pad_shapes[] = {
    { OPK_PAD_CROSS,       KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_CIRCLE,      KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_SQUARE,      KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_TRIANGLE,    KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_L1,          KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_L2,          KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_L3,          KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_R1,          KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_R2,          KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_R3,          KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_START,       KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_SELECT,      KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_DPAD_UP,     KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_DPAD_DOWN,   KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_DPAD_LEFT,   KM_PADSHAPE_KEY_AND_PAD },
    { OPK_PAD_DPAD_RIGHT,  KM_PADSHAPE_KEY_AND_PAD },
    /* The WASD stick directions share the gc_axis_left_* fields with each
     * other and with left_x / left_y, so they take a key only. */
    { OPK_PAD_STICK_UP,    KM_PADSHAPE_KEY_ONLY },
    { OPK_PAD_STICK_DOWN,  KM_PADSHAPE_KEY_ONLY },
    { OPK_PAD_STICK_LEFT,  KM_PADSHAPE_KEY_ONLY },
    { OPK_PAD_STICK_RIGHT, KM_PADSHAPE_KEY_ONLY },
    { OPK_PAD_LEFT_X,      KM_PADSHAPE_AXIS_ONLY },
    { OPK_PAD_LEFT_Y,      KM_PADSHAPE_AXIS_ONLY },
    { OPK_PAD_RIGHT_X,     KM_PADSHAPE_AXIS_ONLY },
    { OPK_PAD_RIGHT_Y,     KM_PADSHAPE_AXIS_ONLY },
};

km_pad_shape_t openpete_keymap_pad_shape(openpete_action_t a) {
    for (size_t i = 0; i < sizeof g_pad_shapes / sizeof g_pad_shapes[0]; i++)
        if (g_pad_shapes[i].action == a) return (km_pad_shape_t)g_pad_shapes[i].shape;
    return KM_PADSHAPE_NONE;
}

int openpete_keymap_action_info(openpete_action_t a,
                                openpete_keymap_action_info_t* out) {
    if ((unsigned)a >= OPK_ACTION_COUNT || !out) return 0;
    const km_action_def_t* d = &g_actions[a];
    out->id = d->id;
    out->purpose = d->purpose;
    out->domain = d->domain;
    out->kind = d->kind;
    out->ctx = d->ctx;
    out->flags = d->flags;
    return 1;
}

int openpete_keymap_find_action(const char* domain, const char* id) {
    return (domain && id) ? km_find(domain, id) : -1;
}

const char* openpete_keymap_domain_name(openpete_action_t a) {
    return (unsigned)a < OPK_ACTION_COUNT ? km_domain_name(g_actions[a].domain) : "host";
}

/** @brief Return whether an action is a host or player-1 pad action, the actions the binding page edits. */
static int km_editable(int a) {
    return a >= 0 && a < OPK_PAD2_CROSS;
}

/** @brief Return whether OPENPETE_REBIND_MUTATE_NO_CAPTURE_TERM drops the capture term from the neutral reads and the pad hold.
 *
 * The negative control of the capture harness legs; read once. */
static int km_mutate_no_capture_term(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_REBIND_MUTATE_NO_CAPTURE_TERM");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

/** @brief Find the actions contesting a source on an action under the conflict rule, with the index of each one's contesting source.
 *
 * The rule of openpete_keymap_conflicts. A km_hold_overlap pair is not a
 * conflict unless OPENPETE_REBIND_MUTATE_NO_V22 disables the rule.
 * @param a the action being edited.
 * @param src the candidate source.
 * @param out receives up to cap actions, or NULL.
 * @param out_slot receives each action's contesting source index, or NULL.
 * @param cap capacity of out and out_slot.
 * @return the number of conflicting actions. */
static int km_conflicts_ex(int a, const km_source_t* src, int* out, int* out_slot, int cap) {
    int n = 0;
    for (int b = 0; b < OPK_PAD2_CROSS; b++) {
        if (b == a || !km_ctx_overlap(g_actions[a].ctx, g_actions[b].ctx)) continue;
        const char* why = NULL;
        if (km_overlap_is_intentional(a, b, &why)) continue;
        for (int j = 0; j < g_bind[b].n; j++) {
            const km_source_t* bs = &g_bind[b].src[j];
            if (!km_src_same(bs, src)) continue;
            if (!km_mutate_no_v22() && km_hold_overlap(a, src, b, bs)) continue;
            if (out && n < cap) out[n] = b;
            if (out_slot && n < cap) out_slot[n] = j;
            n++;
            break;
        }
    }
    return n;
}

int openpete_keymap_conflicts(openpete_action_t a, const km_source_t* src,
                              openpete_action_t* out, int cap) {
    if (!km_editable((int)a) || !src) return 0;
    int tmp[OPK_ACTION_COUNT];
    const int n = km_conflicts_ex((int)a, src, tmp, NULL, OPK_ACTION_COUNT);
    for (int i = 0; out && i < n && i < cap; i++) out[i] = (openpete_action_t)tmp[i];
    return n;
}

/** @brief Write an action's live binding under the store lock.
 *
 * @param a the action.
 * @param src the sources.
 * @param n their count.
 * @param rank the rank stored with the binding.
 * @param origin the origin label stored with it.
 * @param saved the has-saved bit to store. */
static void km_write_live(int a, const km_source_t* src, int n, int rank,
                          const char* origin, int saved) {
    km_lock();
    km_binding_t* b = &g_bind[a];
    if (strcmp(b->origin, origin) != 0)
        snprintf(b->shadowed, sizeof b->shadowed, "%s", b->origin);
    memset(b->src, 0, sizeof b->src);
    memcpy(b->src, src, sizeof(km_source_t) * (size_t)n);
    b->n = n;
    b->rank = rank;
    b->unbound_by_config = n == 0;
    snprintf(b->origin, sizeof b->origin, "%s", origin);
    memset(b->hold_since, 0, sizeof b->hold_since);
    memset(b->hold_on, 0, sizeof b->hold_on);
    b->has_saved = (unsigned char)(saved ? 1 : 0);
    km_unlock();
}

/** @brief Return the rank a live edit stores: the saved layer's, or the stored rank before the bounds are set. */
static int km_saved_rank(int a) {
    return g_bound_saved != INT_MAX ? g_bound_saved : g_bind[a].rank;
}

int openpete_keymap_set_live(openpete_action_t a, const km_source_t* src, int n) {
    if ((unsigned)a >= OPK_ACTION_COUNT || n < 0 || n > KM_MAX_SOURCES || (n && !src))
        return -1;
    km_write_live((int)a, src, n, km_saved_rank((int)a), "saved", 1);
    (void)openpete_keymap_resolve();
    return 0;
}

int openpete_keymap_has_saved(openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return 0;
    km_lock();
    const int v = g_bind[a].has_saved;
    km_unlock();
    return v;
}

void openpete_keymap_clear_saved(openpete_action_t a) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return;
    km_lock();
    g_bind[a].has_saved = 0;
    km_unlock();
}

int openpete_keymap_below_saved(openpete_action_t a, km_source_t* out, int cap,
                                char* origin, size_t origin_cap) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return -1;
    km_lock();
    const km_binding_t* b = &g_bind[a];
    const int n = b->below_n < cap ? b->below_n : cap;
    if (out && n > 0) memcpy(out, b->below_src, sizeof(km_source_t) * (size_t)n);
    if (origin && origin_cap) snprintf(origin, origin_cap, "%s", b->below_origin);
    km_unlock();
    return n;
}

int openpete_keymap_global_layer(openpete_action_t a, km_source_t* out, int cap,
                                 int* scoped) {
    if ((unsigned)a >= OPK_ACTION_COUNT) return -1;
    km_lock();
    const km_binding_t* b = &g_bind[a];
    const int n = b->glob_n < cap ? b->glob_n : cap;
    if (out && n > 0) memcpy(out, b->glob_src, sizeof(km_source_t) * (size_t)n);
    if (scoped) *scoped = b->scoped;
    km_unlock();
    return n;
}

/* ---- admission ------------------------------------------------------- */

/** @brief Return whether two sources are the same in every field (kind, code, invert, device, hold). */
static int km_src_equal(const km_source_t* x, const km_source_t* y) {
    return x->kind == y->kind && x->code == y->code && x->invert == y->invert &&
           x->device == y->device && x->hold == y->hold;
}

/** @brief Return whether a source list holds a controller source (pad button or pad axis). */
static int km_has_pad_source(const km_source_t* src, int n) {
    for (int i = 0; i < n; i++)
        if (src[i].kind == KM_SRC_PADBTN || src[i].kind == KM_SRC_PADAXIS) return 1;
    return 0;
}

/** @brief Test that a source survives a saved row: its token parses back to the same source for the action.
 *
 * The token is the one a [saved.keys] row stores, read back by the [keys]
 * parser for the action's domain, so the parser's refusals (a hold: on a
 * pad action, a mouse source on a pad action, a pulse on a level action)
 * are the binding page's refusals.
 * @param a the action.
 * @param s the source.
 * @param why receives the refusal reason.
 * @param cap capacity of why.
 * @return 1 when admitted, 0 when refused. */
static int km_admit_token(int a, const km_source_t* s, char* why, size_t cap) {
    char tok[64];
    openpete_keymap_source_token(s, tok, sizeof tok);
    km_source_t back[KM_MAX_SOURCES];
    int n = 0;
    const char* tp = tok;
    g_parse_err[0] = '\0';
    g_parse_quiet = 1;
    const int r = km_parse_list(&tp, 1, back, &n, "rebind",
                                km_domain_name(g_actions[a].domain), g_actions[a].id);
    g_parse_quiet = 0;
    if (r != 0) {
        snprintf(why, cap, "%s", g_parse_err[0] ? g_parse_err : "this input cannot be bound here");
        return 0;
    }
    if (n != 1 || !km_src_equal(&back[0], s)) {
        snprintf(why, cap, "this key cannot be saved ('%s' does not read back)", tok);
        return 0;
    }
    return 1;
}

/** @brief Test a draft binding of a player-1 pad action against its PsyCross field shape (V18: one key and one controller input; a key only; one stick axis only).
 *
 * @param a the action; any other action passes.
 * @param src the draft sources.
 * @param n their count.
 * @param why receives the refusal reason.
 * @param cap capacity of why.
 * @return 1 when admitted, 0 when refused. */
static int km_admit_shape(int a, const km_source_t* src, int n, char* why, size_t cap) {
    int keys = 0, pads = 0;
    for (int i = 0; i < n; i++) {
        keys += src[i].kind == KM_SRC_KEY;
        pads += src[i].kind == KM_SRC_PADBTN || src[i].kind == KM_SRC_PADAXIS;
    }
    const char* id = g_actions[a].id;
    switch (openpete_keymap_pad_shape((openpete_action_t)a)) {
    case KM_PADSHAPE_KEY_AND_PAD:
        if (keys <= 1 && pads <= 1) return 1;
        snprintf(why, cap, "pad.%s takes one key and one controller input", id);
        return 0;
    case KM_PADSHAPE_KEY_ONLY:
        if (!pads && keys <= 1) return 1;
        snprintf(why, cap, "pad.%s takes one keyboard key only", id);
        return 0;
    case KM_PADSHAPE_AXIS_ONLY:
        if (n == 0 || (n == 1 && src[0].kind == KM_SRC_PADAXIS)) return 1;
        snprintf(why, cap, "pad.%s takes one controller stick axis only", id);
        return 0;
    default:
        return 1;
    }
}

/** @brief Test one source against the settings menu's mouse rules, the sources openpete_keymap_resolve warns about.
 *
 * @param a the action.
 * @param s the source; a non-mouse source passes.
 * @param why receives the refusal reason.
 * @param cap capacity of why.
 * @return 1 when admitted, 0 when refused. */
static int km_admit_mouse(int a, const km_source_t* s, char* why, size_t cap) {
    if (!km_is_mouse_kind(s->kind)) return 1;
    if (g_actions[a].ctx == KM_CTX_MENU && a != OPK_MENU_CLOSE) {
        snprintf(why, cap, "the settings menu owns the mouse while it is open");
        return 0;
    }
    const int gesture = s->kind == KM_SRC_MOUSEBTN &&
        (s->code == KM_MOUSE_LEFT || s->code == KM_MOUSE_RIGHT ||
         (a == OPK_MENU && KM_MOUSE_IS_PULSE(s->code)));
    if ((a == OPK_MENU || a == OPK_MENU_CLOSE) && gesture) {
        snprintf(why, cap, "that mouse input is a settings-menu gesture");
        return 0;
    }
    return 1;
}

/** @brief Test a whole draft binding against the PsyCross field shape, the settings-menu mouse rules and duplicates.
 *
 * @param a the action.
 * @param src the draft sources.
 * @param n their count.
 * @param why receives the refusal reason.
 * @param cap capacity of why.
 * @return 1 when admitted, 0 when refused. */
static int km_admit_list(int a, const km_source_t* src, int n, char* why, size_t cap) {
    for (int i = 0; i < n; i++)
        for (int j = 0; j < i; j++)
            if (km_src_same(&src[i], &src[j]) && src[i].hold == src[j].hold) {
                snprintf(why, cap, "%s already has that input", g_actions[a].id);
                return 0;
            }
    if (!km_admit_shape(a, src, n, why, cap)) return 0;
    for (int i = 0; i < n; i++)
        if (!km_admit_mouse(a, &src[i], why, cap)) return 0;
    return 1;
}

/* One action's binding as an edit would leave it. */
typedef struct { int action; km_source_t src[KM_MAX_SOURCES]; int n; } km_draft_t;

/** @brief Refuse an edit that would leave `menu` or `devui` without a controller source it had.
 *
 * A pad-only player reaches the settings menu and the overlay only through
 * those sources.
 * @param d the drafts.
 * @param nd their count.
 * @param why receives the refusal reason.
 * @param cap capacity of why.
 * @return 1 when admitted, 0 when refused. */
static int km_admit_reach(const km_draft_t* d, int nd, char* why, size_t cap) {
    for (int i = 0; i < nd; i++) {
        const int a = d[i].action;
        if (a != OPK_MENU && a != OPK_DEVUI) continue;
        if (km_has_pad_source(g_bind[a].src, g_bind[a].n) &&
            !km_has_pad_source(d[i].src, d[i].n)) {
            snprintf(why, cap, "%s would lose its last controller button; a "
                     "controller-only player opens %s with it", g_actions[a].id,
                     a == OPK_MENU ? "the settings menu" : "this overlay");
            return 0;
        }
    }
    return 1;
}

/** @brief Write every draft as a saved binding, rerun the resolve scans and record the actions in the edit result.
 *
 * @param d the drafts; [0] is the edited action.
 * @param nd their count.
 * @param e the edit result; its changed list and pad flag are filled. */
static void km_apply_drafts(const km_draft_t* d, int nd, openpete_keymap_edit_t* e) {
    for (int i = 0; i < nd; i++) {
        const int a = d[i].action;
        km_write_live(a, d[i].src, d[i].n, km_saved_rank(a), "saved", 1);
        if (i < KM_EDIT_MAX_CHANGED) e->changed[i].action = a;
        if (g_actions[a].domain == KM_DOM_PAD) e->pad_changed = 1;
    }
    e->n_changed = nd < KM_EDIT_MAX_CHANGED ? nd : KM_EDIT_MAX_CHANGED;
    (void)openpete_keymap_resolve();
}

/** @brief Fill an edit result as a refusal. */
static int km_refuse(openpete_keymap_edit_t* e, const char* why) {
    e->kind = KM_EDIT_REFUSED;
    snprintf(e->reason, sizeof e->reason, "%s", why);
    return -1;
}

/** @brief Write a captured source into an action, swapping it away from every action it conflicts with.
 *
 * A replace puts the candidate in the slot (keeping the slot's hold flag)
 * and offers the slot's old source to each conflicting action in place of
 * its contesting source, when that action admits it; otherwise, and on an
 * add, the conflicting action loses the contesting source. A plain
 * controller button captured onto a host action is refused when a pad
 * action holds it: there it binds only as a hold. The edit is refused when
 * it would leave `menu` or `devui` without a controller source.
 * @param a the action.
 * @param slot the slot to replace, or -1 to add.
 * @param cand the candidate source.
 * @param e receives the result.
 * @return 0 when committed, -1 when refused. */
static int km_commit(int a, int slot, km_source_t cand, openpete_keymap_edit_t* e) {
    char why[192];
    km_draft_t d[KM_EDIT_MAX_CHANGED];
    memset(d, 0, sizeof d);
    const km_binding_t* ba = &g_bind[a];
    d[0].action = a;
    memcpy(d[0].src, ba->src, sizeof ba->src);
    d[0].n = ba->n;
    km_source_t displaced;
    int have_displaced = 0;
    if (slot >= 0 && slot < ba->n) {
        displaced = ba->src[slot];
        have_displaced = 1;
        cand.hold = displaced.hold;
        d[0].src[slot] = cand;
    } else {
        if (ba->n >= KM_MAX_SOURCES) {
            snprintf(why, sizeof why, "%s already has %d inputs", g_actions[a].id,
                     KM_MAX_SOURCES);
            return km_refuse(e, why);
        }
        d[0].src[d[0].n++] = cand;
    }
    openpete_keymap_source_token(&cand, e->token, sizeof e->token);
    if (!km_admit_token(a, &cand, why, sizeof why) ||
        !km_admit_list(a, d[0].src, d[0].n, why, sizeof why))
        return km_refuse(e, why);

    int bs[OPK_ACTION_COUNT], bslot[OPK_ACTION_COUNT];
    const int nb = km_conflicts_ex(a, &cand, bs, bslot, OPK_ACTION_COUNT);
    int nd = 1;
    for (int i = 0; i < nb; i++) {
        const int b = bs[i];
        const km_source_t* old = &g_bind[b].src[bslot[i]];
        if (g_actions[a].domain == KM_DOM_HOST && !cand.hold &&
            km_src_is_pad_button(&cand) && g_actions[b].domain != KM_DOM_HOST)
            return km_refuse(e, "a PlayStation button binds here only as a hold "
                                "(use + hold)");
        if (nd >= KM_EDIT_MAX_CHANGED)
            return km_refuse(e, "too many actions share that input");
        km_draft_t* db = &d[nd];
        db->action = b;
        memcpy(db->src, g_bind[b].src, sizeof db->src);
        db->n = g_bind[b].n;
        openpete_keymap_source_token(old, e->changed[nd].old_tok, sizeof e->changed[nd].old_tok);
        int placed = 0;
        if (have_displaced) {
            km_draft_t trial = *db;
            trial.src[bslot[i]] = displaced;
            char bwhy[192];
            if (km_admit_token(b, &displaced, bwhy, sizeof bwhy) &&
                km_admit_list(b, trial.src, trial.n, bwhy, sizeof bwhy)) {
                *db = trial;
                placed = 1;
                openpete_keymap_source_token(&displaced, e->changed[nd].new_tok,
                                             sizeof e->changed[nd].new_tok);
            }
        }
        if (!placed) {
            memmove(&db->src[bslot[i]], &db->src[bslot[i] + 1],
                    sizeof(km_source_t) * (size_t)(db->n - bslot[i] - 1));
            db->n--;
            snprintf(e->changed[nd].new_tok, sizeof e->changed[nd].new_tok, "none");
        }
        nd++;
    }
    if (!km_admit_reach(d, nd, why, sizeof why)) return km_refuse(e, why);
    km_apply_drafts(d, nd, e);
    e->kind = KM_EDIT_COMMITTED;
    return 0;
}

int openpete_keymap_remove_source(openpete_action_t a, int slot,
                                  openpete_keymap_edit_t* out) {
    openpete_keymap_edit_t e;
    memset(&e, 0, sizeof e);
    e.action = (int)a;
    int rc = 0;
    if (!km_editable((int)a) || slot < 0 || slot >= g_bind[a].n) {
        rc = km_refuse(&e, "no such input");
    } else {
        km_draft_t d;
        char why[192];
        d.action = (int)a;
        memcpy(d.src, g_bind[a].src, sizeof d.src);
        d.n = g_bind[a].n;
        openpete_keymap_source_token(&d.src[slot], e.token, sizeof e.token);
        memmove(&d.src[slot], &d.src[slot + 1],
                sizeof(km_source_t) * (size_t)(d.n - slot - 1));
        d.n--;
        if (!km_admit_reach(&d, 1, why, sizeof why)) {
            rc = km_refuse(&e, why);
        } else {
            km_apply_drafts(&d, 1, &e);
            e.kind = KM_EDIT_REMOVED;
        }
    }
    if (out) *out = e;
    return rc;
}

int openpete_keymap_restore(openpete_action_t a, openpete_keymap_edit_t* out) {
    openpete_keymap_edit_t e;
    memset(&e, 0, sizeof e);
    e.action = (int)a;
    if (!km_editable((int)a)) {
        km_refuse(&e, "no such action");
        if (out) *out = e;
        return -1;
    }
    const km_binding_t* b = &g_bind[a];
    km_source_t src[KM_MAX_SOURCES];
    char origin[KM_ORIGIN_MAX];
    memcpy(src, b->below_src, sizeof src);
    snprintf(origin, sizeof origin, "%s", b->below_origin);
    km_write_live((int)a, src, b->below_n, b->below_rank, origin, 0);
    (void)openpete_keymap_resolve();
    e.kind = KM_EDIT_RESTORED;
    e.n_changed = 1;
    e.changed[0].action = (int)a;
    e.pad_changed = g_actions[a].domain == KM_DOM_PAD;
    snprintf(e.token, sizeof e.token, "%s", origin);
    if (out) *out = e;
    return 0;
}

/* ---- capture --------------------------------------------------------- */

static int                s_cap_state = KM_CAP_IDLE;
static int                s_cap_action = -1;
static int                s_cap_slot = -1;     /* -1 = add */
static int                s_cap_hold;
static unsigned long long s_cap_t0;
static km_source_t        s_cap_cand;
static long long          s_cap_wheel0;
static long long          s_cap_motion0[KM_MOUSE_AXIS_COUNT];
/* Set when a capture ends; capture_active stays true until the inputs it
 * latched are released. */
static int                s_cap_tail;
static openpete_keymap_edit_t s_cap_result;
static int                s_cap_result_ready;
static _Atomic unsigned   s_cap_word;
static _Atomic int        s_cap_active;
/* The prompt's rect, as float bit patterns (x0, y0, x1, y1). */
static _Atomic unsigned   s_cap_rect[4];

/** @brief Publish the capture word and the capture_active flag from the host loop's capture state. */
static void km_cap_publish(void) {
    unsigned w = 0;
    if (s_cap_state != KM_CAP_IDLE)
        w = (unsigned)s_cap_state |
            ((unsigned)(s_cap_slot < 0 ? KM_CAPWORD_ADD : s_cap_slot) << 4) |
            ((unsigned)(s_cap_hold ? 1 : 0) << 8) |
            ((unsigned)s_cap_action << 16);
    atomic_store_explicit(&s_cap_word, w, memory_order_release);
    int act = s_cap_state != KM_CAP_IDLE;
    if (!act && s_cap_tail) {
        if (km_latch_any()) act = 1;
        else s_cap_tail = 0;
    }
    atomic_store_explicit(&s_cap_active, act, memory_order_release);
}

unsigned openpete_keymap_capture_word(void) {
    return atomic_load_explicit(&s_cap_word, memory_order_acquire);
}

int openpete_keymap_capture_active(void) {
    return atomic_load_explicit(&s_cap_active, memory_order_acquire);
}

int openpete_keymap_capture_neutral(void) {
    return openpete_keymap_capture_active() && !km_mutate_no_capture_term();
}

void openpete_keymap_capture_set_rect(float x0, float y0, float x1, float y1) {
    const float v[4] = { x0, y0, x1, y1 };
    for (int i = 0; i < 4; i++) {
        unsigned u;
        memcpy(&u, &v[i], sizeof u);
        atomic_store_explicit(&s_cap_rect[i], u, memory_order_relaxed);
    }
}

/** @brief Return whether the pointer is inside the published prompt rect; an empty rect or an unknown position is outside. */
static int km_cap_pointer_inside(void) {
    float r[4];
    for (int i = 0; i < 4; i++) {
        const unsigned u = atomic_load_explicit(&s_cap_rect[i], memory_order_relaxed);
        memcpy(&r[i], &u, sizeof r[i]);
    }
    if (!(r[2] > r[0]) || !(r[3] > r[1])) return 0;
    float x = 0.0f, y = 0.0f;
    if (!s_in.mouse_pos || !s_in.mouse_pos(&x, &y)) return 0;
    return x >= r[0] && x < r[2] && y >= r[1] && y < r[3];
}

/** @brief Return the Escape scancode by name, so this file stays free of SDL's numbering; -1 when the lookup has no Escape. */
static int km_escape_code(void) {
    return s_to_code ? s_to_code(KM_SRC_KEY, "Escape") : -1;
}

/** @brief Record the wheel and motion totals the capture measures notches and motion from. */
static void km_cap_baseline(void) {
    s_cap_wheel0 = s_in.mouse_wheel_total ? s_in.mouse_wheel_total() : 0;
    for (int ax = 0; ax < KM_MOUSE_AXIS_COUNT; ax++)
        s_cap_motion0[ax] = s_in.mouse_motion_total ? s_in.mouse_motion_total(ax) : 0;
}

/** @brief End the capture: latch every held input, go idle, store the result and publish. */
static void km_cap_end(const openpete_keymap_edit_t* e) {
    km_latch_held();
    s_cap_state = KM_CAP_IDLE;
    s_cap_tail = 1;
    s_cap_result = *e;
    s_cap_result_ready = 1;
    km_cap_publish();
}

int openpete_keymap_capture_begin(openpete_action_t a, int slot, int hold,
                                  openpete_keymap_edit_t* refused) {
    openpete_keymap_edit_t e;
    memset(&e, 0, sizeof e);
    e.action = (int)a;
    int rc = 0;
    if (!km_editable((int)a))
        rc = km_refuse(&e, "only host and player 1 actions are edited here");
    else if (s_cap_state != KM_CAP_IDLE)
        rc = km_refuse(&e, "a capture is already running");
    else {
        if (slot >= g_bind[a].n) slot = -1;
        if (slot < 0 && g_bind[a].n >= KM_MAX_SOURCES) {
            char why[96];
            snprintf(why, sizeof why, "%s already has %d inputs", g_actions[a].id,
                     KM_MAX_SOURCES);
            rc = km_refuse(&e, why);
        } else if (hold && slot < 0 && (g_actions[a].domain != KM_DOM_HOST ||
                                         g_actions[a].kind != KM_DIGITAL)) {
            rc = km_refuse(&e, "a hold: source binds a host button action only");
        }
    }
    if (rc != 0) {
        if (refused) *refused = e;
        return -1;
    }
    s_cap_action = (int)a;
    s_cap_slot = slot;
    s_cap_hold = slot < 0 && hold ? 1 : 0;
    s_cap_t0 = s_in.now_ms ? s_in.now_ms() : 0;
    s_cap_state = KM_CAP_WAITING;
    memset(&s_cap_cand, 0, sizeof s_cap_cand);
    km_cap_baseline();
    km_cap_publish();
    return 0;
}

void openpete_keymap_capture_cancel(const char* cause) {
    if (s_cap_state == KM_CAP_IDLE) return;
    openpete_keymap_edit_t e;
    memset(&e, 0, sizeof e);
    e.kind = KM_EDIT_CANCELLED;
    e.action = s_cap_action;
    snprintf(e.reason, sizeof e.reason, "%s", cause ? cause : "cancelled");
    km_cap_end(&e);
}

int openpete_keymap_capture_take_result(openpete_keymap_edit_t* out) {
    if (!s_cap_result_ready) return 0;
    if (out) *out = s_cap_result;
    s_cap_result_ready = 0;
    return 1;
}

/** @brief Return whether any input of any kind is held and not latched; Escape, which is latched while a capture runs, does not count. */
static int km_cap_any_held(void) {
    static const int kinds[] = { KM_SRC_KEY, KM_SRC_PADBTN, KM_SRC_PADAXIS, KM_SRC_MOUSEBTN };
    for (size_t k = 0; k < sizeof kinds / sizeof kinds[0]; k++) {
        int bits = 0;
        (void)km_lat_set_of(kinds[k], &bits);
        for (int c = 0; c < bits; c++)
            if (km_raw_held(kinds[k], c) && !km_consumed(kinds[k], c)) return 1;
    }
    return 0;
}

/** @brief Commit the candidate into the captured action and end the capture. */
static void km_cap_commit(void) {
    openpete_keymap_edit_t e;
    memset(&e, 0, sizeof e);
    e.action = s_cap_action;
    km_source_t cand = s_cap_cand;
    if (s_cap_hold) cand.hold = 1;
    (void)km_commit(s_cap_action, s_cap_slot, cand, &e);
    km_cap_end(&e);
}

/** @brief Take a candidate; a source with no release (wheel notch, mouse motion) commits at once. */
static void km_cap_take(int kind, int code, int invert, int immediate) {
    memset(&s_cap_cand, 0, sizeof s_cap_cand);
    s_cap_cand.kind = (unsigned char)kind;
    s_cap_cand.code = (short)code;
    s_cap_cand.invert = (signed char)invert;
    if (immediate) km_cap_commit();
    else s_cap_state = KM_CAP_CANDIDATE;
}

/** @brief Scan the keys and the pad for the first new press; a stick direction is a candidate only on an axis action.
 *
 * @return 1 when a candidate was taken, 0 otherwise. */
static int km_cap_scan_buttons(void) {
    const int esc = km_escape_code();
    for (int c = 0; c < KM_SCAN_KEYS; c++)
        if (c != esc && km_raw_held(KM_SRC_KEY, c) && !km_consumed(KM_SRC_KEY, c)) {
            km_cap_take(KM_SRC_KEY, c, 0, 0);
            return 1;
        }
    for (int c = 0; c < KM_SCAN_PADBTNS; c++)
        if (km_raw_held(KM_SRC_PADBTN, c) && !km_consumed(KM_SRC_PADBTN, c)) {
            km_cap_take(KM_SRC_PADBTN, c, 0, 0);
            return 1;
        }
    for (int c = 0; c < KM_SCAN_PADAXES; c++) {
        if (!km_raw_held(KM_SRC_PADAXIS, c) || km_consumed(KM_SRC_PADAXIS, c)) continue;
        const int v = s_in.pad_axis(c);
        /* A trigger is a button press; a stick throw binds only an axis
         * action and is ignored elsewhere, so a twitch does not end the
         * capture. The throw's sign is the binding's direction. */
        if (km_axis_is_trigger(c)) {
            km_cap_take(KM_SRC_PADAXIS, c, 1, 0);
            return 1;
        }
        if (g_actions[s_cap_action].kind == KM_AXIS) {
            km_cap_take(KM_SRC_PADAXIS, c, v < 0 ? -1 : 1, 0);
            return 1;
        }
    }
    return 0;
}

/** @brief Scan the mouse: a button inside the prompt is a candidate and one outside cancels, a wheel notch and motion on an axis action commit at once.
 *
 * @return 1 when the capture took a candidate or ended, 0 otherwise. */
static int km_cap_scan_mouse(void) {
    for (int c = KM_MOUSE_LEFT; c <= KM_MOUSE_X2; c++) {
        if (!km_raw_held(KM_SRC_MOUSEBTN, c) || km_consumed(KM_SRC_MOUSEBTN, c)) continue;
        if (km_cap_pointer_inside()) km_cap_take(KM_SRC_MOUSEBTN, c, 0, 0);
        else openpete_keymap_capture_cancel("click outside the prompt");
        return 1;
    }
    const long long wheel = s_in.mouse_wheel_total ? s_in.mouse_wheel_total() : 0;
    const long long notches = km_floordiv_unit(wheel) - km_floordiv_unit(s_cap_wheel0);
    if (notches != 0) {
        km_cap_take(KM_SRC_MOUSEBTN, notches > 0 ? KM_MOUSE_WHEEL_UP : KM_MOUSE_WHEEL_DOWN, 0, 1);
        return 1;
    }
    for (int ax = 0; ax < KM_MOUSE_AXIS_COUNT; ax++) {
        const long long t = s_in.mouse_motion_total ? s_in.mouse_motion_total(ax) : 0;
        const long long dm = t - s_cap_motion0[ax];
        if (dm <= KM_CAPTURE_MOTION && dm >= -KM_CAPTURE_MOTION) continue;
        if (g_actions[s_cap_action].kind != KM_AXIS) {
            s_cap_motion0[ax] = t;   /* ignored, not refused */
            continue;
        }
        km_cap_take(KM_SRC_MOUSEAXIS, ax, dm < 0 ? -1 : 1, 1);
        return 1;
    }
    return 0;
}

/** @brief Watch a held candidate: its release commits, a mouse press outside the prompt cancels. */
static void km_cap_watch(void) {
    for (int c = KM_MOUSE_LEFT; c <= KM_MOUSE_X2; c++) {
        if (s_cap_cand.kind == KM_SRC_MOUSEBTN && s_cap_cand.code == c) continue;
        if (km_raw_held(KM_SRC_MOUSEBTN, c) && !km_consumed(KM_SRC_MOUSEBTN, c) &&
            !km_cap_pointer_inside()) {
            openpete_keymap_capture_cancel("click outside the prompt");
            return;
        }
    }
    if (!km_raw_held(s_cap_cand.kind, s_cap_cand.code)) km_cap_commit();
}

/** @brief Advance the capture by one poll: the Escape latch, the timeout, then the state's own step. */
static void km_capture_step(void) {
    if (s_cap_state != KM_CAP_IDLE) {
        /* Escape is never a candidate: the event handler posts the cancel,
         * and the key reads consumed here so a poll that runs before that
         * cancel is drained does not fire Escape's binding. */
        const int esc = km_escape_code();
        if (esc >= 0 && km_raw_held(KM_SRC_KEY, esc))
            openpete_keymap_consume_input(KM_SRC_KEY, esc);
        const unsigned long long now = s_in.now_ms ? s_in.now_ms() : 0;
        if (s_in.now_ms && s_cap_state != KM_CAP_CANDIDATE &&
            now - s_cap_t0 >= KM_CAPTURE_TIMEOUT_MS) {
            openpete_keymap_capture_cancel("timeout");
        } else if (s_cap_state == KM_CAP_WAITING) {
            if (!km_cap_any_held()) s_cap_state = KM_CAP_ARMED;
            km_cap_baseline();
        } else if (s_cap_state == KM_CAP_ARMED) {
            if (!km_cap_scan_buttons()) (void)km_cap_scan_mouse();
        } else if (s_cap_state == KM_CAP_CANDIDATE) {
            km_cap_watch();
        }
    }
    km_cap_publish();
}
