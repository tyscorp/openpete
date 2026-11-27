/* ui_game.h - the per-game half of the shared 2D UI feeder.
 *
 * The decode in renderer/ui_sprite.c owns everything the games share: the
 * primitive classification, the FT4/F4/LINE_G2/SPRT word layouts, the
 * DR_MODE ABR tracking, the deferred feed-previous, the widescreen widening
 * of a full-width bar and the fade drive. Those layouts are the psyq packet
 * shapes, identical in both games.
 *
 * What differs is where the game keeps the layer: the address of the routine
 * every 2D primitive is linked through, the address of the full-screen fade
 * emitter, the signature of the freeze-frame backdrop quad, the VRAM block a
 * texture pack may replace by name, and the addresses of a text chain that
 * links itself and so never reaches the chokepoint. A zero address makes the
 * feeder register no hook for that path; a game with no 2D feeder names no
 * table and binds the all-zero one. */
#ifndef OPENPETE_COMMON_RENDERER_UI_GAME_H
#define OPENPETE_COMMON_RENDERER_UI_GAME_H

#include <stdint.h>
#include "psx_runtime_internal.h"   /* CPUState */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ui_game {
    /* The 2D ordering-table insert (a0 = the primitive being linked). Every
     * 2D emitter calls it and no 3D draw does - the 3D passes inline the
     * link - so it is the 2D-vs-3D discriminator. */
    uint32_t ot_insert;

    /* A second 2D ordering-table insert that takes the depth slot in a1 and
     * splices a0 into that slot of its own table base, rather than at a fixed
     * head word. The primitive pointer is in a0 exactly as above, so the same
     * hook body serves both; the slot only decides where in the table the
     * primitive lands, and the feeder takes its draw order from the DrawOTag
     * walk, or on a `rank_by_link` game from the slot and the link's order. The screen emitters that build their
     * own depth order call this one and never the head insert, so a game that
     * names only `ot_insert` loses their 2D entirely. 0 = the game has one
     * insert. */
    uint32_t ot_insert_slot;

    /* The full-screen fade emitter, called as (abr_mode, r, g, b): one
     * DR_MODE carrying the ABR alone, then a full-screen semi-transparent
     * POLY_F4. The feeder drives the native fade from the arguments. */
    uint32_t fade_emitter;

    /* A second fade emitter of the same (abr_mode, r, g, b) shape inside the
     * overlay window, which links its DR_MODE and POLY_F4 through
     * `ot_rank_slot_insert` rather than `ot_insert`. Its hook carries the
     * overlay id `fade_emitter_overlay` and fires only while that overlay is
     * resident; the address is a different function in every other overlay.
     * 0 = the game has none. */
    uint32_t fade_emitter_ovl;
    uint32_t fade_emitter_overlay;

    /* The emitter that draws a draw buffer's frame back as a grid of cells
     * inside an untextured border, once per tick of a window's shrink or
     * grow, every primitive linked through `ot_insert`. Its opaque F4s are
     * keyed by their emission ordinal within the call, so the 2D carry pairs
     * each border with the same border of the previous tick and moves it
     * with the cells on presents between ticks. 0 = the game has none. */
    uint32_t fb_grid_emitter;

    /* The cutscene letterbox emitter, called with no arguments: it steps
     * the bar height word at `height` toward its target and, when the height
     * is above 0, links two frame-width opaque black POLY_F4s through
     * `ot_insert`, the top one over PSX rows top_y .. top_y + height and the
     * bottom one over bottom_y - height .. bottom_y. The feeder drops those
     * primitives and hands the height the call leaves to the native scene
     * (openpete_native_scene_set_letterbox), which draws the bars lerped
     * between ticks. emitter 0 = the game has none; its bars, if any, feed
     * as frame-width F4s. */
    struct { uint32_t emitter, height; int top_y, bottom_y; } letterbox;

    /* The freeze-frame backdrop quad's signature: its setRGB0 tint as the
     * primitive packs it (0x00BBGGRR) and the VRAM position of the grayscale
     * CLUT it samples through. A quad matching both is reproduced from the
     * native renderer's own last world frame instead of being fed, so the
     * frozen frame shows the native render rather than the emulated VRAM
     * scratch. `freeze_tint` 0 = the game has no such quad. */
    uint32_t freeze_tint;
    int      freeze_clut_x, freeze_clut_y;

    /* Set when the game produces its frozen backdrop by suppressing the
     * framebuffer clear instead of emitting a quad: the menu screens write
     * DRAWENV.isbg = 0 on both draw buffers and draw no world, so the
     * framebuffer keeps the last rendered frame. The native then drives the
     * freeze backdrop from the draw buffer's own isbg byte and draws its last
     * world frame unaltered. 0 = the game has no such screen. */
    int freeze_retained;

    /* The UI texture block, named once at the first 2D primitive so a
     * texture pack can replace the whole block by file name. `atlas_w` 0 =
     * the game names no block. */
    int         atlas_x, atlas_y, atlas_w, atlas_h;
    const char* atlas_name;

    /* Emitters that build a DR_MODE + SPRT chain in the primitive buffer and
     * link it themselves, bypassing `ot_insert`: the glyph emitters and the
     * loading-screen image, which draws the uploaded picture as four
     * quarter-width sprites. Each is hooked separately and its chain read out
     * of the primitive buffer it just filled. 0 = unused slot. */
    uint32_t sprt_chain_emitter[3];

    /* The overlay each emitter above lives in, 0 for one in the base binary.
     * An emitter inside the overlay window shares its address with a different
     * function in every other overlay of the game, so its hook carries the id
     * and fires only while that overlay is resident. */
    uint32_t sprt_chain_overlay[3];

    /* Set when the game's 2D draw order is its own link order: its table is
     * an array of {tail, head} slots, every insert appends at a slot's tail
     * (first linked, first drawn), and its Draw stitches the slots highest
     * first, then the 2D entry, the head word `ot_insert` links into, one
     * slot below slot 0. The stitch runs after the canonical capture, so the
     * DrawOTag chain the capture can walk is the previous VBL's. The feeder
     * then ranks each 2D primitive by (table period; slot descending, the 2D
     * entry as slot -1 last; then link order) recorded where it is linked:
     * the inserts, and the return of a chain emitter that links its chain
     * into the 2D entry itself. 0 = rank by the DrawOTag walk. */
    int      rank_by_link;

    /* A slotted insert of the same shape as `ot_insert_slot` (a0 = the
     * primitive, a1 = the slot) that is hooked only to record the slot for
     * the link rank: the primitives it links reach the feeder another way
     * (the loading picture's SPRT chain), so it claims none. 0 = none. */
    uint32_t ot_rank_slot_insert;

    /* The routine that stitches the table's slots into the chain DrawOTag
     * walks and clears them. Hooked for the link rank: the links after it
     * belong to the next table, drawn after the current one. 0 = none. */
    uint32_t ot_stitch;
    /* The routines that call `ot_stitch`, each as its code range [lo, hi)
     * and the id of the overlay it lives in (0 = the base binary). Each waits
     * its VSync and then stitches, so the table open at a capture inside one
     * is the table the present shows. A capture at a VSync wait whose call
     * (openpete_yield_site.ra) lies outside every range presents the buffer
     * before the open table is drawn: its caller links more into that table
     * after the wait, and the table's 2D linked so far waits for the scene
     * the table is finished in. lo 0 ends the list. */
    struct { uint32_t lo, hi, overlay; } stitch_routine[8];
} ui_game_t;

/* The running game's table: game_runtime_install (game_host_desc.h) copies
 * the registered descriptor's table here before any feeder runs. */
extern ui_game_t ui_game;

/** @brief Feeds one glyph SPRT into the native HUD layer at emission.
 *
 * The glyph emitters link their SPRT chains with an inlined insert, so the
 * chokepoint never sees them; text_sprt hands each glyph here. The SPRT is
 * 0x14 bytes: colour|code at +0x04, x0/y0 at +0x08/+0x0A, u/v at
 * +0x0C/+0x0D, CLUT at +0x0E, and (height << 16) | width at +0x10. It is fed
 * at once, tagged with its address, so the next batch release ranks it
 * against the chokepoint's primitives by the ordering-table traversal.
 * Refused, and counted, on a speculative tick; a no-op with the native
 * scene off.
 * @param cpu The guest CPU state.
 * @param prim The SPRT's guest address.
 * @param tpage The GPU texture-page word in effect, which the SPRT packet
 *        does not carry; it comes from the DR_MODE that heads the chain. */
void ui_sprite_feed_sprt(CPUState* cpu, uint32_t prim, unsigned tpage) OP_REQUIRES(sim);

/** @brief Feeds one glyph POLY_FT4 into the native HUD layer at emission.
 *
 * The quad form of the self-linking glyph chains: a glyph emitted as one
 * 0x28-byte POLY_FT4 (colour|code at +0x04, then four xy/uv word pairs)
 * instead of a DR_MODE plus an SPRT. The packet carries its own texture page
 * and CLUT, so no draw mode is passed. Decoded by the chokepoint's FT4
 * decoder and tagged with its address for the release's ranking. A no-op
 * with the native scene off.
 * @param cpu The guest CPU state.
 * @param prim The POLY_FT4's guest address. */
void ui_sprite_feed_ft4(CPUState* cpu, uint32_t prim) OP_REQUIRES(sim);

/** @brief Records one 2D primitive's link into the game's ordering table for the link rank.
 *
 * Inert unless ui_game.rank_by_link is set. Each call takes the next link
 * ordinal and the count of stitches before it (ui_game.ot_stitch); a later
 * link of the same address supersedes an earlier one.
 * @param prim The primitive's guest address.
 * @param slot The table slot it is appended to; -1 for the 2D entry. */
void ui_sprite_link_note(uint32_t prim, int slot);

/** @brief Records the avatar's ordering-table slot and the table it is linked into.
 *
 * The table is the current one, counted by ui_game.ot_stitch as for
 * ui_sprite_link_note. A 2D primitive linked into an earlier table, or into
 * the same table at a higher slot, draws under the avatar
 * (openpete_native_scene_mark_under_player).
 * @param slot The avatar's slot (player_game.ot_slot); -1 when its draw culls it. */
void ui_sprite_player_link_note(int slot);

/** @brief Returns the count of links ui_sprite_link_note has recorded since boot.
 *
 * A chain emitter compares it across its call to tell whether its chain was
 * linked through a hooked insert or by the emitter itself.
 * @return The count. */
uint64_t ui_sprite_link_count(void);

/** @brief Returns the SPRTs fed into the native HUD layer since boot.
 *
 * Counts both the glyph SPRTs from ui_sprite_feed_sprt and the SPRTs claimed
 * at the chokepoint: the engagement counter a harness reads to tell "no text
 * drew" from "text drew identically".
 * @return The count. */
unsigned ui_sprite_sprt_count(void);

/** @brief Arms the native freeze backdrop for a freeze_retained game when the draw buffer being presented is not cleared.
 *
 * Reads the isbg byte of the env the scene is drawn with
 * (cyclorama_native_present_env: the DrawOTag that already submitted it,
 * a repeat-site row, else the current draw buffer's DRAWENV);
 * isbg 0 means the game keeps the last rendered image, and the
 * backdrop is armed. Called once per canonical present, next to the
 * clear-colour commit that reads the same DRAWENV. A no-op for a game
 * without freeze_retained or before the draw-buffer pointer is valid.
 * Writes no PSX RAM. */
void ui_freeze_retain_commit(void) OP_REQUIRES(sim);
/** @brief Returns the presents this run on which ui_freeze_retain_commit armed the backdrop.
 *
 * The engagement counter a capture reads to tell "retention was off" from
 * "retention was on and changed nothing".
 * @return The count. */
unsigned ui_freeze_retain_count(void);
/** @brief Returns the 2D primitives fed into the native scene since boot.
 *
 * Counts every primitive the module fed, from batch releases and from glyph
 * feeds at emission, including freeze-backdrop claims.
 * @return The count. */
unsigned ui_sprite_prim_count(void);

/** @brief Feeds every 2D primitive still claimed at the chokepoint into the native scene.
 *
 * Called from the DrawOTag thunk and from the scene's frame close. A 2D
 * primitive's words are not final when it is linked (a sprite mirrors its
 * own U coords right after the insert returns; the atlas page turn skews a
 * whole page of already-linked quads a tick later), and they are final once
 * the table is handed to the GPU. The canonical capture normally releases
 * the batch first; this call releases what it left. Inert with no claim
 * pending, which includes every game that registers no chokepoint. */
void ui_sprite_ot_draw_begin(void) OP_REQUIRES(sim);

/** @brief Re-reads the primitives released since the last walk and counts those whose words changed after their feed.
 *
 * Called from the DrawOTag thunk before ui_sprite_ot_draw_begin. The release
 * happens inside the VBL that built the batch and the walk is where the
 * words are final, so a nonzero stale count means the release point is too
 * early. OPENPETE_UISPRITE_TRACE prints each stale primitive;
 * OPENPETE_XY3_PROBE=<VBL> prints each primitive's y at link, release and
 * walk within three VBLs of it. */
void ui_sprite_ot_verify(void);

/** @brief Return one of the most recent ordering-table bases handed to DrawOTag.
 *
 * The thunk layer keeps the last PSYQ_OT_BASE_RING (8) bases, newest first;
 * a base equal to the newest is not recorded again, so alternating tables
 * fill alternate slots. The 2D feeder walks
 * them to rank its primitives by the traversal the GPU will perform, rather
 * than by the order it happened to be fed in.
 * @param i Age, 0 = newest.
 * @return The base's guest address, or 0 for `i` outside 0..7 or a slot not
 *         yet filled. */
uint32_t psyq_ot_recent_base(int i);

/** @brief Returns the 2D primitives the chokepoint's decode chain claimed no encoding for, since boot.
 *
 * The engagement counter for adding a decoder. Primitives linked inside the
 * fade emitter are not counted.
 * @return The count. */
unsigned ui_sprite_unclaimed_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_COMMON_RENDERER_UI_GAME_H */
