/* core/include/openpete_bugreport.h - the bug-report bundle: one zip a
 * reader with this repository and no access to the tester's machine debugs
 * from.
 *
 * A press (the `bug_report` key or
 * the harness variable OPENPETE_BUGREPORT_AT) queues a state op, so the
 * press runs at the next final yield on the sim thread
 * (openpete_bugreport_press). There it copies what moves: the press
 * snapshot (a state file image with the press-time card images), the
 * recorder's replay anchor, the segment bk2 from the anchor, the hash rows,
 * the host-trace, pace and input rings, the hang watchdog's dispatch rings,
 * the session's subsystem reports (logged into a buffer, not to stderr),
 * the config and keymap reports and the mods' effective config rows. It
 * raises the screenshot request with the bundle's staging path; the present
 * services take the image and, with the present thread's device borrowed,
 * the display reports (openpete_bugreport_display_service). The device
 * list and the monitors are collected on the main thread by the event pump
 * (openpete_bugreport_main_poll).
 *
 * A worker thread then waits for those answers (each marked absent after
 * its bound), reads the files (logs, hang files, config files, the enabled
 * mods' directories and loaded modules, the recomp module), redacts the
 * text members and the state files' card paths, scans the members it never
 * rewrites for the home path, user name and host name, hashes every
 * member, writes manifest.json and the zip under
 * openpete_paths_data_dir("bugreports"). Before the zip, a key or menu
 * press asks the tester for a one-line note (notes.txt) through the dev
 * overlay's prompt with the sim paused; with no text input the note is
 * skipped. The sim thread joins the worker at its next poll and posts the
 * toast. A press while a bundle is in flight is
 * refused with a toast. Nothing stores into PSX RAM.
 */
#ifndef OPENPETE_BUGREPORT_H
#define OPENPETE_BUGREPORT_H

#include "openpete_thread_roles.h"

#include <stddef.h>
#include <stdint.h>

typedef struct CPUState CPUState;
struct openpete_options;

#ifdef __cplusplus
extern "C" {
#endif

/* manifest.json's schema version. 2 adds redaction.dropped and
 * redaction.flagged with their totals; a reader accepts 1 and 2. */
#define OPENPETE_BUGREPORT_SCHEMA 2

/** @brief Read the harness variables and register the fatal-exit bundle.
 *
 * OPENPETE_BUGREPORT_AT=N presses at feed key N. OPENPETE_BUGREPORT_NOTE=0
 * never asks for the tester's note, =1 asks for it on a harness press too
 * (by default only a key or menu press asks). Registers the
 * OP_EXIT_FLUSH_BUGREPORT exit flush, which writes a bundle at a
 * hang-watchdog trip or a CdSync spin-guard exit: the replay window, the
 * rings, the reports and collectors, the logs and that exit's hang dump,
 * with no press.state or screen.png. Called once before the host loop, on
 * the sim's thread.
 * @param opts The run options, kept for the fatal bundle's argv and disc. */
void openpete_bugreport_init(const struct openpete_options* opts);

/** @brief Ask for a bug report at the next final yield; safe from any thread (the settings menu row). */
void openpete_bugreport_request(void);

/** @brief Report whether a press is due this iteration: the key edge, a request or the harness feed key; and post the toast of a finished bundle.
 *
 * Called from the state-op collector on the sim thread once per quiescent
 * iteration. A due press while a bundle is in flight is refused with a
 * toast and returns 0.
 * @return 1 when the caller queues the press op. */
int openpete_bugreport_due(void) OP_REQUIRES(sim);

/** @brief Take the press at a final yield: copy the snapshot, rings and reports, raise the screenshot request and start the worker.
 *
 * @param cpu The guest register file.
 * @param opts The run options (argv, disc, game).
 * @return 0 when the worker started, -1 when the press failed (logged and toasted). */
int openpete_bugreport_press(const CPUState* cpu, const struct openpete_options* opts) OP_REQUIRES(sim);

/** @brief Take the pending bundle screenshot request: the staging path of screen.png.
 *
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @return 1 when a bundle waits for its image, else 0. */
int openpete_bugreport_take_shot(char* out, size_t cap) OP_REQUIRES(sim);

/** @brief Record the bundle screenshot's outcome and run the display reports while the device is borrowed.
 *
 * The display reports are the present-thread report, the native present
 * and eager censuses, the swapchain line and the Vulkan device report;
 * they are logged into the bundle's buffer on the calling thread, which
 * holds the display.
 * @param shot_ok Nonzero when screen.png was written. */
void openpete_bugreport_display_service(int shot_ok) OP_REQUIRES(sim);

/** @brief Collect the main-thread facts of a pending bundle: the input devices, monitors, video and audio drivers.
 *
 * Called by the event pump on the thread that owns the window; returns at
 * once when nothing is pending. */
void openpete_bugreport_main_poll(void);

/** @brief Record the knob registry's values on a committed tick: every row's value at the first call, then each live row's change with the tick it was seen at.
 *
 * Covers every route a live value moves by (the dev overlay, the settings
 * menu, a hotkey), since it reads each row's getter rather than hooking the
 * setters: numeric rows compare the getter's number each call, string rows
 * their text, and the OPENPETE_* rows only when the registry's env epoch
 * moved. Runs knob_registry_init first when no window ran it. Applies the
 * harness schedule OPENPETE_BUGREPORT_KNOB_AT=KEY:id=value[,...] (a row set
 * through its setter once the feed key reaches KEY) before it samples.
 * Host-side only: it calls getters and, for the schedule, a live row's
 * setter, which writes renderer or host state.
 * @param opts The run options, for knob_registry_init. */
void openpete_bugknobs_poll(const struct openpete_options* opts) OP_REQUIRES(sim);

struct knob_desc;   /* openpete_knobs.h */

/** @brief Report whether a registry row is an OPENPETE_* variable row: an env spelling, no flag, a string value.
 * @param k The row.
 * @return 1 for such a row, 0 otherwise. */
int openpete_bugknobs_is_env(const struct knob_desc* k);

/** @brief Name a registry row's replay class: action, latched, env, host, sim, follow or render.
 *
 * Action, restart-only and OPENPETE_* variable rows derive theirs; every
 * other live row declares it in knob_desc_t.replay (knob_replay_t): the
 * replay leaves host rows to its own options, sim rows to the sim's
 * timeline and follow rows to game state, and applies render rows only.
 * An undeclared class reads "unset", which the replay never applies.
 * knobs.txt records it for the reader; the replay classifies this build's
 * own row rather than reading it back.
 * @param k The row.
 * @return The class word (static). */
const char* openpete_bugknobs_class(const struct knob_desc* k);

/** @brief Write knobs.txt from the last sample: one `knob` line per registry row (value, boot value, default, boot source, flags, class) and one `change` line per recorded change.
 *
 * Reads only the recorder's own copies, so a fatal exit off the sim
 * thread may call it; the press samples first (openpete_bugknobs_poll).
 * @return The text (heap; the caller frees it), or NULL when out of
 *   memory or before the first sample. */
char* openpete_bugknobs_text(void);

/** @brief Wait for an in-flight bundle to finish and release it; called at session close. */
void openpete_bugreport_shutdown(void) OP_REQUIRES(sim);

/** @brief Log the thumbnail ring's counters, then the bundle counters: presses, bundles written and failed, the last bundle's members, bytes, replacements, findings and sim-thread press time. */
void openpete_bugreport_report(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_BUGREPORT_H */
