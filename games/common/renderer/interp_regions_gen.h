/* games/common/renderer/interp_regions_gen.h - builds a game's interp region
 * table from descriptor rows and registers it with the openpete_interp ring.
 *
 * A game describes its plain-global regions as a const array of
 * game_interp_row_t (address, length, timeline flag) and hands it here; rows
 * whose span is computed at registration time (the camera's
 * cam.base..cam.xform_end) are added one at a time through the same call. The
 * ring's region INDEX is therefore assigned in exactly one place, and a
 * consumer that needs an index asks for it by name instead of restating the
 * table's order (a restated index silently selects the wrong region when a row
 * is inserted).
 *
 * A row whose address or length is 0 describes a mechanism the game has no
 * located counterpart for; it is skipped and its name resolves to -1. */
#ifndef OPENPETE_GAME_INTERP_REGIONS_GEN_H
#define OPENPETE_GAME_INTERP_REGIONS_GEN_H

#include <stdint.h>

#include "openpete_interp_objs.h"   /* openpete_interp_obj_capture_fn */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct game_interp_row {
    const char* name;       /* the lookup key and the diagnostic name */
    uint32_t    addr;       /* KSEG0 PSX vaddr of the region's first byte */
    uint32_t    length;     /* bytes captured each tick */
    int         ra_commit;  /* 1 = present from the committed timeline under
                             * runahead (openpete_interp_region_t.ra_commit) */
} game_interp_row_t;

/** @brief Append descriptor rows to the region table in table order.
 *
 * Each row goes through game_interp_rows_add_one, so a row with a null name,
 * a zero address or a zero length is skipped, and a row past the ring's
 * region cap (OPENPETE_INTERP_MAX_REGIONS) is dropped.
 * @param rows The descriptor rows; NULL adds nothing.
 * @param count Number of rows.
 * @return The index of the first row added, or -1 when nothing was added. */
int game_interp_rows_add(const game_interp_row_t* rows, unsigned count);
/** @brief Append one region row to the table and return its index.
 *
 * The name pointer is stored, not copied, so it must outlive the table.
 * @param name The lookup key and diagnostic name; NULL is refused.
 * @param addr KSEG0 PSX address of the region's first byte; 0 is refused.
 * @param length Bytes captured each tick; 0 is refused.
 * @param ra_commit 1 to present the region from the committed timeline under
 *        runahead.
 * @return The row's index, or -1 when an argument is refused or the table
 *         holds OPENPETE_INTERP_MAX_REGIONS rows. */
int game_interp_rows_add_one(const char* name, uint32_t addr, uint32_t length,
                             int ra_commit);

/** @brief Hand the accumulated region table to the ring.
 *
 * Passes the table to openpete_interp_register_regions. With
 * OPENPETE_INTERP_REGION_CENSUS set, the first call also registers the census
 * table print at exit. Call once, after every add. */
void game_interp_rows_commit(void);

/** @brief The ring region index of a registered row, looked up by name.
 *
 * A linear string compare over the table in registration order.
 * @param name The row's name; NULL resolves to -1.
 * @return The region index, or -1 when the game did not register the row. */
int game_interp_row_index(const char* name);

/* The camera's ring row. Every game registers its draw-time camera transform
 * under this one name, and every consumer resolves the ring index by it. */
#define GAME_INTERP_CAM_ROW "g_Camera"

/* The specular metal matrix's ring row: the 3x3 int16 matrix (five words) the
 * game rebuilds from the camera on gameplay ticks and the environment-reflection
 * map reads. A game with that material registers it under this name. */
#define GAME_INTERP_METAL_ROW "g_MetalMatrix"

/** @brief Snapshot one record per live moby of the game's moby array into the keyed prev/curr table.
 *
 * The array (game_layout.moby: array_head/array_direct, array_stride,
 * state_off, state_term) is fixed and ends at a sentinel element whose state
 * byte is state_term, so a moby's PSX address is its slot and its key across
 * ticks, except a credits letter or a loading-screen tally gem, which is
 * captured under its record key (game_credits_letter_key, the key
 * moby_game.interp_key looks it up by). A
 * moby whose state byte is 0x80 or above is dormant and skipped. Each
 * record captured is one moby record and no bytes of the next, taken from the
 * draw-time stash when it holds the moby and live RAM otherwise. The walk
 * stops 512 slots short of the table capacity so the pools captured after it
 * always fit. Each record is captured as shown when its drawn byte is set or
 * the native view drew it on the tick (moby_native_canon_drawn_mobys,
 * moby_native_view_revealed_mobys), for openpete_interp_objs_capture_shown.
 * OPENPETE_CAPTRACE_CLS=<class|0xFFFF> prints the tick position of
 * every captured moby of that class; OPENPETE_MOBY_STASH_CENSUS prints the
 * stash's served and differing counts per call. Read-only against guest RAM.
 * Call between openpete_interp_objs_begin_tick() and
 * openpete_interp_objs_end_tick(); game_interp_register_common() does. */
void game_interp_capture_moby_array(void) OP_REQUIRES(sim);

/** @brief Capture the moby record at @p m under @p key without a slot serial: one record and no bytes of the next.
 *
 * For a capture source outside the moby-array walk (a HUD pool, a render
 * list): the blob is the lesser of the store's blob size and
 * game_layout.moby.array_stride, and the store records no serial for it, so
 * moby_pair_reborn never pairs it as a reused slot. Read-only against guest
 * RAM.
 * @param key The record's interp key.
 * @param m The moby's PSX address.
 * @return The record's host bytes, or NULL when @p m does not map. */
const uint8_t* game_interp_capture_moby_record(uint32_t key, uint32_t m);

/** @brief Open the draw-time moby stash for the current canonical-render epoch.
 *
 * A game's draw hook calls this, then game_interp_moby_stash_put per moby the
 * draw read, while the records hold the state the draw left and no later
 * routine of the tick has written them. While the stash's epoch is in flight
 * or the last committed one, game_interp_capture_moby_array takes the stashed
 * record for those mobys instead of the live one. A call in a later epoch than
 * the stash's, or the first call after a block, empties the stash; calls in
 * one epoch add to it. A call on a speculative pass blocks the stash: puts are
 * ignored and the capture reads live RAM until the next non-speculative call.
 * Host memory only. */
void game_interp_moby_stash_begin(void) OP_REQUIRES(sim);
/** @brief Copy one moby's record, as the draw left it, into the draw-time stash.
 *
 * Ignored while the stash is blocked or holds 1024 records, and for an
 * address that does not resolve. The copy is one moby record long; a change
 * of that length empties the stash, and a later put of the same moby wins.
 * @param moby The moby's PSX address. */
void game_interp_moby_stash_put(uint32_t moby);

/** @brief Snapshot one record per active slot of the game's particle pool, keyed by the record's PSX address.
 *
 * The walk is the particle routine's own (game_layout.particles: array_ptr,
 * rec_stride, sel_off): from the pool base, one record per `rec_stride`, at
 * most 512, reading the selector halfword whose sign marks an inactive slot
 * and whose arithmetic shift by 8 is -1 at the end-of-pool sentinel. A slot a
 * new record reuses is the consumer's pairing verdict, not this walk's. The
 * renderer projects each record under the tick's camera and links its own
 * primitives, so the sub-tick replay of those primitives needs each record's
 * previous state to rebase onto; the pool sits outside the moby array, so the
 * moby walk never reaches it. Reached through
 * game_interp_capture_feeder_pools. Read-only against guest RAM. */
void game_interp_capture_particle_pool(void);

/** @brief Snapshot one record per live slot of the game's gem-glint pool, keyed by the record's PSX address.
 *
 * The pool (game_layout.glint: pool, slots, stride, packed_off) is a fixed
 * array outside the moby array, so the record's address is its slot; a slot
 * whose packed word has lifetime byte 0 is free and skipped. The glint
 * routine projects each record under the tick's camera and emits the star's
 * screen corners itself, so the sub-tick replay needs each record's previous
 * position and spin phase to rebase them onto. Reached through
 * game_interp_capture_feeder_pools. Read-only against guest RAM. */
void game_interp_capture_glint_pool(void);
/** @brief Captures every live record of the glow pool into the interp store, keyed by slot address.
 *
 * The pool (game_layout.glow: pool, slots, stride, packed_off) is a fixed
 * array outside the moby array; a slot whose packed word has lifetime byte
 * 0 is free and skipped. Reached through game_interp_capture_feeder_pools.
 * Read-only against guest RAM. */
void game_interp_capture_glow_pool(void);

/** @brief Snapshot one record per live slot of Zoe's zap pool, keyed by the record's PSX address.
 *
 * The pool (game_layout.zap) is `slots` records of `stride` bytes: the head
 * world XYZ at `head_off`, the tail twelve bytes on, and a packed word at
 * `packed_off` whose byte 0 is the segment count and 0 on a free slot, which
 * is skipped. Each record is captured whole, up to the store's blob size. The
 * routine projects the chain between head and tail under the tick's camera
 * and emits the screen triangles itself, so the sub-tick replay needs each
 * record's previous head and tail to rebase the ribbon's two ends onto.
 * Reached through game_interp_capture_feeder_pools. Read-only against guest
 * RAM. */
void game_interp_capture_zap_pool(void);

/** @brief Snapshot every live tracer ribbon record and every vertex of the tracer mesh lists the most recent mesh draw read.
 *
 * A ribbon record (game_layout.tracers: ring, slots, stride) whose owner word
 * at +0x00 is 0 is free and skipped; a live one is captured whole, point ring
 * included, keyed by its PSX address. Each mesh vertex is a 12-byte world XYZ
 * int32 triple keyed by its own address, from up to eight lists
 * tracers_mesh_lists reports. Both producers project their points under the
 * tick's camera, so the sub-tick replay needs each point's previous position
 * to rebase onto. Read-only against guest RAM. */
void game_interp_capture_tracer_pool(void) OP_REQUIRES(sim);

/** @brief Walk the particle, gem-glint, glow, zap and tracer pools in one call.
 *
 * The extra-pool walk a game passes to game_interp_register_common when its
 * feeders link their own primitives, so the game registers one walk rather
 * than a list kept in step with the feeders. A pool the game's layout does
 * not locate is skipped by its own walk. */
void game_interp_capture_feeder_pools(void) OP_REQUIRES(sim);

/** @brief Register a game's interp region table and per-object capture in the shared shape.
 *
 * Adds the camera row (GAME_INTERP_CAM_ROW, `cam_length` bytes from
 * game_layout.cam.base, presented from the committed timeline under
 * runahead) first so it is region 0, then `rows`, then commits. The
 * per-object capture it installs runs, between
 * openpete_interp_objs_begin_tick and openpete_interp_objs_end_tick, the
 * moby-array walk, then `extra_pools`, then the env-anim scroll descriptors.
 * The store's blob is the longer of the moby and tracer-ribbon strides, over
 * OPENPETE_INTERP_OBJ_SLOTS slots.
 * @param rows The game's plain-global rows.
 * @param count Number of rows.
 * @param cam_length Bytes of the camera row.
 * @param extra_pools The game's walk of pools outside the moby array, or NULL
 *        when it has none. */
void game_interp_register_common(const game_interp_row_t* rows, unsigned count,
                                 uint32_t cam_length, void (*extra_pools)(void) OP_SLOT(sim));

/** @brief Register the game's per-object capture callback with the ring.
 *
 * Routed through this layer so the region census
 * (OPENPETE_INTERP_REGION_CENSUS) gets a per-tick hook: with the census on,
 * the ring calls a wrapper that samples every region and then runs `fn`. The
 * ring's own region capture runs immediately before the callback.
 * @param fn The game's per-object capture.
 * @param blob_bytes Bytes stored per object.
 * @param slots Object slots in the store. */
void game_interp_rows_obj_capture(openpete_interp_obj_capture_fn fn,
                                  uint32_t blob_bytes, uint32_t slots);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_INTERP_REGIONS_GEN_H */
