#!/usr/bin/env bash
#
# check_presentation_golden.sh - the presentation-golden CTest gate.
#
# Captures the native -vk presentation corpus at the current (default) depth
# encoding and compares each tile's verdict digest (decoded RGB payload, mode
# and size) against its row of the tier's committed blessed.tsv. The reference
# images are not in the tree: a passing run reads none, and a divergent row's
# reference is read from the local store or the archive for its heat diff
# (scripts/presentation-blessed.py). A clean tree is pixel-identical
# (deterministic under llvmpipe), so ANY divergence is a presentation
# regression the gameplay oracle is blind to. This is the strict
# pixel-identity leg (Mode B); an intended visual change is re-blessed with
# tests/harness/presentation-corpus-promote.sh after reference-first adjudication.
#
# Usage:
#   tests/harness/check_presentation_golden.sh [--build-dir=DIR] [--filter=SUBSTR]
#                                        [--freecam-filter=SUBSTR] [--bless]
#                                        [--repeat=N] [--list] [--game SLUG]
#                                        [--env OPENPETE_NAME=VALUE]...
#   tests/harness/check_presentation_golden.sh --verdict-only DIR [--game SLUG]
#   tests/harness/check_presentation_golden.sh --materialize-ref [--game SLUG]
#                                        [--tier T] [--at REV] <tile>...
#     --filter          row substring applied to every corpus leg by one
#                       rule (pg_row_selected): a bk2 row runs when its name
#                       contains it, a freecam row when its name field or tile
#                       key (`<level>-<pose md5 prefix>`) contains it, the
#                       credits leg when `credits-interp` contains it. A
#                       corpus with no matching row runs none of it and says
#                       so. Default: every row of every corpus.
#     --freecam-filter  the freecam corpus's filter in place of --filter (same
#                       rule). Given without --filter, the bk2 corpus and the
#                       credits leg run none of their rows.
#                       Either filter scopes the run to part of the corpus: the
#                       golden tiles it does not capture are then reported as
#                       out of scope instead of missing, and --bless promotes
#                       only the tiles it captured.
#     --list            print the rows each corpus leg selects under the
#                       filters (PRESENTATION_LIST in the corpus scripts) and
#                       exit 0; captures nothing and needs no binary
#     --repeat=N        capture each row the filters select N times as repeat
#                       copies (PRESENTATION_REPEAT, scripts/presentation-game.sh)
#                       and compare every copy's tiles and identity pairs
#                       against the original row's blessed digest and baseline
#                       row. Needs --filter or --freecam-filter; takes no
#                       --bless. The report adds `repeat: N`, one
#                       `repeat-row: <key> divergent=D of=T compared=C copies=N`
#                       per repeated row and the totals repeat-divergent,
#                       repeat-legs and repeat-compared; the run fails when a
#                       row's compared count is under N x its legs (its blessed
#                       tiles plus one identity pair per canon tile) or any
#                       copy diverged. Each copy is its own process at the
#                       run's CORPUS_JOBS, so a tile that differs between
#                       processes diverges in some copies only.
#     --bless           (re)capture, promote --all (rewrite the changed rows of
#                       blessed.tsv, file the tiles into the store) and
#                       re-baseline the identity leg, instead of gating
#     --game            game slug (default spyro-1); binary, disc, bk2 dir,
#                       manifests and golden all derive from it
#     --env OPENPETE_NAME=VALUE
#                       forward one game knob to every capture leg
#                       (repeatable), for an A/B against the golden. Without
#                       it no OPENPETE_* variable of the caller's environment
#                       reaches the game: the run prints each one once as
#                       harness (read by these scripts), forwarded or dropped
#                       (scripts/presentation-game.sh, game knobs). A run
#                       with --env records the knobs in
#                       <candidate>/forwarded-knobs.txt and its report's
#                       forwarded-knobs line; it takes no --bless or
#                       --verdict-only, and presentation-corpus-promote.sh
#                       refuses its candidate directory.
#     --environment-accept=REASON
#                       with --bless: admit a capture environment that departs
#                       from the manifest header (scripts/presentation-blessed.py,
#                       Environment); the header records REASON
#     --verdict-only DIR
#                       capture nothing: decide the verdict of the candidate
#                       directory DIR (a whole-corpus capture, taken elsewhere)
#                       with the post-capture steps a capturing run takes over
#                       its own candidates: the row census, the identity leg,
#                       the credits leg's recorded result and the strict diff
#                       (never --expect-subset). Takes no --bless, --filter or
#                       --freecam-filter. DIR must lie under this tree or the
#                       store's parent directory, the paths the capture image
#                       mounts; outside the image the run enters it through
#                       the wrapper's `run`, which builds no engine.
#
# The verdict is defined over the pinned capture image
# (tests/harness/linux-capture.Dockerfile). Run outside it, the script
# re-runs itself inside the image through scripts/golden-capture-container.sh,
# which builds the engine into build/linux-release-container with the image's
# toolchain (any --build-dir= is replaced by that directory) and holds a lock
# on it; this process stays the host-side parent for the whole run. Without a container
# engine or a current image the run reports SKIP (64) and prints the manifest
# header's environment beside this machine's. Inside the image every run prints
# the same comparison for its capture; a bless or promote whose environment
# departs from the header is refused.
#
# The candidate images are KEPT (build/presentation-candidate by default, or
# PRESENTATION_GOLDEN_OUT) so a selective promote costs nothing: adjudicate the
# diff, then run presentation-corpus-promote.sh against that directory. Only a
# whole-corpus re-bless needs --bless. PRESENTATION_GOLDEN_KEEP=0 discards them.
# Before capturing, the run writes capture-record.txt into the candidate
# directory (the working copy's tree over the build's source roots and the
# recomp identity), from which a promote takes the blessed revision. A tree
# without .git (a copy of a checkout) cannot measure those fields: the run
# reads them from the file OPENPETE_CAPTURE_FIELDS names (`key: value` lines,
# as `scripts/presentation-blessed.py capture-fields` writes them in the
# checkout), and a copy without that variable fails before capturing.
#
# A third leg, tests/harness/check_credits_interp.sh, runs after the corpus capture:
# sub-tick interpolation of the credits-roll letters (settled-line pose,
# fly-in position/depth/rotation, key stability), a case the corpus bk2s do
# not reach. It runs whether or not the diff passes, so one run reports every
# verdict. A filter that does not select it (above) records rc=64, reported
# as skipped. The leg is a capture: its exit code
# and output lines are recorded in credits-verdict.txt in the candidate
# directory (first line `rc=N`), and the verdict reads its result from that
# file, so --verdict-only decides a candidate captured elsewhere the same way.
# On a tier that runs the leg, an absent or unreadable file is a failure.
#
# The presentation tier's verdict also writes tile-digests.tsv (every
# captured tile's digest16, for the tile census, scripts/golden-census.py),
# and its capture legs run with OPENPETE_TILE_CENSUS, so every tile has its
# <tile>.census sidecar.
#
# Every verdict writes verdict-report.txt into the candidate directory: the
# verdict and exit code, the row census, the sha256 of the blessed.tsv and
# identity baseline read, the credits, identity and diff exit codes, the
# divergent, missing and extra tiles, the identity pairs over baseline
# (both members), every crashed capture leg (`crashed-leg: <leg> rc=N
# <kept dir>[ retry=pass|fail]`, from crashes.txt and crash-retries.txt) and
# the incomplete capture stages (`capture-incomplete: bk2 freecam`). The identity and diff output is kept beside
# it in verdict-ident.log and verdict-diff.log. A capture that ends
# incomplete also writes the report.
#
# Crashed legs. A capture leg that exits nonzero or writes no tile is kept,
# its log, argv, exit code and any hang or core dump, in a crash directory
# beside the candidate directory that no run deletes (default
# <candidate>.crashes/<time>-<pid>-<random>/<leg>/; PRESENTATION_GOLDEN_CRASH_DIR
# replaces <candidate>.crashes; scripts/presentation-game.sh, crashed legs).
# A leg whose process died of a signal is run once more (same section); its
# crashed-leg line ends ` retry=pass` or ` retry=fail`, the run prints
# `CRASHED <leg> rc=N retry=pass|fail`, and under OPENPETE_STREAM_FILE the
# stream carries the same line. A failed retry fails the verdict; a crash
# whose retry passed, or a leg not retried, does not change it.
#
# Stages. The bk2 corpus, the freecam corpus and the credits leg each run
# whatever an earlier one reports, so one failed leg costs its own tiles
# only. A capture stage that ends incomplete fails the verdict after the
# identity leg and the diff ran over everything captured (the MISSING
# lines of the diff and of the corpus scripts name every absent tile); the
# report's `capture-incomplete:` line names the stages, and the stream gets
# no `END`.
#
# PRESENTATION_TIER selects the corpus tier (scripts/presentation-game.sh):
# its manifest, golden, identity baseline and default candidate directory. A
# tier without a freecam manifest runs no freecam leg, and the credits leg
# belongs to the presentation tier only. tests/harness/check_dragon_golden.sh
# runs the `dragons` tier through this script.
#
# Every run prints its row counts: images this run captured, images imported
# from an interrupted attempt of the same run (and their rows), the tiles that
# name a blessed.tsv row, and the manifest's row count. A run that holds no
# image compared nothing, so it is refused as VACUOUS rather than reported as
# a pass; a run in which no tile names a row is refused by the diff.
#
# Stream. Under OPENPETE_STREAM_FILE (the golden fleet's supervisor sets it
# for a unit) every finished row appends its tiles' verdict lines to that
# file, the credits leg appends `CREDITS rc=N` and the capture's verdict
# appends `END` (scripts/corpus-resume.sh, Stream). The verdict is unchanged.
#
# Resume. Under OPENPETE_RESUME_RUN (the golden fleet sets it to its run id)
# and without --bless, the run does not clear the candidate directory: it
# imports the rows an interrupted capture of the same run, tree and binary
# finished (scripts/corpus-resume.sh: per-row completion stamps, every tile
# checked against its sha256), deletes everything else of that capture, and
# captures only the rows not imported. The credits leg is imported the same
# way when it passed. Imported tiles go through the same strict diff.
#
# Exit: 0 = matches golden, the identity leg is within baseline and the credits
#       leg passes; 1 = divergence, identity exceed, an incomplete capture
#       stage, a crashed leg whose retry failed, credits failure (or no
#       credits-verdict.txt on the presentation tier), a vacuous (zero-row)
#       run, a --verdict-only DIR that is absent or not mounted in the
#       image, a missing corpus manifest (either one; the message names the
#       path), or a blessed.tsv absent or with zero rows;
#       64 = SKIP (prereqs missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh
# shellcheck source=scripts/corpus-resume.sh
source scripts/corpus-resume.sh

# --materialize-ref [--game G] [--tier T] [--at REV] <tile>...: rebuild the
# blessed revision of the named rows in a throwaway worktree and re-capture
# their tiles (scripts/presentation-materialize.py). Manual; refused under ctest.
if [[ "${1:-}" == "--materialize-ref" ]]; then
  shift; exec python3 scripts/presentation-materialize.py "$@"
fi

FILTER=""           # bk2-name substring; default = the whole fanned-out corpus
FREECAM_FILTER=""   # level substring for the freecam corpus; default = all poses
BLESS=0
LIST=0              # --list: print the selected rows, capture nothing
REPEAT=""          # --repeat=N: repeat copies per selected row
BUILD_ARG=""
ENV_ACCEPT=""
VERDICT_ONLY=""     # candidate directory decided without a capture
KNOBS=()            # --env OPENPETE_NAME=VALUE: game knobs forwarded to every leg
next_verdict=0
pending=""          # a value option given as two words (--filter SUBSTR): the option awaiting its value
for a in "$@"; do
  if (( next_verdict )); then VERDICT_ONLY="$a"; next_verdict=0; continue; fi
  if [[ -n "$pending" ]]; then a="$pending=$a"; pending=""; fi
  case "$a" in
  --bless) BLESS=1;;
  --filter=*) FILTER="${a#--filter=}";;
  --freecam-filter=*) FREECAM_FILTER="${a#--freecam-filter=}";;
  --repeat=*) REPEAT="${a#--repeat=}";;
  --list) LIST=1;;
  --build-dir=*) BUILD_ARG="$a";;
  --environment-accept=*) ENV_ACCEPT="${a#--environment-accept=}";;
  --env=*) KNOBS+=("${a#--env=}");;
  --verdict-only) next_verdict=1;;
  --verdict-only=*) VERDICT_ONLY="${a#--verdict-only=}";;
  --game=*) ;;        # read by pg_parse_game below
  --filter|--freecam-filter|--repeat|--build-dir|--environment-accept|--game|--env) pending="$a";;
  *) echo "[presentation-golden] FAIL: unknown argument '$a' (an ignored --filter would capture the whole corpus)"; exit 64;;
esac; done
if [[ -n "$pending" ]]; then
  echo "[presentation-golden] FAIL: $pending takes a value"; exit 64
fi
if (( next_verdict )) || [[ "$*" == *--verdict-only* && -z "$VERDICT_ONLY" ]]; then
  echo "[presentation-golden] FAIL: --verdict-only takes a candidate directory"; exit 1
fi
if [[ -n "$VERDICT_ONLY" && ( $BLESS -eq 1 || $LIST -eq 1 || -n "$FILTER" || -n "$FREECAM_FILTER" || -n "$REPEAT" ) ]]; then
  echo "[presentation-golden] FAIL: --verdict-only takes no --bless, --list, --filter, --freecam-filter or --repeat"; exit 1
fi
if (( ${#KNOBS[@]} )); then
  why="$(pg_knob_check "${KNOBS[@]}")" || { echo "[presentation-golden] FAIL: $why"; exit 1; }
  if [[ $BLESS -eq 1 || -n "$VERDICT_ONLY" ]]; then
    echo "[presentation-golden] FAIL: --env takes no --bless or --verdict-only (a forwarded knob makes the run an A/B, never a golden)"; exit 1
  fi
fi
# Row selection per corpus leg. One filter selects in every leg; a
# --freecam-filter replaces it for the freecam leg. A leg the scoped run
# leaves without a filter (the bk2 and credits legs under --freecam-filter
# alone) runs none of its rows.
SCOPED=0; [[ -n "$FILTER" || -n "$FREECAM_FILTER" ]] && SCOPED=1
BK2_ON=1 CREDITS_ON=1
FC_SEL="${FREECAM_FILTER:-$FILTER}"
if (( SCOPED )); then
  [[ -n "$FILTER" ]] || BK2_ON=0
  [[ -n "$FILTER" && "credits-interp" == *"$FILTER"* ]] || CREDITS_ON=0
fi
if [[ -n "$REPEAT" ]]; then
  if ! [[ "$REPEAT" =~ ^[1-9][0-9]*$ ]]; then
    echo "[presentation-golden] FAIL: --repeat takes a positive integer (got '$REPEAT')"; exit 1; fi
  if [[ $BLESS -eq 1 || ( -z "$FILTER" && -z "$FREECAM_FILTER" ) ]]; then
    echo "[presentation-golden] FAIL: --repeat needs --filter or --freecam-filter and takes no --bless"; exit 1; fi
fi
export PRESENTATION_REPEAT="$REPEAT"
pg_repeat
GAME="$(pg_parse_game "$@")" || exit 64
BUILD_DIR="build/linux-release"
[[ -n "$BUILD_ARG" ]] && BUILD_DIR="${BUILD_ARG#--build-dir=}"
PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$GAME" || exit 64
# --list reads the manifests only, so it runs outside the capture image.
if (( LIST )); then
  if (( BK2_ON )); then
    PRESENTATION_LIST=1 tests/harness/presentation-corpus.sh default "build/presentation-list" "$FILTER" "--game=$GAME"
  else echo "[presentation-corpus] $GAME: no rows selected (--freecam-filter without --filter)"; fi
  if [[ -n "$PG_FREECAM_MANIFEST" ]]; then
    PRESENTATION_LIST=1 tests/harness/freecam-corpus.sh default "build/presentation-list" "$FC_SEL" "--game=$GAME"
  fi
  if [[ "$PG_TIER" == "presentation" ]]; then
    if (( CREDITS_ON )); then echo "[credits] select credits-interp"; else echo "[credits] not selected by the filter"; fi
  fi
  exit 0
fi
# Every OPENPETE_* variable is reported once, here on the host side; the
# in-image run reports only the knobs it forwards or drops.
if pg_in_capture_image; then
  pg_knob_screen --no-harness-rows presentation-golden "${KNOBS[@]}"
else
  pg_knob_screen presentation-golden "${KNOBS[@]}"
fi
# OPENPETE_CAPTURE_OUTSIDE_IMAGE=<reason>: run in place outside the image, for
# a harness test of this script's control flow on a machine without the
# image. Its verdict is not the golden's: --bless and --verdict-only are
# refused, the candidate directory records the reason (PG_OUTSIDE_IMAGE_FILE)
# and the report an `outside-image:` line, and a promote refuses it.
OUTSIDE_IMAGE=""
if [[ -n "${OPENPETE_CAPTURE_OUTSIDE_IMAGE:-}" ]] && ! pg_in_capture_image; then
  if [[ $BLESS -eq 1 || -n "$VERDICT_ONLY" ]]; then
    echo "[presentation-golden] FAIL: OPENPETE_CAPTURE_OUTSIDE_IMAGE takes no --bless or --verdict-only"; exit 1
  fi
  OUTSIDE_IMAGE="$OPENPETE_CAPTURE_OUTSIDE_IMAGE"
  echo "[presentation-golden] OUTSIDE THE CAPTURE IMAGE ($OUTSIDE_IMAGE): this verdict is not the golden's"
fi
if ! pg_in_capture_image && [[ -z "$OUTSIDE_IMAGE" ]]; then
  if [[ -n "$VERDICT_ONLY" ]]; then
    # The container sees this tree and the store's parent directory only.
    vdir="$(realpath -m -- "$VERDICT_ONLY")"
    store="${OPENPETE_GOLDEN_REF_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/openpete/golden-ref}"
    sparent="$(dirname "$(realpath -m -- "$store")")"
    if [[ "$vdir/" != "$PWD/"* && "$vdir/" != "$sparent/"* ]]; then
      echo "[presentation-golden] FAIL: --verdict-only $VERDICT_ONLY lies outside $PWD and $sparent, which the capture image mounts"
      exit 1
    fi
    pg_enter_capture_image --no-build tests/harness/check_presentation_golden.sh "$@"
  else
    pg_enter_capture_image tests/harness/check_presentation_golden.sh "$@"
  fi
  rc=$?
  if [[ $rc -eq 64 ]]; then
    echo "[presentation-golden] SKIP: no verdict outside the capture image; environment for information:"
    python3 scripts/presentation-blessed.py environment "$GAME" --tier "$PG_TIER" --build-dir "$BUILD_DIR"
  fi
  exit $rc
fi
# Hold the build directory against a build for the whole run (build_lock.sh).
# shellcheck source=scripts/lib/build_lock.sh
source scripts/lib/build_lock.sh
op_build_read_lock "$PG_BIN" presentation-golden || exit 75
GAME_ARG="--game=$GAME"
TIER_LABEL="" TIER_ENV=""
[[ "$PG_TIER" != "presentation" ]] && TIER_LABEL=" $PG_TIER" TIER_ENV="PRESENTATION_TIER=$PG_TIER "
pg_manifest_load "$PG_CORPUS_MANIFEST" pipe
[[ -n "$PG_FREECAM_MANIFEST" ]] && pg_manifest_load "$PG_FREECAM_MANIFEST" pipe
pg_manifest_dry_run_exit

# The candidate images OUTLIVE the run, in a stable per-build path. A capture is
# ~24 minutes; discarding it on exit meant that adjudicating a divergence - or
# promoting a single tile - cost a full re-capture, and the temp dir was already
# gone by the time the diff had been read. One fixed directory (rather than a
# fresh mktemp each run) keeps exactly the last run without accumulating.
# PRESENTATION_GOLDEN_OUT overrides; PRESENTATION_GOLDEN_KEEP=0 restores the old
# discard-on-exit behaviour.
# Per game and tier (PG_CAND_DIR), so a run for one game or tier cannot wipe
# another's kept candidates.
CREDITS_NAME="credits-verdict.txt"
REPORT_NAME="verdict-report.txt"

# ---- post-capture steps ------------------------------------------------------
#
# One implementation for a capturing run over its own OUT and --verdict-only
# over a candidate directory captured elsewhere: vr_census, then vr_verdict.

# Row census. `captured` counts the images in OUT; `compared` counts the
# tiles that name a row of blessed.tsv and `golden` the manifest's rows (the
# identity leg has no row, so it is excluded from both). A capture that
# produced nothing proves nothing about the renderer, so it is refused here;
# a capture no row matches is refused by the diff. Sets captured, compared,
# goldrows.
CAP_FAILED=""       # capture stages that ended incomplete, each after a space
captured=0 imported=0 imported_rows=0 compared=0 copies=0 goldrows=0 irc="not run" crc="not run" drc="not run"
vr_census() {
  local f census total=0
  for f in "$OUT"/*-vk.png; do [[ -e "$f" ]] && total=$((total+1)); done
  rs_census "$OUT"
  imported="$RS_IMPORTED_PNG" imported_rows="$RS_IMPORTED_ROWS"
  captured=$((total - imported))
  census="$(python3 scripts/presentation-blessed.py census "$GAME" --tier "$PG_TIER" --candidate "$OUT")" \
    || { echo "[presentation-golden] FAIL: $PG_BLESSED unreadable (see above)"; vr_finish 1; }
  goldrows="${census#rows=}"; goldrows="${goldrows%% *}"
  local matched="${census##*matched=}"; matched="${matched%% *}"
  copies="${census##*copies=}"
  # A repeat copy's tile is compared against its original's row.
  compared=$((matched + copies))
  echo "[presentation-golden] $GAME$TIER_LABEL rows: captured=$captured imported=$imported imported-rows=$imported_rows compared=$compared golden=$goldrows$( (( PG_REPEAT )) && echo " copies=$copies")"
  if [[ $total -eq 0 ]]; then
    echo "[presentation-golden] VACUOUS: $GAME captured 0 rows - nothing was rendered,"
    echo "  so nothing was verified. Populate $PG_CORPUS_MANIFEST"
    [[ -n "$PG_FREECAM_MANIFEST" ]] && echo "  (or $PG_FREECAM_MANIFEST)"
    echo "  and bless rows into $PG_BLESSED."
    vr_finish 1
  fi
  return 0
}

# Record the credits leg's exit code and output lines in OUT. args: rc log
vr_record_credits() {
  { echo "rc=$1"; cat "$2"; } >"$OUT/$CREDITS_NAME.tmp" && mv -f "$OUT/$CREDITS_NAME.tmp" "$OUT/$CREDITS_NAME"
}

# The credits leg's recorded result: crc = its exit code, 64 on a tier
# without the leg, 1 when the file is absent or its first line is not
# `rc=N`.
vr_read_credits() {
  crc=64
  [[ "$PG_TIER" == "presentation" ]] || return 0
  local first
  if [[ ! -f "$OUT/$CREDITS_NAME" ]]; then
    echo "[presentation-golden] FAIL: $OUT/$CREDITS_NAME absent; the credits leg's result was not recorded"
    crc=1; return 0
  fi
  first="$(head -n 1 "$OUT/$CREDITS_NAME")"
  if [[ ! "$first" =~ ^rc=([0-9]+)$ ]]; then
    echo "[presentation-golden] FAIL: $OUT/$CREDITS_NAME: first line '$first' is not rc=N"
    crc=1; return 0
  fi
  crc="${BASH_REMATCH[1]}"
  if [[ -n "$VERDICT_ONLY" ]]; then
    echo "[presentation-golden] credits leg recorded rc=$crc:"
    tail -n +2 "$OUT/$CREDITS_NAME" | sed 's/^/  /'
  fi
}

# Write OUT/verdict-report.txt and exit with rc. The lists are read from the
# identity and diff logs of this run.
vr_finish() {
  local rc="$1" verdict=FAIL
  [[ $rc -eq 0 ]] && verdict=PASS
  [[ $rc -eq 64 ]] && verdict=SKIP
  {
    echo "verdict: $verdict"
    echo "exit: $rc"
    echo "game: $GAME"
    echo "tier: $PG_TIER"
    echo "mode: $([[ -n "$VERDICT_ONLY" ]] && echo verdict-only || echo capture)"
    echo "candidate: $OUT"
    echo "captured: $captured"
    echo "imported: $imported"
    echo "imported-rows: $imported_rows"
    echo "compared: $compared"
    echo "manifest-rows: $goldrows"
    echo "blessed-sha256: $(sha256sum "$PG_BLESSED" 2>/dev/null | cut -d' ' -f1) $PG_BLESSED"
    echo "ident-baseline-sha256: $(sha256sum "$PG_IDENT_BASELINE" 2>/dev/null | cut -d' ' -f1) $PG_IDENT_BASELINE"
    echo "credits-rc: $crc"
    echo "identity-rc: $irc"
    echo "diff-rc: $drc"
    echo "forwarded-knobs: $(pg_knobs_recorded "$OUT")"
    [[ -s "$OUT/$PG_OUTSIDE_IMAGE_FILE" ]] && echo "outside-image: $(head -n 1 "$OUT/$PG_OUTSIDE_IMAGE_FILE")"
    echo "docs-pairs-rc: ${prc:-}"
    if (( PG_REPEAT )); then
      echo "repeat: $PG_REPEAT"
      [[ -f "$OUT/verdict-diff.log" ]] && awk '
        $1 == "REPEAT" { print "repeat-row: " $2 " " substr($0, index($0, $3)) }
        $1 == "[presentation-diff]" && $2 == "repeat" {
          for (i = 1; i <= NF; i++) {
            if ($(i + 1) == "divergent") d = $i
            if ($(i + 1) == "legs,") l = $i
            if ($(i + 1) == "compared") c = $i
          }
          print "repeat-divergent: " d; print "repeat-legs: " l; print "repeat-compared: " c }' "$OUT/verdict-diff.log"
    fi
    pg_crash_list "crashed-leg: " "$OUT"
    [[ -n "$CAP_FAILED" ]] && echo "capture-incomplete:$CAP_FAILED"
    [[ -f "$OUT/verdict-diff.log" ]] && awk '
      $1 == "DIVERGE" { print "divergent: " $2 " " substr($0, index($0, $3)) }
      $1 == "MISSING" { print "missing: " $2 }
      $1 == "EXTRA"   { print "extra: " $2 }' "$OUT/verdict-diff.log"
    [[ -f "$OUT/verdict-ident.log" ]] && awk '
      $1 == "EXCEED" || $1 == "NOCANON" {
        canon = $2; sub(/__ident__/, "__canon__", canon)
        print "identity-" tolower($1) ": " $2 " " canon " " $3 " " $4 }' "$OUT/verdict-ident.log"
  } >"$OUT/$REPORT_NAME"
  if [[ -s "$OUT/crashes.txt" ]]; then
    echo "[presentation-golden] $(wc -l <"$OUT/crashes.txt") crashed leg(s) kept:"
    pg_crash_list "  " "$OUT"
  fi
  echo "[presentation-golden] report: $OUT/$REPORT_NAME"
  exit "$rc"
}

# Identity leg, the credits leg's recorded result and the strict diff over
# OUT, then the verdict; exits. The identity leg and the diff both run
# whatever the other reports, so one run reports every verdict. Their
# previous outputs in OUT (heat diffs, logs, report) are deleted first. A
# capturing run under OPENPETE_STREAM_FILE whose every capture stage completed
# first appends `END` to the stream: the capture reached its verdict, so every
# row's lines are in the stream.
vr_verdict() {
  [[ -z "$VERDICT_ONLY" && -z "$CAP_FAILED" ]] && rs_stream_end
  rm -rf "$OUT/diff"
  rm -f "$OUT/verdict-ident.log" "$OUT/verdict-diff.log" "$OUT/$REPORT_NAME"
  rm -f "${OUT:?}/tile-digests.tsv"
  # Identity leg: the alpha-0 re-extract against the canon tile of the same
  # capture (no golden).
  python3 tests/harness/presentation-ident-check.py "$OUT" --game "$GAME" --baseline "$PG_IDENT_BASELINE" \
    | tee "$OUT/verdict-ident.log"
  irc=${PIPESTATUS[0]}
  # Every captured tile's digest: the fleet's sidecar pull checks each
  # captured tile has its sidecar against it, and the census store keeps it
  # (scripts/golden-census.py). A failure here fails the verdict.
  if [[ "$PG_TIER" == "presentation" ]]; then
    python3 scripts/golden-census.py digests "$OUT" \
      || { echo "[presentation-golden] FAIL: tile-digests.tsv not written"; vr_finish 1; }
  fi
  vr_read_credits
  # A filtered run captures part of the corpus on purpose, so the golden
  # tiles it did not capture are out of scope rather than missing. An
  # unfiltered run and --verdict-only keep the strict shape check: every
  # golden tile must be captured.
  local -a subset=() repeat=()
  [[ -n "$FILTER" || -n "$FREECAM_FILTER" ]] && subset=(--expect-subset)
  (( PG_REPEAT )) && repeat=(--repeat "$PG_REPEAT" --baseline "$PG_IDENT_BASELINE")
  python3 tests/harness/presentation-corpus-diff.py "$OUT" --game "$GAME" --manifest "$PG_BLESSED" --diff-out "$OUT/diff" "${subset[@]}" "${repeat[@]}" \
    | tee "$OUT/verdict-diff.log"
  drc=${PIPESTATUS[0]}
  if [[ $drc -eq 64 ]]; then echo "[presentation-golden] SKIP (diff prereqs missing)"; vr_finish 64; fi
  # A crashed leg whose retry failed and an incomplete capture stage are
  # reported first: the identity and docs failures that follow from the
  # absent tiles are their consequences.
  local retry_failed cap_fail=0; retry_failed="$(pg_retry_failed "$OUT" | tr '\n' ' ')"
  if [[ -n "$retry_failed" ]]; then
    echo "[presentation-golden] FAIL: crashed leg(s) whose retry failed: ${retry_failed% }"; cap_fail=1
  fi
  if [[ -n "$CAP_FAILED" ]]; then
    echo "[presentation-golden] FAIL: capture incomplete:$CAP_FAILED (the MISSING lines above name the absent tiles)"; cap_fail=1
  fi
  (( cap_fail && drc == 0 )) && vr_finish 1
  # Docs pairs: each before/after pair of docs: rows must differ in this
  # capture (65 = the game has no docs rows).
  rm -f "${OUT:?}/verdict-docs.log"
  if [[ "$PG_TIER" == "presentation" ]]; then
    python3 tests/harness/docs-shot-pairs.py check --game "$GAME" --dir "$OUT" "${subset[@]}" \
      | tee "$OUT/verdict-docs.log"
    prc=${PIPESTATUS[0]}
    [[ $prc -eq 65 ]] && prc=0
    if [[ $prc -ne 0 ]]; then
      echo "[presentation-golden] FAIL: a docs before/after pair is identical or missing (see docs-shot-pairs lines above)."
      [[ $drc -eq 0 ]] && vr_finish 1
    fi
  fi
  if [[ $crc -ne 0 && $crc -ne 64 ]]; then
    echo "[presentation-golden] FAIL: credits interpolation leg (see the credits-interp lines above)."
    [[ $drc -eq 0 ]] && vr_finish 1
  fi
  if [[ $irc -ne 0 && $irc -ne 64 ]]; then
    echo "[presentation-golden] FAIL: identity leg exceeds its baseline (see EXCEED lines above)."
    echo "  A raised count is a sub-tick spine regression. Re-baseline only for a"
    echo "  deliberate change: tests/harness/presentation-ident-check.py $OUT --bless --baseline $PG_IDENT_BASELINE"
    [[ $drc -eq 0 ]] && vr_finish 1
  fi
  local knobs; knobs="$(pg_knobs_recorded "$OUT")"
  [[ -n "$knobs" ]] && echo "[presentation-golden] A/B: captured with forwarded knobs $knobs; this verdict is not the stock configuration's and the candidate is not promotable"
  if [[ $drc -ne 0 ]]; then
    echo "[presentation-golden] FAIL: candidate diverged from golden."
    echo "  Candidate images kept in: $OUT"
    echo "  If this is an INTENDED visual change, adjudicate reference-first, then"
    echo "  promote from the images ALREADY CAPTURED above - no re-capture needed:"
    echo "    ${TIER_ENV}tests/harness/presentation-corpus-promote.sh $OUT --game $GAME <tile-or-substring>...   # selective"
    echo "    ${TIER_ENV}tests/harness/presentation-corpus-promote.sh $OUT --game $GAME --all                    # whole corpus"
    vr_finish 1
  fi
  [[ $crc -eq 64 && "$PG_TIER" == "presentation" ]] && echo "[presentation-golden] credits leg SKIPPED (see the credits-interp line above)"
  echo "[presentation-golden] PASS: $GAME$TIER_LABEL corpus matches golden ($compared rows compared)"
  vr_finish 0
}

if [[ -n "$VERDICT_ONLY" ]]; then
  OUT="$VERDICT_ONLY"
  if [[ ! -d "$OUT" ]]; then
    echo "[presentation-golden] FAIL: --verdict-only: no candidate directory $OUT"; exit 1
  fi
  vr_census
  python3 scripts/presentation-blessed.py environment "$GAME" --tier "$PG_TIER" --candidate "$OUT"
  vr_verdict
fi

OUT="${PRESENTATION_GOLDEN_OUT:-$PG_CAND_DIR}"
# A bless captures the whole corpus afresh: no stamp is read or written.
[[ $BLESS -eq 1 ]] && unset OPENPETE_RESUME_RUN
rs_init "$OUT" "$PG_BIN" default
if (( RS_ON )); then
  rs_import "$OUT" "$FILTER" "$FC_SEL" "$( (( BK2_ON )) || echo bk2) $( (( CREDITS_ON )) || echo credits)"
else
  rm -rf "$OUT"; mkdir -p "$OUT"
fi
if [[ "${PRESENTATION_GOLDEN_KEEP:-1}" == "0" ]]; then trap 'rm -rf "$OUT"' EXIT; fi
# Crashed capture legs are kept outside OUT (scripts/presentation-game.sh,
# crashed legs); OUT/crashes.txt names them and the report lists them.
pg_crash_init "$OUT"
FIELDS_ARG=()
if [[ -n "${OPENPETE_CAPTURE_FIELDS:-}" ]]; then
  FIELDS_ARG=(--fields "$OPENPETE_CAPTURE_FIELDS")
elif [[ ! -e .git ]]; then
  echo "[presentation-golden] FAIL: $PWD has no .git, so the capture record's tree fields cannot be measured here; set OPENPETE_CAPTURE_FIELDS to a file written by scripts/presentation-blessed.py capture-fields in the checkout"
  exit 1
fi
python3 scripts/presentation-blessed.py capture-record "$GAME" --tier "$PG_TIER" "$OUT" --build-dir "$BUILD_DIR" "${FIELDS_ARG[@]}" \
  || { echo "[presentation-golden] FAIL: capture record not written"; exit 1; }
rm -f "${OUT:?}/$PG_KNOBS_FILE" "${OUT:?}/$PG_OUTSIDE_IMAGE_FILE"
(( ${#KNOBS[@]} )) && printf '%s\n' "${KNOBS[@]}" >"$OUT/$PG_KNOBS_FILE"
[[ -n "$OUTSIDE_IMAGE" ]] && echo "$OUTSIDE_IMAGE" >"$OUT/$PG_OUTSIDE_IMAGE_FILE"

# Capture at the compiled DEFAULT depth encoding (now screen-affine), so the
# golden tracks whatever ships. The depth A/B is a separate manual tool
# (presentation-corpus.sh off = the legacy encoding via the kill switch).
# Both corpora write into the SAME dir so the diff covers their union: the bk2
# set (presentation-corpus.sh) + the debug-freecam poses (freecam-corpus.sh).
if (( BK2_ON )); then
  tests/harness/presentation-corpus.sh default "$OUT" "$FILTER" $BUILD_ARG "$GAME_ARG"
  rc=$?
else
  echo "[presentation-corpus] $GAME: no rows selected (--freecam-filter without --filter) - 0 rows"; rc=65
fi
if [[ $rc -eq 64 ]]; then echo "[presentation-golden] SKIP (capture prereqs missing)"; exit 64; fi
# 65 = the bk2 corpus had no rows for this game. Not a failure on its own: the
# freecam corpus may still carry rows, and the row count below decides.
# Any other failure is recorded and the run goes on to the freecam corpus and
# the credits leg; the verdict fails on it (vr_verdict).
if [[ $rc -ne 0 && $rc -ne 65 ]]; then
  echo "[presentation-golden] bk2 capture incomplete (rc=$rc); the freecam corpus and the credits leg still run"
  CAP_FAILED+=" bk2"
fi

# The freecam corpus is additive: a manifest with no rows returns 0 (nothing to
# capture), a missing Vulkan loader returns 64 (skip that corpus, keep the bk2
# result), and a missing capture or a missing manifest (1) fails the gate.
if [[ -n "$PG_FREECAM_MANIFEST" ]]; then
  tests/harness/freecam-corpus.sh default "$OUT" "$FC_SEL" $BUILD_ARG "$GAME_ARG"
  frc=$?
  if [[ $frc -eq 1 ]]; then
    echo "[presentation-golden] freecam capture incomplete; the credits leg still runs"
    CAP_FAILED+=" freecam"
  fi
fi

vr_census

# A refused promote stops the bless before the identity baseline is rewritten,
# so the baseline is never re-blessed against a manifest the promote did not
# write.
ACCEPT_ARG=()
[[ -n "$ENV_ACCEPT" ]] && ACCEPT_ARG=("--environment-accept=$ENV_ACCEPT")
python3 scripts/presentation-blessed.py environment "$GAME" --tier "$PG_TIER" --candidate "$OUT"
if [[ $BLESS -eq 1 && -n "$CAP_FAILED" ]]; then
  echo "[presentation-golden] FAIL: capture incomplete:$CAP_FAILED; nothing promoted"; vr_finish 1
fi
if [[ $BLESS -eq 1 ]]; then
  PRESENTATION_TIER="$PG_TIER" tests/harness/presentation-corpus-promote.sh "$OUT" --all "$GAME_ARG" "${ACCEPT_ARG[@]}" \
    || { echo "[presentation-golden] FAIL: promote refused; identity baseline left as is"; exit 1; }
  python3 tests/harness/presentation-ident-check.py "$OUT" --bless --game "$GAME" --baseline "$PG_IDENT_BASELINE" "${ACCEPT_ARG[@]}" || exit 1
  git add -- "$PG_IDENT_BASELINE"
  echo "[presentation-golden] blessed - review 'git diff --cached --stat' and commit"
  exit 0
fi

# Credits leg (no golden: the checks are relations between one run's own
# canonical and intermediate captures). Presentation tier only. Its result is
# recorded in OUT and read back by vr_verdict.
# A passed leg is stamped as the row `credits-interp`, so a resumed attempt
# imports its record instead of running it again.
if [[ "$PG_TIER" == "presentation" ]] && (( ! CREDITS_ON )); then
  echo "[presentation-golden] credits leg not selected by the filter"
  vr_record_credits 64 <(echo "[credits-interp] SKIP: not selected by the filter")
  rs_stream_credits 64
elif [[ "$PG_TIER" == "presentation" ]]; then
  if rs_imported credits credits; then
    echo "[presentation-golden] credits leg imported from the previous attempt ($(head -n 1 "$OUT/$CREDITS_NAME"))"
  else
    credits_log="$(mktemp)"
    PG_BUILD_DIR="$BUILD_DIR" tests/harness/check_credits_interp.sh "$GAME_ARG" | tee "$credits_log"
    credits_rc=${PIPESTATUS[0]}
    vr_record_credits "$credits_rc" "$credits_log"
    rs_stream_credits "$credits_rc"
    rm -f "$credits_log"
    [[ $credits_rc -eq 0 ]] && rs_files_done credits credits credits-interp "$CREDITS_NAME"
  fi
fi
vr_verdict
