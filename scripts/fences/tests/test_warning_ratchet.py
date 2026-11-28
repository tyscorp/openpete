#!/usr/bin/env python3
"""Tests for the warning ratchet (warning_ratchet.py).

TextTest covers the command rewrite, the GCC text parser on captured
GCC 11 output (function context, an inlining chain into a libc header, a
macro expansion note), row attribution (outermost in-scope expansion, the
first in-scope inlined-from frame, the TU fallback), message
normalisation and the pragma escape. DecideTest drives the pure decision:
a rise an altered entry produces refuses, one only unaltered entries
produce is INHERITED, only rows the mode may lower shrink, an accepted rise
carries its reason. CheckVacuousTest holds `check`'s engagement to its
clean measured entries, not its decided rows. CompileTest compiles a scratch TU with the host GCC and
reads one front-end and one middle-end row from it; skipped without gcc.

OnDemandInputTest drives ratchet_lib's on-demand configure and build of
build-produced inputs against a stub cmake, the mod SDK directory
choice, the hook's windows-mingw SKIP reasons (no toolchain, no configured
build directory), a windows-mingw database missing a generated header
built and then decided, and a database older than a CMake file configured
again (refused, naming the command, when it may not configure).

Run: python3 scripts/fences/tests/test_warning_ratchet.py
"""
import contextlib
import io
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
sys.path.insert(0, FENCES)

import ratchet_lib as lib  # noqa: E402
import warning_ratchet as wr  # noqa: E402

ERRSET = {"-Werror=format", "-Wno-error=format-truncation",
          "-Werror=implicit-function-declaration"}

# GCC 11.4, -O2 -Wall -Wextra and the four diagnostic-format options, over
# h.h (a comparison macro and a static inline strcpy wrapper) and a.c.
GCC_TEXT = """In file included from core/a.c:1:
core/a.c: In function 'f':
core/h.h:2:23: warning: comparison of integer expressions of different signedness: 'int' and 'unsigned int' [-Wsign-compare]
core/a.c:2:47: note: in expansion of macro 'CMP'
core/a.c:2:32: warning: unused variable 'unused' [-Wunused-variable]
In file included from /usr/include/string.h:535,
                 from core/h.h:1,
                 from core/a.c:1:
In function 'strcpy',
    inlined from 'hcopy' at core/h.h:3:52,
    inlined from 'g' at core/a.c:3:29:
/usr/include/x86_64-linux-gnu/bits/string_fortified.h:79:10: warning: '__builtin___strcpy_chk' writing 12 bytes into a region of size 2 [-Wstringop-overflow=]
core/a.c: In function 'g':
core/a.c:3:21: note: destination object 'buf' of size 2
At top level:
core/a.c:9:13: warning: 'unused_fn.constprop.0' defined but not used [-Wunused-function]
core/a.c:12:5: error: implicit declaration of function 'nope' [-Werror=implicit-function-declaration]
"""


class TextTest(unittest.TestCase):
    def test_rewrite_strips_warning_options_but_keeps_the_error_set(self):
        argv = ["/usr/bin/ccache", "/usr/bin/cc", "-DX", "-w", "-Wno-format",
                "-Wformat", "-Werror=format", "-Wno-error=format-truncation",
                "-Wl,--as-needed", "-Wp,-D_FORTIFY_SOURCE=2", "-MD", "-MF", "x.d",
                "-O1", "-o", "x.o", "-c"]
        cc, flags = wr.rewrite(argv, ERRSET)
        self.assertEqual(cc, "/usr/bin/cc")
        self.assertEqual(flags[:6], ["-DX", "-Werror=format", "-Wno-error=format-truncation",
                                     "-Wl,--as-needed", "-Wp,-D_FORTIFY_SOURCE=2", "-O1"])
        self.assertEqual(flags[6:8], ["-Wall", "-Wextra"])
        self.assertIn("-fdiagnostics-color=never", flags)
        self.assertNotIn("-fdiagnostics-plain-output", flags)

    def test_cache_key_command_keeps_the_build_id_length_only(self):
        a = wr.key_command(["cc", '-DOPENPETE_BUILD_ID="v1-2-gabcdef"'])
        b = wr.key_command(["cc", '-DOPENPETE_BUILD_ID="v1-3-g123456"'])
        c = wr.key_command(["cc", '-DOPENPETE_BUILD_ID="v1-13-g123456"'])
        self.assertEqual(a, b)
        self.assertNotEqual(a, c)

    def test_parse_reads_context_inlining_and_macro_notes(self):
        ds = wr.parse(GCC_TEXT, "/r")
        self.assertEqual([d["opt"] for d in ds],
                         ["-Wsign-compare", "-Wunused-variable", "-Wstringop-overflow=",
                          "-Wunused-function", "-Wimplicit-function-declaration"])
        self.assertEqual(ds[0]["macros"], [("/r/core/a.c", 2, 47)])
        self.assertEqual(ds[0]["func"], "f")
        self.assertEqual(ds[2]["func"], "strcpy")
        self.assertEqual([f[0] for f in ds[2]["inl"]], ["hcopy", "g"])
        self.assertIsNone(ds[3]["func"])
        self.assertEqual(ds[4]["sev"], "error")

    def test_attribution(self):
        ds = wr.parse(GCC_TEXT, "/r")
        a = lambda d, k: wr.attribute(d, "core/a.c", "/r", k)
        # The macro use site in a.c, not the definition in h.h.
        self.assertEqual(a(ds[0], "fe"), (("core/a.c", "-Wsign-compare", "fe",
                                           "comparison of integer expressions of different "
                                           "signedness: 'int' and 'unsigned int'"),
                                          ("core/a.c", 2, 47)))
        # Outside the scope: the first in-scope inlined-from frame (h.h's hcopy).
        self.assertEqual(a(ds[2], "me"), (("core/h.h", "-Wstringop-overflow=", "me", "hcopy"),
                                          ("core/h.h", 3, 52)))
        # Clone suffixes leave the key; digits normalise in front-end details.
        key, _ = a(ds[3], "fe")
        self.assertEqual(key[3], "'unused_fn.constprop.N' defined but not used")
        self.assertEqual(a(ds[3], "me")[0][3], "-")
        self.assertEqual(wr.function_name("emit.part.0"), "emit")

    def test_out_of_scope_without_frame_goes_to_the_tu(self):
        d = wr.parse("/usr/include/x.h:4:1: warning: thing 7 [-Wfoo]\n", "/r")[0]
        key, loc = wr.attribute(d, "core/t.c", "/r", "fe")
        self.assertEqual(key, ("core/t.c", "-Wfoo", "fe", "thing N"))
        self.assertEqual(loc[0], "core/t.c")

    def test_pragmas(self):
        names = wr.error_set_names(ERRSET)
        lines = ['#pragma GCC diagnostic ignored "-Wformat"',
                 '#pragma GCC diagnostic ignored "-Wunused-variable"',
                 '// warning-ratchet: the table is read by the generator',
                 '#pragma GCC diagnostic ignored "-Wunused-variable"',
                 '#pragma GCC diagnostic push']
        out = wr.pragma_refusals(lines, "core/a.c", range(1, 6), names)
        self.assertEqual(len(out), 2)
        self.assertIn("error-set member", out[0])
        self.assertIn("core/a.c:2:", out[1])


class DecideTest(unittest.TestCase):
    K1 = ("core/a.h", "-Wx", "fe", "m")
    K2 = ("core/b.c", "-Wx", "fe", "m")

    def run_decide(self, before, after, contrib, altered, shrink=lambda k: False,
                   accept=None):
        return wr.decide(before, after, contrib, altered, lambda k: True, shrink,
                         accept or {}, lambda k: False)

    def test_rise_from_an_altered_entry_refuses_even_in_an_unstaged_header(self):
        v, _, bins = self.run_decide({self.K1: (1, "")}, {self.K1: 2},
                                     {self.K1: {"e1", "e2"}}, {"e2"})
        self.assertEqual(v, "REFUSED")
        self.assertEqual(bins["RISE"][0][1:], (1, 2))

    def test_rise_only_unaltered_entries_produce_is_inherited(self):
        v, rows, bins = self.run_decide({self.K1: (1, "")}, {self.K1: 2},
                                        {self.K1: {"e1"}}, {"e2"})
        self.assertEqual(v, "PASS")
        self.assertEqual(len(bins["INHERITED"]), 1)
        self.assertEqual(rows[self.K1], (1, ""))

    def test_only_permitted_rows_shrink(self):
        v, rows, bins = self.run_decide({self.K1: (3, ""), self.K2: (2, "")},
                                        {self.K1: 1}, {}, set(),
                                        shrink=lambda k: k[0] == "core/b.c")
        self.assertEqual(rows, {self.K1: (3, "")})
        self.assertEqual([b[0] for b in bins["RETIRED"]], [wr.key_text(self.K2)])
        self.assertEqual(len(bins["SHRINKABLE"]), 1)

    def test_accepted_rise_carries_its_reason(self):
        v, rows, bins = self.run_decide({}, {self.K2: 1}, {self.K2: {"e"}}, None,
                                        accept={wr.key_text(self.K2): "why"})
        self.assertEqual((v, rows[self.K2]), ("PASS", (1, "why")))


class CheckVacuousTest(unittest.TestCase):
    """check's engagement: every baseline row in an edited file decides
    no row, but the clean entries were measured and decided (LAW 8)."""

    def corpus(self, ids, measured):
        c = wr.Corpus()
        c.name = "linux-release"
        c.entries = [type("E", (), {"id": i})() for i in ids]
        c.results = {i: {"fe": [], "me": []} for i in measured}
        c.compiled, c.hits = len(measured), 0
        return c

    def test_rows_only_in_edited_files_is_not_vacuous(self):
        # moby.c dirty: its entry is excluded, the others decide clean
        c = self.corpus(["moby", "a", "b"], ["moby", "a", "b"])
        self.assertIsNone(wr.check_vacuous(c, {"moby"}))
        self.assertEqual(wr.clean_measured(c, {"moby"}), 2)

    def test_every_entry_dirty_is_vacuous(self):
        c = self.corpus(["moby", "a"], ["moby", "a"])
        self.assertIn("every one of the 2 measured", wr.check_vacuous(c, {"moby", "a"}))

    def test_nothing_measured_is_vacuous(self):
        c = self.corpus(["a"], [])
        self.assertIn("zero entries measured", wr.check_vacuous(c, set()))
        # the clean entries measured must hold results, not merely exist
        c = self.corpus(["a", "b"], ["a"])
        c.compiled = 1
        self.assertIsNone(wr.check_vacuous(c, set()))
        self.assertIn("VACUOUS", wr.check_vacuous(c, {"a"}))


class CompileTest(unittest.TestCase):
    def setUp(self):
        if not shutil.which("gcc"):
            self.skipTest("no gcc")
        self.tmp = tempfile.mkdtemp(prefix="warning-ratchet-test-")
        os.makedirs(os.path.join(self.tmp, "core"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_front_and_middle_end_rows(self):
        src = os.path.join(self.tmp, "core", "t.c")
        with open(src, "w") as fh:
            fh.write("int f(int c) { int unused; return c; }\n"
                     "static int g(void) { return 0; }\n")
        _, flags = wr.rewrite(["gcc", "-O2", "-c", src, "-o", "t.o"], set())
        flags = [t for t in flags if t != src]
        e = wr.Entry("t", "core/t.c", self.tmp, "gcc", flags, src)
        res = {"t": wr.measure(e)}
        after, contrib, _, errors, stale, n = wr.rows_of([e], res, self.tmp)
        # The unused local is a front-end row; the unused static, reported
        # "At top level" after code generation, a middle-end row with no
        # enclosing function.
        self.assertEqual(set(after), {
            ("core/t.c", "-Wunused-variable", "fe", "unused variable 'unused'"),
            ("core/t.c", "-Wunused-function", "me", "-")})
        self.assertFalse(errors or stale)


# A stand-in for cmake: appends its argv to $STUB_LOG, creates each path of
# $STUB_CREATE (os.pathsep-separated) on a --build or --preset run, prints
# $STUB_OUT and exits $STUB_RC.
STUB_CMAKE = """#!/usr/bin/env python3
import os, sys
with open(os.environ["STUB_LOG"], "a") as fh:
    fh.write(" ".join(sys.argv[1:]) + "\\n")
if os.environ.get("STUB_RC", "0") == "0":
    for p in filter(None, os.environ.get("STUB_CREATE", "").split(os.pathsep)):
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "w").close()
sys.stdout.write(os.environ.get("STUB_OUT", ""))
sys.exit(int(os.environ.get("STUB_RC", "0")))
"""


class OnDemandInputTest(unittest.TestCase):
    """ratchet_lib's on-demand configure and build of build-produced
    inputs, against a stub cmake on PATH: build, already present, build
    fails, still absent after the build, a no-build caller, and the SDK
    directory choice."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="warning-ratchet-ondemand-")
        bindir = os.path.join(self.tmp, "bin")
        os.makedirs(bindir)
        stub = os.path.join(bindir, "cmake")
        with open(stub, "w") as fh:
            fh.write(STUB_CMAKE)
        os.chmod(stub, 0o755)
        self.log = os.path.join(self.tmp, "cmake.log")
        self.build = os.path.join(self.tmp, "build", "linux-release")
        os.makedirs(self.build)
        self.db = os.path.join(self.build, "compile_commands.json")
        self.header = os.path.join(self.build, "gen", "x.gen.h")
        self.saved = dict(os.environ)
        for k in (lib.NO_BUILD_ENV, lib.BUILD_JOBS_ENV, "STUB_CREATE", "STUB_RC",
                  "STUB_OUT", "OPENPETE_FENCE_BUILD_SDK", "OPENPETE_WARNING_DB",
                  wr.MINGW_DB_ENV):
            os.environ.pop(k, None)
        self.bindir = bindir
        os.environ["PATH"] = bindir + os.pathsep + os.environ["PATH"]
        os.environ["STUB_LOG"] = self.log
        self.calls = 0

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def probe(self):
        self.calls += 1
        return ([self.header] if not os.path.exists(self.header) else []), ["extra_t"]

    def cmake_runs(self):
        try:
            with open(self.log) as fh:
                return fh.read().splitlines()
        except OSError:
            return []

    def ensure(self, build=True):
        return lib.ensure_inputs(wr.FENCE, "linux-release", self.db, self.probe, build)

    def test_absent_inputs_are_built_once_then_reprobed(self):
        os.environ["STUB_CREATE"] = self.header
        os.environ[lib.BUILD_JOBS_ENV] = "8"
        (missing, _), note = self.ensure()
        self.assertEqual(missing, [])
        self.assertEqual(self.calls, 2)
        self.assertEqual(self.cmake_runs(), [
            "--build %s --target openpete_generated_headers extra_t -j 8" % self.build])
        self.assertRegex(note, r"^built openpete_generated_headers extra_t in \d+\.\d s$")

    def test_present_inputs_build_nothing(self):
        os.makedirs(os.path.dirname(self.header))
        open(self.header, "w").close()
        (missing, _), note = self.ensure()
        self.assertEqual((missing, note, self.calls, self.cmake_runs()), ([], "", 1, []))

    def test_failed_build_refuses_with_its_output_tail(self):
        os.environ.update(STUB_RC="2", STUB_OUT="ninja: error: boom\n")
        with self.assertRaises(lib.Refusal) as cm:
            self.ensure()
        self.assertIn("failed (rc 2", str(cm.exception))
        self.assertIn("ninja: error: boom", str(cm.exception))
        self.assertEqual(self.calls, 1)

    def test_inputs_still_absent_after_the_build_refuse(self):
        with self.assertRaises(lib.Refusal) as cm:
            self.ensure()
        self.assertIn("MISSING-INPUT: 1 build-produced input(s)", str(cm.exception))
        self.assertIn("still absent after building", str(cm.exception))
        self.assertEqual((self.calls, len(self.cmake_runs())), (2, 1))

    def test_no_build_caller_refuses_without_running_cmake(self):
        for build, env, why in ((False, None, "a build-time check does not build"),
                                (True, "1", lib.NO_BUILD_ENV + " set")):
            if env:
                os.environ[lib.NO_BUILD_ENV] = env
            with self.assertRaises(lib.Refusal) as cm:
                self.ensure(build)
            msg = str(cm.exception)
            self.assertIn("MISSING-INPUT", msg)
            self.assertIn("cmake --build %s --target openpete_generated_headers extra_t"
                          % self.build, msg)
            self.assertIn(why, msg)
        self.assertEqual(self.cmake_runs(), [])

    def test_missing_database_is_configured_unless_the_caller_does_not_build(self):
        os.environ[lib.NO_BUILD_ENV] = "1"
        with self.assertRaises(lib.Refusal) as cm:
            lib.configure_db(self.tmp, wr.FENCE, "linux-release", self.db)
        self.assertIn("VACUOUS: no linux-release compile database", str(cm.exception))
        with self.assertRaises(lib.Refusal):
            os.environ.pop(lib.NO_BUILD_ENV)
            lib.configure_db(self.tmp, wr.FENCE, "linux-release", self.db, build=False)
        self.assertEqual(self.cmake_runs(), [])
        os.environ["STUB_CREATE"] = self.db
        self.assertTrue(lib.configure_db(self.tmp, wr.FENCE, "linux-release", self.db))
        self.assertFalse(lib.configure_db(self.tmp, wr.FENCE, "linux-release", self.db))
        self.assertEqual(self.cmake_runs(), ["--preset linux-release"])

    def stale_database(self, preset):
        """A database of preset older than a CMake file of the scratch root.
        -> (database path, the CMake file's relative path)."""
        bdir = os.path.join(self.tmp, "build", preset)
        os.makedirs(bdir, exist_ok=True)
        db = os.path.join(bdir, "compile_commands.json")
        open(db, "w").close()
        os.utime(db, (1, 1))
        open(os.path.join(self.tmp, "CMakeLists.txt"), "w").close()
        return db, "CMakeLists.txt"

    def test_stale_database_is_configured_again_and_decided(self):
        db, cm = self.stale_database("windows-mingw")
        os.environ["STUB_CREATE"] = db
        self.assertEqual(wr.stale_cmake(self.tmp, db, [cm]), [cm])
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            lib.reconfigure_db(self.tmp, wr.FENCE, "windows-mingw", db, [cm])
        self.assertEqual(self.cmake_runs(), ["--preset windows-mingw"])
        self.assertEqual(wr.stale_cmake(self.tmp, db, [cm]), [])
        self.assertIn("configuring: cmake --preset windows-mingw", err.getvalue())

    def test_stale_database_refuses_with_the_command_when_it_cannot_configure(self):
        db, cm = self.stale_database("windows-mingw")
        os.environ[lib.NO_BUILD_ENV] = "1"
        with self.assertRaises(lib.Refusal) as cmx:
            lib.reconfigure_db(self.tmp, wr.FENCE, "windows-mingw", db, [cm])
        self.assertIn("STALE-DB", str(cmx.exception))
        self.assertIn("Run: cmake --preset windows-mingw", str(cmx.exception))
        os.environ.pop(lib.NO_BUILD_ENV)
        other = os.path.join(self.tmp, "elsewhere.json")
        open(other, "w").close()
        with self.assertRaises(lib.Refusal) as cmx:
            lib.reconfigure_db(self.tmp, wr.FENCE, "windows-mingw", other, [cm])
        self.assertIn("is not the preset's", str(cmx.exception))
        os.environ["STUB_RC"] = "1"
        with self.assertRaises(lib.Refusal) as cmx:
            lib.reconfigure_db(self.tmp, wr.FENCE, "windows-mingw", db, [cm])
        self.assertIn("configuring windows-mingw failed", str(cmx.exception))
        self.assertEqual(self.cmake_runs(), ["--preset windows-mingw"])

    def mingw_toolchain(self, cc, present):
        """A toolchain file under the scratch root naming cc; present puts an
        executable cc on PATH."""
        tc = os.path.join(self.tmp, wr.MINGW_TOOLCHAIN)
        os.makedirs(os.path.dirname(tc), exist_ok=True)
        with open(tc, "w") as fh:
            fh.write("set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_C_COMPILER       %s)\n" % cc)
        if present:
            exe = os.path.join(self.bindir, cc)
            with open(exe, "w") as fh:
                fh.write("#!/bin/sh\nexit 0\n")
            os.chmod(exe, 0o755)

    def test_mingw_skips_only_without_a_toolchain_or_a_configured_directory(self):
        self.mingw_toolchain("openpete-test-absent-mingw-gcc", False)
        path, why = wr.mingw_database(self.tmp)
        self.assertIsNone(path)
        self.assertIn("no mingw toolchain (openpete-test-absent-mingw-gcc", why)
        self.mingw_toolchain("openpete-test-mingw-gcc", True)
        path, why = wr.mingw_database(self.tmp)
        self.assertIsNone(path)
        self.assertIn("no configured windows-mingw build directory", why)
        self.assertEqual(self.cmake_runs(), [])
        mdir = os.path.join(self.tmp, "build", "windows-mingw")
        os.makedirs(mdir)
        open(os.path.join(mdir, "CMakeCache.txt"), "w").close()
        db = os.path.join(mdir, "compile_commands.json")
        os.environ["STUB_CREATE"] = db
        self.assertEqual(wr.mingw_database(self.tmp), (db, None))
        self.assertEqual(self.cmake_runs(), [
            "--preset windows-mingw",
            "--build %s --target openpete_generated_headers" % mdir])

    def test_missing_mingw_generated_header_is_built_then_decided(self):
        mdir = os.path.join(self.tmp, "build", "windows-mingw")
        db = os.path.join(mdir, "compile_commands.json")
        header = os.path.join(mdir, "gen", "spyro1_symbols.gen.h")
        os.environ["STUB_CREATE"] = header
        loads = []

        def load_corpus(root, dbname, db_path, mods=True, jobs=16):
            c = wr.Corpus()
            c.name, c.db_path, c.entries = dbname, db_path, []
            c.missing = [] if os.path.exists(header) else [header]
            c.missing_sdk, c.sdk_targets = [], []
            loads.append(list(c.missing))
            return c

        class Decided(Exception):
            pass

        def stale_cmake(root, db_path, paths):
            raise Decided(db_path)
        saved = wr.load_corpus, wr.stale_cmake
        wr.load_corpus, wr.stale_cmake = load_corpus, stale_cmake
        err = io.StringIO()
        try:
            with contextlib.redirect_stderr(err), self.assertRaises(Decided) as cm:
                wr.hook_database(self.tmp, "windows-mingw", db, [], [], False, set(), {}, 2)
        finally:
            wr.load_corpus, wr.stale_cmake = saved
        self.assertEqual(str(cm.exception), db)
        self.assertEqual(loads, [[header], []])
        self.assertEqual(self.cmake_runs(), [
            "--build %s --target openpete_generated_headers" % mdir])
        self.assertNotIn("SKIP", err.getvalue())

    def test_sdk_directory_choice(self):
        games = {"spyro-1", "spyro-2"}
        test_sdk = os.path.join(self.build, "mod-sdk-test", "sdk")
        self.assertEqual(wr.sdk_root(self.build, games), (test_sdk, [
            "openpete_mod_sdk_test_root", "spyro1_mod_sdk_test_stage",
            "spyro2_mod_sdk_test_stage"]))
        os.makedirs(os.path.join(self.build, "sdk", "games", "spyro-1"))
        self.assertEqual(wr.sdk_root(self.build, games)[0], test_sdk)
        os.environ["OPENPETE_FENCE_BUILD_SDK"] = "1"
        staged = (os.path.join(self.build, "sdk"), ["openpete_sdk_stage"])
        self.assertEqual(wr.sdk_root(self.build, games), staged)
        os.environ.pop("OPENPETE_FENCE_BUILD_SDK")
        os.makedirs(os.path.join(self.build, "sdk", "games", "spyro-2"))
        self.assertEqual(wr.sdk_root(self.build, games), staged)


if __name__ == "__main__":
    unittest.main()
