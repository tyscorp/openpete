/* tests/unit/bugreport_replay_knobs_test.c - --bug-report-replay's knob
 * application (core/record/bugreport_replay_knobs.c) against a knobs.txt
 * that mislabels rows: the replay applies a recorded value only to a row
 * this build classifies `render` (core/runtime/bugknobs_class.c), whatever
 * class the bundle's line names. The registry is a stub of three live
 * rows declaring their replay class, each counting its setter's calls; br_read_file is a stub serving
 * the member text. */

#include "../../core/record/bugreport_internal.h"
#include "openpete_bugreport.h"
#include "openpete_knobs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_set_freecam, s_set_vsync, s_set_bloom;

/** @brief Count a setter call on the freecam row. */
static int set_freecam(knob_desc_t* k, double v) { (void)k; (void)v; s_set_freecam++; return 0; }
/** @brief Count a setter call on the vsync row. */
static int set_vsync(knob_desc_t* k, double v) { (void)k; (void)v; s_set_vsync++; return 0; }
/** @brief Count a setter call on the bloom row. */
static int set_bloom(knob_desc_t* k, double v) { (void)k; (void)v; s_set_bloom++; return 0; }

static knob_desc_t s_rows[] = {
    { .id = "freecam", .cli = "--freecam", .type = KNOB_BOOL, .mut = KNOB_LIVE, .set_num = set_freecam,
      .replay = KNOB_REPLAY_SIM },
    { .id = "vsync", .cli = "--vsync", .type = KNOB_BOOL, .mut = KNOB_LIVE, .set_num = set_vsync,
      .replay = KNOB_REPLAY_HOST },
    { .id = "bloom", .cli = "--bloom", .type = KNOB_BOOL, .mut = KNOB_LIVE, .set_num = set_bloom,
      .replay = KNOB_REPLAY_RENDER },
};

knob_desc_t* knob_registry(size_t* count) {
    *count = sizeof s_rows / sizeof s_rows[0];
    return s_rows;
}

/** @brief Find a stub row by id, as the registry's lookup does.
 *
 * @param id The row id.
 * @return The row, or NULL. */
knob_desc_t* knob_registry_find(const char* id) {
    for (size_t i = 0; id && i < sizeof s_rows / sizeof s_rows[0]; i++)
        if (!strcmp(s_rows[i].id, id)) return &s_rows[i];
    return NULL;
}

/* The member: freecam (sim) and vsync (host) labelled render, bloom
 * (render) labelled host; each changed live before the anchor (flag C) and
 * once more after it. Fields: knob id class value boot - - flags raw;
 * change frame - - t_s id old value old_raw raw. */
static const char k_member[] =
    "session\tdropped=0\n"
    "knob\tfreecam\trender\ton\toff\t-\t-\tC\t1\n"
    "knob\tvsync\trender\toff\ton\t-\t-\tC\t0\n"
    "knob\tbloom\thost\ton\toff\t-\t-\tC\t1\n"
    "change\t200\t-\t-\t6.6\tfreecam\ton\toff\t1\t0\n"
    "change\t200\t-\t-\t6.6\tvsync\toff\ton\t0\t1\n"
    "change\t200\t-\t-\t6.6\tbloom\ton\toff\t1\t0\n";

int br_read_file(const char* path, uint8_t** out, size_t* len) {
    (void)path;
    *len = sizeof k_member - 1;
    *out = (uint8_t*)malloc(sizeof k_member);
    if (!*out) return -1;
    memcpy(*out, k_member, sizeof k_member);
    return 0;
}

int main(void) {
    int fail = 0;
    (void)br_knobs_replay_load("unused", 100);
    br_knobs_replay_tick(300);
    br_knobs_replay_report();
    if (s_set_freecam) { fprintf(stderr, "FAIL: sim row freecam applied %d times under a render label\n", s_set_freecam); fail = 1; }
    if (s_set_vsync) { fprintf(stderr, "FAIL: host row vsync applied %d times under a render label\n", s_set_vsync); fail = 1; }
    if (s_set_bloom != 2) {
        fprintf(stderr, "FAIL: render row bloom applied %d times under a host label, want 2 (anchor, tick)\n", s_set_bloom);
        fail = 1;
    }
    if (fail) return 1;
    printf("PASS bugreport_replay_knobs_test\n");
    return 0;
}
