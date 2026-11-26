#!/usr/bin/env bash
#
# presentation-corpus.sh - capture the native -vk presentation corpus.
#
# For every bk2 in the manifest (filtered), boot the game headless under
# software Vulkan (llvmpipe, deterministic) and capture two legs on the
# native-only path:
#   canonical   VBL-keyed frames                -> <name>__canon__<frame>-vk.png
#   intermediate deterministic alpha=0.5 frames  -> <name>__inter__<frame>-vk.png
#   identity     alpha=1 re-extract on the canon frames -> <name>__ident__<frame>-vk.png
#                (not a golden tile: presentation-ident-check.py diffs it against the
#                 canon tile of the same frame from the same capture)
# Both legs are gameplay-neutral display captures; the same-binary depth kill
# switch (OPENPETE_DEPTH_AFFINE) selects legacy vs candidate depth so golden and
# candidate differ ONLY by the change under test.
#
# Usage:
#   tests/harness/presentation-corpus.sh <off|on> <outdir> [bk2-filter] [--game SLUG]
#     off|on     OPENPETE_DEPTH_AFFINE for the run (default off = HEAD behavior)
#     outdir     where the -vk PNGs are written (created)
#     bk2-filter substring; only manifest rows whose name (the tile key)
#                contains it are run (default: all rows; pg_row_selected, the
#                rule every corpus applies)
#     --game     game slug (default spyro-1); selects binary, disc, bk2 dir
#                and manifest through scripts/presentation-game.sh
#
# Environment:
#   PRESENTATION_LIST         1: print `[presentation-corpus] select <key>` and
#                             the row's sample grid for each row the filter
#                             selects, and the reason for each selected row
#                             skipped, then capture nothing (no binary, disc
#                             or output directory needed)
#   PRESENTATION_REPEAT       N: capture each selected row N times as repeat
#                             copies (scripts/presentation-game.sh, repeat
#                             copies), the original row not at all
#   PRESENTATION_TIER         corpus tier (default presentation); selects the
#                             manifest through scripts/presentation-game.sh
#   PRESENTATION_LEG_TIMEOUT  seconds one capture process may run (default
#                             300); the binary's own --max-runtime-seconds is
#                             20 less. A tier whose rows run past the default
#                             (a row reached through the ending chain) raises it;
#                             a single row raises its own bound with the launch
#                             token PRESENTATION_LEG_TIMEOUT=N, which the harness
#                             consumes and does not pass to the binary.
#
# A row runs one in-tree mod with the launch token
# PRESENTATION_MOD=<id>[:<table>.<key>=<value>[,...]], which the harness
# consumes: each leg copies mods/<id> (without its .build/) into its own
# directory, writes enabled.txt and the given keys as the copy's config.toml,
# and passes --mods-dir and --mods <id>. A value is written as given (a TOML
# integer, float or boolean) unless it holds a character outside [0-9.+-],
# which is written as a quoted string.
#
# A row boots with a committed memory card in slot 1 with the launch token
# PRESENTATION_CARD1=<path>, which the harness consumes: each leg copies the
# card to its own directory and passes --card1 with the copy, so a write the
# run makes never reaches the committed image.
#
# On the presentation tier every leg runs with OPENPETE_TILE_CENSUS and must
# leave each tile's <tile base>.census sidecar (scripts/presentation-game.sh,
# tile census); a PNG without one is a missing capture. Each leg's capture
# seconds are appended to <outdir>/row-cost.tsv, and its frozen-backdrop
# settle tally cost to <outdir>/census-settle.tsv (pg_census_settle).
#
# Exit: 0 = all captures produced, 1 = a capture is missing (hard failure) or
#           the corpus manifest is missing (the message names the path),
#       64 = SKIP (binary / disc / Vulkan loader missing),
#       65 = no rows (the manifest holds none, or the filter matched none) -
#            the caller decides whether an empty corpus is a skip or a
#            vacuous run.
#
# Under OPENPETE_RESUME_RUN (scripts/corpus-resume.sh) a row whose completion
# stamp is in <outdir>/resume is not captured again, and every row this run
# finishes is stamped.
set -u

# Unattended launcher: silence controller vibration for every run this script
# starts. Vibration ships on by default, which is right for a player and wrong
# for a scripted run - no player uses the pad, and the buzzing shows
# up in no log, so it would never surface on its own. Set here rather than at
# each invocation because this script builds its argument list in more than one
# place, and a flag appended to the wrong one is a silent miss.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0
# A failed thread-role check (openpete_thread_roles.h) aborts the run, so a
# role or seal violation on a gated path fails the gate instead of a log line.
export OPENPETE_THREAD_ROLE_FATAL=1


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
FILTER="${3:-}"   # bk2-name substring; empty = the whole manifest (the fanned-out corpus)

# Cap the canonical samples per bk2 to this many evenly-spaced frames (0 = the
# full every-`interval` grid). Keeps the golden broad across bk2s without a dense
# per-bk2 grid; the intermediate leg mirrors the kept canonical frames.
MAX_SAMPLES="$PG_MAX_SAMPLES"

# Capture aspect. The golden is ALWAYS widescreen so it catches Hor+ artifacts
# the 4:3 view can't show; override for a 4:3 spot-check if ever needed.
CAP_ASPECT="$PG_CAP_ASPECT"

# LOD modes swept per bk2/leg - both by default (original = authentic PSX band,
# extended = --no-lod full detail). Override to one mode for a faster spot-check.
CAP_LODS="$PG_CAP_LODS"

LEG_TIMEOUT="${PRESENTATION_LEG_TIMEOUT:-300}"
if ! [[ "$LEG_TIMEOUT" =~ ^[0-9]+$ ]] || (( LEG_TIMEOUT <= 20 )); then
  echo "presentation-corpus: PRESENTATION_LEG_TIMEOUT must be an integer above 20 (got '$LEG_TIMEOUT')" >&2; exit 64; fi

# --build-dir=DIR override (ctest passes this); else the default release tree.
BUILD_DIR="build/linux-release"
for a in "$@"; do case "$a" in --build-dir=*) BUILD_DIR="${a#--build-dir=}";; esac; done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR" pg_resolve "$GAME" || exit 64
BIN="$PG_BIN"
# Hold the build directory against a build for the whole run (build_lock.sh).
# shellcheck source=scripts/lib/build_lock.sh
source scripts/lib/build_lock.sh
op_build_read_lock "$BIN" presentation-corpus || exit 75
DISC="$PG_DISC"
MANIFEST="$PG_CORPUS_MANIFEST"

if [[ -z "$OUTDIR" ]]; then echo "usage: $0 <off|on|default> <outdir> [bk2-filter]" >&2; exit 64; fi
# The golden is captured at `default` (the compiled OPENPETE_DEPTH_AFFINE default,
# now screen-affine), so it tracks whatever ships. off/on force the env for the
# manual depth A/B (off = legacy encoding, the kill switch).
case "$AFFINE" in
  off|0)        AFFINE_ENV=0;;
  on|1)         AFFINE_ENV=1;;
  default|def)  AFFINE_ENV=default;;
  *) echo "presentation-corpus: first arg must be off|on|default (got '$AFFINE')" >&2; exit 64;; esac

pg_manifest "$MANIFEST" pipe
LIST="${PRESENTATION_LIST:-0}"

if [[ "$LIST" != 1 ]]; then
if [[ ! -x "$BIN" ]]; then echo "[presentation-corpus] SKIP: no binary at $BIN"; exit 64; fi
if [[ ! -f "$DISC" ]]; then echo "[presentation-corpus] SKIP: no disc at $DISC"; exit 64; fi
if ! ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1'; then
  echo "[presentation-corpus] SKIP: no Vulkan loader (libvulkan.so.1)"; exit 64; fi

# Deterministic software raster: the golden digests pin one rasterizer.
# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env
# `default` leaves OPENPETE_DEPTH_AFFINE unset so the binary's compiled default
# applies (the golden basis); off/on force the A/B legs.
if [[ "$AFFINE_ENV" != "default" ]]; then export OPENPETE_DEPTH_AFFINE="$AFFINE_ENV"; else unset OPENPETE_DEPTH_AFFINE; fi

mkdir -p "$OUTDIR"
pg_crash_init "$OUTDIR"
rs_init "$OUTDIR" "$BIN" "$AFFINE_ENV"
pg_census_init "$OUTDIR"
echo "[presentation-corpus] game=$GAME bin=$BIN affine=$AFFINE_ENV aspect=$CAP_ASPECT filter='$FILTER' -> $OUTDIR"
fi

# The golden filename encodes the LOD mode: `original` (the authentic PSX band)
# stays UNTAGGED so its golden is unchanged, `extended` (--no-lod, full detail at
# any distance) gets a `lodext__` tag between the bk2 name and the leg. Extended
# LOD pulls in far terrain / moby tiers the original band culls - an
# independently-regressable surface the depth work is most likely to disturb.
lod_tag() { [[ "$1" == "extended" ]] && echo "lodext__" || echo ""; }

# Succeed when a leg's log records the mod loader loading mod <id> with its
# entry run ("mods: loaded <id> v<version> (<object>)"); a mod disabled for
# a missing SDK, a failed compile or a refused object logs no such line.
# args: log id
pg_mod_loaded() {
  grep -aE " mods: loaded $2 v[^ ]+ \(" "$1" | grep -qv 'disabled)\|entry deferred'
}

# Build a leg's mods directory for a PRESENTATION_MOD spec: a copy of
# mods/<id> without its compile cache, enabled.txt naming it, and config.toml
# holding the spec's keys grouped by table. args: dir spec
pg_leg_mod() {
  local dir="$1" spec="$2" id="${2%%:*}" kv k v tbl last=""
  [[ -f "mods/$id/mod.toml" ]] || { echo "  PRESENTATION_MOD: no mod mods/$id"; return 1; }
  mkdir -p "$dir" && cp -r "mods/$id" "$dir/$id" || return 1
  rm -rf -- "${dir:?}/$id/.build" "${dir:?}/$id/config.toml"
  echo "$id" >"$dir/enabled.txt"
  : >"$dir/$id/config.toml"
  [[ "$spec" == *:* ]] || return 0
  local -a kvs; IFS=',' read -ra kvs <<<"${spec#*:}"
  for kv in "${kvs[@]}"; do
    k="${kv%%=*}"; v="${kv#*=}"; tbl="${k%.*}"; k="${k##*.}"
    [[ "$v" =~ ^[0-9.+-]+$ || "$v" == true || "$v" == false ]] || v="\"$v\""
    [[ "$tbl" != "$last" ]] && { echo "[$tbl]" >>"$dir/$id/config.toml"; last="$tbl"; }
    echo "$k = $v" >>"$dir/$id/config.toml"
  done
}

# A row name is the bk2 basename optionally followed by `@<variant>`. The whole
# name is the file key, so the variant's tiles carry it and never collide with
# the plain row's; the bk2 is everything before the first `@`.
row_bk2() { echo "${1%%@*}"; }

# The tile key `key` is the row name, or a repeat copy's key (pg_copy_keys);
# the bk2 is the row name's.
run_one() {  # name key frame_limit leg flag targets lodmode freeze launch row-line
  local bk2name; bk2name="$(row_bk2 "$1")"
  local name="$2" flim="$3" leg="$4" flag="$5" targets="$6" lod="$7" freeze="$8" launch="${9:-}" row="${10:-}"
  local -a ROW_ENV=() ROW_ARGS=(); pg_split_launch "$launch"
  # A launch token PRESENTATION_LEG_TIMEOUT=N is the harness's, not the run's:
  # it raises this row's leg bound to N seconds (never lowers it) and is not
  # passed to the binary.
  local leg_timeout="$LEG_TIMEOUT" e mod_spec="" card_src=""; local -a run_env=()
  for e in "${ROW_ENV[@]}"; do
    if [[ "$e" =~ ^PRESENTATION_LEG_TIMEOUT=([0-9]+)$ ]]; then
      (( BASH_REMATCH[1] > leg_timeout )) && leg_timeout="${BASH_REMATCH[1]}"
    elif [[ "$e" == PRESENTATION_MOD=* ]]; then mod_spec="${e#PRESENTATION_MOD=}"
    elif [[ "$e" == PRESENTATION_CARD1=* ]]; then card_src="${e#PRESENTATION_CARD1=}"
    else run_env+=("$e"); fi
  done
  local tag; tag="$(lod_tag "$lod")"
  local out="$OUTDIR/${name}__${tag}${leg}__{n}.png"
  local legdir; legdir="$(pg_leg_begin)" || return 1
  local log="$legdir/leg.log"
  # A row argument may name a per-leg file as {legdir}/<file>: the leg's own
  # directory, deleted with it, so a file the run writes (a memory card the
  # movie saves to, provisioned formatted by the binary when absent) starts
  # absent on every leg and no two legs share it. A row giving --card1 runs
  # with that card in slot 1: the game's --no-card boot flag is dropped for
  # the leg, and the row says --no-card2 itself when slot 2 stays empty.
  local -a mod_args=()
  if [[ -n "$mod_spec" ]]; then
    pg_leg_mod "$legdir/mods" "$mod_spec" || { rm -rf -- "${legdir:?}"; return 1; }
    mod_args=(--mods-dir "$legdir/mods" --mods "${mod_spec%%:*}")
  fi
  local -a run_args=() row_args=(); local ra card1=0
  if [[ -n "$card_src" ]]; then
    cp -- "$card_src" "$legdir/card1.mcr" ||
      { echo "  PRESENTATION_CARD1: cannot copy $card_src"; rm -rf -- "${legdir:?}"; return 1; }
    row_args+=(--card1 "$legdir/card1.mcr"); card1=1
  fi
  for ra in "${ROW_ARGS[@]}"; do
    row_args+=("${ra//\{legdir\}/$legdir}"); [[ "$ra" == --card1 ]] && card1=1
  done
  for ra in "${PG_RUN_ARGS[@]}"; do
    (( card1 )) && [[ "$ra" == --no-card ]] && continue
    run_args+=("$ra")
  done
  # A row with no movie file is the plain boot: the title screen and the attract
  # demo, which need no input. Everything else about the row is the same.
  local bk2_arg=(); [[ -f "$PG_BK2_DIR/${bk2name}.bk2" ]] && bk2_arg=(--bk2 "$PG_BK2_DIR/${bk2name}.bk2")
  # The identity leg is the intermediate re-extract phased onto the newest tick's
  # stamp (alpha 1), which must reproduce the canonical dump of the same frame.
  # A row whose launch env names another phase for its intermediate leg keeps
  # alpha 1 on the identity leg: the leg's assignment goes after the row's.
  local -a leg_env=() ident_env=(); [[ "$leg" == ident ]] && ident_env=(OPENPETE_INTERMEDIATE_ALPHA=1)
  mapfile -t leg_env < <(pg_leg_env "$legdir")
  # Always capture at 16:9 so the golden catches widescreen (Hor+) artifacts -
  # the revealed side world and the reveal-edge seams a 4:3 dump can't see. The
  # headless native-only capture honors --aspect (openpete_native_widescreen_factor
  # surfaces the fixed aspect when there is no window) and renders the widened
  # view into a dedicated capture target, so both legs are true Hor+ (853x448).
  # --mute 1: harness runs still open the host audio device; keep them
  # silent (a fanned-out corpus otherwise plays dozens of instances).
  # --no-config: golden images are compared pixel-for-pixel, so the run has
  # to be independent of any openpete.toml on the machine capturing them.
  # The row's own launch env and arguments go last, on every leg of the row, so
  # a variant row differs from its plain twin by exactly that field.
  local -a pngs=() t; IFS=',' read -ra TS <<<"$targets"
  for t in "${TS[@]}"; do pngs+=("${name}__${tag}${leg}__${t}-vk.png"); done
  pg_census_clear "$OUTDIR" "${pngs[@]}"
  local t_start="${EPOCHREALTIME:-$(date +%s.%N)}"
  local -a cmd=(env "${leg_env[@]}" "${run_env[@]}" "${ident_env[@]}" timeout -k 10 "$leg_timeout" "$BIN" --headless --no-config --disc "$DISC" --disc-cache 0 --renderer vk
    --mute 1 --aspect "$CAP_ASPECT" --lod "$lod" "${run_args[@]}"
    "${mod_args[@]}" "${bk2_arg[@]}" --screenshot-native-only
    "$flag" "$targets" --screenshot-output "$out"
    --frame-limit "$flim" --max-runtime-seconds "$((leg_timeout - 20))" "${row_args[@]}")
  op_own_run "${cmd[@]}" >"$log" 2>&1
  local rc=$?
  pg_note_device "$log" "$OUTDIR"
  local miss=0
  pg_row_cost "$OUTDIR" bk2 "$name" "$lod-$leg" "$t_start"
  pg_census_settle "$OUTDIR" bk2 "$name" "$lod-$leg" "$log"
  # A row's mod must have loaded: without it the leg captures the unmodded
  # scene, which is a different picture, not this row. Its tiles are deleted
  # and the leg fails and is kept.
  if [[ -n "$mod_spec" ]] && ! pg_mod_loaded "$log" "${mod_spec%%:*}"; then
    echo "  VACUOUS ${name}__${tag}${leg}: PRESENTATION_MOD ${mod_spec%%:*} did not load (no 'mods: loaded ${mod_spec%%:*} v' line); its mods: lines:"
    grep -a ' mods: ' "$log" | head -8 | sed 's/^/    /'
    for t in "${TS[@]}"; do rm -f -- "$OUTDIR/${name}__${tag}${leg}__${t}-vk.png"; done
    miss=1
  fi
  # Assert every requested target produced a -vk PNG (fail loudly, never silently
  # capture a partial corpus that would read as "all clean" downstream).
  (( miss )) || for t in "${TS[@]}"; do
    if [[ ! -f "$OUTDIR/${name}__${tag}${leg}__${t}-vk.png" ]]; then
      echo "  MISSING ${name}__${tag}${leg}__${t}-vk.png (rc=$rc)"; miss=1; fi
  done
  (( miss )) || pg_census_check "$OUTDIR" "$log" "${pngs[@]}" || miss=1
  if [[ $rc -ne 0 || $miss -ne 0 ]]; then
    pg_keep_crash_leg "$legdir" "${name}__${tag}${leg}" "$rc" "${cmd[@]}"
  fi
  # A crashed first attempt is retried (scripts/presentation-game.sh,
  # crashed legs): its tiles and sidecars are deleted so a tile the retry
  # does not write is missing, and the leg is not recorded as done.
  if pg_retry_owed "$rc"; then
    pg_census_clear "$OUTDIR" "${pngs[@]}"
    for t in "${pngs[@]}"; do rm -f -- "${OUTDIR:?}/$t"; done
    rm -rf -- "${legdir:?}"
    return "$PG_LEG_RETRY_RC"
  fi
  if [[ $miss -eq 0 ]]; then
    local -a tiles=("${pngs[@]}")
    (( PG_CENSUS_ON )) && for t in "${pngs[@]}"; do tiles+=("$(pg_census_name "$t")"); done
    rs_leg_done bk2 "$name" "$row" "$lod-$leg" "$log" "${tiles[@]}"
  fi
  rm -rf -- "${legdir:?}"
  return $miss
}

# Authoritative movie length = the number of input-frame lines ('|'-prefixed) in
# the bk2's Input Log.txt - matches the binary's own bk2-end frame exactly. The
# frame-limit is one cycle past that (render the last input's result, then stop),
# and the sample grid runs up to it, so nothing is hardcoded to a bk2's length.
bk2_frames() {
  python3 - "$1" <<'PY'
import zipfile, sys
try:
    z = zipfile.ZipFile(sys.argv[1])
    print(sum(1 for l in z.read('Input Log.txt').decode('latin-1').splitlines()
              if l.startswith('|')))
except Exception:
    print(0)
PY
}

# Emit the sample grid for one bk2 as "<canon_csv>|<inter_csv>|<flim>". The base
# grid steps by `interval` from `start`, extended one point PAST the movie end so
# the final settled frame is captured. With MAX_SAMPLES>0 the canonical points are
# then subsampled to at most MAX_SAMPLES EVENLY-SPACED frames (first and last kept)
# - broad within-movie coverage without one dense golden per bk2. The intermediate
# leg is each kept canonical frame + interval/2 (the +half offset that lands the
# deterministic alpha=0.5 present between snapshot stamps).
#
# PINNED frames (the row's 5th field) are added on top of that grid and are never
# subsampled away. The evenly-spaced grid is derived from movie length, so it lands
# wherever the arithmetic puts it and routinely misses the frame a defect was actually
# observed at; a pin names that frame explicitly. A pin contributes BOTH legs at the
# SAME frame (canon@F and inter@F, not F+interval/2), because the point is to hold the
# exact frame someone looked at, on the canonical and the sub-tick surface alike.
# Pins are additive: the grid's own samples keep the numbers they already have, so
# adding a pin re-blesses nothing that already exists. args: start fc interval maxs pins
compute_grid() {
  python3 - "$1" "$2" "$3" "$4" "${5:-}" <<'PY'
import sys
start, fc, interval, maxs = (int(x) for x in sys.argv[1:5])
pins = sorted({int(x) for x in (sys.argv[5] if len(sys.argv) > 5 else "").split(",") if x.strip()})
end = start + interval * ((fc - start) // interval + 1)
grid = list(range(start, end + 1, interval))
if maxs > 0 and len(grid) > maxs:
    idx = sorted(set(round(i * (len(grid) - 1) / (maxs - 1)) for i in range(maxs)))
    grid = [grid[j] for j in idx]
canon = sorted(set(grid) | set(pins))
inter = sorted({p + interval // 2 for p in grid} | set(pins))
flim = max(canon[-1], inter[-1]) + 1
print(",".join(map(str, canon)) + "|" + ",".join(map(str, inter)) + "|" + str(flim))
PY
}

fail=0 ran=0 imported=0
while IFS= read -r row; do
  IFS='|' read -r name start interval freeze pins launch <<<"$row"
  name="$(echo "$name" | tr -d ' \t')"
  [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
  pg_row_selected "$FILTER" "$name" || continue
  mapfile -t row_keys < <(pg_copy_keys "$name")
  pending_keys=()
  for key in "${row_keys[@]}"; do
    if rs_imported bk2 "$key"; then
      imported=$((imported+1)); echo "[$key] imported from the previous attempt"
    else pending_keys+=("$key"); fi
  done
  (( ${#pending_keys[@]} )) || continue
  start="$(echo "$start" | tr -d ' \t')"
  interval="$(echo "$interval" | tr -d ' \t')"; interval="${interval:-100}"
  freeze="$(echo "$freeze" | tr -d ' \t')"   # 4th field: a label for freeze-reaching rows, no launch effect
  pins="$(echo "$pins" | tr -d ' \t')"       # 5th field: explicit frames, never subsampled
  # 6th field: per-row launch tokens (CLI args and KEY=VALUE env entries). Only
  # the surrounding whitespace is trimmed; the tokens keep their own spacing.
  launch="${launch:-}"; launch="$(echo "$launch" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
  bk2="$PG_BK2_DIR/$(row_bk2 "$name").bk2"
  pinned_len=0
  if [[ -f "$bk2" ]]; then
    fc="$(bk2_frames "$bk2")"
    # A row that starts at or past the movie end samples the game running on
    # without input (e.g. a scripted freecam engaged after the last input);
    # its length is its last pin, as for a movie-less row. The run is not
    # stopped at the movie end (no --bk2-stop-on-end), so it reaches the pin.
    if [[ -n "$pins" && "$fc" =~ ^[0-9]+$ && "$start" -ge "$fc" ]]; then
      fc="$(tr ',' '\n' <<<"$pins" | sort -n | tail -1)"; pinned_len=1
      echo "  [$name] starts past the movie end: length from the last pin ($fc)"
    fi
  else
    # No movie: the plain boot (title screen, attract demo). The row's length is
    # its last pin, so a movie-less row must pin every frame it wants.
    if [[ -z "$pins" ]]; then echo "  SKIP row $name: no bk2 and no pins"; continue; fi
    fc="$(tr ',' '\n' <<<"$pins" | sort -n | tail -1)"; pinned_len=1
    echo "  [$name] no bk2: plain boot, length from the last pin ($fc)"
  fi
  # A length from the movie must pass the start; a length from the last pin
  # may equal it (a row pinned at its own start captures that VBL).
  if ! [[ "$fc" =~ ^[0-9]+$ ]] || (( fc < start || (fc == start && ! pinned_len) )); then
    echo "  SKIP row $name: bad/short $( (( pinned_len )) && echo "last pin" || echo "bk2 length") ($fc) vs start ($start)"; continue; fi
  grid="$(compute_grid "$start" "$fc" "$interval" "$MAX_SAMPLES" "$pins")"
  canon="${grid%%|*}"; rest="${grid#*|}"; inter="${rest%%|*}"; flim="${rest##*|}"
  ncanon=$(awk -F, '{print NF}' <<<"$canon"); ninter=$(awk -F, '{print NF}' <<<"$inter")
  # The binary caps ONE --screenshot list at OPENPETE_MAX_SCREENSHOT_FRAMES (64);
  # MAX_SAMPLES keeps us well under that, but guard anyway (never silently truncate).
  if [[ "$ncanon" -gt 64 || "$ninter" -gt 64 ]]; then
    echo "  FAIL row $name: $ncanon canon / $ninter inter samples exceed the 64-frame"
    echo "        screenshot cap - lower OPENPETE_MAX_SAMPLES or raise this row's interval."
    fail=1; continue; fi
  for key in "${pending_keys[@]}"; do
    ran=$((ran+1))
    if [[ "$LIST" == 1 ]]; then echo "[presentation-corpus] select $key movie=$fc canon=[$canon] inter=[$inter]"; continue; fi
    echo "[$key] movie=$fc frame-limit=$flim  canon=[$canon]  inter=[$inter]  lods=[$CAP_LODS]${freeze:+  freeze}${pins:+  pins=$pins}${launch:+  launch=$launch}"
    # Sweep both LOD modes: original (untagged golden) + extended (lodext__ tag).
    for lod in $CAP_LODS; do
      tag="$(lod_tag "$lod")"
      cj_run pg_leg_retry "${key}__${tag}canon" run_one "$name" "$key" "$flim" canon --screenshot-on-frame "$canon" "$lod" "$freeze" "$launch" "$row"
      cj_run pg_leg_retry "${key}__${tag}inter" run_one "$name" "$key" "$flim" inter --screenshot-intermediate-on-frame "$inter" "$lod" "$freeze" "$launch" "$row"
      cj_run pg_leg_retry "${key}__${tag}ident" run_one "$name" "$key" "$flim" ident --screenshot-intermediate-on-frame "$canon" "$lod" "$freeze" "$launch" "$row"
    done
  done
done < "$MANIFEST"
cj_wait || fail=1

if [[ "$LIST" == 1 ]]; then
  echo "[presentation-corpus] $GAME: $ran row(s) selected by '$FILTER'"
  (( ran )) || exit 65; exit 0; fi
if [[ $fail -ne 0 ]]; then echo "[presentation-corpus] FAIL: missing captures"; exit 1; fi
if [[ $ran -eq 0 && $imported -eq 0 ]]; then
  echo "[presentation-corpus] $GAME: no manifest rows matched '$FILTER' - 0 rows"; exit 65; fi
echo "[presentation-corpus] OK: $(ls "$OUTDIR"/*-vk.png 2>/dev/null | wc -l) images across $ran bk2 captured, $imported imported"
exit 0
