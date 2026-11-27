/* tests/unit/line_quad_test.c - ngte_line_quad covers the pixels the PSX GPU
 * draws for a line.
 *
 * The reference is psx_gpu_line_pixels (psx_gpu_line.c), which
 * gpu_line_table_test holds to the line pixels measured on the oracle's
 * reference GPU (data/gpu-line/line.pixels). Over every line from a fixed
 * start pixel to each end offset in [-RANGE, RANGE]^2 (zero length,
 * the axis-aligned and diagonal cases included), the quad ngte_line_quad
 * builds on the integer endpoints (centre offset 0.5) is rasterized at 1x by
 * pixel centres. A pixel whose centre lies inside the quad must be drawn by
 * the reference and one whose centre lies outside must not; a centre on the
 * quad's edge is a tie, where the GPU's minor-axis rounding picks one side
 * and the native rasterizer's fill rule may pick the other. The test fails
 * on any non-tie difference and prints the tie difference count. The
 * one-pixel quad across the line's perpendicular, the rule the feeders used
 * before, runs on the same sweep as the negative control: it must disagree
 * with the reference off the ties, or the sweep did not reach the case. */

#include "renderer/native_gte.h"
#include "openpete_native_render.h"
#include "renderer/orchestrator.h"
#include "game_layout.h"
#include "psx_gpu_line.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* native_gte.c's references outside ngte_line_quad, none of which the test
 * reaches. */
game_layout_t game_layout OP_SEALED(game_layout); /* thread-roles: never written; native_gte.c's reads are not reached */
/** @brief Link stub; the test feeds no triangle.
 * @param tri Unused.
 * @param category Unused.
 * @param carry Unused. */
void openpete_native_scene_add_tri(const openpete_scene_tri* tri, int category, int carry) OP_REQUIRES_SHARED(sim)
{ (void)tri; (void)category; (void)carry; }
/** @brief Link stub.
 * @return 1. */
float openpete_native_widescreen_factor(void) { return 1.0f; }
/** @brief Link stub.
 * @param xlo Receives 0.
 * @param xhi Receives 512. */
void openpete_native_widescreen_x_bounds(float* xlo, float* xhi) { *xlo = 0.0f; *xhi = 512.0f; }
/** @brief Link stub.
 * @param H_canon Unused.
 * @param y_row Unused.
 * @return 1. */
double openpete_native_fov_h_scale(double H_canon, double y_row) { (void)H_canon; (void)y_row; return 1.0; }
/** @brief Link stub.
 * @param H_canon Unused.
 * @param y_row Unused. */
void openpete_native_fov_note_canon(double H_canon, double y_row) { (void)H_canon; (void)y_row; }
/** @brief Link stub.
 * @param key Unused.
 * @param out Unused.
 * @return 0, no interpolated position. */
int openpete_native_interp_pos(unsigned int key, double out[3]) { (void)key; (void)out; return 0; }

#define RANGE 64            /* end offsets in [-RANGE, RANGE] on each axis */
#define X0 100              /* start pixel */
#define Y0 100
#define GRID 256            /* pixel grid, covers X0 +- RANGE + 2 */

/** @brief Marks one reference pixel in a GRID x GRID grid.
 * @param ctx The grid, indexed [y][x].
 * @param x The pixel column.
 * @param y The pixel row. */
static void ref_plot(void* ctx, int x, int y)
{
    uint8_t (*px)[GRID] = ctx;
    if (x >= 0 && x < GRID && y >= 0 && y < GRID) px[y][x] = 1;
}

/** @brief The perpendicular one-pixel quad of the negative control.
 * @param ax Endpoint a's x.
 * @param ay Endpoint a's y.
 * @param bx Endpoint b's x.
 * @param by Endpoint b's y.
 * @param qx Receives the corners' x in ngte_line_quad's order.
 * @param qy Receives the corners' y. */
static void perp_quad(float ax, float ay, float bx, float by, float qx[4], float qy[4])
{
    ax += 0.5f; ay += 0.5f; bx += 0.5f; by += 0.5f;
    const float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
    float ox = 0.5f, oy = 0.0f;
    if (len > 1e-3f) { ox = -dy / len * 0.5f; oy = dx / len * 0.5f; }
    qx[0] = ax - ox; qx[1] = bx - ox; qx[2] = ax + ox; qx[3] = bx + ox;
    qy[0] = ay - oy; qy[1] = by - oy; qy[2] = ay + oy; qy[3] = by + oy;
}

/** @brief Classifies a point against a convex quad given in ngte_line_quad's corner order.
 * @param qx The corners' x.
 * @param qy The corners' y.
 * @param px The point's x.
 * @param py The point's y.
 * @return 1 inside, 0 on an edge (within 1e-4 px), -1 outside. */
static int quad_side(const float qx[4], const float qy[4], double px, double py)
{
    static const int ring[4] = { 0, 1, 3, 2 };
    double area = 0.0;
    for (int e = 0; e < 4; e++) {
        const int a = ring[e], b = ring[(e + 1) & 3];
        area += (double)qx[a] * qy[b] - (double)qx[b] * qy[a];
    }
    const double s = area < 0.0 ? -1.0 : 1.0;
    int on_edge = 0;
    for (int e = 0; e < 4; e++) {
        const int a = ring[e], b = ring[(e + 1) & 3];
        const double ex = (double)qx[b] - qx[a], ey = (double)qy[b] - qy[a];
        const double len = sqrt(ex * ex + ey * ey);
        if (len < 1e-9) continue;
        const double d = s * (ex * (py - qy[a]) - ey * (px - qx[a])) / len;
        if (d < -1e-4) return -1;
        if (d <= 1e-4) on_edge = 1;
    }
    return on_edge ? 0 : 1;
}

typedef struct { long lines, ref_px, ties, miss, extra, bad_lines; } sweep_t;

/** @brief Compares one quad against the reference pixels of its line and accumulates the counts.
 * @param qx The quad corners' x.
 * @param qy The quad corners' y.
 * @param ref The reference pixels.
 * @param x1 The line's end x, bounding the scan.
 * @param y1 The line's end y.
 * @param sw The accumulator. */
static void compare(const float qx[4], const float qy[4], uint8_t ref[GRID][GRID],
                    int x1, int y1, sweep_t* sw)
{
    const int xl = (x1 < X0 ? x1 : X0) - 2, xh = (x1 > X0 ? x1 : X0) + 2;
    const int yl = (y1 < Y0 ? y1 : Y0) - 2, yh = (y1 > Y0 ? y1 : Y0) + 2;
    long bad = 0;
    for (int y = yl; y <= yh; y++) {
        for (int x = xl; x <= xh; x++) {
            const int side = quad_side(qx, qy, x + 0.5, y + 0.5);
            const int r = ref[y][x];
            sw->ref_px += r;
            if (side == 0) { sw->ties++; continue; }
            if (side > 0 && !r) { sw->extra++; bad++; }
            if (side < 0 && r) { sw->miss++; bad++; }
        }
    }
    if (bad) sw->bad_lines++;
    sw->lines++;
}

/** @brief Runs the sweep for one quad builder.
 * @param use_perp 0 for ngte_line_quad, 1 for the negative control.
 * @param sw Receives the counts. */
static void sweep(int use_perp, sweep_t* sw)
{
    static uint8_t ref[GRID][GRID];
    memset(sw, 0, sizeof *sw);
    for (int dy = -RANGE; dy <= RANGE; dy++) {
        for (int dx = -RANGE; dx <= RANGE; dx++) {
            const int x1 = X0 + dx, y1 = Y0 + dy;
            memset(ref, 0, sizeof ref);
            psx_gpu_line_pixels(X0, Y0, x1, y1, ref_plot, ref);
            float qx[4], qy[4];
            if (use_perp) perp_quad((float)X0, (float)Y0, (float)x1, (float)y1, qx, qy);
            else ngte_line_quad((float)X0, (float)Y0, (float)x1, (float)y1, 0.5f, qx, qy);
            compare(qx, qy, ref, x1, y1, sw);
        }
    }
}

/** @brief Counts the tie centres the reference draws over the sweep.
 *
 * A strict-inside fill rule draws no tie pixel, so this is the difference
 * count under that rule.
 * @return The number of reference-drawn tie pixels. */
static long tie_drawn(void)
{
    static uint8_t ref[GRID][GRID];
    long n = 0;
    for (int dy = -RANGE; dy <= RANGE; dy++) {
        for (int dx = -RANGE; dx <= RANGE; dx++) {
            const int x1 = X0 + dx, y1 = Y0 + dy;
            memset(ref, 0, sizeof ref);
            psx_gpu_line_pixels(X0, Y0, x1, y1, ref_plot, ref);
            float qx[4], qy[4];
            ngte_line_quad((float)X0, (float)Y0, (float)x1, (float)y1, 0.5f, qx, qy);
            for (int y = Y0 - RANGE - 2; y <= Y0 + RANGE + 2; y++)
                for (int x = X0 - RANGE - 2; x <= X0 + RANGE + 2; x++)
                    if (ref[y][x] && quad_side(qx, qy, x + 0.5, y + 0.5) == 0) n++;
        }
    }
    return n;
}

/** @brief Runs the sweep and the negative control.
 * @return 0 when ngte_line_quad differs from the reference only on ties and the control does not. */
int main(void)
{
    sweep_t q, p;
    sweep(0, &q);
    sweep(1, &p);
    const long drawn_ties = tie_drawn();
    printf("line_quad: %ld lines, %ld reference px; ngte_line_quad: %ld non-tie diffs "
           "(%ld missing, %ld extra) on %ld lines, %ld tie centres of which the reference draws %ld\n",
           q.lines, q.ref_px, q.miss + q.extra, q.miss, q.extra, q.bad_lines, q.ties, drawn_ties);
    printf("line_quad: negative control (perpendicular 1 px quad): %ld non-tie diffs "
           "(%ld missing, %ld extra) on %ld lines\n",
           p.miss + p.extra, p.miss, p.extra, p.bad_lines);
    int fail = 0;
    if (q.ref_px == 0) { fprintf(stderr, "line_quad: VACUOUS, no reference pixel\n"); fail = 1; }
    if (q.miss + q.extra) { fprintf(stderr, "line_quad: FAIL, non-tie differences\n"); fail = 1; }
    if (p.miss + p.extra == 0) { fprintf(stderr, "line_quad: FAIL, the negative control matched\n"); fail = 1; }
    if (!fail) printf("line_quad: PASS\n");
    return fail;
}
