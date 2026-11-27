#!/usr/bin/env python3
"""Fence: OPENPETE_TILE_CENSUS selects no code path.

The tile census counts the scene every build composes; the variable gates
only whether a capture records its tally and writes the sidecar beside the
tile. If any other code tested it, a golden capture (which sets it) would run
code a player's session does not, and the golden would stop describing the
shipped renderer. So the variable has one accessor, tile_census_on()
(core/diag/tile_census.c), and the accessor is called only inside the two
capture helpers of core/renderer/native_render.c, census_shot_record and
census_shot_write; the variable itself is read only in tile_census.c.

Refused: a call of tile_census_on outside those two function bodies, and a
getenv of OPENPETE_TILE_CENSUS outside core/diag/tile_census.c, in any C or
C++ source under core/ and games/ (generated directories and the decomp
checkouts excluded). Comments are ignored. A helper that cannot be found is
refused too, since the fence would then pass vacuously.

Run:
  python3 scripts/fences/tile_census_accessor.py tree [root]   every source (build target)
  python3 scripts/fences/tile_census_accessor.py hook          the staged sources, read from the index
Exit: 0 clean, 66 a refused call or read, 67 a helper is missing.

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import os
import re
import subprocess
import sys

ACCESSOR = re.compile(r"\btile_census_on\s*\(")
GETENV = re.compile(r'getenv\s*\(\s*"OPENPETE_TILE_CENSUS"')
DECL = re.compile(r"\bint\s+tile_census_on\s*\(\s*void\s*\)")
HELPERS = ("census_shot_record", "census_shot_write")
HELPER_FILE = "core/renderer/native_render.c"
READER_FILE = "core/diag/tile_census.c"
EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")


def strip_comments(text):
    """Blank out comments and string literals other than the getenv name, keeping line breaks."""
    def keep_lines(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", keep_lines, text, flags=re.S)
    return re.sub(r"//[^\n]*", keep_lines, text)


def helper_spans(text):
    """{helper: (start, end)} character spans of the two helper bodies."""
    spans = {}
    for h in HELPERS:
        m = re.search(r"\bstatic\s+void\s+%s\s*\([^)]*\)(?:\s*OP_\w+\(\w*\))*\s*\{" % h, text)
        if not m:
            continue
        depth, i = 1, m.end()
        while i < len(text) and depth:
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        spans[h] = (m.start(), i)
    return spans


def scan(path, text):
    """Refusal lines of one source text; a missing helper raises LookupError."""
    code = strip_comments(text)
    out = []
    spans = helper_spans(code) if path == HELPER_FILE else {}
    if path == HELPER_FILE and len(spans) != len(HELPERS):
        raise LookupError("%s: helper(s) %s not found" % (path, sorted(set(HELPERS) - set(spans))))
    for m in ACCESSOR.finditer(code):
        line = code.count("\n", 0, m.start()) + 1
        if DECL.match(code, code.rfind("\n", 0, m.start()) + 1) or \
                DECL.search(code[code.rfind("\n", 0, m.start()) + 1:m.end()]):
            continue
        if any(a <= m.start() < b for a, b in spans.values()):
            continue
        out.append("%s:%d: tile_census_on() outside %s" % (path, line, " and ".join(HELPERS)))
    if path != READER_FILE:
        for m in GETENV.finditer(code):
            out.append("%s:%d: OPENPETE_TILE_CENSUS read outside %s" % (
                path, code.count("\n", 0, m.start()) + 1, READER_FILE))
    return out


def in_scope(rel):
    """Whether a repo-relative path is a scanned source."""
    if not rel.endswith(EXTS) or not rel.startswith(("core/", "games/")):
        return False
    parts = rel.split("/")
    if "generated" in parts:
        return False
    return not (len(parts) > 2 and parts[0] == "games" and parts[2] == parts[1])


def tree_sources(root):
    """(rel, text) of every in-scope source under root."""
    for top in ("core", "games"):
        for d, dirs, files in os.walk(os.path.join(root, top)):
            rel_d = os.path.relpath(d, root)
            dirs[:] = [x for x in dirs if in_scope(os.path.join(rel_d, x, "x.c"))]
            for f in files:
                rel = os.path.join(rel_d, f)
                if in_scope(rel):
                    with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as fh:
                        yield rel, fh.read()


def staged_sources(root):
    """(rel, text) of every staged in-scope source, read from the index, plus the helper file."""
    names = subprocess.run(["git", "-C", root, "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
                           capture_output=True, text=True, check=True).stdout.split()
    want = [n for n in names if in_scope(n)]
    if want and HELPER_FILE not in want:
        want.append(HELPER_FILE)
    for rel in want:
        p = subprocess.run(["git", "-C", root, "show", ":" + rel], capture_output=True, text=True)
        if p.returncode == 0:
            yield rel, p.stdout


def main(argv):
    """Run the fence in tree or hook mode."""
    mode = argv[1] if len(argv) > 1 else "tree"
    root = os.path.abspath(argv[2]) if len(argv) > 2 else \
        os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    srcs = tree_sources(root) if mode == "tree" else staged_sources(root)
    refused, n, saw_helper = [], 0, False
    try:
        for rel, text in srcs:
            n += 1
            saw_helper |= rel == HELPER_FILE
            refused += scan(rel, text)
    except LookupError as e:
        print("tile-census-accessor: MISSING %s" % e)
        return 67
    if mode == "tree" and not saw_helper:
        print("tile-census-accessor: MISSING %s" % HELPER_FILE)
        return 67
    for r in refused:
        print("tile-census-accessor: REFUSED %s" % r)
    print("tile-census-accessor: %s (%d source(s) scanned)" % ("FAIL" if refused else "PASS", n))
    return 66 if refused else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
