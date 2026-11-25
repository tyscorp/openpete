/* openpete_psx_material.glsl - the material shader contract.
 *
 * A material replaces the colour of geometry the engine has already decided
 * to draw. The engine owns main(); the mod supplies one function:
 *
 *     #version 450
 *     #extension GL_GOOGLE_include_directive : require
 *     #include "openpete_psx_material.glsl"
 *
 *     layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };
 *
 *     vec4 op_material(vec4 base)      // base = stock shading
 *     {
 *         return vec4(base.rgb * tint.rgb, base.a);
 *     }
 *
 * No main(), no output variable, no discard. The engine writes the output
 * and discards the PSX transparent-texel sentinel (base.a == 0); return it
 * unchanged and the hole stays a hole.
 *
 * The engine keeps main() because the stock fragment stage does more than
 * colour maths: the perspective divide and the per-pixel painter verdicts
 * that decide which surface exists at a pixel. A material cannot lose an
 * engine rule it never knew about.
 *
 * base carries: the perspective divide, the 4/8/16bpp VRAM decode through
 * the CLUT, the texel * vertexColour / 128 modulation, and the active HD
 * texture-pack replacement with its fog. Untextured draws arrive as the
 * interpolated vertex colour, so one op_material serves both paths.
 *
 * Distance fade. On a heavy-fog level under extended LOD the engine fades
 * each world moby triangle into the fog: toward the level's fog colour, or
 * toward black on an additive or subtractive draw, by a weight that grows
 * with view depth. The engine applies that fade to op_material()'s result,
 * so a material does nothing about it: op_color() and base arrive without
 * it, and a material returning base draws exactly what the engine draws.
 * The retained-terrain half and every draw the engine does not fade see no
 * change.
 *
 * Fixed by the engine: blend equation, depth test and write, stencil,
 * painter slot, and the occlusion verdicts. A tint on an additive draw
 * therefore reads as added light, and mixing with base is usually the
 * intended result.
 *
 * Not for postfx shaders: set 0 binding 0 is the VRAM texture here and the
 * previous-pass image in openpete_shader_api.glsl. A shader includes one
 * contract or the other.
 *
 * Reserved for the engine: push constants, every descriptor set except
 * set 3 binding 1, and the input locations. The material's own uniform
 * block is set 3 binding 1 (std140).
 *
 * Two engine halves, one mod half. Retained terrain is drawn from GPU
 * arenas with its per-face material in flat varyings and its HD texture
 * pack folded through an atlas, so the engine compiles a material's source
 * twice, once with OP_PROFILE_TERRAIN defined, and this file supplies a
 * matching main() and helpers for each. The mod's source is identical in
 * both, and its colour must be a function of the face alone.
 *
 * The walked scene half is compiled a second time with OP_PROFILE_PORTAL
 * defined, for terrain that must yield to a portal preview (blended terrain
 * behind an opening, a portal's inner rim): its main() discards what the
 * preview covers before the painter verdicts, exactly as the engine's own
 * stages do. The retained-terrain half always runs that rule.
 *
 * The walked scene half is compiled a third time with OP_PROFILE_ACTOR
 * defined, for the opaque moby (ACTOR) group: its main() writes the
 * fragment's depth through op_actor_tie_depth (psx_key_contest.glsl) and
 * op_snap_depth (psx_moby_snap.glsl, its table at set 7), as the engine's
 * own actor stages do, so a moby face whose stored depth carries the tie
 * pull writes its true depth where the terrain key is in a strictly nearer
 * ordering-table slot, and a pickup sunk into the terrain it rests on
 * writes a depth in front of it. No profile adds anything the mod's source
 * sees or writes. */

#ifndef OPENPETE_PSX_MATERIAL_GLSL
#define OPENPETE_PSX_MATERIAL_GLSL

/* The terrain source tint diagnostic (psx_src_tint.glsl), applied by the
 * engine half after op_material(). */
#include "psx_src_tint.glsl"

#ifdef OP_PROFILE_TERRAIN
/* ---- engine half: the retained terrain pipelines --------------------- */

#include "terrain_shade.glsl"

layout(location = 0) out vec4 op_out_color;

/* Perspective-correct texel coords within the texture page. */
vec2 op_uv()    { return vUV / vQ; }
/* Perspective-correct per-vertex modulate colour (R8G8B8A8 -> 0..1). */
vec4 op_color() { return vColor / vQ; }

uint op_vram_at(int x, int y)  { return ter_vram_at(x, y); }
float op_expand5(uint v)       { return ter_expand5(v); }
vec4 op_decode555(uint p)      { return ter_decode555(p); }

/* PSX texture modulation: texel * vertexColour / 128 (128 = unchanged). */
vec3 op_psx_modulate(vec3 texel, vec3 vcol)
{
    return clamp(texel * vcol * (255.0 / 128.0), 0.0, 1.0);
}

/* Raw texel at page-relative texel coords through this face's page and
 * CLUT. An untextured face has no page and returns the transparent
 * sentinel. */
vec4 op_psx_texel(ivec2 uv)
{
    uint mat0 = vMat.x;
    if ((mat0 & (1u << 23)) == 0u) return vec4(0.0);
    int  tpx  = int(mat0 & 0x3FFu);
    int  tpy  = int((mat0 >> 10) & 0x1FFu);
    uint bppc = (mat0 >> 19) & 3u;
    int  clx  = int(vMat.y & 0xFFFFu);
    int  cly  = int(vMat.y >> 16);
    uint p;
    if (bppc == 2u) {          /* 16bpp direct */
        p = ter_vram_at(tpx + uv.x, tpy + uv.y);
    } else if (bppc == 1u) {   /* 8bpp CLUT */
        uint w   = ter_vram_at(tpx + (uv.x >> 1), tpy + uv.y);
        uint idx = (w >> (8 * (uv.x & 1))) & 0xFFu;
        p = ter_vram_at(clx + int(idx), cly);
    } else {                   /* 4bpp CLUT */
        uint w   = ter_vram_at(tpx + (uv.x >> 2), tpy + uv.y);
        uint nib = (w >> (4 * (uv.x & 3))) & 0xFu;
        p = ter_vram_at(clx + int(nib), cly);
    }
    return ter_decode555(p);
}

/* The stock result for this fragment from VRAM alone, without the texture
 * pack. Untextured faces are the vertex colour. */
vec4 op_psx_shade()
{
    vec4 col = op_color();
    if ((vMat.x & (1u << 23)) == 0u) return vec4(col.rgb, 1.0);
    vec4 c = op_psx_texel(ivec2(op_uv()));
    return vec4(op_psx_modulate(c.rgb, col.rgb), c.a);
}

/* True when the active texture pack replaces this face's texture. */
bool op_pack_active() { return (vHd & 0xFFFFu) != 0u; }

/* Raw replacement texel at page-texel coords (op_uv() space), at the pack
 * file's own resolution and filter, with its per-texel alpha and no fog
 * or modulation. Transparent when no replacement covers the face. */
vec4 op_pack_texel(vec2 page_uv)
{
    if ((vHd & 0xFFFFu) == 0u) return vec4(0.0);
    uint ei = ((vHd & 0xFFFFu) - 1u) * 4u;
    vec4 rect  = g.hd_ent[ei];
    vec4 arect = g.hd_ent[ei + 1u];
    vec2 t  = clamp((page_uv - rect.xy) / rect.zw, 0.0, 1.0);
    vec2 ht = 0.5 / arect.zw;      /* clamp-to-edge inside the atlas gutter */
    t = clamp(t, ht, 1.0 - ht);
    return texelFetch(u_hdatlas, ivec2(arect.xy + t * arect.zw), 0);
}

/* The stock result with the active texture pack: what the player sees
 * without this material, and the value passed to op_material(). */
vec4 op_pack_shade() { return ter_stock_shade(); }


/* ---- the mod's half -------------------------------------------------- */
vec4 op_material(vec4 base);

void main()
{
    /* Sector debug tints take precedence over the material: they show what
     * the engine believes, not what a mod paints. */
    ter_texel_grad_init();
    ter_key_plane_init();
    vec4 c;
    if (ter_debug_shade(c)) { op_out_color = c; return; }
    /* The portal plane-cover rule and the painter verdict, as the stock
     * terrain stage runs them. */
    if (ter_portal_discards()) discard;
    if (ter_key_loses()) discard;
    c = op_material(op_pack_shade());
    if (c.a == 0.0) discard;        /* PSX transparent-texel sentinel */
    op_out_color = op_src_tint_cache(c);
    if (ter_attr(c)) op_out_color = c;   /* debug tint mode 7 */
}

#else
/* ---- engine half: the walked scene pipelines ------------------------- */

#include "psx_key_contest.glsl"
#include "psx_clut.glsl"
#include "psx_texel.glsl"
#include "psx_hq_fog.glsl"

layout(set = 0, binding = 0) uniform usampler2D op_vram; /* R16_UINT 1024x512 */

/* The HD texture-pack replacement covering this draw (RGBA8) when
 * op_pc.hd != 0; a 1x1 placeholder otherwise. Sample through op_pack_*. */
layout(set = 1, binding = 0) uniform sampler2D op_pack_tex;

/* Engine occlusion inputs. main() applies them before op_material() runs. */
layout(set = 2, binding = 0) uniform sampler2D op_key_terrain;
/* Drain sub-cell key records (psx_key_contest.glsl op_drain_record), the
 * terrain key image's set at binding 1. */
layout(set = 2, binding = 1) uniform usampler2D op_drain_records;
/* The terrain key owner's true view-Z and class (op_key_attr_store), the
 * terrain key image's colour attachment: the contest bounds the terrain
 * override by the owner's distance from the fragment's plane. */
layout(set = 2, binding = 2) uniform sampler2D op_key_terrain_attr;
layout(set = 4, binding = 0) uniform sampler2D op_key_moby;
layout(set = 5, binding = 0) uniform sampler2D op_key_actor;
/* The moby identity owning each op_key_actor texel (op_akey_id). */
layout(set = 5, binding = 1) uniform sampler2D op_key_actor_owner;
/* The per-pixel actor key image (op_actor_key) and its owner colour. */
layout(set = 5, binding = 2) uniform sampler2D op_key_actor_px;
layout(set = 5, binding = 3) uniform sampler2D op_key_actor_px_owner;
layout(set = 6, binding = 0) uniform sampler2D op_moby_rank;

/* Engine-selected depth convention, key scope and per-game contest rule
 * (ordering-table slot contest, on for Spyro 3); see psx_key_contest.glsl.
 * The engine sets all three per pipeline, as on its own scene stages. The
 * stored value of gl_FragCoord.z follows the convention and is not an
 * interface; op_vz_from_depth returns the fragment's view-Z. */
layout(constant_id = 0) const bool OP_DEPTH_REVERSED = false;
/* The earlier name of OP_DEPTH_REVERSED, kept for shaders written against
 * it; it reads the pipeline's own depth convention. */
#define OP_DEPTH_AFFINE OP_DEPTH_REVERSED
layout(constant_id = 3) const bool OP_MOBY_TRUESCOPE = true;
layout(constant_id = 4) const bool OP_SLOT_CONTEST = false;

#ifdef OP_PROFILE_PORTAL
/* The portal profile: the opening planes ride a block at set 7, above the
 * sets this half reserves (portal_cover_scene.glsl). */
#define OP_COVER_SET 7
#define OP_PORTAL_RUNTIME 1
#include "portal_cover_scene.glsl"
#endif
#ifdef OP_PROFILE_ACTOR
/* The actor profile: the sunk-pickup snap's contact-plane table rides a
 * storage block at set 7, above the sets this half reserves
 * (psx_moby_snap.glsl). */
#include "psx_moby_snap.glsl"
#endif

layout(push_constant) uniform op_psx_material {
    int tpage_x, tpage_y;   /* texture page origin (VRAM 16-bit-pixel units) */
    int clut_x,  clut_y;    /* CLUT origin (VRAM pixel units; paletted only) */
    int bpp;                /* 4, 8, or 16; 0 marks an untextured draw       */
    int hd;                 /* 1 = op_pack_tex holds this draw's replacement  */
    vec2 hd_org;            /* the pack file's texel box within the texture   */
    vec2 hd_size;           /*   page (origin / size, page texel coords)      */
    vec4 hd_fog_a;          /* CLUT-ladder fog affine for the replacement:    */
    vec4 hd_fog_b;          /*   c' = c*a + b (identity when unfogged); .rgb  */
    vec4 fade;              /* distance fade target (.rgb), .a 1 when the     */
                            /*   draw takes the fade, 0 otherwise             */
    vec2 fade_ramp;         /* the fade weight at true view-Z vz: vz * x + y  */
    uint hq_a, hq_b;        /* the draw's texel fade words (psx_hq_fog.glsl)  */
    vec2 lad;               /* the CLUT-ladder ramp the words read: ts, tb    */
} op_pc;

/* The vertex stage emits UV and colour multiplied by the perspective ratio
 * vQ, and vQ itself, so screen-linear interpolation reconstructs
 * perspective-correct values after the divide. Read them through op_uv()
 * and op_color(). */
layout(location = 0) in vec2  vUV;     /* texel coords   x vQ */
layout(location = 1) in vec4  vColor;  /* modulate 0..1  x vQ */
layout(location = 2) in float vQ;      /* perspective ratio   */
layout(location = 3) flat in float vKoff;  /* this face's OT key channel     */
layout(location = 4) flat in vec2  vRank;  /* own walk rank; moby ordinal or identity */
layout(location = 5) flat in float vAkc;   /* moby-against-moby key offset (op_actor_key) */
/* 1 on a walked terrain face under the engine's terrain source tint. */
layout(location = 10) flat in float vSrcWalk;

layout(location = 0) out vec4 op_out_color;

/* ---- distance fade (see the header) -----------------------------------
 * The fade blends a colour x toward the draw's target T by the fragment's
 * weight w: op_fade_(x) = mix(x, T, w). The engine's own stages fade the
 * vertex colour per corner and the texel by the fade words, then modulate;
 * the material inputs are those stock values with the fade taken back out
 * (op_unfade_), and main() fades the material's result, so a material that
 * returns base reproduces the stock fragment. */

/* The fade weight at this fragment: the engine's depth curve at the
 * fragment's true view-Z, 0 where no fade applies. It stops 1/1024 short of
 * 1, where the fade could not be taken back out: the stock per-corner fade
 * can leave a pixel short of the target where the curve at the pixel is
 * past it, and a fully faded colour then sits within a quarter of an 8-bit
 * level of the target. */
float op_fade_weight_()
{
    if (op_pc.fade.a == 0.0) return 0.0;
    float vz = op_vz_from_depth(gl_FragCoord.z, OP_DEPTH_REVERSED) / vQ;
    return clamp(vz * op_pc.fade_ramp.x + op_pc.fade_ramp.y, 0.0, 1.0 - 1.0 / 1024.0);
}

/* x faded toward the target. */
vec3 op_fade_(vec3 x)
{
    return mix(x, op_pc.fade.rgb, op_fade_weight_());
}

/* The x whose fade is y: (y - w T) / (1 - w), unclamped, so op_fade_ of it
 * returns y. */
vec3 op_unfade_(vec3 y)
{
    float w = op_fade_weight_();
    return w <= 0.0 ? y : (y - w * op_pc.fade.rgb) / (1.0 - w);
}

/* Perspective-correct texel coords within the texture page. */
vec2 op_uv()    { return vUV / vQ; }
/* Perspective-correct per-vertex modulate colour (R8G8B8A8 -> 0..1), without
 * the distance fade. */
vec4 op_color() { vec4 c = vColor / vQ; return vec4(op_unfade_(c.rgb), c.a); }

uint op_vram_at(int x, int y) { return texelFetch(op_vram, ivec2(x, y), 0).r; }

/* op_expand5 and op_decode555 (15-bit VRAM word -> RGBA; a: 0 = the PSX
 * transparent sentinel, 0.5 = STP-set texel, 1 = opaque) come from
 * psx_clut.glsl. */

/* Raw texel at page-relative texel coords through this draw's bpp and
 * CLUT. Pass op_uv() for the fragment's own texel. */
vec4 op_psx_texel(ivec2 uv)
{
    return op_decode555(op_texel_word(op_vram, ivec2(op_pc.tpage_x, op_pc.tpage_y),
                                      ivec2(op_pc.clut_x, op_pc.clut_y), op_pc.bpp,
                                      uv.x, uv.y));
}

/* The modulation of a texel the fade words already apply to, as the stock
 * stage computes it from the faded vertex colour, with the fade taken out. */
vec3 op_modulate_drawn_(vec3 texel, vec3 vcol)
{
    return op_unfade_(clamp(texel * op_fade_(vcol) * (255.0 / 128.0), 0.0, 1.0));
}

/* PSX texture modulation: texel * vertexColour / 128 (128 = unchanged), with
 * the draw's texel fade (psx_hq_fog.glsl) applied to the texel as the stock
 * stage applies it. */
vec3 op_psx_modulate(vec3 texel, vec3 vcol)
{
    float vz = op_vz_from_depth(gl_FragCoord.z, OP_DEPTH_REVERSED);
    return op_modulate_drawn_(op_hq_fog(texel, op_pc.hq_a, op_pc.hq_b, vz, op_pc.lad.x, op_pc.lad.y),
                              vcol);
}

/* The stock result for this fragment from VRAM alone, as psx_scene.frag
 * computes it, with the distance fade taken out. An untextured draw (bpp 0)
 * is the vertex colour, with no texel fetch and no modulation. A textured
 * face reads its texel through the per-pixel CLUT ladder when the fade words
 * select it, takes the texel fade, and on the moby fog band between two
 * ticks blends in its second fogged-CLUT row after the modulation
 * (op_clut_fog_mix). */
vec4 op_psx_shade()
{
    vec4 col = op_color();
    if (op_pc.bpp == 0) return vec4(col.rgb, 1.0);
    vec2 uvf = op_uv();
    ivec2 ti = op_texel(uvf, op_texel_grad(uvf));
    float vz = op_vz_from_depth(gl_FragCoord.z, OP_DEPTH_REVERSED);
    bool lad = op_pc.bpp != 16 && op_hq_ladder_on(op_pc.hq_a, op_pc.hq_b);
    float lpos = lad ? op_hq_ladder_pos(op_pc.hq_a, vz, op_pc.lad.x, op_pc.lad.y) : 0.0;
    int lrow = int(floor(lpos));
    ivec2 page = ivec2(op_pc.tpage_x, op_pc.tpage_y);
    vec4 c = op_decode555(op_texel_word(op_vram, page, ivec2(op_pc.clut_x, (op_pc.clut_y + lrow) & 511),
                                        op_pc.bpp, ti.x, ti.y));
    if (c.a == 0.0) return c;
    if (lad && lpos > float(lrow)) {
        uint w1 = op_texel_word(op_vram, page, ivec2(op_pc.clut_x, (op_pc.clut_y + lrow + 1) & 511),
                                op_pc.bpp, ti.x, ti.y);
        if (w1 != 0u) c.rgb = mix(c.rgb, op_decode555(w1).rgb, lpos - float(lrow));
    }
    c.rgb = op_hq_fog(c.rgb, op_pc.hq_a, op_pc.hq_b, vz, op_pc.lad.x, op_pc.lad.y);
    vec4 cf = vec4(op_fade_(col.rgb), col.a);
    vec3 rgb = clamp(c.rgb * cf.rgb * (255.0 / 128.0), 0.0, 1.0);
    rgb = op_clut_fog_mix(op_vram, rgb, cf, page, ivec2(op_pc.clut_x, op_pc.clut_y), op_pc.bpp,
                          ti.x, ti.y);
    return vec4(op_unfade_(rgb), c.a);
}

/* ---- HD texture-pack access ------------------------------------------ */

/* True when the active texture pack replaces this draw's texture and
 * op_pack_tex holds it. False with no pack, no matching file, or a file
 * whose GPU upload has not completed (one present of stock). */
bool op_pack_active() { return op_pc.hd != 0; }

/* Raw replacement texel at page-texel coords (op_uv() space), at the pack
 * file's own resolution and filter, with its per-texel alpha and no fog
 * or modulation. */
vec4 op_pack_texel(vec2 page_uv)
{
    vec2 t = (page_uv - op_pc.hd_org) / op_pc.hd_size;
    return texture(op_pack_tex, clamp(t, 0.0, 1.0));
}

/* The stock result with the active texture pack: what the player sees
 * without this material, and the value passed to op_material(). Falls
 * back to op_psx_shade() when no replacement covers the draw. */
vec4 op_pack_shade()
{
    if (op_pc.hd == 0) return op_psx_shade();
    vec4 c = op_pack_texel(op_uv());
    if (c.a < 0.5) return vec4(0.0);
    c.rgb = clamp(c.rgb * op_pc.hd_fog_a.rgb + op_pc.hd_fog_b.rgb, 0.0, 1.0);
    return vec4(op_modulate_drawn_(c.rgb, op_color().rgb), 1.0);
}

/* ---- the mod's half --------------------------------------------------
 * Supplied by the material. base = op_pack_shade(); return the colour to
 * write, keeping base.a unless the material changes transparency. */
vec4 op_material(vec4 base);

void main()
{
#ifdef OP_PROFILE_ACTOR
    /* The tie-pull depth export of the opaque actor group and the sunk-pickup
     * snap, written before any discard; a discarded fragment writes
     * nothing. */
    {
        float tkey = op_key_from_stored(texelFetch(op_key_terrain, ivec2(gl_FragCoord.xy), 0).r);
        float zout = op_actor_tie_depth(gl_FragCoord.z, vRank, vKoff, tkey,
                                        OP_SLOT_CONTEST, OP_DEPTH_REVERSED);
        gl_FragDepth = op_snap_depth(zout, vRank, vKoff, tkey,
            texelFetch(op_key_terrain_attr, ivec2(gl_FragCoord.xy), 0), OP_DEPTH_REVERSED);
    }
#endif
    /* The painter verdicts, as the stock stages run them: which surface
     * exists at this pixel is the engine's decision. */
#ifdef OP_PROFILE_PORTAL
    if (op_portal_cover_frag(OP_DEPTH_REVERSED)) discard;
#endif
    bool ranked = vRank.x > 0.5;
    float own = op_vz_from_depth(gl_FragCoord.z, OP_DEPTH_REVERSED);
    if (ranked &&
        op_rank_loses(texelFetch(op_moby_rank, ivec2(gl_FragCoord.xy), 0), vRank, own))
        discard;
    float koff = op_drain_koff(vKoff, vRank.y, op_uv(), op_drain_records);
    if (op_drain_discards(koff)) discard;   /* a sub-cell the drain skips */
    if (!ranked && abs(koff) > 0.25) {
        /* The additive glow fan (rank -2) runs the aura contest instead -
         * see psx_key_contest.glsl. */
        if (vRank.x < -1.5) {
            if (op_aura_loses(texelFetch(op_key_actor, ivec2(gl_FragCoord.xy), 0).r,
                              koff - 0.5))
                discard;
        } else if (op_key_loses(
                op_key_from_stored(texelFetch(op_key_terrain, ivec2(gl_FragCoord.xy), 0).r),
                texelFetch(op_key_terrain_attr, ivec2(gl_FragCoord.xy), 0),
                op_akey_contest(op_akey_select(
                    texelFetch(op_key_actor,       ivec2(gl_FragCoord.xy), 0).r,
                    texelFetch(op_key_actor_owner, ivec2(gl_FragCoord.xy), 0),
                    op_akey_owner(vRank))),
                op_akey_contest(op_akey_select(
                    texelFetch(op_key_actor_px,       ivec2(gl_FragCoord.xy), 0).r,
                    texelFetch(op_key_actor_px_owner, ivec2(gl_FragCoord.xy), 0),
                    op_akey_owner(vRank))),
                op_key_from_stored(texelFetch(op_key_moby,    ivec2(gl_FragCoord.xy), 0).r),
                own, koff, vAkc, op_key_fattr_walk(vRank.x),
                vRank.x < -0.5,     /* unranked moby faces carry rank -1 */
                op_rank_is_prop(vRank.x),
                OP_MOBY_TRUESCOPE, OP_SLOT_CONTEST))
            discard;
    }
    vec4 c = op_material(op_pack_shade());
    if (c.a == 0.0) discard;        /* PSX transparent-texel sentinel */
    c.rgb = op_fade_(c.rgb);        /* the distance fade (see the header) */
    op_out_color = op_src_tint_scene(c, vSrcWalk);
}

#endif /* OP_PROFILE_TERRAIN */

#endif /* OPENPETE_PSX_MATERIAL_GLSL */
