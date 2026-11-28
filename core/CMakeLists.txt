# core/CMakeLists.txt - game-agnostic OpenPete runtime.
#
# Builds a static lib `openpete_core` consumed by per-game targets under
# games/<game>/. Anything Spyro-specific belongs in games/spyro-1/, not
# here.

add_library(openpete_core STATIC
    # Diagnostics: the log and its file mirror, the crash handler, the
    # pacing event log (--pace-log PATH) with the always-on pacing census,
    # the per-run perf summary, the CPU+GPU performance trace
    # (--profile-out PATH), the per-function tracing hooks (fire only when
    # a TU is built with -finstrument-functions, -DOPENPETE_FUNC_TRACE),
    # the sub-tick spine identity audit (OPENPETE_IDENT_AUDIT: composes
    # the canonical scene and the alpha-1 re-extract of the same tick and
    # reports their pixel difference), the per-present frame-time recorder
    # (--frame-time-log PATH), the psxtrace schedule and verifier, the
    # host disc-read trace (OPENPETE_DISC_TRACE PATH), and the tile census
    # (per-capture tallies of the presented scene and the sidecar beside
    # each tile, OPENPETE_TILE_CENSUS; counter registry tile_census.def).
    diag/log.c
    diag/disc_trace.c
    diag/log_file.c
    diag/crash.c
    diag/fatal_message.c
    diag/error_ids.c
    diag/pace_log.c
    diag/perf_stats.c
    diag/boot_timing.c
    diag/profile.c
    diag/func_trace.c
    diag/ident_audit.c
    diag/tile_census.c
    diag/openpete_frame_time_log.c
    diag/psxtrace_ratchet.c
    diag/psxtrace_schedule.c
    diag/psxtrace_verify.c

    # Configuration: the command-line flag parser, openpete.toml (the
    # layered config file, resolved into an argv splice and a setenv pass;
    # config.c is SDL-free so the resolver can be unit-tested standalone in
    # tests/unit/config_resolve_test.c), the knob registry behind the M-key
    # overlay, and the data-directory layout (the game folder for a
    # portable install, the platform's per-user directories otherwise;
    # SDL-free for the same reason config.c is).
    config/options.c
    config/pose_text.c
    config/surface_dump.c
    config/tooltip_vocab.c
    config/config.c
    config/config_schema.c
    config/identify.c
    config/game_registry.c
    config/knob_registry.c
    config/settings_save.c
    config/paths.c
    config/build_id.c

    # Host input.
    # [keys]: the input binding registry. Split the same way and for the
    # same reason as config.c - keymap.c holds the registry, the layered
    # store and the edge latches with no SDL in sight (so it links into
    # tests/unit/keymap_test.c), and keymap_sdl.c is the SDL glue that
    # turns binding names into codes and reads whether they are held.
    input/keymap.c
    input/keymap_sdl.c
    # The binding page's request queue and the side effects of an edit
    # (toasts, log lines, save marks, the pad restamp generation), drained
    # on the host loop. See openpete_rebind.h.
    input/rebind.c
    # Host-owned pad decode: producer publishes an SDL-decoded snapshot, the
    # sim-side consumer arbitrates it against the other override writers and
    # stamps the PsyCross override channel. Split producer/consumer because
    # the pump and the sim run on different threads. See
    # openpete_host_input.h.
    input/host_input.c
    # The host mouse channel: event-derived state, running totals, ownership
    # arbitration, capture requests and the injection seam. See
    # openpete_host_mouse.h.
    input/host_mouse.c
    # The host input lock: discards physical-device events at the pump and
    # blanks the polled snapshot. See openpete_input_lock.h.
    input/input_lock.c
    # Controller vibration. rumble_response.c is the actuator response (a
    # cubic fitted to the measured table) and rumble_motor.c its --vibration
    # scaling to SDL's motor range, both SDL-free so the unit tests link them
    # alone; host_rumble.c is the registration / per-tick sample / per-pump
    # apply seam.
    input/rumble_response.c
    input/rumble_motor.c
    input/stick_response.c
    input/host_rumble.c
    # Per-VBL input log - single source of truth for "what was pressed at
    # VBL i"; shared by bk2_record and the rewind/runahead drivers. See
    # openpete_input_log.h.
    input/input_log.c
    renderer/subtick_view.c
    renderer/screenshot.c
    runtime/run.c
    runtime/host_state_ops.c
    runtime/runahead.c
    runtime/host_loop.c
    runtime/host_present.c
    runtime/host_hotkeys.c
    runtime/host_control.c
    runtime/eager.c
    runtime/host_session.c
    runtime/host_bugreport.c
    runtime/host_bugknobs.c
    runtime/bugknobs_class.c
    runtime/host_pacer.c
    runtime/host_input_tick.c
    runtime/host_trace.c
    runtime/run_capture.c
    runtime/skip_common.c

    # Recording and savestates: true-VFR gameplay video capture (--record;
    # links libav* in-process and reads the present FBO so output is
    # bars-free at internal resolution), BizHawk .bk2 movie replay (--bk2)
    # and record (--bk2-record), save states (quick save/load keys), the
    # rewind keyframe ring (--rewind N seconds; Backspace scrubs back;
    # driver in run.c, see openpete_rewind.h), and the guest-state
    # discontinuity broadcast (host caches derived from guest RAM register
    # resync actions fired after a savestate load; see
    # openpete_discontinuity.h).
    record/record.c
    record/bk2_movie.c
    record/bk2_record.c
    record/bk2_feed.c
    record/bugrec.c
    record/bugrec_input.c
    record/bugrec_subtick.c
    record/bugreport_redact.c
    record/bugreport_pass.c
    record/bugreport_replay.c
    record/bugreport_replay_knobs.c
    record/bugreport_write.c
    record/bugreport_thumbs.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c
    record/savestate.c
    record/rewind.c
    record/rewind_pages.c
    record/discontinuity.c

    # The PSX machine model: RAM and the CPUState bindings, the EXE boot
    # sequence, dispatch glue and the hang watchdog, the BIOS model, the
    # CD-ROM controller, the disc cache, the EXE loader, the RAM
    # virtualization region table, the moby vertex-stream resolver and the
    # cooperative scheduler globals.
    psx/cpu.c
    psx/boot.c
    psx/dispatch.c
    psx/psx_bios.c
    psx/psx_bios_kernel_seed.c
    psx/cdrom_mmio.c
    psx/disc_cache.c
    psx/disc_fs_probe.c
    psx/disc_image.c
    psx/disc_build.c
    psx/disc_build_boot.c
    psx/exe_loader.cpp
    psx/psx_region.c
    psx/vstream.c
    psx/fiber.c
    psx/gte.c
    psx/gte_instruments.cpp

    renderer/openpete_ui_moby.c

    # The launcher common to every game: the disc library (ingest, entries
    # per game, the [disc.*] identity) and the window, Vulkan device and
    # imgui screens (drop, version selector, progress) it hands the game.
    launcher/library.cpp
    launcher/launcher.cpp
    launcher/launcher_quit.cpp
    launcher/launcher_manifest.cpp
    launcher/launcher_pace.cpp
    launcher/launcher_page.cpp
    launcher/launcher_image.cpp
    launcher/launcher_panel.cpp
    launcher/launcher_font.cpp
    launcher/launcher_title.cpp
    launcher/launcher_commands.cpp
    launcher/launcher_shapes.cpp
    launcher/launcher_chips.cpp
    launcher/launcher_return.cpp
    launcher/launcher_progress.cpp
    launcher/launcher_sky.cpp
    launcher/launcher_picker.cpp

    # Debug and in-game UI: the O settings menu; the placement ledger for
    # on-screen chrome (six anchors, a usable-x range that dodges the open
    # settings panel, a per-anchor stacking cursor so two surfaces never
    # claim one corner; see openpete_overlay_anchor.h); OSD toast
    # notifications (the ring, coalescing, rate limit and animation state
    # machine on an injected clock; drawing lives in native_render.c and
    # text_moby.c; see openpete_osd.h); the M-key dev config overlay (the
    # ImGui panel over the knob registry, C++ TU, flat C API in
    # openpete_devui.h; native-vk-only, live presents only); the G camlock
    # and F freecam tools.
    ui/openpete_menu.c
    ui/overlay_anchor.c
    ui/openpete_osd.c
    ui/devui.cpp
    ui/camlock.c
    ui/freecam.c

    renderer/fillpick.c
    runtime/feature_registry.c
    runtime/game_runtime.c

    # Modding framework: per-address override chains riding the
    # psx_override_dispatch prologue chokepoint, the compile-on-load mod
    # loader (mod.toml manifests via vendored tomlc99, POSIX dlopen), and
    # the mod install's checkpoint, exec and restore path (handoff.c).
    mods/mod_vec.c
    mods/mod_bus.c
    mods/mod_svc.c
    mods/mod_chain.c
    mods/mod_save.c
    mods/handoff.c
    mods/mod_loader.c
    mods/mod_games.c
    mods/mod_modset.c
    mods/mod_draw.c
    mods/mod_services.c
    mods/mod_registry.c
    mods/mod_arena.c
    mods/arena_page_census.c
    mods/mod_sfx.c
    mods/mod_texture.c
    mods/mod_config.c
    mods/mod_glsl.c
    mods/mod_spv_cdata.c
    mods/mod_spv_reflect.c
    mods/mod_mat_link.c
    mods/mod_shader.c
    mods/mod_shader_stamp.c
    mods/mod_material_resolve.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c

    # High-resolution monotonic clock + hybrid sleep+spin waiter. Leaf
    # module (no SDL); used by ticker + dual-pacer.
    runtime/openpete_time.c
    # Thread-role masks, check sites and seals (openpete_thread_roles.h);
    # SDL-free like the clock, which seals its init group through it.
    runtime/thread_roles.c

    runtime/present_thread.c
    renderer/native_terrain_cache.c
    # OPENPETE_MOBY_GSTAGE_AB: the moby vertex stage's readback against the
    # CPU projection (diagnostic).
    renderer/native_moby_gstage.c
    renderer/launcher_preview_gpu.c

    # Render-side state interpolation snapshot ring (game-agnostic;
    # regions supplied by games/<game>/). Capture-only in v1; lerp +
    # re-emission is the sub-tick re-feed below.
    renderer/openpete_interp.c

    # Per-object prev-state snapshot table (the INDIRECT counterpart to the
    # singleton interp ring above). Keyed by object identity (game-supplied
    # walk); double-buffered for sub-tick lerp/re-feed.
    renderer/openpete_interp_objs.c

    # World-space render cache. Populated per-vertex by psxrecomp's
    # gte_rtps_internal; consumed by PsyCross's OT walker (PsyX_GPU.cpp
    # MakeVertexTriangle/Quad) to recover world-space + matrix state at
    # draw time. Foundation for draw-call matching and renderer-side lerp.

    # Draw-call matcher. Groups OT prims by matrix_span_id, hashes each
    # batch by geometric fingerprint, matches curr-frame batches against
    # prev-frame. Produces stable object_ids for a renderer-side lerp to
    # use as identity for T/R/S decompose + interpolate. Changes no
    # rendering.

    # Per-batch binary trace for the substitute pipeline. Off unless
    # --vertex-trace-log PATH is given. Consumed by
    # scripts/analyze-vtrace.py.

    # Native Vulkan renderer. Host-side display consumer built on core/rhi;
    # never writes PSX RAM.
    renderer/native_render.c
    renderer/render_res.c
    renderer/launcher_title_pass.c
    renderer/launcher_shape_pass.c
    renderer/launcher_sky_pass.c
    renderer/native_vram.c
    renderer/native_fb_snapshot.c

    # Per-feature modules.
    renderer/no_lod.c
    renderer/widescreen.c

    # Music WAV override: play user PCM WAVs from assets/music/ in place
    # of XA music.
    host/host_spawn.c
    host/window_restack.c
    host/audio_consumer.c
    host/audio_feed_report.c
    host/music_override.c
    host/mod_sound.c
    host/sha256.c
    host/sfx_ident.c
    host/sfx_pack.c
    host/spu_adpcm.c
    host/texture_pack.c
    host/stb_image_impl.c
    host/wav_loader.c

    # Temp-file-and-rename replacement shared by every persisted file.
    host/atomic_file.c
    # Host-backed PSX memory card (save/load).
    host/memcard.c
    # The game window's icon, decoded from the save-file header on the
    # player's disc (openpete_save_icon.h).
    host/save_icon.c

    # Data-parallel extraction worker pool (--extract-threads). See
    # openpete_workpool.h for the threading contracts.
    runtime/work_pool.c

    # Pacing-clock lifecycle + the fast-forward flag. The host loop paces
    # itself against op_sleep_until deadlines; there is no pacing thread.
    runtime/ticker.c

    # --debug-triangle infra-triangle: hardcoded POLY_F3 emit to validate
    # the full PsyCross render pipeline without depending on
    # overlay-resident Spyro code. C++ TU because PsyCross's libgpu.h uses
    # static_assert.
    renderer/debug_render.cpp
)

# The OpenPete mark as RGBA8 (openpete_mark_icon): the window icon before a
# game is picked and whenever the disc's save header cannot be read.
# tools/gen_dist_icon.py draws it; it holds no game bytes.
set(_mark_icon_c ${CMAKE_BINARY_DIR}/gen/openpete_mark_icon.c)
add_custom_command(
    OUTPUT  ${_mark_icon_c}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_dist_icon.py
            --c ${_mark_icon_c} --c-name openpete_mark_icon
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/gen_dist_icon.py
    COMMENT "Generating openpete_mark_icon.c (gen_dist_icon.py)"
    VERBATIM)
target_sources(openpete_core PRIVATE ${_mark_icon_c})

# libco: cooperative single-fiber scheduler. third_party/libco/libco.c
# `#include`s a per-arch sibling (amd64.c on x86_64 Linux/macOS, fiber.c
# under MinGW); these are the real implementations, with no shim.
target_sources(openpete_core PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/libco/libco.c)

# LIBCO_MP is REQUIRED, not an optimisation. libco/settings.h defines
# `thread_local` as NOTHING unless it is set:
#
#     #if !defined(LIBCO_MP)   /* Running in single-threaded environment */
#       #define thread_local
#
# so co_active_ (fiber.c) and co_active_handle (amd64.c) silently become
# plain globals shared by every thread. The failure that produces is not a
# subtle race: the first thread to call co_active() converts itself and
# stores its handle, and every LATER thread then sees a non-NULL handle,
# skips its own ConvertThreadToFiber, and calls SwitchToFiber while not
# being a fiber at all - an access violation inside the OS fiber
# bookkeeping. Reading the declaration is not enough to rule this out; the
# variables ARE spelled thread_local, the macro is just empty.
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/third_party/libco/libco.c
    PROPERTIES COMPILE_DEFINITIONS LIBCO_MP)

target_include_directories(openpete_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/host
        ${CMAKE_CURRENT_SOURCE_DIR}/include/psxrt
        ${CMAKE_SOURCE_DIR}/third_party/libco
    PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${CMAKE_SOURCE_DIR}/third_party/xxhash
        ${CMAKE_SOURCE_DIR}/third_party/miniz
)

# OpenAL for the music WAV override's streaming source. Same discovery
# PsyCross uses (the MinGW preset seeds the OPENAL_* cache vars; native
# Linux finds the system OpenAL).
find_package(OpenAL REQUIRED)
target_link_libraries(openpete_core PUBLIC ${OPENAL_LIBRARY})
target_include_directories(openpete_core PRIVATE ${OPENAL_INCLUDE_DIR})

set_target_properties(openpete_core PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Every translation unit linking openpete_core compiles with
# OPENPETE_ENGINE_BUILD: openpete_mod_api.h then skips the
# per-game openpete_game.h, which only a mod's compile line puts on the
# include path, so no engine TU reads a game identity macro through it.
target_compile_definitions(openpete_core PUBLIC OPENPETE_ENGINE_BUILD)

# SDL3 is required for window/render/screenshot. Built from the
# third_party/SDL submodule (see top-level CMakeLists.txt).
target_link_libraries(openpete_core PUBLIC SDL3::SDL3-static)

# In-process glslang for the mod shader compile service (mod_shader.c).
# Include the repo root (for <glslang/Include/glslang_c_interface.h> and
# <glslang/Public/resource_limits_c.h>) and the generated build_info.h.
if(OPENPETE_BUILD_GLSLANG)
    target_link_libraries(openpete_core PUBLIC
        glslang glslang-default-resource-limits)
    target_include_directories(openpete_core PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party)
    target_include_directories(openpete_core SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/third_party/glslang/include)
else()
    target_compile_definitions(openpete_core PRIVATE OPENPETE_NO_GLSLANG)
endif()

# Build identity for the pacing log's environment block, and the version the
# player sees. --abbrev=6 fixes the commit suffix at six hex characters, for
# the tag-relative form and for the --always fallback alike, so the string has
# a known width. Baselines are only
# comparable within a matching environment, and "which build produced this"
# is the field that decides it. Resolved at configure time, so a rebuild
# without a reconfigure reports the revision the tree was configured at --
# which is why pacing baselines carry a capture timestamp as well.
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --always --dirty --tags --abbrev=6
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OPENPETE_GIT_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT OPENPETE_GIT_REV)
    set(OPENPETE_GIT_REV "unknown")
endif()
# The bare commit an untagged build's version line shows: nine hex
# characters of HEAD, with no tag distance and no -dirty suffix.
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=9 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE OPENPETE_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()
if(NOT OPENPETE_GIT_COMMIT)
    set(OPENPETE_GIT_COMMIT "unknown")
endif()
# The commit-derived defines reach only the sources that read them, so the
# rest of openpete_core compiles to the same command line at every commit
# and stays a compiler-cache hit across checkouts.
set_property(SOURCE diag/pace_log.c APPEND PROPERTY
    COMPILE_DEFINITIONS OPENPETE_BUILD_ID="${OPENPETE_GIT_REV}")
# The runtime reads and writes guest RAM (uint8_t arrays) through wider
# pointer casts; -fno-strict-aliasing keeps the optimizer from reordering or
# dropping those accesses. The game executables and their recompiled module
# pass the same flag (cmake/openpete_game.cmake).
target_compile_options(openpete_core PRIVATE -fno-strict-aliasing)

# The version the running build SHOWS the player (core/config/build_id.c).
# OPENPETE_DIST_VERSION, cache variable or environment, wins: an artifact
# built from a checkout that cannot see the tag would otherwise display the
# previous release's number. It is the same variable scripts/package-dist.sh
# reads for the artifact filename, so the name on disk and the name on screen
# come from one source.
#
# TAGGED means "this tree is exactly a release": describe --exact-match
# resolves and the working tree is clean. An untagged build keeps its version
# line on screen for the whole session, so a build handed to someone else can
# always be identified from a screenshot. An explicit OPENPETE_DIST_VERSION is
# taken as a release name and counts as tagged.
if(NOT DEFINED OPENPETE_DIST_VERSION AND DEFINED ENV{OPENPETE_DIST_VERSION})
    set(OPENPETE_DIST_VERSION "$ENV{OPENPETE_DIST_VERSION}" CACHE STRING
        "Version string shown on screen and used for dist artifact names")
endif()
if(OPENPETE_DIST_VERSION)
    set(OPENPETE_BUILD_VERSION "${OPENPETE_DIST_VERSION}")
    set(OPENPETE_BUILD_TAGGED 1)
else()
    set(OPENPETE_BUILD_VERSION "${OPENPETE_GIT_REV}")
    set(OPENPETE_BUILD_TAGGED 0)
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE OPENPETE_GIT_EXACT_RC
            OUTPUT_QUIET ERROR_QUIET)
        # --dirty is already in OPENPETE_GIT_REV: a tag on a dirty tree is not
        # the release, so the suffix alone disqualifies it.
        if(OPENPETE_GIT_EXACT_RC EQUAL 0 AND
           NOT OPENPETE_GIT_REV MATCHES "-dirty$")
            set(OPENPETE_BUILD_TAGGED 1)
        endif()
    endif()
endif()
# Whether this is a distribution build - one of the presets that set
# OPENPETE_DIST_PRESET (cmake/presets/*-launcher.cmake), and so the only
# builds that leave this machine. The version line draws on those alone; any
# other build shows nothing. The presets set the variable from
# CMAKE_PROJECT_openpete_INCLUDE, which runs before this directory is added,
# so the value is already resolved here. An explicit
# -DOPENPETE_BUILD_DISTRIBUTED wins, so the gate can be exercised in both
# states without configuring a distribution preset.
if(NOT DEFINED OPENPETE_BUILD_DISTRIBUTED)
    if(OPENPETE_DIST_PRESET)
        set(OPENPETE_BUILD_DISTRIBUTED 1)
    else()
        set(OPENPETE_BUILD_DISTRIBUTED 0)
    endif()
endif()
# The Windows VERSIONINFO resource (games/common/openpete.rc.in) takes the same
# version as four integers: a `vX.Y.Z` or `vX.Y.Z-N-gSHA` describe string
# becomes X,Y,Z,N (N = commits past the tag, 0 on the tag); a version that is
# not in that form (a bare SHA, "unknown") becomes 0,0,0,0. The string fields
# carry OPENPETE_BUILD_VERSION verbatim. Both are exported to the game
# directories, which are siblings of this one.
if(OPENPETE_BUILD_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+)-g)?")
    set(_op_ver_build "${CMAKE_MATCH_5}")
    if(_op_ver_build STREQUAL "")
        set(_op_ver_build 0)
    endif()
    set(OPENPETE_VERSION_NUM
        "${CMAKE_MATCH_1},${CMAKE_MATCH_2},${CMAKE_MATCH_3},${_op_ver_build}")
else()
    set(OPENPETE_VERSION_NUM "0,0,0,0")
endif()
set(OPENPETE_VERSION_NUM "${OPENPETE_VERSION_NUM}" PARENT_SCOPE)
set(OPENPETE_VERSION_STR "${OPENPETE_BUILD_VERSION}" PARENT_SCOPE)
message(STATUS "openpete: VERSIONINFO ${OPENPETE_VERSION_NUM} \"${OPENPETE_BUILD_VERSION}\"")
set_property(SOURCE config/build_id.c APPEND PROPERTY
    COMPILE_DEFINITIONS OPENPETE_BUILD_VERSION="${OPENPETE_BUILD_VERSION}"
                        OPENPETE_BUILD_COMMIT="${OPENPETE_GIT_COMMIT}"
                        OPENPETE_BUILD_TAGGED=${OPENPETE_BUILD_TAGGED}
                        OPENPETE_BUILD_DISTRIBUTED=${OPENPETE_BUILD_DISTRIBUTED})

# Native renderer foundation: the thin RHI + Vulkan backend (core/rhi).
# Always available (added unconditionally by the top-level CMakeLists);
# renderer/native_render.c is the only consumer for now. PUBLIC so the RHI's
# headers (and its embedded SPIR-V) reach any downstream renderer TUs.
target_link_libraries(openpete_core PUBLIC openpete_rhi)

# ImGui (third_party/imgui, pinned release tag) - the M-key dev config
# overlay's UI library (ui/devui.cpp). SDL3 + Vulkan backends; the Vulkan
# backend rides volk's already-loaded function pointers
# (IMGUI_IMPL_VULKAN_USE_VOLK - openpete_rhi runs volkInitialize/volkLoadInstance
# at rhi_create, and volk.c is compiled ONLY there, so no duplicate symbols).
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)

# dear_bindings (third_party/dear_bindings, generated output vendored from the
# release matching the pinned ImGui tag) is the C API mods call. The two must
# describe the same ImGui: a stale dcimgui.h would declare functions the
# library does not define and lay out ImGuiIO differently, so the version is
# compared here rather than discovered at link or at runtime. Refresh with
# scripts/update-dear-bindings.sh.
set(DCIMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/dear_bindings)
file(STRINGS ${DCIMGUI_DIR}/dcimgui.h _dcimgui_ver_line
     REGEX "^#define +IMGUI_VERSION_NUM +[0-9]+")
file(STRINGS ${IMGUI_DIR}/imgui.h _imgui_ver_line
     REGEX "^#define +IMGUI_VERSION_NUM +[0-9]+")
string(REGEX MATCH "[0-9]+" _dcimgui_ver "${_dcimgui_ver_line}")
string(REGEX MATCH "[0-9]+" _imgui_ver "${_imgui_ver_line}")
if(NOT _dcimgui_ver OR NOT _imgui_ver OR NOT _dcimgui_ver STREQUAL _imgui_ver)
    message(FATAL_ERROR
        "ImGui version mismatch: third_party/dear_bindings/dcimgui.h "
        "reports IMGUI_VERSION_NUM '${_dcimgui_ver}', "
        "third_party/imgui/imgui.h reports '${_imgui_ver}'. "
        "Run scripts/update-dear-bindings.sh.")
endif()

# The mod ImGui table: a function-pointer table over the dear_bindings API, generated
# from its JSON metadata. openpete_imgui_table.h and openpete_imgui_macros.h
# are staged into the mod SDK; openpete_imgui_guard.cpp is the engine side,
# one call-site-checking wrapper per entry.
set(_imgui_table_dir ${CMAKE_CURRENT_BINARY_DIR}/imgui_table)
set(_imgui_table_outputs
    ${_imgui_table_dir}/openpete_imgui_table.h
    ${_imgui_table_dir}/openpete_imgui_macros.h
    ${_imgui_table_dir}/openpete_imgui_guard.cpp)
# The two headers are staged into the mod SDK from games/<game>/.
set(OPENPETE_IMGUI_TABLE_DIR ${_imgui_table_dir} CACHE INTERNAL
    "Generated ImGui table headers")
add_custom_command(
    OUTPUT  ${_imgui_table_outputs}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_imgui_table_dir}
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_imgui_table.py
            --json ${DCIMGUI_DIR}/dcimgui.json
            --out-dir ${_imgui_table_dir}
    DEPENDS ${DCIMGUI_DIR}/dcimgui.json
            ${CMAKE_SOURCE_DIR}/tools/gen_imgui_table.py
    COMMENT "Generating the mod ImGui table (gen_imgui_table.py)"
    VERBATIM)
add_custom_target(openpete_imgui_table DEPENDS ${_imgui_table_outputs})

# Every header a build rule generates and a tracked source includes. The
# documentation and complexity ratchets and code_docs parse the tracked
# sources from the compile database, so they depend on this target: before
# the headers exist, a TU that includes one stops at its first missing
# include. Each producer target adds itself here.
add_custom_target(openpete_generated_headers)
add_dependencies(openpete_generated_headers openpete_imgui_table)

add_library(openpete_imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    # imgui_demo.cpp defines the five ImGui::Show* functions dcimgui.cpp
    # references; without it the exe does not link.
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp
    ${DCIMGUI_DIR}/dcimgui.cpp
    ${_imgui_table_dir}/openpete_imgui_guard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/imgui_guard.c
)
add_dependencies(openpete_imgui openpete_imgui_table)
target_include_directories(openpete_imgui PRIVATE
    ${DCIMGUI_DIR}
    ${_imgui_table_dir}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ui)
# The Linux exe links -rdynamic. Without this, a prebuilt mod object built
# against raw dcimgui.h - no table, no version check, no guard - would resolve
# ImGui_Begin to the exe at dlopen and drive the context directly, while the
# same object fails to load on Windows. Hidden on both, so it fails on both.
set_source_files_properties(
    ${DCIMGUI_DIR}/dcimgui.cpp
    ${_imgui_table_dir}/openpete_imgui_guard.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-fvisibility=hidden>")
# volk + vulkan headers are PUBLIC: imgui_impl_vulkan.h #includes volk.h under
# IMGUI_IMPL_VULKAN_USE_VOLK, so any TU that includes the backend header (the
# dev UI) needs the same include paths and defines.
target_include_directories(openpete_imgui SYSTEM PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends)
target_include_directories(openpete_imgui PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
    ${CMAKE_SOURCE_DIR}/third_party/volk)
target_compile_definitions(openpete_imgui PUBLIC
    IMGUI_IMPL_VULKAN_USE_VOLK
    VK_NO_PROTOTYPES
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(openpete_imgui PUBLIC SDL3::SDL3-static)
set_target_properties(openpete_imgui PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON)
target_link_libraries(openpete_core PUBLIC openpete_imgui)

# Hide the debug/diagnostic CLI flags from `--help` (options.c only splits the
# text; every flag still parses and works). Enabled by the *-launcher
# distribution presets so the distributed build shows a clean, user-facing flag
# list; OFF everywhere else (dev builds list everything). It is the one switch
# that says "this build carries the development surface", so the keymap reads
# it too: a KM_DEVBUILD row (core/include/openpete_keymap.h) takes its default
# key here and stays unbound in the distributed build.
option(OPENPETE_HIDE_DEBUG_FLAGS "Hide debug/diagnostic flags from --help output" OFF)
if(OPENPETE_HIDE_DEBUG_FLAGS)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HIDE_DEBUG_FLAGS=1)
endif()

# libav* for --record gameplay video capture (record/record.c). On a native
# build this is a hard dependency. On Debian/Ubuntu: libavformat-dev libavcodec-dev
# libavutil-dev libswscale-dev libswresample-dev. Min version libav* 58
# (ffmpeg 4.0).
#
# Cross-compiles (e.g. the MinGW Windows preset) are the exception: host
# pkg-config resolves the *host* (Linux) libav and injects host include paths
# (-isystem /usr/include/x86_64-linux-gnu), which poison the cross compile -
# pkg_check_modules does not honor CMAKE_FIND_ROOT_PATH. There is no target
# libav to link anyway, so when cross-compiling we compile --record out
# (record.c falls back to a stub that reports the feature unavailable).
option(OPENPETE_ENABLE_RECORD "Build --record gameplay video capture (needs libav*)" ON)
# For the windows-mingw cross build there is no system libav to link, so
# scripts/setup_mingw_ffmpeg.sh cross-builds a STATIC FFmpeg into this prefix and
# we link it here. Absent -> --record compiles to the stub (the old behaviour).
set(OPENPETE_FFMPEG_MINGW_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-mingw"
    CACHE PATH "Prefix of the cross-built static FFmpeg for windows-mingw --record")
if(NOT OPENPETE_ENABLE_RECORD)
    message(STATUS "openpete: --record disabled (OPENPETE_ENABLE_RECORD=OFF); record.c is a stub")
elseif(NOT CMAKE_CROSSCOMPILING)
    # Native (linux): host libav via pkg-config.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
    target_link_libraries(openpete_core PUBLIC PkgConfig::LIBAV)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HAS_LIBAV=1)
elseif(EXISTS "${OPENPETE_FFMPEG_MINGW_PREFIX}/lib/libavcodec.a")
    # A distributed exe must not carry GPL x264; the dist presets set
    # OPENPETE_DIST_PRESET, and scripts/package-dist.sh refuses the same
    # exe by its x264/libav strings.
    if(OPENPETE_DIST_PRESET)
        message(FATAL_ERROR "openpete: OPENPETE_ENABLE_RECORD=ON with the static "
            "FFmpeg at ${OPENPETE_FFMPEG_MINGW_PREFIX} links GPL x264 into the "
            "exe, which a dist preset must not ship. Configure without "
            "-DOPENPETE_ENABLE_RECORD=ON, or use the windows-mingw preset.")
    endif()
    # Cross (windows-mingw): static-link the FFmpeg from setup_mingw_ffmpeg.sh.
    # pkg_check_modules ignores the cross sysroot, so isolate pkg-config to OUR
    # prefix via PKG_CONFIG_LIBDIR; --static then resolves the full dep chain
    # (avformat..avutil + x264 + bcrypt/ole32/user32/m). --start-group wraps the
    # archives so inter-library refs resolve regardless of order. Static GPL
    # FFmpeg (x264) => the resulting .exe is GPL.
    find_package(PkgConfig REQUIRED)
    set(ENV{PKG_CONFIG_LIBDIR} "${OPENPETE_FFMPEG_MINGW_PREFIX}/lib/pkgconfig")
    pkg_check_modules(LIBAV REQUIRED
        libavformat libavcodec libavutil libswscale libswresample)
    target_include_directories(openpete_core PUBLIC ${LIBAV_STATIC_INCLUDE_DIRS})
    target_link_directories(openpete_core PUBLIC ${LIBAV_STATIC_LIBRARY_DIRS})
    target_link_libraries(openpete_core PUBLIC
        -Wl,--start-group ${LIBAV_STATIC_LIBRARIES} -Wl,--end-group)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HAS_LIBAV=1)
    message(STATUS "openpete: --record ENABLED for windows-mingw via static FFmpeg "
                   "at ${OPENPETE_FFMPEG_MINGW_PREFIX} (GPL: the .exe becomes GPL)")
else()
    message(STATUS "openpete: --record disabled for this cross build - no static "
                   "FFmpeg at ${OPENPETE_FFMPEG_MINGW_PREFIX} "
                   "(run scripts/setup_mingw_ffmpeg.sh to enable); record.c is a stub")
endif()

# PsyCross provides the PSYQ surface (libgs/libgte/libetc/libapi/...) used by
# PSYQ-targeted thunks. When the target exists, downstream targets include
# PsyCross/include and link `psycross_static` automatically.
if(TARGET psycross_static)
    target_link_libraries(openpete_core PUBLIC psycross_static)
    target_include_directories(openpete_core PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
        ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx)
    target_compile_definitions(openpete_core PUBLIC OPENPETE_HAS_PSYCROSS=1)
endif()

# Win32: openpete_time.c uses timeBeginPeriod from winmm.
if(WIN32)
    target_link_libraries(openpete_core PUBLIC winmm)
endif()

# Win32: the crash filter writes a minidump (diag/crash.c).
if(WIN32)
    target_link_libraries(openpete_core PUBLIC dbghelp)
endif()

# Fail the build (cheap automated check) if anyone smuggles a Spyro
# address or Win32-only header into core/. These greps live here as build-time guards.
if(NOT CMAKE_HOST_WIN32)
    # A handful of core/ TUs legitimately #include <windows.h>, each
    # behind `#ifdef _WIN32` so the POSIX build is unaffected:
    #   - crash.c             Win32 SEH unhandled-exception filter
    #   - openpete_time.c     QPC + waitable timer + timeBeginPeriod
    #   - openpete_frame_time_log.c  FILETIME / GetSystemTimeAsFileTime
    #   - mods/mod_loader.c   LoadLibrary/GetProcAddress (prebuilt mod
    #                         DLL loading - Windows' dlopen analog)
    #   - config/paths.c      GetModuleFileNameA (the exe directory, which
    #                         is the root of the portable data layout)
    #   - host/atomic_file.c  MoveFileExW (replace a file in one step)
    #   - psx/disc_fs_probe.c GetDriveTypeW (is the disc image on a network
    #                         or CD-ROM drive, for --disc-cache auto)
    # The rest of core/ must remain game-agnostic AND platform-agnostic,
    # so keep the grep-guard but list the exempt TUs by name. New core
    # TUs that need Win32 APIs MUST be added here AND keep their
    # platform calls under `#ifdef _WIN32`, AND NOT pull CreateFiber
    # (we use libco's amd64.c on POSIX and fiber.c on Win32; CreateFiber
    # specifically is what core/ is forbidden from touching).
    #
    # The address regex matches Spyro-specific addresses like 0x80012204
    # AND generic PSX bus constants like 0x80000000 (KSEG0 base). We only
    # care about the former, so allow `#define`d named constants through -
    # OPENPETE_PSX_KSEG0_BASE etc. live in openpete_psx.h precisely so
    # core/ doesn't carry bare hex literals for them. grep-pipe-grep keeps
    # the check under POSIX bash without needing PCRE lookarounds.
    add_custom_target(openpete_core_purity_check ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking core/ purity..."
        COMMAND bash -c "! grep -rE '0x800[0-9a-fA-F]{4}' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' | grep -vE '^[^:]+:[[:space:]]*#[[:space:]]*define'"
        COMMAND bash -c "! grep -rE 'CreateFiber|<windows.h>' ${CMAKE_SOURCE_DIR}/core/ --include='*.c' --include='*.h' --include='*.cpp' --include='*.hpp' --exclude='crash.c' --exclude='openpete_time.c' --exclude='openpete_frame_time_log.c' --exclude='mod_loader.c' --exclude='host_spawn.c' --exclude='paths.c' --exclude='log_file.c' --exclude='window_restack.c' --exclude='atomic_file.c' --exclude='disc_image.c' --exclude='disc_fs_probe.c' --exclude='mod_arena.c' --exclude='rewind_pages.c' --exclude='bugreport_write.c' --exclude='bugreport_redact.c'"
        COMMENT "core/ purity check: no Spyro addresses, no Win32-only APIs"
        VERBATIM)

    # The display seam has ONE owner (the present thread while eager is
    # active, the host loop otherwise). scripts/fences/seam_guard.py refuses
    # new frame/present call sites outside the owner and window ops off the
    # window's thread - the build-time half of the fence; the runtime half
    # (rhi_display_claim + the per-entry owner check) lives in rhi_vulkan.c.
    add_custom_target(openpete_seam_guard ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking display-seam ownership..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/seam_guard.py ${CMAKE_SOURCE_DIR}
        COMMENT "display-seam fence: presents and window ops only from their owner"
        VERBATIM)

    # A scene is built for a PRESENT instant. scripts/fences/clock_guard.py
    # refuses a wall-clock read (op_now / SDL_GetTicks* / clock_gettime) in
    # the scene-producer files unless the line says `clock-guard: wall`: the
    # interpolation alpha is (present clock - tick stamp) / period, and a
    # channel phased by op_now() sits at alpha~0 under eager production and
    # flakes under deterministic capture - both invisible to every gate.
    # The tile census counts what every build composes; OPENPETE_TILE_CENSUS
    # gates only the capture's tally record and sidecar write.
    # scripts/fences/tile_census_accessor.py refuses its accessor outside the
    # two capture helpers and a read of the variable outside its module, so
    # setting it for a golden capture selects no code path.
    add_custom_target(openpete_tile_census_guard ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking the tile-census accessor..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/tile_census_accessor.py tree ${CMAKE_SOURCE_DIR}
        COMMENT "tile-census fence: the capture variable selects no code path"
        VERBATIM)

    # Per-worker state fills whole cache lines: an element smaller than a
    # line, or an array not starting on one, shares a line between workers
    # depending on where the linker places it. scripts/fences/
    # worker_line_guard.py refuses a per-worker array whose element type has
    # no OPENPETE_WORKER_ELEM check (core/include/openpete_workpool.h).
    add_custom_target(openpete_worker_line_guard ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking per-worker cache lines..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/worker_line_guard.py tree ${CMAKE_SOURCE_DIR}
        COMMENT "worker-line fence: per-worker arrays fill whole cache lines"
        VERBATIM)

    # The oracle and the older forensic emulator are named as tools, never by
    # their internals (an upstream source path or line, an upstream symbol).
    # scripts/fences/provenance_census.py refuses a tracked line with a listed
    # internal name, or with an emulator's name beside an upstream source
    # form, outside its allow-list rows and the patch series.
    add_custom_target(openpete_provenance_census ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking emulator citations..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/provenance_census.py tree ${CMAKE_SOURCE_DIR}
        COMMENT "provenance fence: other emulators named as tools, never by their internals"
        VERBATIM)

    # The DuckStation series carries no upstream text: a line of its own
    # files or an inserted line of hooks.txt that occurs in the pinned
    # upstream tree is refused (scripts/fences/upstream_text_census.py).
    add_custom_target(openpete_upstream_text_census ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking the DuckStation series for upstream text..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/upstream_text_census.py tree ${CMAKE_SOURCE_DIR}
        COMMENT "upstream-text fence: the DuckStation series holds no upstream line"
        VERBATIM)

    add_custom_target(openpete_clock_guard ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking scene-producer clocks..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/clock_guard.py ${CMAKE_SOURCE_DIR}
        COMMENT "clock fence: scene producers read the present clock, not the wall clock"
        VERBATIM)

    # The frame word's 21-bit stream field is decoded by a shift pair or by
    # an `and` with a 0x1FFFFF mask held in a register; the final shift of
    # each decode is substituted with the stream-handle resolver. A missed
    # reader reads the kernel area silently, so the census over the
    # GENERATED C (opcode words, not C text) and the manifest must agree,
    # and every row's line must carry the substitution (a stale regen runs
    # the stock decode). The census reads Spyro 1's generated C alone, so the
    # target exists only when spyro-1 is configured.
    if("spyro-1" IN_LIST OPENPETE_GAMES)
        add_custom_target(openpete_vstream_sites ALL
            COMMAND ${CMAKE_COMMAND} -E echo "Checking frame-word field readers..."
            COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/vstream_sites.py ${CMAKE_SOURCE_DIR}
            COMMENT "vstream fence: every 21-bit field reader is substituted, at its final shift"
            VERBATIM)
    endif()

    # Launcher hygiene, the two silent-failure defaults. A scripted run must
    # silence the pad (no player is using it, and the buzzing reaches no log),
    # and a script that sets OPENPETE_* knobs for a Windows .exe must name
    # WSLENV or the knob never crosses and the run measures the default.
    # A scanner that runs only by hand is not enforced, so both run on
    # every build.
    add_custom_target(openpete_launch_guards ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking launcher hygiene..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/rumble_guard.py ${CMAKE_SOURCE_DIR}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/wslenv_guard.py ${CMAKE_SOURCE_DIR}
        # An unattended launcher that can open a window locks host input, so
        # host keyboard, mouse or pad input cannot steer or end the run.
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/input_lock_guard.py ${CMAKE_SOURCE_DIR}
        # An unattended Linux launcher that can open a window takes its display
        # from scripts/lib/xvfb.sh, a private Xvfb server, so no window reaches
        # the session's desktop.
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/xvfb_guard.py ${CMAKE_SOURCE_DIR}
        # A launcher runs the game inside a sandbox root (OPENPETE_SANDBOX via
        # scripts/lib/sandbox.sh or hostenv.sandbox), so no run writes into
        # the player's per-user directories.
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/sandbox_guard.py ${CMAKE_SOURCE_DIR}
        # The keymap's KM_LEVEL / KM_DELTA flags against their consumer sites:
        # the parser enforces them for mouse sources and the unit test cannot
        # see the consumers.
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/keymap_flags.py ${CMAKE_SOURCE_DIR}
        COMMENT "launcher fences: silence the pad, make env knobs cross into WSL, lock host input, private Xvfb display, sandbox the run, keymap flags match consumers"
        VERBATIM)

    # Host controls reach the game through the knob registry: every host
    # action names its row and op, no site reads a row-driving action outside
    # hc_edge/hc_held/hc_count, every dev overlay widget names its row or an
    # exemption, and every settings-menu row names a knob. Refuses the build.
    add_custom_target(openpete_host_control_coverage ALL
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/host_control_coverage.py tree ${CMAKE_SOURCE_DIR}
        COMMENT "host-control fence: every host control goes through its registry row"
        VERBATIM)

    # Cognitive-complexity ratchet. The fence acts in a git pre-commit hook,
    # and core.hooksPath is local config a fresh clone does not carry, so
    # `check --armed` fails the build until the clone is armed (or declares
    # OPENPETE_NO_COMMITS=1). `check` re-scores the corpus against HEAD's
    # baseline (about 2 s) and catches a rise that reached HEAD without the
    # hook (merge, revert, --no-verify). It runs under linux-release only:
    # that preset's compile database is the one the baseline was scored
    # from, the MinGW database carries another compiler's flags, and the
    # release container has no clang-tidy. The check reads this build's own
    # compile database (and the OPENPETE_GAMES of the CMakeCache.txt beside
    # it), so a build directory outside build/linux-release is scored as
    # configured.
    get_filename_component(_openpete_bindir_name "${CMAKE_BINARY_DIR}" NAME)
    set(_openpete_complexity_check "")
    if(_openpete_bindir_name STREQUAL "linux-release")
        set(_openpete_complexity_check
            COMMAND ${CMAKE_COMMAND} -E env
                    OPENPETE_COMPLEXITY_DB=${CMAKE_BINARY_DIR}/compile_commands.json
                    python3 ${CMAKE_SOURCE_DIR}/scripts/fences/complexity_ratchet.py check)
    endif()
    add_custom_target(openpete_complexity_ratchet ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking the complexity ratchet..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/complexity_ratchet.py check --armed
        ${_openpete_complexity_check}
        COMMENT "complexity fence: the pre-commit hook is armed, and no function's excess rose"
        VERBATIM)
    add_dependencies(openpete_complexity_ratchet openpete_generated_headers)

    # Function-documentation ratchet, the same shape: the hook shared with
    # the complexity fence must call it (`check --armed`), and `check`
    # re-parses the corpus with libclang against HEAD's baseline (about 4 s)
    # under linux-release only, the database the baseline was captured from.
    set(_openpete_doc_check "")
    if(_openpete_bindir_name STREQUAL "linux-release")
        set(_openpete_doc_check
            COMMAND ${CMAKE_COMMAND} -E env
                    OPENPETE_DOCS_DB=${CMAKE_BINARY_DIR}/compile_commands.json
                    python3 ${CMAKE_SOURCE_DIR}/scripts/fences/doc_ratchet.py check)
    endif()
    add_custom_target(openpete_doc_ratchet ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking the documentation ratchet..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/doc_ratchet.py check --armed
        ${_openpete_doc_check}
        COMMENT "documentation fence: the pre-commit hook calls it, and every function is documented or has a row"
        VERBATIM)
    add_dependencies(openpete_doc_ratchet openpete_generated_headers)

    # Thread-role fence (openpete_thread_roles.h), the same shape: the hook
    # must call it (`check --armed`), and `check` runs clang-19
    # -Wthread-safety and the binding-table checks over the TUs changed since
    # its last pass (a stamp beside the database; a changed header reruns
    # every TU) in this database and the windows-mingw one, configured on
    # demand. linux-release only, like the ratchets.
    set(_openpete_roles_check "")
    if(_openpete_bindir_name STREQUAL "linux-release")
        set(_openpete_roles_check
            COMMAND ${CMAKE_COMMAND} -E env
                    OPENPETE_THREAD_ROLES_DB=${CMAKE_BINARY_DIR}/compile_commands.json
                    python3 ${CMAKE_SOURCE_DIR}/scripts/fences/thread_roles.py check)
    endif()
    add_custom_target(openpete_thread_roles ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking thread-role ownership..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/thread_roles.py check --armed
        ${_openpete_roles_check}
        COMMENT "thread-role fence: the pre-commit hook calls it, and the changed TUs pass clang -Wthread-safety and the binding table"
        VERBATIM)
    add_dependencies(openpete_thread_roles openpete_generated_headers)
endif()
