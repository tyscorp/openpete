#!/usr/bin/env bash
# check_cache_walk_ab.sh - retained-terrain GPU cache vs CPU terrain walk,
# per-row pixel ratchet, headless on llvmpipe.
#
# The retained cache draws a static sector from a persistent vertex buffer and
# re-derives the per-face decisions (LOD tier, near/HQ subdivision, far cull,
# the CLUT-ladder fog row) in its own vertex stage in fp32; the CPU walk takes
# the same decisions on the host and feeds the scene stages, its vertex
# positions through the same float chain (terrain.c tn_vert_project). A vertex
# the two chains round differently crosses a raster snap boundary, which moves
# an edge pixel or takes the neighbouring texel at a texel boundary. llvmpipe
# is deterministic, so the count is a fixed number per row.
#
# Instrument: two legs of one build on one device at the same pose and the
# same pinned VBL, differing only in the feed.
#   leg a  --terrain-cache 0   the CPU walk (reference)
#   leg b  default feed, OPENPETE_TERRAIN_CACHE_FORCE=1 (the retained cache,
#          forced on because software vk does not select it on its own)
# Both legs are 4:3, --lod extended, --vertex-precision subpixel, captured
# native-only (no guest reference leg: the compare is openpete against
# openpete).
#
# Pass criterion: per row, the differing pixels (any channel) are at most
# the row's cap in tests/harness/cache_walk_ab.caps, the count measured on
# the fp32 cache; a rise fails, a fall passes and prints TIGHTEN with the
# new count, so the cap is lowered to it. A row with no cap fails. Each
# row's caps line also records the edge/face split of its pixels at the
# count the cap holds. A row with differing pixels also captures both
# legs' face-identity images (OPENPETE_TERRAIN_FACEID=1) and reports the
# split (tools/terrain_hole_census.py split): an edge pixel lies within one
# pixel of a face boundary in both legs, a face pixel does not.
#
# Both legs run with OPENPETE_TERRAIN_ANIMNEAR=1 and
# OPENPETE_TERRAIN_CANIMNEAR=1, so leg b also draws near-reaching poly-anim
# and env-anim colour sectors from the cache (the walk leg ignores both).
#
# Engagement: per row, leg b must report retained sectors (secs > 0
# on a [tgpu-cen] line) and leg a must report none and must report terrain
# triangles ([nearcen] terrainTris > 0). A row that misses either compared
# nothing about the cache and is VACUOUS, never PASS.
#
# The rows are the poses the class was measured on, named by their
# freecam-corpus slug <level>-<first 6 hex of the pose's md5> so the pose text
# keeps its one authority, games/<game>/corpus/freecam-corpus.manifest. A row
# written <slug>:inter captures the deterministic sub-tick present at the same
# VBL (--screenshot-intermediate-on-frame, alpha 0.5) instead of the canonical
# one, so the moving-terrain lerp is compared between the two feeds. The
# :inter rows are poses whose poly-anim near faces move visibly at that
# present: OPENPETE_TERRAIN_VANIM=0 and =1 captures of either feed differ
# there, so a cache feed that snapped those faces to the current tick would
# fail the row.
#
# Every row reports, beside the pixel count, the count of poly-anim sectors
# leg b drew from near-variant chunks ([tgpu-cen] animnear=) and of the
# previous-corner records they carry (animrecs=) at the captured present
# (the last census line at or before the pinned VBL from the same kind of
# build, sub=1 for an :inter row), the engagement of the near stage's lerp,
# and likewise the env-anim colour sectors drawn from near-variant chunks
# (canimnear=) and those drawn with a colour table (coltab=), and the
# sectors drawn with position-ring faces (ringsecs=) with their faces'
# triangles whose ring-entry corners move (ringmv=). An :inter row whose
# captured present drew no previous-corner record, no colour table and no
# moving ring triangle is VACUOUS.
#
# Usage:
#   tests/harness/check_cache_walk_ab.sh [--game SLUG] [--build-dir=DIR] [--out DIR]
# Exit 0 PASS, 1 FAIL or VACUOUS or a missing corpus manifest or caps
# file (the message names the path), 64 SKIP (no binary, no disc, no python3/PIL, or the game
# has no measured rows).
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
OUT=""
prev=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD_DIR="${a#*=}";;
        --out=*)       OUT="${a#*=}";;
    esac
    case "$prev" in
        --out) OUT="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD_DIR"
pg_resolve "$GAME" || exit 64

# Per game, the freecam-corpus rows the cache-vs-walk class was measured on.
case "$PG_GAME" in
  spyro-1) ROWS="dream_weavers-9dace6 gnastys_loot-ca95bd magic_crafters-fa5245";;
  spyro-2) ROWS="crushs_dungeon-f167d0 crushs_dungeon-5feea7:inter metro_speedway-464858:inter mystic_marsh-6241c8";;
  spyro-3) ROWS="molten_crater-43eca3 sunny_villa-b803cb:inter spooky_swamp-be04ca:inter";;
  *) echo "cache-walk-ab: SKIP ($PG_GAME has no measured cache-vs-walk rows)"; exit 64;;
esac

MAN="$PG_FREECAM_MANIFEST"
pg_manifest "$MAN" pipe
[[ -x "$PG_BIN"  ]] || { echo "cache-walk-ab: SKIP (no binary at $PG_BIN)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "cache-walk-ab: SKIP (no disc at $PG_DISC)"; exit 64; }
python3 -c 'import PIL' 2>/dev/null ||
    { echo "cache-walk-ab: SKIP (python3 or PIL missing)"; exit 64; }

[[ -n "$OUT" ]] || OUT="$BUILD_DIR/cache-walk-ab/$PG_GAME"
mkdir -p "$OUT"
CAPS=tests/harness/cache_walk_ab.caps
[[ -f "$CAPS" ]] || { echo "cache-walk-ab: FAIL (no caps file at $CAPS)"; exit 1; }

# The pinned post-load VBL the freecam corpus samples at.
START=1900

run_leg() {  # tag level pose shotflag faceid(0|1) extra-args...
    local tag="$1" level="$2" pose="$3" shotflag="$4" fid="$5"; shift 5
    local log="$OUT/$tag.log" shot="$OUT/$tag.png" shot_vk="$OUT/$tag-vk.png"
    rm -f "${OUT:?}/$tag.png" "${OUT:?}/$tag-vk.png"
    PG_CAPTURE_ENV=(OPENPETE_TERRAIN_CACHE_FORCE=1
                    OPENPETE_TERRAIN_GPU_CENSUS=1
                    OPENPETE_TERRAIN_NEARCEN=1
                    OPENPETE_TERRAIN_ANIMNEAR=1
                    OPENPETE_TERRAIN_CANIMNEAR=1)
    [[ "$fid" == 1 ]] && PG_CAPTURE_ENV+=(OPENPETE_TERRAIN_FACEID=1)
    PG_CAPTURE_TIMEOUT=900
    pg_capture "$log" "$shot_vk" -- \
        --no-card --mute 1 --vibration 0 \
        --renderer vk --lod extended --aspect 4:3 \
        --vertex-precision subpixel \
        --skip-to-level "$level" --freecam-pose "$pose" \
        --screenshot-native-only "$shotflag" "$START" \
        --screenshot-output "$shot" \
        --frame-limit $((START + 20)) --max-runtime-seconds 840 \
        "$@"
}

# The manifest is read into arrays before any run: a child that inherits the
# loop's stdin would eat the rows it has not reached yet.
LEVELS=(); POSES=(); SLUGS=(); SHOTS=()
while IFS='|' read -r level pose rest; do
    level="$(echo "$level" | tr -d ' \t')"
    [[ -z "$level" || "${level:0:1}" == "#" ]] && continue
    pose="$(echo "$pose" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    [[ -z "$pose" ]] && continue
    slug="$level-$(printf '%s' "$pose" | md5sum | cut -c1-6)"
    if [[ " $ROWS " == *" $slug "* ]]; then
        LEVELS+=("$level"); POSES+=("$pose"); SLUGS+=("$slug"); SHOTS+=(--screenshot-on-frame)
    fi
    if [[ " $ROWS " == *" $slug:inter "* ]]; then
        LEVELS+=("$level"); POSES+=("$pose"); SLUGS+=("$slug-inter")
        SHOTS+=(--screenshot-intermediate-on-frame)
    fi
done < "$MAN"

rows=0; pass=0; fail=0
for i in "${!SLUGS[@]}"; do
    level="${LEVELS[$i]}"; pose="${POSES[$i]}"; slug="${SLUGS[$i]}"; shot="${SHOTS[$i]}"
    rows=$((rows + 1))
    cap="$(awk -v g="$PG_GAME" -v r="$slug" '$1 == g && $2 == r { print $3 }' "$CAPS")"
    if [[ -z "$cap" ]]; then
        echo "[cache-walk-ab] row=$slug verdict=FAIL reason=no-cap ($CAPS)"; fail=$((fail+1)); continue
    fi
    if ! run_leg "$slug-a" "$level" "$pose" "$shot" 0 --terrain-cache 0; then
        echo "[cache-walk-ab] row=$slug verdict=NO-CAPTURE leg=a"; fail=$((fail+1)); continue
    fi
    if ! run_leg "$slug-b" "$level" "$pose" "$shot" 0; then
        echo "[cache-walk-ab] row=$slug verdict=NO-CAPTURE leg=b"; fail=$((fail+1)); continue
    fi
    if ! cmp -s "$OUT/$slug-a-vk.png" "$OUT/$slug-b-vk.png"; then
        # The edge/face split of the differing pixels needs both legs' face
        # identities at the same present.
        run_leg "$slug-a-fid" "$level" "$pose" "$shot" 1 --terrain-cache 0 &&
            run_leg "$slug-b-fid" "$level" "$pose" "$shot" 1 &&
            echo "[cache-walk-ab] row=$slug split $(python3 tools/terrain_hole_census.py split \
                "$OUT/$slug-a-fid-vk.png" "$OUT/$slug-b-fid-vk.png" \
                "$OUT/$slug-a-vk.png" "$OUT/$slug-b-vk.png" --components 0 | head -1)"
    fi
    python3 - "$OUT/$slug-a-vk.png" "$OUT/$slug-b-vk.png" \
              "$OUT/$slug-a.log" "$OUT/$slug-b.log" "$slug" "$START" "$cap" <<'AB_PY'
import re, sys
from PIL import Image

a_png, b_png, a_log, b_log, row, start, cap = sys.argv[1:8]
start, cap = int(start), int(cap)
inter = row.endswith("-inter")
cen = re.compile(r"\[tgpu-cen f=\d+\] sub=\d+ secs=(\d+)")
tri = re.compile(r"\[nearcen\] frame=\d+ nearFaces=\d+ subTris=\d+ terrainTris=(\d+)")
anl = re.compile(r"\[tgpu-cen f=(\d+)\] sub=(\d+) .* animnear=(\d+) animrecs=(\d+)"
                 r" canimnear=(\d+) coltab=(\d+) tabunits=\d+ ringsecs=(\d+) ringmv=(\d+)")

def peak(path, pat):
    v = 0
    for line in open(path, errors="replace"):
        m = pat.search(line)
        if m:
            v = max(v, int(m.group(1)))
    return v

secs_a, secs_b = peak(a_log, cen), peak(b_log, cen)
tris_a = peak(a_log, tri)
def at_capture(path):
    """animnear, animrecs, canimnear, coltab, ringsecs, ringmv of the census line of the captured present."""
    v = (0, 0, 0, 0, 0, 0)
    for line in open(path, errors="replace"):
        m = anl.search(line)
        if m and int(m.group(1)) <= start and int(m.group(2)) == int(inter):
            v = tuple(int(m.group(k)) for k in range(3, 9))
    return v

animnear_b, animrecs_b, canimnear_b, coltab_b, ringsecs_b, ringmv_b = at_capture(b_log)
A = Image.open(a_png).convert("RGB")
B = Image.open(b_png).convert("RGB")
if A.size != B.size:
    print("[cache-walk-ab] row=%s verdict=FAIL reason=size %s vs %s" % (row, A.size, B.size))
    sys.exit(1)
da, db = A.tobytes(), B.tobytes()
diff = 0
first_px = None
delta = 0
for i in range(0, len(da), 3):
    if da[i:i + 3] != db[i:i + 3]:
        diff += 1
        delta = max(delta, max(abs(da[i + k] - db[i + k]) for k in range(3)))
        if first_px is None:
            p = i // 3
            first_px = (p % A.size[0], p // A.size[0],
                        tuple(da[i:i + 3]), tuple(db[i:i + 3]))
# Engagement: the cache leg drew from the retained mesh and the walk leg drew
# terrain on the host without it. Either missing means the two feeds were not
# compared.
if secs_b == 0 or secs_a != 0 or tris_a == 0:
    print("[cache-walk-ab] row=%s verdict=VACUOUS px=%d secsB=%d secsA=%d trisA=%d"
          % (row, diff, secs_b, secs_a, tris_a))
    sys.exit(1)
# An :inter row exists to compare the lerps; a captured present that drew no
# previous-corner record, no colour table and no moving position-ring
# triangle compared none of them.
if inter and animrecs_b == 0 and coltab_b == 0 and ringmv_b == 0:
    print("[cache-walk-ab] row=%s verdict=VACUOUS px=%d animrecsB=0 coltabB=0 ringmvB=0" % (row, diff))
    sys.exit(1)
ok = diff <= cap
print("[cache-walk-ab] row=%s verdict=%s px=%d cap=%d maxChannelDelta=%d secsB=%d trisA=%d animnearB=%d "
      "animrecsB=%d canimnearB=%d coltabB=%d ringsecsB=%d ringmvB=%d%s%s"
      % (row, "PASS" if ok else "FAIL", diff, cap, delta, secs_b, tris_a, animnear_b, animrecs_b,
         canimnear_b, coltab_b, ringsecs_b, ringmv_b, "" if first_px is None else " first=%s" % (first_px,),
         " TIGHTEN cap %d -> %d" % (cap, diff) if diff < cap else ""))
sys.exit(0 if ok else 1)
AB_PY
    if [[ $? -eq 0 ]]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
done

echo "[cache-walk-ab] game=$PG_GAME rows=$rows pass=$pass fail=$fail verdict=$(
    if [[ $rows -eq 0 ]]; then echo VACUOUS
    elif [[ $fail -eq 0 ]]; then echo PASS
    else echo FAIL; fi)"
[[ $rows -gt 0 && $fail -eq 0 ]] || exit 1
exit 0
