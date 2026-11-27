/* games/common/renderer/player_ranges.c - see player_ranges.h. */
#include "renderer/player_ranges.h"

#include "renderer/cut_guard.h"      /* cut_held_set_pick, cut_census_held_set */
#include "renderer/orchestrator.h"   /* the canonical-render epoch */
#include "openpete_psx.h"            /* openpete_psx_to_host_span */
#include "openpete_subtick_view.h"
#include "game_layout.h"

#include <string.h>
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

#define PLAYER_RANGE_SLOTS 2

typedef struct {
    uint8_t  copy[0x200];
    uint32_t off, base, len, epoch;
} player_range;

typedef struct {
    player_range slot[PLAYER_RANGE_SLOTS];
    uint32_t     epoch;   /* the canonical epoch this generation was last captured in */
} player_range_gen;

static player_range_gen s_gen[2];
static player_range_gen* s_cur  = &s_gen[0];
static player_range_gen* s_prev = &s_gen[1];

void player_ranges_capture(CPUState* cpu, unsigned off, unsigned len) OP_REQUIRES(sim) {
    const uint32_t epoch = spyro_native_canon_epoch_stamp();
    if (s_cur->epoch != epoch) {
        *s_prev = *s_cur;
        s_cur->epoch = epoch;
    }
    player_range* r = s_cur->slot;
    int slot = -1;
    for (int i = 0; i < PLAYER_RANGE_SLOTS; i++)
        if (r[i].len && r[i].off == off) { slot = i; break; }
    if (slot < 0)
        for (int i = 0; i < PLAYER_RANGE_SLOTS; i++) if (!r[i].len) { slot = i; break; }
    if (slot < 0) return;
    r[slot].len = 0;
    if (!len || len > sizeof r[slot].copy || !game_layout.polybuf.end) return;
    uint32_t end = cpu->read_word(game_layout.polybuf.end);
    if (end < 0x80000000u) return;
    uint32_t base = end - game_layout.player.part_base_polybuf_off + off;
    const void* host = openpete_psx_to_host_span(base, len);
    if (!host) return;
    memcpy(r[slot].copy, host, len);
    r[slot].off   = off;
    r[slot].base  = base;
    r[slot].len   = len;
    r[slot].epoch = epoch;
}

/** @brief Stages every captured range of one generation whose slots are all of its epoch.
 *
 * @param g The generation.
 * @return The number of ranges staged, or 0 when none is captured or any
 *         range is stale or unstaged. */
static int ranges_stage_gen(const player_range_gen* g) {
    int staged = 0;
    for (int i = 0; i < PLAYER_RANGE_SLOTS; i++) {
        const player_range* r = &g->slot[i];
        if (!r->len) continue;
        if (r->epoch != g->epoch) return 0;
        void* stage = openpete_subtick_view_stage(r->base, r->len);
        if (!stage) return 0;
        memcpy(stage, r->copy, r->len);
        staged++;
    }
    return staged;
}

int player_ranges_stage(int hold) {
    if (!hold)
        return spyro_native_canon_epoch_fresh(s_cur->epoch) ? ranges_stage_gen(s_cur) : 0;
    const cut_held_pick_t pick = cut_held_set_pick(s_cur->epoch, s_prev->epoch);
    const player_range_gen* g = pick == CUT_HELD_PREV ? s_prev
                              : pick == CUT_HELD_CURR ? s_cur : NULL;
    const int n = g ? ranges_stage_gen(g) : 0;
    cut_census_held_set("player-ranges", pick, g ? g->epoch : s_cur->epoch, n);
    return n;
}
