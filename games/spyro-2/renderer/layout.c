// games/spyro-2/renderer/layout.c, the Spyro 2 engine layout. Bases come from
// the drift-checked symbol aliases (manifest/spyro2_symbols.toml, each row
// carrying its evidence). Every offset is a literal whose comment names the
// asm site in games/spyro-2/spyro-2/asm that reads or writes it. A field with
// no located counterpart is 0, and the feeder that reads one refuses to run
// rather than read through address 0 (core/include/game_layout.h).
//
// Filled: the environment header, the terrain decode and binning switches, the
// texture tables, the environment animations, the visibility verdict, the fog
// terms, the draw buffers, the primitive allocator, the camera, the level id
// the world-draw, sky-draw and visibility hooks, the cyclorama header and the
// moby, shadow, particle and player groups. The portal group is still 0.

#include "game_layout.h"
#include "renderer/ui_game.h"
#include "spyro-2/gen/spyro2_symbols.gen.h"

#include <stddef.h>

/* The id word the titlescreen_and_loading overlay carries at the head of the
 * overlay region, the value the dispatcher matches it by (generated/
 * spyro_overlay_table.c). */
#define OVL_TITLESCREEN_AND_LOADING 34u
/* The guidebook overlay's id word, matched the same way. */
#define OVL_GUIDEBOOK 36u

const game_layout_t spyro2_game_layout = {
    .display = {
        /* func_80011BBC: SetDefDrawEnv(a2=0xC, a3=0x200, sp+0x10=0xD8) and
         * SetDefDispEnv(a3=0x200, sp+0x10=0xF0) (init.s:80011C00-80011C48) -
         * a 512x216 clip inside a 512x240 display, 12 undrawn rows per side. */
        .disp_w = 512, .disp_h = 240, .clip_h = 216,
    },
    .sin_table = G_SIN,      /* 256 s16 1.12 entries; func_8001B61C interpolates
                              * it from a 12-bit angle (stdutil.s:308-318) and
                              * func_8001B6C8 pairs it with the cos table
                              * D_80061C58 at +0x80 (stdutil.s:375-378) */
    .env = {
        .base            = G_ENVIRONMENT,
        .sector_ptr      = G_ENVIRONMENT + 0x00u,   /* world draw: lw $s3, 0x0 (environment.s:1594) */
        .sector_count    = G_ENVIRONMENT + 0x04u,   /* world draw: lw $v0, 0x4 (environment.s:1593) */
        .occ_groups      = G_ENVIRONMENT + 0x08u,   /* func_80048D18(0): lw $t6, 0x8 (mobyfunc.s:29) */
        .occ_group_count = 0u,                      /* no count word; the occlusion data carries its own header */
        .lq_tex          = G_ENVIRONMENT + 0x20u,   /* world draw: lw $s3, 0x20 (environment.s:2212); one
                                                     * table of 48-byte entries serves LQ and HQ tiles */
        .hq_tex          = G_ENVIRONMENT + 0x20u,   /* no separate HQ table: the four HQ sub-tiles sit
                                                     * inline at +0x10 of the same 48-byte entry, so the
                                                     * near subdivide reads the LQ table with
                                                     * terrain.hq_stride / terrain.hq_sub0 */
        .tex_count       = G_ENVIRONMENT + 0x24u,   /* the tile count the loader writes beside the table
                                                     * pointer; read by the texture census only */
        .lod_dist        = 0u,                      /* no header field: the draw bins LP/HP with the
                                                     * constants below */
        .cull_dist       = G_ENVIRONMENT + 0x2Cu,   /* the LP far cull, (v >> 7) - 1 against the LP OT
                                                     * key (environment.s:6924-6935) */
        .vis_aspect_independent = 1u,               /* func_80048D18 (mobyfunc.s:13-40) is a PVS lookup
                                                     * keyed on the camera position alone: it reads the
                                                     * Draw-time position copy shifted right by 11 and
                                                     * walks the byte-packed octree at
                                                     * g_Environment+0x08, with no matrix, frustum,
                                                     * aspect or FOV term */
    },
    /* The environment animations live in one inline-word block reached through
     * the word at g_Environment+0x1C. Four routines walk it, each entering with
     * `lw $t5, 0x1C($at)`, $at = g_Environment:
     *   func_80023778  blk+0x08              writes the sector colour array,
     *                                        skipping an entry with bit 31 set
     *                                        (environment.s:1236-1271)
     *   func_800238DC  blk+0x08              takes a sector index and a flag and
     *                                        toggles bit 31 of the matching
     *                                        entry (environment.s:1335-1352)
     *   func_80023994  blk+rd(blk+0x00)      writes the sector vertex bank from
     *                                        the cos table (environment.s:1390-1408)
     *   func_80023A9C  blk+rd(blk+0x04)      writes the same bank from the sin
     *                                        table (environment.s:1465-1483)
     * All four gate on the liveness byte table D_ENV_ANIM_LIVE, indexed by the
     * entry's low byte. The three `sub[]` rows are the three that rewrite
     * geometry or colour; func_800238DC only flips a skip bit in the list
     * sub[0] already walks, so it needs no row.
     *
     * The scrolling-texture animations live in a second registry, seven
     * {count, pointer to a descriptor-pointer array} pairs at +0x00 through
     * +0x34, walked in order by func_800225F8 and func_800231F4. Pair 0 memcpys
     * whole 48-byte texture entries between tiles, a discrete swap the tile
     * generation counter already snaps. Pair 1 is the UV scroll: a descriptor
     * holds a u16 tile index at +4 and a u16 accumulator at +6, and on a
     * keyframe advance the walker adds the keyframe's signed delta, masks the
     * accumulator to 7 bits and commits whole texels into the 48-byte tiledef
     * entry, acc>>2 and that +31 into the V bytes of both LQ halves (+1/+5,
     * +9/+13) and acc>>1 with its +31 into the HQ sub-tiles
     * (environment.s:138-155). `scroll` therefore names pair 1. */
    .env_anim = {
        .form       = ENV_ANIM_ARENA,
        .base       = D_ENV_ANIM_REGISTRY,
        .swap       = D_ENV_ANIM_REGISTRY + 0x00u,   /* func_800225F8 (environment.s:17-18) */
        .scroll     = D_ENV_ANIM_REGISTRY + 0x08u,
        /* The registry func_800225F8 walks: LP geometry, LP colour, HP geometry,
         * HP colour; the geometry walkers read the sector with lbu +4, the
         * colour walkers with lhu +4. */
        .lp_poly    = D_ENV_ANIM_REGISTRY + 0x10u,
        .lp_col     = D_ENV_ANIM_REGISTRY + 0x18u,
        .hp_poly    = D_ENV_ANIM_REGISTRY + 0x20u,
        .hp_col     = D_ENV_ANIM_REGISTRY + 0x28u,
        .poly_sec_u8 = 1,
        .block_ptr  = G_ENVIRONMENT + 0x1Cu,
        .live_table = D_ENV_ANIM_LIVE,
        /* Registry pairs 2 to 5. func_800225F8 advances each descriptor's
         * keyframe and stamps its list index into sector+0x18 / +0x19 /
         * +0x1A / +0x1B (environment.s:226, 283, 340, 397); the world draw
         * packs the posted pose and stores 0xFF there only for a tier it
         * lists (environment.s:1685-1737 bins the tiers; vertices 1743 LP,
         * 1935 HP; colours 1849 LP, 2046 HP). */
        .kf_lp_poly = D_ENV_ANIM_REGISTRY + 0x10u,
        .kf_hp_poly = D_ENV_ANIM_REGISTRY + 0x20u,
        .kf_lp_col  = D_ENV_ANIM_REGISTRY + 0x18u,
        .kf_hp_col  = D_ENV_ANIM_REGISTRY + 0x28u,
        .kf_hide    = 1,             /* environment.s:1756-1762, 1948-1954 */
        .sub = {
            { 0x08u, 0, 8u, 1, ENV_ANIM_KIND_COLOUR, ENV_ANIM_MODEL_COLOUR },
            { 0x00u, 1, 4u, 0, ENV_ANIM_KIND_VERT,   ENV_ANIM_MODEL_WAVE1 },
            { 0x04u, 1, 4u, 0, ENV_ANIM_KIND_VERT,   ENV_ANIM_MODEL_WAVE2 },
        },
        .wave_table = G_SIN,          /* the walkers' sine table; cosine at +0x80 */
        .timer      = 0x8006700Cu,   /* the timer word the walkers read (environment.s:1249, 1403, 1478) */
    },
    .visibility = {
        .base = 0x1F800000u,   /* one byte per sector, written by func_80048D18(0), consumed and
                                * cleared by the world draw's sector loop
                                * (environment.s:1603-1610); the range is general scratch outside
                                * that window */
    },
    .cyc = {
        .base            = G_CYCLORAMA,
        .sector_count    = G_CYCLORAMA + 0x00u,     /* sky draw: lw $ra, 0x0 (cyclorama.s:48); the walk
                                                     * ends when the visibility cursor reaches
                                                     * 0x1F8003C0 + count (cyclorama.s:52,66) */
        .sectors         = G_CYCLORAMA + 0x04u,     /* sky draw: lw $sp, 0x4 (cyclorama.s:49) */
        .occ_group_count = 0u,
        .occ_groups      = G_ENVIRONMENT + 0x0Cu,   /* func_80048D18(1): the argument shifts the header
                                                     * cursor by 4 before lw $t6, 0x8 (mobyfunc.s:19,24) */
        .bg_colour       = G_CYCLORAMA + 0x08u,     /* 0x00BBGGRR; the moby depth cue splits the same word
                                                     * into three 0xFF0 far-colour fields (mobydraw.s:2597-2604) */
        .vertex_base     = 0x14u,                   /* sky draw: addi $t4, $gp, 0x14 (cyclorama.s:89) */
        .face_layout     = CYC_FACES_SPYRO3,        /* vertex count = byte 0 of +0x0C (andi 0xFF,
                                                     * cyclorama.s:91), colour count = byte 1 (srl 6,
                                                     * andi 0x3FC, cyclorama.s:157-158); +0x10 = face-word
                                                     * list length << 16 | index-halfword list length
                                                     * (cyclorama.s:156,161,164); strips */
        .visibility      = 0x1F8003C0u,             /* one byte per sky sector, func_80048D18(1): 0xFF drawn,
                                                     * 0 skipped, 0x40 bytes zeroed first
                                                     * (cyclorama.s:50-51; mobyfunc.s:19,23-24,63,77) */
        .hook_args       = 0,                       /* the sky draw loads the camera +0x00 matrix for both
                                                     * the cull and the projection and zeroes the
                                                     * translation registers (cyclorama.s:30-45) */
        .fade_in_draw    = 1,                       /* DPCS over the colour table when the camera +0x40 word
                                                     * is nonzero, with the environment header's three
                                                     * far-colour words as FC (cyclorama.s:37,171,181-188) */
    },
    .fog = {
        .factor     = G_CAMERA + 0x40u,             /* the sky draw's IR0, loaded beside the
                                                     * projection matrix (cyclorama.s:39) */
        .far_colour = G_ENVIRONMENT + 0x34u,        /* three words R, G, B loaded into ctc2 $21..$23
                                                     * (environment.s:2479-2484), not a packed
                                                     * 0x00BBGGRR word */
        .far_colour_words = 1,
    },
    .db = {
        .cur_ptr = G_CUR_DB,
        .base    = G_DB,
        .stride  = 0x74u,                           /* func_8004D810 flips between G_DB and G_DB+0x74
                                                     * (drawutil/func_8004D810.s:46-52) */
        .drawenv = 0x00u,                           /* DRAWENV at +0x00; the primitive buffer base the
                                                     * cursor is reset from sits at +0x70
                                                     * (drawutil/func_8004D810.s:57) */
        .dispenv = 0x5Cu,                           /* the DISPENV PutDispEnv (func_80055CA0) takes
                                                     * from g_CurDB+0x5C (draw/func_800156FC.s:214-215) */
        .repeat_site = {
            /* func_8004D810 entry: MoveImage of the screen, then isbg zeroed
             * on both buffers (D_80069940, D_800699B4) after the wait
             * (drawutil/func_8004D810.s:12-41). */
            { 0x8004D82Cu, REPEAT_SITE_HOLD },
            /* func_800145AC state 0: the same copy and isbg zeroing inline
             * (draw/func_800145AC.s:79-88). */
            { 0x80014678u, REPEAT_SITE_HOLD },
            /* func_800156FC's draw in gamestates 4 and 9 (D_800681C8,
             * draw/func_800156FC.s:166-170, 205-208): the pause entry's first
             * tick submits an empty OT, and the next tick's MoveImage
             * (draw/func_800145AC.s:79) copies the displayed frame over the
             * buffer it cleared before a swap shows that clear. */
            { 0x80015974u, REPEAT_SITE_HOLD },
            /* func_8004C484: DrawSync, VSync, ClearImage of both buffers
             * black (drawutil/func_8004C484.s:11-29). */
            { 0x8004C49Cu, REPEAT_SITE_CLEAR_BLACK },
            /* func_800145AC waits whose next OT is the level's first tick,
             * drawn before the following wait, which the present at this
             * wait cannot show (draw/func_800145AC.s:261, 368). */
            { 0x80014928u, REPEAT_SITE_HOLD },
            { 0x80014ACCu, REPEAT_SITE_HOLD },
        },
    },
    .polybuf = {
        .cursor    = D_POLYBUF_CURSOR,
        .end       = D_POLYBUF_END,
        .sample_fn = F_UI_OT_STITCH, /* func_8001B2A8, the OT stitch before DrawOTag: every
                                      * caller (draw/func_800156FC.s:220 per present) runs it
                                      * after the state's draw has returned */
        .sample_ra = 0u,
        .scratch   = 0x3000u,       /* mobydraw.s:28-40, environment.s:1559-1563,
                                     * spyrodraw.s:28-30 carve at end - 0x3000 */
        .ot_ptr    = 0x80066FFCu,   /* init/func_80011DD4.s:41-53: the OT sits 8 bytes
                                     * (the 2D entry) above the second buffer's end */
        .db_base_off = 0x70u,       /* each draw buffer's primitive base word */
    },
    .cam = {
        .base        = G_CAMERA,
        .proj_matrix = G_CAMERA + 0x00u,            /* sky draw: ctc2 $0..$4 from +0x00..+0x10
                                                     * (cyclorama.s:32-40) */
        .view_matrix = G_CAMERA + 0x14u,            /* world draw: ctc2 $0..$4 from +0x14..+0x24
                                                     * (environment.s:1572-1581) */
        .position    = G_CAMERA + 0x28u,            /* world draw: lw $s7/$t8/$t9 from +0x28..+0x30,
                                                     * each shifted right 4 for the sector cull
                                                     * (environment.s:1585-1588) */
        .rotation    = G_CAMERA + 0x44u,            /* roll +0x44, pitch +0x46, yaw +0x48 */
        .xform_end   = G_CAMERA + 0x34u,            /* the Draw-time position copy the visibility
                                                     * pre-pass reads shifted right 11 (mobyfunc.s:32-39) */
    },
    .level_id = G_LEVELID,
    .game_state = G_GAMESTATE,
    .terrain = {
        .radius_mask  = 0xFFFu,                     /* world draw: andi $t2, $a0, 0xFFF (environment.s:1624) */
        .flags_mask   = 0xF000u,                    /* world draw: andi $t1, $a0, 0xF000 (environment.s:1623) */
        .flat_flag    = 0x1000u,                    /* bit 12 of the header flags selects the flat HP
                                                     * list; the binning tightens on andi 0x5000
                                                     * (environment.s:1711) */
        .vz_shl       = 1,                          /* Z4 = (v << 3) & 0x1FFC in quarter units */
        .vz_flat_shr  = 2,                          /* flat list: Z4 = v & 0x3FF */
        .uv_ori_table = 0x80062040u,                /* D_80062040: the 8-entry per-corner packed UV
                                                     * delta table the near/HQ 2x2 emit adds
                                                     * to the sub-tile's base UVs
                                                     * (environment.s 80026554-80026594 (near/HQ emit) and
                                                     * 80027814-80027854 (detail sub-path)) */
        .uv_ori_mask  = 0x38u,                      /* environment.s: `srl 25` then `andi 0x38`;
                                                     * ABE occupies tpage bit 15 here, and the
                                                     * unmasked index would reach past the
                                                     * 64-byte table */
        .defer_q      = { 0u, 0u },                 /* the near lists live in the scratchpad; not read */
        .face_layout  = TERRAIN_FACES_SPYRO3,
        .tex_stride   = 48u,                        /* the tile index times (1 << 5) + (1 << 4)
                                                     * (environment.s:2674-2677) */
        .hq_stride    = 48u,                        /* the HQ index (face word3 & 0x7F) times 48 into
                                                     * the same texture table (environment.s:5192-5196) */
        .hq_sub0      = 0x10u,                      /* the entry pointer is bumped 8 in the loop's
                                                     * delay slot and the sub-quad tile read at
                                                     * +0x8/+0xC of it, so the four quadrant halves
                                                     * are +0x10, +0x18, +0x20, +0x28; quadrant order
                                                     * TL, TR, BL, BR over the 3x3 subdivide grid */
        .hq_tri4_nodes = 0x800620A0u,               /* the tier-4 triangle pass's sub-triangle table
                                                     * (environment.s 80028248) */
        .hq_tri4_cells = 0x800622A0u,               /* its selector byte table, indexed by
                                                     * (row & 0x3C) + rot; the byte's bits 3-4 name
                                                     * the quadrant tile, bits 1-2 its 16x16 quarter */
        .hq_tri4_tile0 = 0x10u,                     /* the quadrant tiles, as hq_sub0 */
        .hq_tri4_tile_mask = 0x18u,                 /* `andi 0x18` on the selector byte */
        .hq_tri4_sub   = 0x800622E0u,               /* the quarter's sub-offset word, at
                                                     * (b & 6) << 1 plus (word1 >> 24) & 0x70; its
                                                     * high half folds in word1's -0x1F00 base */
        .hq_tri_uv     = 0x80062460u,               /* 16x16 spans */
        .hq_tri_uv_mask = 0x38u,                    /* `srl 25; andi 0x38` */
        .hq_tri_uv_shift = 6,                       /* ((row + rot) & 3) << 6: 64-byte blocks */
        .hq_tri2_nodes = 0x80062090u,               /* the tier-2 triangle pass's (environment.s 80026AD0) */
        .hq_tri2_tiles = 0x80062290u,               /* its quadrant byte table, indexed by
                                                     * (row & 0xC) + rot, a byte offset from +0x10 */
        .hq_tri2_uv    = 0x80062360u,               /* 32x32 spans, in the tier-4 table's form */
        .hq_tri2_w1    = -0x1F00,                   /* `addi -0x1F00` on the tile's word1 before
                                                     * the UV lookup, as the GT4 emit's uv1 */
        .hq_tri_c_col  = 2,                         /* word 1 byte 1 (environment.s 80026738, 80027E24: srl 6) */
        .abe_in_tpage = 1,
        .tile_word1_br = 1,                         /* GT4 emit: uv1 = word1 - 0x1F00, uv3 = word1
                                                     * (environment.s:2680,2669-2671) */
        .lod_dist_default = 0x7C00u,                /* no header field. The feeder derives its fog
                                                     * reference as this value >> 2 = 0x1F00, the
                                                     * constant the HP walk loads before every
                                                     * depth-cue blend (environment.s:2688: addi $t5,
                                                     * $zero, 0x1F00; 2693-2696: IR0 = 0x1F00 - SZ3,
                                                     * raw near colour at IR0 >= 0x1000, raw far
                                                     * colour at IR0 <= 0), and the ladder envelope
                                                     * as this value >> 4 = 0x7C0 = hp_far, the
                                                     * distance at which the same walk stops listing
                                                     * HP sectors. The draw bins with the constants
                                                     * below. */
        .hp_drop_sum_far    = 0x8800,               /* the HP walk drops a face whose corner SZ3s
                                                     * sum to 0x8800 or more (environment.s:2663,
                                                     * triangle walk 3011: addi -0xC00 on
                                                     * sum - 0x7C00, the bound at 2622, 2970) */
        .hp_drop_corner_min = 0x1F00,               /* and, for a sum in [0x7C00, 0x8800), only
                                                     * with every corner SZ3 at 0x1F00 or more
                                                     * (environment.s:2665-2672; 3013-3018) */
        .ladder_nearest_corner = 1,                 /* the HP walk blends each corner with IR0 =
                                                     * 0x1F00 - SZ3 (environment.s:2693-2698), 0 at
                                                     * IR0 <= 0 (2811) and 0xFFF at IR0 >= 0x1000
                                                     * (2816, 2831, 2846), and keeps the largest
                                                     * (2745-2747, 2772-2774, 2791-2793); the CLUT
                                                     * row is 15 - (IR0 >> 8), added to the tile word
                                                     * as row << 22 (2796-2798, 2875-2876), and row 0
                                                     * selects the +8 tile half (2799-2801) */
        /* The per-sector list binning, environment.s:1686-1720. */
        .no_lp_flag    = 0x2000u,                   /* 1687: andi 0x2000 skips the LP block */
        .no_hp_flag    = 0x4000u,                   /* 1716: andi 0x4000 emits no HP entry */
        .force_lp_flag = 0u,                        /* the LP list is distance-gated only */
        .lp_near_shift_flag = 0x8000u,              /* 1690: andi 0x8000 */
        .lp_near_shift = 0x600,                     /* 1693: addi 0x600 */
        .hp_far        = 0x7C0,                     /* 1709: addi -0x7C0, bgez skips HP */
        .lp_near       = 0x760,                     /* 1692: addi -0x760, blez skips LP */
        .hp_far_tight  = 0x600,                     /* 1713/1714: addi -0x100 then -0x500 */
        .tight_flags   = 0x5000u,                   /* 1711: andi 0x5000 */
        .second_pass_flag = 0x1000u,                /* environment.s:1711-1723 lists it at +0x2200; 3236-3700 draws it */
        .hp_list_arena = TERRAIN_LIST_ARENA_POLYBUF_CARVE,  /* environment.s:1559-1563: *end - 0x3000 */
        .hp_list_off   = { 0x2000u, 0x2200u },          /* 1600-1602: t7 = +0x2000 (HP walk), s1 = +0x2200
                                                         * (second walk); +0x2A00 is the LP list */
        /* The LP near outcode. */
        .lp_near_band    = 0x760,                   /* 1696-1700: vz - r - 0x760 <= 0 sets
                                                     * entry bit 1 */
        .lp_near_sz      = 0x760,                   /* 6972-6974: t1 = ((w10 >> 15) & 0x600)
                                                     * - 0x760; 7036-7039, 7075-7080: SZ3 + t1
                                                     * < 0 tags the vertex 8; 7151-7155 quad,
                                                     * 7255-7258 tri: a face whose corners
                                                     * share a tag drops */
        .lp_near_sz_step = 0x200,
        .sector_cull   = TERRAIN_SECTOR_CULL_SPHERE,
        /* The HP defer classifier, environment.s:2621-2630: the four corner
         * SZ3s summed against 0x2000 and each corner against 0x140, with no
         * face-word bits in the test. */
        .near_band       = 0x2000,
        .near_corner     = 0x140,
        .near_flag_excl  = 0u,
        .near_flag_tier2 = 0u,
        .lp_base_slots   = 0x30,  /* the LP insert's bucket index is
                                   * (4-corner SZ sum >> 5) + (word1 & 0xF) * 8
                                   * + 0x30 (environment.s:7209-7211); the
                                   * insert at environment.s:4300-4308 is the
                                   * HP textured triangle's, with no base */
        .lp_farcull_keyed = 1,    /* that whole index is dropped at or beyond
                                   * (G_ENVIRONMENT+0x2C >> 7) - 1
                                   * (environment.s:6924-6931, 7213-7214) */
        .lp_bias_slots   = 8,
        .hp_bias_slots   = 4,
        .nearband_ot_keys = 1,    /* the faces the near/HQ classifier selects key
                                   * through that same ordinary insert
                                   * (environment.s:2674-2679) */
        .slot_ot_contest  = 1,    /* the scene is submitted through one ordering
                                   * table, and a later insert into a bucket
                                   * paints later, with the moby pass run before
                                   * the terrain pass */
        .hp_source_wad_entry = 35,  /* Skelos Badlands: the title scene's
                                     * sectors are copies of its sectors with
                                     * the HP mesh cut to ground strips */
        .standin_lp_key = 0,  /* stand-ins adjoin whole-mesh borrows */
    },
    /* The moby drop-shadow draw, func_800499D4, and the player's own ground
     * shadow, func_800495E8. Both are handwritten and both align one for one
     * onto the other game's routines at other addresses: 330 instructions for
     * the drop-shadow walk and 251 for the player fan, with every immediate
     * identical and only the global addresses differing. Every switch below is
     * therefore read at a Spyro 2 site with the same meaning. */
    .shadow = {
        .desc        = G_MOBYSHADOWS,     /* mobyfunc.s:919-923: +0x00/+0x04 the sprite UV
                                           * words, +0x08 the drawn-moby list terminator */
        .hook        = F_MOBY_SHADOWS,
        .list_source = SHADOW_LIST_GTE_DATA, /* mobyfunc.s:916 mfc2 from data reg 2, the base
                                              * the moby cull mobydraw.s:36 left there
                                              * (polybuf end - 0x3000) */
        .list_base   = 0u,
        .list_reg    = 2,
        .list_offset = 0x1400u,           /* mobyfunc.s:918 addi 0x1400 */
        .moby_pos    = 0x0Cu,             /* mobyfunc.s:944-945 lw 0x10 / lw 0xC */
        .moby_ground = 0x1Cu,             /* mobyfunc.s:943 lw 0x1C: the packed ground height,
                                           * the two 6-bit ground-normal angles and the
                                           * shadow-enabled bit */
        .size_shl    = 1,                 /* mobyfunc.s:1062 sll 1 before the corners */
        .uv1_bottom_right = 1,            /* mobyfunc.s:924 addi -0x1F00 on the second UV word */
        .matrix_fold_a = 0,               /* mobyfunc.s:989-1006 writes R12 from the first
                                           * MVMVA and folds no angle index into R11 */
        .cull_layout = SHADOW_CULL_SPYRO3, /* mobyfunc.s:1107-1129: four-corner extent tests */
        .ground_plane = D_PLAYERSHADOW + 0x0Cu,  /* the player shadow's world height,
                                           * mobyfunc.s:631 lw 0xC */
        .moby_ot_bias = 0x47u,             /* mobyfunc.s 80049AE8 lb 0x47, 80049CA4 addi -3 */
        .moby_ot_bias_sub = 3,
        .frame_radius_off = 0x07u, /* mobydraw.s 80043AB8-80043AC8 lbu 0x24 of header + frame*8 + 7 */
        .player = {
            .hook         = F_PLAYER_SHADOW,
            .state        = D_PLAYERSHADOW,
            .anim_table   = D_SPYRO_ANIMS,    /* mobyfunc.s:687-688 lw 0x14 then the table
                                               * pointer, as spyrodraw.s:44 */
            .blend_table  = D_SPYRO_BLEND,    /* mobyfunc.s:682-686 lbu g_Spyro+0x1C into the
                                               * table, result << 4 = IR0 */
            .ring_dirs    = D_SHADOW_RING,    /* mobyfunc.s:774-775, 790 */
            .avatar_anim  = 0x14u,            /* mobyfunc.s:687 lw 0x14 */
            .avatar_blend = 0x1Cu,            /* mobyfunc.s:682 lbu 0x1C */
        },
    },
    /* The particle renderer, func_80048128. It aligns onto the other game's
     * func_8003AA38 except at two sites, four instructions in all, and both
     * sites are ordering-key clamps inside the routine, which this engine runs
     * unchanged. At partsdraw.s:582 a class-2 key that has gone negative skips
     * the record, where the other game clamps it to bucket 0 and still draws;
     * at partsdraw.s:751-753 the class-0 key subtracts the size byte before
     * the skip test, where the other game tests the bare depth and clamps the
     * key after. Three classes set the drawn stamp before their last skip
     * test, so a record can carry the stamp and append nothing; stamp_skip
     * holds each such test. */
    .particles = {
        .hook       = F_PARTICLES_DRAW,
        .array_ptr  = D_PARTS_ARRAY,      /* partsdraw.s:57-59 loads the pool base through
                                           * this word into its walk register */
        .rec_stride = 0x20u,              /* partsdraw.s:104-105 addi 0x20 after reading the
                                           * next record's selector at 0x20 */
        .sel_off    = 0x00u,              /* partsdraw.s:66 lh 0x0: bltz ends the walk, and
                                           * the arithmetic shift by 8 tested at :79-89
                                           * dispatches the class */
        .pos_off    = 0x04u,              /* partsdraw.s:91-92 and :709-710 read 0x4 (X low,
                                           * Y high) and 0x8 (Z low) and subtract the camera
                                           * position shifted right by 2 */
        .drawn_off  = 0x03u,              /* partsdraw.s:225 sb zero at record start,
                                           * :360/:417/:530/:693/:745 sb 1 once the record
                                           * passes its screen and depth window */
        .fixed_layer_class = 5u,          /* partsdraw.s:81-89 dispatches classes 0-4 and falls
                                           * through for 5 and up; the fallthrough links at the
                                           * fixed front entry, polybuf scratch - 8
                                           * (partsdraw.s:204-206), instead of a depth bucket */
        .type_table = D_PARTS_TYPES,      /* partsdraw.s:70-71 loads it into $s1; :95-98,
                                           * :241-245, :389-393 and :492-496 index it by the
                                           * selector's low byte; :163-170, :271-278,
                                           * :424-429 and :583-587 add the frame byte times 8
                                           * and read the words at +4 and +8 */
        .stamp_skip = {
            /* class 0: :745 stamps, :744 has shifted SZ3 right by 5, :751-753
             * subtracts the size byte (+0xA, :710/:723-724) and 2 and skips a
             * key <= 0 */
            [0] = { .on = 1, .size_off = 0x0Au, .key_min = 3 },
            /* class 2: :530 stamps, :522 has shifted SZ3 right by 5, :578-582
             * subtracts the byte at +0x11 (lhu +0x10, srl 8) and skips a
             * negative key */
            [2] = { .on = 1, .size_off = 0x11u, .key_min = 0 },
            /* class 3: :417 stamps, :416 has shifted SZ3 right by 5, :410/:419-423
             * subtracts the byte at +0x11 and skips a negative key */
            [3] = { .on = 1, .size_off = 0x11u, .key_min = 0 },
            /* classes 1 (:690 tests, :693 stamps) and 4 (:357 tests, :360
             * stamps) append after the stamp unconditionally */
        },
    },
    /* The moby (3D actor) draw. Three handwritten routines: the cull
     * func_80043858 carves the tick's draw scratch out of the primitive buffer
     * at D_POLYBUF_END - 0x3000, builds the animated record queue at +0x2000 of
     * it and the simple models' stride-0x10 queue at +0x1C00, and writes
     * each moby's drawn verdict (mobydraw.s:26-46); the animated draw
     * func_80044504 and the static draw func_80046FD8 walk those two queues.
     * Every offset below is a literal read off the Spyro 2 disassembly, not
     * carried from the other game that runs the same three routines. */
    .moby = {
        .array_head   = G_MOBYARRAY,
        .array_direct = 0,                 /* the head is a pointer: lw through it (mobydraw.s:32-33) */
        /* The cutscene sampler (func_titlescreen_and_loading_8007490C) walks
         * the first D_80068C64 records and, where two consecutive track
         * points lie 0x800 or more apart (func_8001BA20 length), holds the
         * first and then places the actor on the second, where shorter
         * segments interpolate in sixteenths (0x80074CC0-0x80074D2C). */
        .cutpath_count    = 0x80068C64u,
        .cutpath_snap     = 0x800u,
        .cutpath_overlays = "titlescreen_and_loading",
        /* The point buffers by chunk parity (lw D_80068C70 at 0x80074C08,
         * actor list word +0xC at 0x80074C14) and the per-actor frame
         * lists (D_80068C94, lw +0x3C at 0x80074A5C, count lbu at
         * 0x80074A64). */
        .cutpath_points   = 0x80068C70u,
        .cutpath_frames   = 0x80068C94u,
        /* No distance bound (jump_bound 0): Sparx (class 0x78) moves up to
         * about 31000 units a tick while other mobys jump as little as about
         * 1000, so a moby the sampler does not drive is never held. */
        .array_stride = 0x58u,             /* mobydraw.s:65 addi $ra, $ra, 0x58 */
        .state_off    = 0x48u,             /* mobydraw.s:66 lb; >= 0 processes */
        .state_term   = 0xFFu,             /* mobydraw.s:69-70 state == -1 ends the list */
        .alloc        = 0x80041290u,       /* asm/updatemobys.s func_80041290: v0 = the free
                                            * cursor's slot, 0 when full */
        .alloc_respawn = 1,
        .pos_off      = 0x0Cu,             /* mobydraw.s:81-83 the box cull's three words */
        .angle_off    = 0x44u,             /* mobydraw.s:220 the three byte angles */
        .class_off    = 0x36u,             /* mobydraw.s:111 lhu, model table index */
        .anim_state_off = 0x3Cu,           /* mobydraw.s:112 {id, nextId, frame, nextFrame} */
        .colour_off   = 0x54u,             /* mobydraw.s:219, copied into the record at +0x3C */
        .size_off     = 0x4Fu,             /* mobydraw.s:455 high byte of the +0x4E halfword */
        .drawn_off    = 0x4Du,             /* mobydraw.s:75 cleared, written 1 or 2 later */
        .sector_off   = 0x52u,             /* the cull drops a moby whose byte here, below
                                            * 0xFE, names a sector the PVS hid */
        .rot_off      = 0x20u,             /* mobydraw.s:2634-2638 the five
                                            * rotation words the water-plane split loads */

        .model_table     = G_MOBYMODELS,
        .model_anims_off = 0x3Cu,          /* mobydraw.s:113-120 model + 0x3C + id*4 */
        .model_data_off  = 0x38u,

        .anim_ptr_block   = 0x0Cu,         /* the six geometry pointers, four bytes apart */
        .anim_scale_off   = 0x02u,         /* the cull shifts the quarter-world centre left 2
                                            * then right by this byte before storing it as the
                                            * translation, so the model's unit is world >> byte */
        .anim_shift_off   = 0x02u,
        .anim_ccount_off  = 0x01u,
        .anim_vcount_hi   = 0x04u,         /* HP vertex count */
        .anim_vcount_lo   = 0x05u,         /* LP count, selected by the record flag sign */
        .anim_radius      = 0x07u,         /* mobydraw.s:126 lbu, << 4 for the sphere cull */
        .anim_frames      = 0x24u,         /* 8-byte keyframe records */
        .anim_frame_stride = 8u,

        .rec_base         = 0u,            /* derived from the polybuf end, see below */
        .rec_stride       = 0x44u,         /* mobydraw.s:578,887,891 */
        .rec_header_off   = 0x04u,         /* AnimationHeader* (keyframe A) */
        .rec_frame_a_off  = 0x0Cu,         /* mobydraw.s:893 keyframe A's own stream word */
        .rec_frame_b_off  = 0x10u,         /* keyframe B's stream word */
        .rec_tr_off       = 0x1Cu,         /* mobydraw.s:574-576 view-space translation */
        .rec_matrix_off   = 0x28u,         /* mobydraw.s:356-360 the composed 3x3 */
        .rec_tier_off     = 0x00u,         /* word 0 bits 16-31, sign picks HP/LP */
        .rec_material_off = 0x3Cu,         /* the Moby+0x54 colour word */
        .special_tiledef  = 0x80066ED8u,   /* mobydraw.s:2085-2086: the special material's
                                            * mode-1 setup loads this word, biased by the
                                            * record's CLUT row, into GTE control 16, and the
                                            * per-face emit writes its halves over the
                                            * primitive's texture page and CLUT. The address is
                                            * the middle of an unnamed data block, so it is
                                            * written as the literal the lui/addiu pair forms. */
        .env_sqrt_table   = 0x80066478u,   /* mobydraw.s:2417-2418 */
        .env_atan_table   = 0x80062630u,   /* mobydraw.s:2477-2478 */
        .env_radius_table = 0x800625ACu,   /* mobydraw.s:2496-2497 */
        .env_bias_word    = 0x80067ECCu,   /* mobydraw.s:2106-2108 (G_CAMERA + 0x48) */
        .rec_moby_off     = 0x40u,         /* mobydraw.s:361 */
        .blend_shift      = 4,             /* (w0 >> 4) & 0xFF0 */
        .blend_mask       = 0xFF0u,

        .rec_base_polybuf_off = 0x1000u,   /* scratch = polybuf end - 0x3000, the animated queue
                                            * at +0x2000 of it (mobydraw.s:36-40) */
        .rec_tr_final     = 1,             /* the cull GPF-rescales the translation by the size
                                            * byte and drops the records that pushes past the far
                                            * limit; the draw loads it as stored */
        .rec_frame_is_word = 1,            /* the record holds each keyframe's own stream word */
        .rec_header_b_off = 0x08u,         /* keyframe B's own AnimationHeader* */

        /* The world ordering table: the pointer at D_80066FFC, 0x580 entries
         * of {tail, head}, linked highest entry first by func_8001B2A8
         * (stdutil.s:14-16; draw/func_800156FC.s:220-221 passes the count). */
        .cal_world_ot   = 0x80066FFCu,
        .cal_ot_buckets = 0x580u,

        /* A cutscene's mobys take its actor slot as their class. While a
         * cutscene plays (D_800681C8 = 6, stored by StartCutscene 0x80073E48)
         * the word 0x80068C3C holds the WAD entry it plays; entry 0x29 is the
         * intro_arrival cutscene, whose actor 9 is the portal cone. */
        .props            = { { 9u, 0x29 } },
        .prop_n           = 1,
        .prop_scene_addr  = 0x80068C3Cu,
        .prop_state_addr  = 0x800681C8u,
        .prop_state_scene = 6u,

        /* The loader word (game_layout.game_state) holds 0 while the title
         * screen runs, whose model table names Spyro class 0. */
        .true_depth   = { { 0x0u, 1u << 0 } },
        .true_depth_n = 1,

        /* Class 0x19F: Idol Springs' fish. A flamed fish thrown at the idol
         * takes m_DepthOffset -6 and state 5 (level_12 moby_update
         * 0x80081230, 0x80081294), then state 6 once swallowed (0x800816B8).
         * The idol's lips are terrain at the swallowed fish's depth; the
         * offset moves the fish's key six buckets back, so the lips key into
         * its buckets or nearer and the table paints the closing jaw over
         * the parts of the fish that stand in front of them. */
        .terrain_order         = { 0x19Fu, 0u },
        .terrain_order_mstates = (1u << 5) | (1u << 6),

        /* Class 170 (0xAA): the air bubble around Sparx under water (and the
         * Aquaria Towers ring bubbles), a 9-vertex additive disc the game
         * turns to face the camera and draws after Sparx (world bucket 17
         * against his 20-22). Every state: game.state reads outside the
         * 0..31 range a state mask names while the attract demo plays a
         * level. */
        .sphere_shell   = { { 0xAAu, 0u } },
        .sphere_shell_n = 1,

        .hook_cull          = F_MOBY_CULL,
        .hook_draw_regular  = F_MOBY_DRAW_ANIM,
        .hook_draw_specular = 0u,          /* no specular material: the draw issues no GTE
                                            * lighting op at all */
        .hook_draw_shaded   = 0u,
        .hook_draw_simple   = F_MOBY_DRAW_STATIC,
    },
    /* The avatar's own draw, func_80041DBC. Its translation unit holds that one
     * handwritten routine and nothing else, and it is instruction-identical to
     * the other game's: the whole 1703-instruction listing aligns one for one,
     * with the only disassembly difference a `lhu $a0, 0($a0)` the two listings
     * render differently from the same word 0x00008494 (spyrodraw.s:1198). Every
     * offset below is therefore read at a Spyro 2 site with the same meaning. */
    .player = {
        .base    = G_SPYRO,
        /* The struct's own length, read off the routine that clears it:
         * memset(g_Spyro, 0, 0x2C4) (spyroupdate/func_80030A6C.s:48-49 forms the
         * address into $s3, :62 passes the length). It is also the span the
         * interp ring captures. */
        .rec_len = 0x2C4u,
        .pos_off = 0x00u,                       /* spyrodraw.s:209-211, three words */
        .rot_off = { 0x0Cu, 0x10u },            /* body and head packed byte eulers
                                                 * (spyrodraw.s:271,329) */
        .anim_off = { 0x14u, 0x18u },           /* body and head animation words
                                                 * (spyrodraw.s:44,112) */
        .sort_depth_off    = 0x1Eu,             /* spyrodraw.s:252 lb 0x1E: the ordering bias */
        .colour_filter_off = 0x20u,             /* the tint word: byte 3 non-zero cues the colour
                                                 * block toward bytes 0-2 (spyrodraw.s:742-745) */
        .scale_body_flag   = D_CHEAT_FLAT,      /* the flat cheat: ScaleMatrix on the body
                                                 * record (spyrodraw.s:275-283) */
        .scale_head_flag   = D_CHEAT_BIGHEAD,   /* the big head cheat: ScaleMatrix on the
                                                 * head record (spyrodraw.s:333-341) */
        .scale_vec         = D_CHEAT_SCALE,
        .emit_mode_off     = 0u,                /* one emit path */
        .hook_draw = F_PLAYER_DRAW,

        /* The routine carves its scratch out of the polybuf every tick, as the
         * moby cull does: base = *polybuf.end - 0x3000, the two 0x3C-byte part
         * records at +0x1400 and a zero flags word after them
         * (spyrodraw.s:38,40,109,177,203). */
        .part_base   = 0u,
        .part_base_polybuf_off = 0x3000u,
        .part_off    = 0x1400u,
        .part_stride = 0x3Cu,
        .part_count  = 2,
        .part_block_len = 0xC0u,                /* the two records and the terminator */

        .part_flags_off    = 0x00u,             /* spyrodraw.s:108,176 */
        .part_matrix_off   = 0x10u,             /* spyrodraw.s:272, func_8001B6C8's destination */
        .part_tr_off       = 0x04u,             /* spyrodraw.s:267-269 */
        .part_stream_a_off = 0x24u,             /* static / adjust / block cursors
                                                 * (spyrodraw.s:76-78,144-147) */
        .part_stream_b_off = 0x30u,             /* spyrodraw.s:104-106,172-174 */
        .part_seam_off     = 0x7Cu,             /* host block only: the body's two
                                                 * seam words, after the zero flags
                                                 * word at 0x78 (spyrodraw.s:68,94) */
        .part_stream_flag_bit = { 12, 13 },     /* spyrodraw.s:403,412 */
        .part_matrix_sign  = 0x80000000u,       /* spyrodraw.s:377 bgez skips the reload */
        .blend_mask        = 0x0FFFu,           /* the blend byte << 4, 1.12 (spyrodraw.s:402) */

        .sxy_base = 0x1F800000u,                /* the draw writes every projected vertex's
                                                 * int16 screen position here as it goes
                                                 * (spyrodraw.s:363,390-393,445-450); the walk
                                                 * does not read it, the probe compares
                                                 * against it */
        .sxy_len  = 0x400u,

        .flame_base  = D_FLAME_STATE,
        .flame_len   = 0xC4u,                   /* through the attach matrix's fifth word */
        .shadow_base = D_PLAYERSHADOW,          /* the ground shadow's own state block, read
                                                 * only by the player-shadow draw; inert until
                                                 * that feeder registers on this game */
        .shadow_len  = 0x18u,                   /* mobyfunc.s:625-717 reads +0x00 through
                                                 * +0x17 (flag, position, angles, bias) */

        .pair_bound = 400.0,
    },
    .flame = {
        .hook       = F_FLAME_DRAW,
        .attach_off = 0xB0u,             /* mobyfunc.s:216-220 loads five words */
        .super_off  = 0u,                /* no wide second form */
        .geom           = D_FLAME_GEOM,
        .geom_stride_lo = 0xB0u,         /* mobyfunc.s:243 */
        .geom_stride_hi = 0xE0u,         /* mobyfunc.s:248 */
        .geom_hi_base   = 0x2C0u,        /* mobyfunc.s:250 */
        .conn           = D_FLAME_CONN,
        .conn_hi_off    = 0x78u,         /* mobyfunc.s:251 */
        .cap_colours    = D_FLAME_CAP_COLOURS,
        .cap_amp        = 0x28u, .cap_amp_super   = 0x28u,  /* mobyfunc.s:293 */
        .strip_amp      = 0x2Fu, .strip_amp_super = 0x2Fu,  /* mobyfunc.s:521 */
        .uv1_bias       = -0x7F00,       /* mobyfunc.s:487, before the per-step ramp */
        .cap_colour_off = NULL,          /* always the first colour set */
    },
    .glow = {
        .hook       = F_GLOW_DRAW,
        .ra_guard   = 0u,                /* one call site in the image */
        .pool       = D_GLOW_POOL,
        .slots      = 16u,               /* mobyfunc.s:1947, base +0x140 */
        .stride     = 0x14u,
        .pos_off    = 0x00u,             /* mobyfunc.s:1985 */
        .packed_off = 0x10u,             /* mobyfunc.s:1973 */
        .box        = 0x7800,            /* mobyfunc.s:1997-2008 */
        .near_z     = 0x80,              /* mobyfunc.s:2019 */
        .far_z      = 0x7800,            /* mobyfunc.s:2021 */
        .pyr_x      = 0x400,             /* mobyfunc.s:2035 */
        .pyr_y      = 0x300,             /* mobyfunc.s:2045 */
        .front_bias = -1,                /* no front-entry branch: mobyfunc.s:2050-2053
                                          * clamps the bucket at zero instead */
        .shape_table  = 0x8006430Cu,     /* mobyfunc.s:2010-2014 */
        .cam_yaw_off  = 0x48u,           /* mobyfunc.s:1963, >> 4 at :1967 */
    },
    .glint = {
        .hook       = F_GLINT_DRAW,
        .pool       = D_GLINT_POOL,
        .slots      = 16u,               /* mobyfunc.s:2207, base +0x100 */
        .stride     = 0x10u,
        .pos_off    = 0x00u,             /* mobyfunc.s:2246-2248 */
        .packed_off = 0x0Cu,             /* mobyfunc.s:2230 */
        .box        = 0x7800,            /* mobyfunc.s:2262-2273, half-world */
        .near_z     = 0x80,              /* mobyfunc.s:2290 */
        .far_z      = 0x7800,            /* mobyfunc.s:2292 */
        .pyr_x      = 0x100,             /* mobyfunc.s:2306 */
        .pyr_y      = 0xC0,              /* mobyfunc.s:2316 */
        .near_vz    = 0x2000,            /* mobyfunc.s:2344, the branch depth */
        .outline    = D_GLINT_OUTLINE,      /* mobyfunc.s:2356, 2454 */
        .quads      = D_GLINT_QUADS,        /* mobyfunc.s:2372 */
        .colours    = D_GLINT_COLOURS,      /* mobyfunc.s:2325 */
        /* D_80064520, indexed by the gem's kind byte +0x49 times two plus one
         * random bit (overlays/level_50/moby_update/func_level_50_800705C8.s:366-371,
         * :433-434): five kinds, ten points. */
        .sparkle    = 0x80064520u,
        .sparkle_n  = 10u,
    },
    .zap = {
        .hook       = F_GLINT2_DRAW,
        .pool       = D_GLINT2_POOL,
        .slots      = 8u,                /* mobyfunc.s:2564, base +0x180 */
        .stride     = 0x30u,
        .head_off   = 0x00u,             /* mobyfunc.s:2604 */
        .packed_off = 0x1Cu,             /* mobyfunc.s:2589 */
        .box        = 0x5400,            /* mobyfunc.s:2608-2620 */
        .far_sz     = 0x5000,            /* mobyfunc.s:2819 */
        .spad       = 0x1F800100u,       /* mobyfunc.s:2741 */
        .rerand_hold = -1,               /* mobyfunc.s:2667-2678: no state test */
    },
    .tracers = {
        .ribbon_hook = F_TRACER_RIBBON,
        .mesh_hook   = F_TRACER_MESH,
        .ring        = D_TRACER_RING,
        .slots       = 0x20u,            /* mobyfunc.s:2939, base +0x600 */
        .stride      = 0x30u,            /* mobyfunc.s:2940 */
        .points_off  = 0x04u,            /* mobyfunc.s:2947 */
        .packed_off  = 0x28u,            /* mobyfunc.s:2945 */
        .ring_n      = 6,                /* +0x04 through +0x28, 6 bytes per point */
        .box         = 0x6000,           /* mobyfunc.s:2979-2989 */
        .far_sz      = 0x4000,           /* mobyfunc.s:3061 */
        .spad        = 0x1F800000u,      /* mobyfunc.s:3255 */
    },
    .hooks = {
        .world_draw = F_WORLD_DRAW,
        .sky_draw   = F_SKY_DRAW,
        .visibility = F_VISIBILITY,   /* called by both draws before their walks */
    },

    /* The credits font: WAD entry 187, sub-file 2, the model the credits
     * overlay reads as class 678 (credits_code.ovl, the spawn at 0x8006EF58
     * stores the class and the glyph into the moby's animation-id bytes). It
     * is Spyro 3's credits font, block for block: letters 0-25, digits 26-35,
     * '!' 36, ',' 37, '.' 38, '-' 39, ':' 40 (the overlay's character map at
     * 0x8006E35C-0x8006E3EC). '.' and ',' are lowered onto the letters'
     * baseline, ':' and '-' stay centred.
     *
     * Every face is flat-lit against entry 2 of the flat colour table at
     * 0x80064440 (the overlay stores Moby+0x54 = 2), whose words equal Spyro
     * 3's (base 0x00004040, lit 0x0000A0A0), and the draw loads Spyro 3's
     * light matrix (mobydraw.s:3856-3866), so the lighting constants are
     * Spyro 3's. */
    .font = {
        .form            = GAME_FONT_WAD_MODEL,
        .wad_entry       = 187,
        .wad_subfile     = 2,
        .digit_base      = 26,
        .letter_base     = 0,
        .punct           = { {'!', 36, 0}, {',', 37, 32}, {'.', 38, 36},
                             {'-', 39, 0}, {':', 40, 0} },
        .model_h         = 100.0f,
        GAME_FONT_CREDITS_LIGHT,
    },
    /* The credits letter roll: game state 10 draws through func_80015540
     * (func_800156FC's table jtbl_80010748), which calls
     * func_credits_8006EE18. While D_80066EE8 is not -1 it walks the
     * D_80066F74 (lh) CreditsLetter records at *D_80066FB8 (stride 0x1C,
     * state byte +0x04) and writes one class-0x2A6 moby per record in state
     * 1, 2 or 3 into consecutive slots up from *D_80066F14, then the 0xFF
     * sentinel (func_credits_8006EE18.s:50-90); state 1 is flying in, 2
     * settled, 3 departing, 4 once its y reaches 0x119, 0 free. The update
     * (func_credits_8006D96C) clears 100 records and gives a new letter the
     * first state-0 record or appends one. */
    .credits = {
        .gamestate = 0x800681C8u, .gamestate_value = 10u,
        .stage = 0u, .stage_below = 0,
        .draw_gate = 0x80066EE8u,
        .recs = 0x80066FB8u, .count = 0x80066F74u, .count_width = 2u,
        .rec_stride = 0x1Cu, .state_off = 0x04u, .rec_max = 100u,
        .drawn_states = (1u << 1) | (1u << 2) | (1u << 3), .flyin_state = 1u,
        .slot_base = 0x80066F14u, .slot_dir = 1, .slot_stride = 0x58u,
        .letter_class = 0x2A6u,
    },
    /* The loading screen's treasure tally: in builder state 2 (D_800698F0)
     * func_titlescreen_and_loading_80070EEC rewrites the moby array from
     * *D_80066F14 every tick. While (D_800698EC - 0x21) < 0x48 unsigned it
     * writes one class-1 moby per record of the 24 at *D_80067110 (stride 8)
     * whose byte +4 is nonzero (0x80070FB4-0x8007120C), then the counter gem
     * (+0x0C 0x154, +0x14 0xC00; 0x800713C0) and the total gem (+0x0C 0xB4;
     * 0x8007162C), each under its own timer gate. */
    .tally = {
        .state = 0x800698F0u, .state_value = 2u,
        .timer = 0x800698ECu, .timer_lo = 0x21u, .timer_span = 0x48u,
        .recs = 0x80067110u, .rec_stride = 8u, .active_off = 4u, .rec_count = 24u,
        .slot_base = 0x80066F14u, .slot_stride = 0x58u, .prefix_max = 0u,
        .gem_class = 1u, .hud_x = { 0x154, 0xB4 }, .hud_z = 0xC00,
    },
    .owner = {
        .sparx_class   = 0x78u,
        /* The player update: func_8001B050 (update flags) calls it under
         * flag 0x8 (asm/nonmatchings/update/func_8001B050.s). The moby
         * updates run through the overlay's own entry at D_800670F0 under
         * flag 0x2 rather than a per-class table, so Sparx's spawns are not
         * attributed (moby_update 0). */
        .player_update = 0x8002A07Cu,
        /* The slot allocator of the pool at D_PARTS_ARRAY, the same
         * handwritten routine as Spyro 3's func_80056434
         * (asm/updatemobys.s func_80041C50). */
        .part_alloc    = 0x80041C50u,
        /* Its free routine: class byte -2 at a0+1, or the end sentinel -1
         * (asm/updatemobys.s func_80041D20). */
        .part_free     = 0x80041D20u,
    },
    /* The update-list builder func_80040E94 (asm/updatemobys.s): pod flags
     * D_800682B8 (0x20 bytes, cleared as words), the moby array through
     * D_80066F14, pod member lists through D_800670E0, the list at
     * D_800683F0 + 0x200; pod byte 0x43, update distance 0x4A, listed 0x4B.
     * func_8001B050 calls the overlay's moby update through D_800670F0
     * under flag 0x2 (asm/nonmatchings/update/func_8001B050.s); level_10's
     * gem update skips the spin at 0x80070A08 when +0x4D is clear and the
     * distance is 0x1801 or more. */
    .update_list = {
        .builder = 0x80040E94u, .pod_flags = 0x800682B8u, .pod_count = 0x20u,
        .pod_lists = 0x800670E0u, .list = 0x800685F0u,
        .pod_off = 0x43u, .dist_off = 0x4Au, .listed_off = 0x4Bu,
        .overlay_update = 0x800670F0u,
    },
    /* func_800225F8 (asm/environment.s) walks the seven pairs at
     * D_800681D4; func_8002365C sets a descriptor's flags by id and
     * func_80023710 its keyframe over the same pairs. */
    .kf_anim = { .walker = 0x800225F8u, .registry = D_ENV_ANIM_REGISTRY, .pairs = 7u },
};

/* The 2D UI layer (games/common/renderer/ui_sprite.c). The sprite, box, fade
 * and letterbox primitives carry the same psyq word order as the other game's
 * and reach the same kind of chokepoint; the text chain does not and is fed by
 * games/common/renderer/text_sprt.c instead. */
const ui_game_t spyro2_ui_game = {
    .ot_insert    = F_UI_OT_INSERT,
    /* The title and credits screens order their 2D by depth slot and link
     * every primitive through the slotted insert instead, so it is a second
     * chokepoint carrying the logo, "press start" and the credits text. */
    .ot_insert_slot = F_UI_OT_INSERT_SLOT,
    .fade_emitter = F_UI_FADE,
    /* func_8004DF84: two black F4 borders, then the 4x4 cell grid, per step
     * of the pause window's shrink and grow (drawutil/func_8004DF84.s:37-42,
     * draw/func_800145AC.s:95,336). */
    .fb_grid_emitter = F_UI_FB_GRID,
    /* func_8004DF84(step) reads the right edge at D_800633BC + 2 * step and
     * the top row at D_800633E0 + step (W 512 down to 256 by 16, top 8 up to
     * 120), links its cells down to row 0xE4 and samples rows 12 + 54 * row
     * of the buffer, 216 in all (drawutil/func_8004DF84.s:28-36, :43-44,
     * :57, :77); the shrink calls steps 1..16 (draw/func_800145AC.s:93-105)
     * and the unpause grow 14 down to 2 by 2 (:334-346). */
    .fb_grid_shape = { .w_table = 0x800633BCu, .top_table = 0x800633E0u,
                       .steps = 16, .bottom = 0xE4, .src_h = 4 * 0x36 },

    /* The cutscene letterbox: func_8004D604 steps D_8006701C and draws the
     * bars over rows 12..12+h and 228-h..228 (drawutil/func_8004D604.s:6-94). */
    .letterbox = { .emitter = F_UI_LETTERBOX, .height = 0x8006701Cu,
                   .top_y = 12, .bottom_y = 228 },

    /* No freeze-frame backdrop quad is located on this game: the frozen frame
     * comes from the retained framebuffer below instead. */
    .freeze_tint   = 0u,
    .freeze_clut_x = 0,
    .freeze_clut_y = 0,
    /* The state draw func_800145AC keeps the last rendered frame behind the
     * screens that do not clear by writing DRAWENV.isbg = 0 on both draw
     * buffers (draw/func_800145AC.s:85-88, :325-329) and drawing no world;
     * nothing writes the byte back to 1 until a screen that clears runs
     * (:137-142, :350-355). The isbg bytes are G_DB + 0x18 and
     * G_DB + 0x74 + 0x18, which drawutil/func_8004D810.s:38-41,97-100 clears
     * and restores around the blocking fade for the same reason. */
    .freeze_retained = 1,

    /* The UI texture block: tpage word 0x618 in the text chain's DR_MODE
     * (mobyfunc.s:1570) is page (512, 256) at 4bpp, the same block the HUD and
     * dialog sprites sample. */
    .atlas_x = 512, .atlas_y = 256, .atlas_w = 512, .atlas_h = 256,
    .atlas_name = "ui_atlas",

    /* Both glyph emitters build a tag-to-tag SPRT chain in the primitive
     * buffer and link it into the 2D ordering table themselves, so neither
     * reaches the chokepoint. */
    /* The loading-screen picture is a third chain of this shape: four
     * DR_MODE at tpage 0x108 + 2*i, each followed by a 0x14-byte SPRT of
     * 0x80 x 0xD8 at x = i << 7, y 12. It lives in the
     * titlescreen_and_loading overlay, whose vaddr every one of this game's
     * thirty level overlays also occupies, so its slot carries that overlay's
     * id and the hook fires only while those bytes are the resident ones. */
    .sprt_chain_emitter = { F_TEXT_SPRT, F_TEXT_SPRT_PER_GLYPH, F_LOADING_SPRT },
    .sprt_chain_overlay = { 0u, 0u, OVL_TITLESCREEN_AND_LOADING },

    /* The table is {tail, head} slots off D_80066FFC with the 2D entry
     * D_80067000 one slot below slot 0 (init/func_80011DD4.s:20-29: base
     * X - 0x2C00, entry X - 0x2C08). func_8001B390 and func_8001B354 append
     * at a slot's tail (stdutil.s:66-103) and func_8001B2A8 stitches the
     * slots highest first and the 2D entry last (stdutil.s:13-61), after
     * the canonical capture. Both inserts are hooked above. */
    .rank_by_link = 1,
    .ot_stitch    = F_UI_OT_STITCH,
    /* The four callers of func_8001B2A8, each stitching after its own
     * VSync: the state draw func_800145AC (draw.s, stitch at 0x800146AC and
     * 0x80014A54), func_800156FC (0x800159A4), the blocking fade
     * func_8004D810 (drawutil.s, 0x8004D938) and the guidebook's
     * func_guidebook_8006DB50 (0x8006DF58), resident only with that
     * overlay. */
    .stitch_routine = { { 0x800145ACu, 0x800151F0u, 0u },
                        { 0x800156FCu, 0x800159C8u, 0u },
                        { 0x8004D810u, 0x8004D984u, 0u },
                        { 0x8006DB50u, 0x8006DF80u, OVL_GUIDEBOOK } },
};
