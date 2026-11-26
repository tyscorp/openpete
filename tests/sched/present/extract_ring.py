#!/usr/bin/env python3
"""Copy the scene-ring and display-wrapper functions out of native_render.c, and the state structures out of present_thread.c, for the present-thread interleaving harness.

usage: extract_ring.py <native_render.c> <present_thread.c> <ring_out.c> <pt_state_out.h>

ring_out.c includes ring_prelude.h (the ring's types, statics and stubbed
helpers) and holds each function of RING_FUNCTIONS verbatim, so the harness
runs the exact ring code the engine runs. pt_state_out.h holds the ring's slot
states (RING_DECLS), the constants the structures size by (PT_DECLS) and the text of each structure of PT_STATE as a typedef
of the same layout, so the harness reads present_thread.c's state after the
build makes those statics global.
Refuses (exit 1) when a listed function, structure or declaration is
missing or defined twice.
"""

import re
import sys

RING_FUNCTIONS = [
    "ring_mx",
    "ring_slot_free",
    "openpete_native_scene_publish",
    "openpete_native_scene_next_target",
    "openpete_native_scene_pick",
    "openpete_native_scene_retire",
    "openpete_native_scene_stamp_live",
    "openpete_native_scene_ring_flush",
    "openpete_native_scene_ring_pending",
    "openpete_native_scene_ring_busy",
    "openpete_native_scene_slot_info",
    "openpete_native_scene_slot_ids",
    "openpete_native_display_refresh_hz",
    "openpete_native_display_vsync",
    "openpete_native_scene_ring_free",
    "openpete_native_scene_ring_superseded",
    "openpete_native_scene_ring_report",
    "render_scene_impl",
    "openpete_native_render_scene",
    "openpete_native_render_scene_to_stage",
    "openpete_native_present_stage_acquire",
    "openpete_native_present_stage",
    "openpete_native_display_claim",
    "openpete_native_present_stage_release",
    "openpete_native_stage_count",
    "openpete_native_stage_gen",
    "openpete_native_render_present_outstanding",
    "openpete_native_render_present_wait",
]

PT_STATE = ["s_pt", "s_staged", "s_lat"]

# Declarations of present_thread.c the state structures need, copied whole.
PT_DECLS = [r"^enum \{ LAT_WINDOW[^\n]*\n"]

# Declarations of native_render.c the ring's state needs, copied whole.
RING_DECLS = [r"^enum \{ SS_FREE[^\n]*\n",
              r"^#define NR_RESIZE_IMMEDIATE_GRACE_NS[^\n]*\n",
              r"^#define NR_RESIZE_SETTLE_NS[^\n]*\n"]


def match_brace(text, i):
    """Return the index just past the brace that closes the one at text[i], skipping comments, strings and character literals."""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            i = text.index("\n", i)
            continue
        if text.startswith("/*", i):
            i = text.index("*/", i) + 2
            continue
        if c in "\"'":
            j = i + 1
            while text[j] != c:
                j += 2 if text[j] == "\\" else 1
            i = j + 1
            continue
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    raise SystemExit("extract_ring: unbalanced braces")


def function_text(text, name):
    """Return the definition of function name: from its line's start (a definition at column 0) to its closing brace."""
    pat = re.compile(r"^[A-Za-z_][^;{}()\n]*?\b%s\s*\([^;{]*?\)\s*\{" % re.escape(name), re.M)
    hits = list(pat.finditer(text))
    if not hits:
        raise SystemExit("extract_ring: function %s not defined in the input" % name)
    if len(hits) > 1:
        raise SystemExit("extract_ring: function %s defined %d times in the input" % (name, len(hits)))
    m = hits[0]
    end = match_brace(text, m.end() - 1)
    line = text.count("\n", 0, m.start()) + 1
    return line, text[m.start():end]


def struct_text(text, name):
    """Return the body of `static struct { ... } name;` as `typedef struct { ... } name_t;`."""
    pat = re.compile(r"^static struct \{", re.M)
    found = []
    for m in pat.finditer(text):
        end = match_brace(text, m.end() - 1)
        tail = re.match(r"\s*(\w+)\s*;", text[end:])
        if tail and tail.group(1) == name:
            found.append(text[m.end() - 1:end])
    if not found:
        raise SystemExit("extract_ring: structure %s not defined in the input" % name)
    if len(found) > 1:
        raise SystemExit("extract_ring: structure %s defined %d times in the input" % (name, len(found)))
    return "typedef struct %s %s_t;" % (found[0], name)


def decl_text(text, pattern):
    """Return the one match of a declaration pattern."""
    hits = re.findall(pattern, text, re.M)
    if len(hits) != 1:
        raise SystemExit("extract_ring: declaration /%s/ matched %d times in the input" % (pattern, len(hits)))
    return hits[0]


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    nr_path, pt_path, ring_out, state_out = sys.argv[1:]
    with open(nr_path, encoding="utf-8") as f:
        nr = f.read()
    with open(pt_path, encoding="utf-8") as f:
        pt = f.read()
    parts = ["/* Copied from %s by tests/sched/present/extract_ring.py; do not edit. */" % nr_path,
             '#include "ring_prelude.h"', ""]
    for name in RING_FUNCTIONS:
        line, body = function_text(nr, name)
        parts.append("#line %d \"%s\"" % (line, nr_path))
        parts.append(body)
        parts.append("")
    parts.append('#include "ring_epilogue.h"')
    with open(ring_out, "w", encoding="utf-8") as f:
        f.write("\n".join(parts) + "\n")
    hdr = ["/* Copied from %s by tests/sched/present/extract_ring.py; do not edit. */" % pt_path,
           "#ifndef PT_STATE_H", "#define PT_STATE_H", '#include "pt_state_prelude.h"', ""]
    for pattern in RING_DECLS:
        hdr.append(decl_text(nr, pattern))
    for pattern in PT_DECLS:
        hdr.append(decl_text(pt, pattern))
    for name in PT_STATE:
        hdr.append(struct_text(pt, name))
        hdr.append("extern %s_t %s;" % (name, name))
        hdr.append("")
    hdr.append("#endif")
    with open(state_out, "w", encoding="utf-8") as f:
        f.write("\n".join(hdr) + "\n")


if __name__ == "__main__":
    main()
