#!/usr/bin/env python3
"""Fence: GCC -Wall -Wextra warning ratchet.

Every warning GCC reports under -Wall -Wextra in the project's scope is a
row of baselines/warnings/<database>.tsv, one baseline per GCC preset
database (linux-release, windows-mingw), headed by the compiler's
-dumpfullversion. A row may only shrink.

Measurement compiles; it never reads the build. Each in-scope entry of the
database is rewritten: -w and every -W<name>/-Wno-<name> option except the
error set (the OPENPETE_ERROR_SET_* lists of the top-level CMakeLists.txt)
are removed, -Wa,/-Wp,/-Wl, options are kept, and -Wall -Wextra plus the
four diagnostic-format options GCC 10 and 11 both accept are added. Two
passes per entry: the front end (-fsyntax-only) and the middle end (the
entry's own -c and -O, the object replaced by /dev/null). Every compiler
run is under LC_ALL=C with LANGUAGE unset.

Rows. A front-end row is (file, option, normalised message): the message
keeps its quoted identifiers, every digit run becomes N, and the
[-W...] suffix is dropped. A middle-end row, a diagnostic the entry's
front-end pass lacks at the same (file, line, column, option), is (file,
option, enclosing function). A row's count is the number of distinct
counted locations over every entry. The counted location of a diagnostic
inside a macro expansion is its outermost in-scope `in expansion of macro`
note, whose file is the row's file; a diagnostic located outside the scope
takes the first in-scope `inlined from 'F' at L` frame as (file of L,
option, F), else the TU.

Cache. One result per entry under <build dir>/warning-ratchet/, keyed by
the compiler version, the rewritten command (the build-id defines reduced
to their length), this script's content and the content hash of the
entry's -M -MG dependency set. Only entries whose key misses compile.

Entries: the in-scope .c/.cpp entries of the database, and one mod entry
per in-tree mods/<id>/src/*.c and configured game its mod.toml names (no
game key: spyro-1), plus core/mods/sdk/openpete_mod_abi.c per game, compiled
by the database's C compiler with the Windows mod DLL command's flags
(-O2 -g -fno-strict-aliasing, the game's staged SDK). Exempt: the
ratchet_lib UNSCORED sources other than openpete_mod_abi.c, and paths of
games the database's configuration leaves out (their rows are held).

Modes:
  hook              git pre-commit. Runs when an in-scope source, header,
                    mod source, CMake file or this fence is staged.
                    linux-release is always decided, its database
                    configured and its build-produced inputs built on
                    demand (MISSING-INPUT below); windows-mingw likewise
                    when its build directory is configured (its database
                    is configured on demand when CMakeCache.txt exists).
                    windows-mingw SKIPs, naming the reason, only with no
                    mingw toolchain (the C compiler
                    cmake/toolchains/mingw-w64.cmake names is not on PATH)
                    or no configured build/windows-mingw. An entry is altered when its
                    dependency set holds a staged path (every entry, when this
                    fence is staged), or when a staged CMake file changed its
                    command. A rise in a row an altered entry produces is
                    refused, staged file or not; a rise only unaltered entries
                    produce is INHERITED (reported). Only staged files' rows
                    shrink; the hook alone writes and stages the baseline.
  check [--database <name>] [--no-mods] [--ctest]
                    every build of build/linux-release and
                    build/windows-mingw (the target depends on every compiling
                    target); elsewhere SKIP. The whole corpus against HEAD's
                    baseline, over entries whose dependencies are clean against
                    HEAD: any rise or new key refuses. Never stages.
                    --database windows-mingw configures the preset on demand.
  check --armed     exit 1 unless the tracked pre-commit hook calls this fence
                    and the baselines' merge driver is configured.
  tighten [--database <name>] [--accept <key> "<reason>"]...
                    lowers rows to the measured counts; a rise or new key is
                    kept out unless named by --accept, whose reason is written
                    in the row.
  baseline [--database <name>]
                    recapture on a tree clean against HEAD (a compiler version
                    change); commit the file alone.

Bins: DIRTY (dirty paths outside every altered entry's dependencies,
reported); DIRTY-DEP (an unstaged or untracked path in an altered entry's
dependencies) and DIRTY-HEADER (a staged path with unstaged hunks): hook
refuses; VACUOUS (no linux database where the mode does not configure
one, a staged in-scope source in no entry set and not exempt, zero entries
measured, and for `check` no measured entry clean against HEAD; zero rows
decided alone is not vacuous, since a clean entry with no row is decided):
refuses; STALE-DB (a CMake file newer than the database, or an
error-kind diagnostic with no option): refuses, never baselined, except
that the hook configures the preset of a database older than a staged
CMake file again (`cmake --preset <name>` under nice -n 10, the
preset's own database only) and refuses only when that configure fails
or OPENPETE_FENCE_NO_BUILD=1 is set; ERROR (an
error-set diagnostic): refuses; MISSING-INPUT (a header a build rule
generates, or the mod SDK, is absent from the build directory, or a
staged SDK root file differs from its tracked source): the hook, tighten
and baseline build openpete_generated_headers plus the SDK staging targets
in the database's directory under nice -n 10, re-run the -M pass and
refuse only when the build fails or an input is still absent (the build's
seconds are printed on the decision line); `check` (run inside `cmake
--build` of the same directory) and OPENPETE_FENCE_NO_BUILD=1 refuse
naming the targets.

Mod SDK: the mod entries compile against <build>/sdk (openpete_sdk_stage,
which depends on the executable) when it holds every configured game, else
against <build>/mod-sdk-test/sdk, the same files staged by
openpete_mod_sdk_test_root and <tag>_mod_sdk_test_stage without the
executable; those are the targets the on-demand build names.
OPENPETE_FENCE_BUILD_SDK=1 keeps <build>/sdk and builds openpete_sdk_stage.

Escape: `#pragma GCC diagnostic ignored "-W<x>"` (or `warning`) between a
push and a pop, with `// warning-ratchet: <reason>` on the pragma line or
the line above; it silences the build log too. A pragma naming an error-set
member is refused, marked or not.

Environment:
  OPENPETE_WARNING_DB          linux-release database path.
  OPENPETE_WARNING_MINGW_DB    windows-mingw database path.
  OPENPETE_WARNING_JOBS        parallel compiles (default 16, the ratchets' JOBS).
  OPENPETE_FENCE_NO_BUILD=1    refuse MISSING-INPUT and a missing database
                               instead of building or configuring.
  OPENPETE_FENCE_BUILD_JOBS    -j of the on-demand build (default the
                               build tool's).
  OPENPETE_FENCE_BUILD_SDK=1   the mod SDK is <build>/sdk, built by
                               openpete_sdk_stage (and the executable).
  OPENPETE_NO_COMMITS=1        `check --armed` passes unarmed.
"""
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

import ratchet_lib as lib
import ratchet_tsv_merge as tsv_merge
from ratchet_lib import Refusal, say, git, is_header, head_sha

SCRIPT = "warning_ratchet.py"
SCOPE_ROOTS = ("core/", "games/", "tests/", "mods/")
# Edited third-party headers that stay -I (not -isystem) and so are measured.
EXTRA_HEADER_ROOTS = ("third_party/PsyCross/include/", "third_party/libco/")
TSV_DIR = "baselines/warnings"
DATABASES = ("linux-release", "windows-mingw")
MOD_ABI = "core/mods/sdk/openpete_mod_abi.c"
MOD_FLAGS = ["-O2", "-g", "-fno-strict-aliasing"]
CACHE_DIR = "warning-ratchet"
DIAG_OPTS = ["-fno-diagnostics-show-caret", "-fno-diagnostics-show-line-numbers",
             "-fdiagnostics-color=never", "-fdiagnostics-urls=never"]
ADDED = ["-Wall", "-Wextra"]
# Defines whose value changes on every commit; the cache key keeps only
# their length (a format-truncation verdict depends on it).
VOLATILE_DEFINES = re.compile(r"^(-DOPENPETE_BUILD_(?:ID|VERSION)=)(.*)$")
CMAKE_FILE = re.compile(r"(^|/)CMakeLists\.txt$|\.cmake$|^CMakePresets\.json$")
MARKER = re.compile(r"//\s*warning-ratchet:\s*\S")
PRAGMA = re.compile(r'(?:#\s*pragma\s+GCC\s+diagnostic|_Pragma\s*\(\s*"GCC\s+diagnostic)'
                    r'\s+(ignored|warning|error)\s+\\?"(-W[^"\\]+)')
COLUMNS = ["file", "option", "kind", "detail", "count", "accepted"]
KEY_COLUMNS = 4
JOBS = 16


class WarnFence(lib.Fence):
    """The ratchet's scope: the shared roots plus mods/, and the PsyCross and
    libco headers."""

    def in_scope(self, rel):
        if rel.startswith(EXTRA_HEADER_ROOTS):
            return is_header(rel) and not re.search(r"(^|/)(generated|build)/",
                                                    rel[len("third_party/"):])
        return lib.Fence.in_scope(self, rel)

    def root_dirs(self):
        return [r.rstrip("/") for r in SCOPE_ROOTS + EXTRA_HEADER_ROOTS]


FENCE = WarnFence("warning-ratchet", SCRIPT, "", SCOPE_ROOTS, "OPENPETE_WARNING_DB")
in_scope = FENCE.in_scope


def tsv_path(dbname):
    return "%s/%s.tsv" % (TSV_DIR, dbname)


# --------------------------------------------------------------------------
# The error set and the command rewrite
# --------------------------------------------------------------------------
def error_set(root):
    """-> the set of option tokens of OPENPETE_ERROR_SET_COMMON and
    OPENPETE_ERROR_SET_C_ONLY in the top-level CMakeLists.txt."""
    try:
        text = open(os.path.join(root, "CMakeLists.txt")).read()
    except OSError:
        return set()
    out = set()
    for m in re.finditer(r"set\(OPENPETE_ERROR_SET_(?:COMMON|C_ONLY)\s+([^)]*)\)", text):
        out.update(t for t in m.group(1).split() if t.startswith("-W"))
    return out


def error_set_names(errset):
    """-> the warning names the error set promotes (`format`, `nonnull`)."""
    return {t[len("-Werror="):] for t in errset if t.startswith("-Werror=")}


def rewrite(argv, errset):
    """-> (compiler, flags) of a database command: the compiler (a launcher
    such as ccache dropped), the flags without -c, -o <obj>, the source,
    dependency-file options, -w and the non-error-set -W options, then
    -Wall -Wextra and the diagnostic-format options. The source is
    returned by the caller."""
    toks = list(argv)
    while toks and os.path.basename(toks[0]) in ("ccache", "sccache"):
        toks = toks[1:]
    out, skip = [], 0
    for t in toks[1:]:
        if skip:
            skip -= 1
            continue
        if t in ("-o", "-MF", "-MT", "-MQ"):
            skip = 1
        elif t in ("-c", "-w", "-MD", "-MMD", "-M", "-MM", "-MG", "-MP"):
            pass
        elif t.startswith("-W") and not t.startswith(("-Wa,", "-Wp,", "-Wl,")) \
                and t not in errset:
            pass
        elif t.startswith(("-fdiagnostics-format", "-fmax-errors")):
            pass
        else:
            out.append(t)
    return toks[0], out + ADDED + DIAG_OPTS


def key_command(cmd):
    """The command text the cache key hashes: the build-id defines reduced
    to their value's length."""
    out = []
    for t in cmd:
        m = VOLATILE_DEFINES.match(t)
        out.append("%s<%d>" % (m.group(1), len(m.group(2))) if m else t)
    return "\0".join(out)


class Entry:
    """One measured compile: id, source rel, directory, compiler, flags (the
    rewritten command without the source), source path."""

    def __init__(self, eid, rel, directory, cc, flags, src):
        self.id = eid
        self.rel = rel
        self.dir = directory
        self.cc = cc
        self.flags = flags
        self.src = src
        self.deps = []
        self.missing = []
        self.dep_error = ""
        self.key = ""

    def fe(self):
        return [self.cc] + self.flags + ["-fsyntax-only", self.src]

    def me(self):
        return [self.cc] + self.flags + ["-c", self.src, "-o", "/dev/null"]

    def dep(self):
        return [self.cc] + self.flags + ["-M", "-MG", self.src]


def source_of(e):
    toks = e.get("arguments") or shlex.split(e["command"])
    src = os.path.normpath(os.path.join(e["directory"], e["file"]))
    obj = None
    for i, t in enumerate(toks):
        if t == "-o" and i + 1 < len(toks):
            obj = toks[i + 1]
    flags = [t for t in toks if os.path.normpath(os.path.join(e["directory"], t)) != src]
    return toks, flags, src, obj


def db_entries(root, path, dbname, errset):
    """-> [Entry] of the database's in-scope sources, and the C compiler."""
    out, cc_c = [], None
    for rel, e in lib.db_entries(root, path, FENCE):
        toks, flags, src, obj = source_of(e)
        cc, rw = rewrite(flags, errset)
        if rel.endswith(".c") and cc_c is None:
            cc_c = cc
        out.append(Entry("%s|%s|%s" % (dbname, rel, obj or ""), rel, e["directory"],
                         cc, rw, src))
    return out, cc_c


def mod_games(manifest):
    """The games a mod.toml names (the line `game = ...`), as the DLL rule in
    games/spyro-1/CMakeLists.txt reads it; no game key: spyro-1."""
    try:
        text = open(manifest).read()
    except OSError:
        return []
    m = re.search(r"^[ \t]*game[ \t]*=(.*)$", text, re.M)
    if not m:
        return ["spyro-1"]
    return [g for g in ("spyro-1", "spyro-2", "spyro-3") if g in m.group(1)]


def sdk_root(build_dir, games):
    """-> (the SDK directory the mod entries compile against, the targets
    that stage it). <build>/sdk (openpete_sdk_stage) when it holds every game
    of games; else <build>/mod-sdk-test/sdk, the same root files, shaders
    and per-game headers staged by openpete_mod_sdk_test_root and
    <tag>_mod_sdk_test_stage, which do not depend on the executable as
    openpete_sdk_stage does. OPENPETE_FENCE_BUILD_SDK=1 keeps <build>/sdk
    and names openpete_sdk_stage."""
    staged = os.path.join(build_dir, "sdk")
    if os.environ.get("OPENPETE_FENCE_BUILD_SDK", "") not in ("", "0") or all(
            os.path.isdir(os.path.join(staged, "games", g)) for g in games):
        return staged, ["openpete_sdk_stage"]
    return (os.path.join(build_dir, "mod-sdk-test", "sdk"),
            ["openpete_mod_sdk_test_root"]
            + ["%s_mod_sdk_test_stage" % g.replace("-", "") for g in sorted(games)])


def mod_entries(root, db_path, dbname, cc, config, errset):
    """-> ([Entry], {mod rel: [games]}, missing sdk dirs, the targets that
    stage the SDK) of the in-tree mod sources and the SDK's
    openpete_mod_abi.c, per configured game, against sdk_root's directory."""
    out, games_of, missing = [], {}, []
    mods = os.path.join(root, "mods")
    if not cc or not os.path.isdir(mods):
        return out, games_of, missing, []
    pending = []
    games_used = set()
    for mod in sorted(os.listdir(mods)):
        src_dir = os.path.join(mods, mod, "src")
        if not os.path.isdir(src_dir):
            continue
        games = mod_games(os.path.join(mods, mod, "mod.toml"))
        for f in sorted(os.listdir(src_dir)):
            if not f.endswith(".c"):
                continue
            rel = "mods/%s/src/%s" % (mod, f)
            games_of[rel] = games
            for g in games:
                if config.games and g not in config.games:
                    continue
                games_used.add(g)
                pending.append((rel, g))
    sdk, targets = sdk_root(os.path.dirname(os.path.abspath(db_path)), games_used)
    if games_used:
        missing += stale_sdk_files(root, sdk)
    for rel, g in pending:
        out.append(_mod_entry(root, dbname, rel, g, sdk, cc, errset))
    for g in sorted(games_used):
        out.append(_mod_entry(root, dbname, MOD_ABI, g, sdk, cc, errset))
        if not os.path.isdir(os.path.join(sdk, "games", g)):
            missing.append(os.path.join(sdk, "games", g))
    return out, games_of, missing, targets


def stale_sdk_files(root, sdk):
    """-> the staged copies under sdk of the tracked SDK root files
    (cmake/openpete_mod_sdk.cmake OPENPETE_MOD_SDK_ROOT_FILES, the
    ${CMAKE_SOURCE_DIR} members) that are absent or differ from their
    source. The staging targets copy them only when built, so a build
    directory not rebuilt since a header change compiles the mods against
    the old header; such a copy counts as a missing input."""
    try:
        text = open(os.path.join(root, "cmake", "openpete_mod_sdk.cmake"), encoding="utf-8").read()
    except OSError:
        return []
    m = re.search(r"set\(OPENPETE_MOD_SDK_ROOT_FILES(.*?)\)", text, re.S)
    out = []
    for rel in re.findall(r"\$\{CMAKE_SOURCE_DIR\}/(\S+)", m.group(1) if m else ""):
        staged = os.path.join(sdk, os.path.basename(rel))
        if not os.path.isfile(staged):
            out.append(staged)
            continue
        try:
            with open(os.path.join(root, rel), "rb") as a, open(staged, "rb") as b:
                if a.read() == b.read():
                    continue
        except OSError:
            pass
        out.append(staged + " (stale against " + rel + ")")
    return out


def _mod_entry(root, dbname, rel, game, sdk, cc, errset):
    flags = MOD_FLAGS + ["-I" + os.path.join(sdk, "games", game), "-I" + sdk]
    _, rw = rewrite([cc] + flags, errset)
    return Entry("%s|%s|mod:%s" % (dbname, rel, game), rel, root, cc, rw,
                 os.path.join(root, rel))


# --------------------------------------------------------------------------
# Running the compiler
# --------------------------------------------------------------------------
def env():
    e = dict(os.environ, LC_ALL="C")
    e.pop("LANGUAGE", None)
    return e


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, env=env(),
                       errors="replace")
    return p.returncode, p.stdout, p.stderr


def compiler_version(cc):
    try:
        rc, out, err = run([cc, "-dumpfullversion"], "/")
    except OSError as e:
        raise Refusal("compiler %s does not run: %s" % (cc, e))
    if rc != 0:
        raise Refusal("%s -dumpfullversion failed: %s" % (cc, err.strip()))
    return "%s %s" % (os.path.basename(cc), out.strip())


class Hasher:
    """Content hashes of files under the root, (mtime, size) of files
    outside it, memoised per run."""

    def __init__(self, root):
        self.root = os.path.realpath(root) + os.sep
        self.memo = {}

    def __call__(self, path):
        h = self.memo.get(path)
        if h is None:
            try:
                if os.path.realpath(path).startswith(self.root):
                    with open(path, "rb") as fh:
                        h = hashlib.sha1(fh.read()).hexdigest()
                else:
                    st = os.stat(path)
                    h = "%d:%d" % (st.st_mtime_ns, st.st_size)
            except OSError:
                h = "-"
            self.memo[path] = h
        return h


def dep_pass(entries, jobs):
    """Fill entry.deps (absolute paths) and entry.missing (dependencies -MG
    could not find) for every entry."""
    def one(e):
        e.deps, e.dep_error = lib.make_deps(e.dep(), e.dir)
        if e.dep_error:
            return
        e.missing = [d for d in e.deps if not os.path.exists(d)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        list(ex.map(one, entries))


# --------------------------------------------------------------------------
# Diagnostic text
# --------------------------------------------------------------------------
DIAG = re.compile(r"^(.+?):(\d+):(\d+): (warning|error|fatal error|note): (.*)$")
DIAG_NOLOC = re.compile(r"^(\S+?): (warning|error|fatal error): (.*)$")
IN_FUNC = re.compile(r"^(?:(.+?): )?In [\w \-]*?'(.+)':$")
AT_TOP = re.compile(r"^(?:(.+?): )?At (?:top level|global scope):$")
INL_HEAD = re.compile(r"^In [\w \-]*?'(.+)',$")
INL_FRAME = re.compile(r"^\s+inlined from '(.+)' at (.+?):(\d+):(\d+)[,:]$")
OPTION = re.compile(r" \[(-W[^\]]+)\]$")


def parse(stderr, directory):
    """-> [diagnostic dict] of GCC text output: sev, path (absolute; '' for
    a diagnostic without a location), line, col, msg, opt (the -W option,
    -Werror= folded to -W; None when absent), func (the enclosing function),
    inl ([(function, path, line, col)] of the inlining chain, innermost
    first) and macros ([(path, line, col)] of the `in expansion of macro`
    notes, innermost first)."""
    out, cur_func, pend = [], None, None
    last = None
    for line in stderr.splitlines():
        m = IN_FUNC.match(line)
        if m:
            cur_func = m.group(2)
            continue
        if AT_TOP.match(line):
            cur_func = None
            continue
        m = INL_HEAD.match(line)
        if m:
            pend = (m.group(1), [])
            continue
        m = INL_FRAME.match(line)
        if m and pend is not None:
            pend[1].append((m.group(1), os.path.normpath(os.path.join(directory, m.group(2))),
                            int(m.group(3)), int(m.group(4))))
            continue
        m = DIAG.match(line)
        if m:
            path = os.path.normpath(os.path.join(directory, m.group(1)))
            sev, msg = m.group(4), m.group(5)
            if sev == "note":
                if last is not None and msg.startswith("in expansion of macro"):
                    last["macros"].append((path, int(m.group(2)), int(m.group(3))))
                continue
            last = _diag(sev, path, int(m.group(2)), int(m.group(3)), msg,
                         pend[0] if pend else cur_func, pend[1] if pend else [])
            out.append(last)
            pend = None
            continue
        m = DIAG_NOLOC.match(line)
        if m and not line.startswith(("In file included", " ")):
            last = _diag(m.group(2), "", 0, 0, m.group(3), None, [])
            out.append(last)
    return out


def _diag(sev, path, line, col, msg, func, inl):
    m = OPTION.search(msg)
    opt = None
    if m:
        opt = m.group(1)
        msg = msg[:m.start()]
        if opt.startswith("-Werror="):
            opt = "-W" + opt[len("-Werror="):]
    return {"sev": "warning" if sev == "warning" else "error", "path": path,
            "line": line, "col": col, "msg": msg, "opt": opt, "func": func,
            "inl": inl, "macros": []}


def normalise(msg):
    return re.sub(r"\d+", "N", msg).strip()


CLONE = re.compile(r"(\.(?:constprop|part|isra|cold|lto_priv)(?:\.\d+)?)+$")


def function_name(name):
    """A function name without GCC's clone suffixes (`.constprop.0`,
    `.part.0`, `.isra.0`, `.cold`), which follow inlining decisions."""
    return CLONE.sub("", name) if name else "-"


# --------------------------------------------------------------------------
# The measured result of one entry, cached
# --------------------------------------------------------------------------
def measure(e):
    """-> {fe: [diag], me: [diag], rc: (fe rc, me rc)} of one entry."""
    rc1, _, err1 = run(e.fe(), e.dir)
    rc2, _, err2 = run(e.me(), e.dir)
    return {"fe": parse(err1, e.dir), "me": parse(err2, e.dir), "rc": [rc1, rc2],
            "tail": (err2 or err1).strip()[-600:] if (rc1 or rc2) else ""}


def cache_key(e, version, script_hash, hasher):
    h = hashlib.sha256()
    for part in (version, script_hash, e.dir, key_command([e.cc] + e.flags), e.src):
        h.update(part.encode() + b"\0")
    for d in sorted(set(e.deps)):
        h.update(("%s=%s\n" % (d, hasher(d))).encode())
    return h.hexdigest()


def measure_all(entries, cache_dir, version, script_hash, hasher, jobs):
    """-> ({entry id: result}, compiled count, cache hits). Entries with a
    dependency error are not measured."""
    os.makedirs(cache_dir, exist_ok=True)
    results, todo = {}, []
    for e in entries:
        if e.dep_error:
            continue
        e.key = cache_key(e, version, script_hash, hasher)
        path = os.path.join(cache_dir, e.key[:2], e.key + ".json")
        try:
            with open(path) as fh:
                results[e.id] = json.load(fh)
            os.utime(path)
            continue
        except (OSError, ValueError):
            todo.append((e, path))

    def one(item):
        e, path = item
        r = measure(e)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        fd, tmp = tempfile.mkstemp(dir=os.path.dirname(path))
        with os.fdopen(fd, "w") as fh:
            json.dump(r, fh)
        os.replace(tmp, path)
        return e.id, r
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for eid, r in ex.map(one, todo):
            results[eid] = r
    return results, len(todo), len(results) - len(todo)


def prune_cache(cache_dir, days=7):
    """Remove cache files no run read or wrote for `days` days."""
    cut = time.time() - days * 86400
    for dirpath, _, files in os.walk(cache_dir):
        for f in files:
            p = os.path.join(dirpath, f)
            try:
                if os.stat(p).st_mtime < cut:
                    os.unlink(p)
            except OSError:
                pass


# --------------------------------------------------------------------------
# Attribution: diagnostics -> rows
# --------------------------------------------------------------------------
def attribute(d, tu_rel, root, kind):
    """-> (row key (file, option, kind, detail), counted location) of one
    diagnostic, or None when it carries no option."""
    def rel(p):
        r = os.path.relpath(p, root) if p else ""
        return r if not r.startswith("..") else p

    opt = d["opt"]
    if not opt:
        return None
    func = function_name(d["func"])
    for p, ln, col in reversed(d["macros"]):
        r = rel(p)
        if in_scope(r):
            detail = normalise(d["msg"]) if kind == "fe" else func
            return (r, opt, kind, detail), (r, ln, col)
    r = rel(d["path"])
    if d["path"] and in_scope(r):
        detail = normalise(d["msg"]) if kind == "fe" else func
        return (r, opt, kind, detail), (r, d["line"], d["col"])
    for f, p, ln, col in d["inl"]:
        r2 = rel(p)
        if in_scope(r2):
            return ((r2, opt, kind, function_name(f) if kind == "me" else normalise(d["msg"])),
                    (r2, ln, col))
    detail = normalise(d["msg"]) if kind == "fe" else func
    return (tu_rel, opt, kind, detail), (tu_rel, r, d["line"], d["col"])


def rows_of(entries, results, root):
    """-> (counts {key: count}, contributors {key: set(entry id)},
    locations {key: set(location)}, errors {entry id: [text]},
    stale {entry id: [text]}, diagnostics read)."""
    locs, contrib, errors, stale = {}, {}, {}, {}
    n = 0
    for e in entries:
        r = results.get(e.id)
        if r is None:
            continue
        fe_sites = {(d["path"], d["line"], d["col"], d["opt"]) for d in r["fe"]}
        for kind, diags in (("fe", r["fe"]),
                            ("me", [d for d in r["me"] if (d["path"], d["line"], d["col"],
                                                          d["opt"]) not in fe_sites])):
            for d in diags:
                n += 1
                where = "%s:%s:%s: %s %s" % (os.path.relpath(d["path"], root) if d["path"]
                                             else e.rel, d["line"], d["col"],
                                             d["msg"], d["opt"] or "")
                if d["sev"] == "error":
                    (errors if d["opt"] else stale).setdefault(e.id, []).append(where)
                    continue
                a = attribute(d, e.rel, root, kind)
                if a is None:
                    continue
                key, loc = a
                locs.setdefault(key, set()).add(loc)
                contrib.setdefault(key, set()).add(e.id)
        if (r["rc"][0] or r["rc"][1]) and e.id not in errors and e.id not in stale:
            stale.setdefault(e.id, []).append("%s: compile failed: %s" % (e.rel, r["tail"]))
    return ({k: len(v) for k, v in locs.items()}, contrib, locs, errors, stale, n)


def key_text(key):
    return "|".join(key)


# --------------------------------------------------------------------------
# Baseline TSV
# --------------------------------------------------------------------------
def parse_tsv(text):
    """-> (header dict, {key: (count, accepted reason)})."""
    head = lib.parse_header(text)
    rows = {}
    for cols in lib.parse_rows(text, 5):
        if cols[0] == "file":
            continue
        reason = cols[5] if len(cols) > 5 else ""
        rows[tuple(cols[:4])] = (int(cols[4]), reason)
    return head, rows


def render_tsv(dbname, version, rows, written_by, sha):
    out = ["# warning ratchet baseline: GCC -Wall -Wextra rows of the %s database "
           "(scripts/fences/warning_ratchet.py)" % dbname,
           "# compiler: " + version,
           "# options: " + " ".join(ADDED),
           "# key-columns: %d" % KEY_COLUMNS,
           "# written-by: " + written_by, "# head: " + sha, "\t".join(COLUMNS)]
    for key in sorted(rows, key=lambda k: "\t".join(k).replace("\t", ":", 1)):
        count, reason = rows[key]
        out.append("\t".join(list(key) + [str(count), reason]))
    return "\n".join(out) + "\n"


# --------------------------------------------------------------------------
# The decision (pure)
#
#   before     {key: (count, reason)}   the baseline source
#   after      {key: count}             measured
#   contrib    {key: set(entry id)}
#   altered    set(entry id) | None     None: every entry
#   decidable  key -> bool              (check: rows no dirty entry feeds)
#   shrink     key -> bool              (rows the mode may lower)
#   accept     {key text: reason}
# -> (verdict, new rows, bins)
# --------------------------------------------------------------------------
def decide(before, after, contrib, altered, decidable, shrink, accept, retire_missing):
    bins = {b: [] for b in ("RISE", "INHERITED", "ACCEPTED", "TIGHTENED", "RETIRED",
                            "SHRINKABLE")}
    rows = dict(before)
    for key in sorted(set(before) | set(after)):
        if not decidable(key):
            continue
        b = before.get(key, (0, ""))[0]
        a = after.get(key, 0)
        if a > b:
            mine = altered is None or bool(contrib.get(key, set()) & altered)
            t = key_text(key)
            if mine and t in accept:
                rows[key] = (a, accept[t])
                bins["ACCEPTED"].append((t, b, a))
            elif mine:
                bins["RISE"].append((t, b if key in before else None, a))
            else:
                bins["INHERITED"].append((t, b if key in before else None, a))
        elif a < b:
            if shrink(key) or (a == 0 and retire_missing(key)):
                if a == 0:
                    rows.pop(key, None)
                    bins["RETIRED"].append((key_text(key), b, None))
                else:
                    rows[key] = (a, before[key][1])
                    bins["TIGHTENED"].append((key_text(key), b, a))
            else:
                bins["SHRINKABLE"].append((key_text(key), b, a))
    return ("REFUSED" if bins["RISE"] else "PASS"), rows, bins


# --------------------------------------------------------------------------
# Pragmas
# --------------------------------------------------------------------------
def pragma_refusals(lines, path, numbers, errnames):
    """-> refusals of the GCC diagnostic pragmas on the given line numbers:
    one naming an error-set member, or an `ignored`/`warning` one without
    a warning-ratchet marker on its line or the line above."""
    out = []
    for n in numbers:
        if not 0 < n <= len(lines):
            continue
        s = lines[n - 1]
        for m in PRAGMA.finditer(s):
            name = m.group(2)[2:].split("=")[0]
            names = {name, m.group(2)[2:]}
            if names & errnames:
                out.append("%s:%d: the pragma names %s, an error-set member" % (path, n,
                                                                                m.group(2)))
            elif m.group(1) != "error" and not (MARKER.search(s) or (
                    n > 1 and MARKER.search(lines[n - 2]))):
                out.append("%s:%d: `#pragma GCC diagnostic %s \"%s\"` without a "
                           "`// warning-ratchet: <reason>` marker" % (path, n, m.group(1),
                                                                       m.group(2)))
    return out


def read_lines(root, rel):
    try:
        with open(os.path.join(root, rel), errors="replace") as fh:
            return fh.read().split("\n")
    except OSError:
        return []


# --------------------------------------------------------------------------
# One database: entries, dependencies, measurement, rows
# --------------------------------------------------------------------------
class Corpus:
    pass


def load_corpus(root, dbname, db_path, mods=True, jobs=16):
    """Entries, the -M pass, the cached measurement and the rows of one
    database. -> Corpus."""
    c = Corpus()
    c.name, c.db_path = dbname, db_path
    errset = error_set(root)
    c.errset = errset
    c.config = lib.read_configuration(root, db_path)
    FENCE.config = c.config
    t0 = time.time()
    entries, cc = db_entries(root, db_path, dbname, errset)
    c.db_rels = {e.rel for e in entries}
    c.mod_games = {}
    c.missing_sdk = []
    c.sdk_targets = []
    if mods:
        m, c.mod_games, c.missing_sdk, c.sdk_targets = mod_entries(
            root, db_path, dbname, cc, c.config, errset)
        entries += m
    c.entries = entries
    if not entries:
        raise Refusal("VACUOUS: the %s database holds no in-scope entry" % dbname)
    c.version = compiler_version(cc or entries[0].cc)
    dep_pass(entries, jobs)
    c.t_dep = time.time() - t0
    c.missing = sorted({d for e in entries for d in e.missing})
    c.dep_errors = [e for e in entries if e.dep_error]
    c.hasher = Hasher(root)
    c.cache_dir = os.path.join(os.path.dirname(os.path.abspath(db_path)), CACHE_DIR)
    return c


def measure_corpus(c, jobs):
    t0 = time.time()
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), SCRIPT), "rb") as fh:
        script_hash = hashlib.sha1(fh.read()).hexdigest()
    c.results, c.compiled, c.hits = measure_all(c.entries, c.cache_dir, c.version,
                                                script_hash, c.hasher, jobs)
    c.t_measure = time.time() - t0
    return c


def out_of_config(c, rel):
    """True for a path the database's configuration leaves out: lib's rule,
    and a mod source none of whose games is configured."""
    if c.config.out_of_config(rel):
        return True
    games = c.mod_games.get(rel)
    if games is not None and c.config.partial:
        return not (set(games) & set(c.config.games))
    return False


def engagement(c, rows, decided, entries_decided=None):
    counts = [("entries", len(c.entries)), ("compiled", c.compiled), ("cache hits", c.hits)]
    if entries_decided is not None:
        counts.append(("entries decided", entries_decided))
    counts += [("diagnostics read", c.ndiag), ("rows", rows), ("rows decided", decided),
               ("-M pass %.1f s, measure %.1f s" % (c.t_dep, c.t_measure), "")]
    lib.report_engagement(FENCE, [(c.name, counts)])


def clean_measured(c, dirty_ids):
    """-> the number of measured entries that read no dirty path."""
    return sum(1 for e in c.entries if e.id in c.results and e.id not in dirty_ids)


def check_vacuous(c, dirty_ids):
    """-> the VACUOUS refusal of `check`, or None when it engaged.

    `check` decides an entry, not a row: a measured entry that reads no
    dirty path is held to HEAD's baseline, and any row it adds or raises
    refuses, so a clean entry with no diagnostic is a decision. Rows of
    entries that read a dirty path are the hook's to decide at commit. The
    check is vacuous when nothing was measured or every measured entry
    reads a dirty path; zero decided rows alone is not vacuous, since the
    baseline's rows may all lie in edited files."""
    measured = c.compiled + c.hits
    if not measured:
        return "VACUOUS: zero entries measured in %s" % c.name
    if not clean_measured(c, dirty_ids):
        return ("VACUOUS: every one of the %d measured %s entries reads a dirty path, so "
                "none is decided against HEAD; commit or stash the edits, or let the "
                "pre-commit hook decide them" % (measured, c.name))
    return None


def finish_rows(c, root):
    c.after, c.contrib, c.locs, c.errors, c.stale, c.ndiag = rows_of(c.entries, c.results,
                                                                     root)


def compile_failures(c):
    """-> lines of the ERROR and STALE-DB bins."""
    out = []
    for eid, errs in sorted(c.errors.items()):
        out += ["ERROR      %s: %s" % (eid, x) for x in errs[:3]]
    for eid, errs in sorted(c.stale.items()):
        out += ["STALE-DB   %s: %s" % (eid, x) for x in errs[:3]]
    for e in c.dep_errors:
        out.append("STALE-DB   %s: the -M pass failed: %s" % (e.id, e.dep_error))
    return out


def stale_cmake(root, db_path, paths):
    """-> the CMake files among paths newer than the database."""
    try:
        t = os.stat(db_path).st_mtime_ns
    except OSError:
        return []
    out = []
    for p in paths:
        try:
            if os.stat(os.path.join(root, p)).st_mtime_ns > t:
                out.append(p)
        except OSError:
            pass
    return out


def tracked_cmake(root):
    return [f for f in git(root, "ls-files").stdout.split("\n")
            if f and CMAKE_FILE.search(f) and not f.startswith(("third_party/", "games/spyro-1/spyro-1/",
                                                               "games/spyro-2/spyro-2/",
                                                               "games/spyro-3/spyro-3/"))]


def missing_probe(c):
    """-> (the absent build-produced inputs, the SDK staging targets when the
    SDK is absent): lib.ensure_inputs's probe result for a Corpus."""
    return c.missing + c.missing_sdk, c.sdk_targets if c.missing_sdk else []


def ready_corpus(root, dbname, db_path, mods, jobs, build):
    """load_corpus, with the database's absent build-produced inputs built
    on demand (lib.ensure_inputs; the corpus is reloaded after the build,
    since the SDK directory and every -M dependency set may change).
    -> (Corpus, the build note: empty when nothing was built)."""
    held = []

    def probe():
        held[:] = [load_corpus(root, dbname, db_path, mods=mods, jobs=jobs)]
        return missing_probe(held[0])
    _, note = lib.ensure_inputs(FENCE, dbname, db_path, probe, build=build)
    return held[0], note


# --------------------------------------------------------------------------
# hook
# --------------------------------------------------------------------------
MINGW_DB_ENV = "OPENPETE_WARNING_MINGW_DB"
MINGW_TOOLCHAIN = "cmake/toolchains/mingw-w64.cmake"
MINGW_CC_DEFAULT = "x86_64-w64-mingw32-gcc-posix"


def mingw_compiler(root):
    """-> the C compiler the windows-mingw toolchain file sets
    (CMAKE_C_COMPILER), else MINGW_CC_DEFAULT."""
    try:
        with open(os.path.join(root, MINGW_TOOLCHAIN)) as fh:
            text = fh.read()
    except OSError:
        return MINGW_CC_DEFAULT
    m = re.search(r"^\s*set\(\s*CMAKE_C_COMPILER\s+(\S+?)\s*\)", text, re.M)
    return m.group(1) if m else MINGW_CC_DEFAULT


def mingw_database(root):
    """The hook's windows-mingw database. -> (path, None) when it is decided
    here, else (None, the reason it cannot be): no mingw toolchain, or no
    configured build directory. A configured directory (CMakeCache.txt
    present) missing its compile database is configured again."""
    cc = mingw_compiler(root)
    if shutil.which(cc) is None:
        return None, "no mingw toolchain (%s, the C compiler %s names, is not on PATH)" % (
            cc, MINGW_TOOLCHAIN)
    build_dir = os.path.dirname(os.path.join(root, lib.MINGW_DB))
    configured = os.path.isfile(os.path.join(build_dir, "CMakeCache.txt"))
    path = lib.mingw_db(root, FENCE, MINGW_DB_ENV, configure=configured)
    if path is None:
        return None, ("no configured windows-mingw build directory (%s holds no "
                      "CMakeCache.txt; configure it: cmake --preset windows-mingw)"
                      % build_dir)
    return path, None


def baseline_source(root, dbname, staged_text, version, after):
    """-> (source name, header, rows). A staged tighten/baseline file is the
    source when its head is HEAD; else HEAD's file."""
    tsv = tsv_path(dbname)
    if staged_text is not None:
        sh, srows = parse_tsv(staged_text)
        by = sh.get("written-by")
        if by in ("tighten", "baseline"):
            validate_staged(root, dbname, sh, srows, after)
            return "staged (%s)" % by, sh, srows
    htext = lib.head_tsv(root, tsv)
    if htext is None:
        raise Refusal("no %s in HEAD or the index. Run `warning_ratchet.py baseline "
                      "--database %s` on a tree clean against HEAD and commit it "
                      "alone." % (tsv, dbname))
    hh, hrows = parse_tsv(htext)
    return "HEAD", hh, hrows


def validate_staged(root, dbname, head, rows, after):
    tsv = tsv_path(dbname)
    htext = lib.head_tsv(root, tsv)
    hh, hrows = parse_tsv(htext) if htext is not None else ({}, {})
    if head.get("written-by") == "baseline":
        if htext is not None and hh.get("compiler") == head.get("compiler"):
            raise Refusal("staged %s is a `baseline` recapture and HEAD carries one "
                          "under the same compiler; use `tighten`." % tsv)
        return
    if head.get("head") != head_sha(root):
        raise Refusal("staged %s was computed against %s, HEAD is %s. Re-run "
                      "`warning_ratchet.py tighten --database %s`, stage it, commit."
                      % (tsv, head.get("head", "?")[:10], head_sha(root)[:10], dbname))
    for key, (count, reason) in rows.items():
        if reason:
            continue
        if count > hrows.get(key, (0, ""))[0] and count > after.get(key, 0):
            raise Refusal("staged %s raises %s to %d with no accepted reason: a hand "
                          "edit. Use `tighten --accept`." % (tsv, key_text(key), count))


def check_version(c, head, dbname):
    if head.get("compiler") and head.get("compiler") != c.version:
        raise Refusal("the %s baseline was captured under %s; this compiler is %s. "
                      "Recapture on a tree clean against HEAD: `warning_ratchet.py "
                      "baseline --database %s`, and commit the file alone."
                      % (dbname, head.get("compiler"), c.version, dbname))


def print_rise(c, bins, root):
    for t, b, a in bins["RISE"]:
        key = tuple(t.split("|", 3))
        say("    RISE  %s  %s -> %d" % (t, "-" if b is None else b, a))
        locs = sorted(c.locs.get(key, ()), key=str)[:6]
        for loc in locs:
            say("          at %s" % ":".join(str(x) for x in loc))


def hook_database(root, dbname, db_path, staged, staged_cmake, fence_staged, deleted,
                  renames, jobs):
    """Decide one database for the hook. -> (rc, text to stage or None, the
    source paths its entries compile)."""
    tsv = tsv_path(dbname)
    c, note = ready_corpus(root, dbname, db_path, True, jobs, True)
    stale = stale_cmake(root, db_path, staged_cmake)
    if stale:
        # A database older than a staged CMake file holds the commands of the
        # old configuration: configure the preset again and reload the corpus.
        lib.reconfigure_db(root, FENCE, dbname, db_path, stale)
        stale = stale_cmake(root, db_path, staged_cmake)
        if stale:
            raise Refusal("STALE-DB: `cmake --preset %s` left the database older than "
                          "the staged CMake files %s." % (dbname, ", ".join(stale)))
        c, note = ready_corpus(root, dbname, db_path, True, jobs, True)
    staged_abs = {os.path.normpath(os.path.join(root, p)) for p in staged}
    if fence_staged:
        altered = {e.id for e in c.entries}
    else:
        altered = {e.id for e in c.entries if staged_abs & set(e.deps)}
        if staged_cmake:
            altered |= changed_commands(c)
    # DIRTY-DEP: an unstaged or untracked path read by an altered entry.
    dirty = dirty_set(root)
    dirty_abs = {os.path.normpath(os.path.join(root, p)): p for p in dirty}
    dirty_dep = sorted({dirty_abs[d] for e in c.entries if e.id in altered
                        for d in e.deps if d in dirty_abs and dirty_abs[d] not in staged})
    if dirty_dep:
        raise Refusal("DIRTY-DEP: entries this commit alters read unstaged or untracked "
                      "files, and the passes compile the working tree: %s. Stage them, "
                      "or stash them." % ", ".join(dirty_dep[:10]))
    measure_corpus(c, jobs)
    finish_rows(c, root)
    fails = compile_failures(c)
    staged_text = lib.staged_tsv(root, tsv, [("M", tsv, None)]) \
        if tsv in staged_all(root) else None
    source, head, before_all = baseline_source(root, dbname, staged_text, c.version, c.after)
    check_version(c, head, dbname)
    # A staged move carries its rows to the new path.
    before_all = {((renames.get(k[0], k[0]),) + k[1:]): v for k, v in before_all.items()}
    held = {k: v for k, v in before_all.items() if out_of_config(c, k[0])}
    before = {k: v for k, v in before_all.items() if k not in held}
    after = {k: v for k, v in c.after.items() if not out_of_config(c, k[0])}
    staged_set = set(staged) | set(deleted)
    verdict, rows, bins = decide(
        before, after, c.contrib, altered,
        decidable=lambda k: True,
        shrink=lambda k: k[0] in staged_set,
        accept={}, retire_missing=lambda k: k[0] in deleted)
    rows.update(held)
    bins["DIRTY"] = sorted(p for p in dirty if p not in dirty_dep and p not in staged
                           and in_scope(p))
    say("warning-ratchet: %s: baseline source %s, %d altered entries%s" % (
        dbname, source, len(altered), "; " + note if note else ""))
    engagement(c, len(after), len(after.keys() | before.keys()))
    if bins["INHERITED"]:
        say("  INHERITED rises (not from this commit; `check` refuses them; repair: fix, or "
            "`warning_ratchet.py tighten --accept <key> \"<reason>\"`):")
    lib.print_bins({k: v for k, v in bins.items() if k != "RISE"}, quiet=("DIRTY",))
    rc = 0
    if fails:
        say("warning-ratchet: %s: %d compile failure line(s) (ERROR, STALE-DB):"
            % (dbname, len(fails)))
        for f in fails[:20]:
            say("    " + f)
        rc = 1
    if verdict == "REFUSED":
        say("warning-ratchet: REFUSED  rises in rows an altered entry produces (%s):" % dbname)
        print_rise(c, bins, root)
        say("  Fix the warning, or silence it with `#pragma GCC diagnostic ignored "
            "\"-W<x>\"` between push and pop plus `// warning-ratchet: <reason>`.")
        rc = 1
    if c.compiled + c.hits == 0:
        raise Refusal("VACUOUS: zero entries measured in %s" % dbname)
    known = {e.rel for e in c.entries}
    if rc:
        return rc, None, known
    text = None
    if rows != before_all:
        text = render_tsv(dbname, c.version, rows, "hook", head_sha(root))
    elif staged_text is not None:
        text = staged_text
    save_commands(c)
    return 0, text, known


def changed_commands(c):
    """Entry ids whose command differs from the last passing run's snapshot
    (every entry without one)."""
    snap = os.path.join(c.cache_dir, "commands.json")
    try:
        with open(snap) as fh:
            old = json.load(fh)
    except (OSError, ValueError):
        return {e.id for e in c.entries}
    return {e.id for e in c.entries if old.get(e.id) != key_command([e.cc] + e.flags)}


def save_commands(c):
    os.makedirs(c.cache_dir, exist_ok=True)
    snap = os.path.join(c.cache_dir, "commands.json")
    fd, tmp = tempfile.mkstemp(dir=c.cache_dir)
    with os.fdopen(fd, "w") as fh:
        json.dump({e.id: key_command([e.cc] + e.flags) for e in c.entries}, fh)
    os.replace(tmp, snap)


def dirty_set(root):
    """Paths whose working file differs from the index, and untracked paths
    (gitignored ones excluded)."""
    out = set(git(root, "diff", "--name-only").stdout.split("\n"))
    out |= set(git(root, "ls-files", "--others", "--exclude-standard").stdout.split("\n"))
    out.discard("")
    return out


def staged_all(root):
    return set(git(root, "diff", "--cached", "--name-only").stdout.split("\n"))


def mode_hook(root):
    lib.refuse_temporary_index()
    changes, base = lib.staged_changes(root)
    staged = [b or a for s, a, b in changes if s != "D"]
    deleted = {a for s, a, b in changes if s == "D"}
    renames = {a: b for s, a, b in changes if s == "R"}
    fence_files = ("scripts/fences/%s" % SCRIPT, "scripts/fences/ratchet_lib.py")
    scope = [p for p in staged + list(deleted) + list(renames) if in_scope(p)]
    staged_cmake = [p for p in staged if CMAKE_FILE.search(p)
                    and not p.startswith("third_party/")]
    fence_staged = any(p in fence_files for p in staged)
    tsvs = [p for p in staged if p.startswith(TSV_DIR + "/")]
    if not scope and not staged_cmake and not fence_staged:
        for dbname in DATABASES:
            st = lib.staged_tsv(root, tsv_path(dbname), changes)
            if st is not None and lib.parse_header(st).get("written-by") == "hook":
                git(root, "restore", "--staged", "--", tsv_path(dbname))
                git(root, "checkout", "--", tsv_path(dbname), check=False)
                say("warning-ratchet: unstaged a stale hook-written %s" % tsv_path(dbname))
        if not tsvs:
            return 0
    # DIRTY-HEADER: a staged path with unstaged hunks (the commit carries the
    # index blob; the passes read the working file).
    unstaged = set(git(root, "diff", "--name-only").stdout.split("\n"))
    bad = sorted(p for p in staged if p in unstaged and (in_scope(p) or p in tsvs
                                                         or CMAKE_FILE.search(p)))
    if bad:
        raise Refusal("DIRTY-HEADER: staged with unstaged hunks: %s. `git add` them, or "
                      "stash the rest." % ", ".join(bad))
    # The pragma escape, on added lines.
    errnames = error_set_names(error_set(root))
    hunks = lib.added_ranges(lib.diff_hunks(root, base, [p for p in staged if in_scope(p)]))
    prag = []
    for path, ranges in hunks.items():
        lines = read_lines(root, path)
        prag += pragma_refusals(lines, path, [n for a, b in ranges
                                              for n in range(a, b + 1)], errnames)
    if prag:
        raise Refusal("diagnostic pragmas:\n    " + "\n    ".join(prag))
    jobs = lib.jobs("OPENPETE_WARNING_JOBS", JOBS)
    t0 = time.time()
    linux_path = lib.linux_db(root, FENCE)
    mingw_path, mingw_skip = mingw_database(root)
    if mingw_skip:
        say("warning-ratchet: windows-mingw SKIP: %s (land-branch.sh decides it)"
            % mingw_skip)
    rc, texts = 0, {}
    known = set()
    for dbname, path in (("linux-release", linux_path), ("windows-mingw", mingw_path)):
        if path is None:
            continue
        r, text, rels = hook_database(root, dbname, path, staged, staged_cmake,
                                      fence_staged, deleted, renames, jobs)
        rc |= r
        if text is not None:
            texts[dbname] = text
        known |= rels
    # VACUOUS: a staged in-scope source in no entry set and not exempt.
    srcs = [p for p in staged if in_scope(p) and not is_header(p)]
    lost = sorted(p for p in srcs if p not in known and p not in lib.UNSCORED
                  and not FENCE.config.out_of_config(p))
    if lost:
        raise Refusal("VACUOUS: staged sources in no entry set: %s. Wire them into "
                      "CMake and re-run: %s" % (", ".join(lost), lib.CONFIGURE))
    if rc:
        say("warning-ratchet: REFUSED (%.1f s)" % (time.time() - t0))
        return 1
    for dbname, text in texts.items():
        staged_text = lib.staged_tsv(root, tsv_path(dbname), changes)
        lib.write_staged(root, tsv_path(dbname), text, staged_text)
    say("warning-ratchet: PASS (%.1f s)" % (time.time() - t0))
    return 0


# --------------------------------------------------------------------------
# check / tighten / baseline
# --------------------------------------------------------------------------
def parse_db_args(root, argv, configure_mingw):
    """-> (database name, path) of --database <name> or --database-path
    <path>; the default is linux-release (OPENPETE_WARNING_DB)."""
    name, path = None, None
    i = 0
    while i < len(argv):
        if argv[i] == "--database" and i + 1 < len(argv):
            name = argv[i + 1]
            i += 2
        elif argv[i] == "--database-path" and i + 1 < len(argv):
            path = argv[i + 1]
            i += 2
        else:
            i += 1
    if path:
        name = name or os.path.basename(os.path.dirname(os.path.abspath(path)))
        return name, path
    name = name or "linux-release"
    if name == "linux-release":
        return name, os.environ.get(FENCE.db_env) or os.path.join(root, lib.DB_DEFAULT)
    if name == "windows-mingw":
        return name, lib.mingw_db(root, FENCE, MINGW_DB_ENV,
                                  configure=configure_mingw)
    return name, os.path.join(root, "build", name, "compile_commands.json")


def clean_inputs(root, c):
    """-> (dirty in-scope paths, ids of entries reading a dirty path)."""
    dirty = dirty_set(root)
    dabs = {os.path.normpath(os.path.join(root, p)) for p in dirty}
    dirty_ids = {e.id for e in c.entries if dabs & set(e.deps)}
    return dirty, dirty_ids


def corpus_for(root, argv, mode):
    ctest = "--ctest" in argv
    dbname, path = parse_db_args(root, argv, configure_mingw=(mode == "check"))
    if dbname not in DATABASES:
        say("warning-ratchet: SKIP: %s is not a baselined database (%s)"
            % (dbname, ", ".join(DATABASES)))
        sys.exit(64 if ctest else 0)
    linux = dbname == "linux-release"
    if linux and mode != "check" and not os.environ.get(FENCE.db_env) \
            and path == os.path.join(root, lib.DB_DEFAULT):
        lib.configure_db(root, FENCE, dbname, path)
    if path is None or not os.path.isfile(path):
        if linux and not ctest:
            raise Refusal("VACUOUS: no compile database at %s. Run: cmake --preset %s"
                          % (path, dbname))
        say("warning-ratchet: SKIP: no %s compile database" % dbname)
        sys.exit(64 if ctest else 0)
    if mode != "check":
        FENCE.config = lib.read_configuration(root, path)
        lib.refuse_partial(FENCE, mode)
    jobs = lib.jobs("OPENPETE_WARNING_JOBS", JOBS)
    mods = "--no-mods" not in argv
    # check runs inside `cmake --build` of this directory: never builds.
    c, _ = ready_corpus(root, dbname, path, mods, jobs, mode != "check")
    stale = stale_cmake(root, path, tracked_cmake(root))
    if stale:
        raise Refusal("STALE-DB: CMake files newer than the %s database: %s. Re-run "
                      "`cmake --preset %s`." % (dbname, ", ".join(stale[:5]), dbname))
    # The completeness check of the shared ratchets, with the mod sources as
    # entries (openpete_mod_abi.c stays on the UNSCORED list). Under --no-mods
    # the mod sources are not measured but are still accounted for.
    mod_rels = set(c.mod_games)
    if "--no-mods" in argv:
        mod_rels |= {f for f in lib.git(root, "ls-files", "--", "mods/*/src/*.c"
                                        ).stdout.split("\n") if f}
    known = {r: 1 for r in c.db_rels | mod_rels}
    if dbname == "linux-release":
        lib.check_unscored(root, known, FENCE, complete=True)
    measure_corpus(c, jobs)
    finish_rows(c, root)
    return dbname, path, c


def pragma_scan(root):
    errnames = error_set_names(error_set(root))
    out = []
    for rel in sorted(lib.present_scope_files(root, FENCE)):
        lines = read_lines(root, rel)
        if any("diagnostic" in s for s in lines):
            out += pragma_refusals(lines, rel, range(1, len(lines) + 1), errnames)
    return out


def mode_check(root, argv):
    dbname, path, c = corpus_for(root, argv, "check")
    tsv = tsv_path(dbname)
    htext = lib.head_tsv(root, tsv)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `warning_ratchet.py baseline --database "
                      "%s` and commit it." % (tsv, dbname))
    head, before_all = parse_tsv(htext)
    check_version(c, head, dbname)
    dirty, dirty_ids = clean_inputs(root, c)
    held = {k for k in before_all if out_of_config(c, k[0])}
    before = {k: v for k, v in before_all.items() if k not in held}
    after = {k: v for k, v in c.after.items() if not out_of_config(c, k[0])}
    dirty_files = {p for p in dirty}

    def decidable(k):
        return k[0] not in dirty_files and not (c.contrib.get(k, set()) & dirty_ids)
    verdict, _, bins = decide(before, after, c.contrib, None, decidable,
                              shrink=lambda k: False, accept={},
                              retire_missing=lambda k: False)
    n_dec = sum(1 for k in set(before) | set(after) if decidable(k))
    bins["DIRTY"] = sorted(p for p in dirty if in_scope(p))
    say("warning-ratchet: check %s: %d of %d entries clean against HEAD" % (
        dbname, len(c.entries) - len(dirty_ids), len(c.entries)))
    engagement(c, len(after), n_dec, clean_measured(c, dirty_ids))
    lib.print_bins({k: v for k, v in bins.items() if k != "RISE"},
                   quiet=("DIRTY", "SHRINKABLE") if verdict == "PASS" else ("DIRTY",))
    if bins["SHRINKABLE"]:
        say("  `warning_ratchet.py tighten --database %s` lowers the SHRINKABLE rows." % dbname)
    fails = compile_failures(c)
    prag = pragma_scan(root)
    rc = 0
    if fails:
        say("warning-ratchet: %d compile failure line(s) (ERROR, STALE-DB):" % len(fails))
        for f in fails[:20]:
            say("    " + f)
        rc = 1
    if prag:
        say("warning-ratchet: diagnostic pragmas:\n    " + "\n    ".join(prag))
        rc = 1
    if verdict == "REFUSED" or bins["INHERITED"]:
        say("warning-ratchet: REFUSED  rises against HEAD's %s:" % tsv)
        bins["RISE"] += bins["INHERITED"]
        print_rise(c, bins, root)
        say("A rise reached HEAD without the hook (merge, revert, --no-verify), or a "
            "mingw-only diagnostic landed from a tree without that database. Repair: fix "
            "it, or `warning_ratchet.py tighten --database %s --accept <key> \"<reason>\"` "
            "and commit the baseline." % dbname)
        rc = 1
    vacuous = check_vacuous(c, dirty_ids)
    if vacuous:
        raise Refusal(vacuous)
    if rc == 0:
        save_commands(c)
        prune_cache(c.cache_dir)
        say("warning-ratchet: PASS")
    return rc


def parse_accept(argv):
    accept, i = {}, 0
    while i < len(argv):
        if argv[i] == "--accept":
            if i + 2 >= len(argv) or not argv[i + 2].strip():
                raise Refusal("usage: tighten [--database <name>] [--accept <key> "
                              "\"<reason>\"]...")
            accept[argv[i + 1]] = argv[i + 2].strip()
            i += 3
        else:
            i += 1
    return accept


def mode_tighten(root, argv):
    accept = parse_accept(argv)
    dbname, path, c = corpus_for(root, [a for a in argv if a != "--no-mods"], "tighten")
    tsv = tsv_path(dbname)
    htext = lib.head_tsv(root, tsv)
    if htext is None:
        raise Refusal("HEAD carries no %s. Run `baseline`." % tsv)
    head, before = parse_tsv(htext)
    check_version(c, head, dbname)
    dirty, dirty_ids = clean_inputs(root, c)

    def decidable(k):
        return k[0] not in dirty and not (c.contrib.get(k, set()) & dirty_ids)
    verdict, rows, bins = decide(before, c.after, c.contrib, None, decidable,
                                 shrink=lambda k: True, accept=accept,
                                 retire_missing=lambda k: True)
    bins["DIRTY"] = sorted(p for p in dirty if in_scope(p))
    engagement(c, len(c.after), len(rows))
    lib.print_bins({k: v for k, v in bins.items() if k != "RISE"}, quiet=("DIRTY",))
    for t in sorted(set(accept) - {b[0] for b in bins["ACCEPTED"]}):
        say("warning-ratchet: --accept %s names no refused rise; ignored." % t)
    if rows == before:
        say("warning-ratchet: %s is current (%d rows); nothing written." % (tsv, len(rows)))
    else:
        with open(os.path.join(root, tsv), "w") as fh:
            fh.write(render_tsv(dbname, c.version, rows, "tighten", head_sha(root)))
        say("warning-ratchet: wrote %s (%d rows). Stage and commit it." % (tsv, len(rows)))
    if verdict == "REFUSED":
        print_rise(c, bins, root)
        say("Refused rises keep their rows. Accept one with `tighten --accept <key> "
            "\"<reason>\"`.")
        return 1
    return 0


def mode_baseline(root, argv):
    dbname, path, c = corpus_for(root, argv, "baseline")
    tsv = tsv_path(dbname)
    dirty, _ = clean_inputs(root, c)
    bad = sorted(p for p in dirty if in_scope(p))
    if bad:
        raise Refusal("baseline refuses dirty in-scope files (the rows would describe "
                      "uncommitted content): %s" % ", ".join(bad[:10]))
    htext = lib.head_tsv(root, tsv)
    if htext is not None and parse_tsv(htext)[0].get("compiler") == c.version:
        raise Refusal("HEAD's %s was captured under %s already; use `tighten`."
                      % (tsv, c.version))
    fails = compile_failures(c)
    if fails:
        raise Refusal("compile failures:\n    " + "\n    ".join(fails[:20]))
    rows = {k: (v, "") for k, v in c.after.items()}
    os.makedirs(os.path.join(root, TSV_DIR), exist_ok=True)
    with open(os.path.join(root, tsv), "w") as fh:
        fh.write(render_tsv(dbname, c.version, rows, "baseline", head_sha(root)))
    engagement(c, len(rows), len(rows))
    per = {}
    for k, v in rows.items():
        per[(k[1], k[2])] = per.get((k[1], k[2]), 0) + v[0]
    say("warning-ratchet: baseline %s: %d rows, %d locations -> %s" % (
        dbname, len(rows), sum(v[0] for v in rows.values()), tsv))
    for (opt, kind), n in sorted(per.items(), key=lambda kv: -kv[1]):
        say("    %-40s %-2s %d" % (opt, kind, n))
    save_commands(c)
    return 0


def mode_armed(root):
    if os.environ.get("OPENPETE_NO_COMMITS") == "1":
        say("warning-ratchet: OPENPETE_NO_COMMITS=1; hook arming not required.")
        return 0
    why = lib.armed_reason(root, (SCRIPT,))
    arm = lib.ARM
    if not why:
        p = git(root, "config", "--get", "merge.%s.driver" % tsv_merge.DRIVER_NAME,
                check=False)
        if p.stdout.strip() != tsv_merge.DRIVER_CONFIG:
            why = "merge.%s.driver is not configured" % tsv_merge.DRIVER_NAME
            arm = tsv_merge.ARM_MERGE
    if why:
        say("warning-ratchet: DISARMED: %s.\n  Arm this clone once:  %s" % (why, arm))
        return 1
    say("warning-ratchet: ARMED (%s)" % lib.HOOK_REL)
    return 0


def dispatch(root, mode, argv):
    if mode == "hook":
        return mode_hook(root)
    if mode == "check" and "--armed" in argv:
        return mode_armed(root)
    if mode == "check":
        return mode_check(root, argv[1:])
    if mode == "tighten":
        return mode_tighten(root, argv[1:])
    if mode == "baseline":
        return mode_baseline(root, argv[1:])
    return None


if __name__ == "__main__":
    sys.exit(lib.run(FENCE, dispatch, __doc__))
