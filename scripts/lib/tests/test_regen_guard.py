#!/usr/bin/env python3
"""Tests for scripts/lib/regen_guard.py, its wiring into scripts/regen-recomp.sh
and scripts/regen-manifest.sh, and recomp_stamp.py's message in a checkout
whose generated tree is a link.

Each test builds a scratch repository (a main checkout and linked worktrees
made with `git worktree add`) under a temporary directory. A worktree that
links holds games/g/generated and the manifest tomls as symbolic links into
the main checkout, as worktree-setup.sh makes them; a --no-links worktree
holds real directories. A build in flight is an exclusive flock on
build/linux-release/.openpete-build.lock held by this process; a fleet run is
a stand-in coordinator, a script named golden-fleet.py under the checkout's
scripts/, holding <fleet root>/runs/<id>/active.lock shared. The shell legs
copy the two regen scripts into the scratch repository and stop at their
first step past the guard (the absent recomp.conf or psx.map), so nothing
regenerates. No build or game runs:
sandbox-guard: no-launch, rumble-guard: no-launch, input-lock-guard: no-launch,
xvfb-guard: no-launch
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.dirname(HERE)
SCRIPTS = os.path.dirname(LIB)
sys.path.insert(0, LIB)
sys.path.insert(0, os.path.join(SCRIPTS, "fences"))
import flock_hold                                           # noqa: E402
import recomp_stamp                                         # noqa: E402
import regen_guard as rg                                    # noqa: E402

GAME = "spyro-1"
STAND_IN = """import fcntl, os, sys, time
fd = os.open(sys.argv[1], os.O_RDWR | os.O_CREAT, 0o644)
fcntl.flock(fd, fcntl.LOCK_SH)
open(sys.argv[2], "w").close()
while not os.path.exists(sys.argv[3]):
    time.sleep(0.02)
"""


def clean_env():
    """-> os.environ without the GIT_* overrides, with a commit identity."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t", GIT_COMMITTER_NAME="t",
               GIT_COMMITTER_EMAIL="t@t", GIT_CONFIG_NOSYSTEM="1")
    return env


class RegenGuardTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="regen-guard-test.")
        self.env = clean_env()
        self.main = os.path.join(self.tmp, "main")
        self.fleet = os.path.join(self.tmp, "fleet")
        os.makedirs(os.path.join(self.main, "scripts", "lib"))
        os.makedirs(os.path.join(self.main, "games", GAME, "manifest"))
        for rel in ("scripts/regen-recomp.sh", "scripts/regen-manifest.sh"):
            shutil.copy(os.path.join(os.path.dirname(SCRIPTS), rel), os.path.join(self.main, rel))
        for name in ("regen_guard.py", "flock_hold.py"):
            shutil.copy(os.path.join(LIB, name), os.path.join(self.main, "scripts", "lib", name))
        with open(os.path.join(self.main, "scripts", "golden-fleet.py"), "w") as f:
            f.write(STAND_IN)
        open(os.path.join(self.main, "games", GAME, "manifest", "psyq.toml"), "w").close()
        with open(os.path.join(self.main, ".gitignore"), "w") as f:
            f.write("games/*/generated\ngames/*/manifest/globals.toml\n"
                    "games/*/manifest/symbols.toml\nbuild/\nworktrees/\n")
        self.git(self.main, "init", "-q", "-b", "master")
        self.git(self.main, "add", ".")
        self.git(self.main, "commit", "-q", "-m", "init")
        gen = os.path.join(self.main, "games", GAME, "generated")
        os.makedirs(gen)
        for t in ("globals", "symbols"):
            open(os.path.join(self.main, "games", GAME, "manifest", t + ".toml"), "w").close()
        self.procs = []

    def tearDown(self):
        open(os.path.join(self.tmp, "stop"), "w").close()
        for p in self.procs:
            try:
                p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(self.tmp, ignore_errors=True)

    def git(self, root, *args):
        p = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True,
                           env=self.env)
        self.assertEqual(p.returncode, 0, p.stderr)
        return p.stdout

    def worktree(self, name, links):
        """A linked worktree; links: its outputs are links into the main checkout."""
        wt = os.path.join(self.main, "worktrees", name)
        self.git(self.main, "worktree", "add", "-q", "--detach", wt)
        rels = ["games/%s/generated" % GAME, "games/%s/manifest/globals.toml" % GAME,
                "games/%s/manifest/symbols.toml" % GAME]
        for rel in rels:
            if links:
                os.symlink(os.path.join(self.main, rel), os.path.join(wt, rel))
            elif rel.endswith("generated"):
                os.makedirs(os.path.join(wt, rel))
            else:
                open(os.path.join(wt, rel), "w").close()
        return wt

    def check(self, root, tool="regen-recomp", busy_ok=None):
        err = io.StringIO()
        with redirect_stderr(err):
            rc = rg.check(tool, GAME, root, self.fleet, busy_ok,
                          say=lambda s: print(s, file=sys.stderr))
        return rc, err.getvalue()

    def build_lock(self, wt):
        bdir = os.path.join(wt, "build", "linux-release")
        os.makedirs(bdir, exist_ok=True)
        h = flock_hold.hold(os.path.join(bdir, rg.BUILD_LOCK), flock_hold.EX)
        self.assertIsNotNone(h)
        self.addCleanup(h.release)
        return h

    def fleet_run(self, checkout, run="r1", script=None):
        """Start a stand-in coordinator of run from checkout; -> the process."""
        rdir = os.path.join(self.fleet, "runs", run)
        os.makedirs(rdir, exist_ok=True)
        ready = os.path.join(self.tmp, "ready-" + run)
        script = script or os.path.join(checkout, "scripts", "golden-fleet.py")
        p = subprocess.Popen([sys.executable, script, os.path.join(rdir, "active.lock"), ready,
                              os.path.join(self.tmp, "stop")], cwd=self.tmp)
        self.procs.append(p)
        end = time.monotonic() + 20
        while not os.path.exists(ready):
            self.assertLess(time.monotonic(), end, "stand-in coordinator never took the lock")
            time.sleep(0.02)
        return p

    # ---- linked ----------------------------------------------------------
    def test_linked_worktree_refuses_both_tools(self):
        wt = self.worktree("linked", True)
        for tool in ("regen-recomp", "regen-manifest"):
            rc, out = self.check(wt, tool)
            self.assertEqual(rc, rg.EXIT_LINKED, out)
            self.assertIn("worktree-setup.sh <name> --no-links", out)
            self.assertIn("-> " + os.path.realpath(self.main), out)

    def test_no_links_worktree_and_main_pass(self):
        wt = self.worktree("own", False)
        self.assertEqual(self.check(wt)[0], 0)
        self.assertEqual(self.check(wt, "regen-manifest")[0], 0)
        self.assertEqual(self.check(self.main)[0], 0)

    # ---- busy: a build ---------------------------------------------------
    def test_main_refuses_while_a_linking_worktree_builds(self):
        wt = self.worktree("linked", True)
        self.build_lock(wt)
        rc, out = self.check(self.main)
        self.assertEqual(rc, rg.EXIT_BUSY, out)
        self.assertIn("building " + os.path.join(wt, "build", "linux-release"), out)
        self.assertIn('--busy-ok "<reason>"', out)
        rc, out = self.check(self.main, busy_ok="test override")
        self.assertEqual(rc, 0, out)
        self.assertIn("--busy-ok (test override) overrides", out)

    def test_main_passes_while_a_no_links_worktree_builds(self):
        linked = self.worktree("linked", True)
        own = self.worktree("own", False)
        self.build_lock(own)
        rc, out = self.check(self.main)
        self.assertEqual(rc, 0, out)
        self.assertIn(linked, out)
        self.assertNotIn(own, out)

    # ---- busy: a fleet run -----------------------------------------------
    def test_main_refuses_while_a_fleet_run_from_a_linking_worktree_runs(self):
        wt = self.worktree("linked", True)
        self.fleet_run(wt)
        rc, out = self.check(self.main, "regen-manifest")
        self.assertEqual(rc, rg.EXIT_BUSY, out)
        self.assertIn("fleet run r1 from " + wt, out)

    def test_fleet_run_from_an_unrelated_checkout_passes(self):
        self.worktree("linked", True)
        own = self.worktree("own", False)
        self.fleet_run(own)
        rc, out = self.check(self.main)
        self.assertEqual(rc, 0, out)

    def test_fleet_run_whose_checkout_cannot_be_read_is_busy(self):
        stand_in = os.path.join(self.tmp, "coordinator.py")
        with open(stand_in, "w") as f:
            f.write(STAND_IN)
        self.fleet_run(self.main, script=stand_in)
        rc, out = self.check(self.main)
        self.assertEqual(rc, rg.EXIT_BUSY, out)
        self.assertIn("checkout not readable", out)

    # ---- the shell scripts -----------------------------------------------
    def sh(self, root, script, *args):
        """Run a regen script with the scratch fleet root as the local host's."""
        hosts = os.path.join(self.tmp, "fleet-hosts.toml")
        with open(hosts, "w") as f:
            f.write('[hosts.local]\ntransport = "local"\nfleet_root = "%s"\n' % self.fleet)
        return subprocess.run(["bash", os.path.join(root, "scripts", script)] + list(args),
                              capture_output=True, text=True,
                              env=dict(self.env, GAME=GAME, OPENPETE_FLEET_HOSTS=hosts))

    def test_fleet_root_is_the_local_host_s(self):
        hosts = os.path.join(self.tmp, "h.toml")
        with open(hosts, "w") as f:
            f.write('[hosts.far]\ntransport = "ssh"\nfleet_root = "/far"\n\n'
                    '[hosts.local]\ntransport = "local"\nfleet_root = "/near"\n')
        saved = os.environ.get("OPENPETE_FLEET_HOSTS")
        os.environ["OPENPETE_FLEET_HOSTS"] = hosts
        try:
            self.assertEqual(rg.local_fleet_root(), "/near")
        finally:
            if saved is None:
                os.environ.pop("OPENPETE_FLEET_HOSTS")
            else:
                os.environ["OPENPETE_FLEET_HOSTS"] = saved

    def test_script_on_main_refuses_while_a_fleet_run_from_a_linking_worktree_runs(self):
        wt = self.worktree("linked", True)
        self.fleet_run(wt)
        p = self.sh(self.main, "regen-manifest.sh")
        self.assertEqual(p.returncode, rg.EXIT_BUSY, p.stderr)
        self.assertIn("fleet run r1 from " + wt, p.stderr)

    def test_scripts_refuse_in_a_linked_worktree_before_writing(self):
        wt = self.worktree("linked", True)
        for script in ("regen-recomp.sh", "regen-manifest.sh"):
            p = self.sh(wt, script)
            self.assertEqual(p.returncode, rg.EXIT_LINKED, p.stderr)
            self.assertIn("REFUSED", p.stderr)
        self.assertEqual(os.listdir(os.path.join(self.main, "games", GAME, "generated")), [])

    def test_scripts_pass_the_guard_in_a_no_links_worktree(self):
        wt = self.worktree("own", False)
        p = self.sh(wt, "regen-recomp.sh")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("regen-recomp: no games/%s/recomp.conf" % GAME, p.stderr)
        p = self.sh(wt, "regen-manifest.sh")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("psx.map not found", p.stderr)
        for script in ("regen-recomp.sh", "regen-manifest.sh"):
            p = self.sh(wt, script, "--busy-ok", " ")
            self.assertEqual(p.returncode, 64, p.stderr)
            self.assertIn("--busy-ok takes a reason", p.stderr)

    def test_script_on_main_refuses_while_busy_and_takes_busy_ok(self):
        wt = self.worktree("linked", True)
        self.build_lock(wt)
        p = self.sh(self.main, "regen-recomp.sh")
        self.assertEqual(p.returncode, rg.EXIT_BUSY, p.stderr)
        p = self.sh(self.main, "regen-recomp.sh", "--busy-ok", "test override")
        self.assertEqual(p.returncode, 1, p.stderr)
        self.assertIn("no games/%s/recomp.conf" % GAME, p.stderr)


class RecompStampMessageTest(unittest.TestCase):
    """recomp_stamp.py check names the regen command for a real generated
    directory and says never to regenerate for a linked one."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="recomp-stamp-test.")
        for root in ("main", "wt"):
            src = os.path.join(self.tmp, root, recomp_stamp.SRC_REL)
            os.makedirs(src)
            with open(os.path.join(src, "a.cpp"), "w") as f:
                f.write("int x;\n")
        os.makedirs(os.path.join(self.tmp, "main", "games", GAME, "generated"))
        os.makedirs(os.path.join(self.tmp, "wt", "games", GAME))
        os.symlink(os.path.join(self.tmp, "main", "games", GAME, "generated"),
                   os.path.join(self.tmp, "wt", "games", GAME, "generated"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_check(self, root):
        err = io.StringIO()
        env = os.environ.pop("OPENPETE_RECOMP_STAMP_SKIP", None)
        try:
            with redirect_stderr(err):
                rc = recomp_stamp.main(["recomp_stamp.py", "check", GAME, "--root",
                                        os.path.join(self.tmp, root)])
        finally:
            if env is not None:
                os.environ["OPENPETE_RECOMP_STAMP_SKIP"] = env
        return rc, err.getvalue()

    def test_real_directory_names_the_regen_command(self):
        rc, out = self.run_check("main")
        self.assertEqual(rc, 67)
        self.assertIn("regenerate: for g in %s;" % GAME, out)
        self.assertNotIn("never regenerate", out)

    def test_linked_directory_says_never_regenerate_here(self):
        rc, out = self.run_check("wt")
        self.assertEqual(rc, 67)
        self.assertIn("never regenerate in this checkout", out)
        self.assertIn("--no-links", out)
        self.assertNotIn("regen-recomp.sh", out)


if __name__ == "__main__":
    unittest.main()
