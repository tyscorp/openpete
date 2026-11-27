/* orchestrator.h - the interface a game's sub-tick re-extract orchestrator
 * provides to the shared renderer translation units.
 *
 * The entries below are forwarders (renderer/orchestrator.c) over the
 * running game's game_orchestrator_t, which its runtime descriptor carries
 * (game_host_desc.h). A game that runs sub-tick re-extraction fills the
 * slots; a slot it leaves NULL, and every slot of a game with no
 * orchestrator, reports "no bracket active" and makes each commit a no-op.
 * A shared TU therefore calls them unconditionally and falls back to
 * canonical RAM on its own. */

#ifndef OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H
#define OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H

#include <stdint.h>
#include <stdio.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */
#include "openpete_time.h"           /* op_time_t */

struct CPUState;

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Returns the unrounded sub-tick interpolated position of a camera, player or moby.
 *
 * Feeders use it for camera- and object-relative deltas instead of the
 * integer position in RAM, so the projection follows the sub-tick path.
 * Outside a sub-tick re-extract (the canonical render) it reports no
 * entry and the feeder reads RAM.
 * @param key The object's guest address.
 * @param out Receives the position, world units.
 * @return 1 when a bracket holds a position for key this present, else 0. */
int openpete_native_interp_pos(unsigned int key, double out[3]);

/** @brief Latches the in-flight canonical-render epoch as fresh and starts the next.
 *
 * render_paths_commit calls it once per canonical present.
 * Producer-freshness queries compare a stored stamp against the latched
 * epoch. The sub-tick implementations also close the player hooks' record epoch
 * and run the cut-guard locator census when it is enabled. */
void spyro_native_canon_epoch_commit(void);

/** @brief Moves the in-flight canonical-render epoch past the captures of a render no canonical present showed.
 *
 * render_paths_superseded calls it when the game's render at a newer VBL
 * first captures or opens a scene while its previous scene never reached a
 * canonical present. Captures stamped before
 * the call no longer match the epoch the next commit latches, so the
 * sub-tick re-extracts that follow that commit replay only what the newest
 * render captured. The latched epoch is unchanged until that commit. A
 * game with no sub-tick spine does nothing. */
void spyro_native_canon_epoch_supersede(void);

/** @brief Decide what a canonical present with no game render since the last one shows: the presented frame held, or an empty frame.
 *
 * Called by render_paths_commit before it reads the repeat verdict. A no-op
 * unless openpete_native_scene_canon_repeat reports 1. The env is the
 * current draw buffer's DRAWENV (as cyclorama_native_env_clear_commit reads
 * it) unless game_layout.db.repeat_site has a row for the VSync wait the
 * game is parked in (openpete_yield_site.ra): REPEAT_SITE_HOLD holds,
 * with the env the last canonical present cleared under, REPEAT_SITE_CLEAR_BLACK takes isbg 1 and black, REPEAT_SITE_CLEAR_DB takes
 * isbg 1 and the buffer's colour; a row's env is what
 * cyclorama_native_present_env reports for this present. Without a row,
 * an env with isbg 1 whose buffer's DISPENV (game_layout.db.dispenv) shows
 * a draw area last drawn with content (openpete_native_shown_area_content)
 * becomes that env with isbg 0 and an empty scene: the screen keeps the
 * displayed image uncleared. OPENPETE_REPEAT_CENSUS=1 logs each repeat
 * present's site, env, shown area and verdict; OPENPETE_REPEAT_SHOWN=0
 * disables the shown-area test (negative control). With isbg 1 the
 * present becomes an empty scene (openpete_native_scene_open_empty) cleared
 * to that colour; with isbg 0, without a valid g_CurDB, or on a HOLD row it
 * stays a repeat. Defined by the shared cyclorama feeder for every game. */
void cyclorama_native_repeat_resolve(void) OP_REQUIRES(sim);

/** @brief Report the DRAWENV the scene being presented is cleared with: isbg, r0, g0, b0.
 *
 * In order: the env of a DrawOTag that already submitted the fed scene
 * (openpete_native_drawn_env), the env a repeat-site row set for this
 * present (cyclorama_native_repeat_resolve), else the DRAWENV g_CurDB
 * points at (game_layout.db.cur_ptr; isbg at +0x18, r0/g0/b0 at
 * +0x19..+0x1B) read through the sub-tick view.
 * @param env Receives isbg, r0, g0, b0 when the function returns 1.
 * @return 1 when one of the three names the env, else 0 (no g_CurDB located
 *         or it points at neither draw buffer). */
int cyclorama_native_present_env(uint8_t env[4]) OP_REQUIRES_SHARED(sim);

/** @brief Set the native clear colour from the DRAWENV the game will apply to the scene being presented.
 *
 * Called once per canonical present by render_paths_commit, with the env
 * cyclorama_native_present_env reports. When a DrawOTag
 * since the last canonical capture already submitted the fed scene (a game
 * that draws before its VSync, such as Spyro 1 GS_ExitLevel or a loader
 * loop), the env is the PutDrawEnv in effect at that DrawOTag
 * (openpete_native_drawn_env). Otherwise the game flips
 * g_CurDB at the top of its draw pass, writes the clear into
 * g_CurDB->m_DrawEnv, builds the OT, then waits a VSync (during which the host
 * presents the fed scene) before PutDrawEnv and DrawOTag; a PutDrawEnv latch
 * therefore trails the presented scene by one VSync. Every gamestate writes
 * its clear into that struct before building the OT. The env's isbg and
 * r0/g0/b0 go to openpete_native_set_clear_color; without an env (no valid
 * g_CurDB) the PutDrawEnv latch stays in effect. The same DRAWENV's clip
 * rect and isbg go to openpete_native_note_present_draw_area (no area
 * without a valid g_CurDB). Defined by the shared cyclorama feeder
 * (games/common/renderer/cyclorama.c) for every game. */
void cyclorama_native_env_clear_commit(void) OP_REQUIRES(sim);

/** @brief Bracket a held camera cut's re-extract: its scene shows tick N-1's draw-env clear and its feeders read tick N-1's m_BackgroundColor.
 *
 * Both are the values the commit recorded for tick N-1's render.
 * @param hold Nonzero to open the bracket, 0 to close it. */
void cyclorama_native_hold_background(int hold) OP_REQUIRES(sim);

/** @brief g_Cyclorama.m_BackgroundColor as the present being built shows it.
 *
 * Tick N-1's value inside a held camera cut's bracket
 * (cyclorama_native_hold_background), else guest RAM.
 * @param cpu The guest CPU state.
 * @return The word, 0x00BBGGRR. */
uint32_t cyclorama_native_bg_word(struct CPUState* cpu);

/* Canonical-render epoch. A feeder that snapshots transient render-pass
 * scratch stamps the epoch at capture time and replays the capture on the
 * sub-tick presents that follow the canonical present it was taken during.
 *   _stamp:  the value a producer hook stores at capture time.
 *   _fresh:  whether a stored stamp belongs to the last canonical present.
 * Per-producer freshness for the three canonical moby producers, which share
 * one render-path bit (a load or pause screen runs only the third). A game
 * with no sub-tick spine reports never-fresh, so every replay is skipped. */
/** @brief Returns the in-flight canonical-render epoch, the value a producer hook stores at capture time.
 *
 * On the game's render, the call first supersedes a pending unpresented
 * scene of an older VBL (openpete_native_scene_note_game_capture), as the
 * spyro_native_canon_capture_* entries do, so a capture taken before its
 * render's first feed carries the render's epoch.
 * @return The epoch; always 0 for a game with no sub-tick spine. */
uint32_t spyro_native_canon_epoch_stamp(void) OP_REQUIRES(sim);
/** @brief Reports whether a capture stamp belongs to the last closed canonical render.
 *
 * @param stamp A value from spyro_native_canon_epoch_stamp taken at capture.
 * @return 1 when stamp equals the last closed epoch; always 0 for a game
 *         with no sub-tick spine. */
int      spyro_native_canon_epoch_fresh(uint32_t stamp);
/** @brief Reports whether the moby cull-record capture may be replayed on the present in flight.
 *
 * Spyro 1 tests that its moby-record producer stamped the last closed
 * epoch. Spyro 2 tests that the record queue is staged over its own address
 * for the present. Spyro 3 requires both.
 * @return 1 when the capture is replayable, else 0. */
int      spyro_native_canon_mobyreg_fresh(void);
/** @brief Reports whether the shaded-moby producer's capture is from the last closed canonical render.
 *
 * Spyro 1 tests its shaded producer's epoch against the last closed epoch.
 * Spyro 2 and 3 have no second material pass and return 0.
 * @return 1 when fresh, else 0. */
int      spyro_native_canon_shaded_fresh(void);

extern int openpete_speculative;   /* openpete_fiber.h - runahead lookahead */
/** @brief Whether a canonical latch may be written at the producer hook running now.
 *
 * Runahead speculates 2N VBLs past the committed tick and rolls the guest
 * state back afterwards, and the producer hooks fire during those VBLs as
 * well. A capture taken then holds a speculative tick's render scratch,
 * addressed off that tick's primitive-buffer end, and stamps the epoch the
 * next canonical present latches; the replay would stage one tick's bytes
 * over another tick's address and decode them as geometry. The committed
 * tick's fire is the canonical capture, so every speculative fire is
 * skipped.
 * @return 1 outside a runahead lookahead, 0 during one. */
static inline int spyro_native_canon_capture_open(void) {
    return !openpete_speculative;
}

/* Canonical-scratch capture, called from a canonical render hook while the
 * transient render-pass scratch is still live.
 *   _recs:  the whole cull-record region, captured before a later pass reuses
 *           the overlapping bytes. */
/** @brief Captures a sub-range of the render scratch block at its producer hook for sub-tick replay.
 *
 * Called from a canonical render hook while the transient render-pass
 * scratch is live; the capture is stamped with the in-flight epoch.
 * @param cpu The guest CPU state.
 * @param off The sub-range's offset into the game's render scratch block.
 * @param len The sub-range's length in bytes. */
void spyro_native_canon_capture_range(void* cpu, unsigned off, unsigned len) OP_REQUIRES(sim);
/** @brief Captures the moby cull-record region at the canonical moby hook, while the records are live.
 *
 * The capture is stamped with the in-flight epoch; a region that does not
 * map for its length captures nothing.
 * @param cpu The guest CPU state. */
void spyro_native_canon_capture_recs(void* cpu) OP_REQUIRES(sim);

/** @brief The regular cull records of tick N-1, for a game that keeps them in its scratch capture.
 *
 * @param len Receives the bytes' length.
 * @param ref_ns Receives the tick stamp of their capture: a state stamped later rolled after they were composed.
 * @return The bytes, or NULL when no retired capture is tick N-1's. */
const uint8_t* spyro_native_canon_prev_recs(uint32_t* len, op_time_t* ref_ns);
/** @brief Captures the block where the player draw left every projected vertex's int16 screen position.
 *
 * The block is also the walk's cull verdict. Called from the canonical
 * player hook; a game whose player walk does not read the block back
 * captures nothing.
 * @param cpu The guest CPU state. */
void spyro_native_canon_capture_sxy(void* cpu) OP_REQUIRES(sim);

/* The camera a producer drew under. A feeder re-derives its geometry from the
 * camera, which the sub-tick bracket interpolates between tick-end snapshots;
 * a producer whose draw ran under matrices the tick did not end with must be
 * re-derived from its own camera.
 *   _capture_camera: at the producer hook, record the camera transform block.
 *   _prod_camera_enter/_leave: around that producer's feeder in the
 *   re-extract; enter installs the captured camera when it differs from the
 *   tick-end camera (returns 1), leave restores. */
enum { SPYRO_PROD_MOBYREG = 0, SPYRO_PROD_SPYRO = 1, SPYRO_PROD_SHADED = 2 };
/** @brief Records the camera transform block a producer drew under, at its hook.
 *
 * @param cpu Unused.
 * @param producer SPYRO_PROD_MOBYREG, SPYRO_PROD_SPYRO or SPYRO_PROD_SHADED;
 *        other values are ignored. */
void spyro_native_canon_capture_camera(void* cpu, int producer) OP_REQUIRES(sim);
/** @brief Installs a producer's captured camera over the re-extract's camera when the producer drew under another.
 *
 * Spyro 1 installs it when the capture is from the last closed epoch and
 * differed from the tick-end camera, and no bracket is already open.
 * Spyro 2 and 3 install nothing.
 * @param producer SPYRO_PROD_MOBYREG, SPYRO_PROD_SPYRO or SPYRO_PROD_SHADED.
 * @return 1 when a camera was installed and spyro_native_prod_camera_leave
 *         must follow, else 0. */
int  spyro_native_prod_camera_enter(int producer);
/** @brief Restores the camera spyro_native_prod_camera_enter replaced, or does nothing when none was installed. */
void spyro_native_prod_camera_leave(void);

/** @brief Registers the game's interp region table and per-object capture with the interp ring.
 *
 * Runs the game's interp_regions TU's registration, or nothing for a game
 * with no sub-tick presentation. main() calls it once before
 * openpete_run(), so openpete_interp_init() sees the regions. An explicit
 * call replaces a weak-symbol override, which the PE/COFF linker
 * mis-resolves. */
void game_interp_register(void);
/** @brief Registers the game's sub-tick re-extract body with the native renderer.
 *
 * The game's reextract TU passes its body to game_reextract_view_install;
 * a game with no orchestrator registers nothing. main() calls it beside
 * game_interp_register, as an explicit call for the same reason. */
void game_reextract_init(void);

/* One game's orchestrator: the implementation behind each entry above but
 * game_interp_register. A NULL slot takes the inert entry. */
typedef struct game_orchestrator {
    void     (*reextract_init)(void);
    int      (*interp_pos)(unsigned int key, double out[3]);
    void     (*epoch_commit)(void);
    void     (*epoch_supersede)(void);
    uint32_t (*epoch_stamp)(void);
    int      (*epoch_fresh)(uint32_t stamp);
    int      (*mobyreg_fresh)(void);
    int      (*shaded_fresh)(void);
    void     (*capture_range)(void* cpu, unsigned off, unsigned len);
    void     (*capture_recs)(void* cpu);
    void     (*capture_sxy)(void* cpu);
    void     (*capture_camera)(void* cpu, int producer);
    const uint8_t* (*prev_recs)(uint32_t* len, op_time_t* ref_ns);
    int      (*prod_camera_enter)(int producer);
    void     (*prod_camera_leave)(void);
} game_orchestrator_t;

/** @brief Bind the running game's orchestrator and interp registration behind the entries above.
 *
 * game_runtime_install calls it at each install, before any entry runs.
 * Every slot and the interp registration are first reset to the inert
 * entries, so a slot the game leaves NULL holds the inert entry whatever an
 * earlier install bound.
 * @param o The game's orchestrator, or NULL for none.
 * @param interp_register The game's interp registration, or NULL for none. */
void game_orchestrator_bind(const game_orchestrator_t* o, void (*interp_register)(void));

/** @brief Write each bound orchestrator slot's address and the interp registration's, one `orchestrator.SLOT ADDRESS` line each.
 *
 * The install dump (OPENPETE_INSTALL_DUMP, game_build.h) compares these
 * between a process that installed several games and one that installed
 * the target alone.
 * @param out The stream. */
void game_orchestrator_dump(FILE* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_ORCHESTRATOR_H */
