# cmake/openpete_game.cmake - one game's targets from its directory, and the
# executables that link the games.
#
# openpete_add_game(<id>
#     [FEATURE_LIST <file>]           # the feature list; default none
#     [ORCHESTRATOR <file>]           # sub-tick re-extract orchestrator; default none
#     [INTERP_REGIONS <file>]         # interp ring region table; default none
#     [TERRAIN_GAME <file>]           # the terrain feeder's per-game data; default none
#     [MOBY_GAME <file>]              # the moby feeder's per-game decoders and data;
#                                     # default none
#     [PLAYER_GAME <file>]            # the player feeder's per-game decoders and data;
#                                     # default none
#     [MODULE_LINKAGE static|shared]  # default OPENPETE_RECOMP_LINKAGE
#     [FIRST_BOOT_SOURCES <file>...]  # the first-boot module build (shared
#                                     # linkage under OPENPETE_RECOMP_FIRST_BOOT);
#                                     # default none: a missing module is refused
#     [FIRST_BOOT_EXTRACT <symbol>]   # with FIRST_BOOT_SOURCES: the carve of the
#                                     # pipeline's inputs (game_build.h), compiled
#                                     # with the pipeline
#     [EXTRACT <symbol>]              # in place of FIRST_BOOT_EXTRACT: the carve,
#                                     # defined in SOURCES, so every build of the
#                                     # game answers --extract-recomp-inputs
#     [SYMBOLS_MANIFEST <file>]       # curated alias table -> gen/<tag>_symbols.gen.h
#     [MOD_SDK]                       # generate and stage the game's mod SDK
#                                     # (cmake/openpete_mod_sdk.cmake)
#     [SOURCES <file>...]             # host TUs beyond the skeleton
#     [MODULE_SOURCES <file>...])     # module TUs beyond the generated code
#
# Called from games/<id>/CMakeLists.txt; a game whose generated/ holds no
# recompiled output stops the configure with FATAL_ERROR. <id> is the
# directory name and the game's config-scope id ("spyro-2"); the target
# names drop its hyphen: module `<tag>_recomp_module`, host library
# `<tag>_host`, game target `openpete-<tag>`. After the call the caller's
# scope holds OPENPETE_GAME_TAG, OPENPETE_GAME_EXE (the game target: the
# per-game executable, or with the one executable the target that builds it
# and stages the game's files beside it), OPENPETE_GAME_HOST (the target that
# compiles the game's sources), OPENPETE_GAME_EXE_DIR (the directory of the
# executable that runs the game, where its module, SDK and assets stage),
# OPENPETE_GAME_BIN (that executable's path) with OPENPETE_GAME_BIN_ARGS (the
# arguments selecting the game in it), OPENPETE_GAME_MODULE,
# OPENPETE_GAME_GENERATED and OPENPETE_GAME_GEN_DIR (the build-tree gen/
# directory), for the sources a game adds on top. openpete_game_bin() returns
# the executable's path and arguments from any directory after the call.
#
# A game supplies games/<id>/recomp.conf and game_extras_impl.c; everything
# else is shared. game_extras_impl.c defines the game's runtime descriptor
# (core/include/openpete_game_runtime.h) as `<tag>_game_runtime`; a slot file
# above is reached only through that descriptor, which names no table for an
# omitted slot. The descriptor's `psyq` member is the game's
# psyq_game_hooks_t (games/common/psyq/psyq_game_hooks.h), which names the
# generated `<tag>_psyq_conf` (game_identity.c) and `<tag>_psyq_stub_table`
# (gen/psyq_stub_table.c). The game's build record `<tag>_game_build`
# (games/common/game_build.h) is generated from games/common/game_build.c.in
# into gen/: the entry, the module name and host->module forwarders, the
# first-boot pipeline, and the registration of the tables generated from the
# game's manifests.
#
#  - The module: the recompiled main EXE plus every overlay translated by
#    scripts/regen-recomp.sh. Each overlay is recompiled with a unique
#    --func-prefix and its `psx_dispatch_compiled` is sed-renamed to
#    `psx_dispatch_<name>` so they coexist at link; games/common/
#    overlay_dispatch.c fans calls out to the active overlay through the
#    generated spyro_overlay_table.c, and module_main.c is the descriptor
#    the host resolves.
#  - The host library <tag>_host: the game's own sources and the TUs
#    generated for it (identity, build record, PSYQ stub address table, mod
#    SDK tables). An OBJECT library in the game's directory, so the
#    directory's per-source settings apply to its sources.
#  - The shared host code: openpete_game_common (games/common/CMakeLists.txt)
#    and one object library per further games/common source a game names
#    (_openpete_common_object), each compiled once whatever the number of
#    games linking it, and the compile-once abort-on-call PSYQ stubs with
#    their runtime (openpete_psyq_stubs, top-level CMakeLists.txt).
#
# Invariant: no host TU resolves a game-local header through its include
# path. A host TU's include directories are games/common, core's, and the
# two game roots (games/ in the source and build trees), under which a game's
# header is named by its game-qualified path ("spyro-1/gen/x.h"); a game
# source names a header of its own directory or below relative to itself.
# tools/check_game_includes.py refuses the build otherwise
# (openpete_game_include_check).
#
# The executables. With shared linkage on Linux and MinGW every game links
# into one executable, `openpete` (`openpete.exe`) at the build root
# (openpete_link_games, called by the top-level CMakeLists.txt after the
# games), whose entry selects the game (games/common/game_build.h). Every
# game's module, SDK, assets and first-boot inputs stage beside it at the
# build root; each game's target `openpete-<tag>` builds the executable and
# the game's files and writes no executable of its own. Elsewhere (static
# linkage, and MSVC) each game has its own executable
# <build>/games/<id>/openpete-<tag> linking its host library, the shared host
# code and, under static linkage, its module, with the game's files beside
# it. The configure deletes the executable and module files the tree no
# longer builds (the root `openpete`, or a game directory's executable,
# module and SDK), so no harness runs a stale one.
#
# Shared linkage builds the module as <tag>-recomp.{so,dll} beside the
# executable, which loads it at boot (games/common/recomp_attach_shared.c)
# and depends on it, so a built executable always has its module. The
# executable's export surface is generated from the game's psyq.toml and the
# module ABI lists (tools/gen_module_exports.py): a PE .def plus an implib
# the module links against on Windows, and on every platform a keep-alive TU
# that pulls the ABI's archive members into the executable. The one
# executable on MinGW exports the union of the linked games' .def files
# (tools/merge_module_defs.py) and every module links the one implib made
# from that union. The host->module forwarders are the game's
# module_imports.h, defined in its build record. On ELF the target
# <tag>_module_imports refuses the build unless every module import resolves
# to exactly one definition in the merged image
# (tools/check_module_imports.py); on PE the module's link against the
# implib is that refusal, and the executable's link refuses a .def export it
# does not define.

include(${CMAKE_SOURCE_DIR}/cmake/openpete_mod_sdk.cmake)

# One executable links every game: shared linkage, on Linux and MinGW (MSVC
# links one static executable per game). Set in every directory that
# includes this file.
if(OPENPETE_RECOMP_LINKAGE STREQUAL "shared"
   AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR MINGW))
    set(OPENPETE_ONE_EXECUTABLE ON)
else()
    set(OPENPETE_ONE_EXECUTABLE OFF)
endif()

# The compile settings of every host object library: the game-neutral
# include path (games/common, core's usage requirements, and the two game
# roots under which a game's headers are named by game-qualified paths),
# the language standards, and -fno-strict-aliasing (the host reads guest RAM,
# uint8_t arrays, through wider pointer casts; the first-boot module build,
# games/common/recomp_build.cpp, passes the same flag).
function(openpete_game_common_settings TGT)
    target_include_directories(${TGT} PRIVATE
        ${CMAKE_SOURCE_DIR}/games/common
        ${CMAKE_SOURCE_DIR}/games
        ${CMAKE_BINARY_DIR}/games)
    target_link_libraries(${TGT} PRIVATE openpete_core)
    target_compile_options(${TGT} PRIVATE -fno-strict-aliasing)
    set_target_properties(${TGT} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
endfunction()

# The object library compiling one games/common source once, created at its
# first use: `opc_<path under games/common, / and . as _>`. OUT receives the
# target name.
function(_openpete_common_object SRC OUT)
    file(RELATIVE_PATH _rel ${CMAKE_SOURCE_DIR}/games/common ${SRC})
    string(REGEX REPLACE "[/.]" "_" _name "opc_${_rel}")
    if(NOT TARGET ${_name})
        add_library(${_name} OBJECT ${SRC})
        openpete_game_common_settings(${_name})
    endif()
    set(${OUT} ${_name} PARENT_SCOPE)
endfunction()

# Split SOURCES into the game's own (relative, or under its directory) and
# games/common sources. OWN receives the former; COMMON_LIBS the object
# libraries compiling the latter.
function(_openpete_game_partition OWN COMMON_LIBS)
    set(_own "")
    set(_libs "")
    foreach(_s IN LISTS ARGN)
        get_filename_component(_abs ${_s} ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
        if(_abs MATCHES "^${CMAKE_SOURCE_DIR}/games/common/")
            _openpete_common_object(${_abs} _lib)
            list(APPEND _libs ${_lib})
        else()
            list(APPEND _own ${_s})
        endif()
    endforeach()
    set(${OWN} ${_own} PARENT_SCOPE)
    set(${COMMON_LIBS} ${_libs} PARENT_SCOPE)
endfunction()

# The C file listing the games an executable links (`openpete_game_builds`,
# games/common/game_build.h), written at configure time to OUT.
function(_openpete_game_list OUT)
    set(_decl "")
    set(_rows "")
    foreach(_t IN LISTS ARGN)
        string(APPEND _decl "extern const openpete_game_build_t ${_t}_game_build;\n")
        string(APPEND _rows "    &${_t}_game_build,\n")
    endforeach()
    list(LENGTH ARGN _n)
    file(CONFIGURE OUTPUT ${OUT} CONTENT
"/* Generated by cmake/openpete_game.cmake - the games this executable links,
 * in OPENPETE_GAMES order (games/common/game_build.h). */
#include \"game_build.h\"

${_decl}
const openpete_game_build_t* const openpete_game_builds[] = {
${_rows}};
const size_t openpete_game_build_count = ${_n};
")
endfunction()

# The link settings of an executable linking the games' host objects: core,
# the PSYQ stubs, the SDL main shim, and on Linux -rdynamic (non-static
# functions resolve to names in the crash handler's backtrace, and a shared
# module resolves its host imports against these exports).
function(_openpete_game_exe_link EXE)
    target_link_libraries(${EXE} PRIVATE openpete_core openpete_psyq_stubs)
    if(TARGET SDL3::SDL3main)
        target_link_libraries(${EXE} PRIVATE SDL3::SDL3main)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(${EXE} PRIVATE -rdynamic)
    endif()
    if(MINGW)
        # A distribution build links the GUI subsystem, so no console window
        # opens for a player (main.cpp reattaches stdout/stderr to the parent
        # console when launched from a terminal); a dev build links the
        # console subsystem, so stdout/stderr reach the parent terminal (WSL2
        # interop included).
        if(OPENPETE_DIST_PRESET)
            target_link_options(${EXE} PRIVATE -mwindows)
        else()
            target_link_options(${EXE} PRIVATE -mconsole)
        endif()
    endif()
    set_target_properties(${EXE} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON)
endfunction()

# The shared-linkage wiring between one game's executable and module.
#
# PE: the executable exports exactly the .def surface, and the module links
# an implib generated from the same .def, so every module import names the
# executable; a module import missing from the .def fails the module link.
# GNU LTO does not treat .def exports as liveness roots (with IPO the
# otherwise-unreferenced ABI surface is dead-code-eliminated and ld emits
# the exports as absolute 0), so IPO is forced off for the executable.
# IMPLIB_TARGET names the target that writes the implib,
# <current binary dir>/gen/libopenpete-exe-exports.a; every module in ARGN
# links it.
function(_openpete_game_shared_link_pe IMPLIB_TARGET EXE MODULE_DEF)
    target_link_options(${EXE} PRIVATE ${MODULE_DEF})
    set_property(TARGET ${EXE} APPEND PROPERTY LINK_DEPENDS ${MODULE_DEF})
    set_target_properties(${EXE} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    if(NOT CMAKE_DLLTOOL)
        set(CMAKE_DLLTOOL x86_64-w64-mingw32-dlltool)
    endif()
    set(_implib ${CMAKE_CURRENT_BINARY_DIR}/gen/libopenpete-exe-exports.a)
    add_custom_command(
        OUTPUT  ${_implib}
        COMMAND ${CMAKE_DLLTOOL} -d ${MODULE_DEF} -l ${_implib} -D ${EXE}.exe
        DEPENDS ${MODULE_DEF}
        COMMENT "dlltool: ${EXE} export implib for the recomp modules")
    add_custom_target(${IMPLIB_TARGET} DEPENDS ${_implib})
    foreach(_m IN LISTS ARGN)
        add_dependencies(${_m} ${IMPLIB_TARGET})
        target_link_libraries(${_m} PRIVATE ${_implib})
    endforeach()
endfunction()

# The one executable's export surface on PE: the union of the linked games'
# .def files, each generated in its game's directory by a target listed in
# DEF_TARGETS, written to OUT_DEF by tools/merge_module_defs.py, which
# refuses a name one .def exports as DATA and another as code.
function(_openpete_merge_module_defs OUT_DEF DEFS DEF_TARGETS)
    add_custom_command(
        OUTPUT  ${OUT_DEF}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/gen
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/merge_module_defs.py
                --image openpete.exe --out ${OUT_DEF} ${DEFS}
        DEPENDS ${DEFS} ${CMAKE_SOURCE_DIR}/tools/merge_module_defs.py
        COMMENT "Merging the recomp-module export surfaces into openpete.exe's .def"
        VERBATIM)
    add_custom_target(openpete_module_def DEPENDS ${OUT_DEF})
    add_dependencies(openpete_module_def ${DEF_TARGETS})
endfunction()

# The Win32 resources (icon, application manifest, VERSIONINFO) of the
# executable TARGET, whose file name is IMAGE.exe: games/common/openpete.rc.in
# configured with the OpenPete icon (tools/gen_dist_icon.py, original art
# with no game bytes, on every preset), games/common/openpete.manifest and
# the version core/CMakeLists.txt derives (OPENPETE_VERSION_NUM /
# OPENPETE_VERSION_STR). The .ico is written at configure time, so the
# resource has no custom-command dependency across directories; the
# generator is a configure dependency. Called for the one executable by
# openpete_link_games and for a per-game executable by openpete_add_game.
function(_openpete_exe_win32_resource TARGET IMAGE)
    set(_tool ${CMAKE_SOURCE_DIR}/tools/gen_dist_icon.py)
    set(_rc_icon ${CMAKE_BINARY_DIR}/gen/openpete.ico)
    set(_rc_manifest ${CMAKE_SOURCE_DIR}/games/common/openpete.manifest)
    set(_rc_image ${IMAGE})
    get_property(_have_icon GLOBAL PROPERTY OPENPETE_EXE_ICON_WRITTEN)
    if(NOT _have_icon)
        file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/gen)
        execute_process(
            COMMAND python3 ${_tool} --ico ${_rc_icon}
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0)
            message(FATAL_ERROR "gen_dist_icon.py --ico failed (${_rc})")
        endif()
        set_property(GLOBAL PROPERTY OPENPETE_EXE_ICON_WRITTEN ON)
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_tool})
    set(_rc ${CMAKE_BINARY_DIR}/gen/${IMAGE}.rc)
    configure_file(${CMAKE_SOURCE_DIR}/games/common/openpete.rc.in ${_rc} @ONLY)
    set_source_files_properties(${_rc} PROPERTIES
        OBJECT_DEPENDS "${_rc_icon};${_rc_manifest}")
    target_sources(${TARGET} PRIVATE ${_rc})
endfunction()

# ELF: <tag>_module_imports runs tools/check_module_imports.py over the
# module and the executable that loads it and fails the build on an import
# with no definition or more than one, or a symbol both images define
# outside the forwarders the linked games' module_imports.h list and the
# four overlay-dispatch entry points.
function(_openpete_game_shared_link_elf TAG IMAGE MODULE STAMP)
    set(_headers "")
    set(_args "")
    foreach(_h IN LISTS ARGN)
        list(APPEND _headers ${_h})
        list(APPEND _args --forwarder-header ${_h})
    endforeach()
    add_custom_command(
        OUTPUT  ${STAMP}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_module_imports.py
                --module $<TARGET_FILE:${MODULE}>
                --exe    $<TARGET_FILE:${IMAGE}>
                ${_args}
                --forwarder spyro_set_active_overlay_by_magic
                --forwarder spyro_get_active_overlay_magic
                --forwarder spyro_overlay_find
                --forwarder spyro_overlay_set_resync
                --stamp  ${STAMP}
        DEPENDS ${MODULE} ${IMAGE} ${_headers}
                ${CMAKE_SOURCE_DIR}/tools/check_module_imports.py
        COMMENT "Checking ${TAG}-recomp's imports against ${IMAGE}"
        VERBATIM)
    add_custom_target(${TAG}_module_imports ALL DEPENDS ${STAMP})
endfunction()

# The first-boot module build's inputs, staged next to the executable as
# <exe dir>/recomp/: manifest/<id>/ (the game's converged manifest
# manifest/recomp/<serial>/, its psyq.toml and, when the regen wrote one, its
# channel_sites.txt), src/ (the module source kit) and tools/ (PSXRecomp: on
# Windows the tree's own build of it, on Linux the host build when present; on
# Windows also the exe-export implib the module links against). The
# serial is recomp.conf's EXE_FILENAME with `_` -> `-` and the dot dropped.
# The game-neutral src/ and tools/ stage once per executable directory: with
# the one executable, by the target openpete_first_boot_kit, which every
# game's target depends on, so no two targets write one file.
function(_openpete_game_stage_first_boot GAME_ID EXE EXE_DIR GENERATED SERIAL)
    set(_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/recomp/${SERIAL})
    if(NOT EXISTS ${_manifest}/manifest.toml)
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): FIRST_BOOT_SOURCES "
                            "needs the converged manifest ${_manifest} "
                            "(GAME=${GAME_ID} bash scripts/emit-recomp-manifest.sh)")
    endif()
    set(_rdir ${EXE_DIR}/recomp)
    set(_manifest_files ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml)
    if(EXISTS ${GENERATED}/channel_sites.txt)
        list(APPEND _manifest_files ${GENERATED}/channel_sites.txt)
    endif()
    add_custom_command(TARGET ${EXE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${_rdir}/manifest/${GAME_ID}
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${_manifest} ${_rdir}/manifest/${GAME_ID}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_manifest_files} ${_rdir}/manifest/${GAME_ID}
        VERBATIM)
    # The compile cost table (scripts/emit-first-boot-costs.sh) stages apart
    # from the manifest tree, which the codegen stamp hashes: a re-measured
    # table changes the progress bar, never the module's identity.
    set(_costs ${CMAKE_CURRENT_SOURCE_DIR}/manifest/first_boot_costs.tsv)
    if(EXISTS ${_costs})
        add_custom_command(TARGET ${EXE} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/costs
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_costs} ${_rdir}/costs/${GAME_ID}.tsv
            VERBATIM)
    endif()
    if(OPENPETE_ONE_EXECUTABLE)
        if(NOT TARGET openpete_first_boot_kit)
            add_custom_target(openpete_first_boot_kit ALL)
            _openpete_stage_first_boot_kit(openpete_first_boot_kit ${_rdir})
        endif()
        add_dependencies(${EXE} openpete_first_boot_kit)
    else()
        _openpete_stage_first_boot_kit(${EXE} ${_rdir})
    endif()
endfunction()

# PSXRecomp built for the target platform from third_party/psxrecomp/recompiler
# into <build>/psxrecomp, with this tree's toolchain and compile flags; OUT_VAR
# receives the executable's path. The target openpete_psxrecomp is declared
# once per build tree. BUILD_ALWAYS hands every build to the sub-build, whose
# own dependency scan rebuilds the executable after any recompiler source
# changes, so the copy the first-boot kit stages is never older than its
# sources.
function(_openpete_psxrecomp_target_build OUT_VAR)
    set(_dir ${CMAKE_BINARY_DIR}/psxrecomp)
    set(_bin ${_dir}/PSXRecomp${CMAKE_EXECUTABLE_SUFFIX})
    if(NOT TARGET openpete_psxrecomp)
        include(ExternalProject)
        set(_args
            -DCMAKE_BUILD_TYPE:STRING=Release
            -DCMAKE_C_FLAGS:STRING=${CMAKE_C_FLAGS}
            -DCMAKE_CXX_FLAGS:STRING=${CMAKE_CXX_FLAGS})
        if(CMAKE_TOOLCHAIN_FILE)
            list(APPEND _args -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE})
        else()
            list(APPEND _args
                -DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}
                -DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER})
        endif()
        ExternalProject_Add(openpete_psxrecomp
            SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/recompiler
            PREFIX ${_dir}-ep
            BINARY_DIR ${_dir}
            CMAKE_CACHE_ARGS ${_args}
            BUILD_COMMAND ${CMAKE_COMMAND} --build ${_dir} --target PSXRecomp
            BUILD_ALWAYS ON
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS ${_bin})
    endif()
    set(${OUT_VAR} ${_bin} PARENT_SCOPE)
endfunction()

# The game-neutral half of the first-boot inputs: src/ and tools/ under RDIR,
# staged after TGT builds; on Windows, PSXRecomp.exe comes from
# _openpete_psxrecomp_target_build.
function(_openpete_stage_first_boot_kit TGT RDIR)
    set(_rdir ${RDIR})
    add_custom_command(TARGET ${TGT} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/src
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/games/common/overlay_dispatch.c
            ${CMAKE_SOURCE_DIR}/games/common/module_main.c
            ${CMAKE_SOURCE_DIR}/games/common/spyro_overlay_table.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_recomp_module.h
            ${CMAKE_SOURCE_DIR}/core/include/psxrt/psx_runtime.h
            ${CMAKE_SOURCE_DIR}/core/include/psxrt/psx_runtime_internal.h
            ${_rdir}/src
        VERBATIM)
    set(_tools "")
    if(WIN32)
        _openpete_psxrecomp_target_build(_psxrecomp_bin)
        add_dependencies(${TGT} openpete_psxrecomp)
        list(APPEND _tools ${_psxrecomp_bin})
    else()
        # The host recompiler: scripts/regen-recomp.sh builds it before
        # translating, and the recomp_stamp fence refuses generated C from
        # other recompiler source.
        set(_psxrecomp_bin ${CMAKE_SOURCE_DIR}/third_party/psxrecomp/build/PSXRecomp)
        if(EXISTS ${_psxrecomp_bin})
            list(APPEND _tools ${_psxrecomp_bin})
        endif()
    endif()
    if(WIN32)
        list(APPEND _tools ${OPENPETE_GAME_MODULE_IMPLIB})
    endif()
    if(_tools)
        add_custom_command(TARGET ${TGT} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_rdir}/tools
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_tools} ${_rdir}/tools
            VERBATIM)
    endif()
endfunction()

# OpenAL on Windows is always a runtime DLL load (no static .a in the
# openal-soft prebuilt). The renamed soft_oal.dll is copied next to the .exe
# after EXE builds, so the loader resolves OpenAL32.dll without a system
# install; without it the process fails to start before main() runs.
function(_openpete_stage_openal EXE EXE_DIR)
    add_custom_command(TARGET ${EXE} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/OpenAL-mingw/x86_64-w64-mingw32/bin/OpenAL32.dll"
            "${EXE_DIR}/OpenAL32.dll"
        VERBATIM)
endfunction()

function(openpete_add_game GAME_ID)
    cmake_parse_arguments(G
        "MOD_SDK"
        "FEATURE_LIST;ORCHESTRATOR;INTERP_REGIONS;TERRAIN_GAME;MOBY_GAME;PLAYER_GAME;MODULE_LINKAGE;SYMBOLS_MANIFEST;FIRST_BOOT_EXTRACT;EXTRACT"
        "SOURCES;MODULE_SOURCES;FIRST_BOOT_SOURCES"
        ${ARGN})
    if(G_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): unknown arguments ${G_UNPARSED_ARGUMENTS}")
    endif()

    string(REPLACE "-" "" _tag "${GAME_ID}")
    set(_exe openpete-${_tag})
    set(_host ${_tag}_host)
    set(_module ${_tag}_recomp_module)
    set(_common ${CMAKE_SOURCE_DIR}/games/common)
    set(_generated ${CMAKE_CURRENT_SOURCE_DIR}/generated)
    set(_gen_dir ${CMAKE_CURRENT_BINARY_DIR}/gen)
    if(OPENPETE_ONE_EXECUTABLE)
        set(_exe_dir ${CMAKE_BINARY_DIR})
        set(_bin ${CMAKE_BINARY_DIR}/openpete${CMAKE_EXECUTABLE_SUFFIX})
        set(_bin_args --game ${GAME_ID})
    else()
        set(_exe_dir ${CMAKE_CURRENT_BINARY_DIR})
        set(_bin ${_exe_dir}/${_exe}${CMAKE_EXECUTABLE_SUFFIX})
        set(_bin_args "")
    endif()
    set_property(GLOBAL PROPERTY OPENPETE_GAME_BIN_${GAME_ID} ${_bin})
    set_property(GLOBAL PROPERTY OPENPETE_GAME_BIN_ARGS_${GAME_ID} ${_bin_args})

    # The recompiled C under generated/ is gitignored and produced only by
    # scripts/regen-recomp.sh. A game listed in OPENPETE_GAMES without it
    # stops the configure; -DOPENPETE_GAMES= configures the core alone and
    # -DOPENPETE_GAMES=<id> narrows the configure to the games that have it.
    if(NOT EXISTS ${_generated}/PSX.EXE_full.c)
        set(_other_games ${OPENPETE_GAMES})
        list(REMOVE_ITEM _other_games ${GAME_ID})
        message(FATAL_ERROR
            "${GAME_ID}: no recompiled output in ${_generated}. "
            "Run `GAME=${GAME_ID} bash scripts/regen-recomp.sh` from the source "
            "directory first; it needs PSXRecomp built under "
            "third_party/psxrecomp/build and the decomp built under "
            "games/${GAME_ID}/${GAME_ID}/build (docs/src/developer/building.md). "
            "To configure without this game: -DOPENPETE_GAMES=\"${_other_games}\" "
            "(an empty list configures the core alone).")
    endif()

    if(NOT G_MODULE_LINKAGE)
        set(G_MODULE_LINKAGE ${OPENPETE_RECOMP_LINKAGE})
    endif()
    if(NOT G_MODULE_LINKAGE MATCHES "^(static|shared)$")
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): MODULE_LINKAGE '${G_MODULE_LINKAGE}' is neither static nor shared")
    endif()
    if(OPENPETE_ONE_EXECUTABLE AND NOT G_MODULE_LINKAGE STREQUAL "shared")
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): the one executable "
                            "loads every game's module; MODULE_LINKAGE must be shared")
    endif()
    # The first-boot module build is compiled in only where it can run: a
    # shared module, a preset that ships one, a game that has one.
    set(_first_boot OFF)
    if(G_MODULE_LINKAGE STREQUAL "shared" AND OPENPETE_RECOMP_FIRST_BOOT AND G_FIRST_BOOT_SOURCES)
        set(_first_boot ON)
        if(NOT G_FIRST_BOOT_EXTRACT AND NOT G_EXTRACT)
            message(FATAL_ERROR "openpete_add_game(${GAME_ID}): FIRST_BOOT_SOURCES needs FIRST_BOOT_EXTRACT or EXTRACT")
        endif()
    endif()

    # The module attach and, for shared linkage, the executable's export
    # surface: the PE .def and the keep-alive TU.
    set(_attach_sources "")
    if(G_MODULE_LINKAGE STREQUAL "shared")
        # The PE image the module imports from.
        if(OPENPETE_ONE_EXECUTABLE)
            set(_import_image openpete.exe)
        else()
            set(_import_image ${_exe}.exe)
        endif()
        set(_module_def       ${_gen_dir}/${_tag}_module_exports.def)
        set(_module_keepalive ${_gen_dir}/${_tag}_abi_keepalive.gen.c)
        set(_abi_lists ${_common}/module_abi_exports.txt)
        if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/module_abi_exports.txt)
            list(APPEND _abi_lists ${CMAKE_CURRENT_SOURCE_DIR}/manifest/module_abi_exports.txt)
        endif()
        set(_abi_args "")
        foreach(_l IN LISTS _abi_lists)
            list(APPEND _abi_args --extras ${_l})
        endforeach()
        add_custom_command(
            OUTPUT  ${_module_def} ${_module_keepalive}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_module_exports.py
                    --psyq   ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml
                    ${_abi_args}
                    --tag    ${_tag}
                    --image  ${_import_image}
                    --def-out ${_module_def}
                    --keepalive-out ${_module_keepalive}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/manifest/psyq.toml
                    ${_abi_lists}
                    ${CMAKE_SOURCE_DIR}/tools/gen_module_exports.py
            COMMENT "Generating the ${GAME_ID} recomp-module export surface (.def + keep-alive)")
        list(APPEND _attach_sources ${_common}/recomp_attach_shared.c ${_module_keepalive})
        if(_first_boot)
            list(APPEND _attach_sources ${G_FIRST_BOOT_SOURCES})
        endif()
    else()
        list(APPEND _attach_sources ${_common}/recomp_attach_static.c)
    endif()

    # CONFIGURE_DEPENDS re-runs the GLOB so newly regenerated overlays are
    # picked up without a manual reconfigure.
    file(GLOB _overlay_full_c     CONFIGURE_DEPENDS "${_generated}/*/*.bin_full.c")
    file(GLOB _overlay_dispatch_c CONFIGURE_DEPENDS "${_generated}/*/*.bin_dispatch.c")

    set(_module_sources
        ${_generated}/PSX.EXE_full.c
        ${_generated}/PSX.EXE_dispatch.c
        ${_overlay_full_c}
        ${_overlay_dispatch_c}
        ${_generated}/spyro_overlay_table.c
        ${_common}/overlay_dispatch.c
        ${_common}/module_main.c
        ${G_MODULE_SOURCES})

    if(G_MODULE_LINKAGE STREQUAL "shared")
        add_library(${_module} SHARED ${_module_sources})
        # Route the exe-exported DATA symbols through the import table
        # (__declspec(dllimport)) instead of PE pseudo-relocation; see
        # psx_runtime_internal.h. Module TUs only; the exe defines those symbols.
        target_compile_definitions(${_module} PRIVATE OPENPETE_RECOMP_MODULE_BUILD)
        set_target_properties(${_module} PROPERTIES
            OUTPUT_NAME "${_tag}-recomp"
            PREFIX ""                       # <tag>-recomp.so, not lib*.so
            # Beside the executable that loads it (the .so is a library
            # output, the .dll a runtime one).
            LIBRARY_OUTPUT_DIRECTORY ${_exe_dir}
            RUNTIME_OUTPUT_DIRECTORY ${_exe_dir}
            POSITION_INDEPENDENT_CODE ON
            # The shipped module is compiled at first boot at plain -O2, so
            # this CMake-built .so/.dll is a dev artifact and gets no LTO
            # even when the preset turns IPO on globally.
            INTERPROCEDURAL_OPTIMIZATION OFF)
        if(NOT WIN32)
            # Bind module-internal references internally, matching PE
            # semantics (no cross-image preemption), and leave the
            # module->host imports to resolve at dlopen against the exe's
            # -rdynamic exports.
            target_link_options(${_module} PRIVATE
                -Wl,-Bsymbolic-functions
                -Wl,--unresolved-symbols=ignore-all)
        endif()
    else()
        add_library(${_module} OBJECT ${_module_sources})
        # Usage requirements only (psx_runtime.h et al.); OBJECT libraries
        # do not embed their dependencies.
        target_link_libraries(${_module} PRIVATE openpete_core)
    endif()
    # The generated C must come from the recompiler source in this tree:
    # regen-recomp.sh stamps generated/psxrecomp.id with that source's hash
    # after a full regen, and the module does not compile while the stamp
    # is missing or names other source (scripts/fences/recomp_stamp.py).
    # Runs on every build; OPENPETE_RECOMP_STAMP_SKIP="<reason>" is its escape.
    add_custom_target(${_tag}_recomp_stamp
        COMMAND python3 ${CMAKE_SOURCE_DIR}/scripts/fences/recomp_stamp.py
                check ${GAME_ID} --root ${CMAKE_SOURCE_DIR}
        COMMENT "${GAME_ID}: generated C matches the recompiler source"
        VERBATIM)
    add_dependencies(${_module} ${_tag}_recomp_stamp)
    target_include_directories(${_module} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_common}
        ${_generated}
        # Include dirs only: a shared module must not link openpete_core (a
        # second copy of core state inside the module would be wrong).
        $<TARGET_PROPERTY:openpete_core,INTERFACE_INCLUDE_DIRECTORIES>)
    # The same directories keep their system marking (SDL, imgui), which
    # the copy above drops.
    target_include_directories(${_module} SYSTEM PRIVATE
        $<TARGET_PROPERTY:openpete_core,INTERFACE_SYSTEM_INCLUDE_DIRECTORIES>)
    set_target_properties(${_module} PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON)
    # The recompiled module reads guest RAM (uint8_t arrays) through wider
    # pointer casts; -fno-strict-aliasing keeps the optimizer from
    # reordering or dropping those accesses.
    target_compile_options(${_module} PRIVATE -fno-strict-aliasing)

    # The generated translation units carry no -w: it would also silence the
    # error set the top-level CMakeLists.txt adds to this target.

    # Identity: the game_extras.h identity accessors and the address-only
    # PSYQ hooks, from recomp.conf.
    set(_identity_c ${_gen_dir}/game_identity.c)
    add_custom_command(
        OUTPUT  ${_identity_c}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_game_identity.py
                --game ${GAME_ID}
                --conf ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf
                --out  ${_identity_c}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf
                ${CMAKE_SOURCE_DIR}/tools/gen_game_identity.py
        COMMENT "Generating ${GAME_ID} game_identity.c (gen_game_identity.py)")

    # Gameplay-state region table: the manifest the oracle hashes, generated
    # into the C header the build record compiles, the flat text the
    # DuckStation capture module reads (-psxtrace-regions), and the
    # PCSX-Redux Lua mirror. A game without the manifest registers an empty
    # table and no oracle golden can verify it.
    set(_regions_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/gameplay_state_regions.toml)
    set(_regions_sources)
    set(GB_REGIONS 0)
    if(EXISTS ${_regions_manifest})
        set(GB_REGIONS 1)
        set(_regions_header ${_gen_dir}/gameplay_state_regions.gen.h)
        set(_regions_flat   ${_gen_dir}/gameplay_state_regions.flat)
        set(_regions_lua    ${_gen_dir}/openpete_state_regions.gen.lua)
        add_custom_command(
            OUTPUT  ${_regions_header} ${_regions_flat} ${_regions_lua}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_state_regions.py
                    --manifest ${_regions_manifest}
                    --out      ${_regions_header}
                    --out-flat ${_regions_flat}
                    --out-lua  ${_regions_lua}
            DEPENDS ${_regions_manifest}
                    ${CMAKE_SOURCE_DIR}/tools/gen_state_regions.py
            COMMENT "Generating ${GAME_ID} gameplay_state_regions.gen.h + flat table + lua mirror")
        set(_regions_sources ${_regions_header})
        # The manifest's direct rows must equal their derivation from the
        # link map and the current expect file: an expect row deleted by a
        # fix re-derives the manifest in the same change.
        add_test(NAME state_regions_derivation_${GAME_ID}
                 COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/derive_state_regions.py
                         --check ${_regions_manifest})
        set_tests_properties(state_regions_derivation_${GAME_ID}
                             PROPERTIES LABELS "fence;oracle")
    endif()

    # Every in-image word-table `jr` in the generated C dispatches through an
    # index switch, or is named in recomp.conf's JUMPTABLE_UNSWITCHED; the
    # regen runs the same check.
    add_test(NAME jumptable_dispatch_${GAME_ID}
             COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_jumptable_dispatch.py
                     --game ${GAME_ID})
    set_tests_properties(jumptable_dispatch_${GAME_ID}
                         PROPERTIES LABELS "fence" SKIP_RETURN_CODE 64)

    # Disc boot-order profile: the sector ranges the disc cache loader
    # reads first, generated into the C table the build record installs. A
    # game without the manifest loads in file order.
    set(_boot_order_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/disc_boot_order.toml)
    set(_boot_order_sources)
    set(GB_BOOT_ORDER 0)
    if(EXISTS ${_boot_order_manifest})
        set(GB_BOOT_ORDER 1)
        set(_boot_order_header ${_gen_dir}/disc_boot_order.gen.h)
        add_custom_command(
            OUTPUT  ${_boot_order_header}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/disc_boot_order.py header
                    --manifest ${_boot_order_manifest}
                    --out      ${_boot_order_header}
            DEPENDS ${_boot_order_manifest}
                    ${CMAKE_SOURCE_DIR}/tools/disc_boot_order.py
            COMMENT "Generating ${GAME_ID} disc_boot_order.gen.h")
        set(_boot_order_sources ${_boot_order_header})
    endif()

    # Save-header location: where the game's memory-card save-file header
    # sits on its disc, generated into the C table the build record
    # registers; the window icon is decoded from the player's disc there at
    # run time. A game without the manifest keeps the OpenPete mark.
    set(_save_icon_manifest ${CMAKE_CURRENT_SOURCE_DIR}/manifest/save_icon.toml)
    set(_save_icon_sources)
    set(GB_SAVE_ICON 0)
    if(EXISTS ${_save_icon_manifest})
        set(GB_SAVE_ICON 1)
        set(_save_icon_header ${_gen_dir}/save_icon.gen.h)
        add_custom_command(
            OUTPUT  ${_save_icon_header}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/save_icon.py header
                    --manifest ${_save_icon_manifest}
                    --out      ${_save_icon_header}
            DEPENDS ${_save_icon_manifest}
                    ${CMAKE_SOURCE_DIR}/tools/save_icon.py
            COMMENT "Generating ${GAME_ID} save_icon.gen.h")
        set(_save_icon_sources ${_save_icon_header})
    endif()

    # Curated PSX symbol aliases: gen/<tag>_symbols.gen.h from the game's
    # alias table, validated by address against globals.toml / symbols.toml
    # (the build fails when an addr is not a manifest symbol start).
    set(_symbols_sources "")
    if(G_SYMBOLS_MANIFEST)
        set(_symbols_header ${_gen_dir}/${_tag}_symbols.gen.h)
        add_custom_command(
            OUTPUT  ${_symbols_header}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
            COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_symbols_header.py
                    --symbols   ${G_SYMBOLS_MANIFEST}
                    --globals   ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
                    --functions ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                    --out       ${_symbols_header}
            DEPENDS ${G_SYMBOLS_MANIFEST}
                    ${CMAKE_CURRENT_SOURCE_DIR}/manifest/globals.toml
                    ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                    ${CMAKE_SOURCE_DIR}/tools/gen_symbols_header.py
            COMMENT "Generating ${_tag}_symbols.gen.h (validated vs manifest)")
        set(_symbols_sources ${_symbols_header})
    endif()

    # The game's guest address for each PSYQ abort-on-call stub, from its
    # generated psyq_stubs.c minus the shared thunk layer's names
    # (tools/filter_psyq_stubs.py table); the stub bodies themselves are the
    # compile-once union in openpete_psyq_stubs.
    set(_stub_table ${_gen_dir}/psyq_stub_table.c)
    add_custom_command(
        OUTPUT  ${_stub_table}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_gen_dir}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/filter_psyq_stubs.py
                table ${_stub_table} --tag ${_tag}
                --stubs ${_generated}/psyq_stubs.c
                --thunks ${OPENPETE_PSYQ_THUNK_SOURCES}
        DEPENDS ${_generated}/psyq_stubs.c ${OPENPETE_PSYQ_THUNK_SOURCES}
                ${CMAKE_SOURCE_DIR}/tools/filter_psyq_stubs.py
        COMMENT "Generating ${_tag} PSYQ stub address table"
        VERBATIM)

    # The build record (games/common/game_build.h), beside the generated
    # headers it includes.
    set(GB_ID ${GAME_ID})
    set(GB_TAG ${_tag})
    set(GB_IMPORTS_HEADER ${CMAKE_CURRENT_SOURCE_DIR}/module_imports.h)
    # The carve: EXTRACT's in every build, FIRST_BOOT_EXTRACT's with the
    # pipeline that compiles it.
    if(G_EXTRACT AND G_FIRST_BOOT_EXTRACT)
        message(FATAL_ERROR "openpete_add_game(${GAME_ID}): EXTRACT and FIRST_BOOT_EXTRACT are exclusive")
    endif()
    set(GB_EXTRACT ${G_EXTRACT})
    set(GB_HAS_EXTRACT 0)
    if(G_EXTRACT)
        set(GB_HAS_EXTRACT 1)
    elseif(_first_boot)
        set(GB_EXTRACT ${G_FIRST_BOOT_EXTRACT})
        set(GB_HAS_EXTRACT 1)
    endif()
    set(GB_WINDOW_LO "")
    set(GB_WINDOW_HI "")
    if(G_MODULE_LINKAGE STREQUAL "shared")
        set(GB_SHARED 1)
    else()
        set(GB_SHARED 0)
    endif()
    if(_first_boot)
        set(GB_FIRST_BOOT 1)
        # The first-boot pipeline's per-game facts from recomp.conf: the
        # overlay window its generated overlay table carries, and the
        # serial naming the staged manifest.
        file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf _range
             REGEX "^OVERLAY_RANGE=")
        file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/recomp.conf _exe_name
             REGEX "^EXE_FILENAME=")
        if(NOT _range MATCHES "^OVERLAY_RANGE=(0x[0-9A-Fa-f]+):(0x[0-9A-Fa-f]+)$")
            message(FATAL_ERROR "openpete_add_game(${GAME_ID}): recomp.conf has no OVERLAY_RANGE=<lo>:<hi>")
        endif()
        set(GB_WINDOW_LO ${CMAKE_MATCH_1})
        set(GB_WINDOW_HI ${CMAKE_MATCH_2})
        if(NOT _exe_name MATCHES "^EXE_FILENAME=\"([A-Z]+)_([0-9]+)\.([0-9]+)\"$")
            message(FATAL_ERROR "openpete_add_game(${GAME_ID}): recomp.conf has no EXE_FILENAME=\"XXXX_NNN.NN\"")
        endif()
        set(_serial "${CMAKE_MATCH_1}-${CMAKE_MATCH_2}${CMAKE_MATCH_3}")
    else()
        set(GB_FIRST_BOOT 0)
    endif()
    set(_build_c ${_gen_dir}/${_tag}_game_build.c)
    configure_file(${_common}/game_build.c.in ${_build_c} @ONLY)

    _openpete_game_partition(_own_sources _common_libs
        ${G_FEATURE_LIST}
        ${G_ORCHESTRATOR}
        ${G_INTERP_REGIONS}
        ${G_TERRAIN_GAME}
        ${G_MOBY_GAME}
        ${G_PLAYER_GAME}
        ${G_SOURCES}
        ${_attach_sources})

    add_library(${_host} OBJECT
        game_extras_impl.c
        ${_identity_c}
        ${_build_c}
        ${_regions_sources}
        ${_boot_order_sources}
        ${_save_icon_sources}
        ${_symbols_sources}
        ${_own_sources}
        # This game's PSYQ stub addresses (generated above).
        ${_stub_table})
    openpete_game_common_settings(${_host})

    # The generated headers as a target of their own, so the source parsers
    # order after them without waiting for the host library.
    add_custom_target(${_tag}_generated_headers
        DEPENDS ${_regions_sources} ${_boot_order_sources} ${_save_icon_sources}
                ${_symbols_sources})
    add_dependencies(${_host} ${_tag}_generated_headers)
    add_dependencies(openpete_generated_headers ${_tag}_generated_headers)

    set(_objects $<TARGET_OBJECTS:${_host}>)
    foreach(_lib IN LISTS _common_libs)
        list(APPEND _objects $<TARGET_OBJECTS:${_lib}>)
    endforeach()
    set_property(GLOBAL APPEND PROPERTY OPENPETE_LINKED_GAMES ${_tag})
    set_property(GLOBAL APPEND PROPERTY OPENPETE_HOST_OBJECT_LIBS ${_host} ${_common_libs})
    set_property(GLOBAL APPEND PROPERTY OPENPETE_GAME_MODULES ${_module})
    set_property(GLOBAL APPEND PROPERTY OPENPETE_GAME_IMPORTS_HEADERS
                 ${CMAKE_CURRENT_SOURCE_DIR}/module_imports.h)

    if(OPENPETE_ONE_EXECUTABLE)
        # The game's target: the one executable and the game's module; the
        # SDK, assets and first-boot inputs attach to it below. The game
        # directory holds no executable, module or SDK; a tree configured
        # before the move holds them, and they are deleted here.
        add_custom_target(${_exe} ALL)
        add_dependencies(${_exe} openpete ${_module})
        file(REMOVE ${CMAKE_CURRENT_BINARY_DIR}/${_exe}${CMAKE_EXECUTABLE_SUFFIX}
                    ${CMAKE_CURRENT_BINARY_DIR}/${_tag}-recomp${CMAKE_SHARED_LIBRARY_SUFFIX})
        file(REMOVE_RECURSE ${CMAKE_CURRENT_BINARY_DIR}/sdk
                            ${CMAKE_CURRENT_BINARY_DIR}/recomp)
        set(_image openpete)
    else()
        set(_list_c ${_gen_dir}/${_tag}_game_list.c)
        _openpete_game_list(${_list_c} ${_tag})
        add_executable(${_exe} ${_list_c} $<TARGET_OBJECTS:openpete_game_common> ${_objects})
        target_include_directories(${_exe} PRIVATE ${_common})
        _openpete_game_exe_link(${_exe})
        if(G_MODULE_LINKAGE STREQUAL "static")
            target_link_libraries(${_exe} PRIVATE ${_module})
        else()
            target_link_libraries(${_exe} PRIVATE ${CMAKE_DL_LIBS})
            # The default module path is the executable's directory, where
            # the module target writes; building the executable builds its
            # module.
            add_dependencies(${_exe} ${_module})
        endif()
        set(_image ${_exe})
    endif()

    if(G_MODULE_LINKAGE STREQUAL "shared")
        if(WIN32 AND OPENPETE_ONE_EXECUTABLE)
            # openpete_link_games merges this .def with every other linked
            # game's and links each module against the one implib.
            add_custom_target(${_tag}_module_def DEPENDS ${_module_def} ${_module_keepalive})
            set_property(GLOBAL APPEND PROPERTY OPENPETE_GAME_MODULE_DEFS ${_module_def})
            set_property(GLOBAL APPEND PROPERTY OPENPETE_GAME_MODULE_DEF_TARGETS ${_tag}_module_def)
            set(OPENPETE_GAME_MODULE_IMPLIB ${CMAKE_BINARY_DIR}/gen/libopenpete-exe-exports.a)
        elseif(WIN32)
            _openpete_game_shared_link_pe(${_tag}_module_implib ${_exe} ${_module_def} ${_module})
            set(OPENPETE_GAME_MODULE_IMPLIB ${_gen_dir}/libopenpete-exe-exports.a)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT OPENPETE_ONE_EXECUTABLE)
            _openpete_game_shared_link_elf(${_tag} ${_image} ${_module}
                ${_gen_dir}/${_tag}_module_imports.stamp
                ${CMAKE_CURRENT_SOURCE_DIR}/module_imports.h)
        endif()
        if(_first_boot)
            _openpete_game_stage_first_boot(${GAME_ID} ${_exe} ${_exe_dir} ${_generated} ${_serial})
        endif()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND MINGW AND NOT OPENPETE_ONE_EXECUTABLE)
        _openpete_stage_openal(${_exe} ${_exe_dir})
    endif()
    if(WIN32 AND NOT OPENPETE_ONE_EXECUTABLE)
        _openpete_exe_win32_resource(${_exe} ${_exe})
    endif()

    # Variables the mod SDK and the game's CMakeLists read.
    set(OPENPETE_GAME_EXE     ${_exe})
    set(OPENPETE_GAME_HOST    ${_host})
    set(OPENPETE_GAME_EXE_DIR ${_exe_dir})
    if(G_MOD_SDK)
        openpete_mod_sdk(${GAME_ID})
        set(OPENPETE_GAME_SDK_GEN   ${OPENPETE_GAME_SDK_GEN}   PARENT_SCOPE)
        set(OPENPETE_GAME_SDK_FILES ${OPENPETE_GAME_SDK_FILES} PARENT_SCOPE)
    endif()

    set(OPENPETE_GAME_FIRST_BOOT ${_first_boot} PARENT_SCOPE)
    set(OPENPETE_GAME_MODULE_IMPLIB "${OPENPETE_GAME_MODULE_IMPLIB}" PARENT_SCOPE)
    set(OPENPETE_GAME_TAG       ${_tag}       PARENT_SCOPE)
    set(OPENPETE_GAME_EXE       ${_exe}       PARENT_SCOPE)
    set(OPENPETE_GAME_HOST      ${_host}      PARENT_SCOPE)
    set(OPENPETE_GAME_EXE_DIR   ${_exe_dir}   PARENT_SCOPE)
    set(OPENPETE_GAME_BIN       ${_bin}       PARENT_SCOPE)
    set(OPENPETE_GAME_BIN_ARGS  "${_bin_args}" PARENT_SCOPE)
    set(OPENPETE_GAME_MODULE    ${_module}    PARENT_SCOPE)
    set(OPENPETE_GAME_GENERATED ${_generated} PARENT_SCOPE)
    set(OPENPETE_GAME_GEN_DIR   ${_gen_dir}   PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_FULL_C     ${_overlay_full_c}     PARENT_SCOPE)
    set(OPENPETE_GAME_OVERLAY_DISPATCH_C ${_overlay_dispatch_c} PARENT_SCOPE)
endfunction()

# The one executable (OPENPETE_ONE_EXECUTABLE), declared after every game:
# `openpete` at the build root, linking each game's host library, the
# shared host code once, and the list of the linked games its entry selects
# from. It depends on every game's module, and two build-time refusals
# guard the merged image: each module's imports against it (on ELF
# <tag>_module_imports; on PE each module's link against the implib of the
# merged .def, and the executable's link of that .def) and the include-path
# invariant over every host TU (openpete_game_include_check,
# tools/check_game_includes.py). On PE it also carries the OpenPete Win32
# resources (_openpete_exe_win32_resource) and OpenAL32.dll beside it.
function(openpete_link_games)
    if(NOT OPENPETE_ONE_EXECUTABLE)
        # A tree reconfigured from the one executable keeps its file.
        file(REMOVE ${CMAKE_BINARY_DIR}/openpete${CMAKE_EXECUTABLE_SUFFIX})
        return()
    endif()
    get_property(_tags GLOBAL PROPERTY OPENPETE_LINKED_GAMES)
    get_property(_libs GLOBAL PROPERTY OPENPETE_HOST_OBJECT_LIBS)
    get_property(_modules GLOBAL PROPERTY OPENPETE_GAME_MODULES)
    get_property(_headers GLOBAL PROPERTY OPENPETE_GAME_IMPORTS_HEADERS)
    if(NOT _tags)
        return()
    endif()
    list(REMOVE_DUPLICATES _libs)
    list(PREPEND _libs openpete_game_common)

    set(_list_c ${CMAKE_BINARY_DIR}/gen/openpete_game_list.c)
    _openpete_game_list(${_list_c} ${_tags})
    set(_objects "")
    foreach(_lib IN LISTS _libs)
        list(APPEND _objects $<TARGET_OBJECTS:${_lib}>)
    endforeach()
    add_executable(openpete ${_list_c} ${_objects})
    target_include_directories(openpete PRIVATE ${CMAKE_SOURCE_DIR}/games/common)
    _openpete_game_exe_link(openpete)
    target_link_libraries(openpete PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(openpete ${_modules})

    if(WIN32)
        # The executable exports the union of the games' .def files and each
        # module links the one implib naming openpete.exe; the first-boot
        # kit stages that implib.
        get_property(_defs GLOBAL PROPERTY OPENPETE_GAME_MODULE_DEFS)
        get_property(_def_targets GLOBAL PROPERTY OPENPETE_GAME_MODULE_DEF_TARGETS)
        set(_def ${CMAKE_BINARY_DIR}/gen/openpete_module_exports.def)
        _openpete_merge_module_defs(${_def} "${_defs}" "${_def_targets}")
        add_dependencies(openpete openpete_module_def)
        _openpete_game_shared_link_pe(openpete_module_implib openpete ${_def} ${_modules})
        if(TARGET openpete_first_boot_kit)
            add_dependencies(openpete_first_boot_kit openpete_module_implib)
        endif()
        _openpete_stage_openal(openpete ${CMAKE_BINARY_DIR})
        _openpete_exe_win32_resource(openpete openpete)
    else()
        # Each module's imports against the one executable; the forwarders
        # are every linked game's, since the executable defines them all.
        foreach(_t _m IN ZIP_LISTS _tags _modules)
            _openpete_game_shared_link_elf(${_t} openpete ${_m}
                ${CMAKE_BINARY_DIR}/gen/${_t}_module_imports.stamp ${_headers})
        endforeach()
    endif()

    # The include-path invariant, before the executable links.
    set(_stamp ${CMAKE_BINARY_DIR}/gen/openpete_game_include_check.stamp)
    add_custom_command(
        OUTPUT  ${_stamp}
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/check_game_includes.py
                --compile-db ${CMAKE_BINARY_DIR}/compile_commands.json
                --source-dir ${CMAKE_SOURCE_DIR}
                --build-dir  ${CMAKE_BINARY_DIR}
                --games ${OPENPETE_GAMES}
                --targets ${_libs} openpete
                --stamp ${_stamp}
        DEPENDS ${CMAKE_BINARY_DIR}/compile_commands.json
                ${CMAKE_SOURCE_DIR}/tools/check_game_includes.py
        COMMENT "Checking that no host TU resolves a game-local header through its include path"
        VERBATIM)
    add_custom_target(openpete_game_include_check DEPENDS ${_stamp})
    add_dependencies(openpete openpete_game_include_check)

    openpete_mod_sdk_stage_all()
endfunction()

# The executable that runs GAME_ID and the arguments selecting the game in
# it: OUT_BIN receives the path (the one executable, or the game's own), and
# OUT_ARGS `--game <id>` for the one executable, else nothing. Valid after
# openpete_add_game(GAME_ID); OUT_BIN is empty for a game not configured.
function(openpete_game_bin GAME_ID OUT_BIN OUT_ARGS)
    get_property(_bin GLOBAL PROPERTY OPENPETE_GAME_BIN_${GAME_ID})
    get_property(_args GLOBAL PROPERTY OPENPETE_GAME_BIN_ARGS_${GAME_ID})
    set(${OUT_BIN} "${_bin}" PARENT_SCOPE)
    set(${OUT_ARGS} "${_args}" PARENT_SCOPE)
endfunction()
