/* games/common/renderer/moby_game_spyro23.c - the per-game half of the shared
 * moby feeder for both games whose moby draw is the same three handwritten
 * routines: the vertex-stream and face-stream decoders, the cull-record
 * reader, the model-kind test, the keyframe patch list and the simple models'
 * own pass.
 *
 * The walk itself is games/common/renderer/moby.c; the contract is
 * renderer/moby_game.h. Every claim below carries the asm/mobydraw.s line it
 * was read from: the draw is three handwritten routines with no decompiled C.
 * The two games run them instruction for instruction apart from the special
 * material's mode 0, so the line numbers are one disassembly's and every
 * address, plus the one behavioural switch, comes from the per-game
 * moby_game_s23 block (renderer/moby_game_spyro23.h).
 *
 * Three shapes differ from Spyro 1 and drive everything here:
 *   - the cull writes the FINAL view-space translation (it applies the
 *     per-moby size rescale and drops the records that rescale pushes past
 *     the far limit itself, mobydraw.s:546-551,572-574), so the draw loads
 *     the record's translation straight into the GTE (mobydraw.s:898-900);
 *   - the cull copies each keyframe's leading stream word into the record
 *     rather than a pointer to it (mobydraw.s:532-535);
 *   - the vertex stream accumulates onto a per-model static vertex array and
 *     has a third codec form, a byte indexing a 32-bit adjustment table.
 */

#include "renderer/moby_game.h"
#include "renderer/moby_game_spyro23.h"   /* the per-game constants and the shared decoders */
#include "renderer/moby.h"            /* the simple models' shared emit */
#include "renderer/native_gte.h"       /* ngte_read_matrix, ngte_mac, ngte_mat_mul3, ngte_dpcs_ch */
#include "game_layout.h"
#include "no_lod.h"                    /* openpete_no_lod - the extended-LOD clause */
#include "openpete_recomp_module.h"    /* the game's own static draw */
#include "renderer/interp_regions_gen.h"  /* game_interp_moby_stash_begin/_put */
#include "openpete_psx.h"               /* openpete_psx_to_host_span */
#include "openpete_fiber.h"             /* openpete_frame, openpete_feed_key_now */
#include "openpete_workpool.h"          /* OPENPETE_WORKER_ALIGN: the per-worker scratch */
#include "spyro_overlay_table.h"       /* the resident overlay, for the cutscene sampler rule */
#include "renderer/orchestrator.h"     /* the canonical-render epoch */
#include "renderer/cut_guard.h"        /* cut_census_on */
#include "renderer/credits_key.h"      /* game_credits_letter_key */
#include "openpete_pace_log.h"         /* OP_PACE_SPLIT_SCOPE: the simple pass's host split */
#include "openpete_profile.h"          /* OP_PROF_ACCUM_SCOPE: the simple pass's cost split */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* The one-byte vertex form indexes a 32-bit adjustment table
 * (mobydraw.s:914), moby_game_s23.vertex_adjust_table. The draw XORs the
 * table pointer's sign bit with record word-0 bit 0 (mobydraw.s:913-915),
 * which moves it between KSEG0 and KUSEG: the same physical words either way,
 * and the sign only selects whether the draw also stores per-vertex clip
 * flags (mobydraw.s:1094). The decode reads the words, so it reads one
 * address. */

/* AnimationHeader fields the stream decode needs and game_layout.moby does not
 * name, because only this codec has them: the frame-stream data base the two
 * halfword offsets in a keyframe word are relative to (mobydraw.s:967) and the
 * per-model static vertex array every form accumulates onto (mobydraw.s:975).
 */
#define ANIM_DATA_BASE   0x20u
#define ANIM_STATIC_VERT 0x0Cu

/** @brief Opens one keyframe's vertex stream: the block, adjust and static-vertex cursors and the first item's form.
 *
 * Three forms, chained: bit 0 of the item just consumed picks the next one,
 * and the keyframe word's bit 25 picks the first (mobydraw.s:968,986-988).
 * Bit 0 clear takes the ADJUST form - one byte from the adjust stream whose
 * (byte >> 1) & 0x7F indexes the 32-bit table; the table word is ADDED to this
 * vertex's static word and the sum unpacked (mobydraw.s:1011-1024). Bit 0 set
 * takes a halfword from the block stream, whose own bit 1 picks between the
 * 7/7/5 DELTA form, which accumulates the delta AND the static word
 * (mobydraw.s:1044-1057), and the two-halfword LITERAL form, which ASSIGNS and
 * so resets the accumulator (mobydraw.s:1073-1086). Every form consumes one
 * static word (mobydraw.s:1010).
 *
 * The unpack of a packed word is Z += w >> 21, X -= (w << 11) >> 21,
 * Y -= (w << 22) >> 21 - an 11-bit Z, an 11-bit X and a 10-bit Y at twice its
 * stored scale (mobydraw.s:1019-1024).
 *
 * vstream state: litp = the adjust byte cursor, shortp = the block halfword
 * cursor, scratch_u[0] = the static vertex cursor, flag = which form is next.
 * @param cpu The guest CPU state, read only.
 * @param s Receives the stream state, zeroed first.
 * @param header The AnimationHeader; +0x20 is the data base, +0x0C the static vertex array.
 * @param frame The keyframe word: bits 0-15 the block offset in halfwords,
 *        bits 16-24 the adjust-stream offset from the block in halfwords, bit 25
 *        the first item's form.
 * @param s5 The per-model shift, stored unused: this codec has none.
 */
static void s23_vstream_init(CPUState* cpu, moby_vstream_t* s,
                            uint32_t header, uint32_t frame, int s5) {
    memset(s, 0, sizeof *s);
    uint32_t data = cpu->read_word(header + ANIM_DATA_BASE);
    uint32_t block = data + ((frame & 0xFFFFu) << 1);
    s->shortp = block;
    s->litp   = block + ((frame >> 15) & 0x3FEu);
    s->scratch_u[0] = cpu->read_word(header + ANIM_STATIC_VERT);
    s->flag = (int)((frame >> 25) & 1u);   /* the first item's form */
    s->s5 = s5;                            /* no per-model shift in this codec */
}

void s23_vstream_next_lit(CPUState* cpu, moby_vstream_t* s, int first, int y_lit_halved) {
    (void)first;   /* the keyframe word already said which form opens */
    uint32_t sw = cpu->read_word(s->scratch_u[0]);
    s->scratch_u[0] += 4u;
    if (s->flag == 0) {
        uint32_t b = cpu->read_byte(s->litp); s->litp += 1u;
        uint32_t tw = cpu->read_word(moby_game_s23.vertex_adjust_table + (((b << 1) & 0x1FCu)));
        uint32_t w = tw + sw;
        s->VZ += (int32_t)w >> 21;
        s->VX -= (int32_t)(w << 11) >> 21;
        s->VY -= (int32_t)(w << 22) >> 21;
        s->flag = (int)(b & 1u);
    } else {
        int32_t h = (int16_t)cpu->read_half(s->shortp); s->shortp += 2u;
        s->flag = (int)((uint32_t)h & 1u);
        if (h & 2) {
            int32_t h2 = (int16_t)cpu->read_half(s->shortp); s->shortp += 2u;
            /* The literal Y field is where the two draws' codecs part. The
             * moby draw clears the two form bits and sign-extends bits 2-11 as
             * a 12-bit value (mobydraw.s:1076-1078); the player draw keeps
             * them and arithmetic-shifts the sign-extended low 12 bits right
             * by one, which is the 11-bit field at bits 1-11 with the form bit
             * folded into its LSB (spyrodraw.s:498-499,659-660,682-683). */
            s->VY = y_lit_halved ? ((int32_t)((uint32_t)h << 20) >> 21)
                                 : ((int32_t)(((uint32_t)h & 0xFFCu) << 20) >> 20);
            s->VX = (int32_t)((uint32_t)((int32_t)((uint32_t)h2 << 26) >> 21)
                              | (((uint32_t)h & 0xF000u) >> 11));      /* bits 0-5 x32, bits 12-15 */
            s->VZ = h2 >> 5;
        } else {
            s->VZ += h >> 9;                                     /* bits 9-15 */
            s->VX -= (int32_t)((uint32_t)h << 21) >> 25;         /* bits 4-10 */
            s->VY -= ((int32_t)((uint32_t)h << 26) >> 27) << 2;  /* bits 1-5, x4 */
            s->VZ += (int32_t)sw >> 21;
            s->VX -= (int32_t)(sw << 11) >> 21;
            s->VY -= (int32_t)(sw << 22) >> 21;
        }
    }
}

void s23_vstream_next(CPUState* cpu, moby_vstream_t* s, int first) {
    s23_vstream_next_lit(cpu, s, first, 0);
}

/* The face-stream decode; the record layout is documented at the declaration. */
int s23_decode_faces_stride(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                           moby_wface_t* out, int cap, moby_face_stats_t* st,
                           uint32_t fixed_stride, int all_textured) {
    uint32_t end = fb + 4u + bytecount;
    uint32_t fp  = fb + 4u;
    int n = 0, raw = 0, guard = 0;
    while (fp + 8u <= end && guard++ < 8192) {
        uint32_t w0 = cpu->read_word(fp);
        uint32_t w1 = cpu->read_word(fp + 4u);
        raw++;
        int sign = ((int32_t)w1) < 0;
        int textured = all_textured ? 1 : sign;
        uint32_t adv = fixed_stride ? fixed_stride : (textured ? 0x14u : 0x08u);
        unsigned i0 = (w0 >> 24) & 0xFFu, i1 = (w0 >> 16) & 0xFFu;
        unsigned i2 = (w0 >>  8) & 0xFFu, i3 =  w0        & 0xFFu;
        int tri = (i2 == i3);
        if (n < cap) {
            moby_wface_t* F = &out[n];
            memset(F, 0, sizeof *F);
            F->i0 = (uint16_t)i0; F->i1 = (uint16_t)i1;
            F->i2 = (uint16_t)i2; F->i3 = (uint16_t)i3;
            F->c0 = (uint16_t)((w1 >> 22) & 0x1FCu);
            F->c1 = (uint16_t)((w1 >> 15) & 0x1FCu);
            F->c2 = (uint16_t)((w1 >>  8) & 0x1FCu);
            F->c3 = (uint16_t)(tri ? F->c2 : ((w1 >> 1) & 0x1FCu));
            F->fresh    = (uint8_t)(tri ? 0 : 1);
            F->bit0     = (uint8_t)(tri ? ((w1 >> 3) & 1u) : 0u);
            F->textured = (uint8_t)(textured ? 1 : 0);
            F->untested = (uint8_t)(sign ? 1 : 0);
            /* Bits 4-5 of a triangle's flag field arm the special-material
             * dispatch (mobydraw.s:1707,1710): bit 4 takes a second mechanism
             * this decode does not carry, bit 5 alone takes the three
             * treatments the record's material word selects between. The same
             * word's bits 7-30 are then a signed face normal instead of the
             * three colour indices (mobydraw.s:2552-2570). */
            F->hl = (uint8_t)(tri ? ((w1 & 0x10u) ? 2u : (w1 & 0x20u) ? 1u : 0u) : 0u);
            F->n1 = (int8_t)((w1 >> 15) & 0xFFu);
            F->n2 = (int8_t)((w1 >>  7) & 0xFFu);
            F->n3 = (int8_t)((w1 >> 23) & 0xFFu);
            F->blendbit = 0; F->abr = 0; F->bpp = 16;
            /* Bits 0-2, signed: the depth key's per-face ordering bias
             * (mobydraw.s:1712-1713, spyrodraw.s:1000-1001). */
            F->otbias = (int8_t)(((int32_t)(w1 << 29)) >> 29);
            if (textured) {
                uint32_t td0 = cpu->read_word(fp + 0x08u);
                uint32_t td1 = cpu->read_word(fp + 0x0Cu);
                uint32_t td2 = cpu->read_word(fp + 0x10u);
                F->u[0]=(uint8_t)(td0 & 0xFFu); F->v[0]=(uint8_t)((td0 >> 8) & 0xFFu);
                F->u[1]=(uint8_t)(td1 & 0xFFu); F->v[1]=(uint8_t)((td1 >> 8) & 0xFFu);
                F->u[2]=(uint8_t)(td2 & 0xFFu); F->v[2]=(uint8_t)((td2 >> 8) & 0xFFu);
                F->u[3]=(uint8_t)((td2 >> 16) & 0xFFu); F->v[3]=(uint8_t)((td2 >> 24) & 0xFFu);
                unsigned cl = (td0 >> 16) & 0xFFFFu;
                unsigned tp = (td1 >> 16) & 0x7FFFu;
                F->blendbit = (uint8_t)((td1 >> 31) & 1u);
                ngte_tpage t;
                ngte_decode_tpage(tp, cl, 0u, &t);
                F->abr = (uint8_t)t.abr;
                F->tpx = (int16_t)t.tpx;
                F->tpy = (int16_t)t.tpy;
                F->bpp = (uint8_t)t.bpp;
                F->clx = (int16_t)t.clx;
                F->cly = (int16_t)t.cly;
            }
            n++;
        }
        fp += adv;
    }
    st->raw = raw; st->sprites = 0; st->skips = 0;
    st->align = (fp == fb + 4u + bytecount) ? 0 : (fp == end) ? 1 : 2;
    /* OPENPETE_MOBY_FACEBITS=1: per face stream, how many triangles carry each
     * of the two flag bits that leave the ordinary emit, so a material or
     * colour divergence can be tested against the bit that would explain it. */
    {
        static int s_fb = -1;
        if (s_fb < 0) { const char* e = getenv("OPENPETE_MOBY_FACEBITS"); s_fb = (e && *e && *e != '0'); }
        if (s_fb) {
            int tris = 0, b4 = 0, b5 = 0;
            for (int i = 0; i < n; i++) {
                if (out[i].i2 != out[i].i3) continue;
                tris++;
                if (out[i].hl == 2u) b4++; else if (out[i].hl == 1u) b5++;
            }
            fprintf(stderr, "  [facebits] fb=%08X faces=%d tris=%d bit4=%d bit5=%d\n",
                    fb, n, tris, b4, b5);
        }
    }
    return n;
}

/* The moby draw's own stride: two words untextured, five textured. */
int s23_decode_faces(CPUState* cpu, uint32_t fb, uint32_t bytecount,
                    moby_wface_t* out, int cap, moby_face_stats_t* st) {
    return s23_decode_faces_stride(cpu, fb, bytecount, out, cap, st, 0u, 0);
}

/* ---- per-face special materials (mobydraw.s:1870-1875) ----------------
 *
 * A triangle whose flag bit 5 is set leaves the ordinary emit and takes one of
 * three treatments, selected not by the face but by the record's material word
 * at +0x3C: the draw stashes that word in GTE control 8 at entry
 * (mobydraw.s:987), and where it is non-zero replaces it with its own bits
 * 28-30 as a 0..3 mode and runs that mode's per-moby setup
 * (mobydraw.s:1457-1460,1984-1998). Mode 3 and above disarm the treatment and
 * draw the face normally; a zero material word leaves control 8 at zero, which
 * is mode 0 with no setup.
 *
 * The material word packs three 8-bit colour channels at bits 0-7, 8-15 and
 * 16-23, each widened to the GTE's 12-bit form, plus a scalar in bits 24-27.
 *
 *   mode 0 (mobydraw.s:2215-2262) - the channels are the far colour RFC/GFC/BFC
 *     (mobydraw.s:2073-2076); each vertex depth-cues from BLACK toward it at
 *     IR0 = (the vertex colour's three channels summed) << 3, so the face
 *     grades by its own luminance. Where mode0_ir0_from_colour_sum is 0 the
 *     vertex colour is the source and IR0 is the per-moby value instead.
 *   mode 1 (mobydraw.s:2273-2301) - the same far colour, but one IR0 for the
 *     whole moby, bits 24-27 << 8 (mobydraw.s:2079), and each vertex depth-cues
 *     from its own colour. IR0 zero leaves the colours alone. This mode also
 *     substitutes a texture: its setup loads the global word at
 *     `game_layout.moby.special_tiledef`, biased by the record's CLUT row, into
 *     GTE control 16 (mobydraw.s:2085-2087,2098-2099), and the emit writes that
 *     register's halves over the primitive's texture page and CLUT
 *     (mobydraw.s:2296-2299). Its command word is POLY_GT3 whatever the face
 *     record carries (lui 0x3400 at S3 0x800325F0, S2 0x8004596C), and it
 *     writes per-corner environment coordinates in place of the face's UVs
 *     (mobydraw.s:2377-2547), so an untextured face record draws textured. The
 *     substitution is unconditional, so a zero IR0 leaves a face drawn from the
 *     substitute page with the model's own colours.
 *   mode 2 (mobydraw.s:2551-2564) - the specular sheen. The channels are the
 *     GTE background colour, bits 24-27 doubled is IR0 (mobydraw.s:2024-2045),
 *     and the light matrix is three copies of one constant vector
 *     (mobydraw.s:2048-2057). The face normal rotates into view by the model
 *     matrix, scales by IR0 and projects onto that vector on top of the
 *     background colour, giving one colour for the whole face.
 *
 * All three replace the face's vertex colours; mode 1 also replaces the
 * texture page, CLUT and UVs and draws the face textured. None of them touches
 * the face's geometry. The mode 0 and mode 2 emits start their command word at
 * the untextured code (mobydraw.s:2562) and the shared emit they fall into adds
 * the texture bit back from the face record, so those two keep the face
 * record's textured flag.
 *
 * CARVE-OUT: flag bit 4 takes a different dispatcher (mobydraw.s:2113) that
 * this decode does not carry; the walk leaves those faces on the ordinary
 * path.
 */

/** @brief Saturate a MAC value to a signed 16-bit IR register, as the GTE does with lm = 0.
 *
 * @param v The MAC value.
 * @return v clamped to [-32768, 32767]. */
static int s23_clamp_ir(long v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int)v;
}

/* ---- mode-1 texture coordinates (mobydraw.s:2377-2547) -----------------
 *
 * Mode 1 does not keep the face's own texture coordinates: the emit calls a
 * per-vertex routine three times, once per corner, and stores the halfword it
 * returns into the primitive's three coordinate slots (mobydraw.s:2326,
 * 2332-2336, 2338). The routine builds a spherical environment coordinate.
 *
 * Its input is the vector the vertex stage stored for that vertex: the
 * keyframe-interpolated model-space vertex INTPL leaves in MAC1-3
 * (mobydraw.s:1295-1301, stored at +0x2, +0x400 and +0x402 of the vertex
 * slot), read back as (MAC2, MAC3, MAC1) (mobydraw.s:2378-2384), the order
 * the vertex stage hands RTPS (mobydraw.s:1308-1314), so the vector is the
 * walk's model-space vertex truncated to 16 bits. MVMVA rotates it by the
 * Moby's own rotation at +0x20, which the mode-1 setup loads into the GTE's
 * rotation registers after the vertex stage has projected every vertex
 * (mobydraw.s:2079-2093): the map follows the moby's orientation in the
 * world, not the view.
 *
 * From the rotated vector (x, y, z) the routine takes the azimuth from the
 * ratio of the minor to the major of |z| and |x| through an arctangent table,
 * placed in its octant by the two signs (mobydraw.s:2397-2415, 2448-2496,
 * 2497-2504); the elevation from -y divided by the vector's length, the length
 * itself coming from a square-root table indexed by the squared length
 * normalized by its leading-zero count (mobydraw.s:2416-2446); and turns the
 * pair into a disc coordinate by looking the elevation up as a radius and
 * multiplying it by the sine and cosine of the azimuth, offset to the page's
 * centre texel 0x40 (mobydraw.s:2516-2547). The azimuth carries a per-record
 * bias the setup leaves in GTE control 17 (mobydraw.s:2096-2112).
 *
 * The routine memoizes its result in the vertex slot and marks the slot done,
 * so a vertex shared by several faces is transformed once; the walk computes
 * per face corner instead, which gives the same coordinate for the same
 * vertex.
 *
 * The walk draws the continuous map the routine's tables sample, unrounded,
 * from its inputs at the present (s23_env_uv_f, s23_env_present_read). The
 * integer routine below is kept as the reference the verify probe scores
 * against the game's primitives (s23_env_face_uv).
 */

/** @brief The GTE's LZCS/LZCR count: leading zeroes of a positive value, leading ones of a negative one.
 *
 * @param v The value, read as signed.
 * @return The count, 1..32. */
static int s23_lzcr(uint32_t v) {
    if (v & 0x80000000u) v = ~v;
    int n = 0;
    while (n < 32 && !(v & 0x80000000u)) { n++; v <<= 1; }
    return n;
}

/* The cull record's word whose bytes 2 and 3 the mode-1 setup subtracts for
 * the depth term's shift (mobydraw.s:2096, 2103-2106). The cull stores the
 * keyframe header's byte 3 at +0x1A and that byte plus the header's scale
 * byte at +0x1B (mobydraw.s:499-501), so the difference is the model's scale
 * shift. */
#define S23_REC_ENV_SHIFT_OFF 0x18u

/* The per-record environment-map setup, resolved once per face: the Moby's
 * rotation the routine's MVMVA applies, the azimuth bias, and whether the
 * vertex stage stored the vertex through the packed word that borrows. */
typedef struct { int R[3][3]; int32_t bias; int borrow; } s23_env_setup;

/** @brief The shift the azimuth bias's depth term takes: the difference of bytes 3 and 2 of the cull record's word at +0x18, or the header's scale byte for a host-built record.
 *
 * The amount's low five bits, as SLLV reads it (mobydraw.s:2103-2106). A
 * record the walk built itself has no word at +0x18; the cull stores there
 * the header's byte 3 and that byte plus the header's scale byte
 * (mobydraw.s:499-501), so the shift is the scale byte.
 * @param cpu The guest CPU state, read only.
 * @param rec The cull record.
 * @return The shift, 0..31. */
static int s23_env_depth_shift(CPUState* cpu, const moby_face_rec_t* rec) {
    if (rec->addr) {
        const uint32_t w = moby_rec_word(cpu, rec->addr + S23_REC_ENV_SHIFT_OFF);
        return ((int)(w >> 24) - (int)((w >> 16) & 0xFFu)) & 31;
    }
    return (int)(cpu->read_byte(rec->header + game_layout.moby.anim_scale_off) & 31u);
}

/** @brief Resolves a record's environment-map setup: the Moby's own rotation, the azimuth bias the draw leaves in GTE control 17, and the vertex slot's borrow.
 *
 * The rotation is the five words at the Moby's +0x20 (mobydraw.s:2082-2093).
 * The bias is the record's view-space depth, which its own setup loaded into
 * the translation register (mobydraw.s:898-908), shifted left by the
 * difference of bytes 3 and 2 of the record's word at +0x18 (the shift
 * amount's low five bits, as SLLV reads it) and logically right by 6, plus
 * the global word at `env_bias_word` shifted arithmetically right by 3
 * (mobydraw.s:2096-2112), the shift s23_env_depth_shift's. Only its low
 * byte reaches the azimuth. A record with a zero keyframe blend takes the
 * draw's unblended vertex path (mobydraw.s:922-923, 986), which stores the
 * vertex's X and Y as the one word VX + (VY << 16) (mobydraw.s:1027-1028,
 * 1035), so a negative X borrows one from the Y the routine reads back; the
 * blended path stores the three halves apart (mobydraw.s:1299-1301).
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby the record draws.
 * @param rec The cull record.
 * @param E Receives the setup. */
static void s23_env_setup_read(CPUState* cpu, uint32_t moby, const moby_face_rec_t* rec, s23_env_setup* E) {
    ngte_read_matrix(cpu, moby + game_layout.moby.rot_off, E->R);
    const int sh = s23_env_depth_shift(cpu, rec);
    const uint32_t depth = ((uint32_t)rec->tr_z << sh) >> 6;
    const int32_t g = game_layout.moby.env_bias_word
                    ? (int32_t)cpu->read_word(game_layout.moby.env_bias_word) : 0;
    E->bias = (int32_t)(depth + (uint32_t)(g >> 3));
    E->borrow = ((rec->w0 >> game_layout.moby.blend_shift) & game_layout.moby.blend_mask) == 0u;
}

/** @brief The routine's azimuth in 256ths of a turn, before the bias: the arctangent table entry placed in the octant the signs of z and -x select.
 *
 * The draw branches on the sign of z, then of -x, then on which of the two
 * magnitudes is larger (mobydraw.s:2448-2496). Each comparison branch's delay
 * slot writes the add/subtract flag on both paths, and the taken target
 * rewrites it, so the taken branch (the minor axis is z) takes the target's
 * flag and base: z >= 0 with -x >= 0 gives 0x40 - a when z < -x, else a;
 * z >= 0 with -x < 0 gives 0xC0 + a when z < x, else 0x100 - a; z < 0 with
 * -x >= 0 gives 0x40 + a when -z < -x, else 0x80 - a; z < 0 with -x < 0 gives
 * 0xC0 - a when -z < x, else 0x80 + a. The result is atan2(-x, z) in 256ths
 * of a turn.
 * @param z The rotated vector's third component (MAC3).
 * @param nx The negated first component (-MAC1).
 * @param at The arctangent table entry of the minor-over-major ratio.
 * @return The azimuth, 0..0x100. */
static int s23_env_azimuth(long z, long nx, int at) {
    const long az = z < 0 ? -z : z, anx = nx < 0 ? -nx : nx;
    const int minor_z = az < anx;               /* the taken branch of each octant */
    if (z >= 0 && nx >= 0) return minor_z ? 0x40 - at : at;
    if (z >= 0)            return minor_z ? 0xC0 + at : 0x100 - at;
    if (nx >= 0)           return minor_z ? 0x40 + at : 0x80 - at;
    return minor_z ? 0xC0 - at : 0x80 + at;
}

/** @brief One face corner's mode-1 environment coordinate pair as the game computes it, the verify probe's reference.
 *
 * The per-vertex routine of mobydraw.s:2377-2547: the vertex as the vertex
 * stage stored it (the unblended path's borrow, s23_env_setup_read, and each
 * component truncated to its 16-bit slot), rotated by the Moby's rotation
 * (MVMVA, shift 12); the azimuth from the arctangent table in the octant the
 * signs of z and -x place it (s23_env_azimuth); the elevation from -y over
 * the vector's length through the radius table; and the disc coordinate from
 * that radius times the cosine and sine of the biased azimuth, centred on
 * texel 0x40.
 * @param cpu The guest CPU state, read only (the square-root, arctangent, radius and sine tables).
 * @param E The per-record setup: the rotation, the azimuth bias and the borrow.
 * @param mv The corner's model-space vertex after the keyframe blend.
 * @param pu Receives U.
 * @param pv Receives V. */
static void s23_env_uv(CPUState* cpu, const s23_env_setup* E, const float mv[3],
                      uint8_t* pu, uint8_t* pv) {
    /* INTPL's MAC floors the blend; the slot keeps its low 16 bits
     * (mobydraw.s:1295-1301), read back sign-extended (mobydraw.s:2378-2384). */
    long V[3];
    for (int j = 0; j < 3; j++) V[j] = (long)floor((double)mv[j]);
    if (E->borrow && V[0] < 0) V[1] -= 1;
    for (int j = 0; j < 3; j++) V[j] = (int16_t)(uint16_t)V[j];
    long mac[3], ir[3];
    for (int i = 0; i < 3; i++) {
        const long long s = (long long)E->R[i][0] * V[0] + (long long)E->R[i][1] * V[1]
                          + (long long)E->R[i][2] * V[2];
        mac[i] = (long)(s >> 12);
        ir[i] = s23_clamp_ir(mac[i]);
    }
    long z = mac[2], nx = -mac[0], ny = -mac[1];    /* mobydraw.s:2391-2396 */

    /* The azimuth ratio: the minor of |z| and |x| over the major, in 6
     * fractional bits (mobydraw.s:2397-2415). */
    long a = z < 0 ? -z : z, b = nx < 0 ? -nx : nx;
    if (a > b) { long t = a; a = b; b = t; }
    if (b == 0) b = 1;
    long q = (a << 6) / b;
    if (q > 64) q = 64;

    /* SQR 0 then the square root of the sum (mobydraw.s:2394, 2416-2446). */
    unsigned long len2 = 0;
    for (int i = 0; i < 3; i++) len2 += (unsigned long)(ir[i] * ir[i]);
    long len = 0;
    if (len2) {
        int lz = s23_lzcr((uint32_t)len2) & ~1;
        int sh = (0x1F - lz) >> 1;
        int e2 = lz - 0x18;
        uint32_t t = e2 >= 0 ? ((uint32_t)len2 << e2) : ((uint32_t)len2 >> (-e2));
        int idx = ((int)t - 0x40) * 2;
        int w = (int16_t)cpu->read_half(game_layout.moby.env_sqrt_table + (uint32_t)idx);
        len = ((long)w << sh) >> 12;
    }

    int at = (int)cpu->read_byte(game_layout.moby.env_atan_table + (uint32_t)q);
    int ang = s23_env_azimuth(z, nx, at);

    /* The elevation, clamped to the radius table's range (mobydraw.s:2506-2514). */
    long evq = len ? ((ny << 12) / len) : 0;
    long el = evq >> 5;
    if (el < 0) el = 0; else if (el > 0x80) el = 0x80;
    int rad = (int)cpu->read_byte(game_layout.moby.env_radius_table + (uint32_t)el) >> 1;

    /* The disc coordinate, centred on texel 0x40 (mobydraw.s:2516-2547). */
    int az = (int)(((uint32_t)ang + (uint32_t)E->bias) & 0xFFu);
    uint32_t sp = game_layout.sin_table + (uint32_t)(az << 1);
    int cs = (int16_t)cpu->read_half(sp + 0x80u);
    int sn = (int16_t)cpu->read_half(sp);
    *pu = (uint8_t)(((((long)rad * cs) >> 12) + 0x40) & 0xFF);
    *pv = (uint8_t)(((((long)rad * sn) >> 12) + 0x40) & 0xFF);
}

/* The drawn form of the per-record setup: the Moby's rotation and the azimuth
 * bias, each at the present the walk draws and unrounded. */
typedef struct { float R[3][3]; double bias; } s23_env_present;

/** @brief Resolves a record's environment-map inputs at the present: the Moby's rotation and the azimuth bias, unrounded.
 *
 * The rotation is the Moby's own at this present (moby_env_matrix_at_present).
 * The bias is s23_env_setup_read's sum in float: the record's depth at full
 * precision (rec->tr_zf, the centre it projects through) times 2^shift over
 * 64, plus the camera heading at the present (moby_env_heading) over 8, both
 * in 256ths of a turn. The integer form keeps only the depth's and the
 * heading's whole bits; the sum here is their exact value, so the map turns
 * continuously under a pan and a dolly.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby the record draws.
 * @param rec The cull record.
 * @param P Receives the inputs. */
static void s23_env_present_read(CPUState* cpu, uint32_t moby, const moby_face_rec_t* rec,
                                 s23_env_present* P) {
    moby_env_matrix_at_present(cpu, moby, P->R);
    const double depth = (double)rec->tr_zf * ldexp(1.0, s23_env_depth_shift(cpu, rec)) / 64.0;
    P->bias = depth + (double)moby_env_heading() / 8.0;
}

/** @brief One face corner's drawn environment coordinate pair: the continuous map the routine's tables sample, unrounded.
 *
 * With e the Moby's rotation applied to the corner's model-space vertex, the
 * azimuth is atan2(-x, z) plus the bias, and the radius (128 / pi) acos(c)
 * for c = -y / |e| clamped to [0, 1]; the pair is 0x40 plus the radius times
 * the azimuth's cosine and sine. The routine's tables are samples of the same
 * functions: its arctangent table holds atan in 256ths of a turn, its radius
 * table (256 / pi) acos(i / 128) halved by the routine, its sine table 4096
 * sin (s23_env_uv).
 * @param P The record's inputs at the present.
 * @param mv The corner's model-space vertex at the present.
 * @param pu Receives U, in texels.
 * @param pv Receives V, in texels. */
static void s23_env_uv_f(const s23_env_present* P, const float mv[3], float* pu, float* pv) {
    double e[3];
    for (int i = 0; i < 3; i++)
        e[i] = (double)P->R[i][0] * mv[0] + (double)P->R[i][1] * mv[1] + (double)P->R[i][2] * mv[2];
    const double len = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
    double c = len > 0.0 ? -e[1] / len : 0.0;
    if (c < 0.0) c = 0.0; else if (c > 1.0) c = 1.0;
    const double phi = atan2(-e[0], e[2]) + M_PI / 128.0 * fmod(P->bias, 256.0);
    const double r = 128.0 / M_PI * acos(c);
    *pu = (float)(64.0 + r * cos(phi));
    *pv = (float)(64.0 + r * sin(phi));
}

/** @brief One face's four corner coordinate pairs: the drawn float map, or inside the verify probe's reference walk the game's integer routine.
 *
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby the record draws.
 * @param rec The cull record.
 * @param mv The four corners' model-space vertices.
 * @param tex Receives the coordinates. */
static void s23_env_face_uv(CPUState* cpu, uint32_t moby, const moby_face_rec_t* rec,
                            const float mv[4][3], moby_face_tex_t* tex) {
    if (!moby_env_reference()) {
        s23_env_present P;
        s23_env_present_read(cpu, moby, rec, &P);
        for (int k = 0; k < 4; k++) s23_env_uv_f(&P, mv[k], &tex->u[k], &tex->v[k]);
        return;
    }
    s23_env_setup E;
    s23_env_setup_read(cpu, moby, rec, &E);
    for (int k = 0; k < 4; k++) {
        uint8_t iu, iv;
        s23_env_uv(cpu, &E, mv[k], &iu, &iv);
        tex->u[k] = (float)iu;
        tex->v[k] = (float)iv;
    }
}

/** @brief Which of the two forms the special material's mode 0 takes.
 *
 * OPENPETE_MOBY_MODE0_IR0_SUM=0|1, read once, overrides
 * moby_game_s23.mode0_ir0_from_colour_sum for one run, so the two coded forms
 * can be priced against the guest's own primitive colours on the same pose.
 * Unset or empty, the game's own switch decides.
 * @return 1 for IR0 from the vertex colour sum, 0 for the per-moby IR0. */
static int s23_mode0_ir0_sum(void) {
    static int s = -2;
    if (s == -2) { const char* e = getenv("OPENPETE_MOBY_MODE0_IR0_SUM");
                   s = (e && *e) ? (*e != '0') : -1; }
    return s >= 0 ? s : moby_game_s23.mode0_ir0_from_colour_sum;
}

/** @brief The material_tint hook: mode 3's whole-table depth cue toward the material colour.
 *
 * After the LOD fade has resolved the record's colour table, a material word
 * of mode 3 (bits 28-30) DPCSes every colour of the table (count = model
 * header byte 1) toward FC = the word's bytes 0-2 each shifted into 12 bits
 * at IR0 = bits 24-27 shifted into 0xF00, writes the result to the scratch
 * table at +0x1600 and draws every face from it (S2 mobydraw.s:2004-2035,
 * S3 mobydraw.s:2000-2031, reached from the material branch after every fade
 * band, S2 :1462-1464). The simple models' draw has no material branch.
 * OPENPETE_MOBY_TINT_OFF=1 skips the tint, the negative control of the colour
 * compare.
 * @param mat The record's material word.
 * @param fc Receives the 12-bit far colour, R, G, B.
 * @param ir0 Receives the IR0.
 * @return 1 when the word selects the tint, else 0. */
static int s23_material_tint(uint32_t mat, int fc[3], int* ir0) {
    static int s_off = -1;
    if (s_off < 0) { const char* e = getenv("OPENPETE_MOBY_TINT_OFF"); s_off = (e && *e && *e != '0'); }
    if (s_off || ((mat >> 28) & 7u) != 3u) return 0;
    fc[0] = (int)((mat <<  4) & 0xFF0u);   /* sll 4,  andi 0xFF0 -> ctc2 RFC */
    fc[1] = (int)((mat >>  4) & 0xFF0u);   /* srl 4,  andi 0xFF0 -> ctc2 GFC */
    fc[2] = (int)((mat >> 12) & 0xFF0u);   /* srl 12, andi 0xFF0 -> ctc2 BFC */
    *ir0  = (int)((mat >> 16) & 0xF00u);   /* srl 16, andi 0xF00 -> mtc2 IR0 */
    return 1;
}

/** @brief Whether OPENPETE_MOBY_MIRROR_OFF, the mirrored-record face sign's negative control, is set.
 *
 * Latched on the first call. When set, s23_record_face_sign answers +1 for
 * every record, so a mirrored record keeps the plain facing test.
 * @return 1 when set, else 0. */
static int s23_mirror_disabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_MIRROR_OFF"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief The record_face_sign hook: -1 for a material mode of 4 or above, whose draw negates every NCLIP.
 *
 * The regular draw dispatches a non-zero material word on its mode (bits
 * 28-30). Modes 0-3 take their own setups; modes 4-7 fall to the default arm,
 * which sets the mode register to 3 and flips bit 31 of the colour-table
 * pointer $t9 (S3 mobydraw.s 0x80032144-0x80032150, S2 mobydraw.s
 * 0x80045524-0x80045530). Every face test then sees $t9 positive and takes
 * .L80031AD8 (S2 .L80044EB8): NCLIP1 is negated and NCLIP2 runs on
 * (v3, v2, v1), which is the negated NCLIP(v3, v1, v2). The mirror twin of a
 * moby carries mode 5, whose cull negates matrix column 1, so its screen
 * winding is reversed and the negated test keeps its outward faces.
 * OPENPETE_MOBY_MIRROR_OFF answers +1 for every record.
 * @param mat The record's material word.
 * @return -1 for mode 4-7, else +1. */
static int s23_record_face_sign(uint32_t mat) {
    if (((mat >> 28) & 7u) < 4u || s23_mirror_disabled()) return 1;
    return -1;
}

/** @brief Whether OPENPETE_MOBY_VERIFY_NOPLANE, the moby water-plane split's negative control, is set.
 *
 * Latched on the first call. When set, s23_water_plane answers no split for
 * every record, so a record the draw splits takes the ordinary walk and the
 * class colour verify of a split moby collapses.
 * @return 1 when set, else 0. */
static int s23_water_plane_disabled(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_MOBY_VERIFY_NOPLANE"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief The water_plane hook: the regular draw's split of a moby at a world plane.
 *
 * A material word with bit 31 set and mode (bits 28-30) 0 sends the regular
 * draw to its split loop instead of the mode-0 setup (S3 mobydraw.s:2061
 * bltz to .L80032AB8; S2 mobydraw.s:2065 to .L80045E34; the two loops are the
 * same instruction for instruction). The setup (S3 mobydraw.s:2647-2722,
 * S2 mobydraw.s:2626-2701) takes the low 24 bits of the word, sign-extended,
 * as an offset: the plane is the moby's position Z (Moby+0x14) less the
 * offset's magnitude, and each vertex's distance word is the plane less the
 * second row of the moby's rotation (Moby+0x20) applied to the model vertex
 * (MVMVA sf = 1, no translation) and shifted left by the cull record's
 * byte 0x1B less byte 0x1A, which the cull writes as the AnimationHeader's
 * scale byte (+0x02) (S3 mobydraw.s:485-500). An offset of 0 or above drops
 * a wholly negative face; a negative offset depth-cues it at IR0 0x800
 * toward the environment's far colour (G_ENVIRONMENT+0x34..0x3C), the four
 * corners by one DPCS each (S3 mobydraw.s:3124-3163). A face with corners on
 * both sides is cut at the plane by INTPL and each part takes its side's
 * policy (S3 mobydraw.s:3164 on). The split loop applies no per-face special
 * material. OPENPETE_MOBY_VERIFY_NOPLANE answers 0 for every record.
 * @param cpu Reads the moby, the header and the far colour.
 * @param mat The record's material word.
 * @param moby The Moby the record was built from.
 * @param header The record's AnimationHeader.
 * @param wp Receives the split.
 * @return 1 when the word selects the split, else 0. */
static int s23_water_plane(CPUState* cpu, uint32_t mat, uint32_t moby, uint32_t header,
                           moby_wplane_t* wp) {
    if (!(mat & 0x80000000u) || ((mat >> 28) & 7u) != 0u) return 0;
    if (moby < 0x80000000u || header < 0x80000000u || !game_layout.moby.rot_off) return 0;
    if (s23_water_plane_disabled()) return 0;
    const int32_t off = ((int32_t)(mat << 8)) >> 8;
    const int32_t z = (int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 8u);
    int R[3][3];
    ngte_read_matrix(cpu, moby + game_layout.moby.rot_off, R);
    wp->plane = (double)z - (double)(off < 0 ? -off : off);
    for (int i = 0; i < 3; i++) wp->row[i] = (double)R[1][i];
    const int shift = (int)(cpu->read_byte(header + game_layout.moby.anim_scale_off) & 0xFFu);
    wp->unit = ldexp(1.0, shift > 30 ? 30 : shift);
    for (int i = 0; i < 3; i++)
        wp->fc[i] = (int32_t)cpu->read_word(game_layout.fog.far_colour + (uint32_t)i * 4u);
    wp->ir0 = 0x800;
    wp->drop_below = off >= 0;
    /* OPENPETE_MOBY_WPLANE_CENSUS=1: one line per split record per walk pass,
     * the census of the mobys a corpus tile draws split. */
    static int s_cen = -1;
    if (s_cen < 0) { const char* e = getenv("OPENPETE_MOBY_WPLANE_CENSUS"); s_cen = (e && *e && *e != '0'); }
    if (s_cen)
        fprintf(stderr, "[moby-wplane vbl=%u key=%u] moby=%08X cls=%03X mat=%08X plane=%.0f drop=%d\n",
                openpete_frame, openpete_feed_key_now(), moby, cpu->read_half(moby + game_layout.moby.class_off) & 0xFFFu,
                mat, wp->plane, wp->drop_below);
    return 1;
}

/** @brief Applies the special material's treatment to one triangle's vertex colours and texture.
 *
 * The special_face hook of the animated models. A face whose flag field selects
 * the other dispatcher (`hl` 2, bit 4) or a material whose mode (bits 28-30 of
 * `mat`, 0 for a zero word) exceeds 2 is left to the ordinary emit: mode 3
 * recolours the whole colour table before any face is drawn
 * (s23_material_tint), and modes 3-7 then set the draw's mode register to 3,
 * which no face treatment matches (mobydraw.s:1988-2003). Mode 0
 * depth-cues each vertex colour toward the material's far colour; mode 1 does
 * the same at one IR0 for the moby and also names a substitute texture page,
 * CLUT and generated environment coordinates in `tex`; mode 2 replaces all
 * four colours with one sheen colour computed from the face normal.
 * @param cpu The guest CPU state, read only.
 * @param mat The record's material word.
 * @param M The model-to-view matrix at unit scale; mode 2 rotates the face
 *        normal by it and floors each component as the GTE's >> 12 does.
 * @param moby The Moby the record draws; mode 1 reads its rotation.
 * @param rec The cull record, passed to the environment setup.
 * @param F The face; `hl` must be 1, and n1..n3 are the normal mode 2 reads.
 * @param c The four vertex colours, rewritten in place.
 * @param mv The four corners' model-space vertices after the keyframe blend, read by mode 1.
 * @param textured Set to 1 by mode 1 when it applies the substitute page;
 *        modes 0 and 2 keep the face's own flag.
 * @param tex Receives mode 1's substitute page, CLUT and coordinates; the walk
 *        zeroes it before the call.
 * @return 0 when the face is left alone, else 1 plus the mode applied. */
static int s23_special_face(CPUState* cpu, uint32_t mat, const float M[3][3],
                           uint32_t moby, const moby_face_rec_t* rec, const moby_wface_t* F,
                           uint32_t c[4], const float mv[4][3], int* textured,
                           moby_face_tex_t* tex) {
    if (F->hl != 1) return 0;                      /* bit 4: the other dispatcher */
    int mode = mat ? (int)((mat >> 28) & 7u) : 0;
    if (mode > 2) return 0;                        /* no face treatment; mode 3 tints the table */

    /* The three 12-bit channels the material word carries. */
    int mc[3] = { (int)((mat <<  4) & 0xFF0u),
                  (int)((mat >>  4) & 0xFF0u),
                  (int)((mat >> 12) & 0xFF0u) };

    if (mode == 0) {
        /* The two draws part here, and only here. One zeroes the source colour
         * once for the whole block (mtc2 $zero, RGBC) and sets IR0 per vertex
         * from the sum of that vertex's three colour bytes shifted left 3, so
         * each channel interpolates from 0 toward the far colour by the
         * vertex's own brightness. The other loads the colour word itself into
         * RGBC per vertex and leaves IR0 at the per-moby value the material
         * setup stored, which is the treatment mode 1 gives its colour half.
         * The per-moby IR0 is the material word's bits 24-27 shifted into
         * 0xF00, the same field both setups write to GTE data 8. */
        const int per_moby_ir0 = (int)((mat >> 16) & 0xF00u);
        for (int k = 0; k < 4; k++) {
            int src[3], ir0;
            if (s23_mode0_ir0_sum()) {
                int sum = (int)(c[k] & 0xFFu) + (int)((c[k] >> 8) & 0xFFu)
                        + (int)((c[k] >> 16) & 0xFFu);
                ir0 = sum << 3;
                src[0] = src[1] = src[2] = 0;
            } else {
                ir0 = per_moby_ir0;
                for (int ch = 0; ch < 3; ch++) src[ch] = (int)((c[k] >> (ch * 8)) & 0xFFu);
            }
            uint32_t o = 0u;
            for (int ch = 0; ch < 3; ch++)
                o |= (uint32_t)ngte_dpcs_ch(src[ch], mc[ch], ir0) << (ch * 8);
            c[k] = o;
        }
        return 1;
    }

    if (mode == 1) {
        /* The substitute page. The per-moby setup loads one global word into
         * GTE control 16 whenever the mode is non-zero (mobydraw.s:2085-2087,
         * 2098-2099), and the per-face emit writes that register's low half
         * over the primitive's texture page and its high half over the CLUT
         * (mobydraw.s:2296-2299). Its command word is the opaque POLY_GT3
         * code 0x34 (lui at S3 0x800325F0, S2 0x8004596C) whatever the face
         * record carries: an untextured face record draws textured from the
         * substitute page, and no face takes a semi-transparency bit. */
        uint32_t g = game_layout.moby.special_tiledef
                   ? cpu->read_word(game_layout.moby.special_tiledef) : 0u;
        if (g) {
            unsigned tp = g & 0xFFFFu, cl = (g >> 16) & 0xFFFFu;
            ngte_tpage t;
            ngte_decode_tpage(tp, cl, 0u, &t);
            tex->tpx = (int16_t)t.tpx;
            tex->tpy = (int16_t)t.tpy;
            tex->bpp = (uint8_t)t.bpp;
            tex->abr = (uint8_t)t.abr;
            tex->clx = (int16_t)t.clx;
            tex->cly = (int16_t)t.cly;
            tex->set = 1u;
            *textured = 1;
        }
        /* The coordinates the emit generates per corner in place of the face's
         * own, from that corner's model-space vertex. */
        s23_env_face_uv(cpu, moby, rec, mv, tex);
        tex->uv_set = 1u;
        /* The colour half runs off one IR0 for the whole moby, bits 24-27 << 8
         * (mobydraw.s:2079), carried in GTE control 12. The emit reads that
         * register first and skips every DPCS when it is zero
         * (mobydraw.s:2284), so a zero IR0 leaves the vertex colours as the
         * model stored them and the substitute page is the whole treatment. */
        int ir0 = (int)((mat >> 16) & 0xF00u);
        if (ir0) {
            for (int k = 0; k < 4; k++) {
                uint32_t o = 0u;
                for (int ch = 0; ch < 3; ch++)
                    o |= (uint32_t)ngte_dpcs_ch((int)((c[k] >> (ch * 8)) & 0xFFu),
                                                mc[ch], ir0) << (ch * 8);
                c[k] = o;
            }
        }
        return (tex->set || tex->uv_set || ir0) ? 2 : 0;
    }

    /* mode 2. The normal through the model matrix, scaled by IR0, projected on
     * the light vector over the background colour. The rotation is the float
     * model-to-view matrix, floored to the IR register; every later stage
     * truncates the way its GTE op does: CC shifts by 12, GPF does not, and CC's
     * second stage multiplies by a white 8-bit modulating colour. */
    int ir0 = (int)((mat >> 23) & 0x1Eu);
    int ir[3];
    for (int i = 0; i < 3; i++) {
        const double m = (double)M[i][0] * F->n1 + (double)M[i][1] * F->n2 + (double)M[i][2] * F->n3;
        ir[i] = s23_clamp_ir((long)floor(m));
    }
    for (int i = 0; i < 3; i++) ir[i] = s23_clamp_ir((long)ir0 * ir[i]);
    long dot = (long)moby_game_s23.spec_light[0] * ir[0]
             + (long)moby_game_s23.spec_light[1] * ir[1]
             + (long)moby_game_s23.spec_light[2] * ir[2];
    uint32_t o = 0u;
    for (int ch = 0; ch < 3; ch++) {
        long mac = (((long)mc[ch] * 0x1000) + dot) >> 12;
        if (mac < 0) mac = 0; else if (mac > 32767) mac = 32767;
        long v = (((long)(255 << 4) * mac) >> 12) >> 4;
        if (v < 0) v = 0; else if (v > 255) v = 255;
        o |= (uint32_t)v << (ch * 8);
    }
    c[0] = c[1] = c[2] = c[3] = o;
    return 3;
}

/** @brief Reads one animated cull record into the walk's record form.
 *
 * Stride 0x44 in the tick's draw scratch, terminated by a zero flags word
 * (mobydraw.s:888). The flags word carries the keyframe blend byte and, in its
 * top half, the LOD verdict whose sign picks the low-poly vertex count and
 * face list (mobydraw.s:918-921). The two keyframes are the stream words
 * themselves with their own headers at +0x04 and +0x08 (mobydraw.s:888-889,
 * 911,988-989).
 * @param cpu The guest CPU state, read only.
 * @param rec The record's address.
 * @param R Receives the record; its tier is the flags word's signed top half.
 * @return 0 at the zero flags word that terminates the list, 1 otherwise. */
static int s23_read_record(CPUState* cpu, uint32_t rec, moby_record_t* R) {
    const uint32_t w0 = moby_rec_word(cpu, rec);
    if (w0 == 0) return 0;
    R->w0       = w0;
    R->header   = moby_rec_word(cpu, rec + game_layout.moby.rec_header_off);
    R->header_b = moby_rec_word(cpu, rec + game_layout.moby.rec_header_b_off);
    R->frame_a  = moby_rec_word(cpu, rec + game_layout.moby.rec_frame_a_off);
    R->frame_b  = moby_rec_word(cpu, rec + game_layout.moby.rec_frame_b_off);
    R->moby     = moby_rec_word(cpu, rec + game_layout.moby.rec_moby_off);
    R->material = moby_rec_word(cpu, rec + game_layout.moby.rec_material_off);
    for (int i = 0; i < 3; i++)
        R->TR_raw[i] = (int32_t)moby_rec_word(cpu, rec + game_layout.moby.rec_tr_off + (uint32_t)i * 4u);
    moby_rec_matrix(cpu, rec + game_layout.moby.rec_matrix_off, R->M);
    R->tier = (int)(int16_t)(w0 >> 16);
    return 1;
}

/** @brief The world ordering-table splice the regular draw reads from an animated cull record.
 *
 * The cull stores the world bucket base at +0x18 (halfword) and the splice
 * shift at +0x1B (mobydraw.s 0x80043FA4-0x8004402C); the draw loads them into
 * GTE control 9 and 11 (0x80044DA4-0x80044DC8) and splices from them
 * (0x8004539C).
 * @param cpu The guest CPU state, read only.
 * @param rec The cull record's address.
 * @param wbase Receives the world bucket base.
 * @param n Receives the splice shift.
 * @return 1, or 0 when `rec` is not a KSEG0 address. */
static int s23_otkey_splice(CPUState* cpu, uint32_t rec, int* wbase, int* n) {
    if (rec < 0x80000000u) return 0;
    const uint32_t w = moby_rec_word(cpu, rec + 0x18u);
    *wbase = (int)(w & 0xFFFFu);
    *n = (int)(w >> 24);
    return 1;
}

/** @brief Whether a model-table entry is an animated model.
 *
 * A KSEG0 pointer is an animated model; a KUSEG one is a simple model, drawn
 * by its own pass (mobydraw.s:116).
 * @param cpu Unused.
 * @param model The model-table entry.
 * @return 1 for an animated model, 0 for a simple model. */
static int s23_model_is_animated(CPUState* cpu, uint32_t model) {
    (void)cpu;
    return (int32_t)model < 0;
}

/** @brief The keyframe's faceblock patch list, or 0 when the keyframe patches nothing.
 *
 * The cull resolves the list through the Model's own table and stores the
 * pointer at record +0x14, and marks a keyframe that patches nothing by
 * setting the stored keyframe word's top bits (mobydraw.s:532-534,561-568); the
 * draw skips the apply on exactly that sign (mobydraw.s:935). A keyframe the
 * record does not name is resolved through the model's table the way the cull
 * resolves it.
 * @param cpu The guest CPU state, read only.
 * @param rec The cull record, whose +0x14 holds the list the cull resolved.
 * @param model The moby's model for a keyframe the record does not name, else 0.
 * @param frame_a The keyframe word; a negative one patches nothing.
 * @return The list's address, or 0. */
static uint32_t s23_frame_patch_list(CPUState* cpu, uint32_t rec, uint32_t model,
                                     uint32_t frame_a) {
    if ((int32_t)frame_a < 0) return 0u;
    if (model >= 0x80000000u) {
        /* A keyframe no record names. Resolve the list the cull resolves:
         * Model+0x34 is the table base, the keyframe word's top byte masked to
         * 0x7C indexes it, and the word read there is the list's byte offset
         * from that same base (mobydraw.s:563-569). */
        uint32_t base = cpu->read_word(model + 0x34u);
        if (base < 0x80000000u) return 0u;
        return base + cpu->read_word(base + ((frame_a >> 24) & 0x7Cu));
    }
    return moby_rec_word(cpu, rec + 0x14u);
}

/** @brief The record's blend byte for a Moby+0x40 animation-state index, looked up in the game's blend table.
 *
 * Moby+0x40 is an animation-state index, not the keyframe blend numerator:
 * the cull reads the byte, indexes this table with it and stores the table
 * entry as the record's blend byte, taking the no-blend path (frame A alone,
 * no frame B header or keyframe) when the entry is zero
 * (mobydraw.s:454,458-461,466,487). The draw then scales that byte by 16 into
 * the GTE's IR0 (mobydraw.s:922-923).
 * @param cpu The guest CPU state, read only.
 * @param state4 The animation-state byte; bits above the low 8 are ignored.
 * @return The blend byte. */
static uint32_t s23_anim_blend_byte(CPUState* cpu, uint32_t state4) {
    return cpu->read_byte(moby_game_s23.anim_blend_table + (state4 & 0xFFu)) & 0xFFu;
}


/* ---- the simple models (the static draw, mobydraw.s:3862) --------------
 *
 * The cull sorts each drawn moby by the sign of its model-table entry
 * (mobydraw.s:116) and writes the KUSEG half into a second, stride-0x10 queue
 * at draw scratch +0x1C00, bounded at 63 records by the cursor limit it keeps
 * in GTE control 8 and 9 (mobydraw.s:44,46). One record is
 *
 *   +0x00  Moby*
 *   +0x04  (mobyX - camX) >> 2 in the high half, (camY - mobyY) >> 2 in the low
 *   +0x08  (camZ - mobyZ) >> 2 in the high half, the far-cull distance in the low
 *   +0x0C  Model + (Moby+0x3C byte 0) * 4, whose +0x14 is the geometry header
 *
 * (mobydraw.s:617-630; the three offsets are the camera-relative quarter-world
 * position the cull already computed, mobydraw.s:82,87,96). A record whose
 * far-distance half has bit 15 set is the screen-space kind: the draw skips the
 * camera transform and takes the record's own halves as the GTE screen offsets
 * (mobydraw.s:3915,4010-4022), which s23_simple_place mirrors. A second builder
 * writes that kind's record, out of the moby's own three position words rather
 * than any camera-relative quantity (mobydraw.s:604-628), so its halves are the
 * low 16 bits of Moby+0x0C, +0x10 and +0x14 and its far half is 0xFFFF.
 *
 * The draw transforms the record by the camera matrix at the descriptor's cam.base+0x00 with
 * the vector (+0x04 low, +0x08 high, +0x04 high) in IR1/IR2/IR3
 * (mobydraw.s:3946-3952), then doubles the result into the GTE translation
 * (mobydraw.s:4058-4060,4190-4192). The record holds quarter-world units, so
 * the translation - and every model vertex added to it - is in world/2: full
 * world units are twice the transformed value, which is what this pass hands
 * the shared walk.
 *
 * The 16-byte geometry header is
 *
 *   +0x00  colour count in byte 0, high-poly vertex count in byte 1,
 *          low-poly vertex count in byte 2
 *   +0x04  four bounding-box bytes, each a multiple of 16 in the cull tests;
 *          its sign selects the low-poly vertex count
 *   +0x08  high-poly face-stream offset in the low half, high-poly colour-block
 *          offset in the high half, both relative to the header
 *   +0x0C  the same pair for the low-poly lists
 *   +0x10  the vertex words, one per vertex
 *
 * (mobydraw.s:4196,4216-4219,4291-4297,4336-4346,4222). A vertex word packs
 * X in bits 11-21 (11 bits), Y in bits 0-11 (12 bits) and Z in bits 21-31
 * (11 bits), absolute, with no accumulator (mobydraw.s:4222-4229).
 *
 * The face stream opens with its own byte count (mobydraw.s:4368). A face is
 * two words, five when word 1's sign marks it textured (mobydraw.s:4421-4431).
 * Word 0 holds four 7-bit vertex indices at bits 21, 14, 7 and 0
 * (mobydraw.s:4387-4390) and a signed ordering-table bias in its top nibble;
 * the face is a triangle when the last two indices match (mobydraw.s:4397).
 * Word 1 holds four 7-bit colour indices at bits 24, 17, 10 and 3
 * (mobydraw.s:4446,4438,4444,4611-4613); its bottom two bits instead select
 * the flat-lit material, whose colour comes from a face normal rather than the
 * colour block (mobydraw.s:4440,4720). The three texture words are the same
 * {u0|v0<<8|clut<<16}, {u1|v1<<8|tpage<<16}, {u2|v2<<8|u3<<16|v3<<24} triple
 * the animated stream uses, with word 1's sign the semi-transparency enable
 * (mobydraw.s:4519-4520,4536).
 *
 * Colours are indexed per vertex out of the header's colour block. Inside the
 * fade band the draw depth-cues each entry of the high-poly block toward the
 * matching entry of the low-poly block by IR0 = 4096 - tier
 * (mobydraw.s:4285-4290,4314-4321) and biases the CLUT row by
 * (4096 - tier) >> 9 (mobydraw.s:4303,4335,4542); past the band it reads the
 * low-poly block directly. */

#define S23_SIMPLE_RECS   63
/* The 63 above is the cursor limit the cull keeps in GTE control 8 and 9
 * (mobydraw.s:44,46), so it bounds what the capture can read. The host queue
 * is sized for the reveal instead, which has no such cursor: a camera high
 * enough over a level admits most of its moby table, and a queue that fills
 * drops the tail of the level silently. */
#define S23_SIMPLE_QCAP   1024
#define S23_SIMPLE_VERTS  256
_Static_assert(S23_SIMPLE_VERTS <= MOBY_SIMPLE_MAX_VERTS, "moby_simple_project_record takes every vertex of a record");
#define S23_SIMPLE_COLS   256
#define S23_SIMPLE_FACES  4096

/* The cull's own queue, copied at the hook. The queue lives in the tick's
 * draw scratch, which the passes after this one build their own data in, so
 * the bytes are taken while they are still the cull's.
 *
 * d and far: the same record's camera deltas and far-cull distance at full
 * width. The cull packs the three deltas as int16 halves and the distance as a
 * halfword, which cannot express a centre or a range past ~131000 world units;
 * the capture fills these by sign-extending its own halves, so a canonical
 * record decodes exactly as it did, while a host-built reveal record carries
 * the numbers untruncated and a far distance of -1 for "no range limit".
 *
 * flat: the four flat-material words the draw keeps in PSX scratchpad at
 * 0x1F8003F0..0x1F8003FC (mobydraw.s:4360-4363, $gp = 0x1F800000). They are
 * reloaded from the table only for a record whose Moby+0x54 is non-zero
 * (mobydraw.s:4350-4352), so a record with a zero index draws with whatever
 * the previous record - or the previous draw call - left there. Snapshotting
 * the words before the game's own draw gives the decode the same entry state
 * that draw started from, and a sub-tick replay the same state again, without
 * ever writing the scratchpad.
 *
 * epoch: the canonical epoch the copy was taken in. */
typedef struct {
    uint32_t q[S23_SIMPLE_QCAP][4];
    int32_t  d[S23_SIMPLE_QCAP][3];
    int32_t  far[S23_SIMPLE_QCAP];
    int      qn;
    uint32_t flat[4];
    uint32_t epoch;
} s23_simple_queue;
/* Two copies: the current canonical epoch's and the previous one's, which a
 * held camera cut replays (s23_simple_replay under moby_native_prev_queue_replay). */
static s23_simple_queue s_sq_buf[2];
static s23_simple_queue* s_sq      = &s_sq_buf[0];
static s23_simple_queue* s_sq_prev = &s_sq_buf[1];

/* While a held cut replays the previous copy: the camera position that copy's
 * cull composed its deltas against. */
static int     s_sq_replaying_prev;
static int32_t s_sq_prev_cam[3];
/* While a late present replays the previous copy's despawned records alone
 * (s23_simple_late_ghosts). */
static int     s_sq_ghost_only;

/** @brief Exchange the current simple-model queue copy for the previous epoch's. */
static void s23_simple_queue_swap(void) {
    s23_simple_queue* t = s_sq; s_sq = s_sq_prev; s_sq_prev = t;
}

/** @brief The camera position the decoded copy's cull composed its deltas against, on a sub-tick present.
 *
 * The installed tick camera's position (moby_native_tick_camera_pos), or
 * during a held cut's replay of the previous copy the camera that copy's
 * cull used.
 * @param out Receives the position when a tick camera is installed.
 * @return 1 on a sub-tick present (a tick camera is installed), else 0. */
static int s23_simple_tick_camera_pos(int32_t out[3]) {
    if (!moby_native_tick_camera_pos(out)) return 0;
    if (s_sq_replaying_prev) memcpy(out, s_sq_prev_cam, sizeof s_sq_prev_cam);
    return 1;
}

/** @brief The moby's own world displacement from the state the decoded record was composed from to the state this present shows.
 *
 * moby_record_world_offset, or during a held cut's replay of the previous
 * copy moby_replay_world_offset, which measures from the object's state as of
 * that copy's cull.
 * @param moby The Moby address.
 * @param out Receives the displacement in world units. */
static void s23_simple_own_motion(uint32_t moby, double out[3]) {
    if (s_sq_replaying_prev) moby_replay_world_offset(moby, out);
    else moby_record_world_offset(moby, out);
}

/** @brief Which simple-model copy holds canonical epoch @p epoch when that epoch is the one before the last committed (cut_held_set_pick).
 *
 * @param epoch The canonical epoch the held present shows.
 * @param pick Receives the pick: the previous copy, the current copy (no
 *        capture in the last committed epoch), or no copy (none in @p epoch).
 * @return 1 when @p epoch is the one before the last committed epoch, else 0. */
static int s23_simple_held_pick(uint32_t epoch, cut_held_pick_t* pick) {
    if (!spyro_native_canon_epoch_fresh(epoch + 1u)) return 0;
    *pick = cut_held_set_pick(s_sq->epoch, s_sq_prev->epoch);
    return 1;
}

/** @brief The mobys of the simple-model queue copy of canonical epoch @p epoch (moby_game.simple_prev_mobys).
 *
 * @param epoch The canonical epoch.
 * @param out Receives up to @p max moby addresses.
 * @param max Capacity of @p out.
 * @return The count written (0 when that epoch captured no queue), or -1 when
 *         @p epoch is not the one before the last committed epoch. */
static int s23_simple_prev_mobys(uint32_t epoch, uint32_t* out, int max) {
    cut_held_pick_t pick;
    if (!out || !s23_simple_held_pick(epoch, &pick)) return -1;
    const s23_simple_queue* c = pick == CUT_HELD_PREV ? s_sq_prev
                              : pick == CUT_HELD_CURR ? s_sq : NULL;
    int k = 0;
    for (int i = 0; c && i < c->qn && k < max; i++) out[k++] = c->q[i][0];
    return k;
}

/** @brief The mobys of the simple-model queue copy of the current canonical epoch (moby_game.simple_mobys).
 *
 * @param out Receives up to @p max moby addresses.
 * @param max Capacity of @p out.
 * @return The count written; 0 when the current copy dates from another
 *         epoch. */
static int s23_simple_mobys(uint32_t* out, int max) OP_REQUIRES(sim) {
    if (!out || s_sq->epoch != spyro_native_canon_epoch_stamp()) return 0;
    int k = 0;
    for (int i = 0; i < s_sq->qn && k < max; i++) out[k++] = s_sq->q[i][0];
    return k;
}

/* Per-frame engagement counts: a pass that walked nothing proves nothing, so
 * the verify harness prints what this one actually read. */
static long s_simple_recs, s_simple_verts, s_simple_faces, s_simple_tris;
static long s_simple_flat, s_simple_screen, s_simple_culled;
/* Records the occlusion test kept from the decode (a subset of s_simple_culled). */
static long s_simple_occl;
/* Screen-space records queued, and the subset of them the placement admitted:
 * the kind's own engagement count, since every other counter mixes the kinds. */
static long s_simple_screen_drawn;
/* Records decoded on the low-poly lists (tier <= 0), and records whose colour
 * block took the depth cue (IR0 > 0). */
static long s_simple_lp, s_simple_fade;
static long s_flat_zero_colour, s_flat_back_emitted, s_flat_back_dropped, s_flat_front_blended;
extern unsigned openpete_frame;

#define S23_FLAT_SCRATCH 0x1F8003F0u

/** @brief Whether a simple-model queue copy holds a record of moby @p moby.
 *
 * @param q The copy.
 * @param moby The Moby address.
 * @return 1 when held, else 0. */
static int s23_simple_queued(const s23_simple_queue* q, uint32_t moby) {
    for (int i = 0; i < q->qn; i++)
        if (q->q[i][0] == moby) return 1;
    return 0;
}

/** @brief Whether the decode skips a queued record for the late switch.
 *
 * A late present shows tick N-1's set: a moby new in tick N's capture is left
 * out (moby_native_late_spawned), and the decode of the previous copy keeps
 * only the mobys that capture evicted which the current copy does not hold,
 * staging each (moby_native_late_ghost_admit).
 * @param moby The record's moby.
 * @return 1 when the record is skipped, else 0. */
static int s23_simple_late_skip(uint32_t moby) {
    if (!s_sq_ghost_only) return moby_native_late_spawned(moby);
    return s23_simple_queued(s_sq_prev, moby) || !moby_native_late_ghost_admit(moby);
}

/** @brief Copies the cull's simple-model queue into the host queue s_sq.
 *
 * Reads records from draw scratch + 0x1C00 (the base the cull left in GTE data
 * 2), stride 0x10, until a zero first word or S23_SIMPLE_RECS records, and
 * sign-extends each record's packed halves into its d and far.
 * Sets its qn to the count. Writes no guest memory. */
static void s23_simple_capture(CPUState* cpu) OP_REQUIRES(sim) {
    /* The first copy of a new canonical epoch retires the current one, reveal
     * records included, to the previous slot; a second copy in the same epoch
     * replaces the current. */
    const uint32_t epoch = spyro_native_canon_epoch_stamp();
    if (s_sq->epoch != epoch) s23_simple_queue_swap();
    s_sq->epoch = epoch;
    s_sq->qn = 0;
    /* The cull left the draw scratch base in GTE data 2 (mobydraw.s:35,3893)
     * and parked this queue 0x1C00 into it (mobydraw.s:44,3895). */
    uint32_t q = cpu->gte_data[2] + 0x1C00u;
    for (int i = 0; i < S23_SIMPLE_RECS; i++, q += 0x10u) {
        uint32_t m = cpu->read_word(q);
        if (m == 0u) break;                     /* the cull's terminator (mobydraw.s:584) */
        s_sq->q[i][0] = m;
        s_sq->q[i][1] = cpu->read_word(q + 4u);
        s_sq->q[i][2] = cpu->read_word(q + 8u);
        s_sq->q[i][3] = cpu->read_word(q + 12u);
        s_sq->d[i][0] = (int16_t)(s_sq->q[i][1] & 0xFFFFu);
        s_sq->d[i][1] = (int16_t)(s_sq->q[i][2] >> 16);
        s_sq->d[i][2] = (int16_t)(s_sq->q[i][1] >> 16);
        s_sq->far[i]  = (int32_t)(s_sq->q[i][2] & 0xFFFFu);
        s_sq->qn = i + 1;
    }
}

/* The pass counters a record's face walk and emit bump, kept per walk worker
 * and summed into the pass counters after the fan-out (s23_simple_decode). */
typedef struct {
    long tris, flat, flat_zero_colour, flat_back_emitted, flat_back_dropped, flat_front_blended;
    long held;   /* triangles held for emission, the decode's return */
} s23_simple_counts;

/** @brief The flat-lit material's face colour, from the normal packed in face word 1 (mobydraw.s:4758-4813).
 *
 * Word 1's bits 7-30 are a
 * face normal instead of four colour indices; the draw rotates it into view by
 * the model matrix, projects it onto the light vector it loads into the GTE
 * colour matrix at entry (all three rows of that matrix are the same vector,
 * mobydraw.s:3874-3880, so the result is one dot product), and blends the two
 * colours of the moby's entry in the flat-colour table by it: the second word
 * scaled by the projection, the first added flat, plus a highlight that opens
 * above 1472. The back side halves the flat word and quarters the projection
 * (mobydraw.s:4790-4795).
 * @param R The model-to-view rotation.
 * @param w1 Face word 1.
 * @param cbase The table word added flat.
 * @param clit The table word scaled by the projection.
 * @param backface 1 for the back side.
 * @return The colour, one byte per channel in bits 0-23. */
static uint32_t s23_simple_flat_colour(const int R[3][3], uint32_t w1,
                                      uint32_t cbase, uint32_t clit, int backface) {
    int n1 = (int8_t)((w1 >> 15) & 0xFFu);
    int n2 = (int8_t)((w1 >>  7) & 0xFFu);
    int n3 = (int8_t)((w1 >> 23) & 0xFFu);
    int ir[3];
    for (int i = 0; i < 3; i++) {
        long long m = (long long)R[i][0]*n1 + (long long)R[i][1]*n2 + (long long)R[i][2]*n3;
        ir[i] = (int)(m >> 12);
    }
    int ir0 = (-1916*ir[0] + -3065*ir[1] + -3072*ir[2]) >> 8;
    if (backface) { ir0 >>= 2; cbase >>= 1; }
    int hi = ir0 > 1472 ? (ir0 - 1472) << 3 : 0;
    int out = 0;
    for (int ch = 0; ch < 3; ch++) {
        int shift = ch * 8;
        int base = (int)((cbase >> shift) & 0xFFu) << 4;
        int lit  = (int)((clit  >> shift) & 0xFFu) << 4;
        int v = base + ((ir0 * lit) >> 12) + hi;
        if (v < 0) v = 0; else if (v > 4095) v = 4095;
        out |= (v >> 4) << shift;
    }
    return (uint32_t)out;
}

/* What one face contributes: which of the two triangle halves survive the
 * winding tests, whether the face draws textured, its blend mode, and the
 * colour each of its four vertex slots takes. */
typedef struct {
    int      emit_a, emit_b;   /* the halves (v0,v1,v2) and (v1,v3,v2) */
    int      textured;
    int      blend;
    uint32_t col[4];
} s23_shade;

/** @brief The flat-lit material's face treatment: the winding verdict, the near-band blend and one colour.
 *
 * Two-sided, untextured F3/F4 whatever
 * the face's own texture words say, and one colour off a face normal instead of
 * four out of the colour block (mobydraw.s:4720-4724,4756-4765,4849-4862).
 *
 * The near band is the record's translation Z, less 0x800 when word 1 bit 0 is
 * set, below zero (mobydraw.s:4750-4755). Inside it the front face draws
 * semi-transparent and the back face opaque; outside it the front face draws
 * opaque and the back face is dropped. The draw builds the primitive code as
 * 0x2A/0x22 (F4/F3 with ABE) less t5 (mobydraw.s:4854-4863): t5 is the back
 * bit shifted to 0x2000000 (mobydraw.s:4748-4749), and outside the band the
 * `lui t5, 0x200` in the delay slot of the back-face drop sets it for the front
 * face too (mobydraw.s:4757-4758). The draw takes the back-facing verdict from the sign
 * of the NCLIP register it carries into the emit (mobydraw.s:4747-4748, srl/sll
 * 31/25 at 0x80034930), which the four-way test above it sets: a triangle keeps
 * its own NCLIP and drops on zero (mobydraw.s:4714-4718); a quad forces +1 when
 * the second NCLIP is positive, forces -1 when the first is non-negative and the
 * second negative, drops when the first is non-negative and the second zero, and
 * otherwise keeps the first, which is negative (mobydraw.s:4719-4746). Every
 * surviving quad case therefore reduces to: front exactly when the second NCLIP
 * is positive.
 * @param R The model-to-view rotation the normal rotates by.
 * @param f1 Face word 1.
 * @param tri 1 for a triangle.
 * @param nA The facing value of (v0,v1,v2) (moby_simple_face_nclips).
 * @param nB The facing value of (v3,v1,v2), on a quad.
 * @param trz The record's translation depth the near band is tested against.
 * @param flat_w The four material words in force.
 * @param n The walking worker's counters.
 * @param sh Receives the halves, the blend flag and the colour; `textured` is cleared.
 * @return 0 when the face emits nothing, 1 otherwise. */
static int s23_simple_flat_face(const int R[3][3], uint32_t f1, int tri,
                                double nA, double nB, double trz,
                                const uint32_t flat_w[4], s23_simple_counts* n, s23_shade* sh) {
    n->flat++;
    sh->textured = 0;
    int back;
    if (tri) {
        if (nA == 0.0) return 0;
        back = (nA < 0.0);
        sh->emit_a = 1;
    } else {
        if (nA >= 0.0 && nB == 0.0) return 0;
        back = !(nB > 0.0);
        sh->emit_a = sh->emit_b = 1;
    }
    /* Near band (mobydraw.s:4750-4758): the back face survives only inside it
     * and draws opaque there; the front face blends only inside it. */
    int in_band = (trz - ((f1 & 1u) ? 2048.0 : 0.0) < 0.0);
    if (back) {
        if (!in_band) { n->flat_back_dropped++; return 0; }
        n->flat_back_emitted++;
    } else if (in_band) {
        n->flat_front_blended++;
        sh->blend = 1;
    }
    /* Bit 0 picks which of the entry's two colour pairs (mobydraw.s:4772-4776). */
    uint32_t c = s23_simple_flat_colour(R, f1, flat_w[(f1 & 1u) ? 0 : 2],
                                        flat_w[(f1 & 1u) ? 1 : 3], back);
    if ((c & 0xFFFFFFu) == 0u) n->flat_zero_colour++;
    sh->col[0] = sh->col[1] = sh->col[2] = sh->col[3] = c;
    return 1;
}

/** @brief One simple-model face's material, winding verdict and colours, the step between the vertex walk and the emit.
 *
 * Word 1 bit 1 selects the flat-lit material (s23_simple_flat_face); bit 0
 * alone selects neither (mobydraw.s:4721,4666). A standard quad keeps each
 * half on its own NCLIP sign test and a standard triangle keeps on a positive
 * NCLIP; its four colours are word 1's 7-bit indices into `ctab`, an index
 * past `ncol` taking entry 0.
 * @param R The model-to-view rotation the flat material's normal rotates by.
 * @param f1 Face word 1; its sign is the textured flag.
 * @param tri 1 for a triangle.
 * @param nA The facing value of (v0,v1,v2) (moby_simple_face_nclips).
 * @param nB The facing value of (v3,v1,v2), on a quad.
 * @param trz The record's translation depth the near band is tested against.
 * @param flat_w The four material words in force.
 * @param ctab The record's decoded colours.
 * @param ncol The number of entries in `ctab`.
 * @param n The walking worker's counters.
 * @param sh Receives the halves, the textured and blend flags and the colours.
 * @return 0 when the face emits nothing, 1 otherwise. */
static int s23_simple_shade_face(const int R[3][3], uint32_t f1, int tri,
                                 double nA, double nB, double trz,
                                 const uint32_t flat_w[4],
                                 const uint32_t* ctab, int ncol,
                                 s23_simple_counts* n, s23_shade* sh) {
    sh->emit_a = sh->emit_b = 0;
    sh->textured = ((int32_t)f1) < 0;
    sh->blend = 0;
    if (f1 & 2u) return s23_simple_flat_face(R, f1, tri, nA, nB, trz, flat_w, n, sh);

    if (!tri) {
        /* mobydraw.s:4444-4460,4529-4533: the quad's two halves keep on opposite
         * NCLIP signs, the Z vertex order giving them opposite winding. */
        if (nA >= 0.0) { if (nB > 0.0) sh->emit_b = 1; }
        else if (nB > 0.0) { sh->emit_a = sh->emit_b = 1; }
        else sh->emit_a = 1;
    } else if (nA > 0.0) {
        sh->emit_a = 1;                          /* mobydraw.s:4592-4600 */
    }
    if (!sh->emit_a && !sh->emit_b) return 0;
    int ci[4] = { (int)((f1 >> 24) & 0x7Fu), (int)((f1 >> 17) & 0x7Fu),
                  (int)((f1 >> 10) & 0x7Fu), (int)((f1 >>  3) & 0x7Fu) };
    for (int k = 0; k < 4; k++)
        sh->col[k] = ctab[ci[k] < ncol ? ci[k] : 0];
    return 1;
}

/* Where one queue record puts its model: the translation the vertex walk adds,
 * the matrix the moby's own rotations compose onto, the geometry header, and -
 * for the screen-space kind - the projection origin the record names. The two
 * kinds part at one branch on the record (mobydraw.s:3915) and rejoin at the
 * shared vertex and face walk (mobydraw.s:4036), so everything between them is
 * here and nothing else in the decode differs. */
typedef struct {
    double   TR[3];     /* the GTE translation, world/2 (mobydraw.s:4039-4041) */
    int      R[3][3];   /* the matrix installed before the per-moby rotations */
    uint32_t header;    /* the 16-byte geometry header at slot+0x14 */
    int      screen;    /* the screen-space kind */
    double   ofx, ofy;  /* its projection origin, PSX px */
} s23_place;

/* Records whose camera-relative base s23_simple_base took at full precision
 * (exact) or left on the cull's truncated value (kept), indexed by the decode
 * kind: 0 canonical, 1 sub-tick. Cumulative since startup. */
static _Atomic(long) s_simple_base_exact[2], s_simple_base_kept[2];

/** @brief Whether OPENPETE_SIMPLE_RECORD_BASE keeps every simple-moby base on the cull's truncated record, read once.
 *
 * The in-binary A/B of s23_simple_base: 1 restores the record's quarter-world
 * deltas as the base on every decode.
 * @return Nonzero when the record's base is kept. */
static int s23_simple_record_base_forced(void) {
    static _Atomic(int) s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_SIMPLE_RECORD_BASE"); s = (e && *e && *e != '0'); }
    return s;
}

/** @brief A world-placed simple-moby record's camera-relative base, in its quarter-world units and axis order, at full precision.
 *
 * The cull stores (camY - mobyY) >> 2, (camZ - mobyZ) >> 2 and
 * (mobyX - camX) >> 2 (mobydraw.s:81,87,94), so the record's base carries the
 * floor of a quarter of the camera-relative position. The decode adds the
 * camera's and the moby's motion since the tick to that base at full
 * precision, so a present at phase t shows the truncated tick position minus
 * the untruncated motion: each tick's sub-tick run ends on the truncated
 * position, and the change of the dropped residue (up to three world units
 * per axis) appears as a step at every tick boundary. This divides the same
 * differences by 4.0 instead.
 *
 * The camera is the one the record's cull subtracted: the installed tick
 * camera on a sub-tick present (s23_simple_tick_camera_pos), else the
 * camera position words. The moby position is the moby's at the record's
 * tick: its position words, less the displacement moby_record_world_offset
 * adds to reach this present and plus the own motion the decode adds
 * (@p dmob), which on a previous-queue replay also carries the object's
 * change since that queue's cull. The base replaces the record's only when
 * its floor is the record's on all three axes, the proof that both inputs are
 * the ones the cull read; a reveal record built from the same words passes,
 * and a record whose halves wrapped or whose moby has moved on keeps its own.
 * @param cpu The guest CPU state, read only.
 * @param r The queue index.
 * @param bracketed 1 on a sub-tick present.
 * @param dmob The moby's own displacement over this present, world units.
 * @param base Receives the base.
 * @return 1 when the base is the full-precision one, 0 when it is the record's. */
static int s23_simple_base(CPUState* cpu, int r, int bracketed, const double dmob[3], double base[3]) {
    for (int i = 0; i < 3; i++) base[i] = (double)s_sq->d[r][i];
    if (s23_simple_record_base_forced()) return 0;
    const uint32_t moby = s_sq->q[r][0];
    int32_t cam[3];
    if (!s23_simple_tick_camera_pos(cam))
        for (int i = 0; i < 3; i++) cam[i] = (int32_t)cpu->read_word(game_layout.cam.position + 4u * (uint32_t)i);
    double mw[3] = { 0.0, 0.0, 0.0 };
    if (bracketed) moby_record_world_offset(moby, mw);
    double p[3];
    for (int i = 0; i < 3; i++)
        p[i] = (double)(int32_t)cpu->read_word(moby + game_layout.moby.pos_off + 4u * (uint32_t)i)
             + (bracketed ? mw[i] - dmob[i] : 0.0);
    const double e[3] = { ((double)cam[1] - p[1]) * 0.25,
                          ((double)cam[2] - p[2]) * 0.25,
                          (p[0] - (double)cam[0]) * 0.25 };
    for (int i = 0; i < 3; i++)
        if (floor(e[i]) != base[i]) return 0;
    memcpy(base, e, sizeof e);
    return 1;
}

/** @brief OPENPETE_SIMPLE_BASE_CENSUS: print the cumulative s23_simple_base counts, at most one line per VBL.
 *
 * The line carries the world-placed records whose base was the full-precision
 * one and those that kept the record's, on canonical and sub-tick decodes. */
static void s23_simple_base_census(void) {
    static _Atomic(int) on = -1;
    static _Atomic(unsigned) last = ~0u;
    if (on < 0) { const char* e = getenv("OPENPETE_SIMPLE_BASE_CENSUS"); on = (e && *e && *e != '0'); }
    if (!on || last == openpete_frame) return;
    last = openpete_frame;
    fprintf(stderr, "[simple-base vbl=%u] canon exact=%ld kept=%ld sub exact=%ld kept=%ld\n", openpete_frame,
            (long)s_simple_base_exact[0], (long)s_simple_base_kept[0],
            (long)s_simple_base_exact[1], (long)s_simple_base_kept[1]);
}

/** @brief Places one queued simple-moby record and reports whether it draws.
 *
 * Fills the translation, matrix, geometry header and, for the screen-space
 * kind, the projection origin.
 *
 * The far-cull distance's bit 15 sorts the record into the screen-space kind,
 * which takes its depth and screen offsets from the record itself (dropped at
 * a non-positive depth) and skips the camera transform and the range and
 * bounding-box tests. The other kind transforms its camera-relative base,
 * moved by the camera's and the moby's motion since the tick, by the camera
 * matrix and is dropped past its far distance (unless
 * extended LOD is on) or when the header's four bounding-box bytes fail the
 * draw's frustum tests. Either kind is dropped when the geometry header at
 * slot + 0x14 is not a KSEG0 pointer. `pl->screen` is written before any
 * return.
 * @param r The index into the host queue.
 * @param Rcam The camera matrix, Q12.
 * @param dcam The camera-position change since the tick in the record's
 *        quarter-world units, per axis (X, Y, Z); zero on a canonical decode.
 * @param dmob The moby's displacement over the present's fraction of the tick,
 *        in world units; zero on a canonical decode.
 * @param base The world-placed kind's camera-relative base at the tick, in
 *        the record's quarter-world units and axis order (s23_simple_base).
 * @param pl Receives the placement; valid only on a return of 1.
 * @return 1 when the record draws, 0 when it draws nothing. */
static int s23_simple_place(CPUState* cpu, int r, const int Rcam[3][3],
                            const double dcam[3], const double dmob[3],
                            const double base[3], s23_place* pl) {
    const uint32_t slot = s_sq->q[r][3];
    /* The far-cull distance's bit 15 sorts the record (mobydraw.s:3914-3915). */
    pl->screen = (s_sq->q[r][2] & 0x8000u) != 0;
    pl->ofx = pl->ofy = 0.0;
    memcpy(pl->R, Rcam, sizeof pl->R);
    double m1 = 0.0, m2 = 0.0, m3 = 0.0;

    if (pl->screen) {
        /* The screen-space kind (mobydraw.s:4010-4022). The record's depth is
         * the high half of +0x08 and a non-positive one drops the record
         * (mobydraw.s:4011). The two halves of +0x04 go into the GTE screen
         * offsets, high into OFX and low into OFY, each scaled by 65536 into
         * that register's 16.16 pixels (mobydraw.s:4016-4019). The view X and
         * Y translation are zero (mobydraw.s:4020-4021) and the Z translation
         * is the depth over four (mobydraw.s:4022), all three doubled at the
         * rejoin (mobydraw.s:4039-4041); the depth keeps its fraction here,
         * where the draw shifts it right 2, so the size does not step on a
         * 4-unit depth grid as the moby's own motion moves it. No camera rotation, no range test and
         * no bounding-box test: the matrix is the constant the branch installs,
         * the identity with its Y row at 0xA00 (mobydraw.s:4024-4028,
         * 4031-4035). */
        /* The cull builds this record's three halves straight out of the moby's
         * own position words at +0x0C, +0x10 and +0x14 (mobydraw.s:606-608 the
         * loads, :621-626 the halves of +0x04, :627-628 the depth half), so the
         * moby's own displacement enters each of them directly: X into OFX, Y
         * into OFY and Z into the depth, in the same units and with no sign
         * change. */
        const double dz = (double)s_sq->d[r][1] + dmob[2];
        if (dz <= 0.0) return 0;
        pl->ofx = (double)s_sq->d[r][2] + dmob[0];
        pl->ofy = (double)s_sq->d[r][0] + dmob[1];
        pl->TR[0] = pl->TR[1] = 0.0;
        pl->TR[2] = dz * 0.5;
        memset(pl->R, 0, sizeof pl->R);
        pl->R[0][0] = 0x1000; pl->R[1][1] = 0x0A00; pl->R[2][2] = 0x1000;
    } else {
        /* view-space centre, world/2 after the doubling (mobydraw.s:3946-3952,4058-4060).
         * The record's deltas are (cam - moby) >> 2 on the first two components
         * and (moby - cam) >> 2 on the third (mobydraw.s:81,87,94), so the
         * moby's own displacement enters in quarter-world units with the
         * opposite sign per component, beside the camera's. */
        ngte_mac(Rcam, base[0] + dcam[1] - dmob[1] * 0.25,
                       base[1] + dcam[2] - dmob[2] * 0.25,
                       base[2] + dcam[0] + dmob[0] * 0.25, &m1, &m2, &m3);
        /* The record's own draw range (mobydraw.s:3958). Extended LOD removes
         * the range rather than widening it: the threshold is a per-moby byte
         * scaled by 256, so any raised value is still a ceiling a high camera
         * clears, while terrain carries no distance term at all. */
        if (!openpete_no_lod && s_sq->far[r] >= 0 &&
            m3 - (double)s_sq->far[r] >= 0.0) return 0;
        pl->TR[0] = m1*2.0; pl->TR[1] = m2*2.0; pl->TR[2] = m3*2.0;
    }

    /* A header outside RAM is not a pointer, and the vertex and face walks read
     * through it. */
    pl->header = cpu->read_word(slot + 0x14u);
    if (pl->header < 0x80000000u) return 0;
    if (pl->screen) return 1;

    /* The cull tests the draw repeats, on the header's four bounding-box
     * bytes scaled by 16 (mobydraw.s:3963-3975,3976-3981,3993-4002). */
    const uint32_t h1 = cpu->read_word(pl->header + 4u);
    double bx3 = (double)((h1 >> 20) & 0xFF0u), bx2 = (double)((h1 >> 12) & 0xFF0u);
    double bx1 = (double)((h1 >>  4) & 0xFF0u), bx0 = (double)((h1 <<  4) & 0xFF0u);
    double ax = m1 < 0 ? -m1 : m1, ay = m2 < 0 ? -m2 : m2;
    /* The lateral arm's 3:4 slope is the draw's 4:3 half-angle
     * (mobydraw.s:3976-3981), so the frame's own horizontal widening scales
     * it; the vertical arm's 1:3 slope (Spyro 3 mobydraw.s:3993-4002,
     * 0x80033E58-0x80033E74; Spyro 2 :3997-4006) is the 4:3 frame's half-height, scaled the same
     * way by the vertical widening. At the game's FOV with 4:3 both factors
     * are 1.0 and the tests are the draw's, bit for bit; Hor+ alone leaves
     * the vertical factor at 1.0. */
    if (4.0*(ax - bx3) - 3.0*moby_native_view_widen()*(m3 + bx2) >= 0.0) return 0;
    if (m3 + bx1 <= 0.0) return 0;
    if (moby_native_view_vwiden()*(m3 + bx0) - 3.0*(ay - bx1) <= 0.0) return 0;
    return 1;
}

/** @brief One simple-model face's bucket word in the record's paint order.
 *
 * moby_rank.h rows MOBY_OTK_S23_SIMPLE and MOBY_OTK_S23_SIMPLE_FLAT:
 * face word 1 bit 1 selects the flat-lit branch (mobydraw.s 4720-4722,
 * 4743-4745), whose back faces the walk keeps while TRZ, less 0x800 under word
 * 1 bit 0, is negative (4748-4760). A standard face's bias is the signed top
 * nibble of word 0 (4436, 4584), and word 1 bit 0 lets a standard triangle
 * pass the facing test either way round (4602-4605, 4743-4745).
 * @param ork The record's two row states, standard then flat-lit.
 * @param iv The record's per-vertex key inputs.
 * @param idx The face's four vertex indices.
 * @param tri 1 for a triangle; idx[3] is then unused.
 * @param f0 Face word 0.
 * @param f1 Face word 1.
 * @return The bucket word moby_otk_face_bucket returns. */
static int32_t s23_simple_face_otk(const moby_otk_rec_t ork[2], const moby_otk_vert_t* iv,
                                   const int idx[4], int tri, uint32_t f0, uint32_t f1) {
    const int flat = (f1 & 2u) != 0;
    moby_otk_face_t f = { { &iv[idx[0]], &iv[idx[1]], &iv[idx[2]], tri ? NULL : &iv[idx[3]] },
                          flat ? 0 : (int32_t)f0 >> 28,
                          flat ? ((f1 & 1u) ? 0x800 : 0) : INT32_MIN,
                          (int8_t)(!flat && tri && (f1 & 1u)), (int8_t)flat, 0 };
    return moby_otk_face_bucket(&ork[flat], &f);
}

/* The texture page word the draw links ahead of each record's spliced bucket
 * array, 0xE1000620 (S2 mobydraw.s:4961-4962, S3 mobydraw.s:4980-4981): its
 * ABR field, bits 5-6, is 1. */
#define S23_RECORD_TPAGE_ABR (((0x620u) >> 5) & 3u)

/* One face of a record's face stream as the serial prefix reads it: words 0
 * and 1, and on a face whose word 1 is negative (the textured stride) words
 * 2 to 4, the texels, CLUT and texture page; zero otherwise. */
typedef struct { uint32_t f0, f1, t0, t1, t2; } s23_face_words;

/* One record as its face walk, resolve and emit read it after the serial
 * prefix (s23_simple_decode): everything the prefix read from guest memory or
 * from the pass state, so the job reads neither. The vertex, paint-key,
 * colour and face runs are offsets into the decode's pools (s_s23_pool). */
typedef struct {
    int      R[3][3];       /* the composed rotation, Q12 */
    int      itr[3];        /* the integer translation the draw's RTPS loads */
    double   trz;           /* the translation Z the flat-lit near band tests */
    double   ofx, ofy, H;   /* the view's screen offset and H for the flush's RTPS */
    moby_otk_rec_t ork[2];  /* the face walk's paint-key rows, standard then flat-lit */
    uint32_t flat_w[4];     /* the flat-material words in force for this record */
    int      nv, ncol, clut_bias;
    int      key;           /* the record's world bucket on the depth scale */
    int      single;        /* 1 when the record's colour index is non-zero */
    int      vm_off, iv_off, col_off, face_off, face_n;
    float    Rf[3][3];      /* the rotation at unit scale the projection places the vertices with */
    double   TR[3];         /* the projection's translation, world / 2 */
    int      deferred;      /* 1 when the job projects the record (moby_simple_project_defer) */
    moby_simple_proj_t pj;  /* the deferred projection */
} s23_simple_job;
/* The records of the decode in progress and their emit scopes, in queue order. */
static s23_simple_job      s_s23_job[S23_SIMPLE_QCAP];
static moby_simple_scope_t s_s23_scope[S23_SIMPLE_QCAP];

/* One record's faces as the semi-transparency resolve reads them, in face
 * stream order. An untextured semi-transparent primitive (the flat-lit front
 * face inside the near band) carries no texture page, so the GPU blends it
 * at the ABR of the last texture page set before it in the chain: the
 * record's own DR_TPAGE, or the page of a textured primitive spliced ahead
 * of it. */
typedef struct {
    int      idx[4];
    int      tri;
    uint32_t f0, f1;
    int8_t   tex_abr;   /* the page ABR of a face emitted textured, else -1 */
    int8_t   semi;      /* an emitted untextured semi-transparent face */
} s23_abr_face;

/* One triangle held until the record's faces are resolved. */
typedef struct {
    int      vi[3];     /* the corners: vertex indices into the record's projection */
    uint32_t rgb[3];
    int      u[3], v[3];
    int      textured, tpx, tpy, clx, cly, bpp, blend;
    uint32_t fkey;
    int32_t  otk;
    int      face;
} s23_held_tri;

/* One walk worker's scratch for the face walk, resolve and emit of the record
 * it is running, and its share of the pass counters. Index
 * MOBY_SIMPLE_CHECK_WORKER is the identity check's serial run, whose counters
 * are not summed. */
typedef struct {
    OPENPETE_WORKER_ALIGN s23_abr_face face[S23_SIMPLE_FACES];
    int          face_n;
    s23_held_tri held[2 * S23_SIMPLE_FACES];
    int          held_n;
    int          semi_n;
    moby_otk_vert_t kv[S23_SIMPLE_VERTS];   /* the flush's integer RTPS */
    int32_t      bucket[S23_SIMPLE_FACES], entry[S23_SIMPLE_FACES];
    int8_t       abr[S23_SIMPLE_FACES];
    s23_simple_counts n;
} s23_simple_ws;
OPENPETE_WORKER_ELEM(s23_simple_ws);
static s23_simple_ws s_ws[MOBY_SIMPLE_WORKERS + 1];

/** @brief Start a record's semi-transparency resolve with no faces and no held triangles.
 * @param W The worker's scratch. */
static void s23_abr_begin(s23_simple_ws* W) {
    W->face_n = 0;
    W->held_n = 0;
    W->semi_n = 0;
}

/** @brief Note one face of the record in face stream order, with its texture page ABR when textured.
 *
 * Every face is noted, drawn natively or not, since a textured primitive the
 * draw keeps sets the GPU texture page whatever the native winding test
 * decides. The draw emits a textured GT3/GT4 only on the standard branch
 * with the texture bit set: word 1 bit 1 sends the face to the flat-lit
 * branch (S2 mobydraw.s 0x80047C4C/0x80047C9C, S3 0x800348D0/0x80034920),
 * which emits an untextured F3/F4 whatever the texture bit says, so a flat
 * face sets no page; the standard branch takes the GT code when word 1 is
 * negative (S2 0x800478EC/0x80047AFC, S3 0x80034570/0x80034780).
 * @param W The worker's scratch.
 * @param fw The face's words; word 3 carries the texture page of a textured face.
 * @param idx The face's four vertex indices.
 * @param tri 1 for a triangle.
 * @return The face's index in the record. */
static int s23_abr_note_face(s23_simple_ws* W, const s23_face_words* fw, const int idx[4], int tri) {
    if (W->face_n >= S23_SIMPLE_FACES) return S23_SIMPLE_FACES - 1;
    s23_abr_face* F = &W->face[W->face_n];
    memcpy(F->idx, idx, sizeof F->idx);
    F->tri = tri; F->f0 = fw->f0; F->f1 = fw->f1; F->semi = 0;
    F->tex_abr = -1;
    if ((int32_t)fw->f1 < 0 && !(fw->f1 & 2u)) {
        ngte_tpage t;
        ngte_decode_tpage((fw->t1 >> 16) & 0x7FFFu, 0u, 0u, &t);
        F->tex_abr = (int8_t)t.abr;
    }
    return W->face_n++;
}

/** @brief Hold one triangle of the record for emission after the resolve.
 *
 * An untextured triangle with a nonzero blend marks its face semi-transparent;
 * its blend is replaced at the resolve.
 * @param W The worker's scratch.
 * @param face The face's index from s23_abr_note_face.
 * @param T The triangle, every moby_simple_tri argument. */
static void s23_abr_hold(s23_simple_ws* W, int face, const s23_held_tri* T) {
    if (W->held_n >= 2 * S23_SIMPLE_FACES) return;
    W->held[W->held_n] = *T;
    W->held[W->held_n].face = face;
    if (!T->textured && T->blend && !W->face[face].semi) {
        W->face[face].semi = 1;
        W->semi_n++;
    }
    W->held_n++;
}

/** @brief The ABR in force when a face's primitive draws: the last texture page set before it in the record's chain.
 *
 * The draw splices the record's bucket array highest bucket first, the
 * primitives of one bucket in insertion order, which is face stream order
 * (moby_rank.h), behind the record's own DR_TPAGE. A face precedes face i
 * when its bucket is higher, or equal with a lower face index. The textured
 * face that precedes i last sets the ABR; with none, the DR_TPAGE's does. A
 * face the draw drops (bucket -1) sets nothing; dropped itself, face i reads
 * the state after every kept face.
 * @param W The worker's scratch: the record's noted faces and their local
 *        buckets, -1 where the draw drops a face.
 * @param i The face.
 * @return The ABR, 0 to 3. */
static int s23_abr_in_force(const s23_simple_ws* W, int i) {
    const int32_t* bucket = W->bucket;
    int best = -1;
    for (int j = 0; j < W->face_n; j++) {
        if (W->face[j].tex_abr < 0 || bucket[j] < 0 || j == i) continue;
        if (!(bucket[j] > bucket[i] || (bucket[j] == bucket[i] && j < i))) continue;
        if (best < 0 || bucket[j] < bucket[best] || (bucket[j] == bucket[best] && j > best))
            best = j;
    }
    return best < 0 ? (int)S23_RECORD_TPAGE_ABR : W->face[best].tex_abr;
}

/** @brief Resolve each semi-transparent face's blend class from the ABR in force, then emit the record's held triangles with their world buckets.
 *
 * The buckets come from the draw's own integer RTPS and key arithmetic for
 * this record (moby_otk_record, moby_otk_rtps), evaluated here whether or not
 * the paint rank is on, so the verdict is the same on every present. The
 * screen offset does not enter NCLIP or the key; H is the GTE's.
 *
 * Each held triangle carries the world ordering-table bucket the draw links
 * its face into (moby_simple_world_bucket). A record whose Moby+0x54 colour
 * index is non-zero links every prim into the world bucket `J->key`
 * (mobydraw.s 0x80047F8C-0x80048014; the index clears the sign bit of $t9 at
 * 0x8004771C-0x80047720, which selects that branch at 0x80047EC4); one whose
 * index is zero splices its local bucket array down from that key in runs of
 * eight entries (0x80047ED0-0x80047F88), moby_otk_splice_bucket with shift 2.
 * The key is on the depth scale: the record's ordering-table key without the
 * draw's move of 0x20 buckets at 0x110 and beyond (moby_otk_depth_bucket). A
 * face the draw drops keeps no bucket.
 * @param se The emit context bound to the record.
 * @param W The worker's scratch, holding the record's noted faces and held triangles.
 * @param J The record.
 * @param vmod The record's model-space vertices.
 * @return The number of triangles emitted. */
static int s23_abr_flush(moby_simple_emit* se, s23_simple_ws* W, const s23_simple_job* J,
                         const double (*vmod)[3]) {
    moby_otk_rec_t ork[2];
    moby_otk_record(MOBY_OTK_S23_SIMPLE, 0, J->itr[2], &ork[0]);
    moby_otk_record(MOBY_OTK_S23_SIMPLE_FLAT, 0, J->itr[2], &ork[1]);
    for (int k = 0; k < J->nv; k++)
        moby_otk_rtps(J->itr, (const int (*)[3])J->R, vmod[k][0], vmod[k][1], vmod[k][2],
                      J->ofx, J->ofy, (int)J->H, &W->kv[k]);
    int ltop = -1;
    for (int j = 0; j < W->face_n; j++) {
        const s23_abr_face* F = &W->face[j];
        const int32_t b = s23_simple_face_otk(ork, W->kv, F->idx, F->tri, F->f0, F->f1);
        W->bucket[j] = b < 0 ? -1 : MOBY_OTK_BUCKET(b);
        W->entry[j] = moby_otk_array_entry(&ork[0], b);
        ltop = moby_otk_top_entry(&ork[0], b, ltop);
    }
    if (W->semi_n > 0) {
        for (int j = 0; j < W->face_n; j++)
            W->abr[j] = W->face[j].semi ? (int8_t)s23_abr_in_force(W, j) : 0;
        for (int t = 0; t < W->held_n; t++)
            if (W->face[W->held[t].face].semi && !W->held[t].textured)
                W->held[t].blend = moby_abr_blend(W->abr[W->held[t].face]);
    }
    for (int t = 0; t < W->held_n; t++) {
        const s23_held_tri* T = &W->held[t];
        const int32_t l = W->entry[T->face];
        moby_simple_world_bucket(se, l < 0 ? -1 : J->single ? J->key
                                                 : moby_otk_splice_bucket(J->key, 2, ltop, l));
        moby_simple_tri(se, T->vi, T->rgb, T->u, T->v, T->textured, T->tpx, T->tpy,
                        T->clx, T->cly, T->bpp, T->blend, T->fkey, T->otk);
    }
    return W->held_n;
}

/* Model radii of simple-model geometry headers, direct mapped. An entry holds
 * the header address with its first two words and first vertex word, so a
 * header rewritten in place (a level load) misses instead of reusing a radius. */
#define S23_RADIUS_N 512
static struct { uint32_t H, h0, h1, v0; double r; } s_s23_radius[S23_RADIUS_N];

/** @brief The largest distance of a simple model's vertex words from its origin, model units.
 *
 * Covers the vertex counts of both lists (header word 0 bits 8-15 and 16-23,
 * the counts the decode reads its vertex words by), so the bound holds at
 * every tier. Cached per header.
 * @param cpu The guest CPU state, read only.
 * @param H The geometry header.
 * @return The radius, model units. */
static double s23_simple_model_radius(CPUState* cpu, uint32_t H) {
    const uint32_t h0 = cpu->read_word(H), h1 = cpu->read_word(H + 4u), v0 = cpu->read_word(H + 0x10u);
    const unsigned slot = ((H >> 2) * 2654435761u >> 20) & (S23_RADIUS_N - 1);
    if (s_s23_radius[slot].H == H && s_s23_radius[slot].h0 == h0 &&
        s_s23_radius[slot].h1 == h1 && s_s23_radius[slot].v0 == v0)
        return s_s23_radius[slot].r;
    int n = (int)((h0 >> 8) & 0xFFu), nl = (int)((h0 >> 16) & 0xFFu);
    if (nl > n) n = nl;
    if (n > S23_SIMPLE_VERTS) n = S23_SIMPLE_VERTS;
    double r2 = 0.0;
    for (int i = 0; i < n; i++) {
        const uint32_t w = cpu->read_word(H + 0x10u + (uint32_t)i * 4u);
        const double vx = (double)((int32_t)(w << 10) >> 21);
        const double vy = (double)((int32_t)(w << 20) >> 20);
        const double vz = (double)((int32_t)w >> 21);
        const double d2 = vx * vx + vy * vy + vz * vz;
        if (d2 > r2) r2 = d2;
    }
    s_s23_radius[slot].H = H; s_s23_radius[slot].h0 = h0;
    s_s23_radius[slot].h1 = h1; s_s23_radius[slot].v0 = v0;
    s_s23_radius[slot].r = sqrt(r2);
    return s_s23_radius[slot].r;
}

/** @brief A bound on how far a unit-scale matrix stretches any vector: the square root of the largest absolute row sum of R^T R.
 *
 * Every induced norm of R^T R bounds its largest eigenvalue, which is the
 * square of R's spectral norm, so the result is never below the true
 * stretch; for the near-orthonormal camera-times-rotation matrices it is
 * within a fraction of a percent of it.
 * @param R The matrix, 1.0 = 1.0.
 * @return The bound, 1.0 = no stretch. */
static double s23_matrix_stretch(const float R[3][3]) {
    double worst = 0.0;
    for (int i = 0; i < 3; i++) {
        double row = 0.0;
        for (int j = 0; j < 3; j++) {
            double d = 0.0;
            for (int k = 0; k < 3; k++) d += (double)R[k][i] * (double)R[k][j];
            row += fabs(d);
        }
        if (row > worst) worst = row;
    }
    return sqrt(worst);
}

/** @brief Whether OPENPETE_MOBY_POSTRACE_CLS names a class to trace, read once.
 * @return 1 when s23_simple_postrace prints for some class. */
static int s23_postrace_on(void) {
    static int on = -1;
    if (on < 0) { const char* e = getenv("OPENPETE_MOBY_POSTRACE_CLS"); on = e && strtol(e, NULL, 0) >= 0; }
    return on;
}

/** @brief Print the submitted screen-space centroid, extent and placement of one simple-moby record under OPENPETE_MOBY_POSTRACE_CLS.
 *
 * The variable names the class to trace (0xFFFF traces every class), read
 * the same way as the animated models' trace in the shared feeder, so one
 * setting follows a moby across the kind boundary. The centroid alone is
 * blind to the rotation, so the projected extent and the composed matrix's
 * first row are printed beside it.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param vscr The record's projected vertices.
 * @param nv Their count, at least 1.
 * @param R The record's composed matrix, Q12.
 * @param rot The moby's rotation word.
 * @param dmob The moby's own displacement over this present, world units. */
static void s23_simple_postrace(CPUState* cpu, uint32_t moby, const float (*vscr)[2], int nv,
                                const int R[3][3], uint32_t rot, const double dmob[3]) {
    static int s_cls = -2;
    if (s_cls == -2) { const char* e = getenv("OPENPETE_MOBY_POSTRACE_CLS"); s_cls = e ? (int)strtol(e, NULL, 0) : -1; }
    /* The class read stays inside the enable: the decode calls this per
     * record per present. */
    if (s_cls < 0) return;
    const uint16_t s_tcls = (uint16_t)cpu->read_half(moby + game_layout.moby.class_off);
    if (s_cls != 0xFFFF && s_tcls != (uint16_t)s_cls) return;
    double cx = 0.0, cy = 0.0;
    float lo[2] = { vscr[0][0], vscr[0][1] }, hi[2] = { lo[0], lo[1] };
    for (int i = 0; i < nv; i++) {
        cx += vscr[i][0]; cy += vscr[i][1];
        for (int k = 0; k < 2; k++) {
            if (vscr[i][k] < lo[k]) lo[k] = vscr[i][k];
            if (vscr[i][k] > hi[k]) hi[k] = vscr[i][k];
        }
    }
    fprintf(stderr, "[simple vbl=%u] m=%08x cls=%03x n=%d c=(%.2f,%.2f) "
                    "ext=(%.2f,%.2f) r0=(%d,%d,%d) p=(%d,%d,%d) rot=%08x "
                    "dmob=(%.2f,%.2f,%.2f)\n",
            openpete_frame, moby, s_tcls, nv, cx / nv, cy / nv,
            (double)(hi[0] - lo[0]), (double)(hi[1] - lo[1]),
            R[0][0], R[0][1], R[0][2],
            (int)cpu->read_word(moby + game_layout.moby.pos_off),
            (int)cpu->read_word(moby + game_layout.moby.pos_off + 4u),
            (int)cpu->read_word(moby + game_layout.moby.pos_off + 8u), rot,
            dmob[0], dmob[1], dmob[2]);
}

/** @brief Whether a placed simple-moby record is hidden behind nearer world depth, counted into the pass census when it is.
 *
 * A world-placed record's vertices sit at (R v + TR) * 2 in full world
 * units, so the origin is 2 TR and no vertex lies farther from it than twice
 * the model radius times R's stretch; that sphere takes
 * moby_simple_occluded. A screen-space record is never hidden.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param pl The record's placement.
 * @param R The record's composed matrix at unit scale.
 * @return 1 when hidden, else 0. */
static int s23_simple_hidden(CPUState* cpu, uint32_t moby, const s23_place* pl, const float R[3][3]) OP_REQUIRES(sim) {
    if (pl->screen) return 0;
    const double org[3] = { pl->TR[0] * 2.0, pl->TR[1] * 2.0, pl->TR[2] * 2.0 };
    const double rad = 2.0 * s23_simple_model_radius(cpu, pl->header) * s23_matrix_stretch(R) + 1.0;
    if (!moby_simple_occluded(cpu, moby, org, rad)) return 0;
    s_simple_occl++;
    s_simple_culled++;
    return 1;
}

/** @brief The present camera's position change since the tick, in the units and signs of a simple-moby record's stored delta.
 *
 * The camera position the cull subtracted is baked into every record's
 * delta (mobydraw.s:53 the camera position, :81,87,94 the three
 * subtractions, :621-627 the store), so replaying a record under a staged
 * camera re-rotates it but keeps the tick's translation; the decode re-bases
 * the delta by this change. The cull stores (moby - cam) >> 2 on the first
 * component and (cam - moby) >> 2 on the other two, so a camera that moved
 * by e shifts the stored halves by -e/4, +e/4 and +e/4. The present
 * position is the fractional interpolated camera (openpete_native_interp_pos)
 * when one is published, else the staged integer position. Held in double
 * and added before the transform, so a present position equal to the tick's
 * is a no-op bit for bit.
 *
 * A tick camera is installed only for a sub-tick present, so its absence is
 * what gives every canonical decode - the inline pass and the canonical lag
 * leg alike - no camera delta, no per-record own motion and the tick's own
 * rotation word.
 * @param cpu The guest CPU state, read only.
 * @param dcam Receives the change, zero on a canonical decode.
 * @return 1 on a sub-tick present (a tick camera is installed), else 0. */
static int s23_simple_cam_delta(CPUState* cpu, double dcam[3]) {
    static const double sign[3] = { -0.25, 0.25, 0.25 };
    int32_t tickpos[3];
    dcam[0] = dcam[1] = dcam[2] = 0.0;
    if (!s23_simple_tick_camera_pos(tickpos)) return 0;
    double cf[3];
    if (!openpete_native_interp_pos(game_layout.cam.base, cf))
        for (int i = 0; i < 3; i++) cf[i] = (double)(int32_t)cpu->read_word(game_layout.cam.position + 4u * (uint32_t)i);
    for (int i = 0; i < 3; i++) dcam[i] = (cf[i] - (double)tickpos[i]) * sign[i];
    return 1;
}

/** @brief The ordering-table key the draw drops a simple-moby record on (mobydraw.s:4193-4207).
 *
 * Its far end is where the record runs out of ordering table, a range limit
 * and not paint order, so extended LOD lifts it with the other one; the
 * native path sorts by depth and needs no bucket.
 * @param trz The translation Z, world / 2, truncated.
 * @param rot The moby's rotation word; its top byte is the key bias.
 * @param key Receives the key, at least 0.
 * @return 1 when the draw keeps the record, 0 when the key is past the table. */
static int s23_simple_record_key(int trz, uint32_t rot, int* key) {
    int k = (2 * trz >> 7) - (int)(int8_t)(rot >> 24);
    if (k < 0) k = 0;
    *key = k;
    return openpete_no_lod || !(k >= 272 && (k << 3) + 256 >= 3840);
}

/** @brief The fade and LOD tier of a simple-moby record: the moby's LOD byte against twice the depth (mobydraw.s:4193-4196).
 *
 * Its sign picks the low-poly lists. Extended LOD removes the depth term, the
 * rule the animated models' tier takes: every test then reads the value the
 * draw computes at depth 0, so the vertex count, the list pair, the fade and
 * the CLUT bias all take the draw's own near branch (tier >= 4096 at
 * mobydraw.s:4285-4289). A size term below that band keeps the depth term:
 * such a record fades at depth 0 as well, which is the moby's own authoring
 * and not a range limit.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param trz The translation Z, world / 2, truncated.
 * @return The tier. */
static int s23_simple_tier(CPUState* cpu, uint32_t moby, int trz) {
    const int size_term = (int)(cpu->read_byte(moby + moby_game.renderer_off)
                                & moby_game.renderer_mask) << 10;
    if (openpete_no_lod && size_term >= 4096) return size_term;
    return size_term - 2 * trz;
}

/** @brief Read a simple model's vertex words into model-space vertices.
 *
 * Each word packs X in bits 11-21, Y in bits 0-11 and Z in bits 21-31,
 * signed (mobydraw.s:4222-4229).
 * @param cpu The guest CPU state, read only.
 * @param H The geometry header; the words start at +0x10.
 * @param nv The vertex count.
 * @param vm Receives the vertices. */
static void s23_simple_vertices(CPUState* cpu, uint32_t H, int nv, double (*vm)[3]) {
    for (int i = 0; i < nv; i++) {
        const uint32_t w = cpu->read_word(H + 0x10u + (uint32_t)i * 4u);
        vm[i][0] = (double)((int32_t)(w << 10) >> 21);
        vm[i][1] = (double)((int32_t)(w << 20) >> 20);
        vm[i][2] = (double)((int32_t)w >> 21);
    }
}

/** @brief Build a simple-moby record's colour table and find its face list (mobydraw.s:4285-4346).
 *
 * The vertex count switches on the tier's sign, the face and colour lists
 * one step earlier: tier 0 takes the high-poly count and the low-poly lists
 * (mobydraw.s:4215-4219, 4285-4287). Inside the fade band (0 < tier < 4096,
 * with a low-poly block distinct from the high-poly one) each entry is
 * depth-cued toward the low-poly entry by IR0 = 4096 - tier and the CLUT row
 * is biased by IR0 >> 9.
 * @param cpu The guest CPU state, read only.
 * @param H The geometry header.
 * @param h0 The header's first word; its low byte is the colour count.
 * @param tier The record's tier.
 * @param ctab Receives the colours.
 * @param clut_bias Receives the CLUT row bias.
 * @param faces Receives the face list's address.
 * @return The colour count. */
static int s23_simple_colours(CPUState* cpu, uint32_t H, uint32_t h0, int tier,
                              uint32_t* ctab, int* clut_bias, uint32_t* faces) {
    const uint32_t pair = cpu->read_word(H + (tier > 0 ? 0x08u : 0x0Cu));
    const uint32_t cols = H + (pair >> 16);
    *faces = H + (pair & 0xFFFFu);
    int ncol = (int)(h0 & 0xFFu);
    if (ncol > S23_SIMPLE_COLS) ncol = S23_SIMPLE_COLS;
    const uint32_t far_cols = H + cpu->read_half(H + 0x0Eu);
    const int fade = (tier > 0 && tier < 4096 && far_cols != cols);
    const int ir0 = fade ? 4096 - tier : 0;
    *clut_bias = fade ? ir0 >> 9 : 0;
    if (ir0 > 0) s_simple_fade++;
    for (int i = 0; i < ncol; i++) {
        uint32_t c = cpu->read_word(cols + (uint32_t)i * 4u);
        if (fade) {
            const uint32_t f = cpu->read_word(far_cols + (uint32_t)i * 4u);
            const int rr = ngte_dpcs_ch((int)(c & 0xFFu),         (int)((f <<  4) & 0xFF0u), ir0);
            const int gg = ngte_dpcs_ch((int)((c >> 8) & 0xFFu),  (int)((f >>  4) & 0xFF0u), ir0);
            const int bb = ngte_dpcs_ch((int)((c >> 16) & 0xFFu), (int)((f >> 12) & 0xFF0u), ir0);
            c = (uint32_t)(rr | (gg << 8) | (bb << 16));
        }
        ctab[i] = c & 0xFFFFFFu;
    }
    return ncol;
}

/** @brief Reload the flat-colour entry for a record that names one (mobydraw.s:4351-4364).
 *
 * A record whose Moby+0x54 is zero draws with the last entry loaded, so the
 * words carry over from the record before.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param flat_w The four words, replaced when the record names an entry.
 * @return The moby's colour index. */
static uint32_t s23_simple_flat_reload(CPUState* cpu, uint32_t moby, uint32_t flat_w[4]) {
    const uint32_t mcol = cpu->read_word(moby + game_layout.moby.colour_off);
    if (!mcol) return 0u;
    const uint32_t e = moby_game_s23.simple_flat_coltab + (mcol << 4);
    for (uint32_t i = 0; i < 4u; i++) flat_w[i] = cpu->read_word(e + 4u * i);
    return mcol;
}

/** @brief Read a textured face's texel coordinates, texture page and CLUT into a held triangle's material.
 *
 * Word 2 carries corner 0's UV and the CLUT, word 3 corner 1's UV and the
 * texture page, word 4 the UVs of corners 2 and 3. A face whose page word
 * has bit 31 set draws at the page's ABR.
 * @param fw The face's words.
 * @param clut_bias The record's CLUT row bias.
 * @param u Receives the four corners' U.
 * @param v Receives the four corners' V.
 * @param T Receives the texture page, CLUT, depth and, when the page sets it, the blend. */
static void s23_simple_face_texture(const s23_face_words* fw, int clut_bias, int u[4], int v[4],
                                    s23_held_tri* T) {
    const uint32_t t0 = fw->t0, t1 = fw->t1, t2 = fw->t2;
    u[0] = (int)(t0 & 0xFFu); v[0] = (int)((t0 >> 8) & 0xFFu);
    u[1] = (int)(t1 & 0xFFu); v[1] = (int)((t1 >> 8) & 0xFFu);
    u[2] = (int)(t2 & 0xFFu); v[2] = (int)((t2 >> 8) & 0xFFu);
    u[3] = (int)((t2 >> 16) & 0xFFu); v[3] = (int)((t2 >> 24) & 0xFFu);
    ngte_tpage t;
    ngte_decode_tpage((t1 >> 16) & 0x7FFFu, (t0 >> 16) & 0xFFFFu, 0u, &t);
    T->tpx = t.tpx; T->tpy = t.tpy; T->bpp = t.bpp;
    T->clx = t.clx; T->cly = t.cly + clut_bias;
    if ((t1 >> 31) & 1u) T->blend = moby_abr_blend(t.abr);
}

/** @brief Hold one triangle half of a face: three of its corners in the given order, with their colours and texels.
 * @param W The worker's scratch.
 * @param face The face's index from s23_abr_note_face.
 * @param T The face's material and paint key; its corners, colours, texels and face key are written.
 * @param idx The face's four vertex indices.
 * @param o The half's three corner slots.
 * @param col The four corners' colours.
 * @param u The four corners' U.
 * @param v The four corners' V. */
static void s23_simple_hold_half(s23_simple_ws* W, int face, s23_held_tri* T, const int idx[4],
                                 const int o[3], const uint32_t* col, const int u[4], const int v[4]) {
    for (int k = 0; k < 3; k++) {
        T->vi[k] = idx[o[k]];
        T->rgb[k] = col[o[k]]; T->u[k] = u[o[k]]; T->v[k] = v[o[k]];
    }
    T->fkey = (uint32_t)(T->vi[0] | (T->vi[1] << 10) | (T->vi[2] << 20));
    s23_abr_hold(W, face, T);
    W->n.tris++;
}

/** @brief Shade one face of a record and hold the triangle halves it keeps.
 *
 * The winding tests read the face's facing values (moby_simple_face_nclips:
 * the NCLIPs of its projected screen positions, or its true facing when a
 * vertex lies in the GTE divide-overflow regime), the shading
 * s23_simple_shade_face; a textured face reads its texels
 * (s23_simple_face_texture). The first half is (v0, v1, v2) and the second
 * (v1, v3, v2), each with its paint-key bucket.
 * @param se The emit context bound to the record.
 * @param W The worker's scratch.
 * @param J The record.
 * @param iv The record's paint-key RTPS.
 * @param ctab The record's colours.
 * @param fw The face's words; its vertex indices are each below J->nv. */
static void s23_simple_face(const moby_simple_emit* se, s23_simple_ws* W, const s23_simple_job* J,
                            const moby_otk_vert_t* iv, const uint32_t* ctab, const s23_face_words* fw) {
    const uint32_t f0 = fw->f0, f1 = fw->f1;
    const int idx[4] = { (int)((f0 >> 21) & 0x7Fu), (int)((f0 >> 14) & 0x7Fu),
                         (int)((f0 >>  7) & 0x7Fu), (int)( f0        & 0x7Fu) };
    const int tri = (idx[2] == idx[3]);
    const int face = s23_abr_note_face(W, fw, idx, tri);
    double nA, nB;
    moby_simple_face_nclips(se, idx, tri, &nA, &nB);
    s23_shade sh;
    if (!s23_simple_shade_face((const int (*)[3])J->R, f1, tri, nA, nB, J->trz, J->flat_w, ctab,
                               J->ncol, &W->n, &sh))
        return;
    int u[4] = { 0, 0, 0, 0 }, v[4] = { 0, 0, 0, 0 };
    s23_held_tri T = { .textured = sh.textured, .bpp = 16, .blend = sh.blend };
    if (sh.textured) s23_simple_face_texture(fw, J->clut_bias, u, v, &T);
    const int32_t otk = s23_simple_face_otk(J->ork, iv, idx, tri, f0, f1);
    static const int k_half[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
    if (sh.emit_a) { T.otk = otk; s23_simple_hold_half(W, face, &T, idx, k_half[0], sh.col, u, v); }
    if (sh.emit_b) {
        T.otk = moby_otk_second(otk, sh.emit_a);
        s23_simple_hold_half(W, face, &T, idx, k_half[1], sh.col, u, v);
    }
}

/* The decode's per-record runs: model-space vertices, paint-key RTPS, colours
 * and face words, appended by the serial prefix and read by the jobs. Grown
 * only by the prefix, so every pointer a job derives stays valid through the
 * fan-out. */
static struct {
    double (*vm)[3];          int vm_n, vm_cap;
    moby_otk_vert_t* iv;      int iv_n, iv_cap;
    uint32_t* col;            int col_n, col_cap;
    s23_face_words* face;     int face_n, face_cap;
} s_s23_pool;

/** @brief Make room for n more elements in one of the decode's pools.
 * @param buf The pool's array, reallocated when it grows.
 * @param used Its element count.
 * @param cap Its capacity, updated when it grows.
 * @param n The elements wanted past `used`.
 * @param elem The element size.
 * @return 1 when the room exists, 0 when the allocation failed. */
static int s23_pool_room(void** buf, int used, int* cap, int n, size_t elem) {
    if (used + n <= *cap) return 1;
    int c = *cap ? *cap : 1024;
    while (c < used + n) c *= 2;
    void* p = realloc(*buf, (size_t)c * elem);
    if (!p) return 0;
    *buf = p;
    *cap = c;
    return 1;
}

/** @brief Reserve a record's runs in the decode's pools: nv vertices and paint-key RTPS, the colour table and the face list's upper bound.
 * @param J Receives the runs' offsets.
 * @param nv The record's vertex count.
 * @return 1 when every pool has the room, else 0. */
static int s23_pool_reserve(s23_simple_job* J, int nv) {
    if (!s23_pool_room((void**)&s_s23_pool.vm, s_s23_pool.vm_n, &s_s23_pool.vm_cap, nv, sizeof *s_s23_pool.vm) ||
        !s23_pool_room((void**)&s_s23_pool.iv, s_s23_pool.iv_n, &s_s23_pool.iv_cap, nv, sizeof *s_s23_pool.iv) ||
        !s23_pool_room((void**)&s_s23_pool.col, s_s23_pool.col_n, &s_s23_pool.col_cap, S23_SIMPLE_COLS,
                       sizeof *s_s23_pool.col) ||
        !s23_pool_room((void**)&s_s23_pool.face, s_s23_pool.face_n, &s_s23_pool.face_cap, S23_SIMPLE_FACES,
                       sizeof *s_s23_pool.face))
        return 0;
    J->vm_off = s_s23_pool.vm_n;
    J->iv_off = s_s23_pool.iv_n;
    J->col_off = s_s23_pool.col_n;
    J->face_off = s_s23_pool.face_n;
    return 1;
}

/** @brief Read a record's face stream into its face run, keeping each face whose corners the record holds.
 *
 * The stride is the face's own texture bit: the flat-lit material draws
 * untextured but still carries the three texel words, which are read with the
 * face. Counts every face into s_simple_faces.
 * @param cpu The guest CPU state, read only.
 * @param faces The face list: a byte count, then the faces.
 * @param nv The record's vertex count.
 * @param out Receives the kept faces, at most S23_SIMPLE_FACES.
 * @return The number kept. */
static int s23_simple_read_faces(CPUState* cpu, uint32_t faces, int nv, s23_face_words* out) {
    const uint32_t bytecount = cpu->read_word(faces);
    uint32_t fp = faces + 4u;
    const uint32_t end = fp + bytecount;
    int n = 0;
    for (int guard = 0; fp + 8u <= end && guard < S23_SIMPLE_FACES; guard++) {
        const uint32_t f0 = cpu->read_word(fp);
        const uint32_t f1 = cpu->read_word(fp + 4u);
        const int i0 = (int)((f0 >> 21) & 0x7Fu), i1 = (int)((f0 >> 14) & 0x7Fu);
        const int i2 = (int)((f0 >> 7) & 0x7Fu), i3 = (int)(f0 & 0x7Fu);
        const int textured = ((int32_t)f1) < 0;
        s_simple_faces++;
        if (i0 < nv && i1 < nv && i2 < nv && (i2 == i3 || i3 < nv)) {
            s23_face_words* w = &out[n++];
            w->f0 = f0; w->f1 = f1;
            w->t0 = textured ? cpu->read_word(fp + 0x08u) : 0u;
            w->t1 = textured ? cpu->read_word(fp + 0x0Cu) : 0u;
            w->t2 = textured ? cpu->read_word(fp + 0x10u) : 0u;
        }
        fp += textured ? 0x14u : 0x08u;
    }
    return n;
}

/** @brief The per-record job of the fan-out (moby_simple_job_fn): walk the record's faces, resolve its blends and emit its triangles.
 *
 * Reads the record and the pools the serial prefix filled, and writes only the
 * worker's scratch and what the emit context emits.
 * @param se The emit context bound to the record.
 * @param rec The record's index in s_s23_job.
 * @param worker The worker's scratch index.
 * @param arg Unused. */
static void s23_simple_job_run(moby_simple_emit* se, int rec, int worker, void* arg) {
    (void)arg;
    const s23_simple_job* J = &s_s23_job[rec];
    s23_simple_ws* W = &s_ws[worker];
    const moby_otk_vert_t* iv = s_s23_pool.iv + J->iv_off;
    const uint32_t* ctab = s_s23_pool.col + J->col_off;
    const s23_face_words* fw = s_s23_pool.face + J->face_off;
    if (J->deferred)
        moby_simple_project_run(&J->pj, (const double (*)[3])(s_s23_pool.vm + J->vm_off),
                                s_s23_pool.iv + J->iv_off, worker);
    s23_abr_begin(W);
    for (int i = 0; i < J->face_n; i++) s23_simple_face(se, W, J, iv, ctab, &fw[i]);
    W->n.held += s23_abr_flush(se, W, J, (const double (*)[3])(s_s23_pool.vm + J->vm_off));
}

/** @brief Place, cull and LOD-select one queued simple-moby record: everything the decode settles before reading a vertex.
 *
 * Counts the record into the pass census and its cull into s_simple_culled.
 * @param cpu The guest CPU state, read only.
 * @param r The queue index.
 * @param Rcam The camera matrix.
 * @param dcam The present camera's change since the tick (s23_simple_cam_delta).
 * @param bracketed 1 on a sub-tick present.
 * @param dmob Receives the moby's own displacement over this present, world units.
 * @param pl Receives the placement.
 * @param key Receives the record's ordering-table key.
 * @param tier Receives the record's tier.
 * @return The record's vertex count, 0 when it is not drawn. */
static int s23_simple_admit(CPUState* cpu, int r, const int Rcam[3][3], const double dcam[3], int bracketed,
                            double dmob[3], s23_place* pl, int* key, int* tier) {
    const uint32_t moby = s_sq->q[r][0];
    if (s23_simple_late_skip(moby)) return 0;
    /* The moby's own world displacement over the fraction of the tick this
     * present shows. The cull composed the record at the tick's position,
     * which both kinds encode directly, so without this term the record
     * holds that position for the whole tick and steps at the tick rate. */
    dmob[0] = dmob[1] = dmob[2] = 0.0;
    if (bracketed) s23_simple_own_motion(moby, dmob);
    double base[3];
    const int exact = s23_simple_base(cpu, r, bracketed, dmob, base);
    const int placed = s23_simple_place(cpu, r, Rcam, dcam, dmob, base, pl);
    if (pl->screen) s_simple_screen++;
    else if (exact) s_simple_base_exact[bracketed != 0]++;
    else s_simple_base_kept[bracketed != 0]++;
    s_simple_recs++;
    if (!placed) { s_simple_culled++; return 0; }
    if (pl->screen) s_simple_screen_drawn++;
    const int trz = (int)pl->TR[2];
    if (!s23_simple_record_key(trz, cpu->read_word(moby + game_layout.moby.angle_off), key)) {
        s_simple_culled++;
        return 0;
    }
    *tier = s23_simple_tier(cpu, moby, trz);
    const uint32_t h0 = cpu->read_word(pl->header);
    const int nv = (int)((*tier >= 0 ? (h0 >> 8) : (h0 >> 16)) & 0xFFu);
    if (nv < 3) return 0;
    return nv > S23_SIMPLE_VERTS ? S23_SIMPLE_VERTS : nv;
}

/* What the serial prefix carries from one record to the next: the camera
 * state the whole decode shares, the flat-material words in force and the
 * prefix's own projected-vertex scratch. */
typedef struct {
    int    Rcam[3][3];
    double dcam[3];
    int    bracketed;   /* a sub-tick present */
    int    skip_lp;     /* the cull's far-geometry suppression word is set */
    uint32_t flat_w[4];
    long   faces;       /* the faces the jobs will walk */
} s23_prefix;

/** @brief The serial prefix of one queued record: admit, place, compose, project and read everything its face walk needs into its job and the pools.
 *
 * Admits the record (s23_simple_admit), composes the moby's rotations onto
 * its placement matrix and takes the occlusion test; then names the record,
 * reads its vertex words, projects them (moby_simple_project_record), runs
 * the traces, builds its colour table, reloads the flat-material words in the
 * draw's order (a record with a zero colour index keeps the previous
 * record's), reads its face stream and the view its paint-key RTPS takes, and
 * takes its emit scope. A low-poly record under the cull's suppression word is
 * projected and traced but produces no job.
 * @param cpu The guest CPU state, read only.
 * @param P The decode's shared state; its flat words and face count are updated.
 * @param r The queue index.
 * @param J Receives the record's job.
 * @param sc Receives the record's emit scope.
 * @return 1 when the record has a job, else 0. */
static int s23_simple_prefix_record(CPUState* cpu, s23_prefix* P, int r, s23_simple_job* J,
                                    moby_simple_scope_t* sc) OP_REQUIRES(sim) {
    static float vscr[S23_SIMPLE_VERTS][2];
    s23_place pl;
    int key = 0, tier = 0;
    double dmob[3];
    const int nv = s23_simple_admit(cpu, r, (const int (*)[3])P->Rcam, P->dcam, P->bracketed, dmob,
                                    &pl, &key, &tier);
    if (!nv) return 0;
    const uint32_t moby = s_sq->q[r][0];
    const uint32_t H = pl.header;
    const uint32_t rot = cpu->read_word(moby + game_layout.moby.angle_off);
    /* The moby's three rotations, composed onto the placement matrix in the
     * draw's order: Y, then X, then Z (mobydraw.s:4063-4110, 4111-4158,
     * 4159-4189, one column pair replaced per axis through two MVMVAs).
     * Within a sub-tick present the angles come from the object's own pair,
     * so a moby turning a byte per tick sweeps instead of stepping; the
     * canonical pass takes the tick's word, where the composition is the
     * table lookup the draw itself does. R, in Q12, is the draw's GTE
     * compose and feeds the face lighting and the paint key; Rf, the same
     * rotation at unit scale composed in float at the unrounded angles,
     * places the vertices. */
    float Rf[3][3];
    double ang[3];
    if (!P->bracketed || !moby_record_angles_at_present(cpu, moby, ang)) ngte_word_angles(rot, ang);
    ngte_compose_euler_at(cpu, pl.R, ang, J->R, Rf);
    if (s23_simple_hidden(cpu, moby, &pl, (const float (*)[3])Rf)) return 0;
    if (!s23_pool_reserve(J, nv)) return 0;

    moby_simple_record(cpu, moby);
    if (pl.screen) moby_simple_screen_origin(pl.ofx, pl.ofy);
    /* The translation the draw loads into TRX/TRY/TRZ (mobydraw.s
     * 4190-4192): the MVMVA (sf=1) truncates the rotated delta to an
     * integer before the rejoin doubles it (mobydraw.s 3946-3952,
     * 4058-4060), where TR keeps the fraction. With it, the record's
     * paint key for both face families. */
    J->itr[0] = 2 * (int)floor(pl.TR[0] * 0.5);
    J->itr[1] = 2 * (int)floor(pl.TR[1] * 0.5);
    J->itr[2] = 2 * (int)floor(pl.TR[2] * 0.5);
    moby_simple_otk_record(MOBY_OTK_S23_SIMPLE, J->itr[2], &J->ork[0]);
    moby_simple_otk_record(MOBY_OTK_S23_SIMPLE_FLAT, J->itr[2], &J->ork[1]);
    double (*vmod)[3] = s_s23_pool.vm + J->vm_off;
    s23_simple_vertices(cpu, H, nv, vmod);
    memcpy(J->Rf, Rf, sizeof J->Rf);
    memcpy(J->TR, pl.TR, sizeof J->TR);
    const moby_simple_xform_t xf = { (const int (*)[3])J->R, (const float (*)[3])J->Rf, J->TR, J->itr, &J->ork[0] };
    /* The vertex loop moves into the record's job unless a trace reads the
     * screen positions here. */
    J->deferred = !s23_postrace_on() &&
                  moby_simple_project_defer(&xf, nv, (const double (*)[3])vmod, s_s23_pool.iv + J->iv_off, &J->pj);
    if (!J->deferred) {
        OP_PROF_ACCUM_SCOPE("msim.project");
        moby_simple_project_record(&xf, (const double (*)[3])vmod, nv, vscr, s_s23_pool.iv + J->iv_off);
        s23_simple_postrace(cpu, moby, (const float (*)[2])vscr, nv, (const int (*)[3])J->R, rot, dmob);
        moby_native_late_trace(cpu, moby, (const float (*)[2])vscr, nv, "SIM", s_sq_ghost_only);
    }
    s_simple_verts += nv;

    if (tier <= 0 && P->skip_lp) {
        /* No job reads the record: its slots are filled here, as the
         * projection in place fills them. */
        if (J->deferred)
            moby_simple_project_run(&J->pj, (const double (*)[3])vmod, s_s23_pool.iv + J->iv_off, 0);
        return 0;
    }
    if (tier <= 0) s_simple_lp++;
    uint32_t faces;
    J->ncol = s23_simple_colours(cpu, H, cpu->read_word(H), tier, s_s23_pool.col + J->col_off,
                                 &J->clut_bias, &faces);
    J->single = s23_simple_flat_reload(cpu, moby, P->flat_w) != 0u;
    memcpy(J->flat_w, P->flat_w, sizeof J->flat_w);
    J->face_n = s23_simple_read_faces(cpu, faces, nv, s_s23_pool.face + J->face_off);
    ngte_view view;
    ngte_view_build(cpu, &view);
    J->ofx = view.ofx; J->ofy = view.ofy; J->H = view.H_canon;
    J->trz = pl.TR[2];
    J->nv = nv;
    J->key = key;
    moby_simple_scope_take(nv, sc);
    s_s23_pool.vm_n += nv;
    s_s23_pool.iv_n += nv;
    s_s23_pool.col_n += J->ncol;
    s_s23_pool.face_n += J->face_n;
    P->faces += J->face_n;
    return 1;
}

/** @brief Decodes the host queue of simple-moby records into triangles for the shared walk.
 *
 * A serial prefix runs every queued record through s23_simple_prefix_record
 * in queue order: admission, placement, the occlusion test, projection,
 * colours, the flat-material words and the face stream, everything that reads
 * guest memory or the pass state. moby_simple_fanout then runs each record's
 * face walk, blend resolve and emit (s23_simple_job_run), on the walk workers
 * when the decode is large enough, and the triangle stream is the serial
 * one. A sub-tick present, which has a staged tick camera, re-bases each
 * record by the camera change and applies the moby's own displacement and
 * interpolated rotation; a canonical decode takes the cull's values as they
 * stand. A hidden world-placed record is neither decoded nor drawn; the
 * canonical pass decides each verdict and its replays hold it. Reads guest
 * memory, writes none, and updates the pass counters, the workers' shares
 * summed after the fan-out; it uses static scratch and is not reentrant.
 * @param cpu The guest CPU state, read only.
 * @return The number of triangles emitted. */
static int s23_simple_decode(CPUState* cpu) OP_REQUIRES(sim) {
    s23_prefix P;
    ngte_read_matrix(cpu, game_layout.cam.base, P.Rcam);   /* mobydraw.s:3898-3908 */
    /* The far-geometry suppression word the cull parks in GTE data 3
     * (mobydraw.s:34,36), consulted at mobydraw.s:4336-4341. */
    P.skip_lp = cpu->read_word(moby_game_s23.simple_skip_lowpoly) != 0u;
    P.bracketed = s23_simple_cam_delta(cpu, P.dcam);
    memcpy(P.flat_w, s_sq->flat, sizeof P.flat_w);
    P.faces = 0;
    s_s23_pool.vm_n = s_s23_pool.iv_n = s_s23_pool.col_n = s_s23_pool.face_n = 0;

    int nrec = 0;
    const uint64_t t_pre = moby_native_stage_now();
    for (int r = 0; r < s_sq->qn; r++)
        nrec += s23_simple_prefix_record(cpu, &P, r, &s_s23_job[nrec], &s_s23_scope[nrec]);
    moby_native_stage_add(MOBY_NATIVE_STAGE_SIMPLE_PREFIX, t_pre);

    for (int w = 0; w <= MOBY_SIMPLE_WORKERS; w++) memset(&s_ws[w].n, 0, sizeof s_ws[w].n);
    { OP_PROF_ACCUM_SCOPE("msim.fanout");
      const uint64_t t_fan = moby_native_stage_now();
      moby_simple_fanout(s_s23_scope, nrec, P.faces, s23_simple_job_run, NULL);
      moby_native_stage_add(MOBY_NATIVE_STAGE_SIMPLE_FANOUT, t_fan); }
    long held = 0;
    for (int w = 0; w < MOBY_SIMPLE_WORKERS; w++) {
        const s23_simple_counts* n = &s_ws[w].n;
        s_simple_tris += n->tris;
        s_simple_flat += n->flat;
        s_flat_zero_colour += n->flat_zero_colour;
        s_flat_back_emitted += n->flat_back_emitted;
        s_flat_back_dropped += n->flat_back_dropped;
        s_flat_front_blended += n->flat_front_blended;
        held += n->held;
    }
    return (int)held;
}

/** @brief On a late present, decode the previous copy's records whose moby despawned in the newest tick.
 *
 * The previous copy swaps in under its own cull's camera (as for a held cut's
 * replay) and the decode keeps only the records moby_native_late_ghost_admit
 * stages; the staged spans are restored and the copies swap back after.
 * Needs the previous copy to be of the epoch before the current one.
 * @param cpu The guest CPU state, read only.
 * @return The number of triangles emitted. */
static int s23_simple_late_ghosts(CPUState* cpu) OP_REQUIRES(sim) {
    if (!moby_native_late_active() || cut_held_set_pick(s_sq->epoch, s_sq_prev->epoch) != CUT_HELD_PREV ||
        !moby_native_prev_cull_cam_pos(s_sq_prev_cam))
        return 0;
    s23_simple_queue_swap();
    s_sq_replaying_prev = 1;
    s_sq_ghost_only = 1;
    const int n = s23_simple_decode(cpu);
    s_sq_ghost_only = 0;
    s_sq_replaying_prev = 0;
    s23_simple_queue_swap();
    moby_native_late_unstage();
    return n;
}

/** @brief A sub-tick present's replay of the simple models: the captured queue decoded again under the present's camera.
 *
 * Makes no call into the game's draw. During a held cut's previous-queue
 * replay (moby_native_prev_queue_replay) it decodes that epoch's copy
 * (cut_held_set_pick: the previous copy, the current one when the last
 * committed epoch captured none, or nothing when that epoch captured none),
 * re-based from that cull's camera and from each object's state as of that
 * cull. Resets the pass counters, and prints
 * its own census line under moby_verify_vbl, since the pass census covers the
 * canonical tick alone, and a held-queue line under the cut census.
 * @param cpu The guest CPU state, read only.
 * @return The number of triangles emitted. */
static int s23_simple_replay(CPUState* cpu) OP_REQUIRES(sim) {
    s_simple_recs = s_simple_verts = s_simple_faces = s_simple_tris = 0;
    s_simple_flat = s_simple_screen = s_simple_culled = s_simple_screen_drawn = 0;
    s_simple_lp = s_simple_fade = s_simple_occl = 0;
    /* A held camera cut replaying the previous epoch's animated queue replays
     * this kind's copy of the same epoch: the cull built both against the
     * camera the present shows. */
    uint32_t ep = 0;
    cut_held_pick_t pick = CUT_HELD_EMPTY;
    const int held = moby_native_prev_queue_replay(&ep, s_sq_prev_cam) &&
                     s23_simple_held_pick(ep, &pick);
    if (held && pick == CUT_HELD_PREV) s23_simple_queue_swap();
    s_sq_replaying_prev = held;
    const int n = (held && pick == CUT_HELD_EMPTY) ? 0 : s23_simple_decode(cpu);
    if (cut_census_on() && moby_native_prev_queue_replay(NULL, NULL)) {
        static const char* const k_set[] = { "empty", "curr", "prev" };
        fprintf(stderr, "[cut-census vbl=%u] held-queue kind=simple set=%s epoch=%u queued=%d "
                        "drawn=%ld tris=%d\n",
                openpete_frame, held ? k_set[pick] : "refused", s_sq->epoch,
                (held && pick == CUT_HELD_EMPTY) ? 0 : s_sq->qn, s_simple_recs - s_simple_culled, n);
    }
    if (held && pick == CUT_HELD_PREV) s23_simple_queue_swap();
    s_sq_replaying_prev = 0;
    if (!held) s23_simple_late_ghosts(cpu);
    /* The replay's own census: the queue it read and what that queue decoded
     * to. The pass census covers the canonical tick alone, so without this a
     * replay of a queue no canonical pass built this tick reads as silence. */
    if (moby_verify_vbl())
        fprintf(stderr, "[moby-simple-replay vbl=%u] queued=%d drawn=%ld culled=%ld "
                        "screenkind=%ld screendrawn=%ld "
                        "verts=%ld faces=%ld tris=%d lp=%ld fade=%ld\n",
                openpete_frame, s_sq->qn, s_simple_recs, s_simple_culled,
                s_simple_screen, s_simple_screen_drawn,
                s_simple_verts, s_simple_faces, n, s_simple_lp, s_simple_fade);
    moby_kind_census("sub", "simple", s_sq->qn, s_simple_occl, s_simple_verts, n);
    s23_simple_base_census();
    return n;
}

/* ---- extended-LOD reveal ------------------------------------------------ */
/** @brief The half-width of the distance box the cull tests a moby against (mobydraw.s:75-100).
 *
 * The signed low byte of the halfword at Moby+0x4C scaled by 256, widened by
 * 256 per unit of the low two drawn-verdict bits that halfword's high byte
 * carries. A byte that is zero or negative leaves a degenerate box and the
 * cull skips the moby, which is the unpopulated-slot sentinel.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param drawn_bits The drawn-verdict byte the cull tested.
 * @return The half-width, or 0 for the sentinel. */
static int s23_cull_box_threshold(CPUState* cpu, uint32_t moby, unsigned drawn_bits) {
    const int rad = (int)(int8_t)cpu->read_byte(moby + 0x4Cu);
    if (rad <= 0) return 0;
    return (rad << 8) + (int)((drawn_bits & 3u) << 8);
}

/** @brief One sphere's near, lateral and vertical arms as the cull runs them, the lateral slope scaled by `widen` and the vertical by `vwiden`.
 *
 * The thresholds are the radius's sums of shifts (mobydraw.s:128-138,158-163
 * for the first sphere, :385-393,412-415 for the second; S3 the same
 * instructions at :383-411):
 *   near      z + r <= 0 rejects
 *   lateral   4(|x| - a2) - 3(z + a3) >= 0 rejects, the 3 scaled by `widen`
 *   vertical  (z + t1) - 3(|y| - t0) <= 0 rejects (first sphere: Spyro 2
 *             mobydraw.s:179-189, 0x80043ADC-0x80043AFC; Spyro 3 :178-188),
 *             the (z + t1) scaled by `vwiden`
 * The lateral arm's 3:4 slope is the 4:3 half-angle and the vertical arm's
 * 1:3 slope the half-height; at a factor of 1.0 the arm is the cull's own
 * integer test, the products exact in double.
 * @param c The sphere's view-space centre, x y z.
 * @param r The sphere's radius.
 * @param widen The lateral slope factor, 1.0 at 4:3.
 * @param vwiden The vertical slope factor, 1.0 at the game's FOV.
 * @return MOBY_WIDE_NEAR, MOBY_WIDE_LATERAL or MOBY_WIDE_VERTICAL for the
 *         first arm that rejects, else MOBY_WIDE_ADMIT. */
static moby_wide_verdict_t s23_sphere_arms(const int64_t c[3], int64_t r, double widen,
                                           double vwiden) {
    const int64_t a2 = (r >> 1) + (r >> 2) + (r >> 5), a3 = (r >> 1) + (r >> 4) + (r >> 5);
    const int64_t t0 = (r >> 1) + (r >> 4),            t1 = (r >> 2) + (r >> 4);
    const int64_t ax = c[0] < 0 ? -c[0] : c[0], ay = c[1] < 0 ? -c[1] : c[1];
    if (c[2] + r <= 0) return MOBY_WIDE_NEAR;
    if (4.0 * (double)(ax - a2) - 3.0 * widen * (double)(c[2] + a3) >= 0.0) return MOBY_WIDE_LATERAL;
    if (vwiden * (double)(c[2] + t1) - 3.0 * (double)(ay - t0) <= 0.0) return MOBY_WIDE_VERTICAL;
    return MOBY_WIDE_ADMIT;
}

/** @brief Whether a sphere the arms admit lies wholly inside the cone, the cull's verdict 2.
 *
 * mobydraw.s:194-217 (S2 0x80043B10-0x80043B64): the lateral test
 * 4(|x| + a2) - 3(z - a3) < 0, its 3 scaled by `widen`, then the vertical
 * (z - t1) - 3(|y| + t0) > 0, its (z - t1) scaled by `vwiden`. Verdict 1, a
 * sphere straddling an edge, is the one the cull tests a second sphere for.
 * @param c The sphere's view-space centre, x y z.
 * @param r The sphere's radius.
 * @param widen The lateral slope factor, 1.0 at 4:3.
 * @param vwiden The vertical slope factor, 1.0 at the game's FOV.
 * @return 1 for verdict 2, 0 for verdict 1. */
static int s23_sphere_inside(const int64_t c[3], int64_t r, double widen, double vwiden) {
    const int64_t a2 = (r >> 1) + (r >> 2) + (r >> 5), a3 = (r >> 1) + (r >> 4) + (r >> 5);
    const int64_t t0 = (r >> 1) + (r >> 4),            t1 = (r >> 2) + (r >> 4);
    const int64_t ax = c[0] < 0 ? -c[0] : c[0], ay = c[1] < 0 ? -c[1] : c[1];
    if (4.0 * (double)(ax + a2) - 3.0 * widen * (double)(c[2] - a3) >= 0.0) return 0;
    return vwiden * (double)(c[2] - t1) - 3.0 * (double)(ay + t0) > 0.0;
}

/** @brief The second sphere's view-space centre and radius, from the AnimationHeader +0x08 word.
 *
 * mobydraw.s:366-396 (S2 0x80043DA8-0x80043E20; S3 :364-394,
 * 0x800309C8-0x80030A40):
 * the word's bytes 3, 2 and 1, sign-extended and scaled by 8, are the offset
 * in the moby's model space, loaded as IR3 = byte 3, IR1 = -byte 2 and
 * IR2 = -byte 1; byte 0 scaled by 8 is the radius. One MVMVA through the
 * matrix the cull composed for the record (the camera matrix times the
 * moby's euler rotation, mobydraw.s:227-354) with the first sphere's centre
 * as the translation gives the centre: c = c1 + (M . IR) >> 12.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address, for its rotation word.
 * @param cam_m The camera matrix the first sphere was projected through.
 * @param word The +0x08 word, nonzero.
 * @param c1 The first sphere's view-space centre.
 * @param c Receives the second sphere's centre.
 * @return The second sphere's radius. */
static int64_t s23_second_sphere(CPUState* cpu, uint32_t moby, const int cam_m[3][3],
                                 uint32_t word, const int64_t c1[3], int64_t c[3]) {
    ngte_euler_sc e;
    ngte_euler_sincos(cpu, cpu->read_word(moby + game_layout.moby.angle_off), &e);
    int M[3][3];
    ngte_compose_euler(cam_m, &e, M);
    const int64_t ir1 = -(int64_t)(int8_t)(word >> 16) * 8;
    const int64_t ir2 = -(int64_t)(int8_t)(word >> 8) * 8;
    const int64_t ir3 =  (int64_t)(int8_t)(word >> 24) * 8;
    for (int i = 0; i < 3; i++)
        c[i] = c1[i] + (((int64_t)M[i][0] * ir1 + (int64_t)M[i][1] * ir2 + (int64_t)M[i][2] * ir3) >> 12);
    return (int64_t)(word & 0xFFu) << 3;
}

/** @brief The animated models' admission as the cull runs it, with the lateral slope scaled by `widen` and the vertical by `vwiden`.
 *
 * mobydraw.s:62-187 and 355-450; Spyro 2 0x80043920-0x80043EE8, Spyro 3 the
 * same instructions, the second sphere at mobydraw.s:353-448. The cull
 * clears the drawn-verdict byte, writes 1 or 2 once the first sphere passes
 * (mobydraw.s:73,224), and then drops a verdict-1 moby whose AnimationHeader
 * +0x08 word names a second sphere that fails its arms, leaving the byte at 1
 * with no record. Under a view wider than 4:3 the lateral arms are the ones
 * whose answer changes, and under one taller (a --fov scale below 1) the
 * vertical arms.
 *
 * The box is the cull's own (s23_cull_box_threshold) on the quarter-world
 * deltas (moby - cam) >> 2, (cam - moby) >> 2 and (cam - moby) >> 2
 * (mobydraw.s:77-100). A sector byte at Moby+0x52 below 0xFE must name a
 * sector whose byte in the table $gp holds (D_80072330, mobydraw.s:49-50, the
 * layout's env_anim.live_table) is non-zero (mobydraw.s:95-106); a
 * KUSEG model-table entry is the simple kind, which the second queue tests
 * (mobydraw.s:109-116). The draw radius is the animation header's byte 7
 * scaled by 16 (mobydraw.s:118-124). The centre is the GTE MVMVA of the
 * deltas through the camera matrix at G_CAMERA+0x00, IR1 the Y delta, IR2 the
 * Z and IR3 the X, each loaded into a 16-bit IR register (mobydraw.s:119-141).
 * The range arm MAC3 - box >= 0 rejects (mobydraw.s:142-143); then the first
 * sphere's arms (s23_sphere_arms) with the widened slopes.
 *
 * A moby the 4:3 first sphere admits is revealed only when the cull dropped it
 * by its second sphere's 4:3 lateral or vertical arm: under the widened view the second
 * sphere is skipped when the widened first sphere is verdict 2, and otherwise
 * tested with the widened slopes (s23_second_sphere). A moby only the widened
 * first sphere admits takes the same second-sphere test at verdict 1. At
 * `widen` and `vwiden` 1.0 the widened and canonical arms are one test and
 * nothing is admitted.
 *
 * With `open_sector` the sector-byte test is skipped, so a moby the cull drops
 * on that byte alone returns the verdict of the tests after it.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param drawn_bits The drawn-verdict byte the box threshold widens by.
 * @param widen The lateral slope factor, 1.0 at 4:3.
 * @param vwiden The vertical slope factor, 1.0 at the game's FOV.
 * @param open_sector Nonzero to treat the moby's sector as live.
 * @return The first test that rejects the moby (MOBY_WIDE_SPHERE2 for the
 *         second sphere under the widened view), MOBY_WIDE_CANONICAL when the
 *         game's own arms also admit it, or MOBY_WIDE_ADMIT when only the
 *         widened arms do. */
static moby_wide_verdict_t s23_cull_view_admits(CPUState* cpu, uint32_t moby,
                                                unsigned drawn_bits, double widen,
                                                double vwiden, int open_sector) {
    const int64_t t = s23_cull_box_threshold(cpu, moby, drawn_bits);
    if (t <= 0) return MOBY_WIDE_OUTSIDE;
    const uint32_t cam = game_layout.cam.base;
    const int32_t cpos[3] = { (int32_t)cpu->read_word(cam + 0x28u), (int32_t)cpu->read_word(cam + 0x2Cu),
                              (int32_t)cpu->read_word(cam + 0x30u) };
    int R[3][3];
    ngte_read_matrix(cpu, cam, R);
    int32_t d[3];
    int64_t mac[3];
    moby_cull_centre(cpu, moby, cpos, (const int (*)[3])R, 1, d, mac);
    const int32_t dx = d[0], dy = d[1], dz = d[2];
    if (dx <= -t || dx >= t || dy <= -t || dy >= t || dz <= -t || dz >= t)
        return MOBY_WIDE_OUTSIDE;
    const uint32_t sector = cpu->read_byte(moby + 0x52u);
    if (!open_sector && sector < 0xFEu && game_layout.env_anim.live_table &&
        cpu->read_byte(game_layout.env_anim.live_table + sector) == 0u)
        return MOBY_WIDE_SECTOR;
    const uint32_t cls   = cpu->read_half(moby + game_layout.moby.class_off);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    if (model < 0x80000000u) return MOBY_WIDE_OTHER_KIND;
    const uint32_t id     = cpu->read_byte(moby + game_layout.moby.anim_state_off);
    const uint32_t header = cpu->read_word(model + game_layout.moby.model_anims_off + id * 4u);
    if (header < 0x80000000u) return MOBY_WIDE_OTHER_KIND;
    const int64_t r = (int64_t)cpu->read_byte(header + 7u) << 4;
    if (mac[2] - t >= 0) return MOBY_WIDE_RANGE;
    const moby_wide_verdict_t first = s23_sphere_arms(mac, r, widen, vwiden);
    if (first != MOBY_WIDE_ADMIT) return first;
    const uint32_t word = cpu->read_word(header + 0x08u);
    const int canon = s23_sphere_arms(mac, r, 1.0, 1.0) == MOBY_WIDE_ADMIT;
    if (canon && (!word || s23_sphere_inside(mac, r, 1.0, 1.0))) return MOBY_WIDE_CANONICAL;
    if (!word || s23_sphere_inside(mac, r, widen, vwiden)) return MOBY_WIDE_ADMIT;
    int64_t c2[3];
    const int64_t r2 = s23_second_sphere(cpu, moby, R, word, mac, c2);
    if (canon) {
        const moby_wide_verdict_t v = s23_sphere_arms(c2, r2, 1.0, 1.0);
        if (v == MOBY_WIDE_ADMIT) return MOBY_WIDE_CANONICAL;
        if (v != MOBY_WIDE_LATERAL && v != MOBY_WIDE_VERTICAL) return MOBY_WIDE_SPHERE2;
    }
    return s23_sphere_arms(c2, r2, widen, vwiden) == MOBY_WIDE_ADMIT ? MOBY_WIDE_ADMIT
                                                                     : MOBY_WIDE_SPHERE2;
}

/** @brief Saturating narrow of a full-width intermediate to the range the cull's register or field holds.
 *
 * Every intermediate the cull stores lands in an int16 GTE
 * register or an int16 record field, which wraps once extended LOD reveals a
 * moby past the range the cull could ever queue. Inside that range this is the
 * identity, past it a saturated value stays at the end of the range instead of
 * reappearing at the other end.
 * @param v The value.
 * @param lo The range's low end.
 * @param hi The range's high end.
 * @return v clamped to [lo, hi]. */
static int64_t s23_sat(int64_t v, int64_t lo, int64_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/** @brief The animated record's translation, composed as the cull composes it at full width.
 *
 * The quarter-world view-space centre shifted left 2 and right by the model's
 * scale byte, then rescaled by the moby's size byte through a GPF and a shift
 * of 5
 * (mobydraw.s:510,537-539,546-551,572-574). The size byte scales the moby on
 * screen by moving it in depth, 32 being unity.
 *
 * The GTE holds both the GPF's input and its product in int16 registers.
 * Inside the cull's own distance box neither bound is reachable: the box caps
 * the centre at the render-radius byte scaled by 256, so the shift left 2
 * lands inside an int16 and the size byte is a fraction near unity. Past the
 * box - which is the whole of what this builder composes for - the input bound
 * alone collapses all three axes to the same 1023 and stands the moby on the
 * near plane at the size of the screen. Carrying the rescale at full width is
 * the treatment the centre's own MVMVA already takes, and is identical to the
 * GTE everywhere the GTE's own range holds.
 *
 * The cull drops a record whose result passes 63000 (mobydraw.s:546-551); that
 * is a range limit, which is what extended LOD lifts, so it is not applied.
 * @param f The animated fields, whose centre is used.
 * @param scale The model's scale byte, clamped to [0, 31].
 * @param size The moby's size byte; 0 skips the rescale.
 * @param tr Receives the translation, saturated to int32. */
static void s23_anim_translation(const moby_anim_fields_t* f, int scale, int size,
                                 int32_t tr[3]) {
    for (int i = 0; i < 3; i++) {
        int64_t v = ((int64_t)f->centre[i] << 2) >> (int)s23_sat(scale, 0, 31);
        if (size) v = (v * size) >> 5;
        tr[i] = (int32_t)s23_sat(v, INT32_MIN, INT32_MAX);
    }
}

/** @brief Composes the animated record's 0x44 bytes, the inverse of s23_read_record.
 *
 * The flags word carries the cull's draw verdict in byte 0, the keyframe-blend
 * byte in byte 1 (mobydraw.s:459-461,522) and the LOD verdict in its top half
 * (mobydraw.s:501,918-921). The matrix is five words, and the ordering-table
 * key shares the spare half of the last (mobydraw.s:344-348,559). The verdict
 * byte is the near one: the far verdict only ever narrows what the record
 * draws, and extended LOD is the rule that it is drawn.
 * @param w Receives the record's words.
 * @param f The animated fields: headers, keyframe B, blend byte and matrix.
 * @param moby The Moby address.
 * @param material The record's material word.
 * @param frame_a Keyframe A's reference.
 * @param patch The faceblock patch list, or 0.
 * @param tier The LOD verdict.
 * @param key The ordering-table key.
 * @param tr The translation. */
static void s23_write_anim_record(uint32_t* w, const moby_anim_fields_t* f,
                                  uint32_t moby, uint32_t material,
                                  uint32_t frame_a, uint32_t patch,
                                  int tier, int key, const int32_t tr[3]) {
    const uint32_t L = game_layout.moby.rec_matrix_off / 4u;
    w[0] = ((uint32_t)(tier & 0xFFFF) << 16) | (f->blend << 8) | 1u;
    w[game_layout.moby.rec_header_off   / 4u] = f->header;
    w[game_layout.moby.rec_header_b_off / 4u] = f->header_b;
    w[game_layout.moby.rec_frame_a_off  / 4u] = frame_a;
    w[game_layout.moby.rec_frame_b_off  / 4u] = f->frame_b;
    w[0x14u / 4u]                             = patch;
    for (int i = 0; i < 3; i++)
        w[game_layout.moby.rec_tr_off / 4u + (uint32_t)i] = (uint32_t)tr[i];
    w[L + 0u] = ((uint32_t)(f->M[0][1] & 0xFFFF) << 16) | (uint32_t)(f->M[0][0] & 0xFFFF);
    w[L + 1u] = ((uint32_t)(f->M[1][0] & 0xFFFF) << 16) | (uint32_t)(f->M[0][2] & 0xFFFF);
    w[L + 2u] = ((uint32_t)(f->M[1][2] & 0xFFFF) << 16) | (uint32_t)(f->M[1][1] & 0xFFFF);
    w[L + 3u] = ((uint32_t)(f->M[2][1] & 0xFFFF) << 16) | (uint32_t)(f->M[2][0] & 0xFFFF);
    w[L + 4u] = ((uint32_t)(uint16_t)key      << 16) | (uint32_t)(f->M[2][2] & 0xFFFF);
    w[game_layout.moby.rec_material_off / 4u] = material;
    w[game_layout.moby.rec_moby_off     / 4u] = moby;
}

/** @brief The keyframe reference the record stores, and the faceblock patch list that rides with it.
 *
 * A record on the low-poly tier carries the keyframe word with its top bits
 * set, which is how the draw skips the patch apply
 * (mobydraw.s:501,504-505); otherwise the list is resolved through the Model's
 * own table, whose base is Model+0x34, indexed by the keyframe word's top byte
 * masked to 0x7C, and the word read there is the list's byte offset from that
 * same base (mobydraw.s:511-517).
 * @param cpu The guest CPU state, read only.
 * @param f The animated fields.
 * @param tier The record's LOD verdict; negative is the low-poly tier.
 * @param patch Receives the patch list, or 0 when there is none.
 * @return The keyframe reference. */
static uint32_t s23_anim_keyframe_ref(CPUState* cpu, const moby_anim_fields_t* f,
                                      int tier, uint32_t* patch) {
    *patch = 0u;
    if (tier < 0) return f->frame_a | 0xFC000000u;
    const uint32_t base = cpu->read_word(f->model + 0x34u);
    if (base >= 0x80000000u)
        *patch = base + cpu->read_word(base + ((f->frame_a >> 24) & 0x7Cu));
    return f->frame_a;
}

/** @brief One axis's table sine and cosine at a byte angle and a 4-bit fraction, as the cull's second composition reads them.
 *
 * The angle is (byte << 4) + fraction; zero skips the axis (S3 mobydraw.s
 * 0x80030DF0, 0x80030EF0, 0x80030FF0). Otherwise the sine is the
 * sign-extended halfword at sin_table + 2 * byte and the cosine the one
 * 0x80 bytes on; a non-zero fraction moves each toward the next entry by
 * ((next - this) * fraction) >> 4, arithmetic (0x80030E08-0x80030E4C).
 * @param cpu Reads the game's sin/cos table.
 * @param byte The angle byte.
 * @param frac The fraction, 0-15.
 * @param s Receives the sine; 0 when the axis is skipped.
 * @param c Receives the cosine; 4096 when the axis is skipped.
 * @return 0 when the axis is skipped, else 1. */
static int s23_fine_sincos(CPUState* cpu, uint32_t byte, uint32_t frac, int* s, int* c) {
    const uint32_t tbl = game_layout.sin_table;
    *s = 0; *c = 4096;
    if (!tbl || ((byte << 4) | frac) == 0u) return 0;
    const uint32_t a = tbl + byte * 2u;
    const int s0 = (int16_t)cpu->read_half(a),         s1 = (int16_t)cpu->read_half(a + 2u);
    const int c0 = (int16_t)cpu->read_half(a + 0x80u), c1 = (int16_t)cpu->read_half(a + 0x82u);
    *s = s0 + (int)((int32_t)((s1 - s0) * (int)frac) >> 4);
    *c = c0 + (int)((int32_t)((c1 - c0) * (int)frac) >> 4);
    return 1;
}

/** @brief Recomposes a revealed record's rotation by the cull's second form when the colour word has bit 30 set, and returns the material word the cull stores.
 *
 * Bit 30 of Moby+0x54 sends the cull to .L80030DBC (S3 mobydraw.s 0x800307B4;
 * S2 0x80043B94 to .L8004419C) in place of the byte-angle composition. The
 * axis order is the same, camera * Ry * Rx * Rz, each angle byte of Moby+0x44
 * (byte 2 Y, byte 1 X, byte 0 Z) carrying a 4-bit fraction from the colour
 * word (bits 16-19, 8-11 and 0-3; s23_fine_sincos). The sine and cosine are
 * read signed, and VXY0 takes them as whole words: a negative cosine puts -1
 * in VY0 of the Y step and borrows 1 from the sine above it in the Z step.
 * With bit 28 set the cull then negates column 1 of the product
 * (0x800310E4-0x80031120), the mirror the twin of a reflected moby carries;
 * with it clear the record's material word is stored as 0 (0x80030DC8).
 * The ordinary form stores the word as it is.
 * @param cpu Reads the moby and the sin/cos table.
 * @param moby The Moby address.
 * @param colour The moby's colour word (Moby+0x54).
 * @param f The animated fields; `M` is recomposed against `cam` under bit 30.
 * @return The material word the record carries. */
static uint32_t s23_reveal_other_form(CPUState* cpu, uint32_t moby, uint32_t colour,
                                      moby_anim_fields_t* f) {
    if (!(colour & 0x40000000u)) return colour;
    const uint32_t ang = cpu->read_word(moby + game_layout.moby.angle_off);
    int sy, cy, sx, cx, sz, cz;
    const int ky = s23_fine_sincos(cpu, (ang >> 16) & 0xFFu, (colour >> 16) & 0xFu, &sy, &cy);
    s23_fine_sincos(cpu, (ang >> 8) & 0xFFu, (colour >> 8) & 0xFu, &sx, &cx);
    const int kz = s23_fine_sincos(cpu, ang & 0xFFu, colour & 0xFu, &sz, &cz);
    const int Ry[3][3] = {{cy, 0, -sy}, {-(ky && cy < 0), 4096, 0}, {sy, 0, cy}};
    const int Rx[3][3] = {{4096, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
    const int Rz[3][3] = {{cz, -sz, 0}, {sz - (kz && cz < 0), cz, 0}, {0, 0, 4096}};
    int t1[3][3], t2[3][3];
    ngte_mat_mul3((const int (*)[3])f->cam, Ry, t1);
    ngte_mat_mul3((const int (*)[3])t1, Rx, t2);
    ngte_mat_mul3((const int (*)[3])t2, Rz, f->M);
    if (!(colour & 0x10000000u)) return 0u;
    for (int r = 0; r < 3; r++) f->M[r][1] = (int16_t)-f->M[r][1];
    return colour;
}

/** @brief Builds an animated model's cull record for one moby the box rejected and appends it to the host record list.
 *
 * The record is composed the way the cull composes it and appended to the host copy of the cull's own
 * record list. The regular walk reads that copy, so an appended record decodes
 * through exactly the path a queued one does and a sub-tick present replays it
 * with the rest.
 *
 * The LOD verdict the record carries is
 *
 *   tier = (Moby+0x4E low byte << 8) - the view-space depth before the
 *          translation rescale                              (mobydraw.s:450-456)
 *
 * and its sign picks the low-poly vertex count and face list. The cull stores
 * it in a halfword, which a revealed moby's depth overruns; a wrapped verdict
 * re-enters the near band and re-fades a record that is unambiguously
 * far-tier, so it saturates. The walk applies the extended model rule to the
 * stored verdict per fed record, as it does for a queued one.
 *
 * The ordering-table key is the rescaled depth less 0x200 shifted by the
 * model's span byte, halved (mobydraw.s:554-559). Like the far drop it is a
 * range limit rather than paint order, so it saturates rather than wrapping;
 * the native draw reads neither it nor the ordering-table slot, which the
 * depth buffer replaces.
 *
 * Nothing here writes guest memory: the copy is the host's and the moby's own
 * verdict byte stays the cull's.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param why Receives the reason: MOBY_REVEAL_OK, or why no record was appended.
 * @return 1 when a record was appended, 0 otherwise. */
static int s23_reveal_anim_record(CPUState* cpu, uint32_t moby, moby_reveal_reason_t* why) {
    moby_anim_fields_t f;
    if (!moby_reveal_anim_fields(cpu, moby, &f)) { *why = MOBY_REVEAL_NO_POSE; return 0; }
    const uint32_t material = s23_reveal_other_form(cpu, moby,
        cpu->read_word(moby + game_layout.moby.colour_off), &f);
    if (moby_rec_holds_moby(cpu, moby)) { *why = MOBY_REVEAL_DUPLICATE; return 0; }
    uint32_t* w = moby_rec_append(NULL);
    if (!w) { *why = MOBY_REVEAL_QUEUE_FULL; return 0; }

    const uint32_t lod = cpu->read_half(moby + 0x4Eu);   /* the LOD byte, then the size byte */
    const int tier = (int)s23_sat(((int64_t)(lod & 0xFFu) << 8) - (int64_t)f.centre[2],
                                  -32768, 32767);
    int32_t tr[3];
    s23_anim_translation(&f, (int)(cpu->read_byte(f.header + game_layout.moby.anim_scale_off) & 0xFFu),
                         (int)((lod >> 8) & 0xFFu), tr);
    uint32_t patch = 0u;
    const uint32_t frame_a = s23_anim_keyframe_ref(cpu, &f, tier, &patch);
    /* The span the ordering-table key subtracts is the larger of the two
     * keyframes' own (mobydraw.s:495-498). */
    int span = (int)cpu->read_byte(f.header + 0x03u);
    const int span_b = (int)cpu->read_byte(f.header_b + 0x03u);
    if (f.blend && span_b > span) span = span_b;
    const int key = (int)s23_sat((((int64_t)tr[2] - ((int64_t)0x200 << (int)s23_sat(span, 0, 30))) >> 1),
                                 -32768, 32767);
    s23_write_anim_record(w, &f, moby, material, frame_a, patch, tier, key, tr);
    /* The world ordering-table splice at +0x18: the bucket base from the
     * depth before the rescale less the moby's depth offset (the angle
     * word's top byte), floored at 0 and moved 0x20 buckets on at 0x110 and
     * beyond; +0x1A the span, +0x1B the span plus the scale byte
     * (mobydraw.s 0x80043FA4-0x8004402C). */
    const int dofs = (int)(int8_t)(cpu->read_word(moby + game_layout.moby.angle_off) >> 24);
    int wbase = (int)((((int64_t)f.centre[2] * 4) >> 7) - dofs);
    wbase = moby_otk_world_key(wbase < 0 ? 0 : wbase);
    const int scale = (int)(cpu->read_byte(f.header + game_layout.moby.anim_scale_off) & 0xFFu);
    w[0x18u / 4u] = (uint32_t)(wbase & 0xFFFF) | ((uint32_t)(span & 0xFF) << 16)
                  | ((uint32_t)((scale + span) & 0xFF) << 24);
    *why = MOBY_REVEAL_OK;
    return 1;
}

/** @brief Builds one rejected moby's record for the queue of its model kind.
 *
 * An animated model goes to s23_reveal_anim_record. A simple model's queue
 * record is built the way the cull composes it (mobydraw.s:604-630): the Moby, the three quarter-world
 * camera deltas - (moby - cam) >> 2 on the first component and (cam - moby)
 * >> 2 on the other two (mobydraw.s:81,87,94) - and the model's animation slot
 * at Model + (Moby+0x3C byte 0) * 4. A model-table entry that is KSEG0 is an
 * animated model and belongs to the other queue (mobydraw.s:116).
 *
 * The deltas also go into the full-width fields, since a revealed moby sits
 * past what the packed halves can hold, and the far-cull distance is -1: a
 * host-built record carries no range limit, which is what extended LOD means.
 * Nothing here writes guest memory - the queue is the host copy taken at the
 * cull hook, and the moby's own verdict byte stays the cull's.
 * @param cpu The guest CPU state, read only.
 * @param moby The Moby address.
 * @param kind The queue being filled; a moby of the other kind is refused.
 * @param why Receives the reason: MOBY_REVEAL_OK, or why no record was built.
 * @return 1 when a record was built, 0 otherwise. */
static int s23_reveal_record(CPUState* cpu, uint32_t moby, moby_reveal_kind_t kind,
                             moby_reveal_reason_t* why) {
    const uint32_t cls   = cpu->read_half(moby + game_layout.moby.class_off);
    const uint32_t model = cpu->read_word(game_layout.moby.model_table + cls * 4u);
    if (model == 0u) { *why = MOBY_REVEAL_NO_MODEL; return 0; }
    if (s23_model_is_animated(cpu, model) != (kind == MOBY_REVEAL_ANIMATED)) {
        *why = MOBY_REVEAL_OTHER_KIND; return 0;
    }
    if (kind == MOBY_REVEAL_ANIMATED) return s23_reveal_anim_record(cpu, moby, why);
    if (s_sq->qn >= S23_SIMPLE_QCAP) { *why = MOBY_REVEAL_QUEUE_FULL; return 0; }
    /* The cull admits on more than the box, so a moby outside the box can
     * already hold a captured record; a second one would draw it twice. */
    for (int k = 0; k < s_sq->qn; k++)
        if (s_sq->q[k][0] == moby) { *why = MOBY_REVEAL_DUPLICATE; return 0; }
    const uint32_t anim = cpu->read_word(moby + game_layout.moby.anim_state_off);
    const int32_t cpos[3] = { (int32_t)cpu->read_word(game_layout.cam.position + 0u),
                              (int32_t)cpu->read_word(game_layout.cam.position + 4u),
                              (int32_t)cpu->read_word(game_layout.cam.position + 8u) };
    int32_t d[3];
    moby_cull_centre(cpu, moby, cpos, NULL, 0, d, NULL);
    const int32_t dx = d[0], dy = d[1], dz = d[2];
    const int i = s_sq->qn;
    s_sq->q[i][0] = moby;
    s_sq->q[i][1] = ((uint32_t)dx << 16) | ((uint32_t)dy & 0xFFFFu);
    /* The low half stays clear of bit 15, which marks the screen-space kind
     * the draw reads its own halves as (mobydraw.s:4030-4049). */
    s_sq->q[i][2] = ((uint32_t)dz << 16);
    s_sq->q[i][3] = model + (anim & 0xFFu) * 4u;
    s_sq->d[i][0] = dy;
    s_sq->d[i][1] = dz;
    s_sq->d[i][2] = dx;
    s_sq->far[i]  = -1;
    s_sq->qn = i + 1;
    return 1;
}

/** @brief The simple models' pass on its draw hook: capture the cull's queue, run the game's draw, decode the queue natively.
 *
 * The game's own draw writes the drawn verdict and the prims. Before it runs,
 * the pass appends the extended-LOD reveal records to the host queue and
 * snapshots the four flat-material scratchpad words the draw starts from;
 * after it, the queued Mobys are stashed for interpolation and the queue is
 * decoded. Prints a census line under moby_verify_vbl.
 * @param cpu The guest CPU state; the game's draw writes guest memory.
 * @return The number of triangles emitted. */
static int s23_simple_pass(CPUState* cpu) OP_REQUIRES(sim) {
    s_simple_recs = s_simple_verts = s_simple_faces = s_simple_tris = 0;
    s_simple_flat = s_simple_screen = s_simple_culled = s_simple_screen_drawn = 0;
    s_simple_lp = s_simple_fade = s_simple_occl = 0;
    s_flat_zero_colour = s_flat_back_emitted = s_flat_back_dropped = s_flat_front_blended = 0;
    /* The pass runs inside the bracket's guest region; the host work around
     * the game's draw opens its own regions (msim.capture, msim.decode). */
    int revealed;
    { OP_PROF_ACCUM_SCOPE("msim.capture"); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_MOBY);
      s23_simple_capture(cpu);
      /* The records the cull rejected on its distance box (extended LOD) or on
       * the sector byte alone (every --lod) are rebuilt here and decoded with
       * the rest. The queue this appends to is the host copy above; the
       * game's own draw below still reads its own, unchanged. */
      revealed = moby_reveal_candidates(cpu, MOBY_REVEAL_SIMPLE);
      for (int i = 0; i < 4; i++)
          s_sq->flat[i] = cpu->read_word(S23_FLAT_SCRATCH + (uint32_t)i * 4u); }
    openpete_recomp_module_get()->dispatch(cpu, game_layout.moby.hook_draw_simple);
    int n;
    { OP_PROF_ACCUM_SCOPE("msim.decode"); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_MOBY);
      /* The records as the decode below reads them, after the draw's own stores
       * (byte +0x4D) and before any later routine of the tick writes a queued
       * moby (the HUD update advances a HUD moby's angle after this draw), so
       * the object ring's pair takes the drawn records, not the tick-end ones. */
      game_interp_moby_stash_begin();
      for (int i = 0; i < s_sq->qn; i++) game_interp_moby_stash_put(s_sq->q[i][0]);
      n = s23_simple_decode(cpu); }
    if (moby_verify_vbl())
        fprintf(stderr, "[moby-simple vbl=%u] queued=%d drawn=%ld culled=%ld screenkind=%ld "
                        "screendrawn=%ld "
                        "verts=%ld faces=%ld flat=%ld tris=%d revealed=%d lp=%ld fade=%ld "
                        "flat_zero_colour=%ld flat_back_emitted=%ld flat_back_dropped=%ld "
                        "flat_front_blended=%ld\n",
                openpete_frame, s_sq->qn, s_simple_recs, s_simple_culled, s_simple_screen,
                s_simple_screen_drawn,
                s_simple_verts, s_simple_faces, s_simple_flat, n, revealed,
                s_simple_lp, s_simple_fade,
                s_flat_zero_colour, s_flat_back_emitted, s_flat_back_dropped,
                s_flat_front_blended);
    moby_kind_census("canon", "simple", s_sq->qn, s_simple_occl, s_simple_verts, n);
    s23_simple_base_census();
    return n;
}

/** @brief The animated record's paint-key inputs for moby_rank.h row MOBY_OTK_S23_REGULAR.
 *
 * A = the animation header's byte 3, raised to keyframe B's header byte 3 on
 * a blending record (w0 bits 8-15 non-zero, keyframe B's header a pointer) (mobydraw.s 474-483, 496, 499);
 * the translation the draw loads is the record's own (the cull stored the
 * final one, mobydraw.s 557-560). The face bias is the signed low three bits
 * of the face's second word, decoded into moby_wface_t.otbias. The
 * mirrored-winding material modes and the plane walk (record +0x3C) key the
 * same here.
 * @param cpu The guest CPU state, read only.
 * @param header Keyframe A's AnimationHeader.
 * @param header_b Keyframe B's AnimationHeader.
 * @param w0 The record's flags word.
 * @param TR_raw The record's translation.
 * @param tr Receives the translation the key reads.
 * @param a Receives A.
 * @return MOBY_OTK_S23_REGULAR, or MOBY_OTK_NONE when `header` is not a pointer. */
static moby_otk_row_t s23_otkey_record(CPUState* cpu, uint32_t header, uint32_t header_b,
                                       uint32_t w0, const int TR_raw[3], int tr[3], int* a) {
    if (header < 0x80000000u) return MOBY_OTK_NONE;
    for (int k = 0; k < 3; k++) tr[k] = TR_raw[k];
    int h = cpu->read_byte(header + 3u) & 0xFF;
    if (((w0 >> 8) & 0xFFu) && header_b >= 0x80000000u) {
        int b = cpu->read_byte(header_b + 3u) & 0xFF;
        if (b > h) h = b;
    }
    *a = h;
    return MOBY_OTK_S23_REGULAR;
}

/** @brief Whether the resident overlay is one game_layout.moby.cutpath_overlays names by prefix.
 *
 * @return 1 when its name starts with one of the space-separated prefixes,
 *         else 0 (no overlay resident, or no list). */
static int cutpath_overlay_runs(void) {
    const char* list = game_layout.moby.cutpath_overlays;
    const spyro_overlay_entry_t* ov = spyro_overlay_find(spyro_get_active_overlay_magic());
    if (!list || !ov || !ov->name) return 0;
    while (*list) {
        size_t n = strcspn(list, " ");
        if (n && strncmp(ov->name, list, n) == 0) return 1;
        list += n;
        while (*list == ' ') list++;
    }
    return 0;
}

/* A placed step continues a run of the track, and is a motion rather than a
 * teleport, when a neighbouring segment is also placed (cutpath_snap or
 * more), is within this factor of the step's length and keeps its heading
 * (the cosine between the two at least CUTPATH_RUN_MIN_COS). A parked actor
 * toggling between two far points makes equal-length steps that reverse;
 * a fall, a run or a launch keeps its heading. */
#define CUTPATH_RUN_LEN_RATIO 1.25
#define CUTPATH_RUN_MIN_COS   0.25

/** @brief Read one guest word through the host span.
 *
 * @param addr The guest address.
 * @param out Receives the word.
 * @return 1 when the address maps, else 0. */
static int cutpath_rd32(uint32_t addr, uint32_t* out) {
    const void* p = openpete_psx_to_host_span(addr, 4u);
    if (!p) return 0;
    memcpy(out, p, 4);
    return 1;
}

/** @brief The number of points in one sampler actor's track for one chunk.
 *
 * The first byte of the chunk's frame list (frame-list pointer + 0x3C +
 * 4 * chunk), the count the sampler walks its segments by.
 * @param actor The actor's index in the moby array.
 * @param chunk The chunk (the record's animation id byte).
 * @return The count, or 0 when the actor has no list or a pointer does not map. */
static int cutpath_track_count(uint32_t actor, uint32_t chunk) {
    uint32_t fl = 0, list = 0;
    if (!cutpath_rd32(game_layout.moby.cutpath_frames + actor * 4u, &fl) || fl < 0x80000000u) return 0;
    if (!cutpath_rd32(fl + 0x3Cu + chunk * 4u, &list) || list < 0x80000000u) return 0;
    const uint8_t* b = (const uint8_t*)openpete_psx_to_host_span(list, 1u);
    return b ? *b : 0;
}

/** @brief One point of a sampler actor's track, decoded as the sampler decodes it.
 *
 * The packed word holds three 10-bit fields, each placed at bit 8 of its
 * coordinate (and 0x3FF00000 sra 12, and 0xFFC00 sra 2, andi 0x3FF sll 8).
 * @param actor The actor's index in the moby array.
 * @param chunk The chunk; its low bit picks the point buffer.
 * @param idx The point index, below the chunk's point count.
 * @param out Receives the world position.
 * @return 1 when the index is in the track and every pointer maps, else 0. */
static int cutpath_track_point(uint32_t actor, uint32_t chunk, int idx, int32_t out[3]) {
    uint32_t buf = 0, off = 0, w = 0;
    if (idx < 0 || idx >= cutpath_track_count(actor, chunk)) return 0;
    if (!cutpath_rd32(game_layout.moby.cutpath_points + (chunk & 1u) * 4u, &buf)) return 0;
    if (buf < 0x80000000u || !cutpath_rd32(buf + 0xCu + actor * 4u, &off)) return 0;
    if (!cutpath_rd32(buf + off + (uint32_t)idx * 4u, &w)) return 0;
    out[0] = (int32_t)((w & 0x3FF00000u) >> 12);
    out[1] = (int32_t)((w & 0x000FFC00u) >> 2);
    out[2] = (int32_t)((w & 0x000003FFu) << 8);
    return 1;
}

/** @brief The track index, among a record's two sampled frames (+0x3E, +0x3F), whose point is the record's position.
 *
 * @param actor The actor's index in the moby array.
 * @param rec The moby record.
 * @return The index, or -1 when neither point is the position. */
static int cutpath_track_index(uint32_t actor, const uint8_t* rec) {
    const uint32_t chunk = rec[game_layout.moby.anim_state_off];
    int32_t pos[3], pt[3];
    memcpy(pos, rec + game_layout.moby.pos_off, 12);
    for (uint32_t k = 2; k < 4; k++) {
        const int idx = rec[game_layout.moby.anim_state_off + k];
        if (cutpath_track_point(actor, chunk, idx, pt) && !memcmp(pt, pos, 12)) return idx;
    }
    return -1;
}

/** @brief The track segment from point a of chunk ka to point b of chunk kb, as a vector.
 *
 * @param actor The actor's index in the moby array.
 * @param ka The first point's chunk.
 * @param a The first point.
 * @param kb The second point's chunk.
 * @param b The second point.
 * @param out Receives b - a.
 * @return 1 when both points are in their tracks, else 0. */
static int cutpath_seg(uint32_t actor, uint32_t ka, int a, uint32_t kb, int b, double out[3]) {
    int32_t pa[3], pb[3];
    if (!cutpath_track_point(actor, ka, a, pa) || !cutpath_track_point(actor, kb, b, pb)) return 0;
    for (int k = 0; k < 3; k++) out[k] = (double)pb[k] - pa[k];
    return 1;
}

/** @brief The track segment that ends at point p of chunk k.
 *
 * Point p - 1 to p, or, at a chunk's first point, the previous chunk's last
 * segment when that chunk ends on the same point.
 * @param actor The actor's index in the moby array.
 * @param k The chunk.
 * @param p The point.
 * @param out Receives the segment.
 * @return 1 when the segment exists, else 0. */
static int cutpath_seg_into(uint32_t actor, uint32_t k, int p, double out[3]) {
    if (p > 0) return cutpath_seg(actor, k, p - 1, k, p, out);
    if (k == 0) return 0;
    const int n = cutpath_track_count(actor, k - 1u);
    double join[3];
    if (!cutpath_seg(actor, k - 1u, n - 1, k, 0, join) || join[0] || join[1] || join[2]) return 0;
    return cutpath_seg(actor, k - 1u, n - 2, k - 1u, n - 1, out);
}

/** @brief The track segment that starts at point c of chunk k.
 *
 * Point c to c + 1, or, at a chunk's last point, the next chunk's first
 * segment when that chunk starts on the same point.
 * @param actor The actor's index in the moby array.
 * @param k The chunk.
 * @param c The point.
 * @param out Receives the segment.
 * @return 1 when the segment exists, else 0. */
static int cutpath_seg_out(uint32_t actor, uint32_t k, int c, double out[3]) {
    if (c + 1 < cutpath_track_count(actor, k)) return cutpath_seg(actor, k, c, k, c + 1, out);
    double join[3];
    if (!cutpath_seg(actor, k, c, k + 1u, 0, join) || join[0] || join[1] || join[2]) return 0;
    return cutpath_seg(actor, k + 1u, 0, k + 1u, 1, out);
}

/** @brief Whether a neighbouring segment continues a placed step: placed itself, of comparable length and on the same heading.
 *
 * @param step The step, tick N-1 to tick N.
 * @param len The step's length.
 * @param seg The neighbouring segment.
 * @param cos_out Receives the cosine between the two, 0 when seg has no length.
 * @return 1 when seg is cutpath_snap or more, within CUTPATH_RUN_LEN_RATIO of
 *         len and at least CUTPATH_RUN_MIN_COS in cosine, else 0. */
static int cutpath_continues(const double step[3], double len, const double seg[3], double* cos_out) {
    const double sl = sqrt(seg[0]*seg[0] + seg[1]*seg[1] + seg[2]*seg[2]);
    *cos_out = sl > 0.0 ? (step[0]*seg[0] + step[1]*seg[1] + step[2]*seg[2]) / (len * sl) : 0.0;
    if (sl < (double)game_layout.moby.cutpath_snap) return 0;
    if (sl > CUTPATH_RUN_LEN_RATIO * len || len > CUTPATH_RUN_LEN_RATIO * sl) return 0;
    return *cos_out >= CUTPATH_RUN_MIN_COS;
}

/** @brief Print one placed step's census line (OPENPETE_CUTPATH_CENSUS), once per moby and position pair.
 *
 * @param moby The moby's guest address.
 * @param pp Tick N-1's position.
 * @param cp Tick N's position.
 * @param located Whether both positions are points of the track.
 * @param len The step's length.
 * @param nb The lengths of the segments into and out of the step, -1 where none.
 * @param cs Their cosines with the step.
 * @param jump The verdict. */
static void cutpath_census(uint32_t moby, const int32_t pp[3], const int32_t cp[3], int located,
                           double len, const double nb[2], const double cs[2], int jump) {
    static int s_on = -1;
    if (s_on < 0) { const char* e = getenv("OPENPETE_CUTPATH_CENSUS"); s_on = (e && *e && *e != '0'); }
    if (!s_on) return;
    /* The verdict is taken at the capture and again on presents. */
    static uint32_t s_moby[64], s_pos[64];
    const unsigned slot = (moby / game_layout.moby.array_stride) & 63u;
    uint32_t h = 2166136261u;
    for (int k = 0; k < 3; k++) h = ((h ^ (uint32_t)pp[k]) * 16777619u ^ (uint32_t)cp[k]) * 16777619u;
    if (s_moby[slot] == moby && s_pos[slot] == h) return;
    s_moby[slot] = moby; s_pos[slot] = h;
    fprintf(stderr, "[cutpath-census vbl=%u] obj=%08x located=%d len=%.1f into=%.1f cos=%.2f "
                    "out=%.1f cos=%.2f verdict=%s pp=%d,%d,%d cp=%d,%d,%d\n",
            openpete_feed_key_now(), moby, located, len, nb[0], cs[0], nb[1], cs[1],
            jump ? "jump" : "lerp", pp[0], pp[1], pp[2], cp[0], cp[1], cp[2]);
}

/** @brief Whether a placed step of a sampler actor continues a run of its track, with the neighbours it was judged on.
 *
 * prev and curr each sit on one of their record's two sampled frames (the
 * sampler writes the point itself when it holds or places). The neighbours
 * are the segment into the previous point, on the previous record's chunk,
 * and the segment out of the current point, on the current record's chunk.
 * @param actor The actor's index in the moby array.
 * @param prev_rec Tick N-1's record.
 * @param curr_rec Tick N's record.
 * @param step The step, tick N-1 to tick N.
 * @param len The step's length.
 * @param nb Receives the neighbours' lengths, -1 where none.
 * @param cs Receives their cosines with the step.
 * @return 1 when it continues a run, 0 when it does not, -1 when either
 *         position is not a point of the track. */
static int cutpath_run_step(uint32_t actor, const uint8_t* prev_rec, const uint8_t* curr_rec,
                            const double step[3], double len, double nb[2], double cs[2]) {
    const int p = cutpath_track_index(actor, prev_rec);
    const int c = cutpath_track_index(actor, curr_rec);
    if (p < 0 || c < 0) return -1;
    double seg[2][3];
    const int have[2] = {
        cutpath_seg_into(actor, prev_rec[game_layout.moby.anim_state_off], p, seg[0]),
        cutpath_seg_out(actor, curr_rec[game_layout.moby.anim_state_off], c, seg[1]),
    };
    int run = 0;
    for (int k = 0; k < 2; k++) {
        nb[k] = -1.0; cs[k] = 0.0;
        if (!have[k]) continue;
        nb[k] = sqrt(seg[k][0]*seg[k][0] + seg[k][1]*seg[k][1] + seg[k][2]*seg[k][2]);
        run |= cutpath_continues(step, len, seg[k], &cs[k]);
    }
    return run;
}

/** @brief The cutscene path sampler's verdict on one step of a moby it drives.
 *
 * The sampler runs only in the overlays game_layout.moby.cutpath_overlays
 * names (cutpath_overlay_runs); there it drives the first [cutpath_count]
 * records of the moby array (count taken only in 1..64) and places an actor
 * across any track segment of cutpath_snap world units or more instead of
 * interpolating it, while a shorter segment advances in sixteenths. The
 * game's length (func_8004EDE8 in Spyro 3, func_8001BA20 in Spyro 2) is an
 * integer square root of the same sum; the exact length is used here. A step
 * of cutpath_snap or more is a jump unless it continues a run of the track
 * (cutpath_run_step): a fast fall or run is placed one segment at a time and
 * interpolates, a teleport, a replacement in place or a parked actor's
 * toggle holds. A step whose ends are not points of the track is a jump.
 * @param moby The moby's guest address.
 * @param prev_rec Tick N-1's moby record.
 * @param curr_rec Tick N's moby record.
 * @return 1 for a jump, 0 for an interpolated step, -1 when the game names
 *         no sampler rule, no sampler overlay runs, or the moby is not one of
 *         its actors. */
static int s23_cutpath_jump(uint32_t moby, const uint8_t* prev_rec, const uint8_t* curr_rec) {
    const uint32_t cnt_addr = game_layout.moby.cutpath_count;
    if (!cnt_addr || !game_layout.moby.cutpath_snap) return -1;
    if (!cutpath_overlay_runs()) return -1;
    uint32_t cnt = 0, arr = 0;
    if (!cutpath_rd32(cnt_addr, &cnt) || !cutpath_rd32(game_layout.moby.array_head, &arr)) return -1;
    const uint32_t stride = game_layout.moby.array_stride;
    if (cnt == 0 || cnt > 64u || arr < 0x80000000u || moby < arr || !stride) return -1;
    if ((moby - arr) % stride || (moby - arr) / stride >= cnt) return -1;
    int32_t pp[3], cp[3];
    memcpy(pp, prev_rec + game_layout.moby.pos_off, 12);
    memcpy(cp, curr_rec + game_layout.moby.pos_off, 12);
    const double step[3] = { (double)cp[0] - pp[0], (double)cp[1] - pp[1], (double)cp[2] - pp[2] };
    const double len = sqrt(step[0]*step[0] + step[1]*step[1] + step[2]*step[2]);
    if (len < (double)game_layout.moby.cutpath_snap) return 0;
    double nb[2] = { -1.0, -1.0 }, cs[2] = { 0.0, 0.0 };
    const int run = game_layout.moby.cutpath_points
        ? cutpath_run_step((moby - arr) / stride, prev_rec, curr_rec, step, len, nb, cs) : -1;
    const int jump = run != 1;
    cutpath_census(moby, pp, cp, run >= 0, len, nb, cs, jump);
    return jump;
}

const moby_game_t moby_game_spyro23 = {
    .vstream_init      = s23_vstream_init,
    .vstream_next      = s23_vstream_next,
    .decode_faces      = s23_decode_faces,
    .read_record       = s23_read_record,
    .frame_patch_list  = s23_frame_patch_list,
    .frame_patch_body_first = 1,   /* mobydraw.s 0x80044648 (S2), 0x80031268 (S3) */
    .anim_blend_byte   = s23_anim_blend_byte,
    .model_is_animated = s23_model_is_animated,
    .simple_pass       = s23_simple_pass,
    .simple_replay     = s23_simple_replay,
    .simple_prev_mobys = s23_simple_prev_mobys,
    .simple_mobys      = s23_simple_mobys,
    /* The cull writes its verdict into a gameplay-readable Moby byte, so the
     * shared field reveal stays off (host_reveal below); these two let the
     * shared candidate walk offer the mobys the cull's distance box rejected
     * to this game's own queue builder instead. */
    .cull_box_threshold = s23_cull_box_threshold,
    .reveal_record      = s23_reveal_record,
    .cull_view_admits   = s23_cull_view_admits,
    .special_face      = s23_special_face,
    /* The heading the mode-1 bias reads is game_layout.moby.env_bias_word
     * (cam_heading 0); the routine keeps its bits 3-10, a period of 2048. */
    .cam_heading_period = 2048u,
    .material_tint     = s23_material_tint,
    .record_face_sign  = s23_record_face_sign,
    .water_plane       = s23_water_plane,
    .otkey_record      = s23_otkey_record,
    .otkey_splice      = s23_otkey_splice,
    /* The cull writes its verdict into a gameplay-readable Moby byte, so
     * nothing the host widens may reach it and the walk feeds the records the
     * cull queued, no more. */
    .host_reveal       = 0,
    /* The LOD/fade byte both model kinds' tiers read, whole
     * (mobydraw.s:453-457 animated, 4193-4196 simple). */
    .renderer_off      = 0x4Eu,
    .renderer_mask     = 0xFFu,
    /* The moby array is fixed with a live sentinel, so a record's Moby address
     * is its slot and is stable across ticks, except for the credits letters:
     * the credits draw compacts the drawn letters into consecutive slots, so
     * a letter keys by its CreditsLetter record instead (renderer/credits_key.h)
     * and every other moby by its address. */
    .interp_key        = game_credits_letter_key,
    .cutpath_jump      = s23_cutpath_jump,
};
