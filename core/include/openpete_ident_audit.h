#include "openpete_thread_roles.h"   /* OP_REQUIRES */
/* openpete_ident_audit.h - every-tick sub-tick-spine identity audit.
 *
 * The canonical dump at VBL F composes the current scene: the newest tick's
 * feed. The explicit sub-tick re-extract phased onto that same tick's stamp
 * (alpha 1) must rebuild the identical scene, so the two composes must be
 * pixel-identical. A residual is a piece of gameplay state the spine does not
 * carry across the re-extract.
 *
 * The corpus's `ident` leg checks that on a handful of sampled frames by
 * diffing PNGs. This audit checks it on EVERY canonical present, in-binary, and
 * emits one line per tick instead of a pair of images - so the whole bk2 corpus
 * can be swept in one unattended headless run without writing thousands of
 * files.
 *
 * Armed by OPENPETE_IDENT_AUDIT=<start_vbl>; a harness variable, deliberately
 * outside the knob registry. OPENPETE_IDENT_AUDIT_DUMP=<px> additionally writes
 * the two composes as PNGs for any tick whose differing-pixel count is at or
 * above that threshold.
 *
 * The audit is a pure observer: it brackets its re-extract with the scene
 * save/restore pair and puts back the present-target and capture-delay flags,
 * so the run it measures behaves as though the audit were not there.
 */
#ifndef OPENPETE_IDENT_AUDIT_H
#define OPENPETE_IDENT_AUDIT_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Return nonzero when OPENPETE_IDENT_AUDIT names a start VBL of zero or more.
 *
 * The first call reads OPENPETE_IDENT_AUDIT and OPENPETE_IDENT_AUDIT_DUMP and
 * caches both, so the call is cheap enough for every present. run.c gates the
 * per-present audit on it, and the native renderer brings itself up under
 * --headless when it is set.
 * @return 1 when the audit is armed, else 0. */
int openpete_ident_audit_armed(void);

/** @brief Run one identity audit on the canonical present that has just composed its scene.
 *
 * Composes the canonical scene, then the sub-tick re-extract phased onto the
 * identity instant (alpha 1) with the scene saved and restored around it,
 * prints one `[ident-audit f= tick= px= maxch=]` line with the count of
 * differing pixels and the largest single-channel delta, and records the row
 * for the exit summary (percentiles and the ten worst ticks, printed from an
 * atexit handler). With OPENPETE_IDENT_AUDIT_DUMP=<px> set and the count at or
 * above it, writes both composes as PNGs beside the screenshot path. No-op
 * before the start VBL, when the native path is off, when no re-extract is
 * registered, or when either capture fails or their sizes differ.
 * @param cpu Guest CPU passed to the registered re-extract.
 * @param vbl openpete_frame at this present.
 * @param tick openpete_canon_tick at this present.
 * @param shot_output The --screenshot-output template the PNG dump names its
 *        files after; NULL writes no dump. */
void openpete_ident_audit_present(void *cpu, unsigned vbl, unsigned tick,
                                  const char *shot_output) OP_REQUIRES(sim);

#ifdef __cplusplus
}
#endif
#endif
