# openpete mods

Every subdirectory here with a `mod.toml` is a mod; an `<id>.zip` with
`mod.toml` at its root loads the same way. `enabled.txt` lists the
enabled mods, one ID per line, and order is priority: a mod listed later
runs first on a shared override and `base()` descends toward earlier
mods, then the original game code. `--mods a,b` or `OPENPETE_MODS`
replaces the list for one run; `--mods ""` enables none.

Mods are native programs. A mod's C compiles into the game process and
runs with its privileges; there is no sandbox and no signing. Install
only mods you trust. A mod with no `src/` directory never loads code.

Mods must not ship data derived from a Spyro disc. Ship a generator that
derives the assets from the player's own disc; `recolored-artisans`
(source tree only) shows the pattern.

## Documentation

The mod-author documentation (guides plus the generated API, function,
global, and struct reference) is the Modding section of the documentation
site, published at https://openpete.com/docs/modding/. It builds with the
game:

```
cmake --build --preset linux-release --target docs
```

The site lands in `build/<preset>/games/spyro-1/docs-site/index.html`;
every package ships it as `docs/` beside the executable.
`scripts/build-docs.sh --serve` runs a live preview (needs
`pip install mkdocs-material`). Sources: `docs/src/modding/`.
`AUTHORING.md` is a single-file reference for writing mods against the
shipped SDK.

## In-tree mods

`examples.txt` names the mods `scripts/package-dist.sh --with-mod-sdk`
stages into `<exe>/sdk/examples/`; the documentation's Examples page
lists exactly those. Every entry ships only original content.

In-tree mods: `declarative-example`, `music-swap`,
`hd-single-texture`, `sfx-single-sound`, `declarative-reshade`, and
`recolored-artisans` are declarative; `hello-anchor`, `double-gems`,
`natural-gems`, `hud-stopwatch`, `crt-filter`, `crt-beans`, `scene-sepia`,
`channel-gallery`, `moby-instance-tint`, `egg-hud-3d`, `teapot-dragons`,
`crash3-models`, `supercharge-anywhere`, `imgui-panel`, `glyph-v-fix`,
`game-arms` (one override arm per game), `pose-report` (one source
for every game, through the game services), and `debug-flags` (Mods
panel buttons that set progress flags a memory card would hold) ship
code.

CI fixtures, not examples: `arena-canary`, `arena-probe`, `chain-duo-a`,
`chain-duo-b`, `crash-dummy`, `demo-lib`, `frame-inner`, `frame-park`,
`frame-peer`, `lib-and-client`, `raw-probe`, `sfx-replace-probe`,
`vstream-rehandle`. `tests/harness/check_mods.sh` runs
every mod's `modtest.toml`; `python3 tools/modtest.py mods/<id>` runs
one. A mod whose assets are derived locally from a disc and never
committed lists them in `requires`, and the runner skips it by name when
they are absent instead of passing or failing.

`tools/modget.py` installs mods from a JSON mod-source that names a zip
URL and a mandatory sha256 per entry.
