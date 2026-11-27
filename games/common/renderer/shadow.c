// games/common/renderer/shadow.c - the MOBY DROP-SHADOW renderer re-derived
// from gamestate in float, plus its A/B harness. Every engine address and
// every decode switch comes from game_layout.shadow; the game's own shadow
// draw runs through the recomp module dispatch at game_layout.shadow.hook.
//
// The draw is a textured FT3 fan (GPU code 0x26) per drawn moby: a ground
// anchor projected from the moby's position and its packed ground word, four
// diamond corners tilted to the ground normal by two 6-bit angles out of the
// same word, four triangles fanned from the anchor, and a grey that fades
// with camera distance. The semi-transparency mode is the ABR field of the
// shadow sprite's tpage word, which is level data, so the blend is read per
// level rather than assumed.
//
// Structure of the game's shadow routine:
//   * the descriptor's list terminator is at +0x08, after the two UV words.
//   * the shadow is 4 separate FT3 tris fanned from the ground anchor (not
//     one quad); word 1 of the 8-byte drawn-moby list record is the shadow
//     radius/scale, not an index. The moby's position and ground word are
//     read DIRECTLY from the Moby, not from any render record.
//   * the corner order is V0 = (0, 0, +r), V1 = (r.lo, r.hi, 0),
//     V2 = (0, 0, -r), V3 = (-r, 0, 0), the first three by one RTPT and the
//     fourth by a further RTPS.
//
// Determinism: never writes PSX RAM. Calls the real renderer exactly once
// (behaviour-identical to normal dispatch) then only READS state afterwards.
// Gated by OPENPETE_SHADOW_VERIFY (dump/A/B) or a native renderer mode
// (--renderer vk; scene feed); inert fall-through otherwise.

#include "feature_registry.h"
#include "renderer/shadow.h"
#include "renderer/native_gte.h"    /* shared GTE float-mirror math */
#include "psx_runtime_internal.h"   /* full CPUState (gte_data, read_word, ..) */
#include "openpete_native_render.h"
#include "openpete_recomp_module.h" /* the game's own shadow draw, through the module dispatch */
#include "renderer/render_paths.h"  /* --render-paths isolation */
#include "renderer/hook_bracket.h"  /* the shared feeder hook bracket */
#include "renderer/orchestrator.h"  /* openpete_native_interp_pos */
#include "renderer/moby.h"        /* the record own-motion offsets */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_layout.h"
#include "openpete_profile.h"       /* OP_PROF_ACCUM_SCOPE - fiber-hook cost split */
#include "renderer/env_knob.h"
#include "renderer/cut_guard.h"         /* cut_held_set_pick, cut_census_held_set */
#include "renderer/player_hide.h"       /* --hide-player: the companion's shadow */
#include "openpete_thread_roles.h"

extern unsigned openpete_frame;

/* shadow_s16 and shadow_rd_s16 are documented in shadow.h. */
int shadow_s16(int v) { return (int)(int16_t)(uint32_t)v; }
int shadow_rd_s16(CPUState* cpu, uint32_t addr) {
    uint32_t w = cpu->read_word(addr & ~3u);
    return (int)(int16_t)((addr & 2u) ? (w >> 16) : (w & 0xFFFF));
}

/* ---- captured prims (ground truth) --------------------------------------- */
#define MAX_PRIMS 4096
static shadow_prim s_gte[MAX_PRIMS]; static int s_gte_n;
static uint32_t s_code_hist[256];

/* shadow_prim_count and shadow_prim_at are documented in shadow.h. */
int                shadow_prim_count(void) { return s_gte_n; }
const shadow_prim* shadow_prim_at(int i)   { return (i >= 0 && i < s_gte_n) ? &s_gte[i] : NULL; }

/** @brief Stores one walked primitive into the capture buffer.
 *
 * @param ctx Unused.
 * @param s The decoded primitive.
 * @return 1 when stored; 0 when the buffer's 4096 entries are full. */
static int shadow_prim_sink(void* ctx, const ngte_prim* s) {
    (void)ctx;
    if (s_gte_n >= MAX_PRIMS) return 0;
    shadow_prim* t = &s_gte[s_gte_n];
    memset(t, 0, sizeof(*t));
    t->code = s->code; t->len = s->taglen; t->nv = s->nv;
    for (int k = 0; k < 4; k++) {
        t->x[k] = s->x[k]; t->y[k] = s->y[k]; t->rgb[k] = s->rgb[k];
        t->u[k] = s->u[k]; t->v[k] = s->v[k];
    }
    t->clut = s->clut; t->tpage = s->tpage;
    t->textured = s->textured; t->gouraud = s->gouraud; t->quad = s->quad;
    memcpy(t->raw, s->raw, sizeof(t->raw));
    t->nraw = s->nraw;
    s_gte_n++;
    return 1;
}

/* Walks the polybuf into s_gte; see shadow.h. */
void shadow_capture_prims(CPUState* cpu, uint32_t start, uint32_t end) {
    OP_PROF_ACCUM_SCOPE("shadow.capture");
    s_gte_n = 0;
    memset(s_code_hist, 0, sizeof(s_code_hist));
    const ngte_walk_policy pol = {
        .tag_len_max = 24, .raw_words_max = 20, .guard = MAX_PRIMS,
        .polygons_only = 0, .code_hist = s_code_hist, .tag_hist = NULL,
        .total = NULL, .count_all = 0,
    };
    ngte_walk_prims(cpu, start, end, &pol, shadow_prim_sink, NULL);
}

void shadow_dump_prims(const char* tag) {
    fprintf(stderr, "  [%s] prim code histogram:", tag);
    for (int c = 0; c < 256; c++) if (s_code_hist[c]) fprintf(stderr, " %02X:%u", c, s_code_hist[c]);
    fprintf(stderr, "  (total=%d)\n", s_gte_n);
    int n = s_gte_n < 6 ? s_gte_n : 6;
    for (int p = 0; p < n; p++) {
        shadow_prim* t = &s_gte[p];
        fprintf(stderr, "    prim[%d] code=%02X len=%d nv=%d tex=%d gour=%d  raw:",
                p, t->code, t->len, t->nv, t->textured, t->gouraud);
        for (int i = 0; i < t->nraw; i++) fprintf(stderr, " %08X", t->raw[i]);
        fprintf(stderr, "\n      decoded:");
        for (int k = 0; k < t->nv; k++)
            fprintf(stderr, " v%d(xy=%.0f,%.0f rgb=%06X uv=%d,%d)",
                    k, t->x[k], t->y[k], t->rgb[k], t->u[k], t->v[k]);
        if (t->textured) fprintf(stderr, "  clut=%04X tpage=%04X", t->clut, t->tpage);
        fprintf(stderr, "\n");
    }
}

/* ---- the drawn-moby list base -------------------------------------------- */
/* SHADOW_LIST_GTE_DATA carries the base in a GTE data register the moby draw
 * left behind; the shadow draw overwrites the GTE data registers as it runs,
 * so the base is latched at hook entry and reused by the sub-tick re-extract. */
static uint32_t s_list_base;

/* The list records, copied at hook entry: a game that keeps the list in the
 * primitive buffer's tail overwrites it later in the tick, so that source
 * walks this copy; a fixed-address list is re-read live at each walk. */
#define SHADOW_LIST_MAX 1024
typedef struct { uint32_t moby; uint32_t size; } shadow_list_rec;
static shadow_list_rec s_list_buf[2][SHADOW_LIST_MAX];
static shadow_list_rec* s_list = s_list_buf[0];
static int s_list_n;
/* The game's own records at the head of s_list; the widened-view records
 * follow them. */
static int s_list_game_n;
/* The canonical epoch the copy was taken in, and the previous epoch's copy,
 * which a held camera cut walks (shadow_replay_prev_list). */
static uint32_t s_list_epoch;
static struct { shadow_list_rec* list; uint32_t base, epoch; int n; } s_list_prev = { .list = s_list_buf[1] };

/** @brief Exchange the current list copy, its base and epoch for the previous epoch's. */
static void shadow_list_swap(void) {
    shadow_list_rec* l = s_list; s_list = s_list_prev.list; s_list_prev.list = l;
    uint32_t b = s_list_base; s_list_base = s_list_prev.base; s_list_prev.base = b;
    uint32_t e = s_list_epoch; s_list_epoch = s_list_prev.epoch; s_list_prev.epoch = e;
    int n = s_list_n; s_list_n = s_list_prev.n; s_list_prev.n = n;
}

/** @brief Copies the list records from s_list_base up to the end pointer into s_list.
 *
 * Copies up to SHADOW_LIST_MAX 8-byte records (moby, size word) and sets
 * s_list_n to the count.
 * @param cpu The guest CPU state, read only.
 * @param list_end The list's end pointer, descriptor + 0x08. */
static void shadow_list_read(CPUState* cpu, uint32_t list_end) {
    s_list_n = 0;
    for (uint32_t e = s_list_base; e + 8 <= list_end && s_list_n < SHADOW_LIST_MAX; e += 8) {
        s_list[s_list_n].moby = cpu->read_word(e + 0);
        s_list[s_list_n].size = cpu->read_word(e + 4);
        s_list_n++;
    }
}

/* The list a held present walks (cut_held_set_pick), or -1 outside a hold. */
static int s_list_held = -1;

/** @brief Latches the drawn-moby list base and copies the list records.
 *
 * The base is the GTE data register game_layout.shadow.list_reg plus the list
 * offset, or the layout's fixed address. A register index outside 0..31 empties
 * the list. Copies up to SHADOW_LIST_MAX 8-byte records from the base up to the
 * end pointer at descriptor + 0x08; without a descriptor or base the copy is
 * empty.
 * @param cpu The guest CPU state. */
static void shadow_latch_list_base(CPUState* cpu) OP_REQUIRES(sim) {
    /* The first copy of a new canonical epoch retires the current one. */
    const uint32_t epoch = spyro_native_canon_epoch_stamp();
    if (s_list_epoch != epoch) { shadow_list_swap(); s_list_epoch = epoch; }
    if (game_layout.shadow.list_source == SHADOW_LIST_GTE_DATA) {
        int r = game_layout.shadow.list_reg;
        if (r < 0 || r >= 32) { s_list_base = 0; s_list_n = 0; s_list_game_n = 0; return; }
        s_list_base = cpu->gte_data[r] + game_layout.shadow.list_offset;
    } else {
        s_list_base = game_layout.shadow.list_base;
    }
    s_list_n = 0;
    s_list_game_n = 0;
    const uint32_t desc = game_layout.shadow.desc;
    if (!desc || !s_list_base) return;
    shadow_list_read(cpu, cpu->read_word(desc + 0x08));
    s_list_game_n = s_list_n;
}

/* ---- the widened view's list entries ------------------------------------- */
/* The moby cull appends a moby's list record once the moby has passed its 4:3
 * lateral test, so a moby only the widened view draws has none. These records
 * are built for it from the game's own data and rule, and appended after the
 * game's records. */
#define SHADOW_SYNTH_MAX 256
/* The cull's view-Z bound for a list record: addi -0x1200, bgez over the
 * append (r_moby.s 8001F34C, Spyro 2 mobydraw.s 80043AAC, Spyro 3 mobydraw.s
 * 800306CC). */
#define SHADOW_LIST_VIEWZ_MAX 0x1200
static shadow_list_rec s_synth[SHADOW_SYNTH_MAX];
static int s_synth_n;

/** @brief Whether the first n records of a list name a moby.
 *
 * @param l The list.
 * @param n The record count.
 * @param mob The moby's address.
 * @return 1 when a record names mob, else 0. */
static int shadow_list_has(const shadow_list_rec* l, int n, uint32_t mob) {
    for (int i = 0; i < n; i++) if (l[i].moby == mob) return 1;
    return 0;
}

/** @brief The list record the moby cull would append for one moby, by the cull's own rule.
 *
 * The cull appends when the moby's ground word (shadow.moby_ground) is
 * negative and the view-space Z of its centre is below 0x1200. The Z is the
 * cull's MVMVA: the camera matrix's third row over the quarter-world deltas
 * (cam.y - y, cam.z - z, x - cam.x), each an arithmetic shift by 2, the
 * product shifted by 12. The record's size word is the radius byte of the
 * moby's current keyframe record: AnimationHeader = Model's animation
 * pointer at the animation id byte, keyframe = the frame byte (the first and
 * third bytes of the moby's animation state), byte shadow.frame_radius_off
 * of that 8-byte record (r_moby.s 8001F268-8001F378, Spyro 2 mobydraw.s
 * 800439D0-80043AD8, Spyro 3 mobydraw.s 800305F0-800306F8).
 * @param cpu The guest CPU state, read only.
 * @param v The projection view; supplies the camera matrix and the staged
 *        camera position.
 * @param mob The moby's address.
 * @param out Receives the record.
 * @return 1 when the cull's rule lists the moby, else 0. */
static int shadow_synth_record(CPUState* cpu, const ngte_view* v, uint32_t mob, shadow_list_rec* out) {
    if ((int32_t)cpu->read_word(mob + game_layout.shadow.moby_ground) >= 0) return 0;
    const uint32_t pos = mob + game_layout.moby.pos_off;
    const int32_t ir3 = ((int32_t)cpu->read_word(pos + 0) - (int32_t)v->cam_ram[0]) >> 2;
    const int32_t ir1 = ((int32_t)v->cam_ram[1] - (int32_t)cpu->read_word(pos + 4)) >> 2;
    const int32_t ir2 = ((int32_t)v->cam_ram[2] - (int32_t)cpu->read_word(pos + 8)) >> 2;
    const int64_t mac3 = ((int64_t)v->cam_M[2][0] * ir1 + (int64_t)v->cam_M[2][1] * ir2 +
                          (int64_t)v->cam_M[2][2] * ir3) >> 12;
    if (mac3 >= SHADOW_LIST_VIEWZ_MAX) return 0;
    const uint32_t cls = cpu->read_half(mob + game_layout.moby.class_off);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    if (model < 0x80000000u || model >= 0x80800000u) return 0;
    const uint32_t st = mob + game_layout.moby.anim_state_off;
    const uint32_t hdr = cpu->read_word(model + game_layout.moby.model_anims_off +
                                        (uint32_t)cpu->read_byte(st + 0) * 4u);
    if (hdr < 0x80000000u || hdr >= 0x80800000u) return 0;
    const uint32_t frame = cpu->read_byte(st + 2);
    out->moby = mob;
    out->size = cpu->read_byte(hdr + game_layout.moby.anim_frames +
                               frame * game_layout.moby.anim_frame_stride +
                               game_layout.shadow.frame_radius_off);
    return 1;
}

/** @brief Offers each moby of one set to the widened-view records.
 *
 * A moby already in the game's records or in s_synth is skipped.
 * @param cpu The guest CPU state, read only.
 * @param v The projection view.
 * @param set The mobys.
 * @param n The set's length. */
static void shadow_synth_offer(CPUState* cpu, const ngte_view* v, const uint32_t* set, int n) {
    for (int i = 0; i < n && s_synth_n < SHADOW_SYNTH_MAX; i++) {
        const uint32_t mob = set[i];
        if (mob < 0x80000000u || mob >= 0x80800000u) continue;
        if (shadow_list_has(s_list, s_list_game_n, mob) || shadow_list_has(s_synth, s_synth_n, mob)) continue;
        shadow_list_rec r;
        if (shadow_synth_record(cpu, v, mob, &r)) s_synth[s_synth_n++] = r;
    }
}

/** @brief Rebuilds the widened-view records from this tick's drawn mobys.
 *
 * Empty in a view no wider than the 4:3 frame (ngte_view.extended 0), on
 * a layout without shadow.frame_radius_off, or while a cutscene renderer owns
 * the draw (moby_native_cutscene_owns_draw). Otherwise every moby of the
 * canonical drawn set (cull records, the widened reveal's appended records
 * among them) and of the host reveal's set that the game did not list is
 * offered to shadow_synth_record. Reads PSX RAM only.
 * @param cpu The guest CPU state, read only. */
static void shadow_synth_update(CPUState* cpu) {
    s_synth_n = 0;
    if (!game_layout.shadow.frame_radius_off) return;
    ngte_view v; ngte_view_build(cpu, &v);
    if (!v.extended) return;
    /* The cutscene renderer draws its own actor set and no other moby, and
     * the reveal walk returns before rebuilding its set while it does, so
     * that set still names the mobys revealed before the cutscene. */
    if (moby_native_cutscene_owns_draw(cpu)) return;
    const uint32_t* set = NULL;
    int n = moby_native_canon_drawn_mobys(&set);
    shadow_synth_offer(cpu, &v, set, n);
    n = moby_native_revealed_mobys(&set);
    shadow_synth_offer(cpu, &v, set, n);
}

/** @brief Appends the widened-view records after the game's records in s_list, up to SHADOW_LIST_MAX. */
static void shadow_list_append_synth(void) {
    s_list_game_n = s_list_n;
    for (int i = 0; i < s_synth_n && s_list_n < SHADOW_LIST_MAX; i++) s_list[s_list_n++] = s_synth[i];
}

/* ---- moby-shadow dump ----------------------------------------------------- */
/** @brief Prints the moby-shadow descriptor, the camera and the first six drawn-moby list entries, then the capture.
 *
 * The descriptor line decodes the tpage and its ABR from uv1's high half; each
 * list entry prints its moby's position and packed ground word (ground
 * height, idxB, idxA), or "bad ptr" for an entry outside main RAM. The list is
 * read live from the latched base.
 * @param cpu The guest CPU state. */
static void shadow_moby_dump(CPUState* cpu) {
    const uint32_t desc = game_layout.shadow.desc;
    fprintf(stderr, "\n=== [shadow-moby-dump f=%u] ===\n", openpete_frame);
    uint32_t uv0 = cpu->read_word(desc + 0x00);
    uint32_t uv1 = cpu->read_word(desc + 0x04);
    uint32_t list_end = cpu->read_word(desc + 0x08);
    fprintf(stderr, "  descriptor: uv0=%08X uv1=%08X shadow_list(end)=%08X  tpage=%04X abr=%d\n",
            uv0, uv1, list_end, (unsigned)(uv1 >> 16), (int)((uv1 >> 21) & 3u));
    fprintf(stderr, "  camera matrix (+0x00):");
    for (int i = 0; i < 5; i++) fprintf(stderr, " %08X", cpu->read_word(game_layout.cam.base + (uint32_t)i*4));
    fprintf(stderr, "\n  camera pos = (%d, %d, %d)\n",
            (int)cpu->read_word(game_layout.cam.position + 0),
            (int)cpu->read_word(game_layout.cam.position + 4),
            (int)cpu->read_word(game_layout.cam.position + 8));
    fprintf(stderr, "  drawn-moby list [%08X, %08X):\n", s_list_base, list_end);
    int shown = 0;
    for (uint32_t e = s_list_base; s_list_base && e < list_end && shown < 6; e += 8, shown++) {
        uint32_t mob = cpu->read_word(e + 0);
        uint32_t v1  = cpu->read_word(e + 4);
        if (mob < 0x80000000u || mob >= 0x80800000u) { fprintf(stderr, "    [%d] mob=%08X v1=%08X (bad ptr)\n", shown, mob, v1); continue; }
        int px = (int)cpu->read_word(mob + game_layout.shadow.moby_pos + 0);
        int py = (int)cpu->read_word(mob + game_layout.shadow.moby_pos + 4);
        int pz = (int)cpu->read_word(mob + game_layout.shadow.moby_pos + 8);
        uint32_t sd = cpu->read_word(mob + game_layout.shadow.moby_ground);
        fprintf(stderr, "    [%d] mob=%08X entry1=%08X  pos=(%d,%d,%d) ground=%08X(z=%d idxB=%d idxA=%d)\n",
                shown, mob, v1, px, py, pz, sd, sd & 0xFFFF, (sd >> 16) & 0x3F, (sd >> 22) & 0x3F);
    }
    shadow_dump_prims("shadow-moby");
    fprintf(stderr, "=== end shadow-moby-dump ===\n\n");
}

/* ===========================================================================
 *  NATIVE WALK - re-derive the moby shadows from gamestate in float.
 *  Reproduces the per-moby ground-point projection, the oriented-quad matrix
 *  assembly (two MVMVA passes per packed orientation index), the 4 fan corners
 *  and the distance fade, projecting in float so the geometry carries
 *  sub-pixel/HD precision and sub-tick interpolation. Which records draw is the
 *  game's own verdict on the GTE's integer values (shadow_record_verdict),
 *  never a float re-test. The VERIFY path compares
 *  the SXY shape against the captured FT3 prims (the .s int16 output as the
 *  oracle).
 * =========================================================================== */
/** @brief Saturates `v` to -32768..32767.
 *
 * @param v The value.
 * @return The saturated value. */
static int clamp16i(long v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : (int)v); }

void shadow_mvmva_ir(const int R[3][3], int vx, int vy, int vz, int ir[3]) {
    for (int i = 0; i < 3; i++) {
        int64_t m = (int64_t)R[i][0]*vx + (int64_t)R[i][1]*vy + (int64_t)R[i][2]*vz;
        ir[i] = clamp16i((long)(m >> 12));
    }
}

void shadow_ground_tr(const int C[3][3], double vx, double vy, double vz, double TR[3]) {
    ngte_mac(C, vx, vy, vz, &TR[0], &TR[1], &TR[2]);
}

void shadow_project(const int R[3][3], const double TR[3], double ofx, double ofy,
                    const ngte_view* v, double vx, double vy, double vz, shadow_pt* o) {
    ngte_proj r;   /* shared GTE-faithful pipeline, ±1024 SXY bound ON */
    ngte_project_gte(R, TR, vx, vy, vz, ofx, ofy, v, 1, &r);
    o->sx = (float)r.sx; o->sy = (float)r.sy; o->sz = (float)r.m3;
    o->m1 = (float)r.m1; o->m2 = (float)r.m2;
}

/** @brief Assembles the oriented-quad matrix R' from the two packed orientation indices.
 *
 * A nonzero idxB replaces the camera matrix's columns 1 and 2 with
 * C·(0, cosB, sinB) and C·(0, -sinB, cosB); a nonzero idxA then replaces
 * columns 0 and 1 of that result with R'·(cosA, sinA, 0) and
 * R'·(-sinA, cosA, 0). Each product is shadow_mvmva_ir. The sine of index i
 * is the halfword at game_layout.sin_table + i*8, its cosine 0x80 bytes on.
 *
 * game_layout.shadow.matrix_fold_a selects the idxB block's handwritten
 * variant: it discards the new column-1 X term and instead adds idxA*8 into
 * the R11/R12 word, so R12 becomes 0 (or the carry of that addition) and R11
 * is biased before the idxA rotation reads it.
 * @param cpu The guest CPU state; supplies the sine table.
 * @param C The camera matrix, Q12.
 * @param idxA The A index, bits 22..27 of the ground word.
 * @param idxB The B index, bits 16..21 of the ground word.
 * @param Rp Receives R'. */
static void assemble_matrix(CPUState* cpu, const int C[3][3], int idxA, int idxB, int Rp[3][3]) {
    const uint32_t sinb = game_layout.sin_table;
    memcpy(Rp, C, sizeof(int)*9);
    if (idxB != 0) {
        int sinB = shadow_rd_s16(cpu, sinb + (uint32_t)idxB*8);
        int cosB = shadow_rd_s16(cpu, sinb + (uint32_t)idxB*8 + 0x80);
        int P[3], Q[3];
        shadow_mvmva_ir(Rp, 0,  cosB, sinB, P);     /* C·(0, cosB, sinB) */
        shadow_mvmva_ir(Rp, 0, -sinB, cosB, Q);     /* C·(0, -sinB, cosB) */
        int n[3][3];
        if (game_layout.shadow.matrix_fold_a) {
            int packed11 = (Rp[0][0] & 0xFFFF) + (idxA << 3);
            n[0][0]=shadow_s16(packed11 & 0xFFFF); n[0][1]=shadow_s16((packed11>>16)&0xFFFF);
        } else {
            n[0][0]=Rp[0][0];               n[0][1]=P[0];
        }
        n[0][2]=Q[0];
        n[1][0]=Rp[1][0];               n[1][1]=P[1];                       n[1][2]=Q[1];
        n[2][0]=Rp[2][0];               n[2][1]=P[2];                       n[2][2]=Q[2];
        memcpy(Rp, n, sizeof n);
    }
    if (idxA != 0) {
        int sinA = shadow_rd_s16(cpu, sinb + (uint32_t)idxA*8);
        int cosA = shadow_rd_s16(cpu, sinb + (uint32_t)idxA*8 + 0x80);
        int P[3], Q[3];
        shadow_mvmva_ir(Rp,  cosA, sinA, 0, P);     /* Rp·(cosA, sinA, 0) */
        shadow_mvmva_ir(Rp, -sinA, cosA, 0, Q);     /* Rp·(-sinA, cosA, 0) */
        int n[3][3];
        n[0][0]=P[0];     n[0][1]=Q[0];     n[0][2]=Rp[0][2];
        n[1][0]=P[1];     n[1][1]=Q[1];     n[1][2]=Rp[1][2];
        n[2][0]=P[2];     n[2][1]=Q[2];     n[2][2]=Rp[2][2];
        memcpy(Rp, n, sizeof n);
    }
}

/** @brief 4096² · C⁻¹ via the adjugate: the rows the decal reconstructs a camera-relative GTE input vector with.
 *
 * A view-space point's input vector is V_j = (inv[j] · MAC) / 4096. A
 * determinant within 1e-6 of zero is replaced by ±1e-6, keeping its sign.
 * @param C The camera matrix, Q12.
 * @param inv Receives the scaled inverse. */
static void shadow_cam_inverse(const int C[3][3], double inv[3][3]) {
    double a = C[0][0], b = C[0][1], c = C[0][2];
    double d = C[1][0], e = C[1][1], f = C[1][2];
    double g = C[2][0], hh = C[2][1], ii = C[2][2];
    double det = a*(e*ii - f*hh) - b*(d*ii - f*g) + c*(d*hh - e*g);
    if (det > -1e-6 && det < 1e-6) det = det < 0.0 ? -1e-6 : 1e-6;
    double k = 4096.0 * 4096.0 / det;
    const double r[3][3] = {
        { (e*ii - f*hh) * k, (c*hh - b*ii) * k, (b*f - c*e) * k },
        { (f*g  - d*ii) * k, (a*ii - c*g)  * k, (c*d - a*f) * k },
        { (d*hh - e*g)  * k, (b*g  - a*hh) * k, (a*e - b*d) * k },
    };
    memcpy(inv, r, sizeof r);
}

/* Pushes the shadow decal's camera-reconstruction parameters; see shadow.h. */
void shadow_push_decal_params(CPUState* cpu, const ngte_view* v, const int C[3][3],
                              double ofx, double ofy, double H,
                              openpete_shadow_decal_params* out) {
    openpete_shadow_decal_params sp;
    openpete_shadow_decal_params* p = &sp;
    double inv[3][3];
    shadow_cam_inverse(C, inv);
    for (int i = 0; i < 3; i++) {
        p->cam_col0[i] = (float)inv[0][i];
        p->cam_col1[i] = (float)inv[1][i];
        p->cam_col2[i] = (float)inv[2][i];
    }
    p->cam_pos_up = (float)v->cam_ram[2];                                  /* game z field */
    p->ofx = (float)ofx; p->ofy = (float)ofy; p->h = (float)H;
    p->ground_up = game_layout.shadow.ground_plane
                 ? (float)(int32_t)cpu->read_word(game_layout.shadow.ground_plane) : 0.0f;
    const char* wh = getenv("OPENPETE_SHADOW_WALL_HEIGHT");
    p->wall_height = (wh && *wh) ? (float)atof(wh) : 400.0f;
    /* |up normal| threshold below which a surface is a wall face (walkable
     * slopes stay >~0.64; wall faces ~0). OPENPETE_SHADOW_STEEP_NY=0 = height-only. */
    const char* sn = getenv("OPENPETE_SHADOW_STEEP_NY");
    p->steep_nup = (sn && *sn) ? (float)atof(sn) : 0.5f;
    /* Raised-occluder-in-front (curb/platform) clause; PLAT_EPS=1e5 disables. */
    const char* pe = getenv("OPENPETE_SHADOW_PLAT_EPS");
    const char* ps = getenv("OPENPETE_SHADOW_PLAT_SLACK");
    p->plat_eps = (pe && *pe) ? (float)atof(pe) : 150.0f;
    p->plat_slack = (ps && *ps) ? (float)atof(ps) : 48.0f;
    p->valid = 1;
    openpete_native_set_shadow_decal_params(p);
    if (out) *out = *p;
}

/** @brief Rounds a view-Z to the nearest integer clamped to 0..0xFFFF.
 *
 * @param sz The view-Z.
 * @return The clamped value. */
static uint32_t shadow_sz16(float sz) {
    long z = lrintf(sz);
    return z < 0 ? 0u : (z > 0xFFFF ? 0xFFFFu : (uint32_t)z);
}

int shadow_ot_slot(float sza, float sz1, float sz2, int shift, int bias) {
    uint32_t key = shadow_sz16(sz1) + shadow_sz16(sz2);
    key += key >> 1;
    key += shadow_sz16(sza);
    return (int)(key >> shift) - bias;
}

float shadow_slot_koff(int slot) {
    return slot < 0 ? 0.0f : (float)slot * (float)OPENPETE_OT_SLOT_WORLD + 0.5f;
}

#define MOBY_SHADOW_SKIP (-2)
/** @brief The ordering-table slot of one moby shadow triangle (anchor, p1, p2).
 *
 * The key is (SZ1 + SZ2) * 3/2 + SZA on the GTE's integer SZ (each sum's
 * half a logical shift); the slot is key >> 7 less the moby's signed depth byte
 * less the game's constant. Returns -1 (not keyed) on a game whose layout
 * does not mirror it (moby_ot_bias 0). A mirrored slot below zero is returned
 * as MOBY_SHADOW_SKIP: the game does not insert that triangle (the bltz after
 * the subtraction, Spyro 1 r_shadows.s 0x8005A414, Spyro 2 mobyfunc.s
 * 0x80049E7C, Spyro 3 mobyfunc.s 0x8002E250), so the feed does not draw it;
 * the build keeps it, where the GTE A/B compares four triangles per moby.
 * @param cpu The guest CPU state.
 * @param mob The moby's address.
 * @param sza The anchor's SZ.
 * @param sz1 The fan triangle's second vertex SZ.
 * @param sz2 The fan triangle's third vertex SZ.
 * @return The slot, -1, or MOBY_SHADOW_SKIP. */
static int moby_shadow_slot(CPUState* cpu, uint32_t mob, int sza, int sz1, int sz2) {
    const uint32_t off = game_layout.shadow.moby_ot_bias;
    if (!off) return -1;
    int bias = (int)(int8_t)cpu->read_byte(mob + off) - game_layout.shadow.moby_ot_bias_sub;
    uint32_t key = (uint32_t)sz1 + (uint32_t)sz2;
    key += key >> 1;
    key += (uint32_t)sza;
    int slot = (int)(key >> 7) - bias;
    return slot < 0 ? MOBY_SHADOW_SKIP : slot;
}

/* native moby-shadow tris (4 FT3 fan tris per moby) */
typedef struct { float sx[3], sy[3], sz[3]; int u[3], v[3]; uint32_t rgb;
                 int tpx, tpy, clx, cly, bpp, blend, slot; } moshtri;
#define MAX_MOSH 1024
static moshtri s_mosh[MAX_MOSH]; static int s_mosh_n;

/** @brief Packs a float-projected point as the GTE's (SY << 16) | (SX & 0xFFFF) word.
 *
 * Each component is floored, the arithmetic shift MAC0 >> 16 takes, so a
 * point whose X lies in (-1, 0) packs as -1 with its sign bit set. The
 * admission reads the integer evaluation's words; this form serves the
 * calibration's float verdict.
 * @param p The point.
 * @return The packed word. */
static int32_t packed_sxy(const shadow_pt* p) {
    int xi = (int)floorf(p->sx), yi = (int)floorf(p->sy);
    return (int32_t)(((uint32_t)yi << 16) | ((uint32_t)xi & 0xFFFFu));
}

/** @brief The sign-extended SX of a packed SXY word.
 *
 * @param p The packed word.
 * @return SX. */
static int sxy_x(int32_t p) { return shadow_s16((int)((uint32_t)p & 0xFFFFu)); }

/** @brief The anchor-extent rejection, used when the layout's cull form is not the four-corner one.
 *
 * The anchor alone must be on screen. Its packed word is rejected when it is
 * at most 0 (a negative SY, or SX and SY both 0) or at least 0xF00000, and
 * its sign-extended SX when it is at most -8 or at least 0x208 (r_shadows.s
 * 8005A32C-8005A350).
 * @param packed The anchor's packed SXY word.
 * @return 1 to reject the shadow, else 0. */
static int cull_anchor(int32_t packed) {
    if (packed <= 0) return 1;
    if (packed - 0xF00000 >= 0) return 1;
    int xext = sxy_x(packed);
    if (xext + 8 <= 0) return 1;
    if (xext - 0x208 >= 0) return 1;
    return 0;
}

/** @brief The four-corner rejection of SHADOW_CULL_SPYRO3 layouts.
 *
 * Rejects when every corner is above the screen, when corners 1..3 are all at
 * or below row 0xE4, when corners 1..3 are all left of column 0, or when
 * every corner is at or right of column 0x200 and not every corner is
 * exactly at it. Corner 0 is absent from the second and third tests and the
 * first bound is folded into the running OR: the handwritten routine keeps
 * the constant in the register it accumulates into, so those two tests answer
 * for three corners only.
 * @param p The four corners' packed SXY words.
 * @return 1 to reject the shadow, else 0. */
static int cull_corners(const int32_t p[4]) {
    int32_t xs[4];
    for (int k = 0; k < 4; k++) xs[k] = (int32_t)((uint32_t)p[k] << 16);   /* X into the high half */
    if ((p[0] & p[1] & p[2] & p[3]) < 0) return 1;
    const int32_t ybound = 0x00E40000;
    int32_t ory = ybound | (p[1] - ybound) | (p[2] - ybound) | (p[3] - ybound);
    if (ory >= 0) return 1;
    if ((ory & xs[1] & xs[2] & xs[3]) < 0) return 1;
    const int32_t xbound = 0x02000000;
    int32_t orx = (xs[0] - xbound) | (xs[1] - xbound) | (xs[2] - xbound) | (xs[3] - xbound);
    if (orx > 0) return 1;
    return 0;
}

/** @brief The vertical arm of cull_anchor: rejects an anchor whose packed word is at most 0 or at least 0xF00000.
 *
 * @param packed The anchor's packed SXY word.
 * @return 1 to reject the shadow, else 0. */
static int cull_anchor_vertical(int32_t packed) {
    return packed <= 0 || packed - 0xF00000 >= 0;
}

/** @brief The vertical arms of cull_corners: every corner above the screen, or corners 1..3 all at or below row 0xE4.
 *
 * @param p The four corners' packed SXY words.
 * @return 1 to reject the shadow, else 0. */
static int cull_corners_vertical(const int32_t p[4]) {
    if ((p[0] & p[1] & p[2] & p[3]) < 0) return 1;
    const int32_t ybound = 0x00E40000;
    return (ybound | (p[1] - ybound) | (p[2] - ybound) | (p[3] - ybound)) >= 0;
}

/** @brief The game's vertical test with its rows scaled to a --fov scale below 1.
 *
 * The rows bound the 4:3 frame's band at the GTE's H; the view projects at
 * H · s, so in the points' coordinates its band is the game's scaled about
 * OFY by 1 / s. Each arm keeps the game's form on SY: cull_corners_vertical's
 * every corner above row 0 or corners 1..3 all at or below row 0xE4 (Spyro 2
 * mobyfunc.s:1110-1122, 0x80049D34-0x80049D64; Spyro 3 mobyfunc.s:1108-1120,
 * 0x8002E108-0x8002E138), cull_anchor_vertical's anchor above row 0 or at or
 * below row 0xF0 (r_shadows.s 8005A32C-8005A338). At s >= 1 it is the game's
 * own test.
 * @param v The projection view; supplies ofy and s.
 * @param anchor The anchor's packed SXY word.
 * @param c The four corners' packed SXY words.
 * @return 1 to reject the shadow, else 0. */
static int shadow_vertical_reject(const ngte_view* v, int32_t anchor, const int32_t c[4]) {
    const int spyro3 = game_layout.shadow.cull_layout == SHADOW_CULL_SPYRO3;
    if (v->s >= 1.0) return spyro3 ? cull_corners_vertical(c) : cull_anchor_vertical(anchor);
    const double top = v->ofy - v->ofy / v->s;
    const double bot = v->ofy + ((spyro3 ? 0xE4 : 0xF0) - v->ofy) / v->s;
    if (!spyro3) {
        const double y = (double)(anchor >> 16);
        return y < top || y >= bot;
    }
    int above = 1, below = 1;
    for (int k = 0; k < 4; k++) {
        const double y = (double)(c[k] >> 16);
        above &= y < top;
        if (k) below &= y >= bot;
    }
    return above || below;
}

/** @brief The footprint's lateral test on the screen-X window [xlo, xhi]: rejects when all four corners lie left of xlo or all at or right of xhi.
 *
 * @param c The four corners' packed SXY words.
 * @param xlo The window's left bound.
 * @param xhi The window's right bound.
 * @return 1 to reject the shadow, else 0. */
static int lateral_footprint(const int32_t c[4], float xlo, float xhi) {
    int left = 1, right = 1;
    for (int k = 0; k < 4; k++) {
        const float x = (float)sxy_x(c[k]);
        left  &= x < xlo;
        right &= x >= xhi;
    }
    return left || right;
}

/* One canonical walk's screen-test tallies (OPENPETE_SHADOW_WIDE_CENSUS). */
typedef struct { int listed, game_admit, wide_admit, synth; } shadow_wide_census;

/** @brief The screen-extent rejection of one moby shadow under the view's screen-X window.
 *
 * The points are the integer evaluation's packed SXY words, projected at the
 * GTE's H. In a view no wider than the game's 4:3 frame (ngte_view.extended 0)
 * the result is the game's own test, cull_corners or cull_anchor. In a wider
 * view each game takes its own vertical test with its rows scaled by the
 * view's height (shadow_vertical_reject) and every layout takes the lateral test by the
 * footprint on the widened window, [xlo, xhi] scaled about its centre by 1 / s
 * into the points' H (lateral_footprint over the four corners): a shadow whose
 * anchor has left the window while part of its diamond is still inside is
 * drawn. Spyro 1's corners are the fan's own RTPT and RTPS results
 * (r_shadows.s), evaluated before the screen test.
 * @param v The projection view; supplies extended, s, xlo and xhi.
 * @param anchor The anchor's packed SXY word.
 * @param c The four corners' packed SXY words.
 * @param cs Tallies the game's verdict and the widened admissions; NULL off
 *        the canonical walk.
 * @param game_only 1 answers the game's own test in every view.
 * @return 1 to reject the shadow, else 0. */
static int shadow_screen_reject(const ngte_view* v, int32_t anchor, const int32_t c[4],
                                shadow_wide_census* cs, int game_only) {
    const int spyro3 = game_layout.shadow.cull_layout == SHADOW_CULL_SPYRO3;
    const int game = spyro3 ? cull_corners(c) : cull_anchor(anchor);
    if (cs && !game) cs->game_admit++;
    if (game_only || !v->extended) return game;
    const int vert = shadow_vertical_reject(v, anchor, c);
    float xlo = v->xlo, xhi = v->xhi;
    if (v->s != 1.0) {   /* the points are at H_canon: the window's half-width is 256 F / s */
        const float cx = 0.5f * (xlo + xhi), hw = 0.5f * (xhi - xlo) / (float)v->s;
        xlo = cx - hw; xhi = cx + hw;
    }
    const int rej = vert || lateral_footprint(c, xlo, xhi);
    if (cs && game && !rej) cs->wide_admit++;
    return rej;
}

/** @brief Reports whether the moby-shadow walk applies the record own-motion offsets; OPENPETE_SHADOW_INTERP=0 turns them off.
 *
 * Latched on the first call. Off, every entry the spine does not interpolate
 * stays at the newest tick's world pose. It is the engagement leg of
 * tests/harness/check_feeder_subtick.sh --path shadow: the harness reads the
 * whole tick step back under it.
 * @return 1 when on, else 0. */
static int shadow_interp_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SHADOW_INTERP"); v = !(e && *e && *e == '0'); }
    return v;
}

/* The world pose of one drawn-moby list entry; see shadow_entry_world. */
typedef struct { double px, py, gnd; uint32_t sd; } shadow_entry_pose;

/** @brief Reads the world pose one drawn-moby list entry projects from.
 *
 * The pose is the moby's position in world units and the low halfword of its
 * packed ground word, the ground height in quarter-world units
 * (mobyfunc.s:930-933 reads all three straight out of the Moby, not out of any
 * render record).
 *
 * Those live reads are the state the newest tick left, so a present under an
 * interpolated camera would place a moving moby's shadow one whole tick ahead
 * of the world it falls on. Two sources carry the entry back onto the pose the
 * present shows. A game whose interpolation spine answers for this object
 * hands over its lerped position directly. Otherwise the record own-motion
 * offsets do it: moby_record_world_offset on the position, and its sibling on
 * the ground halfword, which a moby crossing a step in the terrain changes as
 * fast as it changes its position. Both are zero at the end of the tick's own
 * interval and neither is read under the tick's own camera, so the canonical
 * feed is the unmodified one.
 *
 * The entry's radius word and the list membership stay the tick's: both live
 * in the list record rather than in the Moby, and no per-tick copy of the list
 * exists to pair them against.
 *
 * @param cpu The guest CPU state.
 * @param v The projection view; supplies the camera-interpolation flag.
 * @param mob The moby's address.
 * @param off_pos The position's offset in the Moby.
 * @param off_gnd The packed ground word's offset in the Moby.
 * @param e Receives the ground word, the position and the ground height.
 * @return 0 when the ground halfword is zero, the game's own verdict that this
 *         moby casts no shadow, else 1. */
static int shadow_entry_world(CPUState* cpu, const ngte_view* v, uint32_t mob,
                              uint32_t off_pos, uint32_t off_gnd,
                              shadow_entry_pose* e) {
    e->sd = cpu->read_word(mob + off_gnd);
    e->gnd = (double)(int)(e->sd & 0xFFFFu);
    if (e->gnd == 0.0) return 0;
    e->px = (double)(int32_t)cpu->read_word(mob + off_pos + 0);
    e->py = (double)(int32_t)cpu->read_word(mob + off_pos + 4);
    double mf[3];
    if (openpete_native_interp_pos(mob, mf)) { e->px = mf[0]; e->py = mf[1]; return 1; }
    if (!v->cam_interp || !shadow_interp_on()) return 1;
    double mw[3];
    if (moby_record_world_offset(mob, mw)) { e->px += mw[0]; e->py += mw[1]; }
    double dg;
    if (moby_record_halfword_offset(mob, off_gnd, &dg)) e->gnd += dg;
    return 1;
}

/* One walk's invariants: the projection view, the camera in world/4 units
 * (fractional for the drawn geometry, the game's floored camera/4 for the
 * integer evaluation), the GTE's H, the FT3 material the fans share, and the
 * screen-test mode. */
typedef struct {
    const ngte_view* v;
    int C[3][3];
    double cam[3];
    uint32_t icam[3];
    int H;
    int u[3], vt[3];
    int tpx, tpy, clx, cly, bpp, blend;
    shadow_wide_census* cs;
    int game_only;
} shadow_walk;

/* One admitted list record: the projected ground anchor, the four diamond
 * corners, the anchor's view-space Z and each fan triangle's ordering-table
 * slot from the integer evaluation. */
typedef struct { shadow_pt anchor, c[4]; double trz; int slot[4]; } shadow_fan_pts;

/* The integer evaluation of one list record, the values the game's GTE holds:
 * the packed SXY and SZ of the anchor and of the four corners, the anchor's
 * MAC3 (the view-space Z the corners translate by), the NCLIP MAC0 and each
 * fan triangle's ordering-table slot. */
typedef struct {
    int32_t anchor, c[4];
    int32_t asz, sz[4];
    int32_t trz, nclip;
    int slot[4];
} shadow_int_pts;

/* The integer evaluation's verdict: admitted, or the first test that rejects. */
enum { SHADOW_ADMIT = 0, SHADOW_REJ_GROUND, SHADOW_REJ_Z, SHADOW_REJ_NCLIP, SHADOW_REJ_SCREEN };

/** @brief Decodes the shadow sprite's FT3 material from the descriptor's UV words.
 *
 * UV0 is +0x00, UV1 is +0x04 (less 0x1F00 on a bottom-right layout), UV2 is
 * UV0 moved 0x1F rows down; the CLUT is UV0's high half and the tpage UV1's.
 * The semi-transparency rate is the tpage's ABR field, level data; rate 3
 * (B+F/4) has no native pipeline and draws additive.
 * @param cpu The guest CPU state, read only.
 * @param desc The descriptor's address.
 * @param w Receives the UVs and the decoded tpage, CLUT and blend. */
static void shadow_walk_material(CPUState* cpu, uint32_t desc, shadow_walk* w) {
    uint32_t uv0 = cpu->read_word(desc + 0x00);
    uint32_t uv1 = cpu->read_word(desc + 0x04);
    if (game_layout.shadow.uv1_bottom_right) uv1 -= 0x1F00u;   /* bottom-right -> top-right */
    w->u[0] = (int)(uv0 & 0xFF); w->vt[0] = (int)((uv0 >> 8) & 0xFF);
    w->u[1] = (int)(uv1 & 0xFF); w->vt[1] = (int)((uv1 >> 8) & 0xFF);
    w->u[2] = w->u[0];           w->vt[2] = (int)(((uv0 >> 8) & 0xFF) + 0x1F);
    ngte_tpage tp;
    ngte_decode_tpage((unsigned)(uv1 >> 16), (unsigned)(uv0 >> 16), 0u, &tp);
    w->tpx = tp.tpx; w->tpy = tp.tpy; w->bpp = tp.bpp;
    w->clx = tp.clx; w->cly = tp.cly;
    w->blend = ngte_abr_blend(tp.abr, NGTE_ABR_QUARTER_AS_ADD);
}

/** @brief Fills a walk's camera, projection and material from the view.
 *
 * The drawn geometry takes the camera in world/4 units as a fraction (not
 * >> 2), keeping the low 2 bits: a floored /4 integer would step the shadow
 * in 4-world-unit chunks under camera motion, and the view's interpolated
 * camera is used un-rounded. The integer evaluation takes the game's own
 * camera, the staged RAM words shifted right by 2 as unsigned values
 * (r_shadows.s 80059FB4-80059FBC; Spyro 2 mobyfunc.s 80049A30-80049A38).
 * @param cpu The guest CPU state, read only.
 * @param desc The descriptor's address.
 * @param view The projection view.
 * @param w Receives the walk; its census and mode fields are left as given. */
static void shadow_walk_init(CPUState* cpu, uint32_t desc, const ngte_view* view, shadow_walk* w) {
    w->v = view;
    memcpy(w->C, view->cam_M, sizeof w->C);
    for (int i = 0; i < 3; i++) {
        w->cam[i] = view->cam[i] / 4.0;
        w->icam[i] = cpu->read_word(game_layout.cam.base + 0x28u + 4u * (uint32_t)i) >> 2;
    }
    w->H = (int)view->H_canon;
    shadow_walk_material(cpu, desc, w);
}

/** @brief GTE RTPS of one vertex in the walk's projection, as packed SXY and SZ.
 *
 * @param w The walk; supplies OFX, OFY and H.
 * @param M The rotation, Q12.
 * @param tr The translation.
 * @param vx VX0.
 * @param vy VY0.
 * @param vz VZ0.
 * @param sxy Receives the packed (SY << 16) | SX word.
 * @param sz Receives SZ3.
 * @param mac Receives MAC1..MAC3 when non-NULL. */
static void shadow_rtps_word(const shadow_walk* w, const int M[3][3], const int tr[3],
                             int vx, int vy, int vz, int32_t* sxy, int32_t* sz, int32_t mac[3]) {
    ngte_rtps_out o;
    ngte_rtps_int(M, tr, vx, vy, vz, w->v->ofx, w->v->ofy, w->H, &o);
    *sxy = (int32_t)(((uint32_t)(uint16_t)o.sy << 16) | (uint32_t)(uint16_t)o.sx);
    *sz = o.sz3;
    if (!mac) return;
    for (int k = 0; k < 3; k++)
        mac[k] = (int32_t)((((int64_t)tr[k] << 12) + (int64_t)M[k][0] * vx
                            + (int64_t)M[k][1] * vy + (int64_t)M[k][2] * vz) >> 12);
}

/** @brief The game's admission of one list record, evaluated on the GTE's integer values.
 *
 * Reproduces the draw's own arithmetic (Spyro 1 r_shadows.s 8005A07C-8005A350,
 * Spyro 2 mobyfunc.s 80049AA4-80049DA8, Spyro 3 mobyfunc.s 8002DE78-8002E17C).
 * The anchor vector is VZ0 = (X >> 2) - camX/4 and the word
 * ((camZ/4 - ground) << 16) + (camY/4 - (Y >> 2)) as VXY0, every shift
 * logical, so a negative VX0 borrows one from VY0 as the 32-bit add does.
 * RTPS of it under the camera matrix gives MAC1..3, the translation the
 * corners take, and the anchor SXY; the record is rejected when the ground
 * halfword is zero, when MAC3 - 0x1000 >= 0, when NCLIP over corners 0..2
 * is negative, or by shadow_screen_reject on the integer SXY words. The
 * corners are RTPT (0, 0, r), (r.lo, r.hi, 0), (0, 0, -r) and RTPS (-r, 0, 0)
 * under the oriented-quad matrix, with r the record's size word shifted by
 * shadow.size_shl and each component the low halfword the GTE loads. The
 * per-triangle slots are moby_shadow_slot over the integer SZ.
 * @param cpu The guest CPU state, read only.
 * @param w The walk.
 * @param rec The list record.
 * @param sd The moby's packed ground word.
 * @param ip Receives the integer values computed before the verdict.
 * @return SHADOW_ADMIT, or the SHADOW_REJ_* code of the rejecting test. */
static int shadow_record_verdict(CPUState* cpu, const shadow_walk* w, const shadow_list_rec* rec,
                                 uint32_t sd, shadow_int_pts* ip) {
    const uint32_t mob = rec->moby;
    const uint32_t gz = sd & 0xFFFFu;
    if (!gz) return SHADOW_REJ_GROUND;
    const uint32_t px = cpu->read_word(mob + game_layout.shadow.moby_pos + 0);
    const uint32_t py = cpu->read_word(mob + game_layout.shadow.moby_pos + 4);
    const uint32_t vxy = ((w->icam[2] - gz) << 16) + (w->icam[1] - (py >> 2));
    const int vx = shadow_s16((int)(vxy & 0xFFFFu)), vy = shadow_s16((int)(vxy >> 16));
    const int vz = shadow_s16((int)(((px >> 2) - w->icam[0]) & 0xFFFFu));
    static const int ZERO[3] = { 0, 0, 0 };
    int32_t mac[3];
    shadow_rtps_word(w, w->C, ZERO, vx, vy, vz, &ip->anchor, &ip->asz, mac);
    ip->trz = mac[2];
    if (mac[2] - 0x1000 >= 0) return SHADOW_REJ_Z;

    int Rp[3][3];
    assemble_matrix(cpu, w->C, (int)((sd >> 22) & 0x3F), (int)((sd >> 16) & 0x3F), Rp);
    const uint32_t radius = rec->size << game_layout.shadow.size_shl;
    const int rlo = shadow_s16((int)(radius & 0xFFFF)), rhi = shadow_s16((int)((radius >> 16) & 0xFFFF));
    const int nlo = shadow_s16(-rlo);
    const int tr[3] = { mac[0], mac[1], mac[2] };
    shadow_rtps_word(w, Rp, tr, 0,   0,   rlo, &ip->c[0], &ip->sz[0], NULL);   /* RTPT V0 */
    shadow_rtps_word(w, Rp, tr, rlo, rhi, 0,   &ip->c[1], &ip->sz[1], NULL);   /* RTPT V1 */
    shadow_rtps_word(w, Rp, tr, 0,   0,   nlo, &ip->c[2], &ip->sz[2], NULL);   /* RTPT V2 */
    shadow_rtps_word(w, Rp, tr, nlo, 0,   0,   &ip->c[3], &ip->sz[3], NULL);   /* RTPS 4th */
    int64_t x[3], y[3];
    for (int k = 0; k < 3; k++) { x[k] = sxy_x(ip->c[k]); y[k] = (int16_t)((uint32_t)ip->c[k] >> 16); }
    ip->nclip = (int32_t)(x[0] * y[1] + x[1] * y[2] + x[2] * y[0] - x[0] * y[2] - x[1] * y[0] - x[2] * y[1]);
    if (ip->nclip < 0) return SHADOW_REJ_NCLIP;
    if (shadow_screen_reject(w->v, ip->anchor, ip->c, w->cs, w->game_only)) return SHADOW_REJ_SCREEN;
    for (int k = 0; k < 4; k++) ip->slot[k] = moby_shadow_slot(cpu, mob, ip->asz, ip->sz[k], ip->sz[(k + 1) & 3]);
    return SHADOW_ADMIT;
}

/** @brief Projects one list record's shadow in float: the ground anchor, its view-space Z and the four corners.
 *
 * The corners are V0 = (0, 0, r), V1 = (r.lo, r.hi, 0), V2 = (0, 0, -r),
 * V3 = (-r, 0, 0) through the oriented-quad matrix from the ground word's two
 * angle indices; r is the record's size word shifted by shadow.size_shl.
 * @param cpu The guest CPU state, read only.
 * @param w The walk; supplies the fractional camera.
 * @param rec The list record.
 * @param ep The entry's world pose.
 * @param f Receives the points and the anchor's view-space Z. */
static void shadow_record_project(CPUState* cpu, const shadow_walk* w, const shadow_list_rec* rec,
                                  const shadow_entry_pose* ep, shadow_fan_pts* f) {
    const double ofx = w->v->ofx, ofy = w->v->ofy;
    const double vx = w->cam[1] - ep->py / 4.0;
    const double vy = w->cam[2] - ep->gnd;
    const double vz = ep->px / 4.0 - w->cam[0];
    double TR[3]; shadow_ground_tr(w->C, vx, vy, vz, TR);
    const double ZERO[3] = {0, 0, 0};
    shadow_project(w->C, ZERO, ofx, ofy, w->v, vx, vy, vz, &f->anchor);
    int Rp[3][3];
    assemble_matrix(cpu, w->C, (int)((ep->sd >> 22) & 0x3F), (int)((ep->sd >> 16) & 0x3F), Rp);
    const uint32_t radius = rec->size << game_layout.shadow.size_shl;
    const int rlo = shadow_s16((int)(radius & 0xFFFF)), rhi = shadow_s16((int)((radius >> 16) & 0xFFFF));
    shadow_project(Rp, TR, ofx, ofy, w->v,  0,    0,    rlo, &f->c[0]);
    shadow_project(Rp, TR, ofx, ofy, w->v,  rlo,  rhi,  0,   &f->c[1]);
    shadow_project(Rp, TR, ofx, ofy, w->v,  0,    0,   -rlo, &f->c[2]);
    shadow_project(Rp, TR, ofx, ofy, w->v, -rlo,  0,    0,   &f->c[3]);
    f->trz = TR[2];
}

/** @brief Admits one list record by the game's integer verdict and projects its drawn shadow in float.
 *
 * A record is rejected when its moby lies outside the first 8 MiB, its
 * ground word is zero (shadow_entry_world), or shadow_record_verdict rejects
 * it. The verdict reads the tick's own camera and moby words, so a sub-tick
 * present admits the set the tick admitted; the drawn points are
 * shadow_record_project under the view's camera and the entry's pose.
 * @param cpu The guest CPU state, read only.
 * @param w The walk.
 * @param rec The list record.
 * @param f Receives the projected points and the integer slots when admitted.
 * @return 1 when the shadow is drawn, else 0. */
static int shadow_record_admit(CPUState* cpu, const shadow_walk* w, const shadow_list_rec* rec,
                               shadow_fan_pts* f) {
    const uint32_t mob = rec->moby;
    if (mob < 0x80000000u || mob >= 0x80800000u) return 0;
    shadow_entry_pose ep;
    if (!shadow_entry_world(cpu, w->v, mob, game_layout.shadow.moby_pos,
                            game_layout.shadow.moby_ground, &ep)) return 0;
    shadow_int_pts ip;
    if (shadow_record_verdict(cpu, w, rec, ep.sd, &ip) != SHADOW_ADMIT) return 0;
    shadow_record_project(cpu, w, rec, &ep, f);
    memcpy(f->slot, ip.slot, sizeof f->slot);
    return 1;
}

/** @brief Stores one admitted shadow's four fan triangles (anchor, c[k], c[k+1]) in s_mosh, up to MAX_MOSH.
 *
 * The grey is 0x80 below a view-space Z of 0xC00 and (0x1000 - Z) >> 3
 * floored at 0 beyond it; each triangle carries the integer evaluation's
 * ordering slot and the walk's material.
 * @param w The walk.
 * @param f The projected points. */
static void shadow_fan_emit(const shadow_walk* w, const shadow_fan_pts* f) {
    const int trz = (int)f->trz;
    int g = (trz < 0xC00) ? 0x80 : ((0x1000 - trz) >> 3);
    if (g < 0) g = 0;
    const uint32_t grey = ((uint32_t)g) | ((uint32_t)g << 8) | ((uint32_t)g << 16);
    for (int k = 0; k < 4 && s_mosh_n < MAX_MOSH; k++) {
        const shadow_pt* p[3] = { &f->anchor, &f->c[k], &f->c[(k + 1) & 3] };
        moshtri* t = &s_mosh[s_mosh_n++];
        t->slot = f->slot[k];
        for (int i = 0; i < 3; i++) {
            t->sx[i] = p[i]->sx; t->sy[i] = p[i]->sy; t->sz[i] = p[i]->sz;
            t->u[i] = w->u[i];   t->v[i] = w->vt[i];
        }
        t->rgb = grey;
        t->tpx = w->tpx; t->tpy = w->tpy; t->clx = w->clx; t->cly = w->cly;
        t->bpp = w->bpp; t->blend = w->blend;
    }
}

/** @brief Re-reads a fixed-address list and appends the widened-view records; a list in a copy is left as copied.
 *
 * A list at a fixed scratch address stays the game's live list until the
 * next draw, and a sub-tick present reads it as it stands then; a list in the
 * primitive buffer's tail is overwritten later in the tick, so that source
 * walks the copy taken at hook entry.
 * @param cpu The guest CPU state, read only.
 * @param desc The descriptor's address.
 * @param cs Receives the listed count when non-NULL and the list is re-read. */
static void shadow_list_refresh(CPUState* cpu, uint32_t desc, shadow_wide_census* cs) {
    if (game_layout.shadow.list_source == SHADOW_LIST_GTE_DATA || s_list_held >= 0) return;
    shadow_list_read(cpu, cpu->read_word(desc + 0x08));
    if (cs) cs->listed = s_list_n;
    shadow_list_append_synth();
}

/** @brief Rebuilds the moby drop-shadow triangles from the drawn-moby list.
 *
 * Empties s_mosh, then stores four fan triangles for each list record
 * shadow_record_admit admits (shadow_fan_emit), except the companion's while
 * the player is hidden (player_hide_active) outside `game_only`. The list is refreshed by
 * shadow_list_refresh. Pushes the decal camera parameters. Reads PSX RAM only.
 * @param cpu The guest CPU state.
 * @param cs Tallies the screen tests for the census; NULL off the canonical
 *        walk.
 * @param game_only 1 walks the game's records alone under the game's own
 *        screen test, the form the GTE comparison needs. */
static void shadow_moby_build(CPUState* cpu, shadow_wide_census* cs, int game_only) {
    s_mosh_n = 0;
    const uint32_t desc = game_layout.shadow.desc;
    if (!desc || !s_list_base) return;
    ngte_view view; ngte_view_build(cpu, &view);   /* one projection view per walk */
    shadow_walk w = { .cs = cs, .game_only = game_only };
    shadow_walk_init(cpu, desc, &view, &w);
    /* Hand the depth-aware decal the camera-reconstruction params so the TEXTURED
     * moby-shadow decal (shadow_decal_tex.frag) can reject wall/steep/raised
     * terrain per pixel. Set here (not only in a player-shadow feeder) so moby
     * shadows are depth-aware even when the player casts no shadow of his own.
     * Identical params to the player feeder's (same camera; ground_up read from
     * the layout), so feeder order is irrelevant. Display-only. */
    shadow_push_decal_params(cpu, &view, w.C, view.ofx, view.ofy, view.H_eff, NULL);
    shadow_list_refresh(cpu, desc, cs);
    const int n_walk = game_only ? s_list_game_n : s_list_n;
    const int hide_player = !game_only && player_hide_active();
    for (int li = 0; li < n_walk; li++) {
        shadow_fan_pts f;
        if (!shadow_record_admit(cpu, &w, &s_list[li], &f)) continue;
        /* --hide-player: the companion casts no drop shadow; the GTE
         * comparison (game_only) keeps the game's whole list. */
        if (!game_only && player_hide_is_sparx_moby(cpu, s_list[li].moby)) {
            player_hide_count(PH_CAT_SPARX_SHADOW, 4, hide_player);
            if (hide_player) continue;
        }
        /* --hide-classes: a listed class casts no drop shadow. */
        if (!game_only && player_hide_class_shadow(cpu, s_list[li].moby)) continue;
        shadow_fan_emit(&w, &f);
    }
}

/* ---- admission calibration (OPENPETE_SHADOW_ADMIT_CAL) -------------------- */
/* One triangle the integer verdict says the game emits: the record it belongs
 * to and its three packed SXY words (anchor, corner k, corner k+1). */
typedef struct { int rec; int32_t p[3]; } shadow_cal_tri;
#define SHADOW_CAL_MAX 1024
static shadow_cal_tri s_cal_tri[SHADOW_CAL_MAX];

/** @brief The float verdict the walk took before the integer evaluation, on the same record.
 *
 * Rejects at a float view-space Z of at least 4096, at a negative NCLIP on the
 * float SXY, or by the game's screen test on the floored float points.
 * @param cpu The guest CPU state, read only.
 * @param w The walk.
 * @param rec The list record.
 * @param slot Receives each triangle's slot from the rounded float SZ.
 * @return SHADOW_ADMIT or a SHADOW_REJ_* code. */
static int shadow_cal_float_verdict(CPUState* cpu, const shadow_walk* w, const shadow_list_rec* rec,
                                    int slot[4]) {
    shadow_entry_pose ep;
    if (!shadow_entry_world(cpu, w->v, rec->moby, game_layout.shadow.moby_pos,
                            game_layout.shadow.moby_ground, &ep)) return SHADOW_REJ_GROUND;
    shadow_fan_pts f;
    shadow_record_project(cpu, w, rec, &ep, &f);
    if (f.trz - 4096.0 >= 0.0) return SHADOW_REJ_Z;
    if (ngte_nclip(f.c[0].sx, f.c[0].sy, f.c[1].sx, f.c[1].sy, f.c[2].sx, f.c[2].sy, NGTE_NCLIP_FLOAT) < 0.0)
        return SHADOW_REJ_NCLIP;
    int32_t c[4];
    for (int k = 0; k < 4; k++) c[k] = packed_sxy(&f.c[k]);
    if (shadow_screen_reject(w->v, packed_sxy(&f.anchor), c, NULL, 1)) return SHADOW_REJ_SCREEN;
    for (int k = 0; k < 4; k++)
        slot[k] = moby_shadow_slot(cpu, rec->moby, (int)shadow_sz16(f.anchor.sz),
                                   (int)shadow_sz16(f.c[k].sz), (int)shadow_sz16(f.c[(k + 1) & 3].sz));
    return SHADOW_ADMIT;
}

/** @brief The number of a verdict's triangles the game inserts: 0 when rejected, else those whose slot is not MOBY_SHADOW_SKIP.
 *
 * @param verdict SHADOW_ADMIT or a SHADOW_REJ_* code.
 * @param slot The four slots.
 * @return The triangle count. */
static int shadow_cal_tris(int verdict, const int slot[4]) {
    if (verdict != SHADOW_ADMIT) return 0;
    int n = 0;
    for (int k = 0; k < 4; k++) n += slot[k] != MOBY_SHADOW_SKIP;
    return n;
}

/** @brief Compares the integer verdict's expected triangles against the game's emitted FT3 prims, in emission order.
 *
 * A triangle matches when its anchor SXY and both corners' SX equal the
 * prim's, and each corner's SY is within one row (the draw's nearer/farther
 * corner nudge moves it). Each mismatching triangle, and each triangle one
 * side has beyond the other's count, counts once and marks its record.
 * @param n The expected triangle count in s_cal_tri.
 * @param bad Receives 1 per mismatching record index; zeroed by the caller.
 * @param nrec The record count bad holds.
 * @param game_tris Receives the game's FT3 count.
 * @return The mismatching triangle count. */
static int shadow_cal_match(int n, unsigned char* bad, int nrec, int* game_tris) {
    int t = 0, mm = 0;
    for (int p = 0; p < s_gte_n; p++) {
        const shadow_prim* pr = &s_gte[p];
        if (!(pr->textured && pr->nv == 3)) continue;
        if (t >= n) { mm++; t++; continue; }
        const int32_t* e = s_cal_tri[t].p;
        int ok = sxy_x(e[0]) == (int)pr->x[0] && (int16_t)((uint32_t)e[0] >> 16) == (int)pr->y[0];
        for (int k = 1; k < 3; k++) {
            const int dy = (int16_t)((uint32_t)e[k] >> 16) - (int)pr->y[k];
            ok &= sxy_x(e[k]) == (int)pr->x[k] && dy >= -1 && dy <= 1;
        }
        if (!ok) { mm++; if (s_cal_tri[t].rec < nrec) bad[s_cal_tri[t].rec] = 1; }
        t++;
    }
    *game_tris = t;
    for (int i = t; i < n; i++) { mm++; if (s_cal_tri[i].rec < nrec) bad[s_cal_tri[i].rec] = 1; }
    return mm;
}

/** @brief The first VBL the [shadow-admit-cal] line prints at, from OPENPETE_SHADOW_ADMIT_CAL; -1 when unset.
 *
 * A harness knob, latched on the first call.
 * @return The VBL, or -1. */
static int shadow_admit_cal_from(void) {
    static int v = -2;
    ENV_INT_LATCH(v, "OPENPETE_SHADOW_ADMIT_CAL", -1);
    return v;
}

/** @brief Prints one canonical walk's admission calibration: the integer and float verdicts of the game's records against the game's emitted prims.
 *
 * Walks the game's own records under the game's own screen test. The
 * integer verdict's triangles are matched against the captured FT3 prims
 * (shadow_cal_match); a record whose float verdict or float triangle count
 * differs from the integer one counts in float_diff, and each such record
 * prints a detail line. Reads PSX RAM only.
 * @param cpu The guest CPU state.
 * @param have_prims 1 when the draw emitted prims and s_gte holds them. */
static void shadow_admit_cal(CPUState* cpu, int have_prims) {
    const uint32_t desc = game_layout.shadow.desc;
    if (!desc || !s_list_base) return;
    ngte_view view; ngte_view_build(cpu, &view);
    shadow_walk w = { .cs = NULL, .game_only = 1 };
    shadow_walk_init(cpu, desc, &view, &w);
    shadow_list_refresh(cpu, desc, NULL);
    if (!have_prims) s_gte_n = 0;
    static unsigned char bad[SHADOW_LIST_MAX];
    memset(bad, 0, sizeof bad);
    int n = 0, admit = 0, float_diff = 0, int_tris = 0, float_tris = 0;
    for (int li = 0; li < s_list_game_n; li++) {
        const shadow_list_rec* rec = &s_list[li];
        if (rec->moby < 0x80000000u || rec->moby >= 0x80800000u) continue;
        const uint32_t sd = cpu->read_word(rec->moby + game_layout.shadow.moby_ground);
        shadow_int_pts ip;
        int fslot[4] = { 0, 0, 0, 0 };
        const int iv = shadow_record_verdict(cpu, &w, rec, sd, &ip);
        const int fv = shadow_cal_float_verdict(cpu, &w, rec, fslot);
        const int it = shadow_cal_tris(iv, ip.slot), ft = shadow_cal_tris(fv, fslot);
        admit += iv == SHADOW_ADMIT; int_tris += it; float_tris += ft;
        for (int k = 0; k < 4 && iv == SHADOW_ADMIT && n < SHADOW_CAL_MAX; k++) {
            if (ip.slot[k] == MOBY_SHADOW_SKIP) continue;
            s_cal_tri[n++] = (shadow_cal_tri){ li, { ip.anchor, ip.c[k], ip.c[(k + 1) & 3] } };
        }
        if (iv == fv && it == ft) continue;
        float_diff++;
        fprintf(stderr, "  [shadow-admit-cal vbl=%u] rec=%d mob=%08X int=%d/%dtri float=%d/%dtri "
                        "trz=%d nclip=%d anchor=(%d,%d)\n",
                openpete_frame, li, rec->moby, iv, it, fv, ft, ip.trz, ip.nclip,
                sxy_x(ip.anchor), (int16_t)((uint32_t)ip.anchor >> 16));
    }
    int game_tris = 0;
    const int tri_mm = shadow_cal_match(n, bad, SHADOW_LIST_MAX, &game_tris);
    int rec_mm = 0;
    for (int i = 0; i < s_list_game_n; i++) rec_mm += bad[i];
    fprintf(stderr, "[shadow-admit-cal vbl=%u] records=%d int_admit=%d int_tris=%d game_tris=%d "
                    "tri_mismatch=%d rec_mismatch=%d float_tris=%d float_rec_diff=%d\n",
            openpete_frame, s_list_game_n, admit, int_tris, game_tris, tri_mm, rec_mm,
            float_tris, float_diff);
}

/* View-Z to native scene depth; see shadow.h. */
float shadow_depth(float viewz, float scale, float bias) {
    return openpete_native_scene_viewz(
        openpete_worldvz_bias(openpete_worldvz_scaled(viewz, scale), bias));
}
/* Both view-Z biases default to 1.0 (true depth); the env vars scale them.
 *  - Player shadow: terrain standing above the fan's ground plane by more than
 *    OPENPETE_SHADOW_WALL_HEIGHT (default 400) is rejected per pixel by the
 *    depth-aware decal (shadow_decal.frag with the world-opaque depth pre-pass,
 *    native_render.c); everything else keeps the fan, so bumps and slopes
 *    drape. The fan feeds true depth and its pipe has the depth test off.
 *  - Moby shadows: small, distant coplanar decals; the tie-break is the
 *    hardware slope-scaled depth bias on the subtractive pipelines
 *    (native_render.c shadow_hw_bias, OPENPETE_SHADOW_HW_BIAS). A
 *    multiplicative view-Z bias grows with distance, the wrong dimension for a
 *    tie-break: a shadow ten times farther would get a ten times larger
 *    world-space offset. */
/* shadow_bias_spyro and shadow_bias_moby are documented in shadow.h. */
float shadow_bias_spyro(void) {
    static float b = -1.0f;
    if (b < 0.0f) { const char* e = getenv("OPENPETE_SHADOW_BIAS_SPYRO"); b = (e && *e) ? (float)atof(e) : 1.0f; }
    return b;
}
int shadow_player_feed_enabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_SHADOW_PLAYER"); v = !(e && *e && *e == '0'); }
    return v;
}

float shadow_bias_moby(void) {
    static float b = -1.0f;
    if (b < 0.0f) { const char* e = getenv("OPENPETE_SHADOW_BIAS_MOBY"); b = (e && *e) ? (float)atof(e) : 1.0f; }
    return b;
}

/* ---- the player fan's footprint decal ------------------------------------- */

/* A footprint receiver lies between the fan's own surface less this margin and
 * the caster height. The margin holds the pre-pass reconstruction error (a few
 * world units) on a receiver that is the fan's own ground; the prism hull the
 * feed draws spans the same interval, so no receiver the rule admits falls
 * outside the drawn cover. */
#define SHADOW_FOOT_BELOW 16.0
/* A hull vertex nearer the eye plane than this view-Z, or projecting farther
 * off screen than this bound, replaces the hull by a full-screen cover: the
 * fragment stage decides every pixel from the fan data, so a larger cover
 * costs fragments and never changes the image. */
#define SHADOW_HULL_NEAR  16.0
#define SHADOW_HULL_BOUND 4096.0

/** @brief Reports whether the footprint term is on; OPENPETE_SHADOW_FOOTPRINT=0 turns it off.
 *
 * Read on every call. Off, the decal keeps the fan's own surface under the
 * depth rule alone: the negative control of the term's count.
 * @return 1 when on, else 0. */
static int shadow_footprint_on(void) {
    const char* e = getenv("OPENPETE_SHADOW_FOOTPRINT");
    return !(e && *e == '0');
}

/* The caster's world height; see shadow.h. */
double shadow_caster_up(CPUState* cpu, double anchor_up) {
    const uint32_t pl = game_layout.player.base;
    if (!pl) return anchor_up;
    return (double)(int32_t)cpu->read_word(pl + game_layout.player.pos_off + 8u);
}

/** @brief Camera-relative world position (x, y, up) of a projected point.
 *
 * Uses the same 4096² · C⁻¹ rows the decal reconstructs the pre-pass terrain
 * with, so both sides of the footprint test share one coordinate system. The
 * GTE input vector V maps to (V2, -V0, -V1).
 * @param inv The rows from shadow_cam_inverse.
 * @param p The projected point; its MAC1, MAC2 and sz are read.
 * @param w Receives (x, y, up). */
static void fan_wrel(const double inv[3][3], const shadow_pt* p, double w[3]) {
    const double m[3] = { p->m1, p->m2, p->sz };
    double V[3];
    for (int j = 0; j < 3; j++)
        V[j] = (inv[j][0] * m[0] + inv[j][1] * m[1] + inv[j][2] * m[2]) / 4096.0;
    w[0] = V[2]; w[1] = -V[0]; w[2] = -V[1];
}

typedef struct { double x, y; } fan_xy;

/** @brief The cross product of o->a and o->b.
 *
 * @param o The origin.
 * @param a The first point.
 * @param b The second point.
 * @return The signed twice-area; positive when o, a, b turn counter-clockwise
 *         in a y-up frame. */
static double hull_cross(fan_xy o, fan_xy a, fan_xy b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}
/** @brief The qsort comparator ordering points by x, then y.
 *
 * @param pa The first fan_xy.
 * @param pb The second fan_xy.
 * @return -1, 0 or 1. */
static int hull_cmp(const void* pa, const void* pb) {
    const fan_xy* a = (const fan_xy*)pa; const fan_xy* b = (const fan_xy*)pb;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    return 0;
}
/** @brief Andrew's monotone chain: the convex hull of p[0..n) into h.
 *
 * Sorts `p` in place by x, then y. The lower chain is built first, then the
 * upper; collinear points are dropped and the closing duplicate is not
 * counted.
 * @param p The points; reordered.
 * @param n The point count.
 * @param h Receives the hull; room for 2n points.
 * @return The hull's vertex count. */
static int convex_hull(fan_xy* p, int n, fan_xy* h) {
    qsort(p, (size_t)n, sizeof *p, hull_cmp);
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && hull_cross(h[k - 2], h[k - 1], p[i]) <= 0.0) k--;
        h[k++] = p[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && hull_cross(h[k - 2], h[k - 1], p[i]) <= 0.0) k--;
        h[k++] = p[i];
    }
    return k > 1 ? k - 1 : k;
}

/** @brief The screen position of a view-space point by the GTE's divide without its saturation.
 *
 * @param f The fan; supplies the projection registers ofx, ofy and H.
 * @param m The point's MAC1..MAC3.
 * @param o Receives the position; unwritten when the view-Z is below 16.
 * @return 1 when the point can stand as a hull vertex; 0 when its view-Z is
 *         below 16 or either coordinate reaches 4096 in magnitude. */
static int hull_project(const shadow_fan* f, const double m[3], fan_xy* o) {
    if (m[2] < SHADOW_HULL_NEAR) return 0;
    o->x = f->ofx + f->H * m[0] / m[2];
    o->y = f->ofy + f->H * m[1] / m[2];
    return fabs(o->x) < SHADOW_HULL_BOUND && fabs(o->y) < SHADOW_HULL_BOUND;
}

/** @brief Fills the footprint block's point i and the three hull candidates it contributes.
 *
 * Point 0 is the anchor and 1..16 the ring. The block receives the screen
 * position (floored under vertex snap), the biased and unbiased depths, and
 * the camera-relative world position. `out` receives the screen point and
 * the two points vertically over it: the prism floor, 16 world units under
 * the fan, and its roof at the caster height.
 * @param f The fan.
 * @param inv The rows from shadow_cam_inverse.
 * @param w0 The anchor's camera-relative world position.
 * @param caster The caster's world height.
 * @param i The point index.
 * @param fp The footprint block.
 * @param out Receives the screen point, the floor and the roof.
 * @return 1 when both hull points project usably, else 0. */
static int fan_point(const shadow_fan* f, const double inv[3][3], const double w0[3],
                     double caster, int i, openpete_shadow_footprint* fp, fan_xy out[3]) {
    const shadow_pt* p = i ? &f->ring[i - 1] : &f->anchor;
    const int snap = openpete_native_vertex_snap_active();
    const float sx = snap ? floorf(p->sx) : p->sx, sy = snap ? floorf(p->sy) : p->sy;
    fp->scr[i][0] = sx; fp->scr[i][1] = sy;
    fp->scr[i][2] = shadow_depth(p->sz, 1.0f, shadow_bias_spyro());
    fp->scr[i][3] = shadow_depth(p->sz, 1.0f, 1.0f);
    double w[3]; fan_wrel(inv, p, w);
    for (int c = 0; c < 3; c++) fp->wrel[i][c] = (float)w[c];
    out[0].x = sx; out[0].y = sy;
    const double up = f->anchor_up + (w[2] - w0[2]);
    const double dz[2] = { -SHADOW_FOOT_BELOW, caster - up };
    int ok = 1;
    for (int e = 0; e < 2; e++) {
        /* a world rise dz lowers the GTE input's second component by dz */
        const double m[3] = { p->m1 - f->C[0][1] * dz[e] / 4096.0,
                              p->m2 - f->C[1][1] * dz[e] / 4096.0,
                              p->sz - f->C[2][1] * dz[e] / 4096.0 };
        ok &= hull_project(f, m, &out[1 + e]);
    }
    return ok;
}

/** @brief Feeds the linked fan triangles as the guest draws them.
 *
 * Each triangle k whose bit is set in tri_mask is (anchor, ring k,
 * ring k+1), gouraud from anchor_rgb (0x608080 from both player feeders) at
 * the anchor to 0 at the rim, with vertex alpha 1 marking it as the fan. The
 * depth is biased by shadow_bias_spyro; pvz is unbiased.
 * @param f The fan. */
static void fan_feed_tris(const shadow_fan* f) OP_REQUIRES_SHARED(sim) {
    const float bias = shadow_bias_spyro();
    for (int k = 0; k < SHADOW_FAN_RING; k++) {
        if (!(f->tri_mask & (1u << k))) continue;
        const shadow_pt* p[3] = { &f->anchor, &f->ring[k], &f->ring[(k + 1) & (SHADOW_FAN_RING - 1)] };
        openpete_scene_tri st = {0};
        for (int i = 0; i < 3; i++) {
            st.v[i].sx = p[i]->sx; st.v[i].sy = p[i]->sy;
            st.v[i].z  = shadow_depth(p[i]->sz, 1.0f, bias);   /* already full-world MAC3 */
            st.v[i].pvz = shadow_depth(p[i]->sz, 1.0f, 1.0f);  /* true (unbiased) view-Z */
            st.v[i].rgba = (i == 0 ? (f->anchor_rgb & 0xFFFFFFu) : 0u) | 0xFF000000u;
        }
        st.textured = 0;
        st.blend = 3;                                  /* untextured subtract (ABR mode 2) */
        st.koff = shadow_slot_koff(f->slot[k]);        /* the triangle's ordering-table slot; unread */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
}

void shadow_fan_feed(CPUState* cpu, const shadow_fan* f) OP_REQUIRES_SHARED(sim) {
    (void)cpu;
    double inv[3][3]; shadow_cam_inverse(f->C, inv);
    const double caster = f->caster_up;
    openpete_shadow_footprint fp; memset(&fp, 0, sizeof fp);
    fp.caster_up = (float)caster;
    fp.footprint = shadow_footprint_on() ? 1.0f : 0.0f;
    fp.tri_mask  = (float)(f->tri_mask & 0xFFFFu);
    fp.below     = (float)SHADOW_FOOT_BELOW;
    for (int c = 0; c < 3; c++) fp.anchor_rgb[c] = (float)((f->anchor_rgb >> (8 * c)) & 0xFFu) / 255.0f;
    fp.anchor_rgb[3] = 1.0f;

    enum { NPT = 3 * (SHADOW_FAN_RING + 1) };
    fan_xy pts[NPT], hull[2 * NPT];
    double w0[3]; fan_wrel(inv, &f->anchor, w0);
    int cover = 1;
    for (int i = 0; i <= SHADOW_FAN_RING; i++)
        cover &= fan_point(f, inv, w0, caster, i, &fp, &pts[3 * i]);
    openpete_native_set_shadow_footprint(&fp);

    int h;
    if (cover) {
        h = convex_hull(pts, NPT, hull);
    } else {
        const double b = SHADOW_HULL_BOUND;
        const fan_xy screen[4] = { { -b, -b }, { b, -b }, { b, b }, { -b, b } };
        memcpy(hull, screen, sizeof screen);
        h = 4;
    }
    fan_feed_tris(f);
    /* The fragment stage colours every hull pixel from the block; the vertex
     * colour is black with alpha 0, so the depth-tested fallback pipe (no
     * pre-pass) subtracts nothing and the decal tells hull from fan. The
     * depth test is off on the decal pipe, so the vertex depth only has to be
     * a valid view-Z. */
    float vz = shadow_depth(f->anchor.sz, 1.0f, 1.0f);
    if (!(vz >= 1.0f)) vz = 1.0f;
    for (int i = 1; i + 1 < h; i++) {
        const fan_xy* tv[3] = { &hull[0], &hull[i], &hull[i + 1] };
        openpete_scene_tri st = {0};
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = (float)tv[k]->x; st.v[k].sy = (float)tv[k]->y;
            st.v[k].z = vz; st.v[k].pvz = vz;
            st.v[k].rgba = 0u;
        }
        st.textured = 0;
        st.blend = 3;                                  /* untextured subtract (ABR mode 2) */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
}

/** @brief Walks the drawn-moby list and feeds the native moby shadows.
 *
 * Each shadow is four textured FT3 fan triangles, blended by the tpage's ABR,
 * under the moby-shadow scene tag. A triangle the game does not insert
 * (MOBY_SHADOW_SKIP) is not fed. Depth is the world/4 MAC3 scaled by 4,
 * biased by shadow_bias_moby; pvz is unbiased. Nothing is fed, and the scene
 * frame is not begun, when the walk yields no triangle.
 * @param cpu The guest CPU state.
 * @param cs Tallies the screen tests for the census; NULL off the canonical
 *        walk. */
static void shadow_moby_feed_native(CPUState* cpu, shadow_wide_census* cs) OP_REQUIRES(sim) {
    shadow_moby_build(cpu, cs, 0);
    if (s_mosh_n <= 0) return;
    render_material_level_push(cpu);
    openpete_native_scene_begin_frame(openpete_frame);
    openpete_native_scene_set_tag(OPENPETE_TAG_SHADOW(OPENPETE_TAG_SHADOW_MOBY, 0));
    for (int i = 0; i < s_mosh_n; i++) {
        moshtri* m = &s_mosh[i];
        if (m->slot == MOBY_SHADOW_SKIP) continue;
        openpete_scene_tri st = {0};
        float bias = shadow_bias_moby();
        for (int k = 0; k < 3; k++) {
            st.v[k].sx = m->sx[k]; st.v[k].sy = m->sy[k];
            st.v[k].z  = shadow_depth(m->sz[k], 4.0f, bias);  /* world/4 MAC3 -> full-world */
            st.v[k].pvz = shadow_depth(m->sz[k], 4.0f, 1.0f); /* true (unbiased) view-Z */
            st.v[k].u = (float)m->u[k]; st.v[k].v = (float)m->v[k];
            st.v[k].rgba = (m->rgb & 0xFFFFFFu) | 0xFF000000u;
        }
        st.textured = 1; st.tpage_x = m->tpx; st.tpage_y = m->tpy;
        st.clut_x = m->clx; st.clut_y = m->cly; st.bpp = m->bpp; st.blend = m->blend;
        st.koff = shadow_slot_koff(m->slot);           /* the fan triangle's ordering-table slot; the decal does not read it */
        openpete_native_scene_add_tri(&st, OPENPETE_SCENE_SHADOW, 0);
    }
    openpete_native_scene_set_tag(0);
}

/** @brief Compares the native moby-shadow triangles against the captured FT3 prims, the .s int16 SXY reference.
 *
 * Both produce four triangles per moby in the same fan order, so they compare
 * element-wise over the shorter list. The native side walks the game's own
 * records under the game's own screen test, so a widened view compares the
 * same set. One stderr line reports the mean and
 * worst SXY distance, the floored-X and floored-Y agreement counts, and the
 * colour differences. Reports VACUOUS when either side has no triangle: a run
 * with nothing to compare is not a pass. OPENPETE_SHADOW_VERIFY_DETAIL prints
 * the first eight triangle pairs once.
 * @param cpu The guest CPU state. */
static void shadow_moby_compare(CPUState* cpu) {
    shadow_moby_build(cpu, NULL, 1);
    int ft3 = 0;
    for (int p = 0; p < s_gte_n; p++) if (s_gte[p].textured && s_gte[p].nv == 3) ft3++;
    if (ft3 == 0 || s_mosh_n == 0) {
        fprintf(stderr, "[shadow-moby f=%u] VACUOUS native=%d FT3=%d (nothing compared)\n",
                openpete_frame, s_mosh_n, ft3);
        return;
    }
    int n = s_mosh_n < ft3 ? s_mosh_n : ft3;
    double worst = 0, sum = 0; int over2 = 0, nv = 0, qi = 0;
    int cmax = 0, cbad = 0;
    /* The captured SXY is the GTE's truncated 16.16 output, so a float walk
     * that agrees exactly still reads ~0.5 px off per axis. `floor_x` counts
     * vertices whose floored X is the captured X (the walk agrees to the
     * truncation), and `floor_y1` those whose floored Y is within one row of
     * the captured Y - the row the .s's nearest/farthest corner nudge moves. */
    int floor_x = 0, floor_y1 = 0;
    for (int p = 0; p < s_gte_n && qi < n; p++) {
        shadow_prim* pr = &s_gte[p];
        if (!(pr->textured && pr->nv == 3)) continue;
        moshtri* m = &s_mosh[qi];
        double tw = 0;
        for (int k = 0; k < 3; k++) {
            double dx = m->sx[k]-pr->x[k], dy = m->sy[k]-pr->y[k];
            double d = (dx<0?-dx:dx) + (dy<0?-dy:dy);
            sum += d; nv++; if (d > tw) tw = d;
            double fy = floor(m->sy[k]) - pr->y[k];
            if (floor(m->sx[k]) == pr->x[k]) floor_x++;
            if (fy >= -1.0 && fy <= 1.0) floor_y1++;
        }
        for (int ch = 0; ch < 3; ch++) {
            int a = (int)((m->rgb >> (ch*8)) & 0xFF);
            int b = (int)((pr->rgb[0] >> (ch*8)) & 0xFF);
            int d = a > b ? a - b : b - a;
            if (d > cmax) cmax = d;
        }
        if (m->rgb != (pr->rgb[0] & 0xFFFFFFu)) cbad++;
        if (tw > worst) worst = tw;
        if (tw > 2.0) over2++;
        qi++;
    }
    fprintf(stderr, "[shadow-moby f=%u] native=%d FT3=%d cmp=%d mean|dSXY|=%.2f worst=%.1fpx "
                    "over2px=%d/%d floorX=%d/%d floorY+-1=%d/%d dRGBmax=%d rgbMismatch=%d\n",
            openpete_frame, s_mosh_n, ft3, n, nv?sum/nv:0, worst, over2, n,
            floor_x, nv, floor_y1, nv, cmax, cbad);
    if (getenv("OPENPETE_SHADOW_VERIFY_DETAIL")) {
        static int once = 0;
        if (!once) {
            once = 1;
            int q = 0;
            for (int p = 0; p < s_gte_n && q < n && q < 8; p++) {
                shadow_prim* pr = &s_gte[p];
                if (!(pr->textured && pr->nv == 3)) continue;
                moshtri* m = &s_mosh[q];
                fprintf(stderr, "  tri[%d]", q);
                for (int k = 0; k < 3; k++)
                    fprintf(stderr, "  v%d nat=(%.2f,%.2f) gte=(%.0f,%.0f)",
                            k, m->sx[k], m->sy[k], pr->x[k], pr->y[k]);
                fprintf(stderr, "\n");
                q++;
            }
        }
    }
}

/* ---- gating --------------------------------------------------------------- */
int shadow_verify_enabled(void) {
    OP_ROLE_ASSERT(sim);
    static int v = -1;
    ENV_FLAG_LATCH(v, "OPENPETE_SHADOW_VERIFY");
    return v;
}
/* Documented at its declaration in shadow.h. */
unsigned shadow_verify_frame(void) {
    static unsigned f = 0xFFFFFFFFu;
    if (f == 0xFFFFFFFFu) { const char* e = getenv("OPENPETE_SHADOW_VERIFY_FRAME"); f = e ? (unsigned)strtoul(e, NULL, 0) : 0; }
    return f;
}

/** @brief The bracket's pre-draw step: latches the drawn-moby list before the draw overwrites the GTE registers.
 *
 * @param cpu The guest CPU state.
 * @param feed Unused.
 * @param verify Unused. */
static void shadow_pre(CPUState* cpu, int feed, int verify) {
    OP_ROLE_ASSERT(sim);
    (void)feed; (void)verify;
    shadow_latch_list_base(cpu);
}

/** @brief The first VBL the [shadow-wide] census prints at, from OPENPETE_SHADOW_WIDE_CENSUS; -1 when unset.
 *
 * A harness knob, latched on the first call.
 * @return The VBL, or -1. */
static int shadow_wide_census_from(void) {
    static int v = -2;
    ENV_INT_LATCH(v, "OPENPETE_SHADOW_WIDE_CENSUS", -1);
    return v;
}

/** @brief Prints one canonical walk's [shadow-wide] census line to stderr.
 *
 * The verdict is VACUOUS in a widened view whose walk admitted no shadow the
 * game's test rejected and built no widened-view record, ENGAGED otherwise,
 * and 4:3 in a view no wider than the game's frame.
 * @param cpu The guest CPU state, read only.
 * @param cs The walk's tallies. */
static void shadow_wide_census_print(CPUState* cpu, const shadow_wide_census* cs) {
    float xlo = 0.0f, xhi = 0.0f;
    const float F = openpete_native_widescreen_factor();
    openpete_native_widescreen_x_bounds(&xlo, &xhi);
    const char* verdict = !ngte_view_extended(cpu) ? "4:3"
                        : (cs->wide_admit + cs->synth > 0 ? "ENGAGED" : "VACUOUS");
    fprintf(stderr, "[shadow-wide vbl=%u] F=%.4f window=[%.1f,%.1f] listed=%d game_admit=%d "
                    "wide_admit=%d synth=%d verdict=%s\n",
            openpete_frame, F, xlo, xhi, cs->listed, cs->game_admit, cs->wide_admit, cs->synth, verdict);
}

static hook_bracket_state s_shadow_hb;
static const hook_bracket_desc k_shadow_bracket = {
    .path = RP_MOBY_SHADOW, .prof_tag = "shadow.srend",
    .verify_enabled = shadow_verify_enabled, .pre = shadow_pre,
    .st = &s_shadow_hb,
};

/** @brief Moby drop-shadow draw hook.
 *
 * Runs the game's draw once through the hook bracket, which records its
 * polybuf interval under RP_MOBY_SHADOW. When the draw emitted prims, they are
 * captured for the feed or the verify harness. The widened-view records
 * (shadow_synth_update) join the list; when the list is non-empty the native
 * shadows are fed from gamestate while RP_MOBY_SHADOW is selected, and the
 * path is marked drawn when the feed emitted triangles the draw did not.
 * Under OPENPETE_SHADOW_VERIFY the first eligible tick dumps once and the
 * first 60 compare; OPENPETE_SHADOW_ADMIT_CAL prints the [shadow-admit-cal]
 * calibration and OPENPETE_SHADOW_WIDE_CENSUS the [shadow-wide]
 * census from the VBL it names.
 * @param cpu  guest CPU state
 * @param addr the hooked draw's address
 * @return 1 after running the draw, 0 when the bracket declines and the
 *         ordinary dispatch runs it */
static int on_moby_shadows(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    uint32_t startp, endp; int feed, verify;
    if (!hook_bracket_run(cpu, addr, &k_shadow_bracket, &startp, &endp, &feed, &verify))
        return 0;

    if ((feed || verify) && endp > startp) shadow_capture_prims(cpu, startp, endp);

    shadow_wide_census cs = { .listed = s_list_game_n };
    shadow_synth_update(cpu);
    cs.synth = s_synth_n;
    /* A list in a copy carries the widened-view records from here; a fixed
     * address list is re-read, and they appended, at each walk. */
    if (game_layout.shadow.list_source == SHADOW_LIST_GTE_DATA) shadow_list_append_synth();
    const int listed = s_list_game_n + s_synth_n;

    /* The feed runs on the list the draw reads, not on the prims it emitted:
     * a shadow only the widened view admits has no game prim. */
    if (feed && listed > 0 && render_path_enabled(RP_MOBY_SHADOW)) {
        shadow_moby_feed_native(cpu, &cs);           /* native gamestate walk */
        if (s_mosh_n > 0 && endp <= startp) render_path_mark_drawn(RP_MOBY_SHADOW);
    }
    const int from = shadow_wide_census_from();
    if (from >= 0 && openpete_frame >= (unsigned)from) shadow_wide_census_print(cpu, &cs);
    const int cal = shadow_admit_cal_from();
    if (cal >= 0 && openpete_frame >= (unsigned)cal && feed) shadow_admit_cal(cpu, endp > startp);

    if (verify && endp > startp && openpete_frame >= shadow_verify_frame()) {
        static int s_dumped = 0, s_reports = 0;
        if (!s_dumped) { shadow_moby_dump(cpu); s_dumped = 1; }
        if (s_reports < 60) { shadow_moby_compare(cpu); s_reports++; }
    }
    return hook_bracket_end(&k_shadow_bracket);
}

/* Sub-tick moby drop-shadow re-extract; see shadow.h. */
void shadow_reextract(CPUState* cpu) OP_REQUIRES(sim) {
    if (!hook_reextract_ready_held(RP_MOBY_SHADOW, s_list_held)) return;
    shadow_moby_feed_native(cpu, NULL);
}

int shadow_replay_prev_list(int on) {
    if (!on) {
        if (s_list_held == CUT_HELD_PREV) shadow_list_swap();
        s_list_held = -1;
        return 0;
    }
    if (s_list_held >= 0) return s_list_held != CUT_HELD_EMPTY;
    cut_held_pick_t pick = cut_held_set_pick(s_list_epoch, s_list_prev.epoch);
    /* A copy with no base is a list the tick did not build. */
    if ((pick == CUT_HELD_PREV && !s_list_prev.base) || (pick == CUT_HELD_CURR && !s_list_base))
        pick = CUT_HELD_EMPTY;
    if (pick == CUT_HELD_PREV) shadow_list_swap();
    s_list_held = (int)pick;
    cut_census_held_set("shadow", pick, s_list_epoch, pick == CUT_HELD_EMPTY ? 0 : s_list_n);
    return pick != CUT_HELD_EMPTY;
}

/* The hook address is the layout's moby-shadow draw site, bound at boot: a
 * static initializer cannot read another TU's const. */
static feature_dispatch_entry_t shadow_dispatch[] = {
    { 0u, on_moby_shadows, 0u },
    { 0u, NULL, 0u },
};

/** @brief Binds the dispatch entry to the layout's moby-shadow draw address.
 *
 * @param opts Unused. */
static void shadow_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    shadow_dispatch[0].addr = game_layout.shadow.hook;
}

const feature_module_t shadow_module = {
    .name           = "shadow_native",
    .apply_options  = shadow_apply_options,
    .dispatch_table = shadow_dispatch,
};
