#!/usr/bin/env bash
# regen-recomp.sh - re-run psxrecomp on one game's PSX.EXE plus every
# overlay in its matching-decomp's build/wad/. Run whenever you bump
# psxrecomp, change a forced-entry, or pull a new EXE dump.
#
# Which game is selected by GAME= (default spyro-1); its paths, overlay
# vaddr and options come from games/<game>/recomp.conf.
#
#   GAME=spyro-2 bash scripts/regen-recomp.sh [--busy-ok "<reason>"]
#
# Before anything is written, scripts/lib/regen_guard.py refuses a checkout
# whose outputs are links into the main checkout (a worktree made without
# `worktree-setup.sh --no-links`; exit 65), and, in the main checkout, a run
# while a worktree linking this game's generated tree builds or a fleet run
# from it or from this checkout is in flight (exit 75; --busy-ok overrides).
#
# Inputs (relative to repo root):
#   third_party/psxrecomp/build/PSXRecomp   - recompiler binary
#   games/<game>/recomp.conf                - per-game paths and addresses
#   $PSX_EXE                                - main EXE input
#   $WAD_DIR/*.ovl                          - overlay binaries
#   games/<game>/manifest/psyq.toml         - PSYQ symbol manifest
#
# All of a game's overlays share one kseg0 vaddr - only one is in RAM at a
# time, so per-overlay symbol namespaces are disambiguated via
# --func-prefix. The generated dispatch fn is sed-renamed from
# `psx_dispatch_compiled` to `psx_dispatch_<name>` so each overlay can
# coexist at link.
#
# Outputs (overwritten), all under games/<game>/generated/:
#   PSX.EXE_full.c
#   PSX.EXE_dispatch.c
#   discovered_functions.log
#   <name>/<name>.bin_full.c
#   <name>/<name>.bin_dispatch.c
#   <name>/jumptable_targets.txt
#   spyro_overlay_table.c            (auto table)

set -euo pipefail

cd "$(dirname "$0")/.."

GAME="${GAME:-spyro-1}"
BUSY_OK=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --busy-ok)
            [[ $# -ge 2 && -n "${2// /}" ]] || { echo "regen-recomp: --busy-ok takes a reason" >&2; exit 64; }
            BUSY_OK=(--busy-ok "$2"); shift 2 ;;
        *) echo "regen-recomp: unknown argument $1" >&2; exit 64 ;;
    esac
done
python3 scripts/lib/regen_guard.py check --tool regen-recomp --game "$GAME" "${BUSY_OK[@]}"
GAME_CONF="games/$GAME/recomp.conf"
if [[ ! -f "$GAME_CONF" ]]; then
    echo "regen-recomp: no $GAME_CONF - is GAME=$GAME right?" >&2
    exit 1
fi
# shellcheck source=/dev/null
source "$GAME_CONF"
for v in DECOMP_DIR PSX_EXE WAD_DIR ASM_DIR OVERLAY_LOAD_ADDR OVERLAY_RANGE; do
    [[ -n "${!v:-}" ]] || { echo "regen-recomp: $GAME_CONF sets no $v" >&2; exit 1; }
done
echo "[regen] game=$GAME  exe=$PSX_EXE  overlays=$WAD_DIR @ $OVERLAY_LOAD_ADDR"

PSXRECOMP="third_party/psxrecomp/build/PSXRecomp"
if [[ ! -f third_party/psxrecomp/build/CMakeCache.txt ]]; then
    echo "psxrecomp not configured at third_party/psxrecomp/build - run cmake on third_party/psxrecomp first" >&2
    exit 1
fi
# Translate with a binary built from the recompiler source in this tree: a
# binary older than its source emits C the current recompiler would not.
echo "[regen] building psxrecomp from source"
cmake --build third_party/psxrecomp/build --target PSXRecomp
if [[ ! -x "$PSXRECOMP" ]]; then
    echo "psxrecomp not found at $PSXRECOMP after the build" >&2
    exit 1
fi

# The recompiler reports every `jr <reg>` it rewrites to `return` because the
# register reloads a spilled $ra (`ra-spill return: jr $t7 at 0x...`). Each
# binary is translated more than once, so the sites are collected and
# deduplicated, then compared with RA_SPILL_RETURNS from recomp.conf (spyro-1
# 0, spyro-2 1, spyro-3 1): a detector regression silently re-emits the miss
# path, which returns v0=v1=0 and yields inside the guest function.
RA_SPILL_LOG="$(mktemp)"
trap 'rm -f "$RA_SPILL_LOG"' EXIT
# SONY_IMAGE_WINDOW from recomp.conf applies to every binary of the game.
run_psxrecomp() {
    "$PSXRECOMP" "$@" ${SONY_IMAGE_WINDOW:+--sony-image-window="$SONY_IMAGE_WINDOW"} \
        | tee -a "$RA_SPILL_LOG"
    return "${PIPESTATUS[0]}"
}

# Optional debug build mode: emits `cpu->pc = X;` before each MIPS insn
# so write-watchers / state-diff harness can attribute every memory
# write to a specific instruction. Adds one store per emitted insn -
# slows runtime ~5-10% and bloats the generated C, so off by default.
PCTRACK_FLAG=""
if [[ "${OPENPETE_REGEN_PC_TRACKING:-0}" != "0" ]]; then
    PCTRACK_FLAG="--emit-pc-tracking"
    echo "[regen] PC tracking ENABLED (OPENPETE_REGEN_PC_TRACKING set)"
fi

OUT="games/$GAME/generated"
JTBL_TOOL="tools/extract_jumptable_targets.py"
FNPTR_TOOL="tools/extract_overlay_func_pointers.py"
PSYQ_MANIFEST="games/$GAME/manifest/psyq.toml"
ASM_TARGETS_DIR="games/$GAME/manifest/asm_dispatch_targets"
FORCED_ENTRIES_DIR="games/$GAME/manifest/forced_entries"
CHANNEL_SITES="$OUT/channel_sites.txt"

mkdir -p "$OUT"

# The stamp names the recompiler source this generated tree came from
# (scripts/fences/recomp_stamp.py; the build refuses a missing or different
# one). A full regen removes it now and writes it only after every step
# passed, so a failed regen leaves the game unstamped. A scoped regen
# re-translates part of the tree and keeps a stamp only when it already
# names the current source.
STAMP_FILE="${OUT:?}/psxrecomp.id"
if [[ "${OPENPETE_REGEN_SCOPE:-full}" == "full" ]]; then
    rm -f "${STAMP_FILE:?}"
elif [[ -f "$STAMP_FILE" && "$(cat "$STAMP_FILE")" != "$(python3 scripts/fences/recomp_stamp.py id)" ]]; then
    rm -f "${STAMP_FILE:?}"
fi

# Render-channel substitution table: generated from the single source of
# truth, games/spyro-1/manifest/render_channels.toml, and passed to the recompiler via
# --channel-sites so the env/moby queue-head substitutions are data-driven
# (no Spyro addresses hardcoded in code_generator.cpp for that shape).
# A game with no render-channel manifest gets no --channel-sites at all:
# that table is a list of one game's addresses, and feeding it to another
# substitutes at addresses that mean something different there.
if [[ -n "${CHANNEL_MANIFEST:-}" ]]; then
    echo "[regen] Generating $CHANNEL_SITES from $CHANNEL_MANIFEST"
    python3 tools/gen_channel_sites.py \
        --manifest "$CHANNEL_MANIFEST" --out "$CHANNEL_SITES"
else
    echo "[regen] no CHANNEL_MANIFEST for $GAME - recompiling without channel sites"
    rm -f "$CHANNEL_SITES"
fi

# Scoped regen: OPENPETE_REGEN_SCOPE=main_exe re-translates only the main
# EXE (the only binary --channel-sites is passed to), reusing the overlay
# outputs + external-JAL lists already on disk from the last full regen -
# a channel-site edit costs one main-EXE recompile, not the full overlay
# sweep.
REGEN_SCOPE="${OPENPETE_REGEN_SCOPE:-full}"
if [[ "$REGEN_SCOPE" == "main_exe" ]]; then
    echo "[regen] SCOPED regen: main EXE only (overlays + table reused from last full regen)"
fi

# PSYQ stubs: weak-linked abort-on-call placeholders for every PSYQ symbol
# in the manifest. The cmake build links these so any psyq_<name> the
# recompiled C declares but psyq_thunks.cpp doesn't override resolves to
# a stub that logs and aborts (rather than failing the link). Same
# `--external-prefix=psyq_` we pass to PSXRecomp below - change both
# together. Inputs are the manifest TOML alone (no recomp output), so
# this is safe to run before the two-pass recompile.
echo "[regen] Generating $OUT/psyq_stubs.c"
python3 tools/gen_psyq_stubs.py --prefix psyq_ \
    "$PSYQ_MANIFEST" "$OUT/psyq_stubs.c" 2>&1 | sed 's/^/  /'

# Refresh the per-binary forced-entry manifests from the current asm
# tree. extract_asm_dispatch_targets.py harvests `glabel D_<addr>`
# jumptable slots; extract_handwritten_ra_returns.py appends return PCs
# for callers of handwritten functions that return via `jr <reg>` (the
# recompiler can't statically prove the target so the return PC has to
# be a known dispatch entry). Order matters - the ra-return tool merges
# into the file the first tool wrote.
echo "[regen] Refreshing $ASM_TARGETS_DIR/"
mkdir -p "$ASM_TARGETS_DIR"
python3 tools/extract_asm_dispatch_targets.py \
    "$ASM_DIR" "$ASM_TARGETS_DIR" >/dev/null
python3 tools/extract_handwritten_ra_returns.py \
    "$ASM_DIR" "$ASM_TARGETS_DIR" >/dev/null

# Resolve the asm_dispatch_targets file for a binary, or empty string if
# none. Overlays share vaddr 0x8007AA38 with different bytes - passing
# main-EXE entries to an overlay regen would silently fail psxrecomp's
# range check (good), but an overlay-A entry passed to overlay-B would
# pass validation and force psxrecomp to translate garbage. Per-binary
# files keep ownership unambiguous.
asm_targets_for() {
    local f="$ASM_TARGETS_DIR/$1.txt"
    [[ -f "$f" ]] && echo "$f"
}

# --- Overlay regen helper -----------------------------------------------

# process_overlay <name> <ovl_path> <prefix> <load_addr> [extra-args...]
#
# Two-pass regen: first translate the synthesized PS1-EXE wrapper to
# discover jump tables, extract those table targets, then re-translate
# with each table entry as a forced-entry. Finally sed-rename the
# dispatch fn so it can coexist with other overlays at link.
process_overlay() {
    local name="$1"; local ovl_path="$2"; local prefix="$3"; local load_addr="$4"
    shift 4
    local extra_args=("$@")
    if [[ ! -f "$ovl_path" ]]; then
        echo "[regen] $ovl_path missing - skipping $name (build the matching decomp first)"
        return 1
    fi
    mkdir -p "$OUT/$name"
    local asm_targets
    asm_targets=$(asm_targets_for "$name")
    local base_args=(
        # Raw overlay blob straight from the decomp's build/wad - the
        # recompiler synthesizes the PS-X EXE header internally
        # (--raw-bin).
        --raw-bin
        --load-addr="$load_addr"
        --output-stem="$name.bin"
        --output-dir="$OUT/$name"
        --external-symbols="$PSYQ_MANIFEST"
        --external-prefix=psyq_
        --func-prefix="$prefix"
        --skip-on-error
        ${PCTRACK_FLAG:+$PCTRACK_FLAG}
    )

    echo "[regen] $name: first-pass recomp"
    # First pass: handwritten-asm dispatch targets + the overlay's own
    # attributed callback entries (jtbl doesn't exist yet). psxrecomp
    # accepts a single --extra-funcs= file.
    local first_funcs="$OUT/$name/extra_funcs.first.txt"
    local fnptr_filtered="$OUT/$name/fn_pointers.txt"
    local curated="$FORCED_ENTRIES_DIR/$name.txt"
    : > "$first_funcs"
    [[ -n "$asm_targets" ]] && cat "$asm_targets" >> "$first_funcs"
    [[ -f "$curated" ]] && grep '^0x' "$curated" >> "$first_funcs"
    [[ -f "$fnptr_filtered" ]] && { grep '^0x' "$fnptr_filtered" || true; } >> "$first_funcs"
    run_psxrecomp "$ovl_path" "${base_args[@]}" \
        --extra-funcs="$first_funcs" "${extra_args[@]}"

    local jtbl="$OUT/$name/jumptable_targets.txt"
    python3 "$JTBL_TOOL" \
        "$OUT/$name/$name.bin_full.c" \
        "$ovl_path" \
        --load-addr="$load_addr" \
        "$jtbl"

    # Second pass: union of asm dispatch + jumptable + fn-pointer entries.
    local second_funcs="$OUT/$name/extra_funcs.second.txt"
    : > "$second_funcs"
    [[ -n "$asm_targets" ]] && cat "$asm_targets" >> "$second_funcs"
    [[ -f "$curated" ]] && grep '^0x' "$curated" >> "$second_funcs"
    [[ -f "$jtbl" ]] && cat "$jtbl" >> "$second_funcs"
    [[ -f "$fnptr_filtered" ]] && { grep '^0x' "$fnptr_filtered" || true; } >> "$second_funcs"
    echo "[regen] $name: second-pass recomp with jumptable forced entries"
    run_psxrecomp "$ovl_path" "${base_args[@]}" \
        --extra-funcs="$second_funcs" \
        --emit-external-jals="$OUT/$name/external_jals.txt" \
        "${extra_args[@]}"

    # Split-target convergence: promote every cross-piece control transfer
    # to a real entry, re-run, repeat until nothing new appears.
    #
    # A guest function often can't be emitted as one C function - an external
    # JAL needs its continuation separately dispatchable, and a forced entry
    # can land inside an existing range. Transfers that cross a piece boundary
    # can only be written as `call_by_address(<target>); return;`, and a target
    # with no registered entry stub-misses at runtime. The stub *returns*
    # rather than transferring control, so the guest epilogue never runs and
    # cpu->sp leaks one stack frame per call until it walks out of RAM
    # (level_52's 0x800809B4, reached by unlocking Lofty Castle's chest, leaked
    # 0x280 bytes a call; level_55's 0x8007E024 is the external-JAL variant).
    #
    # One pass isn't enough: promoting a target splits its containing function,
    # which can expose further cross-piece transfers. The accumulated list is
    # cumulative because a resolved target stops being reported.
    #
    # Invalidate the previous run's combined list: it is only rewritten when
    # THIS run's loop fires, and a stale one poisons the converged-manifest
    # emission (as it did for level_43).
    rm -f "$OUT/$name/extra_funcs.combined.txt"
    local split_found="$OUT/$name/split_targets.txt"
    local split_accum="$OUT/$name/extra_funcs.split.txt"
    local combined="$OUT/$name/extra_funcs.combined.txt"
    local orphan_found="$OUT/$name/orphan_blocks.txt"
    local converged=0 pass
    : > "$split_accum"
    for pass in 1 2 3 4 5; do
        python3 tools/extract_split_targets.py \
            "$OUT/$name/$name.bin_full.c" "$split_found" \
            --binary="$ovl_path" --load-addr="$load_addr" 2>&1 | sed 's/^/  /'
        # Landing pads of a computed `jr`: emitted, but no emitted control
        # flow reaches them. Same failure as an unresolved split target and
        # the same remedy, so they converge in the same loop.
        python3 tools/extract_orphan_blocks.py \
            "$OUT/$name/$name.bin_full.c" "$orphan_found" 2>&1 | sed 's/^/  /'
        if ! grep -qh '^0x' "$split_found" "$orphan_found"; then
            converged=1; break
        fi
        echo "[regen] $name: split-target pass $pass" \
             "($(grep -c '^0x' "$split_found" || true) unresolved," \
             "$(grep -c '^0x' "$orphan_found" || true) orphan)"
        { grep '^0x' "$split_accum" || true
          grep -h '^0x' "$split_found" "$orphan_found" || true; } \
            | sort -u > "$split_accum.tmp"
        mv "$split_accum.tmp" "$split_accum"
        cat "$second_funcs" "$split_accum" > "$combined"
        run_psxrecomp "$ovl_path" "${base_args[@]}" \
            --extra-funcs="$combined" \
            --emit-external-jals="$OUT/$name/external_jals.txt" \
            "${extra_args[@]}"
    done
    # An in-image target we can't resolve is a correctness defect, not a
    # warning: at runtime it stub-misses and leaks a guest stack frame per
    # call. Abort rather than register an overlay that will corrupt RAM.
    if (( converged == 0 )); then
        echo "[regen] ERROR: $name still has unresolved split targets or" \
             "orphan blocks after $((pass)) passes:" >&2
        grep -h '^0x' "$split_found" "$orphan_found" >&2
        exit 1
    fi
    sed -i "s/\bpsx_dispatch_compiled\b/psx_dispatch_${name}/g" \
        "$OUT/$name/$name.bin_dispatch.c"
    local final="$second_funcs"
    [[ -f "$combined" ]] && final="$combined"
    OVERLAY_FINAL[$name]="$ovl_path|$prefix|$load_addr|$final"
    return 0
}

# classify_overlay <name>
#
# Re-runs an overlay's converged final pass with the main EXE's leaf
# summary, so the loop classification (<name>.bin_loop_sites.tsv) counts a
# constant call into a main-EXE pure leaf as a leaf call. The main EXE is
# resident whenever an overlay runs. The pass has the same inputs as the
# converged one plus the summary, so the C it writes differs only by the
# psx_loop_poll calls at the back-edges the summary qualifies; it is the
# overlay's final C. The main EXE's site list names the overlay targets of
# its deferred call-carrying spins; the overlay's dispatch file answers for
# those that are its functions (psx_quiet_<name>). process_overlay's extra
# arguments are not replayed (the registry loop passes none).
classify_overlay() {
    local name="$1"
    local ovl_path prefix load_addr final
    IFS='|' read -r ovl_path prefix load_addr final <<< "${OVERLAY_FINAL[$name]}"
    run_psxrecomp "$ovl_path" \
        --raw-bin \
        --load-addr="$load_addr" \
        --output-stem="$name.bin" \
        --output-dir="$OUT/$name" \
        --external-symbols="$PSYQ_MANIFEST" \
        --external-prefix=psyq_ \
        --func-prefix="$prefix" \
        --skip-on-error \
        ${PCTRACK_FLAG:+$PCTRACK_FLAG} \
        --extra-funcs="$final" \
        --emit-external-jals="$OUT/$name/external_jals.txt" \
        --leaf-summary="$OUT/PSX.EXE_leaves.tsv" \
        --quiet-targets="$OUT/PSX.EXE_loop_sites.tsv" >/dev/null
    sed -i -E "s/\bpsx_(dispatch|quiet)_compiled\b/psx_\1_${name}/g" \
        "$OUT/$name/$name.bin_dispatch.c"
}

# --- Overlay registry ---------------------------------------------------
#
# The overlay id is the LONG at the head of each overlay section, placed
# there by the decomp's overlays.ld. The runtime reads that same word out of
# RAM once an overlay is resident to know whose bytes are live, so reading it
# from the blob here uses the game's own value rather than re-deriving one.
#
# Spyro 1's ids were previously computed from the filename
# (level_<F>_*_code.ovl -> F+11, titlescreen -> 48, credits -> 47). Reading
# the word reproduces all 37 of those exactly, and the arithmetic does not
# generalise: Spyro 2 orders ids independently of its filenames
# (level_30 is id 15 while level_31 is 14, and level_10 is 5).
#
# The section name is the filename stem with any trailing _code dropped,
# which is what the decomp's own overlays.ld calls the section.

OVERLAY_NAMES=()
declare -A OVERLAY_MAGIC=()
declare -A OVERLAY_FINAL=()

register_overlay() {
    OVERLAY_NAMES+=("$1")
    OVERLAY_MAGIC["$1"]="$2"
}

# tools/overlay_registry.py resolves each blob's id (its first word, the value
# the runtime reads back out of RAM) and its linker section name, which is what
# the generated dispatch function and the forced-entry files are keyed on.
OVERLAY_LIST="$OUT/overlay_registry.txt"
python3 tools/overlay_registry.py "$DECOMP_DIR" "$WAD_DIR" > "$OVERLAY_LIST"

# Per-overlay callback entries. The game installs per-level callbacks into
# fixed RAM slots as `lui+addiu` constants and `jalr`s through them; the
# harvester reads the EXE and every blob, keeps the constants whose slot is
# jump-read, and attributes each to the overlay the installing code names:
# a case of the switch keyed on LEVEL_ID_ADDR (the game's level id, from
# recomp.conf) goes to `level_<id>`, an overlay's own store goes to itself,
# and any other cross-binary install is an error. Output:
# $OUT/<overlay>/fn_pointers.txt and $OUT/overlay_fn_report.txt. Runs before
# the overlays so every overlay's first pass has its entries.
echo "[regen] Attributing overlay callback entries from $PSX_EXE and $WAD_DIR"
python3 "$FNPTR_TOOL" "$PSX_EXE" \
    --overlay-range="$OVERLAY_RANGE" \
    --registry="$OVERLAY_LIST" \
    --out-dir="$OUT" \
    --curated-dir="$FORCED_ENTRIES_DIR" \
    ${LEVEL_ID_ADDR:+--level-id-addr="$LEVEL_ID_ADDR"}

if [[ "$REGEN_SCOPE" == "full" ]]; then
    while read -r magic name ovl_path; do
        [[ -n "$name" ]] || continue
        if process_overlay "$name" "$ovl_path" "func_${name}_" "$OVERLAY_LOAD_ADDR"; then
            register_overlay "$name" "$magic"
        fi
    done < "$OVERLAY_LIST"
fi

# --- Main EXE -----------------------------------------------------------
#
# Done LAST so we can fold each overlay's external-JAL targets into the
# main-EXE forced-entry set. Overlays JAL into main-EXE bytes (gem
# accounting, level setup, audio helpers); the main-EXE static JAL pass
# can't see those callers, so without this union the called functions
# stub-miss at runtime as multi-entry / fall-through fast-paths.
#
# Forced entries:
#   0x80032AB0 - fast-path entry only reached from the titlescreen
#                  overlay; the new multi-entry pass picks it up too,
#                  but kept as belt-and-braces.
#   0x8004F8C8, 0x80050060, 0x800506EC, 0x80038620, 0x8005A7B0 -
#                  runtime stub-miss targets observed on level_10 and at
#                  Stone Hill entry. Re-validated against
#                  bk2/stone-hill-entry.bk2: byte-identical screenshot
#                  vs no-forced-entries baseline at frame 3600, so the
#                  earlier "breaks geometry past Artisans" report could
#                  not be reproduced.
echo "[regen] Recompiling PSX.EXE (with overlay-discovered cross-binary JAL targets)"
main_exe_extra="$OUT/main_exe_extra_funcs.txt"
: > "$main_exe_extra"
main_exe_targets=$(asm_targets_for main_exe)
[[ -n "$main_exe_targets" ]] && cat "$main_exe_targets" >> "$main_exe_extra"
# Hand-curated forced entries (formerly --forced-entry literals here) -
# single source of truth shared with the converged-manifest emission.
# Hand-curated forced entries, if this game has any yet.
if [[ -f "$FORCED_ENTRIES_DIR/main_exe.txt" ]]; then
    grep '^0x' "$FORCED_ENTRIES_DIR/main_exe.txt" >> "$main_exe_extra" || true
fi
for f in "$OUT"/*/external_jals.txt; do
    [[ -f "$f" ]] && cat "$f" >> "$main_exe_extra"
done
# Two-pass main-EXE recompile.
#
# Pass 1 produces $OUT/_main_exe_pass1/PSX.EXE_full.c with no JT-stub
# forced entries - purely a stub-discovery scratch output. Pass 2
# reads its `case N: call_by_address(cpu, 0xADDR);` sites for the
# SetOverlayPointers JT (per-level setup like Toasty's 0x8005A930)
# and folds them in as forced entries before producing the final
# $OUT/PSX.EXE_full.c that links into the binary. Without this, level
# swaps that go through one of those JT cases never re-init their
# fn-pointer slots, the stale level_N pointers from the previous
# overlay get jalr'd in level_N+1 -> cascading stub misses on overlay
# entry, with the user-visible symptom of demo-mode hanging in
# level_24 (Dr Shemp) at frame ~8458.
#
# The previous design read $OUT/PSX.EXE_full.c (the final output of
# the last regen) as its bootstrap input, which made the result
# dependent on whatever was already on disk - a stale or missing
# generated/ produced an under-converged stubs file and a hang. This
# split decouples the two roles into two distinct files, so one
# invocation is always self-converging:
#   * PSX.EXE_pass1.c (intermediate, not linked) -> stubs.txt
#   * PSX.EXE_full.c  (final, links into the binary) <- stubs.txt
# Pass 1 unconditionally deletes pass 2's outputs so the build can't
# silently link a stale converged file if pass 2 errors out.
common_psx_args=(
    --external-symbols="$PSYQ_MANIFEST"
    --external-prefix=psyq_
    --skip-on-error
    --defer-foreign-calls
    ${CHANNEL_MANIFEST:+--channel-sites="$CHANNEL_SITES"}
    ${PCTRACK_FLAG:+$PCTRACK_FLAG}
)
pass1_dir="$OUT/_main_exe_pass1"
mkdir -p "$pass1_dir"
# Invalidate pass-2 outputs: whatever's on disk is stale w.r.t. the
# fresh pass-1 we're about to produce. Mirrors how cmake's mtime-based
# incremental builds would handle a DAG node whose inputs changed.
rm -f "$OUT/PSX.EXE_full.c" "$OUT/PSX.EXE_dispatch.c" \
      "$OUT/main_exe_dispatch_stubs.txt"
echo "[regen]   pass 1 (stub discovery) -> $pass1_dir/"
run_psxrecomp "$PSX_EXE" \
    --output-dir="$pass1_dir" \
    --extra-funcs="$main_exe_extra" \
    "${common_psx_args[@]}" >/dev/null
python3 tools/extract_dispatch_call_by_address.py \
    "$pass1_dir/PSX.EXE_full.c" "$OUT/main_exe_dispatch_stubs.txt" 2>&1 | sed 's/^/  /'
cat "$OUT/main_exe_dispatch_stubs.txt" >> "$main_exe_extra"
echo "[regen]   pass 2 (final, with $(grep -c '^0x' "$OUT/main_exe_dispatch_stubs.txt") JT-stub forced entries) -> $OUT/PSX.EXE_full.c"
run_psxrecomp "$PSX_EXE" \
    --output-dir="$OUT" \
    --extra-funcs="$main_exe_extra" \
    "${common_psx_args[@]}"
# Third pass: promote cross-piece split targets to real entries, the
# same convergence the overlay path runs. A transfer that crosses a
# piece boundary can only be emitted as `call_by_address(<target>);
# return;`, and a target with no registered entry stub-misses. The stub
# returns instead of transferring control, so the guest epilogue never
# runs. Handwritten asm that parks the scratchpad base in `sp` and uses
# it as a general register then leaks that value into ordinary C code,
# and the next frame is built below the scratchpad where it maps to no
# host address.
#
# --require-emitted-insn is what makes this safe on a main EXE: string
# tables sit between functions here, so an address the analyzer decoded
# out of one is word-aligned and in range and passes the overlay path's
# two filters. Keeping only targets with an emitted instruction drops
# those (21 of 21 reported for Spyro 3, all in its data tail).
#
# Off for Spyro 1: its one reported target is 0x800661D0, and promoting
# that reproducibly broke the level_24 demo entry path with stub misses
# at 0x8005AC40 + 0x8008772C - adding a forced entry there shifts
# function-boundary detection so pre-existing dispatch entries are lost.
if [[ "${MAIN_EXE_THIRD_PASS:-0}" != "0" ]]; then
    main_exe_load=$(python3 -c "import struct,sys;print(hex(struct.unpack_from('<I',open(sys.argv[1],'rb').read(),0x18)[0]))" "$PSX_EXE")
    split_found="$OUT/main_exe_split_targets.txt"
    orphan_found="$OUT/main_exe_orphan_blocks.txt"
    for pass in 1 2 3 4 5; do
        python3 tools/extract_split_targets.py \
            "$OUT/PSX.EXE_full.c" "$split_found" \
            --binary="$PSX_EXE" --load-addr="$main_exe_load" \
            --require-emitted-insn 2>&1 | sed 's/^/  /'
        python3 tools/extract_orphan_blocks.py \
            "$OUT/PSX.EXE_full.c" "$orphan_found" 2>&1 | sed 's/^/  /'
        grep -qh '^0x' "$split_found" "$orphan_found" || break
        echo "[regen]   split pass $pass:" \
             "$(grep -c '^0x' "$split_found" || true) split," \
             "$(grep -c '^0x' "$orphan_found" || true) orphan target(s) promoted"
        grep -h '^0x' "$split_found" "$orphan_found" >> "$main_exe_extra"
        run_psxrecomp "$PSX_EXE" \
            --output-dir="$OUT" \
            --extra-funcs="$main_exe_extra" \
            "${common_psx_args[@]}"
    done
fi
sed -i 's/\bpsx_dispatch_compiled\b/psx_dispatch_main_exe/g' \
    "$OUT/PSX.EXE_dispatch.c"

if [[ "$REGEN_SCOPE" == "full" ]]; then
    echo "[regen] Classifying overlay loops against $OUT/PSX.EXE_leaves.tsv"
    for name in "${OVERLAY_NAMES[@]}"; do
        classify_overlay "$name"
    done
fi

# --- Generate the dispatch table ----------------------------------------

if [[ "$REGEN_SCOPE" != "full" ]]; then
    echo "[regen] SCOPED regen: keeping existing $OUT/spyro_overlay_table.c"
else
echo "[regen] Generating $OUT/spyro_overlay_table.c (${#OVERLAY_NAMES[@]} overlays)"
{
    echo "/* AUTO-GENERATED by scripts/regen-recomp.sh - do not edit. */"
    echo "#include \"spyro_overlay_table.h\""
    echo
    for name in "${OVERLAY_NAMES[@]}"; do
        echo "extern int psx_dispatch_${name}(CPUState* cpu, uint32_t addr);"
        if grep -q "^int psx_quiet_${name}(" "$OUT/$name/$name.bin_dispatch.c"; then
            echo "extern int psx_quiet_${name}(uint32_t addr, uint32_t* cost);"
        fi
    done
    echo
    echo "const spyro_overlay_entry_t spyro_overlay_table[] = {"
    for name in "${OVERLAY_NAMES[@]}"; do
        # The quiet column is written only for an overlay that answers a
        # deferred spin; an entry without it initializes the field to NULL.
        if grep -q "^int psx_quiet_${name}(" "$OUT/$name/$name.bin_dispatch.c"; then
            printf '    { %3d, "%s", psx_dispatch_%s, psx_quiet_%s },\n' \
                "${OVERLAY_MAGIC[$name]}" "$name" "$name" "$name"
        else
            printf '    { %3d, "%s", psx_dispatch_%s },\n' \
                "${OVERLAY_MAGIC[$name]}" "$name" "$name"
        fi
    done
    echo "};"
    echo "const size_t spyro_overlay_table_count = sizeof(spyro_overlay_table) / sizeof(spyro_overlay_table[0]);"
    echo "const uint32_t spyro_overlay_window_lo = ${OVERLAY_RANGE%%:*}u;"
    echo "const uint32_t spyro_overlay_window_hi = ${OVERLAY_RANGE##*:}u;"
} > "$OUT/spyro_overlay_table.c"
fi

ra_spill_found=$( { grep -h 'ra-spill return:' "$RA_SPILL_LOG" || true; } | sort -u | wc -l)
if [[ "$ra_spill_found" != "${RA_SPILL_RETURNS:-0}" ]]; then
    echo "[regen] ra-spill returns: found $ra_spill_found, $GAME_CONF expects ${RA_SPILL_RETURNS:-0}:" >&2
    { grep -h 'ra-spill return:' "$RA_SPILL_LOG" || true; } | sort -u >&2
    exit 1
fi
echo "[regen] ra-spill returns: $ra_spill_found (matches recomp.conf)"

# Every in-image word-table `jr` must dispatch through a switch; an
# unswitched one stub-misses on any entry that is not a dispatch entry.
# recomp.conf's JUMPTABLE_UNSWITCHED names the accepted exceptions.
python3 tools/check_jumptable_dispatch.py --game "$GAME"

# Calibrate + verify signature anchors for the determinism-critical channels
# from the freshly-recompiled reference binary (needs discovered_functions.log
# for the function-scoped tier). Fails the regen if any determinism-critical
# site can't be uniquely anchored, which relocating the sites in a romhack
# would need.
if [[ -n "${CHANNEL_MANIFEST:-}" ]]; then
    echo "[regen] Generating $OUT/channel_signatures.txt (determinism-critical anchors)"
    python3 tools/gen_channel_signatures.py \
        --manifest "$CHANNEL_MANIFEST" \
        --exe "$PSX_EXE" \
        --out "$OUT/channel_signatures.txt"
fi

# Snapshot the converged per-binary inputs as committed metadata for the
# single-pass first-boot pipeline of a game whose recomp.conf sets
# FIRST_BOOT=1. A scoped regen describes a local mod-site bake rather than
# the stock full-regen state, so it emits nothing.
if [[ "${FIRST_BOOT:-0}" != "1" ]]; then
    echo "[regen] $GAME has no first-boot module build - no converged manifest emitted"
elif [[ "$REGEN_SCOPE" != "full" ]]; then
    echo "[regen] SCOPED regen: skipping converged-manifest emission"
else
    # The emitter fails when the committed snapshot differs from what this
    # regen converged to; the generated/ tree above is complete either way.
    # OPENPETE_MANIFEST_BLESS=1 moves the snapshot (commit it with the
    # input change that moved it).
    echo "[regen] Emitting converged recomp manifest"
    GAME="$GAME" bash scripts/emit-recomp-manifest.sh
fi

# First boot compiles this C with the bundled clang under -w, which still
# refuses diagnostics GCC accepts; compile it the same way here so an
# emission defect fails the regen instead of a player's first boot. SKIPs
# (exit 0) without the bundled clang or the linux-release database.
python3 scripts/regen_clang_check.py "$GAME"

if [[ "$REGEN_SCOPE" == "full" ]]; then
    python3 scripts/fences/recomp_stamp.py write "$GAME"
fi
echo "[regen] Done - rebuild the project to pick up the new C output."
