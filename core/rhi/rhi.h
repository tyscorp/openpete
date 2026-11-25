#ifndef OPENPETE_RHI_H
#define OPENPETE_RHI_H

/*
 * openpete RHI - thin Render Hardware Interface.
 *
 * A minimal, opaque-handle GPU abstraction sized to exactly what the
 * native Spyro renderer needs, so Vulkan (the first backend) can later be
 * joined by Metal / WebGPU without touching renderer code. It grows
 * function-by-function as the renderer requires; it is deliberately
 * NOT a general render graph.
 *
 * Surface (this file): device + swapchain, an offscreen color target
 * with depth, buffers, pipelines, textures over the native VRAM model,
 * uniform blocks, a blit of the offscreen target onto the swapchain, and
 * present.
 *
 * The offscreen-target -> blit -> present structure: the 3D scene renders
 * INTO the offscreen target, the 2D layer composites over it, framebuffer
 * effects (StoreImage) read back from it, and only then is it blitted to
 * the swapchain.
 *
 * Threading: the thread that owns the display (rhi_display_claim) makes
 * the calls; rhi_compile_job_run, rhi_compile_job_discard and the entry
 * points documented as safe on any thread are the exceptions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct SDL_Window;

typedef struct rhi_device rhi_device;

/** @brief Create a windowed device against an SDL window created with SDL_WINDOW_VULKAN.
 *
 * The calling thread claims the display (rhi_display_claim) and presents
 * until a handoff. `vsync` selects the swapchain present mode: 1 forces FIFO
 * (vsync on, present locked to the monitor refresh), 0 forces a vsync-off
 * mode (immediate, else mailbox: uncapped throughput, low latency), -1 leaves
 * it unset (the OPENPETE_PRESENT_MODE knob, read through rhi_set_knob_source's
 * lookup, decides, else FIFO). A vsync-off
 * request the surface does not support falls back to FIFO with a log.
 * @param window SDL window created with SDL_WINDOW_VULKAN.
 * @param app_name Application name for VkApplicationInfo; NULL selects
 *                 "openpete".
 * @param vsync Present-mode preference: 1, 0 or -1.
 * @param gpu The physical device to use: an enumeration index (an all-digit
 *            value, "0") or a case-insensitive substring of its deviceName
 *            ("radeon"). Every
 *            enumerated device is logged, then the choice and why. A value
 *            that matches no device, or names one without a graphics queue
 *            that presents to the window, is logged and the automatic pick
 *            (discrete, integrated, virtual, other, then CPU) decides, as it
 *            does for NULL or "".
 * @return The device, or NULL on failure (a diagnostic is logged to stderr
 *         and rhi_create_error names the failed step). */
rhi_device *rhi_create(struct SDL_Window *window, const char *app_name, int vsync,
                       const char *gpu);

/** @brief Change the swapchain present-mode preference (vsync) at runtime.
 *
 * `vsync` has rhi_create's meaning: 1 forces FIFO (vsync on), 0 a vsync-off
 * mode (immediate, else mailbox), -1 leaves it unset (the environment
 * decides, else FIFO). The preference is stored and the swapchain flagged for
 * recreation; the next rhi_begin_frame rebuilds it with the present mode
 * choose_present_mode resolves. An unchanged preference does nothing; a
 * headless device stores it and flags no rebuild. Touches no gameplay state.
 * @param dev Device, or NULL (ignored).
 * @param vsync Present-mode preference: 1, 0 or -1. */
void rhi_set_vsync(rhi_device *dev, int vsync);

/** @brief Change the env spelling of the present mode ("immediate", "mailbox", "" for unset) at runtime.
 *
 * Read when the vsync preference is -1. The name is stored and the swapchain
 * flagged for recreation as rhi_set_vsync does; an unchanged name does
 * nothing. Touches no gameplay state.
 * @param dev Device, or NULL (ignored).
 * @param mode The mode name, or NULL for unset. */
void rhi_set_present_mode_name(rhi_device *dev, const char *mode);

/** @brief Install the lookup the RHI reads its device-creation knobs through.
 *
 * The knobs are the env vars read while the instance, the device, the
 * swapchain, the pipeline cache and the timer pools are created
 * (OPENPETE_VK_VALIDATION, OPENPETE_RHI_DEPTH_D24, OPENPETE_PRESENT_MODE,
 * OPENPETE_PRESENT_TIMING, OPENPETE_PACE_FLIP_LOG, OPENPETE_EAGER_LATTICE,
 * OPENPETE_PIPE_EXTS, OPENPETE_GPU_TIMERS, OPENPETE_RHI_BALLOON, OPENPETE_RHI_COMPUTE,
 * OPENPETE_PIPELINE_CACHE_DIR, OPENPETE_PIPELINE_CACHE_COLD). Without a
 * lookup they are read with getenv. Process-global; call before rhi_create.
 * @param fn Returns a knob's value or NULL when unset; NULL restores getenv. */
void rhi_set_knob_source(const char *(*fn)(const char *name));

/** @brief On Windows, write the Vulkan loader's environment before anything loads the loader; elsewhere do nothing.
 *
 * Sets DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1: on laptops pairing an AMD
 * iGPU with an NVIDIA dGPU, an outdated VK_LAYER_AMD_switchable_graphics
 * implicit layer makes vkEnumeratePhysicalDevices report no device, and the
 * layer's only function, choosing between the GPUs, is done by the device
 * pick. The loader reads the variable at every implicit-layer scan, the first
 * of which SDL performs while creating an SDL_WINDOW_VULKAN window
 * (SDL_Vulkan_LoadLibrary queries the instance extensions), so every such
 * window creation and the instance creation call this first. A variable the
 * process environment already carries is kept, so the call is idempotent;
 * otherwise the value @p user_value returns is written, else "1".
 * @param user_value Returns the value the user configured for a variable, or
 *                   NULL when none; NULL reads only the process environment. */
void rhi_loader_env_prepare(const char *(*user_value)(const char *name));

/** @brief Return the device's vsync preference as rhi_create or rhi_set_vsync last received it.
 *
 * The preference is returned unresolved: -1 means the environment decides
 * and FIFO is the fallback, and the resolved present mode is not read here.
 * @param dev Device, or NULL.
 * @return 1 for FIFO, 0 for a vsync-off mode, -1 when unset or @p dev is
 *         NULL. */
int rhi_get_vsync(const rhi_device *dev);

/** @brief Create a surfaceless device: no window, no swapchain, no present.
 *
 * The device renders into the offscreen and render-target images only;
 * pixels are read back with rhi_read_render_target or rhi_read_offscreen.
 * The offscreen color format is B8G8R8A8_UNORM, the windowed preference, so
 * the readback swizzle is the same on both. The calling thread claims the
 * display. Used for headless capture and CI visual regression under a
 * software rasterizer (lavapipe).
 * @param app_name Application name for VkApplicationInfo; NULL selects
 *                 "openpete".
 * @param width Offscreen extent width (the would-be window width); 0 is
 *              raised to 1.
 * @param height Offscreen extent height; 0 is raised to 1.
 * @param gpu The physical device to use, as rhi_create takes it; a headless
 *            device needs only a graphics queue.
 * @return The device, or NULL on failure. */
rhi_device *rhi_create_headless(const char *app_name, uint32_t width, uint32_t height,
                                const char *gpu);

/** @brief Return whether the device was created by rhi_create_headless.
 *
 * Such a device's begin and end frame submit work without acquiring or
 * presenting a swapchain image, and rhi_blit_to_swapchain does nothing.
 * @param dev Device, or NULL.
 * @return true for a headless device; false for a windowed one or NULL. */
bool        rhi_is_headless(const rhi_device *dev);

/** @brief Wait the device idle and destroy every object the RHI owns, then the device and the instance.
 *
 * Textures, pipelines and UBOs created by callers are destroyed by the
 * callers first. NULL is ignored.
 * @param dev Device to destroy. */
void        rhi_destroy(rhi_device *dev);

/** @brief Return the GPU's maximum 2D image dimension (VkPhysicalDeviceLimits.maxImageDimension2D).
 *
 * The renderer steps the SSAA factor down until the supersampled render
 * target fits this bound, so a large --ssaa factor does not fail image
 * creation.
 * @param dev Device, or NULL.
 * @return The limit in texels; 0 for NULL. */
uint32_t    rhi_max_image_dimension(const rhi_device *dev);

/** @brief Block until the GPU is idle (vkDeviceWaitIdle).
 *
 * Called before destroying buffers or pipelines that recent frames may
 * still be using.
 * @param dev Device, or NULL (ignored). */
void rhi_wait_idle(rhi_device *dev);

/** @brief Return whether a frame's command buffer is recording.
 *
 * True between a successful rhi_begin_frame or rhi_begin_frame_staged and
 * the frame's end and submit. Descriptor sets, framebuffers and images the
 * command buffer has already referenced must not be destroyed until it has
 * been submitted and retired; resource (re)creation belongs before the frame
 * begins.
 * @param dev Device, or NULL.
 * @return true while recording; false otherwise or for NULL. */
bool rhi_frame_active(const rhi_device *dev);

/** @brief Begin a frame: retire the frame slot's previous submission, acquire a swapchain image, and open the command buffer.
 *
 * Headless waits on the slot's fence and acquires no image. Windowed polls
 * the fence (bounded wait under a blocking presenter) and returns false
 * instead of blocking when it has not retired; it also returns false inside
 * the acquire back-off window, when the swapchain cannot be rebuilt yet (a
 * pending recreation, a live resize, a minimized window) and when the acquire
 * is refused. The caller skips rendering on false; recreation is handled
 * inside the device. In capture mode (rhi_set_capture_mode) the fence is
 * waited and the frame begins without an image in each of those cases.
 * @param dev Device to begin the frame on.
 * @return true when a command buffer is recording. */
bool rhi_begin_frame(rhi_device *dev);
/** @brief Begin a frame that renders into targets only: no swapchain image is acquired and none is presented.
 *
 * For a compose read back from its own render target. The swapchain is not
 * checked or rebuilt and the acquire back-off is not consulted. The slot
 * fence is waited without bound headless and in capture mode, and up to
 * RHI_BLOCKING_FENCE_TIMEOUT_NS otherwise, where rhi_begin_frame would poll
 * it; an expired wait counts in fence_sheds and refuses. A device loss
 * refuses. End the frame with rhi_end_frame, which submits it without a
 * present; rhi_blit_to_swapchain does nothing in it.
 * @param dev Device to begin the frame on.
 * @return true when a command buffer is recording. */
bool rhi_begin_frame_offline(rhi_device *dev);
/** @brief Return whether the swapchain must be rebuilt before the next present.
 *
 * The rebuild touches the window and calls SDL, so a thread that is not the
 * window's owner declines to present rather than drive it.
 * @param dev Device, or NULL.
 * @return true when a rebuild is pending; false otherwise or for NULL. */
bool rhi_needs_recreate(const rhi_device *dev);
/** @brief Return the number of frames that began since process start.
 *
 * A frame counts when rhi_begin_frame or rhi_begin_frame_staged returned true
 * (slot fence retired, image acquired where one is, command buffer
 * recording); a refused begin does not count, so a frame
 * RHI_FRAMES_IN_FLIGHT serials back has had its fence waited. A pass that
 * writes its ring vertex buffer more than once per frame (one batch per
 * render target) bump-allocates within the buffer and resets its offset when
 * this changes: a second same-frame rhi_update_buffer to the same offset
 * clobbers data a recorded but unexecuted pass reads. The counter is global,
 * not per device.
 * @param dev Unused.
 * @return The monotonic serial. */
uint64_t rhi_frame_serial(rhi_device *dev);
/** @brief Select whether frame begins block on the slot fence and the swapchain acquire.
 *
 * On, rhi_begin_frame waits the slot fence up to
 * RHI_BLOCKING_FENCE_TIMEOUT_NS, the image fence gets the same bound, and a
 * timed-out acquire opens no back-off window: for a thread whose only job is
 * presenting. Off, the slot fence is polled and a busy slot or an acquire
 * back-off sheds the present, so the display never paces the calling
 * thread. A timed-out bounded wait still sheds; proceeding past it would
 * re-record a command buffer the GPU is executing.
 * @param dev Device, or NULL (ignored).
 * @param on true to block, false to poll and shed. */
void rhi_set_present_blocking(rhi_device *dev, bool on);
/** @brief Select offline capture mode, in which every frame begin succeeds short of a device loss and the swapchain present is best effort.
 *
 * On, rhi_begin_frame waits the slot fence and the image fence without
 * bound, the acquire does not wait for an image, and a frame whose
 * swapchain is not usable (rebuild pending, resize settling, back-off
 * window, acquire refused) still begins without an image:
 * rhi_blit_to_swapchain then does nothing and rhi_end_frame submits without
 * a present, so a readback queued on the frame is still taken. For a
 * capture timed by the frames it renders rather than by the wall clock;
 * the display never paces it. Ignored on a headless device, which already
 * waits.
 * @param dev Device, or NULL (ignored).
 * @param on true for capture mode. */
void rhi_set_capture_mode(rhi_device *dev, bool on);
/** @brief Return the refresh rate of the display the current swapchain was built for.
 *
 * Read from SDL_GetCurrentDisplayMode for the window's display at each
 * swapchain (re)creation, so it follows the window across monitors.
 * @param dev Device, or NULL.
 * @return The rate in Hz; 0 when unknown, headless, or for NULL. */
float rhi_refresh_hz(const rhi_device *dev);
/** @brief Harness: answer the device's refresh query with @p hz instead of the display's mode.
 *
 * Read at each swapchain (re)creation and when a display change of the
 * window (SDL_EVENT_WINDOW_DISPLAY_CHANGED) is consumed; a changed answer
 * then rebuilds the swapchain, so a synthetic display change exercises the
 * path a move between displays takes.
 * @param dev Device, or NULL (ignored).
 * @param hz The stubbed rate in Hz; 0 restores the display's. */
void rhi_set_refresh_stub(rhi_device *dev, float hz);
/** @brief Return whether the granted present mode is FIFO or FIFO_RELAXED (vsync).
 *
 * Under those modes the display shows at most one image per refresh, so
 * presenting faster than that only queues.
 * @param dev Device, or NULL.
 * @return true for a windowed device under FIFO or FIFO_RELAXED; false
 *         otherwise, headless, or for NULL. */
bool  rhi_present_is_vsync(const rhi_device *dev);
/* Depth of the frame ring: a frame's fence is waited by the begin_frame this
 * many frames later, which is when its GPU reads are known to be over.
 * Overridable from the build (the depth/refresh trade sweep). */
#ifndef RHI_FRAMES_IN_FLIGHT
#define RHI_FRAMES_IN_FLIGHT 2
#endif

/** @brief Begin the offscreen pass, clearing color to (r, g, b, a) and depth to the scene far value.
 *
 * Draw calls are recorded between this and rhi_end_offscreen. The pass sets a
 * full-extent Y-flipped viewport and scissor and a zero depth bias. It does
 * nothing outside an active frame.
 *
 * off_image is one instance shared by all frames in flight: frame N+1's
 * clear-on-load waits for frame N's reads through the render pass's EXTERNAL
 * source dependency, so back-to-back frames serialize on it. A per-slot ring
 * would have to thread the current slot through rhi_read_offscreen and the
 * swapchain blit, which also read it.
 * @param dev Device whose current frame records the pass.
 * @param r Clear red, linear 0..1.
 * @param g Clear green, linear 0..1.
 * @param b Clear blue, linear 0..1.
 * @param a Clear alpha, 0..1. */
void rhi_begin_offscreen(rhi_device *dev, float r, float g, float b, float a);
/** @brief End the offscreen pass begun by rhi_begin_offscreen or rhi_begin_offscreen_load.
 *
 * The pass's final layout leaves off_image in TRANSFER_SRC_OPTIMAL, ready for
 * the swapchain blit and readbacks. Does nothing outside an active frame.
 * @param dev Device whose current frame records the end. */
void rhi_end_offscreen(rhi_device *dev);
/** @brief Re-begin the offscreen pass loading off_image's existing contents instead of clearing them.
 *
 * Used at the SCENE postfx point: after a SCENE-point chain has
 * post-processed the scene into off_image (leaving it TRANSFER_SRC), the
 * image is transitioned back to a color attachment and drawing (the HUD)
 * resumes over it. Depth clears to the scene far value. Pairs with
 * rhi_end_offscreen; does nothing outside an active frame.
 * @param dev Device whose current frame records the pass. */
void rhi_begin_offscreen_load(rhi_device *dev);

/** @brief Blit the offscreen color target onto the acquired swapchain image with a linear filter.
 *
 * The swapchain image is transitioned UNDEFINED to TRANSFER_DST before the
 * blit and to PRESENT_SRC after it; off_image is read in the TRANSFER_SRC
 * layout its render pass leaves. Does nothing outside an active frame or
 * when the frame acquired no image (headless, staged, or a capture-mode
 * frame the swapchain could not take).
 * @param dev Device whose current frame records the blit. */
void rhi_blit_to_swapchain(rhi_device *dev);

/** @brief Submit the recorded command buffer and present the swapchain image.
 *
 * The submit waits on the frame's acquire semaphore and signals the image's
 * render-done semaphore, which the present waits on; the slot's in_flight
 * fence gates the next reuse of the slot. OUT_OF_DATE and SUBOPTIMAL set
 * needs_recreate; DEVICE_LOST invokes the device-lost handler. A frame that
 * acquired no image (headless, or capture mode) submits with no semaphores
 * and no present. Does nothing outside an active
 * frame. The calling thread is verified against the display owner.
 * @param dev Device whose current frame is ended. */
void rhi_end_frame(rhi_device *dev);

/* Staged presents: the render and the present as two steps, so a frame can
 * be rendered ahead of the instant it is meant to appear and the work left
 * at that instant is an acquire and a blit. rhi_end_frame_to_stage replaces
 * rhi_blit_to_swapchain + rhi_end_frame: it blits the offscreen target into
 * stage image `stage` (0..rhi_stage_count()-1) and submits, acquiring
 * nothing. rhi_present_stage acquires a swapchain image, blits the stage
 * image onto it and presents; false = refused (no image within the acquire
 * budget, a rebuild pending, or the stage image was never filled / was
 * rebuilt since -- compare rhi_stage_gen). A staged frame that is never
 * presented costs nothing: no semaphore crosses the two steps, only queue
 * submission order and image barriers. For the dedicated presenter; the
 * fence and acquire waits here block. Headless: not available. */
/** @brief Begin a frame that will end in a stage image: rhi_begin_frame without the swapchain acquire.
 *
 * No swapchain image is acquired because the frame ends in
 * rhi_end_frame_to_stage: an acquire whose semaphore no submission waits on
 * stays signalled, and the swapchain runs out of acquirable images.
 * @param dev Device to begin the frame on.
 * @return true when a command buffer is recording; always false headless. */
bool     rhi_begin_frame_staged(rhi_device *dev);   /* rhi_begin_frame without the acquire */
/** @brief Return the number of stage images in the staged-present ring.
 *
 * @return RHI_STAGE_RING; a compile-time constant independent of any device. */
int      rhi_stage_count(void);
/** @brief Return the stage-image generation, which advances when the stage ring is destroyed for a rebuild.
 *
 * A stage image filled under one generation is invalid under the next.
 * @param dev Device, or NULL.
 * @return The generation; 0 for NULL. */
uint32_t rhi_stage_gen(const rhi_device *dev);
/** @brief End a staged frame: blit the offscreen target into a stage image and submit, acquiring nothing.
 *
 * Headless, or with an out-of-range or absent stage image, ends the frame
 * with rhi_end_frame instead. A failed submit reaches the device-lost
 * handler.
 * @param dev Device whose current frame is ended.
 * @param stage Stage image index, 0..rhi_stage_count()-1. */
void     rhi_end_frame_to_stage(rhi_device *dev, int stage);
/** @brief Acquire a swapchain image, blit a stage image onto it and present it, in one call.
 *
 * Runs rhi_present_stage_acquire, then rhi_present_stage_submit only when the
 * acquire succeeded; an image already held is reused.
 * @param dev Device whose display the calling thread owns.
 * @param stage Stage image index, 0..rhi_stage_count()-1.
 * @return true when the present was queued; false when either step
 *         refuses. */
bool     rhi_present_stage(rhi_device *dev, int stage);
/** @brief Give back a held acquire by presenting the image's previous contents unchanged.
 *
 * @param dev Device whose display the calling thread owns.
 * @return false when the submit fails, the device is lost, or the present
 *         reports device loss; true otherwise, including when nothing was
 *         held or the swapchain was rebuilt since the acquire. */
bool     rhi_present_stage_release(rhi_device *dev);
/** @brief Make the calling thread the display owner.
 *
 * The runtime half of the display-seam fence: the thread that will present
 * next claims the display at each handoff, and every frame and present entry
 * verifies its caller against the owner. A violation logs (the first eight
 * in full) and counts in rhi_seam_violations; OPENPETE_SEAM_GUARD_FATAL=1
 * aborts at the first, for the stress harnesses. No check runs before the
 * first claim. The owner is global, not per device.
 * @param dev Unused. */
void     rhi_display_claim(rhi_device *dev);
/** @brief Return the number of frame or present entries called from a thread other than the display owner.
 *
 * Global and monotonic; counts every violation, while the log carries the
 * first eight.
 * @return The running count. */
unsigned rhi_seam_violations(void);
/** @brief Acquire the swapchain image for a later rhi_present_stage_submit: rhi_present_stage in two steps.
 *
 * The image is acquired early, right after the render while the display is
 * still on the previous frame, and the blit and present are submitted at the
 * display instant. An image the display released only at the last vblank is
 * not available until the driver has processed that release, and a present
 * issued on it blocks until then (measured 1.3 to 1.7 ms). The present ring
 * slot's fence is waited up to RHI_BLOCKING_FENCE_TIMEOUT_NS first; a timeout
 * counts a fence shed. The call is idempotent while an image is held;
 * rhi_present_stage_submit consumes the hold, and a swapchain rebuild in
 * between voids it.
 * @param dev Device whose display the calling thread owns.
 * @return true when an image is held; false headless, inside an active
 *         frame, when the swapchain cannot be rebuilt, or when the fence
 *         wait or the acquire fails. */
bool     rhi_present_stage_acquire(rhi_device *dev);
/** @brief Blit a stage image onto the held swapchain image and present it.
 *
 * Acquires first when no image is held. A failed submit or a device-lost
 * present reaches the device-lost handler.
 * @param dev Device whose display the calling thread owns.
 * @param stage Stage image index, 0..rhi_stage_count()-1.
 * @return true when the present was queued; false headless, inside an
 *         active frame, after a device loss, for a stage that was never
 *         filled or was rebuilt, or when the acquire, submit or present fails. */
bool     rhi_present_stage_submit(rhi_device *dev, int stage);

/** @brief Return whether display timing (VK_KHR_present_id and VK_KHR_present_wait) is enabled.
 *
 * Both extensions must be offered. Every other timestamp in this engine is
 * a submit time; under FIFO the display quantises submits to its own
 * refresh, so submit spacing can be even while the flips are not.
 * rhi_present_wait blocks until the last queued present has been displayed,
 * up to timeout_ns: 1 = displayed (the caller's clock at return is the flip
 * time, within thread-wakeup jitter), 0 = still not displayed, -1 =
 * unsupported or nothing outstanding. It is called only from the thread
 * that presents: the swapchain is externally synchronized, and a wait on
 * another thread races vkQueuePresentKHR.
 * @param dev Device, or NULL.
 * @return 1 when supported; 0 otherwise or for NULL. */
int  rhi_present_timing_supported(const rhi_device *dev);
/** @brief Wait for queued presents to be displayed, oldest id first, and report the one that blocked.
 *
 * Waits on the oldest unconfirmed present id with vkWaitForPresentKHR for the
 * time left of timeout_ns, then continues with the next id while the wait
 * returns without blocking (a flip that already happened), up to 8 ids per
 * call. Returns at the first id whose wait blocked for at least
 * RHI_FLIP_OBSERVED_NS: the caller's clock at return is that flip. Only the
 * presenting thread calls it. Timeouts under about a millisecond return
 * before the flip on some drivers, so the call is not a poll.
 * @param dev Device, or NULL.
 * @param timeout_ns Total wait budget in nanoseconds.
 * @param out_id Receives the last id confirmed by this call; may be NULL.
 * @param out_waited_ns Receives the time that wait blocked; may be NULL.
 * @return 1 when a blocking wait confirmed an id; 0 on timeout or when the
 *         drain limit ended on already-displayed ids; -1 without present
 *         timing, without a swapchain, with a rebuild pending, with no
 *         outstanding id, or on a wait error. */
int  rhi_present_wait(rhi_device *dev, uint64_t timeout_ns, uint64_t *out_id,
                      uint64_t *out_waited_ns);
/** @brief Return the number of presents queued but not yet confirmed displayed by rhi_present_wait.
 *
 * The difference between the last submitted present id and the newest id
 * rhi_present_wait saw displayed. Same thread rule as rhi_present_wait.
 * @param dev Device, or NULL.
 * @return The count; 0 without present timing, without a swapchain, or for
 *         NULL. */
int  rhi_present_outstanding(const rhi_device *dev);

/** @brief Flag a swapchain rebuild after a window size change.
 *
 * The swapchain and offscreen target are recreated at the next
 * rhi_begin_frame, after the settle window that absorbs the per-pixel steps
 * of a live drag. Safe to call on every resize event.
 * @param dev Device, or NULL. */
void rhi_notify_resize(rhi_device *dev);
/** @brief Flag a swapchain rebuild for a size change the app caused, such as a fullscreen toggle.
 *
 * Unlike rhi_notify_resize, the rebuild runs at the next begin_frame instead
 * of waiting out the settle window that absorbs a live drag's per-pixel
 * steps. The recreate and acquire back-off windows are cleared too: a toggle
 * is not the sustained OUT_OF_DATE storm the back-off damps.
 * @param dev Device, or NULL (ignored). */
void rhi_notify_resize_immediate(rhi_device *dev);

/** @brief Start a present-extent trace window when OPENPETE_PRESENT_EXTENT_TRACE is set.
 *
 * From the call, every present logs the window's pixel size, the swapchain
 * extent, its source (launcher frame, game scene, stage release) and its
 * result, up to a budget of lines; every swapchain rebuild and every window
 * pixel-size and fullscreen event logs as well, on one clock relative to the
 * arm. The first present whose extent equals the window after a pixel-size
 * event logs the interval from that event and the count of mismatched
 * presents since the arm. Until a game present holds non-black pixels
 * (30 s cap), each game present is read back after a device-idle wait and
 * the first with any and with at least 1% non-black pixels log their delay
 * after the first game present. Unset, the call does nothing; headless does
 * nothing.
 * @param dev Device, or NULL (ignored).
 * @param why Label printed on the arm line. */
void rhi_present_trace_arm(rhi_device *dev, const char *why);

/** @brief Register the handler called when the device is lost.
 *
 * A loss is a VK_ERROR_DEVICE_LOST from a fence wait, swapchain acquire,
 * present or stage release, or a failed queue submit. rhi_signal_device_lost
 * logs the loss and the last GPU zone begun, then calls the handler once;
 * later reports are ignored. It lets the owning layer request a clean
 * shutdown while the RHI stays free of core-runtime symbols. The handler is
 * global, not per device.
 * @param fn Handler, or NULL to clear it. */
void rhi_set_device_lost_handler(void (*fn)(void));

/** @brief Return the selected physical device's name.
 *
 * @param dev Device, or NULL.
 * @return The name, valid until rhi_destroy; "" for NULL. */
const char *rhi_device_name(const rhi_device *dev);

/** @brief Return the present mode the swapchain was granted, as a short name.
 *
 * The requested mode does not establish the granted one: a surface that
 * does not advertise IMMEDIATE falls back to FIFO, and a pacing baseline
 * taken against the wrong mode is not comparable.
 * @param dev Device, or NULL.
 * @return "immediate", "mailbox", "fifo", "fifo-relaxed" or "other"; "none"
 *         for NULL. */
const char *rhi_present_mode_name(const rhi_device *dev);

/** @brief Return the selected device's driver version, decoded the way its vendor packs it.
 *
 * NVIDIA's driverVersion decodes as four fields (10.8.8.6 bits), every other
 * vendor's as the Vulkan major.minor.patch triple. Part of a pacing
 * baseline's environment key: a driver update changes present behaviour and
 * must not compare as the same machine.
 * @param dev Device, or NULL.
 * @return The version string; "unknown" for NULL or when none was recorded. */
const char *rhi_driver_version(const rhi_device *dev);

/* Present-path block time, for the pacing log. Under the host thread split
 * the presenting thread is also the sim thread, so time blocked here is time
 * the tick did not run - which makes "which call blocked, and for how long"
 * a pacing quantity rather than a rendering one. Read-and-clear: each call
 * returns the nanoseconds accumulated since the previous one. */
enum { RHI_BLOCK_FENCE = 0, RHI_BLOCK_ACQUIRE, RHI_BLOCK_PRESENT,
       RHI_BLOCK_RECREATE, RHI_BLOCK_IMAGE_FENCE, RHI_BLOCK_PHASE_COUNT };
/** @brief Read and clear the nanoseconds each present-path phase blocked since the previous call.
 *
 * The accumulators are process globals shared by every device. Phases past
 * RHI_BLOCK_PHASE_COUNT are not written.
 * @param out Receives one value per phase, indexed by the RHI_BLOCK_* enum.
 * @param n Entries of @p out; at most RHI_BLOCK_PHASE_COUNT are filled. */
void rhi_take_block_times(int64_t *out, int n);
/** @brief Read the swapchain image index and extent of the last game present and the presents made so far.
 *
 * Written at each vkQueuePresentKHR of a frame or a stage (the release of
 * an unused stage image is not counted). Read on the presenting thread.
 * @param image_index Receives the image index, UINT32_MAX before the first present.
 * @param width Receives the swapchain width at that present.
 * @param height Receives the swapchain height at that present.
 * @param presents Receives the count of presents. */
void rhi_last_present(uint32_t *image_index, uint32_t *width, uint32_t *height, uint64_t *presents);

/** @brief Return the number of presents skipped because a fence had not retired.
 *
 * Counts a begin_frame whose slot fence had not signalled (a poll that found
 * it busy, or a blocking presenter's bounded wait that expired) and a staged
 * present whose ring slot fence wait expired. Windowed only, and never in
 * capture mode: rather than block the presenting thread on the display's
 * cadence, which under the host thread split is also the sim thread, the
 * frame is dropped. Monotonic; the
 * caller diffs it.
 * @param dev Device, or NULL.
 * @return The running count; 0 for NULL. */
uint64_t rhi_fence_sheds(const rhi_device *dev);

/** @brief Return the number of presents the acquire path refused.
 *
 * Counts both an acquire that expired its budget and a present skipped
 * inside the back-off window such an expiry opens; rhi_acquire_timeouts()
 * counts the expiries alone. Together they separate an acquire that waited
 * and got an image from one that waited, gave up, and suppressed the
 * following presents. Monotonic; the caller diffs it.
 * @param dev Device to query; NULL returns 0.
 * @return The running count. */
uint64_t rhi_acquire_sheds(const rhi_device *dev);
/** @brief Return the fence and acquire sheds counted since the previous call, and mark them taken.
 *
 * The pace ledger's drain: it writes one SHED record per shed taken. The
 * monotonic counts rhi_fence_sheds and rhi_acquire_sheds are unchanged.
 * @param dev Device, or NULL, which yields 0 for both.
 * @param fence Receives the fence sheds counted since the previous call.
 * @param acquire Receives the acquire sheds counted since the previous call. */
void     rhi_take_sheds(rhi_device *dev, uint64_t *fence, uint64_t *acquire);
/** @brief Return the number of swapchain acquires that expired their time budget.
 *
 * @param dev Device to query; NULL returns 0.
 * @return The running count; monotonic. */
uint64_t rhi_acquire_timeouts(const rhi_device *dev);

/** @brief Return the number of images the swapchain was granted.
 *
 * The swapchain requests minImageCount + 1, capped by maxImageCount and
 * RHI_MAX_SC_IMAGES; the driver may grant more. A back-pressure diagnosis
 * needs the granted count.
 * @param dev Device, or NULL.
 * @return The image count; 0 headless or for NULL. */
uint32_t rhi_swapchain_image_count(const rhi_device *dev);
/** @brief Write the device report: name, type, vendor and device ids, API and driver versions, the driver id, name and info, the enabled device extensions, the validation layer, present mode, swapchain extent, format, colour space and image count, and the offscreen extent.
 *
 * Read by the display's owner. The last line gives the compute decision
 * (rhi_compute_supported, and the reason when unavailable) and the queue
 * flags and limits it read.
 * @param dev Device, or NULL.
 * @param buf Receives the text, one fact group per line.
 * @param cap Capacity of @p buf.
 * @return snprintf's count. */
int rhi_device_report(const rhi_device *dev, char *buf, size_t cap);
/** @brief Return whether the picked physical device is a software rasterizer.
 *
 * The test is VK_PHYSICAL_DEVICE_TYPE_CPU, which llvmpipe reports. The
 * terrain sector-mesh cache is not initialized on such a device unless
 * OPENPETE_TERRAIN_CACHE_FORCE is set.
 * @param dev Device, or NULL.
 * @return true for a CPU device; false otherwise or for NULL. */
bool        rhi_device_is_software(const rhi_device *dev);
/** @brief Return how many descriptor sets a pipeline layout may bind on this device (maxBoundDescriptorSets).
 *
 * A pipeline needing more is refused at creation; a caller with an
 * alternative checks first and takes it.
 * @param dev Device, or NULL.
 * @return The limit; 0 for NULL. */
uint32_t    rhi_device_max_descriptor_sets(const rhi_device *dev);
/** @brief Return the terrain source tint's blend in thousandths, 0 when the instrument is off.
 *
 * OPENPETE_TERRAIN_SOURCE_TINT=1 arms it: every terrain fragment the
 * retained terrain cache draws mixes toward green and every one the CPU
 * walk draws toward red, after the stock shading (psx_src_tint.glsl).
 * OPENPETE_TERRAIN_SOURCE_TINT_ALPHA=<0..1> sets the blend (default 0.4;
 * 0 turns the instrument off). The value is specialization constant 6 of
 * every stage that declares it, and the scene packer marks walked terrain
 * vertices while it is nonzero. Read once per process.
 * @return 0 to 1000. */
int32_t     rhi_terrain_source_tint(void);
/** @brief Return the retained terrain's debug tint word, 0 when OPENPETE_TERRAIN_GPU_TINT is unset or "0".
 *
 * Otherwise bit 29 (tint on), the mode, value & 7, in bits 26-28
 * (terrain_shade.glsl ter_debug_shade: ladder verdict, raw colour, UV, t0k;
 * mode 7 the per-pixel face attribution word, ter_attr), and bit 25 for
 * value & 8, under which mode 7 carries the face's verdict-class bits in
 * place of its identity (terrain.vert). The word is specialization constant
 * 9 (TER_TINT) of every terrain stage, so a stage built with 0 carries no
 * tint code; the retained globals' gflags carry the same bits. Diagnostic;
 * read once per process.
 * @return The tint word. */
int32_t     rhi_terrain_gpu_tint(void);
/** @brief Return whether fillModeNonSolid is enabled on this device.
 *
 * With it, pipelines may rasterize with VK_POLYGON_MODE_LINE (the wireframe
 * view, rhi_set_wireframe); without it that view draws filled.
 * @param dev Device, or NULL.
 * @return true when enabled; false otherwise or for NULL. */
bool        rhi_device_has_fill_non_solid(const rhi_device *dev);

/** @brief Read the offscreen color target into a host RGBA8 buffer, for screenshots.
 *
 * Byte order is R,G,B,A with alpha forced opaque. Called after a rendered
 * frame (rhi_end_frame), it reads that frame's offscreen contents in the
 * TRANSFER_SRC layout the offscreen pass leaves. The extent is the swapchain
 * extent; under SSAA off_image already holds the resolved 1x result, so the
 * capture matches the present. It waits the device idle, then copies through
 * a persistent staging buffer that grows on demand.
 * @param dev Device to read from.
 * @param out Destination; must hold width x height x 4 bytes.
 * @param out_bytes Size of @p out in bytes.
 * @param out_w Receives the width, also on failure; may be NULL.
 * @param out_h Receives the height, also on failure; may be NULL.
 * @return false when @p out is NULL or too small, the extent is empty, or
 *         the staging buffer cannot be allocated. */
bool rhi_read_offscreen(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h);

/* ---- async frame readback ring (--record real-speed capture) -------------
 *
 * rhi_read_offscreen blocks the GPU (vkDeviceWaitIdle) every call, which
 * collapses pipelining and forces recording into slow motion. This ring
 * records the offscreen->host copy into the frame's own command buffer (no
 * extra wait: it rides the frame's existing fence) and retrieves the pixels
 * RHI_FRAMES_IN_FLIGHT frames later, once that fence has signalled.
 *
 * At record shutdown, call rhi_drain_readback repeatedly (it blocks once via
 * vkDeviceWaitIdle, then returns the still-queued frames oldest-first by tag)
 * until it returns false, so the final presents are not lost. */
/** @brief Queue this frame's offscreen capture and return the one the current frame slot queued last.
 *
 * Call once per frame, between rhi_blit_to_swapchain and rhi_end_frame, while
 * the frame slot is current. The slot's previous capture (queued
 * RHI_FRAMES_IN_FLIGHT frames ago, retired by rhi_begin_frame's fence wait)
 * is swizzled into `out` as R,G,B,A bytes with opaque alpha; a capture that
 * does not fit `out_bytes` is dropped. Then off_image, at the swapchain
 * extent, is copied into the slot's host-cached staging buffer, which grows
 * as needed.
 * @param dev Device whose current frame records the copy.
 * @param tag Opaque token (the caller's PTS) stored with this frame's capture.
 * @param out Destination for a retrieved capture, w*h*4 bytes.
 * @param out_bytes Capacity of @p out.
 * @param out_w Receives the retrieved capture's width; may be NULL.
 * @param out_h Receives the retrieved capture's height; may be NULL.
 * @param out_tag Receives the retrieved capture's tag; may be NULL.
 * @return true when a capture was written to @p out; false until the ring
 *         fills, outside an active frame, or for NULL @p dev. */
bool rhi_cycle_readback(rhi_device *dev, uint64_t tag, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag);
/** @brief Retrieve the thumbnail this frame slot queued the last time it cycled, then optionally queue this frame's.
 *
 * A second ring beside rhi_cycle_readback's, for low-resolution images: on
 * a queueing call the offscreen is downscaled with a linear blit into the
 * slot's own image, fitted inside max_w x max_h at its aspect, and copied
 * to the slot's staging buffer, both in the current frame's command
 * buffer. A slot's capture is retrieved RHI_FRAMES_IN_FLIGHT presents
 * later, after begin_frame's fence wait, so the GPU is never waited on.
 * Called between the blit and end_frame on the thread that holds the
 * display, every present (retrieval runs only when the slot cycles).
 * @param dev Device whose current frame records the downscale.
 * @param queue Nonzero to queue this frame's thumbnail.
 * @param max_w Width bound of a queued thumbnail.
 * @param max_h Height bound.
 * @param tag Opaque token stored with a queued thumbnail.
 * @param out Destination of a retrieved thumbnail, w*h*4 R,G,B,A bytes, alpha opaque.
 * @param out_bytes Capacity of @p out; a thumbnail that does not fit is dropped.
 * @param out_w Receives the retrieved width; may be NULL.
 * @param out_h Receives the retrieved height; may be NULL.
 * @param out_tag Receives the retrieved tag; may be NULL.
 * @return true when a thumbnail was written to @p out. */
bool rhi_cycle_thumb(rhi_device *dev, int queue, int max_w, int max_h, uint64_t tag,
                     void *out, size_t out_bytes, int *out_w, int *out_h, uint64_t *out_tag);
/* ---- player screenshot pool -------------------------------------------
 *
 * The offscreen as presented, copied into a host-cached staging buffer
 * inside the present's own command buffer, so taking a screenshot stalls
 * neither the GPU nor the display's owner. The buffer is read where it is
 * mapped once its frame's fence retired, and handed out to a reader on
 * any thread (the PNG writer), which returns it. */
#define RHI_SHOT_POOL 8
/** @brief Copy the offscreen into a free screenshot buffer in the current frame's command buffer.
 *
 * Called between the blit and end_frame on the display's owner, as
 * rhi_cycle_thumb is. Buffer 0 is used whenever it is free; a buffer
 * smaller than the swapchain extent is recreated.
 * @param dev Device whose current frame records the copy.
 * @param tag Opaque token returned with the copy by rhi_shot_ready.
 * @return The buffer's index, or -1 outside an active frame, at an empty
 *         extent, with every buffer in use, or when allocation failed. */
int  rhi_shot_queue(rhi_device *dev, uint64_t tag);
/** @brief Find the oldest queued screenshot whose frame has retired, without changing its state.
 *
 * A copy is retired once a frame RHI_FRAMES_IN_FLIGHT serials later began
 * (rhi_frame_serial), or after rhi_shot_drain. Display owner only.
 * @param dev Device.
 * @param px Receives the mapped pixels, R,G,B,A or B,G,R,A per rhi_shot_bgra, alpha as rendered; may be NULL.
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL.
 * @param tag Receives the copy's token; may be NULL.
 * @return The buffer's index, or -1 when none is ready. */
int  rhi_shot_ready(rhi_device *dev, const void **px, int *w, int *h, uint64_t *tag);
/** @brief Mark a ready screenshot buffer handed out, or take back a hand-out the reader refused.
 *
 * A held buffer's pixels stay mapped until rhi_shot_release. Mark it held
 * before the reader can see it, since the reader may release it at once.
 * @param dev Device.
 * @param i Index rhi_shot_ready returned.
 * @param held 1 to hand it out, 0 to return it to the ready state. */
void rhi_shot_hold(rhi_device *dev, int i, int held);
/** @brief Return a handed-out screenshot buffer to the pool; safe on any thread.
 * @param dev Device.
 * @param i Index the holder was given. */
void rhi_shot_release(rhi_device *dev, int i);
/** @brief Count the free screenshot buffers, those neither queued nor handed out.
 * @param dev Device, or NULL.
 * @return The count, 0 for NULL. */
int  rhi_shot_free(rhi_device *dev);
/** @brief Free every screenshot buffer but the first when none is in use; display owner only.
 * @param dev Device, or NULL. */
void rhi_shot_trim(rhi_device *dev);
/** @brief Wait the device idle when a screenshot copy is queued, so rhi_shot_ready returns it without later frames.
 *
 * For teardown, after the last present. Display owner only.
 * @param dev Device, or NULL. */
void rhi_shot_drain(rhi_device *dev);
/** @brief Report whether screenshot buffers hold B,G,R,A rather than R,G,B,A.
 * @return 1 for B,G,R,A, else 0. */
int  rhi_shot_bgra(void);

/** @brief Wait the device idle and retrieve the pending --record capture with the lowest tag, without recording a new one.
 *
 * Called at shutdown, up to RHI_FRAMES_IN_FLIGHT times, to flush the
 * ring's tail in tag order. The slot is released even when @p out is too
 * small.
 * @param dev Device.
 * @param out Destination, w*h*4 bytes.
 * @param out_bytes Capacity of @p out.
 * @param out_w Receives the capture's width; may be NULL.
 * @param out_h Receives the capture's height; may be NULL.
 * @param out_tag Receives the capture's tag; may be NULL.
 * @return true when a capture was written to @p out; false when none is
 *         pending or @p out is too small. */
bool rhi_drain_readback(rhi_device *dev, void *out, size_t out_bytes,
                        int *out_w, int *out_h, uint64_t *out_tag);

/* ---- buffers, pipelines, draws ------------------------------------ */

/* Native vertex format - one fixed layout for all renderer geometry, so a
 * single vertex buffer format feeds every pipeline: position, color, and
 * uv for the PSX sampler. The per-draw texture
 * page / CLUT / bpp ride a push constant (one material per draw batch - a
 * native renderer sorts by material), so the packed per-vertex `tex` field
 * is currently unused/reserved (PSX-era per-vertex tpage packing would only
 * return if mixed-material batching is ever needed). */
typedef struct {
    float    x, y, z;   /* position (clip space in the smoke test; proj later) */
    float    u, v;      /* texture coords in TEXEL units (PSX integer indexing) */
    uint32_t rgba;      /* per-vertex color, byte order R,G,B,A                 */
    float    koff;      /* og OT KEY (location 5): the face's ABSOLUTE .s
                         * insertion key in world units (corner-mean view-Z
                         * + 128*slot bias) + 0.5 sentinel; 0 = not keyed /
                         * contest off. POSITIVE: the face both EMITS its
                         * flat key into the key image (terrain_key.vert)
                         * and CONTESTS it (psx_scene_key.frag et al).
                         * NEGATIVE (|koff| = key): CONTEST-ONLY - near-band
                         * terrain, whose huge under-camera footprints must
                         * never stamp the image. Stored depth stays TRUE
                         * view-Z - faithfulness lives in the fragment
                         * SELECT, never in the depth value.                 */
    float    sx, sy;    /* PSX screen coord (location 3) - the textured shadow
                         * decal's depth-pre-pass lookup rides here, because its
                         * u,v are the sprite texel (unlike the untextured Spyro
                         * decal, which parks the screen coord in u,v). Ignored
                         * by every pipeline whose shader omits location 3. On
                         * the scene and key stages the pair is the rank pair:
                         * sx = paint rank or rank sentinel; sy = a negative
                         * drain record tag (terrain), the moby ordinal (ranked
                         * and non-actor faces) or the moby identity (unranked
                         * actor faces), as psx_key_contest.glsl op_akey_owner
                         * reads it.                                           */
    float    w;         /* perspective W = TRUE view-Z (location 4). On the
                         * scene stages z is the ORDERING view-Z and may carry
                         * a feeder's OT-slot shift, so deriving W from it
                         * warps UV/colour interpolation. Always stated.        */
    float    akc;       /* location 6: an actor face's (rank < -0.5)
                         * moby-against-moby key offset, 0 on any other face.
                         * On an opaque keyed face the key at a fragment is
                         * the fragment's ordering view-Z plus akc
                         * (psx_key_contest.glsl op_key_loses, and the actor
                         * key pass's per-pixel form, terrain_key.vert).      */
    float    rgbq;      /* location 7: the residual of rgba's R, G and B from
                         * the vertex's unrounded colour, three signed bytes
                         * in 1/256 of a level as an integer below 2^24, which
                         * a float holds exactly (psx_unorm8.glsl op_unorm8q).
                         * 0 on every vertex but a split terrain node and a
                         * moby near-cut corner.                              */
} rhi_vertex;

typedef struct rhi_buffer   rhi_buffer;
typedef struct rhi_pipeline rhi_pipeline;
typedef struct rhi_texture  rhi_texture;

/* Terrain sector-mesh vertex (the terrain GPU cache). A retained world-space
 * vertex: the camera transform, fog, CLUT ladder and the per-face verdicts
 * (NCLIP two-half cull, near/HQ variant select, LOD handoff) all run in the
 * vertex shader, so every vertex carries its PARENT FACE's four corner
 * positions and both tiledef halves' material/UV alongside its own data.
 * Positions are sector-local with 2 FRACTIONAL bits (value = world/16 * 4):
 * the HQ 2x2 subdivide's centre is the corner mean, a quarter-unit quantity.
 * Layout is load-bearing: the Vulkan backend's terrain vertex-input
 * attributes and terrain.vert's `location` declarations mirror these
 * offsets. */
typedef struct rhi_terrain_vertex {   /* tagged: forward-declared by the
                                       * terrain-cache header */
    int16_t  px, py, pz;        /* position, sector-local, x4 fixed point;
                                 * near-variant chunks, whose stage reads the
                                 * corners alone: px | py << 16 = the face's
                                 * previous-corner record (OPTC_PREV_REC_*) */
    uint16_t flags16;           /* bit0: HQ-subdivide variant (vs flat/LP);
                                 * bits1-2: HQ sub-quad index (0..3);
                                 * bits3-4: corner slot within the sub-quad
                                 * (0..3 in [TL,TR,BL,BR] order);
                                 * bits5-11: scroll tile id (flat: Tiledef,
                                 * HQ: record id); bit12: tile id valid -
                                 * the vertex stage adds the globals' per-tile
                                 * fractional V-scroll offset;
                                 * bit13: SECOND emitted tri of the (sub-)quad
                                 * - the vertex belongs to half 2 / sub-tri 2,
                                 * so per-emitted-tri verdicts (the portal
                                 * inner-rim cull) can rebuild their own tri  */
    uint8_t  uvf_u, uvf_v;      /* far-tile (LQ) texel coords, page-relative */
    uint8_t  uvn_u, uvn_v;      /* near-tile texel coords (HP textured only) */
    uint32_t rgba_far;          /* far colour (LP: THE colour; HQ: the baked
                                 * raw-near/midpoint colour), R,G,B,A        */
    uint32_t rgba_near;         /* near colour (HP flat; else == rgba_far)   */
    uint32_t mat_far0;          /* tpx(10) | tpy(9)<<10 | bppc(2)<<19 |
                                 * blend(2)<<21 | textured(1)<<23
                                 * (bppc: 0=4bpp, 1=8bpp, 2=16bpp)           */
    uint32_t mat_far1;          /* clut_raw halfword (paletted only)         */
    uint32_t mat_near0;         /* near-tile material, same packing          */
    uint32_t mat_near1;
    uint32_t face_flags;        /* sector_slot(9) | tier(1)<<9 | fogbias<<10 |
                                 * gapfill<<11 | dside<<12 (LP w0&0x80 /
                                 * HP w3&0x4) | flip<<13 | quad<<14 |
                                 * near_tier2<<15 | near_excl<<16 (the defer
                                 * classifier's two face-word bits, both 0 on a
                                 * game that classifies on depth alone) |
                                 * hq_eligible<<17 (HP) / ride<<17 (LP: drawn
                                 * by the extended-LOD uncov-only ride) |
                                 * floor<<18 (LP up-facing) |
                                 * admit(5)<<19 (LP authored admission) |
                                 * pincells<<19 (HP: the always-HQ pin sends
                                 * the face to the 4x4 cells) |
                                 * cellok<<20 (HP: the face may take the
                                 * 4x4 cells) |
                                 * keycells<<21 (HP: the drawn triangles key
                                 * each fragment by its drain sub-cell) |
                                 * lpkey<<22 (HP: a borrowed face standing in
                                 * for an uncov LP face keys with the LP base
                                 * slot term) |
                                 * water<<23 (HP: a semi-transparent face of a
                                 * sector the game draws in its second walk;
                                 * past the near split it takes that walk's
                                 * colour curve and drop) |
                                 * otbias(5)<<24 (authored OT-slot bias)     */
    int16_t  c0x, c0y, c0z;     /* parent face corners in i0,i1,i2,i3 order, */
    int16_t  c1x, c1y, c1z;     /* sector-local, same x4 fixed point         */
    int16_t  c2x, c2y, c2z;     /* (c2==c3 for tris)                         */
    int16_t  c3x, c3y, c3z;
} rhi_terrain_vertex;           /* 64 bytes */

/* Anim chunks only (poly-anim and env-anim colour sectors): previous-tick
 * twin of the animated fields, a PARALLEL array in the anim arenas' prev
 * buffer (vertex binding 1). The vertex stage lerps prev->cur at the
 * build's sub-tick alpha on camera-relative integer differences -
 * position for flat/LP verts, parent corners for everything corner-derived
 * (HQ nodes, fog ladder, NCLIP) - the mirror of tsc_vanim_lerp. A sector whose banks jumped (keyframe
 * reset) or has no prev bank yet bakes prev == cur (the CPU snap rule). */
typedef struct rhi_terrain_prev {
    int16_t  px, py, pz, pad;   /* prev position, sector-local x4 fixed */
    int16_t  c0x, c0y, c0z;     /* prev parent corners, i0,i1,i2,i3     */
    int16_t  c1x, c1y, c1z;
    int16_t  c2x, c2y, c2z;
    int16_t  c3x, c3y, c3z;
    uint32_t rgb_far, rgb_near; /* prev far / near colour words (R,G,B,A):
                                 * the vertex stage lerps them to the current
                                 * colours at the globals' colour alpha
                                 * (canim_lerp mirror); == current on a sector
                                 * whose colours do not animate            */
} rhi_terrain_prev;             /* 40 bytes */

/* Near-variant chunks only: the OTHER two emitted-tri members' attribute
 * context, a parallel array in the near arenas' ctx buffer (vertex binding
 * 1). The near-variant vertex stage rebuilds its whole emitted triangle
 * (positions come from the parent corners already in the base vertex; UVs
 * and both-half colours for the two non-self members come from here, in
 * emit order relative to self: a = (self+1)%3, b = (self+2)%3), then
 * mirrors the feed's near-plane clip on it and takes its own slot of the
 * clipped fan. */
typedef struct rhi_terrain_nearctx {
    uint8_t  a_uf, a_vf, a_un, a_vn;   /* member a: far/near-half UVs   */
    uint32_t a_far, a_near;            /* member a: far/near colours    */
    uint8_t  b_uf, b_vf, b_un, b_vn;   /* member b likewise             */
    uint32_t b_far, b_near;
} rhi_terrain_nearctx;          /* 24 bytes */

/* 4x4 near-cell records (the terrain cache's cell arenas; one INSTANCE per
 * record, drawn by the terrain_cells vertex stages). A tier-4-eligible HQ face
 * (the near drain's 5x5 node grid, 16 cells, each textured from a 16x16 tile,
 * the near drain's tier-4 pass) carries its parent corners and raw near
 * colours once, beside the cell tiles; the vertex stage derives each cell vertex from the record and gl_VertexIndex - node
 * position, colour (the drain's halving order over the four near colours),
 * tile UV and material - so no per-vertex cell data is stored.
 *
 * A record holds EIGHT of the face's 16 cells (one half, row-major cells 0-7
 * or 8-15): the whole face would need 16 cell attributes beside the face
 * context, past the 16 vertex-input locations every Vulkan device provides.
 * A record lives in the cell arena of one draw class and draws only the cells
 * its mask names, so a face whose tiles span blend classes has one record copy
 * per class. */
typedef struct rhi_terrain_cell_tile {
    uint8_t  uv[8];             /* u,v of the cell corners in [TL,TR,BL,BR]
                                 * order: the 16x16 tile at hq_cells4 + i*8
                                 * with the D_8006D058 orientation applied   */
    uint32_t mat0;              /* rhi_terrain_vertex.mat_far0 packing, the
                                 * face's material bucket in bits 24-31      */
    uint32_t mat1;              /* clut_raw | HD-fold entry id << 16         */
} rhi_terrain_cell_tile;        /* 16 bytes */

typedef struct rhi_terrain_cell {
    int16_t  c0x, c0y, c0z;     /* parent face corners i0,i1,i2,i3,         */
    int16_t  c1x, c1y, c1z;     /* sector-local x4 fixed point, the         */
    int16_t  c2x, c2y, c2z;     /* rhi_terrain_vertex c0x..c3z layout        */
    int16_t  c3x, c3y, c3z;
    uint32_t face_flags;        /* rhi_terrain_vertex.face_flags            */
    uint32_t info;              /* bits 0-6: HQ record id (scroll tile id);
                                 * bit 7: half (0 = cells 0-7, 1 = 8-15);
                                 * bits 8-15: cells this record draws;
                                 * bits 16-23: cells whose tile is blended
                                 * (the cover-drop key, flags16 bit 14);
                                 * bit 24: a tier-4 triangle's record: entry
                                 * j is sub-triangle half*8 + j, its tile
                                 * uv[0..5] the (u, v) of nodes A, B, C and
                                 * uv[6] its cell;
                                 * bit 25 (with bit 24): a tier-2 triangle's
                                 * record, entries 0-3 its sub-triangles and
                                 * uv[6] the quadrant tile                   */
    uint32_t near_rgb[4];       /* raw near colours of corners i0..i3 (a
                                 * triangle's C colour, word 1 byte 0, in [3]) */
    rhi_terrain_cell_tile tile[8];
    uint32_t far_rgb[4];        /* raw far (depth-cue target) colours, near_rgb's
                                 * order: the nodes of a face the HQ pin draws
                                 * past the game's near band fog toward them */
    uint32_t fog_a, fog_b;      /* fog_a: the face's previous-corner record
                                 * (OPTC_PREV_REC_*), read by the near cell
                                 * stage; fog_b: zero                       */
} rhi_terrain_cell;             /* 200 bytes */

/* Poly-anim sectors' records: the same plus the previous-tick corners the
 * terrain_anim stage lerps from (rhi_terrain_prev's c0x..c3z). */
typedef struct rhi_terrain_cell_anim {
    rhi_terrain_cell cur;
    int16_t  p0x, p0y, p0z;
    int16_t  p1x, p1y, p1z;
    int16_t  p2x, p2y, p2z;
    int16_t  p3x, p3y, p3z;
} rhi_terrain_cell_anim;        /* 224 bytes */

/* Vertices per cell record instance: 8 cells x 2 triangles, each baked as
 * the near variant's primary + spare fan triangle (6) or as the anim
 * variant's single triangle (3). */
#define RHI_TERRAIN_CELL_VERTS_NEAR 96u
#define RHI_TERRAIN_CELL_VERTS_ANIM 48u

/* RHI_BLEND_SUBTRACT = PSX ABR mode 2 (dst = B - F): reverse-subtract, src ONE,
 * dst ONE - used by shadows (subtractive darkening). */
/* RHI_BLEND_ADD_QUARTER = PSX ABR mode 3 (dst = B + F/4): add with the source
 * scaled by a constant quarter - glass and other faint overlays, which full
 * additive blows out and reverse-subtract turns black. */
typedef enum { RHI_BLEND_NONE, RHI_BLEND_ALPHA, RHI_BLEND_ADD, RHI_BLEND_SUBTRACT,
               RHI_BLEND_ADD_QUARTER } rhi_blend_mode;

/* Stencil role of a pipeline (the aperture mask; the depth
 * attachment is a combined depth+stencil format, stencil cleared to 0 at every
 * pass begin):
 *  - MARK: rasterize a mask - where the fragment passes the DEPTH test, write
 *    stencil=1 (depth-fail leaves 0, so world geometry in front punches the
 *    mask out). Pair with color_write_off for an invisible mask pass.
 *  - TEST: draw only where stencil==1 (inside the mask); stencil not written.
 *  - TEST_ZERO: draw only where stencil==0 (OUTSIDE the mask); stencil not
 *    written. The hole-fill role: geometry that must paint only the
 *    pixels a prior MARK pass did NOT cover (e.g. the terrain LP backing layer
 *    filling LP<->HP T-junction seams without ever poking through the HP).
 *  - ONCE: draw only where stencil != 1 and write 1 there, so each pixel takes
 *    one fragment until the next stencil clear: a blended layer whose
 *    triangles overlap blends each pixel once, and a later ONCE draw fills
 *    only the pixels the earlier ones left (the launcher sky's dome, then its
 *    clear-colour fill).
 * Reference is fixed at 1 - per-object isolation is done by rhi_clear_stencil
 * between mask groups, not by reference juggling. */
typedef enum { RHI_STENCIL_NONE, RHI_STENCIL_MARK, RHI_STENCIL_TEST,
               RHI_STENCIL_TEST_ZERO, RHI_STENCIL_ONCE } rhi_stencil_mode;

/* Depth convention of a pipeline or a render target. The compare op, the
 * clear value and the scene stages' depth encoding (vertex and fragment spec
 * constant 0) are device tables indexed by this class, filled once the depth
 * format is chosen (rhi_depth_* below):
 *  - SCENE: the scene depth the colour passes test against (the offscreen
 *    pass, the scene render targets and the overlay targets drawn with the
 *    scene pipelines).
 *  - FORWARD: images with a private encoding that stores nearer as smaller
 *    and clears to 1.0, tested LESS_OR_EQUAL whatever the scene convention:
 *    the WORLD-opaque depth pre-pass, the og key images and the moby rank
 *    image, and the pipelines that draw into them. */
typedef enum { RHI_DEPTH_SCENE = 0, RHI_DEPTH_FORWARD = 1,
               RHI_DEPTH_CLASSES } rhi_depth_class;

/* Primitive topology of a pipeline. TRIANGLE_LIST (the zero value) is every
 * renderer pipeline's; POINT_LIST rasterises each vertex as one point, whose
 * vertex stage writes gl_PointSize (a diagnostic that addresses one pixel
 * per vertex). */
typedef enum { RHI_TOPOLOGY_TRIANGLE_LIST = 0, RHI_TOPOLOGY_POINT_LIST } rhi_topology;

/* Colour format of a render target and of the pipelines drawn into it.
 * SCENE (the zero value) is the fixed internal 8-bit format every pass
 * shares. RGBA32_UINT holds four 32-bit unsigned words per texel: a
 * fragment stage writes `out uvec4`, blending is off, the clear words are
 * the clear colour's float bits, and the texels are read back as stored
 * (rhi_read_render_target_texels). Such a target is not sampleable. */
typedef enum { RHI_COLOR_FORMAT_SCENE = 0, RHI_COLOR_FORMAT_RGBA32_UINT } rhi_color_format;

typedef struct rhi_pipeline_desc {
    const char    *vert_spv;    /* filename within RHI_SHADER_DIR */
    const char    *frag_spv;
    const char    *vert_spv_whole; /* or NULL: the vertex stage a pipeline
                                 * created whole takes in place of vert_spv
                                 * (same interface). A library-linked
                                 * pipeline shares vert_spv's pre-rasterization
                                 * library with every desc of the same state;
                                 * a whole create compiles its vertex stage
                                 * for that one pipeline, so this stage may
                                 * read a per-pipeline specialization constant
                                 * that vert_spv reads at run time. Whole
                                 * creates: devices without pipeline libraries,
                                 * a failed link's fallback, and the background
                                 * optimization of a library-linked pipeline
                                 * (rhi_pipeline_lto_step), which then replaces
                                 * the link with the whole pipeline. Ignored
                                 * when the vertex stage is given as words. */
    rhi_blend_mode blend;
    bool           depth_test;  /* needs a depth attachment (added later) */
    bool           depth_write_off; /* depth_test but NO depth write (transparent: test vs opaque, don't occlude) */
    bool           depth_overwrite; /* depth_test with compare ALWAYS: every
                                 * rasterized fragment REPLACES the stored
                                 * depth, whatever was there. Pair with
                                 * color_write_off for a depth-only pass that
                                 * evicts an occluder from exactly the pixels
                                 * some geometry covers (the dragon-rescue
                                 * front pass). Note the depth TEST must stay
                                 * enabled to write at all - Vulkan disables
                                 * depth writes whenever depthTestEnable is
                                 * false - hence compare ALWAYS rather than
                                 * "write without testing". */
    bool           depth_bias;  /* enable the hardware polygon offset (decal
                                 * tie-break) with DYNAMIC factors. Vulkan
                                 * invalidates dynamic state whenever a
                                 * pipeline with that state STATIC is bound
                                 * (i.e. any non-depth_bias pipeline), so the
                                 * caller MUST rhi_set_depth_bias after EVERY
                                 * bind of a depth_bias pipeline, before its
                                 * draws. */
    rhi_depth_class depth_class; /* SCENE (default) or FORWARD: selects the
                                 * compare op and spec constant 0 from the
                                 * device's tables (rhi_depth_class). A
                                 * FORWARD pipeline draws only into a FORWARD
                                 * render target.                             */
    rhi_stencil_mode stencil;   /* NONE (default) / MARK / TEST - see enum docs */
    bool           color_write_off; /* rasterize without writing colour (an
                                 * invisible stencil-mask pass)               */
    int            texel_stp_select; /* fragment spec constant 5 (STP_SELECT,
                                 * psx_scene.frag): 0 every texel, 1 texels
                                 * with bit 15 clear only, 2 texels with bit
                                 * 15 set only (OPENPETE_STP_*). Shaders that
                                 * do not declare it ignore it.               */
    bool           uses_vram;   /* layout includes a sampler at set 0          */
    bool           uses_vram2;  /* second sampler at set 1 (e.g. the diff pass:
                                 * set 0 = native render, set 1 = reference)   */
    bool           uses_vram3;  /* set 2: the terrain og-key image at binding 0,
                                 * the drain sub-cell key records at binding
                                 * 1 and the key image's owner attributes
                                 * (its colour) at binding 2 (the trio
                                 * layout, bound with
                                 * rhi_bind_terrain_key_set). Implies sets 0..2
                                 * exist in the layout; sets a shader never
                                 * statically accesses need no binding at
                                 * draw.                                       */
    bool           uses_vram4;  /* fourth sampler at set 3 (the MOBY og-key
                                 * image - split from the terrain keys so the
                                 * near-band embed rule applies to moby keys
                                 * only); also turns set 1 into a quad, the
                                 * flat and the per-pixel actor key images'
                                 * depths and owner colours
                                 * (rhi_bind_depth_rt_quad). Implies
                                 * uses_vram3.                                 */
    bool           uses_vram5;  /* fifth sampler at set 4 (the moby og-RANK
                                 * image: per-pixel winning paint rank +
                                 * present-local moby ordinal, RGBA8 - the
                                 * same-moby painter select). Implies
                                 * uses_vram4.                                 */
    bool           set1_vram_depth; /* set 1 holds two samplers: binding 0 a
                                 * colour texture (the VRAM image), binding 1
                                 * a sampled depth image (the terrain key
                                 * image). Replaces set 1 of any layout above
                                 * and implies sets 0..1. Bind it with
                                 * rhi_bind_vram_depth_pair.                   */
    bool           uniform_set2; /* set 2 holds one dynamic-UBO binding (bind
                                 * with rhi_ubo_bind_at(.., 2, ..)), visible
                                 * to both stages. Replaces set 2 of any layout
                                 * above and implies sets 0..2.               */
    bool           uniform_set5; /* set 5 holds one dynamic-UBO binding (bind
                                 * with rhi_ubo_bind_at(.., 5, ..)), visible
                                 * to both stages. Sets 0..4 keep the layout
                                 * the flags above give them; implies sets
                                 * 0..5.                                     */
    bool           pair_set56;  /* sets 5 and 6 hold the two-sampler pair
                                 * layout (bind with rhi_bind_depth_tex_pair):
                                 * the moby front-shell image (depth, owner
                                 * colour) and its intruder image on the
                                 * opaque ACTOR stages' _shell variants and
                                 * the intruder pre-pass. Sets 0..4 keep the
                                 * layout the flags above give them; implies
                                 * sets 0..6. Exclusive with uniform_set5.   */
    bool           uniform_set7; /* set 7 holds one dynamic-UBO binding (bind
                                 * with rhi_ubo_bind_at(.., 7, ..)), visible
                                 * to both stages, above the seven sets of a
                                 * material layout; implies sets 0..7 and a
                                 * device with 8 descriptor sets.            */
    bool           ssbo_set7;   /* set 7 holds one dynamic storage-buffer
                                 * binding, vertex and fragment visible (a
                                 * block of rhi_create_ssbo, bound with
                                 * rhi_ubo_bind_at(.., 7, ..)): the moby
                                 * snap table of the ACTOR snap stages.
                                 * Implies sets 0..7 and a device with 8
                                 * descriptor sets.                          */
    bool           ssbo_set2;   /* set 2 holds the binding of ssbo_set7 (a
                                 * rhi_storage bound with
                                 * rhi_storage_bind(.., 2), or a block of
                                 * rhi_create_ssbo): the terrain layout's
                                 * unused set 2, or set 2 of a kind whose
                                 * own layout stops below it. Not for the
                                 * material and key-resolve (uses_vram3)
                                 * kinds, whose set 2 is the terrain key
                                 * trio. Implies sets 0..2.                  */
    bool           tex_ssbo_set2; /* set 2 holds a combined image sampler at
                                 * binding 0 (fragment visible) and one
                                 * dynamic storage buffer at binding 1
                                 * (vertex visible), bound together with
                                 * rhi_bind_texture_storage: the terrain
                                 * cache's prop key image beside its
                                 * vertex-stage record store. A shader that
                                 * declares binding 0 alone stays
                                 * compatible. Implies sets 0..2.            */
    bool           pair_set7;   /* with terrain_pipeline: set 7 holds the
                                 * two-sampler pair layout (bind with
                                 * rhi_bind_depth_tex_pair): the actor key
                                 * image and its owner colour, which
                                 * terrain.frag reads for the key-order
                                 * contest. Implies sets 0..7 and a device
                                 * with 8 descriptor sets.                   */
    uint32_t       push_const_size; /* fragment push-constant block bytes (0=none) */
    bool           postfx;      /* mod-postfx layout: NO vertex input (fullscreen
                                 * triangle from gl_VertexIndex - pair with the
                                 * embedded postfx_fs.vert), sets 0..2 = one
                                 * sampler each (prev pass / depth / prev frame),
                                 * set 3 = the 2-binding dynamic UBO layout
                                 * (engine block, mod block - rhi_ubo_bind).
                                 * uses_vram/uses_vram2 are ignored.           */
    bool           mod_pipeline; /* mod custom-draw layout: the STANDARD
                                 * rhi_vertex input + the postfx 4-set layout
                                 * (sets 0..2 samplers, set 3 UBO pair), so
                                 * mod vert/frag pairs see attributes at
                                 * locations 0..2 and the engine UBO at set 3
                                 * binding 0. uses_vram/uses_vram2 ignored.   */
    bool           material_pipeline; /* mod MATERIAL layout: mod_pipeline's
                                 * sets 0..3 (samplers, then the UBO pair)
                                 * PLUS sets 4..6 = the engine's og-key
                                 * occlusion inputs (terrain keys, moby keys,
                                 * actor keys / rank), which a material's
                                 * fragment stage evaluates before the mod's
                                 * colour maths. Above the UBO pair, not
                                 * replacing sets 1/2, so a material's own
                                 * uniform block stays at set 3 binding 1.
                                 * Needs maxBoundDescriptorSets >= 7; pipeline
                                 * creation FAILS (caller draws stock) below
                                 * that. Implies mod_pipeline's vertex input. */
    bool           terrain_pipeline; /* terrain sector-mesh layout: vertex
                                 * input = rhi_terrain_vertex (locations 0..8),
                                 * sets = {0: VRAM sampler, 1: the terrain
                                 * key depth and its owner attributes (the
                                 * pair layout, rhi_bind_depth_tex_pair with
                                 * the key target twice), 2: the prop key
                                 * image,
                                 * 3: the material contract's UBO pair,
                                 * 4: HD-replacement atlas sampler,
                                 * 5: TerrainGlobals, 6: TerrainSectors - both
                                 * single-binding via rhi_ubo_bind_at},
                                 * push constants visible to VERTEX|FRAGMENT
                                 * (the per-draw clip affine + class select).
                                 * The engine's own two blocks sit ABOVE the
                                 * contract set rather than inside it: set 3
                                 * binding 1 belongs to the mod, and one
                                 * descriptor set views one buffer per binding,
                                 * so terrain uniforms and a mod's uniforms
                                 * cannot share it. Needs
                                 * maxBoundDescriptorSets >= 7 (as materials
                                 * do); below that, pipeline creation fails and
                                 * the cache stays off. uses_vram/uses_vram2
                                 * ignored.                                   */
    bool           terrain_anim; /* with terrain_pipeline: add vertex binding 1
                                 * = rhi_terrain_prev (locations 9..12), the
                                 * previous-tick position + parent corners for
                                 * the vertex stage's sub-tick lerp (poly-anim
                                 * sector chunks). Bind the prev buffer with
                                 * rhi_bind_vertex_buffer2.                   */
    bool           terrain_near; /* with terrain_pipeline: add vertex binding 1
                                 * = rhi_terrain_nearctx (locations 9..12), the
                                 * emitted-tri member context for the vertex
                                 * stage's near-plane clip mirror (near-variant
                                 * chunks). Mutually exclusive with
                                 * terrain_anim; bind the ctx buffer with
                                 * rhi_bind_vertex_buffer2.                   */
    bool           terrain_cells; /* with terrain_pipeline: the vertex input is
                                 * ONE instance-rate binding of cell records
                                 * (rhi_terrain_cell, locations 0..12; with
                                 * terrain_anim, rhi_terrain_cell_anim, plus
                                 * the previous corners at 13..15) and no
                                 * per-vertex binding. Draw with
                                 * rhi_draw_instanced.                        */
    bool           wire_scene;  /* a depth_test=false draw that belongs to
                                 * the 3D scene (the shadow decals, whose
                                 * fragment stage owns occlusion): the
                                 * wireframe view lines it like the
                                 * depth-tested scene pipelines. The 2D
                                 * layers leave it false and stay filled.     */
    bool           push_vertex; /* the push-constant block is visible to the
                                 * vertex stage as well as the fragment stage
                                 * (the terrain pipelines' visibility).       */
    rhi_topology   topology;    /* primitive topology; TRIANGLE_LIST unless a
                                 * pipeline asks for points                   */
    rhi_color_format color_format; /* the colour format of the render pass
                                 * the pipeline is built against: SCENE, or
                                 * RGBA32_UINT for a target made by
                                 * rhi_create_render_target_format with that
                                 * format (blend must be RHI_BLEND_NONE)      */
} rhi_pipeline_desc;

/** @brief Create a single-instance host-visible vertex buffer, updated by map and memcpy.
 *
 * Safe only for content that is static after init or rewritten under
 * rhi_wait_idle (a rare re-layout): a per-present rewrite would race the
 * frame RHI_FRAMES_IN_FLIGHT presents ago, whose GPU read is fenced only at
 * rhi_begin_frame (a write-after-read tear).
 * @param dev Device the buffer is created on.
 * @param bytes Buffer size in bytes.
 * @return The buffer, or NULL on failure. */
rhi_buffer *rhi_create_vertex_buffer(rhi_device *dev, size_t bytes);
/** @brief Create a single-instance host-mapped vertex buffer, preferring device-local memory.
 *
 * For retained geometry the GPU fetches every present and the CPU writes
 * rarely (the terrain sector-mesh arenas). Allocation is attempted on every
 * host-visible, host-coherent memory type the buffer accepts, DEVICE_LOCAL
 * types first (rhi_memtype_candidates), and the first type that allocates
 * wins. A device-local host-visible heap can be far smaller than the device's
 * video memory (a 256 MB BAR window, or what a resizable-BAR driver reports)
 * and its free space varies with what else is resident, so an allocation
 * that fits on one run can fail on the next; falling through to system
 * memory costs bandwidth and keeps the buffer. Every type
 * OPENPETE_RHI_FAIL_MEMTYPE names is refused without reaching the driver.
 * Same overlap rules as rhi_create_vertex_buffer.
 * @param dev Device the buffer is created on.
 * @param bytes Buffer size in bytes.
 * @return The buffer, or NULL when every candidate type fails. */
rhi_buffer *rhi_create_vertex_buffer_device(rhi_device *dev, size_t bytes);
/** @brief Create a host-mapped storage buffer for vertex-stage reads, written in place with rhi_update_buffer_range.
 *
 * Memory as rhi_create_vertex_buffer_device. The caller keeps the bytes a
 * recorded or executing draw reads unchanged until that draw retires.
 * @param dev Device the buffer is created on.
 * @param bytes Buffer size in bytes.
 * @return The buffer, or NULL when allocation fails. */
rhi_buffer *rhi_create_storage_buffer_device(rhi_device *dev, size_t bytes);
/** @brief Report whether a buffer's memory carries DEVICE_LOCAL.
 *
 * Meaningful for the buffers rhi_create_vertex_buffer_device and
 * rhi_create_storage_buffer_device make, which fall back to a host-visible
 * type without DEVICE_LOCAL when the device-local types refuse.
 * @param b The buffer; NULL reads 0.
 * @return 1 when the memory type is DEVICE_LOCAL, else 0. */
int rhi_buffer_device_local(const rhi_buffer *b);
/** @brief Create a per-frame-in-flight ring vertex buffer for content rewritten every present.
 *
 * rhi_create_vertex_buffer_dynamic_gens with one generation. rhi_update_buffer
 * writes and the bind reads the current frame's slot, which rhi_begin_frame's
 * fence has proven idle. The content must be written on every frame that
 * binds it: a slot written once and bound on a later frame of the other
 * parity reads that parity's stale slot. rhi_update_buffer outside an active
 * frame fills every slot, so init and under-wait-idle updates are
 * parity-safe.
 * @param dev Device the buffer is created on.
 * @param bytes Size of one slot in bytes.
 * @return The buffer, or NULL on failure. */
rhi_buffer *rhi_create_vertex_buffer_dynamic(rhi_device *dev, size_t bytes);
/** @brief Create a ring vertex buffer with `gens` generations per frame slot.
 *
 * A write that follows a bind in the same frame lands in the next
 * generation, so a draw recorded against the earlier data still reads it at
 * GPU execution; a write that finds no generation left overwrites bound data
 * and counts in rhi_ring_gen_overflows. For a buffer refilled and drawn more
 * than once per frame (the scene buffers when the reference-diff or A/B
 * variant renders the scene twice). Memory is bytes x RHI_FRAMES_IN_FLIGHT x
 * gens. Same per-frame write contract as rhi_create_vertex_buffer_dynamic.
 * @param dev Device the buffer is created on.
 * @param bytes Size of one generation slot in bytes.
 * @param gens Generations per frame slot; 0 is raised to 1.
 * @return The buffer, or NULL on failure. */
rhi_buffer *rhi_create_vertex_buffer_dynamic_gens(rhi_device *dev, size_t bytes,
                                                  uint32_t gens);
/** @brief Return the number of same-frame rewrites that moved to a fresh generation.
 *
 * Counts writes to generation ring vertex buffers and to rhi_ubo blocks
 * that found their current generation bound by a draw this frame.
 * rhi_ring_gen_overflows counts the rewrites that found no generation left
 * and overwrote bound data (a defect). Global and monotonic.
 * @return The running count. */
uint64_t    rhi_ring_gen_bumps(void);
/** @brief Return the number of same-frame rewrites that found no generation left and overwrote bound data.
 *
 * Counts ring vertex buffers and rhi_ubo blocks together. Any nonzero value
 * means a draw read data newer than the data it was recorded against.
 * Global and monotonic.
 * @return The running count. */
uint64_t    rhi_ring_gen_overflows(void);
/** @brief Write `bytes` of `data` at offset 0 of a mapped buffer, into the current frame slot of a ring buffer.
 *
 * Inside a frame a ring buffer takes the write in its current generation of
 * the frame slot, the next generation after a bind in the same frame.
 * Outside a frame every slot and generation receives the copy. A plain
 * buffer is written in place. The size is clamped to the buffer's (slot's)
 * capacity.
 * @param buf Buffer; NULL or unmapped is ignored.
 * @param data Source bytes; must not be NULL.
 * @param bytes Byte count. */
void        rhi_update_buffer(rhi_buffer *buf, const void *data, size_t bytes);
/** @brief Write `bytes` at byte `offset` within a buffer's current frame slot.
 *
 * For passes that bump-allocate several batches into one ring slot per frame
 * (the terrain key pass): a plain update at offset 0 would clobber batches an
 * earlier recorded pass reads. The write is clamped to the slot size (the
 * whole size of a single-instance buffer); an offset past it writes nothing.
 * Inside an active frame a ring buffer's write goes to the frame's slot and
 * generation (ring_write_slot); outside a frame it goes to every slot.
 * @param buf Host-mapped buffer, or NULL (ignored).
 * @param data Source bytes.
 * @param offset Byte offset within the slot.
 * @param bytes Byte count. */
void        rhi_update_buffer_range(rhi_buffer *buf, const void *data, size_t offset, size_t bytes);
/** @brief Unmap and free a buffer, its memory and its handle.
 *
 * Destroys immediately, so the caller guarantees no submitted frame still
 * reads the buffer (after rhi_wait_idle, or when it was never drawn). Every
 * buffer the RHI creates is host-mapped, and the memory is unmapped before it
 * is freed.
 * @param dev Device that created the buffer.
 * @param buf Buffer to destroy, or NULL (ignored). */
void        rhi_destroy_buffer(rhi_device *dev, rhi_buffer *buf);
/** @brief Grow a ring vertex buffer so each frame slot holds at least `bytes`, keeping the handle.
 *
 * Applies to rhi_create_vertex_buffer_dynamic and _gens buffers. The slot
 * stride doubles until it fits. The new allocation starts as a copy of every
 * slot and generation; the replaced one goes on the retire list and stays
 * alive until no submitted frame can read it, so a draw recorded before the
 * growth keeps its data. Called from the thread that owns the display.
 * @param dev Device owning the buffer.
 * @param buf Buffer to grow, or NULL.
 * @param bytes Required bytes per slot.
 * @return true when each slot holds @p bytes; false for NULL, when the
 *         allocation fails (the buffer is unchanged and writes past its slot
 *         clamp), or for a single-instance buffer smaller than @p bytes. */
bool        rhi_vertex_buffer_reserve(rhi_device *dev, rhi_buffer *buf, size_t bytes);
/** @brief Return the bytes one frame slot of a ring vertex buffer holds.
 *
 * @param buf Buffer, or NULL.
 * @return The slot stride; the whole size of a single-instance buffer; 0 for
 *         NULL. */
size_t      rhi_vertex_buffer_slot_bytes(const rhi_buffer *buf);

/** @brief Create a graphics pipeline from a description, its shaders named by .spv filename.
 *
 * The pipeline keeps a copy of the desc and the shader names, is compiled
 * against the device's depth convention and the specialization constants set
 * at this call, and is registered so a live rebuild (rhi_rebuild_request) can recompile it.
 * Shaders load from OPENPETE_SHADER_DIR when set, else from the embedded
 * blobs.
 * @param dev Device the pipeline is created on.
 * @param desc Pipeline description, copied.
 * @return The pipeline, or NULL on allocation failure or when a shader or the
 *         pipeline cannot be created (logged). */
rhi_pipeline *rhi_create_pipeline(rhi_device *dev, const rhi_pipeline_desc *desc);
/** @brief Unlink a pipeline from the rebuild registry and free its Vulkan objects and copies.
 *
 * Destroys immediately, so the caller guarantees no submitted frame still
 * uses the pipeline (after rhi_wait_idle or during setup failure). Display
 * owner only: the unlink is seam-checked. A pipeline frames may still use
 * goes through rhi_retire_pipeline instead.
 * @param dev Device that created the pipeline.
 * @param pipe Pipeline to destroy, or NULL (ignored). */
void          rhi_destroy_pipeline(rhi_device *dev, rhi_pipeline *pipe);

/** @brief Return whether the SCENE depth convention is reversed on this device.
 *
 * Reversed is z_ndc = 1/vz, GREATER_OR_EQUAL, clear 0 (the D32_SFLOAT_S8
 * format); forward, on the D24_UNORM_S8 fallback, is z_ndc = 1 - 1/vz,
 * LESS_OR_EQUAL, clear 1. The format selects all three together;
 * FORWARD-class images are forward on every device. The value is
 * specialization constant 0 (DEPTH_REVERSED) of every SCENE-class pipeline.
 * @param dev Device, or NULL.
 * @return true when reversed; false when forward or for NULL. */
bool          rhi_depth_reversed(const rhi_device *dev);

/** @brief Return the depth convention of the pipeline last bound in the open frame: true when reversed.
 *
 * The value specialization constant 0 (DEPTH_REVERSED) takes for that
 * pipeline: the device's convention for the pipeline's depth class
 * (rhi_pipeline_desc.depth_class). A stage that reads the convention from a
 * push constant instead of the specialization constant (terrain.vert) is
 * pushed this value, so one compiled stage serves both classes and writes
 * the depth the specialized stage would. False before any bind.
 * @param dev Device.
 * @return true when the bound pipeline's depth class is reversed. */
bool          rhi_bound_depth_reversed(const rhi_device *dev);

/* rhi_depth_format and rhi_depth_pipelines_by_compare are the depth
 * convention's engagement counters; rhi_depth_report logs both. */
/** @brief Return the chosen depth attachment format as a VkFormat value.
 *
 * @param dev Device, or NULL.
 * @return D32_SFLOAT_S8_UINT or D24_UNORM_S8_UINT; 0 for NULL. */
int           rhi_depth_format(const rhi_device *dev);
/** @brief Return how many depth-tested pipeline compiles used a VkCompareOp.
 *
 * Counts every pipeline_compile with depth testing on, including rebuilds,
 * under the compare op it selected: ALWAYS for a depth_overwrite pipeline,
 * else its depth class's compare op.
 * @param dev Device, or NULL.
 * @param compare_op VkCompareOp value, 0..7.
 * @return The count; 0 for NULL or a value outside 0..7. */
unsigned      rhi_depth_pipelines_by_compare(const rhi_device *dev, int compare_op);
/** @brief Log the depth format, both depth classes' compare op and clear value, and the per-compare pipeline counts.
 *
 * The counts cover the LESS_OR_EQUAL, GREATER_OR_EQUAL and ALWAYS compare ops.
 * @param dev Device, or NULL (nothing is logged). */
void          rhi_depth_report(const rhi_device *dev);

/** @brief Select PSX-affine UV and colour interpolation (vertex specialization constant 1).
 *
 * Affine interpolation (w = 1) reproduces the PSX GPU's screen-linear
 * texture and gouraud swim. Applies to pipelines created after this call:
 * set at bring-up from --texture-mapping before the scene pipelines are
 * built; a live toggle goes through rhi_rebuild_request. Global.
 * @param on Nonzero for affine; 0 (the default) for perspective-correct. */
void          rhi_set_affine_uv(int on);

/** @brief Select the untextured lighting view (fragment specialization constant 2, NOTEX).
 *
 * The textured scene fragments replace the sampled texel with the
 * modulation-neutral grey (128/255), so the output is the raw gouraud vertex
 * colour; transparency cutouts keep their real texel shapes. Applies to
 * pipelines created after this call; a live toggle goes through
 * rhi_rebuild_request. Global.
 * @param on Nonzero to enable; 0 is the default. */
void          rhi_set_notex(int on);

/** @brief Select the wireframe view for pipelines created after this call.
 *
 * Every scene pipeline rasterizes its triangles as their three edges
 * (VK_POLYGON_MODE_LINE, line width 1) and carries the NOTEX
 * specialization, so each edge is coloured by the interpolated gouraud
 * vertex colour. A scene pipeline is one that tests depth, or one marked
 * wire_scene; a colour-write-off pass that does not overwrite depth (the
 * opaque-depth pre-pass, the portal aperture mask) stays filled, so the
 * occlusion grid, the decals' occlusion test and the aperture stencil keep
 * whole-face coverage. Depth and stencil state is unchanged: a line pixel
 * tests and writes depth like the fill it replaces, so a line is hidden
 * only by a nearer line (the decals, whose fragment stage samples the
 * filled pre-pass depth, also by nearer faces). On a device without
 * fillModeNonSolid the pipelines stay filled and only the NOTEX colouring
 * applies. A live toggle goes through rhi_rebuild_request. Global.
 * @param on Nonzero to enable; 0 is the default. */
void          rhi_set_wireframe(int on);

/** @brief Select key-contest scoping (fragment specialization constant 3, MOBY_TRUESCOPE).
 *
 * On, a moby og-key yield additionally requires the key to beat the
 * covering fragment's own true view-Z. Applies to pipelines created after
 * this call; a live toggle goes through rhi_rebuild_request. Global.
 * @param on Nonzero (the default) to scope; 0 restores og's compound
 *           painter. */
void          rhi_set_moby_truescope(int on);
/** @brief Select the key contest's unit (fragment specialization constant 4, SLOT_CONTEST).
 *
 * On, the key contest compares 128-world ordering-table slot indices instead
 * of world distances for terrain against terrain, an equal slot keeping the
 * fragment; an actor face is decided against terrain by true depth. Applies to
 * pipelines created after this call. Global.
 * @param on Nonzero to compare slot indices; 0 compares world distances. */
void          rhi_set_slot_contest(int on);

/** @brief Serialize an environment write against compile runs: 1 waits until no compile runs and holds new runs off, 0 releases.
 *
 * Vulkan drivers call getenv inside vkCreateGraphicsPipelines, on whichever
 * thread compiles (rhi_compile_job_run holds the guard's read side for the
 * create), and the pipeline-cache save worker's file commit reads the
 * environment (rhi_env_read_guard); setenv on another thread may move the
 * environment under them. A thread that writes the environment while
 * compiles may run brackets the write with guard(1) and guard(0). A
 * waiting write holds off new compile runs. Global; any thread.
 * @param begin 1 before the write, 0 after it. */
void          rhi_env_write_guard(int begin);

/** @brief Hold off environment writes around an environment read on a thread other than the writer's: 1 before the read, 0 after.
 *
 * The short read side of rhi_env_write_guard: it waits only while a write
 * is in progress, never behind a waiting write, so it never waits for a
 * compile run. A thread that reads the environment while a writer may run
 * (a file commit's fault-injection lookup, op_atomic_file_set_env_guard)
 * brackets the read with it. Global; any thread.
 * @param begin 1 before the read, 0 after it. */
void          rhi_env_read_guard(int begin);

/* Site classes of the pipeline-create instruments (rhi_pipe_census.h): the
 * class a create is counted in, and the lazy sites whose visits are each
 * class's engagement count. */
typedef enum {
    RHI_PIPE_SITE_BUILTIN = 0,       /* built-in pipeline created eagerly          */
    RHI_PIPE_SITE_TERRAIN_CACHE,     /* built-in terrain-cache pipeline            */
    RHI_PIPE_SITE_BUILTIN_LAZY,      /* built-in postfx compiled at first draw     */
    RHI_PIPE_SITE_MOD_POSTFX,        /* mod postfx pass                            */
    RHI_PIPE_SITE_MOD_MATERIAL,      /* mod material on scene, moby, decal, portal */
    RHI_PIPE_SITE_TERRAIN_MATERIAL,  /* mod material on a terrain-cache bucket     */
    RHI_PIPE_SITE_REBUILD,           /* a live rebuild (rhi_rebuild_request)        */
    RHI_PIPE_SITES
} rhi_pipe_site;

/** @brief Count visits of a lazy pipeline site: draws that reached the slot test of that class, whether or not they compile.
 *
 * Safe on any thread.
 * @param site The site's class.
 * @param count Visits to add. */
void          rhi_pipe_site_visit(rhi_pipe_site site, int count);

/* Explicit create phase for rhi_pipe_phase_set. */
typedef enum {
    RHI_PIPE_PHASE_AUTO = 0,   /* bring-up before the first frame, first use after */
    RHI_PIPE_PHASE_PRECOMPILE, /* the precompile                                   */
    RHI_PIPE_PHASE_PROBE,      /* warm-boot probe creates, counted apart           */
    RHI_PIPE_PHASE_LAUNCHER    /* the launcher: its ahead walk and its passes      */
} rhi_pipe_phase;

/** @brief Set the phase the calling thread's pipeline creates are counted in until the next call.
 *
 * Per thread. Creates of a live rebuild count as rebuild in
 * every phase but PROBE.
 * @param phase The phase. */
void          rhi_pipe_phase_set(rhi_pipe_phase phase);

/** @brief Return the phase the calling thread's pipeline creates are counted in (rhi_pipe_phase_set).
 *
 * @return The phase. */
rhi_pipe_phase rhi_pipe_phase_get(void);

/** @brief Install the clock the RHI stamps its boot events with, in ns.
 *
 * Process-global; call before rhi_create. Without one the events read 0.
 * @param now_ns The clock. */
void          rhi_set_clock(int64_t (*now_ns)(void));

/* Boot facts of the most recently created device, on the rhi_set_clock clock. */
typedef struct {
    int64_t  device_created_ns;  /* rhi_create returned the device              */
    int64_t  first_frame_ns;     /* the first frame was submitted               */
    int64_t  cache_load_ns;      /* reading the cache file and vkCreatePipelineCache */
    uint64_t cache_file_bytes;   /* the cache file's size at open, 0 when absent */
    uint64_t cache_offered_bytes;/* bytes offered to the driver                 */
    int      cache_cold;         /* the file was removed at open (OPENPETE_PIPELINE_CACHE_COLD) */
    unsigned bringup_creates;    /* pipeline creates before the first frame     */
    uint64_t bringup_ns;         /* their summed wall time                      */
    unsigned bringup_known;      /* of them, creates of a create info the cache held */
    unsigned precompile_creates; /* pipeline creates in the precompile phase    */
    unsigned precompile_known;   /* of them, creates of a create info the cache held */
    unsigned devices_created;    /* devices rhi_create and rhi_create_headless returned */
} rhi_boot_stats;

/** @brief Read the boot facts.
 *
 * @param out Receives them. */
void          rhi_boot_stats_get(rhi_boot_stats *out);

/** @brief Mark the frames submitted from now on as launcher frames, or end the marking.
 *
 * A launcher frame is drawn before the game's renderer is brought up; it
 * sets no first-frame boot fact and does not end the bring-up create phase,
 * so those instruments describe the game's first frame whether or not the
 * launcher drew first.
 * @param on true while the launcher draws. */
void          rhi_boot_launcher_frames(bool on);

/** @brief Return whether the launcher hand-off trace (OPENPETE_LAUNCHER_TRACE) logged a present and has not yet logged the game's first.
 *
 * The run loop holds a --frame-limit exit while this holds, so the trace
 * reaches the game's first present however late the present thread makes
 * it. Any thread.
 * @return true while the trace waits for the game's first present; false
 *         when the trace is off, no windowed device presented or the game's
 *         first present was logged. */
bool          rhi_launcher_trace_pending(void);

/** @brief Return the reason the last failed rhi_create or rhi_create_headless gave.
 *
 * A sentence naming the step that failed (no Vulkan loader, the instance,
 * the surface, no listed device, no device that can present to the window,
 * the device, the swapchain or a later resource), for a message to the
 * player. Process-global; the text stays until the next failure.
 * @return The reason, or "" when no create has failed. */
const char   *rhi_create_error(void);

/** @brief Name the directory where devices created afterwards persist their Vulkan pipeline cache, the engine build, and the function that writes the file.
 *
 * Each device loads vk-pipelines-<build>-<vendor>-<device>-<cache uuid>.bin
 * from it at creation (<build> a digest of @p build and the embedded
 * SPIR-V, so engine builds keep separate files), offering the driver only a
 * file whose envelope (length, digest, driverVersion) and Vulkan header
 * match. The file carries a key manifest of the pipeline variants created
 * into it. It is rewritten through write_file after a precompile and at
 * rhi_destroy when the cache bytes or the manifest changed. Each open
 * refreshes the file's mtime and removes the other cache files of its class
 * (windowed, or headless: rhi_set_pipeline_cache_sync_class) unused for 30
 * days or ranked beyond the 4 most recently used. A NULL or ""
 * dir, or a NULL write_file, keeps the cache in memory only (the default).
 * Process-global; call before rhi_create.
 * @param dir Existing directory, or NULL.
 * @param build The engine build string, or NULL.
 * @param write_file Replaces path with n bytes in one step; returns 0 on success. */
void          rhi_set_pipeline_cache_dir(const char *dir, const char *build,
                                         int (*write_file)(const char *path, const void *data, size_t n));

/* Longest line rhi_create_hooks.begin receives, terminator included. */
#define RHI_CREATE_LINE_MAX 768

/* Callbacks around every driver call that creates a graphics pipeline (a
 * whole pipeline, a graphics pipeline library part or a link) or a shader
 * module, run on the creating thread: the display owner, a compile pool
 * worker or the launcher's ahead walk. */
typedef struct {
    /* RHI_CREATE_PIPELINE: a pipeline create begins, before its shader
     * modules are made. line is one line without a newline: the create's
     * sequence number, kind, site class, phase and create flags, each
     * stage's .spv name, SPIR-V size, content hash and specialization
     * constants, and the fixed-function state. RHI_CREATE_STEP: the call
     * in progress inside that create changes: a shader module create
     * (its name, size and hash, then the pipeline's line), or the return
     * to the pipeline's own line before the driver's pipeline call. */
    void (*begin)(int what, const char *line);
    /* After the pipeline create returns, or fails before its call. */
    void (*end)(void);
} rhi_create_hooks;

enum { RHI_CREATE_PIPELINE = 0, RHI_CREATE_STEP = 1 };

/** @brief Register the callbacks run around every pipeline and shader module create; NULL clears them.
 *
 * Process-global; call before rhi_create, with no create in flight. The
 * lines are formatted only while callbacks are registered.
 * @param hooks The callbacks, copied; NULL or a NULL begin clears both. */
void          rhi_set_create_hooks(const rhi_create_hooks *hooks);

/** @brief Put devices created afterwards in the headless cache-file class: their file name ends in -headless.bin.
 *
 * A headless device is in that class without this call. A synchronous run
 * (deterministic capture) sets it so a harness never writes a windowed
 * run's file. Process-global; call before rhi_create.
 * @param on true for the headless class. */
void          rhi_set_pipeline_cache_sync_class(bool on);

/* What rhi_precompile_variants did. */
typedef struct {
    unsigned bases;         /* registered pipelines walked                       */
    unsigned keys;          /* distinct create infos over the walked variants     */
    unsigned current;       /* of them, a registered pipeline's installed variant */
    unsigned queued;        /* of them, variants the walk is to probe or create   */
    unsigned created;       /* variants compiled into the cache and destroyed     */
    unsigned probe_hits;    /* variants a probe create found in the cache         */
    unsigned manifest_skips;/* variants the key manifest skipped                  */
    unsigned failed;        /* variant creates that failed                        */
    unsigned covered;       /* queued variants a live rebuild compiled first      */
    unsigned gone;          /* queued variants whose pipeline was destroyed first */
    unsigned duplicates;    /* variants a live rebuild requested while the walk's
                             * job for the same create info was out            */
    int      saved;         /* the cache file was rewritten afterwards            */
    double   ms;            /* wall time                                          */
} rhi_precompile_stats;

/** @brief Compile every walked non-current variant of every registered pipeline into the pipeline cache, then save the cache when it changed.
 *
 * The walked variants are the texture-mapping dimension (perspective,
 * affine) at the textured view; a notex or wireframe view compiles when it
 * is toggled (rhi_rebuild_request). The enumeration is the one
 * rhi_precompile_walk_start runs, with up to four jobs per worker queued
 * at once. Each variant's create info is keyed on the full
 * pipeline description (rhi_pc_variant_key); a key already seen, including
 * every registered pipeline's installed key, is not created again. A new
 * key is skipped when a probe create
 * (VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT, on a device
 * with pipelineCreationCacheControl) finds it cached, or, without cache
 * control on a calibrated driver, when the key manifest lists it; otherwise
 * it is created and destroyed, never linked. The probes and creates run on
 * the compile pool (rhi_compile_pool_submit), each worker destroying what it
 * created; the call returns when every one has run. @p pump runs between
 * submits and waits. Display owner only.
 * @param dev Device.
 * @param pump Called between submits and waits (event pumping), or NULL.
 * @param ctx Passed to @p pump.
 * @param out Receives the counts, or NULL.
 * @return 0, or -1 for a NULL device. */
int           rhi_precompile_variants(rhi_device *dev, void (*pump)(void *ctx), void *ctx,
                                      rhi_precompile_stats *out);

/** @brief Read the progress of the precompile in flight: compiles finished and queued in the open pipeline batch and the variant walk.
 *
 * The variant walk's total is known before its first submit. Display owner
 * only (the pump of rhi_pipeline_batch_end or rhi_precompile_variants).
 * @param dev Device, or NULL (zeros).
 * @param done Receives the finished count, or NULL.
 * @param total Receives the queued count, or NULL. */
void          rhi_precompile_progress(const rhi_device *dev, unsigned *done, unsigned *total);

/** @brief Mark the precompile ended: pipeline creates after it count as first use, before the first frame too. */
void          rhi_pipe_precompile_done(void);

/* State of the device's background variant walk. */
typedef enum {
    RHI_WALK_IDLE,     /* never started                               */
    RHI_WALK_RUNNING,  /* started; variants left to submit or return  */
    RHI_WALK_DONE,     /* every variant returned, or the pool stopped */
} rhi_walk_state;

/** @brief Start the background variant walk: enumerate the variants rhi_precompile_variants visits, to be submitted one at a time by rhi_precompile_walk_step.
 *
 * The enumeration identifies each pipeline by its registry serial, never
 * by pointer, so a pipeline destroyed before its turn is skipped. Only
 * pipelines registered before this call are walked. Display owner only.
 * @param dev Device.
 * @return The variants queued, or -1 for a NULL device or a walk already running. */
int            rhi_precompile_walk_start(rhi_device *dev);

/** @brief Advance the background walk: when its job has returned, submit the next variant, built from its pipeline now, in the precompile create phase.
 *
 * At most one walk job is in the compile pool at a time, so first-use and
 * rebuild jobs wait behind at most one. A variant a live rebuild request
 * covered (rhi_rebuild_request) is skipped. When the last job returns the
 * walk logs one line, "background walk: keys K, created C, probe hits H,
 * duplicates D, M ms", and arms the debounced off-thread cache save. Call
 * on the display owner once per present, or for the ahead walk after its
 * wake (rhi_precompile_ahead_start); a no-op unless a walk is running.
 * @param dev Device, or NULL.
 * @return The walk's state after the step. */
rhi_walk_state rhi_precompile_walk_step(rhi_device *dev);

/** @brief Read the background walk's state and progress.
 *
 * Display owner only.
 * @param dev Device, or NULL (RHI_WALK_IDLE, zeros).
 * @param done Receives the variants returned or skipped, or NULL.
 * @param total Receives the variants queued, or NULL.
 * @param st Receives the counts so far, or NULL.
 * @return The walk's state. */
rhi_walk_state rhi_precompile_walk_get(const rhi_device *dev, unsigned *done, unsigned *total,
                                       rhi_precompile_stats *st);

/** @brief Open description mode: until rhi_pipeline_describe_end, rhi_create_pipeline_slot records each description instead of creating its pipeline.
 *
 * Display owner only. Inside it rhi_create_pipeline_slot copies a
 * description whose stages are both .spv names (one given SPIR-V words is
 * counted and skipped), queues nothing and sets the slot to a non-NULL
 * marker that is never bound, read or destroyed, so a builder's tests for a
 * created pipeline pass and it describes every pipeline it would create.
 * The descriptions feed rhi_precompile_ahead_start.
 * @param dev Device. */
void           rhi_pipeline_describe_begin(rhi_device *dev);

/** @brief Close description mode.
 *
 * Display owner only.
 * @param dev Device.
 * @return The descriptions recorded on the device so far. */
unsigned       rhi_pipeline_describe_end(rhi_device *dev);

/** @brief Close description mode and drop the descriptions recorded since its rhi_pipeline_describe_begin, returning how many pipeline creates it saw.
 *
 * The count is every rhi_create_pipeline_slot call inside the mode: the
 * descriptions recorded plus the ones given SPIR-V words (counted, never
 * recorded). Descriptions recorded before that begin stay. A pass that
 * only counts the creates a builder would make uses it in place of
 * rhi_pipeline_describe_end. Display owner only.
 * @param dev Device.
 * @return The creates seen; 0 for a NULL device. */
unsigned       rhi_pipeline_describe_discard(rhi_device *dev);

/* Groups of the ahead walk's descriptions (rhi_pipeline_describe_group). */
#define RHI_AHEAD_GROUPS 4u

/** @brief Set the ahead-walk group of the descriptions recorded from now on, the order in which rhi_precompile_ahead_start queues them.
 *
 * rhi_pipeline_describe_begin resets the group to 0. A group at or past
 * RHI_AHEAD_GROUPS reads as the last. Display owner only.
 * @param dev Device.
 * @param group The group, 0 queued first. */
void           rhi_pipeline_describe_group(rhi_device *dev, unsigned group);

/** @brief Start the ahead walk: compile every recorded description into the pipeline cache on the compile pool, before any pipeline of the game exists.
 *
 * The walk is the background variant walk's state machine
 * (rhi_precompile_walk_step advances it, rhi_precompile_walk_get reads it)
 * over the descriptions rhi_pipeline_describe_begin recorded instead of the
 * registry: each description at both SLOT_CONTEST values, at the textured
 * view and the current MOBY_TRUESCOPE, each key once, queued by texture
 * mapping (the one the game will draw first before the other), then by
 * group (rhi_pipeline_describe_group), then in the order recorded. A key is
 * skipped as the variant walk skips one (probe hit, manifest), and its
 * creates count in the launcher create phase. Up to two jobs per worker are
 * out at a time. The first texture mapping's groups 0 and 1 return before
 * any later create info is submitted, and group 1's library parts are
 * queued on the pool together once group 0 returned. Each return of a walk job calls @p wake on the compile
 * worker that ran it, so the display owner steps the walk
 * (rhi_precompile_walk_step) when a job returns rather than on a present;
 * the step after the last return of a group's first texture mapping logs
 * "launcher walk: <mapping> group G returned". Display owner only.
 * @param dev Device.
 * @param affine_first true to queue the affine create infos before the
 *        perspective ones: the effective texture mapping is affine.
 * @param wake Called on a compile worker, with no pool lock held, after
 *        each walk job returns; NULL for none.
 * @return The create infos queued, or -1 for a NULL device, a walk already
 *         running or no recorded description. */
int            rhi_precompile_ahead_start(rhi_device *dev, bool affine_first, void (*wake)(void));

/** @brief Stop the ahead walk: submit nothing more, drop its jobs still queued on the pool, and log what it did.
 *
 * Jobs a worker holds run to the end into the cache; a create of the same
 * key submitted meanwhile waits for it on its worker (rhi_compile_pool_submit).
 * The keys it did not reach are compiled by bring-up's creates and the
 * game's background walk. No-op unless the ahead walk runs. Display owner
 * only.
 * @param dev Device, or NULL. */
void           rhi_precompile_ahead_stop(rhi_device *dev);

/** @brief Return the groups of the ahead walk's first texture mapping whose return the walk logged ("launcher walk: <mapping> group G returned"), bit G per group.
 *
 * The bits outlive the walk's end and are cleared by the next walk's start.
 * Display owner only.
 * @param dev Device, or NULL (0).
 * @return The bit set. */
unsigned       rhi_precompile_ahead_returned(const rhi_device *dev);

/* ---- variant-explicit compile jobs ------------------------------------
 * A compile job owns every input of one pipeline compile: a copy of the
 * desc, each stage's SPIR-V bytes resolved when the job is made (a mod
 * stage's words copied, a built-in stage's .spv name resolved to the
 * embedded blob, held by pointer, or to the bytes of the
 * OPENPETE_SHADER_DIR file, read then), and the variant it compiles for.
 * Jobs are made on the display owner (rhi_display_claim); a job runs on
 * any thread, calls no getenv, opens no file and touches no registry; its
 * result is a bare pipeline and layout with the push-constant stage flags,
 * with no rhi_pipeline. Only rhi_compile_job_install, on the display
 * owner, wraps a result in an rhi_pipeline and links it into the rebuild
 * registry; a result never installed is destroyed by
 * rhi_compile_job_discard on the thread holding it. */

/* Variant inputs of one compile, the creation-time state the variant
 * setters (rhi_set_affine_uv, rhi_set_notex, rhi_set_wireframe,
 * rhi_set_moby_truescope) select for every pipeline created after them. */
typedef struct {
    bool affine;      /* AFFINE_UV: PSX-affine texture mapping */
    bool notex;       /* NOTEX: the untextured view */
    bool wireframe;   /* the wireframe view */
    bool truescope;   /* MOBY_TRUESCOPE */
} rhi_pipe_variant;

typedef struct rhi_compile_job rhi_compile_job;

/* rhi_compile_job_run flags. */
enum {
    RHI_COMPILE_PROBE   = 1u << 0,  /* fail instead of compiling an uncached create info */
    RHI_COMPILE_VARIANT = 1u << 1   /* a precompile variant: left out of the depth report's count */
};

/** @brief Return the variant the setters currently select.
 *
 * Display owner only: the setters write it there.
 * @return The variant. */
rhi_pipe_variant rhi_pipe_variant_current(void);

/** @brief Return the variant epoch: a counter every variant setter bumps when it changes its value, and every rebuild request bumps.
 *
 * @return The epoch. */
uint32_t      rhi_pipe_variant_epoch(void);

/** @brief Make a compile job for a new pipeline.
 *
 * Display owner only. Copies the desc and the stage words (a stage given
 * words has its .spv name cleared), resolves every stage to bytes (see the
 * block comment above) and records the stages' content hashes and declared
 * specialization constant ids.
 * @param dev Device the job compiles on.
 * @param desc Pipeline description, copied.
 * @param vert_words Vertex SPIR-V words, or NULL to use desc->vert_spv.
 * @param vert_n Word count of @p vert_words.
 * @param frag_words Fragment SPIR-V words, or NULL to use desc->frag_spv.
 * @param frag_n Word count of @p frag_words.
 * @param variant Variant the job compiles for.
 * @param generation The enqueuer's slot generation, returned by rhi_compile_job_generation.
 * @param epoch The variant epoch the job compiles for, returned by rhi_compile_job_epoch.
 * @return The job, or NULL on allocation failure or a stage with no source (logged). */
rhi_compile_job *rhi_compile_job_new(rhi_device *dev, const rhi_pipeline_desc *desc,
                                     const uint32_t *vert_words, size_t vert_n,
                                     const uint32_t *frag_words, size_t frag_n,
                                     rhi_pipe_variant variant, uint32_t generation,
                                     uint32_t epoch);

/** @brief Make a compile job that recompiles a registered pipeline from its stored desc and shader sources.
 *
 * Display owner only. The stage bytes are resolved as rhi_compile_job_new
 * resolves them; the stage hashes and constant ids are the pipeline's.
 * @param dev Device the pipeline lives on.
 * @param pipe The pipeline.
 * @param variant Variant the job compiles for.
 * @param generation The enqueuer's slot generation.
 * @param epoch The variant epoch the job compiles for.
 * @return The job, or NULL on allocation failure or a stage with no source. */
rhi_compile_job *rhi_compile_job_of(rhi_device *dev, const rhi_pipeline *pipe,
                                    rhi_pipe_variant variant, uint32_t generation,
                                    uint32_t epoch);

/** @brief Set the SLOT_CONTEST value a job compiles with, in place of the process-wide value it took at its making (rhi_set_slot_contest).
 *
 * Call before the job runs; the process-wide value is left as it is.
 * @param job The job; NULL is ignored.
 * @param on Nonzero to compare slot indices. */
void          rhi_compile_job_set_slot_contest(rhi_compile_job *job, int on);

/** @brief Compile a job's pipeline and layout through the device's pipeline cache.
 *
 * Any thread; the device must outlive the call. A job that already holds a
 * result keeps it and returns 0. The create is counted by the pipeline
 * census, the creation-feedback tally and the first-use hitch metric, and
 * runs inside the read side of rhi_env_write_guard. Not called by a thread
 * inside rhi_env_write_guard or rhi_env_read_guard.
 * @param dev Device the job was made for.
 * @param job The job.
 * @param flags RHI_COMPILE_* bits.
 * @return 0 on success, 1 when a probe found the create info uncached, -1 on failure. */
int           rhi_compile_job_run(rhi_device *dev, rhi_compile_job *job, unsigned flags);

/** @brief Return the slot generation a job was made with.
 *
 * @param job The job.
 * @return The generation. */
uint32_t      rhi_compile_job_generation(const rhi_compile_job *job);

/** @brief Return the variant epoch a job was made with.
 *
 * @param job The job.
 * @return The epoch. */
uint32_t      rhi_compile_job_epoch(const rhi_compile_job *job);

/** @brief Return whether a variant switch since a job was made changed its create info.
 *
 * Display owner only. The job is stale when its create info under the
 * variant drawn now (rhi_pipe_variant_current, which a rebuild's swap
 * changes without an epoch bump) differs from the one it was compiled for;
 * a switch that leaves its create info alone (its stages declare none of
 * the switched constants) does not make it stale.
 * @param dev Device the job was made for.
 * @param job The job.
 * @return true when the result must not be installed. */
bool          rhi_compile_job_variant_stale(const rhi_device *dev, const rhi_compile_job *job);

/** @brief Point a job at the variant drawn now and the current epoch, destroying any result it holds.
 *
 * Display owner only. The job keeps its desc, stage bytes and generation,
 * so it can be submitted again without the registry it was made from.
 * @param dev Device the job was made for.
 * @param job The job. */
void          rhi_compile_job_retarget(rhi_device *dev, rhi_compile_job *job);

/** @brief Wrap a job's result in an rhi_pipeline, link it into the rebuild registry and free the job.
 *
 * Display owner only. The pipeline takes the job's desc, stage sources and
 * push-constant stage flags. A job with no result (never run, or its run
 * failed) is discarded and NULL returned.
 * @param dev Device the job was made for.
 * @param job The job, consumed.
 * @return The pipeline, or NULL. */
rhi_pipeline *rhi_compile_job_install(rhi_device *dev, rhi_compile_job *job);

/** @brief Destroy a job's result as Vulkan objects and free the job.
 *
 * Any thread. The result was never linked or recorded in a command buffer,
 * so it is destroyed at once, with no device idle and no registry access.
 * @param dev Device the job was made for.
 * @param job The job, consumed, or NULL (ignored). */
void          rhi_compile_job_discard(rhi_device *dev, rhi_compile_job *job);

/** @brief Retire a registered pipeline: unlink it, free it, and destroy its Vulkan objects once the frames that may have bound them retire.
 *
 * Display owner only. The pipeline and layout join the list the live
 * rebuild's replaced objects use, stamped with the current frame serial,
 * so no device idle runs (one does only when that list's allocation
 * fails). The rhi_pipeline is freed here: the handle is invalid after the
 * call.
 * @param dev Device that created the pipeline.
 * @param pipe The pipeline, or NULL (ignored). */
void          rhi_retire_pipeline(rhi_device *dev, rhi_pipeline *pipe);

/* What the debounced first-use cache save did (rhi_pcache_autosave_stats). */
typedef struct {
    unsigned triggers;   /* quiet periods reached after first-use creates     */
    unsigned same_size;  /* of them, skipped: the cache size was unchanged     */
    unsigned started;    /* snapshots handed to a save worker                 */
    unsigned written;    /* saves the worker wrote                            */
    unsigned joined_at_shutdown; /* in-flight saves shutdown waited for      */
} rhi_pcache_autosave_stats;

/** @brief Read the debounced first-use cache save's counts for the device.
 *
 * @param dev Device, or NULL (zeros).
 * @param out Receives the counts. */
void          rhi_pcache_autosave_stats_get(const rhi_device *dev, rhi_pcache_autosave_stats *out);

/* ---- compile pool -----------------------------------------------------
 * Worker threads run compile jobs from a locked queue. A job submitted with
 * an install callback enters a locked staging list when its run returns;
 * the display owner drains the list (rhi_compile_pool_drain,
 * rhi_compile_pool_wait, rhi_rebuild_poll) and calls each callback, which
 * installs or discards the result. A job submitted with no callback is a
 * precompile variant: the worker runs it (a probe first with
 * RHI_COMPILE_PROBE), destroys the result and counts the outcome, so its
 * objects never leave the worker. Each run holds off environment writes
 * (rhi_env_write_guard). The steady worker count is OPENPETE_COMPILE_POOL
 * when set (0 runs every job on the display owner at submit), else half the
 * logical core count, at least one; it is read when the pool starts, at
 * the first submit. Without OPENPETE_COMPILE_POOL a pool started before the
 * process's first tick (rhi_compile_pool_first_tick) starts one worker per
 * logical core. Items no wait of the boot is on are background items: the
 * background variant walk, the launcher's ahead walk, the link-time
 * optimized links and their library parts, and host tasks submitted with
 * rhi_compile_pool_submit_background_task. At most the steady count of the
 * pool's own workers run background items at once, in either phase, so the
 * workers beyond it run only the bring-up compiles the boot waits on, and
 * at the first tick at most the steady count of background items are in
 * flight. From the first tick on a worker takes any item only while fewer
 * than the steady count of the pool's own workers run one, so the extra
 * workers park and an item running at the step-down finishes.
 * OPENPETE_COMPILE_POOL is one fixed count for both phases. With an empty
 * driver cache, workers compiling variants
 * that share a stage each compile that stage where one worker would reuse
 * it from the pipeline cache, so the compile work grows with the worker
 * count, and each create slows as workers are added. Measured driver-cold
 * on the pipeline-library bring-up: ANV on 8 threads finishes the compile
 * stage in 40.4 s at four workers and 29-30.5 s at eight; RADV on 6 cores
 * and 12 threads takes 4.2-4.3 s at four workers and 3.9-4.05 s at six,
 * eight or twelve, its creates 33% slower at six than at four and 11% more
 * at twelve. A fixed pool of twelve after the first tick raises RADV's
 * present p99 at 60 Hz from 16.8 to 20-22 ms, which the step-down avoids. */

/* The rc an install callback receives for a job the pool's stop dropped:
 * the callback discards the job and releases its tag. */
#define RHI_POOL_CANCELLED (-2)

/* Install callback: display owner; owns @p job (installs or discards it).
 * @p rc is rhi_compile_job_run's result, or RHI_POOL_CANCELLED. */
typedef void (*rhi_pool_install_fn)(rhi_device *dev, rhi_compile_job *job, int rc, void *tag);

/* What the compile pool did since it started. */
typedef struct {
    unsigned workers;        /* worker threads (0: jobs run at submit)           */
    unsigned workers_steady; /* of them, the most running an item at once from the first tick */
    unsigned submitted;      /* jobs submitted                                   */
    unsigned run;            /* jobs run                                         */
    unsigned staged;         /* results handed to the display owner              */
    unsigned variant_created, variant_probe_hits, variant_failed; /* callback-less jobs */
    unsigned cancelled;      /* jobs the stop dropped, run or not                */
    unsigned max_queued;     /* deepest queue                                    */
    unsigned same_key_waits; /* jobs that waited for another worker's run of the same create info */
    double   run_ms;         /* summed run time over the workers                 */
    unsigned tasks_submitted, tasks_run; /* host tasks (rhi_compile_pool_submit_task)  */
    double   tasks_ms;       /* their summed run time                            */
} rhi_compile_pool_stats;

/* A host task the pool runs on a worker (rhi_compile_pool_submit_task). */
typedef void (*rhi_pool_task_fn)(void *tag);

/** @brief Queue a compile job on the device's compile pool, starting the pool on first use.
 *
 * Display owner only. The job runs in the create phase the submitting
 * thread is in (rhi_pipe_phase_set, a rebuild). A worker that takes a job
 * whose create info another worker is compiling waits for that run and then
 * creates from the pipeline cache, so one create info never compiles twice
 * at once (counted as same_key_waits); a link-time-optimized link is a
 * create info apart from the fast link of the same pipeline. Without a pool (start
 * failure) the job runs at once and @p fn is called before return.
 * @param dev Device the job was made for.
 * @param job The job, consumed.
 * @param flags RHI_COMPILE_* bits; RHI_COMPILE_PROBE probes first and
 *              compiles on a probe miss.
 * @param fn Install callback, or NULL for a precompile variant.
 * @param tag Passed to @p fn.
 * @return 0, or -1 for a NULL device or job. */
int           rhi_compile_pool_submit(rhi_device *dev, rhi_compile_job *job, unsigned flags,
                                      rhi_pool_install_fn fn, void *tag);

/** @brief Queue a host task on the device's compile pool, behind the jobs already queued, starting the pool on first use.
 *
 * Display owner only. A worker runs @p run with @p tag and stages
 * nothing: the task publishes its own result. A wait for the pool
 * (rhi_compile_pool_wait) waits for it. Without a worker (no pool, a pool
 * of none) the task runs before return; a task the pool's stop drops never
 * runs.
 * @param dev Device.
 * @param run The task.
 * @param tag Passed to @p run.
 * @return 0, or -1 for a NULL device or task. */
int           rhi_compile_pool_submit_task(rhi_device *dev, rhi_pool_task_fn run, void *tag);

/** @brief Queue a host task as rhi_compile_pool_submit_task does, as a background item: no wait of the boot is on it, so at most the pool's steady worker count run background items at once.
 *
 * Display owner only.
 * @param dev Device.
 * @param run The task.
 * @param tag Passed to @p run.
 * @return 0, or -1 for a NULL device or task. */
int           rhi_compile_pool_submit_background_task(rhi_device *dev, rhi_pool_task_fn run, void *tag);

/** @brief Call the install callback of every staged result.
 *
 * Display owner only.
 * @param dev Device.
 * @return The results handled. */
unsigned      rhi_compile_pool_drain(rhi_device *dev);

/** @brief Wait until no job is queued, running or staged, draining staged results and calling @p pump between waits.
 *
 * Display owner only.
 * @param dev Device.
 * @param pump Called between waits (event pumping), or NULL.
 * @param ctx Passed to @p pump. */
void          rhi_compile_pool_wait(rhi_device *dev, void (*pump)(void *ctx), void *ctx);

/** @brief Read the compile pool's counts; after rhi_compile_pool_stop, its counts at the stop.
 *
 * @param dev Device, or NULL (zeros).
 * @param out Receives the counts. */
void          rhi_compile_pool_stats_get(const rhi_device *dev, rhi_compile_pool_stats *out);

/** @brief Stop the compile pool: take no more jobs, join the workers, and drop every queued and staged job.
 *
 * Display owner only. A dropped job with an install callback is handed to
 * it with RHI_POOL_CANCELLED; one without is destroyed. A rebuild in flight
 * is abandoned and its compiled results destroyed. rhi_destroy calls it
 * before the device idles. A submit after the stop runs at once.
 * @param dev Device, or NULL. */
void          rhi_compile_pool_stop(rhi_device *dev);

/** @brief Pause or resume the compile pool: paused, each worker finishes the item it holds and takes no other, and the queue keeps the rest.
 *
 * Display owner only. A pool not started yet starts paused. A stop while
 * paused drops the queue as rhi_compile_pool_stop does; a wait for the pool
 * (rhi_compile_pool_wait, rhi_pipeline_batch_end) does not return while it
 * is paused with work queued, so only a caller that runs no batch pauses it.
 * @param dev Device, or NULL.
 * @param on true to pause, false to resume. */
void          rhi_compile_pool_pause(rhi_device *dev, bool on);

/** @brief Mark the process's first tick: from here each compile pool runs at most its steady worker count of items at once.
 *
 * Any thread; process-wide; only the first call acts. Workers read the
 * mark when they take an item, so an item running at the call finishes and
 * a worker over the steady count takes no other; a pool started after the
 * call starts its steady count of workers. Logs the step-down once.
 */
void          rhi_compile_pool_first_tick(void);

/** @brief Enter or leave the compile pool's low mode: in it the pool's own workers take no item and a separate set of low-priority workers runs the queue.
 *
 * Display owner only. The low workers (half the pool's steady count, at least 1)
 * start on the first entry and lower their own priority once; the pool's
 * workers keep theirs. A pause overrides the mode: paused, no worker of
 * either set takes an item. A pool not started yet starts in the mode. A
 * wait for the pool returns in the mode, at the low workers' rate.
 * @param dev Device, or NULL.
 * @param on true to enter, false to leave. */
void          rhi_compile_pool_low(rhi_device *dev, bool on);

/* What one pipeline batch did (rhi_pipeline_batch_end). */
typedef struct {
    unsigned queued;      /* creates queued on the pool                         */
    unsigned installed;   /* results installed into their slots                 */
    unsigned failed;      /* compiles that failed: the slot took its failed value */
    unsigned duplicate;   /* results discarded: their slot was filled first     */
    double   ms;          /* wall time, batch begin to end                      */
} rhi_pipeline_batch_stats;

/** @brief Open a pipeline batch: until rhi_pipeline_batch_end, rhi_create_pipeline_slot queues its create on the compile pool.
 *
 * Display owner only. No slot a batch fills may be read until the batch
 * ends.
 * @param dev Device. */
void          rhi_pipeline_batch_begin(rhi_device *dev);

/** @brief Close a pipeline batch: wait for its creates and install each result into its slot.
 *
 * Display owner only.
 * @param dev Device.
 * @param pump Called between waits (event pumping), or NULL.
 * @param ctx Passed to @p pump.
 * @param out Receives the batch's counts, or NULL. */
void          rhi_pipeline_batch_end(rhi_device *dev, void (*pump)(void *ctx), void *ctx,
                                     rhi_pipeline_batch_stats *out);

/** @brief Create a pipeline into a slot: now, or at the end of the open pipeline batch.
 *
 * Display owner only. Outside a batch the pipeline is created for the
 * current variant (rhi_create_pipeline_spv) and @p slot set to it, or to
 * @p failed when the create fails. Inside a batch the create is queued and
 * @p slot is left unchanged; rhi_pipeline_batch_end sets it to the
 * installed pipeline or to @p failed, or discards the result when the slot
 * was filled first. In description mode (rhi_pipeline_describe_begin) the
 * description is recorded, nothing is created or queued and @p slot is set
 * to a non-NULL marker that is no pipeline.
 * @param dev Device.
 * @param desc Pipeline description, copied.
 * @param vert_words Vertex SPIR-V words, or NULL to use desc->vert_spv.
 * @param vert_n Word count of @p vert_words.
 * @param frag_words Fragment SPIR-V words, or NULL to use desc->frag_spv.
 * @param frag_n Word count of @p frag_words.
 * @param slot The slot.
 * @param failed The value a failed create leaves in the slot.
 * @return 0 created, 1 queued or described, -1 failed. */
int           rhi_create_pipeline_slot(rhi_device *dev, const rhi_pipeline_desc *desc,
                                       const uint32_t *vert_words, size_t vert_n,
                                       const uint32_t *frag_words, size_t frag_n,
                                       void **slot, void *failed);

/* What one live rebuild did, from its first request to its swap
 * (rhi_rebuild_poll). */
typedef struct {
    rhi_pipe_variant shown;   /* the variant the swap installed                    */
    unsigned rebuilt;         /* pipelines given new objects                        */
    unsigned failed;          /* pipelines whose compile failed: old objects kept   */
    unsigned unchanged;       /* pipelines whose create info the switch left alone  */
    unsigned late;            /* pipelines registered after the request that joined the set */
    unsigned stale;           /* results discarded: superseded, or their member retired */
    unsigned retired;         /* members retired while the rebuild was in flight    */
    unsigned requests;        /* requests the swap answers (a later one supersedes) */
    unsigned presents;        /* frames begun between the first request and the swap */
    double   ms;              /* wall time, first request to swap                   */
    double   owner_ms;        /* display-owner time inside the requests and the swap */
    unsigned hit, miss, unknown; /* cache readings of the rebuild's creates         */
} rhi_rebuild_stats;

/** @brief Request a live variant switch: compile the registered pipelines the switch changes on the compile pool.
 *
 * Display owner only. Bumps the variant epoch. Each registered pipeline
 * whose create info under @p var differs from its installed objects' gets
 * a compile job (rhi_compile_job_of); the pipelines keep drawing their
 * installed objects, and pipelines created meanwhile draw the variant
 * still shown (rhi_pipe_variant_current) and join the set. A request during a rebuild supersedes
 * it: a member whose create info the new request leaves as the previous
 * one requested keeps its job or result, a member it changes gets a new
 * job and the earlier result is discarded when it arrives, and a member
 * the new request returns to its installed create info leaves the set.
 * Each pipeline's create info under @p var leaves a running background
 * walk (rhi_precompile_walk_step), which then never submits it.
 * rhi_rebuild_poll performs the swap.
 * @param dev Device, or NULL.
 * @param var The variant to show.
 * @param sync true waits for every job of the set before returning, so the
 *             next rhi_rebuild_poll swaps (a run whose output must not
 *             depend on worker timing).
 * @return 0, or -1 for a NULL device. */
int           rhi_rebuild_request(rhi_device *dev, rhi_pipe_variant var, bool sync);

/** @brief Install finished compile-pool results and, when a rebuild's set is complete, swap it.
 *
 * Display owner only; called between frames, before any command buffer
 * records. The swap installs every member's new objects at once (the
 * replaced ones retire by frame serial) and then makes the requested
 * variant the one rhi_pipe_variant_current returns. A pipeline registered
 * while the rebuild is in flight joins the set (its create info for the
 * requested variant compiles on the pool) and the swap waits for it; one
 * that could not join compiles on the display owner at the swap. A member whose compile failed keeps its
 * old objects.
 * @param dev Device, or NULL.
 * @param out Receives the rebuild's counts when a swap happened, or NULL.
 * @return 1 when a swap happened in this call, else 0. */
int           rhi_rebuild_poll(rhi_device *dev, rhi_rebuild_stats *out);

/** @brief Link the pipelines linked fast from library parts again with link-time optimization, in the background, and swap each result in.
 *
 * Display owner only; called between frames, before any command buffer
 * records, once per present after the first tick. A no-op unless the
 * library path and its optimization are on (OPENPETE_PIPELINE_LTO=0
 * declines the optimization). First the results that returned since the
 * last call replace their pipelines' objects; the replaced objects retire
 * by frame serial, so no draw loses its pipeline and no device idle runs.
 * Then, unless a live rebuild is in flight, every registered fast-linked
 * pipeline with no link out gets one, queued on the compile pool's low
 * queue behind the library parts, the first-use and rebuild jobs and the
 * background variant walk's job; a wait for the pool does not wait for
 * them. A pipeline whose desc names a whole-create vertex stage
 * (rhi_pipeline_desc.vert_spv_whole) is created whole from that stage
 * instead of linked. A result whose pipeline was retired, rebuilt or
 * switched variant meanwhile is destroyed. A create of a fast-linked
 * pipeline whose optimized form the key manifest lists makes it optimized
 * at once when the pipeline cache serves it (pipeline_optimized_listed), so
 * a warm start queues none. With the pipe census on (rhi_pc_level) each
 * swap logs one "pipe-swap" line naming the swapped-in vertex stage. When every
 * link has returned and been swapped, one line "pipeline lto: ..." is
 * logged and the cache save is armed.
 * @param dev Device, or NULL.
 * @param sync true waits for the links queued in this call and swaps them
 *             before returning (a run whose output must not depend on
 *             worker timing). */
void          rhi_pipeline_lto_step(rhi_device *dev, bool sync);

/** @brief Return whether a live rebuild is in flight (requested and not yet swapped).
 *
 * @param dev Device, or NULL.
 * @return true while a rebuild is in flight. */
bool          rhi_rebuild_pending(const rhi_device *dev);


/** @brief Create a pipeline from in-memory SPIR-V words (mod shaders).
 *
 * The words are copied into the pipeline, which keeps them for
 * a live rebuild; the caller keeps ownership of its buffers. A stage
 * given NULL words or a zero count falls back to the desc's embedded
 * filename (mod postfx: vert NULL with desc->vert_spv = "postfx_fs.vert.spv",
 * frag from the mod).
 * @param dev Device the pipeline is created on.
 * @param desc Pipeline description, copied.
 * @param vert_words Vertex SPIR-V words, or NULL.
 * @param vert_n Word count of @p vert_words.
 * @param frag_words Fragment SPIR-V words, or NULL.
 * @param frag_n Word count of @p frag_words.
 * @return The pipeline, or NULL on allocation or compile failure. */
rhi_pipeline *rhi_create_pipeline_spv(rhi_device *dev,
                                      const rhi_pipeline_desc *desc,
                                      const uint32_t *vert_words, size_t vert_n,
                                      const uint32_t *frag_words, size_t frag_n);

/* ---- uniform-buffer blocks (postfx/custom-pipeline uniforms) --------
 * A rhi_ubo is `blocks` fixed-size uniform blocks × the frame ring, in
 * one HOST_VISIBLE buffer, with a 2-binding DYNAMIC-offset descriptor
 * set (binding 0 = block_a, binding 1 = block_b at bind time; both
 * visible to vertex+fragment). block_size is rounded up to the device's
 * minUniformBufferOffsetAlignment. Write targets the CURRENT frame slot
 * (same parity contract as rhi_create_vertex_buffer_dynamic: write
 * every frame that binds). A block rewritten after a bind in the same
 * frame moves to a fresh generation (up to four per frame), so the
 * postfx chains at the SCENE and COMPOSITE points, which both fill
 * block 0 and the per-pass blocks, each read their own uniforms.
 * Bind attaches set 3 of a postfx pipeline.
 *
 * rhi_ubo_bind_at attaches ONE block through a single-binding layout at
 * an arbitrary set index instead. Set 3 is the shader-contract set - its
 * binding 0 is the engine block and its binding 1 is the MOD's block, and
 * a descriptor set can only view one buffer per binding, so an engine
 * subsystem with uniforms of its own has to put them somewhere a mod
 * never looks. That is what this is for. */
typedef struct rhi_ubo rhi_ubo;
/** @brief Create a uniform block set: `blocks` blocks per frame slot and generation, with its two descriptor sets.
 *
 * @param dev Device.
 * @param block_size Bytes per block, rounded up to the UBO offset alignment.
 * @param blocks Blocks per frame slot.
 * @return The set, or NULL when the buffer or the two-binding descriptor
 *         set cannot be allocated (logged). */
rhi_ubo *rhi_create_ubo(rhi_device *dev, uint32_t block_size, uint32_t blocks);
/** @brief Write `len` bytes into one block of a uniform block set.
 *
 * Inside a frame the write goes to the block's current generation, moved to
 * the next when a draw already bound the block this frame (RHI_UBO_GENS per
 * frame; past that the last generation is overwritten and counted in
 * rhi_ring_gen_overflows). Outside a frame every frame slot and generation of
 * the block receives the copy.
 * @param ubo Set; NULL is ignored.
 * @param block Block index; at or past the block count is ignored.
 * @param data Source bytes; NULL is ignored.
 * @param len Byte count, clamped to the block size. */
void     rhi_ubo_write(rhi_ubo *ubo, uint32_t block, const void *data,
                       uint32_t len);
/** @brief Bind two blocks of a set as bindings 0 and 1 of descriptor set 3 of the bound pipeline.
 *
 * Marks both blocks bound, so a later same-frame write moves to a new
 * generation. Does nothing outside an active frame, without a bound
 * pipeline, or when either block index is out of range.
 * @param dev Device whose current frame records the bind.
 * @param ubo Set.
 * @param block_a Block at binding 0.
 * @param block_b Block at binding 1. */
void     rhi_ubo_bind(rhi_device *dev, rhi_ubo *ubo, uint32_t block_a,
                      uint32_t block_b);
/** @brief Create a block set like rhi_create_ubo whose blocks are read-only storage buffers, bound only through rhi_ubo_bind_at.
 *
 * For an engine table larger than the uniform range a device guarantees.
 * Blocks are rounded up to the larger of the storage and uniform offset
 * alignments; writes follow rhi_ubo_write's frame and generation contract.
 * The set has no contract-set view, so rhi_ubo_bind is a no-op on it.
 * @param dev Device.
 * @param block_size Bytes per block.
 * @param blocks Blocks per frame slot.
 * @return The set, or NULL when the buffer or descriptor set cannot be
 *         allocated (logged). */
rhi_ubo *rhi_create_ssbo(rhi_device *dev, uint32_t block_size, uint32_t blocks);
/** @brief Bind one block of a set at descriptor set index `set` through the single-binding layout.
 *
 * Marks the block bound. Does nothing outside an active frame, without a
 * bound pipeline, when the set's single-binding descriptor was not allocated,
 * or when the block index is out of range.
 * @param dev Device whose current frame records the bind.
 * @param ubo Set.
 * @param set Descriptor set index in the bound pipeline's layout.
 * @param block Block index. */
void     rhi_ubo_bind_at(rhi_device *dev, rhi_ubo *ubo, uint32_t set,
                         uint32_t block);
/** @brief Free a uniform block set's descriptor sets and buffer; NULL is ignored.
 *
 * @param dev Device the set was created on.
 * @param ubo Set to destroy. */
void     rhi_destroy_ubo(rhi_device *dev, rhi_ubo *ubo);

/* ---- read-only table data at set 3 binding 2 ------------------------
 * The contract set's layout carries a third binding: a fragment-visible
 * read-only storage buffer. A material fragment whose large constant
 * tables the mod SDK moved out of its SPIR-V (mod_spv_cdata.h) reads them
 * there. A rhi_ubo's own descriptor set points binding 2 at a 16-byte
 * zero buffer, so a shader that declares nothing there binds as before. A
 * rhi_ubo_data is one device-resident copy of a table block plus a
 * descriptor set of the contract layout whose bindings 0 and 1 view the
 * rhi_ubo it was made for and whose binding 2 views the copy. */
typedef struct rhi_ubo_data rhi_ubo_data;
/** @brief Upload a read-only table block and make a contract-set descriptor set over a uniform block set and it.
 *
 * The buffer prefers DEVICE_LOCAL host-visible memory and falls back to
 * host-visible memory; the bytes are copied once and never change.
 * @param dev Device.
 * @param ubo The uniform block set whose blocks bindings 0 and 1 view; it
 *        must outlive the result.
 * @param bytes The table block; at least 4 bytes.
 * @param len Its byte count.
 * @return The data, or NULL when the buffer or the set cannot be allocated
 *         (logged). */
rhi_ubo_data *rhi_ubo_data_create(rhi_device *dev, rhi_ubo *ubo,
                                  const void *bytes, size_t len);
/** @brief Bind two blocks of a set as bindings 0 and 1 of descriptor set 3, with a table block at binding 2.
 *
 * rhi_ubo_bind with @p data's descriptor set in place of the set's own;
 * NULL @p data is rhi_ubo_bind.
 * @param dev Device whose current frame records the bind.
 * @param ubo Set; the one @p data was created for.
 * @param block_a Block at binding 0.
 * @param block_b Block at binding 1.
 * @param data Table block at binding 2, or NULL for the zero buffer. */
void     rhi_ubo_bind_data(rhi_device *dev, rhi_ubo *ubo, uint32_t block_a,
                           uint32_t block_b, const rhi_ubo_data *data);
/** @brief Free a table block's descriptor set and buffer; NULL is ignored.
 *
 * The caller guarantees no submitted frame still reads it.
 * @param dev Device the data was created on.
 * @param data Data to destroy. */
void     rhi_ubo_data_destroy(rhi_device *dev, rhi_ubo_data *data);

/* ---- retained read-only storage for the vertex stage ----------------
 * A rhi_storage is a fixed-size read-only storage buffer whose contents
 * persist across frames: a frame that binds it without writing reads the
 * last committed contents, so data produced once per tick is uploaded once
 * per tick however many presents draw it. It binds through the single
 * dynamic storage binding of rhi_create_ssbo's layout (ssbo_set2 or
 * ssbo_set7 in the pipeline description), visible to the vertex and
 * fragment stages. The buffer holds RHI_STORAGE_VERSIONS copies of the
 * contents. rhi_storage_map hands out a copy that is not the committed one
 * and that no begun frame whose fence is unwaited bound (bound in a frame
 * serial S with S + RHI_FRAMES_IN_FLIGHT > rhi_frame_serial());
 * rhi_storage_commit publishes it. A write for one present therefore never
 * touches the bytes a recorded or executing draw of an earlier present, or
 * an earlier draw of the same frame, reads.
 *
 * Shaders declare the block readonly: a vertex-stage storage write needs
 * vertexPipelineStoresAndAtomics, which the device does not enable. Reading
 * needs no feature; the binding is core Vulkan 1.0. */
#define RHI_STORAGE_VERSIONS (RHI_FRAMES_IN_FLIGHT + 2)
typedef struct rhi_storage rhi_storage;
/** @brief Create a retained read-only storage buffer of `bytes` bytes, zero-filled, with its descriptor set.
 *
 * The buffer prefers DEVICE_LOCAL host-visible memory and falls back to
 * host-visible memory; each version is `bytes` rounded up to the storage
 * offset alignment.
 * @param dev Device.
 * @param bytes Size of the contents the shaders see; at least 4.
 * @return The buffer, or NULL when the buffer or the descriptor set cannot
 *         be allocated (logged). */
rhi_storage *rhi_storage_create(rhi_device *dev, size_t bytes);
/** @brief Return a writable copy of the contents for the caller to fill before rhi_storage_commit.
 *
 * The copy is not the committed one and no unretired frame bound it, so
 * nothing the GPU reads changes. Its bytes are those of an older commit:
 * the caller writes every byte the shaders read. A second call before the
 * commit returns the same copy. When every other copy is bound by an
 * unretired frame (more than two commits followed by binds in one frame)
 * the copy least recently bound is overwritten, counted in
 * rhi_ring_gen_overflows and logged once.
 * @param s Buffer; NULL returns NULL.
 * @return The copy's mapped bytes, rhi_storage_create's `bytes` long. */
void        *rhi_storage_map(rhi_storage *s);
/** @brief Publish the copy rhi_storage_map returned, so binds from now on read it.
 *
 * Callable inside or outside a frame. Does nothing without a pending map.
 * @param s Buffer; NULL is ignored. */
void         rhi_storage_commit(rhi_storage *s);
/** @brief Bind the committed contents at descriptor set index `set` of the bound pipeline.
 *
 * Records the frame serial on the bound copy. Before the first commit the
 * zero-filled copy binds. Does nothing outside an active frame or without
 * a bound pipeline.
 * @param dev Device whose current frame records the bind.
 * @param s Buffer.
 * @param set Descriptor set index; the pipeline's layout holds the storage
 *            binding there (ssbo_set2 or ssbo_set7). */
void         rhi_storage_bind(rhi_device *dev, rhi_storage *s, uint32_t set);
/** @brief Free a retained storage buffer's descriptor set and buffer; NULL is ignored.
 *
 * The caller guarantees no unretired frame bound it.
 * @param dev Device the buffer was created on.
 * @param s Buffer to destroy. */
void         rhi_storage_destroy(rhi_device *dev, rhi_storage *s);

/* ---- compute: dispatches over storage buffers, consumed in the same frame
 * A compute pass runs inside a frame's command buffer, outside any render
 * pass, on the graphics queue: rhi_compute_begin .. rhi_compute_end
 * brackets rhi_compute_bind, rhi_compute_push, rhi_compute_dispatch and
 * rhi_compute_barrier. Inputs are rhi_storage buffers the host commits
 * (their writes reach the GPU at the frame's submit; no barrier is owed);
 * outputs are rhi_gpu_buffer, device-local memory only the GPU writes.
 * Synchronisation is stated by intent and recorded by the RHI:
 *   - rhi_compute_begin orders the pass after every earlier read and write
 *     of a buffer by any stage (draws, indirect reads, copies, dispatches),
 *     this frame's and every earlier frame's, so a dispatch rewriting a
 *     rhi_gpu_buffer each frame never overwrites bytes an unexecuted draw
 *     of an earlier frame reads, and one copy of the buffer suffices;
 *   - rhi_compute_barrier orders the dispatches after it after the writes
 *     of the dispatches before it (a pass whose second dispatch reads the
 *     first's output);
 *   - rhi_compute_end makes the pass's writes visible to everything after
 *     it in the frame: vertex and fragment storage reads
 *     (rhi_gpu_buffer_bind), indirect-argument reads, and copies
 *     (rhi_readback_cycle).
 * Results reach the host through rhi_readback, a ring of
 * RHI_FRAMES_IN_FLIGHT host-visible slots retrieved the frame the slot is
 * reused, so no call waits on the GPU. GPU time is measured by bracketing
 * dispatches with rhi_gpu_zone_begin and rhi_gpu_zone_end inside the pass.
 *
 * Availability: compute needs no device feature or extension; the device is
 * created identically with or without it. rhi_compute_supported is false
 * when the graphics queue the device uses lacks the compute bit, when a
 * limit is below its floor (RHI_COMPUTE_MIN_*, RHI_COMPUTE_MAX_BINDINGS;
 * each at or under the Vulkan 1.0 required limit, so a conformant device
 * meets every one), or when OPENPETE_RHI_COMPUTE starts with 0 (the
 * fallback's test knob, read at device creation). Then
 * rhi_create_compute_pipeline returns NULL and rhi_compute_begin refuses,
 * so a caller keeps its CPU path by testing the pipeline or
 * rhi_compute_supported once; GPU buffers and readback rings stay usable.
 *
 * Threading: every entry point is the display owner's (rhi_display_claim),
 * as the frame's command buffer is; rhi_compute_begin verifies the calling
 * thread. Lifetime: the caller destroys a pipeline, set, buffer or readback
 * after no unretired frame (one begun fewer than RHI_FRAMES_IN_FLIGHT
 * frames ago) recorded a use of it, and before rhi_destroy. */
#define RHI_COMPUTE_MAX_BINDINGS 4          /* storage bindings per pipeline, all at set 0 */
#define RHI_COMPUTE_MIN_INVOCATIONS 64u     /* maxComputeWorkGroupInvocations and maxComputeWorkGroupSize[0] */
#define RHI_COMPUTE_MIN_STORAGE_RANGE (1u << 27) /* maxStorageBufferRange */
typedef struct rhi_compute_pipeline rhi_compute_pipeline;
typedef struct rhi_compute_set rhi_compute_set;
typedef struct rhi_gpu_buffer rhi_gpu_buffer;
typedef struct rhi_readback rhi_readback;

/** @brief Return whether the device runs compute passes.
 *
 * Decided once at device creation (see the section comment); false for
 * NULL.
 * @param dev Device, or NULL.
 * @return true when compute pipelines can be created. */
bool rhi_compute_supported(const rhi_device *dev);
/** @brief Return why compute is unavailable, or "" when it is available.
 *
 * @param dev Device, or NULL.
 * @return A static or device-owned string; "no device" for NULL. */
const char *rhi_compute_unsupported_reason(const rhi_device *dev);

/* A compute pipeline: one SPIR-V compute stage whose layout is set 0 =
 * `bindings` dynamic storage-buffer bindings 0..bindings-1 (std430 blocks)
 * and a push-constant block of `push_const_size` bytes. */
typedef struct {
    const char *comp_spv;        /* .spv name: OPENPETE_SHADER_DIR, else embedded */
    uint32_t    bindings;        /* 1..RHI_COMPUTE_MAX_BINDINGS */
    uint32_t    push_const_size; /* bytes, a multiple of 4, at most 128; 0 = none */
} rhi_compute_pipeline_desc;
/** @brief Create a compute pipeline through the device's pipeline cache.
 *
 * Compiled at the call, on the calling thread; compute pipelines take no
 * part in the graphics pipeline registry, rebuilds or the precompile walk.
 * @param dev Device.
 * @param desc Stage name, binding count and push-constant size.
 * @return The pipeline, or NULL when compute is unavailable, @p desc is out
 *         of range, or the stage or the pipeline cannot be created (logged). */
rhi_compute_pipeline *rhi_create_compute_pipeline(rhi_device *dev,
                                                  const rhi_compute_pipeline_desc *desc);
/** @brief Destroy a compute pipeline and its layouts; NULL is ignored.
 *
 * Its sets are destroyed first, by the caller.
 * @param dev Device the pipeline was created on.
 * @param pipe Pipeline to destroy. */
void rhi_destroy_compute_pipeline(rhi_device *dev, rhi_compute_pipeline *pipe);

/** @brief Create a storage buffer in device-local memory, written by compute passes and read by draws, dispatches and copies.
 *
 * Zero-filled at creation (a one-off transfer that waits for the queue).
 * The host neither maps nor writes it. Usable as storage, as the source of
 * indirect-draw arguments, and as a copy source and destination. Its own
 * descriptor set over the single dynamic storage binding of
 * rhi_create_ssbo's layout lets a draw read it (rhi_gpu_buffer_bind).
 * Falls back to any memory type when no device-local type takes it.
 * @param dev Device.
 * @param bytes Size; at least 4.
 * @return The buffer, or NULL on failure (logged). */
rhi_gpu_buffer *rhi_gpu_buffer_create(rhi_device *dev, size_t bytes);
/** @brief Return the size a GPU buffer was created with.
 *
 * @param b Buffer, or NULL.
 * @return Bytes; 0 for NULL. */
size_t rhi_gpu_buffer_size(const rhi_gpu_buffer *b);
/** @brief Bind a GPU buffer at descriptor set index `set` of the bound graphics pipeline, for its vertex or fragment stage.
 *
 * The pipeline's layout holds the storage binding there (ssbo_set2 or
 * ssbo_set7); the block is declared readonly. The data read is what the
 * last rhi_compute_end before the draw made visible. Does nothing outside
 * an active frame or without a bound pipeline.
 * @param dev Device whose current frame records the bind.
 * @param b Buffer.
 * @param set Descriptor set index. */
void rhi_gpu_buffer_bind(rhi_device *dev, rhi_gpu_buffer *b, uint32_t set);
/** @brief Destroy a GPU buffer and its descriptor set; NULL is ignored.
 *
 * @param dev Device the buffer was created on.
 * @param b Buffer to destroy. */
void rhi_gpu_buffer_destroy(rhi_device *dev, rhi_gpu_buffer *b);

/* One storage binding of a compute set: exactly one of the two members. */
typedef struct {
    rhi_storage    *storage; /* host-written input; a dispatch reads the contents committed at its bind */
    rhi_gpu_buffer *gpu;     /* GPU-written buffer, bound whole */
} rhi_compute_binding;
/** @brief Create the descriptor set that feeds a compute pipeline's bindings 0..n-1.
 *
 * Built once over the buffers named; each bind (rhi_compute_bind) selects
 * a rhi_storage's committed copy by dynamic offset, as rhi_storage_bind
 * does, so the set is never rewritten.
 * @param dev Device.
 * @param pipe Pipeline whose layout the set follows.
 * @param b Bindings, one per pipeline binding.
 * @param n Their count; must equal the pipeline's binding count.
 * @return The set, or NULL on a mismatch or allocation failure (logged). */
rhi_compute_set *rhi_compute_set_create(rhi_device *dev, const rhi_compute_pipeline *pipe,
                                        const rhi_compute_binding *b, uint32_t n);
/** @brief Free a compute set; NULL is ignored.
 *
 * @param dev Device the set was created on.
 * @param set Set to free. */
void rhi_compute_set_destroy(rhi_device *dev, rhi_compute_set *set);

/** @brief Open a compute pass in the current frame, ordered after every earlier use of every buffer.
 *
 * Records one global barrier (see the section comment). Refused when
 * compute is unavailable, outside an active frame, inside a render pass
 * (between a begin and an end of the offscreen pass or a render target),
 * or inside an open compute pass; a refused begin makes the calls up to
 * the next rhi_compute_end do nothing.
 * @param dev Device whose current frame records the pass.
 * @return true when the pass is open. */
bool rhi_compute_begin(rhi_device *dev);
/** @brief Bind a compute pipeline and its set for the dispatches that follow.
 *
 * Each rhi_storage binding reads the copy committed at this call; the bind
 * stamps it with the frame serial, so rhi_storage_map does not hand that
 * copy out until the frame retires. Does nothing outside an open pass.
 * @param dev Device.
 * @param pipe Pipeline.
 * @param set Set created for @p pipe. */
void rhi_compute_bind(rhi_device *dev, rhi_compute_pipeline *pipe, rhi_compute_set *set);
/** @brief Write the bound compute pipeline's push constants.
 *
 * Does nothing outside an open pass, without a bound pipeline, or when
 * @p size exceeds the pipeline's push-constant size.
 * @param dev Device.
 * @param data Bytes to write.
 * @param size Byte count, a multiple of 4. */
void rhi_compute_push(rhi_device *dev, const void *data, uint32_t size);
/** @brief Record a dispatch of gx x gy x gz workgroups of the bound compute pipeline.
 *
 * Does nothing outside an open pass, without a bound pipeline and set, or
 * with a zero count.
 * @param dev Device.
 * @param gx Workgroups in x.
 * @param gy Workgroups in y.
 * @param gz Workgroups in z. */
void rhi_compute_dispatch(rhi_device *dev, uint32_t gx, uint32_t gy, uint32_t gz);
/** @brief Order the dispatches that follow after the storage writes of the dispatches before.
 *
 * Does nothing outside an open pass.
 * @param dev Device. */
void rhi_compute_barrier(rhi_device *dev);
/** @brief Close the compute pass, making its writes visible to every later draw, indirect read, dispatch and copy of the frame.
 *
 * Does nothing without an open pass.
 * @param dev Device. */
void rhi_compute_end(rhi_device *dev);
/** @brief Return the number of dispatches recorded since the last frame began.
 *
 * The engagement count of a compute path.
 * @param dev Device, or NULL.
 * @return The count; 0 for NULL. */
uint32_t rhi_compute_dispatches(const rhi_device *dev);

/** @brief Create a readback ring: RHI_FRAMES_IN_FLIGHT host-visible slots of `bytes` bytes each.
 *
 * Prefers HOST_CACHED memory (fast CPU reads), always HOST_COHERENT.
 * @param dev Device.
 * @param bytes Capacity of one capture.
 * @return The ring, or NULL on failure (logged). */
rhi_readback *rhi_readback_create(rhi_device *dev, size_t bytes);
/** @brief Take the capture the current frame slot holds, then queue this frame's copy of a GPU buffer range into the slot.
 *
 * The slot's capture was queued RHI_FRAMES_IN_FLIGHT frames ago and
 * rhi_begin_frame waited its fence, so the call never waits on the GPU. A
 * capture is copied to @p out when it fits @p out_cap, then the slot is
 * emptied either way. The copy reads what the last rhi_compute_end before
 * it made visible. Call outside a render pass and outside an open compute
 * pass; does nothing outside an active frame.
 * @param dev Device whose current frame records the copy.
 * @param rb Ring.
 * @param src Buffer to copy from, or NULL to only take the old capture.
 * @param offset Byte offset into @p src.
 * @param bytes Byte count; at most the ring's capacity and within @p src.
 * @param out Destination of the old capture, or NULL to drop it.
 * @param out_cap Capacity of @p out.
 * @param out_serial Receives the frame serial the old capture was queued
 *                   in; may be NULL.
 * @return Bytes of the old capture written to @p out; 0 when the slot held
 *         none or it did not fit. */
size_t rhi_readback_cycle(rhi_device *dev, rhi_readback *rb, rhi_gpu_buffer *src,
                          size_t offset, size_t bytes, void *out, size_t out_cap,
                          uint64_t *out_serial);
/** @brief Take the oldest capture still held by the ring, after rhi_wait_idle at the end of a run.
 *
 * For the last RHI_FRAMES_IN_FLIGHT captures, which no later frame
 * retrieves. Refused inside an active frame. Call repeatedly until it
 * returns 0.
 * @param dev Device, idle.
 * @param rb Ring.
 * @param out Destination.
 * @param out_cap Capacity of @p out.
 * @param out_serial Receives the frame serial of the capture; may be NULL.
 * @return Bytes written; 0 when the ring is empty or a frame is active. A
 *         capture that does not fit @p out_cap is dropped and the next one
 *         taken. */
size_t rhi_readback_drain(rhi_device *dev, rhi_readback *rb, void *out, size_t out_cap,
                          uint64_t *out_serial);
/** @brief Free a readback ring; NULL is ignored.
 *
 * @param dev Device the ring was created on.
 * @param rb Ring to free. */
void rhi_readback_destroy(rhi_device *dev, rhi_readback *rb);

/* ---- offscreen-as-input (the COMPOSITE postfx point) ----------------
 * rhi_bind_offscreen_tex: bind off_image as the sampler at `set` (inside a
 * pass, after the pipeline bind). resume_attachment: the return barrier
 * before re-beginning the offscreen pass (fragment reads -> color writes) for
 * a chain's final pass, whose off_rp end restores TRANSFER_SRC for the
 * present blit. */
/** @brief Transition off_image from TRANSFER_SRC to SHADER_READ_ONLY so a postfx pass can sample the composed frame.
 *
 * Call between render passes. The offscreen pass's external dependency
 * covers transfer reads only, so the barrier carries its own color-write to
 * fragment-read dependency. Does nothing outside an active frame or for NULL.
 * @param dev Device whose current frame records the barrier. */
void rhi_offscreen_to_sampled(rhi_device *dev);
/** @brief Bind the offscreen color image, linear-sampled, at `set` of the bound pipeline.
 *
 * Each frame slot keeps its own descriptor set, allocated on first use and
 * rewritten when the offscreen image was rebuilt. Does nothing outside an
 * active frame or without a bound pipeline.
 * @param dev Device whose current frame records the bind.
 * @param set Descriptor set index. */
void rhi_bind_offscreen_tex(rhi_device *dev, uint32_t set);
/** @brief Record the barrier that returns off_image from sampled to color-attachment use.
 *
 * Orders the fragment reads of a postfx pass before the re-begun offscreen
 * pass writes the image; that pass clears on load, so the layout value only
 * names the source scope. Does nothing outside an active frame or for NULL.
 * @param dev Device whose current frame records the barrier. */
void rhi_offscreen_resume_attachment(rhi_device *dev);
/** @brief Transition off_image from SHADER_READ_ONLY back to TRANSFER_SRC without re-rendering it.
 *
 * Restores the layout the present blit reads after a postfx chain that ran no
 * pass. Does nothing outside an active frame or for NULL.
 * @param dev Device whose current frame records the barrier. */
void rhi_offscreen_resume_blit_src(rhi_device *dev);
/** @brief Bind a texture's or render target's sampler set at descriptor set index `set`.
 *
 * rhi_bind_vram and rhi_bind_vram2 are the fixed set-0 and set-1 forms of
 * this bind. Does nothing when either pointer is NULL, outside an active
 * frame, or before a pipeline has been bound.
 * @param dev Device whose current frame records the bind.
 * @param tex Texture whose descriptor set is bound.
 * @param set Descriptor set index in the bound pipeline's layout. */
void rhi_bind_texture_set(rhi_device *dev, rhi_texture *tex, uint32_t set);
/** @brief Blit a postfx chain's final render target into off_image, leaving off_image TRANSFER_SRC for the present blit.
 *
 * Expects off_image in SHADER_READ (rhi_offscreen_to_sampled ran) and `src`
 * in SHADER_READ after its render pass. The blit scales the whole of `src`
 * to the swapchain extent with a nearest filter; `src` is restored to
 * SHADER_READ. Call outside a render pass. Does nothing outside an active
 * frame, for NULL, or when `src` is not a render target.
 * @param dev Device whose current frame records the blit.
 * @param src Render target holding the chain's output. */
void rhi_blit_rt_to_offscreen(rhi_device *dev, rhi_texture *src);
/** @brief Blit a render target into another render target, scaling the whole of `src` to `dst` with a nearest filter.
 *
 * The postfx write-back for a render-target compose path: `dst` is left
 * SHADER_READ for readback and `src` is restored to SHADER_READ. Call
 * outside a render pass. Does nothing outside an active frame, for NULL, or
 * when either texture is not a render target.
 * @param dev Device whose current frame records the blit.
 * @param dst Destination render target.
 * @param src Source render target, in SHADER_READ. */
void rhi_blit_rt_to_rt(rhi_device *dev, rhi_texture *dst, rhi_texture *src);

/* ---- the native-res PSX VRAM model -------------------------------- */

/** @brief Create the R16_UINT image that mirrors PSX VRAM one uint per pixel.
 *
 * Texture pages and CLUTs are sampled from it with texelFetch through the
 * ported PSX 4/8/16bpp and CLUT decoder (shaders/psx_sample.frag), which does
 * its own integer indexing, so no GPU filtering or normalization applies.
 * 1024x512 is the PSX VRAM extent; the renderer keeps a CPU mirror and
 * uploads from it. The image is device-local and sampleable on return.
 * @param dev Device the image is created on.
 * @param width Width in texels.
 * @param height Height in texels.
 * @return The texture, or NULL on failure. */
rhi_texture *rhi_create_vram(rhi_device *dev, uint32_t width, uint32_t height);
/** @brief Upload the whole CPU mirror (width x height uint16 PSX pixels) to a VRAM texture.
 *
 * Upload semantics, shared with rhi_update_rgba8: while a frame is recording
 * (after rhi_begin_frame) the copy is appended to that frame's command buffer
 * through the device upload ring (stage_acquire), with no GPU stall; the
 * call then precedes the frame's first render pass. Outside a frame it uses
 * a blocking one-shot submit through a temporary staging buffer. The texture
 * ends in SHADER_READ_ONLY.
 * @param dev Device.
 * @param tex VRAM texture; NULL or a render target is ignored.
 * @param pixels Source mirror; NULL logs and uploads nothing. */
void rhi_update_vram(rhi_device *dev, rhi_texture *tex, const uint16_t *pixels);

/* Sub-rect upload (dirty-rect path): copy only `rects` regions of the
 * CPU mirror (`mirror` is the FULL width-stride buffer) into the texture, in
 * one barrier pair + one N-region transfer. Same in-frame/one-shot semantics
 * as rhi_update_vram. Rects must be pre-clamped to the texture extent. If the
 * summed rect payload exceeds one full-image staging slot (heavily
 * overlapping rects), it silently falls back to a full upload. */
typedef struct { int x, y, w, h; } rhi_rect;
/** @brief Upload rects to a VRAM texture from a packed copy of their texels instead of a whole-VRAM mirror.
 *
 * The packed layout is each rect's rows contiguous, rects in order at
 * 4-byte-aligned offsets. It is the source for a scene composed against the
 * VRAM it was extracted against, not the newest. Upload semantics are
 * rhi_update_vram's. With no mirror to fall back to a whole-image upload,
 * the rects are split into batches of at most RHI_VRAM_MAX_RECTS whose
 * packed bytes fit the image-sized staging slot; a single rect larger than
 * the image is dropped.
 * @param dev Device.
 * @param tex VRAM texture, or NULL (ignored).
 * @param packed Packed texel source.
 * @param packed_bytes Size of @p packed; unused.
 * @param rects Rects, pre-clamped to the texture extent.
 * @param nrects Rect count. */
void rhi_update_vram_rects_packed(rhi_device *dev, rhi_texture *tex,
                                  const uint8_t *packed, size_t packed_bytes,
                                  const rhi_rect *rects, int nrects);
/** @brief Upload rects of a VRAM texture from a full-width CPU mirror in one N-region copy.
 *
 * Rects with a non-positive size are skipped. Past RHI_VRAM_MAX_RECTS rects,
 * or when the packed payload exceeds one image-sized staging slot, the whole
 * mirror is uploaded instead. Upload semantics are rhi_update_vram's.
 * @param dev Device.
 * @param tex VRAM texture; NULL is ignored.
 * @param mirror Source with a row stride of the texture width.
 * @param rects Rects, pre-clamped to the texture extent.
 * @param nrects Rect count; 0 or less does nothing. */
void rhi_update_vram_rects(rhi_device *dev, rhi_texture *tex,
                           const uint16_t *mirror,
                           const rhi_rect *rects, int nrects);

/** @brief Create an R8G8B8A8_UNORM sampled image (bytes R,G,B,A), such as another renderer's frame for A/B display.
 *
 * Shares the VRAM texture's descriptor layout, so it binds with
 * rhi_bind_vram and is drawn by a textured pipeline that samples a normal
 * sampler2D (shaders/blit.frag). rhi_update_rgba8 fills it from
 * width*height*4 bytes.
 * @param dev Device the image is created on.
 * @param width Width in texels.
 * @param height Height in texels.
 * @return The texture, or NULL on failure. */
rhi_texture *rhi_create_rgba8(rhi_device *dev, uint32_t width, uint32_t height);
/** @brief Create a texelFetch-only R32G32B32A32_UINT image, sampled through a usampler2D.
 *
 * Bound with rhi_bind_texture_set or a pair bind and written through
 * rhi_update_vram_rects_packed (16 bytes per texel).
 * @param dev Device the image is created on.
 * @param width Width in texels.
 * @param height Height in texels.
 * @return The texture, or NULL on failure. */
rhi_texture *rhi_create_u32x4(rhi_device *dev, uint32_t width, uint32_t height);
/** @brief Upload a whole image of pixel bytes to an uploadable texture such as an rhi_create_rgba8 image.
 *
 * Upload semantics are rhi_update_vram's.
 * @param dev Device.
 * @param tex Texture; NULL or a render target is ignored.
 * @param pixels width * height * 4 bytes; NULL logs and uploads nothing. */
void rhi_update_rgba8(rhi_device *dev, rhi_texture *tex, const void *pixels);

/* Sub-rect uploads from TIGHT per-rect source buffers (pixels = w*h*4 RGBA8
 * bytes each) - the HD-atlas append path. Same in-frame/one-shot semantics
 * as rhi_update_rgba8; rects that overflow the staging slot are dropped
 * (re-queue them next frame). At most 96 rects per call. */
typedef struct { const void *pixels; int x, y, w, h; } rhi_tex_rect_src;
/** @brief Upload tight RGBA8 sub-rects into a texture in one barrier pair.
 *
 * Rects are taken in order until the next would overflow an image-sized
 * payload or 96 rects; the rest are not uploaded. In-frame and one-shot
 * behaviour as rhi_update_rgba8.
 * @param dev Device.
 * @param tex Sampled texture; NULL or a render target is ignored.
 * @param rects Rects with their pixels.
 * @param nrects Rect count. */
void rhi_update_rgba8_rects(rhi_device *dev, rhi_texture *tex,
                            const rhi_tex_rect_src *rects, int nrects);

/** @brief Switch a sampled texture's descriptor to the linear-filter sampler.
 *
 * The default sampler is NEAREST (pixel-exact VRAM and blit semantics); the
 * linear one serves high-resolution replacement art whose minification and
 * magnification should filter. The descriptor set is rewritten in place, so
 * the call is made at load time, before the texture is first sampled in a
 * frame.
 * @param dev Device holding the linear sampler.
 * @param tex Texture whose set binding 0 is rewritten. */
void rhi_texture_set_filter_linear(rhi_device *dev, rhi_texture *tex);

/* ---- render-to-texture (the diff pass; later: framebuffer effects) ----- */

/** @brief Create a color image that is both a color attachment and a sampler2D, with its own depth buffer and framebuffer.
 *
 * Geometry is rendered into it between rhi_begin_render_target and
 * rhi_end_render_target, and a later pass in the same frame samples the
 * result. The color format is the fixed internal format (8-bit B,G,R,A),
 * the offscreen pass's, so pipelines built for the offscreen pass are compatible; the image is
 * also a transfer source and destination. Binds as a normal texture with
 * rhi_bind_vram. Sized in pixels; recreate (destroy and create) on a size
 * change.
 * @param dev Device the target is created on.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @return The render target, or NULL on failure. */
rhi_texture *rhi_create_render_target(rhi_device *dev, uint32_t width, uint32_t height);
/** @brief Create a render target of a given colour format, drawn into with rhi_begin_render_target by pipelines whose color_format is that format.
 *
 * RHI_COLOR_FORMAT_SCENE makes the target rhi_create_render_target makes.
 * RHI_COLOR_FORMAT_RGBA32_UINT makes a target of four 32-bit unsigned words
 * per texel, with its own render pass (same depth attachment, colour
 * stored and ending SHADER_READ_ONLY); it has no sampler set and is read
 * with rhi_read_render_target_texels.
 * @param dev Device the target is created on.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @param fmt Colour format.
 * @return The render target, or NULL on failure. */
rhi_texture *rhi_create_render_target_format(rhi_device *dev, uint32_t width, uint32_t height,
                                             rhi_color_format fmt);

/** @brief Begin a render pass into render target `rt`, clearing color to (r, g, b, a) and depth to far.
 *
 * Far is the clear value of the target's depth class. The pass uses the
 * target's own render pass (rt_rp when none is set), and sets a full-target
 * Y-flipped viewport and scissor and a zero depth bias. Draws are recorded
 * until rhi_end_render_target, which closes the pass and leaves the color
 * image in SHADER_READ_ONLY, ready to sample. Must be called inside a frame
 * (after rhi_begin_frame) but outside the offscreen pass, since it is a
 * render pass of its own; does nothing outside a frame or for a texture that
 * is not a render target.
 * @param dev Device whose current frame records the pass.
 * @param rt Render target to draw into.
 * @param r Clear red.
 * @param g Clear green.
 * @param b Clear blue.
 * @param a Clear alpha. */
void rhi_begin_render_target(rhi_device *dev, rhi_texture *rt,
                             float r, float g, float b, float a);
/** @brief End the pass begun by rhi_begin_render_target and mark the color image shader-readable.
 *
 * The target's render pass finishes with the color image in
 * SHADER_READ_ONLY_OPTIMAL, and the texture's tracked layout is updated to
 * match. Does nothing outside an active frame or for a texture that is not a
 * render target.
 * @param dev Device whose current frame records the end.
 * @param rt Render target whose pass ends. */
void rhi_end_render_target(rhi_device *dev, rhi_texture *rt);

/** @brief Create a render target whose depth attachment is also sampleable.
 *
 * The target renders through a render pass whose depth is stored and ends
 * SHADER_READ_ONLY, so a later pass in the same frame can sample the
 * per-pixel depth (a world-opaque depth pre-pass for shadows, decals and
 * soft particles: per-pixel occlusion with no global depth bias). The depth
 * is bound with rhi_bind_depth_sampler after the pre-pass has ended.
 * @param dev Device the target is created on.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @return The render target, or NULL on failure or when the device cannot
 *         sample the depth format (not spec-required for the combined
 *         depth+stencil formats; callers keep a fallback). */
rhi_texture *rhi_create_render_target_depth_sampled(rhi_device *dev,
                                                    uint32_t width, uint32_t height);
/** @brief Bind a depth-sampled render target's depth aspect as a combined image sampler at descriptor `set`.
 *
 * The consumer fragment shader samples it as sampler2D with texel .r =
 * depth. Only valid after rhi_end_render_target on that target this frame
 * (the pass's finalLayout and external dependency make the depth
 * sample-visible). Call after rhi_bind_pipeline, which sets the layout the
 * bind uses. Does nothing outside a frame or for a target without a depth set.
 * @param dev Device whose current frame records the bind.
 * @param rt Depth-sampled render target.
 * @param set Descriptor set index in the bound pipeline's layout. */
void rhi_bind_depth_sampler(rhi_device *dev, rhi_texture *rt, uint32_t set);
/** @brief Bind an image and a storage buffer range together at the set a tex_ssbo_set2 pipeline holds them.
 *
 * Binding 0 samples @p tex (its depth view for a depth-sampled render
 * target, else its colour view) with the VRAM sampler; binding 1 is
 * @p range bytes of @p store starting at @p offset, which must be a
 * multiple of the device's storage offset alignment. The descriptor set is
 * made at the first bind of a (store, range) pair on the texture, kept on
 * the texture and freed with it. A texture holds RHI_TEX_STORE_SETS such
 * sets keyed by the store's identity (a uid per buffer made, never its
 * address); a new pair takes an empty set or one no unretired frame bound.
 * With every set in flight the bind is refused and nothing is bound, so
 * the caller must not draw anything that reads set @p set. Does nothing
 * outside an active frame or without a bound pipeline.
 * @param dev Device.
 * @param tex Image for binding 0.
 * @param store Storage buffer for binding 1 (rhi_create_storage_buffer_device).
 * @param range Bytes binding 1 spans.
 * @param set Descriptor set index (2 for tex_ssbo_set2).
 * @param offset Dynamic offset of binding 1, in bytes.
 * @return 0 when bound (or nothing to do outside a frame), -1 when refused. */
int rhi_bind_texture_storage(rhi_device *dev, rhi_texture *tex, rhi_buffer *store,
                             uint32_t range, uint32_t set, uint32_t offset);
/** @brief Return the device's minimum storage-buffer offset alignment in bytes.
 * @param dev Device.
 * @return The alignment, at least 1. */
uint32_t rhi_storage_offset_align(const rhi_device *dev);
/** @brief Queue this frame's copy of a small render target's color image and return the one the frame slot queued last.
 *
 * Ring-buffered like rhi_cycle_readback, for the occlusion grid: call once
 * per frame after the pass that renders `rt`. The slot's previous capture,
 * queued RHI_FRAMES_IN_FLIGHT frames ago and retired by rhi_begin_frame's
 * fence wait, is written to `out` in R,G,B,A byte order with no GPU stall; a
 * capture that does not fit `out_bytes` is dropped. The copy moves `rt` from
 * SHADER_READ_ONLY to TRANSFER_SRC and back within the frame. One ring per
 * device, so one client.
 * @param dev Device whose current frame records the copy.
 * @param rt Render target to capture, in SHADER_READ_ONLY.
 * @param out Destination, w*h*4 bytes.
 * @param out_bytes Capacity of @p out.
 * @param out_w Receives the capture's width; may be NULL.
 * @param out_h Receives the capture's height; may be NULL.
 * @return true when a capture was written to @p out; false until the ring
 *         fills, outside an active frame, or when @p rt is not a render
 *         target. */
bool rhi_cycle_rt_readback(rhi_device *dev, rhi_texture *rt,
                           void *out, size_t out_bytes, int *out_w, int *out_h);
/** @brief Override the current pass's viewport and scissor to an origin-0 (w, h) Y-flipped rect.
 *
 * A depth pre-pass into an oversized depth target rasterizes only the
 * (w, h) sub-rect its consumers map their lookups to. Recorded inside a
 * render pass, after its begin; does nothing outside an active frame.
 * @param dev Device whose current frame records the state.
 * @param w Width in pixels.
 * @param h Height in pixels. */
void rhi_set_viewport(rhi_device *dev, int w, int h);
/** @brief Write the current swapchain extent, the window's pixel size at the last swapchain build.
 *
 * Equals rhi_offscreen_extent unless rhi_pin_offscreen_extent holds a
 * different extent.
 * @param dev Device, or NULL (writes 0).
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL. */
void rhi_swapchain_extent(const rhi_device *dev, int *w, int *h);
/** @brief Write the extent of the offscreen target, the resolution the offscreen pass renders and reads back at.
 *
 * The pinned extent while rhi_pin_offscreen_extent holds one, else the
 * swapchain extent. Used to size a shared depth pre-pass and the postfx
 * passes over the composed image.
 * @param dev Device, or NULL (writes 0).
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL. */
void rhi_offscreen_extent(const rhi_device *dev, int *w, int *h);
/** @brief Hold the offscreen target and the stage images at a fixed extent across swapchain rebuilds, or release them to follow the swapchain.
 *
 * While pinned, a window resize, minimize or restore rebuilds only the
 * swapchain at the window's size; the offscreen target keeps @p w x @p h,
 * every readback (rhi_cycle_readback, rhi_read_offscreen) returns that
 * extent, and the present blit scales it into the largest centered rect of
 * its aspect ratio with black bars. A change of the effective extent
 * rebuilds the targets at the next frame begin. Ignored on a headless
 * device, whose extent is fixed at creation.
 * @param dev Windowed device, or NULL (ignored).
 * @param w Pinned width; w or h <= 0 releases the pin.
 * @param h Pinned height. */
void rhi_pin_offscreen_extent(rhi_device *dev, int w, int h);
/** @brief Write the render area of the current pass, recorded at every pass begin.
 *
 * The offscreen pass records the swapchain extent, a render-target pass the
 * target's own extent. Every pass begin also sets an origin-0 viewport of
 * exactly this size, so a fragment shader's gl_FragCoord maps to clip space
 * through it alone. Outside a pass the last pass's extent remains; it is
 * zero only before the first pass.
 * @param dev Device, or NULL (writes 0).
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL. */
void rhi_pass_extent(const rhi_device *dev, int *w, int *h);

/** @brief Mark a render target's depth convention (default RHI_DEPTH_SCENE).
 *
 * Its begin-pass depth clear and rhi_clear_depth inside its pass use the
 * class's far value. The offscreen pass is always RHI_DEPTH_SCENE.
 * @param rt Render target; NULL or a non-render-target is ignored.
 * @param cls Depth class. */
void rhi_set_render_target_depth_class(rhi_texture *rt, rhi_depth_class cls);

/** @brief Clear the depth aspect to far over the current pass's render area, within the pass.
 *
 * Far is the clear value of the pass's depth class. vkCmdClearAttachments
 * clears in the active subpass, so no new render pass begins. Used to
 * depth-isolate a late on-top overlay (the 3D HUD icons) so it Z-sorts
 * internally without being occluded by, or occluding, world geometry. Does
 * nothing outside an active frame; must be called inside the offscreen or a
 * render-target pass.
 * @param dev Device whose current pass is cleared. */
void rhi_clear_depth(rhi_device *dev);

/** @brief Clear the stencil aspect to 0 over the current pass's render area, within the pass.
 *
 * Used between per-object stencil-mask groups (one portal's aperture mask
 * and masked draw, then the clear, then the next portal's), so a fixed
 * stencil reference of 1 isolates objects. Does nothing outside an active
 * frame; must be called inside the offscreen or a render-target pass.
 * @param dev Device whose current pass is cleared. */
void rhi_clear_stencil(rhi_device *dev);

/** @brief Read a render target back into a host RGBA8 buffer.
 *
 * Byte order is R,G,B,A with alpha forced opaque. The target must be in
 * SHADER_READ_ONLY (after rhi_end_render_target and the submit of the frame
 * that rendered it); it waits the device idle, copies through a temporary
 * staging buffer, and restores SHADER_READ_ONLY. The screenshot dump uses it
 * to capture the native scene at the reference resolution, without the
 * window letterbox, at any window size.
 * @param dev Device.
 * @param rt Render target, or NULL.
 * @param out Destination; must hold width x height x 4 bytes.
 * @param out_bytes Size of @p out in bytes.
 * @param out_w Receives the target's width, also on a short buffer; may be
 *              NULL.
 * @param out_h Receives the target's height, likewise; may be NULL.
 * @return false when @p rt is not a render target, @p out is NULL or too
 *         small, the extent is empty, or staging cannot be allocated. */
bool rhi_read_render_target(rhi_device *dev, rhi_texture *rt, void *out,
                            size_t out_bytes, int *out_w, int *out_h);
/** @brief Read a render target back into a host RGBA8 buffer with the stored alpha kept.
 *
 * rhi_read_render_target with alpha copied from the image instead of forced
 * to 0xFF, for targets whose alpha channel carries data (the terrain key
 * image's owner class byte).
 * @param dev Device.
 * @param rt Render target, or NULL.
 * @param out Destination; must hold width x height x 4 bytes.
 * @param out_bytes Size of @p out in bytes.
 * @param out_w Receives the target's width; may be NULL.
 * @param out_h Receives the target's height; may be NULL.
 * @return false on the preconditions of rhi_read_render_target. */
bool rhi_read_render_target_raw(rhi_device *dev, rhi_texture *rt, void *out,
                                size_t out_bytes, int *out_w, int *out_h);
/** @brief Read a render target's colour texels back as stored, without swizzle or alpha rewrite.
 *
 * Texel size is the target's format's: 4 bytes in the internal format's
 * own byte order, 16 bytes (four native-endian 32-bit words) for an
 * RHI_COLOR_FORMAT_RGBA32_UINT target. Same preconditions and device wait
 * as rhi_read_render_target.
 * @param dev Device.
 * @param rt Render target, or NULL.
 * @param out Destination; must hold width x height texels.
 * @param out_bytes Size of @p out in bytes.
 * @param out_w Receives the target's width; may be NULL.
 * @param out_h Receives the target's height; may be NULL.
 * @return false on the preconditions of rhi_read_render_target. */
bool rhi_read_render_target_texels(rhi_device *dev, rhi_texture *rt, void *out,
                                   size_t out_bytes, int *out_w, int *out_h);

/** @brief Create a depth-sampled render target whose depth image also allows transfer reads, for rhi_read_render_target_depth.
 *
 * @param dev Device the target is created on.
 * @param width Width in pixels.
 * @param height Height in pixels.
 * @return The render target, or NULL on failure or when the device cannot
 *         sample the depth format. */
rhi_texture *rhi_create_render_target_depth_readable(rhi_device *dev,
                                                     uint32_t width, uint32_t height);
/** @brief Read the depth aspect of a render target made by rhi_create_render_target_depth_readable.
 *
 * One value in [0, 1] per texel, rows in framebuffer order: the float as
 * stored under D32_SFLOAT_S8_UINT, the 24 unorm bits scaled by 1/(2^24 - 1)
 * under D24_UNORM_S8_UINT. Same preconditions as rhi_read_render_target (the
 * pass ended and its frame submitted); waits the device idle and restores
 * SHADER_READ_ONLY on both aspects.
 * @param dev Device.
 * @param rt Render target, or NULL.
 * @param out Destination; must hold width x height floats.
 * @param out_count Capacity of @p out in floats.
 * @param out_w Receives the width, also on a short buffer; may be NULL.
 * @param out_h Receives the height, likewise; may be NULL.
 * @return false for a target that is not depth-readable, a NULL or short
 *         @p out, an empty extent, or a staging allocation failure. */
bool rhi_read_render_target_depth(rhi_device *dev, rhi_texture *rt, float *out,
                                  size_t out_count, int *out_w, int *out_h);

/** @brief Blit a pixel region of the offscreen target into a render target, scaled to fill it with a linear filter.
 *
 * The native freeze backdrop is captured this way, from the windowed render
 * already on screen, instead of from the off-display full-extent render
 * target; the postfx chain also snapshots the previous present with it.
 * Called after rhi_end_offscreen (off_image in TRANSFER_SRC) and before
 * rhi_blit_to_swapchain; dst ends in SHADER_READ_ONLY. Does nothing outside
 * an active frame or for a non-render-target dst.
 * @param dev Device whose current frame records the blit.
 * @param dst Destination render target.
 * @param sx Source region left, offscreen pixels.
 * @param sy Source region top.
 * @param sw Source region width.
 * @param sh Source region height. */
void rhi_snapshot_offscreen_to_rt(rhi_device *dev, rhi_texture *dst,
                                  int sx, int sy, int sw, int sh);

/** @brief Destroy a texture and every Vulkan object it owns, immediately.
 *
 * The caller guarantees no submitted or recording command buffer still
 * references it. NULL is ignored.
 * @param dev Device the texture was created on.
 * @param tex Texture to destroy. */
void rhi_destroy_texture(rhi_device *dev, rhi_texture *tex);

/** @brief Destroy a texture once every frame that may reference it has retired.
 *
 * The texture joins the device's retired list stamped with the current
 * frame serial and is destroyed at the first rhi_begin_frame
 * RHI_FRAMES_IN_FLIGHT serials later (that slot's fence has then been
 * waited), or at rhi_destroy. The caller stops binding it at this call.
 * NULL is ignored.
 * @param dev Device the texture was created on.
 * @param tex Texture to retire. */
void rhi_destroy_texture_deferred(rhi_device *dev, rhi_texture *tex);

/** @brief Write the pixel extent a texture was created with.
 *
 * The RHI does not clamp a requested extent; callers that bound a render
 * target to device limits read the extent back here.
 * @param tex Texture, or NULL (writes 0).
 * @param w Receives the width; may be NULL.
 * @param h Receives the height; may be NULL. */
void rhi_texture_extent(const rhi_texture *tex, int *w, int *h);

/** @brief Bind a graphics pipeline in the current frame's command buffer.
 *
 * Recorded inside a frame, before the draws of the offscreen pass or a
 * render-target pass. Stores the pipeline's layout and push-constant stages,
 * which the later descriptor-set and push-constant binds use. A pipeline
 * created with depth_bias also records the last depth-bias factors set
 * while no such pipeline was bound (rhi_set_depth_bias). Does nothing
 * outside an active frame.
 * @param dev Device whose current frame records the bind.
 * @param pipe Pipeline to bind. */
void rhi_bind_pipeline(rhi_device *dev, rhi_pipeline *pipe);
/** @brief Bind a vertex buffer at binding 0 of the current frame's command buffer.
 *
 * A ring buffer binds at the current frame's slot and generation; a
 * single-instance buffer binds at offset 0. Does nothing outside an active
 * frame.
 * @param dev Device whose current frame records the bind.
 * @param buf Vertex buffer; must not be NULL inside a frame. */
void rhi_bind_vertex_buffer(rhi_device *dev, rhi_buffer *buf);
/** @brief Bind two vertex buffers at bindings 0 and 1 (terrain_anim pipelines: current vertices and their previous twin).
 *
 * A ring buffer binds at its current frame slot and generation, a
 * single-instance buffer at offset 0. Does nothing outside an active frame;
 * neither buffer is checked for NULL.
 * @param dev Device whose current frame records the bind.
 * @param buf Buffer for binding 0.
 * @param prev Buffer for binding 1. */
void rhi_bind_vertex_buffer2(rhi_device *dev, rhi_buffer *buf, rhi_buffer *prev);
/** @brief Bind a texture's descriptor set at set 0 of the current (uses_vram) pipeline.
 *
 * Call after rhi_bind_pipeline, which sets the layout the bind uses, and
 * before the draw. Works for any rhi_texture (the VRAM image, an RGBA8
 * image, a render target): the set layout is the same and only the shader's
 * sampler type differs. Does nothing outside an active frame; `tex` is not
 * checked for NULL.
 * @param dev Device whose current frame records the bind.
 * @param tex Texture whose descriptor set is bound. */
void rhi_bind_vram(rhi_device *dev, rhi_texture *tex);
/** @brief Bind a second texture's descriptor set at set 1 (a uses_vram2 pipeline, such as the diff pass's reference sampler).
 *
 * Call after rhi_bind_pipeline and rhi_bind_vram. Does nothing outside an
 * active frame; `tex` is not checked for NULL.
 * @param dev Device whose current frame records the bind.
 * @param tex Texture whose descriptor set is bound. */
void rhi_bind_vram2(rhi_device *dev, rhi_texture *tex);
/** @brief Bind the sampled depth of `depth_rt` (binding 0) and `tex` (binding 1) as a two-sampler pair set at `set`.
 *
 * This is the pair layout of the actor key image's set (key depth, owner
 * colour) and of the terrain layout's key set (key depth, owner
 * attributes). The frame
 * slot reuses a pair set already written with the same two views, else
 * writes the next set of the slot's list. Both images must be in shader-read
 * layout for the draw. Does nothing outside a frame, when either texture is
 * NULL, or when `depth_rt` has no depth sample view.
 * @param dev Device whose current frame records the bind.
 * @param depth_rt Depth-sampled render target supplying binding 0.
 * @param tex Texture supplying binding 1.
 * @param set Descriptor set index in the bound pipeline's layout. */
void rhi_bind_depth_tex_pair(rhi_device *dev, rhi_texture *depth_rt, rhi_texture *tex,
                             uint32_t set);
/** @brief Bind a quad set at `set`: the sampled depth and colour of `a` (bindings 0 and 1), then of `b` (bindings 2 and 3).
 *
 * The actor key set of the key-resolve and material layouts: the flat actor
 * key image and the per-pixel one, each with its owner colour. The frame
 * slot reuses a quad set already written with the same four views, else
 * writes the next set of the slot's list. Does nothing outside a frame, when
 * either target is NULL, or when either has no depth sample view.
 * @param dev Device whose current frame records the bind.
 * @param a Depth-sampled render target supplying bindings 0 and 1.
 * @param b Depth-sampled render target supplying bindings 2 and 3.
 * @param set Descriptor set index in the bound pipeline's layout. */
void rhi_bind_depth_rt_quad(rhi_device *dev, rhi_texture *a, rhi_texture *b, uint32_t set);
/** @brief Bind the terrain key set at `set`: the sampled depth of `key_rt` (binding 0), `drain` (binding 1) and `key_rt`'s colour image (binding 2).
 *
 * This is the trio layout of the terrain key image's set in the key-resolve
 * and material layouts (key image, drain sub-cell key records, key owner
 * attributes). The frame slot reuses a trio set already written with the
 * same three views, else writes the next set of the slot's list. Every
 * image must be in shader-read layout for the draw. Does nothing outside a
 * frame, when either texture is NULL, or when `key_rt` has no depth sample
 * view.
 * @param dev Device whose current frame records the bind.
 * @param key_rt The terrain key render target (depth-sampled).
 * @param drain The drain sub-cell key records texture.
 * @param set Descriptor set index in the bound pipeline's layout. */
void rhi_bind_terrain_key_set(rhi_device *dev, rhi_texture *key_rt, rhi_texture *drain,
                              uint32_t set);
/** @brief Bind `tex` (binding 0) and the sampled depth of `depth_rt` (binding 1) as a pair set at `set` of a set1_vram_depth pipeline.
 *
 * Both images must be in shader-read layout for the draw.
 * @param dev Device whose current frame records the bind.
 * @param tex Texture supplying binding 0.
 * @param depth_rt Depth-sampled render target supplying binding 1.
 * @param set Descriptor set index in the bound pipeline's layout. */
void rhi_bind_vram_depth_pair(rhi_device *dev, rhi_texture *tex, rhi_texture *depth_rt,
                              uint32_t set);
/** @brief Restrict rasterization to a pixel rect (top-left origin, y down) for subsequent draws this frame.
 *
 * Confines one panel's geometry to its region so off-panel triangles are
 * clipped. The rect is in swapchain pixels; when the bound pass's extent is
 * an integer multiple of the swapchain width (the SSAA scene target) the
 * rect is scaled by that factor. It is then clipped to the pass extent.
 * rhi_reset_scissor restores the full target. Does nothing outside an
 * active frame.
 * @param dev Device whose current frame records the scissor.
 * @param x Left edge.
 * @param y Top edge.
 * @param w Width.
 * @param h Height. */
void rhi_set_scissor(rhi_device *dev, int x, int y, int w, int h);
/** @brief Restore the scissor to the full extent of the bound pass for subsequent draws.
 *
 * Does nothing outside an active frame. The device must not be NULL.
 * @param dev Device whose current frame records the scissor. */
void rhi_reset_scissor(rhi_device *dev);
/** @brief Set the hardware depth-bias factors for subsequent draws of a depth_bias pipeline.
 *
 * Factors are in vkCmdSetDepthBias order (constant, clamp, slope) and apply
 * to draws whose pipeline was created with desc.depth_bias. Negative values
 * pull fragments toward the camera under either scene convention (the RHI
 * negates all three under the reversed one): the decal tie-break. constant×r
 * beats rasterization-rounding ties against coplanar geometry, slope×m grows
 * the pull at grazing angles, clamp bounds the total. r is the minimum
 * resolvable depth delta of the stored value: 2^-24 near z = 1 (forward),
 * about z×2^-23.5 (reversed, z = 1/vz), so a constant pulls 2^-24×vz² view-Z
 * forward and about 2^-23.5×vz reversed. Slope and clamp are in depth units,
 * where |dz/dvz| = 1/vz² in both. The call follows every bind of a
 * depth_bias pipeline, before its draws: binding any non-depth_bias pipeline
 * in between invalidates the dynamic state (Vulkan static-state invalidation
 * rule). Without the depthBiasClamp device feature the clamp is forced to 0
 * (spec). Does nothing outside an active frame.
 * @param dev Device whose current frame records the state.
 * @param constant Constant factor, in units of r.
 * @param clamp Maximum total bias, in depth units.
 * @param slope Slope factor, in depth units. */
void rhi_set_depth_bias(rhi_device *dev, float constant, float clamp, float slope);
/** @brief Set the bound pipeline's push constants from offset 0.
 *
 * The stages are the bound pipeline's push-constant stages, fragment when it
 * names none. Does nothing outside an active frame.
 * @param dev Device whose current frame records the push.
 * @param data Source bytes.
 * @param size Byte count; must match the pipeline desc's push_const_size. */
void rhi_push_constants(rhi_device *dev, const void *data, uint32_t size);
/** @brief Record a non-indexed draw of one instance.
 *
 * Counts as one draw call in rhi_draw_calls. Does nothing outside an active
 * frame.
 * @param dev Device whose current frame records the draw.
 * @param vertex_count Number of vertices.
 * @param first_vertex Index of the first vertex. */
void rhi_draw(rhi_device *dev, uint32_t vertex_count, uint32_t first_vertex);
/** @brief Record an instanced draw.
 *
 * gl_VertexIndex restarts at first_vertex for each instance. Counts as one
 * draw call in rhi_draw_calls. Does nothing outside an active frame.
 * @param dev Device whose current frame records the draw.
 * @param vertex_count Vertices per instance.
 * @param instance_count Number of instances.
 * @param first_vertex First vertex index of each instance.
 * @param first_instance First instance index (the instance-rate binding's
 *                       record index). */
void rhi_draw_instanced(rhi_device *dev, uint32_t vertex_count, uint32_t instance_count,
                        uint32_t first_vertex, uint32_t first_instance);
/** @brief Return the number of draws recorded since the last frame began.
 *
 * rhi_draw and rhi_draw_instanced each count one. A perf census: the
 * draw-submission count the material batching minimizes.
 * @param dev Device.
 * @return The count, reset at every successful frame begin. */
uint32_t rhi_draw_calls(rhi_device *dev);

/* ------------------------------------------------------------------ */
/* GPU per-pass timestamp zones (env OPENPETE_GPU_TIMERS=1).
 *
 * Bracket a named unit of GPU work between begin_frame and end_frame:
 *
 *     rhi_gpu_zone_begin(dev, "prepass");
 *     native_depth_prepass();
 *     rhi_gpu_zone_end(dev);
 *
 * Zones MAY nest (begin/end pair like a stack): a zone begun while another is
 * open becomes its child, and the result carries a `depth`. `name` must be a
 * string literal or otherwise outlive the frame - it is stored by pointer and
 * handed back in results a few presents later.
 * Results are harvested by rhi_begin_frame after the slot's fence wait
 * (never blocks; frame-lagged by RHI_FRAMES_IN_FLIGHT presents) and fetched
 * with rhi_gpu_zone_results - returns the zone count; each result has elapsed
 * µs (timestampPeriod applied), `start_us` (µs from the frame's first GPU
 * timestamp, for laying zones out on a timeline), and nesting `depth`.
 * rhi_gpu_frame_token tags the current frame with a caller id (e.g. the
 * present_index); rhi_gpu_results_token returns the tag of the harvested
 * (frame-lagged) results so the caller can attribute them to the right present.
 * All zone calls are no-ops when the env is unset or the queue lacks timestamp
 * support (one boot log line either way). */
#define RHI_GPU_ZONES_MAX 128
typedef struct { const char *name; double us; double start_us; int depth; } rhi_gpu_zone_result;
/** @brief Return whether GPU pass timestamp zones are active on the device.
 *
 * @param dev Device, or NULL.
 * @return Nonzero when OPENPETE_GPU_TIMERS enabled the zones and the queue
 *         supports timestamps; 0 for NULL or when off. */
int  rhi_gpu_timers_on(const rhi_device *dev);
/** @brief Open a named GPU timestamp zone in the current frame slot.
 *
 * Writes a bottom-of-pipe timestamp for the zone's start and pushes the zone
 * on the nesting stack; the zone's depth is the stack depth before the push.
 * Does nothing when timers are off, outside an active frame, or when the slot
 * holds RHI_GPU_ZONES_MAX zones or the stack is that deep. The name is also
 * the device-lost suspect that rhi_signal_device_lost prints.
 * @param dev Device, or NULL.
 * @param name Zone label, stored by pointer; must outlive the harvest of the
 *             frame slot. */
void rhi_gpu_zone_begin(rhi_device *dev, const char *name);
/** @brief Close the innermost open GPU timestamp zone of the current frame slot.
 *
 * Writes the zone's end timestamp and pops the nesting stack. Does nothing
 * when timers are off, outside an active frame, or with no open zone. An end
 * whose begin was refused closes the enclosing zone instead.
 * @param dev Device, or NULL. */
void rhi_gpu_zone_end(rhi_device *dev);
/** @brief Copy the most recently harvested GPU zone results into `out`.
 *
 * The results are the zones of a frame RHI_FRAMES_IN_FLIGHT presents old,
 * read by rhi_begin_frame; each carries the name pointer given to
 * rhi_gpu_zone_begin. Copies at most `cap` entries.
 * @param dev Device, or NULL.
 * @param out Receives the results; NULL returns 0.
 * @param cap Capacity of @p out in entries; 0 or less returns 0.
 * @return Entries copied; 0 for NULL dev, NULL out, non-positive cap, or no
 *         harvest. */
int  rhi_gpu_zone_results(rhi_device *dev, rhi_gpu_zone_result *out, int cap);
/** @brief Tag the current frame slot's GPU zones with a caller id.
 *
 * The tag comes back from rhi_gpu_results_token when that slot's zones are
 * harvested. Does nothing when timers are off or outside an active frame.
 * @param dev Device, or NULL.
 * @param token Caller id to associate with the frame. */
void rhi_gpu_frame_token(rhi_device *dev, uint64_t token);
/** @brief Return the token tagged on the frame whose zones rhi_gpu_zone_results last harvested.
 *
 * @param dev Device, or NULL.
 * @return The rhi_gpu_frame_token value of that frame; 0 for NULL or before
 *         any harvest. */
uint64_t rhi_gpu_results_token(const rhi_device *dev);

/* ---- dev-UI (imgui) integration seam -------------------------------------
 *
 * The ONE deliberate hole in the opaque-handle abstraction: the ImGui
 * Vulkan backend must be initialized against the real device + the offscreen
 * render pass. Handles are typed as void pointers / uint64_t so rhi.h stays
 * Vulkan-free; the sole consumer (core/ui/devui.cpp) casts them back.
 *
 * render_pass is the offscreen pass (off_rp). It IS destroyed+recreated on a
 * swapchain rebuild, but always with identical attachment formats (the
 * color format is fixed, independent of the swapchain's) - and a
 * VkPipeline only uses its creation render pass for compatibility, so a
 * pipeline built against the old pass stays valid under the new one (the
 * renderer's own pipelines rely on the same rule). No re-init needed on
 * resize/vsync changes.
 *
 * command_buffer() is the frame's open command buffer, valid only between
 * rhi_begin_frame and rhi_end_frame - the dev UI records its draw data into
 * the offscreen pass, alongside the other overlays. */
typedef struct {
    void    *instance;         /* VkInstance */
    void    *physical_device;  /* VkPhysicalDevice */
    void    *device;           /* VkDevice */
    uint32_t queue_family;
    void    *queue;            /* VkQueue (graphics) */
    uint64_t render_pass;      /* VkRenderPass (offscreen color+depth pass) */
    uint64_t pipeline_cache;   /* VkPipelineCache the RHI's own pipelines compile through */
    uint32_t image_count;      /* swapchain image count (>= 2) */
    uint32_t api_version;      /* VK_API_VERSION the instance was created with */
} rhi_imgui_info;
/** @brief Fill `out` with the Vulkan handles the dev UI backend initializes from.
 *
 * The render pass is the offscreen pass, image_count is the swapchain image
 * count raised to at least 2, and api_version is Vulkan 1.2, the version the
 * instance was created with.
 * @param dev Device, or NULL.
 * @param out Destination, or NULL.
 * @return true when filled; false for NULL arguments or a headless device
 *         (no window, so the dev UI never initializes). */
bool  rhi_get_imgui_info(rhi_device *dev, rhi_imgui_info *out);
/** @brief Return the command buffer recording the current frame.
 *
 * @param dev Device, or NULL.
 * @return The VkCommandBuffer as an opaque pointer; NULL for NULL or outside
 *         an active frame. */
void *rhi_current_command_buffer(rhi_device *dev);

#endif /* OPENPETE_RHI_H */
