#!/usr/bin/env bash
# check_moby_occlusion.sh - moby decode census by model kind at freecam poses,
# extended LOD against original LOD, with the image held byte-identical.
#
# The moby occlusion cull skips the decode of a record whose bounds lie wholly
# behind nearer world depth. A record kind without bounds is decoded on every
# present however hidden it is, which costs time and changes no pixel, so
# neither the presentation golden nor the oracle can see it. This harness
# reads the work instead. Per row of games/<game>/corpus/moby-occlusion-corpus.manifest
# it runs three headless legs at the row's freecam pose:
#   ext   --lod extended
#   ref   --lod extended with OPENPETE_MOBY_SIMPLE_OCCL=0 (the simple-kind
#         occlusion test off: the decode set of a build without it)
#   orig  --lod original
# Each leg captures one tick present and 21 sub-tick presents at alpha 0.5
# (--screenshot-intermediate-on-frame); the occlusion grid and the verdicts
# the sub-tick replays hold come from those presents, so a canonical-only
# run never culls a moby.
#
# Instrument (stderr, OPENPETE_MOBY_KIND_CENSUS=1), one line per pass:
#   [moby-kind vbl=<V> leg=canon|sub kind=anim|simple] recs=<n> occl=<n>
#       verts=<n> tris=<n>
# and one per extended-LOD reveal walk (kind=anim-reveal|simple-reveal: recs
# is the records appended, occl the candidates the occlusion test culled).
# The census of a leg is the median of its sub-tick decode lines inside the
# VBL span of its intermediate captures; culled is the sum of occl over every
# line of that span.
#
# Checks: every row requires each screenshot of the ext leg byte-identical to
# the ref leg's; the row's manifest checks add count criteria (see the
# manifest header).
# Engagement (VACUOUS, exit 1): a leg with fewer than MIN_BRACKETED bracketed
# intermediate captures, fewer than MIN_LINES sub-tick census lines of a kind
# a check reads (both kinds on ext and ref, the animated kind alone on Spyro 1,
# which has no simple pass; the simple kind on orig for a max_ratio row), or a
# missing screenshot; a row marked engage whose ext leg culls no record in the
# span.
#
# Pan rows (games/<game>/corpus/moby-occlusion-pan.manifest, when the game
# has one) replay a bk2 that turns the camera, windowed under a private Xvfb
# display (scripts/lib/xvfb.sh): a headless run builds no occlusion grid, so
# its cull never decides. Each row runs RUNS times with the occlusion audit on
# (OPENPETE_MOBY_OCCL_VERIFY=1 OPENPETE_MOBY_OCCL_DBG=1), which decodes every
# record the cull judged hidden and prints one [occl-VERIFY-WRONG] line per
# record whose fresh bounds query visible. A wrong cull whose cached rect is
# at most 2 ticks old is PAN when the fresh rect leaves the cached rect by
# more than the own-motion inflation of the cull query (8 px plus (age+1)
# times the larger of 102400/max(vz,512) and 1.5 x the measured motion, at
# the default --fov), so only camera motion moved it there; else NONPAN. An
# older rect is re-projected at the present's camera (REFRESHED, unclassed);
# a simple-moby record's bounds are built at the present's camera
# ([occl-VERIFY-simple-WRONG], SIMPLE, unclassed).
# The row fails when the PAN total over its runs exceeds pan_max. Engagement
# (VACUOUS): a run that did not load the bk2, did not exit at the frame
# limit, printed no audit line, or judged no record hidden (would_cull 0).
# The culled fraction would_cull / presents (subtick-view presents) is
# printed per run.
#
# Usage: tests/harness/check_moby_occlusion.sh [--game SLUG] [--build-dir=DIR]
#            [--filter SUBSTR] [--keep DIR]
#   --filter  run only rows whose name contains SUBSTR
#   --keep    copy every leg's log and screenshots to DIR/<row>/<leg>/
# Exit 0 PASS, 1 FAIL or VACUOUS, 64 SKIP (binary, disc or manifest missing).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/lib/game.sh
source scripts/lib/game.sh

FILTER="" KEEP=""
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    case "${args[i]}" in
        --build-dir=*) PG_BUILD_DIR="${args[i]#--build-dir=}";;
        --filter) FILTER="${args[i+1]:-}"; i=$((i + 1));;
        --filter=*) FILTER="${args[i]#--filter=}";;
        --keep) KEEP="${args[i+1]:-}"; i=$((i + 1));;
        --keep=*) KEEP="${args[i]#--keep=}";;
    esac
done

GAME="$(pg_parse_game "$@")" || exit 64
pg_resolve "$GAME"
MANIFEST="games/$GAME/corpus/moby-occlusion-corpus.manifest"
PAN_MANIFEST="games/$GAME/corpus/moby-occlusion-pan.manifest"
[[ -f "$MANIFEST" ]] || { echo "moby-occlusion: SKIP ($GAME has no $MANIFEST)"; exit 64; }
[[ -x "$PG_BIN" && -f "$PG_DISC" ]] || { echo "moby-occlusion: SKIP (binary or disc missing)"; exit 64; }
python3 -c '' 2>/dev/null || { echo "moby-occlusion: SKIP (python3 missing)"; exit 64; }

# shellcheck source=scripts/lib/swrender.sh
source scripts/lib/swrender.sh
sw_render_env

CANON_VBL=2473
INTER_VBLS="$(seq -s, 2491 2 2531)"
FRAME_LIMIT=2533
MIN_BRACKETED=15
MIN_LINES=8

OUT="${PG_BUILD_DIR:-build/linux-release}/moby-occlusion/$GAME"
mkdir -p "$OUT"

# One leg. args: row level pose leg [run args...]; writes $OUT/<row>__<leg>.log
# and its PNGs.
run_leg() {
    local row="$1" level="$2" pose="$3" leg="$4"
    shift 4
    local tag="$OUT/${row}__${leg}" lod=extended
    [[ "$leg" == orig ]] && lod=original
    rm -f "${tag:?}.log" "${tag:?}"__*.png
    local -a outs=("${tag}__${CANON_VBL}-vk.png")
    local v
    for v in ${INTER_VBLS//,/ }; do outs+=("${tag}__${v}-vk.png"); done
    PG_CAPTURE_ENV=(OPENPETE_MOBY_KIND_CENSUS=1 OPENPETE_INTERMEDIATE_ALPHA=0.5)
    [[ "$leg" == ref ]] && PG_CAPTURE_ENV+=(OPENPETE_MOBY_SIMPLE_OCCL=0)
    PG_CAPTURE_TIMEOUT=600
    local outlist
    outlist="$(IFS=,; echo "${outs[*]}")"
    pg_capture "$tag.log" "$outlist" -- \
        --renderer vk --aspect 16:9 --lod "$lod" --no-card --mute 1 \
        --skip-to-level "$level" --freecam-pose "$pose" \
        --screenshot-native-only --screenshot-on-frame "$CANON_VBL" \
        --screenshot-intermediate-on-frame "$INTER_VBLS" \
        --screenshot-output "${tag}__{n}.png" --frame-limit "$FRAME_LIMIT" "$@"
    local rc=$?
    echo "moby-occlusion: leg $row/$leg rc=$rc"
    if [[ -n "$KEEP" ]]; then
        mkdir -p "$KEEP/$row/$leg"
        cp -f "$tag.log" "${tag}"__*.png "$KEEP/$row/$leg/" 2>/dev/null
    fi
}

# Pan rows: <name> | <bk2> | <frame limit> | <checks>. Parsed first, so a
# missing Xvfb SKIPs before any leg runs.
pan_rows=()
if [[ -f "$PAN_MANIFEST" ]]; then
    while IFS='|' read -r name bk2 frames checks; do
        name="$(echo "$name" | tr -d ' \t')"
        [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
        [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
        bk2="$(echo "$bk2" | tr -d ' \t')"
        frames="$(echo "$frames" | tr -d ' \t')"
        checks="$(echo "${checks:-}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
        pan_rows+=("$name|$bk2|$frames|$checks")
    done < "$PAN_MANIFEST"
fi
if (( ${#pan_rows[@]} )); then
    # shellcheck source=scripts/lib/xvfb.sh
    source scripts/lib/xvfb.sh
    op_xvfb_prefix moby-occlusion 1280x960
fi

# One windowed audit run of a pan row. args: row bk2 frames n; writes
# $OUT/<row>__run<n>.log.
run_pan() {
    local row="$1" bk2="$2" frames="$3" n="$4"
    local log="$OUT/${row}__run${n}.log"
    rm -f "${log:?}"
    local run_root
    run_root="$(op_sandbox_run_root)" || return 1
    op_sandbox_xdg "$run_root" || return 1
    op_own_run env OPENPETE_VIBRATION=0 "OPENPETE_SANDBOX=$run_root" \
        "OPENPETE_CARD_DIR=$run_root/cards" "${OP_SANDBOX_XDG_ENV[@]}" \
        OPENPETE_MOBY_OCCL_VERIFY=1 OPENPETE_MOBY_OCCL_DBG=1 \
        "${OP_XVFB[@]}" timeout -k "$OP_OWN_GRACE" 900 \
        "$PG_BIN" --no-config --disc "$PG_DISC" --disc-cache 0 "${PG_RUN_ARGS[@]}" \
        --window 640x480 --input-lock 1 --vibration 0 \
        --renderer vk --aspect 4:3 --lod original --no-card --mute 1 \
        --bk2 "$bk2" --frame-limit "$frames" >"$log" 2>&1
    local rc=$?
    rm -rf "${run_root:?}"
    echo "moby-occlusion: pan $row/run$n rc=$rc"
    if [[ -n "$KEEP" ]]; then
        mkdir -p "$KEEP/$row"
        cp -f "$log" "$KEEP/$row/" 2>/dev/null
    fi
}

rows=()
while IFS='|' read -r name level pose checks; do
    name="$(echo "$name" | tr -d ' \t')"
    [[ -z "$name" || "${name:0:1}" == "#" ]] && continue
    [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
    level="$(echo "$level" | tr -d ' \t')"
    pose="$(echo "$pose" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    checks="$(echo "${checks:-}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')"
    rows+=("$name|$checks")
    row_args=()
    for chk in $checks; do
        [[ "$chk" == sector_visibility=* ]] && row_args+=(--sector-visibility "${chk#sector_visibility=}")
    done
    for leg in ext ref orig; do run_leg "$name" "$level" "$pose" "$leg" "${row_args[@]}"; done
done < "$MANIFEST"
pan_args=()
for r in "${pan_rows[@]}"; do
    IFS='|' read -r name bk2 frames checks <<<"$r"
    runs=1
    for chk in $checks; do [[ "$chk" == runs=* ]] && runs="${chk#runs=}"; done
    for ((n = 1; n <= runs; n++)); do run_pan "$name" "$bk2" "$frames" "$n"; done
    pan_args+=("$name|$runs|$checks")
done
(( ${#rows[@]} + ${#pan_rows[@]} )) ||
    { echo "moby-occlusion: FAIL (no manifest row selected)"; exit 1; }

# Spyro 1 draws its simple models in the shaded draw hook, with no simple pass
# of its own (games/spyro-1/renderer/moby_game.c, simple_pass = NULL), so no
# simple-kind census line or simple-kind occlusion test exists there.
SIMPLE_KIND=1
[[ "$GAME" == spyro-1 ]] && SIMPLE_KIND=0

fc_verdict=0
if (( ${#rows[@]} )); then
python3 - "$OUT" "$CANON_VBL" "$INTER_VBLS" "$MIN_BRACKETED" "$MIN_LINES" "$SIMPLE_KIND" "${rows[@]}" <<'PYEOF'
import filecmp, os, re, statistics, sys

out, canon, inter = sys.argv[1], int(sys.argv[2]), [int(x) for x in sys.argv[3].split(',')]
min_br, min_lines = int(sys.argv[4]), int(sys.argv[5])
simple_kind = sys.argv[6] == '1'
rows = [r.split('|', 1) for r in sys.argv[7:]]

kind_re = re.compile(r'\[moby-kind vbl=(\d+) leg=(\w+) kind=([\w-]+)\] recs=(\d+) occl=(\d+) verts=(\d+) tris=(\d+)')
inter_re = re.compile(r'wrote intermediate .* on frame (\d+) \(target=(\d+), paired=\d+, bracketed=(\d+)')

def leg_census(path):
    """Median sub-tick census per kind inside the intermediate captures' VBL span."""
    frames, bracketed = [], 0
    lines = []
    for ln in open(path, errors='replace'):
        m = inter_re.search(ln)
        if m:
            frames.append(int(m.group(1)))
            bracketed += int(m.group(3))
            continue
        m = kind_re.search(ln)
        if m:
            lines.append(m.groups())
    res = {'bracketed': bracketed}
    if not frames:
        return res
    lo, hi = min(frames), max(frames)
    # Every record any pass of the window culled, reveal walks included.
    res['culled'] = sum(int(g[4]) for g in lines if lo <= int(g[0]) <= hi)
    for kind in ('anim', 'simple'):
        sel = [g for g in lines if g[1] == 'sub' and g[2] == kind and lo <= int(g[0]) <= hi]
        res[kind + '_n'] = len(sel)
        for i, key in enumerate(('recs', 'occl', 'verts', 'tris')):
            vals = [int(g[3 + i]) for g in sel]
            res[kind + '_' + key] = statistics.median(vals) if vals else None
    return res

def shots(row, leg):
    return [os.path.join(out, '%s__%s__%d-vk.png' % (row, leg, v)) for v in [canon] + inter]

verdict = 0
print('%-22s %-5s %4s %5s %6s %6s | %4s %5s %6s %6s | %9s %6s' %
      ('row', 'leg', 'aRec', 'aOccl', 'aVerts', 'aTris', 'sRec', 'sOccl', 'sVerts', 'sTris', 'bracketed', 'culled'))
for name, checks in rows:
    cen, notes, fail, vac = {}, [], [], []
    for leg in ('ext', 'ref', 'orig'):
        log = os.path.join(out, '%s__%s.log' % (name, leg))
        if not os.path.exists(log):
            vac.append('%s: no log' % leg); continue
        c = cen[leg] = leg_census(log)
        f = lambda k: '-' if c.get(k) is None else '%g' % c[k]
        print('%-22s %-5s %4s %5s %6s %6s | %4s %5s %6s %6s | %9d %6d' %
              (name, leg, f('anim_recs'), f('anim_occl'), f('anim_verts'), f('anim_tris'),
               f('simple_recs'), f('simple_occl'), f('simple_verts'), f('simple_tris'),
               c['bracketed'], c.get('culled', 0)))
        if c['bracketed'] < min_br:
            vac.append('%s: %d bracketed intermediate captures < %d' % (leg, c['bracketed'], min_br))
        # The ext and ref legs carry every check; the orig leg only the ratio.
        need = (('anim', 'simple') if simple_kind else ('anim',)) if leg != 'orig' else \
            (('simple',) if 'max_ratio=' in checks else ())
        for kind in need:
            if c.get(kind + '_n', 0) < min_lines:
                vac.append('%s: %d %s sub-tick census lines < %d' % (leg, c.get(kind + '_n', 0), kind, min_lines))
        missing = [p for p in shots(name, leg) if not os.path.exists(p)]
        if missing:
            vac.append('%s: %d screenshots missing' % (leg, len(missing)))
    # Image identity: ext against the leg with the simple-kind test off.
    if 'ext' in cen and 'ref' in cen:
        pairs = [(a, b) for a, b in zip(shots(name, 'ext'), shots(name, 'ref'))
                 if os.path.exists(a) and os.path.exists(b)]
        diff = [os.path.basename(a) for a, b in pairs if not filecmp.cmp(a, b, shallow=False)]
        if diff:
            fail.append('%d ext screenshots differ from ref (%s)' % (len(diff), ', '.join(diff[:4])))
        else:
            notes.append('images ext==ref (%d compared)' % len(pairs))
    st = lambda leg: cen.get(leg, {}).get('simple_tris')
    for chk in checks.split():
        if chk.startswith('max_ratio='):
            r = float(chk.split('=', 1)[1])
            e, o = st('ext'), st('orig')
            if e is None or o is None:
                vac.append('max_ratio: no simple census')
            elif e > r * max(o, 1):
                fail.append('ext simple tris %g > %g x orig %g' % (e, r, o))
            else:
                notes.append('ext simple tris %g <= %g x orig %g' % (e, r, o))
        elif chk == 'unchanged':
            e, f0 = st('ext'), st('ref')
            if e is None or f0 is None:
                vac.append('unchanged: no simple census')
            elif e != f0:
                fail.append('ext simple tris %g != ref %g' % (e, f0))
            else:
                notes.append('ext simple tris %g == ref' % e)
        elif chk == 'engage':
            occl = cen.get('ext', {}).get('culled', 0)
            if occl <= 0:
                vac.append('engage: ext leg culled no record')
            else:
                notes.append('ext culled %d records over the window' % occl)
        elif chk == 'report' or chk.startswith('sector_visibility='):
            pass
        else:
            vac.append('unknown check %s' % chk)
    v = 'VACUOUS' if vac else ('FAIL' if fail else 'PASS')
    if v != 'PASS':
        verdict = 1
    print('[moby-occlusion] row=%s verdict=%s %s' % (name, v, '; '.join(vac + fail + notes)))
print('[moby-occlusion] freecam verdict=%s' % ('PASS' if verdict == 0 else 'FAIL'))
sys.exit(verdict)
PYEOF
fc_verdict=$?
fi

pan_verdict=0
if (( ${#pan_rows[@]} )); then
python3 - "$OUT" "${pan_args[@]}" <<'PYEOF'
import os, re, sys

out = sys.argv[1]
rows = [r.split('|', 2) for r in sys.argv[2:]]

wrong_re = re.compile(r'\[occl-VERIFY-WRONG vbl=(\d+)\] moby=\w+ cached rect=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) '
                      r'vz=(-?\d+) age=(\d+) fresh rect=\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) vz=-?\d+ mot=([\d.]+)')
verify_re = re.compile(r'\[moby-occl-verify vbl=\d+\] would_cull=(\d+) wrong=(\d+)')
presents_re = re.compile(r'subtick-view: presents=(\d+)')

def classify(m):
    """PAN, NONPAN or REFRESHED for one wrong-cull line (see the header)."""
    c = [int(m.group(i)) for i in (2, 3, 4, 5)]
    vz, age = int(m.group(6)), int(m.group(7))
    f = [int(m.group(i)) for i in (8, 9, 10, 11)]
    mot = float(m.group(12))
    if age > 2:
        return 'REFRESHED'
    own = max(102400.0 / max(vz, 512), 1.5 * mot)
    inf = 8.0 + (age + 1) * own
    shift = max(c[0] - f[0], f[2] - c[2], c[1] - f[1], f[3] - c[3], 0)
    return 'PAN' if shift > inf else 'NONPAN'

def run_census(path):
    r = {'PAN': 0, 'NONPAN': 0, 'REFRESHED': 0, 'SIMPLE': 0, 'would': None, 'wrong': None,
         'presents': None, 'bk2': False, 'limit': False, 'pan_vbls': []}
    for ln in open(path, errors='replace'):
        m = wrong_re.search(ln)
        if m:
            k = classify(m)
            r[k] += 1
            if k == 'PAN':
                r['pan_vbls'].append(m.group(1))
            continue
        if '[occl-VERIFY-simple-WRONG ' in ln:
            r['SIMPLE'] += 1
            continue
        m = verify_re.search(ln)
        if m:
            r['would'], r['wrong'] = int(m.group(1)), int(m.group(2))
            continue
        m = presents_re.search(ln)
        if m:
            r['presents'] = int(m.group(1))
        elif 'bk2: loaded' in ln:
            r['bk2'] = True
        elif 'openpete-exit: reason=frame-limit' in ln:
            r['limit'] = True
    return r

verdict = 0
print('%-22s %-5s %10s %6s %4s %6s %9s %6s %8s %8s' %
      ('row', 'run', 'would_cull', 'wrong', 'PAN', 'NONPAN', 'REFRESHED', 'SIMPLE', 'presents', 'culled/p'))
for name, runs, checks in rows:
    vac, fail, notes = [], [], []
    tot = {'PAN': 0, 'NONPAN': 0, 'REFRESHED': 0, 'SIMPLE': 0}
    fracs = []
    for n in range(1, int(runs) + 1):
        log = os.path.join(out, '%s__run%d.log' % (name, n))
        if not os.path.exists(log):
            vac.append('run%d: no log' % n); continue
        r = run_census(log)
        frac = r['would'] / r['presents'] if r['would'] is not None and r['presents'] else None
        print('%-22s %-5s %10s %6s %4d %6d %9d %6d %8s %8s' %
              (name, 'run%d' % n, r['would'], r['wrong'], r['PAN'], r['NONPAN'], r['REFRESHED'], r['SIMPLE'],
               r['presents'], '-' if frac is None else '%.4f' % frac))
        for k in tot:
            tot[k] += r[k]
        if frac is not None:
            fracs.append(frac)
        if r['pan_vbls']:
            notes.append('run%d PAN at vbl %s' % (n, ','.join(r['pan_vbls'])))
        if not r['bk2']:
            vac.append('run%d: bk2 not loaded' % n)
        if not r['limit']:
            vac.append('run%d: did not exit at the frame limit' % n)
        if r['would'] is None:
            vac.append('run%d: no [moby-occl-verify] line' % n)
        elif r['would'] == 0:
            vac.append('run%d: would_cull 0' % n)
    for chk in checks.split():
        if chk.startswith('runs='):
            continue
        if chk.startswith('pan_max='):
            lim = int(chk.split('=', 1)[1])
            if tot['PAN'] > lim:
                fail.append('PAN wrong culls %d > %d' % (tot['PAN'], lim))
            else:
                notes.append('PAN wrong culls %d <= %d' % (tot['PAN'], lim))
        else:
            vac.append('unknown check %s' % chk)
    notes.append('NONPAN %d REFRESHED %d SIMPLE %d' % (tot['NONPAN'], tot['REFRESHED'], tot['SIMPLE']))
    if fracs:
        notes.append('culled/present mean %.4f' % (sum(fracs) / len(fracs)))
    v = 'VACUOUS' if vac else ('FAIL' if fail else 'PASS')
    if v != 'PASS':
        verdict = 1
    print('[moby-occlusion] row=%s verdict=%s %s' % (name, v, '; '.join(vac + fail + notes)))
print('[moby-occlusion] pan verdict=%s' % ('PASS' if verdict == 0 else 'FAIL'))
sys.exit(verdict)
PYEOF
pan_verdict=$?
fi

if (( fc_verdict == 0 && pan_verdict == 0 )); then
    echo '[moby-occlusion] verdict=PASS'
    exit 0
fi
echo '[moby-occlusion] verdict=FAIL'
exit 1
