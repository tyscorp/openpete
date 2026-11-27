/* openpete_fb_snapshot.h - framebuffer-window snapshots.
 *
 * A game screen can copy a rect of its draw buffer to a VRAM scratch page with
 * MoveImage and then re-display that copy every frame as a textured quad - a
 * window onto the frame as it stood when the copy was taken. The quad's texels
 * come from the emulated framebuffer, which the native renderer never writes:
 * under a native-only run nothing rasterizes into it, the copy is zeros, and a
 * 16-bit texel of zero is transparent even on an opaque primitive, so the
 * window contributes no pixels at all.
 *
 * This store closes that channel. The move records its destination rect; at the
 * next present the native renderer crops its own composed frame to the source
 * rect and stores it here as RGBA8 at native resolution. A fed textured quad
 * whose texture page and texel box land inside a stored rect is redirected to
 * the snapshot instead of the VRAM sampler. A quad that finds no stored
 * snapshot keeps the stock VRAM path.
 */
#ifndef OPENPETE_FB_SNAPSHOT_H
#define OPENPETE_FB_SNAPSHOT_H

#include <stdint.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

/* Material bpp sentinel for a scene triangle redirected to a snapshot. The
 * material fields are otherwise unused on such a triangle: tpage_x carries the
 * slot, tpage_y says how the vertex u,v map (0: pixels of the stored image;
 * 1: texels of the slot's rect, for a quad fed before its capture resolved,
 * which the draw maps onto the image at whatever resolution it holds). A
 * sentinel rather than a reserved id keeps the snapshot slots out of the mod
 * texture-id namespace, and keeps the run-coalescing material compare from
 * merging a snapshot run with a VRAM or mod run. */
#define OPENPETE_FB_SNAPSHOT_BPP 33

/* Snapshot index of draw area 0: window slots take the indices below it, draw
 * area i takes this plus i. */
#define OPENPETE_FB_SNAPSHOT_AREA_BASE 4
/* Draw areas the store tracks. */
#define OPENPETE_FB_SNAPSHOT_AREA_MAX 4

/** @brief Record a draw area the game put with PutDrawEnv: its clip rect and drawing offset.
 *
 * A primitive at screen (x, y) lands at VRAM (x + ofs_x, y + ofs_y), so a
 * copy out of VRAM (sx, sy) reads the pixels drawn at screen (sx - ofs_x,
 * sy - ofs_y); the area holding a copy's source supplies that offset. The
 * area reuses the live slot with the same clip rect, else a free slot, else
 * evicts slots round-robin among the four; it becomes the area being drawn.
 * With auto-clear set the area's next image starts from the clear colour,
 * so its stored content image is dropped; with it clear the game draws over
 * what the buffer holds and the image is kept. No effect when the mechanism
 * is disabled or the clip rect is empty.
 * @param clip_x Clip rect left, VRAM texels.
 * @param clip_y Clip rect top, VRAM texels.
 * @param clip_w Clip rect width.
 * @param clip_h Clip rect height.
 * @param ofs_x Drawing offset x.
 * @param ofs_y Drawing offset y.
 * @param isbg The DRAWENV's auto-clear flag. */
void openpete_fb_snapshot_note_draw_env(int clip_x, int clip_y, int clip_w, int clip_h,
                                        int ofs_x, int ofs_y, int isbg);

/** @brief Record that the scene the game's render last fed is complete.
 *
 * Called at the canonical capture, after the tick's 2D is fed. A later
 * framebuffer window copy names this scene: the game copies after
 * DrawSync, which waits for the ordering table of the last completed tick,
 * while the native scene open at the copy may already belong to the next
 * tick and receive its 2D after the copy. */
void openpete_fb_snapshot_note_scene_complete(void) OP_REQUIRES_SHARED(sim);

/** @brief Record that the draw area of the last PutDrawEnv holds the scene with a given stamp.
 *
 * Called when a DrawOTag submits a scene no canonical capture has taken
 * yet: the put before it named the scene completed last
 * (openpete_fb_snapshot_note_draw_env), but the table drawn is the open one.
 * A quad that later samples the area resolves to this scene.
 * @param frame The stamp of the scene the DrawOTag drew. */
void openpete_fb_snapshot_note_area_drawn(unsigned frame);

/** @brief Record a MoveImage out of a display buffer.
 *
 * The source must lie in the display buffers (x + w <= 512). Its drawing
 * offset comes from the draw area holding it; with no such area the buffers
 * are taken to split at VRAM row 256 with no offset, and a source crossing
 * the split is ignored. Two idioms are recognised:
 *
 *   destination at x >= 512 (a scratch page): a framebuffer window. The
 *   window slot holding the same destination rect, else a free slot, else
 *   slot 0, records the source in screen pixels and the stamp of the
 *   newest completed scene (openpete_fb_snapshot_note_scene_complete)
 *   and keeps the destination's texels from before the copy (the caller
 *   runs it before the copy lands). When the source area carries a content
 *   image and is not the area being drawn, the image is cropped and stored
 *   at once; otherwise the slot waits for the next present. Returns 1.
 *
 *   destination inside another draw area, covering it whole: the game moving
 *   a finished image into the buffer it is not drawing. The destination
 *   takes a copy of the source area's content image, or, when the source has
 *   none, both areas wait for the next present. Returns 0.
 *
 * Any other copy records nothing.
 * @param src_x Source left, VRAM texels, post-clamp.
 * @param src_y Source top, VRAM texels.
 * @param w Width in texels.
 * @param h Height in texels.
 * @param dst_x Destination left, VRAM texels.
 * @param dst_y Destination top, VRAM texels.
 * @return 1 when a window slot was recorded, 0 otherwise. */
int openpete_fb_snapshot_on_move(int src_x, int src_y, int w, int h,
                                 int dst_x, int dst_y) OP_REQUIRES_SHARED(sim);

/** @brief Return the VRAM texels a window slot's copy overwrote, with their rect.
 *
 * openpete_fb_snapshot_on_move keeps them when it records a window, before
 * the copy lands; a resolve composes the copied scene over them. Valid until
 * the next window copy. Any output pointer may be NULL.
 * @param idx Window slot index.
 * @param x Receives the rect's left, VRAM texels.
 * @param y Receives the rect's top.
 * @param w Receives the width.
 * @param h Receives the height.
 * @return The w x h texels, row-major and tightly packed, or NULL when the
 *         slot holds none. */
const uint16_t *openpete_fb_snapshot_under(int idx, int *x, int *y, int *w, int *h);

/** @brief Report whether any window slot or draw area is waiting for its pixels.
 *
 * @return 1 when one is pending, 0 otherwise. */
int openpete_fb_snapshot_pending(void);

/** @brief Drain the first pending window slot.
 *
 * Clears the slot's pending flag and returns its source rect with the scene
 * stamp taken at the copy: the resolving present composes that scene,
 * because the copy reads what the game drew into it, not what it draws
 * next. Any output pointer may be NULL.
 * @param idx Receives the slot index.
 * @param src_x Receives the source left, framebuffer screen pixels.
 * @param src_y Receives the source top.
 * @param w Receives the width.
 * @param h Receives the height.
 * @param frame Receives the scene stamp of the copy.
 * @return 1 when a slot was drained, 0 when none is pending. */
int openpete_fb_snapshot_take_pending(int *idx, int *src_x, int *src_y,
                                      int *w, int *h, unsigned *frame);

/** @brief Drain the first draw area waiting for a content image.
 *
 * Clears the area's pending flag and returns its clip rect less its drawing
 * offset, and the scene stamp of the copy that left it pending. Any output
 * pointer may be NULL.
 * @param idx Receives the area index.
 * @param src_x Receives the area's left, framebuffer screen pixels.
 * @param src_y Receives the area's top.
 * @param w Receives the width.
 * @param h Receives the height.
 * @param frame Receives the scene stamp of the copy.
 * @return 1 when an area was drained, 0 when none waits. */
int openpete_fb_snapshot_take_pending_area(int *idx, int *src_x, int *src_y,
                                           int *w, int *h, unsigned *frame);

/** @brief Store a drained window slot's pixels.
 *
 * Takes ownership of @p px, frees the slot's previous image, assigns the
 * new image a new key and counts the slot resolved. When @p idx is out of
 * range or the size is not positive, @p px is freed and nothing is stored;
 * a slot with no pixels is skipped by openpete_fb_snapshot_find, so its
 * quads keep the stock VRAM path.
 * @param idx Window slot index.
 * @param px RGBA8 pixels, @p pw by @p ph, tightly packed.
 * @param pw Image width in pixels.
 * @param ph Image height in pixels. */
void openpete_fb_snapshot_store(int idx, unsigned char *px, int pw, int ph);

/** @brief Hand back a drained window slot or draw area whose compose failed, so the next resolve retries it.
 *
 * The entry is pending again with the scene stamp of its copy. Its stored
 * image is dropped: it stands for an earlier copy or an earlier frame of
 * the buffer, not for what the game's copy left at the rect, so a quad
 * sampling the entry draws nothing until the retry stores the new image.
 * No effect on an index out of range.
 * @param idx Window slot index, or OPENPETE_FB_SNAPSHOT_AREA_BASE plus a draw area index. */
void openpete_fb_snapshot_resolve_failed(int idx);

/** @brief Store the content image of a drained draw area.
 *
 * `px` is the crop of the present composed into the area, covering the
 * rect openpete_fb_snapshot_take_pending_area returned. A later window
 * copy out of an area that is not the one being drawn crops this image
 * instead of waiting for a present, and a quad whose texels lie inside the
 * area samples it. The store takes ownership of `px`; it is freed and
 * nothing is stored when `idx` is out of range or the size is not
 * positive. Each store assigns the image a new key.
 * @param idx Draw area index.
 * @param px RGBA8 pixels, `pw` by `ph`, tightly packed.
 * @param pw Image width in pixels.
 * @param ph Image height in pixels. */
void openpete_fb_snapshot_store_area(int idx, unsigned char *px, int pw, int ph);

/** @brief Resolve a fed textured quad against the stored window and draw-area images.
 *
 * The texel box, not the page origin, must lie inside a stored rect: a
 * window copied to a row below its page's top edge is sampled with v offset
 * by that row. Window slots are searched first, then draw areas carrying a
 * content image, which serves a quad sampling the framebuffer itself (the
 * shrink of a frozen image before it is copied out). A slot or area waiting
 * for its capture matches too, mapped to its rect's texels; the capture
 * resolves before the present that draws the quad. Only 16-bit pages
 * match, since a copied framebuffer is re-displayed at one texel per pixel.
 * A hit counts one redirected quad. Any output pointer may be NULL.
 * @param tpage_x Texture page origin x, VRAM texels.
 * @param tpage_y Texture page origin y, VRAM texels.
 * @param bpp Texture page depth.
 * @param u0 Texel box left.
 * @param v0 Texel box top.
 * @param u1 Texel box right edge, inclusive of the edge past the last texel.
 * @param v1 Texel box bottom edge, likewise.
 * @param su Receives the scale from page texels to image pixels in x (1 in
 *        rect-texel mapping).
 * @param sv Receives the scale in y.
 * @param ox Receives the mapped x of the page origin (negative when the
 *        origin precedes the rect).
 * @param oy Receives the mapped y of the page origin.
 * @param texel_space Receives 1 when the stored entry holds no image yet and
 *        the mapping is to the rect's texels, else 0; may be NULL.
 * @return The window slot index, or 4 plus the draw area index, or -1 on a
 *         miss or when the mechanism is disabled. */
int openpete_fb_snapshot_find(int tpage_x, int tpage_y, int bpp,
                              int u0, int v0, int u1, int v1,
                              float *su, float *sv, float *ox, float *oy, int *texel_space);

/** @brief Read the key and size of the image stored at an index openpete_fb_snapshot_find returned.
 *
 * Safe from the presenting thread while the guest thread stores or drops
 * images: the read holds the store's mutex. The key is unique per stored
 * image for the lifetime of the run, so a texture cache keyed on it cannot
 * serve an earlier image. Any output pointer may be NULL.
 * @param idx Window slot index, or 4 plus a draw area index.
 * @param key Receives the key.
 * @param pw Receives the image width.
 * @param ph Receives the image height.
 * @return 1 when the index holds an image, else 0 and nothing is written. */
int openpete_fb_snapshot_image(int idx, uint64_t *key, int *pw, int *ph);

/** @brief Read the texel size of the rect an index openpete_fb_snapshot_find returned stands for.
 *
 * A redirected quad's UVs are texels of this rect; the draw maps the rect
 * onto the stored image. Any output pointer may be NULL.
 * @param idx Window slot index, or 4 plus a draw area index.
 * @param w Receives the rect width in texels.
 * @param h Receives the rect height in texels.
 * @return 1 when the index names a rect, else 0 and nothing is written. */
int openpete_fb_snapshot_rect(int idx, int *w, int *h);

/** @brief Copy the image stored at an index when it is still the image with `key`.
 *
 * The copy is taken under the store's mutex, so the guest thread cannot
 * free the image mid-copy; the caller owns the result and frees it.
 * @param idx Window slot index, or 4 plus a draw area index.
 * @param key Key read by openpete_fb_snapshot_image.
 * @param pw Receives the image width.
 * @param ph Receives the image height.
 * @return A malloc'd RGBA8 copy, or NULL when the index holds no image, holds
 *         a newer image than @p key, or the allocation fails. */
unsigned char *openpete_fb_snapshot_copy(int idx, uint64_t key, int *pw, int *ph);

/** @brief Return whether any window slot or draw area still holds the image with `key`.
 *
 * A consumer holding a GPU texture made from an image frees it once the
 * image is replaced or dropped.
 * @param key Image key.
 * @return 1 when live, else 0. */
int openpete_fb_snapshot_key_live(uint64_t key);

/** @brief Read the engagement counters since boot.
 *
 * Any output pointer may be NULL.
 * @param taken Receives the window rects recorded by a move.
 * @param resolved Receives the window rects given pixels.
 * @param quads Receives the quads redirected into a stored image. */
void openpete_fb_snapshot_counts(unsigned *taken, unsigned *resolved,
                                 unsigned *quads);

/** @brief Record a VRAM fill (ClearImage or LoadImage) so a draw area it covers whole forgets what it held.
 *
 * The buffer's content after such a fill is the fill alone, so a retained
 * run's held content (openpete_fb_snapshot_retained_area) starts afresh.
 * Partial fills change nothing. No effect when the mechanism is disabled.
 * @param x Fill left, VRAM texels.
 * @param y Fill top.
 * @param w Fill width.
 * @param h Fill height. */
void openpete_fb_snapshot_on_fill(int x, int y, int w, int h) OP_REQUIRES_SHARED(sim);

/** @brief Report whether a live draw area's clip rect contains a VRAM rect.
 *
 * @param x Rect left, VRAM texels.
 * @param y Rect top.
 * @param w Rect width.
 * @param h Rect height.
 * @return 1 when an area holds the rect, else 0 (also when the mechanism is disabled). */
int openpete_fb_snapshot_area_holds(int x, int y, int w, int h);

/** @brief Return the draw area the game is drawing into when its last put kept the buffer (auto-clear off).
 *
 * While an area runs with auto-clear off, what it shows is everything drawn
 * into it since its content last started afresh. The epoch changes whenever
 * that content is replaced: a put that changes the area's clip rect or
 * clears, the first retained put after a cleared one, a fill covering it
 * (openpete_fb_snapshot_on_fill), or a copy of another area over it.
 * @param epoch Receives the area's content epoch; may be NULL.
 * @return The area index (0 to OPENPETE_FB_SNAPSHOT_AREA_MAX - 1), or -1 when
 *         the area being drawn is cleared every frame, none is put, or the
 *         mechanism is disabled. */
int openpete_fb_snapshot_retained_area(unsigned *epoch);

/** @brief Record an image op's write into the VRAM model for the native compose's held scenes.
 *
 * Defined by the native renderer. A draw area the rect overlaps was last
 * written by an image op, so an empty present showing it re-displays the
 * VRAM framebuffer; a copy of one whole draw area onto another whole draw
 * area instead gives the destination the source's last writer and held
 * scene. Called for every LoadImage, ClearImage and MoveImage, before the
 * snapshot store's own handling and whether or not that store is enabled.
 * @param x Destination left, VRAM texels, post-clamp.
 * @param y Destination top.
 * @param w Width.
 * @param h Height.
 * @param src_x Copy source left, or -1 for a fill or load.
 * @param src_y Copy source top; unused for a fill or load. */
void openpete_native_hold_note_vram(int x, int y, int w, int h, int src_x, int src_y) OP_REQUIRES_SHARED(sim);

/** @brief Report whether the snapshot mechanism is enabled.
 *
 * OPENPETE_FBSNAP=0 disables it (the negative control: every quad keeps the
 * stock VRAM path); the value is read once and cached.
 * @return 1 when enabled, 0 otherwise. */
int openpete_fb_snapshot_enabled(void);

/** @brief Find the live draw area whose clip rect is exactly the given rect, with its content epoch and auto-clear state.
 *
 * Reads the state openpete_fb_snapshot_note_draw_env and the fills keep; a
 * put of the same rect with auto-clear off continues the area's content
 * when the area is retained now, and starts it afresh otherwise.
 * @param clip_x Clip rect left, VRAM texels.
 * @param clip_y Clip rect top.
 * @param clip_w Clip rect width.
 * @param clip_h Clip rect height.
 * @param epoch Receives the area's content epoch; may be NULL.
 * @param retained Receives 1 when the area's last put kept the buffer (auto-clear off); may be NULL.
 * @return The area index (0 to OPENPETE_FB_SNAPSHOT_AREA_MAX - 1), or -1 when
 *         no live area has that rect or the mechanism is disabled. */
int openpete_fb_snapshot_area_find(int clip_x, int clip_y, int clip_w, int clip_h,
                                   unsigned *epoch, int *retained);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_FB_SNAPSHOT_H */
