/* stubs.c - every external callee of the protocol objects that is not the
 * scheduler's wraps, the fake device or the copied ring: the clock, SDL's
 * window and display queries, and the engine's pace log, profiler, log,
 * crash, VRAM, PSX RAM, config and thread-role entry points.
 *
 * The clock reads (op_now) and sleeps (op_sleep_until) and the window size
 * query are yields over the virtual clock and the scenario's window; every
 * other stub is no-yield and has no effect. The harness links no catch-all
 * library, so a callee the protocol objects gain is a link error until a
 * stub with a declared class exists here. */
#define _GNU_SOURCE
#include "op_sched.h"
#include "fakevk.h"
#include "pt_harness.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "openpete_time.h"
#include "openpete_log.h"
#include "openpete_pace_log.h"
#include "openpete_profile.h"
#include "openpete_config.h"
#include "openpete_crash.h"
#include "openpete_native_vram.h"
#include "openpete_psx.h"
#include "openpete_thread_roles.h"

/* Objects naming the clock seams in the trace and the step hook. */
const char pt_seam_op_now[] = "op_now", pt_seam_op_sleep[] = "op_sleep_until", pt_seam_window[] = "window-size";

/* ---- clock: yields over the virtual clock ---- */
op_time_t op_now(void)
{
    sched_point_ra(SCHED_OP_CLOCK, pt_seam_op_now, __builtin_return_address(0));
    return sched_now();
}
void op_sleep_until(op_time_t deadline_ns)
{
    if (!sched_active()) return;
    pt_note_sleep();
    if (deadline_ns <= sched_now()) {
        sched_point_ra(SCHED_OP_SLEEP, pt_seam_op_sleep, __builtin_return_address(0));
        return;
    }
    sched_sleep_until(deadline_ns, __builtin_return_address(0));
}

/* ---- SDL: window, display, surface ---- */
int pt_standin_size_read;

bool SDL_GetWindowSizeInPixels(SDL_Window *w, int *x, int *y)
{
    (void)w;
    if (sched_active()) pt_site_hit(pt_standin_size_read ? PT_SITE_WINDOW_COMPOSE : PT_SITE_WINDOW_RHI);
    pt_standin_size_read = 0;
    sched_point_ra(SCHED_OP_POINT, pt_seam_window, __builtin_return_address(0));
    int ww = 0, hh = 0;
    fakevk_window(&ww, &hh);
    if (x) *x = ww;
    if (y) *y = hh;
    return true;
}
static const char *const k_iext[] = { "VK_KHR_surface", "VK_KHR_xlib_surface" };
char const *const *SDL_Vulkan_GetInstanceExtensions(Uint32 *n) { *n = 2; return k_iext; }
bool SDL_Vulkan_CreateSurface(SDL_Window *w, VkInstance i, const struct VkAllocationCallbacks *a, VkSurfaceKHR *s)
{
    (void)w; (void)i; (void)a;
    *s = (VkSurfaceKHR)(uintptr_t)0x77770;
    return true;
}
SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window *w) { (void)w; return 1; }
static SDL_DisplayMode s_mode = { .displayID = 1, .w = 2560, .h = 1440, .refresh_rate = 480.0f,
                                  .refresh_rate_numerator = 480, .refresh_rate_denominator = 1 };
const SDL_DisplayMode *SDL_GetCurrentDisplayMode(SDL_DisplayID d) { (void)d; return &s_mode; }
const char *SDL_GetError(void) { return ""; }
int SDL_GetNumLogicalCPUCores(void) { return 16; }
SDL_WindowID SDL_GetWindowID(SDL_Window *w) { (void)w; return 1; }
bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority p) { (void)p; return true; }
char *SDL_strcasestr(const char *h, const char *n) { return strcasestr(h, n); }
bool SDL_GetCurrentTime(SDL_Time *t) { *t = 0; return true; }
bool SDL_GetPathInfo(const char *p, SDL_PathInfo *i) { (void)p; (void)i; return false; }
char **SDL_GlobDirectory(const char *p, const char *pat, SDL_GlobFlags f, int *n)
{
    (void)p; (void)pat; (void)f;
    if (n) *n = 0;
    return NULL;
}
bool SDL_RemovePath(const char *p) { (void)p; return false; }
void SDL_free(void *p) { free(p); }
/* The scene ring's once-created mutex slot: an idempotent first-use cache (once). */
void *SDL_GetAtomicPointer(void **a)
{
    sched_tsan_acquire(a);
    return __atomic_load_n(a, __ATOMIC_SEQ_CST);
}
bool SDL_CompareAndSwapAtomicPointer(void **a, void *oldval, void *newval)
{
    sched_tsan_acquire(a);
    const bool ok = __atomic_compare_exchange_n(a, &oldval, newval, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    if (ok) sched_tsan_release(a);
    return ok;
}

/* ---- engine externals: no-yield, no effect ---- */
void op_role__bind_present(op_role_site_t *site) { (void)site; }
void op_role__bind_sim(op_role_site_t *site) { (void)site; }
void op_role__bind_compile(op_role_site_t *site) { (void)site; }
void op_role__bind_pcache(op_role_site_t *site) { (void)site; }
void openpete_logf(openpete_log_level_t level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}
const char *openpete_config_env(const char *name) { return getenv(name); }
void openpete_crash_thread_attach(void) {}
void openpete_vram_bind_composed(void) {}
uint8_t *openpete_psx_get_ram(void) { return NULL; }
void openpete_pace_flip(uint64_t id, int64_t t_ns, int64_t waited_ns) { (void)id; (void)t_ns; (void)waited_ns; }
void openpete_pace_record_present(uint8_t kind, double alpha, int paired) { (void)kind; (void)alpha; (void)paired; }
int openpete_pace_record_sheds(uint8_t kind) { (void)kind; return 0; }
void openpete_pace_split_add(int which, int64_t ns) { (void)which; (void)ns; }
int op_prof_on(void) { return 0; }
int op_prof_intern(const char *name) { (void)name; return 0; }
void op_prof_begin_flags(int name_id, unsigned flags) { (void)name_id; (void)flags; }
void op_prof_end(void) {}
