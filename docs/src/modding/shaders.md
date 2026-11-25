# Shaders

Mods ship GLSL source. The engine compiles it at load with in-process
glslang in the `vulkan1.0` dialect, caches the SPIR-V by content hash
under `<mod>/.build/shaders/`, and writes compile errors to
`.build/shaders/<name>.log`. A broken shader yields an unavailable effect
and a log line; the mod still loads.

Shaders run on the GPU and cannot touch guest RAM. C code may read game
state to feed uniforms; nothing flows back.

| Kind | API | Effect |
|---|---|---|
| Post-process | `shader_register`, `postfx_register`, `postfx_enable`, `postfx_set_uniforms` | Fullscreen fragment passes over the composed frame: CRT simulation, grading, scanlines |
| Material | `[[material]]` rows, or `material_register` | Replaces the fragment shading of a subset of the engine's own draws: a [moby](glossary.md#moby) class, a terrain tier, the flame plume. The manifest form needs no C |

A mod shades what the engine draws; it cannot add draws of its own.

## Glossary

| Term | Meaning |
|---|---|
| selector | The set of keys deciding which of the engine's draws a material repaints. Every key is optional and an omitted key matches everything, so a selector widens as keys are left out. |
| channel | The family of draws a selector names: `moby`, `terrain`, `player`, `flame`, `particle`, `sky`, `shadow`, `tracer`, `portal`, or `any`. |
| ident | An identity within one channel, since a channel covers several kinds of draw: a flame's ribbon against its caps, a particle kind and class, a portal preview kind, Spyro against his reflection, a tracer ribbon against Zoe's zap. |
| tier | Which terrain detail level a face belongs to, `hp` or `lp`. Omit the key to take both. |
| pass | One fullscreen post-process draw a mod registers. Passes run every [present](glossary.md#present). |
| point | Where in the frame a pass runs: `OP_POSTFX_SCENE` before the HUD, `OP_POSTFX_COMPOSITE` after it. |
| refine callback | A callback run during [extraction](glossary.md#extraction), once per matched instance, returning that instance's uniform block or declining the match. |

## Registering a shader

```c
int frag = api->shader_register(self, "shaders/crt.frag");
```

The path is relative to the mod and the extension selects the stage. The
call returns `-1` for a missing file, a compile error, or a module that
uses 64-bit floats (`double`, `dvec*`, `dmat*`: the SPIR-V `Float64`
capability). The engine creates its device without 64-bit float support,
so such a shader is refused at compile and at load, prebuilt `.spv`
included, with a log line naming the mod and the file.

Every shader includes the binding contract:

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#define OP_POSTFX_STAGE_FRAGMENT 1     /* post-process fragments only: defines op_uv, op_color */
#include "openpete_shader_api.glsl"
```

The include resolves to `<exe>/sdk/shaders/openpete_shader_api.glsl`.
The cache stamp beside each compiled module records every file the
compile included, in the SDK or beside the shader, with its hash, so an
edit to any of them recompiles the modules that read it. Material
shaders include `openpete_psx_material.glsl` or
`openpete_psx_decal.glsl` instead; never both, since each claims the same
descriptor set.

To ship without source, place `<path>.spv` where no source sits beside
it, with a `<path>.spv.ihash` sidecar matching the current interface
hash, which covers the mod API version and every `.glsl` file under
`<exe>/sdk/shaders/`. A stale sidecar refuses the shader.

## Post-process passes

```c
int fx = api->postfx_register(self, frag, OP_POSTFX_COMPOSITE, /*order*/ 0);
api->postfx_set_uniforms(self, fx, &params, sizeof params);   /* copied; any context */
api->postfx_enable(self, fx, 1);
```

Injection points:

- `OP_POSTFX_COMPOSITE`: after the scene and the HUD are composed, before
  the present blit. Sees the whole frame including the HUD.
- `OP_POSTFX_SCENE`: after the 3D scene, before the HUD, so grading and
  fog tint the world while the HUD stays untouched. Receives scene depth
  at `op_scene_depth`.
- `OP_POSTFX_PRESENT`: reserved; registration is refused.

Passes run on every present, lowest priority first. A mod later in the
enabled list ranks higher, so its pass runs later and sees every earlier
pass's output; `order` breaks ties within one mod, lower first. The
renderer runs at most 15 passes per injection point, its own included;
over that, the lowest-priority passes are dropped and one warning is
logged.

Inputs: `op_prev_pass` (set 0), `op_scene_depth` (set 1; the opaque
terrain pre-pass depth, meaningful at the `SCENE` point, no mobys),
`op_prev_frame` (set 2, for temporal effects). The engine block
`op_engine` at set 3 binding 0 carries time, the [tick](glossary.md#tick), the interpolation
alpha (the value present hooks see), the resolutions, and the aspect
ratio. The mod's own block is set 3 binding 1: declare it
[`std140`](glossary.md#std140) and
mirror the layout in the C struct by hand. `postfx_set_uniforms` copies
the bytes verbatim, at most 1024.

Example: `crt-beans` (a three-pass CRT model, shipped under
`sdk/examples/`).

## Materials

A material replaces the fragment shading of a subset of the geometry the
engine draws. A *selector* names the subset; the engine decides per draw
whether the shader runs. The shader never receives game structs and never
decides whether it applies.

A material inherits its draw group's blend mode, depth policy, and draw
order and changes colour math only. It cannot move, reorder, hide, or
re-composite geometry. On an additive draw the output is still added.

### Declare a material in the manifest

```toml
[[material]]
frag    = "shaders/tint.frag"
channel = "player"
params  = [1.0, 0.55, 0.15, 0.45]    # the shader's std140 block
```

Every selector key is optional; an omitted key matches everything:

| Key | Meaning |
|---|---|
| `frag`, `vert` | Shader paths relative to the mod |
| `channel` | `any`, `moby`, `terrain`, `player`, `flame`, `particle`, `sky`, `shadow`, `tracer`, `portal` |
| `ident` | Channel-local identity: flame ribbon (0) or caps (1); particle `(kind << 8) \| sel` with kinds 0 quads, 1 lines, 2 glow rings, 3 sparkle flares, 4 sparkle lines, 5 dragon star (`OP_PART_*`; `sel` is the particle class of a quad or line on Spyro 1 and 0 for the other kinds; on Spyro 2 and 3 `sel` is reserved: a nonzero `sel` matches no draw and logs a warning at registration, and their quads and lines are reached by ident -1; the dragon star is Spyro 1 only); shadow caster kind; portal preview kind (Spyro 2 and 3 draw the background quad and the picture, no fogged dome); player kind, 0 Spyro, 1 his mirrored reflection on a reflective floor, 2 the part of him below the water surface while he swims (`OP_PLAYER_*`; 1 and 2 are Spyro 2 and 3 only); tracer producer, 0 ribbons, 1 meshes, 2 Zoe's zap (`OP_TRACER_*`; Spyro 1 draws ribbons only); on `moby`, the moby class |
| `class` | Moby class |
| `texture` | Terrain texture-record ID, 0 to 127 |
| `level` | Level ID. Texture IDs are per level, so a `texture` row usually needs this |
| `tier` | `"hp"` or `"lp"`; omit for both. Terrain only: a row with it never matches a moby |
| `textured` | `true` or `false`; omit for both. On `moby`, `true` matches every moby triangle and `false` none |
| `env_anim` | `true` selects only the terrain faces the game animates: a face whose texture record a texture swap or UV scroll rewrites, or whose vertex or vertex colour a geometry or colour animation rewrites, textured or not. `false` selects only the terrain faces the game does not animate (from `api = 13`; below it `false` narrows nothing). Omit for both. Terrain only: a row with it never matches a moby |
| `params` | Floats handed to the shader as its uniform block |
| `exclusive` | `true` makes the material terminal: no material ranked below it composes beneath it ([Compose materials](#compose-materials)). From `api = 13`; below 13 every material is terminal and the key is ignored |
| `enabled` | `false` ships the row switched off |
| `game` | One game id or an array of them; the row registers on those games only. Omit for every game the mod loads on |

A far surface the game draws from its static low-detail mesh is not an
animated face, so at original LOD an `env_anim = true` row can select
a lava pool near the camera and not far from it, and an
`env_anim = false` row the far one.

A key the engine does not recognise is refused with an error, since a
typo that silently widened a selector would repaint geometry the row
never named. A row whose channel the running game does not serve, or
with channel `any` on a game that serves none, is logged and skipped,
and `material_register` refuses the same channel with one log line and
-1; today every game serves every channel.
`declarative-example` is a complete example: one manifest,
asset files and one `.frag`.

Terrain is drawn two ways: near and changing geometry is walked per
present; the rest is retained in GPU buffers. The engine compiles a
material's `.frag` once per path and gates both with the same occlusion
verdicts, so one shader covers both, and a terrain material does not
reduce the retained set.

### Register a material from C

```c
openpete_mod_material_selector_t sel = openpete_mod_material_sel_any();
sel.channel    = OP_CHAN_MOBY;
sel.moby_class = 42;
int mat = api->material_register(self, &sel, frag);
api->material_set_params(self, mat, &block, sizeof block);   /* std140, at most 256 bytes */
api->material_enable(self, mat, 1);
```

Build the selector with `openpete_mod_material_sel_any()` and assign
fields; never zero-initialise it. The struct carries its own size and
flag mask, and the engine refuses a selector it cannot honour exactly.

Where two materials claim one draw, the mod later in the enabled list
ranks higher, on every channel and on terrain alike, refine or
declarative. Inside one mod the registration order decides, selected by
the mod's `api` pin: from `api = 13` the later registration ranks
higher, and below 13 the first one does. Manifest rows arm before the
entry point runs, so from `api = 13` a C registration ranks above a
manifest row on the same selector, and below 13 the row does. Whether
the higher material alone shades the draw or composes over the lower
ones is the pin's other effect ([Compose materials](#compose-materials)).

On the moby channel a refine callback's `-1` means "not mine": the moby
passes to the next material in this order that matches it. A declarative
material takes it; a refine material is asked in turn. The moby keeps
stock shading only when every matching material declines, and a refine
ranked below the material that takes a moby is not asked. A material
matches a moby when its channel is `moby` or any, its class and `ident`
are the moby's class or unset, its `level` is the current level or
unset, its shader is not a decal shader, and it has no terrain key
(`texture`, `tier`, `env_anim`, `textured = false`).

A mod first enabled during a session (from the Mods tab) ranks above
every mod in the enabled list, and such mods rank among themselves by id;
its function overrides take the chain tail in the order the mods were
enabled. A restart applies `enabled.txt` order. A dependency ranks below
the mods that depend on it. In a windowed run, on the present that
enables such a mod, mobys keep the material outcome computed before it
entered, and its materials draw stock on the presents that follow until
their pipelines finish compiling. On the other channels, and on a
terrain face or moby the extraction left to the draw, the draws keep the
outcome they had before the mod entered until its composition module is
installed; a moby or terrain chain stamped with one of its materials
draws stock on the present that enables it.

### Compose materials

The `api` pin selects composition on every channel, terrain included. A
material of a mod pinned at `api = 13` or later composes. A material of a
mod pinned below 13 or with no `api` key, and a row with `exclusive =
true` (`OP_SEL_EXCLUSIVE` in a C selector's `flags`, refused below 13),
is terminal.

The engine walks the materials that match a draw from the highest rank
down. A composable material that takes the draw continues the walk; a
terminal one ends it. The draw runs every material the walk took, lowest
rank first: each `op_material` receives the previous one's output, alpha
included and unclamped, and the lowest receives `base`. A material that
returns without reading `base` replaces the ones below it. A material on
channel `any` composes on every channel it matches. A refine material
composes on the moby and terrain channels, and a refine ranked below a
composable material that takes the draw is asked as well, unless it
cannot be linked.

- The highest material that takes the draw shades it alone, and the walk
  stops there, when it is terminal, has a vertex stage, or cannot be
  linked.
- Below a composable material, a material that cannot be linked is
  skipped, a material with a vertex stage joins only as the lowest (the
  draw uses its vertex stage), and a material whose `op_material`
  returns its parameter unchanged is left out.
- At most 4 materials compose on one draw. Over 4, the walk keeps the 4
  highest-ranked and drops the rest, the lowest-ranked first.
- Linking recompiles each member from its source into one module per
  shading contract, every member's identifiers renamed, so a mod's
  shader needs no change to compose. A prebuilt `.spv` shipped without
  its source cannot be linked and draws alone. From `api = 13` a shader
  whose `op_material` reaches a `discard` is refused; below 13 it cannot
  be linked.
- Moby drop-shadows (channel `shadow`, `ident = 0`) compose among decal
  shaders (`openpete_psx_decal.glsl`). Spyro's own shadow (`ident = 1`)
  has no material stage: a material selecting it draws nothing.

A material pinned below 13 takes no material beneath it, so it shades
as it did before composition unless a composable material ranked above
it composes over it. `exclusive = true` gives a material pinned at 13 or
later the same behaviour.

`material_register` is refused inside a present, and a UI section runs
inside one. A material that follows a value the player is changing
retargets with `material_set_selector` instead:

```c
sel.moby_class = picked;
api->material_set_selector(self, mat, &sel);
```

The handle, shader, refine callback, and enabled state survive. From a
present hook or UI section the change applies at the next tick, since
selection is constant across a present; the material keeps its old key
until then.

`api->moby_classes(self, buf, cap)` returns the moby classes the current
level's load placed in the game's model table, ascending, from an
engine-side snapshot refreshed once per tick, so it is safe from a UI
section and gives a class picker no dead entries. On Spyro 3, whose
model table keeps the rows of earlier levels, the set is the classes of
the load's own model lists, not every filled row. Spyro 2's model table
also keeps earlier levels' rows and its loads keep no list, so there the
set is the classes of the level's moby array that have a model row:
every class the level placed, plus any spawned since. Class ids run to
767 on Spyro 2 and 3; the call copies at most `cap` entries, so a buffer of any
size is safe, and `moby_classes(self, NULL, 0)` returns the count.

### Write a material shader

```glsl
#version 450
#extension GL_GOOGLE_include_directive : require
#include "openpete_psx_material.glsl"

layout(set = 3, binding = 1, std140) uniform op_instance { vec4 tint; };

vec4 op_material(vec4 base)          // base = the engine's own result
{
    return vec4(mix(base.rgb, tint.rgb, tint.a), base.a);
}
```

`base` is the face as the engine would draw it: [CLUT](glossary.md#clut) decode, perspective
divide, PSX modulation, and any installed HD texture replacement applied.
For untextured draws it is the interpolated vertex colour. Return
`base.a` unchanged unless the material means to change transparency; the
engine discards the PSX transparent-texel sentinel before the call.

The engine owns `main()`. The stock fragment stage also runs the
per-pixel surface verdicts that decide which surface exists at a pixel; a
shader that owned `main()` would replace those verdicts and break
whenever the engine changed them.

### Distance fade

On a heavy-fog level under extended LOD the engine fades each world moby
into the fog: toward the level's fog colour, or toward black on an
additive or subtractive draw, by a weight that grows with view depth. The
engine applies that fade to the colour `op_material` returns, after it
returns. A material does nothing about fog: `op_color()` and `base`
arrive without the fade, and a material that returns `base` draws exactly
what the engine draws with no material. Terrain, and every draw the engine does
not fade, sees no change.

Moby drop-shadows are drawn on their own path (flat fans, depth test off,
per-pixel occlusion decided in the fragment stage). A material on that
path includes `openpete_psx_decal.glsl` instead; the function and body
are unchanged and the engine picks the include from the draw path. The
occlusion verdict is applied before `op_material` runs. Shadows blend
subtractively, so a brighter tint darkens the ground more.

`mods/channel-gallery` registers one highlight shader once per channel
with a different selector each time, toggled live; it shows what each
selector claims. It loads on all three games from one source; each row's
label and tooltip name what the row lights on the running game, and a
row that lights nothing there is labelled a known gap with the reason.

### Refine callbacks

A selector prunes cheaply; a refine callback decides per instance. It
runs during extraction in tick context and returns the instance's uniform
block, or `-1` for "not mine": a moby passes to the next matching
material (see the ordering above), and so does a terrain face (the next
refine keyed to its texture or band, then the highest-priority
declarative material matching the face); with none left it keeps stock
shading:

```c
static int refine(const openpete_mod_material_key_t* key, void* out, uint32_t cap) {
    const Moby* m = (const Moby*)api->guest(key->u.moby.vaddr);
    if (!m || m->m_State != BURNING) return -1;
    float* v = out; v[0] = 1.0f; v[1] = 0.4f; v[2] = 0.0f; v[3] = 1.0f;
    return 16;
}
...
api->material_register_refine(self, &sel, frag, refine);
```

The typed `Moby` view names the state byte (offset `0x48`) `m_State` on
Spyro 1 and `state` on Spyro 3; Spyro 2's `openpete_game_structs.h` has no
`Moby` struct yet, so a Spyro 2 build reads the byte at `vaddr + 0x48`
(`mods/channel-gallery` carries the three arms).

Available on the moby channel (keyed per instance) and on terrain (keyed
per texture ID or the untextured band). Registration and
`material_set_selector` refuse, with one log line and -1, a refine that
cannot take effect: one on another channel, one with a decal shader, and
a terrain refine with neither a `texture` key nor `OP_SEL_UNTEXTURED`. A
refine on channel any takes effect on mobys and, when keyed, on terrain.
Scope the selector as tightly as possible first; the callback is the
expensive half.

The callback is asked once per tick per key; the presents between ticks
reuse its answer. The call count is not part of the contract, so the
callback must be a pure function of its key and tick-stable guest state. Instances whose blocks are byte-identical share
one slot and one batch, so cost follows the number of distinct outcomes,
not the number of matches; one distinct block per instance is the
worst case. `channel-gallery` (shipped under `sdk/examples/`) registers
materials from C over every selectable surface.
