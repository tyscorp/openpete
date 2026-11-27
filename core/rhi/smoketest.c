/*
 * rhi_smoketest - standalone RHI verification.
 *
 * Opens a Vulkan window and, over a cycling clear color, draws:
 *   - three quads sampled from a synthetic PSX VRAM test pattern
 *     through the ported 4/8/16bpp + CLUT decoder (shaders/psx_sample.*):
 *       left   = 16bpp direct  (red->right / green->down gradient)
 *       middle =  8bpp + 256-entry CLUT (grayscale ramp left->right)
 *       right  =  4bpp +  16-entry CLUT (16 color bars)
 *   - a vertex-colored reference quad (the non-VRAM pipeline path).
 *
 * Proves the whole foundation in isolation: instance/device/swapchain,
 * offscreen target, clear/blit/present, vertex buffers, pipelines, the
 * R16_UINT VRAM image + sampler descriptor set + fragment push constants,
 * and the PSX texture decode.
 *
 * Run:   rhi_smoketest [--frames N] [--headless]
 *   --frames N   exit after N presented frames (for non-interactive CI);
 *                otherwise runs until the window is closed or Esc.
 *   --headless   run the self-checking cases on a surfaceless device and
 *                exit 0 on pass, 1 on a failed case, 77 without a device:
 *                the vertex stage reads its positions and colours from a
 *                retained storage buffer (rhi_storage, set 2), and each
 *                frame's readback must equal the expected image exactly;
 *                a compute pass writes words read back to the host and the
 *                vertex records a draw of the same present reads, both
 *                compared exactly (skipped where compute is unavailable).
 *                A mip-chained image (rhi_create_rgba8_mipped) drawn at 1:1
 *                must equal the linear sampler's result, and drawn
 *                minified must alias no more than the linear sampler
 *                (a 1-texel checker over a low band, measured against the
 *                band); OPENPETE_SMOKE_MIP_TIMING adds the chain's build
 *                and upload cost per image size.
 *   --compute-control  the compute case alone with a perturbed input, whose
 *                every word and present must mismatch: exit 0 when detected.
 */

#include "rhi.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VRAM_W 1024
#define VRAM_H 512
#define TS       64   /* test texture size (texels per side) */

/* Per-draw PSX material - must match the push_constant block in
 * shaders/psx_sample.frag (five tightly-packed 32-bit ints). */
typedef struct {
    int32_t tpage_x, tpage_y; /* texture page origin (VRAM 16-bit-pixel units) */
    int32_t clut_x,  clut_y;  /* CLUT origin (paletted modes only)             */
    int32_t bpp;              /* 4, 8, or 16                                   */
} psx_material;

/** @brief Pack 5-bit r, g, b and a 1-bit stp flag into a PSX 16-bit texel.
 *
 * Each channel is masked to its low 5 bits: r in bits 0-4, g in bits 5-9,
 * b in bits 10-14; the low bit of stp goes to bit 15. */
static uint16_t bgr555(int r, int g, int b, int stp)
{
    return (uint16_t)((r & 31) | ((g & 31) << 5) | ((b & 31) << 10) | ((stp & 1) << 15));
}

/** @brief Store an 8-bit CLUT index at texel (u,v) of an 8bpp texture in the VRAM mirror.
 *
 * Two indices share one 16-bit VRAM word, the even texel in the low byte:
 * the PSX packing the sampler decodes. The other byte of the word is kept.
 * @param vram The 1024x512 VRAM mirror.
 * @param bx Texture origin x in 16-bit VRAM words.
 * @param by Texture origin y in VRAM rows.
 * @param u Texel column.
 * @param v Texel row.
 * @param idx CLUT index; the low 8 bits are stored. */
static void put8(uint16_t *vram, int bx, int by, int u, int v, int idx)
{
    uint16_t *w = &vram[(by + v) * VRAM_W + bx + (u >> 1)];
    if (u & 1) *w = (uint16_t)((*w & 0x00FF) | ((idx & 0xFF) << 8));
    else       *w = (uint16_t)((*w & 0xFF00) |  (idx & 0xFF));
}

/** @brief Store a 4-bit CLUT index at texel (u,v) of a 4bpp texture in the VRAM mirror.
 *
 * Four indices share one 16-bit VRAM word, texel u in nibble u & 3
 * counted from the low end. The other nibbles of the word are kept.
 * @param vram The 1024x512 VRAM mirror.
 * @param bx Texture origin x in 16-bit VRAM words.
 * @param by Texture origin y in VRAM rows.
 * @param u Texel column.
 * @param v Texel row.
 * @param idx CLUT index; the low 4 bits are stored. */
static void put4(uint16_t *vram, int bx, int by, int u, int v, int idx)
{
    uint16_t *w = &vram[(by + v) * VRAM_W + bx + (u >> 2)];
    int sh = 4 * (u & 3);
    *w = (uint16_t)((*w & ~(0xF << sh)) | ((idx & 0xF) << sh));
}

/** @brief Write the six vertices of an axis-aligned quad as two triangles.
 *
 * Triangles tl-tr-br and tl-br-bl, z 0, all with one modulate colour.
 * @param o Output array of six vertices.
 * @param x0 Left edge in clip space.
 * @param y0 Top edge in clip space.
 * @param x1 Right edge in clip space.
 * @param y1 Bottom edge in clip space.
 * @param u0 Left texture coordinate in texels.
 * @param v0 Top texture coordinate in texels.
 * @param u1 Right texture coordinate in texels.
 * @param v1 Bottom texture coordinate in texels.
 * @param rgba Modulate colour. */
static void make_quad(rhi_vertex *o, float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, uint32_t rgba)
{
    rhi_vertex tl = { x0, y0, 0.0f, u0, v0, rgba, 0, .w = 0.0f };
    rhi_vertex tr = { x1, y0, 0.0f, u1, v0, rgba, 0, .w = 0.0f };
    rhi_vertex br = { x1, y1, 0.0f, u1, v1, rgba, 0, .w = 0.0f };
    rhi_vertex bl = { x0, y1, 0.0f, u0, v1, rgba, 0, .w = 0.0f };
    o[0] = tl; o[1] = tr; o[2] = br;
    o[3] = tl; o[4] = br; o[5] = bl;
}

/** @brief Fill the VRAM mirror with one 64x64 test texture per sampler depth and their CLUTs.
 *
 * 16bpp direct at words (0,0): red rises with u, green with v, blue fixed
 * at 1 so no texel is the transparent all-zero value. 8bpp at words (64,0):
 * index ramps 0..255 across u into a 256-entry grey CLUT at row 256, whose
 * blue channel has bit 0 set so entry 0 is not zero. 4bpp at words (128,0):
 * index u * 16 / 64 into a 16-colour bar CLUT at row 257.
 * @param vram The zeroed 1024x512 VRAM mirror. */
static void build_vram(uint16_t *vram)
{
    /* 16bpp texture at tpage (0,0): R across, G down, a dim blue floor so
     * the (0,0) corner isn't the transparent sentinel. */
    for (int v = 0; v < TS; v++)
        for (int u = 0; u < TS; u++)
            vram[v * VRAM_W + u] = bgr555(u * 31 / (TS - 1), v * 31 / (TS - 1), 1, 0);

    /* 8bpp texture at tpage (64,0) + 256-entry grayscale CLUT at (0,256).
     * idx ramps 0..255 across u; CLUT entry 0 gets a blue floor (non-zero). */
    for (int i = 0; i < 256; i++) {
        int g = i >> 3;
        vram[256 * VRAM_W + i] = bgr555(g, g, g | 1, 0);
    }
    for (int v = 0; v < TS; v++)
        for (int u = 0; u < TS; u++)
            put8(vram, 64, 0, u, v, u * 255 / (TS - 1));

    /* 4bpp texture at tpage (128,0) + 16 color bars CLUT at (0,257). */
    static const unsigned char bars[16][3] = {
        {31,0,0},{31,15,0},{31,31,0},{15,31,0},{0,31,0},{0,31,15},{0,31,31},{0,15,31},
        {0,0,31},{15,0,31},{31,0,31},{31,0,15},{31,31,31},{20,20,20},{10,10,10},{31,16,8},
    };
    for (int i = 0; i < 16; i++)
        vram[257 * VRAM_W + i] = bgr555(bars[i][0], bars[i][1], bars[i][2], 0);
    for (int v = 0; v < TS; v++)
        for (int u = 0; u < TS; u++)
            put4(vram, 128, 0, u, v, u * 16 / TS);
}

/* ---- headless case: vertex positions from a retained storage buffer ---- */

#define SC_W 256
#define SC_H 128

/* One storage record of storage_pull.vert: clip xy in pos, RGBA colour. */
typedef struct { float pos[4]; float color[4]; } sc_vert;

/* An axis-aligned rectangle in clip space and the pixel span it covers on
 * the SC_W x SC_H target. The clip edges sit on pixel edges, so coverage is
 * exact: columns [px0, px1) and rows [py0, py1). */
typedef struct {
    float x0, y0, x1, y1;
    int   px0, py0, px1, py1;
    uint8_t rgb[3];
} sc_rect;

static const sc_rect k_sc_red   = { -0.5f, -0.5f, 0.0f,  0.5f,  64, 32, 128, 96, { 255, 0, 0 } };
static const sc_rect k_sc_green = { 0.25f, -0.5f, 0.75f, 0.5f, 160, 32, 224, 96, { 0, 255, 0 } };

/** @brief Write the six storage records of one rectangle as two triangles.
 *
 * @param o Six records.
 * @param r Rectangle and its colour. */
static void sc_fill(sc_vert *o, const sc_rect *r)
{
    const float xy[6][2] = {
        { r->x0, r->y0 }, { r->x1, r->y0 }, { r->x1, r->y1 },
        { r->x0, r->y0 }, { r->x1, r->y1 }, { r->x0, r->y1 },
    };
    for (int i = 0; i < 6; i++) {
        o[i] = (sc_vert){ { xy[i][0], xy[i][1], 0.0f, 1.0f },
                          { r->rgb[0] / 255.0f, r->rgb[1] / 255.0f, r->rgb[2] / 255.0f, 1.0f } };
    }
}

/** @brief Return the colour a pixel must have: the first listed rectangle covering it, else black.
 *
 * @param rects Rectangles drawn in the frame.
 * @param n Their count.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param out Receives R, G, B. */
static void sc_expect(const sc_rect *const *rects, int n, int x, int y, uint8_t out[3])
{
    out[0] = out[1] = out[2] = 0;
    for (int i = 0; i < n; i++) {
        const sc_rect *r = rects[i];
        if (x >= r->px0 && x < r->px1 && y >= r->py0 && y < r->py1) {
            memcpy(out, r->rgb, 3);
            return;
        }
    }
}

/** @brief Compare the offscreen readback with the expected image of a set of rectangles.
 *
 * The rectangles do not overlap, so the image is independent of draw order
 * and of the viewport's Y flip (each rectangle is symmetric about y = 0).
 * @param dev Device after the frame was submitted.
 * @param name Case name for the report line.
 * @param rects Rectangles the frame drew.
 * @param n Their count.
 * @param drawn Receives the number of non-black pixels read back.
 * @return true when every pixel matches. */
static bool sc_check(rhi_device *dev, const char *name, const sc_rect *const *rects, int n,
                     long *drawn)
{
    static uint8_t px[SC_W * SC_H * 4];
    int w = 0, h = 0;
    *drawn = 0;
    if (!rhi_read_offscreen(dev, px, sizeof px, &w, &h) || w != SC_W || h != SC_H) {
        printf("rhi_smoketest: %s: readback failed (%dx%d)\n", name, w, h);
        return false;
    }
    long bad = 0;
    for (int y = 0; y < SC_H; y++) {
        for (int x = 0; x < SC_W; x++) {
            const uint8_t *p = &px[(y * SC_W + x) * 4];
            uint8_t want[3];
            sc_expect(rects, n, x, y, want);
            *drawn += (p[0] | p[1] | p[2]) != 0;
            bad += memcmp(p, want, 3) != 0;
        }
    }
    long expect = 0;
    for (int i = 0; i < n; i++)
        expect += (long)(rects[i]->px1 - rects[i]->px0) * (rects[i]->py1 - rects[i]->py0);
    const bool ok = bad == 0 && *drawn == expect && *drawn > 0;
    printf("rhi_smoketest: %s: %s drawn=%ld expected=%ld mismatched=%ld\n",
           name, ok ? "PASS" : "FAIL", *drawn, expect, bad);
    return ok;
}

/** @brief Map, fill and commit one rectangle into the retained storage buffer.
 *
 * @param st Buffer of six records.
 * @param r Rectangle. */
static void sc_publish(rhi_storage *st, const sc_rect *r)
{
    sc_vert *v = rhi_storage_map(st);
    if (v) sc_fill(v, r);
    rhi_storage_commit(st);
}

/** @brief Record one draw of the six committed records.
 *
 * @param dev Device inside a frame and the offscreen pass.
 * @param pipe The storage_pull pipeline.
 * @param vb Six-vertex buffer bound for the pipeline's vertex input, which the stage does not read.
 * @param st Buffer. */
static void sc_draw(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb, rhi_storage *st)
{
    rhi_bind_pipeline(dev, pipe);
    rhi_bind_vertex_buffer(dev, vb);
    rhi_storage_bind(dev, st, 2);
    rhi_draw(dev, 6, 0);
}

/** @brief Run the four retained-storage frames and check each readback.
 *
 * Frame 1 commits the red rectangle and draws it. Frame 2 draws without a
 * write and must show the same image, so the contents persist across
 * frames. Frame 3 draws the committed red records, then commits the green
 * rectangle and draws again: both must show, so the same-frame commit
 * left the bytes of the recorded red draw intact. Frame 4 draws without a
 * write and must show green only. Every frame must draw a nonzero pixel
 * count equal to its rectangles' area.
 * @param dev Headless device.
 * @param pipe The storage_pull pipeline.
 * @param vb Six-vertex buffer for the vertex input.
 * @param st Buffer of six records.
 * @param drawn_total Receives the summed non-black pixel count.
 * @return true when every frame matches. */
static bool sc_run_frames(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb,
                          rhi_storage *st, long *drawn_total)
{
    static const sc_rect *const red[] = { &k_sc_red };
    static const sc_rect *const green[] = { &k_sc_green };
    static const sc_rect *const both[] = { &k_sc_red, &k_sc_green };
    static const struct {
        const char *name; int write_before, write_mid;
        const sc_rect *const *rects; int n;
    } k_frames[] = {
        { "storage commit",           1, 0, red,   1 },
        { "storage persist",          0, 0, red,   1 },
        { "storage same-frame write", 0, 1, both,  2 },
        { "storage persist after",    0, 0, green, 1 },
    };
    bool ok = true;
    *drawn_total = 0;
    for (size_t f = 0; f < sizeof k_frames / sizeof k_frames[0]; f++) {
        if (k_frames[f].write_before) sc_publish(st, &k_sc_red);
        if (!rhi_begin_frame(dev)) { printf("rhi_smoketest: begin_frame refused\n"); return false; }
        rhi_begin_offscreen(dev, 0.0f, 0.0f, 0.0f, 1.0f);
        sc_draw(dev, pipe, vb, st);
        if (k_frames[f].write_mid) {
            sc_publish(st, &k_sc_green);
            sc_draw(dev, pipe, vb, st);
        }
        rhi_end_offscreen(dev);
        rhi_end_frame(dev);
        long drawn;
        ok &= sc_check(dev, k_frames[f].name, k_frames[f].rects, k_frames[f].n, &drawn);
        *drawn_total += drawn;
    }
    return ok;
}

/** @brief Run the two frames of the combined image-plus-storage set and check each readback.
 *
 * One storage buffer holds two regions, the red rectangle's records at
 * offset 0 and the green one's at @p off. Frame 1 binds the set at offset
 * 0, draws, binds it at @p off and draws again: both rectangles must show.
 * Frame 2 binds at @p off alone: green only. The fragment stage multiplies
 * by the bound image's texel, a white 1x1, so an unbound image fails the
 * frame.
 * @param dev Headless device.
 * @param pipe The tex_ssbo_set2 pipeline.
 * @param vb Six-vertex buffer for the vertex input.
 * @param tex The white 1x1 image.
 * @param store Storage buffer holding both regions.
 * @param range Bytes one region spans.
 * @param off Byte offset of the second region.
 * @param drawn_total Receives the summed non-black pixel count.
 * @return true when both frames match. */
static bool sc_run_texstore(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb, rhi_texture *tex,
                            rhi_buffer *store, uint32_t range, uint32_t off, long *drawn_total)
{
    static const sc_rect *const green[] = { &k_sc_green };
    static const sc_rect *const both[] = { &k_sc_red, &k_sc_green };
    bool ok = true;
    *drawn_total = 0;
    for (int f = 0; f < 2; f++) {
        if (!rhi_begin_frame(dev)) { printf("rhi_smoketest: begin_frame refused\n"); return false; }
        rhi_begin_offscreen(dev, 0.0f, 0.0f, 0.0f, 1.0f);
        rhi_bind_pipeline(dev, pipe);
        rhi_bind_vertex_buffer(dev, vb);
        if (f == 0) {
            rhi_bind_texture_storage(dev, tex, store, range, 2, 0);
            rhi_draw(dev, 6, 0);
        }
        rhi_bind_texture_storage(dev, tex, store, range, 2, off);
        rhi_draw(dev, 6, 0);
        rhi_end_offscreen(dev);
        rhi_end_frame(dev);
        long drawn;
        ok &= sc_check(dev, f == 0 ? "image+storage two regions" : "image+storage second region",
                       f == 0 ? both : green, f == 0 ? 2 : 1, &drawn);
        *drawn_total += drawn;
    }
    return ok;
}

/** @brief Run the combined image-plus-storage case: pipeline, image and two-region buffer setup, then the frames.
 *
 * @param dev Headless device.
 * @param vb Six-vertex buffer for the vertex input.
 * @return 0 on PASS, 1 on a failed frame or setup failure. */
static int run_texstore(rhi_device *dev, rhi_buffer *vb)
{
    const rhi_pipeline_desc desc = {
        .vert_spv = "storage_pull_tex.vert.spv", .frag_spv = "tex_store.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false, .tex_ssbo_set2 = true,
    };
    rhi_pipeline *pipe = rhi_create_pipeline(dev, &desc);
    rhi_texture *tex = rhi_create_rgba8(dev, 1, 1);
    const uint32_t al = rhi_storage_offset_align(dev);
    const uint32_t range = (uint32_t)(6 * sizeof(sc_vert));
    const uint32_t off = (range + al - 1u) / al * al;
    rhi_buffer *store = rhi_create_storage_buffer_device(dev, (size_t)off + range);
    int rc = 1;
    if (pipe && tex && store) {
        const uint32_t white = 0xFFFFFFFFu;
        rhi_update_rgba8(dev, tex, &white);
        sc_vert v[6];
        sc_fill(v, &k_sc_red);
        rhi_update_buffer_range(store, v, 0, sizeof v);
        sc_fill(v, &k_sc_green);
        rhi_update_buffer_range(store, v, off, sizeof v);
        long drawn = 0;
        const bool ok = sc_run_texstore(dev, pipe, vb, tex, store, range, off, &drawn);
        printf("rhi_smoketest: image+storage set: %s engaged_px=%ld region_offset=%u\n",
               ok && drawn > 0 ? "PASS" : "FAIL", drawn, off);
        rc = ok && drawn > 0 ? 0 : 1;
    } else {
        printf("rhi_smoketest: image+storage pipeline/texture/buffer setup failed\n");
    }
    rhi_wait_idle(dev);
    if (tex) rhi_destroy_texture(dev, tex);
    if (store) rhi_destroy_buffer(dev, store);
    if (pipe) rhi_destroy_pipeline(dev, pipe);
    return rc;
}

/* ---- headless case: a compute pass consumed by readback and by a draw ---- */

#define CC_WORDS  4096u      /* input words per present: 64 workgroups of 64 */
#define CC_FRAMES 6          /* presents; each perturbs the input */

/* Head of the compute stage's input (rhi_smoke.comp OpSmokeIn): the
 * rectangle to emit, then CC_WORDS words. */
typedef struct { float edges[4]; float color[4]; } cc_rect;
typedef struct { uint32_t count, key; } cc_push;

/** @brief Return the compute stage's function of one input word: rhi_smoke.comp op_smoke_fn.
 *
 * @param x Input word.
 * @param key Per-present term.
 * @return The output word, in 32-bit unsigned arithmetic. */
static uint32_t cc_fn(uint32_t x, uint32_t key)
{
    return (x * 2654435761u + key) ^ (x >> 7);
}

/** @brief Return input word i of present f: distinct per word and per present.
 *
 * @param f Present index.
 * @param i Word index.
 * @return The word. */
static uint32_t cc_input(int f, uint32_t i)
{
    return i * 7919u + (uint32_t)f * 104729u + ((i ^ (uint32_t)f) << 20);
}

/** @brief Return the per-present key of present f.
 *
 * @param f Present index.
 * @return The key. */
static uint32_t cc_key(int f)
{
    return 0x9E3779B9u * (uint32_t)(f + 1);
}

/** @brief Return the rectangle present f emits: red on even presents, green on odd.
 *
 * @param f Present index.
 * @return The rectangle. */
static const sc_rect *cc_rect_of(int f)
{
    return (f & 1) ? &k_sc_green : &k_sc_red;
}

/** @brief Fill and commit present f's input: the rectangle record and the words.
 *
 * Under the control the uploaded words have their low bit flipped and the
 * other present's rectangle is uploaded, while the expected values stay
 * those of the unperturbed input, so every compared word and pixel of the
 * changed rectangles must mismatch.
 * @param in Input buffer.
 * @param f Present index.
 * @param control true for the perturbed upload. */
static void cc_publish(rhi_storage *in, int f, bool control)
{
    uint8_t *p = rhi_storage_map(in);
    if (!p) return;
    const sc_rect *r = cc_rect_of(control ? f + 1 : f);
    cc_rect head = { { r->x0, r->y0, r->x1, r->y1 },
                     { r->rgb[0] / 255.0f, r->rgb[1] / 255.0f, r->rgb[2] / 255.0f, 1.0f } };
    memcpy(p, &head, sizeof head);
    uint32_t *w = (uint32_t *)(p + sizeof head);
    for (uint32_t i = 0; i < CC_WORDS; i++) w[i] = cc_input(f, i) ^ (control ? 1u : 0u);
    rhi_storage_commit(in);
}

/** @brief Compare one read-back capture with the values present f must produce.
 *
 * @param got The capture, CC_WORDS words.
 * @param f Present index the capture was queued in.
 * @return The number of mismatched words. */
static long cc_compare(const uint32_t *got, int f)
{
    long bad = 0;
    for (uint32_t i = 0; i < CC_WORDS; i++) bad += got[i] != cc_fn(cc_input(f, i), cc_key(f));
    return bad;
}

/* The compute case's GPU objects. */
typedef struct {
    rhi_compute_pipeline *cpipe;
    rhi_compute_set      *set;
    rhi_storage          *in;
    rhi_gpu_buffer       *vals;
    rhi_gpu_buffer       *verts;
    rhi_readback         *rb;
} cc_objs;

/* The compute case's tallies. */
typedef struct {
    long   bad_words, words, bad_frames, drawn, captures, dispatches;
    double zone_us[CC_FRAMES];
    int    zones;
} cc_tally;

/** @brief Check one capture taken from the ring: its present index from the frame serial and its words.
 *
 * @param t Tallies.
 * @param got The capture.
 * @param serial Frame serial the capture was queued in.
 * @param serial0 Frame serial of present 0. */
static void cc_take(cc_tally *t, const uint32_t *got, uint64_t serial, uint64_t serial0)
{
    const int f = (int)(serial - serial0);
    const long bad = f >= 0 && f < CC_FRAMES ? cc_compare(got, f) : (long)CC_WORDS;
    printf("rhi_smoketest: compute readback present %d: %s mismatched=%ld of %u\n",
           f, bad == 0 ? "PASS" : "FAIL", bad, CC_WORDS);
    t->bad_words += bad;
    t->words += CC_WORDS;
    t->captures++;
}

/** @brief Collect the GPU time of the compute zone from the timer results the frame begin harvested.
 *
 * @param dev Device inside a frame.
 * @param t Tallies. */
static void cc_zones(rhi_device *dev, cc_tally *t)
{
    rhi_gpu_zone_result z[8];
    const int n = rhi_gpu_zone_results(dev, z, 8);
    for (int i = 0; i < n && t->zones < CC_FRAMES; i++)
        if (strcmp(z[i].name, "smoke.compute") == 0) t->zone_us[t->zones++] = z[i].us;
}

/** @brief Record and check one present: input commit, dispatch, readback cycle, draw from the compute output.
 *
 * @param dev Headless device.
 * @param o GPU objects.
 * @param pipe The storage_pull pipeline.
 * @param vb Six-vertex buffer for the vertex input.
 * @param f Present index.
 * @param control true for the perturbed input.
 * @param serial0 Frame serial of present 0, written at present 0.
 * @param t Tallies.
 * @return false when the frame could not begin. */
static bool cc_frame(rhi_device *dev, const cc_objs *o, rhi_pipeline *pipe, rhi_buffer *vb, int f,
                     bool control, uint64_t *serial0, cc_tally *t)
{
    static uint32_t got[CC_WORDS];
    cc_publish(o->in, f, control);
    if (!rhi_begin_frame(dev)) { printf("rhi_smoketest: begin_frame refused\n"); return false; }
    if (f == 0) *serial0 = rhi_frame_serial(dev);
    cc_zones(dev, t);
    if (rhi_compute_begin(dev)) {
        const cc_push pc = { CC_WORDS, cc_key(f) };
        rhi_gpu_zone_begin(dev, "smoke.compute");
        rhi_compute_bind(dev, o->cpipe, o->set);
        rhi_compute_push(dev, &pc, sizeof pc);
        rhi_compute_dispatch(dev, CC_WORDS / 64u, 1, 1);
        rhi_gpu_zone_end(dev);
        rhi_compute_end(dev);
    }
    t->dispatches += rhi_compute_dispatches(dev);
    uint64_t serial = 0;
    if (rhi_readback_cycle(dev, o->rb, o->vals, 0, CC_WORDS * 4u, got, sizeof got, &serial))
        cc_take(t, got, serial, *serial0);
    rhi_begin_offscreen(dev, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_bind_pipeline(dev, pipe);
    rhi_bind_vertex_buffer(dev, vb);
    rhi_gpu_buffer_bind(dev, o->verts, 2);
    rhi_draw(dev, 6, 0);
    rhi_end_offscreen(dev);
    rhi_end_frame(dev);
    const sc_rect *const want[] = { cc_rect_of(f) };
    long drawn = 0;
    t->bad_frames += !sc_check(dev, f & 1 ? "compute draw (green)" : "compute draw (red)", want, 1,
                               &drawn);
    t->drawn += drawn;
    return true;
}

/** @brief Create the compute case's GPU objects.
 *
 * @param dev Headless device.
 * @param o Receives the objects; members that failed stay NULL.
 * @return true when every object exists. */
static bool cc_create(rhi_device *dev, cc_objs *o)
{
    const rhi_compute_pipeline_desc cd = {
        .comp_spv = "rhi_smoke.comp.spv", .bindings = 3, .push_const_size = sizeof(cc_push),
    };
    memset(o, 0, sizeof *o);
    o->cpipe = rhi_create_compute_pipeline(dev, &cd);
    o->in = rhi_storage_create(dev, sizeof(cc_rect) + CC_WORDS * 4u);
    o->vals = rhi_gpu_buffer_create(dev, CC_WORDS * 4u);
    o->verts = rhi_gpu_buffer_create(dev, 6 * sizeof(sc_vert));
    o->rb = rhi_readback_create(dev, CC_WORDS * 4u);
    if (!o->cpipe || !o->in || !o->vals || !o->verts || !o->rb) return false;
    const rhi_compute_binding b[3] = { { .storage = o->in }, { .gpu = o->vals }, { .gpu = o->verts } };
    o->set = rhi_compute_set_create(dev, o->cpipe, b, 3);
    return o->set != NULL;
}

/** @brief Destroy the compute case's GPU objects after the device is idle.
 *
 * @param dev Device.
 * @param o Objects; NULL members are skipped. */
static void cc_destroy(rhi_device *dev, cc_objs *o)
{
    rhi_compute_set_destroy(dev, o->set);
    rhi_readback_destroy(dev, o->rb);
    rhi_gpu_buffer_destroy(dev, o->verts);
    rhi_gpu_buffer_destroy(dev, o->vals);
    rhi_storage_destroy(dev, o->in);
    rhi_destroy_compute_pipeline(dev, o->cpipe);
}

/** @brief Print the case's summary line and decide its verdict.
 *
 * Without the control: PASS when every capture and every present matched,
 * all CC_FRAMES captures arrived and pixels and dispatches engaged. Under
 * the control: DETECTED when every compared word and every present
 * mismatched.
 * @param t Tallies.
 * @param control true under the control.
 * @return 0 on PASS or DETECTED, 1 otherwise. */
static int cc_verdict(const cc_tally *t, bool control)
{
    double lo = 0.0, hi = 0.0, sum = 0.0;
    for (int i = 0; i < t->zones; i++) {
        lo = i == 0 || t->zone_us[i] < lo ? t->zone_us[i] : lo;
        hi = t->zone_us[i] > hi ? t->zone_us[i] : hi;
        sum += t->zone_us[i];
    }
    printf("rhi_smoketest: compute gpu zone smoke.compute: %d samples min=%.1f mean=%.1f max=%.1f us\n",
           t->zones, lo, t->zones ? sum / t->zones : 0.0, hi);
    const bool engaged = t->captures == CC_FRAMES && t->dispatches == CC_FRAMES && t->words > 0;
    const bool ok = control ? engaged && t->bad_words == t->words && t->bad_frames == CC_FRAMES
                            : engaged && t->bad_words == 0 && t->bad_frames == 0 && t->drawn > 0;
    printf("rhi_smoketest: compute %s: %s dispatches=%ld captures=%ld words=%ld mismatched_words=%ld "
           "mismatched_presents=%ld engaged_px=%ld\n",
           control ? "control" : "case", control ? (ok ? "DETECTED" : "MISSED") : (ok ? "PASS" : "FAIL"),
           t->dispatches, t->captures, t->words, t->bad_words, t->bad_frames, t->drawn);
    return ok ? 0 : 1;
}

/** @brief Run the compute case: CC_FRAMES presents of dispatch, readback and draw from the compute output.
 *
 * Each present commits a new input, dispatches the stage over it, takes
 * the readback queued RHI_FRAMES_IN_FLIGHT presents earlier and draws the
 * rectangle the stage emitted; the last captures are drained after the
 * device is idle. On a device without compute the case is skipped and
 * passes once the pipeline create is shown to refuse.
 * @param dev Headless device.
 * @param pipe The storage_pull pipeline.
 * @param vb Six-vertex buffer for the vertex input.
 * @param control true to upload the perturbed input (cc_publish).
 * @return 0 on PASS (or SKIP, or DETECTED under the control), 1 otherwise. */
static int run_compute(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb, bool control)
{
    if (!rhi_compute_supported(dev)) {
        const rhi_compute_pipeline_desc cd = { .comp_spv = "rhi_smoke.comp.spv", .bindings = 3 };
        rhi_compute_pipeline *p = rhi_create_compute_pipeline(dev, &cd);
        printf("rhi_smoketest: compute case: SKIP compute unavailable (%s); pipeline create %s\n",
               rhi_compute_unsupported_reason(dev), p ? "succeeded (FAIL)" : "refused");
        rhi_destroy_compute_pipeline(dev, p);
        return p ? 1 : 0;
    }
    cc_objs o;
    cc_tally t = { 0 };
    int rc = 1;
    if (cc_create(dev, &o)) {
        uint64_t serial0 = 0;
        bool ran = true;
        for (int f = 0; f < CC_FRAMES && ran; f++) ran = cc_frame(dev, &o, pipe, vb, f, control, &serial0, &t);
        rhi_wait_idle(dev);
        static uint32_t got[CC_WORDS];
        uint64_t serial = 0;
        while (rhi_readback_drain(dev, o.rb, got, sizeof got, &serial)) cc_take(&t, got, serial, serial0);
        rc = ran ? cc_verdict(&t, control) : 1;
    } else {
        printf("rhi_smoketest: compute pipeline/buffer setup failed\n");
    }
    rhi_wait_idle(dev);
    cc_destroy(dev, &o);
    return rc;
}

/* ---- headless case: minification of a mip-chained image ---- */

/* Output rect sizes in eighths of the source: 1:1, then the minification
 * range of a window drawn between full and half size and below it. */
static const int k_mm_eighths[] = { 8, 7, 6, 5, 4, 3 };
#define MM_SCALES ((int)(sizeof k_mm_eighths / sizeof k_mm_eighths[0]))
#define MM_PI 3.14159265358979323846

/** @brief Return the low band of the test image at a source position: 128 plus a 64x32-texel-period sine product of amplitude 50.
 *
 * @param x Source x in texels (texel centres at +0.5).
 * @param y Source y in texels.
 * @return The level. */
static double mm_low(double x, double y)
{
    return 128.0 + 50.0 * sin(2.0 * MM_PI * x / 64.0) * sin(2.0 * MM_PI * y / 32.0);
}

/** @brief Return one source texel of the test image: the low band plus a 1-texel checker of amplitude 60.
 *
 * The checker sits at the source's Nyquist frequency, above the output's
 * at every minified scale, so a correct minification removes it entirely
 * and whatever of it reaches the output is aliasing.
 * @param x Texel column.
 * @param y Texel row.
 * @return The level, rounded. */
static uint8_t mm_texel(int x, int y)
{
    const double v = mm_low(x + 0.5, y + 0.5) + (((x ^ y) & 1) ? 60.0 : -60.0);
    return (uint8_t)(v + 0.5);
}

/** @brief Draw a texture over the top-left ow x oh pixels of the offscreen target and read the target back.
 *
 * @param dev Headless device.
 * @param pipe The blit pipeline.
 * @param vb Six-vertex buffer.
 * @param tex Texture drawn with UV 0..1 over the rect.
 * @param ow Rect width in pixels.
 * @param oh Rect height in pixels.
 * @param px Receives SC_W x SC_H RGBA bytes.
 * @return false when the frame or the readback failed. */
static bool mm_draw(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb, rhi_texture *tex,
                    int ow, int oh, uint8_t *px)
{
    rhi_vertex q[6];
    make_quad(q, -1.0f, 1.0f, -1.0f + 2.0f * (float)ow / SC_W, 1.0f - 2.0f * (float)oh / SC_H,
              0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
    rhi_wait_idle(dev);
    rhi_update_buffer(vb, q, sizeof q);
    if (!rhi_begin_frame(dev)) return false;
    rhi_begin_offscreen(dev, 0.0f, 0.0f, 0.0f, 1.0f);
    rhi_bind_pipeline(dev, pipe);
    rhi_bind_vertex_buffer(dev, vb);
    rhi_bind_vram(dev, tex);
    rhi_draw(dev, 6, 0);
    rhi_end_offscreen(dev);
    rhi_end_frame(dev);
    int w = 0, h = 0;
    return rhi_read_offscreen(dev, px, (size_t)SC_W * SC_H * 4, &w, &h) && w == SC_W && h == SC_H;
}

/** @brief Return the aliasing energy of a readback's rect: the RMS of its red channel less the low band at each pixel's source position.
 *
 * Pixels within two source texels of the rect's edge are left out, where
 * clamp-to-edge addressing, not minification, sets the value.
 * @param px Readback, SC_W x SC_H RGBA.
 * @param w Source width.
 * @param h Source height.
 * @param ow Rect width.
 * @param oh Rect height.
 * @return The RMS in levels. */
static double mm_alias(const uint8_t *px, int w, int h, int ow, int oh)
{
    const double sx = (double)w / ow, sy = (double)h / oh;
    const int mx = (int)(2.0 / sx) + 1, my = (int)(2.0 / sy) + 1;
    double se = 0.0; long n = 0;
    for (int y = my; y < oh - my; y++)
        for (int x = mx; x < ow - mx; x++) {
            const double d = px[(y * SC_W + x) * 4] - mm_low((x + 0.5) * sx, (y + 0.5) * sy);
            se += d * d; n++;
        }
    return n ? sqrt(se / (double)n) : 0.0;
}

/** @brief Draw one scale through both textures and report its line; return whether the scale passes.
 *
 * At 1:1 (eighths 8) the two readbacks must be identical and equal the
 * source (the rect is texel-aligned, so the linear tap lands on texel
 * centres). Minified, the mip chain's aliasing energy must not exceed the
 * linear sampler's by more than half a level (8-bit rounding).
 * @param dev Headless device.
 * @param pipe The blit pipeline.
 * @param vb Six-vertex buffer.
 * @param tex The linear texture, then the mip-chained one.
 * @param src Level 0 of the source, w x h RGBA.
 * @param w Source width.
 * @param h Source height.
 * @param eighths Output size in eighths of the source.
 * @param a Readback scratch for the linear draw, SC_W x SC_H RGBA.
 * @param b Readback scratch for the mip-chained draw, the same size.
 * @param la Receives the linear sampler's aliasing energy.
 * @return true when the scale passes, false on a failed check, frame or readback. */
static bool mm_scale(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb, rhi_texture *const tex[2],
                     const uint8_t *src, int w, int h, int eighths, uint8_t *a, uint8_t *b,
                     double *la)
{
    const int ow = (w * eighths + 4) / 8, oh = (h * eighths + 4) / 8;
    *la = 0.0;
    if (!mm_draw(dev, pipe, vb, tex[0], ow, oh, a) || !mm_draw(dev, pipe, vb, tex[1], ow, oh, b)) {
        printf("rhi_smoketest: mip minify: frame or readback failed\n");
        return false;
    }
    long diff = 0, off_src = 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            diff += memcmp(&a[(y * SC_W + x) * 4], &b[(y * SC_W + x) * 4], 3) != 0;
            off_src += a[(y * SC_W + x) * 4] != src[((size_t)y * w + x) * 4];
        }
    const bool unit = eighths == 8;
    *la = mm_alias(a, w, h, ow, oh);
    const double ma = mm_alias(b, w, h, ow, oh);
    const bool pass = unit ? (diff == 0 && off_src == 0) : ma <= *la + 0.5;
    printf("rhi_smoketest: mip minify %dx%d -> %dx%d: %s aliasing rms linear=%.2f mip=%.2f px_differ=%ld%s\n",
           w, h, ow, oh, pass ? "PASS" : "FAIL", *la, ma, diff,
           unit ? (off_src ? " (1:1 differs from source)" : " (1:1 equals source)") : "");
    return pass;
}

/** @brief Measure one source size through the linear and the mip-chained sampler at every scale.
 *
 * Each scale is checked by mm_scale; the linear sampler must alias by over
 * 5 levels at some minified scale, or the case did not engage.
 * @param dev Headless device.
 * @param pipe The blit pipeline.
 * @param vb Six-vertex buffer.
 * @param w Source width, at most SC_W.
 * @param h Source height, at most SC_H.
 * @return true when the checks hold. */
static bool mm_case(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb, int w, int h)
{
    uint8_t *src = malloc(rhi_rgba8_mip_chain_bytes((uint32_t)w, (uint32_t)h));
    uint8_t *a = malloc((size_t)SC_W * SC_H * 4), *b = malloc((size_t)SC_W * SC_H * 4);
    rhi_texture *const tex[2] = { rhi_create_rgba8(dev, (uint32_t)w, (uint32_t)h),
                                  rhi_create_rgba8_mipped(dev, (uint32_t)w, (uint32_t)h) };
    bool ok = src && a && b && tex[0] && tex[1];
    for (int i = 0; ok && i < w * h; i++) {
        uint8_t *t = &src[(size_t)i * 4];
        t[0] = t[1] = t[2] = mm_texel(i % w, i / w);
        t[3] = 255;
    }
    if (ok) {
        ok = rhi_rgba8_build_mips(src, (uint32_t)w, (uint32_t)h);
        rhi_texture_set_filter_linear(dev, tex[0]);
        rhi_update_rgba8(dev, tex[0], src);
        rhi_update_rgba8(dev, tex[1], src);
    }
    double engaged = 0.0, la = 0.0;
    for (int i = 0; ok && i < MM_SCALES; i++) {
        ok = mm_scale(dev, pipe, vb, tex, src, w, h, k_mm_eighths[i], a, b, &la);
        if (k_mm_eighths[i] != 8 && la > engaged) engaged = la;
    }
    if (ok && engaged <= 5.0) {
        printf("rhi_smoketest: mip minify %dx%d: VACUOUS linear aliasing peaks at %.2f\n", w, h, engaged);
        ok = false;
    }
    rhi_wait_idle(dev);
    for (int i = 0; i < 2; i++) if (tex[i]) rhi_destroy_texture(dev, tex[i]);
    free(src); free(a); free(b);
    return ok;
}

/** @brief Return the median of five values, sorting them in place.
 *
 * @param t Five values.
 * @return The third smallest. */
static double mm_median5(double t[5])
{
    for (int i = 1; i < 5; i++)
        for (int j = i; j > 0 && t[j] < t[j - 1]; j--) { const double x = t[j]; t[j] = t[j - 1]; t[j - 1] = x; }
    return t[2];
}

/** @brief Print, per image size, the CPU time of building the mip chain and the wall time of a blocking upload with and without it.
 *
 * Outside a frame rhi_update_rgba8 submits a one-shot and waits for it, so
 * the upload time includes the device's copy of every level. Each figure is
 * the median of five runs.
 * @param dev Headless device. */
static void mm_timing(rhi_device *dev)
{
    static const int k_sizes[][2] = { { 853, 432 }, { 640, 432 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
    for (size_t s = 0; s < sizeof k_sizes / sizeof k_sizes[0]; s++) {
        const int w = k_sizes[s][0], h = k_sizes[s][1];
        uint8_t *px = malloc(rhi_rgba8_mip_chain_bytes((uint32_t)w, (uint32_t)h));
        if (!px) return;
        for (size_t i = 0; i < (size_t)w * h * 4; i++) px[i] = (uint8_t)(i * 2654435761u >> 24);
        double build[5], up[2][5];
        for (int r = 0; r < 5; r++) {
            uint64_t t0 = SDL_GetTicksNS();
            rhi_rgba8_build_mips(px, (uint32_t)w, (uint32_t)h);
            build[r] = (double)(SDL_GetTicksNS() - t0) / 1000.0;
            for (int m = 0; m < 2; m++) {
                rhi_texture *tex = m ? rhi_create_rgba8_mipped(dev, (uint32_t)w, (uint32_t)h)
                                     : rhi_create_rgba8(dev, (uint32_t)w, (uint32_t)h);
                t0 = SDL_GetTicksNS();
                rhi_update_rgba8(dev, tex, px);
                up[m][r] = (double)(SDL_GetTicksNS() - t0) / 1000.0;
                rhi_destroy_texture(dev, tex);
            }
        }
        printf("rhi_smoketest: mip chain %dx%d: build=%.0f us upload plain=%.0f us chained=%.0f us\n",
               w, h, mm_median5(build), mm_median5(up[0]), mm_median5(up[1]));
        free(px);
    }
}

/** @brief Run the mip-chain minification case on an even and an odd source size, then print the chain timing under OPENPETE_SMOKE_MIP_TIMING.
 *
 * @param dev Headless device.
 * @param vb Six-vertex buffer.
 * @return 0 on PASS, 1 otherwise. */
static int run_mip_minify(rhi_device *dev, rhi_buffer *vb)
{
    const rhi_pipeline_desc desc = {
        .vert_spv = "psx_sample.vert.spv", .frag_spv = "blit.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = 0,
    };
    rhi_pipeline *pipe = rhi_create_pipeline(dev, &desc);
    if (!pipe) { printf("rhi_smoketest: mip minify pipeline setup failed\n"); return 1; }
    bool ok = mm_case(dev, pipe, vb, SC_W, SC_H);
    ok &= mm_case(dev, pipe, vb, SC_W - 3, SC_H - 1);
    printf("rhi_smoketest: mip minify: %s\n", ok ? "PASS" : "FAIL");
    if (getenv("OPENPETE_SMOKE_MIP_TIMING")) mm_timing(dev);
    rhi_wait_idle(dev);
    rhi_destroy_pipeline(dev, pipe);
    return ok ? 0 : 1;
}

/** @brief Answer the RHI's knob lookups: the environment, with OPENPETE_GPU_TIMERS on unless the environment sets it.
 *
 * @param name Knob name.
 * @return Its value, or NULL when unset. */
static const char *smoke_knob(const char *name)
{
    const char *v = getenv(name);
    if (!v && strcmp(name, "OPENPETE_GPU_TIMERS") == 0) return "1";
    return v;
}

/** @brief Run the retained-storage, image-plus-storage and mip-minification cases.
 *
 * @param dev Headless device.
 * @param pipe The storage_pull pipeline.
 * @param vb Six-vertex buffer for the vertex input.
 * @return 0 when every case passes, 1 otherwise. */
static int run_storage_cases(rhi_device *dev, rhi_pipeline *pipe, rhi_buffer *vb)
{
    rhi_storage *st = rhi_storage_create(dev, 6 * sizeof(sc_vert));
    if (!st) { printf("rhi_smoketest: storage buffer setup failed\n"); return 1; }
    const uint64_t over0 = rhi_ring_gen_overflows();
    long drawn = 0;
    const bool ok = sc_run_frames(dev, pipe, vb, st, &drawn);
    const uint64_t over = rhi_ring_gen_overflows() - over0;
    printf("rhi_smoketest: storage vertex stage: %s engaged_px=%ld overflows=%llu\n",
           ok && over == 0 ? "PASS" : "FAIL", drawn, (unsigned long long)over);
    int rc = ok && over == 0 ? 0 : 1;
    if (run_texstore(dev, vb) != 0) rc = 1;
    if (run_mip_minify(dev, vb) != 0) rc = 1;
    rhi_wait_idle(dev);
    rhi_storage_destroy(dev, st);
    return rc;
}

/** @brief Run the headless self-checking cases, or the compute case under its control alone.
 *
 * GPU timer zones are on unless OPENPETE_GPU_TIMERS says otherwise
 * (smoke_knob), for the compute zone's time.
 * @param compute_control true to run only the compute case with the
 *                        perturbed input, which must be detected.
 * @return 0 when every case passes (or the control is detected), 1 on a
 *         failed case or resource failure, 77 when no Vulkan device can be
 *         created. */
static int run_headless(bool compute_control)
{
    rhi_set_knob_source(smoke_knob);
    rhi_device *dev = rhi_create_headless("openpete-rhi-smoketest", SC_W, SC_H, NULL);
    if (!dev) { printf("rhi_smoketest: no headless device: SKIP\n"); return 77; }
    printf("rhi_smoketest: device='%s' (headless)\n", rhi_device_name(dev));
    const rhi_pipeline_desc desc = {
        .vert_spv = "storage_pull.vert.spv", .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false, .ssbo_set2 = true,
    };
    rhi_pipeline *pipe = rhi_create_pipeline(dev, &desc);
    rhi_buffer *vb = rhi_create_vertex_buffer(dev, 6 * sizeof(rhi_vertex));
    int rc = 1;
    if (pipe && vb) {
        const rhi_vertex zero[6] = { { 0 } };
        rhi_update_buffer(vb, zero, sizeof zero);
        rc = compute_control ? 0 : run_storage_cases(dev, pipe, vb);
        if (run_compute(dev, pipe, vb, compute_control) != 0) rc = 1;
    } else {
        printf("rhi_smoketest: pipeline/buffer setup failed\n");
    }
    rhi_wait_idle(dev);
    if (vb) rhi_destroy_buffer(dev, vb);
    if (pipe) rhi_destroy_pipeline(dev, pipe);
    rhi_destroy(dev);
    return rc;
}

/** @brief Write the cycling clear colour of one iteration: a hue ramp over 768 steps, no libm.
 *
 * @param frame Iteration count.
 * @param rgb Receives red, green, blue in 0..1. */
static void hue_ramp(unsigned long frame, float rgb[3])
{
    unsigned p = (unsigned)(frame % 768u);
    unsigned q = p & 255u;
    float up = q / 255.0f, down = (255u - q) / 255.0f;
    unsigned seg = p >> 8;
    rgb[0] = seg == 0 ? down : seg == 2 ? up : 0.0f;
    rgb[1] = seg == 0 ? up : seg == 1 ? down : 0.0f;
    rgb[2] = seg == 1 ? up : seg == 2 ? down : 0.0f;
}

/** @brief Run the RHI smoke test: draw PSX-sampled and vertex-coloured quads until quit.
 *
 * Opens a resizable Vulkan window, fills a VRAM mirror with a 16bpp gradient,
 * an 8bpp grey ramp and a 4bpp colour-bar CLUT test pattern, and each
 * iteration draws three quads sampled through the PSX material push constant
 * (16, 8 and 4 bpp) and one vertex-coloured reference quad into an offscreen
 * target cleared to a cycling hue, then blits it to the swapchain. Exits on
 * window close or Escape, or after @p frame_limit presented frames.
 * @param frame_limit Presented frames to run; negative runs until quit.
 * @return 0 on a clean run, 1 when SDL, the device or the GPU resources fail
 *         to initialise. */
static int run_windowed(long frame_limit)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    rhi_loader_env_prepare(NULL);   /* before SDL_WINDOW_VULKAN loads the loader */
    SDL_Window *win = SDL_CreateWindow("openpete rhi smoketest", 1280, 720,
                                       SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    rhi_device *dev = rhi_create(win, "openpete-rhi-smoketest", /*vsync=*/-1, NULL);
    if (!dev) {
        fprintf(stderr, "rhi_create failed\n");
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    printf("rhi_smoketest: device='%s'\n", rhi_device_name(dev));
    fflush(stdout);

    /* VRAM model + test pattern. */
    uint16_t *mirror = calloc((size_t)VRAM_W * VRAM_H, sizeof(uint16_t));
    rhi_texture *vram = rhi_create_vram(dev, VRAM_W, VRAM_H);
    if (!mirror || !vram) {
        fprintf(stderr, "rhi_smoketest: VRAM setup failed\n");
        free(mirror); rhi_destroy(dev); SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    build_vram(mirror);
    rhi_update_vram(dev, vram, mirror);

    /* Three sampled quads (top) sharing one buffer; one reference quad
     * (bottom). Y is up (the offscreen viewport is Y-flipped). */
    rhi_vertex tex_verts[18];
    make_quad(&tex_verts[0],  -0.95f, 0.92f, -0.35f, 0.18f, 0, 0, TS, TS, 0xFFFFFFFFu);
    make_quad(&tex_verts[6],  -0.30f, 0.92f,  0.30f, 0.18f, 0, 0, TS, TS, 0xFFFFFFFFu);
    make_quad(&tex_verts[12],  0.35f, 0.92f,  0.95f, 0.18f, 0, 0, TS, TS, 0xFFFFFFFFu);
    rhi_buffer *tex_vb = rhi_create_vertex_buffer(dev, sizeof(tex_verts));

    const rhi_vertex ref[6] = {
        { -0.45f, -0.18f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0, .w = 0.0f }, /* red    */
        {  0.45f, -0.18f, 0.0f, 1.0f, 0.0f, 0xFF00FF00u, 0, .w = 0.0f }, /* green  */
        {  0.45f, -0.92f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0, .w = 0.0f }, /* blue   */
        { -0.45f, -0.18f, 0.0f, 0.0f, 0.0f, 0xFF0000FFu, 0, .w = 0.0f }, /* red    */
        {  0.45f, -0.92f, 0.0f, 1.0f, 1.0f, 0xFFFF0000u, 0, .w = 0.0f }, /* blue   */
        { -0.45f, -0.92f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0, .w = 0.0f }, /* white  */
    };
    rhi_buffer *ref_vb = rhi_create_vertex_buffer(dev, sizeof(ref));

    rhi_pipeline_desc tex_desc = {
        .vert_spv = "psx_sample.vert.spv", .frag_spv = "psx_sample.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
        .uses_vram = true, .push_const_size = sizeof(psx_material),
    };
    rhi_pipeline_desc ref_desc = {
        .vert_spv = "quad.vert.spv", .frag_spv = "quad.frag.spv",
        .blend = RHI_BLEND_NONE, .depth_test = false,
    };
    rhi_pipeline *tex_pipe = rhi_create_pipeline(dev, &tex_desc);
    rhi_pipeline *ref_pipe = rhi_create_pipeline(dev, &ref_desc);
    if (!tex_vb || !ref_vb || !tex_pipe || !ref_pipe) {
        fprintf(stderr, "rhi_smoketest: pipeline/buffer setup failed\n");
        rhi_destroy(dev); SDL_DestroyWindow(win); SDL_Quit();
        return 1;
    }
    rhi_update_buffer(tex_vb, tex_verts, sizeof(tex_verts));
    rhi_update_buffer(ref_vb, ref, sizeof(ref));

    const psx_material mat16 = {   0, 0,   0,   0, 16 };
    const psx_material mat8  = {  64, 0,   0, 256,  8 };
    const psx_material mat4  = { 128, 0,   0, 257,  4 };

    bool running = true;
    unsigned long frame = 0;
    unsigned long presented = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (e.type == SDL_EVENT_WINDOW_RESIZED ||
                       e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                rhi_notify_resize(dev);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
                running = false;
            }
        }
        if (!running) break;

        /* Cycles through the hue wheel so the window is visibly live. */
        float rgb[3];
        hue_ramp(frame, rgb);
        frame++;

        if (!rhi_begin_frame(dev)) {
            /* Swapchain recreating or window minimized; try again. */
            SDL_Delay(8);
            continue;
        }
        rhi_begin_offscreen(dev, rgb[0], rgb[1], rgb[2], 1.0f);

        /* PSX-sampled quads. */
        rhi_bind_pipeline(dev, tex_pipe);
        rhi_bind_vram(dev, vram);
        rhi_bind_vertex_buffer(dev, tex_vb);
        rhi_push_constants(dev, &mat16, sizeof(mat16)); rhi_draw(dev, 6, 0);
        rhi_push_constants(dev, &mat8,  sizeof(mat8));  rhi_draw(dev, 6, 6);
        rhi_push_constants(dev, &mat4,  sizeof(mat4));  rhi_draw(dev, 6, 12);

        /* Vertex-colored reference quad (non-VRAM pipeline). */
        rhi_bind_pipeline(dev, ref_pipe);
        rhi_bind_vertex_buffer(dev, ref_vb);
        rhi_draw(dev, 6, 0);

        rhi_end_offscreen(dev);
        rhi_blit_to_swapchain(dev);
        rhi_end_frame(dev);

        presented++;
        if (presented == 1 || presented % 120 == 0) {
            printf("rhi_smoketest: presented %lu frames\n", presented);
            fflush(stdout);
        }
        if (frame_limit >= 0 && (long)presented >= frame_limit) {
            running = false;
        }
    }

    printf("rhi_smoketest: shutting down after %lu presented frames\n", presented);
    rhi_wait_idle(dev);
    rhi_destroy_pipeline(dev, tex_pipe);
    rhi_destroy_pipeline(dev, ref_pipe);
    rhi_destroy_buffer(dev, tex_vb);
    rhi_destroy_buffer(dev, ref_vb);
    rhi_destroy_texture(dev, vram);
    rhi_destroy(dev);
    free(mirror);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

/** @brief Run the smoke test in the mode the arguments select.
 *
 * `--headless` runs the self-checking cases of run_headless;
 * `--compute-control` runs the compute case under its control; otherwise
 * the windowed run, `--frames N` bounding its presented frames.
 * @param argc Argument count.
 * @param argv Arguments.
 * @return run_headless's status for the headless modes, else run_windowed's. */
int main(int argc, char **argv)
{
    long frame_limit = -1; /* -1 = run until quit */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--headless") == 0) return run_headless(false);
        if (strcmp(argv[i], "--compute-control") == 0) return run_headless(true);
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frame_limit = strtol(argv[++i], NULL, 10);
    }
    return run_windowed(frame_limit);
}
