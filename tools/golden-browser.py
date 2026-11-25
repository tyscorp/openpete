#!/usr/bin/env python3
"""golden-browser.py -- a local web page over the presentation golden.

The golden's verdicts are digest rows in blessed.tsv manifests and its images
live in the reference store, so no directory of PNGs exists to open in an image
viewer. This page reads the manifests, the store, the archive and candidate
directories, and shows them as images. Three views:

  current    every row of a game tier's blessed.tsv at the working tree as a
             thumbnail grid: tile name, corpus row, VBL, leg, LOD; filtered by
             substring (q) and by leg.
  candidate  a candidate directory against its tier's manifest: one row per
             divergent tile with the differing pixel count, the maximum
             channel delta and the bounding box from diff_images in
             tests/harness/presentation-corpus-diff.py, sorted by pixel count
             descending; filtered by a pixel floor, by leg and by corpus row;
             each row carries a reference | candidate | heat strip and a
             checkbox that adds its tile name to the promote list. With og
             naming a directory of PsyCross captures
             (<row>[__lodext]__canon__<vbl>-og.png), the panel adds the -og
             tile of the row's VBL, or of the VBL before it (the tick an
             intermediate tile sits after), and scores the divergent pixels
             of reference and candidate against it, raw and with og
             translated by the fit of tests/harness/og_align.py (og's
             integer GTE vertices sit about half a PSX pixel up-left of
             native's), with the side the aligned score favours at every
             near-best shift.
  history    two revisions (a, default HEAD~1; b, default HEAD) resolved with
             golden-at's tiers (store, archive, materialize price): the rows
             whose digest differs with the same panel, and the rows one side
             lacks as added or removed. A row neither the store nor the archive
             holds shows its materialize price and the golden-at command; the
             page runs nothing. Beside each revision field a list offers
             every tag (newest first, with its date and the tagged commit's
             subject) and the latest 200 commits that touched the compared
             manifests; choosing one fills the field and submits. A game
             with no reference manifest at a revision (before the manifest
             cutover) is read as golden-at reads it: the PNGs tracked in
             each tier directory at that revision are that side's rows,
             digested from the tree and marked unverified.

Each view shows one row per corpus sample: the variants of a row's VBL
(original and extended LOD, the canonical tile and the intermediate tile
captured after it) share it, buttons in it switch the variant shown (keys: l
switches every row's LOD, i every row's leg), and it names the variants whose
blessed digests are identical. The candidate row names each variant's status
(diverge, match, absent) and shows the first divergent variant; the history row
names each listed variant's kind and the variants left unchanged. In both, a
full-width table row under the variant's row holds its image strip, the
figures sharing the width. A served page holds a placeholder for each strip,
fetched from /lazy/<token> when it scrolls near the viewport: the strip's
store reads, archive extraction, pixel diff and og score run then. A pixel
diff is cached by digest pair beside its heat image; the candidate view's
pixel floor and order and the history view's order read that cache, and the
candidate view computes a missing diff when it lists the row. --once lists
every tile in FILE.rows.tsv as before.

The page writes one output, the promote list: one tile name per line in
<promote-dir>/promote-<game>-<tier>.txt, which
tests/harness/presentation-corpus-promote.sh takes as its tile arguments. It
also fills caches in golden-ref-thumbs/, the sibling of the store in the
directory holding golden-ref.state: thumbnails keyed by verdict digest, and
heat images and pixel-diff results keyed by the digest pair. A store blob
whose pixels do not hash to its name is discarded on read, which is the
store's own contract (presentation-blessed.py store_get).

The server binds 127.0.0.1 and serves only files a rendered view registered.
Its only child processes are read-only git: the `git cat-file --batch` and
`git rev-parse` calls presentation-blessed.py makes to read a revision, and
`git for-each-ref`, `git log` and `git ls-tree` for the revision lists and the
tracked tiles of a revision before the cutover.

Usage:
  tools/golden-browser.py [--port N] [--promote-dir DIR]
  tools/golden-browser.py --once VIEW --out FILE [--game G] [--tier T]
        [--cand DIR] [--og DIR] [--a REV] [--b REV] [--q S] [--leg L] [--floor N]
        [--row S]
--once renders one view to a static HTML file with file:// images (thumbnails
and image strips generated eagerly, images marked loading=lazy) and writes FILE.rows.tsv, the listed rows, for a headless
check.
"""
import argparse
import collections
import html
import importlib.util
import io
import json
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer

try:
    from PIL import Image
except ModuleNotFoundError:
    print("[golden-browser] python3-PIL missing", file=sys.stderr)
    sys.exit(1)

# The page writes nothing into the checkout, bytecode caches included.
sys.dont_write_bytecode = True

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
HOST = "127.0.0.1"
THUMB_W = 192
# Pending row fragments a served page registered; the oldest are dropped
# past this count, and a page that old reloads to list them again.
LAZY_CAP = 50000


def _load(name, rel):
    """Import a hyphen-named script as a module."""
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, rel))
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


# The diff script loads presentation-blessed.py itself; its module instance is
# reused so the digest helper and the store are one implementation.
cdiff = _load("presentation_corpus_diff",
              "tests/harness/presentation-corpus-diff.py")
pb = cdiff.pb
# The og scorer needs scipy; without it the panel shows the -og tile unscored.
try:
    oga = _load("og_align", "tests/harness/og_align.py")
except ImportError:
    oga = None


# ---- tile metadata ----------------------------------------------------------

def tile_meta(tile):
    """Corpus row, leg, LOD and VBL of a tile basename of the form
    <row>[__lodext]__<leg>__<vbl>-vk.png."""
    stem = tile[:-len("-vk.png")] if tile.endswith("-vk.png") else tile
    parts = stem.split("__")
    row = parts[0]
    vbl = parts[-1] if len(parts) >= 3 else ""
    leg = parts[-2] if len(parts) >= 3 else ""
    lod = "extended" if "lodext" in parts[1:-2] else "original"
    return {"row": row, "leg": leg, "lod": lod, "vbl": vbl}


# The variants of one corpus sample: original or extended LOD, canonical or
# intermediate leg. Order is display order; the first present is the default.
VARIANTS = (("o-c", "orig canon"), ("o-i", "orig inter"),
            ("e-c", "ext canon"), ("e-i", "ext inter"))
VLABEL = dict(VARIANTS)


def variant_of(tile):
    """Variant id of a tile (VARIANTS), or None for a name outside the
    <row>[__lodext]__(canon|inter)__<vbl>-vk.png form."""
    m = tile_meta(tile)
    if m["leg"] not in ("canon", "inter") or not m["vbl"].isdigit():
        return None
    return ("e" if m["lod"] == "extended" else "o") + "-" + m["leg"][0]


def sample_stem(tile):
    """The tile name without its LOD, leg and VBL parts."""
    stem = tile[:-len("-vk.png")] if tile.endswith("-vk.png") else tile
    return "__".join(x for x in stem.split("__")[:-2] if x != "lodext")


def group_keys(tiles):
    """{tile: group key} over a tile set. A group is one corpus sample: the
    canonical tile of a VBL and the intermediate tile captured after it, at
    both LODs. presentation-corpus.sh captures the intermediate leg at each
    canonical VBL + interval/2 and at each pinned VBL (where canon and inter
    share the VBL), so per row and LOD an intermediate VBL pairs with the
    canonical tile of the same VBL, else with the greatest unpaired canonical
    VBL below it; an unpaired tile keys a group of its own VBL. The key is
    (sample stem, canonical VBL); a name outside the variant form keys a
    group of its own."""
    legs = {}
    out = {}
    for t in tiles:
        v = variant_of(t)
        if v is None:
            out[t] = (t, None)
            continue
        legs.setdefault((sample_stem(t), v[0]), {}).setdefault(
            v[2], []).append((int(tile_meta(t)["vbl"]), t))
    for (stem, _), byleg in legs.items():
        canon = sorted(byleg.get("c", []))
        for vbl, t in canon:
            out[t] = (stem, vbl)
        free = {vbl for vbl, _ in canon}
        inter = sorted(byleg.get("i", []))
        rest = []
        for vbl, t in inter:
            if vbl in free:
                free.discard(vbl)
                out[t] = (stem, vbl)
            else:
                rest.append((vbl, t))
        for vbl, t in rest:
            below = [c for c in free if c < vbl]
            base = max(below) if below else vbl
            free.discard(base)
            out[t] = (stem, base)
    return out


def group_rows(rows, keyf):
    """Rows grouped by keyf(row) in order of first appearance: a list of
    (key, {variant id: row}). A second row of one variant in a group (two
    unpaired tiles colliding) starts a group of its own."""
    groups, index = [], {}
    for r in rows:
        k = keyf(r)
        vid = variant_of(r["tile"]) or "-"
        g = index.get(k)
        if g is None or vid in g:
            g = {}
            index[k] = g
            groups.append((k, g))
        g[vid] = r
    return groups


def same_as(digests):
    """{variant id: earlier variant id} for each variant whose digest equals
    a variant before it in VARIANTS order."""
    first, out = {}, {}
    for vid, _ in VARIANTS:
        d = digests.get(vid)
        if not d:
            continue
        if d in first:
            out[vid] = first[d]
        else:
            first[d] = vid
    return out


def tiers():
    """Every (game, tier) pair that holds a manifest."""
    return [(g, t) for t, gs in sorted(pb.TIERS.items()) for g in gs]


def default_candidate(game, tier):
    """The tier's kept-candidate directory, as scripts/presentation-game.sh
    names it."""
    base = "build/presentation-candidate" if tier == "presentation" \
        else "build/dragon-candidate"
    return base if game == "spyro-1" else "%s-%s" % (base, game)


def thumbs_dir():
    """golden-ref-thumbs/, the sibling of the store."""
    return os.path.join(os.path.dirname(pb.store_dir()), "golden-ref-thumbs")


# ---- rendering context --------------------------------------------------------

class Ctx:
    """Maps images to URLs. Served: an opaque token per registered file, so
    the server serves nothing a view did not register. Static: file:// paths,
    in-memory images written beside the HTML."""

    def __init__(self, static_dir=None):
        self.static_dir = static_dir
        self.files = {}
        self.by_path = {}
        self.lazies = collections.OrderedDict()
        self.lazy_n = 0

    def lazy(self, fn):
        """A row's deferred fragment. Static, fn() runs now and its HTML is
        inlined. Served, fn is registered under a token and the page gets a
        placeholder the page script fetches from /lazy/<token> when it
        scrolls into view."""
        if self.static_dir is not None:
            return fn()
        tok = "%x" % self.lazy_n
        self.lazy_n += 1
        self.lazies[tok] = fn
        while len(self.lazies) > LAZY_CAP:
            self.lazies.popitem(last=False)
        return "<div class=lazy data-lazy='/lazy/%s'>loading</div>" % tok

    def file(self, path):
        """URL of a file on disk."""
        path = os.path.abspath(path)
        if self.static_dir is not None:
            return "file://" + urllib.parse.quote(path)
        tok = self.by_path.get(path)
        if tok is None:
            tok = "%x" % len(self.files)
            self.files[tok] = path
            self.by_path[path] = tok
        return "/f/" + tok

    def blob(self, data, name):
        """URL of in-memory image bytes (an archive read)."""
        d = self.static_dir or os.path.join(thumbs_dir(), "archive")
        os.makedirs(d, exist_ok=True)
        p = os.path.join(d, name)
        with open(p, "wb") as fh:
            fh.write(data)
        return self.file(p)

    def src(self, source, name):
        """URL of a reference source: a path or a BytesIO."""
        if isinstance(source, io.BytesIO):
            return self.blob(source.getvalue(), name)
        return self.file(source)

    def thumb(self, digest):
        """URL of a store blob's thumbnail. Served, it is built on request;
        static, it is built now, or None when the store lacks the blob."""
        if self.static_dir is None:
            return "/thumb/" + digest
        p = make_thumb(digest)
        return self.file(p) if p else None


def make_thumb(digest):
    """Path of the cached thumbnail of a store blob, built from the verified
    blob on a miss; None when the store lacks it."""
    out = os.path.join(thumbs_dir(), digest + ".png")
    if os.path.isfile(out):
        return out
    src = pb.store_get(digest)
    if not src:
        return None
    with Image.open(src) as im:
        im = im.convert("RGB")
        h = max(1, round(im.height * THUMB_W / im.width))
        im = im.resize((THUMB_W, h), Image.BILINEAR)
    os.makedirs(thumbs_dir(), exist_ok=True)
    tmp = out + ".tmp%d" % os.getpid()
    im.save(tmp, format="PNG")
    os.replace(tmp, out)
    return out


# ---- data: current set ----------------------------------------------------------

def load_manifest(game, tier):
    """The working tree's manifest of a tier, or (None, error)."""
    try:
        return pb.load_manifest(game, tier), None
    except (OSError, ValueError) as e:
        return None, str(e)


def current_rows(p):
    """Rows of the current view after its filters."""
    m, err = load_manifest(p["game"], p["tier"])
    if m is None:
        return [], err
    keys = group_keys([r["tile"] for r in m.rows])
    out = []
    for r in m.rows:
        meta = tile_meta(r["tile"])
        if p["q"] and p["q"] not in r["tile"]:
            continue
        if p["leg"] and meta["leg"] != p["leg"]:
            continue
        out.append(dict(r, gkey=keys[r["tile"]], **meta))
    return out, None


# ---- data: candidate review -------------------------------------------------------

_DIGEST_CACHE = {}


def candidate_digests(paths):
    """Verdict digests of candidate tiles, cached by path, mtime and size."""
    def key(pth):
        st = os.stat(pth)
        return (pth, st.st_mtime_ns, st.st_size)
    todo = [pth for pth in paths if key(pth) not in _DIGEST_CACHE]
    if todo:
        got = pb.digest_files(todo)
        for pth in todo:
            _DIGEST_CACHE[key(pth)] = got[pth]
    return {pth: _DIGEST_CACHE[key(pth)] for pth in paths}


def heat_path(a, b):
    """Cached heat image path of a digest pair."""
    return os.path.join(thumbs_dir(), "heat", "%s_%s.png" % (a[:16], b[:16]))


def cached_diff(a_dig, b_dig):
    """The cached pixel_diff result of a digest pair, or None. A digest names
    decoded pixels, so a pair's result never goes stale; a result naming a
    heat image counts only while that image exists."""
    try:
        with open(heat_path(a_dig, b_dig)[:-4] + ".json") as f:
            res = json.load(f)
    except (OSError, ValueError):
        return None
    if res.get("heat") and not os.path.isfile(res["heat"]):
        return None
    if res.get("bbox") is not None:
        res["bbox"] = tuple(res["bbox"])
    return res


def pixel_diff(ref_src, cand_src, a_dig, b_dig):
    """diff_images over two sources, the heat written into the cache and the
    result into cached_diff's JSON. Returns dict(pixels, maxch, bbox, heat)
    or dict(size)."""
    got = cached_diff(a_dig, b_dig)
    if got is not None:
        return got
    heat = heat_path(a_dig, b_dig)
    if isinstance(ref_src, io.BytesIO):
        ref_src.seek(0)
    if isinstance(cand_src, io.BytesIO):
        cand_src.seek(0)
    res = cdiff.diff_images(ref_src, cand_src, heat)
    if res[0] == "size":
        out = {"size": "%dx%d->%dx%d" % (res[1] + res[2])}
    else:
        out = {"pixels": res[1], "maxch": res[2], "bbox": res[3],
               "heat": heat if res[1] else None}
    path = heat[:-4] + ".json"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp%d" % os.getpid()
    with open(tmp, "w") as f:
        json.dump(out, f)
    os.replace(tmp, path)
    return out


def og_score(ref_src, cand_path, og_path, a_dig, b_dig):
    """Raw and aligned |reference-og| and |candidate-og| over the divergent
    pixels (tests/harness/og_align.py), cached as JSON by digest pair and
    the og file's mtime and size. Returns the score dict, or None when the
    sizes do not admit a comparison."""
    if oga is None:
        return None
    st = os.stat(og_path)
    path = os.path.join(thumbs_dir(), "og", "%s_%s_%d_%d.json" % (
        a_dig[:16], b_dig[:16], st.st_mtime_ns, st.st_size))
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        pass
    if isinstance(ref_src, io.BytesIO):
        ref_src.seek(0)
    try:
        res = oga.score([oga.load_rgb(ref_src), oga.load_rgb(cand_path)],
                        oga.load_rgb(og_path))
    except ValueError:
        return None
    if res is None:
        return None
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(res, f)
    return res


def og_text(o):
    """One line of an og_score result: the fit and each side's raw and
    aligned mean |tile-og| over the divergent pixels, and the verdict."""
    if not o:
        return ""
    f = o.get("fit")
    s = "og ref %.1f cand %.1f raw" % (o["A"]["raw"], o["B"]["raw"])
    if f:
        s += "; fit (%+.2f, %+.2f) px: ref %.1f cand %.1f aligned, %s" % (
            f["dx"], f["dy"], o["A"]["aligned"], o["B"]["aligned"],
            {"A": "ref closer", "B": "cand closer"}.get(o.get("verdict"),
                                                         "undecided"))
    return s


def og_tile(ogdir, tile):
    """The -og capture beside a tile: the canon -og tile of its VBL, else of
    the VBL before it. Returns (path, vbl) or (None, None)."""
    meta = tile_meta(tile)
    if not meta["vbl"].isdigit():
        return None, None
    stem = tile[:-len("-vk.png")].rsplit("__", 2)[0]
    for v in (int(meta["vbl"]), int(meta["vbl"]) - 1):
        path = os.path.join(ogdir, "%s__canon__%d-og.png" % (stem, v))
        if os.path.isfile(path):
            return path, v
    return None, None


def candidate_rows(p):
    """Divergent rows of a candidate directory against its manifest, each
    with its pixel diff (cached_diff, else computed here) and the paths
    candidate_panel reads. Returns (rows, summary, error)."""
    cand = p["cand"] or default_candidate(p["game"], p["tier"])
    cand = os.path.join(ROOT, cand) if not os.path.isabs(cand) else cand
    p["cand"] = cand
    ogdir = p["og"] and (p["og"] if os.path.isabs(p["og"])
                         else os.path.join(ROOT, p["og"]))
    if not os.path.isdir(cand):
        return [], None, "no candidate directory %s" % cand
    m, err = load_manifest(p["game"], p["tier"])
    if m is None:
        return [], None, err
    manifest = {r["tile"]: r for r in m.rows}
    tiles = set(pb.list_tiles(cand))
    common = sorted(tiles & set(manifest))
    digs = candidate_digests([os.path.join(cand, t) for t in common])
    tdir = pb.tier_dir(p["game"], p["tier"])
    keys = group_keys(list(manifest))
    variants = {}
    for t, row in manifest.items():
        got = digs.get(os.path.join(cand, t)) if t in tiles else None
        status = "absent" if t not in tiles else (
            "match" if got is not None and got[0] == row["digest"]
            else "diverge")
        variants.setdefault(keys[t], {})[variant_of(t) or "-"] = dict(
            row, status=status, **tile_meta(t))
    arch = None
    rows = []
    for t in common:
        path = os.path.join(cand, t)
        got, row = digs[path], manifest[t]
        if got is not None and got[0] == row["digest"]:
            continue
        ent = dict(row, gkey=keys[t], **tile_meta(t))
        ent["cand_digest"] = got[0] if got else None
        ent["cand_path"], ent["tdir"] = path, tdir
        if ogdir:
            ent["og_path"], ent["og_vbl"] = og_tile(ogdir, t)
        if got is None:
            ent["note"] = "undecodable candidate tile"
            rows.append(ent)
            continue
        cached = cached_diff(row["digest"], got[0])
        if cached is not None:
            ent.update(cached)
            rows.append(ent)
            continue
        if arch is None:
            arch = pb.Archive()
        src, _ = pb.resolve_reference(row, tdir, arch)
        if src is None:
            ent["note"] = unresolved_note(row, got[0], p["game"], t)
        else:
            ent.update(pixel_diff(src, path, row["digest"], got[0]))
        rows.append(ent)
    if arch is not None:
        arch.close()
    summary = {"compared": len(common), "divergent": len(rows),
               "missing": len(set(manifest) - tiles),
               "extra": len(tiles - set(manifest)), "variants": variants}
    rows.sort(key=lambda e: (0 if "pixels" in e else 1,
                             -e.get("pixels", 0), e["tile"]))
    return filter_divergent(rows, p), summary, None


def unresolved_note(row, cand_digest, game, tile):
    """The note of a divergent row whose reference neither the store nor the
    archive holds."""
    return "hex mismatch %s->%s; reference in neither store nor archive " \
        "(golden-at %s %s %s)" % (row["digest"][:12], cand_digest[:12],
                                  row["revision"][:12], game, tile)


def panel(figs):
    """The image strip of a row: its figures share the row width evenly, at
    least three columns so a lone image keeps a third of the width."""
    figs = [f for f in figs if f]
    if not figs:
        return ""
    return "<div class=panel style='--n:%d'>%s</div>" % (max(3, len(figs)),
                                                          "".join(figs))


def candidate_panel(e, game, ctx):
    """The lazy fragment of a divergent candidate row: reference, candidate,
    heat and -og figures, the og score in the -og caption. Resolves the
    reference and reads the diff from its cache."""
    arch = pb.Archive()
    try:
        src, where = pb.resolve_reference(e, e["tdir"], arch)
    finally:
        arch.close()
    if src is None:
        return "<p class=mut>%s</p>" % E(unresolved_note(
            e, e["cand_digest"], game, e["tile"]))
    d = pixel_diff(src, e["cand_path"], e["digest"], e["cand_digest"])
    ref = ctx.src(src, e["digest"] + ".png")
    og = ""
    if e.get("og_path"):
        o = og_score(src, e["cand_path"], e["og_path"], e["digest"],
                     e["cand_digest"]) if d.get("pixels") else None
        og = figure(ctx.file(e["og_path"]), "-og (VBL %s) %s" % (
            e["og_vbl"], og_text(o)))
    return panel([figure(ref, "reference (%s)" % where),
                  figure(ctx.file(e["cand_path"]), "candidate"),
                  figure(ctx.file(d["heat"]) if d.get("heat") else None,
                         "heat x8"), og])


def filter_divergent(rows, p):
    """Apply the pixel floor (to rows that carry a count), leg and row
    filters."""
    out = []
    for e in rows:
        if p["floor"] and "pixels" in e and e["pixels"] < p["floor"]:
            continue
        if p["leg"] and e["leg"] != p["leg"]:
            continue
        if p["row"] and p["row"] not in e["row"]:
            continue
        out.append(e)
    return out


# ---- data: history compare --------------------------------------------------------

class RevReader:
    """Reads manifests at revisions of this repository, else of the
    archive (through its commit map)."""

    def __init__(self):
        self.local = pb.CatFile(ROOT)
        self.arch = pb.Archive()

        self.tracked_cf = {}

    def resolve(self, rev):
        """(reader, sha) of a revision, or (None, None)."""
        sha = pb.resolve(ROOT, rev)
        if sha:
            return self.local, sha
        sha = self.arch.rev(rev) if self.arch.path else None
        return (self.arch.cf, sha) if sha else (None, None)

    def cf_of(self, row):
        """The reader holding a tracked-tile row's blob."""
        return self.tracked_cf.get(row["revision"], self.local)

    def close(self):
        """End both readers."""
        self.local.close()
        self.arch.close()


def tracked_tiles(repo, sha, tdir, cf):
    """{tile: row} of the PNGs tracked directly in a tier directory at a
    revision, the reference before the manifest cutover. A row carries the
    decoded digest of the tracked blob (`undecodable:<oid>` when it does not
    decode), `revision` = sha and `spec`, the object to read its bytes
    from."""
    out = git_out(repo, "ls-tree", sha, "--", tdir + "/")
    ents = []
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        f = meta.split()
        if len(f) == 3 and f[1] == "blob" and path.endswith(".png"):
            ents.append((os.path.basename(path), f[2]))
    blob_digests(cf, [oid for _, oid in ents])
    return {t: {"tile": t, "revision": sha, "spec": oid,
                "digest": _BLOB_DIGEST.get(oid) or "undecodable:" + oid}
            for t, oid in ents}


_BLOB_DIGEST = {}


def blob_digests(cf, oids):
    """Fill _BLOB_DIGEST for blob ids not yet in it, digesting in chunks of
    256 blobs so the bytes held at once stay bounded. A blob is immutable
    under its id, so the cache never goes stale."""
    todo = [o for o in dict.fromkeys(oids) if o not in _BLOB_DIGEST]
    for i in range(0, len(todo), 256):
        chunk = todo[i:i + 256]
        got = pb.digest_blobs([cf.read(o) or b"" for o in chunk])
        for o, d in zip(chunk, got):
            _BLOB_DIGEST[o] = d[0] if d else None


def git_out(repo, *args):
    """stdout of a read-only git command in repo; empty on failure."""
    return pb.git(*args, cwd=repo, check=False) or ""


def side_manifests(rr, cf, sha, game):
    """Per reference manifest path of a game, its {tile: row} at one side,
    or None when absent. A game with no reference manifest at the revision
    predates the cutover: the tracked PNGs of each tier directory fill the
    first manifest naming that directory. Returns ({path: map}, pre)."""
    paths = pb.reference_manifests(game)
    got = {mpath: manifest_at(cf, sha, mpath) for mpath, _, _ in paths}
    if any(m is not None for m in got.values()):
        return got, False
    repo = ROOT if cf is rr.local else rr.arch.path
    seen = set()
    for mpath, _, tdir in paths:
        if tdir in seen:
            continue
        seen.add(tdir)
        got[mpath] = tracked_tiles(repo, sha, tdir, cf) or None
    return got, True


def manifest_at(cf, sha, mpath):
    """{tile: row} of a manifest at a revision, or None when absent."""
    data = cf.read("%s:%s" % (sha, mpath))
    if data is None:
        return None
    try:
        m = pb.parse_manifest(data.decode(errors="replace"), mpath)
    except ValueError:
        return None
    return {r["tile"]: r for r in m.rows}


def locate(row, tdir, rr, rev, game, verify):
    """golden-at's tiers for one row: ("store", path), ("archive", BytesIO)
    or ("materialize", text). An unverified lookup checks store presence
    only (the thumbnail verifies on build). A row read from a tracked tile
    before the cutover returns ("tracked", BytesIO) of that blob, which
    nothing verifies."""
    if row.get("spec"):
        return "tracked", io.BytesIO(rr.cf_of(row).read(row["spec"]) or b"")
    if verify:
        p = pb.store_get(row["digest"])
    else:
        p = pb.blob_path(row["digest"])
        p = p if os.path.isfile(p) else None
    if p:
        return "store", p
    spec = pb.archive_tile(rr.arch, row, tdir)
    if spec:
        return "archive", io.BytesIO(rr.arch.cf.read(spec))
    return "materialize", "%s; scripts/presentation-blessed.py golden-at %s " \
        "%s %s --materialize" % (pb.price(1), rev, game, row["tile"])


def history_rows(p):
    """Changed, added and removed rows between revisions a and b, ordered by
    kind, then by the pixel count of a changed row whose diff is cached
    (pixel_diff runs in the row's lazy fragment), then by tile. Returns
    (rows, notes, error)."""
    rr = RevReader()
    try:
        (cfa, sa), (cfb, sb) = rr.resolve(p["a"]), rr.resolve(p["b"])
        if not sa or not sb:
            return [], [], "revision %s does not resolve" % (
                p["a"] if not sa else p["b"])
        p["a_sha"], p["b_sha"] = sa, sb
        rr.tracked_cf.update({sa: cfa, sb: cfb})
        rows, notes, unchanged = [], [], {}
        games = [p["game"]] if p["game"] else list(pb.GAMES)
        for game in games:
            sides = {}
            for side, cf, sha, rev in (("a", cfa, sa, p["a"]),
                                       ("b", cfb, sb, p["b"])):
                sides[side], pre = side_manifests(rr, cf, sha, game)
                if pre:
                    n = sum(len(m or {}) for m in sides[side].values())
                    notes.append("%s (%s) predates the manifest cutover for "
                                 "%s: %d tracked PNG(s) read as unverified "
                                 "references" % (side, rev, game, n))
            for mpath, tier, tdir in pb.reference_manifests(game):
                ma, mb = sides["a"].get(mpath), sides["b"].get(mpath)
                if ma is None and mb is None:
                    continue
                for side, m, rev in (("a", ma, p["a"]), ("b", mb, p["b"])):
                    if m is None:
                        notes.append("%s absent at %s (%s)" % (mpath, side,
                                                              rev))
                ma, mb = ma or {}, mb or {}
                keys = group_keys(set(ma) | set(mb))
                for t in sorted(set(ma) | set(mb)):
                    if p["q"] and p["q"] not in t:
                        continue
                    ra, rb = ma.get(t), mb.get(t)
                    if ra and rb and ra["digest"] == rb["digest"]:
                        unchanged.setdefault((mpath,) + keys[t], set()).add(
                            variant_of(t) or "-")
                        continue
                    kind = "changed" if ra and rb else (
                        "added" if rb else "removed")
                    ent = dict(tile_meta(t), tile=t, kind=kind, game=game,
                               tier=tier, manifest=mpath,
                               gkey=(mpath,) + keys[t], ra=ra, rb=rb,
                               tdir=tdir,
                               a_digest=ra["digest"] if ra else None,
                               b_digest=rb["digest"] if rb else None)
                    if kind == "changed":
                        ent.update(cached_diff(ra["digest"], rb["digest"])
                                   or {})
                    rows.append(ent)
        for e in rows:
            e["unchanged"] = unchanged.get(e["gkey"], set())
        rows.sort(key=lambda e: ({"changed": 0, "added": 1, "removed": 2}
                                 [e["kind"]], -e.get("pixels", 0),
                                 e["tile"]))
        return rows, notes, None
    finally:
        rr.close()


def history_panel(e, p, ctx):
    """The lazy fragment of a history row: the a and b images where the
    store, the archive or a tracked tile holds them (a changed row's store
    blobs verified), the materialize command of a side neither holds, and
    for a changed row with both sides its diff line and heat figure."""
    rr = RevReader()
    try:
        (cfa, sa), (cfb, sb) = rr.resolve(p["a"]), rr.resolve(p["b"])
        rr.tracked_cf.update({sa: cfa, sb: cfb})
        verify = e["kind"] == "changed"
        srcs, figs, notes = {}, [], []
        for side, row, rev in (("a", e["ra"], p["a"]), ("b", e["rb"], p["b"])):
            if row is None:
                continue
            where, val = locate(row, e["tdir"], rr, rev, e["game"], verify)
            if where == "materialize":
                notes.append("<div>%s: materialize %s</div>" % (side, E(val)))
                continue
            srcs[side] = val
            figs.append(figure(ctx.src(val, row["digest"] + ".png"),
                               "%s %s (%s)" % (side, rev, where)))
    finally:
        rr.close()
    d = {}
    if verify and len(srcs) == 2:
        d = pixel_diff(srcs["a"], srcs["b"], e["a_digest"], e["b_digest"])
        if d.get("heat"):
            figs.append(figure(ctx.file(d["heat"]), "heat x8"))
    line = "<div class=mut>%s</div>" % E(diff_text(d)) if d else ""
    return "".join(notes) + line + panel(figs)


# ---- promote list -------------------------------------------------------------------

def promote_list_path(pdir, game, tier):
    """The promote list of a game tier."""
    return os.path.join(pdir, "promote-%s-%s.txt" % (game, tier))


def read_promote_list(pdir, game, tier):
    """Tile names in a promote list."""
    try:
        with open(promote_list_path(pdir, game, tier)) as fh:
            return [ln.strip() for ln in fh if ln.strip()]
    except OSError:
        return []


def write_promote_list(pdir, game, tier, tiles):
    """Rewrite a promote list, one tile per line, sorted."""
    os.makedirs(pdir, exist_ok=True)
    p = promote_list_path(pdir, game, tier)
    tmp = p + ".tmp"
    with open(tmp, "w") as fh:
        fh.write("".join(t + "\n" for t in sorted(set(tiles))))
    os.replace(tmp, p)


def promote_command(p, pdir):
    """The promote invocation that consumes the list."""
    env = "PRESENTATION_TIER=dragons " if p["tier"] == "dragons" else ""
    cand = p["cand"]
    if cand.startswith(ROOT + os.sep):
        cand = os.path.relpath(cand, ROOT)
    return "%stests/harness/presentation-corpus-promote.sh %s " \
        "$(cat %s) --game %s" % (env, cand,
                                 promote_list_path(pdir, p["game"], p["tier"]),
                                 p["game"])


# ---- HTML -----------------------------------------------------------------------------

E = html.escape

CSS = """
:root{--bg:#15171a;--fg:#e6e6e6;--mut:#9aa0a6;--line:#2c3035;--acc:#7cc4ff}
body{background:var(--bg);color:var(--fg);font:14px system-ui,sans-serif;
margin:0 16px 32px}
a{color:var(--acc)}nav{padding:10px 0;border-bottom:1px solid var(--line)}
nav a{margin-right:16px}form{margin:10px 0}input,select{background:#1f2226;
color:var(--fg);border:1px solid var(--line);padding:3px 6px}
.grid{display:flex;flex-wrap:wrap;gap:10px}.card{width:196px;font-size:11px;
word-break:break-all}.card img{width:192px;display:block;background:#000}
.miss{width:192px;height:144px;display:flex;align-items:center;
justify-content:center;background:#222;color:var(--mut)}
table{border-collapse:collapse;width:100%}td,th{border-bottom:1px solid
var(--line);padding:4px 6px;text-align:left;font-size:12px;
vertical-align:top}tr.pr>td{padding:2px 0 14px}.panel{display:grid;gap:8px;
grid-template-columns:repeat(var(--n,3),minmax(0,1fr))}
@media(max-width:700px){.panel{grid-template-columns:1fr}}
.panel figure{margin:0}.panel img{width:100%;height:auto;display:block;
image-rendering:pixelated;background:#000}
.panel figcaption{color:var(--mut);font-size:11px}.mut{color:var(--mut)}
.lazy{min-height:24vw;color:var(--mut);font-size:11px}
code{word-break:break-all}
.vt{margin:4px 0;font-size:11px}.vt button{font-size:11px;margin:0 3px 3px 0;
padding:1px 5px;background:#1f2226;color:var(--mut);border:1px solid
var(--line);cursor:pointer}.vt button[aria-pressed=true]{color:var(--fg);
border-color:var(--acc)}.vt button.st-diverge,.vt button.st-changed{
color:#ffb38a}.vt button.st-absent{text-decoration:line-through}
"""

JS = """
function promote(cb){fetch('/promote',{method:'POST',headers:{'Content-Type':
'application/json'},body:JSON.stringify({game:cb.dataset.game,tier:
cb.dataset.tier,cand:cb.dataset.cand,tile:cb.dataset.tile,on:cb.checked})})
.then(r=>r.json()).then(j=>{document.getElementById('plist').textContent=
j.count+' tile(s) in the promote list'}).catch(e=>{cb.checked=!cb.checked;
alert('promote list not written: '+e)})}
function sv(g,v){document.querySelectorAll('[data-gv^="'+g+'|"]').forEach(
e=>{e.hidden=e.dataset.gv!==g+'|'+v});document.querySelectorAll(
'[data-gb="'+g+'"]').forEach(b=>b.setAttribute('aria-pressed',b.dataset.v===v))}
function flip(i,a,b){const cur={};document.querySelectorAll(
'[data-gb][aria-pressed=true]').forEach(x=>{cur[x.dataset.gb]=x.dataset.v});
for(const g in cur){const v=cur[g],t=v.slice(0,i)+(v[i]===a?b:a)+v.slice(i+1);
if(document.querySelector('[data-gb="'+g+'"][data-v="'+t+'"]'))sv(g,t)}}
document.addEventListener('keydown',e=>{if(e.ctrlKey||e.metaKey||e.altKey||
/^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName))return;if(e.key==='l')
flip(0,'o','e');else if(e.key==='i')flip(2,'c','i')})
function pick(s,f){if(s.value){s.form.elements[f].value=s.value;
s.form.submit()}}
const lazyIO=new IntersectionObserver(es=>{for(const x of es){
if(!x.isIntersecting)continue;const d=x.target;lazyIO.unobserve(d);
fetch(d.dataset.lazy).then(r=>{if(!r.ok)throw new Error(r.status);
return r.text()}).then(t=>{d.innerHTML=t;d.classList.remove('lazy')})
.catch(e=>{d.textContent='load failed ('+e.message+'); reload the page'})}},
{rootMargin:'600px 0px'});
document.addEventListener('DOMContentLoaded',()=>document.querySelectorAll(
'[data-lazy]').forEach(d=>lazyIO.observe(d)))
"""


def page(title, body, static):
    """A complete HTML document."""
    nav = "" if static else (
        '<nav><a href="/current">current set</a><a href="/candidate">'
        'candidate review</a><a href="/history">history compare</a></nav>')
    return ("<!doctype html><html><head><meta charset=utf-8>"
            "<meta name=viewport content='width=device-width,initial-scale=1'>"
            "<title>Golden browser</title><style>%s</style><script>%s</script>"
            "</head><body>%s<h2>%s</h2>%s</body></html>"
            % (CSS, JS, nav, E(title), body))


def rev_options(paths):
    """(value, label) revision choices: every tag, newest first, labelled
    with its date and the tagged commit's subject, then the latest 200
    commits of HEAD that touched any of paths, labelled with their tag or
    short sha, commit date and subject."""
    fmt = "%(refname:short)%09%(creatordate:short)%09" \
        "%(if)%(*objectname)%(then)%(*objectname)%(else)%(objectname)%(end)" \
        "%09%(if)%(*subject)%(then)%(*subject)%(else)%(subject)%(end)"
    out, tag_of = [], {}
    for line in git_out(ROOT, "for-each-ref", "--sort=-creatordate",
                        "--format=" + fmt, "refs/tags").splitlines():
        f = line.split("\t", 3)
        if len(f) == 4:
            tag_of.setdefault(f[2], f[0])
            out.append((f[0], "%s  %s  %s" % (f[0], f[1], f[3])))
    log = git_out(ROOT, "log", "-n", "200", "--format=%H%x09%cs%x09%s",
                  "HEAD", "--", *paths)
    for line in log.splitlines():
        f = line.split("\t", 2)
        if len(f) == 3:
            rev = tag_of.get(f[0], f[0][:12])
            out.append((rev, "%s  %s  %s" % (rev, f[1], f[2])))
    return out


def rev_select(field, value, opts):
    """A <select> of revision choices that writes the chosen revision into
    the text field named field and submits the form; it carries no name, so
    only the field is submitted."""
    body = "".join("<option value='%s'%s>%s</option>" % (
        E(v), " selected" if v == value else "", E(lab)) for v, lab in opts)
    return "<select onchange='pick(this,\"%s\")'><option value=''>" \
        "tag or commit</option>%s</select>" % (field, body)


def select(name, value, options):
    """A <select> element."""
    opts = "".join("<option value='%s'%s>%s</option>" % (
        E(v), " selected" if v == value else "", E(v or "all")) for v in options)
    return "<select name=%s>%s</select>" % (name, opts)


def game_tier_select(p):
    """The game-tier selector."""
    opts = ["%s/%s" % gt for gt in tiers()]
    return select("gt", "%s/%s" % (p["game"], p["tier"]), opts)


def meta_cells(e):
    """Row, VBL, leg and LOD cells."""
    return "<td>%s</td><td>%s</td><td>%s</td><td>%s</td>" % (
        E(e["row"]), E(e["vbl"]), E(e["leg"]), E(e["lod"]))


def figure(src, cap):
    """One panel figure."""
    if not src:
        return ""
    return "<figure><img loading=lazy src='%s'><figcaption>%s</figcaption>" \
        "</figure>" % (E(src), E(cap))


def diff_text(e):
    """Pixel count, max channel delta and bounding box, or the size change
    or the note."""
    if "pixels" in e:
        s = "pixels=%d maxch=%d bbox=%s" % (e["pixels"], e["maxch"],
                                            e["bbox"])
        return s + ("; " + og_text(e["og"]) if e.get("og") else "")
    if "size" in e:
        return "size=%s" % e["size"]
    return e.get("note", "")


KEY_HINT = "<p class=mut>Variants of one sample (original or extended LOD, " \
    "canonical or intermediate leg) share a row; its buttons switch the " \
    "variant shown. Keys: l switches every row's LOD, i every row's leg. " \
    "= marks a variant pixel-identical in blessed.tsv to the one named.</p>"


def variant_bar(gid, present, sel, digests, status=None, extra=()):
    """The variant toggles of a group: one button per present variant, the
    selected one pressed, and a note naming the variants whose blessed
    digests are identical and, with status ({variant id: word}), each
    variant's status. Empty for a group of one variant with nothing to
    note."""
    same = same_as(digests)
    notes = ["%s = %s" % (VLABEL[v], VLABEL[same[v]]) for v, _ in VARIANTS
             if v in same and v in present]
    if status:
        words = {}
        for v in present:
            words.setdefault(status[v], []).append(VLABEL.get(v, v))
        notes += ["%s: %s" % (w, ", ".join(vs)) for w, vs in words.items()]
    notes += list(extra)
    if len(present) < 2 and not notes:
        return ""
    btns = []
    if len(present) > 1:
        for v in present:
            title = "pixel-identical to %s in blessed.tsv" % VLABEL[same[v]] \
                if v in same else ""
            cls = " class=st-%s" % status[v] if status else ""
            btns.append("<button type=button%s data-gb='%s' data-v='%s' "
                        "aria-pressed=%s title='%s' onclick=\"sv('%s','%s')\">"
                        "%s%s</button>" % (
                            cls, gid, v, "true" if v == sel else "false",
                            E(title), gid, v, E(VLABEL[v]),
                            " =" if v in same else ""))
    return "<div class=vt>%s%s</div>" % (
        "".join(btns), "<div class=mut>%s</div>" % E("; ".join(notes))
        if notes else "")


def gv_attr(gid, vid, sel):
    """The attributes of one variant's element in a group."""
    return " data-gv='%s|%s'%s" % (gid, vid, "" if vid == sel else " hidden")


def ordered(vids):
    """Variant ids in VARIANTS order, any other id last."""
    rank = {v: n for n, (v, _) in enumerate(VARIANTS)}
    return sorted(vids, key=lambda v: rank.get(v, len(rank)))


def render_current(p, ctx, static):
    """The current-set view: one card per sample group."""
    rows, err = current_rows(p)
    form = "" if static else (
        "<form action=/current>%s <input name=q value='%s' placeholder="
        "substring> %s <button>filter</button></form>" % (
            game_tier_select(p), E(p["q"]), select("leg", p["leg"],
                                                   ["", "canon", "inter"])))
    if err:
        return page("current set", form + "<p>%s</p>" % E(err), static), []
    groups = group_rows(rows, lambda r: r["gkey"])
    cards = []
    for n, (_, g) in enumerate(groups):
        gid = "g%d" % n
        present = ordered(g)
        sel = present[0]
        inner = []
        for v in present:
            r = g[v]
            src = ctx.thumb(r["digest"])
            img = "<img loading=lazy src='%s'>" % E(src) if src else \
                "<div class=miss>not in store</div>"
            inner.append("<div%s>%s<b>%s</b><br>row %s &middot; VBL %s "
                         "&middot; %s &middot; %s</div>" % (
                             gv_attr(gid, v, sel), img, E(r["tile"]),
                             E(r["row"]), E(r["vbl"]), E(r["leg"]),
                             E(r["lod"])))
        cards.append("<div class=card>%s%s</div>" % (
            "".join(inner), variant_bar(
                gid, present, sel, {v: g[v]["digest"] for v in present})))
    head = "<p class=mut>%s/%s: %d row(s) shown in %d sample(s)</p>" % (
        E(p["game"]), E(p["tier"]), len(rows), len(groups)) + KEY_HINT
    body = form + head + "<div class=grid>%s</div>" % "".join(cards)
    return page("current set", body, static), [
        ("row", r["tile"], r["digest"]) for r in rows]


def panel_tr(gid, v, sel, ncols, inner):
    """The full-width table row under a variant's row holding its image
    strip, shown and hidden with it."""
    return "<tr class=pr%s><td colspan=%d>%s</td></tr>" % (
        gv_attr(gid, v, sel), ncols, inner)


def candidate_variant_tr(gid, v, sel, bar, ent, ctx):
    """The row of a group variant the listing does not hold: its blessed
    thumbnail and status."""
    src = ctx.thumb(ent["digest"])
    img = "<br><img loading=lazy width=192 src='%s'>" % E(src) if src else ""
    word = {"match": "candidate matches the reference",
            "absent": "absent from the candidate",
            "diverge": "diverges; outside this listing's filters"}[
                ent["status"]]
    return "<tr%s><td></td><td>%s%s%s</td>%s<td>%s</td></tr>" % (
        gv_attr(gid, v, sel), bar, E(ent["tile"]), img, meta_cells(ent),
        E(word))


def render_candidate(p, ctx, static, pdir):
    """The candidate-review view: one table row per sample group, showing
    one variant; the group's other variants carry their status."""
    rows, summ, err = candidate_rows(p)
    form = "" if static else (
        "<form action=/candidate>%s <input name=cand size=48 value='%s' "
        "placeholder='candidate dir'> <input name=og size=32 value='%s' "
        "placeholder='-og dir (optional)'> floor <input name=floor size=6 "
        "value='%s'> %s <input name=row value='%s' placeholder='corpus row'> "
        "<button>show</button></form>" % (
            game_tier_select(p), E(p["cand"] or ""), E(p["og"]),
            p["floor"] or "",
            select("leg", p["leg"], ["", "canon", "inter"]), E(p["row"])))
    if err:
        return page("candidate review", form + "<p>%s</p>" % E(err),
                    static), []
    chosen = set(read_promote_list(pdir, p["game"], p["tier"]))
    groups = group_rows(rows, lambda e: e["gkey"])
    head = "<p class=mut>%s against %s/%s: %d compared, %d divergent, %d " \
        "missing, %d extra; %d listed in %d sample(s)</p>" % (
            E(p["cand"]), E(p["game"]), E(p["tier"]), summ["compared"],
            summ["divergent"], summ["missing"], summ["extra"], len(rows),
            len(groups)) + KEY_HINT
    if not static:
        head += "<p><span id=plist>%d tile(s) in the promote list</span>: " \
            "<code>%s</code></p>" % (len(chosen), E(promote_command(p, pdir)))
    trs = []
    for n, (key, g) in enumerate(groups):
        gid = "g%d" % n
        allv = summ["variants"].get(key, {})
        others = {v: e for v, e in allv.items() if v not in g and (
            not p["leg"] or e["leg"] == p["leg"])}
        present = ordered(list(g) + list(others))
        sel = ordered(g)[0]
        status = {v: "diverge" if v in g else others[v]["status"]
                  for v in present}
        bar = variant_bar(gid, present, sel,
                          {v: allv.get(v, g.get(v, {})).get("digest")
                           for v in present}, status)
        for v in present:
            if v in others:
                trs.append(candidate_variant_tr(gid, v, sel, bar, others[v],
                                                ctx))
                continue
            e = g[v]
            cb = "" if static else (
                "<input type=checkbox onchange='promote(this)' "
                "data-game='%s' data-tier='%s' data-cand='%s' "
                "data-tile='%s'%s>" % (
                    E(p["game"]), E(p["tier"]), E(p["cand"]), E(e["tile"]),
                    " checked" if e["tile"] in chosen else ""))
            trs.append("<tr%s><td>%s</td><td>%s%s</td>%s<td>%s</td></tr>"
                       % (gv_attr(gid, v, sel), cb, bar, E(e["tile"]),
                          meta_cells(e), E(diff_text(e))))
            if not e.get("note"):
                trs.append(panel_tr(gid, v, sel, 7, ctx.lazy(
                    lambda e=e: candidate_panel(e, p["game"], ctx))))
    body = form + head + "<table><tr><th>promote</th><th>tile</th><th>row" \
        "</th><th>VBL</th><th>leg</th><th>LOD</th><th>diff</th></tr>%s" \
        "</table>" % "".join(trs)
    return page("candidate review", body, static), [
        ("DIVERGE", e["tile"], diff_text(e)) for e in rows]


def render_history(p, ctx, static):
    """The history-compare view: one table row per sample group, showing
    one listed variant."""
    rows, notes, err = history_rows(p)
    form = ""
    if not static:
        games = [p["game"]] if p["game"] else list(pb.GAMES)
        opts = rev_options([m for g in games
                            for m, _, _ in pb.reference_manifests(g)])
        form = "<form action=/history>a <input name=a value='%s'> %s b " \
            "<input name=b value='%s'> %s %s <input name=q value='%s' " \
            "placeholder=substring> <button>compare</button></form>" % (
                E(p["a"]), rev_select("a", p["a"], opts), E(p["b"]),
                rev_select("b", p["b"], opts),
                select("game", p["game"], [""] + list(pb.GAMES)), E(p["q"]))
    if err:
        return page("history compare", form + "<p>%s</p>" % E(err),
                    static), []
    groups = group_rows(rows, lambda e: e["gkey"])
    head = "<p class=mut>%s (%s) -> %s (%s): %d changed, %d added, %d " \
        "removed in %d sample(s)</p>" % (
        E(p["a"]), E(p["a_sha"][:12]), E(p["b"]), E(p["b_sha"][:12]),
        sum(e["kind"] == "changed" for e in rows),
        sum(e["kind"] == "added" for e in rows),
        sum(e["kind"] == "removed" for e in rows), len(groups)) + KEY_HINT
    head += "".join("<p class=mut>%s</p>" % E(n) for n in notes)
    trs = []
    for n, (_, g) in enumerate(groups):
        gid = "g%d" % n
        present = ordered(g)
        sel = present[0]
        first = g[sel]
        unch = [VLABEL.get(v, v) for v in ordered(first["unchanged"])]
        bar = variant_bar(
            gid, present, sel,
            {v: g[v]["b_digest"] or g[v]["a_digest"] for v in present},
            {v: g[v]["kind"] for v in present},
            ["unchanged: " + ", ".join(unch)] if unch else ())
        for v in present:
            e = g[v]
            trs.append("<tr%s><td>%s</td><td>%s</td><td>%s%s</td>%s</tr>" % (
                gv_attr(gid, v, sel), E(e["kind"]),
                E(e["game"] + "/" + e["tier"]), bar, E(e["tile"]),
                meta_cells(e)))
            trs.append(panel_tr(gid, v, sel, 7, ctx.lazy(
                lambda e=e: history_panel(e, p, ctx))))
    body = form + head + "<table><tr><th>kind</th><th>tier</th><th>tile</th>" \
        "<th>row</th><th>VBL</th><th>leg</th><th>LOD</th></tr>" \
        "%s</table>" % "".join(trs)
    return page("history compare", body, static), [
        (e["kind"], e["tile"], "%s %s->%s" % (e["manifest"], e["a_digest"],
                                              e["b_digest"])) for e in rows]


# ---- parameters ------------------------------------------------------------------

def params(q):
    """Normalised view parameters from a dict of strings."""
    gt = q.get("gt", "")
    game, tier = (gt.split("/", 1) if "/" in gt else
                  (q.get("game", ""), q.get("tier", "")))
    try:
        floor = int(q.get("floor") or 0)
    except ValueError:
        floor = 0
    p = {"game": game, "tier": tier or "presentation", "q": q.get("q", ""),
         "leg": q.get("leg", ""), "row": q.get("row", ""),
         "cand": q.get("cand", ""), "og": q.get("og", ""), "floor": floor,
         "a": q.get("a") or "HEAD~1", "b": q.get("b") or "HEAD"}
    return p


def fix_game(p, view):
    """Default and validate the game tier (history allows every game)."""
    if view == "history":
        if p["game"] and p["game"] not in pb.GAMES:
            p["game"] = ""
        return
    if (p["game"], p["tier"]) not in tiers():
        p["game"], p["tier"] = "spyro-1", "presentation"


def render(view, p, ctx, static, pdir):
    """Render a view: (html, listed rows)."""
    fix_game(p, view)
    if view == "current":
        return render_current(p, ctx, static)
    if view == "candidate":
        return render_candidate(p, ctx, static, pdir)
    return render_history(p, ctx, static)


# ---- server ------------------------------------------------------------------------

class Handler(BaseHTTPRequestHandler):
    """Serves the views, the registered files and the thumbnails; the one
    POST rewrites the promote list."""

    ctx = None
    pdir = None
    divergent = {}

    def send(self, code, ctype, data):
        """Write a response."""
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def send_file(self, path):
        """Write a PNG file."""
        try:
            with open(path, "rb") as fh:
                self.send(200, "image/png", fh.read())
        except OSError:
            self.send(404, "text/plain", b"absent")

    def do_GET(self):
        """Route a GET."""
        u = urllib.parse.urlsplit(self.path)
        q = dict(urllib.parse.parse_qsl(u.query))
        path = u.path.rstrip("/") or "/current"
        if path.startswith("/f/"):
            p = self.ctx.files.get(path[3:])
            return self.send_file(p) if p else self.send(404, "text/plain",
                                                         b"not registered")
        if path.startswith("/thumb/"):
            d = path[7:]
            p = make_thumb(d) if pb.DIGEST_RE.match(d) else None
            return self.send_file(p) if p else self.send(404, "text/plain",
                                                         b"not in store")
        if path.startswith("/lazy/"):
            fn = self.ctx.lazies.get(path[6:])
            if fn is None:
                return self.send(404, "text/plain", b"expired; reload the page")
            return self.send(200, "text/html; charset=utf-8", fn().encode())
        view = path[1:]
        if view not in ("current", "candidate", "history"):
            return self.send(404, "text/plain", b"no such view")
        p = params(q)
        doc, listed = render(view, p, self.ctx, False, self.pdir)
        if view == "candidate":
            Handler.divergent[(p["game"], p["tier"], p["cand"])] = {
                t for _, t, _ in listed}
        self.send(200, "text/html; charset=utf-8", doc.encode())

    def do_POST(self):
        """Add or remove one tile of the promote list. The tile must be a
        divergent row the candidate view listed for that candidate."""
        if self.path != "/promote":
            return self.send(404, "text/plain", b"no such endpoint")
        n = int(self.headers.get("Content-Length") or 0)
        try:
            j = json.loads(self.rfile.read(n))
            key = (j["game"], j["tier"], j["cand"])
            tile = j["tile"]
        except (ValueError, KeyError, TypeError):
            return self.send(400, "text/plain", b"bad request")
        if tile not in Handler.divergent.get(key, set()):
            return self.send(409, "text/plain", b"not a listed divergent row")
        tiles = set(read_promote_list(self.pdir, j["game"], j["tier"]))
        (tiles.add if j.get("on") else tiles.discard)(tile)
        write_promote_list(self.pdir, j["game"], j["tier"], tiles)
        self.send(200, "application/json",
                  json.dumps({"count": len(tiles)}).encode())

    def log_message(self, fmt, *args):
        """Quiet access log."""


def serve(port, pdir):
    """Serve on 127.0.0.1 until interrupted."""
    Handler.ctx = Ctx()
    Handler.pdir = pdir
    srv = HTTPServer((HOST, port), Handler)
    print("[golden-browser] http://%s:%d/  promote lists in %s"
          % (HOST, srv.server_address[1], pdir), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    srv.server_close()
    return 0


def once(a, pdir):
    """Render one view to a static file and its row list."""
    out = os.path.abspath(a.out)
    assets = out + ".assets"
    os.makedirs(assets, exist_ok=True)
    q = {k: v for k, v in vars(a).items()
         if k in ("game", "tier", "cand", "og", "a", "b", "q", "leg", "row")
         and v is not None}
    q["floor"] = str(a.floor or 0)
    doc, listed = render(a.once, params(q), Ctx(static_dir=assets), True,
                         pdir)
    for p in (out, out + ".rows.tsv"):
        if os.path.exists(p):
            os.unlink(p)
    with open(out, "w") as fh:
        fh.write(doc)
    with open(out + ".rows.tsv", "w") as fh:
        for rec in listed:
            fh.write("\t".join(str(x) for x in rec) + "\n")
    print("[golden-browser] %s: %d row(s) -> %s" % (a.once, len(listed), out))
    return 0


def main():
    """Parse the command line; serve or render once."""
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--promote-dir", default=os.path.join(ROOT, "build",
                                                          "golden-browser"))
    ap.add_argument("--once", choices=("current", "candidate", "history"))
    ap.add_argument("--out")
    for k in ("game", "tier", "cand", "og", "a", "b", "q", "leg", "row"):
        ap.add_argument("--" + k)
    ap.add_argument("--floor", type=int)
    a = ap.parse_args()
    if a.once:
        if not a.out:
            ap.error("--once needs --out")
        return once(a, a.promote_dir)
    return serve(a.port, a.promote_dir)


if __name__ == "__main__":
    sys.exit(main())
