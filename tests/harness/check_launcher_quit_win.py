#!/usr/bin/env python3
"""check_launcher_quit_win.py - check_launcher_flow.py's quit-* cases against a windows-mingw-launcher
package, driven from WSL through interop.

The package is an extracted windows-mingw-launcher zip (scripts/package-dist.sh windows, with the bundled
llvm-mingw) on a Windows drive. Each case runs the exe windowed under OPENPETE_SANDBOX=<case directory> (a
short path under --root, so the module build's paths stay under MAX_PATH), with TMP and TEMP in the case
directory, the scripted gesture of OPENPETE_LAUNCHER_QUIT_AT, and the case's knobs forwarded through WSLENV
(scripts/lib/hostenv.child_env). A library entry's disc.bin is a hard link of one staged copy of the disc
under --root/disc.

Per case: the quit-to-exit time (from the log line "quit during work (" seen in the exe's stdout to the
interop process's exit, and the launcher's own figure) against check_launcher_flow.QUIT_BOUND_MS plus the
preview compile the stop found in flight; the leftovers check_launcher_flow.leftovers counts, with the
module test reading the PE export table and the process count taken on the Windows side (every process
whose image is openpete.exe, PSXRecomp.exe or an llvm-mingw tool, or whose command line names the case
directory); with --next on quit-build, a headless launch that must rebuild the module in the background
and play.

Cases: prime (builds the module quit-assets and quit-mods seed), quit-copy, quit-translate, quit-build,
quit-assets, quit-east-twice, quit-escape, quit-close, quit-mods (check_launcher_flow.py's quit-mods for
Spyro 1: a quit event while the pick compiles a source-only mod; the mod stays stale, the game does not
start, no compiler survives, and the next launch compiles it).

Usage: tests/harness/check_launcher_quit_win.py --app DIR --root DIR CASE...
Exit 0 PASS, 1 FAIL.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "scripts", "lib"))
import check_launcher_flow as fl  # noqa: E402
from hostenv import child_env  # noqa: E402

GAME = "spyro-1"
MODULE = "spyro1-recomp.dll"
TOOL_IMAGES = re.compile(r"^(openpete|PSXRecomp|clang.*|x86_64-w64-mingw32-.*|ld\.lld|lld|ld|llvm-.*)\.exe$", re.I)


def win(p):
    """The Windows spelling of a path on a Windows drive."""
    return subprocess.run(["wslpath", "-w", p], capture_output=True, text=True, check=True).stdout.strip()


def case_dir(a, name):
    """The case's directory under --root, emptied."""
    d = os.path.join(a.root, name)
    if os.path.isdir(d):
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "tmp"))
    return d


def lib_dir(d):
    return os.path.join(d, "data", "openpete", "library")


def seed_entry(a, d, last_played=1):
    """A Spyro 1 entry with no module, disc.bin a hard link of the staged disc."""
    e = os.path.join(lib_dir(d), "0000000000" + GAME)
    os.makedirs(e)
    os.link(os.path.join(a.root, "disc", "SCUS-94228.bin"), os.path.join(e, "disc.bin"))
    with open(os.path.join(e, "disc.cue"), "w") as f:
        f.write('FILE "disc.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n')
    with open(os.path.join(e, "meta.toml"), "w") as f:
        f.write('label = "Spyro the Dragon (launcher test)"\nsource = "SCUS-94228.bin"\nsha256 = "%s"\n'
                'last_played = %d\ngame = "%s"\n' % ("0" * 64, last_played, GAME))
    return e


def stamped(d):
    return glob.glob(os.path.join(lib_dir(d), "*", "modules", "*", MODULE))


def whole_module(path):
    """Whether a file is a PE module exporting openpete_game_recomp_module."""
    if not os.path.isfile(path):
        return False
    out = subprocess.run(["x86_64-w64-mingw32-objdump", "-p", path], capture_output=True, text=True).stdout
    return "openpete_game_recomp_module" in out


def win_processes(d):
    """Windows processes of the tool images, or whose command line names the case directory."""
    key = win(d).lower()
    ps = ("Get-CimInstance Win32_Process | ForEach-Object { \"$($_.ProcessId)`t$($_.Name)`t$($_.CommandLine)\" }")
    out = subprocess.run(["powershell.exe", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    found = []
    for line in out.splitlines():
        parts = line.rstrip("\r").split("\t", 2)
        if len(parts) < 2 or parts[1].lower() == "powershell.exe":
            continue
        cmd = parts[2] if len(parts) > 2 else ""
        if TOOL_IMAGES.match(parts[1]) or key in cmd.lower():
            found.append(f"{parts[0]} {parts[1]} {cmd[:100]}")
    return found


def leftovers(d, stopped_cache=None):
    """check_launcher_flow.leftovers on the Windows layout."""
    lib = lib_dir(d)
    found = {
        "ingest-tmp": glob.glob(os.path.join(lib, "ingest-tmp-*")),
        "failed-markers": glob.glob(os.path.join(lib, "*", "build-failed.toml")),
        "partial-modules": [m for m in glob.glob(os.path.join(lib, "*", "modules", "*", "*-recomp.dll"))
                            if not whole_module(m)],
        "build-dirs": glob.glob(os.path.join(lib, "*", "modules", "*", "build")),
        "atomic-temps": [os.path.join(r, f) for root in ("data", "state") for r, _, fs in os.walk(os.path.join(d, root))
                         for f in fs if ".tmp-" in f],
        "tmpdir-files": [os.path.join(r, f) for r, _, fs in os.walk(os.path.join(d, "tmp")) for f in fs],
        "processes": win_processes(d),
    }
    if stopped_cache:
        c = fl.cache_dir(d, stopped_cache)
        found["stopped-cache-files"] = [os.path.join(c, f) for f in os.listdir(c)] if os.path.isdir(c) else []
    return found


def launch(a, d, tag, args, knobs, timeout=1500, watch=False):
    """One run of the package's exe under the case's sandbox; returns (rc, log, ms from the quit line to exit or None, wall s)."""
    # Every knob a case sets is named here, so each reaches the exe through WSLENV; an empty value reads as unset.
    base = {kk: v for kk, v in os.environ.items() if not kk.startswith("OPENPETE_")}
    env = child_env({"OPENPETE_SANDBOX": win(d), "OPENPETE_MSGBOX_SKIP": "1", "OPENPETE_VIBRATION": "0",
                     "TMP": win(os.path.join(d, "tmp")), "TEMP": win(os.path.join(d, "tmp")),
                     "OPENPETE_PRECOMPILE_AHEAD": knobs.get("ahead", ""),
                     "OPENPETE_LAUNCHER_QUIT_AT": knobs.get("quit_at", ""),
                     "OPENPETE_LAUNCHER_SELECT_GAME": knobs.get("select", ""),
                     "OPENPETE_LAUNCHER_DROP": knobs.get("drop", ""),
                     "OPENPETE_ASSET_JOB_DELAY_MS": knobs.get("delay", "")}, base)
    cmd = [os.path.join(a.app, "openpete.exe"), "--no-config", "--no-card", "--vibration", "0", "--input-lock",
           "--mute", "1"] + args
    path = os.path.join(d, tag + ".log")
    t0 = time.monotonic()
    with open(path, "wb") as f:
        p = subprocess.Popen(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, cwd=a.app)
    seen, off, tail = None, 0, b""
    while p.poll() is None:
        if watch and seen is None:
            with open(path, "rb") as f:
                f.seek(off)
                chunk = f.read()
            off += len(chunk)
            if b"quit during work (" in tail + chunk:
                seen = time.monotonic()
            tail = (tail + chunk)[-64:]
        if time.monotonic() - t0 > timeout:
            p.kill()
            break
        time.sleep(0.005)
    rc = p.wait()
    end = time.monotonic()
    with open(path, errors="replace") as f:
        log = f.read()
    return rc, log, ((end - seen) * 1000.0 if seen else None), end - t0


def prime(a):
    """Build the module once (headless) and return its path; the run's wall time is printed."""
    d = os.path.join(a.root, "prime")
    got = stamped(d)
    if got and whole_module(got[0]):
        return got[0]
    d = case_dir(a, "prime")
    seed_entry(a, d)
    rc, log, _, wall = launch(a, d, "prime", ["--headless", "--game", GAME, "--frame-limit", "1"], {})
    got = stamped(d)
    if not got or not whole_module(got[0]):
        raise RuntimeError(f"prime built no module (exit {rc}); see {d}/prime.log")
    print(f"[quit-win] prime: exit {rc}, module {got[0]} in {wall:.0f} s")
    return got[0]


def seed_built(a, d):
    src = prime(a)
    e = seed_entry(a, d)
    dst = os.path.join(e, "modules", os.path.basename(os.path.dirname(src)), MODULE)
    os.makedirs(os.path.dirname(dst))
    shutil.copy2(src, dst)


def quit_case(a, name, setup, quit_at, expect, extra=None, next_launch=None, args=None):
    """check_launcher_flow.quit_case on the package's exe."""
    d = case_dir(a, name)
    stopped_cache = setup(d)
    k = {"ahead": "1", "quit_at": quit_at}
    k.update(extra or {})
    rc, log, ms, _ = launch(a, d, "run", ["--launcher-window", "1280x800", "--frame-limit", "30"] + (args or []),
                            k, watch=True)
    bad = fl.ordered(log, r"scripted quit point reached", r"quit during work \(", expect,
                     r"quit during work: the work stopped and the launcher closed")
    m = fl.QUIT_CLOSED.search(log)
    own = float(m.group(1)) if m else None
    if rc != 0:
        bad.append(f"exit {rc}")
    if "game picked" in log.split("quit during work (", 1)[-1] or "window, device and imgui handed" in log:
        bad.append("the game started after the quit")
    inflight = fl.QUIT_INFLIGHT.search(log.split("quit during work (", 1)[-1])
    bound = fl.QUIT_BOUND_MS + (float(inflight.group(1)) if inflight else 0.0)
    if ms is None:
        bad.append("the quit line was not seen while the process ran")
    elif ms > bound:
        bad.append(f"the process ended {ms:.0f} ms after the quit, over {bound:.0f} ms")
    found = leftovers(d, stopped_cache)
    for kind, paths in found.items():
        if paths:
            bad.append(f"{len(paths)} {kind}: {paths[:3]}")
    counts = ", ".join(f"{kk} {len(v)}" for kk, v in found.items())
    detail = (f"exit {ms:.0f} ms after the quit (harness), {own if own is not None else '?'} ms (launcher), "
              f"bound {bound:.0f} ms ({inflight.group(1) + ' ms compile in flight' if inflight else 'none in flight'}); "
              if ms is not None else "") + f"leftovers: {counts}"
    if next_launch:
        detail += "; " + next_launch(a, d, bad)
    return fl.verdict(name, bad, detail)


def next_rebuild(a, d, bad):
    """The next launch rebuilds the stopped module in the background and plays."""
    rc2, log2, _, wall = launch(a, d, "next", ["--headless", "--game", GAME, "--frame-limit", "1"], {})
    mods = stamped(d)
    ok = rc2 == 0 and "launch-time module rebuild of spyro-1" in log2 and mods and whole_module(mods[0])
    if not ok:
        bad.append(f"next launch: exit {rc2}, rebuild {'ran' if 'launch-time module rebuild' in log2 else 'not run'}, "
                   f"module {'whole' if mods and whole_module(mods[0]) else 'absent or partial'}")
    marker = glob.glob(os.path.join(lib_dir(d), "*", "build-failed.toml"))
    if marker:
        bad.append(f"next launch left a failed-build marker {marker}")
    return f"next launch exit {rc2}, rebuilt {'yes' if ok else 'no'} in {wall:.0f} s, markers {len(marker)}"


def next_mods(a, d, bad):
    """The next launch compiles the stale mod again."""
    rc2, log2, _, wall = launch(a, d, "next", ["--headless", "--game", GAME, "--frame-limit", "1",
                                               "--mods-dir", win(os.path.join(d, "mods"))], {})
    compiled = re.search(rf"{fl.QUIT_MOD} \[{GAME}\]: compiling", log2) is not None
    ok = rc2 == 0 and compiled and "compile failed" not in log2
    if not ok:
        bad.append(f"next launch: exit {rc2}, mod {'compiled' if compiled else 'not compiled'}")
    return f"next launch exit {rc2}, mod recompiled {'yes' if ok else 'no'} in {wall:.0f} s"


PICK1 = {"select": GAME}


def run(a, name):
    unbuilt = lambda d: seed_entry(a, d) and None  # noqa: E731
    if name == "prime":
        print(f"[quit-win] prime: {prime(a)}")
        return 0
    if name == "quit-copy":
        def setup(d):
            os.makedirs(lib_dir(d))
        return quit_case(a, name, setup, "copy:quit", r"ingest copy of .* stopped by the quit",
                         {"drop": win(os.path.join(a.root, "disc", "SCUS-94228.cue"))})
    if name == "quit-translate":
        return quit_case(a, name, unbuilt, "translate+300:sigterm", fl.BUILD_EXPECT, PICK1)
    if name == "quit-build":
        return quit_case(a, name, unbuilt, "build+1500:quit", fl.BUILD_EXPECT, PICK1, next_launch=next_rebuild)
    if name == "quit-assets":
        def setup(d):
            seed_built(a, d)
            return GAME
        return quit_case(a, name, setup, "assets+300:quit", r"launcher assets of spyro-1 not updated \(stopped\)",
                         dict(PICK1, delay="60000"))
    if name in ("quit-east-twice", "quit-escape", "quit-close"):
        g = {"quit-east-twice": "east2", "quit-escape": "escape", "quit-close": "close"}[name]
        return quit_case(a, name, unbuilt, f"build+1000:{g}", fl.BUILD_EXPECT, PICK1)
    if name == "quit-mods":
        def setup(d):
            seed_built(a, d)
            shutil.copytree(os.path.join(fl.ROOT, "mods", fl.QUIT_MOD), os.path.join(d, "mods", fl.QUIT_MOD),
                            ignore=shutil.ignore_patterns(".build"))
        d_mods = lambda: ["--mods-dir", win(os.path.join(a.root, name, "mods"))]  # noqa: E731
        return quit_case(a, name, setup, "mods:quit", rf"{fl.QUIT_MOD} \[{GAME}\]: compile stopped; the mod stays stale",
                         PICK1, next_launch=next_mods, args=d_mods())
    raise SystemExit(f"unknown case {name}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True, help="the extracted package (WSL path on a Windows drive)")
    ap.add_argument("--root", required=True, help="case directories and the staged disc (short Windows path)")
    ap.add_argument("cases", nargs="+")
    a = ap.parse_args()
    a.app, a.root = os.path.abspath(a.app), os.path.abspath(a.root)
    rc = 0
    for c in a.cases:
        rc |= run(a, c)
    return rc


if __name__ == "__main__":
    sys.exit(main())
