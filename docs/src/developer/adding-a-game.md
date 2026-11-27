# Adding a game

How a PSX title becomes a game the `openpete` executable runs. Three ports
exist: `games/spyro-1/` (the first, with its own entry and the shipped
distribution), `games/spyro-2/` and `games/spyro-3/` (every vaddr through
the recompiler, the shared entry). All three draw through the native
renderer: the walks are under `games/common/renderer/`, and each game
supplies its engine layout and decoder addresses under
`games/<game>/renderer/`. The second and third are the template: a fourth
game is `games/<game>/` holding a `recomp.conf`, a `game_extras_impl.c`, a
`module_imports.h`, a `manifest/` and a `CMakeLists.txt` that calls
`openpete_add_game`, plus the registration rows of
[step 7](#7-registration).

Under shared linkage (`linux-release`, `windows-mingw` and their `-launcher`
presets) every game's host code links into one executable,
`build/<preset>/openpete` (`openpete.exe` on MinGW), and each game's
recompiled code is a module, `<tag>-recomp.so` (`.dll`), beside it; the
executable selects the game at startup and loads that game's module. Under
static linkage (the MSVC, macOS and wasm presets) each game has its own
executable `build/<preset>/games/<game>/openpete-<tag>`
with the same sources. `<tag>` is the game id without its hyphen
(`spyro-2` -> `spyro2`).

## Prerequisites

- A matching-decomp git repo whose `make` produces a GNU ld linker map
  (`build/psx.map`), `build/PSX.EXE`, the PSYQ object the map places
  (`build/asm/psyq.o`), and the overlay binaries (`build/wad/*.ovl`) if the
  title uses overlays.
- The original disc BIN/CUE for the title.
- `third_party/psxrecomp/build/PSXRecomp` (built from `third_party/psxrecomp`).

## 1. Submodule the decomp

```bash
git submodule add <decomp-url> games/<game>/<game>
```

The nested layout keeps openpete commits separate from decomp commits. The
submodule is read-only from openpete's side.

## 2. `recomp.conf`

A shell fragment of `KEY=value` lines, sourced by `scripts/regen-recomp.sh`
and `scripts/regen-manifest.sh` and parsed by `tools/gen_game_identity.py`.
Copy `games/spyro-2/recomp.conf` and set:

| Key | Meaning |
|---|---|
| `DISPLAY_NAME`, `EXE_FILENAME` | window title; the EXE's name in the disc root, from which the disc serial is spelled |
| `DECOMP_DIR`, `PSX_EXE`, `WAD_DIR`, `ASM_DIR` | decomp paths, repo-relative |
| `OVERLAY_LOAD_ADDR`, `OVERLAY_RANGE` | the one vaddr every overlay loads at; that address through the largest overlay |
| `LIBCD_BLOCK_ADDR`, `CD_READ_CB_ADDR` | libcd's state block; the slot a translated `CdReadCallback` stores the handler in (empty when the game registers through the thunk) |
| `CHANNEL_MANIFEST` | render-channel substitution table; empty for a game without one |
| `MAIN_EXE_THIRD_PASS`, `RA_SPILL_RETURNS`, `LEVEL_ID_ADDR` | recompiler passes and the level-switch word, per the comments in the file |
| `JUMPTABLE_UNSWITCHED` | in-image word-table `jr` sites the regen leaves without an index switch; the `jumptable_dispatch_<game>` ctest refuses any other |
| `FIRST_BOOT`, `FIRST_BOOT_CARVE` | `1` when the game has a first-boot module build: the regen emits its converged manifest into `manifest/recomp/<serial>/` (`scripts/emit-recomp-manifest.sh`); `wad_entry` records each overlay's WAD.WAD entry for the carve |
| `FIRST_BOOT_DECRYPT`, `FIRST_BOOT_RESTORE_SYMBOLS`, `FIRST_BOOT_EXE_RESTORE_SYMBOLS` | the post-link mastering transforms between the disc's copy of a binary and the decomp's: `1` when an overlay may be XORed with the word at its byte 4 over `[0x1000, key - load address)`; psx.map symbol templates (`{name}` = the overlay) and executable symbols whose words the disc may carry rewritten. The emitter records, per binary, the inverse that turns the disc's bytes into the decomp's (`decrypt = true`, `restore = [[offset, word], ...]`), which the carve applies |

The build generates `game_identity.c` from these (the runtime descriptor's
identity `<tag>_game_identity`, which `game_get_id`,
`game_get_display_name` and `game_get_exe_filename` return, and the PSYQ
descriptor's recomp.conf addresses `<tag>_psyq_conf`, which
`psyq_game_overlay_vaddr`, `psyq_game_libcd_block_addr` and
`psyq_game_cd_read_cb_addr` return), so no hand-written source carries an
identity constant.

## 3. Manifests and codegen

```bash
GAME=<game> bash scripts/regen-manifest.sh     # manifest/symbols.toml, globals.toml from psx.map
python3 tools/gen_psyq_names.py ...            # recover PSYQ names for an unnamed psyq.s (see its docstring)
python3 tools/gen_psyq_toml.py --exe games/<game>/<game>/build/PSX.EXE \
        games/<game>/<game>/build/psx.map build/asm/psyq.o \
        games/<game>/manifest/psyq.toml       # PSYQ symbol -> address
GAME=<game> bash scripts/regen-recomp.sh       # generated/: main EXE, overlays, dispatch, stubs
```

`psyq.toml`'s second header line records the options it was generated
with (`--translate` per symbol translated instead of stubbed, among them
the libgte closure); a regeneration repeats them.

`manifest/` holds `psyq.toml`, `psyq_names.toml`, `forced_entries/` (entry
points the recompiler's scanner misses), `asm_dispatch_targets/`,
`ram_diff_expect.toml` for `scripts/check_ram_diff.sh`, and two manifests
the build compiles into the game's build record when present:
`gameplay_state_regions.toml` (the oracle's rows, [step 6](#6-running)) and
`disc_boot_order.toml`, the disc cache loader's boot-order profile,
generated from a boot trace ([disc-io.md](disc-io.md#boot-order-profile)).
A game without the first registers an empty region table
(`games/common/state_regions_none.h`); without the second the loader reads
in file order. `module_abi_exports.txt`, when present, adds the game's
names to the executable's module export surface beside
`games/common/module_abi_exports.txt`. `symbols.toml` and `globals.toml`
are written by `regen-manifest.sh` and gitignored, as is `generated/`.

## 4. `CMakeLists.txt` and `module_imports.h`

```cmake
include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(<game>
    SOURCES
        <per-game TUs>)
```

`openpete_add_game` (`cmake/openpete_game.cmake`, whose header lists every
keyword) builds:

- The recomp module `<tag>_recomp_module`: the generated code plus
  `games/common/overlay_dispatch.c` and `module_main.c`. Under shared
  linkage it is `<tag>-recomp.so` (`.dll` on Windows) beside the executable
  that loads it (`games/common/recomp_attach_shared.c`); under static
  linkage it links into the game's executable.
- The host object library `<tag>_host`: `game_extras_impl.c`, the generated
  identity TU, the generated build record, the game's PSYQ stub address
  table (`gen/psyq_stub_table.c`, `<tag>_psyq_stub_table`), the slot files
  and `SOURCES`. Code under `games/common/` compiles once whatever the
  number of games, as do the abort-on-call PSYQ stubs
  (`openpete_psyq_stubs`, the top-level `CMakeLists.txt`): the union of the
  games' generated `psyq_stubs.c` names minus the names the shared thunk
  layer defines (`tools/filter_psyq_stubs.py`); a reached stub reports the
  running game's address from its table.
- The game target `openpete-<tag>`. Under the one executable it builds
  `openpete` and stages the game's module, mod SDK, assets and first-boot
  inputs beside it, and writes no executable of its own; otherwise it is
  the game's executable.

The build record `<tag>_game_build` (`games/common/game_build.h`) is
generated from `games/common/game_build.c.in` into the game's build `gen/`
directory: its module name and host-to-module
forwarders, its first-boot pipeline, and the registration of the tables
generated from its manifests. The executable's entry
(`games/common/game_entry.cpp`) selects one record among the linked games,
in order by the serial of the disc named by `--disc` or `--ingest`, by
`--game`, by the invoked executable name, installs it and runs the one
game entry, `openpete_game_main` (`games/common/game_main.cpp`).
When the executable links several games and none of these names one, the
launcher picks before any game is installed (`games/common/game_pick.cpp`):
it lists every game's library entries, and the game recorded on the entry
it boots (a selector pick or a dropped disc) is installed; the game entry
then boots the same entry. `--help`, the reports, `--compile-mods`, the
batch modes and the surface dump take the first linked game. A game's
known disc hashes, which label library entries, sit on its runtime
descriptor (`known_discs`).

What the entry does differently per game is a field of the runtime
descriptor: `mod_desc` (the mod layer's descriptor), `mod_disc_configure`
(mod services that read the identified disc, Spyro 1's WAD asset overlay)
and `dump_sfx_pack` (the `--dump-sfx-pack` census, Spyro 1's).

Keywords: `FEATURE_LIST`, `ORCHESTRATOR`, `INTERP_REGIONS`,
`TERRAIN_GAME`, `MOBY_GAME` and `PLAYER_GAME` name the game's feature list
and native-renderer TUs; each is optional, and a game without one names no
table for it in its runtime descriptor, which binds the inert default
(`games/common/game_host_desc.c`, `games/common/renderer/orchestrator.c`).
`ICON_SOURCE`, `SYMBOLS_MANIFEST`, `MODULE_LINKAGE` and `MODULE_SOURCES`
override the defaults; `MOD_SDK` generates and stages the game's mod SDK;
`FIRST_BOOT_SOURCES` with `FIRST_BOOT_EXTRACT` adds the first-boot module
build under `OPENPETE_RECOMP_FIRST_BOOT` (the launcher presets); `EXTRACT`
in place of `FIRST_BOOT_EXTRACT` names a carve compiled from `SOURCES`, so
every build answers `--extract-recomp-inputs` (Spyro 1). A game
listed in `OPENPETE_GAMES` whose `generated/` holds no recompiled output
stops the configure; `-DOPENPETE_GAMES=<ids>` narrows the list.

`module_imports.h` lists the recompiled functions the game's host code
calls by name, as `OPENPETE_GAME_MODULE_FUNCS(X)` with one `X(func_...)`
per function; a game that calls none defines the macro empty (Spyro 2).
Under shared linkage each name is a forwarder in the build record, bound
from the loaded module at attach.

Two build-time refusals guard the merged image under the one executable:

- `tools/check_module_imports.py` (target `<tag>_module_imports`) refuses
  a module import with no definition or more than one among the
  executable, the module and their libraries, and a symbol both the
  module and the executable define outside the linked games'
  `module_imports.h` forwarders. On Windows the module's link against the
  executable's import library is the refusal.
- `tools/check_game_includes.py` (target `openpete_game_include_check`)
  refuses a host TU whose include path leads into a game's directory. A
  game source names a header of its own directory relative to itself; any
  other host TU names a game's header by its game-qualified path
  (`"spyro-1/gen/x.h"`).

## 5. `game_extras_impl.c`

`core/include/game_extras.h` declares the per-game hooks, which core
forwards to the game's runtime descriptor
(`core/include/openpete_game_runtime.h`, `openpete_game_runtime_t`).
`game_extras_impl.c` defines that descriptor as `<tag>_game_runtime`, the
name the build record names; a hook or table it leaves NULL takes the
default `game_extras.h` documents. Copy `games/spyro-2/game_extras_impl.c`:
the boot and savestate-restore hooks
(`games/common/game_extras_overlay_resync.c`, which re-point the overlay
dispatcher), the per-tick hook, `override_dispatch` (the per-function
intercept, here the freecam freeze and the write census), the
gameplay-live predicate, the CLI resolvers, and the `game_host_desc_t`
naming the tables of the game's renderer TUs.

The descriptor's `psyq` member is the game's `psyq_game_hooks_t`
(`games/common/psyq/psyq_game_hooks.h`): the PSYQ-internal RAM mirrors and
prologue frames, the library addresses, the CD-read classifiers, the music
route and the sound tables, plus the generated `<tag>_psyq_conf` and
`<tag>_psyq_stub_table`. The shared `psyq_game_*` entries the thunk layer
calls forward to it (`games/common/psyq/psyq_game_hooks.c`); a member left 0
or NULL takes the inert default the header documents.

The shared entry builds the launcher's view of the game
(`openpete_launcher_game`, `core/include/openpete_launcher.h`) from the
descriptor's identity, so the launcher's library, drop screen, version
selector and progress screen (`core/launcher/`) serve a new game with no
code of its own.

## 6. Running

```bash
build/linux-release/openpete --headless --no-card \
  --disc games/<game>/<game>/build/<serial>.cue
```

`--game <game>` selects the game when no disc names it. `--no-card` is
required for Spyro 2 and 3: their memory-card driver waits on a line-7
interrupt the host does not deliver yet. A PSYQ entry with no thunk
aborts with its name; `OPENPETE_PSYQ_STUB_CONTINUE=1` logs each distinct
one and continues, which enumerates the untranslated surface in one run.
`scripts/psyq_stub_census.sh <game>` selects the stubs as weak (`W`)
symbols of the executable, which none is since the stubs compile once as
strong symbols, so it reports no stub.

`scripts/check_ram_diff.sh <game>` compares whole RAM against DuckStation at
an anchor, under the game's oracle golden when it exists.

The oracle gate is per game. `games/<game>/manifest/gameplay_state_regions.toml`
lists the rows the golden hashes (derived from the link map by
`tools/derive_state_regions.py`, minus the classes `ram_diff_expect.toml`
names; the `state_regions_derivation_<game>` ctest re-derives it with
`--check`); `games/<game>/oracle.conf` is the capture and gate recipe
(golden path, record count, disc, anchor, recorder options from
`psyq.toml`, the flags the run needs, the music-routing bounds), and the
one source of the game's disc and boot flags for the shell and Python
harnesses (`scripts/presentation-game.sh`, `scripts/lib/game_conf.py`).
The golden itself is generated, never committed:
`tests/harness/check_psx_oracle.sh --game <game>` captures it from the
pinned DuckStation fork on the first run and verifies against it after; a
ctest row `psx_oracle_match_<game>` runs it under `ctest -L oracle`. A new
game's acceptance is the gate at PASS over 20000 records with a POKE
negative control failing at its record
([`duckstation-oracle.md`](duckstation-oracle.md)).

The game's harness inputs live in its directory: `games/<game>/bk2/`
(movies; the perf corpus of `tests/harness/bench-corpus.py --game`) and
`games/<game>/corpus/` (the presentation, freecam, sub-tick, skip-target
and walk-cache manifests the harnesses read).

## 7. Registration

Outside the game's directory a game is named in these lists, each of
which carries the three games today:

- `OPENPETE_GAMES` in the top-level `CMakeLists.txt` (or
  `-DOPENPETE_GAMES=` at configure time), and the per-game ctest `foreach`
  lists in the same file.
- The serial table of `core/config/game_registry.c`, which identifies the game
  from its disc; `tests/unit/identify_test.c` checks each row against the
  game's `recomp.conf`. The `--game` refusal message in the same file and
  the `--game` help text in `core/config/knob_registry.c` name the games.
- `GAMES` in `scripts/lib/game_conf.py`, `scripts/golden-fleet.py` and
  `scripts/presentation-blessed.py`; `ALL_GAMES` in
  `scripts/fences/surface_coverage.py`.
- `games/shipped.toml`, for a game the distribution packages ship.

## 8. What stays Spyro-1-specific

- The executable's Win32 icon,
  application manifest and VERSIONINFO are OpenPete's, from
  `games/common/openpete.rc.in` and `games/common/openpete.manifest`, and
  belong to no game. Every game has the launcher
  (`core/launcher/`) and, with `MOD_SDK`, the mod SDK.
- The dragon-rescue golden tier (`tests/golden/spyro-1/dragons/`) exists
  for Spyro 1 alone. The presentation golden itself is per game
  (`tests/golden/<game>/presentation/blessed.tsv`; ctest rows
  `presentation_golden`, `presentation_golden_spyro2` and
  `presentation_golden_spyro3`), as is the windowed oracle row
  (`psx_oracle_windowed`, `psx_oracle_windowed_spyro-3`; none for
  Spyro 2).
- `core/` still carries Spyro-named debug knobs (`SPYRO_SCRATCH_MAT`,
  `SPYRO_UNTEX` in `core/config/knob_registry.c`); the `core/` purity check
  refuses bare `0x800XXXXX` literals and Win32-only APIs, not Spyro-specific
  names.
