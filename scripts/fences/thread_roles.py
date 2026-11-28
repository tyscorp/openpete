#!/usr/bin/env python3
"""Fence: thread-role ownership (core/include/openpete_thread_roles.h).

Check 1 (analysis). Every selected (file, flags) entry of the linux-release
and windows-mingw compile databases is compiled by clang-19 -fsyntax-only
with -Werror=thread-safety -Werror=ignored-attributes and
SDL_THREAD_SAFETY_ANALYSIS undefined, over a line-preserving rewritten copy
of the TU served through -ivfsoverlay (diagnostics name the source file and
line). The rewrite:
  - a non-call reference to a role-annotated function passed to an
    OP_SYNC_FN parameter (or to qsort/bsearch) becomes `(helper(), f)`,
    where the helper carries f's role attributes, so the analysis checks
    the reference as a call at that site;
  - a call through an OP_SLOT(<role>) slot becomes `(helper(), slot)(...)`
    with a helper that requires <role>;
  - every OP_SEALED global is declared const outside the functions
    OP_SEAL_TABLE lists for its seal, so a write site (assignment, compound
    assignment, increment, a non-const pointer or reference to it or to an
    array member) fails to compile. A dropped-const warning
    (discards-qualifiers, cast-qual) on a line naming a sealed global is a
    write site too.

Check 3 (binding table), read by libclang 14 from macro-instantiation
cursors and the AST:
  - OP_ROLE_BIND / OP_ROLE_ASSERT / OP_ROLE_ASSERT_SHARED / OP_ROLE_ENTER
    only in a function OP_ROLE_BINDING_TABLE lists, naming a role of its
    row; a role macro inside another macro's body; a direct call of the
    functions those macros expand to;
  - every non-call reference to a role-relevant function classified as
    synchronous, slot, entry (the row's registration form) or test, any
    other refused; a row function carrying role attributes;
  - a read of an OP_SLOT slot that is not a call, a test or a store into a
    slot holding the source slot's roles;
  - a call of a boot function outside openpete_run and run_split_threaded;
    a call of co_switch outside openpete_fiber_switch;
  - declarations of one function whose role attributes differ; a local
    prototype of an annotated function; a declaration of a sealed global
    without OP_SEALED;
  - OP_NO_THREAD_SAFETY_ANALYSIS outside OP_TSA_ACCESSOR_TABLE;
    OP_TSA_SUPPRESS_BEGIN without a non-empty string reason;
  - hook only: an added line that adds or changes a table row, or changes
    a global's form (OP_GUARDED_BY*, OP_PT_GUARDED_BY*, OP_SEALED), without a
    `thread-roles: <reason>` marker. A global's first form needs none.

Check 2 (population, thread_roles_globals.py), two halves. New-global half,
always refusing: a form-less global (declared in a selected TU) in neither
baselines/thread-roles/unannotated.tsv nor deferred.tsv; a row added to
either since the base without a reason in its note (in the hook also
without OPENPETE_THREAD_ROLES_ACCEPT naming it); changed code no TU
reaches. Extent half, refusing only when CHECK2_ARMED: a form-less global
declared or named in a touched extent, listed or not (a row of deferred.tsv
passes); a row whose global has a form; a global in both TSVs; and, for a
whole-tree `check`, a row whose global is gone. A refusal names the
census's confirmed form or the next step.

Check 4 (engagement, LAW 8). TUs analysed per database, annotated
functions and globals, rows, accessors and escapes evaluated and references
classified are printed; zero TUs to analyse while in-scope sources changed,
a missing or unparsable database, or a selected TU that does not parse
under clang is VACUOUS and refuses, and the last line counts the unparsed
entries. Before the analysis the hook and range configure a missing
database and build the headers a build rule generates
(openpete_generated_headers, under nice -n 10) in each database's build
directory when the -M -MG pass of the selected entries finds one absent;
the mode line
prints the entries probed, the seconds, and the build when one ran. `check`
runs inside `cmake --build` of the same directory, after that target, and
never builds; OPENPETE_FENCE_NO_BUILD=1 makes the hook refuse
MISSING-INPUT instead. A binding-table, accessor or escape row
neither database defines is reported (check --all).

The escape table (OP_ESCAPE_TABLE) lists the functions that reach owned
state through a pointer the analysis does not follow, each with the
protocol that orders the accesses; it is reported, not checked.

scripts/thread_roles_annotate.py runs check 1 to a fixed point and writes
the OP_REQUIRES / OP_REQUIRES_SHARED attributes its diagnostics ask for.

Modes:
  hook            git pre-commit: the staged in-scope sources; a staged
                  header (or this fence, or the roles header) selects every
                  TU. Exit 1 refuses the commit.
  check           every build: the TUs changed since the last passing check
                  (a stamp beside the database); a changed header selects
                  every TU. --all selects every TU. --ctest: 64 without a
                  database.
  check --armed   exit 1 unless the tracked pre-commit hook calls this fence.
  range <b>..<h>  the hook's checks over the cumulative diff b..h, at h,
                  which must be HEAD with a clean scope (the landing path: a
                  rebase or cherry-pick runs no pre-commit hook), and the TSV
                  compare with b.
  census          classify the population over both databases, confirm local
                  forms; --write writes unannotated.tsv, --json <path> the
                  per-global facts.
  tighten         drop the unannotated.tsv rows whose global has a form or is
                  gone, then re-run the annotator over deferred.tsv.
  owe "<note>"    tighten's drop, and a row with <note> for every form-less
                  global in neither TSV (the census's confirmed form kept).
  replay <sha>    the touched-extent half of one past commit, over that
                  commit's text in this tree's databases; --json <path>.
  sites           every check site in the tree (kind, file, line).
  join <report>   an OPENPETE_THREAD_ROLE_REPORT file joined to the site
                  list; a site no run reached shows as zero.

Environment:
  OPENPETE_THREAD_ROLES_DB        linux-release database path.
  OPENPETE_THREAD_ROLES_MINGW_DB  windows-mingw database path; missing, the
                                  windows-mingw preset is configured.
  OPENPETE_THREAD_ROLES_JOBS      parallel TUs (default half the CPUs, at most 16).
  OPENPETE_NO_COMMITS=1           `check --armed` passes unarmed.
  OPENPETE_THREAD_ROLES_ACCEPT    the reason for a row added to a TSV on this
                                  commit; it names each added global, or a
                                  deferred row's chain terminal.
  OPENPETE_THREAD_ROLES_REFUSED   a path: the hook and range write the refused
                                  globals there, for the annotator's --batch.
  OPENPETE_THREAD_ROLES_NO_REWRITE=1  calibration control: synchronous
                                  references and slot calls are not
                                  rewritten into helper calls, so check 1
                                  does not see them as calls.
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time

import ratchet_lib as lib
from ratchet_lib import Refusal, say, git, is_header, entry_flags
import thread_roles_globals as pop

# False: check 2 prints what it would refuse and refuses nothing.
CHECK2_ARMED = False
SCOPE_ROOTS = ("core/", "games/", "tests/")
FENCE = lib.Fence("thread-roles", "thread_roles.py", "", SCOPE_ROOTS,
                  "OPENPETE_THREAD_ROLES_DB")
ROLES_H = "core/include/openpete_thread_roles.h"
FENCE_SCRIPT = "scripts/fences/thread_roles.py"
RUNTIME_C = "core/runtime/thread_roles.c"
STAMP = "thread_roles.stamp"
CLANG = {"c": "clang-19", "c++": "clang++-19"}
MINGW_TARGET = "--target=x86_64-w64-mingw32"
LIBCLANG_RESOURCE = os.path.join(os.path.dirname(lib.LIBCLANG), "clang", "14.0.0")
CLANG_CHECK = ["-fsyntax-only", "-Wno-unknown-warning-option",
               "-Wno-everything", "-Werror=thread-safety",
               "-Werror=ignored-attributes",
               "-Wincompatible-pointer-types-discards-qualifiers",
               "-Wcast-qual", "-USDL_THREAD_SAFETY_ANALYSIS", "-ferror-limit=0",
               "-fno-caret-diagnostics", "-fno-diagnostics-color"]

ATTR_MACROS = ("OP_REQUIRES", "OP_REQUIRES_SHARED", "OP_REQUIRES_CAP",
               "OP_REQUIRES_SHARED_CAP", "OP_ACQUIRE_CAP", "OP_RELEASE_CAP",
               "OP_ACQUIRE_SHARED_CAP", "OP_RELEASE_SHARED_CAP")
NTSA = "OP_NO_THREAD_SAFETY_ANALYSIS"
STMT_MACROS = ("OP_ROLE_BIND", "OP_ROLE_ASSERT", "OP_ROLE_ASSERT_SHARED",
               "OP_ROLE_ENTER")
FORM_MACROS = ("OP_GUARDED_BY", "OP_PT_GUARDED_BY", "OP_GUARDED_BY_CAP",
               "OP_PT_GUARDED_BY_CAP", "OP_SEALED")
BOOT_FNS = ("op_role_boot_acquire", "op_role_boot_release",
            "op_role_boot_handoff", "op_role_boot_reacquire")
BOOT_CALLERS = ("c:@F@openpete_run", "c:run.c@F@run_split_threaded")
SWITCH_USR = "c:@F@openpete_fiber_switch"
# The functions the statement macros expand to; called only from inside
# one of those macros.
MACRO_TARGETS = re.compile(r"^op_role__(bind|assert|assert_shared)_\w+$|"
                           r"^op_role_(bind|enter)_bits$")
SYNC_CALLEES = ("qsort", "bsearch")
# Calls that start a thread or a fiber on the function they receive; such an
# argument needs a row. Compiler-spelled: SDL_CreateThread expands to
# SDL_CreateThreadRuntime, whose two CRT arguments are not entries.
CREATION_CALLEES = ("SDL_CreateThreadRuntime", "pthread_create", "_beginthreadex", "CreateThread",
                    "openpete_fiber_create_guarded")
CREATION_FORM_ARGS = ("_beginthreadex", "_endthreadex")
THREAD_PROP = "prop:SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER"
# The functions that may call exit(): the guest's exit on the game fiber
# (holding sim) and the two dump modes main runs before openpete_run.
EXIT_CALLERS = ("psyq_exit", "openpete_surface_dump_run_if_requested",
                "tooltip_vocab_run_if_requested")
MARKER = re.compile(r"thread-roles:\s*\S")
IDENT = re.compile(r"[A-Za-z_]\w*")


# --------------------------------------------------------------------------
# The tables and the text-level scan
# --------------------------------------------------------------------------
class Tables:
    """The roles header's tables and the tree's annotated declarations,
    read from text."""

    def __init__(self, root, files):
        hdr = read_text(os.path.join(root, ROLES_H))
        self.roles = re.findall(r"^\s*X\((\w+),\s*\d+\)", hdr, re.M)
        self.rows = {}
        for usr, form, roles in re.findall(
                r'ROW\("([^"]+)",\s*"([^"]+)",\s*"([^"]+)"\)', hdr):
            self.rows[usr] = (form, set(roles.split("|")))
        self.seal_writers = {}
        for seal, usr in re.findall(r'SEAL\((\w+),\s*"([^"]+)"\)', hdr):
            self.seal_writers.setdefault(seal, set()).add(usr)
        self.accessors = set(re.findall(r'ACCESSOR\("([^"]+)"\)', hdr))
        self.escapes = dict(re.findall(r'ESCAPE\("([^"]+)",\s*"([^"]+)"\)', hdr))
        # name -> seal, over every declaration that carries OP_SEALED
        self.sealed = {}
        self.sealed_files = set()
        self.candidates = set()     # names of possibly role-annotated functions
        self.slot_names = set()     # slot declarator names (fields, variables, parameters)
        self.slot_walk = set()      # the names a TU spells to store into or call a slot
        self.nested = []            # (file, line, macro) role macro in a #define
        self.guarded = 0            # declarations carrying an OP_GUARDED_BY form
        for rel in files:
            if rel == ROLES_H:
                continue
            try:
                text = read_text(os.path.join(root, rel))
            except OSError:
                continue
            if "OP_" not in text:
                continue
            self._scan(rel, text)
        self.candidates |= {usr_name(u) for u in self.rows}

    def _scan(self, rel, text):
        text = strip_comments(text)
        for m in re.finditer(r"\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*OP_SEALED\((\w+)\)", text):
            line = text[text.rfind("\n", 0, m.start()) + 1:m.start()]
            if line.lstrip().startswith("#"):
                continue
            static = re.match(r"\s*static\b", line) is not None
            self.sealed[(rel if static else None, m.group(1))] = m.group(2)
            self.sealed_files.add(rel)
        self.guarded += len(re.findall(r"\bOP_(?:PT_)?GUARDED_BY(?:_CAP)?\(", text))
        for m in re.finditer(r"\bOP_SLOT\(\w+\)", text):
            # The slot's declarator name; for a parameter slot also the
            # function whose parameter it is, the name its callers spell.
            stop = max(text.rfind(c, 0, m.start()) for c in ";}{")
            decl = text[stop + 1:m.start()]
            # `T name OP_SLOT` or a function-pointer declarator `T (*name)(...) OP_SLOT`
            ptr = re.findall(r"\(\s*\*\s*([A-Za-z_]\w*)\s*\)", decl)
            last = re.search(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$", decl)
            name = last.group(1) if last else (ptr[-1] if ptr else "")
            if not name:
                continue
            self.slot_names.add(name)
            depth, opened = 0, None
            for k in range(len(decl) - 1, -1, -1):
                if decl[k] == ")":
                    depth += 1
                elif decl[k] == "(":
                    if depth == 0:
                        opened = k
                        break
                    depth -= 1
            if opened is not None:
                fn = re.search(r"([A-Za-z_]\w*)\s*$", decl[:opened])
                if fn:
                    self.slot_walk.add(fn.group(1))
            else:
                self.slot_walk.add(name)
        for m in re.finditer(r"\b(%s)\(" % "|".join(ATTR_MACROS), text):
            stop = max(text.rfind(c, 0, m.start()) for c in ";}{")
            stmt = text[stop + 1:m.start()]
            self.candidates |= set(re.findall(r"([A-Za-z_]\w*)\s*\(", stmt))
        for m in re.finditer(r"^[ \t]*#[ \t]*define\b(?:.*\\\n)*.*", text, re.M):
            for mm in re.finditer(r"\b(%s)\s*\(" % "|".join(STMT_MACROS), m.group(0)):
                line = text.count("\n", 0, m.start() + mm.start()) + 1
                self.nested.append((rel, line, mm.group(1)))


def read_text(path):
    with open(path, errors="replace") as fh:
        return fh.read()


COMMENT_OR_STRING = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|/\*.*?\*/|//[^\n]*', re.S)


def strip_comments(text):
    """text with every comment replaced by blanks (newlines kept, so line
    numbers hold); string and character literals are left as they are."""
    def blank(m):
        t = m.group(0)
        if t.startswith(("/*", "//")):
            return re.sub(r"[^\n]", " ", t)
        return t
    return COMMENT_OR_STRING.sub(blank, text)


USR_FN = re.compile(r"@F@([^#@]+)")


def usr_name(usr):
    """-> the function name a USR spells: the identifier after its `@F@`,
    without the C++ signature suffix (`c:@F@f#*I#` -> `f`; a parameter type
    may spell `@`, so the last `@` is not the name's start); a USR with no
    `@F@` gives its last `@` component."""
    m = USR_FN.search(usr)
    return m.group(1) if m else usr.rsplit("@", 1)[-1]


def row_forms(row):
    """-> the set of registration forms of a row, joined by `|`. `direct:`
    declares a row its callers call directly (an inline job body)."""
    return set(row[0].split("|"))


# --------------------------------------------------------------------------
# Compile databases
# --------------------------------------------------------------------------
def load_entries(root, path, mingw):
    """-> {rel: [flags]} of the in-scope sources, one flag list per distinct
    (file, flags) entry. An entry whose source is absent from the tree is
    skipped with a STALE-DB line: it holds no code to analyse, and a staged
    or present source always has its own file."""
    out = {}
    absent = []
    for rel, e in lib.db_entries(root, path, FENCE, absent):
        flags = entry_flags(e)
        flags = [t for t in flags if not t.startswith(("-fdiagnostics-color",
                                                       "-fmax-errors"))]
        if mingw:
            flags = [MINGW_TARGET] + flags
        lst = out.setdefault(rel, [])
        if (e["directory"], flags) not in lst:
            lst.append((e["directory"], flags))
    if absent:
        say(lib.stale_db_text(FENCE, "windows-mingw" if mingw else "linux-release", absent,
                              lib.MINGW_CONFIGURE if mingw else lib.CONFIGURE))
    return out


def mingw_db(root):
    return lib.mingw_db(root, FENCE, "OPENPETE_THREAD_ROLES_MINGW_DB")


# --------------------------------------------------------------------------
# Per-TU analysis (worker process)
# --------------------------------------------------------------------------
_CI = None


def ci():
    global _CI
    if _CI is None:
        _CI = lib.libclang()
    return _CI


def lang_of(rel, flags):
    if rel.endswith((".cpp", ".cc", ".cxx")):
        return "c++"
    return "c"


_CXX_INCLUDES = {}


def cxx_includes(mingw):
    """-> the C++ standard library directories of the build's compiler (g++
    or the mingw g++), which libclang 14 does not find on its own."""
    if mingw not in _CXX_INCLUDES:
        cxx = "x86_64-w64-mingw32-g++-posix" if mingw else "g++"
        try:
            p = subprocess.run([cxx, "-x", "c++", "-E", "-v", "-"], input="",
                               capture_output=True, text=True)
            text = p.stderr
        except OSError:
            text = ""
        block = text.split("#include <...> search starts here:")[-1].split("End of search list.")[0]
        _CXX_INCLUDES[mingw] = [os.path.normpath(d.strip()) for d in block.splitlines()
                                if "/c++" in d]
    return _CXX_INCLUDES[mingw]


def macro_args(cur):
    toks = [t.spelling for t in cur.get_tokens()]
    if len(toks) >= 3 and toks[1] == "(":
        return toks[2:-1] if toks[-1] == ")" else toks[2:]
    return []


def annotations(cur):
    out = []
    for ch in cur.get_children():
        if ch.kind == ci().CursorKind.ANNOTATE_ATTR:
            out.append(ch.spelling)
    return out


def slot_role(cur):
    for a in annotations(cur):
        if a.startswith("op_slot:"):
            return a.split(":", 1)[1]
    return None


class Walk:
    """One TU's libclang pass: role macros, declarations, references."""

    def __init__(self, ctx, rel, directory, flags):
        self.ctx = ctx
        self.rel = rel
        self.main = os.path.normpath(os.path.join(ctx["root"], rel))
        self.refusals = []
        self.decl_attrs = {}      # usr -> [(file:line, attrs tuple)]
        self.rows_defined = set()
        self.kinds = {}           # reference kind -> count
        self.edits = []           # (line, col_start, col_end, replacement)
        self.helpers = {}         # (line, col) of the enclosing definition -> [text]
        self.helper_names = {}    # helper -> original name
        self.annotated = {}       # usr -> attrs tuple
        self.slot_stores = []     # (usr, name, slot roles) of every slot store
        self.func_refs = []       # survey: every non-call function reference
        self.row_refs = set()     # rows referenced through a form they declare
        self.row_macros = set()   # rows whose body holds a role macro
        self.n = 0
        K = ci().CursorKind
        args = list(flags) + ["-Wno-everything"]
        if MINGW_TARGET in flags:
            # libclang 14 finds its builtin headers (stdbool.h, stdarg.h) for
            # the host target only; name its resource directory for mingw.
            args += ["-resource-dir", LIBCLANG_RESOURCE]
        if lang_of(rel, flags) == "c++":
            for d in cxx_includes(MINGW_TARGET in flags):
                args += ["-isystem", d]
        self.tu = ci().Index.create().parse(
            self.main, args=args, unsaved_files=ctx.get("unsaved"),
            options=ci().TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD)
        # A libclang error leaves holes in the AST the walk reads: VACUOUS.
        self.parse_errors = ["%s (libclang 14)" % d for d in self.tu.diagnostics
                             if d.severity >= 3][:3]
        self.macros = []          # (file, line, col, name, args, extent end)
        self.defs = []            # main-file definitions: (start, end, cursor)
        self.fn_by_usr = {}
        tops = list(self.tu.cursor.get_children())
        for c in tops:
            f = c.location.file
            if c.kind == K.MACRO_INSTANTIATION:
                if c.spelling in ATTR_MACROS + STMT_MACROS + (NTSA, "OP_SEALED",
                                                              "OP_TSA_SUPPRESS_BEGIN"):
                    self.macros.append((f.name if f else "", c.location.line,
                                        c.location.column, c.spelling, macro_args(c),
                                        (c.extent.end.line, c.extent.end.column)))
        self.macros_by_file = {}
        for m in self.macros:
            self.macros_by_file.setdefault(m[0], []).append(m)
        self.stack = list(tops)
        self._collect_decls(tops)

    # ---- declarations --------------------------------------------------
    def _attrs_of(self, c):
        """Role attribute macros written on this declaration (before the body)."""
        f = c.location.file
        if not f:
            return ()
        end = (c.extent.end.line, c.extent.end.column)
        for ch in c.get_children():
            if ch.kind == ci().CursorKind.COMPOUND_STMT:
                end = (ch.extent.start.line, ch.extent.start.column)
        start = (c.extent.start.line, c.extent.start.column)
        out = []
        for m in self.macros_by_file.get(f.name, ()):
            if m[3] in ATTR_MACROS + (NTSA,) and start <= (m[1], m[2]) <= end:
                out.append("%s(%s)" % (m[3], "".join(m[4])) if m[3] != NTSA else NTSA)
        return tuple(sorted(out))

    def _collect_decls(self, tops):
        K = ci().CursorKind
        fn = (K.FUNCTION_DECL, K.CXX_METHOD)
        stack = list(tops)
        while stack:
            c = stack.pop()
            if c.kind in (K.UNEXPOSED_DECL, K.LINKAGE_SPEC, K.NAMESPACE):
                stack.extend(c.get_children())
                continue
            f = c.location.file
            if not f:
                continue
            if c.kind in fn:
                usr = c.get_usr()
                attrs = self._attrs_of(c)
                rel = os.path.relpath(f.name, self.ctx["root"])
                # Every declaration is kept: one without attributes beside an
                # annotated one is the mismatch the report refuses.
                if attrs or FENCE.in_scope(rel):
                    self.decl_attrs.setdefault(usr, []).append(
                        ("%s:%d" % (rel, c.location.line), attrs))
                if attrs:
                    self.annotated[usr] = attrs
                if c.is_definition():
                    if f.name == self.main:
                        self.defs.append(((c.extent.start.line, c.extent.start.column),
                                          (c.extent.end.line, c.extent.end.column), c))
                    if usr in self.ctx["rows"] or usr in self.ctx["escapes"] \
                            or usr in self.ctx["accessors"]:
                        self.rows_defined.add(usr)
                    if NTSA in attrs and usr not in self.ctx["accessors"] \
                            and rel != RUNTIME_C:
                        self.refuse(c, "%s on %s, which OP_TSA_ACCESSOR_TABLE does "
                                    "not list" % (NTSA, c.spelling))
            elif c.kind == K.VAR_DECL:
                rel = os.path.relpath(f.name, self.ctx["root"])
                seal = self.ctx["sealed"].get((rel, c.spelling)) \
                    or (self.ctx["sealed"].get((None, c.spelling))
                        if c.linkage == ci().LinkageKind.EXTERNAL else None)
                if seal and FENCE.in_scope(rel) and not any(
                        m[3] == "OP_SEALED" and m[1] == c.location.line
                        and m[2] > c.location.column
                        for m in self.macros_by_file.get(f.name, ())):
                    self.refuse(c, "declaration of the sealed global %s without "
                                "OP_SEALED(%s)" % (c.spelling, seal))

    def refuse(self, c, msg):
        f = c.location.file
        where = "%s:%d" % (os.path.relpath(f.name, self.ctx["root"]) if f else self.rel,
                           c.location.line)
        self.refusals.append("%s: %s" % (where, msg))

    def enclosing_def(self, line, col):
        best = None
        for s, e, c in self.defs:
            if s <= (line, col) <= e:
                best = (s, e, c)
        return best

    # ---- statement macros ---------------------------------------------
    def check_role_macros(self):
        for f, line, col, name, args, _ in self.macros:
            if f != self.main:
                continue
            if name == "OP_TSA_SUPPRESS_BEGIN":
                if not args or not re.match(r'^"[^"]+"$', "".join(args)):
                    self.refusals.append("%s:%d: OP_TSA_SUPPRESS_BEGIN needs a non-empty "
                                         "string reason" % (self.rel, line))
                continue
            if name not in STMT_MACROS:
                continue
            role = args[0] if args else "?"
            enc = self.enclosing_def(line, col)
            if not enc:
                self.refusals.append("%s:%d: %s(%s) outside a function"
                                     % (self.rel, line, name, role))
                continue
            usr = enc[2].get_usr()
            row = self.ctx["rows"].get(usr)
            if row:
                self.row_macros.add(usr)
            if not row:
                self.refusals.append(
                    "%s:%d: %s(%s) in %s, which OP_ROLE_BINDING_TABLE does not list "
                    "(%s)" % (self.rel, line, name, role, enc[2].spelling, usr))
            elif role not in row[1]:
                self.refusals.append(
                    "%s:%d: %s(%s) in %s, whose row allows %s"
                    % (self.rel, line, name, role, enc[2].spelling, "|".join(sorted(row[1]))))
        for _, _, c in self.defs:
            usr = c.get_usr()
            if usr in self.ctx["rows"] and usr not in self.row_macros:
                self.refuse(c, "%s has a binding-table row and no role macro in its body"
                            % c.spelling)

    # ---- the body walk --------------------------------------------------
    def walk(self):
        K = ci().CursorKind
        stmt_macro_at = {(m[1], m[2]) for m in self.macros
                         if m[0] == self.main and m[3] in STMT_MACROS}
        for top in self.stack:
            f = top.location.file
            if not f or f.name != self.main:
                continue
            path = []
            self._visit(top, path, stmt_macro_at)

    def _visit(self, root_cur, path, stmt_macro_at):
        K = ci().CursorKind
        todo = [(root_cur, 0)]
        chain = []
        while todo:
            c, depth = todo.pop()
            # The walk covers the main file's own text. A declaration an
            # #include brings in inside a main-file block (`extern "C" {
            # #include ... }`, a namespace) is pruned as one included at file
            # scope is never visited: its prototypes are not local and its
            # inline bodies are not this TU's references.
            f = c.location.file
            if depth > 0 and f is not None and f.name != self.main:
                continue
            del chain[depth:]
            chain.append(c)
            kind = c.kind
            if kind in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
                self._ref(c, chain, stmt_macro_at)
            elif kind == K.CALL_EXPR:
                self._call(c, chain, stmt_macro_at)
            elif kind == K.FUNCTION_DECL and depth > 0 and not c.is_definition():
                # A definition inside `extern "C" {` is the canonical site,
                # not a prototype.
                usr = c.get_usr()
                if usr in self.annotated:
                    self.refuse(c, "local prototype of the annotated function %s" % c.spelling)
            kids = list(c.get_children())
            for k in reversed(kids):
                todo.append((k, depth + 1))

    def _use(self, chain):
        """-> (index of the first ancestor that is not a cast, paren or
        address-of, that ancestor)."""
        K = ci().CursorKind
        skip = (K.UNEXPOSED_EXPR, K.PAREN_EXPR, K.CSTYLE_CAST_EXPR,
                K.CXX_FUNCTIONAL_CAST_EXPR, K.CXX_STATIC_CAST_EXPR,
                K.CXX_REINTERPRET_CAST_EXPR)
        i = len(chain) - 2
        while i >= 0:
            a = chain[i]
            if a.kind in skip:
                i -= 1
                continue
            if a.kind == K.UNARY_OPERATOR:
                toks = list(a.get_tokens())
                if toks and toks[0].spelling == "&":
                    i -= 1
                    continue
            return i, a
        return -1, None

    def _child_index(self, parent, child):
        for i, ch in enumerate(parent.get_children()):
            if ch == child:
                return i
        return -1

    def _binop(self, c):
        kids = list(c.get_children())
        if len(kids) != 2:
            return None
        end = kids[0].extent.end.offset
        for t in c.get_tokens():
            if t.extent.start.offset >= end:
                return t.spelling
        return None

    def _call(self, c, chain, stmt_macro_at):
        ref = c.referenced
        if ref is None:
            self._slot_call(c)
            return
        name = ref.spelling
        enc = self.enclosing_def(c.location.line, c.location.column)
        enc_usr = enc[2].get_usr() if enc else ""
        if name in BOOT_FNS and enc_usr not in BOOT_CALLERS:
            self.refuse(c, "call of the boot function %s outside openpete_run and "
                        "run_split_threaded" % name)
        if name == "co_switch" and enc_usr != SWITCH_USR:
            self.refuse(c, "co_switch outside openpete_fiber_switch")
        if name == "exit" and not self.rel.startswith("tests/") and (
                enc is None or enc[2].spelling not in EXIT_CALLERS):
            self.refuse(c, "exit() outside %s: an atexit handler asserts sim, which holds only "
                           "for these callers" % ", ".join(EXIT_CALLERS))
        row = self.ctx["rows"].get(ref.get_usr())
        if row is not None:
            if "direct:" in row_forms(row):
                self.row_refs.add(ref.get_usr())
            elif not self.rel.startswith("tests/"):
                self.refuse(c, "direct call of the binding-table entry %s, whose row lists %s; "
                               "declare `direct:` on the row" % (name, row[0]))
        if MACRO_TARGETS.match(name) and self.rel != RUNTIME_C \
                and (c.location.line, c.location.column) not in stmt_macro_at:
            self.refuse(c, "direct call of %s; write the OP_ROLE_* macro" % name)
        self._slot_call(c)

    def _slot_call(self, c):
        """A call through an OP_SLOT field, variable or parameter: rewrite the
        callee as a helper call requiring the slot's role."""
        K = ci().CursorKind
        kids = list(c.get_children())
        if not kids:
            return
        callee = kids[0]
        while callee.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR) and list(callee.get_children()):
            callee = list(callee.get_children())[0]
        if callee.kind not in (K.DECL_REF_EXPR, K.MEMBER_REF_EXPR):
            return
        tgt = callee.referenced
        if tgt is None or tgt.kind == K.FUNCTION_DECL:
            return
        role = slot_role(tgt)
        if not role:
            return
        self.kinds["slot-call"] = self.kinds.get("slot-call", 0) + 1
        self._rewrite(callee, "op__slot_%s" % tgt.spelling, ("OP_REQUIRES(%s)" % role,),
                      tgt.spelling)

    def _rewrite(self, expr, base, attrs, original):
        s, e = expr.extent.start, expr.extent.end
        if not s.file or s.file.name != self.main or s.line != e.line \
                or s.offset >= e.offset:
            self.refusals.append("%s:%d: the reference to %s lies in a macro expansion or "
                                 "spans lines; it cannot be checked as a call"
                                 % (self.rel, s.line, original))
            return
        enc = self.enclosing_def(s.line, s.column)
        if not enc:
            self.refusals.append("%s:%d: the reference to %s is outside a function"
                                 % (self.rel, s.line, original))
            return
        self.n += 1
        helper = "%s_L%d_%d" % (base, s.line, self.n)
        self.helper_names[helper] = original
        text = read_text(self.main).split("\n")[s.line - 1]
        span = text[s.column - 1:e.column - 1]
        self.edits.append((s.line, s.column - 1, e.column - 1,
                           "(%s(), %s)" % (helper, span)))
        at = (enc[0][0], enc[0][1])
        self.helpers.setdefault(at, []).append(
            "static inline void %s(void) %s {} " % (helper, " ".join(attrs)))

    def _ref(self, c, chain, stmt_macro_at):
        K = ci().CursorKind
        tgt = c.referenced
        if tgt is not None and tgt.kind in (K.VAR_DECL, K.PARM_DECL, K.FIELD_DECL) \
                and tgt.spelling in self.ctx["slot_names"]:
            role = slot_role(tgt)
            if role:
                self._slot_read(c, chain, role)
            return
        if tgt is None or tgt.kind not in (K.FUNCTION_DECL, K.CXX_METHOD):
            return
        i, use = self._use(chain)
        if use is not None and use.kind == K.CALL_EXPR \
                and self._child_index(use, chain[i + 1]) == 0:
            return   # a call; _call handles it
        name = tgt.spelling
        usr = tgt.get_usr()
        attrs = self.annotated.get(usr) or ()
        internal = name in BOOT_FNS or MACRO_TARGETS.match(name) \
            or name in ("op_role_leave", "op_role_check_at")
        row = self.ctx["rows"].get(usr)
        self._creation_arg(c, chain, i, use, usr, name, row)
        if self.ctx.get("collect_refs"):
            kind, form, slot = self._classify(c, chain, i, use)
            self.func_refs.append((usr, name, kind, tuple(sorted(form)), slot,
                                   self.rel, c.location.line))
        if not (attrs or row or internal or name == "co_switch"):
            # A slot store is recorded so the annotator never gives the
            # function a role the slot does not hold.
            _, _, slot = self._classify(c, chain, i, use)
            if slot is not None:
                self.slot_stores.append((usr, name, slot))
            return
        if internal or name == "co_switch":
            self.refuse(c, "non-call reference to %s" % name)
            return
        kind, form, slot = self._classify(c, chain, i, use)
        if kind == "test":
            self._count("test")
            return
        if row:
            if attrs:
                self.refuse(c, "%s is a binding-table entry and carries %s; an entry "
                            "asserts or binds its role instead" % (name, ", ".join(attrs)))
            if row_forms(row) & form:
                self._count("entry")
                self.row_refs.add(usr)
            else:
                self.refuse(c, "%s is registered through %s; its row lists %s"
                            % (name, ", ".join(sorted(form)) or "an unlisted form", row[0]))
            return
        if kind == "sync":
            self._count("sync")
            self._rewrite(c, "op__ref_%s" % name, attrs, name)
            return
        if slot is not None:
            self.slot_stores.append((usr, name, slot))
            need = self._roles_of(attrs)
            if need is None or not need <= {slot}:
                self.refuse(c, "%s (%s) stored into an OP_SLOT(%s) slot"
                            % (name, ", ".join(attrs), slot))
            else:
                self._count("slot")
            return
        self.refuse(c, "non-call reference to the role-annotated %s (%s) is neither "
                    "synchronous, a slot store, a binding-table form nor a test"
                    % (name, ", ".join(attrs)))

    def _creation_arg(self, c, chain, i, use, usr, name, row):
        """A function handed to a thread or fiber creation needs a row."""
        K = ci().CursorKind
        if row is not None or use is None or use.kind != K.CALL_EXPR \
                or self.rel.startswith("tests/") or name in CREATION_FORM_ARGS:
            return
        callee = use.referenced
        if callee is None or callee.spelling not in CREATION_CALLEES + ("SDL_SetPointerProperty",):
            return
        forms = self._classify(c, chain, i, use)[1]
        if callee.spelling in CREATION_CALLEES or THREAD_PROP in forms:
            self.refuse(c, "%s starts a thread or fiber and has no binding-table row"
                        % name)

    def _slot_read(self, c, chain, role):
        """A read of an OP_SLOT slot: a call (rewritten by _slot_call), a
        test, or a store into a slot whose roles hold the source's."""
        K = ci().CursorKind
        i, use = self._use(chain)
        if use is not None and use.kind == K.CALL_EXPR \
                and self._child_index(use, chain[i + 1]) == 0:
            return
        if use is not None and use.kind == K.BINARY_OPERATOR and self._binop(use) == "=" \
                and self._child_index(use, chain[i + 1]) == 0:
            return   # the slot is the store's target
        kind, _, slot = self._classify(c, chain, i, use)
        if kind == "test":
            self._count("slot-test")
        elif slot is not None and set(role.split("|")) <= set(slot.split("|")):
            self._count("slot-copy")
        else:
            self.refuse(c, "read of the OP_SLOT(%s) slot %s is neither a call, a test nor "
                        "a store into a slot holding its roles" % (role, c.spelling))

    def _count(self, k):
        self.kinds[k] = self.kinds.get(k, 0) + 1

    def _roles_of(self, attrs):
        out = set()
        for a in attrs:
            m = re.match(r"(OP_REQUIRES|OP_REQUIRES_SHARED)\((\w+)\)$", a)
            if not m:
                return None
            out.add(m.group(2))
        return out

    def _classify(self, c, chain, i, use):
        """-> (kind, set of registration forms, slot role or None)."""
        K = ci().CursorKind
        forms = set()
        if use is None:
            return "other", forms, None
        if use.kind == K.UNARY_OPERATOR:
            toks = list(use.get_tokens())
            if toks and toks[0].spelling == "!":
                return "test", forms, None
        if use.kind == K.BINARY_OPERATOR:
            op = self._binop(use)
            if op in ("==", "!=", "&&", "||"):
                return "test", forms, None
            if op == "=":
                lhs = list(use.get_children())[0]
                while lhs.kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR) and list(lhs.get_children()):
                    lhs = list(lhs.get_children())[0]
                tgt = lhs.referenced if lhs.kind in (K.MEMBER_REF_EXPR, K.DECL_REF_EXPR) else None
                if tgt is not None:
                    if tgt.kind == K.FIELD_DECL:
                        forms.add("field:%s.%s" % (record_name(tgt.semantic_parent),
                                                   tgt.spelling))
                    return "store", forms, slot_role(tgt)
        if use.kind in (K.IF_STMT, K.WHILE_STMT, K.CONDITIONAL_OPERATOR) \
                and self._child_index(use, chain[i + 1]) == 0:
            return "test", forms, None
        if use.kind == K.CALL_EXPR:
            idx = self._child_index(use, chain[i + 1]) - 1
            callee = use.referenced
            cname = callee.spelling if callee is not None else ""
            forms.add("arg:%s" % cname)
            if cname == "SDL_SetPointerProperty" and idx == 2:
                forms.add("prop:%s" % "".join(call_args_text(use)[1:2]))
            if cname in SYNC_CALLEES:
                return "sync", forms, None
            if callee is not None:
                params = list(callee.get_arguments())
                if 0 <= idx < len(params):
                    ann = annotations(params[idx])
                    if "op_sync_fn" in ann:
                        return "sync", forms, None
                    role = slot_role(params[idx])
                    if role:
                        return "store", forms, role
            return "arg", forms, None
        if use.kind == K.VAR_DECL:
            return "init", forms, slot_role(use)
        if use.kind == K.INIT_LIST_EXPR:
            forms.add("init:%s" % type_name(use.type))
            field = self._init_field(use, chain[i + 1])
            if field is not None:
                return "init", forms, slot_role(field)
            return "init", forms, None
        return "other", forms, None

    def _init_field(self, lst, child):
        K = ci().CursorKind
        kids = list(lst.get_children())
        for ch in child.get_children():
            if ch.kind == K.MEMBER_REF:
                return ch.referenced
        idx = self._child_index(lst, child)
        try:
            fields = list(lst.type.get_canonical().get_fields())
        except Exception:
            return None
        if 0 <= idx < len(fields) and len(kids) <= len(fields):
            return fields[idx]
        return None


def call_args_text(call):
    """-> the source spelling of each argument of a call, split at the
    top-level commas of its token stream (macro arguments stay unexpanded)."""
    toks = [t.spelling for t in call.get_tokens()]
    if "(" not in toks:
        return []
    out, cur, depth = [], [], 0
    for t in toks[toks.index("(") + 1:]:
        if t in "([{":
            depth += 1
        elif t in ")]}":
            if depth == 0:
                break
            depth -= 1
        if t == "," and depth == 0:
            out.append("".join(cur))
            cur = []
        else:
            cur.append(t)
    out.append("".join(cur))
    return out


def record_name(c):
    n = c.spelling or c.type.spelling
    return re.sub(r"^(struct|union|const)\s+", "", n)


def type_name(t):
    n = t.spelling
    n = re.sub(r"\b(const|volatile|struct|union)\s+", "", n)
    return n.strip()


def sealed_names(sealed, rel):
    """The sealed names visible in rel: every external one, and rel's own
    statics."""
    return sorted({n for (f, n) in sealed if f is None or f == rel})


def rewrite_sealed(text, sealed, writers_spans, lang):
    """Line-preserving: `const ` before each OP_SEALED declarator, and the
    sealed names inside the listed writers' bodies cast back to writable. In
    C++ a const definition needs an initializer: a non-extern declarator
    with none (`;` or `,` follows the macro) gets `{}`, the value
    initialization its static storage already gives it."""
    lines = text.split("\n")
    pat = re.compile(r"\b([A-Za-z_]\w*)(\s*(?:\[[^\]]*\]\s*)*)OP_SEALED\((\w+)\)")

    def sub(line, m):
        init = ""
        if lang == "c++" and re.match(r"\s*[;,]", line[m.end():]) \
                and not re.search(r"\bextern\b", line[:m.start()]):
            init = "{}"
        return "const %s%sOP_SEALED(%s)%s" % (m.group(1), m.group(2), m.group(3), init)
    for n, line in enumerate(lines):
        if "OP_SEALED(" in line and not line.lstrip().startswith("#"):
            lines[n] = pat.sub(lambda m, line=line: sub(line, m), line)
    if writers_spans:
        names = list(sealed)
        if names:
            npat = re.compile(r"(?<![.>\w])(?<!op_seal_check\()(?<!op_seal\()(%s)\b"
                              % "|".join(map(re.escape, names)))
            for (l0, l1) in writers_spans:
                for n in range(l0 - 1, min(l1, len(lines))):
                    if "OP_SEALED(" in lines[n]:
                        continue
                    if lang == "c":
                        rep = r"(*(__typeof_unqual__(\1)*)(__UINTPTR_TYPE__)&(\1))"
                    else:
                        rep = r"(const_cast<__remove_const(decltype(\1))&>(\1))"
                    lines[n] = npat.sub(rep, lines[n])
    return "\n".join(lines)


def writer_spans(text, writer_names):
    """-> [(first line, last line)] of the bodies of the named functions'
    definitions in text (a brace-matching scan that skips comments, strings
    and character literals)."""
    spans = []
    if not writer_names:
        return spans
    for m in re.finditer(r"\b(%s)\s*\(" % "|".join(map(re.escape, writer_names)), text):
        # the definition: the parameter list closes, then '{' before any ';'
        i, depth = m.end(), 1
        while i < len(text) and depth:
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        j = i
        while j < len(text) and text[j] not in "{;":
            j += 1
        if j >= len(text) or text[j] != "{":
            continue
        line_start = text.count("\n", 0, m.start())
        # the declaration's own text: from the last line, statement or
        # brace boundary (a definition inside `namespace {` on one line)
        prefix = text[max(text.rfind(c, 0, m.start()) for c in "\n;{}") + 1:m.start()]
        if not re.fullmatch(r"\s*(?:[A-Za-z_]\w*(?:::|[\s*&]+))*", prefix) \
                or re.search(r"\b(return|if|while|else|case)\b", prefix):
            continue
        k, depth = j + 1, 1
        while k < len(text) and depth:
            ch = text[k]
            if text.startswith("/*", k):
                k = text.find("*/", k + 2) + 2 or len(text)
                continue
            if text.startswith("//", k):
                k = text.find("\n", k) if text.find("\n", k) >= 0 else len(text)
                continue
            if ch in "\"'":
                q, k = ch, k + 1
                while k < len(text) and text[k] != q:
                    k += 2 if text[k] == "\\" else 1
            elif ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
            k += 1
        spans.append((line_start + 1, text.count("\n", 0, k) + 1))
    return spans


def apply_edits(text, edits, helpers):
    lines = text.split("\n")
    by_line = {}
    for line, a, b, rep in edits:
        by_line.setdefault(line, []).append((a, b, rep))
    for (line, col), hs in helpers.items():
        by_line.setdefault(line, []).append((col - 1, col - 1, "".join(hs)))
    for line, es in by_line.items():
        s = lines[line - 1]
        for a, b, rep in sorted(es, key=lambda x: (x[0], x[1]), reverse=True):
            s = s[:a] + rep + s[b:]
        lines[line - 1] = s
    return "\n".join(lines)


def overlay_yaml(pairs):
    roots = []
    for orig, repl in pairs:
        roots.append({"name": os.path.dirname(orig), "type": "directory",
                      "contents": [{"name": os.path.basename(orig), "type": "file",
                                    "external-contents": repl}]})
    return json.dumps({"version": 0, "use-external-names": False, "roots": roots})


# Errors the const rewrite produces at a write site (C and C++ spellings).
SEAL_ERR = re.compile(r"const-qualified|read-only|discards qualifiers|drops '?const|"
                      r"cast from .* drops|cannot (initialize|bind)|no matching")


def analyse(task):
    """Worker: one (rel, db name, directory, flags) entry. -> result dict."""
    ctx, rel, dbname, directory, flags = task
    t0 = time.time()
    res = {"rel": rel, "db": dbname, "refusals": [], "parse": [], "decl_attrs": {},
           "rows_defined": [], "kinds": {}, "walked": False, "rewritten": False,
           "tsa": 0, "seal": 0, "diags": [], "slot_stores": []}
    root = ctx["root"]
    main = os.path.join(root, rel)
    text = read_text(main)
    lang = lang_of(rel, flags)
    tmp = tempfile.mkdtemp(prefix="thread-roles-", dir=ctx["tmp"])
    pairs = []
    rewritten_main = None
    c2 = ctx.get("c2")
    scan = c2 is not None and rel in c2["tus"]
    need_walk = (scan or ctx["walk_re"].search(text) is not None
                 or rel in ctx["sealed_files"] or rel in ctx["writer_files"]
                 or (ctx["redecl_re"] is not None and ctx["redecl_re"].search(text)))
    if need_walk:
        try:
            w = Walk(ctx, rel, directory, flags)
        except Exception as e:   # libclang could not parse at all
            res["parse"].append("%s: libclang parse failed: %s" % (rel, e))
            return res
        res["walked"] = True
        res["parse"] += w.parse_errors
        w.check_role_macros()
        w.walk()
        res["refusals"] += w.refusals
        res["decl_attrs"] = w.decl_attrs
        res["rows_defined"] = sorted(w.rows_defined)
        res["row_refs"] = sorted(w.row_refs)
        res["kinds"] = w.kinds
        res["slot_stores"] = w.slot_stores
        if scan:
            res["c2"] = pop.scan_tu(ci(), w.tu, c2, ctx, FENCE, reader(ctx))
        if (w.edits or w.helpers) and not ctx["no_rewrite"]:
            rewritten_main = apply_edits(text, w.edits, w.helpers)
        helper_names = w.helper_names
    else:
        helper_names = {}
    # Sealed declarations: the main file and every header that declares one.
    for f in ctx["sealed_files"] | ({rel} if rewritten_main is not None else set()):
        path = os.path.join(root, f)
        body = rewritten_main if (f == rel and rewritten_main is not None) \
            else read_text(path)
        spans = writer_spans(body, ctx["writer_names"]) if f == rel else []
        new = rewrite_sealed(body, sealed_names(ctx["sealed"], rel), spans, lang)
        if f == rel:
            rewritten_main = new
        if new != read_text(path):
            out = os.path.join(tmp, "%d_%s" % (len(pairs), os.path.basename(f)))
            with open(out, "w") as fh:
                fh.write(new)
            pairs.append((os.path.normpath(path), out))
    args = [CLANG[lang]] + list(flags) + CLANG_CHECK
    if pairs:
        y = os.path.join(tmp, "overlay.yaml")
        with open(y, "w") as fh:
            fh.write(overlay_yaml(pairs))
        args += ["-ivfsoverlay", y]
        res["rewritten"] = True
    args.append(main)
    p = subprocess.run(args, cwd=directory, capture_output=True, text=True)
    lines_cache = {}

    def src_line(path, n):
        if path not in lines_cache:
            try:
                lines_cache[path] = read_text(path).split("\n")
            except OSError:
                lines_cache[path] = []
        ls = lines_cache[path]
        return ls[n - 1] if 0 < n <= len(ls) else ""

    visible = sealed_names(ctx["sealed"], rel)
    sealed_re = re.compile(r"(?<![.>\w])(%s)\b" % "|".join(map(re.escape, visible))) \
        if visible else None
    for line in p.stderr.splitlines():
        m = re.match(r"^(.+?):(\d+):(\d+): (error|warning): (.*)$", line)
        if not m:
            if "error:" in line:
                res["parse"].append(line)
            continue
        path, ln, sev, msg = m.group(1), int(m.group(2)), m.group(4), m.group(5)
        path = os.path.normpath(os.path.join(directory, path))
        relp = os.path.relpath(path, root)
        for h, orig in helper_names.items():
            msg = msg.replace("'%s'" % h, "'%s' (referenced here)" % orig)
        if "-Wthread-safety" in msg:
            res["refusals"].append("%s:%d: %s" % (relp, ln, msg))
            res["tsa"] += 1
            res["diags"].append((relp, ln, int(m.group(3)), msg))
        elif sealed_re and sealed_re.search(src_line(path, ln)) and (
                SEAL_ERR.search(msg) or "discards-qualifiers" in msg or "cast-qual" in msg
                or "redeclaration of" in msg):
            res["refusals"].append("%s:%d: write site of a sealed global outside its "
                                   "listed writers: %s" % (relp, ln, msg))
            res["seal"] += 1
        elif sev == "error":
            res["parse"].append("%s:%d: %s" % (relp, ln, msg))
    if p.returncode != 0 and not res["refusals"] and not res["parse"]:
        res["parse"].append("%s: clang-19 exit %d: %s" % (rel, p.returncode,
                                                         p.stderr.strip()[-400:]))
    res["sec"] = time.time() - t0
    return res


def reader(ctx):
    """-> rel -> text, the snapshot's text first (a replayed commit), then
    the working file."""
    snap = ctx.get("snapshot") or {}
    root = ctx["root"]

    def read(rel):
        return snap[rel] if rel in snap else read_text(os.path.join(root, rel))
    return read


_SCAN = None        # (ctx, census or None): inherited by the forked workers


def scan_worker(task):
    """Worker: one (rel, db name, directory, flags) entry, walked only.
    -> {"rel", "db", "parse", "tu"}."""
    rel, dbname, directory, flags = task
    ctx, plan = _SCAN
    res = {"rel": rel, "db": dbname, "parse": [], "tu": None}
    try:
        w = Walk(ctx, rel, directory, flags)
    except Exception as e:
        res["parse"].append("%s: libclang parse failed: %s" % (rel, e))
        return res
    res["parse"] += w.parse_errors
    if ctx.get("collect_refs"):
        w.walk()
        res["tu"] = w.func_refs
    elif plan is None:
        res["tu"] = pop.census_tu(ci(), w.tu, ctx, FENCE, reader(ctx))
    else:
        res["tu"] = pop.scan_tu(ci(), w.tu, plan, ctx, FENCE, reader(ctx))
    return res


def run_scan(root, select, plan, snapshot=None, collect_refs=False):
    """Walk the selected entries without check 1. plan None: the census;
    collect_refs: the survey of non-call function references.
    -> (results, wall seconds)."""
    global _SCAN
    tables = Tables(root, tracked_scope(root))
    ctx = build_ctx(root, tables, None)
    ctx["collect_refs"] = collect_refs
    ctx["snapshot"] = snapshot or {}
    ctx["unsaved"] = [(os.path.join(root, r), t) for r, t in ctx["snapshot"].items()] or None
    _SCAN = (ctx, plan)
    tasks = [(rel, db, d, f) for db, entries in select.items()
             for rel, lst in sorted(entries.items()) for d, f in lst]
    t0 = time.time()
    with concurrent.futures.ProcessPoolExecutor(max_workers=jobs()) as ex:
        results = list(ex.map(scan_worker, tasks, chunksize=1))
    _SCAN = None
    return results, time.time() - t0


# --------------------------------------------------------------------------
# Selection, the run, the report
# --------------------------------------------------------------------------
def tracked_scope(root):
    return sorted(lib.present_scope_files(root, FENCE))


def scope_hashes(root, files):
    out = {}
    for f in files + [ROLES_H, "scripts/fences/thread_roles.py",
                      "scripts/fences/thread_roles_globals.py",
                      "scripts/fences/ratchet_lib.py", pop.UNANNOTATED, pop.DEFERRED]:
        try:
            st = os.stat(os.path.join(root, f))
            out[f] = "%d:%d" % (st.st_mtime_ns, st.st_size)
        except OSError:
            out[f] = "-"
    return out


def build_ctx(root, tables, tmp):
    walk_names = set(tables.candidates) | set(BOOT_FNS) | {"co_switch", "exit"} \
        | set(CREATION_CALLEES) | {"SDL_CreateThread", "SDL_SetPointerProperty"} \
        | set(tables.slot_walk) | set(STMT_MACROS) | {"OP_TSA_SUPPRESS_BEGIN", NTSA}
    walk_names = {n for n in walk_names if n and n not in ("if", "while", "for", "sizeof",
                                                          "return", "switch")}
    writer_usrs = set().union(*tables.seal_writers.values()) if tables.seal_writers else set()
    writer_files = set()
    for u in writer_usrs:
        m = re.match(r"c:([^@]+)@", u)
        if m:
            writer_files.add(m.group(1))
    return {"root": root, "rows": tables.rows, "accessors": tables.accessors,
            "escapes": tables.escapes,
            "sealed": tables.sealed, "sealed_files": set(tables.sealed_files),
            "writer_names": sorted({usr_name(u) for u in writer_usrs}),
            "writer_files_base": writer_files, "writer_files": set(),
            "no_rewrite": os.environ.get("OPENPETE_THREAD_ROLES_NO_REWRITE") == "1",
            "walk_names": sorted(walk_names), "slot_names": set(tables.slot_names),
            "tmp": tmp}


def compile_patterns(ctx):
    ctx["walk_re"] = re.compile(r"\b(%s)\b" % "|".join(map(re.escape, ctx["walk_names"])))
    ext = sorted({n for (f, n) in ctx["sealed"] if f is None})
    ctx["redecl_re"] = (re.compile(r"\bextern\b[^;]*\b(%s)\b" % "|".join(
        map(re.escape, ext))) if ext else None)


def run_analysis(root, select, jobs, label, c2=None):
    """select: {db name: {rel: [(directory, flags)]}} -> results. c2: the
    check 2 scan plan, with its `tus`."""
    files = tracked_scope(root)
    tables = Tables(root, files)
    tmp = tempfile.mkdtemp(prefix="thread-roles-")
    ctx = build_ctx(root, tables, tmp)
    ctx["c2"] = c2
    ctx["writer_files"] = {f for f in files
                           if os.path.basename(f) in ctx["writer_files_base"]}
    compile_patterns(ctx)
    tasks = []
    for dbname, entries in select.items():
        for rel, lst in sorted(entries.items()):
            for directory, flags in lst:
                tasks.append((ctx, rel, dbname, directory, flags))
    results = []
    t0 = time.time()
    if tasks:
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as ex:
            for r in ex.map(analyse, tasks, chunksize=1):
                results.append(r)
    wall = time.time() - t0
    subprocess.run(["rm", "-rf", "--", tmp])
    return tables, ctx, results, wall


def report(root, tables, ctx, results, wall, selected_files, mode):
    refusals, parse = [], []
    per_db = {}
    attrs = {}
    rows_defined = set()
    row_refs = set()
    kinds = {}
    walked = rewritten = tsa = seal = 0
    for r in results:
        per_db.setdefault(r["db"], []).append(r)
        refusals += r["refusals"]
        parse += r["parse"]
        for usr, lst in r["decl_attrs"].items():
            attrs.setdefault(usr, set()).update((w, a) for w, a in lst)
        rows_defined.update(r["rows_defined"])
        row_refs.update(r.get("row_refs", ()))
        for k, v in r["kinds"].items():
            kinds[k] = kinds.get(k, 0) + v
        walked += r["walked"]
        rewritten += r["rewritten"]
        tsa += r["tsa"]
        seal += r["seal"]
    # declarations whose role attributes differ (per USR, over every TU)
    for usr, decls in sorted(attrs.items()):
        sets = {tuple(x for x in a if x != NTSA) for _, a in decls}
        if len(sets) > 1 and any(sets):
            refusals.append("%s: declarations carry different role attributes: %s"
                            % (usr, "; ".join("%s %s" % (w, " ".join(a) or "(none)")
                                              for w, a in sorted(decls))))
    if whole_selection(mode):
        for usr in sorted(set(tables.rows) - row_refs):
            refusals.append("%s: a binding-table row with no reference of its form %s in the "
                            "tree" % (usr, tables.rows[usr][0]))
    for rel, line, mac in tables.nested:
        if rel in selected_files:
            refusals.append("%s:%d: %s inside a macro body; role macros are written at "
                            "their site" % (rel, line, mac))
    refusals = sorted(set(refusals))
    say("thread-roles: %s, %.1f s wall" % (mode, wall))
    for db in sorted(per_db):
        rs = per_db[db]
        cpu = sum(r.get("sec", 0) for r in rs)
        say("  %-6s TUs analysed %d (%d distinct files), CPU %.1f s, %.2f s per entry"
            % (db, len(rs), len({r["rel"] for r in rs}), cpu, cpu / len(rs)))
    n_annot_fns = sum(1 for usr, d in attrs.items() if any(a for _, a in d))
    say("  check 1: annotated functions seen %d, annotated globals %d (sealed %d, "
        "guarded %d), TUs rewritten %d, thread-safety diagnostics %d, sealed write "
        "sites %d" % (n_annot_fns, len(tables.sealed) + tables.guarded, len(tables.sealed),
                      tables.guarded, rewritten, tsa, seal))
    say("  check 3: TUs walked %d, rows %d (defined in an analysed TU %d), accessors %d "
        "(defined %d), escapes %d (defined %d), references classified %s"
        % (walked, len(tables.rows), len(rows_defined & set(tables.rows)),
           len(tables.accessors), len(rows_defined & tables.accessors),
           len(tables.escapes), len(rows_defined & set(tables.escapes)),
           ", ".join("%s %d" % kv for kv in sorted(kinds.items())) or "none"))
    listed = set(tables.rows) | tables.accessors | set(tables.escapes)
    unseen = sorted(listed - rows_defined)
    if unseen and mode.startswith("check --all"):
        say("  table rows no analysed TU defines: %s" % ", ".join(unseen))
    slow = sorted(results, key=lambda r: -r.get("sec", 0))[:5]
    if results:
        say("  slowest TUs: %s" % ", ".join("%s[%s] %.2f s" % (r["rel"], r["db"], r.get("sec", 0))
                                             for r in slow))
    if parse:
        say("thread-roles: VACUOUS: %d selected TU(s) do not parse under clang-19 (%d "
            "diagnostic line(s)); nothing in them was checked:" % (unparsed(results), len(parse)))
        for p in parse[:20]:
            say("    " + p)
    if refusals:
        say("thread-roles: %d refusal(s):" % len(refusals))
        for r in refusals[:60]:
            say("    " + r)
        if len(refusals) > 60:
            say("    ... %d more" % (len(refusals) - 60))
    return 1 if (refusals or parse) else 0, unseen


def unparsed(results):
    """-> the number of analysed (file, database) entries with a parse
    failure."""
    return sum(1 for r in results if r["parse"])


def refused_line(results):
    """The fence's last line on a refusal; it names the unparsed entries,
    since a refusal of those is a check that did not run, not a finding."""
    n = unparsed(results)
    return "thread-roles: REFUSED%s" % (
        " (VACUOUS: %d TU(s) unparsed, nothing in them checked)" % n if n else "")


def ensure_inputs_both(root, linux_path, rels):
    """Build the generated inputs either database's selected entries (rels,
    every entry when None) read and its build directory lacks: an entry
    that includes an absent generated header does not parse, and an
    unparsed entry is VACUOUS. -> the note for the mode line."""
    note = lib.ensure_db_inputs(root, FENCE, linux_path, rels, jobs())
    mpath = mingw_db(root)
    if mpath:
        note += "; windows-mingw " + lib.ensure_db_inputs(root, FENCE, mpath, rels, jobs(),
                                                          dbname="windows-mingw")
    return note


def whole_selection(mode):
    """True when the run walked every TU, so a reference it did not see does
    not exist."""
    return mode.startswith("check --all") or "every TU" in mode or "whole tree" in mode


def databases(root, need_mingw=True):
    path = os.environ.get(FENCE.db_env) or os.path.join(root, lib.DB_DEFAULT)
    if not os.path.isfile(path):
        return None, None
    linux = load_entries(root, path, False)
    mingw = load_entries(root, mingw_db(root), True) if need_mingw else {}
    return linux, mingw


def jobs():
    return lib.jobs("OPENPETE_THREAD_ROLES_JOBS")


def select_all(linux, mingw, files=None):
    sel = {"linux": {}, "mingw": {}}
    for name, entries in (("linux", linux), ("mingw", mingw)):
        for rel, lst in entries.items():
            if files is None or rel in files:
                sel[name][rel] = lst
    return sel


def declarator_name(text):
    """-> the identifier a declarator ending text declares, or None: array
    bounds and parameter lists after it are dropped, and a group `(*name...)`
    is entered, so `int a[2]`, `T (*p)[3]` and `void (*cb)(int)` give a, p
    and cb."""
    s = text.rstrip()
    while s and s[-1] in ")]":
        if s[-1] == "]":
            i = s.rfind("[")
            if i < 0:
                return None
            s = s[:i].rstrip()
            continue
        depth, i = 0, len(s) - 1
        while i >= 0:
            depth += {")": 1, "(": -1}.get(s[i], 0)
            if depth == 0:
                break
            i -= 1
        if i < 0:
            return None
        inner = s[i + 1:-1].strip()
        s = inner.lstrip("*& \t") if inner.startswith(("*", "&")) else s[:i].rstrip()
        s = re.sub(r"^(?:(?:const|volatile|restrict)\b\s*)+", "", s)
    m = re.search(r"([A-Za-z_]\w*)$", s)
    return m.group(1) if m else None


def first_form(root, base, path, code):
    """True when the form macro on the added line is the global's first: the
    declarator before it carries no form macro in the base text of path."""
    m = re.search(r"\b(?:%s)\(" % "|".join(FORM_MACROS), code)
    name = declarator_name(code[:m.start()]) if m else None
    if not name:
        return False
    before = strip_comments(pop.tsv_at(root, base, path) or "")
    form = re.compile(r"\b(?:%s)\(" % "|".join(FORM_MACROS))
    for d in re.finditer(r"\b%s\b" % re.escape(name), before):
        # The declarator's tail: up to the first top-level `;`, `,`, `=` or
        # `{`; a `)` closing the group the name sits in keeps the top level.
        depth, i = 0, d.end()
        while i < len(before):
            ch = before[i]
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth = max(depth - 1, 0)
            elif depth == 0 and ch in ";,={":
                break
            i += 1
        if form.search(before, d.end(), i):
            return False
    return True


def marker_refusals(root, base, staged, rev=None):
    """Added lines (the index against base, or base..rev) that add or change a
    table row, or change a global's form, need a `thread-roles:` reason on
    the line. A first form needs none."""
    out = []
    hunks = lib.diff_hunks(root, base, staged, rev=rev)
    for path, hs in lib.added_ranges(hunks).items():
        try:
            lines = read_text(os.path.join(root, path)).split("\n")
        except OSError:
            continue
        if not FENCE.in_scope(path):
            continue
        code_lines = strip_comments("\n".join(lines)).split("\n")
        for a, b in hs:
            for n in range(a, b + 1):
                s = lines[n - 1] if n - 1 < len(lines) else ""
                code = code_lines[n - 1] if n - 1 < len(code_lines) else ""
                if code.lstrip().startswith("#"):
                    continue
                table = path == ROLES_H and re.search(r"\b(ROW|SEAL|ACCESSOR|ESCAPE)\(\"?\w", code)
                form = path != ROLES_H and re.search(r"\b(%s)\(" % "|".join(FORM_MACROS), code)
                if form and first_form(root, base, path, code):
                    continue
                if (table or form) and not MARKER.search(s):
                    out.append("%s:%d: a row or a global's form changes without a "
                               "`thread-roles: <reason>` marker on the line" % (path, n))
    return out


def load_lists(root, rev):
    """-> (unannotated, deferred) at rev (`:` the index, None the working
    file). VACUOUS without unannotated.tsv: no census ran."""
    def text(path):
        if rev is None:
            full = os.path.join(root, path)
            return read_text(full) if os.path.isfile(full) else None
        return pop.tsv_at(root, rev, path)
    un = text(pop.UNANNOTATED)
    if un is None:
        raise Refusal("VACUOUS: no %s; run `thread_roles.py census --write`" % pop.UNANNOTATED)
    return pop.Baseline(un), pop.DeferredList(text(pop.DEFERRED))


def compare_lists(root, base, un, dfr, renames, accept):
    """Each TSV against `base`. -> refusals; the reports are printed."""
    un_text = pop.tsv_at(root, base, pop.UNANNOTATED)
    if un_text is None:
        say("  check 2: %s has no %s; nothing to compare" % (base[:9], pop.UNANNOTATED))
        return []
    old_un = pop.Baseline(un_text)
    old_d = pop.Baseline(pop.tsv_at(root, base, pop.DEFERRED) or "")
    pending = []
    r1, s1 = pop.additions_refused(pop.UNANNOTATED, old_un, un, renames, accept,
                                   moved_from=old_d, pending=pending)
    r2, s2 = pop.additions_refused(pop.DEFERRED, old_d, dfr.base, renames, accept,
                                   confirmed=old_un, need_note=True, pending=pending)
    for line in s1 + s2:
        say("  " + line)
    if not pending:
        return r1 + r2
    # First, so the line cap of the printed refusals never drops it.
    line = pop.accept_line(pending)
    return ["to accept every added row, commit with: %s" % line if line else
            "no one %s value covers every added row (two deferred rows record "
            "different reasons): commit them separately" % pop.ACCEPT_ENV] + r1 + r2


def hunks_scope(root, hunks):
    """-> the in-scope part of a {path: hunks} map."""
    return {p: h for p, h in hunks.items() if FENCE.in_scope(p)}


def c2_plan(root, hunks, entries, read):
    """-> the check 2 scan plan of a diff: the sources it names and every
    source reaching a header it names."""
    files = tracked_scope(root)
    headers = {p for p in hunks if is_header(p)}
    tus = {p for p in hunks if not is_header(p) and p in entries}
    tus |= pop.includers(set(entries), files, read, headers)
    return {"hunks": hunks, "stateless": True, "stateless_headers": bool(headers),
            "macros": pop.static_macros(files, read), "tus": tus}


def dump_refused(r, path):
    """Write the refused globals for `thread_roles_annotate.py --batch`."""
    if path:
        with open(path, "w") as fh:
            json.dump(json_ready({"refused": [r["recs"][g] for g in r["refused"]]}), fh)


def c2_verdict(root, tus, hunks, un, dfr, read):
    """Rule on the merged TUs and print the engagement line. -> (the new-global
    half's exit code, the extent half's exit code, the ruling).

    New-global half, always armed: a form-less global in neither TSV, and
    changed code in a file no TU reached (VACUOUS: the population check did
    not run there). Extent half, armed by CHECK2_ARMED: a form-less global a
    touched extent names, a row whose global has a form, a global in both
    TSVs. No extent touched prints as VACUOUS and rules on nothing."""
    r = pop.rule(tus, un, dfr)
    say(pop.engagement_line(r))
    dump_refused(r, os.environ.get("OPENPETE_THREAD_ROLES_REFUSED"))
    new, other = pop.stateless(r["recs"], r["declared"], un, dfr)
    unseen = sorted(p for p, h in hunks.items()
                    if p not in r["seen"] and p not in lib.UNSCORED
                    and p not in pop.NO_TU_HEADERS
                    and os.path.isfile(os.path.join(root, p))
                    and pop.code_touched(read(p), h, strip_comments))
    if unseen:
        say("thread-roles: VACUOUS: check 2 reached no TU for changed code in: %s"
            % ", ".join(unseen))
    elif not r["extents"]:
        say("  check 2: VACUOUS: no extent or declaration touched, nothing ruled on")
    if new:
        say("  check 2: %d global(s) with no form and no row (refused):" % len(new))
        print_lines(new, 60)
    extent = r["text"] + other
    if extent:
        say("  check 2: %d touched-extent or row finding(s):" % len(extent))
        print_lines(extent, 60)
    return (1 if new or unseen else 0), (1 if extent else 0), r


def print_lines(lines, cap):
    """Print up to cap indented lines and the count of the rest."""
    for line in lines[:cap]:
        say("    " + line)
    if len(lines) > cap:
        say("    ... %d more" % (len(lines) - cap))


def c2_exit(code):
    """-> the extent half's exit status: code when armed, else 0 with a line
    saying the refusals above were reports."""
    if code and not CHECK2_ARMED:
        say("  check 2: REPORT ONLY: the touched-extent and row findings above would "
            "refuse; CHECK2_ARMED is False")
        return 0
    return code


def hook_renames(root, changes):
    return {a: b for s, a, b in changes if s == "R"}


def mode_hook(root):
    lib.refuse_temporary_index()
    changes, base = lib.staged_changes(root)
    staged = [b or a for s, a, b in changes if s != "D"]
    scope = [f for f in staged if FENCE.in_scope(f) or f in (FENCE_SCRIPT, ROLES_H)]
    lists_staged = {pop.UNANNOTATED, pop.DEFERRED} & set(staged)
    if not scope and not lists_staged:
        say("thread-roles: no staged in-scope path")
        return 0
    dirty_lists = lists_staged & set(git(root, "diff", "--name-only").stdout.split("\n"))
    if dirty_lists:
        raise Refusal("staged with unstaged hunks: %s" % ", ".join(sorted(dirty_lists)))
    un, dfr = load_lists(root, ":")
    accept = lib.accept_reason(pop.ACCEPT_ENV) or ""
    list_refusals = compare_lists(root, base, un, dfr, hook_renames(root, changes), accept)
    print_lines(list_refusals, 60)
    if not scope:
        if list_refusals:
            say("thread-roles: REFUSED")
        return 1 if list_refusals else 0
    dirty = set(git(root, "diff", "--name-only").stdout.split("\n"))
    unstaged = sorted(f for f in scope if f in dirty)
    if unstaged:
        raise Refusal("staged with unstaged hunks: %s. The fence reads the working "
                      "file and the commit carries the index blob. `git add` them, "
                      "or stash the rest." % ", ".join(unstaged))
    hunks = hunks_scope(root, lib.diff_hunks(root, base, scope))
    rc, results = change_verdict(root, base, scope, hunks, un, dfr, "hook", None)
    if rc or list_refusals:
        say(refused_line(results))
        return 1
    return 0


def change_verdict(root, base, scope, hunks, un, dfr, label, rev):
    """Checks 1 to 4 and the marker check over the TUs a change reaches: the
    changed sources, or every TU when a header or this fence changed. The
    hook's change is the index against HEAD (rev None); the range's is
    base..rev at rev. -> (exit code, analysis results)."""
    linux_path = lib.linux_db(root, FENCE)
    whole = any(is_header(f) for f in scope) or FENCE_SCRIPT in scope
    sources = {f for f in scope if not is_header(f) and FENCE.in_scope(f)}
    inputs = ensure_inputs_both(root, linux_path, None if whole else sources)
    linux, mingw = databases(root)
    missing = sorted(f for f in sources if f not in linux and f not in mingw
                     and f not in lib.UNSCORED)
    if missing:
        raise Refusal("VACUOUS: changed sources with no compile-database entry: %s. "
                      "Re-run: %s" % (", ".join(missing), lib.CONFIGURE))
    sel = select_all(linux, mingw, None if whole else sources)
    mode = "%s (%s; %s)" % (label, "whole tree: a changed header or fence" if whole
                            else "%d changed source(s)" % len(sources), inputs)
    # A source on the UNSCORED list has no entry in either database by
    # design (a launcher-preset-only TU): it is not analysable, so a change
    # holding only such sources has nothing to analyse rather than a
    # vacuous analysis.
    analysable = {f for f in sources if f not in lib.UNSCORED}
    if not whole and sources and not analysable:
        say("thread-roles: changed sources are all UNSCORED (no compile-database entry by "
            "design): %s" % ", ".join(sorted(sources)))
        return 0, []
    if not any(sel.values()) and analysable:
        raise Refusal("VACUOUS: zero TUs analysed for %d changed source(s)" % len(analysable))
    read = lambda rel: read_text(os.path.join(root, rel))
    plan = c2_plan(root, hunks, set(linux) | set(mingw), read)
    tables, ctx, results, wall = run_analysis(root, sel, jobs(), mode, plan)
    rc, _ = report(root, tables, ctx, results, wall, set(scope), mode)
    t0 = time.time()
    new_rc, extent_rc, _ = c2_verdict(root, [r["c2"] for r in results if r.get("c2")],
                                      hunks, un, dfr, read)
    say("  check 2: ruled in %.2f s over %d TU entries" % (
        time.time() - t0, sum(1 for r in results if r.get("c2"))))
    extent_rc = c2_exit(extent_rc)
    rc = rc or new_rc or extent_rc
    marks = marker_refusals(root, base, scope, rev)
    print_lines(marks, len(marks))
    return (1 if rc or marks else 0), results


def mode_check(root, argv):
    ctest = "--ctest" in argv
    whole = "--all" in argv
    path = os.environ.get(FENCE.db_env) or os.path.join(root, lib.DB_DEFAULT)
    if not os.path.isfile(path):
        if ctest:
            say("thread-roles: SKIP: no compile database at %s" % path)
            return 64
        raise Refusal("VACUOUS: no compile database at %s. Run: %s" % (path, lib.CONFIGURE))
    linux, mingw = databases(root)
    files = tracked_scope(root)
    stamp_path = os.path.join(os.path.dirname(os.path.abspath(path)), STAMP)
    now = scope_hashes(root, files)
    db_sig = "%s %s" % (os.stat(path).st_mtime_ns, len(linux))
    try:
        with open(stamp_path) as fh:
            old = json.load(fh)
    except (OSError, ValueError):
        old = None
    if whole or old is None or old.get("db") != db_sig:
        sel_files = None
        why = "every TU (%s)" % ("--all" if whole else "no stamp" if old is None
                                 else "the database changed")
    else:
        changed = {f for f, h in now.items() if old["files"].get(f) != h}
        if any(is_header(f) or not f.startswith(SCOPE_ROOTS) for f in changed):
            sel_files, why = None, "every TU (a header or the fence changed)"
        else:
            sel_files = {f for f in changed if f in linux or f in mingw}
            why = "%d changed source(s)" % len(sel_files)
            if changed and not sel_files:
                say("thread-roles: check: the changed paths have no database entry "
                    "(%s)" % ", ".join(sorted(changed)[:5]))
    sel = select_all(linux, mingw, sel_files)
    mode = "check --all" if whole else "check: %s" % why
    un, dfr = load_lists(root, None)
    list_refusals = merge_base_compare(root, un, dfr)
    print_lines(list_refusals, 60)
    if sel_files is not None and not sel_files:
        if list_refusals:
            say("thread-roles: REFUSED")
            return 1
        say("thread-roles: check: no analysed source changed since the last pass")
        with open(stamp_path, "w") as fh:
            json.dump({"db": db_sig, "files": now}, fh)
        return 0
    if not any(sel.values()):
        raise Refusal("VACUOUS: zero TUs selected (%s)" % why)
    plan = {"hunks": {}, "stateless": True, "stateless_headers": sel_files is None,
            "macros": pop.static_macros(files, lambda r: read_text(os.path.join(root, r))),
            "tus": set(linux) | set(mingw) if sel_files is None else set(sel_files)}
    tables, ctx, results, wall = run_analysis(root, sel, jobs(), mode, plan)
    rc, _ = report(root, tables, ctx, results, wall,
                   set(files) if sel_files is None else sel_files, mode)
    tus = [r["c2"] for r in results if r.get("c2")]
    extents, recs, seen, declared, defs = pop.merge_tus(tus)
    new, state = pop.stateless(recs, declared, un, dfr)
    if sel_files is None:
        state += stale_lines(un, recs, seen, declared)
        state += stale_lines(dfr.base, recs, seen, declared)
    say("  check 2 (stateless): globals declared %d, rows %d unannotated, %d deferred, "
        "definitions %d, no form and no row %d (refused), row findings %d"
        % (len(declared), len(un.rows), len(dfr), defs, len(new), len(state)))
    print_lines(new, 40)
    print_lines(state, 40)
    extent_rc = c2_exit(1 if state else 0)
    rc = rc or (1 if new or list_refusals else 0) or extent_rc
    if rc == 0:
        with open(stamp_path, "w") as fh:
            json.dump({"db": db_sig, "files": now}, fh)
        say("thread-roles: PASS")
    else:
        say(refused_line(results))
    return rc


def stale_lines(un, recs, seen, declared):
    """-> a line per row on a scanned file whose global no TU declared. Only
    a whole-tree selection decides this."""
    keys = {(recs[g]["path"], recs[g]["function"], recs[g]["name"]) for g in declared}
    names = {recs[g]["name"] for g in declared}
    return ["%s:%s %s has a row and no declaration; delete the row" % k
            for k, row in sorted(un.rows.items())
            if k[0] in seen and k not in keys and not (row[3] == "extern" and k[2] in names)]


def default_branch_base(root):
    """-> the merge-base of HEAD and the default branch (the branch the main
    checkout has checked out) or its origin copy, or None when there is none.
    """
    common = git(root, "rev-parse", "--path-format=absolute", "--git-common-dir",
                 check=False).stdout.strip()
    branch = common and git(root, "--git-dir=" + common, "symbolic-ref", "--short",
                            "-q", "HEAD", check=False).stdout.strip()
    for ref in ((branch, "origin/" + branch) if branch else ()):
        p = git(root, "merge-base", "HEAD", ref, check=False)
        if p.returncode == 0 and p.stdout.strip():
            return p.stdout.strip()
    return None


def merge_base_compare(root, un, dfr):
    """The stateless half's TSV compare with the merge-base."""
    base = default_branch_base(root)
    if base is None:
        say("  check 2: no default branch; the merge-base compare did not run")
        return []
    p = git(root, "diff", "--name-status", "-M", "-l", "100000", "--diff-filter=R", base)
    renames = {c[1]: c[2] for c in (l.split("\t") for l in p.stdout.splitlines())}
    return compare_lists(root, base, un, dfr, renames, None)


def rev_hunks(root, base, head):
    """-> {in-scope path: after-side hunks} of base..head."""
    return hunks_scope(root, lib.diff_hunks(root, base, None, rev=head))


def selected(linux, mingw, tus):
    return {"linux": {r: l for r, l in linux.items() if r in tus},
            "mingw": {r: l for r, l in mingw.items() if r in tus}}


def scan_vacuity(results):
    """-> the parse failures of a scan; each is VACUOUS."""
    return [p for r in results for p in r["parse"]]


def mode_range(root, argv):
    """The hook's verdict over the cumulative change base..head, at head (a
    rebase or cherry-pick runs no pre-commit hook), with the TSV compare
    against base: an added row needs its reason in the note."""
    if len(argv) < 2 or ".." not in argv[1]:
        return None
    base, head = argv[1].split("..", 1)
    head_sha = git(root, "rev-parse", head).stdout.strip()
    if head_sha != lib.head_sha(root):
        raise Refusal("range reads the tree at %s, and HEAD is %s" % (head, lib.head_sha(root)))
    dirty = sorted(lib.dirty_paths(root, FENCE))
    if dirty:
        raise Refusal("range reads the working tree; tracked or untracked changes in "
                      "scope: %s" % ", ".join(dirty[:5]))
    un, dfr = load_lists(root, None)
    p = git(root, "diff", "--name-status", "-M", "-l", "100000", "--diff-filter=R", base, head)
    renames = {c[1]: c[2] for c in (l.split("\t") for l in p.stdout.splitlines())}
    list_refusals = compare_lists(root, base, un, dfr, renames, None)
    print_lines(list_refusals, 60)
    hunks = rev_hunks(root, base, head)
    changed = git(root, "diff", "--name-only", "--no-renames", "--diff-filter=d", base,
                  head).stdout.split("\n")
    scope = sorted(f for f in changed if f and (FENCE.in_scope(f) or f == FENCE_SCRIPT))
    say("thread-roles: range %s..%s: %d changed paths in scope" % (base[:9], head[:9], len(scope)))
    rc, results = (change_verdict(root, base, scope, hunks, un, dfr, "range", head)
                   if scope else (0, []))
    if rc or list_refusals:
        say(refused_line(results))
        return 1
    say("thread-roles: PASS")
    return 0


def census_select(linux, mingw):
    return select_all(linux, mingw)


def census_rows(m, confirmed):
    """-> the unannotated.tsv rows of a merged census: one per form-less
    member, one per defining path of an extern."""
    rows = []
    for g, rec in m["globals"].items():
        if rec["form"]:
            continue
        form, why = confirmed.get(g, ("-", ""))
        for path in sorted(rec["paths"]) or [rec["path"]]:
            rows.append([path, rec["function"], rec["name"], rec["kind"], form, why])
    return rows


def summarize(m, confirmed, rows, wall, entries, parse):
    """Print the census's report."""
    recs = m["globals"]
    by_kind, by_form, by_file = {}, {}, {}
    for r in rows:
        by_kind[r[3]] = by_kind.get(r[3], 0) + 1
        by_form[r[4]] = by_form.get(r[4], 0) + 1
        by_file[r[0]] = by_file.get(r[0], 0) + 1
    formed = sum(1 for r in recs.values() if r["form"])
    say("thread-roles census: %d TU entries, %.1f s wall, %d parse failures"
        % (entries, wall, len(parse)))
    say("  population %d globals (%d with a form, %d rows form-less)"
        % (len(recs), formed, len(rows)))
    say("  rows by kind: %s" % ", ".join("%s %d" % kv for kv in sorted(by_kind.items())))
    say("  confirmed forms: %s" % ", ".join("%s %d" % kv for kv in sorted(by_form.items())))
    say("  largest files: %s" % ", ".join("%s %d" % kv for kv in sorted(
        by_file.items(), key=lambda kv: -kv[1])[:8]))


def census_data(root):
    """-> (merged census, {gid: confirmed form}, rows, wall, entries, parse)."""
    linux, mingw = databases(root)
    results, wall = run_scan(root, select_all(linux, mingw), None)
    parse = scan_vacuity(results)
    m = pop.merge_census([r["tu"] for r in results if r["tu"]])
    confirmed = pop.confirm(m)
    return m, confirmed, census_rows(m, confirmed), wall, len(results), parse


def json_ready(obj):
    """Sets to sorted lists, for json.dump."""
    if isinstance(obj, dict):
        return {str(k): json_ready(v) for k, v in obj.items()}
    if isinstance(obj, (set, frozenset, list, tuple)):
        items = [json_ready(x) for x in obj]
        return sorted(items, key=str) if isinstance(obj, (set, frozenset)) else items
    return obj


def option(argv, name):
    """-> the value after `name` in argv, or None."""
    return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else None


def mode_census(root, argv):
    m, confirmed, rows, wall, entries, parse = census_data(root)
    summarize(m, confirmed, rows, wall, entries, parse)
    for p in parse[:20]:
        say("    " + p)
    out = option(argv, "--json")
    if out:
        with open(out, "w") as fh:
            json.dump(json_ready({"globals": m["globals"], "refs": m["refs"],
                                  "naming": pop.naming_counts(m), "confirmed": confirmed,
                                  "seen": m["seen"], "calls": m["calls"], "fns": m["fns"],
                                  "addr_fn": m["addr_fn"]}), fh)
    if not any(r[4] != "-" for r in rows) or not m["fns"]:
        raise Refusal("VACUOUS: the census confirmed zero forms")
    if parse:
        raise Refusal("VACUOUS: %d entries do not parse under libclang" % len(parse))
    if "--write" in argv:
        path = os.path.join(root, pop.UNANNOTATED)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write(pop.render_tsv("mutable globals with no form (thread_roles.py census)",
                                    rows, "census", lib.head_sha(root)))
        dpath = os.path.join(root, pop.DEFERRED)
        if not os.path.isfile(dpath):
            with open(dpath, "w") as fh:
                fh.write(pop.render_tsv("mutable globals whose demand cascades, each row "
                                        "with its annotator chain", [], "census",
                                        lib.head_sha(root)))
    return 0


def mode_tighten(root):
    """Rows leave unannotated.tsv when the global has a form or is gone;
    the annotator re-runs over deferred.tsv and moves a row that reaches a
    fixed point. No row is added here but such a move."""
    un, dfr = load_lists(root, None)
    m, confirmed, rows, wall, entries, parse = census_data(root)
    if parse:
        raise Refusal("VACUOUS: %d entries do not parse under libclang" % len(parse))
    live = {tuple(r[:3]): r for r in rows}
    kept = [live[k] for k in un.rows if k in live]
    say("thread-roles tighten: unannotated.tsv %d -> %d rows" % (len(un.rows), len(kept)))
    with open(os.path.join(root, pop.UNANNOTATED), "w") as fh:
        fh.write(pop.render_tsv("mutable globals with no form (thread_roles.py census)",
                                kept, "tighten", lib.head_sha(root)))
    annotator = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                             "thread_roles_annotate.py")
    return subprocess.run([sys.executable, annotator, "--tighten"], cwd=root).returncode


def mode_owe(root, argv):
    """Record every form-less global in neither TSV as owed: a row in
    unannotated.tsv whose note is argv[1], the census's confirmed form in
    its form column; the rows whose global has a form or is gone leave, as
    in tighten. The commit carrying it names each added global in
    OPENPETE_THREAD_ROLES_ACCEPT."""
    if len(argv) < 2 or not argv[1].strip():
        raise Refusal("owe needs the note the added rows carry: owe \"<reason>\"")
    un, dfr = load_lists(root, None)
    m, confirmed, rows, wall, entries, parse = census_data(root)
    if parse:
        raise Refusal("VACUOUS: %d entries do not parse under libclang" % len(parse))
    live = {tuple(r[:3]): r for r in rows}
    kept = [un.rows[k] for k in un.rows if k in live]
    added = [live[k][:5] + [argv[1].strip()] for k in sorted(live)
             if k not in un.rows and k not in dfr.base.rows]
    by_kind = {}
    for r in added:
        by_kind[r[3]] = by_kind.get(r[3], 0) + 1
    say("thread-roles owe: %d TU entries, %.1f s; unannotated.tsv %d rows: %d kept, %d left "
        "(form or gone), %d added (%s) in %d files"
        % (entries, wall, len(un.rows), len(kept), len(un.rows) - len(kept), len(added),
           ", ".join("%s %d" % kv for kv in sorted(by_kind.items())),
           len({r[0] for r in added})))
    with open(os.path.join(root, pop.UNANNOTATED), "w") as fh:
        fh.write(pop.render_tsv("mutable globals with no form (thread_roles.py census)",
                                kept + added, "owe", lib.head_sha(root)))
    return 0


def snapshot_at(root, sha):
    """-> {rel: text at sha} of every in-scope path that differs from HEAD."""
    p = git(root, "diff", "--name-only", "--no-renames", sha, "HEAD", "--", *FENCE.root_dirs())
    snap = {}
    for rel in p.stdout.split():
        if not FENCE.in_scope(rel):
            continue
        q = subprocess.run(["git", "-C", root, "show", "%s:%s" % (sha, rel)],
                           capture_output=True)
        if q.returncode == 0:
            snap[rel] = q.stdout.decode("utf-8", "replace")
    return snap


def mode_replay(root, argv):
    """check 2's touched-extent half of one commit, over that commit's text
    served to libclang in this tree's databases, with the census's facts for
    the demand. Exit 0; the verdict is the report."""
    if len(argv) < 2:
        return None
    sha = git(root, "rev-parse", argv[1]).stdout.strip()
    un, dfr = load_lists(root, None)
    snap = snapshot_at(root, sha)
    read = lambda rel: snap[rel] if rel in snap else read_text(os.path.join(root, rel))
    hunks = hunks_scope(root, lib.diff_hunks(root, sha + "^", None, rev=sha))
    linux, mingw = databases(root)
    plan = c2_plan(root, hunks, set(linux) | set(mingw), read)
    plan["stateless"] = False
    results, wall = run_scan(root, selected(linux, mingw, plan["tus"]), plan, snap)
    parse = scan_vacuity(results)
    tus = [r["tu"] for r in results if r["tu"]]
    r = pop.rule(tus, un, dfr)
    say("thread-roles: replay %s: %d files, %d TU entries (%d parse failures), %.1f s, "
        "%d files replaced" % (sha[:9], len(hunks), len(results), len(parse), wall, len(snap)))
    say(pop.engagement_line(r))
    if not r["extents"]:
        say("  VACUOUS: no extent touched")
    out = option(argv, "--json")
    if out:
        with open(out, "w") as fh:
            json.dump(json_ready({"sha": sha, "extents": r["extents"], "named": r["named"],
                                  "refused": [dict(r["recs"][g], where=r["by_global"][g])
                                              for g in r["refused"]],
                                  "passed_form": r["passed_form"], "parse": parse[:10],
                                  "files": sorted(hunks)}), fh)
    for t in r["text"]:
        say("    " + t[:240])
    return 0


def mode_armed(root):
    if os.environ.get("OPENPETE_NO_COMMITS") == "1":
        say("thread-roles: OPENPETE_NO_COMMITS=1; hook arming not required.")
        return 0
    why = lib.armed_reason(root, ("thread_roles.py",))
    if why:
        say("thread-roles: DISARMED: %s.\n  Arm this clone once:  %s" % (why, lib.ARM))
        return 1
    say("thread-roles: ARMED (%s)" % lib.HOOK_REL)
    return 0


SITE_RE = re.compile(r"\b(OP_ROLE_BIND|OP_ROLE_ASSERT_SHARED|OP_ROLE_ASSERT|OP_ROLE_ENTER|"
                     r"OP_ROLE_CHECK|op_seal_check)\s*\(\s*(\w+)")
KIND = {"OP_ROLE_BIND": "bind", "OP_ROLE_ASSERT": "assert", "OP_ROLE_ASSERT_SHARED": "assert",
        "OP_ROLE_ENTER": "enter", "OP_ROLE_CHECK": "check", "op_seal_check": "seal"}


def site_list(root):
    out = []
    for rel in tracked_scope(root):
        if rel == ROLES_H:
            continue
        text = read_text(os.path.join(root, rel))
        for m in SITE_RE.finditer(text):
            line_start = text.rfind("\n", 0, m.start()) + 1
            if text[line_start:m.start()].lstrip().startswith("#"):
                continue
            out.append((KIND[m.group(1)], rel, text.count("\n", 0, m.start()) + 1, m.group(2)))
    return out


def mode_sites(root):
    for kind, rel, line, arg in site_list(root):
        print("%s\t%s\t%d\t%s" % (kind, rel, line, arg))
    return 0


def mode_join(root, argv):
    if len(argv) < 2:
        return None
    reached = {}
    for line in read_text(argv[1]).splitlines()[1:]:
        cols = line.split("\t")
        if len(cols) < 7:
            continue
        reached[(cols[1], int(cols[2]))] = cols
    print("kind\tfile\tline\tfunction\tevals\tfails")
    seen = set()
    for kind, rel, line, arg in site_list(root):
        hit = None
        for (f, ln), cols in reached.items():
            if ln == line and os.path.normpath(f).endswith(rel):
                hit = cols
                seen.add((f, ln))
        if hit:
            print("%s\t%s\t%d\t%s\t%s\t%s" % (kind, rel, line, hit[3], hit[5], hit[6]))
        else:
            print("%s\t%s\t%d\t-\t0\t0" % (kind, rel, line))
    for key, cols in sorted(reached.items()):
        if key not in seen:
            print("%s\t%s\t%s\t%s\t%s\t%s\t(not in the tree's site list)"
                  % (cols[0], cols[1], cols[2], cols[3], cols[5], cols[6]))
    return 0


def dispatch(root, mode, argv):
    if mode in ("hook", "check") and "--armed" not in argv \
            and not os.path.isfile(os.path.join(root, ROLES_H)):
        if os.path.realpath(root) == os.path.realpath(lib.OWN_ROOT):
            raise Refusal("%s is missing" % ROLES_H)
        say("thread-roles: %s holds no %s; nothing to check" % (root, ROLES_H))
        return 0
    if mode == "hook":
        return mode_hook(root)
    if mode == "check" and "--armed" in argv:
        return mode_armed(root)
    if mode == "check":
        return mode_check(root, argv)
    if mode == "range":
        return mode_range(root, argv)
    if mode == "census":
        return mode_census(root, argv)
    if mode == "tighten":
        return mode_tighten(root)
    if mode == "owe":
        return mode_owe(root, argv)
    if mode == "replay":
        return mode_replay(root, argv)
    if mode == "sites":
        return mode_sites(root)
    if mode == "join":
        return mode_join(root, argv)
    return None


if __name__ == "__main__":
    sys.exit(lib.run(FENCE, dispatch, __doc__))
