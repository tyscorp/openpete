#!/usr/bin/env bash
# check_terrain_walk_cache.sh - CPU terrain walk vs retained GPU terrain cache,
# per-pose differing-pixel count against a committed baseline.
#
# The walk and the cache are one scene by contract; where they draw different
# pixels at a pose (a vertex position the two chains round differently, depth
# ties under the stored depth encoding, a blended-face draw order the two feeds
# emit differently), the count of those pixels is the
# measured residual of that pose. The freecam golden captures the walk only, so
# it cannot see a residual whose walk image is already right; this check holds
# one row per such pose and fails on any change of its count. A drop is a fix
# and is promoted with --bless, as a golden tile is.
#
# Rows: games/<game>/corpus/walk-cache.manifest, one of two forms (lod default
# extended):
#   pose row  `<level> | <pose> | [lod]`, a --skip-to-level boot to a freecam
#             pose captured at the freecam corpus's canonical VBL (START).
#             Id `<level>-<first 6 hex of the pose's md5>__<lod>`.
#   bk2 row   `<bk2> | <VBL> | <canon|inter> | [lod]`, a movie replay captured
#             at VBL, on the canonical present (canon) or on the deterministic
#             sub-tick present at alpha 0.5 (inter,
#             --screenshot-intermediate-on-frame). <bk2> names
#             games/<game>/bk2/<bk2>.bk2; a name with no movie file is the plain
#             boot (title screen and attract demo), as in the presentation
#             corpus. Id `<bk2>@<VBL>__<leg>__<lod>`.
# A row whose second field is a bare integer is a bk2 row.
#
# Instrument: two headless legs of one build per row, 16:9, native-only, at the
# row's VBL, differing only in the feed:
#   walk   the software-vk default: the terrain cache is not initialized, every
#          sector goes through the CPU walk
#   cache  OPENPETE_TERRAIN_CACHE_FORCE=1: the retained cache, forced on under
#          software vk
# The row's value is the number of pixels whose RGB differ in any channel.
#
# Engagement, read from the logs at the captured VBL:
#   walk   `[nearcen] frame=<VBL> ... terrainTris=<n>` (OPENPETE_TERRAIN_NEARCEN=1),
#          n > 0
#   cache  `[tgpu-cen f=<VBL>] sub=<n> secs=<n>` (OPENPETE_TERRAIN_GPU_CENSUS=1),
#          secs > 0; the walk leg reports no retained sector (walk secs == 0)
# The value is the line whose VBL equals the row's VBL, else the last line
# before it; on an inter row that is the sub-tick re-extract's own line, which
# follows the canonical walk of the same VBL.
# A missing capture or a zero count is VACUOUS and fails the row.
#
# Baseline: tests/golden/<game>/walk-cache-baseline.tsv, `<row id>\t<px>`. A row
# fails when its count differs from the baseline in either direction, or when it
# has no baseline row. --bless merges the captured counts into the baseline
# (rows outside --filter are kept) and exits 0 when every row engaged.
#
# Usage:
#   tests/harness/check_terrain_walk_cache.sh [--game SLUG] [--build-dir DIR]
#                                       [--filter SUBSTR] [--bless]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest (the message
# names the path), 64 SKIP (no binary, disc, Vulkan loader, or python3/PIL).
set -u

# Unattended launcher: every run this script starts is silenced. Vibration
# ships on by default, which is right for a player and wrong for a
# scripted run.
# (scripts/fences/rumble_guard.py enforces this.)
export OPENPETE_VIBRATION=0

cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

BUILD_DIR=build/linux-release
FILTER=""
BLESS=0
prev=""
for a in "$@"; do
    case "$prev" in
        --build-dir) BUILD_DIR="$a";;
        --filter)    FILTER="$a";;
    esac
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --filter=*)    FILTER="${a#*=}";;
        --bless)       BLESS=1;;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64

MAN="$(pg_corpus_manifest "$PG_GAME" walk-cache.manifest)"
pg_manifest "$MAN" pipe
BASE="tests/golden/$PG_GAME/walk-cache-baseline.tsv"
[[ -x "$PG_BIN"  ]] || { echo "walk-cache: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "walk-cache: SKIP (no disc at $PG_DISC)"; exit 64; }
ldconfig -p 2>/dev/null | grep -q 'libvulkan\.so\.1' ||
    { echo "walk-cache: SKIP (no Vulkan loader)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "walk-cache: SKIP (python3 or PIL missing)"; exit 64; }

# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env

OUT="$BUILD_DIR/walk-cache/$PG_GAME"
mkdir -p "$OUT"

# The freecam corpus's canonical post-load feed-key value, per game
# (pg_cap_settings, read by tests/harness/freecam-corpus.sh).
pg_cap_settings
START="$PG_FREECAM_START"

# run_leg ID LEG VBL SHOTFLAG [ENV=VAL...] -- <boot args>: one capture at VBL;
# writes $OUT/ID__LEG.log and $OUT/ID__LEG-vk.png, both deleted first by
# pg_capture. SHOTFLAG is --screenshot-on-frame (canonical present) or
# --screenshot-intermediate-on-frame (sub-tick present).
run_leg() {
    local id="$1" leg="$2" vbl="$3" shotflag="$4"; shift 4
    local -a envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    local stem="$OUT/${id}__${leg}"
    rm -f "$stem.png"
    PG_CAPTURE_ENV=(OPENPETE_TERRAIN_NEARCEN=1 OPENPETE_TERRAIN_GPU_CENSUS=1 "${envs[@]}")
    PG_CAPTURE_TIMEOUT=600
    pg_capture "$stem.log" "$stem-vk.png" -- \
        --renderer vk --mute 1 --aspect 16:9 --no-card "$@" \
        --screenshot-native-only "$shotflag" "$vbl" \
        --screenshot-output "$stem.png" \
        --frame-limit $((vbl + 10)) --max-runtime-seconds 580
}

# row_boot I: the boot arguments of row I into BOOT (array), its VBL into
# ROW_VBL and its screenshot flag into ROW_SHOT.
row_boot() {
    local i="$1"
    BOOT=(--lod "${LODS[$i]}")
    if [[ "${KINDS[$i]}" == pose ]]; then
        BOOT+=(--skip-to-level "${LEVELS[$i]}" --freecam-pose "${POSES[$i]}")
        ROW_VBL="$START"; ROW_SHOT=--screenshot-on-frame
    else
        local movie="$PG_BK2_DIR/${LEVELS[$i]}.bk2"
        [[ -f "$movie" ]] && BOOT+=(--bk2 "$movie")
        ROW_VBL="${POSES[$i]}"
        if [[ "${LEGS[$i]}" == inter ]]; then
            ROW_SHOT=--screenshot-intermediate-on-frame
        else
            ROW_SHOT=--screenshot-on-frame
        fi
    fi
}

# The manifest is read into arrays before any run: a child that inherits the
# loop's stdin would eat the rows it has not reached yet.
IDS=(); KINDS=(); LEVELS=(); POSES=(); LEGS=(); LODS=(); VBLS=()
trim() { sed 's/^[[:space:]]*//; s/[[:space:]]*$//' <<<"$1"; }
while IFS='|' read -r f1 f2 f3 f4; do
    f1="$(echo "$f1" | tr -d ' \t')"
    [[ -z "$f1" || "${f1:0:1}" == "#" ]] && continue
    f2="$(trim "$f2")"; f3="$(echo "${f3:-}" | tr -d ' \t')"; f4="$(echo "${f4:-}" | tr -d ' \t')"
    [[ -z "$f2" ]] && continue
    if [[ "$f2" =~ ^[0-9]+$ ]]; then
        # bk2 row: <bk2> | <VBL> | <canon|inter> | [lod]
        [[ "$f3" == canon || "$f3" == inter ]] ||
            { echo "walk-cache: bad leg '$f3' on bk2 row $f1 (canon or inter)"; exit 1; }
        lod="${f4:-extended}"
        id="${f1}@${f2}__${f3}__${lod}"
        kind=bk2; leg="$f3"; vbl="$f2"
    else
        # pose row: <level> | <pose> | [lod]
        lod="${f3:-extended}"
        id="$f1-$(printf '%s' "$f2" | md5sum | cut -c1-6)__$lod"
        kind=pose; leg=canon; vbl="$START"
    fi
    [[ -n "$FILTER" && "$id" != *"$FILTER"* ]] && continue
    IDS+=("$id"); KINDS+=("$kind"); LEVELS+=("$f1"); POSES+=("$f2")
    LEGS+=("$leg"); LODS+=("$lod"); VBLS+=("$vbl")
done < "$MAN"

if [[ ${#IDS[@]} -eq 0 ]]; then
    echo "[walk-cache] game=$PG_GAME rows=0 filter='$FILTER' verdict=VACUOUS"
    exit 1
fi
echo "[walk-cache] game=$PG_GAME bin=$PG_BIN rows=${#IDS[@]} filter='$FILTER' -> $OUT"

# Two game processes per row: the walk leg and the cache leg run concurrently.
RESULTS="$OUT/results.tsv"
rm -f "$RESULTS"
for i in "${!IDS[@]}"; do
    id="${IDS[$i]}"
    row_boot "$i"
    run_leg "$id" walk  "$ROW_VBL" "$ROW_SHOT" -- "${BOOT[@]}" & pw=$!
    run_leg "$id" cache "$ROW_VBL" "$ROW_SHOT" OPENPETE_TERRAIN_CACHE_FORCE=1 \
        -- "${BOOT[@]}" & pc=$!
    wait "$pw"; rw=$?
    wait "$pc"; rc=$?
    src="${LEVELS[$i]}"
    [[ "${KINDS[$i]}" == bk2 && ! -f "$PG_BK2_DIR/${LEVELS[$i]}.bk2" ]] && src="${LEVELS[$i]} (plain boot)"
    echo "  [$id] ${KINDS[$i]}=$src vbl=$ROW_VBL leg=${LEGS[$i]} lod=${LODS[$i]} rc walk=$rw cache=$rc"
    printf '%s\t%s\n' "$id" "$ROW_VBL" >> "$RESULTS"
done

python3 - "$OUT" "$RESULTS" "$BASE" "$BLESS" "$PG_GAME" <<'WC_PY'
import os, re, sys
from PIL import Image, ImageChops

out, results, base_path, bless, game = sys.argv[1:6]
bless = bless == "1"
tri = re.compile(r"\[nearcen\] frame=(\d+) .*?terrainTris=(\d+)")
cen = re.compile(r"\[tgpu-cen f=(\d+)\] sub=\d+ secs=(\d+)")


def at_vbl(path, pat, start):
    """The count on the last line at the captured VBL, else the last line before it."""
    v = None
    if not os.path.isfile(path):
        return 0
    for line in open(path, errors="replace"):
        m = pat.search(line)
        if m and int(m.group(1)) <= start:
            v = int(m.group(2))
    return v or 0


def read_base(path):
    base = {}
    if os.path.isfile(path):
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#"):
                k, n = line.split("\t")
                base[k] = int(n)
    return base


base = read_base(base_path)
counts, fails = {}, 0
rows = [l.split("\t") for l in open(results) if l.strip()]
ids = [r[0] for r in rows]
for rid, vbl in rows:
    vbl = int(vbl)
    walk_png = os.path.join(out, f"{rid}__walk-vk.png")
    cache_png = os.path.join(out, f"{rid}__cache-vk.png")
    tris = at_vbl(os.path.join(out, f"{rid}__walk.log"), tri, vbl)
    secs = at_vbl(os.path.join(out, f"{rid}__cache.log"), cen, vbl)
    walk_secs = at_vbl(os.path.join(out, f"{rid}__walk.log"), cen, vbl)
    missing = [p for p in (walk_png, cache_png) if not os.path.isfile(p)]
    if missing or tris == 0 or secs == 0 or walk_secs != 0:
        print(f"[walk-cache] row={rid} verdict=VACUOUS walkTris={tris} cacheSecs={secs}"
              f" walkSecs={walk_secs}"
              + (f" missing={','.join(os.path.basename(p) for p in missing)}" if missing else ""))
        fails += 1
        continue
    A = Image.open(walk_png).convert("RGB")
    B = Image.open(cache_png).convert("RGB")
    if A.size != B.size:
        print(f"[walk-cache] row={rid} verdict=FAIL reason=size {A.size} vs {B.size}")
        fails += 1
        continue
    # The per-pixel maximum over the channels: a luma conversion rounds a
    # one-level difference in blue alone (weight 0.114) to 0.
    r, g, b = ImageChops.difference(A, B).split()
    d = ImageChops.lighter(ImageChops.lighter(r, g), b).point(lambda v: 255 if v else 0)
    px = d.histogram()[255]
    counts[rid] = px
    want = base.get(rid)
    if bless:
        verdict = "BLESS"
    elif want is None:
        verdict = "FAIL"; fails += 1
    elif px != want:
        verdict = "FAIL"; fails += 1
    else:
        verdict = "PASS"
    print(f"[walk-cache] row={rid} verdict={verdict} px={px} baseline="
          f"{'none' if want is None else want} walkTris={tris} cacheSecs={secs}")

if bless:
    if fails:
        print(f"[walk-cache] game={game} bless refused: {fails} row(s) vacuous")
        sys.exit(1)
    merged = dict(base)
    merged.update(counts)
    os.makedirs(os.path.dirname(base_path), exist_ok=True)
    with open(base_path, "w") as f:
        f.write("# row\tpx whose RGB differ, CPU terrain walk vs retained terrain cache "
                "(tests/harness/check_terrain_walk_cache.sh)\n")
        for k in sorted(merged):
            f.write(f"{k}\t{merged[k]}\n")
    print(f"[walk-cache] baseline written: {base_path} ({len(counts)} rows from this run, "
          f"{len(merged) - len(counts)} kept)")
    sys.exit(0)
print(f"[walk-cache] game={game} rows={len(ids)} fail={fails} verdict={'PASS' if not fails else 'FAIL'}")
sys.exit(1 if fails else 0)
WC_PY
