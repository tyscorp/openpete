# tests/sched - deterministic interleaving scheduler (lib/) and its
# calibration on toy programs (toys/).
#
# The scheduler replaces SDL's synchronisation, thread and clock calls by
# -Wl,--wrap and the ThreadSanitizer runtime by its own __tsan_* entry
# points: code under test is compiled with -fsanitize=thread, libtsan is
# never linked, and SDL itself is not linked either, so a call the wraps do
# not cover is a link error. Linux only (GNU ld --wrap).
if(WIN32 OR NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    return()
endif()

cmake_language(DEFER CALL openpete_label_directory_tests unit)
find_package(Threads REQUIRED)

# No link-time optimization for any target here or in present/. GNU ld
# applies --wrap outside the GCC LTO plugin's symbol resolution: the plugin
# sees the reference to SDL_LockMutex resolved and __wrap_SDL_LockMutex
# referenced by no object (PREVAILING_DEF_IRONLY), drops it, and the renamed
# reference is undefined at the final link; when the real definition is IR
# as well, the call binds to it and skips the wrap without an error. A
# regular object's reference is wrapped whether the definition it reaches
# is regular or IR, so an IR archive (openpete_rhi under a preset with IPO
# on) may be linked here while no wrapped symbol has both an IR caller and
# an IR definition.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

set(SCHED_SDL_WRAPS
    SDL_CreateMutex SDL_DestroyMutex SDL_LockMutex SDL_TryLockMutex SDL_UnlockMutex
    SDL_CreateCondition SDL_DestroyCondition SDL_SignalCondition SDL_BroadcastCondition
    SDL_WaitCondition SDL_WaitConditionTimeout
    SDL_CreateThreadRuntime SDL_WaitThread SDL_DetachThread SDL_GetCurrentThreadID
    SDL_GetTicksNS SDL_GetTicks SDL_DelayNS SDL_Delay
    SDL_GetAtomicInt SDL_SetAtomicInt SDL_AddAtomicInt SDL_CompareAndSwapAtomicInt)
set(SCHED_WRAP_LINK "")
foreach(sym IN LISTS SCHED_SDL_WRAPS)
    list(APPEND SCHED_WRAP_LINK "-Wl,--wrap=${sym}")
endforeach()
set(SCHED_WRAP_LINK ${SCHED_WRAP_LINK} PARENT_SCOPE)

# The scheduler, the explorer and the SDL wraps, shared by both runtimes.
add_library(openpete_sched_core OBJECT lib/sched.c lib/explore.c lib/sdl_wraps.c)
target_include_directories(openpete_sched_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_compile_options(openpete_sched_core PRIVATE -Wall -Wextra)
set_target_properties(openpete_sched_core PROPERTIES C_STANDARD 11)
target_link_libraries(openpete_sched_core PUBLIC SDL3::Headers)

# The scripted runtime: the scheduler's own __tsan_* entry points (watched accesses are yields), no libtsan.
add_library(openpete_sched STATIC $<TARGET_OBJECTS:openpete_sched_core> lib/tsan_hooks.c)
target_include_directories(openpete_sched PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
target_compile_options(openpete_sched PRIVATE -Wall -Wextra)
set_target_properties(openpete_sched PROPERTIES C_STANDARD 11)
target_link_libraries(openpete_sched PUBLIC SDL3::Headers Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(openpete_sched INTERFACE ${SCHED_WRAP_LINK})

# The noise runtime: the same scheduler over the real ThreadSanitizer runtime (tsan_noise.c), for the
# race-detector build of a harness. Built where the toolchain links -fsanitize=thread.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS -fsanitize=thread)
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
check_c_source_compiles("int main(void) { return 0; }" OPENPETE_SCHED_HAVE_TSAN)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(OPENPETE_SCHED_HAVE_TSAN)
    add_library(openpete_sched_noise STATIC $<TARGET_OBJECTS:openpete_sched_core> lib/tsan_noise.c)
    target_include_directories(openpete_sched_noise PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
    target_compile_options(openpete_sched_noise PRIVATE -Wall -Wextra)
    set_target_properties(openpete_sched_noise PROPERTIES C_STANDARD 11)
    target_link_libraries(openpete_sched_noise PUBLIC SDL3::Headers Threads::Threads ${CMAKE_DL_LIBS})
    target_link_options(openpete_sched_noise INTERFACE ${SCHED_WRAP_LINK} -fsanitize=thread)
endif()

# The toy whose shared accesses the hooks see.
add_library(sched_toy_tsan OBJECT toys/toy_counter.c)
target_compile_options(sched_toy_tsan PRIVATE -fsanitize=thread -O2)

add_executable(sched_calibrate toys/sched_calibrate.c $<TARGET_OBJECTS:sched_toy_tsan>)
target_include_directories(sched_calibrate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/toys)
target_compile_options(sched_calibrate PRIVATE -Wall -Wextra)
set_target_properties(sched_calibrate PROPERTIES C_STANDARD 11)
target_link_libraries(sched_calibrate PRIVATE openpete_sched)
add_test(NAME sched_calibrate
         COMMAND sched_calibrate ${CMAKE_CURRENT_BINARY_DIR}/calibrate 4)
add_subdirectory(present)
