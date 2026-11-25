/**
 * @file fog.frag
 * @brief Write one of four colours that measure the engine's distance fade.
 *
 * mode.x selects: 0 base unchanged; 1 op_psx_modulate() of the fragment's
 * own texel and op_color() (op_color() on an untextured draw); 2 the flat
 * colour mode.yzw; 3 op_color(). Each keeps base.a, so a transparent texel
 * stays a hole. The engine fades the result after this function returns.
 */
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

/** @brief The [[material]] row's params, as std140. */
layout(set = 3, binding = 1, std140) uniform op_instance {
    vec4 mode;     /**< x = what to write, yzw = the flat colour */
};

vec4 op_material(vec4 base)
{
    int m = int(mode.x + 0.5);
    if (m == 0) return base;
    if (m == 1) {
#ifdef OP_PROFILE_TERRAIN
        return base;
#else
        if (op_pc.bpp == 0) return vec4(op_color().rgb, base.a);
        vec4 t = op_psx_texel(ivec2(op_uv()));
        return vec4(op_psx_modulate(t.rgb, op_color().rgb), base.a);
#endif
    }
    return vec4(m == 2 ? mode.yzw : op_color().rgb, base.a);
}
