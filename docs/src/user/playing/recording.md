# Recording inputs and screenshots

## Input recordings (bk2)

OpenPete records and replays pad input as BizHawk `.bk2` movies (the
Octoshock player 1 pad: buttons and both analog sticks). A replay from the
same start produces the same game, [tick](../help/glossary.md#tick) for tick, which makes a movie a
reproducible record of a run.

### Recording

```
openpete --bk2-record
```

`--bk2-record [PATH]` <!-- id: flag:--bk2-record home --> writes the live pad
input to PATH. With no PATH it writes `<timestamp>.bk2` (for example
`20260925-143000.bk2`) in the bk2 folder
([File locations](../help/file-locations.md)). Recording stops when the
program exits.

Beside the movie it writes `<movie>.args`, the launch flags of the recorded
run minus the `--bk2` flags, so a replay starts the same way. Settings that
came from `openpete.toml` are not in it.

A movie holds one input line per pad read, grouped by the game's input
step, which runs once per tick: `Input Groups.txt` in the movie holds the
number of lines of each step, and `Header.txt` carries the line
`OpenpeteFeedKey consume`. A replay feeds each step the lines it recorded, in
order, so a movie does not depend on how the program times VBLs against
ticks. A movie recorded by an earlier version holds one line per VBL (with
the line `OpenpeteFeedKey vbl`) or one per VBL at which the game hands
control back to the program (with no such line); it is converted when it
loads: each VBL replays the line that version read at that point.

Combined with `--bk2 OLD.bk2`, recording replays the old movie and then keeps
recording live input after its end, extending it into the new file.

### Replaying

```
openpete --bk2 movie.bk2
```

`--bk2 PATH` <!-- id: knob:bk2 --> replays the movie into the pad. A
`movie.bk2.args` file beside it is read as one more settings layer, above
`openpete.toml` and environment variables and below the flags you type. At
the movie's end live input takes over;
{{ flag("flag:--bk2-stop-on-end") }} <!-- id: flag:--bk2-stop-on-end --> exits instead.

A movie records the level-load delay it was made under. A replay under a
different delay refuses to start and names the `--load-delay`
<!-- id: knob:load_delay --> value to pass.

!!! warning "What a replay needs"
    The same disc image, the same set of game-changing mods, and a start
    that matches the recording (the `.args` file covers the flags). The free
    camera cannot be turned on while a movie replays or records. Cheats, the
    level warp and the teleport change the game; a movie recorded with them
    replays correctly only with the same actions at the same ticks.

## Screenshots

{{ key("key:host.screenshot") }} <!-- id: key:host.screenshot home --> saves a PNG of the picture on screen
as `openpete-<timestamp>.png` (for example `openpete-20260925-143000.png`,
with `-2`, `-3` and so on appended for more shots in the same second) in the
screenshots folder ([File locations](../help/file-locations.md)).

The image is the native renderer's output: widescreen, mods and the in-game
overlays it draws are in it. The dev overlay panel is not. The dev overlay's
**Screenshot** row has a **Save PNG** button <!-- id: knob:screenshot home -->
that takes the same screenshot.

The log line for the shot ends in `key=N`; `--screenshot-on-frame N` in a
later run of the same start captures that tick
([Ids that contain "frame"](../help/glossary.md#ids-that-contain-frame)).

A capture taken with `--screenshot-on-frame` in a `--headless` run is drawn
again from the game state, not read back from a window, and leaves out the
settings menu, the notifications, the stats readout and the version line.
`--screenshot-overlays` <!-- id: flag:--screenshot-overlays --> (or
`OPENPETE_SCREENSHOT_OVERLAYS=1`) draws them into those captures as the
window shows them. The default is `0` (off).

## Bug reports

A [bug report](../help/glossary.md#bug-report) zip holds the last 30 to 60
seconds of play with the inputs that replay them; see
[Reporting a bug](../help/reporting-a-bug.md#the-bug-report-zip).

## Without the player

`--hide-player 1` <!-- id: knob:hide_player home --> draws the scene
without Spyro: Spyro, his fire breath and his shadow, Sparx and his glow,
and the particles their moves spawn are not drawn. Every other character,
effect and the HUD stay, and the game plays on as before, so a movie
replays the same with the setting on or off. It applies to screenshots, to
video and to the window, with the native renderer only. The default is
`0` <!-- id: knob:hide_player --> (off).

What is hidden differs by game:

| Game | Not drawn | Still drawn |
| --- | --- | --- |
| Spyro the Dragon | Spyro, his fire breath, his shadow, Sparx | the particles of both, Sparx's glow |
| Spyro 2: Ripto's Rage! | the same, and the particles Spyro spawns | Sparx's sparkles and glow |
| Spyro: Year of the Dragon | the same, and the particles Spyro and Sparx spawn, and Sparx's glow | none of theirs |

In Spyro 2 and Spyro 3 a particle spawned by another particle, not by
Spyro or Sparx themselves, stays drawn.

## Video

The released packages are built without video recording. The `--record`
flags in `--help` apply only to builds made from source with video
encoding enabled.
