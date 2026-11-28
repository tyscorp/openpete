#!/usr/bin/env python3
# sandbox-guard: no-launch (the game argv lines are fake process-table rows; the supervisor is a stub)
"""fleet-stream-check.py - the golden fleet coordinator's stream handling on this machine.

Run by tests/harness/check_fleet_stream.sh. scripts/golden-fleet.py is
imported; its Coordinator places jobs on a local host whose supervisor is a
fake (ship/supervise.sh sourcing a per-job, per-attempt plan written here),
so the real POLL_SCRIPT, KILL_SCRIPT, LAUNCH_SCRIPT, run_jobs, poll, stop
and unit-end check run against stream files and verdict reports the plans
write. Cases (each prints its counts):

  framing     POLL_SCRIPT frames state before bytes, an incomplete last line
              waits for the next poll, offsets continue across polls
  pool        pb.digest_files' process pool after the heat-diff import
  equality    a full unit's stream equals its report (SAME + DIVERGE =
              compared; DIVERGE, EXTRA, EXCEED, CREDITS sets)
  drop-div    OPENPETE_STREAM_DROP of a DIVERGE line: stream mismatch, rc 1
  drop-same   OPENPETE_STREAM_DROP of a SAME line on a passing unit: rc 0 -> 1
  crashed     CRASHED lines: parsed (malformed forms refused), printed as they
              arrive, the verdict and unit-end check unchanged; the result
              line counts crashed legs by retry outcome and names each
  extra       a unit whose only moves are EXTRA tiles (new rows): the stream
              pulls each tile without a heat diff, the accepted pull keeps
              it in the stream directory, promote_select sees it
  absent      a streamed and reported tile absent from the capture: the
              stream directory lacks it, stream mismatch names it
  exempt      an attempt without END (a timed-out leg) and a SKIP unit
  kill        lines written in the last poll interval are read before the
              kill; the re-queued attempt's lines equal the uninterrupted
              run's and pass the unit-end check
  stop        running, pending, local (golden lock held until teardown),
              racing completion both ways; no ledger row for a stopped unit
  stop-sync   a stop during a sync stand-in (a host script with a sleeping
              child) ends the run within STOP_POLL_S + 2 s: both processes
              gone, every unit stopped, done reason=stopped by user; a
              request naming another unit does not cover the run
  reap        REAP_SCRIPT ends a process carrying the run tag and spares an
              untagged one; a remote command and rsync's server carry the tag
  transport   a connect failure (ssh exit 255 with ssh's connect line,
              through a stand-in ssh on PATH) retried and logged for a
              script, an rsync push and a recheck; a remote command's own
              exit, 255 included, run once; a content change still a torn
              sync; a persistent failure fails script, push and recheck
              naming the host after 1 + len(TRANSPORT_BACKOFF_S) tries, and
              is not reported torn; retry=False tries once
  clean       build/fleet-stream/<run-id> removed once its run directory is gone
  expect      EXPECTED 1/0/0, UNEXPECTED 0/1/0, EXPECTED-NOT-MOVED 0/0/1,
              NOT-REACHED on a stopped unit and a timed-out row, typo and
              unknown-unit refusals (function and `run --expect`), resumed
              counts equal uninterrupted ones, summary counts equal the
              tagged lines of stream.log
  canary-verdict  canary-verdict.txt over identical canaries (both hosts
              pass), a differing tile (both fail), a canary leg that did not
              capture (it fails, the lone captured host fails with no
              cross-host comparison) and one host (canary=off); a pulled
              capture record stamped with fleet-run, fleet-host and
              fleet-run-dir

  move        `move` requests: an unknown unit, an unadmitted host and the
              unit's own host refused; an accepted move to a second host of
              the run carries the stamped rows (not an unstamped tile), the
              re-queued attempt runs pinned there outside the attempt budget,
              its stream covers every row, one ledger row over both
              attempts (host local>b); a finished unit
              refused; the CLI's request line, unknown host, done run and
              unknown run
  keep        a stopped unit's stamped rows and ship kept under kept/ at
              teardown, read by resolve_saved; unit_results reads the
              logged results `resume` decides on
  keep-exhausted  a unit past its deadline on every attempt ends `attempts
              exhausted` and its last attempt's stamped rows are kept the
              same way
  ledger      a whole unit writes a ledger row; a filtered run's unit and an
              accepted attempt that imported stamped rows from no attempt of
              this run write none, so the deadline on a speed-0.25 host stays
              the whole unit's time / speed x 2
  repeat      run --repeat N: the unit's FILTERS carry --repeat=N (a
              canary's do not); a unit of copy tiles streams SAME, equals its
              report and passes; its result names the row's divergent count
              of N x legs; check_report refuses fewer compared than N x legs,
              a report repeat count unlike the run's, a repeat that compared
              nothing and a report without a repeat-row line
  takeover-rule  takeover_prediction and takeover_refusal on fixed numbers
  takeover    simulated hosts (local 1.00, steam-machine 0.47, ptah 0.25) on
              a virtual clock (SimCo): the free local host takes over the
              unit predicted to finish last (spyro-2 on ptah), a later
              takeover saving under the threshold is not taken, the run
              ends earlier than under --no-takeover, the moved unit's
              stream holds every tile once and its ledger row sums both
              attempts (ptah>local); --no-takeover evaluates nothing
  takeover-slow  a free host slower than the running unit's takes nothing
  takeover-race  the unit finishes while its stop is in flight: its own
              attempt's result, no carry, one ledger row
  base-*      the repository sync base over a git repository and a second
              worktree of it, local hosts (and FAKE_SSH for base-ssh): the
              first run makes the base (no base, every byte sent); a run
              from the worktree (other path, other mtimes, one file changed)
              sends only that file and its snapshot equals the worktree; a
              base file tampered in place (size and mtime kept) is sent, not
              linked; a linked base file changed after the sync is refused
              by the recheck; a base recorded for another repository is not
              linked from and is replaced; two concurrent runs from both
              checkouts plus a later refresh leave each snapshot equal to
              its checkout; a refresh waits for a sync's shared lock and a
              sync for a refresh's exclusive one; the build seed only from a
              base of the same checkout path; status lines, a stale clean
              and --tree keep the base, --tree removes a checkout-keyed
              tree, a base of a gone repository and --all go; the ssh form
              (rsync server under flock and the run tag)
  on-hosts    hosts run concurrently (three 1 s children in 1 s); one
              host's failure kills the others' children and is raised
  host-wait   prepare_hosts over a scripted preflight: every host held by
              another run's golden lock waits and admits the host once its
              lock frees (the holder printed); --no-host-wait and a
              non-transient drop exit 64 at once; --host-wait 1 ends the
              wait with 64; a stop request ends it; a busy local host is
              left out as --no-local would (printed), kept by
              --local-anyway, admitted once a landing ends
  local-busy  local_busy over a fake process table: land-branch.sh and
              sched_gate.py processes and a game process outside the fleet
              count; a container's, a golden-fleet child's, a conmon
              child's, an own ancestor's and an editor's do not; the land
              lock held by flock counts until released; MemAvailable under
              6 GiB counts
  record-every-end  an accepted pull, a refused pull and an exit-2 unit
              each leave the stamped capture record and device line in the
              stream directory
  new-rows    a filtered unit whose only moves are extra tiles ends
              NEW-ROWS (rc 0) and the summary names them; a divergence, an
              identity failure, a crash, an unfiltered unit and a manifest
              mismatch stay FAIL
  expect-variant  an expect line naming a base row covers its @variant
              rows (freecam `<level>@<v>-<pose>` keys, a lodext leg, a
              repeat copy, a bk2 `<row>@<v>` key), not a variant at another
              pose; check_expect prints the variants covered

Exit: 0 every case holds, 1 a case failed (named).
"""
import argparse
import importlib.util
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TREE = os.path.abspath(os.path.join(HERE, "..", ".."))
_spec = importlib.util.spec_from_file_location("golden_fleet", os.path.join(TREE, "scripts", "golden-fleet.py"))
gf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gf)
gf.POLL_S = 0.3

FAILS = []
GAME, TIER = "spyro-2", "presentation"
UNIT = f"{GAME}-{TIER}"
TILES = sorted(r["tile"] for r in gf.load_pb(TREE).load_manifest(GAME, TIER).rows)


def check(name, ok, detail):
    """Print a case's verdict and counts; record a failure."""
    print(("ok   " if ok else "FAIL ") + f"{name}: {detail}")
    if not ok:
        FAILS.append(name)


FAKE_SUP = r'''#!/usr/bin/env bash
set -u
source "$1"
''' + gf.SH_STARTTIME + r'''
mkdir -p "$UNIT_DIR/cand"
echo "$$ $(starttime $$)" >"$ATTEMPT_BASE.pid"
emit() { printf '%s %s\n' "$*" "$(date +%s.%3N)" >>"$ATTEMPT_BASE.stream"; }
emit_imported() { printf '%s %s imported\n' "$*" "$(date +%s.%3N)" >>"$ATTEMPT_BASE.stream"; }
report() { printf '%s\n' "$@" >"$UNIT_DIR/cand/verdict-report.txt"; }
finish() {
    printf 'rc=%s\nstatus=%s\nattempt=%s\nwall=%s\ncand-bytes=0\nmin-memavail-kib=0\n' "$1" "$2" "$ATTEMPT" "${WALL:-1}" \
        >"$ATTEMPT_BASE.done.tmp"
    mv -f "$ATTEMPT_BASE.done.tmp" "$ATTEMPT_BASE.done"
    exit 0
}
plan="$RUN_DIR/ship/plan-$JOB-$ATTEMPT.sh"
[ -f "$plan" ] || plan="$RUN_DIR/ship/plan-$JOB.sh"
source "$plan"
finish 1 plan-fell-through
'''


class FakeCo(gf.Coordinator):
    """A Coordinator over one local host whose supervisor is FAKE_SUP; results land under a temp directory."""

    def __init__(self, tmp, name):
        """Build the run directories, the host and the ledger redirect."""
        self.tmp = os.path.join(tmp, name)
        run_id = f"test-{name}"
        local = os.path.join(self.tmp, "local")
        os.makedirs(local)
        a = argparse.Namespace(game=GAME, tier=TIER, all=False, all_tiers=False, hosts="",
                               no_local=False, pull="divergent", expect=None)
        super().__init__(a, run_id, local)
        root = os.path.join(self.tmp, "root")
        host = gf.Host("local", {"transport": "local", "fleet_root": root})
        hs = gf.HostState(host, {"root": root, "engine": "podman"}, 1, run_id)
        os.makedirs(os.path.join(hs.run_dir, "ship"))
        with open(os.path.join(hs.run_dir, "ship", "supervise.sh"), "w") as f:
            f.write(FAKE_SUP)
        from PIL import Image
        Image.new("RGB", (4, 4), (7, 0, 0)).save(os.path.join(hs.run_dir, "ship", "tile.png"))
        self.hosts = [hs]
        self.hs = hs
        self.skew = {"local": 0.0}
        self.skip_polls = 0
        self.kill_hook = None

    def plan(self, job, body, attempt=None):
        """Write a job's plan (bash sourced by FAKE_SUP), for one attempt or every attempt."""
        suffix = f"-{attempt}" if attempt else ""
        with open(os.path.join(self.hs.run_dir, "ship", f"plan-{job}{suffix}.sh"), "w") as f:
            f.write(body)

    def pull_target(self, game, tier):
        """Pull into the case's own directory."""
        return os.path.join(self.tmp, "pull")

    def stream_dir(self, job):
        """Stream pulls into the case's own directory."""
        return os.path.join(self.tmp, "stream", job.name)

    def pull_unit(self, job, hs):
        """Pull the fake report and its tiles, keep the run candidate; no manifest-hash refusal."""
        local = self.pull_target(job.game, job.tier)
        shutil.rmtree(local, ignore_errors=True)
        rdir = f"{job.result['unit_dir']}/cand"
        rc, err = gf.rsync_pull(hs.host, rdir, local, names=["verdict-report.txt"])
        if rc != 0:
            return [err]
        report = gf.parse_report(os.path.join(local, "verdict-report.txt"))
        self.note_census(job, report)
        self.pull_tiles(hs, rdir, local, report)
        self.keep_run_candidate(job, local)
        return []

    def poll(self, hs, pending, on_done):
        """Skip the first skip_polls polls (the host looks unreachable), then poll."""
        if self.skip_polls:
            self.skip_polls -= 1
            return
        super().poll(hs, pending, on_done)

    def kill_attempt(self, att):
        """Run the case's hook (lines written in the last poll interval), then kill."""
        if self.kill_hook:
            self.kill_hook(att)
        super().kill_attempt(att)

    def go(self, jobs, deadline_s=60):
        """Run the jobs to their results; return the wall time."""
        self.jobs = jobs
        t0 = time.monotonic()
        self.run_jobs(jobs, deadline=lambda _j, _h: deadline_s, on_done=self.unit_done)
        return time.monotonic() - t0

    def stop_later(self, delay, line):
        """Write the stop request after delay seconds."""
        def write():
            with open(self.stop_path + ".tmp", "w") as f:
                f.write(line + "\n")
            os.replace(self.stop_path + ".tmp", self.stop_path)
        threading.Timer(delay, write).start()

    def stream_log_lines(self):
        """The run's stream.log lines (comment lines excluded)."""
        if not os.path.exists(self.stream_log):
            return []
        return [ln for ln in open(self.stream_log).read().splitlines() if not ln.startswith("#")]


def full_unit(div, same, extra, ident, exceed, credits_rc=0, absent=()):
    """A plan streaming a full unit and writing the matching report and done marker (rc 1 when anything moved).

    Each moved tile (both members of an EXCEED pair) is written into the
    candidate directory before the stream lines, except the tiles in absent.
    """
    moved = bool(div or extra or exceed or credits_rc not in (0, 64))
    rc = 1 if moved else 0
    files = list(div) + list(extra) + [n for t in exceed for n in (t, t.replace("__ident__", "__canon__"))]
    lines = [f'cp "$RUN_DIR/ship/tile.png" "$UNIT_DIR/cand/{t}"' for t in files if t not in absent]
    lines += [f"emit SAME {t} 0123456789abcdef" for t in same]
    lines += [f"emit DIVERGE {t} '0123456789abcdef->fedcba9876543210'" for t in div]
    lines += [f"emit EXTRA {t} '->fedcba9876543210'" for t in extra]
    lines += [f"emit IDENT {t} pixels=0/0" for t in ident]
    lines += [f"emit EXCEED {t} pixels=9/0" for t in exceed]
    if credits_rc != 64:
        lines.append(f"emit CREDITS rc={credits_rc}")
    lines.append("emit END")
    rep = [f"verdict: {'FAIL' if rc else 'PASS'}", f"exit: {rc}", f"compared: {len(same) + len(div)}",
           f"credits-rc: {credits_rc}"]
    rep += [f"divergent: {t} pixels=1 maxch=128 reference=store" for t in div]
    rep += [f"extra: {t}" for t in extra]
    rep += [f"identity-exceed: {t} {t.replace('__ident__', '__canon__')} pixels=9 baseline=0" for t in exceed]
    body = "\n".join(lines) + "\nreport " + " ".join(f"'{r}'" for r in rep) + f"\nfinish {rc} verdict\n"
    return body


def ledger_rows():
    """Rows of the redirected ledger."""
    p = gf.ledger_path()
    return open(p).read().splitlines()[1:] if os.path.exists(p) else []


def case_framing(tmp):
    """POLL_SCRIPT framing over two polls with an incomplete line between them."""
    d = os.path.join(tmp, "framing")
    os.makedirs(d)
    base = os.path.join(d, "attempt-1")
    host = gf.Host("local", {"transport": "local"})
    with open(base + ".stream", "w") as f:
        f.write("SAME a-vk.png 0123 1.000\nDIVERGE b-vk.png 01->23 2.0")
    with open(base + ".done", "w") as f:
        f.write("rc=1\nstatus=verdict\nattempt=1\n")
    out = host.run_script(gf.POLL_SCRIPT, [base, 0], binary=True).stdout
    fr = gf.parse_poll(out)
    state, data = fr[base]
    ok = out.startswith(b"state ") and state.startswith("done rc=1") and data.endswith(b"2.0")
    co = FakeCo(tmp, "framing-co")
    job = gf.Job("unit", GAME, TIER)
    att = gf.Attempt(job, co.hs, 60)
    co.take_stream(att, data)
    n1 = len(att.lines)
    with open(base + ".stream", "a") as f:
        f.write("00\nEND 3.000\n")
    fr2 = gf.parse_poll(host.run_script(gf.POLL_SCRIPT, [base, att.stream_off], binary=True).stdout)
    co.take_stream(att, fr2[base][1])
    verbs = [r["verb"] for r in att.lines]
    ok = ok and n1 == 1 and verbs == ["SAME", "DIVERGE", "END"] and att.lines[1]["epoch"] == 2.0 \
        and att.stream_off == os.path.getsize(base + ".stream")
    try:
        gf.parse_poll(b"state x running\nstream x 9\nabc")
        torn = False
    except ValueError:
        torn = True
    check("framing", ok and torn, f"state before bytes; {n1} line after poll 1, {len(verbs)} after "
          f"poll 2 ({' '.join(verbs)}); offset {att.stream_off}; torn frame refused: {torn}")


def case_pool(tmp):
    """The coordinator's pb.digest_files process pool still works after the heat-diff module is imported."""
    from PIL import Image
    d = os.path.join(tmp, "pool")
    os.makedirs(d)
    paths = []
    for i in range(40):
        paths.append(os.path.join(d, f"t{i}-vk.png"))
        Image.new("RGB", (8, 8), (i, 0, 0)).save(paths[-1])
    co = FakeCo(tmp, "pool-co")
    co.load_diffmod()
    digs = co.pb.digest_files(paths)
    check("pool-coordinator", sum(1 for v in digs.values() if v) == 40,
          f"{len(digs)} tiles digested through the pool after the diff module's import")


def case_stream_off(tmp):
    """OPENPETE_FLEET_STREAM=0: units get STREAM=0 and no unit-end check."""
    os.environ["OPENPETE_FLEET_STREAM"] = "0"
    co = FakeCo(tmp, "stream-off")
    del os.environ["OPENPETE_FLEET_STREAM"]
    on = FakeCo(tmp, "stream-on")
    job = gf.Job("unit", GAME, TIER)
    env_off = [kv for kv in co.job_env(job, co.hs, gf.Attempt(job, co.hs, 60)) if kv.startswith("STREAM=")]
    env_on = [kv for kv in on.job_env(job, on.hs, gf.Attempt(job, on.hs, 60)) if kv.startswith("STREAM=")]
    canary = gf.Job("canary", GAME, TIER, pinned="local")
    env_can = [kv for kv in on.job_env(canary, on.hs, gf.Attempt(canary, on.hs, 60)) if kv.startswith("STREAM=")]
    check("stream-off", env_off == ["STREAM=0"] and env_on == ["STREAM=1"] and env_can == ["STREAM=0"],
          f"OPENPETE_FLEET_STREAM=0 unit {env_off}, default unit {env_on}, canary {env_can}")


def case_equality(tmp):
    """A full unit's stream equals its report; drops of a DIVERGE and a SAME line are refused."""
    ident = [t.replace("__canon__", "__ident__") for t in TILES[:2] if "__canon__" in t]
    co = FakeCo(tmp, "equal")
    co.plan(UNIT, full_unit(div=TILES[:1], same=TILES[1:6], extra=["zz__canon__1-vk.png"],
                            ident=ident[:1], exceed=ident[1:2]))
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    r = job.result
    check("equality", r["rc"] == 1 and not r.get("stream_mismatch") and r.get("stream") != "exempt",
          f"{len(job.atts[1].lines)} lines; SAME 5 + DIVERGE 1 = compared 6; rc {r['rc']} "
          f"(the report's), mismatch: {r.get('stream_mismatch') or 'none'}")
    for name, drop, same, div in (("drop-div", TILES[0], TILES[1:6], TILES[:1]),
                                  ("drop-same", TILES[1], TILES[1:6], [])):
        os.environ["OPENPETE_STREAM_DROP"] = drop
        co = FakeCo(tmp, name)
        del os.environ["OPENPETE_STREAM_DROP"]
        co.plan(UNIT, full_unit(div=div, same=same, extra=[], ident=[], exceed=[]))
        job = gf.Job("unit", GAME, TIER)
        co.go([job])
        r = job.result
        want_rc0 = 1 if div else 0
        check(name, r["rc"] == 1 and "stream mismatch" not in r["status"] and bool(r.get("stream_mismatch")),
              f"dropped {drop}; report rc {want_rc0} -> unit rc {r['rc']}; "
              f"mismatch: {r.get('stream_mismatch')}")


def case_crashed(tmp):
    """CRASHED stream lines are parsed and printed as they arrive, leave the unit-end check and verdict as they were, and are counted on the result line."""
    leg_pass, leg_fail = "rowx__canon", "rowy__ident"
    bad = [gf.parse_stream_line(s) for s in ("CRASHED rowx rc=134 1.0", "CRASHED rowx rc=134 retry=maybe 1.0",
                                             "CRASHED rowx 134 retry=pass 1.0")]
    rec = gf.parse_stream_line(f"CRASHED {leg_pass} rc=134 retry=pass 1.5")
    check("crashed-parse", bad == [None, None, None] and rec is not None and rec["tile"] == leg_pass
          and rec["detail"] == "rc=134 retry=pass" and rec["epoch"] == 1.5,
          f"well-formed {rec}; malformed forms {bad}")
    results = {}
    for name, extra in (("base", ""), ("crashed", "crashed")):
        co = FakeCo(tmp, f"crashed-{name}")
        body = full_unit(div=[], same=TILES[1:6], extra=[], ident=[], exceed=[])
        if extra:
            body = body.replace("emit END", f"emit CRASHED {leg_pass} rc=134 retry=pass\n"
                                f"emit CRASHED {leg_fail} rc=139 retry=fail\nemit END")
            body = body.replace("\nreport ", f"\nreport 'crashed-leg: {leg_pass} rc=134 /k/{leg_pass} retry=pass' "
                                f"'crashed-leg: {leg_fail} rc=139 /k/{leg_fail} retry=fail' "
                                f"'crashed-leg: {leg_fail}.retry rc=139 /k/{leg_fail}.retry' "
                                f"'crashed-leg: rowz__inter rc=1 /k/rowz__inter' ")
        co.plan(UNIT, body)
        job = gf.Job("unit", GAME, TIER)
        _res, out = captured(lambda: (co.go([job]), co.print_summary(0)))
        results[name] = (job, out)
    (bj, _bout), (cj, cout) = results["base"], results["crashed"]
    live = [ln for ln in cout.splitlines() if ": CRASHED " in ln and "result" not in ln]
    summary = [ln for ln in cout.splitlines() if ln.startswith(f"[fleet] result {UNIT}: ")]
    check("crashed-stream", len(live) == 2 and "malformed" not in cout
          and cj.result["rc"] == bj.result["rc"] and not cj.result.get("stream_mismatch")
          and cj.result.get("stream") == bj.result.get("stream"),
          f"{len(live)} live CRASHED line(s); rc {cj.result['rc']} (without: {bj.result['rc']}); "
          f"mismatch: {cj.result.get('stream_mismatch') or 'none'}")
    want = "crashed=3 (1 not retried, 1 retried fail, 1 retried pass)"
    check("crashed-result", bool(summary) and want in summary[0]
          and sum(1 for ln in summary if ": CRASHED " in ln) == 4
          and not any("crashed=" in ln for ln in _bout.splitlines()),
          f"result line {summary[0] if summary else 'absent'!r}; {len(summary) - 1} CRASHED result line(s)")


def case_extra(tmp):
    """EXTRA tiles are streamed, pulled without a heat diff and kept promotable; an absent moved tile is a stream mismatch."""
    import contextlib
    import io
    pb = gf.load_pb(TREE)
    new = ["zz-new__canon__1-vk.png", "zz-new__canon__2-vk.png"]
    co = FakeCo(tmp, "extra")
    co.plan(UNIT, full_unit(div=[], same=TILES[:4], extra=new, ident=[], exceed=[]))
    job = gf.Job("unit", GAME, TIER)
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        co.go([job])
    log = out.getvalue()
    sys.stdout.write(log)
    dest = co.stream_dir(job)
    pulled = [t for t in new if f"stream pulled {t}: new row 4x4, no reference, no heat diff" in log]
    kept = [t for t in new if os.path.isfile(os.path.join(dest, t))]
    heat = os.listdir(os.path.join(dest, "diff")) if os.path.isdir(os.path.join(dest, "diff")) else []
    sel = pb.promote_select(dest, True, [])
    r = job.result
    check("extra", r["rc"] == 1 and not r.get("stream_mismatch") and pulled == new and kept == new
          and not heat and sel == new,
          f"rc {r['rc']} (the report's), mismatch: {r.get('stream_mismatch') or 'none'}; streamed "
          f"pulls {len(pulled)} of {len(new)}, kept {len(kept)}, heat diffs {len(heat)}, "
          f"promote_select {sel}")

    co = FakeCo(tmp, "absent")
    co.plan(UNIT, full_unit(div=TILES[:1], same=TILES[1:4], extra=new, ident=[], exceed=[],
                            absent=new[1:]))
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    mm = job.result.get("stream_mismatch") or ""
    check("absent", job.result["rc"] == 1 and "EXTRA 1 tile(s) absent" in mm and new[1] in mm
          and "DIVERGE" not in mm and "stream mismatch" not in job.result["status"],
          f"rc {job.result['rc']}; mismatch: {mm or 'none'}")


def case_drain(tmp):
    """A refused unit (no run candidate kept) with more moved tiles than STREAM_K in its last poll still gets every one into the stream directory."""
    many = [f"zz-new__canon__{i}-vk.png" for i in range(gf.STREAM_K + 8)]
    co = FakeCo(tmp, "drain")
    co.plan(UNIT, full_unit(div=[], same=[], extra=many, ident=[], exceed=[]))
    def refused(job, hs):
        """Pull the report only and refuse, as the census refusal does, so no run candidate is kept."""
        gf.rsync_pull(hs.host, f"{job.result['unit_dir']}/cand", co.pull_target(GAME, TIER),
                      names=["verdict-report.txt"])
        return ["census compared=0: the filtered unit compared nothing"]
    co.pull_unit = refused
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    dest = co.stream_dir(job)
    kept = [t for t in many if os.path.isfile(os.path.join(dest, t))]
    r = job.result
    check("drain", r["rc"] == 1 and not r.get("stream_mismatch") and len(kept) == len(many),
          f"rc {r['rc']} (compared 0), kept {len(kept)} of {len(many)}, "
          f"mismatch: {r.get('stream_mismatch') or 'none'}")


def case_exempt(tmp):
    """An attempt without END and a SKIP unit are exempt from the unit-end check."""
    co = FakeCo(tmp, "exempt")
    co.plan(UNIT, f"emit SAME {TILES[0]} 0123456789abcdef\n"
                  "report 'verdict: FAIL' 'exit: 1' 'compared: 0'\nfinish 1 verdict\n")
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    co2 = FakeCo(tmp, "exempt-skip")
    co2.plan(UNIT, "finish 64 capture-skip\n")
    job2 = gf.Job("unit", GAME, TIER)
    co2.go([job2])
    check("exempt", job.result.get("stream") == "exempt" and not job.result.get("stream_mismatch")
          and job2.result.get("stream") == "exempt" and job2.result["rc"] == 64,
          f"no END: rc {job.result['rc']}, {job.result.get('stream')}; SKIP: rc {job2.result['rc']}, "
          f"{job2.result.get('stream')}")


def case_kill(tmp):
    """Lines written in the last poll interval are read before KILL_SCRIPT; the re-queued attempt passes the check."""
    a, b, c = TILES[0:2], TILES[2:4], TILES[4:6]
    co = FakeCo(tmp, "kill")
    go_file, written = os.path.join(co.tmp, "go"), os.path.join(co.tmp, "written")
    rows_ab = "\n".join(f"emit SAME {t} 0123456789abcdef" for t in a)
    rows_b = "\n".join(f"emit SAME {t} 0123456789abcdef" for t in b)
    co.plan(UNIT, f"{rows_ab}\nwhile [ ! -e {go_file} ]; do sleep 0.05; done\n{rows_b}\n"
                  f"touch {written}\nsleep 60\n", attempt=1)
    imported = "\n".join(f"emit_imported SAME {t} 0123456789abcdef" for t in a + b)
    co.plan(UNIT, imported + "\n" + "\n".join(f"emit SAME {t} 0123456789abcdef" for t in c)
            + "\nemit CREDITS rc=0\nemit END\n"
            + "report 'verdict: PASS' 'exit: 0' 'compared: 6' 'credits-rc: 0'\nfinish 0 verdict\n",
            attempt=2)

    def hook(att):
        if att.n == 1:
            open(go_file, "w").close()
            t0 = time.monotonic()
            while not os.path.exists(written) and time.monotonic() - t0 < 10:
                time.sleep(0.05)
    co.kill_hook = hook
    co.first_deadline = 1
    job = gf.Job("unit", GAME, TIER)
    co.go([job], deadline_s=60)
    late = [ln for ln in co.stream_log_lines() if ln.split()[2] == "1" and ln.split()[5] in b]
    ref = FakeCo(tmp, "kill-ref")
    ref.plan(UNIT, "\n".join(f"emit SAME {t} 0123456789abcdef" for t in a + b + c)
             + "\nemit CREDITS rc=0\nemit END\n"
             + "report 'verdict: PASS' 'exit: 0' 'compared: 6' 'credits-rc: 0'\nfinish 0 verdict\n")
    rjob = gf.Job("unit", GAME, TIER)
    ref.go([rjob])
    got = {gf.stream_key(x) for x in job.atts[2].lines}
    want = {gf.stream_key(x) for x in rjob.atts[1].lines}
    check("kill", len(late) == 2 and job.result["rc"] == 0 and job.result.get("attempt") == 2
          and got == want and not job.result.get("stream_mismatch"),
          f"{len(late)} of 2 lines written in the last interval read before the kill; attempt 2 "
          f"{len(got)} keys == uninterrupted {len(want)}: {got == want}; rc {job.result['rc']}")


def case_stop(tmp):
    """Stop of a running, a pending and a local unit, and a stop racing completion (both orders)."""
    n0 = len(ledger_rows())
    co = FakeCo(tmp, "stop-running")
    co.golden_fd = gf.try_golden_lock(co.hs.root)
    co.plan(UNIT, f"emit SAME {TILES[0]} 0123456789abcdef\nsleep 60\n")
    job = gf.Job("unit", GAME, TIER)
    co.stop_later(1.0, f"unit={UNIT}")
    wall = co.go([job])
    pid = open(job.atts[1].base + ".pid").read().split()[0]
    alive = os.path.exists(f"/proc/{pid}")
    marker = os.path.exists(os.path.join(co.stream_dir(job), "STOPPED"))
    lock = os.path.join(gf.expand_root(co.hs.root), "golden.lock")
    held = subprocess.run(["flock", "-n", lock, "true"]).returncode != 0
    co.teardown()
    free = subprocess.run(["flock", "-n", lock, "true"]).returncode == 0
    check("stop-running+local", job.result["status"] == gf.STOP_STATUS and job.result["rc"] == 1
          and job.stopped and not alive and marker and wall < 10 and len(ledger_rows()) == n0 and held and free,
          f"stopped after {wall:.1f} s, supervisor alive: {alive}, STOPPED marker: {marker}, "
          f"golden lock held before teardown: {held}, free after: {free}, ledger rows +{len(ledger_rows()) - n0}")

    co = FakeCo(tmp, "stop-pending")
    co.plan("first", "sleep 1\nreport 'verdict: PASS' 'exit: 0'\nfinish 0 verdict\n")
    first = gf.Job("unit", GAME, TIER, label="first")
    second = gf.Job("unit", GAME, TIER, label="second")
    co.stop_later(0.0, "unit=second")
    time.sleep(0.1)
    co.go([first, second])
    launched = os.path.exists(os.path.join(co.hs.run_dir, "u-second"))
    check("stop-pending", second.result["status"] == gf.STOP_STATUS and second.result["host"] == "-"
          and not launched and first.result["rc"] == 0 and len(ledger_rows()) == n0 + 1,
          f"second: {second.result['status']} on {second.result['host']}, launched: {launched}; "
          f"first rc {first.result['rc']}; ledger rows +{len(ledger_rows()) - n0} (first only)")

    co = FakeCo(tmp, "stop-race-done-first")
    co.plan(UNIT, "report 'verdict: PASS' 'exit: 0'\nfinish 0 verdict\n")
    job = gf.Job("unit", GAME, TIER)
    co.stop_later(0.15, f"unit={UNIT}")
    co.go([job])
    check("stop-race-done-read-first", job.result["rc"] == 0 and not job.stopped
          and len(ledger_rows()) == n0 + 2,
          f"done read before the request: rc {job.result['rc']} ({job.result['status']}), "
          f"ledger rows +{len(ledger_rows()) - n0} (its own row kept)")

    co = FakeCo(tmp, "stop-race-unread")
    co.skip_polls = 1
    co.plan(UNIT, "report 'verdict: PASS' 'exit: 0'\nfinish 0 verdict\n")
    job = gf.Job("unit", GAME, TIER)
    co.stop_later(0.1, f"unit={UNIT}")
    co.go([job])
    done = os.path.exists(job.atts[1].base + ".done")
    check("stop-race-done-unread", job.stopped and job.result["status"] == gf.STOP_STATUS and done
          and len(ledger_rows()) == n0 + 2,
          f"done marker on the host unread at the request: {done}; result {job.result['status']}; "
          f"ledger rows +{len(ledger_rows()) - n0} (no row for it)")
    co.jobs = [job]
    rc = co.run_rc()
    check("stop-run-verdict", rc == 1 and co.done_reason == gf.STOP_STATUS,
          f"run rc {rc}, reason={co.done_reason}")


def case_stop_cli(tmp):
    """`golden-fleet.py stop`: request lines, accumulation, refusal once done, unknown run."""
    root = os.path.join(tmp, "cli-root")
    run_dir = os.path.join(root, "runs", "r1")
    os.makedirs(run_dir)
    gf.local_fleet_root = lambda: root
    rc1 = gf.main(["stop", "r1", "--unit", UNIT])
    rc2 = gf.main(["stop", "r1"])
    text = open(os.path.join(run_dir, "stop-request")).read()
    open(os.path.join(run_dir, "done"), "w").write("rc=0\n")
    rc3 = gf.main(["stop", "r1"])
    rc4 = gf.main(["stop", "no-such-run"])
    check("stop-cli", (rc1, rc2, rc3, rc4) == (0, 0, 1, 2) and text == f"all\nunit={UNIT}\n"
          and gf.read_stop_request(os.path.join(run_dir, "stop-request")) == {"all", UNIT},
          f"exits {rc1}, {rc2}, {rc3} (done), {rc4} (no run); request {text.split()}")


class SyncCo(FakeCo):
    """A FakeCo whose preflight is skipped and whose sync is a host script sleeping in a child, its pids recorded."""

    def prepare_hosts(self, src_bytes):
        """Select the local host without preflight, clean or image ship."""
        self.sel = [self.hs.host]

    def build_and_sync(self):
        """Run a 60 s sync stand-in (a script and its sleeping child) on the host."""
        self.mkdirs(self.hs)
        self.script(self.hs, 'echo $$ >"$1/sync.pid"; sleep 60 & echo $! >"$1/child.pid"; wait',
                    [self.tmp], timeout=300)
        raise AssertionError("the sync stand-in returned")


def case_stop_sync(tmp):
    """Stop during the sync: the sync's process group dies, every unit is stopped, the done marker says so."""
    co = SyncCo(tmp, "stop-sync")
    co.stop_later(2.0, "all")
    t_req = time.monotonic() + 2.0
    prev = {s: signal.getsignal(s) for s in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)}
    try:
        rc, _co = gf.drive(co.run_id, co.local_run_dir, lambda: co)
    finally:
        for s, h in prev.items():
            signal.signal(s, h)
    lag = time.monotonic() - t_req
    pids = [open(os.path.join(co.tmp, n)).read().split()[0] for n in ("sync.pid", "child.pid")]
    alive = [p for p in pids if os.path.exists(f"/proc/{p}")]
    done = gf.parse_kv(open(os.path.join(co.local_run_dir, "done")).read())
    job = co.jobs[0] if co.jobs else None
    marker = job is not None and os.path.exists(os.path.join(co.stream_dir(job), "STOPPED"))
    bound = gf.STOP_POLL_S + 2.0
    check("stop-sync", rc == 1 and done.get("reason") == gf.STOP_STATUS and job is not None
          and job.stopped and job.result["host"] == "-" and marker and not alive and lag < bound,
          f"run ended {lag:.1f} s after the request (bound {bound:.1f} s), rc {rc}, done "
          f"reason={done.get('reason')}, units stopped {sum(j.stopped for j in co.jobs)}/"
          f"{len(co.jobs)}, STOPPED marker: {marker}, sync processes alive: {len(alive)} of 2")

    co = SyncCo(tmp, "stop-sync-scope")
    got = []
    for line in ("unit=other", f"unit={UNIT}", "all"):
        with open(co.stop_path, "w") as f:
            f.write(line + "\n")
        got.append(co.stop_covers_run())
    check("stop-sync-scope", got == [False, True, True],
          f"covers the run for unit=other, unit={UNIT}, all: {got}")


def case_reap(tmp):
    """REAP_SCRIPT ends a process carrying the run tag and spares an untagged one; remote argv and rsync carry the tag."""
    tag = f"test-reap-{os.getpid()}"
    tagged = subprocess.Popen(["sleep", "60"], env=dict(os.environ, **{gf.RUN_TAG_VAR: tag}))
    other = subprocess.Popen(["sleep", "60"])
    host = gf.Host("local", {"transport": "local"})
    out = host.run_script(gf.REAP_SCRIPT, [gf.RUN_TAG_VAR, tag], timeout=60, tag=False).stdout.strip()
    tagged_rc, other_alive = tagged.wait(5), other.poll() is None
    other.kill()
    other.wait()
    remote = gf.Host("r", {"transport": "ssh", "ssh": "u@h"})
    with gf.run_tag(tag):
        argv = remote.argv("bash -s --")
        rsync = gf.rsync_base(remote)
    plain = remote.argv("bash -s --")
    check("reap", tagged_rc is not None and other_alive and out == "reaped 1 process(es)"
          and argv[-1] == f"{gf.RUN_TAG_VAR}={tag} bash -s --" and plain[-1] == "bash -s --"
          and f"--rsync-path={gf.RUN_TAG_VAR}={tag} rsync" in rsync,
          f"reaper: {out!r}; tagged exit {tagged_rc}, untagged alive: {other_alive}; "
          f"remote command {argv[-1]!r}; rsync {rsync[-1]!r}")


FAKE_SSH = r'''#!/usr/bin/env bash
# A stand-in ssh: the first $(cat fails) calls (every call for `always`)
# fail as a connect timeout does; the rest run the remote command here.
d="$FAKE_SSH_DIR"
echo call >>"$d/calls"
n=$(wc -l <"$d/calls"); fails=$(cat "$d/fails")
if [ "$fails" = always ] || [ "$n" -le "$fails" ]; then
    echo "ssh: connect to host 192.0.2.1 port 22: Connection timed out" >&2
    exit 255
fi
while [ $# -gt 0 ]; do
    case "$1" in -o|-l|-p) shift 2 ;; -*) shift ;; *) break ;; esac
done
shift
exec bash -c "$*"
'''


class FakeSsh:
    """A FAKE_SSH on PATH whose connect failures a case sets; calls counts its invocations."""

    def __init__(self, tmp):
        """Write the stand-in into tmp/fake-ssh/bin and put it first on PATH."""
        self.dir = os.path.join(tmp, "fake-ssh")
        os.makedirs(os.path.join(self.dir, "bin"))
        path = os.path.join(self.dir, "bin", "ssh")
        with open(path, "w") as f:
            f.write(FAKE_SSH)
        os.chmod(path, 0o755)
        self.env = {"PATH": os.environ["PATH"], "FAKE_SSH_DIR": os.environ.get("FAKE_SSH_DIR")}
        os.environ["PATH"] = os.path.join(self.dir, "bin") + os.pathsep + os.environ["PATH"]
        os.environ["FAKE_SSH_DIR"] = self.dir

    def arm(self, fails):
        """Fail the next `fails` calls (an int, or 'always') and reset the call count."""
        with open(os.path.join(self.dir, "fails"), "w") as f:
            f.write(f"{fails}\n")
        open(os.path.join(self.dir, "calls"), "w").close()

    def calls(self):
        """Return the number of calls since arm."""
        with open(os.path.join(self.dir, "calls")) as f:
            return len(f.read().splitlines())

    def restore(self):
        """Restore PATH and FAKE_SSH_DIR."""
        for k, v in self.env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def captured(fn):
    """Run fn with stdout captured; return (its result or the FleetError it raised, the output)."""
    import contextlib
    import io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        try:
            res = fn()
        except gf.FleetError as e:
            res = e
    return res, buf.getvalue()


def case_transport(tmp):
    """A connect failure of ssh or rsync is retried and logged; a remote command's own exit is not; a persistent one fails naming the host, and a recheck that cannot connect is not a torn sync."""
    fake = FakeSsh(tmp)
    backoff, gf.TRANSPORT_BACKOFF_S = gf.TRANSPORT_BACKOFF_S, (0, 0, 0)
    host = gf.Host("flaky", {"transport": "ssh", "ssh": "u@flaky"})
    src, dst = os.path.join(tmp, "tr-src"), os.path.join(tmp, "tr-dst")
    os.makedirs(src)
    os.makedirs(dst)
    for i in range(3):
        with open(os.path.join(src, f"f{i}"), "w") as f:
            f.write(f"{i}\n")
    lst = os.path.join(tmp, "tr.lst")
    gf.write_list(lst, ["f0", "f1", "f2"])
    hs = argparse.Namespace(host=host, name=host.name, run_dir=dst)
    try:
        # Transient: two connect failures, then the script runs.
        fake.arm(2)
        cp, out = captured(lambda: host.run_script("echo hi", check=True, what="probe"))
        retries = [ln for ln in out.splitlines() if ln.startswith("[fleet] flaky: probe: ssh: connect to host")]
        check("transport-transient-ssh", not isinstance(cp, Exception) and cp.stdout.strip() == "hi"
              and fake.calls() == 3 and len(retries) == 2 and "retry 2/3" in retries[1],
              f"{fake.calls()} calls, {len(retries)} retry lines, "
              f"result {cp if isinstance(cp, Exception) else cp.stdout.strip()!r:.80}")
        # Transient rsync: the push, then a recheck whose first try cannot connect.
        fake.arm(1)
        res, out = captured(lambda: gf.rsync_push(host, src, lst, os.path.join(dst, "tree")))
        push_calls = fake.calls()
        fake.arm(1)
        rc, out2 = captured(lambda: gf.Coordinator.recheck(None, hs, src, lst, "source"))
        check("transport-transient-rsync", not isinstance(res, Exception) and push_calls == 2
              and rc is None and fake.calls() == 2 and "recheck source: 0 changes" in out2
              and "retry 1/3" in out2 and "torn" not in out2,
              f"push {push_calls} calls ({res!r:.60}); recheck {fake.calls()} calls, result {rc!r:.80}; "
              f"log {out2.strip().splitlines()[-1] if out2.strip() else ''!r}")
        # A remote command's own exit (255 included) runs once.
        fake.arm(0)
        cp, _ = captured(lambda: host.run_script("echo boom >&2; exit 255"))
        n255 = fake.calls()
        fake.arm(0)
        cp3, _ = captured(lambda: host.run_script(
            "echo 'ssh: connect to host x port 22: Connection timed out' >&2; exit 3"))
        check("transport-remote-exit", cp.returncode == 255 and n255 == 1 and cp3.returncode == 3
              and fake.calls() == 1, f"exit 255: {n255} call(s); exit 3 with a connect line: {fake.calls()} call(s)")
        # A real content change is still a torn sync.
        with open(os.path.join(src, "f1"), "w") as f:
            f.write("changed\n")
        fake.arm(0)
        err, _ = captured(lambda: gf.Coordinator.recheck(None, hs, src, lst, "source"))
        check("transport-torn", isinstance(err, gf.FleetError) and str(err).startswith("torn sync: flaky")
              and fake.calls() == 1, f"{fake.calls()} call(s): {str(err)[:80]!r}")
        # Persistent: every try fails; script, push and recheck fail naming the host.
        tries = 1 + len(gf.TRANSPORT_BACKOFF_S)
        got = []
        for name, fn in (("script", lambda: host.run_script("echo hi", check=True)),
                         ("push", lambda: gf.rsync_push(host, src, lst, os.path.join(dst, "tree"))),
                         ("recheck", lambda: gf.Coordinator.recheck(None, hs, src, lst, "source"))):
            fake.arm("always")
            err, out = captured(fn)
            got.append((name, fake.calls(), isinstance(err, gf.FleetError) and str(err).startswith("flaky: ")
                        and "torn" not in str(err), "giving up" in out))
        check("transport-persistent", all(n == tries and ok and gave for _nm, n, ok, gave in got),
              "; ".join(f"{nm}: {n} calls, error names the host {ok}, logged {gave}" for nm, n, ok, gave in got))
        # poll's one try.
        fake.arm(1)
        captured(lambda: host.run_script("true", retry=False))
        check("transport-no-retry", fake.calls() == 1, f"retry=False: {fake.calls()} call(s)")
    finally:
        gf.TRANSPORT_BACKOFF_S = backoff
        fake.restore()


def case_clean(tmp):
    """clean removes build/fleet-stream/<run-id> (STOPPED marker included) once the run directory is gone."""
    tree, root = os.path.join(tmp, "clean-tree"), os.path.join(tmp, "clean-root")
    for run_id in ("kept", "gone"):
        d = os.path.join(tree, gf.STREAM_DIR, run_id, UNIT)
        os.makedirs(d)
        open(os.path.join(d, "STOPPED"), "w").close()
    os.makedirs(os.path.join(root, "runs", "kept"))
    gf.clean_stream_dirs(tree, root)
    left = sorted(os.listdir(os.path.join(tree, gf.STREAM_DIR)))
    check("clean", left == ["kept"], f"left {left} (the run directory of `kept` exists)")


def rec(verb, tile, imported=False):
    """A stream record."""
    return {"verb": verb, "tile": tile, "detail": "x", "epoch": 1.0, "imported": imported}


def case_expect(tmp):
    """Expected-moves tags, statuses, counts and refusals."""
    poked, quiet = TILES[0], TILES[1]
    blessed = set(TILES)
    lines = [rec("DIVERGE", poked), rec("SAME", quiet), rec("END", "END")]
    e_poked = gf.Expectation(UNIT, poked, "row poke", 1)
    e_quiet = gf.Expectation(UNIT, quiet, "predicted, not moved", 2)
    c1, _ = gf.expect_counts([e_poked], {UNIT: (True, lines, blessed)})
    c2, _ = gf.expect_counts([], {UNIT: (True, lines, blessed)})
    c3, r3 = gf.expect_counts([e_quiet], {UNIT: (True, [rec("SAME", quiet)], blessed)})
    fmt = (lambda c: f"{c['EXPECTED']}/{c['UNEXPECTED']}/{c['EXPECTED-NOT-MOVED']}")
    check("expect-expected", fmt(c1) == "1/0/0", f"counts {fmt(c1)}")
    check("expect-unexpected", fmt(c2) == "0/1/0", f"counts {fmt(c2)}")
    check("expect-not-moved", fmt(c3) == "0/0/1" and r3[0][1] == "EXPECTED-NOT-MOVED",
          f"counts {fmt(c3)}, status {r3[0][1]}")
    _c, r4 = gf.expect_counts([e_quiet], {UNIT: (False, [], blessed)})
    row = TILES[0].split("__")[0]
    e_row = gf.Expectation(UNIT, row + "__", "timed-out row", 3)
    other = [rec("SAME", t) for t in TILES if not t.startswith(row + "__")][:20]
    _c, r5 = gf.expect_counts([e_row], {UNIT: (True, other, blessed)})
    check("expect-not-reached", r4[0][1] == "NOT-REACHED" and r5[0][1] == "NOT-REACHED",
          f"stopped unit: {r4[0][1]}; timed-out row {row} ({len(r5[0][2])} tiles, none streamed): {r5[0][1]}")
    pb = gf.load_pb(TREE)
    errs = gf.expect_refusals(TREE, pb, [(GAME, TIER)], [gf.Expectation(UNIT, "no-such-row-xyzzy", "typo", 1),
                                                        gf.Expectation("spyro-9-presentation", "a", "unit", 2)])
    path = os.path.join(tmp, "bad-expect.txt")
    with open(path, "w") as f:
        f.write(f"{UNIT} no-such-row-xyzzy typo\n")
    cli = subprocess.run([sys.executable, os.path.join(TREE, "scripts", "golden-fleet.py"), "run",
                          "--game", GAME, "--expect", path], capture_output=True, text=True)
    with open(path, "w") as f:
        f.write(f"spyro-1-dragons {poked} unknown unit\n")
    cli2 = subprocess.run([sys.executable, os.path.join(TREE, "scripts", "golden-fleet.py"), "run",
                           "--game", GAME, "--expect", path], capture_output=True, text=True)
    check("expect-refusals", len(errs) == 2 and cli.returncode == 2 and cli2.returncode == 2
          and "detached" not in cli.stdout + cli2.stdout,
          f"{len(errs)} refusals: {'; '.join(errs)}; `run --expect` exits {cli.returncode} (typo), "
          f"{cli2.returncode} (unknown unit), nothing detached")
    resumed = [rec("DIVERGE", poked, imported=True), rec("SAME", quiet), rec("END", "END")]
    c6, _ = gf.expect_counts([e_poked, e_quiet], {UNIT: (True, resumed, blessed)})
    c7, _ = gf.expect_counts([e_poked, e_quiet], {UNIT: (True, lines, blessed)})
    check("expect-resumed", c6 == c7, f"resumed {fmt(c6)} == uninterrupted {fmt(c7)}")
    mpath = os.path.join(tmp, "games", "spyro-1", "corpus", "presentation-corpus.manifest")
    os.makedirs(os.path.dirname(mpath), exist_ok=True)
    with open(mpath, "w") as f:
        f.write("# x|1|1||1|--lod extended\nrowx|1800|100||1900|--freecam-pose 1,2,3,4,5\n"
                "rowx@ext|1800|100||1900|--freecam-pose 1,2,3,4,5 --lod extended docs:a/b\n")
    e_lod = gf.bind_launch_lod(tmp, [gf.Expectation("spyro-1-presentation", "__lodext__", "lod", 1)])[0]
    got = {t: e_lod.matches("spyro-1-presentation", t) for t in (
        "rowx@ext__canon__1800-vk.png", "rowx@ext~r02__inter__1850-vk.png", "rowx@ext__lodext__canon__1800-vk.png",
        "rowx__canon__1800-vk.png", "rowx__lodext__canon__1800-vk.png")}
    check("expect-launch-lod", sorted(e_lod.ext_rows) == ["rowx@ext"]
          and list(got.values()) == [True, True, True, False, True], f"ext rows {sorted(e_lod.ext_rows)}; {got}")

    co = FakeCo(tmp, "expect-log")
    co.expect = [e_poked]
    co.plan(UNIT, full_unit(div=[poked, TILES[2]], same=TILES[3:6], extra=[], ident=[], exceed=[]))
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    co.print_summary(1)
    tags = [ln.split()[-1] for ln in co.stream_log_lines()]
    lcount = {t: tags.count(t) for t in ("EXPECTED", "UNEXPECTED")}
    units = {UNIT: (True, job.atts[1].lines, blessed)}
    c8, _ = gf.expect_counts(co.expect, units)
    check("expect-log", lcount["EXPECTED"] == c8["EXPECTED"] == 1
          and lcount["UNEXPECTED"] == c8["UNEXPECTED"] == 1,
          f"stream.log tags EXPECTED {lcount['EXPECTED']}, UNEXPECTED {lcount['UNEXPECTED']}; "
          f"summary {fmt(c8)}")


class MoveCo(FakeCo):
    """A FakeCo with a second local host `b` (its own fleet root) already in the run; add_host admits it without a preflight."""

    def __init__(self, tmp, name):
        """Add host b beside the FakeCo host."""
        super().__init__(tmp, name)
        root = os.path.join(self.tmp, "root-b")
        host = gf.Host("b", {"transport": "local", "fleet_root": root})
        hs = gf.HostState(host, {"root": root, "engine": "podman"}, 1, self.run_id)
        os.makedirs(os.path.join(hs.run_dir, "ship"))
        shutil.copyfile(os.path.join(self.hs.run_dir, "ship", "supervise.sh"),
                        os.path.join(hs.run_dir, "ship", "supervise.sh"))
        self.hosts.append(hs)
        self.hb = hs
        self.skew["b"] = 0.0

    def add_host(self, name):
        """Admit a host of the run as is; refuse any other."""
        hs = self.host_state(name)
        if hs is None:
            raise gf.FleetError(f"{name}: unreachable: test host")
        return hs

    def plan_on(self, hs, job, body, attempt=None):
        """Write a job's plan into one host's ship directory."""
        suffix = f"-{attempt}" if attempt else ""
        with open(os.path.join(hs.run_dir, "ship", f"plan-{job}{suffix}.sh"), "w") as f:
            f.write(body)

    def request_move_later(self, delay, lines):
        """Write move-request lines after delay seconds."""
        def write():
            with open(self.move_path + ".tmp", "w") as f:
                f.write("".join(ln + "\n" for ln in lines))
            os.replace(self.move_path + ".tmp", self.move_path)
        threading.Timer(delay, write).start()


def case_move(tmp):
    """`move`: refusals, an accepted move carrying stamped rows to another host, a pinned re-queue outside the attempt budget, one ledger row over both attempts."""
    n0 = len(ledger_rows())
    a, b = TILES[0:2], TILES[2:4]
    co = MoveCo(tmp, "move")
    co.hosts = [co.hs, co.hb]
    stamp = "".join(f"tile: 00 {t}\\n" for t in a)
    rows_a = "\n".join(f"emit SAME {t} 0123456789abcdef" for t in a)
    co.plan_on(co.hs, UNIT, 'mkdir -p "$UNIT_DIR/cand/resume"\n'
               + "".join(f'echo png >"$UNIT_DIR/cand/{t}"\n' for t in a)
               + f'printf "run: r\\n{stamp}" >"$UNIT_DIR/cand/resume/bk2.rowa.stamp"\n'
               + 'echo partial >"$UNIT_DIR/cand/unstamped-vk.png"\n'
               + f"{rows_a}\nsleep 60\n", attempt=1)
    check_rows = "".join(f'[ -f "$UNIT_DIR/cand/{t}" ] || finish 1 row-missing\n' for t in a)
    co.plan_on(co.hb, UNIT, check_rows
               + '[ -f "$UNIT_DIR/cand/resume/bk2.rowa.stamp" ] || finish 1 stamp-missing\n'
               + '[ ! -e "$UNIT_DIR/cand/unstamped-vk.png" ] || finish 1 unstamped-carried\n'
               + "\n".join(f"emit_imported SAME {t} 0123456789abcdef" for t in a) + "\n"
               + "\n".join(f"emit SAME {t} 0123456789abcdef" for t in b)
               + "\nemit CREDITS rc=0\nemit END\n"
               + "report 'verdict: PASS' 'exit: 0' 'compared: 4' 'credits-rc: 0' 'imported-rows: 1'\n"
               + "finish 0 verdict\n", attempt=2)
    co.request_move_later(1.0, ["id=r1 unit=spyro-9-presentation to=b",
                                f"id=r2 unit={UNIT} to=nowhere",
                                f"id=r3 unit={UNIT} to=local",
                                f"id=r4 unit={UNIT} to=b"])
    job = gf.Job("unit", GAME, TIER)
    co.go([job], deadline_s=60)
    log = open(os.path.join(co.local_run_dir, "moves.log")).read().splitlines()
    verdicts = [ln.split()[1] for ln in log]
    keys = {gf.stream_key(x) for x in job.atts[2].lines} if 2 in job.atts else set()
    want = {gf.stream_key(gf.parse_stream_line(f"SAME {t} 0123456789abcdef 1.0"))
            for t in a + b}
    check("move", verdicts == ["refused", "refused", "refused", "accepted"]
          and job.result["rc"] == 0 and job.result["host"] == "b" and job.result["attempt"] == 2
          and job.moves == 1 and job.moved == [("local", "b")] and job.pinned == "b"
          and keys >= want and not job.result.get("stream_mismatch") and len(ledger_rows()) == n0 + 1
          and ledger_rows()[-1].split("\t")[gf.LEDGER_COLS.index("host")] == "local>b",
          f"moves.log {verdicts}; result rc {job.result['rc']} on {job.result['host']} attempt "
          f"{job.result.get('attempt')}; moves {job.moves} {job.moved}; stream keys of attempt 2 "
          f"cover the unmoved tiles: {keys >= want}; ledger rows +{len(ledger_rows()) - n0} "
          f"({ledger_rows()[-1].split(chr(9))[gf.LEDGER_COLS.index('host')] if ledger_rows() else '-'}); "
          f"accepted line: {log[-1] if log else '-'}")
    co.request_move_later(0.0, [f"id=r5 unit={UNIT} to=local"])
    time.sleep(0.2)
    co.apply_moves([], None)
    last = open(os.path.join(co.local_run_dir, "moves.log")).read().splitlines()[-1]
    check("move-finished", "refused" in last and "has finished" in last, last)
    co.teardown()


def case_move_cli(tmp):
    """`golden-fleet.py move`: refused once done, unknown run; the request line and --no-wait."""
    root = os.path.join(tmp, "move-cli-root")
    run = os.path.join(root, "runs", "r-cli")
    os.makedirs(run)
    hosts = os.path.join(tmp, "move-cli-hosts.toml")
    with open(hosts, "w") as f:
        f.write(f'[hosts.local]\ntransport = "local"\nfleet_root = "{root}"\n')
    env = dict(os.environ, OPENPETE_FLEET_HOSTS=hosts)
    cmd = [sys.executable, os.path.join(TREE, "scripts", "golden-fleet.py"), "move"]
    unknown_host = subprocess.run(cmd + ["r-cli", "--unit", UNIT, "--to", "zz", "--no-wait"],
                                  env=env, capture_output=True, text=True)
    ok = subprocess.run(cmd + ["r-cli", "--unit", UNIT, "--to", "local", "--no-wait"], env=env,
                        capture_output=True, text=True)
    req = gf.read_move_requests(os.path.join(run, "move-request"))
    open(os.path.join(run, "done"), "w").write("rc=0\n")
    done = subprocess.run(cmd + ["r-cli", "--unit", UNIT, "--to", "local"], env=env,
                          capture_output=True, text=True)
    norun = subprocess.run(cmd + ["r-none", "--unit", UNIT, "--to", "local"], env=env,
                           capture_output=True, text=True)
    check("move-cli", unknown_host.returncode == 2 and ok.returncode == 0 and len(req) == 1
          and req[0]["unit"] == UNIT and req[0]["to"] == "local" and done.returncode == 1
          and "resume" in done.stderr and norun.returncode == 2,
          f"unknown host {unknown_host.returncode}, request {req}, done {done.returncode}, "
          f"no run {norun.returncode}")


def case_keep_and_resume_inputs(tmp):
    """A stopped unit's stamped rows and ship are kept at teardown; resolve_saved reads them; resume refuses a finished unit and an active run."""
    co = FakeCo(tmp, "keep")
    co.golden_fd = gf.try_golden_lock(co.hs.root)
    co.plan(UNIT, 'mkdir -p "$UNIT_DIR/cand/resume"\n'
                  f'echo png >"$UNIT_DIR/cand/{TILES[0]}"\n'
                  f'printf "run: r\\ntile: 00 {TILES[0]}\\n" >"$UNIT_DIR/cand/resume/bk2.k.stamp"\n'
                  f"emit SAME {TILES[0]} 0123456789abcdef\nsleep 60\n")
    os.makedirs(co.ship_dir, exist_ok=True)
    with open(os.path.join(co.ship_dir, "fields-spyro-2.txt"), "w") as f:
        f.write("head: x\n")
    co.stop_later(0.5, f"unit={UNIT}")
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    co.print_summary(co.run_rc())
    co.teardown()
    kept = os.path.join(co.local_run_dir, "kept")
    files = gf.stamp_files(os.path.join(kept, f"u-{UNIT}", "cand"))
    saved = gf.resolve_saved(kept, UNIT, co.local_run_dir)
    left = sorted(os.listdir(co.local_run_dir))
    check("keep-stopped", gf.row_counts(files) == (1, 1) and saved["fields"].endswith("fields-spyro-2.txt")
          and "kept" in left and "ship" not in left,
          f"kept rows {gf.row_counts(files)}, fields {saved['fields']}, run dir {left}")
    # The summary line names the stopped unit, so `resume` reads its last result.
    with open(os.path.join(co.local_run_dir, "coordinator.log"), "w") as f:
        f.write(f"[fleet] result {UNIT}: FAIL on local (rc 1, attempt 1, - s; stopped by user)\n"
                "[fleet] result spyro-3-presentation: PASS on b (rc 0, attempt 1, 9 s; verdict; census x)\n")
    res = gf.unit_results(co.local_run_dir)
    check("resume-results", res[UNIT][3] == gf.STOP_STATUS and res["spyro-3-presentation"][2] == 0,
          f"{res}")


def case_keep_exhausted(tmp):
    """A unit past its deadline on every attempt ends attempts exhausted; its last attempt's stamped rows are kept for `resume`."""
    co = FakeCo(tmp, "keep-exhausted")
    co.golden_fd = gf.try_golden_lock(co.hs.root)
    co.plan(UNIT, 'mkdir -p "$UNIT_DIR/cand/resume"\n'
                  f'echo png >"$UNIT_DIR/cand/{TILES[0]}"\n'
                  f'printf "run: r\\ntile: 00 {TILES[0]}\\n" >"$UNIT_DIR/cand/resume/bk2.k.stamp"\n'
                  'echo partial >"$UNIT_DIR/cand/unstamped-vk.png"\n'
                  f"emit SAME {TILES[0]} 0123456789abcdef\nsleep 60\n")
    os.makedirs(co.ship_dir, exist_ok=True)
    with open(os.path.join(co.ship_dir, "fields-spyro-2.txt"), "w") as f:
        f.write("head: x\n")
    job = gf.Job("unit", GAME, TIER)
    co.go([job], deadline_s=1)
    co.teardown()
    kept = os.path.join(co.local_run_dir, "kept", f"u-{UNIT}")
    files = gf.stamp_files(os.path.join(kept, "cand")) if os.path.isdir(kept) else []
    host = open(os.path.join(kept, "host")).read().strip() if os.path.exists(os.path.join(kept, "host")) else "-"
    unstamped = os.path.exists(os.path.join(kept, "cand", "unstamped-vk.png"))
    saved = gf.resolve_saved(os.path.join(co.local_run_dir, "kept"), UNIT, co.local_run_dir) if files else {}
    check("keep-exhausted", job.result["status"] == "attempts exhausted" and not job.stopped
          and job.result.get("attempt") == gf.MAX_ATTEMPTS and gf.row_counts(files) == (1, 1)
          and host == "local" and not unstamped and saved.get("host") == "local",
          f"result {job.result['status']} (attempt {job.result.get('attempt')}), kept rows "
          f"{gf.row_counts(files)}, host {host}, unstamped tile kept: {unstamped}")


def case_ledger(tmp):
    """Only a whole unit writes a ledger row; a filtered unit and an attempt that imported rows keep the deadline at the whole unit's time."""
    done = "report 'verdict: PASS' 'exit: 0' 'imported-rows: {n}'\nWALL={w}\nfinish 0 verdict\n"
    slow = gf.HostState(gf.Host("slow", {"transport": "local", "speed": 0.25}), {"root": "/nonexistent"}, 1, "x")
    n0 = len(ledger_rows())
    co = FakeCo(tmp, "ledger-whole")
    co.plan(UNIT, done.format(n=0, w=900))
    whole = gf.Job("unit", GAME, TIER)
    co.go([whole])
    n1 = len(ledger_rows())
    norm = float(ledger_rows()[-1].split("\t")[gf.LEDGER_COLS.index("norm_s")])
    co.ledger = gf.read_ledger()
    before = co.deadline(whole, slow)
    co = FakeCo(tmp, "ledger-filtered")
    co.a.filter, co.a.freecam_filter = "summer_forest", None
    co.plan(UNIT, done.format(n=0, w=200))
    filt = gf.Job("unit", GAME, TIER)
    co.go([filt])
    n2 = len(ledger_rows())
    co = FakeCo(tmp, "ledger-imported")
    co.plan(UNIT, done.format(n=3, w=300))
    imp = gf.Job("unit", GAME, TIER)
    co.go([imp])
    n3 = len(ledger_rows())
    co.ledger = gf.read_ledger()
    after = co.deadline(imp, slow)
    check("ledger", whole.result["rc"] == filt.result["rc"] == imp.result["rc"] == 0
          and n1 == n0 + 1 and n2 == n1 and n3 == n1 and imp.result.get("imported_rows") == 3
          and before == after == norm / 0.25 * 2,
          f"rows +{n1 - n0} whole, +{n2 - n1} filtered, +{n3 - n2} imported "
          f"(imported_rows {imp.result.get('imported_rows')}); deadline at speed 0.25 {before:.0f} s "
          f"before, {after:.0f} s after (norm_s {norm:.0f} / 0.25 x 2)")


def case_repeat(tmp):
    """run --repeat: the unit's FILTERS carry --repeat=N, copy tiles stream SAME and equal the report, the result names each row's divergent count of N x legs, and the report refusals (engagement, repeat count) hold."""
    key, n = "summer_forest-12609f", 3
    orig = sorted(t for t in TILES if t.startswith(key + "__"))
    copies = [t.replace(key, f"{key}~r{k:02d}", 1) for k in range(1, n + 1) for t in orig]
    idents = [t.replace("__canon__", "__ident__") for t in copies if "__canon__" in t]
    legs = len(copies) + len(idents)
    co = FakeCo(tmp, "repeat")
    co.a.filter, co.a.freecam_filter, co.a.repeat = key, key, n
    hs, att = co.hs, gf.Attempt(gf.Job("unit", GAME, TIER), co.hs, 1)
    env_unit = [e for e in co.job_env(att.job, hs, att) if e.startswith("FILTERS=")]
    canary = gf.Job("canary", GAME, TIER, pinned="local")
    env_canary = [e for e in co.job_env(canary, hs, gf.Attempt(canary, hs, 1)) if e.startswith("FILTERS=")]
    row = f"repeat-row: {key}  divergent=0 of={legs} compared={legs} copies={n}"
    body = full_unit(div=[], same=copies, extra=[], ident=idents, exceed=[])
    body = body.replace("\nreport ", f"\nreport 'repeat: {n}' '{row}' 'repeat-divergent: 0' "
                        f"'repeat-legs: {legs}' 'repeat-compared: {legs}' ", 1)
    co.plan(UNIT, body)
    job = gf.Job("unit", GAME, TIER)
    co.go([job])
    r = job.result
    check("repeat-unit", r["rc"] == 0 and not r.get("stream_mismatch") and r.get("stream") != "exempt"
          and env_unit == [f"FILTERS=--filter={key} --freecam-filter={key} --repeat={n}"]
          and "--repeat" not in env_canary[0]
          and r.get("repeat") == f"{key}: 0 divergent of {legs} legs ({legs} compared)",
          f"rc {r['rc']}; {env_unit[0]}; canary {env_canary[0]}; result repeat '{r.get('repeat')}'")
    golden = os.path.join(TREE, "tests", "golden", GAME, TIER)
    fields = {"blessed-sha256": gf.sha256_path(os.path.join(golden, "blessed.tsv")),
              "ident-baseline-sha256": gf.sha256_path(os.path.join(golden, "ident-baseline.tsv")),
              "compared": str(len(copies)), "exit": "0", "repeat": str(n),
              "repeat-legs": str(legs), "repeat-compared": str(legs)}
    job.result["rc"] = 0
    outs = {}
    for name, over, rows in (("whole", {}, [row]), ("short", {"repeat-compared": str(legs - 1)}, [row]),
                             ("count", {"repeat": "2"}, [row]), ("none", {"repeat-compared": "0",
                              "repeat-legs": "0", "compared": "0"}, []),
                             ("no-row", {}, [])):
        rep = {"fields": dict(fields, **over), "divergent": [], "extra": [], "identity": [],
               "repeat-rows": rows}
        outs[name] = co.check_report(job, rep)
    check("repeat-refusals", outs["whole"] == []
          and any("did not engage every copy" in e for e in outs["short"])
          and any("differs from run --repeat 3" in e for e in outs["count"])
          and any("compared nothing" in e for e in outs["none"])
          and any("no repeat-row line" in e for e in outs["no-row"]),
          "; ".join(f"{k}: {v or 'accepted'}" for k, v in outs.items()))
    parsed_path = os.path.join(co.tmp, "report.txt")
    with open(parsed_path, "w") as f:
        f.write(f"verdict: PASS\n{row}\nrepeat-row: other-1 divergent=2 of=6 compared=6 copies=1\n")
    parsed = gf.parse_report(parsed_path)
    check("repeat-parse", parsed["repeat-rows"] == [row.partition(": ")[2], "other-1 divergent=2 of=6 compared=6 copies=1"],
          f"repeat-rows {parsed['repeat-rows']}")


def read_verdict(run_dir):
    """The `key=value` lines of a run directory's canary-verdict.txt as a dict; None when absent."""
    try:
        with open(os.path.join(run_dir, gf.CANARY_VERDICT), encoding="utf-8") as f:
            return dict(ln.rstrip("\n").partition("=")[::2] for ln in f if "=" in ln)
    except OSError:
        return None


def case_canary_verdict(tmp):
    """canary-verdict.txt per host and the pulled record's fleet stamp."""
    from PIL import Image
    pb = gf.load_pb(TREE)
    co = FakeCo(tmp, "canary-verdict")
    dirs = os.path.join(co.tmp, "canary")
    co.canary_dir = lambda h: os.path.join(dirs, h)

    def scenario(pixels, rcs):
        """Canary tiles per host (`pixels` maps host to a pixel value), leg rcs; return the verdict dict."""
        shutil.rmtree(dirs, ignore_errors=True)
        jobs = []
        for h, rc in rcs.items():
            os.makedirs(os.path.join(dirs, h))
            Image.new("RGB", (4, 4), (pixels[h], 0, 0)).save(os.path.join(dirs, h, "c__canon__1-vk.png"))
            j = gf.Job("canary", GAME, TIER, pinned=h)
            j.result = {"rc": rc, "status": "verdict" if rc in (0, 1) else "leg failed"}
            jobs.append(j)
        co.hosts = [co.hs] * len(rcs)
        rrc = co.compare_canaries(jobs)
        got = read_verdict(co.local_run_dir)
        return rrc, got

    rc_same, same = scenario({"a": 1, "b": 1}, {"a": 0, "b": 1})
    rc_diff, diff = scenario({"a": 1, "b": 2}, {"a": 0, "b": 0})
    rc_leg, leg = scenario({"a": 1, "b": 1}, {"a": 0, "b": 2})
    rc_one, one = scenario({"a": 1}, {"a": 0})
    first = lambda d, h: d.get(f"canary.{h}", "").split(" ")[0]  # noqa: E731
    ok_verdicts = (rc_same == 0 and same["run"] == co.run_id and same["canary"] == "on"
                   and first(same, "a") == first(same, "b") == "pass"
                   and rc_diff == 1 and first(diff, "a") == first(diff, "b") == "fail"
                   and first(leg, "b") == "fail" and first(leg, "a") == "fail"
                   and one["canary"].startswith("off") and "canary.a" not in one)
    check("canary-verdict", ok_verdicts,
          f"same {rc_same} {same}; diff {rc_diff} {diff}; leg {rc_leg} {leg}; one {rc_one} {one}")

    pull = os.path.join(co.tmp, "pull")
    os.makedirs(pull, exist_ok=True)
    with open(os.path.join(pull, gf.RECORD_NAME), "w") as f:
        f.write("game=spyro-2\nhost=remote-box\nfleet-run=stale\nimage-digest=sha256:i\n"
                "libvulkan=1.3\nrecipe-hash=r\n")
    co.hs.name = "b"
    co.stamp_record(pull, co.hs)
    recd = pb.read_record(pull)
    stamped = (recd.get("fleet-run") == co.run_id and recd.get("fleet-host") == "b"
               and recd.get("fleet-run-dir") == os.path.abspath(co.local_run_dir)
               and recd.get("host") == "remote-box"
               and open(os.path.join(pull, gf.RECORD_NAME)).read().count("fleet-run=") == 1)
    check("canary-verdict-record", stamped, f"record {recd}")


def case_run_candidate(tmp):
    """An accepted pull becomes the run's own promotable candidate in the stream directory."""
    from PIL import Image
    pb = gf.load_pb(TREE)
    co = FakeCo(tmp, "run-candidate")
    job = gf.Job("unit", GAME, TIER)
    pull, dest = os.path.join(co.tmp, "pull"), co.stream_dir(job)
    for d in (pull, os.path.join(pull, "identity"), os.path.join(dest, "diff")):
        os.makedirs(d)
    for name, px in (("a__canon__1-vk.png", 1), ("b__canon__1-vk.png", 2)):
        Image.new("RGB", (4, 4), (px, 0, 0)).save(os.path.join(pull, name))
    Image.new("RGB", (4, 4), (3, 0, 0)).save(os.path.join(pull, "identity", "c__ident__1-vk.png"))
    for name in ("verdict-report.txt", "capture-device.txt"):
        with open(os.path.join(pull, name), "w") as f:
            f.write(name + "\n")
    with open(os.path.join(pull, gf.RECORD_NAME), "w") as f:
        f.write(f"game={GAME}\ntier={TIER}\nhost=remote-box\n")
    co.hs.name = "b"
    co.stamp_record(pull, co.hs)
    # The stream pulled a's superseded-attempt capture and a tile the accepted pull lacks.
    Image.new("RGB", (4, 4), (9, 0, 0)).save(os.path.join(dest, "a__canon__1-vk.png"))
    Image.new("RGB", (4, 4), (8, 0, 0)).save(os.path.join(dest, "z__canon__1-vk.png"))
    with open(os.path.join(dest, "diff", "a__canon__1-diff.png"), "wb") as f:
        f.write(b"heat")
    co.keep_run_candidate(job, pull)
    shutil.rmtree(pull)  # the next capture clears the shared pull target
    rec = pb.read_record(dest)
    a_px = Image.open(os.path.join(dest, "a__canon__1-vk.png")).getpixel((0, 0))[0]
    tiles = pb.promote_select(dest, True, [])
    ok = (tiles == ["a__canon__1-vk.png", "b__canon__1-vk.png"] and a_px == 1
          and os.path.isfile(os.path.join(dest, gf.STREAM_ONLY_DIR, "z__canon__1-vk.png"))
          and os.path.isfile(os.path.join(dest, "identity", "c__ident__1-vk.png"))
          and os.path.isfile(os.path.join(dest, "diff", "a__canon__1-diff.png"))
          and os.path.isfile(os.path.join(dest, "verdict-report.txt"))
          and os.path.isfile(os.path.join(dest, "capture-device.txt"))
          and rec.get("fleet-run") == co.run_id and rec.get("fleet-host") == "b"
          and rec.get("fleet-run-dir") == os.path.abspath(co.local_run_dir))
    check("run-candidate", ok, f"tiles {tiles}, a pixel {a_px}, record {rec}, "
          f"entries {sorted(os.listdir(dest))}")


class SimClock:
    """A virtual clock standing in for golden-fleet.py's `time` module: sleep advances it."""

    def __init__(self):
        """Start at 0."""
        self.t = 0.0

    def monotonic(self):
        """The virtual time."""
        return self.t

    def time(self):
        """The virtual time as an epoch."""
        return 1.0e9 + self.t

    def sleep(self, s):
        """Advance the virtual time."""
        self.t += s


SIM_TILES, SIM_ROWS = 640, 64          # per unit; a row is 10 tiles, streamed when the row finishes
SIM_START_S, SIM_KILL_S = 30, 10       # supervisor start to the first leg; KILL_SCRIPT's wall
SIM_HOSTS = (("local", True, 1.00, 6), ("steam-machine", False, 0.47, 4), ("ptah", False, 0.25, 4))
SIM_NORM = {"spyro-1": 3450, "spyro-2": 2970, "spyro-3": 3650}


class SimCo(gf.Coordinator):
    """A Coordinator over simulated hosts on a virtual clock: no ssh, no supervisor, no capture.

    Each host captures a unit's rows in order at norm_s / speed for the
    whole unit, streaming a row's tiles as it finishes; launch, poll, kill,
    carry, admit and pull are replaced, and run_jobs, take_over, move,
    finish_attempt, unit_done and the ledger run as they are. race_unit's
    attempt finishes while a kill of it is in flight.
    """

    def __init__(self, tmp, name, norm=None, hosts=SIM_HOSTS, no_takeover=False, race_unit=None):
        """Build the run directory, the ledger times and the hosts."""
        self.tmp = os.path.join(tmp, name)
        local = os.path.join(self.tmp, "local")
        os.makedirs(local)
        a = argparse.Namespace(game="spyro-1", tier=TIER, all=True, all_tiers=False, hosts="",
                               no_local=False, pull="divergent", expect=None, filter=None,
                               freecam_filter=None, repeat=None, no_takeover=no_takeover)
        super().__init__(a, f"sim-{name}", local)
        self.ledger = {(g, TIER): {"norm_s": str(n), "cand_bytes": str(10**9)}
                       for g, n in (norm or SIM_NORM).items()}
        for hname, is_local, speed, slots in hosts:
            cfg = {"transport": "local" if is_local else "ssh", "ssh": f"sim@{hname}", "speed": speed}
            hs = gf.HostState(gf.Host(hname, cfg), {"root": f"/sim/{hname}", "engine": "podman"},
                              slots, self.run_id)
            hs.touched = True
            self.hosts.append(hs)
            self.skew[hname] = 0.0
        self.race_unit = race_unit
        self.carry = {}

    def canary_on(self):
        """No canary: the schedule of the units alone."""
        return False

    def unit_size(self, job):
        """Every simulated unit has SIM_TILES tiles in SIM_ROWS rows."""
        return SIM_TILES, SIM_ROWS

    def stream_dir(self, job):
        """Stream pulls into the case's own directory."""
        return os.path.join(self.tmp, "stream", job.name)

    def launch(self, job, hs, deadline_s):
        """Start a simulated attempt: its carried rows import at once, the rest capture at the host's speed."""
        att = gf.Attempt(job, hs, deadline_s)
        hs.current = att
        att.imported = self.carry.pop(job.name, 0)
        att.start = gf.time.monotonic() + SIM_START_S
        att.row_s = gf.expected_s(job.unit, self.ledger) / hs.speed / SIM_ROWS
        att.finish_at = att.start + (SIM_TILES - att.imported) // 10 * att.row_s
        att.emitted, att.killed, att.done = 0, False, False
        print(f"[fleet] {hs.name}: {job.name} attempt {att.n} started (sim, {att.imported} tile(s) "
              f"imported, finish at {att.finish_at:.0f} s)")
        return att

    def emit(self, att):
        """Stream the tiles the attempt has finished by now (imported first)."""
        now = gf.time.monotonic()
        own = 0 if att.killed else min(SIM_TILES - att.imported,
                                        int(max(0.0, now - att.start) // att.row_s) * 10)
        if att.done:
            own = SIM_TILES - att.imported
        while att.emitted < att.imported + own:
            i = att.emitted
            tag = " imported" if i < att.imported else ""
            self.receive_line(att, f"SAME {att.job.name}-t{i:03d}-vk.png 0123456789abcdef "
                                   f"{gf.time.time():.3f}{tag}")
            att.emitted += 1

    def poll_attempt(self, att):
        """Report the simulated attempt's state: done at its finish time, dead once killed."""
        if att.killed:
            return ["dead"]
        if gf.time.monotonic() >= att.finish_at:
            att.done = True
        self.emit(att)
        if att.done:
            return ["done", f"rc=0 status=verdict attempt={att.n} "
                            f"wall={int(att.finish_at - att.t0)} cand-bytes=1000"]
        return ["running"]

    def kill_attempt(self, att):
        """Spend SIM_KILL_S; the race unit finishes inside it, else the attempt dies."""
        self.emit(att)
        if att.job.name == self.race_unit:
            att.finish_at = gf.time.monotonic() + SIM_KILL_S / 2
        gf.time.sleep(SIM_KILL_S)
        if gf.time.monotonic() >= att.finish_at:
            att.done = True
        else:
            att.killed = True
        att.t_end = att.t_end or gf.time.monotonic()

    def carry_rows(self, att, tgt):
        """Carry the attempt's finished rows (every streamed tile is a stamped row's)."""
        tiles = att.emitted // 10 * 10
        self.carry[att.job.name] = tiles
        return tiles // 10, tiles

    def add_host(self, name):
        """Admit a host of the run as is."""
        return self.host_state(name)

    def pull_unit(self, job, hs):
        """Record the census the importer would report."""
        att = job.atts[job.result["attempt"]]
        job.result["imported_rows"] = att.imported // 10
        job.result["census"] = f"captured={SIM_TILES - att.imported} imported={att.imported}"
        return []

    def go(self):
        """Run the units to their results on the virtual clock; return the run's end time."""
        real, gf.time = gf.time, SimClock()
        try:
            self.jobs = self.make_jobs()
            self.run_jobs(self.jobs, on_done=self.unit_done)
            self.print_summary(self.run_rc())
            return gf.time.monotonic()
        finally:
            gf.time = real


def sim_ledger(run_id):
    """The redirected ledger's rows of one run, as dicts keyed by game."""
    rows = [dict(zip(gf.LEDGER_COLS, ln.split("\t"))) for ln in ledger_rows()]
    return {r["game"]: r for r in rows if r["run"] == run_id}


def case_takeover(tmp):
    """Simulated three-host schedule: the free fast host takes over the unit predicted to finish last, a non-paying one is not taken, --no-takeover keeps placement only, the ledger prices the moved unit over both attempts."""
    base = SimCo(tmp, "takeover-off", no_takeover=True)
    end_off = base.go()
    co = SimCo(tmp, "takeover-on")
    end_on = co.go()
    jobs = {j.game: j for j in co.jobs}
    tks = co.takeovers
    s2 = jobs["spyro-2"]
    att1, att2 = s2.atts.get(1), s2.atts.get(s2.result.get("attempt"))
    tiles = [r["tile"] for r in att2.lines] if att2 else []
    imported = sum(1 for r in att2.lines if r["imported"]) if att2 else 0
    noted = [k for k in co.takeover_noted if k[1] == "spyro-1-presentation"]
    check("takeover", len(tks) == 1 and tks[0]["job"] is s2 and tks[0]["src"] == "ptah"
          and tks[0]["dst"] == "local" and s2.result["host"] == "local" and s2.moved == [("ptah", "local")]
          and len(tiles) == len(set(tiles)) == SIM_TILES and imported == tks[0]["tiles"] > 0
          and noted and jobs["spyro-1"].result["host"] == "steam-machine"
          and end_on < end_off - gf.TAKEOVER_MIN_SAVING_S,
          f"takeovers {[(t['job'].name, t['src'], t['dst'], t['rows']) for t in tks]}; spyro-2 on "
          f"{s2.result['host']} ({len(tiles)} tiles streamed, {len(set(tiles))} distinct, {imported} "
          f"imported); spyro-1 not taken {noted}; end {end_on:.0f} s, {end_off:.0f} s without")
    off = [j.result["host"] for j in base.jobs]
    check("takeover-off", not base.takeovers and not base.takeover_noted
          and all(not j.moved for j in base.jobs) and sorted(off) == ["local", "ptah", "steam-machine"],
          f"--no-takeover: takeovers {len(base.takeovers)}, evaluations {len(base.takeover_noted)}, "
          f"hosts {off}, end {end_off:.0f} s")
    led = sim_ledger(co.run_id)
    want = (att1.t_end - att1.t0) * 0.25 + s2.result["wall"] * 1.0 if att1 and att1.t_end else -1
    check("takeover-ledger", set(led) == set(SIM_NORM) and led["spyro-2"]["host"] == "ptah>local"
          and int(led["spyro-2"]["norm_s"]) == int(want)
          and abs(int(led["spyro-2"]["norm_s"]) - SIM_NORM["spyro-2"]) < 0.1 * SIM_NORM["spyro-2"]
          and int(led["spyro-3"]["norm_s"]) == int(jobs["spyro-3"].result["wall"]),
          f"rows {sorted(led)}; spyro-2 host {led.get('spyro-2', {}).get('host')} norm_s "
          f"{led.get('spyro-2', {}).get('norm_s')} (want {want:.0f}, ledger time {SIM_NORM['spyro-2']}); "
          f"spyro-3 norm_s {led.get('spyro-3', {}).get('norm_s')} = wall {jobs['spyro-3'].result['wall']}")


def case_takeover_slow(tmp):
    """A free host slower than every running unit's host takes over nothing."""
    co = SimCo(tmp, "takeover-slow", norm={"spyro-3": 20000, "spyro-1": 500, "spyro-2": 400},
               hosts=(SIM_HOSTS[0], SIM_HOSTS[2]))
    co.go()
    why = [k for k in co.takeover_noted if k[0] == "ptah"]
    check("takeover-slow", not co.takeovers and why and all(not j.moved for j in co.jobs)
          and {j.game: j.result["host"] for j in co.jobs}["spyro-3"] == "local",
          f"takeovers {len(co.takeovers)}; ptah's refusals {why}")


def case_takeover_race(tmp):
    """The taken-over unit finishes while the stop is in flight: its own attempt's result stands, nothing carried, one ledger row; the free host then takes the next-last unit."""
    co = SimCo(tmp, "takeover-race", race_unit="spyro-2-presentation")
    co.go()
    s2 = next(j for j in co.jobs if j.game == "spyro-2")
    att = s2.atts.get(s2.result.get("attempt"))
    tiles = [r["tile"] for r in att.lines] if att else []
    led = sim_ledger(co.run_id)
    check("takeover-race", not [t for t in co.takeovers if t["job"] is s2] and not s2.moved and s2.moves == 0 and s2.attempts == 1
          and s2.result["host"] == "ptah" and s2.result["rc"] == 0
          and len(tiles) == len(set(tiles)) == SIM_TILES and not any(r["imported"] for r in att.lines)
          and led["spyro-2"]["host"] == "ptah" and "spyro-2-presentation" not in co.carry,
          f"result rc {s2.result['rc']} on {s2.result['host']} attempt {s2.result.get('attempt')}; "
          f"moved {s2.moved}; {len(tiles)} tiles, {len(set(tiles))} distinct; ledger host "
          f"{led.get('spyro-2', {}).get('host')}")


def case_takeover_rule(tmp):
    """takeover_prediction and takeover_refusal on fixed numbers: the formula, the cost floor, the threshold, the speed rule."""
    p = gf.takeover_prediction(done=200, total=640, rows=64, norm_s=2970, cand_b=10**9,
                               src_speed=0.25, src_slots=4, dst_speed=1.0, hops=1)
    left = 440 / 640
    in_flight = 2970 * 4 / (2 * 64 * 0.25)
    carry = (200 / 640) * 10**9 / gf.CARRY_BPS
    ok = abs(p["src_s"] - (left * 2970 / 0.25 - in_flight)) < 1e-6 \
        and abs(p["dst_s"] - (left * 2970 + gf.TAKEOVER_FIXED_S + carry)) < 1e-6
    late = gf.takeover_prediction(638, 640, 64, 2970, 10**9, 0.25, 4, 1.0, 1)
    small = gf.takeover_prediction(560, 640, 64, 2970, 10**9, 0.47, 4, 1.0, 1)
    refusals = (gf.takeover_refusal(p, 0.25, 1.0), gf.takeover_refusal(late, 0.25, 1.0),
                gf.takeover_refusal(small, 0.47, 1.0), gf.takeover_refusal(p, 1.0, 0.25))
    check("takeover-rule", ok and refusals[0] == "" and "under the takeover cost" in refusals[1]
          and f"under {gf.TAKEOVER_MIN_SAVING_S} s" in refusals[2] and "not above" in refusals[3],
          f"src {p['src_s']:.0f} s, dst {p['dst_s']:.0f} s, saving {p['saving_s']:.0f} s; refusals {refusals}")


class BaseCo(gf.Coordinator):
    """A Coordinator reduced to the sync, recheck and base refresh of one checkout onto local hosts."""

    def __init__(self, tmp, name, tree, roots, ssh=False):
        """Bind the checkout, the run id and one host per fleet root (local, or ssh through FAKE_SSH); no preflight, build or units."""
        self.tree, self.run_id = tree, f"test-{name}"
        self.local_run_dir = os.path.join(tmp, "runs-local", name)
        os.makedirs(self.local_run_dir)
        self.hosts = []
        for i, root in enumerate(roots):
            cfg = {"transport": "ssh", "ssh": "u@sim"} if ssh else {"transport": "local"}
            host = gf.Host(f"{name}-h{i}", dict(cfg, fleet_root=root))
            self.hosts.append(gf.HostState(host, {"root": root, "engine": "podman"}, 1, self.run_id))

    def sync_raw(self, files, refresh=True):
        """Sync files to every host, recheck, refresh the bases; output to stdout."""
        for hs in self.hosts:
            self.mkdirs(hs)
        self.sync_hosts([("source", self.tree, files)], self.hosts)
        if refresh:
            self.refresh_bases(self.hosts)

    def sync(self, files, refresh=True):
        """sync_raw with stdout captured; return (FleetError or None, output)."""
        return captured(lambda: self.sync_raw(files, refresh))

    def tree_of(self, hs=None):
        """Return the run's snapshot directory on a host."""
        return f"{(hs or self.hosts[0]).run_dir}/tree"


SYNC_RE = r"\[fleet\] (\S+): synced source: (\d+) files, (\d+) MiB sent in \d+ s \((.*)\)"


def synced(out):
    """Return {host: (MiB sent, note)} from a sync's output."""
    import re
    return {m.group(1): (int(m.group(3)), m.group(4)) for m in re.finditer(SYNC_RE, out)}


def make_repo(tmp, nfiles=8):
    """Make a git repository of nfiles 1 MiB files and a second worktree of it; return (main, worktree, files)."""
    main, wt = os.path.join(tmp, "repo"), os.path.join(tmp, "repo-wt")
    os.makedirs(main)
    files = [f"d/f{i}.bin" for i in range(nfiles)]
    os.makedirs(os.path.join(main, "d"))
    for f in files:
        with open(os.path.join(main, f), "wb") as fh:
            fh.write(os.urandom(1 << 20))
    git = ["git", "-c", "user.name=t", "-c", "user.email=t@t", "-C", main]
    subprocess.run(git + ["init", "-q"], check=True)
    subprocess.run(git + ["add", "."], check=True)
    subprocess.run(git + ["commit", "-q", "-m", "files"], check=True)
    subprocess.run(git + ["worktree", "add", "-q", wt], check=True)
    for f in files:   # the worktree's files carry other mtimes than the main checkout's
        os.utime(os.path.join(wt, f), (1_600_000_000, 1_600_000_000))
    return main, wt, files


def rewrite(path, keep_mtime=True):
    """Change a file's content in place (same size, mtime restored when keep_mtime)."""
    st = os.stat(path)
    with open(path, "r+b") as fh:
        fh.write(os.urandom(4096))
    if keep_mtime:
        os.utime(path, ns=(st.st_atime_ns, st.st_mtime_ns))


def same(a, b):
    """True when two files hold the same bytes."""
    return open(a, "rb").read() == open(b, "rb").read()


def drop_run(co):
    """Remove a run's directories on its hosts, as its teardown does."""
    for hs in co.hosts:
        shutil.rmtree(hs.run_dir, ignore_errors=True)


def case_sync_base(tmp):
    """The repository base: made by a first run, reused by a run from another checkout, never trusted, locked, cleaned."""
    tmp = os.path.join(tmp, "sync-base")
    os.makedirs(tmp)
    main, wt, files = make_repo(tmp)
    root = os.path.join(tmp, "root")
    key = gf.base_key(main)

    # 1. First run, from the main checkout: no base, every byte sent, the base made.
    co1 = BaseCo(tmp, "b1", main, [root])
    err, out = co1.sync(files)
    s1 = synced(out).get(co1.hosts[0].name, (None, ""))
    rec = gf.parse_kv(open(os.path.join(root, "trees", f"{key}.src")).read())
    check("base-first", err is None and s1 == (8, "no base") and gf.base_key(wt) == key
          and rec.get("repo") == gf.repo_dir(main) and rec.get("path") == main
          and os.path.isdir(os.path.join(root, "trees", key)),
          f"sent {s1[0]} MiB ({s1[1]}); base {key} recorded repo={rec.get('repo')}, path={rec.get('path')}; "
          f"the worktree's key {'equals' if gf.base_key(wt) == key else 'differs from'} the main checkout's; {err}")
    drop_run(co1)

    # 2. Second run, from the worktree (another path, other mtimes), one file changed: only it is sent.
    rewrite(os.path.join(wt, files[3]), keep_mtime=False)
    co2 = BaseCo(tmp, "b2", wt, [root])
    err, out = co2.sync(files)
    s2 = synced(out).get(co2.hosts[0].name, (None, ""))
    equal = all(same(os.path.join(wt, f), os.path.join(co2.tree_of(), f)) for f in files)
    check("base-reuse", err is None and s2[0] == 1 and s2[1].startswith(f"base {key} from {main}")
          and "recheck source: 0 changes" in out and equal,
          f"sent {s2[0]} MiB of 8 ({s2[1]}); snapshot equals the worktree: {equal}; {err}")
    drop_run(co2)

    # 3. A tampered base (content changed in place, size and mtime kept): the
    #    checksum sync sends the file instead of linking it.
    base_f = os.path.join(root, "trees", key, files[5])
    rewrite(base_f)
    co3 = BaseCo(tmp, "b3", wt, [root])
    err, out = co3.sync(files, refresh=False)
    s3 = synced(out).get(co3.hosts[0].name, (None, ""))
    ok3 = err is None and s3[0] == 1 and same(os.path.join(wt, files[5]), os.path.join(co3.tree_of(), files[5]))
    # 4. A base file the snapshot links, changed after the sync: the recheck refuses the run.
    linked = next(f for f in files if f != files[5])
    rewrite(os.path.join(root, "trees", key, linked))
    err4, out4 = captured(lambda: co3.recheck(co3.hosts[0], wt, os.path.join(co3.local_run_dir, "source.lst"), "source"))
    check("base-tampered", ok3 and isinstance(err4, gf.FleetError) and "torn sync" in str(err4)
          and linked in str(err4),
          f"tampered base file sent, not linked ({s3[0]} MiB; snapshot equals the checkout: {ok3}); a linked base "
          f"file changed after the sync: {str(err4).splitlines()[0] if err4 else 'no refusal'}")
    drop_run(co3)

    # 5. A foreign base (its record names another repository): not linked from, replaced by the refresh.
    srcf = os.path.join(root, "trees", f"{key}.src")
    text = open(srcf).read()
    with open(srcf, "w") as fh:
        fh.write(text.replace(f"repo={gf.repo_dir(main)}", "repo=/elsewhere/.git"))
    co5 = BaseCo(tmp, "b5", wt, [root])
    err, out = co5.sync(files)
    s5 = synced(out).get(co5.hosts[0].name, (None, ""))
    rec5 = gf.parse_kv(open(srcf).read())
    check("base-foreign", err is None and s5[0] == 8 and s5[1].startswith("foreign base ignored: repo=")
          and rec5.get("repo") == gf.repo_dir(main),
          f"sent {s5[0]} MiB ({s5[1]}); the refresh recorded repo={rec5.get('repo')}")
    drop_run(co5)

    # 6. Overlap: runs from both checkouts (the worktree's file 3 differs) sync
    #    and refresh concurrently onto one host; each snapshot equals its checkout.
    coa, cob = BaseCo(tmp, "ov-a", main, [root]), BaseCo(tmp, "ov-b", wt, [root])
    errs = []

    def run(co):
        try:
            co.sync_raw(files)
        except gf.FleetError as e:
            errs.append(e)

    def both():
        threads = [threading.Thread(target=run, args=(c,)) for c in (coa, cob)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    _r, out6 = captured(both)
    s6 = synced(out6)
    # A third run swaps the base again while both snapshots stand.
    coc = BaseCo(tmp, "ov-c", main, [root])
    errc, _outc = coc.sync(files)
    rechecks = [captured(lambda c=c: c.recheck(c.hosts[0], c.tree, os.path.join(c.local_run_dir, "source.lst"),
                                               "source"))[0] for c in (coa, cob)]
    ta, tb = coa.tree_of(), cob.tree_of()
    ok6 = (not errs and errc is None and rechecks == [None, None] and out6.count("recheck source: 0 changes") == 2
           and not same(os.path.join(ta, files[3]), os.path.join(tb, files[3])))
    check("base-overlap", ok6,
          f"two concurrent runs (sent {[v[0] for v in s6.values()]} MiB) and a later refresh: errors {[str(e) for e in errs]}"
          f"{'' if errc is None else errc}; each snapshot rechecked against its checkout: "
          f"{['0 changes' if r is None else str(r).splitlines()[0] for r in rechecks]}")

    # 7. The lock: a refresh waits while a sync holds the base's lock shared.
    lock = os.path.join(root, "trees", f"{key}.lock")
    holder = subprocess.Popen(["flock", "-s", lock, "sleep", "30"], start_new_session=True)
    time.sleep(0.3)
    done = threading.Event()
    th = threading.Thread(target=lambda: (coc.refresh_base(coc.hosts[0]), done.set()))
    th.start()
    waited = not done.wait(1.5)
    os.killpg(holder.pid, signal.SIGKILL)
    holder.wait()
    th.join(30)
    # A sync reading the base waits while a refresh holds the lock exclusive.
    cod = BaseCo(tmp, "ov-d", wt, [root])
    holder = subprocess.Popen(["flock", "-x", lock, "sleep", "30"], start_new_session=True)
    time.sleep(0.3)
    sdone = threading.Event()
    th2 = threading.Thread(target=lambda: (captured(lambda: cod.sync_raw(files, refresh=False)), sdone.set()))
    th2.start()
    swaited = not sdone.wait(1.5)
    os.killpg(holder.pid, signal.SIGKILL)
    holder.wait()
    th2.join(30)
    check("base-lock", waited and done.is_set() and swaited and sdone.is_set(),
          f"refresh blocked while a sync held the lock shared: {waited}, then finished: {done.is_set()}; "
          f"sync blocked while a refresh held it exclusive: {swaited}, then finished: {sdone.is_set()}")
    for c in (coa, cob, coc, cod):
        drop_run(c)

    # 8. Build seed: only from a base whose record names the same checkout path.
    bt = os.path.join(root, "trees", key, gf.BUILD_TREE)
    os.makedirs(bt, exist_ok=True)
    open(os.path.join(bt, "obj"), "w").write("x")
    seeds = []
    for co in (BaseCo(tmp, "seed-wt", wt, [root]), BaseCo(tmp, "seed-main", main, [root])):
        hs = co.hosts[0]
        co.mkdirs(hs)
        seeds.append(hs.host.run_script(gf.SEED_BUILD, [root, key, co.run_id, co.tree]).stdout.strip())
        drop_run(co)
    check("base-seed", seeds == ["no-seed (the base is another checkout's)", "seeded"],
          f"seed into a worktree run, then a main-checkout run (base recorded from {main}): {seeds}")

    # 9. Status, clean and --tree: a stale clean and --tree keep the base; a
    #    checkout-keyed tree of the path goes with --tree; a base of a gone
    #    repository goes with a stale clean; --all removes trees/.
    host = gf.Host("h0", {"transport": "local", "fleet_root": root, "engine": "no-such-engine"})
    lines = gf.tree_lines(gf.list_trees(host, sizes=True))
    legacy = "0123456789abcdef"
    os.makedirs(os.path.join(root, "trees", legacy))
    with open(os.path.join(root, "trees", f"{legacy}.src"), "w") as fh:
        fh.write(f"coordinator={os.uname().nodename}\npath={wt}\nlast-run={int(time.time())}\n")
    captured(lambda: gf.clean_host(host, "stale"))
    kept_stale = os.path.isdir(os.path.join(root, "trees", key))
    captured(lambda: gf.clean_host(host, "tree", wt))
    kept_tree = os.path.isdir(os.path.join(root, "trees", key))
    legacy_gone = not os.path.exists(os.path.join(root, "trees", legacy))
    gone_key = "fedcba9876543210"
    os.makedirs(os.path.join(root, "trees", gone_key))
    with open(os.path.join(root, "trees", f"{gone_key}.src"), "w") as fh:
        fh.write(f"coordinator={os.uname().nodename}\nrepo={tmp}/gone/.git\npath={tmp}/gone\n"
                 f"last-run={int(time.time())}\n")
    captured(lambda: gf.clean_host(host, "stale"))
    gone_removed = not os.path.exists(os.path.join(root, "trees", gone_key))
    captured(lambda: gf.clean_host(host, "all"))
    all_removed = not os.path.exists(os.path.join(root, "trees"))
    check("base-clean", len(lines) == 1 and lines[0].startswith(f"base {key}: ")
          and kept_stale and kept_tree and legacy_gone and gone_removed and all_removed,
          f"status: {lines}; stale clean keeps the base: {kept_stale}; --tree keeps it: {kept_tree} and removes "
          f"the checkout-keyed tree: {legacy_gone}; a base of a gone repository removed: {gone_removed}; "
          f"--all removes trees/: {all_removed}")

    # 10. The ssh form (FAKE_SSH runs the remote command here): the rsync
    #     server holds the base's lock shared under the run tag; a second run
    #     from the worktree sends only file 3's changed blocks.
    fake = FakeSsh(tmp)
    try:
        fake.arm(0)
        sent = []
        for name, tree in (("ssh-1", main), ("ssh-2", wt)):
            co = BaseCo(tmp, name, tree, [root], ssh=True)
            with gf.run_tag(f"test-{name}"):
                err, out = co.sync(files)
            sent.append((synced(out).get(co.hosts[0].name, (None, "")), err))
            drop_run(co)
    finally:
        fake.restore()
    (s_a, e_a), (s_b, e_b) = sent
    check("base-ssh", e_a is None and e_b is None and s_a == (8, "no base") and s_b[0] == 0
          and s_b[1].startswith(f"base {key} from {main}") and os.path.exists(lock),
          f"first run {s_a[0]} MiB ({s_a[1]}), second from the worktree {s_b[0]} MiB ({s_b[1]}); errors {e_a}, {e_b}")


def case_on_hosts(tmp):
    """on_hosts runs hosts concurrently; one host's failure stops the others' children and is raised."""
    hosts = [gf.HostState(gf.Host(f"p{i}", {"transport": "local"}), {"root": tmp}, 1, "p") for i in range(3)]
    t0 = time.monotonic()
    res = gf.on_hosts(hosts, lambda hs: (gf.run_proc(["sleep", "1"]).returncode, hs.name))
    wall = time.monotonic() - t0
    check("on-hosts", res == [(0, "p0"), (0, "p1"), (0, "p2")] and wall < 2.0,
          f"three 1 s children took {wall:.1f} s; results {res}")
    pid_file = os.path.join(tmp, "on-hosts-{}.pid")

    def fn(hs):
        if hs.name == "p0":
            time.sleep(0.5)
            raise gf.FleetError("p0 failed")
        return gf.run_proc(["bash", "-c", f'echo $$ >"{pid_file.format(hs.name)}"; exec sleep 30'])
    t0 = time.monotonic()
    err, _out = captured(lambda: gf.on_hosts(hosts, fn))
    wall = time.monotonic() - t0
    alive = [p for p in (open(pid_file.format(n)).read().strip() for n in ("p1", "p2"))
             if os.path.exists(f"/proc/{p}")]
    check("on-hosts-fail", isinstance(err, gf.FleetError) and str(err) == "p0 failed"
          and wall < gf.STOP_POLL_S + 3 and not alive,
          f"raised {err!r} after {wall:.1f} s; the other hosts' children alive: {len(alive)} of 2")


class HostWaitCo(gf.Coordinator):
    """A Coordinator whose prepare_hosts runs over a scripted preflight: one result list per call, the last repeated."""

    def __init__(self, tmp, name, rounds, **args):
        """Bind the scripted preflight rounds and the run arguments; no host file, clean or image ship."""
        d = os.path.join(tmp, name)
        os.makedirs(d)
        a = argparse.Namespace(game=GAME, tier=TIER, all=False, all_tiers=False, hosts="",
                               no_local=False, pull="divergent", expect=None, host_wait=gf.HOST_WAIT_S,
                               no_host_wait=False, local_anyway=False)
        for k, v in args.items():
            setattr(a, k, v)
        super().__init__(a, f"test-{name}", d)
        self.rounds, self.calls = rounds, 0
        self.root = os.path.join(d, "root")

    def need_bytes(self, src_bytes):
        """No sync set: the scripted preflight decides."""
        return 0


def with_fakes(co, fn):
    """Run fn() with golden-fleet's host file, preflight, clean, image ship and recipe check replaced by co's script."""
    import contextlib
    import io
    names = ("load_hosts", "preflight", "clean_host", "ship_image", "check_recipe", "local_busy")
    saved = {n: getattr(gf, n) for n in names}
    hosts = {"local": {"transport": "local", "fleet_root": co.root}, "r": {"transport": "ssh", "ssh": "u@r"}}

    def preflight(sel, need):
        """Return the next scripted round as preflight's tuples over the selected hosts."""
        rnd = co.rounds[min(co.calls, len(co.rounds) - 1)]
        co.calls += 1
        return [(h, {"memavail_kib": str(64 * gf.GIB_KIB), "root": co.root, "engine": "podman"},
                 list(rnd.get(h.name, [])), 1) for h in sel]
    gf.load_hosts = lambda required=True: ("hosts.toml", hosts)
    gf.preflight = preflight
    gf.clean_host = lambda *a, **k: 0
    gf.ship_image = lambda *a, **k: None
    gf.check_recipe = lambda tree: None
    gf.local_busy = getattr(co, "busy", lambda tree, kib: [])
    out = io.StringIO()
    err = None
    try:
        with contextlib.redirect_stdout(out):
            try:
                fn()
            except gf.FleetError as e:
                err = e
    finally:
        for n, f in saved.items():
            setattr(gf, n, f)
        if co.golden_fd is not None:
            os.close(co.golden_fd)
            co.golden_fd = None
    sys.stdout.write(out.getvalue())
    return out.getvalue(), err


def case_host_wait(tmp):
    """run waits while every host is held: admitted once a lock frees, --no-host-wait and a non-transient drop exit 64 at once, the bound ends the wait, a stop request ends it."""
    poll, stop_poll = gf.HOST_WAIT_POLL_S, gf.STOP_POLL_S
    gf.HOST_WAIT_POLL_S, gf.STOP_POLL_S = 0.2, 0.05
    held = {"local": ["golden lock held: pid 7 host h golden-fleet run r-1 since x"],
            "r": ["golden lock held: pid 9 host r golden-fleet r-1 spyro-2-presentation since y"]}
    try:
        co = HostWaitCo(tmp, "hw-free", [held, held, {"r": held["r"]}])
        log, err = with_fakes(co, lambda: co.prepare_hosts(0))
        check("host-wait-admit", err is None and co.calls == 3 and [h.name for h in co.hosts] == ["local"]
              and "waiting up to" in log and "golden-fleet run r-1" in log and "waited " in log,
              f"error {err}; preflight calls {co.calls}; hosts {[h.name for h in co.hosts]}; "
              f"wait line printed {'waiting up to' in log}, holder named {'run r-1' in log}")

        co = HostWaitCo(tmp, "hw-nowait", [held], no_host_wait=True)
        log, err = with_fakes(co, lambda: co.prepare_hosts(0))
        check("host-wait-off", err is not None and err.code == gf.EXIT_SKIP and str(err) == "no usable host"
              and co.calls == 1 and "waiting" not in log,
              f"--no-host-wait: exit {err and err.code} '{err}', preflight calls {co.calls}")

        co = HostWaitCo(tmp, "hw-hard", [{"local": ["MemAvailable 1.0 GiB under 2 GiB for one slot"],
                                          "r": ["unreachable: ssh: connect refused"]}])
        log, err = with_fakes(co, lambda: co.prepare_hosts(0))
        check("host-wait-hard", err is not None and err.code == gf.EXIT_SKIP and co.calls == 1,
              f"no transient drop: exit {err and err.code}, preflight calls {co.calls}")

        co = HostWaitCo(tmp, "hw-bound", [held], host_wait=1)
        t0 = time.monotonic()
        log, err = with_fakes(co, lambda: co.prepare_hosts(0))
        dt = time.monotonic() - t0
        check("host-wait-bound", err is not None and err.code == gf.EXIT_SKIP and "after waiting" in str(err)
              and 1 <= dt < 3 and co.calls >= 3,
              f"--host-wait 1: exit {err and err.code} '{err}' after {dt:.1f} s, {co.calls} preflights")

        co = HostWaitCo(tmp, "hw-stop", [held])
        FakeCo.stop_later(co, 0.5, "all")
        t0 = time.monotonic()

        def watched():
            """prepare_hosts under the run's stop predicate, as Coordinator.run installs it."""
            with gf.stop_watch(co.stop_covers_run):
                co.prepare_hosts(0)
        log, err = with_fakes(co, watched)
        dt = time.monotonic() - t0
        check("host-wait-stop", isinstance(err, gf.StopRequested) and dt < 3,
              f"stop request during the wait: {type(err).__name__} after {dt:.1f} s")

        co = HostWaitCo(tmp, "hw-busy", [{"r": ["unreachable: x"]}], host_wait=1)
        co.busy = lambda tree, kib: ["local busy: 4242 build/linux-release/openpete --headless"]
        log, err = with_fakes(co, lambda: co.prepare_hosts(0))
        drop = "left out as --no-local would" in log and "4242" in log
        co2 = HostWaitCo(tmp, "hw-busy-anyway", [{"r": ["unreachable: x"]}], local_anyway=True)
        co2.busy = co.busy
        log2, err2 = with_fakes(co2, lambda: co2.prepare_hosts(0))
        co3 = HostWaitCo(tmp, "hw-busy-ends", [{"r": ["unreachable: x"]}])
        seq = iter([["local busy: a landing holds /x/land-train.lock"], []])
        co3.busy = lambda tree, kib: next(seq, [])
        log3, err3 = with_fakes(co3, lambda: co3.prepare_hosts(0))
        check("host-wait-local-busy", drop and err is not None and "after waiting" in str(err)
              and err.code == gf.EXIT_SKIP and co.hosts == []
              and err2 is None and [h.name for h in co2.hosts] == ["local"]
              and err3 is None and co3.calls == 2 and "land-train.lock" in log3,
              f"busy local dropped and printed: {drop}, waited to the bound: exit {err and err.code} '{err}'; "
              f"--local-anyway keeps it: {[h.name for h in co2.hosts]}; a landing that ends: admitted after "
              f"{co3.calls} preflights")
    finally:
        gf.HOST_WAIT_POLL_S, gf.STOP_POLL_S = poll, stop_poll


def case_local_busy(tmp):
    """local_busy over a fake process table: landing scripts, the scheduler gate, game processes outside the fleet, the land lock, memory."""
    def proc(pid, ppid, argv, exe="", cgroup="0::/user.slice"):
        return {"pid": pid, "ppid": ppid, "argv": argv, "exe": exe, "cgroup": cgroup}
    repo = os.path.join(tmp, "busy-repo")
    os.makedirs(repo)
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    subprocess.run(["git", "init", "-q", repo], check=True, env=env)
    big = 64 * gf.GIB_KIB
    procs = [proc(1, 0, ["/sbin/init"]),
             proc(10, 1, ["bash"]), proc(11, 10, ["python3", "/x/golden-fleet.py", "run", "--foreground"]),
             proc(20, 1, ["bash", "/c/.claude/skills/orchestrate/scripts/land-branch.sh", "agent/x"]),
             proc(30, 1, ["python3", "/c/sched_gate.py", "--repo", "/r"]),
             proc(40, 1, ["build/linux-release/openpete", "--headless"], exe="/r/build/linux-release/openpete"),
             proc(50, 1, ["/opt/openpete/openpete", "--headless"], cgroup="0::/libpod_parent/libpod-abc"),
             proc(60, 11, ["openpete-spyro1", "--headless"]),
             proc(70, 1, ["/usr/bin/conmon", "--cid", "x"]), proc(71, 70, ["openpete"]),
             proc(80, 1, ["vim", "land-branch.sh.notes"])]
    got = gf.local_busy(repo, big, procs=procs, me=11)
    pids = sorted(int(r.split()[2]) for r in got if r.split()[2].isdigit())
    quiet = gf.local_busy(repo, big, procs=[proc(1, 0, ["/sbin/init"])], me=1)
    low = gf.local_busy(repo, 5 * gf.GIB_KIB, procs=[], me=1)
    common = subprocess.run(["git", "-C", repo, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                            capture_output=True, text=True, env=env).stdout.strip()
    lock = os.path.join(common, gf.LAND_LOCK_NAME)
    holder = subprocess.Popen(["flock", lock, "sleep", "30"], start_new_session=True)
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline and not gf.land_lock_holder(repo):
        time.sleep(0.05)
    landing = gf.local_busy(repo, big, procs=[], me=1)
    os.killpg(holder.pid, signal.SIGTERM)
    holder.wait()
    after = gf.local_busy(repo, big, procs=[], me=1)
    check("local-busy", pids == [20, 30, 40] and quiet == [] and len(low) == 1 and "MemAvailable 5.0" in low[0]
          and len(landing) == 1 and gf.LAND_LOCK_NAME in landing[0] and after == [],
          f"busy pids {pids} (landing 20, gate 30, loose game 40; container 50, fleet child 60, conmon child 71, "
          f"own ancestors, an editor 80 not); idle box {quiet}; 5 GiB: {low}; land lock held: {landing}; "
          f"released: {after}")


RECORD_LINES = (f'printf "game={GAME}\\ntier={TIER}\\nhead=0123\\n" >"$UNIT_DIR/cand/capture-record.txt"\n'
                'echo "device=llvmpipe" >"$UNIT_DIR/cand/capture-device.txt"\n')


def case_record_every_end(tmp):
    """Every unit end leaves the stamped capture record in the stream directory: a refused pull and a non-verdict exit as well as an accepted pull."""
    new = ["zz-new__canon__1-vk.png"]
    out = {}
    for name, rc, refuse in (("rec-accepted", 1, False), ("rec-refused", 1, True), ("rec-exit2", 2, False)):
        co = FakeCo(tmp, name)
        if rc == 2:
            body = RECORD_LINES + "report 'verdict: FAIL' 'exit: 2'\nfinish 2 harness-exit\n"
        else:
            body = RECORD_LINES + full_unit(div=[], same=[], extra=new, ident=[], exceed=[])
        co.plan(UNIT, body)

        def pull(job, hs, co=co, refuse=refuse):
            """The real pull's record handling over the fake report: pull and stamp the record, keep the candidate; or refuse after the report, as a census refusal does."""
            local = co.pull_target(GAME, TIER)
            shutil.rmtree(local, ignore_errors=True)
            rdir = f"{job.result['unit_dir']}/cand"
            if refuse:
                gf.rsync_pull(hs.host, rdir, local, names=["verdict-report.txt"])
                return ["census compared=0: the filtered unit compared nothing"]
            gf.rsync_pull(hs.host, rdir, local, names=["verdict-report.txt", gf.RECORD_NAME, "capture-device.txt"])
            co.stamp_record(local, hs)
            co.pull_tiles(hs, rdir, local, gf.parse_report(os.path.join(local, "verdict-report.txt")))
            co.keep_run_candidate(job, local)
            return []
        co.pull_unit = pull
        job = gf.Job("unit", GAME, TIER)
        co.go([job])
        dest = co.stream_dir(job)
        rec_path = os.path.join(dest, gf.RECORD_NAME)
        txt = open(rec_path).read() if os.path.exists(rec_path) else ""
        out[name] = (job.result["rc"], f"fleet-run=test-{name}" in txt and "head=0123" in txt,
                     os.path.exists(os.path.join(dest, "capture-device.txt")),
                     os.path.isdir(os.path.join(dest, ".record-pull")),
                     os.path.exists(os.path.join(dest, new[0])))
    ok = all(v[1] and v[2] and not v[3] for v in out.values()) and out["rec-refused"][4]
    check("record-every-end", ok,
          "; ".join(f"{k}: rc {v[0]}, stamped record {v[1]}, device {v[2]}, temp left {v[3]}, "
                    f"streamed tile {v[4]}" for k, v in out.items()))


def case_new_rows(tmp):
    """A filtered unit whose only moves are extra tiles ends NEW-ROWS (rc 0) and names them; a divergence, an identity failure, a crash or an unfiltered unit keeps FAIL."""
    import contextlib
    import io
    co = FakeCo(tmp, "new-rows")
    co.a.filter = "zz-new"
    golden = os.path.join(TREE, "tests", "golden", GAME, TIER)
    fields = {"blessed-sha256": gf.sha256_path(os.path.join(golden, "blessed.tsv")),
              "ident-baseline-sha256": gf.sha256_path(os.path.join(golden, "ident-baseline.tsv")),
              "compared": "0", "exit": "1", "credits-rc": "64"}
    new = ["zz-new__canon__1-vk.png", "zz-new__inter__1-vk.png"]

    def run(over=None, **lists):
        """check_report then accept_new_rows on a report built from fields and lists; return (refusals, result)."""
        job = gf.Job("unit", GAME, TIER)
        job.result = {"rc": 1, "status": "verdict", "host": "local", "attempt": 1}
        rep = {"fields": dict(fields, **(over or {})), "divergent": [], "extra": list(new), "identity": [],
               "repeat-rows": [], "crashed": [], "missing": []}
        rep.update(lists)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            errs = co.accept_new_rows(job, rep, co.check_report(job, rep))
        return errs, job
    errs, job = run()
    co.jobs = [job]
    summary = io.StringIO()
    with contextlib.redirect_stdout(summary):
        co.print_summary(co.run_rc())
    s = summary.getvalue()
    sys.stdout.write(s)
    errs_d, job_d = run(divergent=[TILES[0]])
    errs_i, job_i = run(identity=["zz-new__ident__1-vk.png"])
    errs_c, job_c = run(crashed=[{"leg": "x", "rc": "139", "retry": None}])
    co.a.filter = None
    errs_u, job_u = run()
    co.a.filter = "zz-new"
    errs_h, job_h = run({"blessed-sha256": "0" * 64})
    check("new-rows", errs == [] and job.result["rc"] == 0 and job.result["status"] == "NEW-ROWS 2 tile(s)"
          and "result spyro-2-presentation: NEW-ROWS on local (rc 0" in s
          and "NEW-ROWS 2 tile(s) with no blessed row (promote them from" in s and new[0] in s
          and "run test-new-rows: PASS (exit 0)" in s
          and job_d.result["rc"] == 1 and any("compared nothing" in e for e in errs_d)
          and job_i.result["rc"] == 1 and job_c.result["rc"] == 1
          and job_u.result["rc"] == 1 and errs_u and job_h.result["rc"] == 1
          and errs_h and not any("compared nothing" in e for e in errs_h),
          f"extras only: refusals {errs}, {job.result['status']} rc {job.result['rc']}; with a divergence: "
          f"rc {job_d.result['rc']} {errs_d}; identity failure rc {job_i.result['rc']}; crash rc "
          f"{job_c.result['rc']}; unfiltered rc {job_u.result['rc']} {errs_u}; manifest mismatch rc "
          f"{job_h.result['rc']} {errs_h}")


def case_expect_variant(tmp):
    """An expect line naming a base row covers its `@<variant>` rows' tiles (bk2 and freecam keys); the preflight count names the variants."""
    import contextlib
    import io
    fc_var = "ocean_speedway@fogorig-9270a9__canon__2473-vk.png"
    fc_lod = "ocean_speedway@fogorig-9270a9__lodext__inter__2523-vk.png"
    bk_var = "attract-demo@heldfade__canon__100-vk.png"
    copy = "ocean_speedway@fogorig-9270a9~r02__canon__2473-vk.png"
    other = "ocean_speedway@fogorig-c96d7a__canon__2473-vk.png"
    e_fc = gf.Expectation(UNIT, "ocean_speedway-9270a9", "base row", 1)
    e_bk = gf.Expectation(UNIT, "attract-demo__canon", "base row's canon leg", 2)
    hits = (e_fc.hit(fc_var), e_fc.hit(fc_lod), e_fc.hit(copy), e_fc.hit(other), e_bk.hit(bk_var),
            e_bk.hit("attract-demo__inter__100-vk.png"))
    lines = [rec("DIVERGE", fc_var), rec("END", "END")]
    c, _r = gf.expect_counts([e_fc], {UNIT: (True, lines, set())})
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        errs = gf.check_expect([e_fc], {UNIT}, {UNIT: ({"ocean_speedway-9270a9__canon__2473-vk.png", fc_var, other},
                                                       {"ocean_speedway", "ocean_speedway@fogorig"})})
    msg = out.getvalue()
    sys.stdout.write(msg)
    check("expect-variant", hits == (True, True, True, False, True, False) and c["EXPECTED"] == 1
          and c["UNEXPECTED"] == 0 and errs == [] and "2 tile(s)" in msg
          and "covers variant row(s) ocean_speedway@fogorig-9270a9" in msg and "c96d7a" not in msg,
          f"hits (freecam variant, its lodext leg, its repeat copy, a variant at another pose, bk2 variant, "
          f"the bk2 inter leg) {hits}; counts {c}; preflight line: {msg.strip()}")


def main():
    """Run every case in a temporary directory with the ledger redirected."""
    tmp = tempfile.mkdtemp(prefix="fleet-stream-check.")
    gf.ledger_path = lambda: os.path.join(tmp, "ledger.tsv")
    try:
        for case in (case_framing, case_pool, case_stream_off, case_equality, case_crashed, case_extra, case_drain, case_exempt,
                     case_kill, case_stop, case_stop_cli, case_stop_sync, case_reap, case_transport, case_clean,
                     case_expect, case_canary_verdict, case_run_candidate, case_move, case_move_cli,
                     case_keep_and_resume_inputs, case_keep_exhausted, case_ledger, case_repeat,
                     case_sync_base, case_on_hosts,
                     case_takeover_rule, case_takeover, case_takeover_slow, case_takeover_race,
                     case_host_wait, case_local_busy, case_record_every_end, case_new_rows,
                     case_expect_variant):
            case(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if FAILS:
        print(f"[fleet-stream-check] FAIL: {', '.join(FAILS)}")
        return 1
    print("[fleet-stream-check] PASS: every case")
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(line_buffering=True)
    sys.exit(main())
