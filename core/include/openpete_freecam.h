#pragma once
/* openpete_freecam.h - host-side state for the debug free camera (F key).
 *
 * This is the GENERIC half: it owns the on/off toggle, the per-axis input
 * intent latched from the keyboard each host iteration, and the discrete
 * speed step. It carries NO Spyro addresses or camera-struct knowledge, so
 * it lives in core alongside the other host input handling (run.c). The
 * game-specific half - reading g_Camera, applying the motion, and freezing
 * the game's camera-follow / player-update functions while active - lives in
 * games/common/freecam_game.c behind a per-game descriptor
 * (games/<game>/features/freecam.c) and reads this state each tick.
 *
 * Ported from C0mposer's Spyro-1-Practice-Rom (src/freecam.c): freeze the
 * follow logic, then drive the camera position/angle directly. Debug tool -
 * OFF by default (zero effect, oracle-clean); ON is an intentional,
 * opt-in gameplay-state divergence for inspecting a level.
 */

#include <stdbool.h>

/* Discrete speed steps (slowest -> fastest). The per-step move/turn magnitudes
 * live game-side (the freecam descriptor), keyed by this index. */
#define OPENPETE_FREECAM_SPEED_STEPS 5

/* Per-axis movement/look intent, each component a signed magnitude in
 * [-1, +1], level-held (set from the live keyboard + pad every host
 * iteration, consumed once per game tick). Keyboard drives full ±1; an
 * analog stick drives proportionally, so the two sum (then clamp). Positive
 * senses are the intuitive ones; the game-side drive maps them onto the
 * camera's angle convention.
 *
 *   axis         keyboard          controller
 *   move_fwd     W / S             left stick Y
 *   move_strafe  D / A             left stick X
 *   move_up      Space / LShift    R1 / L1
 *   look_yaw     -> / <-             right stick X
 *   look_pitch   ↑ / ↓             right stick Y
 *
 * T (teleport the player to the camera) is not an axis - it's a one-shot
 * request; see openpete_freecam_request_teleport below.
 */
typedef struct openpete_freecam_axes {
    float move_fwd;     /* + forward,   - back   */
    float move_strafe;  /* + right,     - left   */
    float move_up;      /* + up,        - down   */
    float look_yaw;     /* + turn right, - turn left */
    float look_pitch;   /* + look up,    - look down */
} openpete_freecam_axes;

/** @brief Report whether the free camera is engaged.
 *
 * @return true while engaged. */
bool openpete_freecam_active(void);

/** @brief Toggle the free camera engaged or disengaged (the F-key edge).
 *
 * Each accepted toggle bumps the engage generation, zeroes the latched axes
 * so a key held across the toggle carries no stale intent, drops a pending
 * teleport request, and logs the new state. The off-to-on edge is refused
 * with a WARN while a bk2 movie is replaying or recording, since freezing
 * the player update would desync the movie from its inputs; the on-to-off
 * edge always passes.
 * @return false when the engage was refused, else true. */
bool openpete_freecam_toggle(void);

/** @brief Request the free camera engaged or disengaged from any thread; the host loop applies it at its camera site.
 *
 * The request names the wanted state, so a repeat before it is applied
 * changes nothing and a state already in force is a no-op when applied.
 * A later request replaces an earlier one. The applying toggle keeps every
 * rule openpete_freecam_toggle has, the bk2 refusal included.
 * @param on true to engage, false to disengage. */
void openpete_freecam_request_active(bool on);

/** @brief Take and clear the pending engage request.
 *
 * @return 1 for engage, 0 for disengage, -1 when none is pending. */
int openpete_freecam_take_active_request(void);

/** @brief Report the pending engage request without taking it.
 *
 * @return 1 for engage, 0 for disengage, -1 when none is pending. */
int openpete_freecam_pending_active(void);

/** @brief Request a speed step from any thread; the host loop applies it at its camera site.
 *
 * @param step The step, clamped to [0, OPENPETE_FREECAM_SPEED_STEPS) when applied. */
void openpete_freecam_request_speed(int step);

/** @brief Take and clear the pending speed step request.
 *
 * @return The requested step, or -1 when none is pending. */
int openpete_freecam_take_speed_request(void);

/** @brief Report whether a bk2 movie is replaying or recording.
 *
 * @return The flag the run loop last set through openpete_bk2_set_active. */
bool openpete_bk2_active(void);
/** @brief Record whether a bk2 movie is replaying or recording.
 *
 * Set by the run loop at movie setup; a scripted freecam engage clears it
 * around its own toggle to pass the refusal.
 * @param active true while a movie is replaying or recording. */
void openpete_bk2_set_active(bool active);

/** @brief Latch this host iteration's fly and look intent.
 *
 * Written by the host loop every iteration the free camera or camlock is
 * engaged (neutral while the menu owns the keyboard); a NULL argument
 * leaves the latch unchanged.
 * @param ax The intent to latch. */
void openpete_freecam_set_axes(const openpete_freecam_axes* ax);

/** @brief Copy out the latched fly and look intent.
 *
 * Read by the game-side freecam drive and by camlock; a NULL argument is
 * ignored.
 * @param out Receives the latched axes. */
void openpete_freecam_get_axes(openpete_freecam_axes* out);

/** @brief Add mouse displacement, in counts, to the look axes' running totals.
 *
 * Called by the host loop every iteration the freecam or camlock is
 * engaged. The totals are never rewritten: each consumer keeps the total it
 * last applied and takes the difference, so a slow drag below one unit per
 * tick still adds up and a speculated or replayed tick applies nothing
 * twice. Atomic, relaxed.
 * @param dx Horizontal counts to add.
 * @param dy Vertical counts to add. */
void openpete_freecam_add_look_counts(long long dx, long long dy);
/** @brief Read the look axes' running mouse totals, in counts.
 *
 * @param x Receives the horizontal total; may be NULL.
 * @param y Receives the vertical total; may be NULL. */
void openpete_freecam_look_counts(long long* x, long long* y);

/* Mouse look scale: angle units (a full turn is 4096) per count at
 * sensitivity 1.0. 0.5 is one full turn per 8192 counts, about 26 cm at
 * 800 counts per inch. A consumer applies
 * round(OPENPETE_FREECAM_MOUSE_K * sensitivity * (total - base)). */
#define OPENPETE_FREECAM_MOUSE_K 0.5

/** @brief Set the live mouse-look sensitivity (--mouse-sensitivity, mouse_sensitivity).
 *
 * Consumers read it at every apply and rebase when it changed, so a change
 * never rotates retroactively. A negative or NaN value is ignored. Atomic,
 * relaxed.
 * @param s The multiplier on OPENPETE_FREECAM_MOUSE_K; default 1.0. */
void  openpete_freecam_set_mouse_sensitivity(float s);
/** @brief Read the live mouse-look sensitivity.
 *
 * @return The multiplier last set, 1.0 by default. */
float openpete_freecam_mouse_sensitivity(void);

/** @brief Read the engage generation, which every accepted toggle bumps.
 *
 * A consumer compares it at its own apply site and rebases its mouse
 * markers when it differs, so an engage from any thread (key binding, O menu
 * row, M overlay row, scripted pose) starts the mouse term from the totals
 * as they stand then. Atomic, relaxed.
 * @return The generation counter. */
unsigned openpete_freecam_engage_generation(void);

/** @brief Request that the player be moved to the free camera's position (the T-key edge).
 *
 * Ignored while disengaged; callable from any thread. The host loop or the
 * dev overlay latches the request; the game side takes it on the next tick, since it needs the game fiber to read the
 * camera and write the player, and disengages the free camera as part of
 * applying it. A toggle before the request is taken drops it. */
void openpete_freecam_request_teleport(void);

/** @brief Take and clear the pending teleport request.
 *
 * @return true once per request, else false. */
bool openpete_freecam_take_teleport_request(void);

/** @brief Read the current speed step.
 *
 * Starts at 2, the middle of the five steps.
 * @return The step, clamped to [0, OPENPETE_FREECAM_SPEED_STEPS). */
int openpete_freecam_speed_index(void);

/** @brief Step the speed one step up or down, clamped to the step range.
 *
 * Logs the new step when it changed.
 * @param dir Positive steps up, negative steps down, 0 does nothing. */
void openpete_freecam_adjust_speed(int dir);

/** @brief Set the speed step, clamped to the step range.
 *
 * Logs the new step when it changed.
 * @param step The step. */
void openpete_freecam_set_speed(int step);
