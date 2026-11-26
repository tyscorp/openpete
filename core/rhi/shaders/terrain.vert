#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_unorm8.glsl"
#include "psx_viewz.glsl"
#include "psx_key_contest.glsl"

/* Position-chain precision. The camera-space chain and every verdict on it
 * run in fp32 with no Float64 capability: a corner's world position is
 * formed as an exact int32 (sector origin x4 + local x4), the integer part
 * of the camera is subtracted as an int32, the difference (under 2^24,
 * exact in float) is rounded to float once and the camera's fractional part
 * subtracted after it (cam_rel). A world vertex shared by several sectors
 * therefore reaches mac_of as the same float operands in every sector, so
 * its clip position is bit-identical across sectors and pipelines without
 * wide precision; `precise` on the position chain keeps the operation order
 * identical in every pipeline compiling the stage. */
/* x / k for a power-of-two constant k, as a multiply by its exact
 * reciprocal: fp32 OpFDiv is specified only to 2.5 ULP, the multiply is
 * exact. */
#define PDIV(x, k) ((x) * (1.0 / (k)))
/* TERRAIN_SECDELTA: the per-sector rounding control of the shared-vertex
 * readback and the hole census. The camera operand is rounded per sector
 * (sdel_of), so a vertex shared by two sectors can round apart.
 * Diagnostic only. */
/* TERRAIN_READBACK: the shared-vertex position readback, drawn as points
 * into an RGBA32_UINT target (rb_emit); the position chain is the shipped
 * stage's, unchanged. Diagnostic only. */
/* TERRAIN_DEPTH_SPEC: the depth convention from specialization constant 0
 * (DEPTH_REVERSED), fixed per pipeline, in place of pc.depth_rev. The
 * stages without it read pc.depth_rev, so one pre-rasterization library
 * serves the forward-class pre-pass and key pipelines and the
 * reversed-class scene pipelines at bring-up; the stages with it are the
 * whole-create stage (rhi_pipeline_desc.vert_spv_whole) of the pipelines
 * that replace those links in the background, which carry no select. */

/* Terrain sector-mesh vertex stage - the retained-terrain GPU path
 * (extraction rearchitecture, seam 2).
 *
 * Mirrors what the CPU walk (terrain.c walk_sector +
 * terrain_feed_native) computes per present for an UNCLAIMED sector: the GTE
 * camera transform (ngte_mac + proj_screen), the per-face verdicts (NCLIP
 * two-half back-face cull with the UNR-overflow regime, the .s LP far-cull
 * and authored admission, the HP LOD drop, face_is_near flat-vs-HQ variant
 * select), the CLUT-ladder / near-tile material select, the GTE-exact DPCS
 * fog blend, and the og OT-key (koff) for the key-resolve contest. Every
 * vertex carries its parent face's four corner positions, so face verdicts
 * are evaluated (redundantly but face-uniformly) per vertex; a culled face
 * collapses all of its vertices to a degenerate point.
 *
 * Precision against the walk: the CPU walk runs this chain in fp32 through
 * the same camera split (openpete_tercache_cam_split) and the same ops in
 * the same order (tn_vert_project, proj_screen, cv_mid, tn_clip_lerp), and
 * this stage is `precise` (no FMA contraction), so walk and cache produce
 * the same screen coordinates and W.
 * HQ sub-quad nodes are built by averaging the TRANSFORMED
 * corner values (cv_mid mirrors), never by transforming baked midpoints -
 * the CPU's float rounding differs between the two.
 *
 * Sector ownership is decided on the CPU (sector-params UBO): a claimed
 * sector renders through the unchanged CPU walk and is killed here, so no
 * face is ever drawn by both paths. Claimed sectors include everything that
 * may approach the near plane - an unclaimed vertex is GUARANTEED m3 well
 * in front of the camera, which is what makes the raw H/m3 projection (no
 * near clipping, no straddle handling) safe here. */

#if !defined(TERRAIN_CELLS) && (defined(TERRAIN_ANIM) || defined(TERRAIN_NEAR))
/* The anim and near chunk stages read a ring face's positions from the
 * sector's position ring (ring_fetch): the position and corner inputs land
 * in these names, which the rest of the stage reads. */
#define TERRAIN_RINGV
#endif
#ifndef TERRAIN_CELLS
#ifdef TERRAIN_RINGV
layout(location = 0) in ivec4 inPosA;     /* xyz: sector-local x4 fixed; w: flags16 */
#else
layout(location = 0) in ivec4 inPos;      /* xyz: sector-local x4 fixed; w: flags16 */
#endif
layout(location = 1) in uvec4 inUV;       /* far u,v, near u,v (texels)   */
layout(location = 2) in vec4  inColFar;   /* far colour  (UNORM 0..1)     */
layout(location = 3) in vec4  inColNear;  /* near colour (UNORM 0..1)     */
layout(location = 4) in uvec4 inMat;      /* far0, far1, near0, near1     */
layout(location = 5) in uint  inFaceFlags;
#ifdef TERRAIN_RINGV
/* A ring face's chunk: px its own bank index, c0x, c0y, c0z, c1x its
 * corners' (openpete_terrain_cache.h, position rings). */
layout(location = 6) in ivec4 inC0A;      /* c0.xyz, c1.x   (x4 fixed)    */
layout(location = 7) in ivec4 inC1A;      /* c1.yz, c2.xy                 */
layout(location = 8) in ivec4 inC2A;      /* c2.z, c3.xyz                 */
ivec4 inPos, inC0, inC1, inC2;
#else
layout(location = 6) in ivec4 inC0;       /* c0.xyz, c1.x   (x4 fixed)    */
layout(location = 7) in ivec4 inC1;       /* c1.yz, c2.xy                 */
layout(location = 8) in ivec4 inC2;       /* c2.z, c3.xyz                 */
#endif
#endif
#if defined(TERRAIN_ANIM) && !defined(TERRAIN_CELLS)
/* Poly-anim chunks (binding 1, rhi_terrain_prev): previous-tick twin of the
 * position + parent corners. Every coordinate is lerped prev->cur at the
 * build's alpha on camera-relative integer differences (cam_rel_lerp), the
 * mirror of tsc_vanim_lerp filling cvF (alpha = 1 on canonical builds
 * collapses to cur exactly). */
layout(location = 9)  in ivec4 inPrevPosA; /* prev xyz (x4 fixed), pad    */
layout(location = 10) in ivec4 inPrevC0A;
layout(location = 11) in ivec4 inPrevC1A;
layout(location = 12) in ivec4 inPrevC2A;
ivec4 inPrevPos, inPrevC0, inPrevC1, inPrevC2;
/* Previous-tick far and near colour words (R,G,B,A bytes): equal to the
 * current colours except on an env-anim colour sector. */
layout(location = 13) in uvec2 inPrevCol;
#endif
#if defined(TERRAIN_NEAR) && !defined(TERRAIN_CELLS)
/* Near-variant chunks (binding 1, rhi_terrain_nearctx): the OTHER two
 * emitted-tri members' attributes, in emit order relative to self
 * (a = self+1, b = self+2 mod 3). Positions need no context - every vertex
 * already carries the parent face corners the whole triangle is built from.
 * With these the vertex stage can resolve all three members' pre-clip colour
 * and UV and re-run the walk's near-plane clip fan. */
layout(location = 9)  in uvec4 inCtxAUV;  /* a: uf, vf, un, vn            */
layout(location = 10) in uvec2 inCtxACol; /* a: far, near (R8G8B8A8)      */
layout(location = 11) in uvec4 inCtxBUV;
layout(location = 12) in uvec2 inCtxBCol;
#endif
#ifdef TERRAIN_CELLS
/* 4x4 near-cell records (rhi_terrain_cell, one per INSTANCE): the face
 * corners, face flags, raw near colours and eight cell tiles. cells_fetch()
 * derives this vertex's attributes from the record and gl_VertexIndex into
 * the same names the chunk variants read from their vertex inputs, so main()
 * runs unchanged on them. Compiled with TERRAIN_NEAR (near-variant sectors,
 * 6 vertices per cell triangle) or TERRAIN_ANIM (poly-anim sectors, 3). */
/* Locations 0-1: the twelve int16 corners two to a word (c0x | c0y << 16,
 * ...), then face_flags and the record info; cells_fetch unpacks them into
 * inC0..inC2 and inCellFace. */
layout(location = 0)  in uvec4 inCellCA;    /* c0x..c2y                   */
layout(location = 1)  in uvec4 inCellCB;    /* c2z..c3z, face_flags, info */
layout(location = 2)  in uvec4 inCellFar;   /* raw far colours i0..i3     */
layout(location = 3)  in uvec2 inCellFog;   /* x: previous-corner record (near stage); y: zero */
layout(location = 4)  in uvec4 inCellNear;  /* raw near colours i0..i3    */
layout(location = 5)  in uvec4 inCellT0;    /* tile: uv TL,TR | uv BL,BR | mat0 | mat1 */
layout(location = 6)  in uvec4 inCellT1;
layout(location = 7)  in uvec4 inCellT2;
layout(location = 8)  in uvec4 inCellT3;
layout(location = 9)  in uvec4 inCellT4;
layout(location = 10) in uvec4 inCellT5;
layout(location = 11) in uvec4 inCellT6;
layout(location = 12) in uvec4 inCellT7;
#ifdef TERRAIN_ANIM
layout(location = 13) in uvec4 inCellPA;    /* previous-tick corners,     */
layout(location = 14) in uvec2 inCellPB;    /* packed as inCellCA/CB      */
ivec4 inPrevC0, inPrevC1, inPrevC2;
#endif
ivec4 inC0, inC1, inC2;
uvec2 inCellFace;
uvec2 cellHqFog;
uvec4 cellNear, cellFar;   /* the record's raw near and far colours; a
                            * colour-table face's from the sector's table */
ivec4 inPos;
uvec4 inUV;
vec4  inColFar;
vec4  inColNear;
uvec4 inMat;
uint  inFaceFlags;
#ifdef TERRAIN_ANIM
ivec4 inPrevPos;
#endif
#ifdef TERRAIN_NEAR
uvec4 inCtxAUV;
uvec2 inCtxACol;
uvec4 inCtxBUV;
uvec2 inCtxBCol;
#endif
#endif

layout(push_constant) uniform TerrainPC {
    vec4 cbase;   /* x_base, x_span, y_base, y_span (scene_to_clip terms)  */
    vec4 cdims;   /* disp_w, clip_h, band, freeze_wf                       */
    int  pass_sel;      /* 0 = WORLD, 1 = FILL (LP hole-fill), 2 = KEY    */
    int  full_extent;   /* full-target branch of the mapping              */
    /* Material pass this draw is: -1 = every face (no material pass, and
     * the depth/key passes, which shade nothing); 0 = the stock pass;
     * >0 = a material's pass. With mat_pass >= 0 a face is kept when its
     * resolution class (baked into the face) is set in cls_mask, so a
     * face's pass membership depends on the face and this present's
     * class resolution, not on the camera or on whether the sector is
     * resident. */
    int  mat_pass;
    /* --vertex-precision integer: 1 = quantise the emitted screen point to the
     * PSX integer pixel grid (the polygon wobble), 0 = sub-pixel. Latched per
     * present from the renderer's live snap state rather than baked as a spec
     * constant, so the runtime setter needs no pipeline rebuild, and carried
     * here rather than in the globals UBO because that block is re-uploaded
     * only when the terrain walk publishes new state - a snap toggled between
     * publishes would lag the CPU walk by one publish and the two feeds would
     * disagree for those presents. */
    int  snap;
    int  cover;          /* fragment-stage fields (terrain_shade.glsl)     */
    int  pad0, pad1, pad2;
    vec4 fc;
    /* The resolution classes this material pass keeps: bit c of word c/32. */
    uvec4 cls_mask[2];
    /* The bound pipeline's depth convention (op_scene_clip): nonzero =
     * reversed, z_ndc = 1/vz; 0 = forward, z_ndc = 1 - 1/vz. The value
     * specialization constant 0 gives the scene stages for the same
     * pipeline (rhi_bound_depth_reversed). */
    int  depth_rev;
} pc;

/* scene_to_clip mirror, term for term in float - a fitted affine loses ~5
 * subpixel LSBs at the screen edges and flips rasterized edge pixels. */
/* The same terms with every operation `precise`: a shared vertex reaches
 * this from pipelines compiled separately, and contraction decided per
 * compilation would round the clip position differently in each. */
vec2 clip_of(vec2 sxy) {
    precise float nx = sxy.x / pc.cdims.x;
    precise float ny = (sxy.y - pc.cdims.z) / pc.cdims.y;
    precise vec2 r;
    if (pc.full_extent != 0)
        r = vec2((-1.0 + nx * 2.0) / pc.cdims.w, 1.0 - ny * 2.0);
    else
        r = vec2(pc.cbase.x + nx * pc.cbase.y, pc.cbase.z - ny * pc.cbase.w);
    return r;
}
/* The window depth and clip w reach gl_Position through the same rule. */
precise gl_Position;

/* Mirrored by native_terrain_cache.c's tc_globals_ubo (C natural layout ==
 * std140 there: every member is a 16-byte vec4/ivec4 run) and, as a prefix
 * up to sdvc, by terrain_shade.glsl - a field added here must land in
 * both. */
layout(std140, set = 5, binding = 0) uniform TerrainGlobals {
    vec4  mrow0;    /* xyz: M[0][*] (raw int16 values)                    */
    vec4  mrow1;    /* xyz: M[1][*]                                       */
    vec4  mrow2;    /* xyz: M[2][*]                                       */
    /* The camera in x4 units (world/16 x 4, the baked corners' scale),
     * split as floor(cam x4) and cam x4 - floor(cam x4) in [0, 1). */
    ivec4 cam_int_x4;
    vec4  cam_frac_x4;
    vec4  ofH;      /* ofx, ofy (GTE screen offset, px), H (projection
                     * distance), poly-anim sub-tick alpha (1.0 on
                     * canonical builds; TERRAIN_ANIM only)               */
    vec4  aux;      /* x: near-plane view-Z (TERRAIN_NEAR clip), y: H_canon
                     * (the GTE's H, limit-regime mirrors), z: env-anim
                     * colour alpha (1.0 selects the current colours;
                     * TERRAIN_ANIM only), w: s_fogscale                  */
    ivec4 fog;      /* x: fogRefBase, y: gflags, z: cullRefS2, w: the LP
                     * insert's ordering-table terms (OT_* accessors)     */
    vec4  sdv[128]; /* per-tile scroll V offsets (lq, hqA, hqB, 0)        */
    /* Portal inner-rim cull frames (portal_rim_classify mirror). rim_cnt.x
     * = active plane count; 0 disables the test (no portal, or
     * rim-candidate sectors claim to the CPU walk instead). Plane pi's
     * outline lives at rim_pv[pi*16 .. pi*16+vn-1]. The frames are the
     * CPU's double frames rounded to float. */
    ivec4 rim_cnt;
    ivec4 hp_drop;  /* x: HP drop sum bound, y: corner bound (x = 0: the
                     * sum against 4 * fogRefBase alone)                  */
    vec4  rim_oxyH;      /* rim ofx, ofy, H, pad                          */
    vec4  rim_knobs;     /* eps, near, reach, cos(angle)                  */
    vec4  rim_abc[6];    /* screen inv-vz plane fit a,b,c; w = valid      */
    vec4  rim_O[6];      /* in-plane basis origin (view space); w = 1 when
                          * the plane discards its rim per fragment       */
    vec4  rim_U[6];      /* in-plane basis U                              */
    vec4  rim_W[6];      /* in-plane basis W                              */
    vec4  rim_nrm[6];    /* unit view normal; w = outline vn              */
    vec4  rim_bbox[6];   /* outline UV bbox pulo, puhi, pvlo, pvhi        */
    vec2  rim_pv[96];    /* outlines in (U,W), 16 slots per plane         */
    vec4  hd_lad;        /* HD fold: CONT-fog ramp ts, tb; count; enable  */
    vec4  hd_ent[1664];  /* 416 entries x 4 vec4 (rect/atlas/fogA/fogB)   */
    /* Portal cover planes, read by terrain.frag (portal_cover.glsl): active
     * opening planes in SCREEN space. cov_cnt.x = plane count (0 = test
     * off); cov_abc.w = the plane's outline point count (pn < 3 never
     * matches). One outline point per cov_poly slot, 16 slots per plane. */
    ivec4 cov_cnt;
    vec4  cov_margin;    /* x = plane-cover near margin M (world units)   */
    vec4  cov_abc[6];    /* screen inv-vz plane fit a, b, c; w = pn       */
    vec4  cov_bbox[6];   /* outline screen bbox x0, y0, x1, y1            */
    vec4  cov_poly[96];  /* outline points (px, py, 0, 0)                 */
    vec4  sdvc[128];     /* per-tile 4x4 near-cell row V offsets (rows 0-3) */
} g;

/* The block's scalars in the build's precision. */
#define G_OF   g.ofH.xy
#define G_H    g.ofH.z
#define G_VA   g.ofH.w
#define G_NEAR g.aux.x
#define G_HCAN g.aux.y
#define G_CA   g.aux.z
#define G_FOGS g.aux.w

/* g.fog.y bits - keep in sync with openpete_terrain_cache.h OPTC_GF_* */
#define GF_NEARHQ    1
#define GF_CLUTFOG   2
#define GF_CULLHP    4
#define GF_HANDOFF   8
#define GF_GAPFILL   16
#define GF_NOLOD     32
#define GF_CULLLP    64
#define GF_KEYON     128
#define GF_HQPIN     256
#define GF_LADNEAR   512
#define GF_NBKEYS    1024
#define GF_CELLS4    2048
#define GF_TRI4      4096
#define GF_TRI2      8192
#define GF_LPNEAROUT 16384
#define GF_HQFOG     32768
#define GF_HPRAW     65536
#define GF_HPFOG     131072
#define GF_FOGOFF    262144
#define GF_FOGPP     524288
#define GF_NOBIAS    1048576

struct TerrainSector {
    vec4 o;        /* xyz: sector origin (world/16), w: uintBitsToFloat(flags) */
    vec4 gp, gq;   /* pack LP grade affine v' = p*v8 + q*255 (rgb; SF_LPGRADE);
                    * gp.w: uintBitsToFloat(the colour table's first unit) */
};
layout(std140, set = 6, binding = 0) uniform TerrainSectors {
    TerrainSector sec[512];
} s;

/* Sector flag bits - keep in sync with openpete_terrain_cache.h OPTC_SF_* */
#define SF_DRAWN      1u
#define SF_DRAWHP     2u
#define SF_DRAWLP     4u
#define SF_LPFARONLY  8u
#define SF_LPGAPONLY  16u
#define SF_FILLLP     32u
#define SF_FILLNEAR   64u
#define SF_SECINLOD   128u
#define SF_LPGRADE    256u
#define SF_NEARVAR    512u
#define SF_LPUNCOV    1024u
#define SF_LPNEAR_SHIFT 16u          /* bits 16-27: LP near-outcode SZ3 bound */
#define SF_LPNEAR_MASK  0xFFFu

/* face_flags bits - keep in sync with rhi_terrain_vertex (rhi.h) */
#define FF_SLOT_MASK  0x1FFu
#define FF_TIER_LP    (1u << 9)
#define FF_FOGBIAS    (1u << 10)
#define FF_GAPFILL    (1u << 11)
#define FF_BORROWED   (1u << 11)   /* HP: borrowed from the HP source level
                                    * (bit 11 is FF_GAPFILL on LP faces) */
#define FF_DSIDE      (1u << 12)
#define FF_FLIP       (1u << 13)
#define FF_QUAD       (1u << 14)
#define FF_W3_40      (1u << 15)
#define FF_W3_80      (1u << 16)
#define FF_LPUNCOVX   (1u << 16)   /* LP: off the HP mesh and the borrowed HP
                                    * faces (bit 16 is FF_W3_80 on HP faces) */
#define FF_HQELIG     (1u << 17)
#define FF_LPRIDE     (1u << 17)   /* LP: uncovX and no stand-in for the scene's
                                    * HP surface, drawn by the uncov-only ride
                                    * (bit 17 is FF_HQELIG on HP faces) */
#define FF_FLOOR      (1u << 18)
#define FF_PINCELLS   (1u << 19)   /* HP: the pin sends this face to the cells */
#define FF_CELLOK     (1u << 20)   /* HP: the face may take the 4x4 cells */
#define FF_KEYCELLS   (1u << 21)   /* HP: the face's drain sub-cells ride its
                                    * key-only records (OPTC_ARENA_KN / KA) */
#define FF_LPKEY      (1u << 22)   /* HP: a borrowed face standing in for an
                                    * uncov LP face; keys with the LP base term */
#define FF_WATER      (1u << 23)   /* HP: a semi-transparent face of a sector
                                    * the game draws in its second walk
                                    * (terrain.c tn_face_water) */
/* admit (LP): bits 19-23; otbias: bits 24-28 */

/* PSX-affine interpolation - the scene stages' spec constant 1
 * (psx_scene.vert), captured at pipeline creation. The depth convention is
 * pc.depth_rev: the cache's pre-pass and key pipelines are FORWARD-class and
 * write the pre-pass image's 1 - 1/vz beside prepass.vert, its scene
 * pipelines the scene class's convention, through one compiled stage. */
#ifdef TERRAIN_DEPTH_SPEC
layout(constant_id = 0) const bool DEPTH_REVERSED = false;
#endif
layout(constant_id = 1) const bool AFFINE_UV = false;
/* Spec-constant 4, as terrain_shade.glsl: the scene is submitted through one
 * ordering table, so a terrain key is stored in its bucket's tier order
 * (op_key_tier_key); the key pass's window depth bounds that store. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;
/* Spec-constant 9, as terrain_shade.glsl: the debug tint word
 * (rhi_terrain_gpu_tint, OPENPETE_TERRAIN_GPU_TINT): bit 29 on, mode in bits
 * 26-28, bit 25 the mode-7 verdict-class word. 0 in every shipped
 * configuration, so the driver drops each tint branch from the stage. */
layout(constant_id = 9) const int TER_TINT = 0;

/* Outputs, component-packed into eleven locations: the interpolated
 * values share locations 0 and 5 with each other's free components, the
 * flat scalars share locations 3 and 4. vQ is also the divisor of vKfl and
 * vKd, which interpolate as the UV does (psx_key_contest.glsl).
 * terrain_shade.glsl and terrain_key_cache.frag declare the same layout. */
layout(location = 0, component = 0)      out vec2  vUV;    /* pre-multiplied by q (scene contract) */
layout(location = 0, component = 2)      out vec2  vKfl;   /* drain face-local coordinate x vQ    */
layout(location = 1)                     out vec4  vColor; /* pre-multiplied by q                  */
layout(location = 2)                flat out uvec4 vMat;   /* x: mat0, y: clx | cly<<16 (post-ladder),
                                                            * zw: HQ ladder-fade words (psx_hq_fog.glsl) */
layout(location = 3, component = 0) flat out float vKoff;  /* og OT key (feed koff mirror)         */
/* The face's terrain class byte (psx_key_contest.glsl op_key_class): the
 * key pass writes it beside the key, the scene stages bound the override
 * with it. OP_KEY_CLASS_NONE on an unkeyed face. */
layout(location = 3, component = 1) flat out float vKcls;
layout(location = 4, component = 0) flat out uint  vHd;    /* HD-fold: entry id (bits 0-15, 0=none)
                                                            * | ladder-face 1<<16 | fogbias 1<<17  */
layout(location = 4, component = 1) flat out uint  vRim;   /* 1 + accepting plane of an inner rim discarded
                                                            * per fragment (terrain.frag); 0 = not rim */
layout(location = 5, component = 0)      out vec3  vKd;    /* fold position D x vQ                 */
layout(location = 5, component = 3)      out float vQ;     /* q (frag divides UV/colour by it)     */
layout(location = 6)                flat out uvec4 vKc0;   /* drain sub-cell key table            */
layout(location = 7)                flat out uvec4 vKc1;   /*   (psx_key_contest.glsl)            */
layout(location = 8)                flat out vec4  vKn0;   /* fold corners TL.xyz TR.x             */
layout(location = 9)                flat out vec4  vKn1;   /*   TR.yz BL.xy                        */
layout(location = 10)               flat out vec4  vKn2;   /*   BL.z BR.xyz                        */

/* Select emitted as OpSelect (mix with a bool), never as a branch: t when
 * c holds, else f, for every input including NaN. The position chain's
 * clamps, min/max scans and guarded picks go through it. */
float dsel(bool c, float t, float f) { return mix(f, t, c); }

/* The camera-relative operand of mac_of for a world position w in x4 units
 * (sector origin x4 + sector-local x4, an exact int32): the walk's
 * permuted operand (s5 - Y, s6 - Z, X + s4), each axis the exact integer
 * difference against cam_int_x4 rounded to float once (under 2^24 in
 * magnitude, so the conversion is exact), then cam_frac_x4 applied, then
 * the exact * 0.25 to local units. The operands depend on w and the camera
 * alone, so a world vertex shared by two sectors yields the same float. */
#ifdef TERRAIN_SECDELTA
/* The hole census's positive control: the sector's camera operand (origin
 * against cam_int_x4, then cam_frac_x4) is rounded to float first and the
 * sector-local corner added after, so the result is rounded at
 * ulp(|origin - camera|) and then again at ulp(|v|). A world vertex two
 * sectors share reaches this from two origins and can round apart.
 * ctl_wo is the drawing sector's origin in x4 units, set in main before
 * any cam_rel call. */
ivec3 ctl_wo;
precise vec3 cam_rel(ivec3 w) {
    precise vec3 sd = vec3(float(g.cam_int_x4.y - ctl_wo.y) + g.cam_frac_x4.y,
                           float(g.cam_int_x4.z - ctl_wo.z) + g.cam_frac_x4.z,
                           float(ctl_wo.x - g.cam_int_x4.x) - g.cam_frac_x4.x);
    const ivec3 l = w - ctl_wo;
    precise vec3 v = (sd + vec3(float(-l.y), float(-l.z), float(l.x))) * 0.25;
    return v;
}
#else
precise vec3 cam_rel(ivec3 w) {
    precise float vx = float(g.cam_int_x4.y - w.y) + g.cam_frac_x4.y;
    precise float vy = float(g.cam_int_x4.z - w.z) + g.cam_frac_x4.z;
    precise float vz = float(w.x - g.cam_int_x4.x) - g.cam_frac_x4.x;
    precise vec3 v = vec3(vx, vy, vz) * 0.25;
    return v;
}
#endif
#if defined(TERRAIN_ANIM) || defined(TERRAIN_NEAR)
/* cam_rel of the poly-anim lerp prev -> cur at alpha a, taken on the exact
 * camera-relative integer differences before the fractional camera: p +
 * (c - p) * a with c - p an exact float integer, so a vertex that does not
 * move (c == p) yields p exactly, with or without FMA, and so the same
 * value as cam_rel gives its static neighbour. */
#ifdef TERRAIN_SECDELTA
/* The control's form: the sector's camera operand (cam_rel's sd, rounded at
 * ulp(|origin - camera|)) plus the lerp of the sector-local differences,
 * rounded again at ulp(|v|), so a moving vertex two sectors share can round
 * apart as cam_rel's control does for a static one; with wp == wc it is
 * cam_rel's control value exactly. */
precise vec3 cam_rel_lerp(ivec3 wp, ivec3 wc, float a) {
    precise vec3 sd = vec3(float(g.cam_int_x4.y - ctl_wo.y) + g.cam_frac_x4.y,
                           float(g.cam_int_x4.z - ctl_wo.z) + g.cam_frac_x4.z,
                           float(ctl_wo.x - g.cam_int_x4.x) - g.cam_frac_x4.x);
    const ivec3 lp = wp - ctl_wo, lc = wc - ctl_wo;
    precise vec3 p = vec3(float(-lp.y), float(-lp.z), float(lp.x));
    precise vec3 c = vec3(float(-lc.y), float(-lc.z), float(lc.x));
    precise vec3 v = (sd + (p + (c - p) * a)) * 0.25;
    return v;
}
#else
precise vec3 cam_rel_lerp(ivec3 wp, ivec3 wc, float a) {
    precise vec3 p = vec3(float(g.cam_int_x4.y - wp.y), float(g.cam_int_x4.z - wp.z),
                          float(wp.x - g.cam_int_x4.x));
    precise vec3 c = vec3(float(g.cam_int_x4.y - wc.y), float(g.cam_int_x4.z - wc.z),
                          float(wc.x - g.cam_int_x4.x));
    precise vec3 l = p + (c - p) * a;
    precise vec3 v = vec3(l.x + g.cam_frac_x4.y, l.y + g.cam_frac_x4.z,
                          l.z - g.cam_frac_x4.x) * 0.25;
    return v;
}
#endif
#endif
/* ngte_mac on the camera-relative operand v (cam_rel): MAC = (M*v)/4096,
 * the /4096 as an exact multiply. */
precise vec3 mac_of(vec3 v) {
    precise float m1 = (g.mrow0.x*v.x + g.mrow0.y*v.y + g.mrow0.z*v.z) * (1.0 / (4096.0));
    precise float m2 = (g.mrow1.x*v.x + g.mrow1.y*v.y + g.mrow1.z*v.z) * (1.0 / (4096.0));
    precise float m3 = (g.mrow2.x*v.x + g.mrow2.y*v.y + g.mrow2.z*v.z) * (1.0 / (4096.0));
    precise vec3 m = vec3(m1, m2, m3);
    return m;
}

/* proj_screen mirror: float sx = (float)(ofx + m1*H/z), with the CPU's
 * degenerate-Z floor. On plain chunks the floor never fires (an unclaimed
 * sector guarantees m3 > 0); the near variant projects pre-clip corners that
 * can sit at or behind the eye, where it does. */
precise vec2 proj_of(vec3 m) {
    float z = dsel(m.z > 1e-9, m.z, 1e-9);
    precise float sx = G_OF.x + m.x * G_H / z;
    precise float sy = G_OF.y + m.y * G_H / z;
    return vec2(float(sx), float(sy));
}

/* Integer vertex-snap mirror (push_tri): the emitted screen point is floored
 * to the PSX integer pixel grid. TRUNCATE, not round - the GTE derives SXY as
 * MAC0 >> 16, an arithmetic shift. X/Y only: camera-space Z, the perspective
 * q and the UVs are untouched, so depth and texturing stay unquantised. The
 * grid is display-resolution-independent because this point is pre-normalise
 * (clip_of upscales afterwards). The face verdicts above run on the UNSNAPPED
 * projections, exactly as the walk's NCLIP runs on sv[].sx. */
vec2 snap_of(vec2 s) { return pc.snap != 0 ? floor(s) : s; }


/* HQ sub-quad node ids: 0=TL 1=TR 2=BR 3=BL; 4..7 = mT,mB,mL,mR; 8 = centre.
 * sq[q] = {TL,mT,mL,C} {mT,TR,C,mR} {mL,C,BL,mB} {C,mR,mB,BR}. */
const int NODEMAP[4][4] = int[4][4](int[4](0, 4, 6, 8),
                                    int[4](4, 1, 8, 7),
                                    int[4](6, 8, 3, 5),
                                    int[4](8, 7, 5, 2));

/* terrain_near_emit's snapped-screen overrides for a subdivided face's nodes.
 *
 * Parent corners and the interior centre carry the floor of their own
 * projection, which is what the generic snap above already produces, so only
 * a BOUNDARY MIDPOINT differs: it is placed on the SEGMENT between its two
 * snapped parent corners, at the parameter of the true camera-space
 * midpoint's projection. That keeps the midpoint collinear with the snapped
 * parent edge - the unsubdivided neighbour sharing that edge draws the same
 * line, so no T-junction crack opens - while staying perspective-correct
 * along the edge, which a plain screen midpoint would not be (the HQ tiles
 * are attached to these vertices, and a screen-parametric midpoint re-warps
 * near textures the way the subdivide exists to prevent).
 *
 * The guard is per edge: an endpoint inside the near plane projects mirrored
 * and unusable, and the neighbour sharing the edge must reach the identical
 * verdict for the shared midpoint, which is a property of the two shared
 * endpoints alone. An unsafe edge gets no override and its midpoint falls
 * back to the floor of its own projection. */
vec2 hq_snap_screen(int node, vec3 TL, vec3 TR, vec3 BL, vec3 BR, vec3 m)
{
    if (pc.snap == 0) return proj_of(m);
    if (node < 4 || node == 8) return floor(proj_of(m));
    vec3 ea, eb;
    if      (node == 4) { ea = TL; eb = TR; }
    else if (node == 5) { ea = BL; eb = BR; }
    else if (node == 6) { ea = TL; eb = BL; }
    else                { ea = TR; eb = BR; }
    if (ea.z < G_NEAR || eb.z < G_NEAR) return floor(proj_of(m));
    vec2 A = floor(proj_of(ea)), B = floor(proj_of(eb));
    vec2 mm = proj_of(m);
    precise float dx = B.x - A.x, dy = B.y - A.y;
    precise float L2 = dx * dx + dy * dy;
    precise float num = (mm.x - A.x) * dx + (mm.y - A.y) * dy;
    float tt = L2 > 0.0 ? num / L2 : 0.5;
    if (tt < 0.0) tt = 0.0; else if (tt > 1.0) tt = 1.0;
    precise float rx = A.x + tt * dx, ry = A.y + tt * dy;
    return vec2(rx, ry);
}

/* NODE_MID: the mean of two camera-space nodes, (a + b) * 0.5. */
#if defined(TERRAIN_SECDELTA) && defined(TERRAIN_READBACK)
/* The readback control's per-triangle parity, set in main: a quad's second
 * triangle, inverted on every other cell of a 4x4 grid (a checkerboard) and
 * on every odd sub-triangle of a split triangle, so the two triangles of one
 * cell and neighbouring cells or sub-triangles take opposite parities.
 * cv_lerp_n reverses its cut order on it; a cell vertex with it set
 * (ctl_alt) takes its node means as a + (b - a) * 0.5, so a node two
 * triangles share can round apart. */
bool ctl_rev = false;
bool ctl_alt = false;
#define NODE_MID(a, b) (ctl_alt ? (a) + ((b) - (a)) * 0.5 : ((a) + (b)) * 0.5)
#else
#define NODE_MID(a, b) (((a) + (b)) * 0.5)
#endif

/* ---- 4x4 near cells (terrain.c nc4_* mirrors) ----------------------------
 * A tier-4 face (face_near_tier4) draws as a 5x5 node grid, 16 cells. A
 * cell vertex carries its cell in mat_near1 (bit 31 set, cell in bits 0-3)
 * and its corner slot [TL,TR,BL,BR] in the role bits; node (r, c) is
 * rebuilt from the transformed parent corners in the walk's halving order. */
precise vec3 nc4_pt(vec3 a, vec3 b, int k) {
    if (k == 0) return a;
    if (k == 4) return b;
    precise vec3 m = NODE_MID(a, b);
    if (k == 2) return m;
    precise vec3 q = NODE_MID((k == 1 ? a : b), m);
    return q;
}
precise vec3 nc4_node(int r, int c, vec3 TL, vec3 TR, vec3 BL, vec3 BR) {
    return nc4_pt(nc4_pt(TL, BL, r), nc4_pt(TR, BR, r), c);
}

/* nc4_snap mirror: corner and interior nodes floor their own projection; an
 * edge node sits on the segment between its edge's snapped corners at the
 * parameter of its own projection, unless an edge corner is inside the near
 * plane. */
/* nc4_snap_edge mirror: the point of the segment between the edge corners'
 * snapped projections at the parameter of the node's own projection, or the
 * floor of that projection when an edge corner is inside the near plane. */
vec2 edge_snap_screen(vec3 ea, vec3 eb, vec3 m)
{
    if (ea.z < G_NEAR || eb.z < G_NEAR) return floor(proj_of(m));
    vec2 A = floor(proj_of(ea)), B = floor(proj_of(eb));
    vec2 mm = proj_of(m);
    precise float dx = B.x - A.x, dy = B.y - A.y;
    precise float L2 = dx * dx + dy * dy;
    precise float num = (mm.x - A.x) * dx + (mm.y - A.y) * dy;
    float tt = L2 > 0.0 ? num / L2 : 0.5;
    if (tt < 0.0) tt = 0.0; else if (tt > 1.0) tt = 1.0;
    precise float rx = A.x + tt * dx, ry = A.y + tt * dy;
    return vec2(rx, ry);
}

vec2 nc4_snap_screen(int r, int c, vec3 TL, vec3 TR, vec3 BL, vec3 BR, vec3 m)
{
    if (pc.snap == 0) return proj_of(m);
    bool er = (r == 0 || r == 4), ec = (c == 0 || c == 4);
    if (er == ec) return floor(proj_of(m));
    vec3 ea = er ? (r == 0 ? TL : BL) : (c == 0 ? TL : TR);
    vec3 eb = er ? (r == 0 ? TR : BR) : (c == 0 ? BL : BR);
    return edge_snap_screen(ea, eb, m);
}

/* nc4_morph mirror: an odd-odd node moves toward the mean of its up-right
 * and down-left neighbours' emitted points, weight at = least corner SZ3 -
 * 0x40, skipped when at <= 0 or the node's own integer projection is off
 * the 512x256 GTE screen (nc4_outcode). Applied under AFFINE_UV only: the
 * morph (r_environment.s 0x80028D1C-0x80028E44) moves the screen point while
 * UV and w stay, which perspective-correct texturing renders as a warp. */
vec2 nc4_screen(int r, int c, vec3 TL, vec3 TR, vec3 BL, vec3 BR)
{
    vec3 m = nc4_node(r, c, TL, TR, BL, BR);
    vec2 sc = nc4_snap_screen(r, c, TL, TR, BL, BR, m);
    if (!AFFINE_UV || (r & 1) == 0 || (c & 1) == 0) return sc;
    float mn = min(min(TL.z, TR.z), min(BL.z, BR.z));
    precise float at = mn * 4.0 - 64.0;
    if (at <= 0.0 || m.z <= 0.0) return sc;
    at = min(at, 256.0);             /* pinned far faces: full weight, never past it */
    vec2 op = floor(proj_of(m));
    if (op.y <= 0.0 || op.y >= 256.0 || op.x <= 0.0 || op.x >= 512.0) return sc;
    vec2 a = nc4_snap_screen(r - 1, c + 1, TL, TR, BL, BR, nc4_node(r - 1, c + 1, TL, TR, BL, BR));
    vec2 b = nc4_snap_screen(r + 1, c - 1, TL, TR, BL, BR, nc4_node(r + 1, c - 1, TL, TR, BL, BR));
    precise float mx = PDIV((256.0 - at) * float(sc.x) + at * 0.5 * (float(a.x) + float(b.x)), 256.0);
    precise float my = PDIV((256.0 - at) * float(sc.y) + at * 0.5 * (float(a.y) + float(b.y)), 256.0);
    vec2 o = vec2(float(mx), float(my));
    return pc.snap != 0 ? floor(o) : o;
}

/* ---- split triangles (terrain.c nt4_* / nt_* mirrors) --------------------
 * A tier-4 triangle (corners A = i0, B = i1, C = i2) draws as 16
 * sub-triangles over 15 nodes, node (r, c) at A(4-r-c)/4 + Bc/4 + Cr/4; a
 * tier-2 triangle as 4 sub-triangles over the grid's nodes 0, 2, 4, 9, 11
 * and 14 (the corners and edge midpoints). A sub-triangle vertex carries
 * bit 30 beside the cell bit 31 in mat_near1, its sub-triangle in bits 4-8
 * (16-19: tier 2) and its cell or quadrant in bits 0-3; its member slot is
 * the role. NT_SUB is D_8006D0F8's node ids (A, B, C per sub-triangle),
 * then D_8006D0E8's mapped onto the 15-node numbering (k_nt2_node). */
const int NT_SUB[60] = int[60](0, 1, 5,   6, 5, 1,   1, 2, 6,   7, 6, 2,
                               2, 3, 7,   8, 7, 3,   3, 4, 8,   5, 6, 9,
                               10, 9, 6,  6, 7, 10,  11, 10, 7, 7, 8, 11,
                               9, 10, 12, 13, 12, 10, 10, 11, 13, 12, 13, 14,
                               0, 2, 9,   11, 9, 2,  2, 4, 11,  9, 11, 14);
/* A node's face edge: 0 none, 1 AB, 2 CA, 3 BC (k_nt4_edge). */
const int NT4_EDGE[15] = int[15](0, 1, 1, 1, 0, 2, 0, 0, 3, 2, 0, 3, 2, 3, 0);

/* nt4_grid mirror: nodes by the drain's halving order, each the camera-space
 * mean of its two parents. */
precise vec3 nt4_node(int n, vec3 A, vec3 B, vec3 C) {
    if (n == 0) return A;
    if (n == 4) return B;
    if (n == 14) return C;
    precise vec3 m2 = NODE_MID(A, B), m9 = NODE_MID(A, C), m11 = NODE_MID(B, C);
    if (n == 2) return m2;
    if (n == 9) return m9;
    if (n == 11) return m11;
    precise vec3 r;
    if      (n == 1)  r = NODE_MID(A, m2);
    else if (n == 3)  r = NODE_MID(B, m2);
    else if (n == 5)  r = NODE_MID(A, m9);
    else if (n == 12) r = NODE_MID(C, m9);
    else if (n == 8)  r = NODE_MID(B, m11);
    else if (n == 13) r = NODE_MID(C, m11);
    else if (n == 6)  r = NODE_MID(A, m11);
    else if (n == 7)  r = NODE_MID(B, m9);
    else              r = NODE_MID(C, m2);   /* node 10 */
    return r;
}

/* nt4_snap mirror: a corner or interior node floors its own projection, an
 * edge node sits on its edge's snapped segment. */
vec2 nt4_screen(int n, vec3 A, vec3 B, vec3 C) {
    vec3 m = nt4_node(n, A, B, C);
    if (pc.snap == 0) return proj_of(m);
    int e = NT4_EDGE[n];
    if (e == 0) return floor(proj_of(m));
    return edge_snap_screen(e == 3 ? B : A, e == 1 ? B : C, m);
}

/* ---- drain sub-cell keys (terrain.c tn_drain_sz, tn_drain_key4 and
 * tn_drain_key3 mirrors) ----------------------------------------------------
 * A drain node's term: its SZ3 (the GTE saturates floor(hqscale * m3) to
 * [0, 0xFFFF]) masked to the 12 bits the node word keeps. A quad cell sums
 * its four nodes, a sub-triangle SZ A + SZ B + 2 SZ C. Only a face in the
 * near band is drained; the rest key whole. */
float drain_sz(float m3) {
    float sz = floor(m3 * 4.0);
    if (sz <= 0.0) return 0.0;
    sz = dsel(sz > 65535.0, 65535.0, sz);
    return float(int(sz) & 0xFFF);
}
float drain_key4(float a, float b, float c, float d) {
    precise float s = drain_sz(a) + drain_sz(b);
    s = s + drain_sz(c);
    s = s + drain_sz(d);
    return float(s);
}
float drain_key3(float a, float b, float c) {
    precise float s = drain_sz(a) + drain_sz(b);
    s = s + 2.0 * drain_sz(c);
    return float(s);
}

float det3d(vec3 a, vec3 b, vec3 c);

/* Node n's place (r, c) in the 15-node triangle grid (terrain.c k_nt4_rc). */
const int NT4_RC[30] = int[30](0, 0,  0, 1,  0, 2,  0, 3,  0, 4,  1, 0,  1, 1,  1, 2,
                               1, 3,  2, 0,  2, 1,  2, 2,  3, 0,  3, 1,  4, 0);

/* The drain key table (psx_key_contest.glsl): a face's sub-cell keys packed
 * as their excess over the least one, by grid position. */
struct dtab { uvec4 t0; uvec4 t1; float kmin; int n; };

void dtab_put(inout dtab T, int slot, uint v) {
    uint sh = uint(slot & 1) * 16u;
    int w = (slot >> 1) & 3;
    if (slot < 8) T.t0[w] |= v << sh; else T.t1[w] |= v << sh;
}

/* tn_sub_slot mirror: a sub-triangle's table entry from its nodes' grid
 * places, h the grid step (1 tier 4, 2 tier 2). An upward sub-triangle has
 * two nodes on its top row, a downward one a single node. */
int drain_sub_slot(int a, int b, int c, int h) {
    int ra = NT4_RC[a * 2], rb = NT4_RC[b * 2], rc = NT4_RC[c * 2];
    int rmin = min(ra, min(rb, rc));
    int cmin = min(NT4_RC[a * 2 + 1], min(NT4_RC[b * 2 + 1], NT4_RC[c * 2 + 1]));
    int top = int(ra == rmin) + int(rb == rmin) + int(rc == rmin);
    int i = cmin / h, j = rmin / h;
    return top == 2 ? j * 4 + i : (3 - j) * 4 + (3 - i);
}

/* Pack keys[] (bit i of onm: the sub-cells the drain inserts, slot[] their
 * entries) over their least key, clamped to 0x3FFE, a skipped sub-cell as
 * the sentinel 0x3FFF (OP_DRAIN_SKIP); grid in entry 0's top bits
 * (terrain.c tn_keycells_pack). */
void dtab_pack(inout dtab T, float keys[16], uint onm, int slot[16], int cnt, uint grid) {
    T.t0 = uvec4(0u); T.t1 = uvec4(0u);
    if (T.n == 0) return;
    for (int i = 0; i < cnt; i++)
        dtab_put(T, slot[i], ((onm >> uint(i)) & 1u) != 0u ? min(uint(keys[i] - T.kmin), 0x3FFEu)
                                                           : 0x3FFFu);
    T.t0.x |= grid << 14;
}

uint quad_fold_mask(vec3 TL, vec3 TR, vec3 BL, vec3 BR, uint onm);

/* tn_quad_cell_keys mirror: the keys of the 16 cells the drain inserts a
 * tier-4 quad as, over the grid of corners TL, TR, BL, BR; a cell whose key
 * is 0 carries none, nor, when `cull` (a straddling single-sided face), one
 * facing away over both of its triples (nc4_front). A folded quadrant q
 * (quad_fold_mask) sets bit 14 of entry 2 + q (tn_keycells_fold). Each
 * cell's four grid nodes are rebuilt by nc4_node where they are read, not
 * held for the whole table: nc4_node is a pure function of its operands,
 * so a node read twice is the same value, and the vertex stage keeps no
 * 25-node array live. */
dtab quad_cells_tab(vec3 TL, vec3 TR, vec3 BL, vec3 BR, bool cull, bool flip)
{
    dtab T;
    T.kmin = 0.0; T.n = 0;
    float keys[16]; int slot[16];
    uint onm = 0u;
    for (int i = 0; i < 16; i++) {
        int r = i >> 2, c = i & 3;
        vec3 tl = nc4_node(r, c, TL, TR, BL, BR), tr = nc4_node(r, c + 1, TL, TR, BL, BR);
        vec3 bl = nc4_node(r + 1, c, TL, TR, BL, BR), br = nc4_node(r + 1, c + 1, TL, TR, BL, BR);
        float key = drain_key4(tl.z, tr.z, bl.z, br.z);
        bool f = true;
        if (cull) {
            precise float d1 = det3d(tl, tr, bl);
            f = flip ? (d1 <= 0.0) : (d1 >= 0.0);
            if (!f) {
                precise float d2 = det3d(tr, br, bl);
                f = flip ? (d2 <= 0.0) : (d2 >= 0.0);
            }
        }
        keys[i] = key; slot[i] = i;
        if (!(f && key > 0.0)) continue;
        onm |= 1u << uint(i);
        if (T.n == 0 || key < T.kmin) T.kmin = key;
        T.n++;
    }
    dtab_pack(T, keys, onm, slot, 16, 1u);
    if (T.n > 0) {
        uint fm = quad_fold_mask(TL, TR, BL, BR, onm);
        for (int q = 0; q < 4; q++)
            if ((fm & (1u << q)) != 0u) dtab_put(T, 2 + q, 0x4000u);
    }
    return T;
}

/* tn_tri_cell_keys mirror: the same over a triangle's tier-4 (16) or tier-2
 * (4) sub-triangles, a sub-triangle facing away over its own triple
 * skipped when `cull`. The 15 grid nodes are rebuilt by nt4_node where they
 * are read, as quad_cells_tab rebuilds its nodes. */
dtab tri_cells_tab(vec3 A, vec3 B, vec3 C, bool tier2, bool cull, bool flip)
{
    int base = tier2 ? 16 : 0, cnt = tier2 ? 4 : 16;
    dtab T;
    T.kmin = 0.0; T.n = 0;
    float keys[16]; int slot[16];
    uint onm = 0u;
    for (int i = 0; i < 16; i++) { keys[i] = 0.0; slot[i] = 0; }
    for (int i = 0; i < cnt; i++) {
        int t = (base + i) * 3;
        vec3 a = nt4_node(NT_SUB[t], A, B, C), b = nt4_node(NT_SUB[t + 1], A, B, C);
        vec3 c = nt4_node(NT_SUB[t + 2], A, B, C);
        float key = drain_key3(a.z, b.z, c.z);
        bool f = true;
        if (cull) {
            precise float d = det3d(a, b, c);
            f = !(flip ? (d > 0.0) : (d < 0.0));
        }
        keys[i] = key;
        slot[i] = drain_sub_slot(NT_SUB[t], NT_SUB[t + 1], NT_SUB[t + 2], tier2 ? 2 : 1);
        if (!(f && key > 0.0)) continue;
        onm |= 1u << uint(i);
        if (T.n == 0 || key < T.kmin) T.kmin = key;
        T.n++;
    }
    dtab_pack(T, keys, onm, slot, cnt, tier2 ? 3u : 2u);
    return T;
}

/* The drain coordinate (psx_key_contest.glsl) of a 15-node triangle grid
 * node: its face-local place in quarter-edge units. */
vec2 nt4_fl(int n) { return vec2(float(NT4_RC[n * 2 + 1]), float(NT4_RC[n * 2])); }

/* terrain.c tn_quad_sides mirror: point V's side coordinates of the 2x2
 * quadrant with corners Q0..Q3 (TL, TR, BL, BR), term by term as the walk
 * computes them: the eye-plane normals of its column and row isolines,
 * dotted with V and scaled to 1 at TR and BL. */
vec3 eye_normal(vec3 a, vec3 b) {
    precise float x = a.y * b.z - b.y * a.z;
    precise float y = a.z * b.x - b.z * a.x;
    precise float z = a.x * b.y - b.x * a.y;
    return vec3(x, y, z);
}
float eye_side(vec3 n, vec3 v) {
    precise float d = n.x * v.x + n.y * v.y;
    d = d + n.z * v.z;
    return d;
}
vec2 quad_side_fl(vec3 Q0, vec3 Q1, vec3 Q2, vec3 Q3, vec3 V) {
    precise vec3 ct = (Q0 + Q1) * 0.5, cb = (Q2 + Q3) * 0.5;
    precise vec3 rl = (Q0 + Q2) * 0.5, rr = (Q1 + Q3) * 0.5;
    vec3 nc = eye_normal(ct, cb), nr = eye_normal(rl, rr);
    float sc = eye_side(nc, Q1), sr = eye_side(nr, Q2);
    if (sc == 0.0) sc = 1.0;
    if (sr == 0.0) sr = 1.0;
    return vec2(float(eye_side(nc, V) / sc), float(eye_side(nr, V) / sr));
}

/* terrain.c tn_quad_folds mirror, term by term: bit q set when a drawn cell
 * of 2x2 quadrant q has a node strictly (1e-6 of the scale) across one of
 * the quadrant's isoline eye planes into a skipped cell's slot. The 5x5
 * node grid is nc4_node over corners TL, TR, BL, BR; bit i of onm is drawn
 * cell i. */
uint quad_fold_mask(vec3 TL, vec3 TR, vec3 BL, vec3 BR, uint onm) {
    uint fm = 0u;
    for (int qd = 0; qd < 4; qd++) {
        int r0 = (qd >> 1) * 2, c0 = (qd & 1) * 2;
        vec3 Q0 = nc4_node(r0, c0, TL, TR, BL, BR), Q1 = nc4_node(r0, c0 + 2, TL, TR, BL, BR);
        vec3 Q2 = nc4_node(r0 + 2, c0, TL, TR, BL, BR), Q3 = nc4_node(r0 + 2, c0 + 2, TL, TR, BL, BR);
        precise vec3 ct = (Q0 + Q1) * 0.5, cb = (Q2 + Q3) * 0.5;
        precise vec3 rl = (Q0 + Q2) * 0.5, rr = (Q1 + Q3) * 0.5;
        vec3 nc = eye_normal(ct, cb), nr = eye_normal(rl, rr);
        float sc = eye_side(nc, Q1), sr = eye_side(nr, Q2);
        if (sc == 0.0) sc = 1.0;
        if (sr == 0.0) sr = 1.0;
        bool f = false;
        for (int k = 0; k < 4 && !f; k++) {
            int lr = k >> 1, lc = k & 1;
            if (((onm >> uint((r0 + lr) * 4 + c0 + lc)) & 1u) == 0u) continue;
            for (int n = 0; n < 4 && !f; n++) {
                vec3 v = nc4_node(r0 + lr + (n >> 1), c0 + lc + (n & 1), TL, TR, BL, BR);
                float fx = eye_side(nc, v) / sc, fy = eye_side(nr, v) / sr;
                int xc = (lc != 0 ? fx < -1e-6 : fx > 1e-6) ? 1 : 0;
                int xr = (lr != 0 ? fy < -1e-6 : fy > 1e-6) ? 1 : 0;
                if ((xc | xr) != 0 && ((onm >> uint((r0 + (lr ^ xr)) * 4 + c0 + (lc ^ xc))) & 1u) == 0u)
                    f = true;
            }
        }
        if (f) fm |= 1u << qd;
    }
    return fm;
}

/* A camera-space point in the homogeneous screen space of the fold test
 * (psx_key_contest.glsl): (OFX z + H x, OFY z + H y, z), terrain.c
 * tn_fold_corner's terms. */
vec3 fold_h(vec3 m) {
    precise float hx = G_OF.x * m.z + G_H * m.x;
    precise float hy = G_OF.y * m.z + G_H * m.y;
    return vec3(float(hx), float(hy), float(m.z));
}

/* GTE_SAT mirror (of the float screen coord, like the CPU's NSAT(sv.sx)). */
float nsat(float v, bool unr_ovf) {
    if (unr_ovf) return float(v);
    return dsel(v < -1024.0, -1024.0, dsel(v > 1023.0, 1023.0, float(v)));
}

/* terrain.c tface_gte_flagged mirror, one corner: the .s vertex flag 0x10
 * (GTE FLAG bit 31 read for SZ3 < 0x600): SZ3 saturated at 0, SX2/SY2
 * saturated, or the UNR divide overflow (m3 < H_canon / 8). */
bool gte_flag(float mz, vec2 p) {
    if (mz <= 0.0) return true;
    if (mz >= 384.0) return false;
    if (p.x < -1024.0 || p.x > 1023.0 || p.y < -1024.0 || p.y > 1023.0) return true;
    return mz < PDIV(G_HCAN, 8.0);
}

/* ngte_dpcs_ch mirror (GTE-exact DPCS fog, one channel; pure int math). */
int dpcs_ch(int c8, int fc12, int ir0) {
    int mac = c8 << 16;
    int ird = clamp(((fc12 << 12) - mac) >> 12, -32768, 32767);
    int o = ((mac + ir0 * ird) >> 12) >> 4;
    return clamp(o, 0, 255);
}

/* One corner's terrain colour, the walk's terrain_color + lp_grade_apply:
 * flat HP corners fog on their OWN view-Z (GTE-exact DPCS), LP/HQ take the
 * baked raw colour, and an active replacement pack regrades LP. This is the
 * colour the feed resolves per corner BEFORE any near-plane clip, so the
 * clip mirror interpolates its output rather than re-fogging a cut vertex. */
#if defined(TERRAIN_ANIM) && !defined(TERRAIN_CELLS)
/* Env-anim colour lerp (canim_lerp mirror): one colour word at the build's
 * colour alpha of the interval prev -> cur, per channel, rounded to nearest
 * onto the 8-bit grid before the fog blend. Alpha 1 selects cur. */
vec3 canim_col(uint prev, vec3 cur)
{
    float a = G_CA;
    if (a == 1.0) return cur;
    ivec3 c = ivec3(round(cur * 255.0));
    ivec3 p = ivec3(int(prev & 0xFFu), int((prev >> 8) & 0xFFu), int((prev >> 16) & 0xFFu));
    precise vec3 v = vec3(p) + vec3(c - p) * a + 0.5;
    return vec3(ivec3(v)) / 255.0;
}
#endif

vec3 terrain_col(float mz, vec3 cfar, vec3 cnear, bool doFog, int fogRef,
                 float fogscale, bool grade, vec4 gp, vec4 gq)
{
    vec3 col;
    if (doFog) {
        int ir0 = fogRef - int(mz * fogscale);
        if (ir0 >= 4096) {
            col = cnear;
        } else if (ir0 <= 0) {
            col = cfar;
        } else {
            ivec3 f8 = ivec3(round(cfar  * 255.0));
            ivec3 n8 = ivec3(round(cnear * 255.0));
            col = vec3(float(dpcs_ch(f8.r, n8.r << 4, ir0)) / 255.0,
                       float(dpcs_ch(f8.g, n8.g << 4, ir0)) / 255.0,
                       float(dpcs_ch(f8.b, n8.b << 4, ir0)) / 255.0);
        }
    } else {
        col = cfar;   /* LP / HQ: raw baked, never fogged */
    }
    /* Pack-aware LP colour grade (lp_grade_apply mirror): the bake carries
     * raw authored colours; with a replacement pack active the sector's
     * grade affine (own HP-texture fit, or the level mean) rides the sector
     * params. Same float32 ops and trunc-after-+0.5 as the walk; WORLD LP
     * and fill LP alike, never HQ (the walk grades passLP only). */
    if (grade) {
        for (int c = 0; c < 3; c++) {
            precise float t = gp[c] * round(col[c] * 255.0);
            t = t + gq[c] * 255.0;
            t = t + 0.5;
            col[c] = float(clamp(int(t), 0, 255)) / 255.0;
        }
    }
    return col;
}

/* face_is_near_canonical mirror (hqscale 4.0; per-corner saturation at 0).
 * Used both for the HQ variant select (via face_is_near = this, or the
 * NOLOD short-circuit) and for the koff near-band/otslots-zero verdict
 * (ALWAYS this canonical form, even under NOLOD). */
/* Camera-space triple product of three terrain corners (the walk's
 * tface_det3): the sign of the projected NCLIP over the same triple, and
 * still defined for a corner behind the eye. */
float det3d(vec3 a, vec3 b, vec3 c) {
    precise float d = a.x * (b.y * c.z - b.z * c.y)
                     - a.y * (b.x * c.z - b.z * c.x)
                     + a.z * (b.x * c.y - b.y * c.x);
    return d;
}

bool near_canonical(float z0, float z1, float z2, float z3, uint w3_40, uint w3_80) {
    precise float sz0 = dsel(z0 > 0.0, z0 * 4.0, 0.0), sz1 = dsel(z1 > 0.0, z1 * 4.0, 0.0);
    precise float sz2 = dsel(z2 > 0.0, z2 * 4.0, 0.0), sz3 = dsel(z3 > 0.0, z3 * 4.0, 0.0);
    precise float sum = sz0 + sz1 + sz2 + sz3;
    if (sum <= 0.0) return false;                 /* fully behind camera */
    if (w3_80 != 0u) return false;                  /* hard-excluded from HQ */
    if (sum >= 8192.0) return false;              /* not foreground (0x2000) */
    float mn = min(min(sz0, sz1), min(sz2, sz3));
    if (mn < 320.0) return true;                  /* a corner SZ3 < 0x140 */
    return w3_40 == 0u;
}

/* tn_sz3 mirror: a corner's GTE SZ3 on a scale, truncated and saturated to
 * [0, 0xFFFF]. */
int sz3_of(float m, float scale)
{
    if (m <= 0.0) return 0;
    precise float v = m * scale;
    return v >= 65535.0 ? 0xFFFF : int(v);
}

/* tn_sz3_sum mirror: a face's corner-SZ3 sum (a triangle passes its shared
 * corner twice as z3). */
int sz3_sum(float z0, float z1, float z2, float z3, float scale)
{
    return sz3_of(z0, scale) + sz3_of(z1, scale) + sz3_of(z2, scale) + sz3_of(z3, scale);
}

/* tn_water_rgb mirror: the game's second-walk corner colour, the sector
 * colour DPCS-faded toward black (FC 0) with IR0 = clamp(2 * (SZ3 - 0x1000),
 * 0, 0xFFF) at SZ3 = mz * fogscale; unfaded under fog mode off. */
vec3 water_col(vec3 c, float mz, float fogscale, bool fogOff)
{
    if (fogOff) return c;   /* fog mode off: no depth cue */
    int ir0 = clamp(2 * (sz3_of(mz, fogscale) - 0x1000), 0, 0xFFF);
    ivec3 c8 = ivec3(round(c * 255.0));
    return vec3(float(dpcs_ch(c8.r, 0, ir0)), float(dpcs_ch(c8.g, 0, ir0)),
                float(dpcs_ch(c8.b, 0, ir0))) / 255.0;
}

/* tn_ladder_t0k mirror: a face's ladder select key, t0k <= 0 the near tile
 * half, else t0k >> 8 the CLUT row. Per-corner int casts, then sum (the
 * walk's exact quantisation); ladnear is the nearest-corner rule (Spyro 2/3):
 * row = 15 - (max clamped IR0 >> 8), t0k = 256 * row. */
int ladder_t0k(float z0, float z1, float z2, float z3, int fogRef,
               float fogscale, bool ladnear)
{
    if (ladnear) {
        int irmax = 0;
        irmax = max(irmax, clamp(fogRef - int(z0 * fogscale), 0, 0xFFF));
        irmax = max(irmax, clamp(fogRef - int(z1 * fogscale), 0, 0xFFF));
        irmax = max(irmax, clamp(fogRef - int(z2 * fogscale), 0, 0xFFF));
        irmax = max(irmax, clamp(fogRef - int(z3 * fogscale), 0, 0xFFF));
        return (15 - (irmax >> 8)) << 8;
    }
    int szsum = int(z0 * fogscale) + int(z1 * fogscale)
              + int(z2 * fogscale) + int(z3 * fogscale);
    return ((szsum >> 7) << 5) - fogRef + 4096;
}

/* tn_flat_atmo_words mirror: the heavy-fog texel fade stepped at the face's
 * ladder row, pre-weighted (word b bit 30): aN = 1 - row/15, bN = 0.5 row/15
 * per channel in 1/256 units, integer-rounded as the walk rounds them. */
uvec2 flat_atmo_words(int t0k)
{
    int row = t0k <= 0 ? 0 : min(t0k >> 8, 15);
    uint aN = uint((256 * (15 - row) + 7) / 15);
    uint bN = uint((128 * row + 7) / 15);
    return uvec2(0x80000000u | aN | (aN << 10) | (aN << 20),
                 0x40000000u | bN | (bN << 10) | (bN << 20));
}

/* tn_clear_words mirror: the texel fades to zero on the ladder ramp, aN = 0
 * and bN = 0, word a bit 30 the fog-reference bias, under the per-pixel
 * form (fogPP); pre-weighted by the face's ladder row under the original
 * form, aN = 1 - row / 15 with word b bit 30 set. */
uvec2 clear_words(bool fogPP, bool fogbias, int t0k)
{
    if (fogPP) return uvec2(0x80000000u | (fogbias ? 0x40000000u : 0u), 0u);
    int row = t0k <= 0 ? 0 : min(t0k >> 8, 15);
    uint aN = uint((256 * (15 - row) + 7) / 15);
    return uvec2(0x80000000u | aN | (aN << 10) | (aN << 20), 0x40000000u);
}

/* face_near_tier4 mirror: a canonical near face with near_flag_tier2 clear
 * and a corner SZ3 under near_corner (0x140). */
bool nc4_tier4(float z0, float z1, float z2, float z3, uint w3_40, uint w3_80) {
    if (w3_40 != 0u) return false;
    if (!near_canonical(z0, z1, z2, z3, w3_40, w3_80)) return false;
    return min(min(z0, z1), min(z2, z3)) * 4.0 < 320.0;
}

/** @brief Returns the LP insert's constant base, in 128-world slots. */
uint ot_lp_base() { return uint(g.fog.w) & 0xFFu; }

/** @brief Returns the HP insert's multiplier on the face's bias field. */
uint ot_hp_mul() { return (uint(g.fog.w) >> 12) & 0xFu; }

/** @brief Returns whether the LP far cull tests the whole LP bucket index. */
bool ot_farcull_keyed() { return (uint(g.fog.w) & 0x10000u) != 0u; }

/** @brief Returns an LP face's authored slot offset: the base plus its biased slots.
 *
 * Mirrors tface_ot_slots for an LP face.
 * @param ob The face's bias field.
 * @return The offset in 128-world slots. */
float ot_lp_slots(uint ob) {
    return float(ot_lp_base() + ((uint(g.fog.w) >> 8) & 0xFu) * ob);
}

/* The face-attribution identity of debug tint mode 7, 22 bits: the decode
 * slot (bits 0-8), LP tier (9), variant (10-11: 0 flat, 1 HQ 2x2 quadrant,
 * 2 4x4 cell), drain sub-cell keys (12), near-variant chunk (13), and an
 * 8-bit FNV-1a fold of the twelve parent-corner coordinates (14-21). The
 * fold is the one OPENPETE_TERRAIN_GPU_FACEDUMP writes per baked face
 * (terrain.c tgpu_corner_hash8), so a decoder names the face by slot, tier
 * and fold. */
uint attr_word(uint slot, bool lp, bool hqv, bool is4, bool drain)
{
    const int cc[12] = int[12](inC0.x, inC0.y, inC0.z, inC0.w, inC1.x, inC1.y,
                               inC1.z, inC1.w, inC2.x, inC2.y, inC2.z, inC2.w);
    uint h = 2166136261u;
    for (int i = 0; i < 12; i++) h = (h ^ (uint(cc[i]) & 0xFFFFu)) * 16777619u;
    h = (h ^ (h >> 8) ^ (h >> 16) ^ (h >> 24)) & 0xFFu;
    uint var = is4 ? 2u : (hqv ? 1u : 0u);
#ifdef TERRAIN_NEAR
    const uint nearv = 1u;
#else
    const uint nearv = 0u;
#endif
    return (slot & 0x1FFu) | (lp ? 1u << 9 : 0u) | (var << 10) |
           (drain ? 1u << 12 : 0u) | (nearv << 13) | (h << 14);
}

#ifdef TERRAIN_READBACK
/* ---- shared-vertex position readback (diagnostic) -------------------------
 * The variant draws each vertex as a point into an RGBA32_UINT target at a
 * pixel its element addresses: pass 0 (pc.pad0) writes the bits of the
 * vertex's final gl_Position, pass 1 its key. Every camera-space position
 * this stage emits is a tree of operations on integer world corners: a leaf
 * is cam_rel (or cam_rel_lerp of a previous -> current pair) of one corner,
 * an inner node the mean (a + b) * 0.5 of two positions (HQ, nc4, nt4 and
 * split nodes), a cut the clip of an edge at zc with its endpoints in
 * canonical order. The key hashes that tree (the operands of a commutative
 * mean unordered, a static lerp pair as its plain leaf), so two vertices
 * carry one key when they apply the same operations to the same operands,
 * and the stage's contract is that such vertices land on bit-identical
 * clip positions. Key words x..z are the hash, w the root's kind; rb_emit
 * adds the drawing sector's slot to w (bits 8-16) for the pairing's
 * cross-sector census. */
#define RB_LEAF 1u
#define RB_NODE 2u
#define RB_CUT  3u
#define RB_W    4096u   /* target width in pixels */

/* One invertible mixing round over four 32-bit lanes (pcg4d). */
uvec4 rb_round(uvec4 s) {
    s = s * 1664525u + 1013904223u;
    s.x += s.y * s.w; s.y += s.z * s.x; s.z += s.x * s.y; s.w += s.y * s.z;
    s ^= s >> 16u;
    s.x += s.y * s.w; s.y += s.z * s.x; s.z += s.x * s.y; s.w += s.y * s.z;
    return s;
}
/* Absorb one word into a hash state. */
uvec4 rb_in(uvec4 s, uint w) {
    return rb_round(s ^ uvec4(w, w * 0x9E3779B9u, ~w, w + 0x7F4A7C15u));
}
/* Absorb a key's four words. */
uvec4 rb_key_in(uvec4 s, uvec4 k) { return rb_in(rb_in(rb_in(rb_in(s, k.x), k.y), k.z), k.w); }
/* Absorb an integer world position. */
uvec4 rb_w_in(uvec4 s, ivec3 w) { return rb_in(rb_in(rb_in(s, uint(w.x)), uint(w.y)), uint(w.z)); }
/* Finish a hash state into a key of the given kind. */
uvec4 rb_close(uvec4 s, uint kind) {
    s = rb_round(rb_round(s));
    return uvec4(s.xyz, kind);
}
/* The key of cam_rel at world position w (x4 units). */
uvec4 rb_leaf(ivec3 w) {
    return rb_close(rb_w_in(uvec4(0x52424B31u, 0x4C454146u, RB_LEAF, 0u), w), RB_LEAF);
}
/* The key of cam_rel_lerp from wp to wc: the plain leaf when the corner
 * does not move, since the lerp then yields cam_rel's value exactly. */
uvec4 rb_leaf_lerp(ivec3 wp, ivec3 wc) {
    if (wp == wc) return rb_leaf(wc);
    return rb_close(rb_w_in(rb_w_in(uvec4(0x52424B31u, 0x4C455250u, RB_LEAF, 1u), wp), wc),
                    RB_LEAF);
}
/* Strict lexicographic order on keys. */
bool rb_less(uvec4 a, uvec4 b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    if (a.z != b.z) return a.z < b.z;
    return a.w < b.w;
}
/* The key of (a + b) * 0.5: the add commutes, so the operands are unordered. */
uvec4 rb_mean(uvec4 a, uvec4 b) {
    bool sw = rb_less(b, a);
    uvec4 s = uvec4(0x52424B31u, 0x4E4F4445u, RB_NODE, 0u);
    s = rb_key_in(rb_key_in(s, sw ? b : a), sw ? a : b);
    return rb_close(s, RB_NODE);
}
/* The key of cv_lerp_n's cut of edge a-b at zc (endpoints in canonical order). */
uvec4 rb_cut(uvec4 a, uvec4 b, float zc) {
    bool sw = rb_less(b, a);
    uvec4 s = uvec4(0x52424B31u, 0x43555420u, RB_CUT, 0u);
    s = rb_in(rb_key_in(rb_key_in(s, sw ? b : a), sw ? a : b), floatBitsToUint(zc));
    return rb_close(s, RB_CUT);
}
/* nc4_pt's key. */
uvec4 rb_nc4_pt(uvec4 a, uvec4 b, int k) {
    if (k == 0) return a;
    if (k == 4) return b;
    uvec4 m = rb_mean(a, b);
    if (k == 2) return m;
    return rb_mean(k == 1 ? a : b, m);
}
/* nc4_node's key. */
uvec4 rb_nc4_node(int r, int c, uvec4 TL, uvec4 TR, uvec4 BL, uvec4 BR) {
    return rb_nc4_pt(rb_nc4_pt(TL, BL, r), rb_nc4_pt(TR, BR, r), c);
}
/* nt4_node's key. */
uvec4 rb_nt4_node(int n, uvec4 A, uvec4 B, uvec4 C) {
    if (n == 0) return A;
    if (n == 4) return B;
    if (n == 14) return C;
    uvec4 m2 = rb_mean(A, B), m9 = rb_mean(A, C), m11 = rb_mean(B, C);
    if (n == 2) return m2;
    if (n == 9) return m9;
    if (n == 11) return m11;
    if (n == 1)  return rb_mean(A, m2);
    if (n == 3)  return rb_mean(B, m2);
    if (n == 5)  return rb_mean(A, m9);
    if (n == 12) return rb_mean(C, m9);
    if (n == 8)  return rb_mean(B, m11);
    if (n == 13) return rb_mean(C, m11);
    if (n == 6)  return rb_mean(A, m11);
    if (n == 7)  return rb_mean(B, m9);
    return rb_mean(C, m2);   /* node 10 */
}
/* hq2_node's key (NODEMAP numbering). */
uvec4 rb_hq2_node(int n, uvec4 TL, uvec4 TR, uvec4 BL, uvec4 BR) {
    if (n == 0) return TL;
    if (n == 1) return TR;
    if (n == 2) return BR;
    if (n == 3) return BL;
    uvec4 mT = rb_mean(TL, TR), mB = rb_mean(BL, BR);
    if (n == 4) return mT;
    if (n == 5) return mB;
    if (n == 6) return rb_mean(TL, BL);
    if (n == 7) return rb_mean(TR, BR);
    return rb_mean(mT, mB);
}
/* tj_pos's key over the base triangle's keys. */
uvec4 rb_tj_pos(int d, uvec4 B[3]) {
    if (d < 3) return B[d];
    int e = (d - 3) / 3;
    return rb_nc4_pt(B[e], B[(e + 1) % 3], (d - 3) % 3 + 1);
}
/* tj_base's corner keys, from the face corner keys k0..k3 (m0, m1, m2, m3c). */
void rb_tj_base(bool hqv, bool quad, bool tri2, int subq, uvec4 k0, uvec4 k1, uvec4 k2, uvec4 k3,
                out uvec4 B[3]) {
    int o = tri2 ? 1 : 0;
    if (hqv) {
        for (int j = 0; j < 3; j++) B[j] = rb_hq2_node(NODEMAP[subq][o + j], k0, k1, k3, k2);
        return;
    }
    if (!quad)     { B[0] = k0; B[1] = k1; B[2] = k2; }
    else if (tri2) { B[0] = k1; B[1] = k3; B[2] = k2; }
    else           { B[0] = k0; B[1] = k1; B[2] = k3; }
}
/* Replace the emitted position by this vertex's readback pixel: pass 0
 * stores the bits of the gl_Position just computed, pass 1 the key.
 * pc.pad1 is the element base of the draw's buffer in the target, pc.pad2
 * the cell vertices per record (bits 0-7; 0 for a vertex arena, where the
 * element is gl_VertexIndex) and the target height (bits 8-31); element e
 * owns pixels 2e and 2e + 1. The key pixel carries the sector slot in w
 * bits 8-16. */
void rb_emit(uvec4 key, uint slot) {
    uint per = uint(pc.pad2) & 0xFFu, h = uint(pc.pad2) >> 8u;
    uint elem = per != 0u ? uint(gl_InstanceIndex) * per + uint(gl_VertexIndex)
                          : uint(gl_VertexIndex);
    uint px = 2u * (uint(pc.pad1) + elem) + uint(pc.pad0 & 1);
    vMat = pc.pad0 == 0 ? floatBitsToUint(gl_Position) : uvec4(key.xyz, key.w | (slot << 8u));
    gl_Position = vec4((float(px % RB_W) + 0.5) * (2.0 / float(RB_W)) - 1.0,
                       1.0 - (float(px / RB_W) + 0.5) * (2.0 / float(h)), 0.5, 1.0);
    gl_PointSize = 1.0;
}
#endif

void kill_vert() {
#ifdef TERRAIN_READBACK
    gl_Position = vec4(-2.0, -2.0, 0.5, 1.0);   /* a point outside the view: no pixel */
    gl_PointSize = 1.0;
#else
    gl_Position = vec4(0.0, 0.0, 0.0, 1.0);   /* degenerate: no raster */
#endif
    vUV = vec2(0.0); vColor = vec4(0.0); vMat = uvec4(0u);
    vKoff = 0.0; vKcls = OP_KEY_CLASS_NONE; vQ = 1.0;
    vKfl = vec2(0.0); vKc0 = uvec4(0u); vKc1 = uvec4(0u);
    vKd = vec3(0.0); vKn0 = vec4(0.0); vKn1 = vec4(0.0); vKn2 = vec4(0.0);
    vRim = 0u;
}

/* ---- portal inner-rim cull (portal_rim_classify mirror) --------------------
 * The CPU feed classifies every emitted tri against the active portal
 * opening planes and DROPS accepted rim faces - they lie in the opening
 * plane, coincident with the destination-sky preview, and no depth ordering
 * can separate them. Same classifier here, in fp32 `precise`, on the tri
 * rebuilt from the corner attributes (flags16 bit13 = second half/sub-tri).
 * rim_uv_contains / rim_uv_edge_dist run on the plane's outline dropped
 * into its in-plane (U,W) basis, exactly as the CPU helpers. */
bool rim_uv_contains_g(int base, int pn, float u, float v) {
    bool inp = false;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        float iu = g.rim_pv[base + i].x, iv = g.rim_pv[base + i].y;
        float ju = g.rim_pv[base + j].x, jv = g.rim_pv[base + j].y;
        if ((iv > v) != (jv > v) &&
            u < (ju - iu) * (v - iv) / (jv - iv) + iu)
            inp = !inp;
    }
    return inp;
}
float rim_uv_edge_dist_g(int base, int pn, float u, float v) {
    float best = 1e30;
    for (int i = 0, j = pn - 1; i < pn; j = i++) {
        float ax = g.rim_pv[base + j].x, ay = g.rim_pv[base + j].y;
        precise float ex = g.rim_pv[base + i].x - ax, ey = g.rim_pv[base + i].y - ay;
        precise float L2 = ex * ex + ey * ey;
        precise float t = (L2 > 1e-9) ? ((u - ax) * ex + (v - ay) * ey) / L2 : 0.0;
        t = dsel(t < 0.0, 0.0, dsel(t > 1.0, 1.0, t));
        precise float dx = u - (ax + t * ex), dy = v - (ay + t * ey);
        precise float d = sqrt(dx * dx + dy * dy);
        best = dsel(d < best, d, best);
    }
    return best;
}
/* The inner-rim verdict of a tri (classify flags == 0 under some plane):
 * 0 = not rim; 1 = rim under a plane the CPU feed drops it for, so the
 * retained draw drops it too; 2 + pi = rim under plane pi, which discards its
 * rim per fragment (rim_O[pi].w), so the face is kept and terrain.frag
 * discards the part inside plane pi's outline (vRim = 1 + pi; the rim and
 * cover plane lists share the feed's plane order). Inputs are the emitted
 * tri's FLOAT screen coords (the feed ttri's sx/sy) and walk view-Z (m3). */
int rim_classified(vec2 s0, vec2 s1, vec2 s2, float z0, float z1, float z2) {
    precise float ofx = g.rim_oxyH.x, ofy = g.rim_oxyH.y, H = g.rim_oxyH.z;
    if (H < 1.0) return 0;
    float eps = g.rim_knobs.x, nearw = g.rim_knobs.y;
    float reach = g.rim_knobs.z, cosw = g.rim_knobs.w;
    float sx[3] = float[3](float(s0.x), float(s1.x), float(s2.x));
    float sy[3] = float[3](float(s0.y), float(s1.y), float(s2.y));
    float vz[3] = float[3](z0 * 16.0, z1 * 16.0, z2 * 16.0);
    for (int pi = 0; pi < g.rim_cnt.x; pi++) {
        if (g.rim_abc[pi].w == 0.0) continue;      /* frame invalid */
        precise float nx = g.rim_nrm[pi].x, ny = g.rim_nrm[pi].y,
                       nz = g.rim_nrm[pi].z;
        precise vec3 V[3];
        float dmin = 1e30, dmax = 0.0;
        bool okz = true;
        for (int k = 0; k < 3; k++) {
            precise float inv = g.rim_abc[pi].x * sx[k]
                               + g.rim_abc[pi].y * sy[k] + g.rim_abc[pi].z;
            if (inv <= 1e-9) { okz = false; break; }
            precise float vzk = vz[k];
            V[k] = vec3((sx[k] - ofx) * vzk / H, (sy[k] - ofy) * vzk / H, vzk);
            precise float dz = vzk - 1.0 / inv;
            precise float d = abs(dz * (nx * (sx[k] - ofx) + ny * (sy[k] - ofy)
                                         + nz * H) / H);
            dmin = dsel(d < dmin, d, dmin);
            dmax = dsel(d > dmax, d, dmax);
        }
        if (!okz) continue;
        if (dmin > 400.0) continue;                /* RIM_CAND_WINDOW */
        int base = pi * 16, pn = int(g.rim_nrm[pi].w);
        precise vec3 O = g.rim_O[pi].xyz, U = g.rim_U[pi].xyz,
                      W = g.rim_W[pi].xyz;
        precise float fu[3], fv[3];
        float fulo = 1e30, fuhi = -1e30, fvlo = 1e30, fvhi = -1e30;
        for (int k = 0; k < 3; k++) {
            precise vec3 r = V[k] - O;
            fu[k] = r.x * U.x + r.y * U.y + r.z * U.z;
            fv[k] = r.x * W.x + r.y * W.y + r.z * W.z;
            fulo = dsel(fu[k] < fulo, fu[k], fulo);
            fuhi = dsel(fu[k] > fuhi, fu[k], fuhi);
            fvlo = dsel(fv[k] < fvlo, fv[k], fvlo);
            fvhi = dsel(fv[k] > fvhi, fv[k], fvhi);
        }
        if (fuhi < g.rim_bbox[pi].x - 600.0 ||     /* RIM_CAND_MARGIN */
            fulo > g.rim_bbox[pi].y + 600.0 ||
            fvhi < g.rim_bbox[pi].z - 600.0 ||
            fvlo > g.rim_bbox[pi].w + 600.0) continue;
        /* contain: any vertex or the centroid inside the outline, or a
         * vertex within `reach` of an outline edge */
        bool touches = false;
        precise float cu = 0.0, cv = 0.0;
        for (int k = 0; k < 3; k++) {
            cu += fu[k]; cv += fv[k];
            if (!touches && rim_uv_contains_g(base, pn, fu[k], fv[k]))
                touches = true;
        }
        if (!touches && rim_uv_contains_g(base, pn, cu / 3.0, cv / 3.0))
            touches = true;
        if (!touches && reach > 0.0)
            for (int k = 0; k < 3 && !touches; k++)
                if (rim_uv_edge_dist_g(base, pn, fu[k], fv[k]) <= reach)
                    touches = true;
        if (!touches) continue;                      /* RIMFAIL_CONTAIN */
        if (dmax > eps || dmin > nearw) continue;    /* RIMFAIL_DIST */
        precise vec3 e1 = V[1] - V[0], e2 = V[2] - V[0];
        precise float fx = e1.y * e2.z - e1.z * e2.y;
        precise float fy = e1.z * e2.x - e1.x * e2.z;
        precise float fz = e1.x * e2.y - e1.y * e2.x;
        precise float fl = sqrt(fx * fx + fy * fy + fz * fz);
        if (fl < 1e-9) continue;                   /* RIMFAIL_ANGLE */
        precise float fcos = abs((fx * nx + fy * ny + fz * nz) / fl);
        if (fcos < cosw) continue;
        return g.rim_O[pi].w != 0.0 ? 2 + pi : 1;  /* all terms pass */
    }
    return 0;
}

/* The residual of a colour level v (bytes plus residual / 256) from its byte
 * c, in 1/256 of a level rounded half up and clamped to [-128, 127]
 * (native_gte.c ngte_rgb_mix). */
ivec3 rgb8_residual(vec3 v, vec3 c) {
    return ivec3(clamp(floor((v - c) * 256.0 + 0.5), vec3(-128.0), vec3(127.0)));
}

#ifdef TERRAIN_NEAR
/* ---- near-plane clip fan (clip_push_tri / cv_lerp mirror) -----------------
 * A near-variant sector's faces may reach or cross the eye plane, so the walk
 * Sutherland-Hodgman-clips every emitted triangle against m3 >= zc before
 * projecting it: 0 crossings -> 1 triangle, 1 -> a quad drawn as 2, 2 -> 1,
 * 3 -> dropped. The bake emits SIX vertices per source triangle (a primary
 * and a spare fan triangle, flags16 bit 15), each of which re-runs the whole
 * clip here and becomes one corner of its fan triangle; the spare dies
 * whenever the clip produced fewer than 4 output vertices.
 *
 * A clip corner carries the position, colour and UV the walk resolved BEFORE
 * the clip - fogging and the LP grade run per parent corner, and the cut
 * interpolates their OUTPUT, never re-fogs at the cut position. */
/* os/ov mirror ngte_cvert.osx/osy/oflag: the screen-space override the HQ
 * subdivide installs on its own nodes under the integer snap. A cut vertex
 * rebuilt by the clip carries none (ngte_clip_lerp zeroes oflag) and falls
 * back to the floor of its own projection, as push_tri does. */
#if defined(TERRAIN_READBACK)
/* rk: the corner's readback key. */
struct cvx { vec3 m; ivec3 c; ivec3 q; vec2 uv; vec2 os; bool ov; vec2 fl; uvec4 rk; };
#else
struct cvx { vec3 m; ivec3 c; ivec3 q; vec2 uv; vec2 os; bool ov; vec2 fl; };
#endif

/* The cut of edge a-b at the plane m3 = zc (cv_lerp mirror). The clip
 * takes a cut only when the endpoints straddle zc, so exactly one is
 * behind it (m3 < zc). The stage orders the operands behind endpoint
 * first: two faces sharing a crossing edge traverse it in either direction,
 * and t from a then (1 - t) from b is a different rounding sequence, so the
 * shared cut position is bit-identical only with one canonical order. */
cvx cv_lerp_n(cvx a, cvx b, float zc) {
    if (a.m.z >= zc) { cvx sw = a; a = b; b = sw; }
#if defined(TERRAIN_SECDELTA) && defined(TERRAIN_READBACK)
    /* The readback's control reverses the order on the triangles ctl_rev
     * marks, so two triangles of one sector that share a crossing edge cut
     * it from opposite ends and can round apart, which the per-sector camera
     * operand cannot make them do. */
    if (ctl_rev) { cvx sw = a; a = b; b = sw; }
#endif
    precise float t = (zc - a.m.z) / (b.m.z - a.m.z);
    t = dsel(t < 0.0, 0.0, dsel(t > 1.0, 1.0, t));
    precise float px = a.m.x + t * (b.m.x - a.m.x);
    precise float py = a.m.y + t * (b.m.y - a.m.y);
    precise float  pu = a.uv.x + float(t) * (b.uv.x - a.uv.x);
    precise float  pv = a.uv.y + float(t) * (b.uv.y - a.uv.y);
    precise float  fx = a.fl.x + float(t) * (b.fl.x - a.fl.x);
    precise float  fy = a.fl.y + float(t) * (b.fl.y - a.fl.y);
    precise vec3 la = vec3(a.c) + vec3(a.q) / 256.0;
    precise vec3 lb = vec3(b.c) + vec3(b.q) / 256.0;
    precise vec3 lv = la + t * (lb - la);
    vec3 cv = clamp(floor(lv + 0.5), vec3(0.0), vec3(255.0));
    cvx r;
    r.m  = vec3(px, py, zc);
    r.uv = vec2(pu, pv);
    r.c  = ivec3(cv);
    r.q  = rgb8_residual(lv, cv);
    r.os = vec2(0.0); r.ov = false;
    r.fl = vec2(fx, fy);
#ifdef TERRAIN_READBACK
    r.rk = rb_cut(a.rk, b.rk, zc);
#endif
    return r;
}

/* push_tri's screen resolution for one clip-fan corner: the installed
 * override when the subdivide left one, otherwise the (snapped) projection. */
vec2 scr_of(cvx v) { return v.ov ? v.os : snap_of(proj_of(v.m)); }
#endif

#if defined(TERRAIN_CELLS) || defined(TERRAIN_NEAR)
/* tgpu_rgb_mid mirror: the walk's per-channel (a + b) >> 1 on packed RGB. */
uint cell_rgb_mid(uint a, uint b) {
    return (((a & 0xFFu) + (b & 0xFFu)) >> 1)
         | ((((a >> 8) & 0xFFu) + ((b >> 8) & 0xFFu)) >> 1) << 8
         | ((((a >> 16) & 0xFFu) + ((b >> 16) & 0xFFu)) >> 1) << 16;
}
#endif

/* The previous-tick store (openpete_terrain_cache.h OPTC_PREV_REC_*,
 * OPTC_FACE_F_*), in units of six words: previous-corner records of
 * poly-anim near faces, per-sector colour tables of env-anim colour
 * sectors, and the split records of split plain and anim vertices. A face's flags ride a near vertex's pz, a cell record's fog_b
 * and an anim vertex's prev pad. PREV_REC_NONE names no run. */
#define PREV_REC_NONE  0xFFFFFFFFu
#define FACE_F_PREV    1u
#define FACE_F_COLTAB  2u
#define PAD_COLTAB     0x4000u
#define COLTAB_BAKED   0xFFu
/* Unpack a baked R8G8B8A8 colour word the way the vertex-input UNORM
 * conversion unpacks the same bytes at locations 2/3. */
vec3 unrgb(uint u) {
    return vec3(float(u & 0xFFu), float((u >> 8) & 0xFFu),
                float((u >> 16) & 0xFFu)) / 255.0;
}
layout(std430, set = 2, binding = 1) readonly buffer TerrainPrevRecs {
    uint w[];
} prevRec;
/* Env-anim colour lerp on a packed word (terrain.c canim_lerp mirror, the
 * anim stage's canim_col on integers): each channel at the build's colour
 * alpha of the interval prev -> cur, rounded to nearest. Alpha 1 selects
 * cur. */
uint coltab_lerp(uint prev, uint cur)
{
    float a = G_CA;
    if (a == 1.0) return cur;
    ivec3 c = ivec3(int(cur & 0xFFu), int((cur >> 8) & 0xFFu), int((cur >> 16) & 0xFFu));
    ivec3 p = ivec3(int(prev & 0xFFu), int((prev >> 8) & 0xFFu), int((prev >> 16) & 0xFFu));
    precise vec3 v = vec3(p) + vec3(c - p) * a + 0.5;
    ivec3 o = ivec3(v);
    return uint(o.x) | (uint(o.y) << 8) | (uint(o.z) << 16);
}
/* Colour idx of array arr (0 LP, 1 HP near, 2 HP far) from the colour-table
 * entry sector slot's state names (gp.w), at the colour alpha: the header
 * word (LP count | HP count << 16), then the current LP, near and far
 * arrays, then the previous three - the word the walk reads from the
 * sector's lerped colour copy. */
uint coltab_col(uint slot, int arr, uint idx)
{
    uint b = floatBitsToUint(s.sec[slot].gp.w) * 6u;
    uint h = prevRec.w[b];
    uint nLp = h & 0xFFFFu, nHp = h >> 16;
    uint off = arr == 0 ? 0u : (arr == 1 ? nLp : nLp + nHp);
    uint at = b + 1u + off + idx;
    return coltab_lerp(prevRec.w[at + nLp + 2u * nHp], prevRec.w[at]);
}

#if defined(TERRAIN_CELLS) || defined(TERRAIN_RINGV)
#define FACE_F_RING    4u
#define PAD_RING       0x2000u
/* Set by ring_corners: this vertex's face reads the position ring, and
 * ringP0..ringP3 hold its previous corners; ringBad: the sector state names
 * no ring entry, so the vertex is killed. */
bool  ringOn = false;
bool  ringBad = false;
ivec3 ringP0, ringP1, ringP2, ringP3;
/* The two sign-extended int16 halves of a ring word, low first. */
ivec2 ring_s16x2(uint w) {
    return ivec2(bitfieldExtract(int(w), 0, 16), bitfieldExtract(int(w), 16, 16));
}
/* The ring entry word base the sector state names (gq.w, in units). */
uint ring_base(uint slot) {
    return floatBitsToUint(s.sec[slot].gq.w) * 6u;
}
/* Bank vertex idx's current and previous positions (x4 fixed) from the
 * ring entry at word b; lp selects the LP bank, else the HP bank after it
 * (openpete_terrain_cache.h, position rings). */
void ring_pos(uint b, bool lp, uint idx, out ivec3 cur, out ivec3 prev) {
    uint off = lp ? 0u : (prevRec.w[b] & 0xFFFFu);
    uint at = b + 1u + 3u * (off + idx);
    ivec2 c = ring_s16x2(prevRec.w[at]);
    ivec2 p = ring_s16x2(prevRec.w[at + 1u]);
    ivec2 z = ring_s16x2(prevRec.w[at + 2u]);
    cur = ivec3(c, z.x);
    prev = ivec3(p, z.y);
}
/* A ring face's corners from the bank indices in idx (i0..i3): the current
 * ones into inC0..inC2 in the chunk layout, the previous into ringP0..3. */
void ring_corners(uint slot, bool lp, ivec4 idx) {
    if (floatBitsToUint(s.sec[slot].gq.w) == 0xFFFFFFFFu) { ringBad = true; return; }
    uint b = ring_base(slot);
    ivec3 c0, c1, c2, c3;
    ring_pos(b, lp, uint(idx.x) & 0xFFFFu, c0, ringP0);
    ring_pos(b, lp, uint(idx.y) & 0xFFFFu, c1, ringP1);
    ring_pos(b, lp, uint(idx.z) & 0xFFFFu, c2, ringP2);
    ring_pos(b, lp, uint(idx.w) & 0xFFFFu, c3, ringP3);
    inC0 = ivec4(c0, c1.x);
    inC1 = ivec4(c1.yz, c2.xy);
    inC2 = ivec4(c2.z, c3);
    ringOn = true;
}
#endif

/* ---- marked-edge split (terrain.c tj_split_topo, tgpu_emit_face_split) ----
 * A split vertex (flags16 bit 14) is one member of a sub-triangle of its
 * base triangle, the emitted triangle its flags name: a flat face's half or
 * an HQ sub-quad's half. pz bits 4-15 hold the sub-triangle's three member
 * descriptors, 4 bits each: base corner 0-2, or the node at quarter
 * k = 1..3 of base edge e, from corner e toward corner (e + 1) % 3
 * (3 + 3e + k - 1). A node sits at nc4_pt of the edge's transformed
 * corners, the operand the subdivide builds its own nodes from, so a flat
 * face and its subdivided neighbour place the shared node identically. Its
 * attributes mix the base corners' at the weight of tj_ends. */
#define TJ_SPLIT 0x4000u

/* Member m's descriptor. */
int tj_desc(int m) {
    return (int(uint(inPos.z) & 0xFFFFu) >> (4 + 4 * m)) & 15;
}

/* The weight of the edge's second corner at quarter k: k / 4 under
 * perspective-correct interpolation, the node's screen parameter
 * s * zb / ((1 - s) * za + s * zb) under AFFINE_UV (terrain.c tj_lerp_t). */
float tj_t(int k, float za, float zb) {
    float s = float(k) * 0.25;
    if (!AFFINE_UV || za <= 0.0 || zb <= 0.0) return s;
    return (s * zb) / ((1.0 - s) * za + s * zb);
}

/* Two colours on the 8-bit grid mixed at weight t and rounded half up. The
 * walk's split node (terrain.c tj_member_cv, ngte_rgb_mix) adds each corner's
 * residual to its byte before the mix and runs in double; these mix the
 * corners' bytes alone in float, so the two agree where both corners are
 * whole bytes. */
vec3 tj_mix8(vec3 a, vec3 b, float t) {
    return floor(mix(round(a * 255.0), round(b * 255.0), t) + 0.5) / 255.0;
}

/* The residual of tj_mix8's byte from the unrounded mix of two colours on the
 * 8-bit grid, in 1/256 of a level, rounded half up and clamped to
 * [-128, 127] as ngte_rgb_mix's is; the corners' own residuals are not
 * inputs (tj_mix8). */
ivec3 tj_rgbq(vec3 a, vec3 b, float t) {
    vec3 v = mix(round(a * 255.0), round(b * 255.0), t);
    return rgb8_residual(v, floor(v + 0.5));
}

/* Member d's camera-space position. */
precise vec3 tj_pos(int d, vec3 B[3]) {
    if (d < 3) return B[d];
    int e = (d - 3) / 3;
    return nc4_pt(B[e], B[(e + 1) % 3], (d - 3) % 3 + 1);
}

/* The screen parameter on the segment A-C of the point nearest m, clamped to
 * [0, 1]; 0.5 on a zero-length segment (terrain.c tj_snap_node). */
float tj_seg_s(vec2 A, vec2 C, vec2 m) {
    precise float dx = C.x - A.x, dy = C.y - A.y;
    precise float L2 = dx * dx + dy * dy;
    precise float num = (m.x - A.x) * dx + (m.y - A.y) * dy;
    float tt = L2 > 0.0 ? num / L2 : 0.5;
    return clamp(tt, 0.0, 1.0);
}

/* The weight of the edge's second corner at screen parameter s, the value
 * the raster gives the unsplit edge there: s under AFFINE_UV, the
 * perspective-correct s * za / ((1 - s) * zb + s * za) otherwise (terrain.c
 * tj_snap_node). */
float tj_seg_t(float s, float za, float zb) {
    if (AFFINE_UV) return float(s);
    float ps = float(s);
    return (ps * za) / ((1.0 - ps) * zb + ps * za);
}

/* Member d's emitted screen point: a corner's own; a node's projection,
 * placed under the integer snap on the segment between its edge's emitted
 * corner points (terrain.c tj_snap_node), unless an edge corner is inside
 * the near plane. */
vec2 tj_scr(int d, vec3 B[3], vec2 S[3]) {
    if (d < 3) return S[d];
    vec3 m = tj_pos(d, B);
    if (pc.snap == 0) return proj_of(m);
    int p = (d - 3) / 3, q = (p + 1) % 3;
    if (B[p].z < G_NEAR || B[q].z < G_NEAR) return floor(proj_of(m));
    vec2 A = S[p], C = S[q];
    float tt = tj_seg_s(A, C, proj_of(m));
    precise float dx = C.x - A.x, dy = C.y - A.y;
    precise float rx = A.x + tt * dx, ry = A.y + tt * dy;
    return vec2(rx, ry);
}

/* Member d's two base corners and the second's weight: a corner is its own
 * pair at weight 0; a node placed on its edge's snapped segment (tj_scr)
 * takes the weight of that placement (tj_seg_t), any other node tj_t's. */
void tj_ends(int d, vec3 B[3], vec2 S[3], out int p, out int q, out float t) {
    if (d < 3) { p = d; q = d; t = 0.0; return; }
    int e = (d - 3) / 3, k = (d - 3) % 3 + 1;
    p = e; q = (e + 1) % 3;
    if (pc.snap != 0 && B[p].z >= G_NEAR && B[q].z >= G_NEAR)
        t = tj_seg_t(tj_seg_s(S[p], S[q], proj_of(tj_pos(d, B))), B[p].z, B[q].z);
    else
        t = tj_t(k, B[p].z, B[q].z);
}

/* HQ 2x2 node n (NODEMAP numbering) from the transformed parent corners,
 * the own-vertex rebuild's expressions. */
precise vec3 hq2_node(int n, vec3 TL, vec3 TR, vec3 BL, vec3 BR) {
    precise vec3 mT = (TL + TR) * 0.5, mB = (BL + BR) * 0.5;
    precise vec3 mL = (TL + BL) * 0.5, mR = (TR + BR) * 0.5;
    precise vec3 C  = (mT + mB) * 0.5;
    return n == 0 ? TL : n == 1 ? TR : n == 2 ? BR : n == 3 ? BL
         : n == 4 ? mT : n == 5 ? mB : n == 6 ? mL : n == 7 ? mR : C;
}

/* A split vertex's base triangle: corner positions B, emitted screen
 * points S, drain face-local coordinates F (psx_key_contest.glsl) and
 * split-record corner slots R. A
 * flat face's halves are (i0, i1, i3) and (i1, i3, i2), a triangle
 * (i0, i1, i2), at record slots i0..i3; an HQ sub-quad's halves are
 * (TL, TR, BL) and (TR, BL, BR) of its NODEMAP nodes, at record slots
 * TL, TR, BL, BR. */
void tj_base(bool hqv, bool quad, bool tri2, int subq, vec3 m0, vec3 m1, vec3 m2, vec3 m3c,
             out vec3 B[3], out vec2 S[3], out vec2 F[3], out int R[3]) {
    int o = tri2 ? 1 : 0;
    for (int j = 0; j < 3; j++) F[j] = vec2(0.0);
    if (hqv) {
        vec3 Q[4];
        for (int j = 0; j < 4; j++) Q[j] = hq2_node(NODEMAP[subq][j], m0, m1, m3c, m2);
        for (int j = 0; j < 3; j++) {
            B[j] = Q[o + j];
            S[j] = hq_snap_screen(NODEMAP[subq][o + j], m0, m1, m3c, m2, B[j]);
            F[j] = quad_side_fl(Q[0], Q[1], Q[2], Q[3], B[j]);
            R[j] = o + j;
        }
        return;
    }
    if (!quad)     { B[0] = m0; B[1] = m1; B[2] = m2;  R[0] = 0; R[1] = 1; R[2] = 2;
                     F[1] = vec2(4.0, 0.0); F[2] = vec2(0.0, 4.0); }
    else if (tri2) { B[0] = m1; B[1] = m3c; B[2] = m2; R[0] = 1; R[1] = 3; R[2] = 2; }
    else           { B[0] = m0; B[1] = m1; B[2] = m3c; R[0] = 0; R[1] = 1; R[2] = 3; }
    for (int j = 0; j < 3; j++) S[j] = snap_of(proj_of(B[j]));
}


#if !defined(TERRAIN_NEAR) && !defined(TERRAIN_CELLS)
/* This split vertex's record, in words. */
uint tj_rec() {
    return ((uint(inPos.x) & 0xFFFFu) | ((uint(inPos.y) & 0xFFFFu) << 16)) * 6u;
}

/* Record corner c's raw far and near colours: a plain record's words; an
 * anim record's lerped at the colour alpha (canim_col), or the sector
 * colour table's (coltab_col) when its flags say so. */
void tj_rec_col(uint r, int c, bool lp, uint slot, out vec3 cf, out vec3 cn) {
#ifdef TERRAIN_ANIM
    if ((prevRec.w[r + 21u] & 1u) != 0u) {
        uint idx = (prevRec.w[r + 20u] >> uint(8 * c)) & 0xFFu;
        cf = unrgb(coltab_col(slot, lp ? 0 : 2, idx));
        cn = unrgb(coltab_col(slot, lp ? 0 : 1, idx));
        return;
    }
    cf = canim_col(prevRec.w[r + 12u + uint(c)], unrgb(prevRec.w[r + uint(c)]));
    cn = canim_col(prevRec.w[r + 16u + uint(c)], unrgb(prevRec.w[r + 4u + uint(c)]));
#else
    cf = unrgb(prevRec.w[r + uint(c)]);
    cn = unrgb(prevRec.w[r + 4u + uint(c)]);
#endif
}

/* Record corner c's UV: the far half's, or the near half's (near). */
vec2 tj_rec_uv(uint r, int c, bool near) {
    uint w = prevRec.w[r + 8u + uint(c)];
    return near ? vec2(float((w >> 16) & 0xFFu), float(w >> 24))
                : vec2(float(w & 0xFFu), float((w >> 8) & 0xFFu));
}
#endif

#ifdef TERRAIN_NEAR
/* The two sign-extended int16 halves of a record word, low first. */
ivec2 prev_s16x2(uint w) {
    return ivec2(bitfieldExtract(int(w), 0, 16), bitfieldExtract(int(w), 16, 16));
}
/* This vertex's face record: a cell record's fog_a word, a near vertex's
 * px | py << 16. */
uint near_prev_rec() {
#ifdef TERRAIN_CELLS
    return cellHqFog.x;
#else
    return (uint(inPos.x) & 0xFFFFu) | ((uint(inPos.y) & 0xFFFFu) << 16);
#endif
}
/* This vertex's face flags (FACE_F_*): a cell record's fog_b, a near
 * vertex's pz. */
uint near_face_flags() {
#ifdef TERRAIN_CELLS
    return cellHqFog.y;
#else
    return uint(inPos.z) & 0xFFFFu;
#endif
}
/* Record r's four previous corners (x4 fixed, sector-local). */
void near_prev_corners(uint r, out ivec3 p0, out ivec3 p1, out ivec3 p2, out ivec3 p3) {
    uint b = r * 6u;
    ivec4 A = ivec4(prev_s16x2(prevRec.w[b]),      prev_s16x2(prevRec.w[b + 1u]));
    ivec4 B = ivec4(prev_s16x2(prevRec.w[b + 2u]), prev_s16x2(prevRec.w[b + 3u]));
    ivec4 C = ivec4(prev_s16x2(prevRec.w[b + 4u]), prev_s16x2(prevRec.w[b + 5u]));
    p0 = A.xyz;
    p1 = ivec3(A.w, B.x, B.y);
    p2 = ivec3(B.z, B.w, C.x);
    p3 = C.yzw;
}
/* This vertex's face's previous corners, true when they differ from its
 * current ones: a ring face's from the position ring, any other face's from
 * its previous-corner record, which the bake writes only for a face whose
 * corners moved. False leaves the stage on the current corners. */
bool near_prev_get(out ivec3 p0, out ivec3 p1, out ivec3 p2, out ivec3 p3) {
    if (ringOn) {
        p0 = ringP0; p1 = ringP1; p2 = ringP2; p3 = ringP3;
        return p0 != inC0.xyz || p1 != ivec3(inC0.w, inC1.x, inC1.y)
            || p2 != ivec3(inC1.z, inC1.w, inC2.x) || p3 != inC2.yzw;
    }
    p0 = p1 = p2 = p3 = ivec3(0);
    const uint r = near_prev_rec();
    if (r == PREV_REC_NONE || (near_face_flags() & FACE_F_PREV) == 0u) return false;
    near_prev_corners(r, p0, p1, p2, p3);
    return true;
}
#ifndef TERRAIN_CELLS
/* Node n of a face's 2x2 subdivide (NODEMAP numbering) coloured from its
 * four corner colours q (i0 = TL, i1 = TR, i2 = BR, i3 = BL) in the bake's
 * and the walk's midpoint order (tgpu_bake_hq2, terrain_near_emit): the
 * centre is the mean of the top and bottom midpoints. */
uint hq2_node_rgb(int n, uvec4 q) {
    if (n < 4) return q[n];
    uint mT = cell_rgb_mid(q.x, q.y), mB = cell_rgb_mid(q.w, q.z);
    if (n == 4) return mT;
    if (n == 5) return mB;
    if (n == 6) return cell_rgb_mid(q.x, q.w);
    if (n == 7) return cell_rgb_mid(q.y, q.z);
    return cell_rgb_mid(mT, mB);
}
/* Emitted-triangle member k's node (NODEMAP numbering): a 2x2 sub-quad's
 * NODEMAP entry, else the parent corner the bake emitted there - a
 * triangle (i0, i1, i2), an LP quad (i0, i1, i2) + (i1, i2, i3), an HP quad
 * (i0, i1, i3) + (i1, i3, i2). */
int near_member_node(int k, bool hqv, bool quad, bool lp, bool tri2n, int subq) {
    int base = tri2n ? 1 : 0;
    if (hqv) return NODEMAP[subq][base + k];
    if (!quad || lp) return base * int(quad) + k;
    const int HPQ[2][3] = int[2][3](int[3](0, 1, 3), int[3](1, 3, 2));
    return HPQ[base][k];
}
#endif
#endif

#ifdef TERRAIN_RINGV
/* Copies the chunk's position and corner inputs into the names the stage
 * reads; a ring face (OPTC_PREV_PAD_RING in an anim vertex's pad,
 * FACE_F_RING in a near vertex's face flags) takes its corners from the
 * position ring instead, and an anim vertex that is neither an HQ node nor
 * a split vertex takes its own position from it too. */
void vert_fetch() {
    inPos = inPosA;
    inC0 = inC0A; inC1 = inC1A; inC2 = inC2A;
#ifdef TERRAIN_ANIM
    inPrevPos = inPrevPosA;
    inPrevC0 = inPrevC0A; inPrevC1 = inPrevC1A; inPrevC2 = inPrevC2A;
    bool ring = (uint(inPrevPosA.w) & PAD_RING) != 0u;
#else
    bool ring = (uint(inPosA.z) & FACE_F_RING) != 0u;
#endif
    if (!ring) return;
    uint slot = inFaceFlags & FF_SLOT_MASK;
    bool lp = (inFaceFlags & FF_TIER_LP) != 0u;
    ring_corners(slot, lp, inC0A);
    if (ringBad) return;
#ifdef TERRAIN_ANIM
    inPrevC0 = ivec4(ringP0, ringP1.x);
    inPrevC1 = ivec4(ringP1.yz, ringP2.xy);
    inPrevC2 = ivec4(ringP2.z, ringP3);
    uint f16 = uint(inPosA.w) & 0xFFFFu;
    if ((f16 & 1u) == 0u && (f16 & 0x4000u) == 0u) {
        ivec3 c, p;
        ring_pos(ring_base(slot), lp, uint(inPosA.x) & 0xFFFFu, c, p);
        inPos.xyz = c;
        inPrevPos.xyz = p;
    }
#endif
}
#endif

#ifdef TERRAIN_CELLS
/* The two sign-extended int16 halves of a packed corner word, low first. */
ivec2 cell_s16x2(uint w) {
    return ivec2(bitfieldExtract(int(w), 0, 16), bitfieldExtract(int(w), 16, 16));
}
/* Unpack the record's corner words into inC0..inC2 (rhi_terrain_vertex's
 * c0x..c3z order, four to a vector), the face flags and info, the record
 * words, and the raw colours into cellNear and cellFar: from the sector's
 * colour table at the colour alpha for a colour-table face. */
void cell_unpack() {
    inC0 = ivec4(cell_s16x2(inCellCA.x), cell_s16x2(inCellCA.y));
    inC1 = ivec4(cell_s16x2(inCellCA.z), cell_s16x2(inCellCA.w));
    inC2 = ivec4(cell_s16x2(inCellCB.x), cell_s16x2(inCellCB.y));
    inCellFace = inCellCB.zw;
    cellHqFog = inCellFog;
    cellNear = inCellNear;
    cellFar = inCellFar;
    if ((cellHqFog.y & FACE_F_COLTAB) != 0u) {
        /* A colour-table face: near_rgb[0] holds its four corners' indices. */
        uint sl = inCellCB.z & FF_SLOT_MASK;
        for (int q = 0; q < 4; q++) {
            uint idx = (inCellNear.x >> uint(8 * q)) & 0xFFu;
            if (idx == COLTAB_BAKED) continue;   /* slot 3 keeps its baked words */
            cellNear[q] = coltab_col(sl, 1, idx);
            cellFar[q]  = coltab_col(sl, 2, idx);
        }
    }
#ifdef TERRAIN_ANIM
    inPrevC0 = ivec4(cell_s16x2(inCellPA.x), cell_s16x2(inCellPA.y));
    inPrevC1 = ivec4(cell_s16x2(inCellPA.z), cell_s16x2(inCellPA.w));
    inPrevC2 = ivec4(cell_s16x2(inCellPB.x), cell_s16x2(inCellPB.y));
#endif
    /* A ring face's record names its corners' bank indices. */
    if ((cellHqFog.y & FACE_F_RING) != 0u) {
        ring_corners(inCellCB.z & FF_SLOT_MASK, false, inC0);
#ifdef TERRAIN_ANIM
        inPrevC0 = ivec4(ringP0, ringP1.x);
        inPrevC1 = ivec4(ringP1.yz, ringP2.xy);
        inPrevC2 = ivec4(ringP2.z, ringP3);
#endif
    }
}
/* tgpu_rgb_pt mirror: node k (0..4) of the 5-node edge a..b in the drain's
 * halving order (midpoint first, then the quarter points off it). */
uint cell_rgb_pt(uint a, uint b, int k) {
    if (k == 0) return a;
    if (k == 4) return b;
    uint m = cell_rgb_mid(a, b);
    return k == 2 ? m : cell_rgb_mid(k == 1 ? a : b, m);
}
/* Node (r, c) colour of the 5x5 grid from four raw corner colours
 * (i0 = TL, i1 = TR, i2 = BR, i3 = BL): rows first, then along the row. */
uint cell_node_rgb(int r, int c, uvec4 q) {
    uint L = cell_rgb_pt(q.x, q.w, r);
    uint R = cell_rgb_pt(q.y, q.z, r);
    return cell_rgb_pt(L, R, c);
}
/* nt4_rgb_mid mirror: the triangle drain's colour average, both words
 * masked with 0xFFFEFEFF, added, shifted right once. */
uint nt4_rgb_mid(uint a, uint b) {
    return ((a & 0xFEFEFFu) + (b & 0xFEFEFFu)) >> 1;
}
/* The midpoint of two node colours: the drain's packed average
 * (nt4_rgb_mid) for the near words it draws, else the per-channel floor
 * mean (cell_rgb_mid), as terrain.c nt4_grid builds its nodes. */
uint nt4_mid(uint a, uint b, bool drain) {
    return drain ? nt4_rgb_mid(a, b) : cell_rgb_mid(a, b);
}
/* Node n's colour of a tier-4 triangle record from corner colours q: A =
 * q.x, B = q.y, C = q.w (the bake writes the layout's triangle C colour slot
 * into slot 3), nodes in nt4_node's parent order; drain selects the drain's
 * average (near words) over the floor mean (far words). */
uint nt4_node_rgb(int n, uvec4 q, bool drain) {
    uint A = q.x, B = q.y, C = q.w;
    if (n == 0) return A;
    if (n == 4) return B;
    if (n == 14) return C;
    uint m2 = nt4_mid(A, B, drain), m9 = nt4_mid(A, C, drain), m11 = nt4_mid(B, C, drain);
    if (n == 2) return m2;
    if (n == 9) return m9;
    if (n == 11) return m11;
    if (n == 1)  return nt4_mid(A, m2, drain);
    if (n == 3)  return nt4_mid(B, m2, drain);
    if (n == 5)  return nt4_mid(A, m9, drain);
    if (n == 12) return nt4_mid(C, m9, drain);
    if (n == 8)  return nt4_mid(B, m11, drain);
    if (n == 13) return nt4_mid(C, m11, drain);
    if (n == 6)  return nt4_mid(A, m11, drain);
    if (n == 7)  return nt4_mid(B, m9, drain);
    return nt4_mid(C, m2, drain);                  /* node 10 */
}
vec4 cell_unorm(uint rgb) {
    return vec4(float(rgb & 0xFFu), float((rgb >> 8) & 0xFFu),
                float((rgb >> 16) & 0xFFu), 255.0) / 255.0;
}
/* Corner q's texel (u, v) of a tile: TL,TR in x, BL,BR in y, a byte each. */
uvec2 cell_uv(uvec4 t, int q) {
    uint w = (q < 2) ? t.x : t.y;
    uint sh = uint(q & 1) * 16u;
    return uvec2((w >> sh) & 0xFFu, (w >> (sh + 8u)) & 0xFFu);
}
/* Fill the per-vertex names main() reads with what the baked cell vertex at
 * this index carried: an instance draws 8 cells x 2 triangles (TL,TR,BL) +
 * (TR,BL,BR) in cell order, each triangle 6 vertices on the near variant
 * (primary fan triangle, then the flags16-bit-15 spare) or 3 on the anim
 * variant; corner slot = triangle member + the second triangle's offset.
 * Returns false for a cell this record copy does not draw (another draw
 * class's copy carries it). */
/* A split-triangle record (info bit 24): entry lc of half h is
 * sub-triangle h*8 + lc of a tier-4 record, 16 + lc of a tier-2 one (bit
 * 25), drawn by the entry's first triangle alone (the second is killed);
 * tile uv[0..5] are nodes A, B, C's (u, v) and uv[6] the cell or quadrant. */
bool tri4_fetch(uint info, int lc, int tri2, int k, bool spare) {
    if (tri2 != 0) return false;
    uvec4 t = lc == 0 ? inCellT0 : lc == 1 ? inCellT1 : lc == 2 ? inCellT2
            : lc == 3 ? inCellT3 : lc == 4 ? inCellT4 : lc == 5 ? inCellT5
            : lc == 6 ? inCellT6 : inCellT7;
    int sub = (info & 0x2000000u) != 0u ? 16 + lc : int((info >> 7) & 1u) * 8 + lc;
    uint cell = (t.y >> 16) & 15u;
    uint f16 = 1u | (uint(k) << 3) | ((info & 0x7Fu) << 5) | 0x1000u
             | (((info >> uint(16 + lc)) & 1u) != 0u ? 0x4000u : 0u)
             | (spare ? 0x8000u : 0u);
    inPos = ivec4(inC0.xyz, int(f16));
    inUV = uvec4(cell_uv(t, k), 0u, 0u);
    inColFar = cell_unorm(nt4_node_rgb(NT_SUB[sub * 3 + k], cellFar, false));
    inColNear = cell_unorm(nt4_node_rgb(NT_SUB[sub * 3 + k], cellNear, true));
    inMat = uvec4(t.z, t.w, 0u, 0xC0000000u | (uint(sub) << 4) | cell);
    inFaceFlags = inCellFace.x;
#ifdef TERRAIN_ANIM
    inPrevPos = ivec4(inPrevC0.xyz, 0);
#endif
#ifdef TERRAIN_NEAR
    int ra = (k + 1) % 3, rb = (k + 2) % 3;
    inCtxAUV = uvec4(cell_uv(t, ra), 0u, 0u);
    inCtxACol = uvec2(0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + ra], cellFar, false),
                      0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + ra], cellNear, true));
    inCtxBUV = uvec4(cell_uv(t, rb), 0u, 0u);
    inCtxBCol = uvec2(0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + rb], cellFar, false),
                      0xFF000000u | nt4_node_rgb(NT_SUB[sub * 3 + rb], cellNear, true));
#endif
    return true;
}

bool cells_fetch() {
    cell_unpack();
    uint info = inCellFace.y;
#ifdef TERRAIN_NEAR
    int tri = gl_VertexIndex / 6;
    int w6 = gl_VertexIndex - tri * 6;
    bool spare = w6 >= 3;
    int k = spare ? w6 - 3 : w6;
#else
    int tri = gl_VertexIndex / 3;
    int k = gl_VertexIndex - tri * 3;
    bool spare = false;
#endif
    int lc = tri >> 1, tri2 = tri & 1;
    if (((info >> uint(8 + lc)) & 1u) == 0u) return false;
    if ((info & 0x1000000u) != 0u) return tri4_fetch(info, lc, tri2, k, spare);
    int cell = int((info >> 7) & 1u) * 8 + lc;
    uvec4 t = lc == 0 ? inCellT0 : lc == 1 ? inCellT1 : lc == 2 ? inCellT2
            : lc == 3 ? inCellT3 : lc == 4 ? inCellT4 : lc == 5 ? inCellT5
            : lc == 6 ? inCellT6 : inCellT7;
    int cr = cell >> 2, cq = cell & 3;
    int role = k + tri2;
    uint f16 = 1u | (uint(role) << 3) | ((info & 0x7Fu) << 5) | 0x1000u
             | (tri2 != 0 ? 0x2000u : 0u)
             | (((info >> uint(16 + lc)) & 1u) != 0u ? 0x4000u : 0u)
             | (spare ? 0x8000u : 0u);
    inPos = ivec4(inC0.xyz, int(f16));
    inUV = uvec4(cell_uv(t, role), 0u, 0u);
    inColFar = cell_unorm(cell_node_rgb(cr + (role >> 1), cq + (role & 1), cellFar));
    inColNear = cell_unorm(cell_node_rgb(cr + (role >> 1), cq + (role & 1), cellNear));
    inMat = uvec4(t.z, t.w, 0u, 0x80000000u | uint(cell));
    inFaceFlags = inCellFace.x;
#ifdef TERRAIN_ANIM
    inPrevPos = ivec4(inPrevC0.xyz, 0);
#endif
#ifdef TERRAIN_NEAR
    int ra = tri2 + (k + 1) % 3, rb = tri2 + (k + 2) % 3;
    inCtxAUV = uvec4(cell_uv(t, ra), 0u, 0u);
    inCtxACol = uvec2(0xFF000000u | cell_node_rgb(cr + (ra >> 1), cq + (ra & 1), cellFar),
                      0xFF000000u | cell_node_rgb(cr + (ra >> 1), cq + (ra & 1), cellNear));
    inCtxBUV = uvec4(cell_uv(t, rb), 0u, 0u);
    inCtxBCol = uvec2(0xFF000000u | cell_node_rgb(cr + (rb >> 1), cq + (rb & 1), cellFar),
                      0xFF000000u | cell_node_rgb(cr + (rb >> 1), cq + (rb & 1), cellNear));
#endif
    return true;
}
#endif

void main()
{
    vRim = 0u;
#ifdef TERRAIN_CELLS
    if (!cells_fetch()) { kill_vert(); return; }
#endif
#ifdef TERRAIN_RINGV
    vert_fetch();
#endif
#if defined(TERRAIN_CELLS) || defined(TERRAIN_RINGV)
    if (ringBad) { kill_vert(); return; }
#endif
    uint ff   = inFaceFlags;
    uint slot = ff & FF_SLOT_MASK;
    uint sf   = floatBitsToUint(s.sec[slot].o.w);
    int  gf   = g.fog.y;
    bool lp   = (ff & FF_TIER_LP) != 0u;
    bool quad = (ff & FF_QUAD) != 0u;
    uint f16  = uint(inPos.w) & 0xFFFFu;   /* SINT attribute: strip the sign */
    bool hqv  = (f16 & 1u) != 0u;             /* HQ-subdivide variant vertex */
    int  subq = int((f16 >> 1) & 3u);         /* HQ sub-quad index           */
    int  role = int((f16 >> 3) & 3u);         /* corner slot in the sub-quad */
    bool is4  = hqv && (inMat.w & 0x80000000u) != 0u;   /* 4x4 near-cell vertex */
    bool ist  = is4 && (inMat.w & 0x40000000u) != 0u;   /* tier-4 sub-triangle  */
    int  cell = int(inMat.w & 15u);           /* its cell, row-major        */
    int  tsub = int((inMat.w >> 4) & 31u);    /* its sub-triangle (ist)     */
    bool ist2 = ist && tsub >= 16;            /* a tier-2 sub-triangle      */
    bool fillPass = (pc.pass_sel == 1);
    bool keyPass  = (pc.pass_sel == 2);
#if defined(TERRAIN_SECDELTA) && defined(TERRAIN_READBACK)
    ctl_rev = ((f16 & 0x2000u) != 0u)
           != (ist ? (tsub & 1) != 0 : is4 && ((cell ^ (cell >> 2)) & 1) != 0);
    ctl_alt = is4 && ctl_rev;
#endif

    if ((sf & SF_DRAWN) == 0u) { kill_vert(); return; }
    /* Plain vs near-variant chunk sets: a slot holds both, and the sector's
     * per-present flags pick exactly one, so a sector never draws twice. */
#ifdef TERRAIN_NEAR
    if ((sf & SF_NEARVAR) == 0u) { kill_vert(); return; }
#else
    if ((sf & SF_NEARVAR) != 0u) { kill_vert(); return; }
#endif
    /* Chunk generation vs the published slot generation: a rebuilt sector's
     * old chunk keeps drawing for presents whose (lagged) state still names
     * the old generation; the new chunk waits its turn. */
    if (((ff >> 29) & 7u) != ((sf >> 12) & 7u)) { kill_vert(); return; }

    /* Material pass: one pass per resolved member over the same arena,
     * each keeping only the faces whose resolution class maps to it. The
     * class is baked into the face's spare material bits, so it survives
     * the sector's whole residency. */
    if (pc.mat_pass >= 0) {
        const uint mc = (inMat.x >> 24) & 0xFFu;
        if (((pc.cls_mask[mc >> 7u][(mc >> 5u) & 3u] >> (mc & 31u)) & 1u) == 0u) {
            kill_vert(); return;
        }
    }

    /* ---- pass / tier admission (sector-level, CPU-computed) ---- */
    if (fillPass) {
        if (!lp || (sf & SF_FILLLP) == 0u) { kill_vert(); return; }
    } else {
        if (lp  && (sf & SF_DRAWLP) == 0u) { kill_vert(); return; }
        if (!lp && (sf & SF_DRAWHP) == 0u) { kill_vert(); return; }
    }
    /* ---- face corners -> camera space ---- */
    ivec3 cc0 = inC0.xyz;
    ivec3 cc1 = ivec3(inC0.w, inC1.x, inC1.y);
    ivec3 cc2 = ivec3(inC1.z, inC1.w, inC2.x);
    ivec3 cc3 = inC2.yzw;
    /* Sector origins are uint16 world/16 values, exact in the float o.xyz;
     * x4 makes them the corners' scale. */
    ivec3 wo = ivec3(s.sec[slot].o.xyz) * 4;
#ifdef TERRAIN_SECDELTA
    ctl_wo = wo;
#endif
#ifdef TERRAIN_READBACK
    uvec4 rk0 = rb_leaf(wo + cc0), rk1 = rb_leaf(wo + cc1);
    uvec4 rk2 = rb_leaf(wo + cc2), rk3 = rb_leaf(wo + cc3);
#endif
#define MAC(l) mac_of(l)
#ifdef TERRAIN_ANIM
    ivec3 pc0 = inPrevC0.xyz;
    ivec3 pc1 = ivec3(inPrevC0.w, inPrevC1.x, inPrevC1.y);
    ivec3 pc2 = ivec3(inPrevC1.z, inPrevC1.w, inPrevC2.x);
    ivec3 pc3 = inPrevC2.yzw;
    float va = G_VA;
#define ANIM_LERP3(P, C) cam_rel_lerp(wo + (P), wo + (C), va)
    precise vec3 lc0 = ANIM_LERP3(pc0, cc0);
    precise vec3 lc1 = ANIM_LERP3(pc1, cc1);
    precise vec3 lc2 = ANIM_LERP3(pc2, cc2);
    precise vec3 lc3 = ANIM_LERP3(pc3, cc3);
#ifdef TERRAIN_READBACK
    rk0 = rb_leaf_lerp(wo + pc0, wo + cc0); rk1 = rb_leaf_lerp(wo + pc1, wo + cc1);
    rk2 = rb_leaf_lerp(wo + pc2, wo + cc2); rk3 = rb_leaf_lerp(wo + pc3, wo + cc3);
#endif
#elif defined(TERRAIN_NEAR)
    /* A poly-anim sector's face with a previous-corner record lerps as the
     * anim stage does; every other face takes cam_rel. */
    precise vec3 lc0, lc1, lc2, lc3;
    ivec3 pc0, pc1, pc2, pc3;
    if (!near_prev_get(pc0, pc1, pc2, pc3)) {
        lc0 = cam_rel(wo + cc0);
        lc1 = cam_rel(wo + cc1);
        lc2 = cam_rel(wo + cc2);
        lc3 = cam_rel(wo + cc3);
    } else {
        float va = G_VA;
        lc0 = cam_rel_lerp(wo + pc0, wo + cc0, va);
        lc1 = cam_rel_lerp(wo + pc1, wo + cc1, va);
        lc2 = cam_rel_lerp(wo + pc2, wo + cc2, va);
        lc3 = cam_rel_lerp(wo + pc3, wo + cc3, va);
#ifdef TERRAIN_READBACK
        rk0 = rb_leaf_lerp(wo + pc0, wo + cc0); rk1 = rb_leaf_lerp(wo + pc1, wo + cc1);
        rk2 = rb_leaf_lerp(wo + pc2, wo + cc2); rk3 = rb_leaf_lerp(wo + pc3, wo + cc3);
#endif
    }
#else
    precise vec3 lc0 = cam_rel(wo + cc0);
    precise vec3 lc1 = cam_rel(wo + cc1);
    precise vec3 lc2 = cam_rel(wo + cc2);
    precise vec3 lc3 = cam_rel(wo + cc3);
#endif
    precise vec3 m0 = MAC(lc0), m1 = MAC(lc1);
    precise vec3 m2 = MAC(lc2), m3c = MAC(lc3);
    int nc = quad ? 4 : 3;

    int    fogRef   = g.fog.x + (((ff & FF_FOGBIAS) != 0u) ? 0x2000 : 0);
    float fogscale = G_FOGS;

    /* ---- NCLIP (both halves; either half admits the near verdict) ----
     * Cross products run on DOUBLES OF THE FLOAT screen coords (the CPU's
     * NSAT(sv.sx)); the GTE [-1024,1023] saturation is waived in the UNR
     * divide-overflow regime (any corner m3 < H_canon/8, the GTE's H, so
     * the regime does not move with --fov - the .s's own SXY there is
     * pinned garbage, so the TRUE facing is used). */
    vec2 p0f = proj_of(m0), p1f = proj_of(m1), p2f = proj_of(m2), p3f = proj_of(m3c);
    precise bool unr_ovf = (m0.z < PDIV(G_HCAN, 8.0)) || (m1.z < PDIV(G_HCAN, 8.0)) ||
                          (m2.z < PDIV(G_HCAN, 8.0)) || (quad && m3c.z < PDIV(G_HCAN, 8.0));
    float nx0 = nsat(p0f.x, unr_ovf), ny0 = nsat(p0f.y, unr_ovf);
    float nx1 = nsat(p1f.x, unr_ovf), ny1 = nsat(p1f.y, unr_ovf);
    float nx2 = nsat(p2f.x, unr_ovf), ny2 = nsat(p2f.y, unr_ovf);
    float nx3 = nsat(p3f.x, unr_ovf), ny3 = nsat(p3f.y, unr_ovf);
    bool flip = (ff & FF_FLIP) != 0u;
    bool front, front2 = false;
    if (lp) {
        /* LP walk: HALF 1 over (i0,i1,i2), STRICTLY positive; HALF 2 over
         * (i3,i1,i2), STRICTLY negative; dside (w0 bit 7) emits regardless. */
        precise float n1 = (nx1 - nx0) * (ny2 - ny0) - (nx2 - nx0) * (ny1 - ny0);
        front = n1 > 0.0;
        if (quad) {
            precise float n2 = (nx1 - nx3) * (ny2 - ny3) - (nx2 - nx3) * (ny1 - ny3);
            front2 = n2 < 0.0;
        }
        if ((ff & FF_DSIDE) != 0u) front2 = true;
    } else {
        /* HP walk: HALF 1 over (i0,i1, quad?i3:i2), flip-adjusted, >= 0;
         * HALF 2 (quads) over (i2,i1,i3) with the flip-selected sign;
         * dside (the per-game word-3 bit) jumps straight to emit; a dside
         * tri skips its NCLIP verdict altogether. */
        float nxA = quad ? nx3 : nx2, nyA = quad ? ny3 : ny2;
        precise float n1 = (nx1 - nx0) * (nyA - ny0) - (nxA - nx0) * (ny1 - ny0);
        if (flip) n1 = -n1;
        front = n1 >= 0.0;
        front2 = (ff & FF_DSIDE) != 0u;
        if (quad) {
            precise float n2 = (nx1 - nx2) * (ny3 - ny2) - (nx3 - nx2) * (ny1 - ny2);
            front2 = front2 || (flip ? (n2 >= 0.0) : (n2 <= 0.0));
        }
    }

    /* ---- LP gates (gaponly / .s far cull / authored admission) ---- */
    bool gapfill_face = (gf & GF_GAPFILL) != 0 && (ff & FF_GAPFILL) != 0u;
    float z3g = quad ? m3c.z : m2.z;
    precise float slotv = PDIV(m0.z + m1.z + m2.z + z3g, 32.0);
    if (lp && (gf & GF_NOLOD) == 0) {
        /* .s LP far cull: slot >= cull_dist>>7 drops (all faces, fill tags
         * included, and the hole fill, which re-emits only faces the game's
         * LP walk draws); a keyed game tests the whole LP bucket index
         * against (cull_dist>>7) - 1. Skipped under extended LOD, which
         * keeps far faces. */
        precise float fcv = slotv;
        int fclim = g.fog.z;
        if (ot_farcull_keyed()) {
            fcv += ot_lp_slots((ff >> 24) & 0x1Fu);
            fclim -= 1;
        }
        if (fcv >= float(fclim)) { kill_vert(); return; }
    }
    if (lp && !fillPass) {
        if ((sf & SF_LPGAPONLY) != 0u && (ff & FF_GAPFILL) == 0u) { kill_vert(); return; }
        if ((sf & SF_LPUNCOV) != 0u && (ff & FF_LPRIDE) == 0u) { kill_vert(); return; }
        float thr = float((sf >> SF_LPNEAR_SHIFT) & SF_LPNEAR_MASK);
        if (!gapfill_face && (gf & GF_LPNEAROUT) != 0) {
            /* LP near outcode, the only near rule of a game that has one:
             * every LP face of a sector whose HP draws or that has no HP
             * (nonzero bound) drops when its corners all sit under the
             * sector's SZ3 bound, covered by that HP or not; with a zero
             * bound the face draws. */
            if (thr > 0.0 && m0.z < thr && m1.z < thr && m2.z < thr &&
                (!quad || m3c.z < thr)) { kill_vert(); return; }
        } else if (!gapfill_face) {
            /* Authored per-face admission: slot + 8*admit >= s3, deficit
             * -31..-1 falls back to any-corner SZ3 >= (s3-8a)*8, deeper
             * rejects. (LP is a FAR tier; og drops it near.) */
            float admit = float((ff >> 19) & 0x1Fu);
            float s3v = float((g.fog.x >> 5) - 0x20);
            precise float a1 = slotv + 8.0 * admit - s3v;
            if (a1 < 0.0) {
                if (a1 <= -32.0) { kill_vert(); return; }
                precise float thr = (s3v - 8.0 * admit) * 8.0;
                bool any = m0.z >= thr || m1.z >= thr || m2.z >= thr ||
                           (quad && m3c.z >= thr);
                if (!any) { kill_vert(); return; }
            }
        }
        /* (The legacy lpFarOnly fog-corner near gate only exists with
         * LP_ADMIT=0, which forces the CPU walk - never mirrored here.) */
    }

    /* ---- second walk (terrain.c tn_face_water, tn_water_far,
     * tn_water_drop): a FF_WATER face past the near split (corner-SZ3 sum
     * at 4 * m3 of 0x2000 or more) takes that walk's model, and is dropped
     * at a sum at fogscale of 0x6000 or more. Runs before the LOD handoff
     * drop, as in the walk. Under GF_HQPIN (terrain.c tn_water_hq) such a
     * face takes the HQ variant with that walk's colours and no facing
     * test (waterHQ). Under GF_HPRAW (terrain.c tn_water_drop) the face
     * is never dropped, and without GF_HPFOG (tn_water_raw) not faded
     * (waterRaw). ---- */
    float wz3 = quad ? m3c.z : m2.z;
    bool waterFar = !lp && !fillPass && (ff & FF_WATER) != 0u
                 && sz3_sum(m0.z, m1.z, m2.z, wz3, 4.0) >= 0x2000;
    bool waterRaw = (gf & GF_HPRAW) != 0 && (gf & GF_HPFOG) == 0;
    if (waterFar && (gf & GF_HPRAW) == 0 && sz3_sum(m0.z, m1.z, m2.z, wz3, fogscale) >= 0x6000) { kill_vert(); return; }
    bool waterHQ = waterFar && (gf & GF_HQPIN) != 0;

    /* ---- the game's HP far model under extended LOD on a heavy-fog level
     * (terrain.c tn_face_blend_far): a textured face of blend 1-3 outside a
     * second-walk sector (FF_WATER clear; a blended face of such a sector
     * carries it) that the game's own near split leaves far (tn_game_far:
     * near_canonical for a quad or under GF_TRI2, the tier-4 drain route
     * for a triangle otherwise) takes the flat variant and the CLUT ladder;
     * where the ladder does not run (SF_SECINLOD clear: a sector past the
     * game's LOD envelope) a (B+F)/2 face takes the heavy-fog words hqw and
     * an additive or subtractive face the clear words (clear_words). ---- */
    uint mblend = (inMat.x >> 21) & 3u;
    bool gameFar = (quad || (gf & GF_TRI2) != 0)
        ? !near_canonical(m0.z, m1.z, m2.z, wz3, ff & FF_W3_40, ff & FF_W3_80)
        : !((gf & GF_TRI4) != 0 && nc4_tier4(m0.z, m1.z, m2.z, m2.z, ff & FF_W3_40, ff & FF_W3_80));
    bool blendFar = !lp && !fillPass && (gf & GF_HPRAW) != 0 && (gf & GF_HPFOG) != 0
                 && (ff & FF_WATER) == 0u && (inMat.x & (1u << 23)) != 0u && mblend != 0u && gameFar;

    /* ---- LOD handoff: .s HP drop (tn_hp_face_drop mirror) ----
     * Dropped at sum of corner SZ3 >= m_LodDistance; with hp_drop.x set, a
     * face below that sum bound with any corner SZ3 < hp_drop.y is kept.
     * Runs BEFORE the near classify, exactly like the walk. */
    if (!lp && (gf & GF_HANDOFF) != 0) {
        float hpz3 = quad ? m3c.z : m2.z;
        precise float hpsum = (m0.z + m1.z + m2.z + hpz3) * fogscale;
        if (hpsum >= 4.0 * float(g.fog.x)) {
            bool hpdrop = g.hp_drop.x == 0 || hpsum >= float(g.hp_drop.x);
            if (!hpdrop) {
                precise float c0 = m0.z * fogscale, c1 = m1.z * fogscale;
                precise float c2 = m2.z * fogscale, c3 = hpz3 * fogscale;
                float cmin = float(g.hp_drop.y);
                hpdrop = c0 >= cmin && c1 >= cmin && c2 >= cmin && c3 >= cmin;
            }
            if (hpdrop) { kill_vert(); return; }
        }
    }

    /* ---- FILL pass per-face gates (fully-fogged LP only, strict x4) ---- */
    if (fillPass) {
        if (((inMat.x >> 21) & 3u) != 0u) { kill_vert(); return; }  /* opaque only */
        if (fogRef - int(m0.z * 4.0) > 0) { kill_vert(); return; }
        if (fogRef - int(m1.z * 4.0) > 0) { kill_vert(); return; }
        if (fogRef - int(m2.z * 4.0) > 0) { kill_vert(); return; }
        if (quad && (fogRef - int(m3c.z * 4.0) > 0)) { kill_vert(); return; }
    }

    /* ---- face_is_near: flat-vs-HQ variant select (HP quads only).
     * The CPU gate is (straddle ? front3d : front || front2): the .s sends
     * a face either NCLIP half accepts to the same depth-sum classifier
     * (r_environment.s 0x80026CB8/0x80026CD0 -> .L80026CFC). The projected
     * NCLIP is not the .s verdict for a straddler: an HP
     * face with a corner carrying the .s vertex flag 0x10 (terrain.c
     * tface_gte_flagged: m3 <= 0, or m3 < 0x600/4 with SX/SY outside
     * [-1024, 1023] or m3 in the UNR divide-overflow regime) skips the face
     * NCLIP, and its near drain NCLIPs every sub-quad over the same two
     * triples, so its facing is the camera-space triple product over those
     * triples (front3d, the walk's tface_det3 mirror). An LP straddler (a
     * corner at or behind the eye) keeps the waiver. Behind-eye corners
     * occur only on near chunks; the flag applies to plain chunks too. ---- */
#ifdef TERRAIN_NEAR
    bool behind = m0.z <= 0.0 || m1.z <= 0.0 ||
                  m2.z <= 0.0 || m3c.z <= 0.0;
#else
    const bool behind = false;
#endif
    bool straddle = lp ? behind
                       : (gte_flag(m0.z, p0f) || gte_flag(m1.z, p1f) ||
                          gte_flag(m2.z, p2f) || (quad && gte_flag(m3c.z, p3f)));
    bool front3d = true;
    if (!lp && straddle) {
        precise float d1 = det3d(m0, m1, quad ? m3c : m2);
        bool f1 = flip ? (d1 <= 0.0) : (d1 >= 0.0);
        bool f2 = false;
        if (quad) {
            precise float d2 = det3d(m1, m2, m3c);
            f2 = flip ? (d2 <= 0.0) : (d2 >= 0.0);
        }
        front3d = ((ff & FF_DSIDE) != 0u) || f1 || f2;
    }
    bool near = false;
    if (!lp && !fillPass && waterFar == waterHQ && !blendFar && (gf & GF_NEARHQ) != 0 && quad
        && (waterHQ || (straddle ? front3d : (front || front2))) && (ff & FF_HQELIG) != 0u) {
        if ((gf & GF_HQPIN) != 0) {
            /* --no-lod: HQ pinned ON for every eligible face at every
             * distance; only the behind-camera guard and the w3&0x80
             * exclusion remain (the latter is folded into FF_HQELIG at pack
             * time). Mirrors the walk's face_is_near extended branch. */
            precise float sz = max(m0.z, 0.0) + max(m1.z, 0.0) + max(m2.z, 0.0) + max(m3c.z, 0.0);
            near = sz > 0.0;
        } else {
            near = near_canonical(m0.z, m1.z, m2.z, m3c.z,
                                  ff & FF_W3_40, ff & FF_W3_80);
        }
    }
    /* Exactly one variant of an HQ-eligible face survives: flat, the 2x2
     * subdivide, or (GF_CELLS4, a tier-4 face the bake let take the cells,
     * FF_CELLOK: the walk's tface_cells_ok) the 4x4 cells. For a face
     * the pin sends to the cells (GF_HQPIN, FF_PINCELLS: the walk's
     * tface_cells_pin, baked per face) the tier-4 distance
     * terms drop and the flag term stays: a near face with w3 & 0x40 clear
     * takes the cells (the walk's face_near_tier4 pinned branch; w3 & 0x80
     * is folded into FF_HQELIG). */
    bool pin = (gf & GF_HQPIN) != 0 && (ff & FF_PINCELLS) != 0u;
    bool t4 = near && (gf & GF_CELLS4) != 0 && (ff & FF_CELLOK) != 0u &&
              (pin ? (ff & FF_W3_40) == 0u
                   : nc4_tier4(m0.z, m1.z, m2.z, m3c.z, ff & FF_W3_40, ff & FF_W3_80));
    /* A triangle face: the flat GT3, (GF_TRI4, the tier-4 verdict over
     * A, B, C, C on a face the bake let take the cells, FF_CELLOK) its 16
     * sub-triangle records, or (GF_TRI2, any other near
     * verdict over A, B, C, C: tn_face_near) its 4 tier-2 records. Pinned
     * to the cells: every triangle not fully behind the camera with
     * w3 & 0x40 clear takes tier 4; the HQ pin sends every other one not
     * fully behind the camera to tier 2. */
    bool trinear = !quad && !lp && !fillPass && waterFar == waterHQ && !blendFar && (gf & GF_NEARHQ) != 0
                && (waterHQ || (straddle ? front3d : (front || front2))) && (ff & FF_HQELIG) != 0u;
    precise bool trifront = max(m0.z, 0.0) + max(m1.z, 0.0) + 2.0 * max(m2.z, 0.0) > 0.0;
    bool t4tri = trinear && (gf & GF_TRI4) != 0 && (ff & FF_CELLOK) != 0u
              && (pin ? ((ff & FF_W3_40) == 0u && trifront)
                      : nc4_tier4(m0.z, m1.z, m2.z, m2.z, ff & FF_W3_40, ff & FF_W3_80));
    bool t2tri = trinear && !t4tri && (gf & GF_TRI2) != 0
              && ((gf & GF_HQPIN) != 0 ? trifront
                                       : near_canonical(m0.z, m1.z, m2.z, m2.z,
                                                        ff & FF_W3_40, ff & FF_W3_80));
    /* Extended LOD (GF_HPRAW, terrain.c s_hp_raw): an HP face draws its
     * near colours and base CLUT row at every distance. On a heavy-fog level
     * (GF_HPFOG, s_hp_fog) its colours DPCS-fade toward each corner's own far
     * colour, the game's depth-cue target, and its texels fade to
     * modulation-neutral grey on the ladder ramp: hqw, the words terrain.c
     * tn_atmo_words packs (aN = 0, bN = 0.5, bit 30 the face's fog-reference
     * bias). */
    bool hpRaw = !lp && (gf & GF_HPRAW) != 0;
    bool hpFog = hpRaw && (gf & GF_HPFOG) != 0;
    bool fogOff = (gf & GF_FOGOFF) != 0;
    bool fogPP  = (gf & GF_FOGPP) != 0;
    /* The heavy-fog words: the per-pixel ramp under the per-pixel form, the
     * per-face ladder step (flat_atmo_words) under the original form. */
    uvec2 hqw = fogPP
        ? uvec2(0x80000000u | (((ff & FF_FOGBIAS) != 0u) ? 0x40000000u : 0u),
                128u | (128u << 10) | (128u << 20))
        : flat_atmo_words(hpFog ? ladder_t0k(m0.z, m1.z, m2.z, m3c.z, fogRef, fogscale,
                                             (gf & GF_LADNEAR) != 0) : 0);
    /* Drain sub-cell keys (FF_KEYCELLS, terrain.c terrain_near_emit_face,
     * terrain_near_emit_tri, tn_flat_tri_emit): where the drain inserts a
     * near-band face as finer sub-cells than the variant drawn here (the 2x2
     * of a quad the drain takes to tier 4, the tier-2 split of a triangle it
     * takes to tier 4, a triangle drawn whole), each fragment of the drawn
     * triangles keys and contests at its own sub-cell's key: the vertex
     * carries the face's sub-cell key table and its face-local coordinate
     * (psx_key_contest.glsl). A triangle drawn whole takes the tier-4
     * sub-triangles or, outside tier 4, the tier-2 ones. */
    bool keyFace = !lp && (ff & FF_KEYCELLS) != 0u;
    bool drain4q = keyFace && quad
                && nc4_tier4(m0.z, m1.z, m2.z, m3c.z, ff & FF_W3_40, ff & FF_W3_80);
    bool drain4t = keyFace && !quad
                && nc4_tier4(m0.z, m1.z, m2.z, m2.z, ff & FF_W3_40, ff & FF_W3_80);
    bool flatKeys = keyFace && !quad && !fillPass && !(t4tri || t2tri)
                 && near_canonical(m0.z, m1.z, m2.z, m2.z, ff & FF_W3_40, ff & FF_W3_80);
    if (hqv != (near || t4tri || t2tri) || is4 != (t4 || t4tri || t2tri) || ist2 != t2tri) {
        kill_vert(); return;
    }
    /* ---- marked-edge split: this vertex's base triangle, and its own
     * member's base corners and weight (tj_ends) ---- */
    bool tjs = !is4 && (f16 & TJ_SPLIT) != 0u;
    vec3 tjB[3];
    vec2 tjS[3], tjF[3];
    int tjR[3];
    int tjD = 0, tjP = 0, tjQ = 0;
    float tjT = 0.0;
#ifdef TERRAIN_READBACK
    uvec4 rtjB[3];
#endif
    if (tjs) {
        tj_base(hqv, quad, (f16 & 0x2000u) != 0u, subq, m0, m1, m2, m3c, tjB, tjS, tjF, tjR);
#ifdef TERRAIN_READBACK
        rb_tj_base(hqv, quad, (f16 & 0x2000u) != 0u, subq, rk0, rk1, rk2, rk3, rtjB);
#endif
#ifndef TERRAIN_NEAR
        tjD = tj_desc(int((uint(inPos.z) >> 2) & 3u));
        tj_ends(tjD, tjB, tjS, tjP, tjQ, tjT);
#endif
    }
    /* nc4_front mirror: a straddling face's cell draws only when one of its
     * two triples faces; a waterHQ face's cells are not culled (terrain.c
     * tn_near_face). */
    bool hqdside = (ff & FF_DSIDE) != 0u || waterHQ;
    if (ist && straddle && !hqdside) {
        /* nt4_emit's straddler facing: the sub-triangle's own triple. */
        precise float d = det3d(nt4_node(NT_SUB[tsub * 3], m0, m1, m2),
                                 nt4_node(NT_SUB[tsub * 3 + 1], m0, m1, m2),
                                 nt4_node(NT_SUB[tsub * 3 + 2], m0, m1, m2));
        if (flip ? (d > 0.0) : (d < 0.0)) { kill_vert(); return; }
    } else if (is4 && straddle && !hqdside) {
        int cr = cell >> 2, cq = cell & 3;
        precise vec3 ctl = nc4_node(cr, cq, m0, m1, m3c, m2);
        precise vec3 ctr = nc4_node(cr, cq + 1, m0, m1, m3c, m2);
        precise vec3 cbl = nc4_node(cr + 1, cq, m0, m1, m3c, m2);
        precise vec3 cbr = nc4_node(cr + 1, cq + 1, m0, m1, m3c, m2);
        precise float d1 = det3d(ctl, ctr, cbl);
        bool cf = flip ? (d1 <= 0.0) : (d1 >= 0.0);
        if (!cf) {
            precise float d2 = det3d(ctr, cbr, cbl);
            cf = flip ? (d2 <= 0.0) : (d2 >= 0.0);
        }
        if (!cf) { kill_vert(); return; }
    }

    /* ---- NCLIP back-face cull (flat faces; near faces dispatched before
     * the cull in the walk; the fill passes included, since the fill
     * re-emits only faces the game's LP walk draws) ---- */
    if (!near) {
        bool cullable = lp ? ((gf & GF_CULLLP) != 0) : ((gf & GF_CULLHP) != 0);
        /* An HP straddler with a corner in front of the eye that the
         * classifier does not queue for the near drain is emitted whole with
         * no NCLIP (.L80026DB8, tri .L80027310). */
        float zq = quad ? m3c.z : m2.z;
        bool whole = !near_canonical(m0.z, m1.z, m2.z, zq, ff & FF_W3_40, ff & FF_W3_80)
                  && (m0.z > 0.0 || m1.z > 0.0 || m2.z > 0.0 || zq > 0.0);
        bool facing = straddle ? (lp || front3d || whole) : (front || front2);
        if (cullable && !facing && !waterFar) { kill_vert(); return; }
    }

    /* ---- portal drops: rebuild THIS vertex's emitted tri and run the
     * feed-side inner-rim cull on it (all faces). Blended faces a portal
     * preview covers are discarded per fragment instead (terrain.frag).
     * Triangulation matches the bake exactly - flat LP (i0,i1,i2)+
     * (i1,i2,i3), flat HP (i0,i1,i3)+(i1,i3,i2), HQ sub-quad (c0,c1,c2)+
     * (c1,c2,c3) - in the walk's own emit corner ORDER ((tl,tr,bl)+
     * (tr,bl,br)); the rim classifier's terms are vertex-SET invariant. Screen coords are the feed's push_tri values:
     * the raw float projections, or their snapped form under the integer
     * vertex precision, which is what the ttri the CPU classifies carries.
     * The near variant runs both classifiers after its clip
     * instead - the feed classifies the tris it actually emits, which for a
     * clipped face are the CLIPPED ones. ---- */
#ifndef TERRAIN_NEAR
    if (g.rim_cnt.x > 0) {
        bool tri2 = (f16 & 0x2000u) != 0u;
        int rimv;
        if (tjs) {
            /* The split sub-triangle's members. */
            int d0 = tj_desc(0), d1 = tj_desc(1), d2 = tj_desc(2);
            precise vec3 ta = tj_pos(d0, tjB), tb = tj_pos(d1, tjB), tc2 = tj_pos(d2, tjB);
            rimv = rim_classified(tj_scr(d0, tjB, tjS), tj_scr(d1, tjB, tjS), tj_scr(d2, tjB, tjS),
                                  ta.z, tb.z, tc2.z);
        } else if (!hqv) {
            precise vec3 ta, tb, tc2;
            vec2 sa, sb, sc;
            if (!quad)      { ta = m0; tb = m1; tc2 = m2;  sa = p0f; sb = p1f; sc = p2f; }
            else if (lp) {
                if (tri2)   { ta = m1; tb = m2; tc2 = m3c; sa = p1f; sb = p2f; sc = p3f; }
                else        { ta = m0; tb = m1; tc2 = m2;  sa = p0f; sb = p1f; sc = p2f; }
            } else {
                if (tri2)   { ta = m1; tb = m3c; tc2 = m2; sa = p1f; sb = p3f; sc = p2f; }
                else        { ta = m0; tb = m1; tc2 = m3c; sa = p0f; sb = p1f; sc = p3f; }
            }
            sa = snap_of(sa); sb = snap_of(sb); sc = snap_of(sc);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        } else if (ist) {
            /* Sub-triangle members (A,B,C), nt4_emit's order. */
            int na = NT_SUB[tsub * 3], nb = NT_SUB[tsub * 3 + 1], nc2 = NT_SUB[tsub * 3 + 2];
            precise vec3 ta = nt4_node(na, m0, m1, m2);
            precise vec3 tb = nt4_node(nb, m0, m1, m2);
            precise vec3 tc2 = nt4_node(nc2, m0, m1, m2);
            vec2 sa = nt4_screen(na, m0, m1, m2);
            vec2 sb = nt4_screen(nb, m0, m1, m2);
            vec2 sc = nt4_screen(nc2, m0, m1, m2);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        } else if (is4) {
            /* Cell members (TL,TR,BL) / (TR,BL,BR), emit_quad_cv's order. */
            precise vec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            int cr = cell >> 2, cq = cell & 3;
            ivec2 n0 = tri2 ? ivec2(cr, cq + 1) : ivec2(cr, cq);
            ivec2 n1 = tri2 ? ivec2(cr + 1, cq) : ivec2(cr, cq + 1);
            ivec2 n2 = tri2 ? ivec2(cr + 1, cq + 1) : ivec2(cr + 1, cq);
            precise vec3 ta = nc4_node(n0.x, n0.y, TL, TR, BL, BR);
            precise vec3 tb = nc4_node(n1.x, n1.y, TL, TR, BL, BR);
            precise vec3 tc2 = nc4_node(n2.x, n2.y, TL, TR, BL, BR);
            vec2 sa = nc4_screen(n0.x, n0.y, TL, TR, BL, BR);
            vec2 sb = nc4_screen(n1.x, n1.y, TL, TR, BL, BR);
            vec2 sc = nc4_screen(n2.x, n2.y, TL, TR, BL, BR);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        } else {
            /* Sub-quad nodes from the transformed parent corners (the same
             * cv_mid mirrors the own-vertex rebuild uses below). */
            precise vec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            precise vec3 mT = (TL + TR) * 0.5, mB = (BL + BR) * 0.5;
            precise vec3 mL = (TL + BL) * 0.5, mR = (TR + BR) * 0.5;
            precise vec3 C  = (mT + mB) * 0.5;
            precise vec3 q0, q1, q2, q3;
            if      (subq == 0) { q0 = TL; q1 = mT; q2 = mL; q3 = C;  }
            else if (subq == 1) { q0 = mT; q1 = TR; q2 = C;  q3 = mR; }
            else if (subq == 2) { q0 = mL; q1 = C;  q2 = BL; q3 = mB; }
            else                { q0 = C;  q1 = mR; q2 = mB; q3 = BR; }
            precise vec3 ta, tb, tc2;
            int na, nb, nc2;
            if (tri2) { ta = q1; tb = q2; tc2 = q3;
                        na = NODEMAP[subq][1]; nb = NODEMAP[subq][2];
                        nc2 = NODEMAP[subq][3]; }
            else      { ta = q0; tb = q1; tc2 = q2;
                        na = NODEMAP[subq][0]; nb = NODEMAP[subq][1];
                        nc2 = NODEMAP[subq][2]; }
            vec2 sa = hq_snap_screen(na, TL, TR, BL, BR, ta);
            vec2 sb = hq_snap_screen(nb, TL, TR, BL, BR, tb);
            vec2 sc = hq_snap_screen(nc2, TL, TR, BL, BR, tc2);
            rimv = rim_classified(sa, sb, sc, ta.z, tb.z, tc2.z);
        }
        if (rimv == 1) { kill_vert(); return; }
        vRim = rimv >= 2 ? uint(rimv - 1) : 0u;
    }
#endif

    /* ---- material: near-tile / CLUT-ladder row select (flat HP textured) ---- */
    uint mat0 = inMat.x;
    uint clut = inMat.y & 0xFFFFu;
    vec2 uv   = vec2(float(inUV.x), float(inUV.y));
    uint dbg_half = 0u, dbg_delta = 0u;
    int  dbg_t0k  = -99999;   /* tint mode 6: exact ladder key (t0k) probe */
    bool ladderFace = false;
    bool ladPP = false;   /* per-pixel ladder words on this face */
    if (!lp && !hqv && !waterFar && (gf & GF_CLUTFOG) != 0 && ((gf & GF_HPRAW) == 0 || blendFar)
        && (sf & SF_SECINLOD) != 0u
        && (mat0 & (1u << 23)) != 0u) {
        ladderFace = true;
        int t0k = ladder_t0k(m0.z, m1.z, m2.z, m3c.z, fogRef, fogscale, (gf & GF_LADNEAR) != 0);
        dbg_t0k = t0k;
        if (t0k <= 0) {
            if ((inMat.z & (1u << 23)) != 0u) {   /* near half textured */
                mat0 = inMat.z;
                clut = inMat.w & 0xFFFFu;
                uv   = vec2(float(inUV.z), float(inUV.w));
                dbg_half = 1u;
            }
        } else {
            /* Per-pixel fog draws the base row and picks the row per pixel
             * (ladPP); fog off draws the base row. */
            int delta = (fogOff || fogPP) ? 0 : min(t0k >> 8, 15);
            ladPP = fogPP;
            dbg_delta = uint(delta);
            if (delta > 0) clut = (clut + uint(delta << 6)) & 0xFFFFu;
        }
    }
#if !defined(TERRAIN_NEAR) && !defined(TERRAIN_CELLS)
    /* A split vertex's UV mixes its base corners' from its record, on the
     * tile half the face's ladder select drew. */
    if (tjs) {
        uint r = tj_rec();
        bool nh = !hqv && dbg_half == 1u;
        uv = mix(tj_rec_uv(r, tjR[tjP], nh), tj_rec_uv(r, tjR[tjQ], nh), float(tjT));
    }
#endif
    /* HD-fold entry select: the bake stores one entry id per LOD half in
     * the clut words' upper 16 bits (0 = no replacement); the drawn half's
     * id ships to the fragment stage with the CONT-fog flags (ladder faces
     * fade on the published ramp; fogbias shifts it - ff bit 10). */
    vHd = 0u;
    if (g.hd_lad.w != 0.0 && (mat0 & (1u << 23)) != 0u) {
        uint hid = (dbg_half == 1u ? (inMat.w >> 16) : (inMat.y >> 16)) & 0xFFFFu;
        if (hid != 0u)
            vHd = hid | ((ladderFace && !fogOff) ? 0x10000u : 0u)
                      | (((ff >> 10) & 1u) << 17);
    }
    /* env UV-scroll: per-tile fractional V offset for THIS build. Flat
     * faces: the LQ dv (both LOD halves share one offset - the game writes
     * the same scroll byte into all four V slots); HQ sub-quads: the
     * per-half dv (entries 1-2 vs 3-4 sit half a window apart in the
     * scroll ring). Zero for a tile that is not on the scroller list. */
    /* A borrowed face's tile id names the source level's table, whose
     * scroll state this scene does not run. */
    bool scrollOn = (f16 & 0x1000u) != 0u && (lp || (ff & FF_BORROWED) == 0u);
    float uvdv = 0.0;
    if (scrollOn) {
        int stile = int((f16 >> 5) & 0x7Fu);
        uvdv = ist2 ? g.sdv[stile][(cell >> 1) + 1]
             : is4 ? g.sdvc[stile][cell >> 2]
             : hqv ? g.sdv[stile][(subq >> 1) + 1] : g.sdv[stile].x;
        uv.y += uvdv;
    }
    bool blendOff = blendFar && !ladderFace;
    vMat = uvec4(mat0, (clut & 0x3Fu) * 16u | (((clut >> 6) & 0x1FFu) << 16),
                 (hpFog && !waterFar && (!blendFar || (blendOff && mblend == 1u))
                  && (mat0 & (1u << 23)) != 0u) ? hqw
                 : blendOff ? clear_words(fogPP, (ff & FF_FOGBIAS) != 0u,
                                          ladder_t0k(m0.z, m1.z, m2.z, m3c.z, fogRef, fogscale,
                                                     (gf & GF_LADNEAR) != 0))
                 : ladPP ? uvec2(((ff & FF_FOGBIAS) != 0u) ? 0x40000000u : 0u, 0x40000000u)
                 : uvec2(0u));
    /* A borrowed face samples the auxiliary VRAM bank, 512 rows below the
     * PSX VRAM: bit 31 tells the fragment stage to offset the page row,
     * and the CLUT row carries the offset here. */
    if (!lp && (ff & FF_BORROWED) != 0u)
        vMat.xy = uvec2((mat0 & 0x00FFFFFFu) | 0x80000000u, vMat.y + (512u << 16));
    /* Debug tints 0-6 repurpose vMat.y; mode 7 keeps it stock, since its
     * fragment runs the stock texel fetch and cut-out. */
    if ((TER_TINT & (1 << 29)) != 0 && ((TER_TINT >> 26) & 7) != 7) {   /* ladder verdict */
        vMat.y |= (dbg_half << 30) | (dbg_delta << 25);
        if (((TER_TINT >> 26) & 7) == 6)       /* mode 6: exact t0k readout */
            vMat.y = dbg_t0k <= -99999
                   ? 0u
                   : (uint(clamp(dbg_t0k + 4096, 0, 8191)) << 18)
                     | (dbg_half << 17) | (1u << 16)
                     | ((slot & 0x7Fu) << 9);   /* winner's sector slot */
    }

    /* ---- own vertex camera-space value ----
     * Flat/LP verts: transform own position (identical ops to the corner
     * transforms). HQ verts: rebuild the sub-quad node from the TRANSFORMED
     * parent corners (cv_mid mirrors - midpoints averaged in camera space).
     * The near variant instead rebuilds the WHOLE emitted triangle, clips it
     * against the near plane and becomes one corner of the clipped fan. */
    precise vec3 mo;
    vec2 sscr;      /* the emitted screen point: push_tri's t->sx/sy mirror */
#ifdef TERRAIN_READBACK
    uvec4 rbk;      /* mo's readback key */
#endif
    vec2 mfl;       /* its drain face-local coordinate (psx_key_contest.glsl) */
#ifdef TERRAIN_NEAR
    vec4 col;
    {
        /* Emitted-tri member positions, in the bake's emit order. */
        bool tri2n = (f16 & 0x2000u) != 0u;
        vec3 tp[3];
        /* Per-member screen override: set on a subdivide node (the CPU's
         * ngte_cvert.oflag), absent on a flat face's own corners. */
        vec2 tos[3] = vec2[3](vec2(0.0), vec2(0.0), vec2(0.0));
        bool tov = false;
        /* Members' drain coordinates (psx_key_contest.glsl): a triangle's
         * corners A, B, C at (0,0), (4,0), (0,4), a split's nodes at their
         * grid places, a 2x2 quadrant's corners at their isoline sides; a
         * flat quad keys whole and carries none. */
        vec2 tfl[3] = vec2[3](vec2(0.0), vec2(0.0), vec2(0.0));
        if (!hqv) {
            if (!quad)      { tp[0] = m0; tp[1] = m1; tp[2] = m2;
                              tfl[1] = vec2(4.0, 0.0); tfl[2] = vec2(0.0, 4.0); }
            else if (lp) {
                if (tri2n)  { tp[0] = m1; tp[1] = m2; tp[2] = m3c; }
                else        { tp[0] = m0; tp[1] = m1; tp[2] = m2;  }
            } else {
                if (tri2n)  { tp[0] = m1; tp[1] = m3c; tp[2] = m2; }
                else        { tp[0] = m0; tp[1] = m1; tp[2] = m3c; }
            }
        } else if (ist) {
            /* Sub-triangle members (A,B,C); each carries its emitted
             * point (the snap) as its override. */
            for (int k = 0; k < 3; k++) {
                int n = NT_SUB[tsub * 3 + k];
                tp[k]  = nt4_node(n, m0, m1, m2);
                tos[k] = nt4_screen(n, m0, m1, m2);
                tfl[k] = nt4_fl(n);
            }
            tov = true;
        } else if (is4) {
            /* Cell members (TL,TR,BL) / (TR,BL,BR); each carries its
             * emitted point (snap and morph) as its override. */
            precise vec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            int cr = cell >> 2, cq = cell & 3;
            ivec2 nn[3];
            nn[0] = tri2n ? ivec2(cr, cq + 1) : ivec2(cr, cq);
            nn[1] = tri2n ? ivec2(cr + 1, cq) : ivec2(cr, cq + 1);
            nn[2] = tri2n ? ivec2(cr + 1, cq + 1) : ivec2(cr + 1, cq);
            for (int k = 0; k < 3; k++) {
                tp[k]  = nc4_node(nn[k].x, nn[k].y, TL, TR, BL, BR);
                tos[k] = nc4_screen(nn[k].x, nn[k].y, TL, TR, BL, BR);
                tfl[k] = vec2(float(nn[k].y), float(nn[k].x));
            }
            tov = true;
        } else {
            precise vec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            precise vec3 mT = (TL + TR) * 0.5, mB = (BL + BR) * 0.5;
            precise vec3 mL = (TL + BL) * 0.5, mR = (TR + BR) * 0.5;
            precise vec3 C  = (mT + mB) * 0.5;
            precise vec3 q0, q1, q2, q3;
            if      (subq == 0) { q0 = TL; q1 = mT; q2 = mL; q3 = C;  }
            else if (subq == 1) { q0 = mT; q1 = TR; q2 = C;  q3 = mR; }
            else if (subq == 2) { q0 = mL; q1 = C;  q2 = BL; q3 = mB; }
            else                { q0 = C;  q1 = mR; q2 = mB; q3 = BR; }
            int base = tri2n ? 1 : 0;
            if (tri2n) { tp[0] = q1; tp[1] = q2; tp[2] = q3; }
            else       { tp[0] = q0; tp[1] = q1; tp[2] = q2; }
            for (int k = 0; k < 3; k++) tfl[k] = quad_side_fl(q0, q1, q2, q3, tp[k]);
            if (pc.snap != 0) {
                for (int k = 0; k < 3; k++)
                    tos[k] = hq_snap_screen(NODEMAP[subq][base + k],
                                            TL, TR, BL, BR, tp[k]);
                tov = true;
            }
        }
#ifdef TERRAIN_READBACK
        /* The members' readback keys, as tp was built above. */
        uvec4 tk[3];
        if (!hqv) {
            if (!quad)      { tk[0] = rk0; tk[1] = rk1; tk[2] = rk2; }
            else if (lp) {
                if (tri2n)  { tk[0] = rk1; tk[1] = rk2; tk[2] = rk3; }
                else        { tk[0] = rk0; tk[1] = rk1; tk[2] = rk2; }
            } else {
                if (tri2n)  { tk[0] = rk1; tk[1] = rk3; tk[2] = rk2; }
                else        { tk[0] = rk0; tk[1] = rk1; tk[2] = rk3; }
            }
        } else if (ist) {
            for (int k = 0; k < 3; k++) tk[k] = rb_nt4_node(NT_SUB[tsub * 3 + k], rk0, rk1, rk2);
        } else if (is4) {
            int cr = cell >> 2, cq = cell & 3;
            for (int k = 0; k < 3; k++) {
                ivec2 n = k == 0 ? (tri2n ? ivec2(cr, cq + 1) : ivec2(cr, cq))
                        : k == 1 ? (tri2n ? ivec2(cr + 1, cq) : ivec2(cr, cq + 1))
                        :          (tri2n ? ivec2(cr + 1, cq + 1) : ivec2(cr + 1, cq));
                tk[k] = rb_nc4_node(n.x, n.y, rk0, rk1, rk3, rk2);
            }
        } else {
            int base = tri2n ? 1 : 0;
            for (int k = 0; k < 3; k++)
                tk[k] = rb_hq2_node(NODEMAP[subq][base + k], rk0, rk1, rk3, rk2);
        }
#endif
        /* Member attributes: self's are this vertex's own, the other two
         * ride the ctx twin in emit order (a = self+1, b = self+2 mod 3).
         * All six vertices of a source triangle sit at whole-triangle
         * multiples from the arena base, so gl_VertexIndex % 3 IS the slot. */
        int self = gl_VertexIndex % 3;
        int ia = (self + 1) % 3, ib = (self + 2) % 3;
        vec3 cf[3], cn[3];
        vec2 uvm[3];
        cf[self] = op_unorm8(inColFar.rgb);
        cn[self] = op_unorm8(inColNear.rgb);
        uvm[self] = uv;
        cf[ia] = unrgb(inCtxACol.x); cn[ia] = unrgb(inCtxACol.y);
        cf[ib] = unrgb(inCtxBCol.x); cn[ib] = unrgb(inCtxBCol.y);
#ifndef TERRAIN_CELLS
        /* A colour-table face: every member's colour from its corners'
         * colours in the sector's table at the colour alpha, by its node
         * (cells take theirs in cell_unpack). rgba_far holds the four
         * corners' table indices. */
        if ((near_face_flags() & FACE_F_COLTAB) != 0u) {
            uvec4 ix = uvec4(round(inColFar * 255.0));
            uvec4 qn, qf;
            for (int q = 0; q < 4; q++) {
                /* A triangle's slot 3 no member reads. */
                qn[q] = ix[q] == COLTAB_BAKED ? 0u : coltab_col(slot, lp ? 0 : 1, ix[q]);
                qf[q] = ix[q] == COLTAB_BAKED ? 0u : coltab_col(slot, lp ? 0 : 2, ix[q]);
            }
            for (int k = 0; k < 3; k++) {
                int n = near_member_node(k, hqv, quad, lp, tri2n, subq);
                cn[k] = unrgb(hq2_node_rgb(n, qn));
                cf[k] = unrgb(hq2_node_rgb(n, qf));
            }
        }
#endif
        uvm[ia] = (dbg_half == 1u) ? vec2(inCtxAUV.z, inCtxAUV.w)
                                   : vec2(inCtxAUV.x, inCtxAUV.y);
        uvm[ib] = (dbg_half == 1u) ? vec2(inCtxBUV.z, inCtxBUV.w)
                                   : vec2(inCtxBUV.x, inCtxBUV.y);
        if (scrollOn) { uvm[ia].y += uvdv; uvm[ib].y += uvdv; }

        /* A split vertex's members (tj_desc) over its base triangle, the
         * members resolved above: under the integer snap every corner takes
         * its emitted point so a node can sit on its edge's segment. An HQ
         * split mixes the corners' raw attributes into its members before
         * they shade at their own depths, as the subdivide's nodes do. */
        if (tjs && pc.snap != 0 && !tov) {
            for (int k = 0; k < 3; k++) tos[k] = snap_of(proj_of(tp[k]));
            tov = true;
        }
        int tjd3[3], tjp3[3], tjq3[3];
        float tjt3[3];
        for (int k = 0; k < 3 && tjs; k++) {
            tjd3[k] = tj_desc(k);
            tj_ends(tjd3[k], tp, tos, tjp3[k], tjq3[k], tjt3[k]);
        }
        /* A member's colour residual; a re-shaded node's colour is the fog's
         * bytes, as a corner's is. */
        ivec3 tq[3] = ivec3[3](ivec3(0), ivec3(0), ivec3(0));
        if (tjs && hqv) {
            vec3 p2[3];
            vec3 f2[3], n2[3];
            ivec3 q2[3];
            vec2 u2[3], o2[3], l2[3];
            for (int k = 0; k < 3; k++) {
                int a = tjp3[k], b = tjq3[k];
                float t = float(tjt3[k]);
                p2[k] = tj_pos(tjd3[k], tp);
                f2[k] = tj_mix8(cf[a], cf[b], t);
                n2[k] = tj_mix8(cn[a], cn[b], t);
                q2[k] = (waterFar || hpFog) ? ivec3(0) : tj_rgbq(cn[a], cn[b], t);
                u2[k] = mix(uvm[a], uvm[b], t);
                o2[k] = tj_scr(tjd3[k], tp, tos);
                l2[k] = mix(tfl[a], tfl[b], t);
            }
#ifdef TERRAIN_READBACK
            uvec4 tk2[3];
            for (int k = 0; k < 3; k++) tk2[k] = rb_tj_pos(tjd3[k], tk);
            tk = tk2;
#endif
            for (int k = 0; k < 3; k++) {
                tp[k] = p2[k]; cf[k] = f2[k]; cn[k] = n2[k]; tq[k] = q2[k];
                uvm[k] = u2[k]; tos[k] = o2[k]; tfl[k] = l2[k];
            }
        }

        bool doFog   = hpRaw ? hpFog : (!lp && !hqv && !fogOff);
        bool doGrade = lp && (sf & SF_LPGRADE) != 0u;
        const int  dbgmode = (TER_TINT >> 26) & 7;
        const bool dbgraw  = (TER_TINT & (1 << 29)) != 0 && (dbgmode == 4 || dbgmode == 5);
        cvx cin[3];
        for (int k = 0; k < 3; k++) {
            /* an HQ vertex bakes its true far colour; unfogged it draws near */
            vec3 c = waterFar ? water_col(cn[k], tp[k].z, fogscale, fogOff || waterRaw)
                   : terrain_col(tp[k].z, hpRaw ? (hpFog ? cf[k] : cn[k]) : ((hqv || (fogOff && !lp)) ? cn[k] : cf[k]), cn[k], doFog,
                                 fogRef, fogscale, doGrade, s.sec[slot].gp, s.sec[slot].gq);
            if (dbgraw) c = (dbgmode == 4) ? cn[k] : cf[k];
            cin[k].m  = tp[k];
            cin[k].c  = ivec3(round(c * 255.0));
            cin[k].q  = tq[k];
            cin[k].uv = uvm[k];
            cin[k].os = tos[k];
            cin[k].ov = tov;
            cin[k].fl = tfl[k];
#ifdef TERRAIN_READBACK
            cin[k].rk = tk[k];
#endif
        }
        /* A flat split mixes its corners' shaded colours into its nodes,
         * which keeps the flat face's shading. */
        if (tjs && !hqv) {
            cvx c2[3];
            for (int k = 0; k < 3; k++) {
                int a = tjp3[k], b = tjq3[k];
                float t = float(tjt3[k]);
                c2[k] = cin[a];
                if (tjd3[k] < 3) continue;
                c2[k].m  = tj_pos(tjd3[k], tp);
                c2[k].c  = ivec3(floor(mix(vec3(cin[a].c), vec3(cin[b].c), t) + 0.5));
                c2[k].q  = tj_rgbq(vec3(cin[a].c) / 255.0, vec3(cin[b].c) / 255.0, t);
                c2[k].uv = mix(cin[a].uv, cin[b].uv, t);
                c2[k].os = tj_scr(tjd3[k], tp, tos);
                c2[k].fl = mix(cin[a].fl, cin[b].fl, t);
#ifdef TERRAIN_READBACK
                c2[k].rk = rb_tj_pos(tjd3[k], tk);
#endif
            }
            cin = c2;
        }

        /* Sutherland-Hodgman against m3 >= G_NEAR (the walk's
         * clip_push_tri): the plane never moves deeper, since a deeper cut
         * removes the part of the face between the planes, which for a large
         * face under a low camera lies inside the frame. */
        cvx outv[4];
        int nout = 0;
        const float zc = G_NEAR;
        for (int i = 0; i < 3; i++) {
            cvx cur = cin[i], nxt = cin[(i + 1) % 3];
            bool curIn = cur.m.z >= zc, nxtIn = nxt.m.z >= zc;
            if (curIn) outv[nout++] = cur;
            if (curIn != nxtIn && nout < 4) outv[nout++] = cv_lerp_n(cur, nxt, zc);
        }
        if (nout < 3) { kill_vert(); return; }   /* fully behind the plane */
        /* Fan the clipped polygon: (out0,out1,out2) + (out0,out2,out3). */
        bool spare = (f16 & 0x8000u) != 0u;
        if (spare && nout < 4) { kill_vert(); return; }
        int e0 = 0, e1 = spare ? 2 : 1, e2 = spare ? 3 : 2;
        if (g.rim_cnt.x > 0) {
            vec2 sa = scr_of(outv[e0]), sb = scr_of(outv[e1]),
                 sc = scr_of(outv[e2]);
            int rimv = rim_classified(sa, sb, sc, outv[e0].m.z, outv[e1].m.z, outv[e2].m.z);
            if (rimv == 1) { kill_vert(); return; }
            vRim = rimv >= 2 ? uint(rimv - 1) : 0u;
        }
        cvx V = outv[self == 0 ? e0 : (self == 1 ? e1 : e2)];
        mo  = V.m;
        sscr = scr_of(V);
#ifdef TERRAIN_READBACK
        rbk = V.rk;
#endif
        mfl = V.fl;
        uv  = V.uv;
        col = vec4(op_rgb8q(V.c, V.q), 1.0);
    }
#else
    if (tjs) {
        mo = tj_pos(tjD, tjB);
        sscr = tj_scr(tjD, tjB, tjS);
#ifdef TERRAIN_READBACK
        rbk = rb_tj_pos(tjD, rtjB);
#endif
        mfl = mix(tjF[tjP], tjF[tjQ], float(tjT));
    } else if (!hqv) {
#ifdef TERRAIN_ANIM
        precise vec3 lown = ANIM_LERP3(inPrevPos.xyz, inPos.xyz);
#else
        precise vec3 lown = cam_rel(wo + inPos.xyz);
#endif
        mo = MAC(lown);
        sscr = snap_of(proj_of(mo));
#ifdef TERRAIN_READBACK
#ifdef TERRAIN_ANIM
        rbk = rb_leaf_lerp(wo + inPrevPos.xyz, wo + inPos.xyz);
#else
        rbk = rb_leaf(wo + inPos.xyz);
#endif
#endif
        /* A triangle's corner A, B or C at (0,0), (4,0), (0,4); a flat
         * quad keys whole and carries none. */
        ivec3 op = inPos.xyz;
        mfl = quad || op == inC0.xyz ? vec2(0.0)
            : op == ivec3(inC0.w, inC1.x, inC1.y) ? vec2(4.0, 0.0) : vec2(0.0, 4.0);
    } else if (ist) {
        int n = NT_SUB[tsub * 3 + role];
        mo = nt4_node(n, m0, m1, m2);
        sscr = nt4_screen(n, m0, m1, m2);
#ifdef TERRAIN_READBACK
        rbk = rb_nt4_node(n, rk0, rk1, rk2);
#endif
        mfl = nt4_fl(n);
    } else if (is4) {
        precise vec3 TL = m0, TR = m1, BL = m3c, BR = m2;
        int nr = (cell >> 2) + (role >> 1), nq = (cell & 3) + (role & 1);
        mo = nc4_node(nr, nq, TL, TR, BL, BR);
        sscr = nc4_screen(nr, nq, TL, TR, BL, BR);
#ifdef TERRAIN_READBACK
        rbk = rb_nc4_node(nr, nq, rk0, rk1, rk3, rk2);
#endif
        mfl = vec2(float(nq), float(nr));
    } else {
        /* Parent corners in .s grid order: TL=i0, TR=i1, BL=i3, BR=i2. */
        precise vec3 TL = m0, TR = m1, BL = m3c, BR = m2;
        precise vec3 mT = (TL + TR) * 0.5, mB = (BL + BR) * 0.5;
        precise vec3 mL = (TL + BL) * 0.5, mR = (TR + BR) * 0.5;
        precise vec3 C  = (mT + mB) * 0.5;
        int node = NODEMAP[subq][role];
        mo = node == 0 ? TL : node == 1 ? TR : node == 2 ? BR : node == 3 ? BL
           : node == 4 ? mT : node == 5 ? mB : node == 6 ? mL : node == 7 ? mR : C;
        sscr = hq_snap_screen(node, TL, TR, BL, BR, mo);
#ifdef TERRAIN_READBACK
        rbk = rb_hq2_node(node, rk0, rk1, rk3, rk2);
#endif
        mfl = subq == 0 ? quad_side_fl(TL, mT, mL, C, mo)
            : subq == 1 ? quad_side_fl(mT, TR, C, mR, mo)
            : subq == 2 ? quad_side_fl(mL, C, BL, mB, mo)
            :             quad_side_fl(C, mR, mB, BR, mo);
    }
#endif

    /* ---- og OT key (feed koff mirror; ambient per face / per HQ sub-quad) ---- */
    precise float koff = 0.0;
    float kcls = OP_KEY_CLASS_NONE;
    dtab ktab;
    ktab.t0 = uvec4(0u); ktab.t1 = uvec4(0u); ktab.kmin = 0.0; ktab.n = 0;
    if ((gf & GF_KEYON) != 0 && !fillPass) {
        /* Foreground (near-band) faces: ALWAYS the canonical classifier
         * here (LP tests the distance band alone - word3 = 0), independent
         * of the NOLOD HQ pin. GF_NBKEYS (game_layout.terrain.nearband_ot_keys)
         * set: the game keys them through the ordinary insert with their
         * authored slots, so they take the far faces' key below. Clear: the
         * game's near queue has no OT index, so opaque ones are contest-only
         * (negative key, no slots). Blended faces stay out at every
         * distance (the feed's koff = 0). */
        bool nearband = near_canonical(m0.z, m1.z, m2.z, quad ? m3c.z : m2.z,
                                       lp ? 0u : (ff & FF_W3_40),
                                       lp ? 0u : (ff & FF_W3_80));
        /* otkey: a whole face's 4-corner view-Z sum x4 (tris sum the shared
         * i2==i3 corner twice, like the .s); a subdivided face in the near
         * band (one the game drains) has its triangles carry their own drain
         * sub-cell's key (drain_key4 / drain_key3, emit_quad_cv and
         * nt_emit), and one the always-HQ pin subdivides outside the band
         * keeps the whole-face key, as the game inserts that face whole.
         * Rounded to float exactly where the walk rounds. */
        precise float ok;
        if (!hqv || !nearband) {
            ok = float((m0.z + m1.z + m2.z + m3c.z) * 4.0);
        } else if (ist) {
            ok = drain_key3(nt4_node(NT_SUB[tsub * 3], m0, m1, m2).z,
                            nt4_node(NT_SUB[tsub * 3 + 1], m0, m1, m2).z,
                            nt4_node(NT_SUB[tsub * 3 + 2], m0, m1, m2).z);
        } else if (is4) {
            int cr = cell >> 2, cq = cell & 3;
            ok = drain_key4(nc4_node(cr, cq, m0, m1, m3c, m2).z,
                            nc4_node(cr, cq + 1, m0, m1, m3c, m2).z,
                            nc4_node(cr + 1, cq, m0, m1, m3c, m2).z,
                            nc4_node(cr + 1, cq + 1, m0, m1, m3c, m2).z);
        } else {
            precise vec3 TL = m0, TR = m1, BL = m3c, BR = m2;
            precise vec3 mT = (TL + TR) * 0.5, mB = (BL + BR) * 0.5;
            precise vec3 mL = (TL + BL) * 0.5, mR = (TR + BR) * 0.5;
            precise vec3 C  = (mT + mB) * 0.5;
            precise vec4 sq =
                subq == 0 ? vec4(TL.z, mT.z, mL.z, C.z)
              : subq == 1 ? vec4(mT.z, TR.z, C.z, mR.z)
              : subq == 2 ? vec4(mL.z, C.z, BL.z, mB.z)
              :             vec4(C.z, mR.z, mB.z, BR.z);
            ok = drain_key4(sq.x, sq.y, sq.z, sq.w);
        }
        /* A drawn triangle of a face the drain inserts finer keys at the
         * least of its sub-cell keys; the fragment adds its own sub-cell's
         * excess from the table (psx_key_contest.glsl op_drain_delta). */
        bool kcull = straddle && !(hqv ? hqdside : (ff & FF_DSIDE) != 0u);
        bool kflip = (ff & FF_FLIP) != 0u;
        if (hqv && !is4 && drain4q)
            ktab = quad_cells_tab(m0, m1, m3c, m2, kcull, kflip);
        else if (ist2 && (gf & GF_TRI4) != 0 && drain4t)
            ktab = tri_cells_tab(m0, m1, m2, false, kcull, kflip);
        else if (!hqv && flatKeys)
            ktab = tri_cells_tab(m0, m1, m2, !drain4t, kcull, kflip);
        if (ktab.n > 0) ok = ktab.kmin;
        /* Blended terrain never contests (koff 0, as the walk's
         * terrain.c): it emits no key, and true depth decides it against
         * opaque terrain at every distance. */
        if (((mat0 >> 21) & 3u) != 0u) {
            koff = 0.0;
        } else if (nearband && (gf & GF_NBKEYS) == 0) {
            koff = -float(float(ok) + 0.5);
        } else {
            uint ob = (ff >> 24) & 0x1Fu;
            precise float slots = lp ? ot_lp_slots(ob)
                              : float(((ff & FF_LPKEY) != 0u ? ot_lp_base() : 0u)
                                       + ot_hp_mul() * ob);
            koff = float(float(ok) + 128.0 * slots + 0.5);
            kcls = op_key_class(!lp && (ff & FF_LPKEY) == 0u,
                                (gf & GF_NOBIAS) != 0 ? 0.0 : float(slots));
        }
    }
    vKoff = koff;
    vKcls = kcls;
    /* The table rides only a positively keyed textured face: the key pass
     * rasters nothing else, op_drain_koff_pf moves nothing else, and the
     * walked scene locates the sub-cell through the texture coordinate
     * (psx_key_contest.glsl op_drain_record), which an untextured face
     * lacks. A folded quadrant's triangle also carries the face's corners
     * and its position D in the fold test's homogeneous screen space. */
    vKn0 = vec4(0.0); vKn1 = vec4(0.0); vKn2 = vec4(0.0);
    if (koff > 0.25 && ktab.n > 0 && (mat0 & (1u << 23)) != 0u) {
        vKc0 = ktab.t0; vKc1 = ktab.t1;
        if (hqv && !is4) vKc0.x |= uint(subq) << 30;   /* the drawn quadrant */
        uint fw = subq < 2 ? vKc0.y : vKc0.z;           /* entry 2 + subq, bit 14 */
        if (hqv && !is4 && ((fw >> (uint(subq & 1) * 16u + 14u)) & 1u) != 0u) {
            vec3 cTL = fold_h(m0), cTR = fold_h(m1), cBL = fold_h(m3c), cBR = fold_h(m2);
            vKn0 = vec4(cTL, cTR.x); vKn1 = vec4(cTR.yz, cBL.xy); vKn2 = vec4(cBL.z, cBR);
        }
    }
    else                           { vKc0 = uvec4(0u); vKc1 = uvec4(0u); }
    vec3 kd = vec3(sscr, 1.0) * float(mo.z);

    /* Debug tint mode 7 (face attribution): bits 8-29 of vRim carry this
     * face's identity word (attr_word) for terrain_shade.glsl's ter_attr. */
    if ((TER_TINT & (1 << 29)) != 0 && ((TER_TINT >> 26) & 7) == 7) {
        if ((TER_TINT & (1 << 25)) == 0) {
            vRim = (vRim & 0xFFu) | (attr_word(slot, lp, hqv, is4, koff > 0.25 && ktab.n > 0) << 8);
        } else {
            /* tint bit 25: the face's verdict classes instead, one bit
             * each, for the precision A/B's per-class engagement. */
            uint cw = (is4 && !ist ? 1u : 0u) | (ist && !ist2 ? 2u : 0u) | (ist2 ? 4u : 0u)
                    | (waterFar ? 8u : 0u) | (hpRaw ? 16u : 0u) | (hpFog ? 32u : 0u)
                    | ((vRim & 0xFFu) != 0u ? 64u : 0u) | (hqv ? 128u : 0u) | (lp ? 256u : 0u)
                    | (tjs ? 1024u : 0u) | (pc.snap != 0 ? 2048u : 0u)
                    | (AFFINE_UV ? 4096u : 0u) | ((gf & GF_FOGOFF) != 0 ? 8192u : 0u)
                    | ((gf & GF_FOGPP) != 0 ? 16384u : 0u) | ((gf & GF_NOLOD) != 0 ? 32768u : 0u)
                    | (koff > 0.25 ? 65536u : 0u) | (fillPass ? 131072u : 0u)
                    | (blendFar ? 262144u : 0u);
#ifdef TERRAIN_NEAR
            cw |= 512u;
#endif
            vRim = (vRim & 0xFFu) | (cw << 8);
        }
    }

    /* ---- KEY pass: the flat og key's face (terrain_key.vert mirror) ----
     * terrain_key.vert receives the key K as view-Z from the CPU feed; the
     * cache holds K itself. terrain_key_cache.frag stores the key as its
     * fragment depth; the window depth here only places the face for the
     * clipper (op_key_clip). */
    if (keyPass) {
        if (koff <= 0.25) { kill_vert(); return; }   /* keyed faces only */
        float K  = koff - 0.5;
        float kv = max(K, 1.0);
        vec2 cxy = clip_of(sscr);
        /* terrain_key_cache.frag declares depth_greater. Every depth it
         * stores for this face is op_key_store of at least kfloor: kv plus a
         * non-negative drain sub-cell key, and on one ordering table at least
         * the key's bucket floor (op_key_tier_key). The window depth sits a
         * factor 1 - 2^-16 below op_key_store(kfloor), so an early depth test
         * rejects only a fragment the stored depth's late test rejects. */
        float kfloor = (SLOT_CONTEST && OP_KEY_TIER_ORDER != 0)
                     ? floor(kv / OP_KEY_SLOT) * OP_KEY_SLOT : kv;
        gl_Position = vec4(cxy * kv, op_key_store(kfloor) * kv * (1.0 - 1.0 / 65536.0), kv);
        float fvz = float(mo.z) * 16.0;              /* TRUE per-vertex view-Z */
        float wq  = AFFINE_UV ? 1.0 : max(fvz, 1.0);
        vUV = uv / wq;
        vQ  = 1.0 / wq;
        /* x: the owner's true 1/view-Z, screen-linear under the flat key W
         * (terrain_key_cache.frag writes the owner's view-Z beside the key). */
        vColor = vec4(1.0 / max(fvz, 1.0), 0.0, 0.0, 1.0);
        /* p interpolates as the UV does (psx_key_contest.glsl). */
        vKfl = mfl / wq;
        vKd  = kd / wq;
        return;
    }

#ifndef TERRAIN_NEAR
    /* ---- colour: GTE-exact DPCS fog (flat HP); baked raw otherwise. The
     * near variant resolved this per member before its clip. ---- */
#if defined(TERRAIN_ANIM) && !defined(TERRAIN_CELLS)
    vec3 cfar8, cnear8;
    if ((uint(inPrevPos.w) & PAD_COLTAB) != 0u) {
        /* A colour-table corner: its colours from the sector's table. */
        uint idx = uint(inPrevPos.w) & 0xFFu;
        cfar8  = unrgb(coltab_col(slot, lp ? 0 : 2, idx));
        cnear8 = unrgb(coltab_col(slot, lp ? 0 : 1, idx));
    } else {
        cfar8  = canim_col(inPrevCol.x, op_unorm8(inColFar.rgb));
        cnear8 = canim_col(inPrevCol.y, op_unorm8(inColNear.rgb));
    }
#else
    vec3 cfar8  = op_unorm8(inColFar.rgb);
    vec3 cnear8 = op_unorm8(inColNear.rgb);
#endif
    /* One vertex's colour at view-Z Z from its raw far and near colours. */
#define TJ_SHADE(Z, CF, CN) (waterFar ? water_col(CN, Z, fogscale, fogOff || waterRaw) \
        : terrain_col(Z, hpRaw ? (hpFog ? CF : CN) : ((hqv || (fogOff && !lp)) ? CN : CF), CN, \
                      hpRaw ? hpFog : (!lp && !hqv && !fogOff), fogRef, fogscale, \
                      lp && (sf & SF_LPGRADE) != 0u, s.sec[slot].gp, s.sec[slot].gq))
    vec4 col = vec4(TJ_SHADE(mo.z, cfar8, cnear8), 1.0);
#ifndef TERRAIN_CELLS
    if (tjs) {
        /* A split vertex mixes its base corners' colours from its record:
         * an HQ node its raw colours, then shades at its own depth as the
         * subdivide's nodes do; a flat member the corners' shaded colours,
         * which keeps the flat face's shading. A mix keeps its rounding
         * residual, so the split face interpolates as the unsplit face does;
         * a fogged or water-faded HQ node's colour is the fade's bytes. */
        uint r = tj_rec();
        vec3 fp, np, fq, nq;
        tj_rec_col(r, tjR[tjP], lp, slot, fp, np);
        tj_rec_col(r, tjR[tjQ], lp, slot, fq, nq);
        float tt = float(tjT);
        if (hqv && (waterFar || hpFog)) {
            col.rgb = TJ_SHADE(mo.z, tj_mix8(fp, fq, tt), tj_mix8(np, nq, tt));
        } else {
            vec3 ca = hqv ? np : TJ_SHADE(tjB[tjP].z, fp, np);
            vec3 cb = hqv ? nq : TJ_SHADE(tjB[tjQ].z, fq, nq);
            col.rgb = op_rgb8q(ivec3(round(tj_mix8(ca, cb, tt) * 255.0)), tj_rgbq(ca, cb, tt));
        }
    }
#endif
#undef TJ_SHADE
    {   /* debug tint modes 4/5: show the baked near / far colour raw */
        const int dbgmode = (TER_TINT >> 26) & 7;
        if ((TER_TINT & (1 << 29)) != 0 && dbgmode == 4) col = vec4(op_unorm8(inColNear.rgb), 1.0);
        if ((TER_TINT & (1 << 29)) != 0 && dbgmode == 5) col = vec4(op_unorm8(inColFar.rgb), 1.0);
    }
#endif

    /* ---- position: the feed + psx_scene.vert contract, exactly ----
     * feed: z = pvz = vz with vz = float(m3)*16 (near-guarded at 1);
     * scene VS: vzo = the clamped z, q = vzo/Wu (Wu = the same vz), and
     * the (zc, W) pair uses W == vzo for clip invariance, so window depth
     * and q follow the CPU-fed pipeline's rule. */
    float fvz = float(mo.z) * 16.0;
    float vzs = max(fvz, 1.0);
    float vzo = op_scene_vz(vzs);
    float Wu  = vzs;                       /* inW = pvz = the same view-Z */
    float q   = AFFINE_UV ? 1.0 : (vzo / Wu);
#ifdef TERRAIN_DEPTH_SPEC
    const bool depthRev = DEPTH_REVERSED;
#else
    const bool depthRev = pc.depth_rev != 0;
#endif
    gl_Position = op_scene_clip(clip_of(sscr), vzo, AFFINE_UV, depthRev);
    vUV    = uv * q;
    vColor = col * q;
    vQ     = q;
    vKfl = mfl * q;      /* p interpolates as the UV does (psx_key_contest.glsl) */
    vKd  = kd * q;
#ifdef TERRAIN_READBACK
    rb_emit(rbk, slot);
#endif
}
