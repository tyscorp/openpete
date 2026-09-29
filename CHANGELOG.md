# Changelog

<!--
Format, read by openpete-website/tools/gen_changelog.py, which renders the
changelog on openpete.com from this file:

  ## vX.Y.Z - YYYY-MM-DD (Status)

  - One item per bullet. A bullet continues onto following lines indented
    by two or more spaces; the lines are joined with one space.

Newest version first. The date and the "(Status)" label are each optional;
the status is shown as a pill ("Unreleased", "Current"). Items take inline
code (`x`), links ([text](url)), emphasis (*x*), and raw inline HTML where
Markdown has none (<kbd>M</kbd>, &times;). A link whose whole text is one
code span renders as <code><a>...</a></code>.
-->

## v0.4.2 - 2026-09-29 (Current)

- Spyro 1: Fixed Sparx, gems and other mobys near Spyro drawing behind him when they are in front of him

## v0.4.1 - 2026-09-22

- Spyro 1: Fixed the HUD eggs swimming between each other with interpolation active
- Spyro 1: Fixed a black line along the bottom of the screen on the pause menu

## v0.4.0 - 2026-09-22

- Fixed subtle flickering with interpolation active
- Added field of view (FOV) option
- Split out wobble and pixelation options
- Reduced VRAM usage
- Fixed the dev menu (default: <kbd>M</kbd>) opening and closing the first time
- Implemented mouse support for the in-game menu, freecam and bindings (<code>mouse:left</code>, <code>mouse:wheel_up</code>, <code>mouse:x</code>)
- Mod SDK: Added [mouse APIs](/docs/modding/reference/api.html#mouse)
- Mod SDK: Added [controller APIs](/docs/modding/reference/api.html#pads)
- Mod SDK: Added [api->disc_path](/docs/modding/reference/api.html#api-disc_path)
- Mod SDK: Dynamically allocate asset arrays to remove fixed capacities
- Mod SDK: Added PSYQ types in the generated struct header with a <code>PSX\_</code> prefix
- Spyro 1: Fixed some near-camera terrain rendering the low-poly geometry in the end credits
- Spyro 1: Fixed the interpolation of text mobys in the end credits
- Spyro 1: Fixed big boi mobys being culled near the edges of the screen in widescreen
- Spyro 1: Fixed the bulls in town square shrinking when changing animation poses with interpolation active
- Spyro 1: Fixed fanning chests' fans rendering pure black at large distances with extended LOD
- Spyro 1: Fixed 2 missing polys on Stone Hill with extended LOD
- Spyro 1: Fixed the green pause framebuffer effect not updating with wobble enabled
- Spyro 1: Fixed moby positioning being slightly off with interpolation active

## v0.3.0 - 2026-09-10

- Fixed a crash when pressing <kbd>F12</kbd> to screenshot
- Spyro 1: Fixed mobys incorrectly appearing at the edges of the screen during dragon cutscenes in widescreen
- Linux: Bundle `libsndio` in the AppImage distribution
- Mod SDK: Full support for ImGui via [`dear_bindings`](https://github.com/dearimgui/dear_bindings)
- Mod SDK: Fixed shader materials rendering corruption (white flashing) caused by a race between the simulation and presentation threads
- Mod SDK: Support dragon, gem, and egg counts in custom levels
- Mods: Fixed installing a `.zip` made by compressing the mod folder itself, where `mod.toml` sits inside a wrapper directory
- Fixed post-processing passes reading the wrong parameters, slightly mis-shading a few pixels per-frame
- Support overriding `gamecontrollerdb.txt` by `--controller-db`, `OPENPETE_CONTROLLER_DB`, `controller_db` in `openpete.toml`, or `$XDG_CONFIG_HOME/openpete/gamecontrollerdb.txt` in the Linux AppImage distribution
- Updated `gamecontrollerdb.txt` to latest
- Support for up to 4 controllers
- Removed SSAA &times;2 from the default configuration

## v0.2.0 - 2026-09-04

- Mod SDK: Initial release
- Linux: Initial release of AppImage distribution
- Improved overall performance by 2-5x, depending on scene and settings
- Significantly improved sim-tick pacing and consistency (99.75% of ticks begin within 250 µs of the target)
- Significantly improved frame pacing and consistency
- Significantly reduced animation error
- Improved texture pack performance
- Improved relative audio volume and stereo mixing
- The game no longer pauses simulation when the window is dragged, resized or fullscreen is toggled
- Improved input handling and responsiveness
- The cursor now hides when inside the game window
- Added a notifications OSD
- Added support for controller vibration
- Spyro 1: Fixed the portal cyclorama not interpolating when entering a portal
- Spyro 1: Fixed Sparx's glow being incorrectly visible in dragon rescue cutscenes
- Various other graphical fixes and improvements
- Spyro 1: Added an easter egg when entering the Crash 3 demo code (instead of just *crash*ing)

## v0.1.4 - 2026-08-17

- Spyro 1: Fixed semi-transparent terrain showing through the ground and through the dragon during rescue cutscenes
- Lowered the CPU requirement to plain x86-64, so pre-2013 processors without AVX2 can run the build
- Fixed in-game menu not saving certain settings

## v0.1.3 - 2026-08-14

- Spyro 1: Fixed a crash when opening a locked chest in Lofty Castle. The same issue caused silent memory corruption in Peace Keepers, Dry Canyon and Tree Tops
- Capped SSAA to 4&times;

## v0.1.2 - 2026-08-13 (Initial release)

- Initial public release
