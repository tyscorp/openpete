/* terrain.h - the shared terrain feeder's exports to a game's orchestrator.
 * The feeder itself (renderer/terrain.c) registers as the feature module
 * `terrain_module`; these entries are called by the sub-tick re-extract
 * orchestrator and the debug-camera pose helpers. */
#ifndef OPENPETE_COMMON_RENDERER_TERRAIN_H
#define OPENPETE_COMMON_RENDERER_TERRAIN_H
#include <stddef.h>
#include <stdint.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */
#ifdef __cplusplus
extern "C" {
#endif
struct CPUState;

/** @brief Rebuilds the terrain scene for a sub-tick present from the current gamestate and the camera the orchestrator installed.
 *
 * Extract only: the world draw (func_800258F0) is not called and no verdict
 * is written; the walk reuses the canonical tick's occlusion group. Returns
 * without effect when the native scene or the terrain render path is off,
 * or when the canonical present did not draw terrain (during a level
 * transition the game stops drawing the world and the level data in RAM is
 * stale). Static terrain moves with the interpolated camera alone; poly-anim
 * sectors, whose vertices the game rewrites once per tick, lerp their
 * previous and current decoded banks at `alpha` so they move every present.
 * OPENPETE_TERRAIN_VANIM=0 and OPENPETE_TERRAIN_TANIM=0 snap the vertex and
 * UV-scroll animation to the current tick; OPENPETE_TERRAIN_CANIM=0 holds
 * the environment-animation colours at the previous tick.
 * @param cpu guest state, the sub-tick camera installed
 * @param alpha sub-tick fraction between the previous and current tick
 * @param interp_on 1 = the camera is bracketed; 0 snaps the vertex animation
 *        to the current tick
 * @param hold nonzero on a held camera cut: a tick N that stopped drawing the
 *        world still reproduces tick N-1's when terrain_game.world_outlives_draw
 *        says the world is whole */
void terrain_reextract(struct CPUState* cpu, double alpha, int interp_on, int hold) OP_REQUIRES(sim);
/** @brief Whether a held camera cut cannot show tick N-1's world: that tick drew it, tick N did not and terrain_game.world_outlives_draw refuses.
 *
 * @param cpu guest state
 * @return 1 when the world is lost to the held present, else 0 */
int terrain_native_held_world_lost(struct CPUState* cpu);

/** @brief Re-reads the heavy-fog verdict when the loaded level's sector table changes.
 *
 * The terrain walk and the moby passes call it before they read the verdict.
 * @param cpu The guest CPU state, read only. */
void terrain_heavy_fog_refresh(struct CPUState* cpu);

/** @brief Whether the loaded level is a heavy-fog level: its HP depth cue pulls vertex colours into one far colour.
 *
 * Set by terrain_heavy_fog_refresh from every sector's near and far colour
 * pairs at the level's first walk or moby pass: heavy when in at least 45%
 * of the pairs the far colour lies within 24 of the per-channel far median
 * on every channel and the near colour does not (S1 misty_bog and
 * beast_makers, S2 mystic_marsh and shady_oasis, whose far palette is one
 * colour, S3 harbor_speedway, S1 jacques, and S3 spider_town, bugbot_factory
 * and dino_mines area 2).
 * @return 1 for a heavy-fog level, 0 otherwise or before the first refresh. */
int terrain_heavy_fog(void);

/** @brief The heavy-fog level's fog colour: the per-channel median of its far colours, 0x00BBGGRR.
 *
 * Valid while terrain_heavy_fog() is 1; 0 before the first refresh.
 * @return The colour. */
uint32_t terrain_heavy_fog_colour(void);

/** @brief Whether the heavy-fog level's far palette is one colour: at least 75% of its far colours within 24 of the median.
 *
 * Read off the same scan as terrain_heavy_fog. On such a level a surface past
 * full fog draws the median colour within that spread whatever it is, so a
 * fully fogged moby is indistinguishable from what lies behind it (S1
 * misty_bog and beast_makers, S2 mystic_marsh and shady_oasis, S3
 * harbor_speedway); the mixed-haze heavy-fog levels score below.
 * @return 1 for a one-colour heavy-fog level, 0 otherwise. */
int terrain_heavy_fog_one_colour(void);

/** @brief Whether terrain_fog_cue is saturated at a view depth: IR0 = fogRef - (int)(m3 * fog scale) <= 0.
 *
 * fogRef is the last walk's reference, as terrain_fog_cue reads it.
 * Saturated means terrain_fog_cue returns the far colour. Never saturated
 * before the first walk or under fog mode off.
 * @param m3 The view-Z in world/16.
 * @return 1 when saturated. */
int terrain_fog_saturated(double m3);

/** @brief Blends a colour toward a far colour on the terrain's vertex depth-cue curve.
 *
 * The HP walk's per-corner fog (terrain_fog_blend): IR0 = fogRef - SZ with SZ
 * = m3 * the walk's fog scale (4 / --fog-scale) and fogRef the last walk's
 * reference (LOD distance >> 2), the GTE DPCS blend near at IR0 >= 4096, far
 * at IR0 <= 0. Returns nearC unchanged before the first terrain walk.
 * @param nearC The colour at zero fog, 0x00BBGGRR.
 * @param farC The fog target, 0x00BBGGRR.
 * @param m3 The view-Z in world/16.
 * @return The blended colour, 0x00BBGGRR. */
uint32_t terrain_fog_cue(uint32_t nearC, uint32_t farC, double m3);

/** @brief The colour terrain_fog_cue blends toward for a far colour: farC after the texture pack's fog shift.
 * @param farC The fog target, 0x00BBGGRR.
 * @param target Receives the colour the cue blends toward, 0x00BBGGRR.
 * @return 1 when the cue is active; 0 when terrain_fog_cue returns its near colour at every depth (before the first walk, or under fog mode off), and nothing is written. */
int terrain_fog_cue_target(uint32_t farC, uint32_t* target);

/** @brief Packs the heavy-fog texel fade stepped at one ladder row, the original fog form's per-face step.
 *
 * The game steps its CLUT ladder per face; the step is t = row / 15,
 * pre-weighted into the affine aN = 1 - t, bN = 0.5 t on every channel
 * (word b bit 30, psx_hq_fog.glsl), rounded to the field's 1/256 units in
 * integers, so the stage draws texel * aN + bN at every pixel. terrain.vert
 * flat_atmo_words builds the same words.
 * @param row The ladder row, clamped to 0..15.
 * @param w Receives words a and b. */
void terrain_fog_step_words(int row, uint32_t w[2]);

/** @brief A freecam pose that frames the whole live level from above.
 *
 * The first pass takes the vertex bounds of every sector over both LOD banks.
 * The camera sits over the bounds' centre at yaw 0 and pitch 992 (the freecam
 * drive clamp, about 3 degrees short of straight down); the second pass sets
 * its height to the lowest at which every vertex projects inside the frame at
 * the canonical H, plus 2%. The height is clamped to 30000 (world/16), under
 * the view-Z at which the game's 16-bit frustum stage rejects every sector, so
 * a level too wide for that height is cropped at the edges. A table outside
 * main RAM, or an entry naming no record wholly inside it (a table mid-load),
 * is skipped, as in the sweep poses. Resolves the named pose `topdown` of
 * `--freecam-pose` and `--freecam-path`.
 * @param cpu guest state holding the live sector table
 * @param v   receives {x, y, z, yaw, pitch}, the position in world units
 * @return 1 on success, 0 when no sector table is loaded or it holds no vertex */
int terrain_native_topdown_pose(struct CPUState* cpu, int32_t v[5]);

/** @brief Waypoint `corner` of a low-altitude rectangular circuit of the live level.
 *
 * Takes the vertex bounds of terrain_native_topdown_pose's first pass. The
 * waypoints are the corners of a rectangle centred on the bounds with 30% of
 * their half-extents, in the order (-x,-y), (+x,-y), (+x,+y), (-x,+y); the
 * camera rides 64 (world/16) above the highest vertex at pitch 0, yawed toward
 * the next waypoint. `corner` is taken modulo 4. Flying the circuit reveals the
 * level progressively, which exercises sector-cache eviction and LOD
 * crossings. Resolves the named poses `sweepN` of `--freecam-pose` and
 * `--freecam-path`.
 * @param cpu    guest state holding the live sector table
 * @param corner the waypoint index, taken modulo 4
 * @param v      receives {x, y, z, yaw, pitch}, the position in world units
 * @return 1 on success, 0 when no sector table is loaded or it holds no vertex */
int terrain_native_sweep_pose(struct CPUState* cpu, int corner, int32_t v[5]);

/** @brief Arms the capture of the sector visibility table for the next world draw.
 *
 * On a game whose world draw computes the table in the scratchpad and
 * consumes it before returning (Spyro 3), the feeder's visibility hook
 * copies the bytes once the visibility function returns. Arming clears the
 * previous capture; on a game with no visibility hook it arms nothing and
 * the verdict stays in RAM at game_layout.visibility. Arm before calling the
 * world draw, then read terrain_visibility_capture after. */
void terrain_visibility_capture_arm(void);

/** @brief Run the canonical native terrain build and feed the world draw deferred for the portal capture.
 *
 * The deferral is terrain_game.env_feed_waits_for_portals; a call with none
 * pending is a no-op. The portal draw calls it once its capture for the tick is
 * complete. */
void terrain_native_flush_deferred(void) OP_REQUIRES(sim);
/** @brief Run a deferred terrain build and feed whose portal draw never released it.
 *
 * The world draw's hook calls it before the next draw and the canonical
 * commit calls it before the scene presents, so a held feed still reaches
 * the scene of its VBL. A pending feed at either point is never expected and
 * is counted as late in the deferral census; a call with none pending is a
 * no-op. */
void terrain_native_flush_deferred_late(void) OP_REQUIRES(sim);
/** @brief Returns the sector visibility table captured during the last armed world draw.
 * @param out receives the byte array (nonzero = the sector is visible); may
 *        be NULL
 * @return the table's length, or -1 when no capture was taken this draw, in
 *         which case the verdict is in RAM at game_layout.visibility */
int  terrain_visibility_capture(const uint8_t** out);

/* ---- contact candidates for the sunk-pickup snap (moby_snap.c) ---------- */

/* A pulled moby's search box: its PSX screen rectangle and view-Z range
 * (full-world units), widened by the caller. */
typedef struct { float x0, y0, x1, y1, z0, z1; } terrain_snap_box;
/* One drawn opaque terrain triangle a search box reaches: its corners as
 * drawn (PSX screen sx, sy and full-world view-Z), whether its face is
 * double-sided, how many triangles its face drew (2 flat, 8 the 2x2
 * quadrants, 16 a split triangle, 32 the 4x4 cells), its face's identity
 * (sector index, face index, tier: 1 LP, 0 HP), whether its sector moves
 * within the tick by its vertex animation (vanim: the build drew it from the
 * sub-tick lerp banks, or rolled banks that differ), and the boxes whose
 * rectangle and depth range it meets (bit k of hit[k >> 6]). */
typedef struct {
    float    sx[3], sy[3], vz[3];
    uint8_t  dside, ftris;
    int16_t  sec, face;
    uint8_t  tier, vanim;
    uint64_t hit[2];
} terrain_snap_tri;
/* What one capture cost: candidate faces, the triangles they drew, and the
 * sectors the retained cache owns that were walked for them. */
typedef struct { int faces, tris, sectors; } terrain_snap_census;

/** @brief Whether the last terrain build fed the scene now accumulating, so its triangles and walk inputs are the contact source of this present.
 * @return 1 when they are, else 0. */
int  terrain_snap_ready(void);
/** @brief Read the projection the last terrain build drew with.
 * @param H Receives the projection distance in PSX screen pixels.
 * @param ofx Receives the optical axis' screen X.
 * @param ofy Receives the optical axis' screen Y. */
void terrain_snap_view(float* H, float* ofx, float* ofy);
/** @brief Read the camera the last terrain build walked with, as the affine map from world to the snap's camera space.
 *
 * A world point w (full-world units: 16 times the walk's sector origin plus
 * vertex) lands at camera point c = L (w - P), 16 (m1, m2, m3) of the walk's
 * vertex transform: the full-world view-Z space terrain_snap_tri carries and
 * terrain_snap_view unprojects screen points into.
 * @param L Receives the linear part (the camera matrix over 4096, with the
 *        walk's axis order and signs).
 * @param P Receives the camera position, full-world units.
 * @return 1 when a build is latched, else 0 (L and P untouched). */
int  terrain_snap_camera(double L[3][3], double P[3]);
/** @brief Collect the drawn opaque terrain triangles that reach a set of search boxes.
 *
 * The walk's own run supplies the sectors it walked. A visible sector the
 * retained cache owns (drawn by the cache's vertex stage, which mirrors the
 * walk bit-exactly) is walked again by walk_sector into a private run, with
 * its faces masked to those whose corner rectangle and depth range meet a
 * box; nothing reaches the scene. Blended and hole-fill triangles are left
 * out: neither cuts a moby by depth.
 * @param b The boxes.
 * @param nb Box count, at most 128.
 * @param out Receives the triangles, valid until the next call.
 * @param cen Receives the cost counters; may be NULL.
 * @return The triangle count, or -1 when terrain_snap_ready does not hold. */
int  terrain_snap_capture(const terrain_snap_box* b, int nb, const terrain_snap_tri** out,
                          terrain_snap_census* cen);
/* A face a listed capture keeps: its level sector index, walk pass (0, 1, 3
 * or 4) and face index. */
typedef struct { int16_t sec; uint8_t pass; uint16_t face; } terrain_snap_fkey;
/** @brief terrain_snap_capture with the walk of cache-owned sectors limited to the boxes that need it.
 *
 * A cache-owned sector that a box of @p walk reaches is walked as
 * terrain_snap_capture walks it. One that only other boxes reach is walked
 * with its faces limited to the listed faces of that sector, each still
 * under the corner mask, and is not walked when none is listed. A box
 * outside @p walk therefore receives, from cache-owned sectors that no box
 * of @p walk reaches, only triangles of listed faces; the triangles of
 * sectors the last build walked reach every box as in terrain_snap_capture.
 * @param b The boxes.
 * @param nb Box count, at most 128.
 * @param walk The boxes that take the full walk, as bits (bit k of walk[k >> 6]).
 * @param fk The listed faces, in any order and with repeats.
 * @param nfk Their count.
 * @param out Receives the triangles, valid until the next call.
 * @param cen Receives the cost counters; may be NULL.
 * @return The triangle count, or -1 when terrain_snap_ready does not hold. */
int  terrain_snap_capture_listed(const terrain_snap_box* b, int nb, const uint64_t walk[2],
                                 const terrain_snap_fkey* fk, int nfk, const terrain_snap_tri** out,
                                 terrain_snap_census* cen);

/* A walk verdict of one terrain face: dropped (a gate or the back-face cull
 * removed it, or its pass or sector did not run), drawn flat, or drawn on
 * the near/HQ path. */
enum { TERRAIN_FACE_DROPPED = 0, TERRAIN_FACE_FLAT = 1, TERRAIN_FACE_NEAR = 2 };
/* A visible sector of the last build whose bounding sphere meets a world
 * box, and the boxes it meets (bit k of hit[k >> 6]). A list of them is in
 * ascending sector order. */
typedef struct { int16_t sec; uint64_t hit[2]; } terrain_snap_sector;
/* A face of such a sector whose corners' world box meets a world box: its
 * level sector index, walk pass (0, 1, 3 or 4: hole-fill passes are left
 * out) and face index, its verdict under the build's camera
 * (TERRAIN_FACE_*), and the boxes it meets. */
typedef struct { int16_t sec; uint8_t pass, verd; uint16_t face; uint64_t hit[2]; } terrain_snap_face;
/** @brief List the visible sectors of the last build whose bounding sphere, widened for vertex animation, meets a world box.
 * @param wb The boxes, full-world units: x0, y0, z0, x1, y1, z1.
 * @param nb Box count, at most 128.
 * @param skip Level sectors to leave out, as bits (sector s is bit s & 63 of skip[s >> 6]), or NULL.
 * @param out Receives the sectors, valid until the next call.
 * @return The sector count, or -1 when terrain_snap_ready does not hold. */
int  terrain_snap_sectors(const float (*wb)[6], int nb, const uint64_t skip[16],
                          const terrain_snap_sector** out);
/** @brief Read the level sectors the last build found visible with a decode (the sectors terrain_snap_sectors can list), as bits.
 * @param vis Receives the set: sector s is bit s & 63 of vis[s >> 6].
 * @return 1 when a build is latched and fed the scene now accumulating, else 0 (vis cleared). */
int  terrain_snap_visible(uint64_t vis[16]);
/** @brief A stamp of the last terrain build, distinct for each build: the face verdicts of terrain_snap_face_verdict hold while it is unchanged.
 * @return The stamp. */
unsigned terrain_snap_build(void);
/** @brief List the faces of the sectors terrain_snap_sectors finds whose corners' world box meets a world box, each with its walk verdict under the last build's camera.
 *
 * Every face of the LP and HP passes (own and borrowed) is a candidate,
 * drawn or not; the corners are the pass's vertices at the build's
 * vertex-animation phase.
 * @param wb The boxes, full-world units: x0, y0, z0, x1, y1, z1.
 * @param nb Box count, at most 128.
 * @param out Receives the faces, valid until the next call.
 * @return The face count, or -1 when terrain_snap_ready does not hold. */
int  terrain_snap_faces(const float (*wb)[6], int nb, const terrain_snap_face** out);
/** @brief The walk verdict of one terrain face under the last build's camera: the walk's own gate chain (tn_face_classify, tn_face_gate) over the face's corners projected by the build's camera.
 *
 * A sector the build did not find visible, a pass that does not run for it
 * and a face index out of range read TERRAIN_FACE_DROPPED.
 * @param sec The level sector index.
 * @param pass The walk pass, 0-5.
 * @param face The face index within the pass.
 * @return TERRAIN_FACE_DROPPED, TERRAIN_FACE_FLAT or TERRAIN_FACE_NEAR. */
int  terrain_snap_face_verdict(int sec, int pass, int face);

/* A level's terrain read from the disc into host bytes, in the record
 * layout the walk's decoders read: the sector records and the texture
 * tables at byte offsets into one buffer. */
typedef struct terrain_host_level {
    const uint8_t*  bytes;          /* the level's terrain bytes */
    uint32_t        len;
    const uint32_t* sector_off;     /* each sector record's byte offset */
    int             sector_count;
    uint32_t        lq_off, hq_off; /* the LQ and HQ texture tables' byte offsets */
    const uint8_t*  exe;            /* the boot executable's text: the static tables the walk
                                     * reads at guest addresses (the HQ tile UV-orientation
                                     * table), as the game's load leaves them; NULL for none */
    uint32_t        exe_addr, exe_len;
    const uint8_t*  vis;            /* the candidate set: a sector k draws only when k < vis_n and
                                     * vis[k] is 0xFF (the game's visibility table form); NULL
                                     * admits every sector */
    int             vis_n;
} terrain_host_level;

/* The view a preview is drawn from: a free-camera pose and the
 * environment distances the game runs the level with. */
typedef struct terrain_preview_view {
    int32_t  pos[3];          /* camera position, world units as --freecam-pose writes it (the walk reads world/16) */
    int32_t  yaw, pitch;      /* stored angle units, as --freecam-pose writes them */
    uint32_t lod_dist;        /* LP/HP LOD distance (m_LodDistance) */
    uint32_t cull_dist;       /* LP far cull distance (m_CullingDistance) */
    double   H;               /* projection distance: the canonical H times the FOV scale */
    double   H_canon;         /* the GTE's canonical projection distance */
    double   ofx, ofy;        /* screen offset, pixels */
} terrain_preview_view;

/* What a preview build did, for its log line and its harness. */
typedef struct terrain_preview_stats {
    int      sectors;          /* records decoded */
    int      vis_hidden;       /* sectors outside the level's candidate set (terrain_host_level.vis) */
    int      drawn;            /* sectors drawing HP or LP from the view */
    int      baked;            /* sectors baked into the batches */
    int      near;             /* sectors baked with near-variant chunks */
    int      bake_fail;        /* sectors whose bake failed */
    uint64_t verts[5];         /* vertices per baked arena (OPAQUE, ALPHA, ADD, SUB, LP) */
    uint64_t dropped;          /* vertices of arenas the preview does not draw */
    uint64_t exe_reads;        /* guest-form reads answered from the boot executable's text */
    uint64_t guest_reads;      /* other guest-form reads, answered 0 */
    uint64_t guest_writes;     /* guest stores refused by the stand-in CPU (0 expected) */
} terrain_preview_stats;

struct openpete_tercache_preview;

/** @brief Builds a one-image preview of a host-loaded level: decodes every sector with the walk's decoders, takes each sector's tier verdicts from the view, bakes the drawn sectors with the cache's chunk packer and fills the globals the terrain pipelines read.
 *
 * Reads only the level's bytes and the installed game's layout: the
 * decoders address the bytes through the host window the HP source level
 * uses (TSRC_TERR_BASE), and a stand-in CPU state answers a guest-form
 * read inside the boot executable's text from that text, any other with 0,
 * and refuses every store, counting each. Every static the
 * build touches (the host window, the bake's scratch statics and the fog
 * scale) is saved on entry and restored on return, so a walk after it sees
 * what it would have seen without it. The game must be installed; no game
 * may run in the process.
 * @param L The level.
 * @param V The view.
 * @param out Receives the batches and globals; its target size, clear colour
 *        and VRAM image are the caller's.
 * @param st Receives the counts, or NULL.
 * @param why Receives the failure's reason.
 * @param cap Its capacity.
 * @return 0, or -1 when the level decodes no sector or an allocation failed. */
int terrain_preview_build(const terrain_host_level* L, const terrain_preview_view* V,
                          struct openpete_tercache_preview* out, terrain_preview_stats* st,
                          char* why, size_t cap);

#ifdef __cplusplus
}
#endif
/** @brief Counts one --sector-visibility filled sky draw in the open load window.
 * @param computed Nonzero when the sky's C' was recomputed this draw.
 * @param added The sky sectors drawn that the game's verdict hid. */
void terrain_sector_filled_note_sky(int computed, int added) OP_REQUIRES(sim);

/** @brief The containment key for an element with no sector byte: whether a point lies in a drawn sector or in none.
 *
 * Under --sector-visibility game or filled, a point inside one or more sector bounding spheres of the last
 * canonical walk is admitted when one of those sectors is drawn (the filled
 * set, or under game the current tick's verdict, terrain_sector_pvs_shown);
 * a point inside none is admitted.
 * Under frustum, or where filled has no PVS: 1.
 * @param cpu The guest CPU state, read only.
 * @param x The point, world X.
 * @param y The point, world Y.
 * @param z The point, world Z.
 * @return 1 to admit, 0 to reject. */
int terrain_point_admitted(struct CPUState* cpu, int32_t x, int32_t y, int32_t z) OP_REQUIRES(sim);

/** @brief The game's PVS verdict for sector `i` on the current tick.
 *
 * Reads the visibility capture the world draw of this tick took
 * (on_visibility_terrain) before the draw's sector loop cleared the bytes, so
 * a moby pass later in the same draw reads this tick's verdict; with no
 * capture held, the copy the last canonical walk made. The verdict is the
 * PVS alone: the table the moby cull tests (env_anim.live_table, D_8006B300
 * on Spyro 2) is the world draw's tier bits, also 0 for a sector the 4:3
 * sphere frustum or the tier ranges dropped. Sim thread.
 * @param i The sector index.
 * @return 1 shown, 0 hidden, -1 when no PVS byte names the sector (a game
 *         without a position-keyed PVS, index 255 or above, or past the
 *         table). */
int terrain_sector_pvs_shown(uint32_t i) OP_REQUIRES(sim);

/** @brief Whether sector `i` is admitted by the --sector-visibility mode for this tick.
 *
 * frustum admits every sector (the identity for non-terrain paths); filled
 * answers from the drawn set U held at the last canonical walk, game from
 * the game's verdict V of the current tick (terrain_sector_pvs_shown); each
 * admits every sector at index 255 or above and every sector on a game or
 * level without a PVS.
 * Sim thread.
 * @param i The sector index.
 * @return 1 admitted, 0 rejected. */
int terrain_sector_admitted(uint32_t i) OP_REQUIRES(sim);

#endif /* OPENPETE_COMMON_RENDERER_TERRAIN_H */
