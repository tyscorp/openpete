# shellcheck shell=bash
# mods_svc_rungs.sh - the services section of check_mods.sh: service tables
# between mods (svc_export, svc_get, svc_list). SOURCED by
# tests/harness/check_mods.sh (helpers say/ok/bad, run_game, $WORK,
# $MODS_SRC); Spyro 1 only.
#
# mods/svc-member is copied under several ids (each copy its own mod key):
# providers p1-p3 export probe/1, consumers c1 and c2 look them up, and a
# keyless copy probes the refusals. Each probe logs one
# "svc-member <id>: R <name>=<value>" line.

# svc_mods <dir> <id>... - copy svc-member into <dir> under each id and list
# the ids in enabled.txt in the given order.
svc_mods() {
    local dir="$1"; shift
    rm -rf "${dir:?}"
    mkdir -p "$dir"
    : > "$dir/enabled.txt"
    local id
    for id in "$@"; do
        cp -r "$MODS_SRC/svc-member" "$dir/$id"
        rm -rf "${dir:?}/$id/.build" "${dir:?}/$id/data"
        sed -i "s/^id *= *\"svc-member\"/id      = \"$id\"/" "$dir/$id/mod.toml"
        echo "$id" >> "$dir/enabled.txt"
    done
}

# svc_cfg <dir> <id> <toml line>... - write the copy's config.toml.
svc_cfg() {
    local dir="$1" id="$2"; shift 2
    printf '%s\n' "$@" > "$dir/$id/config.toml"
}

# svc_keep <log> - copy a leg's log to $BUS_KEEP when set (it outlives $WORK).
svc_keep() { [ -z "${BUS_KEEP:-}" ] || cp "$1" "$BUS_KEEP/"; }

# svc_r <log> <id> <name> - the value text of a copy's "R <name>=" line.
svc_r() { grep -o "svc-member $2: R $3=.*" "$1" | tail -1 | sed "s/^svc-member $2: R $3=//"; }

# svc_expect <label> <log> <id> <name> <regex> - PASS when the R line matches.
svc_expect() {
    local v; v="$(svc_r "$2" "$3" "$4")"
    if [ -n "$v" ] && printf '%s\n' "$v" | grep -Eq "^($5)\$"; then
        ok "$1: $3 $4=$v"
    else
        bad "$1: $3 $4=${v:-<no line>} (want /$5/; see $2)"
    fi
}

# svc_field <log> <name> - the integer after <name>= on the exit report line.
svc_field() { grep -o 'mods: svc: exports=.*' "$1" | tail -1 | grep -o "$2=[0-9]*" | sed 's/.*=//'; }

SVC_A=100 SVC_B=250 SVC_C=450 # GamestateDraw calls of probes A, B and C
SVC_KZ=svc-kz-jt48acb0        # hashes to the mod key 0

# ── lookup, provider order, a hot reload, a failed reload, a toggle ──────
# Entry sequence: c2 (no deps, listed first: its entry lookup of p1 finds
# nothing), p1, c1 (deps p1: its entry lookup finds p1's table), p2, p3, and
# the keyless copy, pinned at api 12 so it loads. Between probes A and B p1
# is hot reloaded with gen 1; between B and C p2's reload fails after the
# strip (fail_entry) and p3 is toggled off.
say "services: svc_get, svc_list and the kept pointer across a hot reload, a failed reload and a toggle"
d="$WORK/svc-main"; log="$WORK/svc-main.log"
svc_mods "$d" c2 c1 p1 p2 p3 "$SVC_KZ"
sed -i 's/^api *= *13/api     = 13\ndeps    = ["p1"]/' "$d/c1/mod.toml"
sed -i 's/^api *= *13/api     = 12/' "$d/$SVC_KZ/mod.toml"
svc_cfg "$d" c1 'consume = "p1,p2,p3"' 'entry_get = "p1"' \
    "probe_a = $SVC_A" "probe_b = $SVC_B" "probe_c = $SVC_C"
svc_cfg "$d" c2 'entry_get = "p1"'
svc_cfg "$d" p1 'provide = true'
svc_cfg "$d" p2 'provide = true' 'twice = true'
svc_cfg "$d" p3 'provide = true' 'major2 = true'
svc_cfg "$d" "$SVC_KZ" 'keyless_probe = true' "probe_a = $SVC_A"
OPENPETE_MODS_CFG_APPLY='160:p1:gen=1,300:p2:fail_entry=1' OPENPETE_MODS_TOGGLE='300:p3:0' \
    run_game "$d" 1000 > "$log" 2>&1
svc_keep "$log"
L="services"
svc_expect "$L" "$log" c1 entry_get '1'
svc_expect "$L" "$log" c2 entry_get '0'
svc_expect "$L" "$log" c1 entry_refusals '-1,-1,-1,-1'
svc_expect "$L" "$log" c1 list_a '3 p3,p2,p1,'
svc_expect "$L" "$log" c1 gets_a 'p1:0\.1,p2:0\.2,p3:0\.1,'
svc_expect "$L" "$log" c1 kept_a '1'
svc_expect "$L" "$log" c1 checks_a 'other_major=0 min_size=0 key0=0 unknown=0 other_name=0 stable=1 major2=1 p3,'
svc_expect "$L" "$log" c1 late_export '-1'
svc_expect "$L" "$log" "$SVC_KZ" keyless_export '-1'
svc_expect "$L" "$log" "$SVC_KZ" keyless_get '0 list=0'
svc_expect "$L" "$log" c1 list_b '3 p1,p3,p2,'
svc_expect "$L" "$log" c1 gets_b 'p1:1\.1,p2:0\.2,p3:0\.1,'
svc_expect "$L" "$log" c1 kept_b '2 moved=1'
svc_expect "$L" "$log" c1 list_c '1 p1,'
svc_expect "$L" "$log" c1 gets_c 'p1:1\.1,p2:-,p3:-,'
svc_expect "$L" "$log" c1 kept_c '2 moved=1'
if grep -q 'hot reload: p1 live' "$log" && grep -q 'svc-member p1: shutdown gen=0' "$log" &&
   grep -q 'svc-member p2: entry failing' "$log"; then
    ok "$L: p1 reloaded (old copy's shutdown ran), p2's reload entry failed"
else
    bad "$L: the reloads did not happen as scheduled (see $log)"
fi
if grep -q 'svc_export refused: svc_export is legal in the entry point only' "$log" &&
   grep -q 'svc_export refused: this mod is keyless' "$log" &&
   grep -q 'loads with api 12; its bus and service calls and mod_key are refused' "$log"; then
    ok "$L: the late export and the keyless lookup were refused with a log line"
else
    bad "$L: refusal log lines missing (see $log)"
fi
hits="$(svc_field "$log" hits)"; ex="$(svc_field "$log" exports)"
if [ "${hits:-0}" -gt 0 ] && [ "${ex:-0}" -gt 0 ]; then
    ok "$L: engaged (exports=$ex hits=$hits)"
else
    bad "$L: VACUOUS (exports=${ex:-none} hits=${hits:-none})"
fi
