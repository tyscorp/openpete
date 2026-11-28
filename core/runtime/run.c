// core/runtime/run.c - openpete_run: boot the PSX EXE, optionally smoke-test
// one recompiled function, then run the host loop that drives the game fiber
// (input, tick, present, pacing, logs and captures). Under the thread split
// main pumps the window and a worker runs the loop; a build without PsyCross
// opens a window that only clears and presents.

#include "openpete_disc_build.h"
#include "openpete_audio_feed_report.h"
#include "openpete_bugreport.h"
#include "openpete_bugreport_replay.h"
#include "openpete.h"
#include "openpete_crash.h"
#include "openpete_launcher.h"   /* openpete_launcher_return_cancel */
#include "openpete_fatal.h"
#include "openpete_mutex_once.h"
#include "openpete_fiber.h"
#include "openpete_log.h"
#include "openpete_save_icon.h"
#include "openpete_knobs.h"
#include "openpete_host_input.h"
#include "openpete_host_mouse.h"
#include "openpete_input_lock.h"
#include "openpete_rumble.h"
#include "openpete_psx.h"
#include "openpete_discontinuity.h"
#include "openpete_recomp_module.h"
#include "openpete_ticker.h"
#include "openpete_time.h"
#include "openpete_present_thread.h"
#include "openpete_host_trace.h"
#include "openpete_workpool.h"
#include "openpete_frame_time_log.h"
#include "openpete_bugrec.h"
#include "openpete_pace_log.h"
#include "openpete_boot_timing.h"
#include "openpete_disc_trace.h"
#include "openpete_disc_image.h"
#include "rhi.h"
#include "../rhi/rhi_gpu_audit.h"
#include "openpete_profile.h"
#include "openpete_func_trace.h"
#include "openpete_interp.h"
#include "openpete_interp_objs.h"
#include "screenshot.h"
#include "game_extras.h"
#include "bk2_feed.h"
#include "bk2_movie.h"
#include "bk2_record.h"
#include "openpete_input_log.h"
#include "openpete_rewind.h"
#include "openpete_record.h"
#include "openpete_native_render.h"
#include "openpete_build_id.h"
#include "game_layout.h"
#include "openpete_game_runtime.h"
#include "openpete_terrain_cache.h"
#include "openpete_fb_snapshot.h"
#include "openpete_ident_audit.h"
#include "openpete_run_capture.h"
#include "openpete_menu.h"
#include "openpete_devui.h"
#include "openpete_freecam.h"
#include "openpete_keymap.h"  /* [keys] - every host hotkey below */
#include "openpete_osd.h"     /* toast the changes the player makes */
#include "openpete_rebind.h"  /* the binding page: drain, pad restamp */
#include "openpete_stick_response.h" /* --stick-response seed */
#include "feature_registry.h"
#include "openpete_mods.h"
#include "openpete_vstream.h"
#include "../host/sfx_pack.h"
#include "no_lod.h"
#include "openpete_state.h"
#include "openpete_fnv1a64.h"
#include "openpete_psxtrace.h"
#include "music_override.h"
#include "openpete_memcard.h"
#include "openpete_handoff.h"
#include "openpete_thread_roles.h"
#include "host_state_ops.h"
#include "host_loop_internal.h"
#include "host_input_tick.h"
#include "host_hotkeys.h"
#include "run_internal.h"
#include "runahead.h"
#include "eager.h"

#include "PsyX/PsyX_public.h"  /* g_PsyX_Pad_InputOverride */

#include <SDL3/SDL.h>
#include <libco.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>  /* _exit */

/* The default window comes from the game's own display environment
 * (game_default_window_w/h, game_layout.h): its height is the visible
 * DRAWENV clip height at two window rows per VRAM row, not the full DISPENV
 * height, because the game never draws into the overscan bands and PsyCross
 * crops them at present time. A game whose clip is shorter than Spyro 1's
 * therefore opens a shorter window instead of letterboxing inside a fixed
 * one. */

#define CLEAR_R 0x10
#define CLEAR_G 0x14
#define CLEAR_B 0x20

/* 1 while the canonical present's lagged re-extract (lag_re) is running -
 * lets feeder-side diagnostics tag which present leg produced a sample. */
int g_openpete_canon_lag_leg = 0;

/* PCSX-Redux trace state - file-static so the dispatch hook (which has
 * to be a plain function pointer with no closure) can find it. There's
 * only ever one open trace per run. */
static openpete_psxtrace_t* s_psxtrace_active = NULL;
/* Set during host init, consumed by the host loop - which run on different
 * threads under the split, so this cannot stay a local. Written once before
 * the worker exists and read-only thereafter. */
static openpete_psxtrace_t* psxtrace = NULL;

/* The record index the next advance() consumes, and a skip on the active
 * trace (the load-window re-alignment), for the thunk layer's anchor hook. */
uint32_t openpete_psxtrace_current_record(void) {
    return s_psxtrace_active ? s_psxtrace_active->records_consumed : 0u;
}
int openpete_psxtrace_skip_current_to(uint32_t rec) {
    return s_psxtrace_active ? openpete_psxtrace_skip_to(s_psxtrace_active, rec) : -1;
}

openpete_psxtrace_t* run_psxtrace(void) { return psxtrace; }

void run_psxtrace_release(void) {
    if (!psxtrace) return;
    openpete_psxtrace_close(psxtrace);
    psxtrace = NULL;
    s_psxtrace_active = NULL;
    openpete_psxtrace_dispatch_hook = NULL;
}

/** @brief Advance the active psxtrace by one record at a guest anchor and update its terminal status.
 *
 * Installed as openpete_psxtrace_dispatch_hook. Does nothing without an
 * active trace or once its status is 1 (end of file) or 3 (error). An
 * advance error sets status 3, end of file status 1, and a hard failure
 * with status still 0 sets status 2, which the host loop reads to exit.
 * @param addr The anchor address, unused. */
static void openpete_psxtrace_on_anchor(uint32_t addr) {
    (void)addr;
    if (!s_psxtrace_active) return;
    /* EOF (1) and error (3) are terminal - stop advancing. Divergence (2)
     * is sticky for the host loop's exit-decision but the verifier keeps
     * comparing every subsequent frame so --psxtrace-keep-going can
     * surface ALL leaks across the trace, not just the first. Only a HARD
     * failure (non-transient mismatch, or a CD-load transient that didn't
     * heal in the window) trips status 2 - a healing CD-load transient
     * leaves hard_failed clear so the run continues to EOF. */
    if (s_psxtrace_active->terminal_status == 1 ||
        s_psxtrace_active->terminal_status == 3) return;
    int rc = openpete_psxtrace_advance(s_psxtrace_active);
    if (rc < 0)      s_psxtrace_active->terminal_status = 3;
    else if (rc == 0) s_psxtrace_active->terminal_status = 1;
    else if (s_psxtrace_active->hard_failed &&
             s_psxtrace_active->terminal_status == 0) {
        s_psxtrace_active->terminal_status = 2;
    }
}

/* Does the PsyCross GL renderer still rasterize the ordering table this run?
 * Computed once at boot (after the native renderer's bring-up). When the native
 * Vulkan renderer is the sole display (pure --renderer vk, windowed) PsyCross
 * does NOT walk/submit the OT - psyq_DrawOTag skips the GL rasterization (keeping
 * only the sim-tick timing pin) and PsyX_EndScene auto-no-ops via begin_scene_flag,
 * so there's no redundant GL draw and no second vsync swap on the hidden window.
 * It STAYS true (PsyCross renders) whenever the native renderer isn't the display
 * or its output is still needed: headless / psycross mode (where the native
 * window DISPLAYS the PsyCross frame, so the rasterization is the picture) /
 * native bring-up failure / --record (taps the PsyCross present FBO) /
 * --screenshot (the -og reference). Read by psyq_DrawOTag. */
int openpete_psycross_renders = 1;

/* Cooperative shutdown flag. Set by the event pump (window close, SDL quit,
 * the quit binding) and by our SIGINT handler. The host-fiber loop checks this
 * once per iteration and drains cleanly, instead of PsyCross's PsyX_Exit()
 * calling exit(0) from inside PsyX_BeginScene (which can hang in WSL/GL with
 * the game fiber and vblank thread still mid-flight). Written by the pump,
 * the SIGINT handler and any thread calling openpete_request_shutdown, read by
 * the host loop and the pump's liveness check: a lock-free atomic, which is
 * both async-signal-safe and ordered across threads, where a volatile
 * sig_atomic_t is only the former. */
static atomic_int g_shutdown_requested = 0;

/* Sets g_shutdown_requested; documented at the declaration. */
void openpete_request_shutdown(void) { g_shutdown_requested = 1; }

/* Documented at the declaration in run_internal.h. */
bool run_shutdown_flagged(void) { return g_shutdown_requested != 0; }

/* --max-runtime-seconds as an absolute SDL_GetTicks deadline, 0 for none.
 * Written by the host loop before the game fiber first runs, read by
 * openpete_shutdown_requested on the sim thread. */
static atomic_ullong g_run_deadline_ms = 0;

/* Documented at the declaration in run_internal.h. */
void run_deadline_arm(const openpete_options_t* opts, Uint64 start) {
    if (opts->max_runtime_seconds <= 0) return;
    atomic_store(&g_run_deadline_ms,
                 (unsigned long long)(start + (Uint64)opts->max_runtime_seconds * 1000));
}

/* Reads the flag and the deadline; documented at the declaration. */
int openpete_shutdown_requested(void) {
    if (g_shutdown_requested) return 1;
    const unsigned long long d =
        atomic_load_explicit(&g_run_deadline_ms, memory_order_relaxed);
    return d != 0 && SDL_GetTicks() > d;
}

#ifndef _WIN32
/* Seconds the process may go without reaching the next teardown step
 * after a SIGINT or SIGTERM before the backstop ends it. The first signal
 * arms alarm(); each sim_teardown_progress re-arms it, so the bound is per
 * step, not on the whole teardown (a recorder flush may take seconds). */
#define QUIT_BACKSTOP_S 30
#define QUIT_BACKSTOP_STR2(x) #x
#define QUIT_BACKSTOP_STR(x) QUIT_BACKSTOP_STR2(x)
/* Set by the first SIGINT/SIGTERM; re-arming happens only while set. */
static atomic_int g_quit_backstop_armed = 0;

/** @brief SIGALRM handler: the quit request went unanswered for QUIT_BACKSTOP_S seconds; report and exit with the panic code.
 *
 * Writes one line to the descriptors the crash report uses (the original
 * stderr and the log file when the file sink is active), since text in the
 * stderr pipe dies with the process. Only write and _exit are called.
 * @param sig The signal number, unused. */
static void openpete_on_quit_backstop(int sig) {
    (void)sig;
    static const char msg[] =
        "quit-backstop: no shutdown progress for "
        QUIT_BACKSTOP_STR(QUIT_BACKSTOP_S) " s "
        "after the quit signal, forcing exit\n";
    int fds[2];
    const int n = openpete_log_file_crash_fds(fds);
    for (int i = 0; i < n; i++) {
        ssize_t w = write(fds[i], msg, sizeof msg - 1);
        (void)w;
    }
    _exit(OPENPETE_EXIT_PANIC);
}
#endif

/* Set by the event pump on a window double-click, by
 * openpete_native_request_fullscreen_toggle from any thread, and by the
 * host loop on its own key edge under the thread split; consumed with an
 * atomic exchange by the thread that owns the window, so a raise between a
 * read and a clear is never lost and a consumed request is never raised
 * again by an observer. Kept as a flag so the heavy SDL_SetWindowFullscreen
 * call runs in a loop, not in the event handler. */
static SDL_AtomicInt g_native_fs_toggle_request;

/* Raises g_native_fs_toggle_request; documented at the declaration. */
void openpete_native_request_fullscreen_toggle(void)
{
    SDL_SetAtomicInt(&g_native_fs_toggle_request, 1);
}

/* The process's single event pump.
 *
 * openpete owns the SDL_PollEvent loop; every dequeued event is routed on to
 * PsyCross's handlers (controller hotplug, resize, its debug keys, text input,
 * Alt tracking) so those keep working without PsyCross draining the queue
 * itself. SDL_PollEvent drains one shared queue, so a second drain does not
 * split the work - it steals events from whichever loop polls second.
 *
 * Each event goes to openpete_handle_event first, which consumes the ones
 * openpete owns outright - quit, window close, Escape's overlay dismiss, the
 * quit binding - and forwards the rest. Consuming quit/close here is what
 * keeps PsyX_Exit (and its exit(0), which hangs on static teardown under
 * WSL+GL+libco) unreachable. */
/** @brief Read OPENPETE_THREAD_SPLIT once: whether the host loop may move to a worker thread while main pumps the window.
 *
 * Win32 runs a modal loop inside DefWindowProc for a title-bar drag or a
 * resize, which blocks the thread that owns the window. With the window and
 * the pump on main and the host loop on a worker (run_split_threaded), a
 * drag blocks main while the worker keeps ticking. OPENPETE_THREAD_SPLIT=0
 * keeps the host loop on main, the A/B for any defect suspected of coming
 * from the split. openpete_split_applies adds the configuration conditions.
 * @return 1 when the variable is unset, empty, or does not start with '0';
 *         else 0. */
static int openpete_thread_split_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_THREAD_SPLIT");
        v = (e && *e) ? (*e != '0') : 1;   /* default ON */
    }
    return v;
}

/* Documented at the declaration in run_internal.h. */
bool openpete_split_applies(const openpete_options_t* opts)
{
    return openpete_thread_split_enabled()
        && !opts->headless
        && (opts->renderer == OPENPETE_RENDERER_VK
            || opts->renderer == OPENPETE_RENDERER_PSYCROSS)
        && opts->screenshot_frame_count <= 0
        && opts->screenshot_presents_start < 0
        && !openpete_host_input_legacy();
}

/* Set once the worker has finished, so main can leave its pump loop. */
static SDL_AtomicInt g_worker_done;
/* Incremented once per host-loop iteration on the sim thread. The pump
 * thread reads it to tell a hung sim (a runaway guest loop, a deadlocked
 * feeder) from a busy one, and to force the process down when a quit
 * request goes unanswered. */
static SDL_AtomicInt g_sim_heartbeat;
/* Set by the worker when its loop has exited and teardown (recorder
 * flush, movie finalize, joins) is running; the pump then allows the
 * longer teardown bound between heartbeats. */
static SDL_AtomicInt g_sim_teardown;

/* Documented at the declaration in run_internal.h. */
void run_sim_heartbeat(void) { SDL_AddAtomicInt(&g_sim_heartbeat, 1); }

/* Documented at the declaration in run_internal.h. */
void sim_teardown_progress(void) {
    SDL_AddAtomicInt(&g_sim_heartbeat, 1);
#ifndef _WIN32
    if (atomic_load(&g_quit_backstop_armed)) alarm(QUIT_BACKSTOP_S);
#endif
}

void sim_teardown_begin(void) {
    SDL_SetAtomicInt(&g_sim_teardown, 1);
    sim_teardown_progress();
}

/** @brief Take a key event while a binding capture owns the keyboard.
 *
 * From the capture's start until the inputs it latched are released,
 * Escape posts the cancel (the key is latched so the poll does not also
 * fire Escape's binding), the quit test is skipped, and no key event or
 * repeat reaches PsyCross's debug keys, so Backspace stays capturable.
 * @param ev the event.
 * @return true when the event is a key event the capture owns. */
static bool capture_owns_key_event(const SDL_Event* ev) {
    if ((ev->type != SDL_EVENT_KEY_DOWN && ev->type != SDL_EVENT_KEY_UP) ||
        !openpete_keymap_capture_active())
        return false;
    if (ev->type == SDL_EVENT_KEY_DOWN && !ev->key.repeat &&
        ev->key.scancode == SDL_SCANCODE_ESCAPE &&
        KM_CAPWORD_STATE(openpete_keymap_capture_word()) != KM_CAP_IDLE) {
        openpete_keymap_consume_key(ev->key.scancode);
        openpete_rebind_request_cancel("escape");
    }
    return true;
}

/* Documented at the declaration in run_internal.h. */
void sim_teardown_injected_stall(void)
{
    const char* e = getenv("OPENPETE_TEARDOWN_STALL_MS");
    const int ms = (e && *e) ? atoi(e) : 0;
    if (ms <= 0) return;
    OPL_WARN_F("host", "OPENPETE_TEARDOWN_STALL_MS: stalling teardown %d ms", ms);
    SDL_Delay((Uint32)ms);
}

static bool openpete_handle_event(SDL_Event* ev);

/* Idle-hide timer for the OS cursor: last mouse activity, in SDL ticks.
 * Written by the event pump (main thread) and read by the host loop (the
 * worker, once the thread split applies), hence the atomic. Unsigned 32-bit
 * differences stay correct across the ~49-day wrap. Seeded to 0 rather than
 * "now" would hide the cursor for the first two seconds of a session; the
 * host loop seeds it from the current tick on its first pass instead. */
static SDL_AtomicU32 g_mouse_last_activity_ms;

/* Documented at the declaration in run_internal.h. */
void openpete_cursor_update(bool headless)
{
    enum { CURSOR_IDLE_HIDE_MS = 2000 };
    if (headless) return;

    Uint32 now_ms = (Uint32)SDL_GetTicks();
    static int cursor_shown = -1;   /* -1 = not yet applied */
    if (cursor_shown < 0)           /* first pass: start the idle clock */
        SDL_SetAtomicU32(&g_mouse_last_activity_ms, now_ms);

    Uint32 idle_ms = now_ms - SDL_GetAtomicU32(&g_mouse_last_activity_ms);
    bool want_cursor = openpete_devui_is_open() ||
                       openpete_devui_wants_mouse() ||
                       openpete_menu_is_open() ||
                       idle_ms < CURSOR_IDLE_HIDE_MS;
    if ((int)want_cursor == cursor_shown) return;

    if (want_cursor) SDL_ShowCursor();
    else             SDL_HideCursor();
    cursor_shown = (int)want_cursor;
}

/** @brief Read OPENPETE_PUMP_PROBE once: whether the event pump times its steps.
 *
 * A diagnostic for a pump that stalls: with it set, each drain first runs
 * the joystick update and the platform message pump as separate timed
 * calls, and each step or event handler that takes 20 ms or longer is
 * logged with its duration and event type.
 * @return 1 when the variable is set to a value not starting with '0'. */
static int pump_probe_enabled(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("OPENPETE_PUMP_PROBE");
        v = (e && *e && *e != '0') ? 1 : 0;
    }
    return v;
}

/** @brief Log one pump step that took 20 ms or longer, under OPENPETE_PUMP_PROBE.
 *
 * @param step The step's name.
 * @param t0 op_now at the step's start.
 * @param type The event type the step dequeued or handled, or 0. */
static void pump_probe_note(const char* step, op_time_t t0, unsigned type)
{
    const op_time_t ns = op_now() - t0;
    if (ns >= 20000000)
        OPL_INFO_F("pump", "slow %s: %.3f ms (event 0x%x)", step,
                   (double)ns / 1e6, type);
}

/** @brief Drain the SDL event queue with each step timed, under OPENPETE_PUMP_PROBE.
 *
 * SDL_UpdateJoysticks and SDL_PumpEvents run first as separate calls, so a
 * stall is attributed to joystick detection and polling or to the
 * platform message pump; the drain then times each SDL_PollEvent (which
 * pumps again) and each handler. The summary line counts the drain's
 * events and its duration when it took 20 ms or longer. */
static void openpete_pump_events_probed(void) {
    SDL_Event ev;
    const op_time_t d0 = op_now();
    op_time_t t0 = d0;
    SDL_UpdateJoysticks();
    pump_probe_note("joystick update", t0, 0);
    t0 = op_now();
    SDL_PumpEvents();
    pump_probe_note("message pump", t0, 0);
    unsigned n = 0;
    for (;;) {
        t0 = op_now();
        if (!SDL_PollEvent(&ev)) break;
        pump_probe_note("poll", t0, ev.type);
        t0 = op_now();
        if (openpete_handle_event(&ev))
            PsyX_Sys_HandleEvent(&ev);
        pump_probe_note("handler", t0, ev.type);
        n++;
    }
    const op_time_t drain = op_now() - d0;
    if (drain >= 20000000)
        OPL_INFO_F("pump", "slow drain: %.3f ms, %u event(s)",
                   (double)drain / 1e6, n);
}

/* Documented at the declaration in run_internal.h. */
void openpete_pump_events(void) {
    SDL_Event ev;
    if (pump_probe_enabled())
        openpete_pump_events_probed();
    else
        while (SDL_PollEvent(&ev)) {
            if (openpete_handle_event(&ev))
                PsyX_Sys_HandleEvent(&ev);
        }
    /* SDL text input is a window op of this thread; the overlay decides on
     * the present thread whether a field wants it. */
    openpete_devui_pump_text_input();
    /* A pending bug report's device and monitor facts: SDL lists keyboards,
     * mice and displays only on this thread. */
    openpete_bugreport_main_poll();
    /* Actuators are driven from here because this thread owns the SDL gamepad
     * handles: PsyCross opens and closes them in the controller-added /
     * -removed handlers just above, so applying on any other thread would race
     * a close. It also puts the focus gate where focus is knowable. */
    openpete_rumble_apply();
}

/** @brief Handle one event dequeued by the event pump and decide whether PsyCross also sees it.
 *
 * In order: the bug-report input ring records the event; the input lock
 * drops physical-device events; the mouse channel
 * and the dev overlay see every remaining event; a joystick arrival is
 * reported against the mapping databases; mouse activity re-arms the
 * cursor idle timer. Quit and window close set the shutdown flag and are
 * consumed, so PsyX_Exit stays unreachable. A left double-click raises the
 * fullscreen toggle request unless the overlay, the menu, a mouse capture
 * or a mouse:left binding claims it. Escape closes an open dev overlay or
 * settings menu and is consumed until release; the quit binding shuts
 * down; an Escape left over is consumed so it cannot reach PsyCross's debug
 * keys.
 * @param ev The event.
 * @return true to forward the event to PsyCross, false when consumed. */
static bool openpete_handle_event(SDL_Event* ev) {
    /* The input lock discards an event from a physical input device before
     * any consumer below sees it. Quit and window events are not input and
     * fall through, so closing the window still exits. */
    const int locked = openpete_input_lock_drops_event(ev);
    openpete_bugrec_input_event(ev, locked);
    if (locked) return false;
    /* The mouse channel derives position, mask and the running totals from
     * the event stream (openpete_host_mouse.h); every consumer reads those. */
    openpete_host_mouse_handle_event(ev);
    /* Dev overlay sees every event (imgui io bookkeeping; frames run only
     * while openpete_devui_frame_wanted: overlay open, fill-tag inspector
     * active, or an enabled always-visible mod section), including ones
     * consumed below. */
    openpete_devui_process_event(ev);
    /* A device arriving after the mapping databases loaded still resolves
     * against them; say which one supplied it, or that none did. */
    if (ev->type == SDL_EVENT_JOYSTICK_ADDED)
        openpete_host_input_pad_arrived(ev->jdevice.which);
    /* Any mouse activity re-arms the idle-hide timer applied in the host
     * loop. Buttons and the wheel count as well as motion: a user clicking
     * without moving is still using the pointer. */
    if (ev->type == SDL_EVENT_MOUSE_MOTION ||
        ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        ev->type == SDL_EVENT_MOUSE_BUTTON_UP ||
        ev->type == SDL_EVENT_MOUSE_WHEEL) {
        SDL_SetAtomicU32(&g_mouse_last_activity_ms, (Uint32)SDL_GetTicks());
    }
    if (ev->type == SDL_EVENT_QUIT) {
        g_shutdown_requested = 1;
        openpete_launcher_return_cancel();   /* a quit ends the process */
        return false;  /* never reaches PsyCross, so PsyX_Exit stays unreachable */
    }
    /* Double-click toggles native-window fullscreen (see the host loop) -
     * unless the click landed on the dev overlay, the settings menu is open,
     * a camera or mod has requested capture, or some action binds
     * mouse:left (two quick presses of it are not a window gesture). */
    if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        ev->button.button == SDL_BUTTON_LEFT && ev->button.clicks == 2 &&
        !openpete_devui_wants_mouse() &&
        openpete_host_mouse_double_click_allowed()) {
        openpete_host_mouse_double_click_engaged();
        SDL_SetAtomicInt(&g_native_fs_toggle_request, 1);
    }
    if (ev->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        g_shutdown_requested = 1;
        openpete_launcher_return_cancel();   /* closing the window ends the process */
        return false;
    }
    /* The pump runs on the window's owner, the one thread that may read its
     * flags; the published state serves every other reader. */
    if (ev->type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
        ev->type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN)
        openpete_native_publish_fullscreen();
    /* Quit-by-key, and Escape's overlay dismiss.
     *
     * Handled here rather than through the polled hotkey block because the
     * pump sees one key event at a time: polling would have to re-derive
     * "was this press already consumed by closing the menu?" from held
     * state, and would quit the game on the press that only closes the menu.
     *
     * Quitting is not gated on gamepad presence, so a synthetic keystroke
     * from a controller-emulation layer can quit; `quit = "none"` unbinds
     * it (see km_sdl_key_down in keymap_sdl.c). The window-close path above
     * quits regardless of any binding. */
    if (capture_owns_key_event(ev)) return false;
    if (ev->type == SDL_EVENT_KEY_DOWN) {
        const bool esc = (ev->key.scancode == SDL_SCANCODE_ESCAPE);

        /* Escape dismisses an open overlay whatever `quit` is bound to -
         * a UI convention rather than a binding, and it keeps unbinding
         * quit from stranding a panel open. (The open flags are plain
         * cross-thread reads; the menu close is routed through a request
         * the host loop consumes at its poll point.)
         *
         * Consuming the key is what stops the dismiss fighting a polled
         * binding on the same key: with `menu` bound to Escape the poll
         * would otherwise still see it held and immediately reopen what
         * this just closed. Consumed until release, so pressing Escape
         * again reopens - the dismiss and the toggle compose. */
        if (esc && openpete_devui_is_open()) {
            openpete_devui_close();
            openpete_keymap_consume_key(ev->key.scancode);
            return false;
        }
        if (esc && openpete_menu_is_open()) {
            /* Requested, not toggled: under the thread split this runs on
             * the pump thread, and the host loop's pad-override clear latch
             * reads the open flag twice per iteration. The request lands
             * between those reads. */
            openpete_host_mouse_request_menu(0);
            openpete_keymap_consume_key(ev->key.scancode);
            return false;
        }

        if (openpete_keymap_key_binds(OPK_QUIT, ev->key.scancode)) {
            g_shutdown_requested = 1;
            openpete_launcher_return_cancel();   /* the quit key ends the process */
            return false;
        }
        /* Escape with quit unbound or rebound elsewhere: swallow it anyway
         * so it can't reach PsyCross's debug-key dispatch. */
        if (esc) return false;
    }
    return true;  /* everything else goes on to PsyCross */
}

/** @brief SIGINT and SIGTERM handler: request a graceful shutdown, and exit at once on a second signal.
 *
 * The first signal sets g_shutdown_requested and, on POSIX, arms the quit
 * backstop (alarm, QUIT_BACKSTOP_S), which ends a process whose shutdown
 * stops making progress with no second signal coming; a second signal
 * calls _exit(130).
 * @param sig The signal number, unused. */
static void openpete_on_sigint(int sig) {
    (void)sig;
    if (g_shutdown_requested) {
        /* Second signal: something is hung in graceful shutdown - escape
         * hatch so the user isn't stuck. _exit is async-signal-safe. */
        _exit(130);  /* 128 + SIGINT */
    }
    g_shutdown_requested = 1;
    openpete_launcher_return_cancel();   /* async-signal-safe: one atomic store */
#ifndef _WIN32
    atomic_store(&g_quit_backstop_armed, 1);
    alarm(QUIT_BACKSTOP_S);
#endif
}

/* Documented at the declaration in run_internal.h. */
const char* feed_key_kind_name(void) {
    return openpete_feed_key() == OPENPETE_FEED_KEY_VBL ? "vbl" : "frame";
}

/* Documented at the declaration in run_internal.h. */
void position_log_emit(FILE* fp, unsigned frame, int vbl) {
    int32_t x = 0, y = 0, z = 0;
    if (game_get_player_position(&x, &y, &z) != 0) return;
    fprintf(fp, "%u,%d,%d,%d,%d\n", frame, vbl, x, y, z);
}

/* Default-empty fallback for the gameplay-state regions table. Per-game
 * TUs call openpete_state_regions_register() from game main (explicit
 * registration, NOT weak-symbol override: MinGW's PE/COFF linker
 * mis-resolves weak overrides in this configuration - count resolved
 * strong while the pointer resolved weak-NULL, a NULL deref the moment
 * --psxtrace-verify computed the first hash on Windows). Without
 * registration the hash log is the FNV1a-64 offset basis on every
 * frame - useless but link-safe. */
const OpenpeteStateRegion* openpete_state_regions = NULL;
size_t openpete_state_region_count = 0;
uint64_t openpete_state_regions_table_hash = 0;

void openpete_state_regions_register(const OpenpeteStateRegion* regions,
                                     size_t count, uint64_t table_hash) {
    openpete_state_regions = regions;
    openpete_state_region_count = count;
    openpete_state_regions_table_hash = table_hash;
}

uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region);
/** @brief Compute the FNV-1a-64 hash over every registered gameplay-state region, and optionally one hash per region.
 *
 * Covers every region the per-game TU registered in openpete_state_regions.
 * Any change to gameplay state appears as a hash divergence at the first
 * tick the change reaches a registered region; the per-region hashes name
 * that region without a RAM dump. Indirect regions read their base pointer
 * from RAM and are skipped when it is NULL or its slot lies outside RAM;
 * END_PTR, COUNT and SENTINEL kinds clamp the hashed length to the live
 * part of the pool. Bytes at a region's mask offsets are excluded from both
 * the per-region and the total hash.
 * @param out_per_region NULL, or an array of at least
 *        openpete_state_region_count slots receiving each region's hash;
 *        0 marks a skipped region.
 * @return The total hash over all regions in registration order. */
uint64_t gameplay_hash_compute_ex(uint64_t* out_per_region) {
    uint64_t total = openpete_fnv1a64_init();
    const uint8_t* ram = openpete_psx_get_ram();
    const uint32_t RAM_BYTES = 2u * 1024u * 1024u;
    for (size_t r = 0; r < openpete_state_region_count; r++) {
        const OpenpeteStateRegion* reg = &openpete_state_regions[r];
        if (out_per_region) out_per_region[r] = 0;
        uint32_t base_off;
        uint32_t live_len = reg->length;
        const int is_indirect = (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY
                              || reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_END_PTR
                              || reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_COUNT
                              || reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_SENTINEL);
        uint32_t base_vaddr = 0;
        /* Gameplay-state regions are canonical-2 MB by contract (regions
         * registered via psx_runtime_register_region are presentation-
         * only and explicitly excluded from the determinism gate).
         * psx_runtime_canonicalize is `addr & 0x1FFFFF` in release; in
         * debug it asserts the input doesn't fall in a registered
         * region's band - catching a future state-region manifest
         * mistake before it silently mirrors into g_ram. */
        if (is_indirect) {
            uint32_t ptr_off = psx_runtime_canonicalize(reg->start);
            if (ptr_off + 4u > RAM_BYTES) continue;
            base_vaddr =
                  (uint32_t)ram[ptr_off]
                | ((uint32_t)ram[ptr_off + 1] << 8)
                | ((uint32_t)ram[ptr_off + 2] << 16)
                | ((uint32_t)ram[ptr_off + 3] << 24);
            if (base_vaddr == 0) continue;
            base_off = psx_runtime_canonicalize(base_vaddr);
        } else {
            base_off = psx_runtime_canonicalize(reg->start);
        }
        if (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_END_PTR) {
            uint32_t aux_off = psx_runtime_canonicalize(reg->aux);
            if (aux_off + 4u <= RAM_BYTES) {
                uint32_t end_vaddr =
                      (uint32_t)ram[aux_off]
                    | ((uint32_t)ram[aux_off + 1] << 8)
                    | ((uint32_t)ram[aux_off + 2] << 16)
                    | ((uint32_t)ram[aux_off + 3] << 24);
                /* Allocator end-pointer < base means the pool is empty
                 * (alloc_ptr reset to NULL). The deallocated residue
                 * isn't game-readable; without this clamp we'd hash
                 * stale bytes in [base..base+reg->length) that vary
                 * across emulator runs without affecting game state. */
                if (end_vaddr >= base_vaddr) {
                    uint32_t live = end_vaddr - base_vaddr;
                    if (live < live_len) live_len = live;
                } else {
                    live_len = 0;
                }
            }
        } else if (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_COUNT) {
            uint32_t aux_off = psx_runtime_canonicalize(reg->aux);
            if (aux_off + 4u <= RAM_BYTES && reg->elem_size > 0u) {
                uint32_t cnt =
                      (uint32_t)ram[aux_off]
                    | ((uint32_t)ram[aux_off + 1] << 8)
                    | ((uint32_t)ram[aux_off + 2] << 16)
                    | ((uint32_t)ram[aux_off + 3] << 24);
                uint64_t live = (uint64_t)cnt * (uint64_t)reg->elem_size;
                if (live < (uint64_t)live_len) live_len = (uint32_t)live;
            }
        } else if (reg->kind == OPENPETE_REGION_INDIRECT_ARRAY_SENTINEL) {
            const uint32_t es = reg->elem_size;
            const uint32_t s_off = reg->aux & 0xFFu;
            const uint8_t  s_val = (uint8_t)((reg->aux >> 8) & 0xFFu);
            if (es > 0u && s_off < es) {
                uint32_t n = 0;
                while ((uint64_t)(n + 1u) * es <= live_len &&
                       base_off + (uint64_t)n * es + s_off < RAM_BYTES &&
                       ram[base_off + n * es + s_off] != s_val)
                    n++;
                live_len = n * es;
            }
        }
        uint32_t end = base_off + live_len;
        if (end > RAM_BYTES) end = RAM_BYTES;
        if (end <= base_off) continue;
        size_t len = end - base_off;
        const uint8_t* p = ram + base_off;
        /* Mask-aware hash: for regions tagged with mask_offsets, skip
         * the masked bytes during FNV update for BOTH the per-region
         * and total hashes. Both absolute and per-element variants
         * supported (see openpete_psx.h). When mask_count==0, falls
         * through to the existing bulk-update fast path so non-masked
         * regions retain identical hashes to pre-mask-aware code. */
        if (reg->mask_count == 0) {
            if (out_per_region) out_per_region[r] = openpete_fnv1a64(p, len);
            total = openpete_fnv1a64_update(total, p, len);
        } else {
            const uint16_t* m  = reg->mask_offsets;
            const uint16_t mc  = reg->mask_count;
            const uint32_t es  = reg->elem_size; /* 0 if not per-element */
            const int per_el   = (reg->mask_per_element != 0u && es != 0u);
            uint64_t per = openpete_fnv1a64_init();
            for (size_t i = 0; i < len; i++) {
                uint32_t off = per_el ? (uint32_t)(i % es) : (uint32_t)i;
                int masked = 0;
                for (uint16_t mi = 0; mi < mc; mi++) {
                    if ((uint32_t)m[mi] == off) { masked = 1; break; }
                }
                if (masked) continue;
                per   ^= (uint64_t)p[i];
                per   *= 0x100000001B3ULL;
                total ^= (uint64_t)p[i];
                total *= 0x100000001B3ULL;
            }
            if (out_per_region) out_per_region[r] = per;
        }
    }
    return total;
}

#define OPENPETE_HASH_LOG_MAX_REGIONS 128

/* Documented at the declaration in run_internal.h. */
void gameplay_hash_log_emit_header(FILE* fp) {
    fputs("frame,vbl,fnv1a64", fp);
    /* One column per registered region, headed by its name. The
     * harness uses these to pinpoint which region first diverged.
     * Capped at OPENPETE_HASH_LOG_MAX_REGIONS to keep the per-frame
     * line bounded; over the cap we still emit the total hash and
     * skip extra columns (an error is logged). */
    size_t n = openpete_state_region_count;
    if (n > OPENPETE_HASH_LOG_MAX_REGIONS) {
        OPL_ERROR_F("hash-log",
                    "too many regions (%zu > %d); per-region columns truncated",
                    n, OPENPETE_HASH_LOG_MAX_REGIONS);
        n = OPENPETE_HASH_LOG_MAX_REGIONS;
    }
    for (size_t r = 0; r < n; r++) {
        const char* name = openpete_state_regions[r].name;
        fputc(',', fp);
        /* Sanitize: strip commas / newlines defensively. */
        for (const char* c = name; *c; c++) {
            fputc((*c == ',' || *c == '\n' || *c == '\r') ? '_' : *c, fp);
        }
    }
    fputc('\n', fp);
}

/* Documented at the declaration in run_internal.h. */
void gameplay_hash_log_emit(FILE* fp, unsigned frame, int vbl, uint64_t total,
                            const uint64_t* per_region) {
    size_t n = openpete_state_region_count;
    if (n > OPENPETE_HASH_LOG_MAX_REGIONS) n = OPENPETE_HASH_LOG_MAX_REGIONS;
    fprintf(fp, "%u,%d,%016llx", frame, vbl, (unsigned long long)total);
    for (size_t r = 0; r < n; r++) {
        fprintf(fp, ",%016llx", (unsigned long long)per_region[r]);
    }
    fputc('\n', fp);
}

/* Implemented in core/psx/exe_loader.cpp. */
extern uint8_t* openpete_psx_load_exe_from_disc(const char* cue_path,
                                                const char* exe_filename,
                                                size_t* out_size);
extern uint8_t* openpete_psx_read_file(const char* path, size_t* out_size);

extern void openpete_psx_set_diag_cpu(CPUState* cpu);

/* CPUState seed file format. Little-endian throughout. Total 528 bytes:
 *   uint32_t magic            = 'CSv1' (LE 0x31765343)
 *   uint32_t gpr[32]          = zero, at, v0, v1, a0..a3, t0..t7, s0..s7,
 *                               t8, t9, k0, k1, gp, sp, fp, ra
 *   uint32_t pc, hi, lo
 *   uint32_t cop0[32]
 *   uint32_t gte_data[32]
 *   uint32_t gte_ctrl[32]
 * Written by scripts/pcsx-redux-ram-dump.lua when PCSXR_DUMP_CPU is set. */
#define OPENPETE_CPU_SEED_MAGIC 0x31765343u   /* 'CSv1' little-endian */
#define OPENPETE_CPU_SEED_SIZE  528u

/** @brief Load a CPUState seed file into cpu: the GPRs, pc, hi, lo, cop0 and both GTE register files.
 *
 * The format is described above (528 bytes, magic 'CSv1'). Bytes past the
 * 528th are ignored and no other CPUState field is touched. A failure is
 * logged and leaves cpu unchanged.
 * @param cpu The CPU state to fill.
 * @param path The seed file.
 * @return 0 on success, -1 when the file cannot be opened, is shorter than
 *         528 bytes, or has a wrong magic. */
static int load_cpu_seed(CPUState* cpu, const char* path) {
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        OPL_ERROR_F("seed-cpu", "fopen %s failed: %s", path, strerror(errno));
        return -1;
    }
    uint32_t buf[OPENPETE_CPU_SEED_SIZE / 4];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n != sizeof(buf)) {
        OPL_ERROR_F("seed-cpu", "%s: short read %zu of %u",
                    path, n, OPENPETE_CPU_SEED_SIZE);
        return -1;
    }
    if (buf[0] != OPENPETE_CPU_SEED_MAGIC) {
        OPL_ERROR_F("seed-cpu", "%s: bad magic 0x%08X (expected 0x%08X 'CSv1')",
                    path, buf[0], OPENPETE_CPU_SEED_MAGIC);
        return -1;
    }
    /* GPRs: 32 contiguous uint32s in CPUState declaration order. */
    memcpy(&cpu->zero,    &buf[1],    32 * sizeof(uint32_t));
    cpu->pc = buf[33];
    cpu->hi = buf[34];
    cpu->lo = buf[35];
    memcpy(cpu->cop0,     &buf[36],   32 * sizeof(uint32_t));
    memcpy(cpu->gte_data, &buf[68],   32 * sizeof(uint32_t));
    memcpy(cpu->gte_ctrl, &buf[100],  32 * sizeof(uint32_t));
    return 0;
}

#ifdef OPENPETE_HAS_PSYCROSS
/* PsyCross window/GL initialiser. Must be called before any libgpu/libgte
 * call (ResetGraph, SetGeomScreen, etc.) - those touch internal SDL/GL
 * state set up here. */
/* Returns 0 if the window, core or PSX GPU state failed to come up. Ignoring
 * that leaves PsyCross's GL entry points unresolved, and the first libgpu call
 * jumps through a null proc pointer. */
extern int PsyX_Initialise(char* windowName, int screenWidth,
                           int screenHeight, int fullscreen, int startHidden);
extern void PsyX_Shutdown(void);
extern void PsyX_SetDithering(int enable);

/** @brief Set PsyCross's ordered dither: off in a run that writes -og screenshots, on otherwise.
 *
 * The PsyCross raster adds the 4x4 dither matrix to every shaded polygon
 * whose draw state asks for it. A run that writes the -og reference
 * (--screenshot-on-frame/-tick or --screenshot-presents-start without
 * --screenshot-native-only) draws it undithered, so the reference compares
 * with the native render, which is undithered unless --dither is set. Every
 * other run keeps the dither: PsyCross's framebuffer is copied back into the
 * VRAM mirror that StoreImage reads into guest RAM, so those bytes stay as
 * they were. OPENPETE_PSYX_DITHER=0|1 overrides either default. Read once at
 * boot.
 * @param opts Resolved launch options. */
static void apply_psyx_dither_env(const openpete_options_t* opts)
{
    const bool og_written = !opts->screenshot_native_only
        && (opts->screenshot_frame_count > 0 || opts->screenshot_presents_start >= 0);
    const char* v = getenv("OPENPETE_PSYX_DITHER");
    int on = !og_written;
    if (v && (strcmp(v, "0") == 0 || strcmp(v, "1") == 0)) on = v[0] == '1';
    PsyX_SetDithering(on);
}
#include "openpete_disc_cache.h"
extern char PsyX_BeginScene(void);
extern void PsyX_EndScene(void);
extern void PsyX_UpdateInput(void);
/* Declared in PsyCross's private src/audio/PsyX_SPUAL.h and PsyX_SPUFeed.h,
 * which are not on any public include path. Runahead and rewind replay run
 * the SPU speculatively (its frames are committed or dropped, never fed
 * twice); pause, fast-forward and replay hold the device feed. */
extern int PsyX_SPUAL_SetMute(int on_off);
extern void PsyX_SPUAL_Speculate(int on, int commit);
extern void PsyX_SPUAL_SetCapture(int on);
extern void PsyX_SPUFeed_SetHold(int on);
extern void PsyX_SPUFeed_SetTap(const char* path);
extern void PsyX_SPUFeed_SetTone(double hz);
extern void PsyX_SPUFeed_Simulate(double drift, unsigned int seed, double start_stall);
extern void PsyX_SPUFeed_SetNominalRate(double rate);
extern void PsyX_SPUFeed_SetPhase(int phase, const char* name, int tag);
extern void PsyX_SPUFeed_SetLog(const char* path);

/** @brief Log one line of the audio feed's exit summary at info level.
 * @param ud Unused.
 * @param line The line. */
static void audio_feed_log_line(void* ud, const char* line) {
    (void)ud;
    OPL_INFO_F("audio", "%s", line);
}

/** @brief Log the SPU's feature engagement and host-input counters, then the device feed's counters (frames pushed, held and consumed, device periods, underruns, primes, the depth target and percentiles, the rate estimate), its frame accounting, its drift and producer speed, its time stretch, and its per-phase counters, at exit. */
static void audio_feed_exit_summary(void) {
    OP_ROLE_ASSERT(sim);
    openpete_audio_feed_snap_t* snap = openpete_audio_feed_snapshot(1, 0);
    openpete_audio_feed_counters(snap, audio_feed_log_line, NULL);
    openpete_audio_feed_snap_free(snap);
}

/* PsyCross's GL window - created by PsyX_Initialise. We need it so --headless
 * can hide the window post-init (we keep a real GL driver for the context;
 * SDL's offscreen driver gives back a context with NULL function pointers
 * and we segfault on the first gl* call from GR_InitialisePSX). */
extern SDL_Window* g_window;

/** @brief Write the resolved [keys.pad] bindings into PsyCross's g_cfg_keyboardMapping and g_cfg_controllerMapping.
 *
 * PsyCross reads both exported structs on every internal pad update and
 * fills them only from its hardcoded table in PsyX_Sys_InitialiseInput, so
 * the boot stamp runs right after PsyX_Initialise; a binding edit restamps
 * on the thread that samples the pad (pad_restamp_if_due), the structs'
 * only reader. Each field holds one keyboard scancode or one gamepad button
 * or axis; the field shape of each action is the keymap's table
 * (openpete_keymap_pad_shape), which the binding page admits by: one key
 * and one pad source per button, a key only for the four WASD stick
 * directions (they share the gc_axis_left_* fields), and one gamepad axis
 * for the four stick axes. A source the shape has no field for is ignored.
 * An unbound field is set to PsyCross's unbound value
 * (SDL_SCANCODE_UNKNOWN, SDL_GAMEPAD_BUTTON_INVALID,
 * SDL_GAMEPAD_AXIS_INVALID), so a removed source unbinds live. The stamp
 * covers player 1: players 2-4 decode through the shared controller
 * mapping.
 * @param warn nonzero at boot: warn about ignored sources and [keys.pad2..4]
 *        rows. */
static void stamp_psx_pad_bindings(int warn) {
    /* Every field of both structs, paired with the action that feeds it;
     * SIZE_MAX marks the half an action's shape has no field for. */
    static const struct {
        openpete_action_t action;
        size_t kbd_off;    /* offset into PsyXKeyboardMapping */
        size_t pad_off;    /* offset into PsyXControllerMapping */
        const char* name;
    } map[] = {
#define KM_PAD_FIELD(act, kfield, gfield, nm) \
        { act, offsetof(PsyXKeyboardMapping, kfield), \
               offsetof(PsyXControllerMapping, gfield), nm }
#define KM_PAD_KEY(act, kfield, nm) \
        { act, offsetof(PsyXKeyboardMapping, kfield), SIZE_MAX, nm }
#define KM_PAD_AXIS(act, gfield, nm) \
        { act, SIZE_MAX, offsetof(PsyXControllerMapping, gfield), nm }
        KM_PAD_FIELD(OPK_PAD_CROSS,      kc_cross,      gc_cross,      "cross"),
        KM_PAD_FIELD(OPK_PAD_CIRCLE,     kc_circle,     gc_circle,     "circle"),
        KM_PAD_FIELD(OPK_PAD_SQUARE,     kc_square,     gc_square,     "square"),
        KM_PAD_FIELD(OPK_PAD_TRIANGLE,   kc_triangle,   gc_triangle,   "triangle"),
        KM_PAD_FIELD(OPK_PAD_L1,         kc_l1,         gc_l1,         "l1"),
        KM_PAD_FIELD(OPK_PAD_L2,         kc_l2,         gc_l2,         "l2"),
        KM_PAD_FIELD(OPK_PAD_L3,         kc_l3,         gc_l3,         "l3"),
        KM_PAD_FIELD(OPK_PAD_R1,         kc_r1,         gc_r1,         "r1"),
        KM_PAD_FIELD(OPK_PAD_R2,         kc_r2,         gc_r2,         "r2"),
        KM_PAD_FIELD(OPK_PAD_R3,         kc_r3,         gc_r3,         "r3"),
        KM_PAD_FIELD(OPK_PAD_START,      kc_start,      gc_start,      "start"),
        KM_PAD_FIELD(OPK_PAD_SELECT,     kc_select,     gc_select,     "select"),
        KM_PAD_FIELD(OPK_PAD_DPAD_UP,    kc_dpad_up,    gc_dpad_up,    "dpad_up"),
        KM_PAD_FIELD(OPK_PAD_DPAD_DOWN,  kc_dpad_down,  gc_dpad_down,  "dpad_down"),
        KM_PAD_FIELD(OPK_PAD_DPAD_LEFT,  kc_dpad_left,  gc_dpad_left,  "dpad_left"),
        KM_PAD_FIELD(OPK_PAD_DPAD_RIGHT, kc_dpad_right, gc_dpad_right, "dpad_right"),
        KM_PAD_KEY(OPK_PAD_STICK_UP,     kc_axis_left_up,    "stick_up"),
        KM_PAD_KEY(OPK_PAD_STICK_DOWN,   kc_axis_left_down,  "stick_down"),
        KM_PAD_KEY(OPK_PAD_STICK_LEFT,   kc_axis_left_left,  "stick_left"),
        KM_PAD_KEY(OPK_PAD_STICK_RIGHT,  kc_axis_left_right, "stick_right"),
        KM_PAD_AXIS(OPK_PAD_LEFT_X,  gc_axis_left_x,  "left_x"),
        KM_PAD_AXIS(OPK_PAD_LEFT_Y,  gc_axis_left_y,  "left_y"),
        KM_PAD_AXIS(OPK_PAD_RIGHT_X, gc_axis_right_x, "right_x"),
        KM_PAD_AXIS(OPK_PAD_RIGHT_Y, gc_axis_right_y, "right_y"),
#undef KM_PAD_AXIS
#undef KM_PAD_KEY
#undef KM_PAD_FIELD
    };

    unsigned char* kbd = (unsigned char*)&g_cfg_keyboardMapping;
    unsigned char* pad = (unsigned char*)&g_cfg_controllerMapping;

    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        const km_pad_shape_t shape = openpete_keymap_pad_shape(map[i].action);
        km_source_t src[KM_MAX_SOURCES];
        int n = openpete_keymap_sources(map[i].action, src, KM_MAX_SOURCES);
        int have_kbd = 0, have_pad = 0, ignored = 0;

        for (int s = 0; s < n; s++) {
            const int is_key = src[s].kind == KM_SRC_KEY;
            const int fits = is_key ? shape != KM_PADSHAPE_AXIS_ONLY
                           : shape == KM_PADSHAPE_KEY_AND_PAD ||
                             (shape == KM_PADSHAPE_AXIS_ONLY && src[s].kind == KM_SRC_PADAXIS);
            if (!fits) { ignored++; continue; }
            if (is_key) {
                if (have_kbd++) { ignored++; continue; }
                *(int*)(kbd + map[i].kbd_off) = src[s].code;
                continue;
            }
            if (have_pad++) { ignored++; continue; }
            int code = src[s].code;
            if (src[s].kind == KM_SRC_PADAXIS) {
                code |= CONTROLLER_MAP_FLAG_AXIS;
                if (src[s].invert < 0) code |= CONTROLLER_MAP_FLAG_INVERSE;
            }
            *(int*)(pad + map[i].pad_off) = code;
        }
        /* An unbound half must be cleared, not left at PsyCross's default -
         * otherwise `cross = "pad:south"` would silently keep C bound too.
         * The sentinels are PsyCross's own "NONE" values: scancode 0
         * indexes a key that is never down, and -1 (button or axis) reads
         * 0 in GetControllerButtonState. */
        if (!have_kbd && map[i].kbd_off != SIZE_MAX)
            *(int*)(kbd + map[i].kbd_off) = SDL_SCANCODE_UNKNOWN;
        if (!have_pad && map[i].pad_off != SIZE_MAX)
            *(int*)(pad + map[i].pad_off) = shape == KM_PADSHAPE_AXIS_ONLY
                                              ? (int)SDL_GAMEPAD_AXIS_INVALID
                                              : (int)SDL_GAMEPAD_BUTTON_INVALID;

        if (warn && ignored)
            OPL_WARN_F("keys", "pad.%s: PsyCross holds %s; %d source%s ignored",
                       map[i].name,
                       shape == KM_PADSHAPE_KEY_ONLY ? "one keyboard key here"
                       : shape == KM_PADSHAPE_AXIS_ONLY ? "one gamepad axis here"
                       : "one keyboard and one gamepad binding per button",
                       ignored, ignored > 1 ? "s" : "");
    }

    /* Players 2-4 have working pads without any binding here, but their
     * decode still runs the shared controller merge, so a row bound under
     * [keys.pad2..4] does not yet change what that player's buttons do.
     * Only say so if the user actually bound something there. */
    for (int a = OPK_PAD2_CROSS; warn && a < OPK_ACTION_COUNT; a++) {
        km_source_t src[KM_MAX_SOURCES];
        if (openpete_keymap_sources((openpete_action_t)a, src, KM_MAX_SOURCES)) {
            OPL_WARN_F("keys", "[keys.pad2..4] rows are parsed but do not "
                       "re-map that player yet: players 2-4 decode through "
                       "the shared controller mapping");
            break;
        }
    }
}

/* Documented at the declaration in run_internal.h. */
void pad_restamp_if_due(int on_main) {
    static unsigned s_stamped_main, s_stamped_host;
    unsigned* stamped = on_main ? &s_stamped_main : &s_stamped_host;
    const unsigned gen = openpete_rebind_restamp_generation();
    if (gen == *stamped) return;
    *stamped = gen;
    stamp_psx_pad_bindings(0);
    openpete_rebind_count_restamp(on_main);
    OPL_INFO_F("keys", "pad restamp on %s (generation %u)", on_main ? "main" : "host", gen);
}
#else
/* Without PsyCross there are no pad maps to restamp. */
void pad_restamp_if_due(int on_main) { (void)on_main; }
#endif

/** @brief Give the game window its icon: frame 0 of the save-file header's icon on the disc, else the OpenPete mark.
 *
 * The decode reads the disc through its own handle (openpete_save_icon.h),
 * before the session's disc is attached. The PsyCross window, when one
 * exists, takes the icon at once; the native window, which bring-up adopts
 * from the launcher or creates afterwards, takes it at bring-up
 * (openpete_native_set_icon), so the pixels live in a static buffer.
 * @param opts The parsed options; disc_path names the disc, or is NULL. */
static void hand_over_game_window_icon(const openpete_options_t* opts) {
    static uint8_t s_disc_icon[OPENPETE_SAVE_ICON_WIDTH * OPENPETE_SAVE_ICON_HEIGHT * 4];
    const openpete_game_icon_t* mark = &openpete_mark_icon;
    const uint8_t* rgba = mark->rgba;
    int w = mark->w, h = mark->h;
    if (openpete_save_icon_decode(opts->disc_path, s_disc_icon) == 0) {
        rgba = s_disc_icon;
        w = OPENPETE_SAVE_ICON_WIDTH;
        h = OPENPETE_SAVE_ICON_HEIGHT;
    }
    if (g_window) {
        SDL_Surface* icon = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, (void*)rgba, w * 4);
        if (icon) {
            SDL_SetWindowIcon(g_window, icon);
            SDL_DestroySurface(icon);
        }
    }
    openpete_native_set_icon(rgba, w, h);
}

/** @brief Open the sector image behind --disc as the session's disc and attach the disc cache to it.
 *
 * The path may be the cue or the bin; the resolver covers both and the
 * error names every candidate it tried. With --disc-cache 1, or auto on a
 * filesystem openpete_disc_fs_slow calls slow, the cache starts loading
 * here, before the EXE is read, so every later read of the image (the EXE,
 * PsyCross's libcd and XA, the host paths) goes through it. auto on any
 * other filesystem starts it measuring (openpete_disc_cache_attach_auto),
 * which keeps or frees it. The decision and its reason are logged. A second
 * call changes nothing.
 * @param opts The parsed options; disc_path must be set.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR when no readable
 *         image resolves or disc_image refuses it (a 2048-byte image). */
static int open_game_disc(const openpete_options_t* opts) {
    if (disc_image_game()) return OPENPETE_EXIT_OK;
    char bin[4096], tried[4096];
    if (openpete_psx_resolve_disc_bin(opts->disc_path, bin, sizeof bin,
                                      tried, sizeof tried) != 0) {
        OPL_ERROR_F("boot",
                    "--disc %s names no readable disc image (tried: %s). "
                    "Pass the .cue, or the .bin itself. %s",
                    opts->disc_path, tried[0] ? tried : "(nothing)",
                    openpete_error_ref(OP_E_DISC_READ));
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    disc_image_t* img = disc_image_open(bin, DISC_READ_CACHED, OPENPETE_DISC_TAG_NONE);
    if (!img) return OPENPETE_EXIT_SETUP_ERROR;
    disc_image_set_game(img);
    const char* bin_path = disc_image_bin_path(img);
    if (opts->disc_cache == OPENPETE_DISC_CACHE_AUTO) {
        char why[96];
        const int slow = openpete_disc_fs_slow(bin_path, why, sizeof why);
        OPL_INFO_F("boot", "disc cache: auto -> %s (%s)", slow ? "on" : "measuring", why);
        if (slow) (void)openpete_disc_cache_attach(bin_path);
        else      (void)openpete_disc_cache_attach_auto(bin_path);
    } else if (opts->disc_cache != OPENPETE_DISC_CACHE_OFF) {
        (void)openpete_disc_cache_attach(bin_path);
    }
    return OPENPETE_EXIT_OK;
}

/** @brief Point PsyCross's CD reads at the session's disc image.
 *
 * Opens the image first when open_game_disc has not, installs the trace
 * observer, and binds PsyCross's image to the handle, which reads through
 * the disc cache when it is attached and the file otherwise.
 * @param opts The parsed options; disc_path must be set.
 * @return OPENPETE_EXIT_OK, or OPENPETE_EXIT_SETUP_ERROR when no readable
 *         image resolves or it cannot be bound. */
static int attach_disc_image(const openpete_options_t* opts) {
    int rc = open_game_disc(opts);
    if (rc != OPENPETE_EXIT_OK) return rc;
    openpete_disc_cache_trace_install();
    if (!openpete_disc_psycross_bind(disc_image_game())) {
        OPL_ERROR_F("boot", "--disc %s: the image cannot be bound to PsyCross (larger than 2 GiB) %s",
                    opts->disc_path, openpete_error_ref(OP_E_DISC_READ));
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    OPL_INFO_F("boot", "PsyCross CD image: %s (%s)", disc_image_bin_path(disc_image_game()),
               openpete_disc_cache_active() ? "disc cache" : "file");
    return OPENPETE_EXIT_OK;
}

/* Documented at the declaration in run_internal.h. */
void disc_cache_exit_census(void) {
    uint64_t m; int64_t w, ln; size_t b;
    openpete_disc_cache_census(&m, &w, &b, &ln);
    if (openpete_disc_cache_active()) {
        char tags[256] = "";
        size_t at = 0;
        for (int t = 0; t < OPENPETE_DISC_TAG_COUNT && at < sizeof tags; t++) {
            uint64_t n = openpete_disc_cache_tag_misses(t);
            if (n) at += (size_t)snprintf(tags + at, sizeof tags - at, " %s=%llu",
                                          openpete_disc_trace_tag_name(t), (unsigned long long)n);
        }
        OPL_INFO_F("cd", "disc cache: %zu MB, loaded in %.2f s, %llu miss(es)%s, %.2f ms read from the file",
                   b >> 20, (double)ln / 1e9, (unsigned long long)m, tags, (double)w / 1e6);
    }
    openpete_disc_cache_shutdown();
}

/** @brief Read the PSX EXE from --exe-path, else from the disc image under the game's EXE filename.
 *
 * The disc route first opens the session's disc (open_game_disc), which
 * attaches the disc cache under --disc-cache.
 * @param opts The parsed options; exe_path takes precedence over disc_path.
 * @param out_bytes Receives a malloc'd copy of the EXE (the caller frees
 *        it), or NULL on failure.
 * @param out_size Receives the EXE size in bytes.
 * @return 0 on success, 1 when neither path is set, the disc image cannot
 *         be opened, or the read fails. */
static int try_load_exe(const openpete_options_t* opts,
                        uint8_t** out_bytes, size_t* out_size) {
    *out_bytes = NULL;
    *out_size  = 0;

    if (opts->exe_path) {
        OPL_INFO_F("boot", "loading EXE from --exe-path: %s", opts->exe_path);
        *out_bytes = openpete_psx_read_file(opts->exe_path, out_size);
        return *out_bytes ? 0 : 1;
    }
    if (opts->disc_path) {
        /* The session's disc, and the cache behind it, open before the EXE
         * read so that read goes through the cache too. */
        if (open_game_disc(opts) != OPENPETE_EXIT_OK) return 1;
        OPL_INFO_F("boot", "loading EXE from --disc: %s (%s)",
                   opts->disc_path, game_get_exe_filename());
        *out_bytes = openpete_psx_load_exe_from_disc(
            opts->disc_path, game_get_exe_filename(), out_size);
        return *out_bytes ? 0 : 1;
    }
    return 1;
}

/** @brief Log why PsyCross's GL context cannot be opened, naming the driver and the remedy.
 *
 * Under the offscreen driver the cause is EGL (no libEGL, or none that
 * offers a surfaceless or default display and a config), which SDL's error
 * names; under any other driver it is a display server that cannot be
 * reached.
 * @param opts The parsed options. */
static void log_psyx_init_failure(const openpete_options_t* opts) {
    const char* drv = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (opts->headless && drv && SDL_strcasecmp(drv, "offscreen") == 0) {
        OPL_ERROR_F("boot",
                    "PsyCross could not open its GL context under SDL's offscreen "
                    "video driver (SDL: %s). A headless run that PsyCross rasterizes "
                    "for (the oracle, an -og capture, a headless --record) takes its "
                    "GL context from EGL with no display server: install libEGL "
                    "(Mesa), or set SDL_VIDEODRIVER=x11 with a reachable DISPLAY "
                    "(scripts/lib/xvfb.sh). %s",
                    SDL_GetError(), openpete_error_ref(OP_E_DISPLAY));
        return;
    }
    OPL_ERROR_F("boot",
                "PsyCross init failed under SDL video driver '%s' (SDL: %s). No "
                "display is reachable. Over ssh, export XDG_RUNTIME_DIR and "
                "WAYLAND_DISPLAY (or DISPLAY) from the login session. %s",
                drv ? drv : "(SDL's pick)", SDL_GetError(),
                openpete_error_ref(OP_E_DISPLAY));
}

/** @brief Boot the EXE, dispatch one recompiled function, check its v0, and exit.
 *
 * Runs instead of the host loop when --test-recomp-call or --test-segv is
 * given. Under PsyCross it initialises PsyCross, stamps the pad bindings
 * and attaches the disc image (optional here) first. --test-segv raises
 * SIGSEGV to exercise the crash handler. The entry is --test-recomp-call,
 * else the EXE's entry pc; --test-recomp-gp overrides $gp; --probe logs the
 * word at a PSX address after the call.
 * @param opts The parsed options.
 * @return OPENPETE_EXIT_OK; OPENPETE_EXIT_SETUP_ERROR when no EXE or disc
 *         loads; OPENPETE_EXIT_PANIC on a boot or PsyCross init failure or
 *         an address with no recompiled function; OPENPETE_EXIT_DIVERGENCE
 *         when v0 differs from --test-recomp-expect-v0. */
static int run_recomp_smoke_test(const openpete_options_t* opts) {
    CPUState cpu;
    openpete_psx_runtime_init(&cpu);
    openpete_psx_set_diag_cpu(&cpu);

    uint8_t* bytes = NULL;
    size_t   len   = 0;
    if (try_load_exe(opts, &bytes, &len) != 0) {
        OPL_ERROR_F("boot", "could not load PSX EXE (need --exe-path or --disc) %s",
                    openpete_error_ref(OP_E_DISC_READ));
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    int rc = openpete_psx_boot(&cpu, bytes, len);
    free(bytes);
    if (rc != 0) return OPENPETE_EXIT_PANIC;

#ifdef OPENPETE_HAS_PSYCROSS
    /* libgpu/libgte calls touch internal SDL/GL state that PsyX_Initialise
     * sets up. For the smoke-test path we still want PsyCross alive. */
    OPL_INFO_F("boot", "PsyX_Initialise: opening %dx%d window",
               game_default_window_w(), game_default_window_h());
    int psyx_ok = PsyX_Initialise((char*)opts->display_name,
                                  game_default_window_w(),
                                  game_default_window_h(),
                                  /*fullscreen=*/0,
                                  /*startHidden=*/opts->headless ? 1 : 0);
    /* --headless still opens a GL context here (g_psyx_null_gpu stays 0).
     * headless_gl_probe has already refused a headless run without a usable
     * EGL; an SDL video init failure returns 0 here. Stopping now names the
     * cause; continuing would crash at the first ResetGraph with a null GL
     * proc and no explanation. */
    if (!psyx_ok) {
        log_psyx_init_failure(opts);
        return OPENPETE_EXIT_PANIC;
    }

    /* PsyX_Sys_InitialiseInput just wrote its hardcoded pad maps; replace
     * them with the resolved [keys.pad] bindings. */
    stamp_psx_pad_bindings(1);
    apply_psyx_dither_env(opts);

    /* Wire PsyCross's CDFS to the disc image so CdRead has real data.
     * Without --disc CdRead returns failure, which the leaf smoke test
     * tolerates. */
    if (opts->disc_path) {
        int disc_rc = attach_disc_image(opts);
        if (disc_rc != OPENPETE_EXIT_OK) return disc_rc;
    } else {
        OPL_WARN_F("boot", "no --disc; CdRead will return no data");
    }
#endif

    if (opts->test_recomp_segv) {
        OPL_INFO_F("boot", "deliberately raising SIGSEGV to verify crash handler");
        volatile int* p = (int*)(uintptr_t)0;
        *p = 0;
        return OPENPETE_EXIT_PANIC;
    }

    uint32_t entry = opts->test_recomp_call_addr;
    if (entry == 0) entry = cpu.pc;

    if (opts->test_recomp_gp != 0) {
        OPL_INFO_F("boot", "overriding cpu.gp 0x%08X -> 0x%08X (skipping PSYQ _96_init)",
                   cpu.gp, opts->test_recomp_gp);
        cpu.gp = opts->test_recomp_gp;
    }

    OPL_INFO_F("boot", "dispatching recompiled function at 0x%08X", entry);
    int matched = openpete_recomp_module_get()->dispatch(&cpu, entry);
    if (!matched) {
        OPL_ERROR_F("boot", "no recompiled function at 0x%08X", entry);
        return OPENPETE_EXIT_PANIC;
    }
    OPL_INFO_F("boot", "function returned: v0=0x%08X v1=0x%08X", cpu.v0, cpu.v1);

    /* Optional post-dispatch probe: read 4 bytes at a PSX vaddr and log
     * them. Used as a portable exit-criteria assertion (e.g. Spyro
     * confirms `g_WadHeader.m_LevelEntry[0].m_Offset` is non-zero). */
    if (opts->probe_addr != 0) {
        uint8_t* ram = openpete_psx_get_ram();
        uint32_t v = 0;
        memcpy(&v, ram + (opts->probe_addr & 0x1FFFFFFFu), 4);
        OPL_INFO_F("boot", "probe[0x%08X] = 0x%08X", opts->probe_addr, v);
    }

    if (opts->test_recomp_expected_v0 != 0 &&
        cpu.v0 != opts->test_recomp_expected_v0) {
        OPL_ERROR_F("boot",
                    "v0 mismatch: got=0x%08X expected=0x%08X",
                    cpu.v0, opts->test_recomp_expected_v0);
        return OPENPETE_EXIT_DIVERGENCE;
    }
    fprintf(stdout, "openpete-exit: reason=recomp-smoke-test v0=0x%08X code=0\n",
            cpu.v0);
    return OPENPETE_EXIT_OK;
}

#ifdef OPENPETE_HAS_PSYCROSS
/* Main-loop driver: dispatch the recompiled function at
 * `opts->run_recomp_addr` on a dedicated fiber. PSYQ thunks (psyq_VSync) yield
 * back to the host fiber on VSync(0); the host ends the current PsyCross scene,
 * advances the frame counter, runs exit checks, begins the next scene, and
 * resumes the game. Returns when frame_limit/runtime exits or the dispatched
 * function returns (which `main()` shouldn't, but guards against premature
 * returns from any other entry point we point this at). */

struct run_game g_game;

/** @brief Body of the game fiber: dispatch the game's entry point and park the fiber for good when it returns.
 *
 * Applies the --test-recomp-gp override to cpu.gp first, and gives
 * OPENPETE_CRASH_TEST its chance to fault on the fiber's stack. Sets
 * g_game.dispatch_ok and g_game.done; sets openpete_game_running at entry
 * and clears it before the permanent yield. */
static void game_fiber_entry(void) {
    OP_ROLE_ASSERT(sim);
    SDL_SetAtomicInt(&openpete_game_running, 1);
    openpete_crash_test();
    if (g_game.gp != 0u) {
        OPL_INFO_F("boot",
                   "game fiber: overriding cpu.gp 0x%08X -> 0x%08X "
                   "(skipping PSYQ _96_init)",
                   g_game.cpu.gp, g_game.gp);
        g_game.cpu.gp = g_game.gp;
    }
    OPL_INFO_F("boot", "game fiber: dispatching 0x%08X", g_game.entry);
    g_game.dispatch_ok = openpete_recomp_module_get()->dispatch(&g_game.cpu, g_game.entry);
    if (!g_game.dispatch_ok) {
        OPL_ERROR_F("boot", "game fiber: no compiled body at 0x%08X", g_game.entry);
    } else {
        OPL_INFO_F("boot",
                   "game fiber: dispatched function returned (v0=0x%08X)",
                   g_game.cpu.v0);
    }
    g_game.done = 1;
    SDL_SetAtomicInt(&openpete_game_running, 0);
    /* Permanent yield - never resume. */
    while (1) openpete_fiber_switch(openpete_host_fiber);
}

bool run_game_fiber_create(void) {
    openpete_host_fiber = co_active();
    /* 8 MB fiber stack - Spyro's deepest reasonable call chain is well under
     * 1 MB but PSYQ + recompiler shadow stacks add up; budget generously. */
    openpete_game_fiber = openpete_fiber_create_guarded(8u * 1024u * 1024u, game_fiber_entry);
    return openpete_game_fiber != NULL;
}

/* Live-knob command queue. The dev overlay and the settings menu set the
 * render fps, tick scale, runahead depth, rewind window and mute from the
 * presenting or pump thread; the values they drive (the pacer's render fps
 * and tick scale, the runahead request, the rewind keyframe ring, s_mute and
 * their dirty flags) belong to the host loop. A setter records its
 * request in s_knob_cmds under s_knob_mx and raises s_knob_pending; the host
 * loop drains the requests at the top of its iteration (live_knob_drain), its
 * quiescent point, so every write of those values and every mute gain change
 * happens on one thread. s_knob_shown carries the values the getters report
 * to other threads. */
typedef struct {
    int    fps, fps_set;
    double scale; int scale_set;
    int    ra, ra_set;
    int    rw, rw_set;
    int    mute, mute_set;
} live_knob_cmds_t;
typedef struct { int fps; double scale; int mute; } live_knob_shown_t;
static void            *s_knob_mx;           /* SDL_Mutex, openpete_mutex_once */
static live_knob_cmds_t s_knob_cmds;         /* under s_knob_mx */
static live_knob_shown_t s_knob_shown = { 0, 1.0, 0 };   /* under s_knob_mx */
static SDL_AtomicInt    s_knob_pending;

/** @brief Lock the live-knob queue. */
static void knob_lock(void)   { SDL_LockMutex(openpete_mutex_once(&s_knob_mx)); }
/** @brief Unlock the live-knob queue. */
static void knob_unlock(void) { SDL_UnlockMutex(openpete_mutex_once(&s_knob_mx)); }

void openpete_set_render_fps(int fps) {
    if (fps < 0) fps = 0;
    knob_lock();
    s_knob_cmds.fps = fps; s_knob_cmds.fps_set = 1;
    s_knob_shown.fps = fps;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

/* Live tick-scale (dev overlay). Queued like render-fps: the host loop's
 * pacer re-derives every scaled period (sim, render, tick, slip) at its
 * top-of-iteration quiescent point and re-anchors the deadlines. Pacing
 * only - the sim executes identical ticks, just on a stretched wall clock
 * (headless/unlock ignores the deadlines entirely). */
void openpete_set_tick_scale(double scale) {
    if (scale < OPENPETE_TICK_SCALE_MIN) scale = OPENPETE_TICK_SCALE_MIN;
    if (scale > OPENPETE_TICK_SCALE_MAX) scale = OPENPETE_TICK_SCALE_MAX;
    knob_lock();
    s_knob_cmds.scale = scale; s_knob_cmds.scale_set = 1;
    s_knob_shown.scale = scale;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

/* Documented at the declaration in run_internal.h. */
void tick_scale_schedule_poll(void) {
    static const char* s_next = NULL;
    static int s_init = 0;
    if (!s_init) {
        s_init = 1;
        const char* e = getenv("OPENPETE_TICK_SCALE_AT");
        s_next = (e && *e) ? e : NULL;
    }
    while (s_next) {
        char* end = NULL;
        const long vbl = strtol(s_next, &end, 10);
        if (end == s_next || *end != ':') { s_next = NULL; return; }
        if (SDL_GetAtomicInt(&openpete_vbl_device) < vbl) return;
        const double scale = strtod(end + 1, &end);
        openpete_set_tick_scale(scale);
        OPL_INFO_F("pacer", "tick-scale schedule: VBL %ld -> %.3f", vbl, scale);
        s_next = (*end == ',') ? end + 1 : NULL;
    }
}

double openpete_get_tick_scale(void) {
    knob_lock();
    const double v = s_knob_shown.scale;
    knob_unlock();
    return v;
}

void host_pace_env_build(char* env, unsigned env_bytes, const openpete_options_t* opts,
                         int refresh_mhz, int render_fps, double tick_scale)
{
    openpete_pace_env_build(env, env_bytes,
                            openpete_native_render_gpu_name(),
                            openpete_native_render_driver_version(),
                            refresh_mhz,
                            openpete_native_render_present_mode(),
                            opts->fullscreen ? 1 : 0,
                            render_fps,
                            openpete_interp_toggle_on(),
                            runahead_requested(),
                            tick_scale,
                            openpete_split_applies(opts) ? 1 : 0,
                            openpete_host_input_legacy(),
                            opts->headless ? 1 : 0);
}

void openpete_runahead_set(int n) {
    if (n < 0) n = 0;
    if (n > 3) n = 3;
    knob_lock();
    s_knob_cmds.ra = n; s_knob_cmds.ra_set = 1;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

void openpete_rewind_window_set(int seconds) {
    knob_lock();
    s_knob_cmds.rw = seconds < 0 ? 0 : seconds; s_knob_cmds.rw_set = 1;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

/* Live engine-level mute (dev overlay). Same three gain clamps the boot
 * apply uses; s_mute replaces opts->mute as the baseline the F7 pause
 * exit restores, so a live toggle survives a pause/resume cycle. Host
 * audio only - never gameplay state. (Toggling OFF while paused unmutes
 * the pause silence until resume; harmless dev-tool edge.) */
static int s_mute = 0;
/* Transient mute held while the skip-to-level present fast-forward is up:
 * the logos, title and loading the fast-forward hides also play their audio,
 * so the same gains stay clamped until presents resume. Layered over s_mute
 * (the baseline), never replacing it. */
static int s_skip_mute = 0;

/* The device feed's phases: its hold policy and per-phase counters. Boot is
 * the phase until the host loop's first iteration names another. */
enum { PH_BOOT, PH_SKIP, PH_UNLOCKED, PH_PAUSED, PH_LIVE, PH_HEADLESS };
static const char* const s_feed_phase_names[] = { "boot", "skip", "unlocked", "paused", "live", "headless" };

/** @brief Set up the SPU render's output sinks before any SPU init: the capture ring, the audio dump, the feed's synthetic device, and the exit summary.
 *
 * --record (with audio) arms the capture ring the recorder drains: every
 * frame the SPU renders, the frames the device plays. A --record-cfr
 * recording muxes no audio (its frames are timed in ticks) and leaves the
 * ring unarmed. OPENPETE_AUDIO_DUMP=PATH arms the same ring, drained into a
 * raw s16 stereo 44.1 kHz file every VBL with a PATH.idx VBL axis, so a
 * headless run, or a --record-cfr run, yields the SPU's output on the game's
 * clock; a recorder that muxes audio owns the ring when both are asked for.
 * OPENPETE_AUDIO_FEED_SIM=DRIFT[,SEED[,STALL]] replaces the audio device
 * with the feed's synthetic device, whose clock runs DRIFT (a fraction,
 * e.g. -0.024) off the producer's and which consumes nothing for STALL
 * seconds after the feed first leaves hold, so a headless run measures the
 * feed's underruns and depth. The synthetic device runs on the virtual
 * clock (op_virtual_clock_enable; --record-cfr enables the same clock): the
 * run is paced in scheduled time, the device and the consumer
 * thread advance inside each clock step, and the producer model's delays
 * (seeded by SEED) are clock advances before each VBL, so the counters are
 * independent of host load and thread timing.
 * OPENPETE_AUDIO_FEED_LOG=PATH writes the feed's timeline (holds, primes,
 * underruns, overflows, and its counters every 250 ms of device time).
 * OPENPETE_AUDIO_FEED_TAP=PATH writes every frame the device plays, raw
 * s16 stereo 44.1 kHz.
 * OPENPETE_AUDIO_FEED_TONE=HZ replaces every pushed frame with a sine of
 * HZ in producer time, for the pitch read of tools/audio_tone_pitch.py.
 * @param opts The parsed options. */
static void audio_sinks_setup(const openpete_options_t* opts) {
    const int record_audio = opts->record_path && !opts->record_no_audio && opts->record_cfr <= 0.0;
    PsyX_SPUAL_SetCapture(record_audio);
    const char* dump = getenv("OPENPETE_AUDIO_DUMP");
    if (dump && *dump && record_audio) {
        OPL_WARN_F("audio", "OPENPETE_AUDIO_DUMP ignored: --record owns the capture ring");
    } else if (dump && *dump) {
        extern int openpete_audio_dump_armed;
        openpete_audio_dump_armed = 1;
        PsyX_SPUAL_SetCapture(1);
    }
    const char* feed_log = getenv("OPENPETE_AUDIO_FEED_LOG");
    if (feed_log && *feed_log)
        PsyX_SPUFeed_SetLog(feed_log);
    PsyX_SPUFeed_SetPhase(PH_BOOT, s_feed_phase_names[PH_BOOT], 0);
    const char* sim = getenv("OPENPETE_AUDIO_FEED_SIM");
    if (sim && *sim) {
        const char* comma = strchr(sim, ',');
        const unsigned seed = comma ? (unsigned)strtoul(comma + 1, NULL, 10) : 1u;
        const char* comma2 = comma ? strchr(comma + 1, ',') : NULL;
        const double stall = comma2 ? atof(comma2 + 1) : 0.0;
        PsyX_SPUFeed_Simulate(atof(sim), seed, stall);
        if (!op_virtual_clock_active())
            op_virtual_clock_enable(OP_NS_PER_SEC);
        OPL_INFO_F("audio", "feed: synthetic device on the virtual clock, drift %+.4f seed %u start stall %.3f s",
                   atof(sim), seed, stall);
    }
    const char* tap = getenv("OPENPETE_AUDIO_FEED_TAP");
    if (tap && *tap)
        PsyX_SPUFeed_SetTap(tap);
    const char* tone = getenv("OPENPETE_AUDIO_FEED_TONE");
    if (tone && *tone)
        PsyX_SPUFeed_SetTone(atof(tone));
    atexit(audio_feed_exit_summary);
}

/* Documented at the declaration in run_internal.h. */
void audio_feed_sync(int unlock_fps, int headless, int paused) {
    int ph = PH_LIVE;
    if (headless)                                ph = PH_HEADLESS;
    else if (openpete_fast_boot_active)          ph = PH_BOOT;
    else if (paused)                             ph = PH_PAUSED;
    else if (openpete_skip_present_until_level)  ph = PH_SKIP;
    else if (unlock_fps)                         ph = PH_UNLOCKED;
    PsyX_SPUFeed_SetPhase(ph, s_feed_phase_names[ph], SDL_GetAtomicInt(&openpete_vbl_device));
    PsyX_SPUFeed_SetHold(ph != PH_LIVE && ph != PH_HEADLESS);
}

/* Documented at the declaration in run_internal.h. */
int run_mute_baseline(void) { return s_mute || s_skip_mute; }

/** @brief Apply the combined mute state (baseline or skip-to-level) to every host audio path.
 *
 * With either s_mute or s_skip_mute set it mutes the SPU layer, forces the XA
 * and music-override gains to 0, and mutes the mod one-shot sources (which
 * are outside the SPU mixer); otherwise it clears the SPU mute and releases
 * the forced gains. Host audio only. */
static void apply_mute_gains(void) {
    extern int  PsyX_SPUAL_SetMute(int on_off);
    extern void PsyX_XA_ForceGain(float linear_gain);
    int on = s_mute || s_skip_mute;
    PsyX_SPUAL_SetMute(on);
    PsyX_XA_ForceGain(on ? 0.0f : -1.0f);
    music_override_set_forced_gain(on ? 0.0f : -1.0f);
    /* The SPU mute silences the render's output, host clips included;
     * the mod-sound mute also stops clips in flight and refuses new ones. */
    openpete_mod_sound_set_mute(on);
}

void openpete_set_mute(int on) {
    knob_lock();
    s_knob_cmds.mute = on ? 1 : 0; s_knob_cmds.mute_set = 1;
    s_knob_shown.mute = on ? 1 : 0;
    knob_unlock();
    SDL_SetAtomicInt(&s_knob_pending, 1);
}

void host_live_knobs_publish(int render_fps, double tick_scale)
{
    knob_lock();
    s_knob_shown.fps = render_fps;
    s_knob_shown.scale = tick_scale;
    s_knob_shown.mute = s_mute;
    knob_unlock();
}

/* Documented at the declaration in run_internal.h. */
void live_knob_drain(host_pacer_t* P)
{
    if (!SDL_GetAtomicInt(&s_knob_pending)) return;
    knob_lock();
    const live_knob_cmds_t c = s_knob_cmds;
    memset(&s_knob_cmds, 0, sizeof s_knob_cmds);
    SDL_SetAtomicInt(&s_knob_pending, 0);
    knob_unlock();
    if (c.fps_set) host_pacer_request_fps(P, c.fps);
    if (c.scale_set) host_pacer_request_scale(P, c.scale);
    if (c.ra_set) runahead_request(c.ra);
    if (c.rw_set) openpete_rewind_configure(c.rw);
    if (c.mute_set) { s_mute = c.mute; apply_mute_gains(); }
}

/* Documented at the declaration in run_internal.h. */
void skip_mute_sync(void) {
    int want = openpete_skip_present_until_level ? 1 : 0;
    if (want == s_skip_mute) return;
    s_skip_mute = want;
    apply_mute_gains();
}

int openpete_get_mute(void) {
    knob_lock();
    const int v = s_knob_shown.mute;
    knob_unlock();
    return v;
}

/* --sfx / --music: the requested in-game slider levels, 0..10, or -1 for
 * "leave the game's own slider alone". These are a host-side REQUEST, not a
 * write: the per-game audio-volume feature reads them from its tick hook and
 * stamps the game's volume globals there. Keeping the guest write on the sim's
 * schedule is what lets the dev overlay edit them safely (north-star: nothing
 * outside the tick touches gameplay-readable RAM). */
static int s_sfx_volume   = -1;
static int s_music_volume = -1;

/** @brief Clamp a slider level to 0..10, mapping every negative value to -1 (leave the setting alone). */
static int clamp_volume(int v) {
    if (v < 0)  return -1;
    if (v > 10) return 10;
    return v;
}

void openpete_set_sfx_volume(int v)   { s_sfx_volume   = clamp_volume(v); }
int  openpete_get_sfx_volume(void)    { return s_sfx_volume; }
void openpete_set_music_volume(int v) { s_music_volume = clamp_volume(v); }
int  openpete_get_music_volume(void)  { return s_music_volume; }

/* --vibration: one request feeds the guest enable flag and the motor amplitude. */
void openpete_set_vibration(int v) { openpete_rumble_set_strength(clamp_volume(v)); }
int  openpete_get_vibration(void)  { return openpete_rumble_get_strength(); }

int openpete_get_render_fps(void) {
    knob_lock();
    const int v = s_knob_shown.fps;
    knob_unlock();
    return v;
}

/** @brief Report whether PsyCross runs without a GPU because nothing in the run reads what it rasterizes.
 *
 * PsyCross's raster reaches a consumer through the display under
 * --renderer psycross, the -og screenshot leg, the present-sequence
 * capture (-og per present), and a headless --record, which encodes
 * PsyCross's present FBO because the native display is off under
 * --headless. A windowed --record reads the native offscreen and needs no
 * PsyCross raster. A windowed run with any screenshot request keeps the GL
 * path, --screenshot-native-only included. A headless
 * --screenshot-native-only run reads only the native capture, whose scene
 * is composed from game state, when a headless native consumer is armed
 * (openpete_native_headless_consumer); without one the native renderer
 * stays off and PsyCross keeps the GL path. A windowed run whose launcher found no
 * Vulkan device (opts->native_unavailable) keeps the GL path: PsyCross's
 * GL window is then its display. In every run the CPU VRAM mirror stays
 * live, so StoreImage reads what LoadImage, MoveImage and ClearImage
 * wrote; null-GPU mode stops only the copy-back of the PsyCross
 * framebuffer into the mirror, and a run that rasterizes no OT never
 * stores a framebuffer rect for that copy-back to read.
 * @param opts Resolved launch options.
 * @return true when PsyCross is to open no window, no GL context and no
 *         PSX GPU state. */
static bool psycross_raster_unread(const openpete_options_t* opts) {
    if (opts->renderer != OPENPETE_RENDERER_VK || opts->screenshot_presents_start >= 0
        || opts->native_unavailable)
        return false;
    if (!opts->headless)
        return opts->screenshot_frame_count == 0;
    return opts->screenshot_native_only && !opts->record_path
        && openpete_native_headless_consumer(opts);
}

void openpete_game_window_size(const openpete_options_t* opts, int* out_w, int* out_h)
{
    int w = (opts->window_w > 0) ? opts->window_w : game_default_window_w();
    int h = (opts->window_h > 0) ? opts->window_h : game_default_window_h();
    /* If --aspect is set but the user didn't pin --window, scale the
     * default window so the *cropped* (post-overscan) src exactly fills
     * it - otherwise the present blit letterboxes the un-rendered
     * disp/clip difference. The cropped aspect = requested_aspect ×
     * game_display_crop_factor(), the game's own disp_h/clip_h. The
     * PsyCross reference renderer has no Hor+ path and draws canonical 4:3
     * only, so a widened window would stretch it; native modes keep
     * --aspect (window-driven Hor+). An explicit --window always wins. */
    if (opts->aspect_w_over_h > 0.0f && opts->renderer != OPENPETE_RENDERER_PSYCROSS &&
        opts->window_w <= 0 && opts->window_h <= 0) {
        h = game_default_window_h();
        w = (int)((float)h * opts->aspect_w_over_h * game_display_crop_factor() + 0.5f);
    }
    *out_w = w;
    *out_h = h;
}

/** @brief Seed the diagnostic toasts that OPENPETE_OSD_DEMO and OPENPETE_OSD_DEMO_TEXT ask for.
 *
 * The OSD is otherwise driven entirely by live user action - a menu row, a
 * hotkey, a mod - which a headless or scripted capture cannot perform, so a
 * gate that wants to SEE a toast has no other way to make one exist.
 * OPENPETE_OSD_DEMO=N posts the first N of eight samples of different
 * lengths: the box is content-sized, and equal-length samples would never
 * exercise that. OPENPETE_OSD_DEMO_TEXT posts further toasts with the given
 * texts, separated by '|', after the samples: the wrap and the ellipsis need
 * texts longer than any sample. Each toast takes the maximum timeout, so the
 * stack is still up whenever the capture lands, and its own rate-limit
 * bucket: the per-source limit exists to stop a runaway producer, and it
 * would otherwise cap the seed at the burst of 4. Diagnostic only; unset in
 * normal play. */
static void osd_demo_seed(void) {
    static const char *const demo[] = {
        "VSYNC: ON", "LOD: EXTENDED", "SSAA: 4X",
        "DISPLAY: FULLSCREEN", "FPS: UNLIMITED", "TEXTURES: AFFINE",
        "DITHER: 15 BIT", "FREECAM: ON",
    };
    const char *e = getenv("OPENPETE_OSD_DEMO");
    int demo_n = (e && *e) ? atoi(e) : 0;
    if (demo_n > (int)(sizeof demo / sizeof demo[0]))
        demo_n = (int)(sizeof demo / sizeof demo[0]);
    char src[24];
    for (int i = 0; i < demo_n; i++) {
        snprintf(src, sizeof src, "demo%d", i);
        openpete_osd_notify(src, NULL, OPENPETE_OSD_TIMEOUT_MAX_MS, "%s", demo[i]);
    }
    const char *t = getenv("OPENPETE_OSD_DEMO_TEXT");
    for (int i = 0; t && *t; i++) {
        const char *bar = strchr(t, '|');
        const int len = bar ? (int)(bar - t) : (int)strlen(t);
        snprintf(src, sizeof src, "demotext%d", i);
        openpete_osd_notify(src, NULL, OPENPETE_OSD_TIMEOUT_MAX_MS, "%.*s", len, t);
        t = bar ? bar + 1 : NULL;
    }
}

/** @brief Bring up the host side of a run: guest CPU and PSX runtime, knobs, mods, PsyCross, audio, input, windows and the native renderer.
 *
 * Runs before the host loop, on the thread that will pump SDL events (main
 * under the thread split, else the single thread). In order it initialises
 * the guest CPU and PSX runtime, seeds the CLI knobs (volumes, vibration,
 * mouse, input lock), boots the EXE and applies --seed-cpu / --seed-ram,
 * applies feature-module knobs and loads mods, resolves the window size
 * from --aspect, sets up PsyCross (null-GPU when nothing reads its raster),
 * input maps and icons, arms --record, brings up the native Vulkan renderer
 * for --renderer vk (a windowed run left with no renderer stops with a
 * message to the player), arms the pad-read probe, halt-at-pc and the psxtrace
 * verifier, configures the memory card, and starts the VBL ticker unless
 * headless without OPENPETE_FORCE_PACING.
 * @param opts Resolved launch options.
 * @return 0 on success, else an OPENPETE_EXIT_* code for the caller to
 *         return. */
static int run_host_init(const openpete_options_t* opts) {
    memset(&g_game, 0, sizeof(g_game));
    openpete_psx_runtime_init(&g_game.cpu);
    openpete_psx_set_diag_cpu(&g_game.cpu);

    /* --sfx / --music: seed the slider presets the per-game audio-volume
     * feature stamps into the game's own volume globals (-1 = leave the
     * game's slider alone, the default). Seeded here rather than beside the
     * --mute apply below because the knob registry stamps its boot values
     * from the live getters during renderer init, which happens first. */
    openpete_set_sfx_volume(opts->sfx_volume);
    openpete_set_music_volume(opts->music_volume);

    /* --vibration: the host-side strength (0..10, -1 = unset, read as 10), plus the
     * context gates. Headless is a refusal rather than a default - a run with
     * no window has no player, and every gate battery, capture and
     * scripted launch is one.
     *
     * Installing the actuator-registration hook is what moves the per-vsync
     * half of libpad's PadSetAct contract onto the host; left uninstalled
     * (the OPENPETE_INPUT_LEGACY A/B) PsyCross keeps its historical one-shot
     * behaviour, which is the "before" side of the vibration A/B. */
    openpete_set_vibration(opts->vibration);
    openpete_rumble_set_headless(opts->headless ? 1 : 0);

    /* --mouse: the host mouse channel switch, seeded before the knob registry
     * stamps its boot values from the live getters. -1 (unset) is on. */
    openpete_host_mouse_set_enabled(opts->mouse != 0);
    /* --input-lock: seeded here for the same reason. -1 (unset) is off. */
    openpete_input_lock_set(opts->input_lock > 0);
    /* --pad-unfocused: seeded here for the same reason; -1 (unset) is off. */
    openpete_host_input_set_pad_unfocused(opts->pad_unfocused > 0);
    /* --stick-response: seeded here for the same reason; -1 (unset) is console. */
    openpete_stick_response_set(opts->stick_response);
    if (opts->input_lock > 0)
        OPL_INFO_F("input", "input lock on: keyboard, mouse and gamepad "
                   "devices are ignored");
    {
        /* OPENPETE_RUMBLE_LEGACY=1 leaves the hook uninstalled, so PadSetAct
         * keeps PsyCross's historical one-shot behaviour. That is the "before"
         * side of the vibration A/B, and it is deliberately its OWN knob
         * rather than a lean on OPENPETE_INPUT_LEGACY: the input-legacy path
         * also moves pumping and decode back to PsyCross, which changes
         * several things at once. An A/B that moves more than one variable
         * cannot attribute what it measures. */
        const char* e = getenv("OPENPETE_RUMBLE_LEGACY");
        const int rumble_legacy = (e && *e && *e != '0');
        if (!openpete_host_input_legacy() && !rumble_legacy)
            g_PsyX_Pad_ActuatorRegister = openpete_rumble_register;
        else
            OPL_INFO_F("rumble", "actuator registration hook NOT installed "
                                 "(%s) - PadSetAct keeps its one-shot behaviour",
                       rumble_legacy ? "OPENPETE_RUMBLE_LEGACY"
                                     : "OPENPETE_INPUT_LEGACY");
    }

    uint8_t* bytes = NULL;
    size_t   len   = 0;
    if (try_load_exe(opts, &bytes, &len) != 0) {
        OPL_ERROR_F("boot", "could not load PSX EXE (need --exe-path or --disc) %s",
                    openpete_error_ref(OP_E_DISC_READ));
        openpete_fatal_message(OP_E_DISC_READ, "OpenPete could not start",
                               "The game's program could not be read from the disc image.");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    int rc = openpete_psx_boot(&g_game.cpu, bytes, len);
    free(bytes);
    if (rc != 0) return OPENPETE_EXIT_PANIC;

    /* seed-cpu / seed-ram block. Both run AFTER openpete_psx_boot so the
     * seeds win over the EXE-loader's writes. seed-cpu also short-circuits
     * the --test-recomp-gp override below - if the user asked us to seed
     * the full register set, we do not want a stale --test-recomp-gp value
     * clobbering the seeded gp inside game_fiber_entry. */
    if (opts->seed_cpu_path) {
        if (load_cpu_seed(&g_game.cpu, opts->seed_cpu_path) != 0) {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        OPL_INFO_F("seed-cpu",
                   "primed from %s: pc=0x%08X gp=0x%08X sp=0x%08X ra=0x%08X",
                   opts->seed_cpu_path,
                   g_game.cpu.pc, g_game.cpu.gp,
                   g_game.cpu.sp, g_game.cpu.ra);
    }
    if (opts->seed_ram_path) {
        FILE* fp = fopen(opts->seed_ram_path, "rb");
        if (!fp) {
            OPL_ERROR_F("seed-ram", "fopen %s failed: %s",
                        opts->seed_ram_path, strerror(errno));
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        const size_t ram_size = 2u * 1024u * 1024u;
        size_t n = fread(openpete_psx_get_ram(), 1, ram_size, fp);
        fclose(fp);
        if (n != ram_size) {
            OPL_ERROR_F("seed-ram",
                        "short read from %s: %zu of %zu bytes",
                        opts->seed_ram_path, n, ram_size);
            return OPENPETE_EXIT_SETUP_ERROR;
        }
        OPL_INFO_F("seed-ram", "g_ram primed from %s (%zu bytes)",
                   opts->seed_ram_path, ram_size);
    }

    /* Feature modules apply their CLI-derived knobs; a game with no
     * feature list in its descriptor makes this a no-op. */
    feature_registry_apply_options(opts);

    /* Modding framework: discover, build and load mods. No mods directory
     * means zero work and a byte-identical modless run; a broken mod is
     * disabled with a log, never a failed launch. */
    openpete_mods_init(opts);
    openpete_boot_mark(OP_BOOT_MODS_INIT);

    int win_w, win_h;
    openpete_game_window_size(opts, &win_w, &win_h);
    openpete_knob_default_window_set(game_default_window_w(), game_default_window_h());
    if (opts->aspect_w_over_h > 0.0f &&
        opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        OPL_WARN_F("aspect", "--aspect is native-renderer-only (the PsyCross "
                   "Hor+ substitution stack was retired); the PsyCross "
                   "reference renders canonical 4:3 - flag ignored");
    } else if (opts->aspect_w_over_h > 0.0f &&
        opts->window_w <= 0 && opts->window_h <= 0) {
        OPL_INFO_F("aspect", "auto window size %dx%d (--aspect=%.4f × crop %.4f)",
                   win_w, win_h, (double)opts->aspect_w_over_h,
                   (double)game_display_crop_factor());
    }

    /* --fov and --aspect are native-renderer display policies: every
     * renderer mode runs the game's verdict producers pure canonical 4:3,
     * the native feeders project through the effective H and widen Hor+
     * from the window aspect (core/renderer/native_render.c). PsyCross
     * reference mode has neither. */
    if (opts->renderer == OPENPETE_RENDERER_PSYCROSS) {
        if (opts->fov_deg > 0.0f)
            OPL_WARN_F("fov", "--fov is a native-renderer policy; inert under "
                       "--renderer psycross");
    } else {
        if (opts->fov_deg > 0.0f)
            openpete_native_set_fov(opts->fov_deg);
        if (opts->aspect_w_over_h > 0.0f) {
            OPL_INFO_F("aspect", "--aspect %.4f sizes the window; the native "
                       "renderer widens Hor+ at the display layer from "
                       "canonical 4:3 producers",
                       (double)opts->aspect_w_over_h);
        }
    }

    /* --no-lod: native-renderer display policy (sets openpete_no_lod and
     * resolves the fog scale; warns and is ignored in PsyCross reference
     * mode, which has no extended-LOD producer). */
    if (no_lod_apply_options(opts) != 0) {
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    audio_sinks_setup(opts);

    /* --psycross-native-raster: size the reference renderer's present FBO to
     * the console display grid instead of the window. Read per scene begin, so
     * setting it before init is enough. */
    extern int g_psyx_present_native_raster;
    g_psyx_present_native_raster = opts->psycross_native_raster ? 1 : 0;

    /* PsyCross opens a GPU exactly when something in the run reads what it
     * rasterizes (psycross_raster_unread names the readers). Otherwise it
     * runs as a library in null-GPU mode: SDL, the vblank/interrupt core,
     * input, SPU and CDFS come up; no window, GL context or PSX GPU state is
     * created, and every GL-touching GR entry point returns early, so the
     * libgpu thunks pay no texture upload or framebuffer readback. The
     * oracle and the determinism runs (headless, no --screenshot-native-only)
     * keep the GL path: PsyCross rasterizes every OT there. */
    extern int g_psyx_null_gpu;
    g_psyx_null_gpu = psycross_raster_unread(opts) ? 1 : 0;
    OPL_INFO_F("boot", "PsyX_Initialise: %s (headless=%d)",
               g_psyx_null_gpu ? "null-GPU (library-only: no window/GL)"
                               : "opening window",
               (int)opts->headless);
    /* The dev overlay exists only in the native-vk window path, so a run
     * without one has nothing for --devui-open to open. Saying so beats a
     * flag that silently does nothing. */
    if (openpete_devui_open_requested(opts)
        && (opts->headless || opts->renderer != OPENPETE_RENDERER_VK))
        OPL_INFO_F("devui", "%s", "--devui-open is inert in this run: the "
                   "dev config overlay exists only in a windowed native-vk "
                   "session");
    /* A headless run that keeps the GL path takes its context from the
     * driver headless_select_video_driver chose before dispatch: SDL's
     * offscreen driver (surfaceless EGL, no display server) on Linux, the
     * platform driver on a hidden window on Windows. Every run creates this
     * window unmapped (startHidden) so it never flashes or steals focus;
     * GL/FBO rendering works without it ever being shown. Null-GPU mode
     * creates no window or context. */
    /* PsyCross renders to its FBO only; the native Vulkan window is the
     * display in every mode. What reaches the screen is either the native
     * scene (--renderer vk) or PsyCross's read-back frame (--renderer
     * psycross), and either way this GL window is not it. Created unmapped
     * and shown again below only if native bring-up fails. */
    int psyx_ok = PsyX_Initialise((char*)opts->display_name, win_w, win_h,
                                  /*fullscreen=*/0,
                                  /*startHidden=*/1);
    /* PsyX_Initialise runs SDL_INIT_VIDEO in every mode, and a headless run
     * that keeps the GL path (the oracle: PsyCross rasterizes every OT) opens
     * a GL context. headless_gl_probe has already refused a headless run
     * without a usable EGL; an SDL video init failure (no display reachable
     * by a windowed run) returns 0 here. Stopping now names the cause;
     * continuing would crash at the first ResetGraph with a null GL proc and
     * no explanation. */
    if (!psyx_ok) {
        log_psyx_init_failure(opts);
        openpete_fatal_message(OP_E_DISPLAY, "OpenPete could not start graphics",
                               "No display could be opened.");
        return OPENPETE_EXIT_PANIC;
    }

    /* PsyX_Sys_InitialiseInput just wrote its hardcoded pad maps; replace
     * them with the resolved [keys.pad] bindings. */
    stamp_psx_pad_bindings(1);
    apply_psyx_dither_env(opts);

    /* The gamepad subsystem is up now, which is the earliest point a mapping
     * can be added and the latest one at which it still reaches a pad that
     * was already attached. */
    openpete_host_input_load_mappings(opts->controller_db);
    openpete_host_input_drop_unread_events();

    /* The game window's icon: the save-file header's icon decoded from the
     * player's disc, else the OpenPete mark. A headless run shows no window
     * and reads nothing for it. */
    if (!opts->headless) hand_over_game_window_icon(opts);

    if (opts->headless && g_window) {
        /* Window was created unmapped (PsyX_Initialise startHidden=1) - no
         * hide needed here; just record the driver SDL picked. */
        OPL_INFO_F("boot", "headless: PsyCross window created hidden (driver=%s)",
                   SDL_GetCurrentVideoDriver());
    }

    /* --record: arm gameplay video capture. The encoder is built lazily on
     * the first captured frame (once the present FBO's native dimensions are
     * known); this only stashes the parameters. Capture fires before
     * PsyX_EndScene, reading the present FBO directly. */
    if (openpete_record_init(opts) != 0)
        return OPENPETE_EXIT_SETUP_ERROR;

    /* --renderer vk: bring up the native Vulkan renderer's window. SDL
     * is already initialized by PsyX_Initialise. Pass the same resolved window
     * size handed to PsyCross so the native window matches the game's. A
     * bring-up failure is non-fatal - we log and run PsyCross-only. The
     * native path is a pure display consumer; it never touches PSX RAM, so
     * the oracle gate / determinism are unaffected. */
    /* Create the window before bring-up so it belongs to this thread - the
     * one that pumps. Bring-up adopts it exactly as it adopts the splash's. */
    openpete_native_precreate_window(opts, win_w, win_h);
    rhi_set_clock(op_now);   /* boot facts on the pace log's clock */
    (void)openpete_native_render_init(opts, win_w, win_h);
    openpete_native_render_arm_capture(opts);   /* --record-cfr: presents never shed */
    /* Bring-up failed in a windowed run: PsyCross's own GL window is the only
     * display left, so map the window that was created hidden above. Without
     * this the run would present to nothing and look like a hang. */
    if (!opts->headless && !openpete_native_render_enabled() && g_window) {
        SDL_ShowWindow(g_window);
        OPL_WARN_F("boot", "%s", "native display unavailable - showing "
                   "PsyCross's own GL window instead");
    }
    openpete_native_set_perf_stats(opts->perf_stats);   /* stats overlay level (O / M menus move it live) */
    openpete_native_set_perf_anchor(opts->perf_position);  /* which corner it anchors to (--perf-position) */
    openpete_osd_set_enabled(opts->osd);
    openpete_osd_set_anchor(opts->osd_position);
    openpete_osd_set_default_timeout_ms(opts->osd_timeout_ms);
    openpete_native_set_version_osd(opts->version_osd);   /* build-version line (--version-osd) */
    if (opts->mouse_sensitivity >= 0.0f)
        openpete_freecam_set_mouse_sensitivity(opts->mouse_sensitivity);
    osd_demo_seed();
    /* OPENPETE_MENU_DEMO=1 opens the settings menu at startup, the menu's
     * counterpart of the toast seed above: an unattended run has no key to
     * open it with. The open menu holds the game pad released, so the run
     * needs no pad input of its own (--skip-to-level, not a movie).
     * Diagnostic only. */
    {
        const char *e = getenv("OPENPETE_MENU_DEMO");
        if (e && *e && *e != '0' && !openpete_menu_is_open()) openpete_menu_toggle();
    }
    openpete_native_set_fps_in_record(opts->record_fps_overlay);  /* --record-fps-overlay */

    /* Decide whether PsyCross still rasterizes the OT (see the global's comment).
     * Only a pure, successfully-brought-up native vk display with no capture
     * consumer lets us skip the redundant GL submission. Everything else
     * (headless, PsyCross mode, native bring-up failure, --screenshot) keeps
     * PsyCross rendering. NOTE: --record does NOT force it on for the native display - the
     * native --record path reads the native offscreen directly (rhi_read_offscreen,
     * self-synchronizing), so forcing PsyCross to ALSO render every frame just
     * doubled the GPU work and throttled the capture to ~8 fps. (Headless --record
     * still uses PsyCross - native is disabled there, so the first term is true.) */
    openpete_psycross_renders =
        (!openpete_native_render_enabled()                 /* headless / psycross / bring-up failed */
         /* PsyCross mode: PsyCross IS the rasterizer, so it always renders.
          * Redundant with the term above only while PsyCross mode creates no
          * native device - keep it stated in its own right, because it is the
          * whole rasterization decision the moment that changes. */
         || opts->renderer == OPENPETE_RENDERER_PSYCROSS
         /* --screenshot-native-only discards the PsyCross reference, so don't pay
          * the GL OT walk to build it - the native -vk dump reads gamestate. */
         || (opts->screenshot_frame_count > 0 && !opts->screenshot_native_only) /* -og leg */
         || opts->screenshot_presents_start >= 0)          /* present-sequence capture (-og leg) */
        /* null-GPU hard override: no GL context exists, so PsyCross MUST NOT
         * rasterize regardless of the terms above (the reachable case is a
         * native bring-up failure in a windowed pure-vk run, which stops
         * below instead of reaching a NULL GL proc). */
        && !g_psyx_null_gpu;
    /* PsyCross skipped its GL init for this run, so a failed native bring-up
     * leaves no renderer: the game would run with sound and no picture. */
    if (g_psyx_null_gpu && !openpete_native_render_enabled()) {
        OPL_ERROR_F("boot", "%s", "native renderer unavailable and PsyCross is "
                    "in null-GPU mode: no display path, stopping");
        openpete_fatal_message(OP_E_VULKAN, "OpenPete could not start graphics",
            "No usable Vulkan graphics device was found.\n\n"
            "Install the latest driver for your graphics card.\n"
            "To run without Vulkan, add renderer = \"psycross\" to openpete.toml.");
        return OPENPETE_EXIT_SETUP_ERROR;
    }
    OPL_INFO_F("boot", "PsyCross OT rasterization: %s (renderer=%d native=%d)",
               openpete_psycross_renders ? "ON" : "OFF (the native scene is the picture)",
               (int)opts->renderer, (int)openpete_native_render_enabled());

    signal(SIGINT,  openpete_on_sigint);
#ifdef SIGTERM
    signal(SIGTERM, openpete_on_sigint);
#endif
#ifndef _WIN32
    signal(SIGALRM, openpete_on_quit_backstop);
#endif

    if (opts->disc_path) {
        int disc_rc = attach_disc_image(opts);
        if (disc_rc != OPENPETE_EXIT_OK) return disc_rc;
    }

    /* The header's entry is the crt0, which sets sp, fp and gp, stores the
     * BIOS return address and calls main; an explicit OPENPETE_RUN_RECOMP bypasses
     * it and is a diagnostic, since main entered directly runs 8 bytes lower
     * on the stack with the heap words unwritten. */
    g_game.entry = opts->run_recomp_addr ? opts->run_recomp_addr : g_game.cpu.pc;
    if (g_game.entry != g_game.cpu.pc) {
        OPL_WARN_F("boot", "OPENPETE_RUN_RECOMP 0x%08X bypasses the EXE entry 0x%08X: "
                   "stack, heap words and gp differ from hardware",
                   g_game.entry, g_game.cpu.pc);
    }
    /* When --seed-cpu is supplied, suppress the --test-recomp-gp override
     * so it can't clobber the freshly-seeded gp inside game_fiber_entry. */
    g_game.gp    = opts->seed_cpu_path ? 0u : opts->test_recomp_gp;

    /* Pad-read probe: arm from the per-game address before the first
     * dispatch, so tick zero's read is seen like every other. */
    {
        extern uint32_t openpete_pad_read_pc;
        /* OPENPETE_PAD_READ_PC overrides the per-game address. It is how the
         * probe is falsified - pointed at an address the game never calls,
         * the census must report NEVER - and how a second game's read site is
         * found before it is written down. */
        const char* pad_pc_env = getenv("OPENPETE_PAD_READ_PC");
        openpete_pad_read_pc = (pad_pc_env && *pad_pc_env)
            ? (uint32_t)strtoul(pad_pc_env, NULL, 0)
            : game_get_pad_read_pc();
        if (openpete_pad_read_pc) {
            openpete_pace_pad_probe_arm();
            OPL_DEBUG_F("pacing", "pad-read probe armed at 0x%08X",
                        openpete_pad_read_pc);
        }
    }

    /* Wire halt-at-pc before dispatch so psx_override_dispatch sees it on
     * the very first recompiled function-entry. dispatch.c owns the slot. */
    if (opts->halt_at_pc != 0u) {
        extern uint32_t    openpete_halt_at_pc;
        extern uint32_t    openpete_halt_hit_target;
        extern const char* openpete_halt_dump_path;
        extern const char* openpete_halt_cpu_dump_path;
        openpete_halt_at_pc         = opts->halt_at_pc;
        openpete_halt_hit_target    = opts->halt_hit_target ? opts->halt_hit_target : 1u;
        openpete_halt_dump_path     = opts->ram_dump_output;
        openpete_halt_cpu_dump_path = opts->halt_cpu_dump_output;
        OPL_INFO_F("halt-at-pc",
                   "armed: PC=0x%08X hit=%u dump=%s%s%s",
                   opts->halt_at_pc, openpete_halt_hit_target,
                   opts->ram_dump_output,
                   opts->halt_cpu_dump_output ? " cpu=" : "",
                   opts->halt_cpu_dump_output ? opts->halt_cpu_dump_output : "");
    }

    /* PCSX-Redux oracle verifier: open the trace before the game loop and
     * register the dispatch hook so advance() ticks at every dispatch of
     * the trace's anchor PC (read from the trace itself unless the user
     * overrides via --psxtrace-anchor-pc), matching the BP
     * scripts/pcsx-redux-trace.lua used to record the trace. */
    static openpete_psxtrace_t s_psxtrace;
    if (opts->psxtrace_verify_path) {
        if (openpete_psxtrace_open(&s_psxtrace,
                                   opts->psxtrace_verify_path) == 0) {
            psxtrace = &s_psxtrace;
            s_psxtrace_active = &s_psxtrace;
            extern uint32_t openpete_psxtrace_anchor_pc;
            extern uint32_t openpete_psxtrace_anchor_hit_target;
            extern void (*openpete_psxtrace_dispatch_hook)(uint32_t);
            openpete_psxtrace_anchor_pc =
                opts->psxtrace_anchor_pc ? opts->psxtrace_anchor_pc
                                         : s_psxtrace.inferred_anchor_pc;
            if (openpete_psxtrace_anchor_pc == 0u) {
                OPL_ERROR_F("psxtrace",
                            "no anchor PC: trace had no first record pc, "
                            "pass --psxtrace-anchor-pc HEX");
                return OPENPETE_EXIT_SETUP_ERROR;
            }
            openpete_psxtrace_anchor_hit_target =
                opts->psxtrace_anchor_hit ? opts->psxtrace_anchor_hit : 1u;
            openpete_psxtrace_dispatch_hook = openpete_psxtrace_on_anchor;
            OPL_INFO_F("psxtrace",
                       "anchor armed: PC=0x%08X hit=%u",
                       openpete_psxtrace_anchor_pc,
                       openpete_psxtrace_anchor_hit_target);
        } else {
            return OPENPETE_EXIT_SETUP_ERROR;
        }
    } else if (opts->psxtrace_anchor_pc) {
        /* No trace: the anchor still counts hits, for the CD read log's
         * hit stamps and OPENPETE_ANCHOR_VBL_LOG. */
        extern uint32_t openpete_psxtrace_anchor_pc;
        openpete_psxtrace_anchor_pc = opts->psxtrace_anchor_pc;
        OPL_INFO_F("psxtrace", "anchor counting: PC=0x%08X (no trace)",
                   openpete_psxtrace_anchor_pc);
    }

    /* Engine-level mute: clamp the SPU AL sources and the XA stream's AL
     * gain. Bypasses the game's volume sliders entirely. The music WAV
     * override mirrors the XA forced gain so --mute covers it too.
     * Routed through the live setter (dev overlay toggles it too). */
    music_override_init();  /* logs the assets/music/ search path */
    sfx_pack_set_dir_override(opts->sfxpack_dir);
    sfx_pack_init();        /* assets/sfx/ drop-in replacement */
    /* Boot: written before the host loop starts, so no other thread reads it. */
    s_mute = opts->mute;
    apply_mute_gains();
    host_live_knobs_publish(0, 1.0);   /* no pacer yet: the unseeded defaults */

    /* Host-backed memory card: apply CLI/env config before the game's
     * MemCardInit thunk fires (during the game's Initialize()). */
    openpete_memcard_set_dir_override(opts->card_dir);
    openpete_memcard_set_paths(opts->card1_path, opts->card2_path);
    openpete_memcard_set_presence(0, opts->card_a_present);
    openpete_memcard_set_presence(1, opts->card_b_present);
    openpete_memcard_set_unformatted(opts->card_unformatted);
    openpete_memcard_set_adopt(opts->adopt_saves);
    openpete_memcard_set_mismatch_toast(opts->save_mismatch_toast);
    const int card_rc = openpete_memcard_startup_check();
    if (card_rc != OPENPETE_EXIT_OK) return card_rc;
    openpete_handoff_init(opts->resume_handoff, opts->handoff_argc,
                          opts->handoff_argv);
    if (openpete_bugreport_replay_admit(opts) != 0) return OPENPETE_BUGREPORT_REPLAY_REFUSED;
    /* Savestate slots every game shared are copied into this game's own
     * folder; an overlay ini an earlier layout kept beside the executable
     * is copied to the state directory's openpete-devui.ini. */
    (void)openpete_state_seed_game_dir();
    (void)openpete_devui_seed_ini();

    /* Skip fast-forward is windowed-only - headless is already unlocked and
     * has nothing to show. Published before the setter below so the boot
     * CLI skip (and every later runtime warp) arms correctly. */
    openpete_skip_ffwd_windowed = opts->headless ? 0 : 1;

    /* Forward CLI overrides to the per-game impl. game_on_frame_pre
     * applies them when the relevant in-game globals are ready. The setter
     * itself arms the present fast-forward
     * (openpete_skip_present_until_level). */
    game_set_skip_to_level(opts->skip_to_level);
    game_set_skip_to_cutscene(opts->skip_to_cutscene);
    game_set_skip_to_area(opts->skip_to_area_level, opts->skip_to_area);
    skip_mute_sync();

    /* Fast-boot gate is on by default (see fiber.c). psyq_DrawOTag clears it
     * the first time real draw commands hit, so host yields engage exactly
     * when the game starts rendering - no CLI knob needed. */
    OPL_INFO_F("boot", "fast-boot: skipping host yields until first DrawOTag");

    /* Start the free-running vblank ticker. Skipped under --headless to keep
     * that path deterministic: psyq_VSync advances `openpete_vbl`
     * synchronously when no ticker is running. */
    {
        /* The pacing measurement harness needs the ticker active even under
         * --headless to capture a paced baseline; OPENPETE_FORCE_PACING=1
         * opts in. */
        const char* fp = getenv("OPENPETE_FORCE_PACING");
        bool force_pacing = (fp && *fp && *fp != '0');
        if (!opts->headless || force_pacing) {
            openpete_ticker_start();
        }
    }

    /* The mouse channel: event-derived state, totals, the injection seam.
     * Headless keeps PsyCross's hidden window, so the channel initialises
     * there too; the native window, when there is one, is found lazily. */
    openpete_host_mouse_init(g_window);

    return 0;
}

/* Documented at the declaration in run_internal.h. */
void rumble_census_report(void) {
    /* Vibration census. Emitted unconditionally because the numbers only mean
     * something together, and every one of them is a zero in some legitimate
     * configuration:
     *
     *   registered/nonzero  the diagnosis. libpad's PadSetAct is a
     *                       registration, so the expected reading is a small
     *                       number of calls all carrying {0,0} - a NON-zero
     *                       registration would mean the game got its buffer
     *                       filled before handing it over, and the one-shot
     *                       reading of PadSetAct was not the whole story.
     *   commands            SDL rumble calls issued. Zero is correct headless,
     *                       unfocused, at --vibration 0, or with no pad.
     *   recorder            the virtual-pad oracle: `live` says the self-probe
     *                       arrived, and without that a zero motor count is
     *                       indistinguishable from a recorder that was never
     *                       wired. Never read the counts without it. */
    {
        uint16_t low = 0, high = 0;
        openpete_rumble_last_pair(&low, &high);
        OPL_INFO_F("rumble",
                   "census: PadSetAct %u call(s), %u carrying non-zero bytes; "
                   "%u motor command(s) issued, last pair {low %u, high %u}",
                   openpete_rumble_register_calls(),
                   openpete_rumble_register_nonzero(),
                   openpete_rumble_commands(), (unsigned)low, (unsigned)high);
        /* Per virtual pad, because "which device received it" is the claim a
         * per-slot actuator path has to answer and a total cannot. */
        for (int v = 0; v < OPENPETE_PAD_SLOTS; v++) {
            if (!openpete_host_input_vpad_rumble_calls(v) &&
                !openpete_host_input_vpad_rumble_live(v))
                continue;
            unsigned vlow = 0, vhigh = 0;
            openpete_host_input_vpad_rumble_last(v, &vlow, &vhigh);
            OPL_INFO_F("rumble",
                       "recorder: pad %d live=%d, %d callback(s), %d non-zero, "
                       "last pair {low %u, high %u}", v,
                       openpete_host_input_vpad_rumble_live(v),
                       openpete_host_input_vpad_rumble_calls(v),
                       openpete_host_input_vpad_rumble_nonzero(v),
                       vlow, vhigh);
        }
        for (int sl = 1; sl < OPENPETE_PAD_SLOTS; sl++) {
            if (!openpete_rumble_mod_commands(sl)) continue;
            uint16_t mlow = 0, mhigh = 0;
            openpete_rumble_mod_pair(sl, &mlow, &mhigh);
            OPL_INFO_F("rumble",
                       "mod slot %d: %u motor command(s), pair {low %u, high %u}",
                       sl, openpete_rumble_mod_commands(sl),
                       (unsigned)mlow, (unsigned)mhigh);
        }
    }
}

/* Documented at the declaration in run_internal.h. */
bool host_fullscreen_poll(bool split, bool prev_alt_enter)
{
    const bool alt_enter = openpete_keymap_scancode_down(SDL_SCANCODE_RETURN) &&
                           (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0;
    const bool own_edge = (alt_enter && !prev_alt_enter) ||
                          openpete_key_pressed(OPK_FULLSCREEN);
    if (split) {
        if (own_edge) SDL_SetAtomicInt(&g_native_fs_toggle_request, 1);
    } else if (SDL_SetAtomicInt(&g_native_fs_toggle_request, 0) || own_edge) {
        openpete_native_toggle_fullscreen();
    }
    return alt_enter;
}

#endif /* OPENPETE_HAS_PSYCROSS */

/** @brief Log the last openpete_frame value at process exit (atexit handler). */
static void log_frame_on_exit(void) {
    OP_ROLE_ASSERT(sim);
    OPL_INFO_F("exit", "last openpete_frame=%u", openpete_frame);
}

/** @brief Run host init and the host loop on the calling thread.
 *
 * The path when the thread split does not apply, and the fallback when
 * run_split_threaded cannot create the worker thread. The mod exit pass
 * runs inside the host loop's close on every path; the call here runs it
 * for a host init that failed before the loop.
 * @param opts The parsed options.
 * @return The init error code, or the host loop's exit code. */
static int run_recomp_fiber(const openpete_options_t* opts) OP_REQUIRES(sim) {
    int rc = run_host_init(opts);
    if (rc != 0) {
        openpete_mods_shutdown(NULL);
        return rc;
    }
    rc = run_host_loop(opts);
    host_trace_finish();
    openpete_ticker_stop();   /* the loop's threads are joined; nothing reads the clock now */
    return rc;
}

/* Worker entry: the entire host loop and the game fiber. libco fibers are
 * thread-affine, so the fiber must be created and switched on this one thread
 * for its whole life - which is why the loop moves wholesale rather than
 * having main drive parts of it. */
static int  s_worker_rc;
static const openpete_options_t* s_worker_opts;

/* PsyCross's GL context and the hidden window it was created on. A GL
 * context is current on one thread at a time: bring-up creates it current on
 * main, the worker takes it for the life of the host loop (psycross mode
 * rasterizes the OT and reads the present FBO back there), and main takes it
 * back for PsyX_Shutdown. Both NULL under null-GPU vk, which has no context. */
static SDL_GLContext s_psyx_gl_ctx;
static SDL_Window*   s_psyx_gl_win;

/** @brief Body of the sim thread under the thread split: take the GL context and run the game loop until it exits.
 *
 * The loop's close runs the mod exit pass on every path, so it precedes
 * main's teardown after the join. */
static int SDLCALL openpete_sim_thread(void* unused)
{
    OP_ROLE_BIND(sim);
    (void)unused;
    openpete_crash_thread_attach();
    if (s_psyx_gl_ctx && !SDL_GL_MakeCurrent(s_psyx_gl_win, s_psyx_gl_ctx))
        OPL_ERROR_F("boot", "GL context handoff to the sim thread failed: %s",
                    SDL_GetError());
    s_worker_rc = run_host_loop(s_worker_opts);
    host_trace_finish();
    if (s_psyx_gl_ctx)
        SDL_GL_MakeCurrent(s_psyx_gl_win, NULL);
    SDL_SetAtomicInt(&g_worker_done, 1);
    return 0;
}

/* Pump-side sim liveness. A guest-side hang (a corrupted model walk, an OT
 * loop) leaves the worker spinning forever with the window still pumping, so
 * the process would outlive every quit request. Silence past
 * SIM_STALL_WARN_MS is logged with the last heartbeat. After a quit request
 * the process is ended from the pump when the worker makes no progress for
 * SIM_QUIT_GRACE_MS, counted from the later of the request and the last
 * heartbeat; once the worker reports teardown the bound between heartbeats
 * is SIM_TEARDOWN_GRACE_MS, since one recorder flush may take seconds. */
enum { SIM_STALL_WARN_MS = 5000, SIM_QUIT_GRACE_MS = 3000, SIM_TEARDOWN_GRACE_MS = 20000 };
typedef struct {
    int    hb_last;          /* heartbeat at the last observed change */
    Uint64 hb_last_ms;       /* when it changed */
    Uint64 quit_req_ms;      /* when the quit request was first seen; 0 = none */
    int    stall_logged;
} sim_liveness_t;

/** @brief Seed the pump's liveness state from the current heartbeat. */
static void sim_liveness_init(sim_liveness_t* L)
{
    L->hb_last = SDL_GetAtomicInt(&g_sim_heartbeat);
    L->hb_last_ms = SDL_GetTicks();
    L->quit_req_ms = 0;
    L->stall_logged = 0;
}

/** @brief One pump iteration of the sim liveness check: log a stall, and force exit on a quit the worker has stopped progressing on.
 *
 * @param L The pump's liveness state. */
static void sim_liveness_check(sim_liveness_t* L)
{
    const int    hb  = SDL_GetAtomicInt(&g_sim_heartbeat);
    const Uint64 now = SDL_GetTicks();
    if (hb != L->hb_last) {
        if (L->stall_logged)
            OPL_INFO_F("host", "sim thread resumed after %llu ms",
                       (unsigned long long)(now - L->hb_last_ms));
        L->hb_last = hb; L->hb_last_ms = now; L->stall_logged = 0;
    } else if (hb > 0 && !L->stall_logged && now - L->hb_last_ms >= SIM_STALL_WARN_MS) {
        L->stall_logged = 1;
        OPL_WARN_F("host", "sim thread has made no progress for %llu ms "
                   "(heartbeat %d): the game is hung; closing the "
                   "window exits", (unsigned long long)(now - L->hb_last_ms), hb);
    }
    if (!g_shutdown_requested) return;
    if (!L->quit_req_ms) L->quit_req_ms = now ? now : 1;
    const Uint64 since = L->hb_last_ms > L->quit_req_ms ? L->hb_last_ms : L->quit_req_ms;
    const int grace = SDL_GetAtomicInt(&g_sim_teardown) ? SIM_TEARDOWN_GRACE_MS : SIM_QUIT_GRACE_MS;
    if (now - since < (Uint64)grace) return;
    OPL_ERROR_F("host", "sim thread made no progress for %d ms after the quit request "
                "(heartbeat %d, request %llu ms ago%s): forcing exit", grace, hb,
                (unsigned long long)(now - L->quit_req_ms),
                SDL_GetAtomicInt(&g_sim_teardown) ? ", in teardown" : "");
    openpete_log_file_sync();
    _exit(OPENPETE_EXIT_PANIC);
}

/** @brief Run the host with main owning SDL video, the window and the event pump, and a worker thread running the host loop.
 *
 * Main runs run_host_init, seeds the input seam, releases PsyCross's GL
 * context, and starts the worker (openpete_sim_thread, 64 MiB stack). Main
 * then pumps events, publishes input, updates the cursor and mouse capture,
 * and performs fullscreen toggles the worker requests, until the worker
 * finishes. The sim liveness check (sim_liveness_check) logs a heartbeat
 * silent for 5 s and forces exit when the worker makes no progress for
 * 3 s after a quit request (20 s between teardown heartbeats). After the join it retakes the GL
 * context and shuts down the native renderer, the disc cache and PsyCross.
 * A host init failure runs the mod exit pass before returning.
 * openpete_thread_split_enabled gives the reason for the split. Main
 * holds `sim` through init, hands it to the sim thread just before creating
 * it (holding `main` while it pumps), and takes it back after the join or
 * when the creation fails.
 * @param opts The parsed options.
 * @return The init error code, the worker's exit code, or
 *         run_recomp_fiber's when the thread cannot be created. */
static int run_split_threaded(const openpete_options_t* opts) OP_REQUIRES(sim)
{
    /* Bring SDL video up here, on the thread that will pump; the
     * SDL_Init(SDL_INIT_VIDEO) inside PsyX_Initialise (run_host_init, also on
     * this thread) then only takes another reference. */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        OPL_ERROR_F("boot", "SDL_Init(video) failed: %s %s", SDL_GetError(),
                    openpete_error_ref(OP_E_DISPLAY));
        openpete_fatal_message(OP_E_DISPLAY, "OpenPete could not start graphics",
                               "No display could be opened.");
        return OPENPETE_EXIT_SETUP_ERROR;
    }

    /* All host, PsyCross and SDL-subsystem init runs here, on main, before
     * the worker exists. The window must belong to the pumping thread (Win32
     * delivers a window's messages only to its creator), and SDL brings its
     * video, gamepad and haptic subsystems up on one thread and pumps them on
     * that thread. Initialising before main starts pumping serialises the
     * two without a barrier. */
    int init_rc = run_host_init(opts);
    if (init_rc != 0) {
        openpete_mods_shutdown(NULL);   /* mods may have loaded before the failure */
        return init_rc;
    }

    /* Seed the input seam before the worker exists: its first latch must find
     * a valid snapshot, or pad injection falls back to reading SDL on the
     * worker. Also publish an initial SDL-state snapshot for the same reason
     * - bindings evaluated on the worker must never reach SDL themselves. */
    openpete_host_input_seed_neutral();
    openpete_host_input_publish_sdl();

    /* Release PsyCross's GL context from this thread so the worker can take
     * it; main makes no GL call between here and the join. */
    s_psyx_gl_ctx = SDL_GL_GetCurrentContext();
    s_psyx_gl_win = SDL_GL_GetCurrentWindow();
    if (s_psyx_gl_ctx)
        SDL_GL_MakeCurrent(s_psyx_gl_win, NULL);

    s_worker_opts = opts;
    SDL_SetAtomicInt(&g_worker_done, 0);
    /* Explicit, large stack. The host loop and everything it calls used to
     * run on the process's main stack, which the OS grows generously; a
     * default-sized thread stack is a fraction of that, and this call tree is
     * deep (recompiled dispatch, feeders, extraction, the RHI). */
    SDL_PropertiesID props = SDL_CreateProperties();
    /* Main drops `sim` before the sim thread exists, and holds `main` while
     * it pumps; the sim thread's entry binds `sim`. */
    op_role_boot_handoff();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER,
                           (void*)openpete_sim_thread);
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "openpete-sim");
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER,
                          64 * 1024 * 1024);
    SDL_Thread* th = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!th) {
        OPL_ERROR_F("boot", "SDL_CreateThread(openpete-sim) failed: %s - "
                            "running single-threaded", SDL_GetError());
        op_role_boot_reacquire();
        if (s_psyx_gl_ctx)
            SDL_GL_MakeCurrent(s_psyx_gl_win, s_psyx_gl_ctx);
        return run_recomp_fiber(opts);
    }
    OPL_INFO_F("boot", "%s", "thread split: main owns window+pump, worker owns the sim");

    /* Pump loop. Deliberately fast relative to the sim: the worker latches
     * whatever was last published, so this period bounds input staleness.
     * SDL_WaitEventTimeout parks the thread instead of spinning, and still
     * wakes immediately when an event arrives: a window or input message
     * wakes the platform wait, and while a gamepad is open SDL polls it
     * every 1 ms inside the wait and returns on the event a poll produces.
     *
     * The park is about 1 ms on every platform. SDL's Windows wait
     * (WIN_WaitEventTimeout) converts the time left of the request to whole
     * milliseconds by truncation, after the time already spent inside the
     * call is subtracted; a 1 ms request becomes a 0 ms
     * MsgWaitForMultipleObjects that returns at once, and the loop spins.
     * Asking Windows for 2 ms leaves a remainder that truncates to 1 ms. X11
     * and Wayland wait in nanoseconds and take 1 ms as asked. */
#ifdef _WIN32
    enum { PUMP_PARK_REQUEST_MS = 2 };
#else
    enum { PUMP_PARK_REQUEST_MS = 1 };
#endif
    sim_liveness_t live;
    sim_liveness_init(&live);
    while (!SDL_GetAtomicInt(&g_worker_done)) {
        sim_liveness_check(&live);
        /* Four instants per iteration, so a stall on this thread can be
         * attributed rather than merely noticed. A Win32 modal resize or move
         * runs the OS's own loop inside the event pump, which is where the
         * split's display defects have historically hidden, and it is
         * indistinguishable from a quiet loop unless the pump is timed apart
         * from the park that precedes it. */
        const int64_t hp_t0 = (int64_t)op_now();
        SDL_WaitEventTimeout(NULL, PUMP_PARK_REQUEST_MS);
        const int64_t hp_waited = (int64_t)op_now();
        openpete_host_mouse_pump_iteration();
        openpete_pump_events();
        const int64_t hp_pumped = (int64_t)op_now();
        openpete_host_input_publish_sdl();
        pad_restamp_if_due(1);
        openpete_host_input_sample(
            (openpete_host_input_modstate() & SDL_KMOD_ALT) != 0);
        const int64_t hp_input = (int64_t)op_now();

        /* Idle-hide of the OS cursor. Another window op that belongs to this
         * thread, and cheap: one atomic read plus three flag reads. */
        openpete_cursor_update(opts->headless);
        /* Relative mouse mode is a window op too: applied here from the
         * capture request bits and the overlay ownership state. */
        openpete_host_mouse_apply_capture();

        /* Window ops belong to this thread. The worker raises the flag (from
         * a binding or Alt+Enter) and main performs the SDL call. */
        int hp_window = 0;
        if (SDL_SetAtomicInt(&g_native_fs_toggle_request, 0)) {
            hp_window = 1;
            OPL_INFO_F("native", "%s", "fullscreen toggle: main performing");
            openpete_native_toggle_fullscreen();
            OPL_INFO_F("native", "%s", "fullscreen toggle: main done");
        }
        openpete_pace_host_pump(hp_t0, hp_waited, hp_pumped, hp_input,
                                (int64_t)op_now(), hp_window);
    }

    SDL_WaitThread(th, NULL);
    op_role_boot_reacquire();   /* the sim thread is joined: main owns its state again */

    /* Worker is joined, so nothing else touches the device or the window.
     * These run here because they own main-thread resources (see the matching
     * comment in run_host_loop). PsyX_Shutdown destroys the GL context, which
     * must be current on the destroying thread. */
    if (s_psyx_gl_ctx)
        SDL_GL_MakeCurrent(s_psyx_gl_win, s_psyx_gl_ctx);
    openpete_stall_monitor_stop();   /* its clock reads must not overlap SDL_Quit */
    openpete_native_render_shutdown();
    openpete_disc_cache_shutdown();
    PsyX_Shutdown();
    openpete_ticker_stop();   /* after the join: the pump read the clock until now */
    return s_worker_rc;
}

/** @brief The disc trace's vbl column: openpete_vbl.
 *
 * @return The current VBL count. */
static int disc_trace_vbl(void) { return SDL_GetAtomicInt(&openpete_vbl_device); }

/** @brief Keep a headless run's SDL init from autolaunching a D-Bus session bus.
 *
 * Every SDL_Init/SDL_InitSubSystem calls SDL_DBus_Init, which opens the
 * session bus through libdbus. With DBUS_SESSION_BUS_ADDRESS unset and no
 * socket at $XDG_RUNTIME_DIR/bus (ssh, cron, a WSL shell without a user
 * manager), libdbus autolaunches one through `dbus-launch --autolaunch`, and
 * that dbus-daemon outlives the process. A headless run uses no session-bus
 * service (screensaver inhibit, IME, portals), so it sets the address to
 * the unknown transport `disabled:`: the connect fails and SDL runs without
 * D-Bus. A set address and a windowed run are left unchanged.
 * @param headless The run is headless. */
static void headless_disable_session_bus_autolaunch(bool headless) {
    if (headless && !SDL_getenv_unsafe("DBUS_SESSION_BUS_ADDRESS"))
        SDL_setenv_unsafe("DBUS_SESSION_BUS_ADDRESS", "disabled:", 0);
}

/** @brief Name the SDL video driver a headless run uses, or NULL for SDL's own pick.
 *
 * A headless run that opens no GL context (null-GPU PsyCross; the native
 * renderer is a surfaceless Vulkan device that takes no SDL surface) needs
 * no window on any platform, so the dummy driver serves it. A run that
 * keeps the PsyCross GL path needs a context: on Linux and the other
 * EGL platforms the offscreen driver gives one through surfaceless EGL
 * with no display server. On Windows and macOS the offscreen driver's
 * EGL is ANGLE's libEGL, which is not shipped, so SDL's platform driver
 * (WGL or CGL on a hidden window, which needs no display server) is kept.
 * @param psyx_gl The run opens PsyCross's GL context.
 * @return "dummy", "offscreen", or NULL. */
static const char* headless_video_driver(bool psyx_gl) {
    if (!psyx_gl) return "dummy";
#if defined(_WIN32) || defined(__APPLE__)
    return NULL;
#else
    return "offscreen";
#endif
}

/** @brief Keep a headless run off the session's display server.
 *
 * SDL's Wayland backend connects to $XDG_RUNTIME_DIR/wayland-0 even with
 * WAYLAND_DISPLAY unset, and its X11 backend to $DISPLAY; a compositor or
 * server that stops answering blocks SDL_Init(SDL_INIT_VIDEO) with no log
 * line. The driver headless_video_driver names is set as the
 * SDL_VIDEO_DRIVER hint at normal priority, so a caller-set
 * SDL_VIDEO_DRIVER or SDL_VIDEODRIVER environment variable still wins. A
 * windowed run is left to SDL's pick.
 * @param headless The run is headless.
 * @param psyx_gl The run opens PsyCross's GL context. */
static void headless_select_video_driver(bool headless, bool psyx_gl) {
    if (!headless) return;
    const char* want = headless_video_driver(psyx_gl);
    if (want) SDL_SetHint(SDL_HINT_VIDEO_DRIVER, want);
    const char* got = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    OPL_INFO_F("boot", "headless video driver: %s%s",
               got ? got : "(SDL's pick)",
               got && want && strcmp(got, want) != 0 ? " (set by the caller)" : "");
}

/** @brief Check that SDL's offscreen driver can load EGL before PsyCross opens its GL context.
 *
 * A PsyX_Initialise whose GL context fails tears PsyCross down through
 * GR_Shutdown, which calls GL entry points that were never loaded, so the
 * failure would surface as a SIGSEGV instead of a message. Under the
 * offscreen driver SDL_GL_LoadLibrary loads libEGL, initialises its
 * surfaceless or default display and chooses a config, which covers the
 * causes a host without a usable EGL hits. The library and the video
 * subsystem reference are released again; PsyCross reloads both. Any other
 * driver passes unchecked.
 * @param opts The parsed options.
 * @return true when the run may open the GL context; false after logging
 *         the cause and the override. */
static bool headless_gl_probe(const openpete_options_t* opts) {
    const char* drv = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (!opts->headless || !drv || SDL_strcasecmp(drv, "offscreen") != 0) return true;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        log_psyx_init_failure(opts);
        return false;
    }
    const bool ok = SDL_GL_LoadLibrary(NULL);
    if (ok) SDL_GL_UnloadLibrary();
    else log_psyx_init_failure(opts);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return ok;
}

/** @brief Choose a headless run's video driver and check that it can serve the run.
 *
 * The recomp smoke test and every run psycross_raster_unread does not
 * clear open PsyCross's GL context.
 * @param opts The parsed options.
 * @return false after logging the cause when the run's GL context cannot
 *         be opened; true otherwise. */
static bool headless_video_setup(const openpete_options_t* opts) {
    const bool psyx_gl = opts->test_recomp_call_addr != 0 || opts->test_recomp_segv
                         || !psycross_raster_unread(opts);
    headless_select_video_driver(opts->headless, psyx_gl);
    return !psyx_gl || headless_gl_probe(opts);
}

/** @brief Run the host on main's `sim` hold: the body of openpete_run.
 *
 * @param opts The parsed options.
 * @return The process exit code. */
static int run_on_sim(const openpete_options_t* opts) OP_REQUIRES(sim) {
    openpete_log_init(opts->log_format, opts->log_level_spec);
    openpete_disc_trace_set_vbl_source(disc_trace_vbl);
    /* The stderr mirror is normally already up (main() opens it before
     * attaching the recomp module). Harmless no-op if so; this covers
     * embedders that call openpete_run() directly. Before
     * atexit(log_frame_on_exit) because atexit runs LIFO - registering the
     * sink first makes it close LAST, so the exit lines still land in it. */
    openpete_log_file_open(opts->log_file);
    atexit(log_frame_on_exit);
    OPL_INFO_F("boot", "starting %s (frame_limit=%d max_seconds=%d headless=%d)",
               opts->display_name, opts->frame_limit, opts->max_runtime_seconds,
               (int)opts->headless);
    headless_disable_session_bus_autolaunch(opts->headless);

    /* Extraction worker pool size (--extract-threads), before any consumer
     * can resolve it. Threads themselves spawn lazily on first parallel run. */
    openpete_workpool_configure(opts->extract_threads);

    /* libco: capture the host fiber. Skipped under the split, where main
     * never switches and the worker captures its own. libco is built with
     * LIBCO_MP so co_active_ is thread-local; without it main's conversion
     * would make the worker skip its own (see core/CMakeLists.txt). */
    if (!openpete_split_applies(opts)) {
        cothread_t host_fiber = co_active();
        OPL_INFO_F("boot", "libco host fiber=%p", host_fiber);
    }

    openpete_psx_install_crash_handler();

    if (!headless_video_setup(opts)) {
        openpete_fatal_message(OP_E_DISPLAY, "OpenPete could not start graphics",
                               "No OpenGL context could be opened.");
        return OPENPETE_EXIT_PANIC;
    }

    /* Smoke test: dispatch one recompiled function and exit before the SDL
     * loop runs. */
    if (opts->test_recomp_call_addr != 0 || opts->test_recomp_segv) {
        return run_recomp_smoke_test(opts);
    }

#ifdef OPENPETE_HAS_PSYCROSS
    /* Dispatch the entry (the EXE header's, or OPENPETE_RUN_RECOMP's) on a fiber
     * and drive a frame loop with VSync yielding back here for present and
     * scene swap. */
    if (openpete_split_applies(opts))
        return run_split_threaded(opts);
    return run_recomp_fiber(opts);
#endif

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        if (opts->headless) {
            OPL_WARN_F("boot", "SDL_Init under '%s' failed (%s); retrying with dummy",
                       SDL_getenv("SDL_VIDEODRIVER"), SDL_GetError());
            SDL_setenv_unsafe("SDL_VIDEODRIVER", "dummy", 1);
            if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
                OPL_ERROR_F("boot", "SDL_Init dummy fallback also failed: %s", SDL_GetError());
                return OPENPETE_EXIT_PANIC;
            }
        } else {
            OPL_ERROR_F("boot", "SDL_Init: %s", SDL_GetError());
            return OPENPETE_EXIT_PANIC;
        }
    }
    OPL_INFO_F("boot", "SDL video driver: %s", SDL_GetCurrentVideoDriver());

    /* SDL3: windows are shown by default; SDL_WINDOW_SHOWN is gone. */
    SDL_WindowFlags window_flags = opts->headless ? SDL_WINDOW_HIDDEN : 0;
    SDL_Window* window = SDL_CreateWindow(
        opts->display_name, game_default_window_w(), game_default_window_h(),
        window_flags);
    if (!window) {
        OPL_ERROR_F("boot", "SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    /* SDL3: SDL_CreateRenderer takes a driver name, not an index+flags.
     * NULL = let SDL pick (accelerated); SDL_SOFTWARE_RENDERER forces the
     * software rasteriser used for the headless screenshot path. */
    SDL_Renderer* renderer = opts->headless
        ? SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER)
        : SDL_CreateRenderer(window, NULL);
    if (!renderer && !opts->headless) {
        OPL_WARN_F("boot", "accelerated renderer unavailable, falling back: %s",
                   SDL_GetError());
        renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    }
    if (!renderer) {
        OPL_ERROR_F("boot", "SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return OPENPETE_EXIT_PANIC;
    }

    Uint64 start = SDL_GetTicks();  /* SDL3: SDL_GetTicks is 64-bit */
    int frame = 0;
    int exit_code = OPENPETE_EXIT_OK;
    const char* exit_reason = "window-closed";
    bool quit = false;
    int  screenshot_status = 0;

    while (!quit) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                quit = true;
                exit_reason = "sdl-quit";
            }
        }

        SDL_SetRenderDrawColor(renderer, CLEAR_R, CLEAR_G, CLEAR_B, 0xFF);
        SDL_RenderClear(renderer);
        SDL_RenderPresent(renderer);

        for (int si = 0; si < opts->screenshot_frame_count; ++si) {
            if (frame != opts->screenshot_frames[si]) continue;
            char path[1024];
            openpete_screenshot_format_path(opts->screenshot_output,
                                            opts->screenshot_frames[si],
                                            /*present=*/0,
                                            path, sizeof(path));
            int rc = openpete_screenshot_save_png(renderer, path);
            if (rc != 0) {
                screenshot_status = 1;
                exit_code = OPENPETE_EXIT_PANIC;
                exit_reason = "screenshot-failed";
                quit = true;
                break;
            }
        }

        frame++;

        if (opts->frame_limit > 0 && frame >= opts->frame_limit) {
            quit = true;
            exit_reason = "frame-limit";
        }
        if (opts->max_runtime_seconds > 0 &&
            (SDL_GetTicks() - start) > (Uint64)opts->max_runtime_seconds * 1000) {
            quit = true;
            exit_reason = "max-runtime-seconds";
        }
    }

    Uint64 elapsed_ms = SDL_GetTicks() - start;
    OPL_INFO_F("exit", "reason=%s frames=%d elapsed_ms=%llu screenshot_status=%d",
               exit_reason, frame, (unsigned long long)elapsed_ms,
               screenshot_status);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    fprintf(stdout, "openpete-exit: reason=%s frames=%d ms=%llu code=%d\n",
            exit_reason, frame, (unsigned long long)elapsed_ms, exit_code);
    return exit_code;
}

int openpete_run(const openpete_options_t* opts) {
    /* main reaches this through a function pointer, so `sim` is acquired
     * here rather than in main. The time init group, the game layout and
     * the registered disc builds are complete (the options parse re-read the clock's knobs; the game build
     * was installed) and sealed before any thread that reads them starts. */
    op_role_boot_acquire();
    op_time_init();
    op_seal(time);
    op_seal(game_layout);
    op_seal(disc_builds);
    const int rc = run_on_sim(opts);
    op_role_boot_release();
    return rc;
}
