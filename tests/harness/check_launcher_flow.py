#!/usr/bin/env python3
"""check_launcher_flow.py - the launcher's flow checks, one case per ctest test of the label
`launcher`, beside check_launcher.py (whose launch helper, virtual display and scratch data root
every case here uses).

Cases:
  startup   Window-open to first present (the launcher's stamps window_shown and first_present)
            with a library of three games (one entry per game, each disc.bin a link to the
            repository's image) and a warm pipeline cache: one launch warms the cache, then
            STARTUP_RUNS launches each read both stamps. On a build with the first-boot pipeline
            each entry holds its built module, so the launch-time jobs compute every game's stamp
            and build nothing. Each stamp also carries the launcher thread's CPU time and runnable
            wait (/proc/thread-self/schedstat) and the process's CPU time (getrusage). PASS when the
            median of the launcher thread's CPU time between the two stamps is at most
            STARTUP_BOUND_MS and, on an idle host (1-minute load under STARTUP_IDLE_LOAD for every
            run), the median delta is at most STARTUP_BOUND_MS, the bound set from the figure
            measured on the build before the launch-time stamps moved off the launcher thread. Under
            load the delta grows by the time the launcher thread is blocked on lavapipe's rasterizer
            threads while they wait runnable, which its CPU time excludes. --measure prints the
            figures and sets no verdict.
  resolution       The resolved options after a scripted launcher pick (OPENPETE_LAUNCHER_SELECT_GAME) of
            a library entry, per game, against a direct --disc launch of the same entry's disc.cue:
            each run writes its resolution (OPENPETE_CONFIG_REPORT_OUT, the --config-report text and
            the disc path) and any differing option fails: the scope, the directories, each knob's
            value and the disc path (provenance lines are printed, not compared). Windowed, --frame-limit 5, no window
            change, so no window state is carried.
  multi-install       The installed globals after the final install (OPENPETE_INSTALL_DUMP: the build record,
            the eight host tables, each orchestrator slot and the interp registration, the
            registered runtime and its scalars, the glyph set's flags) of a process that installed
            every other linked game first (OPENPETE_INSTALL_OTHERS_FIRST) against a fresh process's
            single install, for each game as target, both under setarch -R so addresses match
            between processes; any differing line or a non-zero install refusal count fails. The
            single-install dumps of two games must differ (the instrument sees the install).
  multi-install-extract  The multi-install check with each other game's asset extraction job (domes, glyph
            models, logo) run inline in the `ingest` role after its install, on the repository's
            image of that game (OPENPETE_INSTALL_OTHERS_FIRST=extract, OPENPETE_ASSET_DISCS): every
            job completes with domes, a ready glyph set and a logo, and the dump equals the single
            install's; Spyro 3's image given as Spyro 2's disc fails Spyro 2's job.
  job-*     The launcher's job runner on the module rebuild, on a build with the first-boot
            pipeline (recomp/ staged beside the binary; SKIP otherwise). A launch-time job of game
            A is made by moving A's module off its stamped path. Every windowed case runs with
            OPENPETE_PRECOMPILE_AHEAD=1 and asserts a non-zero queued count.
            job-ingest-stop: A's rebuild is stopped by a drop of game B's disc at B's copy end, B's module is
            built, the selector returns, A's rebuild completes after B's build, and the pick of B
            (OPENPETE_LAUNCHER_SELECT_IDLE) starts B; no install is refused.
            job-pick-same: a pick of A during A's rebuild waits for it and starts A.
            job-pick-other: a pick of B during A's rebuild stops A, whose stamped module path then holds no
            file or a whole module; B starts.
            job-pool-low: a pick of A during A's rebuild on a cold pipeline cache (OPENPETE_PRECOMPILE_FORCE=1,
            so bring-up's compile stage runs on lavapipe): the compile pool keeps running on its
            low-priority workers through the rebuild and creates pipelines in it (a non-zero count,
            the "at low priority" line), and the game starts. Reports the rebuild's wall time, the
            pipelines bring-up then compiles and its compile stage's length, and the time from the pick
            and from the rebuild's end to the game's window (the line after the compile stage). --measure also runs the paused pool's leg
            (OPENPETE_LAUNCHER_JOB_POOL=pause) and prints both legs with the 1-minute load, no verdict.
            lock: a second process started during a headless --ingest of the first logs the lock
            message, queues no ahead compile, writes no pipeline cache and exits 3, and the first
            process's ingest completes with the whole disc copied and its module built.
            failed-marker: a build forced to fail (OPENPETE_RECOMP_BUILD_FAIL) writes the entry's
            failed-build marker and exits 3; the next launch starts no rebuild and exits 3; a launch
            of a copy of the executable (another build id) rebuilds, removes the marker and plays.
            headless-no-disc: headless, a launch naming no disc boots the most recently played entry after the
            inline launch-time check; an empty library, and --game naming a game with no entry,
            exit 3 with the missing-disc diagnostic.
  input     T11, the game page's commands (windowed, three-game library): arrow keys, the d-pad and
            the left stick's X move the focus one box without wrap (the stick re-centres between
            moves); up moves it to the Mods chip and down back to its box; Escape quits at once; East asks once ("quit ask chip
            shown"), the ask lapses after 2 s, and a second East inside 2 s quits; the pointer takes
            the focus only after it moves, so a key move holds against a still pointer; Space and
            West open the file picker for the focused box's game; Enter and South start the focused
            box's game; a disc of another game dropped on the page focuses that game's box when the
            page returns.
  mods-chip The Mods chip at the game page's top-left corner (page.mods in the manifest, also on the empty
            library's page): key up and the d-pad up reach it, down and right return to the box, left and
            up on it do nothing; Enter, South and a click each open the mods folder the loader reads
            (OPENPETE_LAUNCHER_OPEN_URL_STUB=1 logs the path and opens nothing), which is created.
  unsupported-mark  An entry whose meta.toml records a disc build other than supported (disc_build) marks its
            box with an "Unsupported disc" chip below the serial's (box.<game>.unsupported); a supported
            entry's box has none.
  exit-return  EXIT GAME (injected through OPENPETE_MOUSE_INJECT) in a session picked on the game page
            returns to the launcher, three times over a library of Spyro 1 and 2 (picks Spyro 2, Spyro 1,
            Spyro 2 by OPENPETE_LAUNCHER_SELECT_GAME's list), then the page quits: each relaunch carries
            the return count and the game exited, the page focuses that game's box, the one-shot flags
            (--frame-limit, --max-runtime-seconds) are left out once, memory cards the first Spyro 2
            session provisioned load in the second, every page runs with equal descriptor counts and
            thread counts within 2, and the three returns' timings are reported (teardown, exec, first
            present), exit 0.
  exit-return-env  EXIT GAME from Spyro 1, then from Spyro 2, with a config file whose [game.spyro-1.env] sets
            OPENPETE_NOTEX and OPENPETE_TEXPACK_DIR and whose [game.spyro-2.env] sets OPENPETE_TEXPACK_DIR
            to another value: no page's image is given either variable (/proc/<pid>/environ), and each
            relaunch names the variables it left at their entry state (both after Spyro 1,
            OPENPETE_TEXPACK_DIR without OPENPETE_NOTEX after Spyro 2), exit 0.
  exit-fullscreen  EXIT GAME from a fullscreen game, then leaving fullscreen in the relaunched launcher (Alt+Enter),
            gives a window whose position is the windowed position the window had before it went
            fullscreen, or centred on the screen when it never had one; four legs: the game opened
            fullscreen from a windowed launcher (game-fullscreen), the launcher and the game opened
            fullscreen (launcher-fullscreen), fullscreen toggled in the game (game-toggle), and a windowed
            exit, whose relaunched launcher opens at the game window's last position (windowed). Each
            leg's position is read by the launcher's geometry action; engagement: the teardown and
            return lines name the fullscreen (windowed for the last leg) window, the relaunched launcher
            logs its toggle off, exit 0.
  exit-direct  EXIT GAME in a --disc session and a directly booted --game session refuses the return and
            ends the process, exit 0.
  exit-close  A window close and SIGTERM end a session picked on the game page without a relaunch, exit 0.
  picker    T11 and D39's disc choice through the drawn picker (windowed): under gamescope
            (GAMESCOPE_WAYLAND_DISPLAY set) Space opens the drawn picker and never the file dialog;
            the pad walks it over a folder the check made (OPENPETE_LAUNCHER_PICKER_ROOTS): South
            opens a folder, East goes back up, and South on Spyro 2's cue, picked from Spyro 1's box,
            ingests it as Spyro 2 and the page returns focused on Spyro 2's box, which then plays.
            Elsewhere a file dialog whose callback reports a failure (OPENPETE_LAUNCHER_DIALOG_FAIL)
            opens the drawn picker; Escape closes it with the focus unchanged, Space opens it again
            (no request is refused), and Escape on the page then quits.
  ingest-retry  A byte-altered copy of a disc passes the serial check and fails to build: nothing calls the
            original redundant, the copy is removed, and the pad adds the good disc from the empty box, which
            plays; an older failed entry does not refuse the good disc and is removed once it has built.
  ingest-held  An image of a game the library already holds is refused before the copy, with a
            message naming the library's entry, and the library is left as it was: by a headless
            --ingest (non-zero exit), and by a drop on the selector (the selector returns and a
            pick of the held entry plays).
  bind-repair   The pick path's bind-failure rebuild: garbage at A's module path is rebuilt at the bind
            ("rebuilding it from your disc") and the game plays; the same rebuild forced to fail
            writes the failed-build marker, and the next launch exits 3 without a rebuild.
  cache     The launcher asset cache (state/openpete/cache/<game>/: domes.bin, glyphs.txt, logo.rgba,
            manifest.toml) over a three-game library, headless with no display: the cold launch
            writes every game's cache through the launch-time rerun (each output decoded back:
            domes, glyphs, logo); a warm launch writes no file and logs no rerun; a deleted output,
            an older and a newer manifest format and a changed disc hash in meta.toml each rerun
            that game alone; an extraction forced to fail (OPENPETE_ASSET_FAIL) logs the failure
            and writes the failed marker, keeping the previous outputs, and the same build skips
            it; a copy of the executable (another build id) reruns every game once, clearing the
            marker, and its second launch reruns none. After every step each manifest lists
            outputs of its sizes and no temporary is left.
  cache-ingest  A headless --ingest writes the game's cache before the entry boots (the
            assets-ready line, before the module build's end on a build with the first-boot
            pipeline) and logs no launch-time rerun.
  multi-install-launch  The multi-install check through the launcher's launch-time path: a
            headless --game pick over a library of the three games with no cache, whose
            launch-time jobs install and extract every game, dumps the installed globals equal to
            a fresh single install's, for each game as target.
  job-ingest-stop-assets  T8 (a) on the asset extraction (windowed): A's launch-time asset job
            (held by OPENPETE_ASSET_JOB_DELAY_MS) is stopped by a drop of B's disc at B's copy
            end, B's asset job runs at the ingest, A's job is requeued and completes after it.
  job-pick-other-assets  T8 (c) on the asset extraction (windowed): a pick of B during A's held
            launch-time asset job stops A, which leaves no cache file, and B's cache is written
            at the pick.
  quit-*    T13 and T12's quit during work (windowed, OPENPETE_LAUNCHER_QUIT_AT, TMPDIR in the case
            directory). A stop at each step: quit-copy (a quit event during a dropped disc's copy),
            quit-translate (SIGTERM during PSXRecomp), quit-build (a quit event while the compiles
            run; the next launch rebuilds the entry in the background, its inline launch-time job
            headless, and plays), quit-assets (a quit event while the pick waits for an asset job
            held by OPENPETE_ASSET_JOB_DELAY_MS). Each reports the quit-to-exit time (the harness's,
            from the log line to the process's exit, and the launcher's own) against QUIT_BOUND_MS plus
            the length of the preview compile the stop found in flight (the launcher's log line), and
            the leftover counts, each 0: ingest-tmp-* directories, entries, failed-build markers,
            partial modules, module build directories, *.tmp-* atomic temporaries, the stopped job's
            cache files, files in TMPDIR, live processes naming the case directory. The progress
            view's gesture, on the build step: quit-east-once (one East: the quit chip, Press again
            to quit, takes the step chip's place and goes after 2 s, the build completes and the game plays), quit-east-twice,
            quit-escape and quit-close (each quits at once, with the step case's checks).
            quit-mods: for each game, a quit event while the pick compiles a stale source-only mod
            (mods/crash-dummy under --mods-dir): the compile stops and the mod stays stale, the game
            does not start (no hand-off), the step case's checks, and the next launch compiles the mod.
            quit-page-sigterm: SIGTERM sent from outside to the idle game page ends the process,
            exit 0, within QUIT_BOUND_MS.
  shader-step-*  T8's shader step on the launcher's ahead compile (windowed, a cold pipeline
            cache, OPENPETE_PRECOMPILE_FORCE=1 so bring-up's compile stage runs on lavapipe):
            shader-step-during: a pick at the selector's first frame, over an entry whose launcher
            asset cache an earlier windowed run wrote (so no launch-time job holds the pick while the ahead
            walk runs at low priority), stops the ahead walk with create infos not returned, and the compile stage compiles pipelines and draws its
            progress screen (OPENPETE_PRECOMPILE_PANEL_MS=0); every screen's bar has a total
            and its fraction never falls (the "compile stage bar" line).
            shader-step-after: a pick held until the ahead walk has returned every create info
            (OPENPETE_LAUNCHER_PICK_AFTER_WALK=end) starts the game with every bring-up create held
            by the cache (bring-up compiles no pipeline); the progress screens drawn are reported.
            shader-step-order: with the terrain cache described (OPENPETE_TERRAIN_CACHE_FORCE=1),
            the walk's terrain group at the first texture mapping returns before the group of
            the other built-in pipelines; the pick is held until the walk logs group 2's return
            or ends (OPENPETE_LAUNCHER_PICK_AFTER_WALK=2).
  sky-cache  T7's sky (windowed): over a library of Spyro 1 and Spyro 2 whose caches a headless
            launch wrote, the selector draws the domes the cache holds with no game picked: each
            game's dome count loaded equals domes.bin's, the sky pass drew layers, the selector's
            first frame differs from the flat clear on at most SKY_FIRST_TOL of its pixels outside
            every element rect of its manifest (the sky is deferred past the first present), and the
            first frame showing one opaque dome (the shot "sky") differs from the flat clear on at
            least SKY_COVER of its pixels, overall and outside its elements; the control run with the
            cache removed and its re-extraction forced to fail (OPENPETE_ASSET_FAIL) loads no dome,
            draws no layer, writes no sky shot, and its first frame outside the elements is the clear
            within SKY_FIRST_TOL.
  sky-reveal  T8's dome join and first reveal (windowed): a drop of Spyro 1's disc on the empty
            library's drop screen joins its domes to the rotation and starts the reveal in the
            same step that reaps its assets (the sky line directly after the assets-ready line,
            no line between them but the title switch's), and the sky pass draws revealed layers.

Usage: tests/harness/check_launcher_flow.py CASE [--build-dir DIR] [--out DIR] [--measure]
Exit 0 PASS, 1 FAIL, 3 VACUOUS, 64 SKIP (binary, xvfb-run or a disc image missing).
"""
# sandbox-guard: intentional new user boot through check_launcher's per-case XDG roots.
import argparse
import fcntl
import glob
import hashlib
import math
import os
import re
import shutil
import signal
import statistics
import struct
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check_launcher as cl  # noqa: E402

ROOT = cl.ROOT
DISCS = {
    "spyro-1": ("SCUS-94228", "Spyro the Dragon"),
    "spyro-2": ("SCUS-94425", "Spyro 2: Ripto's Rage!"),
    "spyro-3": ("SCUS-94467", "Spyro: Year of the Dragon"),
}
STARTUP_RUNS = 7
# Set from a page carrying three box pictures, three logos and the shape pass (11 runs on the dev
# box under Xvfb and lavapipe at load under 0.5: median 54.3 ms, min 52.1, max 55.2): the baseline's
# median plus 10%. A page with no box pictures read 47 ms.
STARTUP_BOUND_MS = 60.0
# The 1-minute load average under which the host is idle: the wall-clock delta is bounded only below it.
# Above it the launcher thread waits blocked on lavapipe's rasterizer threads, which wait runnable, so the
# delta is the scheduler's; the launcher thread's CPU time between the two stamps, never more than the
# idle delta, is held to the same bound on every host.
STARTUP_IDLE_LOAD = 4.0


def disc_bin(game):
    """The repository's disc image of a game."""
    serial = DISCS[game][0]
    return os.path.join(ROOT, "games", game, game, "build", serial + ".bin")


def disc_cue(game):
    """The repository's cue of a game."""
    serial = DISCS[game][0]
    return os.path.join(ROOT, "games", game, game, "build", serial + ".cue")


def seed_entry(d, game, last_played, tag=None):
    """Add a library entry of a game whose disc.bin links the repository's image; return its directory."""
    b = disc_bin(game)
    if not os.path.isfile(b):
        raise cl.Skip(f"{b} missing")
    e = os.path.join(d, "data", "openpete", "library", tag or ("0000000000" + game))
    os.makedirs(e)
    os.symlink(b, os.path.join(e, "disc.bin"))
    with open(os.path.join(e, "disc.cue"), "w") as f:
        f.write('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    with open(os.path.join(e, "meta.toml"), "w") as f:
        f.write('label = "%s (launcher test)"\nsource = "%s.bin"\nsha256 = "%s"\nlast_played = %d\n'
                'game = "%s"\n' % (DISCS[game][1], DISCS[game][0], "0" * 64, last_played, game))
    return e


def stamps(log):
    """The launcher's start-up stamps in ms after process start, by name."""
    return {m.group(1): float(m.group(2))
            for m in re.finditer(r"stamp (\w+) ([\d.]+) ms after process start", log)}


def stamp_sched(log):
    """The scheduler totals each start-up stamp carries, by name: (thread id, ms the thread ran, ms it waited
    runnable, ms of the process's CPU time)."""
    return {m.group(1): (m.group(2), float(m.group(3)), float(m.group(4)), float(m.group(5)))
            for m in re.finditer(r"stamp (\w+) [\d.]+ ms after process start; thread (\d+) ran ([\d.]+) ms, "
                                 r"waited runnable ([\d.]+) ms; process CPU ([\d.]+) ms", log)}


def case_startup(a):
    """Window-open to first present over a three-game library with a warm pipeline cache: the launcher thread's
    CPU time on every host, the wall-clock delta on an idle one."""
    d = cl.case_dir(a.out, "startup")
    for i, g in enumerate(DISCS):
        # With the first-boot pipeline each entry holds its built module, so the launch-time jobs
        # compute the stamps and build nothing.
        if has_first_boot(a):
            seed_built(a, d, g, i + 1)
        else:
            seed_entry(d, g, i + 1)
    env = {"OPENPETE_LAUNCHER_INPUT": "2500:quit"}
    walls, rans, cpus, loads = [], [], [], []
    for i in range(STARTUP_RUNS + 1):
        load = os.getloadavg()[0]
        rc, log = cl.launch(a.binary, d, f"run{i}", ["--launcher-window", "1280x800"], env)
        s, k = stamps(log), stamp_sched(log)
        if rc != 0 or "window_shown" not in s or "first_present" not in s:
            print(f"[launcher-flow] startup run {i}: exit {rc}, stamps {s}: FAIL")
            return 1
        if "window_shown" not in k or "first_present" not in k or k["window_shown"][0] != k["first_present"][0]:
            print(f"[launcher-flow] startup run {i}: no scheduler totals on one thread for both stamps ({k}): FAIL")
            return 1
        if i == 0:
            continue   # warms the pipeline cache
        w0, w1 = k["window_shown"], k["first_present"]
        wall, ran, waited, cpu = s["first_present"] - s["window_shown"], w1[1] - w0[1], w1[2] - w0[2], w1[3] - w0[3]
        walls.append(wall)
        rans.append(ran)
        cpus.append(cpu)
        loads.append(load)
        print(f"[launcher-flow] startup run {i}: load {load:.1f}, window_shown {s['window_shown']:.1f} ms, "
              f"first_present {s['first_present']:.1f} ms, delta {wall:.1f} ms; process CPU {cpu:.1f} ms; "
              f"launcher thread ran {ran:.1f} ms, waited runnable {waited:.1f} ms, blocked {wall - ran - waited:.1f} ms")
    med, rmed, cmed = statistics.median(walls), statistics.median(rans), statistics.median(cpus)
    idle = max(loads) < STARTUP_IDLE_LOAD
    print(f"[launcher-flow] startup: {len(walls)} runs at load {min(loads):.1f}-{max(loads):.1f}; delta median "
          f"{med:.1f} ms, min {min(walls):.1f}, max {max(walls):.1f}; launcher thread CPU median {rmed:.1f} ms, "
          f"min {min(rans):.1f}, max {max(rans):.1f}; process CPU median {cmed:.1f} ms")
    if a.measure:
        print("[launcher-flow] startup: measured (no verdict)")
        return 0
    bad = []
    if rmed > STARTUP_BOUND_MS:
        bad.append(f"launcher thread CPU median {rmed:.1f} ms over {STARTUP_BOUND_MS} ms")
    if idle and med > STARTUP_BOUND_MS:
        bad.append(f"delta median {med:.1f} ms over {STARTUP_BOUND_MS} ms on an idle host")
    wall_note = f"delta median against {STARTUP_BOUND_MS} ms" if idle else \
        f"delta not bounded (load {max(loads):.1f} at or over {STARTUP_IDLE_LOAD})"
    print(f"[launcher-flow] startup: launcher thread CPU median against {STARTUP_BOUND_MS} ms, {wall_note}: "
          f"{'FAIL ' + '; '.join(bad) if bad else 'PASS'}")
    return 1 if bad else 0


def module_name(game):
    """The game's module file name: spyro-1 -> spyro1-recomp.so."""
    return game.replace("-", "") + "-recomp.so"


def has_first_boot(a):
    """Whether the build stages the first-boot pipeline's inputs beside the binary."""
    base = os.path.join(a.build_dir, "recomp")
    return os.path.isfile(os.path.join(base, "tools", "PSXRecomp")) and \
        all(os.path.isfile(os.path.join(base, "manifest", g, "manifest.toml")) for g in DISCS)


def need_first_boot(a):
    """Skip a case that needs the first-boot pipeline on a build without it."""
    if not has_first_boot(a):
        raise cl.Skip(f"{a.build_dir} stages no first-boot pipeline (a *-launcher preset)")


def stamped(d, game):
    """The stamped module paths under a game's seeded entry."""
    e = os.path.join(d, "data", "openpete", "library", "0000000000" + game)
    return glob.glob(os.path.join(e, "modules", "*", module_name(game)))


def headless(a, d, tag, args, env=None, timeout=900, binary=None):
    """One headless run under a virtual display (PsyCross needs one) with the case's data root."""
    return cl.launch(binary or a.binary, d, tag, ["--headless", "--mute", "1"] + args, env, timeout=timeout)


def prime(a, game):
    """Build a game's module once per out directory and return (stamp directory name, module path).

    A lock file serialises the cases that prime the same game in parallel (ctest -j)."""
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "prime-" + game + ".lock"), "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        return prime_locked(a, game)


def prime_locked(a, game):
    """prime's body, run under its lock."""
    d = os.path.join(a.out, "prime-" + game)
    # The cached module serves only the binary that built it: its stamp names that binary's inputs.
    key = os.path.join(d, "binary.mtime")
    mtime = str(os.stat(a.binary).st_mtime_ns)
    got = glob.glob(os.path.join(d, "data", "openpete", "library", "*", "modules", "*", module_name(game)))
    if got and os.path.isfile(key) and open(key).read() == mtime:
        return os.path.basename(os.path.dirname(got[0])), got[0]
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(d)
    seed_entry(d, game, 1)
    rc, log = headless(a, d, "prime", ["--game", game, "--frame-limit", "1"])
    got = stamped(d, game)
    if not got:
        raise RuntimeError(f"priming {game} built no module (exit {rc}); see {d}/prime.log")
    with open(key, "w") as f:
        f.write(mtime)
    return os.path.basename(os.path.dirname(got[0])), got[0]


def seed_built(a, d, game, last_played):
    """Seed an entry of a game holding its built module at the stamped path; return the module path."""
    stamp, src = prime(a, game)
    e = seed_entry(d, game, last_played)
    dst = os.path.join(e, "modules", stamp, module_name(game))
    os.makedirs(os.path.dirname(dst))
    shutil.copy2(src, dst)
    return dst


def ordered(log, *pats):
    """Problems with the order of first matches of patterns in a log; each must occur after the one before."""
    bad, at = [], -1
    for p in pats:
        m = re.search(p, log[at + 1:])
        if not m:
            bad.append(f"no {p!r} after offset {at + 1}")
            return bad
        at = at + 1 + m.start()
    return bad


def whole_module(path):
    """Whether a file is a module the loader can bind: an ELF exporting openpete_game_recomp_module."""
    if not os.path.isfile(path):
        return False
    out = subprocess.run(["nm", "-D", "--defined-only", path], capture_output=True, text=True).stdout
    return "openpete_game_recomp_module" in out


def common_flow(log):
    """Problems every windowed flow run shares: the ahead compile queued, no install refused."""
    bad = []
    m = re.search(r"launcher precompile: \d+ pipeline descriptions, (\d+) create infos queued", log)
    if not m or int(m.group(1)) == 0:
        bad.append("VACUOUS: the ahead compile queued nothing (OPENPETE_PRECOMPILE_AHEAD)")
    if "install of" in log and "refused" in log:
        bad.append("an install was refused while a job was in flight")
    return bad


def verdict(name, bad, detail=""):
    """Print and return a case's verdict."""
    print(f"[launcher-flow] {name}: {'FAIL ' + '; '.join(bad) if bad else 'PASS'}{(' (' + detail + ')') if detail else ''}")
    return 1 if bad else 0


FLOW_ENV = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_PICK_DELAY_MS": "1500"}


def case_job_ingest_stop(a):
    """A's rebuild stopped by B's ingest at B's copy end, completed after B's build."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "job-ingest-stop")
    mod = seed_built(a, d, "spyro-1", 1)
    os.rename(mod, mod + ".off")
    env = dict(FLOW_ENV, OPENPETE_LAUNCHER_DROP=disc_cue("spyro-2"),
               OPENPETE_LAUNCHER_SELECT_GAME="spyro-2", OPENPETE_LAUNCHER_SELECT_IDLE="1")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted drop", r"\[library\] added",
        r"stopping the launch-time job of spyro-1 \(requeued\)", r"module build stopped",
        r"launcher-side module build of spyro-2", r"first-boot module build of spyro-2",
        r"launch-time job started: spyro-1", r"module rebuild of spyro-1 done",
        r"scripted pick: entry \d+ \(spyro-2\)", r"game picked: spyro-2")
    if rc != 0:
        bad.append(f"exit {rc}")
    if not whole_module(mod):
        bad.append(f"A's module {mod} is not whole after its rebuild")
    return verdict("job-ingest-stop", bad)


def case_job_pick_same(a):
    """A pick of A during A's rebuild shows its progress and continues to the game."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "job-pick-same")
    mod = seed_built(a, d, "spyro-1", 1)
    os.rename(mod, mod + ".off")
    env = dict(FLOW_ENV, OPENPETE_LAUNCHER_SELECT_GAME="spyro-1")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted pick: entry \d+ \(spyro-1\)",
        r"the pick of spyro-1 waits for its running launch-time job", r"module rebuild of spyro-1 done",
        r"game picked: spyro-1", r"shared recomp module attached")
    if "first-boot module build of spyro-1" in log.split("module rebuild of spyro-1 done", 1)[-1]:
        bad.append("the module was built again after the joined rebuild")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-pick-same", bad)


def case_job_pick_other(a):
    """A pick of B during A's rebuild stops A, which leaves no partial module."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "job-pick-other")
    mod = seed_built(a, d, "spyro-1", 1)
    seed_built(a, d, "spyro-2", 2)
    os.rename(mod, mod + ".off")
    env = dict(FLOW_ENV, OPENPETE_LAUNCHER_SELECT_GAME="spyro-2")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted pick: entry \d+ \(spyro-2\)",
        r"stopping the launch-time job of spyro-1", r"game picked: spyro-2")
    stopped = "module build stopped" in log
    partial = os.path.exists(mod) and not whole_module(mod)
    if partial:
        bad.append(f"A's module path holds a partial file {mod}")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-pick-other", bad, f"A {'stopped' if stopped else 'had finished'}; module "
                               f"{'absent' if not os.path.exists(mod) else 'whole'}")


POOL_LOW = re.compile(r"launcher precompile: (\d+) pipelines created \((\d+) jobs run\) at low priority in ([\d.]+) ms")
POOL_PAUSED = re.compile(r"the compile pool resumes after ([\d.]+) ms paused; (\d+) pipelines created in it")
# The game's window is up: bring-up's compile stage has ended and the game's own presents follow.
GAME_WINDOW_UP = r"native: Vulkan vk window up"


def pool_leg(a, name, extra):
    """One pick of A during A's launch-time rebuild on a cold pipeline cache; returns (rc, log, figures)."""
    d = cl.case_dir(a.out, name)
    mod = seed_built(a, d, "spyro-1", 1)
    os.rename(mod, mod + ".off")
    env = dict(OPENPETE_PRECOMPILE_AHEAD="1", OPENPETE_PRECOMPILE_FORCE="1",
               OPENPETE_LAUNCHER_PICK_DELAY_MS="1500", OPENPETE_LAUNCHER_SELECT_GAME="spyro-1", **extra)
    load = os.getloadavg()[0]
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=1500)
    t = {k: log_time(log, p) for k, p in (("start", r"launcher: launch-time module rebuild of spyro-1"),
                                           ("done", r"launcher: module rebuild of spyro-1 done"),
                                           ("pick", r"launcher: scripted pick: entry \d+ \(spyro-1\)"),
                                           ("frame", GAME_WINDOW_UP))}
    st, low, paused = STAGE.search(log), POOL_LOW.search(log), POOL_PAUSED.search(log)
    f = {"load": load, "load_end": os.getloadavg()[0], "low": low, "paused": paused, "stage": st, "t": t,
         "rebuild_s": t["done"] - t["start"] if t["done"] and t["start"] else None,
         "pick_frame_s": t["frame"] - t["pick"] if t["frame"] and t["pick"] else None,
         "done_frame_s": t["frame"] - t["done"] if t["frame"] and t["done"] else None,
         "made": int(low.group(1)) if low else int(paused.group(2)) if paused else None,
         "bringup": int(st.group(3)) if st else None, "stage_ms": float(st.group(1)) if st else None}
    return rc, log, f


def pool_detail(f):
    """One leg's figures as text."""
    def s(v):
        return "?" if v is None else f"{v:.2f}"
    return (f"rebuild {s(f['rebuild_s'])} s, {f['made']} pipelines created in it; bring-up compiled "
            f"{f['bringup']} (compile stage {s(f['stage_ms'])} ms); pick to the game's window {s(f['pick_frame_s'])} s, "
            f"rebuild end to the game's window {s(f['done_frame_s'])} s; load {f['load']:.1f}-{f['load_end']:.1f}")


def case_job_pool_low(a):
    """A pick of A during A's rebuild: the compile pool runs at low priority through the rebuild and creates pipelines."""
    need_first_boot(a)
    rc, log, f = pool_leg(a, "job-pool-low", {})
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1",
        r"scripted pick: entry \d+ \(spyro-1\)", r"the pick of spyro-1 waits for its running launch-time job",
        r"pipelines created \(\d+ jobs run\) at low priority", r"module rebuild of spyro-1 done",
        r"game picked: spyro-1", r"shared recomp module attached")
    if "compile pool: low mode" not in log.split("module rebuild of spyro-1 done", 1)[0]:
        bad.append("the compile pool did not enter its low mode before the rebuild's end")
    workers = re.search(r"compile pool: (\d+) low-priority worker", log)
    if not workers or int(workers.group(1)) == 0:
        bad.append("no low-priority worker started")
    if not f["low"]:
        bad.append("no low-priority compile line")
    elif int(f["low"].group(1)) == 0:
        bad.append("VACUOUS: the pool created no pipeline during the rebuild")
    if "compile pool resumes after" in log.split("module rebuild of spyro-1 done", 1)[0]:
        bad.append("the compile pool was paused during the launch-time rebuild")
    if rc != 0:
        bad.append(f"exit {rc}")
    if not a.measure:
        return verdict("job-pool-low", bad, pool_detail(f))
    rc2, log2, f2 = pool_leg(a, "job-pool-paused", {"OPENPETE_LAUNCHER_JOB_POOL": "pause"})
    print(f"[launcher-flow] job-pool-low: low priority: {pool_detail(f)}")
    print(f"[launcher-flow] job-pool-low: paused:       {pool_detail(f2)} (exit {rc2})")
    print("[launcher-flow] job-pool-low: measured (no verdict)" + (f"; low leg problems: {'; '.join(bad)}" if bad else ""))
    return 0


def case_lock(a):
    """A second process during an ingest is refused by the library lock."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "lock")
    res = {}

    def first():
        res["first"] = headless(a, d, "first", ["--ingest", disc_cue("spyro-1"), "--frame-limit", "1"])

    t = threading.Thread(target=first)
    t.start()
    tmp = os.path.join(d, "data", "openpete", "library")
    deadline = time.time() + 60
    while time.time() < deadline and not glob.glob(os.path.join(tmp, "ingest-tmp-*", "disc.bin")):
        time.sleep(0.05)
    copying = bool(glob.glob(os.path.join(tmp, "ingest-tmp-*", "disc.bin")))
    rc2, log2 = cl.launch(a.binary, d, "second", ["--launcher-window", "1280x800"],
                          {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_INPUT": "3000:quit"})
    cache = os.path.join(d, "state", "openpete", "cache")
    # The pipeline cache lies in the leaf itself; the per-game directories hold the first process's
    # launcher asset cache, which its ingest writes.
    cache_files = [f for f in glob.glob(os.path.join(cache, "*")) if os.path.isfile(f)]
    t.join()
    rc1, log1 = res["first"]
    bad = []
    if not copying:
        bad.append("VACUOUS: the first process's copy was not seen in progress")
    if rc2 != 3:
        bad.append(f"second process exit {rc2}, not 3")
    if "another OpenPete is preparing a game" not in log2:
        bad.append("second process did not log the lock message")
    bad += [f"second process: {x}" for x in ordered(
        log2, r"pipeline cache: in memory only", r"error panel raised \(its dismissal quits\): Another OpenPete",
        r"error panel drawn", r"scripted input: quit", r"error panel dismissed \(quit\); the launcher quits")]
    if "launcher precompile:" in log2:
        bad.append("second process started the ahead compile")
    if "launch-time job" in log2 or "first-boot module build" in log2:
        bad.append("second process ran a job")
    if cache_files:
        bad.append(f"pipeline cache written: {cache_files}")
    if rc1 != 0:
        bad.append(f"first process exit {rc1}")
    entries = [e for e in glob.glob(os.path.join(tmp, "*", "disc.bin"))]
    if len(entries) != 1 or os.path.getsize(entries[0]) != os.path.getsize(disc_bin("spyro-1")):
        bad.append(f"the first process's copy is not whole: {entries}")
    if not stamped_any(tmp, "spyro-1"):
        bad.append("the first process built no module")
    return verdict("lock", bad)


def stamped_any(lib, game):
    """The stamped module paths of a game under any entry of a library."""
    return glob.glob(os.path.join(lib, "*", "modules", "*", module_name(game)))


def exe_copy(a, d):
    """A copy of the binary (another build id: its own modification time) with the build's recomp/ inputs linked beside it."""
    x = os.path.join(d, "exe-copy")
    os.makedirs(x)
    shutil.copy(a.binary, os.path.join(x, "openpete"))
    src = os.path.abspath(a.build_dir)
    for name in os.listdir(src):
        if name in ("recomp", "sdk", "assets", "openpete_xvfb_wm") or name.endswith(".so"):
            os.symlink(os.path.join(src, name), os.path.join(x, name))
    return os.path.join(x, "openpete")


def case_failed_marker(a):
    """A forced build failure writes the marker; the same build skips it; another build retries."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "failed-marker")
    e = seed_entry(d, "spyro-1", 1)
    marker = os.path.join(e, "build-failed.toml")
    args = ["--game", "spyro-1", "--frame-limit", "1"]
    rc1, log1 = headless(a, d, "fail", args, {"OPENPETE_RECOMP_BUILD_FAIL": "1"})
    bad = []
    if rc1 != 3 or not os.path.isfile(marker):
        bad.append(f"forced failure: exit {rc1}, marker {'present' if os.path.isfile(marker) else 'absent'}")
    rc2, log2 = headless(a, d, "same-build", args)
    if rc2 != 3 or "first-boot module build" in log2 or "launch-time job skipped" not in log2:
        bad.append(f"same build: exit {rc2}, rebuild {'ran' if 'first-boot module build' in log2 else 'not run'}")
    other = exe_copy(a, d)
    rc3, log3 = headless(a, d, "other-build", args, binary=other)
    if rc3 != 0 or "launch-time module rebuild of spyro-1" not in log3 or os.path.isfile(marker):
        bad.append(f"another build: exit {rc3}, marker {'present' if os.path.isfile(marker) else 'removed'}")
    return verdict("failed-marker", bad)


def case_headless_no_disc(a):
    """Headless launches naming no disc."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "headless-no-disc")
    seed_built(a, d, "spyro-1", 5)
    seed_built(a, d, "spyro-2", 1)
    rc, log = headless(a, d, "no-disc", ["--frame-limit", "1"])
    bad = ordered(log, r"launch-time check: the module of spyro-1 is current",
                  r"booting library entry 0000000000spyro-1", r"game picked: spyro-1")
    if rc != 0:
        bad.append(f"no-disc exit {rc}")
    diag = "no game found - pass --disc PATH"
    e = os.path.join(d, "empty")
    os.makedirs(e)
    rc, log = headless(a, e, "empty", ["--frame-limit", "1"])
    if rc != 3 or diag not in log:
        bad.append(f"empty library: exit {rc}, diagnostic {'present' if diag in log else 'absent'}")
    g = os.path.join(d, "game-no-entry")
    os.makedirs(g)
    seed_built(a, g, "spyro-1", 1)
    rc, log = headless(a, g, "game", ["--game", "spyro-3", "--frame-limit", "1"])
    if rc != 3 or diag not in log:
        bad.append(f"--game with no entry: exit {rc}, diagnostic {'present' if diag in log else 'absent'}")
    return verdict("headless-no-disc", bad)


def case_bind_repair(a):
    """A module that fails to bind is rebuilt behind the pick; a failed rebuild leaves the marker."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "bind-repair")
    mod = seed_built(a, d, "spyro-1", 1)
    marker = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(mod))), "build-failed.toml")
    with open(mod, "wb") as f:
        f.write(b"\x7fELF not a recomp module")
    rc, log = headless(a, d, "repair", ["--frame-limit", "1"])
    bad = []
    if rc != 0 or "rebuilding it from your disc" not in log or not whole_module(mod):
        bad.append(f"repair: exit {rc}, module {'whole' if whole_module(mod) else 'not whole'}")
    with open(mod, "wb") as f:
        f.write(b"\x7fELF not a recomp module")
    rc, log = headless(a, d, "repair-fail", ["--frame-limit", "1"], {"OPENPETE_RECOMP_BUILD_FAIL": "1"})
    if rc == 0 or "rebuilding it from your disc" not in log or not os.path.isfile(marker):
        bad.append(f"forced failure: exit {rc}, marker {'present' if os.path.isfile(marker) else 'absent'}")
    rc, log = headless(a, d, "after", ["--frame-limit", "1"])
    if rc != 3 or "rebuilding it from your disc" in log or "first-boot module build" in log:
        bad.append(f"next launch: exit {rc}, rebuild {'ran' if 'first-boot module build' in log else 'not run'}")
    return verdict("bind-repair", bad)


def report_lines(path):
    """A resolution report's lines, or None when it was not written."""
    if not os.path.isfile(path):
        return None
    with open(path) as f:
        return f.read().splitlines()


def resolved_options(lines):
    """The options a resolution report states: the scope, the directories, each knob's value (by knob)
    and the disc the run starts with. A knob's provenance ("won at", "shadowed") and the
    identification source are not options: the --disc launch names its disc on the command line
    (a `disc` knob row from cli) and identifies by disc, the pick identifies by its library entry;
    the disc row's value is compared through the disc path."""
    out, in_table = {}, False
    for l in lines:
        t = l.split()
        if not t:
            in_table = False
            continue
        if t[0] == "knob" and "won" in t:
            in_table = True
            continue
        if in_table:
            if t[0] != "disc":
                out["knob " + t[0]] = t[1] if len(t) > 1 else ""
        elif l.startswith("disc_path ") or l.strip().startswith(("scope:", "files:", "save target:", "layout:")):
            out[l.split(":")[0].strip() if ":" in l.split()[0] else t[0]] = l.strip()
        elif l.startswith("  ") and len(t) == 2:
            out["dir " + t[0]] = t[1]
    return out


def case_resolution(a):
    """The launcher pick's resolved options against a direct --disc launch of the same entry."""
    d = cl.case_dir(a.out, "resolution")
    bad, compared = [], 0
    for i, g in enumerate(DISCS):
        e = seed_entry(d, g, i + 1)
        pick = os.path.join(d, g + ".pick.txt")
        direct = os.path.join(d, g + ".disc.txt")
        rc1, _ = cl.launch(a.binary, d, g + "-pick", ["--frame-limit", "5"],
                           {"OPENPETE_LAUNCHER_SELECT_GAME": g, "OPENPETE_CONFIG_REPORT_OUT": pick}, timeout=300)
        rc2, _ = cl.launch(a.binary, d, g + "-disc", ["--disc", os.path.join(e, "disc.cue"), "--frame-limit", "5"],
                           {"OPENPETE_CONFIG_REPORT_OUT": direct}, timeout=300)
        p, q = report_lines(pick), report_lines(direct)
        if rc1 != 0 or rc2 != 0 or p is None or q is None:
            bad.append(f"{g}: exits {rc1}/{rc2}, reports {p is not None}/{q is not None}")
            continue
        po, qo = resolved_options(p), resolved_options(q)
        keys = sorted(set(po) | set(qo))
        diff = [k for k in keys if po.get(k) != qo.get(k)]
        compared += len(keys)
        for k in diff:
            bad.append(f"{g}: {k}: pick {po.get(k)!r} vs disc {qo.get(k)!r}")
        prov = [x for x in set(p) ^ set(q) if "identified by" in x or x.split()[:1] == ["disc"]]
        print(f"[launcher-flow] resolution {g}: {len(keys)} options, {len(diff)} differ; provenance lines "
              f"differing (not options): {sorted(x.strip()[:60] for x in prov)}")
    if compared == 0:
        bad.append("VACUOUS: no line compared")
    return verdict("resolution", bad, f"{compared} lines compared")


def install_dump(a, d, tag, game, others, extra=None):
    """One process's install dump for a target game; with `extra`, also its log text."""
    out = os.path.join(d, tag + ".dump")
    log = os.path.join(d, tag + ".log")
    for f in (out, log):
        if os.path.exists(f):
            os.remove(f)
    env = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
    env.update({"OPENPETE_INSTALL_DUMP": out, "XDG_DATA_HOME": os.path.join(d, "data"),
                "XDG_STATE_HOME": os.path.join(d, "state"), "XDG_CONFIG_HOME": os.path.join(d, "config")})
    if others:
        env["OPENPETE_INSTALL_OTHERS_FIRST"] = "1"
    env.update(extra or {})
    with open(log, "w") as f:
        cl.run_session(["setarch", os.uname().machine, "-R", a.binary, "--no-config", "--config-report",
                        "--game", game], env, f, 300, stdout=subprocess.DEVNULL)
    if extra is None:
        return report_lines(out)
    with open(log, errors="replace") as f:
        return report_lines(out), f.read()


def disc_sha256(a, game):
    """The content hash of a game's repository image, cached beside the case outputs by the image's mtime."""
    b = disc_bin(game)
    if not os.path.isfile(b):
        raise cl.Skip(f"{b} missing")
    os.makedirs(a.out, exist_ok=True)
    key = os.path.join(a.out, f"sha256-{game}.txt")
    stamp = str(os.stat(b).st_mtime_ns)
    if os.path.isfile(key):
        got = open(key).read().split()
        if len(got) == 2 and got[0] == stamp:
            return got[1]
    h = hashlib.sha256()
    with open(b, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 22), b""):
            h.update(chunk)
    with open(key, "w") as f:
        f.write(stamp + " " + h.hexdigest() + "\n")
    return h.hexdigest()


def case_multi_install(a):
    """Multi-install against single install, for each game as target."""
    if not shutil.which("setarch"):
        raise cl.Skip("setarch not found")
    d = cl.case_dir(a.out, "multi-install")
    bad, compared, singles = [], 0, {}
    for g in DISCS:
        one = install_dump(a, d, g + ".single", g, False)
        many = install_dump(a, d, g + ".multi", g, True)
        if not one or not many:
            bad.append(f"{g}: dump missing")
            continue
        singles[g] = one
        diff = [(x, y) for x, y in zip(one, many) if x != y]
        if len(one) != len(many):
            diff.append((len(one), len(many)))
        compared += len(one)
        bad += [f"{g}: single {str(x)[:90]!r} vs multi {str(y)[:90]!r}" for x, y in diff]
        refused = [l for l in many if l.startswith("install_refusals ") and l != "install_refusals 0"]
        bad += [f"{g}: {l}" for l in refused]
        print(f"[launcher-flow] multi-install {g}: {len(one)} lines, {len(diff)} differ")
    if len(singles) >= 2:
        a1, a2 = list(singles.values())[:2]
        control = sum(1 for x, y in zip(a1, a2) if x != y)
        print(f"[launcher-flow] multi-install control: two games' single installs differ on {control} lines")
        if control == 0:
            bad.append("VACUOUS: two games' single-install dumps are identical")
    return verdict("multi-install", bad, f"{compared} lines compared")


def case_multi_install_extract(a):
    """The multi-install check with each other game's asset extraction job run after its install, and an unknown disc refused."""
    if not shutil.which("setarch"):
        raise cl.Skip("setarch not found")
    d = cl.case_dir(a.out, "multi-install-extract")
    shas = {g: disc_sha256(a, g) for g in DISCS}
    discs = ";".join(f"{g}={shas[g]}={disc_cue(g)}" for g in DISCS)
    bad, compared = [], 0
    for g in DISCS:
        one = install_dump(a, d, g + ".single", g, False)
        many, log = install_dump(a, d, g + ".extract", g, True,
                                 {"OPENPETE_INSTALL_OTHERS_FIRST": "extract", "OPENPETE_ASSET_DISCS": discs})
        if not one or not many:
            bad.append(f"{g}: dump missing")
            continue
        for o in DISCS:
            if o == g:
                continue
            m = re.search(r"asset extraction of %s: (\d+) domes \((\d+) vertices, (\d+) triangles\), "
                          r"glyphs (\w+), logo (\d+)x(\d+)" % re.escape(o), log)
            if "asset extraction of %s: ok" % o not in log or not m or int(m.group(1)) == 0 \
                    or m.group(4) != "ready" or int(m.group(5)) == 0:
                bad.append(f"{g}: extraction of {o} did not complete ({m.group(0) if m else 'no result line'})")
            else:
                print(f"[launcher-flow] multi-install-extract {g}: {m.group(0)}")
        diff = [(x, y) for x, y in zip(one, many) if x != y]
        if len(one) != len(many):
            diff.append((len(one), len(many)))
        compared += len(one)
        bad += [f"{g}: single {str(x)[:90]!r} vs extract {str(y)[:90]!r}" for x, y in diff]
        bad += [f"{g}: {l}" for l in many if l.startswith("install_refusals ") and l != "install_refusals 0"]
        print(f"[launcher-flow] multi-install-extract {g}: {len(one)} lines, {len(diff)} differ")
    # A disc whose asset inputs are not the known disc's fails the job: Spyro 3's image as Spyro 2's.
    wrong = discs.replace(f"spyro-2={shas['spyro-2']}={disc_cue('spyro-2')}",
                          f"spyro-2={shas['spyro-3']}={disc_cue('spyro-3')}")
    _, log = install_dump(a, d, "wrong-disc", "spyro-1", True,
                          {"OPENPETE_INSTALL_OTHERS_FIRST": "extract", "OPENPETE_ASSET_DISCS": wrong})
    m = re.search(r"asset extraction of spyro-2 from .* failed: (.*)", log)
    if "asset extraction of spyro-2: failed" not in log or not m:
        bad.append("Spyro 3's image as Spyro 2's disc did not fail Spyro 2's job")
    else:
        print(f"[launcher-flow] multi-install-extract: Spyro 3's image as Spyro 2's disc fails: {m.group(1)}")
    return verdict("multi-install-extract", bad, f"{compared} lines compared")


def library_state(d):
    """The library's entries (any ingest-tmp directory included) and each entry's meta.toml lines but last_played
    and the disc build lines."""
    lib = os.path.join(d, "data", "openpete", "library")
    out = {}
    for e in sorted(os.listdir(lib)):
        if e.startswith("."):
            continue   # the lock file the run takes
        m = os.path.join(lib, e, "meta.toml")
        # last_played is the pick's stamp of a play, and disc_build / disc_build_warned the library scan's
        # classification of an entry that had none: neither is a change the ingest made.
        out[e] = [l for l in open(m).read().splitlines()
                  if not l.startswith(("last_played", "disc_build"))] if os.path.isfile(m) else None
    return out


def case_ingest_held(a):
    """An ingest of a game the library already holds is refused, by --ingest and by a drop."""
    d = cl.case_dir(a.out, "ingest-held")
    seed_entry(d, "spyro-1", 1)
    before = library_state(d)
    bad = []
    rc, log = headless(a, d, "ingest", ["--ingest", disc_cue("spyro-1"), "--frame-limit", "1"])
    if rc == 0 or "the library already holds a disc of spyro-1" not in log:
        bad.append(f"--ingest: exit {rc}, refusal {'logged' if 'already holds' in log else 'absent'}")
    if library_state(d) != before:
        bad.append(f"--ingest changed the library: {sorted(library_state(d))}")
    env = {"OPENPETE_LAUNCHER_DROP": disc_cue("spyro-1"), "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
           "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1"}
    rc, log = cl.launch(a.binary, d, "drop", ["--launcher-window", "1280x800", "--frame-limit", "5"], env,
                        timeout=600)
    bad += ordered(log, r"scripted drop", r"ingest of .* refused", r"scripted pick: entry \d+ \(spyro-1\)",
                   r"game picked: spyro-1")
    if rc != 0:
        bad.append(f"drop: exit {rc}")
    if library_state(d) != before:
        bad.append(f"the drop changed the library: {sorted(library_state(d))}")
    return verdict("ingest-held", bad)


# ---- the launcher asset cache (cache-*, multi-install-launch, job-*-assets) ----

CACHE_FILES = ("domes.bin", "glyphs.txt", "logo.rgba", "manifest.toml")


def nox(a, d, tag, args, env=None, timeout=600, binary=None):
    """One headless run with no display, the case's data root and no inherited OPENPETE_* knob; returns (rc, log)."""
    e = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
    e.update({"XDG_DATA_HOME": os.path.join(d, "data"), "XDG_STATE_HOME": os.path.join(d, "state"),
              "XDG_CONFIG_HOME": os.path.join(d, "config"), "OPENPETE_MSGBOX_SKIP": "1"})
    e.update(env or {})
    log = os.path.join(d, tag + ".log")
    if os.path.exists(log):
        os.remove(log)
    cmd = [binary or a.binary, "--no-config", "--no-card", "--vibration", "0", "--mute", "1", "--headless"] + args
    with open(log, "w") as f:
        rc = cl.run_session(cmd, e, f, timeout)
    with open(log, errors="replace") as f:
        return rc, f.read()


def cache_dir(d, game):
    """A game's launcher asset cache directory under the case's state root."""
    return os.path.join(d, "state", "openpete", "cache", game)


def manifest(d, game):
    """A game's cache manifest as (keys, files), or None when it does not exist."""
    p = os.path.join(cache_dir(d, game), "manifest.toml")
    if not os.path.isfile(p):
        return None
    keys, files, in_files = {}, {}, False
    for l in open(p).read().splitlines():
        if l == "[files]":
            in_files = True
        elif in_files and " = " in l:
            k, v = l.split(" = ", 1)
            files[k.strip('"')] = int(v)
        elif " = " in l:
            k, v = l.split(" = ", 1)
            keys[k] = v.strip('"')
    return keys, files


def cache_snapshot(d):
    """Every file under the cache leaf's game directories: path -> (inode, mtime_ns, size)."""
    out = {}
    for g in DISCS:
        for f in glob.glob(os.path.join(cache_dir(d, g), "*")):
            st = os.stat(f)
            out[f] = (st.st_ino, st.st_mtime_ns, st.st_size)
    return out


def written(before, after):
    """The cache files a run created or replaced."""
    return sorted(f for f, v in after.items() if before.get(f) != v)


def cache_problems(d, game):
    """Problems with a game's cache: a missing output, a size differing from the manifest, a temporary left."""
    bad = []
    m = manifest(d, game)
    if m is None:
        return [f"{game}: no manifest"]
    keys, files = m
    for f in CACHE_FILES[:-1]:
        p = os.path.join(cache_dir(d, game), f)
        if not os.path.isfile(p) or files.get(f) != os.path.getsize(p):
            bad.append(f"{game}: {f} {'missing' if not os.path.isfile(p) else 'size differs from the manifest'}")
    tmp = [f for f in os.listdir(cache_dir(d, game)) if ".tmp" in f]
    if tmp:
        bad.append(f"{game}: temporaries left {tmp}")
    return bad


def domes_count(d, game):
    """The dome count and total vertex count domes.bin holds."""
    b = open(os.path.join(cache_dir(d, game), "domes.bin"), "rb").read()
    if b[:8] != b"OPDOMES1":
        return 0, 0
    n = int.from_bytes(b[8:12], "little")
    at, nv_sum = 12, 0
    for _ in range(n):
        ln = b[at]
        at += 1 + ln
        nv, nt = int.from_bytes(b[at:at + 4], "little"), int.from_bytes(b[at + 4:at + 8], "little")
        at += 12 + 3 + nv * 9 + nt * 6
        nv_sum += nv
    return (n, nv_sum) if at == len(b) else (0, 0)


def logo_size(d, game):
    """The logo's width and height logo.rgba holds, (0, 0) when malformed."""
    b = open(os.path.join(cache_dir(d, game), "logo.rgba"), "rb").read()
    w, h = int.from_bytes(b[8:12], "little"), int.from_bytes(b[12:16], "little")
    return (w, h) if b[:8] == b"OPLOGO01" and len(b) == 16 + w * h * 4 else (0, 0)


def reruns(log):
    """The launch-time rerun lines of a log: (game, trigger)."""
    return re.findall(r"launch-time rerun: asset extraction of (\S+) \((.*)\)", log)


def seed_library(d):
    """A library of one entry per game (the repository's images); return {game: entry dir}."""
    return {g: seed_entry(d, g, i + 1) for i, g in enumerate(DISCS)}


def rewrite(path, fn):
    """Replace a file's text with fn(text)."""
    with open(path) as f:
        text = f.read()
    with open(path, "w") as f:
        f.write(fn(text))


def run_step(a, d, tag, bad, env=None, binary=None, args=None):
    """One headless pick of Spyro 1 over the case's library; returns (log, files written, reruns)."""
    before = cache_snapshot(d)
    rc, log = nox(a, d, tag, args or ["--game", "spyro-1", "--frame-limit", "1"], env, binary=binary)
    w, r = written(before, cache_snapshot(d)), reruns(log)
    if rc != 0:
        bad.append(f"{tag}: exit {rc}")
    for g in DISCS:
        bad += [f"{tag}: {x}" for x in cache_problems(d, g) if manifest(d, g) is not None]
    print(f"[launcher-flow] cache {tag}: exit {rc}, {len(w)} cache files written, {len(r)} reruns {r}")
    return log, w, r


def expect_reruns(bad, tag, r, want):
    """Check a step's reruns against {game: trigger substring}."""
    got = {g: t for g, t in r}
    if len(r) != len(want) or any(g not in got or s not in got[g] for g, s in want.items()):
        bad.append(f"{tag}: reruns {r}, expected {want}")


def cache_cold(a, d, bad):
    """The cold launch: every library game's cache written, each output decoded back."""
    log, w, r = run_step(a, d, "cold", bad)
    expect_reruns(bad, "cold", r, {g: "no cache manifest" for g in DISCS})
    for g in DISCS:
        n, nv = domes_count(d, g)
        lw, lh = logo_size(d, g)
        glyph = os.path.getsize(os.path.join(cache_dir(d, g), "glyphs.txt"))
        print(f"[launcher-flow] cache {g}: {n} domes ({nv} vertices), glyphs.txt {glyph} bytes, logo {lw}x{lh}")
        if n == 0 or nv == 0 or glyph == 0 or lw == 0:
            bad.append(f"cold: {g}: outputs empty or malformed ({n} domes, logo {lw}x{lh})")
        if f"launcher assets of {g} ready (launch-time)" not in log:
            bad.append(f"cold: {g}: no assets-ready line")
    if len(w) != 4 * len(DISCS):
        bad.append(f"cold: {len(w)} cache files written, expected {4 * len(DISCS)}")


def cache_rewrites(a, d, entries, bad):
    """A deleted output, an older and a newer format and a changed disc hash each rerun that game alone."""
    os.remove(os.path.join(cache_dir(d, "spyro-1"), "glyphs.txt"))
    _, w, r = run_step(a, d, "deleted", bad)
    expect_reruns(bad, "deleted", r, {"spyro-1": "glyphs.txt missing"})
    for g, fmt, why in (("spyro-2", "0", "manifest format 0, this build writes"),
                        ("spyro-3", "99", "newer than this build's")):
        p = os.path.join(cache_dir(d, g), "manifest.toml")
        rewrite(p, lambda t: re.sub(r"^format = \d+", "format = " + fmt, t, flags=re.M))
        _, w, r = run_step(a, d, "format-" + fmt, bad)
        expect_reruns(bad, "format-" + fmt, r, {g: why})
        if manifest(d, g)[0].get("format") != "2":
            bad.append(f"format-{fmt}: {g}'s manifest not rewritten in format 2")
    meta = os.path.join(entries["spyro-1"], "meta.toml")
    rewrite(meta, lambda t: t.replace('sha256 = "' + "0" * 64, 'sha256 = "' + "1" * 64))
    _, w, r = run_step(a, d, "hash", bad)
    expect_reruns(bad, "hash", r, {"spyro-1": "hash changed"})
    if manifest(d, "spyro-1")[0].get("disc_sha256") != "1" * 64:
        bad.append("hash: spyro-1's manifest does not name the new hash")


def cache_failed(a, d, bad):
    """A forced extraction failure writes the failed marker; this build skips it; another build reruns."""
    # A rerun is due (outputs of another build) so the forced failure is reached with the outputs recorded.
    p = os.path.join(cache_dir(d, "spyro-2"), "manifest.toml")
    rewrite(p, lambda t: re.sub(r'^build = ".*"$', 'build = "an older build"', t, flags=re.M))
    before = {f: os.path.getsize(os.path.join(cache_dir(d, "spyro-2"), f)) for f in CACHE_FILES[:-1]}
    log, w, r = run_step(a, d, "fail", bad, {"OPENPETE_ASSET_FAIL": "spyro-2"})
    m = manifest(d, "spyro-2")
    fails = re.findall(r"asset extraction of spyro-2 failed: (.*?);", log)
    if not fails or not m or not m[0].get("failed_build"):
        bad.append(f"fail: log lines {len(fails)}, marker {'present' if m and m[0].get('failed_build') else 'absent'}")
    else:
        print(f"[launcher-flow] cache fail: 1 failure line ({fails[0]}), marker build {m[0]['failed_build']}")
    if any(os.path.getsize(os.path.join(cache_dir(d, "spyro-2"), f)) != n for f, n in before.items()):
        bad.append("fail: the failure changed spyro-2's previous outputs")
    log, w, r = run_step(a, d, "fail-same-build", bad)
    if r or w or "asset extraction of spyro-2 skipped" not in log:
        bad.append(f"fail-same-build: reruns {r}, {len(w)} files written, skip "
                   f"{'logged' if 'asset extraction of spyro-2 skipped' in log else 'absent'}")


def case_cache(a):
    """The launcher asset cache over a three-game library, headless: cold, warm, each rewrite trigger, the failed marker, another build id."""
    d = cl.case_dir(a.out, "cache")
    entries = seed_library(d)
    bad = []
    cache_cold(a, d, bad)
    log, w, r = run_step(a, d, "warm", bad)
    current = len(set(re.findall(r"asset cache of (\S+) is current", log)))
    if w or r or current != len(DISCS):
        bad.append(f"warm: {len(w)} files written, {len(r)} reruns, {current} games current")
    cache_rewrites(a, d, entries, bad)
    cache_failed(a, d, bad)
    other = exe_copy(a, d)
    _, w, r = run_step(a, d, "other-build", bad, binary=other)
    expect_reruns(bad, "other-build", r, {"spyro-1": "outputs written by build", "spyro-3": "outputs written by build",
                                          "spyro-2": "a failed marker of build"})
    m = manifest(d, "spyro-2")
    if not m or m[0].get("failed_build"):
        bad.append("other-build: spyro-2's failed marker not cleared")
    _, w, r = run_step(a, d, "other-build-warm", bad, binary=other)
    if w or r:
        bad.append(f"other-build-warm: {len(w)} files written, {len(r)} reruns")
    return verdict("cache", bad)


def case_cache_ingest(a):
    """An ingest writes the game's cache before it boots; the assets-ready line precedes the module build's end."""
    d = cl.case_dir(a.out, "cache-ingest")
    rc, log = nox(a, d, "ingest", ["--ingest", disc_cue("spyro-1"), "--frame-limit", "1"], timeout=900)
    bad = cache_problems(d, "spyro-1")
    if rc != 0:
        bad.append(f"exit {rc}")
    bad += ordered(log, r"\[library\] added", r"asset extraction of spyro-1 at ingest \(no cache manifest\)",
                   r"asset cache of spyro-1 written", r"launcher assets of spyro-1 ready \(ingest\)",
                   r"booting library entry")
    if reruns(log):
        bad.append(f"an ingest logged launch-time reruns {reruns(log)}")
    detail = "no first-boot pipeline: the build-end order is not exercised"
    if "first-boot module build" in log:
        # A headless ingest holds the launcher thread on the build, so its reap
        # (the ready line) follows the build; the cache write must not.
        bad += ordered(log, r"asset cache of spyro-1 written", r"recomp-build: module built: ")
        detail = "cache written before the module build's end"
    n, _ = domes_count(d, "spyro-1") if not bad else (0, 0)
    return verdict("cache-ingest", bad, f"{n} domes; {detail}")


def case_multi_install_launch(a):
    """T18 through the launch-time path: the dump after a pick whose launch-time jobs installed and extracted every library game, against a fresh single install."""
    if not shutil.which("setarch"):
        raise cl.Skip("setarch not found")
    bad, compared, launches = [], 0, {}
    for g in DISCS:
        d = cl.case_dir(a.out, "multi-install-launch-" + g)
        seed_library(d)
        one = install_dump(a, d, g + ".single", g, False)
        out = os.path.join(d, g + ".launch.dump")
        env = {k: v for k, v in os.environ.items() if not k.startswith("OPENPETE_")}
        env.update({"OPENPETE_INSTALL_DUMP": out, "XDG_DATA_HOME": os.path.join(d, "data"),
                    "XDG_STATE_HOME": os.path.join(d, "state"), "XDG_CONFIG_HOME": os.path.join(d, "config")})
        logp = os.path.join(d, g + ".launch.log")
        with open(logp, "w") as f:
            cl.run_session(["setarch", os.uname().machine, "-R", a.binary, "--no-config", "--no-card", "--mute", "1",
                            "--vibration", "0", "--headless", "--game", g, "--frame-limit", "1"],
                           env, f, 600, stdout=subprocess.DEVNULL)
        log = open(logp, errors="replace").read()
        many = report_lines(out)
        r = dict(reruns(log))
        others = [o for o in DISCS if o != g]
        if any(o not in r for o in others):
            bad.append(f"{g}: launch-time reruns {sorted(r)}, expected {others}")
        if not one or not many:
            bad.append(f"{g}: dump missing")
            continue
        diff = [(x, y) for x, y in zip(one, many) if x != y]
        if len(one) != len(many):
            diff.append((len(one), len(many)))
        compared += len(one)
        bad += [f"{g}: single {str(x)[:90]!r} vs launch {str(y)[:90]!r}" for x, y in diff]
        bad += [f"{g}: {l}" for l in many if l.startswith("install_refusals ") and l != "install_refusals 0"]
        launches[g] = many
        print(f"[launcher-flow] multi-install-launch {g}: {len(r)} launch-time reruns, {len(one)} lines, "
              f"{len(diff)} differ")
    if len(launches) >= 2:
        a1, a2 = list(launches.values())[:2]
        control = sum(1 for x, y in zip(a1, a2) if x != y)
        print(f"[launcher-flow] multi-install-launch control: two targets' launch dumps differ on {control} lines")
        if control == 0:
            bad.append("VACUOUS: two targets' launch dumps are identical")
    if compared == 0:
        bad.append("VACUOUS: no line compared")
    return verdict("multi-install-launch", bad, f"{compared} lines compared")


# ---- the sky (sky-*) ----

# The share of the selector's first frame that differs from the flat clear: with a dome behind it
# every pixel but the vignette's darkest differs; over the flat clear only the page's title, boxes
# and pictures do. A pixel differs when a channel is more than SKY_LEVELS from the clear, the
# binary's own criterion (openpete_native_launcher_shot). The first-present check counts only the
# pixels outside every element rect of the frame's manifest (OPENPETE_LAUNCHER_MANIFEST), each rect
# grown by SKY_MARGIN window px for the boxes' shadows (they reach 20 px at 1280x800): the elements
# differ between the dome run and the control, the background outside them does not. That share may
# be at most SKY_FIRST_TOL; the same count over the first opaque-dome shot must reach SKY_COVER,
# which proves the masked measure sees a dome.
SKY_COVER = 0.9
SKY_FIRST_TOL = 0.01
SKY_LEVELS = 8
SKY_MARGIN = 24
SKY_DRAWS = re.compile(r"sky: (\d+) of (\d+) presents handed a view drew the sky: (\d+) layers \((\d+) revealed, "
                       r"(\d+) crossfaded\), (\d+) triangles, (\d+) meshes uploaded")
SHOT = re.compile(r"screen selector: (\d+) of (\d+) pixels drawn over the clear")
SKY_SHOT = re.compile(r"screen sky: (\d+) of (\d+) pixels drawn over the clear")
LAUNCHER_CLEAR = (24, 16, 40)   # kBg, core/launcher/launcher.cpp


def drawn_outside(base, screen):
    """The share of a shot's pixels outside every element rect of its manifest that differ from the clear.

    Returns (share, pixels counted), or (-1.0, 0) when the shot or its manifest is missing."""
    from PIL import Image
    m, png = cl.read_json(base + "." + screen + ".json"), base + "." + screen + ".png"
    if m is None or not os.path.isfile(png):
        return -1.0, 0
    im = Image.open(png).convert("RGB")
    w, h = im.size
    sx, sy = w / max(1, m["window"][0]), h / max(1, m["window"][1])
    inside = bytearray(w * h)
    for it in m.get("items", []):
        x0, y0, x1, y1 = it["rect"]
        c0, c1 = max(0, int((x0 - SKY_MARGIN) * sx)), min(w, int(math.ceil((x1 + SKY_MARGIN) * sx)))
        for y in range(max(0, int((y0 - SKY_MARGIN) * sy)), min(h, int(math.ceil((y1 + SKY_MARGIN) * sy)))):
            inside[y * w + c0:y * w + c1] = b"\1" * max(0, c1 - c0)
    px, n, differ = im.tobytes(), 0, 0
    for i in range(w * h):
        if inside[i]:
            continue
        n += 1
        r, g, b = px[3 * i], px[3 * i + 1], px[3 * i + 2]
        differ += (abs(r - LAUNCHER_CLEAR[0]) > SKY_LEVELS or abs(g - LAUNCHER_CLEAR[1]) > SKY_LEVELS
                   or abs(b - LAUNCHER_CLEAR[2]) > SKY_LEVELS)
    return differ / max(1, n), n


def sky_selector(a, d, tag, extra=None):
    """One windowed run of the selector, quit after 6 s.

    Returns (log, layers drawn, selector and sky shot drawn fractions, the same two outside the element rects)."""
    base = os.path.join(d, tag)
    env = dict({"OPENPETE_LAUNCHER_SHOT": base, "OPENPETE_LAUNCHER_MANIFEST": base,
                "OPENPETE_LAUNCHER_INPUT": "6000:quit"}, **(extra or {}))
    for f in [base + "." + sc + ext for sc in ("selector", "sky") for ext in (".png", ".json")]:
        if os.path.exists(f):
            os.remove(f)
    rc, log = cl.launch(a.binary, d, tag, ["--launcher-window", "1280x800"], env, timeout=600)
    m, shot, sky = SKY_DRAWS.search(log), SHOT.search(log), SKY_SHOT.search(log)
    frac = int(shot.group(1)) / max(1, int(shot.group(2))) if shot else -1.0
    sfrac = int(sky.group(1)) / max(1, int(sky.group(2))) if sky else -1.0
    (out, n), (sout, sn) = drawn_outside(base, "selector"), drawn_outside(base, "sky")
    print(f"[launcher-flow] sky-cache {tag}: exit {rc}, {m.group(0) if m else 'no sky draw line'}, "
          f"selector {frac:.3f} drawn ({out:.4f} of {n} px outside the elements), "
          f"sky shot {sfrac:.3f} ({sout:.4f} of {sn} px outside)")
    return log, int(m.group(3)) if m else 0, frac, sfrac, out, sout


def case_sky_cache(a):
    """T7's sky: the selector draws the domes the cache holds with no game picked; the control without a cache draws none (windowed)."""
    d = cl.case_dir(a.out, "sky-cache")
    seed_entry(d, "spyro-1", 1)
    seed_entry(d, "spyro-2", 2)
    bad = []
    rc, _ = nox(a, d, "cache", ["--game", "spyro-1", "--frame-limit", "1"])
    if rc != 0:
        bad.append(f"cache run: exit {rc}")
    want = {g: domes_count(d, g)[0] for g in ("spyro-1", "spyro-2") if manifest(d, g)}
    if len(want) != 2 or 0 in want.values():
        return verdict("sky-cache", bad + [f"VACUOUS: the caches hold domes {want}"])
    log, layers, frac, sfrac, out, sout = sky_selector(a, d, "sky")
    for g, n in want.items():
        got = re.search(r"sky: (\d+) domes of %s loaded from the cache" % g, log)
        if not got or int(got.group(1)) != n:
            bad.append(f"{g}: {got.group(1) if got else 'no'} domes loaded, domes.bin holds {n}")
    if layers == 0:
        bad.append("the sky pass drew no layer")
    if sfrac < SKY_COVER:
        bad.append(f"the opaque-dome shot differs from the clear on {sfrac:.3f} of its pixels")
    shutil.rmtree(os.path.join(d, "state", "openpete", "cache"))
    # The launch-time jobs would extract the removed cache again; the forced failure keeps it empty.
    log0, layers0, frac0, sfrac0, out0, _ = sky_selector(a, d, "control", {"OPENPETE_ASSET_FAIL": "1"})
    if out < 0 or out > SKY_FIRST_TOL:
        bad.append(f"the selector's first frame differs from the clear on {out:.4f} of its pixels outside the "
                   f"elements: the sky drew on the first present")
    if sout < SKY_COVER:
        bad.append(f"the opaque-dome shot differs from the clear on {sout:.4f} of its pixels outside the "
                   f"elements: the masked measure does not see a dome")
    if out0 < 0 or out0 > SKY_FIRST_TOL:
        bad.append(f"control: the first frame differs from the clear on {out0:.4f} of its pixels outside the "
                   f"elements: the background is not the clear, or an element is drawn the manifest does not list")
    joined = re.search(r"sky: \d+ domes of \S+ (loaded from the cache|joined the rotation)", log0)
    if joined or layers0 or sfrac0 >= 0:
        bad.append(f"control: domes {'loaded' if joined else 'none'}, {layers0} layers, {frac0:.3f} drawn")
    return verdict("sky-cache", bad, f"domes {want}; {layers} layers; first frame {frac:.3f}, sky shot {sfrac:.3f}, "
                                     f"control {frac0:.3f}; outside the elements: first frame {out:.4f}, "
                                     f"sky shot {sout:.4f}, control {out0:.4f}")


def case_sky_reveal(a):
    """T8's dome join and first reveal: both start in the step that reaps the first disc's assets (windowed)."""
    d = cl.case_dir(a.out, "sky-reveal")
    env = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_DROP": disc_cue("spyro-1"),
           "OPENPETE_LAUNCHER_INPUT": "30000:quit"}
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800"], env, timeout=900)
    bad = common_flow(log)
    # The title switch (check_launcher_title.py title-switch) logs in the same reap; its lines are set aside.
    lines = [l for l in log.splitlines() if " launcher: " in l and "title glyph models" not in l
             and "launcher title:" not in l]
    ready = [i for i, l in enumerate(lines) if "launcher assets of spyro-1 ready (ingest)" in l]
    if not ready:
        bad.append("no assets-ready line of the ingest")
    else:
        nxt = lines[ready[0] + 1] if ready[0] + 1 < len(lines) else ""
        m = re.search(r"sky: (\d+) domes of spyro-1 joined the rotation; the reveal starts", nxt)
        if not m or int(m.group(1)) == 0:
            bad.append(f"the line after the assets-ready line is {nxt.strip()[-120:]!r}")
    if "domes of spyro-1 loaded from the cache" in log:
        bad.append("the empty library seeded domes before the ingest")
    m = SKY_DRAWS.search(log)
    if not m or int(m.group(4)) == 0:
        bad.append(f"no revealed layer drawn ({m.group(0) if m else 'no sky draw line'})")
    return verdict("sky-reveal", bad, m.group(0) if m else "")


SLOW_ASSETS = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_ASSET_JOB_DELAY_MS": "20000"}


def case_job_ingest_stop_assets(a):
    """T8 (a) on the extraction rerun: A's launch-time asset job stopped at B's copy end, then completed after B's jobs (windowed)."""
    d = cl.case_dir(a.out, "job-ingest-stop-assets")
    seed_entry(d, "spyro-1", 1)
    env = dict(SLOW_ASSETS, OPENPETE_LAUNCHER_PICK_DELAY_MS="1500", OPENPETE_LAUNCHER_DROP=disc_cue("spyro-2"),
               OPENPETE_LAUNCHER_SELECT_GAME="spyro-2", OPENPETE_LAUNCHER_SELECT_IDLE="1")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"launch-time rerun: asset extraction of spyro-1",
        r"scripted drop", r"\[library\] added", r"stopping the launch-time job of spyro-1 \(requeued\)",
        r"asset extraction of spyro-1 stopped", r"asset extraction of spyro-2 at ingest",
        r"launcher assets of spyro-2 ready \(ingest\)", r"launch-time job started: spyro-1",
        r"launcher assets of spyro-1 ready \(launch-time\)", r"scripted pick: entry \d+ \(spyro-2\)",
        r"game picked: spyro-2")
    bad += cache_problems(d, "spyro-1") + cache_problems(d, "spyro-2")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-ingest-stop-assets", bad)


def case_job_pick_other_assets(a):
    """T8 (c) on the extraction rerun: a pick of B during A's launch-time asset job stops A, which leaves no partial cache file (windowed)."""
    d = cl.case_dir(a.out, "job-pick-other-assets")
    seed_entry(d, "spyro-1", 1)
    seed_entry(d, "spyro-2", 2)
    env = dict(SLOW_ASSETS, OPENPETE_LAUNCHER_PICK_DELAY_MS="1500", OPENPETE_LAUNCHER_SELECT_GAME="spyro-2")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    bad = common_flow(log) + ordered(
        log, r"launch-time job started: spyro-1", r"scripted pick: entry \d+ \(spyro-2\)",
        r"stopping the launch-time job of spyro-1", r"asset extraction of spyro-1 stopped; the cache is unchanged",
        r"launcher assets of spyro-2 ready \(pick\)", r"game picked: spyro-2")
    a_dir = cache_dir(d, "spyro-1")
    left = os.listdir(a_dir) if os.path.isdir(a_dir) else []
    if left:
        bad.append(f"A's cache holds files after its stopped job: {left}")
    bad += cache_problems(d, "spyro-2")
    if rc != 0:
        bad.append(f"exit {rc}")
    return verdict("job-pick-other-assets", bad)


# ---- quit during work (quit-*) ----

# The quit bound on Linux: 2 s for the stop's own work, plus the one pipeline compile the stop may find in
# flight, which cannot be interrupted (the preview's terrain compile: about 0.5 s at idle on llvmpipe, 1-11 s
# on a loaded host). The launcher logs that compile's length when the stop finds one; the ahead compile
# takes no pipeline after the quit and its in-flight launcher pipelines are inside the 2 s.
QUIT_BOUND_MS = 2000.0
QUIT_CLOSED = re.compile(r"quit during work: the work stopped and the launcher closed ([\d.]+) ms after the quit")
QUIT_INFLIGHT = re.compile(r"preview compile stopped after the compile in flight \(([\d.]+) ms long")
LOG_T = r"^\s*([\d.]+) \[\w+\s*\] "


def log_time(log, pattern):
    """The process-relative time in seconds of the first log line matching `pattern` after its timestamp, or None."""
    m = re.search(LOG_T + pattern, log, re.M)
    return float(m.group(1)) if m else None


def quit_launch(a, d, tag, args, env, timeout=900):
    """A windowed run whose log is followed for the quit line; returns (rc, log, ms from the line to the exit or None)."""
    res = {}
    t = threading.Thread(target=lambda: res.update(r=cl.launch(a.binary, d, tag, args, env, timeout=timeout)))
    t.start()
    path, off, tail, seen = os.path.join(d, tag + ".log"), 0, b"", None
    while t.is_alive():
        if seen is None and os.path.isfile(path):
            with open(path, "rb") as f:
                f.seek(off)
                chunk = f.read()
            off += len(chunk)
            if b"quit during work (" in tail + chunk:
                seen = time.monotonic()
            tail = (tail + chunk)[-64:]
        time.sleep(0.005)
    t.join()
    end = time.monotonic()
    rc, log = res["r"]
    return rc, log, ((end - seen) * 1000.0 if seen else None)


def live_processes(d):
    """Processes other than this one and the run's virtual display whose command line names the case directory.

    xvfb-run keeps its authority file in TMPDIR, so its Xvfb, which it signals as it returns and
    does not wait for, names the case directory too."""
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit() or int(pid) == os.getpid():
            continue
        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                cmd = f.read().replace(b"\0", b" ").decode(errors="replace")
        except OSError:
            continue
        if d in cmd and not cmd.startswith("Xvfb "):
            out.append(f"{pid}: {cmd[:120]}")
    return out


def leftovers(d, stopped_cache=None):
    """The files and processes a stopped run must not leave, by kind, with the paths found."""
    lib = os.path.join(d, "data", "openpete", "library")
    found = {
        "ingest-tmp": glob.glob(os.path.join(lib, "ingest-tmp-*")),
        "failed-markers": glob.glob(os.path.join(lib, "*", "build-failed.toml")),
        "partial-modules": [m for m in glob.glob(os.path.join(lib, "*", "modules", "*", "*-recomp.so"))
                            if not whole_module(m)],
        "build-dirs": glob.glob(os.path.join(lib, "*", "modules", "*", "build")),
        "atomic-temps": [os.path.join(r, f) for root in ("data", "state") for r, _, fs in os.walk(os.path.join(d, root))
                         for f in fs if ".tmp-" in f],
        "tmpdir-files": [os.path.join(r, f) for r, _, fs in os.walk(os.path.join(d, "tmp")) for f in fs],
        "processes": live_processes(d),
    }
    if stopped_cache:
        c = cache_dir(d, stopped_cache)
        found["stopped-cache-files"] = [os.path.join(c, f) for f in os.listdir(c)] if os.path.isdir(c) else []
    return found


def quit_case(a, name, setup, quit_at, expect, extra_env=None, next_launch=None, args=None):
    """One quit during work: seed, run windowed with the scripted gesture, then check the stop, its time and its leftovers.

    `setup(d)` seeds the case directory and returns the game whose asset cache the stop must leave empty, or
    None; `args(d)` adds arguments to the run; `next_launch(a, d, bad)` runs the launch after the quit, appends
    its problems to `bad` and returns its detail."""
    d = cl.case_dir(a.out, name)
    os.makedirs(os.path.join(d, "tmp"))
    stopped_cache = setup(d)
    env = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_LAUNCHER_QUIT_AT": quit_at, "TMPDIR": os.path.join(d, "tmp")}
    env.update(extra_env or {})
    rc, log, ms = quit_launch(a, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"]
                              + (args(d) if args else []), env)
    bad = ordered(log, r"scripted quit point reached", r"quit during work \(", expect,
                  r"quit during work: the work stopped and the launcher closed")
    m = QUIT_CLOSED.search(log)
    own = float(m.group(1)) if m else None
    if rc != 0:
        bad.append(f"exit {rc}")
    if "game picked" in log.split("quit during work (", 1)[-1] or "window, device and imgui handed" in log:
        bad.append("the game started after the quit")
    after = log.split("quit during work (", 1)[-1]
    inflight = QUIT_INFLIGHT.search(after)
    bound = QUIT_BOUND_MS + (float(inflight.group(1)) if inflight else 0.0)
    if ms is None:
        bad.append("the quit line was not seen while the process ran")
    elif ms > bound:
        bad.append(f"the process ended {ms:.0f} ms after the quit, over {bound:.0f} ms")
    lib = os.path.join(d, "data", "openpete", "library")
    found = leftovers(d, stopped_cache)
    for kind, paths in found.items():
        if paths:
            bad.append(f"{len(paths)} {kind}: {paths[:3]}")
    counts = ", ".join(f"{k} {len(v)}" for k, v in found.items())
    entries = [e for e in glob.glob(os.path.join(lib, "*", "meta.toml"))]
    detail = (f"exit {ms:.0f} ms after the quit (harness), {own if own is not None else '?'} ms (launcher), "
              f"bound {bound:.0f} ms ({inflight.group(1) + ' ms compile in flight' if inflight else 'none in flight'}); "
              if ms is not None else "") + f"leftovers: {counts}; entries {len(entries)}"
    if next_launch:
        detail += "; " + next_launch(a, d, bad)
    return verdict(name, bad, detail)


def next_rebuild(a, d, bad):
    """The launch after a stopped module build rebuilds Spyro 1's module in the background and plays."""
    rc2, log2 = headless(a, d, "next", ["--game", "spyro-1", "--frame-limit", "1"])
    mods = stamped(d, "spyro-1")
    if rc2 != 0 or "launch-time module rebuild of spyro-1" not in log2 or not mods or not whole_module(mods[0]):
        bad.append(f"next launch: exit {rc2}, rebuild {'ran' if 'launch-time module rebuild' in log2 else 'not run'}, "
                   f"module {'whole' if mods and whole_module(mods[0]) else 'absent or partial'}")
        return "next launch failed"
    return "next launch rebuilt in the background and played"


def seed_unbuilt(d):
    """A Spyro 1 entry with no module: its launch-time job builds, and the pick waits for it."""
    seed_entry(d, "spyro-1", 1)
    return None


PICK1 = {"OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1"}
BUILD_EXPECT = r"module build stopped, its compiler commands killed"


def case_quit_copy(a):
    """A quit event during a dropped disc's copy: no entry, no ingest directory."""
    def setup(d):
        os.makedirs(os.path.join(d, "data", "openpete", "library"))
        return None
    return quit_case(a, "quit-copy", setup, "copy:quit", r"ingest copy of .* stopped by the quit",
                     {"OPENPETE_LAUNCHER_DROP": disc_cue("spyro-1")})


def case_quit_translate(a):
    """SIGTERM during PSXRecomp kills it; no marker, no build directory."""
    need_first_boot(a)
    return quit_case(a, "quit-translate", seed_unbuilt, "translate+300:sigterm", BUILD_EXPECT, PICK1)


def case_quit_build(a):
    """A quit event while the compiles run kills them; the next launch rebuilds in the background."""
    need_first_boot(a)
    return quit_case(a, "quit-build", seed_unbuilt, "build+1500:quit", BUILD_EXPECT, PICK1, next_launch=next_rebuild)


def case_quit_assets(a):
    """A quit event while the pick waits for a held asset job: no cache file."""
    def setup(d):
        seed_built(a, d, "spyro-1", 1) if has_first_boot(a) else seed_entry(d, "spyro-1", 1)
        return "spyro-1"
    return quit_case(a, "quit-assets", setup, "assets+300:quit",
                     r"launcher assets of spyro-1 not updated \(stopped\)",
                     dict(PICK1, OPENPETE_ASSET_JOB_DELAY_MS="60000"))


QUIT_MOD = "crash-dummy"   # a source-only mod every game compiles


def quit_mods_game(a, game):
    """A quit event while the pick compiles a stale source-only mod of `game`: the compile stops, the mod stays
    stale, the game does not start, and the next launch compiles the mod."""
    def setup(d):
        if has_first_boot(a):
            seed_built(a, d, game, 1)
        else:
            seed_entry(d, game, 1)
        shutil.copytree(os.path.join(ROOT, "mods", QUIT_MOD), os.path.join(d, "mods", QUIT_MOD),
                        ignore=shutil.ignore_patterns(".build"))
        return None

    def mods_dir(d):
        return ["--mods-dir", os.path.join(d, "mods")]

    def next_mods(a, d, bad):
        rc2, log2 = headless(a, d, "next", ["--game", game, "--frame-limit", "1"] + mods_dir(d))
        compiled = re.search(rf"{QUIT_MOD} \[{game}\]: compiling", log2) is not None
        if rc2 != 0 or not compiled or "compile failed" in log2:
            bad.append(f"next launch: exit {rc2}, mod {'compiled' if compiled else 'not compiled'}")
            return "next launch failed"
        return "next launch compiled the mod"

    return quit_case(a, f"quit-mods-{game}", setup, "mods:quit",
                     rf"{QUIT_MOD} \[{game}\]: compile stopped; the mod stays stale",
                     {"OPENPETE_LAUNCHER_SELECT_GAME": game}, next_launch=next_mods, args=mods_dir)


def case_quit_mods(a):
    """A quit event during the mod compile on boot ends the run in the launcher, for each game."""
    rc = 0
    for game in cl.GAMES:
        rc |= quit_mods_game(a, game)
    return rc


def case_quit_east_twice(a):
    """East twice within 2 s on the build's progress view quits."""
    need_first_boot(a)
    return quit_case(a, "quit-east-twice", seed_unbuilt, "build+1000:east2", BUILD_EXPECT, PICK1)


def case_quit_escape(a):
    """Escape on the build's progress view quits at once."""
    need_first_boot(a)
    return quit_case(a, "quit-escape", seed_unbuilt, "build+1000:escape", BUILD_EXPECT, PICK1)


def case_quit_close(a):
    """Closing the window on the build's progress view quits at once."""
    need_first_boot(a)
    return quit_case(a, "quit-close", seed_unbuilt, "build+1000:close", BUILD_EXPECT, PICK1)


def case_quit_east_once(a):
    """One East arms the quit (the step chip asks again) and disarms after 2 s; the build completes and the game plays."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "quit-east-once")
    seed_unbuilt(d)
    env = dict(PICK1, OPENPETE_PRECOMPILE_AHEAD="1", OPENPETE_LAUNCHER_QUIT_AT="build+500:east")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env, timeout=900)
    bad = ordered(log, r"scripted quit gesture: east", r"quit armed",
                  r"progress view: the quit chip reads Press again to quit", r"quit disarmed: no second East within 2000 ms",
                  r"module rebuild of spyro-1 done", r"game picked: spyro-1")
    if "quit during work (" in log:
        bad.append("one East quit")
    mods = stamped(d, "spyro-1")
    if rc != 0 or not mods or not whole_module(mods[0]):
        bad.append(f"exit {rc}, module {'whole' if mods and whole_module(mods[0]) else 'absent or partial'}")
    return verdict("quit-east-once", bad)


def launcher_pid(d, timeout=60.0):
    """The pid of the openpete process whose XDG_DATA_HOME is the case's data root, or None."""
    want = ("XDG_DATA_HOME=" + os.path.join(d, "data")).encode()
    deadline = time.time() + timeout
    while time.time() < deadline:
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                if os.path.basename(os.readlink(f"/proc/{pid}/exe")) != "openpete":
                    continue
                with open(f"/proc/{pid}/environ", "rb") as f:
                    if want in f.read().split(b"\0"):
                        return int(pid)
            except OSError:
                continue
        time.sleep(0.05)
    return None


def case_quit_page_sigterm(a):
    """SIGTERM sent from outside to an idle game page ends the process (exit 0) within the quit bound."""
    d = cl.case_dir(a.out, "quit-page-sigterm")
    if has_first_boot(a):
        seed_built(a, d, "spyro-1", 1)
    else:
        seed_entry(d, "spyro-1", 1)
    res = {}
    t = threading.Thread(target=lambda: res.update(r=cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800"],
                                                               {}, timeout=120)))
    t.start()
    log = os.path.join(d, "run.log")
    # Idle is the page after its launch-time job: the entry's box picture renders first, and a terrain pipeline
    # creation in flight is not interruptible, so a SIGTERM during it measures the compile, not the page.
    deadline, idle = time.time() + 120, False
    while time.time() < deadline and t.is_alive():
        text = open(log, errors="replace").read() if os.path.isfile(log) else ""
        if "stamp list_shown" in text and "launcher assets of spyro-1 ready (launch-time)" in text:
            idle = True
            break
        time.sleep(0.05)
    time.sleep(1.0)   # the page idles: no job, no input
    pid = launcher_pid(d, 5.0) if idle else None
    sent = time.monotonic()
    if pid:
        os.kill(pid, 15)
    t.join()
    ms = (time.monotonic() - sent) * 1000.0
    rc, text = res["r"]
    bad = []
    if not idle or not pid:
        bad.append(f"VACUOUS: page {'shown' if idle else 'not shown'}, pid {pid}")
    if rc != 0:
        bad.append(f"exit {rc}")
    if ms > QUIT_BOUND_MS:
        bad.append(f"the process ended {ms:.0f} ms after SIGTERM, over {QUIT_BOUND_MS:.0f} ms")
    if "game picked" in text:
        bad.append("a game started")
    return verdict("quit-page-sigterm", bad, f"pid {pid}, exit {ms:.0f} ms after SIGTERM")


# ---- the shader step (shader-step-*) ----

SHADER_ENV = {"OPENPETE_PRECOMPILE_AHEAD": "1", "OPENPETE_PRECOMPILE_FORCE": "1", "OPENPETE_LAUNCHER_SELECT": "0"}
STAGE = re.compile(r"compile stage: ([\d.]+) ms from its open, \d+ creates queued, \d+ done; pipelines compiled "
                   r"(\d+) \(bring-up (\d+) of (\d+) creates, precompile (\d+) of (\d+);.*?progress screen (\d+) "
                   r"present\(s\), the first at (\d+) of (\d+)")
WALK_END = re.compile(r"launcher walk: keys (\d+), created (\d+), probe hits (\d+), .*?([\d.]+) ms \((.*)\)")
BAR = re.compile(r"compile stage bar: predicted (\d+) creates, final total (\d+) \((\d+) queued\); (\d+) screen\(s\)[^:]*: (.*)")


def bar_problems(log):
    """The compile stage's bar: a total on every screen and a fraction that never falls; returns (problems, detail)."""
    m = BAR.search(log)
    if not m:
        return ["no compile stage bar line"], ""
    vals = [tuple(int(x) for x in v.split("/")) for v in m.group(5).split()] if m.group(5) != "-" else []
    bad = []
    if any(t == 0 for _, t in vals):
        bad.append("a screen drew the bar with no total")
    fr = [d / t for d, t in vals if t]
    if any(b < a for a, b in zip(fr, fr[1:])):
        bad.append("the bar's fraction fell between screens")
    shown = " ".join(f"{d}/{t}" for d, t in vals[:4]) + (" ... " + " ".join(f"{d}/{t}" for d, t in vals[-2:]) if len(vals) > 4 else "")
    return bad, f"bar predicted {m.group(1)}, final {m.group(2)} of {m.group(3)} queued, {m.group(4)} screen(s): {shown}"


def shader_run(a, name, env, warm_assets=False):
    """One scripted pick of a Spyro 1 entry on a cold pipeline cache; returns (rc, log, stage match, walk-end match, problems).

    With warm_assets a windowed run without the ahead walk first writes the entry's launcher asset
    cache (the preview needs the window) and its pipeline cache file is removed, so no launch-time
    asset job holds the pick while the ahead walk runs on."""
    d = cl.case_dir(a.out, name)
    seed_entry(d, "spyro-1", 1)
    if warm_assets:
        rc, _ = cl.launch(a.binary, d, "assets", ["--frame-limit", "1"],
                          {"OPENPETE_PRECOMPILE_AHEAD": "0", "OPENPETE_LAUNCHER_SELECT": "0"}, timeout=900)
        if rc != 0 or not os.path.isfile(os.path.join(d, "state", "openpete", "cache", "spyro-1", "preview.rgba")):
            raise RuntimeError(f"the asset cache run wrote no cache (exit {rc}); see {d}/assets.log")
        for f in glob.glob(os.path.join(d, "state", "openpete", "cache", "vk-pipelines-*.bin")):
            os.remove(f)
    rc, log = cl.launch(a.binary, d, "run", ["--frame-limit", "20"], dict(SHADER_ENV, **env), timeout=900)
    st, wl = STAGE.search(log), WALK_END.search(log)
    bad = common_flow(log)
    if rc != 0:
        bad.append(f"exit {rc}")
    if not st:
        bad.append("no compile stage line")
    if not wl:
        bad.append("no launcher walk end line")
    return rc, log, st, wl, bad


def case_shader_step_during(a):
    """A pick during the ahead walk: the walk stops short and the compile stage compiles and draws (windowed)."""
    rc, log, st, wl, bad = shader_run(a, "shader-step-during", {"OPENPETE_PRECOMPILE_PANEL_MS": "0"}, warm_assets=True)
    if "launch-time rerun:" in log:
        bad.append("a launch-time job reran an asset step (the asset cache was not current)")
    if wl and "stopped at the pick" not in wl.group(5):
        bad.append("VACUOUS: the ahead walk returned every create info before the pick")
    if st and int(st.group(3)) == 0:
        bad.append("bring-up compiled no pipeline: the pick came after the walk's useful part")
    if st and int(st.group(7)) == 0:
        bad.append("the compile stage drew no progress screen")
    bar_bad, bar_detail = bar_problems(log)
    bad += bar_bad
    detail = (f"walk created {wl.group(2)} of {wl.group(1)}; bring-up compiled {st.group(3)} of {st.group(4)}, "
              f"stage {st.group(1)} ms, {st.group(7)} screen(s), first at {st.group(8)} of {st.group(9)}; "
              f"{bar_detail}") if st and wl else ""
    return verdict("shader-step-during", bad, detail)


def case_shader_step_after(a):
    """A pick after the ahead walk ends: no bring-up compile, no progress screen (windowed)."""
    rc, log, st, wl, bad = shader_run(a, "shader-step-after", {"OPENPETE_LAUNCHER_PICK_AFTER_WALK": "end"})
    if wl and "stopped at the pick" in wl.group(5):
        bad.append("the ahead walk was still running at the pick")
    if not re.search(r"scripted pick released: the ahead walk ended", log):
        bad.append("the pick was not held for the ahead walk's end")
    if wl and int(wl.group(2)) + int(wl.group(3)) == 0:
        bad.append("VACUOUS: the ahead walk created nothing")
    if st and int(st.group(4)) == 0:
        bad.append("VACUOUS: bring-up made no create")
    if st and int(st.group(3)) != 0:
        bad.append(f"bring-up compiled {st.group(3)} pipelines the walk had compiled")
    # The progress screen is timed (NR_PRECOMPILE_PANEL_NS): on lavapipe a creation the cache holds
    # still compiles, so a loaded host can pass the wait with nothing to compile; the count of
    # pipelines compiled decides, the screens are reported.
    detail = (f"walk created {wl.group(2)} of {wl.group(1)} in {wl.group(4)} ms; bring-up {st.group(4)} creates, "
              f"{st.group(3)} compiled, stage {st.group(1)} ms, {st.group(7)} screen(s)") if st and wl else ""
    return verdict("shader-step-after", bad, detail)


def case_shader_step_order(a):
    """The ahead walk's terrain group returns before the other built-in pipelines at the first texture mapping (windowed)."""
    rc, log, st, wl, bad = shader_run(a, "shader-step-order", {"OPENPETE_LAUNCHER_PICK_AFTER_WALK": "2",
                                                                "OPENPETE_TERRAIN_CACHE_FORCE": "1"})
    groups = re.search(r"per group at the first mapping (\d+) (\d+) (\d+) (\d+)", log)
    if not groups or int(groups.group(2)) == 0 or int(groups.group(3)) == 0:
        bad.append("VACUOUS: the walk queued no terrain or no other built-in create info")
    bad += ordered(log, r"launcher walk: \w+ group 1 returned", r"launcher walk: \w+ group 2 returned")
    back = {m.group(1): m.group(2) for m in re.finditer(r"launcher walk: \w+ group (\d) returned: \d+ create infos, ([\d.]+) ms", log)}
    detail = (f"first mapping: terrain {groups.group(2)} at {back.get('1', '-')} ms, other {groups.group(3)} "
              f"at {back.get('2', '-')} ms") if groups else ""
    return verdict("shader-step-order", bad, detail)


# ---- the game page's commands (input) ----

# Box i's centre on the game page at 1280x800, in the launcher's 640x448 layout space (mouse= warps
# there): the three boxes' rects come from the reference layout's empty page.
def entry_point(i, dx=0.0):
    """The mouse= action pointing at game page box i's centre (1280x800), dx layout px to the right."""
    ref = cl.read_json(cl.REFERENCE)["presets"]["1280x800"][0]["items"]
    r = next(it["rect"] for it in ref if it["id"] == f"box.{cl.GAMES[i]}.frame")
    sc = min(1280 / 640, 800 / 448)
    ox, oy = (1280 - 640 * sc) / 2, (800 - 448 * sc) / 2
    return f"mouse={((r[0] + r[2]) / 2 - ox) / sc + dx:.1f}/{((r[1] + r[3]) / 2 - oy) / sc:.1f}"


def inputs(*acts, start=1500, step=300):
    """An OPENPETE_LAUNCHER_INPUT value: each action `step` ms after the one before, the first at `start`;
    an int in the list adds that many ms to the gap before the next action."""
    out, t = [], start
    for x in acts:
        if isinstance(x, int):
            t += x
            continue
        out.append(f"{t}:{x}")
        t += step
    return ",".join(out)


def focus_moves(log):
    """The focus moves the log names, in order: (kind, from, to)."""
    out = []
    for m in re.finditer(r"command: focus (left|right) from box (\d+) to box (\d+)|"
                         r"command: focus follows the pointer from box (\d+) to box (\d+)", log):
        if m.group(1):
            out.append((m.group(1), int(m.group(2)), int(m.group(3))))
        else:
            out.append(("pointer", int(m.group(4)), int(m.group(5))))
    return out


def input_run(a, d, tag, acts, extra=None, args=None, timeout=300):
    """One windowed launcher run over the case's library with the scripted actions; returns (rc, log)."""
    env = {"OPENPETE_LAUNCHER_INPUT": acts}
    env.update(extra or {})
    return cl.launch(a.binary, d, tag, ["--launcher-window", "1280x800"] + (args or []), env, timeout=timeout)


# The settings menu's EXIT GAME, injected into a game session (OPENPETE_MOUSE_INJECT): open the menu, Up to
# the EXIT row (the cursor wraps from the first row), Return arms it, Return again fires it.
EXIT_SCRIPT = ("2500ms menu 1\n3000ms keydown Up\n3060ms keyup Up\n3300ms keydown Return\n3360ms keyup Return\n"
               "3600ms keydown Return\n3660ms keyup Return\n")


def exit_script(d, name, extra=""):
    """Write the EXIT GAME injection script (plus `extra` lines) into the case directory; return its path."""
    path = os.path.join(d, name + ".inject")
    with open(path, "w") as f:
        f.write(EXIT_SCRIPT + extra)
    return path


def proc_counts(pid):
    """The process's thread and open-descriptor counts, or None when it is gone; each descriptor's target
    is printed for a diagnosis."""
    try:
        fds = sorted(os.listdir(f"/proc/{pid}/fd"), key=int)
        targets = []
        for f in fds:
            try:
                targets.append(f + "->" + os.readlink(f"/proc/{pid}/fd/{f}"))
            except OSError:
                pass
        print(f"[launcher-flow] pid {pid} descriptors: {' '.join(targets)}")
        return len(os.listdir(f"/proc/{pid}/task")), len(fds)
    except OSError:
        return None


def sampled_launch(a, d, tag, args, env, card, timeout, marker, sample=None):
    """cl.launch in a thread while the main thread samples the process (its XDG_DATA_HOME is the case's)
    each time the log gains a `marker` line with `sample(pid)` (default proc_counts): returns (rc, log,
    [sample per marker line])."""
    sample = sample or proc_counts
    res = {}

    def run():
        try:
            res["r"] = cl.launch(a.binary, d, tag, args, env, timeout=timeout, card=card)
        except cl.Skip as e:
            res["skip"] = e

    t = threading.Thread(target=run)
    t.start()
    path, seen, samples = os.path.join(d, tag + ".log"), 0, []
    while t.is_alive():
        time.sleep(0.1)
        try:
            with open(path, errors="replace") as f:
                n = len(re.findall(marker, f.read()))
        except OSError:
            continue
        if n > seen:
            seen = n
            time.sleep(0.4)
            pid = launcher_pid(d, timeout=2.0)
            c = sample(pid) if pid else None
            if c:
                samples.append(c)
    t.join()
    if "skip" in res:
        raise res["skip"]
    rc, log = res["r"]
    return rc, log, samples


def return_timings(log):
    """Per return: (teardown ms from the second EXIT press to the relaunch, ms from the relaunch to the next
    image's entry, ms from that image's start to its first launcher present), from the return's steady-clock
    log lines and the launcher's first_present stamp."""
    req = [int(x) for x in re.findall(r"return: requested from [\w -]+ at steady (\d+) ms", log)]
    rel = [int(x) for x in re.findall(r"returning to the launcher \([^)]*\) at steady (\d+) ms", log)]
    ent = [int(x) for x in re.findall(r"return \d+: image entered at steady (\d+) ms", log)]
    first = [float(x) for x in re.findall(r"stamp first_present ([\d.]+) ms after process start", log)][1:]
    return [(r1 - r0, e - r1, round(f)) for r0, r1, e, f in zip(req, rel, ent, first)]


def case_exit_return(a):
    """EXIT GAME in a session picked on the game page returns to the launcher, three times in a row."""
    d = cl.case_dir(a.out, "exit-return")
    seed_entry(d, "spyro-1", 2)
    seed_entry(d, "spyro-2", 1)
    card = os.path.join(d, "cards")
    os.makedirs(card)
    env = {"OPENPETE_LAUNCHER_SELECT_GAME": "spyro-2,spyro-1,spyro-2", "OPENPETE_LAUNCHER_PICK_DELAY_MS": "1500",
           "OPENPETE_MOUSE_INJECT": exit_script(d, "exit"), "OPENPETE_LAUNCHER_INPUT": "20000:quit"}
    # Flags the relaunch leaves out (kDropped): each bounds one session, and the first relaunch's argv
    # holds them no longer, so they are dropped once.
    args = ["--launcher-window", "1280x800", "--frame-limit", "100000", "--max-runtime-seconds", "900"]
    rc, log, samples = sampled_launch(a, d, "exit-return", args, env, card, 900, r"stamp first_present")
    bad = []
    returns = re.findall(r"returning to the launcher \(OPENPETE_LAUNCHER_RETURN=(\d+):([\w-]+)", log)
    picks = re.findall(r"game picked: ([\w-]+)", log)
    want = [("1", "spyro-2"), ("2", "spyro-1"), ("3", "spyro-2")]
    if returns != want:
        bad.append(f"returns {returns}, want {want}")
    if picks != ["spyro-2", "spyro-1", "spyro-2"]:
        bad.append(f"picks {picks}, want spyro-2, spyro-1, spyro-2")
    dropped = re.findall(r"return: (--[\w-]+) left out of the relaunch", log)
    if dropped != ["--frame-limit", "--max-runtime-seconds"]:
        bad.append(f"dropped flags {dropped}: want --frame-limit and --max-runtime-seconds at the first relaunch")
    focus = re.findall(r"command: focus starts on box \d+ \(([\w-]+), the game returned from\)", log)
    if focus != ["spyro-2", "spyro-1", "spyro-2"]:
        bad.append(f"returned focus {focus}, want spyro-2, spyro-1, spyro-2")
    # Memory cards: the first Spyro 2 session provisions the cards, the second loads them from disk (what
    # the first wrote survived its teardown and the relaunch); each card file is a whole 128 KiB image.
    prov = re.findall(r"provisioned formatted card \d at (\S+)", log)
    loaded = re.findall(r"loaded card \d from (\S+)", log)
    images = [f for f in glob.glob(os.path.join(card, "*.mcr")) if os.path.getsize(f) == 131072]
    if not prov or not set(prov) <= set(loaded) or len(images) != len(set(prov)):
        bad.append(f"cards: provisioned {prov}, loaded {loaded}, whole images {images}")
    # Every image's game page runs with the same threads and descriptors: nothing leaks across a relaunch.
    if len(samples) != 4 or len({fd for _, fd in samples}) != 1 or max(t for t, _ in samples) - min(
            t for t, _ in samples) > 2:
        bad.append(f"(threads, descriptors) per page {samples}: want 4 samples, equal descriptors, threads within 2")
    gaps = return_timings(log)
    if len(gaps) != 3:
        bad.append(f"return timings {gaps}: want 3")
    if rc != 0 or "command: quit" not in log:
        bad.append(f"exit {rc}, final quit {'logged' if 'command: quit' in log else 'absent'}")
    return verdict("exit-return", bad, f"{len(returns)} returns, (threads, fds) per page {samples}, cards "
                   f"provisioned {len(prov)} loaded {len(loaded)}, (teardown, exec, first present) ms {gaps}")


# The launcher's windowed position before the pick (resize=1000x700 keeps it, so a centred window of the
# size the game exits with lands elsewhere), the screen it is centred on, and the relaunched launcher's
# actions: its state, Alt+Enter, its state after leaving fullscreen, quit. The first image hands off at about
# 2 s, before its Alt+Enter is due.
EXIT_FS_LAUNCHER = "1000:geometry,6000:altenter,8000:geometry,10000:quit"
# The game's fullscreen toggle (game-toggle): the settings menu opens on its DISPLAY row, Right flips it and
# the menu closes; EXIT GAME's menu then opens on the first row again, 3 s later than EXIT_SCRIPT's.
EXIT_FS_TOGGLE = "2500ms menu 1\n2800ms keydown Right\n2860ms keyup Right\n3300ms menu 0\n" + re.sub(
    r"^(\d+)ms", lambda m: f"{int(m.group(1)) + 3000}ms", EXIT_SCRIPT, flags=re.M)


def script_file(d, name, text):
    """Write an injection script into the case directory; return its path."""
    path = os.path.join(d, name + ".inject")
    with open(path, "w") as f:
        f.write(text)
    return path


def window_geometry(log):
    """The launcher's geometry lines: [(x, y, w, h, fullscreen)] in log order."""
    return [tuple(int(v) for v in m) for m in re.findall(
        r"window geometry: at (-?\d+),(-?\d+) size (\d+)x(\d+) fullscreen=(\d)", log)]


def case_exit_fullscreen(a):
    """EXIT GAME from a fullscreen game, then Alt+Enter in the relaunched launcher: the window leaves fullscreen
    at the position it had before it went fullscreen, or centred when it had none."""
    d = cl.case_dir(a.out, "exit-fullscreen")
    seed_entry(d, "spyro-1", 1)
    sw, sh = cl.SCREEN_SIZE
    legs = [  # (tag, launcher args, first-image actions, game injection extra, expected position rule)
        ("game-fullscreen", ["--fullscreen", "1"], "500:resize=1000x700,", "", "before"),
        ("launcher-fullscreen", ["--launcher-fullscreen", "1", "--fullscreen", "1"], "", "", "centred"),
        ("game-toggle", [], "500:resize=1000x700,", EXIT_FS_TOGGLE, "before"),
        ("windowed", [], "500:resize=1000x700,", "", "teardown"),
    ]
    bad, detail = [], []
    for tag, extra, first, inject, rule in legs:
        env = {"OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1", "OPENPETE_LAUNCHER_PICK_DELAY_MS": "2000",
               "OPENPETE_MOUSE_INJECT": script_file(d, tag, inject or EXIT_SCRIPT),
               "OPENPETE_LAUNCHER_INPUT": first + EXIT_FS_LAUNCHER}
        args = ["--launcher-window", "1280x800", "--frame-limit", "100000", "--max-runtime-seconds", "300"] + extra
        rc, log = cl.launch(a.binary, d, tag, args, env, timeout=300)
        geo = window_geometry(log)
        tear = re.findall(r"game window at teardown: fullscreen=(\d) maximized=\d at (-?\d+),(-?\d+)", log)
        ret = re.findall(r"return 1: image entered at steady \d+ ms; from [\w-]+, the game window (\w+)", log)
        want_fs = "0" if rule == "teardown" else "1"
        want_ret = "windowed" if rule == "teardown" else "fullscreen"
        toggle = re.findall(r"launcher: fullscreen (on|off)", log)
        game_toggle = re.findall(r"native: fullscreen (on|off)", log)
        # Engagement: the game window was (or was not) fullscreen at the teardown, the relaunched image read
        # that, the relaunched launcher toggled out of (into, for the windowed leg) fullscreen, and the
        # game-toggle leg's game toggled into fullscreen.
        want_toggle = ["on" if rule == "teardown" else "off"]
        want_game = ["on"] if inject else []
        if len(tear) != 1 or tear[0][0] != want_fs or ret != [want_ret] or toggle != want_toggle or \
                game_toggle != want_game:
            bad.append(f"{tag}: not engaged: teardown {tear}, return {ret}, launcher toggle {toggle}, "
                       f"game toggle {game_toggle}")
        # Image 1 before the pick, image 2 at entry, image 2 after its toggle; the windowed leg's toggle
        # enters fullscreen, so its third reading is fullscreen.
        if rc != 0 or len(geo) != 3:
            bad.append(f"{tag}: exit {rc}, geometry readings {geo}: want 3")
            detail.append(f"{tag} {geo}")
            continue
        before, entry, after = geo
        if rule == "teardown":
            got, want = entry, (int(tear[0][1]), int(tear[0][2])) if tear else None
            if entry[4] != 0 or (got[0], got[1]) != want:
                bad.append(f"{tag}: relaunched launcher at {got[:2]} fullscreen={entry[4]}, want windowed at {want}")
        else:
            got = after
            want = (before[0], before[1]) if rule == "before" else ((sw - after[2]) // 2, (sh - after[3]) // 2)
            if entry[4] != 1 or after[4] != 0 or (after[0], after[1]) != want:
                bad.append(f"{tag}: after leaving fullscreen at {after[:2]} size {after[2]}x{after[3]} "
                           f"(fullscreen {entry[4]}->{after[4]}), want {want} ({rule})")
        detail.append(f"{tag}: before {before[:2]} {before[2]}x{before[3]}, relaunched {entry[:2]} "
                      f"fs={entry[4]}, after toggle {after[:2]} {after[2]}x{after[3]} fs={after[4]}")
    return verdict("exit-fullscreen", bad, "; ".join(detail))


# The config file of exit-return-env: game A (Spyro 1) alone sets two variables in its scope, game B (Spyro 2)
# sets one of them to another value.
EXIT_ENV_CONFIG = ('[game.spyro-1.env]\nOPENPETE_NOTEX = "1"\nOPENPETE_TEXPACK_DIR = "packs-a"\n'
                   '[game.spyro-2.env]\nOPENPETE_TEXPACK_DIR = "packs-b"\n')
EXIT_ENV_NAMES = ("OPENPETE_NOTEX", "OPENPETE_TEXPACK_DIR")


def image_env(pid):
    """The EXIT_ENV_NAMES entries of the environment the process's image was given (/proc/<pid>/environ holds
    the execve environment, not later setenv calls), as a sorted tuple of "NAME=value", or None when the
    process is gone; a present-but-empty sample is ("-",) so it still counts as a sample."""
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            kv = [e.decode(errors="replace") for e in f.read().split(b"\0") if e]
    except OSError:
        return None
    return tuple(sorted(e for e in kv if e.split("=", 1)[0] in EXIT_ENV_NAMES)) or ("-",)


def case_exit_return_env(a):
    """EXIT GAME from game A, then game B: the relaunched image is not given the variables A's session exported
    from A's [game.spyro-1.env] scope, so they do not outrank B's config."""
    d = cl.case_dir(a.out, "exit-return-env")
    seed_entry(d, "spyro-1", 2)
    seed_entry(d, "spyro-2", 1)
    cfg = os.path.join(d, "openpete.toml")
    with open(cfg, "w") as f:
        f.write(EXIT_ENV_CONFIG)
    env = {"OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1,spyro-2", "OPENPETE_LAUNCHER_PICK_DELAY_MS": "1500",
           "OPENPETE_MOUSE_INJECT": exit_script(d, "exit"), "OPENPETE_LAUNCHER_INPUT": "20000:quit"}
    args = ["--launcher-window", "1280x800", "--config", cfg]
    rc, log, samples = sampled_launch(a, d, "exit-return-env", args, env, None, 900, r"stamp first_present",
                                      sample=image_env)
    bad = []
    returns = re.findall(r"returning to the launcher \(OPENPETE_LAUNCHER_RETURN=(\d+):([\w-]+)", log)
    if returns != [("1", "spyro-1"), ("2", "spyro-2")]:
        bad.append(f"returns {returns}, want 1:spyro-1 then 2:spyro-2")
    # Each page's image: the first is the harness's launch, the next two follow A's and B's sessions.
    if samples != [("-",)] * 3:
        bad.append(f"image environments per page {samples}: want none of {EXIT_ENV_NAMES} in any")
    reverted = [set(m.split()) for m in
                re.findall(r"return: \d+ variable\(s\) the session set, changed or removed left at their entry "
                           r"state(?:: ([^\n]*))?", log)]
    if len(reverted) != 2 or not set(EXIT_ENV_NAMES) <= reverted[0] or "OPENPETE_TEXPACK_DIR" not in reverted[1] \
            or "OPENPETE_NOTEX" in reverted[1]:
        bad.append(f"reverted names per return {reverted}: want both after A, OPENPETE_TEXPACK_DIR alone of the "
                   f"two after B")
    if rc != 0 or "command: quit" not in log:
        bad.append(f"exit {rc}, final quit {'logged' if 'command: quit' in log else 'absent'}")
    return verdict("exit-return-env", bad, f"{len(returns)} returns, image environments per page {samples}")


def case_exit_direct(a):
    """EXIT GAME in a session that showed no game page (--disc, and a --game entry booted directly) ends the
    process with exit 0 and no relaunch."""
    d = cl.case_dir(a.out, "exit-direct")
    e = seed_entry(d, "spyro-1", 1)
    env = {"OPENPETE_MOUSE_INJECT": exit_script(d, "exit")}
    bad, legs = [], 0
    for tag, args in (("disc", ["--disc", os.path.join(e, "disc.cue")]), ("game", ["--game", "spyro-1"])):
        rc, log = cl.launch(a.binary, d, tag, ["--launcher-window", "1280x800"] + args, env, timeout=600)
        legs += 1
        if "menu: exit requested" not in log:
            bad.append(f"{tag}: EXIT GAME not reached (exit {rc})")
        if "did not start from the game page: the process exits" not in log or "returning to the launcher" in log:
            bad.append(f"{tag}: the return was not refused, or the process relaunched")
        if rc != 0 or "reason=user-quit" not in log:
            bad.append(f"{tag}: exit {rc}, want 0 with reason user-quit")
    return verdict("exit-direct", bad, f"{legs} legs")


def case_exit_close(a):
    """The quit paths other than EXIT GAME end a session picked on the game page: a window close, and
    SIGTERM sent from outside once the game runs."""
    d = cl.case_dir(a.out, "exit-close")
    seed_entry(d, "spyro-1", 1)
    bad, legs = [], 0
    close_only = os.path.join(d, "close.inject")
    with open(close_only, "w") as f:
        f.write("3000ms close\n")
    for tag in ("close", "sigterm"):
        # The quit is a backstop for a launcher that comes back by mistake; it is timed past the pick's wait
        # for the cold asset cache's launch-time jobs, which runs for seconds under load.
        env = {"OPENPETE_LAUNCHER_SELECT_GAME": "spyro-1", "OPENPETE_LAUNCHER_INPUT": "60000:quit"}
        if tag == "close":
            env["OPENPETE_MOUSE_INJECT"] = close_only
            rc, log = cl.launch(a.binary, d, tag, ["--launcher-window", "1280x800"], env, timeout=600)
        else:
            res = {}
            t = threading.Thread(target=lambda: res.update(r=cl.launch(a.binary, d, tag, ["--launcher-window",
                                                                                          "1280x800"], env,
                                                                       timeout=600)))
            t.start()
            path, sent = os.path.join(d, tag + ".log"), False
            deadline = time.time() + 300
            while t.is_alive() and not sent and time.time() < deadline:
                time.sleep(0.2)
                try:
                    with open(path, errors="replace") as f:
                        up = "boot: identified spyro-1" in f.read()
                except OSError:
                    continue
                if up:
                    time.sleep(4.0)
                    pid = launcher_pid(d, timeout=5.0)
                    if pid:
                        os.kill(pid, signal.SIGTERM)
                        sent = True
            t.join()
            rc, log = res["r"]
            if not sent:
                bad.append(f"{tag}: no SIGTERM sent")
        legs += 1
        if "game picked: spyro-1" not in log:
            bad.append(f"{tag}: no game started (exit {rc})")
        if "returning to the launcher" in log or log.count("stamp first_present") != 1:
            bad.append(f"{tag}: the launcher started again")
        if rc != 0:
            bad.append(f"{tag}: exit {rc}")
    return verdict("exit-close", bad, f"{legs} legs")


def case_mods_chip(a):
    """The Mods chip: drawn at the top-left corner on the game page and the empty library's page, reached
    by key up and the d-pad, pressed by Enter, South and a click, each opening the mods folder the loader
    reads (stubbed), which is created."""
    bad, counts = [], {}
    mods = None
    for tag, lib, acts in (("keys", True, ("key=up", "key=enter", "key=down", "key=right", "key=up", "key=left",
                                           "key=enter", "key=escape")),
                           ("pad", True, ("pad=up", "pad=south", "pad=down", "pad=up", "pad=right", "key=escape")),
                           ("click", False, ("click=@page.mods", "key=escape"))):
        d = cl.case_dir(a.out, "mods-chip-" + tag)
        if lib:
            seed_entry(d, "spyro-1", 2)
            seed_entry(d, "spyro-2", 1)
        mods = os.path.join(d, "data", "openpete", "mods")
        base = os.path.join(d, "m")
        rc, log = input_run(a, d, tag, inputs(*acts), extra={"OPENPETE_LAUNCHER_OPEN_URL_STUB": "1",
                                                             "OPENPETE_LAUNCHER_MANIFEST": base})
        presses = re.findall(r"command: Mods chip pressed \(([\w ]+)\)", log)
        opened = re.findall(r"mods folder: opened (\S+) \(stubbed: nothing opened\)", log)
        ups = len(re.findall(r"command: focus up from box \d+ to the Mods chip", log))
        backs = len(re.findall(r"command: focus from the Mods chip back to box \d+", log))
        counts[tag] = (len(presses), ups, backs)
        want = {"keys": (["Enter or South"] * 2, 2, 1), "pad": (["Enter or South"], 2, 2),
                "click": (["click"], 0, 0)}[tag]
        if presses != want[0] or ups != want[1] or backs != want[2]:
            bad.append(f"{tag}: presses {presses}, ups {ups}, backs {backs}; want {want}")
        if len(opened) != len(presses) or any(os.path.realpath(o) != os.path.realpath(mods) for o in opened):
            bad.append(f"{tag}: opened {opened}, want {mods} once per press")
        if not os.path.isdir(mods):
            bad.append(f"{tag}: {mods} was not created")
        if tag == "keys" and (focus_moves(log) != [("right", 0, 1)] or "left does nothing on the Mods chip" not in log):
            bad.append(f"{tag}: box moves {focus_moves(log)}, want only right 0 -> 1 (left on the chip is inert)")
        screen = "selector" if lib else "drop"
        m = cl.read_json(f"{base}.{screen}.json")
        item = m and next((it for it in m["items"] if it["id"] == "page.mods"), None)
        if not item or item["rect"][0] > 40 or item["rect"][1] > 40:
            bad.append(f"{tag}: page.mods {item and item['rect']} not at the top-left corner of the {screen} screen")
        if rc != 0:
            bad.append(f"{tag}: exit {rc}")
    return verdict("mods-chip", bad, ", ".join(f"{k} (presses, ups, backs) {v}" for k, v in counts.items()))


def case_unsupported_mark(a):
    """The game page marks the box of an entry whose recorded disc build is not supported: a chip below the
    serial's (box.<game>.unsupported in the manifest), clear of the disc hint, at three window sizes; a
    supported entry's box has none."""
    d = cl.case_dir(a.out, "unsupported-mark")
    e1 = seed_entry(d, "spyro-1", 2)
    seed_entry(d, "spyro-2", 1)
    with open(os.path.join(e1, "meta.toml"), "a") as f:
        f.write('disc_build = "recognised"\ndisc_build_warned = 1\n')
    bad, checked = [], 0
    for preset in ("1280x800", "2560x1080", "1024x768"):
        base = os.path.join(d, "m-" + preset)
        rc, log = cl.launch(a.binary, d, preset, ["--launcher-window", preset],
                            {"OPENPETE_LAUNCHER_INPUT": "2500:key=escape", "OPENPETE_LAUNCHER_MANIFEST": base,
                             "OPENPETE_LAUNCHER_SHOT": base}, timeout=300)
        m = cl.read_json(base + ".selector.json")
        items = {it["id"]: it["rect"] for it in (m or {}).get("items", [])}
        mark, serial, hint = items.get("box.spyro-1.unsupported"), items.get("box.spyro-1.id"), items.get("page.hint")
        if not mark or not serial or mark[1] <= serial[3]:
            bad.append(f"{preset}: spyro-1's unsupported chip {mark} not below its serial {serial}")
        elif hint and mark[3] > hint[1]:
            bad.append(f"{preset}: spyro-1's unsupported chip {mark} reaches the disc hint {hint}")
        else:
            checked += 1
        if "box.spyro-2.unsupported" in items:
            bad.append(f"{preset}: spyro-2, a supported entry, is marked")
        if "box of spyro-1 marks an unsupported disc" not in log:
            bad.append(f"{preset}: the mark was not logged")
        if rc != 0:
            bad.append(f"{preset}: exit {rc}")
    return verdict("unsupported-mark", bad, f"{checked} sizes checked")


def case_input(a):
    """T11: keys, pad and pointer drive the game page's focus, activation, file picker and quit."""
    d = cl.case_dir(a.out, "input")
    for i, g in enumerate(DISCS):
        seed_entry(d, g, len(DISCS) - i)
    bad, counts = [], {}

    def expect_moves(tag, log, want):
        got = focus_moves(log)
        counts[tag + " moves"] = len(got)
        if got != want:
            bad.append(f"{tag}: focus moves {got}, want {want}")

    # Keys: no wrap at either edge, up to the Mods chip and down back to the box, Escape quits at once.
    rc, log = input_run(a, d, "keys", inputs("key=left", "key=right", "key=right", "key=right", "key=up",
                                             "key=down", "key=left", "key=escape"))
    expect_moves("keys", log, [("left", 0, 0), ("right", 0, 1), ("right", 1, 2), ("right", 2, 2),
                               ("left", 2, 1)])
    counts["keys chip"] = len(re.findall(r"command: focus (up from box 2 to the Mods chip|from the Mods chip back "
                                         r"to box 2)", log))
    if counts["keys chip"] != 2:
        bad.append(f"keys: {counts['keys chip']} Mods chip moves, want up and back from box 2")
    if rc != 0 or "command: quit (Escape, the window closed" not in log or "game picked" in log:
        bad.append(f"keys: exit {rc}, Escape quit {'logged' if 'Escape or the window' in log else 'absent'}")

    # Pad and stick: the d-pad and the left stick's X move one box per push (the stick re-centres before
    # it moves again); East asks once, the ask lapses after 2 s, a second East inside 2 s quits.
    rc, log = input_run(a, d, "pad", inputs("pad=left", "pad=right", "stick=x:20000", "stick=x:30000",
                                            "stick=x:0", "stick=x:20000", "stick=x:0", "stick=x:-20000",
                                            "stick=x:0", "pad=up", "pad=down", "stick=y:-20000",
                                            "pad=east", 2400, "pad=east", 700, "pad=east"))
    expect_moves("pad", log, [("left", 0, 0), ("right", 0, 1), ("right", 1, 2), ("right", 2, 2),
                              ("left", 2, 1)])
    asks = re.findall(r"command: quit asked", log)
    chip = re.findall(r"command: quit ask chip (shown|hidden)", log)
    counts["pad asks"], counts["pad chip"] = len(asks), len(chip)
    if len(asks) != 2 or chip[:3] != ["shown", "hidden", "shown"]:
        bad.append(f"pad: {len(asks)} asks (want 2), chip {chip} (want shown, hidden, shown)")
    if rc != 0 or not re.search(r"command: quit \(east again \d+ ms after the ask\)", log) or "game picked" in log:
        bad.append(f"pad: exit {rc}, second-East quit {'logged' if 'east again' in log else 'absent'}")
    if len(re.findall(r"command: focus (up from box 1 to the Mods chip|from the Mods chip back to box 1)", log)) != 3:
        bad.append("pad: d-pad up, down and the stick's Y did not move the focus to the Mods chip, back and to it")

    # The pointer takes the focus only after it moves: a key move holds against a still pointer.
    rc, log = input_run(a, d, "pointer", inputs(entry_point(2), "key=left", 900, entry_point(2, 10), "key=escape"))
    expect_moves("pointer", log, [("pointer", 0, 2), ("left", 2, 1), ("pointer", 1, 2)])
    if rc != 0:
        bad.append(f"pointer: exit {rc}")

    # Space and West open the file picker for the focused box's game. The dialog is made to fail
    # (OPENPETE_LAUNCHER_DIALOG_FAIL), as it does on a display with neither the portal nor zenity, so each
    # request opens the drawn picker, which Escape closes; Escape on the page then quits.
    rc, log = input_run(a, d, "browse", inputs("key=right", "key=space", "key=escape", "pad=west", "key=escape",
                                               "key=escape"), extra={"OPENPETE_LAUNCHER_DIALOG_FAIL": "1"})
    pick = re.findall(r"command: file picker for box (\d+) \(([\w-]+)\)", log)
    counts["browse"] = len(pick)
    if len(pick) != 2 or any(b != "1" for b, _ in pick) or rc != 0:
        bad.append(f"browse: exit {rc}, picker lines {pick} (want box 1 twice)")

    # Enter and South activate the focused box: that box's game starts.
    for tag, acts, box in (("enter", ("key=right", "key=enter"), 1), ("south", ("pad=right", "pad=right", "pad=south"), 2)):
        rc, log = input_run(a, d, tag, inputs(*acts), args=["--frame-limit", "5"], timeout=600)
        m = re.search(r"command: activate box (\d+) \(([\w-]+)\)", log)
        counts[tag] = 1 if m else 0
        if not m or int(m.group(1)) != box or rc != 0:
            bad.append(f"{tag}: exit {rc}, activation {m.group(0) if m else 'absent'} (want box {box})")
        else:
            bad += [f"{tag}: {x}" for x in ordered(log, re.escape(m.group(0)), r"game picked: " + m.group(2))]

    # A disc of another game added from the page focuses that game's box when the page returns. The
    # seeded entries' last_played lie past the ingest's time stamp, so the new entry is not the most
    # recently played (box 0), where the focus would start without the added disc's.
    d2 = cl.case_dir(a.out, "input-drop")
    seed_entry(d2, "spyro-1", 4000000001)
    seed_entry(d2, "spyro-2", 4000000000)
    rc, log = cl.launch(a.binary, d2, "drop", ["--launcher-window", "1280x800", "--frame-limit", "5"],
                        {"OPENPETE_LAUNCHER_DROP": disc_cue("spyro-3"), "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
                         "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-3"}, timeout=900)
    found = re.search(r"command: focus starts on box (\d+) \(spyro-3, the disc just added\)", log)
    counts["drop focus"] = 1 if found else 0
    if found and found.group(1) == "0":
        bad.append("drop: the new entry is box 0, so the focus start does not discriminate")
    bad += [f"drop: {x}" for x in ordered(log, r"scripted drop", r"command: focus starts on box \d+ \(spyro-3",
                                          r"game picked: spyro-3")]
    if rc != 0:
        bad.append(f"drop: exit {rc}")
    return verdict("input", bad, ", ".join(f"{k} {v}" for k, v in counts.items()))


def preview_info(d, game):
    """A game's preview.rgba as (width, height, distinct colours), (0, 0, 0) when absent or malformed."""
    p = os.path.join(cache_dir(d, game), "preview.rgba")
    if not os.path.isfile(p):
        return 0, 0, 0
    b = open(p, "rb").read()
    w, h = int.from_bytes(b[8:12], "little"), int.from_bytes(b[12:16], "little")
    if b[:8] != b"OPPREV01" or len(b) != 16 + w * h * 4:
        return 0, 0, 0
    px = b[16:]
    return w, h, len({px[i:i + 3] for i in range(0, len(px), 4 * 97)})


def preview_reruns(log):
    """The launch-time preview rerun lines of a log: (game, trigger)."""
    return re.findall(r"launch-time rerun: preview render of (\S+) \((.*)\)", log)


def preview_step(a, d, tag, bad, env=None, binary=None):
    """One windowed launch over the case's library that waits for the launch-time jobs, then picks Spyro 1."""
    # A step other than "bound" lifts the render's time bound past the launch's, so a loaded machine does not
    # turn its render into the transient that reruns at the next step.
    e = dict(FLOW_ENV, OPENPETE_LAUNCHER_SELECT_GAME="spyro-1", OPENPETE_LAUNCHER_SELECT_IDLE="1",
             OPENPETE_PREVIEW_BOUND_MS="600000")
    e.update(env or {})
    before = cache_snapshot(d)
    rc, log = cl.launch(binary or a.binary, d, tag, ["--launcher-window", "1280x800", "--frame-limit", "5"], e,
                        timeout=900)
    w, r = written(before, cache_snapshot(d)), preview_reruns(log)
    if rc != 0:
        bad.append(f"{tag}: exit {rc}")
    stores = re.findall(r"preview of \S+: .*other guest reads (\d+), guest stores (\d+)", log)
    if any(x[1] != "0" for x in stores):
        bad.append(f"{tag}: the preview build stored to guest memory {stores}")
    print(f"[launcher-flow] preview {tag}: exit {rc}, {len(w)} cache files written, preview reruns {r}")
    return log, w, r


# A box picture's size: 640 wide, two lines per row the game displays (224 rows on Spyro 1, 216 on 2 and 3).
PREVIEW_SIZE = {"spyro-1": (640, 448), "spyro-2": (640, 432), "spyro-3": (640, 432)}


def case_preview(a):
    """The box picture over a three-game library, windowed: cold, warm, a deleted file, another build id, a forced failure, a render past its bound, and a headless launch that neither checks nor logs it."""
    d = cl.case_dir(a.out, "preview")
    seed_library(d)
    bad = []
    log, w, r = preview_step(a, d, "cold", bad)
    expect = {g: "preview.rgba missing" for g in DISCS}
    expect_reruns(bad, "cold", r, expect)
    for g in DISCS:
        pw, ph, colours = preview_info(d, g)
        m = manifest(d, g)
        keys, files = m if m else ({}, {})
        print(f"[launcher-flow] preview {g}: {pw}x{ph}, {colours} sampled colours, view {keys.get('preview_view')}")
        if (pw, ph) != PREVIEW_SIZE[g] or colours < 16:
            bad.append(f"cold: {g}: preview {pw}x{ph} with {colours} sampled colours")
        if files.get("preview.rgba") != (16 + pw * ph * 4 if pw else -1) or not keys.get("preview_disc"):
            bad.append(f"cold: {g}: manifest preview record {keys.get('preview_disc')} {files.get('preview.rgba')}")
        if keys.get("preview_disc") != keys.get("disc_sha256"):
            bad.append(f"cold: {g}: preview_disc differs from disc_sha256")
    log, w, r = preview_step(a, d, "warm", bad)
    current = set(re.findall(r"preview of (\S+) is current", log))
    if r or any(f.endswith("preview.rgba") for f in w) or current != set(DISCS):
        bad.append(f"warm: reruns {r}, current {sorted(current)}")
    os.remove(os.path.join(cache_dir(d, "spyro-2"), "preview.rgba"))
    _, w, r = preview_step(a, d, "deleted", bad)
    expect_reruns(bad, "deleted", r, {"spyro-2": "preview.rgba missing"})
    other = exe_copy(a, d)
    log, w, r = preview_step(a, d, "other-build", bad, binary=other)
    if r or len(re.findall(r"launch-time rerun: asset extraction of", log)) != len(DISCS):
        bad.append(f"other-build: preview reruns {r} (expected none), asset reruns "
                   f"{len(re.findall(r'launch-time rerun: asset extraction of', log))}")
    os.remove(os.path.join(cache_dir(d, "spyro-3"), "preview.rgba"))
    log, w, r = preview_step(a, d, "fail", bad, {"OPENPETE_PREVIEW_FAIL": "spyro-3"}, binary=other)
    m = manifest(d, "spyro-3")
    if "preview of spyro-3 failed: forced" not in log or not m or not m[0].get("preview_failed_build"):
        bad.append("fail: no failure line or no preview failed marker")
    log, w, r = preview_step(a, d, "fail-same-build", bad, binary=other)
    if r or "preview of spyro-3 skipped" not in log:
        bad.append(f"fail-same-build: reruns {r}")
    log, w, r = preview_step(a, d, "fail-other-build", bad)
    expect_reruns(bad, "fail-other-build", r, {"spyro-3": "a preview failed marker of build"})
    os.remove(os.path.join(cache_dir(d, "spyro-1"), "preview.rgba"))
    log, w, r = preview_step(a, d, "bound", bad, {"OPENPETE_PREVIEW_DELAY_MS": "4000",
                                                  "OPENPETE_PREVIEW_BOUND_MS": "1000"})
    m = manifest(d, "spyro-1")
    if "preview of spyro-1 not drawn: past the time bound" not in log or (m and m[0].get("preview_failed_build")):
        bad.append("bound: no transient line, or a marker was written")
    _, w, r = preview_step(a, d, "bound-next", bad)
    expect_reruns(bad, "bound-next", r, {"spyro-1": "preview.rgba missing"})
    rc, log = nox(a, d, "headless", ["--game", "spyro-1", "--frame-limit", "1"])
    lines = re.findall(r".*(?:preview render of|preview of|preview build of).*", log)
    if rc != 0 or lines or reruns(log) or len(set(re.findall(r"asset cache of (\S+) is current", log))) != len(DISCS):
        bad.append(f"headless: exit {rc}, preview lines {lines[:3]}, reruns {reruns(log)}")
    times = [float(x) for x in re.findall(r"preview of \S+ written: .*, ([\d.]+) ms", open(
        os.path.join(d, "cold.log"), errors="replace").read())]
    return verdict("preview", bad, f"cold render times {times} ms")


def case_preview_hold(a):
    """An ingest whose preview outlasts the build holds the progress screen on the render's step, then shows the page; the image is written."""
    d = cl.case_dir(a.out, "preview-hold")
    env = dict(FLOW_ENV, OPENPETE_LAUNCHER_DROP=disc_cue("spyro-1"), OPENPETE_LAUNCHER_SELECT_GAME="spyro-1",
               OPENPETE_LAUNCHER_SELECT_IDLE="1", OPENPETE_PREVIEW_DELAY_MS="6000")
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "5"], env, timeout=900)
    bad = ordered(log, r"\[library\] added", r"preview render of spyro-1 at ingest",
                  r"the progress screen holds for the preview render", r"preview of spyro-1 written",
                  r"the preview hold ends", r"game picked: spyro-1")
    if rc != 0:
        bad.append(f"exit {rc}")
    pw, ph, _ = preview_info(d, "spyro-1")
    if (pw, ph) != PREVIEW_SIZE["spyro-1"]:
        bad.append(f"preview {pw}x{ph}")
    return verdict("preview-hold", bad)


def case_preview_stop(a):
    """A pick of B while A's launch-time preview is prepared and not yet drawn stops A's job within its bound: the stop waits on no present."""
    d = cl.case_dir(a.out, "preview-stop")
    seed_entry(d, "spyro-1", 1)
    seed_entry(d, "spyro-2", 2)
    # The first render parks between its preparation and its draw for 60 s; the pick lands inside the park,
    # 20 s in, after a cold first render (sixteen pipeline compiles) on a loaded machine has parked. The stop
    # is timed in the log, from its start to the job's end: the run's length also holds spyro-2's cold module
    # build after the stop, which a loaded host stretches past a minute.
    env = dict(FLOW_ENV, OPENPETE_PREVIEW_PARK_MS="60000", OPENPETE_PREVIEW_BOUND_MS="90000",
               OPENPETE_LAUNCHER_PICK_DELAY_MS="20000", OPENPETE_LAUNCHER_SELECT_GAME="spyro-2")
    t0 = time.monotonic()
    rc, log = cl.launch(a.binary, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"], env,
                        timeout=900)
    took = time.monotonic() - t0
    stop0 = log_time(log, r"launcher: stopping the launch-time job of spyro-1")
    # The job's stop ends with its last part: the preview's stop, and on a first-boot build the
    # module rebuild's, which a build without the first-boot compile never starts.
    ends = [t for t in (log_time(log, r"launcher: module rebuild of spyro-1 stopped"),
                        log_time(log, r"launcher: preview of spyro-1 stopped; the cache is unchanged")) if t is not None]
    stop1 = max(ends) if ends else None
    park = log_time(log, r"launcher: preview render parked for")
    bad = common_flow(log) + ordered(
        log, r"launch-time rerun: preview render of spyro-1", r"preview render parked for",
        r"scripted pick: entry \d+ \(spyro-2\)", r"stopping the launch-time job of spyro-1",
        r"preview of spyro-1 stopped; the cache is unchanged", r"game picked: spyro-2")
    if rc == -9:
        bad.append("timed out at 900 s")
    elif rc != 0:
        bad.append(f"exit {rc}")
    stop_ms = (stop1 - stop0) * 1000.0 if stop0 is not None and stop1 is not None else None
    if stop_ms is None:
        bad.append("the stop's start or end line is missing")
    elif stop_ms > QUIT_BOUND_MS:
        bad.append(f"the stop took {stop_ms:.0f} ms, over {QUIT_BOUND_MS:.0f} ms: it waited on work")
    if park is not None and stop1 is not None and stop1 >= park + 60.0:
        bad.append("the stop ended after the park: it waited for the draw")
    if preview_info(d, "spyro-1") != (0, 0, 0):
        bad.append("spyro-1 holds a preview from the stopped render")
    m = re.search(r"preview of spyro-1 failed", log)
    if m:
        bad.append("the stopped render wrote a failed marker")
    return verdict("preview-stop", bad, f"stop {stop_ms if stop_ms is not None else float('nan'):.1f} ms, "
                   f"{(stop0 - park) if stop0 is not None and park is not None else float('nan'):.1f} s into the "
                   f"park; run {took:.1f} s")


def case_picker(a):
    """D39: the drawn picker under gamescope and after a failed dialog; the picked disc's game decides its box."""
    bad, counts = [], {}
    # A folder holding Spyro 2's image: its own cue naming the linked bin beside it.
    d = cl.case_dir(a.out, "picker")
    seed_entry(d, "spyro-1", 4000000001)
    seed_entry(d, "spyro-3", 4000000000)
    root = os.path.join(d, "pick-root")
    discs = os.path.join(root, "discs")
    os.makedirs(discs)
    os.makedirs(os.path.join(root, ".hidden"))
    with open(os.path.join(root, "notes.txt"), "w") as f:
        f.write("not a disc\n")
    serial = DISCS["spyro-2"][0]
    os.symlink(disc_bin("spyro-2"), os.path.join(discs, serial + ".bin"))
    with open(os.path.join(discs, serial + ".cue"), "w") as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % serial)
    cue = os.path.join(discs, serial + ".cue")

    # Gamescope: the root list (one root), South into it (.., discs/), East back to the root list, South
    # in again, down to discs/, South (.., the bin, the cue), down twice to the cue, South chooses it.
    acts = inputs("key=space", "pad=south", "pad=east", "pad=south", "pad=down", "pad=south", "pad=down",
                  "pad=down", "pad=south")
    rc, log = cl.launch(a.binary, d, "gamescope", ["--launcher-window", "1280x800", "--frame-limit", "5"],
                        {"GAMESCOPE_WAYLAND_DISPLAY": "gamescope-0", "OPENPETE_LAUNCHER_PICKER_ROOTS": root,
                         "OPENPETE_LAUNCHER_INPUT": acts, "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
                         "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-2"}, timeout=900)
    counts["gamescope dialogs"] = len(re.findall(r"file dialog: requested", log))
    if counts["gamescope dialogs"]:
        bad.append(f"gamescope: {counts['gamescope dialogs']} file dialog requests, want 0")
    listed = re.findall(r"picker: in (.+?): (\d+) folders, (\d+) images", log)
    counts["gamescope listings"] = len(listed)
    want = [(root, "1", "0"), (root, "1", "0"), (discs, "0", "2")]
    if listed != want:
        bad.append(f"gamescope: listings {listed}, want {want}")
    bad += [f"gamescope: {x}" for x in ordered(
        log, r"command: file picker for box 0 \(spyro-1\)", r"picker: open \(gamescope",
        r"picker: the root list", r"picker: the root list", r"picker: chosen " + re.escape(cue),
        r"command: focus starts on box 1 \(spyro-2, the disc just added\)", r"game picked: spyro-2")]
    counts["gamescope chosen"] = len(re.findall(r"picker: chosen ", log))
    if rc != 0:
        bad.append(f"gamescope: exit {rc}")

    # A failed dialog: the drawn picker opens; Escape closes it and leaves the focus; a second request
    # is not refused; Escape on the page quits.
    d2 = cl.case_dir(a.out, "picker-failed")
    for i, g in enumerate(DISCS):
        seed_entry(d2, g, len(DISCS) - i)
    # The first picker frame's shot and manifest are written before the next action: a wait takes only the
    # first command of the events it gathers, so actions must not bunch up behind the readback.
    acts = inputs("key=right", "key=space", 2000, "key=escape", "pad=west", "key=down", "key=escape", "key=escape")
    rc, log = cl.launch(a.binary, d2, "failed", ["--launcher-window", "1280x800"],
                        {"OPENPETE_LAUNCHER_DIALOG_FAIL": "1", "OPENPETE_LAUNCHER_PICKER_ROOTS": root,
                         "OPENPETE_LAUNCHER_INPUT": acts, "OPENPETE_LAUNCHER_SHOT": os.path.join(d2, "shot"),
                         "OPENPETE_LAUNCHER_MANIFEST": os.path.join(d2, "manifest")}, timeout=300)
    # The picker's first frame: its folder line and its one root row drawn, with pixels over the clear.
    man = os.path.join(d2, "manifest.picker.json")
    ids = [it["id"] for it in cl.read_json(man)["items"]] if os.path.isfile(man) else []
    counts["picker rows drawn"] = sum(1 for i in ids if re.fullmatch(r"picker\.row\.\d+", i))
    counts["picker panel"] = ids.count("picker.panel")
    if "picker.path" not in ids or counts["picker rows drawn"] != 1:
        bad.append(f"failed: picker manifest {ids} (want picker.path and one row)")
    m = re.search(r"screen picker: (\d+) of (\d+) pixels drawn", log)
    counts["picker pixels"] = int(m.group(1)) if m else 0
    if not counts["picker pixels"]:
        bad.append("VACUOUS: the picker's first frame drew no pixel over the clear")
    opened = re.findall(r"picker: open \(the file dialog failed\)", log)
    closed = re.findall(r"picker: closed$", log, re.M)
    counts["failed opens"], counts["failed closes"] = len(opened), len(closed)
    if len(opened) != 2 or len(closed) != 2:
        bad.append(f"failed: {len(opened)} picker opens, {len(closed)} closes (want 2 and 2)")
    moves = focus_moves(log)
    if moves != [("right", 0, 1)]:
        bad.append(f"failed: focus moves {moves}, want only the first right")
    bad += [f"failed: {x}" for x in ordered(
        log, r"command: file picker for box 1 \(spyro-2\)", r"file dialog: failed", r"picker: open",
        r"picker: closed", r"command: file picker for box 1 \(spyro-2\)", r"picker: open", r"picker: closed",
        r"command: quit \(Escape, the window closed")]
    if rc != 0 or "game picked" in log:
        bad.append(f"failed: exit {rc}{', a game was picked' if 'game picked' in log else ''}")
    return verdict("picker", bad, ", ".join(f"{k} {v}" for k, v in counts.items()))


# ---- the game page's errors (error-*): the message panel, refusals before the copy, the failed-build box ----

SECTOR = 2352
TRUNC_SECTORS = 1200   # past SYSTEM.CNF (sector 23) and the boot executable's first sectors on all three discs


def write_image(d, name, data):
    """A disc image under d/images: name.bin holding data and its one-track cue; returns the cue."""
    img = os.path.join(d, "images")
    os.makedirs(img, exist_ok=True)
    with open(os.path.join(img, name + ".bin"), "wb") as f:
        f.write(data)
    cue = os.path.join(img, name + ".cue")
    with open(cue, "w") as f:
        f.write(f'FILE "{name}.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    return cue


def truncated_image(d, name, game, serial_from=None, serial_to=None):
    """The first TRUNC_SECTORS sectors of a game's disc, its SYSTEM.CNF serial optionally replaced; returns the cue."""
    b = disc_bin(game)
    if not os.path.isfile(b):
        raise cl.Skip(f"{b} missing")
    with open(b, "rb") as f:
        data = bytearray(f.read(TRUNC_SECTORS * SECTOR))
    if serial_from:
        # Every occurrence: the root directory's record of the boot file and SYSTEM.CNF's BOOT line.
        if data.find(serial_from) < 0:
            raise RuntimeError(f"{serial_from!r} not found in {b}")
        data = bytearray(bytes(data).replace(serial_from, serial_to))
    return write_image(d, name, bytes(data))


def library_bytes(d):
    """The bytes of every file under the case's library but its lock file, and the ingest-tmp directories present."""
    lib = os.path.join(d, "data", "openpete", "library")
    total, tmps = 0, []
    for root, dirs, files in os.walk(lib):
        tmps += [x for x in dirs if x.startswith("ingest-tmp-")]
        total += sum(os.path.getsize(os.path.join(root, f)) for f in files if f != ".lock")
    return total, tmps


def refused_clean(tag, log, d, bad):
    """Problems with a refusal before the copy: one refusal line, no copy started, nothing in the library."""
    n = len(re.findall(r"ingest of .* refused before the copy", log))
    written, tmps = library_bytes(d)
    if n != 1:
        bad.append(f"{tag}: {n} refusal lines, want 1")
    if "copying disc into your library" in log or "COPYING DISC" in log:
        bad.append(f"{tag}: the copy started")
    if written or tmps:
        bad.append(f"{tag}: {written} bytes written to the library, ingest-tmp {tmps}")
    return n, written


def case_error_refuse(a):
    """T12's refusals before the copy: a non-PSX file, a zero-filled image (the negative control: 0 bytes
    written), a disc whose serial no game claims, headless (exit 3) and by a drop on the page (the panel, East
    dismisses it without quitting, the page stays usable); a truncated real disc as the positive control is
    identified and copied."""
    d0 = cl.case_dir(a.out, "error-refuse")
    images = {
        "zero": (write_image(d0, "zero", bytes(600 * SECTOR)), r"has no usable SYSTEM\.CNF boot line"),
        "not-a-disc": (write_image(d0, "notadisc", b"this is not a disc image\n" * 40),
                       r"cannot be read as a raw disc image"),
        "unclaimed": (truncated_image(d0, "unclaimed", "spyro-1", b"SCUS_942.28", b"SLUS_999.99"),
                      r"has no usable SYSTEM\.CNF boot line or boot program"),
    }
    bad, counts = [], {}
    for tag, (cue, why) in images.items():
        d = os.path.join(d0, tag)
        os.makedirs(d)
        rc, log = headless(a, d, "ingest", ["--ingest", cue, "--frame-limit", "1"], timeout=300)
        n, written = refused_clean(tag, log, d, bad)
        counts[tag] = f"{n} refusal, {written} bytes"
        if rc != 3 or not re.search(why, log):
            bad.append(f"{tag}: exit {rc} (want 3), reason {'logged' if re.search(why, log) else 'absent'}")
    # Windowed: the zero-filled image dropped on the empty page.
    d = os.path.join(d0, "drop")
    os.makedirs(d)
    env = {"OPENPETE_LAUNCHER_DROP": images["zero"][0], "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
           "OPENPETE_LAUNCHER_INPUT": "4000:pad=east,5000:key=right,6500:quit"}
    rc, log = cl.launch(a.binary, d, "drop", ["--launcher-window", "1280x800"], env, timeout=300)
    n, written = refused_clean("drop", log, d, bad)
    bad += [f"drop: {x}" for x in ordered(
        log, r"scripted drop", r"ingest of .* refused before the copy", r"error panel raised: zero\.cue",
        r"error panel drawn: \d+ lines", r"scripted input: pad=east", r"error panel dismissed \(east\)",
        r"scripted input: key=right", r"command: focus right from box 0 to box 1", r"command: quit")]
    between = log[log.find("error panel dismissed (east)"):log.find("scripted input: key=right")]
    if "quit asked" in between:
        bad.append("drop: the dismissing East also asked to quit")
    if rc != 0:
        bad.append(f"drop: exit {rc}")
    counts["drop"] = f"{n} refusal, {written} bytes, panel {'drawn' if 'error panel drawn' in log else 'not drawn'}"
    # Over a dome: a library entry whose launch-time asset job loads the sky, then the drop; the panel's fill
    # draws at full opacity and the first such frame is read back (shot tag "panel").
    d = os.path.join(d0, "dome")
    os.makedirs(d)
    seed_entry(d, "spyro-1", 1)
    env = {"OPENPETE_LAUNCHER_DROP": images["zero"][0], "OPENPETE_LAUNCHER_PICK_DELAY_MS": "9000",
           "OPENPETE_LAUNCHER_INPUT": "16000:quit", "OPENPETE_LAUNCHER_SHOT": os.path.join(d, "shot"),
           "OPENPETE_RECOMP_BUILD_FAIL": "1"}
    rc, log = cl.launch(a.binary, d, "dome", ["--launcher-window", "1280x800"], env, timeout=600)
    shot = re.search(r"screen panel: (\d+) of (\d+) pixels drawn over the clear \(shot (\S+),", log)
    # The first fill's opacity is half the dome cover at that frame: the drop can land inside the sky's reveal,
    # whose start waits for the box picture's render. The panel shot is taken only at full cover.
    bad += [f"dome: {x}" for x in ordered(log, r"sky: \d+ domes of spyro-1", r"scripted drop",
                                           r"error panel raised: zero\.cue", r"error panel fill drawn at")]
    fill = re.search(r"error panel fill drawn at ([\d.]+) opacity \(dome cover ([\d.]+)\)", log)
    if fill and abs(float(fill.group(1)) - 0.5 * float(fill.group(2))) > 0.006:
        bad.append(f"dome: fill {fill.group(1)} is not half the dome cover {fill.group(2)}")
    if not shot or int(shot.group(1)) == 0:
        bad.append(f"dome: no panel shot ({shot.group(0) if shot else 'no screen panel line'})")
    counts["dome"] = f"shot {shot.group(3) if shot else 'none'}"
    # Positive control: a truncated real disc is identified and its copy written.
    d = os.path.join(d0, "control")
    os.makedirs(d)
    cue = truncated_image(d0, "real", "spyro-1")
    env = {"OPENPETE_LAUNCHER_DROP": cue, "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
           "OPENPETE_LAUNCHER_INPUT": "8000:quit", "OPENPETE_RECOMP_BUILD_FAIL": "1"}
    rc, log = cl.launch(a.binary, d, "control", ["--launcher-window", "1280x800"], env, timeout=600)
    written, _ = library_bytes(d)
    bad += [f"control: {x}" for x in ordered(log, r"scripted drop", r"identified before the copy",
                                              r"\[library\] added")]
    if has_first_boot(a):   # the forced build failure removes the copy the ingest made
        bad += [f"control: {x}" for x in ordered(log, r"\[library\] added", r"removed: the disc added to the library")]
        if written != 0:
            bad.append(f"control: {written} bytes left in the library after the failed build")
    elif written < TRUNC_SECTORS * SECTOR:
        bad.append(f"control: {written} bytes in the library, want at least {TRUNC_SECTORS * SECTOR}")
    counts["control"] = f"{written} bytes"
    return verdict("error-refuse", bad, "; ".join(f"{k}: {v}" for k, v in counts.items()))


def exe_without(a, d, game):
    """A copy of the binary beside a recomp tree linking the build's but for one game's manifest; returns its path."""
    x = exe_copy(a, d)
    rec = os.path.join(os.path.dirname(x), "recomp")
    os.remove(rec)
    os.makedirs(os.path.join(rec, "manifest"))
    src = os.path.join(os.path.abspath(a.build_dir), "recomp")
    for name in os.listdir(src):
        if name != "manifest":
            os.symlink(os.path.join(src, name), os.path.join(rec, name))
    for g in os.listdir(os.path.join(src, "manifest")):
        if g != game:
            os.symlink(os.path.join(src, "manifest", g), os.path.join(rec, "manifest", g))
    return x


def case_error_staged(a):
    """T12's not-staged refusal: an executable without Spyro 2's first-boot inputs draws no box for it and refuses
    its disc before the copy, headless (exit 3) and by a drop (the panel names the game)."""
    need_first_boot(a)
    d0 = cl.case_dir(a.out, "error-staged")
    x = exe_without(a, d0, "spyro-2")
    cue = truncated_image(d0, "s2", "spyro-2")
    bad, counts = [], {}
    d = os.path.join(d0, "headless")
    os.makedirs(d)
    rc, log = headless(a, d, "ingest", ["--ingest", cue, "--frame-limit", "1"], binary=x, timeout=300)
    n, written = refused_clean("headless", log, d, bad)
    if rc != 3 or "spyro-2, whose first-boot inputs this package does not stage" not in log:
        bad.append(f"headless: exit {rc}, reason {'logged' if 'does not stage' in log else 'absent'}")
    counts["headless"] = f"{n} refusal, {written} bytes"
    d = os.path.join(d0, "drop")
    os.makedirs(d)
    env = {"OPENPETE_LAUNCHER_DROP": cue, "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
           "OPENPETE_LAUNCHER_INPUT": "4000:quit"}
    rc, log = cl.launch(x, d, "drop", ["--launcher-window", "1280x800"], env, timeout=300)
    n, written = refused_clean("drop", log, d, bad)
    bad += [f"drop: {x_}" for x_ in ordered(log, r"game page: no box for spyro-2", r"game page: 2 boxes",
                                             r"error panel raised: s2\.cue \(serial SCUS-94425\) is Spyro 2",
                                             r"error panel drawn")]
    if rc != 0:
        bad.append(f"drop: exit {rc}")
    counts["drop"] = f"{n} refusal, {written} bytes"
    return verdict("error-staged", bad, "; ".join(f"{k}: {v}" for k, v in counts.items()))


def case_error_build(a):
    """T12's failed builds (windowed): a launch-time rebuild forced to fail with the page up raises the panel and
    draws the failed-build box; the next launch draws it without a rebuild; a --game launch shows the page with the
    box focused and the panel (headless: exit 3); activating the box retries from the read step; an ingest whose
    build is forced to fail removes the disc copy it made and returns to the page with the panel."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "error-build")
    e = seed_entry(d, "spyro-1", 1)
    marker = os.path.join(e, "build-failed.toml")
    fail = {"OPENPETE_RECOMP_BUILD_FAIL": "1"}
    bad, counts = [], {}
    rc, log = input_run(a, d, "launch-time", "20000:quit", fail, timeout=600)
    bad += [f"launch-time: {x}" for x in ordered(
        log, r"game page: \d+ boxes", r"launch-time module rebuild of spyro-1", r"module rebuild of spyro-1 failed",
        r"game page: box of spyro-1 shows the failed-build drawing",
        r"error panel raised: Spyro the Dragon could not be built from its disc \(step: .*\)\. The log file",
        r"error panel drawn", r"error panel fill drawn")]
    if rc != 0 or not os.path.isfile(marker):
        bad.append(f"launch-time: exit {rc}, marker {'present' if os.path.isfile(marker) else 'absent'}")
    counts["launch-time panels"] = len(re.findall(r"error panel raised", log))
    rc, log = input_run(a, d, "next", "3000:quit", timeout=300)
    if not re.search(r"game page: \d+ boxes .*spyro-1 frame \d+ vertices, failed build", log) or \
            "launch-time job skipped" not in log or "launch-time module rebuild" in log or rc != 0:
        bad.append(f"next: exit {rc}, failed box {'drawn' if 'failed build' in log else 'absent'}, rebuild "
                   f"{'ran' if 'launch-time module rebuild' in log else 'not run'}")
    rc, log = input_run(a, d, "game", "3000:pad=east,4500:quit", args=["--game", "spyro-1"], timeout=300)
    bad += [f"--game: {x}" for x in ordered(log, r"holds a failed module build of this build",
                                             r"error panel raised: Spyro the Dragon could not be built",
                                             r"command: focus starts on box 0 \(spyro-1", r"error panel drawn",
                                             r"error panel dismissed \(east\)", r"command: quit")]
    if rc != 0 or "game picked" in log:
        bad.append(f"--game: exit {rc}")
    rc, _ = headless(a, d, "game-headless", ["--game", "spyro-1", "--frame-limit", "1"])
    if rc != 3:
        bad.append(f"--game headless: exit {rc}, want 3")
    rc, log = input_run(a, d, "retry", "3000:key=enter,20000:quit", fail, timeout=600)
    bad += [f"retry: {x}" for x in ordered(log, r"command: activate box 0 \(spyro-1\)",
                                            r"failed-build marker removed", r"build failed at the read step",
                                            r"failed-build marker written", r"error panel raised: Spyro the Dragon")]
    if not os.path.isfile(marker):
        bad.append("retry: no marker after the failed retry")
    # An ingest whose build fails: the copy goes, the empty box stays usable, the page returns with the panel.
    d2 = os.path.join(d, "ingest")
    os.makedirs(d2)
    env = dict(fail, OPENPETE_LAUNCHER_DROP=disc_cue("spyro-2"), OPENPETE_LAUNCHER_PICK_DELAY_MS="500",
               OPENPETE_LAUNCHER_INPUT="60000:quit")
    rc, log = cl.launch(a.binary, d2, "ingest", ["--launcher-window", "1280x800"], env, timeout=900)
    copies = glob.glob(os.path.join(d2, "data", "openpete", "library", "*", "disc.bin"))
    markers = glob.glob(os.path.join(d2, "data", "openpete", "library", "*", "build-failed.toml"))
    bad += [f"ingest: {x}" for x in ordered(log, r"\[library\] added", r"build failed at the read step",
                                             r"entry \S+ removed: the disc added to the library did not build",
                                             r"error panel raised: This copy of Spyro 2: Ripto's Rage! could not be added",
                                             r"command: focus starts on box 1 \(spyro-2", r"error panel drawn")]
    if copies or markers or "no longer needed" in log:
        bad.append(f"ingest: copies {copies}, markers {markers}, redundancy line {'no longer needed' in log}")
    if rc != 0:
        bad.append(f"ingest: exit {rc}")
    return verdict("error-build", bad, ", ".join(f"{k} {v}" for k, v in counts.items()))


def error_anchors():
    """The (id, anchor) rows of OPENPETE_ERROR_IDS, and the anchors the troubleshooting page holds (<a id=...>)."""
    with open(os.path.join(ROOT, "core", "include", "openpete_error_ids.h")) as f:
        rows = re.findall(r'X\(\s*\w+\s*,\s*"(OP-E-[A-Z-]+)"\s*,\s*"([a-z-]+)"\s*\)', f.read())
    with open(os.path.join(ROOT, "docs", "src", "user", "help", "troubleshooting.md")) as f:
        page = set(re.findall(r'<a id="([a-z0-9-]+)"></a>', f.read()))
    return rows, page


def altered_disc(game, folder):
    """Write a copy of a game's image into a scratch folder with every sector of WAD.WAD's payload inverted, its
    table of contents kept: the serial and the file system read as the known disc's, the game's data does not.
    Returns the cue's path."""
    with open(disc_bin(game), "rb") as f:
        raw = bytearray(f.read())
    def sector(lba):
        return raw[lba * 2352 + 24: lba * 2352 + 24 + 2048]
    root = sector(16)[156:190]
    root_lba, root_size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    wad, wad_size = None, 0
    for n in range(root_size // 2048):
        s, at = sector(root_lba + n), 0
        while at < 2048 and s[at]:
            name = bytes(s[at + 33: at + 33 + s[at + 32]]).split(b";")[0]
            if name == b"WAD.WAD":
                wad, wad_size = struct.unpack_from("<I", s, at + 2)[0], struct.unpack_from("<I", s, at + 10)[0]
            at += s[at]
    if wad is None:
        raise cl.Skip(f"{disc_bin(game)} holds no WAD.WAD")
    for lba in range(wad + 8, wad + wad_size // 2048):
        o = lba * 2352 + 24
        raw[o:o + 2048] = bytes(b ^ 0xFF for b in raw[o:o + 2048])
    os.makedirs(folder)
    serial = DISCS[game][0]
    with open(os.path.join(folder, serial + ".bin"), "wb") as f:
        f.write(raw)
    cue = os.path.join(folder, serial + ".cue")
    with open(cue, "w") as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % serial)
    return cue


def case_ingest_retry(a):
    """A disc that passes the serial check and cannot build: no redundancy line, the copy removed, the box empty, and
    another copy added from that box by the pad plays. An older failed entry neither refuses a good disc nor outlives it."""
    need_first_boot(a)
    d = cl.case_dir(a.out, "ingest-retry")
    bad, counts = [], {}
    fixture = os.path.join(d, "fixture")
    bad_cue = altered_disc("spyro-3", os.path.join(fixture, "bad"))
    root = os.path.join(fixture, "root")
    os.makedirs(root)
    serial = DISCS["spyro-3"][0]
    os.symlink(disc_bin("spyro-3"), os.path.join(root, serial + ".bin"))
    good_cue = os.path.join(root, serial + ".cue")
    with open(good_cue, "w") as f:
        f.write('FILE "%s.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' % serial)

    # The bad disc drops at the first frame. East closes the panel, South on the focused empty box opens the drawn
    # picker, South enters the root, down twice reaches the cue (.., the bin, the cue), South chooses it.
    acts = inputs(25000, "pad=east", "pad=south", "pad=south", "pad=down", "pad=down", "pad=south")
    run = os.path.join(d, "same-session")
    os.makedirs(run)
    env = {"OPENPETE_LAUNCHER_DROP": bad_cue, "GAMESCOPE_WAYLAND_DISPLAY": "gamescope-0",
           "OPENPETE_LAUNCHER_PICKER_ROOTS": root, "OPENPETE_LAUNCHER_INPUT": acts,
           "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500", "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-3"}
    rc, log = cl.launch(a.binary, run, "same-session", ["--launcher-window", "1280x800", "--frame-limit", "5"], env,
                        timeout=900)
    bad += [f"same session: {x}" for x in ordered(
        log, r"ingest of .*: serial SCUS-94467, spyro-3; identified before the copy", r"\[library\] added",
        r"asset extraction of spyro-3 failed", r"entry \S+ removed: the disc added to the library did not build",
        r"error panel raised: This copy of Spyro: Year of the Dragon could not be added",
        r"error panel dismissed", r"command: activate box 2 \(spyro-3\)", r"picker: open",
        r"picker: chosen " + re.escape(good_cue),
        r"\[library\] added", r"your original .* is no longer needed", r"game picked: spyro-3")]
    first_panel = log.find("error panel raised")
    if 0 <= log.find("no longer needed") < first_panel:
        bad.append("same session: the redundancy line came before the failure")
    lib = os.path.join(run, "data", "openpete", "library")
    copies = glob.glob(os.path.join(lib, "*", "disc.bin"))
    counts["copies left"] = len(copies)
    if len(copies) != 1 or os.path.getsize(copies[0]) != os.path.getsize(disc_bin("spyro-3")) or \
            glob.glob(os.path.join(lib, "*", "build-failed.toml")):
        bad.append(f"same session: copies {copies}")
    if rc != 0:
        bad.append(f"same session: exit {rc}")

    # An entry left failed by an earlier release: the good disc is not refused, boots ahead of it and replaces it.
    run = os.path.join(d, "older-entry")
    os.makedirs(run)
    e = seed_entry(run, "spyro-3", 4000000000, tag="00000000failed")
    os.remove(os.path.join(e, "disc.bin"))
    os.symlink(os.path.splitext(bad_cue)[0] + ".bin", os.path.join(e, "disc.bin"))
    with open(os.path.join(e, "build-failed.toml"), "w") as f:
        f.write('build = "an earlier build"\nstep = "start"\n')
    env = {"OPENPETE_LAUNCHER_DROP": good_cue, "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500",
           "OPENPETE_LAUNCHER_SELECT_GAME": "spyro-3"}
    rc, log = cl.launch(a.binary, run, "older-entry", ["--launcher-window", "1280x800", "--frame-limit", "5"], env,
                        timeout=900)
    bad += [f"older entry: {x}" for x in ordered(
        log, r"identified before the copy", r"entry 00000000failed removed: another disc of its game has built",
        r"your original .* is no longer needed", r"game picked: spyro-3")]
    if "already holds a disc" in log or os.path.isdir(e) or rc != 0:
        bad.append(f"older entry: exit {rc}, entry {'kept' if os.path.isdir(e) else 'gone'}")
    shutil.rmtree(fixture, ignore_errors=True)
    return verdict("ingest-retry", bad, ", ".join(f"{k} {v}" for k, v in counts.items()))


def case_error_link(a):
    """The panel's reference line is a link: a click on it opens the error's troubleshooting URL (stubbed through
    OPENPETE_LAUNCHER_OPEN_URL_STUB) and leaves the panel shown; a click on the message line opens nothing and
    dismisses it; Enter and South open the URL, then dismiss it. Every error id's anchor is on the troubleshooting page."""
    bad, counts = [], {}
    rows, page = error_anchors()
    missing = [i for i, anc in rows if anc not in page]
    counts["ids"], counts["anchors resolved"] = len(rows), len(rows) - len(missing)
    if not rows or missing:
        bad.append(f"anchors: {len(rows)} ids, missing on the page {missing}")
    d0 = cl.case_dir(a.out, "error-link")
    cue = write_image(d0, "zero", bytes(600 * SECTOR))
    stub = {"OPENPETE_LAUNCHER_OPEN_URL_STUB": "1", "OPENPETE_LAUNCHER_DROP": cue,
            "OPENPETE_LAUNCHER_PICK_DELAY_MS": "500"}
    opened_re = r"error panel link: opened (\S+) \(stubbed"

    def panel_ref(log):
        m = re.search(r"error panel raised: .*\[(OP-E-[A-Z-]+): (\S+)\]", log)
        return (m.group(1), m.group(2)) if m else (None, None)

    # Positive: hover and click the reference line, then click the message line, which dismisses.
    d = os.path.join(d0, "link")
    os.makedirs(d)
    rc, log = cl.launch(a.binary, d, "link", ["--launcher-window", "1280x800"],
                        dict(stub, OPENPETE_LAUNCHER_INPUT="4000:click=@panel.error.ref,"
                                                           "5500:click=@panel.error.line.0,7000:quit"), timeout=300)
    eid, url = panel_ref(log)
    opened = re.findall(opened_re, log)
    counts["link opens"] = len(opened)
    if not url or opened != [url]:
        bad.append(f"link: opened {opened}, want [{url}] (the panel's reference)")
    if url and not url.startswith("https://openpete.com/docs/user/help/troubleshooting.html#"):
        bad.append(f"link: {url} is not on the published troubleshooting page")
    if url and url.split("#", 1)[1] not in page:
        bad.append(f"link: anchor of {url} is not on the page")
    bad += [f"link: {x}" for x in ordered(
        log, r"error panel drawn", r"scripted input: click=@panel\.error\.ref",
        r"error panel link: hovered, the hand cursor shows", r"error panel link: pressed", opened_re,
        r"scripted input: click=@panel\.error\.line\.0", r"error panel dismissed \(a click\)", r"command: quit")]
    between = log[log.find("error panel link: pressed"):log.find("scripted input: click=@panel.error.line.0")]
    if "error panel dismissed" in between:
        bad.append("link: the link's click dismissed the panel")
    if "no such element" in log:
        bad.append("link: a scripted click found no element")
    if rc != 0:
        bad.append(f"link: exit {rc}")
    counts["id"] = eid
    # Negative: a click on the message line, and in another run Enter: each dismisses, nothing opens.
    d = os.path.join(d0, "elsewhere")
    os.makedirs(d)
    rc, log = cl.launch(a.binary, d, "elsewhere", ["--launcher-window", "1280x800"],
                        dict(stub, OPENPETE_LAUNCHER_INPUT="4000:click=@panel.error.line.0,5500:quit"), timeout=300)
    counts["elsewhere opens"] = len(re.findall(r"error panel link: (opened|pressed)", log))
    if counts["elsewhere opens"]:
        bad.append(f"elsewhere: {counts['elsewhere opens']} link lines, want 0")
    bad += [f"elsewhere: {x}" for x in ordered(log, r"error panel drawn",
                                                r"scripted input: click=@panel\.error\.line\.0",
                                                r"error panel dismissed \(a click\)")]
    # Enter and South open the entry, then dismiss the panel.
    rcs = [rc]
    for tok, name in (("key=enter", "enter"), ("pad=south", "south")):
        d = os.path.join(d0, name)
        os.makedirs(d)
        rck, log = cl.launch(a.binary, d, name, ["--launcher-window", "1280x800"],
                             dict(stub, OPENPETE_LAUNCHER_INPUT=f"4000:{tok},5500:quit"), timeout=300)
        rcs.append(rck)
        _, kurl = panel_ref(log)
        kopened = re.findall(opened_re, log)
        counts[f"{name} opens"] = len(kopened)
        if not kurl or kopened != [kurl]:
            bad.append(f"{name}: opened {kopened}, want [{kurl}]")
        bad += [f"{name}: {x}" for x in ordered(log, r"error panel drawn", rf"scripted input: {re.escape(tok)}",
                                                 r"error panel link: activated", opened_re,
                                                 r"error panel dismissed \(activate\)")]
    if any(rcs):
        bad.append(f"elsewhere/enter/south runs: exit {rcs}")
    return verdict("error-link", bad, ", ".join(f"{k} {v}" for k, v in counts.items()))


CASES = {"startup": case_startup, "resolution": case_resolution, "multi-install": case_multi_install,
         "multi-install-extract": case_multi_install_extract,
         "job-ingest-stop": case_job_ingest_stop, "job-pick-same": case_job_pick_same,
         "job-pool-low": case_job_pool_low,
         "job-pick-other": case_job_pick_other, "lock": case_lock, "failed-marker": case_failed_marker,
         "headless-no-disc": case_headless_no_disc, "bind-repair": case_bind_repair,
         "ingest-held": case_ingest_held, "cache": case_cache, "cache-ingest": case_cache_ingest,
         "multi-install-launch": case_multi_install_launch,
         "job-ingest-stop-assets": case_job_ingest_stop_assets,
         "job-pick-other-assets": case_job_pick_other_assets,
         "shader-step-during": case_shader_step_during, "shader-step-after": case_shader_step_after,
         "shader-step-order": case_shader_step_order, "input": case_input, "picker": case_picker,
         "sky-cache": case_sky_cache, "sky-reveal": case_sky_reveal,
         "preview": case_preview, "preview-hold": case_preview_hold,
         "preview-stop": case_preview_stop,
         "quit-copy": case_quit_copy, "quit-translate": case_quit_translate, "quit-build": case_quit_build,
         "quit-assets": case_quit_assets, "quit-mods": case_quit_mods, "quit-east-once": case_quit_east_once,
         "quit-east-twice": case_quit_east_twice, "quit-escape": case_quit_escape, "quit-close": case_quit_close,
         "exit-return": case_exit_return, "exit-return-env": case_exit_return_env, "exit-direct": case_exit_direct, "exit-close": case_exit_close,
         "exit-fullscreen": case_exit_fullscreen,
         "mods-chip": case_mods_chip, "unsupported-mark": case_unsupported_mark,
         "quit-page-sigterm": case_quit_page_sigterm, "error-refuse": case_error_refuse,
         "error-staged": case_error_staged, "error-build": case_error_build, "ingest-retry": case_ingest_retry,
         "error-link": case_error_link}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("case", choices=sorted(CASES))
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build", "linux-release"))
    ap.add_argument("--out", default=None)
    ap.add_argument("--measure", action="store_true")
    a = ap.parse_args()
    a.binary = os.path.join(a.build_dir, "openpete")
    a.out = a.out or os.path.join(a.build_dir, "launcher-tests")
    cl.WM = os.path.join(a.build_dir, "openpete_xvfb_wm")
    cl.install_reaper()
    if not os.access(a.binary, os.X_OK):
        print(f"[launcher-flow] SKIP: {a.binary} missing")
        return 64
    try:
        return CASES[a.case](a)
    except cl.Skip as e:
        print(f"[launcher-flow] SKIP: {e}")
        return 64


if __name__ == "__main__":
    sys.exit(main())
