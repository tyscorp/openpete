# Manifest

## mod.toml

`id` and `version` are required. Every other key is optional.

``` toml
[mod]
id      = "my-mod"        # the directory or zip name; enabled.txt and --mods use that name
name    = "My Mod"        # shown in the Mods tab; defaults to id
game    = "spyro-1"       # or ["spyro-1", "spyro-3"]; omitted: see below
version = "0.1.0"
api     = 13              # minimum OPENPETE_MOD_API_VERSION; also selects the overlay-address rules and material composition
deps    = ["other-mod"]   # hard dependencies: their entry points run first
requires_games = ["spyro-2"]  # other games' discs the mod needs; see below
optional_games = ["spyro-3"]  # other games' discs the mod reads when present
sim_mutative = false      # see below
state    = "none"         # "none" | "rebuildable": permits rewind with this mod loaded
runahead = "pure"         # "pure" | "host-fx": permits runahead with this mod loaded
```

**`game`.** Which game the mod is for: one id (`"spyro-1"`, `"spyro-2"`,
`"spyro-3"`) or an array of them. `mods/` is shared across the
openpete ports, so a mod that only makes sense under one of them names it
here and the others never see it: a mod naming no game this exe runs is dropped
during discovery, before it is compiled, listed in the Mods tab, or
counted by `enabled.txt`. Ids are matched exactly against the running
game's id, so a misspelling matches nothing and the mod loads nowhere.
Without the key, the `api` pin decides: at `api = 13` or later the mod
targets every game; below 13, or with no `api` key, it targets Spyro 1
only, since every such mod was written against the Spyro 1 SDK. A mod
with `src/` compiles once per game it names, against that game's SDK
(see [Code mods](code.md#one-mod-for-several-games)).

**`requires_games`, `optional_games`.** Other games whose discs the mod
reads with `read_sector` and `game_disc_path` (see
[Code mods](code.md#other-games-discs)). Each is one id or an array of
ids, the same ids as `game`. The running game is implied and need not be
listed. A game is installed when the player's library holds a disc of
it; any release of the game counts, and the disc booted with `--disc`
counts for the running game only.

- `requires_games`: while a listed game is not installed the mod does
  not load. The log names the mod and the game, the Mods tab lists
  the mod as not loaded with the same reason, and the launcher's version
  selector, when it shows, reads `MOD <ID> NEEDS <GAME> - ADD ITS DISC`;
  dropping that game's disc on it adds the disc to the library. A mod that depends on a refused mod is
  disabled as for a failed dependency. An id that names no game this
  build knows is warned about and, in `requires_games`, refuses the mod.
- `optional_games`: the mod loads either way. While a listed game is not
  installed, `read_sector` returns -1 and `game_disc_path` NULL for it;
  `game_installed` tells the mod which case it is in, so it can drop the
  feature that game supplies.

A game in both lists is required. A disc call naming a game in neither
list is refused (see [Code mods](code.md#other-games-discs)).

**`api`.** The lowest engine API version the mod needs (see
[Publishing](publishing.md#pin-the-api-as-a-minimum)). The pin also
selects the [overlay](glossary.md#overlay)-address behaviour and material composition, and the
boundary for both is api 13, the version that adds `override_ovl` and
composition:

| pin | `override_addr` inside the overlay window, `override_name` on a raw `func_XXXXXXXX` name found in more than one overlay | `game` key absent | `[[material]]` rows and `material_register` |
|---|---|---|---|
| 13 or later | refused on every game: the call returns -1 and logs every overlay the address or name occurs in, with that overlay's names for it | every game | compose with the materials ranked below them, on every channel, terrain included; `exclusive = true` makes a row terminal |
| below 13, or no `api` key | Spyro 1: binds in every overlay with a function at that address, and logs a deprecation once per mod; Spyro 2 and 3: refused | Spyro 1 only | terminal: shade alone over the materials ranked below them; `exclusive` is ignored |

Raising a pin to the boundary for any other api entry therefore turns
the mod's window `override_addr` and ambiguous raw `override_name` calls
into refusals and makes its materials compose with the materials ranked
below them ([Shaders](shaders.md#compose-materials)); `exclusive = true`
on a row keeps that row's output as below the boundary. A game-less
manifest below the boundary loads on Spyro 1 only. Bind an overlay function with `override_ovl` or the
`OP_OVLID_<name>` wrappers (see the game's
[functions page](reference/spyro-1/functions.md)).

**Per-game support of the other keys.** `[[binding]]`, `[[config]]`,
`deps`, `requires_games`, `optional_games`, `sim_mutative`, `state`, `runahead` and `assets/` work on every
game. `assets/<game>/` (for example `assets/spyro-2/`), when present, is
used in place of `assets/` on that game. `[[material]]` rows take their
own `game` key; every game serves every render channel
([Shaders](shaders.md#declare-a-material-in-the-manifest)). `[[level]]`
rows load on Spyro 1 only; on Spyro 2 and 3 they are refused with one
log line and the rest of the mod loads ([Custom levels](levels.md)).

**`deps`.** Dependencies are topologically sorted ahead of enabled-list
order, so a library's entry point runs before its clients' regardless of
where each sits in `enabled.txt`. A cycle or a missing dependency is
logged and disables the dependent.

**`sim_mutative`.** Whether the mod writes guest RAM. The engine records
the mutative mods loaded at each save (per save, in the card's
`.modsave` file) and logs
a line when a save is loaded under a different set; progress is never
blocked, and `--adopt-saves` re-tags. Per-save data a mod keeps belongs
in [save records](save-records.md), not in the save's spare bytes. A mod with no `src/` and no
`[[level]]` is derived non-mutative. A mod with `src/` is assumed mutative
unless it sets `sim_mutative = false`. A savestate records the mutative
mods and the order they entered and hooked functions in: the `enabled.txt`
order at launch, each mod enabled from the Mods tab or hot-reloaded during
the session, and each override registered after the mod's entry point
returned. It loads only under the same mods, versions and order: the log
says `MODSET MISMATCH (mods)` or `MODSET MISMATCH (order)` and prints both
tags, such as `v2:a@1.0+b@2.0#5f1c09a2e43b7d80` (the mods, then a hash of
the order). A savestate written before the order was recorded carries a
tag without the `v2:` prefix and loads under the same mods in any order. Asset replacements, shaders, `draw_text`,
`ui_status`, and `notify` never write guest RAM. The obsolete key
`sim_neutral` has the opposite sense; it is still read when `sim_mutative`
is absent, and the load logs a warning naming the `sim_mutative` line to
write instead.

**`state`, `runahead`.** Both default to undeclared. An enabled code mod
with `runahead` undeclared disables runahead; with `state` undeclared it
refuses rewind and timeline jumps, with a one-shot warning. `runahead =
"host-fx"` declares [tick](glossary.md#tick)-hook side effects outside guest RAM (sounds,
files), which the engine suppresses during speculative ticks. `state =
"rebuildable"` declares that every host-side value derived from guest
history can be re-derived from guest RAM after a jump.

### `[[binding]]`

``` toml
[[binding]]
name    = "warp"          # read with api->binding_down(self, "warp")
key     = "F6"            # default source(s), keymap spelling
purpose = "warp home"     # shown to the player
```

`key` takes the keymap's spelling: a keyboard name, a gamepad source
(`pad:north`, `pad:-lefty`), a mouse button (`mouse:right`), or a
comma-separated list (`"F6,pad:north"`). Wheel
notches and mouse motion are not bindable (the read is a level); a key
that fails to parse is logged at load and reads 0. Conflicts across the
enabled set are reported at load. Players rebind under `[keys.mod.<id>]`
in `openpete.toml` with the same spellings, or `none`. The read is inert
while the [dev overlay](glossary.md#dev-overlay) owns the keyboard, like every host action.

### `[[config]]`

Rows declare the mod's settings. The engine renders them in the M
overlay's Mods tab as checkboxes, sliders, inputs, and combo boxes,
grouped and tooltipped. Apply writes the edit into `config.toml` with
comments preserved and reloads the mod at the next tick boundary, so the
entry point re-reads the values through `api->config_*`.

``` toml
[[config]]
key     = "gems.sfx"      # config.toml path this row edits
type    = "bool"          # bool | int | float | string
default = true            # shown when config.toml has no value
label   = "S3 collect chime"
group   = "Gems"          # section header within the mod's block
help    = "tooltip text"

[[config]]
key     = "spawn.scale"
type    = "float"
default = 1.0
min     = 0.25            # min < max renders a slider; omit for an input box
max     = 4.0

[[config]]
key     = "render.mode"
type    = "string"
default = "fast"
items   = ["fast", "fancy"]   # string + items renders a combo

[[config]]
key     = "service.token"
type    = "string"
default = ""
secret  = true            # a bug report names the key and never its value
```

A bug report ships each enabled mod's `config.toml` as the declared rows'
effective values. A row with `secret = true` and every key `config.toml`
holds that no row declares are listed by key only.

A mod whose UI is settings plus `api->ui_status` lines needs no widget
code. Custom panels use `openpete_mod_ui.h`, which is versioned
separately and carries no stability promise.

### `[[level]]` and `[[material]]`

See [Custom levels](levels.md) and
[Shaders](shaders.md#declare-a-material-in-the-manifest).

## config.toml

Optional, beside `mod.toml`. The engine parses it once, immediately before
the entry point runs. Keys are dotted paths: `"gems.models"` is key
`models` in table `[gems]`; a bare `"verbose"` is a root key.

``` toml
[gems]
models = true
sfx    = true

[chests]
collision = true
```

``` c
int     on    = api->config_bool (self, "gems.models", 1);
int64_t count = api->config_int  (self, "spawn.count", 4);
double  scale = api->config_float(self, "spawn.scale", 1.0);
char who[64];   api->config_str  (self, "greet.name", "world", who, sizeof who);
```

A missing file, a missing key, or a type mismatch returns the default
passed in. A parse error is logged and every getter returns its default;
the mod stays enabled. Bools read as 0/1 through `config_int` and
`config_float`; ints read through `config_float`; floats truncate through
`config_int`; strings never coerce.

Read the config in the entry point. An edit to `config.toml` takes effect
on the next launch.

## enabled.txt and --mods

See [Enabling mods](getting-started.md#enabling-mods).
