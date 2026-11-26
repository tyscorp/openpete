#!/usr/bin/env bash
# check_camera_cut_subtick.sh - a camera cut at a level load never holds a
# tick N-1 that drew no world, and one cut pair never switches between
# showing tick N and holding tick N-1 before alpha 1.
#
# The verdict is taken on live sub-tick presents: a headless capture
# re-extracts once per capture and never presents twice inside one cut pair,
# so it cannot show a verdict that changes between presents. Each leg is a
# windowed eager replay under Xvfb (--render-fps 120 --vsync 0, tick scale 1)
# with OPENPETE_CUT_CENSUS=1, which prints per camera-cut pair
#   [cut-census vbl=V] camcut-pair ptick=P ctick=C canon=.. world=W1/W0 edges=E1/E0 load=L verdict=X
# per present whose holdability differs from the pair's previous present
#   [cut-census vbl=V] camcut-flip ptick=P ctick=C was=X now=Y
# and per camera-cut decision the running counts
#   [cut-census vbl=V] loadcut pairs=N held_noworld=H flips=F
#
# Manifest: games/<slug>/corpus/camera-cut-subtick-corpus.manifest, rows
#   <bk2> | <frame-limit> | <cut VBL>[,<cut VBL>...] | <legs> | <launch tokens>
# A cut VBL is the ctick of the pair (the VBL of the tick the camera cut
# to). `launch` takes the presentation-corpus convention (pg_split_launch).
#
# Pass criterion, per row: on every leg the last loadcut line reports
# held_noworld=0 and flips=0, and no leg prints a camcut-flip line.
# Engagement (VACUOUS, exit 1): the guarded legs together observe at least
# two distinct cut pairs a load test names (camcut-pair lines with
# load=noworld or load=edge), and every guarded leg prints a loadcut line.
# Whether a cut pair reaches a sub-tick present depends on present timing
# during the load, so a listed cut VBL no leg observed is reported, not
# failed.
# Control (reported, not gating): one leg with OPENPETE_CUT_LOAD_GUARD=0,
# whose counts show what the guard refuses; the defect it measures is
# timing-dependent and is not reached on every leg.
#
# Usage:
#   tests/harness/check_camera_cut_subtick.sh [--game SLUG] [--build-dir=DIR]
#       [--filter=SUBSTR] [--out=DIR] [--no-control]
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

FILTER=""; OUT=""; CONTROL=1
for a in "$@"; do
    case "$a" in
        --build-dir=*) PG_BUILD_DIR="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
        --out=*)       OUT="${a#--out=}";;
        --no-control)  CONTROL=0;;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"

WIDTH=640
HEIGHT=448
LEG_RUNTIME=840
LEG_TIMEOUT=900

MANIFEST="$(pg_corpus_manifest "$GAME" camera-cut-subtick-corpus.manifest)"
pg_manifest "$MANIFEST" pipe
op_xvfb_prefix camera-cut-subtick "${WIDTH}x${HEIGHT}"
[[ -x "$PG_BIN" ]] || { echo "camera-cut-subtick: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "camera-cut-subtick: SKIP (no disc at $PG_DISC)"; exit 64; }

[[ -n "$OUT" ]] || OUT="${PG_BUILD_DIR:-build/linux-release}/camera-cut-subtick/$GAME"
mkdir -p "$OUT"

# One live leg. The leg's directory is deleted before the run and its log
# asserted after it. stdin is closed: the row loop reads the manifest.
run_leg() {  # dir movie flim guard
    local dir="$1" movie="$2" flim="$3" guard="$4"
    rm -rf "${dir:?}"; mkdir -p "$dir"
    op_own_run env OPENPETE_VIBRATION=0 OPENPETE_CUT_CENSUS=1 OPENPETE_CUT_LOAD_GUARD="$guard" \
        "${ROW_ENV[@]}" "${OP_XVFB[@]}" timeout -k 10 "$LEG_TIMEOUT" "$PG_BIN" \
        --no-config --disc "$PG_DISC" --disc-cache 0 --mute 1 --vibration 0 \
        --renderer vk "${PG_RUN_ARGS[@]}" --bk2 "$movie" \
        --frame-limit "$flim" --max-runtime-seconds "$LEG_RUNTIME" "${ROW_ARGS[@]}" \
        --input-lock --background 1 --window "${WIDTH}x${HEIGHT}" \
        --vsync 0 --render-fps 120 --tick-scale 1 --perf-stats off --osd off \
        </dev/null >"$dir/run.log" 2>&1
    [[ -s "$dir/run.log" ]]
}

# One leg's counts: "pairs held_noworld flips flip_lines loadcut_cticks", or
# "none" without a loadcut line.
leg_counts() {
    python3 - "$1" <<'PY'
import re, sys
txt = open(sys.argv[1], errors="replace").read()
last = re.findall(r"\] loadcut pairs=(\d+) held_noworld=(\d+) flips=(\d+)", txt)
if not last:
    print("none"); sys.exit(0)
p, h, f = last[-1]
flips = len(re.findall(r"\] camcut-flip ", txt))
cuts = sorted({c for c in re.findall(r"\] camcut-pair ptick=\d+ ctick=(\d+) .* load=(?:noworld|edge) ", txt)}, key=int)
print(p, h, f, flips, ",".join(cuts) or "-")
PY
}

rows=0; failed=0
while IFS='|' read -r bk2 flim cuts nlegs launch; do
    bk2="${bk2//[[:space:]]/}"
    [[ -z "$bk2" || "${bk2:0:1}" == "#" ]] && continue
    flim="${flim//[[:space:]]/}"; cuts="${cuts//[[:space:]]/}"; nlegs="${nlegs//[[:space:]]/}"
    [[ -n "$FILTER" && "$bk2" != *"$FILTER"* ]] && continue
    movie="$PG_BK2_DIR/$bk2"
    [[ -f "$movie" ]] || { echo "camera-cut-subtick: SKIP ($movie missing)"; exit 64; }
    rows=$((rows + 1))
    pg_split_launch "${launch:-}"
    tag="${bk2%.bk2}"

    # Two legs at a time: at most two game processes run at once.
    legs=()
    for ((i = 1; i <= nlegs; i++)); do legs+=("leg$i:1"); done
    (( CONTROL )) && legs+=("control:0")
    pids=()
    for l in "${legs[@]}"; do
        IFS=: read -r name guard <<<"$l"
        run_leg "$OUT/$tag/$name" "$movie" "$flim" "$guard" &
        pids+=($!)
        if (( ${#pids[@]} == 2 )); then wait "${pids[@]}"; pids=(); fi
    done
    (( ${#pids[@]} )) && wait "${pids[@]}"

    bad=0; vac=0; seen=","
    for l in "${legs[@]}"; do
        IFS=: read -r name guard <<<"$l"
        c="$(leg_counts "$OUT/$tag/$name/run.log")"
        if [[ "$c" == none ]]; then
            echo "[camera-cut-subtick] game=$GAME bk2=$bk2 leg=$name verdict=VACUOUS reason=no loadcut line (see $OUT/$tag/$name/run.log)"
            [[ "$guard" == 1 ]] && vac=1
            continue
        fi
        read -r pairs held flips flipl lc <<<"$c"
        echo "[camera-cut-subtick] game=$GAME bk2=$bk2 leg=$name guard=$guard pairs=$pairs held_noworld=$held flips=$flips flip_lines=$flipl loadcuts=$lc"
        [[ "$guard" == 1 ]] || continue
        seen="$seen$lc,"
        (( held == 0 && flips == 0 && flipl == 0 )) || bad=1
    done
    missing=""
    for v in ${cuts//,/ }; do [[ "$seen" == *",$v,"* ]] || missing="$missing $v"; done
    distinct=$(tr ',' '\n' <<<"$seen" | grep -E '^[0-9]+$' | sort -u | wc -l)
    if (( bad )); then
        echo "[camera-cut-subtick] game=$GAME bk2=$bk2 verdict=FAIL"; failed=$((failed + 1))
    elif (( vac || distinct < 2 )); then
        echo "[camera-cut-subtick] game=$GAME bk2=$bk2 verdict=VACUOUS loadcuts_observed=$distinct unobserved listed:${missing:- none}"
        failed=$((failed + 1))
    else
        echo "[camera-cut-subtick] game=$GAME bk2=$bk2 verdict=PASS loadcuts_observed=$distinct unobserved listed:${missing:- none}"
    fi
done <"$MANIFEST"

(( rows > 0 )) || { echo "camera-cut-subtick: FAIL ($MANIFEST has no rows)"; exit 1; }
(( failed == 0 )) || { echo "camera-cut-subtick: FAIL ($failed of $rows row(s))"; exit 1; }
echo "camera-cut-subtick: PASS ($rows row(s))"
