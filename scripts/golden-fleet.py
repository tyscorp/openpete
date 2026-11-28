#!/usr/bin/env python3
"""golden-fleet.py -- place presentation-golden units on a fleet of hosts.

    golden-fleet.py status [--hosts a,b]
    golden-fleet.py preflight [--hosts a,b] [--need-gib N] [--no-calibration]
    golden-fleet.py ship-image --hosts a,b
    golden-fleet.py add local [--dry-run] [--timings FILE]
    golden-fleet.py add <name> --ssh user@host [--engine podman|docker]
                    [--fleet-root DIR] [--dry-run] [--timings FILE]
    golden-fleet.py calibrate [--hosts a,b] [--dry-run]
    golden-fleet.py run [--game G | --all] [--tier T | --all-tiers]
                    [--hosts a,b | --no-local] [--pull divergent|all] [--wait]
                    [--expect FILE] [--filter S] [--freecam-filter S] [--repeat N]
                    [--no-takeover] [--commits RANGE] [--queries DIR]
                    [--host-wait S | --no-host-wait] [--local-anyway]
    golden-fleet.py stop <run-id> [--unit U]
    golden-fleet.py move <run-id> --unit U --to <host> [--no-wait]
    golden-fleet.py resume <run-id> --unit U --to <host> [--from DIR]
                    [--pull divergent|all] [--wait]
    golden-fleet.py clean [--hosts a,b] [--tree PATH | --all]

Host file. $OPENPETE_FLEET_HOSTS, else `${XDG_CONFIG_HOME:-~/.config}/
openpete/fleet-hosts.toml`, the main checkout resolved through `git rev-parse
--git-common-dir` so a worktree resolves the same file. One table per host:

    [hosts.<name>]
    transport = "local" | "ssh"     # required; `local` is the coordinator
    ssh = "user@host"               # required for transport "ssh"
    fleet_root = "~/.cache/openpete-fleet"   # every fleet file on the host
    engine = "podman" | "docker"    # default: the first found on the host
    slots = N                       # CORPUS_JOBS on the host, measured by add
    speed = F                       # throughput relative to local (1.00)
    calib_s = N                     # calibration unit's wall time at slots (s)
    calibrated = "YYYY-MM-DD governor=G cpu=MODEL"
    enabled = true                  # false: never used
    default = true                  # in every run unless opted out
    build = true                    # may build the engine for a run
    nice = 19                       # CPU priority of units on the host

Only `add` and `calibrate` write the file: it replaces the named host's table and keeps
every other line. A missing host file makes status and preflight exit 64
naming the `add` command; a malformed one exits 1 naming the line or key.

status: per host, reachability (ssh with ConnectTimeout=5 and
keepalives), engine, the Id of openpete-linux-capture:fleet beside the
local image's Id, `<fleet_root>/image.id`, the golden lock (tried
non-blocking; the holder record the capture wrapper writes into it),
governor, calibration, free space and the size of fleet_root, and each
tree under trees/: a repository base's size, age since its last refresh,
repository and last checkout path; a checkout-keyed tree's size, age and
path.

preflight: drops a host that is disabled, unreachable, without an engine,
uncalibrated, running a governor other than the calibrated one, short of
free space (--need-gib, plus the image size when the image must ship), or
whose golden lock is held; on the local host the slots are lowered to fit
2 GiB of MemAvailable per slot, and the host is dropped when none fits.
Each drop prints its reason. Exit 0 with a usable host, 64 with none.

ship-image: when a host's openpete-linux-capture:fleet Id differs from the
local image's, streams `<engine> save` into `ssh host <engine> load`, tags
the loaded Id :fleet and writes <fleet_root>/image.id. No label is added,
since a label changes the image config and so its Id.

add: measures a host with run's machinery (steps 1-3 below: preflight
without the calibration requirement, the host's clean and image ship, the
build on the local host and the positive sync set into
runs/<run-id>/tree on each measured host, and each leg's detached
supervisor, under the host golden lock on an ssh host and the coordinator's
on the local one). Each leg is the spyro-2 presentation unit (capture, then
--verdict-only) at one slots value, with LP_NUM_THREADS = ceil(cores /
slots) and MemAvailable sampled at 1 Hz on the host; a leg whose report
compares fewer than every blessed row or lists a missing tile ends the add.
The sweep runs slots 1, 2, 4, ... up to the host's core count and stops at
the first step improving under 10% or a leg whose minimum MemAvailable is
under 2 GiB. The rule: slots is the smallest sweep value past which
wall-clock improves by less than 10%, lowered while the minimum
MemAvailable during its leg is under 2 GiB. calib_s is the leg's wall time
at that slots. For an ssh host, speed = t_local / t_host, t_local being the
local host's calib_s; when the local host has none, a leg at its recorded
slots runs beside the sweep and its calib_s is written too. A local leg
counts only with this machine idle: the 1-minute load average at most
max(1, 0.1 x cores) before the leg (waited for up to 600 s) and, 90 s after
it ends, at most that bound plus a quarter of the load at its end; a
loaded leg is discarded and re-run, and the third loaded leg at one slots
value ends the add (64). `add local` records speed 1.00. The host's table
is then written with slots, speed, calib_s and calibrated (date, governor,
CPU model), and the add's wall time is printed.

--dry-run preflights and prints the build host and every planned leg, and
writes nothing. --timings FILE replaces the measurement with recorded legs
(JSON), ships no image and runs nothing:

    {"cores": 8,
     "sweep": [{"slots": 1, "wall_s": 1500, "min_memavail_kib": 9000000}, ...],
     "local": [{"slots": 4, "wall_s": 270, "load_before": 0.4,
                "load_end": 22.0, "load_after": 5.1}, ...]}

"local" is optional; without it t_local is the local host's calib_s.

calibrate: runs one leg at each host's recorded slots (--hosts, else every
enabled host; the local one under the idle rule) through the same
machinery, in parallel across hosts, and rewrites speed, calib_s and
calibrated; the governor is re-recorded, so a governor change is cleared by
calibrate. A leg under 2 GiB MemAvailable prints that `add` re-measures
slots. t_local is this calibration's local leg, else the local calib_s.

run: gates this checkout's working copy on the fleet and never blesses or
promotes. It detaches (setsid; the log and done marker are
<local fleet_root>/runs/<run-id>/coordinator.log and done, `rc=N` on the
first line of done); --wait follows it and exits with its code. The
coordinator:
  1. selects hosts (--hosts, else every enabled host with default = true,
     minus local under --no-local), preflights them, takes the local golden
     lock, runs clean on each host under its golden lock and ships the image.
     The local host is left out, as under --no-local and printed with the
     reason, while a landing holds land-train.lock, land-branch.sh,
     land-train.sh or sched_gate.py runs, a game process outside the fleet's
     containers runs, or MemAvailable is under one slot plus a 4 GiB
     reserve; --local-anyway keeps it. When no host is usable and a host is
     dropped only for another holder's golden lock or a busy local host,
     preflight re-runs every 60 s for up to --host-wait S (default 21600),
     printing the holders; --no-host-wait exits 64 "no usable host" at once;
  2. builds on the fastest host with build = true: in the checkout through
     the wrapper's `build` when that is the local host, else in the build
     host's snapshot, whose build tree then reaches the other hosts through
     this machine;
  3. syncs the positive set (git's tracked and non-ignored untracked files,
     submodules included; games/*/generated, the manifest globals and
     symbols, each run game's disc cue and bin, third_party/psxrecomp/build;
     the build tree minus home/) to <fleet_root>/runs/<run-id>/tree on every
     host, the hosts concurrently, with --checksum --link-dest from the
     repository base <fleet_root>/trees/<key> (<key>: the coordinator's
     hostname and the repository's git common directory, so every checkout
     of the repository shares it; a file whose content equals the base's is
     not sent; the base is linked from only when its record names this
     coordinator and repository, and is read under trees/<key>.lock held
     shared), then re-checks every copy with rsync -n --checksum over the
     same lists and refuses the run as torn on any change, then refreshes
     each host's base from its snapshot (hard links, renamed over the base
     under the lock held exclusive). While it reads the checkout it holds the checkout's
     build/linux-release-container.build-lock and runs a `golden-fleet-sync`
     marker process, which a build lock can test for; it then hashes the
     synced disc bytes against the blessed headers;
  4. places units longest first (ledger fleet-unit-times.tsv beside the
     store, one row per accepted unit: norm_s is wall x speed summed over
     the accepted attempt and the earlier attempts of this run whose
     stamped rows it imported (same host, or carried by a move), host the
     chain `a>b`; a filtered run's unit, a resumed unit and an import from
     outside the run write no row; a unit without a record is scaled from
     the spyro-1 presentation unit's 60 min by candidate size) on free hosts, one per
     host, each under a detached supervisor holding the host golden lock,
     with CORPUS_JOBS = slots, PRESENTATION_LEG_TIMEOUT = 300 / speed x 1.5 and the deadline
     expected time / speed x 2; a lost, dead or overdue attempt is killed
     (supervisor by PID and start time, containers by the label
     org.openpete.fleet=<run-id>) and re-queued once the golden lock is free,
     at most twice; a host unreachable for 10 min is dropped. A unit or
     canary runs with OPENPETE_RESUME_RUN=<run-id>: every manifest row it
     finishes is stamped (scripts/corpus-resume.sh), and a re-queued attempt
     on the same host imports the stamped rows whose run, tree, binary,
     manifest line and tile digests match, deletes the rest of the previous
     attempt's output and captures only the other rows (`move` and `resume`
     carry the stamped rows to another host); a free host with no queued
     job takes over the running unit predicted to finish last (Takeover,
     below), unless --no-takeover;
  5. runs a canary (one bk2 and one freecam level of the first unit's game)
     on every host first when two or more hosts capture, and refuses the run
     when a canary tile's digest differs between hosts; at the run's end
     writes <local run dir>/canary-verdict.txt: `run=<run-id>`, `canary=on`
     or `canary=off (<reason>)`, and one `canary.<host>=pass <detail>` or
     `canary.<host>=fail <reason>` line per host whose canary was placed (a
     host passes when its canary captured and every canary tile is identical
     on two or more hosts);
  6. decides each unit on its capturing host (the harness's capture, then
     --verdict-only in the image), pulls verdict-report.txt, the capture
     record, capture-device.txt, credits-verdict.txt, the divergent and
     extra tiles (an extra tile is a new row's, with no blessed digest)
     and both members of each identity pair over baseline (identity/), and
     on the presentation tier tile-digests.tsv, row-cost.tsv,
     census-settle.tsv (each leg's frozen-backdrop settle count and tally
     pass time, printed per unit) and, by a
     second rsync with an include pattern under either --pull mode, every
     tile's `*-vk.census` sidecar (scripts/golden-census.py; a captured
     tile without one refuses the unit), into
     the cleared local candidate directory (build/presentation-candidate and
     its per-game and dragon forms, or PRESENTATION_GOLDEN_OUT), refuses a
     unit whose report's blessed.tsv or identity-baseline hash or census
     differs from the local files, appends fleet-run=<run-id>,
     fleet-host=<host> and fleet-run-dir=<local run dir> to the pulled
     capture record (presentation-blessed.py promote prints them), prints
     the report's census (captured,
     imported and compared tiles), and renders heat diffs locally
     (presentation-corpus-diff.py --expect-subset); an accepted pull is then
     copied (hard links) into build/fleet-stream/<run-id>/<unit>/, the run's
     own candidate, which the next run or local capture does not clear
     (streamed tiles the pull does not hold move to its stream-only/), and
     the coordinator prints the promote command for it:
     tests/harness/presentation-corpus-promote.sh
     build/fleet-stream/<run-id>/<unit> <tile>... (game and tier read from
     its capture record); each unit's row-cost.tsv is merged into the
     uncommitted ~/.cache/openpete-fleet/row-cost.tsv (game, row, seconds,
     host, slots, speed, run id; the newest kept); an accepted
     presentation unit of a run whose run-args.json records no filter,
     freecam_filter, rows or repeat, not stopped, whose every sidecar names
     the capture record's source tree, is hard-linked (sidecars,
     tile-digests.tsv and census-settle.tsv) into the census store's
     <local fleet_root>/census/<game>-<tier>/<run-id>.part/, which drive()
     finalizes after the done marker (moved.txt from stream.log, copies of
     run-args.json and done, entry.json last, renamed to <run-id>/) and
     prunes to the unit's two newest entries, removing each .part older than
     24 h; a resume finalizes at its own done marker
     (scripts/golden-census.py reads the store);
  7. pulls every attempt's crashed capture legs (u-<unit>/cand.crashes, the
     harness's crash directory: leg log, argv, exit code, hang and core
     dumps) from every touched host into <local run dir>/crashes/<host>/
     and names each kept leg in the coordinator log, then deletes
     runs/<run-id>/ on every host on every exit path (the local directory
     keeps its log, done marker, stream.log, expect.txt, queries/,
     stop-request, canary-verdict.txt and crashes/).
Stream: a unit's supervisor sets OPENPETE_STREAM_FILE=<attempt base>.stream;
each finished row appends one SAME|DIVERGE|EXTRA|IDENT|EXCEED line per tile
(classified by the end verdict's functions), the credits leg `CREDITS rc=N`,
each retried crashed capture leg `CRASHED <leg> rc=N retry=pass|fail`
(printed as it arrives), imported rows their lines tagged `imported`, and
the capture's verdict `END`.
Each poll returns the done marker, then the stream's new bytes from the
attempt's offset (POLL_SCRIPT); complete lines go to <local run
dir>/stream.log tagged with unit, host, attempt and receipt time; at most
STREAM_K divergent and extra tiles and identity pairs per poll are pulled
into build/fleet-stream/<run-id>/<unit>/ and the heat diffs of the divergent
tiles and pairs rendered. At unit end, when the accepted attempt's stream
holds END, SAME + DIVERGE must equal the report's compared count, the
DIVERGE, EXTRA, EXCEED and CREDITS sets the report's, and every DIVERGE and
EXTRA tile and both members of every EXCEED pair must be in the unit's
stream directory; a mismatch sets the unit's rc to 1 (`stream mismatch`).
Stopped and SKIP units and attempts without END are exempt.
--filter S / --freecam-filter S: each unit captures only the bk2 rows whose
name contains S, the freecam rows whose name field or tile key
(`<level>-<pose md5 prefix>`) contains the freecam substring (unset: S),
and the credits leg when `credits-interp` contains S; with --freecam-filter
alone the bk2 rows and the credits leg run none
(tests/harness/check_presentation_golden.sh, --filter); the unit is decided
by its capture's own verdict (out-of-scope tiles are not missing) instead of --verdict-only, and
the report's compared count must be nonzero instead of
equal to the manifest's rows. A partial run decides nothing for landing.
--repeat N (with a filter): each unit captures every row its filters select
N times as repeat copies (check_presentation_golden.sh --repeat=N), queued
on its one host at the host's slots and LP_NUM_THREADS, and compares every
copy's tiles and identity pairs against the original row's blessed digest
and identity baseline row; a matching copy is SAME, not EXTRA. The unit's
report must carry `repeat: N` and repeat-compared at least repeat-legs (N x
the selected rows' blessed tiles plus one identity pair per canon tile),
else the unit is refused (engagement); the unit's result line names each
row's divergent count of N x legs. The repeat count is recorded in
run-args.json, so `resume` repeats the same N. run-args.json also records
the checkout's HEAD and source tree, the run's games and units, --commits
RANGE (the batch's commit range) and, under --queries DIR, the sha256 of
every file of DIR, all written before any leg starts: `run` copies DIR
into <local run dir>/queries/ before detaching and records the copy, which
the local run directory keeps (scripts/golden-census.py score refuses a
file whose hash differs).
--expect FILE: `<unit> <substring> <reason>` lines (unit = job name); a
unit not in the run or a substring matching no blessed tile, no blessed canon
tile's `__ident__` twin and no manifest row refuses the run (2). A tile of a row whose launch field sets extended
LOD (`--lod extended`, `--no-lod`) also matches as its `__lodext__`-tagged
name, since both of that row's LOD legs run extended. Streamed moves are tagged EXPECTED or UNEXPECTED;
each expectation ends moved, EXPECTED-NOT-MOVED or NOT-REACHED; the counts
change no exit code.
Exit (and done marker rc): the worst unit in the order 1 > 64 > 0.
Negative-control knobs: OPENPETE_FLEET_AFTER_SYNC=<shell command> runs in
the checkout between the syncs and the recheck; OPENPETE_FLEET_POKE=
<host>:<job|unit|canary>:<tile substring> flips a pixel of the first
matching candidate tile on that host before its verdict. Resume test knobs:
OPENPETE_FLEET_DEADLINE_S=<seconds> replaces the deadline of every unit's
and canary's first attempt; OPENPETE_FLEET_CANARY_ONLY=1 runs only the
canaries, one per host even on one host, pulls each canary's tiles and
report into <candidate directory>/canary-<host>, and exits with the worst
canary's code. OPENPETE_FLEET_ROW_POKE=<host>:<job|unit>:<tile substring>
changes the matching tiles of that unit as their leg finishes, before the
leg hashes them, so the row streams DIVERGE; OPENPETE_STREAM_DROP=<tile>
discards the first stream line naming the tile (unit-end check control);
OPENPETE_FLEET_STREAM=0 runs units without the stream (wall-time baseline).

stop: writes <local run dir>/stop-request (`unit=<job>`, or `all` without
--unit) atomically; refused (1) once the run's done marker exists. The
coordinator reads it once per loop iteration: a named running or pending
job gets rc 1, `stopped by user`, no ledger row, before its kill, so it is
not re-queued; a job whose done marker was read first keeps its result.
Before the legs (preflight, image ship, build, sync, recheck, base refresh)
a request naming `all` or every unit is polled every STOP_POLL_S while a
child runs: the child's process group is killed (SIGTERM, SIGKILL after
STOP_KILL_GRACE_S) and every unit is recorded stopped before it started. A
running run's remote commands carry OPENPETE_FLEET_RUN=<run id>; teardown
signals every remote process still carrying it (REAP_SCRIPT).
The run is FAIL with reason=stopped by user. At the run's end the stamped
rows (resume/*.stamp and the files they name, from the last attempt's host)
of each stopped unit and each unit that ended `attempts exhausted` or `host
lost`, the ship directory and, with the local host in the run, its
snapshot (hard links) are kept in <local run dir>/kept/ (u-<unit>/cand,
u-<unit>/host naming the capturing host, ship/, tree/) beside canary/, for
`resume`; clean removes them with the run
directory.

move: appends `id=<id> unit=<job> to=<host>` to <local run dir>/move-request
and waits for the coordinator's outcome line in moves.log (exit 0 accepted,
1 refused; --no-wait returns at once); refused (1) once the run's done marker
exists. One host per move: a unit is one capture on one host. The coordinator
reads the request once per loop iteration after the legs start and:
  1. refuses when the unit is unknown, stopped, finished, already on the
     host, or the host was dropped from the run or is busy with another job;
  2. admits the host on the run-start terms (preflight, its golden lock,
     clean, image ship) and refuses with the preflight's reasons (unreachable,
     golden lock held, free space, governor, calibration); a host not in the
     run joins it: its snapshot is copied from the local host's snapshot when
     the local host is in the run, else from another host's snapshot pulled
     into <local run dir>/join-<host> over the run's sync lists (unchanged
     files copied from the checkout or the build relay) and rechecked against
     that host with rsync -c; the joiner's copy is rechecked against the
     source, its binaries against ship/binaries.sha256 and its disc against
     the blessed header, and its base refreshed. A joiner and every run host
     without a canary get one, compared at the run's end as at run start;
  3. stops the unit's running attempt (an attempt that finished first, or
     whose done marker appears while the stop is in flight, keeps its result
     and the move is refused); its rows in flight are lost, every
     stamped row is kept. The stopped attempt does not count against
     MAX_ATTEMPTS;
  4. copies that attempt's stamps and the files they name into
     u-<unit>/cand on the host (through this machine for a remote source)
     and re-queues the unit first, pinned to the host. The new attempt's
     import accepts them under the importer's own checks (run, tree, binary,
     manifest line, tile digests) and streams them tagged `imported`, so the
     unit's verdict, stream check and expected-move tags cover every row. A
     moved unit's ledger row sums its attempts (step 4); its result line
     names each move. A
     moved unit whose pinned host is later dropped is unpinned.

Takeover: each loop iteration, a free, undropped host that no queued job
fits looks at the running units of the other hosts. With done = the
running attempt's streamed SAME and DIVERGE tiles (imported included),
total = the unit's blessed tiles, left = 1 - done/total, rows = its corpus
rows, E = the ledger's expected time at speed 1.00, B = its candidate
bytes, s_a and slots_a the running host's speed and slots, s_b the free
host's speed and hops the carry's transfers (1 per remote host of the two):
  remaining on a = left x E / s_a - E x min(slots_a, left x rows) / (2 x rows x s_a)
  cost           = TAKEOVER_FIXED_S (120) + hops x (1 - left) x B / CARRY_BPS (10 MiB/s)
  remaining on b = left x E / s_b + cost
  saving         = remaining on a - remaining on b
(the subtracted term is the half-done rows in flight on a, which b
recaptures). The unit with the largest remaining on a is taken over through
`move` when s_b > s_a, remaining on a exceeds the cost and the saving
exceeds TAKEOVER_MIN_SAVING_S (600 s); a refusal is logged once per free
host and attempt. A taken-over unit is taken over again only by the same
test (so only by a faster host). A unit whose done marker appears while
its stop is in flight keeps that attempt's result and nothing is carried.
Each takeover logs the unit, both hosts, the prediction and the rows
carried; the summary prints each one's measured time on the receiving host
and its saving against the predicted remaining time on the source. Off
under --no-takeover, a filter, OPENPETE_FLEET_STREAM=0, CANARY_ONLY,
resume and calibrate.

resume: finishes a unit whose run is done (a stop, lost host or exhausted
attempts; refused when its last logged result is a verdict, while the run's
coordinator holds active.lock, or without a done marker). It detaches a
coordinator for the same run id (log coordinator-resume-<n>.log in the run
directory; the done marker becomes done.<n> and the stop request
stop-request.<n>), which:
  1. takes the unit filters from --filter/--freecam-filter, else the run's
     run-args.json (written at run start), else none, and the run's --repeat
     from run-args.json; reads the saved rows
     from --from DIR (the unit directory itself, or a
     directory holding u-<unit>/) or <run dir>/kept, the fields file
     (ship/fields-<game>.txt, else the unit's fields.txt) and
     ship/binaries.sha256 from DIR, beside it or in the unit directory, and
     canary tiles from DIR/canary or <run dir>/canary;
  2. admits the host as `move` does and gives it the run's snapshot: the
     kept tree/ (its binaries checked against the saved hashes), else this
     checkout, built as `run` builds it and refused unless its fields file
     and binary hashes equal the saved ones, since no row would import;
  3. copies the saved stamps and stamped files into u-<unit>/cand there and
     runs the unit (pinned); when the host is not the one that captured the
     rows (the saved unit directory's `host` file, which kept/ carries, else
     the last host the run's logs started the unit on), a canary runs
     first and its tiles are compared with the kept canary tiles of that host,
     merged into canary-verdict.txt as canary.<host>;
  4. pulls and checks the unit as `run` does and writes the run's done
     marker: the worst over this unit, its canary and every other unit's last
     logged result.

clean: per host, under its golden lock (a busy host is skipped): removes
runs/ directories no process holds (active.lock) and with no done marker
newer than 24 h, signalling the dbus-daemon helpers their supervisors
recorded when PID and start time still match; exited fleet-labelled
containers; trees/<key> this coordinator wrote whose checkout path is gone
or unused for 14 days; images the fleet shipped (image.id and its history)
other than the current :fleet one and named by no other tag. --tree PATH
removes only this coordinator's trees of PATH; the census store (census/)
is touched only by --all. --all removes runs/, trees/, census/, the lock
and image records, every fleet-labelled container and the :fleet
tag (the image itself only when no other tag names it), and fleet_root
itself when nothing else is in it. On the local host it also removes the
checkout's build/fleet-stream/<run-id>/ directories whose run directory is
gone.

clean, trees: a tree whose record names repo= is the repository base;
"checkout path" above reads as its repository (git common directory) for
staleness and 14 days count from its last refresh; --tree never removes
it, since every checkout of the repository shares it. A tree whose record
has no repo= was keyed on a checkout path by an earlier coordinator. A
tree whose lock a sync holds is kept.
"""

import argparse
import datetime
import fcntl
import fnmatch
import glob
import hashlib
import importlib.util
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time

IMAGE_TAG = "openpete-linux-capture:fleet"
DEFAULT_FLEET_ROOT = "~/.cache/openpete-fleet"
CALIBRATION_GAME = "spyro-2"
GIB_KIB = 1024 * 1024
MEM_PER_SLOT_KIB = 2 * GIB_KIB
# The local host takes a unit only with one slot's memory left over this
# reserve for the processes outside the fleet (the desktop, builds, agents).
LOCAL_MEM_RESERVE_KIB = 4 * GIB_KIB
# A run whose every host is dropped for a transient reason (another holder
# of a golden lock, a busy local host) re-runs preflight every
# HOST_WAIT_POLL_S until a host is usable or --host-wait S elapses.
HOST_WAIT_S = 6 * 3600
HOST_WAIT_POLL_S = 60
WAIT_REASONS = ("golden lock held", "golden lock taken since preflight", "local busy")
# Local processes that make the local host busy: a landing (also detected by
# its lock, land-train.lock in the checkout's git common directory) and the
# scheduler gate; and a game process outside the fleet's containers.
LAND_LOCK_NAME = "land-train.lock"
BUSY_SCRIPTS = ("land-branch.sh", "land-train.sh", "sched_gate.py")
GAME_EXE_RE = re.compile(r"(^|/)openpete(-spyro[0-9])?(\.exe)?$")
CONTAINER_CGROUP_RE = re.compile(r"libpod|docker|containerd")
FLEET_PARENTS = ("conmon", "supervise.sh")   # a container's monitor, the fleet unit supervisor
IMPROVEMENT_FLOOR = 0.10
IDLE_SETTLE_S = 90
IDLE_RETRIES = 3
SSH_OPTS = ["-o", "ConnectTimeout=5", "-o", "ServerAliveInterval=15",
            "-o", "ServerAliveCountMax=4", "-o", "BatchMode=yes"]
EXIT_SKIP = 64
CALIB_NEED_BYTES = 2 * 2**30  # calibration candidate set (194 MB) and store, with margin

GAMES = ("spyro-1", "spyro-2", "spyro-3")
TIER_GAMES = {"presentation": GAMES, "dragons": ("spyro-1",)}
# Candidate-set size per unit (MB, dev box); a unit with no recorded time gets
# the spyro-1 presentation unit's REF_UNIT_MIN scaled by size.
UNIT_MB = {("spyro-1", "presentation"): 1100, ("spyro-1", "dragons"): 1900,
           ("spyro-2", "presentation"): 194, ("spyro-3", "presentation"): 324}
REF_UNIT = ("spyro-1", "presentation")
REF_UNIT_MIN = 60.0
LEG_TIMEOUT_S = 300           # the harness's default leg timeout
CANARY_DEADLINE_S = 15 * 60
BUILD_DEADLINE_S = 90 * 60
BUILD_TREE = "build/linux-release-container"
BUILD_TREE_BYTES = 1400 * 10**6
# One bk2 and one freecam level per game, run with --filter/--freecam-filter.
CANARY = {"spyro-1": ("alban-rescue", "gnastys_loot"),
          "spyro-2": ("shady-oasis-flame", "canyon_speedway"),
          "spyro-3": ("crystal-islands-flame", "molten_crater")}
CREDITS_NAME = "credits-verdict.txt"
LEDGER_COLS = ("date", "run", "game", "tier", "host", "speed", "wall_s", "norm_s", "cand_bytes")
POLL_S = 20
START_GRACE_S = 120
GRACE_S = 600
MAX_ATTEMPTS = 2
# Takeover (a free host with no queued job takes over the running unit
# predicted to finish last; Coordinator.take_over). The fixed cost is the kill
# (KILL_SCRIPT's 10 s TERM grace and the container removal), the receiving
# host's preflight, the launch and container start, and the importer's
# digest check of the carried tiles; the receiving host already holds the
# run's snapshot, so no sync is owed. The carry runs at CARRY_BPS per hop
# (the coordinator relays a remote source's rows, so remote -> remote is two
# hops), the rate of the run-start sync to a remote host.
TAKEOVER_FIXED_S = 120
CARRY_BPS = 10 * 2**20
# The saving a takeover must predict: the ledger's times of one unit on the
# shared local host vary by tens of percent with user load, so a saving
# under 10 min is within the error of the prediction it rests on, and a
# takeover that does not pay costs the run its fixed cost and the source's
# rows in flight.
TAKEOVER_MIN_SAVING_S = 600
TREE_MAX_AGE_S = 14 * 86400
# Files of the coordinator's local run directory that outlive the run.
RUN_KEEP = ("coordinator.log", "done", "active.lock", "stream.log", "expect.txt", "stop-request",
            "crashes", "canary-verdict.txt", "canary", "kept", "move-request", "moves.log", "run-args.json",
            "queries")
# Name prefixes of further kept files: a resume's log and the done markers it replaced.
RUN_KEEP_PREFIX = ("coordinator-resume-", "done.", "stop-request.", "move-request.")
CANARY_VERDICT = "canary-verdict.txt"
RECORD_NAME = "capture-record.txt"
FLEET_RECORD_KEYS = ("fleet-run", "fleet-host", "fleet-run-dir")
STREAM_K = 20                 # divergent and extra tiles and identity pairs pulled per poll per unit
STREAM_DIR = os.path.join("build", "fleet-stream")
TILE_VERBS = ("SAME", "DIVERGE", "EXTRA", "IDENT", "EXCEED")
MOVED_VERBS = ("DIVERGE", "EXTRA", "EXCEED")
SIDECAR_GLOB = "*-vk.census"      # tile-census sidecars (scripts/golden-census.py)
DIGESTS_NAME = "tile-digests.tsv"  # every captured tile's digest, written by the verdict step
ROW_COST_NAME = "row-cost.tsv"     # per-leg capture seconds, written by the corpus scripts
SETTLE_NAME = "census-settle.tsv"  # per-leg backdrop settle tally cost (scripts/golden-census.py settles)
ROW_COST_CACHE = os.path.expanduser("~/.cache/openpete-fleet/row-cost.tsv")
CENSUS_STORE = "census"           # <local fleet_root>/census/<unit>/<run-id>/ (scripts/golden-census.py)
CENSUS_FILTER_KEYS = ("filter", "freecam_filter", "rows", "repeat")   # run-args keys of a filtered run
CENSUS_KEEP = 2                   # finalized entries kept per unit
CENSUS_PART_MAX_AGE_S = 24 * 3600 # an unfinished entry older than this is removed
STREAM_ONLY_DIR = "stream-only"  # streamed tiles the accepted pull does not hold
STOP_STATUS = "stopped by user"
COMPARED_NOTHING = "the filtered unit compared nothing"
UNFINISHED_STATUS = ("attempts exhausted", "host lost")   # requeue's results of a unit out of attempts

HOST_KEYS = {
    "transport": str, "ssh": str, "fleet_root": str, "engine": str,
    "slots": int, "speed": float, "calib_s": int, "calibrated": str, "enabled": bool,
    "default": bool, "build": bool, "nice": int,
}
KEY_ORDER = ["transport", "ssh", "fleet_root", "engine", "slots", "speed", "calib_s",
             "calibrated", "enabled", "default", "build", "nice"]


class FleetError(Exception):
    """An error that ends the command with a message and an exit code."""

    def __init__(self, msg, code=1):
        """Store the message and the exit code."""
        super().__init__(msg)
        self.code = code


class StopRequested(FleetError):
    """A stop request covering every unit arrived before the run's legs started."""


# ---- child processes ----------------------------------------------------------

STOP_POLL_S = 1.0             # stop-request poll interval while a child runs before the legs
STOP_KILL_GRACE_S = 5         # SIGTERM to SIGKILL of a child's process group
RUN_TAG_VAR = "OPENPETE_FLEET_RUN"

# While a run is outside its leg loop, its stop predicate: run_proc polls it
# every STOP_POLL_S while a child runs and before starting one. None otherwise.
_stop_watch = None
# While a run executes, its run id: every remote command (ssh scripts, image
# loads, rsync servers) carries RUN_TAG_VAR=<run id> in its environment, which
# REAP_SCRIPT finds. None otherwise.
_run_tag = None


class stop_watch:
    """Context manager installing (or, with None, suspending) the stop predicate run_proc polls."""

    def __init__(self, pred):
        """Bind the predicate; nothing is installed yet."""
        self.pred, self.prev = pred, None

    def __enter__(self):
        """Install the predicate, keeping the previous one."""
        global _stop_watch
        self.prev, _stop_watch = _stop_watch, self.pred
        return self

    def __exit__(self, *_exc):
        """Restore the previous predicate."""
        global _stop_watch
        _stop_watch = self.prev
        return False


class run_tag:
    """Context manager setting the run id remote commands carry in RUN_TAG_VAR."""

    def __init__(self, run_id):
        """Bind the run id; nothing is set yet."""
        self.run_id, self.prev = run_id, None

    def __enter__(self):
        """Set the tag, keeping the previous one."""
        global _run_tag
        self.prev, _run_tag = _run_tag, self.run_id
        return self

    def __exit__(self, *_exc):
        """Restore the previous tag."""
        global _run_tag
        _run_tag = self.prev
        return False


def stop_point():
    """Raise StopRequested when the installed stop predicate holds."""
    if _stop_watch is not None and _stop_watch():
        raise StopRequested(STOP_STATUS)


def kill_group(proc):
    """End a child started with start_new_session: SIGTERM its process group, SIGKILL after STOP_KILL_GRACE_S; reap it."""
    if proc.poll() is None:
        try:
            os.killpg(proc.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            proc.wait(STOP_KILL_GRACE_S)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.wait()
    try:
        proc.communicate(timeout=STOP_KILL_GRACE_S)
    except (subprocess.TimeoutExpired, ValueError, OSError):
        pass


def run_proc(argv, input=None, timeout=None, text=False, cwd=None, env=None, stdin=None,
             stdout=subprocess.PIPE, stderr=subprocess.PIPE):
    """Run argv to completion as subprocess.run with capture does, in its own process group.

    The child's group (rsync's ssh, a script's children) is killed by
    kill_group when the timeout expires (TimeoutExpired is raised), when any
    exception (a signal turned into Interrupted) leaves this function, and,
    while a stop predicate is installed, when it holds at a STOP_POLL_S poll
    (StopRequested is raised). stdin is DEVNULL without input or stdin.
    @param argv the command
    @param input bytes or str written to stdin, then closed
    @param timeout seconds before the child is killed, or None
    @param text decode stdout and stderr (and encode input) as text
    @param cwd working directory, or None
    @param env environment, or None for this process's
    @param stdin a file or pipe for stdin when input is None
    @param stdout PIPE to capture, or a file
    @param stderr PIPE to capture, a file, or subprocess.STDOUT
    @return a subprocess.CompletedProcess
    """
    stop_point()
    if input is not None:
        stdin = subprocess.PIPE
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL if stdin is None else stdin,
                            stdout=stdout, stderr=stderr, text=text, cwd=cwd, env=env,
                            start_new_session=True)
    end = None if timeout is None else time.monotonic() + timeout
    feed = input
    try:
        while True:
            wait = STOP_POLL_S if _stop_watch is not None else None
            if end is not None:
                left = max(0.0, end - time.monotonic())
                wait = left if wait is None else min(wait, left)
            try:
                out, err = proc.communicate(feed, timeout=wait)
                break
            except subprocess.TimeoutExpired:
                feed = None
                if end is not None and time.monotonic() >= end:
                    kill_group(proc)
                    raise subprocess.TimeoutExpired(argv, timeout)
                stop_point()
    except BaseException:
        kill_group(proc)
        raise
    return subprocess.CompletedProcess(argv, proc.returncode, out, err)


# ---- host file --------------------------------------------------------------

def script_repo():
    """Return the root of the checkout or worktree holding this script."""
    here = os.path.dirname(os.path.abspath(__file__))
    return subprocess.run(["git", "-C", here, "rev-parse", "--show-toplevel"],
                          capture_output=True, text=True, check=True).stdout.strip()


def default_hosts_path():
    """Return ${XDG_CONFIG_HOME:-~/.config}/openpete/fleet-hosts.toml."""
    cfg = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(cfg, "openpete", "fleet-hosts.toml")


def hosts_path():
    """Return the host file path: $OPENPETE_FLEET_HOSTS, else the default."""
    return os.environ.get("OPENPETE_FLEET_HOSTS") or default_hosts_path()


def parse_value(raw, where):
    """Parse one TOML scalar (basic string, integer, float, boolean)."""
    if raw.startswith('"'):
        m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', raw)
        if not m:
            raise FleetError(f"{where}: unterminated or malformed string {raw}")
        return json.loads('"' + m.group(1) + '"')
    if raw in ("true", "false"):
        return raw == "true"
    if re.fullmatch(r"[+-]?\d+", raw):
        return int(raw)
    if re.fullmatch(r"[+-]?(\d+\.\d*|\.\d+|\d+)([eE][+-]?\d+)?", raw):
        return float(raw)
    raise FleetError(f"{where}: unsupported value {raw!r} (string, integer, float or boolean)")


def strip_comment(line):
    """Return the line without a trailing # comment outside a string."""
    in_str = esc = False
    for i, c in enumerate(line):
        if esc:
            esc = False
        elif c == "\\" and in_str:
            esc = True
        elif c == '"':
            in_str = not in_str
        elif c == "#" and not in_str:
            return line[:i]
    return line


def parse_toml(text, path):
    """Parse the flat host-file subset of TOML into {table: {key: value}}."""
    tables, cur = {}, None
    for n, line in enumerate(text.splitlines(), 1):
        body = strip_comment(line).strip()
        where = f"{path}:{n}"
        if not body:
            continue
        m = re.fullmatch(r"\[\s*([A-Za-z0-9_.-]+)\s*\]", body)
        if m:
            cur = m.group(1)
            if cur in tables:
                raise FleetError(f"{where}: table [{cur}] defined twice")
            tables[cur] = {}
            continue
        m = re.fullmatch(r"([A-Za-z0-9_-]+)\s*=\s*(.+)", body)
        if not m or cur is None:
            raise FleetError(f"{where}: expected [table] or key = value inside a table")
        key = m.group(1)
        if key in tables[cur]:
            raise FleetError(f"{where}: key {key} repeated in [{cur}]")
        tables[cur][key] = parse_value(m.group(2).strip(), where)
    return tables


def validate_hosts(tables, path):
    """Check the host tables against the schema and return {name: config}."""
    hosts = {}
    for tname, tbl in tables.items():
        if not tname.startswith("hosts.") or tname.count(".") != 1:
            raise FleetError(f"{path}: unexpected table [{tname}] (only [hosts.<name>])")
        name = tname.split(".", 1)[1]
        for key, val in tbl.items():
            want = HOST_KEYS.get(key)
            if want is None:
                raise FleetError(f"{path}: hosts.{name}.{key}: unknown key")
            ok = isinstance(val, want) and not (want is int and isinstance(val, bool))
            if want is float and isinstance(val, int) and not isinstance(val, bool):
                val, ok = float(val), True
            if not ok:
                raise FleetError(f"{path}: hosts.{name}.{key}: expected {want.__name__}, "
                                 f"got {type(val).__name__} {val!r}")
            tbl[key] = val
        transport = tbl.get("transport")
        if transport not in ("local", "ssh"):
            raise FleetError(f"{path}: hosts.{name}.transport: must be \"local\" or \"ssh\"")
        if transport == "ssh" and not tbl.get("ssh"):
            raise FleetError(f"{path}: hosts.{name}: transport \"ssh\" needs ssh = \"user@host\"")
        if transport == "local" and "ssh" in tbl:
            raise FleetError(f"{path}: hosts.{name}: transport \"local\" takes no ssh key")
        if tbl.get("engine") not in (None, "podman", "docker"):
            raise FleetError(f"{path}: hosts.{name}.engine: must be \"podman\" or \"docker\"")
        if tbl.get("slots", 1) < 1:
            raise FleetError(f"{path}: hosts.{name}.slots: must be at least 1")
        if tbl.get("speed", 1.0) <= 0:
            raise FleetError(f"{path}: hosts.{name}.speed: must be positive")
        hosts[name] = tbl
    locals_ = [n for n, t in hosts.items() if t["transport"] == "local"]
    if len(locals_) > 1:
        raise FleetError(f"{path}: more than one local host: {', '.join(locals_)}")
    return hosts


def load_hosts(required=True):
    """Load and validate the host file; absent and required exits 64 naming add."""
    path = hosts_path()
    if not os.path.exists(path):
        if required:
            raise FleetError(
                f"SKIP: no host file {path}; create it with "
                f"`scripts/golden-fleet.py add local` or `scripts/golden-fleet.py add "
                f"<name> --ssh user@host`", EXIT_SKIP)
        return path, {}
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return path, validate_hosts(parse_toml(text, path), path)


def format_value(val):
    """Serialise one scalar as TOML."""
    if isinstance(val, bool):
        return "true" if val else "false"
    if isinstance(val, float):
        return f"{val:.2f}"
    if isinstance(val, int):
        return str(val)
    return json.dumps(val)


def format_table(name, cfg):
    """Serialise one host's table in KEY_ORDER."""
    lines = [f"[hosts.{name}]"]
    lines += [f"{k} = {format_value(cfg[k])}" for k in KEY_ORDER if k in cfg]
    return "\n".join(lines) + "\n"


def write_host_table(path, name, cfg):
    """Replace or append [hosts.<name>] in the host file, keeping other lines."""
    text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
    lines = text.splitlines(keepends=True)
    header = re.compile(r"\s*\[\s*hosts\." + re.escape(name) + r"\s*\]")
    start = next((i for i, ln in enumerate(lines) if header.match(ln)), None)
    table = format_table(name, cfg)
    if start is None:
        sep = "" if not text or text.endswith("\n\n") else ("\n" if text.endswith("\n") else "\n\n")
        new = text + sep + table
    else:
        end = next((i for i in range(start + 1, len(lines))
                    if re.match(r"\s*\[", lines[i])), len(lines))
        tail = lines[end:]
        new = "".join(lines[:start]) + table + ("\n" if tail else "") + "".join(tail)
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(new)
    os.replace(tmp, path)
    validate_hosts(parse_toml(new, path), path)


# ---- transport --------------------------------------------------------------

# ssh's messages for a session that ended before the remote command started
# (TCP connect, name lookup, or the server's identification banner); rsync
# relays them on stderr when its remote shell fails the same way. Only these
# are retried: the remote side ran nothing, so the retry cannot repeat work.
CONNECT_FAIL_RE = re.compile(
    r"^(?:ssh: connect to host \S+ port \d+: (?:Connection timed out|Connection refused|"
    r"No route to host|Network is unreachable|Operation timed out)"
    r"|ssh: Could not resolve hostname \S+: .*"
    r"|kex_exchange_identification: .*"
    r"|Connection timed out during banner exchange)\s*$", re.M)
# ssh exits 255 on its own failure; rsync exits 255 or 12 when its remote shell does.
CONNECT_FAIL_RCS = (255, 12)
TRANSPORT_BACKOFF_S = (5, 15, 30)   # sleep before retry k; len() = retries after the first try


def connect_failure(cp):
    """Return ssh's connect-failure line when a CompletedProcess failed before its remote command started, else ''.

    @param cp a CompletedProcess of an ssh or rsync command; stderr str, bytes or None
    @return the matched stderr line, or '' for success, a remote command's own exit, or no match
    """
    if cp.returncode not in CONNECT_FAIL_RCS:
        return ""
    err = cp.stderr if isinstance(cp.stderr, str) else (cp.stderr or b"").decode(errors="replace")
    m = CONNECT_FAIL_RE.search(err)
    return m.group(0).strip() if m else ""


def backoff_sleep(seconds):
    """Sleep seconds in STOP_POLL_S steps, raising StopRequested when the installed stop predicate holds."""
    end = time.monotonic() + seconds
    while True:
        stop_point()
        left = end - time.monotonic()
        if left <= 0:
            return
        time.sleep(min(STOP_POLL_S, left))


def transport_run(host, what, argv, retry=True, **kw):
    """Run an ssh or rsync argv for a host through run_proc, retrying a connect failure with backoff.

    A try whose stderr carries ssh's connect-failure line (connect_failure)
    is retried after TRANSPORT_BACKOFF_S[k], each retry logged with the
    host and the operation; any other exit, the remote command's own nonzero
    status included, returns at once. The last try's result is returned
    after the retries are spent, logged as given up.
    @param host the Host the command reaches (a local host runs once)
    @param what the operation named in the retry log
    @param argv the command
    @param retry false runs once (callers that tolerate a miss: poll, clock skew, status)
    @param kw run_proc's keyword arguments; input is re-fed on each try
    @return the CompletedProcess of the last try
    """
    tries = 1 + (len(TRANSPORT_BACKOFF_S) if retry and not host.local else 0)
    for k in range(tries):
        cp = run_proc(argv, **kw)
        line = connect_failure(cp)
        if not line or host.local:
            return cp
        if k + 1 == tries:
            if retry:
                print(f"[fleet] {host.name}: {what}: connect failed {tries} times, giving up: {line}")
            return cp
        wait = TRANSPORT_BACKOFF_S[k]
        print(f"[fleet] {host.name}: {what}: {line}; retry {k + 1}/{tries - 1} in {wait} s")
        backoff_sleep(wait)
    return cp


class Host:
    """One fleet host: its config and a local or ssh transport."""

    def __init__(self, name, cfg):
        """Bind the host's name and validated config."""
        self.name = name
        self.cfg = cfg

    @property
    def local(self):
        """True for the coordinator's own machine."""
        return self.cfg["transport"] == "local"

    @property
    def fleet_root(self):
        """The configured fleet_root, unexpanded (`~` resolves on the host)."""
        return self.cfg.get("fleet_root", DEFAULT_FLEET_ROOT)

    def argv(self, remote_cmd, tag=True):
        """Return the argv that runs a shell command string on the host.

        A remote command of a running run is prefixed with RUN_TAG_VAR=<run id>
        (unless tag is false), so it and its children carry the tag REAP_SCRIPT
        finds.
        """
        if self.local:
            return ["bash", "-c", remote_cmd]
        if tag and _run_tag:
            remote_cmd = f"{RUN_TAG_VAR}={shlex.quote(_run_tag)} {remote_cmd}"
        return ["ssh", *SSH_OPTS, self.cfg["ssh"], remote_cmd]

    def run_script(self, script, args=(), timeout=120, check=False, binary=False, tag=True,
                   retry=True, what="script"):
        """Run a bash script on the host via `bash -s`, script on stdin; binary keeps stdout as bytes.

        A connect failure is retried (transport_run) unless retry is false;
        timeout bounds each try.
        """
        cmd = "bash -s -- " + " ".join(shlex.quote(str(a)) for a in args)
        try:
            cp = transport_run(self, what, self.argv(cmd, tag), retry=retry,
                               input=script.encode() if binary else script,
                               text=not binary, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise FleetError(f"{self.name}: script timed out after {timeout} s")
        if check and cp.returncode != 0:
            err = cp.stderr.decode(errors="replace") if binary else cp.stderr
            raise FleetError(f"{self.name}: script exited {cp.returncode}: "
                             f"{err.strip()[-400:]}")
        return cp

    def stream_command(self, argv):
        """Return the argv running `argv` on the host with stdin passed through."""
        return self.argv(" ".join(shlex.quote(a) for a in argv))


def parse_kv(text):
    """Parse key=value lines into a dict."""
    out = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


PROBE_SCRIPT = r'''
set -u
root="$1"; engine="$2"; image="$3"
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
echo "root=$root"
if [ -z "$engine" ]; then
    for e in podman docker; do command -v "$e" >/dev/null 2>&1 && { engine=$e; break; }; done
fi
echo "engine=$engine"
if [ -n "$engine" ] && command -v "$engine" >/dev/null 2>&1; then
    echo "engine_ok=1"
    echo "fleet_image_id=$("$engine" image inspect --format '{{.Id}}' "$image" 2>/dev/null)"
else
    echo "engine_ok=0"
fi
echo "image_id_file=$(cat "$root/image.id" 2>/dev/null)"
lock="$root/golden.lock"
if [ -e "$lock" ]; then
    if flock -n "$lock" true 2>/dev/null; then echo "lock=free"; else echo "lock=busy"; fi
    echo "lock_holder=$(head -c 200 "$lock" 2>/dev/null | tr '\n' ' ')"
else
    echo "lock=free"
    echo "lock_holder="
fi
echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo none)"
echo "kill_user_processes=$(busctl get-property org.freedesktop.login1 /org/freedesktop/login1 \
    org.freedesktop.login1.Manager KillUserProcesses 2>/dev/null | awk '{print $2}')"
echo "linger=$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)"
echo "cpu=$(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //')"
echo "nproc=$(nproc)"
echo "memavail_kib=$(awk '/^MemAvailable:/{print $2}' /proc/meminfo)"
echo "loadavg1=$(cut -d' ' -f1 /proc/loadavg)"
d="$root"; while [ ! -e "$d" ]; do d="$(dirname "$d")"; done
echo "free_bytes=$(df -PB1 "$d" | awk 'NR==2{print $4}')"
if [ -d "$root" ]; then echo "root_bytes=$(du -sb "$root" 2>/dev/null | cut -f1)"; else echo "root_bytes=0"; fi
'''


def probe(host, retry=True):
    """Probe a host in one script; returns a dict with reachable=False on failure; retry=False makes one connect attempt."""
    try:
        cp = host.run_script(PROBE_SCRIPT, [host.fleet_root, host.cfg.get("engine", ""),
                                            IMAGE_TAG], timeout=60, retry=retry, what="probe")
    except FleetError as e:
        return {"reachable": False, "error": str(e)}
    if cp.returncode != 0 or "root=" not in cp.stdout:
        return {"reachable": False,
                "error": (cp.stderr.strip() or f"exit {cp.returncode}")[-300:]}
    info = parse_kv(cp.stdout)
    info["reachable"] = True
    return info


def local_engine():
    """Return the coordinator's container engine (podman first), or None."""
    for e in ("podman", "docker"):
        if subprocess.run(["bash", "-c", f"command -v {e}"], capture_output=True).returncode == 0:
            return e
    return None


def local_image():
    """Return the image the coordinator ships: $OPENPETE_CAPTURE_IMAGE or the default."""
    return os.environ.get("OPENPETE_CAPTURE_IMAGE", "openpete-linux-capture")


def image_inspect(engine, image, fmt):
    """Return one inspect field of a local image, or '' when absent."""
    cp = subprocess.run([engine, "image", "inspect", "--format", fmt, image],
                        capture_output=True, text=True)
    return cp.stdout.strip() if cp.returncode == 0 else ""


def norm_id(image_id):
    """Strip a sha256: prefix so podman and docker Ids compare."""
    return (image_id or "").removeprefix("sha256:")


def calibrated_governor(cfg):
    """Return the governor recorded in `calibrated`, or None."""
    m = re.search(r"governor=(\S+)", cfg.get("calibrated", ""))
    return m.group(1) if m else None


def select_hosts(hosts, names):
    """Return Host objects for a comma-separated name list, or every host."""
    if not names:
        return [Host(n, c) for n, c in hosts.items()]
    out = []
    for n in names.split(","):
        if n not in hosts:
            raise FleetError(f"no host {n!r} in the host file (have: {', '.join(hosts)})")
        out.append(Host(n, hosts[n]))
    return out


# ---- locks ------------------------------------------------------------------

def expand_root(root):
    """Expand `~` in a local fleet_root."""
    return os.path.expanduser(root)


def try_golden_lock(root):
    """Take <root>/golden.lock non-blocking and write the holder record (pid, host, run id, time); fd or None."""
    root = expand_root(root)
    os.makedirs(root, exist_ok=True)
    lock = os.path.join(root, "golden.lock")
    fd = os.open(lock, os.O_WRONLY | os.O_APPEND | os.O_CREAT, 0o644)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        os.close(fd)
        return None
    os.ftruncate(fd, 0)
    stamp = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    run = f"run {_run_tag} " if _run_tag else ""
    os.write(fd, f"pid {os.getpid()} host {os.uname().nodename} golden-fleet {run}"
                 f"since {stamp}\n".encode())
    return fd


# ---- local host busy ----------------------------------------------------------

def proc_table():
    """Return the live processes as dicts (pid, ppid, argv, exe, cgroup); zombies and kernel threads left out."""
    out = []
    for pid in (d for d in os.listdir("/proc") if d.isdigit()):
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                argv = [w.decode(errors="replace") for w in f.read().split(b"\0") if w]
            with open(f"/proc/{pid}/stat", "rb") as f:
                stat = f.read().rsplit(b")", 1)[1].split()
            with open(f"/proc/{pid}/cgroup", "rb") as f:
                cgroup = f.read().decode(errors="replace")
        except (OSError, IndexError):
            continue
        if not argv or stat[0] == b"Z":
            continue
        try:
            exe = os.readlink(f"/proc/{pid}/exe")
        except OSError:
            exe = ""
        out.append({"pid": int(pid), "ppid": int(stat[1]), "argv": argv, "exe": exe, "cgroup": cgroup})
    return out


def land_lock_holder(tree):
    """Return the landing lock's path while a landing holds it (land-branch.sh, land-train.sh), else ''."""
    cp = subprocess.run(["git", "-C", tree, "rev-parse", "--path-format=absolute", "--git-common-dir"],
                        capture_output=True, text=True)
    if cp.returncode != 0:
        return ""
    lock = os.path.join(cp.stdout.strip(), LAND_LOCK_NAME)
    if not os.path.exists(lock):
        return ""
    fd = os.open(lock, os.O_RDONLY)
    try:
        fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        return ""
    except BlockingIOError:
        return lock
    finally:
        os.close(fd)


def local_busy(tree, memavail_kib, procs=None, me=None):
    """Return why the local host must take no unit now (empty when it may): a landing, the scheduler gate, a game process outside the fleet, low memory.

    procs is proc_table()'s list (read when None); me is the pid whose
    ancestors are left out (this process when None). A game process is the
    fleet's own when its cgroup is a container's or an ancestor runs
    golden-fleet.py, conmon or the unit supervisor. Memory: MemAvailable under one slot (MEM_PER_SLOT_KIB)
    plus LOCAL_MEM_RESERVE_KIB.
    """
    procs = proc_table() if procs is None else procs
    by_pid = {p["pid"]: p for p in procs}
    me = os.getpid() if me is None else me
    mine, p = set(), me
    while p in by_pid and p not in mine:
        mine.add(p)
        p = by_pid[p]["ppid"]

    def fleet_owned(rec):
        seen, q = set(), rec
        while q and q["pid"] not in seen:
            seen.add(q["pid"])
            if CONTAINER_CGROUP_RE.search(q["cgroup"]) or any(
                    "golden-fleet.py" in w or os.path.basename(w) in FLEET_PARENTS for w in q["argv"][:2]):
                return True
            q = by_pid.get(q["ppid"])
        return False

    reasons = []
    lock = land_lock_holder(tree)
    if lock:
        reasons.append(f"local busy: a landing holds {lock}")
    for rec in sorted(procs, key=lambda r: r["pid"]):
        if rec["pid"] in mine:
            continue
        cmd = " ".join(rec["argv"])[:120]
        if any(os.path.basename(w) in BUSY_SCRIPTS for w in rec["argv"][:2]):
            reasons.append(f"local busy: {rec['pid']} {cmd}")
        elif (GAME_EXE_RE.search(rec["argv"][0]) or GAME_EXE_RE.search(rec["exe"])) \
                and not fleet_owned(rec):
            reasons.append(f"local busy: {rec['pid']} {cmd}")
    need = MEM_PER_SLOT_KIB + LOCAL_MEM_RESERVE_KIB
    if memavail_kib < need:
        reasons.append(f"local busy: MemAvailable {memavail_kib / GIB_KIB:.1f} GiB under "
                       f"{need / GIB_KIB:.0f} GiB (one slot plus the "
                       f"{LOCAL_MEM_RESERVE_KIB / GIB_KIB:.0f} GiB reserve)")
    return reasons


def waitable(reasons):
    """True when every drop reason of a host is transient (WAIT_REASONS): the host may become usable by waiting."""
    return bool(reasons) and all(r.startswith(WAIT_REASONS) for r in reasons)


# ---- preflight --------------------------------------------------------------

def preflight(hosts, need_bytes=0, require_calibration=True, info=None):
    """Check each host; return [(host, info, reasons, slots)] with reasons empty when usable."""
    engine = local_engine()
    local_id = norm_id(image_inspect(engine, local_image(), "{{.Id}}")) if engine else ""
    image_bytes = int(image_inspect(engine, local_image(), "{{.Size}}") or 0) if engine else 0
    results = []
    for h in hosts:
        reasons, slots = [], h.cfg.get("slots", 1)
        i = (info or {}).get(h.name) or probe(h)
        if not h.cfg.get("enabled", True):
            reasons.append("disabled (enabled = false)")
        if not i["reachable"]:
            reasons.append(f"unreachable: {i['error']}")
            results.append((h, i, reasons, 0))
            continue
        if i.get("engine_ok") != "1":
            reasons.append(f"no container engine ({i.get('engine') or 'none found'})")
        gov = calibrated_governor(h.cfg)
        if gov is None:
            if require_calibration:
                reasons.append("not calibrated (no governor= in calibrated); run add")
        elif gov != i["governor"]:
            reasons.append(f"governor {i['governor']}, calibrated at {gov}")
        need = need_bytes
        ships = norm_id(i.get("fleet_image_id")) != local_id and not h.local
        if ships:
            need += image_bytes
        free = int(i.get("free_bytes") or 0)
        if free < need:
            reasons.append(f"free space {free / 2**30:.1f} GiB under {i['root']}, "
                           f"need {need / 2**30:.1f} GiB"
                           + (" (image ships)" if ships else ""))
        if h.local:
            fit = int(i.get("memavail_kib") or 0) // MEM_PER_SLOT_KIB
            if fit < 1:
                reasons.append(f"MemAvailable {int(i['memavail_kib']) / GIB_KIB:.1f} GiB "
                               f"under 2 GiB for one slot")
            elif fit < slots:
                print(f"[fleet] {h.name}: slots lowered {slots} -> {fit} "
                      f"(MemAvailable {int(i['memavail_kib']) / GIB_KIB:.1f} GiB, 2 GiB per slot)")
                slots = fit
        if not h.local and i.get("kill_user_processes") == "true" and i.get("linger") != "yes":
            reasons.append("logind kills an ssh session's processes at logout "
                           "(KillUserProcesses=yes) and lingering is off; run "
                           "`loginctl enable-linger` on the host")
        if i.get("lock") == "busy":
            reasons.append(f"golden lock held: {i.get('lock_holder', '').strip()}")
        results.append((h, i, reasons, slots))
    return results


# ---- image shipping ---------------------------------------------------------

WRITE_IMAGE_ID = r'''
set -eu
root="$1"; engine="$2"; id="$3"; tag="$4"
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
"$engine" tag "$id" "$tag"
got="$("$engine" image inspect --format '{{.Id}}' "$tag")"
[ "${got#sha256:}" = "${id#sha256:}" ] || { echo "tag $tag names $got, not $id" >&2; exit 1; }
mkdir -p "$root"
# The Id a previous ship recorded joins image.id.history, from which clean
# removes images the fleet shipped and no longer uses.
old="$(cat "$root/image.id" 2>/dev/null || true)"
[ -n "$old" ] && [ "${old#sha256:}" != "${id#sha256:}" ] && printf '%s\n' "$old" >>"$root/image.id.history"
printf '%s\n' "$id" >"$root/image.id.tmp"
mv "$root/image.id.tmp" "$root/image.id"
'''


# The names a load may create beside :fleet (the archive's own tags, or the
# engine's default name); each that did not exist before the load is untagged.
LOAD_NAMES = ("openpete-linux-capture:latest", "localhost/openpete-linux-capture:latest")
NAMES_PRESENT = r'''
engine="$1"; shift
for n in "$@"; do "$engine" image inspect "$n" >/dev/null 2>&1 && echo "$n"; done
exit 0
'''


def transfer_image(host, engine, host_engine, local_id, dry_run):
    """Stream the image saved under the :fleet tag alone into the host's engine.

    The save restarts with each try of the load: a load whose ssh failed to
    connect (connect_failure) read none of the stream, and is retried after
    TRANSPORT_BACKOFF_S like transport_run's.
    """
    subprocess.run([engine, "tag", local_id, IMAGE_TAG], check=True, capture_output=True)
    save = [engine, "save", IMAGE_TAG]
    load = host.stream_command([host_engine, "load"])
    print(f"[fleet] {host.name}: {shlex.join(save)} | {shlex.join(load)}")
    if dry_run:
        return
    before = set(host.run_script(NAMES_PRESENT, [host_engine, *LOAD_NAMES]).stdout.split())
    t0 = time.monotonic()
    for k in range(1 + len(TRANSPORT_BACKOFF_S)):
        src = subprocess.Popen(save, stdout=subprocess.PIPE, start_new_session=True)
        try:
            dst = run_proc(load, stdin=src.stdout, text=True)
        except BaseException:
            kill_group(src)
            raise
        finally:
            src.stdout.close()
        line = connect_failure(dst)
        if not line or k == len(TRANSPORT_BACKOFF_S):
            break
        kill_group(src)
        wait = TRANSPORT_BACKOFF_S[k]
        print(f"[fleet] {host.name}: image load: {line}; retry {k + 1}/{len(TRANSPORT_BACKOFF_S)} "
              f"in {wait} s")
        backoff_sleep(wait)
    if src.wait() != 0 or dst.returncode != 0:
        raise FleetError(f"{host.name}: image transfer failed (save {src.returncode}, "
                         f"load {dst.returncode}): {dst.stderr.strip()[-300:]}")
    print(f"[fleet] {host.name}: shipped in {time.monotonic() - t0:.0f} s")
    after = set(host.run_script(NAMES_PRESENT, [host_engine, *LOAD_NAMES]).stdout.split())
    for name in sorted(after - before):
        host.run_script('"$1" rmi "$2" >/dev/null', [host_engine, name], check=True)
        print(f"[fleet] {host.name}: untagged {name}, which the load created")


def ship_image(host, info, dry_run=False):
    """Deliver the local image to the host as :fleet and record image.id; True when shipped."""
    engine = local_engine()
    if not engine:
        raise FleetError("no local container engine", EXIT_SKIP)
    local_id = norm_id(image_inspect(engine, local_image(), "{{.Id}}"))
    if not local_id:
        raise FleetError(f"no local image {local_image()}", EXIT_SKIP)
    host_engine = info.get("engine") or "podman"
    same = norm_id(info.get("fleet_image_id")) == local_id
    recorded = norm_id(info.get("image_id_file")) == local_id
    if same and recorded:
        print(f"[fleet] {host.name}: {IMAGE_TAG} is {local_id[:12]}; nothing to ship")
        return False
    if not same and not host.local:
        transfer_image(host, engine, host_engine, local_id, dry_run)
    print(f"[fleet] {host.name}: tag {local_id[:12]} {IMAGE_TAG}; write image.id")
    if not dry_run:
        host.run_script(WRITE_IMAGE_ID, [host.fleet_root, host_engine, local_id, IMAGE_TAG],
                        check=True)
    return True


# ---- calibration sweep ------------------------------------------------------

def sweep_values(cores):
    """Return the sweep's slot values: 1, 2, 4, ... up to cores, plus cores itself."""
    vals, s = [], 1
    while s <= cores:
        vals.append(s)
        s *= 2
    if vals[-1] != cores:
        vals.append(cores)
    return vals


def sweep_should_stop(legs):
    """True once the last leg improved on the one before by under 10% or breached memory."""
    if legs and legs[-1]["min_memavail_kib"] < MEM_PER_SLOT_KIB:
        return True
    return len(legs) >= 2 and legs[-1]["wall_s"] > (1 - IMPROVEMENT_FLOOR) * legs[-2]["wall_s"]


def pick_slots(legs):
    """Apply the slots rule to sweep legs; return (slots, wall_s at slots, explanation lines)."""
    legs = sorted(legs, key=lambda x: x["slots"])
    if not legs:
        raise FleetError("slots rule: no sweep legs")
    why, idx = [], len(legs) - 1
    for i in range(len(legs) - 1):
        a, b = legs[i], legs[i + 1]
        gain = 1 - b["wall_s"] / a["wall_s"]
        why.append(f"slots {a['slots']} -> {b['slots']}: {a['wall_s']:.0f} s -> "
                   f"{b['wall_s']:.0f} s, improvement {gain * 100:.1f}%")
        if gain < IMPROVEMENT_FLOOR:
            idx = i
            why.append(f"improvement under {IMPROVEMENT_FLOOR * 100:.0f}%: slots {a['slots']}")
            break
    else:
        why.append(f"every step improved by {IMPROVEMENT_FLOOR * 100:.0f}% or more: "
                   f"slots {legs[idx]['slots']}")
    while idx > 0 and legs[idx]["min_memavail_kib"] < MEM_PER_SLOT_KIB:
        why.append(f"slots {legs[idx]['slots']}: min MemAvailable "
                   f"{legs[idx]['min_memavail_kib'] / GIB_KIB:.2f} GiB under 2 GiB; lowered")
        idx -= 1
    if legs[idx]["min_memavail_kib"] < MEM_PER_SLOT_KIB:
        raise FleetError(f"slots rule: min MemAvailable under 2 GiB even at slots "
                         f"{legs[idx]['slots']}")
    return legs[idx]["slots"], legs[idx]["wall_s"], why


def idle_bound(cores):
    """Return the 1-minute load average at or under which the machine counts as idle."""
    return max(1.0, 0.1 * cores)


def leg_is_idle(leg, cores):
    """Apply the idle rule to one local leg; return (idle, explanation)."""
    bound = idle_bound(cores)
    after_bound = bound + 0.25 * leg["load_end"]
    ok = leg["load_before"] <= bound and leg["load_after"] <= after_bound
    return ok, (f"load before {leg['load_before']:.2f} (<= {bound:.2f}), "
                f"{IDLE_SETTLE_S} s after {leg['load_after']:.2f} (<= {after_bound:.2f})")


def pick_t_local(legs, cores):
    """Return the first idle local leg's wall time, printing each discarded leg."""
    for n, leg in enumerate(legs, 1):
        ok, why = leg_is_idle(leg, cores)
        print(f"[fleet] local leg {n} at slots {leg['slots']}: {leg['wall_s']:.0f} s; {why}; "
              + ("accepted" if ok else "loaded, discarded"))
        if ok:
            return leg["wall_s"]
    raise FleetError(f"no idle local leg in {len(legs)} attempts")


def loadavg1():
    """Return the local 1-minute load average."""
    return os.getloadavg()[0]


# ---- commands ---------------------------------------------------------------

def fmt_bytes(n):
    """Format a byte count in GiB."""
    return f"{int(n or 0) / 2**30:.1f} GiB"


def cmd_status(a):
    """Print each host's reachability, image, lock, governor, calibration and space."""
    path, hosts = load_hosts()
    engine = local_engine()
    local_id = norm_id(image_inspect(engine, local_image(), "{{.Id}}")) if engine else ""
    print(f"host file: {path}")
    print(f"local image: {local_image()} {local_id[:12] or 'absent'}")
    for h in select_hosts(hosts, a.hosts):
        i = probe(h, retry=False)
        where = "local" if h.local else h.cfg["ssh"]
        print(f"[{h.name}] {where}")
        if not i["reachable"]:
            print(f"  reachable: no ({i['error']})")
            continue
        fid = norm_id(i.get("fleet_image_id"))
        rec = norm_id(i.get("image_id_file"))
        print("  reachable: yes")
        print(f"  engine: {i.get('engine') or 'none'}"
              + ("" if i.get("engine_ok") == "1" else " (missing)"))
        print(f"  {IMAGE_TAG}: {fid[:12] or 'absent'} "
              + ("(equals local)" if fid and fid == local_id else "(differs from local)"))
        print(f"  image.id: {rec[:12] or 'absent'}"
              + ("" if not rec else (" (equals :fleet)" if rec == fid else " (differs from :fleet)")))
        holder = i.get("lock_holder", "").strip()
        print(f"  golden lock: {i.get('lock')}" + (f"; holder record: {holder}" if holder else ""))
        gov = calibrated_governor(h.cfg)
        print(f"  governor: {i.get('governor')}"
              + ("" if gov is None else (" (matches calibration)" if gov == i.get("governor")
                                         else f" (calibrated at {gov})")))
        print(f"  calibrated: {h.cfg.get('calibrated', 'never')}; slots "
              f"{h.cfg.get('slots', '-')}, speed {h.cfg.get('speed', '-')}")
        print(f"  cpu: {i.get('cpu')} x{i.get('nproc')}; MemAvailable "
              f"{int(i.get('memavail_kib') or 0) / GIB_KIB:.1f} GiB; load {i.get('loadavg1')}")
        print(f"  free under {i['root']}: {fmt_bytes(i.get('free_bytes'))}; "
              f"fleet_root size {fmt_bytes(i.get('root_bytes'))}")
        try:
            trees = list_trees(h, sizes=True)
        except FleetError as e:
            print(f"  trees: {e}")
            continue
        for line in tree_lines(trees):
            print(f"  {line}")
    return 0


def tree_lines(trees, now=None):
    """Return status's line per trees/<key> record: kind, size, age, owner."""
    me, now = os.uname().nodename, now or time.time()
    if not trees:
        return ["trees: none"]
    out = []
    for t in trees:
        try:
            age = f"{(now - float(t.get('last-run'))) / 3600:.1f} h"
        except (TypeError, ValueError):
            age = "unknown"
        size = fmt_bytes(t.get("bytes")) if t.get("present") == "1" else "absent"
        owner = "" if t.get("coordinator") == me else f", coordinator {t.get('coordinator', '?')}"
        if t.get("repo"):
            out.append(f"base {t['key']}: {size}, refreshed {age} ago, repository {t['repo']}, "
                       f"last checkout {t.get('path', '?')}{owner}")
        else:
            out.append(f"tree {t['key']} (keyed on a checkout path): {size}, last run {age} ago, "
                       f"checkout {t.get('path', '?')}{owner}")
    return out


def print_preflight(results):
    """Print preflight verdicts; return the usable (host, slots) pairs."""
    usable = []
    for h, _i, reasons, slots in results:
        if reasons:
            for r in reasons:
                print(f"[fleet] {h.name}: dropped: {r}")
        else:
            print(f"[fleet] {h.name}: usable, slots {slots}")
            usable.append((h, slots))
    return usable


def cmd_preflight(a):
    """Run preflight on the named hosts; exit 64 when none is usable."""
    _path, hosts = load_hosts()
    sel = [h for h in select_hosts(hosts, a.hosts)]
    usable = print_preflight(preflight(sel, int(a.need_gib * 2**30),
                                       require_calibration=not a.no_calibration))
    if not usable:
        print("[fleet] SKIP: no usable host")
        return EXIT_SKIP
    return 0


def cmd_ship_image(a):
    """Ship the local capture image to each named host whose :fleet Id differs."""
    _path, hosts = load_hosts()
    for h in select_hosts(hosts, a.hosts):
        i = probe(h)
        if not i["reachable"]:
            raise FleetError(f"{h.name}: unreachable: {i['error']}")
        ship_image(h, i, dry_run=a.dry_run)
    return 0


def new_host_cfg(a, existing):
    """Return the host's table before measurement: existing values under the CLI's."""
    cfg = dict(existing or {})
    cfg.pop("calibrated", None)  # the measurement records the governor it runs under
    if a.name == "local":
        cfg["transport"] = "local"
        cfg.pop("ssh", None)
        cfg.setdefault("nice", 19)
    else:
        if not a.ssh:
            raise FleetError("add <name> needs --ssh user@host (only `local` takes none)", 2)
        cfg["transport"] = "ssh"
        cfg["ssh"] = a.ssh
        cfg["engine"] = a.engine or cfg.get("engine", "podman")
    if a.fleet_root:
        cfg["fleet_root"] = a.fleet_root
    cfg.setdefault("fleet_root", DEFAULT_FLEET_ROOT)
    for k in ("enabled", "default", "build"):
        cfg.setdefault(k, True)
    return cfg


def calibrated_stamp(info):
    """Return the `calibrated` value: date, governor and CPU model of a probe."""
    return f"{datetime.date.today().isoformat()} governor={info['governor']} cpu={info['cpu']}"


def find_local(hosts):
    """Return the host file's local Host, or None."""
    return next((Host(n, c) for n, c in hosts.items() if c["transport"] == "local"), None)


def add_from_timings(a, path, hosts, host, cfg):
    """Apply the slots rule and speed to recorded legs (--timings); write the table unless --dry-run."""
    timings = json.load(open(a.timings))
    (_h, info, reasons, _s), = preflight([host], need_bytes=CALIB_NEED_BYTES,
                                         require_calibration=False)
    reasons = [r for r in reasons if not r.startswith("golden lock held")]
    for r in reasons:
        print(f"[fleet] {host.name}: preflight: {r}")
    if reasons:
        raise FleetError(f"{host.name}: preflight failed", EXIT_SKIP)
    cores = int(timings.get("cores") or info["nproc"])
    print(f"[fleet] legs from {a.timings} (cores {cores}); sweep values {sweep_values(cores)}")
    slots, t_host, why = pick_slots(timings["sweep"])
    for w in why:
        print(f"[fleet] rule: {w}")
    speed = 1.0
    if not host.local:
        local = find_local(hosts)
        if timings.get("local"):
            t_local = pick_t_local(timings["local"], os.cpu_count())
        elif local and local.cfg.get("calib_s"):
            t_local = float(local.cfg["calib_s"])
            print(f"[fleet] t_local {t_local:.0f} s: calib_s of [hosts.{local.name}]")
        else:
            raise FleetError("no local leg in --timings and no calib_s on a local host; "
                             "run `add local` first")
        speed = t_local / t_host
        print(f"[fleet] speed = t_local {t_local:.0f} s / t_host {t_host:.0f} s = {speed:.2f}")
    cfg.update(slots=slots, speed=round(speed, 2), calib_s=int(round(t_host)),
               calibrated=calibrated_stamp(info))
    return write_or_show(path, host.name, cfg, a.dry_run)


def write_or_show(path, name, cfg, dry_run):
    """Print a host table and write it into the host file unless dry_run."""
    print(f"[fleet] {'would write' if dry_run else 'writing'} {path}:")
    sys.stdout.write(format_table(name, cfg))
    if not dry_run:
        write_host_table(path, name, cfg)
    return 0


def cmd_add(a):
    """Measure a host through `run`'s machinery and record its slots, speed, calib_s and calibration."""
    t0 = time.monotonic()
    path, hosts = load_hosts(required=False)
    if a.name != "local" and hosts.get(a.name, {}).get("transport") == "local":
        raise FleetError(f"{a.name} is the local host; use `add local`")
    if a.name == "local":
        other = [n for n, c in hosts.items() if c["transport"] == "local" and n != "local"]
        if other:
            raise FleetError(f"the host file already names a local host: {other[0]}")
    cfg = new_host_cfg(a, hosts.get(a.name))
    host = Host(a.name, cfg)
    if a.timings:
        return add_from_timings(a, path, hosts, host, cfg)

    local = host if host.local else find_local(hosts)
    if local is None or (not host.local and not local.cfg.get("slots")):
        raise FleetError("no local host with recorded slots in the host file; run "
                         "`add local` first", EXIT_SKIP)
    plans, measure = {host.name: "sweep"}, [host]
    if not host.local and not local.cfg.get("calib_s"):
        plans[local.name] = local.cfg["slots"]
        measure.append(local)
        print(f"[fleet] {local.name} has no calib_s: t_local is measured in this add at "
              f"slots {local.cfg['slots']}")
    rc, co = run_calibration(local, measure, plans, a.dry_run)
    if rc != 0 or a.dry_run:
        return rc

    slots, t_host, why = pick_slots(co.legs[host.name])
    for w in why:
        print(f"[fleet] rule: {w}")
    speed = 1.0
    if not host.local:
        if local.name in plans:
            t_local = co.legs[local.name][-1]["wall_s"]
            lcfg = dict(local.cfg, calib_s=int(round(t_local)))
            write_or_show(path, local.name, lcfg, False)
        else:
            t_local = float(local.cfg["calib_s"])
            print(f"[fleet] t_local {t_local:.0f} s: calib_s of [hosts.{local.name}]")
        speed = t_local / t_host
        print(f"[fleet] speed = t_local {t_local:.0f} s / t_host {t_host:.0f} s = {speed:.2f}")
    cfg.update(slots=slots, speed=round(speed, 2), calib_s=int(round(t_host)),
               calibrated=calibrated_stamp(co.infos[host.name]))
    write_or_show(path, host.name, cfg, False)
    print(f"[fleet] add {host.name}: wall time {time.monotonic() - t0:.0f} s")
    return 0


def cmd_calibrate(a):
    """Re-run the calibration unit at each host's recorded slots; rewrite speed, calib_s and calibrated."""
    t0 = time.monotonic()
    path, hosts = load_hosts()
    names = a.hosts.split(",") if a.hosts else [
        n for n, c in hosts.items() if c.get("enabled", True)]
    for n in names:
        if n not in hosts:
            raise FleetError(f"no host {n!r} in the host file (have: {', '.join(hosts)})")
        if not hosts[n].get("slots"):
            raise FleetError(f"{n}: no recorded slots; run `add {n}` first", EXIT_SKIP)
    # The governor recorded in `calibrated` is what calibrate re-records, so
    # preflight runs without it.
    strip = {n: {k: v for k, v in c.items() if k != "calibrated"} for n, c in hosts.items()}
    local = find_local(strip)
    if local is None:
        raise FleetError("no local host in the host file; run `add local` first", EXIT_SKIP)
    measure = [local if n == local.name else Host(n, strip[n]) for n in names]
    plans = {h.name: h.cfg["slots"] for h in measure}
    rc, co = run_calibration(local, measure, plans, a.dry_run)
    if rc != 0 or a.dry_run:
        return rc
    if local.name in plans:
        t_local = co.legs[local.name][-1]["wall_s"]
    elif local.cfg.get("calib_s"):
        t_local = float(local.cfg["calib_s"])
    else:
        raise FleetError(f"{local.name} has no calib_s and was not calibrated; include it")
    for h in measure:
        leg = co.legs[h.name][-1]
        if leg["min_memavail_kib"] < MEM_PER_SLOT_KIB:
            print(f"[fleet] {h.name}: min MemAvailable {leg['min_memavail_kib'] / GIB_KIB:.2f} "
                  f"GiB under 2 GiB at slots {leg['slots']}; `add {h.name}` re-measures slots")
        speed = 1.0 if h.local else t_local / leg["wall_s"]
        print(f"[fleet] {h.name}: slots {leg['slots']}, {leg['wall_s']:.0f} s, speed {speed:.2f}")
        cfg = dict(hosts[h.name], speed=round(speed, 2), calib_s=int(round(leg["wall_s"])),
                   calibrated=calibrated_stamp(co.infos[h.name]))
        write_or_show(path, h.name, cfg, False)
    print(f"[fleet] calibrate: wall time {time.monotonic() - t0:.0f} s")
    return 0


# ---- host-side shell helpers ------------------------------------------------

# starttime PID prints field 22 of /proc/PID/stat (the process start time in
# clock ticks since boot), read after the comm field, which may hold spaces.
SH_STARTTIME = r'''
starttime() {
    local s
    s="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
    s="${s##*) }"
    set -- $s
    echo "${20}"
}
'''

# kill_helpers RUNDIR signals each helper recorded in RUNDIR/helpers.tsv as
# attributed whose comm is still dbus-daemon and whose start time still equals
# the recorded one; a reused PID never matches both.
SH_KILL_HELPERS = SH_STARTTIME + r'''
kill_helpers() {
    local f="$1/helpers.tsv" pid start flag
    [ -f "$f" ] || return 0
    while read -r pid start flag; do
        [ "$flag" = attributed ] || continue
        [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = dbus-daemon ] || continue
        [ "$(starttime "$pid")" = "$start" ] || continue
        kill -TERM "$pid" 2>/dev/null && echo "signalled helper dbus-daemon $pid (start $start)"
    done <"$f"
}
'''

# ---- clean ------------------------------------------------------------------

# One line per trees/<key>.src: key=, the record's lines, and with a second
# argument of 1 the tree's size (bytes=, du -sb) and whether it exists
# (present=1/0). args: root [sizes]
TREE_LIST_SCRIPT = r'''
set -u
root="$1"; sizes="${2:-0}"; tab="$(printf '\t')"
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
for f in "$root"/trees/*.src; do
    [ -f "$f" ] || continue
    k="$(basename "$f" .src)"
    extra=""
    if [ "$sizes" = 1 ]; then
        if [ -d "$root/trees/$k" ]; then
            extra="present=1${tab}bytes=$(du -sb "$root/trees/$k" 2>/dev/null | cut -f1)${tab}"
        else
            extra="present=0${tab}bytes=0${tab}"
        fi
    fi
    printf 'key=%s\t%s%s\n' "$k" "$extra" "$(tr '\n' '\t' <"$f")"
done
'''

CLEAN_SCRIPT = SH_KILL_HELPERS + r'''
set -u
root="$1"; engine="$2"; mode="$3"; held="$4"; tag="$5"; shift 5
case "$root" in "~") root="$HOME";; "~/"*) root="$HOME/${root#\~/}";; esac
[ -d "$root" ] || { echo "no $root; nothing to clean"; exit 0; }
if [ "$mode" != tree ] && [ "$held" != 1 ]; then
    exec 8>>"$root/golden.lock"
    flock -n 8 || { echo "busy: golden lock held: $(head -c 200 "$root/golden.lock" | tr '\n' ' ')"; exit 3; }
fi
have_engine=0
command -v "$engine" >/dev/null 2>&1 && have_engine=1
if [ "$mode" != tree ]; then
    for d in "$root"/runs/*/; do
        [ -d "$d" ] || continue
        d="${d%/}"
        if [ -e "$d/active.lock" ] && ! flock -n -x "$d/active.lock" true; then
            echo "kept $d: a coordinator or supervisor holds it"; continue
        fi
        if [ "$mode" = stale ] && [ -n "$(find "$d" -maxdepth 3 \( -name done -o -name '*.done' \) -mmin -1440 -print -quit)" ]; then
            echo "kept $d: done marker under 24 h old"; continue
        fi
        kill_helpers "$d"
        if [ $have_engine = 1 ]; then
            "$engine" ps -aq --filter "label=org.openpete.fleet=${d##*/}" | xargs -r "$engine" rm -f >/dev/null
        fi
        rm -rf "$d" && echo "removed $d"
    done
    if [ $have_engine = 1 ]; then
        filt=(--filter label=org.openpete.fleet)
        [ "$mode" = stale ] && filt+=(--filter status=exited)
        for c in $("$engine" ps -aq "${filt[@]}"); do
            "$engine" rm -f "$c" >/dev/null && echo "removed container $c"
        done
    fi
fi
for k in "$@"; do
    (
        exec 6>"$root/trees/$k.lock"
        flock -n 6 || { echo "kept tree $k: a sync holds it"; exit 0; }
        rm -rf "$root/trees/$k" "$root/trees/$k.new" "$root/trees/$k.src"
        echo "removed tree $k"
    )
    [ -e "$root/trees/$k" ] || rm -f "$root/trees/$k.lock"
done
[ "$mode" = tree ] && exit 0
if [ $have_engine = 1 ]; then
    cur="$("$engine" image inspect --format '{{.Id}}' "$tag" 2>/dev/null || true)"
    for id in $(cat "$root/image.id" "$root/image.id.history" 2>/dev/null | sort -u); do
        [ "$mode" = stale ] && [ "${id#sha256:}" = "${cur#sha256:}" ] && continue
        tags="$("$engine" image inspect --format '{{join .RepoTags " "}}' "$id" 2>/dev/null)" || continue
        other=""
        for t in $tags; do case "$t" in *"$tag") ;; *) other="$other $t";; esac; done
        if [ -n "$other" ]; then echo "kept image ${id#sha256:}: also tagged$other"; continue; fi
        "$engine" rmi "$id" >/dev/null && echo "removed image ${id#sha256:}"
    done
    if [ "$mode" = all ] && "$engine" image inspect "$tag" >/dev/null 2>&1; then
        "$engine" rmi "$tag" >/dev/null && echo "untagged $tag (the image stays while another tag names it)"
    fi
fi
if [ "$mode" = all ]; then
    rm -rf "$root/trees" "${root:?}/census"
    rmdir "$root/runs" 2>/dev/null || echo "kept $root/runs: a run in it is active"
    rm -f "$root/image.id" "$root/image.id.history" "$root/image.id.tmp" "$root/golden.lock"
    rmdir "$root" 2>/dev/null && echo "removed $root" || echo "kept $root: holds files the fleet did not write"
fi
exit 0
'''


def repo_dir(tree):
    """Return a checkout's git common directory, resolved: one per repository, shared by its worktrees."""
    cp = subprocess.run(["git", "-C", tree, "rev-parse", "--git-common-dir"],
                        capture_output=True, text=True)
    if cp.returncode != 0 or not cp.stdout.strip():
        raise FleetError(f"{tree}: no git common directory: {cp.stderr.strip()[-200:]}")
    return os.path.realpath(os.path.join(tree, cp.stdout.strip()))


def base_key(tree):
    """Return the trees/<key> name of a checkout's repository base on this coordinator.

    The key hashes the coordinator's hostname and the repository's git
    common directory, so the main checkout and every worktree of the
    repository share one base per host.
    """
    raw = f"{os.uname().nodename}\0repo\0{repo_dir(tree)}".encode()
    return hashlib.sha256(raw).hexdigest()[:16]


def parse_tree_src(line):
    """Parse one TREE_LIST_SCRIPT line into a dict."""
    return parse_kv(line.replace("\t", "\n"))


def list_trees(host, sizes=False):
    """Return the parsed trees/<key>.src records on a host, with bytes= and present= when sizes."""
    cp = host.run_script(TREE_LIST_SCRIPT, [host.fleet_root, int(sizes)], timeout=600 if sizes else 60)
    if cp.returncode != 0:
        raise FleetError(f"{host.name}: listing trees failed: {cp.stderr.strip()[-200:]}")
    return [parse_tree_src(line) for line in cp.stdout.splitlines() if line.strip()]


def trees_to_remove(host, tree_path=None, now=None):
    """Return the tree keys on a host this coordinator may remove (stale, or those of tree_path).

    A repository base (its record names repo=) is stale when the
    repository's git common directory is gone or no run refreshed it for
    TREE_MAX_AGE_S; tree_path never selects it, since every checkout of the
    repository shares it. A checkout-keyed tree (no repo=, written before
    bases were keyed per repository) is stale when its checkout path is gone
    or unused for TREE_MAX_AGE_S, and tree_path selects those of that path.
    """
    me, now = os.uname().nodename, now or time.time()
    keys = []
    for src in list_trees(host):
        if src.get("coordinator") != me:
            continue
        path, repo = src.get("path", ""), src.get("repo")
        if tree_path is not None:
            if not repo and os.path.abspath(path) == os.path.abspath(tree_path):
                keys.append(src["key"])
            continue
        age = now - float(src.get("last-run") or 0)
        if not os.path.exists(repo or path) or age > TREE_MAX_AGE_S:
            keys.append(src["key"])
    return keys


def clean_host(host, mode="stale", tree_path=None, held=False):
    """Run clean on one host; mode stale, all or tree. Returns the script's exit code."""
    engine = host.cfg.get("engine") or ("podman" if not host.local else (local_engine() or "podman"))
    try:
        keys = trees_to_remove(host, tree_path if mode == "tree" else None)
        if mode == "all":
            keys = []  # the whole trees/ directory goes
        cp = host.run_script(CLEAN_SCRIPT, [host.fleet_root, engine, mode, int(held), IMAGE_TAG,
                                            *keys], timeout=600)
    except FleetError as e:
        print(f"[fleet] {host.name}: clean skipped: {e}")
        return EXIT_SKIP
    for line in cp.stdout.splitlines():
        print(f"[fleet] {host.name}: {line}")
    if cp.returncode not in (0, 3):
        print(f"[fleet] {host.name}: clean exited {cp.returncode}: {cp.stderr.strip()[-300:]}")
    return cp.returncode


def cmd_clean(a):
    """Remove stale runs, trees, containers and images (or everything with --all) per host."""
    _path, hosts = load_hosts()
    sel = select_hosts(hosts, a.hosts) if a.hosts else [
        Host(n, c) for n, c in hosts.items() if c.get("enabled", True)]
    if a.all and a.tree:
        raise FleetError("clean takes --all or --tree, not both", 2)
    mode = "all" if a.all else ("tree" if a.tree else "stale")
    worst = 0
    for h in sel:
        rc = clean_host(h, mode, a.tree)
        worst = max(worst, 1 if rc not in (0, 3, EXIT_SKIP) else 0)
        if h.local and mode != "tree":
            clean_stream_dirs(script_repo(), expand_root(h.fleet_root))
    return worst


def clean_stream_dirs(tree, root):
    """Remove the checkout's build/fleet-stream/<run id> directories whose local run directory is gone."""
    base = os.path.join(tree, STREAM_DIR)
    if not os.path.isdir(base):
        return
    for run_id in sorted(os.listdir(base)):
        if not os.path.isdir(os.path.join(root, "runs", run_id)):
            shutil.rmtree(os.path.join(base, run_id), ignore_errors=True)
            print(f"[fleet] local: removed {os.path.join(STREAM_DIR, run_id)}")


# ---- run: host-side scripts -------------------------------------------------

# The unit supervisor, shipped to <run dir>/ship/supervise.sh. It runs one job
# (the engine build, one unit or a canary) detached from the coordinator,
# writes <attempt>.pid (PID and start time) at launch and <attempt>.done
# (rc, status, wall time, candidate bytes) on every exit, and deletes nothing:
# the coordinator pulls results and removes the run directory.
SUPERVISOR = r'''#!/usr/bin/env bash
set -u
source "$1"
T0=$(date +%s)
mkdir -p "$UNIT_DIR"
''' + SH_STARTTIME + r'''
echo "$$ $(starttime $$)" >"$ATTEMPT_BASE.pid"
# MemAvailable at 1 Hz for the attempt's life; its minimum enters the done marker.
MEMLOG="$ATTEMPT_BASE.memavail"
while :; do awk '/^MemAvailable:/{print $2; exit}' /proc/meminfo; sleep 1; done >"$MEMLOG" 2>/dev/null &
MEMPID=$!
exec 7>>"$RUN_DIR/active.lock"
flock -s 7
helpers() {
    local p
    for p in /proc/[0-9]*; do
        [ -O "$p" ] && [ "$(cat "$p/comm" 2>/dev/null)" = dbus-daemon ] && echo "${p#/proc/} $(starttime "${p#/proc/}")"
    done | sort
}
BEFORE="$(helpers)"
# Helpers that appeared during this job are attributed to it only when no
# running container on the host lacks the fleet label; another engine user's
# helper is recorded unattributed and never signalled.
record_helpers() {
    local flag=attributed foreign
    foreign="$("$ENGINE" ps --format '{{index .Labels "org.openpete.fleet"}}' 2>/dev/null | grep -c '^$\|^<no value>$')"
    [ "${foreign:-0}" = 0 ] || flag=unattributed
    comm -13 <(echo "$BEFORE") <(helpers) | sed "s/\$/ $flag/" >>"$RUN_DIR/helpers.tsv"
}
finish() {  # rc status
    local bytes=0 wall mem
    wall=$(( $(date +%s) - T0 ))
    kill "$MEMPID" 2>/dev/null
    mem="$(sort -n "$MEMLOG" 2>/dev/null | head -n1)"
    [ -d "$UNIT_DIR/cand" ] && bytes="$(du -sb "$UNIT_DIR/cand" | cut -f1)"
    record_helpers
    printf 'rc=%s\nstatus=%s\nattempt=%s\nwall=%s\ncand-bytes=%s\nmin-memavail-kib=%s\n' \
        "$1" "$2" "$ATTEMPT" "$wall" "$bytes" "${mem:-0}" >"$ATTEMPT_BASE.done.tmp"
    mv -f "$ATTEMPT_BASE.done.tmp" "$ATTEMPT_BASE.done"
    exit 0
}
if [ "$LOCK_MODE" = take ]; then
    exec 8>>"$FLEET_ROOT/golden.lock"
    flock -n 8 || finish 75 lock-busy
    : >"$FLEET_ROOT/golden.lock"
    echo "pid $$ host $(uname -n) golden-fleet $RUN_ID $JOB since $(date -Is)" >&8
fi
export OPENPETE_FLEET_ROOT="$FLEET_ROOT" OPENPETE_GOLDEN_LOCK_HELD="$FLEET_ROOT/golden.lock"
export OPENPETE_CAPTURE_IMAGE="$IMAGE" OPENPETE_CAPTURE_LABEL="$RUN_ID"
export OPENPETE_CAPTURE_MOUNT_AS="$MOUNT_AS" OPENPETE_CONTAINER_ENGINE="$ENGINE"
# The wrapper creates and mounts the reference store on every entry (build
# included); a per-job store keeps it under fleet_root.
export OPENPETE_GOLDEN_REF_DIR="$UNIT_DIR/store"
mkdir -p "$UNIT_DIR/store"
[ -n "${PIN_LP_NUM_THREADS:-}" ] && export LP_NUM_THREADS="$PIN_LP_NUM_THREADS"
cd "$RUN_DIR/tree" || finish 1 no-tree
case "$KIND" in
build)
    nice -n "$NICE" scripts/golden-capture-container.sh build >"$UNIT_DIR/build.log" 2>&1 \
        || finish 1 build-failed
    for b in $BINARIES; do [ -f "$b" ] && sha256sum "$b"; done >"$UNIT_DIR/binaries.sha256"
    [ -s "$UNIT_DIR/binaries.sha256" ] || finish 1 no-binaries
    finish 0 built ;;
unit|canary)
    cand="$UNIT_DIR/cand"
    cp "$RUN_DIR/ship/fields-$GAME.txt" "$UNIT_DIR/fields.txt" || finish 1 no-fields
    export PRESENTATION_GOLDEN_OUT="$cand"
    export CORPUS_JOBS="$SLOTS" PRESENTATION_LEG_TIMEOUT="$LEG_TIMEOUT" PRESENTATION_TIER="$TIER"
    # tests/harness/check_dragon_golden.sh captures the dragons tier at stock LOD only.
    [ "$TIER" = dragons ] && export OPENPETE_CAP_LODS=original
    export OPENPETE_CAPTURE_FIELDS="$UNIT_DIR/fields.txt" OPENPETE_CAPTURE_BINARIES="$RUN_DIR/ship/binaries.sha256"
    # Row resume: an attempt after an interrupted one in the same unit
    # directory imports the rows the earlier attempt stamped.
    [ "$RESUME" = 1 ] && export OPENPETE_RESUME_RUN="$RUN_ID"
    # Streamed verdict lines: each finished row's tiles, into the attempt's
    # own file, which the coordinator reads by offset at every poll.
    [ "${STREAM:-0}" = 1 ] && export OPENPETE_STREAM_FILE="$ATTEMPT_BASE.stream"
    [ -n "${STREAM_POKE:-}" ] && export OPENPETE_STREAM_POKE="$STREAM_POKE"
    nice -n "$NICE" tests/harness/check_presentation_golden.sh --game "$GAME" $FILTERS >"$UNIT_DIR/capture.log" 2>&1
    crc=$?
    echo "capture-rc=$crc" >>"$UNIT_DIR/capture.log"
    [ "$crc" = 64 ] && finish 64 capture-skip
    if [ -n "${POKE:-}" ]; then
        scripts/golden-capture-container.sh run -- python3 -c '
import glob, sys
from PIL import Image
fs = sorted(glob.glob(sys.argv[1] + "/*" + sys.argv[2] + "*-vk.png"))
if not fs:
    sys.exit("poke: no tile matches " + sys.argv[2])
im = Image.open(fs[0]).convert("RGB")
p = im.getpixel((0, 0))
im.putpixel((0, 0), ((p[0] + 128) % 256, p[1], p[2]))
im.save(fs[0])
print("poked", fs[0])' "$cand" "$POKE" >>"$UNIT_DIR/capture.log" 2>&1 || finish 1 poke-failed
    fi
    [ "$KIND" = canary ] && finish "$crc" canary-captured
    # A filtered unit is decided by its capture's own verdict (out-of-scope
    # tiles are not missing); --verdict-only takes the whole corpus only.
    if [ -n "$FILTERS" ]; then
        [ -f "$cand/verdict-report.txt" ] || finish "$([ "$crc" = 64 ] && echo 64 || echo 1)" no-report
        finish "$crc" verdict
    fi
    unset OPENPETE_STREAM_FILE OPENPETE_STREAM_POKE
    nice -n "$NICE" tests/harness/check_presentation_golden.sh --verdict-only "$cand" --game "$GAME" \
        >"$UNIT_DIR/verdict.log" 2>&1
    vrc=$?
    [ -f "$cand/verdict-report.txt" ] || finish "$([ "$vrc" = 64 ] && echo 64 || echo 1)" no-report
    finish "$vrc" verdict ;;
*)
    finish 1 "unknown-kind-$KIND" ;;
esac
'''

# Launch a supervisor detached, all descriptors redirected, so ssh returns at
# once. With lingering on, the supervisor is a transient unit of the user's
# systemd manager: logind with KillUserProcesses=yes (SteamOS) kills every
# process of an ssh session's scope when the session closes, setsid or not,
# and a unit under user@.service is outside that scope. systemd already makes
# the unit's main process a session leader, so it runs bash directly: setsid
# there would fork, the unit would end with its main process, and systemd
# would kill the forked child with the unit's cgroup. Without lingering the
# user manager stops with the last session, so setsid is used.
# args: supervisor envfile logfile KEY=VALUE...
LAUNCH_SCRIPT = r'''
set -eu
sup="$1"; envf="$2"; log="$3"; shift 3
mkdir -p "$(dirname "$envf")"
: >"$envf.tmp"
for kv in "$@"; do printf 'export %s=%q\n' "${kv%%=*}" "${kv#*=}" >>"$envf.tmp"; done
mv -f "$envf.tmp" "$envf"
if [ "$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)" = yes ] \
        && command -v systemd-run >/dev/null 2>&1; then
    unit="openpete-fleet-$(basename "$envf" .env | tr -c 'A-Za-z0-9_.-' '_')-$$"
    systemd-run --user --collect --quiet --unit="$unit" \
        bash -c 'exec bash "$0" "$1" >"$2" 2>&1 </dev/null' "$sup" "$envf" "$log"
    echo "launched $(systemctl --user show -p MainPID --value "$unit") unit $unit"
else
    setsid nohup bash "$sup" "$envf" >"$log" 2>&1 </dev/null &
    echo "launched $!"
fi
'''

# Report each attempt's state and the new bytes of its stream file, in two
# framed sections per attempt: `state <base> done <k=v ...>`, `running`,
# `dead` (PID gone or reused) or `starting` (no PID file yet); then
# `stream <base> <n>` followed by the n bytes of <base>.stream from the
# offset. The done marker is read before the stream's size, so a done
# attempt's bytes hold every line it wrote. args: <attempt base> <offset> pairs.
POLL_SCRIPT = SH_STARTTIME + r'''
set -u
while [ $# -ge 2 ]; do
    b="$1"; off="$2"; shift 2
    if [ -f "$b.done" ]; then
        st="done $(tr '\n' ' ' <"$b.done")"
    elif [ -f "$b.pid" ]; then
        read -r pid start <"$b.pid"
        if [ "$(starttime "$pid")" = "$start" ]; then st=running; else st=dead; fi
    else
        st=starting
    fi
    echo "state $b $st"
    n=0
    [ -f "$b.stream" ] && n=$(( $(stat -c %s "$b.stream") - off ))
    [ "$n" -gt 0 ] || n=0
    echo "stream $b $n"
    [ "$n" -gt 0 ] && tail -c +"$((off + 1))" "$b.stream" | head -c "$n"
done
exit 0
'''

# Stop an attempt: every process of its supervisor's session (the supervisor
# runs under setsid, so the session id is its PID; a member started no
# earlier than the recorded start time, so a reused PID never matches), then
# its containers by the run label; then report whether the host golden lock
# is free. args: engine run-id root check-lock attempt-bases...
KILL_SCRIPT = SH_STARTTIME + r'''
set -u
engine="$1"; run="$2"; root="$3"; check="$4"; shift 4
session_members() {  # sid start
    local sid="$1" start="$2" p s f
    for p in /proc/[0-9]*; do
        s="$(cat "$p/stat" 2>/dev/null)" || continue
        read -r -a f <<<"${s##*) }"
        # f[3] is the session id, f[19] the start time (stat fields 6 and 22).
        [ "${f[3]}" = "$sid" ] && [ "${f[19]}" -ge "$start" ] && echo "${p#/proc/}"
    done
}
for b in "$@"; do
    [ -f "$b.pid" ] || continue
    read -r pid start <"$b.pid"
    members="$(session_members "$pid" "$start")"
    [ -n "$members" ] || continue
    kill -TERM $members 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -n "$(session_members "$pid" "$start")" ] || break
        sleep 1
    done
    left="$(session_members "$pid" "$start")"
    [ -n "$left" ] && kill -KILL $left 2>/dev/null
    echo "stopped session $pid: $(echo $members | wc -w) process(es)"
done
for c in $("$engine" ps -aq --filter "label=org.openpete.fleet=$run" 2>/dev/null); do
    "$engine" rm -f "$c" >/dev/null 2>&1 && echo "removed container $c"
done
if [ "$check" = 1 ]; then
    if [ ! -e "$root/golden.lock" ] || flock -n "$root/golden.lock" true; then echo "lock=free"; else echo "lock=busy"; fi
else
    echo "lock=free"
fi
'''

# Delete a run directory: signal its attributed helpers, then remove it;
# names listed after the directory are kept (the coordinator's own files on
# the local host). args: run-dir keep...
DELETE_SCRIPT = SH_KILL_HELPERS + r'''
set -u
d="$1"; shift
[ -d "$d" ] || exit 0
kill_helpers "$d"
if [ $# -eq 0 ]; then rm -rf "$d"; exit 0; fi
for e in "$d"/* "$d"/.[!.]*; do
    [ -e "$e" ] || continue
    keep=0
    for k in "$@"; do [ "${e##*/}" = "$k" ] && keep=1; done
    [ $keep = 1 ] || rm -rf "$e"
done
'''

# Refresh trees/<key> from this run's snapshot by hard links, under the
# key's lock held exclusive (a sync reading the base holds it shared), and
# record who owns it. The base is replaced by a rename, never written in
# place, so a snapshot linked from the old base keeps its content. A
# snapshot without a build tree (a remote build's host before it builds)
# keeps the base's previous build tree, which seeds the next build, only
# when the previous record names the same checkout path: a build tree of
# another checkout's sources can hold objects newer than this checkout's
# changed sources. args: root key run-id src-lines...
REFRESH_BASE = r'''
set -eu
root="$1"; key="$2"; run="$3"; shift 3
bt=build/linux-release-container
path=""
for l in "$@"; do case "$l" in path=*) path="$l";; esac; done
mkdir -p "$root/trees"
exec 6>"$root/trees/$key.lock"
flock 6
rm -rf "$root/trees/$key.new"
cp -al "$root/runs/$run/tree" "$root/trees/$key.new"
if [ ! -d "$root/trees/$key.new/$bt" ] && [ -d "$root/trees/$key/$bt" ] \
   && grep -qxF -- "$path" "$root/trees/$key.src" 2>/dev/null; then
    mkdir -p "$root/trees/$key.new/build"
    mv "$root/trees/$key/$bt" "$root/trees/$key.new/$bt"
fi
# The container home and the disc-hash memo are written in place by units.
rm -rf "$root/trees/$key.new/build/linux-release-container/home" \
       "$root/trees/$key.new/build/linux-release-container/golden-disc-hash.tsv"
rm -rf "$root/trees/$key"
mv "$root/trees/$key.new" "$root/trees/$key"
printf '%s\n' "$@" >"$root/trees/$key.src"
'''

# Copy the base's build tree into the snapshot as real files (a build writes
# its objects in place, which would write through hard links into the base),
# under the key's lock held shared, only when the base's record names this
# checkout path (see REFRESH_BASE). args: root key run-id checkout-path
SEED_BUILD = r'''
set -eu
root="$1"; key="$2"; run="$3"; path="$4"
src="$root/trees/$key/build/linux-release-container"
dst="$root/runs/$run/tree/build/linux-release-container"
mkdir -p "$root/trees"
exec 6>"$root/trees/$key.lock"
flock -s 6
if ! grep -qxF -- "path=$path" "$root/trees/$key.src" 2>/dev/null; then
    echo "no-seed (the base is another checkout's)"
elif [ -d "$src" ] && [ ! -e "$dst" ]; then
    mkdir -p "$(dirname "$dst")"
    cp -a "$src" "$dst"
    echo "seeded"
else
    echo "no-seed"
fi
'''

# Signal every process of this user whose environment holds RUN_TAG_VAR=<run
# id> (the run's ssh scripts, image load and rsync servers, with their
# children): SIGTERM, then SIGKILL after 10 s. The script itself runs untagged.
# args: tag-variable run-id
REAP_SCRIPT = r'''
set -u
tag="$1=$2"
tagged() {
    local p
    for p in /proc/[0-9]*; do
        [ "${p#/proc/}" = $$ ] && continue
        tr '\0' '\n' 2>/dev/null <"$p/environ" | grep -qxF -- "$tag" && echo "${p#/proc/}"
    done
}
m="$(tagged)"
[ -n "$m" ] || { echo "reaped 0 process(es)"; exit 0; }
kill -TERM $m 2>/dev/null
for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -n "$(tagged)" ] || break
    sleep 1
done
left="$(tagged)"
[ -n "$left" ] && kill -KILL $left 2>/dev/null
echo "reaped $(echo $m | wc -w) process(es)"
'''

# sha256 of each named file under a directory. args: dir files...
HASH_SCRIPT = r'''
set -u
d="$1"; shift
cd "$d" || exit 1
for f in "$@"; do
    if [ -f "$f" ]; then sha256sum "$f"; else echo "absent  $f"; fi
done
'''


# ---- run: units, ledger and timing ------------------------------------------

def load_pb(tree):
    """Import the tree's scripts/presentation-blessed.py as a module."""
    path = os.path.join(tree, "scripts", "presentation-blessed.py")
    spec = importlib.util.spec_from_file_location("presentation_blessed", path)
    mod = importlib.util.module_from_spec(spec)
    # Registered by name: digest_files pickles its worker function for a
    # process pool, and pickle resolves functions through sys.modules.
    sys.modules["presentation_blessed"] = mod
    spec.loader.exec_module(mod)
    return mod


def load_census(tree):
    """Import the tree's scripts/golden-census.py as a module."""
    if "golden_census" in sys.modules:
        return sys.modules["golden_census"]
    path = os.path.join(tree, "scripts", "golden-census.py")
    spec = importlib.util.spec_from_file_location("golden_census", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["golden_census"] = mod
    spec.loader.exec_module(mod)
    return mod


def select_units(a):
    """Return the (game, tier) units a run's arguments name, dragons first when present."""
    games = GAMES if a.all else (a.game,)
    tiers = ("dragons", "presentation") if a.all_tiers else (a.tier,)
    units = [(g, t) for t in tiers for g in games if g in TIER_GAMES[t]]
    if not units:
        raise FleetError(f"no unit: tier {a.tier} has no game {a.game}", 2)
    return units


def store_dir():
    """Return the local reference store path the harness resolves."""
    base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.expanduser(os.environ.get("OPENPETE_GOLDEN_REF_DIR")
                              or os.path.join(base, "openpete", "golden-ref"))


def ledger_path():
    """Return the unit-time ledger path, beside the local store."""
    return os.path.join(os.path.dirname(os.path.abspath(store_dir())), "fleet-unit-times.tsv")


def read_ledger():
    """Return {(game, tier): latest ledger row} from the unit-time ledger."""
    rows = {}
    try:
        with open(ledger_path(), encoding="utf-8") as f:
            for line in f:
                if line.startswith("#") or not line.strip():
                    continue
                row = dict(zip(LEDGER_COLS, line.rstrip("\n").split("\t")))
                rows[(row.get("game"), row.get("tier"))] = row
    except OSError:
        pass
    return rows


def append_ledger(row):
    """Append one accepted unit's time to the ledger."""
    path = ledger_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    new = not os.path.exists(path)
    with open(path, "a", encoding="utf-8") as f:
        if new:
            f.write("# " + "\t".join(LEDGER_COLS) + "\n")
        f.write("\t".join(str(row.get(k, "")) for k in LEDGER_COLS) + "\n")


def expected_s(unit, ledger):
    """Return a unit's expected wall time at speed 1.00: ledger, else scaled from the size table."""
    row = ledger.get(unit)
    if row and row.get("norm_s"):
        return float(row["norm_s"])
    return REF_UNIT_MIN * 60 * UNIT_MB[unit] / UNIT_MB[REF_UNIT]


def cand_bytes(unit, ledger):
    """Return a unit's candidate-set bytes: the last run's, else the size table."""
    row = ledger.get(unit)
    if row and row.get("cand_bytes"):
        return int(row["cand_bytes"])
    return UNIT_MB[unit] * 10**6


def takeover_prediction(done, total, rows, norm_s, cand_b, src_speed, src_slots, dst_speed, hops):
    """Return the predicted remaining wall times of a running unit on its host and on a takeover host, and the saving.

    left = 1 - done / total is the unit's share not yet streamed (tiles). On
    the source, left x norm_s / src_speed counts the rows in flight as not
    started; they are on average half done, so their half is subtracted:
    in_flight = norm_s x min(src_slots, left x rows) / (2 x rows x src_speed),
    one row taking norm_s x src_slots / (rows x src_speed) of wall at
    src_slots rows in parallel. The takeover host recaptures every row not
    stamped, rows in flight included: left x norm_s / dst_speed, plus the
    cost TAKEOVER_FIXED_S + hops x (done / total) x cand_b / CARRY_BPS.
    @param done tiles the running attempt has streamed (imported included)
    @param total blessed tiles of the unit
    @param rows corpus rows of the unit
    @param norm_s the unit's expected wall time at speed 1.00
    @param cand_b the unit's candidate-set bytes
    @param src_speed the running host's speed
    @param src_slots the running host's slots
    @param dst_speed the free host's speed
    @param hops transfers of the carried rows (0 to 2)
    @return dict of left, src_s, dst_s, cost_s, carry_s, saving_s
    """
    left = max(0.0, 1.0 - done / total) if total else 1.0
    in_flight = norm_s * min(src_slots, left * rows) / (2 * rows * src_speed) if rows else 0.0
    src_s = max(0.0, left * norm_s / src_speed - in_flight)
    carry_s = hops * (1.0 - left) * cand_b / CARRY_BPS
    cost_s = TAKEOVER_FIXED_S + carry_s
    dst_s = left * norm_s / dst_speed + cost_s
    return {"left": left, "src_s": src_s, "dst_s": dst_s, "cost_s": cost_s, "carry_s": carry_s,
            "saving_s": src_s - dst_s}


def takeover_refusal(pred, src_speed, dst_speed):
    """Return why a takeover does not pay ('' when it does): a slower or equal host, a remainder under the cost, a saving under the threshold."""
    if dst_speed <= src_speed:
        return f"speed {dst_speed:.2f} is not above the source's {src_speed:.2f}"
    if pred["src_s"] <= pred["cost_s"]:
        return f"remaining {pred['src_s']:.0f} s is under the takeover cost {pred['cost_s']:.0f} s"
    if pred["saving_s"] <= TAKEOVER_MIN_SAVING_S:
        return f"saving {pred['saving_s']:.0f} s is under {TAKEOVER_MIN_SAVING_S} s"
    return ""


def lp_threads(info, slots):
    """Return LP_NUM_THREADS for a host: $LP_NUM_THREADS, else ceil(cores / slots).

    Rounding up keeps every core busy when slots does not divide the core
    count; 6 slots x 3 threads measured faster than 6 x 2 or 8 x 2 on 16 cores.
    """
    pinned = os.environ.get("LP_NUM_THREADS")
    if pinned:
        return pinned
    return str(max(1, -(-int(info.get("nproc") or os.cpu_count() or 1) // int(slots))))


def leg_timeout(speed):
    """Return PRESENTATION_LEG_TIMEOUT for a host: the default 300 s / speed x 1.5."""
    return int(round(LEG_TIMEOUT_S / speed * 1.5))


# ---- run: sync set ----------------------------------------------------------

def git_z(path, *args):
    """Return git's NUL-separated output lines for a command run in path."""
    out = subprocess.run(["git", "-C", path, *args], capture_output=True, check=True).stdout
    return [p.decode() for p in out.split(b"\0") if p]


def git_files(tree, prefix=""):
    """Return tracked plus non-ignored untracked files of tree/prefix and its checked-out submodules."""
    base = os.path.join(tree, prefix) if prefix else tree
    links = {e.split("\t", 1)[1] for e in git_z(base, "ls-files", "-s", "-z")
             if e.startswith("160000 ")}
    out = []
    for rel in git_z(base, "ls-files", "-co", "--exclude-standard", "-z"):
        full = os.path.join(prefix, rel) if prefix else rel
        if rel in links:
            if os.path.exists(os.path.join(base, rel, ".git")):
                out += git_files(tree, full)
        elif os.path.isfile(os.path.join(tree, full)):
            out.append(full)
    return out


def walk_files(tree, rel, skip_top=()):
    """Return every file under tree/rel (symlinks followed), tree-relative, minus skipped top entries."""
    top = os.path.join(tree, rel)
    out = []
    for d, dirs, files in os.walk(top, followlinks=True):
        sub = os.path.relpath(d, top)
        if sub == ".":
            dirs[:] = [x for x in dirs if x not in skip_top]
            files = [x for x in files if x not in skip_top]
        out += [os.path.normpath(os.path.join(rel, sub, f)) for f in files
                if os.path.isfile(os.path.join(d, f))]
    return out


def disc_paths(tree, pb, game):
    """Return the tree-relative cue and the bin the engine loads for it."""
    cue = pb.source_conf(f"games/{game}/oracle.conf", ("DISC",))["DISC"]
    if not cue or not os.path.isfile(os.path.join(tree, cue)):
        raise FleetError(f"{game}: no disc cue {cue or '(DISC unset)'} in {tree}", EXIT_SKIP)
    bin_rel = pb.resolve_disc_bin(cue)
    if not bin_rel:
        raise FleetError(f"{game}: no .bin resolves for {cue}", EXIT_SKIP)
    return os.path.normpath(cue), bin_rel


def source_set(tree, pb, games):
    """Return the positive sync set without the build tree: git files plus the listed ignored inputs."""
    files = set(git_files(tree))
    for g in GAMES:
        files.update(walk_files(tree, f"games/{g}/generated"))
        for t in ("globals.toml", "symbols.toml"):
            rel = f"games/{g}/manifest/{t}"
            if os.path.isfile(os.path.join(tree, rel)):
                files.add(rel)
    for g in games:
        files.update(disc_paths(tree, pb, g))
    files.update(walk_files(tree, "third_party/psxrecomp/build"))
    return sorted(files)


def build_set(root):
    """Return the build tree's files under root minus the container home and the disc-hash memo."""
    return sorted(walk_files(root, BUILD_TREE, skip_top=("home", "golden-disc-hash.tsv")))


def set_bytes(root, files):
    """Return the summed size of a file list under root."""
    return sum(os.path.getsize(os.path.join(root, f)) for f in files)


def write_list(path, files):
    """Write a NUL-separated --files-from list."""
    with open(path, "wb") as f:
        f.write(b"".join(x.encode() + b"\0" for x in files))


def binary_rels():
    """Return the build tree's tree-relative executable and every game's recomp module.

    The tree is a shared-linkage Linux build: one executable, `openpete` at its
    root, with each game's <tag>-recomp.so beside it."""
    # rumble-guard: no-launch  input-lock-guard: no-launch  sandbox-guard: no-launch  (the path is hashed and synced; the
    # coordinator never runs the binary, the capture image's supervisor does)
    return [f"{BUILD_TREE}/openpete"] + [f"{BUILD_TREE}/{g.replace('-', '')}-recomp.so" for g in GAMES]


def binary_paths(root):
    """Return the tree-relative executable and recomp modules present under root's build tree."""
    return [r for r in binary_rels() if os.path.isfile(os.path.join(root, r))]


def sha256_path(path):
    """Return a file's sha256, or '' when absent."""
    if not os.path.isfile(path):
        return ""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---- run: transfer ----------------------------------------------------------

def rsync_base(host, server_lock=None):
    """Return rsync's transport arguments for a host; a running run's remote rsync server carries its tag.

    server_lock names a lock file on a remote host that the rsync server
    holds shared (flock -s) for its lifetime, its directory created first.
    """
    if host.local:
        return []
    tag = f"{RUN_TAG_VAR}={shlex.quote(_run_tag)} " if _run_tag else ""
    if server_lock:
        lock = shlex.quote(server_lock)
        path = [f"--rsync-path=mkdir -p {shlex.quote(os.path.dirname(server_lock))} && "
                f"{tag}flock -s {lock} rsync"]
    else:
        path = [f"--rsync-path={tag}rsync"] if tag else []
    return ["-e", "ssh " + " ".join(SSH_OPTS), *path]


def remote_path(host, path):
    """Return an rsync operand naming path on the host."""
    return path if host.local else f"{host.cfg['ssh']}:{path}"


def rsync_push(host, src, listfile, dest, link_dest=None, base_lock=None):
    """Copy a --files-from list from local src to dest on the host (symlinks dereferenced).

    With link_dest, a file whose content (--checksum, not size and mtime)
    equals the base's is not sent: rsync hard-links it from the base when
    its attributes match, else copies it there from the base. base_lock is
    the base's lock file, held shared on the host while rsync reads the base.
    """
    lock = base_lock if link_dest else None
    argv = ["rsync", "-a", "-L", "--from0", f"--files-from={listfile}", "--stats",
            *rsync_base(host, lock)]
    if link_dest:
        argv += ["--checksum", f"--link-dest={link_dest}"]
    argv += [src.rstrip("/") + "/", remote_path(host, dest.rstrip("/")) + "/"]
    if lock and host.local:
        os.makedirs(os.path.dirname(lock), exist_ok=True)
        argv = ["flock", "-s", lock, *argv]
    t0 = time.monotonic()
    cp = transport_run(host, f"rsync to {dest}", argv, text=True)
    if cp.returncode != 0:
        raise FleetError(f"{host.name}: rsync to {dest} exited {cp.returncode}: "
                         f"{cp.stderr.strip()[-400:]}")
    sent = re.search(r"Total bytes sent: ([\d,]+)", cp.stdout)
    return time.monotonic() - t0, int(sent.group(1).replace(",", "")) if sent else 0


def rsync_recheck(host, src, listfile, dest):
    """Return the content changes between local src and dest over a list (empty when equal).

    A connect failure is retried (transport_run) and, once the retries are
    spent, raised naming the host: an unreachable host is not a torn copy.
    """
    argv = ["rsync", "-n", "-i", "-c", "-L", "--from0", f"--files-from={listfile}",
            *rsync_base(host), src.rstrip("/") + "/", remote_path(host, dest.rstrip("/")) + "/"]
    cp = transport_run(host, f"recheck of {dest}", argv, text=True)
    line = connect_failure(cp)
    if line and not host.local:
        raise FleetError(f"{host.name}: recheck of {dest} could not connect after "
                         f"{1 + len(TRANSPORT_BACKOFF_S)} attempts: {line}")
    changes = [ln for ln in cp.stdout.splitlines()
               if ln[:1] in "<>ch*" and ln[1:2] in "fLS" or ln.startswith("*deleting")]
    if cp.returncode != 0:
        changes.append(f"rsync exited {cp.returncode}: {cp.stderr.strip()[-300:]}")
    return changes


def link_or_copy(src, dst):
    """Replace dst with a hard link to src, or a copy where the link fails (another filesystem); return dst."""
    if os.path.lexists(dst):
        os.unlink(dst)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)
    return dst


def rsync_pull(host, src_dir, dst_dir, names=None, includes=None):
    """Copy named files (or include patterns, or everything) from src_dir on the host to dst_dir."""
    os.makedirs(dst_dir, exist_ok=True)
    argv = ["rsync", "-a", *rsync_base(host)]
    lst = None
    if names is not None:
        lst = os.path.join(dst_dir, ".pull-list")
        write_list(lst, names)
        argv += ["--from0", f"--files-from={lst}"]
    for pat in includes or ():
        argv.append(f"--include={pat}")
    if includes:
        argv.append("--exclude=*")
    argv += [remote_path(host, src_dir.rstrip("/")) + "/", dst_dir.rstrip("/") + "/"]
    cp = transport_run(host, f"rsync from {src_dir}", argv, text=True)
    if lst:
        os.unlink(lst)
    return cp.returncode, cp.stderr.strip()[-300:]


def on_hosts(hosts, fn):
    """Run fn(hs) for every host, each in its own thread, and return the results in host order.

    Nothing in a host's sync or recheck reads another host's state, so the
    hosts run concurrently. While they run, the stop predicate run_proc
    polls also holds once any thread has failed or the caller was
    interrupted, so every other thread's child (rsync, ssh) is killed at its
    next STOP_POLL_S poll; the first failure is raised after every thread
    has ended.
    @param hosts the HostState list
    @param fn the per-host function
    @return fn's results, in the order of hosts
    """
    if len(hosts) < 2:
        return [fn(hs) for hs in hosts]
    abort, guard, first = threading.Event(), threading.Lock(), []
    results = [None] * len(hosts)
    prev = _stop_watch

    def work(i, hs):
        try:
            results[i] = fn(hs)
        except BaseException as e:  # re-raised in the calling thread
            with guard:
                if not abort.is_set():
                    first.append(e)
                abort.set()

    with stop_watch(lambda: abort.is_set() or (prev is not None and prev())):
        threads = [threading.Thread(target=work, args=(i, hs), daemon=True)
                   for i, hs in enumerate(hosts)]
        for t in threads:
            t.start()
        try:
            for t in threads:
                while t.is_alive():
                    t.join(0.5)
        except BaseException:
            abort.set()
            for t in threads:
                t.join()
            raise
    if first:
        raise first[0]
    return results


# ---- run: the checkout-reading span -------------------------------------------

class CheckoutSpan:
    """Marker process plus the checkout's container build lock while the fleet reads the checkout."""

    def __init__(self, tree):
        """Bind the checkout; nothing is held yet."""
        self.tree = tree
        self.marker = None
        self.lock_fd = None

    def start_marker(self):
        """Start the `golden-fleet-sync` marker process a build lock tests for."""
        if self.marker is None:
            self.marker = subprocess.Popen(
                [sys.executable, "-c", "import time\nwhile True: time.sleep(3600)",
                 "golden-fleet-sync", self.tree], cwd=self.tree,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def take_build_lock(self):
        """Take build/linux-release-container.build-lock, waiting while a build holds it (a stop request ends the wait)."""
        path = os.path.join(self.tree, "build", "linux-release-container.build-lock")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.lock_fd = os.open(path, os.O_WRONLY | os.O_CREAT, 0o644)
        waiting = False
        while True:
            try:
                fcntl.flock(self.lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return
            except BlockingIOError:
                if not waiting:
                    print(f"[fleet] {path} held by a build; waiting")
                    waiting = True
            stop_point()
            time.sleep(STOP_POLL_S)

    def release(self):
        """Release the build lock and stop the marker."""
        if self.lock_fd is not None:
            os.close(self.lock_fd)
            self.lock_fd = None
        if self.marker is not None:
            self.marker.terminate()
            self.marker.wait()
            self.marker = None


# ---- run: state ---------------------------------------------------------------

class Job:
    """One placement: the engine build, one host's canary, or a (game, tier) unit."""

    def __init__(self, kind, game=None, tier="presentation", pinned=None, slots=None, label=None):
        """Bind the job's kind, unit, pinned host, and a calibration leg's slots and name."""
        self.kind, self.game, self.tier, self.pinned = kind, game, tier, pinned
        self.slots, self.label = slots, label
        self.attempts = 0
        self.moves = 0            # attempts ended by a move, outside the MAX_ATTEMPTS budget
        self.moved = []           # (from host, to host) per move, in order
        self.carried = set()      # attempt numbers whose stamped rows a move carried on
        self.last_carry = (0, 0)  # (rows, -vk.png tiles) of the last move's carry
        self.result = None
        self.atts = {}
        self.stopped = False
        self.candidate = None     # the stream directory an accepted pull kept as the run candidate

    @property
    def unit(self):
        """The (game, tier) key of a unit or canary."""
        return (self.game, self.tier)

    @property
    def name(self):
        """The job's directory name under the run directory."""
        if self.label:
            return self.label
        if self.kind == "build":
            return "build"
        if self.kind == "canary":
            return f"canary-{self.pinned}"
        return f"{self.game}-{self.tier}"


class Attempt:
    """One placement of a job on a host, identified by its attempt number."""

    def __init__(self, job, hs, deadline_s):
        """Start the attempt's clocks."""
        job.attempts += 1
        self.job, self.hs, self.n = job, hs, job.attempts
        self.t0 = time.monotonic()
        self.t_end = None         # coordinator clock at its done marker, kill or loss
        self.deadline = self.t0 + deadline_s
        self.lost_since = None
        self.stream_off = 0       # bytes of <base>.stream read so far
        self.stream_tail = b""    # a line not yet complete at the last read
        self.lines = []           # parsed stream records, in receipt order
        self.backlog = []         # DIVERGE, EXTRA and EXCEED records not yet pulled
        job.atts[self.n] = self

    @property
    def unit_dir(self):
        """The job's directory on the host."""
        return f"{self.hs.run_dir}/u-{self.job.name}"

    @property
    def base(self):
        """The attempt's pid/done file prefix on the host."""
        return f"{self.unit_dir}/attempt-{self.n}"


class HostState:
    """A usable host within one run."""

    def __init__(self, host, info, slots, run_id):
        """Bind the host, its probe, its slots and its run directory."""
        self.host, self.info, self.slots = host, info, slots
        self.name = host.name
        self.root = info["root"]
        self.speed = float(host.cfg.get("speed", 1.0))
        self.run_dir = f"{self.root}/runs/{run_id}"
        self.nice = int(host.cfg.get("nice", 19 if host.local else 0))
        self.engine = info.get("engine") or "podman"
        self.current = None
        self.dropped = None
        self.touched = False


# ---- run: the coordinator -----------------------------------------------------

class Interrupted(FleetError):
    """A signal ended the run."""


class Coordinator:
    """One `run`: hosts, build, sync, placement, verdicts, pulls and cleanup."""

    # A re-queued attempt imports the rows its predecessor stamped.
    resume = True
    # A unit's capture streams its rows' verdict lines.
    stream = True
    # A resume coordinator (golden-fleet.py resume) finishes one stopped unit.
    resumed = False

    def __init__(self, a, run_id, local_run_dir):
        """Bind the arguments and the run's identity."""
        self.a, self.run_id = a, run_id
        self.tree = script_repo()
        self.pb = load_pb(self.tree)
        self.units = select_units(a)
        self.ledger = read_ledger()
        self.local_run_dir = local_run_dir
        self.ship_dir = os.path.join(local_run_dir, "ship")
        self.hosts = []
        self.sel = []
        self.golden_fd = None
        self.span = CheckoutSpan(self.tree)
        self.jobs = []
        self.rc_notes = []
        self.poke = self.parse_poke(os.environ.get("OPENPETE_FLEET_POKE", ""))
        self.row_poke = self.parse_poke(os.environ.get("OPENPETE_FLEET_ROW_POKE", ""))
        self.stream_drop = os.environ.get("OPENPETE_STREAM_DROP", "")
        if os.environ.get("OPENPETE_FLEET_STREAM", "") == "0":
            self.stream = False
        self.stream_log = os.path.join(local_run_dir, "stream.log")
        self.skew = {}
        self.done_reason = ""
        self.stop_path = os.path.join(local_run_dir, "stop-request")
        self.stop_noted = set()
        self.expect = []
        self.diffmod = None
        self.archive = None
        self.rows_cache = {}
        self.first_deadline = self.parse_first_deadline(os.environ.get("OPENPETE_FLEET_DEADLINE_S", ""))
        self.canary_only = os.environ.get("OPENPETE_FLEET_CANARY_ONLY", "") == "1"
        self.src_bytes = 0
        self.move_path = os.path.join(local_run_dir, "move-request")
        self.moves_seen = set()
        self.takeovers = []       # one dict per takeover: job, hosts, prediction, rows carried
        self.takeover_noted = set()
        self.size_cache = {}

    @staticmethod
    def parse_poke(spec):
        """Parse a poke knob's <host>:<job>:<tile substring> (OPENPETE_FLEET_POKE, OPENPETE_FLEET_ROW_POKE)."""
        if not spec:
            return None
        parts = spec.split(":", 2)
        if len(parts) != 3:
            raise FleetError(f"poke {spec}: expected <host>:<job>:<tile>", 2)
        return tuple(parts)

    @staticmethod
    def parse_first_deadline(spec):
        """Parse OPENPETE_FLEET_DEADLINE_S=<seconds> (a resume test knob), or None."""
        if not spec:
            return None
        if not spec.isdigit() or int(spec) < 1:
            raise FleetError(f"OPENPETE_FLEET_DEADLINE_S={spec}: expected whole seconds", 2)
        return int(spec)

    # -- hosts --

    def pick_hosts(self, hosts):
        """Return the Host objects this run considers: --hosts, else defaults, minus --no-local."""
        if self.a.hosts:
            sel = select_hosts(hosts, self.a.hosts)
        else:
            sel = [Host(n, c) for n, c in hosts.items()
                   if c.get("enabled", True) and c.get("default", True)]
        if self.a.no_local:
            sel = [h for h in sel if not h.local]
        if not sel:
            raise FleetError("no host selected (every host is disabled, not default, or local "
                             "under --no-local)", EXIT_SKIP)
        return sel

    def need_bytes(self, src_bytes):
        """Return the free space a host needs: sync set, build tree and the largest candidate set."""
        build = os.path.join(self.tree, BUILD_TREE)
        build_bytes = set_bytes(self.tree, build_set(self.tree)) if os.path.isdir(build) \
            else BUILD_TREE_BYTES
        return src_bytes + build_bytes + max(cand_bytes(u, self.ledger) for u in self.units)

    def prepare_hosts(self, src_bytes):
        """Preflight, take the local golden lock, clean each host and ship the image; wait while every host is held.

        When no host is usable and some host is dropped only for a transient
        reason (WAIT_REASONS: another run's golden lock, a busy local host),
        preflight re-runs every HOST_WAIT_POLL_S, a stop request polled each
        STOP_POLL_S, for up to --host-wait S (HOST_WAIT_S); --no-host-wait,
        or no waitable host, exits 64 "no usable host" at once.
        """
        _path, hosts = load_hosts()
        sel = self.pick_hosts(hosts)
        self.sel = sel
        check_recipe(self.tree)
        limit = 0 if getattr(self.a, "no_host_wait", False) else getattr(self.a, "host_wait", HOST_WAIT_S)
        t0, said = time.monotonic(), None
        while True:
            held = self.admit_hosts(sel, src_bytes)
            if self.hosts:
                break
            waited = int(time.monotonic() - t0)
            if not held or limit is None or limit <= 0 or waited >= limit:
                raise FleetError("no usable host" + (f" after waiting {waited} s (--host-wait {limit})"
                                                     if waited else ""), EXIT_SKIP)
            if held != said:
                print(f"[fleet] no usable host; waiting up to {limit - waited} s more (--host-wait "
                      f"{limit}; --no-host-wait exits 64) for: " + "; ".join(held))
                said = held
            end = time.monotonic() + min(HOST_WAIT_POLL_S, limit - waited)
            while time.monotonic() < end:
                stop_point()
                time.sleep(STOP_POLL_S)
            stop_point()
        if said:
            print(f"[fleet] waited {int(time.monotonic() - t0)} s for a usable host")
        builders = [hs for hs in self.hosts if hs.host.cfg.get("build", True)]
        if not builders:
            raise FleetError("no usable host may build (build = true)", EXIT_SKIP)
        self.build_hs = max(builders, key=lambda hs: (hs.speed, hs.host.local))
        print(f"[fleet] hosts: {', '.join(f'{hs.name} (slots {hs.slots}, speed {hs.speed:.2f})' for hs in self.hosts)}; "
              f"build host {self.build_hs.name}")

    def admit_hosts(self, sel, src_bytes):
        """Preflight sel and admit each usable host into self.hosts; return the transient drop reasons (`<host>: <reason>`), empty when none is waitable.

        A local host with no preflight reason is dropped, as --no-local
        would, while local_busy() names a reason, unless --local-anyway.
        """
        results = preflight(sel, self.need_bytes(src_bytes))
        if not getattr(self.a, "local_anyway", False):
            for k, (h, i, reasons, slots) in enumerate(results):
                if h.local and not reasons:
                    busy = local_busy(self.tree, int(i.get("memavail_kib") or 0))
                    if busy:
                        print(f"[fleet] {h.name}: left out as --no-local would "
                              f"(--local-anyway keeps it): {busy[0]}"
                              + (f" (+{len(busy) - 1} more)" if len(busy) > 1 else ""))
                        results[k] = (h, i, busy, slots)
        usable = print_preflight(results)
        infos = {h.name: i for h, i, _r, _s in results}
        held = [f"{h.name}: {r}" for h, _i, reasons, _s in results if waitable(reasons) for r in reasons]
        for h, slots in usable:
            hs = HostState(h, infos[h.name], slots, self.run_id)
            if h.local:
                self.golden_fd = try_golden_lock(h.fleet_root)
                if self.golden_fd is None:
                    print(f"[fleet] {h.name}: dropped: golden lock taken since preflight")
                    held.append(f"{h.name}: golden lock taken since preflight")
                    continue
            rc = clean_host(h, "stale", held=h.local)
            if rc != 0:
                print(f"[fleet] {h.name}: dropped: start-of-run clean exited {rc}")
                continue
            ship_image(h, infos[h.name])
            self.hosts.append(hs)
        return held

    # -- small remote helpers --

    def script(self, hs, script, args, timeout=300, check=True, what="script"):
        """Run a script on a host, a connect failure retried; on failure raise naming the host."""
        return hs.host.run_script(script, args, timeout=timeout, check=check, what=what)

    def mkdirs(self, hs):
        """Create the run directory on a host and mark it touched."""
        self.script(hs, "set -eu; mkdir -p \"$1/ship\" \"$1/tree\"", [hs.run_dir], what="mkdir")
        hs.touched = True

    def push_ship(self, hs):
        """Copy the ship directory (supervisor, fields, binary hashes) into the host's run directory."""
        if hs.host.local:
            return
        files = sorted(os.listdir(self.ship_dir))
        lst = os.path.join(self.local_run_dir, "ship.lst")
        write_list(lst, files)
        rsync_push(hs.host, self.ship_dir, lst, f"{hs.run_dir}/ship")

    @property
    def base_key(self):
        """The trees/<key> name of this checkout's repository base."""
        if getattr(self, "_base_key", None) is None:
            self._base_key = base_key(self.tree)
        return self._base_key

    def refresh_base(self, hs):
        """Refresh the host's repository base trees/<key> from this run's snapshot."""
        src = [f"coordinator={os.uname().nodename}", f"repo={repo_dir(self.tree)}",
               f"path={self.tree}", f"last-run={int(time.time())}"]
        self.script(hs, REFRESH_BASE, [hs.root, self.base_key, self.run_id, *src],
                    timeout=1800, what="base refresh")

    def usable_base(self, hs):
        """Return (the host's base directory or None, a note for the sync line).

        The base is used only when trees/<key> exists and its record names
        this coordinator and this repository; anything else (absent,
        unrecorded, another coordinator's or repository's) is not linked
        from and is replaced by this run's refresh. The base's content is
        never trusted: the sync compares each file's checksum to the
        checkout's before linking it, and the recheck compares the snapshot.
        """
        d = f"{hs.root}/trees/{self.base_key}"
        cp = self.script(hs, 'test -d "$1" || exit 3; cat "$2" 2>/dev/null; exit 0',
                         [d, f"{d}.src"], check=False, what="base check")
        if cp.returncode == 3:
            return None, "no base"
        if cp.returncode != 0:
            return None, f"no base: check exited {cp.returncode}"
        rec = parse_kv(cp.stdout)
        want = {"coordinator": os.uname().nodename, "repo": repo_dir(self.tree)}
        wrong = [f"{k}={rec.get(k, '')!r}" for k, v in want.items() if rec.get(k) != v]
        if wrong:
            return None, "foreign base ignored: " + ", ".join(wrong)
        try:
            age = f"{(time.time() - float(rec.get('last-run'))) / 3600:.1f} h old"
        except (TypeError, ValueError):
            age = "age unknown"
        return d, f"base {self.base_key} from {rec.get('path', '?')}, {age}"

    # -- sync --

    def sync_segment(self, hs, src_root, lst, nfiles, label, base, note):
        """Copy one segment's list to the host's snapshot with --link-dest from its base (None: no base)."""
        lock = f"{hs.root}/trees/{self.base_key}.lock"
        secs, sent = rsync_push(hs.host, src_root, lst, f"{hs.run_dir}/tree", base, lock)
        print(f"[fleet] {hs.name}: synced {label}: {nfiles} files, {sent / 2**20:.0f} MiB "
              f"sent in {secs:.0f} s ({note})")

    def recheck(self, hs, src_root, lst, label):
        """Refuse the run when the host's snapshot differs from src_root over a segment's list."""
        changes = rsync_recheck(hs.host, src_root, lst, f"{hs.run_dir}/tree")
        if changes:
            shown = "\n  ".join(changes[:20])
            raise FleetError(f"torn sync: {hs.name} snapshot differs from {src_root} over the "
                             f"{label} list ({len(changes)} change(s)):\n  {shown}")
        print(f"[fleet] {hs.name}: recheck {label}: 0 changes")

    def after_sync_hook(self):
        """Run OPENPETE_FLEET_AFTER_SYNC (a negative-control knob) in the checkout."""
        cmd = os.environ.get("OPENPETE_FLEET_AFTER_SYNC")
        if cmd:
            print(f"[fleet] OPENPETE_FLEET_AFTER_SYNC: {cmd}")
            subprocess.run(["bash", "-c", cmd], cwd=self.tree, check=False)

    def write_ship(self):
        """Write the supervisor and each run game's capture fields into the ship directory."""
        os.makedirs(self.ship_dir, exist_ok=True)
        with open(os.path.join(self.ship_dir, "supervise.sh"), "w") as f:
            f.write(SUPERVISOR)
        for g in self.run_games():
            out = os.path.join(self.ship_dir, f"fields-{g}.txt")
            subprocess.run([sys.executable, "scripts/presentation-blessed.py", "capture-fields",
                            g, "--out", out], cwd=self.tree, check=True)

    def run_games(self):
        """Return the games any job of this run captures."""
        return sorted({g for g, _t in self.units} | ({self.canary_game()} if self.canary_on() else set()))

    def write_binaries(self, root):
        """Write ship/binaries.sha256 for the binaries under root."""
        lines = [f"{sha256_path(os.path.join(root, r))}  {r}\n" for r in binary_paths(root)]
        if not lines:
            raise FleetError(f"no game binary under {root}/{BUILD_TREE}")
        with open(os.path.join(self.ship_dir, "binaries.sha256"), "w") as f:
            f.writelines(lines)
        print("[fleet] binaries: " + ", ".join(ln.split()[1].rsplit('/', 1)[1] + " "
                                                + ln[:12] for ln in lines))

    def local_build(self):
        """Build the engine in the checkout through the wrapper, logged in the run directory."""
        log = os.path.join(self.local_run_dir, "build.log")
        env = dict(os.environ, OPENPETE_CAPTURE_IMAGE=IMAGE_TAG)
        env.pop("OPENPETE_CAPTURE_TARGETS", None)
        print(f"[fleet] {self.build_hs.name}: in-image build in {self.tree} (log {log})")
        t0 = time.monotonic()
        with open(log, "w") as f:
            rc = run_proc(["nice", "-n", str(self.build_hs.nice),
                           "scripts/golden-capture-container.sh", "build"], cwd=self.tree,
                          env=env, stdout=f, stderr=subprocess.STDOUT).returncode
        if rc != 0:
            raise FleetError(f"in-image build failed (exit {rc}); log {log}")
        print(f"[fleet] build done in {time.monotonic() - t0:.0f} s")

    def build_and_sync(self):
        """Build once on the build host and give every host an identical snapshot (steps 3-4)."""
        for hs in self.hosts:
            self.mkdirs(hs)
        local_build = self.build_hs.host.local
        self.span.start_marker()
        try:
            if local_build:
                # The build loads every local core: wait out a timing window
                # and hold it shut until the build ends.
                sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                "bench"))
                import timing_lock
                timing_lock.gate(label="golden-fleet local build")
                try:
                    self.local_build()
                finally:
                    timing_lock.release_shared()
            self.span.take_build_lock()
            segs = [("source", self.tree, source_set(self.tree, self.pb, self.run_games()))]
            if local_build:
                segs.append(("build", self.tree, build_set(self.tree)))
            self.sync_hosts(segs, self.hosts)
            # Measured while the checkout equals every snapshot (the recheck just passed).
            self.write_ship()
            if local_build:
                self.write_binaries(self.tree)
        finally:
            self.span.release()
        self.refresh_bases(self.hosts)
        if not local_build:
            self.remote_build()
        for hs in self.hosts:
            self.push_ship(hs)
        self.check_discs()

    def sync_hosts(self, segs, hosts):
        """Sync every segment to every host, the hosts concurrently, run the after-sync hook, then recheck every copy."""
        lists = {}
        for label, _root, files in segs:
            lists[label] = os.path.join(self.local_run_dir, f"{label}.lst")
            write_list(lists[label], files)

        def sync(hs):
            base, note = self.usable_base(hs)
            for label, root, files in segs:
                self.sync_segment(hs, root, lists[label], len(files), label, base, note)

        def recheck(hs):
            for label, root, _files in segs:
                self.recheck(hs, root, lists[label], label)

        on_hosts(hosts, sync)
        self.after_sync_hook()
        on_hosts(hosts, recheck)

    def refresh_bases(self, hosts):
        """Refresh each host's link-dest base from its complete snapshot, the hosts concurrently."""
        on_hosts(hosts, self.refresh_base)

    def remote_build(self):
        """Build in the build host's snapshot, then relay its build tree to the other hosts."""
        bh = self.build_hs
        cp = self.script(bh, SEED_BUILD, [bh.root, self.base_key, self.run_id, self.tree],
                         timeout=1800, what="build seed")
        print(f"[fleet] {bh.name}: build tree {cp.stdout.strip()} from its base")
        self.push_ship(bh)
        job = Job("build", pinned=bh.name)
        self.run_jobs([job], deadline=lambda j, hs: BUILD_DEADLINE_S / hs.speed)
        if job.stopped:
            raise StopRequested(STOP_STATUS)
        if not job.result or job.result["rc"] != 0:
            raise FleetError(f"{bh.name}: in-image build failed: "
                             f"{(job.result or {}).get('status', 'no result')}; log "
                             f"{bh.run_dir}/u-build/build.log")
        rc, err = rsync_pull(bh.host, f"{bh.run_dir}/u-build", self.ship_dir,
                             names=["binaries.sha256"])
        if rc != 0:
            raise FleetError(f"{bh.name}: pulling binaries.sha256 failed: {err}")
        print(f"[fleet] {bh.name}: built in {job.result['wall']} s")
        self.refresh_base(bh)
        others = [hs for hs in self.hosts if hs is not bh]
        if others:
            self.relay_build(bh, others)

    def relay_build(self, bh, others):
        """Pull the build tree from the build host, check its binaries, sync it to the others."""
        relay = os.path.join(self.local_run_dir, "relay")
        rc, err = rsync_pull(bh.host, f"{bh.run_dir}/tree/{BUILD_TREE}",
                             os.path.join(relay, BUILD_TREE))
        if rc != 0:
            raise FleetError(f"{bh.name}: pulling the build tree failed: {err}")
        check = subprocess.run(["sha256sum", "-c", "--strict", "--quiet",
                                os.path.join(self.ship_dir, "binaries.sha256")],
                               cwd=relay, capture_output=True, text=True)
        if check.returncode != 0:
            raise FleetError(f"relayed build tree differs from the build host's binary hashes: "
                             f"{check.stdout.strip()[-300:]}")
        self.sync_hosts([("build", relay, build_set(relay))], others)
        self.refresh_bases(others)

    def check_discs(self, hosts=None):
        """Refuse the run unless every host's (or the given hosts') synced disc bytes hash to the blessed header's."""
        for g in self.run_games():
            cue, bin_rel = disc_paths(self.tree, self.pb, g)
            want = self.blessed_disc(g)
            for hs in hosts or self.hosts:
                cp = self.script(hs, HASH_SCRIPT, [f"{hs.run_dir}/tree", cue, bin_rel], what="disc hash")
                got = {ln.split(None, 1)[1]: ln.split(None, 1)[0]
                       for ln in cp.stdout.splitlines() if ln.strip()}
                for key, rel in (("disc-cue", cue), ("disc-bin", bin_rel)):
                    if want[key] and got.get(rel) != want[key]:
                        raise FleetError(f"{hs.name}: synced {rel} hashes to {got.get(rel)}, "
                                         f"blessed header {key} is {want[key]}")
            print(f"[fleet] {g}: disc cue and bin hash to the blessed header on every host")

    def blessed_disc(self, game):
        """Return the disc-cue and disc-bin sha256 the game's unit headers record (consistent across tiers)."""
        want = {"disc-cue": "", "disc-bin": ""}
        for tier in {t for g, t in self.units if g == game} | {"presentation"}:
            if game not in TIER_GAMES.get(tier, ()):
                continue
            header = self.pb.load_manifest(game, tier).header
            for key in want:
                sha = (header.get(key) or "").split(" ")[0]
                if want[key] and sha and sha != want[key]:
                    raise FleetError(f"{game}: blessed headers disagree on {key}")
                want[key] = want[key] or sha
        return want

    # -- canary --

    def canary_on(self):
        """True when the run spans two or more capture hosts, or under OPENPETE_FLEET_CANARY_ONLY."""
        return len(self.hosts) >= 2 or self.canary_only

    def canary_game(self):
        """Return the canary's game: the first unit's game."""
        return self.units[0][0]

    # -- placement --

    def make_jobs(self):
        """Return the run's jobs: one canary per host when on, then units longest first."""
        jobs = []
        if self.canary_on():
            jobs += [Job("canary", self.canary_game(), "presentation", pinned=hs.name)
                     for hs in self.hosts]
        if self.canary_only:
            return jobs
        units = sorted(self.units, key=lambda u: -expected_s(u, self.ledger))
        jobs += [Job("unit", g, t) for g, t in units]
        return jobs

    def filtered(self):
        """True when run --filter or --freecam-filter scopes the units to part of their corpus."""
        return bool(getattr(self.a, "filter", None) or getattr(self.a, "freecam_filter", None))

    def repeat(self):
        """Return run --repeat's copy count, 0 without it."""
        return int(getattr(self.a, "repeat", None) or 0)

    def deadline(self, job, hs):
        """Return a job's deadline in seconds on a host (first-run rule from the size table)."""
        if job.kind == "canary":
            return CANARY_DEADLINE_S / hs.speed * 2
        return expected_s(job.unit, self.ledger) / hs.speed * 2

    def leg_timeout_s(self, _job, hs):
        """Return a job's PRESENTATION_LEG_TIMEOUT on a host."""
        return leg_timeout(hs.speed)

    def job_slots(self, job, hs):
        """Return a job's CORPUS_JOBS: its own slots (a calibration leg), else the host's."""
        return job.slots or hs.slots

    def job_env(self, job, hs, att):
        """Return the KEY=VALUE list a supervisor sources."""
        game, tier = job.game or "", job.tier
        slots = self.job_slots(job, hs)
        canary = CANARY.get(game, ("", "")) if job.kind == "canary" else None
        filters = f"--filter={canary[0]} --freecam-filter={canary[1]}" if canary else ""
        if job.kind == "unit" and self.filtered():
            filters = f"--filter={self.a.filter or ''} --freecam-filter={self.a.freecam_filter or ''}"
            if self.repeat():
                filters += f" --repeat={self.repeat()}"
        poke = self.poke_for(self.poke, job, hs)
        env = {"RUN_ID": self.run_id, "JOB": job.name, "KIND": job.kind, "GAME": game,
               "TIER": tier, "RUN_DIR": hs.run_dir, "UNIT_DIR": att.unit_dir,
               "ATTEMPT": att.n, "ATTEMPT_BASE": att.base, "FLEET_ROOT": hs.root,
               "LOCK_MODE": "held" if hs.host.local else "take", "IMAGE": IMAGE_TAG,
               "MOUNT_AS": self.tree, "ENGINE": hs.engine, "NICE": hs.nice,
               "SLOTS": slots, "LEG_TIMEOUT": self.leg_timeout_s(job, hs), "FILTERS": filters,
               "POKE": poke, "RESUME": int(self.resume),
               "STREAM": int(self.stream and job.kind == "unit"),
               "STREAM_POKE": self.poke_for(self.row_poke, job, hs), "BINARIES": " ".join(binary_rels()),
               "PIN_LP_NUM_THREADS": lp_threads(hs.info, slots)}
        return [f"{k}={v}" for k, v in env.items()]

    @staticmethod
    def poke_for(poke, job, hs):
        """Return a parsed poke's tile substring when it names this host and job, else ''."""
        if poke and poke[0] == hs.name and poke[1] in (job.name, job.kind):
            return poke[2]
        return ""

    def launch(self, job, hs, deadline_s):
        """Start one attempt of a job on a host through its detached supervisor."""
        if self.first_deadline and job.attempts == 0 and job.kind in ("unit", "canary"):
            deadline_s = self.first_deadline
        att = Attempt(job, hs, deadline_s)
        args = [f"{hs.run_dir}/ship/supervise.sh", f"{att.base}.env", f"{att.base}.log",
                *self.job_env(job, hs, att)]
        cp = self.script(hs, LAUNCH_SCRIPT, args, timeout=60, check=False, what="launch")
        if cp.returncode != 0:
            job.attempts -= 1
            self.drop(hs, f"launch failed: {cp.stderr.strip()[-200:]}")
            return None
        hs.current = att
        slots = self.job_slots(job, hs)
        print(f"[fleet] {hs.name}: {job.name} attempt {att.n} started "
              f"(slots {slots}, LP_NUM_THREADS {lp_threads(hs.info, slots)}, leg timeout "
              f"{self.leg_timeout_s(job, hs)} s, deadline {deadline_s / 60:.0f} min)")
        return att

    def drop(self, hs, why):
        """Drop a host for the rest of the run."""
        hs.dropped = why
        print(f"[fleet] {hs.name}: dropped for the rest of the run: {why}")

    def local_paused(self, hs):
        """True when `hs` is this machine and another job holds the timing window.

        The local host takes no new unit while a timing leg runs here
        (scripts/bench/timing_lock.py): a unit's captures load every core the
        leg is measured on. Its running unit finishes; the timing job's
        preflight waits for it. The other hosts keep taking units, and the
        local host resumes when the window closes."""
        if not hs.host.local:
            return False
        bench = os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench")
        if bench not in sys.path:
            sys.path.insert(0, bench)
        import timing_lock
        st = timing_lock.state()
        paused = st["held"] and not st["mine"]
        if paused != getattr(hs, "timing_paused", False):
            print(f"[fleet] {hs.name}: " + (f"takes no new unit: timing window "
                                            f"{timing_lock.describe(st)}" if paused
                                            else "timing window closed; taking units"))
            hs.timing_paused = paused
        return paused

    def next_job(self, pending, hs):
        """Return the first pending job this host may take, or None."""
        if self.local_paused(hs):
            return None
        for job in pending:
            if job.pinned in (None, hs.name):
                return job
        return None

    def run_jobs(self, jobs, deadline=None, on_done=None):
        """Place jobs on free hosts until every job has a result; on_done may return follow-up jobs."""
        pending = list(jobs)
        deadline = deadline or self.deadline
        # apply_stop owns stops here; a poll or kill must not raise StopRequested.
        with stop_watch(None):
            while True:
                self.apply_stop(pending)
                self.apply_moves(pending, on_done)
                self.take_over(pending, on_done)
                self.place(pending, deadline)
                running = [hs for hs in self.hosts if hs.current]
                if not running:
                    if pending and any(self.local_paused(hs) for hs in self.hosts
                                       if not hs.dropped):
                        time.sleep(POLL_S)      # the local host waits out a timing window
                        continue
                    break
                time.sleep(POLL_S)
                for hs in running:
                    self.poll(hs, pending, on_done)
        for job in pending:
            job.result = {"rc": EXIT_SKIP, "status": "no usable host left", "host": "-"}
            print(f"[fleet] {job.name}: SKIP: no usable host left")

    def place(self, pending, deadline):
        """Give each free, undropped host the next job it may take."""
        for hs in sorted(self.hosts, key=lambda h: -h.speed):
            if hs.current or hs.dropped:
                continue
            job = self.next_job(pending, hs)
            if job and self.launch(job, hs, deadline(job, hs)):
                pending.remove(job)
        for job in [j for j in pending if j.pinned]:
            hs = next((h for h in self.hosts if h.name == job.pinned), None)
            if (hs is None or hs.dropped) and job.kind == "unit":
                print(f"[fleet] {job.name}: pinned host {job.pinned} "
                      f"{'dropped' if hs else 'not in the run'}; unpinned")
                job.pinned = None
            elif hs is None or hs.dropped:
                pending.remove(job)
                job.result = {"rc": EXIT_SKIP, "status": f"host {job.pinned} dropped",
                              "host": job.pinned}

    def poll_attempt(self, att):
        """Run POLL_SCRIPT for an attempt; take its new stream lines; return its state words ([] when unreachable)."""
        try:
            cp = att.hs.host.run_script(POLL_SCRIPT, [att.base, att.stream_off], timeout=60,
                                        binary=True, retry=False)
            frames = parse_poll(cp.stdout) if cp.returncode == 0 else {}
        except (FleetError, ValueError) as e:
            print(f"[fleet] {att.hs.name}: poll of {att.job.name} failed: {e}")
            frames = {}
        state, data = frames.get(att.base, ["", b""])
        if state:
            self.take_stream(att, data)
        return state.split(None, 1)

    def poll(self, hs, pending, on_done):
        """Poll a host's attempt and act on done, dead, overdue or unreachable."""
        att = hs.current
        state = self.poll_attempt(att)
        now = time.monotonic()
        if not state:
            att.lost_since = att.lost_since or now
            if now - att.lost_since > GRACE_S:
                self.drop(hs, f"unreachable for {GRACE_S // 60} min during {att.job.name}; "
                              f"its runs/ directory stays for clean")
                self.requeue(att, pending, killed=False)
            return
        att.lost_since = None
        if state[0] == "done":
            self.finish_attempt(att, parse_kv(state[1].replace(" ", "\n")), pending, on_done)
        elif state[0] == "dead" or (state[0] == "starting" and now - att.t0 > START_GRACE_S):
            print(f"[fleet] {hs.name}: {att.job.name} attempt {att.n}: supervisor {state[0]} "
                  f"without a done marker")
            self.stop_and_requeue(att, pending)
        elif now > att.deadline:
            print(f"[fleet] {hs.name}: {att.job.name} attempt {att.n} past its deadline "
                  f"({(att.deadline - att.t0) / 60:.0f} min)")
            self.stop_and_requeue(att, pending)

    def kill_attempt(self, att):
        """Read the attempt's stream once more, then kill its supervisor and containers; drop the host when the lock stays held."""
        hs = att.hs
        self.poll_attempt(att)
        try:
            cp = hs.host.run_script(KILL_SCRIPT, [hs.engine, self.run_id, hs.root,
                                                  int(not hs.host.local), att.base], timeout=120)
            out = cp.stdout
        except FleetError as e:
            out = str(e)
        for line in out.splitlines():
            if not line.startswith("lock="):
                print(f"[fleet] {hs.name}: {line}")
        att.t_end = att.t_end or time.monotonic()
        if "lock=free" not in out:
            self.drop(hs, "could not confirm the golden lock released after a kill")

    def stop_and_requeue(self, att, pending):
        """Kill an attempt's supervisor and containers, confirm the lock is free, re-queue."""
        self.kill_attempt(att)
        self.requeue(att, pending, killed=True)

    # -- move (golden-fleet.py move) and the rows a unit carries --

    def host_state(self, name):
        """Return the run's HostState named name, or None."""
        return next((h for h in self.hosts if h.name == name), None)

    def add_host(self, name):
        """Admit a host on the run-start terms: preflight, golden lock, clean, image; return its HostState or raise naming the refusal.

        A host already in the run is preflighted again (its own lock excepted
        on the local host, which the coordinator holds) and returned as is.
        """
        _path, hosts = load_hosts()
        if name not in hosts:
            raise FleetError(f"no host {name!r} in the host file (have: {', '.join(hosts)})", 2)
        h = Host(name, hosts[name])
        ((_h, info, reasons, slots),) = preflight([h], self.need_bytes(self.src_bytes))
        have = self.host_state(name)
        if have and h.local:
            reasons = [r for r in reasons if not r.startswith("golden lock held")]
        if reasons:
            raise FleetError(f"{name}: " + "; ".join(reasons))
        if have:
            return have
        if h.local:
            self.golden_fd = try_golden_lock(h.fleet_root)
            if self.golden_fd is None:
                raise FleetError(f"{name}: golden lock taken since preflight")
        rc = clean_host(h, "stale", held=h.local)
        if rc != 0:
            if h.local:
                os.close(self.golden_fd)
                self.golden_fd = None
            raise FleetError(f"{name}: clean exited {rc}")
        ship_image(h, info)
        print(f"[fleet] {name}: usable, slots {slots}")
        return HostState(h, info, slots, self.run_id)

    def join_snapshot(self, hs):
        """Give a joining host this run's snapshot: a copy of an in-run host's tree over the run's sync lists, rechecked, then ship, base and disc check.

        The local host's snapshot is the source when the local host is in
        the run; otherwise another host's snapshot is pulled into
        <local run dir>/join-<host> (unchanged files copied from the checkout
        or the build relay) and checked against that host before it is pushed.
        """
        self.mkdirs(hs)
        live = [h for h in self.hosts if h is not hs and h.touched]
        ref = next((h for h in live if h.host.local), None) or next(iter(live), None)
        if ref is None:
            raise FleetError("no host of the run holds a snapshot to copy")
        lists = [(label, os.path.join(self.local_run_dir, f"{label}.lst"))
                 for label in ("source", "build", "snapshot")]
        lists = [(label, p) for label, p in lists if os.path.exists(p)]
        if ref.host.local:
            root = f"{ref.run_dir}/tree"
        else:
            root = os.path.join(self.local_run_dir, f"join-{hs.name}")
            lists = self.stage_snapshot(ref, root, lists)
        print(f"[fleet] {hs.name}: joins the run with {ref.name}'s snapshot")
        self.sync_hosts([(label, root, read_list(p)) for label, p in lists], [hs])
        self.check_binaries(hs)
        self.refresh_base(hs)
        self.push_ship(hs)
        self.check_discs([hs])
        if not ref.host.local:
            shutil.rmtree(root, ignore_errors=True)

    def stage_snapshot(self, ref, root, lists):
        """Copy ref's snapshot over the lists into the local root, then refuse unless it equals ref's (rsync -c); return the lists used."""
        os.makedirs(root, exist_ok=True)
        relay = os.path.join(self.local_run_dir, "relay")
        src = remote_path(ref.host, f"{ref.run_dir}/tree") + "/"
        for label, lst in lists:
            alt = relay if label == "build" and os.path.isdir(relay) else self.tree
            argv = ["rsync", "-a", "-c", "--from0", f"--files-from={lst}", *rsync_base(ref.host),
                    f"--copy-dest={alt}", src, root + "/"]
            cp = transport_run(ref.host, f"pull of the {label} snapshot", argv, text=True)
            if cp.returncode != 0:
                raise FleetError(f"{ref.name}: pulling its {label} snapshot exited {cp.returncode}: "
                                 f"{cp.stderr.strip()[-300:]}")
        if not any(label == "build" for label, _p in lists):
            # One remote build host and no relay: its build tree has no list here.
            dst = os.path.join(root, BUILD_TREE)
            os.makedirs(dst, exist_ok=True)
            argv = ["rsync", "-a", "-c", *rsync_base(ref.host), "--exclude=/home",
                    "--exclude=/golden-disc-hash.tsv",
                    f"--copy-dest={os.path.join(self.tree, BUILD_TREE)}",
                    remote_path(ref.host, f"{ref.run_dir}/tree/{BUILD_TREE}") + "/", dst + "/"]
            cp = transport_run(ref.host, "pull of the build tree", argv, text=True)
            if cp.returncode != 0:
                raise FleetError(f"{ref.name}: pulling its build tree exited {cp.returncode}: "
                                 f"{cp.stderr.strip()[-300:]}")
            lst = os.path.join(self.local_run_dir, f"join-{ref.name}-build.lst")
            write_list(lst, build_set(root))
            lists = lists + [("build", lst)]
        for label, lst in lists:
            argv = ["rsync", "-n", "-i", "-c", "--from0", f"--files-from={lst}",
                    *rsync_base(ref.host), src, root + "/"]
            cp = transport_run(ref.host, f"recheck of the staged {label} snapshot", argv, text=True)
            changes = [ln for ln in cp.stdout.splitlines() if ln[:1] in "<>ch*" and ln[1:2] in "fLS"]
            if cp.returncode != 0 or changes:
                raise FleetError(f"{ref.name}: staged {label} snapshot differs from it "
                                 f"({len(changes)} change(s), rsync {cp.returncode}): "
                                 + "; ".join(changes[:5]))
        print(f"[fleet] {ref.name}: snapshot staged in {root} and rechecked")
        return lists

    def check_binaries(self, hs):
        """Refuse unless the host's snapshot binaries hash to ship/binaries.sha256."""
        want = {}
        with open(os.path.join(self.ship_dir, "binaries.sha256"), encoding="utf-8") as f:
            for ln in f:
                if ln.strip():
                    sha, rel = ln.split(None, 1)
                    want[rel.strip()] = sha
        cp = self.script(hs, HASH_SCRIPT, [f"{hs.run_dir}/tree", *sorted(want)])
        got = {ln.split(None, 1)[1]: ln.split(None, 1)[0] for ln in cp.stdout.splitlines()
               if ln.strip()}
        bad = [rel for rel, sha in want.items() if got.get(rel) != sha]
        if bad:
            raise FleetError(f"{hs.name}: snapshot binaries differ from the run's: {', '.join(bad)}")
        print(f"[fleet] {hs.name}: snapshot binaries hash to the run's ({len(want)} file(s))")

    def fetch_rows(self, host, cand, dst):
        """Copy the stamps of a candidate directory on a host and the tiles they name into the local dst; return (rows, -vk.png tiles).

        Files of a local source are hard-linked. A tile that is absent leaves
        its row for the importer to refuse.
        """
        shutil.rmtree(dst, ignore_errors=True)
        os.makedirs(dst)
        link = [f"--link-dest={cand}"] if host.local else []
        src = remote_path(host, cand.rstrip("/")) + "/"
        argv = ["rsync", "-a", *rsync_base(host), *link, "--include=/resume/",
                "--include=/resume/*.stamp", "--exclude=*", src, dst + "/"]
        cp = transport_run(host, f"pull of the stamps of {cand}", argv, text=True)
        if cp.returncode != 0:
            raise FleetError(f"{host.name}: pulling the stamps of {cand} exited {cp.returncode}: "
                             f"{cp.stderr.strip()[-300:]}")
        files = stamp_files(dst)
        tiles = [f for f in files if not f.startswith("resume/")]
        if tiles:
            lst = os.path.join(os.path.dirname(dst.rstrip("/")), ".rows.lst")
            write_list(lst, tiles)
            cp = transport_run(host, f"pull of the stamped tiles of {cand}",
                               ["rsync", "-a", "--from0", f"--files-from={lst}", *rsync_base(host),
                                *link, src, dst + "/"], text=True)
            os.unlink(lst)
            if cp.returncode not in (0, 23):
                raise FleetError(f"{host.name}: pulling the stamped tiles of {cand} exited "
                                 f"{cp.returncode}: {cp.stderr.strip()[-300:]}")
        return row_counts(files)

    def push_rows(self, hs, job, cand):
        """Copy a local candidate directory's stamps and stamped tiles into the job's candidate directory on a host; return (rows, -vk.png tiles).

        The host's earlier output of the unit stays; an interrupted import's
        set-aside directory (cand.resume-prev), whose presence makes the
        importer discard the fresh directory, is removed first.
        """
        files = [f for f in stamp_files(cand) if os.path.exists(os.path.join(cand, f))]
        unit_dir = f"{hs.run_dir}/u-{job.name}"
        self.script(hs, 'set -eu; rm -rf "${1:?}/cand.resume-prev"; mkdir -p "$1/cand"', [unit_dir])
        hs.touched = True
        if files:
            lst = os.path.join(self.local_run_dir, f"rows-{job.name}.lst")
            write_list(lst, files)
            rsync_push(hs.host, cand, lst, f"{unit_dir}/cand")
            os.unlink(lst)
        return row_counts(files)

    def carry_rows(self, att, tgt):
        """Bring an attempt's stamped rows from its host into the target host's candidate directory; return (rows, -vk.png tiles)."""
        job, src = att.job, att.hs
        cand = f"{att.unit_dir}/cand"
        stage = None
        if not src.host.local:
            stage = os.path.join(self.local_run_dir, f"carry-{job.name}", "cand")
            try:
                self.fetch_rows(src.host, cand, stage)
            except FleetError as e:
                print(f"[fleet] {job.name}: no rows carried from {src.name}: {e}")
                shutil.rmtree(os.path.dirname(stage), ignore_errors=True)
                return 0, 0
            cand = stage
        try:
            return self.push_rows(tgt, job, cand)
        finally:
            if stage:
                shutil.rmtree(os.path.dirname(stage), ignore_errors=True)

    def apply_moves(self, pending, on_done):
        """Act on each new line of the run's move request once its legs have started; record the outcome in moves.log."""
        if not self.jobs or not os.path.exists(self.move_path):
            return
        for req in read_move_requests(self.move_path):
            if req["id"] in self.moves_seen:
                continue
            self.moves_seen.add(req["id"])
            try:
                note = self.move(req["unit"], req["to"], pending, on_done)
                self.log_move(req["id"], "accepted", note)
            except FleetError as e:
                self.log_move(req["id"], "refused", str(e))

    def log_move(self, rid, verdict, text):
        """Append one move outcome to moves.log (read by the move command) and print it."""
        print(f"[fleet] move {rid}: {verdict}: {text}")
        with open(os.path.join(self.local_run_dir, "moves.log"), "a", encoding="utf-8") as f:
            f.write(f"id={rid} {verdict} {text}\n")

    def move(self, name, to, pending, on_done):
        """Move a running or pending unit to host `to`: admit the host, stop the attempt, carry its stamped rows, re-queue the unit pinned there.

        Every refusal before the attempt is stopped leaves the unit as it
        was. A host new to the run joins it (run-start preflight, lock,
        clean, image, snapshot) and gets a canary, as does every run host
        without one.
        """
        job = next((j for j in self.jobs if j.name == name), None)
        if job is None or job.kind != "unit":
            units = ", ".join(j.name for j in self.jobs if j.kind == "unit")
            raise FleetError(f"no unit {name} in this run (units: {units})")
        if job.stopped:
            raise FleetError(f"{name} was stopped by user; `resume` it once the run is done")
        if job.result is not None:
            raise FleetError(f"{name} has finished (rc {job.result['rc']}, "
                             f"{job.result.get('status', '')} on {job.result.get('host', '-')})")
        running = next((h for h in self.hosts if h.current and h.current.job is job), None)
        if running and running.name == to:
            raise FleetError(f"{name} already runs on {to}")
        if not running and job not in pending:
            raise FleetError(f"{name} is neither running nor pending")
        tgt = self.host_state(to)
        if tgt and tgt.dropped:
            raise FleetError(f"{to} was dropped from this run: {tgt.dropped}")
        if tgt and tgt.current:
            raise FleetError(f"{to} is busy with {tgt.current.job.name}")
        joined = tgt is None
        tgt = self.add_host(to)
        if joined:
            self.hosts.append(tgt)
            self.sel.append(tgt.host)
            try:
                self.join_snapshot(tgt)
            except FleetError as e:
                self.drop(tgt, f"join failed: {e}")
                raise
        att = running.current if running else (job.atts[max(job.atts)] if job.atts else None)
        if running:
            state = self.poll_attempt(att)
            if state and state[0] == "done":
                self.finish_attempt(att, parse_kv(state[1].replace(" ", "\n")), pending, on_done)
                raise FleetError(f"{name} finished before the move; its result stands")
            self.kill_attempt(att)
            # The supervisor may have written its done marker while the kill
            # was in flight: the attempt finished, and its result stands.
            state = self.poll_attempt(att)
            if state and state[0] == "done":
                self.finish_attempt(att, parse_kv(state[1].replace(" ", "\n")), pending, on_done)
                raise FleetError(f"{name} finished while its stop was in flight; its result stands")
            running.current = None
            job.moves += 1
        rows, png = self.carry_rows(att, tgt) if att else (0, 0)
        if att:
            job.carried.add(att.n)
        job.last_carry = (rows, png)
        src = att.hs.name if att else "-"
        job.moved.append((src, to))
        job.pinned = to
        if job in pending:
            pending.remove(job)
        pending.insert(0, job)
        if joined:
            have = {j.pinned for j in self.jobs if j.kind == "canary"}
            for hs in [h for h in self.hosts if not h.dropped and h.name not in have][::-1]:
                can = Job("canary", self.canary_game(), "presentation", pinned=hs.name)
                self.jobs.append(can)
                pending.insert(0, can)
                print(f"[fleet] {hs.name}: canary queued (the run now spans "
                      f"{len(self.hosts)} hosts)")
            self.measure_skew([tgt])
        return (f"{name} {'attempt ' + str(att.n) + ' on ' if att else ''}{src} -> {to}: "
                f"carried {rows} row(s), {png} tile(s); re-queued pinned to {to}")

    # -- takeover: a free host takes over the unit predicted to finish last --

    def takeover_enabled(self):
        """True when units stream rows (progress), import carried rows and capture whole corpora, and run --no-takeover is unset."""
        return (self.resume and self.stream and not self.resumed and not self.canary_only
                and not self.filtered() and not getattr(self.a, "no_takeover", False))

    def unit_size(self, job):
        """Return a unit's (blessed tiles, corpus rows), cached."""
        if job.unit not in self.size_cache:
            self.size_cache[job.unit] = (len(self.pb.load_manifest(job.game, job.tier).rows),
                                         len(corpus_row_names(self.tree, job.game, job.tier)))
        return self.size_cache[job.unit]

    @staticmethod
    def streamed_tiles(att):
        """Return the number of blessed tiles an attempt has streamed (SAME or DIVERGE, imported included)."""
        return len({r["tile"] for r in att.lines if r["verb"] in ("SAME", "DIVERGE")})

    def predict(self, att, dst):
        """Return takeover_prediction for a running unit attempt moving to host dst."""
        job, src = att.job, att.hs
        total, rows = self.unit_size(job)
        hops = int(not src.host.local) + int(not dst.host.local)
        return takeover_prediction(self.streamed_tiles(att), total, rows,
                                   expected_s(job.unit, self.ledger), cand_bytes(job.unit, self.ledger),
                                   src.speed, src.slots, dst.speed, hops)

    def take_over(self, pending, on_done):
        """Give each free host with no queued job the running unit predicted to finish last, when takeover_refusal passes; act through move().

        A refusal is logged once per free host and attempt. A takeover whose
        move is refused (the unit finished first, the host failed its
        preflight) leaves the unit where it was.
        """
        if not self.takeover_enabled():
            return
        for hs in sorted(self.hosts, key=lambda h: -h.speed):
            if (hs.current or hs.dropped or not hs.touched or self.local_paused(hs)
                    or self.next_job(pending, hs)):
                continue
            running = [h.current for h in self.hosts
                       if h is not hs and h.current and h.current.job.kind == "unit"
                       and h.current.job.result is None and not h.current.job.stopped]
            if not running:
                continue
            preds = {id(a): self.predict(a, hs) for a in running}
            att = max(running, key=lambda a: preds[id(a)]["src_s"])
            pred = preds[id(att)]
            src, job = att.hs, att.job
            why = takeover_refusal(pred, src.speed, hs.speed)
            text = (f"{job.name} on {src.name} -> {hs.name}: {1 - pred['left']:.0%} streamed; predicted "
                    f"remaining {pred['src_s']:.0f} s on {src.name}, {pred['dst_s']:.0f} s on "
                    f"{hs.name} (cost {pred['cost_s']:.0f} s: fixed {TAKEOVER_FIXED_S} + carry "
                    f"{pred['carry_s']:.0f}), saving {pred['saving_s']:.0f} s")
            if why:
                key = (hs.name, job.name, att.n)
                if key not in self.takeover_noted:
                    self.takeover_noted.add(key)
                    print(f"[fleet] takeover not taken: {text}; {why}")
                continue
            print(f"[fleet] takeover: {text} (threshold {TAKEOVER_MIN_SAVING_S} s)")
            t0 = time.monotonic()
            try:
                note = self.move(job.name, hs.name, pending, on_done)
            except FleetError as e:
                print(f"[fleet] takeover of {job.name} refused: {e}")
                continue
            rows, png = job.last_carry
            print(f"[fleet] takeover: {note}")
            self.takeovers.append({"job": job, "src": src.name, "dst": hs.name, "t": t0,
                                   "pred": pred, "rows": rows, "tiles": png})

    def takeover_summary(self):
        """Print each takeover's prediction beside the measured remaining time on the receiving host.

        The receiving host's time runs from the takeover's start (the kill)
        to the done marker of the attempt it launched; the saving against
        the prediction is the source's predicted remaining time minus it,
        since the source's own finish was not observed.
        """
        for tk in self.takeovers:
            job, pred = tk["job"], tk["pred"]
            r = job.result or {}
            att = job.atts.get(r.get("attempt"))
            head = (f"[fleet] takeover {job.name} {tk['src']} -> {tk['dst']} ({tk['rows']} row(s), "
                    f"{tk['tiles']} tile(s) carried): predicted {pred['src_s']:.0f} s on "
                    f"{tk['src']}, {pred['dst_s']:.0f} s on {tk['dst']}, saving {pred['saving_s']:.0f} s")
            if att is None or att.hs.name != tk["dst"] or att.t_end is None or job.stopped:
                print(f"{head}; not measured ({r.get('status') or 'no result'} on {r.get('host', '-')})")
                continue
            got = att.t_end - tk["t"]
            print(f"{head}; measured {got:.0f} s on {tk['dst']}, saving {pred['src_s'] - got:.0f} s "
                  f"against the predicted {pred['src_s']:.0f} s on {tk['src']}")

    def keep_unfinished(self):
        """Keep each unfinished unit's stamped rows, the ship directory and the local snapshot under <local run dir>/kept, which `resume` reads.

        A unit is unfinished when it was stopped or its last attempt ended
        without a done marker on its final try (UNFINISHED_STATUS: attempts
        exhausted, host lost); the rows of its last attempt are kept.
        """
        unfinished = [j for j in self.jobs if j.kind == "unit" and j.atts and (
            j.stopped or (j.result or {}).get("status") in UNFINISHED_STATUS)]
        if not unfinished:
            return
        kept = os.path.join(self.local_run_dir, "kept")
        for job in unfinished:
            att = job.atts[max(job.atts)]
            try:
                rows, png = self.fetch_rows(att.hs.host, f"{att.unit_dir}/cand",
                                            os.path.join(kept, f"u-{job.name}", "cand"))
            except FleetError as e:
                print(f"[fleet] {job.name}: rows not kept: {e}")
                continue
            with open(os.path.join(kept, f"u-{job.name}", "host"), "w", encoding="utf-8") as f:
                f.write(att.hs.name + "\n")
            print(f"[fleet] {job.name}: kept {rows} stamped row(s), {png} tile(s) of attempt "
                  f"{att.n} on {att.hs.name} in {kept}")
        if os.path.isdir(self.ship_dir):
            shutil.copytree(self.ship_dir, os.path.join(kept, "ship"), dirs_exist_ok=True)
        local = next((h for h in self.hosts if h.host.local and h.touched), None)
        tree = f"{local.run_dir}/tree" if local else ""
        if tree and os.path.isdir(tree) and not os.path.exists(os.path.join(kept, "tree")):
            subprocess.run(["cp", "-al", tree, os.path.join(kept, "tree")], check=True)
            print(f"[fleet] local: snapshot kept (hard links) in {kept}/tree")

    # -- stop (golden-fleet.py stop) --

    def apply_stop(self, pending):
        """Stop the running and pending jobs the run's stop request names."""
        want = read_stop_request(self.stop_path)
        if not want:
            return
        names = {j.name for j in self.jobs} | {j.name for j in pending}
        for name in sorted(want - names - {"all"} - self.stop_noted) if self.jobs else ():
            print(f"[fleet] stop request: no job {name} in this run")
            self.stop_noted.add(name)
        hit = (lambda j: "all" in want or j.name in want)
        for hs in self.hosts:
            if hs.current and hit(hs.current.job):
                att = hs.current
                self.mark_stopped(att.job, hs.name, att.n)
                self.kill_attempt(att)
                hs.current = None
        for job in [j for j in pending if hit(j)]:
            pending.remove(job)
            self.mark_stopped(job, "-", None)

    def stop_covers_run(self):
        """True when the stop request names `all` or every unit of the run (the predicate run_proc polls before the legs)."""
        want = read_stop_request(self.stop_path)
        return "all" in want or {f"{g}-{t}" for g, t in self.units} <= want

    def stop_before_legs(self):
        """Record a stop acted on before the legs: every unit stopped before it started, the run's verdict printed; return rc 1."""
        print("[fleet] stop request acted on before the legs; child processes ended")
        self.jobs = [Job("unit", g, t) for g, t in self.units]
        for job in self.jobs:
            self.mark_stopped(job, "-", None)
        rc = self.run_rc()
        self.print_summary(rc)
        return rc

    def reap_remote(self):
        """Run REAP_SCRIPT on every remote host the run selected: no process tagged with the run id outlives it."""
        for h in self.sel:
            if h.local:
                continue
            try:
                cp = h.run_script(REAP_SCRIPT, [RUN_TAG_VAR, self.run_id], timeout=60, tag=False)
                out = cp.stdout.strip() if cp.returncode == 0 else f"exited {cp.returncode}"
            except FleetError as e:
                out = str(e)
            print(f"[fleet] {h.name}: {out}")

    def mark_stopped(self, job, host, attempt):
        """Record a job's result as stopped by the user (no ledger row, no re-queue) and mark its stream directory."""
        job.result = {"rc": 1, "status": STOP_STATUS, "host": host, "attempt": attempt}
        job.stopped = True
        print(f"[fleet] {job.name}: {STOP_STATUS}" + (f" (attempt {attempt} on {host})" if attempt else
                                                       " before it started"))
        if job.kind == "unit":
            d = self.stream_dir(job)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "STOPPED"), "w") as f:
                f.write(f"{STOP_STATUS} {datetime.datetime.now().isoformat(timespec='seconds')}\n")

    # -- the stream --

    def stream_dir(self, job):
        """Return the local directory a unit's streamed divergences are pulled into."""
        return os.path.join(self.tree, STREAM_DIR, self.run_id, job.name)

    def log_stream(self, text):
        """Append one line to the run's stream.log."""
        with open(self.stream_log, "a", encoding="utf-8") as f:
            f.write(text + "\n")

    def measure_skew(self, hosts=None):
        """Measure each host's (or the given hosts') clock offset from this machine (host minus local) and log it."""
        for hs in hosts or self.hosts:
            if hs.host.local:
                self.skew[hs.name] = 0.0
                continue
            t0 = time.time()
            try:
                cp = hs.host.run_script("date +%s.%N", timeout=30, retry=False)
                remote = float(cp.stdout.strip())
            except (FleetError, ValueError):
                print(f"[fleet] {hs.name}: clock skew not measured")
                continue
            t1 = time.time()
            self.skew[hs.name] = remote - (t0 + t1) / 2
            self.log_stream(f"# skew {hs.name} {self.skew[hs.name]:+.3f} bound {(t1 - t0) / 2:.3f}")
            print(f"[fleet] {hs.name}: clock {self.skew[hs.name]:+.3f} s from this machine "
                  f"(+-{(t1 - t0) / 2:.3f} s)")

    def take_stream(self, att, data):
        """Take an attempt's new stream bytes: complete lines are recorded, logged and queued for pulls."""
        att.stream_off += len(data)
        buf = att.stream_tail + data
        cut = buf.rfind(b"\n") + 1
        att.stream_tail = buf[cut:]
        for raw in buf[:cut].decode(errors="replace").splitlines():
            self.receive_line(att, raw)
        if att.backlog:
            self.pull_stream(att)

    def receive_line(self, att, raw):
        """Record one stream line of an attempt: stream.log, moved-tile tag, pull backlog."""
        rec = parse_stream_line(raw)
        if rec is None:
            print(f"[fleet] {att.job.name}: malformed stream line: {raw[:120]}")
            return
        if self.stream_drop and rec["tile"] == self.stream_drop:
            self.stream_drop = ""
            print(f"[fleet] {att.job.name}: OPENPETE_STREAM_DROP dropped: {raw}")
            return
        rec["receipt"] = time.time()
        att.lines.append(rec)
        tag = expect_tag(self.expect, att.job.name, rec)
        self.log_stream(f"{att.job.name} {att.hs.name} {att.n} {rec['receipt']:.3f} {raw}"
                        + (f" {tag}" if tag else ""))
        if rec["verb"] == "CRASHED":
            print(f"[fleet] {att.job.name}: CRASHED {rec['tile']} {rec['detail']} "
                  f"(on {att.hs.name}, attempt {att.n})")
        if rec["verb"] in MOVED_VERBS:
            lat = ""
            if rec["epoch"] is not None and att.hs.name in self.skew:
                lat = f", received {rec['receipt'] - rec['epoch'] + self.skew[att.hs.name]:.1f} s after it was written"
            print(f"[fleet] {att.job.name}: stream: {rec['verb']} {rec['tile']} {rec['detail']}"
                  f"{' ' + tag if tag else ''}{' (imported)' if rec['imported'] else ''}{lat}")
            att.backlog.append(rec)

    def pull_stream(self, att):
        """Pull up to STREAM_K queued divergent and extra tiles and identity pairs in one rsync and render their heat diffs.

        A failed pull re-queues its records.
        @return True when the rsync succeeded.
        """
        take, att.backlog = att.backlog[:STREAM_K], att.backlog[STREAM_K:]
        names = []
        for rec in take:
            names.append(rec["tile"])
            if rec["verb"] == "EXCEED":
                names.append(rec["tile"].replace("__ident__", "__canon__"))
        dest = self.stream_dir(att.job)
        rc, err = rsync_pull(att.hs.host, f"{att.unit_dir}/cand", dest, names=sorted(set(names)))
        if rc != 0:
            print(f"[fleet] {att.job.name}: stream pull of {len(take)} item(s) failed: {err}")
            att.backlog = take + att.backlog
            return False
        for rec in take:
            try:
                note = self.stream_heat(att.job, rec, dest)
            except Exception as e:  # a render failure is logged and affects no result
                note = f"heat diff failed: {type(e).__name__}: {e}"
            print(f"[fleet] {att.job.name}: stream pulled {rec['tile']}: {note}")
        return True

    def drain_stream(self, att):
        """Pull every queued record of a finished attempt, STREAM_K per rsync, so the stream directory holds every moved tile; stop at the first failed pull."""
        while att.backlog and self.pull_stream(att):
            pass

    def load_diffmod(self):
        """Import the tree's presentation-corpus-diff.py (heat diffs) once."""
        if self.diffmod is None:
            path = os.path.join(self.tree, "tests", "harness", "presentation-corpus-diff.py")
            spec = importlib.util.spec_from_file_location("presentation_corpus_diff", path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            # The script's own import of presentation-blessed.py replaced
            # self.pb in sys.modules; pb.digest_files pickles its worker by
            # that name, so both use self.pb.
            mod.pb = self.pb
            sys.modules["presentation_blessed"] = self.pb
            self.diffmod = mod
        return self.diffmod

    def unit_rows(self, job):
        """Return a unit's blessed rows by tile, its tile directory and its manifest sizes (cached)."""
        if job.unit not in self.rows_cache:
            path = os.path.join(self.tree, self.pb.manifest_path(job.game, job.tier))
            m = self.pb.load_manifest(job.game, job.tier)
            self.rows_cache[job.unit] = (
                {r["tile"]: r for r in m.rows},
                self.load_diffmod().manifest_tile_dir(job.game, path),
                {x for x in m.header.get("size", "").split(",") if "x" in x})
        return self.rows_cache[job.unit]

    def stream_heat(self, job, rec, dest):
        """Render one pulled record's heat diff into <dest>/diff: a DIVERGE tile against its reference, an identity pair canon against ident.

        An EXTRA tile has no blessed row and so no reference: it is decoded
        and its size reported, and no heat diff is rendered.
        """
        diff = self.load_diffmod()
        name = rec["tile"]
        heat_dir = os.path.join(dest, "diff")
        if rec["verb"] == "EXTRA":
            dig = self.pb.digest_file(os.path.join(dest, name))
            if dig is None:
                return "undecodable"
            return f"new row {dig[1][0]}x{dig[1][1]}, no reference, no heat diff"
        if rec["verb"] == "EXCEED":
            canon = name.replace("__ident__", "__canon__")
            res = diff.diff_images(os.path.join(dest, canon), os.path.join(dest, name),
                                   os.path.join(heat_dir, name.replace("-vk.png", "-diff.png")))
            return f"identity pair {res[0]} {res[1:3]}"
        rows, tdir, sizes = self.unit_rows(job)
        path = os.path.join(dest, name)
        dig = self.pb.digest_file(path)
        if dig is None:
            return "undecodable"
        if self.archive is None:
            self.archive = self.pb.Archive()
        return diff.divergent_line(name, rows[self.pb.copy_origin(name)[0]], path, dig, tdir,
                                   self.archive, heat_dir, job.game, sizes)

    def check_stream(self, job, report_path):
        """Unit-end check: the accepted attempt's stream equals its verdict report and its moved tiles are in the stream directory; a mismatch sets rc 1.

        Exempt: stopped and SKIP units, and an attempt whose stream holds no
        END (its capture did not reach the verdict).
        """
        att = job.atts.get(job.result.get("attempt"))
        lines = att.lines if att else []
        if job.stopped or job.result["rc"] == EXIT_SKIP or not any(r["verb"] == "END" for r in lines):
            job.result["stream"] = "exempt"
            print(f"[fleet] {job.name}: stream: exempt (no END in attempt "
                  f"{job.result.get('attempt', '-')})")
            return
        bad = stream_mismatches(lines, report_path)
        bad += stream_absent(lines, self.stream_dir(job))
        sets, credits = stream_sets(lines)
        print(f"[fleet] {job.name}: stream: {len(sets['SAME'])} SAME + {len(sets['DIVERGE'])} "
              f"DIVERGE, {len(sets['EXTRA'])} EXTRA, {len(sets['IDENT'])} IDENT, "
              f"{len(sets['EXCEED'])} EXCEED, CREDITS {credits or 'absent'}; "
              + ("equals the report" if not bad else f"{len(bad)} mismatch(es)"))
        if bad:
            job.result["rc"] = 1
            job.result["stream_mismatch"] = "; ".join(bad)
            print(f"[fleet] {job.name}: stream mismatch: {job.result['stream_mismatch']}")

    def requeue(self, att, pending, killed):
        """Re-queue an attempt's job while attempts remain, else record its failure."""
        att.hs.current = None
        att.t_end = att.t_end or time.monotonic()
        job = att.job
        if job.attempts - job.moves < MAX_ATTEMPTS and job.kind != "build":
            pending.insert(0, job)
            print(f"[fleet] {job.name}: re-queued (attempt {job.attempts - job.moves} of "
                  f"{MAX_ATTEMPTS} lost)")
        else:
            job.result = {"rc": 1, "status": UNFINISHED_STATUS[0] if killed else UNFINISHED_STATUS[1],
                          "host": att.hs.name, "attempt": att.n}

    def finish_attempt(self, att, done, pending, on_done):
        """Accept the current attempt's done marker (or drop a busy host) and record the result."""
        hs, job = att.hs, att.job
        hs.current = None
        att.t_end = time.monotonic()
        if str(done.get("attempt")) != str(att.n):
            print(f"[fleet] {hs.name}: {job.name}: done marker of attempt {done.get('attempt')} "
                  f"ignored (current {att.n})")
            self.stop_and_requeue(att, pending)
            return
        rc = int(done.get("rc", 1))
        if rc == 75:
            job.attempts -= 1
            pending.insert(0, job)
            self.drop(hs, "golden lock busy")
            return
        job.result = {"rc": rc, "status": done.get("status", ""), "host": hs.name,
                      "attempt": att.n, "wall": int(done.get("wall", 0)),
                      "cand_bytes": int(done.get("cand-bytes", 0)), "speed": hs.speed,
                      "unit_dir": att.unit_dir,
                      "min_memavail_kib": int(done.get("min-memavail-kib") or 0)}
        print(f"[fleet] {hs.name}: {job.name} attempt {att.n} finished: rc {rc} "
              f"({job.result['status']}) in {job.result['wall']} s")
        if on_done:
            pending.extend(on_done(job, hs) or ())

    # -- verdict and pull (step 9) --

    def pull_target(self, game, tier):
        """Return the local candidate directory a unit's results are pulled into."""
        out = os.environ.get("PRESENTATION_GOLDEN_OUT")
        if out:
            return out if len(self.units) == 1 else os.path.join(out, f"{game}-{tier}")
        prefix = "presentation-candidate" if tier == "presentation" else "dragon-candidate"
        name = prefix if game == "spyro-1" else f"{prefix}-{game}"
        return os.path.join(self.tree, "build", name)

    def unit_done(self, job, hs):
        """Pull, check and render a finished unit; canaries pull their tiles only."""
        if job.kind == "canary":
            dst = self.canary_dir(hs.name)
            shutil.rmtree(dst, ignore_errors=True)
            rc, err = rsync_pull(hs.host, f"{job.result['unit_dir']}/cand", dst,
                                 includes=["*-vk.png", SIDECAR_GLOB, "verdict-report.txt", CREDITS_NAME,
                                           "capture-record.txt", "capture-device.txt"])
            if rc != 0:
                job.result.update(rc=1, status=f"canary pull failed: {err}")
                return
            self.note_census(job, parse_report(os.path.join(dst, "verdict-report.txt")))
            return
        if job.result["rc"] in (0, 1):
            errs = self.pull_unit(job, hs)
            if errs:
                job.result.update(rc=1, status="; ".join(errs))
                for e in errs:
                    print(f"[fleet] {job.name}: REFUSED: {e}")
            elif job.result["rc"] in (0, 1):
                self.record_time(job)
        self.keep_record(job, hs)
        if self.stream:
            att = job.atts.get(job.result.get("attempt"))
            if att is not None:
                self.drain_stream(att)
            self.check_stream(job, os.path.join(self.pull_target(job.game, job.tier),
                                                "verdict-report.txt"))

    def pull_unit(self, job, hs):
        """Pull a unit's report and divergences into a cleared local directory; return refusals."""
        local = self.pull_target(job.game, job.tier)
        shutil.rmtree(local, ignore_errors=True)
        os.makedirs(local)
        rdir = f"{job.result['unit_dir']}/cand"
        first = ["verdict-report.txt", "capture-record.txt", "capture-device.txt",
                 "verdict-ident.log", "verdict-diff.log"]
        if job.tier == "presentation":
            first += [CREDITS_NAME, DIGESTS_NAME, ROW_COST_NAME, SETTLE_NAME]
        if self.a.pull == "all":
            rsync_pull(hs.host, rdir, local)
        else:
            rsync_pull(hs.host, rdir, local, names=first)
        report = parse_report(os.path.join(local, "verdict-report.txt"))
        self.note_census(job, report)
        tiles, pairs = report["divergent"] + report["extra"], report["identity"]
        # row-cost.tsv and census-settle.tsv are absent when every row was
        # imported from an earlier attempt, census-settle.tsv also when no leg
        # wrote a sidecar line.
        want = [f for f in first if f not in (ROW_COST_NAME, SETTLE_NAME)] \
            + self.pull_tiles(hs, rdir, local, report)
        if job.tier == "presentation":
            # Every tile's sidecar (the tile census), in a call of its own: the
            # include pattern's --exclude=* would filter the named-file list.
            rc, err = rsync_pull(hs.host, rdir, local, includes=[SIDECAR_GLOB])
            if rc != 0:
                errs_pull = [f"sidecar pull failed: {err}"]
            else:
                errs_pull = census_pull_refusals(local)
                if self.repeat():
                    errs_pull += repeat_sidecar_refusals(self.pb, local)
                merge_row_cost(local, job, hs, self.run_id)
                self.note_settles(job, local)
        else:
            errs_pull = []
        errs = [f"pulled file absent: {f}" for f in want
                if not os.path.exists(os.path.join(local, f))]
        errs += errs_pull
        errs = self.accept_new_rows(job, report, errs + self.check_report(job, report))
        if os.path.exists(os.path.join(local, RECORD_NAME)):
            self.stamp_record(local, hs)
        print(f"[fleet] {job.name}: pulled into {local}: report, record, device, credits, "
              f"{len(report['divergent'])} divergent tile(s), {len(report['extra'])} extra tile(s), "
              f"{len(pairs)} identity tile(s), "
              f"{len(glob.glob(os.path.join(local, SIDECAR_GLOB)))} sidecar(s)")
        if tiles and not errs:
            self.heat_diffs(job, local)
        if not errs:
            self.keep_run_candidate(job, local)
            self.census_store_part(job, local)
        return errs

    def census_store_part(self, job, local):
        """Hard-link an accepted unit's sidecars and tile-digests.tsv into the census store's <run-id>.part/.

        Only a presentation unit that was not stopped, of a run whose
        run-args.json records none of CENSUS_FILTER_KEYS, and only when every
        sidecar names the capture record's source tree; a refusal is logged
        and leaves no entry. drive() finalizes the .part after the done marker.
        """
        if job.tier != "presentation" or job.stopped or self.filtered() or self.repeat():
            return
        ra = read_json_file(os.path.join(self.local_run_dir, "run-args.json")) or {}
        if any(ra.get(k) for k in CENSUS_FILTER_KEYS):
            return
        why = census_tree_refusal(local)
        if why:
            print(f"[fleet] {job.name}: census store entry refused: {why}")
            return
        part = os.path.join(census_root(self.local_run_dir), job.name, self.run_id + ".part")
        shutil.rmtree(part, ignore_errors=True)
        os.makedirs(part)
        names = [n for n in os.listdir(local)
                 if fnmatch.fnmatch(n, SIDECAR_GLOB) or n in (DIGESTS_NAME, SETTLE_NAME)]
        for n in names:
            link_or_copy(os.path.join(local, n), os.path.join(part, n))
        rec = parse_kv(open(os.path.join(local, RECORD_NAME), encoding="utf-8").read())
        f = parse_report(os.path.join(local, "verdict-report.txt"))["fields"]
        accept = {"run_id": self.run_id, "unit": job.name, "attempt": job.result.get("attempt"),
                  "head": rec.get("head"), "source_tree": rec.get("source-tree"),
                  "rc": job.result.get("rc"), "compared": f.get("compared"),
                  "sidecars": sum(1 for n in names if fnmatch.fnmatch(n, SIDECAR_GLOB))}
        with open(os.path.join(part, "accept.json"), "w", encoding="utf-8") as fh:
            json.dump(accept, fh)
        print(f"[fleet] {job.name}: census store: {accept['sidecars']} sidecar(s) into {part}")

    def pull_tiles(self, hs, rdir, local, report):
        """Pull a report's divergent and extra tiles and identity-pair members; return their paths relative to local.

        Under --pull all the whole candidate directory is already local, its
        identity pairs at the top level; otherwise the pairs go to identity/.
        """
        tiles, pairs = report["divergent"] + report["extra"], report["identity"]
        if self.a.pull != "all":
            if tiles:
                rsync_pull(hs.host, rdir, local, names=tiles)
            if pairs:
                rsync_pull(hs.host, rdir, os.path.join(local, "identity"), names=pairs)
        return tiles + [os.path.join("identity" if self.a.pull != "all" else "", p) for p in pairs]

    def keep_run_candidate(self, job, local):
        """Copy an accepted unit pull into build/fleet-stream/<run id>/<unit>/, the run's own promotable candidate.

        The pull target is shared: the next local capture or fleet run of the
        tier clears it. The stream directory is the run's alone, so it receives
        every pulled file and directory (hard links where the filesystem allows,
        copies otherwise), the stamped capture record among them. A top-level
        golden tile the stream pulled that the accepted pull does not hold (an
        identity pair member, or a tile of a superseded attempt) moves to
        STREAM_ONLY_DIR, so promote_select sees exactly the accepted attempt's
        tiles under the record that describes them.
        """
        dest = self.stream_dir(job)
        os.makedirs(dest, exist_ok=True)
        pulled = set(os.listdir(local))
        for n in os.listdir(dest):
            if n.endswith("-vk.png") and n not in pulled and os.path.isfile(os.path.join(dest, n)):
                os.makedirs(os.path.join(dest, STREAM_ONLY_DIR), exist_ok=True)
                os.replace(os.path.join(dest, n), os.path.join(dest, STREAM_ONLY_DIR, n))
        for n in sorted(pulled):
            src, dst = os.path.join(local, n), os.path.join(dest, n)
            if os.path.isdir(src):
                shutil.copytree(src, dst, dirs_exist_ok=True, copy_function=link_or_copy)
            else:
                link_or_copy(src, dst)
        job.candidate = dest
        flags = "" if job.tier == "presentation" else f"PRESENTATION_TIER={job.tier} "
        print(f"[fleet] {job.name}: run candidate {os.path.relpath(dest, self.tree)}; promote with "
              f"{flags}tests/harness/presentation-corpus-promote.sh "
              f"{os.path.relpath(dest, self.tree)} --game {job.game} <tile>...")

    def keep_record(self, job, hs):
        """Put the unit's capture record, device line and verdict report into its stream directory at a unit end that kept no run candidate.

        A refused pull, a SKIP or another exit leaves the streamed tiles in
        build/fleet-stream/<run id>/<unit>/ without the record that
        presentation-corpus-promote.sh reads; the three files are pulled from
        the unit's candidate directory on the host, the record stamped
        (stamp_record), and moved in. A capture that wrote no record is
        printed.
        """
        if job.candidate or not job.result.get("unit_dir"):
            return
        dest = self.stream_dir(job)
        tmp = os.path.join(dest, ".record-pull")
        shutil.rmtree(tmp, ignore_errors=True)
        names = [RECORD_NAME, "capture-device.txt", "verdict-report.txt"]
        rsync_pull(hs.host, f"{job.result['unit_dir']}/cand", tmp, names=names)
        got = [n for n in names if os.path.exists(os.path.join(tmp, n))]
        if RECORD_NAME in got:
            self.stamp_record(tmp, hs)
        for n in got:
            os.replace(os.path.join(tmp, n), os.path.join(dest, n))
        shutil.rmtree(tmp, ignore_errors=True)
        rel = os.path.relpath(dest, self.tree)
        if RECORD_NAME in got:
            print(f"[fleet] {job.name}: {', '.join(got)} kept in {rel} (rc {job.result['rc']}, "
                  f"{job.result.get('status', '')}); its streamed tiles promote with "
                  f"tests/harness/presentation-corpus-promote.sh {rel} <tile>...")
        else:
            print(f"[fleet] {job.name}: no {RECORD_NAME} on {hs.name}: the capture wrote none; "
                  f"{rel} is not promotable")

    def stamp_record(self, local, hs):
        """Append the run id, the fleet host name and the local run directory to a pulled capture record."""
        path = os.path.join(local, RECORD_NAME)
        with open(path, encoding="utf-8") as f:
            keep = [ln for ln in f if ln.partition("=")[0] not in FLEET_RECORD_KEYS]
        vals = (self.run_id, hs.name, os.path.abspath(self.local_run_dir))
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.writelines(keep)
            f.writelines(f"{k}={v}\n" for k, v in zip(FLEET_RECORD_KEYS, vals))
        os.replace(path + ".tmp", path)

    def note_settles(self, job, local):
        """Print a pulled unit's frozen-backdrop settle count and tally pass time (census-settle.tsv)."""
        gc = load_census(self.tree)
        summary = gc.settle_summary(os.path.join(local, SETTLE_NAME))
        line = gc.settle_line(summary) if summary else f"no {SETTLE_NAME} pulled"
        job.result["settles"] = line
        print(f"[fleet] {job.name}: backdrop settles: {line}")

    def note_census(self, job, report):
        """Print a verdict report's census (captured, imported and compared tiles) and keep it in the result."""
        f = report["fields"]
        census = (f"captured={f.get('captured', '?')} imported={f.get('imported', '0')} "
                  f"({f.get('imported-rows', '0')} rows) compared={f.get('compared', '?')} "
                  f"of {f.get('manifest-rows', '?')}")
        job.result["census"] = census
        job.result["imported_rows"] = int(f.get("imported-rows") or 0)
        job.result["crashed"] = report["crashed"]
        print(f"[fleet] {job.name}: census {census}")
        if self.repeat() and job.kind == "unit":
            rows = [repeat_row_text(r) for r in report["repeat-rows"]]
            job.result["repeat"] = "; ".join(rows) or "no repeat-row line"
            for r in rows:
                print(f"[fleet] {job.name}: repeat {r}")

    def canary_dir(self, host_name):
        """Return the local directory one host's canary tiles are pulled into."""
        if self.canary_only:
            return os.path.join(self.pull_target(self.canary_game(), "presentation"),
                                f"canary-{host_name}")
        return os.path.join(self.local_run_dir, "canary", host_name)

    def check_report(self, job, report):
        """Compare the report's manifest hashes and census with the local files."""
        golden = os.path.join(self.tree, "tests", "golden", job.game, job.tier)
        errs = []
        if not report["fields"]:
            return ["verdict-report.txt absent or empty"]
        for key, name in (("blessed-sha256", "blessed.tsv"),
                          ("ident-baseline-sha256", "ident-baseline.tsv")):
            got = first_hex(report["fields"].get(key, ""))
            want = sha256_path(os.path.join(golden, name))
            if got != want:
                errs.append(f"{key} {got[:12] or 'absent'} differs from the local {name} "
                            f"{want[:12] or 'absent'}")
        rows = len(self.pb.load_manifest(job.game, job.tier).rows)
        compared = report["fields"].get("compared", "")
        if self.filtered():
            if not compared.isdigit() or int(compared) == 0:
                errs.append(f"census compared={compared or 'absent'}: {COMPARED_NOTHING}")
            if self.repeat():
                errs += repeat_refusals(report, self.repeat())
        elif compared != str(rows):
            errs.append(f"census compared={compared or 'absent'} differs from the local "
                        f"blessed row count {rows}")
        if report["fields"].get("exit") != str(job.result["rc"]):
            errs.append(f"report exit {report['fields'].get('exit')} differs from the "
                        f"supervisor's rc {job.result['rc']}")
        return errs

    def accept_new_rows(self, job, report, errs):
        """Return the refusals of a pulled unit without the compared-nothing one when new_rows() holds; with none left, the unit ends NEW-ROWS (rc 0).

        A filtered capture of rows with no blessed entry compares nothing and
        its harness exits 1 on the extra tiles; that is the expected end of
        a new row's first capture, not a failure.
        """
        new = self.new_rows(report)
        if not new:
            return errs
        errs = [e for e in errs if COMPARED_NOTHING not in e]
        if not errs:
            job.result.update(rc=0, status=f"NEW-ROWS {len(new)} tile(s)", new_rows=new)
            print(f"[fleet] {job.name}: NEW-ROWS {len(new)} tile(s) with no blessed row: " + " ".join(new))
        return errs

    def new_rows(self, report):
        """Return a filtered unit's extra tiles when they are its only move (the rows have no blessed entry yet), else [].

        Only moves: no divergent tile, no identity-leg failure, no crashed leg,
        no incomplete capture, credits passed or not run.
        """
        f = report["fields"]
        if not self.filtered() or self.repeat() or not report["extra"]:
            return []
        if report["divergent"] or report["identity"] or report.get("crashed") or report.get("missing") \
                or "capture-incomplete" in "".join(f) or f.get("credits-rc", "64") not in ("0", "64"):
            return []
        return sorted(report["extra"])

    def heat_diffs(self, job, local):
        """Render heat diffs of the pulled divergent tiles against the local store."""
        blessed = os.path.join("tests", "golden", job.game, job.tier, "blessed.tsv")
        cp = subprocess.run([sys.executable, "tests/harness/presentation-corpus-diff.py", local,
                             "--game", job.game, "--manifest", blessed, "--diff-out",
                             os.path.join(local, "diff"), "--expect-subset"],
                            cwd=self.tree, capture_output=True, text=True)
        for line in cp.stdout.splitlines():
            if not line.startswith("ok "):
                print(f"[fleet] {job.name}: {line}")

    def unit_time(self, job):
        """Return (wall_s, norm_s, hosts) of an accepted unit over the attempts that captured its rows, or None when the time is not the whole unit's.

        norm_s sums wall x speed over the accepted attempt and, when it
        imported stamped rows, each earlier attempt whose rows it imported:
        walking back, an attempt counts while it ran on the same host as
        its successor (a re-queue imports from the unit directory) or a
        move carried its rows. Each attempt's time is priced at its own
        host's speed, so the sum estimates the unit at speed 1.00 the way a
        single attempt's wall x speed does, and a whole unit's row is
        unchanged. Over-count: the rows in flight at each kill, recaptured.
        None: a filtered run, a resumed unit, an import whose source attempt
        is not this coordinator's, or an attempt time not recorded.
        """
        r = job.result
        att = job.atts.get(r.get("attempt"))
        if att is None or self.filtered() or self.resumed:
            return None
        chain = [(att, float(r["wall"]))]
        if r.get("imported_rows", 0) > 0:
            cur = att
            for n in range(att.n - 1, 0, -1):
                prev = job.atts.get(n)
                if prev is None or not (prev.hs is cur.hs or n in job.carried):
                    break
                if prev.t_end is None:
                    return None
                chain.append((prev, prev.t_end - prev.t0))
                cur = prev
            if len(chain) == 1:
                return None
        wall = sum(w for _a, w in chain)
        norm = sum(w * a.hs.speed for a, w in chain)
        return wall, norm, ">".join(a.hs.name for a, _w in reversed(chain))

    def record_time(self, job):
        """Append an accepted unit's time (unit_time) to the ledger; nothing when it has none."""
        got = self.unit_time(job)
        if got is None:
            return
        wall, norm, hosts = got
        append_ledger({"date": datetime.datetime.now().isoformat(timespec="seconds"),
                       "run": self.run_id, "game": job.game, "tier": job.tier,
                       "host": hosts, "speed": f"{norm / wall if wall else job.result['speed']:.2f}",
                       "wall_s": int(wall), "norm_s": int(norm),
                       "cand_bytes": job.result["cand_bytes"]})

    def compare_canaries(self, jobs):
        """Refuse the run when a canary tile's digest differs between hosts (step 8); write the per-host verdict file."""
        canaries = [j for j in jobs if j.kind == "canary"]
        done = [j for j in canaries if j.result and j.result["rc"] in (0, 1)]
        verdict = {j.pinned: "fail canary did not capture (" + (
            f"rc {j.result['rc']}: {j.result.get('status', '')}" if j.result else "no result")
            + ")" for j in canaries if j not in done}
        rc, why = 0, None
        if not self.canary_on():
            self.write_canary_verdict({}, "fewer than two hosts")
            return 0
        if len(done) < 2:
            print(f"[fleet] canary: {len(done)} host(s) captured it; no cross-host comparison")
            why = f"{len(done)} host(s) captured the canary; no cross-host comparison"
            verdict.update({j.pinned: f"fail {why}" for j in done})
            self.write_canary_verdict(verdict, None)
            return 0
        digests = {j.pinned: self.canary_digests(j.pinned) for j in done}
        names = sorted(set().union(*digests.values()))
        bad = [n for n in names if len({digests[h].get(n, "absent") for h in digests}) > 1]
        hosts = ", ".join(sorted(digests))
        if not names:
            print(f"[fleet] canary: VACUOUS: no canary tile on {hosts}")
            rc, why = 1, f"VACUOUS: no canary tile on {hosts}"
        elif bad:
            for n in bad:
                print(f"[fleet] canary: {n}: " + ", ".join(
                    f"{h}={digests[h].get(n, 'absent')[:12]}" for h in sorted(digests)))
            print(f"[fleet] canary: REFUSED: {len(bad)} of {len(names)} tile(s) differ between "
                  f"{hosts}")
            rc, why = 1, f"{len(bad)} of {len(names)} tile(s) differ between {hosts}"
        else:
            print(f"[fleet] canary: {len(names)} tile(s) identical on {hosts}")
        ok = f"pass {len(names)} tile(s) identical on {hosts}"
        verdict.update({h: f"fail {why}" if why else ok for h in digests})
        self.write_canary_verdict(verdict, None)
        return rc

    def write_canary_verdict(self, verdict, off_reason):
        """Write CANARY_VERDICT in the local run directory: the run id, whether a canary ran, and each host's verdict."""
        lines = [f"run={self.run_id}", f"canary=off ({off_reason})" if off_reason else "canary=on"]
        lines += [f"canary.{h}={v}" for h, v in sorted(verdict.items())]
        path = os.path.join(self.local_run_dir, CANARY_VERDICT)
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(path + ".tmp", path)

    def canary_digests(self, host_name):
        """Return {tile: verdict digest} of the canary tiles pulled from one host, and {sidecar: sha256} of their sidecars."""
        d = self.canary_dir(host_name)
        files = sorted(glob.glob(os.path.join(d, "*-vk.png")))
        out = {os.path.basename(p): (dg[0] if dg else "undecodable")
               for p, dg in self.pb.digest_files(files).items()}
        out.update({os.path.basename(p): sha256_path(p)
                    for p in sorted(glob.glob(os.path.join(d, SIDECAR_GLOB)))})
        return out

    # -- teardown --

    def stop_current(self, hs):
        """Stop a host's in-flight attempt (session and containers) on the way out."""
        try:
            self.poll_attempt(hs.current)
            hs.host.run_script(KILL_SCRIPT, [hs.engine, self.run_id, hs.root, 0,
                                             hs.current.base], timeout=120)
            print(f"[fleet] {hs.name}: stopped {hs.current.job.name}")
        except FleetError as e:
            print(f"[fleet] {hs.name}: stop failed: {e}")

    def pull_crashes(self, hs):
        """Pull a host's crashed capture legs (every unit's cand.crashes) into <local run dir>/crashes/<host>; print each leg."""
        dst = os.path.join(self.local_run_dir, "crashes", hs.name)
        argv = ["rsync", "-a", "-m", *rsync_base(hs.host), "--include=u-*/",
                "--include=u-*/cand.crashes/***", "--exclude=*",
                remote_path(hs.host, hs.run_dir.rstrip("/")) + "/", dst + "/"]
        os.makedirs(dst, exist_ok=True)
        cp = transport_run(hs.host, "crash pull", argv, text=True)
        if cp.returncode != 0:
            print(f"[fleet] {hs.name}: crash pull failed (rc {cp.returncode}): {cp.stderr.strip()[-300:]}")
        legs = sorted(glob.glob(os.path.join(dst, "u-*", "cand.crashes", "*", "*", "rc.txt")))
        for rc_txt in legs:
            rc = parse_kv(open(rc_txt, encoding="utf-8", errors="replace").read()).get("rc", "?")
            print(f"[fleet] {hs.name}: crashed leg kept: {os.path.dirname(rc_txt)} (rc={rc})")
        if not legs and cp.returncode == 0:
            shutil.rmtree(dst, ignore_errors=True)
            try:
                os.rmdir(os.path.join(self.local_run_dir, "crashes"))
            except OSError:
                pass

    def teardown(self):
        """Pull crashed legs, stop in-flight attempts and delete this run's directories on every touched host."""
        self.span.release()
        for hs in self.hosts:
            if hs.current:
                self.stop_current(hs)
        self.reap_remote()
        try:
            self.keep_unfinished()
        except (FleetError, OSError) as e:
            print(f"[fleet] keeping unfinished units failed: {e}")
        for hs in self.hosts:
            if not hs.touched:
                continue
            try:
                self.pull_crashes(hs)
            except OSError as e:
                print(f"[fleet] {hs.name}: crash pull failed: {e}")
            keep = [e for e in os.listdir(self.local_run_dir) if run_keep(e)] if hs.host.local else []
            try:
                cp = hs.host.run_script(DELETE_SCRIPT, [hs.run_dir, *keep], timeout=600)
                ok = cp.returncode == 0
            except FleetError:
                ok = False
            print(f"[fleet] {hs.name}: " + (f"deleted {hs.run_dir}" if ok else
                                            f"{hs.run_dir} kept for clean (host unreachable)"))
        for e in os.listdir(self.local_run_dir):
            if not run_keep(e):
                p = os.path.join(self.local_run_dir, e)
                shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.unlink(p)
        if self.archive is not None:
            self.archive.close()
            self.archive = None
        if self.golden_fd is not None:
            os.close(self.golden_fd)
            self.golden_fd = None

    # -- the whole run --

    def run(self):
        """Execute the run; return the exit code (1 > 64 > 0 over units).

        Before the legs (preflight, image ship, build, sync, recheck) a stop
        request covering every unit is polled every STOP_POLL_S: the running
        child's process group is killed and every unit is recorded stopped.
        """
        with run_tag(self.run_id):
            try:
                with stop_watch(self.stop_covers_run):
                    stop_point()
                    src_bytes = set_bytes(self.tree, source_set(self.tree, self.pb,
                                                                sorted({g for g, _t in self.units})))
                    self.src_bytes = src_bytes
                    self.write_run_args()
                    self.load_expect()
                    self.prepare_hosts(src_bytes)
                    self.measure_skew()
                    self.build_and_sync()
                    stop_point()
            except StopRequested:
                return self.stop_before_legs()
            self.jobs = self.make_jobs()
            self.run_jobs(self.jobs, on_done=self.unit_done)
        rc = self.run_rc()
        self.print_summary(rc)
        return rc

    def run_rc(self):
        """Return the run's exit code over its jobs: the worst unit (canary under CANARY_ONLY), 1 on a canary refusal or a stopped job."""
        decided = "canary" if self.canary_only else "unit"
        rc = worst_rc(j.result["rc"] if j.result else 1 for j in self.jobs if j.kind == decided)
        if self.compare_canaries(self.jobs):
            rc = 1
        if any(j.stopped for j in self.jobs):
            rc, self.done_reason = 1, STOP_STATUS
        return rc

    def write_run_args(self):
        """Record the run's arguments and identity in <local run dir>/run-args.json, which `resume` and the tile census read.

        The unit filters, repeat count and pull mode; the checkout's HEAD and
        source tree (scripts/presentation-blessed.py source_tree), the games
        and units, the batch's commit range (--commits) and the sha256 of
        each query file (--queries, the run directory's copy), taken before
        any leg starts.
        """
        args = {k: getattr(self.a, k, None) for k in ("filter", "freecam_filter", "pull", "repeat")}
        args.update(head=git_head(self.tree), source_tree=self.pb.source_tree(),
                    games=sorted({g for g, _t in self.units}),
                    units=sorted(f"{g}-{t}" for g, t in self.units),
                    commits=getattr(self.a, "commits", None),
                    queries=query_hashes(getattr(self.a, "queries", None)))
        with open(os.path.join(self.local_run_dir, "run-args.json.tmp"), "w") as f:
            json.dump(args, f)
        os.replace(os.path.join(self.local_run_dir, "run-args.json.tmp"),
                   os.path.join(self.local_run_dir, "run-args.json"))

    def load_expect(self):
        """Read the run's copy of the expected-moves file; refuse the run (2) on an unknown unit or a substring matching nothing."""
        if not getattr(self.a, "expect", None):
            return
        copy = os.path.join(self.local_run_dir, "expect.txt")
        if not os.path.exists(copy):
            shutil.copyfile(self.a.expect, copy)
        self.expect = bind_launch_lod(self.tree, parse_expect(copy))
        errs = expect_refusals(self.tree, self.pb, self.units, self.expect)
        if errs:
            raise FleetError("expected-moves file refused: " + "; ".join(errs), 2)

    def expect_summary(self):
        """Print each expectation's status and the EXPECTED, UNEXPECTED and EXPECTED-NOT-MOVED tile counts."""
        units = {}
        for j in self.jobs:
            if j.kind != "unit":
                continue
            r = j.result or {}
            att = j.atts.get(r.get("attempt"))
            reached = bool(att) and not j.stopped and r.get("rc") != EXIT_SKIP
            blessed = {x["tile"] for x in self.pb.load_manifest(j.game, j.tier).rows}
            units[j.name] = (reached, att.lines if att else [], blessed)
        counts, rows = expect_counts(self.expect, units)
        for e, status, match in rows:
            print(f"[fleet] expect {e.unit} '{e.sub}': {status} ({len(match)} matching tile(s)); "
                  f"{e.reason}")
        print(f"[fleet] expected moves: EXPECTED {counts['EXPECTED']}, UNEXPECTED "
              f"{counts['UNEXPECTED']}, EXPECTED-NOT-MOVED {counts['EXPECTED-NOT-MOVED']}")

    def print_summary(self, rc):
        """Print each unit's host beside its verdict, then the run's verdict."""
        for j in self.jobs:
            r = j.result or {"rc": 1, "status": "no result", "host": "-"}
            verdict = "NEW-ROWS" if r.get("new_rows") else {0: "PASS", EXIT_SKIP: "SKIP"}.get(r["rc"], "FAIL")
            census = f"; {r['census']}" if r.get("census") else ""
            moved = "".join(f"; moved {a} -> {b}" for a, b in j.moved)
            crashed = crashed_text(r.get("crashed") or [])
            print(f"[fleet] result {j.name}: {verdict} on {r['host']} (rc {r['rc']}, "
                  f"attempt {r.get('attempt', '-')}, {r.get('wall', '-')} s; {r['status']}{census}"
                  f"{'; ' + crashed if crashed else ''}{moved})")
            for c in r.get("crashed") or []:
                print(f"[fleet] result {j.name}: CRASHED {c['leg']} rc={c['rc']}"
                      + (f" retry={c['retry']}" if c["retry"] else ""))
            if r.get("new_rows"):
                print(f"[fleet] result {j.name}: NEW-ROWS {len(r['new_rows'])} tile(s) with no blessed "
                      f"row (promote them from {os.path.relpath(self.stream_dir(j), self.tree)}): "
                      + " ".join(r["new_rows"]))
            if r.get("repeat"):
                print(f"[fleet] result {j.name}: repeat {r['repeat']}")
            if r.get("stream_mismatch"):
                print(f"[fleet] result {j.name}: stream mismatch: {r['stream_mismatch']}")
        self.takeover_summary()
        if self.expect:
            self.expect_summary()
        print(f"[fleet] run {self.run_id}: " + {0: "PASS", EXIT_SKIP: "SKIP"}.get(rc, "FAIL")
              + f" (exit {rc}" + (f", reason={self.done_reason}" if self.done_reason else "") + ")")


class Resumer(Coordinator):
    """`resume`: finish one unit of a run whose coordinator has exited, on one host, from its saved stamped rows.

    The run id is the stopped run's, so the rows' stamps (run, tree, binary)
    match when the snapshot is the run's: its kept tree when present, else
    the checkout, built and refused unless its fields and binaries equal the
    run's. The run directory gets a new done marker whose rc is the worst
    over this unit's result and every other unit's last logged result.
    """

    resumed = True

    def __init__(self, a, run_id, local_run_dir):
        """Bind the unit, its target host and the saved rows."""
        game, tier = split_unit(a.unit)
        a.game, a.tier, a.all, a.all_tiers = game, tier, False, False
        try:
            with open(os.path.join(local_run_dir, "run-args.json"), encoding="utf-8") as f:
                rec = json.load(f)
        except (OSError, ValueError):
            rec = {}
        for k in ("filter", "freecam_filter", "repeat"):
            if getattr(a, k, None) is None:
                setattr(a, k, rec.get(k))
        a.hosts, a.no_local, a.expect = a.to, False, None
        super().__init__(a, run_id, local_run_dir)
        self.saved = resolve_saved(a.from_dir or os.path.join(local_run_dir, "kept"), a.unit,
                                   local_run_dir)
        prior = unit_hosts(local_run_dir, a.unit)
        self.rows_host = self.saved["host"] or (prior[-1] if prior else None)

    def canary_needed(self):
        """True when the target is not the host that captured the saved rows."""
        return self.rows_host != self.a.to

    def run(self):
        """Admit the target, give it the run's snapshot and the saved rows, run its canary and the unit; return the run's rc."""
        with run_tag(self.run_id):
            game = self.units[0][0]
            self.src_bytes = set_bytes(self.tree, source_set(self.tree, self.pb, [game]))
            self.load_resume_expect()
            print(f"[fleet] resume {self.a.unit} of run {self.run_id} on {self.a.to}; rows from "
                  f"{self.saved['cand']} (captured on {self.rows_host or 'an unknown host'})")
            hs = self.add_host(self.a.to)
            self.hosts, self.sel = [hs], [hs.host]
            self.measure_skew()
            self.resume_snapshot(hs)
            job = Job("unit", game, self.units[0][1], pinned=hs.name)
            rows, png = self.push_rows(hs, job, self.saved["cand"])
            print(f"[fleet] {job.name}: {rows} saved row(s), {png} tile(s) placed on {hs.name} "
                  f"for import")
            job.moved.append((self.rows_host or "-", hs.name))
            self.jobs = [job]
            if self.canary_needed():
                self.jobs.insert(0, Job("canary", game, "presentation", pinned=hs.name))
            self.run_jobs(self.jobs, on_done=self.unit_done)
        rc = self.run_rc()
        self.print_summary(rc)
        return rc

    def load_resume_expect(self):
        """Read the run's expected-moves file, keeping the expectations of the resumed unit."""
        copy = os.path.join(self.local_run_dir, "expect.txt")
        if os.path.exists(copy):
            self.expect = [e for e in bind_launch_lod(self.tree, parse_expect(copy))
                           if e.unit == self.a.unit]

    def write_resume_ship(self):
        """Write the ship directory: this script's supervisor, the run's fields file and binary hashes."""
        os.makedirs(self.ship_dir, exist_ok=True)
        with open(os.path.join(self.ship_dir, "supervise.sh"), "w") as f:
            f.write(SUPERVISOR)
        shutil.copyfile(self.saved["fields"],
                        os.path.join(self.ship_dir, f"fields-{self.units[0][0]}.txt"))
        if self.saved["binaries"]:
            shutil.copyfile(self.saved["binaries"], os.path.join(self.ship_dir, "binaries.sha256"))

    def resume_snapshot(self, hs):
        """Sync the run's kept tree to the host, or build and sync the checkout and refuse unless its fields and binaries are the run's."""
        game = self.units[0][0]
        tree = self.saved["tree"]
        if tree:
            if not self.saved["binaries"]:
                raise FleetError(f"kept tree {tree} without ship/binaries.sha256 beside it", 2)
            self.mkdirs(hs)
            self.write_resume_ship()
            check = subprocess.run(["sha256sum", "-c", "--strict", "--quiet",
                                    os.path.join(self.ship_dir, "binaries.sha256")],
                                   cwd=tree, capture_output=True, text=True)
            if check.returncode != 0:
                raise FleetError(f"kept tree {tree}: binaries differ from the run's hashes: "
                                 f"{check.stdout.strip()[-300:]}")
            skip = (f"{BUILD_TREE}/home/", f"{BUILD_TREE}/golden-disc-hash.tsv")
            files = [f for f in walk_files(tree, ".") if not f.startswith(skip)]
            print(f"[fleet] {hs.name}: snapshot from the kept tree {tree} ({len(files)} files)")
            self.sync_hosts([("snapshot", tree, files)], [hs])
            self.check_binaries(hs)
            self.refresh_base(hs)
            self.push_ship(hs)
            self.check_discs()
            return
        if not hs.host.cfg.get("build", True):
            raise FleetError(f"no kept tree and {hs.name} may not build (build = false)", 2)
        self.build_hs = hs
        self.build_and_sync()
        built = open(os.path.join(self.ship_dir, f"fields-{game}.txt"), encoding="utf-8").read()
        want = open(self.saved["fields"], encoding="utf-8").read()
        if built != want:
            diff = [f"{a.strip()} (run: {b.strip()})" for a, b in
                    zip(built.splitlines(), want.splitlines()) if a != b]
            raise FleetError("the checkout is not the stopped run's tree, so none of its rows "
                             "would import: " + "; ".join(diff), 1)
        if self.saved["binaries"]:
            got = open(os.path.join(self.ship_dir, "binaries.sha256"), encoding="utf-8").read()
            if got != open(self.saved["binaries"], encoding="utf-8").read():
                raise FleetError("the rebuilt binaries differ from the stopped run's "
                                 "(ship/binaries.sha256), so none of its rows would import", 1)
        else:
            print("[fleet] no saved binaries.sha256: the importer's binary check decides each row")

    def compare_canaries(self, jobs):
        """Compare the target's canary tiles with the kept canary tiles of the rows' host; merge canary.<target> into the run's canary verdict."""
        can = next((j for j in jobs if j.kind == "canary"), None)
        if can is None:
            return 0
        tgt, ref = can.pinned, self.rows_host
        refdir = os.path.join(self.saved["canary"], ref or "-") if self.saved["canary"] else ""
        rc, line = 0, ""
        if not can.result or can.result["rc"] not in (0, 1):
            line = "fail canary did not capture (" + (
                f"rc {can.result['rc']}: {can.result.get('status', '')}" if can.result else
                "no result") + ")"
        elif not refdir or not glob.glob(os.path.join(refdir, "*-vk.png")):
            line = f"fail no kept canary tiles of {ref or 'the rows host'} to compare with"
        else:
            mine = self.canary_digests(tgt)
            theirs = {os.path.basename(p): (dg[0] if dg else "undecodable") for p, dg in
                      self.pb.digest_files(sorted(glob.glob(os.path.join(refdir, "*-vk.png")))).items()}
            names = sorted(set(mine) | set(theirs))
            bad = [n for n in names if mine.get(n, "absent") != theirs.get(n, "absent")]
            if bad:
                for n in bad:
                    print(f"[fleet] canary: {n}: {tgt}={mine.get(n, 'absent')[:12]}, "
                          f"{ref}={theirs.get(n, 'absent')[:12]}")
                rc, line = 1, f"fail {len(bad)} of {len(names)} tile(s) differ between {ref}, {tgt}"
            else:
                line = f"pass {len(names)} tile(s) identical on {ref}, {tgt}"
            print(f"[fleet] canary: {line[5:]}")
        path = os.path.join(self.local_run_dir, CANARY_VERDICT)
        lines = open(path, encoding="utf-8").read().splitlines() if os.path.exists(path) else \
            [f"run={self.run_id}"]
        lines = [ln for ln in lines if not ln.startswith(f"canary.{tgt}=")
                 and not ln.startswith("canary=")]
        lines.insert(1, "canary=on")
        lines.append(f"canary.{tgt}={line}")
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        os.replace(path + ".tmp", path)
        return rc

    def run_rc(self):
        """Return the worst of this unit's rc (canary included) and every other unit's last logged result."""
        rc = super().run_rc()
        others = {n: r for n, r in unit_results(self.local_run_dir).items()
                  if n != self.a.unit and not n.startswith("canary-") and n != "build"}
        for n, (verdict, host, orc, status) in sorted(others.items()):
            print(f"[fleet] run {self.run_id}: {n}: {verdict} on {host} (rc {orc}; {status}) "
                  f"from an earlier coordinator")
            if status == STOP_STATUS and not self.done_reason:
                self.done_reason = f"{n} {STOP_STATUS}"
        return worst_rc([rc, *(r[2] for r in others.values())])


# ---- add and calibrate: the calibration unit through run's machinery ----------

CALIB_DEADLINE_S = 4 * 3600         # one calibration leg, any slots
CALIB_LEG_TIMEOUT_S = 3600          # a sweep leg's, or an uncalibrated host's, harness leg timeout
IDLE_WAIT_S = 600                   # longest wait for the dev box to fall idle before a local leg


class Calibrator(Coordinator):
    """Legs of the calibration unit, placed on hosts through `run`'s preflight, sync, build and supervisor.

    plans maps a measured host to "sweep" (slots 1, 2, 4, ... up to its
    core count, stopped by sweep_should_stop) or to a fixed slots value.
    Each leg is one supervisor job (capture, then --verdict-only) whose done
    marker carries its wall time and the minimum MemAvailable sampled at
    1 Hz on the host. A local leg obeys the idle rule: a loaded leg is
    discarded and re-run, and the IDLE_RETRIES-th loaded leg at one slots
    value ends the calibration. legs holds each host's accepted legs.
    """

    def __init__(self, run_id, local_run_dir, measure, plans, local_host, dry_run=False):
        """Bind the measured hosts, their plans and the local host (the builder)."""
        a = argparse.Namespace(game=CALIBRATION_GAME, tier="presentation", all=False,
                               all_tiers=False, hosts="", no_local=False, pull="divergent")
        super().__init__(a, run_id, local_run_dir)
        self.measure, self.plans, self.local_host = measure, plans, local_host
        self.dry_run = dry_run
        self.legs = {h.name: [] for h in measure}
        self.loaded = {}
        self.infos = {}
        self.fit = {}
        self.seq = 0

    # A leg's wall time is the measurement: a re-queued leg captures every row.
    resume = False
    # A leg's wall time is not loaded by per-row classification.
    stream = False

    def canary_on(self):
        """False: calibration legs capture no canary."""
        return False

    def local_legs(self):
        """True when a leg runs on the local host."""
        return self.local_host.name in self.plans

    def planned_slots(self, hs):
        """Return the slots values a host's plan may run, in order."""
        plan = self.plans[hs.name]
        if plan != "sweep":
            return [plan]
        vals = sweep_values(int(hs.info["nproc"]))
        kept = [v for v in vals if v <= self.fit.get(hs.name, v)] or [1]
        if kept != vals and not getattr(self, "cap_shown", False):
            self.cap_shown = True
            print(f"[fleet] {hs.name}: sweep capped at slots {kept[-1]}: MemAvailable fits "
                  f"{self.fit[hs.name]} slot(s) at 2 GiB each")
        return kept

    def prepare_hosts(self, src_bytes):
        """Preflight every measured host and the builder, take the local lock when a local leg runs, clean, ship."""
        check_recipe(self.tree)
        sel = list(self.measure)
        if not self.local_legs():
            sel.append(self.local_host)
        results = preflight(sel, self.need_bytes(src_bytes), require_calibration=False)
        for h, i, reasons, _slots in results:
            self.infos[h.name] = i
            if self.dry_run or (h.local and not self.local_legs()):
                reasons = [r for r in reasons if not r.startswith("golden lock held")]
            for r in reasons:
                print(f"[fleet] {h.name}: preflight: {r}")
            if reasons:
                raise FleetError(f"{h.name}: preflight failed", EXIT_SKIP)
            plan = self.plans.get(h.name)
            fit = int(i.get("memavail_kib") or 0) // MEM_PER_SLOT_KIB
            if h.local:
                self.fit[h.name] = fit  # this machine's sweep stops at 2 GiB MemAvailable per slot
            if h.local and isinstance(plan, int) and fit < plan:
                raise FleetError(f"{h.name}: MemAvailable fits {fit} slot(s) at 2 GiB each, "
                                 f"under the recorded slots {plan}", EXIT_SKIP)
            hs = HostState(h, i, h.cfg.get("slots", 1), self.run_id)
            if h in self.measure:
                self.hosts.append(hs)
            if h.local:
                self.local_hs = hs
        builders = [hs for hs in self.hosts + [self.local_hs] if hs.host.cfg.get("build", True)]
        if not builders:
            raise FleetError("no measured host and not the local host may build (build = true)",
                             EXIT_SKIP)
        self.build_hs = max(builders, key=lambda hs: (hs.host.local, hs.speed))
        if self.dry_run:
            return
        if self.local_legs():
            self.golden_fd = try_golden_lock(self.local_host.fleet_root)
            if self.golden_fd is None:
                raise FleetError(f"{self.local_host.name}: golden lock taken since preflight",
                                 EXIT_SKIP)
        for hs in self.hosts:
            rc = clean_host(hs.host, "stale", held=hs.host.local)
            if rc != 0:
                raise FleetError(f"{hs.name}: start-of-run clean exited {rc}", EXIT_SKIP)
        for hs in {id(h): h for h in self.hosts + [self.build_hs]}.values():
            ship_image(hs.host, hs.info)

    def leg_timeout_s(self, job, hs):
        """Return a leg's harness timeout: a fixed leg's scaled by the recorded speed, else CALIB_LEG_TIMEOUT_S."""
        if self.plans.get(hs.name) != "sweep" and "speed" in hs.host.cfg:
            return leg_timeout(hs.speed)
        return CALIB_LEG_TIMEOUT_S

    def leg_job(self, hs, slots):
        """Return one calibration leg on a host at slots."""
        self.seq += 1
        return Job("unit", CALIBRATION_GAME, "presentation", pinned=hs.name, slots=slots,
                   label=f"calib-{hs.name}-s{slots}-{self.seq}")

    def print_plan(self):
        """Print the build host and every host's planned legs (dry run)."""
        print(f"[fleet] build host {self.build_hs.name}; sync set and snapshot as `run`; "
              f"unit {CALIBRATION_GAME} presentation")
        for hs in self.hosts:
            for s in self.planned_slots(hs):
                job = Job("unit", CALIBRATION_GAME, "presentation", pinned=hs.name, slots=s)
                print(f"[fleet] {hs.name} slots {s}: CORPUS_JOBS={s} "
                      f"LP_NUM_THREADS={lp_threads(hs.info, s)} "
                      f"PRESENTATION_LEG_TIMEOUT={self.leg_timeout_s(job, hs)} "
                      f"tests/harness/check_presentation_golden.sh --game {CALIBRATION_GAME}, "
                      f"then --verdict-only; MemAvailable 1 Hz"
                      + ("; idle rule" if hs.host.local else ""))
            if self.plans[hs.name] == "sweep":
                print(f"[fleet] {hs.name}: the sweep stops at the first step improving under "
                      f"{IMPROVEMENT_FLOOR * 100:.0f}% or a leg under 2 GiB MemAvailable")

    def launch(self, job, hs, deadline_s):
        """Wait for the dev box to fall idle before a local leg, then launch as `run` does."""
        if hs.host.local:
            job.load_before = self.wait_idle()
        return super().launch(job, hs, deadline_s)

    @staticmethod
    def wait_idle():
        """Wait up to IDLE_WAIT_S for the 1-minute load average to reach the idle bound; return it."""
        bound, t0, load = idle_bound(os.cpu_count()), time.monotonic(), loadavg1()
        if load > bound:
            print(f"[fleet] local: load {load:.2f} over the idle bound {bound:.2f}; waiting up "
                  f"to {IDLE_WAIT_S} s")
        while load > bound and time.monotonic() - t0 < IDLE_WAIT_S:
            time.sleep(15)
            load = loadavg1()
        return load

    def check_engaged(self, job, hs):
        """Refuse a leg whose verdict report compared other than every blessed row or lists a missing tile."""
        dst = os.path.join(self.local_run_dir, "calib", job.name)
        rsync_pull(hs.host, f"{job.result['unit_dir']}/cand", dst, names=["verdict-report.txt"])
        path = os.path.join(dst, "verdict-report.txt")
        text = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        fields = parse_report(path)["fields"]
        rows = len(self.pb.load_manifest(CALIBRATION_GAME, "presentation").rows)
        missing = sum(1 for ln in text.splitlines() if ln.startswith("missing: "))
        if fields.get("compared") != str(rows) or missing:
            self.show_logs(job, hs)
            raise FleetError(f"{hs.name}: leg {job.name} compared {fields.get('compared') or 'none'} "
                             f"of {rows} blessed rows, {missing} missing; not a full unit")
        return fields.get("verdict", "")

    @staticmethod
    def show_logs(job, hs):
        """Print the tails of a failed leg's capture and verdict logs before teardown deletes them."""
        cp = hs.host.run_script('for f in "$1"/build.log "$1"/capture.log "$1"/verdict.log; do '
                                '[ -f "$f" ] && { echo "== $f"; tail -n 15 "$f"; }; done; exit 0',
                                [job.result.get("unit_dir", "")], timeout=60)
        for line in cp.stdout.splitlines():
            print(f"[fleet] {hs.name}: {line}")

    def leg_done(self, job, hs):
        """Accept or discard a finished leg; return the host's next leg, if any."""
        r = job.result
        if r["rc"] not in (0, 1) or r["status"] != "verdict":
            self.show_logs(job, hs)
            raise FleetError(f"{hs.name}: leg {job.name} ended rc {r['rc']} ({r['status']}); "
                             f"logs under {r.get('unit_dir', '?')} on {hs.name}")
        verdict = self.check_engaged(job, hs)
        leg = {"slots": job.slots, "wall_s": float(r["wall"]),
               "min_memavail_kib": r["min_memavail_kib"], "rc": r["rc"]}
        print(f"[fleet] {hs.name}: slots {job.slots}: {leg['wall_s']:.0f} s, min MemAvailable "
              f"{leg['min_memavail_kib'] / GIB_KIB:.2f} GiB, verdict {verdict}")
        if hs.host.local:
            leg.update(load_before=job.load_before, load_end=loadavg1())
            time.sleep(IDLE_SETTLE_S)
            leg["load_after"] = loadavg1()
            idle, why = leg_is_idle(leg, os.cpu_count())
            print(f"[fleet] {hs.name}: slots {job.slots}: {why}; "
                  + ("accepted" if idle else "loaded, discarded"))
            if not idle:
                key = (hs.name, job.slots)
                self.loaded[key] = self.loaded.get(key, 0) + 1
                if self.loaded[key] >= IDLE_RETRIES:
                    raise FleetError(f"{hs.name}: {IDLE_RETRIES} loaded legs at slots {job.slots}; "
                                     f"the machine is not idle", EXIT_SKIP)
                return [self.leg_job(hs, job.slots)]
        legs = self.legs[hs.name]
        legs.append(leg)
        if self.plans[hs.name] == "sweep" and not sweep_should_stop(legs):
            rest = [v for v in self.planned_slots(hs) if v > job.slots]
            if rest:
                return [self.leg_job(hs, rest[0])]
        return []

    def run(self):
        """Preflight, build and sync as `run`, then place every host's legs; return 0 or raise."""
        src_bytes = set_bytes(self.tree, source_set(self.tree, self.pb, [CALIBRATION_GAME]))
        self.prepare_hosts(src_bytes)
        if self.dry_run:
            self.print_plan()
            return 0
        self.build_and_sync()
        first = [self.leg_job(hs, self.planned_slots(hs)[0]) for hs in self.hosts]
        self.run_jobs(first, deadline=lambda _j, _hs: CALIB_DEADLINE_S, on_done=self.leg_done)
        for hs in self.hosts:
            if not self.legs[hs.name]:
                raise FleetError(f"{hs.name}: no accepted calibration leg "
                                 f"({hs.dropped or 'no result'})")
        return 0


def run_calibration(local_host, measure, plans, dry_run):
    """Run a Calibrator under drive() (directly under --dry-run, which writes nothing); return (rc, calibrator)."""
    run_id = new_run_id()
    run_dir = os.path.join(expand_root(local_host.fleet_root), "runs", run_id)
    make = lambda: Calibrator(run_id, run_dir, measure, plans, local_host, dry_run)  # noqa: E731
    if dry_run:
        co = make()
        return co.run(), co
    print(f"[fleet] calibration run {run_id}: {run_dir}")
    return drive(run_id, run_dir, make)


def check_recipe(tree):
    """Refuse (64) when the local capture image was built from another recipe than the tree's."""
    engine = local_engine()
    if not engine:
        raise FleetError("no local container engine", EXIT_SKIP)
    label = image_inspect(engine, local_image(),
                          '{{index .Labels "org.openpete.capture-recipe"}}')
    want = sha256_path(os.path.join(tree, "tests", "harness",
                                    "linux-capture.Dockerfile"))
    if label != want:
        raise FleetError(f"local image {local_image()} was built from recipe "
                         f"{label[:12] or 'none'}, the tree's is {want[:12]}; rebuild it with "
                         f"scripts/golden-capture-container.sh image", EXIT_SKIP)


def worst_rc(rcs):
    """Return the worst exit code in the order 1 > 64 > 0."""
    rcs = list(rcs)
    if any(r not in (0, EXIT_SKIP) for r in rcs):
        return 1
    return EXIT_SKIP if EXIT_SKIP in rcs or not rcs else 0


def first_hex(value):
    """Return the leading sha256 of a `<sha> <path>` report value, or ''."""
    tok = value.split()[0] if value.split() else ""
    return tok if re.fullmatch(r"[0-9a-f]{64}", tok) else ""


def repeat_row_text(line):
    """Return a report `repeat-row:` value as `<key>: D divergent of T legs (C compared)`."""
    words = line.split()
    kv = dict(w.split("=", 1) for w in words[1:] if "=" in w)
    extra = f", copies outside 1..N: {kv['outside']}" if "outside" in kv else ""
    return (f"{words[0] if words else '?'}: {kv.get('divergent', '?')} divergent of "
            f"{kv.get('of', '?')} legs ({kv.get('compared', '?')} compared{extra})")


def repeat_refusals(report, n):
    """Return the refusals of a repeat unit's report: its repeat count, and fewer tiles compared than N x legs (engagement)."""
    f = report["fields"]
    errs = []
    if f.get("repeat") != str(n):
        errs.append(f"report repeat {f.get('repeat') or 'absent'} differs from run --repeat {n}")
    legs, cmp_ = f.get("repeat-legs", ""), f.get("repeat-compared", "")
    if not (legs.isdigit() and cmp_.isdigit()) or int(legs) == 0:
        errs.append(f"repeat-legs {legs or 'absent'}, repeat-compared {cmp_ or 'absent'}: "
                    "the repeat compared nothing")
    elif int(cmp_) < int(legs):
        errs.append(f"repeat compared {cmp_} of {legs} legs (N x legs): the repeat did not engage every copy")
    if not report["repeat-rows"]:
        errs.append("no repeat-row line in the report")
    return errs


def git_head(tree):
    """Return the HEAD commit of a checkout, or None."""
    p = subprocess.run(["git", "-C", tree, "rev-parse", "HEAD"], capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else None


def drop_option(argv, valued, flag):
    """Return argv without a valued option (`--x V`, `--x=V`) and a bare flag."""
    out, skip = [], False
    for x in argv:
        if skip:
            skip = False
        elif x == valued:
            skip = True
        elif not x.startswith(valued + "=") and x != flag:
            out.append(x)
    return out


def query_hashes(d):
    """Return {file: sha256} of a queries directory, or None without one."""
    if not d:
        return None
    files = sorted(f for f in os.listdir(d) if os.path.isfile(os.path.join(d, f)))
    return {f: sha256_path(os.path.join(d, f)) for f in files}


def read_json_file(path):
    """Return a JSON file's object, or None when absent or unreadable."""
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def census_root(run_dir):
    """Return the census store of a local run directory's fleet_root (<fleet_root>/census)."""
    return os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(run_dir))), CENSUS_STORE)


def census_tree_refusal(local):
    """Return why a pulled unit's sidecars cannot enter the census store, or None.

    Every sidecar's header must name the capture record's source tree; a
    sidecar of `tree unrecorded` or of another tree refuses the entry.
    """
    try:
        tree = parse_kv(open(os.path.join(local, RECORD_NAME), encoding="utf-8").read()).get("source-tree", "")
    except OSError:
        return "no capture record"
    if not re.match(r"^[0-9a-f]{40}$", tree):
        return f"the capture record names no source tree ({tree or 'none'})"
    for p in sorted(glob.glob(os.path.join(local, SIDECAR_GLOB))):
        with open(p, encoding="utf-8") as f:
            head = f.readline().split()
        if len(head) != 5 or head[3] != "tree" or head[4] != tree:
            return f"{os.path.basename(p)} names tree {head[4] if len(head) == 5 else '?'}, the capture {tree[:12]}"
    return None


def census_stream_moved(run_dir, unit, attempt):
    """Return the sorted moved tiles (DIVERGE, EXTRA, EXCEED) of one unit's attempt in a run's stream.log."""
    moved = set()
    try:
        lines = open(os.path.join(run_dir, "stream.log"), encoding="utf-8").read().splitlines()
    except OSError:
        return []
    for ln in lines:
        f = ln.split()
        if len(f) >= 6 and f[0] == unit and f[2] == str(attempt) and f[4] in MOVED_VERBS:
            moved.add(f[5])
    return sorted(moved)


def census_store_finalize(run_id, run_dir, now=None):
    """Finalize every <run-id>.part of the census store once the run's done marker exists, then prune.

    A .part gets moved.txt (the accepted attempt's moved tiles), copies of
    run-args.json and done, and entry.json last, and is renamed to
    <run-id>/ (replacing an earlier finalize of the same run). Each unit
    keeps its CENSUS_KEEP newest finalized entries; a .part older than
    CENSUS_PART_MAX_AGE_S is removed. Returns the finalized entries.
    """
    store = census_root(run_dir)
    if not os.path.exists(os.path.join(run_dir, "done")) or not os.path.isdir(store):
        return []
    done = []
    now = time.time() if now is None else now
    for unit in sorted(os.listdir(store)):
        ud = os.path.join(store, unit)
        part = os.path.join(ud, run_id + ".part")
        if os.path.isdir(part):
            acc = read_json_file(os.path.join(part, "accept.json")) or {}
            with open(os.path.join(part, "moved.txt"), "w", encoding="utf-8") as f:
                f.writelines(t + "\n" for t in census_stream_moved(run_dir, unit, acc.get("attempt")))
            for n in ("run-args.json", "done"):
                shutil.copyfile(os.path.join(run_dir, n), os.path.join(part, n))
            entry = dict(acc, written=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
            with open(os.path.join(part, "entry.json.tmp"), "w", encoding="utf-8") as f:
                json.dump(entry, f)
            os.replace(os.path.join(part, "entry.json.tmp"), os.path.join(part, "entry.json"))
            final = os.path.join(ud, run_id)
            shutil.rmtree(final, ignore_errors=True)
            os.replace(part, final)
            done.append(final)
            print(f"[fleet] census store: {unit} entry {run_id} finalized ({entry.get('sidecars')} sidecar(s))")
        census_store_prune(ud, now)
    return done


def census_store_prune(ud, now):
    """Remove all but a unit's CENSUS_KEEP newest finalized entries, and each .part older than CENSUS_PART_MAX_AGE_S."""
    names = sorted(n for n in os.listdir(ud) if os.path.isdir(os.path.join(ud, n)))
    final = [n for n in names if not n.endswith(".part") and os.path.exists(os.path.join(ud, n, "entry.json"))]
    for n in final[:-CENSUS_KEEP]:
        shutil.rmtree(os.path.join(ud, n), ignore_errors=True)
        print(f"[fleet] census store: pruned {os.path.basename(ud)} entry {n}")
    for n in names:
        p = os.path.join(ud, n)
        if n.endswith(".part") and now - os.path.getmtime(p) > CENSUS_PART_MAX_AGE_S:
            shutil.rmtree(p, ignore_errors=True)
            print(f"[fleet] census store: removed unfinished {os.path.basename(ud)} {n}")


def census_pull_refusals(local):
    """Return a refusal per captured tile of a pulled unit (tile-digests.tsv) whose sidecar was not pulled."""
    path = os.path.join(local, DIGESTS_NAME)
    try:
        tiles = [ln.split("\t", 1)[0] for ln in open(path, encoding="utf-8")
                 if ln.strip() and not ln.startswith("#")]
    except OSError:
        return [f"pulled file absent: {DIGESTS_NAME}"]
    missing = [t for t in tiles if not os.path.exists(os.path.join(local, t[:-len(".png")] + ".census"))]
    return [f"{len(missing)} tile(s) without a sidecar, first {missing[0]}"] if missing else []


def repeat_sidecar_refusals(pb, local):
    """Return a refusal when the repeat copies of one tile wrote sidecars that differ, or none compared."""
    groups = {}
    for p in sorted(glob.glob(os.path.join(local, SIDECAR_GLOB))):
        origin, k = pb.copy_origin(os.path.basename(p))
        if k is not None:
            with open(p, "rb") as f:
                groups.setdefault(origin, set()).add(f.read())
    differ = sorted(o for o, b in groups.items() if len(b) > 1)
    if not groups:
        return ["repeat: no repeat copy's sidecar pulled (VACUOUS)"]
    print(f"[fleet] repeat sidecars: {len(groups)} tile(s), {len(differ)} with copies that differ")
    return [f"repeat: {len(differ)} tile(s) whose copies' sidecars differ, first {differ[0]}"] if differ else []


def merge_row_cost(local, job, hs, run_id):
    """Merge a unit's per-leg capture seconds into ROW_COST_CACHE: one line per game and row, the newest kept.

    A line: game, row key, seconds summed over the row's legs, host, slots,
    host speed, run id. The cache is uncommitted; it answers what a row costs
    to capture on which host.
    """
    rows = {}
    try:
        for ln in open(os.path.join(local, ROW_COST_NAME), encoding="utf-8"):
            f = ln.rstrip("\n").split("\t")
            if len(f) == 4 and re.match(r"^[0-9.]+$", f[3]):
                rows[f[1]] = rows.get(f[1], 0.0) + float(f[3])
    except OSError:
        return
    os.makedirs(os.path.dirname(ROW_COST_CACHE), exist_ok=True)
    with open(ROW_COST_CACHE + ".lock", "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        keep = {}
        if os.path.exists(ROW_COST_CACHE):
            for ln in open(ROW_COST_CACHE, encoding="utf-8"):
                f = ln.rstrip("\n").split("\t")
                if len(f) == 7:
                    keep[(f[0], f[1])] = f
        for key, sec in rows.items():
            keep[(job.game, key)] = [job.game, key, f"{sec:.1f}", hs.name, str(hs.slots),
                                     f"{hs.speed:.2f}", run_id]
        with open(ROW_COST_CACHE + ".tmp", "w", encoding="utf-8") as f:
            f.writelines("\t".join(v) + "\n" for _k, v in sorted(keep.items()))
        os.replace(ROW_COST_CACHE + ".tmp", ROW_COST_CACHE)


def parse_report(path):
    """Parse verdict-report.txt into fields, divergent, extra and missing tiles, identity-pair members, repeat-row and crashed-leg lines.

    A crashed-leg line is `<leg> rc=N <kept dir>[ retry=pass|fail]`; each
    becomes {leg, rc, retry} (retry None for a leg that was not retried).
    """
    out = {"fields": {}, "divergent": [], "extra": [], "missing": [], "identity": [], "repeat-rows": [],
           "crashed": []}
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return out
    for line in text.splitlines():
        key, _sep, val = line.partition(": ")
        if key in ("divergent", "extra", "missing") and val.split():
            out[key].append(val.split()[0])
        elif key.startswith("identity-"):
            out["identity"] += [t for t in val.split()[:2] if t.endswith(".png")]
        elif key == "repeat-row":
            out["repeat-rows"].append(val.strip())
        elif key == "crashed-leg" and val.split():
            words = val.split()
            retry = next((w[len("retry="):] for w in words[2:] if w.startswith("retry=")), None)
            out["crashed"].append({"leg": words[0], "rc": words[1][len("rc="):] if len(words) > 1 else "?",
                                   "retry": retry})
        elif key:
            out["fields"].setdefault(key, val.strip())
    out["identity"] = sorted(set(out["identity"]))
    return out


def crashed_text(crashed):
    """Return `crashed=N (k retried pass, ...)` over a report's crashed legs, '' when none.

    A `<leg>.retry` line is the retry attempt of a counted leg, not a leg of
    its own.
    """
    legs = [c for c in crashed if not c["leg"].endswith(".retry")]
    if not legs:
        return ""
    counts = {}
    for c in legs:
        k = f"retried {c['retry']}" if c["retry"] else "not retried"
        counts[k] = counts.get(k, 0) + 1
    return f"crashed={len(legs)} (" + ", ".join(f"{n} {k}" for k, n in sorted(counts.items())) + ")"


# ---- run: the stream, its unit-end check and expected moves ---------------------

def parse_poll(out):
    """Parse POLL_SCRIPT's framed bytes into {base: [state text, stream bytes]}; ValueError when torn."""
    res, i = {}, 0
    while i < len(out):
        nl = out.index(b"\n", i)
        tag, base, rest = (out[i:nl].decode(errors="replace").split(" ", 2) + ["", ""])[:3]
        i = nl + 1
        entry = res.setdefault(base, ["", b""])
        if tag == "state":
            entry[0] = rest
        elif tag == "stream" and rest.isdigit():
            entry[1] = out[i:i + int(rest)]
            if len(entry[1]) != int(rest):
                raise ValueError(f"stream section of {base} short: {len(entry[1])} of {rest} bytes")
            i += int(rest)
        else:
            raise ValueError(f"unframed poll line {out[i:nl][:80]!r}")
    return res


def parse_stream_line(line):
    """Parse one stream line into {verb, tile, detail, epoch, imported}, or None when malformed.

    Tile verbs carry `<tile> <detail> <epoch>`, CREDITS `rc=N <epoch>` (tile
    `credits`), CRASHED `<leg> rc=N retry=pass|fail <epoch>` (tile the leg,
    detail `rc=N retry=X`), END `<epoch>` (tile `END`); a trailing `imported`
    marks a line of a row the attempt imported.
    """
    f = line.split()
    if not f:
        return None
    if f[0] == "END":
        tile, detail, rest = "END", "", f[1:]
    elif f[0] == "CREDITS" and len(f) >= 2 and f[1].startswith("rc="):
        tile, detail, rest = "credits", f[1], f[2:]
    elif (f[0] == "CRASHED" and len(f) >= 4 and f[2].startswith("rc=")
          and f[3] in ("retry=pass", "retry=fail")):
        tile, detail, rest = f[1], f"{f[2]} {f[3]}", f[4:]
    elif f[0] in TILE_VERBS and len(f) >= 4:
        tile, detail, rest = f[1], f[2], f[3:]
    else:
        return None
    try:
        epoch = float(rest[0]) if rest else None
    except ValueError:
        return None
    return {"verb": f[0], "tile": tile, "detail": detail, "epoch": epoch,
            "imported": "imported" in rest[1:]}


def stream_key(rec):
    """The fields two attempts' lines of one tile share: verb, tile and detail."""
    return (rec["verb"], rec["tile"], rec["detail"])


def stream_sets(lines):
    """Return ({verb: set of tiles}, credits rc text or None) over stream records."""
    sets = {v: set() for v in TILE_VERBS}
    credits = None
    for r in lines:
        if r["verb"] in sets:
            sets[r["verb"]].add(r["tile"])
        elif r["verb"] == "CREDITS":
            credits = r["detail"][len("rc="):]
    return sets, credits


def report_sets(path):
    """Return the verdict report's compared count, credits-rc and divergent, extra and identity-exceed sets.

    identity-exceed holds the identity member of each EXCEED and NOCANON
    pair, the two the stream spells EXCEED.
    """
    rep = {"compared": None, "credits-rc": None, "divergent": set(), "extra": set(),
           "identity-exceed": set()}
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return rep
    for line in text.splitlines():
        key, _sep, val = line.partition(": ")
        words = val.split()
        if key in ("compared", "credits-rc"):
            rep[key] = val.strip()
        elif key in ("divergent", "extra") and words:
            rep[key].add(words[0])
        elif key in ("identity-exceed", "identity-nocanon") and words:
            rep["identity-exceed"].add(words[0])
    return rep


def stream_mismatches(lines, report_path):
    """Return the differences between an attempt's stream and its verdict report (empty when equal).

    SAME + DIVERGE equals the report's compared count; the DIVERGE, EXTRA
    and EXCEED tile sets equal its divergent, extra and identity-exceed
    sets; the CREDITS rc equals its credits-rc, an absent CREDITS line
    standing for 64 (a tier without the credits leg).
    """
    sets, credits = stream_sets(lines)
    rep = report_sets(report_path)
    out = []
    n = len(sets["SAME"] | sets["DIVERGE"])
    if str(n) != str(rep["compared"]):
        out.append(f"SAME + DIVERGE = {n}, the report compared {rep['compared']}")
    for verb, key in (("DIVERGE", "divergent"), ("EXTRA", "extra"),
                      ("EXCEED", "identity-exceed")):
        got, want = sets[verb], rep[key]
        if got != want:
            out.append(f"{verb} {len(got)} tile(s), the report's {key} {len(want)}; "
                       f"streamed only: {' '.join(sorted(got - want)[:3]) or '-'}; "
                       f"reported only: {' '.join(sorted(want - got)[:3]) or '-'}")
    got_rc = credits if credits is not None else "64"
    if got_rc != rep["credits-rc"]:
        out.append(f"CREDITS rc={got_rc}, the report's credits-rc {rep['credits-rc']}")
    return out


def stream_absent(lines, dest):
    """Return one mismatch per moved verb whose streamed tiles are absent from the unit's stream directory.

    A DIVERGE or EXTRA tile is at dest's top level, where promote reads it;
    both members of an EXCEED pair are under dest/identity/ or at the top
    level (a --pull all candidate). A tile only under stream-only/ counts as
    absent: the accepted pull did not hold it.
    """
    sets, _credits = stream_sets(lines)
    out = []
    for verb in MOVED_VERBS:
        absent = []
        for t in sorted(sets[verb]):
            names = (t, t.replace("__ident__", "__canon__")) if verb == "EXCEED" else (t,)
            dirs = (dest, os.path.join(dest, "identity")) if verb == "EXCEED" else (dest,)
            absent += [n for n in names if not any(os.path.isfile(os.path.join(d, n)) for d in dirs)]
        if absent:
            out.append(f"{verb} {len(absent)} tile(s) absent from {dest}: "
                       f"{' '.join(absent[:3])}{' ...' if len(absent) > 3 else ''}")
    return out


LAUNCH_LOD_EXT = re.compile(r"(?:^|\s)(?:--lod(?:\s+|=)extended|--no-lod)(?=\s|$)")


class Expectation:
    """One line of an expected-moves file: a unit (job name), a tile substring and a reason."""

    def __init__(self, unit, sub, reason, lineno):
        """Bind the line's fields; ext_rows (bind_launch_lod) starts empty."""
        self.unit, self.sub, self.reason, self.lineno = unit, sub, reason, lineno
        self.ext_rows = frozenset()

    def hit(self, tile):
        """True when the substring occurs in the tile name, its launch-LOD name (launch_lod_name) or either's base-row name (variant_base_name)."""
        names = {tile, launch_lod_name(tile, self.ext_rows)}
        names |= {b for b in map(variant_base_name, names) if b}
        return any(self.sub in n for n in names)

    def via_variant(self, tile):
        """True when hit() holds for the tile only through its base-row name: a `@<variant>` tile covered by a line naming the base row."""
        names = {tile, launch_lod_name(tile, self.ext_rows)}
        return not any(self.sub in n for n in names) and self.hit(tile)

    def matches(self, unit, tile):
        """True when the expectation names the unit and hit() holds for the tile."""
        return unit == self.unit and self.hit(tile)


VARIANT_KEY_RE = re.compile(r"^(?P<base>[^@]+)@[^_]*?(?P<pose>-[0-9a-f]{6})?$")


def variant_base_name(tile):
    """Return the tile name of a `@<variant>` row's tile under its base row's key, or None for a tile of no variant row.

    A bk2 variant `<row>@<variant>` keys `<row>`; a freecam variant
    `<level>@<variant>-<pose hash>` (pg_pose_slug) keys `<level>-<pose hash>`,
    the plain row at the same pose. A repeat copy's `~r<k>` suffix is kept.
    """
    key, sep, rest = tile.partition("__")
    m = re.match(r"^(.*?)(~r\d+)?$", key)
    core, copy = m.group(1), m.group(2) or ""
    v = VARIANT_KEY_RE.match(core)
    if not v:
        return None
    return f"{v.group('base')}{v.group('pose') or ''}{copy}{sep}{rest}"


def launch_lod_name(tile, ext_rows):
    """Return the tile name with the `lodext__` tag its capture LOD implies, or the name unchanged.

    presentation-corpus.sh tags a tile `<key>__lodext__<leg>__<n>` only on the
    leg it runs at `--lod extended`; a row whose launch field sets extended LOD
    itself (ext_rows) runs its untagged `original` leg at extended LOD too, the
    row's arguments going last on the command line. The tile key is the row
    name or a repeat copy `<row>~r<k>`.
    """
    key, sep, rest = tile.partition("__")
    if not sep or rest.startswith("lodext__") or re.sub(r"~r\d+$", "", key) not in ext_rows:
        return tile
    return f"{key}__lodext__{rest}"


def launch_ext_rows(tree, game, tier):
    """The bk2 rows of a unit's presentation manifest whose launch field selects extended LOD."""
    if tier == "dragons":
        return frozenset()
    try:
        text = open(os.path.join(tree, "games", game, "corpus", "presentation-corpus.manifest"),
                    encoding="utf-8").read()
    except OSError:
        return frozenset()
    out = set()
    for line in text.splitlines():
        f = line.split("|")
        if len(f) > 5 and not f[0].lstrip().startswith("#") and LAUNCH_LOD_EXT.search(f[5]):
            out.add(f[0].strip())
    return frozenset(out)


def bind_launch_lod(tree, exps):
    """Give each expectation its unit's extended-launch rows (launch_ext_rows); return exps."""
    for e in exps:
        game, _, tier = e.unit.rpartition("-")
        e.ext_rows = launch_ext_rows(tree, game, tier)
    return exps


def parse_expect(path):
    """Read an expected-moves file: `<unit> <substring> <reason>` per line, `#` comments; exit 2 on a malformed line."""
    try:
        text = open(path, encoding="utf-8").read()
    except OSError as e:
        raise FleetError(f"--expect {path}: {e.strerror}", 2)
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        f = line.split(None, 2)
        if len(f) < 3:
            raise FleetError(f"--expect {path}:{n}: expected `<unit> <substring> <reason>`", 2)
        out.append(Expectation(f[0], f[1], f[2].strip(), n))
    return out


def corpus_row_names(tree, game, tier):
    """The name field of every row of a unit's corpus manifests (bk2 rows and freecam levels)."""
    files = (["dragon-rescue-corpus.manifest"] if tier == "dragons"
             else ["presentation-corpus.manifest", "freecam-corpus.manifest"])
    names = set()
    for f in files:
        try:
            text = open(os.path.join(tree, "games", game, "corpus", f), encoding="utf-8").read()
        except OSError:
            continue
        for line in text.splitlines():
            name = line.split("|", 1)[0].strip()
            if name and not name.startswith("#"):
                names.add(name)
    return names


def check_expect(exps, unit_names, universe):
    """Print each expectation's match count; return the refusals (an unknown unit, a substring matching nothing).

    universe maps a unit's job name to (blessed tile names, manifest row names).
    """
    errs = []
    for e in exps:
        if e.unit not in unit_names:
            errs.append(f"line {e.lineno}: unit {e.unit} is not a unit of this run "
                        f"({', '.join(sorted(unit_names))})")
            continue
        tiles, rows = universe[e.unit]
        nt = sum(1 for t in tiles if e.hit(t))
        nr = sum(1 for r in rows if e.sub in r)
        variants = sorted({t.partition("__")[0] for t in tiles if e.via_variant(t)})
        print(f"[fleet] expect: {e.unit} '{e.sub}': {nt} tile(s), {nr} manifest row(s) match"
              + (f"; covers variant row(s) {', '.join(variants)}" if variants else ""))
        if not nt and not nr:
            errs.append(f"line {e.lineno}: '{e.sub}' matches no tile of {e.unit}'s blessed.tsv "
                        f"and no manifest row")
    return errs


def expect_refusals(tree, pb, units, exps):
    """Check an expected-moves file against a run's units (job names `<game>-<tier>`); return the refusals."""
    universe = {}
    for game, tier in units:
        tiles = {r["tile"] for r in pb.load_manifest(game, tier).rows}
        # Each blessed canon tile's identity twin: identity tiles have no
        # blessed row, and their EXCEED lines name them.
        tiles |= {t.replace("__canon__", "__ident__", 1) for t in tiles if "__canon__" in t}
        universe[f"{game}-{tier}"] = (tiles, corpus_row_names(tree, game, tier))
    return check_expect(exps, set(universe), universe)


def expect_tag(exps, unit, rec):
    """EXPECTED or UNEXPECTED for a moved tile's line, '' for any other line."""
    if rec["verb"] not in MOVED_VERBS:
        return ""
    return "EXPECTED" if any(e.matches(unit, rec["tile"]) for e in exps) else "UNEXPECTED"


def expect_status(e, reached, lines, blessed):
    """Return (status, matching tiles) of one expectation over its unit's accepted attempt.

    moved when a matching tile streamed a DIVERGE, EXTRA or EXCEED line;
    EXPECTED-NOT-MOVED when every matching tile (of blessed.tsv and of the
    stream) streamed and none moved; NOT-REACHED otherwise, and always for a
    unit with no accepted attempt (stopped, SKIP).
    """
    streamed = {r["tile"]: r["verb"] for r in lines if r["verb"] in TILE_VERBS}
    match = {t for t in blessed if e.hit(t)} | {t for t in streamed if e.hit(t)}
    if not reached:
        return "NOT-REACHED", match
    if any(streamed.get(t) in MOVED_VERBS for t in match):
        return "moved", match
    if match and all(t in streamed for t in match):
        return "EXPECTED-NOT-MOVED", match
    return "NOT-REACHED", match


def expect_counts(exps, units):
    """Count EXPECTED, UNEXPECTED and EXPECTED-NOT-MOVED tiles over each unit's accepted lines.

    units maps a job name to (reached, accepted attempt's records, blessed
    tile names). Returns (counts, [(expectation, status, matching tiles)]).
    """
    counts = {"EXPECTED": 0, "UNEXPECTED": 0, "EXPECTED-NOT-MOVED": 0}
    for name, (_reached, lines, _blessed) in units.items():
        moved = {r["tile"] for r in lines if r["verb"] in MOVED_VERBS}
        for t in moved:
            hit = any(e.matches(name, t) for e in exps)
            counts["EXPECTED" if hit else "UNEXPECTED"] += 1
    rows, quiet = [], set()
    for e in exps:
        reached, lines, blessed = units.get(e.unit, (False, [], set()))
        status, match = expect_status(e, reached, lines, blessed)
        rows.append((e, status, match))
        if status == "EXPECTED-NOT-MOVED":
            quiet |= {(e.unit, t) for t in match}
    counts["EXPECTED-NOT-MOVED"] = len(quiet)
    return counts, rows


def run_keep(name):
    """True for a file of the coordinator's local run directory that outlives the run (RUN_KEEP, RUN_KEEP_PREFIX)."""
    return name in RUN_KEEP or name.startswith(RUN_KEEP_PREFIX)


def read_list(path):
    """Read a NUL-separated --files-from list."""
    with open(path, "rb") as f:
        return [x.decode() for x in f.read().split(b"\0") if x]


def stamp_files(cand):
    """Return a local candidate directory's row stamps (resume/<kind>.<key>.stamp) and the tile files they name, cand-relative."""
    out = []
    for s in sorted(glob.glob(os.path.join(cand, "resume", "*.stamp"))):
        out.append(os.path.relpath(s, cand))
        with open(s, encoding="utf-8", errors="replace") as f:
            for line in f:
                parts = line.split()
                if len(parts) == 3 and parts[0] == "tile:" and "/" not in parts[2] \
                        and not parts[2].startswith("."):
                    out.append(parts[2])
    return out


def row_counts(files):
    """Return (rows, -vk.png tiles) of a stamp_files list."""
    return (sum(1 for f in files if f.startswith("resume/")),
            sum(1 for f in files if f.endswith("-vk.png")))


def read_move_requests(path):
    """Return the move request's lines as dicts with id, unit and to, in order; malformed lines are skipped."""
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return []
    out = []
    for line in text.splitlines():
        kv = dict(w.split("=", 1) for w in line.split() if "=" in w)
        if {"id", "unit", "to"} <= kv.keys():
            out.append(kv)
    return out


def split_unit(name):
    """Return the (game, tier) of a unit job name `<game>-<tier>`, or raise."""
    game, _, tier = name.rpartition("-")
    if tier not in TIER_GAMES or game not in TIER_GAMES[tier]:
        raise FleetError(f"{name}: not a unit name (<game>-<tier>, e.g. spyro-1-presentation)", 2)
    return game, tier


RESULT_RE = re.compile(r"^\[fleet\] result (\S+): (\w+) on (\S+) \(rc (-?\d+), attempt [^,]*, "
                       r"[^;]*; ([^;)]*)")
STARTED_RE = re.compile(r"^\[fleet\] (\S+): (\S+) attempt \d+ started")


def run_logs(run_dir):
    """Return a run's coordinator logs in order: the run's own, then each resume's."""
    resumes = sorted(glob.glob(os.path.join(run_dir, "coordinator-resume-*.log")),
                     key=lambda p: int(re.sub(r"\D", "", os.path.basename(p)) or 0))
    return [p for p in [os.path.join(run_dir, "coordinator.log"), *resumes] if os.path.exists(p)]


def unit_results(run_dir):
    """Return {job name: (verdict, host, rc, status)} from a run's logs, the latest result line of each job winning."""
    out = {}
    for path in run_logs(run_dir):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = RESULT_RE.match(line)
                if m:
                    out[m.group(1)] = (m.group(2), m.group(3), int(m.group(4)), m.group(5).strip())
    return out


def unit_hosts(run_dir, name):
    """Return the hosts a job's attempts started on, from a run's logs, in order."""
    out = []
    for path in run_logs(run_dir):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = STARTED_RE.match(line)
                if m and m.group(2) == name:
                    out.append(m.group(1))
    return out


def resolve_saved(src, unit, run_dir):
    """Locate a stopped unit's saved rows and the run's ship, snapshot and canary tiles under src.

    Accepted layouts: src is the unit directory itself (cand/ inside it) or
    holds u-<unit>/ (the `kept` layout); ship/, tree/ and canary/ are looked
    up in src, then beside it, then (canary) in the run directory. Returns a
    dict with cand, fields, binaries, tree, canary and host (the unit
    directory's `host` file, the host that captured the rows; '' when absent).
    """
    game, _tier = split_unit(unit)
    unit_dir = next((d for d in (os.path.join(src, f"u-{unit}"), src)
                     if os.path.isdir(os.path.join(d, "cand"))), None)
    if unit_dir is None:
        raise FleetError(f"no saved candidate directory of {unit} in {src} "
                         f"(looked for {src}/u-{unit}/cand and {src}/cand)", 2)
    near = [src, os.path.dirname(os.path.abspath(src)), unit_dir]

    def find(rel):
        return next((os.path.join(d, rel) for d in near if os.path.exists(os.path.join(d, rel))), "")
    fields = find(f"ship/fields-{game}.txt") or find("fields.txt")
    if not fields:
        raise FleetError(f"no fields file of {unit} (ship/fields-{game}.txt or fields.txt) "
                         f"near {src}: the rows' tree identity is unknown", 2)
    canary = find("canary") or (os.path.join(run_dir, "canary")
                                if os.path.isdir(os.path.join(run_dir, "canary")) else "")
    host = ""
    if os.path.exists(os.path.join(unit_dir, "host")):
        host = open(os.path.join(unit_dir, "host"), encoding="utf-8").read().strip()
    return {"cand": os.path.join(unit_dir, "cand"), "fields": fields, "host": host,
            "binaries": find("ship/binaries.sha256"), "tree": find("tree"), "canary": canary}


def read_stop_request(path):
    """Return the stop request's targets: {'all'} and/or job names; empty without a request."""
    try:
        text = open(path, encoding="utf-8").read()
    except OSError:
        return set()
    out = set()
    for line in text.splitlines():
        line = line.strip()
        if line == "all":
            out.add("all")
        elif line.startswith("unit="):
            out.add(line[len("unit="):])
    return out


# ---- run: entry points --------------------------------------------------------

def local_fleet_root():
    """Return the local host's expanded fleet_root (the default without a local host)."""
    try:
        _p, hosts = load_hosts()
    except FleetError:
        hosts = {}
    root = next((c.get("fleet_root", DEFAULT_FLEET_ROOT) for c in hosts.values()
                 if c["transport"] == "local"), DEFAULT_FLEET_ROOT)
    return expand_root(root)


def new_run_id():
    """Return a run id: UTC time plus four random hex digits."""
    return time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "-" + os.urandom(2).hex()


def raise_interrupted(signum, _frame):
    """Turn a termination signal into an exception so teardown runs."""
    raise Interrupted(f"interrupted by signal {signum}", 1)


def drive(run_id, run_dir, factory):
    """Run factory().run() under the run directory's active lock; teardown and done marker on every exit.

    Returns (exit code, coordinator or None). SIGTERM, SIGINT and SIGHUP
    become Interrupted so teardown stops in-flight attempts and deletes the
    run's directories; the done marker's first line is `rc=N`. After the
    marker the run's census-store entries are finalized
    (census_store_finalize).
    """
    os.makedirs(run_dir, exist_ok=True)
    active = os.open(os.path.join(run_dir, "active.lock"), os.O_WRONLY | os.O_CREAT, 0o644)
    fcntl.flock(active, fcntl.LOCK_SH)
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, raise_interrupted)
    rc, why, co = 1, "", None
    try:
        co = factory()
        rc = co.run()
        why = co.done_reason
    except FleetError as e:
        rc, why = e.code, str(e)
        print(f"[fleet] {'SKIP' if rc == EXIT_SKIP else 'FAIL'}: {why}")
    except Exception as e:  # the done marker is written whatever ends the run
        rc, why = 1, f"{type(e).__name__}: {e}"
        import traceback
        traceback.print_exc()
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        if co is not None:
            co.teardown()
        with open(os.path.join(run_dir, "done.tmp"), "w") as f:
            f.write(f"rc={rc}\nrun={run_id}\n" + (f"reason={why}\n" if why else ""))
        os.replace(os.path.join(run_dir, "done.tmp"), os.path.join(run_dir, "done"))
        try:
            census_store_finalize(run_id, run_dir)
        except OSError as e:  # the store never fails the run
            print(f"[fleet] census store: finalize failed: {e}")
        os.close(active)
    return rc, co


def cmd_stop(a):
    """Request a stop of a detached run's units (one with --unit, else all); refused once the run is done."""
    run_dir = os.path.join(local_fleet_root(), "runs", a.run_id)
    if not os.path.isdir(run_dir):
        raise FleetError(f"no run {a.run_id} under {os.path.dirname(run_dir)}", 2)
    if os.path.exists(os.path.join(run_dir, "done")):
        raise FleetError(f"run {a.run_id} is done; nothing to stop", 1)
    path = os.path.join(run_dir, "stop-request")
    want = read_stop_request(path) | {a.unit or "all"}
    with open(path + ".tmp", "w") as f:
        f.write("".join(("all" if w == "all" else f"unit={w}") + "\n" for w in sorted(want)))
    os.replace(path + ".tmp", path)
    print(f"[fleet] run {a.run_id}: stop requested for {a.unit or 'every running and pending unit'}; "
          f"the coordinator acts within {POLL_S} s ({STOP_POLL_S:.0f} s before the legs)")
    return 0


MOVE_WAIT_S = 4 * 3600       # longest wait of `move` for the coordinator's outcome


def cmd_move(a):
    """Request that a running run move one unit to a host; wait for the coordinator's outcome (exit 0 accepted, 1 refused)."""
    run_dir = os.path.join(local_fleet_root(), "runs", a.run_id)
    if not os.path.isdir(run_dir):
        raise FleetError(f"no run {a.run_id} under {os.path.dirname(run_dir)}", 2)
    if os.path.exists(os.path.join(run_dir, "done")):
        raise FleetError(f"run {a.run_id} is done; a unit it stopped is finished with "
                         f"`resume {a.run_id} --unit U --to HOST`", 1)
    split_unit(a.unit)
    _p, hosts = load_hosts()
    if a.to not in hosts:
        raise FleetError(f"no host {a.to!r} in the host file (have: {', '.join(hosts)})", 2)
    rid = os.urandom(3).hex()
    path = os.path.join(run_dir, "move-request")
    with open(path + ".lock", "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else ""
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            f.write(old + f"id={rid} unit={a.unit} to={a.to}\n")
        os.replace(path + ".tmp", path)
    print(f"[fleet] run {a.run_id}: move {rid} of {a.unit} to {a.to} requested; the coordinator "
          f"acts within {POLL_S} s once the legs run (a host joining the run syncs first)")
    if a.no_wait:
        return 0
    log = os.path.join(run_dir, "moves.log")
    t0 = time.monotonic()
    while time.monotonic() - t0 < MOVE_WAIT_S:
        if os.path.exists(log):
            for line in open(log, encoding="utf-8", errors="replace"):
                if line.startswith(f"id={rid} "):
                    verdict, _, text = line[len(f"id={rid} "):].strip().partition(" ")
                    print(f"[fleet] move {rid}: {verdict}: {text}")
                    return 0 if verdict == "accepted" else 1
        if os.path.exists(os.path.join(run_dir, "done")):
            raise FleetError(f"run {a.run_id} ended before acting on move {rid}", 1)
        time.sleep(2)
    raise FleetError(f"no outcome of move {rid} after {MOVE_WAIT_S} s; see {log}", 1)


def cmd_resume(a):
    """Finish a stopped unit of a run whose coordinator has exited, on one host, from its saved rows; detached like run (--wait follows)."""
    run_dir = os.path.join(local_fleet_root(), "runs", a.run_id)
    if a.foreground:
        rc, _co = drive(a.run_id, run_dir, lambda: Resumer(a, a.run_id, run_dir))
        return rc
    if not os.path.isdir(run_dir):
        raise FleetError(f"no run {a.run_id} under {os.path.dirname(run_dir)}", 2)
    split_unit(a.unit)
    done = os.path.join(run_dir, "done")
    if not os.path.exists(done):
        raise FleetError(f"run {a.run_id} has no done marker: its coordinator is running; "
                         f"use `move {a.run_id} --unit {a.unit} --to {a.to}`", 1)
    active = os.open(os.path.join(run_dir, "active.lock"), os.O_WRONLY | os.O_CREAT, 0o644)
    try:
        fcntl.flock(active, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise FleetError(f"run {a.run_id} is held by a coordinator (active.lock)", 1)
    finally:
        os.close(active)
    _p, hosts = load_hosts()
    if a.to not in hosts:
        raise FleetError(f"no host {a.to!r} in the host file (have: {', '.join(hosts)})", 2)
    last = unit_results(run_dir).get(a.unit)
    if last is None:
        raise FleetError(f"run {a.run_id} logged no result of {a.unit}", 2)
    if last[3] == "verdict":
        raise FleetError(f"{a.unit} has finished: {last[0]} on {last[1]} (rc {last[2]}); "
                         f"nothing to resume", 1)
    saved = resolve_saved(a.from_dir or os.path.join(run_dir, "kept"), a.unit, run_dir)
    rows, png = row_counts(stamp_files(saved["cand"]))
    n = len(glob.glob(os.path.join(run_dir, "coordinator-resume-*.log"))) + 1
    os.replace(done, f"{done}.{n}")
    stop = os.path.join(run_dir, "stop-request")
    if os.path.exists(stop):
        os.replace(stop, f"{stop}.{n}")
    log = os.path.join(run_dir, f"coordinator-resume-{n}.log")
    argv = [sys.executable, os.path.abspath(__file__), "resume", "--foreground", a.run_id,
            "--unit", a.unit, "--to", a.to, "--pull", a.pull]
    if a.from_dir:
        argv += ["--from", os.path.abspath(a.from_dir)]
    for flag, val in (("--filter", a.filter), ("--freecam-filter", a.freecam_filter)):
        if val is not None:
            argv.append(f"{flag}={val}")
    with open(log, "w") as f:
        subprocess.Popen(argv, cwd=script_repo(), stdout=f, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    print(f"[fleet] run {a.run_id}: resume {n} of {a.unit} on {a.to} detached ({rows} saved "
          f"row(s), {png} tile(s); last result {last[0]}, {last[3]})\n  log:  {log}\n  done: {done}")
    if not a.wait:
        return 0
    while not os.path.exists(done):
        time.sleep(5)
    sys.stdout.write(open(log, encoding="utf-8", errors="replace").read())
    return int(parse_kv(open(done).read()).get("rc", 1))


def run_foreground(a):
    """Run the coordinator in this process under drive()."""
    run_dir = os.path.join(local_fleet_root(), "runs", a.run_id)
    rc, _co = drive(a.run_id, run_dir, lambda: Coordinator(a, a.run_id, run_dir))
    return rc


def cmd_run(a):
    """Detach a coordinator (setsid) and print its log and done marker; --wait follows it."""
    if a.foreground:
        return run_foreground(a)
    units = select_units(a)
    load_hosts()
    exps = parse_expect(a.expect) if a.expect else []
    if exps:
        tree = script_repo()
        bind_launch_lod(tree, exps)
        errs = expect_refusals(tree, load_pb(tree), units, exps)
        if errs:
            raise FleetError("expected-moves file refused: " + "; ".join(errs), 2)
    if any(re.search(r"\s", f or "") for f in (a.filter, a.freecam_filter)):
        raise FleetError("--filter and --freecam-filter take a substring without whitespace", 2)
    if a.repeat is not None and (a.repeat < 1 or not (a.filter or a.freecam_filter)):
        raise FleetError("--repeat takes N >= 1 and needs --filter or --freecam-filter", 2)
    if a.queries and not os.path.isdir(a.queries):
        raise FleetError(f"--queries {a.queries}: no such directory", 2)
    a.run_id = new_run_id()
    run_dir = os.path.join(local_fleet_root(), "runs", a.run_id)
    os.makedirs(run_dir)
    if a.expect:
        shutil.copyfile(a.expect, os.path.join(run_dir, "expect.txt"))
    log = os.path.join(run_dir, "coordinator.log")
    argv = [sys.executable, os.path.abspath(__file__), "run", "--foreground",
            "--run-id", a.run_id, *drop_option(sys.argv[2:], "--queries", "--wait")]
    if a.queries:
        # The coordinator reads the run directory's copy, which outlives the scratchpad.
        qcopy = os.path.join(run_dir, "queries")
        shutil.copytree(a.queries, qcopy)
        argv += ["--queries", qcopy]
    with open(log, "w") as f:
        subprocess.Popen(argv, cwd=script_repo(), stdout=f, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, start_new_session=True)
    done = os.path.join(run_dir, "done")
    print(f"[fleet] run {a.run_id} detached\n  log:  {log}\n  done: {done}")
    if not a.wait:
        return 0
    while not os.path.exists(done):
        time.sleep(5)
    sys.stdout.write(open(log, encoding="utf-8", errors="replace").read())
    return int(parse_kv(open(done).read()).get("rc", 1))


def main(argv=None):
    """Parse the command line and dispatch to a subcommand."""
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("status")
    p.add_argument("--hosts", default="")
    p.set_defaults(fn=cmd_status)
    p = sub.add_parser("preflight")
    p.add_argument("--hosts", default="")
    p.add_argument("--need-gib", type=float, default=0.0)
    p.add_argument("--no-calibration", action="store_true",
                   help="do not drop an uncalibrated host")
    p.set_defaults(fn=cmd_preflight)
    p = sub.add_parser("ship-image")
    p.add_argument("--hosts", required=True)
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(fn=cmd_ship_image)
    p = sub.add_parser("add")
    p.add_argument("name")
    p.add_argument("--ssh")
    p.add_argument("--engine", choices=("podman", "docker"))
    p.add_argument("--fleet-root")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--timings")
    p.set_defaults(fn=cmd_add)
    p = sub.add_parser("calibrate")
    p.add_argument("--hosts", default="")
    p.add_argument("--dry-run", action="store_true")
    p.set_defaults(fn=cmd_calibrate)
    p = sub.add_parser("run")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--game", default="spyro-1", choices=GAMES)
    g.add_argument("--all", action="store_true", help="every game")
    t = p.add_mutually_exclusive_group()
    t.add_argument("--tier", default="presentation", choices=tuple(TIER_GAMES))
    t.add_argument("--all-tiers", action="store_true", help="presentation and dragons")
    h = p.add_mutually_exclusive_group()
    h.add_argument("--hosts", default="")
    h.add_argument("--no-local", action="store_true", help="leave the local host out")
    p.add_argument("--local-anyway", action="store_true",
                   help="keep the local host while a landing, the scheduler gate or a game process "
                        "outside the fleet runs here, or memory is low (default: left out, printed)")
    w = p.add_mutually_exclusive_group()
    w.add_argument("--host-wait", type=int, default=HOST_WAIT_S, metavar="S",
                   help="while every host is held (another run's golden lock, a busy local host), "
                        f"wait up to S seconds for one (default {HOST_WAIT_S})")
    w.add_argument("--no-host-wait", action="store_true",
                   help="exit 64 \"no usable host\" at once instead of waiting")
    p.add_argument("--pull", choices=("divergent", "all"), default="divergent")
    p.add_argument("--filter", help="row substring each unit captures in every corpus (with "
                   "--freecam-filter: a partial run, decided by the capture's own verdict)")
    p.add_argument("--freecam-filter", help="freecam name or tile-key substring each unit "
                   "captures, in place of --filter for the freecam corpus")
    p.add_argument("--repeat", type=int, metavar="N",
                   help="capture each filtered row N times concurrently on its host and "
                        "compare every copy against the blessed row (needs a filter)")
    p.add_argument("--wait", action="store_true", help="follow the detached run to its end")
    p.add_argument("--expect", metavar="FILE",
                   help="expected-moves file: `<unit> <tile substring> <reason>` per line")
    p.add_argument("--no-takeover", action="store_true",
                   help="never move a running unit to a free faster host (placement only)")
    p.add_argument("--commits", metavar="RANGE",
                   help="the batch's commit range, recorded in run-args.json for the grade")
    p.add_argument("--queries", metavar="DIR",
                   help="the batch's query files (scripts/golden-census.py); copied into the run "
                        "directory, their sha256 recorded in run-args.json before any leg starts")
    p.add_argument("--foreground", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--run-id", help=argparse.SUPPRESS)
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("stop")
    p.add_argument("run_id", metavar="run-id")
    p.add_argument("--unit", help="the job name to stop (default: every running and pending unit)")
    p.set_defaults(fn=cmd_stop)
    p = sub.add_parser("move")
    p.add_argument("run_id", metavar="run-id")
    p.add_argument("--unit", required=True, help="the unit's job name, e.g. spyro-1-presentation")
    p.add_argument("--to", required=True, help="the host the unit moves to")
    p.add_argument("--no-wait", action="store_true",
                   help="return once the request is written, without the coordinator's outcome")
    p.set_defaults(fn=cmd_move)
    p = sub.add_parser("resume")
    p.add_argument("run_id", metavar="run-id")
    p.add_argument("--unit", required=True, help="the stopped unit's job name")
    p.add_argument("--to", required=True, help="the host that finishes the unit")
    p.add_argument("--from", dest="from_dir", metavar="DIR",
                   help="saved rows (default: <run dir>/kept)")
    p.add_argument("--pull", choices=("divergent", "all"), default="divergent")
    p.add_argument("--filter", help="the run's --filter (default: its run-args.json, else none)")
    p.add_argument("--freecam-filter",
                   help="the run's --freecam-filter (default: its run-args.json, else none)")
    p.add_argument("--wait", action="store_true", help="follow the detached resume to its end")
    p.add_argument("--foreground", action="store_true", help=argparse.SUPPRESS)
    p.set_defaults(fn=cmd_resume)
    p = sub.add_parser("clean")
    p.add_argument("--hosts", default="")
    p.add_argument("--tree", help="remove only the trees this coordinator keyed on checkout PATH "
                   "(the repository base stays)")
    p.add_argument("--all", action="store_true",
                   help="remove every fleet file, fleet container and the :fleet tag")
    p.set_defaults(fn=cmd_clean)
    a = ap.parse_args(argv)
    sys.stdout.reconfigure(line_buffering=True)
    try:
        return a.fn(a)
    except FleetError as e:
        msg = str(e)
        if e.code == EXIT_SKIP and not msg.startswith("SKIP"):
            msg = "SKIP: " + msg
        print(f"[fleet] {msg}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())
