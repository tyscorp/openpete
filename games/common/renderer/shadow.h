/* shadow.h - the moby drop-shadow feeder and the pieces a game's own
 * player-shadow feeder reuses.
 *
 * The moby walk in renderer/shadow.c reads every engine address and every
 * decode switch from game_layout.shadow, so one body serves both games. A
 * game whose player shadow is a separate renderer (Spyro 1's silhouette fan)
 * supplies its own feeder and reaches the polybuf decoder, the projection
 * helpers and the shadow-decal parameter push through this header. */
#ifndef OPENPETE_COMMON_RENDERER_SHADOW_H
#define OPENPETE_COMMON_RENDERER_SHADOW_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/native_gte.h"
#include "openpete_native_render.h"   /* openpete_shadow_decal_params */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

/* One primitive decoded out of the polybuf: the ground truth an A/B compares
 * the native walk against. `nv` is 0 for a packet that is not a polygon (the
 * draw-mode mini-packet a player-shadow fan bundles ahead of each triangle). */
typedef struct {
    uint8_t  code;          /* GPU prim code byte (word[1] >> 24) */
    uint8_t  len;           /* OT tag length (word[0] >> 24) */
    int      nv;            /* 0, 3 or 4 */
    float    x[4], y[4];
    uint32_t rgb[4];
    int      u[4], v[4];
    int      clut, tpage;
    int      textured, gouraud, quad;
    uint32_t raw[20];
    int      nraw;
} shadow_prim;

/** @brief Decodes the polybuf range [start, end) into the shared capture buffer.
 *
 * Clears the buffer and its prim-code histogram, then walks every packet,
 * polygon or not, with a 24-word tag bound, 20 raw words per packet and a
 * 4096-packet guard. The buffer holds 4096 packets; the histogram counts the
 * stored ones. A draw-mode mini-packet (tag length 2, code byte 0) stores
 * with nv = 0, and consumers skip it.
 * @param cpu The guest CPU state.
 * @param start The first polybuf address.
 * @param end One past the last polybuf address. */
void                shadow_capture_prims(CPUState* cpu, uint32_t start, uint32_t end);
/** @brief The number of packets in the capture buffer.
 *
 * @return The count, 0 to 4096. */
int                 shadow_prim_count(void);
/** @brief One packet of the capture buffer.
 *
 * @param i The index.
 * @return The packet, or NULL when `i` is negative or not below the count. */
const shadow_prim*  shadow_prim_at(int i);
/** @brief Prints the capture's prim-code histogram and its first six primitives to stderr.
 *
 * Each primitive prints its raw words and its decoded vertices, and the CLUT
 * and tpage when it is textured.
 * @param tag The label printed on the histogram line. */
void                shadow_dump_prims(const char* tag);

/* A projected point: screen position in float plus the GTE's MAC1..MAC3
 * (sz = MAC3, the view-Z). */
typedef struct { float sx, sy, sz; float m1, m2; } shadow_pt;

/** @brief Sign-extends the low 16 bits of a value.
 *
 * @param v The value.
 * @return The low halfword as a signed int. */
int    shadow_s16(int v);
/** @brief Reads the signed halfword at a guest address through a word read.
 *
 * Reads the aligned word and takes its high half when bit 1 of `addr` is
 * set, else its low half.
 * @param cpu The guest CPU state.
 * @param addr The halfword's address.
 * @return The sign-extended halfword. */
int    shadow_rd_s16(CPUState* cpu, uint32_t addr);
/** @brief GTE MVMVA with sf=1, the rotation matrix, V0, no translation and lm=0: IR = clamp16((R·v) >> 12).
 *
 * The products accumulate in 64 bits and the shift is arithmetic.
 * @param R The rotation, Q12.
 * @param vx The vector's X.
 * @param vy The vector's Y.
 * @param vz The vector's Z.
 * @param ir Receives IR1..IR3, saturated to -32768..32767. */
void   shadow_mvmva_ir(const int R[3][3], int vx, int vy, int vz, int ir[3]);
/** @brief MAC1..MAC3 of the camera-relative ground anchor, the translation the fan corners project with.
 *
 * Computes (C·v) / 4096 in double through ngte_mac, with no truncation and no
 * saturation.
 * @param C The camera matrix, Q12.
 * @param vx The anchor's camera-relative X.
 * @param vy The anchor's camera-relative Y.
 * @param vz The anchor's camera-relative Z.
 * @param TR Receives MAC1..MAC3. */
void   shadow_ground_tr(const int C[3][3], double vx, double vy, double vz, double TR[3]);
/** @brief Projects one local vertex through R and TR with the GTE-faithful RTPS pipeline.
 *
 * Runs ngte_project_gte with the -1024..1023 SXY bound applied. The output
 * keeps the screen position in float, MAC3 as sz, and MAC1 and MAC2.
 * @param R The rotation, Q12.
 * @param TR The translation.
 * @param ofx Screen X offset in pixels.
 * @param ofy Screen Y offset in pixels.
 * @param v The view; supplies the projection distance and the divide regime.
 * @param vx The vertex's X.
 * @param vy The vertex's Y.
 * @param vz The vertex's Z.
 * @param o Receives the projected point. */
void   shadow_project(const int R[3][3], const double TR[3], double ofx, double ofy,
                      const ngte_view* v, double vx, double vy, double vz, shadow_pt* o);

/** @brief Pushes the depth-aware shadow decal's camera-reconstruction parameters to the native renderer.
 *
 * Every shadow feeder calls it, so the parameters are valid whenever any
 * shadow draws, including moby shadows on a tick where the player casts
 * none. The shader recovers each terrain pixel's world height from the
 * opaque depth pre-pass via V_j = (4096²·C⁻¹[j]·MAC) / 4096, so the pushed
 * rows are the true inverse (shadow_cam_inverse): the Spyro camera matrix is
 * not orthonormal (its Y row carries a ~0.625 aspect scale), and the
 * transpose would under-weight every MAC2 term 2.56 times. `ground_up` is
 * the signed word at game_layout.shadow.ground_plane, 0 when the layout has
 * none; the textured moby decal ignores it. The wall height, steep-normal
 * threshold and raised-occluder terms read OPENPETE_SHADOW_WALL_HEIGHT
 * (400), OPENPETE_SHADOW_STEEP_NY (0.5), OPENPETE_SHADOW_PLAT_EPS (150) and
 * OPENPETE_SHADOW_PLAT_SLACK (48) on every call. Writes no PSX RAM.
 * @param cpu The guest CPU state; supplies the ground plane word.
 * @param v The view; supplies the camera's up coordinate.
 * @param C The camera matrix, Q12.
 * @param ofx The screen X offset.
 * @param ofy The screen Y offset.
 * @param H The projection distance.
 * @param out When non-NULL, receives the pushed parameters. */
void   shadow_push_decal_params(CPUState* cpu, const ngte_view* v, const int C[3][3],
                                double ofx, double ofy, double H,
                                openpete_shadow_decal_params* out) OP_REQUIRES(sim);

/* A player ground-shadow fan as the footprint decal (shadow_decal.frag) draws
 * it: the anchor and the 16 ring points as projected, the triangles the
 * routine linked, and the camera the points were projected through. The
 * shadow is the fan's own surface where the depth rule keeps it, united with
 * the terrain under the ring's vertical projection between the fan and the
 * caster; the drawn geometry is the screen convex hull of that prism, and the
 * fragment stage decides every pixel from the fan data the feed hands the
 * renderer (openpete_shadow_footprint). */
#define SHADOW_FAN_RING 16
typedef struct {
    shadow_pt anchor;
    shadow_pt ring[SHADOW_FAN_RING];
    uint32_t  tri_mask;      /* bit k: (anchor, ring k, ring k+1) is linked */
    int       slot[SHADOW_FAN_RING]; /* triangle k's ordering-table slot */
    uint32_t  anchor_rgb;    /* the gouraud at the anchor; the rim is 0 */
    double    anchor_up;     /* world height (game z) of the anchor */
    double    caster_up;     /* world height of the caster, read from the same
                              * staged player record as the anchor's block */
    int       C[3][3];       /* camera matrix the points projected through */
    double    ofx, ofy, H;   /* projection registers of that view */
} shadow_fan;

/** @brief Hands the renderer the fan's footprint block and feeds the fan triangles and the prism hull.
 *
 * Both feeds are untextured subtractive SHADOW triangles under the current
 * scene tag. The linked fan triangles carry their gouraud with vertex alpha
 * 1: the depth-tested fallback pipe draws them and the decal discards them.
 * The prism hull carries black with vertex alpha 0: the decal shades it and
 * the fallback subtracts nothing. The hull is the screen convex hull of each
 * fan point with the prism floor (16 world units under the fan) and roof
 * (the caster height) above it; when any of those points lies nearer than
 * view-Z 16 or projects 4096 or more pixels off, a full-screen cover replaces
 * the hull. OPENPETE_SHADOW_FOOTPRINT=0 clears the block's footprint term.
 * @param cpu Unused.
 * @param f The fan. */
void   shadow_fan_feed(CPUState* cpu, const shadow_fan* f) OP_REQUIRES(sim);
/** @brief The caster height a player feeder stores beside its anchor (shadow_fan.caster_up).
 *
 * Reads the z word of the player position in the player state block
 * (game_layout.player), through the same view as the shadow block: the
 * sub-tick re-extract stages both from one record. A game with no player
 * block gets `anchor_up`, which flattens the prism to the fan.
 * @param cpu The guest CPU state.
 * @param anchor_up The anchor's world height, the fallback.
 * @return The caster's world height (game z). */
double shadow_caster_up(CPUState* cpu, double anchor_up);

/** @brief The ordering-table slot a shadow fan triangle (anchor, p1, p2) is inserted at.
 *
 * Every shadow routine keys the triangle the same way: the sum of the two rim
 * points' SZ plus half that sum plus the anchor's SZ, shifted into slot units
 * and less the routine's bias. Each SZ is rounded and saturated to 0..0xFFFF,
 * the GTE's 16-bit view-Z. `shift` is 9 for the player fans (full-world
 * view-Z) and 7 for the moby shadows (quarter-world view-Z), so both land on
 * the terrain's 128-world slots.
 * @param sza The anchor's view-Z.
 * @param sz1 The first rim point's view-Z.
 * @param sz2 The second rim point's view-Z.
 * @param shift The key-to-slot shift.
 * @param bias The routine's bias.
 * @return The slot; a negative slot is a triangle the routine does not link. */
int    shadow_ot_slot(float sza, float sz1, float sz2, int shift, int bias);
/** @brief Converts an ordering-table slot to the scene tri's key channel (openpete_scene_tri.koff).
 *
 * The value is the slot's absolute key in world units (slot * 128) plus the
 * 0.5 sentinel; a negative slot returns 0 (not keyed).
 * @param slot The ordering-table slot.
 * @return The key channel value. */
float  shadow_slot_koff(int slot);

/** @brief Converts a shadow walk's view-Z to the native scene depth.
 *
 * `scale` lifts the walk's view-Z to the one full-world view-Z every feed
 * shares: the moby-shadow walk works in world/4 coordinates and passes 4, the
 * player fans pass 1. `bias` then multiplies the full-world view-Z. The
 * callers pass shadow_bias_spyro or shadow_bias_moby, both 1.0 by default,
 * for the scene depth and 1.0 for the unbiased pvz. A shadow is a decal
 * coplanar with its ground: the moby shadows' tie-break is the subtractive
 * pipelines' hardware depth bias (OPENPETE_SHADOW_HW_BIAS) and the player
 * fan's occlusion is the depth-aware decal (shadow_decal.frag). Shadows draw
 * with depth writes off, so the value never enters the opaque depth field.
 * @param viewz The walk's view-Z (MAC3).
 * @param scale The factor to full-world view-Z.
 * @param bias The multiplicative view-Z bias.
 * @return The native scene depth. */
float  shadow_depth(float viewz, float scale, float bias);
/** @brief The view-Z bias of the player shadow fan, from OPENPETE_SHADOW_BIAS_SPYRO.
 *
 * Read until a non-negative value is seen, then latched; unset or empty reads
 * 1.0 (no bias).
 * @return The multiplicative bias. */
float  shadow_bias_spyro(void);
/** @brief The view-Z bias of the moby drop-shadows, from OPENPETE_SHADOW_BIAS_MOBY.
 *
 * Read until a non-negative value is seen, then latched; unset or empty reads
 * 1.0 (no bias).
 * @return The multiplicative bias. */
float  shadow_bias_moby(void);

/** @brief Reports whether OPENPETE_SHADOW_VERIFY selects the A/B harness and the one-shot dump.
 *
 * The variable is read once and latched; set, non-empty and not starting
 * with '0' enables it.
 * @return 1 when enabled, else 0. */
int      shadow_verify_enabled(void);
/** @brief The first openpete_frame the verify dump and compare run from, from OPENPETE_SHADOW_VERIFY_FRAME.
 *
 * Latched on the first call; unset reads 0. The value parses in any base
 * strtoul accepts.
 * @return The frame. */
unsigned shadow_verify_frame(void);

/** @brief Sub-tick re-extract: rebuild the moby drop-shadows from the current gamestate, extract-only.
 *
 * Runs only while the native scene is live, RP_MOBY_SHADOW is selected, and
 * the last canonical tick rendered the moby shadows (RP_MOBY_SHADOW
 * canon-drawn). Only the moby-shadow draw marks that path, so a tick whose draw
 * filled the shadow list but never rendered it (rescue states 5 and 6 draw the
 * player's shadow alone) replays none. The shadows follow their owners'
 * interpolated poses and the interpolated camera.
 * @param cpu guest state, the sub-tick view staged */
void   shadow_reextract(CPUState* cpu) OP_REQUIRES(sim);
/** @brief Make the moby drop-shadow re-extract walk tick N-1's drawn-moby list for a held camera cut, or restore the current one.
 *
 * A held cut shows tick N-1's camera, and the list the game built that tick is
 * the set of mobys whose shadows it drew. The list is chosen by
 * cut_held_set_pick: the previous copy swapped in, the current copy when tick
 * N built none, or no list when tick N-1 built none; whether tick N drew the
 * path does not gate the held feed, and a fixed-address list walks its copy
 * instead of being re-read. Call on before shadow_reextract and off after it.
 * @param on Nonzero to choose the held list, zero to restore.
 * @return 1 while a held list is walked, else 0. */
int    shadow_replay_prev_list(int on);

/** @brief Sub-tick re-extract of the player's ground shadow for one model record.
 *
 * The leg is renderer/shadow_player.c, described by game_layout.shadow.player.
 * The caller stages the record's shadow state block first, so the anchor
 * reads the bracketed position. Feeds only when hook_reextract_ready(RP_SHADOW)
 * holds.
 * @param cpu The guest CPU state, the sub-tick view staged.
 * @param slot The model record the fan renders under. */
void   shadow_player_reextract(CPUState* cpu, int slot) OP_REQUIRES(sim);

/** @brief Reports whether the player's own ground shadow feeds the native scene; OPENPETE_SHADOW_PLAYER=0 turns it off.
 *
 * Latched on the first call. Off, every game's player-shadow feeder returns
 * before feeding and the moby drop-shadows stay on the shadow render path:
 * the A/B that prices the player fan's painted pixels without a second
 * binary, and the leave-one-out of the shadow path holding moby shadows only.
 * The guest draw still runs.
 * @return 1 when on, else 0. */
int    shadow_player_feed_enabled(void);

#ifdef __cplusplus
}
#endif
#endif /* OPENPETE_COMMON_RENDERER_SHADOW_H */
