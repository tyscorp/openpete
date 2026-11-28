#!/usr/bin/env python3
"""Fence: every host control reaches the game through the knob registry.

The keymap's host actions, the dev overlay's widgets and the settings
menu's rows each change a setting or fire an action. This fence refuses
one that bypasses the registry row that owns it, in four legs:

  keymap  every row of OPENPETE_KEYMAP_HOST_ACTIONS (core/include/
          openpete_keymap.h) names the registry row it drives and an op
          (KM_OP_TOGGLE, _FIRE, _HOLD, _CYCLE_UP, _CYCLE_DOWN), or carries
          NULL and an exempt op (KM_OP_UI_NAV, _FREECAM_INPUT). The row
          exists in every committed surface baseline packaging/surface/
          <game>.json, and its type fits the op: HOLD a row with the hold
          flag, FIRE an action, TOGGLE a bool or enum, CYCLE an int or enum.
          A key_action the registry table writes (core/config/
          knob_registry.c; the registry derives the rest) names a host action.
  direct  outside core/runtime/host_control.c (hc_edge, hc_held,
          hc_count) and the keymap itself, an OPK_ constant of a
          row-driving action appears only as the first argument of an hc_
          read; a key read (openpete_key_pressed/_down/_edge/_pulses/_axis/
          _delta, openpete_keymap_key_binds) whose action argument is not
          an OPK_ constant, an openpete_keymap_scancode_down call, and an
          SDL_SCANCODE_ name that is the default key of a row-driving
          action are refused. A line carrying `host-control: direct
          <reason>` or `host-control: exempt: <reason>` is exempt.
  devui   every ImGui call that edits a value or fires (Button,
          SmallButton, Checkbox, Slider*, Drag*, Input*, Combo, BeginCombo,
          Selectable, MenuItem) in core/ui/devui.cpp carries `host-control:
          <row id>` naming a baseline row or `host-control: exempt:
          <reason>` on its line or on a comment line directly above it, or sits in a function whose definition
          line is preceded by a `// host-control: drawer` line (a row
          drawer: it edits the row it is handed) or by a function-level
          `// host-control: exempt: <reason>` line.
  menu    every settings-menu entry of the baselines names a knob the same
          baseline holds, or is one of MENU_EXEMPT; and core/ui/
          openpete_menu.c changes no setting directly: a call of a function
          a registry row setter calls and no getter calls (core/config/
          knob_registry.c), or of an openpete_ function named *_set*,
          *_toggle* or *_request* outside the menu's own api and the
          settings writer, is refused unless its line carries `host-control:
          exempt: <reason>`. The menu writes a setting through the row
          (menu_row_write); a tree with no such write is VACUOUS.

Scope of the scans: C and C++ under core/ and games/, the decomp checkouts
(games/<g>/<g>/), generated/ and third_party/ excluded. A leg with no
member is VACUOUS and fails.

Run:
  python3 scripts/fences/host_control_coverage.py tree [root]   the working tree (build target)
  python3 scripts/fences/host_control_coverage.py hook          the index, when a staged path is in scope
Exit: 0 clean, 66 a refusal, 65 VACUOUS, 64 an input is missing.

rumble-guard: no-launch, wslenv-guard: no-launch
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

KEYMAP_H = os.path.join("core", "include", "openpete_keymap.h")
DEVUI = os.path.join("core", "ui", "devui.cpp")
SURFACE_GLOB = os.path.join("packaging", "surface", "*.json")
EXTS = (".c", ".h", ".cpp", ".hpp", ".cc")
# The keymap's own store and the helpers read the latch directly.
ALLOWED = {
    KEYMAP_H,
    os.path.join("core", "input", "keymap.c"),
    os.path.join("core", "input", "keymap_sdl.c"),
    os.path.join("core", "runtime", "host_control.c"),
    os.path.join("core", "runtime", "host_control.h"),
}
OPS = {"TOGGLE", "FIRE", "HOLD", "CYCLE_UP", "CYCLE_DOWN"}
EXEMPT_OPS = {"UI_NAV", "FREECAM_INPUT"}
OP_TYPES = {
    "TOGGLE": {"bool", "enum"},
    "FIRE": {"action"},
    "HOLD": {"bool", "int"},
    "CYCLE_UP": {"int", "enum"},
    "CYCLE_DOWN": {"int", "enum"},
}
# Settings-menu rows that name no knob: the overlay entry is navigation, the
# exit row returns to the launcher, and the mod rows act on the mod set.
MENU_EXEMPT = {
    "menu.advanced": "UI_NAV",
    "menu.quit": "the launcher return",
    "menu.modsel": "mod set",
    "menu.moden": "mod set",
}

MENU_C = os.path.join("core", "ui", "openpete_menu.c")
MENU_ROW_WRITE = re.compile(r"\bmenu_row_write\s*\(")
MUTATOR = re.compile(r"\b(openpete_(?!menu_|settings_save_)(?:\w*_)?(?:set|toggle|request)(?:_\w*)?)\s*\(")
# A setter-only callee that reads (a state test, a count, a parse helper) is no write.
READ_NAME = re.compile(r"(?:_active|_count|_parse|_pack|_enabled)$|_get_|_is_")
SETTER_DEF = re.compile(r"^static\s+int\s+(\w+)\s*\(\s*knob_desc_t\s*\*\s*\w+\s*,", re.M)
GETTER_DEF = re.compile(r"^static\s+(?:double|void)\s+(\w+)\s*\(\s*const\s+knob_desc_t\s*\*", re.M)
CALL = re.compile(r"\b(openpete_\w+)\s*\(")

ROW = re.compile(
    r"X\(\s*([A-Z0-9_]+)\s*,\s*\"([a-z0-9_]+)\"\s*,\s*KM_(?:DIGITAL|AXIS)\s*,"
    r"\s*KM_CTX_[A-Z]+\s*,\s*[A-Za-z0-9_|]+\s*,\s*(NULL|\"[a-z0-9_]+\")\s*,"
    r"\s*(KM_OP_[A-Z_]+)\s*,\s*\"([^\"]*)\"")
ROW_START = re.compile(r"^\s*X\(", re.M)
OPK = re.compile(r"\bOPK_([A-Z0-9_]+)\b")
HC_ARG = re.compile(r"\bhc_(?:edge|held|count)\s*\(\s*OPK_([A-Z0-9_]+)")
KEY_READ = re.compile(
    r"\b(openpete_key_(?:pressed|down|edge|pulses|axis|delta)|openpete_keymap_key_binds)"
    r"\s*\(\s*([^,)]*)")
SCANCODE_READ = re.compile(r"\bopenpete_keymap_scancode_down\s*\(")
SCANCODE = re.compile(r"\bSDL_SCANCODE_([A-Z0-9_]+)\b")
WIDGET = re.compile(
    r"\bImGui::(Button|SmallButton|Checkbox|Slider\w*|Drag\w*|Input\w*|Combo|"
    r"BeginCombo|Selectable|MenuItem)\s*\(")
MARKER = re.compile(r"host-control:\s*(direct\b|exempt:|drawer\b|[a-z0-9_]+)")
DEF_LINE = re.compile(r"^(?:static\s+|inline\s+|extern\s+)*[A-Za-z_][\w\s\*&:<>,]*?\b"
                      r"([A-Za-z_]\w*)\s*\([^;]*$")
# Key-token spellings whose SDL scancode name is not the token upper-cased.
SCANCODE_NAMES = {"]": "RIGHTBRACKET", "[": "LEFTBRACKET", "LEFT SHIFT": "LSHIFT",
                  "LEFT CTRL": "LCTRL", "RIGHT SHIFT": "RSHIFT", "RIGHT CTRL": "RCTRL"}


def read(path):
    """Return a text file's content, undecodable bytes replaced."""
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def strip_comments(text):
    """Blank out comments, keeping line breaks and offsets."""
    def blank(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    return re.sub(r"//[^\n]*", blank, text)


def host_actions(root):
    """Return {sym: (id, row or None, op, default)} of the host action rows."""
    text = read(os.path.join(root, KEYMAP_H))
    start = text.index("#define OPENPETE_KEYMAP_HOST_ACTIONS(X)")
    end = text.index("#define OPENPETE_KEYMAP_PAD_ACTIONS(X)")
    body = re.sub(r"\\\n", " ", text[start:end])
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    rows, bad = {}, []
    starts = len(ROW_START.findall(text[start:end]))
    for m in ROW.finditer(body):
        sym, ident, row, op, default = m.groups()
        rows[sym] = (ident, None if row == "NULL" else row.strip('"'), op[len("KM_OP_"):], default)
    if starts != len(rows):
        bad.append(f"keymap: {starts} host rows open in the X-macro, {len(rows)} parse with "
                   "the nine columns (sym, id, kind, ctx, flags, row, op, default, purpose)")
    return rows, bad


def surfaces(root):
    """Return {game: {"knobs": {id: entry}, "menu": [entry]}} from the baselines."""
    out = {}
    for path in sorted(glob.glob(os.path.join(root, SURFACE_GLOB))):
        game = os.path.splitext(os.path.basename(path))[0]
        entries = json.loads(read(path))["entries"]
        knobs = {e["id"][len("knob:"):]: e for e in entries if e["id"].startswith("knob:")}
        menu = [e for e in entries if e["id"].startswith("menu:")]
        out[game] = {"knobs": knobs, "menu": menu}
    return out


def leg_keymap(rows, surf, problems):
    """Check every host action's row and op against the baselines."""
    for sym, (ident, row, op, _d) in sorted(rows.items()):
        if op in EXEMPT_OPS:
            if row is not None:
                problems.append(f"keymap: OPK_{sym} ({ident}) has exempt op {op} and names row {row}")
            continue
        if op not in OPS:
            problems.append(f"keymap: OPK_{sym} ({ident}) has op KM_OP_{op}, not a row op or "
                            "an exemption class")
            continue
        if row is None:
            problems.append(f"keymap: OPK_{sym} ({ident}) has op {op} and no row")
            continue
        for game, s in sorted(surf.items()):
            k = s["knobs"].get(row)
            if k is None:
                problems.append(f"keymap: OPK_{sym} ({ident}) drives row {row}, absent from "
                                f"packaging/surface/{game}.json")
                continue
            if k.get("type") not in OP_TYPES[op]:
                problems.append(f"keymap: OPK_{sym} ({ident}) op {op} on row {row} of type "
                                f"{k.get('type')} ({game})")
            if op == "HOLD" and not k.get("hold"):
                problems.append(f"keymap: OPK_{sym} ({ident}) op HOLD on row {row}, which "
                                f"carries no hold flag ({game})")


REGISTRY_C = os.path.join("core", "config", "knob_registry.c")
KEY_ACTION_LIT = re.compile(r"\.key_action\s*=\s*\"([a-z0-9_]+)\"")


def leg_key_hints(root, rows, problems):
    """Check every key_action the registry table names is a host action id."""
    path = os.path.join(root, REGISTRY_C)
    if not os.path.isfile(path):
        return
    ids = {ident for ident, _r, _o, _d in rows.values()}
    for n, line in enumerate(strip_comments(read(path)).split("\n"), 1):
        for m in KEY_ACTION_LIT.finditer(line):
            if m.group(1) not in ids:
                problems.append(f"keymap: {REGISTRY_C}:{n}: key_action \"{m.group(1)}\" names "
                                "no host action")


def scope_files(root):
    """Yield the repo-relative C and C++ sources the scans read."""
    games_root = os.path.join(root, "games")
    decomps = {os.path.join("games", g, g) for g in os.listdir(games_root)} \
        if os.path.isdir(games_root) else set()
    for top in ("core", "games"):
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            rel_dir = os.path.relpath(dirpath, root)
            dirnames[:] = [d for d in dirnames
                           if d not in ("generated", "third_party", "tests")
                           and os.path.join(rel_dir, d) not in decomps]
            for fn in filenames:
                if fn.endswith(EXTS):
                    yield os.path.join(rel_dir, fn)


def default_scancodes(rows):
    """Return {SDL scancode name: action id} of the row-driving actions' default keys."""
    out = {}
    for _sym, (ident, row, op, default) in rows.items():
        if row is None:
            continue
        for tok in default.split(","):
            tok = tok.strip()
            if not tok or tok == "none" or ":" in tok:
                continue
            name = SCANCODE_NAMES.get(tok.upper(), tok.upper().replace(" ", "_"))
            out[name] = ident
    return out


def line_marker(raw_line):
    """Return the host-control marker word on a line, or None."""
    m = MARKER.search(raw_line)
    return m.group(1) if m else None


def leg_direct(root, rows, problems):
    """Scan every source for a host-action read that bypasses the hc_ helpers; return the reads seen."""
    driving = {sym for sym, (_i, row, _o, _d) in rows.items() if row is not None}
    scan_names = default_scancodes(rows)
    seen = 0
    for rel in scope_files(root):
        if rel in ALLOWED:
            continue
        raw = read(os.path.join(root, rel))
        if "OPK_" not in raw and "openpete_key" not in raw and "SDL_SCANCODE_" not in raw:
            continue
        raw_lines = raw.split("\n")
        for n, line in enumerate(strip_comments(raw).split("\n"), 1):
            marker = line_marker(raw_lines[n - 1])
            exempt = marker in ("direct", "exempt:")
            hc_ok = set(HC_ARG.findall(line))
            for m in OPK.finditer(line):
                sym = m.group(1)
                if sym in rows:
                    seen += 1
                if sym in driving and sym not in hc_ok and not exempt:
                    problems.append(f"direct: {rel}:{n}: OPK_{sym} outside hc_edge/hc_held/"
                                    "hc_count")
            for m in KEY_READ.finditer(line):
                arg = m.group(2).strip()
                if not re.fullmatch(r"OPK_[A-Z0-9_]+", arg) and not exempt:
                    problems.append(f"direct: {rel}:{n}: {m.group(1)}({arg}) reads an action "
                                    "the fence cannot name; mark it `host-control: exempt: "
                                    "<reason>`")
            if SCANCODE_READ.search(line):
                seen += 1
                if not exempt:
                    problems.append(f"direct: {rel}:{n}: openpete_keymap_scancode_down outside "
                                    "the helpers without a `host-control: direct` marker")
            for m in SCANCODE.finditer(line):
                act = scan_names.get(m.group(1))
                if act and not exempt:
                    problems.append(f"direct: {rel}:{n}: SDL_SCANCODE_{m.group(1)} is the "
                                    f"default key of host action {act}")
    return seen


def enclosing_markers(raw_lines):
    """Return per line index the function-level marker of the function it sits in, or None."""
    out = [None] * len(raw_lines)
    current = None
    for i, line in enumerate(raw_lines):
        if line and not line[0].isspace() and not line.startswith(("//", "/*", " *", "*", "#", "}", "{")):
            m = DEF_LINE.match(line)
            if m:
                prev = raw_lines[i - 1] if i else ""
                current = line_marker(prev) if prev.lstrip().startswith("//") else None
        out[i] = current
    return out


def leg_devui(root, surf, problems):
    """Check every editing or firing ImGui call in the dev overlay; return the call count."""
    path = os.path.join(root, DEVUI)
    if not os.path.isfile(path):
        return 0
    raw = read(path)
    raw_lines = raw.split("\n")
    fn_marker = enclosing_markers(raw_lines)
    known = set()
    for s in surf.values():
        known |= set(s["knobs"])
    n_calls = 0
    for n, line in enumerate(strip_comments(raw).split("\n"), 1):
        for m in WIDGET.finditer(line):
            n_calls += 1
            mk = line_marker(raw_lines[n - 1])
            if mk is None and n >= 2 and raw_lines[n - 2].lstrip().startswith("//"):
                mk = line_marker(raw_lines[n - 2])
            if mk is None:
                mk = fn_marker[n - 1]
                if mk not in ("drawer", "exempt:"):
                    problems.append(f"devui: {DEVUI}:{n}: ImGui::{m.group(1)} carries no "
                                    "`host-control: <row id | exempt: reason>` and is outside "
                                    "a row drawer")
                continue
            if mk in ("exempt:", "drawer", "direct"):
                continue
            if mk not in known:
                problems.append(f"devui: {DEVUI}:{n}: host-control names {mk}, no baseline row")
    return n_calls


def function_bodies(text, defn):
    """Yield the body of every function whose definition line matches defn."""
    for m in defn.finditer(text):
        start = text.find("{", m.end())
        if start < 0:
            continue
        depth, i = 0, start
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        yield text[start:i]


def setter_writes(root):
    """Return the functions a registry row setter calls and no row getter calls."""
    path = os.path.join(root, REGISTRY_C)
    if not os.path.isfile(path):
        return set()
    text = strip_comments(read(path))
    sets = {c for b in function_bodies(text, SETTER_DEF) for c in CALL.findall(b)}
    gets = {c for b in function_bodies(text, GETTER_DEF) for c in CALL.findall(b)}
    return {c for c in sets - gets if not READ_NAME.search(c)}


def leg_menu_calls(root, problems):
    """Refuse a settings-menu call that changes a setting outside its registry row; return the row writes seen."""
    path = os.path.join(root, MENU_C)
    if not os.path.isfile(path):
        return 0
    writes = setter_writes(root)
    raw = read(path)
    raw_lines = raw.split("\n")
    row_writes = 0
    for n, line in enumerate(strip_comments(raw).split("\n"), 1):
        if not line.lstrip().startswith("static"):
            row_writes += len(MENU_ROW_WRITE.findall(line))
        if line_marker(raw_lines[n - 1]) == "exempt:":
            continue
        names = {m.group(1) for m in MUTATOR.finditer(line)}
        names |= {c for c in CALL.findall(line) if c in writes}
        for fn in sorted(names):
            problems.append(f"menu: {MENU_C}:{n}: {fn} changes a setting outside its registry "
                            "row; write the row (menu_row_write) or mark the line "
                            "`host-control: exempt: <reason>`")
    return row_writes


def leg_menu(surf, problems):
    """Check every settings-menu entry names a knob or is exempt; return the entry count."""
    n = 0
    for game, s in sorted(surf.items()):
        for e in s["menu"]:
            n += 1
            mid = e["id"][len("menu:"):]
            knob = e.get("knob")
            if knob is None:
                if mid not in MENU_EXEMPT:
                    problems.append(f"menu: {mid} names no knob and is not exempt ({game})")
            elif knob not in s["knobs"]:
                problems.append(f"menu: {mid} names knob {knob}, absent from "
                                f"packaging/surface/{game}.json")
    return n


def check(root):
    """Run every leg on a tree; return the exit code."""
    if not os.path.isfile(os.path.join(root, KEYMAP_H)):
        print(f"host_control_coverage: missing {KEYMAP_H}")
        return 64
    surf = surfaces(root)
    if not surf:
        print(f"host_control_coverage: no baseline matches {SURFACE_GLOB}")
        return 64
    rows, problems = host_actions(root)
    leg_keymap(rows, surf, problems)
    leg_key_hints(root, rows, problems)
    reads = leg_direct(root, rows, problems)
    widgets = leg_devui(root, surf, problems)
    menu = leg_menu(surf, problems)
    menu_writes = leg_menu_calls(root, problems)
    counts = {"host actions": len(rows), "host-action reads": reads,
              "devui widgets": widgets, "menu rows": menu, "menu row writes": menu_writes}
    vacuous = [k for k, v in counts.items() if v == 0]
    for p in problems:
        print("host_control_coverage: " + p)
    if vacuous:
        print("host_control_coverage: VACUOUS: zero " + ", ".join(vacuous))
        return 65
    if problems:
        print(f"host_control_coverage: REFUSED ({len(problems)} finding(s))")
        return 66
    print("host_control_coverage: ok (" + ", ".join(f"{v} {k}" for k, v in counts.items()) + ")")
    return 0


def in_scope(path):
    """Report whether a staged path can change a leg's verdict."""
    return path == KEYMAP_H or path.startswith(("packaging/surface/",)) or (
        path.startswith(("core/", "games/")) and path.endswith(EXTS))


def hook():
    """Check the index when a staged path is in scope."""
    top = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True,
                         text=True, check=True).stdout.strip()
    staged = subprocess.run(["git", "diff", "--cached", "--name-only", "--diff-filter=ACMRD"],
                            capture_output=True, text=True, check=True, cwd=top).stdout.split()
    if not any(in_scope(p) for p in staged):
        return 0
    files = subprocess.run(["git", "ls-files", "--", "core", "games", "packaging/surface"],
                           capture_output=True, text=True, check=True, cwd=top).stdout.split("\n")
    files = [f for f in files if f and (f.endswith(EXTS) or f.endswith(".json"))]
    if KEYMAP_H not in files:
        print(f"host_control_coverage: the index holds no {KEYMAP_H}; nothing to check")
        return 0
    tmp = tempfile.mkdtemp(prefix="hc-coverage-")
    try:
        subprocess.run(["git", "checkout-index", f"--prefix={tmp}/", "--stdin"],
                       input="\n".join(files) + "\n", text=True, check=True, cwd=top,
                       stdout=subprocess.DEVNULL)
        return check(tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "tree"
    if mode == "tree":
        return check(os.path.abspath(sys.argv[2] if len(sys.argv) > 2 else "."))
    if mode == "hook":
        return hook()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
