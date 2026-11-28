#!/usr/bin/env python3
"""Tests for the realloc fence (realloc_guard.py).

Planted refusals: the result assigned to the block passed in (plain, cast,
member, C++ cast, reallocarray, SDL_realloc, inside an in-place test), the
result stored or dereferenced before a NULL test, and a bare call. Planted
passes: the separate-pointer form with each accepted test, the in-place
tests, a ternary chain, `return realloc(...)`, a pointer-to-array
declarator, the marker, and declarations, members and comments. A root
with no call is VACUOUS; a root with only clean calls passes.

Run: python3 scripts/fences/tests/test_realloc_guard.py
(rumble-guard: no-launch, wslenv-guard: no-launch)
"""
import os
import shutil
import sys
import tempfile
import unittest

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, FENCES)

import realloc_guard as g  # noqa: E402

BAD = {
    "same_plain": "void f(void) { s_p = realloc(s_p, n); }\n",
    "same_cast": "void f(void) { if (n == cap) { cap *= 2; s_p = (row_t*)realloc(s_p, cap * sizeof(*s_p)); } }\n",
    "same_member": "void f(T* o) { o->v = (int*)realloc(o->v, (size_t)o->cap * sizeof *o->v); }\n",
    "same_array_cast": "void f(T* o) { o->rc = (float (*)[3])realloc(o->rc, n); }\n",
    "same_cxx_cast": "void f() { s_b = static_cast<uint8_t*>(std::realloc(s_b, n)); }\n",
    "same_in_test": "int f(T* o) { if (!(o->c = (P*)realloc(o->c, n))) return 0; return 1; }\n",
    "same_reallocarray": "void f(void) { a = reallocarray(a, n, 8); }\n",
    "same_sdl": "void f(void) { p->buf = SDL_realloc(p->buf, n); }\n",
    "stored_before_test": "void f(S* s) { uint16_t* u = realloc(s->under, n); s->under = u; if (!u) return; }\n",
    "deref_before_test": "void f(void) { int* q = realloc(p, n); q[0] = 1; if (!q) return; p = q; }\n",
    "memcpy_untested": "void f(void) { char* q = realloc(p, n); memcpy(q, src, n); }\n",
    "never_tested": "void f(void) { void* q = realloc(p, n); }\n",
    "bare": "void f(void) { (void)realloc(p, n); }\n",
    "passed_on": "void f(void) { use(realloc(p, n)); }\n",
}

GOOD = {
    "not_test": "void f(void) { int* q = realloc(p, n); if (!q) return; p = q; }\n",
    "eq_null": "int f(void) { int* q = realloc(p, n);\n  if (q == NULL) return -1;\n  p = q; return 0; }\n",
    "null_ne": "void f(void) { int* q = realloc(p, n); if (NULL != q) p = q; }\n",
    "if_q": "void f(void) { int* q = realloc(p, n); if (q) { p = q; cap = n; } }\n",
    "ternary_chain": ("int f(D* d) { void* v = realloc(d->v, a); void* p = v ? realloc(d->pv, b) : NULL;\n"
                      "  if (v) d->v = v; if (p) d->pv = p; if (!p) return -1; return 0; }\n"),
    "in_place_not": "int f(void) { if (!(q = realloc(p, n))) return 0; p = q; return 1; }\n",
    "in_place_eq": "int f(void) { if ((q = (int*)realloc(p, n)) == NULL) return 0; p = q; return 1; }\n",
    "and_operand": "void f(void) { int* q = realloc(p, n); if (n && q) p = q; }\n",
    "assert": "void f(void) { int* q = realloc(p, n); assert(q); p = q; }\n",
    "return_call": "void* grow(void* p, size_t n) { return realloc(p, n); }\n",
    "return_cast": "T* grow(T* p, size_t n) { return (T*)realloc(p, n); }\n",
    "return_var": "void* grow(void* p, size_t n) { void* q = realloc(p, n); return q; }\n",
    "array_ptr_decl": ("bool f(L* l) { VkImageView (*views)[4] = realloc(l->views, cap * sizeof(*views));\n"
                       "  if (!views) return false; l->views = views; return true; }\n"),
    "marker": "void f(void) {\n  /* realloc-ok: the process exits on NULL below */\n  s_p = realloc(s_p, n);\n}\n",
    "marker_same_line": "void f(void) { s_p = realloc(s_p, n); /* realloc-ok: arena never fails */ }\n",
    "declaration": "void* realloc(void* p, size_t n);\nextern void *realloc(void *, size_t);\n",
    "definition": "void* realloc(void* p, size_t n) { return __libc_realloc(p, n); }\n",
    "member_call": "void f(A* a) { a->realloc(p, n); alloc.realloc(p, n); Pool::realloc(p, n); }\n",
    "comment_and_string": "/* s_p = realloc(s_p, n); */\n// q = realloc(q, n);\nconst char* m = \"p = realloc(p, n)\";\n",
}


def refusals(text):
    """Refusal lines of one fixture source."""
    return g.scan("x.c", text)[1]


class ReallocGuardTest(unittest.TestCase):
    def test_bad_refused(self):
        for name, src in BAD.items():
            with self.subTest(name=name):
                self.assertEqual(len(refusals(src)), 1, src)

    def test_good_passes(self):
        for name, src in GOOD.items():
            with self.subTest(name=name):
                self.assertEqual(refusals(src), [], src)

    def test_refusal_names_line(self):
        r = refusals("int a;\nvoid f(void) {\n  s_p = realloc(s_p, n);\n}\n")
        self.assertEqual(len(r), 1)
        self.assertIn("x.c:3:", r[0])

    def test_call_counts(self):
        self.assertEqual(g.scan("x.c", GOOD["ternary_chain"])[0], 2)
        self.assertEqual(g.scan("x.c", GOOD["declaration"])[0], 0)
        self.assertEqual(g.scan("x.c", GOOD["comment_and_string"])[0], 0)

    def test_scope(self):
        self.assertTrue(g.in_scope("core/diag/x.c"))
        self.assertTrue(g.in_scope("tests/sched/lib/explore.c"))
        self.assertTrue(g.in_scope("games/common/psyq/psyq_thunks.cpp"))
        self.assertFalse(g.in_scope("games/spyro-1/spyro-1/src/main.c"))
        self.assertFalse(g.in_scope("games/spyro-1/generated/recomp_0.c"))
        self.assertFalse(g.in_scope("third_party/x/y.c"))
        self.assertFalse(g.in_scope("core/diag/x.py"))


class TreeModeTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="realloc-guard-test-")

    def tearDown(self):
        shutil.rmtree(self.root)

    def write(self, rel, text):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(text)

    def test_vacuous(self):
        self.write("core/a.c", "int x;\n")
        self.assertEqual(g.main(["realloc_guard.py", "tree", self.root]), 65)

    def test_clean_tree_passes(self):
        self.write("core/a.c", GOOD["not_test"])
        self.write("tests/b.c", GOOD["ternary_chain"])
        self.write("games/spyro-1/spyro-1/src/c.c", BAD["same_plain"])     # decomp checkout: out of scope
        self.write("games/common/generated/d.c", BAD["same_plain"])        # generated: out of scope
        self.assertEqual(g.main(["realloc_guard.py", "tree", self.root]), 0)

    def test_planted_refused(self):
        self.write("core/a.c", GOOD["not_test"])
        self.write("games/common/e.cpp", BAD["same_cxx_cast"])
        self.assertEqual(g.main(["realloc_guard.py", "tree", self.root]), 66)


if __name__ == "__main__":
    unittest.main()
