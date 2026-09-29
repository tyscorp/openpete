#version 450
#extension GL_GOOGLE_include_directive : require
#include "psx_key_contest.glsl"

/* Terrain KEY pass vertex stage: rasterise opaque keyed terrain with depth =
 * the og OT KEY (true view-Z + 128*slots world) instead of true view-Z, into
 * the depth image the scene fragments contest against
 * (psx_scene_key.frag / quad_key.frag / hd_tex_cutout.frag).
 *
 * The image is PRIVATE, so the encoding is free — and it must be the
 * canonical hyperbolic map z = kv/(kv+4096), NOT the affine 1 - 1/kv:
 * near z==1 the D32 image quantises at ~2^-24 absolute, so an affine-coded
 * key decodes with error ~kv^2 * 2^-25 world units, which crosses the
 * contest's 64-world tie margin at kv ~= 46k — beyond that distance faces
 * failed their OWN key test and whole faces vanished. The canonical map's
 * decode error is (kv+4096)^2/4096 * 2^-25 (~4096x finer at range), <64
 * out past kv ~= 3e6. Keys are FLAT per face (all three verts carry the
 * same kv), so any monotone encoding raster-interpolates exactly; the
 * (zc, W) pair uses W == the key view-Z so window depth survives the
 * hardware clipper (the scene stages' clip-invariance rule).
 *
 * A PLAYER face in the actor batch carries the player band's stored value
 * (OP_PKEY_BASE + slot*OP_PKEY_STEP, pack_player_key_z) as its z instead
 * of an NDC depth, and the stage writes it unchanged as z_ndc = z at w = 1,
 * so it reaches the image bit-exact and op_pkey_is recognises it. Decoded
 * as a depth it would floor to view-Z 1 and store as a real key 1, which
 * wins every actor contest over the player's pixels. A non-band NDC z
 * inside the band's range encodes a key view-Z of about 0.004 to 0.17
 * world units, at the eye, and also stores as a band value. */

layout(location = 0) in vec3 inPos;   /* xy = clip-space screen, z = canonical NDC depth (true vz) or player band value */
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec4 inColor;
layout(location = 4) in float inW;    /* TRUE per-vertex view-Z (textured cutout UV) */
layout(location = 5) in float inKoff; /* +key offset + 0.5 sentinel (HP) */

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) noperspective out vec2 vScreenPSX;
layout(location = 3) out float vQ;

/* PSX-affine UV (--texture-mapping affine, spec-constant 1) — must track the
 * SCENE stages' selection: the cutout fragment's texel lookups have to land on
 * the same texels the scene draw samples, or a key raster stamps where the
 * face's own paint discards (a one-texel hole to the backdrop). The position W
 * here is the FLAT key (per-face), so hardware interpolation is screen-linear;
 * perspective UV is recovered manually via the classic (u/w, 1/w) pair in
 * vUV/vQ (w = the TRUE per-vertex view-Z in inW). */
layout(constant_id = 1) const bool AFFINE_UV = false;

void main()
{
    float z  = clamp(inPos.z, 0.0, 0.9999);
    float vz = 4096.0 * z / max(1.0 - z, 1e-6);    /* view-Z the depth encodes */
    vz = max(vz, 1.0);
    float kv = vz + max(abs(inKoff) - 0.5, 0.0);   /* og key view-Z */
    gl_Position = op_pkey_is(inPos.z)
        ? vec4(inPos.xy, inPos.z, 1.0)                        /* band value, stored as is */
        : vec4(inPos.xy * kv, kv * kv / (kv + 4096.0), kv);   /* z_ndc = kv/(kv+4096) */
    float wq = AFFINE_UV ? 1.0 : max(inW, 1.0);
    vColor = inColor;
    vUV = inUV / wq;
    vScreenPSX = inUV;
    vQ = 1.0 / wq;
}
