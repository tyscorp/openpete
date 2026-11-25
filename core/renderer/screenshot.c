// core/renderer/screenshot.c - read renderer pixels and save as PNG.

#include "screenshot.h"
#include "openpete_atomic_file.h"
#include "openpete_log.h"
#include "openpete_paths.h"
#include "openpete_thread_roles.h"

#include <time.h>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

/** @brief stb_image_write output callback: append the encoded bytes to the atomic replacement's temporary.
 *
 * A short write sets the stream's error flag, which op_atomic_file_commit
 * refuses.
 * @param ctx The op_atomic_file_t.
 * @param data Encoded bytes.
 * @param size Their count. */
static void png_to_atomic(void* ctx, void* data, int size) {
    op_atomic_file_t* af = (op_atomic_file_t*)ctx;
    if (size > 0) (void)fwrite(data, 1, (size_t)size, af->f);
}

int openpete_write_png_rgba_stride(const char* path, const void* pixels, int w, int h,
                                   int stride) {
    if (!path || !pixels || w <= 0 || h <= 0) return 1;
    op_atomic_file_t af;
    if (op_atomic_file_open_ex(&af, path, OP_ATOMIC_NOSYNC) != 0) return 1;
    if (!stbi_write_png_to_func(png_to_atomic, &af, w, h, 4, pixels, stride)) {
        OPL_ERROR_F("screenshot", "PNG encode for %s failed", path);
        op_atomic_file_abort(&af);
        return 1;
    }
    return op_atomic_file_commit(&af) == 0 ? 0 : 1;
}

int openpete_write_png_rgba(const char* path, const void* pixels, int w, int h) {
    return openpete_write_png_rgba_stride(path, pixels, w, h, w * 4);
}

/* Asynchronous PNG writer: one worker thread drains a FIFO of encode-and-write
 * jobs, so the thread that read the pixels back never waits on the encoder or
 * the file system. A second FIFO holds requests whose image the display's
 * owner has not captured yet (openpete_png_request). Both queues, the busy
 * flag and the thread handle are guarded by s_aw.mx; a requested or queued
 * job's path stays reserved (openpete_png_async_pending) until its file is
 * committed. */
#define PNG_ASYNC_SLOTS 8
#define PNG_ASYNC_REQUESTS 16

typedef struct png_job {
    char           path[1100];
    char           what[32];
    unsigned char* px;          /* owned; freed by the writer */
    const unsigned char* src;   /* borrowed pixels, converted into px first; NULL when px is given */
    int            bgra;        /* src is B,G,R,A */
    openpete_png_release_fn release;   /* returns src to its owner */
    void*          release_ctx;
    int            release_idx;
    int            w, h;
    unsigned       key;         /* the feed key at the readback */
    Uint64         t_req;       /* a request's instant (SDL_GetTicksNS), 0 for a direct write */
    double         readback_ms; /* the caller's readback span */
} png_job_t;

static struct {
    SDL_Mutex*     mx;
    SDL_Condition* cv;          /* a job was queued, or quit was raised */
    SDL_Condition* idle;        /* the queue emptied and no job is running */
    SDL_Thread*    th;
    png_job_t*     q[PNG_ASYNC_SLOTS];
    int            head, n;
    png_job_t*     cur;         /* the job being written, NULL when none */
    int            quit;
    int            failed;      /* thread creation failed once: write inline */
    png_job_t*     req[PNG_ASYNC_REQUESTS];   /* requested jobs no image is captured for yet, FIFO */
    int            req_head, req_n;
} s_aw;

/** @brief Convert a borrowed B,G,R,A or R,G,B,A image into an owned R,G,B,A one with opaque alpha, then return the borrowed image.
 *
 * The conversion is the RHI readback swizzle: red and blue swapped for
 * B,G,R,A, alpha forced to 0xFF.
 * @param j The job; j->px receives the image, NULL when it cannot be allocated. */
static void png_job_take_src(png_job_t* j) {
    const size_t n = (size_t)j->w * (size_t)j->h;
    j->px = (unsigned char*)malloc(n * 4u);
    if (j->px) {
        const unsigned char* src = j->src;
        unsigned char* dst = j->px;
        const int swap = j->bgra;
        for (size_t i = 0; i < n; i++) {
            const unsigned char c0 = src[i * 4 + 0], c1 = src[i * 4 + 1], c2 = src[i * 4 + 2];
            dst[i * 4 + 0] = swap ? c2 : c0;
            dst[i * 4 + 1] = c1;
            dst[i * 4 + 2] = swap ? c0 : c2;
            dst[i * 4 + 3] = 0xFF;
        }
    }
    j->src = NULL;
    if (j->release) j->release(j->release_ctx, j->release_idx);
}

/** @brief Encode one job's pixels to its PNG, log the line the synchronous path logs, and free the job.
 *
 * Runs on the writer thread, or inline on the caller's when no writer
 * thread is available. OPENPETE_SHOT_WRITE_DELAY_MS lengthens the write by
 * a fixed sleep.
 * @param j The job; freed on return.
 * @param where "writer" or "inline", named in the log line. */
static void png_job_run(png_job_t* j, const char* where) {
    const Uint64 t0 = SDL_GetTicksNS();   /* clock-guard: wall */
    if (j->src) png_job_take_src(j);
    if (!j->px) {
        OPL_ERROR_F("screenshot", "%s screenshot: no memory for %s", j->what, j->path);
        free(j);
        return;
    }
    /* OPENPETE_SHOT_WRITE_DELAY_MS=N adds N ms to the write: a slow file
     * system or an on-access scanner, for the harness that measures which
     * thread waits on it. */
    const char* delay = getenv("OPENPETE_SHOT_WRITE_DELAY_MS");
    if (delay && *delay) SDL_DelayNS((Uint64)strtoul(delay, NULL, 10) * 1000000u);
    const int rc = openpete_write_png_rgba(j->path, j->px, j->w, j->h);
    const double enc_ms = (double)(SDL_GetTicksNS() - t0) / 1e6;   /* clock-guard: wall */
    if (rc == 0)
        OPL_INFO_F("native", "%s screenshot %dx%d -> %s key=%u (readback %.2f ms; encode and write %.2f ms %s)",
                   j->what, j->w, j->h, j->path, j->key, j->readback_ms, enc_ms, where);
    else
        OPL_ERROR_F("screenshot", "%s screenshot: PNG write to %s failed", j->what, j->path);
    free(j->px);
    free(j);
}

/** @brief Writer thread entry: run queued jobs in order until quit is raised and the queue is empty.
 * @param ud Unused.
 * @return 0. */
static int SDLCALL png_writer_main(void* ud) {
    OP_ROLE_BIND(shotpng);
    (void)ud;
    SDL_LockMutex(s_aw.mx);
    for (;;) {
        while (s_aw.n == 0 && !s_aw.quit) SDL_WaitCondition(s_aw.cv, s_aw.mx);
        if (s_aw.n == 0) break;
        png_job_t* j = s_aw.q[s_aw.head];
        s_aw.head = (s_aw.head + 1) % PNG_ASYNC_SLOTS;
        s_aw.n--;
        s_aw.cur = j;
        SDL_UnlockMutex(s_aw.mx);
        png_job_run(j, "on the writer");
        SDL_LockMutex(s_aw.mx);
        s_aw.cur = NULL;
        if (s_aw.n == 0) SDL_BroadcastCondition(s_aw.idle);
    }
    SDL_BroadcastCondition(s_aw.idle);
    SDL_UnlockMutex(s_aw.mx);
    return 0;
}

/** @brief Create the writer's lock objects once; 0 when they cannot be created. */
static int png_async_init(void) {
    if (s_aw.mx) return 1;
    SDL_Mutex* mx = SDL_CreateMutex();
    SDL_Condition* cv = SDL_CreateCondition();
    SDL_Condition* idle = SDL_CreateCondition();
    if (!mx || !cv || !idle) {
        if (mx) SDL_DestroyMutex(mx);
        if (cv) SDL_DestroyCondition(cv);
        if (idle) SDL_DestroyCondition(idle);
        return 0;
    }
    s_aw.cv = cv;
    s_aw.idle = idle;
    s_aw.mx = mx;
    atexit(openpete_png_async_drain);
    return 1;
}

/** @brief Start the writer thread unless it runs, failed to start once, or is quitting; s_aw.mx is held.
 * @return 1 when a writer thread accepts jobs, else 0. */
static int png_writer_ensure_locked(void) {
    if (!s_aw.th && !s_aw.failed && !s_aw.quit) {
        s_aw.th = SDL_CreateThread(png_writer_main, "openpete-shotpng", NULL);
        if (!s_aw.th) {
            s_aw.failed = 1;
            OPL_WARN_F("screenshot", "PNG writer thread not created: %s; writing inline", SDL_GetError());
        }
    }
    return s_aw.th && !s_aw.quit;
}

/** @brief Append a job to the writer's queue and wake the writer; s_aw.mx is held and the queue has room.
 * @param j The job; the writer frees it. */
static void png_enqueue_locked(png_job_t* j) {
    s_aw.q[(s_aw.head + s_aw.n) % PNG_ASYNC_SLOTS] = j;
    s_aw.n++;
    SDL_SignalCondition(s_aw.cv);
}

int openpete_write_png_rgba_async(const char* path, unsigned char* pixels, int w, int h,
                                  const char* what, unsigned key, double readback_ms) {
    if (!path || !pixels || w <= 0 || h <= 0) { free(pixels); return 1; }
    png_job_t* j = (png_job_t*)calloc(1, sizeof *j);
    if (!j) { free(pixels); return 1; }
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->what, sizeof j->what, "%s", what ? what : "offscreen");
    j->px = pixels; j->w = w; j->h = h; j->key = key; j->readback_ms = readback_ms;
    int queued = 0;
    if (!openpete_png_async_inline() && png_async_init()) {
        SDL_LockMutex(s_aw.mx);
        if (png_writer_ensure_locked() && s_aw.n < PNG_ASYNC_SLOTS) {
            png_enqueue_locked(j);
            queued = 1;
        }
        SDL_UnlockMutex(s_aw.mx);
    }
    if (!queued) png_job_run(j, "inline");
    return 0;
}

int openpete_png_async_inline(void) {
    const char* sync = getenv("OPENPETE_SHOT_SYNC");
    return (sync && *sync && *sync != '0') ? 1 : 0;
}

int openpete_png_request(const char* path, const char* what) {
    if (!path || !*path || !png_async_init()) return 1;
    png_job_t* j = (png_job_t*)calloc(1, sizeof *j);
    if (!j) return 1;
    snprintf(j->path, sizeof j->path, "%s", path);
    snprintf(j->what, sizeof j->what, "%s", what ? what : "offscreen");
    j->t_req = SDL_GetTicksNS();   /* clock-guard: wall */
    SDL_LockMutex(s_aw.mx);
    const int n = s_aw.req_n;
    if (n < PNG_ASYNC_REQUESTS) {
        s_aw.req[(s_aw.req_head + n) % PNG_ASYNC_REQUESTS] = j;
        s_aw.req_n++;
    }
    SDL_UnlockMutex(s_aw.mx);
    if (n < PNG_ASYNC_REQUESTS) return 0;
    OPL_WARN_F("screenshot", "%s screenshot %s dropped: %d requests pending", j->what, path, n);
    free(j);
    return 1;
}

int openpete_png_requests_pending(void) {
    if (!s_aw.mx) return 0;
    SDL_LockMutex(s_aw.mx);
    const int n = s_aw.req_n;
    SDL_UnlockMutex(s_aw.mx);
    return n;
}

void* openpete_png_request_take(unsigned key) {
    if (!s_aw.mx) return NULL;
    png_job_t* j = NULL;
    SDL_LockMutex(s_aw.mx);
    if (s_aw.req_n > 0) {
        const int k = s_aw.req_head;
        j = s_aw.req[k];
        s_aw.req[k] = NULL;
        s_aw.req_head = (k + 1) % PNG_ASYNC_REQUESTS;
        s_aw.req_n--;
        j->key = key;
    }
    SDL_UnlockMutex(s_aw.mx);
    return j;
}

int openpete_png_request_submit(void* job, const void* src, int w, int h, int bgra,
                                openpete_png_release_fn release, void* release_ctx,
                                int release_idx) {
    png_job_t* j = (png_job_t*)job;
    if (!j || !src || w <= 0 || h <= 0 || !s_aw.mx) {
        if (release) release(release_ctx, release_idx);
        free(j);
        return 1;
    }
    SDL_LockMutex(s_aw.mx);
    const int writer = png_writer_ensure_locked();
    if (writer && s_aw.n >= PNG_ASYNC_SLOTS) {
        SDL_UnlockMutex(s_aw.mx);
        return 2;
    }
    j->src = (const unsigned char*)src; j->bgra = bgra;
    j->release = release; j->release_ctx = release_ctx; j->release_idx = release_idx;
    j->w = w; j->h = h;
    /* The span logged as the readback: request to hand-off. */
    j->readback_ms = (double)(SDL_GetTicksNS() - j->t_req) / 1e6;   /* clock-guard: wall */
    if (writer) png_enqueue_locked(j);
    SDL_UnlockMutex(s_aw.mx);
    if (!writer) png_job_run(j, "inline");
    return 0;
}

void openpete_png_request_cancel(void* job, const char* why) {
    png_job_t* j = (png_job_t*)job;
    if (!j) return;
    OPL_WARN_F("screenshot", "%s screenshot %s not taken: %s", j->what, j->path, why ? why : "cancelled");
    free(j);
}

int openpete_png_async_pending(const char* path) {
    if (!path || !s_aw.mx) return 0;
    int hit = 0;
    SDL_LockMutex(s_aw.mx);
    if (s_aw.cur && strcmp(s_aw.cur->path, path) == 0) hit = 1;
    for (int i = 0; !hit && i < s_aw.req_n; i++)
        if (strcmp(s_aw.req[(s_aw.req_head + i) % PNG_ASYNC_REQUESTS]->path, path) == 0) hit = 1;
    for (int i = 0; !hit && i < s_aw.n; i++)
        if (strcmp(s_aw.q[(s_aw.head + i) % PNG_ASYNC_SLOTS]->path, path) == 0) hit = 1;
    SDL_UnlockMutex(s_aw.mx);
    return hit;
}

void openpete_png_async_drain(void) {
    if (!s_aw.mx) return;
    SDL_LockMutex(s_aw.mx);
    SDL_Thread* th = s_aw.th;
    s_aw.quit = 1;
    s_aw.th = NULL;
    SDL_BroadcastCondition(s_aw.cv);
    SDL_UnlockMutex(s_aw.mx);
    if (th) SDL_WaitThread(th, NULL);
    SDL_LockMutex(s_aw.mx);
    s_aw.quit = 0;   /* a later request starts a new writer */
    SDL_UnlockMutex(s_aw.mx);
}

int openpete_screenshot_save_png(SDL_Renderer* renderer, const char* path) {
    if (!renderer || !path) return -1;

    /* SDL3 SDL_RenderReadPixels allocates and returns a surface in the
     * renderer's native format. Normalise to RGBA8888 so stb_image_write
     * gets 4 channels per pixel and the BMP path has a known layout. */
    SDL_Surface* raw = SDL_RenderReadPixels(renderer, NULL);
    if (!raw) {
        OPL_ERROR_F("screenshot", "SDL_RenderReadPixels: %s", SDL_GetError());
        return -1;
    }
    SDL_Surface* surface = (raw->format == SDL_PIXELFORMAT_RGBA32)
        ? raw
        : SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGBA32);
    if (surface != raw) SDL_DestroySurface(raw);
    if (!surface) {
        OPL_ERROR_F("screenshot", "SDL_ConvertSurface: %s", SDL_GetError());
        return -1;
    }

    const int w = surface->w, h = surface->h;
    int rc = 0;

    /* SDL_RenderReadPixels gives top-down; both PNG and SDL_SaveBMP want
     * top-down, so no flip needed. */
    if (openpete_screenshot_path_is_png(path)) {
        if (openpete_write_png_rgba_stride(path, surface->pixels, w, h, surface->pitch) != 0) {
            OPL_ERROR_F("screenshot", "PNG write to %s failed", path);
            rc = -1;
        } else {
            OPL_INFO_F("screenshot", "saved %dx%d PNG to %s", w, h, path);
        }
    } else {
        if (!SDL_SaveBMP(surface, path)) {
            OPL_ERROR_F("screenshot", "SDL_SaveBMP to %s failed: %s",
                        path, SDL_GetError());
            rc = -1;
        } else {
            OPL_INFO_F("screenshot", "saved %dx%d BMP to %s", w, h, path);
        }
    }

    SDL_DestroySurface(surface);
    return rc;
}

void openpete_screenshot_format_path(const char* tmpl, int frame,
                                     int present,
                                     char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    if (!tmpl) { out[0] = '\0'; return; }
    size_t o = 0;
    for (size_t i = 0; tmpl[i] && o + 1 < out_sz; ) {
        /* tmpl[i+2] is read only after tmpl[i+1] matched a non-NUL byte, so
         * a template ending in '{' never reads past its terminator. */
        if (tmpl[i] == '{' && (tmpl[i+1] == 'n' || tmpl[i+1] == 'p')
                && tmpl[i+2] == '}') {
            int val = (tmpl[i+1] == 'n') ? frame : present;
            int n = snprintf(out + o, out_sz - o, "%d", val);
            if (n < 0) break;
            o += (size_t)n;
            if (o >= out_sz) o = out_sz - 1;
            i += 3;
        } else {
            out[o++] = tmpl[i++];
        }
    }
    out[o < out_sz ? o : out_sz - 1] = '\0';
}

int openpete_screenshot_path_is_png(const char* path) {
    if (!path) return 0;
    size_t n = strlen(path);
    if (n < 4) return 0;
    const char* e = path + n - 4;
    return (e[0] == '.'
            && tolower((unsigned char)e[1]) == 'p'
            && tolower((unsigned char)e[2]) == 'n'
            && tolower((unsigned char)e[3]) == 'g');
}

int openpete_screenshot_user_path(char* out, size_t cap) {
    if (!out || cap == 0) return -1;
    out[0] = '\0';

    /* Screenshots are output, so the directory has to be writable: next to the
     * exe in a portable install, under the per-user data directory otherwise -
     * which is also where a read-only install medium (an AppImage mount,
     * Program Files) lands. A relative path is the last resort, so a run whose
     * exe location cannot be determined still saves something; under a
     * sandbox the shot is refused instead, since the working directory lies
     * outside the root. */
    char dir[1024];
    if (openpete_paths_data_dir("screenshots", dir, sizeof dir, 0) != 0) {
        if (openpete_paths_sandbox()) {
            OPL_ERROR_F("screenshot", "%s", "no screenshots directory resolves under the sandbox root");
            return -1;
        }
        snprintf(dir, sizeof dir, "screenshots");
    }
    if (!SDL_CreateDirectory(dir)) {  /* mkdir -p; true if it already exists */
        OPL_ERROR_F("screenshot", "could not create %s: %s", dir, SDL_GetError());
        return -1;
    }

    char stamp[24];
    time_t now = time(NULL);
    /* localtime() (not _r/_s): this runs once per keypress on the host loop,
     * so the shared-buffer caveat doesn't apply, and it avoids the
     * platform-specific reentrant variants. */
    struct tm* tmv = localtime(&now);
    if (!tmv || strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tmv) == 0)
        snprintf(stamp, sizeof stamp, "screenshot");

    /* One-second stamp resolution, so a second shot within the same second
     * would overwrite the first: suffix until the name is free, a name a
     * queued asynchronous write still holds counting as taken. */
    for (int n = 1; n < 1000; n++) {
        char leaf[64];
        if (n == 1) snprintf(leaf, sizeof leaf, "openpete-%s.png", stamp);
        else        snprintf(leaf, sizeof leaf, "openpete-%s-%d.png", stamp, n);
        openpete_paths_join(out, cap, dir, leaf);
        SDL_PathInfo info;
        /* Nothing there and no queued write will create it: take it. */
        if (!SDL_GetPathInfo(out, &info) && !openpete_png_async_pending(out)) return 0;
    }
    OPL_ERROR_F("screenshot", "no free filename for %s/openpete-%s", dir, stamp);
    out[0] = '\0';
    return -1;
}
