/* moby_snap.c - contact planes of pulled mobys against the drawn terrain;
 * see moby_snap.h. Camera space throughout is the PSX screen unprojected at
 * view-Z with the terrain build's projection: p = ((sx - ofx) z / H,
 * (sy - ofy) z / H, z), the space the snap stage rebuilds a fragment in. */
#include "renderer/moby_snap.h"
#include "renderer/terrain.h"
#include "renderer/orchestrator.h"   /* canonical epoch: the carry's tables */
#include "openpete_profile.h"        /* mcan.resolve / mrex.resolve */
#include "openpete_pace_log.h"       /* OP_PACE_SPLIT_SCOPE - per-tick CPU split */
#include "openpete_native_render.h"
#include "openpete_fiber.h"          /* openpete_feed_key_now: census line label */
#include "openpete_workpool.h"       /* the contact pass's fan-out */

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "openpete_thread_roles.h"
#include "game_layout.h"             /* moby class and position: the verify's detail lines */
#include "psx_runtime_internal.h"    /* full CPUState (read_half, read_word) */

/** @brief The smaller of two finite floats, inline (fminf is a library call on this target).
 * @param a First value.
 * @param b Second value.
 * @return The smaller. */
static inline float ms_minf(float a, float b) { return b < a ? b : a; }

/** @brief The larger of two finite floats, inline (fmaxf is a library call on this target).
 * @param a First value.
 * @param b Second value.
 * @return The larger. */
static inline float ms_maxf(float a, float b) { return b > a ? b : a; }

/** @brief The smaller of two finite doubles, inline.
 * @param a First value.
 * @param b Second value.
 * @return The smaller. */
static inline double ms_min(double a, double b) { return b < a ? b : a; }

/** @brief The larger of two finite doubles, inline.
 * @param a First value.
 * @param b Second value.
 * @return The larger. */
static inline double ms_max(double a, double b) { return b > a ? b : a; }

/* One queued moby: its identity, ordinal, pull, run of s_q_tri and the
 * bounds of that run (screen x0, y0, x1, y1, view-Z z0, z1); its index in
 * the queue as collected (qi), whether its planes go to the scene's table
 * (add; 0 for a moby the carry already placed) and the walk pass of the
 * scene that queued it (seg, counted by moby_snap_moby_fed); bok is 1 once b
 * and the run's s_q_bnd hold (ms_bounds_ensure). */
typedef struct { uint32_t mid; int mord, dofs; float off; int t0, nt; float b[6]; int qi; int add; int path; float kr; int seg; int bok; } ms_moby;

static ms_moby*   s_q_mob;  static int s_q_nm, s_q_capm;
static float    (*s_q_tri)[3][3]; static int s_q_nt, s_q_capt;
static float    (*s_q_bnd)[6]; static int s_q_capb;   /* per s_q_tri: its bounds */
static uint32_t*  s_q_fk; static int s_q_capf;       /* per s_q_tri: its face key, while the probe is on */
static unsigned   s_q_epoch = ~0u;
static long       s_ms_scenes;   /* scenes that queued (ms_scene_sync), the bound census's present count */
static int        s_q_seg;   /* the walk pass now queueing, counted from 0 per scene */

/* The planes one moby may own; more are counted as overflow. */
#define MS_MAX_PLANES 64
/* Contact planes merge when their normals agree to within this cosine
 * deficit and their offsets to within one world unit. */
#define MS_MERGE_DN 1e-4
#define MS_MERGE_DD 1.0

/* Per-resolve cost counters (the census line). */
typedef struct {
    long pulled, faces, tris, sectors, tests, contacts, planes, ovf;
    long contacts_t4;     /* contacts with a face drawn as 16 or more triangles */
    long overlaps;        /* pairs of snapped mobys whose screen boxes overlap */
    float zmin;           /* the nearest key depth of a snapped moby */
    uint64_t cap_ns;
    int timed;            /* the phase clocks below run */
    uint64_t near_ns;     /* candidate triangles and the near moby triangles */
    uint64_t test_ns;     /* the exact triangle tests and the plane merge */
    uint64_t plane_ns;    /* the plane pass: corners, side and slab */
    long cut_mobys;       /* snapped mobys a coplanar drawn triangle still cuts (ms_falsify) */
    long cut_samples;     /* their cut sample points */
    long cut_cls[3];      /* the cut samples by class (ms_falsify): 0 outside the plane's slab
                           * or disc, 1 inside both under one of the plane's contact
                           * triangles, 2 inside both under a coplanar non-contact triangle */
    long cut_b_mobys;     /* mobys with a class-1 cut */
    long carried;         /* mobys whose planes the carry placed (ms_carry_begin) */
    long fresh;           /* mobys of a carried present resolved by the contact pass */
} ms_census;

/** @brief The monotonic clock in nanoseconds, for the census's phase split.
 * @return The time. */
static uint64_t ms_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/** @brief Grow a queue array to hold need elements.
 * @param p The array pointer.
 * @param cap Its capacity, updated.
 * @param need Elements wanted.
 * @param esz Element size.
 * @return 1 when it holds them, 0 on an allocation failure. */
static int ms_grow(void** p, int* cap, int need, size_t esz) {
    if (need <= *cap) return 1;
    int nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    void* q = realloc(*p, (size_t)nc * esz);
    if (!q) return 0;
    *p = q; *cap = nc;
    return 1;
}

/* The scene's ranked mobys, one (identity, ordinal) per walk pass entry,
 * and its snapped mobys with an ordinal: the ordinal-collision census. */
typedef struct { uint32_t mid; int ord; } ms_ord;
static ms_ord s_rk[1024]; static int s_rk_n;
static ms_ord s_sn[256];  static int s_sn_n;

/** @brief Start the scene's queue and ordinal census when the scene changed. */
static void ms_scene_sync(void) {
    const unsigned ep = openpete_native_scene_epoch();
    if (ep == s_q_epoch) return;
    s_q_epoch = ep; s_q_nm = s_q_nt = 0; s_rk_n = s_sn_n = 0; s_q_seg = 0;
    s_ms_scenes++;
}

void moby_snap_note_ranked(const uint32_t* mids, int n, int base) {
    ms_scene_sync();
    for (int k = 0; k < n && s_rk_n < 1024; k++) { s_rk[s_rk_n].mid = mids[k]; s_rk[s_rk_n++].ord = base + k + 1; }
}

/** @brief Count the pairs (snapped moby with ordinal k, another ranked moby of the scene with ordinal k): faces whose ordinal lookup would find the wrong entry.
 * @return The pair count. */
static long ms_ord_collisions(void) {
    long n = 0;
    for (int a = 0; a < s_sn_n; a++)
        for (int b = 0; b < s_rk_n; b++)
            n += s_rk[b].ord == s_sn[a].ord && s_rk[b].mid != s_sn[a].mid;
    return n;
}

/** @brief Write each triangle's bounds (screen x0, y0, x1, y1, view-Z z0, z1) and their union.
 * @param t The triangles' corners (sx, sy, view-Z).
 * @param b Receives one bounds record per triangle.
 * @param n Triangle count.
 * @param u Receives the union. */
static void ms_bounds_tris(const float (*t)[3][3], float (*b)[6], int n, float u[6]) {
    u[0] = u[1] = u[4] = 1e30f; u[2] = u[3] = u[5] = -1e30f;
    for (int i = 0; i < n; i++) {
        const float(*c)[3] = t[i];
        b[i][0] = ms_minf(ms_minf(c[0][0], c[1][0]), c[2][0]); b[i][2] = ms_maxf(ms_maxf(c[0][0], c[1][0]), c[2][0]);
        b[i][1] = ms_minf(ms_minf(c[0][1], c[1][1]), c[2][1]); b[i][3] = ms_maxf(ms_maxf(c[0][1], c[1][1]), c[2][1]);
        b[i][4] = ms_minf(ms_minf(c[0][2], c[1][2]), c[2][2]); b[i][5] = ms_maxf(ms_maxf(c[0][2], c[1][2]), c[2][2]);
        u[0] = ms_minf(u[0], b[i][0]); u[1] = ms_minf(u[1], b[i][1]); u[2] = ms_maxf(u[2], b[i][2]);
        u[3] = ms_maxf(u[3], b[i][3]); u[4] = ms_minf(u[4], b[i][4]); u[5] = ms_maxf(u[5], b[i][5]);
    }
}

/* ---- the lean queue ------------------------------------------------------
 * A queued moby's bounds (ms_moby.b and its run of s_q_bnd) are read only by
 * the contact pass (ms_box, ms_moby_planes) and the carry verify's detail
 * lines, which run on the mobys a resolve sends to the capture. In a sub-tick
 * resolve the carry places most of the queue and those never reach it, so
 * the bounds are computed when the moby's chunk resolves (ms_bounds_ensure)
 * instead of when it is queued. ms_bounds_tris reads only the moby's run of
 * s_q_tri, which nothing writes between the queue and the resolve, so the
 * bounds are those the queue would have computed. The collect also gathers
 * a moby's corners straight into the queue (moby_snap_queue_room) instead
 * of into its own buffer for moby_snap_queue to copy. OPENPETE_SNAP_QUEUE_LEAN=0
 * computes the bounds at the queue and copies the corners;
 * OPENPETE_SNAP_QUEUE_LEAN_CHECK=1 computes the bounds at the queue and again
 * at the resolve, comparing bit for bit (ms_bounds_ensure), and has the
 * collect gather each moby both ways and compare. */
static struct { long mobys, bounded, compared, bad; } s_lbc;   /* the check's totals */

int moby_snap_queue_lean(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_SNAP_QUEUE_LEAN");
        const char* c = getenv("OPENPETE_SNAP_QUEUE_LEAN_CHECK");
        v = (c && *c && *c != '0') ? 2 : !(e && *e == '0');
    }
    return v;
}

/** @brief Print the lean queue check's bounds totals: mobys queued, those whose chunk resolved, those compared and the mismatches. */
static void ms_lb_print(void) {
    fprintf(stderr, "[snap-queue-lean-check] queued=%ld resolved=%ld compared=%ld mismatched=%ld\n",
            s_lbc.mobys, s_lbc.bounded, s_lbc.compared, s_lbc.bad);
}

/** @brief Make a queued moby's bounds hold before its chunk resolves: compute them when deferred, or under the check compare a fresh computation with the queue's.
 * @param m The moby. */
static void ms_bounds_ensure(ms_moby* m) {
    s_lbc.bounded++;
    if (!m->bok) {
        ms_bounds_tris(&s_q_tri[m->t0], &s_q_bnd[m->t0], m->nt, m->b);
        m->bok = 1;
        return;
    }
    if (moby_snap_queue_lean() != 2) return;
    static float (*tb)[6]; static int tcap;
    if (!ms_grow((void**)&tb, &tcap, m->nt, sizeof *tb)) return;
    float u[6];
    ms_bounds_tris(&s_q_tri[m->t0], tb, m->nt, u);
    s_lbc.compared++;
    s_lbc.bad += memcmp(u, m->b, sizeof u) != 0 || memcmp(tb, &s_q_bnd[m->t0], (size_t)m->nt * sizeof *tb) != 0;
}

int moby_snap_probe_on(void) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_CARRY_VERIFY_CMOV"); s_on = e && *e == '1'; }
    return s_on;
}

float (*moby_snap_queue_room(int n))[3][3] {
    ms_scene_sync();
    if (!moby_snap_queue_lean() || n <= 0 || !ms_grow((void**)&s_q_tri, &s_q_capt, s_q_nt + n, sizeof *s_q_tri)) return NULL;
    return &s_q_tri[s_q_nt];
}

void moby_snap_queue(uint32_t mid, int mordinal, int dofs, float off_m,
                     const float (*v)[3][3], int n) {
    ms_scene_sync();
    if (n <= 0 || !mid || dofs <= 0) return;
    if (!ms_grow((void**)&s_q_mob, &s_q_capm, s_q_nm + 1, sizeof *s_q_mob) ||
        !ms_grow((void**)&s_q_tri, &s_q_capt, s_q_nt + n, sizeof *s_q_tri) ||
        !ms_grow((void**)&s_q_bnd, &s_q_capb, s_q_nt + n, sizeof *s_q_bnd)) return;
    if (moby_snap_probe_on()) {
        if (!ms_grow((void**)&s_q_fk, &s_q_capf, s_q_nt + n, sizeof *s_q_fk)) return;
        memset(&s_q_fk[s_q_nt], 0, (size_t)n * sizeof *s_q_fk);
    }
    ms_moby* m = &s_q_mob[s_q_nm++];
    m->mid = mid; m->mord = mordinal; m->dofs = dofs; m->off = off_m;
    m->t0 = s_q_nt; m->nt = n; m->path = 0; m->kr = 0.0f; m->seg = s_q_seg;
    if (v != (const float (*)[3][3])&s_q_tri[s_q_nt]) memcpy(&s_q_tri[s_q_nt], v, (size_t)n * sizeof *s_q_tri);
    m->bok = moby_snap_queue_lean() != 1;
    if (m->bok) ms_bounds_tris(&s_q_tri[s_q_nt], &s_q_bnd[s_q_nt], n, m->b);
    if (moby_snap_queue_lean() == 2) {
        static int s_atexit;
        if (!s_atexit) { s_atexit = 1; atexit(ms_lb_print); }
        s_lbc.mobys++;
    }
    s_q_nt += n;
}

void moby_snap_queue_probe(uint32_t mid, const uint32_t* fk, int n, int path, float kr) {
    if (!moby_snap_probe_on() || !s_q_nm) return;
    ms_moby* m = &s_q_mob[s_q_nm - 1];
    if (m->mid != mid || m->nt != n) return;
    memcpy(&s_q_fk[m->t0], fk, (size_t)n * sizeof *fk);
    m->path = path; m->kr = kr;
}

/* ---- geometry ------------------------------------------------------------ */

typedef struct { double x, y, z; } ms_v3;
/** @brief Return a minus b. */
static ms_v3 v3sub(ms_v3 a, ms_v3 b) { ms_v3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
/** @brief Return the dot product of a and b. */
static double v3dot(ms_v3 a, ms_v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
/** @brief Return the cross product of a and b. */
static ms_v3 v3cross(ms_v3 a, ms_v3 b) {
    ms_v3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}
/** @brief Return the length of a. */
static double v3len(ms_v3 a) { return sqrt(v3dot(a, a)); }

/* A camera-space triangle with its plane and bounds. */
typedef struct { ms_v3 p[3]; ms_v3 n; double d; ms_v3 lo, hi; int ok; } ms_tri;

/** @brief Build a camera-space triangle from screen corners, with its unit normal, offset and bounds.
 * @param o Receives the triangle; ok is 0 for a degenerate one.
 * @param sx Corner screen X.
 * @param sy Corner screen Y.
 * @param z Corner view-Z.
 * @param H Projection distance.
 * @param ofx Optical axis X.
 * @param ofy Optical axis Y. */
static void ms_tri_make(ms_tri* o, const float sx[3], const float sy[3], const float z[3],
                        double H, double ofx, double ofy) {
    for (int k = 0; k < 3; k++) {
        o->p[k].x = ((double)sx[k] - ofx) * z[k] / H;
        o->p[k].y = ((double)sy[k] - ofy) * z[k] / H;
        o->p[k].z = z[k];
    }
    ms_v3 n = v3cross(v3sub(o->p[1], o->p[0]), v3sub(o->p[2], o->p[0]));
    const double l = v3len(n);
    o->ok = l > 0.0;
    if (o->ok) { n.x /= l; n.y /= l; n.z /= l; }
    o->n = n; o->d = v3dot(n, o->p[0]);
    o->lo = o->hi = o->p[0];
    for (int k = 1; k < 3; k++) {
        o->lo.x = ms_min(o->lo.x, o->p[k].x); o->hi.x = ms_max(o->hi.x, o->p[k].x);
        o->lo.y = ms_min(o->lo.y, o->p[k].y); o->hi.y = ms_max(o->hi.y, o->p[k].y);
        o->lo.z = ms_min(o->lo.z, o->p[k].z); o->hi.z = ms_max(o->hi.z, o->p[k].z);
    }
}

/** @brief Whether the segment a-b crosses triangle T (inside its edges, the plane crossing included).
 * @param a Segment start.
 * @param b Segment end.
 * @param T The triangle.
 * @return 1 when it does, else 0. */
static int ms_seg_tri(ms_v3 a, ms_v3 b, const ms_tri* T) {
    const double da = v3dot(T->n, a) - T->d, db = v3dot(T->n, b) - T->d;
    if (da * db > 0.0 || da == db) return 0;
    const double t = da / (da - db);
    const ms_v3 x = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    for (int i = 0; i < 3; i++) {
        const ms_v3 e = v3sub(T->p[(i + 1) % 3], T->p[i]), w = v3sub(x, T->p[i]);
        const double c = v3dot(T->n, v3cross(e, w));
        /* outside the edge by more than 1e-6 of |e||w| (plus 1e-9) */
        if (c < -1e-9 && (c + 1e-9) * (c + 1e-9) > 1e-12 * v3dot(e, e) * v3dot(w, w)) return 0;
    }
    return 1;
}

/** @brief Exact triangle-triangle intersection: an edge of either crosses the other.
 * @param A First triangle.
 * @param B Second triangle.
 * @return 1 when they intersect, else 0. */
static int ms_tri_tri(const ms_tri* A, const ms_tri* B) {
    int pa = 0, na = 0, pb = 0, nb = 0;
    for (int k = 0; k < 3; k++) {
        const double sb = v3dot(A->n, B->p[k]) - A->d, sa = v3dot(B->n, A->p[k]) - B->d;
        pb += sb > 0.0; nb += sb < 0.0; pa += sa > 0.0; na += sa < 0.0;
    }
    if (pb == 3 || nb == 3 || pa == 3 || na == 3) return 0;
    for (int i = 0; i < 3; i++) {
        if (ms_seg_tri(A->p[i], A->p[(i + 1) % 3], B)) return 1;
        if (ms_seg_tri(B->p[i], B->p[(i + 1) % 3], A)) return 1;
    }
    return 0;
}

/** @brief Whether two bounds overlap.
 * @param a First triangle.
 * @param b Second triangle.
 * @return 1 when they do. */
static int ms_box_overlap(const ms_tri* a, const ms_tri* b) {
    return !(a->hi.x < b->lo.x || a->lo.x > b->hi.x || a->hi.y < b->lo.y ||
             a->lo.y > b->hi.y || a->hi.z < b->lo.z || a->lo.z > b->hi.z);
}

/* ---- planes -------------------------------------------------------------- */

/* A contact plane's source face, for the carry verify: the first contact
 * face's sector index, face index and tier (terrain_snap_tri), and its
 * vertex-animation mark. */
typedef struct { int16_t sec, face; uint8_t tier, vanim; } ms_pid;

/* A contact plane under construction: its normal and offset (the first
 * contact face's), whether any of its faces is double-sided, and its first
 * contact face's identity. */
typedef struct { ms_v3 n; double d; int dside; ms_pid id; } ms_plane;

/** @brief Finish one contact plane into the snap table's form: side (openpete_snap_side) and slab, 128 dofs, the game's own pull.
 * @param T The moby's triangles in camera space (corners only are read).
 * @param nt Triangle count.
 * @param dofs The moby's authored depth offset.
 * @param pl The plane.
 * @param up World up in camera space (unit), or NULL when unknown.
 * @param o Receives the plane. */
static void ms_plane_finish(const ms_tri* T, int nt, int dofs, const ms_plane* pl,
                            const double* up, openpete_snap_plane* o) {
    double mass = 0.0;
    if (pl->dside) {
        for (int i = 0; i < nt; i++)
            for (int k = 0; k < 3; k++) mass += v3dot(pl->n, T[i].p[k]) - pl->d;
        mass /= 3.0 * (nt > 0 ? nt : 1);
    }
    const double n[3] = { pl->n.x, pl->n.y, pl->n.z };
    o->n[0] = (float)pl->n.x; o->n[1] = (float)pl->n.y; o->n[2] = (float)pl->n.z;
    o->d = (float)pl->d;
    o->side = (float)openpete_snap_side(n, pl->d, pl->dside, up, mass);
    o->slab = 128.0f * (float)dofs;
    o->dside = pl->dside;
}

/** @brief Add a contact face to the plane it lies in, or open a plane for it.
 * @param P The moby's planes.
 * @param np Their count, updated.
 * @param F The contact face.
 * @param src The captured triangle F was built from: its double-sided flag, and the identity a plane it opens takes.
 * @return 1 when placed, 0 when the moby's plane cap is full. */
static int ms_plane_add(ms_plane* P, int* np, const ms_tri* F, const terrain_snap_tri* src) {
    ms_plane* pl = NULL;
    for (int j = 0; j < *np && !pl; j++) {
        const double c = v3dot(P[j].n, F->n);
        if (fabs(c) > 1.0 - MS_MERGE_DN && fabs(P[j].d - (c < 0.0 ? -F->d : F->d)) < MS_MERGE_DD)
            pl = &P[j];
    }
    if (!pl) {
        if (*np >= MS_MAX_PLANES) return 0;
        pl = &P[(*np)++];
        pl->n = F->n; pl->d = F->d; pl->dside = 0;
        pl->id.sec = src->sec; pl->id.face = src->face; pl->id.tier = src->tier; pl->id.vanim = src->vanim;
    }
    pl->dside |= src->dside;
    return 1;
}

static unsigned s_dump_keys[16];
static int s_dump_nk = -1;   /* key count, -1 until read */

/** @brief The count of feed keys OPENPETE_MOBY_SNAP_DUMP names, read once; a nonzero count keeps the contact pass serial so the dump lines keep queue order.
 * @return The count. */
static int ms_dump_keys(void) {
    if (s_dump_nk >= 0) return s_dump_nk;
    int n = 0;
    const char* e = getenv("OPENPETE_MOBY_SNAP_DUMP");
    while (e && *e && n < 16) {
        s_dump_keys[n++] = (unsigned)strtoul(e, (char**)&e, 10);
        if (*e == '+') e++; else break;
    }
    s_dump_nk = n;
    return n;
}

/** @brief Print a moby's contact planes at the feed keys the OPENPETE_MOBY_SNAP_DUMP knob names, an instrument.
 *
 * One line per plane: the moby's identity, ordinal, pull and offset, the
 * view, and the plane's normal, offset, signed slab (side * slab) and
 * double-sided flag.
 * @param mb The moby.
 * @param pl Its planes.
 * @param np Their count.
 * @param view H, ofx, ofy. */
static void ms_dump(const ms_moby* mb, const openpete_snap_plane* pl, int np, const float view[3]) {
    const int nk = ms_dump_keys();
    if (!nk) return;
    const unsigned key = openpete_feed_key_now();
    int on = 0;
    for (int k = 0; k < nk; k++) on |= s_dump_keys[k] == key;
    for (int k = 0; on && k < np; k++)
        fprintf(stderr, "[moby-snap-plane] key=%u mid=%u mord=%d dofs=%d off=%.3f H=%.4f ofx=%.4f ofy=%.4f "
                        "n=%.7f,%.7f,%.7f d=%.4f sslab=%.4f dside=%d\n",
                key, mb->mid, mb->mord, mb->dofs, mb->off, view[0], view[1], view[2],
                pl[k].n[0], pl[k].n[1], pl[k].n[2], pl[k].d, pl[k].side * pl[k].slab, pl[k].dside);
}

/* World up in the snap's camera space for the resolve under way, from the
 * camera the terrain walk published (openpete_native_occl_live_camera):
 * the game's camera matrix carries world +Z as its input (0, -1, 0), so up
 * is minus its second column. s_up_p points at it, or is NULL when no
 * camera is published. */
static double s_up[3];
static const double* s_up_p;

/** @brief Set s_up and s_up_p from the camera the terrain walk now feeding the scene published. */
static void ms_up_sync(void) {
    openpete_occl_camera c;
    openpete_native_occl_live_camera(&c);
    const double u[3] = { -(double)c.M[1], -(double)c.M[4], -(double)c.M[7] };
    const double l = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    s_up_p = NULL;
    if (!c.valid || !(l > 1e-6)) return;
    for (int k = 0; k < 3; k++) s_up[k] = u[k] / l;
    s_up_p = s_up;
}

/* Per captured triangle, shared read-only by the contact pass once built
 * (ms_terrain_make). */
static ms_tri*  s_tt;  static int s_tt_cap;      /* camera-space form */
static float  (*s_tb)[6]; static int s_tb_cap;   /* bounds */
/* Per captured triangle: the last moby (s_mserial) it touched. Only the cut
 * falsifier reads them, so they are written only while it runs, which keeps
 * the contact pass serial. */
static unsigned* s_tcon; static int s_tcon_cap;
static unsigned  s_mserial;

/* The near grid (ms_ng_build): the near moby triangles binned on an
 * MS_NG x MS_NG screen grid over the candidates' rectangle, cell by cell. */
#define MS_NG 8
#define MS_NG_PAIRS 256
/* The pool's largest worker count (openpete_workpool_threads). */
#define MS_WORKERS 8

/* One contact-pass worker's scratch, grown on demand: the per-moby arrays,
 * the near grid, the stamp counter, and the census the worker counts into
 * during a fan-out. Worker 0 is the calling thread, and the serial pass. */
typedef struct {
    OPENPETE_WORKER_ALIGN ms_tri* mt; int mt_cap;   /* the moby's camera-space triangles */
    int*     cj;  int cj_cap;              /* one moby's usable candidates */
    int*     ix;  int ix_cap;              /* the moby triangles one terrain triangle intersects */
    uint8_t* made; int made_cap;           /* mt[i] is built (ms_make_near) */
    int*     ml;  int ml_cap;              /* the moby triangles near the candidates */
    unsigned* mstamp; int mstamp_cap;      /* per moby triangle: the last query that met it */
    unsigned mquery;                       /* the query counter mstamp holds */
    int      ng_first[MS_NG * MS_NG + 1];
    int*     ng_ent; int ng_cap;
    float    ng_x0, ng_y0, ng_ix, ng_iy;
    ms_census cs;                          /* the worker's counts in a fan-out */
} ms_ws;
OPENPETE_WORKER_ELEM(ms_ws);
static ms_ws s_ws[MS_WORKERS];

/** @brief Grow every scratch array of one worker's contact pass for a moby of nt triangles against nc candidates.
 *
 * Stamp slots the growth adds are zeroed: the query counter starts at 1 and
 * only rises, so a new slot never reads as met by the current query.
 * @param W The worker's scratch.
 * @param nt Moby triangles.
 * @param nc Candidate terrain triangles.
 * @return 1 when all hold, 0 on an allocation failure. */
static int ms_scratch(ms_ws* W, int nt, int nc) {
    const int sc0 = W->mstamp_cap;
    if (!ms_grow((void**)&W->mstamp, &W->mstamp_cap, nt, sizeof *W->mstamp)) return 0;
    if (W->mstamp_cap > sc0) memset(W->mstamp + sc0, 0, (size_t)(W->mstamp_cap - sc0) * sizeof *W->mstamp);
    return ms_grow((void**)&W->mt, &W->mt_cap, nt, sizeof *W->mt) &&
           ms_grow((void**)&W->cj, &W->cj_cap, nc > 0 ? nc : 1, sizeof *W->cj) &&
           ms_grow((void**)&W->ix, &W->ix_cap, nt * 2, sizeof *W->ix) &&
           ms_grow((void**)&W->made, &W->made_cap, nt, 1) &&
           ms_grow((void**)&W->ml, &W->ml_cap, nt, sizeof *W->ml);
}

/** @brief Widen a bounds record (x0, y0, x1, y1, z0, z1) by one screen point.
 * @param b The bounds, updated.
 * @param x Screen X.
 * @param y Screen Y.
 * @param z View-Z. */
static void ms_bounds_add(float b[6], float x, float y, float z) {
    b[0] = ms_minf(b[0], x); b[2] = ms_maxf(b[2], x);
    b[1] = ms_minf(b[1], y); b[3] = ms_maxf(b[3], y);
    b[4] = ms_minf(b[4], z); b[5] = ms_maxf(b[5], z);
}

/** @brief Build the camera-space form (W->mt) of the moby triangles whose bounds meet u (the candidates' bounds) and list them in W->ml.
 *
 * The rest are marked not built (W->made 0).
 * @param W The worker's scratch.
 * @param m The triangles' screen corners.
 * @param bnd Their bounds records (ms_bounds_tris).
 * @param nt Their count.
 * @param u The candidates' bounds.
 * @param view H, ofx, ofy.
 * @return The count listed. */
static int ms_make_near(ms_ws* W, const float (*m)[3][3], const float (*bnd)[6], int nt, const float u[6],
                        const float view[3]) {
    int nl = 0;
    for (int i = 0; i < nt; i++) {
        const float* b = bnd[i];
        W->made[i] = !(b[2] < u[0] || b[0] > u[2] || b[3] < u[1] || b[1] > u[3] || b[5] < u[4] || b[4] > u[5]);
        if (!W->made[i]) continue;
        const float sx[3] = { m[i][0][0], m[i][1][0], m[i][2][0] };
        const float sy[3] = { m[i][0][1], m[i][1][1], m[i][2][1] };
        const float z[3]  = { m[i][0][2], m[i][1][2], m[i][2][2] };
        ms_tri_make(&W->mt[i], sx, sy, z, view[0], view[1], view[2]);
        if (W->mt[i].ok) W->ml[nl++] = i;
    }
    return nl;
}

/** @brief Unproject the corners (W->mt[i].p) of every moby triangle ms_make_near did not build; the plane pass reads only the corners.
 * @param W The worker's scratch.
 * @param m The triangles' screen corners.
 * @param nt Their count.
 * @param view H, ofx, ofy. */
static void ms_make_rest(ms_ws* W, const float (*m)[3][3], int nt, const float view[3]) {
    const double H = view[0], ofx = view[1], ofy = view[2];
    for (int i = 0; i < nt; i++) {
        if (W->made[i]) continue;
        for (int k = 0; k < 3; k++) {
            const double z = m[i][k][2];
            W->mt[i].p[k].x = ((double)m[i][k][0] - ofx) * z / H;
            W->mt[i].p[k].y = ((double)m[i][k][1] - ofy) * z / H;
            W->mt[i].p[k].z = z;
        }
    }
}

/** @brief Build the camera-space form (s_tt) and the bounds (s_tb) of every captured terrain triangle, once per capture.
 * @param tt The captured triangles.
 * @param n Their count.
 * @param view H, ofx, ofy.
 * @return 0 on an allocation failure, else 1. */
static int ms_terrain_make(const terrain_snap_tri* tt, int n, const float view[3]) {
    if (!ms_grow((void**)&s_tt, &s_tt_cap, n > 0 ? n : 1, sizeof *s_tt) ||
        !ms_grow((void**)&s_tb, &s_tb_cap, n > 0 ? n : 1, sizeof *s_tb) ||
        !ms_grow((void**)&s_tcon, &s_tcon_cap, n > 0 ? n : 1, sizeof *s_tcon)) return 0;
    memset(s_tcon, 0, (size_t)(n > 0 ? n : 1) * sizeof *s_tcon);
    for (int j = 0; j < n; j++) {
        const terrain_snap_tri* T = &tt[j];
        float* b = s_tb[j];
        b[0] = b[1] = b[4] = 1e30f; b[2] = b[3] = b[5] = -1e30f;
        for (int k = 0; k < 3; k++) ms_bounds_add(b, T->sx[k], T->sy[k], T->vz[k]);
        ms_tri_make(&s_tt[j], T->sx, T->sy, T->vz, view[0], view[1], view[2]);
    }
    return 1;
}

/* The near grid (ms_ws.ng_first, ng_ent): a terrain triangle reads the
 * cells its rectangle covers instead of the whole near list. Built for a
 * moby whose near list times its candidates exceeds MS_NG_PAIRS. */

/** @brief Return the near grid's cell range [lo, hi] an interval covers, clamped to the grid.
 * @param a Interval start.
 * @param b Interval end.
 * @param o The grid origin on this axis.
 * @param ic The inverse cell size on this axis.
 * @param c Receives the first and the last cell. */
static void ms_ng_cells(float a, float b, float o, float ic, int c[2]) {
    const float fl = (a - o) * ic, fh = (b - o) * ic;
    const int l = fl < 0.0f ? 0 : (int)fl, h = fh < 0.0f ? 0 : (int)fh;
    c[0] = l > MS_NG - 1 ? MS_NG - 1 : l;
    c[1] = h > MS_NG - 1 ? MS_NG - 1 : h;
}

/** @brief Bin the near list (W->ml, nl long) on the worker's near grid over rectangle u.
 * @param W The worker's scratch.
 * @param mb The moby triangles' bounds.
 * @param nl The near list's length.
 * @param u The candidates' bounds.
 * @return 0 on an allocation failure, else 1. */
static int ms_ng_build(ms_ws* W, const float (*mb)[6], int nl, const float u[6]) {
    int cnt[MS_NG * MS_NG] = { 0 };
    W->ng_x0 = u[0]; W->ng_y0 = u[1];
    W->ng_ix = (float)MS_NG / ms_maxf(u[2] - u[0], 1e-3f);
    W->ng_iy = (float)MS_NG / ms_maxf(u[3] - u[1], 1e-3f);
    int n = 0;
    for (int j = 0; j < nl; j++) {
        int cx[2], cy[2];
        ms_ng_cells(mb[W->ml[j]][0], mb[W->ml[j]][2], W->ng_x0, W->ng_ix, cx);
        ms_ng_cells(mb[W->ml[j]][1], mb[W->ml[j]][3], W->ng_y0, W->ng_iy, cy);
        for (int y = cy[0]; y <= cy[1]; y++)
            for (int x = cx[0]; x <= cx[1]; x++) { cnt[y * MS_NG + x]++; n++; }
    }
    if (!ms_grow((void**)&W->ng_ent, &W->ng_cap, n > 0 ? n : 1, sizeof *W->ng_ent)) return 0;
    W->ng_first[0] = 0;
    for (int c = 0; c < MS_NG * MS_NG; c++) { W->ng_first[c + 1] = W->ng_first[c] + cnt[c]; cnt[c] = W->ng_first[c]; }
    for (int j = 0; j < nl; j++) {
        int cx[2], cy[2];
        ms_ng_cells(mb[W->ml[j]][0], mb[W->ml[j]][2], W->ng_x0, W->ng_ix, cx);
        ms_ng_cells(mb[W->ml[j]][1], mb[W->ml[j]][3], W->ng_y0, W->ng_iy, cy);
        for (int y = cy[0]; y <= cy[1]; y++)
            for (int x = cx[0]; x <= cx[1]; x++) W->ng_ent[cnt[y * MS_NG + x]++] = W->ml[j];
    }
    return 1;
}

/** @brief Test one moby triangle against a terrain triangle and list it at W->ix[nh] when they intersect.
 *
 * Screen bounds reject first (a shared point projects into both
 * rectangles), then camera-space bounds, then the exact test.
 * @param W The worker's scratch.
 * @param F The terrain triangle.
 * @param fb Its bounds.
 * @param mb The moby triangles' bounds.
 * @param i The moby triangle.
 * @param nh Where to list it.
 * @param cs The census; counts the exact tests run.
 * @return 1 when listed, else 0. */
static int ms_hit_one(ms_ws* W, const ms_tri* F, const float fb[6], const float (*mb)[6], int i, int nh, ms_census* cs) {
    const float* b = mb[i];
    if (b[2] < fb[0] || b[0] > fb[2] || b[3] < fb[1] || b[1] > fb[3] || b[5] < fb[4] || b[4] > fb[5])
        return 0;
    if (!ms_box_overlap(&W->mt[i], F)) return 0;
    cs->tests++;
    if (!ms_tri_tri(&W->mt[i], F)) return 0;
    W->ix[nh] = i;
    return 1;
}

/** @brief List the near moby triangles (W->ml) a terrain triangle intersects into W->ix.
 * @param W The worker's scratch.
 * @param F The terrain triangle.
 * @param fb Its bounds.
 * @param mb The moby triangles' bounds.
 * @param nl The near list's length.
 * @param cs The census; counts the exact tests run.
 * @return The count listed. */
static int ms_hits(ms_ws* W, const ms_tri* F, const float fb[6], const float (*mb)[6], int nl, ms_census* cs) {
    int nh = 0;
    for (int j = 0; j < nl; j++) nh += ms_hit_one(W, F, fb, mb, W->ml[j], nh, cs);
    return nh;
}

/** @brief List the near moby triangles of the near grid's cells a terrain triangle's rectangle covers that it intersects into W->ix, each tested once.
 * @param W The worker's scratch.
 * @param F The terrain triangle.
 * @param fb Its bounds.
 * @param mb The moby triangles' bounds.
 * @param cs The census; counts the exact tests run.
 * @return The count listed. */
static int ms_hits_grid(ms_ws* W, const ms_tri* F, const float fb[6], const float (*mb)[6], ms_census* cs) {
    int nh = 0, cx[2], cy[2];
    ms_ng_cells(fb[0], fb[2], W->ng_x0, W->ng_ix, cx);
    ms_ng_cells(fb[1], fb[3], W->ng_y0, W->ng_iy, cy);
    const unsigned q = ++W->mquery;
    for (int y = cy[0]; y <= cy[1]; y++)
        for (int e = W->ng_first[y * MS_NG + cx[0]]; e < W->ng_first[y * MS_NG + cx[1] + 1]; e++) {
            const int i = W->ng_ent[e];
            if (W->mstamp[i] == q) continue;
            W->mstamp[i] = q;
            nh += ms_hit_one(W, F, fb, mb, i, nh, cs);
        }
    return nh;
}


/** @brief The cut falsifier's mode (ms_falsify), a census instrument: OPENPETE_MOBY_SNAP_FALSIFY=1 runs it, =2 runs it as if no plane moved a point (its calibration: the cuts the snap removes).
 * @return 0 (off), 1 or 2. */
static int ms_falsify_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_FALSIFY"); s = e && (*e == '1' || *e == '2') ? *e - '0' : 0; }
    return s;
}

/** @brief The depth the snap stage writes for a moby point at its key depth, or -1 when a plane hides it behind its owner (openpete_snap_rule_vz).
 *
 * The terrain key owner at the point's pixel lies at view-Z zt on the
 * point's ray and the face's bucket is taken as strictly nearer than the
 * owner's (the CPU holds no key image).
 * @param p The point (camera space).
 * @param out The moby's planes.
 * @param np Their count.
 * @param S The largest slab of the present.
 * @param zt The owner's view-Z on the point's ray.
 * @return The written view-Z, or -1. */
static double ms_snap_vz(ms_v3 p, const openpete_snap_plane* out, int np, double S, double zt) {
    const double q[3] = { p.x, p.y, p.z };
    const double vz = openpete_snap_rule_vz(q, p.z, zt, 1, out, np, S);
    return vz > p.z ? -1.0 : vz;
}

/** @brief Whether a screen point lies inside a captured terrain triangle.
 * @param T The triangle.
 * @param x Screen X.
 * @param y Screen Y.
 * @return 1 when inside or on an edge, else 0. */
static int ms_in_tri(const terrain_snap_tri* T, double x, double y) {
    double sg = 0.0;
    for (int k = 0; k < 3; k++) {
        const int l = (k + 1) % 3;
        const double c = ((double)T->sx[l] - T->sx[k]) * (y - T->sy[k]) - ((double)T->sy[l] - T->sy[k]) * (x - T->sx[k]);
        if (c * sg < 0.0) return 0;
        if (c != 0.0) sg = c;
    }
    return 1;
}

/** @brief List the candidates lying in one of a moby's planes (within the merge tolerance).
 * @param out The moby's planes.
 * @param np Their count.
 * @param cj The moby's usable candidates.
 * @param nc Their count.
 * @param own Receives the coplanar candidates.
 * @param ownk Receives, per listed candidate, the plane it lies in.
 * @param cap The capacity of @p own.
 * @return The count listed. */
static int ms_falsify_own(const openpete_snap_plane* out, int np, const int* cj, int nc, int* own, int* ownk, int cap) {
    int no = 0;
    for (int q = 0; q < nc && no < cap; q++) {
        const ms_tri* T = &s_tt[cj[q]];
        for (int k = 0; k < np; k++) {
            const ms_v3 n = { out[k].n[0], out[k].n[1], out[k].n[2] };
            const double c = v3dot(n, T->n);
            if (fabs(c) > 1.0 - MS_MERGE_DN && fabs((c < 0.0 ? -T->d : T->d) - out[k].d) < MS_MERGE_DD) {
                ownk[no] = k;
                own[no++] = cj[q];
                break;
            }
        }
    }
    return no;
}

/** @brief Sample point w of a moby triangle (s_mt): w 0-2 its corners, 3-5 its edge midpoints, 6 its centroid.
 * @param T The triangle.
 * @param w The sample.
 * @return The point. */
static ms_v3 ms_falsify_sample(const ms_tri* T, int w) {
    if (w < 3) return T->p[w];
    if (w == 6) {
        const ms_v3 c = { (T->p[0].x + T->p[1].x + T->p[2].x) / 3.0, (T->p[0].y + T->p[1].y + T->p[2].y) / 3.0,
                          (T->p[0].z + T->p[1].z + T->p[2].z) / 3.0 };
        return c;
    }
    const ms_v3 a = T->p[w - 3], b = T->p[(w - 2) % 3];
    const ms_v3 m = { (a.x + b.x) * 0.5, (a.y + b.y) * 0.5, (a.z + b.z) * 0.5 };
    return m;
}

/** @brief Find the nearest coplanar captured triangle that covers a moby point's screen position, the stand-in for the terrain key owner at its pixel.
 * @param p The point.
 * @param tt The captured triangles.
 * @param own The coplanar candidates.
 * @param no Their count.
 * @param view H, ofx, ofy.
 * @param zt Receives its view-Z along the point's ray.
 * @return Its index in @p own, or -1. */
static int ms_falsify_owner(ms_v3 p, const terrain_snap_tri* tt, const int* own, int no, const float view[3],
                            double* zt) {
    const ms_v3 ray = { p.x / p.z, p.y / p.z, 1.0 };
    const double x = ray.x * view[0] + view[1], y = ray.y * view[0] + view[2];
    int best = -1;
    for (int q = 0; q < no; q++) {
        const ms_tri* T = &s_tt[own[q]];
        const double nr = v3dot(T->n, ray);
        if (fabs(nr) < 1e-9 || !ms_in_tri(&tt[own[q]], x, y)) continue;
        const double zT = T->d / nr;
        if (zT > 1.0 && (best < 0 || zT < *zt)) { *zt = zT; best = q; }
    }
    return best;
}

/** @brief Classify one cut sample (ms_census.cut_cls) and print it under the census when it lies past the covering triangle's plane from the moby's side, within the slab.
 * @param p The sample.
 * @param vz The depth the snap writes for it.
 * @param zt The covering triangle's view-Z on its ray.
 * @param P The covering triangle's plane.
 * @param k That plane's index.
 * @param contact Whether the covering triangle is one of the moby's contact triangles.
 * @param tri The moby triangle and sample (tri * 7 + sample).
 * @param view H, ofx, ofy.
 * @return The class: 0 on the moby's side or past the slab, 1 within it under a contact triangle, 2 within it under a non-contact one. */
static int ms_falsify_class(ms_v3 p, double vz, double zt, const openpete_snap_plane* P, int k, int contact,
                            int tri, const float view[3]) {
    const ms_v3 n = { P->n[0], P->n[1], P->n[2] };
    const double s = P->side * (v3dot(n, p) - P->d), slab = P->slab;
    if (s >= 0.0 || s < -slab) return 0;
    const int cls = contact ? 1 : 2;
    fprintf(stderr, "[moby-snap-cut] key=%u class=%d tri=%d sample=%d sx=%.2f sy=%.2f p=%.2f,%.2f,%.2f vz=%.2f zt=%.2f "
                    "plane=%d s=%.3f slab=%.3f\n",
            openpete_feed_key_now(), cls, tri / 7, tri % 7, p.x / p.z * view[0] + view[1], p.y / p.z * view[0] + view[2],
            p.x, p.y, p.z, vz, zt, k, s, slab);
    return cls;
}

/** @brief Count the sample points of a snapped moby that a drawn terrain triangle lying in one of its own planes still covers in front after the snap (a falsifier census).
 *
 * Samples: each triangle's corners, edge midpoints and centroid at key
 * depth. The nearest captured triangle coplanar with one of the moby's
 * planes (the merge tolerance) covering a sample's screen point stands in
 * for the terrain key owner there; the sample is cut when that triangle
 * lies more than one world unit nearer than the depth the snap writes for
 * it.
 * @param mt The moby's camera-space triangles (ms_ws.mt).
 * @param nt Their count.
 * @param out Its planes.
 * @param np Their count.
 * @param tt The captured terrain triangles.
 * @param cj The moby's usable candidates.
 * @param nc Their count.
 * @param view H, ofx, ofy.
 * @param cls The cut samples by class (ms_falsify_class), updated.
 * @return The cut sample count. */
static long ms_falsify(const ms_tri* mt, int nt, const openpete_snap_plane* out, int np,
                       const terrain_snap_tri* tt, const int* cj, int nc, const float view[3], long cls[3]) {
    double S = 0.0;
    for (int k = 0; k < np; k++) S = ms_max(S, (double)out[k].slab);
    int own[512], ownk[512];
    const int no = ms_falsify_own(out, np, cj, nc, own, ownk, 512);
    long cut = 0;
    for (int i = 0; i < nt && no; i++)
        for (int w = 0; w < 7; w++) {
            const ms_v3 p = ms_falsify_sample(&mt[i], w);
            if (!(p.z > 1.0)) continue;
            double zt = 0.0;
            const int q = ms_falsify_owner(p, tt, own, no, view, &zt);
            if (q < 0) continue;
            const double vz = ms_falsify_on() == 2 ? p.z : ms_snap_vz(p, out, np, S, zt);
            if (vz < 0.0 || !(zt < vz - 1.0)) continue;
            cut++;
            cls[ms_falsify_class(p, vz, zt, &out[ownk[q]], ownk[q], s_tcon[own[q]] == s_mserial, i * 7 + w, view)]++;
        }
    return cut;
}

static void ms_result(const ms_moby* mb, const openpete_snap_plane* out, const ms_pid* id, int np,
                      const float* wb, const double (*kc)[3]);
static void mc_vf_near(const ms_ws* W, const ms_moby* mb, int nc, int nl);

/** @brief Find one queued moby's contact planes against the captured terrain.
 *
 * Reads the queue, the captured terrain's built form (s_tt, s_tb) and world
 * up, and writes only the worker's scratch and census and the outputs, so
 * mobys resolve on any worker in any order; the cut falsifier's touch marks
 * (s_tcon) and the verify's near contacts (mc_vf_near) are written only
 * while those instruments keep the pass serial.
 * @param W The worker's scratch.
 * @param mb The moby.
 * @param tt The captured terrain triangles.
 * @param cand The indices into @p tt of those its search box reaches.
 * @param ncand Their count.
 * @param view H, ofx, ofy.
 * @param cs The census, updated.
 * @param out Receives the planes, MS_MAX_PLANES at most.
 * @param id Receives each plane's source face, parallel to @p out.
 * @return The plane count, or -1 on a scratch allocation failure. */
static int ms_moby_planes(ms_ws* W, const ms_moby* mb, const terrain_snap_tri* tt, const int* cand,
                          int ncand, const float view[3], ms_census* cs, openpete_snap_plane* out,
                          ms_pid* id) {
    const float (*m)[3][3] = (const float (*)[3][3])&s_q_tri[mb->t0];
    const int nt = mb->nt;
    if (!ncand) return 0;
    if (!ms_scratch(W, nt, ncand)) return -1;
    const int fz = ms_falsify_on();
    if (fz) s_mserial++;
    uint64_t t = cs->timed ? ms_mono_ns() : 0, t1;
    float u[6] = { 1e30f, 1e30f, -1e30f, -1e30f, 1e30f, -1e30f };
    int nc = 0;
    for (int q = 0; q < ncand; q++) {
        const float* b = s_tb[cand[q]];
        u[0] = ms_minf(u[0], b[0]); u[1] = ms_minf(u[1], b[1]); u[2] = ms_maxf(u[2], b[2]);
        u[3] = ms_maxf(u[3], b[3]); u[4] = ms_minf(u[4], b[4]); u[5] = ms_maxf(u[5], b[5]);
        if (s_tt[cand[q]].ok) W->cj[nc++] = cand[q];
    }
    const float (*mbnd)[6] = (const float (*)[6])&s_q_bnd[mb->t0];
    const int nl = ms_make_near(W, m, mbnd, nt, u, view);
    if (t) { t1 = ms_mono_ns(); cs->near_ns += t1 - t; t = t1; }
    ms_plane P[MS_MAX_PLANES];
    int np = 0;
    const int grid = nl * nc > MS_NG_PAIRS && ms_ng_build(W, mbnd, nl, u);
    for (int q = 0; q < nc && nl; q++) {
        const int j = W->cj[q];
        const int nh = grid ? ms_hits_grid(W, &s_tt[j], s_tb[j], mbnd, cs) : ms_hits(W, &s_tt[j], s_tb[j], mbnd, nl, cs);
        if (!nh) continue;
        if (fz) s_tcon[j] = s_mserial;
        cs->contacts++;
        cs->contacts_t4 += tt[j].ftris >= 16;
        if (!ms_plane_add(P, &np, &s_tt[j], &tt[j])) cs->ovf++;
    }
    if (t) { t1 = ms_mono_ns(); cs->test_ns += t1 - t; t = t1; }
    mc_vf_near(W, mb, nc, nl);
    if (!np) return 0;
    ms_make_rest(W, m, nt, view);
    for (int k = 0; k < np; k++) {
        ms_plane_finish(W->mt, nt, mb->dofs, &P[k], s_up_p, &out[k]);
        id[k] = P[k].id;
    }
    cs->planes += np;
    if (t) cs->plane_ns += ms_mono_ns() - t;
    if (fz) {
        const long b0 = cs->cut_cls[1];
        const long cut = ms_falsify(W->mt, nt, out, np, tt, W->cj, nc, view, cs->cut_cls);
        cs->cut_samples += cut;
        cs->cut_mobys += cut > 0;
        cs->cut_b_mobys += cs->cut_cls[1] > b0;
    }
    ms_dump(mb, out, np, view);
    return np;
}

/** @brief Add a moby's planes to the scene's snap table and note its ordinal for the collision census.
 * @param mb The moby.
 * @param out Its planes.
 * @param np Their count, > 0.
 * @param cs The census; counts refused planes as overflow.
 * @return np when added, 0 when the table refused them. */
static int ms_add(const ms_moby* mb, const openpete_snap_plane* out, int np, ms_census* cs) {
    if (!openpete_native_snap_add(mb->mid, mb->mord, mb->off, out, np)) { cs->ovf += np; return 0; }
    if (mb->mord > 0 && s_sn_n < 256) { s_sn[s_sn_n].mid = mb->mid; s_sn[s_sn_n++].ord = mb->mord; }
    return np;
}

/* One resolve chunk's per-moby results, by chunk index (at most 128): the
 * planes, their source faces and the count (-1 on a scratch allocation
 * failure). The fan-out's workers write them (ms_fo_item); the identity
 * check (ms_fo_check) has the serial pass record its own into the same form
 * (s_ck_rec). */
typedef struct { openpete_snap_plane pl[MS_MAX_PLANES]; ms_pid id[MS_MAX_PLANES]; int np; } ms_fo_res;
static ms_fo_res s_fo_res[128];
static ms_fo_res s_ck_res[128];
static int s_lc_serial;              /* the listed capture's check runs the contact pass serially */
static uint8_t s_lc_snapped[128];    /* and keeps its snapped flags here */
static const ms_moby* s_ck_rec;   /* the chunk's first moby while the check's serial pass records, else NULL */

/** @brief The search box of one queued moby: its screen rectangle widened by one pixel and its key depth range.
 * @param mb The moby.
 * @param b Receives the box. */
static void ms_box(const ms_moby* mb, terrain_snap_box* b) {
    b->x0 = mb->b[0] - 1.0f; b->y0 = mb->b[1] - 1.0f;
    b->x1 = mb->b[2] + 1.0f; b->y1 = mb->b[3] + 1.0f;
    b->z0 = mb->b[4]; b->z1 = mb->b[5];
}

/** @brief Whether OPENPETE_MOBY_RANK_TIME=1, the moby paint-key cost instrument, also prints the snap's contact cost.
 * @return 1 when it does, else 0. */
static int ms_census_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_RANK_TIME"); s = e && *e == '1'; }
    return s;
}

/** @brief The calling thread's CPU time in nanoseconds.
 * @return The time. */
static uint64_t ms_cpu_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/** @brief Print one resolve's census line with the running mean contact-stage CPU time.
 * @param cs The resolve's counters.
 * @param ns Its CPU time. */
static void ms_census_print(const ms_census* cs, uint64_t ns) {
    static double s_sum_us; static long s_n;
    s_sum_us += (double)ns / 1000.0; s_n++;
    int ne = 0, np = 0; unsigned ovf = 0;
    openpete_native_snap_census(&ne, &np, &ovf);
    fprintf(stderr, "[moby-snap] key=%u pulled=%ld cand_faces=%ld cand_tris=%ld owned_sectors=%ld "
                    "tritri=%ld contact_tris=%ld contact_t4=%ld overlaps=%ld snap_zmin=%.0f ord_collide=%ld planes=%ld table_ents=%d table_planes=%d "
                    "overflow=%ld+%u cut_mobys=%ld cut_samples=%ld cut_a=%ld cut_b=%ld cut_c=%ld cut_b_mobys=%ld cpu_us=%.1f capture_us=%.1f near_us=%.1f test_us=%.1f plane_us=%.1f mean_cpu_us=%.1f n=%ld "
                    "carried=%ld carry_fresh=%ld\n",
            openpete_feed_key_now(), cs->pulled, cs->faces, cs->tris, cs->sectors, cs->tests,
            cs->contacts, cs->contacts_t4, cs->overlaps, (double)cs->zmin, ms_ord_collisions(), cs->planes, ne, np, cs->ovf, ovf, cs->cut_mobys, cs->cut_samples, cs->cut_cls[0], cs->cut_cls[1], cs->cut_cls[2], cs->cut_b_mobys, (double)ns / 1000.0,
            (double)cs->cap_ns / 1000.0, (double)cs->near_ns / 1000.0, (double)cs->test_ns / 1000.0,
            (double)cs->plane_ns / 1000.0, s_sum_us / (double)s_n, s_n, cs->carried, cs->fresh);
}

/** @brief Bucket the captured triangles by the search boxes they reach (a counting sort over the hit sets).
 * @param tt The captured triangles.
 * @param ntt Their count.
 * @param nb Box count.
 * @param first Receives, for each box k, the start of its run in the
 *        result, and first[nb] the total.
 * @return The indices into @p tt, box by box; valid until the next call. */
static const int* ms_cands(const terrain_snap_tri* tt, int ntt, int nb, int* first) {
    static int* s_c; static int s_cap;
    int cnt[129] = { 0 };
    for (int j = 0; j < ntt; j++)
        for (int w = 0; w < 2; w++)
            for (uint64_t r = tt[j].hit[w]; r; r &= r - 1) cnt[w * 64 + __builtin_ctzll(r)]++;
    first[0] = 0;
    for (int k = 0; k < nb; k++) first[k + 1] = first[k] + cnt[k];
    if (!ms_grow((void**)&s_c, &s_cap, first[nb] > 0 ? first[nb] : 1, sizeof *s_c)) {
        for (int k = 0; k <= nb; k++) first[k] = 0;
        return s_c;
    }
    int at[128];
    memcpy(at, first, (size_t)nb * sizeof at[0]);
    for (int j = 0; j < ntt; j++)
        for (int w = 0; w < 2; w++)
            for (uint64_t r = tt[j].hit[w]; r; r &= r - 1) s_c[at[w * 64 + __builtin_ctzll(r)]++] = j;
    return s_c;
}

/** @brief Count the pairs of snapped mobys whose screen boxes overlap: places where two sunk parts may share a ray.
 * @param box The mobys' search boxes.
 * @param snapped 1 for a moby that received planes.
 * @param nb Box count.
 * @return The pair count. */
static long ms_overlaps(const terrain_snap_box* box, const uint8_t* snapped, int nb) {
    long n = 0;
    for (int a = 0; a < nb; a++)
        for (int b = a + 1; b < nb; b++)
            n += snapped[a] && snapped[b] && box[a].x0 < box[b].x1 && box[b].x0 < box[a].x1 &&
                 box[a].y0 < box[b].y1 && box[b].y0 < box[a].y1;
    return n;
}

/* ---- the snap carry ------------------------------------------------------
 * A canonical resolve (one outside a sub-tick re-extract, moby_snap_present)
 * stores each queued moby's contact planes in world space, keyed by moby
 * identity, in the table of its canonical epoch and native scene build; the
 * tables of the two newest epochs are kept (T-1 and T). While the carry is
 * on (the default; OPENPETE_MOBY_SNAP_CARRY=0 turns it off), a sub-tick re-extract maps the planes of
 * the endpoint the moby rank carry selects into its own camera instead of
 * capturing and testing the terrain again, and resolves only the mobys the
 * endpoint did not resolve. The terrain walk's camera space is
 * c = L (w - P) (terrain_snap_camera), so a camera plane n.c = d is the
 * world plane (L^T n).w = d + (L^T n).P, and a world plane m.w = e is
 * (L'^-T m).c' = e - m.P' in a camera c' = L' (w - P'), scaled to a unit
 * normal. Both maps keep the plane function's value at every world point
 * up to a positive factor, so the side the moby stands on, the slab and the
 * double-sided flag carry unchanged, except that a single-sided plane takes
 * the eye's side in the present camera (openpete_snap_side), which a camera
 * crossing the plane within the tick changes. A present whose camera
 * equals the endpoint's bit for bit takes the planes as they were
 * resolved. */

/* One stored plane in world space, and the camera-space plane it came from. */
typedef struct { double n[3], d; } mc_wplane;
/* One terrain face a stored moby may contact: level sector, walk pass,
 * face index, and its walk verdict at the store (TERRAIN_FACE_*). */
typedef struct { int16_t sec; uint8_t pass, verd; uint16_t face; } mc_fc;
/* One stored moby: identity, first plane, plane count (0: resolved, no
 * contact), and, while the carry or the verify runs, the world box of its
 * corners at raster depth (wb, valid when wb_ok) and the corners themselves
 * at key depth (nwc from wc0 in the table's wc, triangle by triangle:
 * ms_moby_kcorner), a signature of its
 * guest state at the store (sig, valid when sig_ok: mc_moby_sig), and its
 * contact region (rb: ms_moby_cbox widened by MC_CAND) with the faces of the visible
 * sectors in it and their verdicts (nf from f0 in the table's fc; fc_ok 0
 * when they were not read: mc_store_cands), and the resolve's H, ofx, ofy
 * (kv) the corners were unprojected with. */
typedef struct { uint32_t mid; int p0, np; float wb[6]; int wb_ok; int wc0, nwc; uint32_t sig; int sig_ok;
                 float rb[6]; int f0, nf, fc_ok;
                 int path; float off, kr; float kv[3]; } mc_ent;   /* path, off: the probe's (moby_snap_queue_probe) */
#define MC_HBITS 10
#define MC_HSIZE (1 << MC_HBITS)
typedef struct {
    uint32_t  epoch;           /* canonical epoch (spyro_native_canon_epoch_stamp) */
    unsigned  scene;           /* native scene build (openpete_native_scene_frame) */
    int       valid;
    double    L[3][3], P[3];   /* the camera of the first store */
    int       cam_mixed;       /* a later store of the table ran under another camera */
    mc_ent*   e;   int ne, cap_e;
    mc_wplane* w;  int npl, cap_p;
    openpete_snap_plane* c; int cap_c;   /* parallel to w: the plane as resolved */
    ms_pid*   id;  int cap_id;           /* parallel to w: the plane's source face */
    double  (*wc)[3]; int nwc, cap_wc;   /* the stored mobys' world corners (verify) */
    float   (*rc)[3]; int cap_rc;        /* parallel to wc: each corner as queued (screen x, y, key depth) */
    uint32_t* fk;  int cap_fk;           /* parallel to wc while the probe is on: the corner's face key */
    uint64_t  vis[16];                   /* the stores' build's visible sectors (terrain_snap_visible) */
    int       vis_ok;
    mc_fc*    fc;  int nfc, cap_fc;      /* the stored mobys' candidate faces (mc_ent.f0) */
    unsigned  hgen;            /* generation the slots below hold */
    unsigned  hg[MC_HSIZE];
    int       hi[MC_HSIZE];
    long      dup, full;       /* repeated identities; mobys refused at 3/4 of MC_HSIZE */
} mc_tab;
static mc_tab   s_mc[2];
static int      s_mc_cur;      /* the latest epoch's table */
static unsigned s_mc_gen;
static long     s_mc_rebuilds; /* tables emptied by a second scene build within one epoch */

/* The leg of the resolve under way (moby_snap_present) and, for the present
 * that resolves, the per-present counts the profiler emits. */
static int      s_rex;
static double   s_rex_alpha = 1.0;
/* One sub-tick present's snap counts: mobys the carry placed and the ones
 * resolved fresh; of those, the ones mc_refresh refused and, of these, the
 * ones refused on a face-verdict change (mc_faces_changed); resolves run,
 * terrain captures (one per chunk), cache-owned sectors the captures walked,
 * walk passes that queued a moby and mobys queued; with the census on
 * (OPENPETE_SNAP_REX_CENSUS) the resolves' and the captures' (chunk runs')
 * CPU time; per walk pass (at most MS_PC_SEGS, the last holding the rest)
 * its queued mobys, resolves and captures. */
#define MS_PC_SEGS 6
typedef struct {
    long carried, fresh, refreshed, fsref, resolves, captures, walked, passes, queued;
    uint64_t res_ns, chunk_ns;
    long seg_q[MS_PC_SEGS], seg_res[MS_PC_SEGS], seg_cap[MS_PC_SEGS];
} ms_pc;
static ms_pc    s_pc;
static int      s_pc_mark;       /* the queue length when the last walk pass ended */
static void ms_rex_end(void) OP_REQUIRES(sim);

/* The resolve under way: the table a canonical resolve stores into, and the
 * walk camera it stores under. */
static mc_tab*  s_store;
static double   s_rs_L[3][3], s_rs_P[3];
static const mc_tab* s_rs_ep;   /* a sub-tick resolve's endpoint, for its verify */
static double   s_rs_Li[3][3];  /* the inverse of s_rs_L, while the verify runs */
static float    s_rs_view[3];   /* H, ofx, ofy of the resolve under way */
static int      s_rs_wb;        /* s_rs_Li holds: moby world boxes are computed */

/** @brief One queued moby triangle corner unprojected at its key depth less a depth offset, in world space, under the resolve's camera.
 * @param mb The moby.
 * @param t The triangle, relative to the moby's run.
 * @param k The corner.
 * @param off The depth subtracted from the key depth.
 * @param w Receives the world point. */
static void ms_moby_corner_at(const ms_moby* mb, int t, int k, double off, double w[3]) {
    const double H = s_rs_view[0], ofx = s_rs_view[1], ofy = s_rs_view[2];
    const float* v = s_q_tri[mb->t0 + t][k];
    const double z = (double)v[2] - off;
    const double c[3] = { ((double)v[0] - ofx) * z / H, ((double)v[1] - ofy) * z / H, z };
    for (int i = 0; i < 3; i++)
        w[i] = s_rs_Li[i][0] * c[0] + s_rs_Li[i][1] * c[1] + s_rs_Li[i][2] * c[2] + s_rs_P[i];
}

/** @brief One queued moby triangle corner at its raster depth (key depth less off) in world space, under the resolve's camera.
 *
 * The raster depth carries the tie pull and the near-floor lift, so the
 * point lies off the corner along the view ray: the contact tests' space.
 * @param mb The moby.
 * @param t The triangle, relative to the moby's run.
 * @param k The corner.
 * @param w Receives the world point. */
static void ms_moby_wcorner(const ms_moby* mb, int t, int k, double w[3]) {
    ms_moby_corner_at(mb, t, k, (double)mb->off, w);
}

/** @brief One queued moby triangle corner at its key depth, the projected geometry's own view-Z, in world space, under the resolve's camera.
 *
 * The screen position and the key depth are one projection of the model
 * vertex, so the point is the vertex itself: a moby still in the world
 * gives the same point under any camera, which the raster-depth point
 * (ms_moby_wcorner) does not, since its pull lies along a ray the camera
 * turns.
 * @param mb The moby.
 * @param t The triangle, relative to the moby's run.
 * @param k The corner.
 * @param w Receives the world point. */
static void ms_moby_kcorner(const ms_moby* mb, int t, int k, double w[3]) {
    ms_moby_corner_at(mb, t, k, 0.0, w);
}

/** @brief The world box of a queued moby's triangle corners at raster depth (ms_moby_wcorner).
 * @param mb The moby.
 * @param wb Receives x0, y0, z0, x1, y1, z1. */
static void ms_moby_wbox(const ms_moby* mb, float wb[6]) {
    for (int i = 0; i < 3; i++) { wb[i] = 1e30f; wb[i + 3] = -1e30f; }
    for (int t = 0; t < mb->nt; t++)
        for (int k = 0; k < 3; k++) {
            double w[3];
            ms_moby_wcorner(mb, t, k, w);
            for (int i = 0; i < 3; i++) {
                wb[i] = ms_minf(wb[i], (float)w[i]); wb[i + 3] = ms_maxf(wb[i + 3], (float)w[i]);
            }
        }
}

/* Per queued triangle corner (3 t0 + 3q + k), for the resolve under way:
 * its key-depth world point, as the carry's store records it
 * (ms_store_prep). */
static double (*s_kc)[3]; static int s_kc_cap;

/** @brief Grow the key-depth corner slab (s_kc) to the queue's triangles; the fan-out grows it before its workers write their mobys' runs.
 * @return 1 when it holds them, 0 on an allocation failure. */
static int ms_store_kc_grow(void) {
    return ms_grow((void**)&s_kc, &s_kc_cap, 3 * s_q_nt, sizeof *s_kc);
}

/** @brief Compute what the carry's store records of a queued moby's geometry under the resolve's camera: the world box of its corners at raster depth (ms_moby_wbox) and every corner at key depth (ms_moby_kcorner).
 *
 * A pure function of the queue and the resolve's camera; the serial pass
 * and the fan-out's workers both produce a moby's record through it, and
 * mc_store_geom copies it into the table.
 * @param mb The moby.
 * @param wb Receives the world box.
 * @param kc Receives 3 corners per triangle, or NULL to compute the box alone. */
static void ms_store_prep(const ms_moby* mb, float wb[6], double (*kc)[3]) {
    ms_moby_wbox(mb, wb);
    for (int q = 0; kc && q < mb->nt; q++)
        for (int c = 0; c < 3; c++) ms_moby_kcorner(mb, q, c, kc[3 * q + c]);
}

/** @brief A world box containing a queued moby's triangle corners at their stored view-Z less a depth offset: the world box of the eight corners of their camera-space box, under the resolve's camera.
 *
 * It contains every such corner (ms_moby_corner_at), and costs one camera-space
 * bound per corner where the exact world box costs one world transform per
 * corner. The x and y bounds are taken over (v - of) * z and scaled by 1/H
 * once: multiplying by a positive constant rounds monotonically, so the
 * scaled minimum and maximum equal the per-corner ones. Two interleaved
 * accumulators split the dependency chain; minimum and maximum are
 * order-independent.
 * @param mb The moby.
 * @param off The depth subtracted from each corner's stored view-Z.
 * @param wb Receives x0, y0, z0, x1, y1, z1; inverted (x0 > x1) for a moby without triangles. */
static void ms_moby_box_at(const ms_moby* mb, double off, float wb[6]) {
    const double iH = 1.0 / s_rs_view[0], ofx = s_rs_view[1], ofy = s_rs_view[2];
    double lo[2][3] = { { 1e300, 1e300, 1e300 }, { 1e300, 1e300, 1e300 } };
    double hi[2][3] = { { -1e300, -1e300, -1e300 }, { -1e300, -1e300, -1e300 } };
    for (int i = 0; i < 3; i++) { wb[i] = 1e30f; wb[i + 3] = -1e30f; }
    if (!mb->nt) return;
    const float (*v)[3] = (const float (*)[3])s_q_tri[mb->t0];
    for (int j = 0, n = mb->nt * 3; j < n; j++) {
        const int s = j & 1;
        const double z = (double)v[j][2] - off;
        const double c[3] = { ((double)v[j][0] - ofx) * z, ((double)v[j][1] - ofy) * z, z };
        for (int i = 0; i < 3; i++) { lo[s][i] = ms_min(lo[s][i], c[i]); hi[s][i] = ms_max(hi[s][i], c[i]); }
    }
    const double sc[3] = { iH, iH, 1.0 };
    double L[3], U[3];
    for (int i = 0; i < 3; i++) {
        L[i] = ms_min(lo[0][i], lo[1][i]) * sc[i];
        U[i] = ms_max(hi[0][i], hi[1][i]) * sc[i];
    }
    for (int q = 0; q < 8; q++) {
        const double c[3] = { (q & 1) ? U[0] : L[0], (q & 2) ? U[1] : L[1], (q & 4) ? U[2] : L[2] };
        for (int i = 0; i < 3; i++) {
            const double w = s_rs_Li[i][0] * c[0] + s_rs_Li[i][1] * c[1] + s_rs_Li[i][2] * c[2] + s_rs_P[i];
            wb[i] = ms_minf(wb[i], (float)w); wb[i + 3] = ms_maxf(wb[i + 3], (float)w);
        }
    }
}

/** @brief A world box containing a queued moby's triangle corners at raster depth (ms_moby_wcorner), under the resolve's camera (ms_moby_box_at).
 * @param mb The moby.
 * @param wb Receives x0, y0, z0, x1, y1, z1; inverted (x0 > x1) for a moby without triangles. */
static void ms_moby_cbox(const ms_moby* mb, float wb[6]) { ms_moby_box_at(mb, (double)mb->off, wb); }

/* Per queue index, for the resolve under way: the world box of the moby's
 * corners' camera-space box (ms_moby_cbox) where a fan-out already computed
 * it (s_cb_ok 1), for s_cb_n queue indices. */
static float  (*s_cb)[6]; static int s_cb_cap;
static uint8_t* s_cb_ok;  static int s_cb_okcap;
static int      s_cb_n;

/** @brief Size the resolve's precomputed corner boxes for the queue and mark none computed.
 *
 * On an allocation failure no box counts as computed (s_cb_n 0), and every
 * reader computes its own. */
static void ms_cb_begin(void) {
    s_cb_n = 0;
    if (!ms_grow((void**)&s_cb, &s_cb_cap, s_q_nm, sizeof *s_cb) ||
        !ms_grow((void**)&s_cb_ok, &s_cb_okcap, s_q_nm, 1)) return;
    memset(s_cb_ok, 0, (size_t)s_q_nm);
    s_cb_n = s_q_nm;
}

/** @brief A queued moby's corner box (ms_moby_cbox): the fan-out's copy when it computed one, else computed here.
 * @param k The moby's queue index.
 * @param mb The moby.
 * @param wb Receives x0, y0, z0, x1, y1, z1. */
static void ms_cb_get(int k, const ms_moby* mb, float wb[6]) {
    if (k >= 0 && k < s_cb_n && s_cb_ok[k]) memcpy(wb, s_cb[k], sizeof s_cb[k]);
    else ms_moby_cbox(mb, wb);
}

static struct CPUState* s_vf_cpu;   /* the guest, for the verify's moby class and state signature */

/** @brief A signature (FNV-1a) of the moby record's guest state its drawn pose is decoded from: position, angle word, animation-state word, and the rotation and screen-scale fields where the game has them.
 * @param mid The moby's scene identity (record word index plus one).
 * @param sig Receives the signature.
 * @return 1 when read, 0 when the guest is unknown. */
static int mc_moby_sig(uint32_t mid, uint32_t* sig) {
    if (!s_vf_cpu || !mid) return 0;
    const uint32_t a = 0x80000000u | ((mid - 1u) << 2);
    uint32_t w[11], h = 2166136261u;
    int n = 0;
    for (int i = 0; i < 3; i++) w[n++] = s_vf_cpu->read_word(a + game_layout.moby.pos_off + 4u * (uint32_t)i);
    w[n++] = s_vf_cpu->read_word(a + game_layout.moby.angle_off);
    w[n++] = s_vf_cpu->read_word(a + game_layout.moby.anim_state_off);
    if (game_layout.moby.rot_off)
        for (int i = 0; i < 5; i++) w[n++] = s_vf_cpu->read_word(a + game_layout.moby.rot_off + 4u * (uint32_t)i);
    if (game_layout.moby.size_off) w[n++] = s_vf_cpu->read_byte(a + game_layout.moby.size_off);
    for (int i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
    *sig = h;
    return 1;
}

/** @brief Whether the snap carry is on (the default; OPENPETE_MOBY_SNAP_CARRY=0 turns it off), read once.
 * @return 1 when on, else 0. */
static int mc_carry_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_CARRY"); s = !(e && *e == '0'); }
    return s;
}

/** @brief The carry verify instrument's mode (OPENPETE_MOBY_SNAP_CARRY_VERIFY), read once.
 *
 * 0 off; 1 compares the carry against a fresh resolve on every sub-tick
 * resolve; 2 is 1 with the carry mapped under the endpoint's own camera (the
 * identity transform, the negative control); 3 is 1 with the map applied
 * even where the cameras are equal (calibrates the map on a static camera).
 * @return The mode. */
static int mc_verify_mode(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_CARRY_VERIFY");
                 s = e && *e >= '1' && *e <= '3' ? *e - '0' : 0; }
    return s;
}

void moby_snap_present(int rex, double alpha) OP_REQUIRES(sim) {
    if (!rex && s_rex) ms_rex_end();
    s_rex = rex;
    s_rex_alpha = rex ? alpha : 1.0;
    if (!rex) return;
    memset(&s_pc, 0, sizeof s_pc);
    ms_scene_sync();
    s_q_seg = 0;
    s_pc_mark = s_q_nm;
}

/** @brief Hash a moby identity to a table slot.
 * @param mid The identity.
 * @return The slot. */
static unsigned mc_hash(uint32_t mid) { return (mid * 2654435761u) >> (32 - MC_HBITS); }

/** @brief Find a moby in a table.
 * @param t The table.
 * @param mid The identity.
 * @return Its entry index, or -1. */
static int mc_find(const mc_tab* t, uint32_t mid) {
    for (unsigned h = mc_hash(mid); t->hg[h] == t->hgen; h = (h + 1) & (MC_HSIZE - 1))
        if (t->e[t->hi[h]].mid == mid) return t->hi[h];
    return -1;
}

/** @brief Empty a table for a store under canonical epoch ep from scene build scene.
 *
 * Hands out a fresh slot generation; a wrap clears both tables' slots first.
 * @param t The table.
 * @param ep The canonical epoch.
 * @param scene The native scene build. */
static void mc_empty(mc_tab* t, uint32_t ep, unsigned scene) {
    if (!++s_mc_gen) {
        for (int k = 0; k < 2; k++) memset(s_mc[k].hg, 0, sizeof s_mc[k].hg);
        s_mc_gen = 1;
    }
    t->hgen = s_mc_gen; t->ne = t->npl = t->nwc = t->nfc = 0; t->vis_ok = 0; t->epoch = ep; t->scene = scene; t->valid = 1;
    t->cam_mixed = -1;
    t->dup = t->full = 0;
}

/** @brief The table a canonical resolve under epoch ep from scene build scene stores into.
 *
 * The rank carry's rule (rank_carry_table_for): the latest table while it
 * holds ep and was stored by the same build, so the passes of one build
 * accumulate; a later build under the same epoch empties it in place; the
 * first store of a new epoch turns the latest table into T-1 and empties the
 * other for the new tick.
 * @param ep The canonical epoch.
 * @param scene The native scene build.
 * @return The table. */
static mc_tab* mc_table_for(uint32_t ep, unsigned scene) {
    mc_tab* t = &s_mc[s_mc_cur];
    if (t->valid && ep == t->epoch) {
        if (scene != t->scene) { mc_empty(t, ep, scene); s_mc_rebuilds++; }
        return t;
    }
    s_mc_cur ^= 1;
    t = &s_mc[s_mc_cur];
    mc_empty(t, ep, scene);
    return t;
}

/** @brief Note the camera a store into a table runs under: the first store's is kept, a different later one marks the table mixed.
 * @param t The table.
 * @param L The walk camera's linear part.
 * @param P Its position. */
static void mc_cam_note(mc_tab* t, const double L[3][3], const double P[3]) {
    if (t->cam_mixed < 0) {
        memcpy(t->L, L, sizeof t->L); memcpy(t->P, P, sizeof t->P);
        t->cam_mixed = 0;
    } else if (memcmp(t->L, L, sizeof t->L) || memcmp(t->P, P, sizeof t->P)) {
        t->cam_mixed = 1;
    }
}

/** @brief Record a stored moby's geometry in its entry: its world box, the probe's fields, and its key-depth corners with, while the probe is on, their face keys.
 *
 * The entry keeps no corners (nwc 0) when none were computed or the table
 * cannot grow; the refresh test then treats the moby as unrecorded.
 * @param t The table.
 * @param e The moby's entry, just claimed.
 * @param mb The moby.
 * @param wb Its world box (ms_store_prep).
 * @param kc Its key-depth world corners, 3 per triangle, or NULL. */
static void mc_store_geom(mc_tab* t, mc_ent* e, const ms_moby* mb, const float* wb, const double (*kc)[3]) {
    const int n = 3 * mb->nt;
    memcpy(e->wb, wb, sizeof e->wb);
    memcpy(e->kv, s_rs_view, sizeof e->kv);
    e->path = mb->path; e->off = mb->off; e->kr = mb->kr;
    if (!kc || !ms_grow((void**)&t->wc, &t->cap_wc, t->nwc + n, sizeof *t->wc) ||
        !ms_grow((void**)&t->rc, &t->cap_rc, t->nwc + n, sizeof *t->rc)) return;
    if (moby_snap_probe_on() && ms_grow((void**)&t->fk, &t->cap_fk, t->nwc + n, sizeof *t->fk))
        for (int i = 0; i < n; i++) t->fk[t->nwc + i] = s_q_fk[mb->t0 + i / 3];
    memcpy(t->wc[t->nwc], kc, (size_t)n * sizeof *t->wc);
    memcpy(t->rc[t->nwc], s_q_tri[mb->t0], (size_t)n * sizeof *t->rc);
    t->nwc += n;
    e->nwc = n;
}

/** @brief Store one resolved moby's planes, mapped to world space under the resolve's camera, in the table.
 *
 * A moby stored twice in one table keeps the later planes (counted as dup).
 * While the carry or the verify runs the entry also keeps the moby's world
 * box and world corners (mc_corner_delta) and its guest state signature
 * (mc_moby_sig), the carry's refresh test (mc_refresh).
 * @param t The table.
 * @param mb The moby.
 * @param out Its camera-space planes.
 * @param id Their source faces.
 * @param np Their count, 0 for a moby resolved with no contact.
 * @param wb The moby's world box (ms_store_prep), NULL when the resolve keeps no world boxes (s_rs_wb 0).
 * @param kc Its key-depth world corners (ms_store_prep), 3 per triangle, NULL when none were computed. */
static void mc_store(mc_tab* t, const ms_moby* mb, const openpete_snap_plane* out, const ms_pid* id, int np,
                     const float* wb, const double (*kc)[3]) {
    const uint32_t mid = mb->mid;
    if (t->ne >= MC_HSIZE - (MC_HSIZE >> 2)) { t->full++; return; }
    if (!ms_grow((void**)&t->e, &t->cap_e, t->ne + 1, sizeof *t->e) ||
        !ms_grow((void**)&t->w, &t->cap_p, t->npl + np + 1, sizeof *t->w) ||
        !ms_grow((void**)&t->c, &t->cap_c, t->npl + np + 1, sizeof *t->c) ||
        !ms_grow((void**)&t->id, &t->cap_id, t->npl + np + 1, sizeof *t->id)) return;
    unsigned h = mc_hash(mid);
    while (t->hg[h] == t->hgen && t->e[t->hi[h]].mid != mid) h = (h + 1) & (MC_HSIZE - 1);
    t->dup += t->hg[h] == t->hgen;
    t->hg[h] = t->hgen; t->hi[h] = t->ne;
    mc_ent* e = &t->e[t->ne++];
    e->mid = mid; e->p0 = t->npl; e->np = np;
    e->wb_ok = s_rs_wb;
    e->wc0 = t->nwc; e->nwc = 0;
    e->f0 = e->nf = e->fc_ok = 0;
    e->kv[0] = e->kv[1] = e->kv[2] = 0.0f;
    e->sig_ok = mc_moby_sig(mid, &e->sig);
    if (wb) mc_store_geom(t, e, mb, wb, kc);
    for (int k = 0; k < np; k++) {
        mc_wplane* w = &t->w[t->npl];
        double dw = out[k].d;
        for (int i = 0; i < 3; i++) {
            w->n[i] = s_rs_L[0][i] * out[k].n[0] + s_rs_L[1][i] * out[k].n[1] + s_rs_L[2][i] * out[k].n[2];
            dw += w->n[i] * s_rs_P[i];
        }
        w->d = dw;
        t->id[t->npl] = id[k];
        t->c[t->npl++] = out[k];
    }
}

/** @brief The table whose planes this present's re-extract carries, or NULL when that endpoint has none.
 *
 * The rank carry's endpoint (rank_carry_endpoint): alpha at or above 0.5
 * takes the latest table, below it the T-1 table; the latest counts only
 * while its epoch is the last canonical present's, T-1 only while its epoch
 * directly precedes the latest's.
 * @return The table, or NULL. */
static const mc_tab* mc_endpoint(void) {
    const mc_tab* latest = &s_mc[s_mc_cur];
    const mc_tab* prev = &s_mc[s_mc_cur ^ 1];
    if (!latest->valid || !spyro_native_canon_epoch_fresh(latest->epoch)) return NULL;
    if (s_rex_alpha >= 0.5) return latest;
    return (prev->valid && prev->epoch + 1u == latest->epoch) ? prev : NULL;
}

/** @brief Write the inverse transpose of a 3x3 matrix.
 * @param L The matrix.
 * @param Q Receives L^-T.
 * @return 0 when L is singular, else 1. */
static int mc_inv_t(const double L[3][3], double Q[3][3]) {
    double C[3][3];
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            C[r][c] = L[r1][c1] * L[r2][c2] - L[r1][c2] * L[r2][c1];
        }
    const double det = L[0][0] * C[0][0] + L[0][1] * C[0][1] + L[0][2] * C[0][2];
    if (!(fabs(det) > 1e-30)) return 0;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) Q[r][c] = C[r][c] / det;
    return 1;
}

/** @brief Map one stored world plane into a camera, with a unit normal.
 * @param w The world plane.
 * @param src The plane as resolved (side, slab, double-sided flag).
 * @param Q The camera's L^-T.
 * @param P The camera's position.
 * @param o Receives the camera-space plane.
 * @return 0 when the mapped normal is degenerate, else 1. */
static int mc_map_plane(const mc_wplane* w, const openpete_snap_plane* src, const double Q[3][3],
                        const double P[3], openpete_snap_plane* o) {
    double n[3], d = w->d;
    for (int i = 0; i < 3; i++) {
        n[i] = Q[i][0] * w->n[0] + Q[i][1] * w->n[1] + Q[i][2] * w->n[2];
        d -= w->n[i] * P[i];
    }
    const double l = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(l > 0.0)) return 0;
    *o = *src;
    for (int i = 0; i < 3; i++) o->n[i] = (float)(n[i] / l);
    o->d = (float)(d / l);
    if (!src->dside) {   /* openpete_snap_side: a single-sided plane's side is the eye's */
        const double nn[3] = { o->n[0], o->n[1], o->n[2] };
        o->side = (float)openpete_snap_side(nn, o->d, 0, NULL, 0.0);
    }
    return 1;
}

/** @brief The squared distance between two world points.
 * @param a First point.
 * @param b Second point.
 * @return The squared distance. */
static double mc_d2(const double a[3], const double b[3]) {
    const double dx = (double)a[0] - b[0], dy = (double)a[1] - b[1], dz = (double)a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

/** @brief The largest distance from a point of set a to its nearest point of set b.
 * @param a The first set.
 * @param na Its count.
 * @param b The second set.
 * @param nb Its count, > 0.
 * @return The distance. */
static double mc_subset_dist(const double (*a)[3], int na, const double (*b)[3], int nb) {
    double worst = 0.0;
    for (int i = 0; i < na; i++) {
        double best = 1e300;
        for (int j = 0; j < nb && best > 0.0; j++) best = ms_min(best, mc_d2(a[i], b[j]));
        worst = ms_max(worst, best);
    }
    return sqrt(worst);
}

/** @brief How far a queued moby's projected triangle corners moved from a stored entry's, in world space at key depth under each leg's own camera.
 *
 * With equal triangle counts the corners correspond in walk order and the
 * result is the largest distance between a corner and the same corner at
 * the endpoint. With unequal counts (the camera culled or clipped other
 * faces, or the animation changed the face set) it is the largest distance
 * from a corner of the leg with fewer triangles to the nearest corner of
 * the other: 0 when one face set's corners are a subset of the other's.
 * @param ep The table holding the entry.
 * @param e The moby's stored entry.
 * @param mb The queued moby (its corners unprojected by ms_moby_kcorner).
 * @param topo Set to 1 when the triangle counts differ, else 0.
 * @return The distance in world units, or -1 when the entry kept no corners. */
static double mc_corner_delta(const mc_tab* ep, const mc_ent* e, const ms_moby* mb, int* topo) {
    static double (*s_pw)[3]; static int s_pwcap;
    *topo = 0;
    if (!e->nwc || !mb->nt || !ms_grow((void**)&s_pw, &s_pwcap, 3 * mb->nt, sizeof *s_pw)) return -1.0;
    for (int q = 0; q < mb->nt; q++)
        for (int k = 0; k < 3; k++) ms_moby_kcorner(mb, q, k, s_pw[3 * q + k]);
    const double (*ew)[3] = (const double (*)[3])&ep->wc[e->wc0];
    const int np = 3 * mb->nt;
    if (e->nwc != np) {
        *topo = 1;
        return e->nwc < np ? mc_subset_dist(ew, e->nwc, (const double (*)[3])s_pw, np)
                           : mc_subset_dist((const double (*)[3])s_pw, np, ew, e->nwc);
    }
    double d2 = 0.0;
    for (int i = 0; i < np; i++) d2 = ms_max(d2, mc_d2(s_pw[i], ew[i]));
    return sqrt(d2);
}

/* The carry's planes for the present's queue, per queued moby (qi): the
 * count (-1 when the endpoint did not resolve the moby) and first plane in
 * s_vc_pl. */
static int* s_vc_np; static int* s_vc_p0; static int s_vc_cap, s_vc_cap0;
static openpete_snap_plane* s_vc_pl; static int s_vc_plcap, s_vc_n;
static ms_pid* s_vc_id; static int s_vc_idcap;   /* parallel to s_vc_pl: the source faces */
static uint8_t* s_vc_ref; static int s_vc_refcap; /* per queued moby: mc_refresh's verdict when it refused its planes, else 0 */
static uint8_t* s_vc_inj; static int s_vc_injcap; /* per queued moby: 1 when the calibration injected into its carried planes */

/* World units a stored moby's contact region extends past the world box of
 * its corners' camera-space box (ms_moby_cbox; mc_ent.rb). */
#define MC_CAND 64.0f
/* Per queued moby: 1 when a sector the endpoint's build did not find
 * visible reaches its endpoint contact region in the present's build
 * (mc_present_sectors). */
static uint8_t* s_pq_new; static int s_pq_cap;
static int s_pq_ok;
/* Cache of terrain_snap_face_verdict, keyed by sector, pass and face; a
 * slot holds while its stamp equals the build's (terrain_snap_build). */
#define MC_FV_BITS 12
static uint32_t s_fv_key[1 << MC_FV_BITS];
static uint8_t  s_fv_v[1 << MC_FV_BITS];
static unsigned s_fv_gen[1 << MC_FV_BITS], s_fv_cur;

/** @brief Index the entries of a terrain query result by the boxes their hit sets name: for each box, the entries in result order.
 * @param base The result's first entry.
 * @param stride Its entry size.
 * @param off The offset of an entry's uint64_t hit[2].
 * @param m The entry count.
 * @param nb The box count, at most 128.
 * @param s0 Receives each box's first slot in the returned index.
 * @param ns Receives each box's entry count.
 * @return The index (entry numbers), valid until the next call, or NULL on an allocation failure. */
static const int* ms_hit_index(const void* base, size_t stride, size_t off, int m, int nb, int* s0, int* ns) {
    static int* s_ix; static int s_ixcap;
    int total = 0;
    for (int k = 0; k < nb; k++) ns[k] = 0;
    for (int j = 0; j < m; j++) {
        const uint64_t* h = (const uint64_t*)((const char*)base + (size_t)j * stride + off);
        for (int w = 0; w < 2; w++)
            for (uint64_t r = h[w]; r; r &= r - 1) { ns[w * 64 + __builtin_ctzll(r)]++; total++; }
    }
    for (int k = 0, at = 0; k < nb; k++) { s0[k] = at; at += ns[k]; ns[k] = 0; }
    if (!ms_grow((void**)&s_ix, &s_ixcap, total + 1, sizeof *s_ix)) return NULL;
    for (int j = 0; j < m; j++) {
        const uint64_t* h = (const uint64_t*)((const char*)base + (size_t)j * stride + off);
        for (int w = 0; w < 2; w++)
            for (uint64_t r = h[w]; r; r &= r - 1) {
                const int k = w * 64 + __builtin_ctzll(r);
                s_ix[s0[k] + ns[k]++] = j;
            }
    }
    return s_ix;
}

/** @brief Record on one chunk of stored entries the faces their contact regions reach, with their verdicts (terrain_snap_faces).
 * @param t The table.
 * @param x The entries' indices in it, -1 for a moby not recorded.
 * @param rb Their contact regions.
 * @param nb The chunk's size, at most 128. */
static void mc_store_chunk(mc_tab* t, const int* x, const float (*rb)[6], int nb) {
    int s0[128], ns[128];
    const terrain_snap_face* ff = NULL;
    const int mf = terrain_snap_faces(rb, nb, &ff);
    const int* ix = mf >= 0 ? ms_hit_index(ff, sizeof *ff, offsetof(terrain_snap_face, hit), mf, nb, s0, ns) : NULL;
    if (!ix) return;
    for (int k = 0; k < nb; k++) {
        if (x[k] < 0 || !ms_grow((void**)&t->fc, &t->cap_fc, t->nfc + ns[k] + 1, sizeof *t->fc)) continue;
        mc_ent* e = &t->e[x[k]];
        e->f0 = t->nfc;
        for (int q = 0; q < ns[k]; q++) {
            const terrain_snap_face* f = &ff[ix[s0[k] + q]];
            const mc_fc c = { f->sec, f->pass, f->verd, f->face };
            t->fc[t->nfc++] = c;
        }
        e->nf = ns[k]; e->fc_ok = 1;
    }
}

/** @brief Record in a canonical store the build's visible sectors and, for each queued moby, its contact region (ms_moby_cbox widened by MC_CAND) and the faces of the visible sectors in it with their walk verdicts.
 * @param t The table the resolve stored into. */
static void mc_store_cands(mc_tab* t) {
    t->vis_ok = terrain_snap_visible(t->vis);
    if (!t->vis_ok) return;
    for (int g = 0; g < s_q_nm; g += 128) {
        const int nb = s_q_nm - g < 128 ? s_q_nm - g : 128;
        int x[128];
        float rb[128][6];
        for (int k = 0; k < nb; k++) {
            x[k] = mc_find(t, s_q_mob[g + k].mid);
            for (int a = 0; a < 3; a++) { rb[k][a] = 1e30f; rb[k][a + 3] = -1e30f; }
            if (x[k] >= 0 && (!s_rs_wb || !s_q_mob[g + k].nt)) x[k] = -1;
            if (x[k] < 0) continue;
            mc_ent* e = &t->e[x[k]];
            ms_cb_get(g + k, &s_q_mob[g + k], e->rb);
            for (int a = 0; a < 3; a++) { e->rb[a] -= MC_CAND; e->rb[a + 3] += MC_CAND; }
            memcpy(rb[k], e->rb, sizeof rb[k]);
        }
        mc_store_chunk(t, x, (const float (*)[6])rb, nb);
    }
}

/** @brief Mark each queued moby whose endpoint contact region a sector reaches that the present's build finds visible and the endpoint's did not (s_pq_new).
 * @param ep The endpoint.
 * @return 0 on an allocation failure, else 1. */
static int mc_present_sectors(const mc_tab* ep) {
    if (!ms_grow((void**)&s_pq_new, &s_pq_cap, s_q_nm, 1)) return 0;
    memset(s_pq_new, 0, (size_t)s_q_nm);
    s_pq_ok = ep->vis_ok;
    if (!s_pq_ok) return 1;
    for (int g = 0; g < s_q_nm; g += 128) {
        const int nb = s_q_nm - g < 128 ? s_q_nm - g : 128;
        float rb[128][6];
        for (int k = 0; k < nb; k++) {
            const int x = mc_find(ep, s_q_mob[g + k].mid);
            for (int a = 0; a < 3; a++) { rb[k][a] = 1e30f; rb[k][a + 3] = -1e30f; }
            if (x >= 0 && ep->e[x].fc_ok) memcpy(rb[k], ep->e[x].rb, sizeof rb[k]);
        }
        const terrain_snap_sector* ss = NULL;
        const int m = terrain_snap_sectors((const float (*)[6])rb, nb, ep->vis, &ss);
        if (m < 0) { s_pq_ok = 0; return 1; }
        for (int j = 0; j < m; j++)
            for (int w = 0; w < 2; w++)
                for (uint64_t r = ss[j].hit[w]; r; r &= r - 1) s_pq_new[g + w * 64 + __builtin_ctzll(r)] = 1;
    }
    return 1;
}

/** @brief A face's walk verdict in the present's build, through the per-build cache.
 * @param c The face.
 * @return Its verdict (terrain_snap_face_verdict). */
static int mc_verdict(const mc_fc* c) {
    const uint32_t key = ((uint32_t)(uint16_t)c->sec << 19) | ((uint32_t)c->pass << 16) | c->face;
    const unsigned h = (key * 2654435761u) >> (32 - MC_FV_BITS);
    if (s_fv_gen[h] == s_fv_cur && s_fv_key[h] == key) return s_fv_v[h];
    const int v = terrain_snap_face_verdict(c->sec, c->pass, c->face);
    s_fv_gen[h] = s_fv_cur; s_fv_key[h] = key; s_fv_v[h] = (uint8_t)v;
    return v;
}

/** @brief Whether the terrain faces a queued moby may contact can differ between the endpoint's walk and the present's.
 *
 * The endpoint recorded the moby's contact region and every face of its
 * visible sectors in it, drawn or not, with its walk verdict
 * (mc_store_cands). The present keeps the endpoint's face set when its
 * moby corners' box (ms_moby_cbox) stays inside that region, no sector the endpoint did not find
 * visible reaches the region (mc_present_sectors), and every recorded face
 * takes the same verdict under the present's camera
 * (terrain_snap_face_verdict: the walk's own gate chain, so a face turning
 * to or from the eye or crossing an LP admission distance is seen).
 * @param ep The endpoint.
 * @param e The moby's entry in it.
 * @param mb The queued moby.
 * @param k Its queue index.
 * @param same 1 when the present camera equals the endpoint's bit for bit: the region test is left to mc_refresh's exact corner comparison.
 * @return 1 when they can differ or a record is missing, else 0. */
static int mc_faces_changed(const mc_tab* ep, const mc_ent* e, const ms_moby* mb, int k, int same) {
    if (!e->fc_ok || !s_pq_ok || s_pq_new[k] || !s_rs_wb) return 1;
    float wb[6];
    if (!same) ms_moby_cbox(mb, wb);   /* a bit-equal camera compares the corners exactly (mc_refresh) */
    for (int a = 0; a < 3 && !same; a++)
        if (wb[a] < e->rb[a] || wb[a + 3] > e->rb[a + 3]) return 1;
    for (int q = 0; q < e->nf; q++)
        if (mc_verdict(&ep->fc[e->f0 + q]) != ep->fc[e->f0 + q].verd) return 1;
    return 0;
}

/** @brief Whether the carry's refresh test runs (OPENPETE_MOBY_SNAP_CARRY_REFRESH, default on; 0 turns it off for an A/B of the verify), read once.
 * @return 1 when on, else 0. */
static int mc_refresh_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_CARRY_REFRESH"); s = !(e && *e == '0'); }
    return s;
}

/** @brief Whether a moby's guest state signature (mc_moby_sig) is equal in the T-1 and T tables, adjacent ticks: its drawn pose is the same at either tick and at every phase between them.
 * @param mid The moby's identity.
 * @return 1 when equal, 0 when it differs, -1 when the tables are not adjacent or either lacks the moby or its signature. */
static int mc_still(uint32_t mid) {
    const mc_tab* lt = &s_mc[s_mc_cur];
    const mc_tab* pv = &s_mc[s_mc_cur ^ 1];
    if (!lt->valid || !pv->valid || pv->epoch + 1u != lt->epoch) return -1;
    const int a = mc_find(lt, mid), b = mc_find(pv, mid);
    if (a < 0 || b < 0 || !lt->e[a].sig_ok || !pv->e[b].sig_ok) return -1;
    return lt->e[a].sig == pv->e[b].sig;
}

/* ---- the corner comparison's input test -----------------------------------
 * Under a camera bit-equal to the endpoint's, mc_refresh compares a moby's
 * key-depth world corners with the stored ones (mc_corner_delta): each is
 * ms_moby_corner_at of the queued corner (screen x, y and key depth) under
 * the resolve's H, ofx, ofy, the inverse of the camera's linear part and
 * its position. The camera is the endpoint's (the caller's same), and the
 * inverse is computed from it by the same arithmetic, so when the queued
 * corners and H, ofx, ofy also equal the stored ones bit for bit every
 * recomputed corner equals its stored corner and the distance is 0; the
 * transforms are skipped (mc_corners_same). Any other input takes the
 * comparison. OPENPETE_SNAP_CORNER_EQ=0 always compares;
 * OPENPETE_SNAP_CORNER_EQ_CHECK=1 runs the input test and the comparison on
 * every such moby and counts the mobys the test passes whose comparison
 * refreshes them (ms_ce_print). */
static struct { long resolves, mobys, same, bad; } s_cec;   /* the check's totals */

/** @brief The corner input test's mode (OPENPETE_SNAP_CORNER_EQ, OPENPETE_SNAP_CORNER_EQ_CHECK), read once.
 * @return 0 off, 1 on (the default), 2 on and checked against the comparison. */
static int mc_ce_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_SNAP_CORNER_EQ");
        const char* c = getenv("OPENPETE_SNAP_CORNER_EQ_CHECK");
        v = (c && *c && *c != '0') ? 2 : !(e && *e == '0');
    }
    return v;
}

/** @brief Print the corner input test check's totals: resolves and mobys compared, mobys whose inputs were equal, and those of them the comparison refreshed. */
static void ms_ce_print(void) {
    fprintf(stderr, "[snap-corner-eq-check] resolves=%ld mobys=%ld inputs_equal=%ld mismatched=%ld\n",
            s_cec.resolves, s_cec.mobys, s_cec.same, s_cec.bad);
}

/** @brief Whether a queued moby's corners and the resolve's H, ofx, ofy equal, bit for bit, those its endpoint entry was stored from.
 * @param ep The endpoint.
 * @param e The moby's entry in it, its corner count equal to the moby's.
 * @param mb The queued moby.
 * @return 1 when they do, else 0. */
static int mc_corners_same(const mc_tab* ep, const mc_ent* e, const ms_moby* mb) {
    return memcmp(e->kv, s_rs_view, sizeof e->kv) == 0 &&
           memcmp(ep->rc[e->wc0], s_q_tri[mb->t0], (size_t)e->nwc * sizeof *ep->rc) == 0;
}

/** @brief Whether a queued moby's key-depth world corners differ from its endpoint entry's under a camera bit-equal to the endpoint's: the input test (mc_corners_same), else the comparison (mc_corner_delta).
 * @param ep The endpoint.
 * @param e The moby's entry in it, with corners and as many as the moby's.
 * @param mb The queued moby.
 * @return 1 when they differ or cannot be compared, else 0. */
static int mc_corners_moved(const mc_tab* ep, const mc_ent* e, const ms_moby* mb) {
    const int mode = mc_ce_mode();
    const int eq = mode && mc_corners_same(ep, e, mb);
    if (eq && mode == 1) return 0;
    int topo = 0;
    const double d = mc_corner_delta(ep, e, mb, &topo);
    const int moved = topo || d != 0.0;
    if (mode == 2) {
        static int s_atexit;
        if (!s_atexit) { s_atexit = 1; atexit(ms_ce_print); }
        s_cec.mobys++; s_cec.same += eq; s_cec.bad += eq && moved;
    }
    return moved;
}

/** @brief Whether a queued moby must resolve fresh instead of taking the endpoint's planes.
 *
 * It must when its guest state changed between the T-1 and T stores
 * (mc_still not 1): the present draws it at a phase between two poses, so
 * its contacts may differ from either endpoint's. It must when the terrain
 * faces it may contact can differ between the endpoint's walk and the
 * present's (mc_faces_changed). With the present camera
 * bit-equal to the endpoint's its projected corners are also compared
 * (mc_corner_delta), exactly: any difference, or a changed triangle count,
 * refreshes it, which covers pose inputs the signature does not read. Under
 * a moving camera a changed triangle count still refreshes it (the walk
 * drew another face set: a tier the present's own verdict picks, faces the
 * camera culls), since the endpoint's planes came from triangles the present
 * may not draw; the corners themselves are not compared: the S2/S3 simple kind
 * places a moby from its cull record's camera-relative offsets, held in
 * integer quarter-world units ((moby - camera) >> 2, mobydraw.s:617-630),
 * so an unchanged moby's corners step by whole grid units (1.0, 2.236) from
 * one camera to the next, more than the motions that change its contacts,
 * and no corner bound separates motion from that step.
 * @param ep The endpoint.
 * @param e The moby's entry in it.
 * @param mb The queued moby.
 * @param k Its queue index.
 * @param same 1 when the present camera equals the endpoint's bit for bit.
 * @return 0 when it takes the endpoint's planes, 2 when it resolves fresh on a face-verdict change, 1 when it resolves fresh for another reason. */
static int mc_refresh(const mc_tab* ep, const mc_ent* e, const ms_moby* mb, int k, int same) {
    if (!mc_refresh_on()) return 0;
    if (mc_still(mb->mid) != 1) return 1;
    if (mc_faces_changed(ep, e, mb, k, same)) return 2;
    if (!same) return s_rs_wb && e->wb_ok && e->nwc != 3 * mb->nt;
    if (e->nwc != 3 * mb->nt) return 1;   /* changed triangle count: mc_corner_delta reports topo or no corners */
    if (!s_rs_wb || !e->wb_ok) return 1;
    return mc_corners_moved(ep, e, mb);
}

/** @brief The verify's calibration injection (OPENPETE_MOBY_SNAP_CARRY_VERIFY_INJECT: 1 a stale plane, 2 a dropped plane), read once; 0 unless the verify runs.
 * @return 0, 1 or 2. */
static int mc_inject_mode(void) {
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("OPENPETE_MOBY_SNAP_CARRY_VERIFY_INJECT");
        s = mc_verify_mode() && e && (*e == '1' || *e == '2') ? *e - '0' : 0;
    }
    return s;
}

/** @brief Inject the calibration's fault (mc_inject_mode) into every eighth carried moby with planes, already appended at s_vc_p0[k]: a copy of its first plane 64 world units off, or its last plane dropped.
 * @param k The moby's queue index.
 * @return 0 on an allocation failure, else 1. */
static int mc_inject(int k) {
    static unsigned s_n;
    const int mode = mc_inject_mode();
    s_vc_inj[k] = 0;
    if (!mode || s_vc_np[k] <= 0 || s_n++ % 8u) return 1;
    s_vc_inj[k] = 1;
    if (mode == 2) { s_vc_np[k]--; s_vc_n--; return 1; }
    if (!ms_grow((void**)&s_vc_pl, &s_vc_plcap, s_vc_n + 1, sizeof *s_vc_pl) ||
        !ms_grow((void**)&s_vc_id, &s_vc_idcap, s_vc_n + 1, sizeof *s_vc_id)) return 0;
    s_vc_pl[s_vc_n] = s_vc_pl[s_vc_p0[k]];
    s_vc_pl[s_vc_n].d += 64.0f;
    s_vc_id[s_vc_n] = s_vc_id[s_vc_p0[k]];
    s_vc_n++; s_vc_np[k]++;
    return 1;
}

/** @brief Append one stored moby's planes for queue index k at s_vc_n: as resolved when the cameras are equal, else mapped into the present camera.
 *
 * A plane whose mapped normal is degenerate leaves the moby absent
 * (s_vc_np -1) and appends nothing.
 * @param ep The endpoint.
 * @param e The moby's entry in it.
 * @param same 1 when the planes are taken as resolved.
 * @param Q The present camera's L^-T (unread when same).
 * @param P The present camera's position (unread when same).
 * @param k The moby's queue index.
 * @return 0 on an allocation failure, else 1. */
static int mc_map_ent(const mc_tab* ep, const mc_ent* e, int same, const double Q[3][3], const double* P, int k) {
    if (!ms_grow((void**)&s_vc_pl, &s_vc_plcap, s_vc_n + e->np + 1, sizeof *s_vc_pl) ||
        !ms_grow((void**)&s_vc_id, &s_vc_idcap, s_vc_n + e->np + 1, sizeof *s_vc_id)) return 0;
    memcpy(&s_vc_id[s_vc_n], &ep->id[e->p0], (size_t)e->np * sizeof *s_vc_id);
    s_vc_p0[k] = s_vc_n;
    int ok = 1;
    for (int j = 0; j < e->np && ok; j++) {
        if (same) s_vc_pl[s_vc_n + j] = ep->c[e->p0 + j];
        else ok = mc_map_plane(&ep->w[e->p0 + j], &ep->c[e->p0 + j], Q, P, &s_vc_pl[s_vc_n + j]);
    }
    if (!ok) return 1;
    s_vc_np[k] = e->np;
    s_vc_n += e->np;
    return mc_inject(k);
}

/** @brief Map the endpoint's planes of every queued moby into the present camera (s_vc_np, s_vc_p0, s_vc_pl).
 *
 * A moby mc_refresh refuses stays absent (s_vc_np -1, s_vc_ref 1) and
 * resolves fresh. The planes are taken as resolved when the camera equals the endpoint's
 * and the verify mode is not 3; verify mode 2 maps under the endpoint's own
 * camera (the identity).
 * @param ep The endpoint, or NULL (every moby absent).
 * @param L The present walk camera's linear part.
 * @param P Its position.
 * @return 0 on an allocation failure, else 1. */
static int mc_map(const mc_tab* ep, const double L[3][3], const double P[3]) {
    if (!ms_grow((void**)&s_vc_np, &s_vc_cap, s_q_nm, sizeof *s_vc_np) ||
        !ms_grow((void**)&s_vc_p0, &s_vc_cap0, s_q_nm, sizeof *s_vc_p0) ||
        !ms_grow((void**)&s_vc_ref, &s_vc_refcap, s_q_nm, 1) ||
        !ms_grow((void**)&s_vc_inj, &s_vc_injcap, s_q_nm, 1)) return 0;
    s_vc_n = 0;
    const int vm = mc_verify_mode();
    const double (*Lm)[3] = vm == 2 && ep ? ep->L : L;
    const double* Pm = vm == 2 && ep ? ep->P : P;
    const int same = ep && !ep->cam_mixed && vm != 3 &&
                     !memcmp(ep->L, Lm, sizeof ep->L) && !memcmp(ep->P, Pm, 3 * sizeof *Pm);
    const int same_cam = ep && !ep->cam_mixed && !memcmp(ep->L, L, sizeof ep->L) && !memcmp(ep->P, P, 3 * sizeof *P);
    double Q[3][3];
    const int inv = ep && mc_inv_t(Lm, Q);
    s_fv_cur = terrain_snap_build();
    s_cec.resolves += same_cam && mc_ce_mode() == 2;
    if (ep && mc_refresh_on() && !mc_present_sectors(ep)) return 0;
    for (int k = 0; k < s_q_nm; k++) {
        const int x = inv || same ? mc_find(ep, s_q_mob[k].mid) : -1;
        s_vc_np[k] = -1; s_vc_ref[k] = 0; s_vc_inj[k] = 0;
        if (x < 0) continue;
        const int ref = mc_refresh(ep, &ep->e[x], &s_q_mob[k], k, same_cam);
        if (ref) { s_vc_ref[k] = (uint8_t)ref; s_pc.refreshed++; s_pc.fsref += ref == 2; continue; }
        if (!mc_map_ent(ep, &ep->e[x], same, Q, Pm, k)) return 0;
    }
    return 1;
}

/* ---- carry verify (OPENPETE_MOBY_SNAP_CARRY_VERIFY) ----------------------
 * Every sub-tick resolve also runs the fresh contact pass over the whole
 * queue and compares, per queued moby, the carried planes against the fresh
 * ones. Planes match when their normals agree to within twice the merge
 * cosine deficit and their offsets to within twice the merge distance (each
 * plane is its group's first contact face, within the merge tolerance of
 * every member), up to the normal's orientation. Classes: a moby the
 * endpoint did not resolve (absent, resolved fresh under the carry); one
 * the fresh pass did not resolve (unresolved); carry-only and fresh-only
 * mobys (planes on one side only) and planes (unmatched); matched planes
 * whose side, slab or double-sided flag differ; table refusals (overflow).
 * A mismatching moby takes the first class that holds: moved, when a corner
 * of the triangles its faces project (key depth, world space) lies more
 * than MC_MOVED world units from the same corner at the endpoint, or its
 * triangle count changed (a moby moving or animating across faces within
 * the tick); grazing, when every unmatched plane its corners straddle by
 * less than MC_GRAZE world units (a grazing contact, which the exact
 * triangle test decides differently under another camera's float
 * projection); tangent, when every unmatched plane not grazing is the
 * plane of a present candidate terrain triangle whose intersection margin
 * with the moby's triangles (mc_pair_margin) lies within MC_TANGENT of
 * zero, the largest such margin when several share the plane (an edge of
 * one triangle touching the other: the exact test's verdict there follows
 * the float projection, not the geometry); split, when every unmatched
 * plane not grazing or tangent matches, under the comparison's tolerance, a
 * plane of the other leg already partnered (two faces within the merge
 * tolerance of each other that one leg merged into one plane and the other
 * kept apart: ms_plane_add compares offsets at the eye, so the merge follows
 * the camera); vanim, when every mismatching plane's source sector moves
 * within the tick by its vertex animation (a plane of the moby, carried or
 * fresh, from that sector carries terrain_snap_tri.vanim), so the fresh
 * plane lies at the sub-tick lerp of the sector's vertex banks and the
 * carried one at the endpoint's banks; static, the rest.
 * Engagement: resolves and presents compared, presents at alpha != 0,
 * mobys and planes compared, and the present-to-endpoint camera delta.
 * Calibration (OPENPETE_MOBY_SNAP_CARRY_VERIFY_INJECT): 1 appends to every
 * eighth carried moby with planes a copy of its first plane 64 world units
 * off (a face no present triangle lies in), 2 drops its last carried plane
 * (a contact face missing from the carried set); the census counts the
 * injected mobys' classes (inj). */
#define MC_MOVED 0.5
#define MC_GRAZE 2.0
/* The tangent class's margin bound, world units: a still moby's contact
 * margin against an unchanged terrain triangle varies by up to 0.005
 * between the resolves of one tick (float screen corners unprojected under
 * each camera), and no margin that decides a contact on one leg only lies
 * between 0.005 and 0.04. */
#define MC_TANGENT 0.01
#define MC_TMAR_N 6   /* unmatched-plane margin census buckets: none, then |m| below 0.001, 0.01, 0.1, 1, the rest */
#define MC_CMOV_N 7   /* corner-displacement census buckets (s_cmov_hi) */
enum { MC_CLS_MOVED, MC_CLS_GRAZING, MC_CLS_STATIC, MC_CLS_VANIM, MC_CLS_TANGENT, MC_CLS_SPLIT, MC_NCLS };
static const char* const s_mc_cls_name[MC_NCLS] = { "moved", "grazing", "static", "vanim", "tangent", "split" };
static int* s_vf_np; static int* s_vf_p0; static int s_vf_cap, s_vf_cap0;
static openpete_snap_plane* s_vf_pl; static int s_vf_plcap, s_vf_n;
static ms_pid* s_vf_id; static int s_vf_idcap;       /* parallel to s_vf_pl: the source faces */
static uint32_t* s_vq_mid; static int s_vq_cap, s_vq_n;
static float (*s_vq_wb)[6]; static int s_vq_wbcap;   /* per queue index: the present's world box */
static int s_vf_on;
/* The present's near contacts (mc_vf_near): per queue index, the planes of
 * the candidate terrain triangles whose margin against the moby exceeds
 * -MC_TANGENT, with that margin. */
typedef struct { float n[3], d, m; } mc_near;
static mc_near* s_vt; static int s_vt_cap, s_vt_n;
static int* s_vt_p0; static int* s_vt_np; static int s_vt_cap0, s_vt_cap1;
typedef struct {
    long resolves, presents, presents_anz, mobys, planes, absent, unresolved;
    long carry_only_m, fresh_only_m, carry_only_p, fresh_only_p, side, slab, dside;
    long mism_mobys, mism_cls[MC_NCLS], ovf, cam_moved;
    long moved_topo;      /* moved mobys whose triangle count changed */
    long refreshed;       /* mobys mc_refresh resolved fresh (not compared) */
    long refreshed_fs;    /* of those, the ones resolved fresh on a face-verdict change */
    long moved_box;       /* mismatching mobys whose world box moved over MC_MOVED */
    long static_box;      /* mismatching mobys the box delta would class static (grazing and
                           * vanim not holding, box at most MC_MOVED) */
    long cmov[MC_CMOV_N], cmov_m[MC_CMOV_N];  /* corner displacement buckets (s_cmov_hi): every compared moby, the
                               * mismatching ones */
    long tmar[MC_TMAR_N]; /* unmatched planes by their present near-contact margin (mc_vf_tangent) */
    long inj, inj_clean;  /* calibration-injected mobys compared; of those, the ones that matched */
    long graze_loose;     /* grazing mobys a mismatching plane of which is neither tangent nor split */
    long inj_cls[MC_NCLS];
    double dp_sum, dp_max, dl_max;
    unsigned last_epoch;
} mc_vstat;
static mc_vstat s_vs = { .last_epoch = ~0u };

/** @brief Record one moby's fresh planes for the verify comparison.
 * @param qi The moby's queue index.
 * @param out Its planes.
 * @param id Their source faces.
 * @param np Their count. */
static void mc_vf_put(int qi, const openpete_snap_plane* out, const ms_pid* id, int np) {
    if (qi < 0 || qi >= s_vq_n ||
        !ms_grow((void**)&s_vf_pl, &s_vf_plcap, s_vf_n + np + 1, sizeof *s_vf_pl) ||
        !ms_grow((void**)&s_vf_id, &s_vf_idcap, s_vf_n + np + 1, sizeof *s_vf_id)) return;
    s_vf_np[qi] = np; s_vf_p0[qi] = s_vf_n;
    memcpy(&s_vf_pl[s_vf_n], out, (size_t)np * sizeof *out);
    memcpy(&s_vf_id[s_vf_n], id, (size_t)np * sizeof *id);
    s_vf_n += np;
}

/** @brief Hand a resolved moby's planes to the carry: a canonical resolve stores them, a verified sub-tick resolve records them for the comparison.
 * @param mb The moby.
 * @param out Its planes.
 * @param id Their source faces.
 * @param np Their count, 0 for no contact.
 * @param wb The moby's world box for the store when already computed, else NULL (mc_store).
 * @param kc Its key-depth world corners for the store when already computed, else NULL. */
static void ms_result(const ms_moby* mb, const openpete_snap_plane* out, const ms_pid* id, int np,
                      const float* wb, const double (*kc)[3]) {
    if (s_store) mc_store(s_store, mb, out, id, np, wb, kc);
    if (s_vf_on) mc_vf_put(mb->qi, out, id, np);
}

/** @brief Start the verify comparison of this resolve: every moby unresolved, the queue's identities kept by queue index.
 * @return 0 on an allocation failure, else 1. */
static int mc_vf_begin(void) {
    if (!ms_grow((void**)&s_vf_np, &s_vf_cap, s_q_nm, sizeof *s_vf_np) ||
        !ms_grow((void**)&s_vf_p0, &s_vf_cap0, s_q_nm, sizeof *s_vf_p0) ||
        !ms_grow((void**)&s_vq_mid, &s_vq_cap, s_q_nm, sizeof *s_vq_mid) ||
        !ms_grow((void**)&s_vq_wb, &s_vq_wbcap, s_q_nm, sizeof *s_vq_wb) ||
        !ms_grow((void**)&s_vt_p0, &s_vt_cap0, s_q_nm, sizeof *s_vt_p0) ||
        !ms_grow((void**)&s_vt_np, &s_vt_cap1, s_q_nm, sizeof *s_vt_np)) return 0;
    s_vq_n = s_q_nm; s_vf_n = 0; s_vt_n = 0;
    for (int k = 0; k < s_q_nm; k++) {
        s_vf_np[k] = -1; s_vq_mid[k] = s_q_mob[k].mid;
        s_vt_np[k] = 0; s_vt_p0[k] = 0;
        if (s_rs_wb) ms_moby_wbox(&s_q_mob[k], s_vq_wb[k]);
    }
    return 1;
}

/** @brief Print the verify census, cumulative over the run. */
static void mc_vf_print(void) {
    OP_ROLE_ASSERT(sim);
    const mc_vstat* v = &s_vs;
    fprintf(stderr, "[snap-carry-verify] mode=%d carry=%d resolves=%ld presents=%ld presents_anz=%ld mobys=%ld "
                    "planes=%ld mismatch_mobys=%ld mismatch_moved=%ld mismatch_grazing=%ld mismatch_static=%ld mismatch_vanim=%ld "
                    "moved_topo=%ld moved_box=%ld static_box=%ld carry_only_mobys=%ld fresh_only_mobys=%ld carry_only_planes=%ld "
                    "fresh_only_planes=%ld side_flips=%ld slab_diff=%ld dside_diff=%ld absent=%ld unresolved=%ld "
                    "overflow=%ld cam_moved=%ld cam_dp_mean=%.3f cam_dp_max=%.3f cam_dl_max=%.6f dup=%ld full=%ld "
                    "rebuilds=%ld refreshed=%ld cmov=%ld/%ld/%ld/%ld/%ld/%ld/%ld cmov_mism=%ld/%ld/%ld/%ld/%ld/%ld/%ld "
                    "refreshed_fs=%ld mismatch_tangent=%ld mismatch_split=%ld tmar=%ld/%ld/%ld/%ld/%ld/%ld "
                    "inj=%ld inj_clean=%ld inj_cls=%ld/%ld/%ld/%ld/%ld/%ld graze_loose=%ld\n",
            mc_verify_mode(), mc_carry_on(), v->resolves, v->presents, v->presents_anz, v->mobys, v->planes,
            v->mism_mobys, v->mism_cls[MC_CLS_MOVED], v->mism_cls[MC_CLS_GRAZING], v->mism_cls[MC_CLS_STATIC],
            v->mism_cls[MC_CLS_VANIM], v->moved_topo, v->moved_box, v->static_box, v->carry_only_m, v->fresh_only_m,
            v->carry_only_p, v->fresh_only_p, v->side,
            v->slab, v->dside, v->absent, v->unresolved, v->ovf, v->cam_moved,
            v->resolves ? v->dp_sum / (double)v->resolves : 0.0, v->dp_max, v->dl_max,
            s_mc[0].dup + s_mc[1].dup, s_mc[0].full + s_mc[1].full, s_mc_rebuilds, v->refreshed,
            v->cmov[0], v->cmov[1], v->cmov[2], v->cmov[3], v->cmov[4], v->cmov[5], v->cmov[6],
            v->cmov_m[0], v->cmov_m[1], v->cmov_m[2], v->cmov_m[3], v->cmov_m[4], v->cmov_m[5], v->cmov_m[6],
            v->refreshed_fs, v->mism_cls[MC_CLS_TANGENT], v->mism_cls[MC_CLS_SPLIT],
            v->tmar[0], v->tmar[1], v->tmar[2], v->tmar[3], v->tmar[4], v->tmar[5],
            v->inj, v->inj_clean, v->inj_cls[0], v->inj_cls[1], v->inj_cls[2], v->inj_cls[3], v->inj_cls[4],
            v->inj_cls[5], v->graze_loose);
}

/** @brief Whether two planes agree under the verify tolerance, up to the normal's orientation: normals within twice the merge cosine deficit, offsets within twice the merge distance.
 * @param n First normal.
 * @param d First offset.
 * @param m Second normal.
 * @param e Second offset.
 * @param sg Receives +1 or -1, the second normal's orientation against the first.
 * @return 1 when they agree, else 0. */
static int mc_plane_eq(const float n[3], double d, const float m[3], double e, double* sg) {
    const double cs = (double)n[0] * m[0] + (double)n[1] * m[1] + (double)n[2] * m[2];
    const double s = cs < 0.0 ? -1.0 : 1.0;
    *sg = s;
    return fabs(cs) > 1.0 - 2.0 * MS_MERGE_DN && fabs(d - s * e) < 2.0 * MS_MERGE_DD;
}

/** @brief The signed margin by which segment a-b meets triangle T, world units.
 *
 * A segment that crosses T's plane: the smaller of its end points' distances
 * to the plane and the crossing point's distances inside T's edges (negative
 * outside an edge). One that does not: minus its nearer end point's
 * distance to the plane, or that point's projection's distance outside an
 * edge when larger. Positive exactly where ms_seg_tri holds, up to its
 * tolerance; its magnitude is how far a corner must move to change that.
 * @param a Segment start.
 * @param b Segment end.
 * @param T The triangle.
 * @return The margin. */
static double mc_seg_margin(ms_v3 a, ms_v3 b, const ms_tri* T) {
    const double da = v3dot(T->n, a) - T->d, db = v3dot(T->n, b) - T->d;
    const int cross = da * db <= 0.0 && da != db;
    double m = cross ? ms_min(fabs(da), fabs(db)) : -ms_min(fabs(da), fabs(db));
    ms_v3 x;
    if (cross) {
        const double t = da / (da - db);
        x.x = a.x + (b.x - a.x) * t; x.y = a.y + (b.y - a.y) * t; x.z = a.z + (b.z - a.z) * t;
    } else {
        const ms_v3 q = fabs(da) < fabs(db) ? a : b;
        const double dq = fabs(da) < fabs(db) ? da : db;
        x.x = q.x - T->n.x * dq; x.y = q.y - T->n.y * dq; x.z = q.z - T->n.z * dq;
    }
    for (int i = 0; i < 3; i++) {
        const ms_v3 e = v3sub(T->p[(i + 1) % 3], T->p[i]), w = v3sub(x, T->p[i]);
        const double l = v3len(e);
        if (l > 0.0) m = ms_min(m, v3dot(T->n, v3cross(e, w)) / l);
    }
    return m;
}

/** @brief The signed intersection margin of two triangles: the largest margin (mc_seg_margin) of an edge of either against the other, positive where ms_tri_tri holds.
 * @param A First triangle.
 * @param B Second triangle.
 * @return The margin, world units. */
static double mc_pair_margin(const ms_tri* A, const ms_tri* B) {
    double m = -1e30;
    for (int i = 0; i < 3; i++) {
        m = ms_max(m, mc_seg_margin(A->p[i], A->p[(i + 1) % 3], B));
        m = ms_max(m, mc_seg_margin(B->p[i], B->p[(i + 1) % 3], A));
    }
    return m;
}

/** @brief Whether two camera-space bounds lie more than g apart on any axis.
 * @param a First triangle.
 * @param b Second triangle.
 * @param g The gap.
 * @return 1 when they do. */
static int mc_box_apart(const ms_tri* a, const ms_tri* b, double g) {
    return a->hi.x < b->lo.x - g || a->lo.x > b->hi.x + g || a->hi.y < b->lo.y - g ||
           a->lo.y > b->hi.y + g || a->hi.z < b->lo.z - g || a->lo.z > b->hi.z + g;
}

/** @brief Record, for the verify, every candidate terrain triangle of a moby's fresh resolve whose intersection margin with the moby's near triangles exceeds -MC_TANGENT (s_vt), with its plane and that margin.
 * @param W The scratch the moby resolved in.
 * @param mb The moby.
 * @param nc Its usable candidates (W->cj).
 * @param nl Its near triangles (W->ml). */
static void mc_vf_near(const ms_ws* W, const ms_moby* mb, int nc, int nl) {
    const int qi = mb->qi;
    if (!s_vf_on || qi < 0 || qi >= s_vq_n) return;
    s_vt_p0[qi] = s_vt_n; s_vt_np[qi] = 0;
    for (int q = 0; q < nc; q++) {
        const ms_tri* F = &s_tt[W->cj[q]];
        double m = -1e30;
        for (int l = 0; l < nl; l++)
            if (!mc_box_apart(&W->mt[W->ml[l]], F, MC_TANGENT)) m = ms_max(m, mc_pair_margin(&W->mt[W->ml[l]], F));
        if (m <= -MC_TANGENT || !ms_grow((void**)&s_vt, &s_vt_cap, s_vt_n + 1, sizeof *s_vt)) continue;
        mc_near* r = &s_vt[s_vt_n++];
        r->n[0] = (float)F->n.x; r->n[1] = (float)F->n.y; r->n[2] = (float)F->n.z;
        r->d = (float)F->d; r->m = (float)m;
        s_vt_np[qi]++;
    }
}

/** @brief Whether an unmatched plane of a moby is a tangent contact: present candidate triangles lie in it (mc_plane_eq) and the largest of their margins is within MC_TANGENT of zero; counts the margin in the census (tmar).
 * @param qi The moby's queue index.
 * @param pl The plane.
 * @return 1 when tangent, else 0. */
static int mc_vf_tangent(int qi, const openpete_snap_plane* pl) {
    double m = -1e30, sg;
    int any = 0;
    for (int r = s_vt_p0[qi]; r < s_vt_p0[qi] + s_vt_np[qi]; r++)
        if (mc_plane_eq(pl->n, pl->d, s_vt[r].n, s_vt[r].d, &sg)) { any = 1; m = ms_max(m, s_vt[r].m); }
    const double a = fabs(m);
    s_vs.tmar[!any ? 0 : a < 0.001 ? 1 : a < 0.01 ? 2 : a < 0.1 ? 3 : a < 1.0 ? 4 : 5]++;
    return any && a < MC_TANGENT;
}

/** @brief Whether a plane agrees (mc_plane_eq) with any plane of the other leg.
 * @param pl The plane.
 * @param o The other leg's planes.
 * @param no Their count.
 * @return 1 when one agrees, else 0. */
static int mc_vf_has_eq(const openpete_snap_plane* pl, const openpete_snap_plane* o, int no) {
    double sg;
    for (int j = 0; j < no; j++)
        if (mc_plane_eq(pl->n, pl->d, o[j].n, o[j].d, &sg)) return 1;
    return 0;
}

/** @brief Find the fresh plane that matches a carried one under the verify tolerance.
 * @param c The carried plane.
 * @param f The fresh planes.
 * @param nf Their count.
 * @param used Per fresh plane, 1 once matched; updated.
 * @param sg Receives +1 or -1, the fresh normal's orientation against the carried one.
 * @return The fresh plane's index, or -1. */
static int mc_vf_match(const openpete_snap_plane* c, const openpete_snap_plane* f, int nf, uint8_t* used, double* sg) {
    for (int j = 0; j < nf; j++) {
        if (used[j] || !mc_plane_eq(c->n, c->d, f[j].n, f[j].d, sg)) continue;
        used[j] = 1;
        return j;
    }
    return -1;
}

/** @brief The endpoint's stored entry of a queued moby while moby world geometry is kept, or NULL.
 * @param qi The moby's queue index.
 * @return The entry, or NULL. */
static const mc_ent* mc_vf_ent(int qi) {
    const int x = s_rs_wb && s_rs_ep ? mc_find(s_rs_ep, s_vq_mid[qi]) : -1;
    return x < 0 || !s_rs_ep->e[x].wb_ok ? NULL : &s_rs_ep->e[x];
}

/** @brief How far a queued moby's world box moved from the endpoint's, the largest of the six bound deltas.
 * @param qi The moby's queue index.
 * @return The delta in world units, or -1 when either box is missing. */
static double mc_vf_box_moved(int qi) {
    const mc_ent* e = mc_vf_ent(qi);
    if (!e) return -1.0;
    double d = 0.0;
    for (int i = 0; i < 6; i++) d = ms_max(d, fabs((double)e->wb[i] - s_vq_wb[qi][i]));
    return d;
}


/** @brief How far a queued moby's projected triangle corners moved from the endpoint's (mc_corner_delta).
 * @param qi The moby's queue index.
 * @param topo Set to 1 when the triangle counts differ, else 0.
 * @return The distance in world units, or -1 when the endpoint kept no corners. */
static double mc_vf_moved(int qi, int* topo) {
    const mc_ent* e = mc_vf_ent(qi);
    *topo = 0;
    return e ? mc_corner_delta(s_rs_ep, e, &s_q_mob[qi], topo) : -1.0;
}

/* Bucket bounds of the corner-displacement census (mc_vstat.cmov): every
 * compared moby with corners on both legs, matched or not. */
static const double s_cmov_hi[MC_CMOV_N - 1] = { 0.001, 0.01, 0.1, 0.5, 2.0, 16.0 };

/** @brief Count one compared moby's corner displacement into its census bucket.
 * @param mv The displacement (mc_vf_moved), ignored when negative.
 * @param h The buckets. */
static void mc_vf_cmov_add(double mv, long* h) {
    if (mv < 0.0) return;
    int b = 0;
    while (b < MC_CMOV_N - 1 && mv >= s_cmov_hi[b]) b++;
    h[b]++;
}

/** @brief Whether a moby has a plane, carried or fresh, from a given sector that carries the vertex-animation mark.
 * @param qi The moby's queue index.
 * @param sec The sector index.
 * @return 1 when it has, else 0. */
static int mc_vf_vsec(int qi, int sec) {
    for (int j = 0; j < s_vc_np[qi]; j++) {
        const ms_pid* p = &s_vc_id[s_vc_p0[qi] + j];
        if (p->vanim && p->sec == sec) return 1;
    }
    for (int j = 0; j < s_vf_np[qi]; j++) {
        const ms_pid* p = &s_vf_id[s_vf_p0[qi] + j];
        if (p->vanim && p->sec == sec) return 1;
    }
    return 0;
}

/** @brief Read a moby record's class halfword and position words from guest RAM for a detail line.
 * @param mid The moby's scene identity (record word index plus one).
 * @param pos Receives the position (0 when the guest is unknown).
 * @return The class, or -1 when the guest is unknown. */
static int mc_vf_moby_class(uint32_t mid, int32_t pos[3]) {
    pos[0] = pos[1] = pos[2] = 0;
    if (!s_vf_cpu || !mid) return -1;
    const uint32_t a = 0x80000000u | ((mid - 1u) << 2);
    for (int i = 0; i < 3; i++) pos[i] = (int32_t)s_vf_cpu->read_word(a + game_layout.moby.pos_off + 4u * (uint32_t)i);
    return s_vf_cpu->read_half(a + game_layout.moby.class_off);
}

static double s_vf_dp, s_vf_dl;   /* the compared resolve's present-to-endpoint camera position and matrix deltas */

/* The probe's per-moby face pairing (mc_vf_fk_pair): triangles at the same
 * index with the same face key, triangles paired by face key, the present's
 * triangles with no partner, the corner distance over the key pairs and the
 * pair holding it. */
typedef struct { int same, paired, unpaired; double mv; int worst_q, worst_k; } mc_fkp;

/** @brief Pair a queued moby's triangles with the endpoint's by face key and measure the corner distance over the pairs.
 *
 * Each present triangle takes the first unused stored triangle with its face
 * key; the walk emits a moby's faces in stream order, so equal keys pair the
 * same face. Read only while the probe is on (moby_snap_probe_on).
 * @param e The moby's endpoint entry.
 * @param mb The queued moby.
 * @param o Receives the pairing. */
static void mc_vf_fk_pair(const mc_ent* e, const ms_moby* mb, mc_fkp* o) {
    static uint8_t* s_used; static int s_ucap;
    memset(o, 0, sizeof *o);
    o->worst_q = o->worst_k = -1;
    const int ns = e->nwc / 3;
    if (!s_rs_ep->fk || !ms_grow((void**)&s_used, &s_ucap, ns + 1, 1)) return;
    memset(s_used, 0, (size_t)ns + 1u);
    const uint32_t* sfk = &s_rs_ep->fk[e->wc0];
    const double (*ew)[3] = (const double (*)[3])&s_rs_ep->wc[e->wc0];
    for (int q = 0; q < mb->nt; q++) {
        const uint32_t f = s_q_fk[mb->t0 + q];
        if (q < ns && sfk[3 * q] == f) o->same++;
        int m = -1;
        for (int j = 0; j < ns && m < 0; j++) if (!s_used[j] && sfk[3 * j] == f) m = j;
        if (m < 0) { o->unpaired++; continue; }
        s_used[m] = 1; o->paired++;
        for (int k = 0; k < 3; k++) {
            double w[3];
            ms_moby_kcorner(mb, q, k, w);
            const double d = sqrt(mc_d2(w, ew[3 * m + k]));
            if (d > o->mv) { o->mv = d; o->worst_q = q; o->worst_k = k; }
        }
    }
}

/** @brief Print one compared or refreshed moby's corner displacement under OPENPETE_MOBY_SNAP_CARRY_VERIFY_CMOV=1, the first 200000 of the run.
 *
 * The line carries the still verdict (mc_still), the camera delta, and the
 * probe's face-key pairing (mc_vf_fk_pair), record path and key offset on
 * both legs.
 * @param qi The moby's queue index.
 * @param mv Its corner displacement (mc_vf_moved).
 * @param topo 1 when its triangle count changed. */
static void mc_vf_cmov_line(int qi, double mv, int topo) {
    static int s_n;
    if (!moby_snap_probe_on() || s_n >= 200000) return;
    s_n++;
    int32_t pos[3];
    const int mcls = mc_vf_moby_class(s_vq_mid[qi], pos);
    const mc_ent* e = mc_vf_ent(qi);
    mc_fkp fp = { 0, 0, 0, -1.0, -1, -1 };
    if (e) mc_vf_fk_pair(e, &s_q_mob[qi], &fp);
    fprintf(stderr, "[snap-carry-cmov] key=%u alpha=%.3f mid=%u mclass=%d nt=%d topo=%d moved=%.6f box=%.4f np=%d/%d "
                    "still=%d dp=%.3f dl=%.6f ref=%d ent_nt=%d fk_same=%d fk_pair=%d fk_un=%d mvfk=%.6f wq=%d wk=%d "
                    "path=%d/%d off=%.3f/%.3f kr=%.6f/%.6f z=%.0f wc=%.1f,%.1f,%.1f\n",
            openpete_feed_key_now(), s_rex_alpha, s_vq_mid[qi], mcls, s_q_mob[qi].nt, topo, mv,
            mc_vf_box_moved(qi), s_vc_np[qi], s_vf_np[qi], mc_still(s_vq_mid[qi]), s_vf_dp, s_vf_dl, s_vc_ref[qi],
            e ? e->nwc / 3 : -1, fp.same, fp.paired, fp.unpaired, fp.mv, fp.worst_q, fp.worst_k,
            s_q_mob[qi].path, e ? e->path : -1, (double)s_q_mob[qi].off, e ? (double)e->off : 0.0,
            (double)s_q_mob[qi].kr, e ? (double)e->kr : 0.0, (double)s_q_mob[qi].b[4],
            fp.worst_q >= 0 ? (double)s_q_tri[s_q_mob[qi].t0 + fp.worst_q][fp.worst_k][0] : 0.0,
            fp.worst_q >= 0 ? (double)s_q_tri[s_q_mob[qi].t0 + fp.worst_q][fp.worst_k][1] : 0.0,
            fp.worst_q >= 0 ? (double)s_q_tri[s_q_mob[qi].t0 + fp.worst_q][fp.worst_k][2] : 0.0);
}

/** @brief Print one plane of a mismatch detail: its geometry, source face, mark, partner, and the eye's signed distance to it under the endpoint camera (eye_ep; the present's is -d, so a sign change means the eye crossed the plane).
 * @param leg "carry" or "fresh".
 * @param j The plane's index on its leg.
 * @param p The plane.
 * @param id Its source face.
 * @param m Its partner on the other leg, or -1 when unmatched. */
static void mc_vf_detail_plane(const char* leg, int j, const openpete_snap_plane* p, const ms_pid* id, int m) {
    double de = 0.0;   /* the eye's signed distance under the endpoint camera (present: -d) */
    if (s_rs_ep) {
        double nw[3], dw = p->d;
        for (int i = 0; i < 3; i++) { nw[i] = s_rs_L[0][i] * p->n[0] + s_rs_L[1][i] * p->n[1] + s_rs_L[2][i] * p->n[2]; dw += nw[i] * s_rs_P[i]; }
        de = nw[0] * s_rs_ep->P[0] + nw[1] * s_rs_ep->P[1] + nw[2] * s_rs_ep->P[2] - dw;
    }
    fprintf(stderr, "  %s[%d] n=%.6f,%.6f,%.6f d=%.3f side=%.0f slab=%.0f dside=%d sec=%d face=%d tier=%d vanim=%d match=%d "
                    "eye_ep=%.3f\n",
            leg, j, p->n[0], p->n[1], p->n[2], p->d, p->side, p->slab, p->dside, id->sec, id->face, id->tier,
            id->vanim, m, de);
}

/** @brief Print one mismatching moby: a header line for each of the first 2048 of the run, and its carried and fresh planes with their source faces and partners for the first 256 not moved or grazing (every class outside mode 1).
 * @param qi The moby's queue index.
 * @param why The mismatch kind (planes, carry_only_moby, fresh_only_moby).
 * @param cls The class decision (MC_CLS_*).
 * @param moved Its corner displacement (mc_vf_moved).
 * @param box Its world box delta (mc_vf_box_moved). */
static void mc_vf_detail(int qi, const char* why, int cls, double moved, double box) {
    static int s_nh, s_np;
    if (s_nh >= 2048) return;
    s_nh++;
    int32_t pos[3];
    const int mcls = mc_vf_moby_class(s_vq_mid[qi], pos);
    const int nc = s_vc_np[qi], nf = s_vf_np[qi];
    fprintf(stderr, "[snap-carry-mismatch] key=%u alpha=%.3f mid=%u mclass=%d pos=%d,%d,%d class=%s why=%s "
                    "carry_np=%d fresh_np=%d nt=%d moved=%.2f box=%.2f inj=%d\n",
            openpete_feed_key_now(), s_rex_alpha, s_vq_mid[qi], mcls, pos[0], pos[1], pos[2], s_mc_cls_name[cls],
            why, nc, nf, s_q_mob[qi].nt, moved, box, s_vc_inj[qi]);
    const int planes = mc_verify_mode() != 1 || (cls != MC_CLS_MOVED && cls != MC_CLS_GRAZING);
    if (!planes || s_np >= 256) return;
    s_np++;
    uint8_t used[MS_MAX_PLANES] = { 0 };
    int fm[MS_MAX_PLANES];
    for (int j = 0; j < nf && j < MS_MAX_PLANES; j++) fm[j] = -1;
    const openpete_snap_plane* f = &s_vf_pl[s_vf_p0[qi]];
    for (int j = 0; j < nc; j++) {
        double sg = 1.0;
        const int m = nf > 0 ? mc_vf_match(&s_vc_pl[s_vc_p0[qi] + j], f, nf, used, &sg) : -1;
        if (m >= 0) fm[m] = j;
        mc_vf_detail_plane("carry", j, &s_vc_pl[s_vc_p0[qi] + j], &s_vc_id[s_vc_p0[qi] + j], m);
    }
    for (int j = 0; j < nf; j++) mc_vf_detail_plane("fresh", j, &f[j], &s_vf_id[s_vf_p0[qi] + j], fm[j]);
}

/** @brief How deep a queued moby's triangle corners (key depth, present camera) straddle a plane: the smaller of the deepest corner on each side.
 *
 * Negative when every corner lies on one side: minus the nearest corner's
 * distance.
 * @param mb The moby.
 * @param pl The plane.
 * @return The straddle depth, world units. */
static double mc_straddle(const ms_moby* mb, const openpete_snap_plane* pl) {
    const double H = s_rs_view[0], ofx = s_rs_view[1], ofy = s_rs_view[2];
    double hi = -1e30, lo = 1e30;
    for (int t = 0; t < mb->nt; t++)
        for (int k = 0; k < 3; k++) {
            const float* v = s_q_tri[mb->t0 + t][k];
            const double z = v[2];
            const double sd = pl->n[0] * ((v[0] - ofx) * z / H) + pl->n[1] * ((v[1] - ofy) * z / H) +
                              pl->n[2] * z - pl->d;
            hi = ms_max(hi, sd); lo = ms_min(lo, sd);
        }
    return hi > 0.0 && lo < 0.0 ? ms_min(hi, -lo) : -ms_min(fabs(hi), fabs(lo));
}

/* What explains a moby's mismatching planes: each flag holds while every
 * mismatching plane so far is a grazing contact (graze), one of those or a
 * tangent contact (tan), one of those or a split merge (split), a tangent
 * contact or a split merge (strict: the grazing test left out), or lies in a
 * sector the vertex animation moves (vanim). */
typedef struct { int graze, tan, split, strict, vanim; } mc_why;

/* The other leg of a plane under explanation: its planes. */
typedef struct { const openpete_snap_plane* pl; int n; } mc_leg;

/** @brief Fold one mismatching plane into a moby's explanations.
 * @param qi The moby's queue index.
 * @param pl The plane.
 * @param id Its source face.
 * @param unmatched 1 for a plane with no partner (tested for grazing, tangent and split), 0 for a partnered one whose attributes differ (none of them).
 * @param o The other leg's planes.
 * @param w The explanations, updated. */
static void mc_vf_explain(int qi, const openpete_snap_plane* pl, const ms_pid* id, int unmatched, mc_leg o, mc_why* w) {
    const int g = unmatched && fabs(mc_straddle(&s_q_mob[qi], pl)) < MC_GRAZE;
    const int t = unmatched && mc_vf_tangent(qi, pl);
    w->graze &= g;
    w->tan &= g || t;
    const int e = unmatched && mc_vf_has_eq(pl, o.pl, o.n);
    w->split &= g || t || e;
    w->strict &= t || e;
    w->vanim &= mc_vf_vsec(qi, id->sec);
}

/** @brief Compare one moby's carried planes against its fresh ones, counting the plane classes.
 * @param qi The moby's queue index.
 * @param w The explanations of its mismatching planes, updated.
 * @return The count of mismatching planes and plane attributes. */
static long mc_vf_planes(int qi, mc_why* w) {
    const int nc = s_vc_np[qi], nf = s_vf_np[qi];
    uint8_t used[MS_MAX_PLANES] = { 0 };
    const openpete_snap_plane* f = &s_vf_pl[s_vf_p0[qi]];
    const mc_leg lf = { f, nf }, lc = { &s_vc_pl[s_vc_p0[qi]], nc };
    long bad = 0;
    for (int j = 0; j < nc; j++) {
        const openpete_snap_plane* c = &s_vc_pl[s_vc_p0[qi] + j];
        double sg = 1.0;
        const int m = mc_vf_match(c, f, nf, used, &sg);
        s_vs.planes++;
        if (m < 0) {
            s_vs.carry_only_p++; bad++;
            mc_vf_explain(qi, c, &s_vc_id[s_vc_p0[qi] + j], 1, lf, w);
            continue;
        }
        const int side = (c->side < 0.0f) != (sg * f[m].side < 0.0);
        const int nb = side + (c->slab != f[m].slab) + (c->dside != f[m].dside);
        s_vs.side += side; s_vs.slab += c->slab != f[m].slab; s_vs.dside += c->dside != f[m].dside;
        bad += nb;
        if (nb) mc_vf_explain(qi, c, &s_vc_id[s_vc_p0[qi] + j], 0, lf, w);
    }
    for (int j = 0; j < nf; j++) {
        if (used[j]) continue;
        s_vs.fresh_only_p++; bad++;
        mc_vf_explain(qi, &f[j], &s_vf_id[s_vf_p0[qi] + j], 1, lc, w);
    }
    return bad;
}

/** @brief Fold every plane of a moby's one resolved leg into its explanations (a carry-only or fresh-only moby).
 * @param qi The moby's queue index.
 * @param pl The planes.
 * @param id Their source faces.
 * @param n Their count.
 * @param w The explanations, updated. */
static void mc_vf_explain_all(int qi, const openpete_snap_plane* pl, const ms_pid* id, int n, mc_why* w) {
    const mc_leg none = { NULL, 0 };
    for (int j = 0; j < n; j++) mc_vf_explain(qi, &pl[j], &id[j], 1, none, w);
}

/** @brief The class of a mismatching moby: the first that holds of moved, grazing, tangent, split, vanim, static.
 * @param mv Its corner displacement (mc_vf_moved).
 * @param w The explanations of its mismatching planes.
 * @return The class (MC_CLS_*). */
static int mc_vf_class(double mv, const mc_why* w) {
    if (mv > MC_MOVED) return MC_CLS_MOVED;
    if (w->graze) return MC_CLS_GRAZING;
    if (w->tan) return MC_CLS_TANGENT;
    if (w->split) return MC_CLS_SPLIT;
    return w->vanim ? MC_CLS_VANIM : MC_CLS_STATIC;
}

/** @brief Compare one moby's carried planes against its fresh ones and count the classes.
 *
 * A moby mc_refresh refused counts as refreshed and is not compared. A
 * mismatching moby takes the first class that holds (mc_vf_class).
 * @param qi The moby's queue index.
 * @return 1 when any class other than absent or unresolved holds, else 0. */
static int mc_vf_moby(int qi) {
    const int nc = s_vc_np[qi], nf = s_vf_np[qi];
    int topo = 0;
    if (s_vc_ref[qi]) {
        s_vs.refreshed++;
        s_vs.refreshed_fs += s_vc_ref[qi] == 2;
        mc_vf_cmov_line(qi, mc_vf_moved(qi, &topo), topo);
        return 0;
    }
    if (nc < 0) { s_vs.absent++; return 0; }
    if (nf < 0) { s_vs.unresolved++; return 0; }
    s_vs.mobys++;
    const double mv = mc_vf_moved(qi, &topo);
    mc_vf_cmov_add(mv, s_vs.cmov);
    mc_vf_cmov_line(qi, mv, topo);
    s_vs.inj += s_vc_inj[qi];
    if (!nc && !nf) { s_vs.inj_clean += s_vc_inj[qi]; return 0; }
    mc_why w = { 1, 1, 1, 1, 1 };
    const char* why = "planes";
    if (nc && !nf) { s_vs.carry_only_m++; why = "carry_only_moby"; }
    if (nf && !nc) { s_vs.fresh_only_m++; why = "fresh_only_moby"; }
    const long bad = nc && nf ? mc_vf_planes(qi, &w) : 0;
    if (nc && nf && !bad) { s_vs.inj_clean += s_vc_inj[qi]; return 0; }
    if (nc && !nf) mc_vf_explain_all(qi, &s_vc_pl[s_vc_p0[qi]], &s_vc_id[s_vc_p0[qi]], nc, &w);
    if (nf && !nc) mc_vf_explain_all(qi, &s_vf_pl[s_vf_p0[qi]], &s_vf_id[s_vf_p0[qi]], nf, &w);
    const double box = mc_vf_box_moved(qi);
    mc_vf_cmov_add(mv, s_vs.cmov_m);
    const int cls = mc_vf_class(mv, &w);
    s_vs.mism_cls[cls]++;
    if (s_vc_inj[qi]) s_vs.inj_cls[cls]++;
    s_vs.moved_topo += cls == MC_CLS_MOVED && topo;
    s_vs.moved_box += box > MC_MOVED;
    s_vs.static_box += !(box > MC_MOVED) && !w.split && !w.vanim;
    s_vs.graze_loose += cls == MC_CLS_GRAZING && !w.strict;
    mc_vf_detail(qi, why, cls, mv, box);
    return 1;
}
/** @brief Compare the carry against the fresh pass for every queued moby of this resolve and count the engagement.
 * @param ep The endpoint, or NULL.
 * @param L The present walk camera's linear part.
 * @param P Its position. */
static void mc_vf_compare(const mc_tab* ep, const double L[3][3], const double P[3]) {
    mc_vstat* v = &s_vs;
    v->resolves++;
    s_vf_dp = s_vf_dl = 0.0;
    const unsigned ep_scene = openpete_native_scene_epoch();
    if (ep_scene != v->last_epoch) { v->last_epoch = ep_scene; v->presents++; v->presents_anz += s_rex_alpha != 0.0; }
    if (ep) {
        double dp = 0.0, dl = 0.0;
        for (int i = 0; i < 3; i++) {
            dp += (P[i] - ep->P[i]) * (P[i] - ep->P[i]);
            for (int j = 0; j < 3; j++) dl = ms_max(dl, fabs(L[i][j] - ep->L[i][j]));
        }
        dp = sqrt(dp);
        s_vf_dp = dp; s_vf_dl = dl;
        v->cam_moved += dp > 0.0 || dl > 0.0;
        v->dp_sum += dp; v->dp_max = ms_max(v->dp_max, dp); v->dl_max = ms_max(v->dl_max, dl);
    }
    for (int k = 0; k < s_vq_n; k++) v->mism_mobys += mc_vf_moby(k);
    static int s_atexit;
    if (!s_atexit) { s_atexit = 1; atexit(mc_vf_print); }
    if (v->resolves % 512 == 0) mc_vf_print();
}

/** @brief Move the queued mobys still to be added (add 1) to the front of the queue, keeping their order.
 * @return Their count. */
static int ms_compact(void) {
    int n = 0;
    for (int k = 0; k < s_q_nm; k++) {
        if (!s_q_mob[k].add) continue;
        if (n != k) { const ms_moby t = s_q_mob[n]; s_q_mob[n] = s_q_mob[k]; s_q_mob[k] = t; }
        n++;
    }
    return n;
}

/* The carried mobys of the resolve under way whose planes wait for the
 * table, in queue order: a copy of the queued moby (ms_compact reorders the
 * queue) and its run of s_vc_pl. s_cr_pos counts those already added. */
typedef struct { ms_moby mb; int p0, np; } ms_cr;
static ms_cr* s_cr; static int s_cr_cap, s_cr_n, s_cr_pos;
static int s_cr_late;   /* the merge check's calibration: carried planes go to the table after every fresh moby */

/** @brief Note the carried planes of every queued moby the endpoint resolved, in queue order, for the table (ms_cr_flush), and mark those mobys added.
 *
 * The planes wait so that the table takes each walk pass's mobys in the
 * order a resolve per pass gives (carried mobys of the pass, then its fresh
 * ones) when one resolve serves several passes. On an allocation failure a
 * moby's planes are added at once.
 * @param cs The census: carried and fresh mobys, overflow. */
static void ms_carry_add(ms_census* cs) {
    for (int k = 0; k < s_q_nm; k++) {
        ms_moby* mb = &s_q_mob[k];
        if (s_vc_np[k] < 0) { cs->fresh++; continue; }
        mb->add = 0;
        cs->carried++;
        if (s_vc_np[k] <= 0) continue;
        if (!ms_grow((void**)&s_cr, &s_cr_cap, s_cr_n + 1, sizeof *s_cr)) {
            ms_add(mb, &s_vc_pl[s_vc_p0[k]], s_vc_np[k], cs);
            continue;
        }
        ms_cr* c = &s_cr[s_cr_n++];
        c->mb = *mb; c->p0 = s_vc_p0[k]; c->np = s_vc_np[k];
    }
}

/** @brief Add the waiting carried planes (ms_carry_add) of every walk pass up to and including seg to the table, in queue order.
 * @param seg The last walk pass to add; INT_MAX adds all.
 * @param cs The census: overflow. */
static void ms_cr_flush(int seg, ms_census* cs) {
    for (; s_cr_pos < s_cr_n && s_cr[s_cr_pos].mb.seg <= seg; s_cr_pos++)
        ms_add(&s_cr[s_cr_pos].mb, &s_vc_pl[s_cr[s_cr_pos].p0], s_cr[s_cr_pos].np, cs);
}

/** @brief Add a freshly resolved moby's planes to the table after the waiting carried planes of its walk pass and the passes before it (ms_cr_flush).
 * @param mb The moby.
 * @param out Its planes.
 * @param np Their count, > 0.
 * @param cs The census.
 * @return np when added, 0 when the table refused them. */
static int ms_add_fresh(const ms_moby* mb, const openpete_snap_plane* out, int np, ms_census* cs) {
    if (!s_cr_late) ms_cr_flush(mb->seg, cs);
    return ms_add(mb, out, np, cs);
}


/* ---- the resolve's stage split (--profile-out) ------------------------------
 * Nanoseconds per part of ms_resolve, summed over the resolve and added to
 * the profiler bucket <leg>.rs.<part> (mcan.rs.capture, mrex.rs.contact) once
 * per resolve. The parts sum to the resolve less its entry tests; rs.carry.map
 * is the part of rs.carry that maps the endpoint's planes (mc_map). */
enum { RSP_CARRY = 0, RSP_BOX, RSP_CAPTURE, RSP_TMAKE, RSP_CANDS, RSP_CONTACT, RSP_OVERLAP, RSP_END,
       RSP_MAP, RSP_N };
static const char* const k_rsp_name[RSP_N] = {
    "rs.carry", "rs.box", "rs.capture", "rs.tmake", "rs.cands", "rs.contact", "rs.overlap", "rs.end",
    "rs.carry.map" };
static uint64_t s_rsp_ns[RSP_N];

/** @brief A monotonic nanosecond stamp while a trace is open and OPENPETE_MOBY_SUBSTAGE is set nonzero, else 0.
 * @return The stamp, or 0. */
static uint64_t rsp_now(void) {
    static int en = -1;
    if (en < 0) { const char* e = getenv("OPENPETE_MOBY_SUBSTAGE"); en = (e && *e && *e != '0'); }
    return (en && op_prof_on()) ? ms_mono_ns() : 0;
}

/** @brief Close one part of the resolve at now and return now as the next part's start.
 * @param part The part, RSP_CARRY to RSP_END.
 * @param t0 The part's start; 0 adds nothing and returns 0.
 * @return The stamp now, or 0. */
static uint64_t rsp_mark(int part, uint64_t t0) {
    if (!t0) return 0;
    const uint64_t t1 = ms_mono_ns();
    s_rsp_ns[part] += t1 - t0;
    return t1;
}

/** @brief Add the resolve's part totals to the profiler buckets of its leg and zero them. */
static void rsp_flush(void) {
    static int id[2][RSP_N];
    static int init;
    if (!op_prof_on()) { memset(s_rsp_ns, 0, sizeof s_rsp_ns); return; }
    if (!init) {
        char nb[48];
        for (int l = 0; l < 2; l++)
            for (int k = 0; k < RSP_N; k++) {
                snprintf(nb, sizeof nb, "%s.%s", l ? "mrex" : "mcan", k_rsp_name[k]);
                id[l][k] = op_prof_intern(nb);
            }
        init = 1;
    }
    for (int k = 0; k < RSP_N; k++) {
        if (s_rsp_ns[k]) op_prof_accum(id[s_rex ? 1 : 0][k], (s_rsp_ns[k] + 500u) / 1000u);
        s_rsp_ns[k] = 0;
    }
}

/** @brief Set up the carry for the resolve under way and return how many queued mobys the contact pass resolves.
 *
 * A canonical resolve (carry or verify on) stores into its epoch's table
 * and resolves the whole queue. A sub-tick resolve maps the endpoint's
 * planes (mc_map); with the carry on it adds them and resolves only the
 * mobys the endpoint did not or mc_refresh refused, moved to the front of
 * the queue, unless the verify runs, which resolves the whole queue and adds
 * only those. Under integer vertex snap the carry runs as well, though the
 * walk floors each corner's screen position under the present camera, so a
 * carried plane is exact only for a bit-equal camera.
 * @param cs The census.
 * @return The count of queued mobys, from the front, the contact pass resolves. */
static int ms_carry_begin(ms_census* cs) OP_REQUIRES(sim) {
    s_store = NULL; s_vf_on = 0; s_rs_ep = NULL; s_rs_wb = 0;
    s_cr_n = s_cr_pos = 0;
    for (int k = 0; k < s_q_nm; k++) { s_q_mob[k].qi = k; s_q_mob[k].add = 1; }
    ms_cb_begin();
    const int carry = mc_carry_on(), vm = mc_verify_mode();
    if (!carry && !vm) return s_q_nm;
    if (!terrain_snap_camera(s_rs_L, s_rs_P)) return s_q_nm;
    double Q[3][3];
    if (mc_inv_t(s_rs_L, Q)) {
        for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) s_rs_Li[r][c] = Q[c][r];
        s_rs_wb = 1;
    }
    if (!s_rex) {
        s_store = mc_table_for(spyro_native_canon_epoch_stamp(), openpete_native_scene_frame());
        mc_cam_note(s_store, s_rs_L, s_rs_P);
        return s_q_nm;
    }
    const mc_tab* ep = mc_endpoint();
    s_rs_ep = ep;
    const uint64_t tm = rsp_now();
    const int mapped = mc_map(s_rs_ep, s_rs_L, s_rs_P);
    rsp_mark(RSP_MAP, tm);
    if (!mapped) return s_q_nm;
    s_vf_on = vm && mc_vf_begin();
    if (!carry) return s_q_nm;
    ms_carry_add(cs);
    s_pc.carried += cs->carried; s_pc.fresh += cs->fresh;
    return s_vf_on ? s_q_nm : ms_compact();
}

/** @brief Finish the carry of the resolve under way: the verify comparison.
 *
 * A canonical store also records each moby's contact region and candidate
 * faces (mc_store_cands), the endpoint side of mc_faces_changed.
 * @param cs The resolve's census; its overflow counts into the verify's. */
static void ms_carry_end(const ms_census* cs) {
    if (s_store && mc_refresh_on()) mc_store_cands(s_store);
    if (s_vf_on) { s_vs.ovf += cs->ovf; mc_vf_compare(s_rs_ep, s_rs_L, s_rs_P); }
    s_store = NULL; s_vf_on = 0;
}

/* ---- the contact pass's fan-out ---------------------------------------------
 * One chunk's contact pass runs per moby on the walk workers when the chunk is
 * large enough. Serial prefix (the chunk's capture, the built terrain and the
 * candidate buckets, on the calling thread): the corner slab's size. Workers:
 * one moby each (ms_fo_item), its planes into its own result slot through the
 * worker's scratch, and, for a resolve that stores into the carry, its world
 * box and key-depth corners. Commit, on the calling thread in queue order:
 * the carry's store (mc_store), the scene's snap table add and the ordinal
 * census (ms_add), and the worker censuses summed. Everything order-dependent
 * is in the commit, so the tables are the serial pass's
 * (OPENPETE_MOBY_SNAP_FANOUT_CHECK compares them). */

static float    s_fo_wb[128][6];                      /* per chunk moby: the store's world box */

/* The fan-out's work bound: below it the chunk runs serially. Work is
 * counted per moby with candidates as its triangle count plus its candidate
 * count, and per moby the carry stores as its triangle count (ms_fo_work).
 * The fan-out pays a fixed cost the serial pass does not (the pool's wake
 * and join, the census merge, the commit), measured near 150 us per chunk
 * whatever its size against about 0.04 us per work unit serial. Measured
 * per chunk by OPENPETE_MOBY_SNAP_FANOUT_CHECK on six rows of the three
 * games (8 workers, Linux): 4,000 to 8,000 units ran 154 to 268 us serial
 * against 166 to 186 fanned out; 8,000 to 32,000 ran 318 to 903 against 182
 * to 310; every sub-tick chunk measured stayed under 2,000. The crossover is
 * near 6,000; OPENPETE_MOBY_SNAP_FANOUT_MIN re-derives it on another
 * machine. */
#define MS_FO_MIN_WORK 6000

typedef struct {
    int g;                          /* the chunk's first queued moby */
    const terrain_snap_tri* tt;     /* the captured terrain */
    const int* cand;                /* the candidate buckets (ms_cands) */
    const int* first;               /* each moby's bucket start; first[nb] the end */
    const float* view;              /* H, ofx, ofy */
    int prep;                       /* the resolve stores into the carry: compute each moby's box and corners */
    int cbox;                       /* the store records contact regions: compute each moby's corner box (s_cb) */
} ms_fo_job;

/** @brief Work-pool job: one chunk moby's contact planes into its result slot, its store box and corners when the resolve stores, and its corner box when the store records contact regions.
 * @param k The moby's chunk index.
 * @param worker The pool worker, 0 for the calling thread.
 * @param arg The ms_fo_job. */
static void ms_fo_item(int k, int worker, void* arg) {
    const ms_fo_job* J = (const ms_fo_job*)arg;
    ms_ws* W = &s_ws[worker];
    const ms_moby* mb = &s_q_mob[J->g + k];
    ms_fo_res* r = &s_fo_res[k];
    if (J->cbox) ms_moby_cbox(mb, s_cb[J->g + k]);
    r->np = ms_moby_planes(W, mb, J->tt, J->cand + J->first[k], J->first[k + 1] - J->first[k], J->view,
                           &W->cs, r->pl, r->id);
    if (J->prep && r->np >= 0) ms_store_prep(mb, s_fo_wb[k], &s_kc[3 * mb->t0]);
}

/** @brief Add the counts a worker's contact pass made to the resolve's census (its phase clocks included).
 * @param d The resolve's census.
 * @param w The worker's. */
static void ms_census_merge(ms_census* d, const ms_census* w) {
    d->tests += w->tests; d->contacts += w->contacts; d->contacts_t4 += w->contacts_t4;
    d->ovf += w->ovf; d->planes += w->planes;
    d->near_ns += w->near_ns; d->test_ns += w->test_ns; d->plane_ns += w->plane_ns;
}

/** @brief Whether OPENPETE_MOBY_SNAP_FANOUT is not 0: the contact pass may fan out (default on), read once.
 * @return 1 when it may. */
static int ms_fo_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_FANOUT"); v = !(e && *e == '0'); }
    return v;
}

/** @brief The fan-out's work bound: OPENPETE_MOBY_SNAP_FANOUT_MIN, else MS_FO_MIN_WORK, a calibration override.
 * @return The bound. */
static long ms_fo_min(void) {
    static long v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_FANOUT_MIN"); v = (e && *e) ? atol(e) : MS_FO_MIN_WORK; }
    return v;
}

/** @brief Whether OPENPETE_MOBY_SNAP_FANOUT_CHECK is set nonzero: every chunk that fans out also runs serially and the two are compared, read once.
 * @return 1 when on. */
static int ms_fo_check_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_SNAP_FANOUT_CHECK"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief The work of one chunk's contact pass, in the unit MS_FO_MIN_WORK bounds.
 * @param g The chunk's first queued moby.
 * @param nb Its moby count.
 * @param first The candidate bucket starts.
 * @return The work. */
static long ms_fo_work(int g, int nb, const int* first) {
    long w = 0;
    const int store = s_store && s_rs_wb;
    for (int k = 0; k < nb; k++) {
        const int nc = first[k + 1] - first[k], nt = s_q_mob[g + k].nt;
        w += (nc > 0 ? nt + nc : 0) + (store ? nt : 0);
    }
    return w;
}

/** @brief Whether this chunk's contact pass fans out: on, not the first chunk of the run, no instrument that runs per moby in queue order, at least two mobys and two workers, the work bound met.
 *
 * The first chunk runs serially so every lazy latch the pass reaches
 * resolves on one thread. The cut falsifier, the carry verify and the plane
 * dump write shared state or print per moby, so each keeps the pass serial.
 * @param g The chunk's first queued moby.
 * @param nb Its moby count.
 * @param first The candidate bucket starts.
 * @return The worker count to fan out over, or 0 for the serial pass. */
static int ms_fo_workers(int g, int nb, const int* first) {
    static int first_chunk = 1;
    if (first_chunk) {
        first_chunk = 0;
        (void)ms_falsify_on(); (void)ms_dump_keys(); (void)moby_snap_probe_on(); (void)ms_fo_check_on();
        return 0;
    }
    if (!ms_fo_on() || s_vf_on || ms_falsify_on() || ms_dump_keys() || nb < 2) return 0;
    int nw = openpete_workpool_threads();
    if (nw > MS_WORKERS) nw = MS_WORKERS;
    if (nw <= 1 || ms_fo_work(g, nb, first) < ms_fo_min()) return 0;
    return nw;
}

/** @brief Find one queued moby's contact planes against the captured terrain, hand them to the carry and add them to the scene's snap table.
 *
 * A moby the carry already placed (add 0) is resolved for the verify
 * instrument only and adds nothing.
 * @param W The worker's scratch.
 * @param mb The moby.
 * @param tt The captured terrain triangles.
 * @param cand The indices into @p tt of those its search box reaches.
 * @param ncand Their count.
 * @param view H, ofx, ofy.
 * @param cs The census, updated.
 * @return The planes added. */
static int ms_moby_contact(ms_ws* W, const ms_moby* mb, const terrain_snap_tri* tt, const int* cand,
                           int ncand, const float view[3], ms_census* cs) {
    openpete_snap_plane out[MS_MAX_PLANES];
    ms_pid id[MS_MAX_PLANES];
    const int np = ms_moby_planes(W, mb, tt, cand, ncand, view, cs, out, id);
    if (s_ck_rec) {
        ms_fo_res* r = &s_ck_res[mb - s_ck_rec];
        r->np = np;
        if (np > 0) { memcpy(r->pl, out, (size_t)np * sizeof *out); memcpy(r->id, id, (size_t)np * sizeof *id); }
    }
    if (np < 0) return 0;
    float wb[6];
    double (*kc)[3] = NULL;
    const int prep = s_store && s_rs_wb;
    if (prep) {
        kc = ms_store_kc_grow() ? &s_kc[3 * mb->t0] : NULL;
        ms_store_prep(mb, wb, kc);
    }
    ms_result(mb, out, id, np, prep ? wb : NULL, (const double (*)[3])kc);
    if (!np || !mb->add) return 0;
    return ms_add_fresh(mb, out, np, cs);
}

/** @brief Run one chunk's contact pass serially, moby by moby, on the calling thread.
 * @param J The chunk.
 * @param nb Its moby count.
 * @param cs The resolve's census.
 * @param snapped Receives, per chunk moby, 1 when its planes went to the snap table. */
static void ms_fo_serial(const ms_fo_job* J, int nb, ms_census* cs, uint8_t* snapped) {
    for (int k = 0; k < nb; k++)
        snapped[k] = ms_moby_contact(&s_ws[0], &s_q_mob[J->g + k], J->tt, J->cand + J->first[k],
                                     J->first[k + 1] - J->first[k], J->view, cs) > 0;
}

/** @brief Run one chunk's contact pass on the walk workers, then commit every moby's planes in queue order.
 * @param J The chunk; prep set when the resolve stores into the carry.
 * @param nb Its moby count.
 * @param cs The resolve's census.
 * @param snapped Receives, per chunk moby, 1 when its planes went to the snap table.
 * @return 1 when it ran, 0 when the corner slab could not grow (nothing changed; run serially). */
static int ms_fo_parallel(const ms_fo_job* J, int nb, ms_census* cs, uint8_t* snapped) {
    if (J->prep && !ms_store_kc_grow()) return 0;
    for (int w = 0; w < MS_WORKERS; w++) { memset(&s_ws[w].cs, 0, sizeof s_ws[w].cs); s_ws[w].cs.timed = cs->timed; }
    openpete_workpool_run(nb, ms_fo_item, (void*)J);
    if (J->cbox) memset(&s_cb_ok[J->g], 1, (size_t)nb);
    for (int w = 0; w < MS_WORKERS; w++) ms_census_merge(cs, &s_ws[w].cs);
    for (int k = 0; k < nb; k++) {
        const ms_moby* mb = &s_q_mob[J->g + k];
        const ms_fo_res* r = &s_fo_res[k];
        snapped[k] = 0;
        if (r->np < 0) continue;
        ms_result(mb, r->pl, r->id, r->np, J->prep ? s_fo_wb[k] : NULL,
                  J->prep ? (const double (*)[3])&s_kc[3 * mb->t0] : NULL);
        if (r->np && mb->add) snapped[k] = ms_add_fresh(mb, r->pl, r->np, cs) > 0;
    }
    return 1;
}

/* ---- the fan-out's identity check (OPENPETE_MOBY_SNAP_FANOUT_CHECK) --------- */

/* The part of a carry table a chunk's commit appends to, and the slots it
 * writes, as they stood before the commit. */
typedef struct { int ne, npl, nwc, nfc; long dup, full; unsigned hg[MC_HSIZE]; int hi[MC_HSIZE]; } mc_tab_mark;

/** @brief Record a carry table's counts and slots.
 * @param t The table.
 * @param m Receives them. */
static void mc_tab_mark_take(const mc_tab* t, mc_tab_mark* m) {
    m->ne = t->ne; m->npl = t->npl; m->nwc = t->nwc; m->nfc = t->nfc; m->dup = t->dup; m->full = t->full;
    memcpy(m->hg, t->hg, sizeof m->hg); memcpy(m->hi, t->hi, sizeof m->hi);
}

/** @brief Return a carry table to its recorded counts and slots; what was appended past them is dropped by the counts.
 * @param t The table.
 * @param m The record. */
static void mc_tab_mark_back(mc_tab* t, const mc_tab_mark* m) {
    t->ne = m->ne; t->npl = m->npl; t->nwc = m->nwc; t->nfc = m->nfc; t->dup = m->dup; t->full = m->full;
    memcpy(t->hg, m->hg, sizeof t->hg); memcpy(t->hi, m->hi, sizeof t->hi);
}

/* What one commit appended to the carry table: its entries, world planes,
 * resolved planes, source faces, corners, queued corners and corner face
 * keys, copied out. */
typedef struct {
    mc_ent* e; int ne, cap_e;
    mc_wplane* w; openpete_snap_plane* c; ms_pid* id; int npl, cap_p;
    double (*wc)[3]; float (*rc)[3]; uint32_t* fk; int nwc, cap_wc;
    long dup, full; unsigned hg[MC_HSIZE]; int hi[MC_HSIZE];
} mc_tab_span;

/** @brief Copy what a table holds past a mark into a span.
 * @param t The table.
 * @param m The mark.
 * @param o Receives the appended entries, planes and corners and the table's counters and slots.
 * @return 0 on an allocation failure, else 1. */
static int mc_tab_span_take(const mc_tab* t, const mc_tab_mark* m, mc_tab_span* o) {
    o->ne = t->ne - m->ne; o->npl = t->npl - m->npl; o->nwc = t->nwc - m->nwc;
    if (!ms_grow((void**)&o->e, &o->cap_e, o->ne + 1, sizeof *o->e) ||
        !ms_grow((void**)&o->w, &o->cap_p, o->npl + 1, sizeof *o->w) ||
        !(o->c = (openpete_snap_plane*)realloc(o->c, (size_t)o->cap_p * sizeof *o->c)) ||
        !(o->id = (ms_pid*)realloc(o->id, (size_t)o->cap_p * sizeof *o->id)) ||
        !ms_grow((void**)&o->wc, &o->cap_wc, o->nwc + 1, sizeof *o->wc) ||
        !(o->rc = (float (*)[3])realloc(o->rc, (size_t)o->cap_wc * sizeof *o->rc)) ||
        !(o->fk = (uint32_t*)realloc(o->fk, (size_t)o->cap_wc * sizeof *o->fk))) return 0;
    memcpy(o->e, t->e + m->ne, (size_t)o->ne * sizeof *o->e);
    memcpy(o->w, t->w + m->npl, (size_t)o->npl * sizeof *o->w);
    memcpy(o->c, t->c + m->npl, (size_t)o->npl * sizeof *o->c);
    memcpy(o->id, t->id + m->npl, (size_t)o->npl * sizeof *o->id);
    memcpy(o->wc, t->wc + m->nwc, (size_t)o->nwc * sizeof *o->wc);
    if (o->nwc) memcpy(o->rc, t->rc + m->nwc, (size_t)o->nwc * sizeof *o->rc);
    const int probe = moby_snap_probe_on() && t->fk;
    for (int i = 0; i < o->nwc; i++) o->fk[i] = probe ? t->fk[m->nwc + i] : 0u;
    o->dup = t->dup; o->full = t->full;
    memcpy(o->hg, t->hg, sizeof o->hg); memcpy(o->hi, t->hi, sizeof o->hi);
    return 1;
}

/** @brief Count the fields in which two spans differ: entry by entry (identity, plane run, world box, corner run, signature, probe fields), plane by plane (world, resolved, source face), corner by corner, and the counters and slots.
 * @param a One span.
 * @param b The other.
 * @return The mismatch count. */
static long mc_tab_span_diff(const mc_tab_span* a, const mc_tab_span* b) {
    if (a->ne != b->ne || a->npl != b->npl || a->nwc != b->nwc) return 1 + labs((long)a->ne - b->ne);
    long bad = a->dup != b->dup || a->full != b->full;
    bad += memcmp(a->hg, b->hg, sizeof a->hg) != 0 || memcmp(a->hi, b->hi, sizeof a->hi) != 0;
    for (int i = 0; i < a->ne; i++) {
        const mc_ent* x = &a->e[i]; const mc_ent* y = &b->e[i];
        bad += x->mid != y->mid || x->p0 != y->p0 || x->np != y->np || x->wb_ok != y->wb_ok ||
               memcmp(x->wb, y->wb, sizeof x->wb) != 0 || x->wc0 != y->wc0 || x->nwc != y->nwc ||
               x->sig_ok != y->sig_ok || (x->sig_ok && x->sig != y->sig) || x->path != y->path ||
               memcmp(&x->off, &y->off, sizeof x->off) != 0 || memcmp(&x->kr, &y->kr, sizeof x->kr) != 0 ||
               memcmp(x->kv, y->kv, sizeof x->kv) != 0;
    }
    for (int i = 0; i < a->npl; i++)
        bad += memcmp(&a->w[i], &b->w[i], sizeof a->w[i]) != 0 || memcmp(&a->c[i], &b->c[i], sizeof a->c[i]) != 0 ||
               memcmp(&a->id[i], &b->id[i], sizeof a->id[i]) != 0;
    for (int i = 0; i < a->nwc; i++)
        bad += memcmp(a->wc[i], b->wc[i], sizeof a->wc[i]) != 0 || memcmp(a->rc[i], b->rc[i], sizeof a->rc[i]) != 0 ||
               a->fk[i] != b->fk[i];
    return bad;
}

/* Check totals: chunks compared and their mobys and planes, by leg (0 canonical,
 * 1 sub-tick); mismatches per moby result, snap table, carry table field,
 * snapped flag and census; chunks refused; serial and fanned-out
 * nanoseconds per work band; the corner boxes the canonical chunks
 * computed (s_cb) against a serial recompute. */
#define FOC_BANDS 9
static const long k_foc_band[FOC_BANDS] = { 250, 500, 1000, 2000, 4000, 8000, 16000, 32000, LONG_MAX };
static struct {
    long chunks[2], mobys[2], planes[2], bad_res, bad_snap, bad_carry, bad_flag, bad_census, refused;
    long n[2][FOC_BANDS]; double ser_ns[2][FOC_BANDS], par_ns[2][FOC_BANDS];
    long boxes, bad_box;
} s_foc;

/** @brief Print the check's totals, with the mean serial and fanned-out microseconds per leg and work band.
 * @param tag Why the line prints. */
static void foc_print(const char* tag) {
    fprintf(stderr, "[snap-fanout-check %s] canon: chunks=%ld mobys=%ld planes=%ld | rex: chunks=%ld mobys=%ld planes=%ld"
                    " | mismatched_results=%ld mismatched_snap=%ld mismatched_carry=%ld mismatched_snapped=%ld"
                    " mismatched_census=%ld refused=%ld |",
            tag, s_foc.chunks[0], s_foc.mobys[0], s_foc.planes[0], s_foc.chunks[1], s_foc.mobys[1], s_foc.planes[1],
            s_foc.bad_res, s_foc.bad_snap, s_foc.bad_carry, s_foc.bad_flag, s_foc.bad_census, s_foc.refused);
    fprintf(stderr, " boxes=%ld mismatched_boxes=%ld |", s_foc.boxes, s_foc.bad_box);
    for (int l = 0; l < 2; l++)
        for (int b = 0; b < FOC_BANDS; b++)
            if (s_foc.n[l][b])
                fprintf(stderr, " %s<%ld: n=%ld ser=%.1f par=%.1f", l ? "rex" : "can", k_foc_band[b], s_foc.n[l][b],
                        s_foc.ser_ns[l][b] / s_foc.n[l][b] / 1000.0, s_foc.par_ns[l][b] / s_foc.n[l][b] / 1000.0);
    fputc('\n', stderr);
}

/** @brief atexit hook: print the check's final totals. */
static void foc_atexit(void) { foc_print("final"); }


/** @brief Count the result fields in which the serial pass's record of one chunk moby differs from the fan-out's.
 * @param a The serial pass's.
 * @param b The fan-out's.
 * @return 0 when equal, else 1. */
static int foc_res_diff(const ms_fo_res* a, const ms_fo_res* b) {
    if (a->np != b->np) return 1;
    return a->np > 0 && (memcmp(a->pl, b->pl, (size_t)a->np * sizeof a->pl[0]) != 0 ||
                         memcmp(a->id, b->id, (size_t)a->np * sizeof a->id[0]) != 0);
}

/** @brief Run one chunk's contact pass serially and then fanned out on the same input, and compare.
 *
 * Between the two the snap table, the carry table the resolve stores into,
 * the ordinal census and the census return to their marks. Compared: each
 * moby's planes, source faces and count; the snap table's entries, planes
 * and counters added (openpete_native_snap_digest); every field the commit
 * appended to the carry table and its slots and counters; each moby's
 * snapped flag; and the census counts. The fanned-out run's result is kept.
 * @param J The chunk.
 * @param nb Its moby count.
 * @param cs The resolve's census.
 * @param snapped Receives, per chunk moby, 1 when its planes went to the snap table. */
static void ms_fo_check(const ms_fo_job* J, int nb, ms_census* cs, uint8_t* snapped) {
    static int s_atexit;
    if (!s_atexit) { s_atexit = 1; atexit(foc_atexit); }
    static mc_tab_mark tm;
    static mc_tab_span sp_ser, sp_par;
    openpete_native_snap_mark_t sm;
    openpete_native_snap_mark(&sm);
    if (s_store) mc_tab_mark_take(s_store, &tm);
    const int sn0 = s_sn_n, cr0 = s_cr_pos;
    const ms_census cs0 = *cs;
    uint8_t fl_ser[128];
    s_ck_rec = &s_q_mob[J->g];
    const uint64_t t0 = ms_mono_ns();
    ms_fo_serial(J, nb, cs, fl_ser);
    const uint64_t t1 = ms_mono_ns();
    s_ck_rec = NULL;
    const uint64_t dg_ser = openpete_native_snap_digest(&sm);
    const ms_census cs_ser = *cs;
    const int carry_ok = !s_store || mc_tab_span_take(s_store, &tm, &sp_ser);
    openpete_native_snap_rewind(&sm);
    if (s_store) mc_tab_mark_back(s_store, &tm);
    s_sn_n = sn0; s_cr_pos = cr0;
    *cs = cs0;
    const uint64_t t2 = ms_mono_ns();
    if (!ms_fo_parallel(J, nb, cs, snapped)) { s_foc.refused++; ms_fo_serial(J, nb, cs, snapped); return; }
    const uint64_t t3 = ms_mono_ns();
    const int leg = s_rex ? 1 : 0;
    long bres = 0, flag = 0, planes = 0;
    for (int k = 0; k < nb; k++) {
        bres += foc_res_diff(&s_ck_res[k], &s_fo_res[k]);
        flag += fl_ser[k] != snapped[k];
        planes += s_fo_res[k].np > 0 ? s_fo_res[k].np : 0;
        if (!J->cbox) continue;
        float b[6];
        ms_moby_cbox(&s_q_mob[J->g + k], b);
        s_foc.bad_box += memcmp(b, s_cb[J->g + k], sizeof b) != 0;
        s_foc.boxes++;
    }
    const long bsnap = openpete_native_snap_digest(&sm) != dg_ser;
    long bcarry = 0;
    if (s_store) bcarry = carry_ok && mc_tab_span_take(s_store, &tm, &sp_par) ? mc_tab_span_diff(&sp_ser, &sp_par) : 1;
    const long bcen = cs_ser.tests != cs->tests || cs_ser.contacts != cs->contacts ||
                      cs_ser.contacts_t4 != cs->contacts_t4 || cs_ser.ovf != cs->ovf || cs_ser.planes != cs->planes;
    if ((bres || bsnap || bcarry || flag || bcen) && s_foc.bad_res + s_foc.bad_snap + s_foc.bad_carry < 8)
        fprintf(stderr, "[snap-fanout-check MISMATCH key=%u leg=%d] mobys=%d results=%ld snap=%ld carry=%ld snapped=%ld census=%ld\n",
                openpete_feed_key_now(), leg, nb, bres, bsnap, bcarry, flag, bcen);
    s_foc.bad_res += bres; s_foc.bad_snap += bsnap; s_foc.bad_carry += bcarry; s_foc.bad_flag += flag;
    s_foc.bad_census += bcen;
    s_foc.chunks[leg]++; s_foc.mobys[leg] += nb; s_foc.planes[leg] += planes;
    const long work = ms_fo_work(J->g, nb, J->first);
    int b = 0;
    while (work >= k_foc_band[b]) b++;
    s_foc.n[leg][b]++; s_foc.ser_ns[leg][b] += (double)(t1 - t0); s_foc.par_ns[leg][b] += (double)(t3 - t2);
    if (((s_foc.chunks[0] + s_foc.chunks[1]) & 1023) == 0) foc_print("running");
}

/** @brief Run one chunk's contact pass: fanned out over the walk workers when the chunk is large enough, else moby by moby.
 *
 * OPENPETE_MOBY_SNAP_FANOUT=0 keeps every chunk serial; under
 * OPENPETE_MOBY_SNAP_FANOUT_CHECK every fanned-out chunk is checked against
 * the serial pass (ms_fo_check).
 * @param J The chunk.
 * @param nb Its moby count.
 * @param cs The resolve's census.
 * @param snapped Receives, per chunk moby, 1 when its planes went to the snap table. */
static void ms_contact(const ms_fo_job* J, int nb, ms_census* cs, uint8_t* snapped) {
    if (!ms_fo_workers(J->g, nb, J->first)) { ms_fo_serial(J, nb, cs, snapped); return; }
    if (ms_fo_check_on()) { ms_fo_check(J, nb, cs, snapped); return; }
    if (!ms_fo_parallel(J, nb, cs, snapped)) ms_fo_serial(J, nb, cs, snapped);
}

static int ms_chunk_run(int g, int nb, const terrain_snap_box* box, const float view[3], ms_census* cs,
                        int listed, uint64_t tp);

/** @brief The per-pass slot of the sub-tick census (ms_pc) for the walk pass now queueing: s_q_seg, the last slot holding the rest.
 * @return The slot. */
static int ms_pc_seg(void) { return s_q_seg < MS_PC_SEGS - 1 ? s_q_seg : MS_PC_SEGS - 1; }

/** @brief Whether OPENPETE_SNAP_REX_CENSUS is set nonzero: the sub-tick census (ms_pc) is totalled and printed at exit, read once.
 * @return 1 when on. */
static int ms_rc_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SNAP_REX_CENSUS"); v = (e && *e && *e != '0'); }
    return v;
}

/* ---- the listed capture --------------------------------------------------------
 * ms_moby_planes tests contacts between a moby's triangles at their stored
 * view-Z and the captured terrain triangles. A terrain triangle that meets a
 * moby triangle has a point in the box of the moby's corners at stored view-Z
 * (ms_moby_box_at with no offset), and lies in its face, so its face's corner
 * box (terrain_snap_faces: every face of every visible sector meeting a box,
 * drawn or not, at the vertex phase the build draws, with the build's own
 * walk verdict) meets that box widened by MC_CAND, which covers the rounding
 * of the screen corners the triangle is rebuilt from. Only a face the build
 * draws (verdict not TERRAIN_FACE_DROPPED) emits triangles. So the capture
 * walks a cache-owned sector over those faces only (terrain_snap_capture_listed),
 * and every moby's planes, their order and its snap table rows are those of
 * the full walk: each triangle left out meets none of its triangles, and the
 * kept ones keep their capture order. The captured triangles, the triangle
 * tests and the sectors walked fall. OPENPETE_SNAP_LISTED=0 walks as before;
 * OPENPETE_SNAP_LISTED_CHECK=1 runs both on every chunk and compares
 * (ms_lc_check). */
static long s_lc_cen[3];   /* chunks listed, mobys listed, faces listed */

/** @brief Whether the listed capture is on (OPENPETE_SNAP_LISTED not 0), read once.
 * @return 1 when on, else 0. */
static int ms_lc_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SNAP_LISTED"); v = !(e && *e == '0'); }
    return v;
}

/** @brief Capture one chunk's terrain with the walk of cache-owned sectors limited to the drawn faces whose corner box meets a moby's stored view-Z box widened by MC_CAND.
 *
 * A moby without triangles keeps the full walk; on a failed face query or
 * allocation the whole chunk does.
 * @param g The chunk's first queued moby.
 * @param nb Its moby count.
 * @param box Its search boxes.
 * @param tt Receives the triangles.
 * @param tc Receives the capture's cost counters.
 * @return The triangle count, or -1 when the terrain is not ready. */
static int ms_lc_capture(int g, int nb, const terrain_snap_box* box, const terrain_snap_tri** tt,
                         terrain_snap_census* tc) {
    static terrain_snap_fkey* fk; static int fkcap;
    float kb[128][6];
    uint64_t walk[2] = { 0, 0 };
    for (int k = 0; k < nb; k++) {
        ms_moby_box_at(&s_q_mob[g + k], 0.0, kb[k]);
        if (!s_q_mob[g + k].nt) walk[k >> 6] |= 1ull << (k & 63);
        for (int a = 0; a < 3; a++) { kb[k][a] -= MC_CAND; kb[k][a + 3] += MC_CAND; }
    }
    const terrain_snap_face* ff = NULL;
    const int mf = terrain_snap_faces((const float (*)[6])kb, nb, &ff);
    if (mf < 0 || !ms_grow((void**)&fk, &fkcap, mf + 1, sizeof *fk)) return terrain_snap_capture(box, nb, tt, tc);
    int n = 0;
    for (int j = 0; j < mf; j++) {
        if (ff[j].verd == TERRAIN_FACE_DROPPED) continue;
        fk[n].sec = ff[j].sec; fk[n].pass = ff[j].pass; fk[n].face = ff[j].face;
        n++;
    }
    s_lc_cen[0]++; s_lc_cen[1] += nb; s_lc_cen[2] += n;
    return terrain_snap_capture_listed(box, nb, walk, fk, n, tt, tc);
}

/* ---- the listed capture's identity check (OPENPETE_SNAP_LISTED_CHECK) ------- */
static struct { long chunks[2], mobys, planes, bad_res, bad_snap, bad_flag, bad_census, tris[2], walks[2]; } s_lcc;

/** @brief Whether OPENPETE_SNAP_LISTED_CHECK is set nonzero, read once.
 * @return 1 when on. */
static int ms_lc_check_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SNAP_LISTED_CHECK"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Print the check's totals. */
static void ms_lc_check_print(void) {
    fprintf(stderr, "[snap-listed-check] chunks_canon=%ld chunks_rex=%ld mobys=%ld planes=%ld mismatched_results=%ld "
                    "mismatched_snap=%ld mismatched_snapped=%ld mismatched_census=%ld tris_full=%ld tris_listed=%ld "
                    "walks_full=%ld walks_listed=%ld faces_listed=%ld\n",
            s_lcc.chunks[0], s_lcc.chunks[1], s_lcc.mobys, s_lcc.planes, s_lcc.bad_res, s_lcc.bad_snap, s_lcc.bad_flag,
            s_lcc.bad_census, s_lcc.tris[0], s_lcc.tris[1], s_lcc.walks[0], s_lcc.walks[1], s_lc_cen[2]);
}

/** @brief Run one chunk both ways, the full walk first and then the listed capture, and compare.
 *
 * The full walk's pass runs serially with its results recorded (s_ck_rec);
 * the snap table, the carry table the resolve stores into, the ordinal
 * census and the census return to their marks before the listed pass runs,
 * serially as well. Compared, moby by moby: plane count, planes (normal,
 * offset, side, slab, double-sided flag) in order and their source faces;
 * the snap table rows added (openpete_native_snap_digest); every field the
 * store appended to the carry table; the snapped flags; and the census's
 * contacts, contacts on split faces, planes and overflow. The captured
 * triangles, the faces and sectors walked and the triangle tests differ by
 * design and are counted. The listed run's result is kept.
 * @param g The chunk's first queued moby.
 * @param nb Its moby count.
 * @param box Its search boxes.
 * @param view H, ofx, ofy.
 * @param cs The census, updated.
 * @return 0 when a capture failed, else 1. */
static int ms_lc_check(int g, int nb, const terrain_snap_box* box, const float view[3], ms_census* cs) {
    static int s_atexit;
    static ms_fo_res ref[128];
    static mc_tab_mark tm;
    static mc_tab_span sp_full, sp_list;
    if (!s_atexit) { s_atexit = 1; atexit(ms_lc_check_print); }
    openpete_native_snap_mark_t sm;
    openpete_native_snap_mark(&sm);
    if (s_store) mc_tab_mark_take(s_store, &tm);
    const int sn0 = s_sn_n, cr0 = s_cr_pos;
    const ms_census cs0 = *cs;
    s_ck_rec = &s_q_mob[g];
    s_lc_serial = 1;
    const int ok_full = ms_chunk_run(g, nb, box, view, cs, 0, 0);
    memcpy(ref, s_ck_res, (size_t)nb * sizeof ref[0]);
    uint8_t fl_full[128];
    memcpy(fl_full, s_lc_snapped, (size_t)nb);
    const uint64_t dg_full = openpete_native_snap_digest(&sm);
    const ms_census cs_full = *cs;
    const int carry_ok = !s_store || mc_tab_span_take(s_store, &tm, &sp_full);
    openpete_native_snap_rewind(&sm);
    if (s_store) mc_tab_mark_back(s_store, &tm);
    s_sn_n = sn0; s_cr_pos = cr0;
    *cs = cs0;
    const int ok = ms_chunk_run(g, nb, box, view, cs, 1, 0);
    s_ck_rec = NULL;
    s_lc_serial = 0;
    if (!ok_full || !ok) return ok;
    long bres = 0, flag = 0, planes = 0;
    for (int k = 0; k < nb; k++) {
        bres += foc_res_diff(&ref[k], &s_ck_res[k]);
        flag += fl_full[k] != s_lc_snapped[k];
        planes += s_ck_res[k].np > 0 ? s_ck_res[k].np : 0;
    }
    long bcarry = 0;
    if (s_store) bcarry = carry_ok && mc_tab_span_take(s_store, &tm, &sp_list) ? mc_tab_span_diff(&sp_full, &sp_list) : 1;
    const long bsnap = (openpete_native_snap_digest(&sm) != dg_full) + bcarry;
    const long bcen = cs_full.contacts - cs0.contacts != cs->contacts - cs0.contacts ||
                      cs_full.contacts_t4 - cs0.contacts_t4 != cs->contacts_t4 - cs0.contacts_t4 ||
                      cs_full.ovf - cs0.ovf != cs->ovf - cs0.ovf || cs_full.planes - cs0.planes != cs->planes - cs0.planes;
    if ((bres || bsnap || flag || bcen) && s_lcc.bad_res + s_lcc.bad_snap + s_lcc.bad_flag + s_lcc.bad_census < 8)
        fprintf(stderr, "[snap-listed-check MISMATCH key=%u leg=%d] mobys=%d results=%ld snap=%ld snapped=%ld census=%ld\n",
                openpete_feed_key_now(), s_rex, nb, bres, bsnap, flag, bcen);
    s_lcc.chunks[s_rex ? 1 : 0]++; s_lcc.mobys += nb; s_lcc.planes += planes;
    s_lcc.bad_res += bres; s_lcc.bad_snap += bsnap; s_lcc.bad_flag += flag; s_lcc.bad_census += bcen;
    s_lcc.tris[0] += cs_full.tris - cs0.tris; s_lcc.tris[1] += cs->tris - cs0.tris;
    s_lcc.walks[0] += cs_full.sectors - cs0.sectors; s_lcc.walks[1] += cs->sectors - cs0.sectors;
    return 1;
}

/** @brief Resolve one chunk of at most 128 queued mobys: capture the terrain their boxes reach, find each one's contact planes, add them to the table.
 * @param g The chunk's first queued moby.
 * @param nb Its moby count.
 * @param view H, ofx, ofy.
 * @param cs The census, updated (its clocks run when cs->timed).
 * @return 0 when the capture failed (the rest of the queue is dropped), else 1. */
static int ms_resolve_chunk(int g, int nb, const float view[3], ms_census* cs) {
    terrain_snap_box box[128];
    uint64_t tp = rsp_now();
    for (int k = 0; k < nb; k++) { ms_bounds_ensure(&s_q_mob[g + k]); ms_box(&s_q_mob[g + k], &box[k]); }
    tp = rsp_mark(RSP_BOX, tp);
    const int listed = ms_lc_on() && s_rs_wb;
    if (listed && ms_lc_check_on()) return ms_lc_check(g, nb, box, view, cs);
    const uint64_t c0 = s_rex && ms_rc_on() ? ms_cpu_ns() : 0;
    const int ok = ms_chunk_run(g, nb, box, view, cs, listed, tp);
    if (c0) s_pc.chunk_ns += ms_cpu_ns() - c0;
    return ok;
}

/** @brief Capture the terrain of one chunk's boxes, by the listed capture (ms_lc_capture) or the full walk, find each moby's contact planes and add them to the table.
 * @param g The chunk's first queued moby.
 * @param nb Its moby count.
 * @param box Its search boxes.
 * @param view H, ofx, ofy.
 * @param cs The census, updated (its clocks run when cs->timed).
 * @param listed 1 to take the listed capture.
 * @param tp The resolve part clock (rsp_now), 0 when off.
 * @return 0 when the capture failed (the rest of the queue is dropped), else 1. */
static int ms_chunk_run(int g, int nb, const terrain_snap_box* box, const float view[3], ms_census* cs,
                        int listed, uint64_t tp) {
    const terrain_snap_tri* tt = NULL;
    terrain_snap_census tc;
    const uint64_t c0 = cs->timed ? ms_cpu_ns() : 0;
    const int ntt = listed ? ms_lc_capture(g, nb, box, &tt, &tc) : terrain_snap_capture(box, nb, &tt, &tc);
    if (cs->timed) cs->cap_ns += ms_cpu_ns() - c0;
    tp = rsp_mark(RSP_CAPTURE, tp);
    if (ntt < 0) return 0;
    if (s_rex) { s_pc.captures++; s_pc.walked += tc.sectors; s_pc.seg_cap[ms_pc_seg()]++; }
    const uint64_t m0 = cs->timed ? ms_mono_ns() : 0;
    if (!ms_terrain_make(tt, ntt, view)) return 0;
    if (cs->timed) cs->near_ns += ms_mono_ns() - m0;
    tp = rsp_mark(RSP_TMAKE, tp);
    cs->faces += tc.faces; cs->tris += tc.tris; cs->sectors += tc.sectors;
    uint8_t snapped[128];
    int first[129];
    const int* cand = ms_cands(tt, ntt, nb, first);
    tp = rsp_mark(RSP_CANDS, tp);
    const int prep = s_store && s_rs_wb;
    const ms_fo_job J = { g, tt, cand, first, view, prep, prep && mc_refresh_on() && g + nb <= s_cb_n };
    if (s_lc_serial) { ms_fo_serial(&J, nb, cs, snapped); memcpy(s_lc_snapped, snapped, (size_t)nb); }
    else ms_contact(&J, nb, cs, snapped);
    tp = rsp_mark(RSP_CONTACT, tp);
    cs->overlaps += ms_overlaps(box, snapped, nb);
    for (int k = 0; k < nb; k++) if (snapped[k]) cs->zmin = ms_minf(cs->zmin, box[k].z0);
    rsp_mark(RSP_OVERLAP, tp);
    return 1;
}

/* ---- the screen-size bound (OPENPETE_SNAP_MIN_PX) ---------------------------
 * A pulled moby whose drawn height on screen is under a bound takes no snap:
 * no capture, no contact, no planes. The height is its queued corners'
 * screen extent (ms_moby.b), in PSX screen lines, 240 whatever the internal
 * resolution, supersampling or aspect, so a bound of p pixels at 1440 lines
 * is p * 240 / 1440 lines. The tick's own resolve decides for each moby
 * and keeps the decision by canonical epoch; an in-between scene takes the
 * decision of the endpoint the carry takes (alpha at or above 0.5 the
 * latest tick, below it the one before), and decides from its own height
 * only for a moby that endpoint did not queue. Hysteresis: a moby left out
 * at the previous tick stays out until its height reaches 1.25 times the
 * bound; "previous" is the table of canonical epoch ep - 1, and a tick whose
 * predecessor's table is not kept decides at the plain bound. Default 16 px,
 * 0 snaps every moby.
 *
 * The lean queue leaves ms_moby.b unset until the moby's chunk resolves, so
 * the filter computes a moby's bounds itself (ms_bounds_hold) where it needs
 * the height: at the tick, which resolves its whole queue and would compute
 * them in its chunks anyway, and in an in-between scene only for a moby the
 * endpoint did not decide. The census counts scenes, mobys considered and
 * left out per leg, and the in-between mobys whose bounds the filter
 * computed; the profiler takes the left-out count per resolve
 * (mcann.snap_small, mrexn.snap_small). */
#define MS_SMALL_BITS 11
#define MS_SMALL_N (1 << MS_SMALL_BITS)
typedef struct { uint32_t mid; uint8_t small; } ms_small_ent;
static ms_small_ent s_small[2][MS_SMALL_N];     /* the decisions of the two newest ticks, by epoch parity */
static uint32_t s_small_ep[2] = { ~0u, ~0u };   /* the canonical epoch each table holds */
static uint32_t s_small_last = ~0u;              /* the newest epoch decided */
/* The census: per leg (0 tick, 1 in-between) mobys considered and left out;
 * in-between mobys the endpoint had not decided, whose bounds the filter computed. */
static struct { long considered[2], out[2], bounded; } s_smc;

/** @brief The bound in PSX screen lines (OPENPETE_SNAP_MIN_PX, pixels at 1440 lines), read once; 0 when off.
 * @return The bound. */
static float ms_small_lines(void) {
    static float v = -1.0f;
    if (v < 0.0f) {
        const char* e = getenv("OPENPETE_SNAP_MIN_PX");
        v = (e && *e ? (float)atof(e) : 16.0f) * (240.0f / 1440.0f);
        if (v < 0.0f) v = 0.0f;
    }
    return v;
}

/** @brief Print the bound's run totals (bound set or OPENPETE_MOBY_RANK_TIME=1): the bound, scenes, and per leg mobys considered and left out, with the in-between mobys whose bounds the filter computed. */
static void ms_small_print(void) {
    fprintf(stderr, "[snap-min-px total] min_lines=%.3f scenes=%ld considered_tick=%ld out_tick=%ld "
                    "considered_scene=%ld out_scene=%ld bounded_scene=%ld\n",
            (double)ms_small_lines(), s_ms_scenes, s_smc.considered[0], s_smc.out[0],
            s_smc.considered[1], s_smc.out[1], s_smc.bounded);
}

/** @brief Find a moby's slot in a decision table: its entry, or the empty slot that would take it.
 * @param t The table.
 * @param mid The moby's identity.
 * @return The slot, or NULL when the table is full. */
static ms_small_ent* ms_small_slot(ms_small_ent* t, uint32_t mid) {
    unsigned h = (mid * 2654435761u) >> (32 - MS_SMALL_BITS);
    for (int n = 0; n < MS_SMALL_N; n++, h = (h + 1) & (MS_SMALL_N - 1))
        if (t[h].mid == mid || t[h].mid == 0) return &t[h];
    return NULL;
}

/** @brief A tick's decision for a moby: 1 left out, 0 snapped, -1 when that tick did not queue it or is not kept.
 * @param ep The canonical epoch.
 * @param mid The moby's identity.
 * @return The decision. */
static int ms_small_at(uint32_t ep, uint32_t mid) {
    if (ep == ~0u || s_small_ep[ep & 1u] != ep) return -1;
    const ms_small_ent* e = ms_small_slot(s_small[ep & 1u], mid);
    return e && e->mid == mid ? e->small : -1;
}

/** @brief Make a queued moby's bounds hold for the filter, computing them when the lean queue deferred them.
 *
 * The chunk's ms_bounds_ensure then finds them held and computes nothing.
 * @param m The moby. */
static void ms_bounds_hold(ms_moby* m) {
    if (m->bok) return;
    ms_bounds_tris(&s_q_tri[m->t0], &s_q_bnd[m->t0], m->nt, m->b);
    m->bok = 1;
}

/** @brief Whether one queued moby is left out of the snap in the resolve under way, recording the tick's decision on its own resolve.
 * @param mb The moby; its bounds are computed when the decision needs its height.
 * @param ml The bound in lines.
 * @param ep The tick's canonical epoch (its own resolve) or the endpoint's (an in-between scene).
 * @return 1 when left out. */
static int ms_small_decide(ms_moby* mb, float ml, uint32_t ep) {
    if (s_rex) {
        const int d = ms_small_at(ep, mb->mid);
        if (d >= 0) return d;
        s_smc.bounded++;
        ms_bounds_hold(mb);
        return mb->b[3] - mb->b[1] < ml;
    }
    ms_bounds_hold(mb);
    const float h = mb->b[3] - mb->b[1];
    const int small = ms_small_at(ep - 1u, mb->mid) == 1 ? h < 1.25f * ml : h < ml;
    ms_small_ent* e = ms_small_slot(s_small[ep & 1u], mb->mid);
    if (e) { e->mid = mb->mid; e->small = (uint8_t)small; }
    return small;
}

/** @brief Leave the mobys the screen-size bound excludes out of the queue, keeping the rest in order. */
static void ms_small_filter(void) OP_REQUIRES(sim) {
    const float ml = ms_small_lines();
    if (ml <= 0.0f) return;
    static int s_pid[2] = { -1, -1 };
    if (s_pid[0] < 0) {
        s_pid[0] = op_prof_intern("mcann.snap_small");
        s_pid[1] = op_prof_intern("mrexn.snap_small");
    }
    uint32_t ep;
    if (!s_rex) {
        ep = spyro_native_canon_epoch_stamp();
        if (s_small_ep[ep & 1u] != ep) { memset(s_small[ep & 1u], 0, sizeof s_small[0]); s_small_ep[ep & 1u] = ep; }
        s_small_last = ep;
    } else {
        ep = s_rex_alpha >= 0.5 || s_small_last == ~0u ? s_small_last : s_small_last - 1u;
    }
    const int leg = s_rex ? 1 : 0;
    int n = 0;
    for (int k = 0; k < s_q_nm; k++) {
        if (ms_small_decide(&s_q_mob[k], ml, ep)) continue;
        s_q_mob[n++] = s_q_mob[k];
    }
    s_smc.considered[leg] += s_q_nm;
    s_smc.out[leg] += s_q_nm - n;
    if (op_prof_on()) op_prof_count(s_pid[leg], (uint64_t)(s_q_nm - n));
    s_q_nm = n;
}

/** @brief Resolve the queue when it and the last terrain build belong to the scene now accumulating.
 *
 * Mobys go to the capture in groups of up to 128 (one bit each in the hit
 * sets); the queue empties afterwards. The carry (ms_carry_begin) may place
 * some mobys' planes first and leave the capture the rest. */
static void ms_resolve(void) OP_REQUIRES(sim) {
    if (!s_q_nm || s_q_epoch != openpete_native_scene_epoch() || !terrain_snap_ready()) return;
    if (s_rex) { s_pc.resolves++; s_pc.seg_res[ms_pc_seg()]++; }
    const uint64_t t0 = ms_census_on() ? ms_cpu_ns() : 0;
    ms_census cs; memset(&cs, 0, sizeof cs);
    cs.zmin = 1e30f;
    cs.timed = t0 != 0;
    static int s_small_atexit;
    if (!s_small_atexit && (t0 || ms_small_lines() > 0.0f)) { s_small_atexit = 1; atexit(ms_small_print); }
    float view[3];
    terrain_snap_view(&view[0], &view[1], &view[2]);
    if (view[0] > 0.0f) {
        openpete_native_snap_view(view[0], view[1], view[2]);
        ms_up_sync();
        memcpy(s_rs_view, view, sizeof s_rs_view);
        uint64_t tp = rsp_now();
        ms_small_filter();
        const int nrun = ms_carry_begin(&cs);
        rsp_mark(RSP_CARRY, tp);
        for (int g = 0; g < nrun; g += 128)
            if (!ms_resolve_chunk(g, nrun - g < 128 ? nrun - g : 128, view, &cs)) break;
        tp = rsp_now();
        ms_cr_flush(INT_MAX, &cs);
        ms_carry_end(&cs);
        rsp_mark(RSP_END, tp);
        rsp_flush();
    }
    cs.pulled = s_q_nm;
    s_q_nm = s_q_nt = 0;
    if (t0) ms_census_print(&cs, ms_cpu_ns() - t0);
    if (t0 && ms_small_lines() > 0.0f)
        fprintf(stderr, "[moby-snap-small] considered=%ld out_tick=%ld out_scene=%ld\n",
                s_smc.considered[0] + s_smc.considered[1], s_smc.out[0], s_smc.out[1]);
}

/** @brief Run ms_resolve inside the profiler scope of its leg: mrex.resolve in a sub-tick re-extract, else mcan.resolve. */
static void ms_resolve_scoped(void) OP_REQUIRES(sim) {
    const uint64_t c0 = s_rex && ms_rc_on() ? ms_cpu_ns() : 0;
    if (s_rex) { OP_PROF_ACCUM_SCOPE("mrex.resolve"); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_SNAP_REX); ms_resolve(); }
    else       { OP_PROF_ACCUM_SCOPE("mcan.resolve"); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_SNAP_CANON); ms_resolve(); }
    if (c0) s_pc.res_ns += ms_cpu_ns() - c0;
}

/* ---- one resolve per sub-tick present ------------------------------------------
 * A sub-tick re-extract feeds the terrain before the mobys, so the queue of
 * each walk pass could resolve as soon as the pass pushed; instead every
 * pass of the present queues (moby_snap_moby_fed counts the passes) and one
 * resolve runs when the re-extract's moby passes end (moby_snap_present),
 * as the canonical leg resolves once after its terrain feed. Its result is
 * the per-pass resolves' table word for word: no stage reads the table
 * before the scene is composed; the resolve reads only the queue, the
 * terrain build and the carry tables, which no walk pass writes; each moby
 * resolves from its own triangles and the terrain triangles its search box
 * reaches, in capture order, whichever mobys share its chunk; and the table
 * takes each pass's carried mobys and then its fresh ones (ms_carry_add,
 * ms_add_fresh), the order the per-pass resolves added them, so the entry
 * order, the plane runs and the cap refusals are theirs. A moby two passes
 * queued stays two queue entries with their own ordinals, as before.
 * OPENPETE_SNAP_REX_MERGE=0 resolves per pass; OPENPETE_SNAP_REX_MERGE_CHECK=1
 * runs both on every present and compares (ms_rm_check). */

/** @brief Whether one resolve serves a sub-tick present (OPENPETE_SNAP_REX_MERGE not 0), read once.
 * @return 1 when on. */
static int ms_rm_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SNAP_REX_MERGE"); v = !(e && *e == '0'); }
    return v;
}

/** @brief The merge check's mode (OPENPETE_SNAP_REX_MERGE_CHECK), read once: 0 off, 1 compare, 2 compare with the single resolve adding every carried moby after the fresh ones (the calibration: a pass order the check must refuse).
 * @return The mode. */
static int ms_rm_check_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SNAP_REX_MERGE_CHECK"); v = e && (*e == '1' || *e == '2') ? *e - '0' : 0; }
    return v;
}

/* The merge check's totals: presents compared, those where two or more walk
 * passes queued, mobys, table entries and planes compared, mismatches of the
 * packed table words and of the insertion-order digest, and queued mobys
 * whose identity another walk pass of the present also queued. */
static struct { long presents, multi, mobys, ents, planes, bad_words, bad_digest, skipped, cross; } s_rmc;

/** @brief Print the merge check's totals. */
static void ms_rm_check_print(void) {
    fprintf(stderr, "[snap-rex-merge-check] presents=%ld presents_multi=%ld mobys=%ld entries=%ld planes=%ld "
                    "mismatched_words=%ld mismatched_digest=%ld skipped=%ld cross_pass_mobys=%ld\n",
            s_rmc.presents, s_rmc.multi, s_rmc.mobys, s_rmc.ents, s_rmc.planes, s_rmc.bad_words,
            s_rmc.bad_digest, s_rmc.skipped, s_rmc.cross);
}

/** @brief Count a saved queue's walk passes and the mobys whose identity another of its passes also queued.
 * @param q The queue.
 * @param nm Its moby count.
 * @param cross Receives the count of such mobys.
 * @return The pass count: the largest seg plus one. */
static int ms_rm_passes(const ms_moby* q, int nm, long* cross) {
    int nseg = 0;
    *cross = 0;
    for (int k = 0; k < nm; k++) {
        if (q[k].seg + 1 > nseg) nseg = q[k].seg + 1;
        for (int j = 0; j < nm; j++)
            if (q[j].mid == q[k].mid && q[j].seg != q[k].seg) { (*cross)++; break; }
    }
    return nseg;
}

/** @brief Resolve a saved queue one walk pass at a time: each pass's mobys, in queue order, alone at the front of the queue.
 * @param q The queue.
 * @param nm Its moby count.
 * @param nt Its triangle count.
 * @param nseg Its pass count. */
static void ms_rm_per_pass(const ms_moby* q, int nm, int nt, int nseg) OP_REQUIRES(sim) {
    for (int seg = 0; seg < nseg; seg++) {
        int n = 0;
        for (int k = 0; k < nm; k++) if (q[k].seg == seg) s_q_mob[n++] = q[k];
        if (!n) continue;
        s_q_nm = n; s_q_nt = nt;
        ms_resolve_scoped();
    }
}

/** @brief Resolve the present's queue pass by pass and then as one, and compare the snap tables.
 *
 * The per-pass leg moves each walk pass's mobys to the front of the queue in
 * queue order and resolves them alone; the table it leaves is packed
 * (openpete_native_snap_pack_live) and digested in insertion order
 * (openpete_native_snap_digest); the table, the ordinal census and the
 * present's counts then return to their marks and the whole queue resolves
 * once. Compared: the packed words and the digests. The single resolve's
 * table is kept; under mode 2 it is the calibration's. The per-pass leg's resolves, captures and walks are not
 * counted in the present's census. */
static void ms_rm_check(void) OP_REQUIRES(sim) {
    static int s_atexit;
    static ms_moby* q; static int qcap;
    enum { W_CAP = (3 + 128 + 2 * 2048) * 4 };
    static uint32_t wa[W_CAP], wb[W_CAP];
    if (!s_atexit) { s_atexit = 1; atexit(ms_rm_check_print); }
    ms_scene_sync();
    const int nm = s_q_nm, nt = s_q_nt;
    if (!nm || !ms_grow((void**)&q, &qcap, nm, sizeof *q)) { s_rmc.skipped += nm > 0; ms_resolve_scoped(); return; }
    memcpy(q, s_q_mob, (size_t)nm * sizeof *q);
    openpete_native_snap_mark_t sm;
    openpete_native_snap_mark(&sm);
    const int sn0 = s_sn_n;
    const ms_pc pc0 = s_pc;
    long cross;
    const int nseg = ms_rm_passes(q, nm, &cross);
    s_rmc.cross += cross;
    ms_rm_per_pass(q, nm, nt, nseg);
    const int na = openpete_native_snap_pack_live(wa, W_CAP);
    const uint64_t da = openpete_native_snap_digest(&sm);
    openpete_native_snap_rewind(&sm);
    s_sn_n = sn0;
    s_pc = pc0;
    memcpy(s_q_mob, q, (size_t)nm * sizeof *q);
    s_q_nm = nm; s_q_nt = nt;
    s_cr_late = ms_rm_check_on() == 2;
    ms_resolve_scoped();
    s_cr_late = 0;
    const int nb = openpete_native_snap_pack_live(wb, W_CAP);
    const long bw = na != nb || na < 0 || memcmp(wa, wb, (size_t)na * sizeof wa[0]) != 0;
    const long bd = openpete_native_snap_digest(&sm) != da;
    if ((bw || bd) && s_rmc.bad_words + s_rmc.bad_digest < 8)
        fprintf(stderr, "[snap-rex-merge-check MISMATCH key=%u] mobys=%d passes=%d words=%d/%d digest=%ld\n",
                openpete_feed_key_now(), nm, nseg, na, nb, bd);
    s_rmc.presents++; s_rmc.multi += s_pc.passes >= 2; s_rmc.mobys += nm;
    s_rmc.ents += nb > 0 ? (long)wb[8] : 0; s_rmc.planes += nb > 0 ? (long)wb[9] : 0;
    s_rmc.bad_words += bw; s_rmc.bad_digest += bd;
}

/* The sub-tick census's run totals (OPENPETE_SNAP_REX_CENSUS): presents,
 * those that queued a moby and those where two or more passes queued, and
 * the sums of ms_pc over them. */
static struct { long presents, queued_presents, multi; ms_pc sum; } s_rc;

/** @brief Print the sub-tick census's totals and per-present means. */
static void ms_rc_print(void) {
    const double n = s_rc.queued_presents > 0 ? (double)s_rc.queued_presents : 1.0;
    const ms_pc* p = &s_rc.sum;
    fprintf(stderr, "[snap-rex-census] merge=%d presents=%ld queued_presents=%ld multi_pass=%ld per_queued_present: "
                    "passes=%.2f queued=%.2f resolves=%.2f captures=%.2f walked=%.2f carried=%.2f fresh=%.2f "
                    "refused=%.2f refused_fs=%.2f absent=%.2f resolve_us=%.1f chunk_us=%.1f seg_queued=",
            ms_rm_on(), s_rc.presents, s_rc.queued_presents, s_rc.multi, p->passes / n, p->queued / n,
            p->resolves / n, p->captures / n, p->walked / n, p->carried / n, p->fresh / n, p->refreshed / n,
            p->fsref / n, (p->fresh - p->refreshed) / n, (double)p->res_ns / 1000.0 / n,
            (double)p->chunk_ns / 1000.0 / n);
    for (int k = 0; k < MS_PC_SEGS; k++) fprintf(stderr, "%s%.2f", k ? "/" : "", p->seg_q[k] / n);
    fprintf(stderr, " seg_resolves=");
    for (int k = 0; k < MS_PC_SEGS; k++) fprintf(stderr, "%s%.2f", k ? "/" : "", p->seg_res[k] / n);
    fprintf(stderr, " seg_captures=");
    for (int k = 0; k < MS_PC_SEGS; k++) fprintf(stderr, "%s%.2f", k ? "/" : "", p->seg_cap[k] / n);
    fputc('\n', stderr);
}

/** @brief Add the present's counts to the census totals (OPENPETE_SNAP_REX_CENSUS). */
static void ms_rc_add(void) {
    static int s_atexit;
    if (!s_atexit) { s_atexit = 1; atexit(ms_rc_print); }
    s_rc.presents++;
    if (!s_pc.queued) return;
    s_rc.queued_presents++; s_rc.multi += s_pc.passes >= 2;
    ms_pc* d = &s_rc.sum;
    const long* a[] = { &s_pc.carried, &s_pc.fresh, &s_pc.refreshed, &s_pc.fsref, &s_pc.resolves,
                        &s_pc.captures, &s_pc.walked, &s_pc.passes, &s_pc.queued };
    long* b[] = { &d->carried, &d->fresh, &d->refreshed, &d->fsref, &d->resolves,
                  &d->captures, &d->walked, &d->passes, &d->queued };
    for (size_t k = 0; k < sizeof a / sizeof a[0]; k++) *b[k] += *a[k];
    d->res_ns += s_pc.res_ns; d->chunk_ns += s_pc.chunk_ns;
    for (int k = 0; k < MS_PC_SEGS; k++) {
        d->seg_q[k] += s_pc.seg_q[k]; d->seg_res[k] += s_pc.seg_res[k]; d->seg_cap[k] += s_pc.seg_cap[k];
    }
}

/** @brief Emit the present's snap counts to the profiler: mobys carried, fresh, refused (refreshed) and refused on a face-verdict change; resolves, terrain captures, cache-owned sectors walked and walk passes that queued. */
static void ms_pc_emit(void) {
    static int id[8] = { -1 };
    if (id[0] < 0) {
        static const char* const k_name[8] = { "mrexn.snap_carried", "mrexn.snap_fresh", "mrexn.snap_refreshed",
                                               "mrexn.snap_refreshed_fs", "mrexn.snap_resolves", "mrexn.snap_captures",
                                               "mrexn.snap_walked", "mrexn.snap_passes" };
        for (int k = 0; k < 8; k++) id[k] = op_prof_intern(k_name[k]);
    }
    const long v[8] = { s_pc.carried, s_pc.fresh, s_pc.refreshed, s_pc.fsref, s_pc.resolves, s_pc.captures,
                        s_pc.walked, s_pc.passes };
    for (int k = 0; k < 8; k++) op_prof_count(id[k], (uint64_t)v[k]);
}

/** @brief End a sub-tick re-extract's moby passes: resolve the present's queue once when the merge is on (ms_rm_on), then emit and total the present's counts. */
static void ms_rex_end(void) OP_REQUIRES(sim) {
    if (ms_rm_on()) {
        if (ms_rm_check_on()) ms_rm_check();
        else ms_resolve_scoped();
    }
    if (s_pc.queued && op_prof_on()) ms_pc_emit();
    if (ms_rc_on()) ms_rc_add();
}

void moby_snap_moby_fed(void) OP_REQUIRES(sim) {
    if (!s_rex) { ms_resolve_scoped(); return; }
    ms_scene_sync();
    const int nq = s_q_nm - s_pc_mark;
    if (nq > 0) { s_pc.passes++; s_pc.queued += nq; s_pc.seg_q[ms_pc_seg()] += nq; }
    if (!ms_rm_on()) ms_resolve_scoped();
    s_pc_mark = s_q_nm;
    s_q_seg++;
}

void moby_snap_terrain_fed(struct CPUState* cpu) OP_REQUIRES(sim) {
    s_vf_cpu = cpu;
    ms_resolve_scoped();
}
