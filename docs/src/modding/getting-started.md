# Getting started

## Where `mods/` lives

| Install | `mods/` location |
|---|---|
| Windows | Next to `openpete.exe` |
| Linux `.tar.xz`, AppImage | `~/.local/share/openpete/mods/` (`$XDG_DATA_HOME/openpete/mods/` when set) |
| `portable = true` at the top level of `openpete.toml` | Next to the executable, when that directory is writable |

On Linux a writable `mods/` directory already present next to the
executable is used in place of the per-user path. `--mods-dir DIR` or
`OPENPETE_MODS_DIR` overrides every location above.

## Enabling mods

`mods/enabled.txt` lists enabled mod IDs, one per line; `#` starts a
comment. Without the file every mod in the directory loads, in
lexicographic order. Order is priority: on a function hooked by several
mods, the mod listed later runs first and `base()` descends toward earlier
mods, then the original game code.

`--mods a,b` or `OPENPETE_MODS=a,b` replaces the list for one run.
`--mods ""` runs without mods.

The [dev overlay](glossary.md#dev-overlay) (hold ++m++; ++esc++ opens
the separate settings menu) has a Mods tab that toggles a loaded mod at
the next [tick](glossary.md#tick) boundary and edits its settings. It
does not reorder mods or write `enabled.txt`. A mod enabled there that did
not run at launch, and a hot-reloaded mod, runs first on every function it
hooks until the program exits; a savestate records that order
([`sim_mutative`](manifest.md)).

A distribution packaged with the SDK carries working examples under
`<exe>/sdk/examples/<id>/`; copy one into your `mods` directory to run
it. The [Examples](reference/examples.md) page lists them first.

## Directory layout

| Path | Contents |
|---|---|
| `mod.toml` | Manifest. See [Manifest](manifest.md). |
| `config.toml` | Optional player-editable settings, read through `api->config_*`. |
| `src/*.c` | C sources, compiled on load against `<exe>/sdk/games/<game>/` and `<exe>/sdk/`. |
| `shaders/*.vert`, `*.frag` | GLSL, compiled on load. See [Shaders](shaders.md). |
| `assets/` | Replacement files: `textures/`, `music/`, `sfx/`, `wad/`, `levels/`. A mod for several games may ship `assets/<game>/` (for example `assets/spyro-2/`); the engine uses it in place of `assets/` on that game. See [Textures](textures.md), [Music](music.md), [Sounds](sounds.md), [Custom levels](levels.md). |
| `data/` | Per-mod directory for files the mod writes, created on first `api->data_dir()` call. It persists across runs for a folder mod; for a zip mod see below. |
| `.build/` | The engine's compile cache, one `.build/<game>/` per game. Not part of a release. |

An `<id>.zip` with `mod.toml` at the archive root, or inside a single
top-level directory that wraps every entry (the layout "zip the folder"
produces), loads the same way: the engine unpacks it under `mods/.unpacked/<id>/` and skips the unpack while
the zip's hash is unchanged. A folder and a zip with the same ID resolve
to the folder.

!!! warning "A changed zip deletes the zip mod's data and settings"

    A zip mod's directory is `mods/.unpacked/<id>/`, so its `data/`
    (`api->data_dir()`) and the `config.toml` the Mods tab writes live
    there. When the zip's hash changes (a new version dropped in), the
    engine extracts it afresh and replaces the whole `.unpacked/<id>/`
    tree: every file the mod wrote under `data/` and every setting the
    player changed in the Mods tab is deleted. A folder mod keeps both.

## A declarative mod

Assets are picked up by file convention, with no manifest block:

- `assets/music/track_NN.wav` replaces music track NN.
- `assets/textures/<name>.png` replaces a texture in the native renderer.
- `assets/sfx/<name>.wav` replaces a sound effect.

Manifest blocks cover what files alone cannot: `[[level]]` wires a custom
level, `[[material]]` re-shades geometry the engine draws, `[[config]]`
declares settings the engine renders, `[[binding]]` declares a
rebindable key. See [Manifest](manifest.md).

## A code mod

A code mod compiles on the player's machine. The Windows archive and the
Linux AppImage bundle the compiler; the Linux `.tar.xz` uses the system
compiler unless it was packaged with one. The engine takes the first of:

1. `OPENPETE_MOD_CC`, for mod compiles only;
2. the bundled compiler, `recomp/tools/llvm-mingw/bin/x86_64-w64-mingw32-clang.exe`
   on Windows or `recomp/tools/clang/bin/clang` on Linux, next to the
   executable;
3. `OPENPETE_CC`;
4. on Linux, `CC`, then `cc` on `PATH`.

On Windows with none of the first three, no compiler resolves and a mod
loads only from a prebuilt object ([Publishing](publishing.md#prebuilt-objects)).

1. `mods/hello/mod.toml`:

    ``` toml
    [mod]
    id       = "hello"
    name     = "Hello"
    version  = "0.1.0"
    game     = "spyro-1"   # the game the source is written for
    api      = 1           # lowest engine API version the mod needs
    state    = "none"      # keeps no state derived from game history: rewind allowed
    runahead = "host-fx"   # has host-side effects (the log line): runahead allowed, suppressed while speculating
    sim_mutative = false   # never writes guest RAM
    ```

    `CameraUpdate` is a Spyro 1 name; the shipped `hello-anchor` example
    adds the Spyro 2 and 3 arms ([One mod for several games](code.md#one-mod-for-several-games)).
    Without `state` the mod refuses rewind, and without `runahead` it
    disables runahead ([Manifest](manifest.md)).

2. `mods/hello/src/hello.c`:

    ``` c
    #include <openpete_mod_api.h>

    static const openpete_mod_api_t* g_api;
    static openpete_mod_t* g_self;
    static unsigned calls;

    static void on_camera_update(CPUState* cpu) {
        if ((++calls % 300u) == 0u)   /* every ~10 s at 29.913 ticks/s */
            g_api->log(g_self, OP_MOD_LOG_INFO, "CameraUpdate call #%u", calls);
        g_api->base(cpu);
    }

    int openpete_mod_entry(const openpete_mod_api_t* api, openpete_mod_t* self) {
        g_api = api; g_self = self;
        return api->override_name(self, "CameraUpdate", on_camera_update);
    }
    ```

3. Add `hello` to `mods/enabled.txt` and launch.

The engine hashes the sources, compiles them when the hash changed (a
"COMPILING MODS" stage of the launcher's progress screen), loads the
object, and runs `openpete_mod_entry`. A compile failure disables the mod and writes the
error to `mods/hello/.build/<game>/compile.log`; the game still starts.
Sources that failed are not compiled again until they, the SDK or the
compiler change, or until `--compile-mods` runs. A
non-zero return from the entry point disables the mod.

`api = N` is the minimum engine API version the mod needs. The loader
refuses a mod whose pin exceeds the engine's version before compiling it.
The API struct is append-only, so a newer engine always satisfies an older
pin. Never compare `api->api_version` with `!=`; guard a single late entry
with `if (api->api_version >= N)`.

### Ways to hook a function

1. **By name.** `api->override_name(self, "CameraUpdate", fn)`. Names come
   from the [function reference](reference/spyro-1/functions.md) and survive
   engine updates.
2. **Through a prototype.** Functions with a natural signature get typed
   wrappers in `openpete_sdk_wrappers.h`: `OP_OVERRIDE_IMPL_Foo`,
   `op_base_Foo(cpu, ...)`, `op_call_Foo(cpu, ...)`, with guest-to-host
   pointer marshalling. Prototypes exist for part of Spyro 1's and Spyro
   3's functions and for none of Spyro 2's; the Signature column of each
   game's Functions page ([Spyro 1](reference/spyro-1/functions.md),
   [Spyro 2](reference/spyro-2/functions.md),
   [Spyro 3](reference/spyro-3/functions.md)) lists them.
3. **By address.** `api->override_addr(self, 0x8003xxxx, fn)`. Arguments
   are in `cpu->a0..a3`, the return value in `cpu->v0`; the register
   fields need `#include <psx_runtime.h>` (in `sdk/`), since the API
   header only forward-declares `CPUState`. Addresses are
   pinned to the SCUS-94228 binary; the loader logs a warning on a mod's
   first raw registration.