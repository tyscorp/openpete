# Glossary

The Modding pages use the engine's own terms. Each entry below is the one
definition of its term; a page links the entry at the term's first use.

The engine runs three clocks:

| Clock | Rate | Counts |
|---|---|---|
| [tick](#tick) | 1125000/37609 Hz, about 29.913 per second | steps of the game's logic |
| [VBL](#vbl) | 2250000/37609 Hz, about 59.826 per second, two per tick | the console's display refreshes |
| [present](#present) | the render rate, set by `--render-fps` | images the engine shows |

### arena

Guest memory a mod owns, from `api->guest_alloc`. It is timeline state:
savestates, rewind and runahead carry and restore it like any other guest
bytes, which host statics are not.

### bk2

An input movie in BizHawk's `.bk2` format: the pad state of every VBL of a
run, written by `--bk2-record` and replayed by `--bk2`. A replay of the
same movie on the same build and mod set produces the same game, tick for
tick.

### canonical, sub-tick

The scene extracted at the tick itself is canonical. Scenes drawn between
two ticks are sub-tick and carry `alpha`, how far past the tick they sit,
from 0 up to 1.

### class

A moby's type, `m_Class`. It selects the model the moby draws with and the
code that runs it, and it is the key most overrides and material
selectors are written against.

### CLUT

Colour look-up table: the PSX palette a 4-bit or 8-bit texture indexes,
16 or 256 15-bit colours held in VRAM. Each textured face names its CLUT
in a halfword of its primitive.

### compose thread

The thread that builds each present's dev overlay and runs every mod UI
section body. It runs at the same time as the sim thread, so state a UI
body shares with a tick hook or a present hook needs the handoff in
[Code mods](code.md#threads).

### dev overlay

The panel the ++m++ key opens (keymap action `devui`; on a controller,
hold Back), titled **openpete config**. Its **Mods** tab toggles mods,
edits `[[config]]` rows, shows `ui_status` lines and runs mod UI sections.
The settings menu, opened with ++esc++ (action `menu`), is a separate
screen.

### DMA-reachable pool

The part of the arena below physical 16 MiB, guest 0x80200000 to
0x80FFFFFF, 14 MiB. The GPU follows packet links through 24-bit addresses,
so packets, primitives and ordering tables live here. Every `guest_alloc`
call without `OP_GALLOC_NO_DMA` takes a block from it.

### extraction

The walk that turns guest state into draw data, once per scene, on the
sim thread. Refine callbacks and present hooks run here.

### general pool

The part of the arena above physical 16 MiB, guest 0x81000000 to
0x9EFFFFFF, 480 MiB. A `guest_alloc` call with `OP_GALLOC_NO_DMA` (api 13)
takes a block from it; nothing may truncate an address inside such a
block to 24 bits.

### guest RAM

The game's 2 MB of PSX memory, held as a byte array in the process.
Writing it changes gameplay; everything else a mod does cannot.

### handoff

Process handoff: the engine writes the session to a file, starts a new
process of itself and restores the session there (`--resume-handoff`).
Guest RAM and the arena cross the process boundary; host statics and host
pointers do not.

### moby

The game's own name for a movable world object: Spyro (sometimes), a gem,
an enemy, a chest, a 3D HUD element.

A moby interpolates between ticks only while it stays allocated: a moby
freed and another allocated in its slot within one tick are two mobys, the
old one drawn until the tick's state takes over, never blended. Mobys a
mod allocates through `api->call` for each tick's draw and frees before
the next update keep interpolating when the mod allocates them in the same
order every tick: the n-th such allocation of a tick continues the n-th of
the tick before, in whichever slot the game's allocator hands it.

### mutative

A mod that writes guest RAM, declared as `sim_mutative` in the manifest.
Each save is tagged with the mutative set that wrote it; a savestate is
tagged with the set and its order.

### overlay

A block of the game's code loaded from the disc into a fixed address
window while it is needed, such as a level's `.OVL`. An address inside
the window names a different function in each overlay, which is why
`override_ovl` takes an overlay id. The panel the ++m++ key opens is the
[dev overlay](#dev-overlay), a different thing.

### override, `base()`

A mod's function that replaces a game function, and the call that runs the
next override in the chain and then the original. Where `base()` sits in
the body makes the override a pre-hook, a post-hook or a replacement.

### present

One image the engine shows, the third clock. How many presents a tick
gets depends on the machine and on `--render-fps`, so a present count is
never a contract.

### PsyCross

openpete's re-implementation of PsyQ, the PSX SDK libraries the game was
built against (GPU, CD, sound, pad). Its GPU rasterizer draws the
reference image a capture writes as `<out>-og.png`, beside the native
renderer's `<out>-vk.png`.

### render path

One family of draws of the native renderer that `--render-paths` selects:
`cyclorama`, `spyro`, `moby`, `terrain`, `shadow`, `flame`, `particles`,
`glow`, `sparkle`, `tracer`, `transition`, `portal`. A material selects
draws by render channel instead ([Shaders](shaders.md)).

### save record

A mod's own data for one game save, kept by the engine in `<card>.modsave`
beside the card and delivered back when that save is loaded
(`api->save_register`, api 13). See [Save records](save-records.md).

### savestate, rewind, runahead

Timeline features: capture and restore a moment, step backwards,
speculatively run ticks ahead. Each needs the mod to declare how its own
state behaves (`state`, `runahead`), and each is refused for an undeclared
mod.

### sim thread

The thread that runs ticks: the entry point, overrides, toggle, refine and
shutdown hooks, and, at extraction, present hooks. Two calls on it never
overlap.

### SPU

The PSX sound processor: 24 voices playing ADPCM samples from 512 KB of
sound RAM. openpete emulates it; a sound replacement swaps a sample where
an SPU voice's decoded audio reaches the mixer.

### std140

The GLSL uniform-block layout rule a mod's shader block follows: a `vec3`
or `vec4` starts on a 16-byte boundary and every array element takes 16
bytes, so a block of `vec4` members matches a C array of floats.

### tick

One simulation step, 29.913 per second (NTSC 1125000/37609). Gameplay
state advances only on a tick, so a mod that changes what the game does
works in tick context.

### tick context, present context

Where a call runs. Guest RAM writes, `api->call`, `guest_alloc` and
registration are legal in tick context and refused in present context,
which covers present hooks and UI section bodies.

### tpage

Texture page: the 256 by 256 region of PSX VRAM a textured face samples,
together with its colour depth and blend mode, named in a halfword of the
face's primitive. The mode bits are `attr >> 7 & 3` of a model face's
`attr` halfword.

### vaddr

A PSX virtual address, the `0x80...` form the game uses internally.
`api->guest(vaddr)` turns one into a host pointer. Guest structs store
vaddrs, never host pointers.

### VBL

The PSX vertical blank, 59.826 Hz, two per tick. The CLI word "frame"
means VBL: `--frame-limit` counts these.
