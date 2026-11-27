/* Target captures of the host loop: the screenshot legs a run writes at the
 * frames, ticks and presents its --screenshot-* options name.
 *
 * The host loop calls these at fixed points of a present. Each leg keeps its
 * own fired and armed state here, so a target is written once, and the
 * present-sequence capture counts its presents across the canonical and the
 * intermediate calls. */
#pragma once

#include "openpete.h"

#include <stdbool.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Record that PsyCross rasterized an OT into the scene the next present stores.
 *
 * Called by the DrawOTag thunk after its PsyCross walk. Stamps the native
 * scene id of that OT (openpete_native_otag_scene), the identity by which
 * the -og leg pairs a PsyCross present with a native scene, and the game
 * scene opens so far (openpete_native_game_scene_opens), by which the leg
 * refuses an OT whose scene a later render superseded. */
void openpete_capture_note_psyx_walk(void) OP_REQUIRES_SHARED(sim);

/** @brief Write the capture legs read from PsyCross at a canonical present, before the native present step.
 *
 * The legs are the -og reference of each --screenshot-on-frame target, taken
 * from the PsyCross present that rasterized the OT of the native scene the
 * keyed present shows (the keyed present or a later one); the OPENPETE_VRAM_DUMP
 * write of PsyCross's VRAM mirror; and the canonical present of
 * --screenshot-presents-start/count when no native scene exists.
 * @param opts Session options.
 * @param shot_key The present's capture key: the canonical tick under
 *        --screenshot-on-tick, else the feed key.
 * @return true when this present owes the native canonical leg of the
 *         present sequence, which openpete_capture_canon_native writes. */
bool openpete_capture_canon_reference(const openpete_options_t *opts, int shot_key) OP_REQUIRES(sim);

/** @brief Start a canonical present's capture state, before the present's device services.
 *
 * A headless capture run keeps the newest freeze-source scene instead of
 * rendering it; when this present is not a source, the kept scene renders
 * here, while the device still holds the state it was drawn with.
 * @param opts Session options. */
void openpete_capture_present_begin(const openpete_options_t *opts) OP_REQUIRES(sim);

/** @brief Write the native capture legs of a canonical present, after the present's device services.
 *
 * The legs are the owed native canonical present of the present sequence,
 * the capture freeze backdrop source of a headless run, and the -vk dump of
 * each --screenshot-on-frame target reached. In PsyCross display
 * mode the -vk dump reads PsyCross's present FBO and takes the same
 * one-present arm as the -og leg.
 * @param opts Session options.
 * @param shot_key The present's capture key, as for openpete_capture_canon_reference.
 * @param presents_owed The value openpete_capture_canon_reference returned. */
void openpete_capture_canon_native(const openpete_options_t *opts, int shot_key,
                                   bool presents_owed) OP_REQUIRES(sim);

/** @brief Write the intermediate capture of each --screenshot-intermediate-on-frame target reached.
 *
 * Each target re-extracts the scene at a sub-tick phase of the newest tick
 * (OPENPETE_INTERMEDIATE_ALPHA, default 0.5; 1 is the identity instant),
 * after an uncaptured re-extract at each phase OPENPETE_INTERMEDIATE_PRE_ALPHA
 * lists, and composes it at depth 0. Fires on the first present at or after the
 * target's feed key.
 * @param opts Session options.
 * @param cpu The guest CPU the re-extract reads. */
void openpete_capture_canon_intermediate(const openpete_options_t *opts, void *cpu) OP_REQUIRES(sim);

/** @brief Write the native intermediate present of --screenshot-presents-start/count while the sequence is open.
 *
 * The present reads the native offscreen at the window extent, the image
 * the sub-tick present just drew.
 * @param opts Session options. */
void openpete_capture_subtick_present(const openpete_options_t *opts);

/** @brief Log the present sequence's engagement when --screenshot-presents-start was given.
 *
 * A sequence with no native intermediate present exercised only the
 * canonical leg and is reported vacuous; a sequence written at two extents
 * is reported mixed.
 * @param opts Session options. */
void openpete_capture_report_presents(const openpete_options_t *opts);

/** @brief Log how many intermediate capture legs and uncaptured pre-alpha re-extracts ran and how many the re-extract bracketed. */
void openpete_capture_report_intermediate(void);

#ifdef __cplusplus
}
#endif
