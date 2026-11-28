// core/ui/camlock.c - host-side state for the render camera lock (G).
// See openpete_camlock.h. Generic, address-free; the g_Camera bracket write
// lives in the game's re-extract orchestrator.

#include "openpete_camlock.h"
#include "openpete_freecam.h"   /* shared fly axes + speed step */
#include "openpete_host_mouse.h" /* probe log */
#include "openpete_log.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#  define M_PI    3.14159265358979323846
#endif
#ifndef M_PI_2
#  define M_PI_2  1.57079632679489661923
#endif

/* Spyro angle units: a full turn is 0x1000 (4096). */
#define CL_ANGLE_FULL  4096.0
/* Same pitch clamp as the freecam (~87 degrees, camera can't flip). */
#define CL_PITCH_LIMIT 992.0   /* 0x3E0 */

/* Per-TICK move (world units) / turn (angle units) magnitudes per speed
 * step - the freecam's tables (the Spyro 1 freecam descriptor); the shared speed index
 * selects the row so both cameras feel identical at a given step. */
static const double s_move_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 25, 100, 300, 500, 800 };
static const double s_turn_speed[OPENPETE_FREECAM_SPEED_STEPS] = { 10,  25,  35,  50,  75 };

static _Atomic unsigned s_engage_gen;
/* Pending lock request: -1 none, 0 disengage, 1 engage. */
static atomic_int s_lock_req = -1;

void openpete_camlock_request(bool on) { atomic_store(&s_lock_req, on ? 1 : 0); }
int  openpete_camlock_take_request(void) { return atomic_exchange(&s_lock_req, -1); }

unsigned openpete_camlock_engage_generation(void) {
    return atomic_load_explicit(&s_engage_gen, memory_order_relaxed);
}

static bool   s_active     = false;
static bool   s_pose_valid = false;
static bool   s_moved      = false;
static double s_x, s_y, s_z;      /* world units */
static double s_yaw, s_pitch;     /* game angle units */

/** @brief Scan up to max signed decimal integers out of arbitrary text, skipping every other character.
 *
 * Same contract as the freecam pose parser, so a whole "camlock:" log line
 * pastes verbatim into OPENPETE_CAMLOCK_POSE.
 * @param s The text.
 * @param out Receives the integers.
 * @param max The capacity of out.
 * @return The number of integers stored. */
static int cl_parse_ints(const char* s, long* out, int max) {
    int n = 0;
    while (*s && n < max) {
        int is_num = (*s >= '0' && *s <= '9') ||
                     ((*s == '-' || *s == '+') && s[1] >= '0' && s[1] <= '9');
        if (!is_num) { s++; continue; }
        char* end;
        out[n++] = strtol(s, &end, 10);
        s = end;
    }
    return n;
}

/** @brief Read the camlock environment variables once, on the first query or toggle.
 *
 * OPENPETE_CAMLOCK_POSE="x,y,z[,yaw,pitch]" with at least three numbers
 * engages at that pose, marked valid and moved; fewer draws a warning.
 * Otherwise OPENPETE_CAMLOCK set to a value not starting with '0' engages
 * with the pose left for the game side to latch. */
static void cl_env_init(void) {
    static int s_done = 0;
    if (s_done) return;
    s_done = 1;
    const char* pose = getenv("OPENPETE_CAMLOCK_POSE");
    const char* eng  = getenv("OPENPETE_CAMLOCK");
    if (pose && *pose) {
        long v[5] = {0, 0, 0, 0, 0};
        int n = cl_parse_ints(pose, v, 5);
        if (n >= 3) {
            s_x = (double)v[0]; s_y = (double)v[1]; s_z = (double)v[2];
            s_yaw   = (n >= 4) ? (double)v[3] : 0.0;
            s_pitch = (n >= 5) ? (double)v[4] : 0.0;
            s_pose_valid = true;
            s_moved      = true;   /* scripted pose: build matrices from it */
            s_active     = true;
            atomic_fetch_add_explicit(&s_engage_gen, 1u, memory_order_relaxed);
            OPL_INFO_F("camlock", "scripted pose (%ld, %ld, %ld) yaw=%ld pitch=%ld",
                       v[0], v[1], v[2], n >= 4 ? v[3] : 0, n >= 5 ? v[4] : 0);
        } else {
            OPL_WARN_F("camlock",
                       "OPENPETE_CAMLOCK_POSE needs at least x,y,z (got %d numbers)", n);
        }
    }
    if (!s_active && eng && *eng && *eng != '0') {
        s_active = true;
        atomic_fetch_add_explicit(&s_engage_gen, 1u, memory_order_relaxed);
    }
}

bool openpete_camlock_active(void) {
    cl_env_init();
    return s_active;
}

void openpete_camlock_toggle(void) {
    cl_env_init();
    s_active = !s_active;
    atomic_fetch_add_explicit(&s_engage_gen, 1u, memory_order_relaxed);
    if (s_active) {
        /* Re-latch from the live camera on every engage: the view moved on
         * since the last session, and the user expects the lock to grab
         * what's on screen now. */
        s_pose_valid = false;
        s_moved      = false;
        OPL_INFO_F("camlock",
                   "ON - view locked; WASD move, arrows look, Space/LShift "
                   "up/down, [ ] speed. Sim continues underneath (render-only).");
    } else {
        OPL_INFO_F("camlock", "OFF - camera returned to the game");
    }
}

bool openpete_camlock_pose_valid(void) { return s_pose_valid; }
bool openpete_camlock_moved(void)      { return s_moved; }

void openpete_camlock_set_pose(double x, double y, double z,
                               double yaw, double pitch) {
    s_x = x; s_y = y; s_z = z; s_yaw = yaw; s_pitch = pitch;
    s_pose_valid = true;
}

void openpete_camlock_get_pose(double* x, double* y, double* z,
                               double* yaw, double* pitch) {
    if (x) *x = s_x;
    if (y) *y = s_y;
    if (z) *z = s_z;
    if (yaw) *yaw = s_yaw;
    if (pitch) *pitch = s_pitch;
}

/* Mouse look markers, this consumer's own: the count total at the last
 * rebase and the angle already applied since. Present thread only. */
static long long s_mb_x, s_mb_y, s_ma_x, s_ma_y;
static unsigned  s_gen_seen = ~0u;
static float     s_sens_seen = -1.0f;

void openpete_camlock_integrate(double dt_ticks) {
    if (!s_active || !s_pose_valid) return;
    openpete_freecam_axes ax;
    openpete_freecam_get_axes(&ax);

    /* Mouse term: a displacement, not a rate, so no dt scale. Rebased on
     * engage, on a sensitivity change, and on a clamped pitch apply. */
    long long tx, ty;
    openpete_freecam_look_counts(&tx, &ty);
    const unsigned gen  = openpete_camlock_engage_generation();
    const float    sens = openpete_freecam_mouse_sensitivity();
    if (gen != s_gen_seen || sens != s_sens_seen) {
        s_mb_x = tx; s_mb_y = ty; s_ma_x = 0; s_ma_y = 0;
        s_gen_seen = gen; s_sens_seen = sens;
    }
    const long long target_x = llround(OPENPETE_FREECAM_MOUSE_K * (double)sens * (double)(tx - s_mb_x));
    const long long target_y = llround(OPENPETE_FREECAM_MOUSE_K * (double)sens * (double)(ty - s_mb_y));
    const long long mdx = target_x - s_ma_x, mdy = target_y - s_ma_y;
    s_ma_x = target_x; s_ma_y = target_y;

    if (ax.move_fwd == 0.0f && ax.move_strafe == 0.0f && ax.move_up == 0.0f &&
        ax.look_yaw == 0.0f && ax.look_pitch == 0.0f && mdx == 0 && mdy == 0)
        return;
    if (dt_ticks < 0.0) dt_ticks = 0.0;
    if (dt_ticks > 4.0) dt_ticks = 4.0;   /* hitch guard: no warp after a stall */

    const int    si    = openpete_freecam_speed_index();
    const double speed = s_move_speed[si] * dt_ticks;
    const double turn  = s_turn_speed[si] * dt_ticks;

    /* Same look/move basis as the freecam drive (freecam_game.c): turning
     * right lowers yaw, looking up lowers pitch; forward from (yaw, pitch)
     * in the world x/y plane with Z up. */
    s_yaw   -= (double)ax.look_yaw   * turn + (double)mdx;
    s_pitch -= (double)ax.look_pitch * turn + (double)mdy;
    if (s_pitch > CL_PITCH_LIMIT || s_pitch < -CL_PITCH_LIMIT) {
        s_pitch = s_pitch > 0 ? CL_PITCH_LIMIT : -CL_PITCH_LIMIT;
        /* Clamped: rebase the pitch markers so the first opposite-sign
         * count moves the pose instead of unwinding an overshoot. */
        s_mb_y = ty; s_ma_y = 0;
    }
    if (mdx || mdy)
        openpete_host_mouse_probe_log("camlock", "mouse apply dx=%lld dy=%lld -> yaw=%.0f pitch=%.0f",
                                      mdx, mdy, s_yaw, s_pitch);

    const double ry = s_yaw   * (2.0 * M_PI / CL_ANGLE_FULL);
    const double rp = s_pitch * (2.0 * M_PI / CL_ANGLE_FULL);
    const double cp = cos(rp);
    const double fwdx =  cos(ry) * cp;
    const double fwdy =  sin(ry) * cp;
    const double fwdz = -sin(rp);
    const double rgtx =  cos(ry - M_PI_2);
    const double rgty =  sin(ry - M_PI_2);

    s_x += (ax.move_fwd * fwdx + ax.move_strafe * rgtx) * speed;
    s_y += (ax.move_fwd * fwdy + ax.move_strafe * rgty) * speed;
    s_z += (ax.move_fwd * fwdz) * speed + (double)ax.move_up * speed;

    s_moved = true;
}
