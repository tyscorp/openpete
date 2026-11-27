# Performance

The settings below trade picture against speed. None of them changes the
game's state, which stays the same [tick](../help/glossary.md#tick) for tick whatever they are set to.

## The performance readout

{{ flag("knob:perf_stats") }} <!-- id: knob:perf_stats home --> shows a readout in a corner of
the window.

| Value | Shows |
| --- | --- |
| `off` | Nothing |
| `fps` (default <!-- id: knob:perf_stats -->) | The [frame rate](../help/glossary.md#frame-rate) |
| `detailed` | The frame rate plus a [frame pacing](../help/glossary.md#frame-pacing) readout: the [VBL](../help/glossary.md#vbl) rate, deadline slips, late ticks per 1000, and the share of presents that were shed or interpolated |

The settings menu's **SHOW FPS** row switches it off and on; the dev
overlay's **Performance stats** row sets all three levels.
{{ flag("knob:perf_position") }} <!-- id: knob:perf_position --> places it: `top-left`,
`top-center`, `top-right` (default <!-- id: knob:perf_position -->),
`bottom-left`, `bottom-center`, `bottom-right`.

`--show-fps 0|1` <!-- id: flag:--show-fps --> is a deprecated spelling of
`--perf-stats off|fps`.

## What costs what

| Setting | Cost |
| --- | --- |
| [Frame rate](smoothness.md#frame-rate) | GPU and CPU work per picture, times pictures per second. **UNLIMITED** with VSync off draws as many as the GPU can. |
| [Supersampling](quality.md#supersampling) | GPU work and memory grow with N squared: 4X draws 16 pixels per shown pixel. |
| [Level of detail](quality.md#level-of-detail) `extended` | Many more triangles in wide views over large levels. |
| [Runahead](smoothness.md#runahead) | About N extra ticks of CPU work per tick. |
| [Rewind](../playing/saves.md#rewind) | About 3 MiB of memory per second of window, plus what enabled code mods change each second. |
| Window size | GPU work grows with the pixel count. |

To find what limits a slow scene, lower one setting at a time and watch the
readout.

## Engine settings

These take effect at start; the dev overlay shows them read-only.

| Setting | Default | Effect |
| --- | --- | --- |
| `--terrain-cache 0\|1` <!-- id: knob:terrain_cache --> | 1 (on) <!-- id: knob:terrain_cache --> | Draws static level geometry from meshes kept in GPU memory. Needs a hardware GPU; on a software renderer the CPU draws terrain as with 0. The dev overlay row shows whether the cache started. |
| `--extract-threads N` <!-- id: knob:extract_threads --> | 0 (auto: half the logical cores, at most 8) <!-- id: knob:extract_threads --> | Worker threads that prepare terrain for drawing. 1 uses no extra threads. The picture is identical at any count. |
| `--disc-cache 0\|1\|auto` <!-- id: knob:disc_cache --> | auto <!-- id: knob:disc_cache --> | Keeps the disc image in memory, filled in the background from launch in the order the game reads it at boot. A read of a part not yet loaded goes to the file. auto turns it on when the disc image is on a network share, a CD-ROM drive, a FUSE mount or a Windows drive seen from WSL (`/mnt/c`), and off on a local disk; the log file states which and why. 1 always keeps the image in memory, 0 never does. |
