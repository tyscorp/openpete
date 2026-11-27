/* games/common/renderer/reextract_view.c - see renderer/reextract_view.h. */

#include "renderer/reextract_view.h"

#include "openpete_native_render.h"   /* openpete_native_register_reextract */
#include "openpete_profile.h"          /* the view's profiler regions */
#include "openpete_subtick_view.h"
#include "psx_runtime_internal.h"     /* CPUState */

#include <stddef.h>
#include "openpete_thread_roles.h"   /* OP_ROLE_ASSERT */

static void (*s_body)(void* cpu);

/** @brief The registered sub-tick re-extract: runs the game's body inside the sub-tick view.
 *
 * Begins the sub-tick view, runs the installed body when there is one, marks
 * the native scene as a replay, then ends the view.
 * @param cpu_v The guest CPU state, passed as CPUState*. */
static void reextract_view(void* cpu_v) {
    OP_ROLE_ASSERT(sim);
    CPUState* cpu = (CPUState*)cpu_v;
    { OP_PROF_ACCUM_SCOPE("rex.view_begin");
      openpete_subtick_view_begin(cpu); }
    if (s_body) s_body(cpu_v);
    openpete_native_scene_mark_replay();
    { OP_PROF_ACCUM_SCOPE("rex.view_end");
      openpete_subtick_view_end(cpu); }
}

void game_reextract_view_install(void (*body)(void* cpu)) {
    s_body = body;
    openpete_native_register_reextract(reextract_view);
}
