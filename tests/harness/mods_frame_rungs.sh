# shellcheck shell=bash
# mods_frame_rungs.sh - the frames section of check_mods.sh: rungs on the
# override frame stack while the game fiber is parked inside an override.
# SOURCED by tests/harness/check_mods.sh (helpers say/ok/bad, run_game, $WORK,
# $MODS_SRC); Spyro 1 only.
#
# frame-park parks the fiber inside its GamestateUpdate override for a span
# of VSync(0) calls, one tick boundary each; frame-inner's link sits behind
# it and frame-peer's links and hooks ahead of it. Each fixture copy lives
# under $WORK, since a rung edits config.toml.

# fr_mods <dir> <mod[=newid]>... - copy fixtures into <dir> (a copy renamed
# to newid where given) and list them in enabled.txt in the given order.
fr_mods() {
    local dir="$1"; shift
    rm -rf "${dir:?}"
    mkdir -p "$dir"
    : > "$dir/enabled.txt"
    local m src id
    for m in "$@"; do
        src="${m%%=*}"; id="${m#*=}"
        cp -r "$MODS_SRC/$src" "$dir/$id"
        rm -rf "${dir:?}/$id/.build" "${dir:?}/$id/data"
        [ "$id" = "$src" ] || sed -i "s/^id *= *\"$src\"/id      = \"$id\"/" "$dir/$id/mod.toml"
        echo "$id" >> "$dir/enabled.txt"
    done
}

# fr_cfg <dir> <id> <toml line>... - write the mod's config.toml.
fr_cfg() {
    local dir="$1" id="$2"; shift 2
    printf '%s\n' "$@" > "$dir/$id/config.toml"
}

# fr_count <log> <regex with one %u capture as \([0-9]*\)> - last match's number.
fr_num() { grep -o "$2" "$1" | tail -1 | grep -o '[0-9]*$'; }

# fr_keep <log> - copy a leg's log to $FR_KEEP when set (it outlives $WORK).
fr_keep() { [ -z "${FR_KEEP:-}" ] || cp "$1" "$FR_KEEP/"; }

# fr_num <log> <pattern ending in a number> - the number of its last match.
fr_num() { grep -o "$2" "$1" | tail -1 | grep -o '[0-9]*$'; }

# fr_before <log> <first regex> <second regex> - true when a line matching
# the first precedes a line matching the second.
fr_before() {
    awk -v a="$2" -v b="$3" '$0 ~ a && !s {s=1} s && $0 ~ b {f=1} END {exit !f}' "$1"
}

# fr_counts <label> <log> - frame-inner ran once per frame-park base(), and
# frame-park was never reentered. The exit shutdown lines carry the counts.
fr_counts() {
    local label="$1" log="$2" bases inner re
    bases="$(fr_num "$log" 'frame-park: shutdown calls=[0-9]* bases=[0-9]*')"
    inner="$(fr_num "$log" 'frame-inner: shutdown calls=[0-9]*')"
    re="$(fr_num "$log" 'reentered=[0-9]*')"
    if [ -n "$bases" ] && [ "$bases" -gt 0 ] && [ "$bases" = "$inner" ] && [ "$re" = 0 ]; then
        ok "$label: frame-inner ran once per frame-park base() ($inner of $bases), no reentry"
    else
        bad "$label: frame-park base() calls ${bases:-none}, frame-inner calls ${inner:-none}, reentries ${re:-none} (see $log)"
    fi
}

# The burst: frame-park's call 300 makes 200 base() calls, the tick
# boundaries of canonical ticks 300 to 499 (measured headless), each with
# frame-park's frame and frame-inner's below it on the stack.
FR_BURST='park_at = 300'
FR_BASES='park_bases = 200'

# ── re-anchor: a reload of a link the parked frame passed ──────────────
# frame-peer is listed last, so its links sit ahead of frame-park's; it is
# toggled off at tick 100, so frame-park's frame passed them without
# running, and reloaded at tick 400 inside the burst. The reload removes
# the links ahead of the parked frames and inserts them again; every
# frame-park base() after it must still reach frame-inner.
fr_reanchor() { # fr_reanchor <links>
    local n="$1" d="$WORK/fr-anchor$1" log="$WORK/fr-anchor$1.log"
    say "frames: re-anchor - reload of $n passed link(s) ahead of a parked frame"
    fr_mods "$d" frame-inner frame-park frame-peer
    fr_cfg "$d" frame-park "$FR_BURST" "$FR_BASES"
    fr_cfg "$d" frame-peer "links = $n"
    OPENPETE_MODS_TOGGLE='100:frame-peer:0' OPENPETE_MODS_CFG_APPLY='400:frame-peer:gen=1' \
        run_game "$d" 1200 > "$log" 2>&1
fr_keep "$log"
    if fr_before "$log" 'frame-park: burst begins' 'hot reload: frame-peer live' &&
       fr_before "$log" 'hot reload: frame-peer live' 'frame-park: burst ends' &&
       grep -q 'frame-peer: entry links=[0-9]* gen=1' "$log"; then
        ok "re-anchor ($n): frame-peer reloaded inside the burst"
    else
        bad "re-anchor ($n): no reload of frame-peer inside the burst (see $log)"
    fi
    fr_counts "re-anchor ($n)" "$log"
}
fr_reanchor 1
fr_reanchor 2

# ── re-anchor: a link entered and reloaded while the frame is parked ────
# Deferred entry exists only in a windowed run (a headless run keeps the
# exact enabled set), so this leg runs on a private Xvfb display
# (scripts/lib/xvfb.sh). frame-peer is in the mods directory but not in
# enabled.txt, so it loads with its entry deferred; it is enabled at tick
# 380, inside the burst (its link is inserted ahead of the parked frames),
# and reloaded at tick 420, before the burst ends (that link removed and
# inserted again).
# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh
fr_window() { # fr_window <mods dir> <log> <frame limit> [VAR=value...]
    local md="$1" log="$2" limit="$3" root rc
    shift 3
    rm -f "${log:?}"
    op_xvfb_set 1024x768 || { echo "SKIP: $OP_XVFB_WHY" > "$log"; return 64; }
    root="$(op_sandbox_run_root)" || return 1
    op_sandbox_xdg "$root" || return 1
    env -u OPENPETE_MODS OPENPETE_VIBRATION=0 "OPENPETE_SANDBOX=$root" \
        "OPENPETE_CARD_DIR=$root/cards" "${OP_SANDBOX_XDG_ENV[@]}" \
        OPENPETE_MODS_DIR="$md" "$@" \
        timeout -k 10 600 "${OP_XVFB[@]}" \
        "$PG_BIN" --game "$PG_GAME" --no-config --disc "$PG_DISC" --disc-cache 0 \
            --no-card --mute 1 --input-lock --vibration 0 --renderer vk \
            --aspect 4:3 --lod original --frame-limit "$limit" > "$log" 2>&1
    rc=$?
    rm -rf "${root:?}"
    return $rc
}
say "frames: re-anchor - a link entered and reloaded while the frame is parked (windowed)"
d="$WORK/fr-late"; log="$WORK/fr-late.log"
fr_mods "$d" frame-inner frame-park frame-peer
sed -i '/^frame-peer$/d' "$d/enabled.txt"
fr_cfg "$d" frame-park "$FR_BURST" "$FR_BASES"
fr_window "$d" "$log" 1200 OPENPETE_MODS_TOGGLE='380:frame-peer:1' \
    OPENPETE_MODS_CFG_APPLY='420:frame-peer:gen=1'
rc=$?
fr_keep "$log"
if [ "$rc" -eq 64 ]; then
    bad "late entry: $(cat "$log")"
elif grep -q 'entry deferred' "$log" &&
     fr_before "$log" 'frame-park: burst begins' 'live toggle: frame-peer ENABLED' &&
     fr_before "$log" 'live toggle: frame-peer ENABLED' 'hot reload: frame-peer live' &&
     fr_before "$log" 'hot reload: frame-peer live' 'frame-park: burst ends'; then
    ok "late entry: frame-peer entered and reloaded inside the burst"
    fr_counts "late entry" "$log"
else
    bad "late entry: no deferred entry and reload inside the burst (rc=$rc, see $log)"
fi

# ── base() and crash attribution from a toggle hook over a parked frame ─
# frame-park's frame is live at every boundary of play, so a toggle hook
# runs over it. frame-peer's toggle hook calls base(): refused, so the
# chain under the parked frames runs no extra time.
say "frames: base() from a toggle hook over a parked frame is refused"
d="$WORK/fr-tbase"; log="$WORK/fr-tbase.log"
fr_mods "$d" frame-inner frame-park frame-peer
fr_cfg "$d" frame-peer 'links = 0' 'toggle_base = true'
RUN_TIMEOUT=120 OPENPETE_MODS_TOGGLE='400:frame-peer:0' run_game "$d" 800 > "$log" 2>&1
fr_keep "$log"
if grep -q 'frame-peer: toggle hook on=0 base returned ran_draw=0' "$log" &&
   grep -q 'base() refused' "$log"; then
    ok "toggle-hook base(): refused, GamestateDraw did not run inside it"
else
    bad "toggle-hook base(): $(grep -o 'base returned ran_draw=[01]' "$log" || echo 'hook never ran'), refusal lines $(grep -c 'base() refused' "$log") (see $log)"
fi
fr_counts "toggle-hook base()" "$log"

say "frames: crash attribution from a toggle hook over a parked frame"
d="$WORK/fr-tcrash"; log="$WORK/fr-tcrash.log"
fr_mods "$d" frame-inner frame-park frame-peer
fr_cfg "$d" frame-peer 'links = 0' 'toggle_crash = true'
RUN_TIMEOUT=120 OPENPETE_MODS_TOGGLE='400:frame-peer:0' run_game "$d" 800 > "$log" 2>&1
rc=$?
fr_keep "$log"
if [ "$rc" -ne 0 ] && grep -q 'frame-peer: toggle hook faulting' "$log" &&
   grep -q 'openpete crash' "$log"; then
    if grep -q "MOD ATTRIBUTION" "$log"; then
        bad "toggle-hook fault attributed to a parked frame: $(grep -m1 'MOD ATTRIBUTION' "$log")"
    else
        ok "toggle-hook fault: crash report names no parked override"
    fi
else
    bad "toggle-hook fault did not run (rc=$rc, see $log)"
fi

# ── reload deferral while the owner's frame is live ─────────────────────
# frame-park's frame is live at every boundary until it is toggled off at
# tick 700. A reload of frame-park requested at tick 400 waits: its old
# object's shutdown hook runs with no call of the override on the stack,
# after the toggle, and the reload then applies.
fr_defer() { # fr_defer <label> <mods dir> <log> -- verdicts on one run
    local label="$1" log="$3"
    if grep -q 'hot reload of frame-park deferred' "$log"; then
        ok "$label: the reload was deferred while the frame was live"
    else
        bad "$label: no deferral logged (see $log)"
    fi
    if grep -q 'frame-park: shutdown calls=[0-9]* bases=[0-9]* reentered=[0-9]* live=1' "$log" &&
       ! fr_before "$log" 'hot reload: frame-park live' 'frame-park: shutdown .*live=1'; then
        bad "$label: the old object's shutdown hook ran with its override on the stack"
    elif fr_before "$log" 'live toggle: frame-park DISABLED' 'frame-park: shutdown .*live=0' &&
         fr_before "$log" 'live toggle: frame-park DISABLED' 'hot reload: frame-park live'; then
        ok "$label: the reload applied after the frame was gone"
    else
        bad "$label: the reload did not apply after the toggle (see $log)"
    fi
}

say "frames: deferral - a hot-reload poll edit while the owner's frame is live"
d="$WORK/fr-dpoll"; log="$WORK/fr-dpoll.log"
fr_mods "$d" frame-inner frame-park
fr_cfg "$d" frame-park 'touch_config = 400'
OPENPETE_MOD_HOT_RELOAD=1 OPENPETE_MODS_TOGGLE='700:frame-park:0' \
    run_game "$d" 1800 > "$log" 2>&1
fr_keep "$log"
grep -q 'frame-park: config.toml edited' "$log" || bad "poll deferral: the config edit never ran (see $log)"
fr_defer "poll deferral" "$d" "$log"

say "frames: deferral - a config apply while the owner's frame is live"
d="$WORK/fr-dcfg"; log="$WORK/fr-dcfg.log"
fr_mods "$d" frame-inner frame-park
OPENPETE_MODS_CFG_APPLY='400:frame-park:gen=1' OPENPETE_MODS_TOGGLE='700:frame-park:0' \
    run_game "$d" 1800 > "$log" 2>&1
fr_keep "$log"
fr_defer "config deferral" "$d" "$log"

# ── reload deferral during a parked level-hook walk ─────────────────────
# frame-peer's level-ready hook calls GamestateDraw 300 times on the
# attract demo's first level, one boundary each, inside the registry's
# level-hook walk. A reload of frame-peer requested inside that span
# applies after the walk ends.
say "frames: deferral - a reload during a parked level-hook walk"
d="$WORK/fr-dlevel"; log="$WORK/fr-dlevel.log"
fr_mods "$d" frame-peer
fr_cfg "$d" frame-peer 'links = 0' 'level_park = 300'
OPENPETE_MODS_CFG_APPLY="${FR_LEVEL_TICK:-2050}:frame-peer:gen=1" run_game "$d" 5500 > "$log" 2>&1
fr_keep "$log"
if fr_before "$log" 'frame-peer: level park begins' 'config-apply schedule: frame-peer' &&
   fr_before "$log" 'config-apply schedule: frame-peer' 'frame-peer: level park ends'; then
    ok "level deferral: the reload was requested inside the parked walk"
else
    bad "level deferral: the request missed the parked walk (see $log)"
fi
if grep -q 'frame-peer: shutdown level_parking=1' "$log"; then
    bad "level deferral: the shutdown hook ran inside the parked level hook"
elif fr_before "$log" 'frame-peer: level park ends' 'hot reload: frame-peer live'; then
    ok "level deferral: the reload applied after the walk ended"
else
    bad "level deferral: no reload after the walk (see $log)"
fi
