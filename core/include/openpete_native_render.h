/* core/include/openpete_native_render.h - native Vulkan renderer A/B harness.
 *
 * The host-side entry points for openpete's native renderer. The native
 * renderer is a pure *display* consumer: it reads gamestate + asset data +
 * the VRAM mirror and draws into its own Vulkan window. It NEVER writes
 * PSX-readable RAM, so it is determinism-safe by construction - the
 * original `.s` renderers keep running (here, via PsyCross) as the headless
 * gameplay-verdict producers, and the oracle gate is unaffected.
 *
 * Two-window model: a single SDL window cannot host both PsyCross's OpenGL
 * context and our Vulkan surface, so the native renderer creates its own
 * window. During development it runs ALONGSIDE a visible PsyCross window for
 * A/B comparison; the end state hides PsyCross (FBO-only, read back as the
 * "reference") and the Vulkan window is the only one shown.
 *
 * Gated entirely by --renderer vk. With --renderer psycross (the
 * default) every entry point is an inert no-op, so normal/headless runs are
 * byte-for-byte unaffected.
 */
#ifndef OPENPETE_NATIVE_RENDER_H
#define OPENPETE_NATIVE_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "openpete.h" /* openpete_options_t */
#include "openpete_thread_roles.h" /* OP_SLOT */
#include "openpete_moby_snap_rule.h" /* openpete_snap_plane */
#include "openpete_osd.h" /* OPENPETE_OSD_MAX, OPENPETE_OSD_LINES_MAX */

#ifdef __cplusplus
extern "C" {
#endif

struct rhi_device;

/** @brief Hand the window icon to the renderer before bring-up.
 *
 * Stores the pointer and size; bring-up applies the image to the native
 * window when it creates it. In the default path PsyCross never creates a
 * window, so the native window is the only one the icon can land on. The
 * pixels must stay valid until bring-up.
 * @param rgba32 RGBA32 pixels, stride w * 4.
 * @param w Width in pixels.
 * @param h Height in pixels. */
void openpete_native_set_icon(const void* rgba32, int w, int h);

/* The state a window opens in (openpete_native_launcher_open). */
#define OPENPETE_WINDOW_OPEN_MAXIMIZED  1   /* maximized */
#define OPENPETE_WINDOW_OPEN_FULLSCREEN 2   /* borderless-desktop fullscreen; dropped under --background */

/** @brief Create the launcher's window, the Vulkan device on it and the imgui context, on the calling thread.
 *
 * The one windowed device of a run: the launcher draws on it before the
 * game is identified, and bring-up adopts it (openpete_native_render_init)
 * instead of creating another. Takes a precreated window when one exists,
 * else creates one with the --background activation hints. The window,
 * instance and device read the device-creation knobs from the game-neutral
 * tiers. rhi_create claims the display for the calling thread (main), and
 * launcher frames are drawn there. No-op returning 0 when the device
 * exists.
 * @param opts Session options; window flags and the synchronous-run class.
 * @param win_w Requested width in pixels, or <= 0 for the default.
 * @param win_h Requested height in pixels, or <= 0 for the default; the size
 *              a maximized or fullscreen window returns to.
 * @param open OPENPETE_WINDOW_OPEN_* bits for a window created here; a
 *             precreated window keeps its own state.
 * @param title Window title; NULL selects "openpete".
 * @return 0 when the device exists, -1 when the window or the device could
 *         not be created (openpete_native_launcher_error names why; the
 *         window is destroyed). */
int openpete_native_launcher_open(const openpete_options_t *opts, int win_w, int win_h,
                                  int open, const char *title);

/** @brief Create the launcher's window at a position on the next openpete_native_launcher_open, so it opens on the display the position lies on.
 *
 * Window coordinates, as SDL_GetWindowPosition reports them. With @p centre
 * the window is centred on the usable bounds of the display holding the
 * point (the primary display when none does). A window created fullscreen
 * returns to this position when it leaves fullscreen. One use: the open
 * that creates the window consumes it.
 * @param x The window's left edge, or a point on the display to centre on.
 * @param y The window's top edge, or the point's y.
 * @param centre Non-zero to centre on the point's display. */
void openpete_native_launcher_set_position(int x, int y, int centre);

/** @brief Report the game window's last state, recorded on the window's thread when the native teardown destroyed it.
 *
 * A fullscreen window reports the geometry it last had when it entered
 * fullscreen from a window neither maximized nor fullscreen; when it never
 * did (it opened fullscreen), the width and height are 0 and the position
 * is its display's origin.
 * @param fullscreen Receives 1 when it was fullscreen.
 * @param maximized Receives 1 when it was maximized.
 * @param x Receives its left edge, window coordinates.
 * @param y Receives its top edge.
 * @param w Receives its width, window coordinates.
 * @param h Receives its height.
 * @return 1 when a window was destroyed and its state recorded, else 0. */
int openpete_native_window_final_state(int *fullscreen, int *maximized, int *x, int *y, int *w, int *h);

/** @brief Return why the last openpete_native_launcher_open failed, as a sentence for the player.
 *
 * @return The reason, or "" when no open has failed. */
const char *openpete_native_launcher_error(void);

/** @brief Report whether the launcher's device exists and has not been adopted or closed.
 *
 * @return 1 while the launcher holds the device, else 0. */
int openpete_native_launcher_active(void);

/* A window operation on the launcher's window (openpete_native_launcher_window_op). */
typedef enum {
    OPENPETE_LAUNCHER_WINDOW_FULLSCREEN_TOGGLE,  /* windowed <-> borderless-desktop fullscreen */
    OPENPETE_LAUNCHER_WINDOW_MAXIMIZE,
    OPENPETE_LAUNCHER_WINDOW_RESTORE,
    OPENPETE_LAUNCHER_WINDOW_RESIZE,             /* to w x h */
    OPENPETE_LAUNCHER_WINDOW_MINIMIZE,
    OPENPETE_LAUNCHER_WINDOW_HIDE,
    OPENPETE_LAUNCHER_WINDOW_SHOW,
    OPENPETE_LAUNCHER_WINDOW_DISPLAY_STUB        /* harness: refresh query answers w Hz, then a display change is posted */
} openpete_launcher_window_op_t;

/** @brief Perform a window operation on the launcher's window, on the thread that created it.
 *
 * The launcher's window belongs to the thread that called
 * openpete_native_launcher_open (main) until bring-up adopts it; the
 * launcher's screens run on that thread, so the operation runs in the call.
 * After the adoption the launcher holds no window and nothing happens.
 * @param op The operation.
 * @param w The width for OPENPETE_LAUNCHER_WINDOW_RESIZE; the stubbed
 *          refresh in Hz for OPENPETE_LAUNCHER_WINDOW_DISPLAY_STUB.
 * @param h The height for OPENPETE_LAUNCHER_WINDOW_RESIZE.
 * @return For the fullscreen toggle, 1 when it requested fullscreen and 0
 *         when it requested windowed; 0 for the others; -1 when the
 *         launcher holds no window. */
int openpete_native_launcher_window_op(openpete_launcher_window_op_t op, int w, int h);

/** @brief Return the refresh rate of the display the launcher's swapchain was built for.
 *
 * @return The rate in Hz; 0 when unknown or when the launcher holds no device. */
float openpete_native_launcher_refresh_hz(void);

/** @brief Report whether the launcher's device is a software rasterizer (VK_PHYSICAL_DEVICE_TYPE_CPU).
 *
 * @return 1 for a CPU device, 0 otherwise or when the launcher holds no device. */
int openpete_native_launcher_software(void);

/** @brief Return the launcher's window, for a dialog parent.
 *
 * @return The window, or NULL when the launcher holds none. */
struct SDL_Window *openpete_native_launcher_window(void);

/** @brief Present one launcher frame: clear to a colour and record the imgui frame a callback draws.
 *
 * The launcher's per-present hook, on the display owner (the thread that
 * opened the device): the launcher's screens on its own device, and the
 * pipeline precompile's progress screen on the device bring-up adopted.
 * The begin waits for the frame slot (blocking present); a refused begin (a
 * pending swapchain rebuild, a minimized window) draws nothing. The frame
 * is a launcher frame for the boot facts (rhi_boot_launcher_frames), so it
 * neither sets the first-frame fact nor ends the bring-up create phase. On
 * the launcher's own device with a callback, the sky view handed by
 * openpete_native_launcher_sky_next draws between the clear and the imgui
 * frame; the present consumes the view either way.
 * @param dev The device; NULL for the launcher's own.
 * @param r Clear red, 0..1.
 * @param g Clear green, 0..1.
 * @param b Clear blue, 0..1.
 * @param draw Issues the frame's imgui calls; NULL presents the clear alone.
 * @param ud Passed to @p draw.
 * @return 1 when a frame was drawn, 0 otherwise (always 0 for a NULL @p draw). */
int openpete_native_launcher_present(struct rhi_device *dev, float r, float g, float b,
                                     void (*draw)(void *ud), void *ud);

/* The kinds of a launcher shape (openpete_launcher_shape). */
typedef enum {
    OPENPETE_LAUNCHER_SHAPE_FILL = 0,   /* a panel or chip: one colour */
    OPENPETE_LAUNCHER_SHAPE_BAR = 1     /* a progress bar: track, fill and border */
} openpete_launcher_shape_kind;

/* One shape of the launcher's shape pass: a rounded rectangle with
 * elliptical corners, every length in framebuffer pixels, colours straight
 * (not premultiplied) RGBA in 0..1. */
typedef struct openpete_launcher_shape {
    openpete_launcher_shape_kind kind;
    float rect[4];        /* x0, y0, x1, y1, y down */
    float rx, ry;         /* corner radii; each at most half the matching side */
    float col[4];         /* FILL: the colour */
    float track[4];       /* BAR: the track */
    float fill[4];        /* BAR: the fill, drawn from x0 to fill_x */
    float border[4];      /* BAR: the border ring */
    float fill_x;         /* BAR: the fill's right edge */
    float border_w;       /* BAR: the ring's width inside the edge */
    float aa;             /* the edge's coverage ramp width */
    float shadow_a;       /* the drop shadow's black alpha; 0 casts none */
    float shadow_dy;      /* the shadow's downward offset */
    float shadow_blur;    /* the shadow's blur width */
} openpete_launcher_shape;

/** @brief Add a shape to the shape pass of the launcher present being drawn.
 *
 * Called from the present's imgui callback (openpete_native_launcher_present).
 * The pass draws the frame's shapes in the order added, every shadow first,
 * after the sky and before the 3D title and imgui's draw data, so imgui text
 * lands over them; the present drops the list when it ends. Shapes past the
 * pass's capacity are counted and not drawn.
 * @param s The shape; copied.
 * @return 1 when it was added, else 0. */
int openpete_native_launcher_shape_add(const openpete_launcher_shape *s);

/* openpete_native_launcher_preview_render results. */
enum {
    OPENPETE_PREVIEW_OK = 0,
    OPENPETE_PREVIEW_FAILED = -1,     /* deterministic: a pipeline or object could not be made */
    OPENPETE_PREVIEW_STOPPED = -2,    /* the job was asked to stop */
    OPENPETE_PREVIEW_TRANSIENT = -3,  /* no device, the time bound, a readback or frame failure */
};

struct openpete_tercache_preview;

/** @brief Draw a built preview on the launcher's device and read it back, from a job thread: the display owner makes the pipelines' compile jobs, this thread runs them, and the display owner installs them, records the draw inside its next present and reads the image back.
 *
 * Any thread but the display owner, which must keep presenting
 * (openpete_native_launcher_present takes the requests; a wake event ends
 * its wait for input). Without the launcher's device (a headless run) it
 * returns at once. At a stop or the deadline the call returns without
 * waiting for a present: a request not yet taken, or a render prepared and
 * not yet drawn, is withdrawn; only a request the display owner is working
 * on inside its current call is waited for.
 * @param p The preview; read until the call returns.
 * @param slot_contest The game's terrain.slot_ot_contest, compiled into the jobs.
 * @param rgba Receives p->w * p->h * 4 bytes.
 * @param cap Its capacity.
 * @param stop Polled while waiting; nonzero stops the call. May be NULL.
 * @param deadline_ns The op_now() time past which the call gives up.
 * @param why Receives the failure's reason.
 * @param why_cap Its capacity.
 * @return An OPENPETE_PREVIEW_* value. */
int openpete_native_launcher_preview_render(struct openpete_tercache_preview *p, int slot_contest,
                                            uint8_t *rgba, size_t cap, int (*stop)(void),
                                            uint64_t deadline_ns, char *why, size_t why_cap);

/** @brief Take a posted preview request at a launcher present, before its frame begins: make the compile jobs, or prepare the render's device objects.
 *
 * Display owner only (openpete_native_launcher_present).
 * @param dev The launcher's device. */
void openpete_native_launcher_preview_before_frame(struct rhi_device *dev);

/** @brief Record a prepared preview render into the present's frame, before the offscreen pass.
 *
 * A render prepared at this present records at the next one, so its
 * preparation and its draw with the readback stall two presents, not one.
 * Display owner only (openpete_native_launcher_present). */
void openpete_native_launcher_preview_in_frame(void);

/** @brief Read a recorded preview render back after the present's frame and finish its request.
 *
 * Display owner only (openpete_native_launcher_present).
 * @param frame_ran 1 when the frame began and was submitted; 0 keeps the request for the next present. */
void openpete_native_launcher_preview_after_frame(int frame_ran);

/** @brief Destroy any preview render's device objects before the launcher's device is closed or handed off: a withdrawn one, or a parked one, whose request then ends transient.
 *
 * Display owner only, outside a frame. */
void openpete_native_launcher_preview_release(void);

/** @brief Read back the last launcher frame, count the pixels that differ from the clear colour, and write it as a PNG.
 *
 * A pixel differs when a channel is more than 8 levels from @p bg. Waits
 * the device idle.
 * @param dev The device the frame was presented on; NULL for the launcher's own.
 * @param path PNG to write, or NULL or "" to only count.
 * @param bg The clear colour as R, G, B bytes.
 * @param differ Receives the differing pixel count.
 * @param total Receives the frame's pixel count.
 * @return 0 on success, -1 when there is no device, the readback
 *         fails, or the PNG cannot be written. */
int openpete_native_launcher_shot(struct rhi_device *dev, const char *path, const uint8_t bg[3],
                                  unsigned *differ, unsigned *total);

/** @brief Start compiling bring-up's pipelines into the pipeline cache on the launcher's device, before a game is picked.
 *
 * Runs once per run, on the display owner, after the launcher's device
 * exists. Bring-up's eager pipeline builders and the terrain cache's run in
 * description mode (rhi_pipeline_describe_begin) and the descriptions start
 * the ahead walk (rhi_precompile_ahead_start): both SLOT_CONTEST values,
 * the texture mapping @p opts resolves (flag, global scope, [saved]) before
 * the other, and at each mapping the terrain cache's pipelines before the
 * eager ones, on the compile pool. Each walk job's return pushes the wake
 * event (openpete_native_launcher_precompile_event), and the launcher's
 * thread steps the walk (openpete_native_launcher_precompile_step) when it
 * takes its events, so the walk advances whether or not the launcher
 * presents. Mod pipelines are not known before the pick and are left to
 * bring-up. Bring-up stops the walk before
 * its first create; the keys it did not reach compile at bring-up or in the
 * game's background walk. Skipped where bring-up skips its precompile (a
 * synchronous run, a software device) unless OPENPETE_PRECOMPILE_AHEAD or
 * OPENPETE_PRECOMPILE_FORCE asks for it.
 * @param opts The game-neutral options.
 * @return The create infos queued; 0 when skipped or already started. */
int openpete_native_launcher_precompile_start(const openpete_options_t *opts);

/** @brief Keep the pipeline cache of the next device openpete_native_launcher_open creates in memory only: nothing is read from or written to the state "cache" directory. */
void openpete_native_launcher_pipeline_cache_off(void);

/** @brief Step the launcher's precompile: submit walk jobs in place of those that returned, and end the walk once every job has returned.
 *
 * Display owner only (the launcher's thread). Clears the pending wake, so
 * the next return pushes a new wake event.
 * @return 1 while the walk runs, 0 when it ended or never started. */
int openpete_native_launcher_precompile_step(void);

/** @brief Read the launcher's precompile progress: whether the ahead walk runs, and the groups of its first texture mapping that returned.
 *
 * Display owner only (the launcher's thread).
 * @param groups Receives bit G for each group G whose return the walk
 *        logged (rhi_precompile_ahead_returned), or NULL.
 * @return 1 while the walk runs, 0 when it ended or never started. */
int openpete_native_launcher_precompile_progress(unsigned *groups);

/** @brief Return the SDL event type a returning walk job pushes to wake the launcher's thread.
 *
 * At most one is queued between steps. The event carries nothing.
 * @return The type, or 0 before the precompile started or when no type could be registered. */
uint32_t openpete_native_launcher_precompile_event(void);

/** @brief Read the launcher's precompile counts: the create infos returned or skipped and the create infos queued.
 *
 * Display owner only (the launcher's thread).
 * @param done Receives the create infos returned or skipped, or NULL.
 * @param total Receives the create infos queued, or NULL.
 * @return 1 while the walk runs, 0 when it ended or never started (both counts then 0). */
int openpete_native_launcher_precompile_counts(unsigned *done, unsigned *total);

/** @brief Pause or resume the launcher's pipeline compile pool around work that uses every core (the first-boot module build).
 *
 * Paused, each worker finishes the pipeline it holds and starts no other;
 * the queue is kept. A pause requested before the pool or the device exists
 * applies when the precompile starts. Bring-up resumes a paused pool.
 * @param on Nonzero to pause, 0 to resume. */
void openpete_native_launcher_compile_pause(int on);

/** @brief Run the launcher's pipeline compile pool at low priority, or at its own again, around work that runs beside the launcher (a launch-time module rebuild).
 *
 * In the low mode the pool's queue runs on low-priority workers
 * (rhi_compile_pool_low); a pause (openpete_native_launcher_compile_pause,
 * openpete_native_launcher_compile_hold) still stops every worker. Leaving
 * the mode logs the pipelines created in it. A mode set before the pool or
 * the device exists applies when the precompile starts; bring-up leaves it.
 * Display owner only (the launcher's thread).
 * @param on Nonzero for low priority, 0 for the pool's own. */
void openpete_native_launcher_compile_low(int on);

/** @brief Pause the launcher's pipeline compile pool for the rest of the process: a quit's stop takes no other pipeline.
 *
 * Each worker finishes the pipeline it holds, which cannot be interrupted;
 * every later openpete_native_launcher_compile_pause(0) is ignored, so the
 * end of a stopped job does not restart the queue, and the pool's stop at
 * the device's destruction drops it. Display owner only (the launcher's
 * thread). */
void openpete_native_launcher_compile_hold(void);

/** @brief Destroy the launcher's imgui context, device and window when the run ends before bring-up adopts them.
 *
 * No-op when the launcher holds no device. */
void openpete_native_launcher_close(void);

/** @brief Create the game window on the caller's thread for the device creation to take.
 *
 * Win32 delivers a window's messages only to the thread that created it,
 * so the window must belong to the thread that runs the event pump. The size
 * comes from the same resolution bring-up uses, and the creation flags carry
 * the --background activation hints. No-op under --headless, when a window
 * was already precreated, when the launcher's window exists, or when the
 * launcher found no Vulkan device (opts->native_unavailable). A failed
 * creation is logged and openpete_native_launcher_open creates its own
 * window.
 * @param opts Session options; must not be NULL.
 * @param win_w Requested width in pixels, or <= 0 for the default.
 * @param win_h Requested height in pixels, or <= 0 for the default. */
void openpete_native_precreate_window(const openpete_options_t *opts,
                                      int win_w, int win_h);

/** @brief Bring up the native Vulkan renderer: its window or a surfaceless device, pipelines, buffers and the terrain cache.
 *
 * Not brought up, returning 0, when @p opts is NULL, under --headless with
 * the PsyCross renderer, and under --headless with the vk renderer unless
 * screenshots, the identity audit or OPENPETE_HEADLESS_NATIVE=1 need a
 * surfaceless device. OPENPETE_HEADLESS_NATIVE=1 brings the device up with
 * no capture consumer: the game-side scene feeders, the canonical capture
 * and the per-VBL headless device services then run every tick as they do
 * under a window, and nothing is composed unless a capture asks. Otherwise it
 * applies the texture-mapping, notex, wireframe and aspect settings, creates
 * the device (adopting a precreated window when one was handed over), the
 * scene pipelines and buffers, subscribes to PsyCross VRAM writes, starts
 * the terrain sector cache when --terrain-cache is on and the device is a
 * hardware GPU (or OPENPETE_TERRAIN_CACHE_FORCE is set), and primes the HD
 * fold atlas when the fold is on.
 * @param opts  Resolved launch options.
 * @param win_w Requested window width, or <= 0 for the default.
 * @param win_h Requested window height, or <= 0 for the default.
 * @return 0 when the renderer is up or deliberately not brought up, 1 when
 *         bring-up failed. */
int openpete_native_render_init(const openpete_options_t *opts, int win_w, int win_h);

/** @brief Return whether a --headless run has a consumer for the surfaceless native device.
 *
 * The consumers are a screenshot or intermediate-capture request, the
 * identity audit and OPENPETE_HEADLESS_NATIVE=1. Without one,
 * openpete_native_render_init leaves the native renderer off under
 * --headless, so a caller deciding whether PsyCross may skip its GPU asks
 * this first.
 * @param opts Resolved launch options.
 * @return true when a headless native consumer is armed. */
bool openpete_native_headless_consumer(const openpete_options_t *opts);

/** @brief Return whether native bring-up succeeded and this process composes through a Vulkan device.
 *
 * Says nothing about where the picture comes from: it is true in PsyCross
 * display mode, where the window shows PsyCross's read-back image. Nor does
 * it promise a window: headless bring-up is surfaceless and still sets it,
 * so a consumer that needs a display checks headless separately.
 * @return True once bring-up succeeded. */
bool openpete_native_render_enabled(void);

/** @brief Return whether a native scene is being extracted, interpolated and drawn.
 *
 * While true the feature modules feed the scene ring, the sub-tick spine
 * re-extracts, and compose walks the tri list. False in PsyCross display
 * mode. Implies openpete_native_render_enabled; the converse does not hold.
 * Every consumer that walks, extracts from, or composes the native scene
 * asks this predicate, not the display predicate above.
 * @return True while the native scene is live. */
bool openpete_native_scene_enabled(void);

/** @brief Return the number of presents dropped because the swapchain slot's previous submission had not retired.
 *
 * Forwards rhi_fence_sheds. Windowed only, and never in capture mode: the
 * present is dropped rather than blocking the presenting thread on the
 * display's cadence. Monotonic;
 * the pacing log diffs it.
 * @return The cumulative count, or 0 before the device exists. */
uint64_t openpete_native_render_fence_sheds(void);

/** @brief Put the device in capture mode when the options request a constant-rate recording.
 *
 * Applies rhi_set_capture_mode when --record and --record-cfr are both
 * set: every present then begins and queues its readback, with the
 * swapchain present best effort, so the capture holds one image per
 * present the pacer owes however slow the GPU is. Called once after
 * bring-up; other runs keep the shedding present path. No effect before
 * the device exists.
 * @param opts Parsed options, or NULL (no effect). */
void openpete_native_render_arm_capture(const openpete_options_t *opts);

/** @brief Return the number of presents render_frame_common skipped because the frame could not begin.
 *
 * The same count the present census reports as "never began". Monotonic.
 * @return The cumulative count. */
unsigned openpete_native_render_begin_refused(void);

/** @brief Print the scene signature census lines for the composed scene to stderr.
 *
 * With OPENPETE_SCENE_SIG=1, one "[scene-sig]" line hashes the scene's
 * discrete fields and counts triangles per category, with the present's
 * alpha; it shows whether re-extracting per present ever decides
 * differently within one tick's alpha span. With OPENPETE_SCENE_POS_SIG=1,
 * one "[scene-pos]" line hashes every vertex's screen position per draw
 * category. With neither set it prints nothing.
 * @param canonical Non-zero on the canonical present, zero on an intermediate. */
void openpete_native_scene_sig_report(int canonical) OP_REQUIRES_SHARED(sim);

/** @brief Return the number of presents queued but not yet confirmed displayed.
 *
 * Forwards rhi_present_outstanding. Call only from the presenting thread,
 * since the swapchain is externally synchronized.
 * @return The count, or 0 without present timing, without a swapchain, or
 *         before the device exists. */
int openpete_native_render_present_outstanding(void);
/** @brief Wait for the last queued present to be displayed on screen.
 *
 * Forwards rhi_present_wait, which confirms outstanding present ids in order
 * and is meant for millisecond-scale timeouts, never as a poll. Call only
 * from the presenting thread: the swapchain is externally synchronized.
 * @param timeout_ns Longest wait in nanoseconds.
 * @param out_id Receives the id of the present confirmed displayed; may be
 *        NULL.
 * @param out_waited_ns Receives the time spent waiting in nanoseconds; may
 *        be NULL.
 * @return 1 when displayed, 0 on timeout, -1 when there is no device, no
 *         present timing, a pending swapchain rebuild, or nothing
 *         outstanding. */
int openpete_native_render_present_wait(uint64_t timeout_ns, uint64_t *out_id,
                                        uint64_t *out_waited_ns);

/** @brief Return the number of presents the swapchain acquire path refused.
 *
 * Forwards rhi_acquire_sheds. Monotonic; openpete_native_render_acquire_timeouts
 * counts the subset that expired the acquire budget.
 * @return The cumulative count, or 0 before the device exists. */
uint64_t openpete_native_render_acquire_sheds(void);
/** @brief Return the fence and acquire sheds counted since the previous call, and mark them taken.
 *
 * Forwards rhi_take_sheds; both are 0 before the device exists.
 * @param fence Receives the fence sheds since the previous call.
 * @param acquire Receives the acquire sheds since the previous call. */
void     openpete_native_render_take_sheds(uint64_t *fence, uint64_t *acquire);
/** @brief Return the number of swapchain acquires that expired their time budget.
 *
 * Forwards rhi_acquire_timeouts. Monotonic.
 * @return The cumulative count, or 0 before the device exists. */
uint64_t openpete_native_render_acquire_timeouts(void);
/** @brief Return the swapchain image count the driver granted.
 *
 * Forwards rhi_swapchain_image_count.
 * @return The image count, or 0 before the device exists. */
uint32_t openpete_native_render_swapchain_images(void);

/** @brief Return the GPU's device name for the pacing log's environment block.
 *
 * The string is owned by the device and stays valid while it exists.
 * @return The name; "unknown" before the device exists and in
 *         PsyCross-only runs. */
const char* openpete_native_render_gpu_name(void);
/** @brief Return the driver version string for the pacing log's environment block.
 *
 * @return The version; "unknown" before the device exists, in PsyCross-only
 *         runs, and when the driver reports none. */
const char* openpete_native_render_driver_version(void);
/** @brief Return the name of the granted present mode for the pacing log's environment block.
 *
 * The mode is the one the driver granted, the only one a baseline may be
 * keyed on.
 * @return A static string such as "immediate" or "fifo"; "none" before the
 *         device exists and in PsyCross-only runs. */
const char* openpete_native_render_present_mode(void);
/** @brief Write the Vulkan device report (rhi_device_report) for a bug report's device.txt.
 *
 * Called by the display's owner.
 * @param buf Receives the text.
 * @param cap Capacity of @p buf.
 * @return snprintf's count; "vulkan: no device" before the device exists. */
int openpete_native_render_device_report(char* buf, size_t cap);

/** @brief Toggle the native window between windowed and borderless-desktop fullscreen.
 *
 * Entering fullscreen first sets the window's fullscreen mode to NULL, which
 * SDL3 takes as borderless desktop with no video-mode switch. The resize
 * debounce adopts the resulting size change on first sight, since it lands
 * in one step; the present path re-reads the window size and the RHI
 * recreates the swapchain on resize. Posts a DISPLAY toast naming the new
 * state. Alt+Enter, the `fullscreen` binding and a window double-click reach
 * it from the host loop in run.c; the O-menu and M-overlay rows reach it
 * through openpete_native_request_fullscreen_toggle. Call only on the thread
 * that owns the window. No-op until the native window is up, so it is inert
 * in headless and PsyCross-only runs. */
void openpete_native_toggle_fullscreen(void);
/** @brief Request a windowed/fullscreen toggle from any thread.
 *
 * Raises a flag that openpete_native_toggle_fullscreen services on the
 * thread owning the window: main under the thread split, else the host
 * loop. SDL_SetWindowFullscreen belongs to that thread; on Win32 a
 * cross-thread call sends a message into a pump that may be busy, or
 * misdelivers the resize. Callers include the settings menu and the dev
 * overlay's knob row. */
void openpete_native_request_fullscreen_toggle(void);

/** @brief Publish the native window's fullscreen flag for other threads to read.
 *
 * Reads the SDL window flags, so call only on the thread that owns the
 * window: at bring-up and on each SDL enter/leave-fullscreen event. */
void openpete_native_publish_fullscreen(void);

/** @brief Return whether the native window is in borderless-desktop fullscreen.
 *
 * Any thread may call it: the state is the one the window-owning thread
 * published (openpete_native_publish_fullscreen, and the toggle's requested
 * state), never a read of the SDL window flags, which that thread's event
 * pump writes. The O-menu DISPLAY row and the fullscreen knob row read it,
 * so they stay in sync with the Alt+Enter and double-click toggles.
 * @return 1 when fullscreen, 0 when windowed or when there is no native
 *         window (headless, native path disabled). */
int openpete_native_fullscreen_active(void);

/** @brief Return the native SDL window.
 *
 * The mouse channel applies relative mode to it.
 * @return The window, or NULL before it exists, when native render is off,
 *         or under headless. */
struct SDL_Window* openpete_native_render_window(void);

/** @brief Set the performance-stats overlay level.
 *
 * The level is one of the OPENPETE_PERF_* values in openpete_perf_stats.h,
 * which owns the numbers; the renderer holds the level because it gates the
 * draw and sizes the supersample region. Out-of-range values are clamped. A
 * non-off level is remembered for openpete_native_set_fps_overlay. Drawn only
 * on the live vk present, never in screenshots. Driven by --perf-stats and
 * the overlay and menu rows. Renderer state only.
 * @param level An OPENPETE_PERF_* value. */
void openpete_native_set_perf_stats(int level);
/** @brief Return the performance-stats overlay level in force.
 *
 * @return An OPENPETE_PERF_* value. */
int  openpete_native_perf_stats_level(void);

/** @brief Turn the performance-stats overlay off, or on at the last non-off level.
 *
 * The two-state view of openpete_native_set_perf_stats used by the O menu
 * row: on restores the last non-off level, so a toggle never demotes a
 * detailed readout to the bare counter.
 * @param on Non-zero for on. */
void openpete_native_set_fps_overlay(int on);

/** @brief Set whether the FPS overlay is drawn into --record video.
 *
 * -1 defers to the OPENPETE_FPS_IN_RECORD env var, else off; 0 and 1 are
 * explicit. The resolved value is dropped and re-resolved on the next
 * recorded frame, so the setting is live. Affects only presents while a
 * recording is active; the live window follows the perf-stats level.
 * @param pref -1 unset, 0 off, 1 on. */
void openpete_native_set_fps_in_record(int pref);

/** @brief Return whether the performance-stats overlay is on at any level.
 *
 * The O menu reads it to re-sync its SHOW FPS row when it opens.
 * @return 1 when the level is not OPENPETE_PERF_OFF, else 0. */
int  openpete_native_fps_overlay_enabled(void);
/** @brief Return the rolling present rate the performance-stats overlay displays.
 *
 * The value counts presents on whichever thread makes them (inline, or the
 * present thread under eager production) and is recomputed at the first
 * present at least 0.5 s of wall time after the previous window started,
 * rounded to the nearest integer. The overlay's FPS line prints it. A
 * headless run makes no presents and composes one scene per tick, so it
 * returns the NTSC tick rate 1125000/37609 Hz rounded (30), a value
 * independent of host speed.
 * @return Presents per second; 0 before the first window completes; the
 *         rounded tick rate under --headless. */
int  openpete_native_fps_value(void);

/* Live presentation setters (the in-game settings menu drives these). All
 * display-only: they mutate renderer/GPU state only, never gameplay-readable RAM,
 * and take effect on the next native present. Harmless (no visible effect) when
 * the native renderer isn't the active present path. */

/** @brief Set the target display aspect (width over height); 0 or less selects AUTO.
 *
 * AUTO (the default) follows the window shape with no bars. A fixed aspect
 * resizes a windowed window's width to match, as --aspect does at launch;
 * fullscreen, or a window the user resized away, letterboxes or pillarboxes
 * the fixed aspect (Hor+). Forces the fitted panel rect to recompute on the
 * next present.
 * @param w_over_h Width over height, e.g. 16.0f / 9.0f; <= 0 for AUTO. */
void openpete_native_set_aspect(float w_over_h);
/** @brief Return the display aspect as last requested.
 *
 * The config-knob row reads it so an O-menu pick registers as an edit of
 * the knob, as with the SSAA and VSync getters.
 * @return Width over height, or 0 for AUTO. */
float openpete_native_get_aspect(void);

/** @brief Set the SSAA supersampling factor.
 *
 * Clamped to [1, OPENPETE_SSAA_MAX]; 1 is off. ensure_ss_rtt caps the
 * effective factor to the GPU's maximum image dimension and reallocates the
 * supersampled scene render target on the next present, a one-time wait-idle
 * like a window resize.
 * @param factor The requested factor. */
void openpete_native_set_ssaa(int factor);
/** @brief Return the requested SSAA factor.
 *
 * This is the requested factor, not the effective one ensure_ss_rtt caps
 * to the GPU. Both the O menu and the M overlay move this state, so the
 * knob row reads it back here rather than keeping a shadow copy, which
 * would hide a pick made in one surface from the other and from saving
 * settings.
 * @return The factor, 1 when unset. */
int openpete_native_get_ssaa(void);

/** @brief Set the internal render resolution (--resolution) as a packed value.
 *
 * Stores the wanted value only; the present latches it between frames and
 * the present-setup ensures reallocate the base targets on the display's
 * owner. A value set through this call counts as explicit for headless
 * captures.
 * @param packed A value packed by openpete_render_res_pack. */
void openpete_native_set_resolution(int packed);
/** @brief Return the wanted internal render resolution, packed.
 * @return The value packed by openpete_render_res_pack; 0 is window. */
int openpete_native_get_resolution(void);

/** @brief Set the vsync preference: FIFO when on, immediate or mailbox when off.
 *
 * Forwards to the RHI, which stores the preference and, on a windowed
 * device, flags the swapchain for recreation; the next begin-frame rebuilds
 * it with the present mode the preference selects. A value equal to the
 * current preference changes nothing. Logs the new setting. The preference
 * is dropped when the native device is not up.
 * @param on Non-zero for vsync on. */
void openpete_native_set_vsync(int on);
/** @brief Return the current vsync preference.
 *
 * The preference is read from the RHI device unresolved; the present mode
 * the surface resolved is not reported.
 * @return 1 for FIFO, 0 for vsync off, -1 when unset (no device yet, or
 *         launched without --vsync so the environment or surface decides). */
int openpete_native_get_vsync(void);

/** @brief Set the vertex screen-coordinate precision: sub-pixel, or the PSX integer grid.
 *
 * With integer snap on, every triangle added afterwards has its sx and sy
 * floored to the 512x240 PSX pixel grid, the GTE's SXY truncation that
 * produces the polygon wobble; depth and UVs are untouched. HUD, OVERLAY and
 * HUD_PANEL triangles and triangles fed while the presnap state is set keep
 * their coordinates. Stores a wanted value from any thread; the feeds take
 * it at the next openpete_native_producer_knobs_latch and the retained
 * terrain at the next present. Logs the new setting. Renderer state only.
 * @param integer_snap Non-zero for the integer grid, zero for sub-pixel. */
void openpete_native_set_vertex_precision(int integer_snap);

/** @brief Return the vertex precision the next latch applies, for the settings rows and the settings save.
 * @return 1 for the integer grid, 0 for sub-pixel. */
int openpete_native_get_vertex_precision(void);

/** @brief Return whether integer vertex snap is active for the feeds, as latched on the sim thread.
 *
 * Feeders that synthesize vertices on another primitive's edge (the terrain
 * subdivision midpoints, the shadow feed) read it to emit snap-consistent
 * screen coordinates, so their T-vertices stay watertight against snapped
 * neighbour edges.
 * @return 1 when integer snap is active, else 0. */
int openpete_native_vertex_snap_active(void);

/** @brief Return whether integer vertex snap is active for the present, as latched once per present.
 *
 * The retained terrain cache reads it for the push constant every arena
 * draw of one present shares.
 * @return 1 when integer snap is active, else 0. */
int openpete_native_vertex_snap_present(void);

/** @brief Mark the triangles added next as already snap-consistent.
 *
 * Sticky feed state: while set, the renderer's integer-snap chokepoint leaves
 * the triangles' coordinates untouched. Set by a feeder that controls vertex
 * placement beyond per-vertex snapping (terrain subdivision midpoints sit at
 * exact off-integer positions on snapped parent edges). Cleared by the feeder
 * after the feed, like the tag.
 * @param on Non-zero to set. */
void openpete_native_scene_set_presnap(int on);

/** @brief Return 1 when the terrain key image can be rendered this run.
 *
 * True when the native renderer is enabled and the terrain key pipeline and
 * its vertex buffer were created. The terrain and moby feeders write a scene
 * triangle's OT key offset (koff) only when this holds, so no LP terrain
 * fragment tests a key image that does not exist.
 * @return 1 when the key pass is available, else 0. */
int openpete_native_key_resolve_on(void);

/** @brief Set the raster-output quantize multiplier; 0 is off.
 *
 * Box-resamples the composed 3D scene (before the HUD) onto the 512N x 240N
 * virtual PSX raster; N = 1 is the 240p raster. The multiplier is an integer
 * multiple of the vertex-snap grid, so snapped edges land on cell boundaries
 * at every setting. Negative values select off. Consumed at the next scene
 * postfx split. Renderer state only.
 * @param mult The multiplier N, or 0 for off. */
void openpete_native_set_raster_quantize(int mult);
/** @brief Return the raster-quantize multiplier as resolved at init or last set.
 *
 * @return The multiplier, 0 when off. */
int  openpete_native_get_raster_quantize(void);

/** @brief Turn the PS1 15-bit colour pass on or off.
 *
 * On applies RGB555 truncation and the GPU's 4x4 dither matrix on the PSX
 * framebuffer lattice, as an engine postfx below the mod chain. The enable
 * is checked per present, so no pipeline rebuild is involved. Renderer state
 * only.
 * @param on Non-zero for on. */
void openpete_native_set_dither(int on);
/** @brief Return whether the PS1 15-bit colour and 4x4 dither view is on.
 *
 * @return 1 when on, else 0. */
int  openpete_native_get_dither(void);

/** @brief Turn hiding the player on or off.
 *
 * On, the native feeders drop the avatar, its fire breath, its ground
 * shadow, the companion and the particles their updates spawn. The setter
 * stores a wanted value from any thread; the feeds take it at the next
 * openpete_native_producer_knobs_latch; particles spawned while
 * both this and the ownership census were off stay drawn until they expire.
 * Renderer state only.
 * @param on Non-zero for on. */
void openpete_native_set_hide_player(int on);
/** @brief Return whether hiding the player is chosen: the value the next latch applies, for the settings rows.
 *
 * @return 1 when hidden, else 0. */
int  openpete_native_get_hide_player(void);
/** @brief Return whether the feeders drop the player's draws, as latched on the sim thread.
 *
 * @return 1 when hidden, else 0. */
int  openpete_native_hide_player_active(void);

/** @brief Turn hiding the HUD on or off.
 *
 * On, the compose skips the flat 2D HUD layer and the HUD3D layer, both
 * passes of each; the overlay quads and the fade still draw. Read per
 * present. Renderer state only.
 * @param on Non-zero for on. */
void openpete_native_set_hide_hud(int on);
/** @brief Return whether the HUD layers are hidden.
 *
 * @return 1 when hidden, else 0. */
int  openpete_native_get_hide_hud(void);

/** @brief Whether --hide-classes lists moby class `cls`.
 *
 * The list is parsed once at renderer init; the moby feed drops every face
 * of a listed class and the shadow feed its drop shadow.
 * @param cls A moby class halfword.
 * @return 1 when listed, else 0. */
int  openpete_native_hide_class(uint32_t cls);
/** @brief Whether --hide-classes lists any class.
 * @return 1 when the list is not empty. */
int  openpete_native_hide_class_any(void);

/** @brief Queue the texture and colour interpolation mode: perspective-correct or PSX affine.
 *
 * The mode is a vertex specialization constant baked into every scene
 * pipeline, so the setter only queues it; at the next present boundary the
 * renderer re-bakes every live pipeline in place (mod material and mesh
 * caches included) under a one-time device idle. A failed rebuild keeps the
 * previous mode. Renderer state only.
 * @param affine Non-zero for affine (screen-linear), zero for perspective. */
void openpete_native_set_texture_mapping(int affine);
/** @brief Return the chosen texture-mapping mode: the boot mode or the last value queued by openpete_native_set_texture_mapping.
 *
 * For the settings rows and the settings save; the pipelines change at a
 * later present (openpete_native_texture_mapping_drawn).
 * @return 1 for PSX affine, 0 for perspective-correct. */
int  openpete_native_get_texture_mapping(void);
/** @brief Return the texture-mapping mode the pipelines draw with, as latched on the sim thread.
 *
 * The present publishes the mode when a rebuild installs it; the terrain
 * producer reads this value so every feed of one tick bakes for one mode.
 * @return 1 for PSX affine, 0 for perspective-correct. */
int  openpete_native_texture_mapping_drawn(void);

/** @brief Queue the untextured lighting view on or off.
 *
 * Textured scene fragments output the raw gouraud vertex colour (the texel
 * replaced with modulation-neutral grey; transparency cutouts keep their
 * silhouettes). Same queued pipeline rebuild as
 * openpete_native_set_texture_mapping. Debug view, renderer state only.
 * @param on Non-zero for on. */
void openpete_native_set_notex(int on);
/** @brief Return whether the untextured lighting view is in effect.
 *
 * Reports the applied state; a value queued by openpete_native_set_notex
 * shows after the next present boundary.
 * @return 1 when on, else 0. */
int  openpete_native_get_notex(void);

/** @brief Queue the wireframe view on or off.
 *
 * Under the view every depth-tested scene pipeline rasterizes triangle
 * edges coloured by the gouraud vertex lighting (the NOTEX colouring);
 * postfx and the depth-only passes stay filled, and a device without
 * fillModeNonSolid draws filled. The setter only queues the value; at the
 * next present boundary the renderer re-bakes every pipeline in place, and
 * a failed rebuild keeps the previous mode. Bring-up seeds the value from
 * --wireframe or OPENPETE_WIREFRAME. Renderer state only.
 * @param on Non-zero for on. */
void openpete_native_set_wireframe(int on);
/** @brief Return whether the wireframe view is in effect.
 *
 * Reports the applied state; a value queued by openpete_native_set_wireframe
 * shows after the next present boundary.
 * @return 1 when on, else 0. */
int  openpete_native_get_wireframe(void);

/** @brief Select the sector candidate set: the game's verdict, a frustum test, or the game's verdict with its holes filled.
 *
 * OPENPETE_SECTOR_VIS_GAME admits the sectors the game marked visible;
 * OPENPETE_SECTOR_VIS_FILLED adds the sectors an opposite pair of neighbour
 * cells of the game's PVS lists (terrain.c, sector_filled_*), on a game
 * whose verdict is a position-keyed PVS; OPENPETE_SECTOR_VIS_AUTO runs FILLED
 * on such a game and FRUSTUM on any other; any other value selects
 * OPENPETE_SECTOR_VIS_FRUSTUM, which admits every sector whose bounding
 * sphere is inside the displayed frame and does not read the game's flags. Seeded from --sector-visibility at init; the
 * setter stores a wanted value from any thread, which the next
 * openpete_native_producer_knobs_latch applies to the terrain feeder's
 * sector walks. Renderer state only; the game's flags are never written.
 * @param mode An openpete_sector_visibility_t value. */
void openpete_native_set_sector_visibility(int mode);
/** @brief Return the chosen terrain candidate-set mode: the value the next latch applies, for the settings rows.
 *
 * @return An openpete_sector_visibility_t value. */
int  openpete_native_get_sector_visibility(void);
/** @brief Return the terrain candidate-set mode the sector walks use, as latched on the sim thread.
 *
 * OPENPETE_SECTOR_VIS_AUTO is resolved for the running game: FILLED where
 * game_layout.env.vis_aspect_independent (a position-keyed PVS), else
 * FRUSTUM.
 * @return OPENPETE_SECTOR_VIS_GAME, _FRUSTUM or _FILLED. */
int  openpete_native_sector_visibility_active(void);
/** @brief Set how the terrain walk packs a skipped drain sub-cell: 1 keys it at the face's least key and draws it, 0 stores the sentinel the key and colour passes discard.
 *
 * The live channel of the TERRAIN_DRAIN_SKIP row; callable from any
 * thread. The next walk reads it at its start. Renderer state only.
 * @param legacy 1 for the keyed packing, 0 for the sentinel. */
void openpete_native_set_terrain_drain_legacy(int legacy);
/** @brief Return the terrain walk's skipped-sub-cell packing: 1 keyed and drawn, 0 the sentinel.
 *
 * Until a setter call, the value OPENPETE_TERRAIN_DRAIN_SKIP selects,
 * read once through openpete_config_env: a value starting with '0' is 1,
 * any other value or none is 0.
 * @return 1 or 0. */
int  openpete_native_terrain_drain_legacy(void);
/** @brief Count a level edge: the terrain walk read a level id other than the previous walk's.
 *
 * Sim thread. A portal entry keeps canonical presents flowing through the
 * load, so the world never turns not-live there; this count is the event
 * the eager planner reads for the new level's first walk instead.
 * Renderer state only. */
void openpete_native_note_level_edge(void) OP_REQUIRES(sim);
/** @brief Return the number of level edges counted so far; sim thread.
 * @return The count, starting at 0. */
unsigned openpete_native_level_edges(void) OP_REQUIRES(sim);

/* One vertex of game-supplied FPS-overlay geometry, in a resolution-independent
 * overlay space: x/y are in units of WINDOW HEIGHT (origin = top-left, +y down,
 * so x and y share one scale -> digits stay square at any aspect); z is a
 * view-Z (>= 1, the scene vertex unit) for the overlay's own mini depth pass
 * (smaller = nearer). rgba is byte order R,G,B,A. */
typedef struct { float x, y, z; uint32_t rgba; } openpete_hud_vert;

/* Alpha byte of a text-surface vertex that draws opaque but seeds no outline:
 * an outlined surface's composite rings only texels of alpha 255 in black,
 * so a frame built in the same layer as the glyphs (the settings menu's)
 * takes this alpha and stays unringed. */
#define OPENPETE_HUD_ALPHA_NO_OUTLINE 0xFEu

/** @brief Hand the launcher's next presents its 3D title: glyph triangles drawn depth-tested at twice the window's size per axis and box-filtered over the launcher's image, under the screens' imgui draw.
 *
 * Set before each launcher present that draws it; a call with no triangles
 * clears it. Creates the title pass's pipelines on the launcher's device at
 * the first call that draws (from the pipeline cache when the launcher's
 * ahead compile reached them). Launcher thread only.
 * @param v The triangles, 3 vertices each: x and y in window pixels, z a
 *        view-Z of the overlay depth band (smaller nearer).
 * @param n The vertex count; below 3 clears the title.
 * @param win_w The window width in pixels the triangles are laid out in.
 * @param win_h The window height in pixels.
 * @return 1 when the next launcher present draws the title, else 0 (no
 *         launcher device, no triangles, or the pass could not start). */
int openpete_native_launcher_set_title(const openpete_hud_vert *v, int n, int win_w, int win_h);

/** @brief Hand the renderer the performance readout's glyph triangles.
 *
 * The glyphs are built each tick by the game-side emitter (text_moby_native)
 * from the game's own 3D glyph models (game_layout.font). The renderer copies
 * up to OVL_MAX_VERTS vertices and draw_perf_overlay draws them on the live
 * vk present. 0 vertices draws no readout; there is no other font, so the
 * list is empty only before the glyph models have loaded. Renderer state
 * only.
 * @param verts Triangle list, 3 vertices per triangle.
 * @param nverts Vertex count; 0 clears. */
void openpete_native_set_overlay_glyphs(const openpete_hud_vert *verts, int nverts);

/** @brief Set the overlay block's region rect in overlay space (window-height units).
 *
 * The game-side builder owns the number because it can measure a string;
 * the renderer reads it back and never re-derives it. The supersample
 * target, the composite quad and the glyph projection all use this one rect.
 * Both floats are published as one 64-bit word, so the present thread never
 * reads a width and height from different pushes. Glyphs are emitted
 * region-local (origin at the rect's top-left); the anchor places the rect.
 * A non-positive width or height is ignored.
 * @param w Region width.
 * @param h Region height. */
void openpete_native_set_overlay_extent(float w, float h);
/** @brief Read the overlay block's region rect published by openpete_native_set_overlay_extent.
 *
 * Both values come from one read of the published word. A value never
 * pushed, or not positive, reads as the default region size.
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL. */
void openpete_native_get_overlay_extent(float *w, float *h);

/** @brief Publish the build-version line's glyph vertices and the region rect they were emitted in.
 *
 * Both arrive in one call because the present thread reads them together
 * and they must describe the same push. The vertices are copied, clamped to
 * the renderer's buffer; the rect is stored as one packed word and keeps its
 * previous value when either dimension is not positive. The renderer pins
 * the line to the bottom-left anchor. A tagged release build holds it for
 * 2 s and fades it over 300 ms, a distribution build that is not a tagged
 * release keeps it for the whole run, and a build that is not a
 * distribution build draws none (openpete_native_version_line_enabled).
 * Display-only.
 * @param verts Triangle-list vertices, region-local, in overlay space.
 * @param nverts Vertex count; 0 clears the line.
 * @param region_w Region width in window-height units.
 * @param region_h Region height in window-height units. */
void openpete_native_set_version_glyphs(const openpete_hud_vert *verts, int nverts,
                                        float region_w, float region_h);

/** @brief Set the --version-osd value, which governs only the tagged release build's two-second version line.
 *
 * An untagged distribution build keeps its line for the whole run and no
 * knob suppresses it; a build that is not a distribution build shows none
 * either way. Live-settable.
 * @param on Non-zero to show the release build's line. */
void openpete_native_set_version_osd(int on);
/** @brief Return the --version-osd value in force.
 *
 * The value starts at 1 and governs only the tagged release build's line;
 * see openpete_native_set_version_osd.
 * @return 1 when the release build's line shows, else 0. */
int  openpete_native_version_osd(void);

/** @brief Return 1 when this build draws a version line.
 *
 * 0 for a build no distribution preset made (openpete_build_distributed):
 * such a build is never distributed. 1 for a
 * distribution build that is not a tagged release, whatever the knob: the
 * line is what names the build in a screenshot of it. For a tagged release,
 * the --version-osd value. The game-side builder asks before building glyph
 * geometry.
 * @return 1 when a version line is drawn, else 0. */
int openpete_native_version_line_enabled(void);

/** @brief Return a phrase naming the build class and the version-line policy it gets, for the shutdown census.
 *
 * The classes are the three of openpete_native_version_line_enabled: not a
 * distribution build, untagged distribution build, and tagged release (brief, or
 * suppressed by --version-osd off).
 * @return A static string, never NULL. */
const char *openpete_native_version_reason(void);

/** @brief Return the number of version-line draws since start.
 *
 * Counts every composite of the line on either path (supersampled or 1x);
 * the shutdown census prints it, and a check harness reads it to tell a
 * drawn line from a suppressed one.
 * @return The cumulative draw count. */
unsigned openpete_native_version_drawn(void);

/** @brief Set the corner or edge the performance-stats block anchors to.
 *
 * Live-settable. The builder reads it back to align each line inside the
 * region (right-anchored text right-aligns, centred text centres), so the
 * block's slack sits on the side away from the screen edge it hugs. Values
 * outside the anchor range are ignored.
 * @param anchor An openpete_anchor_t value (openpete_overlay_anchor.h). */
void openpete_native_set_perf_anchor(int anchor);
/** @brief Return the screen anchor of the performance-stats block.
 *
 * @return An openpete_anchor_t value; the top left until set. */
int  openpete_native_perf_anchor(void);

/** @brief Hand the renderer the mod draw service's overlay geometry.
 *
 * Rects and text glyphs from core/mods/mod_draw.c in the same overlay space
 * as the perf readout, pushed once per present. The renderer copies up to
 * MOD_OVL_MAX_VERTS vertices; each published scene slot carries its own copy.
 * Drawn on the vk variant in the live present and in screenshot and record
 * composes, since a mod's drawing is content. Renderer state only.
 * @param verts Triangle list, 3 vertices per triangle.
 * @param nverts Vertex count; 0 clears. */
void openpete_native_set_mod_overlay(const openpete_hud_vert *verts, int nverts) OP_REQUIRES(sim);

/** @brief Return the window aspect (width over height) for overlay-space layout.
 *
 * Overlay x spans [0, aspect] and y spans [0, 1].
 * @return The aspect, or 4/3 before the window size is known. */
float openpete_native_overlay_aspect(void);

/** @brief Return the output pixels per overlay unit, which is the window height.
 *
 * A game-side text builder snaps a computed origin to a whole pixel with it:
 * right-aligned or centred text derives its origin from the measured string,
 * so without the snap the origin moves by a fraction of a pixel whenever the
 * text changes and the glyphs land at a different sub-pixel phase each time.
 * Left-aligned text has a constant origin and needs no snap.
 * @return The window height in pixels, or 0 before the window size is known. */
float openpete_native_overlay_px_per_unit(void);

/* Capacity of the settings menu's 3D glyph layer, in vertices: the rows'
 * labels, values and title, the close glyph, and the lit frame. The emitter
 * and the renderer size their buffers from this one value. */
#define OPENPETE_MENU_GLYPH_MAX 65536

/** @brief Hand the renderer the in-game settings menu's glyph triangles.
 *
 * Row labels, values and title, built game-side by text_moby_native from the
 * same 3D glyph models, with the selected row's per-glyph yaw baked in. Same
 * overlay space as openpete_native_set_overlay_glyphs. The renderer copies up
 * to MENU_GLYPH_MAX vertices; draw_menu_overlay draws them over the menu
 * panel. Renderer state only.
 * @param verts Triangle list, 3 vertices per triangle.
 * @param nverts Vertex count; 0 clears. */
void openpete_native_set_menu_glyphs(const openpete_hud_vert *verts, int nverts);

/* ---- OSD toast geometry ------------------------------------------------
 * The OSD renders into a FIXED GRID: one supersample target OPENPETE_OSD_GRID_H
 * window heights tall, reallocated only when the window resizes.
 * Content-sized targets were the obvious design and are wrong - every SS
 * target rebuild waits the device idle, and toast arrivals, expiries and
 * coalescing repaints happen many times a second.
 *
 * The builder packs each toast's border and text into the grid top to
 * bottom, oldest first, each starting on a whole 1x pixel row, in
 * TARGET-LOCAL overlay space (row r's top is at y = r / window height), and
 * reports which toast landed at which row plus the box it filled. A toast
 * that does not fit below the ones before it gets no row this tick; it has
 * no geometry, so its timer does not start, and it takes a row when an
 * older toast retires. The compositor samples each box and places it by the
 * anchor ledger at the toast's own alpha.
 *
 * The table is keyed by TOAST ID, never by index: the ring is mutated on
 * other threads between this push and the present that consumes it. Geometry
 * and table are published TOGETHER, under one lock - an id-keyed table is
 * worthless if it can itself be read half-copied. */

/* The grid height in window heights: eight one-line toasts' 0.075 rows. */
#define OPENPETE_OSD_GRID_H    0.6f
/* Baseline-to-baseline distance of a wrapped toast's lines, overlay units:
 * the 0.030 cap height plus 0.010 leading. */
#define OPENPETE_OSD_LINE_STEP 0.040f
/* The most lines the grid holds over all toasts: every toast costs its
 * pad and a frame beyond its line steps, so 0.6 / 0.040 bounds the sum. */
#define OPENPETE_OSD_GRID_LINES 15
/* Glyph vertices one line can carry: the densest glyph of the three games'
 * fonts, Spyro 2's and Spyro 3's comma at 156 vertices per 0.0103 of
 * advance (15146 per overlay unit), times the 0.58 line width, rounded up. */
#define OPENPETE_OSD_LINE_VERTS 9000
/* Each toast's frame (emit_lit_border). */
#define OPENPETE_OSD_FRAME_VERTS 288
/* The OSD glyph vertex budget, shared by the builder and the renderer: the
 * grid's lines at the densest glyph, plus every toast's frame. */
#define OPENPETE_OSD_GLYPH_MAX (OPENPETE_OSD_GRID_LINES * OPENPETE_OSD_LINE_VERTS + \
                                OPENPETE_OSD_MAX * OPENPETE_OSD_FRAME_VERTS)

typedef struct {
    uint32_t id;         /* the toast this row holds                      */
    int      row_px;     /* its top row in the grid, 1x pixels            */
    float    box_w;      /* extent actually filled, overlay units         */
    float    box_h;
} openpete_osd_slot_t;

/** @brief Publish the OSD toast glyph triangles and the toast-to-slot table together.
 *
 * Both lists are copied under the OSD publish mutex and the present reads
 * them under the same mutex, so it never pairs one push's vertices with
 * another push's table. nverts is clamped to [0, OPENPETE_OSD_GLYPH_MAX]
 * (the renderer's glyph capacity; a clamp is counted and logged) and nslots to [0, OPENPETE_OSD_MAX]; the stored counts
 * replace the previous ones, so 0 empties a list. A NULL array with a
 * positive count keeps the previous contents under the new count.
 * @param verts Glyph triangles in target-local overlay space, three
 *              vertices per triangle; may be NULL when nverts is 0.
 * @param nverts Vertex count.
 * @param slots Toast-to-slot table keyed by toast id; may be NULL when
 *              nslots is 0.
 * @param nslots Table entry count. */
void openpete_native_set_osd_glyphs(const openpete_hud_vert *verts, int nverts,
                                    const openpete_osd_slot_t *slots, int nslots);

/** @brief Return the OSD grid's extent for the builder to lay out against.
 *
 * The numbers are the ones the compositor samples with: the grid's 1x pixel
 * size, as published with the supersample target, over the window height.
 * Before a target is built the width is OSD_SLOT_W and the rows
 * OPENPETE_OSD_GRID_H times the window height.
 * @param w Receives the grid width, overlay units; may be NULL.
 * @param px Receives one 1x pixel row's height, overlay units; may be NULL.
 * @param rows Receives the grid height in 1x pixel rows; may be NULL. */
void openpete_native_osd_slot_extent(float *w, float *px, int *rows);

/** @brief Return the number of OSD toasts composited so far this run.
 *
 * An engagement counter: a capture-based gate reading zero here never drew a
 * toast, whatever its pixels show. Logged at shutdown when the OSD is on.
 * @return The cumulative toast count. */
unsigned openpete_native_osd_composited(void);

/** @brief Count OSD glyph vertices the builder could not fit in its budget.
 *
 * Called by the glyph builder; the count joins the publish clamp in
 * openpete_native_osd_clamped. Sim thread.
 * @param verts The vertices refused this tick; 0 or less counts nothing. */
void openpete_native_osd_count_dropped(int verts);

/** @brief Return the OSD glyph vertices lost so far this run to the budget.
 *
 * The sum of the builder's refusals and the publish clamp at
 * OPENPETE_OSD_GLYPH_MAX. Nonzero means a toast drew without some of its
 * glyphs. Logged at shutdown with openpete_native_osd_composited.
 * @return The cumulative vertex count. */
unsigned long long openpete_native_osd_clamped(void);

/** @brief Turn path-isolation mode (--render-paths) on or off.
 *
 * While on, a present whose scene has no game geometry still renders the
 * scene when its background colour is non-zero, instead of falling back to
 * the PSX VRAM framebuffer: the VRAM holds the unfiltered image, and showing
 * it would defeat the isolation. The game enables it when a --render-paths
 * selection is in effect.
 * @param on True for on. */
void openpete_native_set_isolation(bool on);

/** @brief Record the active PSX display rect (DISPENV disp, VRAM pixels).
 *
 * The scanned-out double-buffer page. The renderer crops it vertically to
 * the DRAWENV clip rect (openpete_native_set_draw_clip), dropping the
 * overscan letterbox. Called from the PutDispEnv thunk each frame; a size
 * change is logged when the native path is enabled. Each published scene
 * slot carries the rect.
 * @param x VRAM x.
 * @param y VRAM y.
 * @param w Width in VRAM pixels.
 * @param h Height in VRAM pixels. */
void openpete_native_set_display_rect(int x, int y, int w, int h) OP_REQUIRES(sim);
/** @brief Record the active PSX DRAWENV clip rect, the drawn region the display rect is cropped to.
 *
 * Called from the PutDrawEnv thunk. A change of size is logged when the
 * native path is enabled, and the first nonempty rect is checked once
 * against the game layout's display constants with a warning on a
 * mismatch; the window is not resized.
 * @param x VRAM x.
 * @param y VRAM y.
 * @param w Width in VRAM pixels.
 * @param h Height in VRAM pixels. */
void openpete_native_set_draw_clip(int x, int y, int w, int h) OP_REQUIRES(sim);

/* ---- native 3D scene (cyclorama/sky and world from gamestate) -------
 *
 * The game-specific layer (games/<game>/) re-derives 3D geometry from
 * gamestate in float and pushes screen-space triangles here; core draws
 * them natively into panel 0 (the "native render", replacing the
 * re-displayed PsyCross framebuffer). Coordinates are PSX screen pixels
 * (the same space the GTE projects into: x in [0,disp_w], y in [0,disp_h],
 * OFX/OFY-centred); core maps them into the panel, cropping the overscan
 * band like the reference. rgba is byte order R,G,B,A.
 *
 * Determinism-safe: this is host-side display state only; the game's push
 * reads gamestate but writes nothing PSX-readable. Inert unless the native
 * path is live. */
/* z is the ORDERING view-Z the native depth buffer is written from:
 * full-world view-Z, >= 1 (see openpete_native_scene_viewz). Actors map their
 * view-space Z through openpete_native_scene_viewz; the cyclorama/sky passes
 * OPENPETE_DEPTH_VIEWZ_FAR (always the backdrop); screen-space 2D content
 * passes 1.0. */
/* u,v are texel coords within the texture page (PSX integer texel space, 0..255)
 * - only meaningful when the tri is textured. */
/* pvz is the TRUE full-world view-Z of this vertex - the perspective W the
 * rasterizer divides UV/colour by. It is separate from `z` because `z` is an
 * ORDERING quantity: feeders shift it (openpete_worldvz_pull/_bias, the OT-slot
 * mirrors) to reproduce the PSX's per-object OT placement, and a shifted depth
 * implies a wrong W. Where the shift is large - the level-transition flight
 * pulls the whole player 2488 world units so its nearest vertex sits on the
 * near plane - W varies 2:1 across a face that truly spans 1.01:1, smearing one
 * texel over the snout. State the real view-Z here and `z` stays free to encode
 * ordering. MANDATORY (> 0): an unshifted feeder states the same value as `z`;
 * screen-space 2D content states 1.0. A vertex that reaches
 * openpete_native_scene_add_tri with pvz <= 0 is counted
 * (openpete_native_scene_pvz_missing) and takes its `z`. */
typedef struct {
    float sx, sy, z; float u, v; uint32_t rgba; float pvz;
    uint32_t rgbq;   /* the residual of rgba's R, G and B from the unrounded
                      * colour of a split terrain node (rhi_vertex.rgbq); 0
                      * on every other vertex. */
} openpete_scene_vert;
/* A scene triangle. When `textured` is 0 it is gouraud-shaded from the vertex
 * rgba (the cyclorama, and untextured moby faces). When 1, it samples the PSX
 * VRAM mirror via the material below (tpage/CLUT/bpp in VRAM-pixel units, exactly
 * the GTE/GPU encoding) and modulates by rgba (tex*colour/128).
 * A textured tri's vertex ALPHA byte is not a blend alpha. 0xFF, which every
 * feeder but the moby walk states, draws the tri's CLUT row alone. A value
 * with a high nibble h below 15 and a nonzero low nibble q, constant over the
 * tri, blends the drawn colour by q/30 toward the colour the same CLUT index
 * h - 7 rows from clut_y draws (the moby fog band between two ticks;
 * psx_scene.frag clut_fog_mix). */
/* blend: 0 = opaque; 1 = PSX semi-transparent ABR mode 0 ((B+F)/2) -> alpha blend;
 * 2 = ABR mode 1 (B+F) -> additive blend (flame/glows: src ONE + dst ONE);
 * 3 = ABR mode 2 (B-F) -> subtractive blend (shadows: reverse-subtract,
 * src ONE + dst ONE - the shadow colour darkens the framebuffer). All
 * transparent classes are depth_write_off (test vs opaque, never occlude
 * another transparent layer). ABR mode 3 (B+F/4) + back-to-front sort are still
 * TODO (no consumer yet).
 *   TEXTURED tris go through the VRAM sampler (tex*col/128) for every class.
 *   UNTEXTURED gouraud tris support classes 0 (opaque), 1 (alpha - the
 * scene intake sets the vertex ALPHA byte to 128 for (B+F)/2), 3 (subtract - reverse-
 * subtracts the vertex colour, alpha ignored) and 2 (additive, B+F - the glow
 * rings: gouraud fan, bright centre fading to black edge, alpha ignored). */
/* category: which draw layer an OPAQUE (blend==0) tri belongs to. The opaque
 * pass is split WORLD-opaque -> shadows(subtract) -> ACTOR-opaque so a shadow
 * (a ground decal pulled slightly toward the camera to clear terrain bumps)
 * draws OVER the terrain it falls on but UNDER the actors standing on it - the
 * PSX draw order (terrain, then shadow, then Spyro on top). Ignored for
 * non-opaque tris (blend!=0 routes by blend). Set by add_tri from its explicit
 * `category` argument; feeders that don't care pass OPENPETE_SCENE_WORLD. */
#define OPENPETE_SCENE_WORLD 0
/* ACTOR - regular + shaded mobys (the rescued dragon, level mobys, gems, ...). Drawn
 * after shadows, then the HUD/overlay layer goes ON TOP of them. */
#define OPENPETE_SCENE_ACTOR 1
/* HUD/overlay mobys - the elements the PSX forces into the moby OT's NEAR slot:
 * the treasure/lives HUD icons, the "DEMO MODE" banner, and the level-intro /
 * "RESCUED <dragon>" text. Drawn with the depth test OFF, in the layer BETWEEN the
 * ACTOR group (regular mobys) and the PLAYER group (Spyro), so they sit on top of
 * the world/terrain AND other mobys (not Z-occluded; the near-OT slot draws over
 * other mobys like the rescued dragon) but write no depth - so the player Spyro,
 * drawn next, still covers them (Spyro flies in front of the intro text). Mirrors
 * the PSX moby OT: near-slot UI over other mobys, the player rendered on top.
 * WITHIN the layer, untextured tris draw back-to-front by centroid z (stable:
 * equal-z 2D content keeps submission order) - the depth-off analogue of the PSX
 * OT bucket order, so the rotating near-OT mobys (the inventory screen's spinning
 * letters/dragon icons) self-occlude correctly instead of painting inside-out.
 * Routed independently of `blend`. Tagged by the feeder from the UI-moby classifier
 * OR the .s billboard OT-bias (see moby_native near_ot). */
#define OPENPETE_SCENE_HUD   2
/* PLAYER - the player Spyro's body. An opaque actor like ACTOR, but drawn AFTER the
 * HUD/overlay layer so Spyro covers the near-OT HUD text (the level-intro fly-in),
 * whereas regular mobys (ACTOR) stay UNDER it. The only player feeder is
 * spyro_native; everything else uses ACTOR. */
#define OPENPETE_SCENE_PLAYER 3
/* OVERLAY - 2D prims from the HUD ordering table (func_800168DC), which the PSX
 * draws ON TOP OF THE ENTIRE 3D scene (a separate OT presented over the scene OT):
 * the cutscene letterbox bars, and any full-screen 2D flat quad. Drawn DEAD LAST
 * with the depth test off, OVER everything incl. the player. (The level-intro /
 * "RESCUED" text is NOT here - that is moby near-OT geometry = HUD, under the
 * player.) Untextured opaque; routed by the feeder. */
#define OPENPETE_SCENE_OVERLAY 4
/* HUD_PANEL - a semi-transparent 2D HUD-OT quad that sits UNDER the HUD text/icons
 * but OVER the world/backdrop: the dim panel behind the pause / inventory menu text
 * (func_8001A40C, setRGB0(64,64,64)) and the "shaded box" behind in-game HUD text
 * (func_8001860C, setRGB0(112,112,112)), both emitted as a POLY_F4 code 0x2A via
 * func_800168DC. Depth-test OFF, drawn at the START of the HUD layer (before the HUD
 * text), so the text reads on top. Distinct from OVERLAY (dead-last, over text) and
 * from the full-screen fade (also a 0x2A F4, but emitted by func_800190D4 - the
 * feeder excludes that via a re-entry flag). Untextured. The 0x2A code only means
 * "semi-transparent"; the actual ABR RATE comes from the box's DR_MODE (getTPage abr)
 * - BOTH these boxes set ABR mode 2 (B-F subtractive), which DARKENS the framebuffer
 * by the box colour (NOT (B+F)/2 alpha - that washes it toward grey). The feeder reads
 * the ABR per-prim into `blend` (3 = subtractive, 1 = alpha), and the HUD-panel draw
 * picks the matching pipe per box. */
#define OPENPETE_SCENE_HUD_PANEL 5
/* HUD3D - the in-game 3D HUD icon mobys (g_Hud.m_Mobys: the spinning gem/treasure,
 * dragon-head, Spyro-head/lives, key, and number digits). Unlike the flat 2D HUD
 * layer, these are real 3D models, so they need a Z-buffer to sort their own faces
 * (depth-test OFF makes them paint inside-out). Drawn LAST, on top of everything,
 * after a mid-pass depth clear (rhi_clear_depth) so they sort internally yet are
 * never occluded by world geometry. Only the static g_Hud range routes here
 * (openpete_ui_moby_is_addr_static); the polybuf text mobys stay flat 2D = HUD. */
#define OPENPETE_SCENE_HUD3D 6
/* PORTAL_CYCLORAMA - the destination-level cyclorama preview shown inside a homeworld
 * portal arch (portal.c). It is a far BACKGROUND seen through the arch
 * window: the arch stone must occlude it, but it must never occlude actors or
 * Spyro flying into/through the portal. Drawn in its own pass AFTER the WORLD
 * group (so it depth-tests against the already-written arch/terrain depth) but
 * with depth-WRITE OFF, so the actors/player drawn next are never per-pixel cut
 * by it. This mirrors the PSX OT, where the portal sky occupies one coarse
 * bucket and the player's whole model sorts in front of it (never intersecting
 * it per-pixel the way a shared Z-buffer otherwise would). Untextured only;
 * self-occlusion comes from the feeder's own far->near painter sort. */
#define OPENPETE_SCENE_PORTAL_CYCLORAMA 7
/* PORTAL_MASK - the stencil aperture mask for a portal-sky group. The
 * portal feeder emits each portal's sky FOOTPRINT again under this category
 * (same geometry+z, colour irrelevant): core rasterizes it through an
 * invisible (colour-write-off) depth-TESTED stencil-MARK pass, so the mask is
 * set exactly where the sky is visible past the world depth (the arch stone /
 * a wall in front punches it out). The PORTAL_CYCLORAMA colours then draw
 * depth-FREE, gated on the mask - so dome / flat base / backstop within one
 * portal composite by submission order (painter) instead of z-arithmetic, and
 * portals are isolated from each other per group (rhi_clear_stencil between
 * groups; groups drawn far->near by their plane depth). Untextured only. */
#define OPENPETE_SCENE_PORTAL_MASK 8
/* PORTAL_GUARD - near-band terrain PROTECTION (the PSX mask bit).
 * The .s drains deferred terrain (depthSum < 0x2000 = mean world vz < 8192,
 * minus word3&0x80 faces) via immediate DrawPrim, which the PSX renders with
 * the mask bit SET; every OT prim - including the portal sky - draws with
 * mask-CHECK and is REJECTED on those pixels. So near-band terrain always
 * beats the portal preview on hardware, regardless of OT order. The terrain
 * feeder re-emits exactly those faces (its face_is_near classifier) under
 * this category when a portal fed sky recently; core re-draws them right
 * AFTER the portal-sky sequence (the sky writes no depth, so the redraw
 * re-wins its own pixels - the depth-buffer realization of the mask).
 * Opaque only by construction (the defer test excludes ABE faces). */
#define OPENPETE_SCENE_PORTAL_GUARD 9
/* SKY - the main cyclorama dome (cyclorama.c). Drawn exactly like WORLD
 * (opaque, depth-tested, before everything else) EXCEPT it does not stencil-MARK
 * terrain coverage: the WORLD group's mark defines "pixels real world
 * geometry covered", and the hole-fill LP backing (WORLD_FILL) paints only where
 * that mark is absent - which is precisely where the sky shows through a terrain
 * seam. If the dome marked, every hole would be self-masked and the fill dead. */
#define OPENPETE_SCENE_SKY 10
/* WORLD_FILL - the terrain LP backing layer for sectors whose HP is drawn
 * (hole-fill). The PSX draws the coarse LP list first and the fine HP
 * over it, so LP shows through wherever HP leaves a T-junction seam against an
 * LP-only neighbour (the artisans distant-water gaps). Natively, drawing LP
 * behind HP with a depth bias poked through on non-flat sectors (the coarse
 * mesh bulges in front of the fine one).
 * Instead the WORLD group stencil-MARKs its coverage and this layer draws right
 * after it with RHI_STENCIL_TEST_ZERO: it can only touch pixels NO world
 * geometry covered, so poke-through is impossible by construction and every
 * genuine hole is filled with the same LP the PSX shows there. Depth-tested and
 * depth-written at TRUE LP depth (no bias needed). Untextured only (LP is
 * gouraud G4/G3). EXCLUDED from the occlusion depth pre-pass: rasterized
 * unstencilled there, a bulging LP would be a phantom occluder. */
#define OPENPETE_SCENE_WORLD_FILL 11

/* SHADOW: the drop-shadow decals (Spyro silhouette + moby shadows, blend==3).
 * Split from plain blend-3 so the SUBTRACT draw group's DECAL special-casing
 * (one shared shadow-sprite material for the whole textured range +
 * shadow_decal_tex.frag's ground-height/wall discard) applies ONLY to real
 * shadows. Genuinely subtractive world content - the dragon-fragment SMOKE
 * PUFFS (particle sel-2 billboards, gray ramp, ABR 2) - stays category WORLD
 * and draws through the plain subtractive pipes in the same slot; routing it
 * through the decal path discarded every sky-borne puff (height test) and
 * sampled the wrong texture (the range-uniform shadow sprite material). */
#define OPENPETE_SCENE_SHADOW 12
/* ACTOR_FRONT: the dragon-rescue CUTSCENE actors - the mobys the game keeps in
 * g_DragonCutscene (m_RescuedDragonMoby / m_CutsceneDragon / m_CutsceneSpyro).
 * They render like ACTOR (opaque, depth-tested) EXCEPT during a rescue cutscene,
 * when draw_native_scene front-passes them: a colour-write-off PUNCH pass
 * (depth compare ALWAYS) replaces the stored depth on exactly the pixels they
 * rasterize, then the ordinary opaque draw wins over the terrain it
 * interpenetrates (Delvin's spread wing inside the tower) while keeping exact
 * self-occlusion. Punching per PIXEL, not over a screen bbox, is what keeps the
 * fix local: every pixel the actors do not cover keeps its true depth, so
 * distant level mobys (--lod extended un-culls statues / chests / the level-end
 * vortex + critters) and semi-transparent terrain - both drawn after - keep
 * true occlusion. The pass runs BEFORE the PLAYER group so Spyro sorts against
 * the dragon by real Z instead of being painted over. Tagged by the moby feeder
 * via pointer match against g_DragonCutscene (the game's own actor set - no
 * heuristic). */
#define OPENPETE_SCENE_ACTOR_FRONT 13
/* TERRAIN_KEY: RETIRED (number reserved - do not reuse). Formerly the
 * portal-aperture og-key fan: the sky bucket's key rastered into the
 * HP key image to discard og-covered terrain, plus the WORLD_FILL stencil
 * pre-mark. Replaced by the plane-cover rule: the aperture mask and preview
 * colours test at opening plane − margin, covering everything behind it per
 * pixel - the .s's OT pushes are tier-asymmetric (fan +64 slots, LP +64+8b,
 * HP only +4b), so the og-key verdict breaks wherever tier and distance
 * decouple. No feeder emits this category any more. */
#define OPENPETE_SCENE_TERRAIN_KEY 14
/* carry: 1 = a tri is screen-space 2D-UI that the sub-tick re-extract canNOT
 * re-derive (it has no 2D-prim chokepoint to hook), so it must be snapshotted
 * before the rebuild and re-applied after - like the OVERLAY/HUD_PANEL carry, but
 * for tris that share a re-derivable category (the 2D-UI sprites/lines live in
 * OPENPETE_SCENE_HUD, which the moby feeder ALSO populates with re-derived near-OT
 * billboards). A category check can't tell them apart; this flag can. Passed as
 * add_tri's explicit `carry` argument (0 unless a feeder opts in). */
/* The FEEDER-FACING tri: geometry + material only. The routing state -
 * category / carry / portal group - is NOT a struct field: it travels
 * exclusively through the add_tri arguments and lives on the renderer's
 * internal stored tri (native_render.c). A feeder that tries to write
 * `tri.category = ...` gets a compile error instead of a silently-ignored
 * field. */
typedef struct {
    openpete_scene_vert v[3];
    int textured;
    int tpage_x, tpage_y, clut_x, clut_y, bpp;
    int blend;
    float koff;  /* og OT KEY: the face's ABSOLUTE .s insertion key in world
                  * units + 0.5 sentinel; sign<0 = near-band contest-only
                  * (|koff| = key), 0 = not keyed. Stored depth (v[].z) is
                  * TRUE view-Z either way - og's keys select WHICH surface
                  * owns a pixel, they never enter the depth buffer.
                  * Per-face constant.                                     */
    int prop;    /* authored embedded PROP (pedestal discs, statue bases...):
                  * this face additionally rasters TRUE per-vertex depth
                  * into the prop image, and near-band terrain always
                  * yields to a prop physically within the embed bound -
                  * the class-curated "props render on top of the ground
                  * they are set into" divergence.                         */
    float rank;  /* og OT paint RANK (walk order, >=1; 0 = unranked): the
                  * game paints a moby's faces whole, in OT walk order -
                  * never per-pixel between same-moby faces. Ranked opaque
                  * faces raster (rank, ordinal) into the rank image and a
                  * fragment loses its pixel iff a LATER-ranked face of the
                  * SAME moby covers it. Stored depth stays TRUE per-vertex
                  * - rank decides WHICH same-moby face exists at a pixel,
                  * never what depth it writes. Cross-moby ordering stays
                  * true per-pixel z (the ordinal gate).                   */
    int mordinal;/* present-local moby ordinal (1..255) identifying "same
                  * moby" in the rank image and the snap table, unique over
                  * every moby emit of the scene. Present-LOCAL only: a pool
                  * ordinal is not an identity across presents.            */
    float pkey;  /* PLAYER ordering key: the avatar draw's own
                  * max(0, (part-origin view-Z >> 7) - sortDepth) slot + 1
                  * (0 = none). The face rasters that slot into the ACTOR
                  * key image's player band (psx_key_contest.glsl), where
                  * the additive glow fan is its only reader; the face
                  * itself contests nothing - its koff stays 0 and its
                  * stored depth stays true, so an ordering pull can never
                  * show the avatar through geometry in front of him.     */
    uint32_t mid;/* moby identity of an actor face: the emitting moby
                  * record's PSX RAM word index + 1 (0 = none), unique per
                  * moby within a present. The actor key pass writes it
                  * beside the face's key, and an unranked actor fragment
                  * does not contest a key its own moby owns
                  * (psx_key_contest.glsl op_akey_select): order inside one
                  * moby is decided by true depth and the rank. The scene
                  * vertex's rank pair y (rhi_vertex.sy) carries one of
                  * three values: a negative drain record tag (terrain), the
                  * mordinal (ranked faces and every non-actor face), or mid
                  * (unranked actor faces), which op_akey_owner selects.   */
    int shell;   /* 1 = an actor face of a moby record whose draw links all
                  * its prims into one ordering-table bucket and that emitted
                  * blended and opaque faces this present (moby_shell.glsl):
                  * its blended faces raster the front shell, its opaque
                  * faces write depth inside it. 0 = none, and on a blended
                  * face whose screen footprint is not the true projection
                  * (near-plane fixup or coordinate saturation).           */
    int key_order;/* 1 = a keyed actor face of a class that draws by the
                  * game's key order against terrain (game_layout.moby
                  * key_order): the actor key pass marks its texels, and
                  * a terrain fragment whose key sorts behind the marked key
                  * yields the pixel (psx_key_contest.glsl
                  * op_key_order_yields). 0 = true depth decides.          */
    int ot_slot1;/* 1 + the ordering-table slot the game links the face
                  * into (moby faces: the world bucket the draw splices the
                  * face into), 0 = unknown. The compose compares it with
                  * the fade's slot (openpete_native_scene_set_fade_slot).  */
    uint32_t kattr;/* terrain override inputs of a keyed opaque terrain
                  * face (psx_key_contest.glsl op_key_override_holds), 0 =
                  * none: bits 15-22 the face's class byte (128 for HP, plus
                  * the authored slot term / 4), bits 0-14 q with the
                  * distance from the eye to the triangle's plane = (q/64)^2
                  * world. An unranked face (rank 0) carries it in the rank
                  * channel as 0.25 + kattr * 2^-25 (OPENPETE_SCENE_KATTR). */
    float skey;  /* snap-only key of a blended actor face, koff's encoding
                  * (absolute key + 0.5; 0 = none). The face's vertices
                  * carry it in the key channel only where the face draws
                  * through the blended snap twin, whose stage reads it in
                  * the snap (psx_moby_snap.glsl) and runs no key contest
                  * with it; koff stays 0, so no key image takes the face. */
    float akc;   /* an opaque keyed actor face's moby-against-moby key less
                  * its ordering view-Z: the key at a pixel is the face's
                  * ordering view-Z there (openpete_scene_vert.z,
                  * interpolated) plus akc, its written depth less 128 x
                  * m_DepthOffset. Constant per face (the record's tie pull
                  * and near-floor lift, and the authored offset), so one
                  * value serves the face's every pixel. 0 elsewhere.       */
    int akpx;    /* 1 = the actor key pass rasters the face's per-pixel
                  * moby-against-moby key (akc) instead of koff: every
                  * keyed opaque actor face except the key-order class,
                  * whose key the terrain yield reads (key_order).          */
    uint32_t cbits;/* class bits (OPENPETE_CB_*): the record class or the
                  * emit decision that applied to the triangle, set where it
                  * is emitted; 0 from a feeder that sets none. Read by the
                  * tile census only (core/diag/tile_census.def).          */
} openpete_scene_tri;

/* Class bits of a scene triangle (openpete_scene_tri.cbits). A bit is never
 * reused; bits 0 to 6 are the ones the moby walk carries per walk triangle. */
#define OPENPETE_CB_MOBY_GPF         (1u << 0)  /* the record's GPF translation rescale ran */
#define OPENPETE_CB_MOBY_ENV         (1u << 1)  /* the face takes environment-map UVs */
#define OPENPETE_CB_MOBY_SIMPLE      (1u << 2)  /* a simple-model record */
#define OPENPETE_CB_MOBY_SCREEN      (1u << 3)  /* ... in screen space (a named screen origin) */
#define OPENPETE_CB_MOBY_UNR         (1u << 4)  /* a vertex in the GTE divide-overflow regime */
#define OPENPETE_CB_MOBY_SAT         (1u << 5)  /* a vertex coordinate-saturated */
#define OPENPETE_CB_MOBY_NEARCUT     (1u << 6)  /* a part of a triangle the moby near clip cut */
#define OPENPETE_CB_MOBY_TIEPULL     (1u << 7)  /* an opaque keyed face with a nonzero tie pull */
#define OPENPETE_CB_TERRAIN_NEARCLIP (1u << 8)  /* a part of a terrain triangle the near plane cut */
#define OPENPETE_CB_COUNT            9u         /* bits defined */

/* The rank-channel value of a terrain face's kattr: in [0.25, 0.5), which
 * every rank reader takes as unranked and not an actor. */
#define OPENPETE_SCENE_KATTR(k) (0.25f + (float)(k) * (1.0f / 33554432.0f))

/* Rank sentinels for faces that are not ranked but must still be told
 * apart by the key contest (openpete_scene_tri::rank): -1 = an opaque
 * actor face, -1.25 = an opaque actor face of an embedded prop, -2 = an
 * additive glow fan. Every rank consumer reads all three as unranked; the
 * contest reads them as the class whose arm to run. The prop sentinel sits
 * on the actor side of every threshold (actor < -0.5, aura < -1.5), so a
 * prop face runs the actor contest minus its terrain-key arm: terrain in
 * front of a prop within the embed bound yields to it, and the prop never
 * yields back. */
#define OPENPETE_SCENE_RANK_ACTOR (-1.0f)
#define OPENPETE_SCENE_RANK_PROP  (-1.25f)
/* -0.875 = an opaque actor face of the terrain-order class
 * (game_layout.moby.terrain_order): an actor at every threshold, and it
 * also loses to a terrain key in its own slot or a nearer one
 * (psx_key_contest.glsl op_terrain_order_loses). */
#define OPENPETE_SCENE_RANK_TORDER (-0.875f)
#define OPENPETE_SCENE_RANK_AURA  (-2.0f)

/* World view-Z per ordering-table slot: the games insert a face at its view-Z
 * shifted right by 7, so one slot of ordering bias moves a key by 128 world
 * units. The C mirror of OP_KEY_SLOT (psx_key_contest.glsl). A double
 * constant; a float expression casts it to float. */
#define OPENPETE_OT_SLOT_WORLD 128.0

/** @brief Stamps a world triangle with the ordering-table bucket the game links it at and, when asked, the aura contest by that bucket.
 *
 * A bucket of 0 or more is carried in ot_slot1. With `contest` and a bucket
 * above 0 the triangle also carries the aura rank and a koff whose slot is
 * the bucket less one: a routine that appends its primitive at the tail of
 * the bucket, after the moby faces the moby draw spliced there, paints over
 * the faces of its own bucket, so the triangle loses a pixel only to an
 * actor or player key in the next nearer slot or nearer (psx_key_contest.glsl
 * op_aura_loses). The contest runs on additive triangles alone (blend 2,
 * scene_aura_boxes). Bucket 0 is drawn after every moby face and contests
 * nothing. The stored depth is left alone, so terrain still orders the
 * triangle by true depth.
 * @param st The triangle.
 * @param bucket The bucket, or -1 when unknown (nothing is stamped).
 * @param contest Nonzero to run the aura contest by the bucket. */
static inline void openpete_scene_tri_set_bucket(openpete_scene_tri* st, int bucket, int contest) {
    if (bucket >= 0) st->ot_slot1 = bucket + 1;
    if (contest && bucket > 0) {
        st->koff = (float)((double)(bucket - 1) * OPENPETE_OT_SLOT_WORLD + 0.5);
        st->rank = OPENPETE_SCENE_RANK_AURA;
    }
}

/* ---- scene view-Z: the ONE named point ------------------------------------
 *
 * The scene vertex stream carries depth as FULL-WORLD view-space Z (the GTE
 * MAC3 of full-world camera-relative coords; larger = farther, 1.0 == one GTE
 * unit), unencoded. The vertex stages clamp it to [1, OPENPETE_DEPTH_VIEWZ_FAR]
 * and derive the depth-buffer value from it: 1/vz on the float depth format
 * (reversed: larger is nearer, GREATER_OR_EQUAL, clear 0), 1 - 1/vz on the
 * D24 fallback (LESS_OR_EQUAL, clear 1). Both are affine in 1/vz, so they
 * interpolate exactly in screen space.
 *
 * The feed takes an openpete_worldvz, which can ONLY be built through
 * the unit-claiming constructors below - a bare float is a compile error. A
 * GTE path that produces sub-scaled Z must normalize through _scaled naming
 * its unit. This enforces the "every feeder hands in the SAME absolute scale"
 * invariant whose prose-only form shipped ×4 / ×2^m_scale sorting bugs
 * (world/4 feeders drew through terrain). Known per-feeder units:
 *   terrain            world/16 (camera>>4, verts /16)  -> _scaled(vz, 16)
 *   moby shadows       world/4  (cam+pos >>2)           -> _scaled(vz, 4)
 *   moby records       world/2^m_scale - normalized at project time; feeds _full
 *   particles (world)  world/4 - normalized at build (PART_VZ_WORLD4); feeds _full
 *   spyro / flame / tracer / portal / glows  full-world  -> _full
 *
 * GPU mirrors of the clamp and the depth curves: core/rhi/shaders/psx_viewz.glsl
 * (included by psx_scene.vert, quad.vert, prepass.vert, terrain_key.vert,
 * shadow_decal_tex.vert and terrain.vert). */
/* The hyperbolic curve's scale: 1.0 == 4096 GTE units spreads typical actor
 * depths across the usable range. The key images store
 * z = vz / (vz + OPENPETE_DEPTH_VIEWZ_SCALE). */
#define OPENPETE_DEPTH_VIEWZ_SCALE 4096.0f
/* The farthest view-Z the vertex stages keep: the clamp bounds W. Its value is
 * 4096 * 0.9999 / (1 - 0.9999) in float, the view-Z whose hyperbolic depth is
 * 0.9999. It is the backdrop view-Z the cyclorama feeds: reversed depth
 * 1/W = 2.4e-8, above the clear of 0 and below every nearer surface; forward
 * depth 1 - 1/W rounds to the clear of 1. */
#define OPENPETE_DEPTH_VIEWZ_FAR 0x1.386abap+25f

/* A full-world view-Z. Only the constructors below produce one. */
typedef struct { float vz; } openpete_worldvz;

/** @brief Wrap a view-Z that is already in full-world units.
 *
 * vz is a raw GTE MAC3 of full-world coordinates, or a value normalized to
 * full-world upstream where its unit is known.
 * @param vz full-world view-Z.
 * @return the view-Z unchanged. */
static inline openpete_worldvz openpete_worldvz_full(float vz) {
    openpete_worldvz w; w.vz = vz; return w;
}
/** @brief Convert a sub-scaled view-Z to full-world by multiplying by its unit divisor.
 *
 * The unit claim for sub-scaled GTE paths: vz * mul is full-world. mul is
 * 4 for paths with the camera shifted >>2, 16 for terrain (camera >>4),
 * and (1 << m_scale) for moby record scale.
 * @param vz view-Z in units of full-world / mul.
 * @param mul the unit divisor.
 * @return the full-world view-Z. */
static inline openpete_worldvz openpete_worldvz_scaled(float vz, float mul) {
    openpete_worldvz w; w.vz = vz * mul; return w;
}
/** @brief Apply a proportional ordering bias to a full-world view-Z.
 *
 * mul > 1 pushes the surface behind, mul < 1 pulls it nearer. Applied after
 * unit normalization; the caller is shadow_depth with the
 * OPENPETE_SHADOW_BIAS_SPYRO / _MOBY factors. _scaled states the unit a
 * value is in; _bias states ordering policy.
 * @param z full-world view-Z.
 * @param mul the bias factor.
 * @return z scaled by mul. */
static inline openpete_worldvz openpete_worldvz_bias(openpete_worldvz z, float mul) {
    z.vz *= mul; return z;
}
/** @brief Apply a proportional ordering bias whose offset saturates at cap world units.
 *
 * The offset vz * (mul - 1) is clamped to [-cap, cap]; cap <= 0 leaves it
 * uncapped. An uncapped proportional bias grows with distance until it
 * exceeds the clearance between unrelated surfaces and reorders them. The
 * capped form keeps a biased surface strictly behind a coincident unbiased
 * one (for mul > 1) at every distance, and keeps the map vz -> vz' strictly
 * increasing, so two biased surfaces keep their relative order; a per-face
 * or per-sector exemption would tear one continuous surface at the
 * exemption boundary. No caller uses it at present.
 * @param z full-world view-Z.
 * @param mul the bias factor.
 * @param cap the offset bound in world units, <= 0 for none.
 * @return the biased view-Z. */
static inline openpete_worldvz openpete_worldvz_bias_capped(openpete_worldvz z, float mul, float cap) {
    float d = z.vz * (mul - 1.0f);
    if (cap > 0.0f) { if (d > cap) d = cap; else if (d < -cap) d = -cap; }
    z.vz += d; return z;
}
/** @brief Pull a full-world view-Z toward the camera by a fixed world-unit distance.
 *
 * Applied after unit normalization to reproduce an ordering-table tie-break:
 * the PSX links a moby's prims at its centre-depth bucket, so attachments
 * added later (sparkles) draw over the whole model regardless of per-pixel
 * depth. A constant pull larger than the body's protrusion gives the same
 * order under a Z-buffer. _bias is the proportional form.
 * @param z full-world view-Z.
 * @param world_units the pull distance.
 * @return z.vz - world_units. */
static inline openpete_worldvz openpete_worldvz_pull(openpete_worldvz z, float world_units) {
    z.vz -= world_units; return z;
}
/* One slot of the 3D ordering table in full-world view-Z: the moby, glow and
 * effect routines all key on MAC3 >> 7. */
#define OPENPETE_OT_SLOT_VZ 128
/* The moby feeders' one-slot tie pull, in full-world view-Z units: every moby
 * vertex is fed this much nearer than its true depth, so a moby wins the
 * surface it sits on, as the ordering table resolves same-slot moby-versus-
 * terrain ties. Less than one slot, so it flips only pairs the table calls a
 * tie. */
#define OPENPETE_MOBY_TIE_PULL_VZ 127.0
/** @brief Return an ordering-table key as a full-world view-Z on the moby feeders' depth scale.
 *
 * The moby path keys each face at its average-depth slot minus the
 * owning moby's authored depth offset, and feeds the face at its true depth
 * less the tie pull, so the faces of slot s land in [128*s - 127, 128*s + 1).
 * A primitive linked at key k over a moby whose offset is d sorts against that
 * moby's faces as the table does when fed at 128*(k + d) - 127: in front of
 * every face of slot k + d and beyond, behind every nearer one. `owner_slot` is
 * k + d. The quantization is the key's own.
 * @param owner_slot the key plus the owning moby's depth offset.
 * @return 128 * owner_slot - 127 as a full-world view-Z. */
static inline openpete_worldvz openpete_worldvz_owner_slot(int owner_slot) {
    return openpete_worldvz_full((float)((double)owner_slot * OPENPETE_OT_SLOT_VZ
                                         - OPENPETE_MOBY_TIE_PULL_VZ));
}
/** @brief Return the scene vertex depth (openpete_scene_vert.z / .pvz) of a view-Z, clamped below at 1.
 *
 * Behind-camera geometry is culled upstream; the clamp only guards the
 * near plane.
 * @param z full-world view-Z.
 * @return max(z.vz, 1). */
static inline float openpete_native_scene_viewz(openpete_worldvz z) {
    return z.vz < 1.0f ? 1.0f : z.vz;
}

/* ---- depth-aware shadow decal (height-above-ground wall rejection) ----------
 *
 * The Spyro silhouette shadow is a FLAT fan; over uneven near-terrain it sits
 * behind the bumps, so a pure depth test/band can't tell "my own bumpy ground"
 * (keep) from "a wall in front" (discard) - both put terrain nearer than the fan.
 * The fix keeps the flat fan's LOOK unchanged and only changes the keep/discard
 * decision: per pixel the shadow covers, the shader reconstructs the opaque
 * terrain's world HEIGHT from the depth pre-pass and discards where it sits well
 * ABOVE the shadow's ground plane (a wall). Bumps/ramps near ground height stay
 * shadowed.
 *
 * The reconstruction needs the camera transform, which only the game can read, so
 * the game passes it here each frame (display-only; never written back to PSX RAM).
 * `cam_col0..2` are 4096²·C⁻¹ rows (pushed numerical inverse - the Spyro camera
 * matrix is NOT orthonormal); the inverse recovers V = C⁻¹·MAC with the Spyro GTE
 * swizzle V=(-Δy,-Δz,Δx). AXIS NOTE: Spyro's VERTICAL world axis is
 * the **z field** - V1 carries -Δz, so world height = cam_pos_up − V1 (cam_col1).
 * The original decal tested cam_pos_y − V0, i.e. HORIZONTAL-Y proximity, which
 * rejected walls incidentally and left the wall-base notch. `ofx/ofy/h` are the
 * GTE projection regs. `ground_up` is the shadow's ground plane (SpyroShadow
 * world z). `wall_height` is the height slack above ground before a pixel counts
 * as a wall. valid=0 disables the test (shader draws the fan unmasked).
 *
 * A pure height threshold has one blind band: the BOTTOM wall_height units of a
 * wall rise from ground level, so the fan still painted on a wall's base
 * (dark-hollow f2700 notch). The classifier for that band is surface
 * ORIENTATION - own ground/bumps/slopes are walkable (up-normal large), a wall
 * face is near-vertical (up-normal ≈ 0). The shader reconstructs the terrain
 * normal from neighbouring pre-pass texels (differential - no camera position
 * needed) and discards steep surfaces IN FRONT of the fan fragment
 * (`steep_nup` = |up normal| below which a surface counts as a wall). */
typedef struct {
    float cam_col0[3];
    float cam_col1[3];
    float cam_col2[3];
    float cam_pos_up;     /* camera world height (the game's z field) */
    float ofx, ofy, h;
    float ground_up;      /* shadow ground-plane height (SpyroShadow z) */
    float wall_height;
    float steep_nup;
    /* Raised-occluder-in-front clause: discard fan pixels whose pre-pass
     * terrain is nearer than the fan fragment by more than plat_slack view-Z
     * units AND raised more than plat_eps above the shadow plane (a curb or
     * raised platform between the camera and the shadow). */
    float plat_eps;
    float plat_slack;
    int   valid;
} openpete_shadow_decal_params;
/** @brief Store the camera reconstruction parameters the shadow decal reads.
 *
 * Copies *p into the live block; NULL clears its valid field, which draws
 * the fan unmasked. Each scene publish snapshots the block into its ring
 * slot and a compose of that slot reads the snapshot; a compose of the
 * delayed history or the live accumulator reads the live block.
 * @param p Parameters from the game, or NULL to invalidate. */
void openpete_native_set_shadow_decal_params(const openpete_shadow_decal_params *p) OP_REQUIRES(sim);

/* The player ground-shadow fan in the form the footprint decal reads
 * (shadow_decal.frag, the std140 block at set 2). The decal shadows (A) the
 * fan's own surface where the depth rule keeps it and (B) the opaque terrain
 * whose world (x, y) lies under the ring, which is not steep, and whose height
 * lies between the fan less `below` and `caster_up`. Point 0 is the anchor,
 * points 1..16 the ring. `scr` is each point as fed: PSX screen x, y, the
 * ordering view-Z and the perspective W, so the fragment stage re-rasterizes
 * the fan exactly as the scene pipe would; `wrel` is the camera-relative world
 * (x, y, up) through the decal's own reconstruction. `tri_mask` bit k: the
 * triangle (anchor, ring k, ring k+1) was linked. Display-only. */
#define OPENPETE_SHADOW_FAN_POINTS 17
typedef struct {
    float caster_up;      /* world height (game z) of the caster */
    float footprint;      /* 1 = term (B) runs; 0 = term (A) alone */
    float tri_mask;       /* bits 0..15, exact in a float */
    float below;          /* world units a (B) receiver may lie below the fan */
    float anchor_rgb[4];  /* the fan's gouraud at the anchor, 0..1; the rim is 0 */
    float scr[OPENPETE_SHADOW_FAN_POINTS][4];
    float wrel[OPENPETE_SHADOW_FAN_POINTS][4];
} openpete_shadow_footprint;
/** @brief Store the player shadow fan's footprint block for the footprint decal.
 *
 * Copies *f into the live block. Each scene publish snapshots the block
 * into its ring slot and a compose of that slot reads the snapshot; a
 * compose of the delayed history or the live accumulator reads the live
 * block.
 * @param f Footprint block; NULL leaves the stored block unchanged. */
void openpete_native_set_shadow_footprint(const openpete_shadow_footprint *f) OP_REQUIRES(sim);

/** @brief Begin a sub-tick or lag re-extract's rebuild of the native scene.
 *
 * Called by each game's re-extract before its body feeds. Clears the
 * triangle list and the per-scene background, fade, freeze and flags,
 * latches the producer-read live settings
 * (openpete_native_producer_knobs_latch), bumps the scene epoch and resets the per-present
 * material slots. The first re-extract of a tick first records the game
 * render's blended-world key order from the scene it is about to clear; a
 * later re-extract of the same tick keeps that record. It then stamps the
 * cleared scene with openpete_frame through
 * openpete_native_scene_begin_frame, so the present stamp matches the stamp
 * the terrain builder tags the rebuild with whichever producers feed. The
 * game render's own accumulation begins with
 * openpete_native_scene_begin_frame. */
void openpete_native_scene_begin(void) OP_REQUIRES(sim);
/** @brief Latch the live settings the producers read, on the sim thread, at a point no tick's render is in progress.
 *
 * The live setters (settings menu, dev overlay, LOD hotkey) store wanted
 * values (openpete_live_latch.h) from whichever thread runs them; the dev
 * overlay runs on the present thread under eager production. This copies
 * them into the values the feeders and the guest render hooks read: vertex
 * precision, the FOV and its tangent, hide player, terrain visibility, the
 * LOD mode, fog multiplier and fog pin (openpete_no_lod_latch), the drawn
 * texture-mapping mode and the Hor+ factor the present published. A changed
 * FOV suspends the occlusion grid for two presents. Called by
 * game_on_frame_pre before every guest resume and by
 * openpete_native_scene_begin before a re-extract feeds; the guest renders a
 * tick inside one resume, so every reader of one tick sees one value. */
void openpete_native_producer_knobs_latch(void);
/** @brief Mark the scene now accumulating as a sub-tick or lag re-extract's rebuild.
 *
 * Called by the re-extract wrapper after the game's body has fed, so the
 * flag holds whatever the feeders' own begin_frame calls did while the body
 * ran. The blended-world block of a replay is reordered into the emission
 * order the game render last recorded, because the blend equations do not
 * commute once a channel clamps and the replay emits one producer at a time. */
void openpete_native_scene_mark_replay(void) OP_REQUIRES(sim);
/** @brief Begin the game render's scene accumulation for a VBL, once per VBL.
 *
 * Starts a new scene when @p frame differs from the last call's, or when a
 * call from the game's render (outside the sub-tick view) finds a
 * re-extract's rebuild under the same stamp, or finds the scene of an
 * earlier game tick's render under the same stamp after that render
 * submitted its OT or a capture took it (a render that skipped its VSync
 * wait; the tick is openpete_loop_anchor_ticks); otherwise it is a no-op, so
 * independent feeders (cyclorama, world, mobys, in any order) compose into
 * one scene: the first feeder of a VBL clears and the rest append, and a
 * feeder absent on a level breaks nothing. The game's render of a tick thus
 * never shares a scene with a re-extract. On a stamp change it first fires
 * the frame-close callback for the completed scene (a rebuild closed by the
 * game's render fires none); on either start it pushes
 * that scene with its background, fade, freeze and flags into the
 * frame-history ring after the blend census, warns once when the scene store refused tris, latches the FOV
 * tangent, bumps the scene epoch, clears the replay flag and resets the
 * per-instance material slots. A scene an earlier tick's render left
 * unpresented at the same stamp is closed without the push.
 * @param frame The VBL counter openpete_frame, or the stamp of the live
 *        scene for a late feeder. */
void openpete_native_scene_begin_frame(unsigned frame) OP_REQUIRES(sim);
/** @brief Return the VBL stamp of the scene currently accumulating.
 *
 * A late feeder appends only to a live scene: begin_frame with a newer
 * stamp would push and wipe a scene that a load stall holds on purpose.
 * @return The stamp, or 0xFFFFFFFF before the first begin_frame. */
unsigned openpete_native_scene_frame(void) OP_REQUIRES_SHARED(sim);
/** @brief Return the accumulating scene's epoch, bumped by every scene wipe (openpete_native_scene_begin, a new frame's begin_frame).
 *
 * Two feeders that read the same epoch fed the same scene.
 * @return The epoch. */
unsigned openpete_native_scene_epoch(void) OP_REQUIRES_SHARED(sim);
/** @brief Return the VBL stamp of the scene the game's own render last opened or fed.
 *
 * A sub-tick re-extract opens a scene per VBL under its own stamp; this stamp
 * moves only on a begin_frame call made outside the sub-tick view, so it
 * names the scene of the last tick the game rendered.
 * @return The stamp, or 0xFFFFFFFF before the game's first feed. */
unsigned openpete_native_scene_game_frame(void) OP_REQUIRES_SHARED(sim);

/** @brief Register the callback that fires immediately before a frame's accumulation is snapshotted.
 *
 * The callback runs just before the scene is snapshotted and the next frame
 * started, the last moment an append still reaches that scene. It serves a
 * feeder whose data is not final when its primitives are produced: the 2D UI
 * layer, whose primitive words are final only when the ordering table
 * holding them is handed to the GPU, a tick after the frame that built it.
 * The batch belongs to the scene its build frame opened, beside the 3D of
 * the same ordering table, so the feeder releases what it holds for `frame`
 * here; the ordering table's own walk releases the rest. Runs on the thread
 * doing the rotation, which owns the accumulating scene. One callback; a
 * later call replaces it.
 * @param fn Callback taking the closing frame's VBL stamp, or NULL. */
void openpete_native_scene_register_frame_close(void (*fn)(unsigned frame) OP_SLOT(sim)) OP_REQUIRES(sim);

/** @brief Register the callback that fires when the game's render opens a scene while its previous scene never reached a canonical present.
 *
 * The canonical present shows the newest scene the game rendered, so what a
 * superseded render recorded for the sub-tick re-extract (drawn render
 * paths, captured producer state) describes an image no present showed.
 * The callback runs on the game's render before the new scene opens: from
 * openpete_native_scene_note_game_capture at the render's first producer
 * capture, else from openpete_native_scene_begin_frame, once per render. A
 * scene counts as presented from the canonical capture that takes it
 * (openpete_native_scene_capture_overlay);
 * re-extract rebuilds in between do not present it. One callback; a later
 * call replaces it.
 * @param fn Callback taking the VBL stamp of the scene being opened, or NULL. */
void openpete_native_scene_register_superseded(void (*fn)(unsigned frame));

/** @brief Test whether a record a game render stamped belongs to the render that supersedes the pending scene.
 *
 * A render never spans a VBL boundary or a loop-anchor entry, so a record of
 * a later VBL than @p frame, or of that VBL at the loop-anchor count @p tick,
 * is that render's; any other belongs to a render it superseded. Every
 * supersede (the native scene's per-tick 2D signals, the render-path
 * intervals and marks) drops records by this test.
 * @param vbl The record's VBL (openpete_frame when it was stamped).
 * @param rec_tick openpete_loop_anchor_ticks() when it was stamped.
 * @param frame The VBL stamp of the superseding render's scene.
 * @param tick openpete_loop_anchor_ticks() of the superseding render.
 * @return Nonzero when the record belongs to the superseding render. */
int openpete_native_render_stamp_current(unsigned vbl, uint32_t rec_tick, unsigned frame, uint32_t tick);

/** @brief Supersede the pending game scene when the game's render at a newer VBL captures producer state before its first feed opens its scene.
 *
 * A producer hook may stamp a capture with the canonical-render epoch before
 * its render's first openpete_native_scene_begin_frame. When the previous
 * game scene never reached a canonical present, the superseded callback
 * then runs here, so the epoch moves before the capture is stamped and the
 * capture belongs to the render of its VBL, as a render path's interval
 * does. A no-op off the game fiber, inside the sub-tick view, with no
 * pending game scene, or at the pending scene's own VBL while its render
 * has not submitted its OT or the loop anchor has not been entered since.
 * @param frame The VBL the capture runs on (openpete_frame). */
void openpete_native_scene_note_game_capture(unsigned frame) OP_REQUIRES(sim);

/** @brief Set the ordering-table traversal rank stamped on the triangles added next.
 *
 * Sticky, cleared at each frame boundary; a feeder that never sets it leaves
 * 0. No feeder calls it: openpete_native_scene_rank_2d overwrites the rank of
 * every triangle that carries a primitive address.
 * @param seq The rank. */
void openpete_native_scene_set_seq(uint32_t seq) OP_REQUIRES(sim);

/** @brief Set the PSX address of the primitive the triangles added next were decoded from.
 *
 * Sticky, cleared at each frame boundary. openpete_native_scene_rank_2d
 * ranks triangles by it.
 * @param prim PSX address, or 0 for none. */
void openpete_native_scene_set_prim(uint32_t prim) OP_REQUIRES(sim);

/** @brief Set the framebuffer cell key stamped on the triangles added next.
 *
 * Sticky, cleared at each frame boundary. A 2D feeder sets it on a
 * triangle that samples a draw buffer's texels through the VRAM path,
 * keyed by the primitive's texture page and texel coordinates as the game
 * wrote them, or on an untextured border the copy's emitter draws around
 * those cells, keyed by its emission order within the emitter, and resets
 * it to 0 after. The 2D carry pairs such a triangle with the previous
 * tick's holder of the same key, so a copy whose screen box scales between
 * ticks interpolates its corners, and its borders with it.
 * @param cell Key, or 0 for none. */
void openpete_native_scene_set_fb_cell(uint32_t cell) OP_REQUIRES(sim);

/* Texel STP selection of a textured scene triangle: which texels of its
 * texture the fragment stage keeps (psx_scene.frag STP_SELECT). The GPU
 * applies a semi-transparent primitive's ABR equation only to texels whose
 * bit 15 (STP) is set and draws its STP-clear texels opaque. ABR 0 reproduces
 * that through the texel alpha; the ABR 1-3 equations read no alpha, so a 2D
 * feeder emits such a primitive as two triangle sets: its STP-clear texels
 * through the opaque pipe, then its STP-set texels through the ABR's pipe. */
enum {
    OPENPETE_STP_ALL = 0,      /* every non-zero texel (the default) */
    OPENPETE_STP_CLEAR_ONLY,   /* texels with bit 15 clear */
    OPENPETE_STP_SET_ONLY,     /* texels with bit 15 set */
    OPENPETE_STP_SELS
};

/** @brief Set the STP texel selection stamped on the triangles added next.
 *
 * Sticky, like openpete_native_scene_set_prim: every triangle added after the
 * call carries `sel` until the next call. A value outside the selection range
 * selects OPENPETE_STP_ALL. A feeder that sets it resets it to
 * OPENPETE_STP_ALL after its triangles.
 * @param sel An OPENPETE_STP_* value. */
void openpete_native_scene_set_stp_select(int sel);

/** @brief Stamp every 2D triangle of this frame with its ordering-table traversal rank.
 *
 * The rank is looked up by the primitive address each triangle was decoded
 * from. Covers the live scene and the feed-time carry accumulation, which
 * the next capture commits, so a triangle fed at emission and one fed from a
 * deferred batch are ranked by the same walk and every 2D emitter keeps
 * feeding where it does. A triangle without a primitive address keeps its
 * rank. The caller's lookup returns 0 for an address the walk never reached,
 * which keeps that triangle's feed position under the stable sort. No-op
 * when @p rank_of is NULL.
 * @param rank_of Maps a primitive address to its traversal rank. */
void openpete_native_scene_rank_2d(uint32_t (*rank_of)(uint32_t prim)) OP_REQUIRES_SHARED(sim);

/** @brief Record the guest address of the fade POLY_F4 the game linked into this frame's ordering table.
 *
 * The fade itself is set from the emitter's arguments
 * (openpete_native_scene_set_fade); the address lets the feeder rank the
 * fade among the frame's 2D. Cleared at each game-feed frame boundary.
 * @param prim The fade quad's guest address; 0 = none. */
void openpete_native_scene_set_fade_prim(uint32_t prim) OP_REQUIRES(sim);

/** @brief Record the ordering-table slot this tick's fade quad was linked into on a slotted table.
 *
 * Stored as slot + 1 in bits 26-30 of the fade word the emitter set this
 * tick (openpete_native_scene_set_fade), so it travels with the fade through
 * the sub-tick lerp and every scene snapshot; a slot above 30, or no fade
 * set, records nothing. The stitch draws higher slots first, so a face the
 * game links into a lower slot draws after the fade: the compose draws the
 * HUD3D layer after the fade when every slotted HUD3D face is below it.
 * Renderer state only.
 * @param slot The slot index (the slotted insert's a1). */
void openpete_native_scene_set_fade_slot(int slot) OP_REQUIRES(sim);

/** @brief Return the guest address of this frame's fade POLY_F4, as openpete_native_scene_set_fade_prim recorded it.
 *
 * @return The address, or 0 when the frame linked no fade quad. */
uint32_t openpete_native_scene_fade_prim(void) OP_REQUIRES_SHARED(sim);

/** @brief Mark every 2D triangle of this frame that ranks after the fade quad as drawn over the fade.
 *
 * A triangle with a primitive address whose rank (scene_tri_t::seq, set by
 * openpete_native_scene_rank_2d) exceeds @p fade_rank is marked, every other
 * one with a primitive address is cleared; the compose draws the marked ones
 * after the fade pass. Covers the live scene and the carry accumulation.
 * @param fade_rank The fade quad's rank in the same order; 0 (unranked or no
 *        fade) clears every mark. */
void openpete_native_scene_mark_over_fade(uint32_t fade_rank) OP_REQUIRES_SHARED(sim);

/** @brief Mark the current scene's flat 2D triangles the game draws before the player.
 *
 * Each HUD or HUD_PANEL triangle with a primitive address is marked when
 * @p under answers 1 for that address and cleared otherwise; the compose
 * draws the marked ones before the PLAYER group, the rest over the player.
 * OVERLAY triangles are left unmarked. Covers the live scene and the
 * feed-time carry accumulation, which the next capture commits as the 2D carry
 * the sub-tick re-extract re-applies, so both presents draw one order.
 * Renderer state only.
 * @param under The predicate on a primitive address; NULL clears every mark.
 * @param player_slot The player's ordering-table slot, kept for the
 *        OPENPETE_UI_UNDER_CENSUS line.
 * @return The count of triangles marked. */
int openpete_native_scene_mark_under_player(int (*under)(uint32_t prim), int player_slot) OP_REQUIRES_SHARED(sim);

/** @brief Collect the guest primitive addresses the current scene's 2D was decoded from.
 *
 * A feeder uses them to tell which of the VBL's ordering-table walks is the
 * 2D one. Collects every nonzero prim of the live scene, then of the carry
 * accumulation, in that order.
 * @param out Receives the addresses.
 * @param max Capacity of @p out.
 * @return The number written; 0 when @p out is NULL or @p max is not positive. */
int openpete_native_scene_collect_2d_prims(uint32_t* out, int max) OP_REQUIRES_SHARED(sim);

/** @brief Move the live scene's 2D triangles of primitives a predicate names out of the scene, to re-enter a later one.
 *
 * Each triangle with a nonzero primitive address that @p later accepts
 * leaves the live scene and the carry accumulation and is kept until
 * openpete_native_scene_undefer_2d puts it back: at the next scene the
 * game's render opens, or when the feeder calls it at the table's stitch.
 * A triangle that cannot be kept (allocation failure) stays in the scene.
 * @param later Returns nonzero for a primitive whose triangles are held back.
 * @return The number of triangles held back; 0 when @p later is NULL. */
int openpete_native_scene_defer_2d(int (*later)(uint32_t prim)) OP_REQUIRES(sim);

/** @brief Append every held-back 2D triangle to the live scene and the carry accumulation, in the order they were held back.
 *
 * No-op when none is held. */
void openpete_native_scene_undefer_2d(void) OP_REQUIRES(sim);
/* Append one screen-space triangle to the current native scene. `category`
 * (OPENPETE_SCENE_WORLD/_ACTOR/_HUD/...) and `carry` are passed EXPLICITLY per
 * call - there is NO ambient/global scene category (the old set_category /
 * set_carry globals are gone), so a feeder can't leak a stale layer into the
 * next feeder and EVERY emit path (incl. the sub-tick re-extract) is
 * category-correct by construction. `category` is ignored for non-opaque tris
 * (blend!=0 routes by blend). `carry` = 1 marks sub-tick-uncarryable 2D-UI (the
 * title logo / "PRESS START" / HUD glyph FT4 sprites + UI separator lines the
 * re-extract can't re-derive) so the overlay-carry snapshot preserves them
 * across the sub-tick rebuild instead of strobing at 30 Hz; 0 otherwise. */
/** @brief Set the CLUT-ladder fog row stamped on the triangles added next.
 *
 * Spyro's terrain fogs by selecting a pre-baked, progressively greyer palette
 * row (clut_y + delta) rather than blending at raster time, so identical
 * texels reach the screen through up to 16 palettes depending on distance.
 * Declaring the row lets a consumer recover the base palette, the texture's
 * stable identity: the replacement registry resolves against that base so
 * one pack file covers a texture at every fog distance, and re-applies the
 * ladder's colour transform at draw time. It does not affect the material
 * batch key (clut_y carries the distinction) or how unreplaced terrain
 * rasterizes. The row is bits 7:0; the TP_FOG_DELTA_CONT and
 * TP_FOG_DELTA_FOGBIAS flags above it are kept. A row above 15 zeroes the
 * whole value, flags included.
 *
 * Ambient state rather than a field of the feeder-facing triangle, because a
 * dozen feeders fill that triangle field by field without clearing it. Set it
 * per emit and reset it to 0 when leaving a laddered emit loop, so the value
 * cannot leak into another feeder's triangles.
 * @param delta Ladder row with optional flags; 0 is the unfogged base row. */
void openpete_native_scene_set_fog_delta(int delta);

/** @brief Set the HQ ladder-fade words stamped on subsequently added triangles.
 *
 * A terrain face the HQ pin draws past the game's near band carries the
 * affine of its far CLUT's deepest ladder row (core/rhi/shaders/psx_hq_fog.glsl
 * documents the packing); the textured scene stages fade each texel by it at
 * the fragment's ladder position. Triangles with different words never share
 * a draw run. Ambient like openpete_native_scene_set_fog_delta: reset to
 * (0, 0) when leaving the emit loop.
 * @param a Word a; bit 31 clear disables the fade.
 * @param b Word b. */
void openpete_native_scene_set_hq_fog(uint32_t a, uint32_t b);
/* Word a's enable bit (openpete_native_scene_set_hq_fog, psx_hq_fog.glsl). */
#define OPENPETE_HQFOG_ENABLE (1u << 31)
/* Word b's neutral bit (psx_hq_fog.glsl): the texel's colour is replaced by
 * the modulation-neutral 128 before the vertex colour modulates it, so the
 * triangle draws its vertex colour where the texel is not transparent. */
#define OPENPETE_HQFOG_NEUTRAL (1u << 31)
/* Word b bit 30 (psx_hq_fog.glsl). With word a's enable bit set the affine
 * is pre-weighted, t = 1: the face draws texel * aN + bN at every pixel (the
 * per-face ladder step). With it clear the stage picks the CLUT row at each
 * pixel's depth on the published ramp (the per-pixel ladder), word a bit 30
 * carrying the fog-reference bias. */
#define OPENPETE_HQFOG_B30 (1u << 30)

/** @brief Set the vertex fade target word stamped on subsequently added triangles.
 *
 * An emitter that blends a triangle's vertex colours toward a fog target on
 * a depth curve states the target here, so the stage of a mod material on
 * the triangle applies the same fade to the material's result
 * (include/openpete_psx_material.glsl). The stock stages never read it.
 * Ambient like openpete_native_scene_set_hq_fog: reset to 0 when leaving the
 * emit loop.
 * @param w OPENPETE_FOG_TARGET_ON | the target 0x00BBGGRR, or 0 for no fade. */
void openpete_native_scene_set_fog_target(uint32_t w);
/* The fade bit of a vertex fade target word (openpete_native_scene_set_fog_target). */
#define OPENPETE_FOG_TARGET_ON (1u << 31)

/* ── Portal cover invariant (plane-cover rule) ────────────────────────
 * The portal feeder's opening planes, republished per feed in core terms so
 * the OPENPETE_PORTAL_COVER_CHECK sweep can verify the plane-cover rule
 * against the COMPLETE scene (terrain AND actors): every pixel inside an
 * opening's footprint must resolve to the preview or to content nearer than
 * plane − margin. 1/view-Z of the plane at screen (x,y) = a·x + b·y + c. */
typedef struct {
    double a, b, c;            /* screen-space inv-vz field of the plane */
    float  px[16], py[16];     /* projected opening polygon (walk coords) */
    int    pn;
} openpete_native_cover_plane;
/** @brief Replace the portal opening planes of the current scene.
 *
 * Called once per terrain feed. The OPENPETE_PORTAL_COVER_CHECK sweep reads
 * up to 8 planes. The first 6 also ride the scene to the portal fragment
 * stages (portal_cover_scene.glsl): with @p blend_cover the blended-terrain
 * stages discard a fragment inside an outline at or behind its plane minus
 * @p margin_world, and the inner-rim stages discard a rim fragment inside
 * the outline of its accepting plane (openpete_native_scene_set_portal_rim).
 * A plane's slot is its index in @p planes.
 * @param planes The opening planes; read only when @p n > 0.
 * @param n Plane count; 0 clears.
 * @param margin_world The plane-cover near margin M in world units.
 * @param blend_cover Nonzero to apply the planes to blended terrain. */
void openpete_native_set_cover_planes(const openpete_native_cover_plane *planes,
                                      int n, float margin_world, int blend_cover);

/* ---- sunk-pickup depth snap (psx_moby_snap.glsl) ---------------------- */

/* A contact plane of a pulled moby (openpete_snap_plane,
 * openpete_moby_snap_rule.h) lies in the snap's camera space: the PSX screen
 * point (sx, sy) at view-Z z unprojected as ((sx - ofx) z / H,
 * (sy - ofy) z / H, z) with the view of openpete_native_snap_view. */

/** @brief Whether the sunk-pickup depth snap is on (OPENPETE_MOBY_SNAP, default on; read once and latched).
 * @return 0 when OPENPETE_MOBY_SNAP starts with '0', else 1. */
int openpete_native_moby_snap_on(void);
/** @brief Set the unprojection the current scene's contact planes are expressed in.
 * @param H The projection distance in PSX screen pixels.
 * @param ofx The screen X of the optical axis.
 * @param ofy The screen Y of the optical axis. */
void openpete_native_snap_view(float H, float ofx, float ofy) OP_REQUIRES(sim);
/** @brief Append one pulled moby's contact planes to the current scene's snap table.
 *
 * The ACTOR snap stages find the entry by @p mid (unranked faces) or
 * @p mordinal (ranked faces). Planes past the pool's cap and entries past the
 * entry cap are refused and counted (openpete_native_snap_census).
 * @param mid The moby identity (openpete_scene_tri.mid), nonzero.
 * @param mordinal The moby's present-local ordinal, 0 when no face is ranked.
 * @param off_m The world units a fragment's raster view-Z lies nearer than
 *        the key depth its contact geometry was built at (the tie pull less
 *        the near-floor lift).
 * @param pl The planes.
 * @param n Plane count; 0 adds nothing.
 * @return 1 when added, 0 when refused at a cap or with nothing to add. */
int openpete_native_snap_add(uint32_t mid, int mordinal, float off_m,
                             const openpete_snap_plane *pl, int n);
/** @brief Read the current scene's snap table counts.
 * @param ne Receives the entry count.
 * @param np Receives the plane count.
 * @param ovf Receives the entries plus planes refused at the caps. */
void openpete_native_snap_census(int *ne, int *np, unsigned *ovf) OP_REQUIRES_SHARED(sim);
/* The current scene's snap table counts and version at one instant
 * (openpete_native_snap_mark), for an instrument that adds, compares and
 * takes back a run of entries. */
typedef struct {
    int ne, np;
    float S;
    uint32_t ovf_ent, ovf_plane, ver, ver_next;
} openpete_native_snap_mark_t;
/** @brief Record the current scene's snap table counts, largest slab, refusal counts and version.
 * @param m Receives them. */
void openpete_native_snap_mark(openpete_native_snap_mark_t *m);
/** @brief Return the current scene's snap table to a mark: the entries and planes added since are dropped and the counters and version restored.
 * @param m The mark. */
void openpete_native_snap_rewind(const openpete_native_snap_mark_t *m);
/** @brief A digest (FNV-1a 64) of what the current scene's snap table added since a mark: its counts, refusal counts, version steps, largest slab, and every field of the entries and planes past the mark.
 * @param m The mark.
 * @return The digest. */
uint64_t openpete_native_snap_digest(const openpete_native_snap_mark_t *m);
/** @brief Pack the current scene's snap table into the snap stage's word layout (psx_moby_snap.glsl) with a zero fragment affine: the words an upload of it would carry.
 * @param w Receives the words.
 * @param cap Room in @p w, in words.
 * @return The words written, or -1 when they do not fit. */
int openpete_native_snap_pack_live(uint32_t *w, int cap);

/** @brief Set the sticky portal inner-rim mark stamped on the triangles added after it.
 *
 * A marked opaque terrain triangle is a face the terrain feed accepted as a
 * portal's inner rim under a plane that discards its rim per fragment: it
 * draws through the inner-rim twins of the WORLD and WORLD_FILL pipelines,
 * whose fragment stage discards it inside the opening outline of that plane
 * alone (openpete_native_set_cover_planes), so the plane's preview paints
 * there and the face stays drawn elsewhere, including inside the outlines of
 * other openings. Reset it to 0 when leaving the feed.
 * @param plane 1 + the accepting plane's published slot (1 to 6); 0, or a
 *        value out of range, clears the mark. */
void openpete_native_scene_set_portal_rim(int plane);

/** @brief Append one screen-space triangle to the current scene.
 *
 * Equivalent to openpete_native_scene_add_tri_grouped with group 0.
 * @param tri Triangle to copy; NULL is ignored.
 * @param category Scene layer (OPENPETE_SCENE_*); ignored for blended tris.
 * @param carry 1 marks 2D UI the sub-tick re-extract cannot re-derive; else
 *        0. */
void openpete_native_scene_add_tri(const openpete_scene_tri *tri, int category, int carry) OP_REQUIRES(sim);
/** @brief Append one screen-space triangle to the current scene with a grouping id.
 *
 * openpete_native_scene_add_tri stores group 0. The portal feeder passes a
 * group to tie each portal's PORTAL_MASK footprint to its PORTAL_CYCLORAMA
 * colours, so the renderer stencil-isolates portals per group. The group is
 * an argument, not ambient state, like the category and carry. The stored
 * tri also takes the sticky tag, fog delta, seq, prim and STP selection.
 * A blend class outside 0..4 is stored as 0 (opaque). An untextured tri of
 * blend class 1 (PSX ABR 0) is stored with vertex alpha 0x80 whatever the
 * feeder stated, which halves it to (B+F)/2.
 * @param tri Triangle to copy; NULL is ignored.
 * @param category Scene layer (OPENPETE_SCENE_*); ignored for blended tris.
 * @param carry 1 marks 2D UI the sub-tick re-extract cannot re-derive, so
 *        the overlay carry preserves it; else 0.
 * @param group Portal grouping id. */
void openpete_native_scene_add_tri_grouped(const openpete_scene_tri *tri, int category,
                                           int carry, int group) OP_REQUIRES(sim);
/** @brief Make room for n triangles after the current scene's last and return the first one's slot, for a feeder that stores them in parallel.
 *
 * Latches the environment instruments the store reads, so the stores that
 * follow read no lazily initialised state. The scene count does not move
 * until openpete_native_scene_commit_stored.
 * @param n The triangle count.
 * @return The first slot, or -1 when the scene cannot grow to hold them. */
int openpete_native_scene_reserve(int n) OP_REQUIRES_SHARED(sim);
/** @brief Store one triangle at a slot openpete_native_scene_reserve handed out, as openpete_native_scene_add_tri_grouped stores it with carry and group 0.
 *
 * The tag, heavy-fog words (texel fade a and b, then the vertex fade target
 * word) and host-added mark are arguments instead of the sticky setters; every other sticky field is read as the last setter left
 * it. Counts nothing and logs nothing: a vertex without a true view-Z is
 * repaired and added to *pvz_missing for the commit. Safe on a work-pool
 * worker for distinct slots while no setter runs. The category must not be
 * OPENPETE_SCENE_OVERLAY or OPENPETE_SCENE_HUD_PANEL, which feed the 2D carry.
 * @param slot The slot.
 * @param tri The triangle.
 * @param category Scene layer.
 * @param tag Provenance tag.
 * @param hqfog The three heavy-fog words.
 * @param host_added Nonzero for host-added geometry.
 * @param pvz_missing Incremented per repaired vertex. */
void openpete_native_scene_store_tri(int slot, const openpete_scene_tri* tri, int category, uint32_t tag,
                                     const uint32_t hqfog[3], int host_added, unsigned* pvz_missing) OP_REQUIRES_SHARED(sim);
/** @brief The size of one stored scene triangle, of which openpete_scene_tri is the prefix: the span an instrument compares per triangle.
 * @return The size in bytes. */
size_t openpete_native_scene_tri_bytes(void);
/** @brief The stored triangle at a reserved slot, as its feeder-layout prefix, for a commit that patches ordering fields.
 * @param slot The slot.
 * @return The triangle. */
openpete_scene_tri* openpete_native_scene_stored(int slot);
/** @brief Add n triangles of one provenance tag to the scene's per-family, per-kind counts, as n adds would.
 * @param tag The tag.
 * @param n The count. */
void openpete_native_scene_count_kind(uint32_t tag, int n) OP_REQUIRES(sim);
/** @brief Take n stored triangles after the scene's last into the scene and count the repaired vertices.
 * @param n The triangle count, the reservation's or fewer.
 * @param pvz_missing Vertices the stores repaired.
 * @param category The category the repair line names. */
void openpete_native_scene_commit_stored(int n, unsigned pvz_missing, int category) OP_REQUIRES(sim);
/** @brief Return the number of scene vertices that reached add_tri without a true view-Z since boot.
 *
 * Each such vertex (pvz <= 0) took its ordering view-Z instead.
 * @return The count; 0 when every feeder states pvz. */
unsigned openpete_native_scene_pvz_missing(void);

/** @brief Mark the triangles added next as host-added geometry.
 *
 * Sticky per-triangle origin, the same ambient-state pattern as the tag; the
 * feeder sets it back to 0 when leaving its emit loop. 1 marks a triangle the
 * host added beyond what the game's own render produced (the extended-LOD
 * moby reveal). The verdicts that a frame has content and that it is a
 * gameplay world frame read the game's geometry alone, so host-added geometry
 * never turns a frame the game drew nothing on into a rendered scene.
 * @param on Non-zero to mark. */
void openpete_native_scene_set_host_added(int on);

/** @brief Set the drain sub-cell keys stamped on the world triangles added next.
 *
 * Sticky, the set_host_added pattern. Used for a triangle whose face the
 * game's near drain inserts into the ordering table as sub-cells finer than
 * the triangle. The triangle's koff is the least sub-cell key, and each
 * fragment keys and contests at its own sub-cell's key
 * (psx_key_contest.glsl op_drain_delta). NULL in either argument clears the
 * stamp.
 * @param fl Each vertex's face-local coordinate (x0, y0, x1, y1, x2, y2).
 * @param kc The face's key table. */
void openpete_native_scene_set_key_cells(const float fl[6], const uint32_t kc[8]);

/** @brief Set the fold data stamped on the world triangles added next.
 *
 * Sticky like set_key_cells, which clears it. Used for a triangle whose
 * 2x2 quadrant folds (its drawn and skipped drain cells overlap on
 * screen): a fragment whose isoline slot is skipped is keyed by the drain
 * cell polygons over the face's corners instead (psx_key_contest.glsl
 * op_drain_fold_delta). NULL clears it.
 * @param kn The face's corners TL, TR, BL, BR as (OFX z + H x, OFY z + H y,
 *        z) of their camera-space positions. */
void openpete_native_scene_set_key_fold(const float kn[12]);

/** @brief Mark the start of the terrain feed in the scene being accumulated.
 *
 * Every triangle added after the call, until the scene resets, is stamped as
 * emitted at or after the feed. The retained terrain cache draws geometry
 * the feed does not emit; the mark places the feed in the scene's emission
 * order when the feed emits no triangle of its own. */
void openpete_native_scene_mark_terrain_feed(void) OP_REQUIRES(sim);

/* ── Provenance tag ──────────────────────────────────────────────────────
 * Sticky per-tri provenance stamped by add_tri until changed (same
 * ambient-state pattern as the terrain feeder's s_emit_* context; set
 * 0 when leaving a tagged emit loop). Carries what the material key
 * LACKS - the batcher already splits on tpage/clut, so texture-level
 * selectors ride the material key; the tag adds source identity:
 *
 *   bits 31:28  path (0 = untagged, 1 = moby, 2 = terrain)
 *   moby:       bits 15:0  m_Class
 *   terrain:    bit 27 env-anim member (func_8002A6FC rewrite set),
 *               bit 26 LP tier, bit 25 untextured, bits 8:0 sector id;
 *               textured faces add the Tiledef/texture-record id in
 *               [15:9] (word2 & 0x7F - the texture-selector key); the
 *               face class's resolved material for the present in
 *               [24:16] (OPENPETE_MATERIAL_TERRAIN_* in openpete_mods.h)
 *
 * INERT BY DEFAULT: the tag joins the material sort/coalesce key ONLY
 * when a split consumer is armed (a registered material selector or
 * the OPENPETE_TAG_SPLIT=1 dev flag), so the ~70-draw batching is
 * byte-identical without mods. OPENPETE_TAG_TINT=1 tints vertex colours
 * by tag hash (visual coverage instrument, no split needed). */
#define OPENPETE_TAG_MOBY(cls) \
    (0x10000000u | ((uint32_t)(cls) & 0xFFFFu))
/* moby + the material walk's outcome in the spare bits [27:16]
 * (OPENPETE_MATERIAL_SLOT_* in openpete_mods.h); slot 0 == OPENPETE_TAG_MOBY
 * (no walk ran: the draw-time declarative resolve applies). */
#define OPENPETE_TAG_MOBY_SLOT(cls, slot) \
    (0x10000000u | (((uint32_t)(slot) & 0xFFFu) << 16) | ((uint32_t)(cls) & 0xFFFFu))
#define OPENPETE_TAG_TERRAIN(lp, env, sec) \
    (0x20000000u | ((env) ? 0x08000000u : 0u) | ((lp) ? 0x04000000u : 0u) | \
     ((uint32_t)(sec) & 0x1FFu))
/* Textured terrain face: [15:9] = the face's Tiledef/texture-record id
 * (word2 & 0x7F - the id texture selectors key on), [24:16] = the face
 * class's resolved material for the present (0 = draw-time declarative
 * resolve, 0x100 | index = a declarative material, 0x1FF = stock, else an
 * interned refine slot). */
#define OPENPETE_TAG_TERRAIN_TEX(lp, env, sec, tex, slot) \
    (OPENPETE_TAG_TERRAIN(lp, env, sec) | \
     (((uint32_t)(slot) & 0x1FFu) << 16) | (((uint32_t)(tex) & 0x7Fu) << 9))
/* Untextured terrain band: bit 25 marks the flat/gouraud tris, and
 * [24:16] carries the face class's resolved material, encoded as on a
 * textured face. */
#define OPENPETE_TAG_TERRAIN_UNTEX(lp, env, sec, slot) \
    (OPENPETE_TAG_TERRAIN(lp, env, sec) | 0x02000000u | \
     (((uint32_t)(slot) & 0x1FFu) << 16))
/* Textured terrain face with no Tiledef id (an imported sector's borrowed
 * face): [15:9] stays 0, and [24:16] carries the face class's resolved
 * material, encoded as on a textured face. */
#define OPENPETE_TAG_TERRAIN_NOTEX_ID(lp, env, sec, slot) \
    (OPENPETE_TAG_TERRAIN(lp, env, sec) | (((uint32_t)(slot) & 0x1FFu) << 16))

/* ── The remaining feeder paths ───────────────────────────────────────
 * Every path below copies the MOBY layout exactly - [31:28] path,
 * [27:16] material slot (12 bits), [15:0] source identity (16 bits) -
 * which fills the word with no spare bits. Copy it for any new feeder
 * rather than inventing a payload: one shared shape keeps the decode
 * and the slot-width reasoning uniform across channels.
 *
 * The identity half is whatever the feeder actually HAS at emit time,
 * which varies sharply by path. Where a feeder has none, the field is
 * 0 and the tag identifies the channel only - that is a real limit,
 * not a placeholder to fill in later: a per-element selector cannot be
 * built on a feeder that never knew which element it was drawing.
 * `docs/src/developer/render-channel-census.md` records the per-feeder evidence. */
#define OPENPETE_TAG_PATH(p, ident, slot) \
    (((uint32_t)(p) & 0xFu) << 28 | (((uint32_t)(slot) & 0xFFFu) << 16) | \
     ((uint32_t)(ident) & 0xFFFFu))

/* Player (Spyro) - drawn by the player draw routine, not the moby pool
 * walk, so no moby-class selector reaches him. Identity names the draw
 * kind: the avatar, the mirrored reflection copy (its own call of the
 * routine), or the half below the water plane that the plane split
 * depth-cues. The emitted tri carries no sub-model marker (the walk keeps
 * per-vertex indices, not a segment id), so head, body and wings share
 * one identity. */
#define OPENPETE_TAG_PLAYER_AVATAR    0u
#define OPENPETE_TAG_PLAYER_MIRROR    1u
#define OPENPETE_TAG_PLAYER_SUBMERGED 2u
#define OPENPETE_TAG_PLAYER(kind, slot)   OPENPETE_TAG_PATH(3u, (kind), (slot))
/* Flame breath. Identity distinguishes the ADDITIVE ribbon from the
 * OPAQUE G3 caps - one feeder, two blend modes, so a material that
 * reshades the ribbon must be able to leave the caps alone. */
#define OPENPETE_TAG_FLAME_RIBBON  0u
#define OPENPETE_TAG_FLAME_CAPS    1u
#define OPENPETE_TAG_FLAME(part, slot)    OPENPETE_TAG_PATH(4u, (part), (slot))
/* Particles. The feeder covers six distinct effects and only two of them
 * carry the game's own `sel` class - glows, flares, sparkle lines and the
 * dragon star are not in that space at all. So identity is a pair:
 * [15:8] a feeder-local KIND naming the effect, [7:0] the game's `sel`
 * where one exists (0 otherwise).
 *
 * Both halves CLASSIFY without IDENTIFYING: the pool stores no slot or
 * pointer and emit order is pool-walk order. Enough for "all smoke
 * puffs", never enough for "this puff" - which is also why the channel
 * cannot be geometry-cached. */
#define OPENPETE_TAG_PART_QUAD    0u   /* FT4 billboards (sel-classified) */
#define OPENPETE_TAG_PART_LINE    1u   /* streak lines (sel-classified) */
#define OPENPETE_TAG_PART_GLOW    2u   /* glow rings */
#define OPENPETE_TAG_PART_FLARE   3u   /* sparkle flares */
#define OPENPETE_TAG_PART_SPARK   4u   /* sparkle lines */
#define OPENPETE_TAG_PART_STAR    5u   /* dragon-rescue star fan */
#define OPENPETE_TAG_PARTICLE(kind, sel, slot) \
    OPENPETE_TAG_PATH(5u, (((uint32_t)(kind) & 0xFFu) << 8) | \
                          ((uint32_t)(sel) & 0xFFu), (slot))
/* Sky / cyclorama dome. No per-tri identity exists (the feeder carries a
 * loop index only), so the identity field is 0 = the whole dome. */
#define OPENPETE_TAG_SKY(slot)            OPENPETE_TAG_PATH(6u, 0u, (slot))
/* Drop shadows. Identity is the caster KIND, not the caster: the moby
 * pointer is read while building but never reaches the tri. */
#define OPENPETE_TAG_SHADOW_MOBY   0u
#define OPENPETE_TAG_SHADOW_PLAYER 1u
#define OPENPETE_TAG_SHADOW(kind, slot)   OPENPETE_TAG_PATH(7u, (kind), (slot))
/* Tracer/beam streaks. Identity names the producer: the point-ring
 * ribbon (Spyro 1's beam among them), the indexed mesh, or Zoe's zap.
 * The routines drain their source arrays every tick, so nothing survives
 * to identify one streak. */
#define OPENPETE_TAG_TRACER_RIBBON 0u
#define OPENPETE_TAG_TRACER_MESH   1u
#define OPENPETE_TAG_TRACER_ZAP    2u
#define OPENPETE_TAG_TRACER(kind, slot)   OPENPETE_TAG_PATH(8u, (kind), (slot))
/* Portal destination-sky previews. Identity is the preview KIND, because
 * the game draws a portal three ways across the approach and they are
 * genuinely different art: a flat colour chip far out, a DPCS-fogged dome
 * at medium range, the full gouraud dome up close. Anything that restyles
 * one rung and not the others pops as the player walks up.
 *
 * The owning portal is NOT in the tag - it rides the per-tri GROUP id
 * (the destination skybox pointer), which the portal pass already uses. */
#define OPENPETE_TAG_PORTAL_FLAT     0u
#define OPENPETE_TAG_PORTAL_DOME     1u
#define OPENPETE_TAG_PORTAL_BLENDED  2u
#define OPENPETE_TAG_PORTAL(kind, slot)   OPENPETE_TAG_PATH(9u, (kind), (slot))

/* Kinds counted per tag family (openpete_tag_kind). */
#define OPENPETE_TAG_KINDS 8u
/** @brief The kind a tag counts under within its family, for the families whose kinds belong to different render paths.
 *
 * The particle family (5) takes its feeder-local kind (identity bits 15:8)
 * and the shadow family (7) its caster kind (identity bits 15:0); every
 * other family counts under kind 0. A kind past the last counted one folds
 * into it.
 * @param tag An OPENPETE_TAG_* value.
 * @return The kind, below OPENPETE_TAG_KINDS. */
static inline uint32_t openpete_tag_kind(uint32_t tag) {
    const uint32_t f = tag >> 28;
    const uint32_t k = f == 5u ? (tag >> 8) & 0xFFu : f == 7u ? tag & 0xFFFFu : 0u;
    return k < OPENPETE_TAG_KINDS ? k : OPENPETE_TAG_KINDS - 1u;
}

/** @brief Set the provenance tag stamped on every triangle added afterwards.
 *
 * The tag is sticky until the next call; 0 clears it. The top four bits
 * select the tag family counted in the per-family census.
 * @param tag An OPENPETE_TAG_* value, or 0. */
void openpete_native_scene_set_tag(uint32_t tag);
/** @brief Name a moby whose faces this scene draws under the terrain-order rule, for the OPENPETE_TORDER_CENSUS instrument.
 *
 * Called by the moby feeder for every moby the terrain-order entry selects
 * (game_layout.moby.terrain_order), whether or not the rule is applied, so
 * the census measures the same faces on both legs of an A/B. The list is
 * cleared when a scene begins. Inert unless OPENPETE_TORDER_CENSUS is set.
 * @param mid The moby identity (openpete_scene_tri.mid). */
void openpete_native_scene_note_torder(uint32_t mid);
/** @brief Copy the accepted-triangle count per tag family of the scene being accumulated.
 *
 * The family is tag bits 31:28; index 0 is untagged. The counts reset with
 * the scene.
 * @param out Receives 16 counts. */
void openpete_native_scene_tag_family_counts(uint32_t out[16]) OP_REQUIRES_SHARED(sim);
/** @brief Copy the accepted-triangle count per tag family and kind of the scene being accumulated.
 *
 * The family is tag bits 31:28; the kind is openpete_tag_kind. The counts
 * reset with the scene and sum per family to
 * openpete_native_scene_tag_family_counts.
 * @param out Receives 16 rows of OPENPETE_TAG_KINDS counts. */
void openpete_native_scene_tag_kind_counts(uint32_t out[16][OPENPETE_TAG_KINDS]);

/* The scene's count, repaired-vertex count and per-tag kind counts at one
 * instant (openpete_native_scene_mark), for an instrument that feeds one
 * stream twice and compares. */
typedef struct {
    int n;
    unsigned pvz_missing;
    uint32_t kind[16][OPENPETE_TAG_KINDS];
} openpete_native_scene_mark_t;
/** @brief Record the scene's triangle count, repaired-vertex count and per-tag kind counts.
 * @param m Receives them. */
void openpete_native_scene_mark(openpete_native_scene_mark_t* m) OP_REQUIRES_SHARED(sim);
/** @brief Return the scene to a mark: drop the triangles added since and restore the counts.
 *
 * The triangles' slots keep their contents until overwritten.
 * @param m The mark. */
void openpete_native_scene_rewind(const openpete_native_scene_mark_t* m) OP_REQUIRES(sim);
/** @brief Arm or disarm the provenance tag as a batch-split key.
 *
 * The mod material-selector registry arms it when a selector registers;
 * OPENPETE_TAG_SPLIT arms the split independently.
 * @param on Non-zero to arm. */
void openpete_native_set_tag_split(int on);
/** @brief Set the panel-0 background fill colour of the scene being accumulated.
 *
 * Drawn before the scene so uncovered areas show it instead of black,
 * mirroring the PSX cyclorama's frame clear to m_BackgroundColor. The
 * draw-env clear colour (openpete_native_set_clear_color) takes precedence
 * when set. Reset to 0 at each scene begin; held at 0 under the
 * face-identity diagnostic (openpete_tercache_faceid).
 * @param rgba Colour, byte order R, G, B, A; 0 for no fill. */
void openpete_native_scene_set_bg(uint32_t rgba) OP_REQUIRES(sim);
/** @brief Record the game's draw-env clear colour, the background the canonical render auto-clears to.
 *
 * Two producers: the PutDrawEnv thunk, and the game module's per-canonical
 * present sync from the DRAWENV struct in RAM (the cyclorama feeder). It is
 * the primary background source, preferred over
 * openpete_native_scene_set_bg, which is stale in states without a
 * cyclorama. Persists across sub-tick presents, since both producers run on
 * canonical frames only. OPENPETE_LOAD_CENSUS logs each change. Under the
 * face-identity diagnostic (openpete_tercache_faceid) a set colour is black.
 * @param isbg The PSX DRAWENV auto-clear flag; 0 unsets the colour and
 *             defers to the scene-fed background.
 * @param r Red, 0..255.
 * @param g Green, 0..255.
 * @param b Blue, 0..255. */
void openpete_native_set_clear_color(int isbg, int r, int g, int b) OP_REQUIRES_SHARED(sim);
/** @brief The draw-env clear latch (openpete_native_set_clear_color).
 *
 * @return The clear colour word, byte order R, G, B, A; 0 for no auto-clear. */
uint32_t openpete_native_clear_color(void);
/** @brief Make the scene being accumulated show a given draw-env clear instead of the latch, until the next scene begins.
 *
 * A held camera cut's scene shows tick N-1 while the latch already holds tick
 * N's clear. The latch itself is left alone.
 * @param clear The clear colour word (openpete_native_clear_color). */
void openpete_native_scene_hold_clear(uint32_t clear) OP_REQUIRES(sim);
/** @brief Record the DRAWENV the game's PutDrawEnv just applied.
 *
 * Called by the PutDrawEnv thunk; openpete_native_note_draw_otag reads it.
 * @param isbg The PSX DRAWENV auto-clear flag.
 * @param r Clear red, 0..255.
 * @param g Clear green, 0..255.
 * @param b Clear blue, 0..255. */
void openpete_native_note_put_draw_env(int isbg, int r, int g, int b) OP_REQUIRES_SHARED(sim);
/** @brief Record the draw area the scene being presented will be drawn into, for the next canonical capture.
 *
 * Called once per canonical present, before the capture, with the clip rect
 * and auto-clear flag of the DRAWENV g_CurDB points at: the env the
 * presented scene's DrawOTag applies after the VSync, which the PutDrawEnv
 * latch reaches only one VSync later. The capture reads it to predict the
 * subtractive fade sum that area holds once the scene's OT is drawn. A
 * nonpositive width or height records that no area is known.
 * @param clip_x Clip rect left, VRAM texels.
 * @param clip_y Clip rect top.
 * @param clip_w Clip rect width.
 * @param clip_h Clip rect height.
 * @param isbg The DRAWENV auto-clear flag. */
void openpete_native_note_present_draw_area(int clip_x, int clip_y, int clip_w, int clip_h, int isbg);
/** @brief Record the draw area a display rect shows as the area the scene being presented shows, uncleared, for the next canonical capture.
 *
 * Called instead of openpete_native_note_present_draw_area for a present
 * that draws no table and keeps the displayed image (a kept repeat,
 * cyclorama_native_repeat_resolve): the capture then reads the fade sum of
 * the area on screen. The area is the first recorded draw clip inside the
 * rect (openpete_native_shown_area_content); with none, no area is known.
 * @param disp_x Display rect left, VRAM texels.
 * @param disp_y Display rect top.
 * @param disp_w Display rect width.
 * @param disp_h Display rect height. */
void openpete_native_note_present_shown_area(int disp_x, int disp_y, int disp_w, int disp_h);
/** @brief Note a DrawOTag: when the game fed primitives since the last canonical capture, that content's env is the last PutDrawEnv's.
 *
 * Called by the DrawOTag thunk. A game that submits its OT before the VSync
 * the host presents at has drawn the presented scene with that env; a game
 * that submits after the VSync has fed no primitive of the next scene when
 * its DrawOTag runs, so nothing is recorded. The record holds until the next
 * canonical capture. The subtractive fade emitted since the previous
 * DrawOTag is added to the held fade sum of the retained draw area this
 * DrawOTag draws into. */
void openpete_native_note_draw_otag(void) OP_REQUIRES_SHARED(sim);
/** @brief Report the env of the DrawOTag that already submitted the scene being presented, if one did.
 *
 * @param env Receives isbg, r0, g0, b0 when the function returns 1.
 * @return 1 when a DrawOTag since the last canonical capture submitted fed
 *         primitives, else 0 (the scene's PutDrawEnv has not run yet). */
int openpete_native_drawn_env(uint8_t env[4]);
/** @brief Return whether the yield the game is parked at owes a canonical present, given whether one is pending already.
 *
 * A present is owed beyond a pending one only when the game fed a scene
 * since the last capture, and then when either the screen changes on this
 * VBL without a DrawOTag of that scene: the DISPENV moved onto an area
 * holding a drawn scene no present has shown (that area is then marked
 * presented), or the game draws the fed scene into the buffer on screen,
 * the DRAWENV clip it put since the last capture lying inside the DISPENV
 * rect (OPENPETE_FRONT_DRAW_PRESENT=0 disables this rule).
 * @param pending 1 when a present is pending already.
 * @return 1 when the yield takes a canonical present, else 0. */
int openpete_native_scene_yield_owes_present(int pending) OP_REQUIRES(sim);
/** @brief Return the id of the scene whose primitives the last DrawOTag submitted.
 *
 * A scene's id is the ordinal of the canonical capture that presents it
 * (openpete_native_scene_presenting). A DrawOTag that runs after the game fed
 * the open scene submits that scene; one that runs before any feed since the
 * last capture submits the scene that capture presented. 0 before the first
 * DrawOTag.
 * @return The scene id. */
unsigned openpete_native_otag_scene(void);
/** @brief Return the count of scenes the game's render has opened since boot.
 *
 * Counts each game feed that opens a new scene (a sub-tick re-extract opens
 * none). A scene's id is the ordinal of the capture that presents it, so a
 * render that opens a scene before the previous one reached a capture takes
 * that scene's id: a DrawOTag tagged with the id before such an open
 * submitted the superseded scene, not the one the capture presents.
 * @return The count. */
unsigned openpete_native_game_scene_opens(void) OP_REQUIRES_SHARED(sim);
/** @brief Return the id of the scene the next canonical capture presents.
 *
 * The ordinal of that capture, one more than the captures taken so far.
 * @return The scene id. */
unsigned openpete_native_scene_presenting(void);
/** @brief Set this frame's full-screen fade overlay (the game's func_800190D4).
 *
 * Each transition darkens or brightens the whole frame with one
 * semi-transparent full-screen POLY_F4 whose ABR mode is the direction: 2
 * (B-F) fades to black, 1 (B+F) to white. The native renderer draws a
 * full-panel quad last, over world, actors, HUD and 2D UI, in the matching
 * blend mode with the depth test passing everywhere; the HUD3D faces and the
 * 2D the game links after a slotted fade quad
 * (openpete_native_scene_set_fade_slot) draw after it. The fade clears at each
 * scene begin, so the game's per-tick call is what keeps it on; the
 * re-extract treats it as active only on a tick the game set it. Colour
 * components are clamped to 0..255. Under the retained freeze mode the
 * emission is also held for the next DrawOTag, which adds a subtractive one
 * to its draw area's held fade sum. Renderer state only.
 * @param mode PSX ABR mode (0 = (B+F)/2, 1 = add, 2 = subtract), or < 0 to clear.
 * @param r Red, 0..255.
 * @param g Green, 0..255.
 * @param b Blue, 0..255. */
void openpete_native_scene_set_fade(int mode, int r, int g, int b) OP_REQUIRES(sim);
/** @brief Record the cutscene letterbox the game's emitter drew this tick, by its bar height.
 *
 * The emitter draws two frame-width black bars, the top one over PSX rows
 * top_y .. top_y + h and the bottom one over bottom_y - h .. bottom_y. The
 * native scene draws them from this record instead of from the emitted
 * primitives: the canonical capture commits it as the current tick's
 * height, rolling the previous one, and every present draws the bars at the
 * height lerped between the two by its sub-tick alpha, so the bars slide
 * between ticks. A tick with no record has height 0. Renderer state only.
 * @param top_y PSX row of the top bar's outer edge.
 * @param bottom_y PSX row of the bottom bar's outer edge.
 * @param h Bar height in PSX rows; 0 or less draws no bars.
 * @param over_fade 1 when the game linked the bars after this tick's fade
 *        quad, so they draw over the fade; 0 draws them under it. */
void openpete_native_scene_set_letterbox(float top_y, float bottom_y, float h, int over_fade);
/** @brief Record an opaque screen quad the game drew that the scene holds no triangles for.
 *
 * The freeze backdrop's POLY_FT4s draw the frozen frame over the whole buffer
 * and are drawn natively as the backdrop rather than fed. A draw area that
 * keeps its content (auto-clear off) loses everything such a quad covers, so
 * the retained-area replay drops the earlier 2D entries under it. Consumed at
 * the next canonical capture.
 * @param x The quad's corner x in PSX screen pixels, PSX corner order (TL, TR, BL, BR).
 * @param y The corner y, same order. */
void openpete_native_scene_note_cover_quad(const float x[4], const float y[4]);
/** @brief Set this frame's freeze backdrop (the pause, inventory and flight-results overlay).
 *
 * The game freezes the rendered frame, desaturates it to grayscale and
 * re-displays it green-tinted (setRGB0(0x4C,0x80,0x40)) through a
 * full-screen POLY_FT4 that samples a VRAM scratch copy. The native renderer
 * instead draws its own last world frame desaturated and tinted, so the
 * backdrop shows the native render and needs no PsyCross VRAM. The feeder
 * detects the backdrop FT4, calls this with its tint, and does not feed the
 * FT4. The backdrop clears at each scene begin, so the game's per-tick call
 * keeps it on. Components and @p fade are clamped. Renderer state only.
 * @param r Tint red, 0..255, or < 0 to clear.
 * @param g Tint green, 0..255.
 * @param b Tint blue, 0..255.
 * @param fade Fade-to-black shift 0..127 (the GS_ExitLevel case-0 fade,
 *             where the game shrinks the grayscale CLUT each tick); 0 for the
 *             static backdrop. */
void openpete_native_scene_set_freeze_backdrop(int r, int g, int b, int fade) OP_REQUIRES(sim);

/** @brief Set the freeze-backdrop production mode, once, from the game descriptor.
 *
 * 0 (default): the game emits a full-screen quad that re-displays the frozen
 * frame through a grayscale CLUT with a flat tint, and the backdrop
 * reproduces that desaturation and tint. Two such emitters can hand over
 * with a one-tick gap, which the per-tick freeze carry bridges.
 *
 * 1: the game produces the frozen frame by writing DRAWENV.isbg = 0 and
 * drawing no world, so the framebuffer keeps the last rendered frame. The
 * backdrop then draws the snapshot unaltered, and the carry bridges no gap,
 * because the arming signal is a latched draw-environment word the game
 * clears in one write rather than a primitive it stops emitting.
 * @param retained Non-zero selects mode 1. */
void openpete_native_scene_set_freeze_retained(int retained) OP_REQUIRES(sim);

/** @brief Set whether the flat 2D HUD and text draw under the player this frame.
 *
 * 0 (default) draws them over the player. 1 is for game states where the
 * player flies in front of revealed text (the GS_LevelTransition flyby); the
 * game module's hud_under_player hook names them. The 3D HUD icons and the
 * letterbox and fade draw on top regardless. Clears at each scene begin; the
 * moby feeder sets it every 3D frame. Renderer state only.
 * @param under Non-zero draws the flat HUD under the player. */
void openpete_native_scene_set_hud_under_player(int under) OP_REQUIRES(sim);
/** @brief Set whether the actor group draws depth-isolated in front of the world this frame.
 *
 * When set, the actor group draws after the world and player with a depth
 * clear limited to the actors' screen bounds: actors still depth-sort among
 * their own faces but terrain never occludes them. The PSX's coarse
 * ordering-table buckets painted cutscene actors over walls they
 * interpenetrate (the rescued dragon's wing inside a wall), which per-pixel
 * depth would cut away. The moby feeder sets it when a cutscene owns the
 * draw, unless OPENPETE_ACTOR_FRONT=0. Clears at each scene begin. Renderer
 * state only.
 * @param front Non-zero draws the actors in front. */
void openpete_native_scene_set_actor_front(int front) OP_REQUIRES(sim);
/* True when the native display is live (so the game only does the extra
 * extraction work when it will actually be shown). */
/* (openpete_native_render_enabled, above, serves this gate.) */

/* ---- widescreen (Hor+) display policy -------------------------------- */
/** @brief The Hor+ widen factor: how much wider than 4:3 the native render rect is.
 *
 * When the render rect is wider than the 4:3 PSX display, the native
 * renderer keeps the vertical FOV and shows more world at the left and
 * right; the centre 4:3 region matches the 4:3 render. The game's own render
 * functions still run at canonical 4:3, so gameplay-readable state does not
 * depend on the factor. The layout derives it from a fixed --aspect
 * (aspect / (4/3)) or, under AUTO, from the panel cell's shape against the
 * crop-corrected 4:3 display, clamped to at least 1 (about 1.33 at 16:9).
 * Headless runs have no window, so a fixed --aspect is applied here
 * directly. The layout runs on the present thread and publishes the factor;
 * this returns the copy openpete_native_producer_knobs_latch took on the sim
 * thread. Feeders that pre-cull to the 4:3 screen-X rect widen their
 * horizontal bounds by this factor (openpete_native_widescreen_x_bounds);
 * vertical bounds do not change.
 * @return The widen factor, 1.0 when the rect is not wider than 4:3. */
float openpete_native_widescreen_factor(void);
/** @brief Return the visible PSX screen-X range under the active widescreen factor.
 *
 * The range is [0, 512] at 4:3, widened symmetrically about the screen
 * centre 256 by openpete_native_widescreen_factor, about [-85, 597] at 16:9.
 * Feeders use it as the left and right bound of their horizontal trivial
 * reject in place of the fixed 0 and 512 edges.
 * @param xlo Receives the left bound; may be NULL.
 * @param xhi Receives the right bound; may be NULL. */
void  openpete_native_widescreen_x_bounds(float *xlo, float *xhi);
/** @brief Return the visible PSX screen-Y range of the displayed frame.
 *
 * The range is the clip band of the guest's display geometry, centred in
 * the display height: [(disp_h - clip_h) / 2, (disp_h + clip_h) / 2], which
 * scene_to_clip maps to the frame's top and bottom edges; 240 and 224 stand
 * in before the guest sets them. The widescreen factor does not change it.
 * Reads the guest-side geometry, so it is valid on the extracting thread.
 * @param ylo Receives the top bound; may be NULL.
 * @param yhi Receives the bottom bound; may be NULL. */
void  openpete_native_visible_y_bounds(float *ylo, float *yhi) OP_REQUIRES_SHARED(sim);

/* ---- field of view display policy ------------------------------------ */
/** @brief Set --fov, the vertical FOV in degrees of the visible band; 0 or less keeps the game's projection.
 *
 * The visible band is clip_h rows, and its vertical FOV is the one FOV
 * number Hor+ keeps aspect-invariant. The feeders project with
 * H_eff = H_canon · s, s = tan(vref/2) / tan(fov/2), vref = 2·atan((clip_h/2)
 * / (y_row · H_canon)), where H_canon is the GTE's H and y_row the game's
 * camera-matrix Y scale (the feeder passes both). At deg <= 0 the scale is
 * the literal 1.0 and H_eff == H_canon bit-for-bit. A positive value is
 * clamped to [1, 179]. The setter stores a wanted value from any thread;
 * openpete_native_producer_knobs_latch applies it on the sim thread, so a
 * live change cannot split one tick's feeds across two H values. The latch
 * that applies a change suspends the occlusion grid for two presents, since
 * its cached rects were built at the old H. Renderer state only; the game's own
 * projection keeps the canonical H.
 * @param deg Vertical FOV in degrees; <= 0 for the default. */
void   openpete_native_set_fov(float deg);
/** @brief Return the vertical FOV set by openpete_native_set_fov: the value the next latch applies.
 *
 * @return The clamped setting in degrees; 0 for the game's default. */
float  openpete_native_fov_deg(void);
/** @brief Return the factor that widens or narrows the canonical projection H to the FOV setting.
 *
 * The factor is the canonical vertical half-tangent, (visible rows / 2) /
 * (y_row * H_canon), divided by the tangent latched by
 * openpete_native_producer_knobs_latch. The effective H is H_canon times the
 * factor.
 * @param H_canon The GTE's canonical H.
 * @param y_row The camera matrix's Y-row scale.
 * @return The factor; 1 at the default FOV or when H_canon or y_row is not
 *         positive. */
double openpete_native_fov_h_scale(double H_canon, double y_row);
/** @brief Return the factor that takes the canonical projection H to a vertical FOV over a given number of visible rows.
 *
 * The pure form of openpete_native_fov_h_scale: the canonical vertical
 * half-tangent, (rows / 2) / (y_row * H_canon), divided by tan(fov / 2).
 * --fov's setter and the launcher's box picture both take it.
 * @param fov_deg The vertical FOV in degrees; not positive for the default.
 * @param rows The visible row count.
 * @param H_canon The GTE's canonical H.
 * @param y_row The camera matrix's Y-row scale.
 * @return The factor; 1 at the default FOV or when an input is not positive. */
double openpete_native_fov_h_scale_at(double fov_deg, int rows, double H_canon, double y_row);
/** @brief Return the number of rows in the visible band the panel shows.
 *
 * The value is the guest draw-clip height in s_nr (224 on Spyro 1), or 224
 * while no clip height has been applied. The FOV scale and label derive the
 * canonical vertical FOV from it.
 * @return The visible row count. */
int    openpete_native_visible_rows(void);
/** @brief Return the horizontal FOV in degrees the current window displays for a canonical projection.
 *
 * The value is 2·atan(256·F / H_eff), where F is the Hor+ widescreen factor
 * and H_eff = H_canon · openpete_native_fov_h_scale, which applies the FOV
 * tangent latched at scene begin.
 * @param H_canon The GTE's canonical H.
 * @param y_row The camera matrix's Y-row scale.
 * @return The horizontal FOV in degrees, or 0 when H_eff is not positive. */
double openpete_native_fov_horizontal_deg(double H_canon, double y_row);
/** @brief Record the projection the feeders last built a view from, as the FOV label's inputs.
 *
 * openpete_native_fov_angles derives the label's angles from these values.
 * @param H_canon The GTE's canonical H.
 * @param y_row The camera matrix's Y-row scale. */
void  openpete_native_fov_note_canon(double H_canon, double y_row);
/** @brief Compute the FOV label's vertical and horizontal angles.
 *
 * The vertical angle is the --fov setting, or the game's own at the default.
 * The horizontal angle uses the live Hor+ factor and the pending FOV
 * tangent, not the one latched at scene begin. Both derive from the
 * projection last recorded by openpete_native_fov_note_canon; before any is
 * recorded they assume H 341 and Y row 0.625.
 * @param v_deg Receives the vertical FOV in degrees; may be NULL.
 * @param h_deg Receives the horizontal FOV in degrees; may be NULL.
 * @return 1 when a view has been recorded, 0 when the defaults were used. */
int   openpete_native_fov_angles(double* v_deg, double* h_deg);

/** @brief Compose and present one native image to the Vulkan window on the inline path.
 *
 * Call after PsyX_EndScene, so PsyCross's present target holds a fresh
 * reference. Applies queued texture-mapping, notex, wireframe and truescope
 * changes outside the frame, syncs the VRAM mirror and the reference
 * texture, then composes and presents. Does not pump SDL events: the host
 * loop owns the event queue, which PsyCross's input path depends on, and
 * window-size changes are detected by polling. No-op when native render is
 * disabled or headless; a headless capture composes on demand in
 * openpete_native_render_screenshot_all.
 * @param record_pts_us Capture instant in op_now microseconds tagged onto
 *        this present for --record. The image is queued into the async
 *        readback ring between the blit and the present and retrieved a few
 *        presents later, so a recording holds the on-screen image at real
 *        speed. Unused when recording is inactive. */
void openpete_native_render_frame(uint64_t record_pts_us) OP_REQUIRES(sim);

/** @brief Re-present the last composed image without recomposing it.
 *
 * The offscreen image persists, so acquiring a swapchain image and blitting
 * it again shows the same image. Issues no scene work and reads no guest
 * state, so it is the one present a thread other than the guest's may make.
 * Refused while the present thread owns the display, while a swapchain
 * rebuild is pending (that belongs to the window's thread), and when
 * begin_frame declines (swapchain recreating, acquire budget spent,
 * minimized window).
 * @return 1 when a present went out, else 0. */
int openpete_native_render_repeat_last(void);

/* ---- eager scene ring ----------------------------------------------------
 * The thread running the guest extracts every scene of a span ahead of time
 * and publishes each into a ring; a present thread composes ring slots on
 * the display grid. See native_render.c "eager scene ring" for the slot
 * lifecycle and the ownership rule. Times are op_now() nanoseconds. */
/** @brief Stamp the next terrain build with the eager scene id instead of the VBL counter.
 *
 * Call before the re-extract of an eager scene. The stamp is the next scene
 * id with OPTC_STAMP_SCENE_BIT set, so it cannot collide with a VBL number;
 * openpete_native_scene_build_stamp returns it until
 * openpete_native_scene_publish clears it. */
void     openpete_native_scene_eager_begin(void);
/** @brief Return the stamp the terrain builder tags its build with.
 *
 * @return The eager scene stamp between openpete_native_scene_eager_begin
 *         and the scene's publish, else the VBL counter. */
uint32_t openpete_native_scene_build_stamp(void);
/** @brief Move the accumulating scene into a free ring slot and copy its per-present companions.
 *
 * The triangle storage is exchanged with the slot's, so the accumulator
 * holds no triangles afterwards.
 *
 * Any free slot serves, not only the one after the last publish: slot
 * position carries no meaning, since the pick orders by scene id and retire
 * and flush scan the whole ring, while a presented slot can stay unretired
 * as long as the swapchain refuses images (a live resize), because
 * retirement runs only after a successful begin_frame. Requiring one exact
 * slot would let such a straggler refuse every publish and freeze the
 * display. Clears the eager build stamp.
 * @param target_ns Display instant the scene is for.
 * @param kind OP_PACE_CANONICAL or OP_PACE_INTERMEDIATE.
 * @param alpha Sub-tick alpha of the scene.
 * @param paired Non-zero when the re-extract had a prev/curr pair.
 * @return The slot index, or -1 when no slot is free or the slot could not
 *         be sized: a refused publish, never a truncated scene. */
int      openpete_native_scene_publish(int64_t target_ns, int kind, double alpha, int paired) OP_REQUIRES(sim);
/** @brief Return the earliest display target among the pending ring scenes.
 *
 * @return The target in op_now nanoseconds, or 0 when none is pending. */
int64_t  openpete_native_scene_next_target(void);
/** @brief Pick the pending scene whose target is nearest at; it becomes PRESENTING.
 *
 * Every pending scene older than the pick (by scene id) retires unpresented
 * as superseded. Alpha binds here, at consume time: the scene shown is the
 * animation moment nearest the flip.
 * @param at The display instant, op_now nanoseconds.
 * @param due Receives the pending scenes whose target is at or before at,
 *            counted before the pick; may be NULL.
 * @return The picked slot, or -1 when none is pending. */
int      openpete_native_scene_pick(int64_t at, int *due);
/** @brief Compose a ring slot and present it to the swapchain directly.
 *
 * The slot's lifecycle matches openpete_native_render_scene_to_stage with no
 * stage image.
 * @param slot Ring slot to compose.
 * @param record_pts_us Capture instant in op_now microseconds for --record.
 * @return 1 when the render went out, 0 when it was declined, native render
 *         is off or headless, or @p slot is out of range. */
int      openpete_native_render_scene(int slot, uint64_t record_pts_us);
/** @brief Compose a ring slot into a stage image for a later present.
 *
 * The staged form of openpete_native_render_scene: the render ends in stage
 * image @p stage, and openpete_native_present_stage presents it later. The
 * slot's lifecycle is the same as a direct present's: PRESENTED under the
 * render serial when the render went out, FREE when the swapchain declined.
 * @param slot Ring slot to compose.
 * @param stage Stage image index.
 * @param record_pts_us Capture instant in op_now microseconds for --record.
 * @return 1 when the render went out, 0 when it was declined, native render
 *         is off or headless, or @p slot is out of range. */
int      openpete_native_render_scene_to_stage(int slot, int stage, uint64_t record_pts_us);
/** @brief Hold a swapchain image for the next staged present.
 *
 * Blocks until the image is acquired. An image already held counts as
 * success.
 * @return 1 when an image is held, 0 when native render is off or headless,
 *         there is no device, or the acquire failed. */
int      openpete_native_present_stage_acquire(void);
/** @brief Present a stage image to the swapchain.
 *
 * Acquires an image first when none is held. Blocks until the present is
 * submitted.
 * @param stage Stage image index, as passed to
 *        openpete_native_render_scene_to_stage.
 * @return 1 when presented, 0 when native render is off or headless, there
 *         is no device, the index is out of range, or the swapchain
 *         declined. */
int      openpete_native_present_stage(int stage);
/** @brief Give back a swapchain image held for a staged present that will not be submitted.
 *
 * Used when the display is handed back to the inline path. The image is
 * presented with the pixels it last held, since Vulkan has no un-acquire.
 * @return 1 when nothing was held, native render is off or headless, there
 *         is no device, or the image was given back; 0 after device loss. */
int      openpete_native_present_stage_release(void);
/** @brief Make the calling thread the owner of the display.
 *
 * Forwards rhi_display_claim; the present thread claims as it engages and
 * the host loop as a drain hands the display back. Does nothing before the
 * device exists. */
void     openpete_native_display_claim(void);
/** @brief Return the number of stage images a staged present can target.
 *
 * Valid stage indices are 0 to one less than the count.
 * @return The RHI stage image count, or 0 when there is no device. */
int      openpete_native_stage_count(void);
/** @brief Return the stage image generation, which changes when the stage images are rebuilt.
 *
 * A staged frame records the generation it was rendered under and the
 * present thread discards it when the value differs at present time.
 * @return The device's stage generation, or 0 when there is no device. */
uint32_t openpete_native_stage_gen(void);
/** @brief Free every presented scene whose frame is RHI_FRAMES_IN_FLIGHT or more behind the frame just begun.
 *
 * Called by the presenting thread right after each rhi_begin_frame: those
 * frames' fences have been waited, so their scenes' GPU reads are over.
 * @param begun_serial The serial of the frame just begun. */
void     openpete_native_scene_retire(uint64_t begun_serial);
/* Slots in the eager scene ring. */
#define OPENPETE_NATIVE_SCENE_RING 48
/** @brief Return whether any unretired ring scene carries a terrain-cache build stamp.
 *
 * @param stamp The build stamp.
 * @return 1 while a pending, presenting or presented scene holds it. */
int      openpete_native_scene_stamp_live(uint32_t stamp);
/** @brief Drop every pending ring scene (guest discontinuity: savestate, rewind, level load); a scene mid-present finishes. */
void     openpete_native_scene_ring_flush(void);
/** @brief Return the number of pending ring scenes.
 *
 * @return The count. */
int      openpete_native_scene_ring_pending(void);
/** @brief Return the number of ring slots not free (pending, presenting or presented).
 *
 * @return The count. */
int      openpete_native_scene_ring_busy(void);
/** @brief Read the timing and kind fields of a ring slot.
 *
 * Leaves every output unchanged when @p slot is out of range.
 * @param slot Ring slot index.
 * @param target_ns Receives the slot's display instant in op_now
 *        nanoseconds; may be NULL.
 * @param alpha Receives the slot's sub-tick alpha; may be NULL.
 * @param paired Receives the slot's prev/curr pairing flag; may be NULL.
 * @param kind Receives the slot's OP_PACE_* scene kind; may be NULL. */
void     openpete_native_scene_slot_info(int slot, int64_t *target_ns, double *alpha, int *paired, int *kind);
/** @brief Read a ring slot's scene id and terrain-cache build stamp.
 *
 * Leaves both outputs unchanged when @p slot is out of range. Called by the
 * publishing thread, which alone writes these fields.
 * @param slot Ring slot index.
 * @param scene_id Receives the scene id publish gave the slot; may be NULL.
 * @param stamp Receives the scene's build stamp; may be NULL. */
void     openpete_native_scene_slot_ids(int slot, uint32_t *scene_id, uint32_t *stamp);
/** @brief Log the scene ring's publish census at exit.
 *
 * The line gives the counts published, presented, superseded before
 * presenting, and refused for a full ring. Logs nothing when no scene was
 * published. */
void     openpete_native_scene_ring_report(void);
/** @brief Return the number of ring scenes retired unpresented since start: superseded at a pick or flushed while pending.
 *
 * @return The running count, modulo 2^32. */
unsigned openpete_native_scene_ring_superseded(void);
/** @brief Log the window state and the per-present census at exit.
 *
 * The census counts what each composed image showed (native scene, VRAM
 * framebuffer fallback, PsyCross reference, bare clear) per scene kind,
 * with extents, triangle and retained-sector ranges, and the number of
 * presents whose outcome or kind changed from their predecessor. The census
 * lines are omitted when no present was composed or refused at begin. */
void     openpete_native_present_census_report(void);
/** @brief Return how many scene composes ran and whether the last one showed the PSX VRAM framebuffer.
 *
 * A compose whose scene holds no triangle of the game's own and no retained
 * terrain draws no native scene; the caller re-displays the PSX VRAM
 * framebuffer instead. Live presents and screenshot dumps both count.
 * @param vram Receives 1 when the last compose showed the VRAM framebuffer,
 *        else 0; may be NULL.
 * @return Scene composes so far, modulo 2^31. */
unsigned openpete_native_compose_serial(int *vram);
/** @brief Return the refresh rate of the display the current swapchain presents to.
 *
 * @return The rate in Hz; 0 when unknown or before the device exists. */
float    openpete_native_display_refresh_hz(void);
/** @brief Return whether the granted present mode is FIFO or FIFO relaxed.
 *
 * @return 1 for a vsync-paced mode; 0 for another mode, headless, or before
 *         the device exists. */
int      openpete_native_display_vsync(void);
/** @brief Return the number of ring slots free for the producer to fill.
 *
 * The producer's back-pressure: the ring size minus
 * openpete_native_scene_ring_busy.
 * @return The count. */
int      openpete_native_scene_ring_free(void);
/** @brief Refresh the VRAM mirror from the guest's VRAM, dirty rects only.
 *
 * Takes the VRAM mirror lock the present-side upload also takes. The eager
 * producer calls it before each scene while the guest is idle, so no guest
 * VRAM write lands mid-drain; the inline present path syncs on its own.
 * No-op when the native renderer is disabled or headless. */
void     openpete_native_vram_sync(void);
/** @brief Set the auxiliary VRAM bank: a host image the renderer's VRAM texture holds below the PSX rows.
 *
 * The image is VRAM columns 512-1023 by 512 rows, row-major, 16-bit texels
 * (the layout of a Spyro level's texture sub-file head). It is copied and
 * uploaded at the next frame into texture rows 512-1023 at the same
 * columns, where a material whose texture-page and CLUT rows carry +512
 * samples it with the stock texel, palette and fog-ladder math. The guest's
 * VRAM and the native VRAM model are not touched. A short image is ignored.
 * @param image The texels.
 * @param bytes The image's byte count; at least 512 * 512 * 2. */
void     openpete_native_vram_aux_set(const uint8_t* image, size_t bytes);

/* --- sub-tick re-feed spine ------------------------------------------------ */
/// @brief Game callback that rebuilds the native scene for one present against interpolated gamestate.
typedef void (*openpete_native_reextract_fn)(void* cpu);
/** @brief Register the game's re-extract callback, which rebuilds the native scene for presents between ticks.
 *
 * Above the 29.913 Hz sim the host presents intermediate (sub-tick) images;
 * each rebuilds the native scene against an interpolated view of gamestate
 * (camera, object poses, animation clock) keyed off the openpete_interp
 * snapshot ring. The rebuild is game-specific, so the game registers it.
 * Core calls it on each intermediate present, on the canonical present at
 * the same one-tick-lagged alpha while the spine is active, and once per
 * scene under eager production. The callback receives the host CPUState
 * (opaque here) and must not write guest RAM: the shared installer
 * game_reextract_view_install wraps the game's body in
 * openpete_subtick_view_begin/end, so feeders read interpolated values from
 * staged spans instead of guest memory. NULL disables intermediates.
 * @param fn The callback, or NULL. */
void openpete_native_register_reextract(openpete_native_reextract_fn fn);
/** @brief Return the registered re-extract callback.
 *
 * @return The callback, or NULL when none is registered. */
openpete_native_reextract_fn openpete_native_get_reextract(void);

/// @brief Game callback run once per canonical present for render bookkeeping the game owns.
typedef void (*openpete_native_commit_fn)(void);
/** @brief Register the game's per-canonical-present commit callback.
 *
 * The host present loop calls it once per canonical present, after the game
 * render and before the sub-tick re-extract, whether or not the spine is
 * active. The shared game renderer's callback (render_paths_commit) latches
 * the mask of render paths the canonical render drew, which the spine gates
 * each feeder on, and clears the per-render interval table. It runs from
 * the host loop rather than inside psyq_DrawOTag, so it does not depend on
 * the PsyCross OT submission. NULL = no commit step. The callback runs on
 * the thread holding the sim role, so it may require sim and no other role.
 * @param fn The callback, or NULL. */
void openpete_native_register_commit(openpete_native_commit_fn fn OP_SLOT(sim));
/** @brief Run the registered commit callback, when one is registered. */
void openpete_native_run_commit(void) OP_REQUIRES(sim);

/* ---- moby occlusion culling (native-side, display-only) ---------------------
 *
 * A per-present GPU reduction of the WORLD-opaque depth pre-pass (the shadow
 * decal's pre-pass, shared) into a coarse conservative "farthest view-Z per
 * cell" grid, ring-read-back to the CPU a few presents later. The moby feeder
 * queries it BEFORE re-extracting a cull record: a moby whose whole
 * (conservatively inflated) screen rect is provably behind nearer terrain is
 * skipped - no model walk, no scene tris, no draws - because the depth test
 * would erase every pixel of it anyway. The CANONICAL cull pass and every
 * gameplay-readable byte it writes are untouched (this only skips native
 * drawing), so it is determinism-safe by construction.
 *
 * openpete_native_occl_query: sx/sy in PSX screen px (the scene_vert space),
 * vz_near = the moby's nearest possible view-Z (centre − radius, full-world
 * units). Returns 1 => provably occluded (safe to skip), 0 => visible/unknown.
 * Conservative under grid staleness: rect inflation, view-Z slack
 * (OPENPETE_MOBY_OCCL_SLACK), edge/age refusal. OPENPETE_MOBY_OCCLUSION=0
 * disables. openpete_native_occl_suspend(n) disables culling for the next n
 * presents - the feeder calls it on camera cuts/teleports, where a
 * few-presents-old grid is wrong everywhere.
 *
 * Camera bound: the grid is the reduction of the depth pre-pass of the scene
 * one present composed, so it encodes the world as seen from THAT scene's
 * camera. A query evaluated under a different camera reads cell depths at the
 * wrong screen positions; the error is lateral parallax, which the depth slack
 * does not cover, and it removes visible geometry for the one tick the mismatch
 * lasts. Both feeders therefore pass the camera their own walk projects with,
 * and the depth verdict is only taken while that camera is within
 * OPENPETE_OCCL_CAM_MOVE world units and OPENPETE_OCCL_CAM_TURN degrees of the
 * grid's. Outside the bound the query returns 0 (keep): the cull only ever
 * removes work, never a draw. openpete_native_occl_set_camera publishes the
 * producing walk's camera; it is snapshotted per scene, so a present's grid is
 * tagged with the camera whose geometry it reduced. */
typedef struct openpete_occl_camera {
    float pos[3];   /* camera position, full-world units */
    float M[9];     /* the game's camera matrix, rows unit-normalized, row-major */
    int   valid;
} openpete_occl_camera;
/** @brief Publish the camera the terrain walk now feeding the scene projects with.
 *
 * Called on every extract, sub-tick re-extracts included. The scene publish
 * copies it into the ring slot so the occluder grid built from that scene's
 * depth pre-pass carries the camera it was rasterized under.
 * @param c Camera to copy; NULL marks the published camera invalid. */
void openpete_native_occl_set_camera(const openpete_occl_camera *c) OP_REQUIRES(sim);
/** @brief Copy the camera the terrain walk now feeding the scene published (openpete_native_occl_set_camera).
 * @param c Receives the camera; valid is 0 when none is published. */
void openpete_native_occl_live_camera(openpete_occl_camera *c) OP_REQUIRES_SHARED(sim);
/** @brief Return 1 when every grid cell over a screen rect is nearer than vz_near: the geometry is occluded.
 *
 * Called on the extracting thread; it reads that thread's copy of the last
 * published grid (cells, affine, stamp and camera from one present). Returns
 * 0 while the grid is absent, suspended, more than 8 presents old, or taken
 * under a camera outside the bound of cam; an off-screen rect returns 1.
 * @param sx0 Left edge in PSX screen pixels.
 * @param sy0 Top edge in PSX screen pixels.
 * @param sx1 Right edge in PSX screen pixels.
 * @param sy1 Bottom edge in PSX screen pixels.
 * @param vz_near Nearest view-Z of the object, including the caller's slack.
 * @param cam The camera the caller's walk projects with.
 * @return 1 to cull, 0 to keep. */
int  openpete_native_occl_query(float sx0, float sy0, float sx1, float sy1, float vz_near,
                                const openpete_occl_camera *cam) OP_REQUIRES_SHARED(sim);
/** @brief Return the outcome of an occlusion query: refused, kept by depth, or culled, without counting it in the census.
 *
 * openpete_native_occl_query is this outcome under the configured slack,
 * counted, and mapped to 1 for OPENPETE_OCCL_Q_CULLED and
 * OPENPETE_OCCL_Q_OFFSCREEN. OPENPETE_OCCL_Q_NOGRID, _SUSPENDED, _STALE and
 * _MOVED refuse: the held grid cannot answer for this camera.
 * OPENPETE_OCCL_Q_TOONEAR and _VISIBLE answer that the rect may draw.
 * @param sx0 Left edge in PSX screen pixels.
 * @param sy0 Top edge in PSX screen pixels.
 * @param sx1 Right edge in PSX screen pixels.
 * @param sy1 Bottom edge in PSX screen pixels.
 * @param vz_near Nearest view-Z of the object, including the caller's slack.
 * @param cam The camera the caller's walk projects with.
 * @param slack The view-Z margin by which occluding cells must be nearer.
 * @return The OPENPETE_OCCL_Q_* outcome index. */
int  openpete_native_occl_query_outcome(float sx0, float sy0, float sx1, float sy1, float vz_near,
                                        const openpete_occl_camera *cam, float slack) OP_REQUIRES_SHARED(sim);
/** @brief Return the view-Z slack openpete_native_occl_query applies (OPENPETE_MOBY_OCCL_SLACK, default 1500).
 * @return The slack in view-Z units. */
float openpete_native_occl_slack(void);
/** @brief Build the occlusion grid headless at the host loop's present step, as a live present does.
 *
 * Headless never runs a live present, so without this no grid exists and
 * both halves of the occlusion test (the off-screen reject and the depth
 * query) refuse every query. Runs at the host loop's present step, after the
 * walk: one device frame holding the world-opaque depth pre-pass of the scene
 * a live present would compose and the grid reduction, then a synchronous
 * read of the grid. The next tick's walk reads the grid of this tick's
 * scene, one tick old, where the ring readback a window uses would leave it
 * RHI_FRAMES_IN_FLIGHT present steps (ticks, headless) old; a deterministic
 * function of the present sequence. Self-no-ops with a window, where the
 * live present builds the grid.
 */
void openpete_native_occl_service_headless(void) OP_REQUIRES(sim);
/** @brief Register a census that queries an occluder grid built at the camera of the scene just extracted.
 *
 * openpete_native_occl_census_present calls every registered function, at
 * most four, once per call; registering one twice keeps one entry.
 * @param fn The census; it may call openpete_native_occl_query. */
void openpete_native_occl_census_register(void (*fn)(void));
/** @brief Build the occlusion grid from the current scene, run the registered censuses against it, then restore the previous grid.
 *
 * Headless only, and a no-op with no census registered. One device frame
 * holds the scene's world-opaque depth pre-pass and the grid reduction; the
 * grid is read back synchronously and published with the scene's own camera,
 * so a query under that camera passes the camera bound and answers from the
 * cell depths. The previously published grid, the present counter and the
 * cut suspension are restored afterwards, so the walks that follow read the
 * state they would read without the census. */
void openpete_native_occl_census_present(void) OP_REQUIRES(sim);
/** @brief Refuse occlusion verdicts for at least the next presents presents; callable from any thread.
 *
 * Raises the suspension to presents when it is lower; the presenting thread
 * decrements it once per present.
 * @param presents Presents to suspend for. */
void openpete_native_occl_suspend(int presents);
/* Query-outcome census (cumulative counters; diagnostics). OFFSCREEN = culled
 * because the rect lies fully outside the visible extent (native frustum cull
 * - the game's cull cone is wider than the screen, so it queues off-screen
 * mobys that contribute zero pixels); CULLED = culled by the depth grid;
 * MOVED = kept because the caller's camera is outside the grid camera's
 * move/turn bound. */
enum { OPENPETE_OCCL_Q_NOGRID = 0, OPENPETE_OCCL_Q_SUSPENDED, OPENPETE_OCCL_Q_STALE,
       OPENPETE_OCCL_Q_TOONEAR, OPENPETE_OCCL_Q_EDGE, OPENPETE_OCCL_Q_VISIBLE,
       OPENPETE_OCCL_Q_CULLED, OPENPETE_OCCL_Q_OFFSCREEN, OPENPETE_OCCL_Q_MOVED,
       OPENPETE_OCCL_Q_COUNT };
/** @brief Copy the occlusion query outcome census.
 *
 * The counts accumulate per OPENPETE_OCCL_Q_* outcome from boot and are
 * never reset. The read takes no lock, so it is diagnostic only.
 * @param out Receives OPENPETE_OCCL_Q_COUNT counts, indexed by outcome. */
void openpete_native_occl_qstats(long out[OPENPETE_OCCL_Q_COUNT]);
/** @brief The farthest grid view-Z in the rect of the last query the depth test culled.
 *
 * Diagnostics: how decisively the depth test held. Updated only when
 * openpete_native_occl_query returns 1 through the depth test; an
 * off-screen return leaves it unchanged.
 * @return View-Z of the farthest grid cell scanned in that rect. */
float openpete_native_occl_last_scan_max(void);
/** @brief Return the number of presents elapsed since the occlusion grid was captured.
 *
 * Diagnostics. The query refuses a grid older than 8 presents.
 * @return The age in presents, or -1 when no grid is held. */
int  openpete_native_occl_grid_age(void);

/* ---- per-present phase timers (OPENPETE_PHASE_TIMERS=1) ---------------------
 *
 * Perf diagnosis: where does a present's CPU time go? Feeders/host-loop add
 * per-phase µs into the current present; openpete_native_render_frame rolls
 * the present into a ring and prints a p50/p99/max summary once per second
 * (stderr), plus the worst-present counts (moby records, scene tris, draw
 * calls). Everything below is inert (single branch) unless the env is set.
 * Diagnostic-only; never touches gameplay state. */
enum {
    OPPT_REX_TOTAL = 0,  /* whole sub-tick re-extract callback (run.c)          */
    OPPT_REX_TERRAIN,    /* terrain feeder pass                                 */
    OPPT_REX_MOBY,       /* moby feeder pass (REG+SPEC+SHD walks)               */
    OPPT_REX_SHADOW,     /* shadow feeder pass                                  */
    OPPT_REX_SPYRO,      /* Spyro player feeder pass                            */
    OPPT_PREPASS,        /* WORLD-opaque depth pre-pass record                  */
    OPPT_COMPOSE,        /* scene bucket + material qsort + draw recording      */
    OPPT_GPU_SYNC,       /* begin_frame fence/acquire + blit + end_frame submit */
    OPPT_SIM_TICK,       /* game-fiber yield: game logic + canonical .s render +
                          * feeder-hook extraction (30Hz cost, folded into the
                          * canonical present; NOT reducible by native culling) */
    OPPT_COUNT
};
enum { OPPC_MOBYS = 0, OPPC_TRIS, OPPC_DRAWS, OPPC_CULLED, OPPC_COUNT };
/** @brief Return whether the per-phase microsecond measurements are wanted.
 *
 * Call sites gate their clock reads on it.
 * @return 1 when OPENPETE_PHASE_TIMERS requests the report or a perf trace
 *         is open, else 0. */
int      openpete_phase_timers_on(void);
/** @brief Return a monotonic time in microseconds for phase measurements.
 *
 * Derived from the SDL performance counter; only differences between two
 * calls are meaningful.
 * @return Microseconds on the performance counter's timebase. */
uint64_t openpete_phase_now_us(void);
/** @brief Add microseconds to a phase's total for the present being built.
 *
 * The total is a 32-bit accumulator, so @p us is truncated to 32 bits. When
 * a perf trace is open the value is also recorded as a span of the phase.
 * @param phase OPPT_* phase index; an out-of-range index is ignored.
 * @param us Elapsed microseconds to add. */
void     openpete_phase_add(int phase, uint64_t us);
/** @brief Set a census count for the present being built.
 *
 * Replaces the count of the present in progress. When a perf trace is open
 * the value is also recorded as a count of the census.
 * @param which OPPC_* count index; an out-of-range index is ignored.
 * @param n The count. */
void     openpete_phase_set_count(int which, int n);

/* 2D-overlay carry for the sub-tick re-extract: the fade + cutscene-letterbox +
 * menu-panel overlays are produced by the 2D-UI feeder during the game render
 * and can't be re-derived sub-tick. Carried tris are ACCUMULATED at add_tri
 * time (feed-time capture - the live scene is wiped by every sub-tick
 * re-extract, so scanning it at capture time raced the wipes and dropped any
 * 2D feed an intermediate present landed between; the collected-egg HUD sprite
 * froze/vanished at high --render-fps). capture_overlay() commits the
 * accumulated set as the carry (call on the CANONICAL present, before the
 * re-extract); apply_overlay() re-applies it after the 3D feeders (call at the
 * end of every re-extract). See native_render.c. */
/** @brief Commit the 2D tris accumulated at feed time as this tick's overlay carry.
 *
 * Call on the canonical present, before the re-extract. Runs the
 * pre-capture hook first, rolls the previous carry and its tick stamp to
 * prev so apply_overlay can interpolate animated 2D positions, stamps the
 * capture with openpete_interp_tick_stamp, moves the accumulation into the
 * carry and sorts it into emission order. When a carry entry was fed into a
 * scene that has since been replaced and the scene holds no re-applied
 * carry, the scene's carry-eligible tris are replaced by the carry set, so
 * the canonical and sub-tick presents show the same 2D layer. The scene's
 * 2D is then sorted into emission order. A present with no game render
 * since the previous capture (no scene opened by the game and nothing
 * accumulated) shows the scene that capture took: the carry is kept, with
 * the previous set equal to it, and the retained area's held list is left
 * as it was. */
void openpete_native_scene_capture_overlay(void) OP_REQUIRES(sim);
/** @brief Bound the 2D accumulation to the latest canonical present the host holds back.
 *
 * Call once per owed canonical present that is not presented (the
 * skip-to-level present fast-forward), where its capture would have run.
 * Runs the pre-capture hook, so the 2D feeder releases its pending batch
 * as a capture would, then discards the tris accumulated before the
 * previous held present, keeping the 2D of the latest one, and clears the
 * overflow count. The carry and its previous set are left as they are. The
 * first capture after the hold presents the latest held scene and commits
 * that scene's 2D and anything fed after it, as a capture that had run at
 * every owed present would. */
void openpete_native_scene_hold_2d(void) OP_REQUIRES(sim);
/** @brief Report whether the canonical present in flight repeats the previous one: no game render since the last capture.
 *
 * The game's render made no begin_frame (it opened and fed no scene) and
 * accumulated no 2D since the previous canonical capture, after the 2D
 * feeder's deferred primitive is flushed: the game submitted an ordering
 * table it added nothing to, and the present shows the scene the previous
 * capture took. The per-present commits then hold what that scene's
 * sub-tick replay reads (the drawn render paths, the producer epoch, the 2D
 * carry, the fade and the freeze) instead of rolling it to an empty set.
 * Computed on the first call of a canonical present and cleared by the
 * capture.
 * @return 1 for a repeat present, else 0. */
int openpete_native_scene_canon_repeat(void) OP_REQUIRES(sim);
/** @brief Turn the repeat present in flight into the present of an empty scene.
 *
 * For a game that cleared the screen without drawing: the scene the
 * previous capture took is closed and an empty one is opened at the current
 * VBL as a game feed opens one (openpete_native_scene_begin_frame), so the
 * present draws only the clear and the per-present commits roll to that
 * empty scene (no 2D carry, fade or freeze). A no-op unless
 * openpete_native_scene_canon_repeat reported 1 for this present; the
 * verdict reads 0 afterwards. */
void openpete_native_scene_open_empty(void) OP_REQUIRES(sim);
/** @brief Report what the draw area a display rect shows holds: content, an empty auto-cleared table, or unknown.
 *
 * The area is the first draw clip recorded inside the rect. Its record is
 * the last write into it: a DrawOTag of a table with content (the fed
 * scene, or the last capture's scene when the game rendered between it and
 * the capture before) leaves content; a DrawOTag of an empty table under an
 * auto-clearing DRAWENV leaves the fill; a VRAM op leaves an image, except
 * a copy of one whole recorded area onto another, which carries the
 * source's record. Display state only; reads and writes no PSX RAM.
 * @param disp_x Display rect left, VRAM texels.
 * @param disp_y Display rect top.
 * @param disp_w Display rect width.
 * @param disp_h Display rect height.
 * @return 1 content, 0 the fill of an empty table, -1 no recorded area in the rect or an image op wrote it last. */
int openpete_native_shown_area_content(int disp_x, int disp_y, int disp_w, int disp_h);
/** @brief Re-apply the overlay carry to the scene, interpolated to the present instant.
 *
 * Call at the end of every re-extract, after the 3D feeders. Each carry
 * triangle matched to a triangle of the previous carry gets its screen
 * positions blended between the two by the overlay's own alpha; unmatched
 * triangles are appended unblended. Appended triangles are marked re-applied
 * so the next capture skips them. The freeze and fade words are blended
 * with the same alpha. The overlay alpha spans one tick of the overlay
 * capture cadence measured on the present clock and clamps to 0..1; under
 * deterministic capture it comes from the tick grid instead. It falls back
 * to @p alpha until the capture stamps are primed.
 * @param alpha The camera bracket's alpha, 0 when the camera ring dedupes. */
void openpete_native_scene_apply_overlay(double alpha) OP_REQUIRES(sim);
/** @brief Mark the accumulating scene's triangle count for openpete_native_scene_front_end. */
void openpete_native_scene_front_begin(void) OP_REQUIRES_SHARED(sim);
/** @brief Move the triangles added since openpete_native_scene_front_begin in front of the scene's first 2D triangle.
 *
 * For a feeder that appends geometry to a scene after that scene's 2D was
 * fed: the 2D layer draws in array order and its emission-order sort works
 * within contiguous runs of 2D triangles, so geometry appended among them
 * would split a run. The moved triangles keep their order. Does nothing
 * without a mark or when nothing was added since it.
 */
void openpete_native_scene_front_end(void) OP_REQUIRES(sim);
/** @brief Register the callback run at the top of the canonical overlay capture.
 *
 * Lets the 2D-UI feeder flush a deferred primitive into this tick's
 * accumulation before the carry commits; by the canonical present every
 * post-link primitive mutation is done, so the flush is exact. One callback;
 * a later call replaces it.
 * @param fn Callback, or NULL. */
void openpete_native_scene_register_overlay_precapture(void (*fn)(void) OP_SLOT(sim));
/** @brief Roll the fade and freeze-backdrop carries to the tick just rendered.
 *
 * Call on each canonical present, before the re-extract, so apply_overlay
 * interpolates the fade across sub-ticks. The previous current fade becomes
 * prev; the new fade is the game's set_fade value when it was called since
 * the last commit, else 0. The freeze backdrop rolls the same way. In the
 * emitted-quad freeze mode a freeze that drops on a scene with no world
 * geometry (the pause-to-exit handoff tick) is held, and that scene's
 * geometry is dropped so only the held freeze shows. Clears both one-tick
 * seen flags. A repeat present (openpete_native_scene_canon_repeat) keeps
 * both words, with the previous equal to the current; the first present
 * after a run of repeats rolls from no previous word. */
void openpete_native_scene_commit_fade(void) OP_REQUIRES(sim);

/** @brief Write the offscreen target to a PNG.
 *
 * The offscreen holds whatever the last present blitted: the native image
 * in vk mode and PsyCross's read-back image in psycross mode, chrome and
 * postfx included. Call after openpete_native_render_frame so it holds the
 * latest image. Alpha is forced opaque.
 * @param path Output PNG path.
 * @param what Label for the log line; NULL reads as offscreen.
 * @return 0 on success, nonzero when native render is disabled, @p path is
 *         NULL, or the readback or write fails. */
int openpete_native_render_screenshot(const char *path, const char *what) OP_REQUIRES(sim);

/** @brief Write the offscreen target to a PNG and report the written extent.
 *
 * As openpete_native_render_screenshot. The extent lets a caller that
 * captures a sequence of presents state each present's extent and detect a
 * sequence whose images are not comparable.
 * @param path Output PNG path.
 * @param what Label for the log line; NULL reads as offscreen.
 * @param out_w Receives the image width, or 0 when nothing was written; may be NULL.
 * @param out_h Receives the image height, or 0 when nothing was written; may be NULL.
 * @return 0 on success, nonzero when native render is disabled, @p path is
 *         NULL, or the readback or write fails. */
int openpete_native_render_screenshot_ext(const char *path, const char *what,
                                          int *out_w, int *out_h) OP_REQUIRES(sim);

/** @brief Read the offscreen target back and queue it for PNG encoding and writing off the calling thread.
 *
 * As openpete_native_render_screenshot, except that only the readback (a
 * device wait and a copy) runs on the calling thread; the encode and the
 * file write run on the PNG writer thread (openpete_write_png_rgba_async),
 * which logs the line once the file is committed.
 * @param path Output PNG path.
 * @param what Label for the log line; NULL reads as offscreen.
 * @return 0 when the image was read back and queued, nonzero when native
 *         render is disabled, @p path is NULL, or the readback fails. */
int openpete_native_render_screenshot_async(const char *path, const char *what) OP_REQUIRES(sim);

/** @brief Drain the --record readback ring's queued tail images to the encoder.
 *
 * The ring still holds the last few presents when the run loop ends; they
 * are submitted in PTS order so openpete_record_shutdown finalizes a file
 * with its tail. Call once, after the run loop, while the native device is
 * alive. Blocks on the readbacks. No-op unless native render is enabled and
 * recording is active. */
void openpete_native_render_record_flush(void);

/** @brief Write the native view of one --screenshot-on-frame target as the -vk image.
 *
 * The path is @p base with -vk inserted before the extension. In a windowed
 * run the -vk image is a readback of the just-presented offscreen image,
 * which shows what the window showed (sub-tick scene, mods, postfx,
 * overlays). Headless, and an intermediate capture armed by
 * openpete_native_render_set_capture_nodelay, recompose the scene instead:
 * headless composes the newest tick's canonical scene, which the -og
 * capture, deferred one present, also shows. Psycross mode, which has no
 * native scene, writes the readback and stops. The reference (-og) is
 * captured by the caller.
 * Applies a pending truescope change first, since headless never runs
 * openpete_native_render_frame. Restores the live layout afterwards.
 * @param base Output path before the -vk suffix.
 * @return 0 when every image was written, nonzero otherwise, including when
 *         native render is disabled. */
int openpete_native_render_screenshot_all(const char *base) OP_REQUIRES(sim);

/** @brief Compose the screenshot scene into a malloc'd RGBA buffer instead of a PNG.
 *
 * The in-memory form of openpete_native_render_screenshot_all: the same
 * capture target, widescreen widen and scene selection, always recomposed
 * (no live readback), top-down, alpha forced opaque. Fails in psycross mode,
 * which has no native scene.
 * @param out Receives the buffer, which the caller frees; NULL on failure.
 * @param w Receives the capture width; may be NULL.
 * @param h Receives the capture height; may be NULL.
 * @return 0 on success, nonzero on failure or when @p out is NULL. */
int openpete_native_render_capture_rgba(unsigned char **out, int *w, int *h) OP_REQUIRES(sim);

/** @brief Save the accumulating scene and its per-frame scalars.
 *
 * A caller that drives a re-extract only to inspect its output brackets it
 * with this and openpete_native_scene_restore: the re-extract rebuilds the
 * scene in place, and the next begin_frame would otherwise snapshot the
 * rebuilt scene into the frame-history ring in place of the canonical one.
 * @return 0 on success, 1 when the save buffer could not be sized. */
int  openpete_native_scene_save(void) OP_REQUIRES_SHARED(sim);
/** @brief Restore the scene openpete_native_scene_save saved, with its moved-to-ring-slot mark; no-op when nothing is saved. */
void openpete_native_scene_restore(void) OP_REQUIRES(sim);

/** @brief Make the screenshot compose read the current scene instead of the lagged history slot.
 *
 * For an intermediate (sub-tick) capture: the re-extract builds the
 * interpolated scene into the current scene and never pushes it to the
 * history ring, so the compose reads ring depth 0 (the current scene; an
 * OPENPETE_NATIVE_SCENE_DELAY override still wins). While on, the compose
 * also skips the live readback and recomposes. Set on before the dump and
 * off after; the canonical dump leaves it off.
 * @param on Nonzero to compose the current scene. */
void openpete_native_render_set_capture_nodelay(int on);

/** @brief Note a headless present for the capture freeze backdrop: its widen when it is a source, a late render when it is not.
 *
 * The canonical capture of every freeze-source scene pins it as the
 * backdrop source; a headless capture renders the pin into its own backdrop
 * when a compose or a present that is not a source follows
 * (openpete_native_render_snapshot_present_begin,
 * openpete_native_render_snapshot_settle), so a later dump of a freeze tick
 * (pause, flight results, level exit) shows the green frozen-world
 * backdrop. A source present records the capture widen the render takes;
 * a present that reads as a source only before its services renders the
 * pin here. No-op with a window, where the live present renders the pin
 * itself. Call each present during a freeze-backdrop capture run.
 * @param cap_wf The capture's widescreen factor
 *        (openpete_native_widescreen_factor); values below 1 read as 1. */
void openpete_native_render_snapshot_frozen(float cap_wf) OP_REQUIRES(sim);

/** @brief Render the pinned freeze-backdrop source at the start of a present that is not a source.
 *
 * Headless only, when a pin is newer than the capture backdrop's render. A
 * present that is a source leaves the render for a later present or
 * compose. Call at the start of every present of a freeze-backdrop capture
 * run, before the present's terrain-cache and occlusion services. */
void openpete_native_render_snapshot_present_begin(void) OP_REQUIRES(sim);

/** @brief Render the pinned freeze-backdrop source into the capture backdrop now, when a headless run owes it.
 *
 * The capture composes call it themselves; a caller that changes the
 * scene or the terrain state before a compose (the intermediate capture's
 * re-extract) calls it first. */
void openpete_native_render_snapshot_settle(void) OP_REQUIRES_SHARED(sim);

/** @brief Give every pending framebuffer window and draw area a crop of the native image.
 *
 * Each pending window (see openpete_fb_snapshot.h) is composed in memory
 * from the scene of the tick its copy was taken on and cropped to the rect
 * the game's MoveImage copied, so the quad re-displaying that copy draws the
 * native image instead of an emulated framebuffer the native render never
 * writes. A window or area whose compose fails is handed back with
 * openpete_fb_snapshot_resolve_failed: its image, which stands for an
 * earlier copy, is dropped and the next call retries it.
 * OPENPETE_FBSNAP_RESOLVE_FAIL=N fails the Nth window compose of the run
 * (fault injection). Runs on the host loop with the display in hand, after
 * the present. No-op when native render is off or nothing is pending. */
void openpete_native_render_snapshot_fb_window(void) OP_REQUIRES(sim);

/** @brief Log the run's engagement summaries and tear down the device and window.
 *
 * Always logs the scene box summary and the sub-tick view engagement; the
 * pipeline depth report needs a device. When native render is enabled it
 * then waits the device idle and destroys the imgui backend, the terrain
 * cache, the mod postfx and material objects, every target, buffer and
 * pipeline, the device and the window. Safe to call unconditionally. */
void openpete_native_render_shutdown(void);

/* 1 while the canonical present's lagged re-extract runs (run.c), so
 * feeder-side diagnostics tag the present leg of a sample. */
extern int g_openpete_canon_lag_leg;

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_NATIVE_RENDER_H */
