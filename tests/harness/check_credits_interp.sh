#!/usr/bin/env bash
# check_credits_interp.sh - sub-tick interpolation of the credits-roll
# letters (--skip-to-cutscene credits / credits_100, GS_Credits with
# g_CreditsStage < 99).
#
# The roll draws two kinds of HUD-pool billboard moby at once: a settled
# line (static screen position, rotation 0) and a line flying in (position,
# depth and rotation all change every tick). Headless native-only captures
# at 4:3 with --render-paths moby isolate the letters on a flat backdrop.
#
# Checks, each with an engagement condition so a capture that drew nothing
# cannot pass:
#   identity  the intermediate re-extract at alpha 1 reproduces the canonical
#             dump of the same frame pixel for pixel.
#   settled   the settled line on the alpha-0.5 intermediate is pixel-
#             identical to one of its canonical neighbours (the game itself
#             moves the line by a pixel per tick, and the intermediate
#             re-extract carries the newer pose; a settled glyph paired with
#             a departed glyph's snapshot rotates and matches neither).
#   flying    the flying line's intermediate differs from BOTH neighbours,
#             and the rightmost glyph's centroid lies strictly between theirs
#             (a snapped position equals one neighbour).
#   keys      OPENPETE_INTERP_OBJS_KEYLOG: a HUD-pool key spawns only when a
#             line spawns - none in the window after a line lands (a key
#             renamed on the polybuf flip spawns every other tick).
#
# Two cases: the short roll early in its sequence (two lines on screen) and
# the full roll late in its sequence (three lines, and record slots reused
# ~60 times). Each case names its mid-flight canonical frame and the row
# bands (fractions of the frame height) holding the settled and flying
# lines, plus the line-spawn frame and the end of the quiet window for the
# key census.
#
# Runs as a leg of tests/harness/check_presentation_golden.sh (the ctest gate);
# standalone for iteration. Exit 0 PASS, 1 FAIL, 64 SKIP (binary / disc /
# python+PIL missing, or the game has no credits corpus).
#
# Usage: tests/harness/check_credits_interp.sh [--game SLUG]
set -u
export OPENPETE_VIBRATION=0
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
BIN="$PG_BIN"
DISC="$PG_DISC"
[[ -x "$BIN" && -f "$DISC" ]] || { echo "credits-interp: SKIP (binary or disc missing)"; exit 64; }
python3 -c 'import PIL, numpy' 2>/dev/null || { echo "credits-interp: SKIP (python3 PIL/numpy missing)"; exit 64; }

WORK="$(mktemp -d /tmp/openpete-credits-interp.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# target:C1:settled-band:flying-band:line-spawn:window-end:line-length. C1 is
# the middle canonical; captures are C1-2, C1-1 (intermediate), C1, C1+1
# (intermediate), C1+2. line-length is the number of letters the line
# spawning at line-spawn has: every one must spawn a key, including a letter
# that reuses a departed letter's record slot with the same class (the
# generation in the key). The short roll: "Universal Interactive Studios:"
# settled, an 11-letter name line flying in. The full roll, late: a
# 13-letter line whose spawn reuses 8 same-class slots.
# The cases are per game: each names a --skip-to-cutscene target, the frames
# the roll is mid-flight at and the row bands its lines occupy, all measured
# on that game's own credits sequence. A game with no measured credits target
# has no cases and the leg reports SKIP.
case "$GAME" in
  spyro-1) CASES="credits:4208:0.28,0.40:0.42,0.58:4168:4230:11 credits_100:7488:0.26,0.40:0.46,0.56:7448:7660:13";;
  *)       CASES="";;
esac
if [[ -z "$CASES" ]]; then
    echo "credits-interp: SKIP ($GAME has no credits corpus)"; exit 64; fi

# A leg that exits nonzero is kept as a crashed leg (scripts/presentation-game.sh,
# crashed legs): under the presentation golden in its run's crash directory,
# standalone in build/credits-interp-<game>.crashes.
pg_crash_init "build/credits-interp-$GAME"

run() {  # target leg out-flag out-name frame-list limit [env...]
    local target="$1" leg="$2" flag="$3" name="$4" frames="$5" lim="$6"; shift 6
    local legdir; legdir="$(pg_leg_begin)" || return 1
    local -a henv; mapfile -t henv < <(pg_leg_env "$legdir")
    local -a cmd=(env "$@" "${henv[@]}" timeout -k 10 600 "$BIN" --headless --no-config --disc "$DISC" --disc-cache 0 "${PG_RUN_ARGS[@]}"
        --renderer vk --mute 1 --aspect 4:3 --skip-to-cutscene "$target" --screenshot-native-only
        --render-paths moby "$flag" "$frames" --screenshot-output "$WORK/${target}_${name}_{n}.png"
        --frame-limit "$lim")
    op_own_run "${cmd[@]}" >"$legdir/leg.log" 2>&1
    local rc=$?
    cp -f "$legdir/leg.log" "$WORK/${target}_$leg.log"
    [[ $rc -ne 0 ]] && pg_keep_crash_leg "$legdir" "credits__${target}__$leg" "$rc" "${cmd[@]}"
    rm -rf -- "${legdir:?}"
    if pg_retry_owed "$rc"; then
        local f; local -a fs; IFS=, read -ra fs <<<"$frames"
        for f in "${fs[@]}"; do rm -f -- "${WORK:?}/${target}_${name}_$f-vk.png"; done
        return "$PG_LEG_RETRY_RC"
    fi
    return 0
}

# One leg under the retry rule (pg_leg_retry); a crashed first attempt's
# captures are deleted before the retry. args: as run
leg() { pg_leg_retry "credits__${1}__$2" run "$@"; }

rc=0
for case in $CASES; do
    IFS=: read -r T C1 SETB FLYB SPAWN WEND NLET <<<"$case"
    C0=$((C1-2)); I0=$((C1-1)); I1=$((C1+1)); C2=$((C1+2)); LIM=$((C1+4))
    leg "$T" canon --screenshot-on-frame canon "$C0,$C1,$C2" "$LIM"
    leg "$T" inter --screenshot-intermediate-on-frame inter "$I0,$I1" "$LIM"
    leg "$T" ident --screenshot-intermediate-on-frame ident "$C1" "$LIM" OPENPETE_INTERMEDIATE_ALPHA=1
    for f in canon_$C0 canon_$C1 canon_$C2 inter_$I0 inter_$I1 ident_$C1; do
        [[ -f "$WORK/${T}_$f-vk.png" ]] || { echo "credits-interp: FAIL [$T] capture missing $f"; exit 1; }
    done
    # Key census: keylog lines carry the frame; the intermediate request is
    # what enables per-object capture headless.
    leg "$T" keys --screenshot-intermediate-on-frame keys "$((WEND-1))" "$WEND" OPENPETE_INTERP_OBJS_KEYLOG=1

    python3 - "$WORK" "$T" "$C1" "$SETB" "$FLYB" "$SPAWN" "$WEND" "$NLET" <<'PY' || rc=1
import sys, re
import numpy as np
from PIL import Image
W, T = sys.argv[1], sys.argv[2]
C1, SPAWN, WEND, NLET = int(sys.argv[3]), int(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8])
setb = tuple(map(float, sys.argv[4].split(","))); flyb = tuple(map(float, sys.argv[5].split(",")))
C0, I0, I1, C2 = C1 - 2, C1 - 1, C1 + 1, C1 + 2
def load(n): return np.array(Image.open(f"{W}/{T}_{n}-vk.png").convert("RGB")).astype(int)
c0, i0, c1, i1, c2, ident = (load(n) for n in (f"canon_{C0}", f"inter_{I0}", f"canon_{C1}",
                                                f"inter_{I1}", f"canon_{C2}", f"ident_{C1}"))
H = c0.shape[0]
SET = slice(int(H * setb[0]), int(H * setb[1])); FLY = slice(int(H * flyb[0]), int(H * flyb[1]))
def glyph_mask(a): return (a[..., 0] > 90) & (a[..., 1] > 90) & (a[..., 2] < 80)
def blobs(a):
    cols = glyph_mask(a[FLY]).any(axis=0); out = []; s = None
    for x, v in enumerate(cols):
        if v and s is None: s = x
        if not v and s is not None: out.append((s + x) / 2 if x - s > 3 else None); s = None
    return [b for b in out if b is not None]
fails = []
def check(name, ok, detail):
    print(f"{'ok  ' if ok else 'FAIL'} [{T}] {name}: {detail}")
    if not ok: fails.append(name)

# Engagement: letters drawn in both bands; the flying line moves.
n_set = int(glyph_mask(c1[SET]).sum()); n_fly = int(glyph_mask(c1[FLY]).sum())
check("engage.settled", n_set > 200, f"{n_set} glyph px in the settled band")
check("engage.flying", n_fly > 100, f"{n_fly} glyph px in the flying band")
d_fly_cc = int((np.abs(c1[FLY] - c2[FLY]).sum(axis=2) > 0).sum())
check("engage.motion", d_fly_cc > 100, f"{d_fly_cc} px differ between canonical {C1} and {C2} in the flying band")

# identity
d_id = int((np.abs(ident - c1).sum(axis=2) > 0).sum())
check("identity", d_id == 0, f"{d_id} px differ, alpha-1 intermediate vs canonical {C1}")

# settled: no glyph rotates on the intermediate
s_prev = int((np.abs(i1[SET] - c1[SET]).sum(axis=2) > 0).sum())
s_next = int((np.abs(i1[SET] - c2[SET]).sum(axis=2) > 0).sum())
check("settled", min(s_prev, s_next) == 0,
      f"settled band, intermediate {I1} vs canonical {C1}: {s_prev} px differ, vs {C2}: {s_next} px")

# flying: not a snap to either neighbour, centroid between
d_prev = int((np.abs(i1[FLY] - c1[FLY]).sum(axis=2) > 0).sum())
d_next = int((np.abs(i1[FLY] - c2[FLY]).sum(axis=2) > 0).sum())
check("flying.moved", d_prev > 100 and d_next > 100,
      f"intermediate {I1} differs from canonical {C1} by {d_prev} px and from {C2} by {d_next} px")
b1, bi, b2 = blobs(c1), blobs(i1), blobs(c2)
ok = bool(b1 and bi and b2) and (min(b1[-1], b2[-1]) < bi[-1] < max(b1[-1], b2[-1]))
check("flying.between", ok, f"rightmost glyph centroid x: canonical {C1}={b1[-1] if b1 else None}, "
      f"intermediate {I1}={bi[-1] if bi else None}, canonical {C2}={b2[-1] if b2 else None}")

# keys: every letter of the new line spawns a key (a same-class slot reuse
# that kept its key would pair the new letter with the departed one), and
# none spawns afterwards
log = open(f"{W}/{T}_keys.log", errors="replace").read()
spawns = [(int(f), k) for f, k in re.findall(r"\[interp-objs\] f=(\d+) spawn key=([0-9a-f]{8})", log)]
pool = [(f, k) for f, k in spawns if k[0] in "456"]
at_line = [k for f, k in pool if f == SPAWN]
after = [(f, k) for f, k in pool if SPAWN < f <= WEND]
check("keys.spawn", len(at_line) == NLET, f"{len(at_line)} pool keys spawned at the line spawn ({SPAWN}), line has {NLET} letters")
check("keys.stable", len(after) == 0, f"{len(after)} pool-key spawns in {SPAWN+1}..{WEND}: {sorted(set(after))[:6]}")

if fails: print(f"credits-interp: FAIL [{T}]", ", ".join(fails)); sys.exit(1)
PY
done
[[ $rc -eq 0 ]] && echo "credits-interp: PASS ($GAME)"
exit $rc
