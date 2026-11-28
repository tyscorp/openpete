// core/ui/freecam.c - host-side state for the debug free camera (F).
// See openpete_freecam.h. Generic, address-free; the camera drive and
// function-freeze live in games/common/freecam_game.c behind each game's
// descriptor (games/<game>/features/freecam.c).

#include "openpete_freecam.h"
#include "openpete_bugrec.h"
#include "openpete_log.h"
#include "openpete_rewind.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static bool                  s_active      = false;
static openpete_freecam_axes s_axes        = {0};
static int                   s_speed_index = 2;   /* "normal" (middle step) */
static atomic_bool           s_teleport_req = false;
/* Pending engage request: -1 none, 0 disengage, 1 engage. */
static atomic_int            s_active_req = -1;
/* Pending speed step request: -1 none, else the step. */
static atomic_int            s_speed_req = -1;

bool openpete_freecam_active(void) { return s_active; }

static _Atomic unsigned s_engage_gen;
static _Atomic float    s_mouse_sens = 1.0f;

unsigned openpete_freecam_engage_generation(void) {
    return atomic_load_explicit(&s_engage_gen, memory_order_relaxed);
}

void openpete_freecam_set_mouse_sensitivity(float s) {
    if (!(s >= 0.0f)) return;   /* rejects negatives and NaN */
    atomic_store_explicit(&s_mouse_sens, s, memory_order_relaxed);
}

float openpete_freecam_mouse_sensitivity(void) {
    return atomic_load_explicit(&s_mouse_sens, memory_order_relaxed);
}

static bool s_bk2_active = false;
bool openpete_bk2_active(void) { return s_bk2_active; }
void openpete_bk2_set_active(bool active) { s_bk2_active = active; }

bool openpete_freecam_toggle(void) {
    if (!s_active && s_bk2_active) {
        OPL_WARN_F("freecam", "engage refused: a bk2 movie is replaying or recording");
        return false;
    }
    s_active = !s_active;
    if (s_active) openpete_bugrec_taint(BUGREC_TAINT_FREECAM);
    /* Engaged, the free camera freezes the player update and owns the
     * camera follow; a rewind replay across the edge would rebuild the
     * ticks on the other side of it. */
    openpete_rewind_note_intervention(s_active ? "free camera engage" : "free camera disengage");
    atomic_fetch_add_explicit(&s_engage_gen, 1u, memory_order_relaxed);
    memset(&s_axes, 0, sizeof s_axes);
    /* A request only makes sense against the pose that was on screen when it
     * was made, so it never survives a toggle in either direction. */
    atomic_store(&s_teleport_req, false);
    if (s_active) {
        OPL_INFO_F("freecam",
                   "ON - WASD move, arrows look, Space/LShift up/down, "
                   "[ ] speed (step %d/%d), T teleport Spyro here",
                   s_speed_index, OPENPETE_FREECAM_SPEED_STEPS - 1);
    } else {
        OPL_INFO_F("freecam", "OFF - camera returned to the game");
    }
    return true;
}

void openpete_freecam_set_axes(const openpete_freecam_axes* ax) {
    if (ax) s_axes = *ax;
}

void openpete_freecam_get_axes(openpete_freecam_axes* out) {
    if (out) *out = s_axes;
}

static _Atomic long long s_look_count_x, s_look_count_y;

void openpete_freecam_add_look_counts(long long dx, long long dy) {
    if (dx) atomic_fetch_add_explicit(&s_look_count_x, dx, memory_order_relaxed);
    if (dy) atomic_fetch_add_explicit(&s_look_count_y, dy, memory_order_relaxed);
}

void openpete_freecam_look_counts(long long* x, long long* y) {
    if (x) *x = atomic_load_explicit(&s_look_count_x, memory_order_relaxed);
    if (y) *y = atomic_load_explicit(&s_look_count_y, memory_order_relaxed);
}

void openpete_freecam_request_teleport(void) {
    if (s_active) atomic_store(&s_teleport_req, true);
}

bool openpete_freecam_take_teleport_request(void) {
    return atomic_exchange(&s_teleport_req, false);
}

void openpete_freecam_request_active(bool on) { atomic_store(&s_active_req, on ? 1 : 0); }
int  openpete_freecam_take_active_request(void) { return atomic_exchange(&s_active_req, -1); }
int  openpete_freecam_pending_active(void) { return atomic_load(&s_active_req); }
void openpete_freecam_request_speed(int step) {
    atomic_store(&s_speed_req, step < 0 ? 0 : step);
}
int  openpete_freecam_take_speed_request(void) { return atomic_exchange(&s_speed_req, -1); }

int openpete_freecam_speed_index(void) {
    if (s_speed_index < 0) return 0;
    if (s_speed_index >= OPENPETE_FREECAM_SPEED_STEPS)
        return OPENPETE_FREECAM_SPEED_STEPS - 1;
    return s_speed_index;
}

void openpete_freecam_adjust_speed(int dir) {
    openpete_freecam_set_speed(s_speed_index + (dir > 0 ? 1 : (dir < 0 ? -1 : 0)));
}

void openpete_freecam_set_speed(int n) {
    if (n < 0) n = 0;
    if (n >= OPENPETE_FREECAM_SPEED_STEPS) n = OPENPETE_FREECAM_SPEED_STEPS - 1;
    if (n != s_speed_index) {
        s_speed_index = n;
        OPL_INFO_F("freecam", "speed step %d/%d",
                   s_speed_index, OPENPETE_FREECAM_SPEED_STEPS - 1);
    }
}
