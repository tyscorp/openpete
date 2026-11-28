#!/usr/bin/env python3
"""Fence: the keymap's KM_LEVEL and KM_DELTA flags match their consumers.

The X-macro in core/include/openpete_keymap.h carries two flags the parser
enforces against mouse sources:

  KM_LEVEL   the action is consumed through openpete_key_down() or
             hc_held(); a wheel pulse source has no level and is rejected
             on it.
  KM_DELTA   the action is consumed through openpete_key_delta(); a mouse
             motion source is accepted only on it.

Neither flag can be checked by keymap_test, which never sees the consumers.
This fence reads both directions off the source tree:

  - every openpete_key_down(OPK_X) or hc_held(OPK_X, ...) site names an
    action flagged KM_LEVEL, and every KM_LEVEL action has such a site (a
    flag with no consumer would reject a binding that could have worked);
  - the same for openpete_key_delta(OPK_X) and KM_DELTA;
  - every hc_count(OPK_X, ...) site, an edge-and-pulse read, names an
    action without KM_LEVEL, since the flag rejects the wheel source the
    count reads.

The host rows carry nine columns (sym, id, kind, ctx, flags, row, op,
default, purpose); the row and op columns are parsed so a row that lost
them fails here as well as in host_control_coverage.py.

It scans core/ and games/ excluding tests/ and third_party/, and refuses to
pass when either population enumerates zero members.

Run: python3 scripts/fences/keymap_flags.py [repo root]
Exit: 0 clean, 67 a flag and its consumers disagree, 64 the header is
      missing or a population is empty.
"""
import os
import re
import sys

ROW = re.compile(
    r"^\s*X\(\s*([A-Z0-9_]+)\s*,\s*\"[a-z0-9_]+\"\s*,\s*KM_(?:DIGITAL|AXIS)\s*,"
    r"\s*KM_CTX_[A-Z]+\s*,\s*([A-Za-z0-9_|]+)\s*,\s*(NULL|\"[a-z0-9_]+\")\s*,"
    r"\s*(KM_OP_[A-Z_]+)\s*,",
    re.M,
)
ROW_START = re.compile(r"^\s*X\(", re.M)
SITE_DOWN = re.compile(
    r"\b(?:openpete_key_down\(\s*OPK_([A-Z0-9_]+)\s*\)|hc_held\(\s*OPK_([A-Z0-9_]+)\s*,)")
SITE_DELTA = re.compile(r"\bopenpete_key_delta\(\s*OPK_([A-Z0-9_]+)\s*\)")
SITE_COUNT = re.compile(r"\bhc_count\(\s*OPK_([A-Z0-9_]+)\s*,")


def host_actions(header):
    text = open(header, encoding="utf-8").read()
    start = text.index("#define OPENPETE_KEYMAP_HOST_ACTIONS(X)")
    end = text.index("#define OPENPETE_KEYMAP_PAD_ACTIONS(X)")
    rows = {}
    for m in ROW.finditer(text[start:end]):
        rows[m.group(1)] = set(m.group(2).split("|"))
    opened = len(ROW_START.findall(text[start:end]))
    return rows, opened


def scan_sites(root):
    down, delta, count = {}, {}, {}
    for top in ("core", "games"):
        for dirpath, dirnames, filenames in os.walk(os.path.join(root, top)):
            dirnames[:] = [d for d in dirnames if d not in ("tests", "third_party", "generated")]
            for fn in filenames:
                if not fn.endswith((".c", ".cpp", ".h")):
                    continue
                path = os.path.join(dirpath, fn)
                rel = os.path.relpath(path, root)
                try:
                    text = open(path, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                for m in SITE_DOWN.finditer(text):
                    down.setdefault(m.group(1) or m.group(2), set()).add(rel)
                for m in SITE_COUNT.finditer(text):
                    count.setdefault(m.group(1), set()).add(rel)
                for m in SITE_DELTA.finditer(text):
                    delta.setdefault(m.group(1), set()).add(rel)
    return down, delta, count


def check(name, flag, flagged, sites, problems):
    for sym, files in sorted(sites.items()):
        if sym not in flagged:
            problems.append(
                f"{name}: OPK_{sym} is read in {', '.join(sorted(files))} "
                f"but the registry row lacks {flag}")
    for sym in sorted(flagged):
        if sym not in sites:
            problems.append(
                f"{name}: OPK_{sym} carries {flag} but no {name} site reads it")


def main():
    root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    header = os.path.join(root, "core", "include", "openpete_keymap.h")
    if not os.path.isfile(header):
        print(f"keymap_flags: missing {header}")
        return 64
    rows, opened = host_actions(header)
    if opened != len(rows):
        print(f"keymap_flags: {opened} host rows open in the X-macro, {len(rows)} parse "
              "with the row and op columns")
        return 67
    level = {s for s, f in rows.items() if "KM_LEVEL" in f}
    delta_f = {s for s, f in rows.items() if "KM_DELTA" in f}
    down, delta, count = scan_sites(root)

    for label, pop in (("KM_LEVEL rows", level), ("KM_DELTA rows", delta_f),
                       ("openpete_key_down and hc_held sites", down),
                       ("hc_count sites", count),
                       ("openpete_key_delta sites", delta)):
        if not pop:
            print(f"keymap_flags: {label}: zero members enumerated; the scan "
                  "cannot have engaged")
            return 64

    problems = []
    check("openpete_key_down", "KM_LEVEL", level, down, problems)
    check("openpete_key_delta", "KM_DELTA", delta_f, delta, problems)
    for sym, files in sorted(count.items()):
        if sym in level:
            problems.append(f"hc_count: OPK_{sym} is counted in {', '.join(sorted(files))} "
                            "but carries KM_LEVEL, which rejects the wheel source")
    if problems:
        for p in problems:
            print("keymap_flags: " + p)
        return 67
    print(f"keymap_flags: ok ({len(level)} KM_LEVEL actions, "
          f"{len(delta_f)} KM_DELTA actions, {len(count)} hc_count actions, consumers match)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
