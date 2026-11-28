#!/usr/bin/env bash
# check_asan.sh - AddressSanitizer run of the bus section's mutation legs in
# the clang-19 ASan tree (build/linux-asan-clang: the linux-release
# configuration with every TU, the generated recomp modules, SDL and
# PsyCross built -fsanitize=address).
#
# Runs tests/harness/check_mods.sh --section bus with BUS_LEGS=mutation
# against the ASan binary, the fixture mods built -fsanitize=address through
# tests/harness/mod-sanitize-cc. In those legs a member is hot reloaded, hot
# reloaded with a failing entry, and first enabled while another member's
# handler has the fiber parked inside a walk; every bus-member handler
# writes a heap block its object's shutdown hook frees, so a handler entered
# after its shutdown hook is a heap-use-after-free report. The legs' own
# PASS lines are the engagement (each names the member counts per walk that
# show the parked walk skipped the retired row). OPENPETE_ASAN_BUS_LEGS
# selects other bus groups (core park rewind mutation late).
#
# Reports go to OUT/asan.<pid>; leak detection is off (the bus snapshots
# and tables are kept for the process by design). The symbolizer runs
# through tests/harness/tsan/llvm-symbolizer for the reason given there.
#
# Usage:
#   tests/harness/check_asan.sh [--build-dir=DIR] [--out DIR]
#   tests/harness/check_asan.sh --build-only [--build-dir=DIR]
# --build-only configures DIR when it has no build.ninja and builds the
# openpete target and the mod SDK it stages (openpete_sdk_stage, which
# the fixtures compile against) under nice 10 with OPENPETE_ASAN_JOBS jobs (default: the
# CPU count); a run never builds.
# Exit 0 PASS: no report and the legs ALL GREEN. 1 FAIL: a report, a failed
# leg, or a binary that is not ASan-instrumented. 64 SKIP: no clang-19, no
# binary or no disc.
set -u
set -o pipefail

# Unattended launcher: every run this script starts is silenced.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
ROOT="$PWD"
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-asan-clang
OUT=""
BUILD_ONLY=0
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
        --build-only)  BUILD_ONLY=1;;
    esac
    [[ "$prev" == --out ]] && OUT="$a"
    prev="$a"
done
TAG=asan

if [[ "$BUILD_ONLY" == 1 ]]; then
    for c in clang-19 clang++-19 ninja; do
        command -v "$c" >/dev/null 2>&1 || { echo "$TAG: SKIP (no $c)"; exit 64; }
    done
    F="-fsanitize=address -fno-omit-frame-pointer -g"
    if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
        cmake -S . -B "$BUILD_DIR" -G Ninja \
            -DCMAKE_PROJECT_openpete_INCLUDE="$ROOT/cmake/presets/linux-release.cmake" \
            -DCMAKE_C_COMPILER=clang-19 -DCMAKE_CXX_COMPILER=clang++-19 \
            -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F -Wno-c++11-narrowing" \
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address \
            -DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address \
            -DCMAKE_MODULE_LINKER_FLAGS=-fsanitize=address ||
            { echo "$TAG: FAIL (configure of $BUILD_DIR)"; exit 1; }
    fi
    nice -n 10 cmake --build "$BUILD_DIR" -j"${OPENPETE_ASAN_JOBS:-$(nproc)}" --target openpete openpete_sdk_stage ||
        { echo "$TAG: FAIL (build of $BUILD_DIR)"; exit 1; }
    echo "$TAG: built $BUILD_DIR/openpete"
    exit 0
fi

PG_BUILD_DIR="$BUILD_DIR"
pg_resolve spyro-1 || exit 64
[[ -x "$PG_BIN"  ]] || { echo "$TAG: SKIP (no binary at $PG_BIN; run --build-only)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "$TAG: SKIP (no disc at $PG_DISC)"; exit 64; }
command -v clang-19 >/dev/null 2>&1 || { echo "$TAG: SKIP (no clang-19)"; exit 64; }
# An uninstrumented binary runs clean and reports nothing: refuse it.
nm --defined-only "$PG_BIN" 2>/dev/null | grep ' __asan_init$' >/dev/null ||
    { echo "$TAG: FAIL ($PG_BIN is not built with -fsanitize=address)"; exit 1; }

[[ -n "$OUT" ]] || OUT="$OPENPETE_SANDBOX/$TAG"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
LOG="$OUT/check_mods.log"
rm -f "$LOG" "$OUT"/asan.* "$OUT/symbolizer.err"

OPENPETE_BIN="$PG_BIN" OPENPETE_MOD_CC="$ROOT/tests/harness/mod-sanitize-cc" OPENPETE_MOD_SANITIZE=address \
    BUS_LEGS="${OPENPETE_ASAN_BUS_LEGS:-mutation}" \
    OPENPETE_TSAN_SYMBOLIZER=llvm-symbolizer-19 OPENPETE_TSAN_SYMBOLIZER_LOG="$OUT/symbolizer.err" \
    ASAN_OPTIONS="log_path=$OUT/asan detect_leaks=0 external_symbolizer_path=$ROOT/tests/harness/tsan/llvm-symbolizer" \
    tests/harness/check_mods.sh --section bus > "$LOG" 2>&1
rc=$?
grep '^\[modtest\]' "$LOG"

shopt -s nullglob
reports=0
for f in "$OUT"/asan.*; do
    reports=$(( reports + $(grep -c '^==[0-9]*==ERROR: AddressSanitizer' "$f") ))
done
shopt -u nullglob
(( reports == 0 )) || { echo "$TAG: FAIL ($reports report(s) in $OUT/asan.*)"; exit 1; }
[[ "$rc" == 0 ]] || { echo "$TAG: FAIL (a leg failed; see $LOG)"; exit 1; }
echo "$TAG: PASS (bus legs ${OPENPETE_ASAN_BUS_LEGS:-mutation}, 0 reports)"
exit 0
