/* ident_audit.c - every-tick sub-tick-spine identity audit.
 * See openpete_ident_audit.h for the contract. */

#include "openpete_ident_audit.h"

#include "openpete_fiber.h"          /* openpete_frame / openpete_canon_tick */
#include "openpete_interp.h"         /* present-target override, tick stamps */
#include "openpete_interp_objs.h"    /* g_openpete_tick_period_ns */
#include "openpete_native_render.h"  /* compose readback + scene save/restore */
#include "screenshot.h"              /* path formatting + PNG writer */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

/* Per-tick record, kept for the exit summary's percentiles and worst list. */
typedef struct { unsigned vbl, tick; uint32_t px; int maxch; } audit_row_t;

static audit_row_t *s_rows;
static size_t       s_rows_n, s_rows_cap;
static int          s_start = -1;      /* -1 unread, -2 disarmed */
static long         s_dump_px = -1;    /* >=0 = dump PNGs at/above this count */
static int          s_report_armed;
static uint64_t     s_exact;           /* ticks with px == 0 */

static void audit_report(void);

int openpete_ident_audit_armed(void)
{
    if (s_start == -1) {
        const char *e = getenv("OPENPETE_IDENT_AUDIT");
        s_start = (e && *e) ? atoi(e) : -2;
        const char *d = getenv("OPENPETE_IDENT_AUDIT_DUMP");
        s_dump_px = (d && *d) ? atol(d) : -1;
    }
    return s_start >= 0;
}

/** @brief Write "<stem><suffix><ext>" for a formatted screenshot path into out.
 *
 * The audit's PNGs then land beside the corpus's own dumps. A path with no
 * '.' gets the suffix appended. The last '.' anywhere in the path is taken as
 * the extension separator, directory components included. */
static void path_with_suffix(const char *base, const char *suffix, char *out, size_t cap)
{
    const char *dot = strrchr(base, '.');
    if (dot) snprintf(out, cap, "%.*s%s%s", (int)(dot - base), base, suffix, dot);
    else     snprintf(out, cap, "%s%s", base, suffix);
}

/** @brief qsort comparator ordering audit rows by descending pixel count, ties by ascending VBL. */
static int row_cmp_px_desc(const void *a, const void *b)
{
    const audit_row_t *x = a, *y = b;
    if (x->px != y->px) return x->px < y->px ? 1 : -1;
    return x->vbl < y->vbl ? -1 : (x->vbl > y->vbl);
}

/** @brief qsort comparator ordering audit rows by ascending pixel count. */
static int row_cmp_px_asc(const void *a, const void *b)
{
    const audit_row_t *x = a, *y = b;
    return x->px < y->px ? -1 : (x->px > y->px);
}

void openpete_ident_audit_present(void *cpu, unsigned vbl, unsigned tick,
                                  const char *shot_output) OP_REQUIRES(sim)
{
    if (!openpete_ident_audit_armed()) return;
    if ((int)vbl < s_start) return;
    if (!openpete_native_scene_enabled()) return;
    openpete_native_reextract_fn ire = openpete_native_get_reextract();
    if (!ire) return;

    /* (1) The canonical scene, composed exactly as the -vk dump composes it. */
    unsigned char *canon = NULL; int cw = 0, ch = 0;
    if (openpete_native_render_capture_rgba(&canon, &cw, &ch) != 0) return;

    /* (2) The sub-tick re-extract phased onto the identity instant - the same
     * call the --screenshot-intermediate-on-frame block makes at alpha 1, the
     * alpha that makes it an identity. The scene bracket puts the
     * canonical scene back afterwards: the re-extract rebuilds s_scene in
     * place, and the next frame's begin_frame would otherwise push the rebuilt
     * scene into the frame-history ring in place of the canonical one. */
    unsigned char *ident = NULL; int iw = 0, ih = 0;
    int saved = openpete_native_scene_save();
    openpete_interp_set_present_target(openpete_interp_identity_present_ns());
    ire(cpu);
    openpete_interp_set_present_target(0);
    /* The re-extract's scene is the CURRENT one and was never pushed to
     * history, so the compose must read depth 0. */
    openpete_native_render_set_capture_nodelay(1);
    int rc = openpete_native_render_capture_rgba(&ident, &iw, &ih);
    openpete_native_render_set_capture_nodelay(0);
    if (saved == 0) openpete_native_scene_restore();

    if (rc != 0 || iw != cw || ih != ch) { free(canon); free(ident); return; }

    /* (3) Differing pixels and the largest single-channel delta. */
    const size_t npx = (size_t)cw * (size_t)ch;
    uint32_t diff_px = 0; int maxch = 0;
    for (size_t i = 0; i < npx; i++) {
        int dm = 0;
        for (int c = 0; c < 3; c++) {
            int d = (int)canon[i * 4 + c] - (int)ident[i * 4 + c];
            if (d < 0) d = -d;
            if (d > dm) dm = d;
        }
        if (dm) { diff_px++; if (dm > maxch) maxch = dm; }
    }

    /* (4) One line per tick - the harness's only required output. */
    printf("[ident-audit f=%u tick=%u px=%u maxch=%d]\n", vbl, tick, diff_px, maxch);
    fflush(stdout);

    if (s_rows_n == s_rows_cap) {
        size_t cap = s_rows_cap ? s_rows_cap * 2 : 1024;
        audit_row_t *g = realloc(s_rows, cap * sizeof(*g));
        if (g) { s_rows = g; s_rows_cap = cap; }
    }
    if (s_rows_n < s_rows_cap) {
        s_rows[s_rows_n].vbl = vbl; s_rows[s_rows_n].tick = tick;
        s_rows[s_rows_n].px = diff_px; s_rows[s_rows_n].maxch = maxch;
        s_rows_n++;
    }
    if (diff_px == 0) s_exact++;
    if (!s_report_armed) { s_report_armed = 1; atexit(audit_report); }

    /* (5) The two composes as images, for a tick worth looking at. */
    if (s_dump_px >= 0 && (long)diff_px >= s_dump_px && shot_output) {
        char base[1024], path[1100];
        openpete_screenshot_format_path(shot_output, (int)vbl, 0, base, sizeof(base));
        path_with_suffix(base, "-identaudit-canon", path, sizeof(path));
        openpete_write_png_rgba(path, canon, cw, ch);
        path_with_suffix(base, "-identaudit-ident", path, sizeof(path));
        openpete_write_png_rgba(path, ident, iw, ih);
    }

    free(canon); free(ident);
}

/** @brief Return the px count at index round(p * (n - 1)) of rows sorted ascending, p in [0, 1], 0 when n is 0. */
static uint32_t pct(const audit_row_t *sorted, size_t n, double p)
{
    if (!n) return 0;
    size_t i = (size_t)(p * (double)(n - 1) + 0.5);
    return sorted[i].px;
}

/** @brief atexit report: print the per-tick pixel-difference summary and the ten worst ticks.
 *
 * Sorts a copy of the recorded rows ascending by differing-pixel count for
 * the exact-tick share, the 50th and 90th percentiles and the maximum, then
 * descending for the "[ident-audit worst]" lines (ties broken by lower VBL
 * first). Prints nothing when no tick was recorded. */
static void audit_report(void)
{
    OP_ROLE_ASSERT(sim);
    if (!s_rows_n) return;
    audit_row_t *sorted = malloc(s_rows_n * sizeof(*sorted));
    if (!sorted) return;
    memcpy(sorted, s_rows, s_rows_n * sizeof(*sorted));
    qsort(sorted, s_rows_n, sizeof(*sorted), row_cmp_px_asc);

    printf("[ident-audit] ticks=%zu exact=%llu (%.2f%%) p50=%u p90=%u max=%u\n",
           s_rows_n, (unsigned long long)s_exact,
           100.0 * (double)s_exact / (double)s_rows_n,
           pct(sorted, s_rows_n, 0.50), pct(sorted, s_rows_n, 0.90),
           sorted[s_rows_n - 1].px);

    qsort(sorted, s_rows_n, sizeof(*sorted), row_cmp_px_desc);
    size_t worst = s_rows_n < 10 ? s_rows_n : 10;
    for (size_t i = 0; i < worst; i++)
        printf("[ident-audit worst] f=%u tick=%u px=%u maxch=%d\n",
               sorted[i].vbl, sorted[i].tick, sorted[i].px, sorted[i].maxch);
    fflush(stdout);
    free(sorted);
}
