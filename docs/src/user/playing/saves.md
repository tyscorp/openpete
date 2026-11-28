# Saves, savestates and rewind

OpenPete keeps two kinds of save. Memory cards hold the game's own saves,
made from the game's menus as on a PlayStation. Savestates are OpenPete's
snapshots of the whole running game, made and loaded with a key at any
moment.

## Memory cards

Two formatted memory cards are inserted at every start, as `card1.mcr` and
`card2.mcr` in the cards folder ([File locations](../help/file-locations.md)).
Every write the game makes goes to the file at once. The files are standard
128 KiB PlayStation card images.

The cards are shared by the three games, as on a console: each game's save
takes blocks on the same `card1.mcr`, and a card holds 15 blocks in all. To
use saves from another emulator, copy its raw 128 KiB card image (`.mcr`,
`.mcd` or `.srm`) into the cards folder as `card1.mcr` or `card2.mcr`
while OpenPete is closed, or name it with `--card1`. A DexDrive `.gme`, VGS
`.mem` or PSP/PS3 `.vmp` image loads read-only
([OP-E-CARD-READONLY](../help/troubleshooting.md#op-e-card-readonly)).

Two OpenPete windows can run on the same card, for example Spyro 1 and
Spyro 2 side by side. Each save first takes in what the other window saved
since, so saves to different save slots both stay on the card; a window
reads the other window's saves only after its own next save. When both
windows save to the same save slot, the second save fails with the game's
own save-failed message and the card keeps the first; saving again from
the second window then succeeds and replaces it. A save also fails when
the other window holds the card for more than a second.

| Setting | Effect |
| --- | --- |
| `--card-dir PATH` <!-- id: knob:card_dir --> | Use another folder for both cards |
| `--card1 PATH` <!-- id: flag:--card1 home -->, `--card2 PATH` <!-- id: flag:--card2 home --> | Use a specific image for port A or port B |
| {{ flag("flag:--no-card") }} <!-- id: flag:--no-card --> | Insert no card; the game cannot save |
| {{ flag("flag:--no-card1") }} <!-- id: flag:--no-card1 --> | Leave port A empty; port B keeps its card |
| {{ flag("flag:--no-card2") }} <!-- id: flag:--no-card2 --> | Leave port B empty; port A keeps its card |
| {{ flag("flag:--card-unformatted") }} <!-- id: flag:--card-unformatted --> | Provision a blank unformatted card, so the game's own format screen runs |

### Mods and memory cards

A mod that changes the game can change what a save means, and a mod can
keep its own data for each save.

OpenPete keeps both in a file beside the card,
`card1.mcr.modsave` for `card1.mcr`. For each save on the card it records
the mods the save was made under, counting only mods that change game
state, and each mod's own data for that save. The card itself is never
changed, so it stays readable by a PlayStation or another emulator. The
file is written at the first save under mods; a session that only loads
writes nothing.

- A backup or copy of the card needs its `.modsave` file beside it, or the
  mods lose their data for every save on it.
- A save changed outside OpenPete (played on a PlayStation or another
  emulator, or edited) is reported to its mods as changed when it is next
  loaded here, and they start from their last data for that playthrough.
- A mod that was turned off or not installed when the game saved keeps its
  data: the save carries it forward until the mod is back.

When a loaded save was made under a different set or version of mods,
OpenPete writes a line to the log and the game loads as usual.
{{ flag("flag:--save-mismatch-toast") }} <!-- id: flag:--save-mismatch-toast --> `on` also shows an on-screen
notification; the config file spelling is `save_mismatch_toast = true`
<!-- id: knob:save_mismatch_toast --> (default `{{ default("knob:save_mismatch_toast") }}`<!-- id: knob:save_mismatch_toast -->).
{{ flag("flag:--adopt-saves") }} <!-- id: flag:--adopt-saves --> records the mods running now as the
mods of every save on the card, at the card's first use in the session.

On Spyro 2 and Spyro 3 a save takes a moment to write; a save
interrupted before it finishes (a savestate loaded during the write, or a
failed card) keeps no mod data, and the mods keep the data of the save
before it. A `card1.mcr.modset` file from an earlier version is no longer
read or written; delete it or leave it.

## Savestates

| Key | Action |
| --- | --- |
| {{ key("key:host.savestate_save") }} <!-- id: key:host.savestate_save home --> | Save to the active slot |
| {{ key("key:host.savestate_load") }} <!-- id: key:host.savestate_load home --> | Load from the active slot |
| {{ key("key:host.savestate_cycle") }} <!-- id: key:host.savestate_cycle home --> | Make the next slot active |

There are 10 slots, numbered 0 to 9; the active slot starts at 0 each run and
the slot key wraps from 9 to 0. Each slot is a file, `slot0.state` to
`slot9.state`, in the game's own folder inside the states folder
(`states/spyro-1/`, [File locations](../help/file-locations.md)). Slot
files saved before the per-game folders existed stay in `states/`; the
first run of a game on this version copies the ones it made into its own
folder ([Files from earlier versions](../help/file-locations.md#files-from-earlier-versions)).
Saving overwrites the slot.
The [dev overlay](dev-overlay.md)'s **Savestate** row does the same from the
panel: its list <!-- id: knob:state_slot --> picks the active slot (the one
the slot key moves), and its **Save** <!-- id: knob:state_save --> and
**Load** <!-- id: knob:state_load --> buttons save to and load from it;
**Load** is disabled while the slot is empty.
Each save, load and slot change raises an [on-screen notification](../graphics/display.md#on-screen-notifications)
naming the slot and the result: **STATE SAVED**, **SAVE FAILED**,
**STATE LOADED**, **LOAD REFUSED** (the reasons are below; the log names
the one that applied), **LOAD: SLOT 2 EMPTY**, or, after the slot key,
**STATE SLOT: 3 EMPTY** or **STATE SLOT: 3 OCCUPIED**. One notification
carries all three keys, so pressing them in quick succession updates it in
place. The log records the same events.

The savestate keys do nothing while the settings menu is open.

A load is refused, and the running game left untouched, when:

- the state was made by another game; the log names both games;
- the state was made by a build whose compiled game differs from this one,
  which includes most upgrades;
- the state was made under a different set of game-changing mods, or the
  same mods in another order (the log says **MODSET MISMATCH (mods)** or
  **MODSET MISMATCH (order)** and prints the mods the state was made with;
  enable the same mods and versions, in the same `enabled.txt` order, and
  enable or reload the same mods during the session);
- the game does not return to the wait point where the state was saved
  within 1800 of its waits (at least 30 seconds), for example because a
  pause menu or a level transition is running; the load waits that long,
  and its notification appears when it is refused.

!!! warning
    A savestate restores the whole machine, memory card data excepted: a
    game save made after the savestate stays on the card when you load the
    state.

## Rewind

Holding {{ key("key:host.rewind") }} <!-- id: key:host.rewind home --> steps the game back one
[tick](../help/glossary.md#tick) at a time. Rewind is off by default <!-- id: knob:rewind -->; turn it
on with `--rewind N` <!-- id: knob:rewind -->, where N is how many seconds
of play to keep, or with the **Rewind window** row of the
[dev overlay](dev-overlay.md) (0 to 600 seconds <!-- id: knob:rewind -->).

- Each second of window costs about 3 MiB of memory. Enabled code mods add
  the memory they hold once, plus about the amount they change each second.
- Rewind stays within the current level: loading a level clears the history.
- Changing the window clears the history.
- The dev overlay's **Rewind ticks** row <!-- id: knob:rewind_hold home -->
  rewinds the number of ticks typed into it, one tick per step as the held
  key does, and counts down to 0; it reads 9000 while the key is held. It
  starts at 0 <!-- id: knob:rewind_hold --> and refuses a value while rewind
  is off.
- A code mod that does not declare how it keeps its own state blocks rewind;
  the log names the mod.
- With rewind off, pressing ++backspace++ writes one line to the log saying
  how to turn it on.
