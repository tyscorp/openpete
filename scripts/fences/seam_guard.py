"""Fence: the display seam has one owner, and source outside it may not present.

The display - swapchain, queue, frame lifecycle - is owned by exactly one
thread at a time: the present thread while eager production is active, the
host loop otherwise, with openpete_present_thread_set_active() as the only
handoff. Every defect in this family (a frozen display after a resize, a
silent driver kill after a few interpolation toggles, scenes retired while
the GPU still read their arenas, a fullscreen toggle that no-ops) came from
code touching that state from the wrong side of the handoff. This fence
makes the build refuse the violation instead of describing it in comments.

Three rules, each greppable:

A. The frame/present entry points (rhi_begin_frame*, rhi_end_frame*,
   rhi_present_stage*, rhi_present_wait) may be called only from the files
   that implement the display owner. A new caller anywhere else is a new
   thread with an opinion about the swapchain, which is how every one of
   the defects above started.

B. openpete_native_toggle_fullscreen() may be CALLED only from run.c (the
   host loop, which routes it to the window-owning thread). Everything
   else raises openpete_native_request_fullscreen_toggle() - on Win32 a
   cross-thread SDL_SetWindowFullscreen SendMessages into a pump that may
   be busy, or silently misdelivers the resize.

C. SDL_SetWindowFullscreen / SDL_SetWindowSize are window ops and belong
   to native_render.c (the implementation) and run.c (the window-owning
   loops) only.

A file that legitimately joins the display owner declares it with a marker
comment `seam-guard: display-owner` - a decision someone wrote down, not a
pattern that quietly matched.

The runtime half of this fence lives in rhi_vulkan.c (rhi_display_claim +
the per-entry owner check, OPENPETE_SEAM_GUARD_FATAL=1 to abort): it
catches the interleavings a grep cannot see, this script catches the new
call sites a runtime check would only find once they misbehave.

rumble-guard: no-launch

An allow-list entry that names no file is refused: it is dead after a move
and the file at its new path reads as a violation.

Run: python3 scripts/fences/seam_guard.py [root]
Exit: 0 clean, 65 a file outside the seam touches display-owner state,
67 an allow-list entry names no file.
"""
import os
import re
import sys

RHI_ENTRY = re.compile(
    r"\brhi_(begin_frame|begin_frame_staged|begin_frame_offline|end_frame|end_frame_to_stage|"
    r"present_stage(_acquire|_submit|_release)?|present_wait)\s*\(")
RHI_ALLOW = {
    "core/rhi/rhi.h",              # declarations
    "core/rhi/rhi_vulkan.c",       # the implementation
    "core/rhi/smoketest.c",        # single-threaded bring-up harness
    "core/renderer/native_render.c",  # the display owner's body
    "core/runtime/present_thread.c", # the display owner while eager is active
    "core/include/openpete_native_render.h",  # wrapper declarations
}

FS_TOGGLE = re.compile(r"\bopenpete_native_toggle_fullscreen\s*\(")
FS_ALLOW = {
    "core/runtime/run.c",            # host loop: routes to the window thread
    "core/renderer/native_render.c",  # the implementation itself
    "core/include/openpete_native_render.h",
}

WINDOW_OP = re.compile(r"\bSDL_SetWindow(Fullscreen|Size)\s*\(")
WINDOW_ALLOW = {
    "core/renderer/native_render.c",
    "core/runtime/run.c",
}

EXEMPT = re.compile(r"seam-guard:\s*display-owner")

# "worktrees" holds git worktrees of this repo: whole copies whose paths do
# not match the allow-lists below, so every owner file in one reads as a
# violation. Only the working tree is scanned.
# ".tools" holds downloaded toolchains (clang headers, pcsx-redux): two thirds
# of the C bytes under the root, none of them openpete source. A game's nested
# decomp checkout (games/<game>/<game>/) is PSX code with no RHI access.
SKIP_DIRS = {".git", "worktrees", ".tools", "build", "__pycache__", "third_party",
             "dist", "node_modules", "tests", "generated"}
GAMES_DIR = "games"
SUFFIXES = (".c", ".cpp", ".h", ".hpp")


def stale_allow(root):
    """Allow-list entries that name no file under `root`."""
    return sorted(rel for rel in RHI_ALLOW | FS_ALLOW | WINDOW_ALLOW
                  if not os.path.isfile(os.path.join(root, rel)))


def scan(root):
    bad = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
        parent, game = os.path.split(dirpath)
        if os.path.basename(parent) == GAMES_DIR and game in dirnames:
            dirnames.remove(game)  # the nested decomp checkout
        for name in filenames:
            if not name.endswith(SUFFIXES):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            try:
                text = open(path, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            if EXEMPT.search(text):
                continue
            for rx, allow, what in (
                    (RHI_ENTRY, RHI_ALLOW,
                     "calls a display frame/present entry point"),
                    (FS_TOGGLE, FS_ALLOW,
                     "calls the fullscreen toggle directly (use "
                     "openpete_native_request_fullscreen_toggle)"),
                    (WINDOW_OP, WINDOW_ALLOW,
                     "performs an SDL window op off the window's thread")):
                if rel in allow:
                    continue
                for i, line in enumerate(text.splitlines(), 1):
                    if rx.search(line) and "seam-guard:" not in line:
                        bad.append((rel, i, what, line.strip()))
    return bad


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(__file__), "..", "..")
    root = os.path.abspath(root)
    stale = stale_allow(root)
    if stale:
        print("seam_guard: these allow-list entries name no file; a moved "
              "owner file must move its entry:", file=sys.stderr)
        for rel in stale:
            print(f"  {rel}", file=sys.stderr)
        return 67
    bad = scan(root)
    if not bad:
        return 0
    print("seam_guard: the display has ONE owner; these call sites are "
          "outside it:", file=sys.stderr)
    for rel, line, what, src in bad:
        print(f"  {rel}:{line}: {what}\n      {src}", file=sys.stderr)
    print("\nRoute presents through the scene ring / the owner's wrappers, "
          "window ops through the host loop's request flag. A file that is "
          "genuinely part of the display owner declares it with a "
          "`seam-guard: display-owner` comment.", file=sys.stderr)
    return 65


if __name__ == "__main__":
    sys.exit(main())
