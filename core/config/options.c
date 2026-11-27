// core/config/options.c - command-line flag parser.
//
// Unknown flags exit with OPENPETE_EXIT_SETUP_ERROR (3).

#include "openpete.h"
#include "openpete_config.h"  /* openpete.toml - the layered config file */
#include "openpete_knobs.h"   /* knob_flag_arg_t - the help-only table's arity */
#include "config_schema.h"    /* config key of a flag, for the fallback warning */
#include "openpete_keymap.h"  /* [keys] - install the SDL name lookups */
#include "openpete_overlay_anchor.h"  /* --perf-position / --osd-position spellings */
#include "openpete_osd.h"             /* OPENPETE_OSD_TIMEOUT_MAX_MS */
#include "openpete_log.h"
#include "openpete_time.h"  /* OP_TICK_HZ - --record-cfr NTSC-multiple snap */
#include "game_extras.h"  /* game_resolve_level_arg (--skip-to-level names),
                           * game_validate_cheats_arg (--cheats names) */

#include "openpete_paths.h" /* where --bk2-record output is allowed to live */
#include "openpete_pose_text.h" /* --freecam-pose grammar */
#include "openpete_docs_url.h" /* OPENPETE_DOCS_URL - the --help footer */
#include "openpete_stick_response.h" /* --stick-response values */
#include "openpete_render_res.h"  /* --resolution grammar */

#include <SDL3/SDL.h>     /* SDL_CreateDirectory - --bk2-record output dir */

#include <ctype.h>      /* tolower - case-insensitive disc-image extensions */
#include <math.h>       /* lround/fabs - --record-cfr NTSC-multiple validation */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>       /* time/localtime/strftime - default --bk2-record filename */

/** @brief Fetch the token after argv[i] as the value of flag, or report it missing.
 *
 * Prints "openpete: missing value for <flag>" to stderr when argv[i] is the
 * last token. The value is taken verbatim: a token that starts with '-' is
 * accepted.
 * @param flag The flag's spelling, for the message.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param i Index of the flag in argv.
 * @param out Receives argv[i + 1] on success; untouched on failure.
 * @return 0 on success, -1 when no token follows. */
static int needs_value(const char* flag, int argc, char** argv, int i, const char** out) {
    if (i + 1 >= argc) {
        fprintf(stderr, "openpete: missing value for %s\n", flag);
        return -1;
    }
    *out = argv[i + 1];
    return 0;
}

/** @brief Parse a flag's value as a finite number and clamp it to [lo, hi].
 *
 * The whole value must parse (strtod with an end-pointer check) and be
 * finite, so "nan", "inf" and "9O" are refused rather than reaching a
 * clamp that NaN would pass through. With zero_ok, exactly 0 is stored as 0 unclamped (the option's
 * "off" or "the game's own" value). Any other value outside [lo, hi] is
 * clamped with a message naming the flag.
 * @param flag The flag's spelling, for the messages.
 * @param v The value text.
 * @param lo Lower bound.
 * @param hi Upper bound.
 * @param zero_ok Whether 0 passes unclamped.
 * @param out Receives the clamped value.
 * @return 0 on success, -1 with a message on a refused value. */
static int parse_finite_clamped(const char* flag, const char* v, float lo, float hi,
                                bool zero_ok, float* out) {
    char* end = NULL;
    double d = strtod(v, &end);
    if (!v[0] || end == v || *end || !isfinite(d)) {
        fprintf(stderr, "openpete: %s expects a number in [%g,%g], got: %s\n",
                flag, (double)lo, (double)hi, v);
        return -1;
    }
    if (zero_ok && d == 0.0) {
        *out = 0.0f;
        return 0;
    }
    if (d < lo || d > hi) {
        fprintf(stderr, "openpete: %s clamped to [%g,%g]; got %.2f\n",
                flag, (double)lo, (double)hi, d);
        d = d < lo ? lo : hi;
    }
    *out = (float)d;
    return 0;
}

/** @brief Parse a --tick-scale value: a finite positive number, clamped to the tick scale's range.
 *
 * The whole value must parse (strtod with an end-pointer check). A value
 * that is not a number, not finite, or not above 0 is refused; a positive
 * value outside [OPENPETE_TICK_SCALE_MIN, OPENPETE_TICK_SCALE_MAX] is
 * clamped with a message, as openpete_set_tick_scale clamps the live knob.
 * @param flag The flag's spelling, for the messages.
 * @param v The value text.
 * @param out Receives the clamped factor; untouched on a refusal.
 * @return 0 on success, -1 with a message on a refused value. */
static int parse_tick_scale(const char* flag, const char* v, double* out) {
    char* end = NULL;
    double d = strtod(v, &end);
    if (!v[0] || end == v || *end || !isfinite(d) || !(d > 0.0)) {
        fprintf(stderr, "openpete: %s expects a positive number in [%g,%g] (e.g. 0.5), got: %s\n",
                flag, OPENPETE_TICK_SCALE_MIN, OPENPETE_TICK_SCALE_MAX, v);
        return -1;
    }
    if (d < OPENPETE_TICK_SCALE_MIN || d > OPENPETE_TICK_SCALE_MAX) {
        fprintf(stderr, "openpete: %s clamped to [%g,%g]; got %g\n",
                flag, OPENPETE_TICK_SCALE_MIN, OPENPETE_TICK_SCALE_MAX, d);
        d = d < OPENPETE_TICK_SCALE_MIN ? OPENPETE_TICK_SCALE_MIN : OPENPETE_TICK_SCALE_MAX;
    }
    *out = d;
    return 0;
}

/** @brief Parse a window-size value: WxH or maximized.
 *
 * @param name The flag or env var, for the message.
 * @param v The value.
 * @param w Receives the width; 0 for maximized.
 * @param h Receives the height; 0 for maximized.
 * @param maximized Receives 1 for maximized, else 0.
 * @return 0, or -1 after printing why the value is refused (nothing is written). */
static int parse_window_value(const char* name, const char* v, int* w, int* h, int* maximized) {
    if (strcmp(v, "maximized") == 0) {
        *w = *h = 0;
        *maximized = 1;
        return 0;
    }
    int pw = 0, ph = 0;
    if (sscanf(v, "%dx%d", &pw, &ph) != 2 || pw <= 0 || ph <= 0) {
        fprintf(stderr, "openpete: %s expects WxH (e.g. 1280x720) or maximized, got: %s\n",
                name, v);
        return -1;
    }
    *w = pw;
    *h = ph;
    *maximized = 0;
    return 0;
}

/** @brief Parse a --sfx, --music or --vibration value on the game's 0..10 slider scale.
 *
 * "off" is 0 and "on" is 10; -1 means the game's own setting stands. On
 * success writes the level to *out and returns 0; otherwise prints the
 * accepted vocabulary and returns -1.
 * @param flag The flag or env var name, for the message.
 * @param v The value text.
 * @param out Receives the level. */
static int parse_volume_arg(const char* flag, const char* v, int* out) {
    if (strcmp(v, "off") == 0)      { *out = 0;  return 0; }
    if (strcmp(v, "on")  == 0)      { *out = 10; return 0; }
    char* end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || *end != '\0' || n < -1 || n > 10) {
        /* "setting", not "slider": this parser is shared with --vibration,
         * which is a toggle plus a host-side strength rather than an in-game
         * slider. */
        fprintf(stderr, "openpete: %s expects 0..10 (on|off also accepted, "
                        "-1 = leave the game's own setting alone), got: %s\n",
                flag, v);
        return -1;
    }
    *out = (int)n;
    return 0;
}

/** @brief Tokenize an args-file buffer in place.
 *
 * Whitespace separates tokens and '#' starts a comment that runs to the end
 * of the line. A double-quoted span may hold whitespace, and inside it a
 * backslash escapes a double quote or a backslash. The buffer is rewritten: each token is unescaped and
 * NUL-terminated where it lies. Returns the token count, or -1 when the
 * tokens exceed max_tokens or a quote is unterminated.
 * @param buf The NUL-terminated file contents.
 * @param tokens Receives a pointer into buf per token.
 * @param max_tokens Capacity of tokens. */
static int tokenize_sidecar_args(char* buf, char** tokens, int max_tokens) {
    int n = 0;
    char* p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (n >= max_tokens) {
            fprintf(stderr, "openpete: too many tokens in args file (max %d)\n", max_tokens);
            return -1;
        }
        /* A token that is empty or carries whitespace is written
         * double-quoted, with \" and \\ escapes inside. Unescaping runs in
         * place: `out` never outruns `p`, since every escape sequence is
         * longer than the byte it produces and a quote produces none. */
        char* out = p;
        int   quoted = 0;
        tokens[n++] = out;
        while (*p) {
            if (quoted) {
                if (*p == '"') { quoted = 0; p++; continue; }
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) {
                    *out++ = p[1]; p += 2; continue;
                }
            } else {
                if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') break;
                if (*p == '"') { quoted = 1; p++; continue; }
            }
            *out++ = *p++;
        }
        if (quoted) {
            fprintf(stderr, "openpete: unterminated quote in args file\n");
            return -1;
        }
        if (*p) p++;      /* step past the separator before it is overwritten */
        *out = '\0';
    }
    return n;
}

/** @brief Splice the tokens of an args file into argv right after argv[0].
 *
 * The file is at most 4096 bytes and 64 tokens, tokenized by
 * tokenize_sidecar_args. The tokens parse before the existing arguments, so
 * the existing arguments win scalar conflicts under last-write. Returns 1
 * when spliced, with *out_argc and *out_argv holding the new argv (heap
 * memory kept for the process lifetime, since the tokens point into a buffer
 * that outlives opts); 0 when the file is absent or holds no tokens, and the
 * caller keeps argv; -1 on an error, for which the caller returns
 * OPENPETE_EXIT_SETUP_ERROR (an oversized file, a short read and a tokenize
 * failure print a message; a seek or allocation failure does not).
 * @param path The args file.
 * @param label Names the source in messages.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param out_argc Receives the new argument count.
 * @param out_argv Receives the new argument vector. */
static int splice_args_file(const char* path, const char* label,
                            int argc, char** argv,
                            int* out_argc, char*** out_argv) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0; /* absent is the common no-override case */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || sz > 4096) {
        fprintf(stderr, "openpete: %s %s rejected (size=%ld; cap 4096)\n",
                label, path, sz);
        fclose(f); return -1;
    }
    rewind(f);
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "openpete: short read on %s\n", path);
        fclose(f); free(buf); return -1;
    }
    buf[sz] = '\0';
    fclose(f);

    enum { MAX_SIDECAR_TOKENS = 64 };
    char** tokens = (char**)malloc(sizeof(char*) * MAX_SIDECAR_TOKENS);
    if (!tokens) { free(buf); return -1; }
    int ntoks = tokenize_sidecar_args(buf, tokens, MAX_SIDECAR_TOKENS);
    if (ntoks < 0) { free(buf); free(tokens); return -1; }
    if (ntoks == 0) {
        free(buf); free(tokens);
        return 0; /* empty/comments-only - nothing to splice */
    }

    int new_argc = argc + ntoks;
    char** new_argv = (char**)malloc(sizeof(char*) * (size_t)(new_argc + 1));
    if (!new_argv) { free(buf); free(tokens); return -1; }
    int o = 0;
    new_argv[o++] = argv[0];
    for (int i = 0; i < ntoks; i++) new_argv[o++] = tokens[i];
    for (int i = 1; i < argc; i++)  new_argv[o++] = argv[i];
    new_argv[o] = NULL;

    fprintf(stderr, "openpete: %s %s -> %d arg(s) prepended\n",
            label, path, ntoks);

    free(tokens);
    /* buf is intentionally retained - token pointers reference it. */

    *out_argc = new_argc;
    *out_argv = new_argv;
    return 1;
}

/** @brief Splice PATH.args after argv[0] when argv holds "--bk2 PATH" and that file exists.
 *
 * The first --bk2 followed by a value names the movie. The sidecar tokens
 * parse before the typed flags, so a typed flag wins a scalar conflict.
 * Returns as splice_args_file does, or -1 on allocation failure, and 0 when
 * argv has no --bk2.
 * @param argc Argument count.
 * @param argv Argument vector.
 * @param out_argc Receives the new argument count.
 * @param out_argv Receives the new argument vector. */
static int expand_bk2_sidecar(int argc, char** argv, int* out_argc, char*** out_argv) {
    int bk2_idx = -1;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--bk2") == 0) { bk2_idx = i; break; }
    }
    if (bk2_idx < 0) return 0;

    const char* bk2_path = argv[bk2_idx + 1];
    size_t plen = strlen(bk2_path);
    char* args_path = (char*)malloc(plen + 6);
    if (!args_path) return -1;
    memcpy(args_path, bk2_path, plen);
    memcpy(args_path + plen, ".args", 6);

    int rc = splice_args_file(args_path, "bk2 sidecar", argc, argv, out_argc, out_argv);
    free(args_path);
    return rc;
}

/* Persistent settings come from openpete.toml, resolved by
 * core/config/config.c, which materializes the winning layer as an argv
 * splice or a setenv before this parser runs. */

/* --bk2-record args sidecar helpers -----------------------------------
 *
 * When --bk2-record captures a session, we drop a "<bk2>.args" sidecar
 * mirroring the launch flags so `--bk2 <recording>` re-applies the same
 * scenario (see expand_bk2_sidecar). The captured flags come from the
 * *original* argv - before the openpete.toml / bk2-sidecar splices - so
 * only what the user typed on the command line is recorded, not the
 * machine-local persistent defaults. Every --bk2* flag is filtered out:
 * they control recording/replay, not the scenario. Re-emitting
 * --bk2-record would make the replay re-record, and re-emitting the
 * --bk2 replay flag would make a recording-while-replaying session try
 * to replay the *old* movie instead of the new one. */

/** @brief 1 when a token must be written double-quoted in an args sidecar.
 *
 * A token survives a bare whitespace split only if it is non-empty, does not
 * open a comment, and holds no whitespace, double quote or backslash.
 * sidecar_emit_token quotes and escapes every other token, and
 * tokenize_sidecar_args restores it. */
static int sidecar_needs_quotes(const char* a) {
    if (!*a || *a == '#') return 1;
    for (const char* q = a; *q; q++)
        if (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r' ||
            *q == '"' || *q == '\\')
            return 1;
    return 0;
}

/** @brief Length in bytes of a's token in the args sidecar, quotes and escapes included.
 *
 * Matches what sidecar_emit_token writes: strlen(a) for a bare token, else
 * the two enclosing quotes plus one byte per character, doubled for '"' and
 * '\'. The terminating NUL and the separator are not counted. */
static size_t sidecar_token_len(const char* a) {
    size_t n;
    if (!sidecar_needs_quotes(a)) return strlen(a);
    n = 2;  /* the enclosing quotes */
    for (const char* q = a; *q; q++) n += (*q == '"' || *q == '\\') ? 2 : 1;
    return n;
}

/** @brief Write a's sidecar token at p and return the position after it.
 *
 * A token that needs no quotes is copied as is; any other is written
 * double-quoted with '"' and '\' preceded by a backslash. No NUL is written
 * and the caller has reserved sidecar_token_len(a) bytes.
 * @param p Output cursor.
 * @param a Token to emit. */
static char* sidecar_emit_token(char* p, const char* a) {
    if (!sidecar_needs_quotes(a)) {
        size_t n = strlen(a);
        memcpy(p, a, n);
        return p + n;
    }
    *p++ = '"';
    for (const char* q = a; *q; q++) {
        if (*q == '"' || *q == '\\') *p++ = '\\';
        *p++ = *q;
    }
    *p++ = '"';
    return p;
}

/** @brief 1 when a begins with "--bk2", which covers every recording and replay flag. */
static int is_bk2_flag(const char* a) {
    return strncmp(a, "--bk2", 5) == 0;
}

/** @brief How many following tokens (0 or 1) a --bk2* flag consumes as its value.
 *
 * The sidecar exclusion walk skips the value with the flag. --bk2 and
 * --bk2-frame-offset always take one; --bk2-record takes the next token only
 * when it does not start with '-'.
 * @param a The --bk2* flag.
 * @param next The token after a, or NULL at the end of argv. */
static int bk2_flag_value_tokens(const char* a, const char* next) {
    if (strcmp(a, "--bk2") == 0) return 1;                       /* replay PATH */
    if (strcmp(a, "--bk2-frame-offset") == 0) return 1;          /* always a value */
    if (strcmp(a, "--bk2-record") == 0)                           /* optional value */
        return (next && next[0] != '-') ? 1 : 0;
    return 0;   /* --bk2-stop-on-end and any other flag-only --bk2* */
}

/** @brief Build the <bk2>.args sidecar text from the typed argv, without the --bk2* flags.
 *
 * argv is the command line before the openpete.toml and sidecar splices.
 * Every --bk2* flag and its value are dropped. The text is a '#' comment
 * header, then the tokens space-separated (quoted where
 * sidecar_needs_quotes says so), then a newline. Returns a heap buffer kept
 * for the process lifetime, or NULL when allocation fails.
 * @param argc Argument count.
 * @param argv The typed argument vector. */
static char* build_bk2_record_args(int argc, char** argv) {
    static const char header[] =
        "# openpete --bk2-record: launch args auto-captured for replay via --bk2\n"
        "# (excludes --bk2* flags and anything sourced from openpete.toml)\n";
    const size_t hlen = sizeof(header) - 1;  /* without NUL */

    size_t len = hlen;
    for (int i = 1; i < argc; i++) {
        if (is_bk2_flag(argv[i])) {
            i += bk2_flag_value_tokens(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL);
            continue;
        }
        len += sidecar_token_len(argv[i]) + 1;  /* token + separator */
    }

    char* buf = (char*)malloc(len + 2);  /* +2: newline fixup + NUL */
    if (!buf) return NULL;
    char* p = buf;
    memcpy(p, header, hlen); p += hlen;
    for (int i = 1; i < argc; i++) {
        if (is_bk2_flag(argv[i])) {
            i += bk2_flag_value_tokens(argv[i], (i + 1 < argc) ? argv[i + 1] : NULL);
            continue;
        }
        p = sidecar_emit_token(p, argv[i]);
        *p++ = ' ';
    }
    if (p > buf && p[-1] == ' ') p[-1] = '\n';  /* last separator -> newline */
    else                        *p++  = '\n';   /* no tokens: header already ends '\n' */
    *p = '\0';
    return buf;
}

/** @brief 1 when a bare argument names an existing disc image file.
 *
 * The extension must be .cue, .bin, .img, .iso, .chd or .pbp, compared
 * case-insensitively, and the file must open for reading. Both tests are
 * required: an existence check alone would take a mistyped path, and an
 * extension check alone would take a mistyped flag value.
 * @param a The argument. */
static int arg_is_disc_image(const char* a) {
    const char* dot = strrchr(a, '.');
    static const char* const kExt[] = { ".cue", ".bin", ".img", ".iso",
                                        ".chd", ".pbp" };
    if (!dot) return 0;
    size_t i;
    int match = 0;
    for (i = 0; i < sizeof kExt / sizeof kExt[0] && !match; i++) {
        const char* e = kExt[i];
        size_t k;
        for (k = 0; e[k]; k++)
            if (tolower((unsigned char)dot[k]) != e[k]) break;
        match = (e[k] == '\0' && dot[k] == '\0');
    }
    if (!match) return 0;
    FILE* f = fopen(a, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

/** @brief The default --bk2-record path, "<bk2 dir>/<YYYYmmdd-HHMMSS>.bk2", creating the directory.
 *
 * The bk2 directory is the "bk2" data directory from openpete_paths_data_dir,
 * or "bk2" relative to the working directory when none resolves outside a
 * sandbox (openpete_paths_sandbox). A failed directory creation is logged
 * and the path is still returned. Returns a heap buffer kept for the
 * process lifetime, or NULL when allocation fails or no directory resolves
 * under a sandbox. */
static char* resolve_default_bk2_record_path(void) {
    char stamp[24];
    time_t now = time(NULL);
    /* localtime() (not _r/_s): option parsing is single-threaded and runs
     * once, so the shared-buffer caveat doesn't apply, and this avoids the
     * platform-specific reentrant variants. */
    struct tm* tmv = localtime(&now);
    if (!tmv || strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", tmv) == 0)
        snprintf(stamp, sizeof(stamp), "recording");

    /* Recordings are output, so the directory has to be writable: next to
     * the exe in a portable install, under the per-user data directory
     * otherwise (an install medium we cannot write is exactly the case
     * that used to fail here). */
    char resolved[1024];
    char* dir;
    if (openpete_paths_data_dir("bk2", resolved, sizeof resolved, 0) == 0) {
        dir = (char*)malloc(strlen(resolved) + 1);
        if (!dir) return NULL;
        strcpy(dir, resolved);
    } else if (openpete_paths_sandbox()) {
        fprintf(stderr, "openpete: --bk2-record: no bk2 directory resolves under the sandbox root\n");
        return NULL;
    } else {
        dir = (char*)malloc(4);  /* "bk2" + NUL - relative to CWD */
        if (!dir) return NULL;
        memcpy(dir, "bk2", 4);
    }
    if (!SDL_CreateDirectory(dir))  /* mkdir -p; true if it already exists */
        fprintf(stderr, "openpete: --bk2-record: could not create %s: %s\n",
                dir, SDL_GetError());

    size_t need = strlen(dir) + 1 /* '/' */ + strlen(stamp) + 4 /* ".bk2" */ + 1;
    char* path = (char*)malloc(need);
    if (!path) { free(dir); return NULL; }
    snprintf(path, need, "%s/%s.bk2", dir, stamp);
    free(dir);
    return path;
}

void openpete_options_set_default_renderer(openpete_options_t* opts,
                                           openpete_renderer_t r) {
    opts->renderer         = r;
    opts->default_renderer = r;
}

/* Every flag the parser accepts that has no knob registry row and is not a
 * config schema alias (--no-lod): the one-shot reports and the preamble's
 * --no-config, --help, the deprecated --show-fps, and the debug and
 * diagnostic flags. The surface dump lists them beside the registry's flags,
 * so the two together name every parser literal. `block` is the --help block
 * that prints the flag ("none" = not printed; "preamble" = the fixed help
 * header names it); help_arg, help_order and help_text are its generated
 * --help line, as on a registry row. */
static const openpete_help_only_t k_help_only[] = {
    { "--help",                              KNOB_FLAG_ARG_NONE,     NULL, "none",     NULL, NULL, 0, NULL },
    { "-h",                                  KNOB_FLAG_ARG_NONE,     NULL, "none",     NULL, NULL, 0, NULL },
    { "--no-config",                         KNOB_FLAG_ARG_NONE,     NULL, "preamble", NULL, NULL, 0, NULL },
    { "--config-report",                     KNOB_FLAG_ARG_NONE,     NULL, "preamble", NULL, NULL, 0, NULL },
    { "--keys-report",                       KNOB_FLAG_ARG_NONE,     NULL, "preamble", NULL, NULL, 0, NULL },
    { "--show-fps",                          KNOB_FLAG_ARG_REQUIRED, NULL, "release",  "deprecated spelling of --perf-stats",
      "0|1", 140,
      "deprecated spelling of --perf-stats off|fps (on|off also accepted)" },
    { "--bug-report-replay",                 KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 25,
      "replay a bug-report bundle (zip or extracted directory) headless\n"
      "from its anchor and compare every tick's region hashes and the end\n"
      "state; exit 0 IDENTICAL, 2 DIVERGED, 4 REPRODUCED (a fatal bundle's\n"
      "fatal exit recurred at its tick), 64 refused; pass --disc" },
    { "--bug-report-replay-beyond",          KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "TICKS", 26,
      "with --bug-report-replay, run TICKS ticks past the bundle's last\n"
      "recorded tick on neutral input, compared with nothing (default 0)" },
    { "--debug-triangle",                    KNOB_FLAG_ARG_NONE,     NULL, "debug",    NULL,
      NULL, 460,
      "emit a hardcoded POLY_F3 each frame (infra-only validation)" },
    { "--dump-sfx-pack",                     KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "DIR", 80,
      "write every game sound on --disc as a content-named WAV\n"
      "(+ index.txt) into DIR and exit: the sfx pack census" },
    { "--dump-sfx-wads",                     KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "A.wad,B.wad", 90,
      "also walk these custom-level Data files (a [[level]]\n"
      "mod's assets/levels/<stem>.wad)" },
    { "--exe-path",                          KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 50,
      "direct PSX EXE bytes (overrides --disc)" },
    { "--extract-recomp-inputs",             KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "DIR", 70,
      "extract PSX.EXE + overlay blobs from --disc\n"
      "into DIR and exit (recomp pipeline stage)" },
    { "--frame-limit",                       KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 30,
      "exit once the feed key reaches N (yields on Spyro 1, VBLs on Spyro 2 and 3)" },
    { "--frame-time-log",                    KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 510,
      "write per-present binary record (32B header + 32B/record) for scripts/measure-pacing.py" },
    { "--freecam-path",                      KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "SPEC", 470,
      "engage the free camera and fly it along a looping waypoint\n"
      "path: \"[ticks=N;] WP(;WP)+\" where WP is \"topdown\" or\n"
      "\"x,y,z[,yaw,pitch[,roll]]\", or \"@FILE\" to read the spec\n"
      "from FILE (';' or newline between segments, any count).\n"
      "Interpolates over N ticks per segment (default 120), angles\n"
      "along the shorter arc. Fractional values make presents follow\n"
      "a Catmull-Rom curve through the waypoints (\"cut\" segment:\n"
      "none through it); the game's camera takes the rounded pose.\n"
      "Exercises the costs only movement reaches\n"
      "(sector-cache churn, LOD crossings) that a static pose cannot.\n"
      "CLI twin of OPENPETE_FREECAM_PATH; supersedes --freecam-pose." },
    { "--gameplay-hash-log",                 KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 500,
      "append CSV (frame,vbl,hex_hash) of FNV1a-64 over the\n"
      "gameplay-state region (games/<game>/manifest/gameplay_state_regions.toml).\n"
      "Primary determinism gate - see check_aspect_determinism.sh" },
    { "--halt-at-pc",                        KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "HEX", 340,
      "dump RAM to --ram-dump-output at first call to fn HEX, then exit" },
    { "--halt-cpu-dump-output",              KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 350,
      "also write 528B 'CSv1' CPU dump alongside RAM at halt" },
    { "--halt-hit",                          KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 360,
      "dump on Nth (1-indexed) call to --halt-at-pc (default 1)" },
    { "--headless",                          KNOB_FLAG_ARG_NONE,     NULL, "debug",    NULL,
      NULL, 10,
      "no on-screen window (offscreen video driver)" },
    { "--max-runtime-seconds",               KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 40,
      "wall-clock SIGTERM after N seconds" },
    { "--pace-log",                          KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 520,
      "write the unified pacing event log (every VBL + present, one timebase) for scripts/pacing/" },
    { "--position-log",                      KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 490,
      "append CSV (frame,vbl,x,y,z) of g_Spyro.m_Position each yield" },
    { "--probe",                             KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "HEX", 440,
      "read 4 bytes at this PSX vaddr post-dispatch and log" },
    { "--profile-out",                       KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 530,
      "stream a binary .opp perf trace (CPU scopes + GPU zones + census,\n"
      "keyed per present / canonical tick) for scripts/profile-summary.py\n"
      "(plain text) and scripts/profile-to-trace.py (Perfetto flame chart).\n"
      "Windowed only for GPU/intermediate coverage; ~0.1% CPU overhead." },
    { "--psxtrace-anchor-hit",               KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 400,
      "skip first N-1 hits before starting (default 1 = first)" },
    { "--psxtrace-anchor-pc",                KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "HEX", 390,
      "PC where advance() fires (default: trace's first-record pc)" },
    { "--psxtrace-keep-going",               KNOB_FLAG_ARG_NONE,     NULL, "debug",    NULL,
      NULL, 380,
      "don't exit on first divergence (collect all)" },
    { "--psxtrace-verify",                   KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 370,
      "consume per-frame .psxtrace + diff against PSX oracle" },
    { "--ram-dump-on-frame",                 KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 230,
      "write 2MB main RAM to --ram-dump-output once the feed key reaches N" },
    { "--ram-dump-output",                   KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 240,
      "RAM dump path (default ./ram.bin)" },
    { "--recomp-module",                     KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 60,
      "shared-linkage builds: load the recomp module\n"
      "from PATH instead of the default next to the exe" },
    { "--resume-handoff",                    KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 20,
      "resume a session from a process-handoff checkpoint\n"
      "(written by the handoff exec; not for manual use)" },
    { "--rewind-hold",                       KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "A:N", 310,
      "headless: simulate holding the rewind key for N iterations once the feed key reaches A" },
    { "--screenshot-freeze-backdrop",        KNOB_FLAG_ARG_NONE,     NULL, "debug",    NULL,
      NULL, 190,
      "no effect, accepted for existing command lines: every headless capture maintains the freeze backdrop" },
    { "--screenshot-intermediate-on-frame",  KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N[,...]", 180,
      "deterministic sub-tick (alpha 0.5) native captures at each feed-key value, headless-capable; implies --screenshot-native-only" },
    { "--screenshot-native-only",            KNOB_FLAG_ARG_NONE,     NULL, "debug",    NULL,
      NULL, 170,
      "capture the native -vk variant alone: skip PsyCross OT rasterization and -og (presentation-golden corpus path)" },
    { "--screenshot-on-frame",               KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N[,N2,...]", 150,
      "save image at each listed feed-key value (yields on Spyro 1, VBLs on\n"
      "Spyro 2 and 3); can be passed once with a comma list.\n"
      "In a windowed pure-vk run, -vk.png is a READBACK of the exact\n"
      "presented frame (sub-tick interp, mods, overlays included);\n"
      "headless runs recompose the canonical scene instead." },
    { "--screenshot-on-tick",                KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N[,N2,...]", 160,
      "same, keyed on canonical draw-ticks instead of VBL frames.\n"
      "Tick-keyed captures hit the same sim moment run-to-run (the\n"
      "frame<->tick mapping drifts with host timing); used by the visual gates.\n"
      "The PsyCross render is written as <out>-og.png; the native render as <out>-vk.png" },
    { "--screenshot-output",                 KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 200,
      "screenshot path template (default ./screenshot.png); supports {n} for sim frame and {p} for present index; .png ext writes PNG, otherwise BMP" },
    { "--screenshot-presents-count",         KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "K", 220,
      "number of consecutive presents to capture when --screenshot-presents-start is set (default 1)" },
    { "--screenshot-presents-start",         KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 210,
      "capture every present (canonical + intermediate) once the feed key reaches N (default -1 = off)" },
    { "--seed-cpu",                          KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 330,
      "pre-fill CPUState (GPRs/pc/hi/lo/cop0/GTE) from 528B dump" },
    { "--seed-ram",                          KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 320,
      "pre-fill g_ram from a 2MB dump before dispatch" },
    { "--state-load-input",                  KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 280,
      "state file for the scheduled load" },
    { "--state-load-on-frame",               KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 270,
      "load --state-load-input once the feed key reaches N (re-anchors the counters)" },
    { "--state-save-on-frame",               KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 250,
      "savestate to --state-save-output once the feed key reaches N" },
    { "--state-save-output",                 KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "PATH", 260,
      "scheduled-save path (default ./frameN.state)" },
    { "--state-stress",                      KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 290,
      "every N ticks: in-memory save + immediate quiet restore (restore-at-yield gate)" },
    { "--state-stress-rewind",               KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "K", 300,
      "save, run K iterations, restore, repeat (runahead rollback shape)" },
    { "--test-recomp-call",                  KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "HEX", 410,
      "boot, dispatch one fn at HEX, exit" },
    { "--test-recomp-expect-v0",             KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "N", 420,
      "verify cpu->v0 after --test-recomp-call" },
    { "--test-recomp-gp",                    KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "HEX", 430,
      "set cpu->gp before --test-recomp-call (skips PSYQ _96_init)" },
    { "--test-segv",                         KNOB_FLAG_ARG_NONE,     NULL, "debug",    NULL,
      NULL, 450,
      "deliberately raise SIGSEGV (crash-handler smoke test)" },
    { "--unlock-frames",                     KNOB_FLAG_ARG_REQUIRED, NULL, "debug",    NULL,
      "LIST", 480,
      "force fast-forward inside feed-key ranges, e.g. 100-200,500-600" },
};

const openpete_help_only_t* openpete_options_help_only(size_t* count) {
    *count = sizeof k_help_only / sizeof k_help_only[0];
    return k_help_only;
}

/* Column at which every --help description starts and continues. */
#define HELP_TEXT_COL 29

/* The --help blocks a flag can print in, in print order. */
enum { HELP_RELEASE, HELP_LIBAV, HELP_DEBUG, HELP_NONE };

/* One --help entry: a registry row's flag or a help-only table entry. */
typedef struct {
    const char*    flag;
    const char*    arg;
    const char*    text;
    const char*    def;
    unsigned short order;
    const char*    key;    /* registry row's key_action, or NULL */
} help_entry_t;

/* Capacity of one block's entry list: the registry's flag rows plus the
 * help-only table. */
#define HELP_ENTRIES_MAX 256

/** @brief The --help block of a registry row: debug for a row of the debug block, libav for an OPENPETE_HAS_LIBAV row, else release. */
static int help_block_of_row(const knob_desc_t* k) {
    if (k->debug_block) return HELP_DEBUG;
    if (k->build_cond && strcmp(k->build_cond, "OPENPETE_HAS_LIBAV") == 0) return HELP_LIBAV;
    return HELP_RELEASE;
}

/** @brief The --help block of a help-only entry; HELP_NONE for "none" and "preamble", which the header prints. */
static int help_block_of_entry(const openpete_help_only_t* h) {
    if (h->build_cond && strcmp(h->build_cond, "OPENPETE_HAS_LIBAV") == 0) return HELP_LIBAV;
    if (strcmp(h->block, "release") == 0) return HELP_RELEASE;
    if (strcmp(h->block, "debug") == 0) return HELP_DEBUG;
    return HELP_NONE;
}

/** @brief qsort order of help entries: ascending help_order. */
static int help_entry_cmp(const void* a, const void* b) {
    const help_entry_t* x = (const help_entry_t*)a;
    const help_entry_t* y = (const help_entry_t*)b;
    return (int)x->order - (int)y->order;
}

/** @brief Collect the entries of one --help block from the registry rows and the help-only table, sorted by help_order.
 *
 * @param block HELP_RELEASE, HELP_LIBAV or HELP_DEBUG.
 * @param out Receives the entries.
 * @param cap Capacity of out.
 * @return The entry count. */
static size_t help_collect(int block, help_entry_t* out, size_t cap) {
    size_t n = 0, count = 0;
    const knob_desc_t* ks = knob_registry(&count);
    for (size_t i = 0; i < count && n < cap; i++) {
        const knob_desc_t* k = &ks[i];
        if (!k->cli || !k->help_text || help_block_of_row(k) != block) continue;
        out[n++] = (help_entry_t){ k->cli, k->help_arg, k->help_text, k->def_str, k->help_order,
                                   k->key_action };
    }
    for (size_t i = 0; i < sizeof k_help_only / sizeof k_help_only[0] && n < cap; i++) {
        const openpete_help_only_t* h = &k_help_only[i];
        if (!h->help_text || help_block_of_entry(h) != block) continue;
        out[n++] = (help_entry_t){ h->flag, h->help_arg, h->help_text, NULL, h->help_order, NULL };
    }
    qsort(out, n, sizeof *out, help_entry_cmp);
    return n;
}

/** @brief Print a help description, expanding its tokens and indenting each continuation line to HELP_TEXT_COL.
 *
 * "{def}" prints def; "{default_mark:NAME}" prints " - default" when NAME
 * is `renderer`; any other brace is literal text.
 * @param t The description.
 * @param def The row's declared default, or NULL.
 * @param renderer Name of the game's default renderer. */
static void help_put_text(const char* t, const char* def, const char* renderer) {
    static const char kMark[] = "{default_mark:";
    while (*t) {
        const char* end = NULL;
        if (strncmp(t, "{def}", 5) == 0) {
            fputs(def ? def : "", stderr);
            t += 5;
        } else if (strncmp(t, kMark, sizeof kMark - 1) == 0 &&
                   (end = strchr(t, '}')) != NULL) {
            const char* name = t + sizeof kMark - 1;
            size_t len = (size_t)(end - name);
            if (strlen(renderer) == len && strncmp(name, renderer, len) == 0)
                fputs(" - default", stderr);
            t = end + 1;
        } else if (*t == '\n') {
            fprintf(stderr, "\n%*s", HELP_TEXT_COL, "");
            t++;
        } else {
            fputc(*t++, stderr);
        }
    }
    fputc('\n', stderr);
}

/** @brief Print the entries of one --help block, each as "  FLAG ARG" padded to HELP_TEXT_COL and its description.
 *
 * A head of 27 characters takes one separating space and a longer head
 * two, so the description starts past HELP_TEXT_COL. An entry whose row
 * names a key action adds a "Key:" line with the action's built-in binding,
 * so the output depends on no config file.
 * @param block HELP_RELEASE, HELP_LIBAV or HELP_DEBUG.
 * @param renderer Name of the game's default renderer. */
static void help_put_block(int block, const char* renderer) {
    help_entry_t es[HELP_ENTRIES_MAX];
    size_t n = help_collect(block, es, HELP_ENTRIES_MAX);
    for (size_t i = 0; i < n; i++) {
        char head[128];
        snprintf(head, sizeof head, "%s%s%s", es[i].flag, es[i].arg ? " " : "",
                 es[i].arg ? es[i].arg : "");
        size_t len = strlen(head);
        int pad = len < 27 ? (int)(27 - len) : (len == 27 ? 1 : 2);
        fprintf(stderr, "  %s%*s", head, pad, "");
        help_put_text(es[i].text, es[i].def, renderer);
        const int act = openpete_keymap_host_action(es[i].key);
        if (act >= 0) {
            char key[160];
            openpete_keymap_action_default_label((openpete_action_t)act, key, sizeof key);
            fprintf(stderr, "%*sKey: %s\n", HELP_TEXT_COL, "", key);
        }
    }
}

/** @brief Print the command-line help for `display_name`'s build to stderr.
 *
 * The header (usage, the config-file layers and the preamble flags) is
 * fixed text. The flag blocks are generated from the knob registry rows and
 * the help-only table: the release block always prints; the libav block
 * ("Video recording") prints only under OPENPETE_HAS_LIBAV; the debug block
 * is compiled out under OPENPETE_HIDE_DEBUG_FLAGS, set by the *-launcher
 * distribution presets, and a one-line note replaces it. The omission is
 * display-only: every flag parses in every build. A footer names the
 * published documentation and the packaged copy.
 * @param display_name Game title printed in the heading.
 * @param argv0 Program path printed in the usage line.
 * @param default_renderer Renderer the game starts with when --renderer is absent. */
static void print_help(const char* display_name, const char* argv0,
                       openpete_renderer_t default_renderer) {
    const char* renderer = default_renderer == OPENPETE_RENDERER_VK       ? "vk"
                         : default_renderer == OPENPETE_RENDERER_PSYCROSS ? "psycross"
                         : "";
    fprintf(stderr,
        "OpenPete %s\n"
        "Usage: %s [options] [DISC-IMAGE]\n"
        "  A bare path to a .cue/.bin/.img/.iso/.chd/.pbp means --ingest, so opening a\n"
        "  disc image from a file manager or dragging it onto the program just works.\n"
        "  An 'openpete.toml' next to the executable and one in your config dir supply\n"
        "  persistent settings, in layers: global < [game.ID] < [disc.SERIAL] < [disc.SHA]\n"
        "  < [saved] < environment < bk2 sidecar < the flags you type. Inside a layer,\n"
        "  bare keys < [env] < [args]. --config-report shows what won and what it beat.\n"
        "  --no-config  ignore every config file\n"
        "  --config-report  print the resolution table and exit\n"
        "  Keys and pad buttons rebind the same way, under [keys.pad] / [keys.host]\n"
        "  in any scope (e.g. [game.spyro-1.keys.host]). Bind a key by its name\n"
        "  (\"F5\", \"Left Shift\"), a pad input with the pad: prefix (\"pad:south\",\n"
        "  \"pad:-leftx\"), several with an array, or none with \"none\" to unbind.\n"
        "  --keys-report    print every action, its binding and where it came from, and exit\n",
        display_name, argv0);
    help_put_block(HELP_RELEASE, renderer);
#ifdef OPENPETE_HAS_LIBAV
    fputs("\nVideo recording (needs libav):\n", stderr);
    help_put_block(HELP_LIBAV, renderer);
#endif
#ifdef OPENPETE_HIDE_DEBUG_FLAGS
    fputs("  (plus dev/diagnostic flags, hidden in this build)\n", stderr);
#else
    fputs("\nDebug / diagnostic flags:\n", stderr);
    help_put_block(HELP_DEBUG, renderer);
#endif
    fputs("\nDocumentation: " OPENPETE_DOCS_URL "\n"
          "  The Windows zip and the Linux tar.xz carry a copy as docs/index.html\n"
          "  beside the program.\n",
          stderr);
}

void openpete_options_init(openpete_options_t* opts) {
    memset(opts, 0, sizeof(*opts));
    opts->display_name        = "OpenPete";
    opts->screenshot_frame_count = 0;
    opts->screenshot_ticks_keyed = false;
    opts->screenshot_output      = "./screenshot.png";
    opts->screenshot_presents_start = -1;
    opts->screenshot_presents_count = 1;
    opts->ram_dump_on_frame   = -1;
    opts->ram_dump_output     = "./ram.bin";
    opts->state_save_on_frame = -1;
    opts->state_save_output   = "./frameN.state";
    opts->state_load_on_frame = -1;
    opts->state_load_input    = NULL;
    opts->state_stress        = 0;
    opts->state_stress_rewind = 0;
    opts->rewind_window       = 0;
    opts->rewind_hold_frame   = -1;
    opts->rewind_hold_iters   = 0;
    opts->runahead            = 0;
    opts->log_format          = OPENPETE_LOG_PLAIN;
    opts->log_file            = NULL;  /* unset: env, else <state>/logs/openpete.log */
    opts->renderer            = OPENPETE_RENDERER_VK;  /* the native Vulkan renderer both rasterizes and displays; --renderer psycross keeps the GL reference as the rasterizer and displays its frame in the same native window. Inert under --headless, where native stays off and PsyCross rasterizes for the oracle. openpete_options_set_default_renderer, called before parsing, changes both this and default_renderer for a game that starts elsewhere. */
    opts->default_renderer    = OPENPETE_RENDERER_VK;
    opts->mute                = false; /* audio on by default; --mute 1 to silence */
    opts->sfx_volume          = -1;    /* -1 = unset; the game's own slider stands */
    opts->music_volume        = -1;
    opts->vibration           = -1;    /* -1 = unset; the game's own setting stands */
    opts->mouse               = -1;    /* -1 = unset; the mouse channel is on */
    opts->input_lock          = -1;    /* -1 = unset; host input is taken */
    opts->pad_unfocused       = -1;    /* -1 = unset; unfocused pads are ignored */
    opts->stick_response      = -1;    /* -1 = unset; console */
    opts->background          = -1;    /* -1 = unset; the window activates normally */
    opts->screenshot_overlays = -1;    /* -1 = unset; recomposed screenshots draw no chrome */
    opts->mouse_sensitivity   = -1.0f; /* -1 = unset (1.0) */
    opts->ssaa                = 1;     /* native vk supersampling: 1 = no-op */
    opts->perf_stats          = OPENPETE_PERF_FPS;  /* the counter, no pacing readout; --perf-stats / O-menu SHOW FPS row */
    /* Top-RIGHT: the stats block and the notifications both default to a
     * corner, and putting them in the same one would have every toast push
     * itself down past a readout the player is watching. Opposite corners is
     * the arrangement that needs no explanation. */
    opts->perf_position       = OPENPETE_ANCHOR_TOP_RIGHT;
    opts->osd                 = 1;                          /* toasts on */
    opts->osd_position        = OPENPETE_ANCHOR_TOP_LEFT;
    opts->osd_timeout_ms      = 3000u;
    opts->version_osd         = 1;                          /* version line on */
    opts->vsync               = -1;    /* -1 = unset; default vsync ON (FIFO) unless OPENPETE_PRESENT_MODE */
    opts->render_fps          = -1;    /* -1 = unset; resolved at startup: display refresh (windowed) /
                                        * 0 legacy match-sim pacer (headless). --render-fps 0 = explicit legacy. */
    opts->fullscreen          = 0;     /* windowed by default; --fullscreen 1 = borderless-desktop launch */
    opts->launcher_fullscreen = -1;    /* -1 = unset; the launcher opens windowed */
    opts->launcher_window_maximized = -1;  /* -1 = unset; the launcher takes --window's size, else 1280x800 */
    opts->tick_scale          = 1.0;   /* engine speed multiplier: 1.0 = real-time */
    opts->record_encoder      = "libx264";
    opts->record_crf          = 23;
    opts->record_gop          = 120;
    opts->record_pixfmt       = "yuv420p";
    opts->record_audio_encoder = "aac";
    opts->record_fps_overlay  = -1;    /* -1 = unset; default off (env OPENPETE_FPS_IN_RECORD) */
    opts->devui_open          = -1;    /* -1 = unset; default closed (env OPENPETE_DEVUI_OPEN) */
    opts->record_cfr          = 0.0;   /* 0 = true VFR (wall-clock PTS) */
    opts->record_frames       = 0;     /* 0 = record until the run ends */
    opts->texpack_dir         = NULL;  /* NULL = unset; env OPENPETE_TEXPACK_DIR / built-in dir */
    opts->controller_db       = NULL;  /* NULL = unset; env OPENPETE_CONTROLLER_DB */
    opts->sfxpack_dir         = NULL;  /* NULL = unset; env OPENPETE_SFXPACK_DIR / built-in dir */
    opts->mods_dir            = NULL;  /* NULL = unset; env OPENPETE_MODS_DIR / <exe>/mods */
    opts->mods_enabled        = NULL;  /* NULL = unset; env OPENPETE_MODS / mods/enabled.txt */
    opts->compile_mods        = 0;     /* --compile-mods: compile every mod and exit */
    opts->dump_sfx_pack_dir   = NULL;
    opts->dump_sfx_wads       = NULL;
    opts->card_dir            = NULL;  /* NULL = default appdir; env OPENPETE_CARD_DIR */
    opts->card1_path          = NULL;
    opts->card2_path          = NULL;
    opts->card_a_present      = 1;     /* both cards inserted by default */
    opts->card_b_present      = 1;
    opts->card_unformatted    = 0;     /* pre-formatted empty card by default */
    opts->adopt_saves         = 0;     /* modset-mismatch saves warn until adopted */
    opts->save_mismatch_toast = 0;     /* the mismatch log line only */
    opts->resume_handoff      = NULL;  /* process-handoff resume file */
    opts->bug_report_replay   = NULL;  /* bug-report bundle replayed */
    opts->handoff_argc        = 0;     /* original argv (exec rebuild) */
    opts->handoff_argv        = NULL;
    opts->vertex_precision    = OPENPETE_VERTEX_PRECISION_SUBPIXEL;  /* smooth sub-pixel; --vertex-precision integer for the PSX polygon wobble */
    opts->raster_quantize = -1;   /* auto: 1x under --vertex-precision integer, else off */
    opts->extract_threads = 0;    /* auto: logical cores / 2, clamped to [1, 8] */
    opts->dither          = 0;    /* --dither 1 for PS1 15-bit colour + hardware dither */
    /* -1 = unset: resolve_env_spellings applies OPENPETE_NOTEX and
     * OPENPETE_WIREFRAME, else 0. */
    opts->notex     = -1;
    opts->wireframe = -1;
    /* -1 = unset: resolve_env_spellings applies OPENPETE_HIDE_HUD, else 0;
     * NULL = unset: it applies OPENPETE_HIDE_CLASSES, else none. */
    opts->hide_hud     = -1;
    opts->hide_classes = NULL;
    opts->psycross_native_raster = 0;  /* reference renders on the window-derived square-pixel grid */
    opts->texture_mapping = OPENPETE_TEXTURE_MAPPING_PERSPECTIVE;  /* --texture-mapping affine for the PSX texture swim */
    /* Retained-terrain GPU cache (default on) and the resident disc image
     * (default auto); -1 = unset until resolve_env_spellings applies the env
     * spelling (OPENPETE_TERRAIN_CACHE, OPENPETE_DISC_CACHE) or the default. */
    opts->terrain_cache = -1;
    opts->disc_cache    = OPENPETE_DISC_CACHE_UNSET;
    opts->scan_guard    = -1;   /* resolve_env_spellings: OPENPETE_SCAN_GUARD, else 1 */
    opts->mod_hot_reload = -1;  /* resolve_env_spellings: OPENPETE_MOD_HOT_RELOAD, else 0 */
    /* -1 = the game's own default (game_get_load_delay_default), resolved by
     * run.c; resolve_env_spellings applies OPENPETE_LOAD_DELAY. */
    opts->load_delay    = -1;
    opts->lod                 = OPENPETE_LOD_ORIGINAL;  /* authentic PSX LOD band; --lod extended (a.k.a. --no-lod) for full detail at any distance */
    opts->fog_mode            = OPENPETE_FOG_AUTO;      /* follows --lod until --fog picks one */
    /* Unset until resolve_env_spellings applies OPENPETE_SECTOR_VISIBILITY,
     * else frustum. */
    opts->sector_visibility = OPENPETE_SECTOR_VIS_UNSET;
    /* Unset until resolve_env_spellings applies OPENPETE_CADENCE, else even. */
    opts->cadence = OPENPETE_CADENCE_UNSET;
}

/** @brief Parse a --cadence value: even or fill.
 * @param v The value text.
 * @param out Receives the policy; untouched on failure.
 * @return 0 on success, -1 for any other text. */
static int cadence_parse(const char* v, openpete_cadence_t* out) {
    if (strcmp(v, "even") == 0) { *out = OPENPETE_CADENCE_EVEN; return 0; }
    if (strcmp(v, "fill") == 0) { *out = OPENPETE_CADENCE_FILL; return 0; }
    return -1;
}

/** @brief Parse a --disc-cache value: 0, 1 or auto.
 * @param v The value text.
 * @param out Receives the mode; untouched on failure.
 * @return 0 on success, -1 for any other text. */
static int disc_cache_parse(const char* v, int* out) {
    if (strcmp(v, "0") == 0)    { *out = OPENPETE_DISC_CACHE_OFF;  return 0; }
    if (strcmp(v, "1") == 0)    { *out = OPENPETE_DISC_CACHE_ON;   return 0; }
    if (strcmp(v, "auto") == 0) { *out = OPENPETE_DISC_CACHE_AUTO; return 0; }
    return -1;
}

/** @brief Parse a --sector-visibility value: game, frustum, filled or auto.
 * @param v The value text.
 * @param out Receives the mode; untouched on failure.
 * @return 0 on success, -1 for any other text. */
static int sector_vis_parse(const char *v, openpete_sector_visibility_t *out) {
    static const char *const names[] = { "game", "frustum", "filled", "auto" };
    for (int m = 0; m < 4; m++)
        if (strcmp(v, names[m]) == 0) { *out = (openpete_sector_visibility_t)m; return 0; }
    return -1;
}

/** @brief Parse a --fog value: original, per-pixel or off.
 * @param v The value text.
 * @param out Receives the mode; untouched on failure.
 * @return 0 on success, -1 for any other text. */
static int fog_mode_parse(const char *v, openpete_fog_mode_t *out) {
    static const char *const names[] = { "original", "per-pixel", "off" };
    for (int m = 0; m < 3; m++)
        if (strcmp(v, names[m]) == 0) { *out = (openpete_fog_mode_t)m; return 0; }
    return -1;
}

/* Facts the flag chain learns that are not options: the post-parse phases
 * consume them. */
typedef struct {
    /* --bk2-record was given, with or without a path argument (the default
     * path is resolved after the chain). */
    bool bk2_record_requested;
    /* --player-pose was typed: the CLI spelling rides the .args sidecar, the
     * env spelling does not. */
    bool player_pose_flag;
} parse_notes_t;

/* 1 while openpete_options_parse_neutral runs: the game-neutral parse writes
 * nothing to the process environment, so the full parse after
 * identification sees the environment the user exported. */
static int s_parse_neutral;

/** @brief Set the env spelling a flag stands for, except in the game-neutral parse.
 *
 * --freecam-path, --freecam-pose and --player-pose hand their value to
 * consumers that read the variable. The full parse after identification
 * repeats the flag and sets it then.
 * @param name The variable.
 * @param v The flag's value. */
static void flag_setenv(const char* name, const char* v) {
    if (s_parse_neutral) return;
#ifdef _WIN32
    _putenv_s(name, v);
#else
    setenv(name, v, 1);
#endif
}

/** @brief Add one --player-pose placement: the first typed one replaces an exported OPENPETE_PLAYER_POSE, each later one is appended after ';'.
 *
 * The placements apply in order (games/common/freecam_game.c), each at its
 * "@N" feed-key value.
 * @param v The flag's value.
 * @param notes Records that the flag was typed. */
static void player_pose_add(const char* v, parse_notes_t* notes) {
    static char list[2048];
    if (!notes->player_pose_flag) snprintf(list, sizeof list, "%s", v);
    else {
        const size_t used = strlen(list);
        snprintf(list + used, sizeof list - used, ";%s", v);
    }
    flag_setenv("OPENPETE_PLAYER_POSE", list);
    notes->player_pose_flag = true;
}

/* parse_one_flag's result for an argument no arm accepts, which no config
 * fallback covers: an unknown flag is a typo to fix, not a value to replace. */
#define PARSE_UNKNOWN_ARG (-2)

/** @brief Parse the flag at argv[*ip] and its value, one arm per flag; *ip ends on the last token consumed.
 *
 * An argument no arm accepts returns PARSE_UNKNOWN_ARG; the message names
 * the openpete.toml table when the token came from the config splice. A
 * refused value returns OPENPETE_EXIT_SETUP_ERROR, possibly after the arm
 * wrote part of opts; parse_flag_chain restores them. Facts that are not
 * options go to notes.
 * @param opts The options block.
 * @param argc Argument count.
 * @param argv The spliced argument vector.
 * @param ip In: index of the flag. Out: index of its last token.
 * @param notes Receives the post-parse facts.
 * @return OPENPETE_EXIT_OK, OPENPETE_EXIT_SETUP_ERROR or PARSE_UNKNOWN_ARG. */
// complexity-ratchet: one chain of sibling arms, one per flag, where a new flag adds an arm and no other structure
static int parse_one_flag(openpete_options_t* opts, int argc, char** argv,
                          int* ip, parse_notes_t* notes) {
    int i = *ip;
    const char* a = argv[i];
    const char* v = NULL;

    if (strcmp(a, "--frame-limit") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->frame_limit = atoi(v); i++;
    } else if (strcmp(a, "--max-runtime-seconds") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->max_runtime_seconds = atoi(v); i++;
    } else if (strcmp(a, "--headless") == 0) {
        opts->headless = true;
    } else if (strcmp(a, "--renderer") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if      (strcmp(v, "psycross") == 0) opts->renderer = OPENPETE_RENDERER_PSYCROSS;
        else if (strcmp(v, "vk")       == 0) opts->renderer = OPENPETE_RENDERER_VK;
        else {
            fprintf(stderr, "openpete: --renderer expects psycross|vk, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--render-paths") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->render_paths = v; i++;
    } else if (strcmp(a, "--ssaa") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->ssaa = atoi(v); i++;
        if (opts->ssaa < 1) {
            fprintf(stderr, "openpete: --ssaa expects 1..%d (1 = off), got: %s\n",
                    OPENPETE_SSAA_MAX, v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        /* Clamp rather than reject: a config file may carry a higher
         * factor, and refusing to start is worse than rendering at the
         * highest safe one. */
        if (opts->ssaa > OPENPETE_SSAA_MAX) {
            fprintf(stderr, "openpete: --ssaa %s exceeds the %dx maximum (VRAM); using %dx\n",
                    v, OPENPETE_SSAA_MAX, OPENPETE_SSAA_MAX);
            opts->ssaa = OPENPETE_SSAA_MAX;
        }
    } else if (strcmp(a, "--resolution") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        i++;
        openpete_render_res_t rv;
        char msg[160];
        const int rc = openpete_render_res_parse(v, &rv, msg, sizeof msg);
        if (msg[0]) fprintf(stderr, "openpete: %s\n", msg);
        if (rc < 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->resolution = openpete_render_res_pack(&rv);
        /* window is the default path in every mode, so only N or WxH marks
         * the value as given for the headless captures. */
        opts->resolution_set = rv.kind != OPENPETE_RR_WINDOW;
    } else if (strcmp(a, "--osd") == 0) {
        const char *v = (i + 1 < argc) ? argv[++i] : "";
        if      (!strcmp(v, "on")  || !strcmp(v, "1")) opts->osd = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0")) opts->osd = 0;
        else {
            fprintf(stderr, "openpete: --osd expects on|off, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (strcmp(a, "--version-osd") == 0) {
        const char *v = (i + 1 < argc) ? argv[++i] : "";
        if      (!strcmp(v, "on")  || !strcmp(v, "1")) opts->version_osd = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0")) opts->version_osd = 0;
        else {
            fprintf(stderr, "openpete: --version-osd expects on|off, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (strcmp(a, "--osd-position") == 0) {
        const char *v = (i + 1 < argc) ? argv[++i] : "";
        int an = openpete_anchor_parse(v);
        if (an < 0) {
            fprintf(stderr, "openpete: --osd-position expects one of "
                    "top-left|top-center|top-right|bottom-left|bottom-center|"
                    "bottom-right, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->osd_position = an;
    } else if (strcmp(a, "--osd-timeout") == 0) {
        /* Seconds in, milliseconds out - the ONE conversion point. */
        const char *v = (i + 1 < argc) ? argv[++i] : "";
        char *end = NULL;
        double secs = strtod(v, &end);
        if (!v[0] || end == v || *end || !(secs > 0.0) ||
            secs > (double)OPENPETE_OSD_TIMEOUT_MAX_MS / 1000.0) {
            fprintf(stderr, "openpete: --osd-timeout expects seconds in "
                    "(0, %.0f], got: %s\n",
                    (double)OPENPETE_OSD_TIMEOUT_MAX_MS / 1000.0, v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->osd_timeout_ms = (uint32_t)(secs * 1000.0 + 0.5);
    } else if (strcmp(a, "--perf-position") == 0) {
        const char *v = (i + 1 < argc) ? argv[++i] : "";
        int an = openpete_anchor_parse(v);
        if (an < 0) {
            fprintf(stderr, "openpete: --perf-position expects one of "
                    "top-left|top-center|top-right|bottom-left|bottom-center|"
                    "bottom-right, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->perf_position = an;
    } else if (strcmp(a, "--perf-stats") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        /* Words are the documented spelling (and what the config file
         * writes back); the numbers are accepted because the level is an
         * ordered scale and scripts reach for it that way. */
        if      (!strcmp(v, "off")      || !strcmp(v, "0")) opts->perf_stats = OPENPETE_PERF_OFF;
        else if (!strcmp(v, "fps")      || !strcmp(v, "1")) opts->perf_stats = OPENPETE_PERF_FPS;
        else if (!strcmp(v, "detailed") || !strcmp(v, "2")) opts->perf_stats = OPENPETE_PERF_DETAIL;
        else {
            fprintf(stderr, "openpete: --perf-stats expects off|fps|detailed (0|1|2 also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--show-fps") == 0) {
        /* The two-state spelling this setting used to have. ON restores
         * the FPS level rather than the detailed one: a flag that only
         * ever meant "the counter" must not start turning on a readout
         * its author never asked for. */
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        /* Accept 0|1 (preferred) plus on|off, matching --mute. */
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->perf_stats = OPENPETE_PERF_FPS;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->perf_stats = OPENPETE_PERF_OFF;
        else {
            fprintf(stderr, "openpete: --show-fps expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--vsync") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        /* Accept 0|1 (preferred) plus on|off, matching --show-fps / --mute. */
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->vsync = 1;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->vsync = 0;
        else {
            fprintf(stderr, "openpete: --vsync expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--gpu") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->gpu = v; i++;
    } else if (strcmp(a, "--fullscreen") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        /* Accept 0|1 (preferred) plus on|off, matching --vsync / --mute. */
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->fullscreen = 1;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->fullscreen = 0;
        else {
            fprintf(stderr, "openpete: --fullscreen expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--render-fps") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->render_fps = atoi(v); i++;
        if (opts->render_fps < 0) {
            fprintf(stderr, "openpete: --render-fps expects a non-negative integer\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (strcmp(a, "--tick-scale") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_tick_scale(a, v, &opts->tick_scale) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--window") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_window_value(a, v, &opts->window_w, &opts->window_h,
                               &opts->window_maximized) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--launcher-window") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_window_value(a, v, &opts->launcher_window_w, &opts->launcher_window_h,
                               &opts->launcher_window_maximized) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--launcher-fullscreen") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->launcher_fullscreen = 1;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->launcher_fullscreen = 0;
        else {
            fprintf(stderr, "openpete: --launcher-fullscreen expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--aspect") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (strcmp(v, "auto") == 0) {
            /* Explicit auto = same as unset: the render aspect follows the
             * window / fullscreen shape (Hor+ fill, the default). */
            opts->aspect_w_over_h = 0.0f;
            i++;
        } else {
            int aw = 0, ah = 0;
            if (sscanf(v, "%d:%d", &aw, &ah) != 2 || aw <= 0 || ah <= 0) {
                fprintf(stderr, "openpete: --aspect expects W:H (e.g. 16:9) or auto, got: %s\n", v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->aspect_w_over_h = (float)aw / (float)ah;
            i++;
        }
    } else if (strcmp(a, "--fov") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_finite_clamped(a, v, 30.0f, 120.0f, true, &opts->fov_deg) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--vertex-precision") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (strcmp(v, "subpixel") == 0) {
            opts->vertex_precision = OPENPETE_VERTEX_PRECISION_SUBPIXEL;
        } else if (strcmp(v, "integer") == 0) {
            opts->vertex_precision = OPENPETE_VERTEX_PRECISION_INTEGER;
        } else {
            fprintf(stderr, "openpete: --vertex-precision expects subpixel|integer, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--raster-quantize") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        long mult = strtol(v, NULL, 10);
        if (mult < 0 || mult > 16) {
            fprintf(stderr, "openpete: --raster-quantize expects 0 (off) or a grid multiplier in [1,16], got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->raster_quantize = (int)mult;
        i++;
    } else if (strcmp(a, "--extract-threads") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        long n = strtol(v, NULL, 10);
        if (n < 0 || n > 8) {
            fprintf(stderr, "openpete: --extract-threads expects 0 (auto), 1 (serial) or a count in [2,8], got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->extract_threads = (int)n;
        i++;
    } else if (strcmp(a, "--dither") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->dither = (atoi(v) != 0);
        i++;
    } else if (strcmp(a, "--hide-player") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->hide_player = (atoi(v) != 0);
        i++;
    } else if (strcmp(a, "--hide-hud") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->hide_hud = (atoi(v) != 0);
        i++;
    } else if (strcmp(a, "--hide-classes") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->hide_classes = v; i++;
    } else if (strcmp(a, "--notex") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->notex = (atoi(v) != 0);
        i++;
    } else if (strcmp(a, "--wireframe") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->wireframe = (atoi(v) != 0);
        i++;
    } else if (strcmp(a, "--psycross-native-raster") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->psycross_native_raster = (atoi(v) != 0);
        i++;
    } else if (strcmp(a, "--texture-mapping") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (strcmp(v, "perspective") == 0) {
            opts->texture_mapping = OPENPETE_TEXTURE_MAPPING_PERSPECTIVE;
        } else if (strcmp(v, "affine") == 0) {
            opts->texture_mapping = OPENPETE_TEXTURE_MAPPING_AFFINE;
        } else {
            fprintf(stderr, "openpete: --texture-mapping expects perspective|affine, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--lod") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (strcmp(v, "original") == 0) {
            opts->lod = OPENPETE_LOD_ORIGINAL;
        } else if (strcmp(v, "extended") == 0) {
            opts->lod = OPENPETE_LOD_EXTENDED;
        } else {
            fprintf(stderr, "openpete: --lod expects original|extended, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--fog") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (fog_mode_parse(v, &opts->fog_mode) != 0) {
            fprintf(stderr, "openpete: --fog expects original|per-pixel|off, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--sector-visibility") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (sector_vis_parse(v, &opts->sector_visibility) != 0) {
            fprintf(stderr, "openpete: --sector-visibility expects auto|game|frustum|filled, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--cadence") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (cadence_parse(v, &opts->cadence) != 0) {
            fprintf(stderr, "openpete: --cadence expects even|fill, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--no-lod") == 0) {
        opts->lod = OPENPETE_LOD_EXTENDED;   /* deprecated alias for --lod extended */
    } else if (strcmp(a, "--disc-cache") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (disc_cache_parse(v, &opts->disc_cache) != 0) {
            fprintf(stderr, "openpete: --disc-cache expects 0|1|auto, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--load-delay") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->load_delay = atoi(v); i++;
        if (opts->load_delay < 0) {
            fprintf(stderr, "openpete: --load-delay must be >= 0\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (strcmp(a, "--terrain-cache") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->terrain_cache = atoi(v) != 0; i++;
    } else if (strcmp(a, "--fog-scale") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_finite_clamped(a, v, 0.25f, 128.0f, false, &opts->fog_scale) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--screenshot-on-frame") == 0 ||
               strcmp(a, "--screenshot-on-tick") == 0) {
        bool ticks = (a[16] == 't');   /* "--screenshot-on-[t]ick" */
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (opts->screenshot_frame_count > 0 &&
            opts->screenshot_ticks_keyed != ticks) {
            fprintf(stderr, "openpete: --screenshot-on-frame and --screenshot-on-tick "
                            "are mutually exclusive\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->screenshot_frame_count = 0;
        opts->screenshot_ticks_keyed = ticks;
        const char* p = v;
        while (*p) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            if (opts->screenshot_frame_count >= OPENPETE_MAX_SCREENSHOT_FRAMES) {
                fprintf(stderr, "openpete: %s supports up to %d frames\n",
                        a, OPENPETE_MAX_SCREENSHOT_FRAMES);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            char* end = NULL;
            long n = strtol(p, &end, 10);
            if (end == p || n < 0) {
                fprintf(stderr, "openpete: %s expects N[,N2,...], got: %s\n", a, v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->screenshot_frames[opts->screenshot_frame_count++] = (int)n;
            p = end;
        }
        i++;
    } else if (strcmp(a, "--screenshot-intermediate-on-frame") == 0) {
        /* Deterministic sub-tick (alpha 0.5) native capture targets, VBL-keyed.
         * Fills a separate list from --screenshot-on-frame; implies native-only. */
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->screenshot_intermediate_count = 0;
        const char* p = v;
        while (*p) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            if (opts->screenshot_intermediate_count >= OPENPETE_MAX_SCREENSHOT_FRAMES) {
                fprintf(stderr, "openpete: %s supports up to %d frames\n",
                        a, OPENPETE_MAX_SCREENSHOT_FRAMES);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            char* end = NULL;
            long n = strtol(p, &end, 10);
            if (end == p || n < 0) {
                fprintf(stderr, "openpete: %s expects N[,N2,...], got: %s\n", a, v);
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            opts->screenshot_intermediate_frames[opts->screenshot_intermediate_count++] = (int)n;
            p = end;
        }
        opts->screenshot_native_only = true;   /* intermediate capture is vk-only */
        i++;
    } else if (strcmp(a, "--screenshot-native-only") == 0) {
        opts->screenshot_native_only = true;
    } else if (strcmp(a, "--screenshot-freeze-backdrop") == 0) {
        /* Accepted with no effect: every headless capture keeps the backdrop. */
    } else if (strcmp(a, "--screenshot-output") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->screenshot_output = v; i++;
    } else if (strcmp(a, "--screenshot-presents-start") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->screenshot_presents_start = atoi(v); i++;
    } else if (strcmp(a, "--screenshot-presents-count") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->screenshot_presents_count = atoi(v); i++;
    } else if (strcmp(a, "--ram-dump-on-frame") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->ram_dump_on_frame = atoi(v); i++;
    } else if (strcmp(a, "--ram-dump-output") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->ram_dump_output = v; i++;
    } else if (strcmp(a, "--state-save-on-frame") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->state_save_on_frame = atoi(v); i++;
    } else if (strcmp(a, "--state-save-output") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->state_save_output = v; i++;
    } else if (strcmp(a, "--state-load-on-frame") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->state_load_on_frame = atoi(v); i++;
    } else if (strcmp(a, "--state-load-input") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->state_load_input = v; i++;
    } else if (strcmp(a, "--state-stress") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->state_stress = atoi(v); i++;
    } else if (strcmp(a, "--state-stress-rewind") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->state_stress_rewind = atoi(v); i++;
    } else if (strcmp(a, "--rewind") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->rewind_window = atoi(v); i++;
    } else if (strcmp(a, "--runahead") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->runahead = atoi(v);
        if (opts->runahead < 0) opts->runahead = 0;
        if (opts->runahead > 3) opts->runahead = 3;
        i++;
    } else if (strcmp(a, "--rewind-hold") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (sscanf(v, "%d:%d", &opts->rewind_hold_frame,
                   &opts->rewind_hold_iters) != 2) {
            fprintf(stderr, "openpete: --rewind-hold expects A:N, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--seed-ram") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->seed_ram_path = v; i++;
    } else if (strcmp(a, "--seed-cpu") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->seed_cpu_path = v; i++;
    } else if (strcmp(a, "--halt-at-pc") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->halt_at_pc = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--halt-cpu-dump-output") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->halt_cpu_dump_output = v; i++;
    } else if (strcmp(a, "--halt-hit") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->halt_hit_target = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--log-format") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if      (strcmp(v, "json")  == 0) opts->log_format = OPENPETE_LOG_JSON;
        else if (strcmp(v, "plain") == 0) opts->log_format = OPENPETE_LOG_PLAIN;
        else {
            fprintf(stderr, "openpete: unknown --log-format value: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--log-level") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->log_level_spec = v; i++;
    } else if (strcmp(a, "--log-file") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->log_file = v; i++;
    } else if (strcmp(a, "--no-log-file") == 0) {
        opts->log_file = "off";
    } else if (strcmp(a, "--disc") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->disc_path = v; i++;
    } else if (strcmp(a, "--exe-path") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->exe_path = v; i++;
    } else if (strcmp(a, "--recomp-module") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->recomp_module_path = v; i++;
    } else if (strcmp(a, "--ingest") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->ingest_path = v; i++;
    } else if (strcmp(a, "--game") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->game_selector = v; i++;
    } else if (strcmp(a, "--extract-recomp-inputs") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->extract_recomp_inputs_dir = v; i++;
    } else if (strcmp(a, "--test-recomp-call") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->test_recomp_call_addr = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--test-recomp-expect-v0") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->test_recomp_expected_v0 = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--test-recomp-gp") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->test_recomp_gp = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--probe") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->probe_addr = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--test-segv") == 0) {
        opts->test_recomp_segv = true;
    } else if (strcmp(a, "--freecam-path") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        flag_setenv("OPENPETE_FREECAM_PATH", v);
        i++;
    } else if (strcmp(a, "--freecam-pose") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        char why[256];
        if (openpete_pose_text_check(v, why, sizeof why) != 0) {
            fprintf(stderr, "openpete: --freecam-pose: %s\n", why);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        flag_setenv("OPENPETE_FREECAM_POSE", v);
        i++;
    } else if (strcmp(a, "--player-pose") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        player_pose_add(v, notes);
        i++;
    } else if (strcmp(a, "--debug-triangle") == 0) {
        opts->debug_triangle = true;
    } else if (strcmp(a, "--skip-to-level") == 0) {
        /* No value (end of argv, or the next word is a flag): print the
         * target list, as --help does. Level names never start with '-'. */
        if (i + 1 >= argc || argv[i + 1][0] == '-') {
            (void)game_resolve_level_arg("");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        v = argv[i + 1];
        opts->skip_to_level = game_resolve_level_arg(v);
        if (opts->skip_to_level < 0) {
            fprintf(stderr, "openpete: --skip-to-level: unknown level '%s'\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--skip-to-cutscene") == 0) {
        if (i + 1 >= argc || argv[i + 1][0] == '-') {
            (void)game_resolve_cutscene_arg("");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        v = argv[i + 1];
        opts->skip_to_cutscene = game_resolve_cutscene_arg(v);
        if (opts->skip_to_cutscene < 0) {
            fprintf(stderr, "openpete: --skip-to-cutscene: unknown cutscene '%s'\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--skip-to-area") == 0) {
        if (i + 1 >= argc || argv[i + 1][0] == '-') {
            (void)game_resolve_area_arg("", &opts->skip_to_area);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        v = argv[i + 1];
        opts->skip_to_area_level = game_resolve_area_arg(v, &opts->skip_to_area);
        if (opts->skip_to_area_level < 0) {
            fprintf(stderr, "openpete: --skip-to-area: no sub-area '%s'\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--cheats") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (game_validate_cheats_arg(v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->cheats = v; i++;
    } else if (strcmp(a, "--bk2") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->bk2_path = v; i++;
    } else if (strcmp(a, "--bk2-frame-offset") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->bk2_frame_offset = atoi(v);
        opts->bk2_frame_offset_set = true;
        i++;
    } else if (strcmp(a, "--bk2-stop-on-end") == 0) {
        opts->bk2_stop_on_end = true;
    } else if (strcmp(a, "--bk2-record") == 0) {
        /* Optional PATH: consume the next token only when it isn't
         * another flag. With no path, the default
         * "<bk2 data dir>/<timestamp>.bk2" is resolved after the loop. */
        notes->bk2_record_requested = true;
        if (i + 1 < argc && argv[i + 1][0] != '-') {
            opts->bk2_record_path = argv[i + 1]; i++;
        }
    } else if (strcmp(a, "--record") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->record_path = v; i++;
    } else if (strcmp(a, "--record-encoder") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->record_encoder = v; i++;
    } else if (strcmp(a, "--record-crf") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->record_crf = atoi(v); i++;
    } else if (strcmp(a, "--record-gop") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->record_gop = atoi(v); i++;
        if (opts->record_gop < 0) {
            fprintf(stderr, "openpete: --record-gop expects a non-negative integer\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (strcmp(a, "--record-pixfmt") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->record_pixfmt = v; i++;
    } else if (strcmp(a, "--no-record-audio") == 0) {
        opts->record_no_audio = true;
    } else if (strcmp(a, "--record-audio-encoder") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->record_audio_encoder = v; i++;
    } else if (strcmp(a, "--devui-open") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->devui_open = 1;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->devui_open = 0;
        else {
            fprintf(stderr, "openpete: --devui-open expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--record-fps-overlay") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        /* Accept 0|1 (preferred) plus on|off, matching --vsync. */
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->record_fps_overlay = 1;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->record_fps_overlay = 0;
        else {
            fprintf(stderr, "openpete: --record-fps-overlay expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--record-cfr") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        double rate = atof(v);
        /* Only exact multiples of the NTSC tick rate are representable as a
         * whole number of presents per sim tick; anything else would have
         * the interpolator emit 2 intermediates on some ticks and 3 on
         * others. Reject rather than round, so a typo is loud. */
        double mult = rate / OP_TICK_HZ;
        long   k    = lround(mult);
        /* Relative tolerance so the rounded spellings people actually type
         * - 30, 60, 120 - all land on their NTSC neighbour, while a genuine
         * non-multiple like 45 is refused. 60 sits 0.29 % above 2x (the
         * true tick rate is 29.913, not 30), so the window has to clear
         * that; 0.5 % does, and still rejects 45 (50 % out) and 61 (2 %). */
        if (rate <= 0.0 || k < 1 || k > OPENPETE_RECORD_CFR_MAX_MULT ||
            fabs(mult - (double)k) > 0.005 * (double)k) {
            fprintf(stderr,
                    "openpete: --record-cfr expects an integer multiple of the NTSC "
                    "tick rate 1125000/37609 (29.913, 59.826, 119.652, ... up to %dx), "
                    "got: %s\n",
                    OPENPETE_RECORD_CFR_MAX_MULT, v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->record_cfr = (double)k * OP_TICK_HZ;
        if (fabs(opts->record_cfr - rate) > 0.001)
            fprintf(stderr, "openpete: --record-cfr %s -> %.6f fps (NTSC %ldx)\n",
                    v, opts->record_cfr, k);
        i++;
    } else if (strcmp(a, "--record-frames") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->record_frames = atoi(v); i++;
        if (opts->record_frames < 0) {
            fprintf(stderr, "openpete: --record-frames expects a non-negative integer\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (strcmp(a, "--sfxpack-dir") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->sfxpack_dir = v; i++;
    } else if (strcmp(a, "--texpack-dir") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->texpack_dir = v; i++;
    } else if (strcmp(a, "--controller-db") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->controller_db = v; i++;
    } else if (strcmp(a, "--mods-dir") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->mods_dir = v; i++;
    } else if (strcmp(a, "--mods") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->mods_enabled = v; i++;
    } else if (strcmp(a, "--scan-guard") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (strcmp(v, "0") != 0 && strcmp(v, "1") != 0) {
            fprintf(stderr, "openpete: --scan-guard takes 0 or 1, got '%s'\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->scan_guard = v[0] == '1'; i++;
    } else if (strcmp(a, "--mod-hot-reload") == 0) {
        /* Optional value, as --input-lock: an explicit 0 overrides the env spelling. */
        opts->mod_hot_reload = 1;
        if (i + 1 < argc) {
            const char* n = argv[i + 1];
            if      (strcmp(n, "1") == 0 || strcmp(n, "on")  == 0) { opts->mod_hot_reload = 1; i++; }
            else if (strcmp(n, "0") == 0 || strcmp(n, "off") == 0) { opts->mod_hot_reload = 0; i++; }
        }
    } else if (strcmp(a, "--compile-mods") == 0) {
        opts->compile_mods = 1;
    } else if (strcmp(a, "--dump-sfx-pack") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->dump_sfx_pack_dir = v; i++;
    } else if (strcmp(a, "--dump-sfx-wads") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->dump_sfx_wads = v; i++;
    } else if (strcmp(a, "--config") == 0) {
        /* Consumed by the config loader before this parse; accepted
         * here so it isn't an unknown flag. */
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--no-config") == 0) {
        /* likewise */
    } else if (strcmp(a, "--config-report") == 0) {
        opts->config_report = 1;
    } else if (strcmp(a, "--keys-report") == 0) {
        opts->keys_report = 1;
    } else if (strcmp(a, "--card-dir") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->card_dir = v; i++;
    } else if (strcmp(a, "--card1") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->card1_path = v; i++;
    } else if (strcmp(a, "--card2") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->card2_path = v; i++;
    } else if (strcmp(a, "--no-card") == 0) {
        opts->card_a_present = 0;
        opts->card_b_present = 0;
    } else if (strcmp(a, "--no-card1") == 0) {
        opts->card_a_present = 0;
    } else if (strcmp(a, "--no-card2") == 0) {
        opts->card_b_present = 0;
    } else if (strcmp(a, "--card-unformatted") == 0) {
        opts->card_unformatted = 1;
    } else if (strcmp(a, "--adopt-saves") == 0) {
        opts->adopt_saves = 1;
    } else if (strcmp(a, "--save-mismatch-toast") == 0) {
        const char *v = (i + 1 < argc) ? argv[++i] : "";
        if      (!strcmp(v, "on")  || !strcmp(v, "1")) opts->save_mismatch_toast = 1;
        else if (!strcmp(v, "off") || !strcmp(v, "0")) opts->save_mismatch_toast = 0;
        else {
            fprintf(stderr, "openpete: --save-mismatch-toast expects on|off, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (strcmp(a, "--resume-handoff") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->resume_handoff = v; i++;
    } else if (strcmp(a, "--bug-report-replay") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->bug_report_replay = v; i++;
    } else if (strcmp(a, "--bug-report-replay-beyond") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        char* end = NULL;
        const unsigned long n = strtoul(v, &end, 10);
        if (!*v || *end || v[0] == '-' || n > 1000000ul) {
            fprintf(stderr, "openpete: --bug-report-replay-beyond expects a tick count 0..1000000, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->bug_report_replay_beyond = (unsigned)n; i++;
    } else if (strcmp(a, "--unlock-frames") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->unlock_frames_spec = v;
        /* Parse "A-B,C-D,..." into ranges. Permissive: skip empty
         * tokens. Cap at OPENPETE_UNLOCK_RANGES_MAX. */
        opts->unlock_ranges_count = 0;
        const char* p = v;
        while (*p && opts->unlock_ranges_count < OPENPETE_UNLOCK_RANGES_MAX) {
            while (*p == ',' || *p == ' ') p++;
            if (!*p) break;
            char* end = NULL;
            long lo = strtol(p, &end, 10);
            if (end == p) break;
            p = end;
            long hi = lo;
            if (*p == '-') {
                p++;
                hi = strtol(p, &end, 10);
                if (end == p) break;
                p = end;
            }
            if (hi < lo) { long t = lo; lo = hi; hi = t; }
            opts->unlock_ranges[opts->unlock_ranges_count].lo = (int)lo;
            opts->unlock_ranges[opts->unlock_ranges_count].hi = (int)hi;
            opts->unlock_ranges_count++;
        }
        i++;
    } else if (strcmp(a, "--position-log") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->position_log_path = v; i++;
    } else if (strcmp(a, "--gameplay-hash-log") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->gameplay_hash_log_path = v; i++;
    } else if (strcmp(a, "--frame-time-log") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->frame_time_log_path = v; i++;
    } else if (strcmp(a, "--pace-log") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->pace_log_path = v; i++;
    } else if (strcmp(a, "--profile-out") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->profile_out_path = v; i++;
    } else if (strcmp(a, "--psxtrace-verify") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->psxtrace_verify_path = v; i++;
    } else if (strcmp(a, "--psxtrace-keep-going") == 0) {
        opts->psxtrace_keep_going = true;
    } else if (strcmp(a, "--psxtrace-anchor-pc") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->psxtrace_anchor_pc = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--psxtrace-anchor-hit") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        opts->psxtrace_anchor_hit = (uint32_t)strtoul(v, NULL, 0); i++;
    } else if (strcmp(a, "--mute") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        /* Accept 0|1 (preferred) plus the legacy on|off so existing
         * scripts/movies keep working. */
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->mute = true;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->mute = false;
        else {
            fprintf(stderr, "openpete: --mute expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--sfx") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_volume_arg(a, v, &opts->sfx_volume) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--music") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_volume_arg(a, v, &opts->music_volume) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--vibration") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (parse_volume_arg(a, v, &opts->vibration) != 0)
            return OPENPETE_EXIT_SETUP_ERROR;
        i++;
    } else if (strcmp(a, "--mouse") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) opts->mouse = 1;
        else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) opts->mouse = 0;
        else {
            fprintf(stderr, "openpete: --mouse expects 0|1 (on|off also accepted), got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--input-lock") == 0) {
        /* The value is optional: the bare flag turns the lock on, and an
         * explicit 0 is how the command line overrides the env spelling. */
        opts->input_lock = 1;
        if (i + 1 < argc) {
            const char* n = argv[i + 1];
            if      (strcmp(n, "1") == 0 || strcmp(n, "on")  == 0) { opts->input_lock = 1; i++; }
            else if (strcmp(n, "0") == 0 || strcmp(n, "off") == 0) { opts->input_lock = 0; i++; }
        }
    } else if (strcmp(a, "--pad-unfocused") == 0) {
        /* Optional value, as --input-lock. */
        opts->pad_unfocused = 1;
        if (i + 1 < argc) {
            const char* n = argv[i + 1];
            if      (strcmp(n, "1") == 0 || strcmp(n, "on")  == 0) { opts->pad_unfocused = 1; i++; }
            else if (strcmp(n, "0") == 0 || strcmp(n, "off") == 0) { opts->pad_unfocused = 0; i++; }
        }
    } else if (strcmp(a, "--stick-response") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        if (strcmp(v, "linear") == 0) {
            opts->stick_response = OPENPETE_STICK_RESPONSE_LINEAR;
        } else if (strcmp(v, "console") == 0) {
            opts->stick_response = OPENPETE_STICK_RESPONSE_CONSOLE;
        } else {
            fprintf(stderr, "openpete: --stick-response expects linear|console, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        i++;
    } else if (strcmp(a, "--screenshot-overlays") == 0) {
        /* Optional value, as --input-lock. */
        opts->screenshot_overlays = 1;
        if (i + 1 < argc) {
            const char* n = argv[i + 1];
            if      (strcmp(n, "1") == 0 || strcmp(n, "on")  == 0) { opts->screenshot_overlays = 1; i++; }
            else if (strcmp(n, "0") == 0 || strcmp(n, "off") == 0) { opts->screenshot_overlays = 0; i++; }
        }
    } else if (strcmp(a, "--background") == 0) {
        /* Optional value, as --input-lock. */
        opts->background = 1;
        if (i + 1 < argc) {
            const char* n = argv[i + 1];
            if      (strcmp(n, "1") == 0 || strcmp(n, "on")  == 0) { opts->background = 1; i++; }
            else if (strcmp(n, "0") == 0 || strcmp(n, "off") == 0) { opts->background = 0; i++; }
        }
    } else if (strcmp(a, "--mouse-sensitivity") == 0) {
        if (needs_value(a, argc, argv, i, &v) != 0) return OPENPETE_EXIT_SETUP_ERROR;
        char* end = NULL;
        double d = strtod(v, &end);
        if (!end || *end || !(d >= 0.0) || d > 100.0) {
            fprintf(stderr, "openpete: --mouse-sensitivity expects a number "
                    "in 0..100, got: %s\n", v);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->mouse_sensitivity = (float)d;
        i++;
    } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
        print_help(opts->display_name, argv[0], opts->default_renderer);
        opts->help = 1;
        return OPENPETE_EXIT_OK;
    } else if (a[0] != '-' && !opts->ingest_path && !opts->disc_path
               && !opts->exe_path && arg_is_disc_image(a)) {
        /* A bare path to a disc image - what a desktop file's %f, a
         * file manager's "Open with" and a drag onto the icon all
         * deliver. Same meaning as --ingest: the library dedups by
         * content, so re-opening an already-installed disc just boots
         * it. */
        opts->ingest_path = a;
    } else {
        /* Config-spliced tokens were never typed, so pointing at the
         * command line would send the user hunting in the wrong place. */
        const char* src = openpete_config_token_source(a);
        if (src)
            fprintf(stderr, "openpete: unknown arg: %s (from openpete.toml "
                            "[%s])\n", a, src);
        else
            fprintf(stderr, "openpete: unknown arg: %s\n", a);
        return PARSE_UNKNOWN_ARG;
    }
    *ip = i;
    return OPENPETE_EXIT_OK;
}

/* 1 = a refused config or env value fails the parse instead of warning and
 * keeping the default (openpete_options_set_strict). */
static int s_strict_values;

void openpete_options_set_strict(int on) { s_strict_values = on != 0; }

/** @brief Whether a refused config or env value fails the parse: under strict mode, and under --config-report, which is how a user checks a file. */
static int refusals_fail(void) {
    return s_strict_values || openpete_config_report_requested();
}

/** @brief Warn that the config file's flag at argv[at] was refused and step *ip past it and its value.
 *
 * A flag the config splice put into argv is a stored setting: refusing it
 * would stop a config file from booting the game, so the caller keeps the
 * option's previous value (a refused value) or drops the flag (an unknown
 * [args] flag). The warning names the config key, the value and the
 * openpete.toml table it came from. A typed or bk2-sidecar flag is not
 * spliced and is refused by the caller, as is every flag under strict mode
 * and under --config-report (refusals_fail).
 * @param argc Argument count.
 * @param argv The spliced argument vector.
 * @param at Index of the refused flag.
 * @param unknown 1 when no arm knows the flag, 0 when its value was refused.
 * @param ip Receives the index of the flag's last token.
 * @return 1 when the flag came from the config file and falls back, else 0. */
static int config_value_falls_back(int argc, char** argv, int at, int unknown,
                                   int* ip) {
    const char* src = openpete_config_token_source(argv[at]);
    if (!src || refusals_fail()) return 0;
    const char* v = NULL;
    if (at + 1 < argc && strncmp(argv[at + 1], "--", 2) != 0 &&
        openpete_config_token_source(argv[at + 1]))
        v = argv[at + 1];
    int ki = cfg_schema_find_cli(argv[at]);
    const char* key = ki >= 0 ? cfg_schema(NULL)[ki].id : argv[at];
    openpete_config_diag("openpete: openpete.toml [%s]: %s = \"%s\" %s\n", src, key,
                         v ? v : "", unknown ? "is not a known flag; ignored"
                                             : "is not accepted; keeping the default");
    *ip = v ? at + 1 : at;
    return 1;
}

/** @brief Skip a flag whose value names a target of the running game while the game-neutral parse runs.
 *
 * --skip-to-level, --skip-to-cutscene, --skip-to-area and --cheats resolve
 * their value through the installed game's tables. The game-neutral parse
 * runs before any game is installed when no --disc names it (--game alone,
 * or the launcher's pick), so it consumes the flag and its value unread; the
 * full parse after identification resolves them.
 * @param argc Argument count.
 * @param argv The spliced argument vector.
 * @param ip In: index of the flag. Out: index of its last token when skipped.
 * @return 1 when the flag was skipped, else 0. */
static int neutral_skip_game_flag(int argc, char** argv, int* ip) {
    static const char* const k_game_flags[] = {
        "--skip-to-level", "--skip-to-cutscene", "--skip-to-area", "--cheats",
    };
    if (!s_parse_neutral) return 0;
    const char* a = argv[*ip];
    for (size_t k = 0; k < sizeof k_game_flags / sizeof k_game_flags[0]; k++) {
        if (strcmp(a, k_game_flags[k]) != 0) continue;
        if (*ip + 1 < argc && argv[*ip + 1][0] != '-') (*ip)++;
        return 1;
    }
    return 0;
}

/** @brief Walk the spliced argv left to right, the last write winning.
 *
 * A flag whose value is refused fails the parse, except a flag the config
 * file supplied: its arm's writes are undone and the option keeps the value
 * it had (config_value_falls_back). An unknown argument fails unless the
 * config file supplied it, which is dropped with a warning.
 * --help stops the walk.
 * @param opts The options block.
 * @param argc Argument count.
 * @param argv The spliced argument vector.
 * @param notes Receives the post-parse facts.
 * @return OPENPETE_EXIT_OK or OPENPETE_EXIT_SETUP_ERROR. */
static int parse_flag_chain(openpete_options_t* opts, int argc, char** argv,
                            parse_notes_t* notes) {
    for (int i = 1; i < argc && !opts->help; i++) {
        openpete_options_t before;
        memcpy(&before, opts, sizeof before);
        const int at = i;
        if (neutral_skip_game_flag(argc, argv, &i)) continue;
        const int rc = parse_one_flag(opts, argc, argv, &i, notes);
        if (rc == OPENPETE_EXIT_OK) continue;
        if (!config_value_falls_back(argc, argv, at, rc == PARSE_UNKNOWN_ARG, &i))
            return OPENPETE_EXIT_SETUP_ERROR;
        memcpy(opts, &before, sizeof before);
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Complete a --bk2-record request: default path, then the .args sidecar text.
 *
 * Runs after the flag chain has validated the command line, so a bad
 * invocation creates no directory. A NULL sidecar (allocation failure) keeps
 * the recording without a sidecar. OPENPETE_PLAYER_POSE set without a typed
 * --player-pose fails the parse, since the sidecar carries argv only and the
 * replay would start from the spawn.
 * @param opts The options block.
 * @param orig_argc The typed argument count.
 * @param orig_argv The typed argument vector.
 * @param notes The flag chain's post-parse facts. */
static int finish_bk2_record(openpete_options_t* opts, int orig_argc,
                             char** orig_argv, const parse_notes_t* notes) {
    if (!notes->bk2_record_requested) return OPENPETE_EXIT_OK;
    if (!opts->bk2_record_path || !*opts->bk2_record_path) {
        char* def = resolve_default_bk2_record_path();
        if (!def) {
            fprintf(stderr, "openpete: --bk2-record: out of memory resolving default path\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        opts->bk2_record_path = def;  /* leaked for program lifetime */
        fprintf(stderr, "openpete: --bk2-record: recording to %s\n", def);
    }
    opts->bk2_record_args = build_bk2_record_args(orig_argc, orig_argv);
    /* NULL (OOM) just means no sidecar - the recording itself still works. */
    /* The sidecar carries argv, not the environment: a placement given
     * only as OPENPETE_PLAYER_POSE would be missing from the replay, which
     * then runs the movie's input from the spawn instead. */
    const char* pp = getenv("OPENPETE_PLAYER_POSE");
    if (pp && *pp && !notes->player_pose_flag) {
        fprintf(stderr, "openpete: --bk2-record: OPENPETE_PLAYER_POSE is set but a bk2 "
                        "replays only the command line; pass --player-pose \"%s\" instead\n", pp);
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Answer a refused env-spelling value: warn and keep the default, or fail under strict mode.
 *
 * The variable may come from the environment or from an openpete.toml [env]
 * table; either way it is a stored setting, refused by the same rules as the
 * flag, and a stored setting that stops the game from booting is worse than
 * the default.
 * @param name The env var.
 * @param v Its value.
 * @return OPENPETE_EXIT_OK after the warning, or OPENPETE_EXIT_SETUP_ERROR when refusals_fail. */
static int env_value_refused(const char* name, const char* v) {
    if (refusals_fail()) return OPENPETE_EXIT_SETUP_ERROR;
    openpete_config_diag("openpete: %s = \"%s\" is not accepted; keeping the default\n",
                         name, v);
    return OPENPETE_EXIT_OK;
}

/** @brief Apply the env spelling of an on/off flag when the flag was not typed (*slot < 0).
 *
 * The CLI beats the env. The variable accepts 1, 0, on or off; an empty or
 * unset variable leaves the slot alone and any other value is refused
 * through env_value_refused. A harness that launches the game from several
 * sites sets the variable once at the top of the script instead of
 * threading an argument into each site.
 * @param name The env var.
 * @param slot The tri-state option field. */
static int env_onoff_fallback(const char* name, int* slot) {
    if (*slot >= 0) return OPENPETE_EXIT_OK;
    const char* v = openpete_config_env(name);
    if (!v || !*v) return OPENPETE_EXIT_OK;
    if      (strcmp(v, "1") == 0 || strcmp(v, "on")  == 0) *slot = 1;
    else if (strcmp(v, "0") == 0 || strcmp(v, "off") == 0) *slot = 0;
    else {
        openpete_config_diag("openpete: %s expects 0|1 (on|off also accepted), got: %s\n",
                             name, v);
        return env_value_refused(name, v);
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Refuse an OPENPETE_FREECAM_POSE the pose grammar rejects: warn and unset it, or fail under strict mode.
 *
 * A typed --freecam-pose was checked by its arm before it set the variable,
 * so only an exported or [env] value reaches the refusal. Unsetting the
 * variable keeps the default, the camera following the player, instead of
 * the runtime reader's reading of the malformed text. The game-neutral parse
 * writes no environment and only reports.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR when refusals_fail. */
static int env_freecam_pose_check(void) {
    const char* v = openpete_config_env("OPENPETE_FREECAM_POSE");
    char why[256];
    if (!v || !*v || openpete_pose_text_check(v, why, sizeof why) == 0) return OPENPETE_EXIT_OK;
    openpete_config_diag("openpete: OPENPETE_FREECAM_POSE: %s\n", why);
    if (env_value_refused("OPENPETE_FREECAM_POSE", v) != OPENPETE_EXIT_OK)
        return OPENPETE_EXIT_SETUP_ERROR;
    if (!s_parse_neutral) {
#ifdef _WIN32
        _putenv_s("OPENPETE_FREECAM_POSE", "");
#else
        unsetenv("OPENPETE_FREECAM_POSE");
#endif
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Apply the env spellings of dual-spelling flags the command line left unset.
 *
 * Runs after the flag chain, so a typed flag has already claimed its slot.
 * Covers OPENPETE_VIBRATION, OPENPETE_MOUSE, OPENPETE_INPUT_LOCK,
 * OPENPETE_BACKGROUND, OPENPETE_PAD_UNFOCUSED and OPENPETE_SCREENSHOT_OVERLAYS,
 * turns the input lock on for a --psxtrace-verify run that left it unset,
 * and refuses --input-lock with OPENPETE_INPUT_LEGACY.
 * @param opts The options block. */
static int apply_env_spellings(openpete_options_t* opts) {
    /* OPENPETE_VIBRATION: the env spelling of --vibration, applied only when
     * the flag was not given (CLI beats env, the project-wide rule).
     *
     * The env spelling exists for scripted launches. A gate or bench harness
     * needs one line it can put at the top of the file, not an argument
     * threaded into however many invocation sites it builds - and an argument
     * appended to the wrong one of those is a silent miss, which for this knob
     * means a connected controller buzzing for the length of a corpus
     * run. (Under WSL an env knob only reaches a Windows .exe if WSLENV names
     * it; scripts/fences/wslenv_guard.py is what enforces that half.) */
    if (opts->vibration < 0) {
        const char* v = openpete_config_env("OPENPETE_VIBRATION");
        if (v && *v) {
            int level = -1;
            if (parse_volume_arg("OPENPETE_VIBRATION", v, &level) != 0)
                return env_value_refused("OPENPETE_VIBRATION", v);
            opts->vibration = level;
        }
    }
    /* OPENPETE_MOUSE matters to captures: a stray pointer movement over the
     * window would otherwise steer the free camera mid-run. */
    if (env_onoff_fallback("OPENPETE_MOUSE", &opts->mouse) != 0
        || env_onoff_fallback("OPENPETE_INPUT_LOCK", &opts->input_lock) != 0
        || env_onoff_fallback("OPENPETE_BACKGROUND", &opts->background) != 0
        || env_onoff_fallback("OPENPETE_PAD_UNFOCUSED", &opts->pad_unfocused) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;
    if (env_onoff_fallback("OPENPETE_SCREENSHOT_OVERLAYS", &opts->screenshot_overlays) != 0
        || env_onoff_fallback("OPENPETE_LAUNCHER_FULLSCREEN", &opts->launcher_fullscreen) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;
    if (opts->launcher_window_maximized < 0) {
        const char* v = openpete_config_env("OPENPETE_LAUNCHER_WINDOW");
        if (v && *v && parse_window_value("OPENPETE_LAUNCHER_WINDOW", v, &opts->launcher_window_w,
                                          &opts->launcher_window_h,
                                          &opts->launcher_window_maximized) != 0)
            return env_value_refused("OPENPETE_LAUNCHER_WINDOW", v);
    }

    if (env_freecam_pose_check() != 0) return OPENPETE_EXIT_SETUP_ERROR;

    /* --psxtrace-verify replays the reference's input: the run's pad is the
     * reference's (a recorded movie, else no input), never a host device. A
     * connected gamepad at rest otherwise reads into the guest's pad buffer
     * (its sticks map to 0x7F, the reference's centred pad reads 0x80). An
     * explicit --input-lock 0 or OPENPETE_INPUT_LOCK=0 still wins. */
    if (opts->input_lock < 0 && opts->psxtrace_verify_path)
        opts->input_lock = 1;

    /* The legacy input path hands the event pump and the pad decode to
     * PsyCross, where the lock has no filter. */
    if (opts->input_lock > 0) {
        const char* legacy = openpete_config_env("OPENPETE_INPUT_LEGACY");
        if (legacy && *legacy && *legacy != '0') {
            fprintf(stderr, "openpete: --input-lock cannot be combined with "
                    "OPENPETE_INPUT_LEGACY=1\n");
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    }
    return OPENPETE_EXIT_OK;
}

/** @brief The value of env var `name` in *out when it is set and nonempty.
 *
 * Read through openpete_config_env, so the game-neutral parse sees the
 * [env] winners it has not written.
 * @param name The env var.
 * @param out Receives the value; untouched when the variable is unset or empty.
 * @return 1 when set and nonempty, else 0. */
static int env_value(const char* name, const char** out) {
    const char* e = openpete_config_env(name);
    if (!e || !*e) return 0;
    *out = e;
    return 1;
}

/** @brief Resolve one unset 0/1 field from its env spelling: the default when the variable is unset or empty, else 0 when the value starts with '0' (or, with by_atoi, when atoi gives 0), else 1.
 * @param field The field; left alone unless it holds the -1 unset sentinel.
 * @param name The variable.
 * @param def The value when the variable is unset or empty.
 * @param by_atoi 1 to take atoi's verdict, 0 to test the first character. */
static void env_switch(int* field, const char* name, int def, int by_atoi) {
    const char* e = NULL;
    if (*field >= 0) return;
    if (!env_value(name, &e)) *field = def;
    else *field = by_atoi ? (atoi(e) != 0) : (*e != '0');
}

/** @brief Resolve the fields openpete_options_init leaves unset for their env spelling: the env value, else the built-in default.
 *
 * Runs after the flag walk, so a typed flag has claimed its field already,
 * and after the config resolver has applied the openpete.toml [env] tables.
 * OPENPETE_NOTEX, OPENPETE_WIREFRAME, OPENPETE_HIDE_HUD and
 * OPENPETE_MOD_HOT_RELOAD are on unless they start with '0' (default 0);
 * OPENPETE_TERRAIN_CACHE takes atoi's verdict (default 1);
 * OPENPETE_DISC_CACHE takes 0, 1 or auto (default auto, also for any other
 * text); OPENPETE_SCAN_GUARD is off when it starts with '0' (default
 * 1); OPENPETE_HIDE_CLASSES is taken as written;
 * OPENPETE_SECTOR_VISIBILITY=game selects the game's verdict and =filled
 * the filled set (default frustum). OPENPETE_LOAD_DELAY is applied with atoi when set; unset,
 * load_delay stays -1 and run.c applies the game's default.
 * OPENPETE_RUN_RECOMP names a guest entry address (strtoul, base auto).
 * @param opts The options block. */
static void resolve_env_spellings(openpete_options_t* opts) {
    const char* e = NULL;
    env_switch(&opts->notex,          "OPENPETE_NOTEX",          0, 0);
    env_switch(&opts->wireframe,      "OPENPETE_WIREFRAME",      0, 0);
    env_switch(&opts->hide_hud,       "OPENPETE_HIDE_HUD",       0, 0);
    env_switch(&opts->terrain_cache,  "OPENPETE_TERRAIN_CACHE",  1, 1);
    env_switch(&opts->scan_guard,     "OPENPETE_SCAN_GUARD",     1, 0);
    env_switch(&opts->mod_hot_reload, "OPENPETE_MOD_HOT_RELOAD", 0, 0);
    if (!opts->hide_classes && env_value("OPENPETE_HIDE_CLASSES", &e))
        opts->hide_classes = strdup(e);   /* outlives a later setenv or [env] release */
    if (opts->load_delay < 0 && env_value("OPENPETE_LOAD_DELAY", &e))
        opts->load_delay = atoi(e);
    if (opts->sector_visibility == OPENPETE_SECTOR_VIS_UNSET &&
        !(env_value("OPENPETE_SECTOR_VISIBILITY", &e) && sector_vis_parse(e, &opts->sector_visibility) == 0))
        opts->sector_visibility = OPENPETE_SECTOR_VIS_AUTO;
    if (opts->disc_cache == OPENPETE_DISC_CACHE_UNSET &&
        !(env_value("OPENPETE_DISC_CACHE", &e) && disc_cache_parse(e, &opts->disc_cache) == 0))
        opts->disc_cache = OPENPETE_DISC_CACHE_AUTO;
    if (opts->cadence == OPENPETE_CADENCE_UNSET &&
        !(env_value("OPENPETE_CADENCE", &e) && cadence_parse(e, &opts->cadence) == 0))
        opts->cadence = OPENPETE_CADENCE_EVEN;
    if (env_value("OPENPETE_RUN_RECOMP", &e))
        opts->run_recomp_addr = (uint32_t)strtoul(e, NULL, 0);
}

void openpete_options_effective(const openpete_options_t* in,
                                openpete_options_t* out) {
    *out = *in;
    if (out->mouse < 0)                 out->mouse = 1;
    if (out->input_lock < 0)            out->input_lock = 0;
    if (out->background < 0)            out->background = 0;
    if (out->launcher_fullscreen < 0)   out->launcher_fullscreen = 0;
    if (out->screenshot_overlays < 0)   out->screenshot_overlays = 0;
    if (out->pad_unfocused < 0)         out->pad_unfocused = 0;
    if (out->stick_response < 0)        out->stick_response = OPENPETE_STICK_RESPONSE_CONSOLE;
    if (out->mouse_sensitivity < 0.0f)  out->mouse_sensitivity = 1.0f;
    if (out->load_delay < 0)            out->load_delay = (int)game_get_load_delay_default();
    if (out->notex < 0)                 out->notex = 0;
    if (out->wireframe < 0)             out->wireframe = 0;
    if (out->hide_hud < 0)              out->hide_hud = 0;
    if (out->terrain_cache < 0)         out->terrain_cache = 1;
    if (out->disc_cache < 0)            out->disc_cache = OPENPETE_DISC_CACHE_AUTO;
    if (out->scan_guard < 0)            out->scan_guard = 1;
    if (out->mod_hot_reload < 0)        out->mod_hot_reload = 0;
    if (out->sector_visibility == OPENPETE_SECTOR_VIS_UNSET)
        out->sector_visibility = OPENPETE_SECTOR_VIS_AUTO;
    if (out->cadence == OPENPETE_CADENCE_UNSET) out->cadence = OPENPETE_CADENCE_EVEN;
}

/** @brief Splice the bk2 sidecar and one config stage into argv, then walk the flags into opts.
 *
 * The shared body of the game-neutral parse and the full parse. The full
 * parse completes a --bk2-record request; both apply the env spellings,
 * read through openpete_config_env.
 * @param opts The options block.
 * @param argc Argument count.
 * @param argv Arguments as typed.
 * @param neutral 1 for the game-neutral stage (openpete_config_apply_neutral), 0 for the full stage.
 * @return OPENPETE_EXIT_OK or the parser's exit code. */
static int options_parse_stage(openpete_options_t* opts, int argc, char** argv,
                               int neutral) {
    /* Snapshot the command line as typed, before the sidecar / config
     * splices below rewrite argv. --bk2-record captures this (and not the
     * machine's own config defaults) into its .args sidecar. */
    int    orig_argc = argc;
    char** orig_argv = argv;

    /* Process handoff re-execs with the ORIGINAL argv (the fresh
     * process re-splices env/opts/sidecar files itself). */
    opts->handoff_argc = orig_argc;
    opts->handoff_argv = orig_argv;

    /* Per-bk2 sidecar: <path>.bk2.args next to the movie can supply
     * scenario-specific defaults (e.g. --skip-to-level 10 for an
     * Artisans-only movie). Spliced before user argv so explicit CLI
     * flags still override. See games/spyro-1/bk2/README.md. */
    int sidecar_argc = 0;
    char** sidecar_argv = NULL;
    int sidecar_rc = expand_bk2_sidecar(argc, argv, &sidecar_argc, &sidecar_argv);
    if (sidecar_rc < 0) return OPENPETE_EXIT_SETUP_ERROR;
    if (sidecar_rc == 1) { argc = sidecar_argc; argv = sidecar_argv; }

    /* openpete.toml. Resolves every configured layer, applies the winning
     * [env] entries to the environment - which must happen before ANY
     * getenv consumer, since features lazy-cache their env reads on first
     * use - and hands back the winning flags to splice.
     *
     * Spliced after the bk2 splice and so left of it, giving the argv
     * order [config][sidecar][typed]: left is parsed first and the last
     * write wins, which is the file < sidecar < CLI half of the chain.
     * The other half (which of two config layers wins, and args-vs-env)
     * was already decided inside the resolver - see openpete_config.h. */
    /* Give the keymap its SDL-backed name lookups before the config runs:
     * openpete_config_apply seeds the registry defaults, and parsing them
     * needs the lookup live. Pure string tables in SDL, so this is safe
     * long before SDL_Init. */
    openpete_keymap_install_sdl();

    int cfg_argc = 0;
    char** cfg_argv = NULL;
    int cfg_rc = neutral
        ? openpete_config_apply_neutral(argc, argv, orig_argc, &cfg_argc, &cfg_argv)
        : openpete_config_apply(argc, argv, orig_argc, &cfg_argc, &cfg_argv);
    if (cfg_rc < 0) return OPENPETE_EXIT_SETUP_ERROR;
    if (cfg_rc == 1) { argc = cfg_argc; argv = cfg_argv; }

    parse_notes_t notes = { false, false };
    s_parse_neutral = neutral;
    int rc = parse_flag_chain(opts, argc, argv, &notes);
    s_parse_neutral = 0;
    if (rc != OPENPETE_EXIT_OK || opts->help) return rc;

    /* The EXE-source requirement is deferred to
     * openpete_options_require_exe_source(): the game may resolve a
     * disc AFTER parse via the content-addressed library (--ingest, or
     * a no-argument boot of an installed entry). Without OPENPETE_RUN_RECOMP the
     * runtime dispatches the EXE header's entry, the crt0, once the header
     * has been read. */

    /* After validation, so a bad invocation creates no directory. */
    if (!neutral) {
        rc = finish_bk2_record(opts, orig_argc, orig_argv, &notes);
        if (rc != OPENPETE_EXIT_OK) return rc;
    }
    rc = apply_env_spellings(opts);
    resolve_env_spellings(opts);
    return rc;
}

int openpete_options_parse_neutral(openpete_options_t* opts, int argc, char** argv) {
    return options_parse_stage(opts, argc, argv, 1);
}

int openpete_options_parse(openpete_options_t* opts, int argc, char** argv) {
    return options_parse_stage(opts, argc, argv, 0);
}

int openpete_options_require_exe_source(const openpete_options_t* opts) {
    /* Called by game main AFTER library resolution (--ingest / installed
     * library entries can supply the disc post-parse). --test-segv never
     * touches the EXE. */
    if (opts->test_recomp_segv) return OPENPETE_EXIT_OK;
    if (opts->disc_path || opts->exe_path) return OPENPETE_EXIT_OK;
    fprintf(stderr,
            "openpete: no game found - pass --disc PATH (or --exe-path), or "
            "install a disc into the library with --ingest PATH\n");
    return OPENPETE_EXIT_SETUP_ERROR;
}
