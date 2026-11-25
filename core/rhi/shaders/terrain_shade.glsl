/* terrain_shade.glsl - fragment shading for the retained terrain mesh.
 * Included by the material contract's terrain half; not a mod-facing
 * interface.
 *
 * The retained mesh carries its material per face as flat varyings from
 * the vertex stage (the CLUT-ladder fog row and the near-tile select are
 * camera-dependent face verdicts computed there) instead of the per-draw
 * push constants of the walked scene stages. The stock terrain stage and
 * the material tier both include this file, so they run the same decode,
 * the same key contest and the same debug tints.
 *
 * Contest scope: terrain is never ranked and never an actor, and the moby
 * true-scope is always on for the retained path, so only the terrain-key
 * and prop-key rules apply. */

#ifndef OPENPETE_TERRAIN_SHADE_GLSL
#define OPENPETE_TERRAIN_SHADE_GLSL

#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"
#include "psx_texel.glsl"
#include "psx_hq_fog.glsl"

layout(set = 0, binding = 0) uniform usampler2D u_vram; /* R16_UINT 1024x512 */
layout(set = 1, binding = 0) uniform sampler2D u_hpkey;   /* terrain key image */
/* The terrain key owner's true view-Z and class (op_key_attr_store), the
 * key image's colour attachment. */
layout(set = 1, binding = 1) uniform sampler2D u_hpattr;
layout(set = 2, binding = 0) uniform sampler2D u_mobykey; /* prop true-depth image */
layout(set = 4, binding = 0) uniform sampler2D u_hdatlas; /* HD-fold atlas (NEAREST) */

/* terrain.vert's component-packed output layout. vQ divides vKfl and vKd
 * as it divides vUV. */
layout(location = 0, component = 0)      in vec2  vUV;    /* x vQ (divide by vQ)            */
layout(location = 0, component = 2)      in vec2  vKfl;   /* drain face-local coordinate x vQ */
layout(location = 1)                     in vec4  vColor; /* x vQ (divide by vQ)            */
layout(location = 2)                flat in uvec4 vMat;   /* x: tpx|tpy<<10|bppc<<19|textured<<23,
                                                           * y: clx | cly<<16, zw: HQ ladder-fade
                                                           * words (psx_hq_fog.glsl) */
layout(location = 3, component = 0) flat in float vKoff;  /* OT key (0 = out of contest) */
layout(location = 3, component = 1) flat in float vKcls;  /* the face's terrain class byte */
layout(location = 4, component = 0) flat in uint  vHd;    /* HD-fold entry id | flags */
layout(location = 5, component = 0)      in vec3  vKd;    /* fold position D x vQ */
layout(location = 5, component = 3)      in float vQ;
layout(location = 6)                flat in uvec4 vKc0;   /* drain sub-cell key table */
layout(location = 7)                flat in uvec4 vKc1;
layout(location = 8)                flat in vec4  vKn0;   /* fold corners (psx_key_contest.glsl) */
layout(location = 9)                flat in vec4  vKn1;
layout(location = 10)               flat in vec4  vKn2;

/* Prefix mirror of the vertex stage's TerrainGlobals block. Only the fields
 * this stage reads are named, but every field before them is declared so
 * the layout matches; a field added to the vertex stage's block ahead of
 * `fog`, `hd_lad` or the cover planes must be mirrored here. */
layout(std140, set = 5, binding = 0) uniform TerrainGlobals {
    vec4  mrow0; vec4 mrow1; vec4 mrow2;
    vec4  cam_pad_[2];   /* the camera split (vertex-stage state)          */
    vec4  ofH;           /* xy: the GTE screen offset, z: H                */
    vec4  aux_;          /* vertex-stage state                             */
    ivec4 fog;
    vec4  pad_[264];     /* scroll_dv and the float rim frames (vertex-stage state) */
    vec4  hd_lad;        /* HD fold: continuous-fog ramp ts, tb; count; enable */
    vec4  hd_ent[1664];  /* 416 entries x 4 vec4 (rect/atlas/fogA/fogB)    */
    ivec4 cov_cnt;       /* portal cover planes (portal_cover.glsl)       */
    vec4  cov_margin;
    vec4  cov_abc[6];
    vec4  cov_bbox[6];
    vec4  cov_poly[96];
} g;

/* gl_FragCoord.z convention, matching the vertex stage's selection. */
layout(constant_id = 0) const bool DEPTH_REVERSED = false;
/* The earlier name of DEPTH_REVERSED, kept for mod materials written
 * against it; it reads the pipeline's own depth convention. */
#define DEPTH_AFFINE DEPTH_REVERSED

/* The fragment-visible part of terrain.vert's TerrainPC: the fields up to
 * `snap` are the vertex stage's; `cover` turns on the blended cover discard
 * and `fc` maps gl_FragCoord to PSX screen space for both portal discards.
 * The vertex stage's block continues past `fc` with the material pass's
 * class mask and the depth convention word. */
layout(push_constant) uniform TerrainPC {
    vec4 cbase;
    vec4 cdims;
    int  pass_sel;
    int  full_extent;
    int  mat_pass;
    int  snap;
    int  cover;          /* 1: discard what a portal preview covers        */
    int  pad0;
    uint kflags;         /* OPTC_KEYS_*: 1 the prop key image is all clear,
                          * 2 no actor key owner texel is key-order marked */
    int  pad2;
    vec4 fc;             /* gl_FragCoord -> PSX screen: x = fc.x * fx + fc.y, y = fc.z * fy + fc.w (the portal discards and ter_key_plane_init) */
} ter_pc;

/* Bits 0-7: 1 + the plane index on a face the vertex stage accepted as a
 * portal's inner rim under a plane that discards its rim per fragment
 * (rim_classified); 0 on every other face. Bits 8-29: the face identity
 * word under debug tint mode 7 (terrain.vert attr_word), else 0. */
layout(location = 4, component = 1) flat in uint vRim;

#define OP_COVER_SRC g
#include "portal_cover.glsl"

/* The portal plane-cover rule (portal_cover.glsl) for this fragment: in a
 * blended pass with the cover on, discard at or behind plane - M inside an
 * opening; on an inner-rim face, discard inside the accepting plane's
 * opening at any depth (OP_PORTAL_RIM_MARGIN), where its preview paints. */
bool ter_portal_discards()
{
    const uint rim = vRim & 0xFFu;   /* bits 8-29: tint mode 7's identity */
    if (!((ter_pc.cover != 0 && g.cov_cnt.y != 0) || rim != 0u)) return false;
    const vec2 p = vec2(ter_pc.fc.x * gl_FragCoord.x + ter_pc.fc.y,
                        ter_pc.fc.z * gl_FragCoord.y + ter_pc.fc.w);
    const float m = rim != 0u ? OP_PORTAL_RIM_MARGIN : float(g.cov_margin.x);
    return op_portal_covered(p, op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED), m,
                             int(rim) - 1);
}
/* Fragment spec-constant 4 (`SLOT_CONTEST`): the key contest compares
 * 128-world ordering-table slot indices for terrain against terrain, an
 * equal slot keeping the fragment, and leaves terrain against an actor face
 * to true depth. Set per game from game_layout.terrain.slot_ot_contest. */
layout(constant_id = 4) const bool SLOT_CONTEST = false;
/* Fragment spec-constant 2 (`NOTEX`): the untextured lighting view (and the
 * wireframe view, which bakes it on): a textured face's texel colour is
 * replaced by the modulation-neutral grey, so the output is the gouraud
 * vertex colour; transparent texels still cut out. */
layout(constant_id = 2) const bool NOTEX = false;


uint ter_vram_at(int x, int y) { return texelFetch(u_vram, ivec2(x, y), 0).r; }

float ter_expand5(uint v) { return float((v << 3) | (v >> 2)) / 255.0; }

vec4 ter_decode555(uint p)
{
    if (p == 0u) return vec4(0.0);            /* PSX transparent sentinel */
    return vec4(ter_expand5( p        & 31u),
                ter_expand5((p >> 5)  & 31u),
                ter_expand5((p >> 10) & 31u),
                1.0 - 0.5 * float((p >> 15) & 1u));
}

/* The debug tint word, specialization constant 9 (rhi_terrain_gpu_tint,
 * OPENPETE_TERRAIN_GPU_TINT; terrain.vert declares the same constant): bit
 * 29 on, mode in bits 26-28. 0 in every shipped configuration, so the driver
 * drops ter_debug_shade's and ter_attr's work from the stage. */
layout(constant_id = 9) const int TER_TINT = 0;

/* Debug tints (TER_TINT bit 29, mode in bits 26-28): 0 and 1 the
 * CLUT-ladder verdict, 2, 4 and 5 the raw vertex colour (4 and 5 substitute
 * the baked near or far colour in the vertex stage), 3 a UV visualiser, 6
 * the exact ladder t0k readout. These modes replace the colour before the
 * portal and key-contest discards, so every rasterised fragment that passes
 * the depth test paints. Mode 7 (face attribution) is not answered here:
 * the fragment runs the stock discards and ter_attr replaces the colour of
 * a fragment that survives them. Returns true when a mode answered: the
 * caller writes `c` and stops. */
bool ter_debug_shade(out vec4 c)
{
    c = vec4(0.0);
    if ((TER_TINT & (1 << 29)) == 0) return false;
    const int mode = (TER_TINT >> 26) & 7;
    if (mode == 7) return false;
    if (mode == 6) {                        /* exact ladder t0k readout */
        if ((vMat.y & (1u << 16)) == 0u) {  /* ladder never ran here */
            c = vec4(0.0, 0.0, 1.0, 1.0);
        } else {
            uint enc = (vMat.y >> 18) & 0x1FFFu;   /* t0k + 4096 */
            c = vec4(float((enc >> 8) & 255u) / 255.0,
                     float(enc & 255u) / 255.0,
                     float(((vMat.y >> 17) & 1u) * 128u
                           + ((vMat.y >> 9) & 0x7Fu)) / 255.0, 1.0);
        }
        return true;
    }
    if (mode == 2 || mode == 4 || mode == 5) {  /* raw (or substituted) colour */
        c = vec4((vColor / vQ).rgb, 1.0);
    } else if (mode == 3) {                 /* UV visualiser (texel/256) */
        vec2 uvq = vUV / vQ;
        c = vec4(fract(uvq.x / 256.0), fract(uvq.y / 256.0), 0.0, 1.0);
    } else {                                /* ladder verdict */
        uint half_ = (vMat.y >> 30) & 1u;
        uint dlt   = (vMat.y >> 25) & 31u;
        c = vec4(float(half_), float(dlt) / 15.0,
                 (vMat.x & (1u << 23)) != 0u ? 0.0 : 1.0, 1.0);
    }
    return true;
}

/* Debug tint mode 7: the colour of a fragment that survived the stock
 * discards, encoding the 24-bit word R | G << 8 | B << 16 = the face
 * identity (terrain.vert attr_word, bits 0-21) | the draw pass << 22
 * (pass_sel: 0 WORLD, 1 FILL; 3 for a material pass). Exact in an 8-bit
 * target; a blended arena's draw mixes it with the destination. Returns
 * false outside mode 7. */
bool ter_attr(out vec4 c)
{
    c = vec4(0.0);
    if ((TER_TINT & (1 << 29)) == 0 || ((TER_TINT >> 26) & 7) != 7) return false;
    uint pass = ter_pc.mat_pass > 0 ? 3u : uint(ter_pc.pass_sel) & 3u;
    uint w = ((vRim >> 8) & 0x3FFFFFu) | (pass << 22);
    c = vec4(float(w & 255u), float((w >> 8) & 255u), float((w >> 16) & 255u), 255.0) / 255.0;
    return true;
}

/* The distance from the eye to this fragment's triangle plane (the terrain
 * override's D_F, psx_key_contest.glsl op_key_override_holds), set by
 * ter_key_plane_init at the top of main(), before any discard. */
float ter_key_plane;

/* Set ter_key_plane from the screen derivatives of the fragment's
 * camera-space position: the PSX screen point (ter_pc.fc) placed at its
 * view-Z, x = (sx - ofx) z / H, y = (sy - ofy) z / H. The cross product of
 * the two derivatives is the plane's normal n, and D_F = |n . P| / |n|.
 * A screen derivative needs every lane of the 2x2 quad live. */
void ter_key_plane_init()
{
    const float z = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
    const vec2 s = vec2(ter_pc.fc.x * gl_FragCoord.x + ter_pc.fc.y,
                        ter_pc.fc.z * gl_FragCoord.y + ter_pc.fc.w);
    const vec3 P = vec3((s - g.ofH.xy) * (z / g.ofH.z), z);
    const vec3 n = cross(dFdx(P), dFdy(P));
    const float nl = length(n);
    ter_key_plane = nl > 0.0 ? abs(dot(n, P)) / nl : 0.0;
}

/* The key contest (psx_key_contest.glsl), terrain subset: never ranked,
 * never an actor or a prop, true-scope on, so the actor-key arm receives the
 * never-wins sentinel. A face with a drain sub-cell table contests at the
 * key of the sub-cell under the fragment (op_drain_koff_pf) and loses a
 * fragment in a sub-cell the drain skips. */
bool ter_key_loses()
{
    if (abs(vKoff) <= 0.25) return false;
    float koff = op_drain_koff_pf(vKoff, vKfl / vQ, vKc0, vKc1, vKd / vQ, vKn0.xyz,
                                  vec3(vKn0.w, vKn1.xy), vec3(vKn1.zw, vKn2.x), vKn2.yzw);
    if (op_drain_discards(koff)) return true;
    return op_key_loses(
        op_key_from_stored(texelFetch(u_hpkey,   ivec2(gl_FragCoord.xy), 0).r),
        texelFetch(u_hpattr, ivec2(gl_FragCoord.xy), 0),
        OP_KEY_NONE, OP_KEY_NONE,
        (ter_pc.kflags & 1u) != 0u
            ? OP_KEY_NONE
            : op_key_from_stored(texelFetch(u_mobykey, ivec2(gl_FragCoord.xy), 0).r),
        op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED),
        koff, 0.0, vec2(vKcls, ter_key_plane),
        false, false, true, SLOT_CONTEST);
}

#ifdef OP_TERRAIN_ACTOR_KEY
/* The actor key image and its owner colour (set 7, stock terrain pipelines
 * only: rhi_pipeline_desc.pair_set7). */
layout(set = 7, binding = 0) uniform sampler2D u_actorkey;
layout(set = 7, binding = 1) uniform sampler2D u_actorown;

/* The key-order verdict for this fragment (op_key_order_yields): 0 keep,
 * 1 discard, 2 paint. The owner texel is read first: at an unmarked texel
 * (op_key_order_mark 0, the clear value included) the verdict is 0 whatever
 * the key and the drain sub-cell, so the actor key texel and the drain key
 * are read only at a marked one. A drain sub-cell the face skips never
 * reaches here: ter_key_loses discards it first. */
int ter_key_order()
{
    if (vKoff <= 0.25 || (ter_pc.kflags & 2u) != 0u) return 0;
    vec4 owner = texelFetch(u_actorown, ivec2(gl_FragCoord.xy), 0);
    if (op_key_order_mark(owner) == 0) return 0;
    float koff = op_drain_koff_pf(vKoff, vKfl / vQ, vKc0, vKc1, vKd / vQ, vKn0.xyz,
                                  vec3(vKn0.w, vKn1.xy), vec3(vKn1.zw, vKn2.x), vKn2.yzw);
    return op_key_order_yields(texelFetch(u_actorkey, ivec2(gl_FragCoord.xy), 0).r,
                               owner, koff, SLOT_CONTEST);
}
#endif

/* The texel coordinate's screen gradient (op_texel_grad) for ter_stock_shade,
 * set by ter_texel_grad_init at the top of main(), before any discard. */
vec2 ter_texel_grad;

/* Set ter_texel_grad from this fragment's texel coordinate. Called first in
 * main(): a screen derivative needs every lane of the 2x2 quad live. */
void ter_texel_grad_init()
{
    ter_texel_grad = op_texel_grad(vUV / vQ);
}

/* The stock colour for this fragment. a == 0 marks a texel the PSX treats
 * as transparent (the sentinel, or an HD replacement's cut-out); the
 * caller discards it. Untextured faces (LP, flat-gouraud HP) are the
 * vertex colour at alpha 1, except blend class 1 (mat0 bits 21-22, PSX
 * ABR 0), which takes alpha 128/255 so the alpha arena's SRC_ALPHA blend
 * draws (B+F)/2, the vertex alpha the scene intake stores for the same
 * face on the CPU walk. */
vec4 ter_stock_shade()
{
    uint mat0 = vMat.x;
    vec4 col = vColor / vQ;
    if ((mat0 & (1u << 23)) == 0u)            /* untextured: plain gouraud */
        return vec4(col.rgb, ((mat0 >> 21) & 3u) == 1u ? 128.0 / 255.0 : 1.0);
    /* HD-fold replacement: a baked entry replaces the VRAM fetch (file-box
     * mapping, alpha cut-out, the continuous pack-fog ramp), then the same
     * PSX modulation. Entries exist only on opaque faces. */
    if ((vHd & 0xFFFFu) != 0u) {
        uint ei = ((vHd & 0xFFFFu) - 1u) * 4u;
        vec4 rect  = g.hd_ent[ei];
        vec4 arect = g.hd_ent[ei + 1u];
        vec4 fogA  = g.hd_ent[ei + 2u];
        vec4 fogB  = g.hd_ent[ei + 3u];
        vec2 uvh = vUV / vQ;
        vec2 t = clamp((uvh - rect.xy) / rect.zw, 0.0, 1.0);
        /* Half a texel inside the file's atlas rect: clamp-to-edge without
         * bleeding across the atlas gutter. */
        vec2 ht = 0.5 / arect.zw;
        t = clamp(t, ht, 1.0 - ht);
        vec2 auv = arect.xy + t * arect.zw;
        vec4 hc;
        if (fogA.w > 0.5) {   /* linear-class file: manual bilinear (atlas is NEAREST) */
            vec2 tx = auv - 0.5;
            vec2 fw = fract(tx);
            ivec2 b = ivec2(floor(tx));
            vec4 s00 = texelFetch(u_hdatlas, b, 0);
            vec4 s10 = texelFetch(u_hdatlas, b + ivec2(1, 0), 0);
            vec4 s01 = texelFetch(u_hdatlas, b + ivec2(0, 1), 0);
            vec4 s11 = texelFetch(u_hdatlas, b + ivec2(1, 1), 0);
            hc = mix(mix(s00, s10, fw.x), mix(s01, s11, fw.x), fw.y);
        } else {              /* exact-multiple file: stock-identical fetch */
            hc = texelFetch(u_hdatlas, ivec2(auv), 0);
        }
        if (hc.a < 0.5) return vec4(0.0);
        if (NOTEX) return vec4(clamp(col.rgb, 0.0, 1.0), 1.0);
        if ((vHd & 0x10000u) != 0u && g.hd_lad.x != 0.0) {
            float w = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
            float tt = clamp(w * g.hd_lad.x +
                             (g.hd_lad.y - ((vHd & 0x20000u) != 0u ? 8192.0 / 3840.0 : 0.0)),
                             0.0, 1.0);
            hc.rgb = clamp(hc.rgb * mix(vec3(1.0), fogA.rgb, tt) + fogB.rgb * tt, 0.0, 1.0);
        }
        return vec4(clamp(hc.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0), 1.0);
    }
    int tpx  = int(mat0 & 0x3FFu);
    int tpy  = int((mat0 >> 10) & 0x1FFu) + int(mat0 >> 31) * 512;   /* bit 31: auxiliary bank */
    uint bppc = (mat0 >> 19) & 3u;
    int clx  = int(vMat.y & 0xFFFFu);
    int cly  = int(vMat.y >> 16);

    vec2 uvq = vUV / vQ;
    ivec2 ti = op_texel(uvq, ter_texel_grad);
    int u = ti.x, v = ti.y;
    float vz = op_vz_from_depth(gl_FragCoord.z, DEPTH_REVERSED);
    /* Per-pixel CLUT ladder (psx_hq_fog.glsl): the row at this fragment's
     * depth, lerped toward the next one, inside the CLUT's VRAM bank (a
     * borrowed face's rows sit 512 below). */
    bool lad = !NOTEX && bppc != 2u && op_hq_ladder_on(vMat.z, vMat.w);
    float lpos = lad ? op_hq_ladder_pos(vMat.z, vz, g.hd_lad.x, g.hd_lad.y) : 0.0;
    int lrow = int(floor(lpos));
    uint p;
    int idx = 0;
    if (bppc == 2u) {          /* 16bpp direct */
        p = ter_vram_at(tpx + u, tpy + v);
    } else {
        if (bppc == 1u) {      /* 8bpp CLUT */
            uint word = ter_vram_at(tpx + (u >> 1), tpy + v);
            idx = int((word >> (8 * (u & 1))) & 0xFFu);
        } else {               /* 4bpp CLUT */
            uint word = ter_vram_at(tpx + (u >> 2), tpy + v);
            idx = int((word >> (4 * (u & 3))) & 0xFu);
        }
        p = ter_vram_at(clx + idx, lad ? ((cly & 512) | ((cly + lrow) & 511)) : cly);
    }
    vec4 c = ter_decode555(p);
    if (c.a == 0.0) return vec4(0.0);
    if (lad && lpos > float(lrow)) {
        uint p1 = ter_vram_at(clx + idx, (cly & 512) | ((cly + lrow + 1) & 511));
        if (p1 != 0u) c.rgb = mix(c.rgb, ter_decode555(p1).rgb, lpos - float(lrow));
    }
    if (NOTEX) c.rgb = vec3(128.0 / 255.0);
    else c.rgb = op_hq_fog(c.rgb, vMat.z, vMat.w, vz, g.hd_lad.x, g.hd_lad.y);
    /* PSX texture modulation: texel * vertexColour / 128 (clamped). */
    return vec4(clamp(c.rgb * col.rgb * (255.0 / 128.0), 0.0, 1.0), c.a);
}

#endif /* OPENPETE_TERRAIN_SHADE_GLSL */
