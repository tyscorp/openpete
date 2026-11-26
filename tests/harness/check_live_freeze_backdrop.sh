#!/usr/bin/env bash
# check_live_freeze_backdrop.sh - the live freeze backdrop is the image
# of the last world present displayed, at every tick rate.
#
# Pause, the inventory and the level-exit screens draw the frozen world
# behind their panels from one backdrop image. The live present keeps the
# ring slot of the last world present it displayed (its re-extracted scene,
# alpha, compose inputs and retained-terrain state) and renders it into
# frozen_tex at the first present that is not a source, so the paused image
# is the last image the player saw, without the mod HUD or the chrome.
# Under eager presentation that present is usually a sub-tick re-extract,
# so the backdrop differs from the tick's own frame, which a headless
# capture renders (the canonical pin; the headless golden is unchanged).
#
# Input: games/<slug>/corpus/live-freeze-backdrop-corpus.manifest, rows
#   <bk2> | <frame-limit> | <dump VBL> | <pin VBL> | <launch tokens>
# The movie replays to the frame limit, past the VBL the freeze starts on.
# The dump VBL is a paused VBL: each live leg dumps frozen_tex at its first
# present at or after it (OPENPETE_PT_DUMP_FROZEN), and the headless leg
# captures it, rendering the canonical pin, extracted at the pin VBL, as
# bd-shot-f<pin>.png. Under OPENPETE_FREEZE_BD_DUMP each live leg copies
# every world present's scene-only image (after the scene draw, before the
# SCENE postfx point, the mod HUD and the chrome) and, at each backdrop
# render, writes the copy of the kept slot's present as bd-ref-f<VBL>.png and
# the render as bd-live-f<VBL>.png, VBL being that scene's extract VBL; the
# reference of a leg is its newest bd-ref at or before the dump VBL.
# Launch tokens follow the presentation corpus convention (KEY=VALUE with an
# upper-case key is an environment entry).
#
# Legs per row: one headless capture; one live leg per tick scale (default
# 0.25 0.5 1, --scales "A B ..."), Xvfb at 640x448 (the reference size, so
# the window picture and the backdrop share an extent), eager presentation
# (--render-fps 120 --vsync 0); and the control, the live leg at the first
# scale under OPENPETE_FREEZE_BD=0, which copies the finished picture of
# every world present (mod HUD included) into the backdrop.
#
# Pass criterion per live leg: the frozen_tex dump at the dump VBL equals
# the leg's reference, 0 differing pixels, and bd-live of the same VBL
# equals it too. Engagement (refused as VACUOUS, never PASS): each live leg
# logged `eager: engaged`, presented at least one intermediate scene, wrote
# a reference, and its `[freeze-bd]` line counts rendered >= 1 and
# displayed >= 1 (a render of a kept slot, not the canonical fallback); the
# row needs at least one live leg whose reference differs from bd-shot-f<pin>
# (tickn>0), or the reference cannot tell the displayed scene from the
# tick's own frame. The control reports its frozen dump against its own
# reference (diff) for the record; it has no pass condition beyond
# producing both images (--no-control skips it).
#
# Verdict line per leg:
#   [live-freeze-bd] game=<g> bk2=<b> leg=<scale|control> ref=f<VBL>
#                    diff=<px> render_diff=<px> tickn=<px> rendered=<n>
#                    displayed=<n> verdict=...
#
# Usage:
#   tests/harness/check_live_freeze_backdrop.sh [--game SLUG]
#       [--build-dir=DIR] [--filter=SUBSTR] [--out=DIR] [--scales "0.25 0.5 1"]
#       [--no-control]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing manifest, 64 SKIP (no
# xvfb-run, binary, disc or bk2).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh
# shellcheck source=scripts/lib/sandbox.sh
source scripts/lib/sandbox.sh

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

FILTER=""; OUT=""; SCALES="0.25 0.5 1"; CONTROL=1
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
        --out=*)       OUT="${a#--out=}";;
        --scales=*)    SCALES="${a#--scales=}";;
        --no-control)  CONTROL=0;;
    esac
    case "$prev" in
        --filter) FILTER="$a";;
        --out)    OUT="$a";;
        --scales) SCALES="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

WIDTH=640
HEIGHT=448
LEG_RUNTIME=840
LEG_TIMEOUT=900

MANIFEST="$(pg_corpus_manifest "$GAME" live-freeze-backdrop-corpus.manifest)"
pg_manifest "$MANIFEST" pipe
op_xvfb_prefix live-freeze-bd "${WIDTH}x${HEIGHT}"
[[ -x "$PG_BIN" ]] || { echo "live-freeze-bd: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "live-freeze-bd: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/live-freeze-backdrop/$GAME"
mkdir -p "$OUT"

# Differing pixels between two PNGs, or "shape" / "missing".
px_diff() {
    python3 - "$1" "$2" <<'PY'
import os, sys
import numpy as np
from PIL import Image
a, b = sys.argv[1], sys.argv[2]
if not (os.path.isfile(a) and os.path.isfile(b)):
    print("missing"); sys.exit(0)
A = np.asarray(Image.open(a).convert("RGB")); B = np.asarray(Image.open(b).convert("RGB"))
print("shape" if A.shape != B.shape else int((A != B).any(axis=2).sum()))
PY
}

# One leg. The leg's directory and log are deleted before the run and the
# log asserted after it, so no output of an earlier run is read as this
# one's. stdin is closed: the row loop reads the manifest on its own stdin.
run_leg() {  # dir movie flim dump mode(headless|live) scale bd(1|0)
    local dir="$1" movie="$2" flim="$3" dump="$4" mode="$5" scale="$6" bd="$7"
    rm -rf "${dir:?}"; mkdir -p "$dir"
    local -a env=(OPENPETE_VIBRATION=0 OPENPETE_FREEZE_BD_DUMP="$dir" "${ROW_ENV[@]}")
    local -a args=(--no-config --disc "$PG_DISC" --disc-cache 0 --mute 1 --vibration 0
                   --renderer vk "${PG_RUN_ARGS[@]}" --bk2 "$movie"
                   --frame-limit "$flim" --max-runtime-seconds "$LEG_RUNTIME" "${ROW_ARGS[@]}")
    if [[ "$mode" == headless ]]; then
        op_own_run env "${env[@]}" timeout -k 10 "$LEG_TIMEOUT" "$PG_BIN" --headless \
            "${args[@]}" --window "${WIDTH}x${HEIGHT}" --screenshot-native-only \
            --screenshot-on-frame "$dump" --screenshot-output "$dir/shot__{n}.png" \
            </dev/null >"$dir/run.log" 2>&1
    else
        env+=(OPENPETE_FREEZE_BD="$bd" OPENPETE_PT_DUMP="$dump" OPENPETE_PT_DUMP_COUNT=1
              OPENPETE_PT_DUMP_FROZEN=1 OPENPETE_PT_DUMP_PATH="$dir")
        op_own_run env "${env[@]}" "${OP_XVFB[@]}" timeout -k 10 "$LEG_TIMEOUT" "$PG_BIN" \
            "${args[@]}" --input-lock --background 1 --window "${WIDTH}x${HEIGHT}" \
            --vsync 0 --render-fps 120 --tick-scale "$scale" --perf-stats off --osd off \
            </dev/null >"$dir/run.log" 2>&1
    fi
    [[ -s "$dir/run.log" ]]
}

# The frozen_tex dump OPENPETE_PT_DUMP_FROZEN wrote in a leg directory.
frozen_of() { find "$1" -maxdepth 1 -name 'p*.png.frozen.png' | sort | head -1; }

# The newest bd-ref-f<VBL>.png at or before a VBL in a leg directory, as its VBL.
ref_vbl_of() {
    find "$1" -maxdepth 1 -name 'bd-ref-f*.png' -printf '%f\n' | sed 's/^bd-ref-f//; s/\.png$//' |
        awk -v lim="$2" '$1 <= lim' | sort -n | tail -1
}

# A `[freeze-bd]` counter from a leg log, 0 when absent.
bd_count() {
    local v
    v=$(grep -a '^\[freeze-bd\] rendered=' "$1" 2>/dev/null | tail -1 | grep -oE "(^|[] ])$2=[0-9]+" | cut -d= -f2)
    echo "${v:-0}"
}

rows=0; failed=0
while IFS='|' read -r bk2 flim dump pin launch; do
    bk2="${bk2//[[:space:]]/}"
    [[ -z "$bk2" || "${bk2:0:1}" == "#" ]] && continue
    flim="${flim//[[:space:]]/}"; dump="${dump//[[:space:]]/}"; pin="${pin//[[:space:]]/}"
    [[ -n "$FILTER" && "$bk2" != *"$FILTER"* ]] && continue
    movie="$PG_BK2_DIR/$bk2"
    [[ -f "$movie" ]] || { echo "live-freeze-bd: SKIP ($movie missing)"; exit 64; }
    rows=$((rows + 1))
    pg_split_launch "${launch:-}"
    tag="${bk2%.bk2}"
    hd="$OUT/$tag/headless"

    # The headless leg and the first live leg run side by side; the rest one
    # at a time, so at most two game processes run at once.
    legs=()
    for s in $SCALES; do legs+=("live:$s:1"); done
    (( CONTROL )) && legs+=("control:${SCALES%% *}:0")
    run_leg "$hd" "$movie" "$flim" "$dump" headless 1 1 &
    hpid=$!
    first=1
    for l in "${legs[@]}"; do
        IFS=: read -r kind scale bd <<<"$l"
        d="$OUT/$tag/$kind-$scale"
        run_leg "$d" "$movie" "$flim" "$dump" live "$scale" "$bd" || echo "  leg $d produced no log"
        if (( first )); then wait "$hpid"; first=0; fi
    done
    (( first )) && wait "$hpid"

    shot="$hd/bd-shot-f$pin.png"
    if [[ ! -f "$shot" ]]; then
        echo "[live-freeze-bd] game=$GAME bk2=$bk2 leg=headless verdict=VACUOUS reason=no bd-shot-f$pin.png (see $hd/run.log)"
        failed=$((failed + 1)); continue
    fi
    seen=0
    for l in "${legs[@]}"; do
        IFS=: read -r kind scale bd <<<"$l"
        d="$OUT/$tag/$kind-$scale"
        log="$d/run.log"
        frz="$(frozen_of "$d")"
        rv="$(ref_vbl_of "$d" "$dump")"
        ref="$d/bd-ref-f${rv:-none}.png"
        diff="$(px_diff "${frz:-$d/none.png}" "$ref")"
        tickn="$(px_diff "$ref" "$shot")"
        rendered="$(bd_count "$log" rendered)"
        displayed="$(bd_count "$log" displayed)"
        eager=$(grep -ac 'eager: engaged' "$log" 2>/dev/null)
        inter=$(grep -aoE '[0-9]+ intermediate scene\(s\) presented' "$log" 2>/dev/null | tail -1 | cut -d' ' -f1)
        inter="${inter:-0}"
        verdict=PASS; why=""; rdiff="-"
        if [[ "$kind" == control ]]; then
            if ! [[ "$diff" =~ ^[0-9]+$ ]]; then verdict=VACUOUS; why="frozen dump or reference $diff"; fi
        else
            rdiff="$(px_diff "$d/bd-live-f${rv:-none}.png" "$ref")"
            if (( eager == 0 || inter == 0 )); then verdict=VACUOUS; why="eager not engaged (engaged=$eager intermediates=$inter)"
            elif (( rendered == 0 || displayed == 0 )); then verdict=VACUOUS; why="rendered=$rendered displayed=$displayed"
            elif ! [[ "$diff" =~ ^[0-9]+$ && "$rdiff" =~ ^[0-9]+$ ]]; then verdict=VACUOUS; why="dump $diff/$rdiff"
            elif (( diff != 0 || rdiff != 0 )); then verdict=FAIL; fi
            [[ "$tickn" =~ ^[0-9]+$ ]] && (( tickn > 0 )) && seen=$((seen + 1))
        fi
        [[ "$verdict" == PASS ]] || failed=$((failed + 1))
        echo "[live-freeze-bd] game=$GAME bk2=$bk2 leg=$kind-$scale ref=f${rv:-none} diff=$diff render_diff=$rdiff" \
             "tickn=$tickn rendered=$rendered displayed=$displayed intermediates=$inter verdict=$verdict${why:+ reason=$why}"
    done
    if (( seen == 0 )); then
        echo "[live-freeze-bd] game=$GAME bk2=$bk2 verdict=VACUOUS reason=no live reference differs from bd-shot-f$pin.png"
        failed=$((failed + 1))
    fi
done <"$MANIFEST"

echo "[live-freeze-bd] game=$GAME rows=$rows failed=$failed"
if (( rows == 0 )); then
    echo "live-freeze-bd: FAIL ($MANIFEST has no selected rows)"; exit 1
fi
(( failed )) && exit 1
exit 0
