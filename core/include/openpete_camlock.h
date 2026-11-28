#pragma once
/* openpete_camlock.h - host-side state for the render camera lock (G key).
 *
 * The lock is the PRESENTATION-side sibling of the gameplay freecam (F key).
 * The freecam freezes the game's camera-follow / player-update functions and
 * writes g_Camera on the game fiber - an intentional gameplay-state
 * divergence, and inert during cutscenes (whose cameras never run the
 * follow function it intercepts). The lock instead overrides the camera
 * INSIDE the sub-tick re-extract bracket: the bytes are saved, overwritten
 * for extraction only, and restored before the sim resumes, so gameplay
 * state is byte-identical whether the lock is on or off (the bracket-
 * integrity self-check covers the writes). That makes it the tool for
 * render-only inspection: a cutscene or flyby keeps playing underneath
 * while the view holds still or flies free - e.g. parking a close-up on an
 * animated moby to judge sub-pixel flicker that is invisible from the
 * authored camera distance.
 *
 * This is the GENERIC half: the on/off toggle, the flown pose (world
 * position + game angle units, 4096 per turn), and pose integration from
 * the freecam's shared input axes (WASD/sticks - one set of fly controls
 * for both cameras; run.c keeps the two mutually exclusive). The
 * game-specific half - latching the initial pose from the live camera and
 * writing the bracketed g_Camera bytes/matrices - lives in the game's
 * re-extract orchestrator.
 *
 * Scripted use: OPENPETE_CAMLOCK=1 engages at the first present;
 * OPENPETE_CAMLOCK_POSE="x,y,z,yaw,pitch" engages AND flies to that pose
 * (the numbers round-trip through the change-gated "camlock:" log line),
 * so a headless capture reproduces an eyeballed view exactly.
 */

#include <stdbool.h>

/** @brief Report whether the render camera lock is engaged.
 *
 * The first call reads OPENPETE_CAMLOCK and OPENPETE_CAMLOCK_POSE, either of
 * which engages the lock.
 * @return true while engaged. */
bool openpete_camlock_active(void);

/** @brief Engage or disengage the render camera lock, the G-key edge.
 *
 * Bumps the engage generation. Engaging clears the pose and moved flags, so
 * the game side's first apply latches the pose from the live camera.
 * Disengaging only stops the override; the re-extract bracket restore
 * already returns every present's camera bytes. */
void openpete_camlock_toggle(void);

/** @brief Request the lock engaged or disengaged from any thread; the host loop applies it at its camera site.
 *
 * The request names the wanted state; a later request replaces an earlier
 * one. An engage applied while the free camera is engaged disengages the
 * free camera first, as the G key does.
 * @param on true to engage, false to disengage. */
void openpete_camlock_request(bool on);

/** @brief Take and clear the pending lock request.
 *
 * @return 1 for engage, 0 for disengage, -1 when none is pending. */
int openpete_camlock_take_request(void);

/** @brief Return the engage generation, bumped by every toggle and by the environment engage.
 *
 * openpete_camlock_integrate compares it with the last generation it saw to
 * rebase its mouse-look markers on an engage.
 * @return The generation count. */
unsigned openpete_camlock_engage_generation(void);

/** @brief Report whether the flown pose holds real values.
 *
 * Set by a scripted pose or by openpete_camlock_set_pose (the game side's
 * latch from the live camera); cleared on each engage. While false the game
 * side derives a pose before flying, and integration does nothing.
 * @return true once the pose is valid. */
bool openpete_camlock_pose_valid(void);

/** @brief Set the flown pose and mark it valid.
 *
 * Position is world units (game ints, double for sub-unit fly precision);
 * yaw and pitch are game angle units (4096 = full turn).
 * @param x World x.
 * @param y World y.
 * @param z World z.
 * @param yaw Yaw in game angle units.
 * @param pitch Pitch in game angle units. */
void openpete_camlock_set_pose(double x, double y, double z,
                               double yaw, double pitch);
/** @brief Read the flown pose, in the units openpete_camlock_set_pose takes.
 *
 * @param x Receives world x; may be NULL.
 * @param y Receives world y; may be NULL.
 * @param z Receives world z; may be NULL.
 * @param yaw Receives yaw in game angle units; may be NULL.
 * @param pitch Receives pitch in game angle units; may be NULL. */
void openpete_camlock_get_pose(double* x, double* y, double* z,
                               double* yaw, double* pitch);

/** @brief Report whether fly input or a scripted pose has moved the pose since the last engage.
 *
 * Until then the game side holds the latched camera bytes verbatim, so an
 * engage with no input holds the on-screen view exactly.
 * @return true once moved. */
bool openpete_camlock_moved(void);

/** @brief Advance the pose from the freecam's shared fly axes and mouse-look counts.
 *
 * Does nothing while disengaged or before the pose is valid. Stick and key
 * axes scale by dt_ticks and the freecam speed step's per-tick move and turn
 * rates, so fly speed matches the freecam at any present rate; the mouse
 * term is a displacement and does not scale. Pitch clamps at +/-992 (0x3E0).
 * Any input sets the moved flag.
 * @param dt_ticks Elapsed time since the previous call in sim ticks
 *        (29.913 Hz), clamped to [0, 4] so a stall does not warp the
 *        pose. */
void openpete_camlock_integrate(double dt_ticks);
