// games/common/freecam_game.c - game-side body of the debug free camera (F),
// shared by every game target. See freecam_game.h.
//
// Technique (from C0mposer's Spyro-1-Practice-Rom, src/freecam.c):
//
//   1. Freeze the camera-follow and player-update functions so nothing
//      overwrites the camera each tick. The game's override_dispatch returning
//      1 for a function entry makes the generated body early-return
//      (core/psx/dispatch.c); freecam_should_freeze() drives that.
//
//   2. Hold the camera pose host-side and stamp it into the game's camera
//      bytes at the follow function's own tick point, in its place. A writer
//      that runs before the follow is overwritten by the stamp; only a writer
//      between the follow and the draw would need freezing.
//
//   3. The game's own per-tick matrix setup, which still runs, rebuilds the
//      view matrix from the bytes we wrote, so both the reference rasterizer
//      and the native renderer follow the free camera with no matrix math
//      duplicated here.
//
// The host pose is seeded from RAM at the first frozen follow entry after an
// engage (so the camera starts where the game left it) and after any follow
// entry that was not frozen. It is not savestated: the axes, the scripted
// sweeps and the path counter apply only on committed ticks, since a
// speculated (runahead) or replayed (rewind) tick would otherwise move the
// pose permanently where the RAM restore undoes everything else.
//
// Debug tool: OFF by default (zero writes, oracle-clean). ON is an
// intentional, opt-in gameplay-state divergence for inspecting a level.

#include "freecam_game.h"
#include "game_layout.h"        /* game_layout.cam: the pose bytes */
#include "game_extras.h"        /* game_is_gameplay_live */
#include "openpete_freecam.h"
#include "openpete_bugrec.h"
#include "openpete_config.h"     /* openpete_config_env */
#include "openpete_fiber.h"     /* openpete_speculative / openpete_replaying */
#include "openpete_host_mouse.h" /* probe log */
#include "openpete_log.h"
#include "openpete_rewind.h"   /* openpete_rewind_note_intervention */
#include "psx_runtime_internal.h"        /* CPUState + its read/write callbacks */
#include "renderer/terrain.h"   /* terrain_native_topdown_pose / _sweep_pose */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#  define M_PI    3.14159265358979323846
#endif
#ifndef M_PI_2
#  define M_PI_2  1.57079632679489661923
#endif

/* Angle units: a full turn is 0x1000 (4096). */
#define ANGLE_FULL   4096.0

#define D (&freecam_desc)

/* ---- Stored-angle codec ---------------------------------------------------
 * A 16-bit game keeps int16 angles; a 12-bit game keeps 0..0xFFF and masks
 * at every consumer. Both are held here as signed int32 wrapped to the
 * game's width so arithmetic and logging read the same either way. */
/** @brief Wrap an angle to the game's stored width as a signed value.
 *
 * @param v The angle in stored units, any range.
 * @return v wrapped to [-0x800, 0x7FF] for a 12-bit game, else to int16. */
static int32_t angle_wrap(int32_t v) {
    if (D->angle_bits == 12) return ((v + 0x800) & 0xFFF) - 0x800;
    return (int32_t)(int16_t)(uint16_t)(uint32_t)v;
}
/** @brief Read a stored angle from guest RAM as a wrapped signed value.
 *
 * @param cpu The guest CPU state.
 * @param addr The address of the 16-bit angle.
 * @return The angle after angle_wrap. */
static int32_t angle_read(CPUState* cpu, uint32_t addr) {
    return angle_wrap((int32_t)(uint32_t)cpu->read_half(addr));
}
/** @brief Encode an angle in the game's stored form.
 *
 * @param v The angle in stored units.
 * @return The low 12 bits for a 12-bit game, else the low 16 bits. */
static uint16_t angle_stored(int32_t v) {
    if (D->angle_bits == 12) return (uint16_t)(v & 0xFFF);
    return (uint16_t)(int16_t)v;
}

/* ---- Host-held pose -------------------------------------------------------
 * Stored units (the game's own), so a seed/stamp round trip is exact. */
static struct {
    int32_t  x, y, z, yaw, pitch;
    int32_t  roll;      /* about the view axis; set by a pose, 0 after a seed */
    int      seeded;
    unsigned gen_seen;
} s_pose;

/** @brief Load the host pose from the game's camera bytes.
 *
 * Reads position, yaw and pitch from game_layout.cam, sets roll 0 (the
 * freecam starts level whatever roll the game held) and records the current
 * engage generation.
 * @param cpu The guest CPU state. */
static void pose_seed(CPUState* cpu) {
    s_pose.x     = (int32_t)cpu->read_word(game_layout.cam.position + 0u);
    s_pose.y     = (int32_t)cpu->read_word(game_layout.cam.position + 4u);
    s_pose.z     = (int32_t)cpu->read_word(game_layout.cam.position + 8u);
    s_pose.yaw   = angle_read(cpu, game_layout.cam.rotation + 4u);
    s_pose.pitch = angle_read(cpu, game_layout.cam.rotation + 2u);
    s_pose.roll  = 0;
    s_pose.seeded   = 1;
    s_pose.gen_seen = openpete_freecam_engage_generation();
}

/** @brief Seed the host pose when it is unseeded or the freecam engaged again since the seed.
 *
 * @param cpu The guest CPU state. */
static void pose_seed_if_needed(CPUState* cpu) {
    if (!s_pose.seeded || s_pose.gen_seen != openpete_freecam_engage_generation())
        pose_seed(cpu);
}

/* The pose the last stamp wrote, for the matrix check: the game's builder
 * runs after the stamp, so the next drive reads its product. yaw, pitch and
 * roll are the pose's; stored[] is the (+0, +2, +4) triple written. */
static struct {
    int     valid;
    int32_t yaw, pitch, roll;
    int32_t stored[3];
} s_stamped;

/** @brief Read OPENPETE_FREECAM_MATRIX_CHECK: unset is off, "roll=N" also adds N to the stored roll.
 *
 * The roll form is the check's negative control: a stored roll the pose
 * does not hold, which the check must report as a matrix error and a roll
 * off the pose's.
 * @param roll Receives the roll to add, 0 unless the roll form is given.
 * @return Non-zero while the check is armed. */
static int matrix_check_armed(int32_t* roll) {
    static int s_armed = -1;
    static int32_t s_roll;
    if (s_armed < 0) {
        const char* e = getenv("OPENPETE_FREECAM_MATRIX_CHECK");
        s_armed = (e && *e) ? 1 : 0;
        s_roll  = (e && strncmp(e, "roll=", 5) == 0) ? atoi(e + 5) : 0;
    }
    *roll = s_roll;
    return s_armed;
}

/** @brief Fill the game-form rotation matrices Rx, Ry and Rz of an angle.
 *
 * The forms are freecam_builder_order's: Rx rows (1,0,0) (0,c,-s) (0,s,c),
 * Ry rows (c,0,s) (0,1,0) (-s,0,c), Rz rows (c,s,0) (-s,c,0) (0,0,1).
 * @param axis 0 for Rx, 1 for Ry, 2 for Rz.
 * @param units The angle, 4096 units per turn.
 * @param m Receives the matrix, unit scale. */
static void rot_game(int axis, double units, double m[3][3]) {
    const double r = units * (2.0 * M_PI / ANGLE_FULL), c = cos(r), sn = sin(r);
    memset(m, 0, 9 * sizeof(double));
    if (axis == 0) { m[0][0] = 1; m[1][1] = c; m[1][2] = -sn; m[2][1] = sn; m[2][2] = c; }
    else if (axis == 1) { m[0][0] = c; m[0][2] = sn; m[1][1] = 1; m[2][0] = -sn; m[2][2] = c; }
    else { m[0][0] = c; m[0][1] = sn; m[1][0] = -sn; m[1][1] = c; m[2][2] = 1; }
}

/** @brief Multiply two 3x3 matrices, out = a·b.
 *
 * @param a The left factor.
 * @param b The right factor.
 * @param out Receives the product; may not alias a or b. */
static void mat3_mul(const double a[3][3], const double b[3][3], double out[3][3]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            out[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}

/** @brief Log the view matrix the game built from the last stamp against the intended view of the pose.
 *
 * Runs only under a known builder order with the check armed. The intended
 * view over the GTE vector g = (camY - Y, camZ - Z, X - camX) is Rz(roll)·W,
 * with W the roll-free view: rows right = (sin yaw, -cos yaw, 0), down =
 * forward × right and forward = (cos pitch cos yaw, cos pitch sin yaw,
 * -sin pitch) in world axes, carried into g axes; scaled by 4096. Logs the
 * largest element difference in Q12 units and the measured roll, the angle
 * of the built right row about the view axis from W's right row toward W's
 * down row, in angle units and degrees.
 * @param cpu The guest CPU state. */
static void matrix_check(CPUState* cpu) {
    int32_t inject;
    if (!D->builder_order || !matrix_check_armed(&inject) || !s_stamped.valid) return;
    int M[3][3];
    for (uint32_t i = 0; i < 9u; i++)
        M[i / 3u][i % 3u] = (int16_t)cpu->read_half(game_layout.cam.view_matrix + 2u * i);
    const double ry = (double)((s_stamped.yaw - D->yaw_offset) * D->yaw_sign) * (2.0 * M_PI / ANGLE_FULL);
    const double rp = (double)(s_stamped.pitch * D->pitch_sign) * (2.0 * M_PI / ANGLE_FULL);
    const double fw[3] = { cos(rp) * cos(ry), cos(rp) * sin(ry), -sin(rp) };
    const double rt[3] = { sin(ry), -cos(ry), 0.0 };
    const double dn[3] = { fw[1] * rt[2] - fw[2] * rt[1], fw[2] * rt[0] - fw[0] * rt[2],
                           fw[0] * rt[1] - fw[1] * rt[0] };
    const double* Ww[3] = { rt, dn, fw };
    double W[3][3], Rz[3][3], T[3][3];
    for (int i = 0; i < 3; i++) {             /* world row in g axes */
        W[i][0] = -Ww[i][1]; W[i][1] = -Ww[i][2]; W[i][2] = Ww[i][0];
    }
    rot_game(2, (double)s_stamped.roll, Rz);
    mat3_mul(Rz, W, T);
    double err = 0.0;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) err = fmax(err, fabs(T[i][j] * 4096.0 - (double)M[i][j]));
    double along = 0.0, toward = 0.0;
    for (int j = 0; j < 3; j++) { along += M[0][j] * W[0][j]; toward += M[0][j] * W[1][j]; }
    const double roll_rad = atan2(toward, along);
    OPL_INFO_F("freecam", "matrix-check yaw=%d pitch=%d roll=%d stored=(%d, %d, %d) maxerr=%.2f "
               "roll_meas=%.2f roll_deg=%.3f M=[%d %d %d|%d %d %d|%d %d %d]",
               (int)s_stamped.yaw, (int)s_stamped.pitch, (int)s_stamped.roll,
               (int)s_stamped.stored[0], (int)s_stamped.stored[1], (int)s_stamped.stored[2],
               err, roll_rad * ANGLE_FULL / (2.0 * M_PI), roll_rad * 180.0 / M_PI,
               M[0][0], M[0][1], M[0][2], M[1][0], M[1][1], M[1][2], M[2][0], M[2][1], M[2][2]);
}

/** @brief Solve the Rx(b)·Ry(c)·Rz(a) triple whose product is Rz(roll)·Rx(pitch)·Ry(yaw).
 *
 * With M = Rx(b)·Ry(c)·Rz(a) in the game forms, M[0][2] = sin c,
 * (M[0][0], M[0][1]) = cos c (cos a, sin a) and (M[1][2], M[2][2]) =
 * cos c (-sin b, cos b). Of the two solutions, (a, b, c) and (a + 0x800,
 * b + 0x800, 0x800 - c), the one whose c is nearer the pose's yaw is taken.
 * At cos c = 0 only a + b or a - b is determined; a is set 0 and b read from
 * rows 1 and 2. Angles are rounded to whole units.
 * @param yaw The pose's yaw, stored units.
 * @param pitch The pose's pitch, stored units.
 * @param roll The pose's roll, stored units.
 * @param out Receives the (+0, +2, +4) triple: a, b, c. */
static void solve_rx_ry_rz(int32_t yaw, int32_t pitch, int32_t roll, int32_t out[3]) {
    double X[3][3], Y[3][3], Z[3][3], XY[3][3], T[3][3];
    rot_game(0, (double)pitch, X);
    rot_game(1, (double)yaw, Y);
    rot_game(2, (double)roll, Z);
    mat3_mul(X, Y, XY);
    mat3_mul(Z, XY, T);
    const double k = ANGLE_FULL / (2.0 * M_PI);
    const double sc = fmax(-1.0, fmin(1.0, T[0][2]));
    const double cc = sqrt(fmax(0.0, 1.0 - sc * sc));
    double a, b, c = atan2(sc, cc) * k;
    if (cc < 1e-9) {
        a = 0.0;
        b = atan2(T[2][1], T[1][1]) * k;
    } else {
        a = atan2(T[0][1], T[0][0]) * k;
        b = atan2(-T[1][2], T[2][2]) * k;
    }
    const double c2 = ANGLE_FULL / 2.0 - c;
    const double d1 = fabs((double)angle_wrap((int32_t)lround(c) - yaw));
    const double d2 = fabs((double)angle_wrap((int32_t)lround(c2) - yaw));
    if (d2 < d1) { a += ANGLE_FULL / 2.0; b += ANGLE_FULL / 2.0; c = c2; }
    out[0] = angle_wrap((int32_t)lround(a));
    out[1] = angle_wrap((int32_t)lround(b));
    out[2] = angle_wrap((int32_t)lround(c));
}

/** @brief Write the host pose into the game's camera bytes.
 *
 * Wraps yaw, pitch and roll to the game's angle width first, then stores
 * position as words and the rotation record as halfwords under
 * game_layout.cam. The record written depends on the builder order: under
 * an unknown order, yaw and pitch only; under Rz·Rx·Ry, (roll, pitch, yaw)
 * as they are; under Rx·Ry·Rz, (0, pitch, yaw) at roll 0, else the triple
 * solve_rx_ry_rz finds. Every known order thus composes the pose's view
 * whatever roll the game left at +0. The matrix check's injected roll is
 * added to the stored +0.
 * @param cpu The guest CPU state. */
static void pose_stamp(CPUState* cpu) {
    s_pose.yaw   = angle_wrap(s_pose.yaw);
    s_pose.pitch = angle_wrap(s_pose.pitch);
    s_pose.roll  = angle_wrap(s_pose.roll);
    int32_t st[3] = { s_pose.roll, s_pose.pitch, s_pose.yaw };
    if (D->builder_order == FREECAM_BUILDER_RX_RY_RZ && s_pose.roll != 0)
        solve_rx_ry_rz(s_pose.yaw, s_pose.pitch, s_pose.roll, st);
    cpu->write_word(game_layout.cam.position + 0u, (uint32_t)s_pose.x);
    cpu->write_word(game_layout.cam.position + 4u, (uint32_t)s_pose.y);
    cpu->write_word(game_layout.cam.position + 8u, (uint32_t)s_pose.z);
    cpu->write_half(game_layout.cam.rotation + 4u, angle_stored(st[2]));
    cpu->write_half(game_layout.cam.rotation + 2u, angle_stored(st[1]));
    if (D->builder_order) {
        int32_t inject = 0;
        (void)matrix_check_armed(&inject);
        st[0] = angle_wrap(st[0] + inject);
        cpu->write_half(game_layout.cam.rotation + 0u, angle_stored(st[0]));
    }
    s_stamped.valid = 1;
    s_stamped.yaw = s_pose.yaw; s_stamped.pitch = s_pose.pitch; s_stamped.roll = s_pose.roll;
    memcpy(s_stamped.stored, st, sizeof st);
}

/** @brief Report whether a level is running.
 *
 * Uses the game's own predicate when its descriptor has one, else
 * game_is_gameplay_live.
 * @param cpu The guest CPU state.
 * @return Non-zero while a level is live. */
static int level_live(CPUState* cpu) {
    return D->level_live ? D->level_live(cpu) : game_is_gameplay_live(cpu);
}

/** @brief Extract up to max signed decimal integers from text, skipping every non-numeric run.
 *
 * A sign counts only directly before a digit. The leniency lets a freecam log
 * line be pasted verbatim: "pos=(84992, 45582, 9948) yaw=1024 pitch=160"
 * yields all five.
 * @param s The text.
 * @param out Receives the integers.
 * @param max Capacity of @p out.
 * @return The count parsed. */
static int freecam_parse_ints(const char* s, int32_t* out, int max) {
    int n = 0;
    while (*s && n < max) {
        int is_num = (*s >= '0' && *s <= '9') ||
                     ((*s == '-' || *s == '+') && s[1] >= '0' && s[1] <= '9');
        if (!is_num) { s++; continue; }
        char* end;
        long v = strtol(s, &end, 10);
        out[n++] = (int32_t)v;
        s = end;
    }
    return n;
}

/** @brief Take the optional "roll=N" key out of pose text.
 *
 * The key matches where "roll=" starts the text or follows a character that
 * is not a letter, digit or underscore, and a signed decimal integer follows
 * the '='. The first match is read; the text with that key blanked to spaces
 * is copied into @p rest (truncated to @p cap - 1 characters), so the
 * positional integers and a pose name parse from @p rest as from text that
 * never held the key. Without a match @p rest is not written.
 * @param s The pose text.
 * @param rest Receives the text without the key when one is found.
 * @param cap Capacity of @p rest, at least 1.
 * @param roll Receives the key's value, 0 when absent.
 * @return 1 when the key was found and @p rest written, else 0. */
static int freecam_take_roll(const char* s, char* rest, size_t cap, int32_t* roll) {
    *roll = 0;
    for (const char* k = strstr(s, "roll="); k; k = strstr(k + 1, "roll=")) {
        if (k > s) {
            const char pc = k[-1];
            if ((pc >= 'a' && pc <= 'z') || (pc >= 'A' && pc <= 'Z') ||
                (pc >= '0' && pc <= '9') || pc == '_') continue;
        }
        const char* v = k + 5;
        const char* d = (*v == '-' || *v == '+') ? v + 1 : v;
        if (*d < '0' || *d > '9') continue;
        char* end;
        *roll = (int32_t)strtol(v, &end, 10);
        size_t n = strlen(s);
        if (n >= cap) n = cap - 1;
        memcpy(rest, s, n);
        rest[n] = '\0';
        for (size_t i = (size_t)(k - s); i < (size_t)(end - s) && i < n; i++) rest[i] = ' ';
        return 1;
    }
    return 0;
}

/** @brief Resolve a named pose through the game's resolver.
 *
 * A game without a resolver rejects every name with a WARN.
 * @param cpu The guest CPU state.
 * @param name The pose name ("topdown", "sweepN").
 * @param v Receives {x, y, z, yaw, pitch} in stored units.
 * @return Non-zero on success; 0 for an unknown name, no live level, or no
 *         resolver. */
static int resolve_named(CPUState* cpu, const char* name, int32_t v[5]) {
    if (!D->resolve_named_pose) {
        OPL_WARN_F("freecam", "named pose '%s': this game has no named poses", name);
        return 0;
    }
    return D->resolve_named_pose(cpu, name, v);
}

int freecam_terrain_named_pose(CPUState* cpu, const char* name, int32_t v[5]) {
    if (strcmp(name, "topdown") == 0) return terrain_native_topdown_pose(cpu, v);
    if (strncmp(name, "sweep", 5) == 0) return terrain_native_sweep_pose(cpu, atoi(name + 5), v);
    return 0;
}

/* ---- Scripted flight path (OPENPETE_FREECAM_PATH / --freecam-path) --------
 *
 * A static pose reveals a whole level in its first frame, so every first-use
 * resource it needs is created at once and nothing is created afterwards. The
 * costs that only *movement* reaches - terrain sector cache eviction and
 * refill, LOD tier crossings, moby extraction churn - are invisible to it.
 * This flies the camera instead: waypoints are linearly interpolated over
 * `ticks` canonical ticks per segment, and the path loops so a run of any
 * length stays in motion. With ticks=1 every waypoint is the pose of one
 * tick, which replays a keyed camera path (the Spyro 3 credits' one key per
 * tick) without interpolation.
 *
 * Spec: "[ticks=N;] WP(;WP)+" where WP is "x,y,z[,yaw,pitch[,roll]]" or a
 * name the game's resolver knows ("topdown", "sweepN"); either may also
 * carry the pose's "roll=N" key. A newline separates segments as ';' does.
 * A numeric value may carry a decimal fraction. The game's camera bytes take
 * the pose rounded to whole units on every tick; with any fraction present,
 * each present instead shows the Catmull-Rom curve through the waypoints as
 * written at its instant (freecam_present_pose), so the view moves below
 * the game's angle and position quanta. A "cut" segment ends the curve at
 * the waypoint before it. An all-integer path keeps the lerp between the
 * ticks' integer poses.
 * "@FILE" reads the spec from FILE, which carries a path of any length past
 * a command line's limit. Names resolve against the live
 * level once it is up, so a path can dive from above the level into a flight
 * across it without the caller knowing any level's bounds. */

#define FREECAM_PATH_DEFAULT_TICKS 120
#define FREECAM_NAME_MAX 32
#define FREECAM_PATH_FILE_MAX (64u << 20)
#define FREECAM_PATH_LOG_MAX 16

/** @brief Extract up to max signed decimal numbers from text as doubles, skipping every non-numeric run.
 *
 * freecam_parse_ints' grammar, and its values on integer text, with an
 * optional fraction: a number starts at a digit, or at a sign directly
 * before a digit, and a '.' and digits after its integer part are its
 * fraction. No exponent or radix prefix is read.
 * @param s The text.
 * @param out Receives the numbers.
 * @param max Capacity of @p out.
 * @return The count parsed. */
static int freecam_parse_reals(const char* s, double* out, int max) {
    int n = 0;
    while (*s && n < max) {
        int is_num = (*s >= '0' && *s <= '9') ||
                     ((*s == '-' || *s == '+') && s[1] >= '0' && s[1] <= '9');
        if (!is_num) { s++; continue; }
        const int neg = (*s == '-');
        char* end;
        double v = (double)strtol(s, &end, 10);
        if (end[0] == '.' && end[1] >= '0' && end[1] <= '9') {
            double frac = 0.0, scale = 0.1;
            for (end++; *end >= '0' && *end <= '9'; end++, scale *= 0.1)
                frac += (double)(*end - '0') * scale;
            v = neg ? v - frac : v + frac;
        }
        out[n++] = v;
        s = end;
    }
    return n;
}

/** @brief Parse pose text into its positional numbers, its roll and, without three numbers, a name.
 *
 * The grammar a pose and a path waypoint share. The optional "roll=N" key is
 * taken out first (freecam_take_roll); up to @p max numbers are then read
 * positionally: as integers (freecam_parse_ints) when @p real is NULL, else
 * as doubles into @p real (freecam_parse_reals) with @p v receiving each
 * rounded to the nearest integer. With fewer than three, @p name receives
 * the text with leading spaces skipped, truncated to FREECAM_NAME_MAX - 1
 * characters and trailing spaces trimmed (empty for blank text); otherwise
 * it is the empty string.
 * @param text The pose text.
 * @param v Receives the integers.
 * @param real Receives the numbers as read, or NULL to read integers only.
 * @param max Capacity of @p v and @p real.
 * @param roll Receives the key's roll, 0 when absent.
 * @param name Receives the name.
 * @return The count of numbers read. */
static int freecam_parse_pose_text(const char* text, int32_t* v, double* real, int max,
                                   int32_t* roll, char name[FREECAM_NAME_MAX]) {
    char rest[256];
    if (freecam_take_roll(text, rest, sizeof rest, roll)) text = rest;
    int n;
    if (real) {
        n = freecam_parse_reals(text, real, max);
        for (int i = 0; i < n; i++) v[i] = (int32_t)lround(real[i]);
    } else {
        n = freecam_parse_ints(text, v, max);
    }
    name[0] = '\0';
    if (n >= 3) return n;
    while (*text == ' ') text++;
    strncpy(name, text, FREECAM_NAME_MAX - 1);
    name[FREECAM_NAME_MAX - 1] = '\0';
    for (char* e = name + strlen(name); e > name && e[-1] == ' ';) *--e = '\0';
    return n;
}

/* One path waypoint: a numeric pose, or a name resolved from the live level. */
typedef struct {
    int32_t v[6];                      /* x, y, z, yaw, pitch, roll; the stamped (rounded) pose */
    double  f[6];                      /* the same as written, fractions kept */
    int     cut_after;                 /* a "cut" segment follows: no curve through it */
    char    named[FREECAM_NAME_MAX];   /* "" = numeric */
} freecam_waypoint;

/* One committed path stamp, kept for the presentation pose: the VBL it ran
 * on, the path parameter (waypoint index plus the fraction of the segment)
 * and the stamped position, which identifies the stamp in a camera
 * snapshot. */
typedef struct {
    uint32_t vbl;
    double   u;
    int32_t  pos[3];
    int32_t  ang[3];    /* yaw, pitch, roll as stamped */
} freecam_path_stamp;

#define FREECAM_STAMP_RING 64

static struct {
    freecam_waypoint* wp;   /* heap, cap entries, n used */
    int     n;
    int     cap;
    int     ticks;      /* ticks per segment */
    int     t;          /* tick index within the current segment */
    int     seg;
    int     resolved;   /* named waypoints filled in from the live level */
    int     active;
    int     frac;       /* some numeric waypoint carries a fraction: presents
                         * evaluate the path curve (freecam_present_pose) */
    freecam_path_stamp ring[FREECAM_STAMP_RING];
    unsigned ring_n;    /* stamps written; the newest is ring[(ring_n - 1) % RING] */
} s_path;

/** @brief Read a whole spec file into a NUL-terminated heap buffer.
 *
 * Refuses a file that cannot be opened or read, or that is larger than
 * FREECAM_PATH_FILE_MAX bytes, with a WARN.
 * @param file The file path.
 * @return The text, owned by the caller (free), or NULL on failure. */
static char* freecam_path_read_file(const char* file) {
    FILE* f = fopen(file, "rb");
    if (!f) {
        OPL_WARN_F("freecam", "--freecam-path: cannot open '%s'", file);
        return NULL;
    }
    char* buf = NULL;
    size_t len = 0;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long size = ftell(f);
        if (size >= 0 && (unsigned long)size <= FREECAM_PATH_FILE_MAX &&
            fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)size + 1)) != NULL) {
            len = fread(buf, 1, (size_t)size, f);
            if (len != (size_t)size) { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    if (!buf) {
        OPL_WARN_F("freecam", "--freecam-path: cannot read '%s' (at most %u bytes)",
                   file, FREECAM_PATH_FILE_MAX);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

/** @brief Append one waypoint to s_path, growing the array by doubling.
 *
 * @param v The six pose integers.
 * @param f The six pose numbers as written.
 * @param named The name, "" for a numeric waypoint.
 * @return 1 on success, 0 when the allocation failed. */
static int freecam_path_push(const int32_t v[6], const double f[6], const char* named) {
    if (s_path.n == s_path.cap) {
        const int cap = s_path.cap ? s_path.cap * 2 : 16;
        freecam_waypoint* wp = realloc(s_path.wp, (size_t)cap * sizeof *wp);
        if (!wp) return 0;
        s_path.wp = wp;
        s_path.cap = cap;
    }
    freecam_waypoint* w = &s_path.wp[s_path.n++];
    memcpy(w->v, v, sizeof w->v);
    memcpy(w->f, f, sizeof w->f);
    w->cut_after = 0;
    memcpy(w->named, named, sizeof w->named);
    return 1;
}

/** @brief Parse one ';'- or newline-delimited segment of a path spec into s_path.
 *
 * A segment containing "ticks=" sets the ticks per segment when positive. A
 * segment "cut" marks the previous waypoint's segment as a cut, which the
 * presentation curve does not pass through. Any other segment parses as pose
 * text (freecam_parse_pose_text) with fractions kept: three or more numbers
 * make a numeric waypoint, missing yaw and pitch reading 0 and the roll the
 * sixth number, else the "roll=N" key's value, else 0; a number with a
 * fraction marks the path fractional; a non-blank name makes a named
 * waypoint with the key's roll, resolved later.
 * The segment is truncated to 127 characters.
 * @param p The segment's first character.
 * @param len The segment's length.
 * @return 1 to continue; 0 to refuse the spec (a named waypoint on a game
 *         without named poses, or an allocation failure). */
static int freecam_path_parse_segment(const char* p, size_t len) {
    char seg[128];
    if (len >= sizeof seg) len = sizeof seg - 1;
    memcpy(seg, p, len);
    seg[len] = '\0';
    for (char* e = seg + len; e > seg && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t');)
        *--e = '\0';

    const char* tk = strstr(seg, "ticks=");
    if (tk) {
        const int v = atoi(tk + 6);
        if (v > 0) s_path.ticks = v;
        return 1;
    }
    if (strcmp(seg, "cut") == 0) {
        if (s_path.n > 0) s_path.wp[s_path.n - 1].cut_after = 1;
        return 1;
    }
    int32_t v[6] = {0, 0, 0, 0, 0, 0}, roll;
    double f[6] = {0, 0, 0, 0, 0, 0};
    char named[FREECAM_NAME_MAX];
    const int nv = freecam_parse_pose_text(seg, v, f, 6, &roll, named);
    if (nv < 6) { v[5] = roll; f[5] = (double)roll; }
    for (int i = 0; i < 6; i++)
        if (f[i] != (double)v[i]) s_path.frac = 1;
    if (named[0] && !D->resolve_named_pose) {
        OPL_WARN_F("freecam", "--freecam-path: named waypoint '%s' but "
                   "this game has no named poses", named);
        return 0;
    }
    if (nv < 3 && !named[0]) return 1;
    if (!freecam_path_push(v, f, named)) {
        OPL_WARN_F("freecam", "--freecam-path: out of memory at waypoint %d", s_path.n);
        return 0;
    }
    return 1;
}

/** @brief Parse a flight-path spec into s_path and arm it.
 *
 * A spec starting with '@' names a file whose whole text is the spec. The
 * spec splits into segments on ';' and newline, each parsed by
 * freecam_path_parse_segment; the waypoint count is bounded only by memory.
 * A refused segment, an unreadable file or fewer than two waypoints leave
 * the path inactive.
 * @param spec The spec text, or "@" and a file path.
 * @return 1 when two or more waypoints were parsed and the path is active,
 *         else 0. */
static int freecam_path_parse(const char* spec) {
    free(s_path.wp);
    memset(&s_path, 0, sizeof s_path);
    s_path.ticks = FREECAM_PATH_DEFAULT_TICKS;

    char* text = NULL;
    if (spec[0] == '@') {
        text = freecam_path_read_file(spec + 1);
        if (!text) return 0;
        spec = text;
    }
    int ok = 1;
    for (const char* p = spec; ok && *p;) {
        while (*p == ' ' || *p == ';' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (!*p) break;
        const size_t len = strcspn(p, ";\n");
        ok = freecam_path_parse_segment(p, len);
        p += len;
    }
    free(text);
    if (!ok) return 0;

    if (s_path.n < 2) {
        OPL_WARN_F("freecam",
                   "--freecam-path needs at least 2 waypoints (got %d)", s_path.n);
        return 0;
    }
    s_path.active = 1;
    OPL_INFO_F("freecam", "flight path armed: %d waypoint(s), %d tick(s) per segment%s",
               s_path.n, s_path.ticks, s_path.frac ? ", fractional (presents follow the curve)" : "");
    return 1;
}

/** @brief Fill in the path's named waypoints from the live level, once.
 *
 * After the first success logs the first FREECAM_PATH_LOG_MAX waypoints and
 * the count of the rest.
 * @param cpu The guest CPU state.
 * @return 1 once resolved; 0 while a name cannot resolve yet. */
static int freecam_path_resolve(CPUState* cpu) {
    if (s_path.resolved) return 1;
    for (int i = 0; i < s_path.n; i++) {
        int32_t v[5] = {0, 0, 0, 0, 0};
        if (!s_path.wp[i].named[0]) continue;
        if (!resolve_named(cpu, s_path.wp[i].named, v)) return 0;  /* not live yet */
        memcpy(s_path.wp[i].v, v, sizeof v);   /* roll, v[5], is the spec's */
        for (int k = 0; k < 5; k++) s_path.wp[i].f[k] = (double)v[k];
    }
    for (int i = 0; i < s_path.n && i < FREECAM_PATH_LOG_MAX; i++) {
        const int32_t* w = s_path.wp[i].v;
        if (w[5])
            OPL_INFO_F("freecam", "path wp%d = (%d, %d, %d) yaw=%d pitch=%d roll=%d", i,
                       w[0], w[1], w[2], w[3], w[4], w[5]);
        else
            OPL_INFO_F("freecam", "path wp%d = (%d, %d, %d) yaw=%d pitch=%d", i,
                       w[0], w[1], w[2], w[3], w[4]);
    }
    if (s_path.n > FREECAM_PATH_LOG_MAX)
        OPL_INFO_F("freecam", "path: %d more waypoint(s) not logged",
                   s_path.n - FREECAM_PATH_LOG_MAX);
    s_path.resolved = 1;
    return 1;
}

/** @brief Interpolate between two stored angles along the shorter arc.
 *
 * For a 16-bit game, 32000 to -32000 is 1536 units forward, not 64000 back.
 * The fraction num / den truncates toward zero.
 * @param a The start angle.
 * @param b The end angle.
 * @param num Numerator of the fraction.
 * @param den Denominator of the fraction; non-zero.
 * @return The wrapped angle. */
static int32_t freecam_lerp_angle(int32_t a, int32_t b, int num, int den) {
    int32_t d = angle_wrap(b - a);
    return angle_wrap(a + (d * num) / den);
}

/** @brief Write the path's pose for the current tick into the host pose, then advance the counter.
 *
 * Interpolates linearly between the current segment's two waypoints, angles
 * along the shorter arc; the last segment loops to the first waypoint. Does
 * nothing while a named waypoint cannot resolve.
 * @param cpu The guest CPU state.
 * @param committed Non-zero on a committed tick, the only kind that advances
 *        the counter. */
static void freecam_path_step(CPUState* cpu, int committed) {
    if (!freecam_path_resolve(cpu)) return;

    const int32_t* a = s_path.wp[s_path.seg].v;
    const int32_t* b = s_path.wp[(s_path.seg + 1) % s_path.n].v;
    const int num = s_path.t, den = s_path.ticks;

    s_pose.x     = a[0] + (int32_t)(((int64_t)(b[0] - a[0]) * num) / den);
    s_pose.y     = a[1] + (int32_t)(((int64_t)(b[1] - a[1]) * num) / den);
    s_pose.z     = a[2] + (int32_t)(((int64_t)(b[2] - a[2]) * num) / den);
    s_pose.yaw   = freecam_lerp_angle(a[3], b[3], num, den);
    s_pose.pitch = freecam_lerp_angle(a[4], b[4], num, den);
    s_pose.roll  = freecam_lerp_angle(a[5], b[5], num, den);

    if (committed && s_path.frac) {
        freecam_path_stamp* st = &s_path.ring[s_path.ring_n++ % FREECAM_STAMP_RING];
        st->vbl = openpete_frame;
        st->u = (double)s_path.seg + (double)num / (double)den;
        st->pos[0] = s_pose.x; st->pos[1] = s_pose.y; st->pos[2] = s_pose.z;
        st->ang[0] = angle_wrap(s_pose.yaw);
        st->ang[1] = angle_wrap(s_pose.pitch);
        st->ang[2] = angle_wrap(s_pose.roll);
    }
    /* OPENPETE_FCPATH_LOG: the committed path key and pose of each tick. */
    if (committed && getenv("OPENPETE_FCPATH_LOG"))
        fprintf(stderr, "[fcpath] vbl=%lu seg=%d t=%d pose=(%d,%d,%d) yaw=%d pitch=%d roll=%d\n",
                (unsigned long)openpete_feed_key_now(), s_path.seg, s_path.t, s_pose.x, s_pose.y, s_pose.z,
                s_pose.yaw, s_pose.pitch, s_pose.roll);
    if (committed && ++s_path.t >= s_path.ticks) {
        s_path.t = 0;
        s_path.seg = (s_path.seg + 1) % s_path.n;
    }
}

/** @brief Find the committed path stamp a camera snapshot holds.
 *
 * The snapshot is captured after the tick's stamp, on the stamp's VBL or a
 * later one. Searches the stamps at or before @p tick, newest first, for the
 * first whose stamped position equals the snapshot's, looking at most four
 * deep.
 * @param tick The snapshot's VBL (openpete_interp_snapshot_t.tick).
 * @param pos The snapshot's camera position.
 * @return The stamp, or NULL when none matches. */
static const freecam_path_stamp* freecam_stamp_find(uint32_t tick, const int32_t pos[3]) {
    const unsigned n = s_path.ring_n < FREECAM_STAMP_RING ? s_path.ring_n : FREECAM_STAMP_RING;
    int looked = 0;
    for (unsigned k = 0; k < n && looked < 4; k++) {
        const freecam_path_stamp* st = &s_path.ring[(s_path.ring_n - 1u - k) % FREECAM_STAMP_RING];
        if (st->vbl > tick) continue;
        looked++;
        if (st->pos[0] == pos[0] && st->pos[1] == pos[1] && st->pos[2] == pos[2]) return st;
    }
    return NULL;
}

/** @brief Evaluate the path curve at path parameter u.
 *
 * u is a waypoint index plus the fraction of its segment, taken modulo the
 * waypoint count as the path loops. Within segment i -> i+1 the curve is
 * the uniform Catmull-Rom spline through waypoints i-1, i, i+1 and i+2 (C1,
 * passing through every waypoint), angles unwrapped about waypoint i along
 * the shorter arc. A neighbour across a cut is replaced by the linear
 * extension of the segment, and a segment that is itself a cut is linear.
 * @param u The path parameter.
 * @param out Receives x, y, z, yaw, pitch, roll; angles in [-0x800, 0x800]. */
static void freecam_path_curve(double u, double out[6]) {
    const int n = s_path.n;
    const double fl = floor(u);
    const double t = u - fl;
    const int i = (int)(((long long)fl % n + n) % n);
    const int j = (i + 1) % n;
    const freecam_waypoint* w = s_path.wp;
    const int lin = w[i].cut_after;
    const int has0 = !w[(i + n - 1) % n].cut_after;
    const int has3 = !w[j].cut_after;
    const double* f0 = w[(i + n - 1) % n].f;
    const double* f1 = w[i].f;
    const double* f2 = w[j].f;
    const double* f3 = w[(j + 1) % n].f;
    for (int c = 0; c < 6; c++) {
        double a0, a1 = f1[c], a2, a3;
        if (c < 3) {
            a0 = f0[c]; a2 = f2[c]; a3 = f3[c];
        } else {
            a2 = a1 + remainder(f2[c] - f1[c], ANGLE_FULL);
            a0 = a1 - remainder(f1[c] - f0[c], ANGLE_FULL);
            a3 = a2 + remainder(f3[c] - f2[c], ANGLE_FULL);
        }
        double v;
        if (lin) {
            v = a1 + (a2 - a1) * t;
        } else {
            if (!has0) a0 = 2.0 * a1 - a2;
            if (!has3) a3 = 2.0 * a2 - a1;
            v = 0.5 * (2.0 * a1 + (a2 - a0) * t + (2.0 * a0 - 5.0 * a1 + 4.0 * a2 - a3) * t * t +
                       (3.0 * a1 - a0 - 3.0 * a2 + a3) * t * t * t);
        }
        out[c] = (c < 3) ? v : remainder(v, ANGLE_FULL);
    }
}

int freecam_present_pose(uint32_t prev_tick, const int32_t prev_pos[3],
                         uint32_t curr_tick, const int32_t curr_pos[3],
                         double alpha, double pose[6], int32_t prev_ang[3]) {
    if (!s_path.active || !s_path.frac || !s_path.resolved || !openpete_freecam_active()) return 0;
    const freecam_path_stamp* sp = freecam_stamp_find(prev_tick, prev_pos);
    const freecam_path_stamp* sc = freecam_stamp_find(curr_tick, curr_pos);
    if (!sp || !sc) return 0;
    double du = sc->u - sp->u;
    if (du < 0.0) du += (double)s_path.n;          /* the loop back to waypoint 0 */
    if (du <= 0.0 || du > 2.0) return 0;
    freecam_path_curve(sp->u + alpha * du, pose);
    memcpy(prev_ang, sp->ang, sizeof sp->ang);
    return 1;
}

int freecam_pose_numbers(const char* text, int32_t v[5]) {
    int32_t roll;
    char name[FREECAM_NAME_MAX];
    for (int i = 0; i < 5; i++) v[i] = 0;
    return text ? freecam_parse_pose_text(text, v, NULL, 5, &roll, name) : 0;
}

void freecam_view_rotation(double yaw, double pitch, double roll, double R[3][3]) {
    double X[3][3], Y[3][3], Z[3][3], XY[3][3];
    rot_game(0, pitch, X);
    rot_game(1, yaw, Y);
    rot_game(2, roll, Z);
    mat3_mul(X, Y, XY);
    mat3_mul(Z, XY, R);
}

/** @brief Set the host pose from an OPENPETE_FREECAM_POSE string.
 *
 * The text parses through freecam_parse_pose_text; the "roll=N" key sets
 * the roll, 0 when absent. Three or more integers set x, y, z and, when
 * present, yaw and pitch, keeping the current angles otherwise; a sixth
 * integer sets the roll, as it does in a path waypoint. Other text
 * resolves as a named pose, which sets all five.
 * An unresolvable string is logged and changes nothing.
 * @param cpu The guest CPU state.
 * @param s The pose text. */
static void freecam_apply_pose(CPUState* cpu, const char* s) {
    int32_t v[6] = {0, 0, 0, 0, 0, 0}, roll;
    char name[FREECAM_NAME_MAX];
    int n = freecam_parse_pose_text(s, v, NULL, 6, &roll, name);
    if (n >= 6) roll = v[5];
    if (n < 3) {
        if (!name[0] || !resolve_named(cpu, name, v)) {
            OPL_WARN_F("freecam",
                       "OPENPETE_FREECAM_POSE needs at least x,y,z or a named pose (got %d numbers)", n);
            return;
        }
        n = 5;
        s_pose.x = v[0]; s_pose.y = v[1]; s_pose.z = v[2];
        s_pose.yaw = angle_wrap(v[3]); s_pose.pitch = angle_wrap(v[4]);
        s_pose.roll = angle_wrap(roll);
        OPL_INFO_F("freecam", "%s pose set to (%d, %d, %d) yaw=%d pitch=%d",
                   name, v[0], v[1], v[2], v[3], v[4]);
        if (roll) OPL_INFO_F("freecam", "pose roll=%d", (int)roll);
        return;
    }
    s_pose.x = v[0]; s_pose.y = v[1]; s_pose.z = v[2];
    if (n >= 4) s_pose.yaw   = angle_wrap(v[3]);
    if (n >= 5) s_pose.pitch = angle_wrap(v[4]);
    s_pose.roll = angle_wrap(roll);
    OPL_INFO_F("freecam", "pose set to (%d, %d, %d) yaw=%d pitch=%d",
               v[0], v[1], v[2], v[3], v[4]);
    if (roll) OPL_INFO_F("freecam", "pose roll=%d", (int)roll);
}

/* A scripted pose (OPENPETE_FREECAM_POSE / --freecam-pose) waiting to be
 * stamped. The engage hook runs before the guest fiber, so a pose written
 * there lands one frame point too early: the camera position jumps while the
 * matrices the guest builds this frame are still the follow camera's, and the
 * frame is snapshot in that half-applied state. Held here instead and stamped
 * from freecam_drive, so position and matrices settle within one frame. */
static const char* s_pending_pose = NULL;

/** @brief Move the host pose for one tick and stamp it into the camera bytes.
 *
 * Runs in place of the game's camera follow, at its tick point, so the same
 * tick's matrix setup and renderer consume the stamped camera. Seeds the pose
 * from RAM after an engage, applies a queued scripted pose, and, when a path
 * is active, takes the path's pose instead of input. Otherwise applies the
 * look axes, the mouse look, OPENPETE_FREECAM_SPIN and the pitch clamp, then
 * the move axes and OPENPETE_FREECAM_DRIFT. Input, mouse, spin and drift
 * apply on committed ticks only; the mouse look applies the difference
 * between the scaled count total since its last rebase and the angle already
 * applied, rebasing on engage, on a sensitivity change, and for pitch on a
 * clamp. A changed pose is logged. Calls the game's draw_all hook when set.
 * @param cpu The guest CPU state. */
static void freecam_drive(CPUState* cpu) {
    const int committed = !openpete_speculative && !openpete_replaying;

    pose_seed_if_needed(cpu);
    matrix_check(cpu);

    /* Stamp a queued scripted pose here, at the follow logic's own tick
     * point, before the axes are read: the same tick's matrix setup then
     * consumes it, so no frame renders the new position under the old
     * orientation. */
    if (s_pending_pose) {
        const char* pose = s_pending_pose;
        s_pending_pose = NULL;
        freecam_apply_pose(cpu, pose);
        openpete_rewind_note_intervention("free camera pose");
    }
    /* A scripted path owns the camera outright - the interactive axes would
     * fight it, and a headless run has none to read anyway. */
    if (s_path.active) {
        freecam_path_step(cpu, committed);
        pose_stamp(cpu);
        if (D->draw_all) D->draw_all(cpu);
        return;
    }

    openpete_freecam_axes ax;
    openpete_freecam_get_axes(&ax);

    const int si    = openpete_freecam_speed_index();  /* pre-clamped */
    const int speed = D->move_speed[si];
    const int turn  = D->turn_speed[si];

    /* --- Look: modify the logical yaw + pitch. Turning right lowers yaw,
     * looking up lowers pitch ("positive pitch = look down"); the descriptor
     * maps the logical angles onto the game's stored ones. --- */
    int32_t yaw   = (s_pose.yaw - D->yaw_offset) * D->yaw_sign;
    int32_t pitch = s_pose.pitch * D->pitch_sign;
    if (committed) {
        yaw   -= (int32_t)lround((double)ax.look_yaw   * turn);
        pitch -= (int32_t)lround((double)ax.look_pitch * turn);
    }

    /* Mouse look. The host loop accumulates the look axes' mouse counts into
     * running totals; this consumer keeps the total at its last rebase and
     * the angle already applied, and on each COMMITTED tick applies
     * round(k * sensitivity * (total - base)) - applied. Speculated
     * (runahead) and replayed (rewind) ticks apply nothing: they are
     * re-executed or erased, and the committed tick that follows sees the
     * whole motion. The pose after N ticks is exact between rebases; a rebase
     * happens on engage (generation counter), on a sensitivity change, and,
     * for pitch, on a clamped apply so reversing the mouse moves at once. */
    static long long mb_x, mb_y, ma_x, ma_y;
    static unsigned  gen_seen = ~0u;
    static float     sens_seen = -1.0f;
    long long mdx = 0, mdy = 0, tx = 0, ty = 0;
    if (committed) {
        openpete_freecam_look_counts(&tx, &ty);
        const unsigned gen  = openpete_freecam_engage_generation();
        const float    sens = openpete_freecam_mouse_sensitivity();
        if (gen != gen_seen || sens != sens_seen) {
            mb_x = tx; mb_y = ty; ma_x = 0; ma_y = 0;
            gen_seen = gen; sens_seen = sens;
        }
        const long long target_x = llround(OPENPETE_FREECAM_MOUSE_K * (double)sens * (double)(tx - mb_x));
        const long long target_y = llround(OPENPETE_FREECAM_MOUSE_K * (double)sens * (double)(ty - mb_y));
        mdx = target_x - ma_x; mdy = target_y - ma_y;
        ma_x = target_x; ma_y = target_y;
        /* Mouse right turns right (lowers yaw); the default `mouse:-y`
         * spelling makes mouse up a positive count, which looks up (lowers
         * pitch). */
        yaw   -= (int32_t)mdx;
        pitch -= (int32_t)mdy;
    }
    /* OPENPETE_FREECAM_SPIN=<yaw-units-per-tick>: scripted continuous yaw
     * sweep (4096 = full turn), on top of input - lets one run profile every
     * view direction. */
    {
        static int s_have_spin = -1; static int s_spin;
        if (s_have_spin < 0) {
            const char* e = getenv("OPENPETE_FREECAM_SPIN");
            s_spin = e ? atoi(e) : 0;
            s_have_spin = 1;
        }
        if (committed) yaw += s_spin;
    }
    if (pitch > D->pitch_limit || pitch < -D->pitch_limit) {
        pitch = pitch > 0 ? D->pitch_limit : -D->pitch_limit;
        if (committed) { mb_y = ty; ma_y = 0; }   /* rebase pitch on clamp */
    }
    if (mdx || mdy)
        openpete_host_mouse_probe_log("freecam", "mouse apply dx=%lld dy=%lld -> yaw=%d pitch=%d",
                                      mdx, mdy, (int)angle_wrap(yaw), (int)angle_wrap(pitch));
    s_pose.yaw   = angle_wrap(yaw * D->yaw_sign + D->yaw_offset);
    s_pose.pitch = angle_wrap(pitch * D->pitch_sign);

    /* --- Move: modify the int32 position. Forward/right basis is the
     * practice ROM's: forward from (yaw, pitch), right = yaw − 90° on the
     * horizontal plane, and Space/Shift + forward-pitch move along world Z.
     * Freecam is already non-canonical, so host float trig is fine here (no
     * need to route through the game's fixed-point sin/cos). --- */
    int32_t px = s_pose.x;
    int32_t py = s_pose.y;
    int32_t pz = s_pose.z;

    const double ry = (double)yaw   * (2.0 * M_PI / ANGLE_FULL);
    const double rp = (double)pitch * (2.0 * M_PI / ANGLE_FULL);
    const double cp = cos(rp);
    const double fwdx =  cos(ry) * cp;
    const double fwdy =  sin(ry) * cp;
    const double fwdz = -sin(rp);
    const double rgtx =  cos(ry - M_PI_2);
    const double rgty =  sin(ry - M_PI_2);

    if (committed) {
        const double mvx = (ax.move_fwd * fwdx + ax.move_strafe * rgtx) * speed;
        const double mvy = (ax.move_fwd * fwdy + ax.move_strafe * rgty) * speed;
        const double mvz = (ax.move_fwd * fwdz) * speed + (double)ax.move_up * speed;

        px += (int32_t)lround(mvx);
        py += (int32_t)lround(mvy);
        pz += (int32_t)lround(mvz);
    }

    /* OPENPETE_FREECAM_DRIFT="dx,dy,dz": scripted per-tick camera drift
     * (world units), applied on top of input. Deterministic boundary-crossing
     * repro for cull/handoff work - headless runs can sweep the camera across
     * an admission edge from a script. */
    {
        static int32_t s_drift[3]; static int s_have_drift = -1;
        if (s_have_drift < 0) {
            const char* e = getenv("OPENPETE_FREECAM_DRIFT");
            s_have_drift = 0;
            if (e && *e) {
                char* end;
                s_drift[0] = (int32_t)strtol(e, &end, 10);
                if (*end) s_drift[1] = (int32_t)strtol(end + 1, &end, 10);
                if (*end) s_drift[2] = (int32_t)strtol(end + 1, &end, 10);
                s_have_drift = 1;
            }
        }
        if (s_have_drift && committed) { px += s_drift[0]; py += s_drift[1]; pz += s_drift[2]; }
    }

    s_pose.x = px; s_pose.y = py; s_pose.z = pz;
    pose_stamp(cpu);

    /* Log the live camera while engaged, but only when it changed since the
     * last log (no console spam while parked). */
    {
        int32_t cyaw = s_pose.yaw, cpitch = s_pose.pitch;
        static int      s_have = 0;
        static int32_t  l_x, l_y, l_z, l_yaw, l_pitch;
        if (!s_have || px != l_x || py != l_y || pz != l_z ||
            cyaw != l_yaw || cpitch != l_pitch) {
            s_have = 1;
            l_x = px; l_y = py; l_z = pz; l_yaw = cyaw; l_pitch = cpitch;
            OPL_INFO_F("freecam", "pos=(%d, %d, %d) yaw=%d pitch=%d",
                       px, py, pz, (int)cyaw, (int)cpitch);
        }
    }

    /* Draw all sectors in view, not just the camera's region (no pop-in). */
    if (D->draw_all) D->draw_all(cpu);
}

/** @brief Place the player at a world position, optionally setting the facing and re-seating the follow camera.
 *
 * The previous position is written to match: the physics derives the tick's
 * movement from it, and an old previous position would present the jump as
 * one tick of travel that collides with everything between the two points.
 * The facing is written as the pair the game keeps, the 12-bit physics angle
 * and the 8-bit copy (angle >> 4) the player update re-derives from it each
 * tick; the byte alone lasts one tick, the angle alone leaves one tick drawn
 * at the old facing. Momentum is cancelled through the game's reset_momentum
 * routine. State, animation and health are untouched, and the next player
 * update re-derives the floor and wall queries from the new position. A
 * missing descriptor hook or address skips its step with a log line.
 * @param cpu The guest CPU state.
 * @param x World x.
 * @param y World y.
 * @param z World z.
 * @param yaw Facing, 4096 per turn, masked to 12 bits.
 * @param have_yaw Non-zero writes the facing.
 * @param seat_camera Non-zero calls the game's place_camera routine. */
static void freecam_teleport_player(CPUState* cpu, int32_t x, int32_t y, int32_t z,
                                    int32_t yaw, int have_yaw, int seat_camera) {
    cpu->write_word(D->player_pos + 0, (uint32_t)x);
    cpu->write_word(D->player_pos + 4, (uint32_t)y);
    cpu->write_word(D->player_pos + 8, (uint32_t)z);

    if (D->player_prev_pos) {
        cpu->write_word(D->player_prev_pos + 0, (uint32_t)x);
        cpu->write_word(D->player_prev_pos + 4, (uint32_t)y);
        cpu->write_word(D->player_prev_pos + 8, (uint32_t)z);
    }

    if (have_yaw) {
        if (D->player_facing) {
            const uint32_t a = (uint32_t)yaw & 0xFFFu;
            cpu->write_word(D->player_facing, a);
            if (D->player_facing_byte) cpu->write_byte(D->player_facing_byte, (uint8_t)(a >> 4));
        } else {
            OPL_WARN_F("freecam", "facing ignored: this game has no facing field located");
        }
    }

    if (D->reset_momentum) D->reset_momentum(cpu);
    else OPL_INFO_F("freecam", "teleport keeps momentum: this game has no reset routine");

    if (seat_camera) {
        if (D->place_camera) D->place_camera(cpu);
        else OPL_INFO_F("freecam", "camera left to converge: this game has no placement camera routine");
    }
    openpete_rewind_note_intervention("teleport");
}

/** @brief Move the player to the free camera's position.
 *
 * Camera and player share one world space, so the position copies across.
 * The facing is kept and the follow camera is not re-seated, so the follow
 * converges from the view that was being inspected.
 * @param cpu The guest CPU state. */
static void freecam_teleport_player_to_camera(CPUState* cpu) {
    pose_seed_if_needed(cpu);
    freecam_teleport_player(cpu, s_pose.x, s_pose.y, s_pose.z, 0, 0, 0);
    OPL_INFO_F("freecam", "teleported player to (%d, %d, %d)", s_pose.x, s_pose.y, s_pose.z);
}

/** @brief Report whether the player object can be moved.
 *
 * Uses the game's own predicate when its descriptor has one, else level_live.
 * @param cpu The guest CPU state.
 * @return Non-zero when a placement can apply. */
static int player_placeable(CPUState* cpu) {
    return D->player_placeable ? D->player_placeable(cpu) : level_live(cpu);
}

/* The placements OPENPETE_PLAYER_POSE lists, ';' between them, applied in
 * order: each waits for its "@N" feed-key value (or the first placeable
 * tick, without one) and for the one before it. s_pose_n is -1 until the
 * variable is read. The freecam's scripted engage waits until the last has
 * applied: the freecam engages at level_live, before the fly-in ends, and
 * freezes the player update and the follow, so a placement gated on the
 * ordinary follow would never fire under it. */
#define PLAYER_POSE_MAX 16
static char s_pose_text[PLAYER_POSE_MAX][128];
static long s_pose_at_key[PLAYER_POSE_MAX];
static int  s_pose_n = -1;
static int  s_pose_next;

/** @brief Read one listed placement into slot k: its text before '@' and its "@N" feed-key value.
 *
 * @param e The entry's text, not NUL-terminated.
 * @param len Its length.
 * @param k The slot.
 * @return 0, or -1 when the text after '@' is not a feed-key value or the
 *         entry does not fit. */
static int player_pose_read_entry(const char* e, size_t len, int k) {
    char buf[128];
    if (len >= sizeof buf) return -1;
    memcpy(buf, e, len);
    buf[len] = '\0';
    s_pose_at_key[k] = -1;
    char* at = strchr(buf, '@');
    if (at) {
        char* end;
        const long n = strtol(at + 1, &end, 10);
        while (*end == ' ') end++;
        if (end == at + 1 || *end || n < 0) {
            OPL_WARN_F("freecam", "OPENPETE_PLAYER_POSE: \"%s\" after '@' is not a feed-key value; not applied", at + 1);
            return -1;
        }
        s_pose_at_key[k] = n;
        *at = '\0';
    }
    snprintf(s_pose_text[k], sizeof s_pose_text[k], "%s", buf);
    return 0;
}

/** @brief Report whether a placement OPENPETE_PLAYER_POSE lists is still to apply.
 *
 * Reads the variable on the first call and splits it at ';' into at most
 * PLAYER_POSE_MAX placements; a malformed entry or one too many applies
 * none of them.
 * @return 1 while a placement is waiting, else 0. */
static int player_pose_pending(void) {
    if (s_pose_n < 0) {
        s_pose_n = 0;
        const char* pose = getenv("OPENPETE_PLAYER_POSE");
        for (const char* e = pose; e && *e; ) {
            const char* semi = strchr(e, ';');
            const size_t len = semi ? (size_t)(semi - e) : strlen(e);
            if (s_pose_n == PLAYER_POSE_MAX || player_pose_read_entry(e, len, s_pose_n) != 0) {
                OPL_WARN_F("freecam", "OPENPETE_PLAYER_POSE: entry %d is malformed or past the %d allowed; "
                           "no placement applied", s_pose_n + 1, PLAYER_POSE_MAX);
                s_pose_n = 0;
                break;
            }
            s_pose_n++;
            e = semi ? semi + 1 : e + len;
        }
    }
    return s_pose_next < s_pose_n;
}

void freecam_player_pose_maybe_apply(CPUState* cpu) {
    if (!player_pose_pending()) return;
    const int k = s_pose_next;
    if (s_pose_at_key[k] >= 0 &&
        openpete_feed_key_now() < (unsigned long)s_pose_at_key[k]) return;
    if (!player_placeable(cpu)) return;
    s_pose_next++;
    int32_t v[5] = {0, 0, 0, 0, 0};
    const int n = freecam_parse_ints(s_pose_text[k], v, 5);
    if (n < 3 || n > 4) {
        OPL_WARN_F("freecam", "OPENPETE_PLAYER_POSE needs x,y,z or x,y,z,yaw (got %d numbers); not applied", n);
        return;
    }
    freecam_teleport_player(cpu, v[0], v[1], v[2], v[3], n == 4, 1);
    if (n == 4) OPL_INFO_F("freecam", "player placed at (%d, %d, %d) yaw=%d key=%u", v[0], v[1], v[2], v[3] & 0xFFF,
                           openpete_feed_key_now());
    else        OPL_INFO_F("freecam", "player placed at (%d, %d, %d) key=%u", v[0], v[1], v[2],
                           openpete_feed_key_now());
}

int game_get_player_pose(CPUState* cpu, int32_t* out_x, int32_t* out_y, int32_t* out_z,
                         int32_t* out_yaw, int32_t* out_level) {
    if (!level_live(cpu)) return -1;
    if (!player_placeable(cpu)) return -2;
    return game_read_player_pose(cpu, out_x, out_y, out_z, out_yaw, out_level) >= 0 ? 0 : -1;
}

int game_read_player_pose(CPUState* cpu, int32_t* out_x, int32_t* out_y, int32_t* out_z,
                          int32_t* out_yaw, int32_t* out_level) {
    if (!level_live(cpu)) return -1;
    *out_x = (int32_t)cpu->read_word(D->player_pos + 0);
    *out_y = (int32_t)cpu->read_word(D->player_pos + 4);
    *out_z = (int32_t)cpu->read_word(D->player_pos + 8);
    *out_yaw   = D->player_facing ? (int32_t)(cpu->read_word(D->player_facing) & 0xFFFu) : -1;
    *out_level = D->level_id ? (int32_t)cpu->read_word(D->level_id) : -1;
    return player_placeable(cpu) ? 1 : 0;
}

/** @brief Read OPENPETE_FREECAM_AT_VBL, the feed-key value the scripted engage waits for.
 *
 * A timed engage passes a replaying bk2's refusal; the movie desyncs from
 * that VBL on, since the camera follow is frozen, so it serves captures of a
 * state the movie reaches, taken at or just after the VBL.
 * @return The VBL, or -1 when unset, to engage as soon as the level is
 *         live. */
static int freecam_engage_vbl(void) {
    const char* at = getenv("OPENPETE_FREECAM_AT_VBL");
    return (at && *at) ? atoi(at) : -1;
}

/** @brief Engage the free camera for a scripted pose or path.
 *
 * With @p through_bk2 and a bk2 active, the bk2 flag is cleared around the
 * toggle so the movie's refusal does not apply, then restored.
 * @param through_bk2 True bypasses the bk2 refusal.
 * @return False when the engage was refused, else true. */
static bool freecam_engage_scripted(bool through_bk2) {
    const bool bk2 = through_bk2 && openpete_bk2_active();
    if (bk2) openpete_bk2_set_active(false);
    const bool engaged = openpete_freecam_toggle();
    if (bk2) openpete_bk2_set_active(true);
    return engaged;
}

/* Scripted engage from the environment; see freecam_game.h. */
void freecam_maybe_autoengage(CPUState* cpu) {
    static int s_done = 0;
    if (s_done) return;
    if (player_pose_pending()) return;   /* the placement fires first */
    const char* pose = openpete_config_env("OPENPETE_FREECAM_POSE");
    const char* path = getenv("OPENPETE_FREECAM_PATH");
    const char* eng  = getenv("OPENPETE_FREECAM");
    int want = (pose && *pose) || (path && *path) ||
               (eng && *eng && *eng != '0');
    if (!want || openpete_freecam_active()) { s_done = 1; return; }
    const int at_vbl = freecam_engage_vbl();
    if (at_vbl >= 0 && openpete_feed_key_now() < (unsigned)at_vbl) return;
    if (level_live(cpu)) {
        s_done = 1;
        if (!freecam_engage_scripted(at_vbl >= 0)) return;   /* refused (bk2 replay/record) */
        /* A path supersedes a single pose: it stamps the camera every tick,
         * so an initial jump would only be overwritten on the next one. */
        if (path && *path) freecam_path_parse(path);
        else if (pose && *pose) s_pending_pose = pose;
    }
}

/** @brief Report whether a function entry is on the descriptor's extra freeze list.
 *
 * @param addr The function's guest address.
 * @return 1 when the zero-terminated extra_freeze list holds the address,
 *         else 0. */
static int in_extra_freeze(uint32_t addr) {
    if (!D->extra_freeze) return 0;
    for (const uint32_t* p = D->extra_freeze; *p; ++p)
        if (*p == addr) return 1;
    return 0;
}

int freecam_should_freeze(CPUState* cpu, uint32_t addr) {
    if (!openpete_freecam_active()) return 0;

    /* The player's own update: suppress it entirely so the scene holds
     * still. */
    if (addr == D->player_update || in_extra_freeze(addr))
        return !D->freeze_only_when_live || level_live(cpu);

    /* The camera follow: replace it with our drive (same fiber, same tick
     * point), then suppress the original so it can't overwrite the camera. */
    if (addr == D->cam_follow) {
        if (D->freeze_only_when_live && !level_live(cpu)) {
            /* The follow runs and moves the camera; the host pose is re-read
             * at the next frozen entry. */
            s_pose.seeded = 0;
            return 0;
        }
        /* A pending T: move the player up to the camera and hand the camera
         * back. Disengaging here rather than on the host loop keeps the two
         * halves atomic - the same tick that moves the player is the one
         * that stops freezing the player update. Returning 0 then lets the
         * real follow run immediately, so it re-derives the follow camera
         * from the player's new position on this very tick and there's no
         * frame of stale free-camera view before it snaps back. */
        if (openpete_freecam_take_teleport_request()) {
            openpete_bugrec_taint(BUGREC_TAINT_TELEPORT);
            freecam_teleport_player_to_camera(cpu);
            openpete_freecam_toggle();
            s_pose.seeded = 0;
            return 0;
        }
        freecam_drive(cpu);
        return 1;
    }
    return 0;
}
