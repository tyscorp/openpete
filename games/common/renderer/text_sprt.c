// games/common/renderer/text_sprt.c - native renderer feed for the SPRT text
// chain.
//
// Most of a game's 2D layer reaches the shared feeder through the
// ordering-table chokepoint (ui_game.ot_insert). The glyph emitters do not:
// each builds its own tag-to-tag chain of glyph packets in the primitive
// buffer, then links the whole chain into the 2D ordering table with an
// inlined insert, so the chokepoint never sees a glyph. Two packet forms are
// emitted: one 0xC-byte DR_MODE followed by one 0x14-byte SPRT per glyph, and
// one 0x28-byte POLY_FT4 per glyph carrying its own texture page and CLUT
// (Spyro 2 `func_8004A59C`, mobyfunc.s 8004A778; Spyro 3 `func_8002E970`,
// mobyfunc.s 8002EB4C, both storing the 0x2C command byte at packet offset 7).
//
// Reproducing the glyph walk would mean re-implementing the emitters' escape
// handling, per-colour line heights and clip range. Instead this module runs
// the real emitter and walks the chain it just left in the primitive buffer:
// one linear walk over the bytes the primitive cursor advanced past finds
// every glyph packet, and each is claimed into the chokepoint's batch, whose
// release reads the packet's words. The words are not final when the emitter
// returns: Spyro 2's pause panel page turn (func_800145AC) rewrites the xy of
// every primitive it emitted, glyphs included, through func_8004BF94. An SPRT
// carries no texture page, so the walk tracks the DR_MODE that precedes it
// (the per-glyph-DR_MODE variant rewrites that page inside every packet,
// which the same tracking picks up).
//
// Display-only: the hook reads the primitive buffer and the real emitter runs
// unchanged, so no gameplay-readable byte moves.

#include "feature_registry.h"
#include "psx_runtime_internal.h"
#include "openpete_native_render.h"
#include "openpete_recomp_module.h"
#include "game_layout.h"
#include "renderer/ui_game.h"
#include "renderer/render_paths.h"   /* --render-paths: RP_UI */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "openpete_pace_log.h"      /* OP_PACE_SPLIT_SCOPE: the guest call's split */
#include "openpete_profile.h"       /* the guest call's profiler region */
#include "openpete_thread_roles.h"

/* Chains walked and glyphs claimed since boot: the engagement counters a
 * capture reads to tell "no text drew" from "text drew identically". */
static unsigned s_chains, s_glyphs;

/** @brief Reports whether the text chain census is enabled by OPENPETE_TEXT_SPRT_CENSUS.
 *
 * Latched on the first call.
 * @return 1 when the variable is set to a non-empty value not starting with
 *         '0', else 0. */
static int census_on(void) {
    static int v = -1;
    if (v < 0) { const char* e = getenv("OPENPETE_TEXT_SPRT_CENSUS"); v = (e && *e && *e != '0'); }
    return v;
}

/** @brief Walks a primitive-buffer range as contiguous packets and claims each glyph packet for the UI sprite batch release.
 *
 * A packet's tag word holds its length in words in the top byte, so the
 * next packet starts (len + 1) * 4 bytes on; the command byte is the top
 * byte of word 1. A DR_MODE (0xE1) sets the texture page for the SPRTs
 * (0x64..0x67) after it, which are claimed with that packet's address; a
 * SPRT before any DR_MODE is skipped. A POLY_FT4 (0x2C..0x2F) carries its
 * own page. The walk stops at a zero length or a packet that overruns the
 * range. Counts the chain and each glyph claimed. A chain the emitter
 * linked itself is one link appended to the 2D entry when the emitter
 * returns, so each glyph is recorded there for the link rank, in chain
 * order, before it is claimed.
 * @param cpu The guest CPU state.
 * @param start The first packet's guest address.
 * @param end The primitive cursor after the emitter ran.
 * @param self_linked 1 when no hooked insert linked any of the chain. */
static void feed_chain(CPUState* cpu, uint32_t start, uint32_t end, int self_linked) OP_REQUIRES(sim) {
    uint32_t dr_mode = 0;               /* the chain's own DR_MODE packet */
    uint32_t p = start;
    while (p + 8u <= end) {
        uint32_t w0 = cpu->read_word(p);
        unsigned len = (w0 >> 24) & 0xFFu;
        if (len == 0u) break;           /* not a tagged packet: stop rather than drift */
        uint32_t size = (len + 1u) * 4u;
        if (p + size > end) break;
        unsigned code = (cpu->read_word(p + 4) >> 24) & 0xFFu;
        if (code == 0xE1u) {            /* DR_MODE: bits 0-10 are the draw mode */
            dr_mode = p;
        } else if ((code & 0xFCu) == 0x64u && dr_mode) {   /* SPRT 0x64..0x67 */
            if (self_linked) ui_sprite_link_note(p, -1);
            ui_sprite_claim_glyph_sprt(cpu, p, dr_mode);
            s_glyphs++;
        } else if ((code & 0xFCu) == 0x2Cu) {   /* POLY_FT4 0x2C..0x2F, len 9 -> 0x28 bytes */
            if (self_linked) ui_sprite_link_note(p, -1);
            ui_sprite_claim_glyph_ft4(cpu, p);  /* the packet carries its own tpage */
            s_glyphs++;
        }
        p += size;
    }
    s_chains++;
}

/** @brief Dispatch hook for a glyph chain emitter: runs it, then claims the chain it left in the primitive buffer.
 *
 * Inert (returns 0, so the normal dispatch runs the emitter) while the native
 * scene is disabled and no render-path selection is active, and on re-entry
 * from its own dispatch. Otherwise reads the polybuf cursor, dispatches the
 * real emitter and reads the cursor again; under a selection the advanced
 * range is recorded as RP_UI, and with the native scene on and RP_UI
 * selected it is walked with feed_chain, told whether a hooked insert
 * recorded a link during the call. Reads guest RAM only. Prints a
 * census line at most once per 100 openpete_frame values under
 * OPENPETE_TEXT_SPRT_CENSUS.
 * @param cpu The guest CPU state.
 * @param addr The emitter's guest address.
 * @return 1 when the emitter ran here, 0 to leave dispatch to the caller. */
static int on_text_emitter(CPUState* cpu, uint32_t addr) {
    OP_ROLE_ASSERT(sim);
    const int feed = openpete_native_scene_enabled();
    const int rec  = render_paths_active();
    if (!feed && !rec) return 0;                      /* inert: normal dispatch runs it */
    static int s_in = 0;
    if (s_in) return 0;                               /* re-entry from our own call */
    s_in = 1;

    uint32_t start = cpu->read_word(game_layout.polybuf.cursor);
    const uint64_t links = ui_sprite_link_count();
    { OP_PROF_FLAGS_ACCUM_SCOPE("text.srend", OP_PROF_F_GUEST); OP_PACE_SPLIT_SCOPE(OP_PACE_SPLIT_GUEST);
      openpete_recomp_module_get()->dispatch(cpu, addr); } /* the real emitter (ref unchanged) */
    uint32_t end = cpu->read_word(game_layout.polybuf.cursor);
    /* No hooked insert ran: the emitter linked its chain into the 2D entry
     * itself (the loading picture's chain goes through the slotted insert). */
    const int self_linked = ui_sprite_link_count() == links;

    /* The chain is the 2D layer's: its range keeps it in the OT walk while
     * RP_UI is selected, and RP_UI deselected keeps it out of the native feed. */
    if (rec) render_path_record(RP_UI, start, end);
    if (feed && end > start && render_path_enabled(RP_UI)) feed_chain(cpu, start, end, self_linked);

    if (census_on()) {
        static unsigned last = 0xFFFFFFFFu;
        extern unsigned openpete_frame;
        if (openpete_frame / 100u != last) {
            last = openpete_frame / 100u;
            fprintf(stderr, "[census f=%u TEXT-SPRT] chains=%u glyphs=%u bytes=%u\n",
                    openpete_frame, s_chains, s_glyphs, end - start);
        }
    }
    s_in = 0;
    return 1;
}

/* The hook addresses are the descriptor's, bound at boot: a static
 * initializer cannot read another TU's const. Each entry carries the overlay
 * its emitter lives in beside the address, so an emitter inside the overlay
 * window is dispatched only while that overlay is resident and the unrelated
 * function sharing its address in the other overlays runs untouched. */
static feature_dispatch_entry_t text_sprt_dispatch[] = {
    { 0u, on_text_emitter, 0u },
    { 0u, on_text_emitter, 0u },
    { 0u, on_text_emitter, 0u },
    { 0u, NULL, 0u },
};

/** @brief Fills the dispatch table from the game's SPRT chain emitter list.
 *
 * Each of the first three ui_game.sprt_chain_emitter entries that is
 * non-zero becomes a table row with its overlay id; the table is then
 * terminated.
 * @param opts Unused. */
static void text_sprt_apply_options(const openpete_options_t* opts) {
    OP_ROLE_ASSERT(sim);
    (void)opts;
    int n = 0;
    for (int i = 0; i < 3; i++)
        if (ui_game.sprt_chain_emitter[i]) {
            text_sprt_dispatch[n].addr    = ui_game.sprt_chain_emitter[i];
            text_sprt_dispatch[n].fn      = on_text_emitter;
            text_sprt_dispatch[n].overlay = ui_game.sprt_chain_overlay[i];
            n++;
        }
    text_sprt_dispatch[n].addr    = 0u;
    text_sprt_dispatch[n].fn      = NULL;
    text_sprt_dispatch[n].overlay = 0u;
}

const feature_module_t text_sprt_module = {
    .name           = "text_sprt_native",
    .apply_options  = text_sprt_apply_options,
    .dispatch_table = text_sprt_dispatch,
};
