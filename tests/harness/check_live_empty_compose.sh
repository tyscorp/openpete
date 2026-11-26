#!/usr/bin/env bash
# check_live_empty_compose.sh - a live ring-slot compose of an empty scene
# shows the held scene its capture's verdict named, at every tick rate.
#
# A scene with no game triangle composes the scene held for the draw area
# its display shows (the hold store). The verdict is taken when the scene
# is published into a ring slot; the compose runs later on the present
# thread. An image op between the two (a ClearImage of both buffers) marks
# the area as written by VRAM; a slot that kept only the area index then
# finds no scene at the compose, counts it lost and composes the VRAM
# framebuffer, which on a live run still holds the last image upload (a
# boot logo). A slot holding a reference on the pool entry its verdict
# named composes that entry.
#
# Input: games/<slug>/corpus/live-empty-compose-corpus.manifest, rows
#   <bk2> | <frame-limit> | <dump frame> | <og VBL> | <launch tokens>
# The dump frame is the scene frame of the empty scene (the [pt-dump]
# frame); the og VBL is the VBL the headless leg captures the guest
# reference (-og) at, the same picture on hardware. Launch tokens follow the
# presentation corpus convention (KEY=VALUE with an upper-case key is an
# environment entry).
#
# Legs per row: one headless leg writing <og VBL>-og.png; then, per tick
# scale (default 0.25 0.5 1, --scales "A B ..."), --repeats N (default 3)
# live legs, Xvfb at 640x432 (the reference size), eager presentation
# (--render-fps 120 --vsync 0), OPENPETE_PT_DUMP=<dump frame> with one dump
# and OPENPETE_HOLD_CENSUS=1; and the same legs under
# OPENPETE_HOLD_SLOT_REF=0 (the control: slots keep the area index alone),
# skipped by --no-control; and one stalled pair (fix and control) at tick
# scale 1 under OPENPETE_HOLD_STALL_FRAME=<dump frame>, which holds the
# present thread before composing that scene until the guest has begun a
# later scene, so the guest's next tick (its image ops) runs
# before the empty scene's compose on every run. At most two game
# processes run at once.
#
# Pass criterion per live leg: the `hold-scene:` census counts lost=0, and
# the dumped present differs from the -og reference in at most 8 pixels by
# more than 8 levels in any channel. Engagement (refused as VACUOUS, never
# PASS): the leg logged `eager: engaged`, its single [pt-dump] line names
# the dump frame with tri=0, and a `[hold] empty ... scene=<dump frame>
# ... slot=1` line shows the ring-slot empty compose of that scene. The
# control has no pass condition per leg. The row is VACUOUS unless the
# stalled fix leg's compose of the dump scene took the slot's reference
# after the area stopped naming it (branch held-ref-moved) and the stalled
# control leg shows the defect (lost>0 or more than 8 differing pixels),
# so the row proves it reached the race and tells the fixed path from the
# broken one.
#
# Verdict line per leg:
#   [live-empty-compose] game=<g> bk2=<b> leg=<fix|control>-<scale>-<r>
#       frame=<f> tri=<n> branch=<held-ref|held|lost|vram|none> lost=<n>
#       px=<n> verdict=...
#
# Usage:
#   tests/harness/check_live_empty_compose.sh [--game SLUG]
#       [--build-dir=DIR] [--filter=SUBSTR] [--out=DIR]
#       [--scales "0.25 0.5 1"] [--repeats N] [--no-control]
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

FILTER=""; OUT=""; SCALES="0.25 0.5 1"; REPEATS=3; CONTROL=1
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
        --out=*)       OUT="${a#--out=}";;
        --scales=*)    SCALES="${a#--scales=}";;
        --repeats=*)   REPEATS="${a#--repeats=}";;
        --no-control)  CONTROL=0;;
    esac
    case "$prev" in
        --filter)  FILTER="$a";;
        --out)     OUT="$a";;
        --scales)  SCALES="$a";;
        --repeats) REPEATS="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

WIDTH=640
HEIGHT=432
PX_LEVEL=8
PX_MAX=8
LEG_RUNTIME=840
LEG_TIMEOUT=900

MANIFEST="$(pg_corpus_manifest "$GAME" live-empty-compose-corpus.manifest)"
pg_manifest "$MANIFEST" pipe
op_xvfb_prefix live-empty-compose "${WIDTH}x${HEIGHT}"
[[ -x "$PG_BIN" ]] || { echo "live-empty-compose: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "live-empty-compose: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/live-empty-compose/$GAME"
mkdir -p "$OUT"

# Pixels of A differing from B by more than PX_LEVEL in any channel, or
# "shape" / "missing".
px_diff() {
    python3 - "$1" "$2" "$PX_LEVEL" <<'PY'
import os, sys
import numpy as np
from PIL import Image
a, b, lvl = sys.argv[1], sys.argv[2], int(sys.argv[3])
if not (os.path.isfile(a) and os.path.isfile(b)):
    print("missing"); sys.exit(0)
A = np.asarray(Image.open(a).convert("RGB")).astype(int)
B = np.asarray(Image.open(b).convert("RGB")).astype(int)
print("shape" if A.shape != B.shape else int((np.abs(A - B) > lvl).any(axis=2).sum()))
PY
}

# One leg. The leg's directory and log are deleted before the run and the
# log asserted after it, so no output of an earlier run is read as this
# one's. stdin is closed: the row loop reads the manifest on its own stdin.
run_leg() {  # dir movie flim dump og mode(headless|live) scale slotref(1|0) stall(1|0)
    local dir="$1" movie="$2" flim="$3" dump="$4" og="$5" mode="$6" scale="$7" ref="$8" stall="$9"
    rm -rf "${dir:?}"; mkdir -p "$dir"
    local -a env=(OPENPETE_VIBRATION=0 "${ROW_ENV[@]}")
    local -a args=(--no-config --disc "$PG_DISC" --disc-cache 0 --mute 1 --vibration 0
                   --renderer vk "${PG_RUN_ARGS[@]}" --bk2 "$movie"
                   --frame-limit "$flim" --max-runtime-seconds "$LEG_RUNTIME" "${ROW_ARGS[@]}")
    if [[ "$mode" == headless ]]; then
        op_own_run env "${env[@]}" timeout -k 10 "$LEG_TIMEOUT" "$PG_BIN" --headless \
            "${args[@]}" --screenshot-on-frame "$og" --screenshot-output "$dir/shot__{n}.png" \
            </dev/null >"$dir/run.log" 2>&1
    else
        (( stall )) && env+=(OPENPETE_HOLD_STALL_FRAME="$dump")
        env+=(OPENPETE_HOLD_SLOT_REF="$ref"
              OPENPETE_HOLD_CENSUS=1 OPENPETE_PT_DUMP="$dump"
              OPENPETE_PT_DUMP_COUNT=1 OPENPETE_PT_DUMP_PATH="$dir")
        op_own_run env "${env[@]}" "${OP_XVFB[@]}" timeout -k 10 "$LEG_TIMEOUT" "$PG_BIN" \
            "${args[@]}" --input-lock --background 1 --window "${WIDTH}x${HEIGHT}" \
            --vsync 0 --render-fps 120 --tick-scale "$scale" --perf-stats off --osd off \
            </dev/null >"$dir/run.log" 2>&1
    fi
    [[ -s "$dir/run.log" ]]
}

# A `hold-scene:` counter from a leg log, empty when absent.
hold_count() {
    grep -a '^hold-scene: empty composes' "$1" 2>/dev/null | tail -1 | grep -oE "(^|[ (])$2=[0-9]+" |
        head -1 | cut -d= -f2
}

rows=0; failed=0
while IFS='|' read -r bk2 flim dump og launch; do
    bk2="${bk2//[[:space:]]/}"
    [[ -z "$bk2" || "${bk2:0:1}" == "#" ]] && continue
    flim="${flim//[[:space:]]/}"; dump="${dump//[[:space:]]/}"; og="${og//[[:space:]]/}"
    [[ -n "$FILTER" && "$bk2" != *"$FILTER"* ]] && continue
    movie="$PG_BK2_DIR/$bk2"
    [[ -f "$movie" ]] || { echo "live-empty-compose: SKIP ($movie missing)"; exit 64; }
    rows=$((rows + 1))
    pg_split_launch "${launch:-}"
    tag="${bk2%.bk2}"
    hd="$OUT/$tag/headless"

    legs=("fix-stall:1:1:1:1" "control-stall:1:1:0:1")
    for s in $SCALES; do
        for ((r = 1; r <= REPEATS; r++)); do
            legs+=("fix:$s:$r:1:0")
            (( CONTROL )) && legs+=("control:$s:$r:0:0")
        done
    done
    # The headless leg runs beside the first live leg; the live legs then
    # run two at a time.
    run_leg "$hd" "$movie" "$flim" "$dump" "$og" headless 1 1 0 &
    pids=("$!")
    for l in "${legs[@]}"; do
        IFS=: read -r kind scale r ref stall <<<"$l"
        run_leg "$OUT/$tag/$kind-$scale-$r" "$movie" "$flim" "$dump" "$og" live "$scale" "$ref" "$stall" &
        pids+=("$!")
        if (( ${#pids[@]} >= 2 )); then wait "${pids[0]}"; pids=("${pids[@]:1}"); fi
    done
    for p in "${pids[@]}"; do wait "$p"; done

    ref_png="$hd/shot__$og-og.png"
    if [[ ! -f "$ref_png" ]]; then
        echo "[live-empty-compose] game=$GAME bk2=$bk2 leg=headless verdict=VACUOUS reason=no shot__$og-og.png (see $hd/run.log)"
        failed=$((failed + 1)); continue
    fi
    shown=0; rescued=0
    for l in "${legs[@]}"; do
        IFS=: read -r kind scale r ref stall <<<"$l"
        d="$OUT/$tag/$kind-$scale-$r"
        log="$d/run.log"
        dl="$(grep -a '^\[pt-dump\] seq=0 ' "$log" 2>/dev/null | head -1)"
        frame="$(sed -nE 's/.* frame=([0-9]+) .*/\1/p' <<<"$dl")"
        tri="$(sed -nE 's/.* tri=(-?[0-9]+) .*/\1/p' <<<"$dl")"
        png="$(sed -nE 's/.* -> (.*\.png)$/\1/p' <<<"$dl")"
        branch="$(grep -aE "^\[hold\] empty f=[0-9]+ scene=$dump .* slot=1 " "$log" 2>/dev/null |
                  head -1 | sed -nE 's/.* -> ([a-z-]+).*/\1/p')"
        lost="$(hold_count "$log" lost)"
        px="$(px_diff "${png:-$d/none.png}" "$ref_png")"
        eager=$(grep -ac 'eager: engaged' "$log" 2>/dev/null)
        verdict=PASS; why=""
        if (( eager == 0 )); then verdict=VACUOUS; why="eager not engaged"
        elif [[ "$frame" != "$dump" || "$tri" != 0 ]]; then verdict=VACUOUS; why="dump frame=${frame:-none} tri=${tri:-none}"
        elif [[ -z "$branch" ]]; then verdict=VACUOUS; why="no ring-slot empty compose of scene $dump"
        elif [[ -z "$lost" || ! "$px" =~ ^[0-9]+$ ]]; then verdict=VACUOUS; why="census lost=${lost:-none} px=$px"
        elif (( lost != 0 || px > PX_MAX )); then verdict=FAIL; fi
        [[ "$kind" == fix-stall && "$verdict" == PASS && "$branch" == held-ref-moved ]] && rescued=1
        if [[ "$kind" == control* ]]; then
            [[ "$kind" == control-stall && "$verdict" == FAIL ]] && shown=1
            [[ "$verdict" == VACUOUS ]] || verdict="control-${verdict,,}"
        fi
        [[ "$verdict" == PASS || "$verdict" == control-* ]] || failed=$((failed + 1))
        echo "[live-empty-compose] game=$GAME bk2=$bk2 leg=$kind-$scale-$r frame=${frame:-none} tri=${tri:-none}" \
             "branch=${branch:-none} lost=${lost:-none} px=$px verdict=$verdict${why:+ reason=$why}"
    done
    if (( rescued == 0 || shown == 0 )); then
        echo "[live-empty-compose] game=$GAME bk2=$bk2 verdict=VACUOUS reason=stalled legs did not reach the race" \
             "(fix held-ref-moved=$rescued control defect=$shown)"
        failed=$((failed + 1))
    fi
done <"$MANIFEST"

echo "[live-empty-compose] game=$GAME rows=$rows failed=$failed"
if (( rows == 0 )); then
    echo "live-empty-compose: FAIL ($MANIFEST has no selected rows)"; exit 1
fi
(( failed )) && exit 1
exit 0
