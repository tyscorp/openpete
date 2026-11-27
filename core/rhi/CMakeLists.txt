# core/rhi - game-agnostic Render Hardware Interface (thin) + Vulkan backend.
#
# The native renderer's foundation. Lives under core/ because it is both
# game-agnostic and platform-agnostic (windowing/surface via SDL, never a
# platform header).
#
# Vulkan is loaded dynamically via volk (third_party/volk): it dlopens
# libvulkan.so.1 / LoadLibrary("vulkan-1.dll") at runtime, so NO Vulkan
# loader import library is linked on any platform and the Windows
# cross-build needs no Vulkan SDK - only the vendored headers
# (third_party/vulkan-headers). The rhi.h seam keeps Metal/WebGPU additive.

set(VULKAN_HEADERS_INC ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
set(VOLK_DIR           ${CMAKE_SOURCE_DIR}/third_party/volk)

# Shaders: GLSL -> SPIR-V via glslangValidator (a host build tool; .spv is
# portable, so this is identical for native and cross builds).
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
set(RHI_SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(RHI_SHADER_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${RHI_SHADER_OUT_DIR})
set(RHI_SHADERS quad.vert quad.frag quad_key.frag psx_sample.vert psx_scene.vert psx_sample.frag psx_scene.frag psx_scene_key.frag psx_key_cutout.frag key_ident.frag blit.frag freeze_overlay.frag resolve.frag render_res.frag overlay_resolve.frag shadow_decal.frag shadow_decal_tex.vert shadow_decal_tex.frag prepass.vert terrain_key.vert occl_downsample.frag hd_tex.frag hd_tex_cutout.frag postfx_fs.vert raster_quantize.frag psx_dither.frag moby_rank.vert moby_rank.frag moby_rank_cutout.frag moby_shell.frag moby_shell_intr.frag terrain.vert terrain.frag terrain_prepass.frag terrain_prepass_cutout.frag prepass_cutout.frag terrain_key_cache.frag terrain_faceid.frag terrain_readback.frag storage_pull.vert storage_pull_tex.vert tex_store.frag moby_rb.vert)
# Compute stages (rhi_create_compute_pipeline): the smoketest's compute case.
list(APPEND RHI_SHADERS rhi_smoke.comp)
# The launcher shape pass (core/renderer/launcher_shape_pass.c).
list(APPEND RHI_SHADERS launcher_shape.vert launcher_shape.frag)
# The launcher sky pass (core/renderer/launcher_sky_pass.c).
list(APPEND RHI_SHADERS launcher_sky.vert launcher_sky.frag)
# Shared .glsl includes (GL_GOOGLE_include_directive). Every shader DEPENDS
# on all of them: the set is tiny, and a per-shader dependency scan would be
# one more thing to keep in sync with the #include lines.
set(RHI_SHADER_INCLUDES ${RHI_SHADER_SRC_DIR}/psx_clut.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_texel.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_hq_fog.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_key_contest.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_unorm8.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_viewz.glsl
                        ${RHI_SHADER_SRC_DIR}/portal_cover.glsl
                        ${RHI_SHADER_SRC_DIR}/portal_cover_scene.glsl
                        ${RHI_SHADER_SRC_DIR}/terrain_shade.glsl
                        ${RHI_SHADER_SRC_DIR}/prepass_pack.glsl
                        ${RHI_SHADER_SRC_DIR}/moby_shell.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_moby_snap.glsl
                        ${RHI_SHADER_SRC_DIR}/psx_src_tint.glsl
                        ${RHI_SHADER_SRC_DIR}/moby_stage.glsl
                        ${RHI_SHADER_SRC_DIR}/launcher_sky.glsl)
# terrain.vert and its four variants are rewritten to SSA form after
# glslangValidator: every function-local variable whose loads and stores are
# whole-value is promoted to an SSA value (spirv-opt's local single-block and
# single-store elimination, then ssa-rewrite). glslangValidator -V emits each
# local as a Function-storage variable with explicit loads and stores. For
# the cell-record variant the NVIDIA driver compiled that memory form of this
# large stage into code whose output differs between executions of the
# same draw on the same inputs: a record's cells land at wrong positions or
# not at all for one present. The passes replace each load with the value
# last stored (a phi where control flow merges) and drop the stores; no
# arithmetic instruction is added, reordered or re-associated, and
# NoContraction stays on every arithmetic result, so the SSA form is the same
# computation, and the driver compiles it to a stage whose output is a
# function of its inputs. The moby stage (moby_stage.glsl, through its
# readback variant moby_rb.vert) and every compute stage (.comp) take the
# same passes.
find_program(SPIRV_OPT spirv-opt REQUIRED)
set(RHI_TERRAIN_SSA_ARGS --eliminate-local-single-block
                         --eliminate-local-single-store
                         --ssa-rewrite)
set(RHI_SHADER_SPV "")
foreach(s IN LISTS RHI_SHADERS)
    set(post "")
    if(s STREQUAL "terrain.vert" OR s STREQUAL "moby_rb.vert" OR s MATCHES "\\.comp$")
        set(post COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
                         ${RHI_SHADER_OUT_DIR}/${s}.spv -o ${RHI_SHADER_OUT_DIR}/${s}.spv)
    endif()
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR}
                ${RHI_SHADER_SRC_DIR}/${s}
                -o ${RHI_SHADER_OUT_DIR}/${s}.spv
        ${post}
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s} ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s} -> ${s}.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}.spv)
endforeach()
# <stage>_cover.frag: the fragment stage of every CPU-fed blended terrain
# pipeline (alpha, additive, subtractive and quarter-additive, untextured,
# textured and HD replacement) recompiled with the portal plane-cover discard
# (portal_cover_scene.glsl); their layout adds the cover planes' UBO at set 5.
foreach(s IN ITEMS quad_key psx_scene_key quad psx_scene hd_tex)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}_cover.frag.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro OP_PORTAL_COVER=1
                ${RHI_SHADER_SRC_DIR}/${s}.frag
                -o ${RHI_SHADER_OUT_DIR}/${s}_cover.frag.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s}.frag ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s}.frag (OP_PORTAL_COVER) -> ${s}_cover.frag.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}_cover.frag.spv)
endforeach()
# <stage>_rim.frag: the fragment stage of every CPU-fed opaque terrain
# pipeline (WORLD, its stencil-MARK twin, the HD cutouts and the LP hole-fill)
# recompiled with the portal inner-rim discard (portal_cover_scene.glsl,
# OP_PORTAL_RIM); the feed routes the faces it accepts as inner rim through
# them.
foreach(s IN ITEMS quad_key psx_scene_key hd_tex_cutout quad)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}_rim.frag.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro OP_PORTAL_COVER=1
                --define-macro OP_PORTAL_RIM=1
                ${RHI_SHADER_SRC_DIR}/${s}.frag
                -o ${RHI_SHADER_OUT_DIR}/${s}_rim.frag.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s}.frag ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s}.frag (OP_PORTAL_RIM) -> ${s}_rim.frag.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}_rim.frag.spv)
endforeach()
# <stage>_actor.frag: the fragment stage of every opaque ACTOR pipeline
# (plain, stencil-MARK and HD cutout share these stages) recompiled with the
# tie-pull depth export (psx_key_contest.glsl op_actor_tie_depth,
# OP_ACTOR_TIE); same layout as the stock stage.
foreach(s IN ITEMS quad_key psx_scene_key hd_tex_cutout)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}_actor.frag.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro OP_ACTOR_TIE=1
                ${RHI_SHADER_SRC_DIR}/${s}.frag
                -o ${RHI_SHADER_OUT_DIR}/${s}_actor.frag.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s}.frag ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s}.frag (OP_ACTOR_TIE) -> ${s}_actor.frag.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}_actor.frag.spv)
endforeach()
# <stage>_shell.frag: the _actor stage plus the front-shell depth export
# (moby_shell.glsl, OP_MOBY_SHELL); their layout adds the shell image pair at
# set 5.
foreach(s IN ITEMS quad_key psx_scene_key hd_tex_cutout)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}_shell.frag.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro OP_MOBY_SHELL=1
                --define-macro OP_ACTOR_TIE=1
                ${RHI_SHADER_SRC_DIR}/${s}.frag
                -o ${RHI_SHADER_OUT_DIR}/${s}_shell.frag.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s}.frag ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s}.frag (OP_MOBY_SHELL) -> ${s}_shell.frag.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}_shell.frag.spv)
endforeach()
# <stage>_actor_snap.frag / <stage>_shell_snap.frag: the _actor and _shell
# stages plus the sunk-pickup depth snap (psx_moby_snap.glsl, OP_MOBY_SNAP);
# their layout adds the snap table's storage buffer at set 7. <stage>_snapb:
# the blended ACTOR twin, the _cover stage plus the snap's depth export
# (cover planes at set 5, snap table at set 7).
foreach(s IN ITEMS quad_key psx_scene_key hd_tex_cutout)
    foreach(v actor shell)
        set(defs --define-macro OP_ACTOR_TIE=1 --define-macro OP_MOBY_SNAP=1)
        if(v STREQUAL "shell")
            list(APPEND defs --define-macro OP_MOBY_SHELL=1)
        endif()
        add_custom_command(
            OUTPUT ${RHI_SHADER_OUT_DIR}/${s}_${v}_snap.frag.spv
            COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} ${defs}
                    ${RHI_SHADER_SRC_DIR}/${s}.frag
                    -o ${RHI_SHADER_OUT_DIR}/${s}_${v}_snap.frag.spv
            DEPENDS ${RHI_SHADER_SRC_DIR}/${s}.frag ${RHI_SHADER_INCLUDES}
            COMMENT "glslangValidator ${s}.frag (${v}, OP_MOBY_SNAP) -> ${s}_${v}_snap.frag.spv"
            VERBATIM)
        list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}_${v}_snap.frag.spv)
    endforeach()
endforeach()
foreach(s IN ITEMS quad_key psx_scene_key)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${s}_snapb.frag.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro OP_MOBY_SNAP=1
                --define-macro OP_PORTAL_COVER=1
                ${RHI_SHADER_SRC_DIR}/${s}.frag
                -o ${RHI_SHADER_OUT_DIR}/${s}_snapb.frag.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/${s}.frag ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator ${s}.frag (OP_MOBY_SNAP, OP_PORTAL_COVER) -> ${s}_snapb.frag.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${s}_snapb.frag.spv)
endforeach()
# terrain_anim.vert: terrain.vert recompiled with the poly-anim vertex-lerp
# path (second vertex binding = previous-tick positions/corners).
add_custom_command(
    OUTPUT ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv
    COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro TERRAIN_ANIM=1
            ${RHI_SHADER_SRC_DIR}/terrain.vert
            -o ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv
    COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
            ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv -o ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv
    DEPENDS ${RHI_SHADER_SRC_DIR}/terrain.vert ${RHI_SHADER_INCLUDES}
    COMMENT "glslangValidator terrain.vert (TERRAIN_ANIM) -> terrain_anim.vert.spv"
    VERBATIM)
list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/terrain_anim.vert.spv)
# terrain_near.vert: terrain.vert recompiled with the near-plane clip-fan path
# (second vertex binding = the emitted triangle's other two members).
add_custom_command(
    OUTPUT ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv
    COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} --define-macro TERRAIN_NEAR=1
            ${RHI_SHADER_SRC_DIR}/terrain.vert
            -o ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv
    COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
            ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv -o ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv
    DEPENDS ${RHI_SHADER_SRC_DIR}/terrain.vert ${RHI_SHADER_INCLUDES}
    COMMENT "glslangValidator terrain.vert (TERRAIN_NEAR) -> terrain_near.vert.spv"
    VERBATIM)
list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/terrain_near.vert.spv)
# terrain_cells_near.vert / terrain_cells_anim.vert: terrain.vert recompiled
# for the 4x4 near-cell records (one instance-rate binding; each cell vertex
# derived from its record), with the near clip-fan or the poly-anim lerp.
foreach(v near anim)
    string(TOUPPER ${v} V)
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR}
                --define-macro TERRAIN_CELLS=1 --define-macro TERRAIN_${V}=1
                ${RHI_SHADER_SRC_DIR}/terrain.vert
                -o ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
        COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
                ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
                -o ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/terrain.vert ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator terrain.vert (TERRAIN_CELLS, TERRAIN_${V}) -> terrain_cells_${v}.vert.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/terrain_cells_${v}.vert.spv)
endforeach()
# <variant>_ctl.vert: the five terrain vertex stages above with
# TERRAIN_SECDELTA, the positive control of the shared-vertex readback and
# the hole census (OPENPETE_TERRAIN_SECDELTA=1): the camera operand is
# rounded per sector, so a vertex shared by two sectors can round apart.
# <variant>_rb.vert / <variant>_ctlrb.vert: the stock and control stages
# with TERRAIN_READBACK, the shared-vertex position readback
# (OPENPETE_TERRAIN_READBACK); diagnostic only, no shipped configuration
# selects them. <variant>_dspec.vert: the stock stages with
# TERRAIN_DEPTH_SPEC, the depth convention from specialization constant 0
# instead of the push constant; shipped as the stock pipelines' whole-create
# stage (rhi_pipeline_desc.vert_spv_whole), which the background optimized
# pipelines compile. All go through the same SSA passes: the NVIDIA
# memory-form fault is a property of the stage's form.
foreach(vv plain:ctl anim:ctl near:ctl cells_near:ctl cells_anim:ctl
           plain:rb anim:rb near:rb cells_near:rb cells_anim:rb
           plain:ctl:rb anim:ctl:rb near:ctl:rb cells_near:ctl:rb cells_anim:ctl:rb
           plain:dspec anim:dspec near:dspec cells_near:dspec cells_anim:dspec)
    string(REPLACE ":" ";" vp "${vv}")
    list(GET vp 0 v)
    set(defs "")
    set(sfx "")
    if(vv MATCHES ":ctl")
        list(APPEND defs --define-macro TERRAIN_SECDELTA=1)
        set(sfx ctl)
    endif()
    if(vv MATCHES ":rb$")
        list(APPEND defs --define-macro TERRAIN_READBACK=1)
        set(sfx ${sfx}rb)
    endif()
    if(vv MATCHES ":dspec$")
        list(APPEND defs --define-macro TERRAIN_DEPTH_SPEC=1)
        set(sfx dspec)
    endif()
    if(v STREQUAL "plain")
        set(name terrain_${sfx})
    else()
        set(name terrain_${v}_${sfx})
        if(v MATCHES "^cells_")
            list(APPEND defs --define-macro TERRAIN_CELLS=1)
        endif()
        string(REGEX REPLACE "^cells_" "" base ${v})
        string(TOUPPER ${base} V)
        list(APPEND defs --define-macro TERRAIN_${V}=1)
    endif()
    add_custom_command(
        OUTPUT ${RHI_SHADER_OUT_DIR}/${name}.vert.spv
        COMMAND ${GLSLANG_VALIDATOR} -V -I${RHI_SHADER_SRC_DIR} ${defs}
                ${RHI_SHADER_SRC_DIR}/terrain.vert
                -o ${RHI_SHADER_OUT_DIR}/${name}.vert.spv
        COMMAND ${SPIRV_OPT} ${RHI_TERRAIN_SSA_ARGS}
                ${RHI_SHADER_OUT_DIR}/${name}.vert.spv -o ${RHI_SHADER_OUT_DIR}/${name}.vert.spv
        DEPENDS ${RHI_SHADER_SRC_DIR}/terrain.vert ${RHI_SHADER_INCLUDES}
        COMMENT "glslangValidator terrain.vert (${vv}) -> ${name}.vert.spv"
        VERBATIM)
    list(APPEND RHI_SHADER_SPV ${RHI_SHADER_OUT_DIR}/${name}.vert.spv)
endforeach()
# No shader carries fp64: no module declares the Float64 capability (the
# primary check, on the compiled output), and no source line spells an fp64
# type, literal or extension (the secondary check, naming the line). The
# embed depends on both stamps, so a violation fails the build that made it.
set(RHI_FP64_GUARD ${CMAKE_SOURCE_DIR}/scripts/fences/shader_fp64_guard.py)
file(GLOB RHI_SHADER_SOURCES CONFIGURE_DEPENDS
     ${RHI_SHADER_SRC_DIR}/*.vert ${RHI_SHADER_SRC_DIR}/*.frag ${RHI_SHADER_SRC_DIR}/*.comp
     ${RHI_SHADER_SRC_DIR}/*.glsl)
add_custom_command(
    OUTPUT ${RHI_SHADER_OUT_DIR}/fp64_capability.stamp
    COMMAND python3 ${RHI_FP64_GUARD} capability ${RHI_SHADER_OUT_DIR}/fp64_capability.stamp
            ${RHI_SHADER_SPV}
    DEPENDS ${RHI_SHADER_SPV} ${RHI_FP64_GUARD}
    COMMENT "shader fp64 fence: no Float64 capability in any shader"
    VERBATIM)
add_custom_command(
    OUTPUT ${RHI_SHADER_OUT_DIR}/fp64_source.stamp
    COMMAND python3 ${RHI_FP64_GUARD} source ${RHI_SHADER_SRC_DIR}
            ${RHI_SHADER_OUT_DIR}/fp64_source.stamp
    DEPENDS ${RHI_SHADER_SOURCES} ${RHI_FP64_GUARD}
    COMMENT "shader fp64 fence: no fp64 spelling in the shader sources"
    VERBATIM)
# Embed the compiled .spv into the binary: a build-time step
# (embed_shaders.py) turns the SPIR-V into a generated C source (byte arrays
# + a name->blob table), which is compiled into the library. The renderer
# then loads shaders from memory - the binary is self-contained, no loose
# shaders/ dir ships. (A dev/modding file override via OPENPETE_SHADER_DIR
# still reads loose .spv from disk - e.g. this build's ${RHI_SHADER_OUT_DIR}
# - without a relink.)
set(RHI_EMBED_C ${CMAKE_CURRENT_BINARY_DIR}/rhi_shaders_embedded.c)
set(RHI_EMBED_PY ${CMAKE_CURRENT_SOURCE_DIR}/embed_shaders.py)
add_custom_command(
    OUTPUT ${RHI_EMBED_C}
    COMMAND python3 ${RHI_EMBED_PY} ${RHI_EMBED_C} ${RHI_SHADER_SPV}
    DEPENDS ${RHI_SHADER_SPV} ${RHI_EMBED_PY}
            ${RHI_SHADER_OUT_DIR}/fp64_capability.stamp ${RHI_SHADER_OUT_DIR}/fp64_source.stamp
    COMMENT "Embedding SPIR-V shaders -> rhi_shaders_embedded.c"
    VERBATIM)

add_library(openpete_rhi STATIC
    rhi_vulkan.c
    rhi_memtype.c
    rhi_mip.c
    rhi_gpu_audit.c
    rhi_pipe_census.c
    rhi_pcache_file.c
    ${RHI_EMBED_C}
    ${VOLK_DIR}/volk.c
)
target_include_directories(openpete_rhi
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${VULKAN_HEADERS_INC} ${VOLK_DIR}
)
target_compile_definitions(openpete_rhi
    PRIVATE VK_NO_PROTOTYPES)
# volk dlopens the loader; on Linux that needs libdl (CMAKE_DL_LIBS), on
# Windows it uses LoadLibrary (no extra lib).
target_link_libraries(openpete_rhi PUBLIC SDL3::SDL3-static ${CMAKE_DL_LIBS})
# C11: the pipeline compile path counts creates through <stdatomic.h>
# atomics, since a compile job may run on any thread.
set_target_properties(openpete_rhi PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
# The RHI's sources, for a test that compiles them under its own flags
# (tests/unit rhi_compile_stress_tsan, built with ThreadSanitizer).
set_property(GLOBAL PROPERTY OPENPETE_RHI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/rhi_vulkan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rhi_memtype.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rhi_mip.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rhi_gpu_audit.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rhi_pipe_census.c
    ${CMAKE_CURRENT_SOURCE_DIR}/rhi_pcache_file.c
    ${RHI_EMBED_C}
    ${VOLK_DIR}/volk.c)
# The embedded-shader source (RHI_EMBED_C) carries its own dependency on the
# .spv via the custom command above, which in turn depends on the glslang
# outputs - so building openpete_rhi compiles shaders and regenerates the
# embed as needed. No explicit shader target/dependency required.

# RHI smoke test: a standalone Vulkan window that draws three quads sampled
# from a synthetic PSX VRAM pattern and a vertex-colored quad over a cycling
# clear color. Verifies the bringup without the rest of openpete. On by
# default; off via -DOPENPETE_BUILD_RHI_SMOKETEST=OFF.
option(OPENPETE_BUILD_RHI_SMOKETEST "Build the RHI smoke test exe" ON)
if(OPENPETE_BUILD_RHI_SMOKETEST)
    add_executable(rhi_smoketest smoketest.c)
    target_sources(rhi_smoketest PRIVATE ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
    target_include_directories(rhi_smoketest PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(rhi_smoketest PRIVATE openpete_rhi SDL3::SDL3-static)
endif()
