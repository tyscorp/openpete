#!/usr/bin/env python3
"""Fence: the present-thread interleaving harness (tests/sched/present) knows every synchronisation point of the display protocol.

The harness runs present_thread.c, the scene ring copied from
native_render.c and rhi_vulkan.c under a deterministic scheduler: a
synchronisation call is a yield only when the scheduler wraps it, a shared
atomic only when the harness watches its address, and the compose that
render_scene_impl calls is a stand-in. A protocol change the harness does
not know about runs unscheduled, or tests a compose the engine no longer
has, and the harness still passes. Six checks refuse that:

1. Wraps. Every undefined symbol of the instrumented protocol objects in
   the synchronisation, thread, atomic and clock families (SDL mutex,
   condition, rwlock, semaphore, atomic, thread, delay and tick calls,
   pthread_*, sem_*, __atomic_*, op_now, op_sleep*) is wrapped (named in
   the link's --wrap list, with a __wrap_ definition in the harness's
   objects) or is in STUBBED with its reason and defined by the harness.
2. Atomic census. Every _Atomic lvalue and every SDL atomic call in
   present_thread.c, rhi_vulkan.c and the ring copy names an object of
   ATOMICS. A `watched` object's harness token appears in harness.c (its
   address is registered with the scheduler) and no exclusion names it;
   an `excluded` object is excluded in harness.c by sched_watch_exclude;
   `once`, `outside` and `wrap` carry their reason. A row no access
   matches is stale.
3. Extraction. extract_ring.py copies the ring from the current sources
   (it refuses a listed function missing or defined twice).
4. Compose stand-in pin. The protocol events of native_render.c's
   render_frame_common (PIN_CALLS, the s_last_frame_presented stores, and
   every return between the first and last of them), each with the
   condition text of its enclosing if statements and conditional
   operators, equal the stand-in's in tests/sched/present/ring_epilogue.h
   after STANDIN_SUBST. render_scene_impl is copied verbatim by the
   extraction, so check 3 pins it.
5. Source seams. The protocol files carry no op_sched or OP_SCHED token:
   the harness seams them by link-time wraps and instrumentation only, so
   their release objects are untouched.
6. Client set. Every file that calls openpete_present_thread_set_active,
   _borrow or _return, or is on seam_guard.py's RHI_ALLOW list, is a
   harness file or a CLIENTS entry with its reason (the sim driver models
   each client's call sequence).

A change to a protocol file that breaks a negative control fails the
harness build instead (tests/sched/present/apply_mutant.py).

Run:
  python3 scripts/fences/sched_seams.py check --root R --db compile_commands.json
          --nm NM --objects O... --harness-objects H... --wraps W... --ring R.c [--stamp FILE]
  (the build target present_sched_seams and the ctest sched_seams, label fence, pass these)
Exit: 0 clean, 66 a refusal, 64 an input (compile database entry, object, libclang) missing.

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

FENCES = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, FENCES)

PROTOCOL_FILES = ("core/runtime/present_thread.c", "core/renderer/native_render.c", "core/rhi/rhi_vulkan.c")
HARNESS_C = "tests/sched/present/harness.c"
EPILOGUE = "tests/sched/present/ring_epilogue.h"
EXTRACT = "tests/sched/present/extract_ring.py"

FAMILY = re.compile(r"^(SDL_\w*(Mutex|Condition|RWLock|Semaphore|Atomic|Thread)\w*|SDL_Delay\w*|SDL_GetTicks\w*|"
                    r"pthread_\w+|sem_\w+|__atomic_\w+|op_now|op_sleep\w*)$")

# Family symbols the harness defines instead of wrapping, each with the reason it needs no yield.
STUBBED = {
    "op_now": "harness definition: a yield and a read of the virtual clock",
    "op_sleep_until": "harness definition: blocks on the virtual clock",
    "SDL_GetAtomicPointer": "openpete_mutex_once's create-once cache (class once)",
    "SDL_CompareAndSwapAtomicPointer": "openpete_mutex_once's create-once cache (class once)",
    "SDL_SetCurrentThreadPriority": "no synchronisation: a scheduling hint",
}

# Atomic objects of the protocol files: name (or a regex in re:) -> (class, harness token or reason).
ATOMICS = {
    "core/runtime/present_thread.c": {
        "s_pt.active": ("watched", "sched_watch_guarded(&s_pt,"),
        "s_pt.rewarm": ("wrap", "SDL_*AtomicInt wraps; s_pt is watched"),
        "s_pt.render_ema": ("excluded", "relaxed cost average the planner reads"),
        "s_pt.present_ema": ("excluded", "relaxed cost average the planner reads"),
        "s_pt.service_ema": ("excluded", "relaxed cost average the planner reads"),
        "s_pt.presents": ("watched", "sched_watch_guarded(&s_pt,"),
        "s_pt.picks": ("excluded", "relaxed statistic"),
        "s_pt.starved": ("excluded", "relaxed statistic"),
        "re:op_prof_id_\\d+": ("once", "profiler zone id, registered on first use"),
    },
    "core/rhi/rhi_vulkan.c": {
        "s_display_owner": ("watched", "sched_watch(s_display_owner,"),
        "dev.display_changed": ("watched", "\"dev->display_changed\""),
        "s_first_frame_done": ("once", "set at the first present; compile jobs read it"),
        "s_precompile_done": ("once", "set when the precompile walk ends"),
        "s_launcher_frames": ("outside", "launcher presentation flag, set before the host loop"),
        "re:s_lxt_\\w+": ("outside", "launch-timing log window"),
        "re:s_pxt_\\w+": ("outside", "present-timing log window; no protocol decision reads it"),
        "re:s_create_\\w+": ("outside", "pipeline-create hooks and numbering"),
        "s_env_state": ("outside", "compile-environment guard"),
        "s_order": ("outside", "compile-job ordering knob cache"),
        "s_tint": ("outside", "compile-job tint knob cache"),
        "re:s_census(\\.\\w+)?": ("outside", "pipeline census"),
        "re:s_pool_\\w+": ("outside", "compile-pool statistics; the harness runs no compile worker"),
        "s_variant_epoch": ("outside", "pipeline variant epoch"),
        "cache": ("outside", "pipeline-cache handle"),
        "re:(dev|l)\\.lto\\.\\w+|l\\.(direct|probe_miss)": ("outside", "pipeline link-time optimisation counters"),
        "dev.pcache_fu_misses": ("outside", "pipeline-cache statistic"),
        "dev.pipes_by_cmp": ("outside", "pipeline registry statistic"),
        "j.done": ("outside", "compile job completion; the harness runs no compile worker"),
        "dev.shot_pool.state": ("outside", "player screenshot buffer, handed between the display owner and the "
                                           "PNG writer; the compose stand-in queues no screenshot and the "
                                           "harness runs no writer"),
        "v": ("outside", "pipeline-cache entry state"),
    },
    "tests/sched/present/gen/ring_extract.c": {},
}

PIN_CALLS = {
    "rhi_notify_resize_immediate", "rhi_notify_resize", "rhi_begin_frame", "rhi_begin_frame_staged",
    "openpete_native_scene_retire", "rhi_begin_offscreen_load", "rhi_end_offscreen",
    "rhi_blit_to_swapchain", "rhi_end_frame", "rhi_end_frame_to_stage", "rhi_wait_idle",
}
PIN_STORE = "s_last_frame_presented"
# Guards of render_frame_common the stand-in replaces by a scenario variable: engine text -> stand-in text.
STANDIN_SUBST = {
    "native_postfx_point_active ( 0 ) && overlay_was_live && openpete_devui_frame_wanted ( )":
        "pt_standin_overlay_live",
}

HARNESS_FILES = {
    "core/runtime/present_thread.c", "core/renderer/native_render.c", "core/rhi/rhi_vulkan.c",
    "core/rhi/rhi.h", "core/include/openpete_present_thread.h", "core/include/openpete_native_render.h",
    HARNESS_C,
}
CLIENTS = {
    "core/runtime/eager.c": "drv_publish (publish and kick), drv_disengage/set_active(1) (eager_set_active), "
                            "the ring flush",
    "core/runtime/eager.h": "declarations only",
    "core/runtime/host_present.c": "drv_borrow / drv_inline / drv_return (screenshot and capture brackets)",
    "core/runtime/run_capture.c": "drv_borrow / drv_inline / drv_return (capture bracket)",
    "core/rhi/smoketest.c": "single-threaded bring-up harness; no present thread",
}
CLIENT_CALL = re.compile(r"\bopenpete_present_thread_(set_active|borrow|return)\s*\(")
SKIP_DIRS = {"third_party", "build", "worktrees", ".git", "generated"}

# Engagement of the last run: what each check read (a check that read nothing is vacuous).
STATS = {}


class Missing(Exception):
    """An input of the check is absent (exit 64)."""


# ------------------------------------------------------------------ check 1

def nm_symbols(nm, path, undefined):
    """Undefined symbols, or defined global symbols, of an object or archive."""
    if not os.path.exists(path):
        raise Missing("object %s missing" % path)
    args = [nm, "-u", path] if undefined else [nm, "--defined-only", "-g", path]
    out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    return {ln.split()[-1] for ln in out.splitlines() if ln.strip() and not ln.endswith(":")}


def check_wraps(undefined, defined, wraps):
    """Refusals for family symbols of the protocol objects that run unscheduled.

    undefined: {object: set of undefined symbols}; defined: symbols the harness's objects define;
    wraps: symbols the link wraps."""
    out = []
    for obj, syms in sorted(undefined.items()):
        for s in sorted(syms):
            if not FAMILY.match(s):
                continue
            STATS["family symbols"] = STATS.get("family symbols", 0) + 1
            if s in wraps and "__wrap_" + s in defined:
                continue
            if s in STUBBED and s in defined:
                continue
            why = ("wrapped by the link but no __wrap_%s is defined" % s if s in wraps
                   else "listed in STUBBED but the harness does not define it" if s in STUBBED
                   else "not in the link's --wrap list (tests/sched/CMakeLists.txt SCHED_SDL_WRAPS) nor STUBBED")
            out.append("wraps: %s references %s, %s: the call would run unscheduled" % (os.path.basename(obj), s, why))
    return out


# ------------------------------------------------------------------ check 2

def atomic_name(c, K):
    """Dotted name of an accessed object: var, var.field, var.a.b."""
    if c.kind == K.MEMBER_REF_EXPR:
        ch = list(c.get_children())
        return (atomic_name(ch[0], K) + "." if ch else "") + c.spelling
    if c.kind == K.DECL_REF_EXPR:
        return c.spelling
    ch = list(c.get_children())
    return atomic_name(ch[0], K) if ch else "?"


def atomic_census(tu):
    """{object name: [line, ...]} of every _Atomic lvalue and SDL atomic call in the TU's main file."""
    import ratchet_lib as lib
    K = lib.libclang().CursorKind
    main = tu.spelling
    found = {}

    def walk(c):
        for ch in c.get_children():
            f = ch.location.file
            if f is not None and f.name != main:
                continue
            ref = ch.referenced if ch.kind in (K.MEMBER_REF_EXPR, K.DECL_REF_EXPR) else None
            if (ref is not None and ref.kind in (K.VAR_DECL, K.FIELD_DECL)
                    and "_Atomic" in ch.type.get_canonical().spelling):
                found.setdefault(atomic_name(ch, K), []).append(ch.location.line)
                continue
            if ch.kind == K.CALL_EXPR and ch.spelling.startswith("SDL_") and "Atomic" in ch.spelling:
                args = list(ch.get_arguments())
                found.setdefault(atomic_name(args[0], K) if args else "?", []).append(ch.location.line)
            walk(ch)
    walk(tu.cursor)
    return found


def row_for(name, rows):
    """The ATOMICS row key matching an object name, or None."""
    if name in rows:
        return name
    for k in rows:
        if k.startswith("re:") and re.fullmatch(k[3:], name):
            return k
    return None


def check_census(rel, found, rows, harness_text):
    """Refusals for one file's atomic census against its ATOMICS rows and the harness's watch registrations."""
    out = []
    used = set()
    STATS["atomic accesses"] = STATS.get("atomic accesses", 0) + sum(len(v) for v in found.values())
    for name, lines in sorted(found.items()):
        k = row_for(name, rows)
        if k is None:
            out.append("atomics: %s:%d: atomic object %s is unknown to the harness; add an ATOMICS row "
                       "(watched with its harness registration, excluded, once, outside or wrap) and, for "
                       "watched, register its address in %s" % (rel, lines[0], name, HARNESS_C))
            continue
        used.add(k)
        cls, token = rows[k]
        excluded = re.search(r"sched_watch_exclude\(\s*&\s*%s\b" % re.escape(name), harness_text) is not None
        if cls == "watched" and (token not in harness_text or excluded):
            out.append("atomics: %s: %s is classed watched but %s" % (
                rel, name, "the harness excludes it" if excluded else "%s does not register it (%s)" % (HARNESS_C, token)))
        if cls == "excluded" and not excluded:
            out.append("atomics: %s: %s is classed excluded but %s has no sched_watch_exclude(&%s"
                       % (rel, name, HARNESS_C, name))
    for k in sorted(set(rows) - used):
        out.append("atomics: %s: ATOMICS row %s matches no access (stale row)" % (rel, k))
    return out


# ------------------------------------------------------------------ check 4

def tok_text(c):
    """Token text of a cursor, single-spaced."""
    return " ".join(t.spelling for t in c.get_tokens())


def pin_events(fn):
    """The protocol events of a function body: (event, guards) in source order, trimmed to the first..last call or store."""
    import ratchet_lib as lib
    K = lib.libclang().CursorKind
    out = []

    def walk(c, guards):
        kids = list(c.get_children())
        if c.kind == K.IF_STMT and kids:
            cond = tok_text(kids[0])
            walk(kids[0], guards)
            for i, k in enumerate(kids[1:], 1):
                walk(k, guards + [("if" if i == 1 else "else", cond)])
            return
        if c.kind == K.CONDITIONAL_OPERATOR and len(kids) == 3:
            cond = tok_text(kids[0])
            walk(kids[0], guards)
            walk(kids[1], guards + [("?", cond)])
            walk(kids[2], guards + [(":", cond)])
            return
        if c.kind == K.CALL_EXPR and c.spelling in PIN_CALLS:
            out.append((c.spelling, tuple(guards)))
        elif (c.kind == K.BINARY_OPERATOR and kids and kids[0].kind == K.DECL_REF_EXPR
              and kids[0].spelling == PIN_STORE):
            toks = [t.spelling for t in c.get_tokens()]
            if "=" in toks:
                out.append(("%s = %s" % (PIN_STORE, " ".join(toks[toks.index("=") + 1:])), tuple(guards)))
        elif c.kind == K.RETURN_STMT:
            out.append(("return", tuple(guards)))
        for k in kids:
            walk(k, guards)
    walk(fn, [])
    idx = [i for i, (n, _) in enumerate(out) if n != "return"]
    return out[idx[0]:idx[-1] + 1] if idx else []


def find_definition(tu, name, file_suffix):
    """The definition cursor of a function in a TU, located in a file ending in file_suffix, or None."""
    import ratchet_lib as lib
    K = lib.libclang().CursorKind
    for c in tu.cursor.walk_preorder():
        if (c.kind == K.FUNCTION_DECL and c.spelling == name and c.is_definition()
                and c.location.file is not None and c.location.file.name.endswith(file_suffix)):
            return c
    return None


def fmt_event(e):
    """One event as text: name | guard && guard."""
    return "%s | %s" % (e[0], " && ".join("%s(%s)" % g for g in e[1]) or "-")


def check_pin(real, standin, subst):
    """Refusals when the stand-in's events differ from the engine's after the substitutions."""
    mapped = [(n, tuple((k, subst.get(c, c)) for k, c in g)) for n, g in real]
    STATS["pinned events"] = len(real)
    if mapped == list(standin):
        return []
    lines = ["standin: render_frame_common's protocol events differ from the stand-in in %s; update the "
             "stand-in (or STANDIN_SUBST for a guard it models by a scenario variable)" % EPILOGUE]
    n = max(len(mapped), len(standin))
    for i in range(n):
        a = fmt_event(mapped[i]) if i < len(mapped) else "(none)"
        b = fmt_event(standin[i]) if i < len(standin) else "(none)"
        if a != b:
            lines.append("  event %d: engine   %s" % (i + 1, a))
            lines.append("  event %d: stand-in %s" % (i + 1, b))
    return ["\n".join(lines)]


# ------------------------------------------------------------ checks 3, 5, 6

def check_extraction(root, python):
    """Refusals of extract_ring.py run on the current sources."""
    with tempfile.TemporaryDirectory(prefix="sched-seams-") as tmp:
        r = subprocess.run([python, os.path.join(root, EXTRACT), os.path.join(root, PROTOCOL_FILES[1]),
                            os.path.join(root, PROTOCOL_FILES[0]), os.path.join(tmp, "r.c"), os.path.join(tmp, "s.h")],
                           capture_output=True, text=True)
    if r.returncode != 0:
        return ["extraction: %s refused: %s" % (EXTRACT, (r.stderr or r.stdout).strip())]
    return []


def check_tokens(texts):
    """Refusals for source-seam tokens in the protocol files. texts: {rel: text}."""
    out = []
    for rel, text in sorted(texts.items()):
        for i, ln in enumerate(text.splitlines(), 1):
            if re.search(r"\b(op_sched_\w*|OP_SCHED_\w*)", ln):
                out.append("seams: %s:%d: a source seam (%s); the harness seams by wraps and instrumentation, "
                           "so the release object would change" % (rel, i, ln.strip()))
    return out


def source_files(root):
    """Relative paths of C and C++ sources under core/, games/ and tests/, decomp checkouts excluded."""
    for top in ("core", "games", "tests"):
        for d, dirs, files in os.walk(os.path.join(root, top)):
            rel_d = os.path.relpath(d, root)
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS
                       and not re.fullmatch(r"games/spyro-\d/spyro-\d", os.path.join(rel_d, x))]
            for f in files:
                if f.endswith((".c", ".h", ".cpp", ".hpp")):
                    yield os.path.join(rel_d, f)


def check_clients(root, rhi_allow):
    """Refusals for protocol clients the harness's driver does not model."""
    out = []
    seen = set()
    for rel in source_files(root):
        try:
            with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        if CLIENT_CALL.search(text):
            seen.add(rel)
    seen |= set(rhi_allow)
    STATS["client files"] = len(seen)
    for rel in sorted(seen):
        if rel not in HARNESS_FILES and rel not in CLIENTS:
            out.append("clients: %s calls the display handoff (or joins seam_guard's RHI_ALLOW) and is neither "
                       "a harness file nor a CLIENTS entry; model its call sequence in %s's driver and list it "
                       "with the reason" % (rel, HARNESS_C))
    for rel in sorted(CLIENTS):
        if rel not in seen:
            out.append("clients: CLIENTS entry %s no longer calls the handoff (stale row)" % rel)
    return out


# ------------------------------------------------------------------ driver

def db_entry(db, root, rel):
    """The first compile-database entry of a source (relative to root, or absolute)."""
    want = os.path.normpath(rel if os.path.isabs(rel) else os.path.join(root, rel))
    for e in db:
        if os.path.normpath(os.path.join(e["directory"], e["file"])) == want:
            return e
    raise Missing("no compile-database entry for %s" % rel)


def run_check(a):
    """All six checks; -> refusal list."""
    import ratchet_lib as lib
    import seam_guard
    root = os.path.abspath(a.root)
    refusals = []
    undefined = {o: nm_symbols(a.nm, o, True) for o in a.objects}
    defined = set()
    for h in a.harness_objects:
        defined |= nm_symbols(a.nm, h, False)
    refusals += check_wraps(undefined, defined, {w for arg in a.wraps for w in re.split(r"[\s;]+", arg) if w})

    with open(a.db) as f:
        db = json.load(f)
    with open(os.path.join(root, HARNESS_C), encoding="utf-8") as f:
        harness_text = f.read()
    ring_rel = os.path.relpath(os.path.abspath(a.ring), root) if a.ring else None
    tus = {}
    for rel in (PROTOCOL_FILES[0], PROTOCOL_FILES[2], a.ring):
        e = db_entry(db, root, rel)
        tus[rel] = lib.parse_tu(os.path.normpath(os.path.join(e["directory"], e["file"])), lib.entry_flags(e))
    for rel in (PROTOCOL_FILES[0], PROTOCOL_FILES[2]):
        refusals += check_census(rel, atomic_census(tus[rel]), ATOMICS[rel], harness_text)
    refusals += check_census(ring_rel, atomic_census(tus[a.ring]), ATOMICS["tests/sched/present/gen/ring_extract.c"],
                             harness_text)

    refusals += check_extraction(root, sys.executable)

    e = db_entry(db, root, PROTOCOL_FILES[1])
    nr = lib.parse_tu(os.path.normpath(os.path.join(e["directory"], e["file"])), lib.entry_flags(e))
    real = find_definition(nr, "render_frame_common", PROTOCOL_FILES[1])
    stand = find_definition(tus[a.ring], "render_frame_common", EPILOGUE)
    if real is None or stand is None:
        refusals.append("standin: render_frame_common not found in %s" % (PROTOCOL_FILES[1] if real is None else EPILOGUE))
    else:
        refusals += check_pin(pin_events(real), pin_events(stand), STANDIN_SUBST)

    texts = {}
    for rel in PROTOCOL_FILES:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            texts[rel] = f.read()
    refusals += check_tokens(texts)
    refusals += check_clients(root, seam_guard.RHI_ALLOW)
    return refusals


def lib_refusal():
    """ratchet_lib's Refusal (libclang bindings or library missing)."""
    import ratchet_lib as lib
    return lib.Refusal


def main(argv):
    """Parse the command line and run the checks; -> exit status."""
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("mode", choices=["check"])
    p.add_argument("--root", default=".")
    p.add_argument("--db", required=True)
    p.add_argument("--nm", default="nm")
    p.add_argument("--objects", nargs="+", required=True)
    p.add_argument("--harness-objects", nargs="+", required=True)
    p.add_argument("--wraps", nargs="+", required=True)
    p.add_argument("--ring", required=True)
    p.add_argument("--stamp")
    a = p.parse_args(argv)
    try:
        refusals = run_check(a)
        for k in ("family symbols", "atomic accesses", "pinned events", "client files"):
            if not STATS.get(k):
                refusals.append("engagement: the run read no %s (vacuous)" % k)
    except Missing as e:
        print("sched-seams: input missing: %s" % e)
        return 64
    except lib_refusal() as e:
        print("sched-seams: %s" % e)
        return 64
    for r in refusals:
        print("sched-seams: REFUSED: %s" % r)
    if refusals:
        print("sched-seams: %d refusal(s)" % len(refusals))
        return 66
    print("sched-seams: clean (wraps, atomic census, extraction, compose stand-in, source seams, clients): %s"
          % ", ".join("%s %d" % kv for kv in sorted(STATS.items())))
    if a.stamp:
        with open(a.stamp, "w") as f:
            f.write("ok\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
