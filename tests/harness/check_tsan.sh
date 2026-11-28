#!/usr/bin/env bash
# check_tsan.sh - ThreadSanitizer run of one game in the clang-19 TSan tree
# (build/linux-tsan-clang: the linux-release configuration with every TU,
# the generated recomp modules, SDL and PsyCross built -fsanitize=thread).
#
# Headless (default): the freecam row of check_moby_pose_cache.sh at extended
# LOD with sixteen sub-tick re-extracts at each of two VBLs and every fan-out
# bound at 1, so the moby walk, simple-model replay and emit fan-outs, the
# terrain sector walk and the pose cache run on the work pool on every walk
# with two runs. The fan-out identity checks run in the same process
# (OPENPETE_MOBY_PUSH_FANOUT_CHECK, OPENPETE_MOBY_SIMPLE_FANOUT_CHECK,
# OPENPETE_MOBY_POSE_CACHE_REPORT) and are the engagement counters.
#
# --windowed: the same row launched windowed on a private Xvfb display
# (scripts/lib/xvfb.sh) with --input-lock --vibration 0 and no screenshot
# target, so eager production runs the present thread (openpete-present) and
# the thread split runs the sim on openpete-sim beside main's host-loop
# pump. Engagement: the present thread's start line and presented-scene
# count, the split line, and the pace log's host-pump iterations.
#
# The stderr log mirror stays on (core/diag/log_file.c). TSan starts its
# symbolizer as a child that inherits fd 2, which the mirror has made the
# write end of its pump pipe; the pump's EOF at exit then never comes and
# the process hangs in the pump join. The symbolizer is launched through
# tests/harness/tsan/llvm-symbolizer, which points the child's fd 2 at
# OUT/<game>.symbolizer.err before it execs the real one.
#
# --mods (Spyro 1): a headless attract-demo run with three copies of the
# bus-member fixture, built -fsanitize=thread through
# tests/harness/mod-sanitize-cc: m1 sends tick/1 on every GamestateDraw
# call, m2's reader thread polls bus_members and mod_info until shutdown,
# and m3 is toggled off, hot reloaded through a config Apply and toggled on,
# and m1 toggled off and on, so the membership snapshot is published while
# the reader reads it. Engagement: the reader's poll count and the records
# it matched, m3's reload, and the walker's handler-run count.
# OPENPETE_TSAN_MODS_PLANT=1 also sets m2's plant_race, a write of one plain
# global from the reader thread and the sim thread: the run must then FAIL
# with a report in bus_member.c (the mode's positive control).
#
# Reports go to OUT/<game>[-windowed|-mods].tsan.<pid>; suppressions are
# tests/harness/tsan/suppressions.txt (library-internal races only, each
# read); tests/harness/tsan/summarize.py sorts them by module.
#
# Usage:
#   tests/harness/check_tsan.sh [--game SLUG] [--windowed | --mods] [--build-dir=DIR] [--out DIR]
#   tests/harness/check_tsan.sh --build-only [--build-dir=DIR]
# --build-only configures DIR when it has no build.ninja and builds the
# openpete target and the mod SDK it stages (openpete_sdk_stage, which
# the --mods fixtures compile against) under nice 10 with OPENPETE_TSAN_JOBS jobs (default: the
# CPU count); a run never builds.
# Exit 0 PASS: no report, the run exited 0 inside its time limit, and every
# engagement counter above zero with no identity mismatch. 1 FAIL: a TSan
# report, a mismatch, a timeout or another exit status, a missing total, or
# a binary that is not TSan-instrumented. 2 VACUOUS: a counter at zero.
# 64 SKIP: no clang-19, no binary, no disc, or no xvfb-run (--windowed).
set -u
set -o pipefail

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
ROOT="$PWD"
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-tsan-clang
OUT=""
WINDOWED=0
MODS=0
BUILD_ONLY=0
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --windowed)    WINDOWED=1;;
        --mods)        MODS=1;;
        --build-only)  BUILD_ONLY=1;;
    esac
    [[ "$prev" == --out ]] && OUT="$a"
    prev="$a"
done
TAG=tsan
[[ "$WINDOWED" == 1 ]] && TAG=tsan-windowed
[[ "$MODS" == 1 ]] && TAG=tsan-mods

if [[ "$BUILD_ONLY" == 1 ]]; then
    for c in clang-19 clang++-19 ninja; do
        command -v "$c" >/dev/null 2>&1 || { echo "$TAG: SKIP (no $c)"; exit 64; }
    done
    F="-fsanitize=thread -fno-omit-frame-pointer -g"
    if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
        cmake -S . -B "$BUILD_DIR" -G Ninja \
            -DCMAKE_PROJECT_openpete_INCLUDE="$ROOT/cmake/presets/linux-release.cmake" \
            -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
            -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F -Wno-c++11-narrowing" \
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=thread \
            -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=thread \
            -DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=thread ||
            { echo "$TAG: FAIL (configure of $BUILD_DIR)"; exit 1; }
    fi
    nice -n 10 cmake --build "$BUILD_DIR" -j"${OPENPETE_TSAN_JOBS:-$(nproc)}" --target openpete openpete_sdk_stage ||
        { echo "$TAG: FAIL (build of $BUILD_DIR)"; exit 1; }
    echo "$TAG: built $BUILD_DIR/openpete"
    exit 0
fi

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64
[[ -x "$PG_BIN"  ]] || { echo "$TAG: SKIP (no binary at $PG_BIN; run --build-only)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "$TAG: SKIP (no disc at $PG_DISC)"; exit 64; }
SYMB=llvm-symbolizer-19
command -v "$SYMB" >/dev/null 2>&1 || { echo "$TAG: SKIP (no $SYMB)"; exit 64; }
# An uninstrumented binary runs clean and reports nothing: refuse it.
# grep reads all of nm's output: an early exit would fail the pipeline.
nm --defined-only "$PG_BIN" 2>/dev/null | grep ' __tsan_init$' >/dev/null ||
    { echo "$TAG: FAIL ($PG_BIN is not built with -fsanitize=thread)"; exit 1; }
if [[ "$WINDOWED" == 1 ]]; then
    # shellcheck source=scripts/lib/xvfb.sh
    source scripts/lib/xvfb.sh
    op_xvfb_prefix "$TAG" 1280x960
fi

if [[ "$MODS" == 1 && "$GAME" != spyro-1 ]]; then echo "$TAG: SKIP (the bus fixtures are Spyro 1's)"; exit 64; fi
case "$GAME" in
    spyro-1) ROW=(--skip-to-level stone_hill --freecam-pose topdown);;
    spyro-2) ROW=(--skip-to-level skelos_badlands --freecam-pose "pos=(116034, 2704, 95746) yaw=1112 pitch=541");;
    spyro-3) ROW=(--skip-to-level sunrise_spring --freecam-pose topdown);;
    *) echo "$TAG: SKIP (no row for $GAME)"; exit 64;;
esac
VBLS=3000,3020
LIMIT=3030
RUN_TIMEOUT=900

[[ -n "$OUT" ]] || OUT="$OPENPETE_SANDBOX/$TAG"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
STEM="$OUT/$GAME"
[[ "$WINDOWED" == 1 ]] && STEM="$OUT/$GAME-windowed"
[[ "$MODS" == 1 ]] && STEM="$OUT/$GAME-mods"
LOG="$STEM.log"
PACE="$STEM.pace"
rm -f "$LOG" "$PACE" "$STEM.symbolizer.err" "$STEM".tsan.* "${STEM}__"*.png

ENV=(OPENPETE_MOBY_PUSH_FANOUT_MIN=1 OPENPETE_MOBY_SIMPLE_FANOUT_MIN=1 OPENPETE_MOBY_FANOUT_MIN=1
     OPENPETE_MOBY_PUSH_FANOUT_CHECK=1 OPENPETE_MOBY_SIMPLE_FANOUT_CHECK=1 OPENPETE_MOBY_POSE_CACHE_REPORT=1
     "OPENPETE_TSAN_SYMBOLIZER=$SYMB" "OPENPETE_TSAN_SYMBOLIZER_LOG=$STEM.symbolizer.err"
     "TSAN_OPTIONS=log_path=$STEM.tsan suppressions=$ROOT/tests/harness/tsan/suppressions.txt external_symbolizer_path=$ROOT/tests/harness/tsan/llvm-symbolizer history_size=4 second_deadlock_stack=1 halt_on_error=0 report_signal_unsafe=0")

rc=0
if [[ "$MODS" == 1 ]]; then
    MD="$STEM.modsdir"
    rm -rf "${MD:?}"
    mkdir -p "$MD"
    : > "$MD/enabled.txt"
    for id in m1 m2 m3; do
        cp -r mods/bus-member "$MD/$id"
        rm -rf "${MD:?}/$id/.build" "${MD:?}/$id/data"
        sed -i "s/^id *= *\"bus-member\"/id      = \"$id\"/" "$MD/$id/mod.toml"
        echo "$id" >> "$MD/enabled.txt"
    done
    echo 'every_tick = true' > "$MD/m1/config.toml"
    printf 'reader = true\n' > "$MD/m2/config.toml"
    [[ "${OPENPETE_TSAN_MODS_PLANT:-0}" == 1 ]] && echo 'plant_race = true' >> "$MD/m2/config.toml"
    # m3's GamestateDraw link encloses the fiber's park, which defers its
    # reload: it is toggled off around the Apply.
    PG_CAPTURE_ENV=("${ENV[@]}" "OPENPETE_MOD_CC=$ROOT/tests/harness/mod-sanitize-cc" OPENPETE_MOD_SANITIZE=thread
                    OPENPETE_MODS_TOGGLE='100:m3:0,300:m3:1,400:m1:0,500:m1:1'
                    OPENPETE_MODS_CFG_APPLY='200:m3:gen=1')
    PG_CAPTURE_TIMEOUT=$RUN_TIMEOUT
    pg_capture "$LOG" "" -- --mods-dir "$MD" --no-card --mute 1 --frame-limit 1200
    rc=$?
elif [[ "$WINDOWED" == 0 ]]; then
    PG_CAPTURE_ENV=("${ENV[@]}" OPENPETE_INTERMEDIATE_ALPHA=0.5
                    OPENPETE_INTERMEDIATE_PRE_ALPHA=0.06,0.12,0.18,0.25,0.31,0.37,0.43,0.56,0.62,0.68,0.75,0.81,0.87,0.93,0.97)
    PG_CAPTURE_TIMEOUT=$RUN_TIMEOUT
    pg_capture "$LOG" "${STEM}__${VBLS%,*}-vk.png,${STEM}__${VBLS#*,}-vk.png" -- \
        --renderer vk --aspect 16:9 --lod extended --no-card --mute 1 "${ROW[@]}" \
        --screenshot-native-only --screenshot-intermediate-on-frame "$VBLS" \
        --screenshot-output "${STEM}__{n}.png" --frame-limit "$LIMIT"
    rc=$?
else
    run_root="$(op_sandbox_run_root)" || { echo "$TAG: FAIL (no sandbox root)"; exit 1; }
    op_sandbox_xdg "$run_root" || { echo "$TAG: FAIL (no sandbox XDG)"; exit 1; }
    op_own_run env "OPENPETE_SANDBOX=$run_root" "OPENPETE_CARD_DIR=$run_root/cards" "${OP_SANDBOX_XDG_ENV[@]}" \
        "${ENV[@]}" OPENPETE_PRESENT_TRACE=1 \
        "${OP_XVFB[@]}" timeout -k "$OP_OWN_GRACE" "$RUN_TIMEOUT" "$PG_BIN" \
        --no-config --disc "$PG_DISC" --disc-cache 0 "${PG_RUN_ARGS[@]}" \
        --no-card --mute 1 --vibration 0 --input-lock --background 1 \
        --renderer vk --window 1280x720 --aspect 16:9 --lod extended "${ROW[@]}" \
        --pace-log "$PACE" --frame-limit "$LIMIT" --max-runtime-seconds "$(( RUN_TIMEOUT - 30 ))" \
        </dev/null >"$LOG" 2>&1
    rc=$?
    rm -rf "${run_root:?}"
fi

shopt -s nullglob
reports=0
for f in "$STEM".tsan.*; do
    reports=$(( reports + $(grep -c '^WARNING: ThreadSanitizer: ' "$f") ))
done
shopt -u nullglob
if (( reports > 0 )); then
    python3 tests/harness/tsan/summarize.py "$OUT" --full --prefix "$(basename "$STEM")"
    echo "$TAG: FAIL ($reports report(s) in $STEM.tsan.*)"
    exit 1
fi
# 66 is TSan's exit code when it reported; with no report it is the game's own.
[[ "$rc" == 0 ]] || { echo "$TAG: FAIL (run exited $rc, 124/137 = time limit; see $LOG)"; exit 1; }

fail=0 vac=0
# field LINE NAME: the integer after NAME= in LINE, empty when absent.
field() { sed -n "s/.*[ |]$2=\([0-9]*\).*/\1/p" <<<"$1" | head -n 1; }
if [[ "$MODS" == 1 ]]; then
    reader="$(grep -o 'bus-member m2: reader polls=[0-9]* listed=[0-9]*' "$LOG" | tail -1)"
    runs="$(grep -o 'bus: buses=[0-9]* handler runs=[0-9]*' "$LOG" | tail -1 | sed 's/.*runs=//')"
    reloads=$(grep -c 'hot reload: m3 live' "$LOG")
    toggles=$(grep -c 'live toggle: m[13] ' "$LOG")
    echo "$TAG: $GAME ${reader:-no reader line} handler_runs=${runs:-0} m3_reloads=$reloads toggles=$toggles"
    polls="$(field "$reader" polls)" listed="$(field "$reader" listed)"
    (( ${polls:-0} > 0 && ${listed:-0} > 0 && ${runs:-0} > 0 && reloads > 0 && toggles == 4 )) || vac=1
elif [[ "$WINDOWED" == 0 ]]; then
    push="$(grep -m1 '^\[push-fanout-check final\]' "$LOG")"
    pose="$(grep -m1 '^\[pose-cache\]' "$LOG")"
    [[ -n "$push" && -n "$pose" ]] || { echo "$TAG: FAIL (no push-fanout or pose-cache total in $LOG)"; exit 1; }
    echo "$TAG: $GAME ${push%% |*}"
    echo "$TAG: $GAME $pose"
    [[ "$(field "$push" mismatched_tris)" == 0 && "$(field "$push" mismatched_state)" == 0 ]] || fail=1
    [[ "$(field "$pose" mismatches)" == 0 ]] || fail=1
    emits="$(field "$push" emits)" hits="$(field "$pose" hits)"
    (( ${emits:-0} > 0 && ${hits:-0} > 0 )) || vac=1
    if [[ "$GAME" != spyro-1 ]]; then
        # Spyro 1 has no simple-model decode.
        simple="$(grep -m1 '^\[simple-fanout-check final' "$LOG")"
        [[ -n "$simple" ]] || { echo "$TAG: FAIL (no simple-fanout total in $LOG)"; exit 1; }
        echo "$TAG: $GAME $simple"
        bad="$(grep -oE 'mismatches=[0-9]+' <<<"$simple" | grep -vc '=0$')"
        [[ "$bad" == 0 ]] || fail=1
        decodes="$(field "$simple" decodes)"
        (( ${decodes:-0} > 0 )) || vac=1
    fi
else
    started=$(grep -c 'present-thread.*started: eager' "$LOG")
    split=$(grep -c 'thread split: main owns window+pump' "$LOG")
    presented=$(grep -oE '[0-9]+ scene\(s\) presented' "$LOG" | tail -n 1 | grep -oE '^[0-9]+')
    pump=$(python3 -c 'import sys, pathlib; sys.path.insert(0, sys.argv[1]); import pacelog
log = pacelog.read(pathlib.Path(sys.argv[2]))
print(sum(r.hp_iters for r in log.records if r.type == pacelog.HOST_PUMP and r.hp_summary))' \
        scripts/pacing "$PACE" 2>/dev/null)
    echo "$TAG: $GAME present_thread_started=$started thread_split=$split scenes_presented=${presented:-0} pump_iters=${pump:-0}"
    (( started > 0 && split > 0 && ${presented:-0} > 0 && ${pump:-0} > 0 )) || vac=1
fi
[[ "$fail" == 0 ]] || { echo "$TAG: FAIL (identity mismatch)"; exit 1; }
[[ "$vac" == 0 ]] || { echo "$TAG: VACUOUS (an engagement counter is zero)"; exit 2; }
echo "$TAG: PASS ($GAME, 0 reports)"
exit 0
