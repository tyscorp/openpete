/* openpete_native_vram.h - the natively-owned PSX VRAM model.
 *
 * A 1024x512 uint16 surface written by native implementations of the PSYQ
 * image ops (LoadImage / MoveImage / ClearImage, including the OT-embedded
 * DR_LOAD/DR_MOVE and the LoadTPage/LoadClut wrappers, all forwarded from
 * PsyCross's LIBGPU adapters via PsyX_SetVRAMOpCallbacks). The ops replicate
 * the PSX GPU coordinate masking/clamping semantics of PsyCross's
 * GR_CopyVRAM / GR_ClearVRAM to the byte, so while PsyCross is still alive
 * the model can be memcmp-verified against its mirror per written rect
 * (OPENPETE_VRAM_VERIFY, native_render.c).
 *
 * While PsyCross exists, framebuffer-copyback content (GL FBO -> vram) is
 * NOT produced natively - those regions are shadow-copied from PsyCross's
 * mirror at present time (the dirty-rect path), which also
 * re-shadows op-written rects (byte-identical when the ops are correct).
 * StoreImage (VRAM -> gameplay RAM, canonical) keeps reading PsyCross's
 * mirror until the native renderer owns framebuffer rendering.
 */

#ifndef OPENPETE_NATIVE_VRAM_H
#define OPENPETE_NATIVE_VRAM_H

#include <stdint.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

#define OPENPETE_VRAM_W 1024
#define OPENPETE_VRAM_H 512

/** @brief Return the VRAM model's storage.
 *
 * A static OPENPETE_VRAM_W x OPENPETE_VRAM_H array of 16-bit texels, row
 * stride OPENPETE_VRAM_W. The native renderer uses it as its CPU VRAM
 * mirror; the texture-pack registry and the terrain decoder read it.
 * @return Pointer to texel (0, 0). */
uint16_t* openpete_vram_data(void);

/** @brief Return the composed VRAM image's storage, written only by the thread that owns the display.
 *
 * Same size and stride as the model. The native renderer applies each VRAM
 * journal entry to it as it records the entry's upload, so it holds the
 * texels the GPU VRAM texture holds for the scene being composed. A thread
 * composing concurrently with the guest reads it instead of the model,
 * which the guest's image ops and the mirror refresh rewrite meanwhile.
 * @return Pointer to texel (0, 0). */
uint16_t* openpete_vram_composed(void);

/** @brief Bind the calling thread's VRAM reads (openpete_vram_read) to the composed image.
 *
 * Called once by the present thread at its start. The binding is
 * thread-local; an unbound thread reads the model. */
void openpete_vram_bind_composed(void);

/** @brief Return the VRAM image the calling thread reads: the composed image when bound, else the model.
 *
 * Read-only consumers that run at compose time (texture-pack identity, the
 * pack fog and grade fits, held-texel tests) read through this, so a
 * present thread never reads the model the guest is writing.
 * @return Pointer to texel (0, 0). */
const uint16_t* openpete_vram_read(void);

/** @brief Count one write into the composed VRAM image.
 *
 * Called by the image's writer after each journal entry it applies. The
 * count is the image's version: a value computed from the image (a palette
 * hash, a grade fit) is reusable while the version it was computed at
 * holds. */
void openpete_vram_composed_bump(void);

/** @brief Return which VRAM image the calling thread reads (openpete_vram_read) and that image's version.
 *
 * The model's version is reported as 0: every write to it passes the
 * texture-pack registry's op hooks, which step the registry generation, so
 * that generation already versions it. The composed image is versioned by
 * openpete_vram_composed_bump.
 * @param version Receives the version.
 * @return 1 when the thread reads the composed image, 0 when it reads the model. */
int openpete_vram_read_view(uint32_t* version);

/** @brief Install the native image ops into PsyCross and initialise the texture-pack registry.
 *
 * Registers openpete_vram_load, openpete_vram_move and openpete_vram_clear
 * as PsyCross's VRAM op callbacks, then calls texture_pack_init. Called at
 * native renderer bring-up. Idempotent: a repeat registers the same table
 * and texture_pack_init returns at once. */
void openpete_vram_install_ops(void);

/* Native op implementations (also callable directly once PsyCross goes). */
/** @brief Copy a tightly packed image into the VRAM model (the PSYQ LoadImage op).
 *
 * Replicates PsyCross's GR_CopyVRAM(src, 0, 0, w, h, x, y): the destination
 * origin is masked to the VRAM size and the width and height are clamped at
 * its right and bottom edges, while the source keeps the original @p w as
 * its row stride. The written rect is logged for the verify census and
 * passed to the texture-pack registry; OPENPETE_TEXPACK_CENSUS logs it with
 * a content hash. A non-positive size or NULL @p src is ignored.
 * @param x Destination X in texels.
 * @param y Destination Y in texels.
 * @param w Width in texels.
 * @param h Height in rows.
 * @param src Source texels, @p w per row. */
void openpete_vram_load (int x, int y, int w, int h, const uint16_t* src);
/** @brief Copy a rect within the VRAM model (the PSYQ MoveImage op).
 *
 * Replicates PsyCross's GR_CopyVRAM(NULL, sx, sy, w, h, dx, dy): the source
 * origin is masked to the VRAM size and the size clamped at the right and
 * bottom edges first, then the destination is masked and clamps the size
 * again, so an edge rect shrinks to the smaller fit. Rows copy top-down with
 * memmove per row; two rects that overlap across rows are not copied as a
 * snapshot. The effective rect is logged for the verify census, passed to
 * the texture-pack registry, and recorded by the framebuffer snapshot when
 * it is a copy out of a display buffer. A non-positive size is ignored.
 * @param src_x Source X in texels.
 * @param src_y Source Y in rows.
 * @param w Width in texels.
 * @param h Height in rows.
 * @param dst_x Destination X in texels.
 * @param dst_y Destination Y in rows. */
void openpete_vram_move (int src_x, int src_y, int w, int h, int dst_x, int dst_y) OP_REQUIRES_SHARED(sim);
/** @brief Fill a rect of the VRAM model with one colour (the PSYQ ClearImage op).
 *
 * Replicates PsyCross's GR_ClearVRAM: the origin is not masked (a negative
 * origin trims the rect, an origin past the right or bottom edge is
 * ignored) and the size is clamped at the edges. The texel is
 * r | g << 5 | b << 11 with the channels unmasked, the packing PsyCross
 * uses rather than the hardware's b << 10. The rect is logged for the
 * verify census and passed to the texture-pack registry.
 * @param x Left edge in texels.
 * @param y Top edge in rows.
 * @param w Width in texels.
 * @param h Height in rows.
 * @param r Red channel.
 * @param g Green channel.
 * @param b Blue channel. */
void openpete_vram_clear(int x, int y, int w, int h,
                         uint8_t r, uint8_t g, uint8_t b);

typedef struct { int x, y, w, h; } openpete_vram_op_rect;
/** @brief Copy out and reset the log of rects the VRAM ops wrote (verify census).
 *
 * Each rect is the effective (post-mask, post-clamp) extent of one load,
 * move or clear since the last drain. The log holds 64 entries; further ops
 * are counted only as overflow. The log is reset whether or not every entry
 * fit in `max`.
 * @param out Destination for up to `max` rects.
 * @param max Capacity of `out` in entries; must not be negative.
 * @param overflowed Out: set to 1 when ops were dropped or did not fit in
 *        `max`, else 0; NULL is ignored. The census must then skip
 *        exact-match accounting for the window.
 * @return The number of rects copied. */
int openpete_vram_drain_op_rects(openpete_vram_op_rect* out, int max, int* overflowed);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_VRAM_H */
