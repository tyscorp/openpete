#!/usr/bin/env bash
#
# freecam-corpus.sh - capture the native -vk debug-freecam presentation corpus.
#
# A second corpus alongside the bk2 set (presentation-corpus.sh): a set of
# (level, freecam pose) tuples, each a place a rendering bug was once chased.
# For every manifest row, boot headless under software Vulkan, --skip-to-level
# into the level, jump the debug freecam to the pose (which freezes Spyro + the
# camera so the view holds still), and capture two legs on the native-only path:
#   canonical    a pinned VBL frame                  -> <slug>__<lod>canon__<f>-vk.png
#   intermediate deterministic alpha=0.5 at +50      -> <slug>__<lod>inter__<f>-vk.png
#                (fails the row unless the engine reports the leg bracketed)
#   identity     alpha=1 re-extract on the canon frame -> <slug>__<lod>ident__<f>-vk.png
#                (not a golden tile; presentation-ident-check.py diffs it against canon)
# at 16:9, at every LOD mode in CAP_LODS. The freecam freezes the camera but NOT
# world animation, so the canonical leg is pinned to a fixed frame (deterministic
# run-to-run); the intermediate leg then exercises the moving terrain / mobys at
# that pose (the toasty platform is a freecam case).
#
# A row is `<level>[:<area>][@<variant>] | <pose> | [start] | [launch]`. The
# level is everything before the first `@`; `<level>:<area>` with an integer
# area enters that sub-area (--skip-to-area) instead of the level's area 0. The
# whole field keys the tiles (a `:` written `-`), so a variant row never
# collides with a plain row at the same pose. `launch` holds tokens
# applied to every leg of that row only (KEY=VALUE with an upper-case key is an
# environment entry, anything else a command-line argument), so the row
# measures that mode's own pixels. Rows without the field launch as before.
#
# Shares the diff/promote/golden infrastructure with the bk2 corpus: it writes
# PNGs into the same output directory, so check_presentation_golden.sh runs both
# capture scripts into one dir and diffs the union.
#
# Usage:
#   tests/harness/freecam-corpus.sh <off|on> <outdir> [level-filter] [--game SLUG]
#     off|on        OPENPETE_DEPTH_AFFINE for the run (default off = HEAD behavior)
#     outdir        where the -vk PNGs are written (created)
#     level-filter  substring; only rows whose name field or tile key
#                   (`<level>-<pose md5 prefix>`) contains it are run
#                   (default: all), so a key selects one pose of a level
#                   (pg_row_selected, the rule every corpus applies)
#     --game        game slug (default spyro-1); selects binary, disc and
#                   manifest through scripts/presentation-game.sh
#
# Exit: 0 = all captures produced (or a manifest with no rows = nothing to
#           do),
#       1 = a capture is missing, an intermediate leg was unbracketed
#           (hard failure), or the manifest is missing (the message names
#           the path),
#       64 = SKIP (binary / disc / Vulkan loader missing).
#
# Under OPENPETE_RESUME_RUN (scripts/corpus-resume.sh) a row whose completion
# stamp is in <outdir>/resume is not captured again, and every row this run
# finishes is stamped.
#
# Environment:
#   PRESENTATION_LIST         1: print `[freecam-corpus] select <key>` for each
#                             row the filter selects and capture nothing (no
#                             binary, disc or output directory needed)
#   PRESENTATION_REPEAT       N: capture each selected row N times as repeat
#                             copies (scripts/presentation-game.sh, repeat
#                             copies), the original row not at all
#   PRESENTATION_LEG_TIMEOUT  seconds one capture process may run (default
#                             300); the binary's own --max-runtime-seconds is
#                             20 less, as in presentation-corpus.sh.
set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0


cd "$(dirname "$0")/../.."
# shellcheck source=scripts/corpus-jobs.sh
source scripts/corpus-jobs.sh
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh
# shellcheck source=scripts/corpus-resume.sh
source scripts/corpus-resume.sh
pg_cap_settings
pg_repeat

AFFINE="${1:-off}"
OUTDIR="${2:-}"
FILTER="${3:-}"

CAP_ASPECT="$PG_CAP_ASPECT"
CAP_LODS="$PG_CAP_LODS"
LEG_TIMEOUT="${PRESENTATION_LEG_TIMEOUT:-300}"
if ! [[ "$LEG_TIMEOUT" =~ ^[0-9]+$ ]] || (( LEG_TIMEOUT <= 20 )); then
  echo "freecam-corpus: PRESENTATION_LEG_TIMEOUT must be an integer above 20 (got '$LEG_TIMEOUT')" >&2; exit 64; fi

BUILD_DIR="build/linux-release"
for a in "$@"; do case "$a" in --build-dir=*) BUILD_DIR="${a#--build-dir=}";; esac; done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$GAME"
# Pinned post-load feed-key value for the canonical leg (+50 for the
# intermediate), per game (pg_cap_settings). --skip-to-level is playable well
# before it; a row may override.
pg_cap_settings
START_DEFAULT="$PG_FREECAM_START"
BIN="$PG_BIN"
DISC="$PG_DISC"
MANIFEST="$PG_FREECAM_MANIFEST"

if [[ -z "$OUTDIR" ]]; then echo "usage: $0 <off|on|default> <outdir> [level-filter]" >&2; exit 64; fi
case "$AFFINE" in
  off|0)        AFFINE_ENV=0;;
  on|1)         AFFINE_ENV=1;;
  default|def)  AFFINE_ENV=default;;
  *) echo "freecam-corpus: first arg must be off|on|default (got '$AFFINE')" >&2; exit 64;; esac

pg_manifest "$MANIFEST" pipe
LIST="${PRESENTATION_LIST:-0}"

if [[ "$LIST" != 1 ]]; then
if [[ ! -x "$BIN" ]]; then echo "[freecam-corpus] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]]; then echo "[freecam-corpus] SKIP: no disc at $DISC"; exit 64; fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "[freecam-corpus] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi

# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env
if [[ "$AFFINE_ENV" != "default" ]]; then export OPENPETE_DEPTH_AFFINE="$AFFINE_ENV"; else unset OPENPETE_DEPTH_AFFINE; fi

mkdir -p "$OUTDIR"
pg_crash_init "$OUTDIR"
rs_init "$OUTDIR" "$BIN" "$AFFINE_ENV"
pg_census_init "$OUTDIR"
echo "[freecam-corpus] game=$GAME bin=$BIN affine=$AFFINE_ENV aspect=$CAP_ASPECT lods=[$CAP_LODS] filter='$FILTER' -> $OUTDIR"
fi

lod_tag() { [[ "$1" == "extended" ]] && echo "lodext__" || echo ""; }

run_leg() {  # slug frame_limit leg flag target lodmode level pose launch row-line
  local slug="$1" flim="$2" leg="$3" flag="$4" target="$5" lod="$6" level="$7" pose="$8" launch="${9:-}" row="${10:-}"
  local -a ROW_ENV=() ROW_ARGS=() PG_SKIP_ARGS=(); pg_split_launch "$launch"; pg_skip_args "$level"
  local tag; tag="$(lod_tag "$lod")"
  local out="$OUTDIR/${slug}__${tag}${leg}__{n}.png"
  local legdir; legdir="$(pg_leg_begin)" || return 1
  local log="$legdir/leg.log"
  local -a leg_env=(); [[ "$leg" == ident ]] && leg_env=(OPENPETE_INTERMEDIATE_ALPHA=1)
  mapfile -t -O "${#leg_env[@]}" leg_env < <(pg_leg_env "$legdir")
  # --no-config: the tiles are compared pixel-for-pixel, so the run is
  # independent of any openpete.toml in the capturing machine's home.
  local png="${slug}__${tag}${leg}__${target}-vk.png"
  pg_census_clear "$OUTDIR" "$png"
  local t_start="${EPOCHREALTIME:-$(date +%s.%N)}"
  local -a cmd=(env "${leg_env[@]}" "${ROW_ENV[@]}" timeout -k 10 "$LEG_TIMEOUT" "$BIN" --headless --no-config --disc "$DISC" --disc-cache 0 --renderer vk
    --mute 1 --aspect "$CAP_ASPECT" --lod "$lod" "${PG_RUN_ARGS[@]}"
    "${PG_SKIP_ARGS[@]}" --freecam-pose "$pose"
    --screenshot-native-only "$flag" "$target" --screenshot-output "$out"
    --frame-limit "$flim" --max-runtime-seconds "$((LEG_TIMEOUT - 20))" "${ROW_ARGS[@]}")
  op_own_run "${cmd[@]}" >"$log" 2>&1
  local rc=$? miss=0
  pg_row_cost "$OUTDIR" freecam "$slug" "$lod-$leg" "$t_start"
  pg_census_settle "$OUTDIR" freecam "$slug" "$lod-$leg" "$log"
  pg_note_device "$log" "$OUTDIR"
  if [[ ! -f "$OUTDIR/${slug}__${tag}${leg}__${target}-vk.png" ]]; then
    echo "  MISSING ${slug}__${tag}${leg}__${target}-vk.png (rc=$rc)"; miss=1
  fi
  (( miss )) || pg_census_check "$OUTDIR" "$log" "$png" || miss=1
  # A leg that wrote its tile and then exited nonzero (an abort at shutdown)
  # keeps the tile and its verdict, and is kept as a crashed leg.
  if [[ $rc -ne 0 || $miss -ne 0 ]]; then
    pg_keep_crash_leg "$legdir" "${slug}__${tag}${leg}" "$rc" "${cmd[@]}"
  fi
  # A crashed first attempt is retried (scripts/presentation-game.sh,
  # crashed legs): its tile and sidecar are deleted and the leg is not
  # recorded as done.
  if pg_retry_owed "$rc"; then
    pg_census_clear "$OUTDIR" "$png"
    rm -f -- "${OUTDIR:?}/$png"
    rm -rf -- "${legdir:?}"
    return "$PG_LEG_RETRY_RC"
  fi
  if [[ $miss -ne 0 ]]; then rm -rf -- "${legdir:?}"; return 1; fi
  # The intermediate leg is only evidence when the sub-tick re-extract actually
  # installed an interpolation bracket. Unbracketed, the re-extract composes the
  # canonical pose and writes it under the sub-tick filename, so the image is a
  # copy of the canonical leg and every diff over it is vacuous. The engine's
  # exit summary counts the legs it bracketed; an absent summary (a run killed
  # before shutdown) proves nothing either, and is refused the same way.
  if [[ "$leg" == inter ]]; then
    local sum; sum="$(grep -o 'intermediate screenshot leg(s): [0-9]* bracketed, [0-9]* unbracketed' "$log" | tail -1)"
    local unbr="${sum##*, }"; unbr="${unbr%% *}"
    if [[ -z "$sum" || "$unbr" != 0 ]]; then
      echo "  VACUOUS ${slug}__${tag}${leg}__${target}-vk.png: no bracketed sub-tick re-extract (${sum:-no exit summary})"
      rm -rf -- "${legdir:?}"; return 1
    fi
  fi
  local -a done_files=("$png"); (( PG_CENSUS_ON )) && done_files+=("$(pg_census_name "$png")")
  rs_leg_done freecam "$slug" "$row" "$lod-$leg" "$log" "${done_files[@]}"
  rm -rf -- "${legdir:?}"; return 0
}

fail=0 ran=0 imported=0
while IFS= read -r row; do
  IFS='|' read -r name pose start launch <<<"$row"
  name="$(echo "$name" | tr -d ' \t')"
  [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
  level="${name%%@*}"
  # Trim surrounding whitespace from the pose but keep its internal spaces.
  pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
  start="$(echo "$start" | tr -d ' \t')"; start="${start:-$START_DEFAULT}"
  launch="$(echo "${launch:-}" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
  [[ -z "$pose" ]] && { echo "  SKIP row: no pose for level '$level'"; continue; }
  row_key="$(pg_pose_slug "$name" "$pose")"
  pg_row_selected "$FILTER" "$name" "$row_key" || continue
  local_istart=$((start + 50))
  flim=$((local_istart + 51))
  mapfile -t row_keys < <(pg_copy_keys "$row_key")
  for slug in "${row_keys[@]}"; do
    if [[ "$LIST" == 1 ]]; then ran=$((ran+1)); echo "[freecam-corpus] select $slug level=$level start=$start"; continue; fi
    if rs_imported freecam "$slug"; then
      imported=$((imported+1)); echo "[$slug] imported from the previous attempt"; continue; fi
    ran=$((ran+1))
    echo "[$slug] level=$level start=$start pose='$pose'${launch:+  launch=$launch}"
    for lod in $CAP_LODS; do
      tag="$(lod_tag "$lod")"
      cj_run pg_leg_retry "${slug}__${tag}canon" run_leg "$slug" "$flim" canon --screenshot-on-frame "$start" "$lod" "$level" "$pose" "$launch" "$row"
      cj_run pg_leg_retry "${slug}__${tag}inter" run_leg "$slug" "$flim" inter --screenshot-intermediate-on-frame "$local_istart" "$lod" "$level" "$pose" "$launch" "$row"
      cj_run pg_leg_retry "${slug}__${tag}ident" run_leg "$slug" "$flim" ident --screenshot-intermediate-on-frame "$start" "$lod" "$level" "$pose" "$launch" "$row"
    done
  done
done < "$MANIFEST"
cj_wait || fail=1

if [[ $ran -eq 0 && $imported -eq 0 ]]; then echo "[freecam-corpus] $GAME: no rows matched '$FILTER' - 0 rows"; exit 0; fi
if [[ "$LIST" == 1 ]]; then echo "[freecam-corpus] $GAME: $ran row(s) selected by '$FILTER'"; exit 0; fi
if [[ $fail -ne 0 ]]; then echo "[freecam-corpus] FAIL: missing captures"; exit 1; fi
echo "[freecam-corpus] OK: $(ls "$OUTDIR"/*-vk.png 2>/dev/null | wc -l) images in $OUTDIR across $ran pose(s) captured, $imported imported"
exit 0
