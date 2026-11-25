cmake_minimum_required(VERSION 3.22)
project(openpete LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Path remaps for every preset: the source tree reads as `openpete` and the
# build tree as `openpete-build` in every path an object carries (__FILE__,
# assert strings, DWARF names and DW_AT_comp_dir), so a binary a bug report
# ships (the recompiled module, the executable) names no directory of the
# build machine. The build tree comes last because it sits inside the
# source tree in every preset and GCC applies the last remap that matches.
# Set on the flags variables (after project(), where the preset file ran),
# so the PSXRecomp external project that forwards CMAKE_C_FLAGS takes them.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT MSVC)
    set(_op_prefix_map
        "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=openpete -ffile-prefix-map=${CMAKE_BINARY_DIR}=openpete-build")
    string(APPEND CMAKE_C_FLAGS " ${_op_prefix_map}")
    string(APPEND CMAKE_CXX_FLAGS " ${_op_prefix_map}")
    unset(_op_prefix_map)
endif()

# OpenAL has no Config-mode package file in the openal-soft prebuilt, so
# when cross-compiling with MinGW seed the cache vars FindOpenAL.cmake reads.
# Native Linux uses the system OpenAL via FindOpenAL's default search.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW)
    set(_OAL_ROOT "${CMAKE_SOURCE_DIR}/third_party/OpenAL-mingw/x86_64-w64-mingw32")
    # PsyCross uses `#include <AL/al.h>` form, so point at the parent of
    # AL/ (FindOpenAL.cmake's PATH_SUFFIXES allow either, but the one
    # consumers expect is the prefix that makes the angle-bracket include
    # legal). Header lookup: <AL/al.h> -> ${OPENAL_INCLUDE_DIR}/AL/al.h.
    set(OPENAL_INCLUDE_DIR "${_OAL_ROOT}/include"
        CACHE PATH "OpenAL include dir (vendored MinGW prebuilt)" FORCE)
    set(OPENAL_LIBRARY "${_OAL_ROOT}/lib/libOpenAL32.dll.a"
        CACHE FILEPATH "OpenAL import library (vendored MinGW prebuilt)" FORCE)
    unset(_OAL_ROOT)
endif()

# Emscripten ships an OpenAL implementation in its sysroot, linked via the
# `-lopenal` flag rather than a discoverable .a archive. PsyCross calls
# find_package(OpenAL REQUIRED), so seed the cache vars FindOpenAL's
# FindPackageHandleStandardArgs checks (it only verifies they're set and
# not -NOTFOUND). Headers live in the toolchain sysroot, found
# automatically by emcc; OPENAL_INCLUDE_DIR just has to point somewhere
# real for FPHSA. The -lopenal flag also gets appended on the final
# browser target's link line (see games/spyro-1/CMakeLists.txt).
if(EMSCRIPTEN)
    set(OPENAL_INCLUDE_DIR "${CMAKE_SYSROOT}/include"
        CACHE PATH "OpenAL include dir (Emscripten sysroot)" FORCE)
    set(OPENAL_LIBRARY "-lopenal"
        CACHE STRING "OpenAL link flag (Emscripten built-in)" FORCE)
endif()

# SDL3 is built from the third_party/SDL submodule, pinned to the
# release-3.4.x branch (currently the release-3.4.16 tag) rather than to
# main: the release branch carries the 2026 Steam Controller support the
# pin was originally taken from main for, and carries it further along -
# input report formats, touchpads and the rumble fix all landed there
# after that commit. Static link, no shared lib, no SDL test apps -
# produces the SDL3::SDL3-static target consumed by openpete_core and
# PsyCross.
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
# XTEST (X11 synthetic-input extension) is not used by openpete - it only
# polls input, never injects it. Disabling drops the libXtst-dev build
# dependency so a from-source SDL build needs no extra system packages.
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL)

# Marks the include directories target exports as system directories for its
# consumers (-isystem): GCC then reports no warning located in those headers.
# Only for submodules the project does not edit (SDL, glslang, imgui):
# -isystem also drops error-set diagnostics in a macro a system header
# defines, so PsyCross, libco and the third_party/ root stay -I.
function(openpete_system_includes target)
    get_target_property(_real ${target} ALIASED_TARGET)
    if(NOT _real)
        set(_real ${target})
    endif()
    get_target_property(_dirs ${_real} INTERFACE_INCLUDE_DIRECTORIES)
    if(_dirs)
        set_property(TARGET ${_real} APPEND PROPERTY
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES ${_dirs})
    endif()
endfunction()
openpete_system_includes(SDL3::Headers)

# Which games to enable. A game whose generated/ has no recompiled output
# warns and skips itself, so listing one costs nothing until it is regenerated.
set(OPENPETE_GAMES "spyro-1;spyro-2;spyro-3" CACHE STRING "Games to build (semicolon-separated)")

# In-process GLSL->SPIR-V for the mod shader compile service. glslang
# 15.4.0 pinned:
# C++17 under GCC 10 (the MinGW cross compiler's ceiling), targets
# vulkan1.0 like the engine's own glslangValidator -V builds. OFF is a
# degraded build: mods' prebuilt .spv still load, GLSL sources refuse
# with a log pointer.
option(OPENPETE_BUILD_GLSLANG
       "Build third_party/glslang for the mod shader compile service" ON)
if(OPENPETE_BUILD_GLSLANG)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)         # no SPIRV-Tools dep
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    add_subdirectory(third_party/glslang)
    openpete_system_includes(glslang)
    openpete_system_includes(glslang-default-resource-limits)
endif()

# PsyCross integration. Builds the static lib and links it
# into openpete_core; psyq_thunks.cpp routes recompiled PSYQ calls into
# PsyCross. PsyCross's CMake calls find_package(OpenAL REQUIRED), so a
# Linux build needs `libopenal-dev`. Disable with -DOPENPETE_BUILD_PSYCROSS=OFF
# if you need a non-PsyCross link path (the per-game thunks will then
# fall through to the abort-on-call weak stubs in psyq_stubs.c).
option(OPENPETE_BUILD_PSYCROSS "Build third_party/PsyCross as a static lib" ON)
if(OPENPETE_BUILD_PSYCROSS)
    add_subdirectory(third_party/PsyCross)
    # PsyCross walks guest-RAM primitives and ordering tables through
    # type-punned pointers, like the runtime (core/CMakeLists.txt).
    target_compile_options(psycross_static PRIVATE -fno-strict-aliasing)
endif()

# Native render foundation: RHI + Vulkan backend (game-agnostic, under
# core/). Added before core/ so openpete_core can link openpete_rhi.
add_subdirectory(core/rhi)

add_subdirectory(core)

# Host-side unit tests (no PSX boot, just pure-function checks). Testing is
# enabled before the game directories so their per-game ctest rows
# (state_regions_derivation_<game>) register.
option(OPENPETE_BUILD_TESTS "Build host-side unit tests" ON)
if(OPENPETE_BUILD_TESTS)
    enable_testing()
endif()

# Rendered header documentation (Doxyfile at the source root). Not part of
# ALL: `cmake --build --preset <preset> --target openpete_docs` writes
# <build>/docs/html and <build>/docs/xml; without doxygen the target prints
# a SKIP line and succeeds.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(openpete_docs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/docs
        COMMAND ${CMAKE_COMMAND} -E env OPENPETE_DOXYGEN_OUT=${CMAKE_BINARY_DIR}/docs
                ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "doxygen: rendering the headers into ${CMAKE_BINARY_DIR}/docs"
        VERBATIM)
else()
    add_custom_target(openpete_docs
        COMMAND ${CMAKE_COMMAND} -E echo
                "openpete_docs: SKIP, doxygen not found (sudo apt install doxygen)"
        VERBATIM)
endif()

# Codebase reference pages for the documentation site (docs/mkdocs.yml).
# Not part of ALL: `cmake --build --preset <preset> --target code_docs`
# rewrites docs/src/developer/reference/ from the doxygen comments, parsed
# by the documentation ratchet over this tree's compile database.
# Stamp-guarded: it reruns when a scanned source or the generator changes.
file(GLOB_RECURSE _code_docs_inputs CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/core/*.c ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/core/*.h ${CMAKE_CURRENT_SOURCE_DIR}/core/*.hpp
     ${CMAKE_CURRENT_SOURCE_DIR}/games/*.c ${CMAKE_CURRENT_SOURCE_DIR}/games/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/games/*.h ${CMAKE_CURRENT_SOURCE_DIR}/games/*.hpp
     ${CMAKE_CURRENT_SOURCE_DIR}/games/*.inc)
list(FILTER _code_docs_inputs EXCLUDE REGEX
     "/(generated|third_party|build)/|/games/(spyro-1/spyro-1|spyro-2/spyro-2|spyro-3/spyro-3)/")
set(_code_docs_stamp ${CMAKE_CURRENT_BINARY_DIR}/codedocs.stamp)
add_custom_command(
    OUTPUT  ${_code_docs_stamp}
    COMMAND ${CMAKE_COMMAND} -E env
            OPENPETE_DOCS_DB=${CMAKE_BINARY_DIR}/compile_commands.json
            python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_code_docs.py
    COMMAND ${CMAKE_COMMAND} -E touch ${_code_docs_stamp}
    DEPENDS ${_code_docs_inputs}
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_code_docs.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/doc_ratchet.py
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/ratchet_lib.py
            ${CMAKE_BINARY_DIR}/compile_commands.json
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating the codebase reference (gen_code_docs.py)"
    VERBATIM)
add_custom_target(code_docs DEPENDS ${_code_docs_stamp})
add_dependencies(code_docs openpete_generated_headers)

# How each game's recompiled module reaches its executable
# (cmake/openpete_game.cmake):
#   static - the module's objects link into the executable; one image holds
#            one game's recompiled code, since the names collide across
#            games.
#   shared - <tag>-recomp.{so,dll} beside the executable, loaded at boot
#            (games/common/recomp_attach_shared.c); linux-release,
#            windows-mingw and the launcher presets.
set(OPENPETE_RECOMP_LINKAGE "static" CACHE STRING
    "Recomp module linkage: static or shared")
set_property(CACHE OPENPETE_RECOMP_LINKAGE PROPERTY STRINGS static shared)
# The resolved linkage, one word, at the tree root for tools that read a
# build tree's arm (tests/harness/bench-corpus.py). The cache entry keeps the
# declaration default where a preset file sets the knob as a normal variable,
# and a module file beside the executable survives a reconfigure to static,
# so neither answers the question. Rewritten on every configure; the file's
# timestamp moves only when the value changes.
file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/openpete-recomp-linkage.txt"
     CONTENT "${OPENPETE_RECOMP_LINKAGE}\n")
# Whether a shared-linkage executable builds a missing or unusable module
# from the disc (the first-boot pipeline of a game that has one). OFF: the
# module comes from the build tree alone and a missing one is refused, so a
# gate run never compiles a module from the disc. The distribution presets
# turn it on.
option(OPENPETE_RECOMP_FIRST_BOOT
       "Shared linkage: build a missing recomp module from the disc at boot" OFF)

# PSYQ abort-on-call stubs, compiled once: the union of every built game's
# generated stub names minus the names the shared thunk layer defines
# (tools/filter_psyq_stubs.py union), with the runtime that reports a reached
# stub. Every executable links the objects (openpete_add_game); each game's
# guest address for a stub is in its own table, reached through its runtime
# descriptor. The stubs stay exe-side: PE has no dynamic symbol preemption,
# so module-side stubs would shadow the thunks on Windows.
set(OPENPETE_PSYQ_THUNK_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/games/common/psyq/psyq_thunks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/games/common/psyq/psyq_game_hooks.c
    ${CMAKE_CURRENT_SOURCE_DIR}/games/common/psyq/psyq_sfx_slots.c
    ${CMAKE_CURRENT_SOURCE_DIR}/games/common/psyq/psyq_thunk_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/games/common/psyq/psyq_stub_runtime.c)
set(_psyq_stub_inputs "")
foreach(game IN LISTS OPENPETE_GAMES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/generated/psyq_stubs.c")
        list(APPEND _psyq_stub_inputs
             "${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/generated/psyq_stubs.c")
    endif()
endforeach()
if(_psyq_stub_inputs)
    set(_psyq_stubs_union ${CMAKE_CURRENT_BINARY_DIR}/psyq/psyq_stubs_union.c)
    add_custom_command(
        OUTPUT  ${_psyq_stubs_union}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/psyq
        COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/filter_psyq_stubs.py
                union ${_psyq_stubs_union}
                --stubs ${_psyq_stub_inputs}
                --thunks ${OPENPETE_PSYQ_THUNK_SOURCES}
        DEPENDS ${_psyq_stub_inputs} ${OPENPETE_PSYQ_THUNK_SOURCES}
                ${CMAKE_CURRENT_SOURCE_DIR}/tools/filter_psyq_stubs.py
        COMMENT "Generating the PSYQ stub union (filter_psyq_stubs.py)"
        VERBATIM)
    add_library(openpete_psyq_stubs OBJECT
        ${_psyq_stubs_union}
        games/common/psyq/psyq_stub_runtime.c)
    target_include_directories(openpete_psyq_stubs PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/games/common)
    target_link_libraries(openpete_psyq_stubs PRIVATE openpete_core)
    set_target_properties(openpete_psyq_stubs PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON)
endif()

# The host code every game links, compiled once (games/common/CMakeLists.txt),
# then each game's targets (cmake/openpete_game.cmake), then, with shared
# linkage on Linux and MinGW, the one executable linking them all.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/openpete_game.cmake)
add_subdirectory(games/common)
foreach(game IN LISTS OPENPETE_GAMES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/games/${game}/CMakeLists.txt")
        add_subdirectory(games/${game})
    else()
        message(WARNING "OPENPETE_GAMES requested '${game}' but games/${game}/CMakeLists.txt is missing - skipping")
    endif()
endforeach()
openpete_link_games()

# openpete_games: every configured game's target (executable, module, SDK,
# assets) and nothing else ALL carries. A capture-image build
# (scripts/golden-capture-container.sh) builds it in place of ALL: the image
# holds the toolchain a capture needs, not the fences' (libclang bindings,
# clang-tidy, an armed hook), and those run in the developer's linux-release
# tree.
add_custom_target(openpete_games)
get_property(_openpete_linked_tags GLOBAL PROPERTY OPENPETE_LINKED_GAMES)
foreach(_tag IN LISTS _openpete_linked_tags)
    add_dependencies(openpete_games openpete-${_tag})
endforeach()

# The Modding reference pages (the docs target, games/spyro-1) decide which
# games have a mod SDK from mod-sdk-test/sdk/games/<id>/, so every game's
# staging runs first.
if(TARGET docs)
    foreach(game IN LISTS OPENPETE_GAMES)
        string(REPLACE "-" "" _docs_tag "${game}")
        if(TARGET ${_docs_tag}_mod_sdk_test_stage)
            add_dependencies(docs ${_docs_tag}_mod_sdk_test_stage)
        endif()
    endforeach()
endif()

# surface_dump: each built game writes its OPENPETE_SURFACE_DUMP to
# <build>/surface/<game>.json. Not ALL and absent from a cross build, which
# cannot run the binaries. tools/surface_docs.py baseline turns a dump into
# the committed packaging/surface/<game>.json the docs read.
if(NOT CMAKE_CROSSCOMPILING)
    set(_surface_cmds "")
    set(_surface_deps "")
    foreach(game IN LISTS OPENPETE_GAMES)
        string(REPLACE "-" "" _surface_exe "${game}")
        set(_surface_exe "openpete-${_surface_exe}")
        if(TARGET ${_surface_exe})
            openpete_game_bin(${game} _surface_bin _surface_args)
            list(APPEND _surface_cmds
                 COMMAND ${CMAKE_COMMAND} -E env
                         OPENPETE_SURFACE_DUMP=${CMAKE_BINARY_DIR}/surface/${game}.json
                         ${_surface_bin} ${_surface_args})
            list(APPEND _surface_deps ${_surface_exe})
        endif()
    endforeach()
    if(_surface_deps)
        add_custom_target(surface_dump
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/surface
            ${_surface_cmds}
            COMMENT "Writing the surface dumps to ${CMAKE_BINARY_DIR}/surface/"
            VERBATIM)
        add_dependencies(surface_dump ${_surface_deps})
        # The docs coverage fence, per build: every release id of a shipped
        # game has a User docs entry. Warnings only, so a half-written row
        # in one session does not fail another session's build; ctest
        # surface_coverage and the release scripts refuse. Linux only, where
        # the built binaries run, and only where the debug block is compiled
        # (the check reads every row; the shipped presets hide the block).
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT OPENPETE_HIDE_DEBUG_FLAGS)
            add_custom_target(openpete_surface_coverage ALL
                COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/surface_coverage.py
                        check --warn --build-dir ${CMAKE_BINARY_DIR}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                COMMENT "docs coverage fence: every release id has a User docs entry"
                VERBATIM)
            add_dependencies(openpete_surface_coverage ${_surface_deps})
        endif()
    endif()
endif()

# openpete_label_directory_tests(<label>): append <label> to every test the
# calling directory defines, run at the end of that directory through
# cmake_language(DEFER), so a test added anywhere in the file is covered.
# The LABELS directory property is not used: CTest 3.22 does not apply it to
# a test that sets WORKING_DIRECTORY.
function(openpete_label_directory_tests label)
    get_property(_tests DIRECTORY PROPERTY TESTS)
    if(_tests)
        set_property(TEST ${_tests} APPEND PROPERTY LABELS ${label})
    endif()
endfunction()

if(OPENPETE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/CMakeLists.txt")
    add_subdirectory(tests/unit)
    add_subdirectory(tests/modsave)

    # The executables the game-launching tests below run (OPENPETE_BIN); each
    # harness passes --disc, which selects the game in the one executable.
    openpete_game_bin(spyro-1 _spyro1_bin _spyro1_args)
    openpete_game_bin(spyro-3 _spyro3_bin _spyro3_args)
    get_filename_component(_spyro1_dir "${_spyro1_bin}" DIRECTORY)

    # RESOURCE_LOCK openpete_game: every test below that launches a game
    # process holds it, so ctest never co-schedules two of them. A second
    # game process invalidates any timing measurement and is refused
    # outright by scripts/fences/bench_guard.py (tests/harness/bench-corpus.py,
    # tests/harness/check_pacing.sh), and a
    # run that shares the machine also overruns its TIMEOUT. The
    # presentation-labelled per-feature tests keep their parallelism:
    # they carry no timing threshold and run under `ctest -L presentation`.

    # Modding-framework PoC-ladder gate, one test per check_mods.sh
    # section (games: the per-game SDK, multigame, other-games, shader,
    # material and table-census rungs; matrix: the cross-mod matrix, asset
    # and wad overlays and the [test]-block runner; timeline: the arena
    # canary, rewind, stream handles, save safety and session rungs). No
    # section reads another's output. Each TIMEOUT is twice the section's
    # measured wall time on an idle machine (games 283 s, matrix 481 s,
    # timeline 342 s). `ctest -L mods`, or tests/harness/check_mods.sh
    # with no --section for the whole ladder.
    foreach(_sec_to games:570 matrix:970 timeline:690)
        string(REPLACE ":" ";" _sec_to "${_sec_to}")
        list(GET _sec_to 0 _sec)
        list(GET _sec_to 1 _to)
        add_test(NAME mod_ladder_${_sec}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mods.sh
                         --section ${_sec})
        set_tests_properties(mod_ladder_${_sec} PROPERTIES
            LABELS "slow;mods"
            RESOURCE_LOCK openpete_game
            TIMEOUT ${_to}
            ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The debug-flags row check alone (check_mods.sh --section debugflags),
    # one test per game with rows: Spyro 1's table holds none, so
    # mod_ladder_games cannot reach them. Each run measures 4 s on an idle
    # machine; TIMEOUT 30 leaves room for the mod compile under load.
    openpete_game_bin(spyro-2 _spyro2_bin _spyro2_args)
    foreach(_g_to spyro-2:30 spyro-3:30)
        string(REPLACE ":" ";" _g_to "${_g_to}")
        list(GET _g_to 0 _g)
        list(GET _g_to 1 _to)
        string(REPLACE "spyro-" "_spyro" _gvar "${_g}")
        add_test(NAME mod_ladder_debugflags_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mods.sh
                         --game ${_g} --section debugflags)
        set_tests_properties(mod_ladder_debugflags_${_g} PROPERTIES
            LABELS "slow;mods"
            RESOURCE_LOCK openpete_game
            TIMEOUT ${_to}
            ENVIRONMENT "OPENPETE_BIN=${${_gvar}_bin}"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Per-mod save-record harness (Spyro 1): the save and load recordings
    # under the pad-format fixtures, chained load legs, checksum repair,
    # fixture-driven saves, and the api legs, which report SKIP until the
    # engine has the save-record api (tests/harness/check_modsave.sh).
    # TIMEOUT is twice the measured wall time (360 s; calibrate 79 s).
    add_test(NAME modsave_harness
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_modsave.sh)
    set_tests_properties(modsave_harness PROPERTIES
        LABELS "slow;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 720
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # Its calibration (LAW §13): a record byte flipped outside the pad and a
    # skipped stock update must each fail the check that guards it.
    add_test(NAME modsave_harness_calibrate
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_modsave.sh --calibrate)
    set_tests_properties(modsave_harness_calibrate PROPERTIES
        LABELS "slow;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 160
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # ImGui containment gate: the mods fixture proves the table binds
    # headless, but every guard it installs runs only inside a UI section
    # body, and a body needs an open overlay - so this leg opens one in a
    # real window and reads engagement, the error bracket and the
    # out-of-body refusal off the log. Skips (64) with no display.
    add_test(NAME mod_imgui
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mod_imgui.sh)
    set_tests_properties(mod_imgui PROPERTIES
        LABELS "mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Per-channel material gate: turns every mods/channel-gallery row into
    # an assertion (a row that should paint must, a KNOWN GAP row must
    # not), plus refine-vs-declarative equivalence - including the
    # many-distinct-outcome case, which is the shape that caught
    # per-instance slots being stamped after their intern table was reset.
    # Neither check_mods nor native_visual_regression runs a mod material
    # at all, so this is the only gate that exercises the selector seam.
    # Slow (a dozen headless sessions); `ctest -L mods` or the script.
    add_test(NAME mod_channel_selectors
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mod_channels.sh)
    set_tests_properties(mod_channel_selectors PROPERTIES
        LABELS "slow;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The same gate on Spyro 2 and 3: one declarative row per render channel
    # repaints pixels against a disabled-row baseline at a corpus pin, with
    # a repeat-baseline control.
    foreach(_chg spyro-2 spyro-3)
        string(REPLACE "-" "" _chtag "${_chg}")
        add_test(NAME mod_channel_selectors_${_chtag}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_mod_channels.sh
                         --game ${_chg})
        set_tests_properties(mod_channel_selectors_${_chtag} PROPERTIES
            LABELS "slow;mods"
            RESOURCE_LOCK openpete_game
            TIMEOUT 3600
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # A Windows mod may include the SDK headers and <windows.h>, whose
    # windef.h defines RECT. The SDK's PSYQ types carry a PSX_ prefix to
    # keep clear of it; this cross-compiles one such translation unit and
    # fails on a redefinition. Skips (64) with no mingw cross compiler.
    add_test(NAME sdk_windows_include
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_sdk_windows_include.sh
                     --sdk ${_spyro1_dir}/sdk)
    set_tests_properties(sdk_windows_include PROPERTIES
        LABELS "mods"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Freecam-corpus gate for the terrain near-LP gapfill classifier -
    # replays recorded problematic freecam spots (holes that must stay
    # filled, walls/arches the fill must not touch) and asserts metric
    # expectations with the feature on vs off. Slow (boots ~7 headless
    # sessions); `ctest -L gapfill` or the script directly.
    add_test(NAME gapfill_corpus
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_gapfill_corpus.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(gapfill_corpus PROPERTIES
        LABELS "slow;gapfill"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Terrain env-anim motion census at extended LOD: four Spyro 2 and
    # Spyro 3 freecam poses, two game processes at a time (~90 s). Every
    # listed sector the native pass draws must animate, and the walker model
    # must equal guest RAM on every sector the game animates. Slow;
    # `ctest -L terrain_anim` or the script directly. Exit 64 = no binary,
    # disc or Vulkan loader.
    add_test(NAME terrain_anim_census
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_anim_census.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_anim_census PROPERTIES
        LABELS "slow;presentation;terrain_anim"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Spyro 1 moby slot-reuse census: three rows (two bk2 windows and the
    # attract demo) where a moby slot is freed and reallocated within one
    # tick; no sub-tick present may lerp the dead moby into the newborn, and
    # each row must see a reuse (engagement). Slow; `ctest -L moby_serial`
    # or the script directly. Exit 64 = no binary or disc.
    add_test(NAME moby_serial_census
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_serial_census.sh)
    set_tests_properties(moby_serial_census PROPERTIES
        LABELS "slow;presentation;moby_serial"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1500
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Native moby face-decode cache: on Spyro 1 artisans, Spyro 2 glimmer and
    # Spyro 3 sunrise_spring, OPENPETE_FCACHE_CHECK=1 rewrites each faceblock's
    # last Tiledef in guest RAM mid-level and the cache must serve a decode of
    # the rewritten bytes, then of the restored ones; each row must check at
    # least one faceblock (engagement). `ctest -L fcache` or the script
    # directly. Exit 64 = no binary or disc.
    add_test(NAME fcache_rewrite
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_fcache_rewrite.sh)
    set_tests_properties(fcache_rewrite PROPERTIES
        LABELS "slow;presentation;fcache"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Hor+ presentation symmetry - boots the headless demo at 4:3, 16:9
    # and 21:9, takes a screenshot at the same frame, and pixel-diffs the
    # central 4:3 region. Asserts that widening the GTE projection does
    # not perturb the canonical 4:3 view. Currently RED: cyclorama
    # stretching + sub-pixel projection drift leak into the central
    # region. Closing the leak makes this gate green.
    add_test(NAME aspect_screenshot_symmetry
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_aspect_screenshot.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frame=850)
    set_tests_properties(aspect_screenshot_symmetry PROPERTIES
        LABELS "slow;determinism"
        RESOURCE_LOCK openpete_game
        TIMEOUT 600
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Headless capture lattice: four Spyro 1 captures under
    # --raster-quantize 1 at 4:3 and 16:9, each at the capture shape and at
    # a --window of another shape; the duplicate column and row counts of
    # the pixelation lattice must not change with --window (128 columns and
    # 224 rows at 4:3); four no-postfx captures at the same shapes, each
    # mismatched one pixel-identical to its matching one, plus a no-shadow
    # control leg.
    # Exit 64 = no binary or disc.
    add_test(NAME capture_lattice
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_capture_lattice.sh)
    set_tests_properties(capture_lattice PROPERTIES
        LABELS "slow;presentation"
        RESOURCE_LOCK openpete_game
        SKIP_RETURN_CODE 64
        TIMEOUT 600
        ENVIRONMENT "OPENPETE_BIN=${CMAKE_BINARY_DIR}/openpete"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Host disc-read trace: two Spyro 1 boots to VBL 600 (disc cache on and
    # off) under OPENPETE_DISC_TRACE; every row parses, the exe, libcd and
    # woah paths appear, the woah NSF decodes, and the libcd sector sum
    # equals the CdRead thunk's.
    # Exit 64 = no binary or disc.
    add_test(NAME disc_trace
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_disc_trace.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(disc_trace PROPERTIES
        LABELS "disc"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # SIGTERM in each phase of a headless Spyro 1 run: under
    # OPENPETE_FAST_BOOT_HOLD (quit from the fast-boot VBL path), after fast
    # boot, the second-signal escape (rc 130) and the quit backstop (panic
    # within 35 s) over OPENPETE_TEARDOWN_STALL_MS, and --max-runtime-seconds
    # under the hold. About 55 s. Exit 64 = no binary or disc.
    add_test(NAME sigterm_exit
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_sigterm_exit.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(sigterm_exit PROPERTIES
        LABELS "slow;quit"
        RESOURCE_LOCK openpete_game
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Vulkan device creation failing before vkCreateDevice: a missing ICD
    # under --headless capture, and the offscreen video driver's missing
    # VK_EXT_headless_surface when windowed. Each run tears the partial
    # device down without a fault and continues on its fallback (rc 0).
    # About 10 s. Exit 64 = no binary or disc.
    add_test(NAME vk_create_failure
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_vk_create_failure.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(vk_create_failure PROPERTIES
        LABELS "startup"
        RESOURCE_LOCK openpete_game
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Spyro 2/3 cutscene speech ring: per row, run to the speech voice's
    # key-off; every ring refill lands behind the guest voice head (0
    # overwrite, 0 stale samples), the OpenAL offset takes no slip step,
    # and each chunk's body correlates >= 0.95 with its payload.
    # Exit 64 = no binary or disc for any row.
    add_test(NAME cutscene_audio
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_cutscene_audio.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(cutscene_audio PROPERTIES
        LABELS "slow;audio"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Level warp from guest states other than gameplay: per row of
    # games/<game>/corpus/levelwarp.manifest, a headless run that pauses
    # (or reaches a loading screen), fires OPENPETE_LEVELWARP_TEST there and
    # unpauses; Spyro 2 and 3 refuse with the demo word 0 and return to
    # gameplay, Spyro 1 arrives. Fails at zero engaged rows. Exit 64 = no
    # binary or disc for any game.
    add_test(NAME levelwarp
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_levelwarp.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(levelwarp PROPERTIES
        LABELS "slow;levelwarp"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Framebuffer window resolve failure: Spyro 3's pause Options window
    # copied in Seashell Shore, then in Molten Crater with the second
    # resolve's compose failed by OPENPETE_FBSNAP_RESOLVE_FAIL=2; the
    # capture of the second Options page must equal the clean leg's byte
    # for byte (no earlier copy's image in the window). VACUOUS (exit 3)
    # without two copies, the injection, or a drawn window. Exit 64 = no
    # Spyro 3 binary or disc.
    add_test(NAME fbsnap_resolve
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_fbsnap_resolve.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(fbsnap_resolve PROPERTIES
        LABELS "slow;fbsnap"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Level warp into every level of each game's table from gameplay in a
    # start level, Cross tapped 2 VBLs in 40 to dismiss arrival speeches;
    # each row must log the arrival and reach gameplay (state word 0) with
    # the level word equal to the target. Fails on a dropped warp, a
    # VACUOUS row, or zero engaged rows. Exit 64 = no binary or disc for
    # any game.
    add_test(NAME levelwarp_all
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_levelwarp_all.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(levelwarp_all PROPERTIES
        LABELS "slow;levelwarp"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Spyro 3 Area warp pairs (games/spyro-3/corpus/areawarp.manifest,
    # default set) with Cross tapped 2 VBLs in 40: each engaged row must
    # arrive in the target sub-area with no black judged screenshot after
    # the warp starts. Fails on a failing or VACUOUS row or zero engaged
    # rows. Exit 64 = no Spyro 3 binary or disc.
    add_test(NAME s3_areawarp
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_s3_areawarp.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(s3_areawarp PROPERTIES
        LABELS "slow;levelwarp"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Audio feed across live tick-scale changes: per game and producer-model
    # seed (1-3), a headless run on the feed's synthetic device on the
    # virtual clock under a 0.25-3.0 schedule of
    # openpete_set_tick_scale calls; 0 underruns and 0 overflows, the
    # queued depth within 45 ms of its target per scale, and back within
    # the band <= 1 s after each change. Exit 64 = no binary or disc.
    add_test(NAME audio_tick_scale
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_audio_tick_scale.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(audio_tick_scale PROPERTIES
        LABELS "slow;audio"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Audio feed latency after a slow producer: audio_feed_sim drives the
    # feed's synthetic device with no game process through a producer-speed
    # span (0.8 for 120 s; 0.96 for 60 s), then 1.0; the queued latency
    # from 2 s after the span stays at or below target + deadband + one
    # catch-up burst. The driver refuses at zero span VBLs or samples.
    # Exit 64 = driver not built.
    add_test(NAME audio_feed_latency
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_audio_feed_latency.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(audio_feed_latency PROPERTIES
        LABELS "audio"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Host sim slips reach the audio feed's consumer: a Spyro 1 headless
    # run on the feed's synthetic device per injected stall (one 500 ms
    # stall; a 0.8x host for 150 s), virtual-clock advances through
    # OPENPETE_PACE_INJECT_US; the pacer's sim slip periods are nonzero and
    # the feed timeline's slips= equals them. Exit 64 = no binary or disc.
    add_test(NAME audio_feed_slip
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_audio_feed_slip.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(audio_feed_slip PROPERTIES
        LABELS "slow;audio"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Audio feed stretch on host stalls: Spyro 1 headless on the feed's
    # synthetic device, stalls injected through OPENPETE_PACE_INJECT_US per
    # producer-model seed (1-3); one stall the host catches up (55, 70,
    # 80 ms) engages no stretch, causes no underrun and keeps the content
    # rate within 0.5%; a 100 ms slip, two stalls 100 ms apart and 2 s at
    # 0.8x and 0.96x engage it; the single-lag rule (A/B) engages on every
    # leg. Exit 64 = no binary or disc.
    add_test(NAME audio_feed_hitch
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_audio_feed_hitch.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(audio_feed_hitch PROPERTIES
        LABELS "slow;audio"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # CD drive head: the CdlGetlocP head model replayed against every
    # reference answer of the Spyro 2/3 20k goldens (|diff| <= 2 LBA), and
    # the XA decoder head's rate on a Spyro 2 boot run (37609/15000 LBA per
    # VBL +- 0.0005). Exit 64 = no golden, binary or disc for any row.
    add_test(NAME cd_head
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_cd_head.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(cd_head PROPERTIES
        LABELS "slow;audio;cd"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # VBL delivery hold on the schedule: two Spyro 2 full-20k oracle runs
    # under OPENPETE_VBL_HOLD=charge:6000:4 (held and dry); the dry leg
    # passes the oracle, the hold leg diverges in no region and fails only
    # on a vbl-delta of D VBLs, holds 4 and flushes once with D = 3, both
    # in-binary window checks pass, the drive head matches across legs, and
    # the device-row leaf check ran with no guest write. held=0 is VACUOUS
    # and fails (exit 2). The cadence row reaches no corpus input and is
    # not run here. Exit 64 = no golden, binary or disc.
    add_test(NAME vbl_hold
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_vbl_hold.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=hold-on)
    set_tests_properties(vbl_hold PROPERTIES
        LABELS "slow;oracle;cd"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Disc boot-order profiles: per game, the committed profile equals the
    # generator's output over the committed boot trace, and the loader
    # simulation (tools/disc_boot_order.py) finds the lowest modelled
    # bandwidth at which file order misses no game read and asserts, one
    # step below it, that profile order misses none while file order does.
    # Exit 3 (reported as skipped) = VACUOUS: file order misses nothing even
    # at the lowest bandwidth. The selftest covers the profile parser and
    # the generator's determinism.
    foreach(_dbo_game spyro-1 spyro-2 spyro-3)
        add_test(NAME disc_boot_order_${_dbo_game}
                 COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/disc_boot_order.py
                         check --game ${_dbo_game}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(disc_boot_order_${_dbo_game} PROPERTIES
            LABELS "disc"
            SKIP_RETURN_CODE 3)
    endforeach()
    add_test(NAME disc_boot_order_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/test_disc_boot_order.py)
    set_tests_properties(disc_boot_order_selftest PROPERTIES LABELS "disc")

    # Bug-report bundle reader: info, extract, refusals of malformed
    # bundles, diff-hashes and the replay route on synthetic bundles.
    add_test(NAME bugreport_reader_selftest
             COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/test_bugreport.py)

    # Licence check of the packages (scripts/dist_compliance.py licences):
    # its refusals on synthetic trees, then the repository's own NOTICE.md
    # and manifest staged into a scratch directory.
    add_test(NAME dist_compliance_selftest
             COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/test_dist_compliance.py)
    set_tests_properties(dist_compliance_selftest PROPERTIES LABELS "dist")
    add_test(NAME dist_compliance_licences
             COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/scripts/dist_compliance.py
                     licences --stage ${CMAKE_CURRENT_BINARY_DIR}/dist-compliance-stage
                     --scope all --scope windows --scope sdk --scope appimage
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(dist_compliance_licences PROPERTIES LABELS "dist")

    # Profile summary's sim-tick split on synthetic .opp traces: the region
    # records (guest, host by subsystem, pool waits, the leaves summing to
    # sim_tick) and a trace written before them (the inclusive hook table).
    add_test(NAME profile_summary_selftest
             COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/test_profile_summary.py)

    # Bug-report replay's bundle unpack and scratch reset on synthetic
    # bundles: the member, total and entry-count bounds, duplicate names,
    # links in a directory bundle, a member under the replay's own
    # sandbox/, and a link planted in the scratch directory. No disc and no
    # game run: every case is refused while the command line is prepared.
    # Exit 64 = no binary.
    add_test(NAME bugreport_replay_unpack
             COMMAND python3 -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/test_bugreport_replay_unpack.py
                     --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(bugreport_replay_unpack PROPERTIES
        SKIP_RETURN_CODE 64
        TIMEOUT 120)

    # Bug-report replay on the engine: bundles recorded headless on Spyro 2
    # (one under --skip-to-level, one without) replay IDENTICAL; the launch
    # flags are dropped from a later anchor (negative control); an
    # instrument named in OPENPETE_BUGREPORT_REPLAY_ENV runs under the
    # replay and a gameplay variable there is refused. Exit 64 = no binary
    # or disc.
    add_test(NAME bugreport_replay
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_bugreport_replay.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(bugreport_replay PROPERTIES
        LABELS "disc"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Bug-report warp replay: a dev-overlay level warp pressed inside a
    # Spyro 2 bundle's window and a custom-level warp inside a Spyro 1
    # bundle's window are queued again from warps.txt at their drain ticks
    # and replay IDENTICAL; each bundle without the member diverges at the
    # warp (negative control). Exit 64 = no binary or disc.
    add_test(NAME bugreport_warp
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_bugreport_warp.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(bugreport_warp PROPERTIES
        LABELS "disc"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Bug-report custom-level replay (Spyro 1): a slot/replace toggle inside
    # the bundle's window is taken again from warps.txt at its tick, on a
    # first-run card the replay provisions in its scratch directory; an
    # anchor taken with the shared slot claimed restores the claim from the
    # state's registry section. Negative controls: the bundle without its
    # custom_mode lines, the anchor without its registry section. Exit 64 =
    # no binary or disc.
    add_test(NAME bugreport_custom_mode
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_bugreport_custom_mode.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(bugreport_custom_mode PROPERTIES
        LABELS "disc"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Sfx pack census: --dump-sfx-pack over the Spyro 1 disc through the
    # disc reader; the counters, the WAV set's name digest and the trace
    # rows are pinned to the retail disc. Exit 64 = no binary or disc.
    add_test(NAME sfx_pack
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_sfx_pack.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(sfx_pack PROPERTIES
        LABELS "disc"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Terrain-arena allocation legs: an arena's memory type refused
    # (OPENPETE_RHI_FAIL_MEMTYPE) reports each failed attempt and ends in the
    # cache's init-failure path with the run continuing on the CPU terrain
    # walk; the uninjected leg names the type it allocated on. Two headless
    # llvmpipe boots. Exit 64 = no binary or disc.
    add_test(NAME tercache_alloc
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tercache_alloc.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(tercache_alloc PROPERTIES
        LABELS "slow;renderer"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Terrain-cache level edges, one test per game: the attract demo (20,000
    # VBLs, several level loads on each game) with the cache forced on and
    # every arena sized from the level's content estimate reads
    # claims_sum=0 with bakes>0, failed_edges=0, and one fulfilled
    # replacement per level latch (the level edges plus the session's
    # first latch); the first-touch bake budget defers sectors at the edges
    # (budget_claims>0), bakes at least one per tick (budget_starved=0) and
    # closes every deferring run before exit (budget_open=0). Exit 64 = no
    # binary or disc.
    foreach(_g spyro-1 spyro-2 spyro-3)
        add_test(NAME tercache_level_edges_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tercache_growth.sh
                         --game ${_g} --leg level-edges --demo-limit 20000
                         --exe ${_spyro1_bin}
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        set_tests_properties(tercache_level_edges_${_g} PROPERTIES
            LABELS "slow;renderer"
            RESOURCE_LOCK openpete_game
            TIMEOUT 3600
            SKIP_RETURN_CODE 64)
    endforeach()

    # Headless native captures repeat across host load: one Spyro 3 capture
    # twice, the second pinned to CPU 0, with the terrain cache forced on;
    # every PNG digest, the native-feed triangle total and the cache's bakes
    # and budget_claims agree, and both legs report bakes>0 and
    # budget_claims>0 (the first-touch budget deferred sectors). A
    # presentation decision keyed on wall time separates the legs. Exit 64 =
    # no binary or disc.
    add_test(NAME capture_repeat_spyro-3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_capture_repeat.sh
                     --game spyro-3 --exe ${_spyro1_bin}
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(capture_repeat_spyro-3 PROPERTIES
        LABELS "slow;renderer;determinism"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64)

    # Cognitive-complexity ratchet, the ctest spelling of the per-build
    # check (core/CMakeLists.txt, openpete_complexity_ratchet). Exit 64 =
    # no compile database.
    add_test(NAME complexity_ratchet
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/complexity_ratchet.py
                     check --ctest
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(complexity_ratchet PROPERTIES
        LABELS "fence"
        ENVIRONMENT "OPENPETE_COMPLEXITY_DB=${CMAKE_BINARY_DIR}/compile_commands.json"
        SKIP_RETURN_CODE 64)
    # The fence's own tests: the decision function per rule and the hook end
    # to end in a scratch repo.
    add_test(NAME complexity_ratchet_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_complexity_ratchet.py)
    set_tests_properties(complexity_ratchet_selftest PROPERTIES LABELS "fence")
    # Function-documentation ratchet, the ctest spelling of the per-build
    # check (core/CMakeLists.txt, openpete_doc_ratchet). Exit 64 = no
    # compile database.
    add_test(NAME doc_ratchet
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/doc_ratchet.py
                     check --ctest
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(doc_ratchet PROPERTIES
        LABELS "fence"
        ENVIRONMENT "OPENPETE_DOCS_DB=${CMAKE_BINARY_DIR}/compile_commands.json"
        SKIP_RETURN_CODE 64)
    # The documentation fence's own tests: the decision function per rule,
    # the touched rule, the per-entry merge, the fixture controls (and a
    # tampered copy that must fail), and the hook end to end in a scratch
    # repository, including a cherry-pick across a landing.
    add_test(NAME doc_ratchet_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_doc_ratchet.py)
    set_tests_properties(doc_ratchet_selftest PROPERTIES LABELS "fence")
    # Thread-role fence, the ctest spelling of the per-build check
    # (core/CMakeLists.txt, openpete_thread_roles). Exit 64 = no compile
    # database.
    add_test(NAME thread_roles
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/thread_roles.py
                     check --ctest
             WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(thread_roles PROPERTIES
        LABELS "fence"
        ENVIRONMENT "OPENPETE_THREAD_ROLES_DB=${CMAKE_BINARY_DIR}/compile_commands.json"
        SKIP_RETURN_CODE 64)
    # The per-worker cache-line fence's own tests.
    add_test(NAME worker_line_guard_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_worker_line_guard.py)
    set_tests_properties(worker_line_guard_selftest PROPERTIES LABELS "fence")
    # The provenance fence's own tests: the token and form rules, the
    # allow-list and marker, and planted-token controls in a scratch repo.
    add_test(NAME provenance_census_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_provenance_census.py)
    set_tests_properties(provenance_census_selftest PROPERTIES LABELS "fence")
    # The upstream-text fence's own tests: a line planted from a scratch
    # upstream file fails tree and hook mode; the clean series passes.
    add_test(NAME upstream_text_census_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_upstream_text_census.py)
    set_tests_properties(upstream_text_census_selftest PROPERTIES LABELS "fence")
    # The thread-role fence's own tests: the text scans and rewrites.
    add_test(NAME thread_roles_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_thread_roles.py)
    set_tests_properties(thread_roles_selftest PROPERTIES LABELS "fence")
    # Warning ratchet: the command rewrite, the GCC text parser, row
    # attribution, the decision and one scratch compile.
    add_test(NAME warning_ratchet_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_warning_ratchet.py)
    set_tests_properties(warning_ratchet_selftest PROPERTIES LABELS "fence")
    # The baselines' merge driver: the three-way merge by row key, and a
    # cherry-pick across a landing in a scratch repository.
    add_test(NAME ratchet_tsv_merge_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_ratchet_tsv_merge.py)
    set_tests_properties(ratchet_tsv_merge_selftest PROPERTIES LABELS "fence")
    # The golden-image fence: the path rule and the pre-commit dispatcher end
    # to end in a
    # scratch repository (staged image shapes, the bypass, and a complexity
    # refusal through the dispatcher).
    add_test(NAME golden_png_guard_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_golden_png_guard.py)
    set_tests_properties(golden_png_guard_selftest PROPERTIES LABELS "fence")
    # The WSLENV fence's own tests: the per-launch rule on Python sources
    # (refused and passing shapes), the shell file-wide clearance, and the
    # checkout clean.
    add_test(NAME wslenv_guard_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_wslenv_guard.py)
    set_tests_properties(wslenv_guard_selftest PROPERTIES LABELS "fence")
    # The timing window and the benchmark preflight: one winner between two
    # contenders, stale-lock recovery after SIGKILL, a hand-written marker
    # holding the window, the shell gate, and every unreadable precondition
    # reported UNCHECKED and refused. Private bench directory; no game runs.
    add_test(NAME timing_lock_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bench/tests/test_timing_lock.py)
    add_test(NAME bench_preflight_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/bench/tests/test_preflight.py)
    set_tests_properties(timing_lock_selftest bench_preflight_selftest PROPERTIES LABELS "fence")
    # The build-directory lock: a gate waits for a running build, a build
    # waits for a running gate (and runs beside it under the bypass), gates
    # share, a build under its own gate refuses, and a gate the build runs
    # proceeds. Scratch build directory and a stand-in driver; nothing builds.
    add_test(NAME build_lock_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/lib/tests/test_build_lock.py)
    set_tests_properties(build_lock_selftest PROPERTIES LABELS "fence")
    # The sandbox fence's own tests: a launch without the sandbox library or
    # OPENPETE_SANDBOX refused in shell and Python, the providers, helpers and
    # markers passing, the provider closure, and the checkout clean.
    add_test(NAME sandbox_guard_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_sandbox_guard.py)
    set_tests_properties(sandbox_guard_selftest PROPERTIES LABELS "fence")
    # The knob read-path fence: no registered OPENPETE_* variable is read
    # both by getenv (or a wrapper of it) and by openpete_config_env (or a
    # wrapper of it), so a config file [env] value has one source of truth.
    # The selftest feeds split and clean fixtures and runs the checkout.
    add_test(NAME knob_read_path
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/knob_read_path.py
                     --root ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(knob_read_path PROPERTIES LABELS "fence")
    add_test(NAME knob_read_path_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_knob_read_path.py)
    set_tests_properties(knob_read_path_selftest PROPERTIES LABELS "fence")
    # The recomp identity pair of the blessed manifests: the psx.map symbol
    # digest is invariant under the build path, address width and section
    # order; the generated-tree digest ignores stale files and refuses a
    # missing output; rebless-identity refuses outside the capture image and
    # on a dirty recomp input, else rewrites only the two columns.
    add_test(NAME presentation_identity_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_presentation_identity.py)
    set_tests_properties(presentation_identity_selftest PROPERTIES LABELS "fence")
    # The tile census (scripts/golden-census.py): the selftest seeds the
    # census store's finalize, prune, ancestry and refusals, the query
    # file's fixing and scoring and the accessor fence's controls into
    # scratch copies.
    add_test(NAME tile_census_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_golden_census.py)
    set_tests_properties(tile_census_selftest PROPERTIES LABELS "presentation;fence")
    # The docs-image pair check: an identical before/after pair, a missing
    # tile and a malformed docs: tag fail; distinct pairs pass.
    add_test(NAME docs_shot_pairs_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_docs_shot_pairs.py)
    set_tests_properties(docs_shot_pairs_selftest PROPERTIES LABELS "fence")
    # The package docs image fence (scripts/fences/docs_image_allowlist.py,
    # run by scripts/package-dist.sh on every package's docs/): an image not
    # on packaging/docs-images.txt, by extension or by signature, and a
    # listed image absent both refuse. The second test runs the check on the
    # docs target's site; exit 64 when the site was not built.
    add_test(NAME docs_image_allowlist_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_docs_image_allowlist.py)
    set_tests_properties(docs_image_allowlist_selftest PROPERTIES LABELS "fence")
    add_test(NAME docs_image_allowlist
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/docs_image_allowlist.py
                     check ${CMAKE_BINARY_DIR}/games/spyro-1/docs-site)
    set_tests_properties(docs_image_allowlist PROPERTIES
        LABELS "fence"
        SKIP_RETURN_CODE 64)

    # The image-side gates below (native_visual_regression, the presentation
    # golden rows and the dragon tier) are defined over the pinned capture
    # image (tests/harness/linux-capture.Dockerfile). Run from a host build
    # tree, each re-enters itself through scripts/golden-capture-container.sh, which
    # rebuilds this source tree with the image's toolchain into
    # build/linux-release-container, overriding the --build-dir passed here,
    # and captures with that binary; a machine without podman/docker or the
    # image reports SKIP (64). The rows share that one build directory, so they
    # carry RUN_SERIAL (the wrapper's lock on it also covers a manual run
    # beside ctest), and each timeout includes the in-image build (~4 min
    # from fresh at nproc 16; priced at 10 min).

    # Native renderer visual-regression gate. Renders the native `-vk`
    # frame headless through the software rasterizer (surfaceless Vulkan, no
    # GPU, no display) at seven attract-demo draw-ticks and compares each
    # tile's verdict digest against its row of
    # games/spyro-1/native-tick-corpus.manifest; the capture is
    # byte-deterministic, so the compare is exact. See
    # tests/golden/spyro-1/native-vk/README.md.
    add_test(NAME native_visual_regression
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_native_visual.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(native_visual_regression PROPERTIES
        LABELS "slow;visual"
        RESOURCE_LOCK openpete_game
        RUN_SERIAL TRUE
        # Headroom over the game's own --max-runtime-seconds 600 so the
        # in-loop self-limit always fires first (equal timeouts raced, and a
        # hung game loop can't fire its own limit), plus the in-image build.
        TIMEOUT 1500
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Presentation-golden image gate (the image-side counterpart to the psx
    # oracle). Captures the native -vk corpus (canonical tick-keyed +
    # deterministic alpha=0.5 intermediate) headless under llvmpipe and compares
    # each tile's verdict digest against its row of the committed manifest
    # tests/golden/<game>/presentation/blessed.tsv; no reference image is read
    # on a pass. A clean tree is pixel-identical, so any divergence is a
    # presentation regression the gameplay oracle can't see. Covers the
    # fanned-out corpus: the skip-to-level bk2s (capped to a few evenly-spaced
    # samples each, 16:9, both LOD modes) plus the debug-freecam poses. Re-bless
    # an intended change with tests/harness/check_presentation_golden.sh --bless
    # or a selective promote; scope a fast subset with --filter=<bk2-substr>.
    # Skips (exit 64) if the capture image, binary, disc, Vulkan loader or PIL
    # are absent. The spyro-1 row also runs the credits-roll interpolation leg
    # (tests/harness/check_credits_interp.sh) so it cannot be skipped
    # separately. The gate takes --game SLUG and derives binary, disc, bk2 dir,
    # manifests and golden from it.
    add_test(NAME presentation_golden
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_presentation_golden.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(presentation_golden PROPERTIES
        LABELS "slow;presentation"
        RESOURCE_LOCK openpete_game
        RUN_SERIAL TRUE
        # The whole corpus replays ~53 bk2 movies + the freecam poses (~35 min),
        # plus the in-image build; headroom so a loaded host doesn't spuriously
        # time out.
        TIMEOUT 4200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # Spyro 2 and Spyro 3: the same gate, scoped by --game, over each game's
    # games/<game>/corpus manifests (~10 min each plus the in-image build).
    foreach(_pg_game spyro2 spyro3)
        string(REPLACE "spyro" "spyro-" _pg_slug ${_pg_game})
        add_test(NAME presentation_golden_${_pg_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_presentation_golden.sh
                         --game ${_pg_slug} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(presentation_golden_${_pg_game} PROPERTIES
            LABELS "slow;presentation"
            RESOURCE_LOCK openpete_game
            RUN_SERIAL TRUE
            TIMEOUT 2400
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    # Spyro 1 dragon-rescue tier (tests/harness/check_dragon_golden.sh): one
    # pose row per dragon rescue, sampled every 60 VBLs through the cutscene,
    # stock LOD, golden tests/golden/spyro-1/dragons. Labelled `dragons`, not
    # `presentation`, so `ctest -L presentation` and the default gate chains
    # skip it; the harness header states when it runs.
    add_test(NAME dragon_golden_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_dragon_golden.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(dragon_golden_spyro-1 PROPERTIES
        LABELS "slow;dragons"
        RESOURCE_LOCK openpete_game
        RUN_SERIAL TRUE
        TIMEOUT 4200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Skip-target rows: every --skip-to-cutscene (and --skip-to-area) row of
    # games/<slug>/corpus/skip-targets.manifest runs headless; a row passes only when
    # the skip's release line carries the row's engagement values (the cutscene
    # index, the credits sequence, the return level, the level and area words),
    # and a negative control with the fail-safe forced must fail. One row per
    # game with a manifest.
    foreach(_skip_game spyro-1 spyro-2 spyro-3)
        add_test(NAME skip_targets_${_skip_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_skip_targets.sh
                         --game ${_skip_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(skip_targets_${_skip_game} PROPERTIES
            LABELS "slow;skip-targets"
            RESOURCE_LOCK openpete_game
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Host-loop decision trace: every headless row of
    # games/<slug>/corpus/loop-trace.manifest (the attract-demo boot, the bk2
    # rows under runahead, state stress, stress rewind and rewind hold, the
    # handoff exec, the setup-error closes) runs with OPENPETE_LOOP_TRACE and
    # its trace digest must equal tests/golden/<slug>/loop-trace-baseline.tsv.
    # A row that misses an engagement term of its manifest row is VACUOUS
    # (fail). The windowed rows need Xvfb and run by hand (--windowed).
    foreach(_lt_game spyro-1 spyro-2 spyro-3)
        add_test(NAME loop_trace_${_lt_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_loop_trace.sh
                         --game ${_lt_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(loop_trace_${_lt_game} PROPERTIES
            LABELS "slow;determinism;loop-trace"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Option-parse exit codes: --help exits 0, a typed non-finite --fov exits
    # 3, the same value from openpete.toml warns and keeps the default, and
    # fov = 0 parses as the game's projection. Parse-only legs plus one
    # 120-VBL headless boot under the bad config file.
    foreach(_cli_game spyro-1 spyro-2 spyro-3)
        add_test(NAME config_cli_${_cli_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_config_cli.sh
                         --game ${_cli_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(config_cli_${_cli_game} PROPERTIES
            LABELS "tooling"
            RESOURCE_LOCK openpete_game
            TIMEOUT 300
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The surface dump (OPENPETE_SURFACE_DUMP): each game writes its flags,
    # registry rows, menu rows, key actions and vocabularies with their
    # declared defaults, and the selfcheck parses every flag at its declared
    # default against the default parse. A broken default (the negative
    # control) must fail it. Parse-only; no disc.
    foreach(_surface_game spyro-1 spyro-2 spyro-3)
        add_test(NAME surface_dump_${_surface_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_surface_dump.sh
                         --game ${_surface_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(surface_dump_${_surface_game} PROPERTIES
            LABELS "tooling"
            TIMEOUT 120
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The tooltip vocabulary (OPENPETE_TOOLTIP_VOCAB): every registry row's
    # tooltip and --help description, the game's rows, the help-only flags,
    # the settings-menu hints and the mod notes, read from each game binary
    # at runtime, carry no mutability class or gameplay-neutrality claim, and
    # the strings a player reads no instrument term. A barred term planted in
    # one tooltip (the negative control) must fail it. No disc.
    foreach(_vocab_game spyro-1 spyro-2 spyro-3)
        add_test(NAME tooltip_vocab_${_vocab_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tooltip_vocab.sh
                         --game ${_vocab_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(tooltip_vocab_${_vocab_game} PROPERTIES
            LABELS "tooling"
            TIMEOUT 120
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The User docs' surface macros: each committed macro is what
    # tools/surface_docs.py's converter writes for the literal it expands
    # to, and a per-kind negative control (a mutated def, binding, config
    # key or flag spelling) changes the expansion. Then every TOML block
    # and command line on the User pages, macros expanded, passes
    # --config-report, and a refused value is refused. No disc.
    add_test(NAME docs_surface_identity
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/surface_docs.py identity)
    set_tests_properties(docs_surface_identity PROPERTIES
        LABELS "tooling"
        TIMEOUT 120
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # The docs coverage fence (scripts/fences/surface_coverage.py): every
    # release flag, row, menu row, key action and directive of a shipped
    # game has a docs-map entry or resolves through one, every release value
    # declares a default, and the parser, --help, schema and dump agree.
    # The selftest feeds one negative control per refusal kind.
    add_test(NAME surface_coverage
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/surface_coverage.py
                     check --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(surface_coverage PROPERTIES
        LABELS "fence"
        TIMEOUT 120
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME surface_coverage_selftest
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/tests/test_surface_coverage.py
                     --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(surface_coverage_selftest PROPERTIES
        LABELS "fence"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME docs_surface_snippets
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/surface_docs.py snippets
                     --build-dir ${CMAKE_BINARY_DIR})
    set_tests_properties(docs_surface_snippets PROPERTIES
        LABELS "tooling"
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The 3D glyph text gate: the settings menu, the OSD toasts and the
    # performance readout publish glyph vertices from the game's own font,
    # decoded from guest RAM (Spyro 1) or read from the disc (Spyro 2 and 3,
    # in every level). Rows in games/<slug>/corpus/text-font-corpus.manifest.
    # Windowed on a private Xvfb display (the producer is inert headless);
    # exits 64 without xvfb-run.
    foreach(_font_game spyro-1 spyro-2 spyro-3)
        add_test(NAME text_font_${_font_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_text_font.sh
                         --game ${_font_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(text_font_${_font_game} PROPERTIES
            LABELS "slow;presentation"
            RESOURCE_LOCK openpete_game
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Terrain triangle-swing census under --lod original, where the near/HQ
    # classifier keeps the game's foreground band (--lod extended pins the
    # highest tier on every front face by design). Replays a camera-swing bk2
    # headless and reads the terrain triangle count of every canonical walk
    # over a fixed VBL window; fails on any adjacent-walk jump of 1000
    # triangles or more, the signature of a near/HQ subdivide routed without
    # the band. Engagement floor: at least 300 walks in the window and a
    # nonzero mean near-face count. Spyro 3 is the game whose draw
    # bins the HP list by distance; the check skips for the others.
    add_test(NAME nearhq_swing_spyro3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_nearhq_swing.sh
                     --game spyro-3 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(nearhq_swing_spyro3 PROPERTIES
        LABELS "presentation;terrain"
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Player sub-tick continuity. Per row of
    # games/<slug>/corpus/player-subtick-corpus.manifest, two headless replays of one
    # bk2 with the intermediate presents pinned at alpha 0 and at alpha 1, and
    # the player model's screen centroid and bounding box read per present:
    # the alpha-0 present of one tick interval must equal the alpha-1 present
    # of the interval before it to 0.01 px, and the alpha-1 present must equal
    # the tick present exactly. A part placed from the wrong tick's data fails
    # the first and passes the second. Fails a leg whose present is not
    # bracketed, so a row cannot pass on tick presents alone.
    foreach(_subtick_game spyro-1 spyro-2 spyro-3)
        add_test(NAME player_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_player_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(player_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;player"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # ThreadSanitizer, label tsan (and slow: out of the default pass). tsan_build
    # configures and builds the clang-19 TSan tree build/linux-tsan-clang in the
    # source tree (a fixture of every run row); per game, a headless run of
    # the moby, simple and emit fan-outs, the terrain sector walk and the pose
    # cache on the work pool with the fan-out identity checks, and a windowed
    # run on a private Xvfb display with the eager present thread and the
    # sim/host-loop thread split. Fails on any report not in tests/harness/tsan/suppressions.txt.
    # 3 to 5 min per row. Exit 64 = no clang-19, disc or xvfb-run.
    add_test(NAME tsan_build
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tsan.sh --build-only)
    set_tests_properties(tsan_build PROPERTIES
        LABELS "slow;tsan"
        FIXTURES_SETUP tsan_bin
        TIMEOUT 7200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    foreach(_tsan_game spyro-1 spyro-2 spyro-3)
        add_test(NAME tsan_${_tsan_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tsan.sh --game ${_tsan_game})
        add_test(NAME tsan_windowed_${_tsan_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tsan.sh --game ${_tsan_game} --windowed)
        set_tests_properties(tsan_${_tsan_game} tsan_windowed_${_tsan_game} PROPERTIES
            LABELS "slow;tsan"
            FIXTURES_REQUIRED tsan_bin
            RESOURCE_LOCK openpete_game
            TIMEOUT 1000
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Moby sub-tick continuity. Per row of
    # games/<slug>/corpus/moby-subtick-corpus.manifest, two headless replays of one
    # bk2 with the intermediate presents pinned at alpha 0 and at alpha 1, and
    # every animated moby's keyframe pose and screen centroid read per
    # present: the previous pose rebuilt at each leg must equal the pose the
    # record of the tick before carries, and a leg where that record blends
    # out of a clip must seam no worse than the same moby's other legs. A row
    # with a wbase= floor also requires every alpha-0 world bucket base to
    # equal the base the cull stored on the tick before. Fails as vacuous when
    # the row measures fewer bracketed, blending or moving legs than its floor.
    foreach(_subtick_game spyro-1 spyro-2 spyro-3)
        add_test(NAME moby_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(moby_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Moby sub-tick pose cache. One headless capture per game with sixteen
    # sub-tick re-extracts at each of two VBLs under OPENPETE_MOBY_POSE_CACHE=2:
    # every decode the cache would read back decodes again and compares its
    # vertices with the filed copy. Fails on a mismatch, vacuous on no hit.
    foreach(_pc_game spyro-1 spyro-2 spyro-3)
        add_test(NAME moby_pose_cache_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(moby_pose_cache_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The moby emit fan-out against the serial emit on the same capture:
        # every stored triangle and the tie, ordinal and scene counts.
        add_test(NAME moby_push_fanout_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=push-fanout)
        set_tests_properties(moby_push_fanout_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The snap contact pass's fan-out against the serial pass on a walking
        # bk2, every chunk of both legs: each moby's planes, the snap and carry
        # tables, the snapped flags, the census and the corner boxes.
        add_test(NAME moby_snap_fanout_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=snap-fanout)
        set_tests_properties(moby_snap_fanout_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The sub-tick record pass's decode fan-out against a serial decode of
        # the same records on a walking bk2, the deferred path included.
        add_test(NAME moby_rec_fanout_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=rec-fanout)
        set_tests_properties(moby_rec_fanout_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The paint rank's fan-out against the serial rank of the same runs on
        # a walking bk2: the canonical census and rank, and the sub-tick
        # re-rank of the runs the carry table misses.
        add_test(NAME moby_rank_fanout_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=rank-fanout)
        set_tests_properties(moby_rank_fanout_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The sub-tick re-rank's skip of runs whose absent faces contest
        # nothing against the full re-rank of the same runs on a walking bk2:
        # the contested order the select reads.
        add_test(NAME moby_rank_absent_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=rank-absent)
        set_tests_properties(moby_rank_absent_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The sub-tick re-confirm of held provisional occlusion admissions on
        # against off on a walking bk2: eight captured presents byte for byte,
        # with a moby dropped on one of them.
        add_test(NAME moby_occl_held_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=occl-held)
        set_tests_properties(moby_occl_held_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 1200
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The snap capture limited to the drawn faces near each moby's contact
        # box against the full walk of the same chunk, on a walking bk2 with
        # the terrain cache forced on: every moby's planes, the snap table and
        # carry rows, the snapped flags and the contact census.
        add_test(NAME moby_snap_listed_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=snap-listed)
        set_tests_properties(moby_snap_listed_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # One snap resolve per sub-tick present against one per walk pass, on
        # a walking bk2 with the terrain cache forced on: the packed snap
        # table words and the table's insertion-order digest.
        add_test(NAME moby_snap_rex_merge_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=snap-rex-merge)
        set_tests_properties(moby_snap_rex_merge_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The snap carry's corner input test against the corner comparison it
        # skips, on the still-camera rows: every moby compared under a camera
        # bit-equal to its endpoint's takes both.
        add_test(NAME moby_snap_corner_eq_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=snap-corner-eq)
        set_tests_properties(moby_snap_corner_eq_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The lean snap queue against the queue as before, on a walking bk2
        # with the terrain cache forced on: every resolved moby's triangle and
        # union bounds computed at the resolve against those of the queue, and
        # every moby's corners gathered into the queue against the copy.
        add_test(NAME moby_snap_queue_lean_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=snap-queue-lean)
        set_tests_properties(moby_snap_queue_lean_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # The snap's screen-size bound at 16 px on the still-camera rows with
        # the terrain cache forced on: mobys left out of the tick's resolve
        # and of the sub-tick resolves, and in-between mobys taking their
        # endpoint's decision. Fails when the bound is set and nothing is left out.
        add_test(NAME moby_snap_min_px_${_pc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_pc_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=snap-min-px)
        set_tests_properties(moby_snap_min_px_${_pc_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The simple-moby decode's projection moved into each record's job,
    # against the serial prefix's projection of the same record: every
    # projected field and the paint key's integer RTPS. Spyro 1 has no
    # simple-model decode.
    foreach(_spd_game spyro-2 spyro-3)
        add_test(NAME moby_sim_proj_defer_${_spd_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_spd_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=sim-proj-defer)
        set_tests_properties(moby_sim_proj_defer_${_spd_game} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The terrain decode cache's vertex-hash memo against the hash it skips,
    # with the terrain cache forced on: every skipped hash is computed and
    # compared with the stored one.
    foreach(_vhm_game spyro-1 spyro-2 spyro-3)
        add_test(NAME terrain_vhash_memo_${_vhm_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_pose_cache.sh
                         --game ${_vhm_game} --build-dir=${CMAKE_BINARY_DIR} --instrument=tsc-vhash-memo)
        set_tests_properties(terrain_vhash_memo_${_vhm_game} PROPERTIES
            LABELS "presentation;terrain"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Moby occlusion census. Per row of
    # games/<game>/corpus/moby-occlusion-corpus.manifest, three headless legs at
    # one freecam pose (extended LOD, extended LOD with the simple-kind
    # occlusion test off, original LOD) with sub-tick presents, reading the
    # per-kind decode census: records, culled records, vertices and triangles.
    # A hidden record decoded on every present costs time and moves no pixel,
    # which neither the golden nor the oracle sees. Fails when the extended
    # decode exceeds a row's bound over original LOD, when a row's visible
    # extension changes, or when an extended screenshot differs from the leg
    # with the test off; vacuous without bracketed sub-tick presents, census
    # lines or, where the row asks, a culled record. Spyro 1 has no simple
    # pass, so its rows read the animated kind alone. Spyro 2 and 3 add pan
    # rows: a camera-pan bk2 replayed windowed under Xvfb three times with
    # the occlusion audit on, failing on a wrong cull the camera turn caused.
    foreach(_occl_game spyro-1 spyro-2 spyro-3)
        string(REPLACE "-" "" _occl_tag "${_occl_game}")
        add_test(NAME moby_occlusion_${_occl_tag}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_occlusion.sh
                         --game ${_occl_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(moby_occlusion_${_occl_tag} PROPERTIES
            LABELS "presentation;moby"
            TIMEOUT 2700
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Vulkan validation errors on the boot-to-level path. One headless run per
    # game under --renderer vk with VK_LAYER_KHRONOS_validation loaded, from
    # boot through --skip-to-level to a native screenshot, asserting zero
    # `Validation Error` lines. API misuse the driver tolerates changes no
    # pixel and no gameplay byte, so neither the presentation golden nor the
    # oracle can see it. Skips when the binary or the disc is missing; fails as
    # vacuous when the layer did not load or the render path drew nothing.
    foreach(_vkval_game spyro-1 spyro-2 spyro-3)
        add_test(NAME vk_validation_${_vkval_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_vk_validation.sh
                         --game ${_vkval_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(vk_validation_${_vkval_game} PROPERTIES
            LABELS "presentation;validation"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Host reads across a level load. One headless run per game through
    # --skip-to-level under --render-paths moby, whose draw hooks run the
    # heavy-fog scan on every tick of the load, asserting zero `reached no
    # device` lines: a host-side scan that follows a sector-table entry the
    # load overwrote reads off the bus. Fails as vacuous when no scan found a
    # far colour. The same runs cover the trailing-edge reveal, whose first
    # canonical pass after the load tests the previous level's queue.
    foreach(_hfs_game spyro-1 spyro-2 spyro-3)
        add_test(NAME heavy_fog_scan_${_hfs_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_heavy_fog_scan.sh
                         --game ${_hfs_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(heavy_fog_scan_${_hfs_game} PROPERTIES
            LABELS "presentation;terrain"
            TIMEOUT 900
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Runahead prediction contract. Per row of
    # games/<slug>/corpus/runahead-corpus.manifest, two headless replays of one bk2:
    # the native capture at the row's VBL under --runahead <depth>, and the
    # capture at that VBL plus 2*<depth> without runahead. --runahead presents
    # the state <depth> ticks ahead of the committed timeline, so the two are
    # the same picture and any pixel between them is a canonical presentation
    # latch that a speculative tick wrote. A second pair captures the same two
    # VBLs through the sub-tick re-extract (the windowed present path), which
    # replays the canonical epoch's producer captures; a speculative tick that
    # moves the epoch or leaves a capture set behind shows there. Fails as
    # vacuous when the run never armed runahead, retired no led prediction, had
    # its speculation adopted into the real timeline before the row's VBL, or
    # the re-extract pair drew no actor or player triangle.
    foreach(_ra_game spyro-1 spyro-2 spyro-3)
        add_test(NAME runahead_ident_${_ra_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_runahead_ident.sh
                         --game ${_ra_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(runahead_ident_${_ra_game} PROPERTIES
            LABELS "presentation;runahead"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # 2D-UI carry identity across sub-tick presents. Per row of
    # games/<slug>/corpus/carry-subtick-corpus.manifest, three headless replays of one
    # bk2 with the intermediate presents pinned at alpha 0.25, 0.5 and 0.75,
    # and every interpolated carry triangle checked for a crossed pair: one
    # whose partner is another entry's predecessor, so the interpolation sweeps
    # the element between two different sprites for the length of a tick. Fails
    # as vacuous when the row interpolates fewer triangles than its floor.
    foreach(_subtick_game spyro-1 spyro-2 spyro-3)
        add_test(NAME carry_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_carry_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(carry_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;carry"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Sub-tick interpolation of the gem-glint stars. Per row of
    # games/<slug>/corpus/glint-subtick-corpus.manifest, six headless replays of one
    # bk2 - the two canonical ticks of an interval and the intermediate present
    # inside it, each captured with the glint feed on and off - and the
    # leave-one-out mask's per-star centroid compared against
    # (1-alpha)*canon(V-2) + alpha*canon(V). Spyro 1 has no such routine and
    # registers no row. Fails as vacuous when a leg's mask is empty, when a
    # named gem resolves ambiguously, or when its canonical motion over the
    # interval is below the row's floor.
    # Sub-tick interpolation of Zoe's zap. Per row of
    # games/<slug>/corpus/zap-subtick-corpus.manifest, three headless replays pinning
    # the intermediate present at alpha 0, 0.5 and 1, with the record's own
    # projected head and tail traced per present and each required to sit on
    # the alpha blend of the two canonical ticks. Spyro 1 has no such routine
    # and registers no row. Fails as vacuous when a leg feeds nothing, when the
    # legs pair different records, or when the canonical motion is below the
    # row's floor.
    foreach(_zap_game spyro-2 spyro-3)
        add_test(NAME zap_subtick_${_zap_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_zap_subtick.sh
                         --game ${_zap_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(zap_subtick_${_zap_game} PROPERTIES
            LABELS "presentation;zap"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The blended-world draw order on the identity instant: the game render's
    # own block against the sub-tick replay's, run for run, keyed by provenance
    # and blend class. The classes composite in emission order and the blend
    # equations do not commute once a channel clamps, so a replay that emits
    # its producers in a different order than the guest linked them draws a
    # picture the canonical present never produced. Fails as vacuous when
    # either leg draws no blended run.
    foreach(_bwo_game spyro-2)
        add_test(NAME blendorder_ident_${_bwo_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_blendorder_ident.sh
                         --game ${_bwo_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(blendorder_ident_${_bwo_game} PROPERTIES
            LABELS "presentation;blendorder"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # A second sub-tick re-extract inside one canonical epoch: per row of
    # games/<slug>/corpus/subtick-repeat-corpus.manifest, an alpha 0.25
    # re-extract followed by a captured alpha 0.5 one must be pixel-identical
    # to a single alpha 0.5 re-extract, with no geometry replayed from the
    # first. Fails as vacuous when a re-extract is unbracketed or the alpha
    # 0.25 leg equals the alpha 0.5 leg.
    foreach(_subtick_game spyro-1 spyro-2 spyro-3)
        add_test(NAME subtick_repeat_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_subtick_repeat.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(subtick_repeat_${_subtick_game} PROPERTIES
            LABELS "presentation;subtick_repeat"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    foreach(_subtick_game spyro-2 spyro-3)
        add_test(NAME glint_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_glint_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(glint_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;glint"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    foreach(_subtick_game spyro-2 spyro-3)
        add_test(NAME glow_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_glow_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(glow_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;glow"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Spyro 2 has no shadow row: its two candidate movies carry only shadows
    # whose moby is static in the world or none at all over the window, and a
    # blob that cannot move measures nothing. The harness SKIPs there for want
    # of the manifest until a movie with a moving shadowed moby is recorded.
    foreach(_subtick_game spyro-1 spyro-3)
        add_test(NAME shadow_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_shadow_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(shadow_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;shadow"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # The remaining render paths of the shared feeder harness. Each is one
    # channel of tests/harness/check_feeder_subtick.sh, with its own manifest per
    # game; the game list per path is the set whose rows engage and pass
    # today, and each manifest records why the games left out have no row.
    foreach(_subtick_path_game
            "terrain;spyro-1" "terrain_tanim;spyro-1" "terrain_tanim;spyro-3"
            "terrain_canim;spyro-1" "terrain_canim;spyro-2" "terrain_vis;spyro-1"
            "cyclorama;spyro-2" "cyclorama;spyro-3"
            "particles;spyro-1" "particles;spyro-2" "particles;spyro-3"
            "flame;spyro-2" "flame;spyro-3"
            "tracer;spyro-2" "tracer;spyro-3"
            "moby_mask;spyro-1" "moby_mask;spyro-2" "moby_mask;spyro-3"
            "glint_ext;spyro-2" "glint_ext;spyro-3"
            "glint_rot;spyro-2" "glint_rot;spyro-3")
        list(GET _subtick_path_game 0 _sp)
        list(GET _subtick_path_game 1 _sg)
        add_test(NAME ${_sp}_subtick_${_sg}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_${_sp}_subtick.sh
                         --game ${_sg} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(${_sp}_subtick_${_sg} PROPERTIES
            LABELS "presentation;subtick;${_sp}"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Sub-tick interpolation of the homeworld portal preview's aperture. Per
    # row of games/<slug>/corpus/portal-subtick-corpus.manifest, three headless replays
    # of one bk2 - the intermediate present at alpha 0, 0.5 and 1 - with the feeder's
    # emitted aperture polygon read out of each run's census and every named
    # corner required to sit on the alpha blend of the two canonical ticks the
    # same run recorded. The statistic is the polygon rather than a pixel mask:
    # the aperture spans most of the frame. Spyro 1 draws its portals as a
    # re-projected dome and registers no row. Fails as vacuous when a present
    # feeds no portal triangle, when a named corner resolves ambiguously, or
    # when its canonical motion over the interval is below the row's floor.
    foreach(_subtick_game spyro-2 spyro-3)
        add_test(NAME portal_subtick_${_subtick_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_portal_subtick.sh
                         --game ${_subtick_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(portal_subtick_${_subtick_game} PROPERTIES
            LABELS "presentation;portal"
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Vertex-stream field resolution on a stock boot: one headless 206-VBL
    # boot with a native screenshot armed, and every frame word the moby walk
    # decodes required to name an address. An unresolved field there is the
    # walk decoding a record the game never queued and reading the frame word
    # from unmapped memory. Spyro 1 only: it is the one game whose reader
    # routes frame words through the handle resolver that feeds the census
    # (games/spyro-1/renderer/moby_game.c, s1_vstream_init), so on the others
    # the run decodes no field and the case could only read as vacuous.
    add_test(NAME vstream_boot_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_vstream_boot.sh
                     --game spyro-1 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(vstream_boot_spyro-1 PROPERTIES
        LABELS "presentation;vstream"
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Capture survival under contention. Runs the presentation golden's own
    # legs at CORPUS_JOBS=nproc for three rounds and keeps each round's log.
    # A golden divergence (exit 1) is a rendering verdict and passes here;
    # what fails is a leg that died - an exit outside {0,1}, a MISSING tile,
    # or a "capture incomplete" line. Engagement: compared > 0 per round.
    # Registered for the games that have a golden to run.
    foreach(_contention_game spyro-1 spyro-3)
        add_test(NAME contention_${_contention_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_contention.sh
                         --game ${_contention_game} --rounds 3
                         --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(contention_${_contention_game} PROPERTIES
            LABELS "slow;presentation"
            RESOURCE_LOCK openpete_game
            # Three whole-corpus runs: the golden's own timeout, tripled, with
            # headroom for the load this deliberately creates.
            TIMEOUT 14400
            SKIP_RETURN_CODE 64
            # Serial: the point is the load the corpus itself makes, so a
            # concurrent ctest row would be measuring something else.
            RUN_SERIAL TRUE
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Freeze-backdrop source frame, windowed. Per row of
    # games/<slug>/corpus/freeze-backdrop-corpus.manifest, one windowed replay
    # of a bk2 that enters the pause menu, counting the composed presents the
    # live path accepted as the backdrop's source frame. When that count is
    # zero nothing is ever snapshotted and the pause backdrop is a flat fill of
    # the clear colour. The source frame is chosen in the live compose, which
    # no headless run executes, so the presentation golden cannot see the class
    # and this check drives a real window on a private Xvfb display. Skips
    # (exit 64) without xvfb-run, a binary, a disc or the bk2; a missing manifest fails (exit 1), so a row
    # exists only for a game that has the manifest. The corpus directory is a
    # configure dependency: adding a game's manifest adds its row on the next
    # build.
    foreach(_freeze_game spyro-1 spyro-2 spyro-3)
        set(_freeze_corpus ${CMAKE_CURRENT_SOURCE_DIR}/games/${_freeze_game}/corpus)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_freeze_corpus})
        if(NOT EXISTS ${_freeze_corpus}/freeze-backdrop-corpus.manifest)
            continue()
        endif()
        add_test(NAME freeze_backdrop_${_freeze_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_freeze_backdrop.sh
                         --game ${_freeze_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(freeze_backdrop_${_freeze_game} PROPERTIES
            LABELS "presentation;freeze;windowed"
            TIMEOUT 3600
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Paused repaint draws the retained terrain: one windowed row per game
    # pauses at a pinned VBL (OPENPETE_PAUSE_AT_VBL), steps one tick, resumes,
    # and compares every paused present's terrain-cache draw count with the
    # count of the scene shown before it. The paused repaint returns under
    # --headless and eager production needs a window, so no headless capture
    # reaches the path. Runs on a private Xvfb display; skips (exit 64) without
    # xvfb-run, a binary or a disc.
    foreach(_pr_game spyro-1 spyro-2 spyro-3)
        add_test(NAME pause_repaint_${_pr_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_pause_repaint.sh
                         --game ${_pr_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(pause_repaint_${_pr_game} PROPERTIES
            LABELS "presentation;terrain;windowed"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1200
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Eager canonical presents under scene-ring pressure: one windowed row per
    # game stalls the present thread (OPENPETE_PRESENT_STALL_MS) until the
    # ring refuses publishes, and fails on a canonical present with no
    # retained terrain, a retained-terrain state overwritten while its scene
    # is unretired, or a canonical present composing fewer triangles of a
    # render path than its scene was published with. Eager production needs a
    # window, so no headless capture reaches the path. Runs on a private Xvfb
    # display; skips (exit 64) without xvfb-run, a binary or a disc.
    foreach(_rp_game spyro-1 spyro-2 spyro-3)
        add_test(NAME eager_ring_pressure_${_rp_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_eager_ring_pressure.sh
                         --game ${_rp_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(eager_ring_pressure_${_rp_game} PROPERTIES
            LABELS "presentation;terrain;windowed"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1200
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Retained-terrain GPU cache hole census, windowed on the machine's own
    # GPU. Per freecam-corpus row, two legs of one build on one device
    # differing only in --terrain-cache, and a count of the pixels the cached
    # leg renders as the framebuffer clear where the CPU walk feed renders
    # terrain. The class is an exact-bit tie in the fragment key contest, so it
    # is device-dependent: a software rasteriser cannot produce it and the
    # headless presentation golden, which runs llvmpipe with the cache off,
    # cannot separate a build carrying it from a build that does not. Skips
    # (exit 64) without a display, a windows-mingw exe, a disc or PIL.
    foreach(tcgh_game spyro-1 spyro-2 spyro-3)
        string(REPLACE "-" "" tcgh_slug ${tcgh_game})
        add_test(NAME tercache_gpu_holes_${tcgh_slug}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tercache_gpu_holes.sh
                         --game ${tcgh_game})
        set_tests_properties(tercache_gpu_holes_${tcgh_slug} PROPERTIES
            LABELS "slow;presentation;terrain;windowed"
            RESOURCE_LOCK openpete_game
            # Two windowed legs per corpus row, each booting to a pinned
            # post-load VBL; the Spyro 1 corpus is the long one.
            TIMEOUT 21600
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Retained-terrain GPU cache vs CPU terrain walk, whole-frame A/B over a
    # bk2 camera swing, windowed on the machine's own GPU. Four legs of one
    # build (cache on and off, under --lod extended and --lod original) at
    # --render-fps 0; the extended pair may differ by no more than the
    # original pair plus a margin at every sampled VBL, the original pair
    # being the in-run control. Refuses a verdict unless the cache-on legs
    # report retained sectors and the cache-off legs report none. Skips (exit
    # 64) without a display, a windows-mingw exe, a disc, the bk2 or PIL.
    add_test(NAME tercache_cache_ab_spyro3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_tercache_cache_ab.sh
                     --game spyro-3)
    set_tests_properties(tercache_cache_ab_spyro3 PROPERTIES
        LABELS "slow;presentation;terrain;windowed"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Multi-Spyro sub-tick records:
    # the spyro-coop mod draws N dragons through the game's own player
    # renderers; each corpus case asserts the OPENPETE_SPYRO_RECDBG hook-call
    # counts, the hook-vs-ring byte identity and the staging census with the
    # mod loaded. The mod is out of tree: SPYRO_COOP_DIR names its source dir,
    # else the test skips (exit 64).
    add_test(NAME spyro_records
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_spyro_records.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(spyro_records PROPERTIES
        LABELS "slow;presentation;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The pacing analysis suite's own maths, against synthetic inputs with
    # known answers. Fast, no PSX boot, no display: a wrong Allan deviation or
    # a mis-set gate tolerance is worse than none, because it looks
    # authoritative and nobody re-derives it.
    add_test(NAME pacing_stats
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pacing/test_pacestats.py)
    set_tests_properties(pacing_stats PROPERTIES
        LABELS "pacing"
        TIMEOUT 300
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # The level-entry anchor on a recorded Spyro 1 portal pace log (the load
    # tick, not a present interval) and on a synthetic flagged entry.
    add_test(NAME pacing_entry
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/pacing/test_entry.py)
    set_tests_properties(pacing_entry PROPERTIES LABELS "pacing" TIMEOUT 60)

    # The standing pacing gate: does the sim clock hold its grid, does it
    # hold BECAUSE presents shed (proved by fault injection, not by a machine
    # fast enough never to exercise the mechanism), and does it hold in a level
    # rather than only on the attract demo. Exit 3 = vacuous (nothing engaged)
    # and fails the test deliberately: a gate that cannot reach its own subject
    # must never read as a pass. 64 = SKIP (no binary / disc / display).
    #
    # It runs four 120 s legs and opens a window for three of them. Plain
    # ctest has no label exclusion, so the test SKIPs unless
    # OPENPETE_PACING_TESTS=1: `OPENPETE_PACING_TESTS=1 ctest -L pacing`.
    add_test(NAME pacing_strict
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_pacing.sh --ctest)
    set_tests_properties(pacing_strict PROPERTIES
        LABELS "slow;pacing"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Eager decimation-level recovery after a stall: one windowed vsync-on row
    # (the spyro-3 icy-peak bk2 with a 25 ms cost injected into every
    # intermediate scene from VBL 2200 to 2320) fails while the level stays
    # above its pre-stall value for more than 10 spans after the stall, or
    # after a level-entry stall. Exit 2 (VACUOUS) when no span was raised:
    # under Xvfb the present thread's cost floors the grid at one slot per
    # span on a loaded host. Eager production needs a window, so no headless
    # gate reaches the planner; eager_ladder_test replays a recorded stall.
    # Runs on a private Xvfb display; skips (exit 64) without xvfb-run, or
    # without a binary, disc, card or bk2.
    add_test(NAME eager_ladder_recovery_spyro-3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_eager_ladder_recovery.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(eager_ladder_recovery_spyro-3 PROPERTIES
        LABELS "pacing;windowed"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Extended-LOD reveal stability: the spyro-2 summer_forest spawn pose,
    # windowed, fails while the reveal's appended record count changes across
    # 60 canonical ticks (a moby admitted on the occlusion cache's rebuilt
    # rect, culled once its decode refreshes the cache, admitted again when it
    # ages) or a deterministic sub-tick capture differs with the occlusion
    # confirmation switched off. Headless runs build no occlusion grid, so no
    # headless gate reaches it. Runs on a private Xvfb display; skips (exit
    # 64) without xvfb-run, or without a binary, disc or PIL.
    add_test(NAME moby_reveal_stability_spyro-2
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_reveal_stability.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(moby_reveal_stability_spyro-2 PROPERTIES
        LABELS "presentation;moby;windowed"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Display ownership under eager production: the Spyro 2 attract demo with
    # OPENPETE_IDENT_AUDIT=300 borrows the display from the present thread
    # once per canonical present; no device call may come from a thread that
    # does not own the display (seam 0), over at least 400 drains with eager
    # engaged. Exit 3 = vacuous (fewer drains, or eager never engaged) and
    # fails; 64 = SKIP (no xvfb-run, no binary or disc).
    add_test(NAME display_seam_spyro-2
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_display_seam.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(display_seam_spyro-2 PROPERTIES
        LABELS "launcher;pacing;windowed"
        RESOURCE_LOCK openpete_game
        TIMEOUT 720
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Profiler present cadence under eager production, on the Windows exe and
    # the real GPU: --profile-out's present interval p99 (profile-summary.py)
    # within 0.5 ms of the pace log's OP_PACE_PRESENT interval p99, both after
    # canonical tick 300 of one run of row s1_h0_r1 (Spyro 1 town_square high
    # pose, --vsync 0 --render-fps 1000). Exit 3 = vacuous (no eager scene, no
    # present after the cut) and fails; 64 = SKIP (no Windows exe or disc);
    # 65 = the bench preconditions refused a contended machine. The windows
    # label names the device it needs. Plain ctest has no label exclusion, so
    # the test SKIPs unless OPENPETE_WINDOWS_TESTS=1: `OPENPETE_WINDOWS_TESTS=1
    # ctest -L windows`. The Linux analogue
    # (lavapipe under xvfb-run, --platform linux) is run by hand.
    add_test(NAME profile_present_cadence_windows
             COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_profile_present_cadence.py
                     --platform windows --ctest)
    set_tests_properties(profile_present_cadence_windows PROPERTIES
        LABELS "slow;pacing;windows"
        RESOURCE_LOCK openpete_game
        TIMEOUT 600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # --record video-capture smoke test. Boots the headless demo with
    # --record and uses ffprobe to assert a playable VFR H.264 stream with
    # even dimensions, the reported frame count, monotonic non-uniform PTS,
    # and window-independent (bars-free) capture at 21:9. Skips cleanly
    # (exit 64) if the disc/binary/ffprobe are absent.
    add_test(NAME record_smoke
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_record.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=300)
    set_tests_properties(record_smoke PROPERTIES
        LABELS "slow;tooling"
        RESOURCE_LOCK openpete_game
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # --input-lock gate. Replays one key-and-pointer injection script three
    # ways on the headless build: filed with the physical devices under the
    # lock (nothing may change), filed with them without the lock (the
    # negative control: free camera, menu, motion totals and the quit binding
    # all respond), and marked as injected under the lock (still flows).
    # Exit 2 when the negative control did not engage.
    add_test(NAME input_lock
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_input_lock.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(input_lock PROPERTIES
        LABELS "slow;input"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Sandbox gate. Runs every writer a session reaches (log, pipeline
    # cache, savestate, player screenshot, settings save, default bk2, bug
    # bundle, hang files, a replay) under OPENPETE_SANDBOX and fails when a
    # per-user listing changes or a file is missing under the run's root;
    # its controls are a root inside a per-user directory (exit 66, nothing
    # written) and a run without a root (the listing changes).
    add_test(NAME sandbox
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_sandbox.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(sandbox PROPERTIES
        LABELS "slow;tooling"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Settings-menu autosave gate. Drives the menu with an injected key
    # script against a --config file per leg: a change written at the
    # debounce deadline (a typed-flag-pinned knob included, freecam left
    # out, the hand-written section above the marker kept), a change equal
    # to the global value removed on menu close, and an injected write
    # failure that keeps the file and retries its dirty knob at shutdown.
    # Exit 2 when no write engaged.
    add_test(NAME settings_autosave
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_settings_autosave.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(settings_autosave PROPERTIES
        LABELS "slow;input"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Binding page gate: captures, swaps, restores and refusals driven by the
    # injection script's rebind verbs and the virtual pad, [saved.keys] rows
    # written through the settings writer, port-0 records held neutral
    # through a capture and its latched inputs, replay and record identity,
    # and the mutation controls that must fail. Exit 2 when a leg's capture
    # did not engage.
    add_test(NAME rebind
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_rebind.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(rebind PROPERTIES
        LABELS "slow;input"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Performance benchmark pipeline smoke - replays a few bk2 movies through
    # the profiler (--profile-out) and asserts the trace/summary/rollup pipeline
    # runs and parses. NOT a perf-threshold gate (CI hardware varies); the
    # baseline regression check (tests/harness/bench-corpus.py --baseline) is a
    # manually-run tool. Registered only when spyro-1 is configured.
    if(TARGET openpete-spyro1)
        add_test(NAME bench_corpus_smoke
                 COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/bench-corpus.py
                         --exe ${_spyro1_bin}
                         --movies artisans-bridge,stone-hill-entry
                         --frame-limit 4000 --max-runtime-seconds 90
                         --skip-ticks 30
                         --out-dir ${CMAKE_BINARY_DIR}/bench-smoke --run-id ctest)
        set_tests_properties(bench_corpus_smoke PROPERTIES
            LABELS "slow;bench"
            RESOURCE_LOCK openpete_game
            TIMEOUT 400
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # Channel registry audit - fast static check (no PSX boot). Cross-
    # references games/spyro-1/manifest/render_channels.toml against the
    # channel-tagged substitutions in the generated main-EXE C and the
    # recompiler's hand-written `addr ==` arms; fails on missing, orphaned
    # or mismatched substitutions and on zero sites reconciled.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME channel_registry_audit
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_channel_registry.py)
        set_tests_properties(channel_registry_audit PROPERTIES
            LABELS "determinism"
            TIMEOUT 30
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

        # Channel signature anchors: calibrate +
        # verify that every determinism-critical channel site is uniquely
        # anchorable by an instruction-pattern signature (local or
        # function-scoped) in the reference binary, and that the matcher
        # relocates a spliced window (self-test). Skips cleanly if the regen
        # artifacts (PSX.EXE / discovered_functions.log) aren't present.
        add_test(NAME channel_signatures
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_channel_signatures.py
                         --out ${CMAKE_CURRENT_BINARY_DIR}/channel_signatures.txt)
        set_tests_properties(channel_signatures PROPERTIES
            LABELS "determinism"
            TIMEOUT 120
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    # The launcher's checks (tests/harness/check_launcher.py), one test per
    # case: each runs the binary windowed under its own xvfb-run display, with
    # the minimal window manager below and a scratch data root of its own, so
    # the cases run in parallel; exits 64 without xvfb-run, the binary or the
    # window manager. Every run of the four launcher harnesses starts in a
    # session of its own, ended whole when the run returns or times out (the
    # reap case proves it on a run that hangs on purpose).
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_package(X11 QUIET)
        if(X11_FOUND)
            add_executable(openpete_xvfb_wm tests/harness/xvfb_wm.c)
            target_link_libraries(openpete_xvfb_wm PRIVATE X11::X11)
        endif()
    endif()
    if(Python3_Interpreter_FOUND)
        foreach(_launcher_case manifest layout page handoff window-fit window-default font density reap)
            add_test(NAME launcher_${_launcher_case}
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_launcher.py
                             ${_launcher_case} --build-dir ${CMAKE_BINARY_DIR})
            set_tests_properties(launcher_${_launcher_case} PROPERTIES
                LABELS "launcher"
                TIMEOUT 900
                SKIP_RETURN_CODE 64
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        endforeach()
        # The launcher's flow checks (tests/harness/check_launcher_flow.py):
        # start-up time, the pick's resolved options, the multi-install dump,
        # the launcher asset cache and its box picture, the quit during work, and the job runner's module cases, which
        # need the first-boot pipeline and exit 64 on a build without it (any
        # but the *-launcher presets).
        foreach(_launcher_case startup resolution multi-install multi-install-extract job-ingest-stop job-pick-same
                                job-pool-low job-pick-other lock failed-marker headless-no-disc bind-repair
                                ingest-held cache cache-ingest multi-install-launch
                                job-ingest-stop-assets job-pick-other-assets
                                shader-step-during shader-step-after shader-step-order input picker
                                sky-cache sky-reveal
                                preview preview-hold preview-stop
                                quit-copy quit-translate quit-build quit-assets quit-east-once
                                quit-east-twice quit-escape quit-close quit-page-sigterm error-refuse error-staged error-build ingest-retry
                                error-link mods-chip exit-return exit-return-env exit-direct exit-close exit-fullscreen unsupported-mark)
            add_test(NAME launcher_flow_${_launcher_case}
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_launcher_flow.py
                             ${_launcher_case} --build-dir ${CMAKE_BINARY_DIR})
            set_tests_properties(launcher_flow_${_launcher_case} PROPERTIES
                LABELS "launcher"
                TIMEOUT 1800
                SKIP_RETURN_CODE 64
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        endforeach()
        # The start-up delta is bounded against a figure set on an idle host
        # (its CPU time on any host): run alone under ctest -j.
        set_tests_properties(launcher_flow_startup PROPERTIES RUN_SERIAL TRUE)
        # The launcher's present rate (tests/harness/check_launcher_present.py):
        # one present per refresh while shown, none while minimised or hidden,
        # and a display change that rebuilds the swapchain at its refresh;
        # timed (presents per second), so it runs alone under ctest -j.
        add_test(NAME launcher_present
                 COMMAND ${Python3_EXECUTABLE}
                         ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_launcher_present.py
                         --build-dir ${CMAKE_BINARY_DIR})
        set_tests_properties(launcher_present PROPERTIES
            LABELS "launcher"
            TIMEOUT 300
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        # Swapchain rebuilds on a window the driver keeps bound after
        # vkDestroySwapchainKHR (tests/harness/check_swapchain_rebuild.py):
        # OPENPETE_VK_FAULT=window-held refuses a creation on the bound window
        # unless it names the live swapchain as oldSwapchain, and
        # window-held-surface refuses it until the surface is rebuilt. The
        # launcher resizes through OPENPETE_LAUNCHER_INPUT; the game, booted
        # from the disc, through an X11 client on its display. Pass: a rebuild
        # per resize, no unrecovered creation failure, presents after the last
        # rebuild. Each run starts its own xvfb-run display; skips (exit 64)
        # without xvfb-run, the binary, the window manager or the disc.
        foreach(_sc_case launcher game)
            foreach(_sc_fault window-held window-held-surface)
                add_test(NAME swapchain_rebuild_${_sc_case}_${_sc_fault}
                         COMMAND ${Python3_EXECUTABLE}
                                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_swapchain_rebuild.py
                                 ${_sc_case} --fault ${_sc_fault} --build-dir ${CMAKE_BINARY_DIR})
                set_tests_properties(swapchain_rebuild_${_sc_case}_${_sc_fault} PROPERTIES
                    LABELS "renderer;windowed"
                    TIMEOUT 300
                    SKIP_RETURN_CODE 64
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
            endforeach()
        endforeach()
        set_tests_properties(swapchain_rebuild_game_window-held swapchain_rebuild_game_window-held-surface
            PROPERTIES RESOURCE_LOCK openpete_game)
        # The launcher's 3D title (tests/harness/check_launcher_title.py): drawn
        # from the asset cache against the reference layout and a model of the
        # emitter, and switched in on the assets-ready reap.
        foreach(_launcher_case title-cache title-switch)
            add_test(NAME launcher_${_launcher_case}
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_launcher_title.py
                             ${_launcher_case} --build-dir ${CMAKE_BINARY_DIR})
            set_tests_properties(launcher_${_launcher_case} PROPERTIES
                LABELS "launcher"
                TIMEOUT 1800
                SKIP_RETURN_CODE 64
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        endforeach()
        # The launcher's shape pass (tests/harness/check_launcher_shapes.py):
        # the chip text's contrast, the chip fill against Spyro 3's blend and
        # the shapes' rects against the reference layout, read from the
        # binary's own readback.
        foreach(_launcher_case contrast fill layout)
            add_test(NAME launcher_shapes_${_launcher_case}
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_launcher_shapes.py
                             ${_launcher_case} --build-dir ${CMAKE_BINARY_DIR})
            set_tests_properties(launcher_shapes_${_launcher_case} PROPERTIES
                LABELS "launcher"
                TIMEOUT 1800
                SKIP_RETURN_CODE 64
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        endforeach()
        # The launcher's corner chips, the progress view's weighted bar and the
        # shapes' corner profile (tests/harness/check_launcher_chips.py): the
        # corner profile on a flat clear, the bar over a first boot (a *-launcher build; SKIP
        # otherwise), and the chips' places, texts and the level chip's click.
        foreach(_launcher_case corner progress chips)
            add_test(NAME launcher_chips_${_launcher_case}
                     COMMAND ${Python3_EXECUTABLE}
                             ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_launcher_chips.py
                             ${_launcher_case} --build-dir ${CMAKE_BINARY_DIR})
            set_tests_properties(launcher_chips_${_launcher_case} PROPERTIES
                LABELS "launcher"
                TIMEOUT 1800
                SKIP_RETURN_CODE 64
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        endforeach()
        # Timed against the sky clock and elapsed time: run alone under ctest -j.
        set_tests_properties(launcher_chips_progress launcher_chips_chips PROPERTIES RUN_SERIAL TRUE)
    endif()

    # No PSX RAM dump in the tree (scripts/fences/no-ram-dump.py). The
    # hook half refuses `git add`/`git commit` of a dump; this half scans
    # `git ls-files` by content (has_ram trace, 2 MB image, PS-X EXE) so a
    # commit made outside the hook is caught before it is pushed. The scan
    # runs its own negative controls first and refuses PASS unless they
    # were refused.
    add_test(NAME no_ram_dumps
             COMMAND python3
                     ${CMAKE_CURRENT_SOURCE_DIR}/scripts/fences/no-ram-dump.py tree
                     ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(no_ram_dumps PROPERTIES
        LABELS "fence;oracle"
        TIMEOUT 60
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Recompiler GTE data-register side effects: mtc2/lwc2 to $30 must
    # recompute LZCR, to $28 must unpack IR1-3. Recompiles a probe binary
    # with PSXRecomp and runs the emitted C against a harness. Skips when
    # PSXRecomp is not built.
    add_test(NAME recomp_gte_sideeffects
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_recomp_gte.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --cc=${CMAKE_C_COMPILER})
    set_tests_properties(recomp_gte_sideeffects PROPERTIES
        LABELS "recompiler;determinism"
        TIMEOUT 120
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # PSX oracle gate - verify openpete gameplay-state RAM matches a
    # DuckStation (real SCPH1001) baseline tick-for-tick over the 20k-tick
    # attract-demo soak (4+ level loads + gameplay). The 3 CD-load pool
    # regions are tolerated only while a load transient heals; every other
    # region must match bit-exact (see check_psx_oracle.sh / the manifest's
    # cd_load_transient flag). The golden is generated, not committed: the
    # script captures it from the pinned DuckStation fork on the first run
    # (about fifteen minutes, hence the timeout) and skips when the fork is
    # not built (see docs/src/developer/duckstation-oracle.md).
    add_test(NAME psx_oracle_match
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh)
    set_tests_properties(psx_oracle_match PROPERTIES
        LABELS "slow;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}")

    # One row per game with a recipe; the Spyro 1 row keeps its name above.
    foreach(_g spyro-2 spyro-3)
        openpete_game_bin(${_g} _gbin _gargs)
        add_test(NAME psx_oracle_match_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh --game ${_g})
        set_tests_properties(psx_oracle_match_${_g} PROPERTIES
            LABELS "slow;oracle"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            ENVIRONMENT "OPENPETE_BIN=${_gbin}")
    endforeach()

    # Live controller input against DuckStation, one row per game: the
    # game's timed pad stream (tests/livepad/) through openpete's live input
    # path and through DuckStation's live mapping (-bk2-phys), Spyro's
    # position and every region compared per record on the clock row
    # (tests/harness/check_livepad.sh) under openpete's default
    # --stick-response, console; skips on a build without the flag, or
    # without the fork's -bk2-phys.
    # Run by `ctest -L livepad`; `slow` keeps it out of the default run.
    foreach(_g spyro-1 spyro-2 spyro-3)
        openpete_game_bin(${_g} _gbin _gargs)
        add_test(NAME livepad_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_livepad.sh --game ${_g})
        set_tests_properties(livepad_${_g} PROPERTIES
            LABELS "slow;livepad"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            ENVIRONMENT "OPENPETE_BIN=${_gbin}")
    endforeach()

    # The windowed oracle row: the same golden verified with the native
    # renderer drawing into a real window under llvmpipe, the eager present
    # thread active. Headless never runs that path, so a presentation-side
    # write into a hashed row is reachable only here. Paced to the display,
    # hence the reduced VBL count (~3k records, the first level load
    # included); FAIL unless the present thread presented; SKIP without a
    # display or the lvp ICD. The windowed rows run with
    # OPENPETE_SUBTICK_RAM_GUARD (a store into guest RAM inside a sub-tick
    # present fails the row) and OPENPETE_PRESENT_RAM_ASSERT (a store through
    # a pointer the present thread resolved aborts it), and fail when the
    # scene-ring and present-thread exit counts do not reconcile
    # (check_psx_oracle.sh). Negative controls: OPENPETE_SUBTICK_FAULT_STORE=1
    # and OPENPETE_PRESENT_FAULT_STORE=1 each fail every windowed row.
    set(_windowed_guards "OPENPETE_ORACLE_WINDOWED=1;OPENPETE_SUBTICK_RAM_GUARD=1;OPENPETE_PRESENT_RAM_ASSERT=1")
    add_test(NAME psx_oracle_windowed
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh)
    set_tests_properties(psx_oracle_windowed PROPERTIES
        LABELS "slow;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin};${_windowed_guards};OPENPETE_FRAME_LIMIT=6000")

    # The Spyro 3 windowed row. Same contract as the row above on the Spyro 3
    # golden: --render-fps 60 is above the 29.913 Hz tick rate, so every
    # non-drawing VBL plans a sub-tick present and the run's re-extract count
    # is nonzero; the row fails if it is zero. The frame limit reaches VBL
    # 4000, past the demo's first level load (the bulk reads at VBLs
    # 2903-3475 in the golden's load schedule), so the sub-tick leg is
    # verified across a load transient and not only in steady gameplay.
    add_test(NAME psx_oracle_windowed_spyro-3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh
                     --game spyro-3 -- --render-fps 60)
    set_tests_properties(psx_oracle_windowed_spyro-3 PROPERTIES
        LABELS "slow;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro3_bin};${_windowed_guards};OPENPETE_FRAME_LIMIT=4000")

    # The Spyro 2 windowed row, on the contract of the Spyro 3 row. The frame
    # limit reaches VBL 5000, past the demo's first level load (the bulk
    # reads at VBLs 3678-4219 in the golden's load schedule).
    add_test(NAME psx_oracle_windowed_spyro-2
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh
                     --game spyro-2 -- --render-fps 60)
    set_tests_properties(psx_oracle_windowed_spyro-2 PROPERTIES
        LABELS "slow;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro2_bin};${_windowed_guards};OPENPETE_FRAME_LIMIT=5000")

    # The sub-tick RAM-write row: the windowed oracle over the full Spyro 1
    # golden on a private Xvfb display, --render-fps 60, with
    # OPENPETE_SUBTICK_RAM_GUARD mapping guest RAM read-only for every
    # sub-tick present. The guard logs a store and lets it through, so a
    # write-and-restore passes the oracle; the row fails on any
    # `GUEST RAM WRITTEN` line, and is VACUOUS at zero re-extracts or zero
    # armed presents. Paced to the display (~12 min). Negative control:
    # OPENPETE_SUBTICK_FAULT_STORE=1 must fail it.
    add_test(NAME psx_oracle_subtick_ram_guard
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_subtick_ram_guard.sh)
    set_tests_properties(psx_oracle_subtick_ram_guard PROPERTIES
        LABELS "slow;oracle;subtick"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}")

    # The Spyro 3 twin of the row above, over the full Spyro 3 golden.
    add_test(NAME psx_oracle_subtick_ram_guard_spyro-3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_subtick_ram_guard.sh
                     --game spyro-3)
    set_tests_properties(psx_oracle_subtick_ram_guard_spyro-3 PROPERTIES
        LABELS "slow;oracle;subtick"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro3_bin}")

    # The headless re-extract row, one per game: the full golden headless
    # with OPENPETE_ORACLE_REEXTRACT=1, so every non-drawing VBL re-extracts
    # the scene through the sub-tick view and makes the mods' present flush
    # with the RAM guard armed. It runs the re-extract body against gameplay
    # RAM at headless speed with no display. FAIL on a divergence or any
    # `GUEST RAM WRITTEN` line; VACUOUS at zero re-extracts, view opens, view
    # restores, staged spans, armed presents or armed windows. Negative controls:
    # OPENPETE_SUBTICK_FAULT_STORE=1 (a store inside the view) and
    # OPENPETE_MODS_FLUSH_FAULT_STORE=1 (a store inside the flush) must fail it.
    foreach(_g spyro-1 spyro-2 spyro-3)
        openpete_game_bin(${_g} _gbin _gargs)
        add_test(NAME psx_oracle_reextract_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh --game ${_g})
        set_tests_properties(psx_oracle_reextract_${_g} PROPERTIES
            LABELS "slow;oracle;subtick"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            ENVIRONMENT "OPENPETE_BIN=${_gbin};OPENPETE_ORACLE_REEXTRACT=1")
    endforeach()

    # Letterbox edge row: Spyro 2 windowed on a private Xvfb display at
    # 641x600 AUTO (bars top and bottom) and 1280x720 --aspect 4:3 (bars
    # left and right); no lit pixel of the live present may lie outside the
    # view rect the renderer logs. Labels `slow;letterbox`: outside the
    # default run by `ctest -LE slow`, run by `ctest -L letterbox`. Exit 64
    # (skip) without xvfb-run, the binary or the disc.
    add_test(NAME letterbox_edge
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_letterbox_edge.sh
                     --out ${CMAKE_BINARY_DIR}/letterbox-edge)
    set_tests_properties(letterbox_edge PROPERTIES
        LABELS "slow;letterbox"
        RESOURCE_LOCK openpete_game
        TIMEOUT 600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}")

    # Player screenshot off the sim thread: Spyro 3 windowed on a private
    # Xvfb display in Lost Fleet, three `shot` requests with the PNG write
    # lengthened by 40 ms; on the writer thread no feed stretch or slip
    # follows a screenshot, inline (OPENPETE_SHOT_SYNC=1) one does. Labels
    # `slow;audio`. Exit 64 (skip) without xvfb-run, the binary or the disc.
    add_test(NAME screenshot_async
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_screenshot_async.sh
                     --out ${CMAKE_BINARY_DIR}/screenshot-async)
    set_tests_properties(screenshot_async PROPERTIES
        LABELS "slow;audio"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}")

    # Matrix determinism gate - broaden coverage from one (aspect,
    # scenario) cell to a sweep across aspects × fovs × scenarios.
    # Divergence in an open channel of the game's channel registry is
    # expected; in any other channel, or in a region no channel claims, it
    # is a regression.
    # Slow (boots N PSX sessions); only runs under `ctest -L slow`.
    add_test(NAME determinism_matrix
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --frames=2000)
    set_tests_properties(determinism_matrix PROPERTIES
        LABELS "slow;determinism;matrix"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    # One row per further game, at the game's default scenarios and
    # --frames (classified against that game's own manifests).
    foreach(_g spyro-2 spyro-3)
        add_test(NAME determinism_matrix_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_determinism_matrix.sh
                         --game ${_g}
                         --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(determinism_matrix_${_g} PROPERTIES
            LABELS "slow;determinism;matrix"
            RESOURCE_LOCK openpete_game
            TIMEOUT 1800
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Soak determinism gate - long-replay mode for hunting slow-burn
    # / late-frame channel leaks the 2000-frame matrix cells miss.
    # Iterates the full BK2 corpus (minus tas/frame-time-test) at
    # 4:3 vs 16:9, default fov, with --bk2-stop-on-end so each cell
    # exits on movie end. Not part of the default ctest run; opt in
    # via `ctest -L soak`. Substantially slower than matrix.
    add_test(NAME determinism_soak
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_determinism_matrix.sh
                     --build-dir=${CMAKE_BINARY_DIR}
                     --soak)
    set_tests_properties(determinism_soak PROPERTIES
        LABELS "slow;determinism;soak"
        RESOURCE_LOCK openpete_game
        TIMEOUT 7200
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Savestate restore-at-yield gate. Per corpus movie, three headless legs
    # (clean, --state-stress 1, --state-stress-rewind 25) whose
    # --gameplay-hash-log outputs must agree byte-for-byte, plus the runahead
    # stanzas: --runahead 2 must leave the COMMITTED timeline identical to
    # clean, the read-only prediction oracle must report >= 50 checks with
    # zero mismatches (a driver that never speculates would pass the compare
    # trivially), and a portal entry must still complete its level load under
    # speculation. Slow (~20 headless sessions); `ctest -L savestate` or the
    # script directly. Skips (exit 64) if the binary / disc / movies are
    # absent.
    add_test(NAME state_roundtrip
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_state_roundtrip.sh)
    set_tests_properties(state_roundtrip PROPERTIES
        LABELS "slow;determinism;savestate"
        RESOURCE_LOCK openpete_game
        # Each leg self-limits at `timeout 600`; headroom for ~20 of them.
        TIMEOUT 7200
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The savestate deferral row at an emitted polling-loop delivery: Spyro 2
    # formats an unformatted card (games/spyro-2/bk2/s2-format-card.bk2),
    # whose card-status waits take their VBLs at the loops' back edges. A
    # save and a load armed inside the wait must be deferred to the next
    # final yield (a nonzero count, at least one attributed to a loop header)
    # and leave the hash log identical to clean; the state is then loaded by
    # the saving run and by a fresh process. Six headless runs, ~50 s.
    add_test(NAME state_roundtrip_loop_deferral
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_state_roundtrip.sh
                     --game spyro-2 --row s2-format-card.bk2)
    set_tests_properties(state_roundtrip_loop_deferral PROPERTIES
        LABELS "slow;determinism;savestate"
        RESOURCE_LOCK openpete_game
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Memory-card gate over the kernel's bu file device, with a formatted card
    # in port A and port B empty: Spyro 3 boots past its title's card probe,
    # and Spyro 2 creates, writes and reads back its save from
    # games/spyro-2/bk2/s2-create-save.bk2 (directory entry, 40 sectors
    # written and read). VACUOUS (fail) when no bu call is made; skips (exit
    # 64) without the binary, the disc or the card fixture.
    add_test(NAME card_boot
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(card_boot PROPERTIES
        LABELS "slow;savestate;card"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Spyro 2's story-step autosave on leaving Glimmer for Summer Forest:
    # s2-create-save.bk2 (New Game on the blank card) with --skip-to-cutscene
    # summer_forest_story. The loading loop's autosave waits on a flag only a
    # VBL's callback sets; pass is exit 0, VBLs delivered at its back-edge
    # (0x800194F8), the 0x600-byte write and its 12 sectors on the card.
    # VACUOUS (fail) when the exit call or the story cutscene did not run.
    add_test(NAME card_autosave
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=autosave)
    set_tests_properties(card_autosave PROPERTIES
        LABELS "slow;savestate;card"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Guest RAM mirror end to end: mods/ram-mirror-probe makes each game's
    # own moby grid relink run with a moby at a negative X (bucket store in
    # the RAM mirror) and a negative Y (bucket store above 8 MB), and checks
    # the bucket, the census engagement count and the unmapped-store
    # warning (tests/harness/check_ram_mirror.sh). Six runs measure about
    # 50 s on an idle machine; skips (exit 64) without the binary or discs.
    add_test(NAME ram_mirror_e2e
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_ram_mirror.sh)
    set_tests_properties(ram_mirror_e2e PROPERTIES
        LABELS "slow;mods"
        RESOURCE_LOCK openpete_game
        TIMEOUT 300
        SKIP_RETURN_CODE 64
        ENVIRONMENT "RAM_MIRROR_WORK=${CMAKE_BINARY_DIR}/ram-mirror"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Hang watchdog stall monitor: an inline mod that spins without a VBL
    # trips within the stall limit (Linux, and Windows when the MinGW exe is
    # built), the community-mod triple trips on Windows when
    # OPENPETE_HANG_STALL_MODS names its copies, and the attract demo and the
    # level-transition movie never trip, and the Spyro 3 portal row's first
    # sight of the Crystal Islands picture at midnight_mountain under
    # --lod extended holds the game fiber under 500 ms. VACUOUS (fail) when
    # the monitor took no sample; skips (exit 64) when every row lacks its
    # binary or inputs.
    add_test(NAME hang_stall
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_hang_stall.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(hang_stall PROPERTIES
        LABELS "slow;watchdog"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The Spyro 2 save along s2-create-save.bk2 against the DuckStation fork's
    # capture of the same movie (games/spyro-2/oracle-card-save.conf): every
    # record's gameplay-state regions, the card image each side left, and
    # the 40 sectors of the save each side wrote. The capture is generated on
    # the first run; skips (exit 64) without DuckStation, the binary or the
    # disc.
    add_test(NAME card_save_reference
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=save-ref)
    set_tests_properties(card_save_reference PROPERTIES
        LABELS "slow;card;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The same Spyro 2 save movie along 600 records under the clock row
    # (games/spyro-2/oracle-card-save-clock.conf, its own capture): past the
    # save, the level takes a VBLANK inside the game tick that only the clock
    # row delivers where the reference took it.
    add_test(NAME card_save_reference_clock
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=save-ref-clock)
    set_tests_properties(card_save_reference_clock PROPERTIES
        LABELS "slow;card;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Spyro 2 formats an unformatted card and saves to it along
    # games/spyro-2/bk2/s2-format-card.bk2 against the fork's capture of the
    # same movie (games/spyro-2/oracle-card-format.conf, 500 records): every
    # record and the card image each side left.
    add_test(NAME card_format_reference
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=format-ref)
    set_tests_properties(card_format_reference PROPERTIES
        LABELS "slow;card;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The same format movie along 800 records under the clock row
    # (games/spyro-2/oracle-card-format-clock.conf, its own capture): from
    # record 502 the level takes a VBLANK inside the game tick that only the
    # clock row delivers where the reference took it.
    add_test(NAME card_format_reference_clock
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=format-ref-clock)
    set_tests_properties(card_format_reference_clock PROPERTIES
        LABELS "slow;card;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The Spyro 2 save movie on a full card (15 foreign one-block saves,
    # tests/harness/make_full_card.py) along 520 records under the clock row
    # (games/spyro-2/oracle-card-full-save-clock.conf, its own capture): every
    # record, the card image each side left, the foreign saves kept, no save
    # created, and the same block-0 sectors written on each side.
    add_test(NAME card_full_save_reference_clock
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=full-save-ref-clock)
    set_tests_properties(card_full_save_reference_clock PROPERTIES
        LABELS "slow;card;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The Spyro 3 save along games/spyro-3/bk2/s3-create-save.bk2 against the
    # fork's capture of the same movie (games/spyro-3/oracle-card-save.conf):
    # every record, the card image each side left, and the 59 sectors of the
    # save each side wrote.
    add_test(NAME card_save_reference_s3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=save-ref --game=spyro-3)
    set_tests_properties(card_save_reference_s3 PROPERTIES
        LABELS "slow;card;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2400
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The save loaded from the title against the fork's capture of the same
    # movie (games/<game>/oracle-card-load.conf, games/<game>/bk2/s2-load-save.bk2
    # or s3-load-save.bk2), both sides booting the card the save capture
    # left: every record, the card image each side left, and each
    # asynchronous read of the save (fd, one lseek per read, the file span's
    # sectors and bytes, the reference's sectors and cadence).
    add_test(NAME card_load_reference
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=load-ref --game=spyro-2)
    add_test(NAME card_load_reference_s3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=load-ref --game=spyro-3)
    # Spyro 2's load into the level, 1300 records under the clock row
    # (games/spyro-2/oracle-card-load-clock.conf): the level takes VBLANKs
    # inside the game tick that only the clock row delivers where the
    # reference took them; the stock row's 330 records end before it.
    add_test(NAME card_load_reference_clock
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_card_boot.sh
                     --build-dir=${CMAKE_BINARY_DIR} --row=load-ref-clock)
    set_tests_properties(card_load_reference card_load_reference_s3 card_load_reference_clock PROPERTIES
        LABELS "slow;card;oracle"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The card-present oracle goldens over the 20k attract demo: slot 1 holds
    # the blank formatted card (games/<game>/oracle-card.conf) or the card
    # holding the game's save (games/<game>/oracle-card-with-save.conf, whose
    # first capture also captures the save reference that leaves the card);
    # slot 2 is empty.
    foreach(_g spyro-2 spyro-3)
        openpete_game_bin(${_g} _gbin _gargs)
        add_test(NAME psx_oracle_card_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh
                         --game ${_g} --recipe games/${_g}/oracle-card.conf)
        add_test(NAME psx_oracle_card_with_save_${_g}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_psx_oracle.sh
                         --game ${_g} --recipe games/${_g}/oracle-card-with-save.conf)
        set_tests_properties(psx_oracle_card_${_g} psx_oracle_card_with_save_${_g} PROPERTIES
            LABELS "slow;card;oracle"
            RESOURCE_LOCK openpete_game
            TIMEOUT 3600
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            ENVIRONMENT "OPENPETE_BIN=${_gbin}")
    endforeach()

    # Sub-tick present gameplay-state gate. The psx oracle replays canonical
    # ticks only, so an intermediate present that writes gameplay-readable RAM
    # and mis-restores it is invisible to it. Per movie, two headless legs of
    # the same binary - intermediate re-extracts armed vs not - whose
    # gameplay-hash logs must be identical, with a per-present bracket check
    # that names the region a leak came from, and the canonical leg compared
    # against the blessed baseline under tests/state-baselines/. Refuses to
    # pass at zero sub-tick presents (exit 3, VACUOUS). Runs the `quick`
    # corpus here; `--corpus full` is the complete sweep. Skips (exit 64) if
    # the binary or disc are absent.
    add_test(NAME subtick_state
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_subtick_state.py)
    set_tests_properties(subtick_state PROPERTIES
        LABELS "slow;determinism"
        RESOURCE_LOCK openpete_game
        # Four headless legs, each self-limited at --max-runtime-seconds 280.
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The same gate over the `full` corpus: the `quick` pair plus the
    # skip-to-level vista and distant-terrain movies, whose baselines no
    # `quick` run compares. Labelled apart from `subtick_state` so a run
    # excluding `slow` skips it and an arc-end run selects it with
    # `-L subtick_full`.
    add_test(NAME subtick_state_full
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_subtick_state.py
                     --corpus full)
    set_tests_properties(subtick_state_full PROPERTIES
        LABELS "slow;subtick_full;determinism"
        RESOURCE_LOCK openpete_game
        # Twelve headless legs, each self-limited at --max-runtime-seconds 280.
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        ENVIRONMENT "OPENPETE_BIN=${_spyro1_bin}"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Retained-terrain GPU cache vs CPU terrain walk, headless on llvmpipe.
    # Per measured freecam row, two legs of one build at the same pose and
    # pinned VBL differing only in the feed (--terrain-cache 0 against the
    # cache forced on); the fp32 cache differs from the double walk by its
    # position rounding, so the pass criterion is the row's differing pixels
    # at most its cap in tests/harness/cache_walk_ab.caps (a ratchet), with
    # the edge/face split from two face-identity legs reported on a row that
    # differs. Refuses a verdict unless the cache leg reports retained
    # sectors and the walk leg reports none and reports terrain triangles.
    # Skips (exit 64) without a binary, a disc, PIL, or measured rows for
    # the game.
    add_test(NAME cache_walk_ab_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_cache_walk_ab.sh
                     --game spyro-1 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(cache_walk_ab_spyro-1 PROPERTIES
        LABELS "slow;presentation;terrain"
        RESOURCE_LOCK openpete_game
        # Up to four headless boots per row to a post-load VBL, each
        # self-limited at --max-runtime-seconds 840.
        TIMEOUT 7200
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Retained terrain cache state against a fresh bake of the current guest
    # data (OPENPETE_TERRAIN_AUDIT), headless on llvmpipe: Spyro 2 Summer
    # Forest to VBL 2000 with the cache forced on, a row whose sectors both
    # re-bake and move through the position ring (the attract demo's first
    # level rewrites its ring entries with unchanged positions). Passes on audits, and
    # re-baked plus re-rolled (position ring) sector audits, above 0 with 0
    # mismatches; on the poison control (one baked corner of the sampled
    # sector offset before upload) naming that sector and field; and on the
    # ring control (ring entries after a ring's first dropped) reporting ring
    # mismatches. Skips (exit 64) without a binary or disc.
    add_test(NAME terrain_audit_spyro-2
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_audit.sh
                     --game spyro-2 --level summer_forest --frames 2000
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_audit_spyro-2 PROPERTIES
        LABELS "slow;terrain;spyro-2"
        RESOURCE_LOCK openpete_game
        # Up to three sequential headless boots to VBL 2000.
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The same audit on levels whose env-anim colour sectors are also
    # poly-anim (vertices on the position ring, colours on the colour table),
    # each from --skip-to-level to VBL 2000. Such a sector re-bakes on no
    # roll, so every vertex its plain chunks hold must read its colour from
    # the table; a baked colour there stays at the bake's value and the clean
    # leg counts it. Spyro 1 Peace Keepers re-bakes only (no Spyro 1 sector
    # moves its ring); the Spyro 3 rows both re-bake and re-roll. Spyro 3
    # Country Speedway also holds HP triangles whose unused slot-3 colour
    # index points past the sector record at bytes rewritten every tick; the
    # bake keeps no such word.
    foreach(_row spyro-1:peace_keepers spyro-2:aquaria_towers spyro-2:robotica_farms
                 spyro-3:seashell_shore spyro-3:evening_lake spyro-3:starfish_reef
                 spyro-3:country_speedway)
        string(REPLACE ":" ";" _rl "${_row}")
        list(GET _rl 0 _g)
        list(GET _rl 1 _lvl)
        add_test(NAME terrain_audit_${_g}_${_lvl}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_audit.sh
                         --game ${_g} --level ${_lvl} --frames 2000
                         --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(terrain_audit_${_g}_${_lvl} PROPERTIES
            LABELS "slow;terrain;${_g}"
            RESOURCE_LOCK openpete_game
            # Up to three sequential headless boots to VBL 2000.
            TIMEOUT 1800
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()

    # Terrain colour-anim `mix` block bounds: one headless row on Spyro 2 and 3 with
    # the block flush against an inaccessible page and the past-bank face
    # counter. Exit 2 is VACUOUS (the counter at 0) and fails the test. Skips
    # (exit 64) without a binary or a disc.
    add_test(NAME canim_bounds
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_canim_bounds.sh
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(canim_bounds PROPERTIES
        LABELS "slow;terrain"
        RESOURCE_LOCK openpete_game
        TIMEOUT 2700
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Retained-terrain claim census: per row of
    # games/<game>/corpus/terrain-claims.manifest, one headless leg per LOD
    # tier with the cache forced on under software vk; the last canonical
    # walk at the capture VBL must claim 0 sectors in every class (arena
    # included). Exit 2 is VACUOUS (a leg's walk saw no sector or wrote no
    # census line) and fails the test. Skips (exit 64) without a binary, a
    # disc or a Vulkan loader. The legs run CORPUS_JOBS at a time
    # (scripts/corpus-jobs.sh), each one --skip-to-level boot to the capture
    # VBL. The rows hold the lock: each runs a full machine's worth of
    # legs, and all of them write one default output directory.
    foreach(_tc_game IN ITEMS spyro-1 spyro-2 spyro-3)
        add_test(NAME terrain_claims_${_tc_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_claims.sh
                         --game ${_tc_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(terrain_claims_${_tc_game} PROPERTIES
            LABELS "slow;terrain;${_tc_game}"
            RESOURCE_LOCK openpete_game
            # Spyro 2 holds 21 rows (42 legs), Spyro 3 20, Spyro 1 2; each
            # leg self-limited at --max-runtime-seconds 580.
            TIMEOUT 18000
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    # Its negative control: OPENPETE_TERRAIN_CANIMNEAR=0 on a row with
    # colour-anim near sectors must read claims > 0, else the census is blind.
    add_test(NAME terrain_claims_calibrate
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_claims.sh
                     --game spyro-3 --filter lost_fleet:1 --lod extended --calibrate
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_claims_calibrate PROPERTIES
        LABELS "slow;terrain;spyro-3"
        RESOURCE_LOCK openpete_game
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # Moby vertex stage readback: per row of
    # games/<game>/corpus/moby-gstage.manifest, per LOD tier, the canonical
    # present and the sub-tick re-extract at phase 0.5, the GPU stage
    # (core/rhi/shaders/moby_stage.glsl) against the CPU projection of the
    # same present's vertices on software vk; every class engaged by the
    # negative control or carved out. Exit 2 is VACUOUS and fails the test.
    # Skips (exit 64) without a binary, a disc or numpy.
    foreach(_gs_game IN ITEMS spyro-1 spyro-2 spyro-3)
        add_test(NAME moby_gstage_ab_${_gs_game}
                 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_gstage_ab.sh
                         --game ${_gs_game} --build-dir=${CMAKE_BINARY_DIR})
        set_tests_properties(moby_gstage_ab_${_gs_game} PROPERTIES
            LABELS "renderer;moby;${_gs_game}"
            RESOURCE_LOCK openpete_game
            # Up to four rows of four legs, each self-limited at
            # --max-runtime-seconds 840.
            TIMEOUT 5400
            SKIP_RETURN_CODE 64
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    endforeach()
    # Its negative control: the stage arm drawn with the control's 1/64 px
    # offset must fail every leg.
    add_test(NAME moby_gstage_ab_negctl
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_moby_gstage_ab.sh
                     --game spyro-2 --filter icy_speedway --negctl
                     --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(moby_gstage_ab_negctl PROPERTIES
        LABELS "renderer;moby;spyro-2"
        RESOURCE_LOCK openpete_game
        TIMEOUT 3600
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # CPU terrain walk vs retained terrain cache, per-pose residual against a
    # committed baseline. Per row of games/spyro-1/corpus/walk-cache.manifest, two
    # headless legs at the freecam corpus's canonical VBL (the walk, and the
    # cache forced on under software vk); the count of pixels whose RGB differ
    # must equal the row of tests/golden/spyro-1/walk-cache-baseline.tsv, in
    # either direction. VACUOUS when a capture is missing, the walk leg draws
    # no terrain triangle or the cache leg retains no sector. Skips (exit 64)
    # without a binary, a disc, a Vulkan loader or PIL.
    add_test(NAME terrain_walk_cache_spyro-1
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_walk_cache.sh
                     --game spyro-1 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_walk_cache_spyro-1 PROPERTIES
        LABELS "presentation;terrain;spyro-1"
        # Three rows of two concurrent boots, each self-limited at
        # --max-runtime-seconds 580.
        TIMEOUT 1800
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

    # The same check on Spyro 2, whose row is a bk2 row
    # (games/spyro-2/corpus/walk-cache.manifest): the plain boot's attract demo at
    # one VBL on the deterministic sub-tick present, walk against cache.
    add_test(NAME terrain_walk_cache_spyro-2
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_walk_cache.sh
                     --game spyro-2 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_walk_cache_spyro-2 PROPERTIES
        LABELS "presentation;terrain;spyro-2"
        # One row of two concurrent boots, each self-limited at
        # --max-runtime-seconds 580.
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME terrain_walk_cache_spyro-3
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/harness/check_terrain_walk_cache.sh
                     --game spyro-3 --build-dir=${CMAKE_BINARY_DIR})
    set_tests_properties(terrain_walk_cache_spyro-3 PROPERTIES
        LABELS "presentation;terrain;spyro-3"
        # One row of two concurrent boots, self-limited at
        # --max-runtime-seconds 580.
        TIMEOUT 900
        SKIP_RETURN_CODE 64
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# Error set: GCC diagnostics that mark a defect class (an undeclared call
# compiled against an int-returning guess, int/pointer conversions, a
# missing return, a format/argument mismatch) fail the build. Added to
# every compiling target defined outside third_party/, generated module
# code included; the targets third_party/ defines keep their own flags.
# GCC only: the clang, MSVC and wasm presets keep theirs. g++ rejects the
# five C-only members. Truncation stays a warning, since -Werror=format
# alone also makes -Wformat-truncation an error.
set(OPENPETE_ERROR_SET_COMMON
    -Werror=return-type -Werror=int-to-pointer-cast
    -Werror=builtin-declaration-mismatch -Werror=format
    -Wno-error=format-truncation -Werror=return-local-addr
    -Werror=shift-count-overflow -Werror=shift-count-negative
    -Werror=sizeof-pointer-memaccess -Werror=nonnull -Werror=address
    -Werror=memset-transposed-args -Werror=div-by-zero)
set(OPENPETE_ERROR_SET_C_ONLY
    -Werror=implicit-function-declaration -Werror=implicit-int
    -Werror=int-conversion -Werror=incompatible-pointer-types
    -Werror=pointer-to-int-cast)

# Appends the compiling targets defined in dir and its subdirectories to
# the list named by out_var; directories under third_party/ are skipped.
function(openpete_collect_project_targets dir out_var)
    set(_found ${${out_var}})
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_target_property(_type ${_t} TYPE)
        if(_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$")
            list(APPEND _found ${_t})
        endif()
    endforeach()
    get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(_sub IN LISTS _subdirs)
        file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_sub}")
        if(NOT _rel MATCHES "^third_party(/|$)")
            openpete_collect_project_targets("${_sub}" _found)
        endif()
    endforeach()
    set(${out_var} ${_found} PARENT_SCOPE)
endfunction()

# Adds the error set to every project target, prints the target count, and
# fails the configure at zero.
function(openpete_apply_error_set)
    set(_targets "")
    openpete_collect_project_targets("${CMAKE_SOURCE_DIR}" _targets)
    list(LENGTH _targets _count)
    if(_count EQUAL 0)
        message(FATAL_ERROR "error set: no project target found")
    endif()
    foreach(_t IN LISTS _targets)
        target_compile_options(${_t} PRIVATE
            "$<$<C_COMPILER_ID:GNU>:${OPENPETE_ERROR_SET_COMMON}>"
            "$<$<AND:$<C_COMPILER_ID:GNU>,$<COMPILE_LANGUAGE:C>>:${OPENPETE_ERROR_SET_C_ONLY}>")
    endforeach()
    message(STATUS "error set: ${_count} project targets")
endfunction()
openpete_apply_error_set()

# Build-output warnings: plain -Wall -Wextra on every source of a project
# target that lies in the warning ratchet's scope (core/, games/, tests/,
# outside generated/ directories, the decomp checkouts and the build tree),
# set per source so vendored sources compiled into project targets (miniz,
# toml, libco, volk, imgui) and generated sources never take it. The
# property is set in the scope of each target's own directory
# (TARGET_DIRECTORY); a top-level call without it reaches no target defined
# in a subdirectory. No -Wno- for the classes not yet cleared: the build log
# shows every warning, and scripts/fences/warning_ratchet.py decides which
# may not grow. Prints the source count and fails the configure at zero.
function(openpete_apply_build_warnings)
    set(_targets "")
    openpete_collect_project_targets("${CMAKE_SOURCE_DIR}" _targets)
    set(_seen "")
    set(_count 0)
    foreach(_t IN LISTS _targets)
        get_target_property(_srcs ${_t} SOURCES)
        get_target_property(_dir ${_t} SOURCE_DIR)
        if(NOT _srcs)
            continue()
        endif()
        foreach(_s IN LISTS _srcs)
            if(_s MATCHES "\\$<" OR NOT _s MATCHES "\\.(c|cc|cpp|cxx)$")
                continue()
            endif()
            get_filename_component(_abs "${_s}" ABSOLUTE BASE_DIR "${_dir}")
            file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_abs}")
            string(FIND "${_abs}/" "${CMAKE_BINARY_DIR}/" _in_build)
            if(NOT _in_build EQUAL -1
               OR NOT _rel MATCHES "^(core|games|tests)/"
               OR _rel MATCHES "(^|/)(generated|third_party|build)/"
               OR _rel MATCHES "^games/(spyro-[123])/spyro-[123]/")
                continue()
            endif()
            # One append per (directory scope, source): two targets of one
            # directory share the source's property.
            set(_id "${_dir}|${_abs}")
            if(_id IN_LIST _seen)
                continue()
            endif()
            list(APPEND _seen "${_id}")
            set_property(SOURCE "${_abs}" TARGET_DIRECTORY ${_t} APPEND PROPERTY
                COMPILE_OPTIONS "$<$<C_COMPILER_ID:GNU>:-Wall>"
                                "$<$<C_COMPILER_ID:GNU>:-Wextra>")
            math(EXPR _count "${_count} + 1")
        endforeach()
    endforeach()
    if(_count EQUAL 0)
        message(FATAL_ERROR "build warnings: no in-scope source found")
    endif()
    message(STATUS "build warnings: -Wall -Wextra on ${_count} in-scope sources")
endfunction()
openpete_apply_build_warnings()

# Warning ratchet (scripts/fences/warning_ratchet.py), the per-build half of
# the pre-commit fence: `check --armed` fails the build of an unarmed clone,
# and `check` measures the whole corpus from its result cache against HEAD's
# baselines/warnings/<database>.tsv in the two baselined build directories
# (linux-release, windows-mingw; elsewhere it SKIPs). It runs after every
# compiling target, the generated headers and the staged mod SDK, which its
# compiles read.
function(openpete_add_warning_ratchet)
    if(CMAKE_HOST_WIN32)
        return()
    endif()
    set(_targets "")
    openpete_collect_project_targets("${CMAKE_SOURCE_DIR}" _targets)
    get_filename_component(_name "${CMAKE_BINARY_DIR}" NAME)
    set(_check "")
    if(_name STREQUAL "linux-release" OR _name STREQUAL "windows-mingw")
        set(_check
            COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/warning_ratchet.py check
                    --database-path ${CMAKE_BINARY_DIR}/compile_commands.json)
    endif()
    add_custom_target(openpete_warning_ratchet ALL
        COMMAND ${CMAKE_COMMAND} -E echo "Checking the warning ratchet..."
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/warning_ratchet.py check --armed
        ${_check}
        COMMENT "warning fence: the pre-commit hook calls it, and no -Wall -Wextra row rose"
        VERBATIM)
    add_dependencies(openpete_warning_ratchet ${_targets} openpete_generated_headers)
    if(TARGET openpete_sdk_stage)
        add_dependencies(openpete_warning_ratchet openpete_sdk_stage)
    endif()
endfunction()
openpete_add_warning_ratchet()

# Every Ninja build holds its build directory against gates for its whole
# run; deferred to the end of this file so every target exists.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/openpete_build_lock.cmake)
cmake_language(DEFER CALL openpete_build_lock_all)
