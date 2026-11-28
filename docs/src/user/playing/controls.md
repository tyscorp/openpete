# Controls

Any gamepad SDL recognises plays the game with its own buttons; no keyboard
is needed. The keyboard stands in for the PlayStation pad and also carries
OpenPete's own keys. Every binding below can be changed; see
[Key bindings](../customizing/key-bindings.md). `--keys-report`
<!-- id: flag:--keys-report --> prints every action, its binding and which
config section set it.

## The PlayStation pad

| PlayStation | Keyboard | Gamepad |
| --- | --- | --- |
| Cross | {{ key("key:pad.cross") }} <!-- id: key:pad.cross home --> | South face button (Xbox A) |
| Circle | {{ key("key:pad.circle") }} <!-- id: key:pad.circle home --> | East face button (Xbox B) |
| Square | {{ key("key:pad.square") }} <!-- id: key:pad.square home --> | West face button (Xbox X) |
| Triangle | {{ key("key:pad.triangle") }} <!-- id: key:pad.triangle home --> | North face button (Xbox Y) |
| D-pad | ++up++ <!-- id: key:pad.dpad_up home --> ++down++ <!-- id: key:pad.dpad_down --> ++left++ <!-- id: key:pad.dpad_left --> ++right++ <!-- id: key:pad.dpad_right --> | D-pad |
| Left stick | ++w++ <!-- id: key:pad.stick_up home --> ++a++ <!-- id: key:pad.stick_left --> ++s++ <!-- id: key:pad.stick_down --> ++d++ <!-- id: key:pad.stick_right --> | Left stick |
| Start | {{ key("key:pad.start") }} <!-- id: key:pad.start home --> | Start |
| Select | {{ key("key:pad.select") }} <!-- id: key:pad.select home --> | Back |
| L1 | ++shift++ (left) <!-- id: key:pad.l1 home --> | Left shoulder |
| L2 | ++ctrl++ (left) <!-- id: key:pad.l2 home --> | Left trigger |
| L3 | {{ key("key:pad.l3") }} <!-- id: key:pad.l3 home --> | Left stick click |
| R1 | ++shift++ (right) <!-- id: key:pad.r1 home --> | Right shoulder |
| R2 | ++ctrl++ (right) <!-- id: key:pad.r2 home --> | Right trigger |
| R3 | {{ key("key:pad.r3") }} <!-- id: key:pad.r3 home --> | Right stick click |

Keyboard keys are positional: on a non-US layout the binding is the key in
the position the US layout names.

Only the first pad has default bindings. Pads 2 to 4 (`[keys.pad2]` to
`[keys.pad4]`) start unbound.

## OpenPete keys

| Action | Key | Page |
| --- | --- | --- |
| Open or close the settings menu | {{ key("key:host.menu") }} <!-- id: key:host.menu --> | [Settings menu](settings-menu.md) |
| Quit | {{ key("key:host.quit") }} <!-- id: key:host.quit home --> | |
| Open or close the dev overlay | {{ key("key:host.devui") }} <!-- id: key:host.devui --> | [Dev overlay](dev-overlay.md) |
| Toggle fullscreen | ++alt+enter++ | [Display](../graphics/display.md#fullscreen) |
| Screenshot | {{ key("key:host.screenshot") }} <!-- id: key:host.screenshot --> | [Recording](recording.md#screenshots) |
| Savestate: save to the active slot | {{ key("key:host.savestate_save") }} <!-- id: key:host.savestate_save --> | [Saves](saves.md#savestates) |
| Savestate: load from the active slot | {{ key("key:host.savestate_load") }} <!-- id: key:host.savestate_load --> | [Saves](saves.md#savestates) |
| Savestate: next slot | {{ key("key:host.savestate_cycle") }} <!-- id: key:host.savestate_cycle --> | [Saves](saves.md#savestates) |
| Rewind (hold) | {{ key("key:host.rewind") }} <!-- id: key:host.rewind --> | [Saves](saves.md#rewind) |
| Pause / resume | {{ key("key:host.pause") }} <!-- id: key:host.pause --> | [Practice tools](practice-tools.md#pause-and-tick-step) |
| Advance one tick while paused | {{ key("key:host.frame_step") }} <!-- id: key:host.frame_step --> | [Practice tools](practice-tools.md#pause-and-tick-step) |
| Fast-forward (hold) | {{ key("key:host.turbo") }} <!-- id: key:host.turbo --> | [Practice tools](practice-tools.md#fast-forward) |
| Free camera on / off | {{ key("key:host.freecam") }} <!-- id: key:host.freecam --> | [Practice tools](practice-tools.md#free-camera) |
| Drop Spyro at the free camera | {{ key("key:host.teleport") }} <!-- id: key:host.teleport --> | [Practice tools](practice-tools.md#teleport) |
| Toggle interpolation | {{ key("key:host.interp_toggle") }} <!-- id: key:host.interp_toggle --> | [Smoothness](../graphics/smoothness.md#interpolation) |
| Toggle LOD original / extended | {{ key("key:host.lod_toggle") }} <!-- id: key:host.lod_toggle --> | [Quality](../graphics/quality.md#level-of-detail) |

On a controller, hold Start for half a second to open the settings menu,
and hold Back (View on Xbox, Select on PlayStation) for half a second to
open the dev overlay. A tap of either still reaches the game as the
PlayStation pad's Start or Select. The other OpenPete actions have no
controller binding by default; [Key bindings](../customizing/key-bindings.md)
adds one, for example `savestate_save = ["1", "hold:pad:r3"]`.

The dev overlay's **Quit** button <!-- id: knob:quit home --> quits the game
as the quit key does.

++alt+enter++ is fixed. The rebindable `fullscreen` action is a second,
single-key route to the same toggle and is unbound by default
<!-- id: key:host.fullscreen home -->.

++esc++ closes an open settings menu or dev overlay whatever it is bound to.
No key quits by default; the window's close button always quits, and
[Key bindings](../customizing/key-bindings.md) binds `quit` to a key.

## Free camera keys

While the free camera is on, these keys fly it. Keyboard and pad inputs add
together.

| Action | Keyboard | Gamepad |
| --- | --- | --- |
| Forward / back | ++w++ <!-- id: key:host.fly_forward home --> ++s++ <!-- id: key:host.fly_back --> | Left stick |
| Strafe left / right | ++a++ <!-- id: key:host.fly_left --> ++d++ <!-- id: key:host.fly_right --> | Left stick |
| Rise | {{ key("key:host.fly_up") }} <!-- id: key:host.fly_up home --> | Right shoulder |
| Descend | ++shift++ (left) <!-- id: key:host.fly_down --> | Left shoulder |
| Look | ++up++ <!-- id: key:host.look_up home --> ++down++ <!-- id: key:host.look_down --> ++left++ <!-- id: key:host.look_left --> ++right++ <!-- id: key:host.look_right -->, mouse | Right stick |
| Faster / slower | ++bracket-right++ <!-- id: key:host.speed_up home --> ++bracket-left++ <!-- id: key:host.speed_down --> | Right trigger / left trigger |

Mouse look is scaled by {{ flag("knob:mouse_sensitivity") }} <!-- id: knob:mouse_sensitivity -->
(default {{ default("knob:mouse_sensitivity") }} <!-- id: knob:mouse_sensitivity -->, one full turn per 8192
mouse counts). Binding `look_axis_y` to `"pad:-righty,mouse:y"` inverts the
mouse's vertical look; see [Key bindings](../customizing/key-bindings.md).

## The mouse

The mouse drives the settings menu, the dev overlay and free camera look.
`--mouse 0` <!-- id: knob:mouse --> ignores every mouse event and leaves the
pointer a normal desktop cursor over the window; default on
<!-- id: knob:mouse -->.
