#!/usr/bin/env python3
"""Check 2 of the thread-role fence: the mutable-global population.

The population is every variable of static storage duration declared in an
in-scope file outside tests/, not const (through arrays), not thread-local,
and not of a type exempt by construction (EXEMPT_TYPES). A global has a form when every
declaration carries OP_GUARDED_BY*, OP_PT_GUARDED_BY* or OP_SEALED, or its
type is atomic.

Keys: (path, "-", name) for a file-scope static or an external definition,
(path, function, name) for a function-local static. A static spelled inside
a macro body is one global, keyed at the macro with function `#define:<macro>`.
Qualified C++ names join with `.`.

This module holds the per-TU scan (`scan_tu`), the census's confirmation of
local forms (`confirm`), the baseline files (`Baseline`, `DeferredList`), the
touched-extent ruling (`rule`) and the include graph (`includers`). libclang
cursors are read only inside the scan; everything after it is plain data.
"""
import os
import re

import ratchet_lib as lib
from ratchet_lib import git, is_header

# In-scope headers no database entry includes: the stub a game's regions
# replace, the headers the generated C includes, and the scene-ring
# harness's prelude and epilogue, which only the ring source it extracts
# into the build tree includes.
NO_TU_HEADERS = ("games/common/state_regions_none.h", "games/spyro-1/module_imports.h",
                 "games/spyro-2/module_imports.h", "games/spyro-3/module_imports.h",
                 "tests/sched/present/ring_prelude.h", "tests/sched/present/ring_epilogue.h")

UNANNOTATED = "baselines/thread-roles/unannotated.tsv"
DEFERRED = "baselines/thread-roles/deferred.tsv"
TSV_COLUMNS = ["file", "function", "name", "kind", "form", "note"]
ACCEPT_ENV = "OPENPETE_THREAD_ROLES_ACCEPT"

FORM_RE = re.compile(r"\bOP_(?:PT_)?GUARDED_BY(?:_CAP)?\(|\bOP_SEALED\(")
# Canonical spellings of the atomic types; arrays are stripped first.
ATOMIC_RE = re.compile(r"^(_Atomic\(|std::(__\w+::)?atomic<|std::atomic_|SDL_AtomicInt$|"
                       r"SDL_AtomicU32$|struct SDL_AtomicInt$|struct SDL_AtomicU32$)")
# Exempt by type: a check site must be reachable from the wrong thread, and a
# lock object or a handle to one orders its own users, so no form names an
# owner above it. A lock handle's creation is ordered by openpete_mutex_once
# or by the seal of the group that creates it.
EXEMPT_TYPES = ("op_role_site", "op_seal", "op_lock_cap", "SDL_Mutex *", "SDL_Condition *",
                "SDL_RWLock *", "SDL_Semaphore *")
ASSIGN = ("=",)
BINARY = ("+", "-", "*", "/", "%", "<<", ">>", "<", ">", "<=", ">=", "==", "!=", "&", "^",
          "|", "&&", "||", ",")
COMPARE = ("==", "!=", "<", ">", "<=", ">=")
STATIC_LINE = re.compile(r"\bstatic\b")
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^">]+)[">]', re.M)
STOP_OPEN = "([{"
STOP_CLOSE = ")]}"


def population_file(rel, fence):
    """True when a declaration in rel can be a population member."""
    return fence.in_scope(rel) and not rel.startswith("tests/")


# --------------------------------------------------------------------------
# Per-TU scan (worker process)
# --------------------------------------------------------------------------
class Scan:
    """One parsed TU: its population members, and the touched extents or
    every extent's references."""

    def __init__(self, ci, tu, ctx, fence, read):
        self.ci = ci
        self.K = ci.CursorKind
        self.tu = tu
        self.root = ctx["root"]
        self.fence = fence
        self.read = read
        self.fns = (self.K.FUNCTION_DECL, self.K.CXX_METHOD, self.K.CONSTRUCTOR,
                    self.K.DESTRUCTOR, self.K.CONVERSION_FUNCTION,
                    self.K.FUNCTION_TEMPLATE)
        self.descend = (self.K.UNEXPOSED_DECL, self.K.LINKAGE_SPEC, self.K.NAMESPACE,
                        self.K.CLASS_DECL, self.K.STRUCT_DECL, self.K.UNION_DECL,
                        self.K.CLASS_TEMPLATE,
                        self.K.CLASS_TEMPLATE_PARTIAL_SPECIALIZATION)
        self.rel_cache = {}
        self.var_cache = {}
        self.text_cache = {}
        self._macro_defs = None
        self.globals = {}       # gid -> record

    # ---- paths and text ------------------------------------------------
    def rel_of(self, name):
        rel = self.rel_cache.get(name)
        if rel is None:
            rel = os.path.relpath(name, self.root)
            self.rel_cache[name] = rel
        return rel

    def text(self, rel):
        t = self.text_cache.get(rel)
        if t is None:
            t = self.read(rel)
            self.text_cache[rel] = t
        return t

    def lines(self, rel):
        key = (rel, "lines")
        t = self.text_cache.get(key)
        if t is None:
            t = self.text(rel).split("\n")
            self.text_cache[key] = t
        return t

    # ---- declarations ---------------------------------------------------
    def items(self):
        """-> every declaration cursor of an in-scope file, with its rel,
        through extern "C" blocks, namespaces and class bodies."""
        stack = list(self.tu.cursor.get_children())
        while stack:
            c = stack.pop()
            f = c.location.file
            if not f:
                continue
            rel = self.rel_of(f.name)
            if not self.fence.in_scope(rel):
                continue
            if c.kind in self.descend:
                stack.extend(c.get_children())
            yield c, rel

    def macro_defs(self):
        """-> {macro: rel} of the in-scope macro definitions this TU saw."""
        if self._macro_defs is None:
            self._macro_defs = {}
            for c in self.tu.cursor.get_children():
                if c.kind == self.K.MACRO_DEFINITION and c.location.file:
                    rel = self.rel_of(c.location.file.name)
                    if self.fence.in_scope(rel):
                        self._macro_defs[c.spelling] = rel
        return self._macro_defs

    def macro_key(self, c, rel):
        """-> (path, function, name) when c's identifier comes from a macro
        expansion, else None. The location of such a declaration is the
        expansion, so its text there spells the macro, not the identifier. A
        name pasted with __LINE__ loses the number: one global, at the macro."""
        n, col = c.location.line - 1, c.location.column - 1
        lines = self.lines(rel)
        if n >= len(lines) or lines[n][col:].startswith(c.spelling):
            return None
        m = re.match(r"\w+", lines[n][col:])
        if m is None or m.group(0) not in self.macro_defs():
            return None
        return (self.macro_defs()[m.group(0)], "#define:" + m.group(0),
                re.sub(r"_?\d+$", "", c.spelling))

    def has_form_text(self, c, rel):
        """True when the declarator at c carries a form macro: the text from
        the identifier to the first top-level `;`, `,`, `=` or `{`. A `)`
        closing a declarator group the identifier sits in (`(*s_cb)(int)`)
        leaves the depth at the top level."""
        lines = self.lines(rel)
        n, col = c.location.line - 1, c.location.column - 1
        if n >= len(lines) or c.spelling not in lines[n]:
            return bool(FORM_RE.search(lines[n])) if n < len(lines) else False
        depth, seen = 0, []
        for ln in range(n, min(n + 8, len(lines))):
            s = lines[ln][col if ln == n else 0:]
            for ch in s:
                if ch in STOP_OPEN:
                    depth += 1
                elif ch in STOP_CLOSE:
                    depth = max(depth - 1, 0)
                elif depth == 0 and ch in ";,={":
                    return bool(FORM_RE.search("".join(seen)))
                seen.append(ch)
            seen.append(" ")
        return bool(FORM_RE.search("".join(seen)))

    def type_facts(self, t):
        """-> dict of the canonical type: const, atomic, exempt, scalar,
        pointer; arrays are stripped, a reference reads its pointee. An array
        of const pointers is const, an array of pointers to const is not."""
        T = self.ci.TypeKind
        ct = t.get_canonical()
        array = False
        # A canonical array carries its element's const on the array type;
        # the element libclang returns from it has lost the qualifier.
        const = ct.is_const_qualified()
        while ct.kind in (T.CONSTANTARRAY, T.INCOMPLETEARRAY, T.VARIABLEARRAY,
                          T.DEPENDENTSIZEDARRAY):
            array = True
            ct = ct.get_array_element_type().get_canonical()
            const = const or ct.is_const_qualified()
        if ct.kind == T.LVALUEREFERENCE:
            ct = ct.get_pointee().get_canonical()
            const = ct.is_const_qualified()
        spelled = re.sub(r"\b(const|volatile)\s+", "", ct.spelling).strip()
        bare = re.sub(r"^(struct|union|class)\s+", "", spelled)
        pointer = ct.kind in (T.POINTER, T.MEMBERPOINTER, T.BLOCKPOINTER)
        record = ct.kind == T.RECORD
        scalar = not array and not pointer and not record and ct.kind not in (
            T.UNEXPOSED, T.ELABORATED, T.ATOMIC, T.DEPENDENT)
        return {"const": const,
                "atomic": ct.kind == T.ATOMIC or bool(ATOMIC_RE.match(spelled)),
                "exempt": bare in EXEMPT_TYPES, "scalar": scalar, "pointer": pointer,
                "array": array, "record": record, "spelled": spelled}

    def qualified(self, c):
        """-> the `.`-joined name of c through its class and namespace
        parents (anonymous parents and a lambda's operator() left out)."""
        K = self.K
        names = [c.spelling]
        p = c.semantic_parent
        while p is not None and p.kind != K.TRANSLATION_UNIT:
            if p.spelling and p.spelling != "operator()" and p.kind in (
                    K.NAMESPACE, K.CLASS_DECL, K.STRUCT_DECL, K.UNION_DECL, K.CLASS_TEMPLATE) \
                    + self.fns:
                names.append(p.spelling)
            p = p.semantic_parent
        return ".".join(reversed(names))

    def enclosing_fn(self, c):
        """-> the function cursor whose body holds c, or None at file or
        class scope."""
        p = c.semantic_parent
        while p is not None and p.kind != self.K.TRANSLATION_UNIT:
            if p.kind in self.fns:
                return p
            p = p.semantic_parent
        return None

    def member(self, c):
        """-> the population record of the VAR_DECL c, or None when it is
        outside the population. Cached by cursor hash."""
        h = c.hash
        if h in self.var_cache:
            return self.var_cache[h]
        rec = self._member(c)
        self.var_cache[h] = rec
        return rec

    def _member(self, c):
        ci = self.ci
        S = ci.StorageClass
        fn = self.enclosing_fn(c)
        if fn is not None and c.storage_class not in (S.STATIC, S.EXTERN):
            return None
        f = c.location.file
        if not f:
            return None
        rel = self.rel_of(f.name)
        if not population_file(rel, self.fence):
            return None
        if c.tls_kind != ci.TLSKind.NONE:
            return None
        facts = self.type_facts(c.type)
        if facts["const"] or facts["exempt"]:
            return None
        external = fn is None and c.linkage == ci.LinkageKind.EXTERNAL
        external = external or c.storage_class == S.EXTERN
        path, func = rel, "-"
        if fn is not None and not external:
            func = self.qualified(fn)
        name = self.qualified(c) if fn is None else c.spelling
        at = self.macro_key(c, rel)
        if at is not None:
            path, func, name = at
        kind = "extern" if external else ("local" if func != "-" else "static")
        gid = "E|" + c.get_usr() if external else "S|%s|%s|%s" % (path, func, name)
        form = ""
        if facts["atomic"]:
            form = "atomic"
        elif self.has_form_text(c, rel):
            form = "named"
        rec = {"gid": gid, "path": path, "function": func, "name": name, "kind": kind,
               "line": c.location.line, "form": form, "defined": c.is_definition(),
               "type": facts["spelled"], "scalar": facts["scalar"],
               "pointer": facts["pointer"], "array": facts["array"],
               "record": facts["record"], "lang": "c++" if rel.endswith(
                   (".cpp", ".cc", ".cxx", ".hpp")) else "c",
               "m1": False}
        if facts["scalar"] and form == "":
            rec["m1"] = "=-1" in "".join(t.spelling for t in c.get_tokens())
        return rec

    def add(self, rec):
        """Merge a declaration: a form-less declaration makes the global
        form-less; a definition's line wins."""
        old = self.globals.get(rec["gid"])
        if old is None:
            self.globals[rec["gid"]] = dict(rec)
            return
        if not rec["form"]:
            old["form"] = ""
        if rec["defined"] and not old["defined"]:
            old.update(path=rec["path"] if rec["kind"] == "extern" else old["path"],
                       line=rec["line"], defined=True)

    # ---- bodies ---------------------------------------------------------
    def walk_body(self, top):
        """-> (cursor, chain) over top's subtree, sizeof and alignof operands
        pruned: a name there evaluates nothing."""
        K = self.K
        todo = [(top, 0)]
        chain = []
        while todo:
            c, depth = todo.pop()
            del chain[depth:]
            chain.append(c)
            yield c, chain
            if c.kind == K.CXX_UNARY_EXPR:
                continue
            kids = list(c.get_children())
            for k in reversed(kids):
                todo.append((k, depth + 1))

    def var_of(self, c):
        """-> the record of the population member the cursor names or
        declares, or None."""
        K = self.K
        if c.kind == K.DECL_REF_EXPR:
            t = c.referenced
            if t is not None and t.kind == K.VAR_DECL:
                return self.member(t)
        elif c.kind == K.VAR_DECL:
            return self.member(c)
        return None

    def extent_names(self, top):
        """-> the records of the population members top's extent names."""
        out = {}
        for c, _ in self.walk_body(top):
            rec = self.var_of(c)
            if rec is not None:
                out[rec["gid"]] = rec
        return out

    def binop(self, c):
        kids = list(c.get_children())
        if len(kids) != 2:
            return None
        end = kids[0].extent.end.offset
        for t in c.get_tokens():
            if t.extent.start.offset >= end:
                return t.spelling
        return None

    def child_index(self, parent, child):
        for i, ch in enumerate(parent.get_children()):
            if ch == child:
                return i
        return -1

    def access(self, c, chain):
        """-> how the reference c to a global uses it: store, rmw, addr,
        read, test (a read under a comparison or a condition), or unknown
        (a macro hides the operator)."""
        K = self.K
        T = self.ci.TypeKind
        cur = c.type.get_canonical()
        arrays = (T.CONSTANTARRAY, T.INCOMPLETEARRAY, T.VARIABLEARRAY)
        i = len(chain) - 2
        child = c
        while i >= 0:
            p = chain[i]
            if p.kind in (K.PAREN_EXPR, K.UNEXPOSED_EXPR):
                child, i = p, i - 1
                continue
            if p.kind == K.MEMBER_REF_EXPR or (
                    p.kind == K.ARRAY_SUBSCRIPT_EXPR and self.child_index(p, child) == 0):
                if cur.kind == T.POINTER:
                    return "read"
                cur = p.type.get_canonical()
                child, i = p, i - 1
                continue
            break
        if i < 0:
            return "read"
        p = chain[i]
        if p.kind == K.BINARY_OPERATOR:
            op = self.binop(p)
            first = self.child_index(p, child) == 0
            if op == "=" and first:
                return "store"
            if op == "=" or op in BINARY:
                return "test" if op in COMPARE else "read"
            return "unknown"
        if p.kind == K.COMPOUND_ASSIGNMENT_OPERATOR:
            return "rmw" if self.child_index(p, child) == 0 else "read"
        if p.kind == K.UNARY_OPERATOR:
            toks = [t.spelling for t in p.get_tokens()]
            if not toks:
                return "unknown"
            if toks[0] == "&":
                return "addr"
            if toks[0] in ("++", "--") or toks[-1] in ("++", "--"):
                return "rmw"
            if toks[0] == "!":
                return "test"
            if toks[0] in ("-", "+", "~", "*"):
                return "read"
            return "unknown"
        if p.kind in (K.IF_STMT, K.WHILE_STMT, K.CONDITIONAL_OPERATOR) \
                and self.child_index(p, child) == 0:
            return "test"
        if cur.kind in arrays:
            return "addr"
        if p.kind == K.CALL_EXPR and self.tu_is_cpp():
            return self.call_use(p, child, cur)
        return "read"

    def tu_is_cpp(self):
        return self.tu.spelling.endswith((".cpp", ".cc", ".cxx"))

    def call_use(self, call, child, cur):
        """A C++ argument bound to a non-const reference, or a record used
        as a call's object, can write it."""
        T = self.ci.TypeKind
        idx = self.child_index(call, child) - 1
        callee = call.referenced
        if cur.kind == T.RECORD:
            return "addr"
        if callee is not None and idx >= 0:
            params = list(callee.get_arguments())
            if idx < len(params):
                pt = params[idx].type.get_canonical()
                if pt.kind == T.LVALUEREFERENCE \
                        and not pt.get_pointee().is_const_qualified():
                    return "addr"
        return "read"


def static_lines(text):
    """-> the set of line numbers spelling `static`."""
    return {text.count("\n", 0, m.start()) + 1 for m in STATIC_LINE.finditer(text)}


def touched(lo, hi, hunks):
    """An added line in [lo, hi], or a pure deletion after line b with
    lo <= b < hi. hunks are after-side (start, count)."""
    for start, count in hunks:
        if count:
            if start <= hi and start + count - 1 >= lo:
                return True
        elif lo <= start < hi:
            return True
    return False


def scan_tu(ci, tu, plan, ctx, fence, read):
    """-> the plain-data result of one TU for check 2.

    plan: {"hunks": {rel: [(start, count)]}, "stateless": bool (collect the
    population), "stateless_headers": bool (function-local statics of
    headers too), "macros": set of macro names whose body spells static}.

    extents: {usr: (rel, line, name, {gid})} of the touched definitions and
    touched file-scope declarations; globals: the records seen; declared:
    the gids of the members the stateless half collected; seen: the in-scope
    files holding a cursor; defs: the definitions walked."""
    s = Scan(ci, tu, ctx, fence, read)
    K = s.K
    hunks = plan["hunks"]
    extents, seen, declared = {}, set(), set()
    defs = 0
    main = os.path.relpath(tu.spelling, ctx["root"])
    hint_re = None
    if plan.get("macros"):
        hint_re = re.compile(r"\b(%s)\b" % "|".join(sorted(plan["macros"])))
    hints = {}

    def hinted(rel, lo, hi):
        if rel not in hints:
            text = s.text(rel)
            ls = static_lines(text)
            if hint_re is not None:
                ls |= {text.count("\n", 0, m.start()) + 1 for m in hint_re.finditer(text)}
            hints[rel] = ls
        return any(lo <= n <= hi for n in hints[rel])

    for c, rel in s.items():
        seen.add(rel)
        if c.kind == K.MACRO_DEFINITION or c.kind == K.MACRO_INSTANTIATION:
            continue
        lo, hi = c.extent.start.line, c.extent.end.line
        hs = hunks.get(rel)
        hit = bool(hs) and touched(lo, hi, hs)
        if c.kind == K.VAR_DECL:
            rec = s.member(c)
            if rec is not None:
                s.add(rec)
                declared.add(rec["gid"])
            if hit:
                named = s.extent_names(c)
                for g, r in named.items():
                    s.add(r)
                extents[c.get_usr()] = (rel, lo, c.spelling, set(named))
        elif c.kind in s.fns and c.is_definition():
            defs += 1
            if hit:
                named = s.extent_names(c)
                for g, r in named.items():
                    s.add(r)
                extents[c.get_usr()] = (rel, lo, c.spelling, set(named))
            if plan["stateless"] and (rel == main or plan.get("stateless_headers")) \
                    and hinted(rel, lo, hi):
                for b, _ in s.walk_body(c):
                    if b.kind == K.VAR_DECL:
                        rec = s.member(b)
                        if rec is not None:
                            s.add(rec)
                            declared.add(rec["gid"])
    return {"extents": extents, "globals": s.globals, "declared": declared,
            "seen": seen, "defs": defs}


def census_tu(ci, tu, ctx, fence, read):
    """-> every reference of the TU's in-scope definitions and file-scope
    initializers: population records, per-global per-function access kinds,
    direct call edges, functions whose address is taken."""
    s = Scan(ci, tu, ctx, fence, read)
    K = s.K
    refs = {}       # gid -> {fn usr: [kinds in source order]}
    fns = {}        # usr -> (name, rel, line)
    calls = {}      # caller usr -> {callee usr}
    addr_fn = set()
    entry_order = {}  # caller usr of the boot chain -> [(line, callee name, callee usr)]
    seen = set()
    fn_ref = (K.FUNCTION_DECL, K.CXX_METHOD, K.CONSTRUCTOR, K.FUNCTION_TEMPLATE)
    ENTRY = ("main", "openpete_game_main")

    def body(top, usr):
        for c, chain in s.walk_body(top):
            if c.kind == K.CALL_EXPR:
                ref = c.referenced
                if ref is not None and ref.kind in fn_ref and usr:
                    calls.setdefault(usr, set()).add(ref.get_usr())
                    if top.spelling in ENTRY:
                        entry_order.setdefault(usr, []).append(
                            (c.location.line, ref.spelling, ref.get_usr()))
                continue
            if c.kind == K.DECL_REF_EXPR:
                t = c.referenced
                if t is not None and t.kind in fn_ref:
                    if not (len(chain) >= 2 and _callee_of(s, chain)):
                        addr_fn.add(t.get_usr())
                    continue
            rec = s.var_of(c)
            if rec is None:
                continue
            s.add(rec)
            if c.kind == K.DECL_REF_EXPR:
                refs.setdefault(rec["gid"], {}).setdefault(usr, []).append(s.access(c, chain))

    for c, rel in s.items():
        seen.add(rel)
        if c.kind == K.VAR_DECL:
            rec = s.member(c)
            if rec is not None:
                s.add(rec)
            body(c, "")
        elif c.kind in s.fns and c.is_definition():
            usr = c.get_usr()
            fns[usr] = (s.qualified(c), rel, c.location.line)
            calls.setdefault(usr, set())
            body(c, usr)
    return {"globals": s.globals, "refs": refs, "fns": fns, "calls": calls,
            "addr_fn": addr_fn, "entry_order": entry_order, "seen": seen}


def _callee_of(s, chain):
    """True when the reference at the chain's end is the callee of the
    enclosing call."""
    K = s.K
    child = chain[-1]
    i = len(chain) - 2
    while i >= 0 and chain[i].kind in (K.UNEXPOSED_EXPR, K.PAREN_EXPR):
        child = chain[i]
        i -= 1
    return i >= 0 and chain[i].kind == K.CALL_EXPR and s.child_index(chain[i], child) == 0


# --------------------------------------------------------------------------
# The census: merge and confirmation
# --------------------------------------------------------------------------
def merge_census(results):
    """Merge per-TU census results (each a census_tu dict) over every
    database entry."""
    out = {"globals": {}, "refs": {}, "fns": {}, "calls": {}, "addr_fn": set(),
           "entry_order": {}, "seen": set()}
    for r in results:
        out["seen"] |= r["seen"]
        out["addr_fn"] |= r["addr_fn"]
        out["fns"].update(r["fns"])
        for u, cs in r["calls"].items():
            out["calls"].setdefault(u, set()).update(cs)
        for u, lst in r["entry_order"].items():
            out["entry_order"].setdefault(u, set()).update(lst)
        for g, rec in r["globals"].items():
            old = out["globals"].get(g)
            if old is None:
                out["globals"][g] = dict(rec, paths={rec["path"]} if rec["defined"] else set())
                continue
            if not rec["form"]:
                old["form"] = ""
            if rec["defined"]:
                old["paths"].add(rec["path"])
                if not old["defined"]:
                    old.update(path=rec["path"], line=rec["line"], defined=True)
        for g, by_fn in r["refs"].items():
            dst = out["refs"].setdefault(g, {})
            for fn, kinds in by_fn.items():
                # Entries of one function differ only by #ifdef; the kinds
                # are a set, the first kind the order the source spells.
                old = dst.get(fn)
                if old is None:
                    dst[fn] = {"kinds": set(kinds), "first": kinds[0]}
                else:
                    old["kinds"] |= set(kinds)
    return out


WRITES = ("store", "rmw", "addr")


def stores_of(refs):
    """-> {fn usr: {gid}} of the functions writing each global."""
    out = {}
    for g, by_fn in refs.items():
        for fn, info in by_fn.items():
            if fn and info["kinds"] & set(WRITES):
                out.setdefault(fn, set()).add(g)
    return out


def boot_only(fns_writing, data):
    """-> True when every function in fns_writing is reached by direct calls
    only, every chain ending in a boot entry (main,
    openpete_game_main) at a call that precedes openpete_run in that entry,
    and none is address-taken or reached through openpete_run."""
    callers = data["callers"]
    entry_lines = data["entry_lines"]
    stack = list(fns_writing)
    seen = set()
    while stack:
        fn = stack.pop()
        if fn in seen:
            continue
        seen.add(fn)
        if fn in data["addr_fn"] or fn == "c:@F@openpete_run":
            return False
        if fn in entry_lines:
            continue
        up = callers.get(fn)
        if not up:
            return False
        for c in up:
            lim = entry_lines.get(c)
            if lim is not None:
                if data["call_line"].get((c, fn), 1 << 30) >= lim:
                    return False
                continue
            stack.append(c)
    return True


def boot_data(m):
    """-> the call-graph facts boot_only reads."""
    callers = {}
    for caller, cs in m["calls"].items():
        for c in cs:
            callers.setdefault(c, set()).add(caller)
    entry_lines, call_line = {}, {}
    for usr, lst in m["entry_order"].items():
        runs = [ln for ln, name, _ in lst if name == "openpete_run"]
        entry_lines[usr] = min(runs) if runs else 1 << 30
        for ln, _, callee in lst:
            k = (usr, callee)
            call_line[k] = max(call_line.get(k, 0), ln)
    return {"callers": callers, "entry_lines": entry_lines, "call_line": call_line,
            "addr_fn": m["addr_fn"]}


def confirm(m):
    """-> {gid: (form, evidence)} of the local forms confirmed from each
    form-less member, in the order once, atomic, seal.

    once: a scalar initialised to -1, one storing function, whose first
    reference tests it; no address taken, no other global stored there.
    atomic: a scalar no function both reads and stores, no address taken,
    no other mutable global stored in a function that stores it, every
    reference classified. A pointer is never confirmed.
    seal: every write site reached only from a boot entry before
    openpete_run (boot_only)."""
    stores = stores_of(m["refs"])
    boot = boot_data(m)
    out = {}
    for g, rec in m["globals"].items():
        if rec["form"]:
            continue
        by_fn = m["refs"].get(g, {})
        writers = [fn for fn, i in by_fn.items() if i["kinds"] & set(WRITES)]
        kinds = set().union(*[i["kinds"] for i in by_fn.values()]) if by_fn else set()
        co = sorted({o for fn in writers for o in stores.get(fn, ()) if o != g})
        if rec["scalar"] and not rec["pointer"] and "addr" not in kinds \
                and "unknown" not in kinds and "" not in writers and not co:
            if rec["m1"] and len(writers) == 1 \
                    and by_fn[writers[0]]["first"] == "test":
                out[g] = ("once", "= -1, one filler, first reference tests it")
                continue
            both = [fn for fn in writers
                    if by_fn[fn]["kinds"] & {"read", "test", "rmw"}]
            if writers and not both:
                out[g] = ("atomic", "%d storing function(s), none reads it" % len(writers))
                continue
        if writers and "" not in writers and "unknown" not in kinds \
                and boot_only(writers, boot):
            out[g] = ("seal", "%d writer(s), all before openpete_run" % len(writers))
    return out


def naming_counts(m):
    """-> {gid: (functions naming it, files)} over the merged census."""
    out = {}
    for g, by_fn in m["refs"].items():
        fns = [f for f in by_fn if f]
        files = {m["fns"][f][1] for f in fns if f in m["fns"]}
        out[g] = (len(fns), len(files))
    return out


# --------------------------------------------------------------------------
# Baselines
# --------------------------------------------------------------------------
class Baseline:
    """A thread-roles TSV: rows keyed (path, function, name), with kind,
    confirmed form and note columns. An extern's key path is its definition;
    a TU that sees only the declaration matches on the name."""

    def __init__(self, text):
        self.rows = {}
        self.externs = {}
        for cols in lib.parse_rows(text or "", 3):
            cols = (cols + ["", "", ""])[:6]
            self.rows[tuple(cols[:3])] = cols
            if cols[3] == "extern":
                self.externs.setdefault(cols[2], []).append(tuple(cols[:3]))

    def keys_of(self, rec):
        """-> the row keys that match a population record."""
        if rec["kind"] == "extern":
            return self.externs.get(rec["name"], [])
        key = (rec["path"], rec["function"], rec["name"])
        return [key] if key in self.rows else []

    def form_of(self, rec):
        for k in self.keys_of(rec):
            return self.rows[k][4], self.rows[k][5]
        return "", ""


class DeferredList:
    """The read side of baselines/thread-roles/deferred.tsv. check 2 asks
    only `covers(rec)` and `next_step()`, so a precomputed file and one grown
    from annotator runs read the same way."""

    def __init__(self, text):
        self.base = Baseline(text)

    def covers(self, rec):
        return bool(self.base.keys_of(rec))

    def __len__(self):
        return len(self.base.rows)

    @staticmethod
    def next_step():
        return ("Choose the owner: OP_GUARDED_BY(<role>) on every declaration, or "
                "OP_GUARDED_BY_CAP(<lock>) under a module lock, then run "
                "scripts/thread_roles_annotate.py, which writes OP_REQUIRES to a fixed "
                "point. To test every refused global at once: run the commit with "
                "OPENPETE_THREAD_ROLES_REFUSED=<json>, then thread_roles_annotate.py "
                "--batch <json>; a demand that stops at a function with no binding-table "
                "row or exceeds 64 functions is recorded in %s by --write-rows, and the "
                "commit carries %s=\"<reason naming the global or the chain's "
                "terminal>\"." % (DEFERRED, ACCEPT_ENV))


def render_tsv(title, rows, written_by, sha):
    """-> the file text. rows: [[file, function, name, kind, form, note]]."""
    out = ["# " + title, "# key-columns: 3", "# written-by: " + written_by, "# head: " + sha,
           "\t".join(TSV_COLUMNS)]
    out += ["\t".join(r) for r in sorted(rows)]
    return "\n".join(out) + "\n"


def tsv_at(root, rev, path):
    """-> the file's text at rev (`:` for the index), or None when absent."""
    spec = ":" + path if rev == ":" else "%s:%s" % (rev, path)
    p = git(root, "show", spec, check=False)
    return p.stdout if p.returncode == 0 else None


def terminal_of(note):
    """-> the terminal a deferred row's note records (a function, or
    `over-64`), or a string no reason holds."""
    m = re.search(r"\b(?:ORPHAN|OVER) ([\w-]+)", note)
    return m.group(1) if m else "\0"


def added_rows(before, after, renames):
    """-> the keys of `after` absent from `before`, a key whose path a rename
    carries counting as present."""
    carried = {(renames.get(k[0], k[0]), k[1], k[2]) for k in before.rows}
    return sorted(k for k in after.rows if k not in carried)


def accept_refusal(key, note, accept, need_note):
    """-> why the hook's accept reason does not cover one added row with a
    note, or None when it does: the reason names the global (or, for a
    deferred row, its chain's terminal), and a deferred row's note records
    the reason."""
    if key[2] not in accept and not (need_note and terminal_of(note) in accept):
        return "%s must name it or its chain's terminal" % ACCEPT_ENV
    if need_note and accept.strip() not in note:
        return "the row must record the reason in %s" % ACCEPT_ENV
    return None


def accept_line(pending):
    """-> the `OPENPETE_THREAD_ROLES_ACCEPT="..."` assignment that covers
    every (key, note, need_note) in pending, or None when no single value
    does. Tried in order: the global names followed by the notes (enough
    when no deferred row is pending), then each deferred row's note (it
    holds its own terminal), and the first value accept_refusal passes for
    every row is returned."""
    if not pending:
        return None
    names = []
    for k, _, _ in pending:
        if k[2] not in names:
            names.append(k[2])
    notes = []
    for _, note, _ in pending:
        if note not in notes:
            notes.append(note)
    cands = ["%s: %s" % (" ".join(names), "; ".join(notes))]
    cands += [note for _, note, need in pending if need]
    for c in cands:
        if '"' in c or "\\" in c or "$" in c or "`" in c:
            continue
        if all(accept_refusal(k, note, c, need) is None for k, note, need in pending):
            return '%s="%s"' % (ACCEPT_ENV, c)
    return None


def additions_refused(path, before, after, renames, accept, moved_from=None,
                      confirmed=None, need_note=False, pending=None):
    """The compare of one TSV with its base: an added row records its reason
    in the note column, and in the hook the accept reason names it. accept
    None: the stateless half (`check`, `range`), which cannot read a
    commit's environment and reports a row with a note. A row with no note
    is refused in every mode, so a row the hook passed passes a later range.
    moved_from: a TSV whose rows may move here freely (tighten's move out of
    deferred.tsv). confirmed: a TSV whose row with a confirmed local form
    bars the addition. need_note: the note records the annotator's chain.
    pending: a list that gains (key, note, need_note) per row the accept
    reason alone refuses, for accept_line.
    -> (refusals, reports)."""
    refused, reports = [], []
    for k in added_rows(before, after, renames):
        row = after.rows[k]
        label = "%s row %s:%s %s" % (path, k[0], k[1], k[2])
        if moved_from is not None and k in moved_from.rows:
            continue
        if confirmed is not None and k in confirmed.rows and confirmed.rows[k][4] in REMEDY:
            refused.append("%s added, and the census confirms the local form %s; a "
                           "global with a confirmed form is not deferred"
                           % (label, confirmed.rows[k][4]))
            continue
        if not row[5]:
            refused.append("%s added with no %s in its note" % (
                label, "annotator chain" if need_note else "recorded reason"))
            continue
        if accept is None:
            reports.append("%s added since the base: %s" % (label, row[5]))
            continue
        why = accept_refusal(k, row[5], accept, need_note)
        if why is not None:
            refused.append("%s added: %s" % (label, why))
            if pending is not None:
                pending.append((k, row[5], need_note))
    return refused, reports


# --------------------------------------------------------------------------
# The ruling
# --------------------------------------------------------------------------
REMEDY = {
    "atomic": "confirmed form atomic (%s): make the type `_Atomic` (`std::atomic` in C++).",
    "once": "confirmed form OP_ONCE_INT (%s): `static OP_ONCE_INT s = -1;`, read through "
            "op_once_int_get, filled through op_once_int_fill.",
    "seal": "confirmed form OP_SEALED(<seal>) (%s): add the seal and list its writers in "
            "OP_SEAL_TABLE.",
}


def remedy(unannotated, deferred, rec):
    """-> the refusal's next step for one global: its confirmed form when the
    census recorded one, else the owner choice and the deferred route."""
    form, note = unannotated.form_of(rec)
    if form in REMEDY:
        return REMEDY[form] % note
    return deferred.next_step()


def merge_tus(tus):
    """Merge per-TU scan results. Extents are deduplicated by USR, across
    the entries of one source and the TUs including one header."""
    extents, recs, seen, declared = {}, {}, set(), set()
    defs = 0
    for t in tus:
        defs += t["defs"]
        seen |= t["seen"]
        declared |= t["declared"]
        for usr, (rel, line, name, gids) in t["extents"].items():
            cur = extents.get(usr)
            if cur is None:
                extents[usr] = (rel, line, name, set(gids))
            else:
                cur[3].update(gids)
        for g, rec in t["globals"].items():
            old = recs.get(g)
            if old is None:
                recs[g] = dict(rec)
            else:
                if not rec["form"]:
                    old["form"] = ""
                if rec["defined"] and not old["defined"]:
                    old.update(path=rec["path"], line=rec["line"], defined=True)
    return extents, recs, seen, declared, defs


def rule(tus, unannotated, deferred):
    """Check 2's touched-extent half over the merged TUs. -> dict: extents,
    named, passed_form, passed_deferred, refused (gid list), refusal text,
    recs, by_global ({gid: [extent names]}), seen, defs."""
    extents, recs, seen, declared, defs = merge_tus(tus)
    by_global = {}
    for usr, (rel, line, name, gids) in extents.items():
        for g in gids:
            by_global.setdefault(g, []).append("%s:%d %s" % (rel, line, name))
    refused, text = [], []
    formed = dfr = 0
    for g in sorted(by_global):
        rec = recs[g]
        if rec["form"]:
            formed += 1
        elif deferred.covers(rec):
            dfr += 1
        else:
            refused.append(g)
            where = sorted(by_global[g])
            listed = "listed" if unannotated.keys_of(rec) else "new"
            text.append("%s:%d: %s (%s, %s) has no form; touched here: %s%s. %s" % (
                rec["path"], rec["line"], rec["name"], rec["kind"], listed,
                ", ".join(where[:4]), " and %d more" % (len(where) - 4) if len(where) > 4 else "",
                remedy(unannotated, deferred, rec)))
    return {"extents": len(extents), "named": len(by_global), "passed_form": formed,
            "passed_deferred": dfr, "refused": refused, "text": text, "recs": recs,
            "by_global": by_global, "seen": seen, "defs": defs, "declared": declared}


def stateless(recs, declared, unannotated, deferred):
    """The stateless half over the declared members. -> (new, rows): new, a
    line per form-less global in neither TSV; rows, a line per row whose
    global has a form and per global in both TSVs."""
    new, rows = [], []
    for g in sorted(declared):
        rec = recs[g]
        un, de = unannotated.keys_of(rec), deferred.base.keys_of(rec)
        at = "%s:%d: %s" % (rec["path"], rec["line"], rec["name"])
        if rec["form"] and (un or de):
            rows.append("%s has a form and still has a row; delete the row" % at)
        elif not rec["form"] and not un and not de:
            new.append("%s has no form and no row in %s or %s; give it a form. %s" % (
                at, UNANNOTATED, DEFERRED, deferred.next_step()))
        if un and de:
            rows.append("%s is in both TSVs; %s wins, delete the other row" % (at, DEFERRED))
    return new, rows


def stale_rows(base, recs_by_key, paths):
    """-> the rows of a baseline, on the given paths, whose global no scanned
    TU declares any more."""
    return sorted(k for k in base.rows if k[0] in paths and k not in recs_by_key)


def engagement_line(r):
    """The LAW 8 line: extents touched, globals named, refused, passed."""
    return ("  check 2: extents touched %d, globals named %d, refused %d, passed %d "
            "(form %d, deferred %d), definitions walked %d, files seen %d"
            % (r["extents"], r["named"], len(r["refused"]),
               r["passed_form"] + r["passed_deferred"], r["passed_form"],
               r["passed_deferred"], r["defs"], len(r["seen"])))


def code_touched(text, hunks, strip):
    """True when a hunk's added lines hold anything but comments and blank
    lines, or a hunk is a pure deletion (no after-side text to read)."""
    lines = strip(text).split("\n")
    for start, count in hunks:
        if count == 0:
            return True
        for n in range(start, start + count):
            if n - 1 < len(lines) and lines[n - 1].strip():
                return True
    return False


# --------------------------------------------------------------------------
# The include graph
# --------------------------------------------------------------------------
def includers(sources, files, read, headers):
    """-> the sources that reach one of `headers`, over the `#include`
    spellings of every in-scope file, matched by basename. An over-approximate
    graph only selects a TU that then contributes nothing."""
    if not headers:
        return set()
    rev = {}
    for f in files:
        try:
            text = read(f)
        except OSError:
            continue
        for inc in INCLUDE_RE.findall(text):
            rev.setdefault(os.path.basename(inc), set()).add(f)
    seen, stack, out = set(), [os.path.basename(h) for h in headers], set()
    while stack:
        b = stack.pop()
        if b in seen:
            continue
        seen.add(b)
        for f in rev.get(b, ()):
            if is_header(f):
                stack.append(os.path.basename(f))
            elif f in sources:
                out.add(f)
    return out | ({h for h in headers if h in sources})


def static_macros(files, read):
    """-> the names of the in-scope macros whose body spells `static`, so
    the stateless walk finds a local static a macro declares."""
    out = set()
    pat = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(\w+)(?:.*\\\n)*.*", re.M)
    for f in files:
        try:
            text = read(f)
        except OSError:
            continue
        for m in pat.finditer(text):
            if STATIC_LINE.search(m.group(0)):
                out.add(m.group(1))
    return out
