// core/runtime/host_hotkeys.h - the host loop's keyboard and pad UI: the
// binding sample and menu input, the pause and step keys, the host hotkeys.
#pragma once

#include "openpete.h"

#include <stdbool.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Reset the menu-close, pointer-owner, Alt+Enter and mouse-probe latches to their start-of-run state. */
void host_hotkey_latch_init(void);

/** @brief Sample the bindings and run the input-owning UI: the binding page's requests, the keymap poll, the mouse probe log, the dev overlay, the settings menu and the settings writer.
 *
 * Runs once per host iteration before any hotkey test, since every
 * openpete_key_pressed() edge after it reads the sample this takes. While
 * the settings menu is open it drives the menu's nav keys and pointer and
 * holds the debug cameras' fly axes neutral; the menu closes on the release
 * edge of a menu_close source pressed while it was open. */
void host_ui_poll(void);

/** @brief Apply the pause and step keys and the dev overlay's pause and step requests for one host iteration.
 *
 * Pause is an edge-triggered toggle. Step raises the pending-step flag on
 * the key's press edge while paused; the flag stays raised across VBLs until
 * the game draws (its tick boundary), so one press is one tick whether the
 * game's tick spans one VBL or two. A key held for STEP_REPEAT_DELAY_NS
 * re-raises the flag each time the previous step finishes, so holding steps
 * tick after tick. The overlay's requests take the same paths: a pause
 * request that differs from the state toggles it, a step request steps one
 * tick. Pausing mutes the audio and silences the motors; resuming restores
 * the live mute baseline. Publishes the paused state for
 * openpete_sim_paused. Drives the OPENPETE_PAUSE_AT_VBL schedule first.
 * @param paused The host loop's paused flag, toggled here.
 * @param step_one The host loop's one-tick flag, raised here while paused. */
void host_pause_step(bool *paused, bool *step_one);

/** @brief Present the held scene on a paused host iteration that presented nothing else.
 *
 * While the sim is paused with no step pending no tick runs, so neither the
 * canonical nor the sub-tick present fires, and the window would hold its
 * last image with the dev overlay, the toasts and the cursor frozen inside
 * it - the overlay could not even be used to resume. This presents the held
 * scene instead, once per call (the paused loop sleeps 16 ms per iteration),
 * without a re-extract, since nothing advanced: the hold the intermediate
 * path applies while the world is not live. Windowed only, never during a
 * skip fast-forward, and never while eager production holds the display
 * (it is disengaged while paused). Every call counts one paused idle
 * iteration for the OPENPETE_PAUSE_AT_VBL schedule, presented or not.
 * @param opts The parsed options. */
void host_paused_repaint(const openpete_options_t *opts) OP_REQUIRES(sim);

/** @brief Decide whether the VBL about to run belongs to a paused step, clearing the step flag when it does not.
 *
 * @param paused The host loop's paused flag.
 * @param step_one The host loop's pending-step flag.
 * @return True when the VBL runs for a pending paused step. */
bool step_begin_vbl(bool paused, bool *step_one);

/** @brief End a paused step on the VBL the game drew, its tick boundary.
 *
 * A VBL with no draw (the first VBL of a two-VBL tick, or a load) keeps the
 * step pending into the next; after STEP_MAX_VBLS VBLs with no draw the step
 * ends with a warning and the sim stays paused.
 * @param stepping Whether the VBL ran for a paused step (step_begin_vbl); false returns at once.
 * @param step_one The host loop's pending-step flag, lowered when the step ends. */
void step_after_vbl(bool stepping, bool *step_one);

/** @brief Apply the single-action hotkeys: screenshot request, interpolation toggle, LOD toggle and the fullscreen gestures.
 *
 * Each arm reads its own key edge and writes its own subsystem.
 * @param split Whether the thread split is active: the fullscreen toggle
 *        then goes to the window-owning thread as a request. */
void host_hotkeys_toggles(bool split);

/** @brief Apply the fill-tag inspector keys: the engage toggle, then while engaged the preview toggle, the three verdict marks and undo. */
void host_hotkeys_fillpick(void);

/** @brief Apply the debug camera rows' requests and keys: the freecam and camera-lock toggles with their exclusivity, the relative-mouse request, the teleport request, then while a camera is engaged its fly and look axes and speed steps.
 *
 * The rows' pending requests (freecam, camlock, freecam_speed) apply
 * first, so a row write and a key press in one iteration apply in that
 * order. The order is load-bearing: the exclusivity test reads both
 * toggles' results, and the capture request and the axes latch read the
 * resulting engagement. */
void host_hotkeys_camera(void);
