#!/usr/bin/env bash
#
# check_leg_retry.sh - the retry of a crashed capture leg
# (scripts/presentation-game.sh, crashed legs) through the real corpus
# scripts, with a stub binary in place of the game: no disc, no Vulkan device, no capture image.
#
# A synthetic tree links this checkout's scripts/ and tests/ and holds its own
# one-row corpus manifests. The stub writes the tiles (a valid 8x8 PNG) and
# tile-census sidecars a leg asks for, prints the freecam intermediate leg's
# bracket summary, and aborts (SIGABRT, rc 134) on the legs whose output
# pattern contains STUB_CRASH, by STUB_MODE:
#   once      the first launch of the leg aborts after writing its tiles, the
#             second passes: the corpus passes, every tile is present, the
#             leg is kept once and recorded `retry=pass`, the stream carries
#             `CRASHED <leg> rc=134 retry=pass` and the row's tile lines once
#   always    every launch aborts before writing a tile: the corpus fails,
#             the leg and <leg>.retry are kept, the leg is recorded
#             `retry=fail` and pg_retry_failed names it
#   exit1     the leg exits 1 (no signal): kept, not retried
# Each case runs the bk2 corpus (presentation-corpus.sh) and the freecam
# corpus (freecam-corpus.sh), and counts the stub's launches (engagement: 3
# legs of a row plus one per retry). The `once` case runs twice and its
# normalized record (crashes, retries, tiles, stream verbs, launch count) must
# be identical (deterministic).
#
# Exit: 0 every case holds, 1 a case failed (named), 64 SKIP (no Vulkan
# loader, which the corpus scripts require, or no PIL).
set -u
cd "$(dirname "$0")/../.."
REPO="$PWD"
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1' || { echo "[leg-retry] SKIP: no Vulkan loader"; exit 64; }
python3 -c 'import PIL' 2>/dev/null || { echo "[leg-retry] SKIP: no PIL"; exit 64; }

W="$(mktemp -d -t leg-retry.XXXXXX)"
trap 'rm -rf "${W:?}"' EXIT
fails=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1: $2"; fails=$((fails + 1)); }
ulimit -c 0

# The synthetic tree: this checkout's scripts and harnesses, its own corpus.
T="$W/tree"
mkdir -p "$T/games/spyro-1/corpus" "$W/build"
ln -s "$REPO/scripts" "$T/scripts"
ln -s "$REPO/tests" "$T/tests"
printf 'rowa | 100 | 100 |  | 150\n' >"$T/games/spyro-1/corpus/presentation-corpus.manifest"
printf 'lvl_one | pos=(1, 2, 3) yaw=0 pitch=0\n' >"$T/games/spyro-1/corpus/freecam-corpus.manifest"
FSLUG="$(pg_pose_slug lvl_one 'pos=(1, 2, 3) yaw=0 pitch=0')"
: >"$W/disc.cue"
python3 -c 'import sys; from PIL import Image; Image.new("RGB", (8, 8), (10, 20, 30)).save(sys.argv[1])' "$W/tile.png"

# The stub binary. Its launches are counted in STUB_STATE/launches.
cat >"$W/build/openpete" <<'STUB'
#!/usr/bin/env bash
out="" frames=""
while (( $# )); do
  case "$1" in
    --screenshot-output) out="$2"; shift;;
    --screenshot-on-frame|--screenshot-intermediate-on-frame) frames="$2"; shift;;
  esac
  shift
done
echo x >>"$STUB_STATE/launches"
leg="$(basename "$out")"
crash=0
if [[ -n "${STUB_CRASH:-}" && "$leg" == *"$STUB_CRASH"* ]]; then
  key="$STUB_STATE/$(echo "$leg" | md5sum | cut -c1-8)"
  n=$(( $(cat "$key" 2>/dev/null || echo 0) + 1 )); echo "$n" >"$key"
  case "$STUB_MODE" in
    once) (( n == 1 )) && crash=1;;
    always) kill -ABRT $$;;
    exit1) echo "stub: refused"; exit 1;;
  esac
fi
echo "[rhi/vk] device='stub' api=1.3 driver-info='stub'"
IFS=',' read -ra fs <<<"$frames"
for f in "${fs[@]}"; do
  png="${out//\{n\}/$f}"; png="${png%.png}-vk.png"
  cp "$STUB_TILE" "$png"
  [[ -n "${OPENPETE_TILE_CENSUS:-}" ]] && echo "census stub" >"${png%.png}.census"
done
echo "intermediate screenshot leg(s): 1 bracketed, 0 unbracketed"
(( crash )) && kill -ABRT $$
exit 0
STUB
chmod +x "$W/build/openpete"

# Run both corpora into a fresh candidate directory under one crash mode.
# Sets OUT, LOG, BRC, FRC, LAUNCHES. args: case-name mode crash-substring
run_case() {
  local name="$1"
  OUT="$W/$name/cand" LOG="$W/$name/run.log"
  mkdir -p "$W/$name/state"
  : >"$W/$name/stream"
  local -a envs=(STUB_STATE="$W/$name/state" STUB_TILE="$W/tile.png" STUB_MODE="$2" STUB_CRASH="$3"
    OPENPETE_DISC="$W/disc.cue" OPENPETE_CAP_LODS=original CORPUS_JOBS=1
    OPENPETE_STREAM_FILE="$W/$name/stream" OPENPETE_RESUME_RUN=
    # One crash directory and list for both corpora, as the gate exports them.
    PG_CRASH_DIR="$W/$name/crashes" PG_CRASH_LIST="$OUT/crashes.txt")
  env "${envs[@]}" "$T/tests/harness/presentation-corpus.sh" default "$OUT" "" "--build-dir=$W/build" >"$LOG" 2>&1
  BRC=$?
  env "${envs[@]}" "$T/tests/harness/freecam-corpus.sh" default "$OUT" "" "--build-dir=$W/build" >>"$LOG" 2>&1
  FRC=$?
  LAUNCHES=$(wc -l <"$W/$name/state/launches" 2>/dev/null || echo 0)
}

# A run's normalized record: crash rows (no paths), retries, tiles, stream
# verbs and tiles (no epochs), launch count. args: case-name
record() {
  { cut -f1,2 "$OUT/crashes.txt" 2>/dev/null | sort
    sort "$OUT/crash-retries.txt" 2>/dev/null
    (cd "$OUT" && ls ./*-vk.png | sort)
    awk '{ print $1, $2, ($1 == "CRASHED" ? $3 " " $4 : "") }' "$W/$1/stream" | sort
    echo "launches=$LAUNCHES brc=$BRC frc=$FRC"; }
}

# once: the canon legs abort on their first launch only.
run_case once once __canon__
tiles=$(ls "$OUT"/*-vk.png 2>/dev/null | wc -l)
if [[ $BRC -eq 0 && $FRC -eq 0 && $tiles -eq 11 && $LAUNCHES -eq 8 ]] \
   && [[ "$(sort "$OUT/crash-retries.txt")" == "$(printf 'rowa__canon\t134\tpass\n%s__canon\t134\tpass\n' "$FSLUG" | sort)" ]] \
   && [[ $(wc -l <"$OUT/crashes.txt") -eq 2 ]] && ! grep -q '\.retry' "$OUT/crashes.txt" \
   && grep -q '^  CRASHED rowa__canon rc=134 retry=pass$' "$LOG" \
   && grep -qx 'retry=pass' "$(cut -f3 "$OUT/crashes.txt" | head -1)/rc.txt"; then
  pass "once: both corpora pass, 11 tiles, 8 launches (6 legs + 2 retries), 2 crashes recorded retry=pass"
else fail once "brc=$BRC frc=$FRC tiles=$tiles launches=$LAUNCHES; $(cat "$OUT/crash-retries.txt" 2>/dev/null); $(tail -20 "$LOG")"; fi
if [[ $(grep -c '^CRASHED .* rc=134 retry=pass ' "$W/once/stream") -eq 2 \
   && $(grep -c 'rowa__canon__' "$W/once/stream") -eq 3 \
   && -z "$(awk '{ print $1, $2 }' "$W/once/stream" | sort | uniq -d)" ]] && ! grep -q '^\[stream\] FAIL' "$LOG"; then
  pass "once: stream carries 2 CRASHED lines and each of the 3 bk2 canon tiles once"
else fail "once stream" "$(cat "$W/once/stream")"; fi
lines="$(pg_crash_list "crashed-leg: " "$OUT")"
if [[ $(grep -c ' rc=134 .* retry=pass$' <<<"$lines") -eq 2 && -z "$(pg_retry_failed "$OUT")" ]]; then
  pass "once: report lines end retry=pass, no failed retry"
else fail "once report" "$lines"; fi
rec1="$(record once)"

# Determinism: the same case again gives the same record.
rm -rf "${W:?}/once"
run_case once once __canon__
rec2="$(record once)"
[[ "$rec1" == "$rec2" ]] && pass "deterministic: a second once run gives the same record" \
  || fail deterministic "$(diff <(echo "$rec1") <(echo "$rec2"))"

# always: the canon legs abort on every launch, before writing a tile.
run_case always always __canon__
tiles=$(ls "$OUT"/*-vk.png 2>/dev/null | wc -l)
if [[ $BRC -eq 1 && $FRC -eq 1 && $tiles -eq 7 && $LAUNCHES -eq 8 \
   && $(grep -c '\.retry' "$OUT/crashes.txt") -eq 2 \
   && "$(pg_retry_failed "$OUT" | sort)" == "$(printf '%s__canon\nrowa__canon' "$FSLUG")" ]] \
   && grep -q '^  CRASHED rowa__canon rc=134 retry=fail$' "$LOG" \
   && [[ $(grep -c '^CRASHED .* rc=134 retry=fail ' "$W/always/stream") -eq 2 ]]; then
  pass "always: both corpora fail, 7 tiles (the 4 canon tiles absent), legs and .retry legs kept, retry=fail named"
else fail always "brc=$BRC frc=$FRC tiles=$tiles launches=$LAUNCHES; $(cat "$OUT/crashes.txt" 2>/dev/null); $(tail -20 "$LOG")"; fi

# exit1: a nonzero exit without a signal is kept and not retried.
run_case exit1 exit1 __canon__
if [[ $BRC -eq 1 && $FRC -eq 1 && $LAUNCHES -eq 6 && $(wc -l <"$OUT/crashes.txt") -eq 2 \
   && ! -s "$OUT/crash-retries.txt" ]] && ! grep -q 'retry=' <<<"$(pg_crash_list "" "$OUT")" \
   && ! grep -q '^CRASHED' "$W/exit1/stream"; then
  pass "exit1: kept, not retried (6 launches, no retry row, no CRASHED line)"
else fail exit1 "brc=$BRC frc=$FRC launches=$LAUNCHES; $(cat "$OUT/crashes.txt" 2>/dev/null)"; fi

if [[ $fails -ne 0 ]]; then echo "[leg-retry] FAIL: $fails case(s)"; exit 1; fi
echo "[leg-retry] PASS: every case"
exit 0
