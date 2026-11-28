/*
 * M-key dev config overlay - ImGui panel over the knob registry.
 *
 * One window, sections as collapsing headers laid out by a table that
 * regroups and combines the registry's rows, a search filter, per-row source
 * tag (cli/env/default + edited + the winning openpete.toml layer), tooltips
 * carrying each knob's help text and flag spellings, a Save bar that writes
 * the session's edits into the config file's managed [saved] block, and a
 * right-click context menu (reset to boot / copy as CLI flag / copy as env
 * line - the copy items move a setting to ANOTHER machine, or across the
 * WSL->Windows boundary env vars don't cross).
 *
 * Rendering: records into the offscreen pass (off_rp) at the live-present
 * compose point, after the SSAA resolve - so the panel is always at 1x window
 * resolution and never reaches screenshot composes (the caller gates
 * on s_overlay_live). The Vulkan backend rides volk's loaded pointers
 * (IMGUI_IMPL_VULKAN_USE_VOLK); its pipeline is built against off_rp once -
 * swapchain rebuilds recreate off_rp with identical formats, which keeps the
 * pipeline valid (render-pass compatibility), so no re-init on resize/vsync.
 */

#include "imgui.h"
/* The error bracket around a mod UI section body uses the recovery API and
 * the context's error callback, both internal. */
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>

extern "C" {
#include "../rhi/rhi.h"          /* no C++ guard of its own */
}
#include "openpete.h"
#include "openpete_atomic_file.h"
#include "openpete_devui.h"
#include "openpete_fiber.h"      /* openpete_frame, for the open and close log lines */
#include "openpete_fillpick.h"
extern "C" {
#include "openpete_host_input.h"   /* C-only header: the mouse snapshot read */
}
#include "openpete_host_mouse.h"
#include "openpete_keymap.h"
#include "openpete_knobs.h"
#include "openpete_config.h"
#include "openpete_log.h"
#include "openpete_menu.h"
#include "openpete_mods.h"
#include "openpete_mod_ui.h"
#include "openpete_paths.h"
#include "openpete_settings_save.h"
#include "openpete_rebind.h"

#include <atomic>
#include <deque>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* The imgui context and its two backends exist (openpete_devui_create). */
static bool        s_ctx    = false;
/* The overlay is attached to the session's options (openpete_devui_init). */
static bool        s_inited = false;
static bool        s_open   = false;
/* The "reveal", held until the panel has drawn it: the panel window
 * uncollapsed, the Mods tab selected when any mod is loaded, and every mod
 * node open. Set only when --devui-open opened the
 * overlay, because a run nobody is driving has no way to click a tree node
 * open, and a mod's UI section body runs only inside its node. ImGui
 * does not persist node state, so this is the only way an unattended
 * windowed run reaches a body. */
static bool        s_boot_reveal = false;
static rhi_device *s_dev    = nullptr;
static SDL_Window *s_win    = nullptr;
static char        s_ini_path[1024];
/* --devui-inject-mouse X,Y: a cursor position posted into imgui's IO before
 * every NewFrame, so an unattended run can park the pointer over a widget
 * and the hover state can be measured. Off unless both coordinates parse. */
static bool        s_inject_mouse = false;
static float       s_inject_x = 0.0f, s_inject_y = 0.0f;

int openpete_devui_open_requested(const struct openpete_options *opts)
{
    if (opts && opts->devui_open >= 0) return opts->devui_open ? 1 : 0;
    const char *e = getenv("OPENPETE_DEVUI_OPEN");
    return (e && *e && *e != '0') ? 1 : 0;
}

int openpete_devui_inject_mouse_requested(float *x, float *y)
{
    /* Harness-only knob (env var, no CLI spelling): automation parks the
     * cursor so an unattended run can hover a widget. */
    const char *v = getenv("OPENPETE_DEVUI_INJECT_MOUSE");
    if (!v || !*v) return 0;
    char *end = nullptr;
    float px = strtof(v, &end);
    if (!end || *end != ',') return 0;
    char *end2 = nullptr;
    float py = strtof(end + 1, &end2);
    if (!end2 || end2 == end + 1) return 0;
    if (x) *x = px;
    if (y) *y = py;
    return 1;
}

/** @brief Format the overlay's ini path: openpete-devui.ini in the state directory, made when missing.
 *
 * The state directory is openpete_paths_state_file's: beside the executable
 * in the portable layout, the per-user state directory otherwise, and
 * <root>/state/openpete under a sandbox. A read-only install (an AppImage
 * mount, Program Files) therefore never holds the file. When the path does
 * not resolve it is empty and no ini is loaded or written.
 * @param out Receives the path.
 * @param cap Capacity of @p out. */
static void devui_ini_path(char *out, size_t cap)
{
    if (openpete_paths_state_file("openpete-devui.ini", out, cap, 1) != 0 && cap)
        out[0] = '\0';
}

/** @brief Name the overlay ini an earlier layout left beside the executable.
 *
 * openpete-devui.ini in the executable's directory when it is a file, else
 * the most recently written openpete-devui-<game id>.ini there (the one-ini
 * per game layout), else nothing.
 * @param base The executable's directory, with its trailing separator.
 * @param out Receives the path.
 * @param cap Capacity of @p out.
 * @return 1 when @p out names a file, 0 when there is none. */
static int devui_legacy_ini(const char *base, char *out, size_t cap)
{
    SDL_PathInfo info;
    snprintf(out, cap, "%sopenpete-devui.ini", base);
    if (SDL_GetPathInfo(out, &info) && info.type == SDL_PATHTYPE_FILE) return 1;
    out[0] = '\0';
    int count = 0;
    char **names = SDL_GlobDirectory(base, "openpete-devui-*.ini", 0, &count);
    SDL_Time newest_time = 0;
    for (int i = 0; names && i < count; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s%s", base, names[i]);
        if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE) continue;
        if (out[0] && info.modify_time <= newest_time) continue;
        snprintf(out, cap, "%s", path);
        newest_time = info.modify_time;
    }
    SDL_free(names);
    return out[0] != '\0';
}

int openpete_devui_seed_ini(void)
{
    static bool s_done = false;
    if (s_done) return 0;
    s_done = true;
    if (openpete_paths_sandbox()) return 0;  /* a sandbox never reads the install's files */
    char target[1024];
    devui_ini_path(target, sizeof target);
    SDL_PathInfo info;
    if (!target[0] || SDL_GetPathInfo(target, &info)) return 0;
    const char *base = SDL_GetBasePath();
    char legacy[1024];
    if (!base || !devui_legacy_ini(base, legacy, sizeof legacy)) return 0;
    if (op_atomic_copy_file(legacy, target) != 0) return -1;
    OPL_INFO_F("devui", "copied %s, the overlay ini of an earlier layout, to %s; "
               "the original stays in place", legacy, target);
    return 1;
}

/* Overlay ini saves that failed this session; the first one logs. */
static SDL_AtomicInt s_ini_save_failures;

/** @brief Write imgui's window state to the overlay's ini path when imgui asks for a save, or unconditionally when forced.
 *
 * io.IniFilename is NULL, so imgui never writes the file itself: it raises
 * io.WantSaveIniSettings and this replaces the file atomically (synced) from
 * SaveIniSettingsToMemory. The request is consumed whether or not the write
 * succeeds: a failed write leaves the previous file whole and is retried at
 * imgui's next request (the next layout change) and at shutdown, not on
 * every present. Only the session's first failure logs a warning.
 * @param force Save even without a pending request once settings are
 *        loaded (at shutdown, where imgui itself would have saved). */
static void devui_save_ini(bool force)
{
    ImGuiIO &io = ImGui::GetIO();
    if (!s_ini_path[0]) return;   /* no context: no ini is loaded or written */
    if (force ? !ImGui::GetCurrentContext()->SettingsLoaded : !io.WantSaveIniSettings) return;
    io.WantSaveIniSettings = false;
    size_t n = 0;
    const char *text = ImGui::SaveIniSettingsToMemory(&n);
    if (op_atomic_write_file_ex(s_ini_path, text, n, OP_ATOMIC_QUIET) == 0) return;
    const int err = errno;
    if (SDL_AddAtomicInt(&s_ini_save_failures, 1) != 0) return;
    OPL_WARN_F("devui", "cannot save the overlay layout to %s: %s; later failures this "
               "session are not logged", s_ini_path, err ? strerror(err) : "write refused");
}

/** @brief Seed and load the overlay ini once per context.
 *
 * UI state (window position, collapse) lives in one file in the state
 * directory (devui_ini_path), not the working directory, shared by every
 * game the executable runs and by the launcher. */
static void devui_load_ini(void)
{
    if (s_ini_path[0]) return;
    (void)openpete_devui_seed_ini();
    devui_ini_path(s_ini_path, sizeof s_ini_path);
    ImGui::LoadIniSettingsFromDisk(s_ini_path);
}

int openpete_devui_create(struct rhi_device *dev, struct SDL_Window *win)
{
    if (s_ctx) return dev == s_dev ? 1 : 0;
    if (!dev || !win) return 0;

    rhi_imgui_info info;
    if (!rhi_get_imgui_info(dev, &info)) return 0;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    /* Gamepad navigation reads the KM_CTX_DEVUI actions the host loop
     * publishes (devui_feed_padnav), never the backend's own pad reads. */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;   /* saved by devui_save_ini */
    devui_load_ini();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha = 0.96f;

    if (!ImGui_ImplSDL3_InitForVulkan(win)) {
        ImGui::DestroyContext();
        return 0;
    }
    /* The backend's automatic gamepad modes call SDL_OpenGamepad and read the
     * pad from the thread that renders the frame, while the pump thread owns
     * every gamepad handle. Manual mode with an empty list opens and reads
     * none; imgui then sees no gamepad. */
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    /* The backend's IME hook calls SDL_StartTextInput from the thread that
     * renders the frame, which is not the thread that owns the window;
     * openpete_devui_pump_text_input does it on the pump thread instead. The
     * backend's per-NewFrame IME update then sees no request and makes no
     * SDL call that changes state. */
    ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;

    ImGui_ImplVulkan_InitInfo ii = {};
    ii.ApiVersion         = info.api_version;
    ii.Instance           = (VkInstance)info.instance;
    ii.PhysicalDevice     = (VkPhysicalDevice)info.physical_device;
    ii.Device             = (VkDevice)info.device;
    ii.QueueFamily        = info.queue_family;
    ii.Queue              = (VkQueue)info.queue;
    ii.PipelineCache      = (VkPipelineCache)info.pipeline_cache;
    ii.DescriptorPool     = VK_NULL_HANDLE;
    ii.DescriptorPoolSize = 8;   /* backend creates + owns its own pool */
    ii.PipelineInfoMain.RenderPass  = (VkRenderPass)info.render_pass;
    ii.PipelineInfoMain.Subpass     = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.MinImageCount      = info.image_count >= 2 ? info.image_count : 2;
    ii.ImageCount         = info.image_count >= 2 ? info.image_count : 2;
    if (!ImGui_ImplVulkan_Init(&ii)) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return 0;
    }

    s_dev    = dev;
    s_win    = win;
    s_ctx    = true;
    return 1;
}

int openpete_devui_init(struct rhi_device *dev, struct SDL_Window *win,
                        const struct openpete_options *opts)
{
    if (s_inited) return 1;
    if (!openpete_devui_create(dev, win)) return 0;

    knob_registry_init(opts);
    s_inject_mouse =
        openpete_devui_inject_mouse_requested(&s_inject_x, &s_inject_y)
            != 0;
    s_inited = true;
    s_open   = openpete_devui_open_requested(opts) != 0;
    s_boot_reveal = s_open;
    char key[160];
    openpete_keymap_action_label(OPK_DEVUI, key, sizeof key);
    OPL_INFO_F("devui", "dev config overlay ready (%s toggles)%s", key,
               s_open ? " - open at boot (--devui-open)" : "");
    const ImGuiIO &io = ImGui::GetIO();
    const ImFont *font = io.FontDefault ? io.FontDefault
                         : io.Fonts->Fonts.Size ? io.Fonts->Fonts[0] : nullptr;
    OPL_INFO_F("devui", "overlay font: %s", font ? font->GetDebugName() : "imgui's default (none added yet)");
    return 1;
}

void openpete_devui_shutdown(void)
{
    if (!s_ctx) return;
    devui_save_ini(true);
    s_ini_path[0] = '\0';
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    s_ctx    = false;
    s_inited = false;
    s_open   = false;
    s_boot_reveal = false;
    s_dev    = nullptr;
    s_win    = nullptr;
}

int  openpete_devui_available(void) { return s_inited; }
int  openpete_devui_is_open(void)   { return s_inited && s_open; }
void openpete_devui_close(void)     { s_open = false; }

/* A scroll-to-top request the next panel frame consumes
 * (openpete_devui_open_top): set on the host loop, taken on the frame
 * thread. */
static SDL_AtomicInt s_top_req;

void openpete_devui_open_top(void)
{
    if (!s_inited) return;
    SDL_SetAtomicInt(&s_top_req, 1);
    s_open = true;
    OPL_INFO_F("devui", "%s", "open at top");
}

int openpete_devui_toggle(void)
{
    if (!s_inited) return 0;
    s_open = !s_open;
    return s_open;
}

/* Event queue: the pump and the imgui frame do not necessarily run on the
 * same thread, and imgui is single-threaded - ProcessEvent mutates the same
 * IO state NewFrame consumes. So the pump only *queues* here, and the events
 * are fed to imgui on the thread that records its frame.
 *
 * A text-input event's `text` points into SDL's temporary memory, which SDL
 * frees at the start of the next SDL_PollEvent, before the frame thread
 * drains the ring. The text is copied into the slot's own buffer and the
 * event repointed at it; text longer than a slot spans consecutive slots,
 * split on UTF-8 character boundaries.
 *
 * Single-producer/single-consumer ring. On overflow the newest event is
 * dropped rather than the oldest: losing the tail of a burst degrades an
 * overlay interaction, while dropping the oldest can strip a press and leave
 * imgui holding a key down forever. Drops are counted and reported, because a
 * silently lossy input queue is a miserable thing to debug. */
#define DEVUI_EVQ_CAP 512
#define DEVUI_EVQ_TEXT 64
static SDL_Event  s_evq[DEVUI_EVQ_CAP];
static char       s_evq_text[DEVUI_EVQ_CAP][DEVUI_EVQ_TEXT];
static SDL_AtomicInt s_evq_head;   /* producer writes */
static SDL_AtomicInt s_evq_tail;   /* consumer writes */
static unsigned   s_evq_dropped;

/** @brief Return whether imgui's SDL3 backend reads events of this type.
 *
 * Mirrors the cases of ImGui_ImplSDL3_ProcessEvent: mouse motion, wheel and
 * buttons, text input, keys, the window's mouse enter and leave and focus
 * changes, and gamepad arrival and removal. Every other type (joystick
 * axes and buttons, sensor and UPDATE_COMPLETE reports, display and window
 * geometry) would only occupy a slot the backend skips.
 * @param type The SDL event type.
 * @return true when the backend handles the type. */
static bool devui_event_feeds_imgui(Uint32 type)
{
    switch (type) {
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        return true;
    default:
        return false;
    }
}

/** @brief Append one event to the ring, or count it dropped when the ring is full.
 *
 * A text-input event's text is copied into the slot's buffer, at most
 * DEVUI_EVQ_TEXT - 1 bytes cut on a character boundary.
 * @param ev The event; for text input, `text` is the chunk to store.
 * @return Bytes of text stored for a text-input event, 1 for any other
 *         stored event, 0 when dropped. */
static size_t devui_enqueue(const SDL_Event *ev)
{
    int head = SDL_GetAtomicInt(&s_evq_head);
    int tail = SDL_GetAtomicInt(&s_evq_tail);
    int next = (head + 1) % DEVUI_EVQ_CAP;
    if (next == tail) {
        if ((s_evq_dropped++ % 256u) == 0u)
            OPL_WARN_F("devui", "event queue full; dropped %u", s_evq_dropped);
        return 0;
    }
    size_t stored = 1;
    s_evq[head] = *ev;
    if (ev->type == SDL_EVENT_TEXT_INPUT) {
        stored = SDL_utf8strlcpy(s_evq_text[head], ev->text.text,
                                 DEVUI_EVQ_TEXT);
        s_evq[head].text.text = s_evq_text[head];
    }
    SDL_MemoryBarrierRelease();          /* event visible before the index */
    SDL_SetAtomicInt(&s_evq_head, next);
    return stored;
}

void openpete_devui_process_event(const union SDL_Event *ev)
{
    if (!s_ctx || !ev) return;
    const SDL_Event *e = (const SDL_Event *)ev;
    if (!devui_event_feeds_imgui(e->type)) return;
    if (e->type != SDL_EVENT_TEXT_INPUT) {
        (void)devui_enqueue(e);
        return;
    }
    if (!e->text.text) return;
    SDL_Event chunk = *e;
    const char *rest = e->text.text;
    while (*rest) {
        chunk.text.text = rest;
        const size_t n = devui_enqueue(&chunk);
        if (n == 0) return;
        rest += n;
    }
}

/* ------------------------------------------------------------------ */
/* bug-report note prompt                                             */
/* ------------------------------------------------------------------ */

/* The prompt's lifecycle (OPENPETE_DEVUI_NOTE_*): the requester moves IDLE
 * to REQUESTED, the frame thread REQUESTED to SHOWN on its first draw and
 * SHOWN to SAVED or SKIPPED on Enter or Escape; openpete_devui_note_end
 * returns it to IDLE from any state. s_note_text belongs to the frame
 * thread until the state reads SAVED, then to the poller. */
static SDL_AtomicInt s_note_state;
static char s_note_text[OPENPETE_DEVUI_NOTE_MAX];
/* The pump thread's latch of the text-input facts SDL reports only there:
 * a keyboard attached, on-screen keyboard support, and whether the
 * on-screen keyboard is shown on the overlay's window. */
static SDL_AtomicInt s_have_kb, s_have_osk, s_osk_shown;
/* A pad answer to the prompt, published by the host loop on the release
 * of the overlay's activate (save) or cancel (skip) action: 1 save, 2 skip. */
static SDL_AtomicInt s_note_pad;

/** @brief Report whether the note prompt is requested or on screen.
 * @return true while the prompt owns the keyboard. */
static bool note_active(void)
{
    const int st = SDL_GetAtomicInt(&s_note_state);
    return st == OPENPETE_DEVUI_NOTE_REQUESTED || st == OPENPETE_DEVUI_NOTE_SHOWN;
}

/** @brief Report whether the overlay or the note prompt owns the keyboard and text input.
 * @return true while either is up. */
static bool input_owner(void)
{
    return s_open || note_active();
}

int openpete_devui_note_begin(void)
{
    if (!s_inited || !s_win) return 0;
    if (!SDL_GetAtomicInt(&s_have_kb) && !SDL_GetAtomicInt(&s_have_osk)) return 0;
    s_note_text[0] = '\0';
    SDL_SetAtomicInt(&s_note_pad, 0);
    SDL_SetAtomicInt(&s_note_state, OPENPETE_DEVUI_NOTE_REQUESTED);
    return 1;
}

int openpete_devui_note_poll(char *out, size_t cap, int *keyboard, int *osk_shown)
{
    const int st = SDL_GetAtomicInt(&s_note_state);
    if (keyboard) *keyboard = SDL_GetAtomicInt(&s_have_kb);
    if (osk_shown) *osk_shown = SDL_GetAtomicInt(&s_osk_shown);
    if (st == OPENPETE_DEVUI_NOTE_SAVED && out && cap) snprintf(out, cap, "%s", s_note_text);
    return st;
}

void openpete_devui_note_end(void)
{
    SDL_SetAtomicInt(&s_note_state, OPENPETE_DEVUI_NOTE_IDLE);
}

int openpete_devui_note_active(void)
{
    return note_active() ? 1 : 0;
}

/** @brief Draw the note prompt on the frame thread and move its state on Enter, Escape or a pad answer.
 *
 * A centred window with one text field that takes keyboard focus on the
 * prompt's first frame. Enter saves the field's text, Escape skips; a pad
 * answer (note_pad_poll) saves or skips. */
static void draw_note_prompt(void)
{
    static bool s_focus = false;
    if (SDL_CompareAndSwapAtomicInt(&s_note_state, OPENPETE_DEVUI_NOTE_REQUESTED,
                                    OPENPETE_DEVUI_NOTE_SHOWN))
        s_focus = true;
    if (SDL_GetAtomicInt(&s_note_state) != OPENPETE_DEVUI_NOTE_SHOWN) return;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                   vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x * 0.6f, 0.0f), ImGuiCond_Always);
    ImGui::Begin("Bug report note", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("What went wrong? Enter to save, Esc to skip.");
    if (s_focus) { ImGui::SetKeyboardFocusHere(); s_focus = false; }
    ImGui::SetNextItemWidth(-FLT_MIN);
    // host-control: exempt: the tester note text
    const bool enter = ImGui::InputText("##note", s_note_text, sizeof s_note_text,
                                        ImGuiInputTextFlags_EnterReturnsTrue);
    const int pad = SDL_SetAtomicInt(&s_note_pad, 0);
    if (enter || pad == 1)
        SDL_SetAtomicInt(&s_note_state, OPENPETE_DEVUI_NOTE_SAVED);
    else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || pad == 2)
        SDL_SetAtomicInt(&s_note_state, OPENPETE_DEVUI_NOTE_SKIPPED);
    ImGui::End();
}

void openpete_devui_pump_text_input(void)
{
    static bool s_text_active = false;
    if (!s_inited || !s_win) return;
    SDL_SetAtomicInt(&s_have_kb, SDL_HasKeyboard() ? 1 : 0);
    SDL_SetAtomicInt(&s_have_osk, SDL_HasScreenKeyboardSupport() ? 1 : 0);
    SDL_SetAtomicInt(&s_osk_shown, SDL_ScreenKeyboardShown(s_win) ? 1 : 0);
    const bool want = openpete_devui_wants_text_input() != 0;
    if (want == s_text_active) return;
    if (want) SDL_StartTextInput(s_win);
    else      SDL_StopTextInput(s_win);
    s_text_active = want;
}

/* Frame-thread open state: the open flag at the last openpete_devui_render,
 * and a focus request for the panel's next Begin. */
static bool s_frame_prev_open = false;
static bool s_focus_panel     = false;

/** @brief Drop the keyboard state imgui kept from before the overlay opened, and request focus for the panel.
 *
 * imgui runs no frame while the overlay is closed, so it still holds the
 * keys down at the close, the item active at the close (a text field
 * left by Escape) and that field's text-input want. They are released and
 * cleared here, on the frame thread before the open's first drain, so the
 * first frame starts from no key held and no field active. The panel then
 * takes navigation focus on every open, as on the first one, which is how a
 * controller reaches it after a click gave the game focus. */
static void devui_reset_on_open(void)
{
    ImGui::GetIO().ClearInputKeys();
    ImGui::ClearActiveID();
    ImGui::GetCurrentContext()->WantTextInputNextFrame = -1;
    s_focus_panel = true;
}

/** @brief Feed every queued SDL event to imgui's SDL3 backend and advance the ring's tail.
 *
 * Runs on the thread that records the imgui frame, immediately before it.
 * While openpete_keymap_capture_active reads 1 the press, wheel, text and
 * axis events are dropped and the releases fed. Key and text events are
 * also dropped when no imgui frame follows: imgui consumes its input queue
 * only in NewFrame, so a key event queued while the overlay is closed
 * would be replayed into the panel when it next opens.
 * @param frame_follows an imgui frame is recorded after this drain. */
static void devui_drain_events(bool frame_follows)
{
    int tail = SDL_GetAtomicInt(&s_evq_tail);
    int head = SDL_GetAtomicInt(&s_evq_head);
    SDL_MemoryBarrierAcquire();
    /* While a binding capture runs (and its latched inputs are held) the
     * captured input must not also drive imgui: presses, wheel, text and
     * stick motion are dropped. Releases pass, so a key or button imgui
     * saw go down before the capture is not left held. */
    const bool capturing = openpete_keymap_capture_active() != 0;
    while (tail != head) {
        const Uint32 t = s_evq[tail].type;
        const bool key = t == SDL_EVENT_KEY_DOWN || t == SDL_EVENT_KEY_UP ||
                         t == SDL_EVENT_TEXT_INPUT;
        const bool drop = (!frame_follows && key) || (capturing &&
            (t == SDL_EVENT_KEY_DOWN || t == SDL_EVENT_TEXT_INPUT ||
             t == SDL_EVENT_MOUSE_BUTTON_DOWN || t == SDL_EVENT_MOUSE_WHEEL ||
             t == SDL_EVENT_GAMEPAD_BUTTON_DOWN || t == SDL_EVENT_GAMEPAD_AXIS_MOTION));
        if (!drop) ImGui_ImplSDL3_ProcessEvent(&s_evq[tail]);
        tail = (tail + 1) % DEVUI_EVQ_CAP;
    }
    SDL_SetAtomicInt(&s_evq_tail, tail);
}

/* imgui's IO capture flags are published rather than read live: they belong to
 * the imgui frame's thread, but the pump needs them too (to decide whether a
 * double-click is the overlay's or the window's). One frame stale by
 * construction, which is what a capture flag is anyway - it describes the
 * frame that was drawn. */
static SDL_AtomicInt s_wants_kb;
static SDL_AtomicInt s_wants_text;
static SDL_AtomicInt s_wants_mouse;
/* Open with an imgui window holding navigation focus (io.NavActive). */
static SDL_AtomicInt s_nav_engaged;

/** @brief Publish imgui's keyboard, text-input and mouse capture wants for the pump's threads.
 *
 * Called after ImGui::Render. The keyboard and text wants are 0 while the
 * panel is closed; the mouse want also counts while the fill-tag inspector
 * is active. Also publishes pad navigation engagement: open with
 * io.NavActive, which holds while an imgui window has navigation focus. */
static void devui_publish_io_wants(void)
{
    const ImGuiIO &io = ImGui::GetIO();
    SDL_SetAtomicInt(&s_wants_kb,
                     (input_owner() && (io.WantCaptureKeyboard || io.WantTextInput)) ? 1 : 0);
    /* Text input is published on its own because the two wants call for
     * different gating: a focused field must swallow every key, while bare
     * nav focus -- which keyboard navigation sets for the whole panel -- must
     * still leave the key that closes the panel readable. */
    SDL_SetAtomicInt(&s_wants_text, (input_owner() && io.WantTextInput) ? 1 : 0);
    /* The fill-tag inspector draws its interactive panel with the overlay
     * closed, so its hover counts: a click aimed at its buttons must not
     * also reach the game or the settings menu under it. */
    const bool mouse_frame = s_open || openpete_fillpick_active() != 0;
    SDL_SetAtomicInt(&s_wants_mouse, (mouse_frame && io.WantCaptureMouse) ? 1 : 0);
    SDL_SetAtomicInt(&s_nav_engaged, (s_open && io.NavActive) ? 1 : 0);
}

/** @brief Publish all three imgui capture wants (keyboard, text input, mouse) and pad navigation engagement as 0.
 *
 * Called on each early return of openpete_devui_render that skips
 * devui_publish_io_wants, so no want from an earlier drawn present stays
 * set: a stale mouse want would keep the overlay as the mouse's owner until
 * the next present that draws it. */
static void devui_publish_none(void)
{
    SDL_SetAtomicInt(&s_wants_kb, 0);
    SDL_SetAtomicInt(&s_wants_text, 0);
    SDL_SetAtomicInt(&s_wants_mouse, 0);
    SDL_SetAtomicInt(&s_nav_engaged, 0);
}

int openpete_devui_wants_keyboard(void)
{
    if (!s_inited || !input_owner()) return 0;
    return SDL_GetAtomicInt(&s_wants_kb);
}

int openpete_devui_wants_text_input(void)
{
    if (!s_inited || !input_owner()) return 0;
    return SDL_GetAtomicInt(&s_wants_text);
}

int openpete_devui_wants_mouse(void)
{
    if (!s_inited) return 0;
    if (!s_open && !openpete_fillpick_active()) return 0;
    return SDL_GetAtomicInt(&s_wants_mouse);
}

int openpete_devui_padnav_engaged(void)
{
    if (!s_inited || !s_open) return 0;
    return SDL_GetAtomicInt(&s_nav_engaged);
}

/* ------------------------------------------------------------------ */
/* pad navigation                                                     */
/* ------------------------------------------------------------------ */

/* The host loop owns the pad (the keymap reads it there, through the input
 * lock and every remap); the imgui frame owns imgui. The host loop publishes
 * the levels of the KM_CTX_DEVUI actions as one word, and the frame turns
 * each change into an imgui gamepad key event before its NewFrame. */
enum {
    PADNAV_UP = 1u << 0, PADNAV_DOWN = 1u << 1, PADNAV_LEFT = 1u << 2,
    PADNAV_RIGHT = 1u << 3, PADNAV_ACTIVATE = 1u << 4, PADNAV_CANCEL = 1u << 5,
    PADNAV_MENU = 1u << 6, PADNAV_SLOWER = 1u << 7, PADNAV_FASTER = 1u << 8,
    PADNAV_HAVE_PAD = 1u << 15,   /* player 1's pad is connected */
};

static const struct { unsigned bit; ImGuiKey key; } kPadnavKeys[] = {
    { PADNAV_UP,       ImGuiKey_GamepadDpadUp },
    { PADNAV_DOWN,     ImGuiKey_GamepadDpadDown },
    { PADNAV_LEFT,     ImGuiKey_GamepadDpadLeft },
    { PADNAV_RIGHT,    ImGuiKey_GamepadDpadRight },
    { PADNAV_ACTIVATE, ImGuiKey_GamepadFaceDown },
    { PADNAV_CANCEL,   ImGuiKey_GamepadFaceRight },
    { PADNAV_MENU,     ImGuiKey_GamepadFaceLeft },
    { PADNAV_SLOWER,   ImGuiKey_GamepadL1 },
    { PADNAV_FASTER,   ImGuiKey_GamepadR1 },
};

static SDL_AtomicInt s_padnav_bits;        /* host loop -> frame */
static unsigned      s_padnav_fed;         /* frame: the bits imgui holds */
static ImGuiID       s_padnav_prev_nav_id; /* frame: NavId after the last NewFrame */
/* Frame-side engagement: imgui's navigation moved to another item, or
 * activated one, on a frame whose navigation input source is the gamepad. */
static SDL_AtomicInt s_imgui_moves;
static SDL_AtomicInt s_imgui_activations;
/* Frame-side: gamepad key presses fed to imgui while no overlay window held
 * navigation focus. */
static SDL_AtomicInt s_imgui_unengaged_presses;

/** @brief Turn the host loop's published pad navigation levels into imgui gamepad key events.
 *
 * Runs on the imgui frame thread before ImGui_ImplSDL3_NewFrame. Only a
 * level that changed since the last fed frame is queued, and a closed
 * overlay feeds all-released, so imgui never holds a gamepad key the pad
 * no longer holds.
 * @param io the overlay's imgui IO. */
static void devui_feed_padnav(ImGuiIO &io)
{
    const unsigned bits = s_open ? (unsigned)SDL_GetAtomicInt(&s_padnav_bits) : 0u;
    const unsigned diff = bits ^ s_padnav_fed;
    const bool engaged = SDL_GetAtomicInt(&s_nav_engaged) != 0;
    for (const auto &m : kPadnavKeys) {
        if (!(diff & m.bit)) continue;
        io.AddKeyEvent(m.key, (bits & m.bit) != 0);
        if (!engaged && (bits & m.bit)) SDL_AddAtomicInt(&s_imgui_unengaged_presses, 1);
    }
    s_padnav_fed = bits;
}

/** @brief Count imgui's gamepad-sourced navigation moves and activations of the frame just begun.
 *
 * Runs after ImGui::NewFrame, where imgui's navigation update has applied
 * this frame's input. */
static void devui_count_padnav(void)
{
    const ImGuiContext &g = *ImGui::GetCurrentContext();
    if (g.NavInputSource == ImGuiInputSource_Gamepad) {
        if (g.NavId != s_padnav_prev_nav_id && g.NavId != 0)
            SDL_AddAtomicInt(&s_imgui_moves, 1);
        if (g.NavActivatePressedId != 0)
            SDL_AddAtomicInt(&s_imgui_activations, 1);
    }
    s_padnav_prev_nav_id = g.NavId;
}

/* Host-loop state of the toggle and the per-open counters. */
static bool               s_host_prev_open;
static bool               s_close_armed;
static unsigned           s_open_count;
static unsigned long long s_open_engaged, s_open_moves, s_open_activations;
static int                s_open_imgui_moves0, s_open_imgui_activations0;
static int                s_open_imgui_unengaged0;

/** @brief Log the close of the current open with its pad navigation counts. */
static void devui_log_close(void)
{
    OPL_INFO_F("devui",
               "pad nav #%u closed at frame %u: engaged_iterations=%llu "
               "moves=%llu activations=%llu imgui_moves=%d imgui_activations=%d "
               "imgui_unengaged_presses=%d",
               s_open_count, openpete_frame, s_open_engaged, s_open_moves,
               s_open_activations,
               SDL_GetAtomicInt(&s_imgui_moves) - s_open_imgui_moves0,
               SDL_GetAtomicInt(&s_imgui_activations) - s_open_imgui_activations0,
               SDL_GetAtomicInt(&s_imgui_unengaged_presses) - s_open_imgui_unengaged0);
}

/** @brief Apply this iteration's `devui` edges: a press opens a closed overlay, a release after a press closes an open one. */
static void devui_host_toggle(void)
{
    if (!s_open) {
        s_close_armed = false;
        if (openpete_key_pressed(OPK_DEVUI)) {
            s_open = true;
            if (openpete_menu_is_open())
                openpete_menu_toggle();   /* close the settings menu underneath */
        }
        return;
    }
    if (openpete_key_pressed(OPK_DEVUI)) {
        s_close_armed = true;
    } else if (s_close_armed && !openpete_key_down(OPK_DEVUI)) {
        s_close_armed = false;
        s_open = false;
    }
}

/** @brief Read the KM_CTX_DEVUI levels into a snapshot word, and player 1's pad presence. */
static unsigned devui_padnav_levels(void)
{
    unsigned bits = 0;
    if (openpete_key_down(OPK_DEVUI_UP))       bits |= PADNAV_UP;
    if (openpete_key_down(OPK_DEVUI_DOWN))     bits |= PADNAV_DOWN;
    if (openpete_key_down(OPK_DEVUI_LEFT))     bits |= PADNAV_LEFT;
    if (openpete_key_down(OPK_DEVUI_RIGHT))    bits |= PADNAV_RIGHT;
    if (openpete_key_down(OPK_DEVUI_ACTIVATE)) bits |= PADNAV_ACTIVATE;
    if (openpete_key_down(OPK_DEVUI_CANCEL))   bits |= PADNAV_CANCEL;
    if (openpete_key_down(OPK_DEVUI_MENU))     bits |= PADNAV_MENU;
    if (openpete_key_down(OPK_DEVUI_SLOWER))   bits |= PADNAV_SLOWER;
    if (openpete_key_down(OPK_DEVUI_FASTER))   bits |= PADNAV_FASTER;
    if (openpete_host_input_pad_connected(0))  bits |= PADNAV_HAVE_PAD;
    return bits;
}

/** @brief Log each change of the toggle's inputs and the overlay's keyboard and pad ownership under OPENPETE_DEVUI_TOGGLE_TRACE=1.
 *
 * One line per host-loop iteration whose word differs from the last: the
 * `devui` action's level and press edge, the open flag, the armed close,
 * and the published text-input, keyboard and pad navigation wants, which
 * decide whether the action's keyboard source is read. */
static void devui_toggle_trace(void)
{
    static int en = -1;
    if (en < 0) {
        const char *e = getenv("OPENPETE_DEVUI_TOGGLE_TRACE");
        en = (e && *e == '1') ? 1 : 0;
    }
    if (!en) return;
    static int last = -1;
    const int v = (openpete_key_down(OPK_DEVUI) ? 1 : 0) |
                  (openpete_key_pressed(OPK_DEVUI) ? 2 : 0) |
                  (s_open ? 4 : 0) | (s_close_armed ? 8 : 0) |
                  (openpete_devui_wants_text_input() ? 16 : 0) |
                  (openpete_devui_wants_keyboard() ? 32 : 0) |
                  (openpete_devui_padnav_engaged() ? 64 : 0);
    if (v == last) return;
    last = v;
    OPL_INFO_F("devui",
               "toggle trace frame=%u ms=%llu held=%d pressed=%d open=%d armed=%d "
               "wtext=%d wkb=%d nav=%d",
               openpete_frame, (unsigned long long)SDL_GetTicks(), v & 1, (v >> 1) & 1,
               (v >> 2) & 1, (v >> 3) & 1, (v >> 4) & 1, (v >> 5) & 1, (v >> 6) & 1);
}

/* Host loop: navigation buttons held when engagement last began, withheld
 * from imgui until each is released. */
static unsigned s_padnav_withheld;

/** @brief Reduce the pad navigation levels to what imgui may see: none while no overlay window holds navigation focus.
 *
 * While unengaged the game owns the pad, so imgui gets no gamepad key: West
 * (Square), the game's charge, would otherwise start imgui's window
 * switching and the d-pad resize the panel. The frame feeds releases for
 * the keys imgui holds when the bits drop (devui_feed_padnav). A button
 * held when engagement begins is withheld until released, so a held
 * Square does not reach imgui as a press on the click that focuses the
 * panel. PADNAV_HAVE_PAD passes either way.
 * @param levels the levels from devui_padnav_levels.
 * @param engaged openpete_devui_padnav_engaged this iteration.
 * @return the levels to publish. */
static unsigned devui_padnav_gate(unsigned levels, bool engaged)
{
    const unsigned nav = levels & ~(unsigned)PADNAV_HAVE_PAD;
    if (!engaged) {
        s_padnav_withheld = nav;
        return levels & PADNAV_HAVE_PAD;
    }
    s_padnav_withheld &= nav;
    return levels & ~s_padnav_withheld;
}

/** @brief Turn the overlay's activate and cancel actions into the note prompt's pad answer, on the host loop.
 *
 * A press of either arms it and the armed action's release answers
 * (activate saves, cancel skips), so the button is up when the prompt lets
 * the pad go and the sim resumes. The actions stay live while the prompt
 * suspends the pad (devui_suspend_exempt holds every KM_CTX_DEVUI action). */
static void note_pad_poll(void)
{
    static openpete_action_t s_armed = OPK_ACTION_COUNT;
    if (!note_active()) { s_armed = OPK_ACTION_COUNT; return; }
    if (openpete_key_pressed(OPK_DEVUI_ACTIVATE)) s_armed = OPK_DEVUI_ACTIVATE;
    else if (openpete_key_pressed(OPK_DEVUI_CANCEL)) s_armed = OPK_DEVUI_CANCEL;
    if (s_armed == OPK_ACTION_COUNT || openpete_key_down(s_armed)) return;   // host-control: exempt: devui_activate/devui_cancel nav
    SDL_SetAtomicInt(&s_note_pad, s_armed == OPK_DEVUI_ACTIVATE ? 1 : 2);
    s_armed = OPK_ACTION_COUNT;
}

void openpete_devui_host_poll(void)
{
    if (!s_inited) return;
    note_pad_poll();
    devui_host_toggle();
    devui_toggle_trace();

    if (s_open && !s_host_prev_open) {
        s_open_count++;
        s_open_engaged = s_open_moves = s_open_activations = 0;
        s_open_imgui_moves0 = SDL_GetAtomicInt(&s_imgui_moves);
        s_open_imgui_activations0 = SDL_GetAtomicInt(&s_imgui_activations);
        s_open_imgui_unengaged0 = SDL_GetAtomicInt(&s_imgui_unengaged_presses);
        OPL_INFO_F("devui", "open #%u at frame %u", s_open_count, openpete_frame);
    } else if (!s_open && s_host_prev_open) {
        devui_log_close();
    }
    s_host_prev_open = s_open;

    const bool engaged = openpete_devui_padnav_engaged() != 0;
    SDL_SetAtomicInt(&s_padnav_bits,
                     s_open ? (int)devui_padnav_gate(devui_padnav_levels(), engaged) : 0);
    if (!engaged) return;
    s_open_engaged++;
    if (openpete_key_pressed(OPK_DEVUI_UP) || openpete_key_pressed(OPK_DEVUI_DOWN) ||
        openpete_key_pressed(OPK_DEVUI_LEFT) || openpete_key_pressed(OPK_DEVUI_RIGHT))
        s_open_moves++;
    if (openpete_key_pressed(OPK_DEVUI_ACTIVATE)) s_open_activations++;
}

/* ------------------------------------------------------------------ */
/* panel                                                              */
/* ------------------------------------------------------------------ */

/** @brief Name the source of a knob's boot value for the row title's tooltip.
 *
 * @param s The source recorded at boot.
 * @return "command line" for KNOB_SRC_CLI, "environment" for KNOB_SRC_ENV,
 *         "default" for every other value. */
static const char *src_word(knob_src_t s)
{
    switch (s) {
    case KNOB_SRC_CLI: return "command line";
    case KNOB_SRC_ENV: return "environment";
    default:           return "default";
    }
}

/** @brief Format a knob's current value as its CLI or environment spelling, for the clipboard copy.
 *
 * A bool reads 0 or 1; every other type uses knob_value_str.
 * @param k The knob.
 * @param buf Receives the text.
 * @param cap The capacity of buf. */
static void format_value_token(const knob_desc_t *k, char *buf, size_t cap)
{
    if (k->type == KNOB_BOOL) {
        double v = k->get_num ? k->get_num(k) : k->stored_num;
        snprintf(buf, cap, "%d", v != 0.0 ? 1 : 0);
    } else {
        knob_value_str(k, buf, cap);
    }
}

/** @brief Report whether a knob's current value differs from its boot value.
 *
 * String and action rows never count as edited. */
static bool row_edited(const knob_desc_t *k)
{
    if (k->type == KNOB_STRING) return false;   /* strings are read-only v1 */
    if (k->type == KNOB_ACTION) return false;   /* fires; holds no value */
    /* A row without a setter cannot be edited here; its getter may still
     * move after the boot snapshot (load_delay resolves to the game's own
     * default once the disc is identified), which is not an edit. */
    if (!k->set_num) return false;
    double v = k->get_num ? k->get_num(k) : k->stored_num;
    return v != k->boot_num;
}

/** @brief Draw a knob row's right-click menu: reset to boot value and clipboard copies.
 *
 * Reset appears only for a live, settable, non-action knob and calls its
 * setter with the boot value. The copy items appear when the knob has a CLI
 * spelling or an environment variable name, and put "<flag> <value>" or
 * "<var>=<value>" on the clipboard. */
// host-control: drawer
static void row_context_menu(knob_desc_t *k)
{
    if (!ImGui::BeginPopupContextItem("##ctx")) return;
    char val[KNOB_STR_MAX];
    if (k->mut == KNOB_LIVE && k->set_num && k->type != KNOB_ACTION &&
        ImGui::MenuItem("Reset to boot value")) {
        k->set_num(k, k->boot_num);
        OPL_INFO_F("devui", "%s reset to boot value", k->id);
    }
    if (k->cli) {
        char line[KNOB_STR_MAX + 64];
        format_value_token(k, val, sizeof val);
        snprintf(line, sizeof line, "%s %s", k->cli, val);
        if (ImGui::MenuItem("Copy as CLI flag")) ImGui::SetClipboardText(line);
    }
    if (k->env) {
        char line[KNOB_STR_MAX + 64];
        format_value_token(k, val, sizeof val);
        snprintf(line, sizeof line, "%s=%s", k->env, val);
        if (ImGui::MenuItem("Copy as env line")) ImGui::SetClipboardText(line);
    }
    ImGui::EndPopup();
}

/** @brief Write a knob's help and its flag, variable and key spellings into the open tooltip.
 *
 * The key line carries the resolved binding of the row's key_action, in the
 * spelling of the current keyboard layout and the connected pad's family.
 * Adds a "restart to apply" line for a restart-only knob. */
static void knob_tooltip_body(const knob_desc_t *k)
{
    ImGui::TextUnformatted(k->help);
    ImGui::Separator();
    if (k->cli) ImGui::Text("flag: %s", k->cli);
    if (k->env) ImGui::Text("env:  %s", k->env);
    const int act = openpete_keymap_host_action(k->key_action);
    if (act >= 0) {
        char key[160];
        openpete_keymap_action_label((openpete_action_t)act, key, sizeof key);
        ImGui::Text("Key:  %s", key);
    }
    if (k->mut == KNOB_RESTART_ONLY)
        ImGui::TextDisabled("restart to apply");
}

/** @brief Report whether the item just submitted is hovered for a tooltip, disabled or not. */
static bool tooltip_hovered(void)
{
    return ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                                ImGuiHoveredFlags_AllowWhenDisabled);
}

/** @brief Open a tooltip that wraps at 30 font sizes. */
static void tooltip_begin(void)
{
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
}

/** @brief Close a tooltip opened by tooltip_begin. */
static void tooltip_end(void)
{
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/** @brief Draw the hover tooltip of the item just submitted: a knob's help and spellings.
 *
 * Shows over a disabled item too. */
static void row_tooltip(const knob_desc_t *k)
{
    if (!tooltip_hovered()) return;
    tooltip_begin();
    knob_tooltip_body(k);
    tooltip_end();
}

/** @brief Draw the hover tooltip of an action button: its get_str status line, when non-empty, above its help and spellings.
 *
 * Shows over a disabled button too, where the status says why it is
 * disabled. */
static void action_tooltip(const knob_desc_t *k)
{
    if (!tooltip_hovered()) return;
    char status[KNOB_STR_MAX] = "";
    if (k->get_str) k->get_str(k, status, sizeof status);
    tooltip_begin();
    if (status[0]) {
        ImGui::TextUnformatted(status);
        ImGui::Separator();
    }
    knob_tooltip_body(k);
    tooltip_end();
}

/* In-progress edit shadow. Numeric widgets are re-seeded from the live
 * getter every frame, so without this a +/- click or a drag is overwritten
 * by the getter on the next frame before the deactivate-commit ever fires
 * (the value visibly "reverts"). While a row's widget is active its edited
 * value lives here; the commit on release reads the shadow. */
static const knob_desc_t *s_edit_row = nullptr;
static double             s_edit_val = 0.0;

/** @brief Place the next widget on the previous widget's line at the button-group gap (ItemInnerSpacing.x). */
static void same_line_inner(void)
{
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
}

/** @brief Report the Left/Right step a focused slider takes this frame, claiming those keys from navigation.
 *
 * While the last item has navigation focus and is not being dragged, it owns
 * the keyboard's Left/Right arrows, so keyboard navigation does not move off
 * it; ownership set in one frame takes effect at the next frame's start,
 * before navigation reads the keys. Each press, and each key repeat, is one
 * step. The d-pad keeps the pad navigation's own slider handling.
 * @return -1 for a step down, 1 for a step up, else 0. */
static int slider_arrow_dir(void)
{
    if (!ImGui::IsItemFocused() || ImGui::IsItemActive()) return 0;
    static const ImGuiKey keys[2] = { ImGuiKey_LeftArrow, ImGuiKey_RightArrow };
    const ImGuiID id = ImGui::GetItemID();
    int dir = 0;
    for (int i = 0; i < 2; i++) {
        ImGui::SetKeyOwner(keys[i], id);
        if (ImGui::IsKeyPressed(keys[i], ImGuiInputFlags_Repeat, id)) dir += i == 0 ? -1 : 1;
    }
    return dir;
}

/** @brief Draw a float slider whose Left/Right step is a power of ten near a twentieth of its range.
 *
 * @param label The imgui label.
 * @param v The value, written on a drag or a step.
 * @param mn The range's low end.
 * @param mx The range's high end.
 * @param fmt The value's display format.
 * @return true when the value changed. */
// host-control: exempt: slider helper, the calling drawer writes the row
static bool slider_float_stepped(const char *label, float *v, float mn, float mx, const char *fmt)
{
    bool changed = ImGui::SliderFloat(label, v, mn, mx, fmt);
    const int dir = slider_arrow_dir();
    if (dir && mx > mn) {
        const float step = powf(10.0f, floorf(log10f((mx - mn) / 20.0f)));
        const float n = roundf(*v / step) + (float)dir;
        *v = fminf(fmaxf(n * step, mn), mx);
        changed = true;
    }
    return changed;
}

/** @brief Draw an int slider whose Left/Right step is 1.
 *
 * @param label The imgui label.
 * @param v The value, written on a drag or a step.
 * @param mn The range's low end.
 * @param mx The range's high end.
 * @return true when the value changed. */
// host-control: exempt: slider helper, the calling drawer writes the row
static bool slider_int_stepped(const char *label, int *v, int mn, int mx)
{
    bool changed = ImGui::SliderInt(label, v, mn, mx);
    const int dir = slider_arrow_dir();
    if (dir) {
        const int n = *v + dir;
        *v = n < mn ? mn : (n > mx ? mx : n);
        changed = true;
    }
    return changed;
}

/** @brief Find the registry row with an id, hidden or not.
 *
 * @param id The row id.
 * @return The row, or NULL when no row has the id (compiled out). */
static knob_desc_t *row_by_id(const char *id)
{
    size_t n = 0;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++)
        if (strcmp(ks[i].id, id) == 0) return &ks[i];
    return nullptr;
}

/** @brief Write a value through a row's setter, logging it as the row drawers do.
 *
 * @param k The row, or NULL (nothing is written).
 * @param v The value; 1 fires an action row.
 * @return true when the setter accepted it. */
static bool row_write(knob_desc_t *k, double v)
{
    if (!k || !k->set_num || k->set_num(k, v) != 0) return false;
    OPL_INFO_F("devui", "%s = %g", k->id, v);
    return true;
}

/** @brief Draw a captioned button that fires an action row, disabled while the row's gate reads 0.
 *
 * @param id The action row's id.
 * @param caption The button caption.
 * @return true when the row fired. */
// host-control: drawer
static bool row_button(const char *id, const char *caption)
{
    knob_desc_t *k = row_by_id(id);
    const bool ready = k && (!k->get_num || k->get_num(k) != 0.0);
    if (!ready) ImGui::BeginDisabled();
    const bool fired = ImGui::Button(caption) && row_write(k, 1.0);
    if (!ready) ImGui::EndDisabled();
    return fired;
}

/** @brief Draw a captioned checkbox over a bool row, written through the row's setter.
 *
 * @param id The bool row's id.
 * @param caption The checkbox caption. */
// host-control: drawer
static void row_checkbox(const char *id, const char *caption)
{
    knob_desc_t *k = row_by_id(id);
    bool b = k && (k->get_num ? k->get_num(k) : k->stored_num) != 0.0;
    if (ImGui::Checkbox(caption, &b)) (void)row_write(k, b ? 1.0 : 0.0);
}

/** @brief Draw a float knob as a slider over its registered range, applied on every change.
 *
 * A drag widget that commits only on release reads as dead while it is
 * moving, and its per-pixel speed suits one range and crawls on another; a
 * slider spans the range whatever the range is. Ctrl+click is imgui's own
 * typed entry and applies through the same call. A row carrying a
 * zero_label has a mode at 0 that the span up to num_min does not reach: a
 * drag through that span only ever hands the setter values it refuses, so
 * the mode is a checkbox, the slider starts at num_min, and the slider is
 * drawn only while the mode is off.
 * @param k The knob. */
// host-control: drawer
static void knob_row_float(knob_desc_t *k)
{
    double live = k->get_num ? k->get_num(k) : k->stored_num;
    if (k->zero_label) {
        bool off = (live <= 0.0);
        if (ImGui::Checkbox(k->zero_label, &off) && k->set_num) {
            const double v = off ? 0.0 : (double)k->num_min;
            if (k->set_num(k, v) == 0) {
                OPL_INFO_F("devui", "%s = %.3f", k->id, v);
                live = v;
            }
        }
        if (off) return;
        same_line_inner();
        ImGui::SetNextItemWidth(-FLT_MIN);
    }
    float v = (float)live;
    if (slider_float_stepped("##v", &v, k->num_min, k->num_max, "%.2f") &&
        k->set_num) {
        if (k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %.3f", k->id, v);
    }
}

/** @brief Read a numeric knob's current value: its getter, else the registry's stored copy. */
static double knob_num(const knob_desc_t *k)
{
    return k->get_num ? k->get_num(k) : k->stored_num;
}

/** @brief Draw a bool knob as a checkbox applied through its setter on each click. */
// host-control: drawer
static void knob_widget_bool(knob_desc_t *k)
{
    bool b = knob_num(k) != 0.0;
    if (ImGui::Checkbox("##v", &b) && k->set_num) {
        if (k->set_num(k, b ? 1.0 : 0.0) == 0)
            OPL_INFO_F("devui", "%s = %s", k->id, b ? "on" : "off");
    }
}

/** @brief Draw an int knob as an input field that commits when it loses focus after an edit.
 *
 * A half-typed value must not reach a setter with an apply cost (rewind
 * clears its history), so the edit lives in the shadow until release, and
 * the committed value is clamped to the knob's range when the range is
 * non-empty. */
// host-control: drawer
static void knob_widget_int(knob_desc_t *k)
{
    int v = (int)((s_edit_row == k) ? s_edit_val : knob_num(k));
    ImGui::InputInt("##v", &v);
    if (ImGui::IsItemActive()) { s_edit_row = k; s_edit_val = v; }
    if (ImGui::IsItemDeactivatedAfterEdit() && k->set_num) {
        if (k->num_max > k->num_min) {
            if (v < (int)k->num_min) v = (int)k->num_min;
            if (v > (int)k->num_max) v = (int)k->num_max;
        }
        if (k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %d", k->id, v);
    }
    if (ImGui::IsItemDeactivated() && s_edit_row == k) s_edit_row = nullptr;
}

/** @brief Draw an enum knob as a combo box applied through its setter on each pick. */
// host-control: drawer
static void knob_widget_enum(knob_desc_t *k)
{
    int idx = (int)knob_num(k);
    if (ImGui::Combo("##v", &idx, k->enum_values, k->enum_count) && k->set_num) {
        if (k->set_num(k, idx) == 0)
            OPL_INFO_F("devui", "%s = %s", k->id,
                       (idx >= 0 && idx < k->enum_count) ? k->enum_values[idx] : "?");
    }
}

/** @brief Draw an action knob as a button that fires its setter with 1.0; its status text is the button's tooltip.
 *
 * get_num, when present, gates the button: 0 draws it disabled. */
// host-control: drawer
static void knob_widget_action(knob_desc_t *k)
{
    const bool ready = !k->get_num || k->get_num(k) != 0.0;
    if (!ready) ImGui::BeginDisabled();
    if (ImGui::Button(k->action_label ? k->action_label : "Apply") && k->set_num) {
        if (k->set_num(k, 1.0) == 0)
            OPL_INFO_F("devui", "%s fired", k->id);
    }
    if (!ready) ImGui::EndDisabled();
    action_tooltip(k);
}

/** @brief Draw a string knob as a text field, editable and committed on release only for a live row with a setter.
 *
 * imgui keeps its own edit state while the field is active and writes back
 * into the buffer each frame, so a per-frame buffer seeded from the live
 * value is safe. */
// host-control: drawer
static void knob_widget_string(knob_desc_t *k)
{
    char buf[KNOB_STR_MAX];
    knob_value_str(k, buf, sizeof buf);
    if (!k->set_str || k->mut != KNOB_LIVE) {
        ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_ReadOnly);
        return;
    }
    ImGui::InputText("##v", buf, sizeof buf);
    if (ImGui::IsItemDeactivatedAfterEdit() && k->set_str(k, buf) == 0)
        OPL_INFO_F("devui", "%s = %s", k->id, buf);
}

/** @brief Draw a knob's type-specific value widget, disabled unless the knob is live, with its tooltip.
 *
 * An action row's tooltip carries its status (action_tooltip). The caller
 * sets the item width. */
static void knob_widget(knob_desc_t *k)
{
    const bool disabled = (k->mut != KNOB_LIVE);
    if (disabled) ImGui::BeginDisabled();
    switch (k->type) {
    case KNOB_BOOL:   knob_widget_bool(k);   break;
    case KNOB_INT:    knob_widget_int(k);    break;
    case KNOB_FLOAT:  knob_row_float(k);     break;
    case KNOB_ENUM:   knob_widget_enum(k);   break;
    case KNOB_ACTION: knob_widget_action(k); break;
    case KNOB_STRING: knob_widget_string(k); break;
    }
    if (disabled) ImGui::EndDisabled();
    if (k->type != KNOB_ACTION) row_tooltip(k);
}

/** @brief Write where a knob's value comes from into the open tooltip.
 *
 * The boot source, "edited this session" while the value differs from it,
 * and the launch tier that pins the knob or else the non-default
 * openpete.toml layer that set it. A pinned knob outranks the file, so saving
 * it does nothing until that override is gone; the line says so.
 * @param k The knob.
 * @param name Prefix naming the knob on a row of several; NULL on a single-knob row. */
static void knob_source_lines(const knob_desc_t *k, const char *name)
{
    const char *pre = name ? name : "source";
    if (row_edited(k))
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                           "%s: %s, edited this session", pre,
                           src_word(k->boot_src));
    else
        ImGui::TextDisabled("%s: %s", pre, src_word(k->boot_src));
    const char *pin = openpete_config_knob_pin_spelling(k->id);
    if (pin) {
        ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.0f, 1.0f),
                           "pinned: %s is set for this launch, which outranks "
                           "openpete.toml; a saved value applies once that "
                           "override is gone", pin);
        return;
    }
    const char *layer = openpete_config_knob_layer(k->id);
    if (layer && strcmp(layer, "default") != 0)
        ImGui::TextDisabled("openpete.toml: [%s]", layer);
}

/* Title-column edges: one shared by the top-level rows of every section, and
 * one per subsection title, so a subsection's titles size its own tables and
 * not the panel's. Every table sizes its title column so the value column
 * starts where the widest title of its record ends: `next` collects the right
 * edge of every title drawn this present (window-relative), and `edge` is the
 * last present's, applied now. A record that draws no title keeps its last
 * edge. */
struct title_edge {
    const char *key;   /* NULL for the panel's record, else the subsection title */
    float       edge;
    float       next;
};
static std::deque<title_edge> s_title_edges;
static title_edge            *s_title_cur = nullptr;

/** @brief Make the title-column record of a subsection, or the panel's for NULL, current; appends one on its first use.
 *
 * Records live in a deque, so a pointer to one stays valid as others are
 * appended. */
static void title_edge_enter(const char *key)
{
    for (title_edge &e : s_title_edges)
        if (e.key == key || (e.key && key && strcmp(e.key, key) == 0)) {
            s_title_cur = &e;
            return;
        }
    s_title_edges.push_back({ key, 0.0f, 0.0f });
    s_title_cur = &s_title_edges.back();
}

/** @brief Adopt every record's measured title edge and start a new measurement; called once per panel draw. */
static void title_edge_roll(void)
{
    for (title_edge &e : s_title_edges) {
        if (e.next > 0.0f) e.edge = e.next;
        e.next = 0.0f;
    }
    s_title_cur = nullptr;
}

/** @brief Draw the label cell of a panel row: the caption, its tooltip and the primary knob's right-click menu.
 *
 * The tooltip holds the primary knob's help and spellings, then the source
 * lines of every knob on the row, each prefixed with its label when the row
 * has several.
 * @param label The caption.
 * @param ks The row's knobs, the primary first.
 * @param n The knob count. */
static void row_label_cell(const char *label, knob_desc_t *const *ks, int n)
{
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (s_title_cur) {
        const float right = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
        if (right > s_title_cur->next) s_title_cur->next = right;
    }
    if (tooltip_hovered()) {
        tooltip_begin();
        knob_tooltip_body(ks[0]);
        ImGui::Separator();
        for (int i = 0; i < n; i++)
            knob_source_lines(ks[i], n > 1 ? ks[i]->label : nullptr);
        tooltip_end();
    }
    row_context_menu(ks[0]);
}

/** @brief Log a row's id the first time the panel draws it, under the OPENPETE_DEVUI_ROW_CENSUS harness variable.
 *
 * One line per row and drawing thread ("devui: row census: <id> drawn in
 * <section>"; the panel draws on the display's owner, the present thread
 * under eager production, else the host loop), so a scripted windowed run
 * proves which rows the open panel drew; a row in a collapsed section is not
 * drawn and logs nothing.
 * @param k The row being drawn. */
static void devui_row_census(const knob_desc_t *k)
{
    static std::atomic<int> on{-1};
    if (on.load(std::memory_order_relaxed) < 0) {
        const char *e = getenv("OPENPETE_DEVUI_ROW_CENSUS");
        on.store((e && *e && *e != '0') ? 1 : 0, std::memory_order_relaxed);
    }
    if (on.load(std::memory_order_relaxed) == 0 || !k) return;
    static thread_local std::unordered_set<std::string> seen;
    if (!seen.insert(k->id).second) return;
    OPL_INFO_F("devui", "row census: %s drawn in %s", k->id, k->section ? k->section : "?");
}

/** @brief Draw one knob as a table row: label, then its type-specific value widget. */
static void draw_row(knob_desc_t *k)
{
    devui_row_census(k);
    ImGui::TableNextRow();
    row_label_cell(k->label, &k, 1);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    knob_widget(k);
}

/* ------------------------------------------------------------------ */
/* Save bar                                                           */
/* ------------------------------------------------------------------ */

/** @brief Draw the Save settings button at a given width; the writer's last result is in its tooltip.
 *
 * Save enqueues every row edited this session to the settings writer
 * (openpete_settings_save_request), which writes them into the config
 * file's [saved] block at the host loop's next iteration; the button is
 * disabled while no row is edited. Overlay edits are saved only on a click.
 * @param w The button width. */
static void draw_save_button(float w)
{
    const char *ids[256];
    int edited = 0;
    size_t n = 0;
    knob_desc_t *ks = knob_registry(&n);
    for (size_t i = 0; i < n; i++)
        if (ks[i].type != KNOB_ACTION && row_edited(&ks[i]) &&
            edited < (int)(sizeof ids / sizeof ids[0]))
            ids[edited++] = ks[i].id;

    if (edited == 0) ImGui::BeginDisabled();
    // host-control: exempt: writes openpete.toml
    if (ImGui::Button("Save settings", ImVec2(w, 0.0f)))
        openpete_settings_save_request(ids, edited);
    if (edited == 0) ImGui::EndDisabled();
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip |
                              ImGuiHoveredFlags_AllowWhenDisabled))
        return;
    ImGui::BeginTooltip();
    unsigned writes = 0;
    int rows = 0, failed = 0, pending = 0;
    openpete_settings_save_status(&writes, &rows, &failed, &pending);
    if (failed) {
        ImGui::Text("save FAILED, %d setting(s) kept for the next save (see the log)", pending);
        ImGui::Separator();
    } else if (writes > 0) {
        ImGui::Text("last save: %d setting(s) to %s", rows, openpete_config_save_path());
        ImGui::Separator();
    }
    ImGui::Text("Write every row you changed this session into the\n"
                "[saved] block of %s.\n\n"
                "Saved settings beat everything else in the file, but\n"
                "still lose to a flag you type or a variable you export\n"
                "- rows in that position are marked 'pinned by ...'.",
                openpete_config_save_path());
    ImGui::EndTooltip();
}

/* ------------------------------------------------------------------ */
/* Mods panel                                                         */
/* ------------------------------------------------------------------ */

/* The SDK's openpete_mod_ui_t widget vtable, implemented over imgui -
 * mods draw their section through these and never see imgui types.
 * Callbacks run inside the mod's section under the render-purity guard
 * (openpete_mods_ui_draw for the tree node, openpete_mods_ui_draw_always
 * for a top-level always section). */

/** @brief Draw printf-formatted text, truncated to 1023 bytes.
 *
 * @param fmt The printf format, followed by its arguments. */
static void mui_text(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextUnformatted(buf);
}

/** @brief Draw printf-formatted text in the disabled colour, truncated to 1023 bytes. */
static void mui_text_disabled(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    ImGui::TextDisabled("%s", buf);
}

/** @brief Draw a button and report whether it was clicked (1) or not (0). */
// host-control: exempt: mod UI helper, the mod's own state
static int mui_button(const char *label)
{
    return ImGui::Button(label) ? 1 : 0;
}

/** @brief Draw a checkbox over an int and report 1 when the click changed it.
 *
 * A NULL v draws an unchecked box and stores nothing. */
// host-control: exempt: mod UI helper, the mod's own state
static int mui_checkbox(const char *label, int *v)
{
    bool b = v && *v != 0;
    if (ImGui::Checkbox(label, &b)) {
        if (v) *v = b ? 1 : 0;
        return 1;
    }
    return 0;
}

/** @brief Draw an int slider over [mn, mx] and report 1 when the value changed; a NULL v draws nothing. */
static int mui_slider_int(const char *label, int *v, int mn, int mx)
{
    return (v && slider_int_stepped(label, v, mn, mx)) ? 1 : 0;
}

/** @brief Draw a float slider over [mn, mx] and report 1 when the value changed; a NULL v draws nothing. */
static int mui_slider_float(const char *label, float *v, float mn, float mx)
{
    return (v && slider_float_stepped(label, v, mn, mx, "%.3f")) ? 1 : 0;
}

/** @brief Draw a combo box over the item strings and report 1 when the selection changed.
 *
 * A NULL idx or items draws nothing. */
// host-control: exempt: mod UI helper, the mod's own state
static int mui_combo(const char *label, int *idx, const char *const *items,
                     int count)
{
    return (idx && items && ImGui::Combo(label, idx, items, count)) ? 1 : 0;
}

/** @brief Draw a text field editing buf of cap bytes in place and report 1 when the text changed.
 *
 * A NULL buf or a non-positive cap draws nothing. */
// host-control: exempt: mod UI helper, the mod's own state
static int mui_input_text(const char *label, char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    return ImGui::InputText(label, buf, (size_t)cap) ? 1 : 0;
}

/** @brief Draw a horizontal separator line. */
static void mui_separator(void) { ImGui::Separator(); }
/** @brief Place the next widget on the previous widget's line. */
static void mui_same_line(void) { ImGui::SameLine(); }

/** @brief Report 1 while the config panel is open, else 0. */
static int mui_overlay_open(void) { return s_open ? 1 : 0; }

/** @brief Show text as a tooltip while the previous widget is hovered; a NULL text shows nothing. */
static void mui_tooltip(const char *text)
{
    if (text && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", text);
}

static const openpete_mod_ui_t s_mod_ui = {
    mui_text,       mui_text_disabled, mui_button,    mui_checkbox,
    mui_slider_int, mui_slider_float,  mui_combo,     mui_input_text,
    mui_separator,  mui_same_line,     mui_tooltip,   mui_overlay_open,
    {},
};

/* The state notes a mod row's tooltip can show, indexed by mod_note_t. The
 * surface's vocabulary check reads them through openpete_devui_mod_notes. */
enum mod_note_t {
    MOD_NOTE_NOT_STARTED,
    MOD_NOTE_ASSETS,
    MOD_NOTE_OWNER_BUSY,
    MOD_NOTE_NO_CONTROLS,
    MOD_NOTE_COUNT
};
static const char *const k_mod_notes[MOD_NOTE_COUNT] = {
    "not started yet - enabling runs the mod at the next tick boundary",
    "asset overlay: textures/music follow the toggle; level data applies at "
    "the next level load",
    "can't be turned off while you're inside its level",
    "no controls registered",
};

const char *const *openpete_devui_mod_notes(size_t *count)
{
    if (count) *count = MOD_NOTE_COUNT;
    return k_mod_notes;
}

/** @brief Draw a mod's guest memory use in each arena pool as one disabled-colour line, nothing when it holds none.
 *
 * The figures are the mod's ledger rows (openpete_mods_arena_owner_use),
 * IMMUTABLE bytes included and also shown apart.
 * @param id The mod id. */
static void draw_mod_memory_line(const char *id)
{
    openpete_mods_arena_use_t u;
    if (!openpete_mods_arena_owner_use(id, &u)) return;
    ImGui::TextDisabled("Guest memory: %.1f KiB DMA-reachable, %.1f KiB general "
                        "(%.1f KiB immutable)",
                        u.dma_bytes / 1024.0, u.general_bytes / 1024.0,
                        (u.dma_immutable + u.general_immutable) / 1024.0);
}

/** @brief Draw the Mods section's memory line: every mod's pool use summed, and runahead's snapshot cost once it has run.
 *
 * Nothing is drawn while no mod holds guest memory and runahead has not
 * speculated.
 * @param n The loaded mod count. */
static void draw_mods_memory_summary(int n)
{
    uint64_t dma = 0, gen = 0;
    for (int i = 0; i < n; i++) {
        openpete_mods_menu_info_t mi;
        openpete_mods_arena_use_t u;
        if (!openpete_mods_menu_info(i, &mi) ||
            !openpete_mods_arena_owner_use(mi.id, &u))
            continue;
        dma += u.dma_bytes;
        gen += u.general_bytes;
    }
    const uint32_t ra = openpete_mods_runahead_snapshot_us();
    if (dma || gen)
        ImGui::TextDisabled("Mod memory: %.2f of 14 MiB DMA-reachable, %.2f MiB general",
                            dma / 1048576.0, gen / 1048576.0);
    if (ra)
        ImGui::TextDisabled("Runahead snapshot: %.2f ms per tick", ra / 1000.0);
}

/** @brief Draw the hover tooltip of a mod's row: id and version plus the state notes that apply.
 *
 * Notes cover a mod not yet started, an asset overlay, an enabled mod that
 * cannot be disabled while its level is active, an enabled mod with no
 * controls, and the guest memory the mod holds. A build with the debug
 * flags compiled in adds last present's ImGui call count. */
static void mod_row_tooltip(const openpete_mods_menu_info_t *mi)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
    ImGui::Text("%s v%s", mi->id, mi->version);
    if (!mi->entered)
        ImGui::TextDisabled("%s", k_mod_notes[MOD_NOTE_NOT_STARTED]);
    if (mi->has_assets)
        ImGui::TextDisabled("%s", k_mod_notes[MOD_NOTE_ASSETS]);
    if (mi->enabled && openpete_mods_registry_owner_busy(mi->id))
        ImGui::TextDisabled("%s", k_mod_notes[MOD_NOTE_OWNER_BUSY]);
    if (mi->enabled && !mi->has_ui && !mi->has_cfg)
        ImGui::TextDisabled("%s", k_mod_notes[MOD_NOTE_NO_CONTROLS]);
    draw_mod_memory_line(mi->id);
#ifndef OPENPETE_HIDE_DEBUG_FLAGS
    if (mi->imgui_calls)
        ImGui::TextDisabled("ImGui: %llu calls last present",
                            (unsigned long long)mi->imgui_calls);
#endif
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

/* ── Engine-rendered config schema ([[config]] rows) ────────────────
 * The engine draws each mod's settings from its manifest schema; edits
 * buffer here (per mod id) and commit through openpete_mods_cfg_apply,
 * which writes config.toml and queues a reload so the mod's entry
 * re-reads the values. Nothing applies until the user hits Apply - a
 * half-dragged slider never half-configures a mod. */

struct mod_cfg_edit_value {
    int64_t i;
    double  d;
    char    s[128];
};

struct mod_cfg_edit_state {
    char id[64];
    int  n;
    bool seeded;
    std::vector<mod_cfg_edit_value> v;   /* one per [[config]] row */
};

/* Deque, not vector: a slot pointer handed to the draw below must stay
 * valid when a later mod's panel appends its own slot. */
static std::deque<mod_cfg_edit_state> s_cfg_edit;

/** @brief Find the config edit buffer of a mod id, appending an unseeded one when none exists.
 *
 * The returned pointer stays valid for the session because the slots live
 * in a deque. */
static mod_cfg_edit_state *cfg_edit_slot(const char *id)
{
    for (auto &e : s_cfg_edit)
        if (strcmp(e.id, id) == 0) return &e;
    s_cfg_edit.emplace_back();
    mod_cfg_edit_state *slot = &s_cfg_edit.back();
    snprintf(slot->id, sizeof slot->id, "%s", id);
    slot->n = 0;
    slot->seeded = false;
    return slot;
}

/** @brief Fill a mod's edit buffer from the schema's current values, one entry per config row.
 *
 * Rows the registry cannot return stay zeroed. Marks the buffer seeded with
 * the given row count. */
static void cfg_seed(mod_cfg_edit_state *st, int mod_index, int rows)
{
    st->n = rows;
    st->v.assign((size_t)(rows > 0 ? rows : 0), mod_cfg_edit_value{});
    for (int r = 0; r < rows; r++) {
        openpete_mods_cfg_row_t row;
        if (!openpete_mods_cfg_row(mod_index, r, &row)) continue;
        st->v[r].i = row.ival;
        st->v[r].d = row.fval;
        snprintf(st->v[r].s, sizeof st->v[r].s, "%s", row.sval);
    }
    st->seeded = true;
}

/** @brief Draw a mod's [[config]] rows over its edit buffer, with Apply and Revert once edited.
 *
 * The buffer is reseeded when it is new or the row count changed. Rows are
 * grouped under separator headings by their group field; a bool, int, float
 * or string row uses a checkbox, an int or float slider (when min < max, else
 * a plain input), or a combo (string with an item list) or text field. Apply
 * hands every row to openpete_mods_cfg_apply; Revert reseeds from the
 * current values. Nothing reaches the mod before Apply. */
// host-control: exempt: a mod's config values, applied by the mod
static void draw_mod_config_rows(int mod_index,
                                 const openpete_mods_menu_info_t *mi)
{
    const int rows = openpete_mods_cfg_row_count(mod_index);
    if (rows <= 0) return;
    mod_cfg_edit_state *st = cfg_edit_slot(mi->id);
    if (!st) return;
    if (!st->seeded || st->n != rows) cfg_seed(st, mod_index, rows);

    bool dirty = false;
    const char *open_group = "";
    for (int r = 0; r < rows && r < (int)st->v.size(); r++) {
        openpete_mods_cfg_row_t row;
        if (!openpete_mods_cfg_row(mod_index, r, &row)) continue;
        if (row.group[0] && strcmp(row.group, open_group) != 0) {
            ImGui::SeparatorText(row.group);
            open_group = row.group;
        }
        ImGui::PushID(row.key);
        switch (row.type) {
        case 'b': {
            bool b = st->v[r].i != 0;
            if (ImGui::Checkbox(row.label, &b)) st->v[r].i = b ? 1 : 0;
            dirty |= (st->v[r].i != row.ival);
            break;
        }
        case 'i': {
            int iv = (int)st->v[r].i;
            if (row.min < row.max) {
                if (slider_int_stepped(row.label, &iv, (int)row.min,
                                       (int)row.max))
                    st->v[r].i = iv;
            } else if (ImGui::InputInt(row.label, &iv)) {
                st->v[r].i = iv;
            }
            dirty |= (st->v[r].i != row.ival);
            break;
        }
        case 'd': {
            float f = (float)st->v[r].d;
            if (row.min < row.max) {
                if (slider_float_stepped(row.label, &f, (float)row.min,
                                         (float)row.max, "%.3f"))
                    st->v[r].d = f;
            } else if (ImGui::InputFloat(row.label, &f)) {
                st->v[r].d = f;
            }
            dirty |= (st->v[r].d != row.fval);
            break;
        }
        case 's': {
            if (row.items && row.n_items > 0) {
                int idx = 0;
                for (int k = 0; k < row.n_items; k++)
                    if (strcmp(row.items[k], st->v[r].s) == 0) idx = k;
                if (ImGui::Combo(row.label, &idx, row.items, row.n_items))
                    snprintf(st->v[r].s, sizeof st->v[r].s, "%s",
                             row.items[idx]);
            } else {
                ImGui::InputText(row.label, st->v[r].s,
                                 sizeof st->v[r].s);
            }
            dirty |= (strcmp(st->v[r].s, row.sval) != 0);
            break;
        }
        }
        if (row.help[0] &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
            ImGui::SetTooltip("%s", row.help);
        ImGui::PopID();
    }

    if (dirty) {
        if (ImGui::Button("Apply")) {
            std::vector<openpete_mods_cfg_edit_t> edits(
                (size_t)(rows > 0 ? rows : 0));
            int n = 0;
            for (int r = 0; r < rows && r < (int)st->v.size(); r++) {
                openpete_mods_cfg_row_t row;
                if (!openpete_mods_cfg_row(mod_index, r, &row)) continue;
                edits[n].key  = row.key;
                edits[n].type = row.type;
                edits[n].ival = st->v[r].i;
                edits[n].fval = st->v[r].d;
                edits[n].sval = st->v[r].s;
                n++;
            }
            if (openpete_mods_cfg_apply(mod_index, n, edits.data()) == 0)
                OPL_INFO_F("devui", "mod %s: config applied", mi->id);
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert")) cfg_seed(st, mod_index, rows);
        ImGui::SameLine();
        ImGui::TextDisabled("(pending)");
    }
}

/* ── Mod UI section bracket ──────────────────────────────────────────
 * A body is mod C driving the engine's ImGui context directly (the mod table,
 * openpete_imgui.h). ImGui reports unbalanced Begin/End, Push/Pop and
 * similar misuse as recoverable user errors; the bracket stores the stack
 * sizes before the body and recovers to them after, so a mod that leaves a
 * window open does not swallow the panel or the frame. The body also holds
 * a writable ImGuiStyle and ImGuiIO, so both are snapshotted and put back.
 * ImGui's own reaction is silenced: the assert is inert under NDEBUG and
 * would abort the process in linux-debug, and the tooltip would paint over
 * the panel, so the engine logs the error instead. A body that errors on
 * three consecutive presents loses its section for the session.
 *
 * io.FontGlobalScale is not in the snapshot: it is compiled out under
 * IMGUI_DISABLE_OBSOLETE_FUNCTIONS, and its replacement style.FontScaleMain
 * rides the style copy. */
static bool s_mod_body_errored = false;

/** @brief Log an ImGui error raised inside a mod's UI body and flag that the body errored.
 *
 * Installed as the context's error callback for the duration of the body.
 * user_data is the mod id. */
static void mod_body_error_cb(ImGuiContext *, void *user_data,
                              const char *msg)
{
    s_mod_body_errored = true;
    OPL_ERROR_F("mods", "%s: ImGui error inside the mod's UI section: "
                "%s", (const char *)user_data, msg ? msg : "");
}

/* Consecutive presents whose body errored, per pass and mod index. The
 * vector grows to the highest mod index drawn, so a mod loaded after boot is
 * counted like any other.
 *
 * Per pass, because the two passes run in the same present and only one of
 * them runs a given mod's body: for an always section the node pass draws
 * ui_status lines and nothing else, so a shared counter would see a clean
 * bracket from that pass every present and reset a streak the always pass
 * was accumulating - the refusal would never fire. */
static std::vector<int> s_mod_body_err_streak[2];

/** @brief Draw one mod's UI section inside the error-recovery bracket and count its errors.
 *
 * Stores ImGui's stack state, style and writable IO fields before the body
 * and restores them after; routes ImGui errors to mod_body_error_cb. A body
 * that errors on three consecutive presents of the same pass has its
 * section refused for the session.
 * @param mod_index The loader's mod index; a negative index skips the
 *        error count.
 * @param mod_id The mod id, named in the error log.
 * @param always false draws the mod's node contents (status lines plus a
 *        plain body); true draws an always-flagged body at top level.
 * @param no_input true on an always pass with the overlay closed, which
 *        puts ImGuiWindowFlags_NoInputs on every window the body opens. */
static void draw_mod_section_bracketed(int mod_index, const char *mod_id,
                                       bool always, bool no_input)
{
    ImGuiContext &g  = *ImGui::GetCurrentContext();
    ImGuiIO      &io = ImGui::GetIO();

    ImGuiErrorRecoveryState recovery;
    ImGui::ErrorRecoveryStoreState(&recovery);

    const ImGuiStyle       style_save    = g.Style;
    const char *const      ini_save      = io.IniFilename;
    const char *const      log_save      = io.LogFilename;
    const ImGuiConfigFlags flags_save    = io.ConfigFlags;
    void *const            user_save     = io.UserData;
    void *const            renderer_save = io.BackendRendererUserData;
    void *const            platform_save = io.BackendPlatformUserData;
    const bool             assert_save   = io.ConfigErrorRecoveryEnableAssert;
    const bool             tooltip_save  = io.ConfigErrorRecoveryEnableTooltip;
    ImGuiErrorCallback     cb_save       = g.ErrorCallback;
    void *const            cb_user_save  = g.ErrorCallbackUserData;

    io.ConfigErrorRecoveryEnableAssert  = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    g.ErrorCallback         = mod_body_error_cb;
    g.ErrorCallbackUserData = (void *)mod_id;
    s_mod_body_errored      = false;

    if (always)
        openpete_mods_ui_draw_always(mod_index, &s_mod_ui, no_input ? 1 : 0);
    else
        openpete_mods_ui_draw(mod_index, &s_mod_ui);

    ImGui::ErrorRecoveryTryToRecoverState(&recovery);

    g.Style                             = style_save;
    io.IniFilename                      = ini_save;
    io.LogFilename                      = log_save;
    io.ConfigFlags                      = flags_save;
    io.UserData                         = user_save;
    io.BackendRendererUserData          = renderer_save;
    io.BackendPlatformUserData          = platform_save;
    io.ConfigErrorRecoveryEnableAssert  = assert_save;
    io.ConfigErrorRecoveryEnableTooltip = tooltip_save;
    g.ErrorCallback                     = cb_save;
    g.ErrorCallbackUserData             = cb_user_save;

    const int pass = always ? 1 : 0;
    if (mod_index < 0) return;
    /* One counter per loaded mod: the table grows with the mod set, so a
     * late mod's misbehaving body is refused like any other's. */
    if ((size_t)mod_index >= s_mod_body_err_streak[pass].size())
        s_mod_body_err_streak[pass].resize((size_t)mod_index + 1, 0);
    if (!s_mod_body_errored) {
        s_mod_body_err_streak[pass][mod_index] = 0;
        return;
    }
    if (++s_mod_body_err_streak[pass][mod_index] >= 3)
        openpete_mods_ui_refuse_section(
            mod_index, "its body reported an ImGui error on three "
                       "consecutive presents");
}

/** @brief Report whether a mod row passes the text filter, by matching its id or name.
 *
 * An inactive filter passes every row. */
static bool mod_row_matches(const ImGuiTextFilter &f,
                            const openpete_mods_menu_info_t *mi)
{
    if (!f.IsActive()) return true;
    return f.PassFilter(mi->id) || f.PassFilter(mi->name);
}

static bool mod_owns_custom_levels(const char *mod_id);
static void draw_custom_level_rows(const char *mod_id);

/** @brief Draw the Open folder button at the right end of the line just submitted.
 *
 * Disabled when no mods root resolved; the tooltip names the folder.
 * @param right The content region's right edge in window coordinates. */
static void draw_mods_folder_button(float right)
{
    const char *cap = "Open folder";
    const float w = ImGui::CalcTextSize(cap).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(right - w);
    const char *dir = openpete_mods_root_dir();
    if (!dir) ImGui::BeginDisabled();
    // host-control: exempt: opens the mods folder
    if (ImGui::Button(cap)) (void)openpete_mods_open_folder(nullptr);
    if (!dir) ImGui::EndDisabled();
    if (dir && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        ImGui::SetTooltip("%s", dir);
}

/** @brief Draw one mod's row: the enable checkbox, then its name or, for an enabled mod with controls or levels, a tree node holding them.
 *
 * The checkbox queues the toggle and is disabled while the mod owns the level
 * being played. The node holds the mod's config rows, its bracketed UI body
 * and its custom-level rows; the boot reveal opens it.
 * @param i The mod's loader index.
 * @param mi The mod's menu info. */
static void draw_mod_row(int i, const openpete_mods_menu_info_t &mi)
{
    /* The checkbox queues through the live-toggle path; the loader
     * applies it at the next tick boundary and the row reads the
     * applied state back - no local shadow to go stale. Grayed while
     * the mod owns the level the player is inside (a disable would
     * strand them; the loader would defer it anyway). */
    const bool busy =
        mi.enabled && openpete_mods_registry_owner_busy(mi.id) != 0;
    bool on = mi.enabled != 0;
    if (busy) ImGui::BeginDisabled();
    // host-control: exempt: the enabled mod set
    if (ImGui::Checkbox("##en", &on)) {
        if (openpete_mods_set_mod_enabled(mi.id, on ? 1 : 0) == 0)
            OPL_INFO_F("devui", "mod %s -> %s", mi.id,
                       on ? "enabled" : "disabled");
    }
    if (busy) ImGui::EndDisabled();
    mod_row_tooltip(&mi);
    ImGui::SameLine();

    const bool has_levels = mod_owns_custom_levels(mi.id);
    if (mi.enabled && (mi.has_ui || mi.has_cfg || has_levels)) {
        if (s_boot_reveal) ImGui::SetNextItemOpen(true);
        if (ImGui::TreeNodeEx(mi.name,
                              ImGuiTreeNodeFlags_SpanAvailWidth)) {
            mod_row_tooltip(&mi);
            draw_mod_config_rows(i, &mi);
            if (mi.has_ui)
                draw_mod_section_bracketed(i, mi.id, false, false);
            if (has_levels) draw_custom_level_rows(mi.id);
            ImGui::TreePop();
        } else {
            mod_row_tooltip(&mi);
        }
    } else {
        ImGui::AlignTextToFramePadding();
        if (mi.enabled)
            ImGui::TextUnformatted(mi.name);
        else
            ImGui::TextDisabled("%s", mi.name);
        mod_row_tooltip(&mi);
    }
}

/** @brief Draw one row per mod refused at load whose id passes the filter: a cleared, disabled checkbox, the id, and the reason.
 *
 * @param filter The panel's search filter. */
static void draw_refused_mod_rows(const ImGuiTextFilter &filter)
{
    const int n = openpete_mods_refused_count();
    for (int i = 0; i < n; i++) {
        const char *id = nullptr, *why = nullptr;
        if (!openpete_mods_refused_at(i, &id, &why) || !filter.PassFilter(id)) continue;
        ImGui::PushID(i);
        ImGui::PushID("refused");
        bool off = false;
        ImGui::BeginDisabled();
        // host-control: exempt: read-only state of a refused mod
        ImGui::Checkbox("##en", &off);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s - not loaded: %s", id, why);
        ImGui::PopID();
        ImGui::PopID();
    }
}

/** @brief Write the channel names of an OP_CHAN_* bit set, comma separated.
 *
 * @param chans The bits.
 * @param buf Receives the text.
 * @param cap Bytes in @p buf. */
static void overlap_chan_names(unsigned chans, char *buf, size_t cap)
{
    static const char *const k_chan[] = {"", "moby", "terrain", "player", "flame",
                                         "particle", "sky", "shadow", "tracer", "portal"};
    size_t at = 0;
    buf[0] = 0;
    for (int ch = 1; ch <= 9 && at < cap; ch++)
        if (chans & (1u << ch))
            at += (size_t)snprintf(buf + at, cap - at, "%s%s", at ? "," : "", k_chan[ch]);
}

/** @brief Draw a collapsed tree node listing each pair of enabled materials whose selectors share a key, with its shadow verdict.
 *
 * One line per pair: the higher-ranked material, the lower one, full or
 * partial, the shared channels, and the accept-all note when the full
 * verdict rests on a moby refine that never declined. Draws nothing while
 * no pair overlaps. Runs inside the compose, under the registry read lock
 * the published pair list is read under. */
static void draw_material_overlaps()
{
    const int n = openpete_mods_material_overlap_count();
    const int total = openpete_mods_material_overlap_total();
    if (total == 0) return;
    if (!ImGui::TreeNode("##overlaps", "Material overlaps (%d)", total)) return;
    for (int i = 0; i < n; i++) {
        openpete_mods_material_overlap_info_t p;
        if (openpete_mods_material_overlap_at(i, &p) != 0) continue;
        char chans[96];
        overlap_chan_names(p.chans, chans, sizeof chans);
        ImGui::Text("%s #%d %s over %s #%d %s: %s on %s%s", p.hi_owner ? p.hi_owner : "?", p.hi,
                    p.hi_refine ? "refine" : "decl", p.lo_owner ? p.lo_owner : "?", p.lo,
                    p.lo_refine ? "refine" : "decl", p.verdict, chans,
                    p.accept_all ? " (accept-all refine)" : "");
    }
    if (total > n) ImGui::TextDisabled("%d more not listed", total - n);
    ImGui::TreePop();
}

/** @brief Draw the Mods tab: a status line with the Open folder button at its right end, then one row per loaded mod that passes the filter.
 *
 * The status line says no mod is installed (the folder button shows where
 * they go), that no mod matches an active filter, or how many mods are
 * loaded and enabled. Each row has an enable checkbox that queues the toggle
 * (disabled while the mod owns the level being played) and a name; an
 * enabled mod with a UI section, config rows or custom levels gets a tree
 * node holding its config rows, its bracketed UI body and its level rows.
 * Mods refused at load follow, each with its reason, then the material
 * overlaps and the memory summary while no filter hides every mod. With no
 * mod installed or refused only the status line draws. */
static void draw_mods_section(const ImGuiTextFilter &filter)
{
    const int n = openpete_mods_count();
    int shown = 0, enabled = 0;
    for (int i = 0; i < n; i++) {
        openpete_mods_menu_info_t mi;
        if (!openpete_mods_menu_info(i, &mi)) continue;
        if (mi.enabled) enabled++;
        if (mod_row_matches(filter, &mi)) shown++;
    }

    const bool none = n == 0 && openpete_mods_refused_count() == 0;
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (none)
        ImGui::TextDisabled("No mods installed. Open folder shows where they go.");
    else if (shown == 0 && filter.IsActive())
        ImGui::TextDisabled("No mod matches the search.");
    else
        ImGui::TextDisabled("%d loaded, %d enabled", n, enabled);
    draw_mods_folder_button(right);
    if (none) return;
    ImGui::Separator();

    for (int i = 0; i < n; i++) {
        openpete_mods_menu_info_t mi;
        if (!openpete_mods_menu_info(i, &mi)) continue;
        if (!mod_row_matches(filter, &mi)) continue;
        ImGui::PushID(mi.id);
        draw_mod_row(i, mi);
        ImGui::PopID();
    }
    draw_refused_mod_rows(filter);
    if (shown == 0 && filter.IsActive()) return;
    draw_material_overlaps();
    draw_mods_memory_summary(n);
}

/** @brief Report whether a mod declares any [[level]] custom level.
 *
 * @param mod_id The mod id.
 * @return true when a custom level in the registry names mod_id as owner. */
static bool mod_owns_custom_levels(const char *mod_id)
{
    const int n = openpete_mods_custom_count();
    for (int i = 0; i < n; i++) {
        openpete_mods_custom_info_t ci;
        if (openpete_mods_custom_info(i, &ci) && !strcmp(ci.owner, mod_id))
            return true;
    }
    return false;
}

/** @brief Write the tooltip of a custom level's name: its Custom World, donor and serving.
 *
 * The registry's index order is alphabetical across all mods, and that
 * global order buckets levels five per Custom World (7 + index / 5), so the
 * world number is shown per level here rather than as a grouping.
 * @param i The level's position in the registry's name order.
 * @param ci The level's descriptor. */
static void custom_level_tooltip(int i, const openpete_mods_custom_info_t &ci)
{
    ImGui::BeginTooltip();
    ImGui::Text("Custom World %d - donor level %d", 7 + i / 5, ci.donor_id);
    if (!ci.files_ok)
        ImGui::TextDisabled("level files not derived yet");
    else
        ImGui::TextDisabled(ci.replace_mode
            ? "serves over its donor level's own entry"
            : "time-shares the free level slot (id 65); "
              "per-level progress is shared between custom "
              "levels until real extra worlds land");
    ImGui::EndTooltip();
}

/** @brief Draw one custom level's line: the Warp button, its name, and the serving combo.
 *
 * The Warp button and the combo write the custom-level rows: the level
 * becomes the custom_level target, then custom_level_go fires or
 * custom_level_mode takes the picked mode. The button is disabled while the
 * level's files are missing or its mod is disabled.
 * @param i The level's position in the registry's name order.
 * @param ci The level's descriptor. */
static void draw_custom_level_line(int i, const openpete_mods_custom_info_t &ci)
{
    knob_desc_t *target = row_by_id("custom_level");
    const bool playable = ci.files_ok && openpete_mods_is_enabled(ci.owner) != 0;
    if (!playable) ImGui::BeginDisabled();
    // host-control: custom_level_go
    if (ImGui::Button(ci.active ? "In slot" : "Warp") && row_write(target, i))
        (void)row_write(row_by_id("custom_level_go"), 1.0);
    if (!playable) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(ci.name);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) custom_level_tooltip(i, ci);
    ImGui::SameLine();
    knob_desc_t *mode_row = row_by_id("custom_level_mode");
    if (!mode_row) return;
    int mode = ci.replace_mode;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    // host-control: custom_level_mode
    if (ImGui::Combo("##m", &mode, mode_row->enum_values, mode_row->enum_count) &&
        row_write(target, i))
        (void)row_write(mode_row, mode);
}

/** @brief Draw a mod's declarative [[level]] rows, each with a Warp button, inside its node in the Mods list.
 *
 * @param mod_id The owning mod's id. */
static void draw_custom_level_rows(const char *mod_id)
{
    const int n = openpete_mods_custom_count();
    for (int i = 0; i < n; i++) {
        openpete_mods_custom_info_t ci;
        if (!openpete_mods_custom_info(i, &ci) || strcmp(ci.owner, mod_id) != 0) continue;
        ImGui::PushID(i);
        draw_custom_level_line(i, ci);
        ImGui::PopID();
    }
}

/** @brief Report whether a knob row applies to this run.
 *
 * A row whose knob_desc_t::visible callback returns 0 is left out of the
 * panel and out of the scan that decides whether its section has a row to
 * show, so a section whose rows are all hidden emits no header. A NULL
 * callback means always shown.
 * @param k The knob row.
 * @return true when the row applies. */
static bool row_visible(const knob_desc_t *k)
{
    return !k->visible || k->visible(k) != 0;
}

/** @brief Report whether a knob passes the panel's text filter.
 *
 * An inactive filter passes every knob; an active one passes a knob when it
 * matches the knob's id, label, CLI spelling, or environment variable name.
 * @param f The panel's text filter.
 * @param k The knob row.
 * @return true when the knob matches. */
static bool knob_matches(const ImGuiTextFilter &f, const knob_desc_t *k)
{
    if (!f.IsActive()) return true;
    return f.PassFilter(k->id) || f.PassFilter(k->label) ||
           (k->cli && f.PassFilter(k->cli)) || (k->env && f.PassFilter(k->env));
}

/* ------------------------------------------------------------------ */
/* panel layout                                                       */
/* ------------------------------------------------------------------ */

/* The panel's arrangement of the registry. The registry's own section field
 * groups the --help text and the docs map; the panel regroups rows here,
 * combines related knobs into one row, and picks the widget. A registry row
 * that no layout row names and that is not in k_lay_hidden is drawn by a
 * LAY_REST entry naming its registry section, or, when no entry names that
 * section, under a header of the section's own name after the laid-out
 * sections - so a new registry row always appears somewhere. */
enum lay_kind {
    LAY_ROW,       /* ids[0] with its type's widget */
    LAY_SEG,       /* ids[0] (enum or bool) as a row of buttons */
    LAY_INLINE,    /* ids[0..2] side by side; ids[0] at 0 disables the rest */
    LAY_DRAWMODE,  /* ids[0] notex, ids[1] wireframe as one three-way combo */
    LAY_ONOFF,     /* ids[0] int, 0 = off: checkbox captioned `caption`, the
                    * field while on; `fallback` is the first on value */
    LAY_DEPTH,     /* ids[0] int: on/off box, 1..3 buttons while on */
    LAY_OVERRIDE,  /* ids[0] int, -1 = leave alone: Override box, slider while on */
    LAY_TOKENS,    /* ids[0] comma-list string: a toggle per vocabulary token */
    LAY_PICK,      /* ids[0] enum combo, ids[1..2] its action buttons */
    LAY_REST,      /* every unplaced row of registry section `label` */
    LAY_SUB,       /* opens a subsection titled `label`, closed by default */
    LAY_SUB_END
};

struct lay_row {
    lay_kind           kind;
    const char        *label    = nullptr; /* caption; LAY_REST: the registry section */
    const char        *ids[3]   = {};      /* knob ids; unused slots NULL */
    const char *const *seg      = nullptr; /* LAY_SEG captions; NULL = the enum's values */
    const double      *seg_val  = nullptr; /* value each caption sets; NULL = its index */
    int                seg_n    = 0;
    const char        *caption  = nullptr; /* LAY_ONOFF checkbox caption */
    int                fallback = 0;       /* LAY_ONOFF first on value */
};

/* What a section draws: its layout rows, or one of the sections with a
 * drawer of their own. */
enum lay_section_kind { LAY_SEC_ROWS, LAY_SEC_MODS, LAY_SEC_CONTROLS };

/* The panel's tabs, in tab-bar order. A registry section no LAY_REST entry
 * names draws on the Settings tab. */
enum lay_tab { LAY_TAB_SETTINGS, LAY_TAB_MODS, LAY_TAB_COUNT };
static const char *const k_lay_tab_names[LAY_TAB_COUNT] = { "Settings", "Mods" };

struct lay_section {
    const char       *title;     /* header caption; a LAY_SEC_MODS section draws no header */
    lay_tab           tab;
    bool              open;      /* expanded until the user collapses it */
    lay_section_kind  kind;
    const lay_row    *rows;
    size_t            n;
};

#define LAY_N(a) (sizeof(a) / sizeof((a)[0]))

static const char *const k_depth_caps[] = { "15-bit", "24-bit" };
static const double      k_depth_vals[] = { 1.0, 0.0 };

static const lay_row k_lay_display[] = {
    { LAY_ROW,    nullptr, { "fullscreen" } },
    { LAY_ROW,    nullptr, { "aspect" } },
    { LAY_ONOFF,  nullptr, { "render_fps" }, nullptr, nullptr, 0,
      "Interpolation", 60 },
    { LAY_ROW,    nullptr, { "interp" } },
    { LAY_ROW,    nullptr, { "fov" } },
    { LAY_ROW,    nullptr, { "ssaa" } },
    { LAY_ROW,    nullptr, { "vsync" } },
    { LAY_INLINE, "Performance stats", { "perf_stats", "perf_position" } },
    { LAY_SEG,    nullptr, { "vertex_precision" } },
    { LAY_ROW,    nullptr, { "raster_quantize" } },
    { LAY_SEG,    nullptr, { "dither" }, k_depth_caps, k_depth_vals,
      (int)LAY_N(k_depth_caps) },
    { LAY_SEG,    nullptr, { "texture_mapping" } },
    { LAY_SEG,    nullptr, { "lod" } },
    { LAY_ROW,    nullptr, { "fog" } },
    { LAY_REST,   "Display" },
    { LAY_SUB,    "Advanced" },
    { LAY_SEG,    nullptr, { "cadence" } },
    { LAY_INLINE, "Notifications", { "osd", "osd_position", "osd_timeout" } },
    { LAY_ROW,    nullptr, { "fog_scale" } },
    { LAY_DRAWMODE, "Render mode", { "notex", "wireframe" } },
    { LAY_ROW,    nullptr, { "terrain_cache" } },
    { LAY_SEG,    nullptr, { "sector_visibility" } },
    { LAY_TOKENS, nullptr, { "render_paths" } },
    { LAY_SUB_END, nullptr },
};


static const lay_row k_lay_gameplay[] = {
    { LAY_ROW,    nullptr, { "freecam" } },
    { LAY_ROW,    nullptr, { "freecam_speed" } },
    { LAY_ROW,    nullptr, { "teleport" } },
    { LAY_PICK,   "Savestate", { "state_slot", "state_save", "state_load" } },
    { LAY_ROW,    nullptr, { "tick_scale" } },
    { LAY_INLINE, nullptr, { "pause", "frame_step" } },
    { LAY_ROW,    nullptr, { "turbo" } },
    { LAY_DEPTH,  nullptr, { "runahead" } },
    { LAY_ONOFF,  "Rewind", { "rewind" }, nullptr, nullptr, 0, "", 30 },
    { LAY_ROW,    nullptr, { "rewind_hold" } },
    { LAY_REST,   "Gameplay" },
    { LAY_PICK,   "Level warp", { "warp_level", "warp_go" } },
    { LAY_PICK,   "Area warp", { "warp_area", "warp_area_go" } },
    { LAY_SUB,    "Cheats" },
    { LAY_REST,   "Cheats" },
    { LAY_SUB_END, nullptr },
};

static const lay_row k_lay_audio[] = {
    { LAY_REST,   "Audio" },
    { LAY_OVERRIDE, nullptr, { "sfx" } },
    { LAY_OVERRIDE, nullptr, { "music" } },
};

static const lay_row k_lay_input[] = { { LAY_REST, "Input" } };

static const lay_row k_lay_info[] = {
    { LAY_ROW,    nullptr, { "skip_to_level" } },
    { LAY_ROW,    nullptr, { "skip_to_cutscene" } },
    { LAY_ROW,    nullptr, { "skip_to_area" } },
    { LAY_ROW,    nullptr, { "freecam_pose" } },
    { LAY_ROW,    nullptr, { "player_pose" } },
    { LAY_ROW,    nullptr, { "bk2" } },
    { LAY_ROW,    nullptr, { "bk2_frame_offset" } },
    { LAY_ROW,    nullptr, { "bk2_record" } },
    { LAY_ROW,    nullptr, { "renderer" } },
    { LAY_ROW,    nullptr, { "extract_threads" } },
    { LAY_ROW,    nullptr, { "window" } },
    { LAY_SUB,    "Paths" },
    { LAY_REST,   "Paths" },
    { LAY_SUB_END, nullptr },
};

static const lay_row k_lay_recording[] = { { LAY_REST, "Recording" } };

static const lay_row k_lay_debug[] = {
    { LAY_ROW,    nullptr, { "TERRAIN_HDFOLD" } },
    { LAY_REST,   "Debug" },
};

static const lay_section k_layout[] = {
    { "Display",   LAY_TAB_SETTINGS, true,  LAY_SEC_ROWS,     k_lay_display,   LAY_N(k_lay_display) },
    { "Gameplay",  LAY_TAB_SETTINGS, true,  LAY_SEC_ROWS,     k_lay_gameplay,  LAY_N(k_lay_gameplay) },
    { "Audio",     LAY_TAB_SETTINGS, false, LAY_SEC_ROWS,     k_lay_audio,     LAY_N(k_lay_audio) },
    { "Input",     LAY_TAB_SETTINGS, false, LAY_SEC_ROWS,     k_lay_input,     LAY_N(k_lay_input) },
    { "Controls",  LAY_TAB_SETTINGS, false, LAY_SEC_CONTROLS, nullptr,         0 },
    { "Info",      LAY_TAB_SETTINGS, false, LAY_SEC_ROWS,     k_lay_info,      LAY_N(k_lay_info) },
    { "Recording", LAY_TAB_SETTINGS, true,  LAY_SEC_ROWS,     k_lay_recording, LAY_N(k_lay_recording) },
    { "Debug",     LAY_TAB_SETTINGS, false, LAY_SEC_ROWS,     k_lay_debug,     LAY_N(k_lay_debug) },
    { "Mods",      LAY_TAB_MODS,     true,  LAY_SEC_MODS,     nullptr,         0 },
};

/* Registry rows the panel does not show. They stay in the registry, so
 * --help, --config-report, the config file and saving still carry them. */
static const char *const k_lay_hidden[] = {
    "background", "version_osd", "mouse", "input_lock", "camlock", "bk2_stop_on_end", "cheats",
    "launcher_fullscreen", "launcher_window", "hide_player",
    /* Drawn by the fill-tag inspector's panel and by each custom level's
     * line in the Mods list, through these rows. */
    "fillpick_fill", "fillpick_nofill", "fillpick_none", "fillpick_undo",
    "custom_level", "custom_level_mode", "custom_level_go",
};

/** @brief Report whether a knob id is named by a layout row or hidden from the panel.
 *
 * @param id The knob id.
 * @return true when a LAY_REST entry must skip the row. */
static bool lay_names_id(const char *id)
{
    for (const char *h : k_lay_hidden)
        if (strcmp(h, id) == 0) return true;
    for (const lay_section &s : k_layout)
        for (size_t r = 0; r < s.n; r++)
            for (const char *rid : s.rows[r].ids)
                if (rid && strcmp(rid, id) == 0) return true;
    return false;
}

/** @brief Report whether a registry section is drawn by a LAY_REST entry of the layout. */
static bool lay_rests_section(const char *section)
{
    for (const lay_section &s : k_layout)
        for (size_t r = 0; r < s.n; r++)
            if (s.rows[r].kind == LAY_REST && strcmp(s.rows[r].label, section) == 0)
                return true;
    return false;
}

/* Per registry row: 1 when the layout names it or hides it. Rebuilt when the
 * row count changes; rows are added only before the panel first draws. */
static std::vector<unsigned char> s_lay_claimed;

/** @brief Refresh the per-row claimed flags when the registry's row count changed. */
static void lay_claim_refresh(const knob_desc_t *ks, size_t n)
{
    if (s_lay_claimed.size() == n) return;
    s_lay_claimed.assign(n, 0);
    for (size_t i = 0; i < n; i++)
        s_lay_claimed[i] = lay_names_id(ks[i].id) ? 1 : 0;
}

/* The registry and the filter for one panel draw. */
struct lay_ctx {
    knob_desc_t           *ks;
    size_t                 n;
    const ImGuiTextFilter *filter;
};

/** @brief Find the visible registry row with an id.
 *
 * @return The row; NULL when no row has the id or the row is hidden for this run. */
static knob_desc_t *lay_find(const lay_ctx &c, const char *id)
{
    if (!id) return nullptr;
    for (size_t i = 0; i < c.n; i++)
        if (strcmp(c.ks[i].id, id) == 0)
            return row_visible(&c.ks[i]) ? &c.ks[i] : nullptr;
    return nullptr;
}

/** @brief Report whether a registry row is drawn by a LAY_REST entry of its section under the filter. */
static bool lay_rest_shows(const lay_ctx &c, size_t i, const char *section)
{
    return !s_lay_claimed[i] && strcmp(c.ks[i].section, section) == 0 &&
           row_visible(&c.ks[i]) && knob_matches(*c.filter, &c.ks[i]);
}

/** @brief Collect a layout row's knobs that exist and apply to this run.
 *
 * @param out Receives up to three knobs, in id order, skipping absent ones.
 * @return The knob count; 0 when the row's first id is absent. */
static int lay_knobs(const lay_ctx &c, const lay_row &r, knob_desc_t *out[3])
{
    int n = 0;
    for (const char *id : r.ids) {
        knob_desc_t *k = lay_find(c, id);
        if (k) out[n++] = k;
        else if (n == 0 && id == r.ids[0]) return 0;
    }
    return n;
}

/** @brief Report whether a layout row draws anything under the filter.
 *
 * A LAY_REST entry shows when any of its rows does; a knob row when its knobs
 * exist (both for LAY_DRAWMODE and LAY_PICK) and its caption or one of its
 * knobs matches. */
static bool lay_row_shows(const lay_ctx &c, const lay_row &r)
{
    if (r.kind == LAY_SUB || r.kind == LAY_SUB_END) return false;
    if (r.kind == LAY_REST) {
        for (size_t i = 0; i < c.n; i++)
            if (lay_rest_shows(c, i, r.label)) return true;
        return false;
    }
    knob_desc_t *ks[3];
    const int n = lay_knobs(c, r, ks);
    const bool pair = (r.kind == LAY_DRAWMODE || r.kind == LAY_PICK);
    if (n == 0 || (pair && n < 2)) return false;
    if (r.label && c.filter->PassFilter(r.label)) return true;
    for (int i = 0; i < n; i++)
        if (knob_matches(*c.filter, ks[i])) return true;
    return false;
}

/** @brief Report whether any row in [from, to) of a section draws under the filter. */
static bool lay_span_shows(const lay_ctx &c, const lay_row *rows, size_t from,
                           size_t to)
{
    for (size_t r = from; r < to; r++)
        if (lay_row_shows(c, rows[r])) return true;
    return false;
}

/** @brief Draw an enum or bool knob as a row of equal-width buttons, the current value's highlighted.
 *
 * Clicking another button applies its value through the knob's setter.
 * @param k The knob.
 * @param r The layout row: its seg captions and values, or the enum's. */
// host-control: drawer
static void seg_widget(knob_desc_t *k, const lay_row &r)
{
    const int n = r.seg ? r.seg_n : k->enum_count;
    const char *const *caps = r.seg ? r.seg : k->enum_values;
    double live = knob_num(k);
    if (k->type == KNOB_BOOL) live = (live != 0.0) ? 1.0 : 0.0;
    const float sp = ImGui::GetStyle().ItemInnerSpacing.x;
    const float w = (ImGui::GetContentRegionAvail().x - sp * (float)(n - 1)) / (float)n;
    const bool disabled = (k->mut != KNOB_LIVE);
    if (disabled) ImGui::BeginDisabled();
    for (int i = 0; i < n; i++) {
        const double val = r.seg_val ? r.seg_val[i] : (double)i;
        const bool on = (live == val);
        if (i > 0) ImGui::SameLine(0.0f, sp);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                      ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        ImGui::PushID(i);
        const bool clicked = ImGui::Button(caps[i], ImVec2(w, 0.0f));
        ImGui::PopID();
        if (on) ImGui::PopStyleColor();
        row_tooltip(k);
        if (clicked && !on && k->set_num && k->set_num(k, val) == 0)
            OPL_INFO_F("devui", "%s = %s", k->id, caps[i]);
    }
    if (disabled) ImGui::EndDisabled();
}

/** @brief Draw up to three knobs side by side; while the first reads 0 only the first is drawn.
 *
 * A checkbox takes its natural width; the other widgets share the remaining
 * width equally, split as a button group splits its row (seg_widget), so
 * the edges line up with a group of as many buttons. */
static void inline_widget(knob_desc_t *const *ks, int n)
{
    if (knob_num(ks[0]) == 0.0) n = 1;
    int flex = 0;
    for (int i = 0; i < n; i++)
        if (ks[i]->type != KNOB_BOOL) flex++;
    const float sp = ImGui::GetStyle().ItemInnerSpacing.x;
    float w = 0.0f;   /* per-widget width, fixed at the first non-checkbox */
    for (int i = 0; i < n; i++) {
        if (i > 0) same_line_inner();
        ImGui::PushID(i);
        if (ks[i]->type != KNOB_BOOL) {
            if (w == 0.0f)
                w = (ImGui::GetContentRegionAvail().x - sp * (float)(flex - 1)) /
                    (float)flex;
            ImGui::SetNextItemWidth(flex > 1 ? w : -FLT_MIN);
            flex--;
        }
        knob_widget(ks[i]);
        ImGui::PopID();
    }
}

/** @brief Draw the untextured and wireframe views as one Textured / Untextured / Wireframe combo.
 *
 * Wireframe sets the wireframe knob and clears untextured; Untextured the
 * reverse; Textured clears both.
 * @param notex The untextured-view knob.
 * @param wire The wireframe-view knob. */
// host-control: drawer
static void drawmode_widget(knob_desc_t *notex, knob_desc_t *wire)
{
    static const char *const k_modes[] = { "Textured", "Untextured", "Wireframe" };
    int idx = knob_num(wire) != 0.0 ? 2 : knob_num(notex) != 0.0 ? 1 : 0;
    const bool disabled = notex->mut != KNOB_LIVE || wire->mut != KNOB_LIVE ||
                          !notex->set_num || !wire->set_num;
    if (disabled) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##v", &idx, k_modes, (int)LAY_N(k_modes)) && !disabled) {
        wire->set_num(wire, idx == 2 ? 1.0 : 0.0);
        notex->set_num(notex, idx == 1 ? 1.0 : 0.0);
        OPL_INFO_F("devui", "render mode = %s", k_modes[idx]);
    }
    if (disabled) ImGui::EndDisabled();
    row_tooltip(idx == 2 ? wire : notex);
}

/** @brief Find the last on-value remembered for a knob, seeding it on first use.
 *
 * @param k The knob.
 * @param seed The value stored when the knob has no slot yet.
 * @return The knob's slot; valid for the session. */
static int *last_on_slot(const knob_desc_t *k, int seed)
{
    static std::deque<std::pair<const knob_desc_t *, int>> s_last;
    for (auto &e : s_last)
        if (e.first == k) return &e.second;
    s_last.emplace_back(k, seed);
    return &s_last.back().second;
}

/** @brief Draw an int knob whose 0 is off as a checkbox, with its value field beside it only while on.
 *
 * Turning it on restores the last value that was on: the boot value when it
 * was on, else `fallback`.
 * @param k The knob.
 * @param caption The checkbox caption; "" for none.
 * @param fallback The value turning it on uses before any was on. */
// host-control: drawer
static void onoff_int_widget(knob_desc_t *k, const char *caption, int fallback)
{
    int *last = last_on_slot(k, k->boot_num > 0.0 ? (int)k->boot_num : fallback);
    const double live = knob_num(k);
    if (live > 0.0) *last = (int)live;
    bool on = live > 0.0 || s_edit_row == k;
    const bool disabled = (k->mut != KNOB_LIVE || !k->set_num);
    if (disabled) ImGui::BeginDisabled();
    char id[64];
    snprintf(id, sizeof id, "%s##on", caption);
    if (ImGui::Checkbox(id, &on) && !disabled) {
        const double v = on ? (double)*last : 0.0;
        if (k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %d", k->id, (int)v);
    }
    if (disabled) ImGui::EndDisabled();
    row_tooltip(k);
    if (!on) return;
    same_line_inner();
    ImGui::SetNextItemWidth(-FLT_MIN);
    knob_widget(k);
}

/** @brief Draw an int knob whose 0 is off as an on/off box, with a 1..num_max button group beside it only while on.
 *
 * Turning it on restores the last depth that was on, 1 before any. At most
 * three depths are offered. */
// host-control: drawer
static void depth_widget(knob_desc_t *k)
{
    static const char *const k_depths[] = { "1", "2", "3" };
    static const double      k_depth_v[] = { 1.0, 2.0, 3.0 };
    static int s_last_on = 1;
    const int live = (int)knob_num(k);
    if (live > 0) s_last_on = live;
    bool on = live > 0;
    const bool disabled = (k->mut != KNOB_LIVE || !k->set_num);
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Checkbox("##on", &on) && !disabled) {
        const int v = on ? s_last_on : 0;
        if (k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %d", k->id, v);
    }
    if (disabled) ImGui::EndDisabled();
    row_tooltip(k);
    if (!on) return;
    const int max = (int)k->num_max < (int)LAY_N(k_depths)
                        ? (int)k->num_max : (int)LAY_N(k_depths);
    const lay_row depths = { LAY_SEG, nullptr, { k->id }, k_depths, k_depth_v, max };
    same_line_inner();
    seg_widget(k, depths);
}

/** @brief Draw an int knob whose -1 leaves a value alone as an Override box, with a slider over 0..num_max beside it only while on.
 *
 * Turning it on restores the last overriding value, num_max before any. */
// host-control: drawer
static void override_widget(knob_desc_t *k)
{
    const int live = (int)knob_num(k);
    int *last = last_on_slot(k, (int)k->num_max);
    if (live >= 0) *last = live;
    bool on = live >= 0;
    const bool disabled = (k->mut != KNOB_LIVE || !k->set_num);
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Override", &on) && !disabled) {
        const int v = on ? *last : -1;
        if (k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %d", k->id, v);
    }
    row_tooltip(k);
    if (on) {
        same_line_inner();
        ImGui::SetNextItemWidth(-FLT_MIN);
        int v = live >= 0 ? live : *last;
        if (slider_int_stepped("##v", &v, 0, (int)k->num_max) && !disabled &&
            k->set_num(k, v) == 0)
            OPL_INFO_F("devui", "%s = %d", k->id, v);
        row_tooltip(k);
    }
    if (disabled) ImGui::EndDisabled();
}

/** @brief Read a comma-list string knob's value as a bit per vocabulary token.
 *
 * An empty value or a "(...)" placeholder reads as every token on; "none"
 * and unknown tokens contribute no bit.
 * @param k The knob; enum_values holds at most 32 tokens.
 * @return The token mask. */
static uint32_t tokens_mask(const knob_desc_t *k)
{
    const uint32_t all = k->enum_count >= 32 ? 0xFFFFFFFFu
                                             : (1u << k->enum_count) - 1u;
    char cur[KNOB_STR_MAX];
    knob_value_str(k, cur, sizeof cur);
    if (!cur[0] || cur[0] == '(') return all;
    uint32_t m = 0;
    for (char *tok = strtok(cur, ", "); tok; tok = strtok(nullptr, ", "))
        for (int i = 0; i < k->enum_count && i < 32; i++)
            if (strcmp(tok, k->enum_values[i]) == 0) m |= 1u << i;
    return m;
}

/** @brief Apply a token mask to a comma-list string knob through its setter.
 *
 * Every token on writes the empty value (no selection), none writes "none",
 * and any other mask writes the on tokens in vocabulary order. */
static void tokens_apply(knob_desc_t *k, uint32_t m)
{
    const uint32_t all = k->enum_count >= 32 ? 0xFFFFFFFFu
                                             : (1u << k->enum_count) - 1u;
    char spec[KNOB_STR_MAX] = "";
    if (m == 0) snprintf(spec, sizeof spec, "none");
    size_t off = 0;
    for (int i = 0; m != all && i < k->enum_count && i < 32; i++) {
        if (!(m & (1u << i))) continue;
        off += (size_t)snprintf(spec + off, off < sizeof spec ? sizeof spec - off : 0,
                                "%s%s", off ? "," : "", k->enum_values[i]);
    }
    if (k->set_str(k, spec) == 0)
        OPL_INFO_F("devui", "%s = %s", k->id, spec[0] ? spec : "(all)");
}

/** @brief Draw a comma-list string knob as All and None buttons and one checkbox per vocabulary token, wrapped to the cell width.
 *
 * A knob with no vocabulary bound, or one wider than 32 tokens, falls back to
 * its text field. */
// host-control: drawer
static void tokens_widget(knob_desc_t *k)
{
    if (!k->enum_values || k->enum_count <= 0 || k->enum_count > 32) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        knob_widget(k);
        return;
    }
    const uint32_t all = k->enum_count == 32 ? 0xFFFFFFFFu
                                             : (1u << k->enum_count) - 1u;
    const uint32_t cur = tokens_mask(k);
    uint32_t m = cur;
    const bool disabled = (k->mut != KNOB_LIVE || !k->set_str);
    if (disabled) ImGui::BeginDisabled();
    if (ImGui::SmallButton("All")) m = all;
    row_tooltip(k);
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) m = 0;
    row_tooltip(k);
    const ImGuiStyle &st = ImGui::GetStyle();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < k->enum_count; i++) {
        const float w = ImGui::GetFrameHeight() + st.ItemInnerSpacing.x +
                        ImGui::CalcTextSize(k->enum_values[i]).x;
        if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right)
            ImGui::SameLine();
        bool on = (m & (1u << i)) != 0;
        if (ImGui::Checkbox(k->enum_values[i], &on))
            m = on ? (m | (1u << i)) : (m & ~(1u << i));
        row_tooltip(k);
    }
    if (disabled) ImGui::EndDisabled();
    if (m != cur && !disabled) tokens_apply(k, m);
}

/** @brief Draw an enum combo and its action buttons on one line; each button's status is its tooltip.
 *
 * The combo's list grows to the height of the display before it scrolls.
 * @param ks ks[0] is the enum knob holding the selected entry; ks[1..n-1]
 *           are the action knobs the buttons fire, left to right.
 * @param n Number of knobs in @p ks, 2 or 3. */
// host-control: drawer
static void pick_widget(knob_desc_t *const *ks, int n)
{
    knob_desc_t *lvl = ks[0];
    const ImGuiStyle &st = ImGui::GetStyle();
    /* Each button spans an int field's -/+ pair (two frame-height squares and
     * their gap), so with one button the combo ends where those fields' text
     * boxes end. */
    const float bw = ImGui::GetFrameHeight() * 2.0f + st.ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x -
                            (float)(n - 1) * (bw + st.ItemInnerSpacing.x));
    const int idx = (int)knob_num(lvl);
    const char *preview =
        (idx >= 0 && idx < lvl->enum_count) ? lvl->enum_values[idx] : "";
    if (ImGui::BeginCombo("##lvl", preview, ImGuiComboFlags_HeightLargest)) {
        for (int i = 0; i < lvl->enum_count; i++) {
            const bool sel = (i == idx);
            if (ImGui::Selectable(lvl->enum_values[i], sel) && lvl->set_num &&
                lvl->set_num(lvl, i) == 0)
                OPL_INFO_F("devui", "%s = %s", lvl->id, lvl->enum_values[i]);
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    row_tooltip(lvl);

    for (int i = 1; i < n; i++) {
        knob_desc_t *go = ks[i];
        const char *cap = go->action_label ? go->action_label : "Apply";
        same_line_inner();
        const bool ready = !go->get_num || go->get_num(go) != 0.0;
        if (!ready) ImGui::BeginDisabled();
        if (ImGui::Button(cap, ImVec2(bw, 0.0f)) && go->set_num &&
            go->set_num(go, 1.0) == 0)
            OPL_INFO_F("devui", "%s fired", go->id);
        if (!ready) ImGui::EndDisabled();
        action_tooltip(go);
    }
}

/** @brief Draw one layout row of knobs as a table row: caption, then its widget. */
static void lay_draw_knob_row(const lay_ctx &c, const lay_row &r)
{
    knob_desc_t *ks[3];
    const int n = lay_knobs(c, r, ks);
    if (r.kind == LAY_ROW) { draw_row(ks[0]); return; }
    for (int i = 0; i < n; i++) devui_row_census(ks[i]);
    ImGui::TableNextRow();
    row_label_cell(r.label ? r.label : ks[0]->label, ks, n);
    ImGui::TableNextColumn();
    switch (r.kind) {
    case LAY_SEG:      seg_widget(ks[0], r);           break;
    case LAY_INLINE:   inline_widget(ks, n);           break;
    case LAY_DRAWMODE: drawmode_widget(ks[0], ks[1]);  break;
    case LAY_ONOFF:    onoff_int_widget(ks[0], r.caption, r.fallback); break;
    case LAY_DEPTH:    depth_widget(ks[0]);            break;
    case LAY_OVERRIDE: override_widget(ks[0]);         break;
    case LAY_TOKENS:   tokens_widget(ks[0]);           break;
    case LAY_PICK:     pick_widget(ks, n);             break;
    default:                                           break;
    }
}

/** @brief Draw the rows [from, to) of a section, none of them a subsection marker, as one table.
 *
 * A LAY_REST entry draws every unclaimed registry row of its section in
 * registry order. */
static void lay_draw_table(const lay_ctx &c, const lay_row *rows, size_t from,
                           size_t to)
{
    if (!lay_span_shows(c, rows, from, to)) return;
    /* Title column ends at the record's widest title plus one item gap;
     * 0 (nothing measured yet) auto-fits this table's own titles. */
    const ImGuiStyle &st = ImGui::GetStyle();
    float title_w = 0.0f;
    if (s_title_cur && s_title_cur->edge > 0.0f) {
        const float x0 = ImGui::GetCursorScreenPos().x - ImGui::GetWindowPos().x +
                         st.CellPadding.x;
        title_w = s_title_cur->edge + st.ItemSpacing.x - x0;
        if (title_w < 1.0f) title_w = 1.0f;
    }
    ImGui::PushID((int)from);
    if (ImGui::BeginTable("##t", 2, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("name",  ImGuiTableColumnFlags_WidthFixed, title_w);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        for (size_t r = from; r < to; r++) {
            if (!lay_row_shows(c, rows[r])) continue;
            if (rows[r].kind != LAY_REST) {
                ImGui::PushID((int)r);
                lay_draw_knob_row(c, rows[r]);
                ImGui::PopID();
                continue;
            }
            for (size_t i = 0; i < c.n; i++) {
                if (!lay_rest_shows(c, i, rows[r].label)) continue;
                ImGui::PushID(c.ks[i].id);
                draw_row(&c.ks[i]);
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

/** @brief Find the index of the LAY_SUB_END that closes the subsection opened at `sub`, or `n` when none does. */
static size_t lay_sub_end(const lay_row *rows, size_t sub, size_t n)
{
    size_t j = sub + 1;
    while (j < n && rows[j].kind != LAY_SUB_END) j++;
    return j;
}

/** @brief Draw a section's rows: tables between subsections, each subsection a tree node closed by default.
 *
 * An active filter opens every subsection with a match; a subsection with no
 * row to show is left out. A subsection sizes its title column from its own
 * title-column record. */
static void lay_draw_rows(const lay_ctx &c, const lay_row *rows, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (rows[i].kind != LAY_SUB) {
            size_t j = i;
            while (j < n && rows[j].kind != LAY_SUB) j++;
            lay_draw_table(c, rows, i, j);
            i = j;
            continue;
        }
        const size_t end = lay_sub_end(rows, i, n);
        if (lay_span_shows(c, rows, i + 1, end)) {
            if (c.filter->IsActive()) ImGui::SetNextItemOpen(true);
            if (ImGui::TreeNodeEx(rows[i].label, ImGuiTreeNodeFlags_SpanAvailWidth)) {
                title_edge *outer = s_title_cur;
                title_edge_enter(rows[i].label);
                lay_draw_table(c, rows, i + 1, end);
                s_title_cur = outer;
                ImGui::TreePop();
            }
        }
        i = end + 1;
    }
}

/** @brief Draw one section under its collapsing header when any of its rows shows.
 *
 * An active filter forces the header open. The panel's title-column record
 * is current while its top-level rows draw. */
static void lay_draw_section(const lay_ctx &c, const char *title, bool open,
                             const lay_row *rows, size_t n)
{
    if (!lay_span_shows(c, rows, 0, n)) return;
    if (c.filter->IsActive()) ImGui::SetNextItemOpen(true);
    ImGui::PushID(title);
    if (ImGui::CollapsingHeader(title, open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        title_edge_enter(nullptr);
        lay_draw_rows(c, rows, n);
        s_title_cur = nullptr;
    }
    ImGui::PopID();
}

/** @brief Draw, after the Settings tab's laid-out sections, each registry section no LAY_REST entry names, under its own name.
 *
 * Sections come in registry order and hold their unclaimed rows; a Debug*
 * section starts collapsed. */
static void lay_draw_unplaced(const lay_ctx &c)
{
    size_t i = 0;
    while (i < c.n) {
        const char *section = c.ks[i].section;
        size_t end = i;
        while (end < c.n && strcmp(c.ks[end].section, section) == 0) end++;
        if (!lay_rests_section(section)) {
            const lay_row rest[] = { { LAY_REST, section } };
            lay_draw_section(c, section, strncmp(section, "Debug", 5) != 0,
                             rest, 1);
        }
        i = end;
    }
}

/** @brief Draw the search field and the Save settings button on one line spanning the section headers' width, a magnifier drawn inside the field's left edge.
 *
 * @param f The panel's filter; rebuilt when the text changes.
 * @param hint The field's text while empty. */
static void draw_search(ImGuiTextFilter &f, const char *hint)
{
    /* The section headers overhang the content region by half the window
     * padding on each side; the search field and the Save button span the
     * same width, the button at the right end. */
    const ImGuiStyle &st = ImGui::GetStyle();
    const float half = IM_TRUNC(st.WindowPadding.x * 0.5f);
    const float save_w = ImGui::CalcTextSize("Save settings").x + st.FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - half);
    const float search_w = ImGui::GetContentRegionAvail().x + half - save_w -
                           st.ItemInnerSpacing.x;

    const float icon = ImGui::GetFontSize();
    const ImVec2 pad = st.FramePadding;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(pad.x * 2.0f + icon, pad.y));
    ImGui::SetNextItemWidth(search_w);
    // host-control: exempt: the overlay's search filter
    if (ImGui::InputTextWithHint("##search", hint, f.InputBuf,
                                 IM_COUNTOF(f.InputBuf)))
        f.Build();
    ImGui::PopStyleVar();

    /* A lens and a handle at 45 degrees, in the hint text's colour. */
    const ImVec2 mn = ImGui::GetItemRectMin();
    const ImVec2 mx = ImGui::GetItemRectMax();
    const float r = icon * 0.30f;
    const ImVec2 ctr(mn.x + pad.x + icon * 0.42f, (mn.y + mx.y) * 0.5f - icon * 0.08f);
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircle(ctr, r, col, 0, 1.5f);
    const float d = r * 0.7071f;
    dl->AddLine(ImVec2(ctr.x + d, ctr.y + d),
                ImVec2(ctr.x + d + r * 0.75f, ctr.y + d + r * 0.75f), col, 2.0f);

    ImGui::SameLine(0.0f, st.ItemInnerSpacing.x);
    draw_save_button(save_w);
}

/* ------------------------------------------------------------------ */
/* Controls: the binding page                                         */
/* ------------------------------------------------------------------ */

/** @brief Name the layer a Reset returns an action to: "default", "global", "game" or "disc", from the layer's origin label. */
static const char *reset_layer_word(const char *origin)
{
    if (!strncmp(origin, "game", 4)) return "game";
    if (!strncmp(origin, "disc", 4)) return "disc";
    if (!strcmp(origin, "default")) return "default";
    return "global";
}

/** @brief Report whether an action's row is listed: never a fill-tag inspector action, and KM_DEV rows only in a build that compiles the debug flags in.
 *
 * @param a The action.
 * @param info The action's description.
 * @return true when the binding page shows the row. */
static bool controls_row_listed(openpete_action_t a, const openpete_keymap_action_info_t &info)
{
    if (a >= OPK_FILLPICK && a <= OPK_FILLPICK_UNDO) return false;   // host-control: exempt: binding page listing
#ifdef OPENPETE_HIDE_DEBUG_FLAGS
    if (info.flags & KM_DEV) return false;
#endif
    (void)info;
    return true;
}

/** @brief Draw a button per bound source (click: replace it) with a remove button beside each, all on one line. */
// host-control: exempt: key bindings
static void draw_source_buttons(openpete_action_t a, const km_source_t *src, int n)
{
    for (int i = 0; i < n; i++) {
        char label[80];
        openpete_keymap_source_label(&src[i], label, sizeof label);
        if (src[i].hold) strncat(label, " (hold)", sizeof label - strlen(label) - 1);
        ImGui::PushID(i);
        if (ImGui::Button(label)) openpete_rebind_request_capture((int)a, i, 0);
        if (tooltip_hovered()) ImGui::SetTooltip("Click, then press the input that replaces it");
        ImGui::SameLine(0.0f, 2.0f);
        if (ImGui::SmallButton("x")) openpete_rebind_request_remove((int)a, i);
        if (tooltip_hovered()) ImGui::SetTooltip("Remove this input");
        ImGui::PopID();
        ImGui::SameLine();
    }
}

/** @brief Show Reset's tooltip: the layer it returns to ("default", "global", "game", "disc") and that layer's binding. */
static void reset_tooltip(openpete_action_t a)
{
    km_source_t below[KM_MAX_SOURCES];
    char origin[64];
    const int nb = openpete_keymap_below_saved(a, below, KM_MAX_SOURCES, origin, sizeof origin);
    char text[256];
    size_t used = (size_t)snprintf(text, sizeof text, "Restore the %s binding: ",
                                   reset_layer_word(origin));
    for (int i = 0; i < nb && used < sizeof text; i++) {
        char label[80];
        openpete_keymap_source_label(&below[i], label, sizeof label);
        used += (size_t)snprintf(text + used, sizeof text - used, "%s%s", i ? " / " : "", label);
    }
    if (nb <= 0 && used < sizeof text) snprintf(text + used, sizeof text - used, "unbound");
    ImGui::SetTooltip("%s", text);
}

/** @brief Draw one action's row: its purpose, a button per source (replace) with a remove button beside it, "+" (add), "+ hold" on a host button action, and Reset.
 *
 * Every button posts a request to the binding page's queue
 * (openpete_rebind.h); the host loop applies it. */
// host-control: exempt: key bindings
static void draw_binding_row(openpete_action_t a, const openpete_keymap_action_info_t &info)
{
    km_source_t src[KM_MAX_SOURCES];
    const int n = openpete_keymap_sources(a, src, KM_MAX_SOURCES);
    ImGui::PushID((int)a);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(info.purpose);
    if (tooltip_hovered()) {
        tooltip_begin();
        ImGui::Text("%s.%s", openpete_keymap_domain_name(a), info.id);
        tooltip_end();
    }
    ImGui::TableNextColumn();
    draw_source_buttons(a, src, n);
    ImGui::BeginDisabled(n >= KM_MAX_SOURCES);
    if (ImGui::Button("+")) openpete_rebind_request_capture((int)a, -1, 0);
    if (tooltip_hovered()) ImGui::SetTooltip("Add an input");
    if (info.domain == KM_DOM_HOST && info.kind == KM_DIGITAL) {
        ImGui::SameLine();
        if (ImGui::Button("+ hold")) openpete_rebind_request_capture((int)a, -1, 1);
        if (tooltip_hovered())
            ImGui::SetTooltip("Add an input that acts only after a half-second hold; "
                              "a controller button the game uses keeps its game role");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool saved = openpete_keymap_has_saved(a) != 0;
    ImGui::BeginDisabled(!saved);
    if (ImGui::Button("Reset")) openpete_rebind_request_restore((int)a);
    ImGui::EndDisabled();
    if (saved && tooltip_hovered()) reset_tooltip(a);
    ImGui::PopID();
}

/** @brief Draw one group of rows (a domain, and for host actions one context) as a two-column table. */
static void draw_binding_group(const ImGuiTextFilter &filter, const char *table_id, int first,
                               int last, int ctx)
{
    if (!ImGui::BeginTable(table_id, 2, ImGuiTableFlags_SizingFixedFit)) return;
    for (int a = first; a < last; a++) {
        openpete_keymap_action_info_t info;
        if (!openpete_keymap_action_info((openpete_action_t)a, &info)) continue;
        if (ctx >= 0 && info.ctx != ctx) continue;
        if (!controls_row_listed((openpete_action_t)a, info)) continue;
        if (filter.IsActive() && !filter.PassFilter(info.purpose) && !filter.PassFilter(info.id))
            continue;
        draw_binding_row((openpete_action_t)a, info);
    }
    ImGui::EndTable();
}

/** @brief Draw the Controls section: player 1's PlayStation pad, then the host hotkeys grouped by the context that reads them.
 *
 * The fill-tag inspector's actions are left out: they stay bindable in the
 * config file. The last edit's message (committed, refused or cancelled)
 * shows under the header. */
static void draw_controls_section(const ImGuiTextFilter &filter)
{
    if (filter.IsActive()) ImGui::SetNextItemOpen(true);
    if (!ImGui::CollapsingHeader("Controls")) return;
    char msg[224];
    (void)openpete_rebind_last_message(msg, sizeof msg);
    if (msg[0]) ImGui::TextWrapped("%s", msg);

    ImGui::SeparatorText("Player 1");
    ImGui::TextDisabled("A controller change here re-maps every connected gamepad that "
                        "has no [keys.padN] rows of its own.");
    draw_binding_group(filter, "##pad", OPK_PAD_CROSS, OPK_PAD2_CROSS, -1);

    ImGui::SeparatorText("Hotkeys");
    static const struct { int ctx; const char *label; } groups[] = {
        { KM_CTX_ALWAYS,   "Anywhere" },
        { KM_CTX_GAMEPLAY, "During play" },
        { KM_CTX_MENU,     "Settings menu" },
        { KM_CTX_FREECAM,  "Free camera" },
        { KM_CTX_DEVUI,    "This overlay" },
    };
    for (const auto &g : groups) {
        ImGui::TextDisabled("%s", g.label);
        char id[32];
        snprintf(id, sizeof id, "##host%d", g.ctx);
        draw_binding_group(filter, id, 0, OPK_PAD_CROSS, g.ctx);
    }
}

/** @brief Draw the capture prompt while a capture runs and publish its rect; publish an empty rect otherwise.
 *
 * A mouse press inside the published rect is a candidate and one outside it
 * cancels (openpete_keymap_capture_set_rect). */
static void draw_capture_prompt(void)
{
    const unsigned w = openpete_keymap_capture_word();
    const int state = KM_CAPWORD_STATE(w);
    if (state == KM_CAP_IDLE) {
        openpete_keymap_capture_set_rect(0, 0, 0, 0);
        return;
    }
    openpete_keymap_action_info_t info;
    if (!openpete_keymap_action_info((openpete_action_t)KM_CAPWORD_ACTION(w), &info)) return;
    const ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();
    if (ImGui::Begin("Press an input", nullptr,
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("%s%s", info.purpose, KM_CAPWORD_HOLD(w) ? " (hold)" : "");
        ImGui::Separator();
        ImGui::TextUnformatted(state == KM_CAP_WAITING ? "Release every key and button..."
                               : state == KM_CAP_CANDIDATE ? "Release to assign"
                               : "Press a key, a controller button, or a mouse button here");
        if (info.kind == KM_AXIS)
            ImGui::TextUnformatted("For a stick or the mouse, move it in the positive direction.");
        ImGui::TextDisabled("Esc, a click outside this box, or 5 s without input cancels.");
        const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
        openpete_keymap_capture_set_rect(p.x, p.y, p.x + sz.x, p.y + sz.y);
    }
    ImGui::End();
}

/** @brief Draw the layout sections of one tab in table order; the Settings tab then draws each registry section no LAY_REST entry names. */
static void draw_tab_sections(const lay_ctx &c, lay_tab tab)
{
    for (const lay_section &s : k_layout) {
        if (s.tab != tab) continue;
        switch (s.kind) {
        case LAY_SEC_MODS:     draw_mods_section(*c.filter); break;
        case LAY_SEC_CONTROLS: draw_controls_section(*c.filter); break;
        default:               lay_draw_section(c, s.title, s.open, s.rows, s.n); break;
        }
    }
    if (tab == LAY_TAB_SETTINGS) lay_draw_unplaced(c);
}

/** @brief Draw the tab bar, then the selected tab's sections below it.
 *
 * The tab bar ends before the sections draw, because a mod's UI body runs
 * inside an error-recovery bracket that ends every tab bar open in the
 * window. imgui keeps the selected tab for the session; a SetSelected tab
 * draws from the next frame on.
 * @param pick The tab to select, or LAY_TAB_COUNT to keep the selection.
 * @return The tab drawn this frame, or LAY_TAB_COUNT when none was. */
static lay_tab draw_tabs(const lay_ctx &c, lay_tab pick)
{
    lay_tab drawn = LAY_TAB_COUNT;
    if (!ImGui::BeginTabBar("##tabs")) return drawn;
    for (int t = 0; t < LAY_TAB_COUNT; t++) {
        const ImGuiTabItemFlags fl = (t == pick) ? ImGuiTabItemFlags_SetSelected : 0;
        if (!ImGui::BeginTabItem(k_lay_tab_names[t], nullptr, fl)) continue;
        drawn = (lay_tab)t;
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    if (drawn != LAY_TAB_COUNT) draw_tab_sections(c, drawn);
    return drawn;
}

/** @brief Draw the "openpete config" window: search field and Save button, then the Settings and Mods tabs.
 *
 * The search field filters the selected tab. A scroll-to-top request
 * (openpete_devui_open_top) selects the Settings tab and scrolls to the
 * top; the boot reveal selects the Mods tab when any mod is loaded. Gives
 * the window navigation focus on the first frame after an open, clears the
 * open flag when its close button is pressed, and clears the boot reveal
 * once the frame drew what it reveals. */
static void draw_panel(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(24, 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(540, io.DisplaySize.y - 96), ImGuiCond_FirstUseEver);
    const bool to_top = SDL_GetAtomicInt(&s_top_req) != 0;
    if (s_boot_reveal || to_top) ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
    if (s_focus_panel) {
        ImGui::SetNextWindowFocus();
        s_focus_panel = false;
    }

    /* Only the title bar's close button writes the flag: copying it back
     * whole would undo a close the host loop or the pump applied while
     * this frame was being recorded. */
    bool open = true;
    if (!ImGui::Begin("openpete config", &open)) {
        ImGui::End();
        draw_capture_prompt();
        if (!open) s_open = false;
        return;
    }

    lay_tab pick = LAY_TAB_COUNT;
    if (s_boot_reveal && openpete_mods_count() > 0) pick = LAY_TAB_MODS;
    if (to_top) {
        SDL_SetAtomicInt(&s_top_req, 0);
        ImGui::SetScrollY(0.0f);
        pick = LAY_TAB_SETTINGS;
    }

    /* The tab drawn last frame names the field's hint. */
    static lay_tab s_tab = LAY_TAB_SETTINGS;
    static ImGuiTextFilter s_filter;
    draw_search(s_filter, s_tab == LAY_TAB_MODS ? "Search mods" : "Search settings");

    lay_ctx c;
    c.ks = knob_registry(&c.n);
    c.filter = &s_filter;
    lay_claim_refresh(c.ks, c.n);
    title_edge_roll();

    const lay_tab drawn = draw_tabs(c, pick);
    if (drawn != LAY_TAB_COUNT) s_tab = drawn;

    ImGui::End();
    draw_capture_prompt();
    if (!open) s_open = false;
    /* A tab selected by SetSelected draws from the next frame on, so the
     * reveal holds until the Mods tab has drawn its mod nodes open. */
    if (pick != LAY_TAB_MODS || drawn == LAY_TAB_MODS) s_boot_reveal = false;
}

/* ------------------------------------------------------------------ */
/* LP fill-tag inspector (P) - see openpete_fillpick.h                 */
/* ------------------------------------------------------------------ */

/** @brief Draw the fill-tag inspector's crosshair at the window centre, the pick point.
 *
 * The terrain walk picks at the projection centre (OFX/OFY), which is the
 * optical axis at every aspect, so the window centre is the pick point.
 * Drawn on the foreground list, yellow over a 1-px black shadow, so it sits
 * over the scene without a window. */
static void draw_fillpick_crosshair(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float cx = io.DisplaySize.x * 0.5f, cy = io.DisplaySize.y * 0.5f;
    const float r0 = 4.0f, r1 = 14.0f;
    const ImU32 col = IM_COL32(255, 255, 0, 220);
    const ImU32 shadow = IM_COL32(0, 0, 0, 200);
    for (int pass = 0; pass < 2; pass++) {
        ImU32 c = pass ? col : shadow;
        float o = pass ? 0.0f : 1.0f;
        dl->AddLine(ImVec2(cx - r1 + o, cy + o), ImVec2(cx - r0 + o, cy + o), c, 1.5f);
        dl->AddLine(ImVec2(cx + r0 + o, cy + o), ImVec2(cx + r1 + o, cy + o), c, 1.5f);
        dl->AddLine(ImVec2(cx + o, cy - r1 + o), ImVec2(cx + o, cy - r0 + o), c, 1.5f);
        dl->AddLine(ImVec2(cx + o, cy + r0 + o), ImVec2(cx + o, cy + r1 + o), c, 1.5f);
    }
}

/** @brief Name a fill-tag verdict: "FILL", "NOFILL", or "clear" for any other value. */
static const char *fp_verdict_word(int v)
{
    return v == OPENPETE_FILLPICK_FILL   ? "FILL"
         : v == OPENPETE_FILLPICK_NOFILL ? "NOFILL"
                                         : "clear";
}

/** @brief Draw the fill-tag inspector window over the picked face and the session verdicts.
 *
 * Shows the level, sector and face under the crosshair, the current verdict
 * and whether it is a session override, and the LP view-Z. When a
 * high-poly face lies on the same sightline it reports whether that face is
 * behind or in front of the LP. The FILL, NOFILL and clear buttons mark the
 * picked face; undo removes the last session verdict; the child list shows
 * the session's verdicts newest first. */
static void draw_fillpick_panel(void)
{
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 400.0f, 24.0f),
                            ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(376, 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("fill-tag inspector (P)", nullptr,
                      ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus)) {
        ImGui::End();
        return;
    }

    // host-control: fillpick_preview
    row_checkbox("fillpick_preview", "preview untagged LP candidates (H)");
    ImGui::TextDisabled("green = listed FILL   magenta = candidate   yellow = picked");
    ImGui::Separator();

    openpete_fillpick_hit h;
    if (!openpete_fillpick_get(&h)) {
        ImGui::TextDisabled("no terrain face under the crosshair");
    } else {
        ImGui::Text("%s  sector %d  face %d%s", h.level_name, h.sec, h.face,
                    h.quad ? "  (quad)" : "  (tri)");
        ImGui::Text("verdict: %s%s", fp_verdict_word(h.verdict),
                    h.overridden ? "   [session]" : "   [frozen list]");
        ImGui::Text("LP view-Z at crosshair: %.0f", h.vz);
        /* The FILL question in one line: FILL means og draws this LP over a
         * sightline no authored HP covers, so "no HP here" argues for it and
         * "HP already covers, nearer or behind" argues against. */
        if (!h.hp_valid) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f),
                               "no HP on this sightline -> LP is the only cover");
        } else if (h.hp_vz > h.vz) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                               "HP s%d/f%d is %.0f BEHIND the LP -> LP punches through",
                               h.hp_sec, h.hp_face, h.hp_vz - h.vz);
        } else {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                               "HP s%d/f%d is %.0f in front -> LP is hidden anyway",
                               h.hp_sec, h.hp_face, h.vz - h.hp_vz);
        }
        ImGui::Separator();
        (void)row_button("fillpick_fill", "FILL (Y)");       // host-control: fillpick_fill
        ImGui::SameLine();
        (void)row_button("fillpick_nofill", "NOFILL (N)");   // host-control: fillpick_nofill
        ImGui::SameLine();
        (void)row_button("fillpick_none", "clear (B)");      // host-control: fillpick_none
    }

    ImGui::Separator();
    int n = openpete_fillpick_count();
    ImGui::Text("session: %d verdict(s)", n);
    ImGui::SameLine();
    (void)row_button("fillpick_undo", "undo (U)");           // host-control: fillpick_undo
    ImGui::TextDisabled("%s", openpete_fillpick_session_path());
    if (ImGui::BeginChild("##fpsession", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        for (int i = n - 1; i >= 0; i--) {
            const char *name = "?";
            int sec = 0, face = 0, verdict = OPENPETE_FILLPICK_NONE;
            if (!openpete_fillpick_entry(i, nullptr, &name, &sec, &face, &verdict))
                continue;
            ImGui::Text("%-6s %s %d %d", fp_verdict_word(verdict), name, sec, face);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Always-on mod sections                                             */
/* ------------------------------------------------------------------ */

/** @brief Draw every enabled mod's OPENPETE_MOD_UI_ALWAYS body at top level on this present.
 *
 * Top level and not the Mods panel's tree node: a node the user collapsed
 * must not make a mod's window disappear, and the panel does not exist at
 * all while the overlay is closed. Skipped while the settings menu is open. The menu and the overlay are
 * mutually exclusive input owners, and both compose branches draw the menu
 * before this frame is recorded, so an always window would paint over it.
 * With the overlay closed the bodies' windows take no input. */
static void draw_always_sections(void)
{
    if (openpete_menu_is_open()) return;
    const int n = openpete_mods_count();
    for (int i = 0; i < n; i++) {
        openpete_mods_menu_info_t mi;
        if (!openpete_mods_menu_info(i, &mi)) continue;
        if (!mi.enabled || !mi.ui_always) continue;
        ImGui::PushID(mi.id);
        draw_mod_section_bracketed(i, mi.id, true, !s_open);
        ImGui::PopID();
    }
}

int openpete_devui_frame_wanted(void)
{
    if (!s_inited) return 0;
    if (s_open || note_active()) return 1;
    if (openpete_fillpick_active()) return 1;
    return openpete_mods_ui_always_count() > 0 ? 1 : 0;
}

void openpete_devui_render(struct rhi_device *dev)
{
    /* The inspector's crosshair + panel ride the same imgui frame as the
     * config overlay, but must show with the overlay closed (aiming a camera
     * behind a 540px panel is not the workflow). */
    if (!s_inited) return;

    bool fillpick = openpete_fillpick_active() != 0;
    const int always_n = openpete_mods_ui_always_count();
    const bool note = note_active();
    const bool frame = s_open || note || fillpick || always_n > 0;
    if (s_open && !s_frame_prev_open) devui_reset_on_open();
    s_frame_prev_open = s_open;
    /* The prompt's first frame starts from no key held, as the overlay's
     * open does, so the key that pressed the report types nothing. */
    static bool s_note_prev = false;
    if (note && !s_note_prev) {
        ImGui::GetIO().ClearInputKeys();
        ImGui::ClearActiveID();
    }
    s_note_prev = note;
    /* Drain every present, open or not, so the ring never fills and drops;
     * key events with no frame to consume them are discarded (see
     * devui_drain_events), mouse events keep the pointer current. */
    devui_drain_events(frame);

    if (!frame) { devui_publish_none(); return; }
    if (dev != s_dev) { devui_publish_none(); return; }
    void *cb = rhi_current_command_buffer(dev);
    if (!cb) { devui_publish_none(); return; }

    /* First open: adopt late-settling boot values (render-fps pacer). An
     * always section runs the frame with the overlay closed, so the stamp
     * waits for the open that gives it its meaning. */
    if (s_open) {
        static bool s_late_stamped = false;
        if (!s_late_stamped) { knob_registry_late_stamp(); s_late_stamped = true; }
    }

    ImGuiIO &io = ImGui::GetIO();
    /* The SDL3 backend re-applies the cursor shape and calls SDL_ShowCursor
     * on every NewFrame unless this flag is set. With an always section the
     * frame runs every present, which would undo the host loop's idle
     * cursor hide. Decided per present from the open state rather than on
     * its edges, so the first frame that draws the panel is the frame that
     * gets imgui's cursors back. */
    if (s_open) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else        io.ConfigFlags |=  ImGuiConfigFlags_NoMouseCursorChange;

    devui_feed_padnav(io);
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    /* The backend clears HasGamepad on every NewFrame because it holds no
     * pad (manual mode, empty list); imgui navigates with the gamepad only
     * while the flag is set, so it is set after the backend's update. */
    if (s_open && (SDL_GetAtomicInt(&s_padnav_bits) & PADNAV_HAVE_PAD))
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    /* After the backend's own IO update, which would otherwise overwrite
     * the parked position with the real pointer. */
    if (s_inject_mouse) {
        io.AddMousePosEvent(s_inject_x, s_inject_y);
    } else if (openpete_host_mouse_inject_active()) {
        /* A scripted run: the backend just read SDL's pointer, which a
         * pushed event never moves; follow the channel's event-derived
         * position so the script's hover reaches these widgets. */
        openpete_mouse_state_t st;
        openpete_host_input_mouse(&st);
        io.AddMousePosEvent(st.px, st.py);
    } else if (!openpete_host_mouse_accepting()) {
        /* The backend's NewFrame polls the global pointer position when no
         * window of ours is hovered, which bypasses the event stream the
         * mouse switch and the input lock filter. Report the pointer as
         * absent so nothing under it hovers or activates. */
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    }
    ImGui::NewFrame();
    devui_count_padnav();
    if (s_open) draw_panel();
    else        openpete_keymap_capture_set_rect(0, 0, 0, 0);   /* no prompt drawn */
    if (always_n > 0) draw_always_sections();
    if (fillpick) { draw_fillpick_crosshair(); draw_fillpick_panel(); }
    if (note) draw_note_prompt();
    ImGui::Render();
    devui_save_ini(false);
    devui_publish_io_wants();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cb);
}

int openpete_devui_render_frame(struct rhi_device *dev, void (*draw)(void *ud), void *ud)
{
    return openpete_devui_render_frame_under(dev, draw, ud, nullptr, nullptr);
}

int openpete_devui_render_frame_under(struct rhi_device *dev, void (*draw)(void *ud), void *ud,
                                      void (*under)(void *under_ud), void *under_ud)
{
    if (!s_ctx || dev != s_dev || !draw) return 0;
    void *cb = rhi_current_command_buffer(dev);
    if (!cb) return 0;
    devui_drain_events(true);
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    draw(ud);
    ImGui::Render();
    if (under) under(under_ud);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cb);
    return 1;
}

