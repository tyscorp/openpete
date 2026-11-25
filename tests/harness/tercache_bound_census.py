#!/usr/bin/env python3
"""tercache_bound_census.py LOG...: checks the terrain cache's arena estimate
against the exact bake on every `[tc-bound]` line (OPENPETE_TC_BOUND_CENSUS).

A line carries three per-arena columns in openpete_terrain_cache.h's arena
order: est (the content bound, one copy), exact (a dry bake of every sector
of the table) and nosplit (the same bake without the marked-edge split,
not checked). The estimate bounds the bake when est >= exact on every arena; the CN family
takes one figure (cell_r, on CN_OPAQUE) for its four arenas, so it is
checked summed. Prints one row per arena with its smallest est - exact over
the lines, then the verdict.

Exit 0 PASS, 1 FAIL (an arena with est < exact, or a line whose columns do
not parse), 2 VACUOUS (no line, or every exact column 0).
"""
import re
import sys

CLASSES = ["OPAQUE", "ALPHA", "ADD", "SUB", "LP"]
ARENAS = (CLASSES + ["A_" + c for c in CLASSES] + ["N_" + c for c in CLASSES]
          + ["CN_" + c for c in CLASSES[:4]] + ["CA_" + c for c in CLASSES[:4]])
CN = [i for i, n in enumerate(ARENAS) if n.startswith("CN_")]
LINE = re.compile(r"\[tc-bound\] f=(\d+) level=(-?\d+) .*?fails=(\d+) .*?"
                  r"exact=([\d,]+) est=([\d,]+)")


def groups(v):
    """Per-arena figures with the CN family folded into one row."""
    g = {n: v[i] for i, n in enumerate(ARENAS) if i not in CN}
    g["CN"] = sum(v[i] for i in CN)
    return g


def main(paths):
    rows, bad, engaged, fails = 0, [], 0, 0
    worst = {}   # group -> (est - exact, est, exact, level, f)
    for p in paths:
        with open(p, errors="replace") as fh:
            for line in fh:
                m = LINE.search(line)
                if not m:
                    continue
                rows += 1
                f, lvl, nf = m.group(1), m.group(2), int(m.group(3))
                fails += nf
                ex, es = ([int(x) for x in m.group(k).split(",")] for k in (4, 5))
                if len(ex) != len(ARENAS) or len(es) != len(ARENAS):
                    bad.append("%s f=%s: %d/%d arenas, want %d" % (p, f, len(ex), len(es), len(ARENAS)))
                    continue
                engaged += any(ex)
                ge, gs = groups(ex), groups(es)
                for n in ge:
                    if ge[n] == 0:
                        continue
                    d = gs[n] - ge[n]
                    if n not in worst or d < worst[n][0]:
                        worst[n] = (d, gs[n], ge[n], lvl, f)
                    if d < 0:
                        bad.append("level=%s f=%s %s est=%d < exact=%d" % (lvl, f, n, gs[n], ge[n]))
    for n, (d, e, x, lvl, f) in sorted(worst.items(), key=lambda t: t[1][0]):
        print("  %-9s min est-exact %8d (est=%d exact=%d level=%s f=%s)" % (n, d, e, x, lvl, f))
    print("  lines=%d engaged=%d bake_fails=%d" % (rows, engaged, fails))
    for b in bad[:20]:
        print("  FAIL " + b)
    if rows == 0 or engaged == 0:
        print("tercache-bound: VACUOUS (no [tc-bound] line with a nonzero bake)")
        return 2
    if bad:
        print("tercache-bound: FAIL (%d arena rows with est < exact)" % len(bad))
        return 1
    print("tercache-bound: PASS")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1:]))
