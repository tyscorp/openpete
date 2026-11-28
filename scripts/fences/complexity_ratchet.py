#!/usr/bin/env python3
"""Fence: cognitive-complexity ratchet.

Every function over CAP cognitive complexity (clang-tidy 14,
readability-function-cognitive-complexity, IgnoreMacros) carries a row in
baselines/complexity/cognitive.tsv. A function's excess is
max(score, CAP) - CAP. A commit passes when, over the keys of the files it
stages, the sum of excess after is at most the sum before: a row never
rises unfunded, a split passes, a new function over the cap is funded only
by a reduction in the same commit.

Modes:
  hook              git pre-commit (githooks/pre-commit). Scores the working
                    tree, decides over the index, writes and stages the
                    baseline on PASS. Exit 1 aborts the commit. A missing
                    linux-release database is configured, and headers a
                    build rule generates that the -M -MG pass finds absent
                    (over every TU when a header is acted on, else the
                    staged sources) are built (openpete_generated_headers,
                    nice -n 10) before scoring; OPENPETE_FENCE_NO_BUILD=1
                    refuses MISSING-INPUT instead.
  check             every build. The same inequality over TUs clean against
                    HEAD, against HEAD's baseline. Exit 1 on a rise or an
                    UNWIRED row; 64 = SKIP (no compile database) under --ctest.
                    A file dirty against HEAD is not read; the PASS line
                    names each one (the hook decides them on commit).
  check --armed     exit 1 unless the resolved pre-commit hook is the tracked
                    githooks/pre-commit, executable, calling this fence, and
                    the baselines' merge driver (ratchet_tsv_merge.py) is
                    configured.
  tighten [--accept <key> <reason>]...
                    full-corpus rewrite, run on demand: tightenings, retirements,
                    admissions; a rise is refused unless named by --accept.
  baseline          recapture the whole file (tool-version change only).

Escapes:
  // complexity-ratchet: <reason>        on the definition line or the line
                                         above: the function leaves the
                                         balance; its score is still recorded.
  OPENPETE_COMPLEXITY_ACCEPT="<reason>"  one-time accepted rise; the reason
                                         lands in the row's `accepted` column.
Environment:
  OPENPETE_COMPLEXITY_DB   compile database path (default
                           build/linux-release/compile_commands.json).
  OPENPETE_NO_COMMITS=1    a machine that builds and never commits passes
                           `check --armed` unarmed.
Configuration:
  OPENPETE_GAMES is read from the CMakeCache.txt beside the database (none:
  the full default). Under a partial list, paths under games/<g>/ for an
  unconfigured <g> and under games/common/ are UNCONFIGURED: their rows are
  held unchanged, their sources need no entry, the hook refuses a staged
  path there, and tighten and baseline refuse to run.
"""
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys

import ratchet_lib as lib
import ratchet_tsv_merge as tsv_merge
from ratchet_lib import (Refusal, say, git, is_header, key_file, entry_flags,
                         head_sha, print_bins, wired, UNSCORED,
                         CONFIGURE, HOOK_REL, ARM)

CAP = 25
TOOL_MAJOR = 14
CHECK = "readability-function-cognitive-complexity"
CHECKS_ARG = "-checks=-*," + CHECK
CONFIG_ARG = ("-config={CheckOptions: ["
              "{key: %s.Threshold, value: 0}, "
              "{key: %s.IgnoreMacros, value: true}, "
              "{key: %s.DescribeBasicIncrements, value: true}]}"
              % (CHECK, CHECK, CHECK))
OPTIONS_LINE = "Threshold=0 IgnoreMacros=true header-filter=.* cap=%d" % CAP
JOBS = 16

TSV = "baselines/complexity/cognitive.tsv"
MARKER = re.compile(r"//\s*complexity-ratchet:\s*(\S.*)")

SCOPE_ROOTS = ("core/", "games/", "tests/")
FENCE = lib.Fence("complexity-ratchet", "complexity_ratchet.py", TSV,
                  SCOPE_ROOTS, "OPENPETE_COMPLEXITY_DB")
FIXTURE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "tests", "complexity_fixture")
in_scope = FENCE.in_scope


def excess(score):
    return max(score, CAP) - CAP


# --------------------------------------------------------------------------
# The decision. Pure: every rule is a function of these inputs.
#
#   before_rows    {key: (score, accepted_reason)}   the baseline source
#   after_scores   {key: score}                      the scored working tree
#   acted_on       set of repo paths (new names, deleted names); None = all
#   renames        {old_path: (new_path, identical_blob)}
#   extents        {key: (first_line, last_line)}    for keys under suspicion
#   changed_ranges {path: [(first, last), ...]}      the commit's touched lines
#   markers        {key: reason}
#   wired_files    paths with a compile-database entry (headers: all present)
#   present_files  paths on disk
#   accept_reason  str (every refused rise) | {key: reason} (named rises) | None
#   unwired_all    refuse an UNWIRED row outside the acted-on set too
#
# Returns (verdict, new_rows, bins); verdict is "PASS" or "REFUSED"; bins maps
# a bin name to a list of (key, before_score, after_score).
# --------------------------------------------------------------------------
def decide(before_rows, after_scores, acted_on, renames, extents,
           changed_ranges, markers, wired_files, present_files,
           accept_reason, unwired_all=False):
    rows = {}
    identical_targets = set()
    for old, (new, identical) in renames.items():
        if identical:
            identical_targets.add(new)
    for key, val in before_rows.items():
        f, name = key.rsplit(":", 1)
        if f in renames:
            key = renames[f][0] + ":" + name
        rows[key] = val
    after = dict(after_scores)
    # An identical rename is not scored: the score is a function of content
    # and flags, both unchanged, and the new path has no database entry yet.
    for key, (score, _) in rows.items():
        if key_file(key) in identical_targets:
            after[key] = score

    bins = {b: [] for b in ("RISE", "ACCEPTED", "INHERITED", "MARKED",
                            "TIGHTENED", "RETIRED", "ADMITTED", "UNWIRED")}

    def acted(f):
        return acted_on is None or f in acted_on

    unwired = set()
    for key, (score, _) in rows.items():
        f = key_file(key)
        if (f in present_files and f not in wired_files
                and f not in identical_targets and not is_header(f)
                and (unwired_all or acted(f))):
            unwired.add(key)
            bins["UNWIRED"].append((key, score, None))

    def touched(key):
        if key not in extents:
            return True
        lo, hi = extents[key]
        return any(a <= hi and b >= lo
                   for a, b in changed_ranges.get(key_file(key), ()))

    keys = sorted(k for k in set(rows) | set(after)
                  if acted(key_file(k)) and k not in unwired)
    sum_before = sum_after = 0
    risers, inherited = [], set()
    for key in keys:
        if key in markers:
            continue
        row = rows[key][0] if key in rows else None
        score = after.get(key, 0)
        if score > (row if row is not None else CAP) and not touched(key):
            inherited.add(key)
            bins["INHERITED"].append((key, row, score))
            continue
        b, a = (row - CAP if row is not None else 0), excess(score)
        sum_before += b
        sum_after += a
        if a > b:
            risers.append((key, row, score, a - b))

    accepted = {}
    refused = []
    if sum_after > sum_before:
        refused = risers
        if accept_reason:
            remaining = []
            for r in risers:
                reason = (accept_reason if isinstance(accept_reason, str)
                          else accept_reason.get(r[0]))
                if reason:
                    accepted[r[0]] = reason
                    sum_after -= r[3]
                else:
                    remaining.append(r)
            refused = remaining if sum_after > sum_before else []
    for key, row, score, _ in risers:
        if key in accepted:
            bins["ACCEPTED"].append((key, row, score))
    refused_keys = {r[0] for r in refused}
    for key, row, score, _ in refused:
        bins["RISE"].append((key, row, score))

    new_rows = dict(rows)
    for key in keys:
        if key in inherited or key in refused_keys:
            continue
        row = rows.get(key)
        score = after.get(key, 0)
        if key in markers:
            bins["MARKED"].append((key, row[0] if row else None, score))
        if score > CAP:
            if key in accepted:
                reason = accepted[key]
            elif row is not None and score <= row[0]:
                reason = row[1]
            else:
                reason = ""
            new_rows[key] = (score, reason)
            if key not in markers and key not in accepted:
                if row is None:
                    bins["ADMITTED"].append((key, None, score))
                elif score < row[0]:
                    bins["TIGHTENED"].append((key, row[0], score))
        elif row is not None:
            del new_rows[key]
            bins["RETIRED"].append((key, row[0], score))

    verdict = "REFUSED" if (refused or unwired) else "PASS"
    return verdict, new_rows, bins


# --------------------------------------------------------------------------
# Baseline file
# --------------------------------------------------------------------------
def parse_tsv(text):
    """-> (header dict, {key: (score, accepted)})"""
    return lib.parse_header(text), {
        c[0] + ":" + c[1]: (int(c[2]), c[4]) for c in lib.parse_rows(text, 5)}

def marked_keys(text):
    return {c[0] + ":" + c[1] for c in lib.parse_rows(text, 5) if c[3] == "1"}

def render_tsv(rows, markers, written_by, head_sha):
    return lib.render(
        "cognitive-complexity ratchet baseline (scripts/fences/complexity_ratchet.py)",
        [("tool", "clang-tidy %d" % TOOL_MAJOR), ("options", OPTIONS_LINE)],
        ["file", "function", "score", "marked", "accepted"],
        [(key, ["%d" % score, "1" if key in markers else "0", reason])
         for key, (score, reason) in rows.items()],
        written_by, head_sha)

# --------------------------------------------------------------------------
# Scoring
# --------------------------------------------------------------------------
def find_tool():
    for name in ("clang-tidy-%d" % TOOL_MAJOR, "clang-tidy"):
        path = shutil.which(name)
        if not path:
            continue
        out = subprocess.run([path, "--version"], capture_output=True,
                             text=True).stdout
        m = re.search(r"version (\d+)\.", out)
        if m and int(m.group(1)) == TOOL_MAJOR:
            return path
        found = m.group(1) if m else "unknown"
        if name == "clang-tidy":
            raise Refusal(
                "clang-tidy major version %s, the baseline is pinned to %d. "
                "Install clang-tidy-%d, or recapture under the new version: "
                "set TOOL_MAJOR and run `complexity_ratchet.py baseline`."
                % (found, TOOL_MAJOR, TOOL_MAJOR))
    raise Refusal("clang-tidy-%d not found. Install: sudo apt install "
                  "clang-tidy-%d clang-%d" % ((TOOL_MAJOR,) * 3))


def verify_config(tool):
    out = subprocess.run([tool, CHECKS_ARG, CONFIG_ARG, "--dump-config"],
                         capture_output=True, text=True, cwd="/").stdout
    ok = (re.search(r"^Checks:\s*'?(clang-diagnostic-\*,clang-analyzer-\*,)?-\*,%s'?\s*$"
                    % re.escape(CHECK), out, re.M)
          and re.search(r"IgnoreMacros\s*\n?\s*value:\s*'?true", out)
          and re.search(r"Threshold\s*\n?\s*value:\s*'?0'?\s*$", out, re.M))
    if not ok:
        raise Refusal("clang-tidy --dump-config does not show the pinned "
                      "check list and options; refusing to score.")


def load_db(root):
    return lib.load_db(root, FENCE)

DIAG = re.compile(r"^(.+?):(\d+):\d+: (warning|error|note): (.*)$")
FUNC = re.compile(r"function '(.+?)' has cognitive complexity of (\d+)")
INCR = re.compile(r"\+(\d+)(?:, including nesting penalty of \d+, "
                  r"nesting level increased to (\d+))?")


def run_tidy(tool, src, flags, cwd, root):
    """-> (functions [(relfile, line, name, score, increments)], errors)"""
    p = subprocess.run(
        [tool, CHECKS_ARG, CONFIG_ARG, "-header-filter=.*", "--quiet", src,
         "--"] + flags, capture_output=True, text=True, cwd=cwd)
    funcs, errors, cur = [], [], None
    for line in p.stdout.splitlines():
        m = DIAG.match(line)
        if not m:
            continue
        path, lineno, kind, msg = m.group(1), int(m.group(2)), m.group(3), m.group(4)
        if kind == "error":
            errors.append(line)
            cur = None
            continue
        rel = os.path.relpath(os.path.normpath(os.path.join(cwd, path)), root)
        if kind == "warning":
            fm = FUNC.search(msg)
            cur = None
            if fm and CHECK in msg and fm.group(1) != "operator()":
                cur = (rel, lineno, fm.group(1), int(fm.group(2)), [])
                funcs.append(cur)
        elif cur is not None:
            im = INCR.match(msg)
            if im:
                cur[4].append((lineno, int(im.group(1)), int(im.group(2) or 0)))
    if p.returncode != 0 and not errors and not funcs:
        errors.append((p.stderr.strip().splitlines() or ["clang-tidy exit %d"
                                                         % p.returncode])[-1])
    return funcs, errors


class Corpus:
    def __init__(self):
        self.scores = {}      # key -> score
        self.where = {}       # key -> (relfile, line, name)
        self.incr = {}        # key -> [(line, amount, nesting)]
        self.tu_of = {}       # key -> (tu relpath, entry)
        self.errors = {}      # tu relpath -> [error line]
        self.scored = 0       # TUs run
        self.diagnostics = 0  # cognitive-complexity diagnostics seen


def score_tus(tool, root, jobs, scope=in_scope):
    """jobs: [(tu_rel, src_abs, flags, cwd, entry)] -> Corpus. Functions are
    keyed per (file, line, name) at the maximum score across runs, then a
    repeated name within one file takes an ordinal by line order."""
    corpus = Corpus()
    sites = {}
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        futs = {pool.submit(run_tidy, tool, j[1], j[2], j[3], root): j
                for j in jobs}
        for fut in concurrent.futures.as_completed(futs):
            tu_rel, _, _, _, entry = futs[fut]
            funcs, errors = fut.result()
            corpus.scored += 1
            if errors:
                corpus.errors.setdefault(tu_rel, []).extend(errors)
            for rel, line, name, score, incr in funcs:
                corpus.diagnostics += 1
                if rel.startswith("..") or not scope(rel):
                    continue
                site = (rel, line, name)
                if site not in sites or score > sites[site][0]:
                    sites[site] = (score, incr, tu_rel, entry)
    by_name = {}
    for (rel, line, name) in sites:
        by_name.setdefault((rel, name), []).append(line)
    for (rel, name), lines in by_name.items():
        for n, line in enumerate(sorted(lines)):
            key = "%s:%s" % (rel, name if n == 0 else "%s#%d" % (name, n + 1))
            score, incr, tu_rel, entry = sites[(rel, line, name)]
            corpus.scores[key] = score
            corpus.where[key] = (rel, line, name)
            corpus.incr[key] = incr
            corpus.tu_of[key] = (tu_rel, entry)
    return corpus


def find_markers(root, corpus):
    markers, cache = {}, {}
    for key, (rel, line, _) in corpus.where.items():
        if rel not in cache:
            try:
                with open(os.path.join(root, rel), errors="replace") as fh:
                    cache[rel] = fh.read().split("\n")
            except OSError:
                cache[rel] = []
        src = cache[rel]
        for n in (line - 1, line - 2):
            if 0 <= n < len(src):
                m = MARKER.search(src[n])
                if m:
                    markers[key] = m.group(1).strip()
                    break
    return markers


def function_extent(root, corpus, key):
    """(first, last) line of the function's definition, from clang's AST."""
    clang = shutil.which("clang-%d" % TOOL_MAJOR)
    if not clang or key not in corpus.where:
        return None
    rel, line, name = corpus.where[key]
    tu_rel, entry = corpus.tu_of[key]
    flags, cwd = (entry_flags(entry), entry["directory"]) if entry else (["-std=c11"], root)
    p = subprocess.run(
        [clang, "-fsyntax-only", "-Xclang", "-ast-dump=json", "-Xclang",
         "-ast-dump-filter=" + name, os.path.join(root, tu_rel)] + flags,
        capture_output=True, text=True, cwd=cwd)
    dec, text, pos = json.JSONDecoder(), p.stdout, 0
    while True:
        while pos < len(text) and text[pos] in " \r\n\t":
            pos += 1
        if pos >= len(text):
            return None
        try:
            node, pos = dec.raw_decode(text, pos)
        except ValueError:
            return None
        if (node.get("kind") in ("FunctionDecl", "CXXMethodDecl")
                and node.get("name") == name
                and any(c.get("kind") == "CompoundStmt"
                        for c in node.get("inner", ()))):
            loc = node.get("loc", {})
            loc = loc.get("expansionLoc", loc)
            if loc.get("line") not in (None, line):
                continue
            rng = node.get("range", {})
            begin = rng.get("begin", {}).get("line", line)
            end = rng.get("end", {}).get("line")
            if end is None:
                end = begin
            return (min(begin, line), max(end, line))


# --------------------------------------------------------------------------
# Negative controls: five fixtures, each with its own TU and rows, evaluated
# before every hook and check verdict.
# --------------------------------------------------------------------------
def run_controls(tool):
    with open(os.path.join(FIXTURE_DIR, "controls.json")) as fh:
        controls = json.load(fh)
    jobs = []
    for c in controls:
        for tu in c["tus"]:
            jobs.append((tu, os.path.join(FIXTURE_DIR, tu), ["-std=c11"],
                         FIXTURE_DIR, None))
    corpus = score_tus(tool, FIXTURE_DIR, jobs, scope=lambda rel: True)
    if corpus.errors or not corpus.scores:
        raise Refusal("negative controls: fixture TUs did not score clean: %s"
                      % (corpus.errors or "no diagnostics"))
    markers = find_markers(FIXTURE_DIR, corpus)
    failures = []
    for c in controls:
        after = {k: v for k, v in corpus.scores.items() if key_file(k) in c["tus"]}
        for k, want in c["scores"].items():
            if after.get(k) != want:
                failures.append("%s: %s scored %s, fixture expects %d"
                                % (c["name"], k, after.get(k), want))
        rows = {k: (v, "") for k, v in c["rows"].items()}
        verdict, _, bins = decide(
            rows, after, set(c["tus"]) | set(c.get("present", [])), {}, {}, {},
            {k: v for k, v in markers.items() if k in after},
            set(c["tus"]), set(c["tus"]) | set(c.get("present", [])), None)
        if verdict != c["verdict"] or (c.get("bin") and not bins[c["bin"]]):
            failures.append("%s: verdict %s, expected %s%s" % (
                c["name"], verdict, c["verdict"],
                " with bin " + c["bin"] if c.get("bin") else ""))
    if failures:
        raise Refusal("negative controls FAILED, no verdict is trusted:\n  "
                      + "\n  ".join(failures))
    return len(controls)


# --------------------------------------------------------------------------
# Shared corpus run
# --------------------------------------------------------------------------
def score_corpus(tool, root, entries, moved):
    """Score every in-scope database entry whose file exists. -> (corpus,
    bins) with MOVED (old path of a staged rename) and STALE-DB (absent
    file) entries skipped."""
    jobs, bins = [], {"MOVED": [], "STALE-DB": []}
    for rel, es in sorted(entries.items()):
        if not os.path.isfile(os.path.join(root, rel)):
            bins["MOVED" if rel in moved else "STALE-DB"].append(rel)
            continue
        for e in es:
            jobs.append((rel, os.path.join(root, rel), entry_flags(e),
                         e["directory"], e))
    corpus = score_tus(tool, root, jobs)
    if corpus.scored == 0 or corpus.diagnostics == 0:
        raise Refusal("VACUOUS: %d TUs scored, %d cognitive-complexity "
                      "diagnostics. No verdict." % (corpus.scored,
                                                    corpus.diagnostics))
    return corpus, bins


def check_unscored(root, entries, complete=False):
    return lib.check_unscored(root, entries, FENCE, complete)

def drop_unconfigured(scores):
    """Scores minus the keys whose path is out of configuration."""
    return {k: v for k, v in scores.items()
            if not FENCE.config.out_of_config(key_file(k))}

def present_scope_files(root):
    return lib.present_scope_files(root, FENCE)

def head_tsv(root):
    return lib.head_tsv(root, TSV)

REFUSAL_OPTIONS = """
How to land this commit. The refactor is the end-state design, never the
smallest change that clears the number. Read every caller and every sibling
of the code being moved; choose the abstraction that its usages, the
surrounding subsystem and future maintenance call for; build that whole.
Refused by review even when the fence passes: a helper cut at an arbitrary
line, a function named for its position (`_part2`, `_inner`), a parameter
struct that exists only to carry locals across the cut, a wrapper that hides
a branch without removing it, a second copy of logic that already exists.
When the right abstraction is larger than this commit, land it first as its
own no-behaviour-change commit, then land the feature on top of it.

  1. de-nest   the new block sits at depth; an early return, a guard
               clause, or a table lookup clears the rise when the nesting
               itself is the defect (nesting is what cognitive complexity
               charges).
  2. extract   move a block into a static function in this TU when the
               block is one operation with a name, a contract and inputs
               the callers would recognize. The commit passes while its
               total excess over %(cap)d does not rise; a new function over
               %(cap)d is funded by the reduction it came from.
  3. split     break the function along its phase boundaries into several
               functions, each owning one phase and its state; the same
               balance admits them, and each piece's nesting increments
               start at depth 0, so the pieces total below the original by
               the nesting charge.
  4. lift      if the new branch is `if (knob)`, decide the knob at the
               caller or in a dispatch table so this function sees one
               path.
  5. accept    OPENPETE_COMPLEXITY_ACCEPT="<reason>" git commit ...
               For a rise that is the end-state design. Never a deferral of
               the refactor: a deferred refactor is done now, in its own
               commit.
  6. exempt    `// complexity-ratchet: <reason>` on the definition line
               or the line above it. For table-shaped functions (flat
               flag chains); the score is still recorded, and removing
               the marker reinstates the ratchet at the then-current
               score.
  7. fence     if the score is a mis-count (macro, generated block), the
               fence is the defect: fix or retire it in the same commit and say
               so in its message.
""" % {"cap": CAP}


def print_refusal(root, corpus, bins, added):
    for key, row, score in bins["RISE"]:
        base = row if row is not None else CAP
        say("complexity-ratchet: REFUSED  %s  %s -> %d (+%d over %s)" % (
            key, row if row is not None else "no row", score, score - base,
            "its row" if row is not None else "the cap %d" % CAP))
        incr = corpus.incr.get(key, [])
        rel = key_file(key)
        ranges = added.get(rel, ())
        hits = [i for i in incr if any(a <= i[0] <= b for a, b in ranges)]
        label = "increments on changed lines (%d of %d):" % (len(hits), len(incr))
        if not hits:
            hits = sorted(incr, key=lambda i: (-i[2], -i[1], i[0]))[:5]
            label = ("no increment on a changed line; top increments by "
                     "nesting level (%d of %d):" % (len(hits), len(incr)))
        say("  " + label)
        try:
            with open(os.path.join(root, rel), errors="replace") as fh:
                src = fh.read().split("\n")
        except OSError:
            src = []
        for line, amount, nest in hits[:20]:
            text = src[line - 1].strip()[:60] if 0 < line <= len(src) else ""
            say("    %s:%d  +%d  nesting level %d, `%s`" % (
                os.path.basename(rel), line, amount, nest, text))
        if len(hits) > 20:
            say("    ... %d more" % (len(hits) - 20))
    if bins["UNWIRED"]:
        say("complexity-ratchet: REFUSED  UNWIRED rows (the file is on disk "
            "and no target compiles it):")
        for key, row, _ in bins["UNWIRED"]:
            say("    %s  row %d" % (key, row))
        say("  Wire the file into CMake and re-run `%s`, or delete it." % CONFIGURE)
    if bins["RISE"]:
        say(REFUSAL_OPTIONS)


# --------------------------------------------------------------------------
# hook
# --------------------------------------------------------------------------
def diff_ranges(root, base, paths):
    """-> (touched, added): {path: [(first, last)]} in new-file line numbers.
    touched includes the two lines around a pure deletion (the complexity
    fence's touched rule: it feeds only the INHERITED bin)."""
    hunks = lib.diff_hunks(root, base, paths)
    touched = {p: [(s, s + c - 1) if c else (s, s + 1) for s, c in hs]
               for p, hs in hunks.items()}
    return touched, lib.added_ranges(hunks)

def validate_source(root, head, rows, corpus_scores):
    """The staged TSV, written by tighten or baseline, against HEAD."""
    sha = head_sha(root)
    htext = head_tsv(root)
    hhead, hrows = parse_tsv(htext) if htext is not None else ({}, {})
    by = head.get("written-by")
    if by == "baseline":
        if htext is not None and hhead.get("tool") == head.get("tool"):
            raise Refusal("staged %s is a `baseline` recapture, and HEAD already "
                          "carries a baseline under the same tool version. Use "
                          "`tighten`." % TSV)
        return
    if head.get("head") != sha:
        raise Refusal("staged %s was computed against %s, HEAD is %s. Re-run "
                      "`complexity_ratchet.py tighten`, stage it, commit."
                      % (TSV, head.get("head", "?")[:10], sha[:10]))
    for key, (score, reason) in rows.items():
        if reason:
            continue
        if key in hrows and score > hrows[key][0]:
            raise Refusal("staged %s raises %s (%d -> %d) with no `accepted` "
                          "reason: a hand edit. Use `tighten --accept`." % (
                              TSV, key, hrows[key][0], score))
        if key not in hrows and key in corpus_scores and score > corpus_scores[key]:
            raise Refusal("staged %s adds %s at %d, the function scores %d: a "
                          "hand edit." % (TSV, key, score, corpus_scores[key]))


def mode_hook(root):
    lib.refuse_temporary_index()
    changes, base = lib.staged_changes(root)
    scope = lib.scope_changes(changes, in_scope)
    staged_text = lib.staged_tsv(root, TSV, changes)
    shead, srows = parse_tsv(staged_text) if staged_text is not None else ({}, {})

    if not scope:
        if not lib.reset_stale_hook_tsv(root, FENCE, staged_text) \
                and staged_text is not None:
            validate_source(root, shead, srows, {})
        return 0

    tool = find_tool()
    verify_config(tool)
    lib.linux_db(root, FENCE)
    db_path, entries = load_db(root)
    if entries is None:
        raise Refusal("no compile database at %s. Run: %s" % (db_path, CONFIGURE))
    lib.refuse_unconfigured(FENCE, [p for _, a, b in scope for p in (a, b)])
    controls = run_controls(tool)
    check_unscored(root, entries)

    renames, acted_on, need_entry, moved = lib.acted_on_paths(root, base, scope)
    # Build-produced inputs are built before scoring: a staged source that
    # does not parse refuses below, and an includer that does not parse
    # holds its headers' rows unscored. The whole corpus is probed when a
    # header is acted on, the staged sources otherwise.
    if any(is_header(f) for f in acted_on):
        probe = None
    else:
        probe = {rel for rel in need_entry if rel in entries}
    if probe is None or probe:
        say("complexity-ratchet: %s" % lib.ensure_db_inputs(root, FENCE, db_path,
                                                            probe, JOBS))

    corpus, cbins = score_corpus(tool, root, entries, moved)

    for rel in need_entry:
        if is_header(rel) or (rel in UNSCORED and rel not in entries):
            continue
        if rel not in entries:
            raise Refusal("%s has no exact compile-database entry (clang-tidy "
                          "would borrow a neighbour's flags). Run: %s"
                          % (rel, CONFIGURE))
        if rel in corpus.errors:
            raise Refusal("%s does not parse clean; functions after an error "
                          "are dropped and would read as retirements:\n  %s"
                          % (rel, "\n  ".join(corpus.errors[rel][:5])))
    dirty = lib.unstaged_refusal(root, acted_on, TSV)
    dirty_headers = sorted(f for f in dirty if f and in_scope(f) and is_header(f))

    # Baseline source: a staged tighten/baseline file, else HEAD's; never the
    # working copy, never a staged hook-written file.
    if staged_text is not None and shead.get("written-by") in ("tighten", "baseline"):
        validate_source(root, shead, srows, corpus.scores)
        source_name, source_text, before = "staged (%s)" % shead["written-by"], staged_text, srows
    else:
        htext = head_tsv(root)
        if htext is None:
            raise Refusal("no baseline in HEAD or the index. Run "
                          "`complexity_ratchet.py baseline`, stage %s." % TSV)
        source_name, source_text, before = "HEAD", htext, parse_tsv(htext)[1]
        if staged_text is not None:
            source_name += " (staged hook-written file discarded)"
    # Rows out of configuration leave the decision and return unchanged.
    before_all = before
    before, held = lib.split_unconfigured(FENCE.config, before_all)

    # A header key reported through an includer that failed to parse is
    # unknown, not retired.
    after = drop_unconfigured(corpus.scores)
    if corpus.errors:
        for key, (score, _) in before.items():
            if is_header(key_file(key)) and key not in after:
                after[key] = score

    markers = find_markers(root, corpus)
    present = present_scope_files(root) | {f for f in acted_on
                                           if os.path.isfile(os.path.join(root, f))}
    touched, added = diff_ranges(root, base, sorted(
        f for f in acted_on if os.path.isfile(os.path.join(root, f))))
    accept = lib.accept_reason("OPENPETE_COMPLEXITY_ACCEPT")

    args = (before, after, acted_on, renames)
    rest = (touched, markers, wired(entries, present), present)
    verdict, new_rows, bins = decide(*args, {}, *rest, accept)
    if bins["RISE"] or bins["ACCEPTED"]:
        extents = {}
        for key, _, _ in bins["RISE"] + bins["ACCEPTED"]:
            ext = function_extent(root, corpus, key)
            if ext:
                extents[key] = ext
        verdict, new_rows, bins = decide(*args, extents, *rest, accept)

    new_rows.update(held)
    bins.update(cbins)
    bins["DIRTY-HEADER"] = dirty_headers
    lib.report_unconfigured(FENCE, [], held)
    say("complexity-ratchet: %d TUs scored, %d functions, %d controls; baseline "
        "source: %s; %d markers, %d accepted rows" % (
            corpus.scored, len(corpus.scores), controls, source_name,
            len(markers), sum(1 for v in new_rows.values() if v[1])))
    if bins["INHERITED"]:
        say("  INHERITED rises (not from this commit; repair: "
            "`complexity_ratchet.py tighten --accept <key> \"<reason>\"`):")
    print_bins({k: v for k, v in bins.items() if k not in ("RISE", "UNWIRED")},
               quiet=("MARKED",))
    if verdict == "REFUSED":
        print_refusal(root, corpus, bins, added)
        return 1
    if accept:
        for key, row, score in bins["ACCEPTED"]:
            say("complexity-ratchet: ACCEPTED  %s  %s -> %d  (%s)" % (
                key, row if row is not None else "no row", score, accept))

    # Held rows keep their marked column as the source file records it.
    marked = {k for k in new_rows if k in markers and k not in held} | (
        marked_keys(source_text) & set(held))
    if new_rows == before_all and marked_keys(source_text) == marked:
        text = source_text
    else:
        text = render_tsv(new_rows, marked, "hook", head_sha(root))
    lib.write_staged(root, TSV, text, staged_text)
    say("complexity-ratchet: PASS")
    return 0


# --------------------------------------------------------------------------
# check / tighten / baseline
# --------------------------------------------------------------------------
def dirty_paths(root):
    return lib.dirty_paths(root, FENCE)

def corpus_inputs(root, ctest=False, mode="check"):
    """-> (tool, entries, controls, corpus, cbins, present, dirty, clean,
    unconfigured sources). tighten and baseline refuse a partial configure."""
    db_path, entries = load_db(root)
    if entries is None:
        say("complexity-ratchet: SKIP, no compile database at %s" % db_path)
        sys.exit(64 if ctest else 0)
    if mode != "check":
        lib.refuse_partial(FENCE, mode)
    tool = find_tool()
    verify_config(tool)
    controls = run_controls(tool)
    unconfigured = check_unscored(root, entries, complete=True)
    corpus, cbins = score_corpus(tool, root, entries, set())
    present = present_scope_files(root)
    dirty = dirty_paths(root)
    clean = {f for f in present | set(entries) if f not in dirty}
    return (tool, entries, controls, corpus, cbins, present, dirty, clean,
            unconfigured)


def clean_after(corpus, before, clean_tus):
    """Scores with the keys of error TUs' headers held at their rows."""
    after = dict(corpus.scores)
    if corpus.errors:
        for key, (score, _) in before.items():
            if is_header(key_file(key)) and key not in after:
                after[key] = score
    return after


def mode_check(root, ctest):
    tool, entries, controls, corpus, cbins, present, dirty, clean, unconfigured = \
        corpus_inputs(root, ctest)
    htext = head_tsv(root)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `complexity_ratchet.py "
                      "baseline` and commit it." % TSV)
    before, held = lib.split_unconfigured(FENCE.config, parse_tsv(htext)[1])
    clean_tus = [f for f in entries if f in clean
                 and os.path.isfile(os.path.join(root, f))]
    total_tus = [f for f in entries if os.path.isfile(os.path.join(root, f))]
    acted = {f for f in clean if f not in corpus.errors}
    # A row whose file left the tree is a retirement for tighten to write.
    acted |= {key_file(k) for k in before
              if not os.path.exists(os.path.join(root, key_file(k)))}
    markers = find_markers(root, corpus)
    verdict, _, bins = decide(before, drop_unconfigured(
                                  clean_after(corpus, before, clean_tus)),
                              acted, {}, {}, {}, markers,
                              wired(entries, present), present, None,
                              unwired_all=True)
    bins.update(cbins)
    bins["DIRTY"] = sorted(f for f in dirty if in_scope(f))
    bins["ERROR-TU"] = sorted(corpus.errors)
    say("complexity-ratchet: engaged %d/%d TUs clean against HEAD, %d functions, "
        "%d controls, %d rows, %d markers, %d accepted rows" % (
            len(clean_tus), len(total_tus), len(corpus.scores), controls,
            len(before), len(markers), sum(1 for v in before.values() if v[1])))
    lib.report_unconfigured(FENCE, unconfigured, held)
    if not clean_tus:
        raise Refusal("VACUOUS: no TU is clean against HEAD.")
    quiet = ("MARKED", "DIRTY") if verdict == "PASS" else ("MARKED",)
    print_bins({k: v for k, v in bins.items() if k not in ("RISE", "UNWIRED")},
               quiet=quiet)
    if any(bins[b] for b in ("TIGHTENED", "RETIRED", "ADMITTED")):
        say("  `complexity_ratchet.py tighten` writes the TIGHTENED, RETIRED "
            "and ADMITTED bins.")
    if verdict == "REFUSED":
        print_refusal(root, corpus, bins, {})
        say("A rise reached HEAD without the hook (merge, revert, "
            "--no-verify). Repair: reduce it, or `complexity_ratchet.py "
            "tighten --accept <key> \"<reason>\"` and commit the baseline.")
        return 1
    say(check_verdict(len(clean_tus), bins["DIRTY"]))
    return 0


def check_verdict(n_clean, dirty):
    """The PASS line of `check`: the clean TUs it read, and each dirty
    in-scope file it skipped by name (at most ten), since a PASS over a
    worktree whose edits are all dirty read none of them."""
    if not dirty:
        return "complexity-ratchet: PASS over %d clean TUs" % n_clean
    names = ", ".join(dirty[:10]) + (", ... %d more" % (len(dirty) - 10) if len(dirty) > 10 else "")
    return ("complexity-ratchet: PASS over %d clean TUs; %d dirty files NOT READ (the "
            "pre-commit hook decides them): %s" % (n_clean, len(dirty), names))


def mode_tighten(root, argv):
    accept = {}
    i = 0
    while i < len(argv):
        if argv[i] == "--accept" and i + 2 < len(argv) + 0 and argv[i + 2].strip():
            accept[argv[i + 1]] = argv[i + 2].strip()
            i += 3
        else:
            raise Refusal("usage: tighten [--accept <key> \"<reason>\"]...")
    tool, entries, controls, corpus, cbins, present, dirty, clean, _ = \
        corpus_inputs(root, mode="tighten")
    htext = head_tsv(root)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `baseline`." % TSV)
    before = parse_tsv(htext)[1]
    acted = {f for f in clean if f not in corpus.errors}
    acted |= {key_file(k) for k in before
              if not os.path.exists(os.path.join(root, key_file(k)))}
    markers = find_markers(root, corpus)
    verdict, new_rows, bins = decide(
        before, clean_after(corpus, before, None), acted, {}, {}, {}, markers,
        wired(entries, present), present, accept or None, unwired_all=True)
    unknown = sorted(k for k in accept if k not in {b[0] for b in bins["ACCEPTED"]})
    bins.update(cbins)
    bins["DIRTY"] = sorted(f for f in dirty if in_scope(f))
    print_bins({k: v for k, v in bins.items() if k not in ("RISE", "UNWIRED")},
               quiet=("MARKED",))
    for key in unknown:
        say("complexity-ratchet: --accept %s names no refused rise; ignored." % key)
    if new_rows == before and marked_keys(htext) == {
            k for k in new_rows if k in markers}:
        say("complexity-ratchet: %s is current (%d rows); nothing written."
            % (TSV, len(new_rows)))
    else:
        with open(os.path.join(root, TSV), "w") as fh:
            fh.write(render_tsv(new_rows, markers, "tighten", head_sha(root)))
        say("complexity-ratchet: wrote %s (%d rows). Stage and commit it." % (
            TSV, len(new_rows)))
    if verdict == "REFUSED":
        print_refusal(root, corpus, bins, {})
        say("Refused rises keep their rows. Accept one with "
            "`tighten --accept <key> \"<reason>\"`.")
        return 1
    return 0


def mode_baseline(root):
    tool, entries, controls, corpus, cbins, present, dirty, clean, _ = \
        corpus_inputs(root, mode="baseline")
    bad = sorted(f for f in dirty if in_scope(f) and f != TSV)
    if bad:
        raise Refusal("baseline refuses dirty scored files (the rows would "
                      "describe uncommitted content): %s" % ", ".join(bad[:10]))
    if corpus.errors:
        raise Refusal("TUs with errors: %s\n  %s" % (
            ", ".join(sorted(corpus.errors)),
            "\n  ".join(e for es in corpus.errors.values() for e in es[:2])))
    markers = find_markers(root, corpus)
    rows = {k: (s, "") for k, s in corpus.scores.items() if s > CAP}
    os.makedirs(os.path.join(root, os.path.dirname(TSV)), exist_ok=True)
    with open(os.path.join(root, TSV), "w") as fh:
        fh.write(render_tsv(rows, markers, "baseline", head_sha(root)))
    scores = sorted(corpus.scores.values())
    say("complexity-ratchet: baseline: %d TUs, %d functions (median %d, p90 %d), "
        "%d rows over %d, %d marked -> %s" % (
            corpus.scored, len(scores), scores[len(scores) // 2],
            scores[len(scores) * 9 // 10], len(rows), CAP,
            sum(1 for k in rows if k in markers), TSV))
    print_bins(cbins)
    return 0


def mode_armed(root):
    if os.environ.get("OPENPETE_NO_COMMITS") == "1":
        say("complexity-ratchet: OPENPETE_NO_COMMITS=1, this machine declares "
            "it never commits; hook arming not required.")
        return 0
    why = lib.armed_reason(root, ("complexity_ratchet.py",))
    arm = ARM
    if not why:
        p = git(root, "config", "--get", "merge.%s.driver" % tsv_merge.DRIVER_NAME,
                check=False)
        if p.stdout.strip() != tsv_merge.DRIVER_CONFIG:
            why = ("merge.%s.driver is %s; the baselines would merge as text "
                   "and conflict on their `# head:` line"
                   % (tsv_merge.DRIVER_NAME, repr(p.stdout.strip()) if
                      p.stdout.strip() else "unset"))
            arm = tsv_merge.ARM_MERGE
    if why:
        say("complexity-ratchet: DISARMED: %s.\n  Arm this clone once:  %s\n"
            "  A machine that builds and never commits sets "
            "OPENPETE_NO_COMMITS=1 in its build script." % (why, arm))
        return 1
    say("complexity-ratchet: ARMED (%s, merge driver %s)"
        % (HOOK_REL, tsv_merge.DRIVER_NAME))
    return 0


def dispatch(root, mode, argv):
    if mode == "hook":
        return mode_hook(root)
    if mode == "check" and "--armed" in argv:
        return mode_armed(root)
    if mode == "check":
        return mode_check(root, "--ctest" in argv)
    if mode == "tighten":
        return mode_tighten(root, argv[1:])
    if mode == "baseline":
        return mode_baseline(root)
    return None


if __name__ == "__main__":
    sys.exit(lib.run(FENCE, dispatch, __doc__))
