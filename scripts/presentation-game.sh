#!/usr/bin/env bash
#
# presentation-game.sh - per-game paths for the presentation-golden scripts.
# SOURCED, not executed.
#
# The presentation gate and its capture, diff and promote scripts are one
# implementation across the three games; everything that differs between them
# is a path derived from the game slug here, so no script carries a second
# copy of the layout.
#
# Usage:
#   source scripts/presentation-game.sh
#   GAME="$(pg_parse_game "$@")" || exit 64     # --game SLUG / --game=SLUG
#   pg_resolve "$GAME"           || exit 64     # sets the PG_* variables
#
# pg_resolve sets:
#   PG_GAME              the slug
#   PG_BIN               the game's openpete binary (OPENPETE_BIN overrides)
#   PG_DISC              the game's disc image (OPENPETE_DISC overrides)
#   PG_RUN_ARGS          per-game boot flags the game needs (array)
#   PG_BK2_DIR           where the corpus bk2 movies live
#   PG_TIER              the corpus tier (PRESENTATION_TIER, default presentation)
#   PG_GOLDEN            the tier's golden directory (manifest and baseline)
#   PG_BLESSED           the tier's verdict manifest, $PG_GOLDEN/blessed.tsv
#   PG_IDENT_BASELINE    the identity leg's per-tile baseline
#   PG_CORPUS_DIR        the game's corpus manifest directory
#   PG_CORPUS_MANIFEST   the tier's row manifest (bk2 and pose rows)
#   PG_FREECAM_MANIFEST  the freecam pose manifest; empty for a tier without one
#   PG_CAND_DIR          the tier's default kept-candidate directory
#
# PG_BUILD_DIR defaults to build/linux-release and is honoured by pg_resolve,
# so a caller parsing --build-dir=DIR sets it before calling.

# Every game run started by a script that sources this file boots on scratch
# memory cards, never the player's.
# shellcheck source=scripts/lib/scratch_cards.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/scratch_cards.sh"
# A TERM, INT or HUP to the sourcing script stops every process it started;
# a game run started through op_own_run is stopped at once.
# shellcheck source=scripts/lib/own_children.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/own_children.sh"

PG_GAMES="spyro-1 spyro-2 spyro-3"

# Read the game slug out of an argument list. Both spellings are accepted;
# check_psx_oracle.sh uses the two-word form and the capture scripts take
# positional arguments, where an extra word would shift them.
pg_parse_game() {
  local a next=0 game="spyro-1"
  for a in "$@"; do
    if [[ "$a" == "--help" || "$a" == "-h" ]]; then
      # Every harness that shares this parser prints its usage header and
      # exits 64 instead of starting a run; the caller treats the failed
      # parse as exit 64.
      sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2
      return 1
    fi
    if   (( next ));                then game="$a"; next=0
    elif [[ "$a" == "--game" ]];    then next=1
    elif [[ "$a" == --game=* ]];    then game="${a#--game=}"
    fi
  done
  case " $PG_GAMES " in
    *" $game "*) ;;
    *) echo "unknown --game '$game' (expected one of: $PG_GAMES)" >&2; return 1;;
  esac
  echo "$game"
}

# A per-game executable (games/<game>/<name> in a static tree) is
# named after the slug with the separator dropped, matching the game's target
# name.
pg_bin_name() { echo "openpete-${1//-/}"; }

# pg_tree_bin BUILD GAME: the executable that runs GAME in the tree BUILD. A
# shared-linkage Linux tree builds one executable, `openpete` at its root,
# whose disc argument selects the game; a static tree builds the game's own
# executable. The configure deletes the one a tree no longer builds, so the
# file present decides; with neither built, the root name.
pg_tree_bin() {
  local own="$1/games/$2/$(pg_bin_name "$2")"
  if [[ ! -e "$1/openpete" && -e "$own" ]]; then echo "$own"; else echo "$1/openpete"; fi
}

pg_resolve() {
  PG_GAME="$1"
  local build="${PG_BUILD_DIR:-build/linux-release}"
  PG_BIN="${OPENPETE_BIN:-$(pg_tree_bin "$build" "$PG_GAME")}"

  # The disc path and the game's own boot flags already have one authority,
  # the oracle recipe. It is sourced in a subshell so its other variables
  # (GOLDEN, MANIFEST) cannot shadow the caller's.
  local conf="games/$PG_GAME/oracle.conf"
  local disc="" runargs=""
  if [[ -f "$conf" ]]; then
    disc="$(source "$conf" >/dev/null 2>&1; echo "$DISC")"
    runargs="$(source "$conf" >/dev/null 2>&1; echo "${RUN_ARGS[*]-}")"
  fi
  PG_DISC="${OPENPETE_DISC:-$disc}"
  PG_RUN_ARGS=()
  [[ -n "$runargs" ]] && read -ra PG_RUN_ARGS <<<"$runargs"

  PG_BK2_DIR="games/$PG_GAME/bk2"
  PG_CORPUS_DIR="$(pg_corpus_dir "$PG_GAME")"

  # A tier is a corpus with its own manifest, golden and candidate directory,
  # captured and diffed by the same scripts. `presentation` is the gate the
  # default chains run; `dragons` (Spyro 1's dragon-rescue cutscenes) runs on
  # its trigger policy through tests/harness/check_dragon_golden.sh and has no
  # freecam leg.
  PG_TIER="${PRESENTATION_TIER:-presentation}"
  case "$PG_TIER" in
    presentation)
      PG_GOLDEN="tests/golden/$PG_GAME/presentation"
      PG_CORPUS_MANIFEST="$PG_CORPUS_DIR/presentation-corpus.manifest"
      PG_FREECAM_MANIFEST="$PG_CORPUS_DIR/freecam-corpus.manifest"
      PG_CAND_DIR="build/presentation-candidate"
      [[ "$PG_GAME" != "spyro-1" ]] && PG_CAND_DIR="build/presentation-candidate-$PG_GAME"
      ;;
    dragons)
      PG_GOLDEN="tests/golden/$PG_GAME/dragons"
      PG_CORPUS_MANIFEST="$PG_CORPUS_DIR/dragon-rescue-corpus.manifest"
      PG_FREECAM_MANIFEST=""
      PG_CAND_DIR="build/dragon-candidate"
      [[ "$PG_GAME" != "spyro-1" ]] && PG_CAND_DIR="build/dragon-candidate-$PG_GAME"
      ;;
    *) echo "unknown PRESENTATION_TIER '$PG_TIER' (expected presentation or dragons)" >&2; return 1;;
  esac
  PG_BLESSED="$PG_GOLDEN/blessed.tsv"
  PG_IDENT_BASELINE="$PG_GOLDEN/ident-baseline.tsv"
  return 0
}

# ---- capture image ---------------------------------------------------------
#
# A presentation verdict is defined over the pinned capture image
# (tests/harness/linux-capture.Dockerfile), which carries its recipe at
# PG_IMAGE_RECIPE; pg_in_capture_image is true inside it.
#
# pg_enter_capture_image [--no-build|--no-engine] SCRIPT ARGS... runs SCRIPT
# with ARGS inside the image through `scripts/golden-capture-container.sh
# gate`, which builds the engine into PG_CONTAINER_BUILD_DIR with the image's
# toolchain under an exclusive lock, then runs SCRIPT there; a --build-dir=
# argument is replaced by PG_CONTAINER_BUILD_DIR, since the host's build tree
# holds host-compiled objects. --no-build enters through the wrapper's `run`
# instead, for a SCRIPT that runs no engine but reads the build directory's
# name (its preset); `run` builds the image when it is absent or stale rather
# than skipping. When OPENPETE_CAPTURE_BINARIES is set (a tree whose engine
# was built elsewhere, its binaries' hashes in that file), both forms enter
# through `gate --no-build`, which builds nothing and runs SCRIPT only after
# the shipped binaries match their hashes. --no-engine enters through `run`
# whatever OPENPETE_CAPTURE_BINARIES holds and passes ARGS unchanged, with no
# --build-dir= argument, for a SCRIPT that takes no build directory and runs
# no engine. The machine's
# hostname enters as OPENPETE_CAPTURE_HOST, since inside the container the
# hostname is the container id (scripts/presentation-blessed.py host_here).
# The caller stays the host-side parent for the whole run, so a process
# listing on the host shows the gate in flight. Returns the inner
# run's status; 64 (SKIP) when no container engine or no current image exists.
PG_IMAGE_RECIPE=/etc/openpete/capture.Dockerfile
# A gate run with OPENPETE_CAPTURE_OUTSIDE_IMAGE=<reason> (a harness test of
# the gate's own control flow) runs in place on this machine; its candidate
# directory holds this file with the reason, and a promote refuses it.
PG_OUTSIDE_IMAGE_FILE=outside-image.txt
PG_CONTAINER_BUILD_DIR=build/linux-release-container

pg_in_capture_image() { [[ -f "$PG_IMAGE_RECIPE" ]]; }

pg_enter_capture_image() {  # [--no-build|--no-engine] script args...
  if [[ "$1" == "--no-engine" ]]; then
    shift
    scripts/golden-capture-container.sh run -- \
      env "OPENPETE_CAPTURE_HOST=$(uname -n)" "$@"
    return
  fi
  local -a sub=(gate)
  [[ "$1" == "--no-build" ]] && { sub=(run); shift; }
  [[ -n "${OPENPETE_CAPTURE_BINARIES:-}" ]] && sub=(gate --no-build)
  local script="$1" a; shift
  local -a args=()
  for a in "$@"; do [[ "$a" == --build-dir=* ]] || args+=("$a"); done
  scripts/golden-capture-container.sh "${sub[@]}" -- \
    env "OPENPETE_CAPTURE_HOST=$(uname -n)" "$script" \
    "--build-dir=$PG_CONTAINER_BUILD_DIR" "${args[@]}"
}

# ---- game knobs --------------------------------------------------------------
#
# A gate verdict is about the stock configuration, so an OPENPETE_* variable
# in the gate's environment never reaches the game: outside the image the
# capture container passes only the harness variables in (scripts/
# golden-capture-container.sh), and inside it pg_knob_screen unsets every
# other one. A knob reaches every capture leg only when the run names it with
# `--env KEY=VALUE`; such a run is an A/B, its candidate directory holds
# forwarded-knobs.txt, and a bless or promote from it is refused.
#
# PG_HARNESS_VARS matches the OPENPETE_* names the harness scripts and the
# capture wrapper read or set themselves; they are not game knobs.
PG_HARNESS_VARS='^OPENPETE_(BIN|DISC|CAP_ASPECT|CAP_LODS|MAX_SAMPLES|FREECAM_START|MANIFEST_DRY_RUN|FRAME_LIMIT|CAPTURE_[A-Z0-9_]+|CONTAINER_ENGINE|RESUME_RUN|STREAM_FILE|STREAM_POKE|GOLDEN_REF_DIR|GOLDEN_LOCK_HELD|FLEET_ROOT|HARNESS_REAL_DISPLAY|NO_COMMITS|SANDBOX|SANDBOX_CACHE|SANDBOX_PARENT|CARD_DIR|VIBRATION|THREAD_ROLE_FATAL|TOOLS_DIR|RELEASE_ARCHIVE_DIR)$'
PG_KNOBS_FILE=forwarded-knobs.txt

# Check the --env KEY=VALUE arguments: KEY an OPENPETE_* name outside
# PG_HARNESS_VARS, each named once. Prints the reason and returns 1 on the
# first bad one. args: KEY=VALUE...
pg_knob_check() {
  local kv k; local -A seen=()
  for kv in "$@"; do
    k="${kv%%=*}"
    if [[ "$kv" != *=* || ! "$k" =~ ^OPENPETE_[A-Z0-9_]+$ ]]; then
      echo "--env takes OPENPETE_<NAME>=<value> (got '$kv')"; return 1; fi
    if [[ "$k" =~ $PG_HARNESS_VARS ]]; then
      echo "--env $k: a harness variable, not a game knob; set it in the environment"; return 1; fi
    [[ -z "${seen[$k]:-}" ]] || { echo "--env $k given twice"; return 1; }
    seen[$k]=1
  done
}

# Print each OPENPETE_* variable of this environment once, as harness,
# forwarded or dropped, then unset the dropped ones and export the forwarded
# ones, so every capture leg started after this inherits exactly the --env
# set. Exports PG_KNOBS_KEY, the sorted KEY=VALUE list (empty for a stock run),
# which the resume stamps carry. --no-harness-rows leaves the harness rows
# out, for the in-image run of a gate whose host side printed them.
# args: [--no-harness-rows] tag KEY=VALUE...
pg_knob_screen() {
  local hrows=1; [[ "$1" == --no-harness-rows ]] && { hrows=0; shift; }
  local tag="$1" kv k; shift
  local -A fwd=()
  for kv in "$@"; do fwd[${kv%%=*}]="${kv#*=}"; done
  while IFS= read -r k; do
    [[ -n "$k" ]] || continue
    if [[ -n "${fwd[$k]+x}" ]]; then continue
    elif [[ "$k" =~ $PG_HARNESS_VARS ]]; then (( hrows )) && echo "[$tag] env $k: harness"
    else echo "[$tag] env $k: dropped (a gate captures the stock configuration; --env $k=<value> forwards it)"
      unset "$k"
    fi
  done < <(compgen -e | grep '^OPENPETE_' | sort)
  for kv in "$@"; do
    echo "[$tag] env ${kv%%=*}: forwarded to every capture leg (--env), value '${kv#*=}'"
    export "${kv%%=*}=${kv#*=}"
  done
  export PG_KNOBS_KEY; PG_KNOBS_KEY="$(printf '%s\n' "$@" | sed '/^$/d' | sort | paste -sd, -)"
}

# Print the forwarded-knob list a candidate directory recorded (empty for a
# stock capture). args: dir
pg_knobs_recorded() {
  [[ -s "$1/$PG_KNOBS_FILE" ]] && paste -sd, "$1/$PG_KNOBS_FILE"
  return 0
}

# Append the engine's Vulkan device line (`[rhi/vk] device='<name>' ...
# driver-info='<info>'`) from a capture log to OUTDIR/capture-device.txt, from
# which scripts/presentation-blessed.py reads the Mesa and device strings a
# manifest header records. args: log outdir
pg_note_device() {
  pg_device_line "$1" >>"$2/capture-device.txt"
  return 0
}

# Print a capture log's first Vulkan device line (empty when it has none).
# args: log
pg_device_line() {
  grep -a -o "\[rhi/vk\] device='[^']*'.* driver-info='[^']*'" "$1" 2>/dev/null | head -1
  return 0
}

# Capture settings both corpora read from the environment, defaults in one
# place (a resumed attempt compares them against a row's completion stamp):
#   PG_CAP_ASPECT     OPENPETE_CAP_ASPECT, default 16:9 (the golden is widescreen)
#   PG_CAP_LODS       OPENPETE_CAP_LODS, default "original extended"
#   PG_MAX_SAMPLES    OPENPETE_MAX_SAMPLES, canonical samples per bk2, default 5
#   PG_FREECAM_START  OPENPETE_FREECAM_START, a freecam row's default canonical
#                     feed-key value: 1900 on Spyro 1 (a yield count), 2473 on
#                     Spyro 2 and 3 (a VBL count: the same present, 573 fast-boot
#                     VBLs later); read after pg_resolve, PG_GAME picks the default
pg_cap_settings() {
  PG_CAP_ASPECT="${OPENPETE_CAP_ASPECT:-16:9}"
  PG_CAP_LODS="${OPENPETE_CAP_LODS:-original extended}"
  PG_MAX_SAMPLES="${OPENPETE_MAX_SAMPLES:-5}"
  local fc_default=1900
  case "${PG_GAME:-}" in spyro-2|spyro-3) fc_default=2473;; esac
  PG_FREECAM_START="${OPENPETE_FREECAM_START:-$fc_default}"
}

# A freecam row's tile key: the row's level field plus the first six hex
# digits of the pose's md5, so two poses in one level never collide and the
# key survives re-ordering the manifest. A sub-area row's `:` is written `-`,
# so the key is a valid file name on Windows. args: level-field pose
pg_pose_slug() {
  local h; h="$(printf '%s' "$2" | md5sum | cut -c1-6)"
  echo "${1//:/-}-${h}"
}

# The row filter every corpus applies: a row is selected when the filter is
# empty or a substring of the row's name field or of its tile key (the bk2
# row's key is its name, a freecam row's is pg_pose_slug). One rule for
# every corpus, so one substring selects the same row wherever it lives.
# args: filter name [tile-key]
pg_row_selected() {
  [[ -z "$1" || "$2" == *"$1"* || ( -n "${3:-}" && "$3" == *"$1"* ) ]]
}

# ---- repeat copies ---------------------------------------------------------
#
# PRESENTATION_REPEAT=N (N >= 1) captures every selected row N times instead
# of once, the copies queued one after another through cj_run so CORPUS_JOBS
# of their legs run at once, and the original row not at all. Copy k writes
# its tiles under the key `<key>~r<k>` (k from 1, two digits at least), which
# scripts/presentation-blessed.py copy_origin maps back to the original
# tile: every copy is compared against the original row's blessed digest and
# identity baseline, so a race that moves a tile in some copies is counted
# per copy. Unset or empty is off.

# Set PG_REPEAT from PRESENTATION_REPEAT (0 = off); exit 64 on a value that
# is not a positive integer.
pg_repeat() {
  PG_REPEAT=0
  local v="${PRESENTATION_REPEAT:-}"
  [[ -z "$v" ]] && return 0
  if ! [[ "$v" =~ ^[1-9][0-9]*$ ]]; then
    echo "FAIL: PRESENTATION_REPEAT must be a positive integer (got '$v')" >&2; exit 64; fi
  PG_REPEAT="$v"
}

# The tile keys a row captures: the key itself without repeat, else its N
# copy keys. args: key
pg_copy_keys() {
  if (( ${PG_REPEAT:-0} == 0 )); then echo "$1"; return 0; fi
  local k
  for (( k = 1; k <= PG_REPEAT; k++ )); do printf '%s~r%02d\n' "$1" "$k"; done
}

# The skip arguments of a freecam row's level (the level field before any
# `@<variant>`): `<level>:<area>` with an integer area enters that sub-area
# (--skip-to-area), any other level the level itself (--skip-to-level).
# Result in the caller's PG_SKIP_ARGS. args: level
pg_skip_args() {
  if [[ "$1" =~ ^(.+):([0-9]+)$ ]]; then
    PG_SKIP_ARGS=(--skip-to-area "$1")
  else
    PG_SKIP_ARGS=(--skip-to-level "$1")
  fi
}

# ---- corpus manifests ------------------------------------------------------
#
# A corpus manifest lists the rows a harness runs. Each game's corpus
# manifests sit in games/<slug>/corpus/; games/<slug>/manifest/ holds the
# recompiler's state manifests, which no harness row set reads.
#
# Every harness loads its manifest through pg_manifest_load, so a manifest is
# found, counted and reported one way:
#   - a missing manifest is FAIL: the message names the path and the script
#     exits 1. A wrong path is a defect in the harness, never a skip.
#   - the row count under the manifest's row rule is printed as
#     `rows=N path=<manifest>` and left in PG_MANIFEST_ROWS. A manifest with
#     zero rows is not refused here; the harness keeps its own zero-row exit.
#   - OPENPETE_MANIFEST_DRY_RUN=1 makes pg_manifest_dry_run_exit exit 0, so a
#     harness run under it loads and reports its manifests and runs nothing.
#
# Row rules:
#   line  strip from the first `#`; a row is a line left with a non-blank
#         character.
#   pipe  split on `|`; a row is a line whose trimmed first field is non-empty
#         and does not start with `#`.

pg_corpus_dir() { echo "games/$1/corpus"; }            # args: slug
pg_corpus_manifest() { echo "games/$1/corpus/$2"; }    # args: slug file-name

# args: path form(line|pipe)
pg_manifest_load() {
  local path="$1" form="$2"
  if [[ ! -f "$path" ]]; then
    echo "FAIL: corpus manifest missing: $path" >&2
    exit 1
  fi
  case "$form" in
    line) PG_MANIFEST_ROWS="$(awk '{ sub(/#.*/, "") } /[^[:space:]]/ { n++ }
                                   END { print n + 0 }' "$path")";;
    pipe) PG_MANIFEST_ROWS="$(awk -F'|' '{ f = $1; gsub(/^[[:space:]]+|[[:space:]]+$/, "", f) }
                                         f != "" && f !~ /^#/ { n++ }
                                         END { print n + 0 }' "$path")";;
    *) echo "FAIL: pg_manifest_load: unknown row form '$form' for $path" >&2; exit 1;;
  esac
  echo "rows=$PG_MANIFEST_ROWS path=$path"
}

pg_manifest_dry_run_exit() {
  if [[ "${OPENPETE_MANIFEST_DRY_RUN:-0}" == "1" ]]; then exit 0; fi
  return 0
}

# One manifest, then the dry-run exit. args: path form(line|pipe)
pg_manifest() {
  pg_manifest_load "$1" "$2"
  pg_manifest_dry_run_exit
}

# ---- per-row launch field --------------------------------------------------
#
# A corpus row's optional `launch` field holds whitespace-separated tokens
# applied to every leg of that row and to no other row. A token spelled
# KEY=VALUE with an upper-case key is an environment entry; every other token
# is a command-line argument. A token spelled docs:<page>#<anchor>[/before|/after]
# is a documentation tag read by tests/harness/docs-shot-pairs.py and is passed
# to no run, so tagging a row leaves its capture unchanged. Results land in the
# caller's ROW_ENV and ROW_ARGS arrays, both emptied first so a row without the
# field adds nothing.
pg_split_launch() {  # launch-field
  ROW_ENV=(); ROW_ARGS=()
  local tok
  for tok in $1; do
    if [[ "$tok" == docs:* ]]; then continue; fi
    if [[ "$tok" == [A-Z_]*=* ]]; then ROW_ENV+=("$tok"); else ROW_ARGS+=("$tok"); fi
  done
}

# ---- crashed legs ----------------------------------------------------------
#
# A capture leg's log is the only record of why it failed, and a crash writes
# its evidence to that log: the crash handler prints the fault address, the
# host RIP, the guest pc/ra and the FIRST backtrace frames (the faulting
# function) before the last frames (the dispatcher, which is the same for
# every guest-side fault); a glibc abort prints only its one-line message.
# The candidate directory is cleared by the next run, so a crash that does
# not reproduce would lose its evidence there. A leg that exits nonzero or
# writes no tile is therefore kept in a crash directory outside the candidate
# directory, which no harness deletes:
#
#   ${PRESENTATION_GOLDEN_CRASH_DIR:-<outdir>.crashes}/<YYYYmmdd-HHMMSS>-<pid>-<random>/<leg>/
#     leg.log        the leg's stdout and stderr
#     argv.txt       the leg's command line, shell-quoted, env entries first
#     rc.txt         exit code, signal name, host, time, working directory,
#                    core_pattern, core size limit, moved core files
#     hang-trace.bin, hang-ram.bin   the hang watchdog's dumps, when it tripped
#     core*          core files written into the working directory during the
#                    leg (attributed by time: concurrent legs share the
#                    working directory)
#
# The per-run directory is created by the first crashed leg, so a run with no
# crashed leg creates nothing. PG_CRASH_LIST, the candidate directory's
# crashes.txt, lists the run's crashed legs (`<leg>\t<rc>\t<kept dir>`), from
# which the verdict report names each one.
#
# Retry. A leg whose process died of a signal (pg_rc_crashed) is run once more
# (pg_leg_retry), so a rare abort (glibc heap corruption detected on a
# llvmpipe worker thread, say) costs one leg instead of the leg's row. The
# crashed attempt is kept as above; its tiles are deleted and it is not
# recorded as done, so only the retry's tiles reach the verdict, the resume
# stamps and the stream. A retry that fails in any way is kept as
# <leg>.retry. PG_RETRY_LIST, crash-retries.txt beside crashes.txt, holds one
# row per retried leg (`<leg>\t<rc>\t<pass|fail>`): pass when the retry wrote
# every tile and exited 0, fail otherwise. The outcome is also appended to the
# kept rc.txt as `retry=`, printed as `  CRASHED <leg> rc=N retry=pass|fail`
# and, under OPENPETE_STREAM_FILE, streamed as
# `CRASHED <leg> rc=N retry=pass|fail <epoch>`. pg_crash_list names it on the
# leg's line; a fail fails the gate's verdict.
#
# Leg function contract: on attempt 1 (PG_LEG_TRY=1) an exit code
# pg_retry_owed accepts makes the function keep the leg, delete its outputs
# and return PG_LEG_RETRY_RC without recording the leg.

# Exit code a leg function returns in place of its own when the leg crashed
# and a retry is owed.
PG_LEG_RETRY_RC=3

# Set and export PG_CRASH_DIR, this run's crash directory, PG_CRASH_LIST,
# outdir/crashes.txt, and PG_RETRY_LIST, outdir/crash-retries.txt, unless an
# enclosing script set them; when this call sets them, also delete outdir's
# two lists and legacy *.crash.log copies of the previous run. A script run
# by an enclosing run shares that run's directory and lists. args: outdir
pg_crash_init() {
  [[ -n "${PG_CRASH_DIR:-}" ]] && return 0
  local root="${PRESENTATION_GOLDEN_CRASH_DIR:-${1%/}.crashes}"
  PG_CRASH_DIR="$(realpath -m -- "$root")/$(date +%Y%m%d-%H%M%S)-$$-$RANDOM"
  PG_CRASH_LIST="$(realpath -m -- "$1")/crashes.txt"
  PG_RETRY_LIST="${PG_CRASH_LIST%/*}/crash-retries.txt"
  export PG_CRASH_DIR PG_CRASH_LIST PG_RETRY_LIST
  [[ -d "$1" ]] || return 0
  rm -f -- "${1:?}/crashes.txt" "${1:?}/crash-retries.txt"
  find "${1:?}" -maxdepth 1 -type f -name '*.crash.log' -delete
}

# Make a leg's scratch directory and print its path. The leg logs to
# <dir>/leg.log with pg_leg_env's entries in its environment; the caller
# removes the directory after the leg. args: none
pg_leg_begin() {
  local d; d="$(mktemp -d -t openpete-leg.XXXXXX)" || return 1
  : >"$d/.start"
  echo "$d"
}

# Print the environment entries that put the hang watchdog's dumps in the
# leg's scratch directory instead of the shared logs state directory, and on
# the presentation tier OPENPETE_TILE_CENSUS (pg_census_init), which writes
# each tile's sidecar. args: legdir
pg_leg_env() {
  echo "OPENPETE_HANG_TRACE=$1/hang-trace.bin"
  echo "OPENPETE_HANG_RAM=$1/hang-ram.bin"
  (( ${PG_CENSUS_ON:-0} )) && echo "OPENPETE_TILE_CENSUS=$PG_CENSUS_TREE"
  return 0
}

# ---- tile census -------------------------------------------------------------
#
# Every leg of the presentation tier runs with OPENPETE_TILE_CENSUS, so the
# binary writes <tile base>.census beside each -vk PNG (scripts/golden-census.py,
# core/diag/tile_census.def). The variable's value is the capture record's
# source tree, which the sidecar's header names; without a record it is 1 and
# the header says `tree unrecorded`. A leg whose PNG exists without its
# sidecar fails (pg_census_check); the leg's sidecars are deleted before it
# runs. The dragons tier writes none.

# Set PG_CENSUS_ON and PG_CENSUS_TREE for an output directory. args: outdir
pg_census_init() {
  PG_CENSUS_ON=0 PG_CENSUS_TREE=1
  [[ "${PG_TIER:-presentation}" == presentation ]] || return 0
  PG_CENSUS_ON=1
  local t; t="$(sed -n 's/^source-tree=//p' "$1/capture-record.txt" 2>/dev/null | tail -1)"
  [[ "$t" =~ ^[0-9a-f]{40}$ ]] && PG_CENSUS_TREE="$t"
  return 0
}

# The sidecar of a -vk PNG name. args: png-name
pg_census_name() { echo "${1%.png}.census"; }

# Delete the sidecars of a leg's tiles before it runs. args: outdir png-name...
pg_census_clear() {
  (( ${PG_CENSUS_ON:-0} )) || return 0
  local d="$1" t; shift
  for t in "$@"; do rm -f -- "${d:?}/$(pg_census_name "$t")"; done
}

# Print `MISSING-CENSUS <sidecar>` per tile of a leg whose PNG exists and whose
# sidecar does not, with the binary's tile-census failure lines; return 1 when
# any is missing. args: outdir log png-name...
pg_census_check() {
  (( ${PG_CENSUS_ON:-0} )) || return 0
  local d="$1" log="$2" t miss=0; shift 2
  for t in "$@"; do
    [[ -f "$d/$t" && ! -f "$d/$(pg_census_name "$t")" ]] || continue
    echo "  MISSING-CENSUS $(pg_census_name "$t")"; miss=1
  done
  (( miss )) && grep -a 'tile-census' "$log" | grep -a 'FAIL' | head -4 | sed 's/^/    /'
  return $miss
}

# Append a leg's frozen-backdrop settle tally cost to <outdir>/census-settle.tsv
# (`kind<TAB>row key<TAB>lod-leg<TAB>settles<TAB>pass us total<TAB>pass us
# longest`), from the last `tile-census:` line of its log, which carries the
# process's running totals (census_shot_write); a leg that wrote no sidecar
# appends nothing. scripts/golden-fleet.py keeps the file in the census store
# entry and `golden-census.py settles` sums it. args: outdir kind key lod-leg log
pg_census_settle() {
  (( ${PG_CENSUS_ON:-0} )) || return 0
  grep -a 'tile-census: .*backdrop settles' "$5" 2>/dev/null | tail -1 | sed -n \
    's/.*backdrop settles \([0-9]*\), tally pass \([0-9.]*\) us total, \([0-9.]*\) us longest.*/\1\t\2\t\3/p' |
    awk -v k="$2" -v r="$3" -v l="$4" -F'\t' '{ printf "%s\t%s\t%s\t%s\t%s\t%s\n", k, r, l, $1, $2, $3 }' \
    >>"$1/census-settle.tsv"
  return 0
}

# Append a finished leg's capture seconds to <outdir>/row-cost.tsv
# (`kind<TAB>row key<TAB>lod-leg<TAB>seconds`), which the golden fleet merges
# into its per-row cost cache. args: outdir kind key lod-leg start-epoch
pg_row_cost() {
  local now="${EPOCHREALTIME:-$(date +%s.%N)}"
  awk -v k="$2" -v r="$3" -v l="$4" -v a="$5" -v b="$now"     'BEGIN { printf "%s\t%s\t%s\t%.1f\n", k, r, l, b - a }' >>"$1/row-cost.tsv"
}

# Succeed when a leg's exit code is a signal's (128 + N) other than a stop of
# the run (SIGHUP, SIGINT, SIGTERM: 129, 130, 143): an abort (134), a fault
# (135, 139), or SIGKILL (137: the OOM killer, or the leg timeout's -k after
# an ignored SIGTERM). args: rc
pg_rc_crashed() { (( $1 > 128 && $1 != 129 && $1 != 130 && $1 != 143 )); }

# Succeed when the leg running now is a first attempt under pg_leg_retry and
# its exit code is a crash. args: rc
pg_retry_owed() { (( ${PG_LEG_TRY:-0} == 1 )) && pg_rc_crashed "$1"; }

# Run a leg function under the retry rule (Retry above): once, and once more
# when it returns PG_LEG_RETRY_RC; then record the outcome. Returns the last
# attempt's status. args: leg-name function arg...
pg_leg_retry() {
  local leg="$1" r frc outcome=pass kept; shift
  PG_LEG_TRY=1 PG_LEG_CRASH_RC=""
  "$@"; r=$?
  if (( r != PG_LEG_RETRY_RC )); then PG_LEG_TRY=0; return "$r"; fi
  frc="$PG_LEG_CRASH_RC" kept="$PG_CRASH_DIR/$leg"
  echo "  retrying crashed leg $leg (rc=$frc) once"
  PG_LEG_TRY=2 PG_LEG_CRASH_RC=""
  "$@"; r=$?
  PG_LEG_TRY=0
  [[ $r -ne 0 || -n "$PG_LEG_CRASH_RC" ]] && outcome=fail
  [[ -d "$kept" ]] && echo "retry=$outcome" >>"$kept/rc.txt"
  printf '%s\t%s\t%s\n' "$leg" "$frc" "$outcome" >>"${PG_RETRY_LIST:-${PG_CRASH_LIST%/*}/crash-retries.txt}"
  echo "  CRASHED $leg rc=$frc retry=$outcome"
  [[ -z "${OPENPETE_STREAM_FILE:-}" ]] \
    || printf 'CRASHED %s rc=%s retry=%s %s\n' "$leg" "$frc" "$outcome" "$(date +%s.%3N)" >>"$OPENPETE_STREAM_FILE"
  return "$r"
}

# Keep a crashed leg: copy its scratch directory to PG_CRASH_DIR/<leg> (on a
# retry attempt <leg>.retry), write argv.txt and rc.txt, move core files
# written into the working directory since the leg started, append the leg to
# PG_CRASH_LIST and quote the identifying log lines into the corpus log. Sets
# PG_LEG_CRASH_RC, which pg_leg_retry reads. args: legdir leg-name rc argv...
pg_keep_crash_leg() {
  local legdir="$1" name="$2" rc="$3"; shift 3
  (( ${PG_LEG_TRY:-0} == 2 )) && name="$name.retry"
  PG_LEG_CRASH_RC="$rc"
  local kept="${PG_CRASH_DIR:?pg_crash_init not called}/$name" log="$legdir/leg.log" sig="" c
  mkdir -p "$kept"
  cp -a "$legdir/." "$kept/"
  rm -f -- "${kept:?}/.start"
  { printf '%q ' "$@"; echo; } >"$kept/argv.txt"
  (( rc > 128 )) && sig="$(kill -l "$((rc - 128))" 2>/dev/null)"
  (( rc == 124 )) && sig="timeout"
  {
    echo "rc=$rc"
    echo "signal=${sig:-none}"
    echo "host=${OPENPETE_CAPTURE_HOST:-$(uname -n)}"
    echo "time=$(date -Is)"
    echo "cwd=$PWD"
    echo "core_pattern=$(cat /proc/sys/kernel/core_pattern 2>/dev/null)"
    echo "core_ulimit=$(ulimit -c)"
  } >"$kept/rc.txt"
  while IFS= read -r c; do
    [[ -n "$c" ]] && mv -f -- "$c" "$kept/" && echo "core=${c##*/}" >>"$kept/rc.txt"
  done < <(find "$PWD" -maxdepth 1 -type f -name 'core*' -newer "$legdir/.start" 2>/dev/null)
  mkdir -p "$(dirname "$PG_CRASH_LIST")"
  printf '%s\t%s\t%s\n' "$name" "$rc" "$kept" >>"$PG_CRASH_LIST"
  echo "  rc=$rc${sig:+ ($sig)}, crashed leg kept: $kept"
  if grep -q '=== openpete crash:' "$log"; then
    echo "  --- crash handler ---"
    sed -n '/=== openpete crash:/,$p' "$log" \
      | grep -E '^=== openpete crash:|^  fault addr=|^  host RIP=|^  guest (pc|ra) =|^  MOD ATTRIBUTION:|^  host backtrace|^    #[0-2] ' \
      | sed 's/^/  /'
  else
    # No crash banner: an abort, a timeout, a refused launch or a clean exit
    # that wrote no tile. The tail is what says which.
    echo "  --- last log lines ---"
    grep -m1 'watchdog: hang detected' "$log" | sed 's/^/  /'
    tail -5 "$log" | sed 's/^/  /'
  fi
}

# Print one line per crashed leg listed in outdir/crashes.txt after the
# prefix: `<leg> rc=N <kept dir>`, then ` retry=pass|fail` for a retried leg
# (outdir/crash-retries.txt). args: prefix outdir
pg_crash_list() {
  [[ -s "$2/crashes.txt" ]] || return 0
  local name rc dir retry
  while IFS=$'\t' read -r name rc dir; do
    retry="$(awk -F'\t' -v l="$name" '$1 == l { print $3 }' "$2/crash-retries.txt" 2>/dev/null | tail -n 1)"
    echo "$1$name rc=$rc $dir${retry:+ retry=$retry}"
  done <"$2/crashes.txt"
}

# Print the legs of outdir/crash-retries.txt whose retry failed, one per line.
# args: outdir
pg_retry_failed() {
  [[ -s "$1/crash-retries.txt" ]] || return 0
  awk -F'\t' '$3 == "fail" { print $1 }' "$1/crash-retries.txt"
}
