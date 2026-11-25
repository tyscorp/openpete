// core/include/screenshot.h - read pixels from the host renderer to PNG.

#ifndef OPENPETE_SCREENSHOT_H
#define OPENPETE_SCREENSHOT_H

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Save the SDL renderer's current target to an image file.
 *
 * Reads the pixels with SDL_RenderReadPixels and converts them to RGBA32
 * when the renderer's format differs; the rows are already top-down. A path
 * ending in `.png` (any case) is written with
 * openpete_write_png_rgba_stride, any other path as BMP with SDL_SaveBMP. Used by run.c's SDL-renderer screenshot
 * path.
 * @param renderer Renderer to read.
 * @param path Output path.
 * @return 0 on success; -1 on a NULL argument or a read, convert or write
 *         failure. */
int openpete_screenshot_save_png(SDL_Renderer* renderer, const char* path);

/** @brief Expand the `{n}` and `{p}` placeholders of a screenshot path template.
 *
 * Each `{n}` becomes @p frame and each `{p}` becomes @p present in decimal;
 * every other character is copied. Output longer than @p out_sz is
 * truncated and always NUL-terminated. A NULL template yields an empty
 * string. A `{` that does not open a complete `{n}` or `{p}` is copied
 * as is; the scan never reads past the terminator.
 * @param tmpl Template, such as the --screenshot-output value.
 * @param frame VBL number substituted for `{n}`.
 * @param present Present index substituted for `{p}`; callers capturing one
 *        present per VBL pass 0.
 * @param out Output buffer; nothing is written when NULL.
 * @param out_sz Size of @p out; nothing is written when 0. */
void openpete_screenshot_format_path(const char* tmpl, int frame,
                                     int present,
                                     char* out, size_t out_sz);

/** @brief Report whether a path ends in `.png`, ignoring case.
 *
 * @param path Path to test; NULL returns 0.
 * @return 1 for a `.png` suffix, else 0. */
int  openpete_screenshot_path_is_png(const char* path);

/** @brief Write a top-down RGBA8 buffer with a row stride to a PNG file, replacing it atomically.
 *
 * The PNG is encoded with stb_image_write into a temporary beside @p path
 * and renamed over it without a device flush (OP_ATOMIC_NOSYNC, the
 * capture class), so a failed or interrupted write leaves any previous
 * file whole. No flip or channel swap is applied; alpha is written as
 * given.
 * @param path Output path.
 * @param pixels h rows of w*4 bytes, @p stride bytes apart.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @param stride Bytes from one row's start to the next.
 * @return 0 on success; 1 on a NULL argument, a non-positive dimension,
 *         or a write failure. */
int  openpete_write_png_rgba_stride(const char* path, const void* pixels, int w, int h,
                                    int stride);

/** @brief Write a top-down, packed RGBA8 buffer to a PNG file, replacing it atomically.
 *
 * openpete_write_png_rgba_stride with a stride of w*4. The native
 * renderer's readbacks already return top-down R,G,B,A. Alpha is written
 * as given. Used by the native screenshot paths and the identity audit.
 * @param path Output path.
 * @param pixels w*h*4 bytes, rows packed.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return 0 on success; 1 on a NULL argument, a non-positive dimension,
 *         or a write failure. */
int  openpete_write_png_rgba(const char* path, const void* pixels, int w, int h);

/** @brief Queue a top-down, packed RGBA8 buffer for PNG encoding and writing on the writer thread.
 *
 * The writer thread is created on the first call and runs the queued jobs
 * in order, each through openpete_write_png_rgba, then logs
 * `<what> screenshot WxH -> <path> key=<key>` with the readback and
 * encode spans. The caller returns without waiting on the encoder or the
 * file system. With OPENPETE_SHOT_SYNC=1, a full queue (8 jobs) or no
 * writer thread, the job runs inline on the calling thread instead. The
 * path counts as taken for openpete_screenshot_user_path until the file
 * is committed. openpete_png_async_drain, also registered with atexit,
 * writes every queued job before the program ends.
 * @param path Output path, copied.
 * @param pixels w*h*4 bytes from malloc; ownership passes to the call, which frees them.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @param what Label for the log line ("player").
 * @param key The feed key at the readback, logged.
 * @param readback_ms The caller's readback span, logged.
 * @return 0 when the job was queued or written; 1 on a NULL argument, a
 *         non-positive dimension or an allocation failure (the pixels are
 *         freed). */
int  openpete_write_png_rgba_async(const char* path, unsigned char* pixels, int w, int h,
                                   const char* what, unsigned key, double readback_ms);

/** @brief Return borrowed pixels to their owner; called once per borrowed write, on any thread.
 * @param ctx The owner's context.
 * @param idx The owner's index for the pixels. */
typedef void (*openpete_png_release_fn)(void* ctx, int idx);

/** @brief Request a PNG of an image not captured yet; the capturing thread takes the request later.
 *
 * The path counts as taken (openpete_png_async_pending) from this call
 * until the file is committed or the request is cancelled. Requests are
 * taken in order; 16 can wait at once.
 * @param path Output path, copied.
 * @param what Label for the log line ("player"); NULL reads as offscreen.
 * @return 0 when queued; 1 on a NULL or empty path, an allocation failure,
 *         or 16 requests already waiting (logged). */
int  openpete_png_request(const char* path, const char* what);

/** @brief Return how many requests wait for a capture; safe on any thread.
 * @return The count. */
int  openpete_png_requests_pending(void);

/** @brief Take the oldest request for capture; its path stays taken.
 *
 * The caller passes the result to openpete_png_request_submit or
 * openpete_png_request_cancel exactly once.
 * @param key The feed key logged with the file.
 * @return The request, or NULL when none waits. */
void* openpete_png_request_take(unsigned key);

/** @brief Hand a taken request its captured pixels; the writer encodes them and never the calling thread while a writer runs.
 *
 * The writer converts the pixels into its own R,G,B,A copy (red and blue
 * swapped when @p bgra, alpha forced to 0xFF), calls @p release, then
 * encodes and writes as openpete_write_png_rgba_async does, logging the
 * span from the request to this call as the readback. A full writer queue
 * takes nothing and returns 2: the caller keeps the request and the pixels
 * and retries. The job runs inline only when no writer thread can be
 * started.
 * @param job A request from openpete_png_request_take.
 * @param src w*h*4 bytes, rows packed, valid until @p release is called.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @param bgra Nonzero when @p src is B,G,R,A.
 * @param release Called with @p release_ctx and @p release_idx once @p src is no longer read; may be NULL.
 * @param release_ctx Passed to @p release.
 * @param release_idx Passed to @p release.
 * @return 0 when queued or written; 1 on a NULL argument or a non-positive
 *         dimension (@p release was called, the request freed); 2 when the
 *         queue is full (nothing was taken). */
int  openpete_png_request_submit(void* job, const void* src, int w, int h, int bgra,
                                 openpete_png_release_fn release, void* release_ctx,
                                 int release_idx);

/** @brief Drop a taken request without writing it, logging why.
 * @param job A request from openpete_png_request_take; NULL is ignored.
 * @param why The reason logged. */
void openpete_png_request_cancel(void* job, const char* why);

/** @brief Report whether OPENPETE_SHOT_SYNC asks for screenshots encoded inline on the requesting thread.
 * @return 1 when OPENPETE_SHOT_SYNC is set to a value other than 0, else 0. */
int  openpete_png_async_inline(void);

/** @brief Report whether a queued or running asynchronous PNG write, or a waiting request, holds a path.
 * @param path The path to test; NULL returns 0.
 * @return 1 when a write to @p path is queued, running or requested, else 0. */
int  openpete_png_async_pending(const char* path);

/** @brief Write every queued asynchronous PNG and stop the writer thread.
 *
 * Blocks until the queue is empty and the writer has exited; a later
 * openpete_write_png_rgba_async starts a new writer. Safe to call when no
 * writer ran. */
void openpete_png_async_drain(void);

/** @brief Name the next player-facing screenshot and create its directory.
 *
 * The name is `<dir>/openpete-<YYYYmmdd-HHMMSS>.png` in local time, then
 * `-2` to `-999` suffixes when the name exists. The directory is the
 * per-user or portable `screenshots` data directory
 * (openpete_paths_data_dir), else the relative path `screenshots`. Used by
 * run.c's screenshot key.
 * @param out Receives the path; emptied on failure.
 * @param cap Size of @p out.
 * @return 0 on success; -1 when @p out is NULL or @p cap is 0, the
 *         directory cannot be created, or every suffix is taken. */
int  openpete_screenshot_user_path(char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_SCREENSHOT_H */
