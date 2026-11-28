#!/usr/bin/env python3
"""Refusals of scripts/regen-recomp.sh and scripts/regen-manifest.sh before
they write a game's generated tree or manifest.

A worktree made by worktree-setup.sh without --no-links holds
games/<g>/generated and games/<g>/manifest/{globals,symbols}.toml (and
third_party/psxrecomp/build) as symbolic links into the main checkout, so
every worktree compiles the main checkout's generated C. Two refusals
follow:

  linked   An output path of the regen in this checkout is a symbolic link,
           or resolves outside the checkout: the regen would write the main
           checkout's tree. Exit 65. No escape: regenerate in the main
           checkout, or in a worktree made with `worktree-setup.sh <name>
           --no-links`, whose outputs are real directories.
  busy     In a main checkout (git dir equal to the common dir), another
           worktree whose output path for the game resolves to this
           checkout's (it links the tree the regen rewrites) is building, or
           a golden-fleet run from it or from this checkout is in flight. A
           build holds <wt>/build/<preset>/.openpete-build.lock exclusive
           (build_lock.py); a fleet run's coordinator holds
           <fleet root>/runs/<id>/active.lock, and its checkout is the one
           holding the golden-fleet.py on its command line. A regen in that
           window changes the C under a running compile, or the source tree
           between a fleet run's pin and its sync recheck. A held run whose
           checkout cannot be read counts as busy. Exit 75. Escape:
           `--busy-ok "<reason>"`, printed with the refusal it overrides.

After a pass in a main checkout the guard lists each linking worktree: its
next build compiles the regenerated C.

Run:  python3 scripts/lib/regen_guard.py check --tool regen-recomp|regen-manifest
          --game <g> [--root <checkout>] [--fleet-root <dir>] [--busy-ok "<reason>"]
Exit: 0 pass, 64 usage, 65 linked, 75 busy.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flock_hold                                           # noqa: E402

EXIT_USAGE, EXIT_LINKED, EXIT_BUSY = 64, 65, 75
DEFAULT_FLEET_ROOT = "~/.cache/openpete-fleet"
BUILD_LOCK = ".openpete-build.lock"
FLEET_SCRIPT = "golden-fleet.py"


def outputs(tool, game):
    """-> (every path the regen writes, the game-specific ones a worktree
    links), relative to the checkout."""
    if tool == "regen-recomp":
        game_paths = ["games/%s/generated" % game]
        return game_paths + ["third_party/psxrecomp/build"], game_paths
    game_paths = ["games/%s/manifest/globals.toml" % game,
                  "games/%s/manifest/symbols.toml" % game]
    return game_paths, game_paths


def git(root, *args):
    """-> stdout of a git command in root, or "" when it fails or git is absent
    (a container build)."""
    try:
        p = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True)
    except OSError:
        return ""
    return p.stdout.strip() if p.returncode == 0 else ""


def is_main_checkout(root):
    """True when root is the repository's main checkout, not a linked worktree."""
    gd, cd = git(root, "rev-parse", "--git-dir"), git(root, "rev-parse", "--git-common-dir")
    if not gd or not cd:
        return False
    return os.path.realpath(os.path.join(root, gd)) == os.path.realpath(os.path.join(root, cd))


def linked_outputs(root, rels):
    """-> [(rel, the path it resolves to)] of outputs that are symbolic links
    or resolve outside root."""
    real_root = os.path.realpath(root)
    out = []
    for rel in rels:
        p = os.path.join(root, rel)
        real = os.path.realpath(p)
        if os.path.islink(p) or not (real == real_root or real.startswith(real_root + os.sep)):
            out.append((rel, real))
    return out


def worktrees(root):
    """-> the paths of the repository's other worktrees."""
    me = os.path.realpath(root)
    out = []
    for line in git(root, "worktree", "list", "--porcelain").splitlines():
        if line.startswith("worktree "):
            p = line[len("worktree "):]
            if os.path.realpath(p) != me and os.path.isdir(p):
                out.append(p)
    return out


def linking_worktrees(root, game_rels):
    """-> the other worktrees whose game output paths resolve to root's."""
    out = []
    for wt in worktrees(root):
        for rel in game_rels:
            mine = os.path.realpath(os.path.join(root, rel))
            theirs = os.path.join(wt, rel)
            if os.path.lexists(theirs) and os.path.realpath(theirs) == mine:
                out.append(wt)
                break
    return out


def cmdline(pid):
    """-> the argv of pid, [] when unreadable."""
    try:
        with open("/proc/%d/cmdline" % pid, "rb") as f:
            return [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
    except OSError:
        return []


def holder_pids(lock, mode=None):
    """-> the pids holding lock, each holder process replaced by the pid it serves."""
    return sorted({flock_hold.served_pid(p, lock) or p for p in flock_hold.lockers(lock, mode)})


def builds_in_flight(wt):
    """-> one line per build directory of wt a build holds exclusive."""
    out = []
    for lock in sorted(glob.glob(os.path.join(wt, "build", "*", BUILD_LOCK))):
        for pid in holder_pids(lock, "WRITE"):
            out.append("building %s: pid %d %s" % (os.path.dirname(lock), pid,
                                                   " ".join(cmdline(pid))[:120]))
    return out


def local_fleet_root():
    """-> the local host's fleet_root from the fleet host file
    ($OPENPETE_FLEET_HOSTS, else ~/.config/openpete/fleet-hosts.toml),
    else DEFAULT_FLEET_ROOT; `~` expanded."""
    cfg = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    path = os.environ.get("OPENPETE_FLEET_HOSTS") or os.path.join(cfg, "openpete", "fleet-hosts.toml")
    root = DEFAULT_FLEET_ROOT
    try:
        with open(path, encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return os.path.expanduser(root)
    for table in re.split(r"(?m)^\[", text):
        if re.search(r'(?m)^transport\s*=\s*"local"', table):
            m = re.search(r'(?m)^fleet_root\s*=\s*"([^"]*)"', table)
            if m:
                root = m.group(1)
            break
    return os.path.expanduser(root)


def run_checkouts(lock):
    """-> (the checkouts of a held run's coordinators, the holders' pids).
    A coordinator's checkout is the one holding the golden-fleet.py its
    command line runs, read against its working directory."""
    pids = holder_pids(lock)
    out = set()
    for pid in pids:
        argv = cmdline(pid)
        try:
            cwd = os.readlink("/proc/%d/cwd" % pid)
        except OSError:
            cwd = "/"
        for a in argv:
            if os.path.basename(a) == FLEET_SCRIPT:
                out.add(os.path.realpath(os.path.dirname(os.path.dirname(os.path.join(cwd, a)))))
    return out, pids


def fleet_runs_in_flight(fleet_root, checkouts):
    """-> one line per held fleet run whose coordinator runs from one of
    checkouts, or whose checkout cannot be read."""
    real = {os.path.realpath(c): c for c in checkouts}
    out = []
    for lock in sorted(glob.glob(os.path.join(fleet_root, "runs", "*", "active.lock"))):
        found, pids = run_checkouts(lock)
        if not pids:
            continue
        run = os.path.basename(os.path.dirname(lock))
        if not found:
            out.append("fleet run %s (pid %s): checkout not readable"
                       % (run, ",".join(str(p) for p in pids)))
        for c in sorted(found):
            if c in real:
                out.append("fleet run %s from %s" % (run, real[c]))
    return out


def check(tool, game, root, fleet_root=None, busy_ok=None, say=print):
    """Decide one regen. -> exit code; the refusal or report is printed."""
    rels, game_rels = outputs(tool, game)
    linked = linked_outputs(root, rels)
    if linked:
        say("%s: REFUSED: this checkout links the main checkout's outputs, and a regen here "
            "writes them:" % tool)
        for rel, real in linked:
            say("  %s -> %s" % (rel, real))
        say("  Never regenerate here, and test only commits whose recomp-stamp check passes. "
            "Regenerate in the main checkout, or in a worktree made with "
            "`worktree-setup.sh <name> --no-links`, whose outputs are its own.")
        return EXIT_LINKED
    if not is_main_checkout(root):
        return 0
    linking = linking_worktrees(root, game_rels)
    busy = []
    for wt in linking:
        busy += builds_in_flight(wt)
    busy += fleet_runs_in_flight(fleet_root or local_fleet_root(), [root] + linking)
    if busy and not busy_ok:
        say("%s: REFUSED: the %s tree this regen rewrites is in use:" % (tool, game))
        for line in busy:
            say("  " + line)
        say("  Wait for them to end, or pass --busy-ok \"<reason>\".")
        return EXIT_BUSY
    if busy:
        say("%s: --busy-ok (%s) overrides:" % (tool, busy_ok))
        for line in busy:
            say("  " + line)
    if linking:
        say("%s: worktrees linking %s's %s tree; their next build compiles the regenerated "
            "output:" % (tool, root, game))
        for wt in linking:
            say("  " + wt)
    return 0


def main(argv=None):
    """Parse the command line and run check. -> exit code."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("check")
    p.add_argument("--tool", required=True, choices=("regen-recomp", "regen-manifest"))
    p.add_argument("--game", required=True)
    p.add_argument("--root", default=os.getcwd())
    p.add_argument("--fleet-root", default=None)
    p.add_argument("--busy-ok", default=None)
    try:
        a = ap.parse_args(argv)
    except SystemExit as e:
        return EXIT_USAGE if e.code else 0
    if a.busy_ok is not None and not a.busy_ok.strip():
        print("%s: --busy-ok takes a reason" % a.tool, file=sys.stderr)
        return EXIT_USAGE
    return check(a.tool, a.game, a.root, a.fleet_root, a.busy_ok,
                 say=lambda s: print(s, file=sys.stderr))


if __name__ == "__main__":
    sys.exit(main())
