// core/runtime/host_hotkeys.c - the host loop's keyboard and pad UI: the
// binding sample, the dev overlay and settings menu input, the pause and
// step keys with the scripted pause schedule and the paused repaint, and
// the host hotkeys (toggles, the fill-tag inspector, the debug cameras).

#include "host_hotkeys.h"
#include "run_internal.h"
#include "eager.h"
#include "host_loop_internal.h"

#include "openpete.h"
#include "openpete_camlock.h"
#include "openpete_devui.h"
#include "openpete_fiber.h"
#include "openpete_fillpick.h"
#include "openpete_freecam.h"
#include "openpete_host_mouse.h"
#include "openpete_interp.h"
#include "openpete_keymap.h"
#include "openpete_log.h"
#include "openpete_menu.h"
#include "openpete_mods.h"
#include "openpete_native_render.h"
#include "openpete_osd.h"
#include "openpete_rebind.h"
#include "openpete_rumble.h"
#include "openpete_settings_save.h"
#include "openpete_time.h"
#include "no_lod.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef OPENPETE_HAS_PSYCROSS

/* Pause and step requests from the dev overlay, read by the host loop where it
 * reads the pause and step keys: s_pause_req holds -1 (none), 0 or 1;
 * s_step_req is nonzero while a step waits. s_paused_shown publishes the
 * loop's paused state. */
static SDL_AtomicInt s_pause_req = { -1 };
static SDL_AtomicInt s_step_req;
static SDL_AtomicInt s_paused_shown;

void openpete_sim_pause_request(int on) { SDL_SetAtomicInt(&s_pause_req, on ? 1 : 0); }
void openpete_sim_step_request(void)    { SDL_SetAtomicInt(&s_step_req, 1); }
int  openpete_sim_paused(void)          { return SDL_GetAtomicInt(&s_paused_shown); }

/* The turbo row's hold, ORed with the turbo key by openpete_turbo_held. */
static SDL_AtomicInt s_turbo_row;

void openpete_turbo_hold(int on) { SDL_SetAtomicInt(&s_turbo_row, on ? 1 : 0); }
int  openpete_turbo_held(void)
{
    return openpete_key_down(OPK_TURBO) || SDL_GetAtomicInt(&s_turbo_row) != 0;
}

/* Scripted pause for windowed harnesses, OPENPETE_PAUSE_AT_VBL=N[,M[,K]]:
 * pause at VBL N through the dev overlay's pause request (the pause key's
 * path), then, after every M paused idle iterations (each one repaints the
 * held scene where a repaint runs), request one step, K steps in all (M
 * defaults to 30, K to 1), and after M more request the resume. Each transition logs a frame-step line the harness parses.
 * Unset or malformed leaves it disarmed. */
static struct {
    int      armed;      /* -1 unread, 0 off, 1 on */
    unsigned vbl;        /* N */
    int      every;      /* M */
    int      steps;      /* K */
    int      stage;      /* 0 waiting for N, 1 paused, 2 done */
    int      idle;       /* paused idle iterations since the last transition */
    int      stepped;    /* steps requested so far */
} s_pause_at = { -1, 0, 30, 1, 0, 0, 0 };

/* Set when a paused step finished; host_pause_step logs it on the next
 * iteration, after that iteration's canonical present counted its tick. */
static int s_step_done_log;
/* VBLs the pending step has run. A step with no draw for STEP_MAX_VBLS
 * VBLs (about 10 s) ends with a warning and the sim stays paused. */
static unsigned s_step_vbls;
#define STEP_MAX_VBLS 600u

/** @brief Drive the OPENPETE_PAUSE_AT_VBL schedule for one host iteration, before the pause and step requests are read.
 *
 * Raises the same requests the dev overlay's Pause sim row and Step tick
 * button raise, so the run takes the pause key's path. A step is requested
 * only once the previous one has finished (no step pending).
 * @param paused The host loop's paused flag.
 * @param step_one The host loop's pending-step flag. */
static void pause_at_vbl_drive(bool paused, bool step_one)
{
    if (s_pause_at.armed < 0) {
        const char *e = getenv("OPENPETE_PAUSE_AT_VBL");
        unsigned n = 0; int m = 30, k = 1;
        const int got = (e && *e) ? sscanf(e, "%u,%d,%d", &n, &m, &k) : 0;
        s_pause_at.armed = got >= 1 && m > 0 && k >= 0;
        s_pause_at.vbl = n; s_pause_at.every = m; s_pause_at.steps = k;
    }
    if (!s_pause_at.armed || s_pause_at.stage == 2) return;
    if (s_pause_at.stage == 0) {
        if (openpete_feed_key_now() >= s_pause_at.vbl && !paused) {
            openpete_sim_pause_request(1);
            s_pause_at.stage = 1; s_pause_at.idle = 0;
        }
        return;
    }
    if (!paused || step_one || s_pause_at.idle < s_pause_at.every) return;
    s_pause_at.idle = 0;
    if (s_pause_at.stepped < s_pause_at.steps) {
        s_pause_at.stepped++;
        openpete_sim_step_request();
    } else {
        openpete_sim_pause_request(0);
        s_pause_at.stage = 2;
    }
}


/* The menu, fullscreen and pointer-probe latches across iterations: state
 * the keymap's press edges do not carry. Reset per run by
 * host_hotkey_latch_init. */
static struct {
    bool prev_menu_close;   /* menu_close was down at the previous poll; the menu closes on RELEASE */
    bool menu_close_stale;  /* menu_close held from before the menu opened */
    int  prev_mouse_owner;  /* the pointer's owner at the previous poll */
    bool prev_alt_enter;    /* Alt+Enter was down at the previous poll */
    int  probe_prev_owner;  /* the pointer's owner the mouse probe last logged, -1 before the first */
} s_hotkey_latch;

/* Documented at the declaration in host_hotkeys.h. */
void host_hotkey_latch_init(void) {
    s_hotkey_latch.prev_menu_close  = false;
    s_hotkey_latch.menu_close_stale = false;
    s_hotkey_latch.prev_mouse_owner = OPENPETE_MOUSE_OWNED_NONE;
    s_hotkey_latch.prev_alt_enter   = false;
    s_hotkey_latch.probe_prev_owner = -1;
}

/** @brief Report whether this poll is the menu_close release that closes the open settings menu.
 *
 * The close fires on the release edge of a menu_close source. A source held
 * while the menu was still closed (Start held through the hold that opens
 * the menu, which binds both `menu` as hold:pad:start and `menu_close` as
 * pad:start) is stale: its release clears the stale flag instead of closing
 * the menu, so only a press that began with the menu open closes it.
 * @param close_k Whether menu_close reads held on this poll.
 * @param prev_close Whether menu_close read held on the previous open poll.
 * @param owner_changed Whether the mouse owner changed on this poll; a
 *        release on such a poll is the arbitrated view going neutral.
 * @param stale In: a menu_close source has been held since before the menu
 *        opened. Cleared once menu_close reads released.
 * @return True when the menu closes on this poll. */
static bool menu_close_released(bool close_k, bool prev_close,
                                bool owner_changed, bool *stale)
{
    const bool edge = !close_k && prev_close && !owner_changed && !*stale;
    if (!close_k) *stale = false;
    return edge;
}

/* Documented at the declaration in host_hotkeys.h. */
bool step_begin_vbl(bool paused, bool *step_one)
{
    const bool stepping = paused && *step_one;
    if (!stepping) *step_one = false;
    return stepping;
}

/* Documented at the declaration in host_hotkeys.h. */
void step_after_vbl(bool stepping, bool *step_one)
{
    if (!stepping) return;
    s_step_vbls++;
    if (openpete_present_pending) {
        *step_one = false;
        s_step_done_log = 1;
    } else if (s_step_vbls >= STEP_MAX_VBLS) {
        *step_one = false;
        OPL_WARN_F("frame-step", "no tick completed in %u VBLs; step ended "
                   "at openpete_frame=%u", s_step_vbls, openpete_frame);
        s_step_vbls = 0;
    }
}

/* Hold time after which a held step key repeats, one tick at a time. */
#define STEP_REPEAT_DELAY_NS ((op_time_t)300 * 1000 * 1000)

/* Documented at the declaration in host_hotkeys.h. */
void host_pause_step(bool *paused, bool *step_one)
{
    if (s_step_done_log) {
        s_step_done_log = 0;
        OPL_INFO_F("frame-step", "stepped to openpete_frame=%u canon_tick=%u vbls=%u key=%u",
                   openpete_frame, openpete_canon_tick, s_step_vbls,
                   openpete_feed_key_now());
        s_step_vbls = 0;
    }
    pause_at_vbl_drive(*paused, *step_one);
    static op_time_t step_held_since;
    const bool step_edge = openpete_key_pressed(OPK_FRAME_STEP);
    if (step_edge) step_held_since = op_now();
    const bool step_repeat = !step_edge && openpete_key_down(OPK_FRAME_STEP) &&
                             op_now() - step_held_since >= STEP_REPEAT_DELAY_NS;
    const bool step_k = step_edge || step_repeat ||
                        SDL_SetAtomicInt(&s_step_req, 0) != 0;
    const int pause_req = SDL_SetAtomicInt(&s_pause_req, -1);
    if (openpete_key_pressed(OPK_PAUSE) ||
        (pause_req >= 0 && (pause_req != 0) != *paused)) {
        *paused = !*paused;
        /* Enter: mute. Exit: restore the live mute baseline (boot --mute,
         * possibly toggled since via the dev overlay). */
        extern int PsyX_SPUAL_SetMute(int on_off);
        PsyX_SPUAL_SetMute(*paused ? 1 : run_mute_baseline());
        /* A paused game is not producing actuator bytes, but the last pair
         * it published would keep being re-issued; silence the motors for the
         * same reason the audio mutes. */
        openpete_rumble_set_paused(*paused ? 1 : 0);
        OPL_INFO_F("frame-step", "%s at openpete_frame=%u canon_tick=%u key=%u",
                   *paused ? "paused" : "resumed", openpete_frame, openpete_canon_tick,
                   openpete_feed_key_now());
    }
    if (!*paused) { *step_one = false; s_step_vbls = 0; }
    else if (step_k && !*step_one) {
        *step_one = true;
        OPL_INFO_F("frame-step", "step requested at openpete_frame=%u canon_tick=%u",
                   openpete_frame, openpete_canon_tick);
    }
    SDL_SetAtomicInt(&s_paused_shown, *paused ? 1 : 0);
}

/* Documented at the declaration in host_hotkeys.h. */
void host_paused_repaint(const openpete_options_t *opts) OP_REQUIRES(sim)
{
    s_pause_at.idle++;
    if (opts->headless || openpete_skip_present_until_level || eager_active())
        return;
    openpete_mods_present_flush(&g_game.cpu, 0);
    openpete_native_render_frame((uint64_t)(op_now() / 1000));
}


/** @brief Log the pointer's owner and the keymap's mouse markers when the owner changed since the last log, under the mouse probe.
 *
 * Across an ownership change the poll's markers show the snap: they sit at
 * the totals, so nothing made under the overlay is delivered when it
 * closes. */
static void mouse_probe_owner_log(void)
{
    if (!openpete_host_mouse_probe()) return;
    const int owner = openpete_host_mouse_owned();
    if (owner == s_hotkey_latch.probe_prev_owner) return;
    long long w, mx, my;
    openpete_keymap_mouse_markers(&w, &mx, &my);
    openpete_host_mouse_probe_log("keymap",
        "owner %d -> %d; markers wheel=%lld mx=%lld my=%lld "
        "(totals %lld %lld %lld)", s_hotkey_latch.probe_prev_owner, owner, w, mx, my,
        (long long)openpete_host_mouse_wheel_total(),
        (long long)openpete_host_mouse_motion_total(0),
        (long long)openpete_host_mouse_motion_total(1));
    s_hotkey_latch.probe_prev_owner = owner;
}

/** @brief Apply the menu key's toggle and a target state requested from another thread; opening the menu closes the dev overlay. */
static void menu_toggle_poll(void)
{
    if (openpete_key_pressed(OPK_MENU)) {
        openpete_menu_toggle();
        if (openpete_menu_is_open()) openpete_devui_close();
    }
    /* A target state requested from another thread (the pump's
     * Escape dismiss, the `menu` inject verb). Consumed here so the
     * toggle lands between two reads of the override clear latch,
     * and mirrors the OPK_MENU block: opening closes the overlay. */
    int want_open = 0;
    if (openpete_host_mouse_take_menu_request(&want_open) &&
        (want_open != 0) != (openpete_menu_is_open() != 0)) {
        openpete_menu_toggle();
        if (openpete_menu_is_open()) openpete_devui_close();
        openpete_host_mouse_probe_log("menu", "request %s -> open=%d",
                                      want_open ? "open" : "close",
                                      openpete_menu_is_open());
    }
}

/** @brief Drive the open settings menu: close on the menu_close release, else the nav keys and the pointer, and hold the debug cameras' fly axes neutral. */
static void menu_open_input(void)
{
    /* Keyboard and pad both drive nav through one binding per
     * direction, so holding a direction - key, d-pad or stick -
     * steps once until released, the same feel either way. The
     * pad's own view of these buttons is held neutral while the
     * menu is open, so this cannot double up with gameplay. */
    bool close_k = openpete_key_down(OPK_MENU_CLOSE);
    /* A mouse-bound menu_close held while the pointer drifts
     * onto the fill-tag inspector reads a release as the
     * arbitrated view goes neutral; a release edge on a poll
     * where the owner changed is not a release. */
    const int mouse_owner = openpete_host_mouse_owned();
    const bool owner_changed = mouse_owner != s_hotkey_latch.prev_mouse_owner;
    s_hotkey_latch.prev_mouse_owner = mouse_owner;
    if (menu_close_released(close_k, s_hotkey_latch.prev_menu_close, owner_changed,
                            &s_hotkey_latch.menu_close_stale)) {
        /* Close on the RELEASE edge of Start/Circle. The game pad
         * is held neutral only while the menu is open, so closing
         * on the press would let the still-held button bleed into
         * the game next frame (Start -> the game's pause menu); by
         * release it's already up. */
        openpete_menu_toggle();   /* closes it */
    } else {
        if (openpete_key_pressed(OPK_MENU_UP))    openpete_menu_nav(-1);
        if (openpete_key_pressed(OPK_MENU_DOWN))  openpete_menu_nav(+1);
        if (openpete_key_pressed(OPK_MENU_LEFT))  openpete_menu_adjust(-1);
        if (openpete_key_pressed(OPK_MENU_RIGHT)) openpete_menu_adjust(+1);
        if (openpete_key_pressed(OPK_MENU_SELECT)) openpete_menu_activate();
        /* Pointer: hover, click, wheel, the X glyph. After the
         * keys so a key and a click in one iteration apply in
         * a fixed order. */
        openpete_menu_mouse_poll();
    }
    s_hotkey_latch.prev_menu_close = close_k;

    /* Freecam axes are level-held and consumed every game tick,
     * but the latch below is skipped while the menu owns the
     * keyboard - hold neutral here so a movement key held at
     * menu-open doesn't keep flying the camera behind the panel. */
    if (openpete_freecam_active() || openpete_camlock_active()) {
        openpete_freecam_axes ax = {0};
        openpete_freecam_set_axes(&ax);
    }
}

/* Documented at the declaration in host_hotkeys.h. */
void host_ui_poll(void)
{
    /* Sample every binding once per iteration; openpete_key_pressed()
     * below reads the edge this computed, one latch bit per action, so a
     * held key fires once and every hotkey is rebindable from [keys.host]. Reads SDL's keyboard/gamepad
     * state caches, which the previous iteration's pump refreshed - so
     * this must run before the first hotkey test, and the turbo peek at
     * the top of the loop deliberately sees the previous sample.
     * openpete_handle_event consumes QUIT/CLOSE/ESC without forwarding
     * them, so PsyCross's exit-on-quit path never fires. The binding
     * page's requests apply first, so a capture's Escape cancel and its
     * commit land between two polls. */
    openpete_rebind_drain();
    openpete_keymap_poll();
    mouse_probe_owner_log();

    /* Dev config overlay. Mutually exclusive with the settings menu -
     * two input-owning overlays with different capture models is
     * complexity with no payoff. */
    openpete_devui_host_poll();

    /* The in-game settings menu. While open it captures the nav keys and
     * the game pad is neutralised, so the other hotkeys and gameplay
     * input stay inert. Esc-closes is handled in the event pump. */
    menu_toggle_poll();
    if (openpete_menu_is_open()) {
        menu_open_input();
    } else {
        s_hotkey_latch.prev_menu_close = false;
        s_hotkey_latch.menu_close_stale = openpete_key_down(OPK_MENU_CLOSE) != 0;
    }
    /* The settings writer: menu changes and the dev overlay's Save
     * settings reach openpete.toml from this thread only. */
    openpete_settings_save_poll();
}

/* Documented at the declaration in host_hotkeys.h. */
void host_hotkeys_toggles(bool split)
{
    /* Player screenshot. The key edge only raises the request: the
     * capture is a blocking readback of the native renderer's
     * offscreen, and the device belongs to the present thread while
     * eager is engaged, so it runs beside the --screenshot-on-frame
     * dumps later this iteration, under the same handoff. */
    if (openpete_key_pressed(OPK_SCREENSHOT))
        openpete_screenshot_request();

    /* Live on/off A/B for interpolation itself. Off = intermediate
     * renders present the lagged frame unchanged (a clean "is
     * frame-gen doing anything perceptible?" test). Bound to I by
     * default; the toast states the new mode. */
    if (openpete_key_pressed(OPK_INTERP_TOGGLE)) {
        openpete_interp_toggle_set(!openpete_interp_toggle_on());
        openpete_osd_notify("host", "hotkey.interp", 0, "INTERPOLATION: %s",
                            openpete_interp_toggle_on() ? "ON" : "OFF");
    }

    /* Live toggle of sector LOD original <-> extended (the settings
     * menu's LOD row / --lod flag). Goes through openpete_no_lod_set
     * so openpete_fog_scale follows the mode - a raw openpete_no_lod
     * poke would leave the terrain fog latched. The setter logs the
     * new state. Native-renderer display policy; inert under
     * PsyCross. The toggle flips the wanted mode, which the next latch
     * applies; the menu re-syncs its LOD row from it when next opened. */
    if (openpete_key_pressed(OPK_LOD_TOGGLE)) {
        const int on = !openpete_no_lod_wanted();
        openpete_no_lod_set(on, -1.0f);
        /* Same key the O menu's LOD row uses, so a player who toggles
         * both ways sees one toast repaint rather than two stack. */
        openpete_osd_notify("host", "menu.lod", 0, "LOD: %s",
                            on ? "EXTENDED" : "ORIGINAL");
    }

    /* Alt+Enter (either Alt), the `fullscreen` binding, or a window
     * double-click toggles the native window between windowed and
     * borderless fullscreen. Moved here from PsyCross, which only ever
     * drove its now-hidden GL window; openpete_native_toggle_fullscreen
     * is a no-op in headless / PsyCross-only runs. The PsyCross
     * g_altKeyState gate still stops the Enter from registering as
     * Start while Alt is held.
     *
     * Alt+Enter is hardcoded rather than a binding because the keymap
     * has no chord vocabulary; `fullscreen` is the rebindable single
     * key for it and ships unbound so nothing changes by default. */
    s_hotkey_latch.prev_alt_enter = host_fullscreen_poll(split, s_hotkey_latch.prev_alt_enter);
}

/* Documented at the declaration in host_hotkeys.h. */
void host_hotkeys_fillpick(void)
{
    /* The LP fill-tag inspector. Engaged, it picks the terrain face
     * under the crosshair each present and tints the fill-tag set, so
     * the frozen list in tests/freecam/fill-tags.verdicts can be
     * audited against the live scene. Preview draws the untagged
     * candidates too - an under-fill site's covering face is
     * invisible until it is tagged. FILL / NOFILL / "no session
     * verdict" record against the picked face and re-render with it
     * immediately; undo steps back. The verdict keys are gated on
     * engaged so their bindings stay free while it is off. */
    if (openpete_key_pressed(OPK_FILLPICK)) openpete_fillpick_toggle();
    if (!openpete_fillpick_active()) return;
    if (openpete_key_pressed(OPK_FILLPICK_PREVIEW))
        openpete_fillpick_set_preview(!openpete_fillpick_preview());
    if (openpete_key_pressed(OPK_FILLPICK_FILL))
        openpete_fillpick_mark(OPENPETE_FILLPICK_FILL);
    if (openpete_key_pressed(OPK_FILLPICK_NOFILL))
        openpete_fillpick_mark(OPENPETE_FILLPICK_NOFILL);
    if (openpete_key_pressed(OPK_FILLPICK_NONE))
        openpete_fillpick_mark(OPENPETE_FILLPICK_NONE);
    if (openpete_key_pressed(OPK_FILLPICK_UNDO))
        openpete_fillpick_undo();
}

/** @brief Latch the engaged debug camera's fly and look axes, the mouse-look counts and the speed steps from this poll's bindings. */
static void camera_axes_latch(void)
{
    /* Digital fly/look keys and the analog stick axes are separate
     * bindings that SUM (then clamp) - so a key and a stick both
     * drive the camera, and either can be rebound or unbound
     * without disturbing the other. */
    float k_fwd = (float)(openpete_key_down(OPK_FLY_FORWARD)
                        - openpete_key_down(OPK_FLY_BACK));
    float k_str = (float)(openpete_key_down(OPK_FLY_RIGHT)
                        - openpete_key_down(OPK_FLY_LEFT));
    float k_up  = (float)(openpete_key_down(OPK_FLY_UP)
                        - openpete_key_down(OPK_FLY_DOWN));
    float k_yaw = (float)(openpete_key_down(OPK_LOOK_RIGHT)
                        - openpete_key_down(OPK_LOOK_LEFT));
    float k_pit = (float)(openpete_key_down(OPK_LOOK_UP)
                        - openpete_key_down(OPK_LOOK_DOWN));
    /* The stick axes carry their own sign convention in the
     * binding: fly_axis_y and look_axis_y default to the INVERTED
     * axis ("pad:-lefty"), because SDL reports stick-up as
     * negative and the camera wants stick-up = forward / look up.
     * Anyone who prefers the other convention flips it in
     * openpete.toml instead of in this file. */
    float p_fwd = openpete_key_axis(OPK_FLY_AXIS_Y);
    float p_str = openpete_key_axis(OPK_FLY_AXIS_X);
    float p_yaw = openpete_key_axis(OPK_LOOK_AXIS_X);
    float p_pit = openpete_key_axis(OPK_LOOK_AXIS_Y);

    openpete_freecam_axes ax;
    #define FC_CLAMP(x)  ( (x) > 1.0f ? 1.0f : ((x) < -1.0f ? -1.0f : (x)) )
    ax.move_fwd    = FC_CLAMP(k_fwd + p_fwd);
    ax.move_strafe = FC_CLAMP(k_str + p_str);
    ax.move_up     = FC_CLAMP(k_up);
    ax.look_yaw    = FC_CLAMP(k_yaw + p_yaw);
    ax.look_pitch  = FC_CLAMP(k_pit + p_pit);
    #undef FC_CLAMP
    openpete_freecam_set_axes(&ax);
    /* Mouse look: the look axes' mouse:x / mouse:y sources are
     * displacements, accumulated into running count totals the
     * camera drives consume against their own markers. Host
     * state; no guest store here. */
    openpete_freecam_add_look_counts(
        openpete_key_delta(OPK_LOOK_AXIS_X),
        openpete_key_delta(OPK_LOOK_AXIS_Y));

    /* Speed step: one step per key or button edge plus one per
     * wheel notch counted into the action this poll, so several
     * notches in one iteration step several times. */
    for (int n = openpete_key_edge(OPK_SPEED_DOWN)
               + openpete_key_pulses(OPK_SPEED_DOWN); n > 0; n--)
        openpete_freecam_adjust_speed(-1);
    for (int n = openpete_key_edge(OPK_SPEED_UP)
               + openpete_key_pulses(OPK_SPEED_UP); n > 0; n--)
        openpete_freecam_adjust_speed(+1);
}

/** @brief Apply the free camera's and the camera lock's pending row requests, in that order, then the speed step request.
 *
 * Each engage or disengage request applies only when it differs from the
 * current state, through the same toggles the F and G keys use, so the bk2
 * refusal, the taint, the rewind clear and the lock's free camera
 * disengage are the keys'. */
static void camera_requests_apply(void)
{
    const int fc = openpete_freecam_take_active_request();
    if (fc >= 0 && (fc != 0) != openpete_freecam_active())
        openpete_freecam_toggle();
    const int cl = openpete_camlock_take_request();
    if (cl >= 0 && (cl != 0) != openpete_camlock_active()) {
        if (cl && openpete_freecam_active())
            openpete_freecam_toggle();   /* the two share the fly axes */
        openpete_camlock_toggle();
    }
    const int sp = openpete_freecam_take_speed_request();
    if (sp >= 0) openpete_freecam_set_speed(sp);
}

/* Documented at the declaration in host_hotkeys.h. */
void host_hotkeys_camera(void)
{
    camera_requests_apply();

    /* Debug free camera. While engaged: the fly keys / left stick
     * move, the look keys / right stick aim, rise+descend raise and
     * lower, the speed keys / triggers step the speed, and teleport
     * drops Spyro here and hands the camera back. We own only the
     * toggle + input latch here; the camera write and the
     * camera-follow / player-update freeze are game-side
     * (games/common/freecam_game.c). Latched as level-held
     * axes so movement continues while held, consumed once per game
     * tick in game_on_frame_pre. Keyboard and pad sum (then clamp), so
     * either or both drive it. */
    if (openpete_key_pressed(OPK_FREECAM)) {
        openpete_freecam_toggle();
        openpete_osd_notify("host", "menu.freecam", 0, "FREECAM: %s",
                            openpete_freecam_active() ? "ON" : "OFF");
    }

    /* Render camera lock (openpete_camlock.h) - holds/flies the
     * PRESENTED view while the sim (cutscenes included) continues
     * untouched. Shares the freecam's fly axes and speed step, so the
     * two are mutually exclusive: engaging one disengages the other. */
    if (openpete_key_pressed(OPK_CAMLOCK)) {
        if (!openpete_camlock_active() && openpete_freecam_active())
            openpete_freecam_toggle();
        openpete_camlock_toggle();
    }
    if (openpete_freecam_active() && openpete_camlock_active())
        openpete_camlock_toggle();   /* freecam engaged while locked wins */
    /* Either camera engaged asks for relative mouse mode; the request
     * is arbitrated against the overlays and applied on the
     * window-owning thread. Dropped on disengage. */
    openpete_host_mouse_request_capture(OPENPETE_MOUSE_CAP_CAMERA,
        openpete_freecam_active() || openpete_camlock_active());

    /* Drop Spyro at the camera and hand the camera back to the game.
     * Latched as a one-shot request - the move needs the game fiber, so
     * freecam_game applies it (and disengages) on the next tick; the
     * request is dropped if the camera isn't engaged. Edge-tracked out
     * here rather than under the engaged branch so a teleport key
     * already held when the camera engages doesn't count as a press. */
    if (openpete_key_pressed(OPK_TELEPORT))
        openpete_freecam_request_teleport();

    if (openpete_freecam_active() || openpete_camlock_active())
        camera_axes_latch();
}

#endif /* OPENPETE_HAS_PSYCROSS */
