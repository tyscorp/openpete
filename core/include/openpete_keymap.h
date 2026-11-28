#ifndef OPENPETE_KEYMAP_H
#define OPENPETE_KEYMAP_H

/*
 * The keymap - every key and pad button openpete reads, in one table.
 *
 * Three input surfaces used to be hardcoded in three places: the PSX pad
 * in PsyCross's PsyX_Sys_InitialiseInput, ~30 host hotkeys as raw
 * SDL_SCANCODE_* reads in run.c, and mod [[binding]] rows in mod.toml.
 * They shared no vocabulary and none of them could be rebound. This
 * table is the single registry; `[keys]` in openpete.toml overrides it.
 *
 * Bindings resolve through the same layered scope ladder as the rest of
 * the config (openpete_config.h): defaults < global < [game.<id>] <
 * [disc.<serial>] < [disc.<sha>] < [saved], and exe-dir < user-dir
 * within a tier. Merge is PER ACTION - a scope that names one action
 * leaves every other one alone - and WITHIN an action a scope replaces
 * the whole source list rather than appending, which is what makes
 * `fillpick = "none"` able to unbind something a lower scope bound.
 *
 * This header is SDL-free, and so is keymap.c: the name<->code lookups
 * live behind openpete_keymap_set_lookup so the resolver can be linked
 * into console unit tests (and so a headless run that never brings up
 * SDL's gamepad subsystem still resolves cleanly). keymap_sdl.c
 * installs the real SDL-backed implementation.
 */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- source kinds --------------------------------------------------- */

typedef enum {
    KM_SRC_NONE = 0,
    KM_SRC_KEY,       /* keyboard scancode */
    KM_SRC_PADBTN,    /* gamepad button */
    KM_SRC_PADAXIS,   /* gamepad axis (invert flips the sign) */
    KM_SRC_MOUSEBTN,  /* mouse button (a level) or wheel direction (a pulse) */
    KM_SRC_MOUSEAXIS  /* mouse motion: a displacement in counts, not a level */
} km_srckind_t;

/* KM_SRC_MOUSEBTN codes. The five buttons are levels read from the mask;
 * the two wheel directions are PULSE sources: they have no level, and the
 * poll counts notches into a per-action pulse count instead. The wheel is
 * the one mouse dimension with a +/- split, because its consumers are
 * digital actions; motion has none, because its consumers are signed axes. */
enum {
    KM_MOUSE_LEFT = 0, KM_MOUSE_MIDDLE, KM_MOUSE_RIGHT, KM_MOUSE_X1, KM_MOUSE_X2,
    KM_MOUSE_WHEEL_UP, KM_MOUSE_WHEEL_DOWN,
    KM_MOUSE_BTN_COUNT
};
#define KM_MOUSE_IS_PULSE(code) ((code) >= KM_MOUSE_WHEEL_UP)

/* KM_SRC_MOUSEAXIS codes ("mouse:x", "mouse:-y"). */
enum { KM_MOUSE_X = 0, KM_MOUSE_Y, KM_MOUSE_AXIS_COUNT };

typedef struct {
    unsigned char kind;    /* km_srckind_t */
    signed char   invert;  /* KM_SRC_PADAXIS / KM_SRC_MOUSEAXIS: "pad:-leftx" */
    short         code;    /* SDL scancode / gamepad button / gamepad axis /
                            * KM_MOUSE_* */
    /* Which player's device the source reads, 1-based; 0 means UNQUALIFIED.
     * The distinction matters inside a [keys.padN] block, where an
     * unqualified "pad:" reads that block's own player - so player 2's cross
     * can be written "pad:south" rather than forcing "pad2:" on every row -
     * while an explicit "padM:" still cross-wires deliberately. For host
     * actions, unqualified means player 1, as it always did.
     * Meaningless for KM_SRC_KEY: there is one keyboard. */
    unsigned char device;
    /* "hold:" prefix: the source reads held only after its input has been
     * held without a break for KM_HOLD_MS. Live host reads only; a reader
     * (the pad decode) sees a hold source as not held. */
    unsigned char hold;
} km_source_t;

/* How long a "hold:" source's input is held before the source reads held. */
#define KM_HOLD_MS 500

/* Most actions bind one key plus one pad source; four is slack enough
 * that no real config hits the cap, and it keeps the store flat. */
#define KM_MAX_SOURCES 4

/* ---- domains, kinds, contexts --------------------------------------- */

typedef enum { KM_DOM_HOST = 0, KM_DOM_PAD, KM_DOM_PAD2,
               KM_DOM_PAD3, KM_DOM_PAD4, KM_DOM_COUNT } km_domain_t;

typedef enum { KM_DIGITAL = 0, KM_AXIS } km_kind_t;

/* Which input context owns the binding. The conflict scan only reports a
 * clash between actions whose contexts can be live at the same moment,
 * so the collisions the port has always had - WASD as both the pad's
 * left stick (GAMEPLAY) and the freecam's fly axes (FREECAM), arrows as
 * both the d-pad and menu nav - stay quiet, while a genuine double-bind
 * inside one context gets named. */
typedef enum {
    KM_CTX_ALWAYS = 0,   /* overlaps everything */
    KM_CTX_GAMEPLAY,     /* no overlay open, freecam disengaged */
    KM_CTX_MENU,         /* the settings menu is open */
    KM_CTX_FREECAM,      /* freecam or camlock engaged */
    KM_CTX_FILLPICK,     /* the fill-tag inspector is engaged */
    KM_CTX_DEVUI,        /* dev overlay pad navigation is engaged */
    KM_CTX_COUNT
} km_ctx_t;

/* Registry flags. */
#define KM_DEV      0x1u   /* a development instrument: DEFAULTS TO UNBOUND
                            * so it is never live in a shipped build unless
                            * a config binds it back. */
#define KM_REQUIRED 0x2u   /* losing every source makes something
                            * unreachable; warn (never error) at resolve. */
#define KM_LEVEL    0x4u   /* consumed through openpete_key_down: a pulse
                            * source (mouse wheel) has no level and can never
                            * fire it, so the parser rejects one here. */
#define KM_DELTA    0x8u   /* consumed through openpete_key_delta: the only
                            * actions a mouse motion source may bind to. On
                            * any other action a displacement would bind and
                            * do nothing, so the parser rejects it there.
                            * scripts/fences/keymap_flags.py checks both
                            * flags against their consumer sites. */
#define KM_DEVBUILD 0x10u  /* a KM_DEV row whose default binding is live in a
                            * build that compiles the debug flags in, and is
                            * withheld exactly as a plain KM_DEV row in the
                            * shipped build (OPENPETE_HIDE_DEBUG_FLAGS, set by
                            * the *-launcher distribution presets). The key it
                            * occupies is therefore spent only where the rest
                            * of the debug surface is already present. */

/* ---- the action registry -------------------------------------------- */

/* What a host action does to the knob registry row it drives. A row's
 * value is reached through the row (TOGGLE flips a bool or a two-entry
 * enum, FIRE fires an action, HOLD holds a HOLD row while the action is
 * held, CYCLE_UP/CYCLE_DOWN step an int or enum row by one), or the
 * action is in an exempt class that drives no row: UI_NAV opens, closes
 * or navigates an overlay, FREECAM_INPUT is the engaged debug camera's
 * continuous fly and look input. */
typedef enum {
    KM_OP_TOGGLE = 0,
    KM_OP_FIRE,
    KM_OP_HOLD,
    KM_OP_CYCLE_UP,
    KM_OP_CYCLE_DOWN,
    KM_OP_UI_NAV,
    KM_OP_FREECAM_INPUT,
    KM_OP_COUNT
} km_op_t;

/* Host rows: X(sym, id, kind, ctx, flags, row, op, default_binding, purpose)
 * Pad rows:  X(sym, id, kind, ctx, flags, default_binding, purpose)
 *
 * `row` is the knob registry id of the row the action drives and `op` how
 * (km_op_t); an exempt op carries a NULL row. The registry derives each
 * row's key_action from these two columns, and every consumer site reads
 * a row-driving action through hc_edge, hc_held or hc_count
 * (core/runtime/host_control.h); scripts/fences/host_control_coverage.py
 * refuses a site that reads one directly.
 *
 * `default_binding` is spelled in the TOML vocabulary, not as
 * SDL_SCANCODE_*, so --keys-report prints defaults and overrides in one
 * spelling and so every default is itself parsed at init - a typo fails
 * loudly at boot instead of silently never matching. */

#define OPENPETE_KEYMAP_HOST_ACTIONS(X)                                        \
  /* --- overlays ----------------------------------------------------- */     \
  /* Guide is no PSX input, so the settings menu takes it whole. The dev
   * overlay's pad source is a hold of Back: every other button is a PSX
   * input, and a tap of Back still reaches the game as select. */             \
  X(MENU, "menu", KM_DIGITAL, KM_CTX_ALWAYS, KM_REQUIRED, NULL, KM_OP_UI_NAV,  \
        "Escape,hold:pad:start", "open/close the settings menu")                    \
  X(DEVUI, "devui", KM_DIGITAL, KM_CTX_ALWAYS, KM_LEVEL, NULL, KM_OP_UI_NAV,   \
        "M,hold:pad:back", "open/close the dev config overlay (closes on release)") \
  /* Dev overlay pad navigation, read while the overlay is open and fed to
   * imgui as its gamepad keys. Pad sources only: the keyboard drives the
   * overlay through imgui's own keyboard navigation. */                       \
  X(DEVUI_UP, "devui_up", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:dpup,pad:-lefty",      "dev overlay: move up")                    \
  X(DEVUI_DOWN, "devui_down", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:dpdown,pad:lefty",     "dev overlay: move down")                  \
  X(DEVUI_LEFT, "devui_left", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:dpleft,pad:-leftx",    "dev overlay: move left / decrease")       \
  X(DEVUI_RIGHT, "devui_right", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:dpright,pad:leftx",    "dev overlay: move right / increase")      \
  X(DEVUI_ACTIVATE, "devui_activate", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:south",      "dev overlay: activate or open the focused item")    \
  X(DEVUI_CANCEL, "devui_cancel", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:east",       "dev overlay: cancel, leave a field or popup")       \
  X(DEVUI_MENU, "devui_menu", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:west",       "dev overlay: tweak with the d-pad; hold to switch windows") \
  X(DEVUI_SLOWER, "devui_slower", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:leftshoulder", "dev overlay: slower tweak; previous window")      \
  X(DEVUI_FASTER, "devui_faster", KM_DIGITAL, KM_CTX_DEVUI, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:rightshoulder", "dev overlay: faster tweak; next window")         \
  /* The d-pad AND the left stick drive menu nav, and cross doubles as
   * "increase" - the analog fallbacks matter because the settings menu is
   * reachable on a pad with no keyboard attached. */                          \
  X(MENU_UP, "menu_up", KM_DIGITAL, KM_CTX_MENU, KM_REQUIRED, NULL, KM_OP_UI_NAV, \
        "Up,pad:dpup,pad:-lefty",    "settings menu: previous row")            \
  X(MENU_DOWN, "menu_down", KM_DIGITAL, KM_CTX_MENU, KM_REQUIRED, NULL, KM_OP_UI_NAV, \
        "Down,pad:dpdown,pad:lefty", "settings menu: next row")                \
  X(MENU_LEFT, "menu_left", KM_DIGITAL, KM_CTX_MENU, KM_REQUIRED, NULL, KM_OP_UI_NAV, \
        "Left,pad:dpleft,pad:-leftx", "settings menu: decrease value")         \
  X(MENU_RIGHT, "menu_right", KM_DIGITAL, KM_CTX_MENU, KM_REQUIRED, NULL, KM_OP_UI_NAV, \
        "Right,pad:dpright,pad:leftx,pad:south",                               \
        "settings menu: increase value")                                       \
  X(MENU_CLOSE, "menu_close", KM_DIGITAL, KM_CTX_MENU, KM_LEVEL, NULL, KM_OP_UI_NAV, \
        "pad:start,pad:east", "settings menu: close (on release)")             \
  /* Activates ACTION rows (EXIT GAME), which ignore left/right entirely -
   * cycling a value must never be able to fire a button. Cross doubles as
   * "increase value" on the ordinary rows; the two never contend, because a
   * row is either a value or an action, never both. */                        \
  X(MENU_SELECT, "menu_select", KM_DIGITAL, KM_CTX_MENU, 0, NULL, KM_OP_UI_NAV, \
        "Return,pad:south",   "settings menu: activate the selected action")   \
  /* --- capture ------------------------------------------------------ */     \
  X(SCREENSHOT, "screenshot", KM_DIGITAL, KM_CTX_ALWAYS, 0, "screenshot", KM_OP_FIRE, \
        "F12",            "save a PNG of the current frame")                   \
  X(BUG_REPORT, "bug_report", KM_DIGITAL, KM_CTX_ALWAYS, 0, "bug_report", KM_OP_FIRE, \
        "F9",             "write a bug report zip to send")                    \
  /* --- savestates / rewind / pacing --------------------------------- */     \
  X(STATE_SAVE, "savestate_save", KM_DIGITAL, KM_CTX_GAMEPLAY, 0, "state_save", KM_OP_FIRE, \
        "1",              "quick-save to the active slot")                     \
  X(STATE_LOAD, "savestate_load", KM_DIGITAL, KM_CTX_GAMEPLAY, 0, "state_load", KM_OP_FIRE, \
        "2",              "quick-load from the active slot")                   \
  X(STATE_CYCLE, "savestate_cycle", KM_DIGITAL, KM_CTX_GAMEPLAY, 0, "state_slot", KM_OP_CYCLE_UP, \
        "3",              "cycle the active savestate slot")                   \
  X(REWIND, "rewind", KM_DIGITAL, KM_CTX_GAMEPLAY, KM_LEVEL, "rewind_hold", KM_OP_HOLD, \
        "Backspace",      "hold to rewind (needs --rewind)")                   \
  X(PAUSE, "pause", KM_DIGITAL, KM_CTX_ALWAYS, 0, "pause", KM_OP_TOGGLE,       \
        "F7",             "pause / resume the sim")                            \
  X(FRAME_STEP, "frame_step", KM_DIGITAL, KM_CTX_ALWAYS, KM_LEVEL, "frame_step", KM_OP_FIRE, \
        "F8",             "advance one tick while paused")                     \
  X(TURBO, "turbo", KM_DIGITAL, KM_CTX_ALWAYS, KM_LEVEL, "turbo", KM_OP_HOLD,  \
        "Tab",            "hold to run unlocked (fast-forward)")               \
  X(FULLSCREEN, "fullscreen", KM_DIGITAL, KM_CTX_ALWAYS, 0, "fullscreen", KM_OP_TOGGLE, \
        "none",           "toggle fullscreen (Alt+Enter always works too)")    \
  /* Unbound by default and not KM_REQUIRED: the window close button and
   * the OS still quit, so no binding leaves quitting reachable. */            \
  X(QUIT, "quit", KM_DIGITAL, KM_CTX_ALWAYS, 0, "quit", KM_OP_FIRE,            \
        "none",           "quit the game")                                     \
  /* --- camera -------------------------------------------------------- */    \
  X(FREECAM, "freecam", KM_DIGITAL, KM_CTX_ALWAYS, 0, "freecam", KM_OP_TOGGLE, \
        "F",              "engage/disengage the debug free camera")            \
  X(CAMLOCK, "camlock", KM_DIGITAL, KM_CTX_ALWAYS, KM_DEV, "camlock", KM_OP_TOGGLE, \
        "G",              "lock the presented view while the sim runs")        \
  X(TELEPORT, "teleport", KM_DIGITAL, KM_CTX_ALWAYS, 0, "teleport", KM_OP_FIRE, \
        "T",              "drop Spyro at the free camera")                     \
  X(FLY_FORWARD, "fly_forward", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "W",              "free camera: move forward")                         \
  X(FLY_BACK, "fly_back", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "S",              "free camera: move back")                            \
  X(FLY_LEFT, "fly_left", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "A",              "free camera: strafe left")                          \
  X(FLY_RIGHT, "fly_right", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "D",              "free camera: strafe right")                         \
  X(FLY_UP, "fly_up", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "Space,pad:rightshoulder", "free camera: rise")                        \
  X(FLY_DOWN, "fly_down", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "Left Shift,pad:leftshoulder", "free camera: descend")                 \
  X(LOOK_UP, "look_up", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "Up",             "free camera: pitch up")                             \
  X(LOOK_DOWN, "look_down", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "Down",           "free camera: pitch down")                           \
  X(LOOK_LEFT, "look_left", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "Left",           "free camera: yaw left")                             \
  X(LOOK_RIGHT, "look_right", KM_DIGITAL, KM_CTX_FREECAM, KM_LEVEL, NULL, KM_OP_FREECAM_INPUT, \
        "Right",          "free camera: yaw right")                            \
  X(SPEED_UP, "speed_up", KM_DIGITAL, KM_CTX_FREECAM, 0, "freecam_speed", KM_OP_CYCLE_UP, \
        "],pad:righttrigger", "free camera: faster")                           \
  X(SPEED_DOWN, "speed_down", KM_DIGITAL, KM_CTX_FREECAM, 0, "freecam_speed", KM_OP_CYCLE_DOWN, \
        "[,pad:lefttrigger",  "free camera: slower")                           \
  X(FLY_AXIS_X, "fly_axis_x", KM_AXIS, KM_CTX_FREECAM, 0, NULL, KM_OP_FREECAM_INPUT, \
        "pad:leftx",      "free camera: strafe axis")                          \
  X(FLY_AXIS_Y, "fly_axis_y", KM_AXIS, KM_CTX_FREECAM, 0, NULL, KM_OP_FREECAM_INPUT, \
        "pad:-lefty",     "free camera: forward axis")                         \
  /* The mouse rides the look axes as a displacement source alongside the
   * stick; "mouse:y" in place of "mouse:-y" inverts, "pad:rightx" alone
   * disables mouse look. */                                                   \
  X(LOOK_AXIS_X, "look_axis_x", KM_AXIS, KM_CTX_FREECAM, KM_DELTA, NULL, KM_OP_FREECAM_INPUT, \
        "pad:rightx,mouse:x",   "free camera: yaw axis")                       \
  X(LOOK_AXIS_Y, "look_axis_y", KM_AXIS, KM_CTX_FREECAM, KM_DELTA, NULL, KM_OP_FREECAM_INPUT, \
        "pad:-righty,mouse:-y", "free camera: pitch axis")                     \
  X(INTERP_TOGGLE, "interp_toggle", KM_DIGITAL, KM_CTX_ALWAYS, 0, "interp", KM_OP_TOGGLE, \
        "I",              "toggle frame interpolation")                        \
  X(LOD_TOGGLE, "lod_toggle", KM_DIGITAL, KM_CTX_ALWAYS, 0, "lod", KM_OP_TOGGLE, \
        "L",              "toggle sector LOD original/extended")               \
  /* --- development instruments (KM_DEV: unbound unless configured) --- */    \
  X(FILLPICK, "fillpick", KM_DIGITAL, KM_CTX_ALWAYS, KM_DEV, "fillpick", KM_OP_TOGGLE, \
        "P",              "toggle the LP fill-tag inspector")                  \
  X(FILLPICK_PREVIEW, "fillpick_preview", KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV, "fillpick_preview", KM_OP_TOGGLE, \
        "H",              "fill-tag inspector: toggle preview")                \
  X(FILLPICK_FILL, "fillpick_fill", KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV, "fillpick_fill", KM_OP_FIRE, \
        "Y",              "fill-tag inspector: mark FILL")                     \
  X(FILLPICK_NOFILL, "fillpick_nofill", KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV, "fillpick_nofill", KM_OP_FIRE, \
        "N",              "fill-tag inspector: mark NOFILL")                   \
  X(FILLPICK_NONE, "fillpick_none", KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV, "fillpick_none", KM_OP_FIRE, \
        "B",              "fill-tag inspector: clear the verdict")             \
  X(FILLPICK_UNDO, "fillpick_undo", KM_DIGITAL, KM_CTX_FILLPICK, KM_DEV, "fillpick_undo", KM_OP_FIRE, \
        "U",              "fill-tag inspector: undo")

/* The PSX pad. Defaults reproduce PsyCross's PsyX_Sys_InitialiseInput
 * exactly, so remapping is opt-in and a stock config plays identically.
 * KM_REQUIRED on the face buttons and d-pad: unbinding every source of
 * one of these makes the game unplayable, which is worth a warning even
 * though it is a legitimate thing for a pad-only player to do. */
#define OPENPETE_KEYMAP_PAD_ACTIONS(X)                                         \
  X(CROSS,    "cross",    KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "C,pad:south",        "PSX pad: cross")                                \
  X(CIRCLE,   "circle",   KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "V,pad:east",         "PSX pad: circle")                               \
  X(SQUARE,   "square",   KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "X,pad:west",         "PSX pad: square")                               \
  X(TRIANGLE, "triangle", KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "Z,pad:north",        "PSX pad: triangle")                             \
  X(L1,       "l1",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Left Shift,pad:leftshoulder",   "PSX pad: L1")                        \
  X(L2,       "l2",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Left Ctrl,pad:lefttrigger",     "PSX pad: L2")                        \
  X(L3,       "l3",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "[,pad:leftstick",               "PSX pad: L3")                        \
  X(R1,       "r1",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Right Shift,pad:rightshoulder", "PSX pad: R1")                        \
  X(R2,       "r2",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Right Ctrl,pad:righttrigger",   "PSX pad: R2")                        \
  X(R3,       "r3",       KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "],pad:rightstick",              "PSX pad: R3")                        \
  X(START,    "start",    KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,            \
        "Return,pad:start",   "PSX pad: start")                                \
  X(SELECT,   "select",   KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                      \
        "Space,pad:back",     "PSX pad: select")                               \
  X(DPAD_UP,  "dpad_up",   KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,           \
        "Up,pad:dpup",        "PSX pad: d-pad up")                             \
  X(DPAD_DOWN,"dpad_down", KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,           \
        "Down,pad:dpdown",    "PSX pad: d-pad down")                           \
  X(DPAD_LEFT,"dpad_left", KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,           \
        "Left,pad:dpleft",    "PSX pad: d-pad left")                           \
  X(DPAD_RIGHT,"dpad_right",KM_DIGITAL, KM_CTX_GAMEPLAY, KM_REQUIRED,          \
        "Right,pad:dpright",  "PSX pad: d-pad right")                          \
  X(STICK_UP,   "stick_up",    KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "W",              "PSX pad: left stick up (keyboard)")                 \
  X(STICK_DOWN, "stick_down",  KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "S",              "PSX pad: left stick down (keyboard)")               \
  X(STICK_LEFT, "stick_left",  KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "A",              "PSX pad: left stick left (keyboard)")               \
  X(STICK_RIGHT,"stick_right", KM_DIGITAL, KM_CTX_GAMEPLAY, 0,                 \
        "D",              "PSX pad: left stick right (keyboard)")              \
  X(LEFT_X,   "left_x",   KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:leftx",      "PSX pad: left stick X")                             \
  X(LEFT_Y,   "left_y",   KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:lefty",      "PSX pad: left stick Y")                             \
  X(RIGHT_X,  "right_x",  KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:rightx",     "PSX pad: right stick X")                            \
  X(RIGHT_Y,  "right_y",  KM_AXIS, KM_CTX_GAMEPLAY, 0,                         \
        "pad:righty",     "PSX pad: right stick Y")

/* Action ids. HOST first, then one pad block per player - the enum and the
 * registry are generated from the same lists so they cannot drift. */
#define KM_ENUM_HOST(sym, id, k, c, f, r, o, d, p)  OPK_##sym,
#define KM_ENUM_PAD(sym, id, k, c, f, d, p)   OPK_PAD_##sym,
#define KM_ENUM_PAD2(sym, id, k, c, f, d, p)  OPK_PAD2_##sym,
#define KM_ENUM_PAD3(sym, id, k, c, f, d, p)  OPK_PAD3_##sym,
#define KM_ENUM_PAD4(sym, id, k, c, f, d, p)  OPK_PAD4_##sym,

typedef enum {
    OPENPETE_KEYMAP_HOST_ACTIONS(KM_ENUM_HOST)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD2)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD3)
    OPENPETE_KEYMAP_PAD_ACTIONS(KM_ENUM_PAD4)
    OPK_ACTION_COUNT
} openpete_action_t;

/* First action of one player's pad block. The blocks are generated from one
 * list, so a decoder can walk any player's with the same offsets. */
#define OPK_PAD_BLOCK_FIRST(player) \
    ((player) == 0 ? OPK_PAD_CROSS : (player) == 1 ? OPK_PAD2_CROSS \
   : (player) == 2 ? OPK_PAD3_CROSS : OPK_PAD4_CROSS)

/* ---- name lookup seam ----------------------------------------------- */

/* Turn a token name into a code, or return -1 for "no such name".
 * `kind` is KM_SRC_KEY / KM_SRC_PADBTN / KM_SRC_PADAXIS. */
typedef int (*openpete_keymap_name_to_code_fn)(int kind, const char* name);
/* Reverse, for the report. Returns NULL when the code has no name. */
typedef const char* (*openpete_keymap_code_to_name_fn)(int kind, int code);
/* The label the CURRENT keyboard layout prints for a scancode ("Q" for
 * the scancode SDL calls "A" on AZERTY), or NULL. Report-only garnish. */
typedef const char* (*openpete_keymap_key_label_fn)(int scancode);

/** @brief Install the name lookups the token parser and the report use.
 *
 * A NULL to_code makes every token fail to parse; a NULL to_name prints
 * every source as "?" in the report; a NULL label omits the layout label.
 * @param to_code turns a token name into a code.
 * @param to_name turns a code into a name for the report.
 * @param label gives the layout label of a scancode. */
void openpete_keymap_set_lookup(openpete_keymap_name_to_code_fn to_code,
                                openpete_keymap_code_to_name_fn to_name,
                                openpete_keymap_key_label_fn label);

/* The label a player reads for a pad source on the connected controller's
 * own family ("A" on an Xbox pad, "Cross" on a PlayStation pad, "L1",
 * "Left Stick Up"), or NULL when the source has no label.
 * `kind` is KM_SRC_PADBTN or KM_SRC_PADAXIS; `invert` is the source's sign
 * (-1 for "pad:-lefty"). */
typedef const char* (*openpete_keymap_pad_label_fn)(int kind, int code, int invert);

/** @brief Install the pad-label lookup openpete_keymap_action_label uses.
 *
 * NULL makes a pad source print its config spelling ("pad:south").
 * @param fn the lookup. */
void openpete_keymap_set_pad_label(openpete_keymap_pad_label_fn fn);

/** @brief Return the knob registry row a host action drives.
 *
 * @param a the action.
 * @return the row id, or NULL for an exempt host action, a pad action and
 *         an out-of-range action. */
const char* openpete_keymap_action_row(openpete_action_t a);

/** @brief Return what a host action does to the row it drives.
 *
 * @param a the action.
 * @return the km_op_t, or -1 for a pad action and an out-of-range action. */
int openpete_keymap_action_op(openpete_action_t a);

/** @brief Return the config id of the first host action that drives a knob registry row.
 *
 * The registry's key_action of the row, derived from the row column of
 * OPENPETE_KEYMAP_HOST_ACTIONS.
 * @param row_id the row id ("pause").
 * @return the action id ("pause"), or NULL when no host action drives the row. */
const char* openpete_keymap_row_key_action(const char* row_id);

/** @brief Return the host action a config id names.
 *
 * @param id the action id as `[keys.host]` spells it ("pause").
 * @return the action, or -1 when no host action has that id. */
int openpete_keymap_host_action(const char* id);

/** @brief Write an action's resolved binding in the spelling a player reads.
 *
 * A key prints the current layout's label for it (its positional name when
 * the layout gives none); a pad source prints the pad-label lookup's label
 * (openpete_keymap_set_pad_label), a "PadN " prefix for a source qualified
 * to player N; a mouse source prints "Mouse " and its name. Sources are
 * joined by " / "; an action with no source prints "unbound".
 * @param a the action; out of range prints "unbound".
 * @param out receives the text, always NUL-terminated; truncated to fit.
 * @param cap capacity of out in bytes; at least 1. */
void openpete_keymap_action_label(openpete_action_t a, char* out, size_t cap);

/** @brief Write an action's built-in binding in the config spelling.
 *
 * The registry default ("Escape", "Right,pad:dpright") with its sources
 * joined by ", "; "unbound" for a default of "none" and for a KM_DEV row
 * this build withholds. Reads only the registry, so it is valid before
 * openpete_keymap_init and before any config layer.
 * @param a the action; out of range prints "unbound".
 * @param out receives the text, always NUL-terminated; truncated to fit.
 * @param cap capacity of out in bytes; at least 1. */
void openpete_keymap_action_default_label(openpete_action_t a, char* out, size_t cap);

/* ---- live input seam ------------------------------------------------- */

/* Where the resolved bindings are actually READ. Behind a seam for the
 * same reason as the lookups: it keeps the store, the edge latch and the
 * deadzone logic in this SDL-free TU (so the unit test can drive them
 * with fake input) and leaves keymap_sdl.c as pure glue. All three may
 * be NULL - every query then reads as "not held", which is what a
 * headless run wants. */
typedef struct {
    int (*key_down)(int scancode);     /* 0/1 */
    int (*pad_button)(int button);     /* 0/1 */
    int (*pad_axis)(int axis);         /* [-32768, 32767] */
    /* Slot-qualified reads, for sources carrying a device > 0. Optional: a
     * NULL pair makes every such source read as "not held" rather than
     * silently falling back to player 1's device, which would let a mistyped
     * "pad2:" quietly control player 1. The unqualified pair above still
     * serves device 0, so existing bindings do not change path. */
    int (*pad_button_slot)(int slot, int button);
    int (*pad_axis_slot)(int slot, int axis);
    /* The mouse channel (openpete_host_mouse.h). `mouse_button` is the level
     * of KM_MOUSE_LEFT..X2 from the published snapshot; the totals are the
     * channel's running counts (motion axis KM_MOUSE_X/Y in counts, wheel in
     * 1/256 notch); `mouse_owned` is OPENPETE_MOUSE_OWNED_*. A NULL member
     * reads as released / no motion / unowned. */
    int       (*mouse_button)(int code);
    long long (*mouse_motion_total)(int axis);
    long long (*mouse_wheel_total)(void);
    int       (*mouse_owned)(void);
    /* Monotonic milliseconds, read by "hold:" sources at each poll and by
     * the capture timeout. NULL leaves every hold source not held and the
     * capture without a timeout. */
    unsigned long long (*now_ms)(void);
    /* The pointer position in window points, for the capture prompt's rect
     * test. Returns 0 when no position is known; NULL places the pointer
     * outside every rect. */
    int (*mouse_pos)(float* x, float* y);
} openpete_keymap_input_fns;

/** @brief Install the reads the resolver evaluates bindings with.
 *
 * The struct is copied. A NULL member reads as not held, no motion, or
 * unowned.
 * @param fns the reads, or NULL to clear all of them. */
void openpete_keymap_set_input(const openpete_keymap_input_fns* fns);

/* ---- resolving against a caller's own snapshot ----------------------- */

/* Reads supplied by a caller that already holds the input state it wants the
 * bindings resolved against, rather than letting them reach for live SDL.
 * That is what keeps an extra player's pad decode replayable: it is resolved
 * on the producer side from the sample being published, so the bytes a tick
 * consumes are the bytes the input log records for it. Any member may be
 * NULL, reading as "not held". */
typedef struct {
    int (*key_down)(void* ctx, int scancode);            /* 0/1 */
    int (*pad_button)(void* ctx, int slot, int button);  /* 0/1 */
    int (*pad_axis)(void* ctx, int slot, int axis);      /* [-32768, 32767] */
    void* ctx;
} openpete_keymap_reader_t;

/** @brief Return whether any source bound to an action is held, resolved against a caller's reader.
 *
 * Unlike openpete_key_down this resolves the bindings directly instead of
 * reading the poll's latch, so a caller outside the host-loop poll order
 * gets this instant's answer. With a reader, keys and pad sources read
 * through it and mouse sources read released; consumed keys and the
 * keyboard suspension apply either way.
 * @param a the action; out of range reads 0.
 * @param default_slot pad slot for sources with no "padN:" qualifier, so a
 *        [keys.pad2] row written "pad:south" reads player 2's device.
 * @param rd the reader, or NULL for the installed live reads.
 * @return 1 when held, 0 otherwise. */
int   openpete_keymap_action_down_via(openpete_action_t a, int default_slot,
                                      const openpete_keymap_reader_t* rd);
/** @brief Return an action's analog value in [-1, 1], resolved against a caller's reader.
 *
 * The first source outside the deadzone wins; a held digital source reads
 * 1. Resolution follows openpete_keymap_action_down_via.
 * @param a the action; out of range reads 0.
 * @param default_slot pad slot for sources with no "padN:" qualifier.
 * @param rd the reader, or NULL for the installed live reads.
 * @return the value, 0 when every source is neutral. */
float openpete_keymap_action_axis_via(openpete_action_t a, int default_slot,
                                      const openpete_keymap_reader_t* rd);

/** @brief Return the raw value of an action's first non-neutral source, resolved against a caller's reader.
 *
 * The same first-non-neutral rule as openpete_keymap_action_axis_via
 * without the float conversion, for callers reproducing the game's
 * fixed-point axis conversion. A pad axis inside the deadzone is skipped;
 * a held digital source reads 32767.
 * @param a the action; out of range reads 0.
 * @param default_slot pad slot for sources with no "padN:" qualifier.
 * @param rd the reader, or NULL for the installed live reads.
 * @return the raw value, sign-corrected for an inverted source, or 0 when
 *         every source is neutral. */
int   openpete_keymap_action_axis_raw_via(openpete_action_t a, int default_slot,
                                          const openpete_keymap_reader_t* rd);

/** @brief Return the number of sources bound to an action.
 *
 * @param a the action.
 * @return the source count; 0 for an unbound or out-of-range action. */
int   openpete_keymap_action_bound(openpete_action_t a);

/** @brief Install the SDL-backed name lookups and input reads (keymap_sdl.c) into the keymap.
 *
 * Passes the name-to-code, code-to-name and key-label functions to
 * openpete_keymap_set_lookup and the key, pad, mouse and mouse-owner reads
 * to openpete_keymap_set_input, and the controller-family pad labels to
 * openpete_keymap_set_pad_label. Call before openpete_keymap_init, which
 * parses every registry default through the name lookup. */
void openpete_keymap_install_sdl(void);

/* ---- boot ------------------------------------------------------------ */

/** @brief Seed every action from its registry default.
 *
 * Clears the action store, the consumed-key list and the per-poll wheel
 * and motion counts, then parses each row's default. A KM_DEV row stays
 * unbound unless km_devbuild_bound allows it; an unparseable default is
 * logged and left unbound. Mod bindings are not cleared. Must run after
 * openpete_keymap_set_lookup, or every default fails to parse. */
void openpete_keymap_init(void);

/** @brief Record one config layer's binding for domain.action.
 *
 * The call replaces the action's source list rather than appending to it,
 * so a higher scope can unbind what a lower one bound. A call whose rank is
 * below the stored rank is ignored; an equal rank replaces, so ties go to
 * the later call. The replaced origin is kept as the shadowed label that
 * openpete_keymap_report prints. An unknown domain is logged; an unknown
 * action is logged with the nearest action name of that domain within edit
 * distance 3. A token that fails to parse leaves the stored binding as it
 * was.
 * @param domain "host" or "pad".."pad4" (any case).
 * @param action the action id within the domain.
 * @param tokens the value as written: a single string, or an array's
 *        elements.
 * @param n number of tokens; 0 (an empty array) unbinds like the token
 *        "none".
 * @param rank the caller's packed (tier, file, spelling) precedence;
 *        higher wins.
 * @param origin the display label ("game.spyro-1"); NULL stores "config".
 * @param where the config file named in error messages; NULL reads
 *        "config".
 * @return 0, including for an ignored lower-rank call, or -1 for a NULL
 *         domain or action, an unknown domain or action, or a token that
 *         fails to parse. */
int openpete_keymap_set(const char* domain, const char* action,
                        const char* const* tokens, int n,
                        int rank, const char* origin, const char* where);

/** @brief Store a [keys.mod.<id>] override for a mod's declared [[binding]] name.
 *
 * Stored apart from the action registry because mods are discovered after
 * the config is read, so the name is not validated here. A stored entry of
 * higher rank is kept; an equal or higher rank replaces the entry. Config
 * ranks are above 0, so the override always replaces the mod's own
 * mod.toml key, which registers at rank 0. A token that fails to parse
 * stores nothing.
 * @param mod_id the mod's id; NULL fails.
 * @param name the binding name; NULL fails.
 * @param tokens the value as written.
 * @param n number of tokens; 0 unbinds.
 * @param rank the config scope's packed precedence.
 * @param where the config file named in error messages.
 * @return 0, or -1 on a parse failure, a NULL id or name, or allocation
 *         failure. */
int openpete_keymap_set_mod(const char* mod_id, const char* name,
                            const char* const* tokens, int n,
                            int rank, const char* where);

/** @brief Register a mod's own mod.toml `key` for a binding name at rank 0.
 *
 * Registered under the mod.<id> domain, so `key = "pad:north"` and
 * `key = "mouse:right"` work with no config override. A stored
 * [keys.mod.<id>] row, which ranks above 0, is kept. A key that fails to
 * parse registers unbound, so the mod's binding_down reads 0 as for an
 * unbound override.
 * @param mod_id the mod's id; NULL fails.
 * @param name the declared binding name; NULL fails.
 * @param key the key text from mod.toml.
 * @param where the manifest named in error messages.
 * @return 0, or -1 when the key fails to parse (the name is still
 *         registered, unbound), for a NULL id or name, or on allocation
 *         failure. */
int openpete_keymap_set_mod_default(const char* mod_id, const char* name,
                                    const char* key, const char* where);

/** @brief Record a mod's live enabled state on every binding it registered.
 *
 * A disabled mod's bindings leave the mouse:left scan behind the
 * double-click fullscreen gate; the scan is recomputed on every call that
 * carries a mod id. The bindings stay stored and readable through
 * openpete_keymap_mod_binding.
 * @param mod_id the mod's id; NULL does nothing.
 * @param on nonzero for enabled. */
void openpete_keymap_set_mod_enabled(const char* mod_id, int on);

/** @brief Run the conflict and reachability scans over the stored bindings.
 *
 * Collects every pair of actions whose contexts overlap and that share a
 * source (the same half of a pad axis, any other identical source); a
 * deliberate pair is collected with its reason and not warned about, any
 * other pair is warned about. Warns for a KM_REQUIRED, non-KM_DEV action
 * with no source, for a mouse source on a MENU-context action other than
 * menu_close (the settings menu owns the mouse while open), and for a
 * menu or menu_close binding on a settings-menu gesture (left or right
 * click; the wheel for menu). Recomputes the mouse:left scan. The
 * collected pairs, up to 32, are what openpete_keymap_report prints.
 * @return the number of warnings emitted; 0 is a clean map. The caller
 *         never fails a boot on it. */
int openpete_keymap_resolve(void);

/* ---- queries --------------------------------------------------------- */

/** @brief Sample every binding and advance the edge latches, wheel pulses and mouse deltas.
 *
 * Releases consumed keys that are no longer held, copies the held set to
 * the previous set, and resamples every action. The wheel and motion
 * markers advance every poll whether or not anything is bound; an action
 * whose mouse sources read neutral under the current mouse owner gets no
 * pulses and no delta, so input made under an overlay is dropped rather
 * than delivered late. Call exactly once per host-loop iteration, after
 * SDL events are pumped and before any openpete_key_pressed. */
void openpete_keymap_poll(void);

/** @brief Return whether an action was held at the last openpete_keymap_poll.
 *
 * The level is the sample the poll took, not a live read, so every caller in
 * one host-loop iteration sees the same value.
 * @param a the action; out of range reads 0.
 * @return 1 when held, 0 otherwise. */
int   openpete_key_down(openpete_action_t a);
/** @brief Return whether an action was pressed this poll.
 *
 * Pressed is the held-and-not-previously-held edge or at least one wheel
 * pulse: one press per poll whatever the source mix.
 * @param a the action; out of range reads 0.
 * @return 1 when pressed, 0 otherwise. */
int   openpete_key_pressed(openpete_action_t a);
/** @brief Return whether an action went from released to held at this poll, ignoring wheel pulses.
 *
 * A consumer that steps once per wheel notch combines this with
 * openpete_key_pulses.
 * @param a the action; out of range reads 0.
 * @return 1 on the edge, 0 otherwise. */
int   openpete_key_edge(openpete_action_t a);
/** @brief Return the wheel notches counted into an action this poll.
 *
 * @param a the action; out of range reads 0.
 * @return the notch count. */
int   openpete_key_pulses(openpete_action_t a);
/** @brief Return an action's analog level, read live rather than at the last poll.
 *
 * The first bound source that is active wins. A pad axis inside the
 * deadzone (8000 of 32767) is inactive; outside it the value is the throw
 * scaled to [-1, 1]. A held key, pad button or mouse button source reads 1.
 * Mouse motion sources are ignored (see openpete_key_delta). Player-1
 * sources read the installed seams, and "padN:" sources their slot.
 * @param a the action; out of range reads 0.
 * @return the level in [-1, 1], 0 when no source is active. */
float openpete_key_axis(openpete_action_t a);
/** @brief Return an action's mouse motion since the previous poll.
 *
 * Signed counts summed over the action's mouse:x and mouse:y sources,
 * invert applied, no deadzone and no clamp. Reads 0 while the dev overlay
 * owns the mouse, and while the settings menu owns it for every action but
 * menu and menu_close; the markers still advance, so motion made under an
 * overlay is dropped rather than delivered late.
 * @param a the action; out of range reads 0.
 * @return the motion in mouse counts. */
long long openpete_key_delta(openpete_action_t a);

/** @brief Return whether any action or enabled mod binding carries a mouse:left source.
 *
 * Recomputed by openpete_keymap_resolve, by every mod-binding store, and by
 * openpete_keymap_set_mod_enabled; read on the pump thread by the
 * double-click fullscreen gate, so presses of a mouse:left-bound action do
 * not also toggle fullscreen.
 * @return 1 when mouse:left is bound, 0 otherwise. */
int openpete_keymap_mouse_left_bound(void);

/** @brief Return the mouse totals the last openpete_keymap_poll consumed.
 *
 * Diagnostics: the host-mouse probe logs them across an ownership change,
 * where they sit at the totals because the poll drops input made under an
 * overlay.
 * @param wheel receives the wheel total in 1/OPENPETE_MOUSE_WHEEL_UNIT
 *        notch, or NULL.
 * @param mx receives the x motion total in counts, or NULL.
 * @param my receives the y motion total in counts, or NULL. */
void openpete_keymap_mouse_markers(long long* wheel, long long* mx, long long* my);

/** @brief Suspend or resume every keyboard source while an overlay owns the keyboard.
 *
 * Only keyboard sources go inert; pad and mouse sources keep working, so
 * the game stays playable under a mouse-driven dev overlay.
 * openpete_keymap_scancode_down also reads 0 while suspended. The run loop
 * calls this every iteration with the dev overlay's keyboard want.
 * @param on nonzero to suspend.
 * @param exempt actions whose keyboard sources are read anyway, so the key
 *        that opened an overlay can close it; NULL or n 0 exempts none.
 *        Members out of range are ignored.
 * @param n number of members of exempt. */
void openpete_keymap_suspend_keyboard(int on, const openpete_action_t* exempt, int n);

/** @brief Suspend or resume every live pad source while an overlay navigates with the pad.
 *
 * Pad buttons and pad axes read through the live seams (host actions,
 * openpete_key_axis) read neutral; a caller-supplied reader (the pad
 * decode of every player) is untouched, and keyboard and mouse sources
 * keep working. The run loop calls this every iteration with the dev
 * overlay's pad navigation state.
 * @param on nonzero to suspend.
 * @param exempt actions whose pad sources are read anyway (the overlay's
 *        own toggle and navigation); NULL or n 0 exempts none. Members out
 *        of range are ignored.
 * @param n number of members of exempt. */
void openpete_keymap_suspend_pad(int on, const openpete_action_t* exempt, int n);

/** @brief Return the actions whose live pad sources stay readable while the settings menu is open.
 *
 * The run loop suspends every other live pad source while the menu is
 * open, as it does under the dev overlay's pad navigation, so a hold: pad
 * hotkey sharing a button with menu navigation never fires in the menu:
 * the KM_CTX_MENU actions and the two overlay toggles (menu, devui).
 * @param n receives the number of actions, or NULL.
 * @return the actions, a static list. */
const openpete_action_t* openpete_keymap_menu_pad_exempt(int* n);

/* ---- diagnostics / consumers ---------------------------------------- */

/** @brief Copy an action's stored sources, for consumers that need the binding rather than a live read.
 *
 * The PsyCross pad stamp needs the scancode and gamepad code themselves.
 * @param a the action; out of range returns 0.
 * @param out receives up to cap sources; NULL returns 0.
 * @param cap capacity of out.
 * @return the number of sources copied. */
int openpete_keymap_sources(openpete_action_t a, km_source_t* out, int cap);

/** @brief Return the origin label of the layer that set an action's binding.
 *
 * @param a the action; out of range returns "default".
 * @return the label ("default", "game.spyro-1"). */
const char* openpete_keymap_origin(openpete_action_t a);

/** @brief Print the binding table for --keys-report.
 *
 * One row per action with its binding, the scope that set it and its
 * purpose, grouped by domain; a keyboard source prints the layout's key
 * label beside the scancode name when they differ, and a replaced scope
 * prints as "shadowed". An unbound row prints "-" with the reason (a
 * KM_DEV row withheld by the registry, or the scope that unbound it).
 * Then every stored mod binding, the shared-input pairs that
 * openpete_keymap_resolve collected with the reason or an "unintended"
 * mark, and a rebinding example.
 * @param out the stream; NULL prints nothing. */
void openpete_keymap_report(FILE* out);

/** @brief Return 1 when openpete_keymap_request_report was called, else 0.
 *
 * openpete_config_apply sets the request when argv carries --keys-report.
 * No caller reads it; the Spyro 1 main tests openpete_options_t.keys_report
 * instead.
 * @return 1 when a keymap report was requested, else 0. */
int openpete_keymap_report_requested(void);
/// @brief Record that --keys-report was passed; read back by openpete_keymap_report_requested.
void openpete_keymap_request_report(void);

/** @brief Return the stored source list of a mod's declared binding name.
 *
 * The stored entry is the highest-ranked of the mod's mod.toml `key` and
 * any [keys.mod.<id>] override.
 * @param mod_id the mod's id; NULL returns 0.
 * @param name the declared binding name; NULL returns 0.
 * @param out receives up to cap sources, or NULL.
 * @param cap capacity of out.
 * @param out_n receives the copied count (0 when unbound), or NULL.
 * @return 1 when the name is registered, 0 otherwise. */
int openpete_keymap_mod_binding(const char* mod_id, const char* name,
                                km_source_t* out, int cap, int* out_n);

/** @brief Return whether a keyboard scancode is held, read directly rather than through a binding.
 *
 * Reads 0 while the keyboard is suspended, with no exemption; ignores the
 * consumed-key list. The run loop reads Enter through it for the
 * hardcoded Alt+Enter fullscreen toggle.
 * @param scancode the SDL scancode.
 * @return 1 when held, 0 otherwise or when no key read is installed. */
int openpete_keymap_scancode_down(int scancode);

/** @brief Return whether one source is held, for consumers holding a km_source_t rather than an action.
 *
 * Mod bindings read through it. Resolved as for a host action with no
 * exemptions: a keyboard source reads 0 while the keyboard is suspended
 * and for a consumed key; a mouse button reads 0 while the dev overlay or
 * the settings menu owns the mouse; a mouse axis and a wheel pulse have no
 * level and read 0.
 * @param s the source; NULL reads 0.
 * @return 1 when held, 0 otherwise. */
int openpete_keymap_source_down(const km_source_t* s);

/** @brief Return whether a keyboard scancode is one of an action's sources.
 *
 * A binding query, not a live read, for the pump's event handler, which
 * sees one key event at a time and cannot use the polled latch. Unaffected
 * by the keyboard suspension and the consumed-key list.
 * @param a the action; out of range reads 0.
 * @param scancode the SDL scancode.
 * @return 1 when the scancode is bound to the action, 0 otherwise. */
int openpete_keymap_key_binds(openpete_action_t a, int scancode);

/** @brief Mark a key press as already acted on by the event handler, so polled bindings ignore it until release.
 *
 * The pump's event handler sees each key event and the polled keymap reads
 * the held keyboard state, so declining to forward an event does not stop
 * the poll seeing the key held. The handler calls this for a press it
 * consumes (Escape dismissing the dev overlay or the settings menu); the
 * key then reads released to every polled binding until
 * openpete_keymap_poll sees it physically released, so one press produces
 * one action and the next press is live. Equivalent to
 * openpete_keymap_consume_input(KM_SRC_KEY, scancode); safe from any
 * thread.
 * @param scancode the SDL scancode. */
void openpete_keymap_consume_key(int scancode);

/** @brief Latch one input as consumed until openpete_keymap_poll sees it released.
 *
 * The latch is one bitset per kind: keys (KM_SCAN_KEYS scancodes), player
 * 1's pad buttons (KM_SCAN_PADBTNS), pad axes (KM_SCAN_PADAXES; released
 * once the axis reads under KM_AXIS_PRESS either way) and mouse buttons
 * (KM_MOUSE_LEFT..KM_MOUSE_X2). A consumed input reads released to every
 * live keymap read; the pad decode of the game reads SDL directly and is
 * not affected. Safe from any thread.
 * @param kind KM_SRC_KEY, KM_SRC_PADBTN, KM_SRC_PADAXIS or KM_SRC_MOUSEBTN;
 *        another kind or an out-of-range code is ignored.
 * @param code the scancode, button, axis or KM_MOUSE_* code. */
void openpete_keymap_consume_input(int kind, int code);

/** @brief Latch every physically held key as consumed until openpete_keymap_poll sees it released.
 *
 * The run loop calls this every iteration a dev overlay text field takes
 * input, before openpete_keymap_poll. A key that field swallowed then
 * reads released to every polled binding until it is let go, so the
 * field letting go of the keyboard while the key is still down produces
 * no press: the key that opened the overlay cannot close it that way.
 * Pad, axis and mouse inputs are not touched. */
void openpete_keymap_latch_held_keys(void);

/** @brief Return the display spelling of a mod binding's stored first source.
 *
 * The cross-mod conflict scan compares these, so a clash resolved by a
 * [keys.mod.<id>] override stops warning. The spelling is "mouse:<name>",
 * a key name, or "pad[N]:<name>" with '-' on an inverted axis.
 * @param mod_id the mod's id; must not be NULL.
 * @param name the declared binding name; must not be NULL.
 * @param fallback returned unchanged when the name is not registered.
 * @return a pointer into a static buffer overwritten by the next call,
 *         "(unbound)" for a registered name with no source, or fallback. */
const char* openpete_keymap_mod_effective_key(const char* mod_id,
                                              const char* name,
                                              const char* fallback);

/* ---- binding editor -------------------------------------------------- */

/* The input ranges the capture scans and the consume latch covers: SDL3's
 * scancode, gamepad button and gamepad axis counts. */
#define KM_SCAN_KEYS    512
#define KM_SCAN_PADBTNS 32
#define KM_SCAN_PADAXES 8

/* Past this throw a pad axis reads as pressed (a trigger as a button, a
 * stick direction in the capture and the consume latch). */
#define KM_AXIS_PRESS  16384

/* The capture cancels when no candidate arrives in this many milliseconds
 * of the now_ms seam. */
#define KM_CAPTURE_TIMEOUT_MS 5000

/* Mouse motion past this many counts since the capture armed is a
 * candidate on an axis action. */
#define KM_CAPTURE_MOTION 64

/* Which PsyCross fields a player-1 pad action feeds (stamp_psx_pad_bindings
 * writes them; the binding editor admits only what they hold). */
typedef enum {
    KM_PADSHAPE_NONE = 0,     /* not a player-1 pad action */
    KM_PADSHAPE_KEY_AND_PAD,  /* one key and one gamepad button or axis */
    KM_PADSHAPE_KEY_ONLY,     /* one key: the WASD stick directions */
    KM_PADSHAPE_AXIS_ONLY     /* one gamepad axis: the four stick axes */
} km_pad_shape_t;

/** @brief Return the PsyCross field shape of a player-1 pad action.
 *
 * @param a the action.
 * @return the shape; KM_PADSHAPE_NONE for any action outside the player-1
 *         pad block. */
km_pad_shape_t openpete_keymap_pad_shape(openpete_action_t a);

/* One registry row, for the binding page. */
typedef struct {
    const char* id;        /* "cross", "pause" */
    const char* purpose;   /* the registry's one-line purpose */
    int         domain;    /* KM_DOM_* */
    int         kind;      /* KM_DIGITAL or KM_AXIS */
    int         ctx;       /* KM_CTX_* */
    unsigned    flags;     /* KM_DEV, KM_REQUIRED, KM_LEVEL, KM_DELTA, ... */
} openpete_keymap_action_info_t;

/** @brief Copy an action's registry row.
 *
 * @param a the action.
 * @param out receives the row; NULL fails.
 * @return 1 when a is a valid action, 0 otherwise. */
int openpete_keymap_action_info(openpete_action_t a,
                                openpete_keymap_action_info_t* out);

/** @brief Return the action a config domain and id name.
 *
 * @param domain "host" or "pad".."pad4", any case.
 * @param id the action id.
 * @return the action, or -1 when none matches. */
int openpete_keymap_find_action(const char* domain, const char* id);

/** @brief Return the config spelling of an action's domain.
 *
 * @param a the action; out of range returns "host".
 * @return "host" or "pad".."pad4". */
const char* openpete_keymap_domain_name(openpete_action_t a);

/** @brief Write one source in the config spelling a saved row stores.
 *
 * The spelling openpete_keymap_report prints without the layout label
 * ("hold:pad:south", "mouse:wheel_up", "Q"), so the token parses back
 * through the [keys] parser.
 * @param s the source; NULL writes "".
 * @param out receives the text, always NUL-terminated.
 * @param cap capacity of out in bytes; at least 1. */
void openpete_keymap_source_token(const km_source_t* s, char* out, size_t cap);

/** @brief Write one source in the spelling a player reads, as openpete_keymap_action_label does for one source without the hold suffix.
 *
 * @param s the source; NULL writes "".
 * @param out receives the text, always NUL-terminated.
 * @param cap capacity of out in bytes; at least 1. */
void openpete_keymap_source_label(const km_source_t* s, char* out, size_t cap);

/** @brief List the actions a capture of a source onto an action conflicts with.
 *
 * Action B conflicts when it is not a, is a host or player-1 pad action
 * whose context overlaps a's, holds a source contesting src (kind and
 * code, an axis also its polarity; hold and device ignored), is not a
 * deliberate pair with a, and the two sources are not an intended overlap:
 * a hold: pad source on a host button action against the same plain pad
 * button or trigger on a pad action, whichever side holds which.
 * @param a the action being edited.
 * @param src the candidate source.
 * @param out receives up to cap conflicting actions, or NULL.
 * @param cap capacity of out.
 * @return the number of conflicting actions (may exceed cap). */
int openpete_keymap_conflicts(openpete_action_t a, const km_source_t* src,
                              openpete_action_t* out, int cap);

/** @brief Replace an action's live binding, as a [saved] row would set it.
 *
 * Writes the list at the saved layer's rank with origin "saved", marks the
 * action's has-saved bit, clears its hold timers and reruns
 * openpete_keymap_resolve. Takes the store lock.
 * @param a the action.
 * @param src the new sources.
 * @param n number of sources, 0..KM_MAX_SOURCES.
 * @return 0, or -1 for an invalid action or count. */
int openpete_keymap_set_live(openpete_action_t a, const km_source_t* src, int n);

/** @brief Set the ranks that bound the saved layer and the global layer.
 *
 * openpete_keymap_set records per action the best call ranked below
 * saved_rank (the binding a restore returns to) and below game_rank (the
 * global layer), and whether a call ranked in [game_rank, saved_rank)
 * bound the action (a game or disc scope). A call at saved_rank or above
 * sets the has-saved bit. Until called every call is below both bounds.
 * @param saved_rank the lowest rank of the saved layer.
 * @param game_rank the lowest rank of the game scopes. */
void openpete_keymap_set_layer_bounds(int saved_rank, int game_rank);

/** @brief Return whether an action's live binding comes from the saved layer.
 *
 * Set by a [saved.keys] row at boot and by every edit; cleared by a
 * restore and by openpete_keymap_clear_saved. Takes the store lock.
 * @param a the action; out of range reads 0.
 * @return 1 when saved, 0 otherwise. */
int openpete_keymap_has_saved(openpete_action_t a);

/** @brief Clear an action's has-saved bit after its saved row was removed.
 *
 * @param a the action; out of range is ignored. */
void openpete_keymap_clear_saved(openpete_action_t a);

/** @brief Copy the binding below the saved layer, the one a restore returns to.
 *
 * @param a the action.
 * @param out receives up to cap sources.
 * @param cap capacity of out.
 * @param origin receives the layer's origin label ("default", "global",
 *        "game.spyro-1"), or NULL.
 * @param origin_cap capacity of origin.
 * @return the number of sources copied, or -1 for an invalid action. */
int openpete_keymap_below_saved(openpete_action_t a, km_source_t* out, int cap,
                                char* origin, size_t origin_cap);

/** @brief Copy the global layer's binding of an action, and whether a game or disc scope binds it.
 *
 * The settings writer removes a saved row whose live binding equals the
 * global layer when no game or disc scope binds the action.
 * @param a the action.
 * @param out receives up to cap sources.
 * @param cap capacity of out.
 * @param scoped receives 1 when a game or disc scope binds the action, or
 *        NULL.
 * @return the number of sources copied, or -1 for an invalid action. */
int openpete_keymap_global_layer(openpete_action_t a, km_source_t* out, int cap,
                                 int* scoped);

/* What one binding edit did. The capture, a remove and a restore each
 * produce one; the caller turns it into toasts, log lines, save marks and
 * the pad restamp. */
typedef enum {
    KM_EDIT_NONE = 0,
    KM_EDIT_COMMITTED,   /* a captured source was written */
    KM_EDIT_REFUSED,     /* nothing written; reason says why */
    KM_EDIT_CANCELLED,   /* the capture ended without a candidate */
    KM_EDIT_REMOVED,     /* one source was removed */
    KM_EDIT_RESTORED     /* the binding below the saved layer is live again */
} km_edit_kind_t;

#define KM_EDIT_MAX_CHANGED 8

typedef struct {
    int  kind;            /* km_edit_kind_t */
    int  action;          /* the action edited (A) */
    char reason[192];     /* refusal reason or cancel cause */
    char token[64];       /* the committed source in the config spelling */
    /* Every action the edit wrote: [0] is A, the rest the actions the
     * conflict rule swapped. old_tok/new_tok name the swapped source of a B
     * ("none" when it lost the source). */
    int  n_changed;
    struct { int action; char old_tok[64]; char new_tok[64]; } changed[KM_EDIT_MAX_CHANGED];
    int  pad_changed;     /* a player-1 pad action changed */
} openpete_keymap_edit_t;

/* The capture states, published in the low bits of the capture word. */
typedef enum {
    KM_CAP_IDLE = 0,
    KM_CAP_WAITING,      /* waiting for every input to be released */
    KM_CAP_ARMED,        /* the next press is the candidate */
    KM_CAP_CANDIDATE     /* the candidate is held; its release commits */
} km_cap_state_t;

/* Capture word layout: state in bits 0-3, slot in bits 4-7 (15 = add),
 * hold in bit 8, the action from bit 16. */
#define KM_CAPWORD_STATE(w)  ((int)((w) & 0xFu))
#define KM_CAPWORD_SLOT(w)   ((int)(((w) >> 4) & 0xFu))
#define KM_CAPWORD_HOLD(w)   ((int)(((w) >> 8) & 1u))
#define KM_CAPWORD_ACTION(w) ((int)((w) >> 16))
#define KM_CAPWORD_ADD       15

/** @brief Start capturing a source for one slot of an action.
 *
 * The capture waits for every input to be released, takes the next press
 * as the candidate and commits on its release (a wheel notch or mouse
 * motion commits at once); openpete_keymap_poll advances it. While it runs
 * every action reads neutral. Host loop only.
 * @param a a host or player-1 pad action.
 * @param slot the source index to replace, or -1 to add a source.
 * @param hold nonzero to add a "hold:" source (host button actions only;
 *        a replace keeps the slot's own hold flag).
 * @param refused receives the refusal when the capture cannot start, or
 *        NULL.
 * @return 0 when the capture started, -1 when refused. */
int openpete_keymap_capture_begin(openpete_action_t a, int slot, int hold,
                                  openpete_keymap_edit_t* refused);

/** @brief End a running capture without a commit.
 *
 * Every input still held is latched consumed until released. The result
 * (KM_EDIT_CANCELLED with the cause) is taken through
 * openpete_keymap_capture_take_result. No-op when no capture runs. Host
 * loop only.
 * @param cause the cancel cause for the log ("escape", "timeout"). */
void openpete_keymap_capture_cancel(const char* cause);

/** @brief Take the result of the capture that last ended.
 *
 * @param out receives the result.
 * @return 1 when a result was pending, 0 otherwise. */
int openpete_keymap_capture_take_result(openpete_keymap_edit_t* out);

/** @brief Return the published capture word; safe from any thread.
 *
 * @return the word, decoded by the KM_CAPWORD_* macros; 0 while idle. */
unsigned openpete_keymap_capture_word(void);

/** @brief Return whether a capture runs or the inputs latched at its end are still held.
 *
 * True from openpete_keymap_capture_begin until the capture ends and every
 * input it latched is released. Safe from any thread.
 * @return 1 while active, 0 otherwise. */
int openpete_keymap_capture_active(void);

/** @brief Return whether the capture holds input neutral: openpete_keymap_capture_active, unless OPENPETE_REBIND_MUTATE_NO_CAPTURE_TERM is set.
 *
 * While it reads 1 every action reads neutral to the live reads
 * (openpete_key_down, openpete_key_pressed, pulses, deltas,
 * openpete_key_axis), openpete_keymap_scancode_down and
 * openpete_keymap_source_down read 0, and the run loop holds player 1's pad
 * neutral. The environment variable is the harness's negative control.
 * Safe from any thread.
 * @return 1 while input is held neutral, 0 otherwise. */
int openpete_keymap_capture_neutral(void);

/** @brief Publish the capture prompt's rect in window points; a mouse press inside it is a candidate, outside it cancels.
 *
 * An empty rect (x1 <= x0 or y1 <= y0) places every press outside. Safe
 * from any thread.
 * @param x0 left edge.
 * @param y0 top edge.
 * @param x1 right edge.
 * @param y1 bottom edge. */
void openpete_keymap_capture_set_rect(float x0, float y0, float x1, float y1);

/** @brief Remove one source from an action.
 *
 * Refused when it would leave `menu` or `devui` without a pad source.
 * Host loop only.
 * @param a the action.
 * @param slot the source index.
 * @param out receives the result.
 * @return 0 when removed, -1 when refused. */
int openpete_keymap_remove_source(openpete_action_t a, int slot,
                                  openpete_keymap_edit_t* out);

/** @brief Make the binding below the saved layer live again and clear the has-saved bit.
 *
 * Host loop only.
 * @param a the action.
 * @param out receives the result.
 * @return 0 when restored, -1 for an invalid action. */
int openpete_keymap_restore(openpete_action_t a, openpete_keymap_edit_t* out);

#ifdef __cplusplus
}
#endif

#endif /* OPENPETE_KEYMAP_H */
