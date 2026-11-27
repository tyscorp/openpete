#!/usr/bin/env python3
"""Tests for the thread-role fence (thread_roles.py).

TextTest covers the text-level pieces: comment stripping, the roles
header's tables, the sealed-declaration scan, the writer-body scan and the
const rewrite. AnalyseTest runs the per-TU analysis (libclang 14 walk and
clang-19 over the rewritten copy) on fixture TUs in a scratch tree that
holds a copy of the roles header: each negative control yields its one
refusal, each positive control none. Skipped when clang-19 or libclang is
missing.

Run: python3 scripts/fences/tests/test_thread_roles.py
"""
import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
sys.path.insert(0, FENCES)

import thread_roles as tr  # noqa: E402
import thread_roles_globals as pop  # noqa: E402


class TextTest(unittest.TestCase):
    def test_strip_comments_keeps_lines_and_strings(self):
        s = 'a /* x(y)\n z */ b // c(d)\n"//s" e('
        out = tr.strip_comments(s)
        self.assertEqual(out.count("\n"), s.count("\n"))
        self.assertNotIn("x(y)", out)
        self.assertNotIn("c(d)", out)
        self.assertIn('"//s" e(', out)

    def test_tables_of_the_tree(self):
        t = tr.Tables(ROOT, [tr.ROLES_H])
        self.assertIn("sim", t.roles)
        self.assertIn("c:run.c@F@openpete_sim_thread", t.rows)
        form, roles = t.rows["c:run.c@F@openpete_sim_thread"]
        self.assertTrue(form.startswith("prop:"))
        self.assertEqual(roles, {"sim"})
        self.assertIn("c:@F@openpete_game_runtime_register", t.seal_writers["game_layout"])

    def test_writer_spans_skip_calls_and_conditions(self):
        text = ("static void w(void) {\n  x = 1;\n}\n"
                "void f(void) { if (w()) { y(); } w(); }\n"
                "int g = w();\n")
        self.assertEqual(tr.writer_spans(text, ["w"]), [(1, 3)])

    def test_rewrite_sealed(self):
        text = ("static int s_a OP_SEALED(t) = 0;\n"
                "extern T g_b OP_SEALED(t);\n"
                "void w(void) {\n  s_a = 1; op_seal_check(s_a); o.s_a = 2;\n}\n")
        out = tr.rewrite_sealed(text, ["s_a", "g_b"], [(3, 5)], "c")
        lines = out.split("\n")
        self.assertEqual(lines[0], "static int const s_a OP_SEALED(t) = 0;")
        self.assertEqual(lines[1], "extern T const g_b OP_SEALED(t);")
        self.assertIn("(*(__typeof_unqual__(s_a)*)(__UINTPTR_TYPE__)&(s_a)) = 1;", lines[3])
        self.assertIn("op_seal_check(s_a)", lines[3])
        self.assertIn("o.s_a = 2", lines[3])
        self.assertEqual(len(lines), len(text.split("\n")))


HEADER = r'''
#include "openpete_thread_roles.h"
#include <string.h>
typedef struct lay { int a; int* p; } lay_t;
OP_SEAL_DEFINE(lay);
extern lay_t g_lay OP_SEALED(lay);
lay_t g_lay OP_SEALED(lay);
static int s_g OP_GUARDED_BY(sim);
static void need(void) OP_REQUIRES(sim) { s_g = 1; }
static void call_it(void (*fn)(void) OP_SYNC_FN) { fn(); }
static void (*s_cb)(void) OP_SLOT(sim);
void lay_register(void) { op_seal_check(lay); g_lay.a = 1; memset(&g_lay, 0, sizeof g_lay); }
void lay_read(void) { const int* q = &g_lay.a; g_lay.p[0] = *q; }
void ok_sync(void) OP_REQUIRES(sim);
void ok_sync(void) OP_REQUIRES(sim) { call_it(need); s_cb(); }
void ok_store(void) { s_cb = need; }
void ok_test(void) { if (need) { } }
'''

CONTROLS = {
    "guarded_write": ("void bad(void) { s_g = 2; }", "writing variable 's_g'"),
    "sync_without_role": ("void bad(void) { call_it(need); }", "calling function 'need'"),
    "slot_call_without_role": ("void bad(void) { s_cb(); }", "calling function 's_cb'"),
    "other_reference": ("void bad(void) { void (*p)(void) = need; (void)p; }",
                        "is neither synchronous"),
    "sealed_write": ("void bad(void) { g_lay.a = 3; }", "write site of a sealed global"),
    "sealed_address": ("void bad(void) { memset(&g_lay, 0, 1); }", "write site of a sealed global"),
    "bind_outside_row": ("void bad(void) { OP_ROLE_BIND(sim); }", "does not list"),
    "boot_outside": ("void bad(void) { op_role_boot_reacquire(); }", "boot function"),
    "co_switch_outside": ("void co_switch(void*); void bad(void) { co_switch(0); }",
                          "co_switch outside"),
    "direct_assert_call": ("void bad(void) { op_role__assert_sim(0); }", "direct call"),
    "reference_before_assert": ("void bad(void) { need(); OP_ROLE_ASSERT(sim); need(); }",
                                "calling function 'need'"),
    "exit_outside": ("void exit(int); void bad(void) { exit(1); }", "exit() outside"),
    "thread_entry_without_row": (
        "typedef int (*tf)(void*); void* SDL_CreateThreadRuntime(tf, const char*, void*, void*, void*);\n"
        "static int ent(void* p) { return 0; }\n"
        "void bad(void) { SDL_CreateThreadRuntime(ent, \"t\", 0, 0, 0); }",
        "starts a thread or fiber and has no binding-table row"),
    "slot_read_escapes": ("void bad(void) { void (*p)(void) = s_cb; (void)p; }",
                          "read of the OP_SLOT(sim) slot"),
}


class DatabaseTest(unittest.TestCase):
    def test_entry_with_an_absent_source_is_skipped(self):
        tmp = tempfile.mkdtemp(prefix="thread-roles-test-")
        try:
            os.makedirs(os.path.join(tmp, "core"))
            with open(os.path.join(tmp, "core", "here.c"), "w") as fh:
                fh.write("int here;\n")
            db = os.path.join(tmp, "compile_commands.json")
            with open(db, "w") as fh:
                json.dump([{"directory": tmp, "file": f, "arguments": ["cc", "-c", f]}
                           for f in ("core/here.c", "core/gone.c")], fh)
            out = io.StringIO()
            with contextlib.redirect_stderr(out):
                ent = tr.load_entries(tmp, db, True)
            self.assertEqual(sorted(ent), ["core/here.c"])
            self.assertIn("STALE-DB (skipped): 1 windows-mingw entry names a source "
                          "absent from the tree: core/gone.c. Re-run: cmake --preset "
                          "windows-mingw", out.getvalue())
            # The selection a whole-tree hook analyses holds no absent source,
            # so no worker opens one.
            sel = tr.select_all({}, ent)
            self.assertNotIn("core/gone.c", sel["mingw"])
        finally:
            shutil.rmtree(tmp, ignore_errors=True)


@unittest.skipUnless(shutil.which("clang-19") and os.path.isfile(tr.lib.LIBCLANG),
                     "clang-19 or libclang 14 missing")
class AnalyseTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="thread-roles-test-")
        inc = os.path.join(self.tmp, "core", "include")
        os.makedirs(inc)
        hdr = tr.read_text(os.path.join(ROOT, tr.ROLES_H))
        hdr = hdr.replace('SEAL(game_layout, "c:@F@openpete_game_runtime_register")',
                          'SEAL(lay, "c:@F@lay_register")')
        with open(os.path.join(inc, "openpete_thread_roles.h"), "w") as fh:
            fh.write(hdr)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def analyse(self, body):
        rel = "core/fixture.c"
        with open(os.path.join(self.tmp, rel), "w") as fh:
            fh.write(HEADER + body + "\n")
        files = [tr.ROLES_H, rel]
        tables = tr.Tables(self.tmp, files)
        ctx = tr.build_ctx(self.tmp, tables, self.tmp)
        ctx["writer_files"] = {rel}
        tr.compile_patterns(ctx)
        flags = ["-I" + os.path.join(self.tmp, "core", "include"), "-std=gnu11"]
        return tr.analyse((ctx, rel, "linux", self.tmp, flags))

    def test_positive_controls_pass(self):
        r = self.analyse("")
        self.assertEqual(r["parse"], [])
        self.assertEqual(r["refusals"], [])
        self.assertTrue(r["walked"])
        self.assertGreaterEqual(r["kinds"].get("sync", 0), 1)
        self.assertGreaterEqual(r["kinds"].get("slot", 0), 1)
        self.assertGreaterEqual(r["kinds"].get("slot-call", 0), 1)
        self.assertGreaterEqual(r["kinds"].get("test", 0), 1)

    def test_negative_controls_refuse_once(self):
        for name, (body, want) in sorted(CONTROLS.items()):
            with self.subTest(control=name):
                r = self.analyse(body)
                self.assertEqual(r["parse"], [], name)
                hits = [x for x in r["refusals"] if want in x]
                self.assertEqual(len(hits), 1, "%s: %s" % (name, r["refusals"]))

    def test_unannotated_redeclaration_is_refused(self):
        r = self.analyse("void ok_sync(void);")
        self.assertEqual(r["refusals"], [])
        tables = tr.Tables(self.tmp, [tr.ROLES_H, "core/fixture.c"])
        rc, _ = tr.report(self.tmp, tables, {}, [r], 0.0, set(), "test")
        self.assertEqual(rc, 1)

    def analyse_cxx(self, body):
        """A C++ TU that includes an annotated header inside `extern "C" {`
        and names a slot, so the walk runs over it."""
        with open(os.path.join(self.tmp, "core", "include", "fixture_api.h"), "w") as fh:
            fh.write('#include "openpete_thread_roles.h"\n'
                     "void api_fn(void) OP_REQUIRES(sim);\n"
                     "static inline void api_inline(void) { void (*p)(void) = api_fn; (void)p; }\n")
        rel = "core/fixture.cpp"
        with open(os.path.join(self.tmp, rel), "w") as fh:
            fh.write('#include "openpete_thread_roles.h"\n'
                     'extern "C" {\n#include "fixture_api.h"\n}\n'
                     "static void (*s_cb)(void) OP_SLOT(sim);\n" + body + "\n")
        files = [tr.ROLES_H, "core/include/fixture_api.h", rel]
        ctx = tr.build_ctx(self.tmp, tr.Tables(self.tmp, files), self.tmp)
        tr.compile_patterns(ctx)
        flags = ["-I" + os.path.join(self.tmp, "core", "include"), "-std=gnu++17"]
        return tr.analyse((ctx, rel, "linux", self.tmp, flags))

    def test_included_declarations_are_not_local(self):
        # The header's prototype and inline body arrive through the
        # #include inside extern "C"; they are the header's, not this TU's.
        r = self.analyse_cxx("")
        self.assertEqual(r["parse"], [])
        self.assertTrue(r["walked"])
        self.assertEqual(r["refusals"], [])

    def test_local_prototype_in_cxx_is_refused(self):
        for name, body in (("extern_c_block", 'extern "C" { void api_fn(void) OP_REQUIRES(sim); }'),
                           ("block_scope", 'extern "C" void bad(void) '
                                           "{ void api_fn(void) OP_REQUIRES(sim); }")):
            with self.subTest(control=name):
                r = self.analyse_cxx(body)
                self.assertEqual(r["parse"], [], name)
                hits = [x for x in r["refusals"] if "local prototype" in x]
                self.assertEqual(len(hits), 1, "%s: %s" % (name, r["refusals"]))
                self.assertIn("core/fixture.cpp:", hits[0])

    def test_definition_in_an_extern_c_block_is_not_a_prototype(self):
        r = self.analyse_cxx('extern "C" { void api_fn(void) OP_REQUIRES(sim) { } }')
        self.assertEqual(r["parse"], [])
        self.assertEqual([x for x in r["refusals"] if "local prototype" in x], [])

    def analyse_cxx_seal(self, body):
        """A C++ TU holding sealed globals whose seal lists C++-linkage
        writers: their USRs carry a signature suffix (`#`, `#*I#`, `#1`)."""
        path = os.path.join(self.tmp, "core", "include", "openpete_thread_roles.h")
        hdr = tr.read_text(path).replace(
            'SEAL(lay, "c:@F@lay_register")',
            'SEAL(lay, "c:@F@lay_register#") SEAL(lay, "c:fixture.cpp@aN@F@lay_fill#*I#") '
            'SEAL(lay, "c:@S@Lay@F@load#1")')
        with open(path, "w") as fh:
            fh.write(hdr)
        rel = "core/fixture.cpp"
        with open(os.path.join(self.tmp, rel), "w") as fh:
            fh.write('#include "openpete_thread_roles.h"\n'
                     "static int s_v OP_SEALED(lay);\n"
                     "int s_arr[4] OP_SEALED(lay);\nint s_n OP_SEALED(lay) = 2;\n"
                     "struct Lay { void load() const; };\n"
                     "void lay_register() { s_v = 1; s_arr[0] = 1; }\n"
                     "namespace { void lay_fill(int* p) { s_n = *p; } }\n"
                     "void Lay::load() const { s_v = 2; }\n"
                     "int lay_get() { return s_v + s_arr[1] + s_n; }\n" + body + "\n")
        files = [tr.ROLES_H, rel]
        ctx = tr.build_ctx(self.tmp, tr.Tables(self.tmp, files), self.tmp)
        ctx["writer_files"] = {rel}
        tr.compile_patterns(ctx)
        flags = ["-I" + os.path.join(self.tmp, "core", "include"), "-std=gnu++17"]
        return tr.analyse((ctx, rel, "linux", self.tmp, flags))

    def test_cxx_seal_writers_are_recognised(self):
        self.assertEqual(tr.usr_name("c:@N@ns@F@f#*$@N@ns@S@T#"), "f")
        self.assertEqual(tr.usr_name("c:@S@Lay@F@load#1"), "load")
        self.assertEqual(tr.usr_name("c:run.c@F@openpete_sim_thread"), "openpete_sim_thread")
        r = self.analyse_cxx_seal("")
        self.assertEqual(r["parse"], [])
        self.assertEqual(r["refusals"], [])
        self.assertTrue(r["rewritten"])
        r = self.analyse_cxx_seal("void bad() { s_v = 3; }")
        self.assertEqual(r["parse"], [])
        hits = [x for x in r["refusals"] if "write site of a sealed global" in x]
        self.assertEqual(len(hits), 1, r["refusals"])

    def test_slot_store_recorded(self):
        r = self.analyse("")
        self.assertIn(("c:fixture.c@F@need", "need", "sim"), r["slot_stores"])


POPULATION = r'''
#include "openpete_thread_roles.h"
#include <stdatomic.h>
static const int k_c1[3] = {1, 2, 3};
static const char* const k_c2[] = {"x"};
static const int k_c3[2][2] = {{1}};
static const char* s_names[] = {"x"};
static int s_plain;
static int s_guarded OP_GUARDED_BY(sim);
static _Atomic int s_atomic;
_Thread_local int t_local;
static op_lock_cap_t s_cap;
#define PASTE2(a, b) a##b
#define PASTE(a, b) PASTE2(a, b)
#define SITE() static int PASTE(site_, __LINE__) = -1; (void)PASTE(site_, __LINE__)
static int s_init_target;
static int* s_init_ref = &s_init_target;
static int s_store_only;
static int s_pair_a, s_pair_b;
static int s_rmw;
static int s_addr;
void reader(int* out) { *out = s_store_only + s_pair_a + s_pair_b + s_addr; }
void writer(void) { s_store_only = 1; }
void pair(void) { s_pair_a = 1; s_pair_b = 2; }
void rmw(void) { s_rmw += 1; }
void addr(void) { reader(&s_addr); }
int once(void) { static int v = -1; if (v < 0) v = 3; return v; }
void macro_a(void) { SITE(); }
void macro_b(void) { SITE(); }
void no_globals(int* p) { *p = 1; }
void size_only(void) { (void)sizeof(s_plain); }
void names_plain(void) { s_plain = 1; (void)s_guarded; (void)s_atomic; (void)t_local;
                         (void)k_c1; (void)s_names; }
'''

POPULATION_CPP = r'''
static int s_ctor; static int s_dtor; static int s_tpl; static int s_inline_m;
struct Obj {
    Obj() { s_ctor = 1; }
    ~Obj() { s_dtor = 1; }
    void m() { s_inline_m = 1; }
};
template <typename T> T tpl(T v) { s_tpl = 1; return v; }
'''


@unittest.skipUnless(os.path.isfile(tr.lib.LIBCLANG), "libclang 14 missing")
class PopulationTest(unittest.TestCase):
    """Check 2's scan, census and ruling on fixture TUs, line numbers taken
    from the text, against a scratch tree holding the roles header."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="thread-roles-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        inc = os.path.join(self.tmp, "core", "include")
        os.makedirs(inc)
        shutil.copy(os.path.join(ROOT, tr.ROLES_H), inc)
        self.ctx = {"root": self.tmp, "rows": {}, "escapes": {}, "accessors": {}, "sealed": {},
                    "slot_names": set()}
        self.read = lambda r: tr.read_text(os.path.join(self.tmp, r))

    def parse(self, rel, text):
        path = os.path.join(self.tmp, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)
        flags = ["-Icore/include", "-I" + os.path.join(self.tmp, "core", "include")]
        if rel.endswith(".cpp"):
            flags.append("-std=c++17")
        return tr.Walk(self.ctx, rel, self.tmp, flags)

    def scan(self, rel, text, names):
        """Scan with the fixture's functions `names` touched."""
        w = self.parse(rel, text)
        self.assertEqual(w.parse_errors, [])
        lines = text.split("\n")
        hunks = [(i + 1, 1) for i, s in enumerate(lines) if any(n in s for n in names)]
        plan = {"hunks": {rel: hunks}, "stateless": True, "stateless_headers": True,
                "macros": set()}
        return pop.scan_tu(tr.ci(), w.tu, plan, self.ctx, tr.FENCE, self.read)

    def census(self, rel, text):
        w = self.parse(rel, text)
        m = pop.merge_census([pop.census_tu(tr.ci(), w.tu, self.ctx, tr.FENCE, self.read)])
        return m, pop.confirm(m)

    def names(self, m):
        return {r["name"]: g for g, r in m["globals"].items()}

    def test_const_test_on_known_members(self):
        m, _ = self.census("core/a.c", POPULATION)
        n = self.names(m)
        for out in ("k_c1", "k_c2", "k_c3", "t_local", "s_cap"):
            self.assertNotIn(out, n)
        for member in ("s_names", "s_plain", "s_init_ref"):
            self.assertIn(member, n)
        self.assertEqual(m["globals"][n["s_guarded"]]["form"], "named")
        self.assertEqual(m["globals"][n["s_atomic"]]["form"], "atomic")

    def test_function_pointer_form_ends_at_its_declaration(self):
        # The `)` closing `(*s_cb_b` must not hide the `;` that ends the
        # declaration, or the next declaration's form is read as its own.
        m, _ = self.census("core/a.c", "static void (*s_cb_a)(unsigned) OP_GUARDED_BY(sim);\n"
                                       "static void (*s_cb_b)(unsigned);\n"
                                       "static int s_after OP_GUARDED_BY(sim);\n"
                                       "static float (*s_t)[3] OP_GUARDED_BY(sim);\n"
                                       "void use(void) { s_cb_a = 0; s_cb_b = 0; s_after = 0; s_t = 0; }\n")
        n = self.names(m)
        forms = {k: m["globals"][n[k]]["form"] for k in ("s_cb_a", "s_cb_b", "s_after", "s_t")}
        self.assertEqual(forms, {"s_cb_a": "named", "s_cb_b": "", "s_after": "named", "s_t": "named"})

    def test_macro_static_counts_once_at_the_macro(self):
        m, _ = self.census("core/a.c", POPULATION)
        keys = [g for g, r in m["globals"].items() if r["function"].startswith("#define:")]
        self.assertEqual(keys, ["S|core/a.c|#define:SITE|site"])

    def test_confirmed_forms(self):
        m, conf = self.census("core/a.c", POPULATION)
        n = self.names(m)
        self.assertEqual(conf[n["s_store_only"]][0], "atomic")
        self.assertEqual(conf[n["v"]][0], "once")
        for no in ("s_pair_a", "s_pair_b", "s_rmw", "s_addr", "s_names", "s_init_ref"):
            self.assertNotIn(n[no], conf, no)

    def test_seal_needs_a_boot_only_writer(self):
        src = ("int openpete_run(void);\nstatic int s_boot; static int s_late;\n"
               "static void w(void) { s_boot = 1; }\nstatic void l(void) { s_late = 1; }\n"
               "int main(void) {\n  w();\n  openpete_run();\n  l();\n  return s_boot + s_late;\n}\n")
        m, conf = self.census("core/a.c", src)
        n = self.names(m)
        self.assertEqual(conf[n["s_boot"]][0], "atomic")      # the scalar shape wins
        d = pop.boot_data(m)
        self.assertTrue(pop.boot_only(["c:a.c@F@w"], d))
        self.assertFalse(pop.boot_only(["c:a.c@F@l"], d))

    def rule_of(self, tu, rows=""):
        head = "file\tfunction\tname\tkind\tform\tnote\n"
        return pop.rule([tu], pop.Baseline(head + rows), pop.DeferredList(""))

    def test_seeded_global_in_a_touched_function_refuses(self):
        r = self.rule_of(self.scan("core/a.c", POPULATION, ["names_plain"]))
        names = {r["recs"][g]["name"] for g in r["refused"]}
        self.assertEqual(names, {"s_plain", "s_names"})
        self.assertEqual(r["extents"], 1)
        self.assertEqual(r["passed_form"], 2)
        self.assertIn("Choose the owner", r["text"][0])

    def test_listed_global_still_refuses_and_carries_its_form(self):
        row = "core/a.c\t-\ts_store_only\tstatic\tatomic\t1 storing function(s)\n"
        r = self.rule_of(self.scan("core/a.c", POPULATION, ["void writer"]), row)
        self.assertEqual(len(r["refused"]), 1)
        self.assertIn("confirmed form atomic", r["text"][0])
        self.assertIn("listed", r["text"][0])

    def test_function_with_no_globals_passes(self):
        r = self.rule_of(self.scan("core/a.c", POPULATION, ["no_globals"]))
        self.assertEqual((r["extents"], r["named"], r["refused"]), (1, 0, []))

    def test_sizeof_names_nothing(self):
        r = self.rule_of(self.scan("core/a.c", POPULATION, ["size_only"]))
        self.assertEqual((r["extents"], r["named"]), (1, 0))

    def test_file_scope_initializer_touched(self):
        r = self.rule_of(self.scan("core/a.c", POPULATION, ["int* s_init_ref"]))
        names = {r["recs"][g]["name"] for g in r["refused"]}
        self.assertEqual(names, {"s_init_ref", "s_init_target"})

    def test_deferred_row_passes(self):
        tu = self.scan("core/a.c", POPULATION, ["void writer"])
        head = "file\tfunction\tname\tkind\tform\tnote\n"
        d = pop.DeferredList(head + "core/a.c\t-\ts_store_only\tstatic\t-\trole=sim\n")
        r = pop.rule([tu], pop.Baseline(head), d)
        self.assertEqual((r["refused"], r["passed_deferred"]), ([], 1))

    def test_cpp_extents(self):
        for touch, name in (("Obj() {", "s_ctor"), ("~Obj", "s_dtor"), ("T tpl", "s_tpl"),
                            ("void m()", "s_inline_m")):
            r = self.rule_of(self.scan("core/b.cpp", POPULATION_CPP, [touch]))
            got = {r["recs"][g]["name"] for g in r["refused"]}
            self.assertIn(name, got, touch)

    def test_header_extent_deduplicates_by_usr(self):
        hdr = "static int s_h;\nstatic inline void hf(void) { s_h = 1; }\n"
        path = os.path.join(self.tmp, "core", "include", "h.h")
        with open(path, "w") as fh:
            fh.write(hdr)
        plan = {"hunks": {"core/include/h.h": [(2, 1)]}, "stateless": False, "macros": set()}
        tus = []
        for rel in ("core/a.c", "core/b.c"):
            w = self.parse(rel, '#include "h.h"\n')
            tus.append(pop.scan_tu(tr.ci(), w.tu, plan, self.ctx, tr.FENCE, self.read))
        r = pop.rule(tus, pop.Baseline(""), pop.DeferredList(""))
        self.assertEqual((r["extents"], len(r["refused"])), (1, 1))

    def verdict(self, hunks, tus, rows=""):
        """-> (new-global exit code, extent exit code, printed text)."""
        out = io.StringIO()
        head = "file\tfunction\tname\tkind\tform\tnote\n"
        with contextlib.redirect_stderr(out):
            new, extent, _ = tr.c2_verdict(self.tmp, tus, hunks, pop.Baseline(head + rows),
                                           pop.DeferredList(""), self.read)
        return new, extent, out.getvalue()

    def test_code_no_tu_reached_is_vacuous_and_refuses(self):
        with open(os.path.join(self.tmp, "core", "u.h"), "w") as fh:
            fh.write("int f(void);\n")
        rc, _, text = self.verdict({"core/u.h": [(1, 1)]}, [])
        self.assertEqual(rc, 1)
        self.assertIn("VACUOUS: check 2 reached no TU", text)

    def test_no_extent_touched_prints_vacuous(self):
        tu = self.scan("core/a.c", "/* x */\nint f(void) { return 0; }\n", [])
        rc, _, text = self.verdict({"core/a.c": [(1, 1)]}, [tu])
        self.assertEqual(rc, 0)
        self.assertIn("VACUOUS: no extent or declaration touched", text)
        self.assertNotIn("PASS", text)

    def test_new_global_refuses_and_a_listed_one_reports(self):
        # A global with no form and no row is refused by the new-global
        # half, which CHECK2_ARMED does not gate; the same global with a row,
        # named in a touched function, is an extent finding only.
        src = "static int s_new;\nint touch(void) { return s_new; }\n"
        hunks = {"core/a.c": [(2, 1)]}
        new, extent, text = self.verdict(hunks, [self.scan("core/a.c", src, ["touch"])])
        self.assertEqual((new, extent), (1, 1))
        self.assertIn("1 global(s) with no form and no row (refused)", text)
        row = "core/a.c\t-\ts_new\tstatic\t-\towed\n"
        new, extent, _ = self.verdict(hunks, [self.scan("core/a.c", src, ["touch"])], row)
        self.assertEqual((new, extent), (0, 1))

    def test_lock_handles_are_exempt_by_type(self):
        m, _ = self.census("core/a.c", "typedef struct SDL_Mutex SDL_Mutex;\nstatic SDL_Mutex* s_mx;\n"
                           "SDL_Mutex* get(void) { return s_mx; }\n")
        self.assertEqual(self.names(m), {})

    def test_once_is_confirmed_in_cpp(self):
        m, conf = self.census("core/b.cpp", "int f() { static int v = -1; if (v < 0) v = 3; return v; }\n")
        self.assertEqual(conf[self.names(m)["v"]][0], "once")

    def test_stateless_half(self):
        tu = self.scan("core/a.c", '#include "openpete_thread_roles.h"\nstatic int s_x;\n'
                       'static int s_y OP_GUARDED_BY(sim);\n', [])
        recs, declared = tu["globals"], tu["declared"]
        head = "file\tfunction\tname\tkind\tform\tnote\n"
        new, rows = pop.stateless(recs, declared, pop.Baseline(""), pop.DeferredList(""))
        self.assertEqual((len(new), rows), (1, []))
        self.assertIn("s_x has no form and no row", new[0])
        new, rows = pop.stateless(recs, declared, pop.Baseline(
            head + "core/a.c\t-\ts_x\tstatic\t-\t\ncore/a.c\t-\ts_y\tstatic\t-\t\n"),
            pop.DeferredList(""))
        self.assertEqual((new, len(rows)), ([], 1))
        self.assertIn("s_y has a form and still has a row", rows[0])


class ReportModeTest(unittest.TestCase):
    def test_report_mode_refuses_nothing_and_says_so(self):
        out = io.StringIO()
        with contextlib.redirect_stderr(out):
            self.assertEqual(tr.c2_exit(1), 0)
            self.assertEqual(tr.c2_exit(0), 0)
        self.assertEqual(out.getvalue().count("REPORT ONLY"), 1)

    def test_refused_line_names_unparsed_entries(self):
        ok = {"rel": "core/a.c", "parse": []}
        bad = {"rel": "core/b.c", "parse": ["x: fatal error: 'g.h' file not found",
                                            "x: fatal error: 'g.h' file not found (libclang 14)"]}
        self.assertEqual(tr.refused_line([ok]), "thread-roles: REFUSED")
        self.assertEqual(tr.refused_line([ok, bad]), "thread-roles: REFUSED (VACUOUS: 1 TU(s) "
                         "unparsed, nothing in them checked)")
        self.assertEqual(tr.unparsed([ok, bad, bad]), 2)

    def test_armed_refuses(self):
        saved = tr.CHECK2_ARMED
        tr.CHECK2_ARMED = True
        try:
            self.assertEqual(tr.c2_exit(1), 1)
        finally:
            tr.CHECK2_ARMED = saved


class RowFormTest(unittest.TestCase):
    def test_forms_split_and_direct_is_one(self):
        self.assertEqual(tr.row_forms(("arg:x|direct:", {"sim"})), {"arg:x", "direct:"})

    def test_whole_selection_modes(self):
        self.assertTrue(tr.whole_selection("check --all"))
        self.assertTrue(tr.whole_selection("hook (whole tree: a staged header; x)"))
        self.assertFalse(tr.whole_selection("hook (1 staged source(s); x)"))


class ListsTest(unittest.TestCase):
    HEAD = "file\tfunction\tname\tkind\tform\tnote\n"

    def base(self, *rows):
        return pop.Baseline(self.HEAD + "".join("\t".join(r) + "\n" for r in rows))

    def test_added_row_needs_the_reason_naming_it(self):
        before = self.base()
        after = self.base(("a.c", "-", "s_x", "static", "-", "role=sim; ORPHAN f"))
        self.assertTrue(pop.additions_refused("t", before, after, {}, "")[0])
        self.assertTrue(pop.additions_refused("t", before, after, {}, "s_x why",
                                              need_note=True)[0])
        self.assertTrue(pop.additions_refused("t", before, after, {}, "other")[0])
        self.assertEqual(pop.additions_refused("t", before, after, {}, "s_x orphan")[0], [])
        # The accept reason without a note: a later range could not see why.
        bare = self.base(("a.c", "-", "s_x", "static", "-", ""))
        ref = pop.additions_refused("t", before, bare, {}, "s_x orphan")[0]
        self.assertIn("no recorded reason in its note", ref[0])

    def test_stateless_compare_reports_a_row_with_a_note(self):
        after = self.base(("a.c", "-", "s_x", "static", "-", "chain"))
        ref, rep = pop.additions_refused("t", self.base(), after, {}, None)
        self.assertEqual((ref, len(rep)), ([], 1))
        bare = self.base(("a.c", "-", "s_x", "static", "-", ""))
        self.assertEqual(len(pop.additions_refused("t", self.base(), bare, {}, None)[0]), 1)

    def test_rename_carries_a_row(self):
        before = self.base(("a.c", "-", "s_x", "static", "-", ""))
        after = self.base(("b.c", "-", "s_x", "static", "-", ""))
        self.assertEqual(pop.added_rows(before, after, {"a.c": "b.c"}), [])
        self.assertEqual(pop.added_rows(before, after, {}), [("b.c", "-", "s_x")])

    def test_reason_may_name_the_chain_terminal(self):
        note = "role=sim; reason=no row at walk_sector_job yet; ORPHAN walk_sector_job (x); chain y"
        after = self.base(("a.c", "-", "s_x", "static", "-", note))
        reason = "no row at walk_sector_job yet"
        self.assertEqual(pop.additions_refused("t", self.base(), after, {}, reason,
                                               need_note=True)[0], [])

    def test_deferred_row_refused_on_a_confirmed_form_or_no_chain(self):
        un = self.base(("a.c", "-", "s_x", "static", "atomic", "shape"))
        d = self.base(("a.c", "-", "s_x", "static", "-", "chain"))
        ref = pop.additions_refused("t", self.base(), d, {}, "s_x", confirmed=un)[0]
        self.assertIn("confirms the local form atomic", ref[0])
        bare = self.base(("b.c", "-", "s_y", "static", "-", ""))
        ref = pop.additions_refused("t", self.base(), bare, {}, "s_y", need_note=True)[0]
        self.assertIn("no annotator chain", ref[0])

    def test_tighten_move_is_not_an_addition(self):
        d = self.base(("a.c", "-", "s_x", "static", "-", "chain"))
        un = self.base(("a.c", "-", "s_x", "static", "-", ""))
        self.assertEqual(pop.additions_refused("t", self.base(), un, {}, "", moved_from=d)[0], [])

    def test_includers_follow_headers(self):
        text = {"a.c": '#include "h1.h"\n', "b.c": "", "h1.h": '#include "dir/h2.h"\n',
                "dir/h2.h": ""}
        self.assertEqual(pop.includers({"a.c", "b.c"}, list(text), text.get, {"dir/h2.h"}),
                         {"a.c"})

    def test_touched_rule(self):
        self.assertTrue(pop.touched(10, 20, [(20, 1)]))
        self.assertFalse(pop.touched(10, 20, [(21, 1)]))
        self.assertTrue(pop.touched(10, 20, [(19, 0)]))
        self.assertFalse(pop.touched(10, 20, [(20, 0)]))


class MarkerTest(unittest.TestCase):
    """marker_refusals over a scratch repository."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="thread-roles-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        os.makedirs(os.path.join(self.tmp, "core"))
        self.git("init", "-q")
        self.git("config", "user.email", "t@t")
        self.git("config", "user.name", "t")

    def git(self, *a):
        return tr.git(self.tmp, *a)

    def stage(self, text, msg=None):
        with open(os.path.join(self.tmp, "core", "a.c"), "w") as fh:
            fh.write(text)
        self.git("add", "core/a.c")
        if msg:
            self.git("commit", "-qm", msg)

    def test_first_form_needs_no_marker_and_a_change_does(self):
        self.stage("static int s_x;\n", "base")
        self.stage("static int s_x OP_GUARDED_BY(sim);\n")
        self.assertEqual(tr.marker_refusals(self.tmp, "HEAD", ["core/a.c"]), [])
        self.git("commit", "-qm", "form")
        self.stage("static int s_x OP_GUARDED_BY(main);\n")
        self.assertEqual(len(tr.marker_refusals(self.tmp, "HEAD", ["core/a.c"])), 1)
        self.stage("static int s_x OP_GUARDED_BY(main); /* thread-roles: moved */\n")
        self.assertEqual(tr.marker_refusals(self.tmp, "HEAD", ["core/a.c"]), [])

    def test_first_form_of_a_function_pointer_or_array_pointer(self):
        # The name sits inside a declarator group: `(*s_cb)(unsigned)`.
        self.stage("static void (*s_cb)(unsigned f);\nstatic float (*s_t)[3];\n", "base")
        self.stage("static void (*s_cb)(unsigned f) OP_GUARDED_BY(sim) OP_SLOT(sim);\n"
                   "static float (*s_t)[3] OP_GUARDED_BY(sim);\n")
        self.assertEqual(tr.marker_refusals(self.tmp, "HEAD", ["core/a.c"]), [])
        self.git("commit", "-qm", "form")
        self.stage("static void (*s_cb)(unsigned f) OP_GUARDED_BY(main) OP_SLOT(sim);\n"
                   "static float (*s_t)[3] OP_GUARDED_BY(sim);\n")
        self.assertEqual(len(tr.marker_refusals(self.tmp, "HEAD", ["core/a.c"])), 1)

    def test_declarator_name(self):
        for text, want in (("static int x", "x"), ("static int s_a[2][8]", "s_a"),
                           ("static float (*s_t)[3][3]", "s_t"),
                           ("static void (*s_cb)(unsigned frame)", "s_cb"),
                           ("static void (*s_cbs[4])(int)", "s_cbs"),
                           ("static int * const p", "p")):
            self.assertEqual(tr.declarator_name(text), want, text)

    def test_range_reads_the_commits(self):
        # base..rev: a form change committed without its marker, which a
        # cherry-pick carries past the hook.
        self.stage("static int s_x OP_GUARDED_BY(sim);\n", "base")
        self.stage("static int s_x OP_GUARDED_BY(main);\n", "change")
        self.assertEqual(tr.marker_refusals(self.tmp, "HEAD", ["core/a.c"]), [])
        self.assertEqual(len(tr.marker_refusals(self.tmp, "HEAD~1", ["core/a.c"], "HEAD")), 1)


class AnnotatorTextTest(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, os.path.join(ROOT, "scripts"))
        import thread_roles_annotate as ta
        self.ta = ta

    def test_place_adds_and_upgrades(self):
        b = b"static int f(int a,\n              int b) {\n  return a;\n}\n"
        out, changed = self.ta.place(b, 1, 12, "f", "sim", False)
        self.assertTrue(changed)
        self.assertIn(b"int b) OP_REQUIRES_SHARED(sim) {", out)
        out2, changed = self.ta.place(out, 1, 12, "f", "sim", True)
        self.assertTrue(changed)
        self.assertIn(b"int b) OP_REQUIRES(sim) {", out2)
        out3, changed = self.ta.place(out2, 1, 12, "f", "sim", False)
        self.assertFalse(changed)

    def test_place_refuses_a_name_not_spelled_there(self):
        with self.assertRaises(ValueError):
            self.ta.place(b"#define G f\nint G(void);\n", 2, 5, "f", "sim", True)

    def test_place_past_a_returned_array_pointer(self):
        # T (*f(P))[N][M]: the attribute after the parameter list would sit
        # inside the declarator group; it goes past the array bounds.
        b = b"float (*room(int n))[3][3];\nfloat (*room(int n))[3][3] {\n  return 0;\n}\n"
        out, changed = self.ta.place(b, 1, 9, "room", "sim", False)
        self.assertTrue(changed)
        self.assertIn(b"float (*room(int n))[3][3] OP_REQUIRES_SHARED(sim);\n", out)
        out, changed = self.ta.place(out, 2, 9, "room", "sim", True)
        self.assertIn(b"float (*room(int n))[3][3] OP_REQUIRES(sim) {", out)
        self.assertFalse(self.ta.place(out, 2, 9, "room", "sim", False)[1])

    def test_place_past_a_returned_function_pointer(self):
        b = b"void (*(*pick(int k))(char))(double);\nint (* const get(void))(int);\n"
        out, _ = self.ta.place(b, 1, 10, "pick", "sim", True)
        self.assertIn(b"void (*(*pick(int k))(char))(double) OP_REQUIRES(sim);", out)
        out, _ = self.ta.place(out, 2, 14, "get", "sim", True)
        self.assertIn(b"int (* const get(void))(int) OP_REQUIRES(sim);", out)

    def test_declarator_end_past_groups(self):
        for text, name, want in (
                (b"static float (*s_t)[3][3];", b"s_t", b"static float (*s_t)[3][3]"),
                (b"static void (*s_cb)(int a);", b"s_cb", b"static void (*s_cb)(int a)"),
                (b"static void (*s_cbs[4])(int);", b"s_cbs", b"static void (*s_cbs[4])(int)"),
                (b"static int s_a[2][8];", b"s_a", b"static int s_a[2][8]"),
                (b"static char *s_p;", b"s_p", b"static char *s_p")):
            at = text.index(name)
            self.assertEqual(text[:self.ta.declarator_end(text, at, name.decode())], want)

    def test_ensure_include_after_the_last_top_include(self):
        b = b'#include <a.h>\n#include "b.h"\nstruct s { int x; };\n#include "c.h"\n'
        out = self.ta.ensure_include(b)
        self.assertTrue(out.startswith(b'#include <a.h>\n#include "b.h"\n#include "openpete_thread_roles.h"'))
        self.assertEqual(self.ta.ensure_include(out), out)


if __name__ == "__main__":
    unittest.main()
