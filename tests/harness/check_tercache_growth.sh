#!/usr/bin/env bash
# check_tercache_growth.sh - the terrain cache's arena growth, consolidation,
# failed-edge and previous-tick store legs, headless on llvmpipe.
#
# Every run forces the cache on (OPENPETE_TERRAIN_CACHE_FORCE=1: software
# Vulkan does not select it alone) and reads the `tercache:` exit row, which
# the renderer prints at shutdown. A leg whose row reports bakes=0 compared
# nothing about the cache and is VACUOUS, never PASS.
#
# Legs (--leg selects one; the default set is level-edges, squeeze-bk2,
# consolidate, store, failed-edge, steady-pose, custom-level, rewind-hold,
# runahead-bk2, estimate-bound):
#   level-edges   the attract demo (boot to title, --frame-limit
#                 DEMO_LIMIT) at the content-estimate sizing: claims_sum=0,
#                 failed_edges=0, and svc_fulfilled equal to the level
#                 latches (`[tc-est] latch` lines: every level edge plus the
#                 session's first latch, each of which posts a replacement).
#                 The first-touch bake budget engages at the edges and
#                 closes: budget_claims>0 (the edge bakes exceed the
#                 per-tick budget), budget_open=0 (no walk still deferring
#                 at exit), budget_starved=0 (every tick that deferred a
#                 sector baked one: the budget admits the first candidate of
#                 a tick). budget_starved=0 is the bound on a deferring run:
#                 with at least one bake per walk, a run that opens on C
#                 candidates and receives A more while it lasts ends within
#                 C + A walks. No constant bounds the run's walk count
#                 (budget_walks_max, printed): host load sets the bakes per
#                 walk and the camera sets A.
#                 OPENPETE_TC_BAKE_BUDGET_US=off is the separating control
#                 (budget_claims=0).
#   squeeze-demo  the attract demo (boot to title, --frame-limit
#                 DEMO_LIMIT) with OPENPETE_TC_INITIAL_CAP squeezed below the
#                 levels' demand, so every arena must grow: claims_sum=0,
#                 grow>0, grow_claims>0, consolidations=0, dropped_spans=0,
#                 draws_retired=0.
#   squeeze-bk2   the same criteria on one corpus bk2 that crosses a level
#                 edge (Spyro 1: dark-passage-level-transition).
#   consolidate   OPENPETE_TC_BUF_MAX=2 with the squeeze, so the buffer cap
#                 is reached and consolidation runs: consolidations>0,
#                 hold_drops no higher than the stock run's, claims_sum=0;
#                 migrate_ticks is reported.
#   store         OPENPETE_TERRAIN_ANIMNEAR=1 and OPENPETE_TERRAIN_CANIMNEAR=1
#                 with OPENPETE_TC_PREV_CAP small (it overrides the
#                 level-edge store estimate) and the position ring's
#                 starting depth squeezed to 1 (OPENPETE_TC_RING_INITIAL=1),
#                 so the previous-tick store and the rings must grow:
#                 prev_grow>0, prev_fail=0, claims_sum=0, ring_grow>0;
#                 ring_writes=0 is VACUOUS. A ring run the squeezed store
#                 refuses while its growth is pending re-bakes the sector
#                 whole (ring_fail, ring_rebakes and prev_grow_claims are
#                 reported, not checked); steady-pose checks ring_fail=0
#                 at the store's own sizing.
#   failed-edge   OPENPETE_TC_REFUSE=edge:* on a run that crosses a level edge
#                 with a held scene: failed_edges>0, consolidations>0,
#                 hold_takes>0, sealed_allocs=0, hold_drops=0, claims_sum=0;
#                 sealed_draws is reported (it needs a hold valid across
#                 the refused edge).
#   refuse-grow   OPENPETE_TC_REFUSE=grow:*,cons:* with the squeeze: every
#                 growth and consolidation refused. Reports the claims the
#                 refusal policy leaves (the exhaustion case); no verdict.
#   refuse-ring   the steady-pose run with OPENPETE_TC_REFUSE=ring:* and
#                 OPENPETE_TC_RING_INITIAL=1: every position-ring run
#                 refused, so every poly-anim sector bakes its positions
#                 whole: claims_sum=0, ring_writes=0, ring_fail>0,
#                 ring_rebakes>0.
#   steady-pose   Spyro 2 skelos_badlands at a freecam pose under
#                 --texture-mapping affine and --lod extended, where the
#                 poly-anim cell-record arena (CA_OPAQUE) re-bakes every
#                 build and outgrows its content estimate, run
#                 STEADY_LIMIT VBLs (the level loads near VBL 1290 and
#                 settles within 300 builds): claims_sum=0,
#                 consolidations=0, exhaust_warnings=0, and at most
#                 STEADY_REQ growth requests per arena after the level
#                 load (7: a growth series that fits the 8 slots; an arena
#                 that never settles requests without bound). The
#                 position rings: ring_fail=0, ring_rebakes=0;
#                 ring_writes=0 is VACUOUS.
#   custom-level  the M4 fixture, built at run time from the disc: a scratch
#                 mod whose WAD is lofty_castle's (level 52) served with
#                 `replace = true` over the donor toasty (sunny_flight, the
#                 planned donor, hangs at load), at the freecam topdown pose.
#                 The arenas are sized from the level's own sector
#                 headers: claims_sum=0, window_bakes=0, exhaust_warnings=0.
#   rewind-hold   Spyro 3 sunny-villa-level-entry with --rewind 30 and a
#                 held rewind (--rewind-hold 2427:300). Each rewind step
#                 replays its ticks from a keyframe; only the last replayed
#                 tick's scene is presented. Over the hold (the [tcf] bake
#                 lines from the first to the last `rewind: hold` line, one
#                 step per line) a step bakes each sector at most once:
#                 hold_repeats=0, the bakes past the step's distinct sectors
#                 summed (hold_bakes and hold_distinct are reported, and
#                 hold_bakes=0 is VACUOUS). The arenas and the position ring
#                 stay at the stock run's sizing: afail_sum=0 (the last
#                 [ter-gpu] afail column summed), ring_grow=0,
#                 ring_depth_max<7. A binary that reports the rewind
#                 summary's presented draws replayed unseen must report 0
#                 (unseen_last).
#   runahead-bk2  Spyro 1 dark-passage-level-transition under --runahead 2
#                 with OPENPETE_RA_PREDICT_CHECK=1. A host iteration runs
#                 the committed tick and a speculation, and only the
#                 speculation head's scene is presented. Over the run (the
#                 [tcf] bake lines between consecutive `[canonscene` lines,
#                 one presented scene per interval) a presented scene bakes
#                 each sector at most once: ra_repeats=0, the bakes past
#                 each interval's distinct sectors summed (ra_bakes and
#                 ra_scenes are reported; ra_scenes=0 or ra_bakes=0 is
#                 VACUOUS). afail_sum=0, ring_depth_max<7, the
#                 `[ra-predict]` row present with mismatch=0, and a binary
#                 that reports the runahead summary's presented draws built
#                 unseen must report 0 past those on a speculation that left
#                 gameplay (ra_unseen_presented; ra_unseen_left, a draw the
#                 departure made the last, is reported).
#   estimate-bound
#                 OPENPETE_TC_BOUND_CENSUS=1 on one level per game whose
#                 table bakes split faces in a blend class (Spyro 1
#                 crystal_flight: ALPHA; Spyro 2 idol_springs and Spyro 3
#                 sunrise_spring: ADD, A_ADD, CA_OPAQUE), BOUND_LIMIT VBLs:
#                 every `[tc-bound]` line's content estimate is at least the
#                 exact dry bake on every arena, the CN family summed
#                 (tests/harness/tercache_bound_census.py); no line, or no
#                 line with a nonzero bake, is VACUOUS.
#
# --exe PATH drives another binary with the same legs (a build without
# growth reports claims>0 on the squeeze legs, which is the separation).
#
# Usage:
#   tests/harness/check_tercache_growth.sh [--game SLUG] [--leg NAME]
#        [--exe PATH] [--out DIR] [--squeeze N] [--demo-limit N]
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (no binary, no disc, or no leg for
# the game).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

export OPENPETE_VIBRATION=0

LEG=""; EXE=""; OUT=""; SQUEEZE=32768; DEMO_LIMIT=40000; STEADY_LIMIT=2400; STEADY_REQ=7; BOUND_LIMIT=1756
prev=""
for a in "$@"; do
    case "$prev" in
        --leg) LEG="$a";; --exe) EXE="$a";; --out) OUT="$a";;
        --squeeze) SQUEEZE="$a";; --demo-limit) DEMO_LIMIT="$a";;
    esac
    prev="$a"
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
[[ -n "$EXE" ]] || EXE="$PG_BIN"
[[ -x "$EXE" ]] || { echo "tercache-growth: SKIP (no binary at $EXE)"; exit 64; }
[[ -f "$PG_DISC" ]] || { echo "tercache-growth: SKIP (no disc at $PG_DISC)"; exit 64; }
[[ -n "$OUT" ]] || OUT="${TMPDIR:-/tmp}/tercache-growth/$GAME"
mkdir -p "$OUT"

# Per-game rows: the level a spawn leg loads, the bk2 the squeeze-bk2
# leg replays, and the level the estimate-bound leg loads.
case "$GAME" in
    spyro-1) SPAWN=artisans; BK2=dark-passage-level-transition; STORE_AREA=artisans; BOUND_AREA=crystal_flight;;
    spyro-2) SPAWN=summer_forest; BK2=""; STORE_AREA=idol_springs; BOUND_AREA=idol_springs;;
    spyro-3) SPAWN=sunrise_spring; BK2=""; STORE_AREA=seashell_shore; BOUND_AREA=sunrise_spring;;
esac

# One run: deletes its log first and asserts the exit row after it.
run_leg() {  # tag frame-limit envs... -- args...
    local tag="$1" limit="$2"; shift 2
    local -a envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    local log="$OUT/$tag.log"
    rm -f "$log"
    env OPENPETE_VIBRATION=0 OPENPETE_HEADLESS_NATIVE=1 OPENPETE_TERRAIN_CACHE_FORCE=1 \
        "${envs[@]}" timeout 3600 "$EXE" "${PG_RUN_ARGS[@]}" --headless --no-config \
        --no-card --mute 1 --vibration 0 --renderer vk --disc-cache 0 --disc "$PG_DISC" \
        --frame-limit "$limit" "$@" < /dev/null > "$log" 2>&1
    local rc=$?
    [[ -s "$log" ]] || { echo "  leg $tag produced no log"; return 1; }
    return $rc
}

# A field of the leg's `tercache:` or `tercache-ring:` row (the last of
# each), or empty.
field() {  # tag name
    { grep -o '^tercache: .*' "$OUT/$1.log" 2>/dev/null | tail -1
      grep -o '^tercache-ring: .*' "$OUT/$1.log" 2>/dev/null | tail -1; } |
        tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

fail=0
ran=0
want() { [[ "$LEG" == "$1" || ( -z "$LEG" && " level-edges squeeze-bk2 consolidate store failed-edge steady-pose custom-level rewind-hold runahead-bk2 estimate-bound " == *" $1 "* ) ]]; }
# check tag expr...: each expr is name<op>value over the row's fields.
check() {
    local tag="$1"; shift
    local bakes; bakes="$(field "$tag" bakes)"
    echo "[tercache-growth] leg=$tag $(grep -o '^tercache: .*' "$OUT/$tag.log" | tail -1 | cut -c11-)"
    echo "[tercache-growth] leg=$tag $(grep -o '^tercache-ring: .*' "$OUT/$tag.log" | tail -1)"
    if [[ -z "$bakes" || "$bakes" == 0 ]]; then
        echo "[tercache-growth] leg=$tag VACUOUS (no tercache: row or bakes=0)"; fail=1; return
    fi
    local e v want_v op ok
    for e in "$@"; do
        if [[ "$e" =~ ^([a-z_]+)(=|\>|\<)([0-9]+)$ ]]; then
            v="$(field "$tag" "${BASH_REMATCH[1]}")"; op="${BASH_REMATCH[2]}"; want_v="${BASH_REMATCH[3]}"
            ok=0
            case "$op" in
                =) [[ "${v:-x}" == "$want_v" ]] && ok=1;;
                \>) [[ -n "$v" && "$v" -gt "$want_v" ]] && ok=1;;
                \<) [[ -n "$v" && "$v" -lt "$want_v" ]] && ok=1;;
            esac
            [[ "$ok" == 1 ]] || { echo "[tercache-growth] leg=$tag FAIL ($e, read ${v:-none})"; fail=1; }
        fi
    done
}

SQ=(OPENPETE_TC_INITIAL_CAP="$SQUEEZE")
GROW_PASS=(claims_sum=0 grow\>0 grow_claims\>0 consolidations=0 dropped_spans=0 draws_retired=0)

if want level-edges; then
    ran=1
    run_leg level-edges "$DEMO_LIMIT" --
    latches="$(grep -c '^\[tc-est\] latch level=' "$OUT/level-edges.log")"
    echo "[tercache-growth] leg=level-edges latches=$latches (level edges + 1)"
    check level-edges claims_sum=0 failed_edges=0 "svc_fulfilled=$latches" \
        budget_claims\>0 budget_open=0 budget_starved=0
    [[ "$latches" -gt 1 ]] ||
        { echo "[tercache-growth] leg=level-edges VACUOUS (no level edge crossed)"; fail=1; }
fi

if want squeeze-demo; then
    ran=1
    run_leg squeeze-demo "$DEMO_LIMIT" "${SQ[@]}" --
    check squeeze-demo "${GROW_PASS[@]}"
fi

if want squeeze-bk2; then
    if [[ -z "$BK2" ]]; then
        echo "[tercache-growth] leg=squeeze-bk2 SKIP (no bk2 row for $GAME)"
    else
        ran=1
        mapfile -t BK2_ARGS < <(grep -v '^#' "games/$GAME/bk2/$BK2.bk2.args" | tr ' ' '\n' | sed '/^$/d')
        run_leg squeeze-bk2 6000 "${SQ[@]}" -- --bk2 "games/$GAME/bk2/$BK2.bk2" --bk2-stop-on-end "${BK2_ARGS[@]}"
        check squeeze-bk2 "${GROW_PASS[@]}"
    fi
fi

if want consolidate; then
    ran=1
    run_leg consolidate 1900 OPENPETE_TC_BUF_MAX=2 "${SQ[@]}" -- --skip-to-level "$SPAWN"
    check consolidate consolidations\>0 hold_drops\<2 claims_sum=0 dropped_spans=0
fi

if want store; then
    ran=1
    run_leg store 1900 OPENPETE_TERRAIN_ANIMNEAR=1 OPENPETE_TERRAIN_CANIMNEAR=1 \
        OPENPETE_TC_PREV_CAP=256 OPENPETE_TC_RING_INITIAL=1 -- --skip-to-level "$STORE_AREA" --lod extended
    check store prev_grow\>0 prev_fail=0 claims_sum=0 ring_grow\>0
    ring_vacuous store
fi

if want failed-edge; then
    ran=1
    run_leg failed-edge 1900 OPENPETE_TC_REFUSE='edge:*' -- --skip-to-level "$SPAWN"
    check failed-edge failed_edges\>0 consolidations\>0 hold_takes\>0 sealed_allocs=0 \
        hold_drops=0 claims_sum=0
fi

# A leg that ran no ring sector compared nothing about the position ring.
ring_vacuous() {  # tag
    local w; w="$(field "$1" ring_writes)"
    if [[ -z "$w" || "$w" == 0 ]]; then
        echo "[tercache-growth] leg=$1 VACUOUS (ring_writes=${w:-none})"; fail=1
    fi
}

if want steady-pose; then
    if [[ "$GAME" != spyro-2 ]]; then
        echo "[tercache-growth] leg=steady-pose SKIP (the pose is a Spyro 2 row)"
    else
        ran=1
        run_leg steady-pose "$STEADY_LIMIT" -- --skip-to-level skelos_badlands \
            --freecam-pose "pos=(116034, 2704, 95746) yaw=1112 pitch=541" \
            --texture-mapping affine --lod extended
        check steady-pose claims_sum=0 consolidations=0 exhaust_warnings=0 ring_fail=0 ring_rebakes=0
        ring_vacuous steady-pose
        # Growth and consolidation requests per arena after the level load.
        req="$(sed -n '/loader took level=/,$p' "$OUT/steady-pose.log" |
            sed -n 's/^\[ter-gpu\] arena \([0-9]*\) \(grow\|consolidation\) request.*/\1/p' |
            sort | uniq -c | sort -rn | head -1)"
        read -r n arena <<< "${req:-0 -}"
        echo "[tercache-growth] leg=steady-pose most requests after the level load: $n (arena $arena)"
        if ! grep -q 'loader took level=' "$OUT/steady-pose.log"; then
            echo "[tercache-growth] leg=steady-pose VACUOUS (the level never loaded)"; fail=1
        elif [[ "$n" -gt "$STEADY_REQ" ]]; then
            echo "[tercache-growth] leg=steady-pose FAIL (arena $arena: $n requests > $STEADY_REQ)"; fail=1
        fi
    fi
fi

if want refuse-grow; then
    ran=1
    run_leg refuse-grow 1900 "${SQ[@]}" OPENPETE_TC_REFUSE='grow:*,cons:*' -- --skip-to-level "$SPAWN"
    check refuse-grow
fi

if want refuse-ring; then
    if [[ "$GAME" != spyro-2 ]]; then
        echo "[tercache-growth] leg=refuse-ring SKIP (the pose is a Spyro 2 row)"
    else
        ran=1
        run_leg refuse-ring "$STEADY_LIMIT" OPENPETE_TC_RING_INITIAL=1 OPENPETE_TC_REFUSE='ring:*' -- \
            --skip-to-level skelos_badlands \
            --freecam-pose "pos=(116034, 2704, 95746) yaw=1112 pitch=541" \
            --texture-mapping affine --lod extended
        check refuse-ring claims_sum=0 ring_writes=0 ring_fail\>0 ring_rebakes\>0
    fi
fi

# The held-rewind row: bakes per rewind step against the step's distinct
# sectors, and the arena and ring census at exit.
if want rewind-hold; then
    if [[ "$GAME" != spyro-3 ]]; then
        echo "[tercache-growth] leg=rewind-hold SKIP (the bk2 is a Spyro 3 row)"
    else
        ran=1
        RB=sunny-villa-level-entry
        mapfile -t RB_ARGS < <(grep -v '^#' "games/$GAME/bk2/$RB.bk2.args" | tr ' ' '\n' | sed '/^$/d')
        run_leg rewind-hold 20000 OPENPETE_TERRAIN_GPU_DBG=1 OPENPETE_TC_DBG=1 -- \
            --bk2 "games/$GAME/bk2/$RB.bk2" --bk2-stop-on-end "${RB_ARGS[@]}" \
            --rewind 30 --rewind-hold 2427:300
        read -r hb hd hr hs < <(awk '
            /rewind: hold [0-9]+\/[0-9]+ at/ {
                if (on) { b += nb; d += nd; s++ }
                on = 1; nb = 0; nd = 0; delete seen
                split($0, a, "hold "); split(a[2], q, "/"); split(q[2], r, " ")
                if (q[1] == r[1]) on = 0
                next }
            on && /^\[tcf\]/ {
                nb++; m = $0; sub(/.* sec=/, "", m); sub(/ .*/, "", m)
                if (!(m in seen)) { seen[m] = 1; nd++ } }
            END { print b + 0, d + 0, b - d, s + 0 }' "$OUT/rewind-hold.log")
        afail="$(grep -o 'afail=[0-9,]*' "$OUT/rewind-hold.log" | tail -1 | cut -d= -f2 |
            tr ',' '\n' | awk '{ t += $1 } END { print t + 0 }')"
        unseen_last="$(sed -n 's/.*presented draws replayed unseen: \([0-9]*\).*/\1/p' "$OUT/rewind-hold.log" | tail -1)"
        echo "[tercache-growth] leg=rewind-hold steps=$hs hold_bakes=$hb hold_distinct=$hd hold_repeats=$hr afail_sum=$afail unseen_last=${unseen_last:-none}"
        check rewind-hold ring_grow=0 ring_depth_max\<7
        [[ "$hs" -gt 0 && "$hb" -gt 0 ]] ||
            { echo "[tercache-growth] leg=rewind-hold VACUOUS (steps=$hs hold_bakes=$hb)"; fail=1; }
        [[ "$hr" == 0 ]] || { echo "[tercache-growth] leg=rewind-hold FAIL (hold_repeats=0, read $hr)"; fail=1; }
        [[ "$afail" == 0 ]] || { echo "[tercache-growth] leg=rewind-hold FAIL (afail_sum=0, read $afail)"; fail=1; }
        [[ "${unseen_last:-0}" == 0 ]] ||
            { echo "[tercache-growth] leg=rewind-hold FAIL (unseen_last=0, read $unseen_last)"; fail=1; }
    fi
fi

# The runahead row: bakes per presented scene against the scene's distinct
# sectors, the prediction check and the arena and ring census at exit.
if want runahead-bk2; then
    if [[ "$GAME" != spyro-1 ]]; then
        echo "[tercache-growth] leg=runahead-bk2 SKIP (the bk2 is a Spyro 1 row)"
    else
        ran=1
        mapfile -t RA_ARGS < <(grep -v '^#' "games/$GAME/bk2/$BK2.bk2.args" | tr ' ' '\n' | sed '/^$/d')
        run_leg runahead-bk2 6000 OPENPETE_TERRAIN_GPU_DBG=1 OPENPETE_TC_DBG=1 \
            OPENPETE_FREEZE_LEG_PROBE=1 OPENPETE_RA_PREDICT_CHECK=1 -- \
            --bk2 "games/$GAME/bk2/$BK2.bk2" --bk2-stop-on-end "${RA_ARGS[@]}" --runahead 2
        read -r rb rd rr rs < <(awk '
            /^\[canonscene f=/ { b += nb; d += nd; s++; nb = 0; nd = 0; delete seen; next }
            /^\[tcf\]/ {
                nb++; m = $0; sub(/.* sec=/, "", m); sub(/ .*/, "", m)
                if (!(m in seen)) { seen[m] = 1; nd++ } }
            END { print b + 0, d + 0, b - d, s + 0 }' "$OUT/runahead-bk2.log")
        afail="$(grep -o 'afail=[0-9,]*' "$OUT/runahead-bk2.log" | tail -1 | cut -d= -f2 |
            tr ',' '\n' | awk '{ t += $1 } END { print t + 0 }')"
        mism="$(sed -n 's/^\[ra-predict\] checked=[0-9]* mismatch=\([0-9]*\).*/\1/p' "$OUT/runahead-bk2.log" | tail -1)"
        checked="$(sed -n 's/^\[ra-predict\] checked=\([0-9]*\).*/\1/p' "$OUT/runahead-bk2.log" | tail -1)"
        unseen_pres="$(sed -n 's/.*presented draws built unseen: \([0-9]*\).*/\1/p' "$OUT/runahead-bk2.log" | tail -1)"
        unseen_left="$(sed -n 's/.*presented draws built unseen: [0-9]* (\([0-9]*\) on a speculation that left.*/\1/p' "$OUT/runahead-bk2.log" | tail -1)"
        [[ -n "$unseen_pres" ]] && unseen_pres=$((unseen_pres - ${unseen_left:-0}))
        echo "[tercache-growth] leg=runahead-bk2 scenes=$rs ra_bakes=$rb ra_distinct=$rd ra_repeats=$rr afail_sum=$afail ra_predict_checked=${checked:-none} mismatch=${mism:-none} ra_unseen_presented=${unseen_pres:-none} ra_unseen_left=${unseen_left:-none}"
        check runahead-bk2 ring_depth_max\<7
        [[ "$rs" -gt 0 && "$rb" -gt 0 ]] ||
            { echo "[tercache-growth] leg=runahead-bk2 VACUOUS (scenes=$rs ra_bakes=$rb)"; fail=1; }
        [[ "${checked:-0}" -gt 0 ]] ||
            { echo "[tercache-growth] leg=runahead-bk2 VACUOUS (ra-predict checked=${checked:-none})"; fail=1; }
        [[ "$rr" == 0 ]] || { echo "[tercache-growth] leg=runahead-bk2 FAIL (ra_repeats=0, read $rr)"; fail=1; }
        [[ "$afail" == 0 ]] || { echo "[tercache-growth] leg=runahead-bk2 FAIL (afail_sum=0, read $afail)"; fail=1; }
        [[ "${mism:-x}" == 0 ]] || { echo "[tercache-growth] leg=runahead-bk2 FAIL (mismatch=0, read ${mism:-none})"; fail=1; }
        [[ "${unseen_pres:-0}" == 0 ]] ||
            { echo "[tercache-growth] leg=runahead-bk2 FAIL (ra_unseen_presented=0, read $unseen_pres)"; fail=1; }
    fi
fi

# The arena estimate against an exact dry bake of the level's tables.
if want estimate-bound; then
    ran=1
    run_leg estimate-bound "$BOUND_LIMIT" OPENPETE_TC_BOUND_CENSUS=1 -- \
        --skip-to-level "$BOUND_AREA" --lod original
    check estimate-bound
    python3 tests/harness/tercache_bound_census.py "$OUT/estimate-bound.log" |
        sed 's/^/[tercache-growth] leg=estimate-bound /'
    [[ "${PIPESTATUS[0]}" == 0 ]] || fail=1
fi

# The M4 fixture: lofty_castle's WAD (level 52) as a replace-mode custom
# level over the donor toasty, at the freecam topdown pose.
custom_level() {  # tag envs... -- checks...
    local tag="$1"; shift
    local -a envs=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do envs+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    if [[ "$GAME" != spyro-1 ]]; then
        echo "[tercache-growth] leg=$tag SKIP (custom levels run on Spyro 1 only)"; return
    fi
    ran=1
    MODS="$OUT/m4mods"
    rm -rf "${MODS:?}"
    mkdir -p "$MODS/m4big/assets/levels"
    python3 tools/extract_wad_entry.py --disc "$PG_DISC" --level 52 \
        --out "$MODS/m4big/assets/levels/big.wad" > /dev/null ||
        { echo "[tercache-growth] leg=$tag FAIL (WAD extraction)"; fail=1; }
    printf '[mod]\nid      = "m4big"\nname    = "M4 big"\nversion = "1.0.0"\ngame    = "spyro-1"\napi     = 1\n\n[[level]]\nname  = "M4 BIG"\nfiles = "big"\ndonor = "toasty"\nreplace = true\n' \
        > "$MODS/m4big/mod.toml"
    run_leg "$tag" 2400 "${envs[@]}" -- --mods-dir "$MODS" --skip-to-level toasty \
        --lod extended --freecam-pose topdown
    check "$tag" "$@"
}

want custom-level && custom_level custom-level -- \
    claims_sum=0 window_bakes=0 exhaust_warnings=0

[[ "$ran" == 1 ]] || { echo "tercache-growth: SKIP (no leg ran)"; exit 64; }
if [[ "$fail" == 0 ]]; then echo "tercache-growth: PASS"; exit 0; fi
echo "tercache-growth: FAIL"
exit 1
