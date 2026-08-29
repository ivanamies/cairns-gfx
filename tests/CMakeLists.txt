# tests/CMakeLists.txt
#
# Two targets, on purpose:
#
#   cairns_spec_tests   -- Tier S. Pure CPU. Links nothing from the engine; it
#                          compiles a handful of headers + Catch2 + the
#                          offset_allocator TU that cpu_pool.hpp depends on.
#                          Builds and runs identically on macOS Metal / macOS vk
#                          / Android NDK / iOS simulator because it never
#                          touches a backend. This is the target the TDD relay
#                          implements against, and the one that must build
#                          before a single line of renderer exists.
#
#   cairns_golden_tests -- Tier G. Links cairns_core (backend compile-selected
#                          via -DCAIRNS_GFX_BACKEND). Drives RenderHeadlessFrame
#                          + ReadFinalTargetRgba. Off by default; turn on with
#                          -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON once engine
#                          enablers (Phase 3) are wired.

include(FetchContent)

# Catch2 v3 -- single-header pivot, plainer CMake than gtest.
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
    GIT_SHALLOW    TRUE)

# Cross-compile targets (Android NDK / iOS sim) can't easily resolve a
# co-located libCatch2.so at exec time -- force static Catch2 there so the
# spec binary is self-contained, push-and-run.
if(CMAKE_CROSSCOMPILING)
    set(_cairns_saved_build_shared "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Catch2 static for cross builds" FORCE)
endif()
FetchContent_MakeAvailable(Catch2)
if(CMAKE_CROSSCOMPILING)
    set(BUILD_SHARED_LIBS "${_cairns_saved_build_shared}" CACHE BOOL "" FORCE)
endif()
list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)

# ---- Tier S: pure spec tests (no engine, no GPU) --------------------------
add_executable(cairns_spec_tests
    # allocators (impl order 1)
    test_handle.cpp
    test_range_pool.cpp
    test_frame_budget.cpp
    # profiling (2)
    test_hud_stats.cpp
    # cameras (4)
    test_frustum_cull.cpp
    # particles (5)
    test_particle_emitter.cpp
    # render graph (6)
    test_draw_key.cpp
    test_render_graph_schedule.cpp
    # scene data layer (8)
    test_transform.cpp
    # cpu_pool.hpp pulls in offset_allocator.hpp + needs its TU for the
    # allocator implementation. Compile that one file directly into the spec
    # target so we don't link the engine library.
    "${CMAKE_SOURCE_DIR}/src/util/offset_allocator.cpp")

target_include_directories(cairns_spec_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/glm")

target_compile_features(cairns_spec_tests PRIVATE cxx_std_20)

# Asserts STAY ON. They are the cheapest tripwire for the #228 reuse-not-reset
# / #222 dangling-Hot* class of bug, and a test suite that disables them is
# blind to exactly what hurt us. No spec test deliberately trips an assert.
target_compile_options(cairns_spec_tests PRIVATE -UNDEBUG)

target_link_libraries(cairns_spec_tests PRIVATE Catch2::Catch2WithMain)

# CTest discovery -- one ctest entry per SCENARIO, labelled [spec] so
# `ctest -L spec` picks the lot. POST_BUILD discovery runs the binary on the
# host to enumerate tests; on cross-compiles (Android NDK / iOS device) that
# fails because the binary isn't a host binary. Use PRE_TEST there so
# discovery happens only when ctest actually runs (on-device).
if(CMAKE_CROSSCOMPILING)
    catch_discover_tests(cairns_spec_tests
        PROPERTIES LABELS "spec"
        DISCOVERY_MODE PRE_TEST)
else()
    catch_discover_tests(cairns_spec_tests PROPERTIES LABELS "spec")
endif()

# ---- Tier G: golden ladder (links the engine; backend-selected) -----------
# Off by default. Phase 3 lands the engine enablers + test_seams; this option
# turns the target on once those are real.
option(CAIRNS_GFX_BUILD_GOLDEN_TESTS
    "Build cairns_golden_tests (links cairns_core; backend-selected)" OFF)
if(CAIRNS_GFX_BUILD_GOLDEN_TESTS)
    add_executable(cairns_golden_tests
        test_golden_ladder.cpp
        test_golden_scenarios.cpp
        test_seams.cpp
        test_refs.cpp
        test_md5.cpp)
    target_include_directories(cairns_golden_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/third_party/glm"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(cairns_golden_tests PRIVATE cxx_std_20)
    target_link_libraries(cairns_golden_tests PRIVATE
        cairns_core Catch2::Catch2WithMain)
    catch_discover_tests(cairns_golden_tests PROPERTIES LABELS "golden")
endif()
