/* native_fb_snapshot.c - see openpete_fb_snapshot.h. */

#include "openpete_fb_snapshot.h"
#include "openpete_native_vram.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_mutex.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* The scene a copy's pixels come from is the one the game's render last fed,
 * which the scene ring stamps with its own frame. That stamp lags the VBL
 * counter across the tick's tail: the ordering-table walk keeps feeding the
 * open scene while openpete_frame has already moved on, so a copy taken after
 * the walk belongs to that scene, not to the VBL it happens on. A sub-tick
 * re-extract opens scenes of its own between the game's renders; they are not
 * the buffer's content, so the stamp is the game render's
 * (openpete_native_scene_game_frame), not the open scene's. */
extern unsigned openpete_frame;
extern unsigned openpete_native_scene_game_frame(void) OP_REQUIRES_SHARED(sim);

/** @brief Return the frame stamp a copy taken now belongs to.
 *
 * The stamp of the scene the game's render last fed, else openpete_frame
 * before the first feed (the ring reports 0xFFFFFFFF then). */
static unsigned fb_copy_frame(void) OP_REQUIRES_SHARED(sim)
{
    const unsigned live = openpete_native_scene_game_frame();
    return (live != 0xFFFFFFFFu) ? live : openpete_frame;
}

/* Stamp of the newest completed scene: the game render's scene at the last
 * canonical capture, 0xFFFFFFFF before the first. The games copy a window
 * out of a display buffer after DrawSync, which waits for the ordering table
 * of the last completed tick; by then the next tick's feeders may already
 * have opened its scene, and its 2D lands in that scene after the copy. */
static unsigned s_done_frame = 0xFFFFFFFFu;

void openpete_fb_snapshot_note_scene_complete(void) OP_REQUIRES_SHARED(sim)
{
    if (openpete_fb_snapshot_enabled()) s_done_frame = fb_copy_frame();
}

/** @brief Return the stamp a framebuffer window copy names: the newest completed scene.
 *
 * Falls back to fb_copy_frame before the first canonical capture. */
static unsigned fb_window_frame(void) OP_REQUIRES_SHARED(sim)
{
    return (s_done_frame != 0xFFFFFFFFu) ? s_done_frame : fb_copy_frame();
}

/* One slot per live window. A screen holds at most one; the spare slots cover a
 * screen that keeps two copies (a before/after pair) without evicting either. */
#define FB_SNAP_MAX OPENPETE_FB_SNAPSHOT_AREA_BASE

/* The two display buffers occupy the left half of VRAM (a 512-texel-wide
 * framebuffer at y 0 and y 256); everything at x >= 512 is texture and scratch
 * space. A window copy runs from the first region to the second. */
#define FB_BUFFER_W  512
#define FB_BUFFER_H  256

typedef struct {
    int dst_x, dst_y, w, h;   /* VRAM texel rect the copy landed on */
    int src_x, src_y;         /* copy source, in framebuffer screen pixels */
    int pending;              /* waiting for a present to crop its pixels */
    unsigned frame;           /* the frame the copy was taken on */
    unsigned char *px;        /* RGBA8 crop of the native composed frame */
    int pw, ph;
    uint64_t key;             /* unique per stored image (see the header) */
    uint16_t *under;          /* the w x h VRAM texels the copy overwrote */
} fb_snap_t;

static fb_snap_t s_slot[FB_SNAP_MAX];

/* Guards every image pointer, size and key in s_slot and s_area. The guest
 * thread is the only writer (moves, draw-env puts, stores); the presenting
 * thread reads images through openpete_fb_snapshot_image and
 * openpete_fb_snapshot_copy, which hold it for the read, so no image is
 * freed while it is copied. Guest-thread reads need no lock. */
static SDL_Mutex *s_px_mx;

/** @brief Return the image mutex, creating it on first use; a compare-and-swap publishes exactly one.
 *
 * @return The mutex. */
static SDL_Mutex *px_mx(void)
{
    SDL_Mutex *m = (SDL_Mutex *)SDL_GetAtomicPointer((void **)&s_px_mx);
    if (m) return m;
    SDL_Mutex *n = SDL_CreateMutex();
    if (!SDL_CompareAndSwapAtomicPointer((void **)&s_px_mx, NULL, n)) SDL_DestroyMutex(n);
    return (SDL_Mutex *)SDL_GetAtomicPointer((void **)&s_px_mx);
}
static uint64_t  s_key_next = 1;
static unsigned  s_taken, s_resolved, s_quads, s_area_hits;

/* A draw area the game put, with the native frame that stands for the content
 * of the buffer it covers. Two areas serve a double-buffered game; the spares
 * cover a screen that puts a third (an off-screen render target). */
#define FB_AREA_MAX OPENPETE_FB_SNAPSHOT_AREA_MAX
typedef struct {
    int x, y, w, h;           /* DRAWENV clip rect, VRAM texels */
    int ofx, ofy;             /* DRAWENV drawing offset */
    int live;                 /* the game has put this area */
    int pending;              /* waiting for a present to stand for its content */
    unsigned frame;           /* the frame the copy that put it was taken on */
    unsigned char *px;        /* RGBA8 crop of that present over the area's rect */
    int pw, ph;
    uint64_t key;             /* unique per stored image (see the header) */
    int retained;             /* last put with auto-clear off: the buffer keeps what it holds */
    unsigned epoch;           /* bumped whenever the buffer's held content is replaced */
    unsigned drawn;           /* stamp of the scene the last put drew into the buffer */
    int drawn_valid;          /* a put has named a completed scene for the buffer */
    int sampled;              /* the image was taken for a quad sampling the buffer (fb_pend_sampled_area) */
} fb_area_t;
static fb_area_t s_area[FB_AREA_MAX];
static int       s_area_next;   /* the slot a new clip rect takes when all are live */
static int       s_area_cur = -1;  /* the area the game last put: the buffer it draws */

/** @brief Compute the mean and standard deviation of an RGBA8 image's luma, for the census.
 *
 * Luma is the unweighted mean of R, G and B. An image that is one clear
 * colour has a spread near zero; an image carrying the world does not. An
 * empty image yields zero for both.
 * @param px RGBA8 pixels.
 * @param pw Image width.
 * @param ph Image height.
 * @param mean Receives the mean luma.
 * @param spread Receives the standard deviation of luma. */
static void fb_image_stats(const unsigned char *px, int pw, int ph,
                           double *mean, double *spread)
{
    const size_t n = (size_t)pw * (size_t)ph;
    double s = 0.0, s2 = 0.0;
    for (size_t i = 0; i < n; i++) {
        const double l = (px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3.0;
        s += l; s2 += l * l;
    }
    *mean = n ? s / (double)n : 0.0;
    const double v = n ? s2 / (double)n - (*mean) * (*mean) : 0.0;
    *spread = v > 0.0 ? sqrt(v) : 0.0;
}

/** @brief Report whether the snapshot census is on, reading OPENPETE_FBSNAP_CENSUS once.
 *
 * On when the variable is set to a non-empty value other than "0"; the
 * result is latched at the first call. The census logs each draw
 * environment, area image and window image to stderr.
 * @return 1 when on, else 0. */
static int fbsnap_census(void)
{
    static int s = -1;
    if (s < 0) { const char *e = getenv("OPENPETE_FBSNAP_CENSUS"); s = (e && *e && *e != '0'); }
    return s;
}

/** @brief Compute the screen origin an area covers: its clip rect origin less its drawing offset.
 *
 * @param a Draw area.
 * @param sx Receives the screen x.
 * @param sy Receives the screen y. */
static void fb_area_screen(const fb_area_t *a, int *sx, int *sy)
{ *sx = a->x - a->ofx; *sy = a->y - a->ofy; }

/** @brief Free an area's image and clear its pending flag, under the image mutex.
 *
 * @param a Draw area. */
static void fb_area_drop(fb_area_t *a)
{
    SDL_LockMutex(px_mx());
    free(a->px); a->px = NULL; a->pw = a->ph = 0; a->pending = 0;
    SDL_UnlockMutex(s_px_mx);
}

void openpete_fb_snapshot_note_draw_env(int clip_x, int clip_y, int clip_w, int clip_h,
                                        int ofs_x, int ofs_y, int isbg)
{
    if (!openpete_fb_snapshot_enabled()) return;
    if (clip_w <= 0 || clip_h <= 0) return;
    int idx = -1;
    for (int i = 0; i < FB_AREA_MAX && idx < 0; i++)
        if (s_area[i].live && s_area[i].x == clip_x && s_area[i].y == clip_y &&
            s_area[i].w == clip_w && s_area[i].h == clip_h) idx = i;
    if (idx < 0)
        for (int i = 0; i < FB_AREA_MAX && idx < 0; i++)
            if (!s_area[i].live) idx = i;
    if (idx < 0) { idx = s_area_next; s_area_next = (s_area_next + 1) % FB_AREA_MAX; }
    fb_area_t *a = &s_area[idx];
    if (fbsnap_census())
        fprintf(stderr, "[fbsnap] drawenv area %d clip=%d,%d %dx%d ofs=%d,%d isbg=%d had_px=%d\n",
                idx, clip_x, clip_y, clip_w, clip_h, ofs_x, ofs_y, isbg, a->px != NULL);
    /* Auto-clear on: the buffer's next frame starts blank, so whatever image
     * stood for its content is gone. Auto-clear off: the game draws over what
     * the buffer holds, and a copy it has just taken stands until the buffer is
     * cleared again. */
    if (isbg) fb_area_drop(a);
    /* A new clip rect in the slot, an auto-cleared frame, or the first put of a
     * retained run each start the buffer's content afresh. */
    const int same = a->live && a->x == clip_x && a->y == clip_y && a->w == clip_w && a->h == clip_h;
    if (!same || isbg || !a->retained) a->epoch++;
    /* The scene the put draws is the one completed last: the game captures
     * a scene at its VSync and then puts the area and draws that scene's
     * ordering table into it. An image taken for a sampling quad stood for
     * the buffer before this draw, so it is dropped. */
    if (a->sampled) { fb_area_drop(a); a->sampled = 0; }
    a->drawn = s_done_frame;
    a->drawn_valid = s_done_frame != 0xFFFFFFFFu;
    a->retained = !isbg;
    a->x = clip_x; a->y = clip_y; a->w = clip_w; a->h = clip_h;
    a->ofx = ofs_x; a->ofy = ofs_y; a->live = 1;
    s_area_cur = idx;
}

void openpete_fb_snapshot_on_fill(int x, int y, int w, int h)
{
    openpete_native_hold_note_vram(x, y, w, h, -1, 0);
    if (!openpete_fb_snapshot_enabled()) return;
    for (int i = 0; i < FB_AREA_MAX; i++) {
        fb_area_t *a = &s_area[i];
        if (!a->live) continue;
        if (x <= a->x && y <= a->y && x + w >= a->x + a->w && y + h >= a->y + a->h) a->epoch++;
    }
}

void openpete_fb_snapshot_note_area_drawn(unsigned frame)
{
    if (!openpete_fb_snapshot_enabled() || s_area_cur < 0) return;
    fb_area_t *a = &s_area[s_area_cur];
    if (!a->live) return;
    a->drawn = frame;
    a->drawn_valid = 1;
}

int openpete_fb_snapshot_retained_area(unsigned *epoch)
{
    if (!openpete_fb_snapshot_enabled() || s_area_cur < 0) return -1;
    const fb_area_t *a = &s_area[s_area_cur];
    if (!a->live || !a->retained) return -1;
    if (epoch) *epoch = a->epoch;
    return s_area_cur;
}

/** @brief Find the first live draw area whose clip rect contains a VRAM rect.
 *
 * @param x Rect left, VRAM texels.
 * @param y Rect top.
 * @param w Rect width.
 * @param h Rect height.
 * @return The area, or NULL when none contains the rect. */
static fb_area_t *fb_area_holding(int x, int y, int w, int h)
{
    for (int i = 0; i < FB_AREA_MAX; i++) {
        fb_area_t *a = &s_area[i];
        if (!a->live) continue;
        if (x < a->x || y < a->y || x + w > a->x + a->w || y + h > a->y + a->h) continue;
        return a;
    }
    return NULL;
}

int openpete_fb_snapshot_area_holds(int x, int y, int w, int h)
{
    return openpete_fb_snapshot_enabled() && fb_area_holding(x, y, w, h) != NULL;
}

int openpete_fb_snapshot_enabled(void)
{
    static int s_en = -1;
    if (s_en < 0) { const char *e = getenv("OPENPETE_FBSNAP"); s_en = !(e && *e == '0'); }
    return s_en;
}

/** @brief Crop the part of an area's content image that covers a screen rect.
 *
 * The image stands for the screen rect at (asx, asy) of size aw by ah; the
 * requested rect is mapped into image pixels by that scale, clamped to the
 * image, and its rows are copied unscaled, so the result is at the image's
 * resolution.
 * @param src RGBA8 source image.
 * @param sw Source width in pixels.
 * @param sh Source height in pixels.
 * @param asx Screen x the image's left edge stands for.
 * @param asy Screen y the image's top edge stands for.
 * @param aw Screen width the image stands for.
 * @param ah Screen height the image stands for.
 * @param sx Requested rect left, screen pixels.
 * @param sy Requested rect top.
 * @param w Requested rect width.
 * @param h Requested rect height.
 * @param out_w Receives the crop width.
 * @param out_h Receives the crop height.
 * @return A malloc'd RGBA8 buffer, or NULL when the area size is not
 *         positive, the rect falls outside the image, or allocation fails. */
static unsigned char *fb_crop_image(const unsigned char *src, int sw, int sh,
                                    int asx, int asy, int aw, int ah,
                                    int sx, int sy, int w, int h,
                                    int *out_w, int *out_h)
{
    if (aw <= 0 || ah <= 0) return NULL;
    int x0 = (sx - asx) * sw / aw, x1 = (sx + w - asx) * sw / aw;
    int y0 = (sy - asy) * sh / ah, y1 = (sy + h - asy) * sh / ah;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    const int pw = x1 - x0, ph = y1 - y0;
    if (pw <= 0 || ph <= 0) return NULL;
    unsigned char *px = (unsigned char *)malloc((size_t)pw * (size_t)ph * 4u);
    if (!px) return NULL;
    for (int row = 0; row < ph; row++)
        memcpy(px + (size_t)row * pw * 4u,
               src + ((size_t)(y0 + row) * sw + x0) * 4u, (size_t)pw * 4u);
    *out_w = pw; *out_h = ph;
    return px;
}

/** @brief Claim the window slot for a destination rect and set the rect on it.
 *
 * Takes the slot already holding that rect, else one with no pixels and
 * nothing pending, else slot 0. The slot's stored pixels are kept until
 * new ones arrive, so a quad fed between the move and its resolve draws
 * the previous window rather than the transparent VRAM copy.
 * @param dst_x Destination left, VRAM texels.
 * @param dst_y Destination top.
 * @param w Width in texels.
 * @param h Height in texels.
 * @return The claimed slot. */
static fb_snap_t *fb_window_slot(int dst_x, int dst_y, int w, int h)
{
    int idx = -1;
    for (int i = 0; i < FB_SNAP_MAX; i++)
        if (s_slot[i].dst_x == dst_x && s_slot[i].dst_y == dst_y &&
            s_slot[i].w == w && s_slot[i].h == h) { idx = i; break; }
    if (idx < 0)
        for (int i = 0; i < FB_SNAP_MAX; i++)
            if (!s_slot[i].px && !s_slot[i].pending) { idx = i; break; }
    if (idx < 0) idx = 0;
    fb_snap_t *s = &s_slot[idx];
    s->dst_x = dst_x; s->dst_y = dst_y; s->w = w; s->h = h;
    return s;
}

/** @brief Handle a copy from one draw area into another that it covers whole.
 *
 * This is the game moving a finished image into the buffer it is not
 * drawing. A copy that does not match the destination's clip rect exactly
 * is ignored. Otherwise the destination's image is dropped and it takes a
 * copy of the source area's content image with a new key; when the source
 * has none, or the copy allocation fails, the destination waits for the
 * next present, and with no source image the source waits too, both
 * stamped with the current scene.
 * @param from Source draw area.
 * @param to Destination draw area.
 * @param dst_x Destination left, VRAM texels.
 * @param dst_y Destination top.
 * @param w Width in texels.
 * @param h Height in texels. */
static void fb_move_area_to_area(fb_area_t *from, fb_area_t *to,
                                 int dst_x, int dst_y, int w, int h) OP_REQUIRES_SHARED(sim)
{
    if (dst_x != to->x || dst_y != to->y || w != to->w || h != to->h) return;
    fb_area_drop(to);
    to->epoch++;   /* the copy replaced everything the destination held */
    if (!from->px) {
        /* The source is the buffer being drawn, so the present composed next
         * is the frame this copy moved. After it both buffers hold that frame:
         * the destination by the copy, the source because the game has stopped
         * drawing into it. One capture stands for both. */
        to->pending = 1;
        from->pending = 1;
        to->frame = from->frame = fb_copy_frame();
        return;
    }
    const size_t n = (size_t)from->pw * (size_t)from->ph * 4u;
    unsigned char *px = (unsigned char *)malloc(n);
    if (px) memcpy(px, from->px, n);
    SDL_LockMutex(px_mx());
    if (px) { to->px = px; to->pw = from->pw; to->ph = from->ph; to->key = s_key_next++; }
    else { to->pw = to->ph = 0; to->pending = 1; }
    SDL_UnlockMutex(s_px_mx);
}

/** @brief Keep the VRAM texels a window copy is about to overwrite.
 *
 * Runs before the copy lands in the VRAM model. The scene the copy names was
 * drawn with these texels in place: its textures and CLUTs may lie in the
 * destination page, which the game reuses as scratch while the world is
 * frozen. A failed allocation leaves the slot without texels.
 * @param s The window slot, its destination rect already set. */
static void fb_save_under(fb_snap_t *s)
{
    const size_t n = (size_t)s->w * (size_t)s->h;
    uint16_t *u = (uint16_t *)realloc(s->under, n * sizeof *u);
    s->under = u;
    if (!u) return;
    const uint16_t *vr = openpete_vram_data();
    for (int row = 0; row < s->h; row++)
        memcpy(u + (size_t)row * s->w,
               vr + (size_t)(s->dst_y + row) * OPENPETE_VRAM_W + s->dst_x,
               (size_t)s->w * sizeof *u);
}

const uint16_t *openpete_fb_snapshot_under(int idx, int *x, int *y, int *w, int *h)
{
    if (idx < 0 || idx >= FB_SNAP_MAX || !s_slot[idx].under) return NULL;
    const fb_snap_t *s = &s_slot[idx];
    if (x) *x = s->dst_x;
    if (y) *y = s->dst_y;
    if (w) *w = s->w;
    if (h) *h = s->h;
    return s->under;
}

int openpete_fb_snapshot_on_move(int src_x, int src_y, int w, int h,
                                 int dst_x, int dst_y) OP_REQUIRES_SHARED(sim)
{
    openpete_native_hold_note_vram(dst_x, dst_y, w, h, src_x, src_y);
    if (!openpete_fb_snapshot_enabled()) return 0;
    if (w <= 0 || h <= 0) return 0;
    if (src_x + w > FB_BUFFER_W) return 0;

    fb_area_t *from = fb_area_holding(src_x, src_y, w, h);
    int ofx = 0, ofy = 0;
    if (from) { ofx = from->ofx; ofy = from->ofy; }
    else {
        /* No draw area recorded yet: the display buffers split at VRAM row 256
         * with no drawing offset. */
        ofy = (src_y >= FB_BUFFER_H) ? FB_BUFFER_H : 0;
        if (src_y + h > ofy + FB_BUFFER_H) return 0;
    }
    const int ssx = src_x - ofx, ssy = src_y - ofy;

    if (dst_x < FB_BUFFER_W) {
        fb_area_t *to = fb_area_holding(dst_x, dst_y, w, h);
        if (from && to && to != from) fb_move_area_to_area(from, to, dst_x, dst_y, w, h);
        return 0;
    }

    fb_snap_t *s = fb_window_slot(dst_x, dst_y, w, h);
    s->src_x = ssx; s->src_y = ssy;
    s->frame = fb_window_frame();
    fb_save_under(s);
    s_taken++;
    const fb_area_t *cur = (s_area_cur >= 0) ? &s_area[s_area_cur] : NULL;
    if (from && from->px && from != cur) {
        /* The copied buffer is not the one the game is drawing into, so no
         * present will compose it: crop the frame that stands for its content.
         * The buffer being drawn keeps the next-present rule, since the present
         * that follows this move composes exactly what the copy took. */
        int asx, asy, pw = 0, ph = 0;
        fb_area_screen(from, &asx, &asy);
        unsigned char *px = fb_crop_image(from->px, from->pw, from->ph,
                                          asx, asy, from->w, from->h,
                                          ssx, ssy, w, h, &pw, &ph);
        if (px) {
            s->pending = 0;
            openpete_fb_snapshot_store((int)(s - s_slot), px, pw, ph);
            s_area_hits++;
            if (fbsnap_census())
                fprintf(stderr, "[fbsnap] window %d,%d %dx%d from area clip=%d,%d "
                                "image=%dx%d hits=%u\n",
                        ssx, ssy, w, h, from->x, from->y, pw, ph, s_area_hits);
            return 1;
        }
    }
    s->pending = 1;
    return 1;
}

int openpete_fb_snapshot_pending(void)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) if (s_slot[i].pending) return 1;
    for (int i = 0; i < FB_AREA_MAX; i++) if (s_area[i].pending) return 1;
    return 0;
}

int openpete_fb_snapshot_take_pending_area(int *idx, int *src_x, int *src_y,
                                           int *w, int *h, unsigned *frame)
{
    for (int i = 0; i < FB_AREA_MAX; i++) {
        if (!s_area[i].pending) continue;
        s_area[i].pending = 0;
        int sx, sy; fb_area_screen(&s_area[i], &sx, &sy);
        if (frame) *frame = s_area[i].frame;
        if (idx)   *idx   = i;
        if (src_x) *src_x = sx;
        if (src_y) *src_y = sy;
        if (w)     *w     = s_area[i].w;
        if (h)     *h     = s_area[i].h;
        return 1;
    }
    return 0;
}

void openpete_fb_snapshot_resolve_failed(int idx)
{
    unsigned char **px; int *pw, *ph, *pending;
    if (idx >= 0 && idx < FB_SNAP_MAX) {
        fb_snap_t *s = &s_slot[idx];
        px = &s->px; pw = &s->pw; ph = &s->ph; pending = &s->pending;
    } else if (idx >= FB_SNAP_MAX && idx - FB_SNAP_MAX < FB_AREA_MAX) {
        fb_area_t *a = &s_area[idx - FB_SNAP_MAX];
        px = &a->px; pw = &a->pw; ph = &a->ph; pending = &a->pending;
    } else {
        return;
    }
    SDL_LockMutex(px_mx());
    free(*px); *px = NULL; *pw = *ph = 0;
    SDL_UnlockMutex(s_px_mx);
    *pending = 1;
}

void openpete_fb_snapshot_store_area(int idx, unsigned char *px, int pw, int ph)
{
    if (idx < 0 || idx >= FB_AREA_MAX || !px || pw <= 0 || ph <= 0) { free(px); return; }
    fb_area_t *a = &s_area[idx];
    SDL_LockMutex(px_mx());
    free(a->px);
    a->px = px; a->pw = pw; a->ph = ph;
    a->key = s_key_next++;
    SDL_UnlockMutex(s_px_mx);
    if (fbsnap_census()) {
        double m = 0.0, sd = 0.0;
        fb_image_stats(px, pw, ph, &m, &sd);
        fprintf(stderr, "[fbsnap] area %d clip=%d,%d %dx%d ofs=%d,%d image=%dx%d "
                        "luma=%.1f spread=%.1f\n",
                idx, a->x, a->y, a->w, a->h, a->ofx, a->ofy, pw, ph, m, sd);
    }
}

int openpete_fb_snapshot_take_pending(int *idx, int *src_x, int *src_y,
                                      int *w, int *h, unsigned *frame)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) {
        if (!s_slot[i].pending) continue;
        s_slot[i].pending = 0;
        if (frame) *frame = s_slot[i].frame;
        if (idx)   *idx   = i;
        if (src_x) *src_x = s_slot[i].src_x;
        if (src_y) *src_y = s_slot[i].src_y;
        if (w)     *w     = s_slot[i].w;
        if (h)     *h     = s_slot[i].h;
        return 1;
    }
    return 0;
}

void openpete_fb_snapshot_store(int idx, unsigned char *px, int pw, int ph)
{
    if (idx < 0 || idx >= FB_SNAP_MAX) { free(px); return; }
    if (!px || pw <= 0 || ph <= 0) { free(px); return; }
    fb_snap_t *s = &s_slot[idx];
    SDL_LockMutex(px_mx());
    free(s->px);
    s->px = px; s->pw = pw; s->ph = ph;
    s->key = s_key_next++;
    SDL_UnlockMutex(s_px_mx);
    s_resolved++;
    if (fbsnap_census()) {
        double m = 0.0, sd = 0.0;
        fb_image_stats(px, pw, ph, &m, &sd);
        fprintf(stderr, "[fbsnap] window slot %d rect=%d,%d %dx%d image=%dx%d "
                        "luma=%.1f spread=%.1f\n",
                idx, s->src_x, s->src_y, s->w, s->h, pw, ph, m, sd);
    }
}

/** @brief Test whether a quad's texel box, offset from a stored rect's origin, lies inside the rect.
 *
 * The page origin may precede the rect (a copy landed at row 12 of a page
 * is drawn with v 12..120), so only the box is bounded. A quad's far corner
 * names the edge past its last texel (a 128-row copy is drawn with v
 * 0..128), so the far bound is inclusive of the edge.
 * @param du Page origin x less the rect's x.
 * @param dv Page origin y less the rect's y.
 * @param u0 Texel box left.
 * @param v0 Texel box top.
 * @param u1 Texel box right edge.
 * @param v1 Texel box bottom edge.
 * @param w Rect width.
 * @param h Rect height.
 * @return 1 when the box lies inside, 0 otherwise. */
static int fb_rect_holds(int du, int dv, int u0, int v0, int u1, int v1, int w, int h)
{
    return du + u0 >= 0 && dv + v0 >= 0 && du + u1 <= w && dv + v1 <= h;
}

/** @brief Compute the mapping from page texels to a stored rect's image, or to the rect's texels when it holds no image yet.
 *
 * With an image the scale is image size over rect size per axis and the
 * offset the page-to-rect displacement times that scale, so a UV is a pixel
 * position in the image. A rect still waiting for its capture has no image
 * size, so the scale is 1 and a UV is a texel of the rect; the draw maps
 * either onto the image it finds (texel_space tells it which). Any output
 * pointer may be NULL.
 * @param du Page origin x less the rect's x.
 * @param dv Page origin y less the rect's y.
 * @param pw Image width in pixels, 0 when there is no image.
 * @param ph Image height in pixels.
 * @param w Rect width in texels.
 * @param h Rect height in texels.
 * @param su Receives the x scale.
 * @param sv Receives the y scale.
 * @param ox Receives the mapped x of the page origin.
 * @param oy Receives the mapped y of the page origin.
 * @param texel_space Receives 1 for the rect-texel mapping, 0 for image pixels. */
static void fb_fill_map(int du, int dv, int pw, int ph, int w, int h,
                        float *su, float *sv, float *ox, float *oy, int *texel_space)
{
    const int img = pw > 0 && ph > 0;
    const float sx = img ? (float)pw / (float)w : 1.0f, sy = img ? (float)ph / (float)h : 1.0f;
    if (su) *su = sx;
    if (sv) *sv = sy;
    if (ox) *ox = (float)du * sx;
    if (oy) *oy = (float)dv * sy;
    if (texel_space) *texel_space = !img;
}

/** @brief Find the first window slot with pixels or a pending capture whose rect holds a quad's texel box.
 *
 * A slot waiting for its capture serves a quad fed before the resolve: its
 * UVs are mapped to the rect's texels, and the present that composes it runs
 * after the resolve stores the image. Fills the mapping on a hit, as
 * fb_fill_map does.
 * @param tpage_x Texture page origin x, VRAM texels.
 * @param tpage_y Texture page origin y.
 * @param u0 Texel box left.
 * @param v0 Texel box top.
 * @param u1 Texel box right edge.
 * @param v1 Texel box bottom edge.
 * @param su Receives the x scale.
 * @param sv Receives the y scale.
 * @param ox Receives the mapped x of the page origin.
 * @param oy Receives the mapped y of the page origin.
 * @param texel_space Receives 1 when the mapping is to rect texels.
 * @return The slot index, or -1 when none holds the box. */
static int fb_find_window(int tpage_x, int tpage_y, int u0, int v0, int u1, int v1,
                          float *su, float *sv, float *ox, float *oy, int *texel_space)
{
    for (int i = 0; i < FB_SNAP_MAX; i++) {
        const fb_snap_t *s = &s_slot[i];
        const int du = tpage_x - s->dst_x, dv = tpage_y - s->dst_y;
        if ((!s->px && !s->pending) || !fb_rect_holds(du, dv, u0, v0, u1, v1, s->w, s->h))
            continue;
        fb_fill_map(du, dv, s->px ? s->pw : 0, s->ph, s->w, s->h, su, sv, ox, oy, texel_space);
        return i;
    }
    return -1;
}

/** @brief Find the first draw area with a content image or a pending capture whose clip rect holds a quad's texel box.
 *
 * A quad whose texels lie in a draw buffer samples the framebuffer itself
 * rather than a copy of it (the shrink of a frozen image before it is
 * copied out), and the area's clip rect stands in for a copy's destination
 * rect. Fills the mapping on a hit, as fb_fill_map does.
 * @param tpage_x Texture page origin x, VRAM texels.
 * @param tpage_y Texture page origin y.
 * @param u0 Texel box left.
 * @param v0 Texel box top.
 * @param u1 Texel box right edge.
 * @param v1 Texel box bottom edge.
 * @param su Receives the x scale.
 * @param sv Receives the y scale.
 * @param ox Receives the mapped x of the page origin.
 * @param oy Receives the mapped y of the page origin.
 * @param texel_space Receives 1 when the mapping is to rect texels.
 * @return FB_SNAP_MAX plus the area index, or -1 when none holds the box. */
static int fb_find_area(int tpage_x, int tpage_y, int u0, int v0, int u1, int v1,
                        float *su, float *sv, float *ox, float *oy, int *texel_space)
{
    for (int i = 0; i < FB_AREA_MAX; i++) {
        const fb_area_t *a = &s_area[i];
        const int du = tpage_x - a->x, dv = tpage_y - a->y;
        if ((!a->px && !a->pending) || !fb_rect_holds(du, dv, u0, v0, u1, v1, a->w, a->h))
            continue;
        fb_fill_map(du, dv, a->px ? a->pw : 0, a->ph, a->w, a->h, su, sv, ox, oy, texel_space);
        return FB_SNAP_MAX + i;
    }
    return -1;
}

/** @brief Leave the draw area a quad samples waiting for the image of the scene last drawn into it, and find it.
 *
 * Runs after a quad's texel box matched no stored image. A 16-bit quad
 * whose texels lie in a live draw area samples that buffer as the GPU
 * holds it when the quad is drawn: what the scene last drawn into the
 * buffer left there. The area is marked pending with that scene's stamp,
 * so the canonical capture that follows the feed composes the scene and
 * stores the crop (openpete_native_render_snapshot_fb_window) before the
 * present draws the quad, whether or not the guest reference is
 * rasterised. An area no put has drawn a completed scene into, or one
 * already holding an image or waiting for one, is left as it is.
 * @param tpage_x Texture page origin x, VRAM texels.
 * @param tpage_y Texture page origin y.
 * @param u0 Texel box left.
 * @param v0 Texel box top.
 * @param u1 Texel box right edge.
 * @param v1 Texel box bottom edge.
 * @param su Receives the x scale.
 * @param sv Receives the y scale.
 * @param ox Receives the mapped x of the page origin.
 * @param oy Receives the mapped y of the page origin.
 * @param texel_space Receives 1 when the mapping is to rect texels.
 * @return FB_SNAP_MAX plus the area index, or -1 when no area was marked. */
static int fb_pend_sampled_area(int tpage_x, int tpage_y, int u0, int v0, int u1, int v1,
                                float *su, float *sv, float *ox, float *oy, int *texel_space)
{
    fb_area_t *a = fb_area_holding(tpage_x + u0, tpage_y + v0, u1 - u0, v1 - v0);
    if (!a || !a->drawn_valid || a->px || a->pending) return -1;
    a->pending = 1;
    a->frame = a->drawn;
    a->sampled = 1;
    return fb_find_area(tpage_x, tpage_y, u0, v0, u1, v1, su, sv, ox, oy, texel_space);
}

int openpete_fb_snapshot_find(int tpage_x, int tpage_y, int bpp,
                             int u0, int v0, int u1, int v1,
                             float *su, float *sv, float *ox, float *oy, int *texel_space)
{
    if (!openpete_fb_snapshot_enabled() || bpp != 16) return -1;
    int idx = fb_find_window(tpage_x, tpage_y, u0, v0, u1, v1, su, sv, ox, oy, texel_space);
    if (idx < 0) idx = fb_find_area(tpage_x, tpage_y, u0, v0, u1, v1, su, sv, ox, oy, texel_space);
    if (idx < 0) idx = fb_pend_sampled_area(tpage_x, tpage_y, u0, v0, u1, v1, su, sv, ox, oy, texel_space);
    if (idx >= 0) s_quads++;
    return idx;
}

/** @brief Resolve a snapshot index to its stored image's pixel pointer, size and key, or NULL.
 *
 * The caller holds the image mutex.
 * @param idx Window slot index, or FB_SNAP_MAX plus a draw area index.
 * @param pw Receives the width.
 * @param ph Receives the height.
 * @param key Receives the key.
 * @return The pixels, or NULL when the index is out of range or holds no image. */
static const unsigned char *fb_image_locked(int idx, int *pw, int *ph, uint64_t *key)
{
    const unsigned char *px = NULL;
    if (idx >= FB_SNAP_MAX && idx - FB_SNAP_MAX < FB_AREA_MAX) {
        const fb_area_t *a = &s_area[idx - FB_SNAP_MAX];
        px = a->px; *pw = a->pw; *ph = a->ph; *key = a->key;
    } else if (idx >= 0 && idx < FB_SNAP_MAX) {
        const fb_snap_t *s = &s_slot[idx];
        px = s->px; *pw = s->pw; *ph = s->ph; *key = s->key;
    }
    return px;
}

int openpete_fb_snapshot_image(int idx, uint64_t *key, int *pw, int *ph)
{
    int w = 0, h = 0; uint64_t k = 0;
    SDL_LockMutex(px_mx());
    const int has = fb_image_locked(idx, &w, &h, &k) != NULL;
    SDL_UnlockMutex(s_px_mx);
    if (!has) return 0;
    if (key) *key = k;
    if (pw) *pw = w;
    if (ph) *ph = h;
    return 1;
}

unsigned char *openpete_fb_snapshot_copy(int idx, uint64_t key, int *pw, int *ph)
{
    int w = 0, h = 0; uint64_t k = 0;
    unsigned char *out = NULL;
    SDL_LockMutex(px_mx());
    const unsigned char *px = fb_image_locked(idx, &w, &h, &k);
    if (px && k == key) {
        const size_t n = (size_t)w * (size_t)h * 4u;
        out = (unsigned char *)malloc(n);
        if (out) memcpy(out, px, n);
    }
    SDL_UnlockMutex(s_px_mx);
    if (out) { if (pw) *pw = w; if (ph) *ph = h; }
    return out;
}

int openpete_fb_snapshot_rect(int idx, int *w, int *h)
{
    int rw = 0, rh = 0;
    if (idx >= FB_SNAP_MAX && idx - FB_SNAP_MAX < FB_AREA_MAX) {
        rw = s_area[idx - FB_SNAP_MAX].w; rh = s_area[idx - FB_SNAP_MAX].h;
    } else if (idx >= 0 && idx < FB_SNAP_MAX) {
        rw = s_slot[idx].w; rh = s_slot[idx].h;
    }
    if (rw <= 0 || rh <= 0) return 0;
    if (w) *w = rw;
    if (h) *h = rh;
    return 1;
}

int openpete_fb_snapshot_key_live(uint64_t key)
{
    int live = 0;
    SDL_LockMutex(px_mx());
    for (int i = 0; i < FB_SNAP_MAX && !live; i++) live = s_slot[i].px && s_slot[i].key == key;
    for (int i = 0; i < FB_AREA_MAX && !live; i++) live = s_area[i].px && s_area[i].key == key;
    SDL_UnlockMutex(s_px_mx);
    return live;
}

void openpete_fb_snapshot_counts(unsigned *taken, unsigned *resolved,
                                 unsigned *quads)
{
    if (taken)    *taken    = s_taken;
    if (resolved) *resolved = s_resolved;
    if (quads)    *quads    = s_quads;
}

int openpete_fb_snapshot_area_find(int clip_x, int clip_y, int clip_w, int clip_h,
                                   unsigned *epoch, int *retained)
{
    if (!openpete_fb_snapshot_enabled()) return -1;
    for (int i = 0; i < FB_AREA_MAX; i++) {
        const fb_area_t *a = &s_area[i];
        if (!a->live || a->x != clip_x || a->y != clip_y || a->w != clip_w || a->h != clip_h) continue;
        if (epoch) *epoch = a->epoch;
        if (retained) *retained = a->retained;
        return i;
    }
    return -1;
}
