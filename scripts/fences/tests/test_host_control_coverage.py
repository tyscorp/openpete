#!/usr/bin/env python3
"""Tests for the host-control fence (host_control_coverage.py).

Each case copies the inputs the fence reads (the keymap header, the dev
overlay, the helpers and the surface baselines) from this checkout into a
scratch root, checks the copy passes, then plants one bypass and checks the
leg that owns it refuses: a direct key read of a row-driving action, a
default-key scancode test, an unmarked dev overlay widget, a widget marker
naming no row, a keymap row with a wrong op, a registry key hint naming no
action, a menu entry with no knob, and a settings-menu call that changes a
setting outside its row (by name, and as a row setter's callee).
A root with no source reads, and a menu with no row write, is VACUOUS.

Run: python3 scripts/fences/tests/test_host_control_coverage.py
(rumble-guard: no-launch, wslenv-guard: no-launch)
"""
import glob
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

FENCES = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(FENCES))
sys.path.insert(0, FENCES)

import host_control_coverage as g  # noqa: E402

COPIED = [g.KEYMAP_H, g.DEVUI, g.REGISTRY_C, g.MENU_C,
          os.path.join("core", "runtime", "host_control.c"),
          os.path.join("core", "runtime", "host_hotkeys.c")]
MENU_PUBLIC = "/* --- public -------------------------------------------------------------- */\n"


class HostControlCoverageTest(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="hc-cov-test-")
        for rel in COPIED:
            os.makedirs(os.path.join(self.root, os.path.dirname(rel)), exist_ok=True)
            shutil.copy(os.path.join(ROOT, rel), os.path.join(self.root, rel))
        os.makedirs(os.path.join(self.root, "packaging", "surface"))
        for p in glob.glob(os.path.join(ROOT, g.SURFACE_GLOB)):
            shutil.copy(p, os.path.join(self.root, "packaging", "surface"))

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)

    def run_fence(self):
        out = io.StringIO()
        with redirect_stdout(out):
            rc = g.check(self.root)
        return rc, out.getvalue()

    def plant(self, rel, text):
        path = os.path.join(self.root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)

    def edit(self, rel, old, new):
        path = os.path.join(self.root, rel)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        self.assertIn(old, text)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace(old, new, 1))

    def test_copy_passes(self):
        rc, out = self.run_fence()
        self.assertEqual(rc, 0, out)

    def test_direct_key_read_refused(self):
        self.plant("core/runtime/planted.c",
                   "void f(void) { if (openpete_key_pressed(OPK_SCREENSHOT)) g(); }\n")
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("direct: core/runtime/planted.c:1: OPK_SCREENSHOT", out)

    def test_direct_marker_exempts(self):
        self.plant("core/runtime/planted.c",
                   "void f(void) { if (openpete_key_pressed(OPK_SCREENSHOT)) g(); }"
                   "  // host-control: direct test\n")
        rc, out = self.run_fence()
        self.assertEqual(rc, 0, out)

    def test_helper_read_passes_and_exempt_action_passes(self):
        self.plant("games/common/planted.c",
                   "void f(void) { if (hc_edge(OPK_SCREENSHOT, HC_GATE_MENU)) g();\n"
                   "  if (openpete_key_down(OPK_FLY_UP)) h(); }\n")
        rc, out = self.run_fence()
        self.assertEqual(rc, 0, out)

    def test_action_table_refused(self):
        self.plant("core/ui/planted.c", "static const int k[] = { OPK_PAUSE, OPK_MENU };\n")
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("OPK_PAUSE", out)
        self.assertNotIn("OPK_MENU ", out)

    def test_unnamed_key_read_refused(self):
        self.plant("core/ui/planted.c", "int f(int a) { return openpete_key_down(a); }\n")
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("cannot name", out)

    def test_default_scancode_refused(self):
        self.plant("core/runtime/planted.c",
                   "int f(int sc) { return sc == SDL_SCANCODE_F12; }\n")
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("default key of host action screenshot", out)

    def test_decomp_checkout_ignored(self):
        self.plant("games/spyro-9/spyro-9/src/x.c",
                   "void f(void) { openpete_key_pressed(OPK_SCREENSHOT); }\n")
        rc, out = self.run_fence()
        self.assertEqual(rc, 0, out)

    def test_unmarked_widget_refused(self):
        self.edit(g.DEVUI, "// host-control: exempt: the overlay's search filter\n", "")
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("ImGui::InputTextWithHint carries no", out)

    def test_widget_marker_unknown_row_refused(self):
        self.edit(g.DEVUI, "// host-control: exempt: the overlay's search filter",
                  "// host-control: no_such_row")
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("names no_such_row", out)

    def test_keymap_wrong_op_refused(self):
        self.edit(g.KEYMAP_H, '"screenshot", KM_OP_FIRE', '"screenshot", KM_OP_TOGGLE')
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("op TOGGLE on row screenshot of type action", out)

    def test_keymap_missing_row_refused(self):
        self.edit(g.KEYMAP_H, '"screenshot", KM_OP_FIRE', '"no_such_row", KM_OP_FIRE')
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("drives row no_such_row, absent", out)

    def test_registry_key_hint_unknown_refused(self):
        self.edit(g.REGISTRY_C, '.key_action = "rewind"', '.key_action = "rewnd"')
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn('key_action "rewnd" names no host action', out)

    def test_menu_entry_without_knob_refused(self):
        for p in glob.glob(os.path.join(self.root, g.SURFACE_GLOB)):
            with open(p, encoding="utf-8") as f:
                d = json.load(f)
            for e in d["entries"]:
                if e["id"] == "menu:menu.fov":
                    e["knob"] = None
            with open(p, "w", encoding="utf-8") as f:
                json.dump(d, f)
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("menu: menu.fov names no knob", out)

    def test_menu_direct_toggle_refused(self):
        self.edit(g.MENU_C, MENU_PUBLIC,
                  "static void planted(void) { openpete_freecam_toggle(); }\n" + MENU_PUBLIC)
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("openpete_freecam_toggle changes a setting outside its registry row", out)

    def test_menu_setter_callee_refused(self):
        self.edit(g.MENU_C, MENU_PUBLIC,
                  "static void planted(void) { openpete_turbo_hold(1); }\n" + MENU_PUBLIC)
        rc, out = self.run_fence()
        self.assertEqual(rc, 66, out)
        self.assertIn("openpete_turbo_hold changes a setting", out)

    def test_menu_exempt_marker_passes(self):
        self.edit(g.MENU_C, MENU_PUBLIC,
                  "static void planted(void) { openpete_freecam_toggle(); }"
                  "  /* host-control: exempt: test */\n" + MENU_PUBLIC)
        rc, out = self.run_fence()
        self.assertEqual(rc, 0, out)

    def test_menu_without_row_write_is_vacuous(self):
        path = os.path.join(self.root, g.MENU_C)
        with open(path, encoding="utf-8") as f:
            text = f.read()
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace("menu_row_write(", "menu_row_wrote("))
        rc, out = self.run_fence()
        self.assertEqual(rc, 65, out)
        self.assertIn("zero menu row writes", out)

    def test_no_reads_is_vacuous(self):
        os.remove(os.path.join(self.root, "core", "runtime", "host_hotkeys.c"))
        self.edit(g.DEVUI, "OPK_", "XPK_")
        with open(os.path.join(self.root, g.DEVUI), encoding="utf-8") as f:
            text = f.read().replace("OPK_", "XPK_").replace("SDL_SCANCODE_", "XDL_")
        with open(os.path.join(self.root, g.DEVUI), "w", encoding="utf-8") as f:
            f.write(text)
        rc, out = self.run_fence()
        self.assertEqual(rc, 65, out)
        self.assertIn("VACUOUS", out)


if __name__ == "__main__":
    unittest.main()
