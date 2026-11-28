# The dev overlay

The dev overlay is a panel titled **openpete config** listing every setting
OpenPete has, grouped by section on its **Settings** tab, with each row's
current value and where it came from, and the installed mods on its **Mods**
tab. It changes live settings without a restart, saves them to the
config file, and carries the cheat buttons and the level warp.

{{ key("key:host.devui") }} <!-- id: key:host.devui home --> opens it, and
closes it when released. On a controller, hold Back (View on Xbox, Select
on PlayStation) for half a second; a tap of Back is still the PlayStation
pad's Select. ++esc++ also closes it. It is drawn by the native renderer in
the game window.

The game keeps running behind the panel. While the panel has focus (it
takes focus each time it opens), the PlayStation pad is released, the
controller moves through the panel, and the keyboard's game and hotkey
bindings and the controller's OpenPete bindings pause. Click the game
outside the panel to give the game the keyboard and controller back; click
the panel to take them again. While the game has focus every controller
button reaches only the game. On a controller alone, close and reopen the
panel to give it focus. Mouse use alone leaves the game playable.

## Controller

| Action | Controller | Does |
| --- | --- | --- |
| `devui_up` `devui_down` `devui_left` `devui_right` | d-pad, left stick<!-- id: key:host.devui_up home --><!-- id: key:host.devui_down home --><!-- id: key:host.devui_left home --><!-- id: key:host.devui_right home --> | move between rows; left and right also change a slider or a value |
| `devui_activate` | South (A, Cross)<!-- id: key:host.devui_activate home --> | press a button, tick a box, open a section or a field |
| `devui_cancel` | East (B, Circle)<!-- id: key:host.devui_cancel home --> | leave a field or a popup |
| `devui_menu` | West (X, Square)<!-- id: key:host.devui_menu home --> | hold with the d-pad to change a value; hold to switch between windows |
| `devui_slower` / `devui_faster` | left / right shoulder<!-- id: key:host.devui_slower home --><!-- id: key:host.devui_faster home --> | change a value in smaller or larger steps; while switching windows, the previous or next window |

These actions are rebindable in `[keys.host]`; see
[Key bindings](../customizing/key-bindings.md).

On the keyboard, the Left and Right arrow keys step a slider such as **Tick
scale** down and up while it is selected.

## Reading the panel

| Part | Meaning |
| --- | --- |
| Search field | Filters the selected tab: on Settings, the rows whose name, flag or environment variable contains the text, with every section holding a match opened; on Mods, the mods whose id or name contains it. The text stays when the tab changes. |
| Tabs | **Settings** holds every section below; **Mods** holds the [mods](#mods). The selected tab lasts until the game closes. |
| Row value | Changes apply at once on a live row. A row that needs a restart is drawn greyed out. |
| Row title tooltip | The setting's description, its flag and variable spellings, the key bound to it (as the current keyboard layout and the connected controller label it) when a key action changes it, "restart to apply" on a restart row, and where the value came from: the command line, the environment or the default, "edited this session" once changed, and the `openpete.toml` section that set it, for example `[saved]`. **pinned** marks a setting that a flag or variable of this launch overrides; saving it takes effect once that override is gone. |
| Button tooltip | A button's status, such as **not in a level**, above its description. |
| Right click on a row title | **Reset to boot value**, **Copy as CLI flag**, **Copy as env line**. |

**Save settings**, beside the search field, writes every row changed this
session into the `[saved]` block of the config file; its tooltip shows the
result of the last save. Changes made in the panel are saved only by this
button, and closing the panel writes nothing; changes made in the settings
menu are saved as they are made. See
[Saving your settings](settings-menu.md#saving-your-settings).

## Sections

The Settings tab holds these sections, in this order. The graphics rows are
described under [Graphics](../graphics/display.md); each Graphics page names
its settings. Sections marked collapsed start closed; click a title to open
it.

| Section | Rows |
| --- | --- |
| Display | Fullscreen, Aspect ratio, Render FPS, [Interpolate](../graphics/smoothness.md#interpolation), Field of view, Supersampling, VSync, Performance stats (level and position), Vertex precision, Raster quantize, Color depth, Texture mapping, LOD, Fog |
| Display, Advanced (collapsed) | [Cadence](../graphics/smoothness.md#cadence), Notifications (on, position, timeout), Fog scale, Render mode, Terrain GPU cache, Sector visibility, Render paths |
| Gameplay | Freecam, [Freecam speed](practice-tools.md#free-camera), [Teleport player](practice-tools.md#teleport), [Savestate](saves.md#savestates), Tick scale, Pause sim, [Turbo](practice-tools.md#fast-forward), Runahead, Rewind, [Rewind ticks](saves.md#rewind), [Screenshot](recording.md#screenshots), [Bug report](../help/reporting-a-bug.md#the-bug-report-zip), [Quit](controls.md#openpete-keys), Level warp, Area warp (Spyro 3) |
| Gameplay, Cheats (collapsed) | one button per cheat, on games that have them |
| Audio (collapsed) | Mute, SFX volume, Music volume |
| Input (collapsed) | Vibration, Pad while unfocused, Mouse sensitivity |
| Controls (collapsed) | every binding of the PlayStation pad in port 1 and of OpenPete's keys; see [below](#controls) |
| Info (collapsed) | the launch-only rows: Skip to level, Skip to cutscene, Skip to sub-area, Freecam pose, Player pose, bk2 replay, bk2 frame offset, bk2 recording, Renderer, Extraction threads, Window size |
| Info, Paths (collapsed) | every path and memory card row |
| Recording | the video capture rows, shown only while recording |
| Debug, Debug diagnostics (collapsed) | diagnostic settings |

Button groups (Vertex precision, Color depth, Texture mapping, LOD, Terrain
visibility) highlight the current choice. **Color depth** **15-bit** is the
dither setting on, **24-bit** off. **Runahead** has a checkbox; buttons for 1,
2 and 3 ticks appear beside it while runahead is on. **Rewind** has a
checkbox; the window in seconds appears beside it while rewind is on. The
same pattern holds
elsewhere: a row's further controls show only while its first control is
on, as Render FPS's target (while **Interpolation** is on), Field of view's
slider (while **Original** is off), Performance
stats' position, Notifications' position and timeout, and the
**SFX volume** and **Music volume** sliders beside their **Override**
checkbox.

**Render paths** <!-- id: knob:render_paths --> draws only the chosen
render paths (sky, Spyro, mobys, terrain, shadows, flame, particles, the 2D
UI and the rest), one checkbox each, for comparing one path in isolation;
**All** returns to normal drawing and **None** draws no path. It changes
only the picture, and the overlay's **Save settings** does not save it.
The default is every path <!-- id: knob:render_paths -->. At launch,
`--render-paths LIST` <!-- id: flag:--render-paths --> takes the same names,
comma-separated: `cyclorama`, `spyro`, `moby`, `terrain`, `shadow`,
`flame`, `particles`, `tracer`, `transition`, `portal`, `glow`, `sparkle`,
`moby_shadow`, `ui`, or `none`.

**Pause sim** <!-- id: knob:pause --> stops the game's ticks while the
picture keeps presenting, as the pause key ({{ key("key:host.pause") }} <!-- id: key:host.pause -->)
does; audio mutes while paused. Checked, it shows a **Step tick** button
<!-- id: knob:frame_step --> that advances the game by one tick, as the step
key ({{ key("key:host.frame_step") }} <!-- id: key:host.frame_step -->) does. It starts unchecked
<!-- id: knob:pause -->.

**Freecam pose** <!-- id: knob:freecam_pose --> engages the free camera
at a pose (`pos=(x, y, z) yaw=N pitch=N`, the format the free camera logs)
once the level is live. An optional `roll=N` after the pose tilts the view
about its forward axis, 4096 per turn; the free camera never logs a roll, and
a pose without one is level. A bare list of numbers (`x,y,z,yaw,pitch,roll`)
gives the roll as its sixth number. **Player pose** <!-- id: knob:player_pose --> places
Spyro at `x,y,z[,yaw]` once gameplay is live, with the camera re-seated
behind him; a trailing `@N` waits until count `N` of the counter
`--frame-limit` reads. Both are empty by default, and both change the game's state.

**Render mode** offers **Textured**, **Untextured**
<!-- id: knob:notex --> and **Wireframe** <!-- id: knob:wireframe -->.
Untextured and Wireframe are inspection views: the first replaces textures
with grey to show the lighting alone, the second draws every triangle's
edges. {{ flag("flag:--notex") }}<!-- id: flag:--notex --> followed by `1`
turns the untextured view on at launch, as the variable `OPENPETE_NOTEX=1`
does; the flag wins over the variable. The view is off by default.

The camera lock <!-- id: knob:camlock --> holds the picture on the current
view, and lets it fly with the free camera keys, while the game keeps running
underneath: cutscenes play on. It changes only the picture, not the game.
Turning it on turns the free camera off. It has no row in the panel; its
key, `camlock`, is unbound unless configured <!-- id: key:host.camlock -->.

## Controls

The **Controls** section edits key and button bindings in place of
`[keys.pad]` and `[keys.host]` lines in [`openpete.toml`](../customizing/key-bindings.md).
It starts collapsed. The fill-tag inspector's keys are not listed; they
are bound in the file (see [Development actions](../customizing/key-bindings.md#development-actions)).

**Player 1** lists the PlayStation pad in port 1; **Hotkeys** lists
OpenPete's own keys, grouped by when they are read. Each row shows the
action, one button per bound input and:

| Control | Does |
| --- | --- |
| an input's button | replace that input with the next one pressed |
| **x** beside it | remove that input |
| **+** | add an input (an action holds up to four) |
| **+ hold** | add an input that acts only after a half-second hold; OpenPete's keys only |
| **Reset** | return to the binding below the saved one (the file's own line, or the default); its tooltip names it |

After a click a prompt asks for the input: a key, a controller button or
trigger, a mouse button pressed inside the prompt, or a wheel notch. A stick
or mouse movement is taken only on a stick or look action, in the positive
direction. ++esc++, a click outside the prompt, or 5 seconds without input
cancels; ++esc++ itself cannot be assigned here (it can in the file). The
input is assigned when it is released.

An input another action already uses moves: the two actions swap, and a
notification names the action that changed. A controller button the
PlayStation pad uses binds one of OpenPete's in-play keys only through
**+ hold**: a tap still reaches the game, and the key acts on a
half-second hold. While the settings menu or this overlay is open, a held
button acts only as their navigation. A change is refused, with the reason
shown, when it would leave the settings menu or this overlay without a
controller button, when the PlayStation pad cannot hold it (one key and one
controller input per button, a key only for `stick_up` ... `stick_right`, a
stick only for `left_x` ... `right_y`), or when the key cannot be written
back to the file (++comma++ and **Keypad ,**).

A change to **Player 1** re-maps every connected controller that has no
`[keys.padN]` lines of its own. Players 2 to 4 and mod keys are edited in
the file. Each change is saved to the `[saved.keys]` block of the config
file half a second later; see [Saved settings](../customizing/config-file.md#saved-settings).

## Cheat buttons

Gameplay's **Cheats** subsection has one row per cheat, **99 lives** and
**Unlock all levels** on Spyro the Dragon, each with an **Apply** button. A
press applies the cheat on the next tick in a level, during play or on the
pause and inventory screens; it can be pressed again, for example to top up
lives. The button's tooltip reads **queued**, **applied**, or **not in a
level**; outside a level the button is disabled. The cheats are the ones
`--cheats` applies at start; see
[Practice tools](practice-tools.md#cheats).

## Level warp

The **Level warp** row moves to any level mid-session. Its list
<!-- id: knob:warp_level --> picks the destination and follows the level you
are in until you pick one; the **Warp** button <!-- id: knob:warp_go -->
starts the warp.

In a level, the warp runs the game's own exit-level sequence and then loads
the chosen level; progress is kept. From the title screen, or from the
menus that open after Press Start, it starts a new session in the chosen
level, with progress and lives reset as with `--skip-to-level`; the
button's tooltip reads **from title or menu (resets progress)**. From a
menu the warp leaves it the way the game does: Spyro the Dragon fades out
as when a save file is picked, Spyro 2: Ripto's Rage! backs out as
++triangle++ does, and Spyro: Year of the Dragon starts the level from the
menu screen. A menu screen that is reading or writing the memory card
holds the warp until the screen waits for input again. During the attract
demo the warp ends the demo, as a button press does, and then starts the
new session from the title screen; the tooltip reads **ends the demo
(resets progress)**. Warping while the free camera is on turns the free
camera off first. The button is disabled when no route exists (**not in a
level**).

### Area warp

On Spyro: Year of the Dragon, the **Area warp** row moves to any
[sub-area](../games/spyro-3.md#sub-areas). Its list
<!-- id: knob:warp_area --> holds one entry per sub-area, such as
**Sunny Villa - Area 1**, and follows the sub-area you are in until you pick
one; the **Warp** button <!-- id: knob:warp_area_go --> starts the warp, and
progress is kept. From another level the warp first loads the chosen level,
as the level warp does, then enters the area the way its portal does. From
another sub-area of the same level it passes through the main area, as the
level's portals do. From the title screen it starts a new session in the
chosen sub-area, as `--skip-to-area` does, with progress and lives reset.
Warping while the free camera is on turns the free camera off first. The
button is disabled in the chosen area (**already here**) and outside gameplay
(during a speech, a load or a menu). The
main area of a level is reached with the level warp.

## Mods

The Mods tab lists each installed mod with a checkbox that enables or
disables it at the next tick. Its first line counts the loaded and enabled
mods; the **Open folder** button at the line's right end opens the mods
folder in the file manager, creating it when it is missing. With no mod
installed the tab shows only that line and button. A mod that registers its own controls shows
them under its name. A mod that adds levels lists each with a **Warp**
button <!-- id: knob:custom_level_go home --> and a serving list
<!-- id: knob:custom_level_mode home -->. **Warp** makes that level the
custom-level destination, named `<mod id>/<level name>`
<!-- id: knob:custom_level home -->, and loads it on the next tick; it is
disabled while the level's files are missing or its mod is disabled. The
serving list picks **own slot** (the level time-shares the free level slot)
or **replace donor** (the level serves over its donor level's own entry) on
the next tick; the default is the level's `replace` setting in its mod's
`mod.toml` <!-- id: knob:custom_level_mode -->. The destination starts at the
first custom level in name order <!-- id: knob:custom_level -->. See
[Mods](../customizing/mods.md).

## The I and L keys

Two more keys toggle renderer settings.

| Key | Action | Notification |
| --- | --- | --- |
| {{ key("key:host.interp_toggle") }} <!-- id: key:host.interp_toggle --> | Toggle [interpolation](../graphics/smoothness.md#interpolation) on and off | **INTERPOLATION: ON** / **OFF** |
| {{ key("key:host.lod_toggle") }} <!-- id: key:host.lod_toggle --> | Toggle [LOD](../graphics/quality.md#level-of-detail) between original and extended | **LOD: ORIGINAL** / **EXTENDED** |

Interpolation off presents each tick's picture unchanged between ticks,
which shows what interpolation adds.
