/* native_vram.c - see openpete_native_vram.h. The op bodies replicate
 * PsyCross's GR_CopyVRAM / GR_ClearVRAM semantics (masking, clamp ORDER,
 * stride handling, the clear's b<<11 packing quirk) to the byte - any change
 * must re-pass the OPENPETE_VRAM_VERIFY memcmp census against PsyCross. */

#include "openpete_native_vram.h"
#include "texture_pack.h"   /* texture-replacement registry: fed from this op seam */
#include "openpete_fb_snapshot.h" /* framebuffer-window copies: recorded from this op seam */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* PsyCross seam (see PsyX_public.h). Declared here to keep this TU free of
 * PsyCross headers. */
typedef struct PsyX_VRAMOps {
    void (*load) (int x, int y, int w, int h, const unsigned short* src);
    void (*move) (int src_x, int src_y, int w, int h, int dst_x, int dst_y);
    void (*clear)(int x, int y, int w, int h,
                  unsigned char r, unsigned char g, unsigned char b);
} PsyX_VRAMOps;
extern void PsyX_SetVRAMOpCallbacks(const PsyX_VRAMOps* ops);

static uint16_t s_model[(size_t)OPENPETE_VRAM_W * OPENPETE_VRAM_H];

uint16_t* openpete_vram_data(void) { return s_model; }

/* The texels the GPU VRAM texture holds for the scene being composed; see
 * openpete_vram_composed. */
static uint16_t s_composed[(size_t)OPENPETE_VRAM_W * OPENPETE_VRAM_H];
static _Thread_local const uint16_t* s_read_view;
/* Writes into s_composed so far (openpete_vram_composed_bump). */
static _Atomic uint32_t s_composed_version;

uint16_t* openpete_vram_composed(void) { return s_composed; }
void openpete_vram_bind_composed(void) { s_read_view = s_composed; }
const uint16_t* openpete_vram_read(void) { return s_read_view ? s_read_view : s_model; }
void openpete_vram_composed_bump(void)
{
    atomic_fetch_add_explicit(&s_composed_version, 1u, memory_order_relaxed);
}
int openpete_vram_read_view(uint32_t* version)
{
    if (!s_read_view) { *version = 0u; return 0; }
    *version = atomic_load_explicit(&s_composed_version, memory_order_relaxed);
    return 1;
}

/* ---- op-rect log (verify census) ---------------------------------------- */
#define NV_MAX_OP_RECTS 64
static openpete_vram_op_rect s_op_rects[NV_MAX_OP_RECTS];
static int s_op_n = 0;
static int s_op_overflow = 0;

/** @brief Append one written rect to the op-rect log, or flag overflow when the log is full.
 *
 * @param x Left edge in texels.
 * @param y Top edge in rows.
 * @param w Width in texels.
 * @param h Height in rows. */
static void op_log(int x, int y, int w, int h)
{
    if (s_op_n >= NV_MAX_OP_RECTS) { s_op_overflow = 1; return; }
    s_op_rects[s_op_n].x = x; s_op_rects[s_op_n].y = y;
    s_op_rects[s_op_n].w = w; s_op_rects[s_op_n].h = h;
    s_op_n++;
}

int openpete_vram_drain_op_rects(openpete_vram_op_rect* out, int max, int* overflowed)
{
    int n = s_op_n < max ? s_op_n : max;
    memcpy(out, s_op_rects, (size_t)n * sizeof(*out));
    if (overflowed) *overflowed = s_op_overflow || s_op_n > max;
    s_op_n = 0;
    s_op_overflow = 0;
    return n;
}

/* ---- Texture-pack census ---------------------------------------------------
 * OPENPETE_TEXPACK_CENSUS=1: log every op with rect + (loads) a cheap 64-bit
 * content hash, one line per op, for the upload-granularity analysis (TIM vs
 * atlas packing, CLUT placement, per-level repack, hash stability across
 * runs). FNV-1a here is census-only; the registry uses XXH3. */
extern unsigned openpete_frame;
/** @brief Report whether OPENPETE_TEXPACK_CENSUS is set to a non-empty value.
 *
 * The environment is read once, on the first call, and latched.
 * @return 1 when the census is on, else 0. */
static int tp_census_on(void) {
    static int s = -1;
    if (s < 0) { const char* e = getenv("OPENPETE_TEXPACK_CENSUS"); s = (e && *e) ? 1 : 0; }
    return s;
}
/** @brief Hash a texel rect with 64-bit FNV-1a over the 16-bit texels.
 *
 * @param p First texel of the rect.
 * @param w Row width in texels.
 * @param h Row count.
 * @param stride Texels from one row start to the next.
 * @return The hash of the w x h texels, row by row. */
static uint64_t tp_fnv1a(const uint16_t* p, int w, int h, int stride) {
    uint64_t hv = 1469598103934665603ull;
    for (int i = 0; i < h; i++, p += stride)
        for (int j = 0; j < w; j++) { hv ^= p[j]; hv *= 1099511628211ull; }
    return hv;
}

/* ---- ops ----------------------------------------------------------------- */

/* LoadImage: src keeps the original width as stride through the clamp. */
void openpete_vram_load(int x, int y, int w, int h, const uint16_t* src) OP_REQUIRES_SHARED(sim)
{
    if (w <= 0 || h <= 0 || !src) return;
    const int stride = w;
    int dst_x = x & (OPENPETE_VRAM_W - 1);
    int dst_y = y & (OPENPETE_VRAM_H - 1);
    if (dst_x + w > OPENPETE_VRAM_W)  w = OPENPETE_VRAM_W - dst_x;
    if (dst_y + h > OPENPETE_VRAM_H)  h = OPENPETE_VRAM_H - dst_y;
    if (w <= 0 || h <= 0) return;

    uint16_t* dst = s_model + dst_x + (size_t)dst_y * OPENPETE_VRAM_W;
    const uint16_t* src0 = src;
    for (int i = 0; i < h; i++) {
        memcpy(dst, src, (size_t)w * sizeof(uint16_t));
        dst += OPENPETE_VRAM_W;
        src += stride;
    }
    op_log(dst_x, dst_y, w, h);
    texture_pack_on_load(dst_x, dst_y, w, h, src0, stride);
    openpete_fb_snapshot_on_fill(dst_x, dst_y, w, h);
    if (tp_census_on())
        fprintf(stderr, "[texpack] f=%u load %d,%d %dx%d h=%016llx\n",
                openpete_frame, dst_x, dst_y, w, h,
                (unsigned long long)tp_fnv1a(src0, w, h, stride));
}

/* MoveImage: GR_CopyVRAM(NULL, sx,sy, w,h, dx,dy) - source origin masked and
 * clamped FIRST, then the destination (the sequential clamp order matters for
 * edge rects). Rows copied top-down; memmove per row (PsyCross uses memcpy on
 * rows that are disjoint in every real caller - the census would flag any
 * intra-row-overlap divergence). */
void openpete_vram_move(int src_x, int src_y, int w, int h, int dst_x, int dst_y) OP_REQUIRES_SHARED(sim)
{
    if (w <= 0 || h <= 0) return;
    src_x &= (OPENPETE_VRAM_W - 1);
    src_y &= (OPENPETE_VRAM_H - 1);
    if (src_x + w > OPENPETE_VRAM_W)  w = OPENPETE_VRAM_W - src_x;
    if (src_y + h > OPENPETE_VRAM_H)  h = OPENPETE_VRAM_H - src_y;
    dst_x &= (OPENPETE_VRAM_W - 1);
    dst_y &= (OPENPETE_VRAM_H - 1);
    if (dst_x + w > OPENPETE_VRAM_W)  w = OPENPETE_VRAM_W - dst_x;
    if (dst_y + h > OPENPETE_VRAM_H)  h = OPENPETE_VRAM_H - dst_y;
    if (w <= 0 || h <= 0) return;

    /* A copy out of a display buffer onto a scratch page is a framebuffer
     * window: the screen re-displays it as a textured quad, and the emulated
     * framebuffer this copy reads carries no pixels on a native render. Record
     * the rect so a present can put the native frame's own pixels behind it,
     * before the copy lands, so the record keeps the texels it overwrites. */
    openpete_fb_snapshot_on_move(src_x, src_y, w, h, dst_x, dst_y);
    const uint16_t* src = s_model + src_x + (size_t)src_y * OPENPETE_VRAM_W;
    uint16_t*       dst = s_model + dst_x + (size_t)dst_y * OPENPETE_VRAM_W;
    for (int i = 0; i < h; i++) {
        memmove(dst, src, (size_t)w * sizeof(uint16_t));
        dst += OPENPETE_VRAM_W;
        src += OPENPETE_VRAM_W;
    }
    op_log(dst_x, dst_y, w, h);
    texture_pack_on_move(src_x, src_y, w, h, dst_x, dst_y);
    if (tp_census_on())
        fprintf(stderr, "[texpack] f=%u move %d,%d -> %d,%d %dx%d\n",
                openpete_frame, src_x, src_y, dst_x, dst_y, w, h);
}

/* ClearImage: GR_ClearVRAM - NO origin masking (only w/h clamp), and the
 * b<<11 packing quirk (NOT the PSX's b<<10) replicated as-is: the reference
 * behaviour is PsyCross's, and every game call site passes (0,0,0) anyway.
 * Negative origins (which PsyCross would write out-of-bounds with) are
 * clamped defensively instead of reproduced. */
void openpete_vram_clear(int x, int y, int w, int h,
                         uint8_t r, uint8_t g, uint8_t b) OP_REQUIRES_SHARED(sim)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x >= OPENPETE_VRAM_W || y >= OPENPETE_VRAM_H) return;
    if (x + w > OPENPETE_VRAM_W)  w = OPENPETE_VRAM_W - x;
    if (y + h > OPENPETE_VRAM_H)  h = OPENPETE_VRAM_H - y;
    if (w <= 0 || h <= 0) return;

    const uint16_t packed = (uint16_t)(r | (g << 5) | (b << 11));
    uint16_t* dst = s_model + x + (size_t)y * OPENPETE_VRAM_W;
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) dst[j] = packed;
        dst += OPENPETE_VRAM_W;
    }
    op_log(x, y, w, h);
    texture_pack_on_clear(x, y, w, h);
    openpete_fb_snapshot_on_fill(x, y, w, h);
    if (tp_census_on())
        fprintf(stderr, "[texpack] f=%u clear %d,%d %dx%d rgb=%u,%u,%u\n",
                openpete_frame, x, y, w, h, r, g, b);
}

/* ---- install ------------------------------------------------------------- */

/** @brief PsyCross load callback: forwards to openpete_vram_load.
 *
 * @param x Destination X in texels.
 * @param y Destination Y in texels.
 * @param w Width in texels.
 * @param h Height in rows.
 * @param src Source texels, @p w per row. */
static void nv_load(int x, int y, int w, int h, const unsigned short* src)
{ OP_ROLE_ASSERT_SHARED(sim); openpete_vram_load(x, y, w, h, (const uint16_t*)src); }
/** @brief PsyCross move callback: forwards to openpete_vram_move.
 *
 * @param sx Source X in texels.
 * @param sy Source Y in rows.
 * @param w Width in texels.
 * @param h Height in rows.
 * @param dx Destination X in texels.
 * @param dy Destination Y in rows. */
static void nv_move(int sx, int sy, int w, int h, int dx, int dy)
{ OP_ROLE_ASSERT_SHARED(sim); openpete_vram_move(sx, sy, w, h, dx, dy); }
/** @brief PsyCross clear callback: forwards to openpete_vram_clear.
 *
 * @param x Left edge in texels.
 * @param y Top edge in rows.
 * @param w Width in texels.
 * @param h Height in rows.
 * @param r Red channel.
 * @param g Green channel.
 * @param b Blue channel. */
static void nv_clear(int x, int y, int w, int h,
                     unsigned char r, unsigned char g, unsigned char b)
{ OP_ROLE_ASSERT_SHARED(sim); openpete_vram_clear(x, y, w, h, r, g, b); }

void openpete_vram_install_ops(void)
{
    static const PsyX_VRAMOps ops = { nv_load, nv_move, nv_clear };
    PsyX_SetVRAMOpCallbacks(&ops);
    texture_pack_init();
}
