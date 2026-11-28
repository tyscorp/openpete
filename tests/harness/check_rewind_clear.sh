#!/usr/bin/env bash
# tests/harness/check_rewind_clear.sh - a host intervention inside the rewind
# window clears the rewind history.
#
# A rewind restores a keyframe and replays the ticks after it from the logged
# pad input only, so a teleport applied inside the window would be missing
# from the rebuilt state. The harness places the player (--player-pose
# "x,y,z@K", the teleport body the free camera's T key shares) at feed key K
# of a level with the pad neutral, then holds rewind from feed key A > K for N iterations,
# far enough to cross K. Four headless runs:
#   clean    - the placement, --gameplay-hash-log, no rewind
#   bare     - no placement, no rewind
#   control  - no placement, --rewind 30 --rewind-hold A:N
#   rewind   - the placement, --rewind 30 --rewind-hold A:N
# The control calibrates the row: its hold must land before K (the hold
# reaches past the placement) and match the bare run row for row (the rewind
# itself is exact here), so a divergence of the rewind run is the
# placement's. Pass: the rewind run logs the placement and the history clear
# it caused (engagement: a nonzero clear count in its exit report), its hash
# log jumps backward and every jump lands at or after K (the rewind did not
# cross the placement; a hold that outlasts the cleared history steps
# between the oldest keyframes, one jump each), and every row matches the
# clean row of its frame (the pad is neutral, so the resumed timeline re-runs the same
# future). A build that keeps the history across the placement
# rewinds past K, replays the window without it, and diverges.
#
# The attract-demo oracle never places the player or rewinds, so it cannot
# reach this path (LAW §8); this is the row that does.
#
# Usage: tests/harness/check_rewind_clear.sh [--game SLUG]
# Exit 0 PASS, 1 FAIL (a VACUOUS run fails), 64 SKIP (no binary or disc). KEEP_OUT=1 keeps the run directory. Runs from the repo root.

set -u

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME" || exit 64
# Hold the build directory against a build for the whole run (build_lock.sh).
# shellcheck source=scripts/lib/build_lock.sh
source scripts/lib/build_lock.sh
op_build_read_lock "$PG_BIN" rewind-clear || exit 75

# Per game: the boot (a level, the pad neutral), the placement (x,y,z@K, a
# point above the player at K so the placement moves him) and the hold A:N.
# Spyro 1: on level 10 the player stands at (84992, 47173, 9556) from feed
# key 1800; the placement drops him from 1444 units above, and 40 held
# iterations from 2200 reach back past 2150.
case "$PG_GAME" in
    spyro-1)
        BOOT=(--skip-to-level 10)
        POSE="84992,47173,11000@2150"
        HOLD=2200:40 ;;
    *) echo "SKIP: $PG_GAME has no rewind-clear row"; exit 64 ;;
esac
POSE_KEY="${POSE##*@}"

if [ ! -x "$PG_BIN" ]; then echo "SKIP: no binary at $PG_BIN"; exit 64; fi
if [ ! -f "$PG_DISC" ]; then echo "SKIP: no disc at $PG_DISC"; exit 64; fi

OUT="${TMPDIR:-/tmp}/openpete-rewind-clear.$$"
mkdir -p "$OUT"
trap '[ -n "${KEEP_OUT:-}" ] || rm -rf "${OUT:?}"' EXIT

# pg_capture deletes the hash log and the run log first and asserts the hash
# log after.
run() { # $1 csv  $2 extra-args...
    local csv="$1"; shift
    local PG_CAPTURE_TIMEOUT=600
    pg_capture "$csv.log" "$csv" -- \
        "${BOOT[@]}" --frame-limit 2400 \
        --unlock-frames 0-2400 --gameplay-hash-log "$csv" "$@"
}

# The rewind run's hash log against the run with no rewind: `before`, one
# backward jump landing before K; `after`, at least one, every one landing
# at or after K; and no row that differs from the other run's row of its
# frame.
verdict() { # $1 no-rewind csv  $2 rewind csv  $3 before|after  $4 leg name
    python3 - "$1" "$2" "$POSE_KEY" "$3" "$4" <<'PY'
import csv, sys
clean = {r[0]: r[2] for r in csv.reader(open(sys.argv[1])) if r and r[0] != 'frame'}
rows = [r for r in csv.reader(open(sys.argv[2])) if r and r[0] != 'frame']
key, side, leg = int(sys.argv[3]), sys.argv[4], sys.argv[5]
fr = [int(r[0]) for r in rows]
gaps = [(fr[i-1], fr[i]) for i in range(1, len(fr)) if fr[i] < fr[i-1]]
mism = [r[0] for r in rows if r[0] in clean and clean[r[0]] != r[2]]
landed = min(g[1] for g in gaps) if gaps else None
if side == 'before':
    ok = len(gaps) == 1 and landed < key
else:
    ok = bool(gaps) and landed >= key
ok = ok and not mism
print(f"  {leg}: jumps={len(gaps)} first={gaps[:1]} lowest={landed} (want {side} {key}) "
      f"mismatches={len(mism)}" + (f" first={mism[0]}" if mism else "")
      + " -> " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PY
}

fail=0
echo "== rewind-clear ($PG_GAME ${BOOT[*]}, pose@$POSE_KEY, hold@${HOLD%%:*} x${HOLD##*:})"
run "$OUT/clean.csv" --player-pose "$POSE" || { echo "  clean run FAILED"; exit 1; }
run "$OUT/bare.csv" || { echo "  bare run FAILED"; exit 1; }
run "$OUT/ctl.csv" --rewind 30 --rewind-hold "$HOLD" || { echo "  control run FAILED"; exit 1; }
run "$OUT/rw.csv" --player-pose "$POSE" --rewind 30 --rewind-hold "$HOLD" \
    || { echo "  rewind run FAILED"; exit 1; }

verdict "$OUT/bare.csv" "$OUT/ctl.csv" before control || fail=1
placed="$(grep -c 'player placed at' "$OUT/rw.csv.log")"
clears="$(grep -o 'cleared by a host intervention: [0-9]*' "$OUT/rw.csv.log" | tail -1 | awk '{print $NF}')"
held="$(grep -c 'rewind: hold [0-9]*/' "$OUT/rw.csv.log")"
echo "  placements=$placed hold-iterations=$held intervention-clears=${clears:-none}"
if [ "$placed" -eq 0 ] || [ "$held" -eq 0 ]; then
    echo "  VACUOUS: the placement or the hold never ran -> FAIL"; fail=1
fi
if [ "${clears:-0}" -eq 0 ]; then
    echo "  no intervention clear reported -> FAIL"; fail=1
fi
verdict "$OUT/clean.csv" "$OUT/rw.csv" after rewind || fail=1

[ $fail -eq 0 ] && echo "REWIND CLEAR PASS" || echo "REWIND CLEAR FAILED"
exit $fail
