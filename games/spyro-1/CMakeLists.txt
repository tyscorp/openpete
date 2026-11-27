# games/spyro-1/CMakeLists.txt - Spyro 1 binary.
#
# The module and executable skeleton come from cmake/openpete_game.cmake;
# this file adds what only this game has: its first-boot extraction, the
# mod SDK and its docs, the build-time mod compile, the SoundTable names, the
# state-region tables, the Windows subsystem choice, and the browser link.

set(SPYRO_GENERATED ${CMAKE_CURRENT_SOURCE_DIR}/generated)

# Per-function CPU tracing for the perf profiler: instrument the main
# generated EXE with -finstrument-functions so func_trace.c can attribute
# self/total time per recompiled func_80XXXXXX. ~1-8 ms/tick overhead - a
# drill-down build, never the baseline. Off by default (zero cost).
option(OPENPETE_FUNC_TRACE
       "Instrument the generated EXE for per-function CPU tracing"
       OFF)
# The pointer->name symtab takes the address of every recompiled function in
# the exe, which links them only under static linkage.
if(OPENPETE_FUNC_TRACE AND NOT OPENPETE_RECOMP_LINKAGE STREQUAL "static")
    message(FATAL_ERROR "OPENPETE_FUNC_TRACE needs the module linked into the "
                        "exe: configure with -DOPENPETE_RECOMP_LINKAGE=static")
endif()

include(${CMAKE_SOURCE_DIR}/cmake/openpete_game.cmake)
openpete_add_game(spyro-1
    FEATURE_LIST   feature_list.c
    ORCHESTRATOR   renderer/reextract.c
    INTERP_REGIONS renderer/interp_regions.c
    TERRAIN_GAME   renderer/terrain_game.c
    MOBY_GAME      renderer/moby_game.c
    PLAYER_GAME    renderer/player_game.c
    SYMBOLS_MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest/spyro1_symbols.toml
    MOD_SDK
    SOURCES
        # The builds a disc of this game can hold (openpete_disc_build.h).
        disc_builds.c
        psyq_hooks.cpp

        # The native renderer's game half: the feeders and the interp region
        # table. The sub-tick re-extract orchestrator is ORCHESTRATOR above.
        renderer/render_decouple.c
        renderer/spyro.c
        renderer/shadow_spyro.c
        renderer/particle.c
        renderer/tracer.c
        renderer/tracer_snapshot.c
        renderer/layout.c
        renderer/portal.c
        # The 3D glyph text producer, shared with Spyro 3.
        ${CMAKE_SOURCE_DIR}/games/common/renderer/text_moby.c
        ${CMAKE_SOURCE_DIR}/games/common/renderer/wad_subfile.c
        # The launcher's asset extraction job: domes, glyph models and logo from a disc.
        ${CMAKE_SOURCE_DIR}/games/common/launcher_assets.c
        ${CMAKE_SOURCE_DIR}/games/common/launcher_cache.c
        ${CMAKE_SOURCE_DIR}/games/common/launcher_preview.c
        renderer/text_moby_id.c
        renderer/transition.c

        # Non-renderer feature modules.
        features/freecam.c
        features/audio_volume.c
        features/vibration.c
        features/cheats.c
        features/credits_skip.c
        features/host_hop_test.c
        features/crash_demo_woah.c
        features/levelwarp.c
        ${CMAKE_SOURCE_DIR}/games/common/features/levelwarp_ui.c
        ${CMAKE_SOURCE_DIR}/games/common/features/level_table.c
        ${CMAKE_SOURCE_DIR}/games/common/features/level_skip.c

        sfx_census.cpp
        write_trace.cpp

        # Mods: the WAD asset overlay, the moby-type and level-slot
        # registry game ops, and the game descriptor that carries them.
        mods/mod_wad_overlay.c
        mods/mod_moby_ops.c
        mods/mod_level_ops.c
        mods/mod_game_desc.c
        mods/mod_save_desc.c

        # First-run distribution: the WAD extraction (the disc library and
        # the launcher are core/launcher/; the module build is
        # FIRST_BOOT_SOURCES below).
        dist/recomp_extract.cpp
    FIRST_BOOT_SOURCES
        # The first-boot module build: extract (dist/recomp_extract.cpp
        # above), recompile, compile, link.
        ${CMAKE_SOURCE_DIR}/games/common/recomp_build.cpp
    # The carve is in SOURCES, so every build answers --extract-recomp-inputs.
    EXTRACT spyro1_extract_first_boot_inputs)

# SoundTable slot names (tools/gen_sfx_names.py over the decomp's
# sound_table.h): one table shared by the sfx resolver, the sfx pack's
# symbolic form and the census dump's file labels.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_sfx_names.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
    COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
            --header ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include/sound_table.h
            --out ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_sfx_names.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/spyro-1/include/sound_table.h
            ${CMAKE_SOURCE_DIR}/tools/gen_sfx_names.py
    COMMENT "Generating SoundTable slot names (gen_sfx_names.py)")
add_custom_target(spyro1_sfx_names DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gen/spyro1_sfx_names.h)
add_dependencies(${OPENPETE_GAME_HOST} spyro1_sfx_names)


# --- Mod SDK ---
# Generated and staged by openpete_add_game(MOD_SDK)
# (cmake/openpete_mod_sdk.cmake): <exe>/sdk/ and <exe>/sdk/games/spyro-1/;
# the root's dispatchers select Spyro 1's headers for build lines that
# pass `-I sdk` alone. renderer/layout.c takes its member offsets from the
# typed structs header in the generation directory.
set(_mod_sdk_gen ${OPENPETE_GAME_SDK_GEN})
add_dependencies(openpete_generated_headers spyro1_sfx_names)

# --- Documentation site (docs/mkdocs.yml), shipped sections ---
# scripts/build-docs.sh runs tools/gen_mod_docs.py, which extracts the SDK
# surface (api header, symbol/global/prototype manifests, typed structs,
# mod examples) into the Modding section's markdown, then builds the site
# with mkdocs when it is installed (notice + green build when it isn't;
# scripts/package-dist.sh refuses a tree without the site). One build of
# the published sections, User and Modding, into docs-site/: every package
# (Windows zip, Linux tarball, AppImage, with or without the mod SDK)
# stages it as docs/, and openpete.com serves the same set. Developer and
# Spyro are not published, so this target neither builds them nor runs
# gen_code_docs.py (`code_docs`; scripts/build-docs.sh with no --sections
# builds every section); a link into them renders as its text
# (tools/mkdocs_sections.py). Stamp-guarded: the docs rebuild exactly when
# an input changed.
set(_docs_stamp ${CMAKE_CURRENT_BINARY_DIR}/docs-site.stamp)
# The reference pages cover every game's manifests and decomp headers and
# link each symbol to the decomp checkout at its commit; the reflog is
# appended on every HEAD move, so it stands in for "the submodule commit
# changed".
set(_docs_game_inputs "")
foreach(_g spyro-1 spyro-2 spyro-3)
    execute_process(
        COMMAND git -C ${CMAKE_SOURCE_DIR}/games/${_g}/${_g} rev-parse --git-path logs/HEAD
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET RESULT_VARIABLE _rc)
    if(_rc EQUAL 0 AND EXISTS ${_head})
        list(APPEND _docs_game_inputs ${_head})
    endif()
    file(GLOB _hdrs CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/games/${_g}/${_g}/include/*.h)
    list(APPEND _docs_game_inputs ${_hdrs}
         ${CMAKE_SOURCE_DIR}/games/${_g}/manifest/symbols.toml
         ${CMAKE_SOURCE_DIR}/games/${_g}/manifest/globals.toml)
endforeach()
file(GLOB _docs_manifests CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/mods/*/mod.toml)
# Every source file of the built sections, the top-level pages and the
# shared assets. modding/imgui.md and modding/reference/ are written by
# the command below, so listing them as inputs would make the stamp depend
# on its own output; the Modding glob is one level deep so it never sees
# reference/.
file(GLOB_RECURSE _docs_pages CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/docs/src/user/*
     ${CMAKE_SOURCE_DIR}/docs/src/assets/*)
file(GLOB _docs_modding CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/docs/src/modding/*.md)
list(REMOVE_ITEM _docs_modding ${CMAKE_SOURCE_DIR}/docs/src/modding/imgui.md)
file(GLOB _docs_top CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/docs/src/*.md)
# The surface macros of the User pages and the generated user/reference/
# pages read the committed baselines, the shipped toml, the shipped-games
# list, the key-name table and CHANGELOG.md (tools/mkdocs_surface.py).
file(GLOB _docs_surface CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/packaging/surface/*.json)
add_custom_command(
    OUTPUT  ${_docs_stamp}
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build-docs.sh
            --sections user,modding
            --structs ${_mod_sdk_gen}/openpete_game_structs.h
            --sdk-dir ${CMAKE_BINARY_DIR}/mod-sdk-test/sdk
            --site-dir ${CMAKE_CURRENT_BINARY_DIR}/docs-site
    COMMAND ${CMAKE_COMMAND} -E touch ${_docs_stamp}
    DEPENDS ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_api.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_mod_ui.h
            ${CMAKE_SOURCE_DIR}/core/include/openpete_imgui.h
            ${CMAKE_SOURCE_DIR}/third_party/imgui/imgui.h
            ${CMAKE_SOURCE_DIR}/tools/gen_imgui_table.py
            ${_docs_game_inputs}
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/prototypes.toml
            ${CMAKE_CURRENT_SOURCE_DIR}/manifest/struct_layout.toml
            ${_mod_sdk_gen}/openpete_game_structs.h
            ${CMAKE_SOURCE_DIR}/tools/gen_mod_docs.py
            ${CMAKE_SOURCE_DIR}/scripts/build-docs.sh
            ${CMAKE_SOURCE_DIR}/docs/mkdocs.yml
            ${CMAKE_SOURCE_DIR}/tools/mkdocs_sections.py
            ${CMAKE_SOURCE_DIR}/tools/mkdocs_surface.py
            ${CMAKE_SOURCE_DIR}/tools/surface_macros.py
            ${CMAKE_SOURCE_DIR}/tools/mkdocs_surface_keys.toml
            ${CMAKE_SOURCE_DIR}/packaging/openpete-dist.toml
            ${CMAKE_SOURCE_DIR}/games/shipped.toml
            ${CMAKE_SOURCE_DIR}/CHANGELOG.md
            ${_docs_surface}
            ${_docs_manifests}
            ${_docs_pages}
            ${_docs_modding}
            ${_docs_top}
            ${CMAKE_SOURCE_DIR}/mods/examples.txt
    COMMENT "Building the documentation site (gen_mod_docs.py + mkdocs)")
add_custom_target(docs ALL DEPENDS ${_docs_stamp})

# --- Build-time mod compile
# `cmake --build` leaves every mod compiled, not just the engine:
#  - Native presets: run the freshly built exe in --compile-mods mode over
#    OPENPETE_COMPILE_MODS_DIR, so keys/stamps/flags are byte-identical to
#    compile-on-boot (a boot after a clean build recompiles nothing). A
#    failed mod compile fails the build (escape: -DOPENPETE_COMPILE_MODS=OFF
#    or build --target openpete-spyro1).
#  - Cross presets (windows-mingw*): the target exe can't run at build
#    time; stage every repo mod next to the exe (cmake/stage_mods.cmake:
#    exact source mirror; user-owned config.toml/enabled.txt and staged-
#    only mod dirs untouched) and cross-compile each one with the
#    preset's own toolchain into <build>/games/spyro-1/mods/<id>/.build/
#    spyro-1/<id>.dll, DROPPING the stamp - an unstamped prebuilt is the
#    loader's designed cross-handoff artifact (build_mod: loads silently
#    when the host has no compiler, recompiles+stamps once when it
#    does). DLLs compile from the REPO src/*.c (identical to the staged
#    mirror), so neither staged sources nor staged DLLs can rot.
#    OPENPETE_STAGE_MOD_DLLS=OFF (windows-mingw-launcher, the distribution
#    preset) stages sources only, no cmake-side DLLs: that exe's mods
#    dir is the first-boot-compile validation surface - the bundled
#    llvm-mingw must build every mod at boot exactly as it would on an
#    end user's box, so cmake pre-seeding .build/ would mask it.
option(OPENPETE_COMPILE_MODS "Compile all mods as part of the build" ON)
option(OPENPETE_STAGE_MOD_DLLS
    "Cross presets: also cross-compile each staged mod's DLL (OFF = stage sources only; the exe compiles them on boot)" ON)
set(OPENPETE_COMPILE_MODS_DIR ${CMAKE_SOURCE_DIR}/mods CACHE PATH
    "Mods root the native-preset build-time compile runs over")
if(OPENPETE_COMPILE_MODS)
    if(CMAKE_CROSSCOMPILING)
        # Staged-SDK *sources* - mirrors mod_compute_key()'s sdk_inputs
        # list, so an SDK change re-cross-compiles every staged mod.
        set(_mod_sdk_deps
            ${OPENPETE_MOD_SDK_ROOT_FILES}
            ${OPENPETE_GAME_SDK_FILES})
        # Always runs (cheap: copy-if-different over a <5MB tree); keeps
        # staged mod sources/assets current with the repo every build.
        add_custom_target(spyro1_mods_stage ALL
            COMMAND ${CMAKE_COMMAND}
                    -DSRC=${OPENPETE_COMPILE_MODS_DIR}
                    -DDST=${OPENPETE_GAME_EXE_DIR}/mods
                    -P ${CMAKE_SOURCE_DIR}/cmake/stage_mods.cmake
            COMMENT "Staging mods into <exe>/mods/ (source mirror)"
            VERBATIM)
        set(_staged_mod_dlls "")
        if(OPENPETE_STAGE_MOD_DLLS)
        # The compiler's install tree: the parent of its bin/ directory.
        get_filename_component(_mod_cc_tree ${CMAKE_C_COMPILER} DIRECTORY)
        get_filename_component(_mod_cc_tree ${_mod_cc_tree} DIRECTORY)
        file(GLOB _mod_manifests CONFIGURE_DEPENDS
             ${OPENPETE_COMPILE_MODS_DIR}/*/mod.toml)
        foreach(_manifest IN LISTS _mod_manifests)
            get_filename_component(_src_mod_dir ${_manifest} DIRECTORY)
            get_filename_component(_mod_id ${_src_mod_dir} NAME)
            file(GLOB _mod_srcs CONFIGURE_DEPENDS ${_src_mod_dir}/src/*.c)
            if(NOT _mod_srcs)
                continue() # assets-only/binary-only mod: nothing to build
            endif()
            # One DLL per game the manifest names, each against that game's
            # SDK, into .build/<game>/ where the loader looks for a prebuilt
            # object when no compiler is bundled; a manifest without a game
            # key builds for spyro-1.
            file(STRINGS ${_manifest} _mod_game_line REGEX "^[ \t]*game[ \t]*=")
            set(_mod_games "")
            foreach(_g spyro-1 spyro-2 spyro-3)
                if(NOT _mod_game_line OR _mod_game_line MATCHES "${_g}")
                    list(APPEND _mod_games ${_g})
                endif()
            endforeach()
            if(NOT _mod_game_line)
                set(_mod_games spyro-1)
            endif()
            file(GLOB _mod_hdrs CONFIGURE_DEPENDS ${_src_mod_dir}/src/*.h)
            foreach(_g IN LISTS _mod_games)
                set(_mod_build_dir
                    ${OPENPETE_GAME_EXE_DIR}/mods/${_mod_id}/.build/${_g})
                set(_dll ${_mod_build_dir}/${_mod_id}.dll)
                add_custom_command(OUTPUT ${_dll}
                    COMMAND ${CMAKE_COMMAND} -E make_directory ${_mod_build_dir}
                    # Same compile line mod_compile() (core/mods/mod_loader.c)
                    # uses on Windows - keep in sync - plus -static-libgcc:
                    # the exe ships no mingw runtime DLLs (mingw-w64.cmake
                    # links -static), so the mod DLL must be self-contained.
                    # The path remaps are mod_compile's mod_cc_map_templates
                    # in its order (most specific last): the DLL names no
                    # directory of the build machine.
                    COMMAND ${CMAKE_C_COMPILER} -shared -Wl,--export-all-symbols
                            -O2 -g -fno-strict-aliasing -Wall -Wundef -static-libgcc
                            -ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=.
                            -ffile-prefix-map=${_mod_cc_tree}=toolchain
                            -ffile-prefix-map=${OPENPETE_GAME_EXE_DIR}/sdk=sdk
                            -ffile-prefix-map=${_src_mod_dir}=mods/${_mod_id}
                            -I${OPENPETE_GAME_EXE_DIR}/sdk/games/${_g}
                            -I${OPENPETE_GAME_EXE_DIR}/sdk
                            -o ${_dll} ${_mod_srcs}
                            ${OPENPETE_GAME_EXE_DIR}/sdk/openpete_mod_abi.c
                    COMMAND ${CMAKE_COMMAND} -E rm -f
                            ${_mod_build_dir}/${_mod_id}.hash
                    DEPENDS ${_mod_srcs} ${_mod_hdrs} ${_mod_sdk_deps}
                            openpete-spyro1
                    COMMENT "Cross-compiling mod ${_mod_id}.dll for ${_g}"
                    VERBATIM)
                list(APPEND _staged_mod_dlls ${_dll})
            endforeach()
        endforeach()
        endif() # OPENPETE_STAGE_MOD_DLLS
        add_custom_target(spyro1_mods ALL DEPENDS ${_staged_mod_dlls})
        add_dependencies(spyro1_mods spyro1_mods_stage spyro1_sdk_stage)
    else()
        # Always runs (cheap: the exe's pass-1 content-hash check skips
        # up-to-date mods in milliseconds); nonzero exit fails the build.
        # The run's log file and per-user directories resolve under a sandbox
        # root in the build tree, so a build never rotates the player's log.
        add_custom_target(spyro1_mods ALL
            COMMAND ${CMAKE_COMMAND} -E env
                    OPENPETE_SANDBOX=${CMAKE_BINARY_DIR}/compile-mods-sandbox
                    ${OPENPETE_GAME_BIN} ${OPENPETE_GAME_BIN_ARGS} --compile-mods
                    --mods-dir ${OPENPETE_COMPILE_MODS_DIR}
            COMMENT "Compiling mods (${OPENPETE_GAME_BIN} --compile-mods)"
            VERBATIM)
        add_dependencies(spyro1_mods openpete-spyro1 spyro1_sdk_stage)
    endif()
endif()
# Emscripten / WebAssembly browser link.
#
#  ASYNCIFY        - required twice over: (1) libco's emscripten backend
#                    maps co_switch onto emscripten_fiber_swap, and (2) the
#                    host loop yields to the browser via emscripten_sleep
#                    inside op_sleep_until. Both are Asyncify operations.
#  STACK_SIZE      - the primary (host) fiber runs on the main C stack; the
#                    PsyCross render path is deep, so bump well past the
#                    64 KiB default. The game fiber has its own 8 MiB stack
#                    (see run.c co_create).
#  MEMORY GROWTH   - the player's disc (~630 MiB) is streamed into MEMFS, so
#                    we start modest and grow toward the wasm32 ceiling.
#  INVOKE_RUN=0    - main() is launched from the shell's callMain() only
#                    after the disc has been written into the FS.
#  FULL_ES2        - PsyCross selects RENDERER_OGLES / OGLES_VERSION 2 under
#                    __EMSCRIPTEN__ (WebGL via GLES2 emulation).
if(EMSCRIPTEN)
    target_link_options(openpete-spyro1 PRIVATE
        "-sASYNCIFY=1"
        "-sASYNCIFY_STACK_SIZE=1048576"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"      # 256 MiB
        "-sMAXIMUM_MEMORY=2147483648"     # 2 GiB
        "-sSTACK_SIZE=4194304"            # 4 MiB host/primary-fiber C stack
        "-sFORCE_FILESYSTEM=1"
        "-sINVOKE_RUN=0"
        "-sEXIT_RUNTIME=0"
        "-sEXPORTED_RUNTIME_METHODS=callMain,FS"
        "-sFULL_ES3=1"
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
        "-sGL_ENABLE_GET_PROC_ADDRESS=1"
        "-sALLOW_BLOCKING_ON_MAIN_THREAD=1"
        "-lopenal"
    )
    # Optional: keep WASM function names in the binary so browser stack
    # traces are legible (negligible runtime cost, larger .wasm). Useful
    # for diagnosing Asyncify / call_indirect signature issues.
    option(OPENPETE_WASM_PROFILING "Emit named WASM functions for debugging" OFF)
    if(OPENPETE_WASM_PROFILING)
        target_link_options(openpete-spyro1 PRIVATE "--profiling-funcs")
    endif()
    # emcc emits openpete-spyro1.js + openpete-spyro1.wasm. Ship the
    # hand-authored drag-and-drop shell alongside them - NO copyrighted
    # disc data is bundled; the player supplies their own dump at runtime.
    add_custom_command(TARGET openpete-spyro1 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
            "${OPENPETE_GAME_EXE_DIR}/index.html"
        VERBATIM)
endif()

# The native renderer's SPIR-V shaders are embedded in the binary (baked at
# build time by core/rhi/embed_shaders.cmake), so nothing is staged beside the
# exe - the executable is self-contained. For shader dev/modding without a
# relink, point OPENPETE_SHADER_DIR at a dir of loose .spv (e.g. the build's
# core/rhi/shaders).

# Stage the music WAV override directory next to the executable so users
# have a ready-made drop point, and seed it with the template manifest
# (full track table + instructions). copy_if_missing never clobbers a
# user-edited manifest on rebuild.
add_custom_command(TARGET openpete-spyro1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${OPENPETE_GAME_EXE_DIR}/assets/music"
    COMMAND ${CMAKE_COMMAND}
        -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/assets/music/manifest.txt
        -DDST=${OPENPETE_GAME_EXE_DIR}/assets/music/manifest.txt
        -P ${CMAKE_SOURCE_DIR}/cmake/copy_if_missing.cmake
    VERBATIM)

# Under OPENPETE_FUNC_TRACE the main EXE TU carries -finstrument-functions
# (per-function CPU tracing). The exclude-list keeps the hot
# unaligned-access helpers and the dispatch switch uninstrumented. When the
# option is OFF nothing is added: zero instrumentation, byte-identical code.
if(OPENPETE_FUNC_TRACE)
    set(_functrace_full_flags
        "-finstrument-functions -finstrument-functions-exclude-function-list=psx_lwl,psx_lwr,psx_swl,psx_swr,psx_dispatch")

    # Generate the pointer->name symtab func_trace.c resolves the hooks against.
    set(_functrace_symtab ${CMAKE_CURRENT_BINARY_DIR}/gen/func_trace_symtab.c)
    add_custom_command(
        OUTPUT  ${_functrace_symtab}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/gen
        COMMAND python3 ${CMAKE_SOURCE_DIR}/tools/gen_func_trace_symtab.py
                --dispatch ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
                --symbols  ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                --out      ${_functrace_symtab}
        DEPENDS ${SPYRO_GENERATED}/PSX.EXE_dispatch.c
                ${CMAKE_CURRENT_SOURCE_DIR}/manifest/symbols.toml
                ${CMAKE_SOURCE_DIR}/tools/gen_func_trace_symtab.py
        COMMENT "Generating func-trace symtab")
    target_sources(openpete-spyro1 PRIVATE ${_functrace_symtab})
    set_source_files_properties(${_functrace_symtab} PROPERTIES
        INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/core/include")
    set_source_files_properties(
        ${SPYRO_GENERATED}/PSX.EXE_full.c
        PROPERTIES
            COMPILE_FLAGS "${_functrace_full_flags}"
    )
endif()
