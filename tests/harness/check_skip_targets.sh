#!/usr/bin/env bash
# check_skip_targets.sh - the skip-target row harness.
#
# Runs every row of games/<slug>/corpus/skip-targets.manifest headless (a
# mode:windowed row windowed, below). Each row is
# a target of --skip-to-cutscene (or --skip-to-area); the skip module prints
# one release line, "[skip-to-level] level playable - presents resume at
# vbl=N" followed by the engagement values it reads back from RAM at the
# release edge (the cutscene index, the credits sequence, the return level,
# the area word), or its fail-safe line, "[skip-to-level] fail-safe release
# (no playable edge)", when the deadline passes first.
#
# A row passes only when its run exits 0, prints the release line, the
# release line carries every expect token of the row, and the run prints no
# fail-safe line. No release line, the fail-safe line, a missing token or a
# nonzero exit fails the row.
#
# Negative control: the first row runs a second time with
# OPENPETE_SKIP_FAILSAFE_VBL=1, which moves the skip's fail-safe deadline to
# one VBL after its first check. That run must classify as FAIL; a PASS there
# means the classifier cannot see a skip that never engaged, and the harness
# fails.
#
# Manifest fields, pipe-separated: name | args | frames | expect
#   args    the skip flags of the row (word-split)
#   frames  --frame-limit of the run, in VBLs
#   expect  whitespace-separated tokens. A plain token must occur in the
#           release line. Log tokens test the whole log, with `~` standing
#           for a space inside TEXT:
#             log:TEXT      some log line contains TEXT
#             log:A>>B      a line containing A precedes the first line
#                           containing B, and B occurs
#             !log:TEXT     no log line contains TEXT
#           The token vbl<=N requires the release line's vbl= value to be
#           at most N.
#           The token mode:windowed runs the row windowed instead (below).
#
# Windowed rows (mode:windowed): the run opens a window on a virtual X
# display (scripts/lib/xvfb.sh: xvfb-run -a, SDL_VIDEODRIVER=x11) with the
# native renderer, --input-lock --vibration 0 and
# OPENPETE_CARRY_EPOCH_PROBE=1, so the skip's present hold engages, which a
# headless run never does. Beyond the release-line test the row passes only
# when the log has no OVERLAY_CARRY_MAX line (the 2D carry set overflowed at
# the first present after the hold), has a "held presents=" line (the hold
# held at least one present), and the first "[carry" probe line with a
# mismatch field after the release line reads mismatch=0. The token
# carry_f>=N moves that probe line to the first one after the release whose
# f= (openpete_frame) is at least N. A missing
# "held presents=" line or probe line after the release is VACUOUS: the hold
# or the probe never engaged. A missing xvfb-run fails the row.
#
#   tests/harness/check_skip_targets.sh [--game SLUG] [--build-dir=DIR] [--filter=NAME]
#
# Exit 0 PASS, 1 FAIL or a missing corpus manifest (the message names the
# path), 64 SKIP (binary or disc missing). At most
# SKIP_TARGETS_JOBS (default 2) game processes run at once.
set -uo pipefail
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh
export OPENPETE_VIBRATION=0

BUILD="build/linux-release"; FILTER=""
for a in "$@"; do
    case "$a" in
        --build-dir=*) BUILD="${a#--build-dir=}";;
        --filter=*)    FILTER="${a#--filter=}";;
    esac
done
GAME="$(pg_parse_game "$@")" || exit 64
PG_BUILD_DIR="$BUILD" pg_resolve "$GAME" || exit 64
MANIFEST="$(pg_corpus_manifest "$GAME" skip-targets.manifest)"
pg_manifest "$MANIFEST" pipe
OUT="${SKIP_TARGETS_OUT:-$BUILD/skip-targets/$GAME}"
JOBS="${SKIP_TARGETS_JOBS:-2}"

if [[ ! -x "$PG_BIN" || ! -f "$PG_DISC" ]]; then
    echo "check_skip_targets: SKIP ($PG_BIN or $PG_DISC missing)"; exit 64
fi

mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/*.rc

NAMES=(); ARGS=(); FRAMES=(); EXPECT=()
while IFS='|' read -r name args frames expect; do
    name="${name//[[:space:]]/}"
    [[ -z "$name" || "$name" == \#* ]] && continue
    [[ -n "$FILTER" && "$name" != "$FILTER" ]] && continue
    NAMES+=("$name"); ARGS+=("$(echo $args)")
    FRAMES+=("${frames//[[:space:]]/}"); EXPECT+=("$(echo $expect)")
done < "$MANIFEST"
if (( ${#NAMES[@]} == 0 )); then
    echo "check_skip_targets: FAIL (no rows in $MANIFEST${FILTER:+ matching $FILTER})"; exit 1
fi

# windowed EXPECT: status 0 when the row's expect field holds mode:windowed.
windowed() { [[ " $1 " == *' mode:windowed '* ]]; }

# run TAG ARGS FRAMES EXPECT [ENV...]: one session, headless or (mode:windowed)
# on a virtual X display; log and exit code under $OUT.
run() {
    local tag="$1" args="$2" frames="$3" expect="$4"; shift 4
    local -a argv; read -ra argv <<<"$args"
    if windowed "$expect"; then
        if ! op_xvfb_set 1280x720; then
            echo "$OP_XVFB_WHY" >"$OUT/$tag.log"; echo 127 >"$OUT/$tag.rc"; return
        fi
        env OPENPETE_CARRY_EPOCH_PROBE=1 "$@" \
            timeout -k 10 900 "${OP_XVFB[@]}" \
            "$PG_BIN" --no-config --disc "$PG_DISC" "${PG_RUN_ARGS[@]}" \
            --no-card --mute 1 --input-lock --vibration 0 --renderer vk \
            --lod original --osd off --version-osd off --perf-stats off \
            --window 1280x720 "${argv[@]}" \
            --frame-limit "$frames" >"$OUT/$tag.log" 2>&1
    else
        env "$@" timeout -k 10 900 "$PG_BIN" --headless --no-config --disc "$PG_DISC" \
            "${PG_RUN_ARGS[@]}" --no-card --mute 1 "${argv[@]}" \
            --frame-limit "$frames" >"$OUT/$tag.log" 2>&1
    fi
    echo $? >"$OUT/$tag.rc"
}

# windowed_verdict LOG EXPECT: prints "PASS <held line> | <probe line>" when LOG
# meets the windowed-row criteria (header), else "FAIL <reason>" or
# "VACUOUS <reason>".
windowed_verdict() {
    local log="$1" expect="$2" rl held carry tok minf=0
    for tok in $expect; do [[ "$tok" == 'carry_f>='* ]] && minf="${tok#'carry_f>='}"; done
    if grep -q 'OVERLAY_CARRY_MAX' "$log"; then
        echo "FAIL OVERLAY_CARRY_MAX line: $(grep -m1 'OVERLAY_CARRY_MAX' "$log")"; return
    fi
    held="$(grep -m1 'held presents=' "$log")"
    [[ -n "$held" ]] || { echo "VACUOUS no 'held presents=' line (no present was held)"; return; }
    rl="$(grep -n -m1 '\[skip-to-level\] level playable' "$log" | cut -d: -f1)"
    carry="$(tail -n +"$((rl + 1))" "$log" | grep '\[carry f=.* mismatch=' |
             awk -v m="$minf" '{f = $2; sub(/^f=/, "", f); sub(/]$/, "", f)}
                               f + 0 >= m + 0 {print; exit}')"
    [[ -n "$carry" ]] || { echo "VACUOUS no [carry probe line after the release line"; return; }
    [[ "$carry" == *' mismatch=0 '* ]] || { echo "FAIL first [carry line after release: $carry"; return; }
    echo "PASS $held | $carry"
}

# log_token LOG TOKEN: status 0 when LOG meets the log token (see the
# expect field above).
log_token() {
    local log="$1" tok="$2" body a b na nb
    if [[ "$tok" == '!log:'* ]]; then
        body="${tok#'!log:'}"; body="${body//\~/ }"
        ! grep -qF -- "$body" "$log"; return
    fi
    body="${tok#log:}"; body="${body//\~/ }"
    if [[ "$body" != *'>>'* ]]; then grep -qF -- "$body" "$log"; return; fi
    a="${body%%>>*}"; b="${body#*>>}"
    na="$(grep -nF -m1 -- "$a" "$log" | cut -d: -f1)"
    nb="$(grep -nF -m1 -- "$b" "$log" | cut -d: -f1)"
    [[ -n "$na" && -n "$nb" ]] && (( na < nb ))
}

# classify TAG EXPECT: prints "PASS <line>" or "FAIL <reason>".
classify() {
    local tag="$1" expect="$2" log="$OUT/$1.log" rc line tok rv
    [[ -f "$log" && -f "$OUT/$tag.rc" ]] || { echo "FAIL no output"; return; }
    rc="$(cat "$OUT/$tag.rc")"
    if grep -q '\[skip-to-level\] fail-safe release' "$log"; then
        echo "FAIL fail-safe line: $(grep -m1 'fail-safe release' "$log")"; return
    fi
    line="$(grep -m1 '\[skip-to-level\] level playable' "$log")"
    [[ -n "$line" ]] || { echo "FAIL no release line (exit $rc)"; return; }
    for tok in $expect; do
        case "$tok" in
            mode:windowed|'carry_f>='*) ;;
            'vbl<='*)
                rv="${line#* at vbl=}"; rv="${rv%% *}"
                [[ "$rv" =~ ^[0-9]+$ ]] && (( rv <= ${tok#'vbl<='} )) ||
                    { echo "FAIL release vbl=$rv above ${tok#'vbl<='}: $line"; return; };;
            '!log:'*|log:*)
                log_token "$log" "$tok" || { echo "FAIL log token '$tok' unmet: $line"; return; };;
            *)
                [[ " $line " == *" $tok "* ]] || { echo "FAIL token '$tok' absent: $line"; return; };;
        esac
    done
    if windowed "$expect"; then
        local wv; wv="$(windowed_verdict "$log" "$expect")"
        [[ "$wv" == PASS* ]] || { echo "$wv"; return; }
        line="$line | ${wv#PASS }"
    fi
    [[ "$rc" == 0 ]] || { echo "FAIL exit $rc: $line"; return; }
    echo "PASS $line"
}

# Rows and the negative control, JOBS at a time.
running=0
for i in "${!NAMES[@]}"; do
    run "${NAMES[$i]}" "${ARGS[$i]}" "${FRAMES[$i]}" "${EXPECT[$i]}" &
    running=$((running + 1))
    if (( running >= JOBS )); then wait -n; running=$((running - 1)); fi
done
run "negative-${NAMES[0]}" "${ARGS[0]}" "${FRAMES[0]}" "${EXPECT[0]}" OPENPETE_SKIP_FAILSAFE_VBL=1 &
wait

status=0
for i in "${!NAMES[@]}"; do
    verdict="$(classify "${NAMES[$i]}" "${EXPECT[$i]}")"
    echo "${verdict%% *} ${NAMES[$i]}: ${verdict#* }"
    [[ "$verdict" == PASS* ]] || status=1
done
neg="$(classify "negative-${NAMES[0]}" "${EXPECT[0]}")"
if [[ "$neg" == PASS* ]]; then
    echo "FAIL negative control (negative-${NAMES[0]}) passed; the classifier cannot see a skip that never engaged: ${neg#* }"
    status=1
else
    echo "ok   negative control (negative-${NAMES[0]}) fails as required: ${neg#* }"
fi
echo "check_skip_targets: $GAME ${#NAMES[@]} rows: $( ((status)) && echo FAIL || echo PASS)"
exit $status
