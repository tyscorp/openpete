#!/usr/bin/env python3
"""Annotator: role attributes to a fixed point (core/include/openpete_thread_roles.h).

Runs scripts/fences/thread_roles.py's check 1 (clang-19 -Wthread-safety over
the line-preserving rewrite, where a synchronous reference and a slot call
are helper calls) and reads its diagnostics. A diagnostic naming a role
(`requires holding role 'op_role_<r>'`) adds, to every declaration of the
function it lies in (matched by USR), OP_REQUIRES(<r>) when the diagnostic
says "exclusively" and OP_REQUIRES_SHARED(<r>) otherwise; an exclusive
diagnostic upgrades an OP_REQUIRES_SHARED(<r>). The analysis then re-runs
over the TUs that name a newly annotated function, until no diagnostic adds
an attribute, and a last pass runs every TU. The process only adds or
upgrades attributes over a finite set of (function, role) pairs, so it ends.

It never annotates a binding-table entry, an assert, bind, enter or boot
function, or a function stored into an OP_SLOT whose roles lack the role
(that would widen the slot). There it stops and prints the chain from the
diagnostic to the access that started it; the fence reports the same site
as a refusal. A diagnostic naming a capability that is not a role (a
module's lock object) is printed as unresolved: it is fixed by taking the
lock, not by propagation. A check 3 refusal of the last pass is printed too.

Every declaration is found by libclang 14 in the TUs that define or name
the function; the attribute goes directly after the parameter list's
closing parenthesis, or, for a function returning a pointer to an array or
a function (`T (*f(P))[N]`), after the declarator's last suffix, outside
its groups, the place the fence reads it from. A declaration whose
parameter list cannot be placed in in-scope text (a macro expansion, a
generated header) is a stop.

Probe and deferral. --batch <json> (the refused list of `thread_roles.py replay
--json`, or of the hook under OPENPETE_THREAD_ROLES_REFUSED=<path>), or --probe
<file>:<function or ->:<name> for one global, writes OP_GUARDED_BY(<role>) on
every listed global without a confirmed local form, runs once to a fixed point
over the TUs naming what it annotates, restores every file, and reads one
verdict per global from its part of the demand: FIXED (a fixed point of at most
--limit functions, default 64), ORPHAN (it reaches a function with no
binding-table row: a non-call reference, or no call site and no generated
caller), OVER (more than the limit), TWOROLE (it reaches a binding-table entry
or slot of another role: the global takes an atomic or a lock, never a row) or
STUCK (an unplaceable declaration). --role names
the role (default sim), --role-of <path>=<role> a file's. --write-rows adds the
ORPHAN and OVER globals to baselines/thread-roles/deferred.tsv with their
chains, removes their unannotated.tsv rows, and records
OPENPETE_THREAD_ROLES_ACCEPT, stated once for the run. --tighten runs once over
every deferred row under the role it records and moves a FIXED row to
unannotated.tsv.

Usage:
  thread_roles_annotate.py [--db linux|mingw|both] [--dry-run] [--max-iter N]
  thread_roles_annotate.py --batch <json> [--role R] [--role-of P=R] [--write-rows]
  thread_roles_annotate.py --probe <key> [--role R] [--write-rows]
  thread_roles_annotate.py --tighten [--limit N]

Environment: as thread_roles.py (OPENPETE_THREAD_ROLES_DB,
OPENPETE_THREAD_ROLES_MINGW_DB, OPENPETE_THREAD_ROLES_JOBS).
Exit 0: fixed point with no stop, no unresolved diagnostic and no refusal.
Exit 1: a stop, an unresolved diagnostic or a refusal remains (printed).
Exit 2: the analysis did not run (no database, a TU does not parse).
A probe or batch exits 0 when all are FIXED, 3 when some are ORPHAN or OVER, 4
on a TWOROLE or STUCK.
"""
import os
import re
import sys
import glob
import signal
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "fences"))

import ratchet_lib as lib          # noqa: E402
import thread_roles as tr          # noqa: E402
import thread_roles_globals as pop  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, ".."))

DIAG = re.compile(r"^(calling function|reading variable|writing variable|passing variable|"
                  r"reading the value pointed to by|writing the value pointed to by|"
                  r"returning variable|returning the value pointed to by) "
                  r"'([^']+)'( \(referenced here\))?(?: by reference)? requires holding "
                  r"(\w+) '([^']+)'( exclusively)?")
ATTR_RE = re.compile(rb"\bOP_REQUIRES(_SHARED)?\((\w+)\)")


def say(msg=""):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# libclang: enclosing definitions and declarations by USR
# --------------------------------------------------------------------------
def parse(rel, directory, flags):
    ci = tr.ci()
    args = list(flags) + ["-Wno-everything"]
    if tr.MINGW_TARGET in flags:
        args += ["-resource-dir", tr.LIBCLANG_RESOURCE]
    cwd = os.getcwd()
    try:
        os.chdir(directory)
        return ci.Index.create().parse(os.path.join(ROOT, rel), args=args)
    finally:
        os.chdir(cwd)


def function_cursors(tu):
    """Every function declaration and definition the TU holds, in any file."""
    K = tr.ci().CursorKind
    out = []
    stack = list(tu.cursor.get_children())
    while stack:
        c = stack.pop()
        if c.kind in (K.UNEXPOSED_DECL, K.LINKAGE_SPEC, K.NAMESPACE, K.CLASS_DECL,
                      K.STRUCT_DECL):
            stack.extend(c.get_children())
            continue
        if c.kind in (K.FUNCTION_DECL, K.CXX_METHOD) and c.location.file:
            out.append(c)
    return out


class TUCache:
    """One libclang parse per (file, database) per iteration."""

    def __init__(self, dbs):
        self.dbs = dbs             # db name -> {rel: [(directory, flags)]}
        self.cache = {}

    def get(self, rel, db):
        if (rel, db) not in self.cache:
            d, f = self.dbs[db][rel][0]
            self.cache[(rel, db)] = function_cursors(parse(rel, d, f))
        return self.cache[(rel, db)]

    def clear(self):
        self.cache = {}


def enclosing(cursors, path, line):
    """-> the innermost definition in `path` whose extent holds `line`."""
    best = None
    for c in cursors:
        if not c.is_definition() or os.path.normpath(c.location.file.name) != path:
            continue
        s, e = c.extent.start.line, c.extent.end.line
        if s <= line <= e and (best is None or e - s < best.extent.end.line - best.extent.start.line):
            best = c
    return best


# --------------------------------------------------------------------------
# Text placement
# --------------------------------------------------------------------------
def probe_include(b, path):
    """-> b with the roles header included ahead of its first #include, by a
    path relative to the file: a probe's text is restored, and it must parse
    in a target with no core/include and above a global declared between
    two includes."""
    if re.search(rb'^[ \t]*#[ \t]*include[ \t]*"(?:\.\./include/)?openpete_thread_roles\.h"', b, re.M):
        return b
    rel = os.path.relpath(os.path.join(ROOT, tr.ROLES_H), os.path.dirname(path)).encode()
    m = re.search(rb"^[ \t]*#[ \t]*include\b", b, re.M)
    at = m.start() if m else 0
    return b[:at] + b'#include "' + rel + b'"\n' + b[at:]


def skip_ws_comments(b, i):
    while i < len(b):
        if b[i:i + 2] == b"/*":
            j = b.find(b"*/", i + 2)
            i = len(b) if j < 0 else j + 2
        elif b[i:i + 2] == b"//":
            j = b.find(b"\n", i)
            i = len(b) if j < 0 else j
        elif b[i:i + 1] in b" \t\r\n\\":
            i += 1
        else:
            return i
    return i


def close_paren(b, i):
    """b[i] is '('; -> the offset just past its matching ')'."""
    depth = 0
    while i < len(b):
        ch = b[i:i + 1]
        if b[i:i + 2] == b"/*":
            j = b.find(b"*/", i + 2)
            i = len(b) if j < 0 else j + 2
            continue
        if b[i:i + 2] == b"//":
            j = b.find(b"\n", i)
            i = len(b) if j < 0 else j
            continue
        if ch in (b'"', b"'"):
            q, i = ch, i + 1
            while i < len(b) and b[i:i + 1] != q:
                i += 2 if b[i:i + 1] == b"\\" else 1
        elif ch == b"(":
            depth += 1
        elif ch == b")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return -1


QUALIFIER = re.compile(rb"(?<![\w])(?:const|volatile|restrict|__restrict|__restrict__)$")


def group_depth(b, at):
    """-> how many parenthesised declarator groups enclose the name at `at`:
    each `(` of the run of `(`, `*` and pointer qualifiers that ends at the
    name, as in `T (*name`, `T (* const name` or `T (*(*name`."""
    depth, j = 0, at
    while True:
        k = j
        while k > 0 and b[k - 1:k] in b" \t\r\n":
            k -= 1
        m = QUALIFIER.search(b, max(0, k - 16), k)
        if k > 0 and b[k - 1:k] == b"*":
            j = k - 1
        elif m:
            j = m.start()
        elif k > 0 and b[k - 1:k] == b"(":
            depth, j = depth + 1, k - 1
        else:
            return depth


def suffixes_end(b, i):
    """-> the offset past the array bounds and parameter lists that follow
    offset i (`[N][M]`, `(int)`)."""
    while True:
        k = skip_ws_comments(b, i)
        if b[k:k + 1] == b"[":
            j = b.find(b"]", k)
            if j < 0:
                return i
            i = j + 1
        elif b[k:k + 1] == b"(":
            j = close_paren(b, k)
            if j < 0:
                return i
            i = j
        else:
            return i


def close_groups(b, i, depth, line, col):
    """-> the offset past `depth` declarator-group closers that follow offset
    i, each with the suffixes after it; raises ValueError when a `)` is not
    there."""
    for _ in range(depth):
        k = skip_ws_comments(b, i)
        if b[k:k + 1] != b")":
            raise ValueError("an unclosed declarator group after %d:%d" % (line, col))
        i = suffixes_end(b, k + 1)
    return i


def line_offset(b, line, col):
    off = 0
    for _ in range(line - 1):
        off = b.index(b"\n", off) + 1
    return off + col - 1


def place(b, line, col, name, role, excl):
    """-> (new bytes, changed) with OP_REQUIRES[_SHARED](role) after the
    parameter list of the declaration whose name is at (line, col);
    raises ValueError when the text there is not that declaration."""
    at = line_offset(b, line, col)
    nm = name.encode()
    if b[at:at + len(nm)] != nm:
        raise ValueError("the name is not spelled at %d:%d (a macro expansion?)" % (line, col))
    i = skip_ws_comments(b, at + len(nm))
    if b[i:i + 1] != b"(":
        raise ValueError("no parameter list after the name at %d:%d" % (line, col))
    end = close_paren(b, i)
    if end < 0:
        raise ValueError("unbalanced parameter list at %d:%d" % (line, col))
    # A function returning a pointer to an array or a function, `T (*f(P))[N]`:
    # the attribute goes past the whole declarator, outside its groups.
    end = close_groups(b, end, group_depth(b, at), line, col)
    # the attribute region: up to the declaration's ';' or the body's '{'
    stop = len(b)
    for t in (b";", b"{"):
        k = b.find(t, end)
        if 0 <= k < stop:
            stop = k
    region = b[end:stop]
    want = ("OP_REQUIRES(%s)" if excl else "OP_REQUIRES_SHARED(%s)") % role
    for m in ATTR_RE.finditer(region):
        if m.group(2).decode() != role:
            continue
        if not m.group(1) or not excl:
            return b, False            # held already (exclusive covers shared)
        a, z = end + m.start(), end + m.end()
        return b[:a] + want.encode() + b[z:], True
    return b[:end] + b" " + want.encode() + b[end:], True


INCLUDE = b'#include "openpete_thread_roles.h"'


def ensure_include(b):
    """-> b with the roles header included after its last top-of-file
    #include, when it does not include it already."""
    if re.search(rb'^[ \t]*#[ \t]*include[ \t]*"(?:\.\./include/)?openpete_thread_roles\.h"', b, re.M):
        return b
    last = None
    first_brace = b.find(b"{")
    for m in re.finditer(rb"^[ \t]*#[ \t]*include\b[^\n]*\n", b, re.M):
        if 0 <= first_brace < m.start():
            break          # top-of-file includes only
        last = m
    at = last.end() if last else 0
    return b[:at] + INCLUDE + b"   /* OP_REQUIRES */\n" + b[at:]


# --------------------------------------------------------------------------
# The fixed point
# --------------------------------------------------------------------------
class Annotator:
    def __init__(self, which, dry):
        self.dry = dry
        linux, mingw = tr.databases(ROOT, need_mingw=which in ("mingw", "both"))
        if linux is None:
            raise lib.Refusal("no linux-release compile database; run %s" % lib.CONFIGURE)
        self.dbs = {}
        if which in ("linux", "both"):
            self.dbs["linux"] = linux
        if which in ("mingw", "both"):
            self.dbs["mingw"] = mingw
        self.all_dbs = {"linux": linux, "mingw": mingw or {}}
        self.entries = set(linux) | set(mingw or {})
        self.tus = TUCache(self.all_dbs)
        self.held = {}         # usr -> {role: "excl" | "shared"} written by this run
        self.reason = {}       # (usr, role) -> (where, what)
        self.names = {}        # usr -> spelling
        self.stops = []        # (where, entry name, why, chain start (usr, role))
        self.unresolved = []
        self.slot_roles = {}   # usr -> [slot roles]
        self.edits = 0
        self.iterations = 0
        self.saved = None      # path -> bytes before this run, kept by a probe
        self.edges = {}        # (usr, role) -> {(verb, object)}: why it needs the role

    def select(self, files):
        sel = {}
        for name, entries in self.dbs.items():
            sel[name] = {r: l for r, l in entries.items() if files is None or r in files}
        return sel

    def analyse(self, files):
        sel = self.select(files)
        n = sum(len(lst) for v in sel.values() for lst in v.values())
        tables, ctx, results, wall = tr.run_analysis(ROOT, sel, tr.jobs(), "annotate")
        say("annotate: pass %d, %d TU entries, %.1f s wall" % (self.iterations, n, wall))
        parse = [p for r in results for p in r["parse"]]
        if parse:
            for p in parse[:20]:
                say("    " + p)
            raise lib.Refusal("VACUOUS: %d TU(s) do not parse under clang-19" % len(parse))
        for r in results:
            for usr, name, slot in r.get("slot_stores", ()):
                self.slot_roles.setdefault(usr, set()).update(slot.split("|"))
        return tables, results

    def run(self, max_iter, files=None):
        """To a fixed point, confirmed over every TU; a run given `files`
        (a probe) confirms over the TUs naming what it annotated."""
        probe = files is not None
        named = set()
        while True:
            self.iterations += 1
            if self.iterations > max_iter:
                raise lib.Refusal("no fixed point after %d passes" % max_iter)
            tables, results = self.analyse(files)
            self.tus.clear()
            want = self.collect(tables, results)
            if not want:
                return tables, results
            touched_names = self.apply(want)
            if self.dry:
                return tables, results
            named |= touched_names
            files = self.files_naming(touched_names)
            if probe:
                files |= self.files_naming(named)

    def files_naming(self, names):
        pat = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, sorted(names))))
        out = set()
        for rel in self.entries:
            try:
                if pat.search(tr.read_text(os.path.join(ROOT, rel))):
                    out.add(rel)
            except OSError:
                pass
        return out

    def collect(self, tables, results):
        """-> {usr: {role: excl bool}} the diagnostics ask for."""
        want = {}
        seen = set()
        for r in results:
            for relp, ln, col, msg in r["diags"]:
                key = (relp, ln, col, msg)
                if key in seen:
                    continue
                seen.add(key)
                m = DIAG.match(msg)
                where = "%s:%d" % (relp, ln)
                if not m or m.group(4) != "role" or not m.group(5).startswith("op_role_"):
                    self.unresolved.append("%s: %s" % (where, msg))
                    continue
                verb, obj, ref, _, cap, excl = m.groups()
                role = cap[len("op_role_"):]
                excl = bool(excl)
                if self.saved is not None and relp.startswith("tests/"):
                    continue       # a test calls what it tests; its main holds no role
                cur = enclosing(self.tus.get(r["rel"], r["db"]), os.path.join(ROOT, relp), ln)
                if cur is None:
                    self.unresolved.append("%s: %s (in no function definition)" % (where, msg))
                    continue
                usr, name = cur.get_usr(), cur.spelling
                self.names[usr] = name
                what = "%s %s%s" % (verb, obj, " (a synchronous or slot reference)" if ref else "")
                self.edges.setdefault((usr, role), set()).add((verb, obj))
                why = self.refuse_reason(tables, usr, name, role)
                if why:
                    self.stops.append((where, name, why, what, (obj, role)))
                    continue
                if self.holds(usr, role, excl):
                    continue
                w = want.setdefault(usr, {})
                w[role] = w.get(role, False) or excl
                self.reason.setdefault((usr, role), (where, what, name))
        return want

    def refuse_reason(self, tables, usr, name, role):
        if usr in tables.rows:
            return "a binding-table entry (%s); it asserts or binds its role instead" \
                % tables.rows[usr][0]
        if name in tr.BOOT_FNS or tr.MACRO_TARGETS.match(name) \
                or name in ("op_role_leave", "op_role_check_at"):
            return "a role or boot function"
        slots = self.slot_roles.get(usr)
        if slots and role not in slots:
            return "stored into an OP_SLOT(%s) slot, which %s would widen" \
                % ("|".join(sorted(slots)), role)
        return None

    def holds(self, usr, role, excl):
        h = self.held.get(usr, {}).get(role)
        return h == "excl" or (h == "shared" and not excl)

    def declarations(self, usr, name):
        """-> {path: {(line, col)}} of every declaration of usr in the TUs that
        define or name it: their linux parse, and their mingw parse when the
        linux one finds none or the file has no linux entry."""
        out = {}
        naming = sorted(self.files_naming({name}))
        for db in ("linux", "mingw"):
            for rel in naming:
                if rel not in self.all_dbs[db]:
                    continue
                if db == "mingw" and out and rel in self.all_dbs["linux"]:
                    continue
                try:
                    cursors = self.tus.get(rel, db)
                except Exception as e:      # libclang could not parse
                    raise lib.Refusal("libclang parse of %s [%s] failed: %s" % (rel, db, e))
                for c in cursors:
                    if c.get_usr() == usr:
                        out.setdefault(os.path.normpath(c.location.file.name), set()).add(
                            (c.location.line, c.location.column))
        return out

    def apply(self, want):
        names = set()
        per_file = {}
        for usr, roles in sorted(want.items()):
            name = self.names[usr]
            decls = self.declarations(usr, name)
            bad = [p for p in decls if not p.startswith(ROOT + os.sep)
                   or not tr.FENCE.in_scope(os.path.relpath(p, ROOT))]
            if bad or not decls:
                for role in roles:
                    where, what, _ = self.reason[(usr, role)]
                    self.stops.append((where, name, "declared outside the in-scope tree (%s)"
                                       % ", ".join(sorted(bad)) if bad else "no declaration found",
                                       what, None))
                continue
            for role, excl in roles.items():
                h = self.held.setdefault(usr, {})
                h[role] = "excl" if excl or h.get(role) == "excl" else "shared"
                for p, locs in decls.items():
                    for line, col in locs:
                        per_file.setdefault(p, []).append((line, col, name, role, excl))
            names.add(name)
        for p, items in sorted(per_file.items()):
            with open(p, "rb") as fh:
                b = fh.read()
            orig = b
            # last position first, so earlier offsets hold
            for line, col, name, role, excl in sorted(items, reverse=True):
                try:
                    b, changed = place(b, line, col, name, role, excl)
                except ValueError as e:
                    self.stops.append(("%s:%d" % (os.path.relpath(p, ROOT), line), name,
                                       "cannot place the attribute: %s" % e, "", None))
                    continue
                if changed:
                    self.edits += 1
                    say("  + %s:%d %s %s(%s)" % (os.path.relpath(p, ROOT), line, name,
                                                  "OP_REQUIRES" if excl else "OP_REQUIRES_SHARED",
                                                  role))
            if b != orig:
                b = ensure_include(b) if self.saved is None else probe_include(b, p)
            if b != orig and not self.dry:
                if self.saved is not None:
                    self.saved.setdefault(p, orig)
                with open(p, "wb") as fh:
                    fh.write(b)
        return names

    def chain(self, start):
        """The reasons from a stopped edge back to the access that began it."""
        out = []
        obj, role = start
        guard = 0
        while obj and guard < 64:
            guard += 1
            hit = [(u, r) for (u, r) in self.reason if r == role and self.names.get(u) == obj]
            if not hit:
                break
            where, what, name = self.reason[hit[0]]
            out.append("%s at %s: %s" % (name, where, what))
            m = re.match(r"(calling function|reading variable|writing variable) (\S+)", what)
            if not m or not m.group(1).startswith("calling"):
                break
            obj = m.group(2)
        return out


# --------------------------------------------------------------------------
# Probe, batch, deferred rows, tighten
# --------------------------------------------------------------------------
def scan_of(rel, directory, flags):
    """-> a population Scan of one TU, for reading declaration locations."""
    ci = tr.ci()
    args = list(flags) + ["-Wno-everything"]
    mingw = tr.MINGW_TARGET in flags
    if mingw:
        args += ["-resource-dir", tr.LIBCLANG_RESOURCE]
    if tr.lang_of(rel, flags) == "c++":
        for d in tr.cxx_includes(mingw):
            args += ["-isystem", d]
    tu = ci.Index.create().parse(os.path.join(ROOT, rel), args=args)
    return pop.Scan(ci, tu, {"root": ROOT}, tr.FENCE,
                    lambda r: tr.read_text(os.path.join(ROOT, r)))


def declarator_end(b, at, name):
    """-> the offset past the declarator whose identifier is at `at` in b:
    past its array bounds and past each enclosing group's `)` with the array
    bounds or parameter list after it (`(*name)(int)`, `(*name)[3][3]`). -1
    when the bytes there do not spell the name or a group is unclosed."""
    nm = name.encode()
    if b[at:at + len(nm)] != nm:
        return -1
    i = suffixes_end(b, at + len(nm))
    try:
        return close_groups(b, i, group_depth(b, at), 0, 0)
    except ValueError:
        return -1


def declaration_sites(keys, entries, which):
    """-> {key: [(rel, line, col)]} of every declaration of each row key in
    the TUs naming it, read by libclang."""
    out = {k: set() for k in keys}
    by_name = {}
    for k in keys:
        by_name.setdefault(k[2].rsplit(".", 1)[-1], []).append(k)
    pat = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, by_name)))
    K = tr.ci().CursorKind
    for rel in sorted(entries):
        if not pat.search(tr.read_text(os.path.join(ROOT, rel))):
            continue
        d, f = entries[rel][0]
        s = scan_of(rel, d, f)

        def note(c, rec):
            for k in by_name.get(c.spelling, ()):
                if (rec["path"], rec["function"], rec["name"]) == k or (
                        rec["kind"] == "extern" and k[2] == rec["name"]):
                    out[k].add((s.rel_of(c.location.file.name), c.location.line,
                                c.location.column))
        for c, r in s.items():
            if c.kind == K.VAR_DECL and c.spelling in by_name:
                rec = s.member(c)
                if rec:
                    note(c, rec)
            elif c.kind in s.fns and c.is_definition() and c.location.file \
                    and any(k[1] != "-" and k[1].rsplit(".", 1)[-1] == c.spelling
                            for ks in by_name.values() for k in ks):
                for b, _ in s.walk_body(c):
                    if b.kind == K.VAR_DECL and b.spelling in by_name:
                        rec = s.member(b)
                        if rec:
                            note(b, rec)
    return out


def write_forms(sites, roles, saved):
    """Insert OP_GUARDED_BY at each declaration. sites: {key: locations}.
    -> the keys with a location the text does not spell."""
    per_file, bad = {}, set()
    for key, locs in sites.items():
        for rel, line, col in locs:
            per_file.setdefault(rel, []).append((line, col, key))
    for rel, items in per_file.items():
        p = os.path.join(ROOT, rel)
        with open(p, "rb") as fh:
            b = fh.read()
        saved.setdefault(p, b)
        places = []
        for line, col, key in set(items):
            end = declarator_end(b, line_offset(b, line, col), key[2].rsplit(".", 1)[-1])
            if end < 0:
                bad.add(key)
            else:
                places.append((end, key))
        for end, key in sorted(places, key=lambda t: -t[0]):
            b = b[:end] + b" OP_GUARDED_BY(%s)" % roles[key].encode() + b[end:]
        with open(p, "wb") as fh:
            fh.write(probe_include(b, p))
    return bad


def call_sites(name, sources):
    """-> the count of `name(` outside its declarations and definition, a
    declaration being a line that starts at column 0 with a type or a name."""
    pat = re.compile(r"\b%s\s*\(" % re.escape(name))
    n = 0
    for text in sources:
        if name not in text:
            continue
        for m in pat.finditer(text):
            ls = text.rfind("\n", 0, m.start()) + 1
            if m.start() == ls or (text[ls:m.start()].strip() and not text[ls].isspace()):
                continue
            n += 1
    return n


def generated_dirs():
    """-> the directories holding recompiled C, whose callers count as sim."""
    env = os.environ.get("OPENPETE_THREAD_ROLES_GENERATED")
    if env:
        return env.split(os.pathsep)
    return glob.glob(os.path.join(ROOT, "games/*/generated"))


def in_generated(name):
    for d in generated_dirs():
        p = subprocess.run(["grep", "-rlwm1", "--include=*.c", "--include=*.h", name, d],
                           capture_output=True, text=True)
        if p.stdout.strip():
            return True
    return False


class Sources:
    """The in-scope text, read once per process."""
    text = None

    @classmethod
    def all(cls):
        if cls.text is None:
            cls.text = [tr.read_text(os.path.join(ROOT, r)) for r in tr.tracked_scope(ROOT)]
        return cls.text


def demand_graph(A):
    """-> (var: {global name: {(usr, role)}}, callers: {function name: {(usr,
    role)}}). Accesses come from the diagnostics, all raised on the first
    pass, when every form is in place. Call edges come from the text of the
    functions the run annotated: a caller annotated for one global raises no
    diagnostic for its other callees on a later pass, so the diagnostics alone
    would give a shared function's callers to one global only."""
    var, callers = {}, {}
    for key, objs in A.edges.items():
        for verb, obj in objs:
            base = re.match(r"\w+", obj)
            dst = callers if verb.startswith("calling") else var
            dst.setdefault(base.group(0) if base else obj, set()).add(key)
    keys = {}
    for key in A.edges:
        keys.setdefault(A.names.get(key[0], ""), []).append(key)
    names = sorted(n for n in keys if n)
    pat = re.compile(r"\b(%s)\s*\(" % "|".join(map(re.escape, names))) if names else None
    for rel in tr.tracked_scope(ROOT) if pat else ():
        text = tr.read_text(os.path.join(ROOT, rel))
        here = sorted({n for n in names if n in text})
        if not here:
            continue
        for fn in here:
            for lo, hi in tr.writer_spans(text, [fn]):
                body = "\n".join(text.split("\n")[lo - 1:hi])
                for m in pat.finditer(body):
                    if m.group(1) != fn:
                        callers.setdefault(m.group(1), set()).update(keys[fn])
    return var, callers


def closure_of(name, A, var, callers):
    """-> {(usr, role): parent function name or the global} of the functions
    a form on the global `name` makes require a role."""
    seen = {}
    todo = []
    for k in var.get(name, ()):
        seen[k] = name
        todo.append(k)
    while todo:
        k = todo.pop()
        fn = A.names.get(k[0], "")
        for c in callers.get(fn, ()):
            if c not in seen:
                seen[c] = fn
                todo.append(c)
    return seen


def terminals(A, results):
    """-> {function name: why} of a finished run: a stop, a non-call
    reference with no form, or a held function with no call site and no
    generated caller."""
    out = {}
    tables = tr.Tables(ROOT, [tr.ROLES_H])
    for where, name, why, what, start in A.stops:
        if "binding-table entry" in why:
            role = start[1] if start else ""
            if not any(name == tr.usr_name(u) and role in v[1] for u, v in tables.rows.items()):
                out[name] = ("TWOROLE", why)
        elif "OP_SLOT" in why:
            out[name] = ("TWOROLE", why)
        else:
            out[name] = ("STUCK", why)
    for r in results:
        for x in r["refusals"]:
            m = re.search(r"reference to the role-annotated (\w+)", x)
            if m:
                out.setdefault(m.group(1), ("ORPHAN", "non-call reference"))
    for usr, name in A.names.items():
        if usr in A.held and name not in out and not call_sites(name, Sources.all()) \
                and not in_generated(name):
            out[name] = ("ORPHAN", "no call site, no row")
    return out


def chain_to(start, seen, A):
    """-> the steps from a function in the closure down to the global."""
    steps = []
    cur = [k for k in seen if A.names.get(k[0]) == start][:1]
    guard = 0
    while cur and guard < 24:
        guard += 1
        parent = seen[cur[0]]
        steps.append("%s <- %s" % (parent, A.names.get(cur[0], "?")))
        cur = [k for k in seen if A.names.get(k[0]) == parent][:1]
    return steps


def batch(keys, which, limit, max_iter):
    """One annotator run over every (key: role): the forms go in together,
    the shared closure is computed once, the tree is restored, and each key's
    verdict is read from its own part of the demand graph.
    -> {key: (verdict, terminal, chain, attributes)}."""
    A = Annotator(which, False)
    A.saved = {}
    # A terminating signal must reach the `finally` that restores the tree.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    entries = dict(A.all_dbs["mingw"])
    entries.update(A.all_dbs["linux"])
    sites = declaration_sites(list(keys), entries, which)
    out = {}
    for k, locs in sites.items():
        if not locs:
            out[k] = ("STUCK", "no declaration libclang places in the text", [], 0)
    live = {k: r for k, r in keys.items() if k not in out}
    try:
        bad = write_forms({k: sites[k] for k in live}, live, A.saved)
        for k in bad:
            out[k] = ("STUCK", "a declaration comes from a macro expansion", [], 0)
        names = {k[2].rsplit(".", 1)[-1] for k in live}
        files = A.files_naming(names) & A.entries
        tables, results = A.run(max_iter, files or None)
    finally:
        for p, b in A.saved.items():
            with open(p, "wb") as fh:
                fh.write(b)
    var, callers = demand_graph(A)
    term = terminals(A, results)
    say("batch: %d global(s), %d pass(es), %d attribute(s) over the union, %d terminal(s)"
        % (len(live), A.iterations, sum(len(h) for h in A.held.values()), len(term)))
    for k in live:
        if k in out:
            continue
        seen = closure_of(k[2].rsplit(".", 1)[-1], A, var, callers)
        # A role crossing outranks an orphan: it takes a form, never a row.
        rank = {"TWOROLE": 0, "STUCK": 1, "ORPHAN": 2}
        hit = sorted({A.names.get(c[0]) for c in seen} & set(term),
                     key=lambda n: (rank[term[n][0]], n))
        n = len({c[0] for c in seen})
        if hit:
            v, why = term[hit[0]]
            out[k] = (v, "%s (%s)" % (hit[0], why), chain_to(hit[0], seen, A), n)
        elif n > limit:
            out[k] = ("OVER", "over-%d" % limit, [], n)
        else:
            out[k] = ("FIXED", "%d attribute(s)" % n, [], n)
    return out


def row_note(role, reason, verdict, terminal, chain):
    """-> the note column: role, the commit's reason, verdict, terminal and
    the printed chain."""
    text = "role=%s; reason=%s; %s %s; chain %s" % (
        role, reason, verdict, terminal, " <- ".join(chain[:24]) or "(none printed)")
    return re.sub(r"\s+", " ", text)


def rewrite_lists(un_rows, de_rows, sha):
    with open(os.path.join(ROOT, pop.UNANNOTATED), "w") as fh:
        fh.write(pop.render_tsv("mutable globals with no form (thread_roles.py census)",
                                un_rows, "annotator", sha))
    with open(os.path.join(ROOT, pop.DEFERRED), "w") as fh:
        fh.write(pop.render_tsv("mutable globals whose demand cascades, each row with its "
                                "annotator chain", de_rows, "annotator", sha))


def read_lists():
    """-> (unannotated rows, deferred rows) as {key: cols}."""
    un = pop.Baseline(tr.read_text(os.path.join(ROOT, pop.UNANNOTATED)))
    path = os.path.join(ROOT, pop.DEFERRED)
    de = pop.Baseline(tr.read_text(path) if os.path.isfile(path) else "")
    return un.rows, de.rows


def role_for(key, default, role_of):
    """-> the role a key is probed under: the first `path=role` whose path
    the key's file ends with, else the default."""
    for path, role in role_of:
        if key[0].endswith(path):
            return role
    return default


def keys_of_json(path, un):
    """-> the row keys of a refused-global list (`replay --json`, or the
    hook's OPENPETE_THREAD_ROLES_REFUSED output)."""
    import json
    with open(path) as fh:
        data = json.load(fh)
    names = {}
    for k, row in un.items():
        names.setdefault(k[2], []).append((k, row))
    out = []
    for r in data["refused"]:
        hits = [k for k, row in names.get(r["name"], ())
                if (row[3] == "extern" if r["kind"] == "extern"
                    else k[0] == r["path"] and k[1] == r["function"])]
        out += hits[:1]
    return out


def report_batch(res, roles, un, de, write, reason):
    """Print each verdict; with write, add the ORPHAN and OVER rows. -> the
    verdict counts."""
    counts = {}
    for key, (v, terminal, chain, n) in sorted(res.items()):
        counts[v] = counts.get(v, 0) + 1
        say("PROBE %s role %s: %s: %s; %d attribute(s)" % (":".join(key), roles[key], v, terminal, n))
        for step in chain:
            say("       because %s" % step)
        if write and v in ("ORPHAN", "OVER"):
            kind = (un.get(key) or [None, None, None, "static"])[3]
            de[key] = list(key) + [kind, "-", row_note(roles[key], reason, v, terminal, chain)]
            un.pop(key, None)
    if write:
        rewrite_lists(list(un.values()), list(de.values()), lib.head_sha(ROOT))
    say("batch verdicts: %s" % ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    return counts


def mode_probe(argv, which, limit, max_iter):
    """--probe <key> | --batch <json>, with --write-rows."""
    default = argv[argv.index("--role") + 1] if "--role" in argv else "sim"
    role_of = [tuple(argv[i + 1].split("=", 1)) for i, a in enumerate(argv) if a == "--role-of"]
    write = "--write-rows" in argv or "--write-row" in argv
    reason = lib.accept_reason(pop.ACCEPT_ENV)
    if write and not reason:
        say("thread_roles_annotate: writing rows needs %s, stated once" % pop.ACCEPT_ENV)
        return 2
    un, de = read_lists()
    if "--probe" in argv:
        wanted = [tuple(argv[argv.index("--probe") + 1].split(":"))]
    else:
        wanted = keys_of_json(argv[argv.index("--batch") + 1], un)
    if any(len(k) != 3 for k in wanted):
        say("thread_roles_annotate: a key is <file>:<function or ->:<name>")
        return 2
    keys = {}
    for k in wanted:
        row = un.get(k)
        if row is not None and row[4] in pop.REMEDY:
            say("PROBE %s: the census confirms the local form %s (%s); not deferred"
                % (":".join(k), row[4], row[5]))
        else:
            keys[k] = role_for(k, default, role_of)
    counts = report_batch(batch(keys, which, limit, max_iter), keys, un, de, write, reason)
    return 0 if set(counts) <= {"FIXED"} else 4 if set(counts) - {"FIXED", "ORPHAN", "OVER"} else 3


def mode_tighten(which, limit, max_iter):
    """One run over every deferred row under the role it records; a row
    whose verdict is FIXED moves to unannotated.tsv."""
    un, de = read_lists()
    roles = {}
    for key, row in de.items():
        m = re.match(r"role=(\w+)", row[5])
        roles[key] = m.group(1) if m else "sim"
    moved = 0
    for key, (v, _, _, _) in sorted(batch(roles, which, limit, max_iter).items() if roles else []):
        say("  %s: %s" % (":".join(key), v))
        if v == "FIXED":
            un[key] = list(key) + [de[key][3], "-", ""]
            del de[key]
            moved += 1
    rewrite_lists(list(un.values()), list(de.values()), lib.head_sha(ROOT))
    say("annotate --tighten: %d row(s) moved to %s, %d deferred" % (moved, pop.UNANNOTATED, len(de)))
    return 0


def main(argv):
    which, dry, max_iter, limit = "both", False, 40, 64
    if "--limit" in argv:
        limit = int(argv[argv.index("--limit") + 1])
    if "--db" in argv:
        which = argv[argv.index("--db") + 1]
    try:
        if "--probe" in argv or "--batch" in argv:
            return mode_probe(argv, which, limit, max_iter)
        if "--tighten" in argv:
            return mode_tighten(which, limit, max_iter)
    except lib.Refusal as e:
        say("annotate: %s" % e)
        return 2
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--db":
            which = argv[i + 1]
            i += 1
        elif a == "--dry-run":
            dry = True
        elif a == "--max-iter":
            max_iter = int(argv[i + 1])
            i += 1
        elif a in ("-h", "--help"):
            say(__doc__)
            return 0
        else:
            say("thread_roles_annotate: unknown argument %s" % a)
            return 2
        i += 1
    if which not in ("linux", "mingw", "both"):
        say("thread_roles_annotate: --db takes linux, mingw or both")
        return 2
    try:
        A = Annotator(which, dry)
        tables, results = A.run(max_iter)
    except lib.Refusal as e:
        say("annotate: %s" % e)
        return 2
    return finish(A, results, dry)


def finish(A, results, dry):
    """Print the run's stops, unresolved diagnostics and refusals. -> exit."""
    other = sorted({x for r in results for x in r["refusals"] if "-Wthread-safety" not in x})
    stop_sites = {w for w, *_ in A.stops}
    left = sorted({"%s:%d: %s" % (d[0], d[1], d[3]) for r in results for d in r["diags"]
                   if "%s:%d" % (d[0], d[1]) not in stop_sites})
    A.unresolved += left
    say("annotate: %d pass(es), %d attribute(s) written%s, %d function(s) annotated by this run"
        % (A.iterations, A.edits, " (dry run)" if dry else "", len(A.held)))
    stops = sorted(set((w, n, why, what, s) for w, n, why, what, s in A.stops))
    for where, name, why, what, start in stops:
        say("STOP %s: %s %s; %s is %s" % (where, name, what, name, why))
        if start:
            for step in A.chain(start):
                say("       because %s" % step)
    for u in sorted(set(A.unresolved)):
        say("UNRESOLVED %s" % u)
    for o in other:
        say("REFUSED %s" % o)
    return 1 if (stops or A.unresolved or other) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
