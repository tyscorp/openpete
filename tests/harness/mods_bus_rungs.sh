# shellcheck shell=bash
# mods_bus_rungs.sh - the bus section of check_mods.sh: named buses between
# mods (bus_attach, bus_call, bus_call_to, bus_members, mod_key, mod_info).
# SOURCED by tests/harness/check_mods.sh (helpers say/ok/bad, run_game,
# $WORK, $MODS_SRC); Spyro 1 only.
#
# mods/bus-member is copied under several ids (each copy its own mod key);
# its config selects the sender, the probes and the handler results, and
# each probe logs one "bus-member <id>: ..." line. mods/bus-ident edits its
# own manifest identity before a hot reload. Each copy lives under $WORK.

# bus_mods <dir> <src[=id]>... - copy fixtures into <dir>, each renamed to id
# where given, and list them in enabled.txt in the given order.
bus_mods() {
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

# bus_cfg <dir> <id> <toml line>... - write the copy's config.toml.
bus_cfg() {
    local dir="$1" id="$2"; shift 2
    printf '%s\n' "$@" > "$dir/$id/config.toml"
}

# bus_keep <log> - copy a leg's log to $BUS_KEEP when set (it outlives $WORK).
bus_keep() { [ -z "${BUS_KEEP:-}" ] || cp "$1" "$BUS_KEEP/"; }

# bus_r <log> <id> <name> - the value text of a copy's "R <name>=" line.
bus_r() { grep -o "bus-member $2: R $3[= ].*" "$1" | tail -1 | sed "s/^bus-member $2: R $3[= ]//"; }

# bus_expect <label> <log> <id> <name> <regex> - PASS when the R line matches.
bus_expect() {
    local v; v="$(bus_r "$2" "$3" "$4")"
    if [ -n "$v" ] && printf '%s\n' "$v" | grep -Eq "^($5)\$"; then
        ok "$1: $4=$v"
    else
        bad "$1: $4=${v:-<no line>} (want /$5/; see $2)"
    fi
}

# bus_links <log> - the chain order of the GamestateDraw call that made the
# walk: the link lines since the previous call's innermost link (m1, the
# first entered, is innermost), up to the walk line.
bus_links() {
    awk '/bus-member [^:]*: link GamestateDraw/ {
             id = $0; sub(/.*bus-member /, "", id); sub(/:.*/, "", id)
             if (last == "m1") cur = ""
             cur = cur (cur == "" ? "" : ",") id; last = id }
         /bus-member m1: walk rc=/ { print cur; exit }' "$1"
}
# bus_walk <log> <id> - the ids the sender's order/1 walk appended, in order.
bus_walk() { grep -o "bus-member $2: walk rc=[-0-9]* n=[0-9]* mix=[0-9A-F]* ids=[^ ]*" "$1" | tail -1 | sed 's/.*ids=//; s/,$//'; }
# bus_mix <log> <id> - the walk's order hash.
bus_mix() { grep -o "bus-member $2: walk rc=[-0-9]* n=[0-9]* mix=[0-9A-F]*" "$1" | tail -1 | sed 's/.*mix=//'; }

BUS_AT=300 # the GamestateDraw call of the probes (inside the attract demo)

# bus_order <label> <log> <want ids> - the walk and the chain order both equal want.
bus_order() {
    local walk links; walk="$(bus_walk "$2" m1)"; links="$(bus_links "$2")"
    if [ "$walk" = "$3" ] && [ "$links" = "$3" ]; then
        ok "$1: walk order = chain order = entry sequence ($3)"
    else
        bad "$1: walk '$walk', chain '$links', want '$3' (see $2)"
    fi
}

# bus_on <group> - true when BUS_LEGS is unset or names the group (core,
# park, rewind, mutation, late; space-separated), so a run can select legs.
bus_on() { [ -z "${BUS_LEGS:-}" ] || [[ " $BUS_LEGS " == *" $1 "* ]]; }

# bus-kc-462789 and bus-kc-679192 share the FNV-1a key 0xE00B690B;
# bus-kz-6snkaaq1 hashes to 0.
KC1=bus-kc-462789; KC2=bus-kc-679192; KZ=bus-kz-6snkaaq1
KLONG="bus-klong-$(printf 'a%.0s' $(seq 54))" # 64 bytes

# shellcheck source=scripts/lib/xvfb.sh
source scripts/lib/xvfb.sh
bus_window() { # bus_window <mods dir> <log> <frame limit> [VAR=value...]
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

if bus_on core; then
# ── walk order: boot, hot reload ─────────────────────────────────────────
say "bus: walk order at boot equals the entry sequence and the override chain order"
d="$WORK/bus-order"; log="$WORK/bus-order.log"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3
bus_cfg "$d" m1 'sender = true' "send_at = $BUS_AT" 'log_links = true'
bus_cfg "$d" m2 'log_links = true'
bus_cfg "$d" m3 'log_links = true'
run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
bus_order "boot" "$log" "m3,m2,m1"

say "bus: walk order after a hot reload (the reloaded mod moves to the front)"
d="$WORK/bus-reload"; log="$WORK/bus-reload.log"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3
bus_cfg "$d" m1 'sender = true' "send_at = $BUS_AT" 'log_links = true'
bus_cfg "$d" m3 'log_links = true'
# m2's GamestateDraw link encloses the fiber's park, so a reload of m2 waits
# while it runs: m2 is toggled off around the reload. The Apply writes the
# one key it names, so it carries m2's log_links.
OPENPETE_MODS_TOGGLE='90:m2:0,150:m2:1' OPENPETE_MODS_CFG_APPLY="100:m2:log_links=1" \
    run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
if grep -q 'hot reload: m2 live' "$log" && [ "$(grep -c 'bus-member m2: entry key=' "$log")" = 2 ]; then
    bus_order "reload" "$log" "m2,m3,m1"
else
    bad "reload: m2 was not reloaded (see $log)"
fi

# ── order engagement: the walk order decides the arena bytes ─────────────
say "bus: order engagement - the walk's order hash follows the enabled-list order"
d="$WORK/bus-mixa"; log_a="$WORK/bus-mixa.log"; log_a2="$WORK/bus-mixa2.log"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3
bus_cfg "$d" m1 'sender = true' "send_at = $BUS_AT"
run_game "$d" 800 > "$log_a" 2>&1
run_game "$d" 800 > "$log_a2" 2>&1
d="$WORK/bus-mixb"; log_b="$WORK/bus-mixb.log"
bus_mods "$d" bus-member=m1 bus-member=m3 bus-member=m2
bus_cfg "$d" m1 'sender = true' "send_at = $BUS_AT"
run_game "$d" 800 > "$log_b" 2>&1
bus_keep "$log_a"; bus_keep "$log_b"
ma="$(bus_mix "$log_a" m1)"; ma2="$(bus_mix "$log_a2" m1)"; mb="$(bus_mix "$log_b" m1)"
if [ -n "$ma" ] && [ "$ma" = "$ma2" ] && [ -n "$mb" ] && [ "$ma" != "$mb" ]; then
    ok "order engagement: unflipped runs match ($ma), the flipped order differs ($mb)"
else
    bad "order engagement: runs '${ma:-}' '${ma2:-}', flipped '${mb:-}'"
fi

# ── results, depth, registers, base() ────────────────────────────────────
# m1 runs the battery; m2 stops stop/1 with 5 and returns -7 on order/1; m3
# is toggled off at tick 100 (the disabled key); m4 is loaded and attached
# to order/1 but never to nobody/1.
say "bus: results, depth, reentry, the saved register set and base() in a handler"
d="$WORK/bus-res"; log="$WORK/bus-res.log"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3 bus-member=m4
bus_cfg "$d" m1 "selftest_at = $BUS_AT" 'deep = true' 'attach_late = true' \
    'probe_disabled = "m3"' 'probe_unattached = "m4"'
bus_cfg "$d" m2 'stop = 5' 'neg = true'
OPENPETE_MODS_TOGGLE='100:m3:0' run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
L="results"
bus_expect "$L" "$log" m1 pass '0'
bus_expect "$L" "$log" m1 stop '5'
bus_expect "$L" "$log" m1 none_unattached '-3'
bus_expect "$L" "$log" m1 none_misspelt '-3'
bus_expect "$L" "$log" m1 malformed '-1'
bus_expect "$L" "$log" m1 range '-1'
bus_expect "$L" "$log" m1 members '3 m4,m2,m1,'
bus_expect "$L" "$log" m1 'call_to m4' '0 appended=1 key_ok=1'
bus_expect "$L" "$log" m1 call_to_self '0'
bus_expect "$L" "$log" m1 call_to_unloaded '-3'
bus_expect "$L" "$log" m1 call_to_zero '-3'
bus_expect "$L" "$log" m1 call_to_disabled '-3'
bus_expect "$L" "$log" m1 call_to_never_attached '-3'
bus_expect "$L" "$log" m1 deep 'levels=8 failed_with=-4 outer=0'
bus_expect "$L" "$log" m1 reent '0 levels=2'
bus_expect "$L" "$log" m1 regs '0 unchanged=1 handler_writes=[1-9][0-9]* hcall_base_ran=1 vec=11,22,33'
k="$(grep -o 'bus-member m1: R mod_key=[0-9A-F]* sdk=[0-9A-F]*' "$log" | tail -1)"
if [ -n "$k" ] && [ "$(echo "$k" | sed 's/.*mod_key=\([0-9A-F]*\) .*/\1/')" = "${k##*sdk=}" ]; then
    ok "$L: mod_key equals openpete_mod_key_of of the id (${k##*sdk=})"
else
    bad "$L: mod_key line '${k:-none}'"
fi
bus_expect "$L" "$log" m1 mod_info_self '0 id=m1'
if grep -q 'bus-member m2: handler stop/1 ran' "$log" && ! grep -q 'bus-member m1: handler stop/1 ran' "$log"; then
    ok "$L: stop/1 ended the walk at m2 (m1, behind it, did not run)"
else
    bad "$L: stop/1 walk ran past its stopper or never reached it"
fi
if [ "$(grep -c 'bus "order/1": handler returned -7' "$log")" = 1 ]; then
    ok "$L: m2's negative result logged once and read as OP_BUS_PASS"
else
    bad "$L: negative result log lines $(grep -c 'handler returned -7' "$log") (want 1)"
fi
if grep -q 'base() refused in a bus handler (bus "regs/1", mod m1)' "$log"; then
    ok "$L: base() inside a bus handler refused"
else
    bad "$L: no base() refusal from the regs/1 handler"
fi
if grep -q 'bus-member m1: late attach=-1' "$log" && grep -q 'bus_attach refused: bus_attach is legal in the entry point only' "$log"; then
    ok "$L: bus_attach outside the entry point refused"
else
    bad "$L: late bus_attach not refused"
fi
if grep -q 'bus-member m1: entry key=[0-9A-F]* gen=0 entry_bus_call=-1' "$log" &&
   grep -q 'bus_call refused: bus calls are refused in the entry point' "$log"; then
    ok "$L: bus_call from the entry point refused"
else
    bad "$L: entry-point bus_call not refused"
fi

# m1 alone: its GamestateDraw frame, then one VecCopy frame per level, so the
# bus frame is the one that finds the stack full.
say "bus: a send with the frame stack full of override frames returns OP_BUS_DEPTH"
d="$WORK/bus-fill"; log="$WORK/bus-fill.log"
bus_mods "$d" bus-member=m1
bus_cfg "$d" m1 "selftest_at = $BUS_AT" 'fill = true'
run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
bus_expect "fill" "$log" m1 fill 'depth=31 passes=30 result=-4'

say "bus: a broadcast over more members than the depth cap reaches every member"
d="$WORK/bus-wide"; log="$WORK/bus-wide.log"
bus_wide=()
for i in 1 2 3 4 5 6 7 8 9 10 11; do bus_wide+=("bus-member=m$i"); done
bus_mods "$d" "${bus_wide[@]}"
bus_cfg "$d" m1 'sender = true' "send_at = $BUS_AT"
run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
n="$(grep -o 'bus-member m1: walk rc=0 n=[0-9]*' "$log" | tail -1 | sed 's/.*n=//')"
if [ "${n:-0}" = 11 ]; then ok "wide: 11 members walked, rc=0"; else bad "wide: walk n=${n:-none} (want 11)"; fi

# ── refusals and the toggle drain ────────────────────────────────────────
say "bus: refusals from a present hook, a mod thread, the shutdown hook and an override it reaches"
d="$WORK/bus-ref"; log="$WORK/bus-ref.log"
bus_mods "$d" bus-member=m1 bus-member=m2
bus_cfg "$d" m1 'refusals = true'
run_game "$d" 600 > "$log" 2>&1
bus_keep "$log"
for pat in 'thread bus_call=-1 members=2 mod_info=0' \
           'shutdown bus_call=-1' 'shutdown-override bus_call=-1'; do
    if grep -q "bus-member m1: $pat" "$log"; then ok "refusal: $pat"
    else bad "refusal: no '$pat' line (see $log)"; fi
done

say "bus: a toggle-off hook's send during a multi-mod drain reaches the membership without the toggled mod"
d="$WORK/bus-tog"; log="$WORK/bus-tog.log"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3
bus_cfg "$d" m1 'toggle_send = true'
bus_cfg "$d" m2 'toggle_send = true'
OPENPETE_MODS_TOGGLE='200:m1:0,200:m2:0' run_game "$d" 500 > "$log" 2>&1
bus_keep "$log"
if grep -q 'bus-member m1: toggle on=0 send=0 appended=2 members=2 self_listed=0 list=m3,m2,' "$log" &&
   grep -q 'bus-member m2: toggle on=0 send=0 appended=1 members=1 self_listed=0 list=m3,' "$log"; then
    ok "toggle drain: each toggle-off hook's walk and members exclude the toggled mod and the ones before it"
else
    bad "toggle drain: $(grep -o 'bus-member m[12]: toggle on=0.*' "$log" | paste -sd'|' -)"
fi

# ── crash attribution ────────────────────────────────────────────────────
say "bus: a fault inside a bus handler names the bus and the handler's owner"
d="$WORK/bus-crash"; log="$WORK/bus-crash.log"
bus_mods "$d" bus-member=m1 bus-member=m2
bus_cfg "$d" m1 "crash_at = $BUS_AT"
bus_cfg "$d" m2 'crash_handler = true'
RUN_TIMEOUT=120 run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
if grep -q "MOD ATTRIBUTION: inside bus handler of bus 'crash/1' attached by mod 'm2'" "$log"; then
    ok "crash: attributed to bus crash/1, handler of m2"
else
    bad "crash: $(grep -m1 'MOD ATTRIBUTION' "$log" || echo 'no attribution line') (see $log)"
fi

# ── keys ─────────────────────────────────────────────────────────────────
say "bus: keyless mods pinned at api 13 are refused at load"
d="$WORK/bus-keys"; log="$WORK/bus-keys.log"
bus_mods "$d" bus-member=m1 "bus-member=$KC1" "bus-member=$KC2" "bus-member=$KZ" bus-member=klong
# A directory name is cut at 63 bytes too, so the 64-byte id is the
# manifest's alone.
sed -i "s/^id *= *\"klong\"/id      = \"$KLONG\"/" "$d/klong/mod.toml"
run_game "$d" 300 > "$log" 2>&1
bus_keep "$log"
if grep -q "\"$KC1\" and \"$KC2\" are keyless: their ids share the mod key 0xE00B690B" "$log" &&
   grep -q "\"$KC1\" refused: a keyless mod cannot load" "$log" &&
   grep -q "\"$KC2\" refused: a keyless mod cannot load" "$log"; then
    ok "keys: a colliding pair, both named, both refused"
else
    bad "keys: the colliding pair was not named and refused (see $log)"
fi
if grep -q "\"$KZ\" is keyless: its key is 0" "$log" && grep -q "\"$KZ\" refused" "$log"; then
    ok "keys: an id hashing to 0 refused"
else
    bad "keys: the zero-key id was not refused"
fi
if grep -q "is keyless: its id is 64 bytes or longer" "$log" && grep -q "\"${KLONG:0:63}\" refused" "$log"; then
    ok "keys: a 64-byte id refused"
else
    bad "keys: the 64-byte id was not refused"
fi
if grep -q 'bus-member m1: attach rc=0' "$log"; then ok "keys: a keyed mod beside them loads"; else bad "keys: m1 did not load"; fi

say "bus: a keyless mod pinned below 13 loads with its bus calls and mod_key refused"
d="$WORK/bus-k12"; log="$WORK/bus-k12.log"
bus_mods "$d" bus-member=m1 "bus-member=$KZ"
sed -i 's/^api *= *13/api     = 12/' "$d/$KZ/mod.toml"
bus_cfg "$d" "$KZ" "selftest_at = $BUS_AT"
run_game "$d" 600 > "$log" 2>&1
bus_keep "$log"
if grep -q "bus-member $KZ: entry key=00000000 " "$log" &&
   grep -q "bus-member $KZ: attach rc=-1" "$log" &&
   grep -q "bus-member $KZ: R pass=-1" "$log" &&
   grep -q "bus-member $KZ: link\|bus-member $KZ: shutdown draws=[1-9]" "$log"; then
    ok "keys: api-12 keyless mod loaded, its overrides ran, bus calls refused, mod_key 0"
else
    bad "keys: api-12 keyless mod (see $log)"
fi

say "bus: a sim_mutative = false mod is refused bus_attach, bus_call and bus_call_to"
d="$WORK/bus-neutral"; log="$WORK/bus-neutral.log"
bus_mods "$d" bus-member=m1 bus-member=m2
sed -i 's/^api *= *13/api     = 13\nsim_mutative = false/' "$d/m2/mod.toml"
bus_cfg "$d" m2 "selftest_at = $BUS_AT"
run_game "$d" 600 > "$log" 2>&1
bus_keep "$log"
if grep -q 'bus-member m2: attach rc=-1' "$log" && grep -q 'bus-member m2: R pass=-1' "$log" &&
   grep -q 'bus-member m2: R call_to_self=-1' "$log" &&
   grep -q 'the manifest declares sim_mutative = false' "$log"; then
    ok "neutral: attach, bus_call and bus_call_to refused"
else
    bad "neutral: sim_mutative = false mod was not refused (see $log)"
fi

# ── reload identity ──────────────────────────────────────────────────────
bus_ident() { # bus_ident <1 id | 2 api> <label>
    local d="$WORK/bus-ident$1" log="$WORK/bus-ident$1.log"
    say "bus: a hot reload that changes the manifest $2 is refused, the running object kept"
    bus_mods "$d" bus-ident
    bus_cfg "$d" bus-ident "touch_ident = $1" "touch_at = $BUS_AT"
    # Toggled off after the edit so the poll's reload is not deferred by
    # its parked GamestateDraw frame, and on again to show the kept object
    # still running (its calls pass the 320 made before the toggle).
    OPENPETE_MOD_HOT_RELOAD=1 OPENPETE_MODS_TOGGLE="320:bus-ident:0,480:bus-ident:1" \
        run_game "$d" 1200 > "$log" 2>&1
    bus_keep "$log"
    local entries calls
    entries="$(grep -c 'bus-ident: entry gen=' "$log")"
    calls="$(grep -o 'bus-ident: shutdown calls=[0-9]* gen=0' "$log" | tail -1 | sed 's/.*calls=\([0-9]*\).*/\1/')"
    if ! grep -q "bus-ident: edit $1 call=$BUS_AT rc=0,0" "$log"; then
        bad "ident ($2): the manifest edit did not run (see $log)"
    elif grep -q "reload refused - the manifest $2" "$log" && [ "$entries" = 1 ] &&
         [ "$(grep -c 'bus-ident: shutdown calls=' "$log")" = 1 ] && [ "${calls:-0}" -gt 330 ]; then
        ok "ident ($2): reload refused; one entry, one shutdown (at exit), the override kept running ($calls calls)"
    else
        bad "ident ($2): refusal $(grep -c 'reload refused' "$log"), entries $entries, shutdowns $(grep -c 'bus-ident: shutdown calls=' "$log"), calls ${calls:-none}"
    fi
}
bus_ident 1 "id"
bus_ident 2 "api pin"

# ── runahead: bus traffic on speculative ticks ───────────────────────────
say "bus: runahead with bus traffic every tick - the prediction oracle is exact"
d="$WORK/bus-ra"; log="$WORK/bus-ra.log"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3
bus_cfg "$d" m1 'every_tick = true'
OPENPETE_RA_PREDICT_CHECK=1 RUN_TIMEOUT=900 run_game "$d" 3000 --runahead 1 --skip-to-level 10 > "$log" 2>&1
bus_keep "$log"
ra_sum="$(grep -oE '\[ra-predict\] checked=[0-9]+ mismatch=[0-9]+' "$log" | tail -1)"
spec="$(grep -o 'bus: buses=[0-9]* handler runs=[0-9]* speculative=[0-9]*' "$log" | tail -1 | sed 's/.*speculative=//')"
if ! grep -q "runahead: armed" "$log"; then
    bad "runahead: never armed (see $log)"
elif [ -z "$ra_sum" ] || [ "${ra_sum#*checked=}" = "${ra_sum#*checked=0 }" ]; then
    bad "runahead: VACUOUS - ${ra_sum:-no [ra-predict] summary}"
elif [ "${ra_sum##*mismatch=}" != 0 ]; then
    bad "runahead: prediction mismatch ($ra_sum)"
elif [ "${spec:-0}" -le 0 ]; then
    bad "runahead: no handler ran on a speculative tick (speculative=${spec:-none})"
else
    ok "runahead: $ra_sum, $spec handler runs on speculative ticks"
fi

# ── savestate order with bus-only members ────────────────────────────────
say "bus: a savestate loads under the same bus-only members and is refused in the other order"
BUS_CARDS="$WORK/bus-cards"; mkdir -p "$BUS_CARDS"
d="$WORK/bus-ss"; log="$WORK/bus-ss-save.log"
bus_mods "$d" bus-member=m1 bus-member=b1 bus-member=b2
bus_cfg "$d" m1 'every_tick = true'
bus_cfg "$d" b1 'no_override = true'
bus_cfg "$d" b2 'no_override = true'
rm -f "$WORK/bus.state"
run_game "$d" 2600 --skip-to-level 10 --card-dir "$BUS_CARDS" \
    --state-save-on-frame 2000 --state-save-output "$WORK/bus.state" > "$log" 2>&1
bus_keep "$log"
if grep -q "savestate: saved" "$log" && [ -e "$WORK/bus.state" ]; then
    log="$WORK/bus-ss-same.log"
    run_game "$d" 2600 --skip-to-level 10 --card-dir "$BUS_CARDS" \
        --state-load-on-frame 2200 --state-load-input "$WORK/bus.state" > "$log" 2>&1
    bus_keep "$log"
    if grep -q "savestate: loaded" "$log"; then ok "savestate: loads under the same order"
    else bad "savestate: the same order did not load (see $log)"; fi
    printf 'm1\nb2\nb1\n' > "$d/enabled.txt"
    log="$WORK/bus-ss-flip.log"
    run_game "$d" 2600 --skip-to-level 10 --card-dir "$BUS_CARDS" \
        --state-load-on-frame 2200 --state-load-input "$WORK/bus.state" > "$log" 2>&1
    bus_keep "$log"
    if grep -q "MODSET MISMATCH (order)" "$log" && ! grep -q "savestate: loaded" "$log"; then
        ok "savestate: the bus-only members in the other order are refused (order)"
    else
        bad "savestate: the flipped order was not refused by order (see $log)"
    fi
else
    bad "savestate: the save was not written (see $log)"
fi

# ── windowed: present hooks and load-all discovery ───────────────────────
# A headless run neither runs present hooks nor discovers mods outside
# enabled.txt, so this leg runs on a private Xvfb display
# (scripts/lib/xvfb.sh). m1's present hook sends with a kept CPUState
# (refused); one of the colliding pair is a load-all row, which only a
# windowed run discovers.
say "bus: windowed - a present hook's send is refused; a load-all row is keyed with the boot set"
d="$WORK/bus-win"; log="$WORK/bus-win.log"
bus_mods "$d" bus-member=m1 bus-member=m2 "bus-member=$KC1" "bus-member=$KC2"
printf 'm1\nm2\n%s\n' "$KC1" > "$d/enabled.txt"   # KC2: a load-all row
bus_cfg "$d" m1 'refusals = true'
bus_window "$d" "$log" 600
rc=$?
bus_keep "$log"
if [ "$rc" -eq 64 ]; then
    bad "windowed: $(cat "$log")"
else
    if grep -q 'bus-member m1: present bus_call=-1 members=2' "$log" &&
       grep -q 'bus_call refused: bus calls are refused inside a present hook' "$log"; then
        ok "windowed: present-hook bus_call refused, bus_members answered"
    else
        bad "windowed: no present-hook refusal (rc=$rc, see $log)"
    fi
    if grep -q "\"$KC1\" and \"$KC2\" are keyless" "$log" && grep -q "\"$KC2\" refused" "$log" &&
       grep -q "\"$KC1\" refused" "$log"; then
        ok "windowed: a colliding pair with one deferred row, both named and refused"
    else
        bad "windowed: the pair with a deferred member was not refused"
    fi
fi

fi # core

if bus_on late; then
# ── windowed: walk order after a deferred first enable ───────────────────
# m3 is a load-all row (not in enabled.txt): a windowed run discovers it and
# defers its entry until the toggle enables it, which inserts its bus table
# and its links at the front.
say "bus: windowed - walk order after a deferred first enable equals the entry sequence"
d="$WORK/bus-late"; log="$WORK/bus-late.log"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3
printf 'm1\nm2\n' > "$d/enabled.txt"
bus_cfg "$d" m1 'sender = true' "send_at = $BUS_AT" 'log_links = true'
bus_cfg "$d" m2 'log_links = true'
bus_cfg "$d" m3 'log_links = true'
bus_window "$d" "$log" 600 OPENPETE_MODS_TOGGLE='100:m3:1'
rc=$?
bus_keep "$log"
if [ "$rc" -eq 64 ]; then
    bad "late enable: $(cat "$log")"
elif grep -q 'entry deferred' "$log" && grep -q 'live toggle: m3 ENABLED' "$log" &&
     [ "$(grep -c 'bus-member m3: entry key=' "$log")" = 1 ]; then
    bus_order "late enable" "$log" "m3,m2,m1"
else
    bad "late enable: m3 was not entered late (rc=$rc, see $log)"
fi
fi # late

# ── park: a bus handler parks the fiber across a save and a load ─────────
# park: m1's GamestateDraw override sends park/1; its handler calls
# GamestateDraw through api->call (the override again, which calls base()),
# so the original parks in VSync under the stack override, bus frame,
# override. park_control: the override makes the same api->call itself,
# inside the walker's saved set and 64-byte sp reserve, with no bus frame.
# Each run saves at PK_T, in level 10's gameplay (--skip-to-level 10 holds
# the demo until about VBL 2021, and a load applies only at the park site
# of the save); a second run loads that file at PK_L and saves again at PK_E, whose guest RAM, scratchpad, CPU
# registers with GTE, and arena the two kinds must share.
PK_T=2200; PK_L=2400; PK_E=2600; PK_LIM=2700
# bus_state <file> <field> - park_ra, park_sp (hex) of a state header.
bus_state() {
    python3 - "$1" "$2" <<'EOF'
import struct, sys
h = open(sys.argv[1], 'rb').read(124)
ra, sp = struct.unpack_from('<II', h, 104)
print('%08X' % {'park_ra': ra, 'park_sp': sp}[sys.argv[2]])
EOF
}
# bus_state_cmp <a> <b> - "equal", or the sections (cpu, ram, scratch, arena)
# and frame that differ between two state files.
bus_state_cmp() {
    python3 - "$1" "$2" <<'EOF'
import struct, sys
def sections(p):
    d = open(p, 'rb').read()
    f = struct.unpack_from('<12I', d, 40)  # frame .. arena_bytes
    cpu, ram, scr, vram, spu_s, spu_m, cd, extra, tag, arena = f[1], f[2], f[3], f[4], f[6], f[7], f[8], f[9], f[10], f[11]
    o = 124
    out = {'cpu': d[o:o + cpu]}; o += cpu
    out['ram'] = d[o:o + ram]; o += ram
    out['scratch'] = d[o:o + scr]; o += scr
    reg = struct.unpack_from('<I', d, 120)[0]
    end = len(d) - reg - tag
    out['arena'] = d[end - arena:end]  # the arena precedes the modset tag and the registry
    return out, f[0]
a, fa = sections(sys.argv[1]); b, fb = sections(sys.argv[2])
diff = [k for k in a if a[k] != b[k]]
if fa != fb: diff.append('frame %d/%d' % (fa, fb))
print('equal' if not diff else 'differ: ' + ','.join(diff))
EOF
}
# bus_frames <log> <save|load> - the bus frames live at the last such state op.
bus_frames() { grep -o "bus: [0-9]* bus frame(s) live at the state $2" "$1" | tail -1 | sed 's/bus: \([0-9]*\).*/\1/'; }
# bus_pk_run <tag> <cfg line> <log> [args...] - one run of m1 under the config.
bus_pk_run() {
    local tag="$1" cfg="$2" log="$3"; shift 3
    local d="$WORK/bus-pk-$tag"
    bus_mods "$d" bus-member=m1
    [ -n "$cfg" ] && bus_cfg "$d" m1 "$cfg"
    RUN_TIMEOUT=900 run_game "$d" "$PK_LIM" --skip-to-level 10 --card-dir "$BUS_PK_CARDS" "$@" > "$log" 2>&1
    bus_keep "$log"
}
# bus_pk_field <log> <name> - the integer after "<name>=" on the m1 park line.
bus_pk_field() { grep -o "bus-member m1: park walks=.*" "$1" | tail -1 | grep -o " $2=[0-9]*" | sed 's/.*=//'; }

if bus_on park; then
say "bus: park - a handler parked across a save and a load equals the direct control (runahead off and on)"
BUS_PK_CARDS="$WORK/bus-pk-cards"; mkdir -p "$BUS_PK_CARDS"
for k in bus ctl plain; do rm -f "$WORK/pk-$k-T.state" "$WORK/pk-$k-E.state" "$WORK/pk-$k-raE.state"; done
bus_pk_run bus 'park = true' "$WORK/pk-bus-T.log" --state-save-on-frame "$PK_T" --state-save-output "$WORK/pk-bus-T.state"
bus_pk_run ctl 'park_control = true' "$WORK/pk-ctl-T.log" --state-save-on-frame "$PK_T" --state-save-output "$WORK/pk-ctl-T.state"
bus_pk_run plain '' "$WORK/pk-plain-T.log" --state-save-on-frame "$PK_T" --state-save-output "$WORK/pk-plain-T.state"
for k in bus ctl plain; do
    [ -e "$WORK/pk-$k-T.state" ] || bad "park: the $k run wrote no state at $PK_T (see $WORK/pk-$k-T.log)"
done
if [ -e "$WORK/pk-bus-T.state" ] && [ -e "$WORK/pk-ctl-T.state" ] && [ -e "$WORK/pk-plain-T.state" ]; then
    ra_b="$(bus_state "$WORK/pk-bus-T.state" park_ra)"; sp_b="$(bus_state "$WORK/pk-bus-T.state" park_sp)"
    ra_c="$(bus_state "$WORK/pk-ctl-T.state" park_ra)"; sp_c="$(bus_state "$WORK/pk-ctl-T.state" park_sp)"
    ra_p="$(bus_state "$WORK/pk-plain-T.state" park_ra)"; sp_p="$(bus_state "$WORK/pk-plain-T.state" park_sp)"
    spl="$(grep -o 'bus-member m1: park sp_in=[0-9A-F]* sp_call=[0-9A-F]*' "$WORK/pk-bus-T.log" | tail -1)"
    sp_in="$(echo "$spl" | sed 's/.*sp_in=\([0-9A-F]*\).*/\1/')"; sp_call="${spl##*sp_call=}"
    if [ -n "$spl" ] && [ "$ra_b" = "$ra_c" ] && [ "$ra_b" = "$ra_p" ] && [ "$sp_b" = "$sp_c" ] &&
       [ $(( 16#$sp_p - 16#$sp_b )) -eq $(( 16#$sp_in - 16#$sp_call )) ] && [ "$sp_b" != "$sp_p" ]; then
        ok "park: save header park_ra=$ra_b in all three; park_sp bus=control=$sp_b, plain $sp_p, offset = the handler's call (sp $sp_in -> $sp_call)"
    else
        bad "park: header ra $ra_b/$ra_c/$ra_p sp $sp_b/$sp_c/$sp_p, handler sp ${spl:-<no line>}"
    fi
    fb="$(bus_frames "$WORK/pk-bus-T.log" save)"; fc="$(bus_frames "$WORK/pk-ctl-T.log" save)"
    if [ "${fb:-}" = 1 ] && [ "${fc:-}" = 0 ]; then ok "park: bus-frame depth at the save: bus 1, control 0"
    else bad "park: bus frames at the save: bus ${fb:-none}, control ${fc:-none}"; fi
    for ra in off on; do
        rargs=(); sfx=E
        [ "$ra" = on ] && { rargs=(--runahead 1); sfx=raE; }
        for k in bus ctl; do
            cfg='park = true'; [ "$k" = ctl ] && cfg='park_control = true'
            OPENPETE_RA_PREDICT_CHECK=1 bus_pk_run "$k" "$cfg" "$WORK/pk-$k-$sfx.log" "${rargs[@]}" \
                --state-load-on-frame "$PK_L" --state-load-input "$WORK/pk-$k-T.state" \
                --state-save-on-frame "$PK_E" --state-save-output "$WORK/pk-$k-$sfx.state"
        done
        lb="$WORK/pk-bus-$sfx.log"; lc="$WORK/pk-ctl-$sfx.log"
        if ! grep -q 'savestate: loaded' "$lb" || ! grep -q 'savestate: loaded' "$lc" ||
           [ ! -e "$WORK/pk-bus-$sfx.state" ] || [ ! -e "$WORK/pk-ctl-$sfx.state" ]; then
            bad "park (runahead $ra): a load or the second save did not happen (see $lb, $lc)"
            continue
        fi
        cmpv="$(bus_state_cmp "$WORK/pk-bus-$sfx.state" "$WORK/pk-ctl-$sfx.state")"
        lfb="$(bus_frames "$lb" load)"; sfb="$(bus_frames "$lb" save)"
        walks="$(bus_pk_field "$lb" walks)"; dups="$(bus_pk_field "$lb" dups)"; sec="$(bus_pk_field "$lb" second_pass)"
        if [ "$cmpv" = equal ] && [ "${lfb:-}" = 1 ] && [ "${sfb:-}" = 1 ] &&
           [ "${walks:-0}" -gt 0 ] && [ "${dups:-1}" = 0 ] && [ "${sec:-0}" = "${walks:-x}" ]; then
            ok "park (runahead $ra): bus run = control at $PK_E (RAM, scratchpad, CPU with GTE, arena); bus frames at load $lfb, at save $sfb; $walks walks, every second send passed"
        else
            bad "park (runahead $ra): state $cmpv; bus frames load ${lfb:-none} save ${sfb:-none}; walks ${walks:-none} dups ${dups:-none} second ${sec:-none}"
        fi
        if [ "$ra" = on ]; then
            rb="$(grep -o 'bus: rollbacks over a bus frame=[0-9]*' "$lb" | tail -1 | sed 's/.*=//')"
            rc_="$(grep -o 'bus: rollbacks over a bus frame=[0-9]*' "$lc" | tail -1 | sed 's/.*=//')"
            pb="$(grep -oE '\[ra-predict\] checked=[0-9]+ mismatch=[0-9]+' "$lb" | tail -1)"
            if grep -q 'runahead: armed' "$lb" && [ "${rb:-0}" -gt 0 ] && [ "${rc_:-x}" = 0 ] &&
               [ -n "$pb" ] && [ "${pb##*mismatch=}" = 0 ] && [ "${pb#*checked=}" != "${pb#*checked=0 }" ]; then
                ok "park (runahead on): $rb rollbacks applied over a live bus frame (control 0); $pb"
            else
                bad "park (runahead on): rollbacks over a bus frame ${rb:-none} (control ${rc_:-none}); ${pb:-no [ra-predict] line}"
            fi
        fi
    done
fi

say "bus: park - after a toggle-off the bus frame depth is 0 at every tick boundary once the parked walk unwinds"
log="$WORK/pk-tog.log"
OPENPETE_MODS_TOGGLE='1400:m1:0' bus_pk_run tog 'park = true' "$log"
bl="$(grep -o 'bus: rollbacks over a bus frame=[0-9]*; tick boundaries over a bus frame=[0-9]*, the last at tick [0-9]*' "$log" | tail -1)"
nb="$(echo "$bl" | sed 's/.*tick boundaries over a bus frame=\([0-9]*\).*/\1/')"
if grep -q 'live toggle: m1 DISABLED (tick 1400)' "$log" && [ "${nb:-0}" -gt 0 ] && [ "${bl##* }" = 1400 ]; then
    ok "park toggle-off: $nb boundaries over a parked bus frame, the last at the toggle's tick 1400"
else
    bad "park toggle-off: ${bl:-no boundary line} (see $log)"
fi
fi # park

# ── rewind: a seek over ticks with bus traffic ───────────────────────────
# m1 sends tick/1 on every GamestateDraw call; every handler folds the
# sender's arena word into its own arena counter. A held rewind steps back
# 40 ticks and replays them from a keyframe; every hashed tick and each
# copy's counter at exit must equal the run that never rewound.
if bus_on rewind; then
say "bus: rewind - a seek over bus traffic replays the lived ticks"
d="$WORK/bus-rw"
bus_mods "$d" bus-member=m1 bus-member=m2 bus-member=m3
bus_cfg "$d" m1 'every_tick = true'
for k in clean rw; do
    [ "$k" = rw ] && rwargs=(--rewind 30 --rewind-hold 2200:40) || rwargs=()
    RUN_OUTS="$WORK/bus-rw-$k.csv" RUN_TIMEOUT=900 run_game "$d" 2600 --skip-to-level 10 \
        --gameplay-hash-log "$WORK/bus-rw-$k.csv" "${rwargs[@]}" > "$WORK/bus-rw-$k.log" 2>&1
    bus_keep "$WORK/bus-rw-$k.log"
done
rep="$(grep -o 'bus: buses=[0-9]* handler runs=[0-9]* speculative=[0-9]* replaying=[0-9]*' "$WORK/bus-rw-rw.log" | tail -1 | sed 's/.*replaying=//')"
cnt_c="$(grep -o 'bus-member m[123]: tick counter=[0-9A-F]*' "$WORK/bus-rw-clean.log" | sort | paste -sd' ' -)"
cnt_r="$(grep -o 'bus-member m[123]: tick counter=[0-9A-F]*' "$WORK/bus-rw-rw.log" | sort | paste -sd' ' -)"
hv="$(python3 - "$WORK/bus-rw-clean.csv" "$WORK/bus-rw-rw.csv" 2>&1 <<'EOF'
import csv, sys
clean = {r[0]: r[2] for r in csv.reader(open(sys.argv[1])) if r and r[0] != 'frame'}
rows = [r for r in csv.reader(open(sys.argv[2])) if r and r[0] != 'frame']
fr = [int(r[0]) for r in rows]
back = sum(1 for i in range(1, len(fr)) if fr[i] < fr[i - 1])
seen = {}
replayed = 0
for r in rows:
    replayed += r[0] in seen
    seen[r[0]] = 1
mism = sum(1 for r in rows if r[0] in clean and clean[r[0]] != r[2])
print(f"jumps={back} replayed={replayed} mismatches={mism} rows={len(rows)}")
EOF
)"
if ! grep -q 'hold 40/40' "$WORK/bus-rw-rw.log"; then
    bad "rewind: VACUOUS - the hold did not finish (see $WORK/bus-rw-rw.log)"
elif [ "${rep:-0}" -le 0 ]; then
    bad "rewind: no handler ran while replaying (replaying=${rep:-none})"
elif [[ "$hv" != *"mismatches=0 "* ]] || [[ "$hv" == *"replayed=0 "* ]]; then
    bad "rewind: hash log $hv"
elif [ -z "$cnt_c" ] || [ "$cnt_c" != "$cnt_r" ]; then
    bad "rewind: arena counters at exit '$cnt_r', lived run '$cnt_c'"
else
    ok "rewind: $rep handler runs while replaying; $hv against the lived run; arena counters equal ($cnt_r)"
fi
fi # rewind

# ── mutation of a member while another member's handler is parked ────────
# s, entered last, walks first: its park/1 handler parks the fiber in
# VSync at every tick boundary. q walks behind it and registers no override
# link (no_override = true; a config Apply edits its one key in place in
# config.toml), so nothing defers q's reload while s is parked. Each copy logs a handler entry made after its shutdown hook, and
# every handler writes a heap block that hook frees (an AddressSanitizer
# build reports such an entry as a use after free: tests/harness/check_asan.sh).
# s logs each change of the member count that appended to a walk's packet,
# and counts a key appended twice in one walk.
# bus_mut_mods <dir> - q (bus-only) then s (parking).
bus_mut_mods() {
    bus_mods "$1" bus-member=q bus-member=s
    bus_cfg "$1" q 'no_override = true'
    bus_cfg "$1" s 'park = true'
}
# bus_mut_seq <log> - s's member-count changes, "a>b@walk ..." in order.
bus_mut_seq() { grep -o 'bus-member s: park members [0-9]* -> [0-9]* at walk [0-9]*' "$1" | sed 's/.*members \([0-9]*\) -> \([0-9]*\) at walk \([0-9]*\)/\1>\2@\3/' | paste -sd' ' -; }
# bus_mut_check <label> <log> <want seq regex> - no entry after a shutdown, no key twice in a walk, the sequence matches.
bus_mut_check() {
    local seq dups after
    seq="$(bus_mut_seq "$2")"
    dups="$(grep -o 'bus-member s: park walks=[0-9]* dups=[0-9]*' "$2" | tail -1 | sed 's/.*dups=//')"
    after="$(grep -c 'handler entered after the shutdown hook' "$2")"
    if [ "$after" = 0 ] && [ "${dups:-x}" = 0 ] && printf '%s\n' "$seq" | grep -Eq "^($3)\$"; then
        ok "$1: members per walk $seq; no handler entered after its shutdown hook, none twice in a walk"
    else
        bad "$1: members per walk '$seq' (want /$3/), dups ${dups:-none}, entries after shutdown $after (see $2)"
    fi
}
if bus_on mutation; then
say "bus: mutation - q hot reloaded while s's handler is parked in the walk"
d="$WORK/bus-mut-rl"; log="$WORK/bus-mut-rl.log"
bus_mut_mods "$d"
OPENPETE_MODS_CFG_APPLY='200:q:gen=1' run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
if grep -q 'hot reload: q live' "$log" && [ "$(grep -c 'bus-member q: entry key=' "$log")" = 2 ]; then
    # 2 members, then 1 for the parked walk whose q row was retired, then 2.
    bus_mut_check "reload" "$log" '0>2@1 2>1@[0-9]+ 1>2@[0-9]+'
else
    bad "reload: q was not reloaded (see $log)"
fi

say "bus: mutation - a hot reload of q that fails after the strip, while s's handler is parked"
d="$WORK/bus-mut-fail"; log="$WORK/bus-mut-fail.log"
bus_mut_mods "$d"
OPENPETE_MODS_CFG_APPLY='200:q:fail_entry=1' run_game "$d" 800 > "$log" 2>&1
bus_keep "$log"
if grep -q 'bus-member q: entry failing' "$log" && [ "$(grep -c 'bus-member q: shutdown draws=' "$log")" = 1 ]; then
    bus_mut_check "failed reload" "$log" '0>2@1 2>1@[0-9]+'
else
    bad "failed reload: q's second entry did not fail (see $log)"
fi

say "bus: mutation - a deferred first enable of q while s's handler is parked (windowed)"
d="$WORK/bus-mut-late"; log="$WORK/bus-mut-late.log"
bus_mut_mods "$d"
printf 's\n' > "$d/enabled.txt"   # q: a load-all row, entered at its first enable
bus_window "$d" "$log" 600 OPENPETE_MODS_TOGGLE='200:q:1'
rc=$?
bus_keep "$log"
if [ "$rc" -eq 64 ]; then
    bad "late enable: $(cat "$log")"
elif grep -q 'live toggle: q ENABLED' "$log" && [ "$(grep -c 'bus-member q: entry key=' "$log")" = 1 ]; then
    bus_mut_check "late enable" "$log" '0>1@1 1>2@[0-9]+'
else
    bad "late enable: q was not entered late (rc=$rc, see $log)"
fi
fi # mutation
