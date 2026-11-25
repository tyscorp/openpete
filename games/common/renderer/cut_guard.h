/* games/common/renderer/cut_guard.h - the sub-tick discontinuity guard the
 * re-extract bodies share: the camera-cut predicate, the hold verdict, the
 * census, and the whole camera bracket for a body that needs no more than the
 * linear lerp.
 *
 * A guard fires when the two ticks of the pair are not one motion: the camera
 * jumped, turned or broke its motion, or an object jumped.
 * The interval between the two ticks shows tick N-1 and reaches tick N at
 * alpha 1, so a guard holds N-1 for every present below alpha 1 and lets N
 * show at alpha 1 - the discontinuity lands on the present where every
 * interpolated channel reaches its canonical N value. A guard with no previous
 * state to hold (no open interval, no staging arena, no prev snapshot) snaps
 * to N instead and the census counts the reason. */
#ifndef OPENPETE_GAME_CUT_GUARD_H
#define OPENPETE_GAME_CUT_GUARD_H

#include <stdint.h>

#include "openpete_interp.h"
#include "openpete_interp_objs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Guard kinds, in census-tally order. */
enum { CUT_CAM = 0, CUT_CLASS = 1, CUT_TELEPORT = 2, CUT_HELD = 3, CUT_KINDS = 4 };

/** @brief Whether the cut census is armed: OPENPETE_CUT_CENSUS set to a nonempty value not starting with '0', read once.
 *
 * It arms every stderr print below; the callers also gate
 * cut_locate_camera on it.
 * @return 1 when armed, else 0. */
int cut_census_on(void);

/** @brief Count one guard decision in the per-kind hold/snap tallies and, with the census armed, print it on one line.
 *
 * The line carries the kind, the object, the verdict and its reason, both
 * positions and their distance D, and the running tallies, which are the
 * guards' engagement counters. The tallies count whether or not the census
 * is armed, so they are complete from the first present. A kind outside
 * 0..CUT_KINDS-1 is ignored.
 * @param kind CUT_CAM, CUT_CLASS, CUT_TELEPORT or CUT_HELD.
 * @param addr The object's address; the camera guard passes 0.
 * @param held Nonzero when the guard held tick N-1, zero when it snapped.
 * @param why The verdict's reason.
 * @param prev_pos World position of tick N-1; NULL reads as the origin.
 * @param shown_pos World position shown; NULL reads as the origin. */
void cut_census_note(int kind, uint32_t addr, int held, const char* why,
                     const int32_t* prev_pos, const int32_t* shown_pos);

/* Which of a producer's two captured sets a held present shows. */
typedef enum {
    CUT_HELD_EMPTY = 0,   /* tick N-1 captured nothing: show no set */
    CUT_HELD_CURR  = 1,   /* the current set is tick N-1's: tick N captured nothing */
    CUT_HELD_PREV  = 2,   /* the previous set is tick N-1's: swap it in */
} cut_held_pick_t;

/** @brief Which of a producer's two captured sets is tick N-1's, the set a held present shows.
 *
 * A producer keeps the set of the last canonical epoch it captured in and the
 * set it retired at that capture, each with its epoch. Tick N is the last
 * committed canonical epoch (spyro_native_canon_epoch_fresh); a held present
 * shows exactly tick N-1's set whatever tick N captured: the current set when
 * its epoch is N-1, the previous set when the current is N and the previous
 * N-1, and no set otherwise, since a producer that captured nothing in epoch
 * N-1 drew nothing then.
 * @param curr_epoch The current set's canonical epoch.
 * @param prev_epoch The previous set's canonical epoch.
 * @return The set to show. */
cut_held_pick_t cut_held_set_pick(uint32_t curr_epoch, uint32_t prev_epoch);

/* The two captured sets a producer keeps so a held present can show tick
 * N-1's: the set of the last canonical epoch it captured in and the set that
 * capture retired. The producer owns both buffers; `swap` exchanges them with
 * their counts and `n` points at the current set's count. */
typedef struct cut_held_sets {
    void      (*swap)(void);  /* exchanges the producer's current and retired sets */
    const int*  n;            /* the current set's item count */
    const char* kind;         /* the census name */
    uint32_t    epoch[2];     /* canonical epoch of the current set, of the retired set */
    unsigned    vbl;          /* the VBL of the render the current set was opened in */
} cut_held_sets_t;

/** @brief Open one capture in the canonical epoch in flight: the epoch's first capture retires the current set.
 *
 * On retirement the producer's sets are exchanged, so the current buffer is
 * the one retired an epoch earlier; the caller empties it before appending.
 * A runahead speculation renders its ticks inside the committed tick's epoch;
 * a speculative render's first capture empties the current set without
 * retiring it, so the set holds the last speculative render's items alone.
 * @param s The producer's sets.
 * @return 1 on the epoch's first capture and on a speculative render's first
 *         capture, else 0. */
int cut_held_sets_capture(cut_held_sets_t* s);

/** @brief Select the set a re-extract shows and, on a held cut, swap tick N-1's set in.
 *
 * Outside a hold nothing changes. On a hold the choice is
 * cut_held_set_pick's; CUT_HELD_PREV exchanges the two sets until
 * cut_held_sets_leave, and the census line names the set shown.
 * @param s The producer's sets.
 * @param hold Nonzero on a held camera cut.
 * @return -1 outside a hold, else the cut_held_pick_t shown, the argument
 *         hook_reextract_ready_held takes. */
int cut_held_sets_enter(cut_held_sets_t* s, int hold);

/** @brief Undo cut_held_sets_enter: exchange the sets back when it swapped tick N-1's set in.
 *
 * @param s The producer's sets.
 * @param pick cut_held_sets_enter's return value. */
void cut_held_sets_leave(cut_held_sets_t* s, int pick);

/** @brief Print which captured set one channel shows on a held cut, when the census is armed.
 *
 * @param kind The channel's name.
 * @param pick The set shown (cut_held_set_pick).
 * @param epoch The canonical epoch of the shown set; for CUT_HELD_EMPTY the
 *        current set's.
 * @param n The shown set's item count, 0 for CUT_HELD_EMPTY. */
void cut_census_held_set(const char* kind, cut_held_pick_t pick, unsigned epoch, int n);

/** @brief Print the camera a present showed, at the alpha it showed it, when the census is armed.
 *
 * A snapped present writes no bracket trace, so this line is the only record
 * of what the interval drew. Nothing is printed when `pos` or `mat` is NULL.
 * @param pos The shown world position, int32 x/y/z.
 * @param mat The shown rotation; its first row is printed.
 * @param alpha The alpha the present drew at.
 * @param cam_cut Nonzero when the pair is a camera cut. */
void cut_census_camera(const int32_t* pos, const int16_t* mat,
                       double alpha, int cam_cut);

/* Where the camera's fields sit inside a game's camera transform block, and
 * which ring row carries the transform. */
typedef struct cut_cam_desc {
    int      cam_region;     /* ring row index of the camera transform */
    uint32_t pos_off;        /* int32 x/y/z world position */
    uint32_t mat_off;        /* the 3x3 int16 rotation the cut test reads */
} cut_cam_desc_t;

/* The measured inputs of one cut decision, for the callers' diagnostics. */
typedef struct cut_cam_metrics {
    double   step;      /* position step length, world units */
    double   rot;       /* largest per-row rotation angle, 4096 to a full turn */
    double   prev_rot;  /* the previous step's rotation (0 after a hold) */
    double   jerk;      /* length of the change from the previous position step */
    int      prev_cut;  /* the previous pair is a cut one tick earlier */
} cut_cam_metrics_t;

/** @brief Decide whether the newest camera step is a discontinuity rather than one motion.
 *
 * The step is prev to curr; the previous step is prev2 to prev, counted as
 * zero (rest) when prev2 is NULL or curr follows prev by CUT_HOLD_VBL (4)
 * VBLs or more. The rotation of a step is the largest angle a row of the
 * rotation matrix turns, 4096 units to a full turn. The step is a cut when
 * its position moves more than 1440 world units ("pos"), its rotation
 * exceeds 768 ("rot"), its rotation exceeds 256 and eight times the previous
 * step's ("rot-onset"), or its position step differs from the previous one
 * by more than 800 units ("step-change") unless the previous pair is itself
 * a cut one tick earlier, or its position moves more than 400 units after a
 * previous step at rest, at most 2 units and 2 rotation units ("rest-start").
 * The previous pair's verdict is taken from prev2 and prev alone, its own
 * predecessor counted as rest for the first four terms; the rest-start term
 * does not apply to it.
 * @param prev2 The snapshot before prev, or NULL.
 * @param prev The previous tick's snapshot.
 * @param curr The current tick's snapshot.
 * @param desc The game's camera descriptor; NULL, or a snapshot without the
 *        camera row, yields NULL.
 * @param out Receives the measured inputs when non-NULL, whatever the verdict.
 * @return "pos", "rot", "rot-onset", "step-change" or "rest-start", or NULL
 *         when the pair is one motion. */
const char* cut_camera_reason(const openpete_interp_snapshot_t* prev2,
                              const openpete_interp_snapshot_t* prev,
                              const openpete_interp_snapshot_t* curr,
                              const cut_cam_desc_t* desc, cut_cam_metrics_t* out);

/** @brief Whether an object's prev/curr pair is two consecutive ticks.
 *
 * prev's observation stamp is the later of prev_ns (its first capture) and
 * prev_seen_ns (the last capture that still observed it); the pair qualifies
 * when curr_ns is after that stamp by at most 1.5 tick periods. A record that
 * held prev unchanged on every capture up to curr qualifies however long it
 * held it. An object whose prev was last observed earlier than that has no
 * tick N-1 to show: a level load leaves pre-load records in the table, and a
 * slot the capture did not observe in between names a state the interval does
 * not span.
 * @param o The interp object; NULL, a zero prev_ns, or curr_ns not after
 *        prev_ns yields 0.
 * @return 1 when the pair spans one tick, else 0. */
int cut_pair_is_tick(const openpete_interp_obj_t* o);

/** @brief Whether a feeder pool record's snapshot pair spans one tick of motion, by prev's observation stamp (cut_pair_is_tick).
 *
 * The particle, glint, glow, zap and tracer pools are walked on every tick,
 * so a record that holds one state on every capture keeps prev observed up to
 * the change, and its first move after a rest pairs instead of snapping.
 * OPENPETE_FEEDER_PAIR_AGE=1 restores the roll-stamp test (the two distinct
 * states at most 1.5 tick periods apart) for a same-binary A/B.
 * OPENPETE_FEEDER_PAIR_CENSUS=1 prints one line per call: the feeder, the
 * key, the ticks since curr rolled, the roll span, the span from prev's
 * observation stamp, whether the bytes at pos_off changed, and both verdicts.
 * @param name The feeder's census name.
 * @param key The record's key.
 * @param o The record's pair, with have_prev set and curr_ns after prev_ns.
 * @param pos_off Offset of the bytes whose change the census counts as a move.
 * @param pos_len Their length.
 * @return 1 when the pair qualifies, else 0. */
int cut_feeder_pair_is_tick(const char* name, uint32_t key, const openpete_interp_obj_t* o,
                            uint32_t pos_off, uint32_t pos_len);

/* The lowest phase a sub-tick present takes past the current tick's stamp.
 * The canonical present fires a few ms before its tick's grid stamp, so its
 * phase is slightly negative; the camera bracket (cam_bracket_t) extrapolates
 * back to this floor rather than clamping at 0, and every channel that moves
 * with the scene floors its own phase here so it extrapolates with the camera
 * instead of holding while the camera moves. */
#define CUT_PHASE_FLOOR (-0.35)

/** @brief Whether one object's world-position step between two ticks is a jump rather than a move, by the game's distance bound.
 *
 * The step is a jump when its length exceeds game_layout.moby.jump_bound
 * world units (OPENPETE_MOBY_JUMP_BOUND overrides it); a bound of 0 is none,
 * and no step is a jump.
 * @param prev_pos Tick N-1's int32 x/y/z position.
 * @param curr_pos Tick N's int32 x/y/z position.
 * @return 1 for a jump, else 0. */
int cut_pos_jump(const int32_t* prev_pos, const int32_t* curr_pos);

/** @brief Whether one moby's step between two ticks is a jump, by the game's own marker where it has one, and which rule decided.
 *
 * The rules, in order: moby_game.cutpath_jump decides for a moby the game's
 * cutscene path sampler drives (a placement that does not continue a run
 * of its track); a row of the placement table
 * (game_layout.moby.place) matching the record's class and state transition
 * is a jump; a director row (game_layout.moby.director) whose ring record's
 * state edge or pointer edge names this moby is a jump; a props-word row
 * (game_layout.moby.props_mark) whose props word goes 0 -> nonzero on the
 * step's tick is a jump; an attachment row (game_layout.moby.attach) whose
 * parent's step on the same tick is a jump is a jump; a pair a regime row
 * (game_layout.moby.regime) holds on is a jump; every other step takes the
 * distance bound (cut_pos_jump) on the positions at game_layout.moby.pos_off.
 * OPENPETE_MOBY_JUMP_MARKERS=0 skips every marker. No rule measures distance
 * before the bound.
 * @param addr The moby's guest address.
 * @param prev_rec Tick N-1's moby record.
 * @param curr_rec Tick N's moby record.
 * @param why Receives "cutpath", "table", "director", "props", "attach", "regime", "bound" or "none" when
 *        non-NULL.
 * @return 1 for a jump, else 0. */
int cut_obj_jump_reason(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec,
                        const char** why);

/** @brief Whether one moby's step between two ticks is a jump: cut_obj_jump_reason without the reason.
 *
 * @param addr The moby's guest address.
 * @param prev_rec Tick N-1's moby record.
 * @param curr_rec Tick N's moby record.
 * @return 1 for a jump, else 0. */
int cut_obj_jump(uint32_t addr, const uint8_t* prev_rec, const uint8_t* curr_rec);

/** @brief Whether one moby's step from tick N-2 to tick N-1 was a jump, so a curve through the three records must not span it.
 *
 * The step is a jump on a class change or on cut_obj_jump's rules applied to
 * the older pair (the ring-row rules read the ring's prev2 and prev
 * snapshots). A Hermite position curve whose start tangent runs back across
 * such a step overshoots by up to half the jump on tick N's segment.
 * @param addr The moby's guest address.
 * @param prev2_rec Tick N-2's moby record.
 * @param prev_rec Tick N-1's moby record.
 * @return 1 for a jump, else 0. */
int cut_obj_prev_jump(uint32_t addr, const uint8_t* prev2_rec, const uint8_t* prev_rec);

/** @brief Capture the props block of one moby record as an interp object keyed by the props address, when a props or attachment marker row names its class in the resident overlay.
 *
 * The span runs from the block's start through the last word a matching row
 * reads (game_layout.moby.props_mark, game_layout.moby.attach). Called by the
 * moby capture after each record's capture, with the record it captured.
 * Reads guest RAM only.
 * @param rec The moby record. */
void cut_props_capture(const uint8_t* rec);

/** @brief With OPENPETE_JUMP_CENSUS=<min> set, print the jump verdict of one moby the capture just rolled, when its step is at least min world units.
 *
 * Called by the moby capture after the array walk, once per captured record.
 * The line carries the VBL, the object key, tick N's class, both state bytes,
 * the step length, each rule's own verdict (the sampler's -1/0/1, the
 * per-moby markers as `table`, the regime rows as `load`, the bound in force), the verdict and its rule
 * (cut_obj_jump_reason, or "class" for a class change). Objects that did not
 * roll on this capture print nothing.
 * @param key The object's interp key, which the array capture takes as the
 *        guest address. */
void cut_jump_census_capture(uint32_t key);

/** @brief On a present that holds a camera cut, whether an object rolled on the cut's tick N.
 *
 * A held cut shows tick N-1 on every record: an object that rolled at N shows
 * its previous state, one that did not shows its current state, which is
 * already its state at N-1. Rolled means its current stamp is within half a
 * tick period of the held snapshot's stamp or later. The held stamp is set by
 * cut_camera_hold (OPENPETE_CUT_HOLD_ALL=0 never sets it) and cleared by
 * cam_bracket_run and cut_obj_present_end.
 * @param o The interp object.
 * @return -1 when no cut is held or the object has no previous state, 1 when
 *         it rolled at tick N, 0 when it did not. */
int cut_held_rolled(const openpete_interp_obj_t* o);

/** @brief Open a sub-tick present's per-object hold set, emptying the previous present's.
 *
 * Every object cut_obj_hold decides on is recorded in the set until the next
 * cut_obj_present_begin or cut_obj_present_end, so each channel that reads the
 * object on the same present reads one verdict. */
void cut_obj_present_begin(void);

/** @brief Close the present's per-object hold set: cut_obj_held reports every object undecided until the next cut_obj_present_begin. */
void cut_obj_present_end(void);

/** @brief Record whether this present's camera cut without a hold (the view shows tick N).
 *
 * cam_bracket_run records its own verdict; a body that brackets its camera
 * itself calls this once per present before any cut_obj_hold.
 * @param snap Nonzero when the camera cut and shows tick N. */
void cut_set_camera_snap(int snap);

/** @brief This present's verdict for one object.
 *
 * @param addr The object's guest address.
 * @return 1 when cut_obj_hold held it, 0 when it snapped, -1 when no verdict
 *         was taken on this present or the set is closed. */
int cut_obj_held(uint32_t addr);

/** @brief Decide whether the sub-tick view holds one object whole at tick N-1 for the rest of the interval, for a class change or a position jump between its two ticks.
 *
 * The hold refuses, leaving tick N (the snap), when the pair is not two
 * consecutive ticks (cut_pair_is_tick, "stale"), when the object's alpha has
 * reached 1 ("alpha1"), when the camera cut on this present without a hold
 * ("camsnap"; a record held at N-1 under a tick-N view stands where tick N no
 * longer has it), or when `stage` returns 0 ("unstaged"). The verdict is
 * noted in the census with both positions and recorded in the present's set
 * (cut_obj_held). Guest memory is never written.
 * @param kind CUT_CLASS, CUT_TELEPORT or CUT_HELD.
 * @param addr The object's guest address.
 * @param o The object's interp pair.
 * @param pos_off Offset of the int32 x/y/z position in the record.
 * @param alpha The object's alpha at this present over a denominator capped
 *        at one tick period.
 * @param stage Called once every other test passes, to publish the previous
 *        record to the view; NULL when the caller applies the hold through its
 *        own channels. It returns 0 to refuse.
 * @return 1 when the hold is in force, else 0. */
int cut_obj_hold(int kind, uint32_t addr, const openpete_interp_obj_t* o,
                 uint32_t pos_off, double alpha,
                 int (*stage)(uint32_t addr, const openpete_interp_obj_t* o));

/** @brief Whether this present can show tick N-1 whole.
 *
 * A held cut shows the previous tick on every channel, so every record the
 * moby pass will draw needs a previous tick of its own: a record that has none
 * is drawn at tick N whatever the camera does, and against a camera held one
 * whole cut away it leaves the view. The records checked are the drawn live
 * records of the moby array and the records the canonical render drew from
 * outside it. A record whose class changed does not refuse: it names a
 * reloaded model table and carries no drawable previous state either way.
 * With OPENPETE_CUT_CENSUS set, every refusal is counted and printed.
 * @return 1 when no drawn record refuses, else 0. */
int cut_scene_holdable(void);

/** @brief Whether the interval a camera cut ends still has a tick N-1 to show.
 *
 * The alpha is (openpete_interp_present_now() - curr_tick_ns) / period_ns,
 * or 1 when the period is not positive or the ring's head is stale (its
 * forward endpoint stopped advancing, so there is no open interval). The cut
 * holds while the alpha is below 1 and cut_scene_holdable() passes. The
 * verdict is noted in the census as kind CUT_CAM with reason "hold",
 * "scene" or "stale". A hold records curr_tick_ns as the held stamp
 * cut_held_rolled reads for the rest of the present.
 * @param period_ns The pair's tick period.
 * @param curr_tick_ns The current snapshot's tick stamp.
 * @return 1 to hold tick N-1, 0 to show tick N. */
int cut_camera_hold(double period_ns, op_time_t curr_tick_ns);

/** @brief Print a `locate=camcut` line when the ring pair at the present instant is a camera cut.
 *
 * The re-extract bodies call it once per canonical pass with the census
 * armed. The guard itself runs on intermediate presents, which a headless run
 * produces only where a capture asks for one, so this line names the ticks a
 * sub-tick capture must sample; it reports nothing about what was presented.
 * Nothing is printed when the ring has no pair or the game registered no
 * camera row.
 * @param desc The game's camera descriptor; NULL prints nothing. */
void cut_locate_camera(const cut_cam_desc_t* desc);

/* The result of one present's camera bracket. */
typedef struct cam_bracket {
    int      bracketed;        /* the staged camera carries an interpolated pair */
    int      cut;              /* the pair is a discontinuity */
    int      hold;             /* the interval is open and shows tick N-1 */
    int      stale;            /* curr is more than CUT_PAIR_STALE_TICKS ticks old
                                * at the present: a cut the pair carries is one the
                                * ring replays, not one this interval ends */
    double   alpha;            /* the alpha the present drew at (0 while held) */
    const uint8_t* tickcam;    /* curr's camera bytes, for a feeder that re-bases
                                * records composed against the tick's camera */
    double   fpos[3];          /* the un-rounded interpolated position */
} cam_bracket_t;

/** @brief Interpolate a snapshot pair's camera into `cam` and report what the present shows.
 *
 * `cam` is the staged camera copy; the sky matrix at `mat_off`, the world
 * matrix at `view_off` and the position at `pos_off` are written. The alpha
 * is the present instant's offset past curr's tick over the pair's cadence
 * (one nominal tick when the gap exceeds CUT_PAIR_STALE_TICKS ticks),
 * clamped to -0.35..1, and 1 when the ring's
 * head is stale. On a cut with an open interval (the pair at most
 * CUT_PAIR_STALE_TICKS old and cut_camera_hold passing) the bracket runs
 * with alpha pinned to 0, where the lerp is the previous state byte for
 * byte; on a cut with no open interval, or when `cam` or either snapshot's
 * camera row is missing, nothing is written and the caller presents the
 * live tick-N camera. Guest memory is never written.
 * @param cam The staged camera copy.
 * @param prev2 The snapshot before prev, or NULL; the cut rule reads the
 *        step it ends.
 * @param prev The previous snapshot.
 * @param curr The current snapshot.
 * @param desc The game's camera descriptor.
 * @param view_off Offset of the world matrix in the camera block.
 * @param out Receives the result when non-NULL. */
void cam_bracket_run(uint8_t* cam, const openpete_interp_snapshot_t* prev2,
                     const openpete_interp_snapshot_t* prev,
                     const openpete_interp_snapshot_t* curr,
                     const cut_cam_desc_t* desc, uint32_t view_off,
                     cam_bracket_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_GAME_CUT_GUARD_H */
