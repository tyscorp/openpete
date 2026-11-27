// games/common/renderer/portal_spyro23.c : the homeworld portal previews of
// the two games whose portal pass and preview leaf are the same routines at
// different addresses. Each game's renderer/portal_game.c supplies the three
// addresses; the contract and the asm evidence are renderer/portal_spyro23.h.
//
// The game's portal pass gates each portal record, projects
// its five pentagon vertices with the sky matrix, writes the clip rect, the
// visible pentagon edges, the depth and the fade terms into the parameter
// block, and calls the preview leaf for the background quad and, when
// the portal is nearer than 0x5000 units, for the baked picture of the
// destination level. Both meshes are pre-projected 2-D triangle lists in
// level RAM: a 0x18-byte header {vertex count, vertex offset, colour count,
// colour offset, face count, face offset}, vertices as (y << 16 | x) << 5
// with the low five bits draw-owned scratch, colours 0x00BBGGRR, faces of two
// words (vertex indices in 12/10/10 bits; word 1 bit 31 flat with the colour
// index in bits 29-20, else three colour byte offsets). The leaf drops a face
// whose three vertices share a rect outcode or lie outside one pentagon edge,
// clips the rest to the edges, fades the colours toward the block's far
// colour by its IR0 and then toward the environment far colour by the sky
// fog word, and links the prims at OT index 3 + depth.
//
// This feeder hooks the leaf: it copies the block and the argument before
// the real leaf runs (the block is rewritten per portal), and after the pass
// re-derives every preview in float: the same outcode and half-plane
// rejects, a Sutherland-Hodgman clip to the pentagon and the two colour
// fades. The pentagon, its bounds and the opening plane come from the portal
// record the pass drew, projected under the view without the pass's 4:3
// clamp (p3_native_opening), so the preview reaches the edges of a wider
// frame. A record the pass rejects only because its pentagon lies off the
// 4:3 frame's sides is re-gated natively from game state and added to the
// tick's previews when it falls inside the wider frame (p3_band_admit); under
// extended LOD a record past its draw radius is re-gated the same way at every
// aspect. The opening goes out as the portal's stencil mask and the picture
// as portal-cyclorama triangles. Inside the pentagon both test at the
// opening plane's depth minus the plane-cover margin, so terrain in front of
// the opening occludes the picture per pixel and terrain behind it does not.
// The pentagon is the game's approximation of the frame's hole; a band
// P3_BAND_W wide outside it, in the opening plane, tests just behind the
// plane, so the picture reaches the frame's stone wherever the hole is wider
// than the pentagon and loses to any stone at or in front of the plane. The
// terrain feeder marks the faces lying in the opening plane at the pentagon
// (the inner rim), and their fragments inside the opening outline are
// discarded (portal_cover_scene.glsl). The picture is
// authored in the 4:3 frame and shifts by the frame's centre offset. Sub-tick
// presents replay the captured previews; a preview that has a partner in the
// previous tick (same mesh and leaf argument) has its rect, edges, depth and
// fade terms lerped between the two ticks, and its opening is re-derived
// under the present's view.
//
// Never writes PSX RAM: the real pass and leaf run unchanged, and the feeder
// only reads afterwards.

#include "feature_registry.h"
#include "openpete.h"          /* openpete_options_t */
#include "psx_runtime_internal.h"
#include "openpete_psx.h"
#include "openpete_native_render.h"
#include "openpete_recomp_module.h"
#include "openpete_fiber.h"           /* openpete_frame */
#include "renderer/render_paths.h"
#include "renderer/hook_bracket.h"   /* hook_reextract_ready_held */
#include "renderer/cut_guard.h"      /* cut_held_sets_t: the held cut's tick N-1 set */
#include "renderer/native_gte.h"
#include "renderer/portal_spyro23.h"
#include "renderer/portal_plane.h"     /* openpete_portal_plane_margin */
#include "renderer/terrain.h"          /* terrain_native_flush_deferred */
#include "openpete_pace_log.h"         /* OP_PACE_SPLIT_SCOPE: the guest calls' split */
#include "openpete_profile.h"          /* OP_PROF_ACCUM_SCOPE */
#include "renderer/portal_sky_s23.h"   /* portal_sky_s23_extension */
#include "no_lod.h"                    /* openpete_no_lod */
#include "game_layout.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"

#define P3_MAX_PORTALS 16
#define P3_MAX_EDGES   8
#define P3_MAX_VERTS   1024
#define P3_MAX_COLS    256

typedef struct {
    uint32_t mesh;                 /* the preview mesh header vaddr */
    int      arg;                  /* 1 = background record, 0 = the +0x3C picture */
    int      ymin, ymax, xmin, xmax;
    int      depth7;               /* portal depth >> 7, as captured */
    float    depth_w;              /* the same depth in world units, lerpable */
    uint32_t far_rgb;              /* packed 8-bit far colour */
    int      ir0;                  /* portal fade factor */
    int      nedge;
    float    ex[P3_MAX_EDGES][2], ey[P3_MAX_EDGES][2];
    int      slot;                 /* portal index within the pass */
    uint32_t rec;                  /* the portal record the pass drew, 0 when unidentified */
} p3_preview;

typedef struct {
    p3_preview p[P3_MAX_PORTALS * 2];
    int n;
    unsigned frame;
    int valid;
} p3_tick;

static p3_tick s_curr, s_prev;

/** @brief Exchange the current tick's captures with the previous tick's. */
static void p3_sets_swap(void) {
    const p3_tick t = s_curr; s_curr = s_prev; s_prev = t;
}

/* The two captured ticks keyed by canonical epoch: the pass's first run in an
 * epoch retires the current tick into s_prev, and a held camera cut shows the
 * tick N-1 set (cut_held_sets_enter). */
static cut_held_sets_t s_sets = { p3_sets_swap, &s_curr.n, "portal", { 0u, 0u }, 0u };
static int s_pick = -1;   /* the set a held present shows (cut_held_pick_t), -1 outside a hold */
static int s_in_pass = 0;
static uint32_t s_rec_next = 0;   /* the first record index the next background capture may name */
static int s_leaf_calls = 0;      /* engagement: leaf hooks seen this tick */
static unsigned s_sky_fed_frame = 0;  /* the last present a preview actually emitted */
static uint32_t s_pass_start = 0;

/** @brief Whether OPENPETE_PORTAL3_DBG selects the portal census and dump output.
 *
 * Latched on the first call; set, non-empty and not starting with '0' enables
 * it.
 * @return 1 when enabled, else 0. */
static int p3_dbg(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_DBG"); v = (e && *e && *e != '0'); }
    return v;
}
/** @brief The first feed-key value the OPENPETE_PORTAL3_DBG reports print from, read from OPENPETE_PORTAL3_DBG_FROM.
 *
 * Latched on the first call; unset reads 0. The value parses in any base
 * strtoul accepts.
 * @return The VBL. */
static unsigned p3_dbg_from(void) {
    static unsigned v = 0; static int init = 0;
    if (!init) { const char* e = getenv("OPENPETE_PORTAL3_DBG_FROM"); v = e ? (unsigned)strtoul(e, NULL, 0) : 0u; init = 1; }
    return v;
}
static int s_cen_faces, s_cen_idx, s_cen_oc, s_cen_edge, s_cen_tris;
static unsigned s_off_feed, s_off_plane;  /* off-screen openings skipped, run totals */
static unsigned s_off_rows;               /* of the feed's, the ones inside the view but outside the pass's rows */

/** @brief Whether the sub-tick replay lerps previews, off when OPENPETE_PORTAL3_INTERP starts with 0.
 *
 * Latched on the first call. Off, every preview replays at the state its tick
 * captured, with no lerp toward the previous tick.
 * @return 1 when on, else 0. */
static int p3_interp_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_INTERP"); v = !(e && *e == '0'); }
    return v;
}

/* What the census names the feed that is running: the canonical pass, or the
 * sub-tick replay at its alpha. */
static const char* s_feed_src = "canon";
static float s_feed_alpha = 1.0f;

/* ---- capture ---------------------------------------------------------- */
#define P3_REC_STRIDE 0x44u

/** @brief The portal record a background capture belongs to, found by its preview mesh.
 *
 * The pass visits the records in index order and loads the block's mesh
 * pointer from the record's +0x3C, so the record is the first one at or after
 * s_rec_next whose +0x3C equals the block's +0x10. Advances s_rec_next past
 * it, so two portals previewing one destination resolve to two records.
 * @param cpu Reads the record array.
 * @param mesh The block's +0x10 word.
 * @return The record address, or 0 when no record from s_rec_next holds the mesh. */
static uint32_t p3_find_record(CPUState* cpu, uint32_t mesh) {
    if (!portal_game_s23.records || !portal_game_s23.count) return 0;
    const uint32_t base = cpu->read_word(portal_game_s23.records);
    const uint32_t n = cpu->read_word(portal_game_s23.count);
    if (base < 0x80000000u || base >= 0x80200000u || n > 256u) return 0;
    for (uint32_t i = s_rec_next; i < n; i++) {
        const uint32_t r = base + i * P3_REC_STRIDE;
        if (cpu->read_word(r + 0x3C) == mesh) { s_rec_next = i + 1; return r; }
    }
    return 0;
}

/** @brief Copies the pass's parameter block for one leaf call into the tick's preview list.
 *
 * Called before the leaf runs, after the pass rewrote the block for this
 * portal. Reads the clip rect (+0x00..+0x0C), the mesh header (+0x10, plus
 * 0x3C for the picture call), the depth >> 7 (+0x14), the far colour from
 * three halfwords >> 4 (+0x18..+0x1C), IR0 (+0x1E) and up to P3_MAX_EDGES
 * pentagon edges (count at +0x20, two packed int16 xy words each from +0x24).
 * A picture call following its portal's background call shares its slot and
 * record; every other call opens the next slot and names its record through
 * p3_find_record. Nothing is captured once the list holds P3_MAX_PORTALS * 2
 * previews.
 * @param cpu Reads the block.
 * @param arg The leaf's argument: 1 for the background record, 0 for the picture. */
static void p3_capture(CPUState* cpu, int arg) {
    if (s_curr.n >= P3_MAX_PORTALS * 2) return;
    p3_preview* p = &s_curr.p[s_curr.n];
    memset(p, 0, sizeof *p);
    uint32_t B = portal_game_s23.block;
    p->ymin = (int32_t)cpu->read_word(B + 0x00);
    p->ymax = (int32_t)cpu->read_word(B + 0x04);
    p->xmin = (int32_t)cpu->read_word(B + 0x08);
    p->xmax = (int32_t)cpu->read_word(B + 0x0C);
    p->mesh = cpu->read_word(B + 0x10);
    p->arg  = arg;
    if (arg == 0) p->mesh += 0x3C;
    p->depth7 = (int32_t)cpu->read_word(B + 0x14);
    p->depth_w = (float)(p->depth7 << 7);
    uint32_t r = (cpu->read_half(B + 0x18) >> 4) & 0xFF, g = (cpu->read_half(B + 0x1A) >> 4) & 0xFF,
             b = (cpu->read_half(B + 0x1C) >> 4) & 0xFF;
    p->far_rgb = r | (g << 8) | (b << 16);
    p->ir0 = (int16_t)cpu->read_half(B + 0x1E);
    int ne = (int32_t)cpu->read_word(B + 0x20);
    if (ne < 0) ne = 0;
    if (ne > P3_MAX_EDGES) ne = P3_MAX_EDGES;
    p->nedge = ne;
    for (int i = 0; i < ne; i++) {
        uint32_t a = cpu->read_word(B + 0x24 + i * 8), b2 = cpu->read_word(B + 0x28 + i * 8);
        p->ex[i][0] = (float)(int16_t)(a & 0xFFFF);  p->ey[i][0] = (float)(int16_t)(a >> 16);
        p->ex[i][1] = (float)(int16_t)(b2 & 0xFFFF); p->ey[i][1] = (float)(int16_t)(b2 >> 16);
    }
    /* Portal slot: the leaf pair (1 then 0) belongs to one portal. */
    const int pairs = s_curr.n > 0 && s_curr.p[s_curr.n - 1].arg == 1 && arg == 0;
    p->slot = (s_curr.n == 0) ? 0 : (pairs ? s_curr.p[s_curr.n - 1].slot
                                           : s_curr.p[s_curr.n - 1].slot + 1);
    p->rec = pairs ? s_curr.p[s_curr.n - 1].rec : p3_find_record(cpu, cpu->read_word(B + 0x10));
    s_curr.n++;
}

/* ---- extraction ------------------------------------------------------ */
typedef struct { float x, y; uint32_t rgb; } p3_v;

/** @brief The signed cross product of edge a->b with the vector a->v.
 *
 * Computes (bx - ax) * (vy - ay) - (by - ay) * (vx - ax). A value of 0 or
 * more is the inside of a pentagon edge; the callers reject below 0.
 * @param ax The edge's first X.
 * @param ay The edge's first Y.
 * @param bx The edge's second X.
 * @param by The edge's second Y.
 * @param vx The tested point's X.
 * @param vy The tested point's Y.
 * @return The cross product; the test treats a value below 0 as outside. */
static float p3_cross(float ax, float ay, float bx, float by, float vx, float vy) {
    return (bx - ax) * (vy - ay) - (by - ay) * (vx - ax);
}

/** @brief Clips a polygon to the inside half-plane (cross >= 0) of one edge, Sutherland-Hodgman.
 *
 * A crossing vertex takes the position and per-channel colour lerped at the
 * crossing, rounded and clamped to a byte. Output stops once it holds 30
 * vertices, so `out` needs room for 31.
 * @param in The polygon.
 * @param n Its vertex count.
 * @param out Receives the clipped polygon.
 * @param ax The edge's first X.
 * @param ay The edge's first Y.
 * @param bx The edge's second X.
 * @param by The edge's second Y.
 * @return The clipped vertex count. */
static int p3_clip_edge(const p3_v* in, int n, p3_v* out, float ax, float ay, float bx, float by) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const p3_v *cur = &in[i], *nxt = &in[(i + 1) % n];
        float dc = p3_cross(ax, ay, bx, by, cur->x, cur->y);
        float dn = p3_cross(ax, ay, bx, by, nxt->x, nxt->y);
        int ci = dc >= 0.0f, ni = dn >= 0.0f;
        if (ci) out[m++] = *cur;
        if (ci != ni) {
            float t = dc / (dc - dn);
            p3_v v;
            v.x = cur->x + (nxt->x - cur->x) * t;
            v.y = cur->y + (nxt->y - cur->y) * t;
            uint32_t c = 0;
            for (int k = 0; k < 3; k++) {
                float c0 = (float)((cur->rgb >> (k * 8)) & 0xFF), c1 = (float)((nxt->rgb >> (k * 8)) & 0xFF);
                int cv = (int)(c0 + (c1 - c0) * t + 0.5f);
                if (cv < 0) cv = 0; else if (cv > 255) cv = 255;
                c |= (uint32_t)cv << (k * 8);
            }
            v.rgb = c;
            out[m++] = v;
        }
        if (m >= 30) break;
    }
    return m;
}

/* The screen bounds a preview's colours are admitted within, in the 2-D
 * screen space the meshes are authored in.
 *
 * The pass's clip rect and pentagon edges are written for the 4:3 frame: the
 * rect is clamped to X [0,512] and rows [12,228], and an edge whose two
 * vertices share an outcode of func_8001C0F4 (X <= 0, X >= 512, row < 13,
 * row >= 228, SZ < 256) is not written at all. Under a wider frame both cut
 * the preview at the 4:3 sides. So the feeder re-derives the opening from the
 * portal record itself (p3_native_opening): the five world vertices through
 * the pass's own projection (func_8001C040: V = (camY - y, camZ - z,
 * x - camX), rotated by the camera matrix, divided about (256, 120)), with no
 * frame clamp, every edge kept and a vertex behind SZ 256 clipped at SZ 256
 * the way the pass intersects such an edge. Its bounds are the projected
 * pentagon's outward band (p3_dilate_polygon), clamped to the visible
 * screen-X range and the pass's rows; its edges are the pentagon's, at 4:3
 * the pass's own computed in float.
 *
 * The same derivation yields the opening plane, which the preview tests
 * against per pixel (p3_vertex_depth) instead of at the pass's single depth.
 *
 * A record the feeder cannot identify, or a pentagon wholly behind SZ 256,
 * falls back to the captured rect and edges: a rect spanning the whole 4:3
 * width with no edge cutting the frame is the camera standing inside the
 * portal, and its X bounds become the visible screen-X range.
 *
 * A pentagon that projects but whose bounds miss the view is off-screen and
 * draws nothing; it never falls back to the capture. The pass's rtps input is
 * 16-bit (func_8001C040), so a vertex far from the camera at shift 0 wraps and
 * saturates, and the pass writes a rect where the true pentagon is not, with
 * no edges to cut it. The view is the visible screen-X range and the visible
 * rows (openpete_native_visible_y_bounds) moved into the authored space; a
 * pentagon inside the view but outside the pass's rows is off-screen as well,
 * since the bounds clamp to those rows and nothing remains to paint. */
typedef struct { float xmin, xmax, ymin, ymax; int widened; } p3_bounds;

#define P3_FRAME_X0 0      /* the 4:3 framebuffer's screen-X range, the rect's */
#define P3_FRAME_X1 512    /* clamp and the authored frame quad's edges */
#define P3_FRAME_Y0 12     /* the rows the pass clamps the rect to */
#define P3_FRAME_Y1 228
#define P3_NEAR_SZ  256.0  /* func_8001C0F4's near outcode, where the pass clips an edge */
#define P3_CX       256.0  /* the screen centre the meshes are authored about */
#define P3_BAND_W   32.0   /* the outline's outward band in the opening plane, world units */
#define P3_BAND_EPS 24.0   /* the band's test depth behind the opening plane, world units */
#define P3_CY       120.0

/* A preview's opening re-derived from its portal record under the view being
 * presented, in the authored screen space. */
typedef struct {
    int      ok;                         /* 0: use the captured rect and edges */
    int      offscreen;                  /* nonzero: draw nothing; 1 outside the view, 2 inside it but outside the pass's rows */
    p3_bounds b;
    int      nedge;
    float    ex[P3_MAX_EDGES][2], ey[P3_MAX_EDGES][2];
    int      ndil;                       /* the outline dilated by P3_BAND_W in the plane */
    float    dx[P3_MAX_EDGES][2], dy[P3_MAX_EDGES][2];
    double   a, b_, c;                   /* 1/vz = a x + b_ y + c */
    double   far_vz;                     /* the farthest opening vertex's view-Z */
    double   V[5][3];                    /* the pentagon in view space, unclipped */
} p3_opening;

/** @brief Whether every corner of the 4:3 frame's width at the rect's rows is inside every pentagon edge.
 *
 * The corners are X P3_FRAME_X0 and P3_FRAME_X1 at the rect's ymin and ymax.
 * @param p The preview.
 * @return 1 when no edge cuts those corners, else 0. */
static int p3_edges_clear_frame(const p3_preview* p) {
    const float cx[4] = { (float)P3_FRAME_X0, (float)P3_FRAME_X1, (float)P3_FRAME_X1, (float)P3_FRAME_X0 };
    const float cy[4] = { (float)p->ymin, (float)p->ymin, (float)p->ymax, (float)p->ymax };
    for (int e = 0; e < p->nedge; e++)
        for (int k = 0; k < 4; k++)
            if (p3_cross(p->ex[e][0], p->ey[e][0], p->ex[e][1], p->ey[e][1], cx[k], cy[k]) < 0.0f)
                return 0;
    return 1;
}

/** @brief The screen bounds a preview's colours are admitted within.
 *
 * The re-derived opening's bounds when `op` holds one. Otherwise the captured
 * clip rect; when it spans the whole 4:3 frame width and no pentagon edge cuts
 * the frame's corners at its rows, the X bounds become the visible screen-X
 * range of the window and `widened` is set.
 * @param p The preview.
 * @param op The re-derived opening, or NULL.
 * @return The bounds. */
static p3_bounds p3_bounds_of(const p3_preview* p, const p3_opening* op) {
    if (op && op->ok) return op->b;
    p3_bounds b = { (float)p->xmin, (float)p->xmax, (float)p->ymin, (float)p->ymax, 0 };
    float xlo = 0.0f, xhi = 0.0f;
    openpete_native_widescreen_x_bounds(&xlo, &xhi);
    if (p->xmin <= P3_FRAME_X0 && p->xmax >= P3_FRAME_X1 && xlo < (float)P3_FRAME_X0 &&
        p3_edges_clear_frame(p)) {
        b.xmin = xlo; b.xmax = xhi; b.widened = 1;
    }
    return b;
}

/** @brief Reads a portal record's five pentagon vertices in world units, with the opening animation applied.
 *
 * Under portal_game_s23.anim and a nonzero +0x43 counter each vertex is moved
 * to m + ((v - m) * counter >> 8) per axis, m the vertex mean truncated toward
 * zero, as the pass does before projecting (func_8001EDEC.s:40-100,
 * 303-334).
 * @param cpu Reads the record.
 * @param rec The record address.
 * @param W Receives the vertices. */
static void p3_record_vertices(CPUState* cpu, uint32_t rec, double W[5][3]) {
    int32_t v[5][3], m[3] = { 0, 0, 0 };
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 3; k++) {
            v[i][k] = (int32_t)cpu->read_word(rec + (uint32_t)(i * 12 + k * 4));
            m[k] += v[i][k];
        }
    const int cnt = portal_game_s23.anim ? (int)cpu->read_byte(rec + 0x43) : 0;
    for (int k = 0; k < 3; k++) m[k] /= 5;
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 3; k++)
            W[i][k] = cnt ? (double)(m[k] + (((v[i][k] - m[k]) * cnt) >> 8)) : (double)v[i][k];
}

/** @brief Clips a view-space polygon to SZ >= P3_NEAR_SZ, Sutherland-Hodgman.
 * @param in The polygon, view-space (x, y, z).
 * @param n Its vertex count, at most 5.
 * @param out Receives the clipped polygon; room for 2 n vertices.
 * @return The clipped vertex count. */
static int p3_clip_near(const double in[][3], int n, double out[][3]) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const double* a = in[i];
        const double* b = in[(i + 1) % n];
        const int ai = a[2] >= P3_NEAR_SZ, bi = b[2] >= P3_NEAR_SZ;
        if (ai) { memcpy(out[m], a, sizeof(double) * 3); m++; }
        if (ai != bi) {
            const double t = (P3_NEAR_SZ - a[2]) / (b[2] - a[2]);
            for (int k = 0; k < 3; k++) out[m][k] = a[k] + (b[k] - a[k]) * t;
            m++;
        }
    }
    return m;
}

/** @brief The opening plane's screen-space 1/vz field from its view-space vertices.
 *
 * The normal is Newell's over the polygon, the plane n.P = k through the
 * vertex mean. Dividing by vz at sx = P3_CX + x H / z gives
 * 1/vz = (n_x (sx - P3_CX) / H + n_y (sy - P3_CY) / H + n_z) / k.
 * @param V The view-space vertices.
 * @param n Their count.
 * @param H The projection distance.
 * @param o Receives a, b_ and c.
 * @return 1 on success, 0 when the polygon is degenerate or the camera lies within 1e-3 of the plane. */
static int p3_plane_fit(const double V[][3], int n, double H, p3_opening* o) {
    double nx = 0.0, ny = 0.0, nz = 0.0, mx = 0.0, my = 0.0, mz = 0.0;
    for (int i = 0; i < n; i++) {
        const double* a = V[i];
        const double* b = V[(i + 1) % n];
        nx += (a[1] - b[1]) * (a[2] + b[2]);
        ny += (a[2] - b[2]) * (a[0] + b[0]);
        nz += (a[0] - b[0]) * (a[1] + b[1]);
        mx += a[0]; my += a[1]; mz += a[2];
    }
    const double nn = sqrt(nx * nx + ny * ny + nz * nz);
    if (nn < 1e-6 || H < 1.0) return 0;
    const double k = (nx * mx + ny * my + nz * mz) / (double)n;
    if (k > -1e-3 * nn && k < 1e-3 * nn) return 0;
    o->a  = nx / (H * k);
    o->b_ = ny / (H * k);
    o->c  = (nz - nx * P3_CX / H - ny * P3_CY / H) / k;
    return 1;
}

/** @brief Fills an edge list from a projected polygon, wound so the inside is cross >= 0.
 * @param x The projected X.
 * @param y The projected Y.
 * @param n The vertex count, at most P3_MAX_EDGES.
 * @param ex Receives each edge's two X.
 * @param ey Receives each edge's two Y. */
static void p3_polygon_edges(const float* x, const float* y, int n,
                             float ex[P3_MAX_EDGES][2], float ey[P3_MAX_EDGES][2]) {
    double area = 0.0;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        area += (double)x[i] * y[j] - (double)x[j] * y[i];
    }
    for (int i = 0; i < n; i++) {
        const int a = area >= 0.0 ? i : n - 1 - i;
        const int b = area >= 0.0 ? (i + 1) % n : (2 * n - 2 - i) % n;
        ex[i][0] = x[a]; ey[i][0] = y[a];
        ex[i][1] = x[b]; ey[i][1] = y[b];
    }
}

/** @brief Projects a view-space polygon, clipped at SZ P3_NEAR_SZ, into the authored screen space.
 * @param V The view-space polygon.
 * @param n Its vertex count, at most P3_MAX_EDGES / 2.
 * @param H The projection distance.
 * @param x Receives the projected X; room for P3_MAX_EDGES.
 * @param y Receives the projected Y; room for P3_MAX_EDGES.
 * @return The projected vertex count, 0 when fewer than 3 survive the clip. */
static int p3_project_polygon(const double V[][3], int n, double H, float* x, float* y) {
    double C[P3_MAX_EDGES][3];
    const int m = p3_clip_near(V, n, C);
    if (m < 3 || m > P3_MAX_EDGES) return 0;
    for (int i = 0; i < m; i++) {
        x[i] = (float)(P3_CX + C[i][0] * H / C[i][2]);
        y[i] = (float)(P3_CY + C[i][1] * H / C[i][2]);
    }
    return m;
}

/** @brief Dilates a planar convex view-space polygon outward by `w` within its own plane.
 *
 * Each edge moves out by `w` along the in-plane direction perpendicular to it
 * and away from the vertex mean; each vertex moves to the meeting point of its
 * two moved edges, V + w (u0 + u1) / (1 + u0.u1). Vertices closer than 1e-3
 * to their predecessor are dropped first, since a zero-length edge has no
 * direction (the pass's own pentagon repeats a vertex).
 * @param V The polygon.
 * @param n Its vertex count, at most 5.
 * @param w The band width in world units.
 * @param D Receives the dilated polygon.
 * @return The dilated vertex count, 0 when fewer than 3 distinct vertices remain or the polygon is degenerate. */
static int p3_dilate_polygon(const double V[][3], int n, double w, double D[][3]) {
    double P[5][3], m[3] = { 0, 0, 0 }, N[3] = { 0, 0, 0 };
    int k = 0;
    for (int i = 0; i < n; i++) {
        const double* q = V[i];
        if (k && fabs(q[0] - P[k - 1][0]) + fabs(q[1] - P[k - 1][1]) + fabs(q[2] - P[k - 1][2]) < 1e-3) continue;
        memcpy(P[k++], q, sizeof P[0]);
    }
    if (k >= 2 && fabs(P[0][0] - P[k - 1][0]) + fabs(P[0][1] - P[k - 1][1]) + fabs(P[0][2] - P[k - 1][2]) < 1e-3) k--;
    if (k < 3) return 0;
    for (int i = 0; i < k; i++) {
        const double* a = P[i];
        const double* b = P[(i + 1) % k];
        N[0] += (a[1] - b[1]) * (a[2] + b[2]);
        N[1] += (a[2] - b[2]) * (a[0] + b[0]);
        N[2] += (a[0] - b[0]) * (a[1] + b[1]);
        for (int c = 0; c < 3; c++) m[c] += a[c] / (double)k;
    }
    double U[5][3];
    for (int i = 0; i < k; i++) {
        const double* a = P[i];
        const double* b = P[(i + 1) % k];
        const double d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        double u[3] = { N[1] * d[2] - N[2] * d[1], N[2] * d[0] - N[0] * d[2], N[0] * d[1] - N[1] * d[0] };
        const double ul = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (ul < 1e-9) return 0;
        const double out = (a[0] + b[0]) * 0.5 - m[0], oy = (a[1] + b[1]) * 0.5 - m[1], oz = (a[2] + b[2]) * 0.5 - m[2];
        const double sgn = (u[0] * out + u[1] * oy + u[2] * oz) < 0.0 ? -1.0 : 1.0;
        for (int c = 0; c < 3; c++) U[i][c] = sgn * u[c] / ul;
    }
    for (int i = 0; i < k; i++) {
        const double* u0 = U[(i + k - 1) % k];
        const double* u1 = U[i];
        const double dot = u0[0] * u1[0] + u0[1] * u1[1] + u0[2] * u1[2];
        const double f = dot > -0.9 ? w / (1.0 + dot) : w;
        for (int c = 0; c < 3; c++) D[i][c] = P[i][c] + f * (u0[c] + u1[c]);
    }
    return k;
}

/** @brief Re-derives a preview's opening from its portal record under the view `v`.
 *
 * Projects the record's pentagon as func_8001C040 does, in float with no
 * frame clamp (see the bounds note above), and fits the opening plane.
 * Reads PSX RAM only.
 * @param cpu Reads the record.
 * @param v The view; supplies the camera, its matrix and H.
 * @param p The preview; its record is used.
 * @param o Receives the opening; ok is 0 when the record is unknown, the
 *        pentagon lies wholly behind SZ 256, or the bounds are empty, and
 *        offscreen is nonzero in the last case. */
static void p3_native_opening(CPUState* cpu, const ngte_view* v, const p3_preview* p, p3_opening* o) {
    memset(o, 0, sizeof *o);
    if (!p->rec) return;
    double W[5][3], V[5][3], D[5][3];
    p3_record_vertices(cpu, p->rec, W);
    for (int i = 0; i < 5; i++)
        ngte_mac(v->cam_M, v->cam[1] - W[i][1], v->cam[2] - W[i][2], W[i][0] - v->cam[0],
                 &V[i][0], &V[i][1], &V[i][2]);
    memcpy(o->V, V, sizeof o->V);
    float x[P3_MAX_EDGES], y[P3_MAX_EDGES], dxs[P3_MAX_EDGES], dys[P3_MAX_EDGES];
    const int n = p3_project_polygon((const double (*)[3])V, 5, v->H_eff, x, y);
    if (n < 3) return;
    if (!p3_plane_fit((const double (*)[3])V, 5, v->H_eff, o)) return;
    const int nd = p3_dilate_polygon((const double (*)[3])V, 5, P3_BAND_W, D);
    o->ndil = nd ? p3_project_polygon((const double (*)[3])D, nd, v->H_eff, dxs, dys) : 0;
    const float* bx = o->ndil ? dxs : x;
    const float* by = o->ndil ? dys : y;
    const int bn = o->ndil ? o->ndil : n;
    float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
    for (int i = 0; i < bn; i++) {
        x0 = fminf(x0, bx[i]); x1 = fmaxf(x1, bx[i]);
        y0 = fminf(y0, by[i]); y1 = fmaxf(y1, by[i]);
    }
    for (int i = 0; i < 5; i++) o->far_vz = fmax(o->far_vz, V[i][2]);
    float xlo = (float)P3_FRAME_X0, xhi = (float)P3_FRAME_X1;
    openpete_native_widescreen_x_bounds(&xlo, &xhi);
    o->b.xmin = fmaxf(x0, xlo); o->b.xmax = fminf(x1, xhi);
    o->b.ymin = fmaxf(y0, (float)P3_FRAME_Y0); o->b.ymax = fminf(y1, (float)P3_FRAME_Y1);
    if (o->b.xmin >= o->b.xmax || o->b.ymin >= o->b.ymax) {
        float ylo = 0.0f, yhi = 0.0f;
        openpete_native_visible_y_bounds(&ylo, &yhi);
        const float sh = (float)(v->ofy - P3_CY);
        const int in_view = x0 < xhi && x1 > xlo && y0 < yhi - sh && y1 > ylo - sh;
        o->offscreen = in_view ? 2 : 1;
        return;
    }
    o->b.widened = o->b.xmin < (float)P3_FRAME_X0 || o->b.xmax > (float)P3_FRAME_X1;
    o->nedge = n;
    p3_polygon_edges(x, y, n, o->ex, o->ey);
    if (o->ndil) p3_polygon_edges(dxs, dys, o->ndil, o->dx, o->dy);
    o->ok = 1;
}

/** @brief The scene depth a preview vertex tests at.
 *
 * On a re-derived opening, the opening plane's view-Z at the vertex moved by
 * `plane_off` world units, floored at 1. Inside the outline the offset is
 * minus the plane-cover margin (openpete_portal_plane_margin): the preview
 * covers everything at or behind plane - M. In the outline's outward band it
 * is +P3_BAND_EPS: the band covers only what lies behind the plane, so frame
 * stone at or in front of it wins. Otherwise the pass's single depth.
 * @param op The re-derived opening, or NULL.
 * @param depth_world The pass's depth in world units.
 * @param plane_off The offset from the plane in world units, + behind.
 * @param x The vertex's authored screen X.
 * @param y The vertex's authored screen Y.
 * @return The scene depth. */
static float p3_vertex_depth(const p3_opening* op, float depth_world, double plane_off, float x, float y) {
    if (!op || !op->ok)
        return openpete_native_scene_viewz(openpete_worldvz_scaled(depth_world, 1.0f));
    const double inv = op->a * x + op->b_ * y + op->c;
    double vz = (inv > 1e-9 ? 1.0 / inv : 1e9) + plane_off;
    if (vz < 1.0) vz = 1.0;
    return openpete_native_scene_viewz(openpete_worldvz_full((float)vz));
}

/** @brief The preview as the feeder draws it under the view `v`: its opening re-derived, its edges replaced by the opening's.
 *
 * Without a re-derived opening `q` is the capture unchanged.
 * @param cpu Reads the portal record.
 * @param v The view.
 * @param p The captured (or lerped) preview.
 * @param q Receives the preview to draw.
 * @param op Receives the opening. */
static void p3_resolve(CPUState* cpu, const ngte_view* v, const p3_preview* p, p3_preview* q, p3_opening* op) {
    *q = *p;
    p3_native_opening(cpu, v, p, op);
    if (!op->ok) return;
    q->nedge = op->nedge;
    memcpy(q->ex, op->ex, sizeof q->ex);
    memcpy(q->ey, op->ey, sizeof q->ey);
}

static int s_emitted = 0;

/* What every triangle of one preview shares: the recentring offset from the
 * authored screen space to the view's, the depth source and the scene group. */
typedef struct {
    float             offx, offy;
    const p3_opening* op;           /* the re-derived opening, or NULL */
    float             depth_world;  /* the pass's depth, used without an opening */
    double            plane_off;    /* world units added to the plane's view-Z, + behind */
    int               group;
} p3_emit_ctx;

/** @brief Adds one untextured triangle of a preview to the native scene.
 *
 * Each vertex is shifted by the context's recentring offset, takes
 * p3_vertex_depth at its authored position and the context's plane offset as
 * both depth and pvz, and its
 * colour with alpha 0xFF. The triangle is added under `category` and the
 * context's group, with no blend. Under OPENPETE_PORTAL3_DBG the first 12
 * portal-cyclorama triangles from p3_dbg_from print to stderr. Counts the
 * triangle in s_emitted.
 * @param a The first vertex.
 * @param b The second vertex.
 * @param c The third vertex.
 * @param e The preview's emission context.
 * @param category The scene category, such as OPENPETE_SCENE_PORTAL_MASK.
 * @param tag Unused. */
static void p3_emit_tri(const p3_v* a, const p3_v* b, const p3_v* c, const p3_emit_ctx* e,
                        int category, uint32_t tag) OP_REQUIRES_SHARED(sim) {
    openpete_scene_tri t = {0};
    const p3_v* v[3] = { a, b, c };
    const float offx = e->offx, offy = e->offy;
    for (int k = 0; k < 3; k++) {
        const float z = p3_vertex_depth(e->op, e->depth_world, e->plane_off, v[k]->x, v[k]->y);
        t.v[k].sx = v[k]->x + offx; t.v[k].sy = v[k]->y + offy;
        t.v[k].z = z; t.v[k].pvz = z;
        t.v[k].u = 0.0f; t.v[k].v = 0.0f;
        t.v[k].rgba = (v[k]->rgb & 0xFFFFFFu) | 0xFF000000u;
    }
    t.textured = 0; t.blend = 0;
    (void)tag;
    openpete_native_scene_add_tri_grouped(&t, category, 0, e->group);
    if (p3_dbg() && category == OPENPETE_SCENE_PORTAL_CYCLORAMA) {
        static unsigned s_dumps = 0;
        if (s_dumps < 12 && openpete_feed_key_now() >= p3_dbg_from()) {
            s_dumps++;
            fprintf(stderr, "[portal3-native f=%u] rgb=%06X/%06X/%06X xy=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) off=(%.0f,%.0f)\n",
                    openpete_frame, a->rgb & 0xFFFFFF, b->rgb & 0xFFFFFF, c->rgb & 0xFFFFFF,
                    a->x, a->y, b->x, b->y, c->x, c->y, offx, offy);
        }
    }
    s_emitted++;
}

/* One mesh face: the index check, the shared-side outcode reject and the
 * flat-or-gouraud colour indices. The clip to the outline follows in
 * p3_emit_split. The pentagon test is the leaf's own: a face with every
 * vertex outside one edge contributes nothing inside the outline, a face
 * straddling an edge is clipped to it. */
typedef struct {
    const float*    vx;
    const float*    vy;
    const uint8_t*  oc;
    const uint32_t* col;
    uint32_t        vc, cc;
} p3_mesh;

/** @brief Decodes one mesh face into its three screen vertices with their colours.
 *
 * Decodes the three vertex indices from `w0` and the colour indices from `w1`
 * (flat with one index in bits 29-20 when bit 31 is set, else three byte
 * offsets). Drops the face when an index reaches the vertex count or when the
 * three vertices share an outcode bit; each drop increments its census
 * counter. A colour index at or past the colour count reads as black.
 * @param m The projected vertices, outcodes and faded colours.
 * @param w0 The face's first word.
 * @param w1 The face's second word.
 * @param tri Receives the three vertices.
 * @return 1, or 0 when the face is dropped. */
static int p3_decode_face(const p3_mesh* m, uint32_t w0, uint32_t w1, p3_v tri[3]) {
    uint32_t i0 = (w0 >> 20) & 0xFFF, i1 = (w0 >> 10) & 0x3FF, i2 = w0 & 0x3FF;
    if (i0 >= m->vc || i1 >= m->vc || i2 >= m->vc) { s_cen_idx++; return 0; }
    if (m->oc[i0] & m->oc[i1] & m->oc[i2]) { s_cen_oc++; return 0; }
    uint32_t k0, k1, k2;
    if (w1 & 0x80000000u) { k0 = k1 = k2 = (w1 >> 20) & 0x3FF; }
    else { k0 = ((w1 >> 18) & 0xFFC) >> 2; k1 = ((w1 >> 8) & 0xFFC) >> 2; k2 = ((w1 << 2) & 0xFFC) >> 2; }
    const uint32_t idx[3] = { i0, i1, i2 }, kidx[3] = { k0, k1, k2 };
    for (int k = 0; k < 3; k++) {
        tri[k].x = m->vx[idx[k]]; tri[k].y = m->vy[idx[k]];
        tri[k].rgb = kidx[k] < m->cc ? m->col[kidx[k]] : 0;
    }
    return 1;
}

/** @brief Fan-triangulates a convex polygon into the native scene under one emission context.
 * @param poly The polygon.
 * @param n Its vertex count.
 * @param E The emission context.
 * @param category The scene category.
 * @param tag The provenance tag. */
static void p3_emit_fan(const p3_v* poly, int n, const p3_emit_ctx* E, int category, uint32_t tag) OP_REQUIRES_SHARED(sim) {
    for (int k = 1; k + 1 < n; k++)
        p3_emit_tri(&poly[0], &poly[k], &poly[k + 1], E, category, tag);
    if (category == OPENPETE_SCENE_PORTAL_CYCLORAMA && n >= 3) s_cen_tris += n - 2;
}

/** @brief Clips a polygon to the outline and emits it: the interior at E's depth, the outward band behind the plane.
 *
 * With a dilated outline, the polygon is first clipped to the dilated
 * outline; then, edge by edge of the outline, the part outside the edge (and
 * inside every earlier edge) is emitted as band at +P3_BAND_EPS behind the
 * plane and the rest carried on. What survives every edge is the interior,
 * emitted under E. The pieces are disjoint and convex, and their union is the
 * polygon's part inside the dilated outline. Without a dilated outline only
 * the interior is emitted.
 * @param in The polygon.
 * @param n Its vertex count, at most 30.
 * @param q The preview; supplies the outline edges.
 * @param E The interior's emission context; its opening supplies the dilated outline.
 * @param category The scene category.
 * @param tag The provenance tag. */
static void p3_emit_split(const p3_v* in, int n, const p3_preview* q, const p3_emit_ctx* E,
                          int category, uint32_t tag) OP_REQUIRES_SHARED(sim) {
    p3_v P[32], T[32];
    memcpy(P, in, sizeof(p3_v) * (size_t)n);
    const p3_opening* op = E->op;
    const int nd = (op && op->ok) ? op->ndil : 0;
    for (int e = 0; e < nd && n >= 3; e++) {
        n = p3_clip_edge(P, n, T, op->dx[e][0], op->dy[e][0], op->dx[e][1], op->dy[e][1]);
        memcpy(P, T, sizeof(p3_v) * (size_t)n);
    }
    p3_emit_ctx band = *E;
    band.plane_off = P3_BAND_EPS;
    for (int e = 0; e < q->nedge && n >= 3; e++) {
        const float ax = q->ex[e][0], ay = q->ey[e][0], bx = q->ex[e][1], by = q->ey[e][1];
        if (nd) {
            const int m = p3_clip_edge(P, n, T, bx, by, ax, ay);   /* reversed: the outside */
            p3_emit_fan(T, m, &band, category, tag);
        }
        n = p3_clip_edge(P, n, T, ax, ay, bx, by);
        memcpy(P, T, sizeof(p3_v) * (size_t)n);
    }
    if (n >= 3) p3_emit_fan(P, n, E, category, tag);
    else s_cen_edge++;
}

/** @brief Emits one portal's stencil footprint as PORTAL_MASK triangles.
 *
 * The footprint is the bounds rect split by p3_emit_split: the outline's
 * interior at the interior depth and, on a re-derived opening, its outward
 * band behind the plane.
 * @param q The preview.
 * @param B The preview's bounds.
 * @param E The preview's emission context. */
static void p3_emit_mask(const p3_preview* q, p3_bounds B, const p3_emit_ctx* E) OP_REQUIRES_SHARED(sim) {
    const p3_v rect[4] = { { B.xmin, B.ymin, 0 }, { B.xmax, B.ymin, 0 },
                           { B.xmax, B.ymax, 0 }, { B.xmin, B.ymax, 0 } };
    p3_emit_split(rect, 4, q, E, OPENPETE_SCENE_PORTAL_MASK, 0);
}

static int p3_opening_poly(const p3_preview* p, const p3_opening* op, p3_v* out, int maxn);

/** @brief Decodes a preview mesh's vertex table into screen positions and outcodes against the bounds.
 *
 * Each word is (y << 16 | x) << 5, the low five bits draw-owned scratch: x is
 * the signed 16 bits above them, y the signed 11 bits above x (a picture
 * reaches a few dozen rows above and below the frame). The outcode bits are
 * 1 at or above B.ymin, 2 at or below B.ymax, 4 at or left of B.xmin, 8 at or
 * right of B.xmax.
 *
 * The background record (arg 1) is one quad spanning the 4:3 frame, the
 * backdrop the picture is painted over; the picture itself is authored well
 * past the frame's sides. Bounds past a side of the frame therefore reach
 * real picture geometry, and the backdrop's vertices on that side move out to
 * the bound so the band the picture does not cover keeps its colour instead
 * of the frame's clear. The edge colours travel with the vertices, so the
 * band continues the frame's edge exactly and the interior interpolates as it
 * did.
 * @param cpu Reads the table.
 * @param tab The vertex table's address.
 * @param vc The vertex count, at most P3_MAX_VERTS.
 * @param arg The leaf argument of the preview.
 * @param B The preview's bounds.
 * @param vx Receives the X positions.
 * @param vy Receives the Y positions.
 * @param oc Receives the outcodes. */
static void p3_decode_vertices(CPUState* cpu, uint32_t tab, uint32_t vc, int arg, p3_bounds B,
                               float* vx, float* vy, uint8_t* oc) {
    const float x_lo = (arg == 1 && B.xmin < (float)P3_FRAME_X0) ? B.xmin : (float)P3_FRAME_X0;
    const float x_hi = (arg == 1 && B.xmax > (float)P3_FRAME_X1) ? B.xmax : (float)P3_FRAME_X1;
    for (uint32_t i = 0; i < vc; i++) {
        const uint32_t w = cpu->read_word(tab + i * 4);
        const int x = (int16_t)((w >> 5) & 0xFFFF);
        const float fx = x <= P3_FRAME_X0 ? fminf((float)x, x_lo)
                       : x >= P3_FRAME_X1 ? fmaxf((float)x, x_hi) : (float)x;
        const float fy = (float)((int32_t)w >> 21);
        vx[i] = fx; vy[i] = fy;
        oc[i] = (uint8_t)((fy <= B.ymin) | ((fy >= B.ymax) << 1) |
                          ((fx <= B.xmin) << 2) | ((fx >= B.xmax) << 3));
    }
}

/** @brief Whether OPENPETE_PORTAL3_PROBE paints each preview layer one flat sentinel colour.
 *
 * Latched on the first call; set, non-empty and not starting with '0'
 * enables it. Under it the backdrop draws 0xFF00FF, the picture 0x00FF00,
 * the destination-sky extension 0xFFFF00, its background fill 0x00FFFF and
 * the strip continuation 0x0080FF (0x00BBGGRR), so a capture counts each
 * layer's pixels inside the aperture.
 * @return 1 when enabled, else 0. */
static int p3_probe(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_PROBE"); v = (e && *e && *e != '0'); }
    return v;
}

/* The fade terms every colour of one preview passes. */
typedef struct {
    int      ir0;        /* the portal's fade toward far_rgb */
    uint32_t far_rgb;
    int      sky_ir0;    /* the sky fog factor toward env_far, 0 for none */
    uint32_t env_far;
    uint32_t probe;      /* the sentinel replacing every colour, 0xFF000000 for none */
} p3_fade;

#define P3_NO_PROBE 0xFF000000u

/** @brief One colour of a preview after its two fades: the portal's IR0 toward its far colour, then the sky fog toward the environment far colour.
 * @param c The authored colour, 0x00BBGGRR.
 * @param f The preview's fade terms.
 * @return The faded colour, or the probe sentinel. */
static uint32_t p3_fade_colour(uint32_t c, const p3_fade* f) {
    if (f->probe != P3_NO_PROBE) return f->probe;
    if (f->ir0) c = ngte_depth_cue(c, f->far_rgb, f->ir0);
    if (f->sky_ir0) c = ngte_depth_cue(c, f->env_far, f->sky_ir0);
    return c;
}

/** @brief Decodes a preview mesh's colour table through p3_fade_colour.
 * @param cpu Reads the table.
 * @param tab The colour table's address.
 * @param cc The colour count, at most P3_MAX_COLS.
 * @param f The preview's fade terms.
 * @param col Receives the colours. */
static void p3_decode_colours(CPUState* cpu, uint32_t tab, uint32_t cc, const p3_fade* f, uint32_t* col) {
    for (uint32_t j = 0; j < cc; j++) col[j] = p3_fade_colour(cpu->read_word(tab + j * 4) & 0xFFFFFF, f);
}

/** @brief Whether OPENPETE_PORTAL3_SKY draws the destination sky read from the disc beneath each picture.
 *
 * Latched on the first call; on unless the variable starts with '0'.
 * @return 1 when on, else 0. */
static int p3_sky_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_SKY"); v = !(e && *e == '0'); }
    return v;
}

/** @brief Emits the destination sky beneath a portal's picture: its background colour over the side bands, then every triangle portal_sky_s23_extension returns, faded as the picture is.
 *
 * Drawn with the picture record, before its faces and after the backdrop's,
 * at the same depth, so the picture keeps every pixel it covers and the sky
 * replaces the backdrop wherever the sky has a face and the picture stops.
 * Beneath the sky, the parts of the bounds left and right of the 4:3 frame
 * (where p3_decode_vertices stretches the backdrop) are filled with the sky's
 * background colour, the colour the destination level clears its frame to,
 * so the band below the sky's horizon shows it instead of the backdrop; the
 * frame itself keeps the backdrop, which the picture covers. The extension's
 * leading triangles, the continuation of a strip the picture holds below the
 * sky, draw over that fill and under the sky. A triangle whose three
 * corners share an outcode bit against the bounds is dropped, as the leaf
 * drops a picture face; the rest are clipped to the outline by p3_emit_split.
 * @param cpu Reads the portal record.
 * @param p The picture preview; its record names the portal.
 * @param E The preview's emission context.
 * @param B The preview's bounds.
 * @param f The preview's fade terms, the probe sentinel for the extension. */
static void p3_emit_sky(CPUState* cpu, const p3_preview* p, const p3_emit_ctx* E, p3_bounds B, const p3_fade* f) OP_REQUIRES(sim) {
    portal_sky_ext X = { NULL, 0, 0, 0 };
    const int n = p->rec ? portal_sky_s23_extension(cpu, p->rec, &X) : 0;
    const portal_sky_tri* t = X.tri;
    if (n > 0) {
        const uint32_t c = f->probe != P3_NO_PROBE ? 0x00FFFFu : p3_fade_colour(X.bg, f);
        const float side[2][2] = { { B.xmin, fminf(B.xmax, (float)P3_FRAME_X0) },
                                   { fmaxf(B.xmin, (float)P3_FRAME_X1), B.xmax } };
        for (int s = 0; s < 2; s++) {
            if (side[s][1] <= side[s][0] || B.ymax <= B.ymin) continue;
            const p3_v rect[4] = { { side[s][0], B.ymin, c }, { side[s][1], B.ymin, c },
                                   { side[s][1], B.ymax, c }, { side[s][0], B.ymax, c } };
            p3_emit_split(rect, 4, p, E, OPENPETE_SCENE_PORTAL_CYCLORAMA, 0);
        }
    }
    for (int i = 0; i < n; i++) {
        p3_v tri[3];
        int all = 0xF;
        for (int k = 0; k < 3; k++) {
            tri[k].x = t[i].x[k]; tri[k].y = t[i].y[k];
            tri[k].rgb = (i < X.nstrip && f->probe != P3_NO_PROBE) ? 0x0080FFu : p3_fade_colour(t[i].rgb[k], f);
            all &= (tri[k].y <= B.ymin) | ((tri[k].y >= B.ymax) << 1) | ((tri[k].x <= B.xmin) << 2) |
                   ((tri[k].x >= B.xmax) << 3);
        }
        if (!all) p3_emit_split(tri, 3, p, E, OPENPETE_SCENE_PORTAL_CYCLORAMA, 0);
    }
}

/** @brief Re-derives one captured preview in float and emits its triangles.
 *
 * Returns without emitting when the mesh header is outside main RAM or its
 * vertex, colour or face count exceeds P3_MAX_VERTS, P3_MAX_COLS or 4096.
 * Otherwise decodes the vertices (p3_decode_vertices) and the faded colours
 * (p3_decode_colours), emits the footprint once per portal as the stencil mask (the
 * opening polygon on a re-derived opening, else the bounds rect), and emits
 * each surviving face fan-triangulated as portal-cyclorama triangles under the
 * portal's provenance tag. Reads PSX RAM only.
 * @param cpu The guest CPU state.
 * @param p The preview; its edges are the ones the faces clip to.
 * @param op The re-derived opening, or NULL.
 * Before a picture's faces the destination sky is emitted beneath them
 * (p3_emit_sky), unless OPENPETE_PORTAL3_SKY is 0; a portal drawn as its
 * backdrop alone gets no sky.
 * @param vw The view; supplies the screen recentring offset.
 * @param sky_ir0 The sky fog factor, 0 for none.
 * @param env_far The environment far colour.
 * @param mask_done Per-slot flags, set once the slot's mask is emitted. */
static void p3_extract_one(CPUState* cpu, const p3_preview* p, const p3_opening* op, const ngte_view* vw,
                           int sky_ir0, uint32_t env_far, int mask_done[P3_MAX_PORTALS]) OP_REQUIRES(sim) {
    static float vx[P3_MAX_VERTS], vy[P3_MAX_VERTS];
    static uint8_t oc[P3_MAX_VERTS];
    static uint32_t col[P3_MAX_COLS];
    uint32_t M = p->mesh;
    if (M < 0x80000000u || M >= 0x80200000u) return;
    uint32_t vc = cpu->read_word(M + 0x00), vo = cpu->read_word(M + 0x04);
    uint32_t cc = cpu->read_word(M + 0x08), co = cpu->read_word(M + 0x0C);
    uint32_t fc = cpu->read_word(M + 0x10), fo = cpu->read_word(M + 0x14);
    if (vc > P3_MAX_VERTS || cc > P3_MAX_COLS || fc > 4096) return;
    const p3_emit_ctx E = { (float)(vw->ofx - P3_CX), (float)(vw->ofy - P3_CY), op, p->depth_w,
                            -(double)openpete_portal_plane_margin(), 1 + p->slot };
    const p3_bounds B = p3_bounds_of(p, op);
    p3_decode_vertices(cpu, M + vo, vc, p->arg, B, vx, vy, oc);
    const int probe = p3_probe();
    p3_fade F = { p->ir0, p->far_rgb, sky_ir0, env_far, probe ? (p->arg ? 0xFF00FFu : 0x00FF00u) : P3_NO_PROBE };
    p3_decode_colours(cpu, M + co, cc, &F, col);
    /* The portal's stencil footprint, once per portal (the background record
     * comes first). */
    if (p->slot < P3_MAX_PORTALS && !mask_done[p->slot]) {
        mask_done[p->slot] = 1;
        p3_emit_mask(p, B, &E);
    }
    const uint32_t tag = OPENPETE_TAG_PORTAL(p->arg ? OPENPETE_TAG_PORTAL_FLAT : OPENPETE_TAG_PORTAL_DOME, p->slot);
    openpete_native_scene_set_tag(tag);
    if (p->arg == 0 && p3_sky_on()) {
        p3_fade S = F;
        S.probe = probe ? 0xFFFF00u : P3_NO_PROBE;
        p3_emit_sky(cpu, p, &E, B, &S);
    }
    const p3_mesh mesh = { vx, vy, oc, col, vc, cc };
    for (uint32_t f = 0; f < fc; f++) {
        uint32_t w0 = cpu->read_word(M + fo + f * 8), w1 = cpu->read_word(M + fo + f * 8 + 4);
        s_cen_faces++;
        p3_v tri[3];
        if (p3_decode_face(&mesh, w0, w1, tri))
            p3_emit_split(tri, 3, p, &E, OPENPETE_SCENE_PORTAL_CYCLORAMA, tag);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Prints the aperture census: a summary line, the off-screen skip totals, then one uncapped line per fed preview.
 *
 * The skip line holds the run totals of off-screen openings p3_feed and
 * portal_s23_sky_planes skipped, and how many of p3_feed's lay inside the view
 * but outside the pass's rows. Each preview line holds the opening polygon in
 * the screen space p3_emit_tri draws into and the per-record channels the
 * replay carries; its native field is 1 on a re-derived opening, 2 on an
 * off-screen one, else 0. It is the
 * statistic tests/harness/check_feeder_subtick.sh --path portal judges, the
 * aperture being far larger than the compact blob a pixel mask models.
 * @param cpu Reads the portal records.
 * @param T The tick whose previews were fed.
 * @param vw The view; supplies the screen recentring offset. */
static void p3_census(CPUState* cpu, const p3_tick* T, const ngte_view* vw) {
    const float offx = (float)(vw->ofx - P3_CX), offy = (float)(vw->ofy - P3_CY);
    fprintf(stderr, "[portal3-feed f=%u src=%s a=%.3f portals=%d tris=%d emitted=%d]\n",
            openpete_feed_key_now(), s_feed_src, (double)s_feed_alpha, T->n, s_cen_tris, s_emitted);
    fprintf(stderr, "[portal3-off f=%u feed=%u plane=%u rows=%u]\n",
            openpete_feed_key_now(), s_off_feed, s_off_plane, s_off_rows);
    for (int i = 0; i < T->n; i++) {
        p3_preview q; p3_opening op;
        p3_resolve(cpu, vw, &T->p[i], &q, &op);
        const p3_preview* p = &q;
        p3_v poly[32];
        const int pn = p3_opening_poly(p, &op, poly, 32);
        fprintf(stderr, "[portal3-ap f=%u src=%s a=%.3f slot=%d arg=%d n=%d d7=%d dw=%.1f ir0=%d far=%06X rec=%08X native=%d]",
                openpete_feed_key_now(), s_feed_src, (double)s_feed_alpha, p->slot, p->arg, pn,
                p->depth7, (double)p->depth_w, p->ir0, p->far_rgb, p->rec, op.ok ? 1 : op.offscreen ? 2 : 0);
        for (int k = 0; k < pn; k++)
            fprintf(stderr, " (%.2f,%.2f)", (double)(poly[k].x + offx), (double)(poly[k].y + offy));
        fprintf(stderr, "\n");
    }
}

/** @brief Whether extended LOD draws every portal's picture: openpete_no_lod, unless OPENPETE_NOLOD_PORTAL_DOME starts with '0'.
 *
 * The variable is latched on the first call; openpete_no_lod is read live.
 * @return 1 when the picture is forced, else 0. */
static int p3_lod_picture(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_NOLOD_PORTAL_DOME"); v = !(e && *e == '0'); }
    return openpete_no_lod && v;
}

/** @brief The previews extended LOD draws: every preview unfaded by distance, and the picture of every portal the pass drew only as its backdrop.
 *
 * The pass draws a portal at depth 0x5000 and beyond as its backdrop alone,
 * faded fully toward its far colour, and fades both records toward it from
 * depth 0x4000. Here every preview's IR0 is 0, and a backdrop record that no
 * picture record of the same slot follows is followed by one: the same
 * block, with the leaf argument 0 and the mesh header at +0x3C.
 * @param T The tick as captured (or lerped).
 * @param O Receives the previews to feed. */
static void p3_lod_tick(const p3_tick* T, p3_tick* O) {
    O->n = 0; O->frame = T->frame; O->valid = T->valid;
    for (int i = 0; i < T->n && O->n < P3_MAX_PORTALS * 2; i++) {
        O->p[O->n] = T->p[i];
        O->p[O->n++].ir0 = 0;
        const int pic_follows = i + 1 < T->n && T->p[i + 1].arg == 0 && T->p[i + 1].slot == T->p[i].slot;
        if (T->p[i].arg != 1 || pic_follows || O->n >= P3_MAX_PORTALS * 2) continue;
        O->p[O->n] = O->p[O->n - 1];
        O->p[O->n].arg = 0;
        O->p[O->n++].mesh += 0x3Cu;
    }
}

/** @brief Emits every preview of a tick into the native scene.
 *
 * Returns without emitting when the native scene or RP_PORTAL is off, and
 * before opening the frame's scene when the tick holds no preview. Resets the
 * census counters, opens the scene for the current openpete_frame, resolves
 * each preview's opening under the view (p3_resolve) and extracts it unless the
 * opening is off-screen, and records the frame in s_sky_fed_frame when any
 * triangle was emitted. Under extended LOD the previews are p3_lod_tick's.
 * @param cpu The guest CPU state.
 * @param T The captured (or lerped) previews. */
static void p3_feed(CPUState* cpu, const p3_tick* T) OP_REQUIRES(sim) {
    if (!openpete_native_scene_enabled() || !render_path_enabled(RP_PORTAL)) return;
    s_emitted = 0;
    s_cen_faces = s_cen_idx = s_cen_oc = s_cen_edge = s_cen_tris = 0;
    /* A pass that captured no preview emits nothing, so it must not open the
     * frame's scene: opening snapshots and clears whatever the frame holds. */
    if (T->n == 0) return;
    static p3_tick s_lod;
    if (p3_lod_picture()) { p3_lod_tick(T, &s_lod); T = &s_lod; }
    /* In a game state that draws neither sky nor terrain this pass is the
     * frame's first native feeder, so it opens the frame's scene itself.
     * Without that, the previews append to the previous frame's already
     * presented scene and the next feeder's frame-keyed clear discards them.
     * The call is keyed on the frame: a scene an earlier feeder opened is
     * left as it is and the previews append to it. */
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    ngte_view vw; ngte_view_build(cpu, &vw);
    int sky_ir0 = game_layout.fog.factor ? (int)cpu->read_word(game_layout.fog.factor) : 0;
    uint32_t env_far = ngte_fog_far_colour(cpu);
    int mask_done[P3_MAX_PORTALS]; memset(mask_done, 0, sizeof mask_done);
    for (int i = 0; i < T->n; i++) {
        p3_preview q; p3_opening op;
        p3_resolve(cpu, &vw, &T->p[i], &q, &op);
        if (op.offscreen) { s_off_feed++; s_off_rows += op.offscreen == 2; continue; }
        p3_extract_one(cpu, &q, &op, &vw, sky_ir0, env_far, mask_done);
    }
    if (s_emitted) s_sky_fed_frame = openpete_frame;
    if (p3_dbg() && openpete_feed_key_now() >= p3_dbg_from()) p3_census(cpu, T, &vw);
}

/* ---- opening planes for the shared terrain feeder ----------------------
 *
 * The terrain feeder drops a transparent face whose centroid falls inside an
 * opening and whose mean view-Z is at or behind the opening plane minus the
 * cover margin: blended terrain draws AFTER the portal sequence, so without
 * the drop it paints over a preview that has already covered it.
 *
 * The preview is a pre-projected 2-D picture, but on a re-derived opening it
 * tests at the opening plane's own depth (p3_vertex_depth), so the plane
 * handed to the terrain feeder is that plane: the screen-space inv-vz field
 * p3_plane_fit computes, moved from the authored screen space into the
 * view's by the recentring offset. back_vz is the farthest pentagon vertex.
 * Without a re-derived opening the plane is the pass's single depth
 * (inv-vz = 1 / (depth7 << 7), a = b = 0). The preview's render depth
 * carries no OT slot push, so push_vz is 0.
 *
 * The footprint is where the preview's colours actually land: the bounds
 * intersected with the pentagon half-planes, in the same recentred screen
 * space p3_emit_tri draws into.
 *
 * The planes come from the tick's capture, which names the portal records the
 * pass drew; the opening is re-derived under the view the terrain feed
 * projects with. The terrain feed runs before the portal pass within a
 * present, so a canonical present reads the previous tick's set of drawn
 * portals. */
/** @brief A preview's plane depth in full-world view-Z, floored at 1.
 *
 * @param p The preview.
 * @return depth_w, or 1 when it is at or below 1. */
static double p3_plane_depth_world(const p3_preview* p) {
    double d = (double)p->depth_w;
    return d > 1.0 ? d : 1.0;
}

/** @brief The preview's painted footprint as a screen polygon.
 *
 * The preview's colour bounds (p3_bounds_of) are clipped by each of its
 * pentagon edges' inside half-plane, the same cross >= 0 rule the colour
 * extraction clips with. An edge with every vertex inside is skipped.
 * @param p The preview as p3_resolve returns it.
 * @param op Its opening, or NULL.
 * @param out Receives the polygon.
 * @param maxn The capacity of `out`.
 * @return The vertex count; 0 when the polygon collapses below three vertices,
 *         lies wholly outside an edge, or exceeds `maxn`. */
static int p3_opening_poly(const p3_preview* p, const p3_opening* op, p3_v* out, int maxn) {
    p3_v poly[32], tmp[32];
    const p3_bounds B = p3_bounds_of(p, op);
    poly[0].x = B.xmin; poly[0].y = B.ymin; poly[0].rgb = 0;
    poly[1].x = B.xmax; poly[1].y = B.ymin; poly[1].rgb = 0;
    poly[2].x = B.xmax; poly[2].y = B.ymax; poly[2].rgb = 0;
    poly[3].x = B.xmin; poly[3].y = B.ymax; poly[3].rgb = 0;
    int n = 4;
    for (int e = 0; e < p->nedge; e++) {
        const float ax = p->ex[e][0], ay = p->ey[e][0], bx = p->ex[e][1], by = p->ey[e][1];
        int outside = 0;
        for (int k = 0; k < n; k++)
            if (p3_cross(ax, ay, bx, by, poly[k].x, poly[k].y) < 0.0f) outside++;
        if (outside == 0) continue;
        if (outside == n) return 0;
        n = p3_clip_edge(poly, n, tmp, ax, ay, bx, by);
        memcpy(poly, tmp, sizeof(p3_v) * (size_t)n);
        if (n < 3) return 0;
    }
    if (n > maxn) return 0;
    memcpy(out, poly, sizeof(p3_v) * (size_t)n);
    return n;
}

int portal_s23_sky_recent(void) {
    if (s_pick >= 0) return s_pick != CUT_HELD_EMPTY && s_curr.n > 0;
    return s_sky_fed_frame != 0u && (openpete_frame - s_sky_fed_frame) <= 2u;
}

/** @brief Fills one terrain-feeder plane record from a resolved preview.
 *
 * On a re-derived opening the inv-vz field is the opening plane's, moved from
 * the authored screen space into the view's by the recentring offset, and
 * back_vz is the farthest opening vertex; otherwise the field is the pass's
 * single depth and back_vz that depth. The outline is the opening polygon
 * shifted by the same offset, with its bounding box. On a re-derived opening
 * the view-space outline is the pentagon's five vertices, which lets the
 * terrain feeder's rim classifier find the band of terrain lying in the
 * opening plane, discarded per fragment inside the opening (rim_fragment);
 * otherwise vn = 0 leaves the classifier's frames invalid.
 * @param o Receives the record.
 * @param p The resolved preview.
 * @param op Its opening.
 * @param poly The opening polygon in the authored screen space.
 * @param pn Its vertex count, at most 16.
 * @param offx The recentring offset's X.
 * @param offy The recentring offset's Y. */
static void p3_skyplane_fill(openpete_portal_skyplane* o, const p3_preview* p, const p3_opening* op,
                             const p3_v* poly, int pn, float offx, float offy) {
    memset(o, 0, sizeof *o);
    const double dz = p3_plane_depth_world(p);
    o->portal  = p->mesh;                 /* the preview mesh: this portal's identity */
    o->a       = op->ok ? op->a : 0.0;
    o->b       = op->ok ? op->b_ : 0.0;
    o->c       = op->ok ? op->c - op->a * offx - op->b_ * offy : 1.0 / dz;
    o->back_vz = op->ok ? op->far_vz : dz;
    o->ot_vz   = dz;
    o->push_vz = 0.0;
    o->pn = pn;
    o->x0 = o->y0 = 1e9f; o->x1 = o->y1 = -1e9f;
    for (int k = 0; k < pn; k++) {
        o->px[k] = poly[k].x + offx;
        o->py[k] = poly[k].y + offy;
        o->x0 = fminf(o->x0, o->px[k]); o->x1 = fmaxf(o->x1, o->px[k]);
        o->y0 = fminf(o->y0, o->py[k]); o->y1 = fmaxf(o->y1, o->py[k]);
    }
    o->vn = op->ok ? 5 : 0;
    for (int k = 0; k < o->vn; k++) {
        o->vx[k] = op->V[k][0]; o->vy[k] = op->V[k][1]; o->vz[k] = op->V[k][2];
    }
    /* An inner-rim face is discarded per fragment inside the opening, where
     * the preview paints, and drawn outside it. */
    o->rim_fragment = op->ok ? 1 : 0;
}

int portal_s23_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max) {
    /* No preview paints an opening while the portal path is deselected
     * (p3_feed), so terrain keeps its faces there. */
    if (!render_path_enabled(RP_PORTAL)) return 0;
    if (s_pick == CUT_HELD_EMPTY) return 0;
    if (!s_curr.valid || s_curr.n == 0 || max <= 0) return 0;
    if (s_pick < 0 && openpete_frame - s_curr.frame > 3u) return 0;
    const float offx = (float)(v->ofx - P3_CX), offy = (float)(v->ofy - P3_CY);
    int n = 0;
    for (int i = 0; i < s_curr.n && n < max; i++) {
        if (s_curr.p[i].arg != 1) continue;   /* one entry per portal, on its base record */
        p3_preview q; p3_opening op;
        p3_resolve(cpu, v, &s_curr.p[i], &q, &op);
        if (op.offscreen) { s_off_plane++; continue; }
        p3_v poly[16];
        const int pn = q.nedge < 3 ? 0 : p3_opening_poly(&q, &op, poly, 16);
        if (pn < 3) continue;                 /* no bounded opening */
        openpete_portal_skyplane* o = &out[n++];
        p3_skyplane_fill(o, &q, &op, poly, pn, offx, offy);
        if (p3_dbg() && openpete_feed_key_now() >= p3_dbg_from()) {
            static unsigned s_lines = 0;
            if (s_lines < 24) {
                s_lines++;
                fprintf(stderr, "[portal3-plane f=%u] mesh=%08X vz=%.0f pn=%d bbox=(%.0f,%.0f)-(%.0f,%.0f)\n",
                        openpete_frame, o->portal, o->ot_vz, pn, o->x0, o->y0, o->x1, o->y1);
            }
        }
    }
    return n;
}

/* ---- records the pass culls only for the 4:3 frame ---------------------
 *
 * The pass rejects a portal whose five projected pentagon vertices share an
 * outcode bit of func_8001C0F4, and the X bits test the 4:3 frame (X <= 0,
 * X >= 512) (S2 func_8004C66C.s:215-336, S3 func_8001EDEC.s:294-482). A
 * portal lying wholly in a widescreen side band is therefore never drawn by
 * the game. The depth gate's lateral cone (slope 1 about the view axis, at
 * vertex 2) holds the 4:3 frustum (half-tangent 256 / H, 0.75 at H 341) but
 * not the outer band of a frame wider than 16:9, so a portal there is
 * rejected before its outcodes are taken. After the pass, every record it
 * did not draw is evaluated again from game state: the record gates with the
 * cone scaled to the view, the depth and fade terms the pass would have
 * written, and the outcode test with the X bits against the view's screen-X
 * range. A record whose tests fail at 4:3 and pass for the view is added to
 * the tick's previews as the pass would have captured it.
 * A record the pass drew keeps the pass's own capture. The integer helpers
 * below are the pass's math routines, which are instruction-identical in
 * the two games; the S2 address is named first, the S3 one second. */

/** @brief The pass's 12-bit angle of (x, y): func_8001B4B8 / func_8004E880 with mode 1.
 *
 * The minor of |x| and |y| over the major in 14 fractional bits indexes the
 * u16 octant table in 7-bit steps with a linear lerp; the signs and the
 * comparison of x with y pick the octant base (S2 stdutil.s:198-299). The result
 * is (angle + 2) >> 2 masked to 12 bits.
 * @param cpu Reads portal_game_s23.atan_table.
 * @param x The first component.
 * @param y The second component.
 * @return The angle, 0x1000 to the turn. */
static int p3g_atan(CPUState* cpu, int32_t x, int32_t y) {
    const int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    const int32_t lo = ax > ay ? ay : ax;
    int32_t hi = ax > ay ? ax : ay;
    if (hi == 0) hi = 1;
    const uint32_t q = (uint32_t)((int32_t)((uint32_t)lo << 14) / hi);
    static const struct { int sub; int32_t base; } oct[2][2][2] = {
        /* x >= 0: y >= 0, y < 0; each by (|x| - |y| < 0) */
        { { { 0, 0x0000 }, { 1, 0x1000 } }, { { 1, 0x4000 }, { 0, 0x3000 } } },
        /* x < 0 */
        { { { 1, 0x2000 }, { 0, 0x1000 } }, { { 0, 0x2000 }, { 1, 0x3000 } } },
    };
    const int sx = x < 0, sy = y < 0;
    const int lt = ax - ay < 0;
    const uint32_t tab = 0x80000000u | ((portal_game_s23.atan_table + ((q >> 7) << 1)) & 0x1FFFFFu);
    uint32_t t = cpu->read_half(tab);
    if (q & 0x7Fu) t += ((q & 0x7Fu) * (uint32_t)((int32_t)cpu->read_half(tab + 2) - (int32_t)t)) >> 7;
    const int32_t a = oct[sx][sy][lt].sub ? oct[sx][sy][lt].base - (int32_t)t : oct[sx][sy][lt].base + (int32_t)t;
    return (int)(((uint32_t)(a + 2) >> 2) & 0xFFFu);
}

/** @brief The pass's interpolated sine (cos_off 0) or cosine (cos_off 0x80) of a 12-bit angle: func_8001B61C / func_8001B664.
 *
 * Entry a >> 4 of the game's sin table, lerped by the low four bits
 * (S2 stdutil.s:309-353).
 * @param cpu Reads game_layout.sin_table.
 * @param a The angle, 0x1000 to the turn.
 * @param cos_off 0 for the sine, 0x80 for the cosine.
 * @return The value, Q12. */
static int32_t p3g_sin(CPUState* cpu, int32_t a, uint32_t cos_off) {
    a &= 0xFFF;
    const uint32_t p = game_layout.sin_table + cos_off + (uint32_t)((a >> 4) << 1);
    int32_t t = (int16_t)cpu->read_half(p);
    if (a & 0xF) t += ((a & 0xF) * ((int16_t)cpu->read_half(p + 2) - t)) >> 4;
    return t;
}

/** @brief The number of leading zero bits of a positive word, 32 for zero, as the GTE's LZCR reads it.
 * @param v The word.
 * @return The count. */
static int p3g_lzc(uint32_t v) {
    int n = 0;
    while (n < 32 && !(v & 0x80000000u)) { v <<= 1; n++; }
    return n;
}

/** @brief The pass's planar length of (x, y): func_8001BA20 / func_8004EDE8 with a1 = 0.
 *
 * Both components shift right until they fit 14 bits, SQR sums their
 * squares, and the root is the table's seed for the normalized sum refined
 * by one Newton step, shifted back (S2 stdutil.s:610-700).
 * @param cpu Reads portal_game_s23.sqrt_table.
 * @param x The first component.
 * @param y The second component.
 * @return The length. */
static int32_t p3g_len2d(CPUState* cpu, int32_t x, int32_t y) {
    const uint32_t m = (uint32_t)(x < 0 ? -x : x) | (uint32_t)(y < 0 ? -y : y);
    const int sh0 = 0x12 - p3g_lzc(m);
    const int sh = sh0 > 0 ? sh0 : 0;
    const int32_t a = (int16_t)(x >> sh), b = (int16_t)(y >> sh);
    const int32_t sum = a * a + b * b;
    if (sum == 0) return 0;
    const int lz = p3g_lzc((uint32_t)sum) & 0xFE;
    const int e = lz - 0x18;
    const uint32_t n = e >= 0 ? (uint32_t)sum << e : (uint32_t)sum >> -e;
    const int32_t seed = (int16_t)cpu->read_half(portal_game_s23.sqrt_table + ((n - 0x40u) << 1));
    int32_t r = (int32_t)(((uint32_t)seed << ((0x1F - lz) >> 1)) >> 12);
    if (r) r += ((sum - r * r) / (r + 1)) >> 1;
    return r << sh;
}

/** @brief The pass's rotation of a vector by the world matrix: func_8001B9A4 / func_8004ED6C.
 *
 * IR1..IR3 = (-v1, -v2, v0) truncated to 16 bits, MVMVA with sf = 1, and the
 * result (MAC3, -MAC1, -MAC2) (S2 stdutil.s:572-608).
 * @param M The matrix, Q12.
 * @param v The vector.
 * @param o Receives the rotated vector; may alias v. */
static void p3g_rotate(const int M[3][3], const int32_t v[3], int32_t o[3]) {
    const int64_t ir[3] = { (int16_t)-v[1], (int16_t)-v[2], (int16_t)v[0] };
    int32_t mac[3];
    for (int r = 0; r < 3; r++)
        mac[r] = (int32_t)((M[r][0] * ir[0] + M[r][1] * ir[1] + M[r][2] * ir[2]) >> 12);
    o[0] = mac[2]; o[1] = -mac[0]; o[2] = -mac[1];
}

/** @brief A right shift that rounds toward zero, as func_8001BD48 / func_8004F110 shifts each component.
 * @param v The value.
 * @param n The shift.
 * @return The shifted value. */
static int32_t p3g_shr(int32_t v, int n) {
    return v < 0 ? -((-v) >> n) : (v >> n);
}

/* What the record gates leave for the rest of the pass. */
typedef struct {
    int32_t d;      /* the view depth of vertex 2 lowered 0x540, floored at 0 */
    int32_t face;   /* the facing term the edge-free case reads (+0x108 on the stack) */
    int     sh;     /* the projection's right shift, 3 at depth 0x4000 and beyond */
    int     fit;    /* the right shift the depth gate applied, 2 unless radius-free */
    int     beyond; /* 1 when the depth is at or past the record's draw radius */
} p3_gate;

/** @brief The smallest right shift, at least s, that leaves every component of v plus a margin inside int16.
 * @param v The camera-relative vector.
 * @param margin Added to each component's magnitude before the test.
 * @param s The least shift.
 * @return The shift. */
static int p3g_fit_shift(const int32_t v[3], int32_t margin, int s) {
    int64_t m = 0;
    for (int k = 0; k < 3; k++) { const int64_t a = v[k] < 0 ? -(int64_t)v[k] : v[k]; if (a > m) m = a; }
    m += margin;
    while ((m >> s) > 0x7FFF) s++;
    return s;
}

/** @brief The pass's depth gates of one record: the enable and draw radius byte, the centre cone and the depth floor.
 *
 * Vertex 2 minus the camera, Z lowered 0x540, shifted right 2, rotated by the
 * world matrix and shifted back gives the depth d and the lateral term l.
 * The record passes when +0x40 is nonzero, d >= -0x7FF, d < +0x40 << 10,
 * c > l - 0xA00 and c > -(l + 0x800) (func_8004C66C.s:28-83,
 * func_8001EDEC.s:30-142), c the cone depth: d at 4:3, where the last two
 * are the pass's lateral cone of slope 1 about the view axis, and d * k for
 * d > 0 under a view whose screen-X half-tangent is k times the 4:3 one
 * (p3_cone_scale), so the cone keeps its margin to the visible frustum edge.
 *
 * Radius-free (radius_free = 1) the draw radius clause is skipped and the shift is
 * the least one from 2 that keeps each component in int16
 * (p3g_fit_shift), since the pass's int16 truncation wraps the vector past
 * 131071 units. For a record past its radius g->sh also grows until the
 * outcode projection's components fit, with a margin of 0x4000 for the
 * other vertices. Within the radius and 131071 units the result equals
 * the pass's own.
 * @param cpu Reads the record, the camera and the world matrix.
 * @param rec The record.
 * @param k The cone slope scale, 1.0 for the pass's own test.
 * @param radius_free 1 to drop the draw radius clause, 0 for the pass's test.
 * @param g Receives the floored depth, the shifts and whether the depth is past the radius.
 * @return 1 when the record passes, else 0. */
static int p3_gate_depth(CPUState* cpu, uint32_t rec, double k, int radius_free, p3_gate* g) {
    const uint32_t en = cpu->read_byte(rec + 0x40);
    if (!en) return 0;
    const uint32_t cp = game_layout.cam.position;
    int32_t v[3];
    for (int a = 0; a < 3; a++)
        v[a] = (int32_t)cpu->read_word(rec + 0x18 + (uint32_t)a * 4) - (int32_t)cpu->read_word(cp + (uint32_t)a * 4);
    v[2] -= 0x540;
    const int32_t w[3] = { v[0], v[1], v[2] };
    const int s = radius_free ? p3g_fit_shift(v, 0, 2) : 2;
    for (int a = 0; a < 3; a++) v[a] = p3g_shr(v[a], s);
    int M[3][3];
    ngte_read_matrix(cpu, game_layout.cam.view_matrix, M);
    p3g_rotate(M, v, v);
    const int32_t d = (int32_t)((uint32_t)v[0] << s), l = (int32_t)((uint32_t)v[1] << s);
    g->beyond = d >= (int32_t)(en << 10);
    if (d < -0x7FF || (g->beyond && !radius_free)) return 0;
    const double c = d > 0 ? (double)d * k : (double)d;
    if (!((double)(l - 0xA00) < c) || !((double)-(l + 0x800) < c)) return 0;
    g->d = d < 0 ? 0 : d;
    g->sh = g->d >= 0x4000 ? 3 : 0;
    if (g->beyond) g->sh = p3g_fit_shift(w, 0x4000, g->sh);
    g->fit = s;
    return 1;
}

/** @brief The pass's facing gates of one record: the side the camera sees the plane from, the back-side flag and the distance from the plane.
 *
 * a is the angle of the edge from vertex 0 to vertex 4, c the angle from
 * vertex 2 to the camera in XY, and t = a - c wrapped to (-0x800, 0x800].
 * The record passes when 0x40 < |t| < 0x7C0, when t >= 0 or +0x41 is set,
 * when |sin t * |camera - vertex 2|_XY| >= 0xA0, and when the plane normal
 * (sin a, -cos a, 0) rotated by the world matrix has a depth term, negated
 * for t < 0, of at most 0x800 (func_8004C66C.s:93-207,
 * func_8001EDEC.s:143-268). Under a radius-free depth gate's shift
 * (g->fit > 2) c is taken of the XY vector shifted right by g->fit - 2,
 * which keeps the arctangent's ratio from overflowing.
 * @param cpu Reads the record, the camera, the world matrix and the tables.
 * @param rec The record.
 * @param g Carries the depth gate's shift; receives the facing term.
 * @return 1 when the record passes, else 0. */
static int p3_gate_facing(CPUState* cpu, uint32_t rec, p3_gate* g) {
    const uint32_t cp = game_layout.cam.position;
    const int32_t rx = (int32_t)cpu->read_word(rec + 0x18), ry = (int32_t)cpu->read_word(rec + 0x1C);
    const int32_t cx = (int32_t)cpu->read_word(cp) - rx, cy = (int32_t)cpu->read_word(cp + 4) - ry;
    const int a = (int16_t)p3g_atan(cpu, (int32_t)cpu->read_word(rec + 0x30) - (int32_t)cpu->read_word(rec + 0x00),
                                    (int32_t)cpu->read_word(rec + 0x34) - (int32_t)cpu->read_word(rec + 0x04));
    const int c = (int16_t)p3g_atan(cpu, p3g_shr(cx, g->fit - 2), p3g_shr(cy, g->fit - 2));
    int t = (a - c) & 0xFFF;
    if (t > 0x800) t -= 0x1000;
    if ((uint32_t)(t + 0x40) < 0x81u || (uint32_t)(t + 0x7BF) >= 0xF7Fu) return 0;
    if (t < 0 && !cpu->read_byte(rec + 0x41)) return 0;
    int32_t perp = (int32_t)((uint32_t)p3g_sin(cpu, t, 0) * (uint32_t)p3g_len2d(cpu, cx, cy)) >> 12;
    if (perp < 0) perp = -perp;
    if (perp < 0xA0) return 0;
    int M[3][3];
    ngte_read_matrix(cpu, game_layout.cam.view_matrix, M);
    int32_t n[3] = { p3g_sin(cpu, a, 0), -p3g_sin(cpu, a, 0x80), 0 };
    p3g_rotate(M, n, n);
    g->face = t < 0 ? -n[0] : n[0];
    return g->face <= 0x800;
}

/** @brief Projects one world point as func_8001C040 / func_8004F408 does and returns func_8001C0F4's outcode against [xlo, xhi].
 *
 * The camera-relative vector (T1 - y, T2 - z, x - T0) is shifted right by the
 * gate's shift and truncated to 16 bits, rotated by the sky matrix and
 * divided as RTPS divides (ngte_screen_gte, SXY bound on); SZ is shifted back.
 * The bits are 1 at X <= xlo, 2 at X >= xhi, 4 above row 13, 8 at row 228
 * and below, 0x10 at SZ < 256 (S2 stdutil.s:1134-1209). X and Y are floored to
 * the integer screen position the GTE returns.
 * @param v The view; supplies the camera, the sky matrix, OFX/OFY and H.
 * @param W The world point.
 * @param sh The gate's shift.
 * @param xlo The left bound.
 * @param xhi The right bound.
 * @return The outcode. */
static int p3_gate_outcode(const ngte_view* v, const double W[3], int sh, float xlo, float xhi) {
    const int32_t a = (int16_t)((int32_t)(v->cam_ram[1] - W[1]) >> sh);
    const int32_t b = (int16_t)((int32_t)(v->cam_ram[2] - W[2]) >> sh);
    const int32_t c = (int16_t)((int32_t)(W[0] - v->cam_ram[0]) >> sh);
    double m1, m2, m3;
    ngte_mac(v->cam_M, a, b, c, &m1, &m2, &m3);
    ngte_proj o;
    ngte_screen_gte(m1, m2, m3, v->ofx, v->ofy, v, 1, &o);
    const double sz = (m3 < 0.0 ? 0.0 : m3 > 65535.0 ? 65535.0 : floor(m3)) * (double)(1 << sh);
    const double sx = floor(o.sx), sy = floor(o.sy);
    return (sx <= (double)xlo) | ((sx >= (double)xhi) << 1) | ((sy < 13.0) << 2) |
           ((sy >= 228.0) << 3) | ((sz < P3_NEAR_SZ) << 4);
}

/** @brief Whether the pass would call the leaf for one record's call under the screen-X range [xlo, xhi].
 *
 * Call 1 (the background) projects the pentagon; call 0 (the picture)
 * projects each vertex moved by ((v - vertex 2) + (0, 0, dz)) >> 6, dz 0x540
 * in a level whose number is a multiple of 10 and 0x320 elsewhere
 * (func_8004C66C.s:245-290, func_8001EDEC.s:355-409), after the opening
 * animation where the game has one. The call is rejected when the five
 * outcodes share a bit, and when no edge has two vertices free of a shared
 * bit unless the facing term is negative and the outcodes together hold all
 * four screen bits (func_8004C66C.s:399-444, func_8001EDEC.s:540-593).
 * @param cpu Reads the record and the level number.
 * @param v The view the outcodes project under.
 * @param rec The record.
 * @param g The record's gate results.
 * @param call 1 for the background, 0 for the picture.
 * @param xlo The left bound.
 * @param xhi The right bound.
 * @return 1 when the leaf would be called, else 0. */
static int p3_gate_call(CPUState* cpu, const ngte_view* v, uint32_t rec, const p3_gate* g, int call,
                        float xlo, float xhi) {
    double W[5][3];
    p3_record_vertices(cpu, rec, W);
    const int32_t dz = (cpu->read_word(game_layout.level_id) % 10u) == 0u ? 0x540 : 0x320;
    int oc[5], all = -1, any = 0;
    for (int k = 0; k < 5 && !call; k++)
        for (int a = 0; a < 3; a++) {
            const int32_t r = (int32_t)cpu->read_word(rec + (uint32_t)(k * 12 + a * 4));
            const int32_t r2 = (int32_t)cpu->read_word(rec + 0x18 + (uint32_t)a * 4);
            W[k][a] += (double)p3g_shr(r - r2 + (a == 2 ? dz : 0), 6);
        }
    for (int k = 0; k < 5; k++) {
        oc[k] = p3_gate_outcode(v, W[k], g->sh, xlo, xhi);
        all &= oc[k]; any |= oc[k];
    }
    if (all) return 0;
    int edges = 0;
    for (int k = 0; k < 5; k++) edges += (oc[k] & oc[(k + 1) % 5]) == 0;
    return edges > 0 || (g->face < 0 && (any & 0xF) == 0xF);
}

/** @brief Fills one preview as the pass's block would hold it for a record it did not draw.
 *
 * The depth is d >> 7; the far colour is R = G = (cos entry >> 4) + 0xE00
 * and B = 0xC00 at GTE scale, the cos entry at index (far_counter * 4 +
 * record index * 64) & 0xFF (func_8004C66C.s:359-398,
 * func_8001EDEC.s:505-541); IR0 is 0xFFF from depth 0x5000, depth - 0x4000
 * from 0x4000, else 0 (func_8004C66C.s:679-698, func_8001EDEC.s:825-845).
 * The rect and the edges stay empty: the feeder re-derives the opening
 * from the record (p3_native_opening).
 * @param cpu Reads the record, the counter and the cos table.
 * @param p Receives the preview.
 * @param rec The record.
 * @param idx The record's index.
 * @param g The record's gate results.
 * @param arg 1 for the background, 0 for the picture.
 * @param slot The portal's slot. */
static void p3_band_fill(CPUState* cpu, p3_preview* p, uint32_t rec, uint32_t idx, const p3_gate* g,
                         int arg, int slot) {
    memset(p, 0, sizeof *p);
    p->mesh = cpu->read_word(rec + 0x3C) + (arg ? 0u : 0x3Cu);
    p->arg = arg;
    p->depth7 = g->d >> 7;
    p->depth_w = (float)(p->depth7 << 7);
    const uint32_t ci = (cpu->read_word(portal_game_s23.far_counter) * 4u + idx * 64u) & 0xFFu;
    const int32_t h = (int16_t)cpu->read_half(game_layout.sin_table + 0x80u + ci * 2u);
    const uint32_t rg = ((uint32_t)(uint16_t)((h >> 4) + 0xE00) >> 4) & 0xFFu;
    p->far_rgb = rg | (rg << 8) | (((0xC00u >> 4) & 0xFFu) << 16);
    p->ir0 = g->d >= 0x5000 ? 0xFFF : g->d >= 0x4000 ? g->d - 0x4000 : 0;
    p->slot = slot;
    p->rec = rec;
}

/** @brief Whether OPENPETE_PORTAL3_BAND_AB selects the band admission's A/B instrument.
 *
 * Latched on the first call; set, non-empty and not starting with '0'
 * enables it. Under it p3_band_admit discards the pass's captures and admits
 * every record whose gates and wide-frame outcode test pass, so a capture
 * compares the native derivation with the pass's own on the same portals.
 * @return 1 when enabled, else 0. */
static int p3_band_ab(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_PORTAL3_BAND_AB"); v = (e && *e && *e != '0'); }
    return v;
}

/* Per-pass engagement of the band admission, for the census. */
static int s_band_admit = 0;     /* records added this pass */
static int s_band_disagree = 0;  /* records the 4:3 test admits that the pass did not draw */
static int s_band_far = 0;       /* records added that lie at or past their draw radius */

/** @brief Whether the tick's capture names a record.
 * @param T The tick.
 * @param rec The record.
 * @param mesh The record's mesh, matched for a capture with no record.
 * @return 1 when a preview of the tick belongs to it, else 0. */
static int p3_tick_has(const p3_tick* T, uint32_t rec, uint32_t mesh) {
    for (int i = 0; i < T->n; i++)
        if (T->p[i].rec == rec || (!T->p[i].rec && T->p[i].mesh == mesh)) return 1;
    return 0;
}

/** @brief The ratio of the view's screen-X half-tangent to the 4:3 frame's, floored at 1.
 *
 * The half-tangent is the larger screen-X distance from OFX to a bound,
 * over the projection distance: [xlo, xhi] over H_eff for the view, [0, 512]
 * over H_canon for the frame the pass tests. Scaling the pass's cone slope by
 * it keeps the cone's angular margin to the frustum edge of the view.
 * @param v The view.
 * @return The scale, 1.0 for a view no wider than the 4:3 frame. */
static double p3_cone_scale(const ngte_view* v) {
    const double wide = fmax(v->ofx - (double)v->xlo, (double)v->xhi - v->ofx) / v->H_eff;
    const double canon = fmax(v->ofx - (double)P3_FRAME_X0, (double)P3_FRAME_X1 - v->ofx) / v->H_canon;
    const double k = canon > 0.0 ? wide / canon : 1.0;
    return k > 1.0 ? k : 1.0;
}

/** @brief Adds one record the pass did not draw to the tick when only the 4:3 frame's cone or X bounds, or under extended LOD its draw radius, rejected it.
 *
 * The depth and facing gates, then per call the outcode test, each twice:
 * as the pass runs them (the cone at slope 1, the X bits at [P3_FRAME_X0,
 * P3_FRAME_X1] under the canonical view) and for the view (the cone scaled
 * by p3_cone_scale, the X bits at the view's screen-X range under its own).
 * A call that fails the first and passes the second is appended as the pass
 * would have captured it (the background first, the picture only below
 * depth 0x5000) under the next slot. A call that passes the 4:3 evaluation
 * is counted in s_band_disagree and not added, since the pass's verdict
 * stands; under p3_band_ab the 4:3 evaluation is skipped. Under
 * p3_lod_picture the view's depth gate is radius-free (p3_gate_depth), so a
 * record past its draw radius is appended as its background, which
 * p3_lod_tick follows with the picture; each such record is counted in
 * s_band_far.
 * @param cpu Reads the record and game state.
 * @param T The tick; receives the previews.
 * @param vw The view being presented.
 * @param vc Its canonical 4:3 counterpart.
 * @param rec The record.
 * @param idx Its index.
 * @return 1 when a preview was added, else 0. */
static int p3_band_record(CPUState* cpu, p3_tick* T, const ngte_view* vw, const ngte_view* vc,
                          uint32_t rec, uint32_t idx) {
    p3_gate g, g43;
    if (!p3_gate_depth(cpu, rec, p3_cone_scale(vw), p3_lod_picture(), &g) || !p3_gate_facing(cpu, rec, &g)) return 0;
    const int ok43 = p3_gate_depth(cpu, rec, 1.0, 0, &g43) && p3_gate_facing(cpu, rec, &g43);
    int added = 0;
    for (int call = 1; call >= 0 && T->n < P3_MAX_PORTALS * 2; call--) {
        if (call == 0 && g.d >= 0x5000) break;
        if (!p3_band_ab() && ok43 &&
            p3_gate_call(cpu, vc, rec, &g43, call, (float)P3_FRAME_X0, (float)P3_FRAME_X1)) {
            s_band_disagree++;
            continue;
        }
        if (!p3_gate_call(cpu, vw, rec, &g, call, vw->xlo, vw->xhi)) continue;
        const int slot = T->n == 0 ? 0 : T->p[T->n - 1].slot + (added ? 0 : 1);
        p3_band_fill(cpu, &T->p[T->n++], rec, idx, &g, call, slot);
        added = 1;
    }
    s_band_far += added && g.beyond;
    return added;
}

/** @brief Adds to the tick every record the pass rejected only because the 4:3 frame's cone or X bounds, or under extended LOD its draw radius, culled it.
 *
 * Runs p3_band_record on every record the tick's capture does not name and
 * marks RP_PORTAL drawn when it added any. Returns at once when the view is
 * no wider than the 4:3 frame, except under p3_lod_picture, where a record
 * the draw radius alone rejects is admitted at every aspect. Under
 * p3_band_ab the pass's captures are discarded first. Reads PSX RAM only.
 * @param cpu The guest CPU state.
 * @param T The tick the pass just captured. */
static void p3_band_admit(CPUState* cpu, p3_tick* T) {
    s_band_admit = s_band_disagree = s_band_far = 0;
    ngte_view vw, vc;
    ngte_view_build(cpu, &vw);
    if (!p3_lod_picture() && !(vw.xlo < (float)P3_FRAME_X0 || vw.xhi > (float)P3_FRAME_X1)) return;
    ngte_view_canon(&vw, &vc);
    const uint32_t base = cpu->read_word(portal_game_s23.records);
    const uint32_t n = cpu->read_word(portal_game_s23.count);
    if (base < 0x80000000u || base >= 0x80200000u || n > 256u || !game_layout.sin_table) return;
    if (p3_band_ab()) T->n = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t rec = base + i * P3_REC_STRIDE;
        if (!p3_tick_has(T, rec, cpu->read_word(rec + 0x3C)))
            s_band_admit += p3_band_record(cpu, T, &vw, &vc, rec, i);
    }
    /* A pass that drew nothing itself records no polybuf interval; the mark
     * keeps the sub-tick replay feeding the added previews. */
    if (s_band_admit) render_path_mark_drawn(RP_PORTAL);
}

/** @brief The capture of one record's call in the tick, or NULL.
 * @param T The tick.
 * @param rec The record.
 * @param arg The leaf argument.
 * @return The preview. */
static const p3_preview* p3_tick_find(const p3_tick* T, uint32_t rec, int arg) {
    for (int i = 0; i < T->n; i++)
        if (T->p[i].rec == rec && T->p[i].arg == arg) return &T->p[i];
    return NULL;
}

/* The gate census's running counts over one pass. */
typedef struct { int calls, agree, fields, fmis; } p3_gate_census;

/** @brief Compares one record's native gate evaluation at 4:3 with the pass's own captures, printing every disagreement.
 *
 * Per call, the verdict p3_gate_call returns at [P3_FRAME_X0, P3_FRAME_X1]
 * under the canonical projection against whether the pass called the leaf;
 * for a call both admit, the depth >> 7, far colour, IR0 and mesh
 * p3_band_fill derives against the captured block.
 * @param cpu Reads the record and game state.
 * @param T The tick as the pass captured it.
 * @param vc The canonical view.
 * @param rec The record.
 * @param idx Its index.
 * @param c Accumulates the counts. */
static void p3_band_census_record(CPUState* cpu, const p3_tick* T, const ngte_view* vc, uint32_t rec,
                                  uint32_t idx, p3_gate_census* c) {
    p3_gate g;
    const int ok = p3_gate_depth(cpu, rec, 1.0, 0, &g) && p3_gate_facing(cpu, rec, &g);
    for (int call = 1; call >= 0; call--) {
        const p3_preview* cap = p3_tick_find(T, rec, call);
        const int mine = ok && !(call == 0 && g.d >= 0x5000) &&
                         p3_gate_call(cpu, vc, rec, &g, call, (float)P3_FRAME_X0, (float)P3_FRAME_X1);
        c->calls++;
        c->agree += mine == (cap != NULL);
        if (mine != (cap != NULL))
            fprintf(stderr, "[portal3-gate-mism f=%u rec=%u call=%d native=%d pass=%d d=%d]\n",
                    openpete_frame, idx, call, mine, cap != NULL, ok ? g.d : -1);
        if (!mine || !cap) continue;
        p3_preview q;
        p3_band_fill(cpu, &q, rec, idx, &g, call, 0);
        c->fields++;
        if (q.depth7 == cap->depth7 && q.far_rgb == cap->far_rgb && q.ir0 == cap->ir0 && q.mesh == cap->mesh) continue;
        c->fmis++;
        fprintf(stderr, "[portal3-gate-field f=%u rec=%u call=%d d7=%d/%d far=%06X/%06X ir0=%d/%d mesh=%08X/%08X]\n",
                openpete_frame, idx, call, q.depth7, cap->depth7, q.far_rgb, cap->far_rgb, q.ir0, cap->ir0,
                q.mesh, cap->mesh);
    }
}

/** @brief Prints the gate census of one pass: the native 4:3 evaluation of every record against the pass's captures.
 *
 * Runs before p3_band_admit adds anything to the tick. Reads PSX RAM only.
 * @param cpu The guest CPU state.
 * @param T The tick as the pass captured it. */
static void p3_band_census(CPUState* cpu, const p3_tick* T) {
    const uint32_t base = cpu->read_word(portal_game_s23.records);
    const uint32_t n = cpu->read_word(portal_game_s23.count);
    if (base < 0x80000000u || base >= 0x80200000u || n > 256u || !game_layout.sin_table) return;
    ngte_view vw, vc;
    ngte_view_build(cpu, &vw);
    ngte_view_canon(&vw, &vc);
    p3_gate_census c = { 0, 0, 0, 0 };
    for (uint32_t i = 0; i < n; i++) p3_band_census_record(cpu, T, &vc, base + i * P3_REC_STRIDE, i, &c);
    fprintf(stderr, "[portal3-gate f=%u recs=%u calls=%d agree=%d fields=%d field-mism=%d]\n",
            openpete_frame, n, c.calls, c.agree, c.fields, c.fmis);
}

/* ---- hooks ------------------------------------------------------------ */
/** @brief The preview leaf's hook: captures the parameter block, then runs the real leaf.
 *
 * Only inside the portal pass hook and only when the native scene is enabled
 * or OPENPETE_PORTAL3_DBG is set; otherwise, and on the re-entry the dispatch
 * makes, it returns 0 and the leaf runs unhooked. Under OPENPETE_PORTAL3_DBG
 * the first six calls from p3_dbg_from dump up to six of the leaf's F3 and G3
 * prims for the census.
 * @param cpu The guest CPU state; a0 is the leaf's argument.
 * @param addr The leaf's address, dispatched.
 * @return 1 when the hook ran the leaf, else 0. */
static int on_portal_leaf(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    int feed = openpete_native_scene_enabled();
    if (!feed && !p3_dbg()) return 0;
    if (!s_in_pass) return 0;
    static int s_in = 0;              /* the dispatch below re-enters this hook */
    if (s_in) return 0;
    s_in = 1;
    int arg = (int32_t)cpu->a0;
    p3_capture(cpu, arg);
    s_leaf_calls++;
    uint32_t start = cpu->read_word(game_layout.polybuf.cursor);
    { OP_PROF_FLAGS_ACCUM_SCOPE("portal.srend", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      openpete_recomp_module_get()->dispatch(cpu, addr); }
    uint32_t end = cpu->read_word(game_layout.polybuf.cursor);
    if (p3_dbg()) {
        /* The leaf's prims, for the census: F3 0x14 bytes, G3 0x1C, the size
         * byte of the link word naming the type. */
        static unsigned s_dumps = 0;
        if (s_dumps < 6 && openpete_feed_key_now() >= p3_dbg_from()) {
            s_dumps++;
            fprintf(stderr, "[portal3-prims f=%u arg=%d] bytes=%u:", openpete_frame, arg, end - start);
            uint32_t q = start; int n = 0;
            while (q + 0x14 <= end && n < 6) {
                uint32_t w0 = cpu->read_word(q), code = w0 >> 24;
                if (code == 0x04) {
                    fprintf(stderr, " F3 rgb=%06X xy=(%d,%d)(%d,%d)(%d,%d)", cpu->read_word(q + 4) & 0xFFFFFF,
                            (int16_t)(cpu->read_word(q + 8) & 0xFFFF), (int16_t)(cpu->read_word(q + 8) >> 16),
                            (int16_t)(cpu->read_word(q + 12) & 0xFFFF), (int16_t)(cpu->read_word(q + 12) >> 16),
                            (int16_t)(cpu->read_word(q + 16) & 0xFFFF), (int16_t)(cpu->read_word(q + 16) >> 16));
                    q += 0x14;
                } else if (code == 0x06) {
                    fprintf(stderr, " G3 rgb=%06X/%06X/%06X xy=(%d,%d)(%d,%d)(%d,%d)",
                            cpu->read_word(q + 4) & 0xFFFFFF, cpu->read_word(q + 12) & 0xFFFFFF, cpu->read_word(q + 20) & 0xFFFFFF,
                            (int16_t)(cpu->read_word(q + 8) & 0xFFFF), (int16_t)(cpu->read_word(q + 8) >> 16),
                            (int16_t)(cpu->read_word(q + 16) & 0xFFFF), (int16_t)(cpu->read_word(q + 16) >> 16),
                            (int16_t)(cpu->read_word(q + 24) & 0xFFFF), (int16_t)(cpu->read_word(q + 24) >> 16));
                    q += 0x1C;
                } else { fprintf(stderr, " ?%02X", code); q += 4; }
                n++;
            }
            fprintf(stderr, "\n");
        }
    }
    s_in = 0;
    return 1;
}

/* The dispatcher's world-draw wrapper stores its $ra at sp+0x10
 * [CITE: games/spyro-3/spyro-3/asm/nonmatchings/drawutil/func_8001EC24.s:10;
 * games/spyro-2/spyro-2/asm/nonmatchings/drawutil/func_8004C4FC.s:12], and the
 * dispatcher runs the portal pass under bit 0x8 of the flag word it keeps in
 * $s3 [CITE: games/spyro-3/spyro-3/asm/nonmatchings/draw/func_8001E460.s:59-71;
 * games/spyro-2/spyro-2/asm/nonmatchings/draw/func_800155A0.s:61-67]. */
#define P3_WRAP_RA_SLOT  0x10u
#define P3_FLAG_PORTALS  0x8u

int portal_s23_capture_follows(CPUState* cpu) {
    if (!portal_game_s23.env_draw_ret || !portal_game_s23.env_wrap_ret) return 0;
    return cpu->ra == portal_game_s23.env_draw_ret
        && cpu->read_word(cpu->sp + P3_WRAP_RA_SLOT) == portal_game_s23.env_wrap_ret
        && (cpu->s3 & P3_FLAG_PORTALS) != 0
        && s_curr.frame != openpete_frame;
}

/** @brief Queues the destination-sky build of the first portal record without one, one record per pass.
 *
 * Spreads the destination sky reads over the level's first passes and starts
 * every record's registration on the background thread before its picture
 * is in view (portal_sky_s23_prefetch). Reads PSX RAM only.
 * @param cpu Reads the record array. */
static void p3_sky_prefetch(CPUState* cpu) OP_REQUIRES(sim) {
    if (!portal_game_s23.records || !portal_game_s23.count) return;
    const uint32_t base = cpu->read_word(portal_game_s23.records);
    const uint32_t n = cpu->read_word(portal_game_s23.count);
    if (base < 0x80000000u || base >= 0x80200000u || n > 16u) return;
    for (uint32_t i = 0; i < n; i++)
        if (portal_sky_s23_prefetch(cpu, base + i * P3_REC_STRIDE)) return;
}

/** @brief The portal pass's hook: rolls the tick buffers, runs the real pass, then feeds the previews.
 *
 * Returns 0 (the pass runs unhooked) when neither the native scene, a
 * --render-paths selection nor OPENPETE_PORTAL3_DBG is active, and on
 * re-entry. On a new openpete_frame the current tick becomes the previous one
 * (valid for up to 3 frames back) and the current list is emptied. Sets
 * s_in_pass so the leaf hook captures, dispatches the pass once, records the
 * pass's polybuf interval under RP_PORTAL, and feeds the captured previews
 * when the native scene is enabled, after queueing one record's destination
 * sky build (p3_sky_prefetch) unless OPENPETE_PORTAL3_SKY is 0. Then
 * releases a terrain feed the world draw held for this capture
 * (portal_s23_capture_follows). Under
 * OPENPETE_PORTAL3_DBG prints up to 64 capture reports. Writes no PSX RAM
 * itself.
 * @param cpu The guest CPU state.
 * @param addr The pass's address, dispatched.
 * @return 1 after running the pass, else 0. */
static int on_portal_pass(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    int feed = openpete_native_scene_enabled();
    if (!feed && !render_paths_active() && !p3_dbg()) return 0;
    static int s_in = 0;
    if (s_in) return 0;
    s_in = 1;
    if (openpete_frame != s_curr.frame) {
        /* The epoch's first pass retires the current tick into s_prev; a
         * speculative render inside the epoch replaces the current tick only. */
        const uint32_t epoch = s_sets.epoch[0];
        cut_held_sets_capture(&s_sets);
        if (s_sets.epoch[0] != epoch)
            s_prev.valid = s_prev.valid && (openpete_frame - s_prev.frame <= 3u);
        s_curr.n = 0; s_curr.frame = openpete_frame; s_curr.valid = 1;
    }
    s_leaf_calls = 0;
    s_rec_next = 0;
    s_in_pass = 1;
    s_pass_start = cpu->read_word(game_layout.polybuf.cursor);
    { OP_PROF_FLAGS_ACCUM_SCOPE("portal.srend", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      openpete_recomp_module_get()->dispatch(cpu, addr); }
    uint32_t end = cpu->read_word(game_layout.polybuf.cursor);
    s_in_pass = 0;
    render_path_record(RP_PORTAL, s_pass_start, end);
    if (p3_dbg() && openpete_feed_key_now() >= p3_dbg_from()) p3_band_census(cpu, &s_curr);
    if (feed && p3_sky_on()) p3_sky_prefetch(cpu);
    if (feed) {
        p3_band_admit(cpu, &s_curr);
        p3_feed(cpu, &s_curr);
    }
    { OP_PROF_ACCUM_SCOPE("portal.defer.terrain");  /* timed as terrain_native's */
      terrain_native_flush_deferred(); }      /* a world draw that ran first this tick */
    if (p3_dbg()) {
        static unsigned s_reports = 0, s_passes = 0;
        s_passes++;
        /* The first passes report even with nothing captured: a pass that runs
         * and captures nothing is the reading that separates "no portal in
         * view" from "the hook never fired". */
        if (s_reports < 64 && (s_curr.n || s_passes <= 4) && openpete_feed_key_now() >= p3_dbg_from()) {
            s_reports++;
            fprintf(stderr, "[portal3 f=%u] leaf-calls=%d captured=%d band=%d band-far=%d band-disagree=%d prim-bytes=%u emitted=%d faces=%d idx-drop=%d oc-drop=%d edge-drop=%d tris=%d",
                    openpete_frame, s_leaf_calls, s_curr.n, s_band_admit, s_band_far, s_band_disagree, end - s_pass_start, s_emitted,
                    s_cen_faces, s_cen_idx, s_cen_oc, s_cen_edge, s_cen_tris);
            for (int i = 0; i < s_curr.n; i++) {
                const p3_preview* p = &s_curr.p[i];
                fprintf(stderr, " | slot%d arg%d mesh=%08X rect=[%d,%d]x[%d,%d] d7=%d ir0=%d edges=%d",
                        p->slot, p->arg, p->mesh, p->xmin, p->xmax, p->ymin, p->ymax, p->depth7, p->ir0, p->nedge);
            }
            fprintf(stderr, "\n");
        }
    }
    s_in = 0;
    return 1;
}

/** @brief The previous tick's record for one preview.
 *
 * The portal's identity is its preview mesh, since the pass's slot numbering
 * is the order of the portals it gated this tick and shifts whenever one
 * enters or leaves the set; the leaf argument separates the backdrop record
 * from the picture record of the same portal. A record whose slot agrees as
 * well is returned at once, which settles two portals previewing one
 * destination; otherwise the match is taken only when it is the one match.
 * @param prev The previous tick.
 * @param c The current preview.
 * @return The partner, or NULL when there is none or several without a slot match. */
static const p3_preview* p3_pair(const p3_tick* prev, const p3_preview* c) {
    const p3_preview* hit = NULL;
    int n = 0;
    for (int i = 0; i < prev->n; i++) {
        const p3_preview* p = &prev->p[i];
        if (p->arg != c->arg || p->mesh != c->mesh) continue;
        if (p->slot == c->slot) return p;
        hit = p;
        n++;
    }
    return n == 1 ? hit : NULL;
}

/** @brief One preview's captured state at the instant `a` between its two ticks.
 *
 * The pentagon edges pair by index over the count the two ticks share; a tick
 * that shows more edges than its partner holds the extra ones at its own,
 * since the partner has no line for them. The clip rect and the two fade terms
 * pair outright. The depth is lerped in world units rather than in the
 * captured depth >> 7: the native path takes it as a view-Z, so the step the
 * capture quantizes to 128 units is a step in the order the preview occludes
 * terrain, and the interval between the two ticks is continuous.
 *
 * The preview mesh itself is not interpolated: it is baked screen-space art
 * whose vertices are identical from tick to tick, and the aperture is the only
 * thing that moves.
 * @param O The output; must hold the current capture, whose mesh, slot and
 *        extra edges are kept.
 * @param P The previous tick's record.
 * @param C The current tick's record.
 * @param a The sub-tick alpha. */
static void p3_lerp_preview(p3_preview* O, const p3_preview* P, const p3_preview* C,
                            float a) {
    O->xmin = (int)lroundf((float)P->xmin + (float)(C->xmin - P->xmin) * a);
    O->xmax = (int)lroundf((float)P->xmax + (float)(C->xmax - P->xmax) * a);
    O->ymin = (int)lroundf((float)P->ymin + (float)(C->ymin - P->ymin) * a);
    O->ymax = (int)lroundf((float)P->ymax + (float)(C->ymax - P->ymax) * a);
    O->depth_w = P->depth_w + (C->depth_w - P->depth_w) * a;
    O->ir0 = (int)lroundf((float)P->ir0 + (float)(C->ir0 - P->ir0) * a);
    for (int k = 0; k < 24; k += 8) {
        const int pc = (int)((P->far_rgb >> k) & 0xFFu), cc = (int)((C->far_rgb >> k) & 0xFFu);
        const uint32_t lc = (uint32_t)(int)lroundf((float)pc + (float)(cc - pc) * a);
        O->far_rgb = (O->far_rgb & ~(0xFFu << k)) | (lc << k);
    }
    const int ne = P->nedge < C->nedge ? P->nedge : C->nedge;
    for (int e = 0; e < ne; e++)
        for (int k = 0; k < 2; k++) {
            O->ex[e][k] = P->ex[e][k] + (C->ex[e][k] - P->ex[e][k]) * a;
            O->ey[e][k] = P->ey[e][k] + (C->ey[e][k] - P->ey[e][k]) * a;
        }
}

void portal_s23_reextract_enter(int hold) {
    s_pick = cut_held_sets_enter(&s_sets, hold);
}

void portal_s23_reextract_leave(void) {
    cut_held_sets_leave(&s_sets, s_pick);
    s_pick = -1;
}

/* Sub-tick re-extract: replays the tick's previews, lerped per portal. */
void portal_s23_reextract(CPUState* cpu, double alpha, int interp_on) OP_REQUIRES(sim) {
    /* Scene on, selected, and canon-drawn; on a held cut the set's own epoch
     * stands in for the canon-drawn term. */
    if (!hook_reextract_ready_held(RP_PORTAL, s_pick)) return;
    if (!s_curr.valid || s_curr.n == 0) return;
    p3_tick T = s_curr;
    /* At alpha 1 the present is the current tick's own view: replay the
     * capture unchanged, so the identity present reproduces the canonical one
     * bit for bit (a lerp evaluated at 1 rounds differently). A held set is
     * tick N-1's capture shown under tick N-1's camera, replayed unchanged. */
    int can_lerp = s_pick < 0 && interp_on && p3_interp_on() && alpha < 0.999 && s_prev.valid;
    if (can_lerp) {
        const float a = (float)alpha;
        for (int i = 0; i < T.n; i++) {
            const p3_preview* P = p3_pair(&s_prev, &s_curr.p[i]);
            if (P) p3_lerp_preview(&T.p[i], P, &s_curr.p[i], a);
        }
    }
    s_feed_src = "sub"; s_feed_alpha = (float)alpha;
    p3_feed(cpu, &T);
    s_feed_src = "canon"; s_feed_alpha = 1.0f;
}

/* The two hook addresses come from the game's own table, so the table is
 * filled at boot rather than written out here. A game that names neither
 * registers nothing and never enters this feeder. */
static feature_dispatch_entry_t portal_dispatch[3];

/** @brief Fills the dispatch table with the game's portal pass and leaf addresses.
 *
 * An address of 0 registers nothing; the table ends with a zero entry.
 * @param opts Unused. */
static void portal_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    const feature_dispatch_entry_t hooks[] = {
        { .addr = portal_game_s23.pass, .fn = on_portal_pass },
        { .addr = portal_game_s23.leaf, .fn = on_portal_leaf },
    };
    int n = 0;
    for (size_t i = 0; i < sizeof hooks / sizeof hooks[0]; i++)
        if (hooks[i].addr) { portal_dispatch[n].addr = hooks[i].addr; portal_dispatch[n].fn = hooks[i].fn; n++; }
    portal_dispatch[n].addr = 0u;
    portal_dispatch[n].fn   = NULL;
}

const feature_module_t portal_module = {
    .name           = "portal_native",
    .apply_options  = portal_apply_options,
    .dispatch_table = portal_dispatch,
};
