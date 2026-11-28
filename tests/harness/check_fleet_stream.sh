#!/usr/bin/env bash
#
# check_fleet_stream.sh - the golden fleet's streamed verdict lines, unit-end
# check, stop and expected moves on synthetic inputs: no binary, no disc, no
# capture image, no remote host.
#
# Row stream (scripts/corpus-resume.sh under OPENPETE_STREAM_FILE, PNG tiles
# drawn here, a blessed.tsv and ident-baseline.tsv written for them):
#   lines       a finished row streams one line per tile: SAME, DIVERGE (a
#               tile unlike its row), EXTRA (a tile with no row), IDENT and
#               EXCEED (identity pairs within and over the baseline); a
#               partial row streams nothing
#   classify    every streamed verb equals the end verdict's classification of
#               the same directory (presentation-corpus-diff.py and
#               presentation-ident-check.py)
#   pool        a row of 40 tiles (pb.digest_files' process pool) streams 40 lines
#   poke        OPENPETE_STREAM_POKE turns a SAME tile into DIVERGE and the
#               row's stamp holds the poked tile's sha256
#   stream-only the stream without OPENPETE_RESUME_RUN: lines, no stamp
#   off         neither: no line, no resume directory
#   resume      an attempt killed after one row, then a re-queued attempt that
#               imports it and captures the rest, streams the uninterrupted
#               run's lines (sets of verb, tile and detail), the imported ones
#               tagged `imported`, the credits leg included
#   repeat      repeat copies (`<key>~r<k>`): pg_copy_keys; the diff's
#               per-row accounting (all copies SAME and no EXTRA, one
#               divergent copy tile, a missing copy tile, a copy number past
#               N); copies stream SAME and IDENT against the original rows
#               and baseline; list_tiles and promote_select leave copies out;
#               a copy's stamp imports only into a repeat run holding it
#   promote-stream  presentation-corpus-promote.sh refuses a fleet stream
#               directory holding no capture record, naming it
# Coordinator (tests/harness/fleet-stream-check.py, golden-fleet.py imported,
# a fake supervisor on this machine): poll framing, the unit-end check and
# its refusals under OPENPETE_STREAM_DROP, kill in the last poll interval and
# re-queue, the exemptions, stop (running, pending, local, racing
# completion, no ledger row), the transport retry of a connect failure
# (transient, persistent, a remote exit not retried), expected moves, the run candidate an accepted
# pull leaves in the stream directory, the kept rows of a stopped and
# of an attempts-exhausted unit, no ledger row from a filtered unit or an
# import from outside the run, and takeover over simulated hosts on a
# virtual clock (last-finishing unit, not paying, slower host, --no-takeover,
# completion racing the stop, the ledger row over both attempts), and the
# repository sync base (reuse from another checkout, checksum trust, recheck,
# locks, concurrent runs, build seed, status and clean) with concurrent hosts.
#
# Exit: 0 every case holds, 1 a case failed (named).
set -u
cd "$(dirname "$0")/../.."
# shellcheck source=scripts/presentation-game.sh
source scripts/presentation-game.sh
# shellcheck source=scripts/corpus-resume.sh
source scripts/corpus-resume.sh

W="$(mktemp -d -t fleet-stream.XXXXXX)"
trap 'rm -rf "${W:?}"' EXIT
fails=0
pass() { echo "ok   $1"; }
fail() { echo "FAIL $1: $2"; fails=$((fails + 1)); }

export OPENPETE_CAP_LODS=original      # 3 legs per row
PG_GAME=spyro-1 PG_TIER=presentation
PG_CORPUS_MANIFEST="$W/presentation-corpus.manifest"
PG_FREECAM_MANIFEST="$W/freecam-corpus.manifest"
PG_BLESSED="$W/blessed.tsv" PG_IDENT_BASELINE="$W/ident-baseline.tsv"
ROW_A='rowa | 100 | 100'
ROW_B='rowb | 100 | 100'
ROW_C='rowc | 100 | 100'
printf '%s\n%s\n%s\n' "$ROW_A" "$ROW_B" "$ROW_C" >"$PG_CORPUS_MANIFEST"
: >"$PG_FREECAM_MANIFEST"
printf 'head: 0123\nsource-tree: abcd\n' >"$W/fields.txt"
head -c 4096 /dev/urandom >"$W/bin"
export OPENPETE_CAPTURE_FIELDS="$W/fields.txt"
echo "[rhi/vk] device='llvmpipe (LLVM 15.0.7, 256 bits)' api=1.3 driver-info='Mesa 23.2.1'" >"$W/leg.log"

# Tiles: solid 8x8 images by colour; the blessed digests name the `ref`
# colour, so a tile drawn in another colour diverges.
python3 - "$W" <<'PY'
import importlib.util, os, sys
from PIL import Image
w = sys.argv[1]
spec = importlib.util.spec_from_file_location("pb", "scripts/presentation-blessed.py")
pb = importlib.util.module_from_spec(spec); spec.loader.exec_module(pb)
def img(name, rgb, odd=0):
    im = Image.new("RGB", (8, 8), rgb)
    for i in range(odd):
        im.putpixel((i, 0), (255, 255, 255))
    im.save(os.path.join(w, "src", name))
os.makedirs(os.path.join(w, "src"))
ref, other = (10, 20, 30), (200, 20, 30)
# rowa: canon SAME, inter DIVERGE, ident = canon (IDENT 0)
img("rowa__canon__100-vk.png", ref); img("rowa__inter__100-vk.png", other)
img("rowa__ident__100-vk.png", ref)
# rowb: canon SAME, inter SAME, ident 3 pixels off its canon over a baseline of 1 (EXCEED)
img("rowb__canon__100-vk.png", ref); img("rowb__inter__100-vk.png", ref)
img("rowb__ident__100-vk.png", ref, odd=3)
# rowc: no blessed row for canon (EXTRA), inter SAME, ident 1 pixel within its baseline of 2
img("rowc__canon__100-vk.png", ref); img("rowc__inter__100-vk.png", ref)
img("rowc__ident__100-vk.png", ref, odd=1)
d = pb.digest_image(Image.new("RGB", (8, 8), ref))[0]
rows = [{"tile": t, "digest": d, "revision": "0" * 40, "recomp_input": "-",
         "generated_tree": "-", "origin": "mapped"}
        for t in ("rowa__canon__100-vk.png", "rowa__inter__100-vk.png",
                  "rowb__canon__100-vk.png", "rowb__inter__100-vk.png",
                  "rowc__inter__100-vk.png")]
open(os.path.join(w, "blessed.tsv"), "w").write(pb.format_manifest(pb.Manifest({"game": "spyro-1"}, rows)))
open(os.path.join(w, "ident-baseline.tsv"), "w").write(
    "# presentation identity-leg baseline\n# tile\tn\nrowb__ident__100-vk.png\t1\nrowc__ident__100-vk.png\t2\n")
PY

# One leg: copy its tiles from src, then record it. args: kind key row leg tile...
leg() {
  local kind="$1" key="$2" row="$3" lg="$4" t; shift 4
  for t in "$@"; do cp "$W/src/$t" "$OUT/$t"; done
  rs_leg_done "$kind" "$key" "$row" "$lg" "$W/leg.log" "$@"
}
row() {  # key row-line
  local lg; for lg in canon inter ident; do leg bk2 "$1" "$2" "original-$lg" "${1}__${lg}__100-vk.png"; done
}
# verb tile detail of a stream, sorted, without times and tags. args: file
keys() { awk '$1 == "END" { print $1; next } $1 == "CREDITS" { print $1, $2; next } { print $1, $2, $3 }' "$1" | sort; }

# Uninterrupted run: three rows, stamps and stream on.
export OPENPETE_RESUME_RUN=run-1 OPENPETE_STREAM_FILE="$W/full.stream"
OUT="$W/full"; mkdir -p "$OUT"; rs_init "$OUT" "$W/bin" default
row rowa "$ROW_A"; row rowb "$ROW_B"; row rowc "$ROW_C"
printf 'rc=0\n' >"$OUT/credits-verdict.txt"; rs_stream_credits 0
rs_files_done credits credits credits-interp credits-verdict.txt
rs_stream_end
want="$(printf '%s\n' 'DIVERGE rowa__inter__100-vk.png' 'EXCEED rowb__ident__100-vk.png' \
  'EXTRA rowc__canon__100-vk.png' 'IDENT rowa__ident__100-vk.png' 'IDENT rowc__ident__100-vk.png' \
  'SAME rowa__canon__100-vk.png' 'SAME rowb__canon__100-vk.png' 'SAME rowb__inter__100-vk.png' \
  'SAME rowc__inter__100-vk.png' | sort)"
got="$(awk '$1 != "CREDITS" && $1 != "END" { print $1, $2 }' "$W/full.stream" | sort)"
n=$(wc -l <"$W/full.stream")
if [[ "$got" == "$want" ]] && grep -q '^CREDITS rc=0 ' "$W/full.stream" && grep -q '^END ' "$W/full.stream" \
   && grep -q '^EXCEED rowb__ident__100-vk.png pixels=3/1 ' "$W/full.stream" \
   && grep -q '^IDENT rowc__ident__100-vk.png pixels=1/2 ' "$W/full.stream"; then
  pass "lines: $n lines (9 tiles, CREDITS, END); SAME 4, DIVERGE 1, EXTRA 1, IDENT 2, EXCEED 1"
else fail lines "$(cat "$W/full.stream")"; fi
awk '$1 == "END" && NF == 2 { next } $1 == "CREDITS" && NF == 3 { next } NF != 4 { bad++ } END { exit bad > 0 }' "$W/full.stream" \
  && pass "line format: verb tile detail epoch; CREDITS rc=N epoch; END epoch" || fail "line format" "$(cat "$W/full.stream")"

# The end verdict's own scripts over the same directory.
python3 tests/harness/presentation-corpus-diff.py "$OUT" --manifest "$PG_BLESSED" --diff-out "$W/diff" >"$W/diff.log" 2>&1
python3 tests/harness/presentation-ident-check.py "$OUT" --baseline "$PG_IDENT_BASELINE" >"$W/ident.log" 2>&1
endv="$( { awk '$1 == "ok" { print "SAME", $2 } $1 == "DIVERGE" || $1 == "EXTRA" { print $1, $2 }' "$W/diff.log"
           awk '$1 == "EXCEED" || $1 == "NOCANON" { print "EXCEED", $2 } $1 == "within" || $1 == "ident" { print "IDENT", $2 }' "$W/ident.log"; } | sort)"
[[ "$endv" == "$want" ]] && pass "classify: the stream's 9 verbs equal the end verdict's" \
  || fail classify "end verdict: $endv"

# A row of 32 tiles or more digests through pb.digest_files' process pool.
mkdir -p "$W/pool"
python3 -c 'import sys; from PIL import Image
for i in range(40): Image.new("RGB", (8, 8), (i, 0, 0)).save(f"{sys.argv[1]}/rowm__canon__{i}-vk.png")' "$W/pool"
python3 tests/harness/presentation-stream.py "$W/pool" $(cd "$W/pool" && ls) --manifest "$PG_BLESSED" \
  --baseline "$PG_IDENT_BASELINE" --stream "$W/pool.stream" >"$W/pool.log" 2>&1
n=$(grep -c '^EXTRA rowm__canon__' "$W/pool.stream" 2>/dev/null)
[[ "$n" -eq 40 ]] && pass "pool: 40 tiles in one row stream 40 lines" || fail pool "${n:-0} lines; $(tail -3 "$W/pool.log")"

# Poke: rowb's canon tile changed before its leg hashes it.
OPENPETE_STREAM_POKE=rowb__canon OPENPETE_STREAM_FILE="$W/poke.stream" OPENPETE_RESUME_RUN=run-p bash -c '
  source scripts/presentation-game.sh; source scripts/corpus-resume.sh
  PG_GAME=spyro-1 PG_TIER=presentation PG_BLESSED="$1/blessed.tsv" PG_IDENT_BASELINE="$1/ident-baseline.tsv"
  OUT="$1/poke"; mkdir -p "$OUT"; rs_init "$OUT" "$1/bin" default
  for lg in canon inter ident; do cp "$1/src/rowb__${lg}__100-vk.png" "$OUT/"
    rs_leg_done bk2 rowb "rowb | 100 | 100" "original-$lg" "$1/leg.log" "rowb__${lg}__100-vk.png"; done' _ "$W" >"$W/poke.log" 2>&1
sha="$(sha256sum <"$W/poke/rowb__canon__100-vk.png" | cut -d' ' -f1)"
grep -q '^DIVERGE rowb__canon__100-vk.png ' "$W/poke.stream" && grep -q "^tile: $sha rowb__canon__100-vk.png$" "$W/poke/resume/bk2.rowb.stamp" \
  && pass "poke: the poked tile streams DIVERGE and its stamp holds the poked sha256" \
  || fail poke "$(cat "$W/poke.log" "$W/poke.stream")"

# Stream only, and off.
unset OPENPETE_RESUME_RUN
OPENPETE_STREAM_FILE="$W/only.stream"; OUT="$W/only"; mkdir -p "$OUT"; rs_init "$OUT" "$W/bin" default
row rowa "$ROW_A"
[[ $RS_ON -eq 0 && $(wc -l <"$W/only.stream") -eq 3 && -z "$(find "$OUT/resume" -name '*.stamp')" ]] \
  && pass "stream-only: 3 lines for one row, no stamp" || fail stream-only "$(ls -R "$OUT/resume"; cat "$W/only.stream")"
unset OPENPETE_STREAM_FILE
OUT="$W/off"; mkdir -p "$OUT"; rs_init "$OUT" "$W/bin" default
row rowa "$ROW_A"
[[ -z "$RS_STREAM" && ! -e "$OUT/resume" ]] && pass "off: no line, no resume directory" || fail off "$(ls -R "$OUT")"

# Resume: attempt 1 finishes rowa and two legs of rowb, then dies; attempt 2
# imports rowa and captures rowb and rowc.
export OPENPETE_RESUME_RUN=run-r OPENPETE_STREAM_FILE="$W/a1.stream"
OUT="$W/cand"; mkdir -p "$OUT"; rs_init "$OUT" "$W/bin" default
row rowa "$ROW_A"
leg bk2 rowb "$ROW_B" original-canon rowb__canon__100-vk.png
leg bk2 rowb "$ROW_B" original-inter rowb__inter__100-vk.png
printf 'rc=0\n' >"$OUT/credits-verdict.txt"; rs_stream_credits 0
rs_files_done credits credits credits-interp credits-verdict.txt
OPENPETE_STREAM_FILE="$W/a2.stream"; rs_init "$OUT" "$W/bin" default
rs_import "$OUT" "" "" >"$W/import.log"
row rowb "$ROW_B"; row rowc "$ROW_C"; rs_stream_end
imp=$(grep -c ' imported$' "$W/a2.stream")
if [[ "$(keys "$W/a2.stream" | grep -v '^END')" == "$(keys "$W/full.stream" | grep -v '^END')" && $imp -eq 4 \
      && $(grep -c '^CREDITS rc=0 .* imported$' "$W/a2.stream") -eq 1 ]]; then
  pass "resume: attempt 2 streams the uninterrupted run's $(grep -vc '^END' "$W/a2.stream") lines; $imp tagged imported (3 tiles, CREDITS)"
else fail resume "$(cat "$W/import.log"; diff <(keys "$W/a2.stream") <(keys "$W/full.stream"))"; fi
unset OPENPETE_RESUME_RUN OPENPETE_STREAM_FILE

# Repeat copies (PRESENTATION_REPEAT, `<key>~r<k>`): pg_copy_keys, the
# diff's per-row accounting against the original rows, the stream's verbs
# for copies, and the copy rule of the stamp import.
keys3="$(PRESENTATION_REPEAT=3 bash -c 'source scripts/presentation-game.sh; pg_repeat; pg_copy_keys rowa' | tr '\n' ' ')"
PRESENTATION_REPEAT=x bash -c 'source scripts/presentation-game.sh; pg_repeat' >/dev/null 2>&1; badrc=$?
[[ "$keys3" == "rowa~r01 rowa~r02 rowa~r03 " && $badrc -eq 64 ]] \
  && pass "repeat-keys: 3 copy keys rowa~r01..rowa~r03; PRESENTATION_REPEAT=x exits 64" \
  || fail repeat-keys "keys '$keys3', bad rc $badrc"
# rep/<case>: 3 copies of rowa (canon, inter, ident: 3 legs each) and of
# rowb, whose ident is 1 px off its canon, within rowb's baseline row of 1.
python3 - "$W" <<'PY'
import os, sys
from PIL import Image
w = sys.argv[1]
ref, other = (10, 20, 30), (200, 20, 30)
def img(d, name, rgb, odd=0):
    os.makedirs(d, exist_ok=True)
    im = Image.new("RGB", (8, 8), rgb)
    for i in range(odd):
        im.putpixel((i, 0), (255, 255, 255))
    im.save(os.path.join(d, name))
def copies(d, n=3):
    for k in range(1, n + 1):
        for leg in ("canon", "inter", "ident"):
            img(d, f"rowa~r{k:02d}__{leg}__100-vk.png", ref)
        img(d, f"rowb~r{k:02d}__canon__100-vk.png", ref); img(d, f"rowb~r{k:02d}__inter__100-vk.png", ref)
        img(d, f"rowb~r{k:02d}__ident__100-vk.png", ref, odd=1)
for case in ("same", "div", "short", "outside"):
    copies(os.path.join(w, "rep", case))
img(os.path.join(w, "rep", "div"), "rowa~r02__inter__100-vk.png", other)
os.remove(os.path.join(w, "rep", "short", "rowa~r03__inter__100-vk.png"))
img(os.path.join(w, "rep", "outside"), "rowa~r04__canon__100-vk.png", ref)
PY
rdiff() { python3 tests/harness/presentation-corpus-diff.py "$W/rep/$1" --manifest "$PG_BLESSED" --diff-out "$W/rep/$1.diff" \
            --expect-subset --repeat 3 --baseline "$PG_IDENT_BASELINE" >"$W/rep/$1.log" 2>&1; }
rdiff same; rc=$?
if [[ $rc -eq 0 ]] && grep -q '^REPEAT   rowa  divergent=0 of=9 compared=9 copies=3$' "$W/rep/same.log" \
   && grep -q '^REPEAT   rowb  divergent=0 of=9 compared=9 copies=3$' "$W/rep/same.log" \
   && ! grep -q '^EXTRA' "$W/rep/same.log" \
   && grep -q 'repeat 3: 2 row(s), 0 divergent of 18 legs, 18 compared' "$W/rep/same.log"; then
  pass "repeat-same: 2 rows x 3 copies, 0 divergent of 18 legs, no EXTRA, exit 0"
else fail repeat-same "rc $rc; $(cat "$W/rep/same.log")"; fi
rdiff div; rc=$?
[[ $rc -eq 1 ]] && grep -q '^DIVERGE  rowa~r02__inter__100-vk.png ' "$W/rep/div.log" \
  && grep -q '^REPEAT   rowa  divergent=1 of=9 compared=9 ' "$W/rep/div.log" \
  && pass "repeat-div: one divergent copy tile: DIVERGE under the copy's name, rowa divergent=1 of 9, exit 1" \
  || fail repeat-div "rc $rc; $(cat "$W/rep/div.log")"
rdiff short; rc=$?
[[ $rc -eq 1 ]] && grep -q '^REPEAT   rowa  divergent=0 of=9 compared=8 ' "$W/rep/short.log" \
  && pass "repeat-short: a missing copy tile: compared 8 of 9 legs, exit 1" \
  || fail repeat-short "rc $rc; $(cat "$W/rep/short.log")"
rdiff outside; rc=$?
[[ $rc -eq 1 ]] && grep -q '^REPEAT   rowa  divergent=0 of=9 compared=9 copies=3 outside=1..3:4$' "$W/rep/outside.log" \
  && pass "repeat-outside: copy 4 of a repeat 3 is refused, exit 1" \
  || fail repeat-outside "rc $rc; $(cat "$W/rep/outside.log")"
census="$(python3 -c '
import importlib.util, sys
s = importlib.util.spec_from_file_location("pb", "scripts/presentation-blessed.py"); pb = importlib.util.module_from_spec(s); s.loader.exec_module(pb)
print(len(pb.list_tiles(sys.argv[1])), len(pb.list_copies(sys.argv[1])), len(pb.list_copies(sys.argv[1], ident=True)),
      len(pb.promote_select(sys.argv[1], True, [])), len(pb.promote_select(sys.argv[1], False, ["rowa"])))' "$W/rep/same")"
[[ "$census" == "0 12 6 0 0" ]] && pass "repeat-select: 12 golden and 6 identity copies; list_tiles and promote (--all, a filter) select none" \
  || fail repeat-select "$census"
python3 tests/harness/presentation-stream.py "$W/rep/same" $(cd "$W/rep/same" && ls -- *-vk.png) --manifest "$PG_BLESSED" \
  --baseline "$PG_IDENT_BASELINE" --stream "$W/rep/same.stream" >"$W/rep/stream.log" 2>&1
sv="$(awk '{ n[$1]++ } END { printf "SAME=%d IDENT=%d EXTRA=%d EXCEED=%d", n["SAME"], n["IDENT"], n["EXTRA"], n["EXCEED"] }' "$W/rep/same.stream" 2>/dev/null)"
[[ "$sv" == "SAME=12 IDENT=6 EXTRA=0 EXCEED=0" ]] \
  && pass "repeat-stream: copies stream SAME against the original rows and IDENT within the original's baseline" \
  || fail repeat-stream "$sv; $(cat "$W/rep/stream.log")"
# A copy's stamp imports only into a repeat run whose copies include it.
export OPENPETE_RESUME_RUN=run-rep
OUT="$W/repimp"; mkdir -p "$OUT"; rs_init "$OUT" "$W/bin" default
for lg in canon inter ident; do cp "$W/rep/same/rowa~r02__${lg}__100-vk.png" "$OUT/"
  rs_leg_done bk2 'rowa~r02' "$ROW_A" "original-$lg" "$W/leg.log" "rowa~r02__${lg}__100-vk.png"; done
cp -a "$OUT" "$W/repimp.keep"
nimp=""
for r in 0 1 3; do
  rm -rf "${OUT:?}"; cp -a "$W/repimp.keep" "$OUT"
  PG_REPEAT=$r rs_import "$OUT" "" "" >"$W/repimp-$r.log"; nimp+="$(wc -l <"$OUT/resume/imported.tsv")"
done
unset OPENPETE_RESUME_RUN
[[ "$nimp" == "001" ]] && grep -q 'outside this run.s repeat 0' "$W/repimp-0.log" \
  && grep -q 'outside this run.s repeat 1' "$W/repimp-1.log" \
  && pass "repeat-import: copy 2's stamp imports under repeat 3, not under repeat 0 or 1" \
  || fail repeat-import "imported under repeat 0/1/3: $nimp; $(cat "$W"/repimp-*.log)"

# promote-stream: a fleet stream directory without a capture record is refused
# before the capture image is entered, naming the cause; one with the record
# passes that check (it reaches the image step, so only the refusal is run).
SD="$W/build/fleet-stream/run-x/spyro-1-presentation"; mkdir -p "$SD"
cp "$W/src/rowa__canon__100-vk.png" "$SD/"
out="$(tests/harness/presentation-corpus-promote.sh "$SD" rowa 2>&1)"; prc=$?
[[ $prc -eq 1 && "$out" == *"holds streamed tiles and no capture-record.txt"* ]] \
  && pass "promote-stream: a stream directory without a record refused (rc 1) naming the missing record" \
  || fail promote-stream "rc $prc: $out"

python3 tests/harness/fleet-stream-check.py || fails=$((fails + 1))

if [[ $fails -ne 0 ]]; then echo "[fleet-stream] FAIL: $fails case(s)"; exit 1; fi
echo "[fleet-stream] PASS: every case"
exit 0
