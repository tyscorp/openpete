# tests/unit/CMakeLists.txt - host-side unit tests.
#
# These don't boot the PSX runtime; they exercise pure functions
# (parsers, decoders) that we'd rather catch regressions in without a full
# game build. Hooked into ctest via enable_testing() in the root list.
#
# Every test defined in this directory carries the `unit` label: the landing
# runs `ctest -L unit -LE slow`, so a new test here joins that set unasked.
cmake_language(DEFER CALL openpete_label_directory_tests unit)

# bk2_movie depends on miniz (zip read) + the log macros. Compile those
# sources directly into the test target instead of linking openpete_core,
# whose link interface carries SDL3, OpenAL and every other runtime
# dependency into a console test that uses none of them.
add_executable(bk2_movie_test
    bk2_movie_test.c
    ${CMAKE_SOURCE_DIR}/core/record/bk2_movie.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(bk2_movie_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/third_party/miniz)
set_target_properties(bk2_movie_test PROPERTIES C_STANDARD 99)
add_test(NAME bk2_movie_test COMMAND bk2_movie_test)

# fnv1a64_test - header-only, no openpete deps. Verifies the FNV1a-64
# implementation underpinning the determinism harness's per-frame
# gameplay-state hash.
add_executable(fnv1a64_test fnv1a64_test.c)
target_include_directories(fnv1a64_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(fnv1a64_test PROPERTIES C_STANDARD 99)
add_test(NAME fnv1a64_test COMMAND fnv1a64_test)

# op_appendf_test - header-only. op_appendf keeps its length at the bytes
# the buffer holds when the text overflows it.
add_executable(op_appendf_test op_appendf_test.c)
target_include_directories(op_appendf_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(op_appendf_test PROPERTIES C_STANDARD 99)
add_test(NAME op_appendf_test COMMAND op_appendf_test)

# portal_cover_slot_test - header-only. The feed-plane to published
# cover-plane slot map an inner-rim face names its accepting plane by.
add_executable(portal_cover_slot_test portal_cover_slot_test.c)
target_include_directories(portal_cover_slot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(portal_cover_slot_test PROPERTIES C_STANDARD 99)
add_test(NAME portal_cover_slot_test COMMAND portal_cover_slot_test)

# eager_ladder_test - header-only. The eager producer's sum pricing and
# decimation ladder on synthetic spans and on recorded plan lines, with the
# previous pricing and ladder as the negative control.
add_executable(eager_ladder_test eager_ladder_test.c)
target_include_directories(eager_ladder_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(eager_ladder_test PRIVATE m)
endif()
set_target_properties(eager_ladder_test PROPERTIES C_STANDARD 99)
add_test(NAME eager_ladder_test COMMAND eager_ladder_test)

# moby_snap_rule_test - header-only. The sunk-pickup snap's side term and
# depth rule on synthetic floors and walls: a double-sided floor keeps the
# upper side from beneath, so the tip hides and the body is not pulled.
add_executable(moby_snap_rule_test moby_snap_rule_test.c)
target_include_directories(moby_snap_rule_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(moby_snap_rule_test PRIVATE m)
endif()
set_target_properties(moby_snap_rule_test PROPERTIES C_STANDARD 99)
add_test(NAME moby_snap_rule_test COMMAND moby_snap_rule_test)

# moby_patch_list_test - header-only. The keyframe patch-list walk under
# each game's copy loop: a count-0 entry copies one word where the loop runs
# its body first (Spyro 2/3) and none where it tests first (Spyro 1).
add_executable(moby_patch_list_test moby_patch_list_test.c)
target_include_directories(moby_patch_list_test PRIVATE
    ${CMAKE_SOURCE_DIR}/games/common/renderer)
set_target_properties(moby_patch_list_test PROPERTIES C_STANDARD 99)
add_test(NAME moby_patch_list_test COMMAND moby_patch_list_test)

# view_rect_test - header-only. The native view scissor equals the pixel set
# the view's quads cover over a window-size grid; the separately rounded
# origin-and-size arithmetic is the negative control.
add_executable(view_rect_test view_rect_test.c)
target_include_directories(view_rect_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(view_rect_test PRIVATE m)
endif()
set_target_properties(view_rect_test PROPERTIES C_STANDARD 99)
add_test(NAME view_rect_test COMMAND view_rect_test)

# line_quad_test - ngte_line_quad's quad, rasterized at 1x by pixel centres,
# draws the pixels psx_gpu_line_pixels draws, ties excepted, over every line
# within 64 px of a start pixel; the perpendicular 1 px quad is the negative
# control. Compiles native_gte.c with link stubs for the renderer
# entry points it references.
add_executable(line_quad_test
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    line_quad_test.c
    psx_gpu_line.c
    ${CMAKE_SOURCE_DIR}/games/common/renderer/native_gte.c)
target_include_directories(line_quad_test PRIVATE
    $<TARGET_PROPERTY:openpete_game_common,INCLUDE_DIRECTORIES>)
target_compile_definitions(line_quad_test PRIVATE
    $<TARGET_PROPERTY:openpete_game_common,COMPILE_DEFINITIONS>)
if(NOT WIN32)
    target_link_libraries(line_quad_test PRIVATE m)
endif()
set_target_properties(line_quad_test PROPERTIES C_STANDARD 99)
add_test(NAME line_quad_test COMMAND line_quad_test)

# gpu_line_table_test - psx_gpu_line_pixels against the measured table of the
# pixels the PSX GPU draws for every line within 64 px of a start pixel,
# data/gpu-line/line.pixels (tools/gpu_blackbox/measure_lines.py).
add_executable(gpu_line_table_test gpu_line_table_test.c psx_gpu_line.c)
set_target_properties(gpu_line_table_test PROPERTIES C_STANDARD 99)
add_test(NAME gpu_line_table_test COMMAND gpu_line_table_test
         ${CMAKE_CURRENT_SOURCE_DIR}/data/gpu-line/line.pixels)

# gpu_uv_table_test - psx_uv_plane_setup against the measured table of the
# texel the PSX GPU reads at each pixel of a textured triangle or sprite,
# data/gpu-uv/uv.texels (tools/gpu_blackbox/measure_uv.py).
add_executable(gpu_uv_table_test gpu_uv_table_test.c
    ${CMAKE_SOURCE_DIR}/games/common/renderer/psx_uv.c)
target_include_directories(gpu_uv_table_test PRIVATE ${CMAKE_SOURCE_DIR}/games/common)
if(NOT WIN32)
    target_link_libraries(gpu_uv_table_test PRIVATE m)
endif()
set_target_properties(gpu_uv_table_test PROPERTIES C_STANDARD 99)
add_test(NAME gpu_uv_table_test COMMAND gpu_uv_table_test
         ${CMAKE_CURRENT_SOURCE_DIR}/data/gpu-uv/uv.texels)

# live_latch_test - header-only. A setting latched once per present reads one
# value through the present while another thread keeps setting it; the
# pre-latch read pattern is the negative control. POSIX threads, so not
# built for Windows.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(live_latch_test live_latch_test.c)
    target_include_directories(live_latch_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include)
    set_target_properties(live_latch_test PROPERTIES C_STANDARD 11)
    target_link_libraries(live_latch_test PRIVATE Threads::Threads)
    add_test(NAME live_latch_test COMMAND live_latch_test)
endif()

# render_res_test - the --resolution grammar and the base-image arithmetic
# (render_res.c): strict parser, pack round trip, the N width rule, the bound
# steps and the per-axis filter choice. Pure C, no device.
add_executable(render_res_test render_res_test.c
    ${CMAKE_SOURCE_DIR}/core/renderer/render_res.c)
target_include_directories(render_res_test PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(render_res_test PROPERTIES C_STANDARD 11)
if(NOT WIN32)
    target_link_libraries(render_res_test PRIVATE m)
endif()
add_test(NAME render_res_test COMMAND render_res_test)

# openpete_time_test - verifies the high-resolution monotonic clock +
# hybrid sleep+spin waiter.
# Compiles openpete_time.c directly to stay SDL-free. The Win32 path
# needs winmm; Linux is link-clean.
add_executable(openpete_time_test
    openpete_time_test.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
target_include_directories(openpete_time_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(openpete_time_test PROPERTIES C_STANDARD 99)
if(WIN32)
    target_link_libraries(openpete_time_test PRIVATE winmm)
endif()
add_test(NAME openpete_time_test COMMAND openpete_time_test)

# keymap_test - the [keys] action registry, token parser, layered store
# and conflict scan. keymap.c reaches SDL only through the lookup/input
# seams, so this installs fakes for both: the link line stays SDL-free
# like every other console test here, and the fake input state is what
# lets the edge latch and the keyboard-suspend rule be exercised at all.
# log.c comes along for OPL_WARN_F.
add_executable(keymap_test
    keymap_test.c
    ${CMAKE_SOURCE_DIR}/core/input/keymap.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(keymap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(keymap_test PROPERTIES C_STANDARD 11)
add_test(NAME keymap_test COMMAND keymap_test)

# log_snapshot_test - the logger's level filter, spec read-back and format
# (the `semantics` mode), and the same state reconfigured on one thread
# while others log and read the spec back (the `race` mode). The race runs
# twice, under AddressSanitizer (a rule list freed under a reader) and
# ThreadSanitizer (any unsynchronized access to the logger's state).
# sanitizer_run.sh exits 64 (skipped) where the sanitizer runtime does not
# start; a compiler that cannot build the sanitizer registers the same skip.
# The harness uses POSIX threads, pread and setenv, so it is not built for
# Windows.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    add_executable(log_snapshot_test
        log_snapshot_test.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(log_snapshot_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include)
    set_target_properties(log_snapshot_test PROPERTIES C_STANDARD 11)
    target_link_libraries(log_snapshot_test PRIVATE Threads::Threads)
    add_test(NAME log_snapshot_semantics COMMAND log_snapshot_test semantics)
    set_tests_properties(log_snapshot_semantics PROPERTIES LABELS "log")
    include(CheckCSourceCompiles)
    foreach(san address thread)
        set(CMAKE_REQUIRED_FLAGS "-fsanitize=${san}")
        set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=${san}")
        check_c_source_compiles("int main(void) { return 0; }"
            OPENPETE_HAVE_FSANITIZE_${san})
        unset(CMAKE_REQUIRED_FLAGS)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
        set(test_name log_snapshot_race_${san})
        if(OPENPETE_HAVE_FSANITIZE_${san})
            set(target log_snapshot_test_${san})
            add_executable(${target}
                log_snapshot_test.c
                ${CMAKE_SOURCE_DIR}/core/diag/log.c)
            target_include_directories(${target} PRIVATE
                ${CMAKE_SOURCE_DIR}/core/include)
            set_target_properties(${target} PROPERTIES C_STANDARD 11)
            target_compile_options(${target} PRIVATE
                -fsanitize=${san} -fno-omit-frame-pointer -g -O1)
            target_link_options(${target} PRIVATE -fsanitize=${san})
            target_link_libraries(${target} PRIVATE Threads::Threads)
            add_test(NAME ${test_name}
                COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/sanitizer_run.sh
                        $<TARGET_FILE:${target}> race 2 3 200000)
        else()
            add_test(NAME ${test_name} COMMAND sh -c
                "echo '-fsanitize=${san} does not build here; skipped'; exit 64")
        endif()
        set_tests_properties(${test_name} PROPERTIES
            LABELS "log;sanitizer" SKIP_RETURN_CODE 64 TIMEOUT 120)
    endforeach()
endif()

# psx_region_test - verifies the PSX RAM virtualization region table
# (registration / lookup / overlap-rejection). Compiles psx_region.c
# directly so openpete_core's SDL3 and OpenAL dependencies stay out of a
# console test binary.
add_executable(psx_region_test
    psx_region_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(psx_region_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psx_region_test PROPERTIES C_STANDARD 99)
add_test(NAME psx_region_test COMMAND psx_region_test)

# psxtrace_schedule_test - the oracle schedule's account of a CdRead or
# waited VSync its sidecars have no row left for: counted past the trace's
# last record, a failure inside the records. One process per case, since
# the schedule keeps its state in file statics. Compiles
# psxtrace_schedule.c and log.c directly; the test defines the anchor
# counters and the root counter 2 hook the schedule links against.
add_executable(psxtrace_schedule_test
    psxtrace_schedule_test.c
    ${CMAKE_SOURCE_DIR}/core/diag/psxtrace_schedule.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(psxtrace_schedule_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psxtrace_schedule_test PROPERTIES C_STANDARD 11)
add_test(NAME psxtrace_schedule_past_end COMMAND psxtrace_schedule_test past-end)
add_test(NAME psxtrace_schedule_beyond_inside COMMAND psxtrace_schedule_test inside)

# psxtrace_ratchet_test - the oracle's event ratchet: a row's listed GTE
# events, VBL events and gameplay bursts pass, an unlisted one is new, a
# listed one the run never produced is stale; and the carry, VBL-event and
# load-span rules on synthetic record sequences. Compiles
# psxtrace_ratchet.c, log.c and tomlc99 directly.
add_executable(psxtrace_ratchet_test
    psxtrace_ratchet_test.c
    ${CMAKE_SOURCE_DIR}/core/diag/psxtrace_ratchet.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(psxtrace_ratchet_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psxtrace_ratchet_test PROPERTIES C_STANDARD 11)
add_test(NAME psxtrace_ratchet_test
         COMMAND psxtrace_ratchet_test ${CMAKE_CURRENT_BINARY_DIR}/psxtrace_ratchet_test.toml)

# psx_runtime_layout_test - static assertions that CPUState in
# core/include/psxrt/psx_runtime.h has the offset, size and type of every
# field of the recompiler runner's CPUState, which savestates and mods built
# against an earlier SDK depend on. The only target that reads the runner
# header.
add_executable(psx_runtime_layout_test psx_runtime_layout_test.c)
target_include_directories(psx_runtime_layout_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psx_runtime_layout_test PROPERTIES C_STANDARD 11)
add_test(NAME psx_runtime_layout_test COMMAND psx_runtime_layout_test)

# psx_span_test - the span form of the guest-pointer resolver and the
# width-checked bus accessors at the ends of main RAM, the scratchpad and a
# registered region. Compiles cpu.c and psx_region.c with local stubs for
# the CD-ROM register model, the BIOS ROM window and the logger.
add_executable(psx_span_test
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    psx_span_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(psx_span_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(psx_span_test PROPERTIES C_STANDARD 11)
add_test(NAME psx_span_test COMMAND psx_span_test)

# psx_span_test_nomirror - the same checks over a bus built with the RAM
# mirror removed (OPENPETE_PSX_RAM_MIRROR_END = 2 MB): the mirror checks
# must fail, so the test is expected to fail.
add_executable(psx_span_test_nomirror
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    psx_span_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(psx_span_test_nomirror PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_compile_definitions(psx_span_test_nomirror PRIVATE OPENPETE_PSX_RAM_MIRROR_END=0x00200000u)
set_target_properties(psx_span_test_nomirror PROPERTIES C_STANDARD 11)
add_test(NAME psx_span_test_nomirror COMMAND psx_span_test_nomirror)
set_tests_properties(psx_span_test_nomirror PROPERTIES WILL_FAIL TRUE)
set_tests_properties(psx_span_test psx_span_test_nomirror PROPERTIES LABELS "mirror")

# present_ram_assert_test - the present-thread write refusal
# (OPENPETE_PRESENT_RAM_ASSERT, core/psx/cpu.c): a thread bound to `present`
# reads guest RAM through the resolvers, and a store through its pointer
# aborts a forked child with the named message (the negative control).
# Linux only: the refusal maps a memfd twice.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    add_executable(present_ram_assert_test
        ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
        present_ram_assert_test.c
        ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
        ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
    target_include_directories(present_ram_assert_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    target_link_libraries(present_ram_assert_test PRIVATE Threads::Threads)
    set_target_properties(present_ram_assert_test PROPERTIES C_STANDARD 11)
    add_test(NAME present_ram_assert_test COMMAND present_ram_assert_test)
    set_tests_properties(present_ram_assert_test PROPERTIES LABELS "thread-roles" TIMEOUT 30)
endif()

# s2_grid_relink_test - Spyro 2's moby grid relink (func_80041538), its
# recompiled body copied verbatim from the generated C, run on the bus of
# psx_span_test against a RAM image holding only the grid pointer, the
# grid and one moby moved to a negative X with flag 2: the moby must end in
# bucket 159. The _nomirror build (mirror removed as above) passes only when
# the unlink walk at 0x80041634 runs to the poll cap, proving the first
# build's pass rests on the mirror. Needs Spyro 2's generated C.
set(_s2_generated ${CMAKE_SOURCE_DIR}/games/spyro-2/generated/PSX.EXE_full.c)
if("spyro-2" IN_LIST OPENPETE_GAMES AND EXISTS ${_s2_generated})
    set(_s2_relink_c ${CMAKE_CURRENT_BINARY_DIR}/s2_func_80041538.gen.c)
    add_custom_command(
        OUTPUT  ${_s2_relink_c}
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/extract_recomp_function.py
                ${_s2_generated} func_80041538 ${_s2_relink_c}
        DEPENDS ${_s2_generated} ${CMAKE_CURRENT_SOURCE_DIR}/extract_recomp_function.py
        COMMENT "Copying Spyro 2 func_80041538 out of the generated C")
    foreach(_variant s2_grid_relink_test s2_grid_relink_test_nomirror)
        add_executable(${_variant}
            ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
            s2_grid_relink_test.c
            ${_s2_relink_c}
            ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
            ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
        target_include_directories(${_variant} PRIVATE
            ${CMAKE_SOURCE_DIR}/core/include
            ${CMAKE_SOURCE_DIR}/core/include/psxrt)
        set_target_properties(${_variant} PROPERTIES C_STANDARD 11)
        add_test(NAME ${_variant} COMMAND ${_variant})
        set_tests_properties(${_variant} PROPERTIES LABELS "mirror" TIMEOUT 30)
    endforeach()
    target_compile_definitions(s2_grid_relink_test_nomirror PRIVATE OPENPETE_PSX_RAM_MIRROR_END=0x00200000u)
endif()

# div_semantics_test_<game> - the first div and divu statement of each
# game's generated PSX.EXE_full.c, copied verbatim, run on R3000 results:
# x/0 and 0x80000000 / -1 never trap and leave the hardware's LO and HI.
# A recompiler that emits a bare C division for the overflow pair ends the
# test with SIGFPE.
foreach(_game IN LISTS OPENPETE_GAMES)
    set(_gen ${CMAKE_SOURCE_DIR}/games/${_game}/generated/PSX.EXE_full.c)
    if(NOT EXISTS ${_gen})
        continue()
    endif()
    set(_sites_c ${CMAKE_CURRENT_BINARY_DIR}/div_sites_${_game}.gen.c)
    add_custom_command(
        OUTPUT  ${_sites_c}
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/extract_div_sites.py ${_gen} ${_sites_c}
        DEPENDS ${_gen} ${CMAKE_CURRENT_SOURCE_DIR}/extract_div_sites.py
        COMMENT "Copying ${_game}'s first div and divu sites out of the generated C")
    add_executable(div_semantics_test_${_game} div_semantics_test.c ${_sites_c})
    target_include_directories(div_semantics_test_${_game} PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(div_semantics_test_${_game} PROPERTIES C_STANDARD 11)
    add_test(NAME div_semantics_test_${_game} COMMAND div_semantics_test_${_game})
    set_tests_properties(div_semantics_test_${_game} PROPERTIES LABELS "recomp" TIMEOUT 30)
endforeach()

# knob_registry_test - the M-overlay knob table: module-contributed rows
# (openpete_knob_add) landing inside their section, capacity refusal, one row
# per id, flag and env var, and KNOB_ACTION button rows. Compiles knob_registry.c directly and stubs the
# renderer/host setters it writes through, so no SDL in a console test.
# config.c comes along because the registry now asks the resolver where each
# row's value came from instead of re-deriving it - and having both tables in
# one binary is what lets this test assert they agree (see the schema
# cross-check in main).
add_executable(knob_registry_test
    knob_registry_test.c
    ${CMAKE_SOURCE_DIR}/core/config/knob_registry.c
    ${CMAKE_SOURCE_DIR}/core/renderer/render_res.c
    ${CMAKE_SOURCE_DIR}/core/config/config.c
    ${CMAKE_SOURCE_DIR}/core/config/config_schema.c
    # keymap.c rides along because config.c now seeds and resolves the
    # binding store, paths.c because it decides where the [saved] zone is
    # written. No log.c: this test stubs the log entry points itself.
    ${CMAKE_SOURCE_DIR}/core/input/keymap.c
    # stick_response.c is SDL-free: the stick_response row's getter and
    # setter call it directly.
    ${CMAKE_SOURCE_DIR}/core/input/stick_response.c
    ${CMAKE_SOURCE_DIR}/core/config/paths.c
    ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(knob_registry_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(knob_registry_test PROPERTIES C_STANDARD 11)
if(NOT WIN32)
    target_link_libraries(knob_registry_test PRIVATE m)   # render_res.c: floor, ceil
endif()
add_test(NAME knob_registry_test COMMAND knob_registry_test)

# pose_text_test - the free-camera pose grammar --freecam-pose is checked
# against: accepted and refused forms, then every pose in the committed
# corpus manifests, so a malformed row fails here before a harness runs it.
# POSIX only (glob).
if(NOT WIN32)
    add_executable(pose_text_test
        pose_text_test.c
        ${CMAKE_SOURCE_DIR}/core/config/pose_text.c)
    target_include_directories(pose_text_test PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
    set_target_properties(pose_text_test PROPERTIES C_STANDARD 11)
    add_test(NAME pose_text_test COMMAND pose_text_test ${CMAKE_SOURCE_DIR})
endif()

# paths_lock_wait_test - openpete_paths_lock_wait, the lock a first-boot
# module build takes on its module directory and a mod save sidecar write
# takes on its lock file: a waiter takes the lock only after the holding
# process (a forked child) exits, a cancelled waiter returns without it, a
# bounded waiter returns without it once the bound passes. POSIX only
# (fork).
if(NOT WIN32)
    add_executable(paths_lock_wait_test
        paths_lock_wait_test.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(paths_lock_wait_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(paths_lock_wait_test PROPERTIES C_STANDARD 11)
    add_test(NAME paths_lock_wait_test COMMAND paths_lock_wait_test)
endif()

# paths_state_file_test - openpete_paths_state_file, where the dev overlay's
# openpete-devui.ini lives: beside the state leaves in the sandbox, per-user
# and portable layouts, its directory made and the file not. POSIX only
# (mkdtemp, setenv).
if(NOT WIN32)
    add_executable(paths_state_file_test
        paths_state_file_test.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(paths_state_file_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(paths_state_file_test PROPERTIES C_STANDARD 11)
    add_test(NAME paths_state_file_test COMMAND paths_state_file_test)
endif()

# bugreport_replay_knobs_test - --bug-report-replay applies a recorded knob
# value only to a row this build classifies render, whatever class the
# bundle's knobs.txt line names. The registry and the member reader are
# stubs; SDL is linked for the bug-report headers' atomics.
add_executable(bugreport_replay_knobs_test
    bugreport_replay_knobs_test.c
    ${CMAKE_SOURCE_DIR}/core/record/bugreport_replay_knobs.c
    ${CMAKE_SOURCE_DIR}/core/runtime/bugknobs_class.c)
target_include_directories(bugreport_replay_knobs_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_link_libraries(bugreport_replay_knobs_test PRIVATE SDL3::SDL3-static)
set_target_properties(bugreport_replay_knobs_test PROPERTIES C_STANDARD 11)
add_test(NAME bugreport_replay_knobs_test COMMAND bugreport_replay_knobs_test)

# audio_volume_test - the --sfx / --music slider presets: stamping, the
# re-stamp over the game's own reseeds (title screen, SpuInitialize, save
# load), and the release when the player moves that slider in the options
# menu. The module is a state machine over guest RAM, so a fake CPUState over
# a small array covers it without a controller replay.
add_executable(audio_volume_test
    audio_volume_test.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/features/audio_volume.c)
target_include_directories(audio_volume_test PRIVATE
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(audio_volume_test PROPERTIES C_STANDARD 99)
add_test(NAME audio_volume_test COMMAND audio_volume_test)

# vibration_test - the --vibration strength scaling to SDL's motor range and
# the --vibration preset's state machine. SDL-free: rumble_response.c and
# rumble_motor.c are the byte-to-motor conversion, vibration.c is the
# guest-side preset over a fake CPUState. The per-byte response itself is
# rumble_response_test's. The rest of the vibration path needs a real pad and
# is covered by the virtual-pad rumble oracle instead - see
# scripts/check_rumble.sh.
add_executable(vibration_test
    vibration_test.c
    ${CMAKE_SOURCE_DIR}/core/input/rumble_response.c
    ${CMAKE_SOURCE_DIR}/core/input/rumble_motor.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/features/vibration.c)
target_include_directories(vibration_test PRIVATE
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
if(NOT WIN32)
    target_link_libraries(vibration_test PRIVATE m)   # rumble_response.c: pow
endif()
set_target_properties(vibration_test PROPERTIES C_STANDARD 99)
add_test(NAME vibration_test COMMAND vibration_test)

# rumble_response_test - the motor intensity per actuator byte,
# openpete_rumble_response (a fitted cubic), against the measured table
# data/rumble-response/ (tools/rumble_response_measure.py), with two
# known-wrong curves the comparator must reject. SDL-free.
add_executable(rumble_response_test
    rumble_response_test.c
    ${CMAKE_SOURCE_DIR}/core/input/rumble_response.c)
target_include_directories(rumble_response_test PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(rumble_response_test PRIVATE m)   # rumble_response.c: pow
endif()
set_target_properties(rumble_response_test PROPERTIES C_STANDARD 11)
add_test(NAME rumble_response_test COMMAND rumble_response_test
         ${CMAKE_CURRENT_SOURCE_DIR}/data/rumble-response/reference.tsv)

# stick_response_test - --stick-response's mappings over the whole SDL axis
# range, console against the measured table data/stick-response/
# (tools/stick_response_measure.py). SDL-free: stick_response.c alone.
add_executable(stick_response_test
    stick_response_test.c
    ${CMAKE_SOURCE_DIR}/core/input/stick_response.c)
target_include_directories(stick_response_test PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(stick_response_test PROPERTIES C_STANDARD 11)
add_test(NAME stick_response_test COMMAND stick_response_test
         ${CMAKE_CURRENT_SOURCE_DIR}/data/stick-response/console.runs)

# memcard_test - the host memory card against killed and failed writes and
# against card files it did not create (atomic replacement, the libmcrd and
# BIOS error paths, set-aside of unknown files, read-only headered images,
# in-place frame writes, the card guard). POSIX only: the cases fork.
if(NOT WIN32)
    add_executable(memcard_test
        memcard_test.c
        ${CMAKE_SOURCE_DIR}/core/host/memcard.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/diag/error_ids.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(memcard_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(memcard_test PROPERTIES C_STANDARD 11)
    add_test(NAME memcard_test
             COMMAND memcard_test ${CMAKE_CURRENT_BINARY_DIR}/memcard_test.d)
endif()

# atomic_write_test - every write class of the atomic-file helper (synced,
# copy, unsynced, the .part rename, the directory swap) against an injected
# OPENPETE_WRITE_FAIL: the previous file or tree must be byte-identical, and
# the run refuses success when no failure was injected. POSIX only: the
# directory case uses dirent and rmdir.
if(NOT WIN32)
    add_executable(atomic_write_test
        atomic_write_test.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(atomic_write_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    set_target_properties(atomic_write_test PROPERTIES C_STANDARD 11)
    add_test(NAME atomic_write_test
             COMMAND atomic_write_test ${CMAKE_CURRENT_BINARY_DIR}/atomic_write_test.d)
endif()

# tile_census_test - the tile census's class-bit counters on a synthetic
# scene: every defined bit names a counter, own and backdrop forms, and a
# bit with no registered counter refuses the sidecar.
if(NOT WIN32)
    add_executable(tile_census_test
        tile_census_test.c
        ${CMAKE_SOURCE_DIR}/core/diag/tile_census.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(tile_census_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt
        ${CMAKE_SOURCE_DIR}/core/diag)
    set_target_properties(tile_census_test PROPERTIES C_STANDARD 11)
    add_test(NAME tile_census_test COMMAND tile_census_test ${CMAKE_CURRENT_BINARY_DIR})
endif()

# config_resolve_test - openpete.toml precedence. The resolver decides which
# of ~10 layers wins each knob and then materializes exactly one spelling of
# it; this pins that ordering down so an edit can't quietly re-order the
# tiers. config.c is deliberately SDL-free so it compiles straight in
# alongside tomlc99.
add_executable(config_resolve_test
    config_resolve_test.c
    ${CMAKE_SOURCE_DIR}/core/config/config.c
    ${CMAKE_SOURCE_DIR}/core/config/config_schema.c
    ${CMAKE_SOURCE_DIR}/core/input/keymap.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c
    ${CMAKE_SOURCE_DIR}/core/config/paths.c
    ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
    ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
target_include_directories(config_resolve_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(config_resolve_test PROPERTIES C_STANDARD 11)
add_test(NAME config_resolve_test COMMAND config_resolve_test)

# identify_test - game identification: the serial table against every
# game's recomp.conf, and openpete_identify's source order and refusals.
# The disc read and identify.c's calls into the parser, the config layer,
# the boot line, the clock and the disc trace are stubbed in the test.
add_executable(identify_test
    identify_test.c
    ${CMAKE_SOURCE_DIR}/core/config/identify.c
    ${CMAKE_SOURCE_DIR}/core/config/game_registry.c
    ${CMAKE_SOURCE_DIR}/core/diag/error_ids.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(identify_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_compile_definitions(identify_test PRIVATE
    OPENPETE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_target_properties(identify_test PROPERTIES C_STANDARD 11)
add_test(NAME identify_test COMMAND identify_test)

# options_validate_test - the option parser's shared value validator: a
# refused value fails when typed and warns and keeps the default from
# openpete.toml or an env spelling, strict mode fails both, the env spellings
# resolved after the flags (an [env] table reaches them), and
# openpete_options_effective. options.c calls SDL for the --bk2-record
# directory, so the test links SDL statically; the game hooks, the SDL key
# names and the anchor parser are stubbed.
if(TARGET SDL3::SDL3-static)
    add_executable(options_validate_test
        options_validate_test.c
        ${CMAKE_SOURCE_DIR}/core/config/options.c
        ${CMAKE_SOURCE_DIR}/core/config/pose_text.c
        ${CMAKE_SOURCE_DIR}/core/renderer/render_res.c
        ${CMAKE_SOURCE_DIR}/core/config/config.c
        ${CMAKE_SOURCE_DIR}/core/config/config_schema.c
        ${CMAKE_SOURCE_DIR}/core/input/keymap.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
    target_include_directories(options_validate_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(options_validate_test PRIVATE SDL3::SDL3-static)
    if(NOT WIN32)
        target_link_libraries(options_validate_test PRIVATE m)
    endif()
    set_target_properties(options_validate_test PROPERTIES C_STANDARD 11)
    add_test(NAME options_validate_test COMMAND options_validate_test)
endif()

# record_encode_bench - encode-throughput validation for --record. Drives the swscale RGBA->yuv420p ->
# libx264 path with synthetic frames as fast as the encoder allows and
# reports the sustained fps (480 is the target). Self-contained: finds
# libav* locally so the unit-test dir needn't depend on link order with
# core/. The ctest floor is loose (a gross-regression catch); the real
# number is printed in the RESULT line.
# Native only: host pkg-config resolves host libav, which would poison a
# cross compile (and there's no target libav to link). Mirrors the guard in
# core/CMakeLists.txt.
# Gated on OPENPETE_ENABLE_RECORD too: distribution presets set it OFF, and a
# dist build must not link the GPL libav* anywhere in the tree - not even in a
# (never-shipped) test binary.
find_package(PkgConfig QUIET)
if(OPENPETE_ENABLE_RECORD AND PkgConfig_FOUND AND NOT CMAKE_CROSSCOMPILING)
    pkg_check_modules(BENCH_LIBAV QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale)
    if(BENCH_LIBAV_FOUND)
        add_executable(record_encode_bench
            ${CMAKE_SOURCE_DIR}/tests/bench/record_encode_bench.c)
        target_link_libraries(record_encode_bench PRIVATE PkgConfig::BENCH_LIBAV)
        set_target_properties(record_encode_bench PROPERTIES C_STANDARD 11)
        add_test(NAME record_encode_bench
                 COMMAND record_encode_bench --width 854 --height 480
                         --frames 1000 --fps 480 --min-fps 60
                         --out ${CMAKE_CURRENT_BINARY_DIR}/encode-bench.mkv)
        set_tests_properties(record_encode_bench PROPERTIES
            LABELS "slow;tooling"
            # An encoder throughput floor measured beside a game process
            # measures the machine; share the game tests' lock.
            RESOURCE_LOCK openpete_game
            TIMEOUT 120)
    endif()
endif()

# openpete_osd_test - the OSD toast ring's state machine on a synthetic
# clock (coalescing, the ttl's first-geometry start, id skew between the two
# readers, eviction, the rate limit, the stack glide). openpete_osd.c is
# compiled directly and SDL's mutex API is faked in the test, so the link
# line stays SDL-free like the rest of the console tests here. log.c comes
# along for the toast ring's debug lines.
add_executable(openpete_osd_test
    openpete_osd_test.c
    ${CMAKE_SOURCE_DIR}/core/ui/openpete_osd.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(openpete_osd_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
target_include_directories(openpete_osd_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/SDL/include)
set_target_properties(openpete_osd_test PROPERTIES C_STANDARD 11)
add_test(NAME openpete_osd_test COMMAND openpete_osd_test)

# vstream_test - the moby vertex-stream resolver + handle table
# (core/psx/vstream.c): stock pass-through, handle indexing in both
# encodings, bind-once entries, and the bounded host alias through a
# registered region reached via a top-bit-masked (KUSEG) pointer.
add_executable(vstream_test
    vstream_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/vstream.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(vstream_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(vstream_test PROPERTIES C_STANDARD 99)
add_test(NAME vstream_test COMMAND vstream_test)

# mod_arena_test - the guest arena's two pools (core/mods/mod_arena.c):
# pool selection by flag, the upper-then-lower segment order, per-segment
# exhaustion naming every owner, the version-3 section mapped onto the
# upper segment, the version-4 and in-memory snapshot round trips, KSEG2
# refusal, a read past a committed extent answered by the ROM model, and
# the rewind page tables (core/record/rewind_pages.c) over the three ranges.
# Links the region table, the bus accessors, the stream-handle table and
# the flat vector; the logger, CD-ROM registers and BIOS ROM are stubs.
add_executable(mod_arena_test
    mod_arena_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_arena.c
    ${CMAKE_SOURCE_DIR}/core/record/rewind_pages.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/psx/vstream.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(mod_arena_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_arena_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_arena_test COMMAND mod_arena_test)
set_tests_properties(mod_arena_test PROPERTIES LABELS "mods")

# mod_chain_test - the override frame stack (core/mods/mod_chain.c): a
# removal of a frame's own link or of links ahead of it while the frame is
# live keeps base() on the next remaining link, a removal at index 0 and a
# re-insert included; base() and crash attribution under a site opened over
# a live frame; a bus frame refused by base(), named by attribution, left
# alone by the re-anchor. Includes the module whole; the dispatch layer,
# overlay probe, ImGui guard and clock are stubs, SDL only its headers (the
# guard thread id is stubbed in the test).
add_executable(mod_chain_test
    mod_chain_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(mod_chain_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods)
target_include_directories(mod_chain_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/SDL/include)
set_target_properties(mod_chain_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_chain_test COMMAND mod_chain_test)
set_tests_properties(mod_chain_test PROPERTIES LABELS "mods")

# mod_bus_test - the bus walker, membership snapshot and key table
# (core/mods/mod_bus.c) over the override frame stack (core/mods/mod_chain.c):
# walk order across a re-entry, results, the depth cap and a full frame
# stack, the saved register set and sp reserve, base() refused in a handler,
# the context and owner refusals, members disabled, retired or entered while
# a walk is in progress, record sizes, and a reader thread polling
# bus_members and mod_info across publishes. Includes both modules whole;
# host services are stubs.
add_executable(mod_bus_test
    mod_bus_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(mod_bus_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods)
target_include_directories(mod_bus_test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/SDL/include)
target_compile_definitions(mod_bus_test PRIVATE OPENPETE_ENGINE_BUILD)
find_package(Threads REQUIRED)
target_link_libraries(mod_bus_test PRIVATE Threads::Threads)
set_target_properties(mod_bus_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_bus_test COMMAND mod_bus_test)
set_tests_properties(mod_bus_test PROPERTIES LABELS "mods")

# mod_save_block_test - the mod-save key's block a savestate carries
# (core/mods/mod_save.c): a block with a legacy length, key, key slot or
# save index out of range is refused at parse, and a well-formed block
# restores and saves back unchanged. Includes the module whole; host
# services are stubs.
add_executable(mod_save_block_test
    mod_save_block_test.c
    ${CMAKE_SOURCE_DIR}/core/host/sha256.c
    ${CMAKE_SOURCE_DIR}/core/config/paths.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(mod_save_block_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_save_block_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_save_block_test COMMAND mod_save_block_test)
set_tests_properties(mod_save_block_test PROPERTIES LABELS "mods")

# mod_save_sidecar_test - the per-card .modsave file (core/mods/mod_save.c)
# on disk: a sidecar that exists but cannot be read (mode 0200) is
# byte-identical after a save and is merged into once readable; two
# processes holding one card directory each keep the other's snapshots,
# in sequence and saving concurrently under the lock. Includes the module
# whole; the atomic write and the lock are the real ones, other host
# services are stubs. POSIX only: fork and chmod.
if(NOT WIN32)
    add_executable(mod_save_sidecar_test
        mod_save_sidecar_test.c
        ${CMAKE_SOURCE_DIR}/core/host/sha256.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(mod_save_sidecar_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt
        ${CMAKE_SOURCE_DIR}/core/mods)
    set_target_properties(mod_save_sidecar_test PROPERTIES C_STANDARD 11)
    add_test(NAME mod_save_sidecar_test
             COMMAND mod_save_sidecar_test ${CMAKE_CURRENT_BINARY_DIR}/mod_save_sidecar_test.d)
    set_tests_properties(mod_save_sidecar_test PROPERTIES LABELS "mods")
endif()

# text_moby_id_test - the HUD text-glyph provenance table
# (games/spyro-1/renderer/text_moby_id.c): keys are the emitter call, not
# the pool address; a generation survives its own draw's drain and is
# cleared by a drain with no emit between it (a draw that allocates pool
# mobys without the text emitters, e.g. the credits roll); the class guard.
add_executable(text_moby_id_test
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c
    text_moby_id_test.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/renderer/text_moby_id.c)
target_include_directories(text_moby_id_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(text_moby_id_test PROPERTIES C_STANDARD 99)
add_test(NAME text_moby_id_test COMMAND text_moby_id_test)

# mod_shader_compile_test - compiles every shipped mod shader (mods/*/**.vert
# and .frag) against the SDK staged next to Spyro 1's executable, through the
# compile-on-boot front end (core/mods/mod_glsl.c) - same glslang settings,
# include path and profiles - and fails on any error or when no material
# fragment was compiled. The shaders are otherwise compiled only when a mod
# loads at boot, so an SDK header change can break every material mod with
# no build or test noticing.
if(OPENPETE_BUILD_GLSLANG AND TARGET openpete-spyro1)
    add_executable(mod_shader_compile_test
        mod_shader_compile_test.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_glsl.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_cdata.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(mod_shader_compile_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/mods
        ${CMAKE_SOURCE_DIR}/third_party)
    target_include_directories(mod_shader_compile_test SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/third_party/glslang/include)
    target_link_libraries(mod_shader_compile_test PRIVATE
        glslang glslang-default-resource-limits)
    set_target_properties(mod_shader_compile_test PROPERTIES C_STANDARD 99)
    # The staged SDK is the directory compile-on-boot resolves includes in.
    add_dependencies(mod_shader_compile_test spyro1_sdk_stage)
    openpete_game_bin(spyro-1 _shader_bin _shader_args)
    get_filename_component(_shader_dir "${_shader_bin}" DIRECTORY)
    # spirv-val, when the host has it, validates every module the
    # constant-table move rewrites; the fixture run requires one to move.
    find_program(OPENPETE_SPIRV_VAL spirv-val)
    set(_spirv_val "${OPENPETE_SPIRV_VAL}")
    if(NOT OPENPETE_SPIRV_VAL)
        set(_spirv_val "")
    endif()
    add_test(NAME mod_shader_compile
             COMMAND mod_shader_compile_test ${CMAKE_SOURCE_DIR}/mods
                     ${_shader_dir}/sdk "${_spirv_val}")
    set_tests_properties(mod_shader_compile PROPERTIES LABELS "mods")
    # mod_shader_stamp_test - the mod shader compile cache's stamp
    # (core/mods/mod_shader_stamp.c): it records every file a compile
    # includes (nested SDK includes, mod-local and subdirectory includes,
    # absent SDK probes), so an edit to any of them makes the cached SPIR-V
    # stale; the prebuilt interface hash covers every staged SDK shader.
    # POSIX only (mkdtemp, mkdir with a mode).
    if(NOT WIN32)
        add_executable(mod_shader_stamp_test
            mod_shader_stamp_test.c
            ${CMAKE_SOURCE_DIR}/core/mods/mod_shader_stamp.c
            ${CMAKE_SOURCE_DIR}/core/mods/mod_glsl.c
            ${CMAKE_SOURCE_DIR}/core/host/sha256.c
            ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
            ${CMAKE_SOURCE_DIR}/core/diag/log.c)
        target_include_directories(mod_shader_stamp_test PRIVATE
            ${CMAKE_SOURCE_DIR}/core/include
            ${CMAKE_SOURCE_DIR}/core/mods
            ${CMAKE_SOURCE_DIR}/third_party)
        target_include_directories(mod_shader_stamp_test SYSTEM PRIVATE
            ${CMAKE_BINARY_DIR}/third_party/glslang/include)
        target_link_libraries(mod_shader_stamp_test PRIVATE
            glslang glslang-default-resource-limits)
        set_target_properties(mod_shader_stamp_test PROPERTIES C_STANDARD 99)
        add_dependencies(mod_shader_stamp_test spyro1_sdk_stage)
        add_test(NAME mod_shader_stamp
                 COMMAND mod_shader_stamp_test ${_shader_dir}/sdk
                         ${CMAKE_SOURCE_DIR}/mods/declarative-reshade/shaders/tint.frag)
        set_tests_properties(mod_shader_stamp PROPERTIES LABELS "mods")
    endif()
    add_test(NAME mod_shader_table_move
             COMMAND mod_shader_compile_test
                     ${CMAKE_CURRENT_SOURCE_DIR}/data/spv-cdata-mods
                     ${_shader_dir}/sdk "${_spirv_val}" --expect-moved)
    set_tests_properties(mod_shader_table_move PROPERTIES LABELS "mods")

    # mod_material_link_test - material composition by source prefixing
    # (core/mods/mod_mat_link.c): every in-tree material shader classified
    # linkable or not with its reason (each linkable but mat-chain-unlinkable,
    # which declares a buffer of its own to be unlinkable), the fixture cases under
    # data/mat-link/cases.tsv, and every combined module compiled, checked by
    # spirv-val and rebuilt in other member orders to the same bytes. The
    # _vk test creates each module's pipelines on a headless device under
    # the Khronos validation layer (exit 77 without a device or the layer).
    add_executable(mod_material_link_test
        mod_material_link_test.c
        mod_material_link_vk.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_mat_link.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_reflect.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_glsl.c
        ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_cdata.c
        ${CMAKE_SOURCE_DIR}/core/host/sha256.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(mod_material_link_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/mods
        ${CMAKE_SOURCE_DIR}/core/rhi
        ${CMAKE_SOURCE_DIR}/third_party)
    target_include_directories(mod_material_link_test SYSTEM PRIVATE
        ${CMAKE_BINARY_DIR}/third_party/glslang/include)
    target_link_libraries(mod_material_link_test PRIVATE
        glslang glslang-default-resource-limits openpete_rhi SDL3::SDL3-static ${CMAKE_DL_LIBS})
    set_target_properties(mod_material_link_test PROPERTIES C_STANDARD 11)
    add_dependencies(mod_material_link_test spyro1_sdk_stage)
    add_test(NAME mod_material_link
             COMMAND mod_material_link_test ${_shader_dir}/sdk "${_spirv_val}"
                     --expect-linkable
                     --expect-unlinkable mat-chain-unlinkable/shaders/tint_table.frag
                     --census ${CMAKE_SOURCE_DIR}/mods
                     --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/data/mat-link)
    set_tests_properties(mod_material_link PROPERTIES LABELS "mods")
    add_test(NAME mod_material_link_vk
             COMMAND mod_material_link_test ${_shader_dir}/sdk "${_spirv_val}" --vk
                     --expect-linkable
                     --expect-unlinkable mat-chain-unlinkable/shaders/tint_table.frag
                     --census ${CMAKE_SOURCE_DIR}/mods
                     --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/data/mat-link)
    set_tests_properties(mod_material_link_vk PROPERTIES LABELS "mods;vulkan"
                         SKIP_RETURN_CODE 77)
endif()

# rhi_smoketest --headless - the RHI self-checking cases on a surfaceless
# device (llvmpipe where no GPU is present): a vertex stage reading its
# positions and colours from a retained storage buffer must reproduce the
# expected image exactly on four frames that cover commit, persistence and
# a same-frame rewrite, and a compute pass over a per-present input must
# produce words read back exactly and vertex records a draw of the same
# present reads (core/rhi/smoketest.c). Exit 77 without a device.
# rhi_smoketest_compute_control: the compute case with a perturbed input;
# passes only when every word and present mismatches.
# rhi_smoketest_compute_off: OPENPETE_RHI_COMPUTE=0 makes the device report
# compute unavailable; the compute case must skip with the pipeline create
# refused while the other cases pass.
if(TARGET rhi_smoketest)
    # rhi_smoketest is defined in core/rhi; this target puts it in
    # tests/unit/all, the target that builds every program these tests run.
    add_custom_target(rhi_smoketest_unit ALL)
    add_dependencies(rhi_smoketest_unit rhi_smoketest)
    add_test(NAME rhi_smoketest_headless COMMAND rhi_smoketest --headless)
    set_tests_properties(rhi_smoketest_headless PROPERTIES LABELS "vulkan"
                         SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME rhi_smoketest_compute_control COMMAND rhi_smoketest --compute-control)
    set_tests_properties(rhi_smoketest_compute_control PROPERTIES LABELS "vulkan"
                         SKIP_RETURN_CODE 77 TIMEOUT 120)
    add_test(NAME rhi_smoketest_compute_off COMMAND rhi_smoketest --headless)
    set_tests_properties(rhi_smoketest_compute_off PROPERTIES LABELS "vulkan"
                         ENVIRONMENT "OPENPETE_RHI_COMPUTE=0"
                         PASS_REGULAR_EXPRESSION "compute case: SKIP compute unavailable \\(OPENPETE_RHI_COMPUTE=0\\); pipeline create refused"
                         FAIL_REGULAR_EXPRESSION "FAIL"
                         TIMEOUT 120)
endif()

# rhi_memtype_candidates_test - the memory-type preference order behind the
# terrain arenas' allocations (core/rhi/rhi_memtype.c). Compiles that one
# translation unit against the vendored Vulkan headers: the function reads a
# reported memory table and needs no instance, device or loader, so the test
# links nothing.
add_executable(rhi_memtype_candidates_test
    rhi_memtype_candidates_test.c
    ${CMAKE_SOURCE_DIR}/core/rhi/rhi_memtype.c)
target_include_directories(rhi_memtype_candidates_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/rhi
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include)
set_target_properties(rhi_memtype_candidates_test PROPERTIES C_STANDARD 99)
add_test(NAME rhi_memtype_candidates_test COMMAND rhi_memtype_candidates_test)

# mod_vec_test - the mod surface's two growable tables
# (core/mods/mod_vec.c): the chunked table's growth past one block and the
# element addresses that survive it, the flat table's contiguity after a
# realloc, the order-preserving remove, clear keeping storage, the no-ops
# on a zeroed table, and the exit census line. mod_vec.c reaches nothing
# but malloc and the logger, so log.c is the whole link line.
add_executable(mod_vec_test
    mod_vec_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(mod_vec_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_vec_test PROPERTIES C_STANDARD 99)
add_test(NAME mod_vec_test COMMAND mod_vec_test)
set_tests_properties(mod_vec_test PROPERTIES LABELS "mods")

# mod_material_resolve_test - the material resolver
# (core/mods/mod_material_resolve.c) against stub loader hooks: per case,
# materials for stub mods (enabled-list position, api pin), the extraction,
# bake and draw-time outcome, and the resolve census counters.
add_executable(mod_material_resolve_test
    mod_material_resolve_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_material_resolve.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(mod_material_resolve_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods)
target_compile_definitions(mod_material_resolve_test PRIVATE OPENPETE_ENGINE_BUILD)
set_target_properties(mod_material_resolve_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_material_resolve_test COMMAND mod_material_resolve_test)
set_tests_properties(mod_material_resolve_test PROPERTIES LABELS "mods")

# mod_games_test - the games a mod names besides the one it runs on
# (core/mods/mod_games.c): list merging and limits, the route of a disc call
# for every role and install state against a fake library lookup, the first
# missing required game, the installed query and the refused-mod rows.
add_executable(mod_games_test
    mod_games_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_games.c)
target_include_directories(mod_games_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_games_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_games_test COMMAND mod_games_test)
set_tests_properties(mod_games_test PROPERTIES LABELS "mods")

# tracer_snapshot_test - the Spyro 1 tracer set copy clamps the guest set
# count to the game's four-entry tables (fake bus, no game link).
add_executable(tracer_snapshot_test
    tracer_snapshot_test.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/renderer/tracer_snapshot.c)
target_include_directories(tracer_snapshot_test PRIVATE
    ${CMAKE_SOURCE_DIR}/games/spyro-1
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(tracer_snapshot_test PROPERTIES C_STANDARD 11)
add_test(NAME tracer_snapshot_test COMMAND tracer_snapshot_test)

# boot_memfill_test - a crafted EXE header whose memfill range wraps in
# 32 bits is skipped (boot.c + cpu.c with local stubs).
add_executable(boot_memfill_test
    boot_memfill_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/boot.c
    ${CMAKE_SOURCE_DIR}/core/psx/cpu.c
    ${CMAKE_SOURCE_DIR}/core/psx/psx_region.c)
target_include_directories(boot_memfill_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
set_target_properties(boot_memfill_test PROPERTIES C_STANDARD 11)
add_test(NAME boot_memfill_test COMMAND boot_memfill_test)

# exe_loader_cap_test - the disc EXE loader refuses a directory size over
# 2 MiB + 2048 before allocating (crafted ISO9660 image, disc_image reader).
add_executable(exe_loader_cap_test
    exe_loader_cap_test.cpp
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
target_include_directories(exe_loader_cap_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_link_libraries(exe_loader_cap_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(exe_loader_cap_test PRIVATE winmm)
endif()
set_target_properties(exe_loader_cap_test PROPERTIES CXX_STANDARD 20)
add_test(NAME exe_loader_cap_test COMMAND exe_loader_cap_test)

# disc_ingest_resolve_test - the .bin a disc path resolves to, which the
# library's ingest copies and reads the serial from: a cue's FILE line wins
# over another game's .bin beside it or a subdirectory, the first of several
# FILE lines is taken, and two unnamed .bin files resolve to nothing.
add_executable(disc_ingest_resolve_test
    disc_ingest_resolve_test.cpp
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
target_include_directories(disc_ingest_resolve_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt)
target_link_libraries(disc_ingest_resolve_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(disc_ingest_resolve_test PRIVATE winmm)
endif()
set_target_properties(disc_ingest_resolve_test PROPERTIES CXX_STANDARD 20)
add_test(NAME disc_ingest_resolve_test COMMAND disc_ingest_resolve_test)

# bugreport_redact_test - the bug-report redactor and redaction pass on a
# made-up identity given by parameter: whole-word user and host names in
# text, the member drops (redaction out of memory, a bk2 or state image that
# does not parse, an identity surviving redaction, an identity in a member's
# name) and the flag on a scanned binary member. The state-image functions
# are stubbed in the test.
add_executable(bugreport_redact_test
    bugreport_redact_test.c
    ${CMAKE_SOURCE_DIR}/core/record/bugreport_redact.c
    ${CMAKE_SOURCE_DIR}/core/record/bugreport_pass.c
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(bugreport_redact_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/record
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/third_party/miniz)
target_link_libraries(bugreport_redact_test PRIVATE SDL3::SDL3-static)
set_target_properties(bugreport_redact_test PROPERTIES C_STANDARD 11)
add_test(NAME bugreport_redact_test COMMAND bugreport_redact_test)

# disc_cache_short_read_test - a chunk the disc cache loader reads short is
# retried, then served from the file, and never served as stale heap bytes.
# The test TU includes disc_cache.c with fread and malloc replaced.
add_executable(disc_cache_short_read_test
    disc_cache_short_read_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_fs_probe.c
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
target_include_directories(disc_cache_short_read_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include)
target_link_libraries(disc_cache_short_read_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(disc_cache_short_read_test PRIVATE winmm)
endif()
set_target_properties(disc_cache_short_read_test PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
add_test(NAME disc_cache_short_read_test COMMAND disc_cache_short_read_test)

# disc_cache_auto_test - --disc-cache auto end to end on a synthetic image:
# a fast file detaches and a cold burst re-attaches it through the watchdog;
# a slow file keeps the cache. The link wraps pread (and pread64) with the
# test's, which injects the latency.
if(NOT WIN32)
    add_executable(disc_cache_auto_test
        disc_cache_auto_test.c
        ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
        ${CMAKE_SOURCE_DIR}/core/psx/disc_cache.c
        ${CMAKE_SOURCE_DIR}/core/psx/disc_fs_probe.c
        ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
        ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
        ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
        ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
    target_include_directories(disc_cache_auto_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt
        ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include)
    target_link_libraries(disc_cache_auto_test PRIVATE SDL3::SDL3-static)
    target_link_options(disc_cache_auto_test PRIVATE -Wl,--wrap=pread,--wrap=pread64)
    set_target_properties(disc_cache_auto_test PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
    add_test(NAME disc_cache_auto_test_fast COMMAND disc_cache_auto_test fast)
    add_test(NAME disc_cache_auto_test_slow COMMAND disc_cache_auto_test slow)
endif()

# disc_fs_probe_test - the --disc-cache auto classifier on injected statfs
# f_type values, Windows drive types and UNC paths, and the probe on the
# host's temporary directory.
add_executable(disc_fs_probe_test
    disc_fs_probe_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_fs_probe.c)
target_include_directories(disc_fs_probe_test PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
set_target_properties(disc_fs_probe_test PROPERTIES C_STANDARD 11)
add_test(NAME disc_fs_probe_test COMMAND disc_fs_probe_test)

# disc_image_test - the disc reader on synthetic raw images written by the
# test: PVD, root and one-subdirectory lookup, ';1' and case folding, the
# 2048-byte refusal, sector and user-data reads, cache hits and misses
# against direct reads, and the booted-disc read_sector the mod API forwards
# to (with the api table's appended slot).
add_executable(disc_image_test
    disc_image_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_cache.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_fs_probe.c
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
target_include_directories(disc_image_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include)
target_link_libraries(disc_image_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(disc_image_test PRIVATE winmm)
endif()
set_target_properties(disc_image_test PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
add_test(NAME disc_image_test COMMAND disc_image_test)

# disc_build_test - disc build classification: synthetic SYSTEM.CNF texts and
# executables into every class and warning (the decomp checkouts' PSX.EXE
# files stand in for the supported executables), the BOOT-line parser, the
# player message and toast texts, and the image reader and the boot check
# on written raw images. OPENPETE_DISC_BUILD_REAL=1 adds the three
# checkouts' discs with the time of each read.
add_executable(disc_build_test
    disc_build_test.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_build.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_build_boot.c
    ${CMAKE_SOURCE_DIR}/games/spyro-1/disc_builds.c
    ${CMAKE_SOURCE_DIR}/games/spyro-2/disc_builds.c
    ${CMAKE_SOURCE_DIR}/games/spyro-3/disc_builds.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_image.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_cache.c
    ${CMAKE_SOURCE_DIR}/core/psx/disc_fs_probe.c
    ${CMAKE_SOURCE_DIR}/core/psx/exe_loader.cpp
    ${CMAKE_SOURCE_DIR}/core/host/sha256.c
    ${CMAKE_SOURCE_DIR}/core/diag/disc_trace.c
    ${CMAKE_SOURCE_DIR}/core/diag/error_ids.c
    ${CMAKE_SOURCE_DIR}/core/runtime/openpete_time.c
    ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
target_include_directories(disc_build_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include)
target_compile_definitions(disc_build_test PRIVATE
    OPENPETE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(disc_build_test PRIVATE SDL3::SDL3-static)
if(WIN32)
    target_link_libraries(disc_build_test PRIVATE winmm)
endif()
set_target_properties(disc_build_test PROPERTIES C_STANDARD 11 CXX_STANDARD 20)
add_test(NAME disc_build_test COMMAND disc_build_test)

# sfx_smpl_walk_test - the WAV smpl chunk walk ends on a chunk that runs past
# the file and refuses loop points outside int range. The test TU includes
# sfx_pack.c with stubs for PsyCross, the mod registry and paths.
add_executable(sfx_smpl_walk_test
    sfx_smpl_walk_test.c
    ${CMAKE_SOURCE_DIR}/core/host/sfx_ident.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
target_include_directories(sfx_smpl_walk_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/mods
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/xxhash)
target_link_libraries(sfx_smpl_walk_test PRIVATE SDL3::SDL3-static)
set_target_properties(sfx_smpl_walk_test PROPERTIES C_STANDARD 11)
add_test(NAME sfx_smpl_walk_test COMMAND sfx_smpl_walk_test)

# spual_interp_test - the SPU mixer's 4-point gaussian for stock voices,
# looping sample-override voices across the loop seam, and host clips,
# against the psx-spx formula over the sequence each voice plays. The TU
# includes PsyX_SPUAL.cpp and stubs its other PsyCross dependencies.
add_executable(spual_interp_test spual_interp_test.cpp
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio/PsyX_SPUEnvelope.cpp)
target_include_directories(spual_interp_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio)
target_include_directories(spual_interp_test PRIVATE ${OPENAL_INCLUDE_DIR})
target_link_libraries(spual_interp_test PRIVATE SDL3::SDL3-static ${OPENAL_LIBRARY})
target_compile_options(spual_interp_test PRIVATE -Wno-unused-parameter -Wno-maybe-uninitialized)
set_target_properties(spual_interp_test PROPERTIES CXX_STANDARD 20)
add_test(NAME spual_interp_test COMMAND spual_interp_test)

# spu_envelope_test - the SPU level generators of PsyX_SPUEnvelope.cpp (a
# voice's ADSR envelope, its ENVX, and the volume-register sweeps) against
# the reference's measured levels, every 44.1 kHz boundary of every case of
# tests/unit/data/spu-envelope/reference.env (tools/spu_envelope_measure.py),
# with no case exempt. spu_envelope_test_wrong builds the test with every
# attack tick off by one, which must report an attack case as mismatching.
foreach(_spu_env_variant test test_wrong)
    add_executable(spu_envelope_${_spu_env_variant} spu_envelope_test.cpp
        ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio/PsyX_SPUEnvelope.cpp)
    target_include_directories(spu_envelope_${_spu_env_variant} PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio)
    set_target_properties(spu_envelope_${_spu_env_variant} PROPERTIES CXX_STANDARD 20)
    add_test(NAME spu_envelope_${_spu_env_variant} COMMAND spu_envelope_${_spu_env_variant}
             ${CMAKE_CURRENT_SOURCE_DIR}/data/spu-envelope/reference.env)
endforeach()
target_compile_definitions(spu_envelope_test_wrong PRIVATE SPU_ENV_TEST_WRONG=1)
set_tests_properties(spu_envelope_test_wrong PROPERTIES
    PASS_REGULAR_EXPRESSION "attack_lin_s0b_st0 mismatches")

# xa_image_end_test - the XA decoder's image reads stay inside the disc
# image while the drive head runs past its last sector, and it reads up to
# that sector. The TU includes PsyX_XA.cpp with the image reader and the CD
# input stubbed.
add_executable(xa_image_end_test xa_image_end_test.cpp)
target_include_directories(xa_image_end_test PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio)
target_link_libraries(xa_image_end_test PRIVATE SDL3::SDL3-static)
set_target_properties(xa_image_end_test PROPERTIES CXX_STANDARD 20)
add_test(NAME xa_image_end_test COMMAND xa_image_end_test)

# sfx_name_parse_test - sound pack file names classify as hash, symbolic or
# ignored, with case-insensitive ".wav" and "-flat". sfx_ident.c compiles
# with PsyCross's include/psx on the path, as in openpete_core.
add_executable(sfx_name_parse_test
    sfx_name_parse_test.c
    ${CMAKE_SOURCE_DIR}/core/host/sfx_ident.c)
target_include_directories(sfx_name_parse_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/xxhash)
set_target_properties(sfx_name_parse_test PROPERTIES C_STANDARD 11)
add_test(NAME sfx_name_parse_test COMMAND sfx_name_parse_test)

# rhi_compile_stress - variant-explicit compile jobs run on worker threads
# while the mod registry's per-tick writers, enable toggles and hot reloads
# run on a guest thread and the display owner makes jobs, edits the
# environment, installs results and retires pipelines (contract in the
# source). Compiles the mod registry with local stubs for the loader, the
# GLSL front end and the renderer, and links the RHI. Needs a Vulkan device:
# exit 77 (no device) is a skip. The verdict on lock contention counts
# rather than times: the link wraps SDL's read-write lock calls
# (RHI_STRESS_LINK) so the harness sees which threads take the registry
# lock, and it routes volk's vkCreateGraphicsPipelines through a census of
# pipelines created holding that lock.
set(RHI_STRESS_SOURCES
    rhi_compile_stress.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_shader.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_shader_stamp.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_material_resolve.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_cdata.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_spv_reflect.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_mat_link.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_vec.c
    ${CMAKE_SOURCE_DIR}/core/host/sha256.c
    ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
    ${CMAKE_SOURCE_DIR}/core/diag/log.c)
set(RHI_STRESS_INCLUDES
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/mods
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/rhi
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
    ${CMAKE_SOURCE_DIR}/third_party/volk)
set(RHI_STRESS_LINK
    -Wl,--wrap=SDL_LockRWLockForReading
    -Wl,--wrap=SDL_LockRWLockForWriting
    -Wl,--wrap=SDL_UnlockRWLock)
add_executable(rhi_compile_stress ${RHI_STRESS_SOURCES})
target_include_directories(rhi_compile_stress PRIVATE ${RHI_STRESS_INCLUDES})
target_compile_definitions(rhi_compile_stress PRIVATE OPENPETE_NO_GLSLANG VK_NO_PROTOTYPES)
target_link_options(rhi_compile_stress PRIVATE ${RHI_STRESS_LINK})
target_link_libraries(rhi_compile_stress PRIVATE openpete_rhi SDL3::SDL3-static ${CMAKE_DL_LIBS})
# INTERPROCEDURAL_OPTIMIZATION OFF: under LTO the plugin drops the __wrap_
# definitions and the SDL definitions the __real_ names reach, since no IR
# object names either (tests/sched/CMakeLists.txt). As regular objects,
# mod_shader.c's calls reach the wraps and the wraps reach SDL's IR.
set_target_properties(rhi_compile_stress PROPERTIES C_STANDARD 11 INTERPROCEDURAL_OPTIMIZATION OFF)
add_test(NAME rhi_compile_stress COMMAND rhi_compile_stress 4 3)
set_tests_properties(rhi_compile_stress PROPERTIES SKIP_RETURN_CODE 77 LABELS "vulkan")
# The compile pool's step-down at the first tick, on host tasks: one worker
# per logical core before it, at most half of them running after it.
add_test(NAME rhi_compile_pool_boot COMMAND rhi_compile_stress 0 0 - boot)
set_tests_properties(rhi_compile_pool_boot PROPERTIES SKIP_RETURN_CODE 77 LABELS "vulkan")

# The same harness with the RHI compiled in under ThreadSanitizer. Built on
# request only (cmake --build --preset linux-release --target
# rhi_compile_stress_tsan); run under `setarch "$(uname -m)" -R` where the
# kernel's mmap randomization exceeds the sanitizer's layout.
if(NOT WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    get_property(OPENPETE_RHI_SOURCES GLOBAL PROPERTY OPENPETE_RHI_SOURCES)
    foreach(src IN LISTS OPENPETE_RHI_SOURCES)
        if(src MATCHES "rhi_shaders_embedded\\.c$")
            set_source_files_properties(${src} PROPERTIES GENERATED TRUE)
        endif()
    endforeach()
    add_executable(rhi_compile_stress_tsan EXCLUDE_FROM_ALL
        ${RHI_STRESS_SOURCES} ${OPENPETE_RHI_SOURCES})
    target_include_directories(rhi_compile_stress_tsan PRIVATE ${RHI_STRESS_INCLUDES})
    target_compile_definitions(rhi_compile_stress_tsan PRIVATE OPENPETE_NO_GLSLANG VK_NO_PROTOTYPES)
    target_compile_options(rhi_compile_stress_tsan PRIVATE -fsanitize=thread -g -O1)
    target_link_options(rhi_compile_stress_tsan PRIVATE -fsanitize=thread ${RHI_STRESS_LINK})
    target_link_libraries(rhi_compile_stress_tsan PRIVATE SDL3::SDL3-static ${CMAKE_DL_LIBS})
    set_target_properties(rhi_compile_stress_tsan PROPERTIES C_STANDARD 11 INTERPROCEDURAL_OPTIMIZATION OFF)
    # openpete_rhi's build generates the embedded-shader source.
    add_dependencies(rhi_compile_stress_tsan openpete_rhi)
endif()

# write_census_test - the write census range table: a spec of twelve ranges
# logs a row for a store into each range past the eighth, and a malformed
# range refuses the whole spec. Compiles write_census.c with stubs for the
# overlay table; the CPU is a struct with counting store hooks. SDL3 is
# linked for the headers openpete_fiber.h (openpete_frame) includes.
add_executable(write_census_test
    write_census_test.c
    ${CMAKE_SOURCE_DIR}/games/common/write_census.c)
target_include_directories(write_census_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/games/common)
set_target_properties(write_census_test PROPERTIES C_STANDARD 11)
target_link_libraries(write_census_test PRIVATE SDL3::SDL3-static)
add_test(NAME write_census_ranges COMMAND write_census_test ranges
    ${CMAKE_CURRENT_BINARY_DIR}/write_census_ranges.csv)
add_test(NAME write_census_refuse COMMAND write_census_test refuse
    ${CMAKE_CURRENT_BINARY_DIR}/write_census_refuse.csv)

# launcher_fit_test - header-only. The launcher window's design size scaled
# by a display content scale and fitted to a display's full and usable
# bounds, as on Windows, at four scales on three taskbar displays.
add_executable(launcher_fit_test launcher_fit_test.c)
target_include_directories(launcher_fit_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include)
if(NOT WIN32)
    target_link_libraries(launcher_fit_test PRIVATE m)
endif()
set_target_properties(launcher_fit_test PROPERTIES C_STANDARD 99)
add_test(NAME launcher_fit_test COMMAND launcher_fit_test)

# launcher_page_lines_test - the game page's lines below its boxes: their text
# for 0 to 3 games in the library, and their fit at the supported windows,
# measured with the real body font in an imgui context with no backend.
if(TARGET openpete_imgui)
    add_executable(launcher_page_lines_test
        launcher_page_lines_test.cpp
        ${CMAKE_SOURCE_DIR}/core/launcher/launcher_font.cpp
        ${CMAKE_SOURCE_DIR}/core/config/game_registry.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c)
    target_include_directories(launcher_page_lines_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/launcher
        ${CMAKE_SOURCE_DIR}/core/include
        ${CMAKE_SOURCE_DIR}/core/include/psxrt)
    target_link_libraries(launcher_page_lines_test PRIVATE openpete_imgui)
    set_target_properties(launcher_page_lines_test PROPERTIES CXX_STANDARD 20 C_STANDARD 11)
    add_test(NAME launcher_page_lines_test COMMAND launcher_page_lines_test)
endif()

# launcher_relaunch_env_test - the environment the return to the launcher
# hands its relaunched image: computed from the entry snapshot over lists,
# then two sessions through the real config layer's full stage, and on POSIX
# read back by an execve'd shell.
if(TARGET SDL3::SDL3-static)
    add_executable(launcher_relaunch_env_test
        launcher_relaunch_env_test.cpp
        ${CMAKE_SOURCE_DIR}/core/config/config.c
        ${CMAKE_SOURCE_DIR}/core/config/config_schema.c
        ${CMAKE_SOURCE_DIR}/core/input/keymap.c
        ${CMAKE_SOURCE_DIR}/core/diag/log.c
        ${CMAKE_SOURCE_DIR}/core/config/paths.c
        ${CMAKE_SOURCE_DIR}/core/host/atomic_file.c
        ${CMAKE_SOURCE_DIR}/third_party/tomlc99/toml.c)
    target_include_directories(launcher_relaunch_env_test PRIVATE
        ${CMAKE_SOURCE_DIR}/core/launcher
        ${CMAKE_SOURCE_DIR}/core/include)
    target_link_libraries(launcher_relaunch_env_test PRIVATE SDL3::SDL3-static)
    set_target_properties(launcher_relaunch_env_test PROPERTIES CXX_STANDARD 20 C_STANDARD 11)
    add_test(NAME launcher_relaunch_env_test COMMAND launcher_relaunch_env_test)
endif()

# These compile sources that carry role asserts and binds, whose definitions
# live in the role runtime.
foreach(t boot_memfill_test line_quad_test mod_arena_test mod_chain_test mod_material_link_test
          mod_material_resolve_test mod_vec_test psx_span_test rhi_compile_stress
          rhi_compile_stress_tsan sfx_smpl_walk_test text_moby_id_test
          write_census_test)
    if(TARGET ${t})
        target_sources(${t} PRIVATE ${CMAKE_SOURCE_DIR}/core/runtime/thread_roles.c)
    endif()
endforeach()

# audio_feed_sim - the audio device feed on its synthetic device, driven by
# a synthetic producer through a producer-speed span (the fourth field of
# OPENPETE_AUDIO_FEED_SIM); reports the queued latency after the span and
# the splice step of every drop. The TU includes PsyX_SPUFeed.cpp; the
# OpenAL calls link but never run. tests/harness/check_audio_feed_latency.sh
# runs its rows.
add_executable(audio_feed_sim audio_feed_sim.cpp)
target_include_directories(audio_feed_sim PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/include/psx
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src
    ${CMAKE_SOURCE_DIR}/third_party/PsyCross/src/audio)
target_include_directories(audio_feed_sim PRIVATE ${OPENAL_INCLUDE_DIR})
target_link_libraries(audio_feed_sim PRIVATE SDL3::SDL3-static ${OPENAL_LIBRARY})
set_target_properties(audio_feed_sim PROPERTIES CXX_STANDARD 20)

# The interleaving scheduler and its harnesses (tests/sched), which carry
# their own `unit` label.
add_subdirectory(${CMAKE_SOURCE_DIR}/tests/sched ${CMAKE_BINARY_DIR}/tests/sched)

# mod_modset_test - the modset tags (core/mods/mod_modset.c): the set form
# byte-identical to the order-free tag cards carry, the ordered form and
# its order hash (entry order, deferred entry, hot reload, late links), and
# the savestate verdict matrix (ordered equal, reordered, other set,
# set-form file). The module is
# pure string code, so it links alone.
add_executable(mod_modset_test
    mod_modset_test.c
    ${CMAKE_SOURCE_DIR}/core/mods/mod_modset.c)
target_include_directories(mod_modset_test PRIVATE
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/mods)
set_target_properties(mod_modset_test PROPERTIES C_STANDARD 11)
add_test(NAME mod_modset_test COMMAND mod_modset_test)
set_tests_properties(mod_modset_test PROPERTIES LABELS "mods")
