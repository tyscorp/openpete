/* mod_material_link_vk.c - the Vulkan leg of mod_material_link_test (see
 * mod_material_link_vk.h). The RHI logs each validation message to stderr
 * (its debug-utils callback), so every create runs with stderr redirected
 * to a file that is then read back and counted. */

#include "mod_material_link_vk.h"
#include "rhi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

static rhi_device* s_dev;
#ifndef _WIN32
static int s_saved_fd = -1;
static const char* const k_capture = "mod_material_link_vk.stderr";
#endif

/* Push block sizes of the engine's material pipelines: psx_material_hd
 * (native_render.c, 120 B), tc_pc (native_terrain_cache.c, 116 B) and
 * shadow_decal_tex_pc (native_render.c, 128 B). */
enum { PC_MATERIAL = 120, PC_TERRAIN = 116, PC_DECAL = 128 };

/* The retained-terrain arena families in OPENPETE_MATERIAL_TER_VARIANTS
 * order (plain, anim, near, anim cell records, near cell records) and each
 * family's vertex stage: k_tc_vert row 0 and k_tc_vert_dspec in
 * native_terrain_cache.c, the stage a library-linked create takes and the
 * one a whole create compiles. */
enum { TER_VARIANTS = 5, MAX_DESCS = 2 * TER_VARIANTS };
static const char* const k_ter_vert[TER_VARIANTS] = {
    "terrain.vert.spv", "terrain_anim.vert.spv", "terrain_near.vert.spv",
    "terrain_cells_anim.vert.spv", "terrain_cells_near.vert.spv",
};
static const char* const k_ter_vert_whole[TER_VARIANTS] = {
    "terrain_dspec.vert.spv", "terrain_anim_dspec.vert.spv", "terrain_near_dspec.vert.spv",
    "terrain_cells_anim_dspec.vert.spv", "terrain_cells_near_dspec.vert.spv",
};
static int s_cell_pipes;

/** @brief Redirect stderr to the capture file. */
static void capture_begin(void) {
#ifndef _WIN32
    fflush(stderr);
    const int fd = open(k_capture, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    s_saved_fd = dup(2);
    dup2(fd, 2);
    close(fd);
#endif
}

/** @brief Restore stderr and return the captured text (malloc'd; NULL when nothing was captured). */
static char* capture_end(void) {
#ifndef _WIN32
    if (s_saved_fd < 0) return NULL;
    fflush(stderr);
    dup2(s_saved_fd, 2);
    close(s_saved_fd);
    s_saved_fd = -1;
    FILE* f = fopen(k_capture, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = len >= 0 ? (char*)malloc((size_t)len + 1) : NULL;
    const size_t got = buf ? fread(buf, 1, (size_t)len, f) : 0;
    if (buf) buf[got] = '\0';
    fclose(f);
    remove(k_capture);
    return buf;
#else
    return NULL;
#endif
}

/** @brief Count the `Validation Error` messages in captured text and print the first. */
static int count_errors(const char* txt) {
    int n = 0;
    for (const char* p = txt; p && (p = strstr(p, "Validation Error")) != NULL; p++) {
        if (n++ == 0) {
            const char* nl = strchr(p, '\n');
            printf("  first: %.*s\n", nl ? (int)(nl - p) : 300, p);
        }
    }
    return n;
}

int link_vk_open(void) {
#ifdef _WIN32
    return 77;
#else
    setenv("OPENPETE_VK_VALIDATION", "1", 1);
    /* Monolithic pipelines unless the caller asks otherwise: validation
     * layers older than VK_EXT_graphics_pipeline_library reject its create
     * structures and crash in its fragment-part creates, which says nothing
     * about the module. */
    setenv("OPENPETE_PIPELINE_LIBRARY", "0", 0);
    capture_begin();
    s_dev = rhi_create_headless("mod_material_link_test", 64, 64, NULL);
    char* txt = capture_end();
    const int layer = txt && strstr(txt, "validation layer enabled") != NULL;
    free(txt);
    if (!s_dev) { printf("mod_material_link: no Vulkan device - SKIP\n"); return 77; }
    if (!layer) {
        printf("mod_material_link: VK_LAYER_KHRONOS_validation did not load - SKIP\n");
        rhi_destroy(s_dev);
        s_dev = NULL;
        return 77;
    }
    return 0;
#endif
}

/** @brief Fill the descriptions of the pipelines a contract and profile builds, as the renderer builds them.
 *
 * @return The count written. */
static int descs_for(int contract, int profile, rhi_pipeline_desc* d) {
    static const rhi_pipeline_desc k_scene = {
        .vert_spv = "psx_scene.vert.spv", .frag_spv = "psx_scene.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = true, .material_pipeline = true,
        .push_const_size = PC_MATERIAL,
    };
    static const rhi_pipeline_desc k_terrain = {
        .vert_spv = "terrain.vert.spv", .blend = RHI_BLEND_NONE, .depth_test = true,
        .terrain_pipeline = true, .tex_ssbo_set2 = true, .push_const_size = PC_TERRAIN,
    };
    static const rhi_pipeline_desc k_decal = {
        .vert_spv = "shadow_decal_tex.vert.spv", .frag_spv = "shadow_decal_tex.frag.spv",
        .blend = RHI_BLEND_SUBTRACT, .depth_test = false, .wire_scene = true,
        .mod_pipeline = true, .set1_vram_depth = true, .push_const_size = PC_DECAL,
    };
    if (contract == 1) { d[0] = k_decal; return 1; }
    if (profile == 2) { d[0] = k_scene; d[0].uniform_set7 = true; return 1; }
    if (profile == 3) {             /* the actor profile: opaque and its MARK twin */
        d[0] = d[1] = k_scene;
        d[0].ssbo_set7 = d[1].ssbo_set7 = true;  /* the moby snap table */
        d[1].stencil = RHI_STENCIL_MARK;
        return 2;
    }
    if (profile == 1) {             /* openpete_tercache_material_pipe: each variant, slots 0 and 3 */
        for (int v = 0; v < TER_VARIANTS; v++) {
            rhi_pipeline_desc* o = &d[2 * v];
            o[0] = k_terrain;
            o[0].vert_spv = k_ter_vert[v];
            o[0].vert_spv_whole = k_ter_vert_whole[v];
            o[0].terrain_anim = v == 1 || v == 3;
            o[0].terrain_near = v == 2 || v == 4;
            o[0].terrain_cells = v >= 3;
            o[1] = o[0];
            o[1].blend = RHI_BLEND_ALPHA; o[1].depth_write_off = true;
        }
        return MAX_DESCS;
    }
    d[0] = d[1] = d[2] = d[3] = k_scene;
    d[1].blend = RHI_BLEND_ADD; d[1].depth_write_off = true;
    d[2].stencil = RHI_STENCIL_MARK;
    d[3].depth_write_off = true; d[3].stencil = RHI_STENCIL_TEST;   /* sky */
    return 4;
}

int link_vk_pipelines(const uint32_t* w, size_t n, int contract, int profile, int* made,
                      int* errors) {
    *made = *errors = 0;
    if (!s_dev) return -1;
    rhi_pipeline_desc d[MAX_DESCS];
    const int count = descs_for(contract, profile, d);
    int rc = 0;
    capture_begin();
    for (int i = 0; i < count; i++) {
        void* slot = NULL;
        if (rhi_create_pipeline_slot(s_dev, &d[i], NULL, 0, w, n, &slot, NULL) != 0 || !slot) {
            rc = -1;
            continue;
        }
        (*made)++;
        s_cell_pipes += d[i].terrain_cells;
        rhi_destroy_pipeline(s_dev, (rhi_pipeline*)slot);
    }
    char* txt = capture_end();
    *errors = count_errors(txt);
    free(txt);
    return rc;
}

int link_vk_cell_pipelines(void) {
    return s_cell_pipes;
}

void link_vk_close(void) {
    if (s_dev) rhi_destroy(s_dev);
    s_dev = NULL;
}
