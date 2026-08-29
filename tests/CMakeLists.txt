# tests/CMakeLists.txt
#
# Two targets, on purpose:
#
#   cairns_spec_tests   -- Tier S. Pure CPU. Links nothing from the engine; it
#                          compiles a handful of headers + Catch2 + the
#                          offset_allocator TU that cpu_pool.hpp depends on.
#                          Builds and runs identically on macOS Metal / macOS vk
#                          / Android NDK / iOS simulator because it never
#                          touches a backend.
#
#   cairns_golden_tests -- Tier G. Links cairns_core (backend compile-selected
#                          via -DCAIRNS_GFX_BACKEND). Drives RenderHeadlessFrame
#                          + ReadFinalTargetRgba. Off by default; turn on with
#                          -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON.

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
    # allocators
    test_handle.cpp
    test_range_pool.cpp
    test_frame_budget.cpp
    # profiling
    test_hud_stats.cpp
    # cameras
    test_frustum_cull.cpp
    # particles
    test_particle_emitter.cpp
    # render graph
    test_draw_key.cpp
    test_render_graph_schedule.cpp
    # scene data layer
    test_transform.cpp
    # loader material->texture mapping (1:1 desync invariant)
    test_material_map.cpp
    # CPU mip-chain math (the multi-level initial_data blob contract)
    test_texture_mips.cpp
    # scene ECS PODs
    test_scene_ecs.cpp
    # render proxies
    test_render_proxies.cpp
    # multi-scene per-viewport draw fan-out (CarveSceneDrawRanges)
    test_scene_draw_ranges.cpp
    # rhi cross-platform interfaces
    test_rhi_interfaces.cpp
    # rhi backend boot-cap / offset×16
    test_rhi_backends.cpp
    # top-risk consolidation
    test_top_risks.cpp
    # threading SPSC invariants
    test_threading.cpp
    # asset/GPU PODs (pure CPU)
    test_asset_gpu_pods.cpp
    # MemoryBudget + ChunkAllocator fixed reservation
    test_memory_budget.cpp
    # control / agent CommandRegistry round-trip
    test_command_registry.cpp
    "${CMAKE_SOURCE_DIR}/src/control/command_registry.cpp"
    # cpu_pool.hpp pulls in offset_allocator.hpp + needs its TU for the
    # allocator implementation. Compile that one file directly into the spec
    # target so we don't link the engine library.
    "${CMAKE_SOURCE_DIR}/src/util/offset_allocator.cpp")

target_include_directories(cairns_spec_tests PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/glm"
    "${CMAKE_SOURCE_DIR}/third_party/entt/single_include"
    "${CMAKE_SOURCE_DIR}/third_party/entt"
    "${CMAKE_SOURCE_DIR}/third_party"
    "${CMAKE_BINARY_DIR}/generated")

target_compile_features(cairns_spec_tests PRIVATE cxx_std_20)

# Asserts STAY ON. They are the cheapest tripwire for the reuse-not-reset /
# dangling-Hot* class of bug, and a test suite that disables them is blind to
# exactly what hurt us. No spec test deliberately trips an assert.
target_compile_options(cairns_spec_tests PRIVATE -UNDEBUG)

target_link_libraries(cairns_spec_tests PRIVATE Catch2::Catch2WithMain)

# CTest discovery -- one ctest entry per SCENARIO, labelled [spec] so
# `ctest -L spec` picks the lot. POST_BUILD discovery runs the binary on the
# host to enumerate tests; on cross-compiles (Android NDK / iOS device) that
# fails because the binary isn't a host binary. Use PRE_TEST there so
# discovery happens only when ctest actually runs (on-device).
# SKIP_RETURN_CODE 4: Catch2 exits 4 when every selected test SKIPs; without
# the property ctest reports an honest SKIP as a failure.
if(CMAKE_CROSSCOMPILING)
    catch_discover_tests(cairns_spec_tests
        PROPERTIES LABELS "spec" SKIP_RETURN_CODE 4
        DISCOVERY_MODE PRE_TEST)
else()
    catch_discover_tests(cairns_spec_tests
        PROPERTIES LABELS "spec" SKIP_RETURN_CODE 4)
endif()

# ---- Tier G: golden ladder (links the engine; backend-selected) -----------
# Off by default -- golden runs need a built backend + assets.
option(CAIRNS_GFX_BUILD_GOLDEN_TESTS
    "Build cairns_golden_tests (links cairns_core; backend-selected)" OFF)
if(CAIRNS_GFX_BUILD_GOLDEN_TESTS)
    add_executable(cairns_golden_tests
        test_golden_scenarios.cpp
        test_golden_stress.cpp
        test_npr_properties.cpp
        test_state_hash.cpp
        test_js_smoke.cpp
        test_seams.cpp
        test_refs.cpp
        test_md5.cpp)
    target_include_directories(cairns_golden_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/third_party/glm"
        "${CMAKE_SOURCE_DIR}/third_party/entt"
        "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_features(cairns_golden_tests PRIVATE cxx_std_20)
    # tests/refs/ baseline path -- the refs loader checks
    # $CAIRNS_TEST_REFS_DIR first, then this compile-time fallback. Each
    # run-from-build-dir CI invocation can override via the env var.
    target_compile_definitions(cairns_golden_tests PRIVATE
        CAIRNS_TEST_REFS_DIR_DEFINE=\"${CMAKE_CURRENT_SOURCE_DIR}/refs\")
    # cairns_control holds the command registry + QuickJS host so scenarios are
    # composed in JS (cairns.dispatch) instead of bespoke C++ seams.
    target_link_libraries(cairns_golden_tests PRIVATE
        cairns_core cairns_control Catch2::Catch2WithMain)
    if(CMAKE_CROSSCOMPILING)
        catch_discover_tests(cairns_golden_tests
            PROPERTIES LABELS "golden" SKIP_RETURN_CODE 4
            DISCOVERY_MODE PRE_TEST)
    else()
        catch_discover_tests(cairns_golden_tests
            PROPERTIES LABELS "golden" SKIP_RETURN_CODE 4)
    endif()

    # Asset bundling -- mirrors the root CMakeLists.txt EXECUTABLE_NAME path
    # but scoped to the cairns_golden_tests target. iOS+macOS use
    # MACOSX_PACKAGE_LOCATION (Resources/); Android uses POST_BUILD copy into
    # MOBILE_ASSETS_DIR; other Unixy targets land assets next to the binary.
    set(_cairns_assets_root "${CMAKE_SOURCE_DIR}/assets")
    set(_cairns_ext_glbs_root
        "/Users/ivanamies/dev/gfx/assets/riot/lol_champions/staging")
    # CONFIGURE_DEPENDS: re-glob when the asset/staging dirs change (a swapped
    # champion GLB would otherwise leave a stale copy-list -> build fails
    # copying a now-missing file).
    file(GLOB _cairns_metal_shaders CONFIGURE_DEPENDS "${_cairns_assets_root}/*.metal")
    file(GLOB _cairns_spv_shaders   CONFIGURE_DEPENDS "${_cairns_assets_root}/*.spv")
    file(GLOB _cairns_wgsl_shaders  CONFIGURE_DEPENDS "${_cairns_assets_root}/*.wgsl")
    file(GLOB _cairns_glbs          CONFIGURE_DEPENDS "${_cairns_assets_root}/*.glb")
    file(GLOB _cairns_ext_glbs      CONFIGURE_DEPENDS "${_cairns_ext_glbs_root}/*.glb")
    set(_cairns_misc_assets
        "${_cairns_assets_root}/run.js"
        "${_cairns_assets_root}/Inter-VariableFont.ttf"
        "${_cairns_assets_root}/wc_paper_noise.png")

    if(APPLE AND IOS)
        # iOS .app: tag every asset MACOSX_PACKAGE_LOCATION Resources so it
        # lands inside the .app/Resources directory. xcrun simctl spawn
        # finds them via SDL_GetBasePath = the .app root.
        macro(_cairns_bundle_one FILE)
            if(EXISTS "${FILE}")
                target_sources(cairns_golden_tests PRIVATE "${FILE}")
                set_property(SOURCE "${FILE}" PROPERTY
                             MACOSX_PACKAGE_LOCATION "Resources")
                set_source_files_properties("${FILE}" PROPERTIES
                    HEADER_FILE_ONLY TRUE)
            endif()
        endmacro()
        foreach(F ${_cairns_misc_assets} ${_cairns_metal_shaders}
                  ${_cairns_spv_shaders} ${_cairns_glbs} ${_cairns_ext_glbs})
            _cairns_bundle_one("${F}")
        endforeach()
    elseif(APPLE)
        # macOS desktop -- cairns_golden_tests is NOT a .app bundle, so
        # MACOSX_PACKAGE_LOCATION doesn't apply. Copy assets next to the
        # binary instead (SDL_GetBasePath returns its directory).
        foreach(F ${_cairns_misc_assets} ${_cairns_metal_shaders}
                  ${_cairns_spv_shaders} ${_cairns_wgsl_shaders}
                  ${_cairns_glbs} ${_cairns_ext_glbs})
            get_filename_component(_name "${F}" NAME)
            if(EXISTS "${F}")
                add_custom_command(POST_BUILD TARGET cairns_golden_tests
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${F}" "$<TARGET_FILE_DIR:cairns_golden_tests>/${_name}")
            endif()
        endforeach()
    elseif(ANDROID)
        # Push assets alongside the binary at MOBILE_ASSETS_DIR (also used by
        # spec target's MOBILE_ASSETS_DIR config). The actual `adb push` is
        # done by the test runner; CMake just stages a known directory.
        foreach(F ${_cairns_misc_assets} ${_cairns_spv_shaders}
                  ${_cairns_glbs} ${_cairns_ext_glbs})
            get_filename_component(_name "${F}" NAME)
            if(EXISTS "${F}")
                add_custom_command(POST_BUILD TARGET cairns_golden_tests
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${F}" "${MOBILE_ASSETS_DIR}/${_name}")
            endif()
        endforeach()
    else()
        # Plain Linux / desktop fallback: stage assets alongside the binary.
        foreach(F ${_cairns_misc_assets} ${_cairns_spv_shaders}
                  ${_cairns_glbs} ${_cairns_ext_glbs})
            get_filename_component(_name "${F}" NAME)
            if(EXISTS "${F}")
                add_custom_command(POST_BUILD TARGET cairns_golden_tests
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${F}" "$<TARGET_FILE_DIR:cairns_golden_tests>/${_name}")
            endif()
        endforeach()
    endif()
endif()
