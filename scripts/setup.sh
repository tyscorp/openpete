#!/usr/bin/env bash
# scripts/setup.sh - first-time setup check for OpenPete.
#
# Arms the tracked git hooks and reports:
#   1. The submodules the configure reads (hard error when missing).
#   2. Per game: the decomp submodule, its nested maspsx (hard error when
#      missing), its build outputs (psx.map drives the recompiler manifest),
#      and the PSYQ SDK headers its build compiles against.
#   3. The disc image the development scripts run the game with. The
#      configure and build do not read it.
#
# The engine (openpete_core, openpete_rhi, the unit tests) needs none of the
# per-game inputs. The PSYQ SDK is a user-supplied prerequisite of a game
# build: it is Sony proprietary and is never redistributed with this project,
# so this script detects the headers' absence and says what to place where;
# it does not obtain them.
#
# Returns exit code 3 (OPENPETE_EXIT_SETUP_ERROR) on a hard error.

set -uo pipefail

OPENPETE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Dev default: the disc rebuilt from the decomp output (override with
# OPENPETE_DISC). The rebuild is not byte-identical to the Redump image, so it
# is checked for presence only; the library labels the Redump SHA256 at
# ingest (core/launcher/library.cpp, the known-disc table in
# games/spyro-1/game_extras_impl.c).
DISC_BIN_DEFAULT="$OPENPETE_ROOT/games/spyro-1/spyro-1/build/SCUS-94228.bin"

DISC_BIN="${OPENPETE_DISC:-$DISC_BIN_DEFAULT}"

red()    { printf '\033[31m%s\033[0m\n' "$*"; }
green()  { printf '\033[32m%s\033[0m\n' "$*"; }
yellow() { printf '\033[33m%s\033[0m\n' "$*"; }

errs=0

echo "==> Disc image"
if [[ ! -f "$DISC_BIN" ]]; then
    yellow "  missing: $DISC_BIN"
    yellow "    Running the game needs a disc image made from your own copy;"
    yellow "    place it at that path or set OPENPETE_DISC=/path/to/SCUS-94228.bin."
    yellow "    The configure and build do not read it."
else
    green "  ok: $DISC_BIN"
fi

echo
echo "==> Submodules the configure reads"
required=(
    third_party/PsyCross
    third_party/SDL
    third_party/glslang
    third_party/imgui
)
for path in "${required[@]}"; do
    if [[ -d "$OPENPETE_ROOT/$path" ]] && [[ -n "$(ls -A "$OPENPETE_ROOT/$path" 2>/dev/null)" ]]; then
        green "  ok: $path"
    else
        red "  missing or empty: $path  (run: git submodule update --init --recursive -- $path)"
        errs=$((errs+1))
    fi
done

# Per game: the decomp checkout, its build command, and the artifacts the
# recompiler manifest is generated from. A game whose submodule is absent is
# reported and skipped rather than failing the run, so a spyro-1-only tree
# still passes.
check_decomp() {
    local game="$1" build_cmd="$2"
    local root="$OPENPETE_ROOT/games/$game/$game"

    echo
    echo "==> $game matching-decomp build artifacts"
    if [[ ! -d "$root" ]] || [[ -z "$(ls -A "$root" 2>/dev/null)" ]]; then
        yellow "  submodule not checked out: games/$game/$game"
        yellow "    git submodule update --init --recursive -- games/$game/$game"
        return
    fi

    # maspsx rewrites the compiler output before the assembler sees it. Both
    # decomps invoke it in a pipeline whose exit status is the assembler's, so
    # an uninitialized maspsx yields empty assembly, zero-byte objects, and a
    # build that reports success until the link finds symbols missing.
    if [[ -f "$root/.gitmodules" ]] && grep -q maspsx "$root/.gitmodules" 2>/dev/null; then
        if [[ -f "$root/tools/maspsx/maspsx.py" ]]; then
            green "  ok: tools/maspsx"
        else
            red "  missing: games/$game/$game/tools/maspsx (nested submodule)"
            red "    git -C games/$game/$game submodule update --init tools/maspsx"
            red "    Without it the build appears to succeed and produces empty objects."
            errs=$((errs+1))
        fi
    fi

    local map="$root/build/psx.map"
    local exe="$root/build/PSX.EXE"
    if [[ -f "$map" ]]; then
        green "  ok: games/$game/$game/build/psx.map"
    else
        yellow "  missing: games/$game/$game/build/psx.map"
        yellow "    cd games/$game/$game && $build_cmd"
    fi
    if [[ -f "$exe" ]]; then
        green "  ok: games/$game/$game/build/PSX.EXE"
    else
        yellow "  missing: games/$game/$game/build/PSX.EXE"
    fi
}

# The Spyro 1 decomp compiles against the PSYQ 4.0 headers and Spyro 2 against
# 4.7. They are Sony proprietary and not distributable, so neither decomp
# builds from a clean clone without a copy the developer supplies. Detect and
# diagnose; never fetch.
check_psyq_headers() {
    local game="$1" version="$2"
    local inc="$OPENPETE_ROOT/games/$game/$game/psyq/include"

    [[ -n "$(ls -A "$OPENPETE_ROOT/games/$game/$game" 2>/dev/null)" ]] || return
    if [[ -f "$inc/libgpu.h" ]]; then
        green "  ok: games/$game/$game/psyq/include (PSYQ $version headers)"
    else
        yellow "  missing: games/$game/$game/psyq/include (PSYQ $version headers)"
        yellow "    Supply your own copy of the PSYQ $version SDK; it is Sony"
        yellow "    proprietary and is not distributed with this project."
        yellow "    Extract the SDK's include/ there, lowercasing filenames and"
        yellow "    keeping the sys/ subdirectory: the sources include lowercase"
        yellow "    names, the archive ships uppercase."
    fi
}

# The tracked pre-commit hook (the complexity and documentation ratchets) runs only
# when this clone's core.hooksPath names githooks/, and the ratchet baselines
# merge by row key only when the `ratchet-tsv` merge driver .gitattributes
# names is defined; both settings are local config and a fresh clone carries
# neither. The build refuses an unarmed clone.
echo
echo "==> git hooks"
if git -C "$OPENPETE_ROOT" rev-parse --git-dir >/dev/null 2>&1; then
    git -C "$OPENPETE_ROOT" config core.hooksPath githooks
    green "  ok: core.hooksPath = githooks"
    git -C "$OPENPETE_ROOT" config merge.ratchet-tsv.driver \
        'python3 scripts/fences/ratchet_tsv_merge.py %O %A %B %P'
    green "  ok: merge.ratchet-tsv.driver = scripts/fences/ratchet_tsv_merge.py"
else
    yellow "  not a git checkout; hooks not armed"
fi

check_decomp spyro-1 "make all -j\"\$(nproc)\""
check_psyq_headers spyro-1 4.0
# `make` alone builds a single object and exits 0 - the Makefile's default goal
# is one .o, not the game. The target that builds everything is `all`.
check_decomp spyro-2 "make all -j\"\$(nproc)\""
check_psyq_headers spyro-2 4.7
# build.py verifies its own output with hashlib.file_digest, which needs Python
# 3.11; on an older interpreter the build completes and then the verifier
# raises AttributeError. Check the output with sha256sum instead, remapping the
# `build.wad/` entries in sha256sum.txt onto the `build/wad/` the build writes.
check_decomp spyro-3 "python3 ./build_multiproc.py"

echo
if [[ $errs -gt 0 ]]; then
    red "setup: $errs hard error(s)"
    exit 3
fi
green "setup: ok"
