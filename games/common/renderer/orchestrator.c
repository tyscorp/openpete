/* orchestrator.c - the orchestrator entries of renderer/orchestrator.h,
 * forwarded to the running game's game_orchestrator_t.
 *
 * game_orchestrator_bind resets every slot to the inert entry of a game with
 * no sub-tick re-extract spine (no bracket is active, each commit does
 * nothing, and every producer reports never-fresh, so each feeder skips its
 * replay and reads canonical RAM), then takes each slot the game fills: the
 * bound table depends on the last game bound alone, whatever was bound
 * before it. */

#include "renderer/orchestrator.h"

#include <stdio.h>

#include "openpete_fiber.h"          /* openpete_frame */
#include "openpete_native_render.h"  /* openpete_native_scene_note_game_capture */
#include "openpete_thread_roles.h"   /* OP_REQUIRES */

/** @brief Report no interpolated position: no bracket is active. */
static int none_interp_pos(unsigned int key, double out[3]) {
    (void)key; (void)out;
    return 0;
}
/** @brief Do nothing: no game entry to run. */
static void none_void(void) {}
/** @brief Return the epoch of a game with no sub-tick spine, 0. */
static uint32_t none_epoch_stamp(void) { return 0u; }
/** @brief Report a capture stamp never fresh. */
static int none_epoch_fresh(uint32_t stamp) { (void)stamp; return 0; }
/** @brief Report a producer's capture never fresh. */
static int none_fresh(void) { return 0; }
/** @brief Report no retired record capture. */
static const uint8_t* none_prev_recs(uint32_t* len, op_time_t* ref_ns) {
    (void)len; (void)ref_ns;
    return NULL;
}
/** @brief Capture nothing. */
static void none_capture_range(void* cpu, unsigned off, unsigned len) {
    (void)cpu; (void)off; (void)len;
}
/** @brief Capture nothing. */
static void none_capture(void* cpu) { (void)cpu; }
/** @brief Record no producer camera. */
static void none_capture_camera(void* cpu, int producer) { (void)cpu; (void)producer; }
/** @brief Install no producer camera. */
static int none_prod_camera_enter(int producer) { (void)producer; return 0; }

/* The entries of a game that fills no slot. */
#define ORCH_NONE { \
    .reextract_init    = none_void, \
    .interp_pos        = none_interp_pos, \
    .epoch_commit      = none_void, \
    .epoch_supersede   = none_void, \
    .epoch_stamp       = none_epoch_stamp, \
    .epoch_fresh       = none_epoch_fresh, \
    .mobyreg_fresh     = none_fresh, \
    .shaded_fresh      = none_fresh, \
    .capture_range     = none_capture_range, \
    .capture_recs      = none_capture, \
    .capture_sxy       = none_capture, \
    .capture_camera    = none_capture_camera, \
    .prev_recs         = none_prev_recs, \
    .prod_camera_enter = none_prod_camera_enter, \
    .prod_camera_leave = none_void, \
}
static const game_orchestrator_t k_orch_none = ORCH_NONE;
/* The running game's entries, every slot non-NULL after the bind. */
static game_orchestrator_t s_orch = ORCH_NONE;
#undef ORCH_NONE
static void (*s_interp_register)(void) = none_void;

/* Take the game's slot when it fills one; the reset left the inert entry. */
#define BIND(slot) if (o->slot) s_orch.slot = o->slot

void game_orchestrator_bind(const game_orchestrator_t* o, void (*interp_register)(void)) {
    s_orch = k_orch_none;
    s_interp_register = interp_register ? interp_register : none_void;
    if (!o) return;
    BIND(reextract_init);
    BIND(interp_pos);
    BIND(epoch_commit);
    BIND(epoch_supersede);
    BIND(epoch_stamp);
    BIND(epoch_fresh);
    BIND(mobyreg_fresh);
    BIND(shaded_fresh);
    BIND(capture_range);
    BIND(capture_recs);
    BIND(capture_sxy);
    BIND(capture_camera);
    BIND(prev_recs);
    BIND(prod_camera_enter);
    BIND(prod_camera_leave);
}

#undef BIND

/** @brief Lets a producer capture of the game's render supersede the pending unpresented scene before the capture takes the epoch.
 *
 * Every capture stamps the in-flight epoch through these entries; a render
 * at a newer VBL than the pending game scene moves the epoch here, before
 * its first stamp, so all of the render's captures share one epoch whichever
 * path feeds first. */
static void capture_note(void) OP_REQUIRES(sim) { openpete_native_scene_note_game_capture(openpete_frame); }

int openpete_native_interp_pos(unsigned int key, double out[3]) { return s_orch.interp_pos(key, out); }
void spyro_native_canon_epoch_commit(void) { s_orch.epoch_commit(); }
void spyro_native_canon_epoch_supersede(void) { s_orch.epoch_supersede(); }
uint32_t spyro_native_canon_epoch_stamp(void) OP_REQUIRES(sim) { capture_note(); return s_orch.epoch_stamp(); }
int spyro_native_canon_epoch_fresh(uint32_t stamp) { return s_orch.epoch_fresh(stamp); }
int spyro_native_canon_mobyreg_fresh(void) { return s_orch.mobyreg_fresh(); }
int spyro_native_canon_shaded_fresh(void) { return s_orch.shaded_fresh(); }
void spyro_native_canon_capture_range(void* cpu, unsigned off, unsigned len) OP_REQUIRES(sim) {
    capture_note();
    s_orch.capture_range(cpu, off, len);
}
void spyro_native_canon_capture_recs(void* cpu) OP_REQUIRES(sim) { capture_note(); s_orch.capture_recs(cpu); }
const uint8_t* spyro_native_canon_prev_recs(uint32_t* len, op_time_t* ref_ns) {
    return s_orch.prev_recs(len, ref_ns);
}
void spyro_native_canon_capture_sxy(void* cpu) OP_REQUIRES(sim) { capture_note(); s_orch.capture_sxy(cpu); }
void spyro_native_canon_capture_camera(void* cpu, int producer) OP_REQUIRES(sim) {
    capture_note();
    s_orch.capture_camera(cpu, producer);
}
int spyro_native_prod_camera_enter(int producer) { return s_orch.prod_camera_enter(producer); }
void spyro_native_prod_camera_leave(void) { s_orch.prod_camera_leave(); }
void game_interp_register(void) { s_interp_register(); }
void game_reextract_init(void) { s_orch.reextract_init(); }

void game_orchestrator_dump(FILE* out) {
    static const char* const k_slot[] = {
        "reextract_init", "interp_pos", "epoch_commit", "epoch_supersede", "epoch_stamp",
        "epoch_fresh", "mobyreg_fresh", "shaded_fresh", "capture_range", "capture_recs",
        "capture_sxy", "capture_camera", "prod_camera_enter", "prod_camera_leave"};
    const void* const v[] = {
        (const void*)s_orch.reextract_init, (const void*)s_orch.interp_pos,
        (const void*)s_orch.epoch_commit, (const void*)s_orch.epoch_supersede,
        (const void*)s_orch.epoch_stamp, (const void*)s_orch.epoch_fresh,
        (const void*)s_orch.mobyreg_fresh, (const void*)s_orch.shaded_fresh,
        (const void*)s_orch.capture_range, (const void*)s_orch.capture_recs,
        (const void*)s_orch.capture_sxy, (const void*)s_orch.capture_camera,
        (const void*)s_orch.prod_camera_enter, (const void*)s_orch.prod_camera_leave};
    for (size_t i = 0; i < sizeof k_slot / sizeof k_slot[0]; i++)
        fprintf(out, "orchestrator.%s %p\n", k_slot[i], v[i]);
    fprintf(out, "orchestrator.interp_register %p\n", (const void*)s_interp_register);
}
