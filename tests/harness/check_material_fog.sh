#!/usr/bin/env bash
# Material distance-fade contract: the engine fades a mod material's result,
# so a material loses no fog it never knew about.
#
# mods/mat-fx-fog claims every moby. Five legs per row, at a heavy-fog pose
# under extended LOD:
#   stock     no mod: what the engine draws
#   base      the material returns base
#   modulate  the material returns op_psx_modulate(op_psx_texel(op_uv()),
#             op_color()), the stock shading written by hand
#   stockC    the material returns op_color() under OPENPETE_HFOG_FLAT_RGB=C:
#             every faded world moby corner carries the colour C before the
#             engine's per-corner fade, so this is the stock fade of C
#   flatC     the material returns C under the same variable
# Pass: base and modulate equal stock at every pixel (MAXPASS levels per
# channel, default 0); |flatC - stockC| is at most MAXFLAT (the per-pixel
# fade against the per-corner one) over the pixels the fade moved, and every
# such pixel the stock fade drew as the fog colour, flatC draws within
# MAXFLAT of it. Engagement: the pixels the fade moved (stockC differs from
# both stock and C); 0 refuses.
#
# Rows: Spyro 1 beast_makers and Spyro 2 mystic_marsh at a pose whose
# mobys span the fade band; Spyro 3 harbor_speedway at its spawn view.
# Usage: tests/harness/check_material_fog.sh [--game spyro-1|spyro-2|spyro-3] [--out DIR]
# Exit: 0 pass, 1 fail, 64 skip (no binary, disc or PIL).
set -uo pipefail

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/sandbox.sh
source scripts/lib/sandbox.sh

GAMES=(spyro-1 spyro-2 spyro-3)
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --game) GAMES=("$2"); shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "usage: $0 [--game G] [--out DIR]"; exit 64 ;;
    esac
done
BIN=build/linux-release/openpete
[ -x "$BIN" ] || { echo "[matfog] SKIP: no binary at $BIN"; exit 64; }
python3 -c "import PIL, numpy" 2>/dev/null || { echo "[matfog] SKIP: PIL or numpy unavailable"; exit 64; }
WORK="${OUT:-${TMPDIR:-/tmp}/openpete-matfog.$$}"
mkdir -p "$WORK"
[ -n "$OUT" ] || trap 'rm -rf "${WORK:?}"; op_sandbox_cleanup' EXIT

FLAT_RGB=0x783CC8          # C = (200, 60, 120), 0x00BBGGRR
FLAT_F="0.78431, 0.23529, 0.47059"
MAXPASS="${MAXPASS:-0}" MAXFLAT="${MAXFLAT:-1}"
SHOT=2600

row_args() {   # game -> disc|level|pose
    case "$1" in
        spyro-1) echo "games/spyro-1/spyro-1/build/SCUS-94228.cue|beast_makers|pos=(91882, 121695, 19026) yaw=1256 pitch=7" ;;
        spyro-2) echo "games/spyro-2/spyro-2/build/SCUS-94425.cue|mystic_marsh|pos=(69053, 80629, 20029) yaw=-1425 pitch=28" ;;
        spyro-3) echo "games/spyro-3/spyro-3/build/SCUS-94467.cue|harbor_speedway|" ;;
    esac
}

# leg <game> <name> <mode or -> <flat 0|1>
leg() {
    local g="$1" name="$2" mode="$3" flat="$4" disc level pose md root f
    IFS='|' read -r disc level pose <<< "$(row_args "$g")"
    [ -f "$disc" ] || { echo "[matfog] SKIP: no disc $disc"; return 64; }
    md="$WORK/$g-$name-mods"
    rm -rf "${md:?}"
    mkdir -p "$md"
    if [ "$mode" = - ]; then
        : > "$md/enabled.txt"
    else
        cp -r mods/mat-fx-fog "$md/"
        echo mat-fx-fog > "$md/enabled.txt"
        sed -i "s/^params  = .*/params  = [$mode.0, $FLAT_F]/" "$md/mat-fx-fog/mod.toml"
    fi
    rm -f "$WORK/$g-$name.png" "$WORK/$g-$name.log"
    local -a posearg=()
    [ -n "$pose" ] && posearg=(--freecam-pose "$pose")
    root="$(op_sandbox_run_root)"
    (
    [ "$flat" = 1 ] && export OPENPETE_HFOG_FLAT_RGB="$FLAT_RGB"
    OPENPETE_SANDBOX="$root" OPENPETE_CARD_DIR="$root/cards" OPENPETE_VIBRATION=0 \
        op_contain timeout -s KILL 600 "$BIN" --headless \
        --no-config --game "$g" --disc "$disc" --renderer vk --aspect 4:3 --lod extended \
        --no-card --mute 1 --vibration 0 --skip-to-level "$level" "${posearg[@]}" \
        --screenshot-native-only --screenshot-on-frame "$SHOT" \
        --screenshot-output "$WORK/$g-$name-{n}.png" --frame-limit $((SHOT + 10)) \
        --mods-dir "$md" > "$WORK/$g-$name.log" 2>&1
    )
    f="$(ls "$WORK/$g-$name-"*.png 2>/dev/null | head -1)"
    [ -n "$f" ] || { echo "[matfog] FAIL: $g $name wrote no capture (log $WORK/$g-$name.log)"; return 1; }
    mv "$f" "$WORK/$g-$name.png"
}

fail=0
for g in "${GAMES[@]}"; do
    gfail=0
    for spec in stock:-:0 base:0:0 modulate:1:0 stockC:3:1 flatC:2:1; do
        IFS=: read -r name mode flat <<< "$spec"
        leg "$g" "$name" "$mode" "$flat"
        rc=$?
        [ "$rc" = 64 ] && exit 64
        [ "$rc" = 0 ] || gfail=1
    done
    if [ "$gfail" != 0 ]; then fail=1; continue; fi
    python3 - "$WORK" "$g" "$MAXPASS" "$MAXFLAT" <<'PY' || fail=1
import sys
import numpy as np
from PIL import Image
w, g, maxpass, maxflat = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
im = {k: np.asarray(Image.open(f"{w}/{g}-{k}.png").convert("RGB")).astype(int)
      for k in ("stock", "base", "modulate", "stockC", "flatC")}
C = np.array([200, 60, 120])
ok = True
for k in ("base", "modulate"):
    d = np.abs(im[k] - im["stock"]).max(axis=2)
    n = int((d > maxpass).sum())
    print(f"[matfog] {g}: {k} vs stock: {int((d > 0).sum())} px differ, max {d.max()} levels; "
          f"over {maxpass}: {n}")
    ok &= n == 0
cover = np.all(im["flatC"] == C, axis=2) | np.any(im["flatC"] != im["stock"], axis=2)
moved = cover & np.any(im["stockC"] != C, axis=2)
d = np.abs(im["flatC"] - im["stockC"]).max(axis=2)
cols, counts = np.unique(im["stockC"][moved], axis=0, return_counts=True) if moved.any() else (np.zeros((1, 3), int), np.zeros(1, int))
T = cols[np.argmax(counts)]
full = moved & np.all(im["stockC"] == T, axis=2)
print(f"[matfog] {g}: fade engaged on {int(moved.sum())} px (fog colour {tuple(int(x) for x in T)} on {int(full.sum())}); "
      f"|flatC - stockC| max {d[moved].max() if moved.any() else 0}, mean {d[moved].mean() if moved.any() else 0:.3f}; "
      f"at full fog max {d[full].max() if full.any() else 0}")
if not moved.any():
    print(f"[matfog] {g}: VACUOUS: the fade moved no pixel"); ok = False
else:
    ok &= d[moved].max() <= maxflat and (not full.any() or d[full].max() <= maxflat)
print(f"[matfog] {g}: {'PASS' if ok else 'FAIL'} (pass-through max {maxpass}, flat max {maxflat})")
sys.exit(0 if ok else 1)
PY
done
exit "$fail"
