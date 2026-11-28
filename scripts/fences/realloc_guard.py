#!/usr/bin/env python3
"""Fence: a realloc result is held apart from the block it replaces and tested before use.

realloc returns NULL on failure and leaves the old block allocated. Assigning
the result to the pointer passed in (`p = realloc(p, n)`) loses that block
and leaves a NULL table behind a capacity that claims memory; using the
result before a NULL test writes through NULL. The form this fence accepts
assigns the result to a separate pointer, tests it, and only then stores it
and the new capacity.

A call is realloc, SDL_realloc or reallocarray. Refused:
  same     the result is assigned to the expression passed as the block;
  untested the assigned pointer's next appearance after the statement is not
           a NULL test (`!p`, `p == NULL`, `p != NULL`, `if (p)`, `p ?`,
           `p &&`, `&& p)`, `assert(p`), nor `return p;`, which hands the
           result to a caller; an assignment tested in place
           (`if (!(p = realloc(...)))`, `if ((p = realloc(...)) == NULL)`)
           is a test;
  bare     the result is neither assigned nor returned.
A declaration of the function itself (a type or `*` before the name) is not
a call. A call whose line, or the line above, carries
`realloc-ok: <reason>` is exempt; the reason says why the form is safe.
Scope: C and C++ sources under core/, games/ and tests/, generated
directories and the decomp checkouts (games/<game>/<game>/) excluded.
Comments and string literals are ignored.

Run:
  python3 scripts/fences/realloc_guard.py tree [root]   every source (build target)
  python3 scripts/fences/realloc_guard.py hook          the staged sources, read from the index
Exit: 0 clean, 66 a refused call, 65 VACUOUS (tree mode scanned no call).

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import os
import re
import subprocess
import sys

EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")
TOPS = ("core", "games", "tests")
MARKER = re.compile(r"realloc-ok:\s*\S")
CALL = re.compile(r"\b(?:SDL_realloc|reallocarray|realloc)\s*\(")
LVALUE = re.compile(r"([A-Za-z_]\w*(?:\s*(?:\.|->)\s*[A-Za-z_]\w*|\s*\[[^\[\]]*\])*)\s*$")
ARRAY_PTR_DECL = re.compile(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)\s*(?:\[[^\[\]]*\]\s*)+$")
CAST = re.compile(r"^\s*(?:\(\s*[A-Za-z_][\w\s\*\(\)\[\]:<>,]*?\)\s*"
                  r"|(?:static|reinterpret)_cast\s*<[^<>]*>\s*\(\s*)")
NULLISH = r"(?:NULL|nullptr|0)"
WINDOW = 6000


def strip_code(text):
    """Blank out comments and string and character literals, keeping line breaks and offsets."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    pat = re.compile(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)
    return pat.sub(blank, text)


def close_paren(code, i):
    """Index of the parenthesis closing the one at i, or -1."""
    depth = 0
    for j in range(i, len(code)):
        c = code[j]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return j
    return -1


def first_arg(code, open_i, close_i):
    """The call's first argument text."""
    depth = 0
    for j in range(open_i + 1, close_i):
        c = code[j]
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        elif c == "," and depth == 0:
            return code[open_i + 1:j]
    return code[open_i + 1:close_i]


def norm(expr):
    """An expression with whitespace, outer parentheses and leading casts removed."""
    e = expr.strip()
    while True:
        m = CAST.match(e)
        if m and m.end() < len(e):
            e = e[m.end():].strip()
            continue
        if e.startswith("(") and close_paren(e, 0) == len(e) - 1:
            e = e[1:-1].strip()
            continue
        break
    return re.sub(r"\s+", "", e)


def statement_start(code, i):
    """Offset just past the last `;`, `{` or `}` before i."""
    k = max(code.rfind(";", 0, i), code.rfind("{", 0, i), code.rfind("}", 0, i))
    return k + 1


def assignment(prefix):
    """(lvalue, text before it) of the last plain `=` in a statement prefix, or None."""
    for m in reversed(list(re.finditer(r"(?<![=!<>+\-*/%&|^])=(?!=)", prefix))):
        left = prefix[:m.start()]
        lv = ARRAY_PTR_DECL.search(left) or LVALUE.search(left)
        if lv:
            return lv.group(1), left[:lv.start()]
    return None


def lvalue_pattern(lv):
    """A regex matching the lvalue as a whole operand."""
    parts = re.split(r"(\.|->|\[[^\[\]]*\])", re.sub(r"\s+", "", lv))
    body = r"\s*".join(re.escape(p) for p in parts if p)
    return re.compile(r"(?<![\w.>])" + body + r"(?![\w])")


def tested_in_place(before_lv, after_call):
    """Whether the assignment expression is itself the operand of a NULL test."""
    if re.search(r"!\s*\(\s*$", before_lv):
        return True
    if re.search(r"\(\s*$", before_lv):
        if re.match(r"\s*\)\s*(?:==|!=)\s*" + NULLISH + r"\b", after_call):
            return True
        if re.search(r"\b(?:if|while)\s*\(\s*\(\s*$", before_lv) and re.match(r"\s*\)\s*(?:\)|&&|\|\|)", after_call):
            return True
    return False


def is_test(code, m):
    """Whether the lvalue occurrence m is a NULL test of it or hands it back with `return`."""
    before = code[max(0, m.start() - 80):m.start()]
    after = code[m.end():m.end() + 80]
    if re.match(r"\s*(?:\.|->|\[|\(|=(?!=))", after):
        return False                      # a dereference, a call or a store
    if re.search(r"(?<![!=])!\s*$", before):
        return True
    if re.match(r"\s*(?:==|!=)\s*" + NULLISH + r"\b", after):
        return True
    if re.search(r"\b" + NULLISH + r"\s*(?:==|!=)\s*$", before):
        return True
    if re.match(r"\s*\?", after):
        return True
    if re.match(r"\s*(?:\)|&&|\|\|)", after) and \
            re.search(r"(?:\b(?:if|while|assert\w*)\s*\(|&&|\|\|)\s*$", before):
        return True
    return bool(re.search(r"\breturn\s+$", before) and re.match(r"\s*;", after))


def scan(path, text):
    """(sites, refusal lines) of one source."""
    code = strip_code(text)
    raw = text.split("\n")
    out, sites = [], 0
    for m in CALL.finditer(code):
        pre = code[:m.start()].rstrip()
        if pre.endswith((".", "->")) or (pre.endswith("::") and not pre.endswith("std::")):
            continue              # a member or another scope's function
        if re.search(r"[\w*&]$", pre) and not re.search(r"\breturn$", pre):
            continue              # a declaration or definition of the function
        open_i = m.end() - 1
        close_i = close_paren(code, open_i)
        if close_i < 0:
            continue
        sites += 1
        line = code.count("\n", 0, m.start()) + 1
        if MARKER.search(raw[line - 1]) or (line >= 2 and MARKER.search(raw[line - 2])):
            continue
        start = statement_start(code, m.start())
        prefix = code[start:m.start()]
        asg = assignment(prefix)
        where = "%s:%d" % (path, line)
        if not asg:
            if not re.match(r"\s*return\b", prefix):
                out.append("%s: realloc result is neither assigned nor returned" % where)
            continue
        lv, before_lv = asg
        block = first_arg(code, open_i, close_i)
        if norm(lv) == norm(block):
            out.append("%s: `%s = %s(%s, ...)` loses the block on failure; assign to a separate pointer, "
                       "test it, then store it" % (where, norm(lv), m.group(0).rstrip("( \t"), norm(block)))
            continue
        if tested_in_place(before_lv, code[close_i + 1:close_i + 81]):
            continue
        semi = code.find(";", close_i)
        if semi < 0:
            semi = close_i
        pat = lvalue_pattern(lv)
        nxt = pat.search(code, semi + 1, min(len(code), semi + 1 + WINDOW))
        if not nxt or not is_test(code, nxt):
            used = code.count("\n", 0, nxt.start()) + 1 if nxt else None
            out.append("%s: realloc result `%s` is %s" % (
                where, norm(lv), "used at line %d before a NULL test" % used if used else "never NULL-tested"))
    return sites, out


def in_scope(rel):
    """Whether a repo-relative path is a scanned source."""
    if not rel.endswith(EXTS) or not rel.startswith(tuple(t + "/" for t in TOPS)):
        return False
    parts = rel.split("/")
    if "generated" in parts:
        return False
    return not (len(parts) > 2 and parts[0] == "games" and parts[2] == parts[1])


def tree_sources(root):
    """(rel, text) of every in-scope source under root."""
    for top in TOPS:
        for d, dirs, files in os.walk(os.path.join(root, top)):
            rel_d = os.path.relpath(d, root)
            dirs[:] = sorted(x for x in dirs if in_scope(os.path.join(rel_d, x, "x.c")))
            for f in sorted(files):
                rel = os.path.join(rel_d, f)
                if in_scope(rel):
                    with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
                        yield rel, fh.read()


def staged_sources(root):
    """(rel, text) of every staged in-scope source, read from the index."""
    names = subprocess.run(["git", "-C", root, "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
                           capture_output=True, text=True, check=True).stdout.split()
    for rel in (n for n in names if in_scope(n)):
        p = subprocess.run(["git", "-C", root, "show", ":" + rel], capture_output=True, text=True)
        if p.returncode == 0:
            yield rel, p.stdout


def main(argv):
    """Run the fence in tree or hook mode."""
    mode = argv[1] if len(argv) > 1 else "tree"
    root = os.path.abspath(argv[2]) if len(argv) > 2 else \
        os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    refused, files, sites = [], 0, 0
    for rel, text in (tree_sources(root) if mode == "tree" else staged_sources(root)):
        files += 1
        n, r = scan(rel, text)
        sites += n
        refused += r
    for r in refused:
        print("realloc-guard: REFUSED %s" % r)
    if mode == "tree" and sites == 0:
        print("realloc-guard: VACUOUS: zero realloc calls in %d source(s) under %s" % (files, root))
        return 65
    print("realloc-guard: %s (%d call(s) in %d source(s) scanned)" % ("FAIL" if refused else "PASS", sites, files))
    return 66 if refused else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
