# tests/sched/present - the present-thread interleaving harness: the real
# present_thread.c, the scene ring copied from native_render.c and the real
# rhi_vulkan.c over a fake Vulkan device, under the scheduler of ../lib.
#
# The protocol objects are test copies compiled with -fsanitize=thread (the
# scheduler's __tsan_* entry points, no libtsan); objcopy makes the statics
# the harness reads global. Release objects are not touched.
cmake_language(DEFER CALL openpete_label_directory_tests unit)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(PT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(PT_GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${PT_GEN})

add_custom_command(
    OUTPUT ${PT_GEN}/ring_extract.c ${PT_GEN}/pt_state.h
    COMMAND ${Python3_EXECUTABLE} ${PT_DIR}/extract_ring.py
            ${CMAKE_SOURCE_DIR}/core/renderer/native_render.c
            ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c
            ${PT_GEN}/ring_extract.c ${PT_GEN}/pt_state.h
    DEPENDS ${PT_DIR}/extract_ring.py
            ${CMAKE_SOURCE_DIR}/core/renderer/native_render.c
            ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c
    COMMENT "present_sched: copying the scene ring and the present thread's state"
    VERBATIM)
add_custom_target(present_sched_gen DEPENDS ${PT_GEN}/ring_extract.c ${PT_GEN}/pt_state.h)

set(PT_INCLUDES
    ${PT_DIR} ${PT_GEN} ${CMAKE_CURRENT_SOURCE_DIR}/../lib
    ${CMAKE_SOURCE_DIR}/core/include
    ${CMAKE_SOURCE_DIR}/core/include/psxrt
    ${CMAKE_SOURCE_DIR}/core/host
    ${CMAKE_SOURCE_DIR}/core/rhi
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include
    ${CMAKE_SOURCE_DIR}/third_party/volk)
set(PT_TSAN -fsanitize=thread -O2 -g)

# The protocol objects, instrumented.
add_library(present_sched_pt OBJECT ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c)
target_include_directories(present_sched_pt PRIVATE ${PT_INCLUDES})
target_compile_definitions(present_sched_pt PRIVATE VK_NO_PROTOTYPES OPENPETE_ENGINE_BUILD)
target_compile_options(present_sched_pt PRIVATE ${PT_TSAN})
target_link_libraries(present_sched_pt PRIVATE SDL3::Headers)
set_target_properties(present_sched_pt PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

add_library(present_sched_rhi OBJECT ${CMAKE_SOURCE_DIR}/core/rhi/rhi_vulkan.c)
target_include_directories(present_sched_rhi PRIVATE ${PT_INCLUDES})
target_compile_definitions(present_sched_rhi PRIVATE VK_NO_PROTOTYPES)
target_compile_options(present_sched_rhi PRIVATE ${PT_TSAN})
target_link_libraries(present_sched_rhi PRIVATE SDL3::Headers)
set_target_properties(present_sched_rhi PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)

# The statics the harness reads, made global on the test copies.
add_custom_command(
    OUTPUT ${PT_GEN}/present_thread.sched.o
    COMMAND ${CMAKE_OBJCOPY} --globalize-symbol=s_pt --globalize-symbol=s_staged --globalize-symbol=s_lat
            $<TARGET_OBJECTS:present_sched_pt> ${PT_GEN}/present_thread.sched.o
    DEPENDS present_sched_pt $<TARGET_OBJECTS:present_sched_pt>
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_command(
    OUTPUT ${PT_GEN}/rhi_vulkan.sched.o
    COMMAND ${CMAKE_OBJCOPY} --globalize-symbol=s_display_owner
            $<TARGET_OBJECTS:present_sched_rhi> ${PT_GEN}/rhi_vulkan.sched.o
    DEPENDS present_sched_rhi $<TARGET_OBJECTS:present_sched_rhi>
    COMMAND_EXPAND_LISTS VERBATIM)
add_custom_command(
    OUTPUT ${PT_GEN}/vk_list.h
    COMMAND ${Python3_EXECUTABLE} ${PT_DIR}/gen_vk_list.py ${CMAKE_NM} ${PT_GEN}/rhi_vulkan.sched.o ${PT_GEN}/vk_list.h
    DEPENDS ${PT_DIR}/gen_vk_list.py ${PT_GEN}/rhi_vulkan.sched.o
    VERBATIM)
# The headers the harness's compile-database entries include, built with the
# engine's generated headers so a scan of the database finds them.
add_custom_target(present_sched_headers DEPENDS ${PT_GEN}/pt_state.h ${PT_GEN}/vk_list.h)
add_dependencies(openpete_generated_headers present_sched_headers)
set_source_files_properties(${PT_GEN}/present_thread.sched.o ${PT_GEN}/rhi_vulkan.sched.o
    PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

# The harness's own objects, once per ring size.
function(present_sched_objects ring)
    add_library(present_sched_obj${ring} OBJECT
        ${PT_DIR}/harness.c ${PT_DIR}/fakevk.c ${PT_DIR}/stubs.c ${PT_GEN}/vk_list.h)
    target_include_directories(present_sched_obj${ring} PRIVATE ${PT_INCLUDES})
    # The build's file-prefix maps (top-level CMakeLists.txt), which the race detector reverses to read sources.
    target_compile_definitions(present_sched_obj${ring} PRIVATE VK_NO_PROTOTYPES PT_SCENE_RING=${ring}
        PT_SOURCE_DIR="${CMAKE_SOURCE_DIR}" PT_BINARY_DIR="${CMAKE_BINARY_DIR}")
    target_compile_options(present_sched_obj${ring} PRIVATE -Wall -Wextra)
    target_link_libraries(present_sched_obj${ring} PRIVATE SDL3::Headers)
    set_target_properties(present_sched_obj${ring} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    add_dependencies(present_sched_obj${ring} present_sched_gen)
endfunction()
present_sched_objects(48)
present_sched_objects(3)

# A copy of the scene ring, instrumented, for a ring size.
function(present_sched_ring name ring src)
    add_library(${name} OBJECT ${src})
    target_include_directories(${name} PRIVATE ${PT_INCLUDES})
    target_compile_definitions(${name} PRIVATE VK_NO_PROTOTYPES PT_SCENE_RING=${ring})
    target_compile_options(${name} PRIVATE ${PT_TSAN})
    target_link_libraries(${name} PRIVATE SDL3::Headers)
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    add_dependencies(${name} present_sched_gen)
endfunction()
present_sched_ring(present_sched_ring48 48 ${PT_GEN}/ring_extract.c)
present_sched_ring(present_sched_ring3 3 ${PT_GEN}/ring_extract.c)

# One harness binary: the harness's objects for a ring size, a copy of the ring (an object
# library), and an instrumented present-thread object.
function(present_sched_harness name ring ring_lib pt_obj)
    add_executable(${name} $<TARGET_OBJECTS:present_sched_obj${ring}> $<TARGET_OBJECTS:${ring_lib}>
        ${pt_obj} ${PT_GEN}/rhi_vulkan.sched.o)
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    add_dependencies(${name} present_sched_gen openpete_rhi)
    target_link_options(${name} PRIVATE -Wl,--wrap=rhi_display_claim)
    # The RHI's other objects (pipeline cache, memory types, GPU audit) from its archive, before
    # the scheduler's; no SDL, no libtsan.
    target_link_libraries(${name} PRIVATE $<TARGET_FILE:openpete_rhi> openpete_sched m)
endfunction()
present_sched_harness(present_sched 48 present_sched_ring48 ${PT_GEN}/present_thread.sched.o)
present_sched_harness(present_sched_r3 3 present_sched_ring3 ${PT_GEN}/present_thread.sched.o)

# The race-detector (noise) build: the same harness objects, scenarios and instrumented protocol objects,
# linked against the scheduler's noise runtime and the real ThreadSanitizer runtime instead of the
# scheduler's own __tsan_* entry points.
function(present_sched_noise name ring_lib pt_obj)
    add_executable(${name} $<TARGET_OBJECTS:present_sched_obj48> $<TARGET_OBJECTS:${ring_lib}>
        ${pt_obj} ${PT_GEN}/rhi_vulkan.sched.o)
    set_target_properties(${name} PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
    add_dependencies(${name} present_sched_gen openpete_rhi)
    target_link_options(${name} PRIVATE -Wl,--wrap=rhi_display_claim)
    target_link_libraries(${name} PRIVATE $<TARGET_FILE:openpete_rhi> openpete_sched_noise m)
endfunction()
if(OPENPETE_SCHED_HAVE_TSAN)
    present_sched_noise(present_sched_tsan present_sched_ring48 ${PT_GEN}/present_thread.sched.o)
endif()

# Negative controls: each mutant is a unified diff applied to a copy of one protocol file at build
# time (apply_mutant.py refuses a patch that no longer applies, naming the mutant); its binary links
# the mutated copy in place of the original. Mutants of present_thread.c and of the scene ring.
set(PT_MUTANTS_PT claim_by_flag drain_no_release drain_no_busy_wait drain_release_before_claim
    drain_no_flush busy_exit_no_signal kick_no_signal)
set(PT_MUTANTS_RING retire_early census_no_presented)
# The race detector's controls (present_thread.c, noise build only): a lock removed around a protocol access.
set(PT_MUTANTS_NOISE)
if(OPENPETE_SCHED_HAVE_TSAN)
    set(PT_MUTANTS_NOISE engage_unlocked)
endif()
set(PT_MUT_BIN ${CMAKE_CURRENT_BINARY_DIR}/mutants)
file(MAKE_DIRECTORY ${PT_MUT_BIN})
set(PT_MUTANT_TARGETS "")
foreach(m IN LISTS PT_MUTANTS_PT PT_MUTANTS_RING PT_MUTANTS_NOISE)
    set(mgen ${PT_GEN}/${m})
    file(MAKE_DIRECTORY ${mgen})
    if(m IN_LIST PT_MUTANTS_PT OR m IN_LIST PT_MUTANTS_NOISE)
        set(msrc ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c)
        set(mout ${mgen}/present_thread.c)
    else()
        set(msrc ${CMAKE_SOURCE_DIR}/core/renderer/native_render.c)
        set(mout ${mgen}/native_render.c)
    endif()
    add_custom_command(
        OUTPUT ${mout}
        COMMAND ${Python3_EXECUTABLE} ${PT_DIR}/apply_mutant.py ${m} ${msrc} ${PT_DIR}/mutants/${m}.patch ${mout}
        DEPENDS ${PT_DIR}/apply_mutant.py ${PT_DIR}/mutants/${m}.patch ${msrc}
        COMMENT "present_sched: mutant ${m}"
        VERBATIM)
    if(m IN_LIST PT_MUTANTS_PT OR m IN_LIST PT_MUTANTS_NOISE)
        add_library(present_sched_pt_${m} OBJECT ${mout})
        target_include_directories(present_sched_pt_${m} PRIVATE ${PT_INCLUDES} ${CMAKE_SOURCE_DIR}/core/runtime)
        target_compile_definitions(present_sched_pt_${m} PRIVATE VK_NO_PROTOTYPES OPENPETE_ENGINE_BUILD)
        target_compile_options(present_sched_pt_${m} PRIVATE ${PT_TSAN})
        target_link_libraries(present_sched_pt_${m} PRIVATE SDL3::Headers)
        set_target_properties(present_sched_pt_${m} PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
        add_custom_command(
            OUTPUT ${mgen}/present_thread.sched.o
            COMMAND ${CMAKE_OBJCOPY} --globalize-symbol=s_pt --globalize-symbol=s_staged --globalize-symbol=s_lat
                    $<TARGET_OBJECTS:present_sched_pt_${m}> ${mgen}/present_thread.sched.o
            DEPENDS present_sched_pt_${m} $<TARGET_OBJECTS:present_sched_pt_${m}>
            COMMAND_EXPAND_LISTS VERBATIM)
        set_source_files_properties(${mgen}/present_thread.sched.o PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        if(m IN_LIST PT_MUTANTS_NOISE)
            present_sched_noise(present_sched_tsan_${m} present_sched_ring48 ${mgen}/present_thread.sched.o)
            set_target_properties(present_sched_tsan_${m} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PT_MUT_BIN})
            add_dependencies(present_sched_tsan present_sched_tsan_${m})
            continue()
        endif()
        present_sched_harness(present_sched_${m} 48 present_sched_ring48 ${mgen}/present_thread.sched.o)
    else()
        add_custom_command(
            OUTPUT ${mgen}/ring_extract.c ${mgen}/pt_state.h
            COMMAND ${Python3_EXECUTABLE} ${PT_DIR}/extract_ring.py
                    ${mout} ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c
                    ${mgen}/ring_extract.c ${mgen}/pt_state.h
            DEPENDS ${PT_DIR}/extract_ring.py ${mout} ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c
            VERBATIM)
        present_sched_ring(present_sched_ring_${m} 48 ${mgen}/ring_extract.c)
        present_sched_harness(present_sched_${m} 48 present_sched_ring_${m} ${PT_GEN}/present_thread.sched.o)
    endif()
    set_target_properties(present_sched_${m} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${PT_MUT_BIN})
    list(APPEND PT_MUTANT_TARGETS present_sched_${m})
endforeach()
add_custom_target(present_sched_mutants DEPENDS ${PT_MUTANT_TARGETS})
add_dependencies(present_sched present_sched_mutants)

add_test(NAME present_sched
         COMMAND present_sched --out ${CMAKE_CURRENT_BINARY_DIR}/run
                 --sub $<TARGET_FILE:present_sched_r3> --mutants ${PT_MUT_BIN})

# The upkeep fence (scripts/fences/sched_seams.py): the protocol objects' synchronisation calls are
# wrapped, their atomics known to the harness, the ring extractable, the compose stand-in equal to
# render_frame_common's protocol events, no source seam, every handoff client modelled. Reruns when a
# protocol file, the harness, the stand-in or the fence changes.
set(PT_SEAMS_ARGS
    --root ${CMAKE_SOURCE_DIR} --db ${CMAKE_BINARY_DIR}/compile_commands.json --nm ${CMAKE_NM}
    --objects ${PT_GEN}/present_thread.sched.o ${PT_GEN}/rhi_vulkan.sched.o $<TARGET_OBJECTS:present_sched_ring48>
    --harness-objects $<TARGET_OBJECTS:present_sched_obj48> $<TARGET_FILE:openpete_sched>
    --wraps "${SCHED_SDL_WRAPS};rhi_display_claim" --ring ${PT_GEN}/ring_extract.c)
add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/sched_seams.stamp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/fences/sched_seams.py check ${PT_SEAMS_ARGS}
            --stamp ${CMAKE_CURRENT_BINARY_DIR}/sched_seams.stamp
    DEPENDS ${CMAKE_SOURCE_DIR}/scripts/fences/sched_seams.py ${CMAKE_SOURCE_DIR}/scripts/fences/seam_guard.py
            ${PT_DIR}/harness.c ${PT_DIR}/ring_epilogue.h ${PT_DIR}/extract_ring.py
            ${CMAKE_SOURCE_DIR}/core/runtime/present_thread.c ${CMAKE_SOURCE_DIR}/core/renderer/native_render.c
            ${CMAKE_SOURCE_DIR}/core/rhi/rhi_vulkan.c
            ${PT_GEN}/present_thread.sched.o ${PT_GEN}/rhi_vulkan.sched.o
            present_sched_ring48 present_sched_obj48 openpete_sched
    COMMENT "present_sched: upkeep fence (sched_seams.py)"
    COMMAND_EXPAND_LISTS VERBATIM)
# In ALL under linux-release only, like the libclang ratchets (core/CMakeLists.txt): the fence needs
# the python clang bindings, which the release container's image does not carry. Elsewhere the target
# builds on request and the ctest sched_seams skips (64) without the bindings.
get_filename_component(_pt_bindir_name "${CMAKE_BINARY_DIR}" NAME)
set(_pt_seams_all "")
if(_pt_bindir_name STREQUAL "linux-release")
    set(_pt_seams_all ALL)
endif()
add_custom_target(present_sched_seams ${_pt_seams_all} DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/sched_seams.stamp)
add_test(NAME sched_seams
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/fences/sched_seams.py check ${PT_SEAMS_ARGS}
         COMMAND_EXPAND_LISTS)
set_tests_properties(sched_seams PROPERTIES LABELS "fence" SKIP_RETURN_CODE 64)
add_test(NAME sched_seams_selftest
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/fences/tests/test_sched_seams.py)
set_tests_properties(sched_seams_selftest PROPERTIES LABELS "fence" SKIP_RETURN_CODE 64)

# The PCT soak (label slow): every scenario of the shipped ring and the random_clients scenario,
# 2,000-step change-point range, ten minutes, base seed from the date; run at the end of a change to
# a protocol file.
add_test(NAME present_sched_soak
         COMMAND present_sched --out ${CMAKE_CURRENT_BINARY_DIR}/soak --soak 600)
set_tests_properties(present_sched_soak PROPERTIES LABELS "slow" TIMEOUT 900)

# The race-detector soak (label slow): PCT over every scenario of the shipped ring and random_clients
# under the real ThreadSanitizer, every report classified by its sched: marker or failing the schedule;
# then the control engage_unlocked's soak must report its race, or the run is VACUOUS. The sanitizer's
# shadow layout needs the kernel's address-space randomisation off for the process (setarch -R).
if(OPENPETE_SCHED_HAVE_TSAN)
    add_test(NAME present_sched_tsan
             COMMAND setarch ${CMAKE_SYSTEM_PROCESSOR} -R $<TARGET_FILE:present_sched_tsan>
                     --out ${CMAKE_CURRENT_BINARY_DIR}/tsan --soak 60
                     --tsan-control $<TARGET_FILE:present_sched_tsan_engage_unlocked>)
    set_tests_properties(present_sched_tsan PROPERTIES LABELS "slow" TIMEOUT 300)
endif()
