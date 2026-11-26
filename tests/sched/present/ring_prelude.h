/* ring_prelude.h - included first by the generated ring translation unit
 * (extract_ring.py): the ring's statics with external linkage for the
 * harness, and the native_render.c helpers the copied functions call,
 * stubbed. A helper a copied function gains and this file lacks is a
 * compile error naming it. */
#ifndef PT_RING_PRELUDE_H
#define PT_RING_PRELUDE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ring_types.h"
#include "openpete_mutex_once.h"
#include "openpete_native_render.h"
#include "openpete_log.h"
#include "fakevk.h"
#include "pt_harness.h"

scene_slot_t s_ring[SCENE_RING];
int          s_ring_head = -1;
void        *s_ring_mx;
struct pt_ring_census s_ring_census;
pt_nr_state  s_nr;
const scene_slot_t *s_ps_slot;
int          s_stage_target = -1;
static int   s_last_frame_presented;

/* The accumulator a publish moves into its slot. */
static mod_fvec s_scene_v;
static int      s_scene_n;
static uint32_t s_scene_bg, s_scene_fade, s_scene_freeze;
static int      s_scene_replay, s_scene_hud_under, s_scene_actor_front;
static uint32_t s_eager_stamp;
static unsigned s_scene_frame;
static nr_cover_block s_cover_live;
static nr_snap_table  s_snap_live;
static uint32_t s_scene_id_next = 1;
static int      s_scene_moved, s_scene_moved_slot;
static uint32_t s_scene_moved_id;

static void render_frame_common(uint64_t record_pts_us, bool have_ref);

/** @brief Stub: the slot's triangle storage always has room.
 * @param sl The slot.
 * @param n Triangles needed.
 * @return 1. */
static int slot_grow(scene_slot_t *sl, int n) { (void)sl; (void)n; return 1; }
/** @brief Stub: a refused publish keeps no build stamp. */
static void scene_keep_refused_stamp(void) {}
/** @brief Stub: no face-id debug view.
 * @param on Unused. */
static void scene_faceid_debug(int on) { (void)on; }
/** @brief Exchange two triangle runs, as native_render.c's helper does.
 * @param a One run.
 * @param b The other. */
static void scene_run_swap(mod_fvec *a, mod_fvec *b) { const mod_fvec t = *a; *a = *b; *b = t; }
/** @brief Stub: the slot carries no per-present compose inputs.
 * @param sl The slot. */
static void slot_capture_inputs(scene_slot_t *sl) { (void)sl; }
/** @brief Copy a contact-plane table.
 * @param d Destination.
 * @param s Source. */
static void snap_copy(nr_snap_table *d, const nr_snap_table *s) { *d = *s; }
/** @brief Stub: no present-trace census.
 * @param sl The slot. */
static void slot_note_census(scene_slot_t *sl) { (void)sl; }
/** @brief Stub: the harness never holds a compose.
 * @param frame Unused. */
static void hold_stall_for(unsigned frame) { (void)frame; }
/** @brief Stub: no queued pipeline variant change. */
static void variant_apply_pending(void) {}
/** @brief Stub: no material view.
 * @param v The view, or NULL. */
static void openpete_mods_material_view_set(const openpete_mods_material_view_t *v) { (void)v; }
/** @brief Stub: no present dump.
 * @param rendered Unused.
 * @param frame Unused.
 * @param stamp Unused.
 * @param freeze Unused.
 * @param kind Unused.
 * @param target_ns Unused.
 * @param alpha Unused.
 * @param paired Unused.
 * @param ntri Unused. */
static void pt_dump_maybe(int rendered, unsigned frame, uint32_t stamp, uint32_t freeze, int kind,
                          int64_t target_ns, double alpha, int paired, int ntri)
{
    (void)rendered; (void)frame; (void)stamp; (void)freeze; (void)kind; (void)target_ns; (void)alpha;
    (void)paired; (void)ntri;
}

static void *s_hold_mx;   /* the hold-redirect lock (native_render.c's hold_mx) */
/** @brief Take and drop the hold-redirect lock as the engine's hold_redirect_begin does on its census path; no held scene is redirected.
 * @param prev Receives the compose source to restore.
 * @return -1: the present composes as it is. */
static int hold_redirect_begin(const scene_slot_t **prev)
{
    *prev = s_ps_slot;
    SDL_Mutex *m = openpete_mutex_once(&s_hold_mx);
    SDL_LockMutex(m);
    SDL_UnlockMutex(m);
    return -1;
}
/** @brief End a hold redirect: nothing was redirected.
 * @param k hold_redirect_begin's return value.
 * @param prev The compose source it saved. */
static void hold_redirect_end(int k, const scene_slot_t *prev) { (void)k; (void)prev; }
/** @brief Take and drop the hold lock as the engine's freeze_bd_edge does reading the pin; no backdrop is rendered. */
static void freeze_bd_edge(void)
{
    SDL_Mutex *m = openpete_mutex_once(&s_hold_mx);
    SDL_LockMutex(m);
    SDL_UnlockMutex(m);
}
/** @brief Take and drop the hold lock as the engine's hold_slot_verdict does taking a slot's verdict; the slot references no held scene.
 * @param sl The slot. */
static void hold_slot_verdict(scene_slot_t *sl)
{
    (void)sl;
    SDL_Mutex *m = openpete_mutex_once(&s_hold_mx);
    SDL_LockMutex(m);
    SDL_UnlockMutex(m);
}
/** @brief Take and drop the hold lock as the engine's hold_slot_release does, nested under the ring lock when a slot goes free.
 * @param sl The slot. */
static void hold_slot_release(scene_slot_t *sl)
{
    (void)sl;
    SDL_Mutex *m = openpete_mutex_once(&s_hold_mx);
    SDL_LockMutex(m);
    SDL_UnlockMutex(m);
}
/* The ring slot kept as the live freeze-backdrop source; the harness never keeps one. */
static int s_ring_bd_keep = -1;
/** @brief Take and drop the ring lock as the engine's freeze_bd_note_present does recording the kept slot; no slot is kept.
 * @param slot The present's ring slot.
 * @param did Nonzero when the present went out. */
static void freeze_bd_note_present(int slot, int did)
{
    (void)slot; (void)did;
    SDL_Mutex *m = openpete_mutex_once(&s_ring_mx);
    SDL_LockMutex(m);
    SDL_UnlockMutex(m);
}

#endif
