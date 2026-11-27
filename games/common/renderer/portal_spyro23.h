/* games/common/renderer/portal_spyro23.c's interface: the homeworld portal
 * preview feeder shared by the two games whose portal pass and preview leaf
 * are the same routines at different addresses.
 *
 * The leaf is instruction-identical across the two: 525 words each, of which
 * 49 differ and every one of those is an address inside a lui/addiu pair or a
 * jump target [CITE: games/spyro-2/spyro-2/asm/cyclorama.s:382-951;
 * games/spyro-3/spyro-3/asm/cyclorama.s:378-945]. The pass is the same routine
 * minus Spyro 3's opening animation: Spyro 3 reads the portal record's counter
 * byte +0x43, advances it by 0x10 per tick to a ceiling of 0x100 and then to 0
 * [CITE: games/spyro-3/spyro-3/asm/nonmatchings/drawutil/func_8001EDEC.s:40-51],
 * and while it is nonzero scales each projected pentagon vertex, taken
 * relative to the pentagon's centroid, by counter / 256
 * [CITE: games/spyro-3/spyro-3/asm/nonmatchings/drawutil/func_8001EDEC.s:303-334];
 * Spyro 2's pass never touches +0x43
 * [CITE: games/spyro-2/spyro-2/asm/nonmatchings/drawutil/func_8004C66C.s:33,65,141].
 * The animation only moves the numbers the pass writes into the parameter
 * block, which this feeder reads, so the difference needs no code here.
 *
 * The parameter block's field offsets are identical in both games; the two
 * disassemblies differ only in which register holds each clip-rect bound when
 * it is stored [CITE: games/spyro-2/spyro-2/asm/nonmatchings/drawutil/func_8004C66C.s:363-369;
 * games/spyro-3/spyro-3/asm/nonmatchings/drawutil/func_8001EDEC.s:509-517].
 * So each game supplies the block address, the two hook addresses, the record
 * array and count, and the three tables and the counter the pass's gates and
 * far colour read, which the feeder re-evaluates for a record the pass culls
 * only because the 4:3 frame's X bounds reject it. */
#ifndef OPENPETE_COMMON_RENDERER_PORTAL_SPYRO23_H
#define OPENPETE_COMMON_RENDERER_PORTAL_SPYRO23_H

#include <stdint.h>
#include "psx_runtime_internal.h"
#include "renderer/native_gte.h"     /* ngte_view: the caller's projection view */
#include "renderer/terrain_game.h"   /* openpete_portal_skyplane */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/* What the two games do not share. Each game's renderer/portal_game.c defines
 * the instance; a game that leaves `pass` or `leaf` zero registers no hook and
 * never enters the feeder.
 *
 *   block  the pass's parameter block for the leaf: +0x00 y-min, +0x04 y-max,
 *          +0x08 x-min, +0x0C x-max, +0x10 preview mesh pointer, +0x14
 *          depth >> 7, +0x18/+0x1A/+0x1C far colour halfwords at GTE scale,
 *          +0x1E IR0, +0x20 edge count, +0x24 the 8-byte edge entries
 *   pass   the portal pass: gates each record, projects its pentagon, fills
 *          the block and calls the leaf
 *   leaf   the preview leaf: draws one pre-projected mesh under the block
 *   records  the pointer to the level's portal record array, 0x44 bytes per
 *          record: five world-space pentagon vertices (int32 x, y, z) at
 *          +0x00, the preview mesh pointer at +0x3C, the enable byte at +0x40
 *   count  the portal record count
 *   anim   1 when the pass scales the pentagon about its vertex mean by the
 *          record's +0x43 counter / 256 while the counter is nonzero, else 0
 *   atan_table  the u16 octant table of the pass's angle routine (S2
 *          func_8001B4B8, S3 func_8004E880: 129 entries over the minor/major
 *          ratio in 7 fractional bits)
 *   sqrt_table  the s16 table of the pass's planar length routine (S2
 *          func_8001BA20, S3 func_8004EDE8)
 *   far_counter the word whose value, times 4 plus the record index times
 *          64, indexes the cos table for the far colour the pass writes into
 *          the block's +0x18/+0x1A
 *   env_draw_ret the return address of the world draw's call inside the
 *          draw dispatcher's world-draw wrapper, the instruction after its
 *          jal; the wrapper keeps its own $ra at sp+0x10
 *   env_wrap_ret the return address of that wrapper's call inside the draw
 *          dispatcher (the world draw under flag 0x4, the portal pass later
 *          under flag 0x8, the flag word in $s3)
 *   level_slots the byte table the loader indexes by a level number to get
 *          the level's slot, the record's +0x42 byte naming the level a
 *          portal leads to
 *   level_wad_base the WAD.WAD entry holding slot 0's level data; slot n's
 *          is level_wad_base + 2 * n, its sub-file 1 the block chain the
 *          loader walks
 *   level_wad_hops the size-prefixed blocks the loader steps over from the
 *          start of that sub-file before the block whose size word sits at
 *          +0x0C; the sky block follows it and the portal block follows the
 *          sky block */
typedef struct portal_game_s23_consts {
    uint32_t block;
    uint32_t pass;
    uint32_t leaf;
    uint32_t records;
    uint32_t count;
    int      anim;
    uint32_t atan_table;
    uint32_t sqrt_table;
    uint32_t far_counter;
    uint32_t env_draw_ret;
    uint32_t env_wrap_ret;
    uint32_t level_slots;
    int      level_wad_base;
    int      level_wad_hops;
} portal_game_s23_consts_t;

/* The running game's table: game_runtime_install (game_host_desc.h) copies
 * the registered descriptor's table here before any feeder runs. */
extern portal_game_s23_consts_t portal_game_s23;

/** @brief Selects the captured tick a sub-tick present shows: tick N-1's set on a held camera cut.
 *
 * Outside a hold nothing changes. On a hold the set is cut_held_set_pick's,
 * swapped in until portal_s23_reextract_leave, so portal_s23_sky_planes,
 * portal_s23_sky_recent and portal_s23_reextract read tick N-1's previews,
 * the set tick N-1's camera drew.
 * @param hold Nonzero on a held camera cut. */
void portal_s23_reextract_enter(int hold);

/** @brief Undoes portal_s23_reextract_enter, restoring the current tick's set. */
void portal_s23_reextract_leave(void);

/** @brief Feeds the tick's captured portal previews again on a sub-tick present.
 *
 * Returns without feeding when the native scene is off, when the portal path
 * did not draw on the canonical present (on a held cut, when tick N-1 drew
 * no set), or when the shown tick captured nothing. A held set
 * (portal_s23_reextract_enter) is fed unchanged. Otherwise, with
 * `interp_on`, OPENPETE_PORTAL3_INTERP not starting with 0, `alpha` below
 * 0.999 and a valid previous capture, each preview is lerped
 * toward its partner on the previous tick (clip rect, world depth, IR0, far
 * colour and the pentagon edges the two share); a preview with no partner
 * keeps its own capture. Otherwise the capture is fed unchanged, so a present at alpha 1
 * matches the canonical present bit for bit.
 * @param cpu The guest CPU state.
 * @param alpha The sub-tick alpha in [0, 1].
 * @param interp_on Nonzero when the caller brackets the present between two ticks. */
void portal_s23_reextract(CPUState* cpu, double alpha, int interp_on) OP_REQUIRES(sim);

/** @brief Reports whether a portal preview emitted geometry within the last two VBLs.
 *
 * Compares openpete_frame, the VBL counter, against the value it held when
 * the feeder last emitted a preview triangle. On a held cut it reports
 * whether the held set holds a preview.
 * @return Nonzero when a preview emitted at most two VBLs ago (on a held
 *         cut, when the held set holds one), else 0. */
int portal_s23_sky_recent(void);

/** @brief Fills `out` with the screen-space opening of every portal in the latest capture.
 *
 * The latest capture counts only when it was taken at most three VBLs ago;
 * on a held cut the held set counts whatever its age, and no set counts
 * when tick N-1 drew none.
 * One entry is written per portal, from its base record (leaf argument 1),
 * and a record without a pentagon of at least three edges, or whose
 * re-derived opening is off-screen, is skipped. The opening polygon is the
 * preview's colour bounds clipped by each pentagon edge, shifted by the
 * view's screen offset from (256, 120). On a re-derived opening the plane is
 * the opening plane and carries the pentagon as its view-space outline;
 * otherwise it is constant-depth (a = b = 0, c = 1 / depth) at the preview's
 * world depth, floored at 1, with no outline (vn = 0).
 * @param cpu Reads the portal records.
 * @param v The view whose screen offset places the polygon.
 * @param out Receives the planes.
 * @param max The capacity of `out`.
 * @return The number of planes written; 0 when no recent capture holds a
 *         preview or the portal render path is deselected. */
int portal_s23_sky_planes(CPUState* cpu, const ngte_view* v, openpete_portal_skyplane* out, int max) OP_REQUIRES_SHARED(sim);

/** @brief 1 when the running world draw is the draw dispatcher's and this tick's portal pass follows it uncaptured.
 *
 * The test is the dispatcher's own: the world draw's $ra is
 * portal_game_s23.env_draw_ret, the wrapper's saved $ra at sp+0x10 is
 * env_wrap_ret, the dispatcher's flag word in $s3 has bit 0x8 (the portal
 * pass runs after the world draw in the same call), and the feeder holds no
 * capture for openpete_frame yet. The terrain feed then waits for that
 * capture (terrain_game.env_feed_waits_for_portals); the portal pass hook
 * releases it.
 * @param cpu guest state at the world-draw hook
 * @return 1 when the terrain feed must wait for this tick's portal capture */
int portal_s23_capture_follows(CPUState* cpu);

#endif /* OPENPETE_COMMON_RENDERER_PORTAL_SPYRO23_H */
