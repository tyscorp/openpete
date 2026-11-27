// games/common/renderer/native_gte.c - see native_gte.h. The one
// implementation of the GTE mirror math shared by every native feeder; a
// change here moves every feeder.

#include "renderer/native_gte.h"
#include "game_layout.h"           /* cam.base, sin_table */
#include "openpete_native_render.h"
#include "renderer/orchestrator.h" /* openpete_native_interp_pos */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "openpete_thread_roles.h"

ngte_census_t ngte_census;
int ngte_census_on;
/** @brief Prints the --fov census counters to stderr; registered to run at exit. */
static void ngte_census_print(void) {
    OP_ROLE_ASSERT(sim);
    static const char* site[NGTE_SITE_N] = { "raw_sat", "gte_sat", "part_cap", "moby_near", "terrain_unr" };
    fprintf(stderr, "[fov-census]");
    for (int i = 0; i < NGTE_SITE_N; i++)
        fprintf(stderr, " %s=%lu/eff%lu", site[i], ngte_census.hit[i], ngte_census.eff[i]);
    fprintf(stderr, " sxy_bound=%lu bb_untagged=%lu reveal_admitted=%lu"
                    " reveal_spec_solo=%lu\n",
            ngte_census.sxy_bound, ngte_census.bb_untagged, ngte_census.reveal_admitted,
            ngte_census.reveal_spec_solo);
}
/** @brief Latches OPENPETE_FOV_CENSUS on the first call.
 *
 * When the variable is set and not '0', ngte_census_on becomes 1 and
 * ngte_census_print is registered to run at exit. */
static void ngte_census_arm(void) {
    static int armed = 0;
    if (armed) return;
    armed = 1;
    const char* e = getenv("OPENPETE_FOV_CENSUS");
    ngte_census_on = (e && *e && *e != '0') ? 1 : 0;
    if (ngte_census_on) atexit(ngte_census_print);
}

void ngte_unpack_matrix(uint32_t w0, uint32_t w1, uint32_t w2,
                        uint32_t w3, uint32_t w4, int M[3][3]) {
    M[0][0] = (int16_t)(w0 & 0xFFFF); M[0][1] = (int16_t)(w0 >> 16); M[0][2] = (int16_t)(w1 & 0xFFFF);
    M[1][0] = (int16_t)(w1 >> 16);    M[1][1] = (int16_t)(w2 & 0xFFFF); M[1][2] = (int16_t)(w2 >> 16);
    M[2][0] = (int16_t)(w3 & 0xFFFF); M[2][1] = (int16_t)(w3 >> 16);  M[2][2] = (int16_t)(w4 & 0xFFFF);
}

void ngte_read_matrix(CPUState* cpu, uint32_t p, int M[3][3]) {
    ngte_unpack_matrix(cpu->read_word(p), cpu->read_word(p + 4),
                       cpu->read_word(p + 8), cpu->read_word(p + 12),
                       cpu->read_word(p + 16), M);
}

void ngte_mac(const int M[3][3], double vx, double vy, double vz,
              double* m1, double* m2, double* m3) {
    *m1 = (M[0][0]*vx + M[0][1]*vy + M[0][2]*vz) / 4096.0;
    *m2 = (M[1][0]*vx + M[1][1]*vy + M[1][2]*vz) / 4096.0;
    *m3 = (M[2][0]*vx + M[2][1]*vy + M[2][2]*vz) / 4096.0;
}

void ngte_mac_f(const float M[3][3], double vx, double vy, double vz,
                double* m1, double* m2, double* m3) {
    *m1 = (double)M[0][0]*vx + (double)M[0][1]*vy + (double)M[0][2]*vz;
    *m2 = (double)M[1][0]*vx + (double)M[1][1]*vy + (double)M[1][2]*vz;
    *m3 = (double)M[2][0]*vx + (double)M[2][1]*vy + (double)M[2][2]*vz;
}

void ngte_mat_unit_f(const int M[3][3], float out[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) out[i][j] = (float)M[i][j] * (1.0f / 4096.0f);
}

void ngte_occl_camera(const ngte_view* v, struct openpete_occl_camera* c) {
    for (int i = 0; i < 3; i++) c->pos[i] = (float)v->cam[i];
    for (int r = 0; r < 3; r++) {
        double a = v->cam_M[r][0], b = v->cam_M[r][1], d = v->cam_M[r][2];
        double n = sqrt(a*a + b*b + d*d);
        if (n < 1e-6) n = 1.0;
        c->M[r*3 + 0] = (float)(a / n);
        c->M[r*3 + 1] = (float)(b / n);
        c->M[r*3 + 2] = (float)(d / n);
    }
    c->valid = 1;
}

/** @brief Whether a view with Hor+ factor F and --fov scale s is wider than the canonical 4:3 frame.
 *
 * Feeders project with H_canon · s, so in canonical screen coordinates the
 * view's half-width is 256 · F / s and its half-height band / s: the view is
 * wider in X when F / s > 1 and in Y when s < 1. At s == 1 the X test is
 * F > 1.0001, the tolerance absorbing float noise in F.
 * @param F The Hor+ factor.
 * @param s The --fov scale.
 * @return 1 when either half-extent exceeds the 4:3 frame's. */
static int ngte_extended_of(float F, double s) {
    return (double)F > s * 1.0001 || s < 1.0;
}

int ngte_view_extended(CPUState* cpu) {
    const double H_canon = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    return ngte_extended_of(openpete_native_widescreen_factor(),
                            openpete_native_fov_h_scale(H_canon, NGTE_Y_ROW));
}

void ngte_view_build(CPUState* cpu, ngte_view* v) {
    ngte_census_arm();
    const uint32_t cam_base = game_layout.cam.base;
    ngte_read_matrix(cpu, cam_base + 0x00, v->cam_M);
    v->cam_ram[0] = (double)(int32_t)cpu->read_word(cam_base + 0x28);
    v->cam_ram[1] = (double)(int32_t)cpu->read_word(cam_base + 0x2C);
    v->cam_ram[2] = (double)(int32_t)cpu->read_word(cam_base + 0x30);
    v->cam_interp = openpete_native_interp_pos(cam_base, v->cam) ? 1 : 0;
    if (!v->cam_interp) { v->cam[0] = v->cam_ram[0]; v->cam[1] = v->cam_ram[1]; v->cam[2] = v->cam_ram[2]; }
    v->ofx     = (double)(int32_t)cpu->gte_ctrl[24] / 65536.0;
    v->ofy     = (double)(int32_t)cpu->gte_ctrl[25] / 65536.0;
    v->H_canon = (double)(uint16_t)(cpu->gte_ctrl[26] & 0xFFFF);
    openpete_native_fov_note_canon(v->H_canon, NGTE_Y_ROW);
    v->s       = openpete_native_fov_h_scale(v->H_canon, NGTE_Y_ROW);
    v->H_eff   = v->H_canon * v->s;
    v->F       = openpete_native_widescreen_factor();
    openpete_native_widescreen_x_bounds(&v->xlo, &v->xhi);
    v->extended = ngte_extended_of(v->F, v->s);
}

void ngte_view_canon(const ngte_view* v, ngte_view* c) {
    *c = *v;
    c->s = 1.0;
    c->H_eff = c->H_canon;
}

double ngte_project_raw(const int M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx_px, double ofy_px, const ngte_view* v,
                        double* out_sx, double* out_sy, int* out_sat) {
    float Mf[3][3];
    ngte_mat_unit_f(M, Mf);
    return ngte_project_raw_f(Mf, TR, vx, vy, vz, ofx_px, ofy_px, v, out_sx, out_sy, out_sat);
}

double ngte_project_raw_f(const float M[3][3], const double TR[3],
                          double vx, double vy, double vz,
                          double ofx_px, double ofy_px, const ngte_view* v,
                          double* out_sx, double* out_sy, int* out_sat) {
    const double H = v->H_eff;
    double t0 = TR ? TR[0] : 0.0, t1 = TR ? TR[1] : 0.0, t2 = TR ? TR[2] : 0.0;
    double r1, r2, r3;
    ngte_mac_f(M, vx, vy, vz, &r1, &r2, &r3);
    double mac1 = t0 + r1, mac2 = t1 + r2, mac3 = t2 + r3;
    double z = (mac3 != 0.0) ? mac3 : 1.0;
    *out_sx = ofx_px + mac1 * H / z;
    *out_sy = ofy_px + mac2 * H / z;
    /* Limit mirrors read the GTE's H: the regime is the hardware's, not the
     * display's (H_eff moves with --fov, H_canon does not). */
    if (out_sat) {
        int ir = (mac1 > 32767.0 || mac1 < -32768.0 || mac2 > 32767.0 || mac2 < -32768.0 || mac3 <= 0.0);
        *out_sat = ir || v->H_canon >= 2.0 * mac3;
        NGTE_LIMIT(NGTE_SITE_RAW_SAT, *out_sat, ir || H >= 2.0 * mac3);
    }
    return mac3;
}

double ngte_gte_quot(const ngte_view* v, double m3) {
    const double sz3 = m3 < 0.0 ? 0.0 : (m3 > 65535.0 ? 65535.0 : m3);
    if (sz3 <= 0.0 || v->H_canon >= 2.0 * sz3) return NGTE_DIV_CAP * v->s;
    return v->H_eff / sz3;
}

void ngte_screen_gte(double m1, double m2, double m3,
                     double ofx, double ofy, const ngte_view* v,
                     int sxy_clamp, ngte_proj* o) {
    const double H = v->H_eff;
    /* IR1/IR2 = MAC saturated to int16 (the GTE projects from saturated IR). */
    double ir1 = m1 < -32768.0 ? -32768.0 : (m1 > 32767.0 ? 32767.0 : m1);
    double ir2 = m2 < -32768.0 ? -32768.0 : (m2 > 32767.0 ? 32767.0 : m2);
    /* SZ3 = MAC3 saturated to [0,0xFFFF]; the divide caps at 0x1FFFF when
     * SZ3 <= H/2 - the GTE's H (H_canon), so the regime does not move with
     * --fov. The capped quotient scales with the view (H_eff/sz3 =
     * s·H_canon/sz3), so the cap is NGTE_DIV_CAP·s: one arithmetic form
     * on both sides of the regime boundary. The SXY bound is applied to
     * the projected output: a vertex the hardware pinned at the edge stays
     * at the edge of the scaled view instead of inside it. */
    double sz3 = m3 < 0.0 ? 0.0 : (m3 > 65535.0 ? 65535.0 : m3);
    int sat = (sz3 <= 0.0 || v->H_canon >= 2.0 * sz3);
    double q = ngte_gte_quot(v, m3);
    NGTE_LIMIT(NGTE_SITE_GTE_SAT, sat, sz3 <= 0.0 || H >= 2.0 * sz3);
    double sx = ofx + ir1 * q;
    double sy = ofy + ir2 * q;
    if (sxy_clamp) {
        int hit = 0;
        if (sx < -NGTE_SXY_BOUND) { sx = -NGTE_SXY_BOUND; hit = 1; } else if (sx > NGTE_SXY_BOUND - 1.0) { sx = NGTE_SXY_BOUND - 1.0; hit = 1; }
        if (sy < -NGTE_SXY_BOUND) { sy = -NGTE_SXY_BOUND; hit = 1; } else if (sy > NGTE_SXY_BOUND - 1.0) { sy = NGTE_SXY_BOUND - 1.0; hit = 1; }
        if (hit) NGTE_CENSUS_HIT(sxy_bound);
    }
    o->sx = sx; o->sy = sy;
    o->m1 = m1; o->m2 = m2; o->m3 = m3;
    o->sat = sat;
}

void ngte_project_gte(const int M[3][3], const double TR[3],
                      double vx, double vy, double vz,
                      double ofx, double ofy, const ngte_view* v,
                      int sxy_clamp, ngte_proj* o) {
    float Mf[3][3];
    ngte_mat_unit_f(M, Mf);
    ngte_project_gte_f(Mf, TR, vx, vy, vz, ofx, ofy, v, sxy_clamp, o);
}

void ngte_project_gte_f(const float M[3][3], const double TR[3],
                        double vx, double vy, double vz,
                        double ofx, double ofy, const ngte_view* v,
                        int sxy_clamp, ngte_proj* o) {
    double t0 = TR ? TR[0] : 0.0, t1 = TR ? TR[1] : 0.0, t2 = TR ? TR[2] : 0.0;
    double r1, r2, r3;
    ngte_mac_f(M, vx, vy, vz, &r1, &r2, &r3);
    ngte_screen_gte(t0 + r1, t1 + r2, t2 + r3, ofx, ofy, v, sxy_clamp, o);
}

static int s_unr_tab[257];
/** @brief The GTE's UNR division H / SZ3 as RTPS computes it, capped at 0x1FFFF.
 *
 * Builds the 257-entry UNR reciprocal table on the first call. When H >= 2 *
 * SZ3 (including SZ3 == 0) the divide overflows and returns 0x1FFFF, as the
 * hardware does. Otherwise SZ3 is normalised to bit 15, the table seeds the
 * reciprocal, two Newton steps refine it, and the product with the shifted H is
 * rounded to the 16-bit fraction.
 * @param H   the GTE's projection distance H
 * @param sz3 SZ3, 0..0xFFFF
 * @return H / SZ3 in 16.16 fixed point, at most 0x1FFFF */
static int ngte_unr_divide(int H, int sz3)
{
    if (!s_unr_tab[0]) for (int i = 0; i < 257; i++) {
        int u = (0x40000 / (i + 0x100) + 1) / 2 - 0x101;
        s_unr_tab[i] = u > 0 ? u : 0;
    }
    if (H >= sz3 * 2) return 0x1FFFF;
    int z = 0;
    while (z < 16 && !((sz3 << z) & 0x8000)) z++;
    int64_t n = (int64_t)H << z;
    int64_t d = (int64_t)sz3 << z;
    int64_t u = s_unr_tab[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    int64_t q = (n * d + 0x8000) >> 16;
    return q > 0x1FFFF ? 0x1FFFF : (int)q;
}

/** @brief `v` clamped to [lo, hi].
 *
 * @param v  the value
 * @param lo lower bound
 * @param hi upper bound, >= lo
 * @return v saturated to the range */
static int64_t ngte_sat64(int64_t v, int64_t lo, int64_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

void ngte_rtps_int(const int M[3][3], const int tr[3], int vx, int vy, int vz,
                   double ofx, double ofy, int H, ngte_rtps_out* out)
{
    const int64_t V[3] = { vx, vy, vz };
    int64_t mac[3];
    for (int k = 0; k < 3; k++)
        mac[k] = (((int64_t)tr[k] << 12) + M[k][0] * V[0] + M[k][1] * V[1] + M[k][2] * V[2]) >> 12;
    for (int k = 0; k < 3; k++) out->mac[k] = (int32_t)mac[k];
    out->sz3 = (int32_t)ngte_sat64(mac[2], 0, 0xFFFF);
    const int64_t q = ngte_unr_divide(H, out->sz3);
    out->q = (int32_t)q;
    const int64_t x = (ngte_sat64(mac[0], -0x8000, 0x7FFF) * q + llround(ofx * 65536.0)) >> 16;
    const int64_t y = (ngte_sat64(mac[1], -0x8000, 0x7FFF) * q + llround(ofy * 65536.0)) >> 16;
    out->sx = (int16_t)ngte_sat64(x, -0x400, 0x3FF);
    out->sy = (int16_t)ngte_sat64(y, -0x400, 0x3FF);
}

int ngte_dpcs_ch(int c8, int fc12, int ir0) {
    long mac = (long)c8 << 16;
    long ird = (((long)fc12 << 12) - mac) >> 12;
    if (ird >  32767) ird =  32767;
    if (ird < -32768) ird = -32768;
    long out = ((mac + (long)ir0 * ird) >> 12) >> 4;
    if (out < 0)   out = 0;
    if (out > 255) out = 255;
    return (int)out;
}

uint32_t ngte_depth_cue(uint32_t rgb, uint32_t fc, int ir0) {
    uint32_t out = 0;
    for (int k = 0; k < 3; k++) {
        int c = (int)((rgb >> (k * 8)) & 0xFF), f = (int)((fc >> (k * 8)) & 0xFF);
        int v = (int)(c + (f - c) * (double)ir0 / 4096.0 + 0.5);   /* float, like the rest of the path */
        if (v < 0) v = 0; else if (v > 255) v = 255;
        out |= (uint32_t)v << (k * 8);
    }
    return out;
}

uint32_t ngte_fog_far_colour(CPUState* cpu) {
    uint32_t a = game_layout.fog.far_colour;
    if (!a) return 0;
    if (!game_layout.fog.far_colour_words) return cpu->read_word(a) & 0xFFFFFFu;
    uint32_t r = (cpu->read_word(a) >> 4) & 0xFF, g = (cpu->read_word(a + 4) >> 4) & 0xFF,
             b = (cpu->read_word(a + 8) >> 4) & 0xFF;
    return r | (g << 8) | (b << 16);
}

void ngte_mat_mul3(const int A[3][3], const int B[3][3], int O[3][3]) {
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        O[i][j] = (int)(((int64_t)A[i][0]*B[0][j] + (int64_t)A[i][1]*B[1][j] +
                         (int64_t)A[i][2]*B[2][j]) >> 12);
}

void ngte_sincos_f(CPUState* cpu, double fb, double* s_out, double* c_out) {
    const uint32_t tbl = game_layout.sin_table;
    if (!tbl) { *s_out = 0.0; *c_out = 4096.0; return; }
    while (fb < 0.0) fb += 256.0;
    while (fb >= 256.0) fb -= 256.0;
    int b0 = (int)fb; double frac = fb - (double)b0;
    uint32_t i0 = (uint32_t)b0 << 1, i1 = (uint32_t)((b0 + 1) & 0xFF) << 1;
    double s0 = (int16_t)cpu->read_half(tbl + i0),        s1 = (int16_t)cpu->read_half(tbl + i1);
    double c0 = (int16_t)cpu->read_half(tbl + 0x80u + i0), c1 = (int16_t)cpu->read_half(tbl + 0x80u + i1);
    *s_out = s0 + (s1 - s0) * frac;
    *c_out = c0 + (c1 - c0) * frac;
}

void ngte_project_offset(const ngte_view* v, const double off[3], ngte_proj* o) {
    ngte_project_gte(v->cam_M, NULL, off[1], off[2], off[0], v->ofx, v->ofy, v, 0, o);
}

int ngte_table_rot(CPUState* cpu, double from_ang, double to_ang,
                   double* rc, double* rs) {
    *rc = 1.0; *rs = 0.0;
    if (to_ang == from_ang) return 0;
    double sf, cf, st, ct;
    ngte_sincos_f(cpu, from_ang, &sf, &cf);
    ngte_sincos_f(cpu, to_ang, &st, &ct);
    const double n = sf * sf + cf * cf;
    if (n < 1.0) return 0;
    *rc = (sf * st + cf * ct) / n;
    *rs = (cf * st - sf * ct) / n;
    return 1;
}

/** @brief Rounds a double to the nearest integer, halves away from zero.
 *
 * @param x The value.
 * @return The rounded value. */
static int ngte_rnd_i(double x) { return (int)(x >= 0.0 ? x + 0.5 : x - 0.5); }

void ngte_sincos_interp(CPUState* cpu, double fb, int* s_out, int* c_out) {
    double s, c;
    ngte_sincos_f(cpu, fb, &s, &c);
    *s_out = ngte_rnd_i(s);
    *c_out = ngte_rnd_i(c);
}

void ngte_euler_sincos(CPUState* cpu, uint32_t rot, ngte_euler_sc* e) {
    const uint32_t tbl = game_layout.sin_table;
    if (!tbl) {
        e->sy = e->sx = e->sz = 0;
        e->cy = e->cx = e->cz = 4096;
        return;
    }
    uint32_t iy = (rot >> 15) & 0x1FEu, ix = (rot & 0xFF00u) >> 7, iz = (rot & 0xFFu) << 1;
    e->sy = (int16_t)cpu->read_half(tbl + iy); e->cy = (int16_t)cpu->read_half(tbl + 0x80u + iy);
    e->sx = (int16_t)cpu->read_half(tbl + ix); e->cx = (int16_t)cpu->read_half(tbl + 0x80u + ix);
    e->sz = (int16_t)cpu->read_half(tbl + iz); e->cz = (int16_t)cpu->read_half(tbl + 0x80u + iz);
}

void ngte_compose_euler(const int start[3][3], const ngte_euler_sc* e, int out[3][3]) {
    int Ry[3][3] = {{e->cy, 0, -e->sy}, {0, 4096, 0}, {e->sy, 0, e->cy}};
    int Rx[3][3] = {{4096, 0, 0}, {0, e->cx, -e->sx}, {0, e->sx, e->cx}};
    int Rz[3][3] = {{e->cz, -e->sz, 0}, {e->sz, e->cz, 0}, {0, 0, 4096}};
    int t1[3][3], t2[3][3];
    ngte_mat_mul3(start, Ry, t1);
    ngte_mat_mul3(t1, Rx, t2);
    ngte_mat_mul3(t2, Rz, out);
}

void ngte_compose_euler_f(CPUState* cpu, const int start[3][3], const double ang[3],
                          double out[3][3]) {
    double sz, cz, sx, cx, sy, cy;
    ngte_sincos_f(cpu, ang[0], &sz, &cz);
    ngte_sincos_f(cpu, ang[1], &sx, &cx);
    ngte_sincos_f(cpu, ang[2], &sy, &cy);
    const double Ry[3][3] = {{cy, 0.0, -sy}, {0.0, 4096.0, 0.0}, {sy, 0.0, cy}};
    const double Rx[3][3] = {{4096.0, 0.0, 0.0}, {0.0, cx, -sx}, {0.0, sx, cx}};
    const double Rz[3][3] = {{cz, -sz, 0.0}, {sz, cz, 0.0}, {0.0, 0.0, 4096.0}};
    double t1[3][3], t2[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        t1[i][j] = (start[i][0]*Ry[0][j] + start[i][1]*Ry[1][j] + start[i][2]*Ry[2][j]) / 4096.0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        t2[i][j] = (t1[i][0]*Rx[0][j] + t1[i][1]*Rx[1][j] + t1[i][2]*Rx[2][j]) / 4096.0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++)
        out[i][j] = (t2[i][0]*Rz[0][j] + t2[i][1]*Rz[1][j] + t2[i][2]*Rz[2][j]) / 4096.0;
}

void ngte_word_angles(uint32_t rot, double ang[3]) {
    ang[0] = (double)(rot & 0xFFu);
    ang[1] = (double)((rot >> 8) & 0xFFu);
    ang[2] = (double)((rot >> 16) & 0xFFu);
}

void ngte_compose_euler_mf(CPUState* cpu, const float start[3][3], const double ang[3],
                           float out[3][3]) {
    double s[3], c[3];
    for (int k = 0; k < 3; k++) ngte_sincos_f(cpu, ang[k], &s[k], &c[k]);
    const float sz = (float)(s[0] / 4096.0), cz = (float)(c[0] / 4096.0);
    const float sx = (float)(s[1] / 4096.0), cx = (float)(c[1] / 4096.0);
    const float sy = (float)(s[2] / 4096.0), cy = (float)(c[2] / 4096.0);
    const float Ry[3][3] = {{cy, 0.0f, -sy}, {0.0f, 1.0f, 0.0f}, {sy, 0.0f, cy}};
    const float Rx[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, cx, -sx}, {0.0f, sx, cx}};
    const float Rz[3][3] = {{cz, -sz, 0.0f}, {sz, cz, 0.0f}, {0.0f, 0.0f, 1.0f}};
    float t1[3][3], t2[3][3];
    ngte_mat_mul3_f(start, Ry, t1);
    ngte_mat_mul3_f((const float (*)[3])t1, Rx, t2);
    ngte_mat_mul3_f((const float (*)[3])t2, Rz, out);
}

void ngte_compose_euler_at(CPUState* cpu, const int start[3][3], const double ang[3], int out[3][3],
                           float outf[3][3]) {
    ngte_euler_sc e;
    ngte_sincos_interp(cpu, ang[2], &e.sy, &e.cy);
    ngte_sincos_interp(cpu, ang[1], &e.sx, &e.cx);
    ngte_sincos_interp(cpu, ang[0], &e.sz, &e.cz);
    ngte_compose_euler(start, &e, out);
    if (!outf) return;
    float S[3][3];
    ngte_mat_unit_f(start, S);
    ngte_compose_euler_mf(cpu, (const float (*)[3])S, ang, outf);
}

void ngte_mat_mul3_f(const float A[3][3], const float B[3][3], float O[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            O[i][j] = A[i][0] * B[0][j] + A[i][1] * B[1][j] + A[i][2] * B[2][j];
}

/* ---- texture page / CLUT bitfields -------------------------------------- */

void ngte_decode_tpage(unsigned tp, unsigned clut, unsigned flags, ngte_tpage* out) {
    out->mode = (int)((tp >> 7) & 3u);
    out->abr  = (int)((tp >> 5) & 3u);
    if ((flags & NGTE_TP_REPLACED_ID) && out->mode == 3) {
        out->bpp = 32;
        out->replaced_id = (int)(int16_t)(uint16_t)clut;
        out->tpx = out->replaced_id;
        out->tpy = 0;
        out->clx = 0;
        out->cly = 0;
        return;
    }
    out->replaced_id = -1;
    out->tpx = (int)(tp & 0xFu) * 64;
    out->tpy = (int)((tp >> 4) & 1u) * 256;
    out->bpp = (out->mode == 0) ? 4 : (out->mode == 1) ? 8 : 16;
    out->clx = (int)(clut & 0x3Fu) * 16;
    out->cly = (int)((clut >> 6) & 0x1FFu);
}

int ngte_abr_blend(int abr, ngte_abr_policy policy) {
    switch (abr & 3) {
        case 0:  return 1;                                        /* (B+F)/2 */
        case 1:  return 2;                                        /* B+F     */
        case 2:  return 3;                                        /* B-F     */
        default: return (policy == NGTE_ABR_QUARTER_AS_ADD) ? 2 : 4;  /* B+F/4 */
    }
}

/* ---- flat-depth screen primitive feed ----------------------------------- */

int ngte_prim_tpage_blend(const ngte_prim* p, ngte_tpage* tp) {
    if (!p->textured) {
        memset(tp, 0, sizeof *tp);
        return p->stp ? 1 : 0;
    }
    ngte_decode_tpage((unsigned)p->tpage, (unsigned)p->clut, 0u, tp);
    return p->stp ? ngte_abr_blend(tp->abr, NGTE_ABR_QUARTER_AS_ADD) : 0;
}

int ngte_feed_flat_quad(const float x[4], const float y[4], const float u[4],
                        const float v[4], const uint32_t rgb[4], float depth,
                        const ngte_tpage* tp, int blend, int nv) OP_REQUIRES_SHARED(sim) {
    return ngte_feed_flat_quad_q(x, y, u, v, rgb, NULL, depth, tp, blend, nv, -1);
}

int ngte_feed_flat_quad_q(const float x[4], const float y[4], const float u[4],
                          const float v[4], const uint32_t rgb[4], const uint32_t rgbq[4],
                          float depth, const ngte_tpage* tp, int blend, int nv, int bucket) OP_REQUIRES_SHARED(sim) {
    static const int split[2][3] = { {0, 1, 2}, {1, 3, 2} };
    const int ntri = (nv == 4) ? 2 : 1;
    int fed = 0;
    for (int s = 0; s < ntri; s++) {
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            const int j = split[s][k];
            st.v[k].sx = x[j]; st.v[k].sy = y[j]; st.v[k].z = depth;
            st.v[k].pvz = depth;   /* fed at one depth: W constant across the prim */
            st.v[k].u = u[j]; st.v[k].v = v[j];
            st.v[k].rgba = (rgb[j] & 0xFFFFFFu) | 0xFF000000u;
            st.v[k].rgbq = rgbq ? rgbq[j] : 0u;
        }
        st.textured = tp ? 1 : 0;
        if (tp) {
            st.tpage_x = tp->tpx; st.tpage_y = tp->tpy;
            st.clut_x  = tp->clx; st.clut_y  = tp->cly; st.bpp = tp->bpp;
        }
        st.blend = blend;
        openpete_scene_tri_set_bucket(&st, bucket, blend == 2);
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_WORLD, 0);
        fed++;
    }
    return fed;
}

void ngte_line_quad(float ax, float ay, float bx, float by, float centre,
                    float qx[4], float qy[4]) {
    const float x0 = ax + centre, y0 = ay + centre;
    const float x1 = bx + centre, y1 = by + centre;
    const float dx = x1 - x0, dy = y1 - y0;
    const int xmajor = fabsf(dx) >= fabsf(dy);
    const float major = fmaxf(fabsf(xmajor ? dx : dy), 1.0f);
    const float ex = 0.5f * (xmajor ? (dx < 0.0f ? -1.0f : 1.0f) : dx / major);   /* half-pixel end extension */
    const float ey = 0.5f * (xmajor ? dy / major : (dy < 0.0f ? -1.0f : 1.0f));
    const float sx = xmajor ? 0.0f : 0.5f, sy = xmajor ? 0.5f : 0.0f;         /* half-pixel side offset */
    qx[0] = x0 - ex - sx; qx[1] = x1 + ex - sx; qx[2] = x0 - ex + sx; qx[3] = x1 + ex + sx;
    qy[0] = y0 - ey - sy; qy[1] = y1 + ey - sy; qy[2] = y0 - ey + sy; qy[3] = y1 + ey + sy;
}

int ngte_feed_flat_line(float ax, float ay, float bx, float by, uint32_t rgb_a,
                        uint32_t rgb_b, float centre, float depth, int blend) OP_REQUIRES_SHARED(sim) {
    float qx[4], qy[4];
    ngte_line_quad(ax, ay, bx, by, centre, qx, qy);
    const float zu[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const uint32_t cc[4] = { rgb_a, rgb_b, rgb_a, rgb_b };
    return ngte_feed_flat_quad(qx, qy, zu, zu, cc, depth, NULL, blend, 4);
}

/* ---- screen-space backface test ----------------------------------------- */

double ngte_nclip(double ax, double ay, double bx, double by,
                  double cx, double cy, ngte_nclip_mode mode) {
    if (mode == NGTE_NCLIP_SXY_GRID) {
        ax = lrint(ax); ay = lrint(ay);
        bx = lrint(bx); by = lrint(by);
        cx = lrint(cx); cy = lrint(cy);
    }
    return ax * (by - cy) + bx * (cy - ay) + cx * (ay - by);
}

void ngte_dump_words(CPUState* cpu, const char* label, uint32_t base, int nwords) {
    fprintf(stderr, "  %s @%08X:\n   ", label, base);
    for (int i = 0; i < nwords; i++) {
        fprintf(stderr, "%08X ", cpu->read_word(base + (uint32_t)i * 4));
        if ((i & 7) == 7 && i != nwords - 1) fprintf(stderr, "\n   ");
    }
    fprintf(stderr, "\n");
}

/* ---- ordering-table polybuf walk ---------------------------------------- */

/** @brief Payload word count of a GPU polygon packet, tag word excluded.
 *
 * The count is the colour word that carries the code, one screen-XY word per
 * vertex, one extra colour word per vertex past the first when gouraud, and
 * one UV word per vertex when textured.
 * @param code The GPU command code byte.
 * @return The word count, or 0 for a code that is not a polygon. */
static uint32_t ngte_prim_payload_words(uint8_t code) {
    if ((code & 0xE0) != 0x20) return 0;
    uint32_t nv = (code & 0x08) ? 4u : 3u;
    uint32_t w = 1u + nv;
    if (code & 0x10) w += nv - 1u;
    if (code & 0x04) w += nv;
    return w;
}

uint32_t ngte_rgb_mix(uint32_t a, uint32_t aq, uint32_t b, uint32_t bq, double t, uint32_t* q) {
    uint32_t r = 0, rq = 0;
    for (int s = 0; s < 24; s += 8) {
        const double ca = (double)((a >> s) & 0xFFu) + (double)(int8_t)((aq >> s) & 0xFFu) / 256.0;
        const double cb = (double)((b >> s) & 0xFFu) + (double)(int8_t)((bq >> s) & 0xFFu) / 256.0;
        const double v = ca + (cb - ca) * t;
        double c = floor(v + 0.5);
        c = c < 0.0 ? 0.0 : (c > 255.0 ? 255.0 : c);
        double d = floor((v - c) * 256.0 + 0.5);
        d = d < -128.0 ? -128.0 : (d > 127.0 ? 127.0 : d);
        r |= (uint32_t)c << s;
        rq |= ((uint32_t)(int32_t)d & 0xFFu) << s;
    }
    if (q) *q = rq;
    return r;
}

ngte_cvert ngte_clip_lerp(ngte_cvert a, ngte_cvert b, double zc) {
    double t = (zc - a.m3) / (b.m3 - a.m3);
    if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
    ngte_cvert r;
    r.osx = r.osy = 0.0f; r.oflag = 0;
    r.m1 = a.m1 + t * (b.m1 - a.m1);
    r.m2 = a.m2 + t * (b.m2 - a.m2);
    r.m3 = zc;
    r.u  = a.u + (float)t * (b.u - a.u);
    r.v  = a.v + (float)t * (b.v - a.v);
    r.fx = a.fx + (float)t * (b.fx - a.fx);
    r.fy = a.fy + (float)t * (b.fy - a.fy);
    r.rgb = ngte_rgb_mix(a.rgb, a.rgbq, b.rgb, b.rgbq, t, &r.rgbq);
    r.rgbf = 0;
    return r;
}

int ngte_near_clip_tri(const double z[3], double zc, ngte_clip_ref out[4], int* clipped) {
    int nout = 0;
    *clipped = 0;
    for (int i = 0; i < 3; i++) {
        const int nx = (i + 1) % 3;
        const int curIn = z[i] >= zc, nxtIn = z[nx] >= zc;
        if (curIn) out[nout++] = (ngte_clip_ref){ (uint8_t)i, (uint8_t)i, 0, 0.0 };
        if (curIn != nxtIn && nout < 4) {
            double t = (zc - z[i]) / (z[nx] - z[i]);
            if (t < 0.0) t = 0.0; else if (t > 1.0) t = 1.0;
            out[nout++] = (ngte_clip_ref){ (uint8_t)i, (uint8_t)nx, 1, t };
            *clipped = 1;
        }
    }
    return nout < 3 ? 0 : nout;
}

void ngte_prim_decode(const uint32_t* w, int words, int raw_max, ngte_prim* t) {
    if (words > NGTE_PRIM_RAW_MAX) words = NGTE_PRIM_RAW_MAX;
    memset(t, 0, sizeof(*t));
    t->code = (uint8_t)(w[1] >> 24);
    t->taglen = (uint8_t)(w[0] >> 24);
    for (int i = 0; i < words; i++) t->raw[i] = w[i];
    t->nraw = (raw_max < words) ? raw_max : words;
    if ((t->code & 0xE0) != 0x20) return;
    t->gouraud  = (t->code & 0x10) != 0;
    t->quad     = (t->code & 0x08) != 0;
    t->textured = (t->code & 0x04) != 0;
    t->stp      = (t->code & 0x02) != 0;
    t->nv = t->quad ? 4 : 3;
    uint32_t rgb0 = t->raw[1] & 0xFFFFFF;
    int c = 2;
    for (int k = 0; k < t->nv; k++) {
        t->rgb[k] = (t->gouraud && k > 0) ? (t->raw[c++] & 0xFFFFFF) : rgb0;
        uint32_t xy = t->raw[c++];
        t->x[k] = (float)(int16_t)(xy & 0xFFFF);
        t->y[k] = (float)(int16_t)(xy >> 16);
        if (!t->textured) continue;
        uint32_t uv = t->raw[c++];
        t->u[k] = (int)(uv & 0xFF);
        t->v[k] = (int)((uv >> 8) & 0xFF);
        if (k == 0)      t->clut  = (int)(uv >> 16);
        else if (k == 1) t->tpage = (int)(uv >> 16);
    }
}

void ngte_walk_prims(CPUState* cpu, uint32_t start, uint32_t end,
                     const ngte_walk_policy* pol, ngte_prim_sink sink, void* ctx) {
    ngte_prim t;
    uint32_t w[NGTE_PRIM_RAW_MAX];
    uint32_t p = start;
    int guard = 0;
    while (p < end && guard++ < pol->guard) {
        uint32_t w0 = cpu->read_word(p);
        uint32_t taglen = (w0 >> 24) & 0xFF;
        if (taglen > pol->tag_len_max) break;              /* corrupt */
        uint8_t code = (uint8_t)(cpu->read_word(p + 4) >> 24);
        int polygon = (code & 0xE0) == 0x20;
        uint32_t stride = (taglen + 1) * 4;
        if (taglen == 0) {
            /* A packet whose tag length byte is zero. The ordering table's
             * hardware traversal follows the tag's link and never draws it,
             * but the producer still wrote the packet into the buffer, and
             * the packets after it are reached only by stepping over it. Its
             * length comes from the primitive code instead of the tag, which
             * puts the walk back in step; a code with no derivable length is
             * not a packet and ends the walk. */
            uint32_t pw = polygon ? ngte_prim_payload_words(code) : 0;
            if (!pw) break;
            stride = (pw + 1) * 4;
        }
        if (pol->count_all) {
            if (pol->code_hist) pol->code_hist[code]++;
            if (pol->tag_hist)  pol->tag_hist[taglen]++;
            if (pol->total)     (*pol->total)++;
        }
        if (!pol->polygons_only || polygon) {
            int words = (int)(stride / 4);
            if (words > NGTE_PRIM_RAW_MAX) words = NGTE_PRIM_RAW_MAX;
            for (int i = 0; i < words; i++) w[i] = cpu->read_word(p + (uint32_t)i * 4);
            ngte_prim_decode(w, words, pol->raw_words_max, &t);
            if (sink(ctx, &t) && !pol->count_all) {
                if (pol->code_hist) pol->code_hist[code]++;
                if (pol->tag_hist)  pol->tag_hist[taglen]++;
                if (pol->total)     (*pol->total)++;
            }
        }
        p += stride;
    }
}
