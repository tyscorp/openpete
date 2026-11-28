# Golden fleet

`scripts/golden-fleet.py` runs the presentation golden
(`tests/harness/check_presentation_golden.sh`) on a set of hosts reached
over ssh. The work is split into units, one per (game, tier) pair: the
presentation tier of spyro-1, spyro-2 and spyro-3, and the dragons tier of
spyro-1. Each unit is captured on one host inside the pinned capture image
(`tests/harness/linux-capture.Dockerfile`, run through
`scripts/golden-capture-container.sh`) and decided on that host by the same
harness with `--verdict-only`, so a fleet verdict is the harness's verdict
over the same image, binaries and disc bytes. A run never blesses and never
promotes.

## Host file

Path: `$OPENPETE_FLEET_HOSTS` when set, else
`${XDG_CONFIG_HOME:-~/.config}/openpete/fleet-hosts.toml`.
The main checkout is resolved through `git rev-parse --git-common-dir`, so
every worktree reads the same file. The file is gitignored and belongs to
one machine: it names that machine's hosts and their measurements.

One table per host:

```toml
[hosts.local]
transport = "local"
fleet_root = "~/.cache/openpete-fleet"
slots = 4
speed = 1.00
calib_s = 270
calibrated = "2026-01-01 governor=performance cpu=AMD Ryzen 9 7950X"
enabled = true
default = true
build = true
nice = 19

[hosts.box2]
transport = "ssh"
ssh = "user@box2"
engine = "podman"
slots = 2
speed = 0.45
calib_s = 600
calibrated = "2026-01-01 governor=performance cpu=Intel Core i5-8400"
enabled = true
default = true
build = true
```

| Key | Type | Meaning |
|---|---|---|
| `transport` | string | Required. `"local"` is the coordinator's own machine (at most one host); `"ssh"` a remote host. |
| `ssh` | string | `user@host`. Required for `"ssh"`, refused for `"local"`. |
| `fleet_root` | string | Directory holding every fleet file on the host; `~` resolves on the host. Default `~/.cache/openpete-fleet`. |
| `engine` | string | `"podman"` or `"docker"`. Default: the first of the two found on the host. |
| `slots` | integer | `CORPUS_JOBS` for a unit on the host (at least 1). Written by `add`. |
| `speed` | float | Throughput relative to the local host (local is 1.00). Written by `add` and `calibrate`. Scales leg timeouts, deadlines and placement. |
| `calib_s` | integer | Wall time in seconds of the calibration unit at `slots`. Written by `add` and `calibrate`; the local host's is `t_local` for every remote host's `speed`. |
| `calibrated` | string | `YYYY-MM-DD governor=G cpu=MODEL`, written by `add` and `calibrate`. A host without `governor=` here, or whose current governor differs, is dropped by preflight. |
| `enabled` | boolean | `false`: the host is never used. Default `true`. |
| `default` | boolean | `true`: the host joins every run that names no `--hosts`. Default `true`. |
| `build` | boolean | The host may build the engine for a run. Default `true`. |
| `nice` | integer | CPU priority of units on the host. Default 19 on the local host, 0 on a remote host when absent; `add local` writes 19. |

Only `add` and `calibrate` write the file. Each replaces the named host's
table and keeps every other line. The parser accepts strings, integers,
floats and booleans; any other table, key or type exits 1 naming the line
or key.

`slots`, `speed` and `calib_s` are measured, not chosen. `add` measures a
host with the machinery of `run` (preflight without the calibration
requirement, the host's `clean`, image shipping, the build on the local
host, the positive sync set into `runs/<run-id>/tree` and a detached
supervisor per leg under the host golden lock). Each leg is the spyro-2
presentation unit (capture, then `--verdict-only`) at one slots value, with
`LP_NUM_THREADS = ceil(cores / slots)` and `MemAvailable` sampled once
a second on the host. A leg whose report compares fewer than every blessed
row, or lists a missing tile, ends the `add` with exit 1 after printing the
tails of the leg's logs.

The sweep runs slots 1, 2, 4, ... up to the host's core count (on the local
host, up to `MemAvailable / 2 GiB`) and stops at the first step that
improves wall time by under 10% or whose leg's minimum `MemAvailable` falls
under 2 GiB. `slots` is the smallest sweep value past which wall time
improves by under 10%, lowered while the leg's minimum `MemAvailable` is
under 2 GiB; `calib_s` is the leg's wall time at that value. For a remote
host, `speed` is `t_local / t_host`, where `t_local` is the local host's
`calib_s`. When the local host has none, the same `add` runs a local leg at
the local `slots` beside the remote sweep and writes the local `calib_s`
too. `add local` records speed 1.00. `add` prints its wall time.

A local leg counts only with the local machine idle: the 1-minute load
average at most `max(1, 0.1 x cores)` before the leg (waited for up to
600 s) and, 90 s after it, at most that bound plus a quarter of the load at
the leg's end. A loaded leg is discarded and re-run; the third loaded leg
at one slots value ends the `add` with exit 64.

`--dry-run` preflights, prints the build host and every planned leg, and
writes nothing. `--timings FILE` applies the rules to recorded legs, ships
no image and runs nothing; `"local"` is optional, and without it `t_local`
is the local host's `calib_s`:

```json
{"cores": 8,
 "sweep": [{"slots": 1, "wall_s": 1500, "min_memavail_kib": 9000000}],
 "local": [{"slots": 4, "wall_s": 270, "load_before": 0.4,
            "load_end": 22.0, "load_after": 5.1}]}
```

`calibrate` runs one leg at each host's recorded `slots` (`--hosts`, else
every enabled host; the local one under the idle rule) through the same
machinery, in parallel across hosts, and rewrites `speed`, `calib_s` and
`calibrated`. It records the current governor, so it clears a governor
mismatch that preflight reports. A leg whose minimum `MemAvailable` falls
under 2 GiB prints that `add` re-measures `slots`; `calibrate` never
changes `slots`.

## Commands

Run from the checkout (or a worktree) whose working copy is to be gated.

```sh
# Record the local host (measured sweep, takes the local golden lock).
scripts/golden-fleet.py add local
# Record a remote host (measured sweep on it; a local leg too when the local
# host has no calib_s); --dry-run prints every planned leg and writes nothing.
scripts/golden-fleet.py add box2 --ssh user@box2 --engine podman
# Apply the rules to legs measured elsewhere.
scripts/golden-fleet.py add box2 --ssh user@box2 --timings box2-legs.json
# Re-measure speed at the recorded slots on every enabled host.
scripts/golden-fleet.py calibrate
scripts/golden-fleet.py calibrate --hosts box2
# Reachability, engine, :fleet image Id against the local image, image.id,
# golden lock and its holder, governor, calibration, free space, fleet_root size,
# and each tree under trees/ (the repository base: size, age, last checkout).
scripts/golden-fleet.py status
scripts/golden-fleet.py status --hosts box2
# Drop reasons per host; exit 0 with a usable host, 64 with none.
scripts/golden-fleet.py preflight --need-gib 20
# Ship the capture image to hosts whose :fleet Id differs.
scripts/golden-fleet.py ship-image --hosts box2
```

`run` gates the working copy. Without `--game` or `--all` the unit is
spyro-1; without `--tier` or `--all-tiers` the tier is `presentation`.

```sh
scripts/golden-fleet.py run --wait                          # spyro-1 presentation
scripts/golden-fleet.py run --game spyro-3 --wait
scripts/golden-fleet.py run --all --all-tiers --wait        # every unit: 4
scripts/golden-fleet.py run --all --hosts local,box2 --wait
scripts/golden-fleet.py run --all --no-local --wait         # keep the dev box free
scripts/golden-fleet.py run --game spyro-2 --pull all --wait
scripts/golden-fleet.py run --all --expect moves.txt        # tag streamed moves against predictions
scripts/golden-fleet.py run --all --commits master~6..master --queries queries --expect moves.txt --wait   # a scored batch
scripts/golden-fleet.py run --game spyro-2 --hosts ptah \
  --filter summer_forest-12609f --freecam-filter summer_forest-12609f --repeat 40 --wait   # a repeat row
scripts/golden-fleet.py stop <run-id> --unit spyro-3-presentation
scripts/golden-fleet.py stop <run-id>                       # every running and pending unit
```

| Flag | Meaning |
|---|---|
| `--game G` / `--all` | One game (`spyro-1`, `spyro-2`, `spyro-3`) or every game. |
| `--tier T` / `--all-tiers` | `presentation` or `dragons` (spyro-1 only), or both. |
| `--hosts a,b` | These hosts instead of every enabled host with `default = true`. |
| `--no-local` | Leave the local host out. Exclusive with `--hosts`. |
| `--local-anyway` | Keep the local host while it is busy (Hosts, step 1). |
| `--host-wait S` / `--no-host-wait` | While no host is usable and a host is dropped only for another holder's golden lock or a busy local host, re-run preflight every 60 s for up to `S` seconds (default 21600); `--no-host-wait` exits 64 `no usable host` at once. |
| `--pull divergent\|all` | `divergent` (default): the report files, divergent tiles and identity pairs; `all`: the whole candidate directory. |
| `--wait` | Follow the detached coordinator and exit with its code. |
| `--filter S` | In every corpus, the rows whose name or tile key contains `S`: bk2 rows by name, freecam rows by name field or tile key, the credits leg when `credits-interp` contains `S`. A corpus with no matching row runs none of it. With either filter the unit captures part of its corpus and is decided by its capture's own verdict; a partial run decides nothing for landing. |
| `--freecam-filter S` | Freecam rows whose name field or tile key (`<level>-<pose md5 prefix>`) contains `S`, in place of `--filter` for the freecam corpus; a tile key selects one pose. Without `--filter`, the bk2 rows and the credits leg run none. |
| `--repeat N` | Capture every row the filters select `N` times on its one host and compare every copy against the blessed row (Repeat rows). Needs a filter. |
| `--expect FILE` | Expected-moves file (Streamed divergences); a line naming a unit not in the run, or a substring matching nothing, refuses the run with exit 2 before it detaches. |
| `--no-takeover` | Placement only: a free host never takes over a running unit (Takeover). |
| `--commits RANGE` | The batch's commit range, recorded in `run-args.json`; `golden-census.py score` classes a miss `environment` when no commit of the range touches a renderer path. |
| `--queries DIR` | The batch's query files ([Tile census](tile-census.md)). `run` copies `DIR` to `<local run dir>/queries/` before it detaches and records each file's SHA-256 in `run-args.json` before any leg starts; the local run directory keeps the copy for `golden-census.py score`. A missing directory exits 2. |

`run` detaches the coordinator (`setsid`) and prints its log and done
marker, `<local fleet_root>/runs/<run-id>/coordinator.log` and `done`.
Without `--wait` it exits 0 at once; the run's code is the `rc=N` first
line of `done`.

```sh
scripts/golden-fleet.py clean                     # stale files on every enabled host
scripts/golden-fleet.py clean --hosts box2
scripts/golden-fleet.py clean --tree "$PWD"       # checkout-keyed trees of one checkout (the base stays)
scripts/golden-fleet.py clean --all --hosts box2  # every fleet file on box2
```

## What a run does

1. Hosts. The selected hosts are preflighted: a host is dropped when it
   is disabled, unreachable, has no container engine, is uncalibrated,
   runs another governor than the calibrated one, lacks free space (the
   sync set, the build tree and the largest candidate set, plus the image
   when it must ship), or its golden lock is held. The local host's slots
   are lowered to fit 2 GiB of `MemAvailable` per slot. The local host is
   left out, as under `--no-local` and printed with the reason, while it is
   busy: a landing holds `land-train.lock` in the git common directory,
   `land-branch.sh`, `land-train.sh` or `sched_gate.py` runs, a game
   process runs outside the fleet's containers, or `MemAvailable` is under
   one slot plus a 4 GiB reserve; `--local-anyway` keeps it. When no host
   is usable and some host is dropped only for another holder's golden
   lock (the holder record names its pid, host and run) or for being busy,
   the coordinator prints the holders and re-runs preflight every 60 s,
   polling the stop request, for up to `--host-wait S`; then, or at once
   under `--no-host-wait` or with no such host, the run exits 64 `no usable
   host`. The run refuses
   (64) when the local capture image was built from a recipe other than
   the tree's `tests/harness/linux-capture.Dockerfile`. The coordinator
   takes the local golden lock, runs `clean` on each host and ships the image.
2. Image shipping. The image is identified by its Id under the tag
   `openpete-linux-capture:fleet`. When a host's `:fleet` Id differs from
   the local image's, `<engine> save` is streamed into
   `ssh host <engine> load`, the loaded Id is tagged `:fleet` and
   `<fleet_root>/image.id` records it. No label is added, since a label
   changes the image's Id.
3. Build once. The fastest host with `build = true` builds the engine
   inside the image (`golden-capture-container.sh build`, the CMake target
   `openpete_games` into `build/linux-release-container`): in the checkout when that is the local
   host, else in its snapshot, seeded from its previous tree. The binaries'
   SHA-256 go to `binaries.sha256`; a remote build tree is relayed through
   the coordinator to the other hosts and checked against those hashes.
   Every unit then runs `gate --no-build`, which verifies the binaries with
   `sha256sum -c` before capturing.
4. Sync. The positive set is copied to `<fleet_root>/runs/<run-id>/tree`
   on every host concurrently, with `rsync --checksum --link-dest` from the
   repository base `<fleet_root>/trees/<key>` (Sync base): git's tracked
   and non-ignored untracked files (submodules included),
   `games/*/generated`, each game's manifest `globals.toml` and
   `symbols.toml`, each run game's disc cue and bin,
   `third_party/psxrecomp/build`, and the build tree minus `home/` and the
   disc-hash memo. While it reads the checkout the coordinator holds
   `build/linux-release-container.build-lock` and runs a
   `golden-fleet-sync` marker process, which marks the checkout as being
   read; do not build while it runs. Each run game's
   capture fields are written with `scripts/presentation-blessed.py
   capture-fields` (a snapshot has no `.git`; the capture record reads them
   through `OPENPETE_CAPTURE_FIELDS`).
5. Torn-sync recheck. Every copy is compared again with
   `rsync -n --checksum` over the same lists, the hosts concurrently; any
   change refuses the run as torn. The synced disc cue and bin are hashed
   on every host against the blessed manifest headers. Each host's base
   is then refreshed from its snapshot.
6. Placement. Units are placed longest first, one per free host, from the
   unit-time ledger `fleet-unit-times.tsv` beside the reference store (a
   unit without a record is scaled from 60 min for the spyro-1
   presentation unit by candidate size). Each unit runs under a detached
   supervisor that holds the host's golden lock, with `CORPUS_JOBS` =
   slots, `PRESENTATION_LEG_TIMEOUT` = 300 / speed x 1.5 and a deadline of
   expected time / speed x 2. A lost, dead or overdue attempt is killed
   (the supervisor's session by PID and start time, its containers by the
   label `org.openpete.fleet=<run-id>`) and re-queued, at most 2 attempts;
   a host unreachable for 10 min is dropped. A re-queued attempt on the same
   host resumes at row level (Row resume, below). A free host that no
   queued job fits takes over the running unit predicted to finish last
   when that pays (Takeover, below). Every poll also reads the
   attempt's streamed row verdicts, pulls new divergences and renders their
   heat diffs, and acts on a `stop` request (Streamed divergences, below).
7. Canary. With two or more capture hosts, every host first captures one
   bk2 and one freecam level of the first unit's game. A canary tile whose
   verdict digest differs between hosts fails the run.
8. Verdict. The capturing host runs the harness's capture, then
   `check_presentation_golden.sh --verdict-only <candidate> --game G` in
   the image. The coordinator prints the report's census,
   `census captured=C imported=I (R rows) compared=K of N`, and the summary
   repeats it beside each unit's verdict. The accepted attempt's stream is
   checked against the report (unit-end check).
9. Pull. `verdict-report.txt`, `capture-record.txt`,
   `capture-device.txt`, `credits-verdict.txt` (presentation tier), the
   verdict logs, the divergent tiles and both members of each identity pair
   over baseline (under `identity/`) are pulled into the cleared local
   candidate directory: `build/presentation-candidate`,
   `build/presentation-candidate-<game>`, `build/dragon-candidate`, or
   `$PRESENTATION_GOLDEN_OUT` (with a `<game>-<tier>` subdirectory when the
   run has several units). On the presentation tier the pull also takes
   `tile-digests.tsv`, `row-cost.tsv` and, by a second rsync under either
   `--pull` mode, every tile's `*-vk.census` sidecar; a captured tile
   without its sidecar refuses the unit. A unit fails when the report's
   `blessed.tsv` or identity-baseline hash differs from the local file, its
   `compared` census differs from the local row count, or its exit differs
   from the supervisor's. A filtered unit whose only moves are extra tiles
   (no divergent tile, identity failure, crashed leg or failed credits
   leg) ends `NEW-ROWS n tile(s)`, rc 0, its compared-nothing census
   accepted: the expected end of a new row's first capture; the summary's
   `result <unit>: NEW-ROWS` line names the tiles. A unit end that keeps no
   run candidate (a refused pull, an exit other than 0 or 1) still pulls
   `capture-record.txt`, stamped, `capture-device.txt` and
   `verdict-report.txt` into `build/fleet-stream/<run-id>/<unit>/`, so its
   streamed tiles promote. An accepted presentation unit of an unfiltered,
   unrepeated run that was not stopped is hard-linked into the census store
   (`<local fleet_root>/census/<game>-presentation/<run-id>/`, finalized
   after the done marker, two entries kept per unit; layout under
   [Tile census](tile-census.md#census-store)).
10. Heat diffs. The pulled divergent tiles are diffed locally against the
    store with `tests/harness/presentation-corpus-diff.py --expect-subset`
    into `<candidate>/diff`.
11. Teardown. Every touched host's crashed capture legs (each unit's
    `cand.crashes/`: leg log, argv, exit code, hang and core dumps) are
    pulled into `<local run dir>/crashes/<host>/` and each kept leg is
    named in `coordinator.log`. `runs/<run-id>/` is then deleted on every
    host on every exit path; the local run directory keeps
    `coordinator.log`, `done`, `active.lock`, `run-args.json`,
    `stream.log`, `expect.txt`, `queries/`, `stop-request`,
    `canary-verdict.txt` and `crashes/`. A host unreachable at teardown keeps its directory for
    `clean`.

## Connect retries

A connect failure is retried. It is an ssh or rsync command of a remote
host that exits 255 (rsync: 255 or 12) with one of ssh's pre-session
messages on stderr: `ssh: connect to host <h> port <p>:` followed by
`Connection timed out`, `Connection refused`, `No route to host`,
`Network is unreachable` or `Operation timed out`; `ssh: Could not
resolve hostname`; `kex_exchange_identification:`; or `Connection timed
out during banner exchange`. The remote command never started, so a
retry repeats no work. Each retry waits 5, 15, then 30 s (at most 3
retries, `TRANSPORT_BACKOFF_S`) and logs
`[fleet] <host>: <operation>: <ssh line>; retry k/3 in N s`; a stop
request ends the wait. A remote command's own nonzero exit, 255
included, is not retried.

Retried: preflight's probe, `clean`, the image load (the save restarts
with it), run-directory creation, base check and refresh, every sync
push, the torn-sync recheck, the build seed, the binary and build-tree
pulls, disc and binary hashes, unit launch, kill, row carry and pulls,
crash pulls, reap and teardown deletion. Not retried: the per-unit poll
(a host unreachable for 10 min is dropped instead), the clock-skew
measurement (a retry would widen its bound) and `status`.

A recheck whose rsync connects after a retry compares as usual; one that
cannot connect after the retries fails the run with
`<host>: recheck of <dir> could not connect after 4 attempts: <ssh
line>`, not as a torn sync. Any other call that exhausts its retries
fails as before, the message naming the host, after
`[fleet] <host>: <operation>: connect failed 4 times, giving up: <ssh
line>`.

## Row resume

A unit's or canary's supervisor sets `OPENPETE_RESUME_RUN=<run-id>`
(passed into the image by `scripts/golden-capture-container.sh`). The
harness (`scripts/corpus-resume.sh`, sourced by
`check_presentation_golden.sh`, `presentation-corpus.sh` and
`freecam-corpus.sh`) then stamps every manifest row it finishes: when the
last of a row's legs (each LOD mode times canon, inter and ident) has
written every tile it was asked for, `cand/resume/<kind>.<key>.stamp` is
written atomically:

```
run: <run id>
tree: <sha256 of the capture-fields file: head, source tree, recomp input, generated tree>
binary: <sha256 of the capture binary>
params: kind=bk2 game=spyro-1 tier=presentation affine=default aspect=16:9 lods=original extended samples=5
row: <the row's manifest line, verbatim>
tile: <sha256> <tile name>          one per tile, identity tiles included
device: <the leg's Vulkan device line>
```

`kind` is `bk2`, `freecam` or `credits`; a passed credits leg is the row
`credits-interp` whose one tile is `credits-verdict.txt`. A row interrupted
mid-capture has no stamp.

A re-queued attempt on the same host runs in the same `u-<job>/` directory.
Before capturing, the harness moves the previous `cand/` aside and imports
a stamp's row only when its run id, tree, binary and params equal the
attempt's, its row line is a line of the current manifest that the run's
filter selects, its key is that row's key, and every listed tile is present
with its sha256. An imported row's tiles move into a fresh `cand/` (its
device lines into `capture-device.txt`); everything else of the previous
attempt is deleted. The capture then skips the imported rows, and the
strict diff covers imported and captured tiles alike.
`cand/resume/imported.tsv` lists the imported rows; the census line
(`captured=C imported=I imported-rows=R compared=K golden=N`) and
`verdict-report.txt` (`captured:`, `imported:`, `imported-rows:`) count
them apart, so an import never stands in for a missing row. A new run id
imports nothing: its run directory is new and every stamp names its run.
A re-queued attempt on another host captures every row. Calibration legs
never resume, since their wall time is the measurement. `--bless` neither
reads nor writes stamps.

`tests/harness/check_corpus_resume.sh` checks the stamp and import rules on
a synthetic candidate directory (no binary or image): a finished, a
partial and a tampered row, another run id, binary, tree, LOD set,
manifest line and filter, an interrupted import, concurrent legs, and
resume off.

## Takeover

Placement gives each free host the next queued job, one per host. Once
the queue holds nothing a free host may take, the host looks at the units
running on the other hosts and takes over the one predicted to finish
last, through the `move` mechanism: the running attempt is stopped, its
stamped rows are carried to the free host (through the coordinator for a
remote source), and the unit is re-queued pinned there; the new attempt
imports the carried rows (Row resume), captures the rest and runs
`--verdict-only` over the whole unit. The prediction, for running host
`a` and free host `b`:

| Term | Value |
|---|---|
| `done`, `total` | the running attempt's streamed SAME and DIVERGE tiles (imported included); the unit's blessed tiles |
| `left` | `1 - done / total` |
| `E`, `B` | the ledger's expected time at speed 1.00 and candidate bytes |
| `rows` | the unit's corpus rows (bk2 rows and freecam levels) |
| remaining on `a` | `left x E / s_a - E x min(slots_a, left x rows) / (2 x rows x s_a)`; the second term is the half-done rows in flight, which `b` recaptures |
| cost | `120 s + hops x (1 - left) x B / 10 MiB/s`: kill, preflight, launch and import, plus the carry (one hop per remote host of the two) |
| remaining on `b` | `left x E / s_b + cost` |
| saving | remaining on `a` minus remaining on `b` |

The takeover happens when `s_b > s_a` (never from a faster host to a
slower one), remaining on `a` exceeds the cost, and the saving exceeds
600 s (`TAKEOVER_MIN_SAVING_S`: a unit's time on the shared local host
varies by tens of percent with user load, so a smaller saving is within
the error of its prediction). A taken-over unit is taken over again only by
the same test, so only by a still faster host. A refusal is logged once
per free host and attempt (`takeover not taken: ...`). A unit whose done
marker appears while its stop is in flight keeps that attempt's result;
nothing is carried and the takeover is refused.

Each takeover logs `takeover: <unit> on <a> -> <b>`, the share streamed,
both predicted remaining times, the cost and the saving, then the rows and
tiles carried. The summary prints, per takeover, the measured time on `b`
(from the stop to its done marker) and the saving against the predicted
remaining time on `a`. Takeover is off under `--no-takeover`, a filter,
`OPENPETE_FLEET_STREAM=0` (no progress), `OPENPETE_FLEET_CANARY_ONLY`,
`resume` and `calibrate`.

The unit-time ledger prices a unit over the attempts that captured its
rows: `norm_s` is wall x speed summed over the accepted attempt and each
earlier attempt of the run whose stamped rows it imported (the previous
attempt on the same host, or one a move carried), and `host` names the
chain (`ptah>local`). A whole unit's single attempt gives the same row as
before. Each attempt is priced at its own host's speed, which is how a
single attempt is normalised; the rows in flight at each kill are counted
twice (a few percent of a unit). A filtered unit, a resumed unit and an
attempt that imported rows from outside the run write no row.

## Repeat rows

A defect that moves a tile in some processes and not others (a data race
between the game thread and a renderer worker) passes a single capture
most of the time. `run --repeat N` measures such a row: each unit captures
every row its filters select `N` times as repeat copies and compares every
copy against the original row.

- The unit runs `check_presentation_golden.sh --repeat=N` with its filters.
  The harness exports `PRESENTATION_REPEAT=N`; `presentation-corpus.sh` and
  `freecam-corpus.sh` then capture copy `k` (1 to `N`) of each selected row
  under the tile key `<key>~r<k>` (`summer_forest-12609f~r07__lodext__canon__2473-vk.png`)
  and do not capture the original row. The copies are queued one after
  another through `cj_run`, so `CORPUS_JOBS` (the host's `slots`) of their
  legs run at once, at the host's `LP_NUM_THREADS`.
- `scripts/presentation-blessed.py` `copy_origin` maps a copy's tile to its
  original's. A copy's golden tile is compared against the original row's
  blessed digest (`SAME` or `DIVERGE` under the copy's name, never `EXTRA`),
  and its identity tile against the copy's own canon tile with the original
  tile's baseline row. `list_tiles` leaves copies out, so a promote, a
  `write` or a `verify` never sees one, and an identity `--bless` writes no
  copy row.
- `presentation-corpus-diff.py --repeat N` prints one line per repeated
  row, `REPEAT <key> divergent=D of=T compared=C copies=N`, where `T` is `N`
  x the row's legs (its blessed tiles plus one identity pair per canon
  tile). A row fails when `C` is under `T`, a copy number lies outside
  1..`N`, or `D` is nonzero. `verdict-report.txt` adds `repeat: N`, one
  `repeat-row:` line per row and the totals `repeat-divergent`,
  `repeat-legs` and `repeat-compared`; the census's `compared` counts the
  copies' golden tiles.
- The coordinator refuses the unit when the report's `repeat` differs from
  the run's or `repeat-compared` is under `repeat-legs` (the engagement
  bound), and prints each row's divergent count of `N` x legs on the
  unit's result line. `run-args.json` records `N`, so `resume` repeats it;
  a copy's resume stamp imports only into a repeat run whose copies
  include it.

The pass criterion is 0 divergent of `N` x legs. The committed repeat row:

| Game | Row (tile key) | N | Host | Command |
|---|---|---|---|---|
| spyro-2 | `summer_forest-12609f` (homeworld portal sky, lodext) | 40 | ptah | `scripts/golden-fleet.py run --game spyro-2 --hosts ptah --filter summer_forest-12609f --freecam-filter summer_forest-12609f --repeat 40 --wait` |

`--filter` names the tile key too, so the bk2 corpus selects no row. The
row covers the cyclorama sector decode the game thread's sky feed and the
portal-sky worker share; with a decode buffer shared between the two, 24
of 240 legs diverged at `N` = 40 on ptah.
`N` = 40 separates at the measured pre-fix rate of the most frequent leg (11
of 40 copies); a build with a 15 % per-copy rate passes 40 copies with
probability 0.0015. 0 of 40 bounds a residual rate only below 7.5 % per
copy (95 %).

## Streamed divergences

A unit's capture streams each row's verdict while it runs, so a divergence
reaches the dev box minutes into a unit instead of at its end. The
supervisor sets `OPENPETE_STREAM_FILE=<u-job>/attempt-N.stream` for a unit
(never for a canary, a calibration leg, a bless or a plain local gate);
`scripts/golden-capture-container.sh` passes it into the image. When the
last leg of a row finishes, `scripts/corpus-resume.sh` classifies the row's
tiles outside the row lock with `tests/harness/presentation-stream.py`,
which calls the end verdict's own functions (`classify_tiles` of
`presentation-corpus-diff.py`, `classify_pair` of
`presentation-ident-check.py`), and appends one line per tile:

```
SAME    <tile> <blessed digest>                     <host epoch>
DIVERGE <tile> <blessed digest>-><candidate digest> <host epoch>
EXTRA   <tile> -><candidate digest>                 <host epoch>
IDENT   <tile> pixels=<n>/<baseline>                <host epoch>
EXCEED  <tile> pixels=<n>/<baseline>                <host epoch>
CREDITS rc=<N> <host epoch>
END     <host epoch>
```

Digests are the first 16 hex digits of the verdict digest. EXCEED also
covers an identity tile without its canon twin (`pixels=nocanon/<b>`). The
credits leg appends `CREDITS`; a tier without the leg has none, which
stands for credits-rc 64. A re-queued attempt classifies the rows it
imports again and appends their lines tagged `imported` (the credits leg
included). The capture's verdict appends `END`: the capture reached its
verdict, so every row's lines are in the stream. The end verdict's image
comparison is unchanged.

Each poll runs `POLL_SCRIPT <attempt base> <offset>`, which returns the
done marker first and then the stream's new bytes from the offset, in
framed sections; a kill reads them once more before `KILL_SCRIPT`. The
coordinator appends each complete line to `<local run dir>/stream.log` as
`<unit> <host> <attempt> <receipt epoch> <line> [EXPECTED|UNEXPECTED]`,
after one `# skew <host> <seconds> bound <seconds>` line per remote host
(the host clock minus this machine's, measured at run start). A DIVERGE,
EXTRA or EXCEED line is printed in the coordinator log with its latency
(receipt minus the host epoch, skew removed). Per poll, at most 20
divergent tiles, extra tiles and identity pairs (both members) are pulled
in one rsync into `build/fleet-stream/<run-id>/<unit>/`, outside every
candidate directory, and their heat diffs are rendered into its `diff/`
against the local store (a divergent tile) or canon against ident (an
identity pair); an extra tile, a new row's with no blessed digest, has no
reference and gets no heat diff; the rest wait for the next poll in stream order. A pull or render failure
is logged and changes no result. `clean` removes the directory with its run.

Unit-end check. When the accepted attempt's stream holds `END`, its lines
must satisfy SAME + DIVERGE = the report's `compared`, and its DIVERGE,
EXTRA and EXCEED tiles and CREDITS rc must equal the report's `divergent`,
`extra`, `identity-exceed`/`identity-nocanon` and `credits-rc`; and,
after the accepted pull, every DIVERGE and EXTRA tile must be at the top
level of `build/fleet-stream/<run-id>/<unit>/` and both members of every
EXCEED pair under its `identity/` (a tile only under `stream-only/` is
absent). A mismatch
sets the unit's rc to 1 and prints `stream mismatch: ...` beside the
report's own status. Stopped and SKIP units and an attempt without `END`
(a capture that timed out or failed before its verdict) are exempt.
`OPENPETE_FLEET_POKE` changes a tile after the stream classified it, so a
poked unit also reports a stream mismatch.

### Stop

`scripts/golden-fleet.py stop <run-id> [--unit <job>]` writes
`<local run dir>/stop-request` atomically (`unit=<job>` per named unit,
`all` without `--unit`); it is refused (exit 1) once the run's `done`
exists, and with exit 2 for an unknown run. The coordinator reads the
request once per loop iteration. A named running or pending job gets its
result (rc 1, `stopped by user`, no ledger row) before `KILL_SCRIPT`, so
it is not re-queued; a job whose done marker was read first keeps its
result. A stopped unit's `build/fleet-stream/<run-id>/<unit>/` gets a
`STOPPED` marker. A run with a stopped job is FAIL, `reason=stopped by
user` in `done`; the local golden lock is released when the coordinator
exits. A stopped run is not resumed.

### Expected moves

`run --expect FILE` reads one line per expectation, `#` comments allowed:

```
<unit> <tile substring> <reason>
spyro-1-presentation cliff-town-vista__lodext__ far-band LOD change
```

`<unit>` is the job name (`<game>-<tier>`); the substring is matched
against tile names and manifest row names, so a row name covers the row's
`__ident__` tiles. At start each line must match a unit of the run and at
least one blessed tile, the `__ident__` twin of a blessed canon tile, or a
manifest row name; otherwise the run exits 2 before it detaches
(`expected-moves file refused: line N: ...`). A bk2 row whose launch field
selects extended LOD (`--lod extended`, `--no-lod`) runs its untagged leg
at extended LOD too, so its tiles also match as their `__lodext__`-tagged
names. A line naming a base row covers that row's `@<variant>` rows: a
tile of a bk2 variant `<row>@<v>` also matches as `<row>`'s tile name, a
freecam variant `<level>@<v>-<pose>` as the plain row `<level>-<pose>` at
the same pose. The file is copied into the local run directory before the
coordinator detaches and only the copy is read. Each line's match count is
printed at start: `expect: <unit> '<substring>': <t> tile(s), <r> manifest
row(s) match`, followed by `; covers variant row(s) ...` when the line
matches variant tiles only through their base row.

The streamed verbs that count as a move are DIVERGE (a blessed tile's
digest changed), EXTRA (a tile with no blessed row: a new row) and EXCEED
(an identity pair over its baseline, or an identity tile without its canon
twin). A moved tile is EXPECTED when an expectation of its unit matches it,
else UNEXPECTED (the tag at the end of its `stream.log` line). At the end
each expectation prints `expect <unit> '<substring>': <status> (<k>
matching tile(s)); <reason>`, the status one of `moved` (a matching tile
moved), EXPECTED-NOT-MOVED (every matching tile, of `blessed.tsv` and of
the stream, streamed and none moved) or NOT-REACHED (a matching tile never
streamed: a timed-out row, a stopped or SKIP unit), and the run prints
`expected moves: EXPECTED <a>, UNEXPECTED <b>, EXPECTED-NOT-MOVED <c>`,
counted over each unit's accepted attempt (imported lines included; a
killed attempt's lines are tagged for display only). The tags and counts
change no exit code and take no part in the unit-end check.

`scripts/golden-census.py expect` writes expected-moves lines from the
census store: per tile a predicate over the tile counters selects, and per
row added since the store's run ([Tile census](tile-census.md#expect)).

`tests/harness/check_fleet_stream.sh` checks the stream on synthetic tiles
(the line per verb against the end verdict's own scripts, the row poke,
stream without stamps, off, a resumed attempt against the uninterrupted
one) and runs `tests/harness/fleet-stream-check.py`, which places fake
supervisors on this machine through the coordinator: poll framing, the
unit-end check and its refusals under `OPENPETE_STREAM_DROP`, a kill in the
last poll interval and the re-queued attempt, the exemptions, `stop`
(running, pending, local, racing completion) and expected moves. Its
takeover cases run the coordinator over simulated hosts on a virtual clock
(no ssh, no supervisor): the takeover of the last-finishing unit and its
ledger row, a takeover that does not pay, a free slower host,
`--no-takeover`, and a unit finishing while its stop is in flight.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | PASS: every unit passed. |
| 1 | FAIL: a unit diverged or failed, a pull or report check refused, the canary differed, or the run failed (torn sync, build, transfer). |
| 64 | SKIP: no usable host, no host file, no local engine or image, a stale image recipe, a missing disc, or a unit whose capture skipped. |
| 2 | Usage error (argparse, `clean --all --tree`, `add <name>` without `--ssh`, a malformed `OPENPETE_FLEET_POKE` or `OPENPETE_FLEET_ROW_POKE`, an expected-moves file naming a unit not in the run or a substring matching nothing, `stop` of an unknown run). |

A run's code is the worst unit's in the order 1 > 64 > 0: one SKIP unit
makes the run non-PASS. The summary prints each unit's verdict beside its
host, attempt and wall time.

## Files on each host

```
<fleet_root>/
  golden.lock            flock: one presentation golden per host; holds the holder record
  image.id               Id of the :fleet image the fleet shipped
  image.id.history       earlier shipped Ids, removed by clean
  trees/<key>/           the repository base: the last refreshed snapshot of any checkout of the repository
  trees/<key>.src        coordinator, repository (git common directory), last checkout path, last-run time
  trees/<key>.lock       flock: shared while a sync or build seed reads the base, exclusive while a refresh or clean replaces it
  census/<game>-presentation/<run-id>/
                         local host only: the census store (Tile census); removed only by clean --all
  runs/<run-id>/
    coordinator.log      local host only
    done                 local host only: rc=N, run=, reason=
    run-args.json        local host only: the run's arguments, HEAD, source tree, units, --commits, query hashes
    queries/             local host only: the run's copy of --queries DIR
    active.lock          held shared by the coordinator and supervisors
    ship/                supervise.sh, fields-<game>.txt, binaries.sha256
    tree/                the snapshot units run in
    stream.log           local host only: received stream lines
    expect.txt           local host only: the run's copy of --expect FILE
    stop-request         local host only: written by `stop`
    crashes/<host>/      local host only: crashed capture legs pulled at teardown
    u-<job>/             attempt-N.pid, attempt-N.done, attempt-N.memavail,
                         attempt-N.stream, capture.log, verdict.log, store/, cand/,
                         cand.crashes/ (crashed capture legs, kept across attempts)
                         (cand/resume/: row stamps, imported.tsv)
                         (calibration legs: u-calib-<host>-s<slots>-<n>/)
    helpers.tsv          dbus-daemon helpers started during a job
```

`<key>` is the first 16 hex digits of the SHA-256 of the coordinator's
hostname and the repository's git common directory (`git rev-parse
--git-common-dir`, resolved), so the main checkout and every worktree of the
repository share one base per host. A tree whose record has no `repository`
line was keyed on a checkout path by an earlier coordinator; `clean`
removes it once its checkout is gone or unused for 14 days, or by `--tree`.

## Sync base

- Bytes sent. `--checksum` makes rsync compare each file's content with the
  base's file, not its size and mtime: a gate worktree's files carry the
  worktree's checkout time, so a size-and-mtime match never holds against a
  base from another checkout and every file would be sent. A file whose
  content equals the base's is not sent: rsync hard-links it from the base
  when its mode and mtime also match, else copies it there from the base. A
  run from a new gate worktree sends only the files whose content differs
  from the last synced tree; the log line ends `(base <key> from <last
  checkout>, <age>)`, `(no base)`, or `(foreign base ignored: ...)`.
- Trust. The base's content is never trusted. A base file whose content
  differs from the checkout's (tampered, stale, torn by a killed refresh)
  is sent, not linked, by the checksum comparison; the torn-sync recheck
  then compares every listed file of the snapshot with the checkout by
  checksum, whatever its origin. A base whose record is missing or names
  another coordinator or repository is not linked from; the run's refresh
  replaces it.
- Separation. A run reads only its own snapshot, `runs/<run-id>/tree`. The
  refresh builds `trees/<key>.new` by hard links from a snapshot and
  renames it over the base, so no fleet step writes a base file in place
  and a snapshot linked from an earlier base keeps its content. A sync
  holds `trees/<key>.lock` shared on the host while rsync reads the base
  (the remote rsync server runs under `flock -s`); the refresh and
  `clean` take it exclusive, so a base is never swapped under a sync.
  The units' in-place writers stay off linked files: the container home
  and the disc-hash memo are excluded from the sync and the base, and a
  remote build runs in a build tree copied, not linked, from the base.
- Build seed. A remote build host's build tree is seeded from the base
  only when the base's record names the same checkout path: another
  checkout's objects can be newer than this checkout's changed sources,
  which the build would then not rebuild. A refresh from a snapshot
  without a build tree keeps the previous build tree on the same rule.
- Discs. The sync set holds the disc cue and bin of the run's games only:
  the games of its units plus the canary's game (`run_games`). The refresh
  replaces the whole base with the run's snapshot, so after a run limited
  to some games (`--game G`) the base holds only those games' discs, and the
  next run that captures another game sends that game's disc bin in full.
  `--filter`, `--freecam-filter` and `--repeat` select rows, not games, and
  leave the disc set unchanged.
- Disk per host. Between runs: one base, about the source set plus the
  build tree (2.3 + 1.1 GiB). During a run from a new checkout the
  snapshot's files are copies (their mtimes differ from the base's), so
  the run's tree adds the same again until the run's directory is
  removed; a rerun from the same checkout links its unchanged files.

`clean` works per host under its golden lock (a busy host is skipped) and
removes: `runs/` directories no process holds and with no done marker
newer than 24 h (signalling the `dbus-daemon` helpers their supervisors
recorded, when PID and start time still match); exited fleet-labelled
containers; a repository base this coordinator wrote whose repository is
gone or that no run refreshed for 14 days, and a checkout-keyed tree whose
checkout path is gone or unused for 14 days; shipped images other than the
current `:fleet` one and named by no other tag. A tree whose lock a sync
holds is kept. `--tree PATH` removes only this coordinator's
checkout-keyed trees of PATH; the repository base stays, so a gate
worktree's teardown leaves it for the next run. `--all` removes `runs/`, `trees/`, `census/`, the lock
and image records, every fleet-labelled container and the `:fleet` tag
(the image itself only when no other tag names it), and `fleet_root` when
nothing else is in it. On the local host `clean` also removes the
checkout's `build/fleet-stream/<run-id>/` whose run directory is gone.

## Gate worktree

A batch golden runs from a worktree pinned at the batch commit, so the
main checkout's build directories stay free for landings while the batch
runs. From `openpete/`:

```sh
../.claude/skills/orchestrate/scripts/worktree-setup.sh gate-<batch> --ref <batch-sha>   # detached at the batch commit, configured, not built
cd worktrees/gate-<batch> && python3 scripts/golden-fleet.py run --all --expect <file> \
  --commits <base>..<batch-sha> --queries <queries dir>
```

`--commits` and `--queries` are needed only when the batch's query files
are scored afterwards (`golden-census.py score --run <local fleet_root>/runs/<run-id>`).
The census store lies under the local `fleet_root`, not in the worktree,
so a gate worktree's unfiltered run feeds the same store as a run from the
main checkout.

The run's in-image build writes the worktree's
`build/linux-release-container`; `worktree-setup.sh --build` is not needed
(it builds `build/linux-release`, which no fleet unit reads). The fleet
marker `golden-fleet-sync` names the worktree and every unit, the local
host's included, runs in its host's snapshot under `fleet_root`, so
`build-gate-lock.py` refuses only builds into the worktree's own build
directories while the run builds and syncs; a build in the main checkout
proceeds. The worktree
links `games/<g>/generated` and `manifest/{globals,symbols}.toml` to the
main checkout, so a regen on main before the run's sync recheck changes
the gated sources; the capture record's `generated-tree` records what was
captured. The run's candidates land in the worktree's
`build/fleet-stream/<run-id>/<unit>/`; promote them (below) before
`worktree-teardown.sh gate-<batch>` removes the worktree. Its
`clean --tree` keeps the repository base, from which the next batch's
worktree syncs only its changed files.

## Promote

A pull appends `fleet-run`, `fleet-host` and `fleet-run-dir` to the pulled
`capture-record.txt`; `build/fleet-stream/<run-id>/<unit>/` holds the
unit's divergent tiles, its extra tiles (new rows, which the promote
adds), verdict report, `capture-device.txt` and that record, and is the
promote's candidate directory; the unit-end check fails a unit whose
streamed DIVERGE or EXTRA tile is absent from it. A promote admits a
capture from any host; it is refused only on the Environment surface
(`presentation-blessed.py`, Environment) and on the source tree. The
blessed revision is the first-parent ancestor of the promoting checkout's
HEAD, at most 500 commits deep, whose tree over the source roots equals
the record's `source-tree` (other games' corpus directories aside): the
record's `head` when it matches, else the newest match. A batch captured
at a commit master has since moved past therefore promotes at that commit.
A record matching no ancestor is refused, naming the nearest commit by
differing-path count, and nothing is filed into the store. Promote from
any checkout whose first-parent history holds the batch commit, before
`clean` removes the run directory:

```sh
tests/harness/presentation-corpus-promote.sh worktrees/gate-<batch>/build/fleet-stream/<run-id>/<unit> <tile>...
tests/harness/presentation-corpus-promote.sh build/presentation-candidate-spyro-2 <tile>... --game spyro-2
```

The candidate directory lies under the promoting checkout or the store's
parent directory, which the capture image mounts. A full re-bless
(`--bless`) runs on the dev box.

## Environment

| Variable | Effect |
|---|---|
| `OPENPETE_FLEET_HOSTS` | Host file path. |
| `OPENPETE_CAPTURE_IMAGE` | Local image the coordinator ships (default `openpete-linux-capture`). |
| `OPENPETE_GOLDEN_REF_DIR`, `XDG_DATA_HOME` | Local reference store; the unit-time ledger lies beside it. |
| `PRESENTATION_GOLDEN_OUT` | Local pull target instead of `build/*-candidate*`. |
| `LP_NUM_THREADS` | Pins llvmpipe threads per capture on every host; default `ceil(cores / slots)` per host. |
| `OPENPETE_FLEET_AFTER_SYNC` | Test only: a shell command run in the checkout between the syncs and the recheck (negative control for the torn-sync recheck). |
| `OPENPETE_FLEET_POKE` | Test only: `<host>:<job\|unit\|canary>:<tile substring>` flips one pixel of the first matching candidate tile on that host before its verdict (negative control for the verdict and canary). |
| `OPENPETE_FLEET_ROW_POKE` | Test only: `<host>:<job\|unit>:<tile substring>` changes pixel (0,0) of each matching tile of that host's unit when its leg finishes, before the leg hashes it (`OPENPETE_STREAM_POKE` in the image), so the row streams DIVERGE and the report agrees. |
| `OPENPETE_STREAM_DROP` | Test only: a tile name; the coordinator discards the first stream line naming it (negative control for the unit-end check). |
| `OPENPETE_FLEET_STREAM` | Test only: `0` runs every unit without the stream and its unit-end check (the wall-time baseline of the stream's cost). |
| `OPENPETE_FLEET_DEADLINE_S` | Test only: seconds; replaces the deadline of every unit's and canary's first attempt, so the attempt is killed and re-queued (row resume check). |
| `OPENPETE_FLEET_CANARY_ONLY` | Test only: `1` runs only the canaries, one per host even on one host, pulls each canary's tiles, report, record, device and credits files into `<candidate directory>/canary-<host>`, and exits with the worst canary's code. |

The supervisor sets `CORPUS_JOBS`, `PRESENTATION_LEG_TIMEOUT`,
`PRESENTATION_TIER`, `OPENPETE_FLEET_ROOT`, `OPENPETE_GOLDEN_LOCK_HELD`,
`OPENPETE_CAPTURE_LABEL`, `OPENPETE_CAPTURE_MOUNT_AS`,
`OPENPETE_CAPTURE_FIELDS`, `OPENPETE_CAPTURE_BINARIES`,
`OPENPETE_RESUME_RUN`, `OPENPETE_STREAM_FILE` and `OPENPETE_STREAM_POKE`
itself; a value
set by hand does not reach a unit.

## Troubleshooting

| Message | Cause and action |
|---|---|
| `SKIP: no host file ...` | Create it with `scripts/golden-fleet.py add local`. |
| `dropped: not calibrated (no governor= in calibrated); run add` | Run `add` for the host. |
| `dropped: governor X, calibrated at Y` | Restore the governor, or run `calibrate --hosts <host>` to recalibrate under the new one. |
| `dropped: logind kills an ssh session's processes at logout (KillUserProcesses=yes) and lingering is off ...` | Run `loginctl enable-linger` on the host (SteamOS ships `KillUserProcesses=yes`). With lingering on, each supervisor runs as a transient unit of the user's systemd manager (`systemd-run --user`), outside the ssh session; without it, under `setsid`. |
| `dropped: golden lock held: <holder>` | Another golden runs on that host; the holder record names it. |
| `dropped: free space ... need ... (image ships)` | Free space under `fleet_root`, or run `clean`. |
| `local image ... was built from recipe ..., the tree's is ...` | Rebuild it: `scripts/golden-capture-container.sh image`. |
| `no local host with recorded slots in the host file; run add local first` | `add` of a remote host needs the local host's slots. |
| `local: 3 loaded legs at slots N; the machine is not idle` | The local machine was busy through three legs; rerun when it is idle. |
| `leg calib-... compared N of M blessed rows, K missing; not a full unit` | A capture leg timed out or failed; the `add` printed the tails of the leg's `capture.log` and `verdict.log` before deleting the run directory. |
| `torn sync: <host> snapshot differs from ...` | The checkout changed during the sync; rerun when no edit or build is in flight. |
| `<host>: script exited 255: ssh: connect to host ...` (or `rsync ... exited 255`) after `<host>: <operation>: connect failed 4 times, giving up` | Four connects to the host failed over about 70 s (Connect retries); check its network and sshd, then rerun. |
| `<host>: recheck of <dir> could not connect after 4 attempts: ...` | The same during the torn-sync recheck; the copy was not found torn. |
| `synced <file> hashes to ..., blessed header disc-... is ...` | The disc image differs from the one the golden was blessed on. |
| `canary: REFUSED: N of M tile(s) differ between ...` | The hosts render differently; take the named host out (`enabled = false`) until the difference is explained. |
| `REFUSED: blessed-sha256 ... differs from the local blessed.tsv` | The host decided against another manifest; the checkout changed after the sync. |
| `stream mismatch: ...` | The accepted attempt's stream and its verdict report disagree (the message names the counts and the first tiles that differ); a row that finished without its compare (`[stream] FAIL: no line for ...` in `capture.log`), or a tile changed after its row streamed. |
| `expected-moves file refused: line N: ...` | Fix the unit (a job name of this run) or the substring (it matches no blessed tile and no manifest row). |
| `<dir> kept for clean (host unreachable)` | Run `clean --hosts <host>` once it is reachable. |
