cmake_minimum_required(VERSION 3.16)

# set the output directory for built objects.
# This makes sure that the dynamic library goes into the build directory automatically.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

# prevent installing to system directories.
set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}" CACHE INTERNAL "")

# Declare the project
project(sdl-min)

# Tests (and everything) build + run in Release by default. Single-config
# generators (Ninja, Make) honor CMAKE_BUILD_TYPE; multi-config (Xcode, VS)
# pick the config at build time via --config Release. The spec target keeps
# asserts on via its own -UNDEBUG, so Release optimization + asserts coexist.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if ((APPLE AND NOT CMAKE_SYSTEM_NAME MATCHES "Darwin") OR EMSCRIPTEN)
    set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")    # Disable shared builds on platforms where it does not make sense to use them
    set(SDL_SHARED OFF)
else()
    set(SDL_SHARED ON)
endif()

if(MSVC)
    if(NOT CMAKE_GENERATOR STREQUAL "Ninja")
        add_definitions(/MP)				# parallelize each target, unless Ninja is the generator
    endif()
endif()

# run.js is a MANDATORY bundled asset on every platform. boot_run.cpp aborts
# at launch if it can't load it, so we fail fast at configure time instead.
if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/run.js")
    message(FATAL_ERROR
        "assets/run.js is missing -- it is the bundled boot script that "
        "loadBatch + instantiateGrid through cairns.dispatch on every "
        "platform. Add it to assets/ or fix the asset pipeline.")
endif()

# GPU backend is a true compile-time feature: pick exactly one.
# No silent default -- forgetting the flag once cost a chunk-bisect session
# (built metal while debugging a vk regression). Hard-fail at configure time.
if(NOT DEFINED CAIRNS_GFX_BACKEND)
  message(FATAL_ERROR
    "CAIRNS_GFX_BACKEND is not set. Pass -DCAIRNS_GFX_BACKEND=metal "
    "or -DCAIRNS_GFX_BACKEND=vulkan on the cmake configure line.")
endif()
set(CAIRNS_GFX_BACKEND "${CAIRNS_GFX_BACKEND}" CACHE STRING
    "GPU backend: metal or vulkan" FORCE)
set_property(CACHE CAIRNS_GFX_BACKEND PROPERTY STRINGS metal vulkan webgpu)

if(CAIRNS_GFX_BACKEND STREQUAL "metal")
  set(CAIRNS_GFX_BACKEND_METAL 1)
  set(CAIRNS_GFX_BACKEND_VULKAN 0)
  set(CAIRNS_GFX_BACKEND_WEBGPU 0)
elseif(CAIRNS_GFX_BACKEND STREQUAL "vulkan")
  set(CAIRNS_GFX_BACKEND_METAL 0)
  set(CAIRNS_GFX_BACKEND_VULKAN 1)
  set(CAIRNS_GFX_BACKEND_WEBGPU 0)
elseif(CAIRNS_GFX_BACKEND STREQUAL "webgpu")
  set(CAIRNS_GFX_BACKEND_METAL 0)
  set(CAIRNS_GFX_BACKEND_VULKAN 0)
  set(CAIRNS_GFX_BACKEND_WEBGPU 1)
else()
  message(FATAL_ERROR "CAIRNS_GFX_BACKEND must be 'metal', 'vulkan', or 'webgpu', got '${CAIRNS_GFX_BACKEND}'")
endif()

# #229 allocation receipts. When ON, alloc_count.cpp replaces the global
# operator new/delete set to count every heap (de)allocation and the engine
# prints per-phase deltas at the [LOAD]/[RELOAD] fences. Diagnostic only;
# default OFF leaves the operators untouched (zero overhead). Applied globally
# so every TU's allocations route through the counted operators.
option(CAIRNS_GFX_ALLOC_TRACE
  "Count heap allocations and print [ALLOC-RECEIPT] deltas at load/reload fences"
  OFF)
if(CAIRNS_GFX_ALLOC_TRACE)
  add_compile_definitions(CAIRNS_ALLOC_TRACE=1)
  message(STATUS "cairns: allocation tracing ENABLED (CAIRNS_ALLOC_TRACE=1)")
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/gfx_config.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/util/gfx_config.hpp"
  @ONLY)

# =============================================================================
# Third-party dependencies (built first so cairns_core can link them).
# =============================================================================

# fastgltf
set(FASTGLTF_ENABLE_ASSET_DEBUGGER OFF CACHE BOOL "" FORCE)
set(FASTGLTF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fastgltf EXCLUDE_FROM_ALL)

# SDL3 + satellite libs.
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

set(SDLTTF_VENDORED ON)
add_subdirectory(third_party/SDL_ttf EXCLUDE_FROM_ALL)

set(SDLMIXER_MIDI_NATIVE OFF)
set(SDLMIXER_GME OFF)
set(SDLMIXER_WAVPACK OFF)
set(SDLMIXER_MOD OFF)
set(SDLMIXER_MOD_XMP OFF)
set(SDLMIXER_OPUS OFF)
set(SDLMIXER_MP3_MPG123 OFF)
set(SDLMIXER_VORBIS_VORBISFILE OFF)
set(SDLMIXER_FLAC_LIBFLAC OFF)
set(SDLMIXER_VENDORED ON)
add_subdirectory(third_party/SDL_mixer EXCLUDE_FROM_ALL)

set(SDLIMAGE_VENDORED ON)
set(SDLIMAGE_AVIF OFF)
set(SDLIMAGE_BMP OFF)
set(SDLIMAGE_JPEG OFF)
set(SDLIMAGE_WEBP OFF)
add_subdirectory(third_party/SDL_image EXCLUDE_FROM_ALL)

# Dear ImGui: core + the SDL3 platform backend ONLY (input/timing). The GPU
# renderer is our own, through the cairns RHI -- no imgui_impl_metal/vulkan.
# Built as a separate static lib so third-party warnings don't trip the app's
# -Werror; keeps -fno-exceptions/-fno-rtti to match the app ABI.
add_library(imgui STATIC
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
)
target_include_directories(imgui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends"
)
target_compile_features(imgui PUBLIC cxx_std_20)
target_compile_options(imgui PRIVATE -fno-exceptions -fno-rtti)
# The SDL platform backend (input) drives ImGuiIO on every target -- SDL's
# emscripten video driver feeds DOM events into SDL, so the web build uses it
# too; the GPU renderer is our own RHI on all backends.
target_sources(imgui PRIVATE third_party/imgui/backends/imgui_impl_sdl3.cpp)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# QuickJS: vendored copy of quickjs-ng (third_party/QuickJS). Minimal static
# lib over the core C TUs; we don't pull the vendor's CMakeLists because it
# also builds qjs/qjsc/wasm/test executables we don't need. quickjs-libc is
# included so script.eval can use stdlib bindings (console.log etc).
# quickjs-libc binds OS facilities (environ, signals, popen) absent in the
# browser; the engine never calls its helpers (no js_std_add_helpers), so it's
# native-only. The core JS engine (quickjs.c + regexp/unicode/cutils) is enough
# for script.eval.
add_library(quickjs STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuickJS/quickjs.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuickJS/cutils.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuickJS/libregexp.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuickJS/libunicode.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuickJS/xsum.c")
if(NOT EMSCRIPTEN)
    target_sources(quickjs PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuickJS/quickjs-libc.c")
endif()
target_include_directories(quickjs PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/QuickJS")
target_compile_definitions(quickjs PRIVATE
    CONFIG_VERSION="cairns-vendored")
# quickjs's own warnings would trip a -Werror build; relax for its TUs only.
target_compile_options(quickjs PRIVATE -Wno-everything)

# ── wgpu-native (vendored prebuilt C bindings) ──────────────────────────────
# third_party/wgpu-native/ holds the v29.0.0.0 macOS static lib + webgpu.h (see
# its VENDORED.md). INTERFACE target so any consumer pulls the static lib + the
# macOS framework deps + the include dir. Consumed by the WebGPU backend (W2+).
add_library(wgpu_native INTERFACE)
target_include_directories(wgpu_native INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/include")
target_link_libraries(wgpu_native INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/wgpu-native/lib/libwgpu_native.a")
if(APPLE)
  target_link_libraries(wgpu_native INTERFACE
      "-framework Metal" "-framework QuartzCore" "-framework Foundation"
      "-framework IOKit" "-framework IOSurface" "-framework CoreFoundation"
      "-framework Security" "-framework CoreGraphics")
endif()

# ── emdawnwebgpu (browser WebGPU) ───────────────────────────────────────────
# The Emscripten port = Dawn's webgpu.h header + a JS shim forwarding C calls to
# navigator.gpu. Nothing of Dawn's implementation links into us (the impl is
# Chrome's). Replaces wgpu_native for the WASM target; same v29 header dialect.
if(EMSCRIPTEN)
  add_library(wgpu_web INTERFACE)
  target_compile_options(wgpu_web INTERFACE "--use-port=emdawnwebgpu")
  target_link_options(wgpu_web INTERFACE "--use-port=emdawnwebgpu")
endif()

# W0 link smoke (off by default): proves the vendored lib + headers + frameworks
# resolve and a GPU adapter is reachable.
option(CAIRNS_BUILD_WGPU_SMOKE "Build the wgpu-native link smoke" OFF)
if(CAIRNS_BUILD_WGPU_SMOKE)
  add_executable(wgpu_link_smoke
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/wgpu_link_smoke.cpp")
  target_link_libraries(wgpu_link_smoke PRIVATE wgpu_native)
  add_executable(wgpu_readback_smoke
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/wgpu_readback_smoke.cpp")
  target_link_libraries(wgpu_readback_smoke PRIVATE wgpu_native)
endif()

# nlohmann/json uses exceptions internally + has compile-time warnings that
# trip the app's -Werror. Wrap the control-plane TUs (registry + transport +
# handlers) in their own static lib with relaxed flags. Mirrors the
# cairns_render_thread / Taskflow pattern below.
add_library(cairns_control STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/agent_stdin_drain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/boot_run.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/command_registry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/transport_stdio.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/entity_ops.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/lifecycle_ops.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/perf_ops.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/render_ops.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/scene_ops.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/script_ops.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/selection_ops.cpp")
target_include_directories(cairns_control PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/taskflow"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/entt/single_include")
target_compile_features(cairns_control PUBLIC cxx_std_20)
target_compile_options(cairns_control PRIVATE -Wall -Wextra)
target_compile_definitions(cairns_control PRIVATE
    $<$<BOOL:${CAIRNS_GFX_BACKEND_METAL}>:CAIRNS_METAL=1>
    $<$<BOOL:${CAIRNS_GFX_BACKEND_VULKAN}>:CAIRNS_VULKAN=1>
    $<$<BOOL:${CAIRNS_GFX_BACKEND_WEBGPU}>:CAIRNS_WEBGPU=1>)
# render_ops.cpp pulls engine.hpp which transitively needs every header
# cairns_core sees -- glm/stb/entt above, plus the backend framework includes
# and SDL. cairns_core declared its dependencies PUBLIC so linking it here is
# enough to surface them at compile time without circularity (cairns_core
# does not include or link cairns_control).
target_link_libraries(cairns_control PRIVATE cairns_core quickjs)

# Taskflow needs exceptions + has size_t->int narrowing in its headers, so the
# one TU that includes it lives in its own static library with relaxed flags.
add_library(cairns_render_thread STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/render/render_thread.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/util/worker_pool.cpp")
target_include_directories(cairns_render_thread PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/taskflow"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/entt/single_include"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm")
target_compile_features(cairns_render_thread PUBLIC cxx_std_20)
target_compile_options(cairns_render_thread PRIVATE -Wall -Wextra)
target_compile_definitions(cairns_render_thread PRIVATE
    $<$<BOOL:${CAIRNS_GFX_BACKEND_METAL}>:CAIRNS_METAL=1>
    $<$<BOOL:${CAIRNS_GFX_BACKEND_VULKAN}>:CAIRNS_VULKAN=1>
    $<$<BOOL:${CAIRNS_GFX_BACKEND_WEBGPU}>:CAIRNS_WEBGPU=1>)
if (CAIRNS_GFX_BACKEND_VULKAN AND APPLE)
    target_link_libraries(cairns_render_thread PRIVATE Vulkan::Vulkan)
endif()
if (CAIRNS_GFX_BACKEND_WEBGPU)
    if(EMSCRIPTEN)
        target_link_libraries(cairns_render_thread PRIVATE wgpu_web)
    else()
        target_link_libraries(cairns_render_thread PRIVATE wgpu_native)
    endif()
endif()

# =============================================================================
# cairns_core -- the engine library. Everything in src/ except the entry-point
# translation units (main.cpp for the SDL app, serve_main.cpp for the headless
# host) and the already-separated render_thread.cpp.
#
# P0a: still transitively links SDL because rhi/vulkan/device.cpp and
# rhi/swap_chain.hpp (Metal) call SDL_* directly. P0b will lift those out so
# cairns_core no longer needs SDL.
# =============================================================================

file(GLOB_RECURSE CAIRNS_CORE_SOURCES CONFIGURE_DEPENDS
  "src/*.cpp"
  "src/*.hpp"
)
list(FILTER CAIRNS_CORE_SOURCES EXCLUDE REGEX "/src/control/")
# Exclude:
#  - entry points (main.cpp, serve_main.cpp)
#  - render_thread.cpp / worker_pool.cpp (separate lib, exceptions enabled
#    because taskflow needs them)
#  - src/control/** (lives in cairns_control STATIC lib; nlohmann/json wants
#    exceptions which cairns_core forbids)
list(REMOVE_ITEM CAIRNS_CORE_SOURCES
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/serve_main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/web/web_main.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/render/render_thread.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/worker_pool.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/command_registry.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/command_registry.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/transport_stdio.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/transport_stdio.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/lifecycle_ops.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/lifecycle_ops.hpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/render_ops.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/control/handlers/render_ops.hpp"
)

add_library(cairns_core STATIC ${CAIRNS_CORE_SOURCES})

# PRIVATE -- these compile options govern cairns_core's own TUs. They must
# NOT be inherited by consumers: cairns_control links cairns_core and uses
# nlohmann/json (needs exceptions). Apps (sdl-min, cairns_serve) carry their
# own compile options on their main TU.
target_compile_options(cairns_core PRIVATE
  -Wall
  -Wextra
  -Werror
  -fno-exceptions
  -fno-rtti
)
target_compile_features(cairns_core PUBLIC cxx_std_20)

target_include_directories(cairns_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/taskflow"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/entt/single_include"
)

# Metal frameworks.
if(CAIRNS_GFX_BACKEND_METAL)
    message("tagiamies using metal")
    target_include_directories(cairns_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/metal-cpp")
    find_library(METAL_LIB Metal REQUIRED)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    find_library(QUARTZ_LIB QuartzCore REQUIRED)
    target_link_libraries(cairns_core PUBLIC
        ${METAL_LIB} ${FOUNDATION_LIB} ${QUARTZ_LIB})
endif()

# Vulkan.
if(CAIRNS_GFX_BACKEND_VULKAN)
    if (APPLE)
        message("tagiamies using vulkan too!!")
        find_package(Vulkan REQUIRED)
        target_link_libraries(cairns_core PUBLIC Vulkan::Vulkan)
    elseif (ANDROID)
        target_link_libraries(cairns_core PUBLIC vulkan android log)
    endif()
endif()

# WebGPU (wgpu-native). Headless renders offscreen; a surface is created only in
# a windowed/web build. cairns_core links the vendored static lib + its macOS
# framework deps via the wgpu_native INTERFACE target (which also adds the
# webgpu.h include dir).
if(CAIRNS_GFX_BACKEND_WEBGPU)
    if(EMSCRIPTEN)
        target_link_libraries(cairns_core PUBLIC wgpu_web)
    else()
        target_link_libraries(cairns_core PUBLIC wgpu_native)
    endif()
endif()

# Third-party link surface. Core SDL (window/input/timer/IO/filesystem) links on
# EVERY target including emscripten -- asset IO, the scenario picker, and shader
# loads all route through SDL. The satellites (audio, fonts, image codecs) stay
# native-only; the web build has no audio path and loads its own assets.
target_link_libraries(cairns_core PUBLIC
    fastgltf::fastgltf
    imgui
    cairns_render_thread
    SDL3::SDL3
)
if(NOT EMSCRIPTEN)
    target_link_libraries(cairns_core PUBLIC
        SDL3_ttf::SDL3_ttf
        SDL3_mixer::SDL3_mixer
        SDL3_image::SDL3_image
    )
endif()

# SDL_Image bug: https://github.com/libsdl-org/SDL_image/issues/506
if (APPLE AND NOT BUILD_SHARED_LIBS)
    find_library(IO_LIB ImageIO REQUIRED)
    find_library(CS_LIB CoreServices REQUIRED)
    find_library(CT_LIB CoreText REQUIRED)
    find_library(CG_LIB CoreGraphics REQUIRED)
    find_library(CF_LIB CoreFoundation REQUIRED)
    target_link_libraries(cairns_core PUBLIC
        ${CF_LIB} ${CT_LIB} ${IO_LIB} ${CS_LIB} ${CG_LIB})
endif()

# =============================================================================
# sdl-min -- the SDL3 interactive app. Thin shell over cairns_core.
#
# Keeps the project name so the existing verify_metal.sh / verify_vk.sh /
# regenerate_golden.sh scripts continue to find the binary.
# =============================================================================

set(EXECUTABLE_NAME ${PROJECT_NAME})

if (ANDROID)
    # The SDL java code is hardcoded to load libmain.so on android.
    set(EXECUTABLE_NAME main)
    add_library(${EXECUTABLE_NAME} SHARED)
else()
    add_executable(${EXECUTABLE_NAME})
endif()

target_sources(${EXECUTABLE_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)
target_link_libraries(${EXECUTABLE_NAME} PRIVATE cairns_core)

# To get an app icon on Apple platforms, add it to your executable.
if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png")
    target_sources("${EXECUTABLE_NAME}" PRIVATE "assets/logo.png")
endif()

# on Web targets, we need CMake to generate a HTML webpage.
if(EMSCRIPTEN)
    set(CMAKE_EXECUTABLE_SUFFIX ".html" CACHE INTERNAL "")
endif()

# =============================================================================
# cairns_web -- the browser entry (WebGPU via emdawnwebgpu, no SDL). Renders the
# scenarios into the offscreen final_target_ and copies it to the <canvas>; the
# scene is driven from JS via window.cairns.dispatch -> the CommandRegistry.
# =============================================================================
if(EMSCRIPTEN AND CAIRNS_GFX_BACKEND_WEBGPU)
    add_executable(cairns_web "${CMAKE_CURRENT_SOURCE_DIR}/src/web/web_main.cpp")
    target_link_libraries(cairns_web PRIVATE cairns_core cairns_control wgpu_web)
    target_compile_definitions(cairns_web PRIVATE CAIRNS_WEBGPU=1)
    set_target_properties(cairns_web PROPERTIES SUFFIX ".html")
    target_link_options(cairns_web PRIVATE
        "-sENVIRONMENT=web"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=268435456"            # 256 MB; engine reserves CPU blocks
        "-sSTACK_SIZE=4MB"
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString"
        "-sASSERTIONS=1"
        # LAZY GLB LOADING: champion GLBs are NOT bundled into the .data (that put
        # ~0.9-1.2 GB resident in MEMFS and OOM-killed the tab). They're served as
        # loose files (symlinked into the output dir below) and fetched ON DEMAND in
        # platform_web.cpp ReadAsset -- so only ~one GLB (~18 MB) is resident at a
        # time, baseline ~40 MB. ASYNCIFY lets the synchronous loader await the
        # fetch (emscripten_wget_data); the deep QuickJS+load stack needs the larger
        # asyncify stack.
        "-sASYNCIFY"
        "-sASYNCIFY_STACK_SIZE=1048576"
        # NOTE: NOT --use-preload-cache -- emscripten stores the whole package as a
        # single IndexedDB value, which silently fails at large sizes. The (now
        # small) .data is cached via a service worker (Cache API); the loose GLBs
        # are browser-HTTP-cached. See the POST_BUILD sw.js stamp + src/web/sw.js.in.
        # The 100/300-actor perf smokes are kept out of the WEB build (web ships only
        # perf_smoke_50); native still has them.
        "--exclude-file=*perf_smoke_100*"
        "--exclude-file=*perf_smoke_300*"
        "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/src/web/shell.html"
        "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/assets@/")
    # Serve the champion GLBs as loose files (lazy-fetched, NOT bundled). Symlink
    # the whole staging roster into the output dir next to cairns_web.html so the
    # dev http server hands them out on demand. No disk duplication, no MEMFS cost.
    add_custom_command(TARGET cairns_web POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSTAGING=/Users/ivanamies/dev/gfx/assets/riot/lol_champions/staging
            -DOUT=$<TARGET_FILE_DIR:cairns_web>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/link_glbs.cmake
        VERBATIM)
    # Stamp sw.js next to cairns_web.html with the built .data's content hash as
    # the Cache API version, so a rebuilt bundle auto-invalidates the cache.
    add_custom_command(TARGET cairns_web POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/src/web/sw.js.in
            -DOUT=$<TARGET_FILE_DIR:cairns_web>/sw.js
            -DDATA=$<TARGET_FILE_DIR:cairns_web>/cairns_web.data
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stamp_sw.cmake
        VERBATIM)
endif()

# =============================================================================
# cairns_serve -- headless NDJSON control plane host. No SDL window, no main
# loop -- a stdin/stdout NDJSON dispatch loop. Built on desktop only.
#
# P0a: bare echo loop. Transitively links SDL via cairns_core (RHI still calls
# SDL_*); P0b lifts those out and `otool -L cairns_serve` will show zero SDL3.
# =============================================================================

if (NOT ANDROID AND NOT IOS AND NOT EMSCRIPTEN)
    add_executable(cairns_serve
        "${CMAKE_CURRENT_SOURCE_DIR}/src/serve_main.cpp"
    )
    target_link_libraries(cairns_serve PRIVATE cairns_core cairns_control)

    # Shaders + a small smoke-test GLB copied next to the cairns_serve binary
    # so the engine's SDL_GetBasePath()-based asset lookup resolves. The
    # sdl-min path puts them inside the .app bundle's Resources/ dir, but
    # cairns_serve is unbundled and SDL_GetBasePath returns the binary's dir.
    set(_cairns_serve_assets
        "run.js"                                 # mandatory boot script
        "unlit.vert.spv" "unlit.frag.spv"
        "unlit_noid.frag.spv"                    # #222 Phase A.1
        "particle.vert.spv" "particle.frag.spv" "particle.comp.spv"
        "imgui.vert.spv" "imgui.frag.spv"
        "composite_pip.vert.spv" "composite_pip.frag.spv"
        "depthviz.frag.spv"
        "outline.vert.spv" "outline.frag.spv"  # #207
        "skin.comp.spv"                          # #221 Phase 9
        "anim_eval.comp.spv"                     # #221 Phase 5b
    )
    if (APPLE)
        # Metal needs the .metal source files (compile_metal_library reads
        # them via fopen at runtime). Glob to catch all of them.
        file(GLOB _cairns_serve_metal_files
            RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.metal")
        list(APPEND _cairns_serve_assets ${_cairns_serve_metal_files})
    endif()
    # WebGPU loads .wgsl source at runtime (CreateGraphicsPipeline reads the
    # file). Backend-agnostic glob; the copy is a no-op for metal/vk builds.
    file(GLOB _cairns_serve_wgsl_files
        RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/assets"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/*.wgsl")
    list(APPEND _cairns_serve_assets ${_cairns_serve_wgsl_files})
    foreach(_asset ${_cairns_serve_assets})
        if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/${_asset}")
            add_custom_command(POST_BUILD
                TARGET cairns_serve
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/assets/${_asset}"
                    "${CMAKE_BINARY_DIR}/$<CONFIGURATION>/${_asset}"
            )
        endif()
    endforeach()
    # Mirror the sdl-min asset bundle for cairns_serve: every glb under
    # EXTERNAL_ASSETS_DIR lands next to the binary so the engine's
    # kDebugGlbs[3..103) loop finds them. Without this the headless byte
    # gate renders empty (entities=0) and is meaningless.
    set(EXTERNAL_ASSETS_DIR "/Users/ivanamies/dev/gfx/assets/riot/lol_champions/staging")
    file(GLOB _cairns_serve_glbs CONFIGURE_DEPENDS "${EXTERNAL_ASSETS_DIR}/*.glb")
    foreach(_glb ${_cairns_serve_glbs})
        get_filename_component(_glb_name "${_glb}" NAME)
        add_custom_command(POST_BUILD
            TARGET cairns_serve
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_glb}"
                "${CMAKE_BINARY_DIR}/$<CONFIGURATION>/${_glb_name}"
        )
    endforeach()
endif()

# Also link cairns_control into the SDL app so ImGui-routed registry dispatch
# (planned P5) and the agent transport (P4) can share the same registry.
target_link_libraries(${EXECUTABLE_NAME} PRIVATE cairns_control)

# =============================================================================
# Asset packaging -- attaches to sdl-min only (cairns_serve does its own asset
# loading via NDJSON commands).
# =============================================================================

if (APPLE)
    # Define the base directory for assets
    set(input_root "${CMAKE_CURRENT_SOURCE_DIR}/assets")

    # This macro ensures files end up in the "Resources" folder of the .app bundle
    macro(add_resource FILE)
        if(EXISTS "${FILE}")
            target_sources(${EXECUTABLE_NAME} PRIVATE "${FILE}")
            get_filename_component(FILENAME "${FILE}" NAME)
            set_property(SOURCE "${FILE}" PROPERTY MACOSX_PACKAGE_LOCATION "Resources")
        else()
            message(WARNING "add_resource: missing, skipping ${FILE}")
        endif()
    endmacro()

    # 1. Add specific known files
    add_resource("${input_root}/run.js")  # mandatory boot script
    add_resource("${input_root}/Inter-VariableFont.ttf")

    # 1b. Scenario launcher scripts -> Resources/scripts/. The sdl-min imgui
    # picker enumerates this dir at runtime; drop a .js in assets/scripts/ to
    # add a scenario (no recompile of the picker, no per-scenario C++).
    file(GLOB SCENARIO_SCRIPTS CONFIGURE_DEPENDS "${input_root}/scripts/*.js")
    foreach(SCRIPT_FILE ${SCENARIO_SCRIPTS})
        target_sources(${EXECUTABLE_NAME} PRIVATE "${SCRIPT_FILE}")
        set_property(SOURCE "${SCRIPT_FILE}" PROPERTY
                     MACOSX_PACKAGE_LOCATION "Resources/scripts")
    endforeach()
    add_resource("${input_root}/the_entertainer.ogg")
    add_resource("${input_root}/gs_tiger.svg")
    add_resource("${input_root}/mc_grass.jpeg")

    # 2. Add Shaders (*.metal) as RAW TEXT
    file(GLOB SHADER_FILES "${input_root}/*.metal")
    foreach(METAL_FILE ${SHADER_FILES})
        add_resource("${METAL_FILE}")
        set_source_files_properties("${METAL_FILE}" PROPERTIES
            HEADER_FILE_ONLY TRUE
            XCODE_EXPLICIT_FILE_TYPE "text"
        )
    endforeach()

    # 2.5 Add Shaders (*.spv) as RAW BINARY
    file(GLOB SHADER_FILES "${input_root}/*.spv")
    foreach(SPV_FILE ${SHADER_FILES})
      add_resource("${SPV_FILE}")
        set_source_files_properties("${SPV_FILE}" PROPERTIES
            HEADER_FILE_ONLY TRUE
            XCODE_EXPLICIT_FILE_TYPE "file"
        )
    endforeach()

    # 3. Add GLB Models (*.glb)
    file(GLOB GLB_FILES CONFIGURE_DEPENDS "${input_root}/*.glb")
    foreach(GLB_FILE ${GLB_FILES})
        add_resource("${GLB_FILE}")
    endforeach()

    # 4. Hack in copying all of my league of legends assets
    set(EXTERNAL_ASSETS_DIR "/Users/ivanamies/dev/gfx/assets/riot/lol_champions/staging")
    file(GLOB EXTERNAL_GLB_FILES CONFIGURE_DEPENDS "${EXTERNAL_ASSETS_DIR}/*.glb")
    foreach(EXT_GLB ${EXTERNAL_GLB_FILES})
        add_resource("${EXT_GLB}")
    endforeach()

elseif(EMSCRIPTEN)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE)
else()
    if (ANDROID)
        if (NOT MOBILE_ASSETS_DIR)
            message(FATAL_ERROR "Building for Android, but MOBILE_ASSETS_DIR is not set")
        endif()
        file(MAKE_DIRECTORY "${MOBILE_ASSETS_DIR}")
    endif()

    macro(copy_helper filename)
        if (ANDROID)
            set(outname "${MOBILE_ASSETS_DIR}/${filename}")
        else()
            set(outname "${CMAKE_BINARY_DIR}/$<CONFIGURATION>/${filename}")
        endif()
        add_custom_command(POST_BUILD
            TARGET "${EXECUTABLE_NAME}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_LIST_DIR}/assets/${filename}" "${outname}"
        )
    endmacro()
    copy_helper("run.js")  # mandatory boot script
    copy_helper("unlit.vert.spv")
    copy_helper("unlit.frag.spv")
    copy_helper("unlit_noid.frag.spv") # #222 Phase A.1
    copy_helper("particle.vert.spv")
    copy_helper("particle.frag.spv")
    copy_helper("particle.comp.spv")
    copy_helper("imgui.vert.spv")
    copy_helper("imgui.frag.spv")
    copy_helper("composite_pip.vert.spv")
    copy_helper("composite_pip.frag.spv")
    copy_helper("depthviz.frag.spv")
    copy_helper("outline.vert.spv")  # #207
    copy_helper("outline.frag.spv")
    copy_helper("skin.comp.spv")     # #221 Phase 9
    copy_helper("anim_eval.comp.spv") # #221 Phase 5b
    # Scenario picker scripts -> assets/scripts. SDL_EnumerateDirectory finds
    # them via the AssetManager on Android (posix SDL_sysfsops -> AAssetDir).
    file(GLOB CAIRNS_SCENARIO_SCRIPTS CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_LIST_DIR}/assets/scripts/*.js")
    foreach(SCENARIO_SCRIPT ${CAIRNS_SCENARIO_SCRIPTS})
        get_filename_component(SCENARIO_SCRIPT_NAME "${SCENARIO_SCRIPT}" NAME)
        copy_helper("scripts/${SCENARIO_SCRIPT_NAME}")
    endforeach()
    # Android APK: real GLB copies (an APK can't hold symlinks, so the .app's
    # link/add_resource approach doesn't port). Mirror the .app bundle:
    # assets/*.glb = die/two_die/viking_room; the staging dir = the kDebugGlbs
    # champion roster. Missing champions make the roster resolve short -- the
    # silent 99 the loader now aborts on -- and missing test GLBs leave
    # one_die/two_die/viking_room with no mesh. copy_if_different = incremental.
    if (ANDROID)
        set(ANDROID_GLB_SRC_DIRS
            "${CMAKE_CURRENT_LIST_DIR}/assets"
            "/Users/ivanamies/dev/gfx/assets/riot/lol_champions/staging")
        foreach(GLB_SRC_DIR ${ANDROID_GLB_SRC_DIRS})
            file(GLOB ANDROID_GLB_FILES CONFIGURE_DEPENDS "${GLB_SRC_DIR}/*.glb")
            foreach(ANDROID_GLB ${ANDROID_GLB_FILES})
                get_filename_component(ANDROID_GLB_NAME "${ANDROID_GLB}" NAME)
                add_custom_command(TARGET ${EXECUTABLE_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${ANDROID_GLB}"
                        "${MOBILE_ASSETS_DIR}/${ANDROID_GLB_NAME}")
            endforeach()
        endforeach()
    endif()
endif()

# set some extra configs for each platform
set(RESOURCE_FILES "assets/logo.png")
if(IOS)
    set(RESOURCE_FILES "assets/iosLaunchScreen.storyboard;assets/logo.png")
endif()

set_target_properties(${EXECUTABLE_NAME} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist.in"
    # Generator-agnostic plist substitution (template uses these instead of
    # Xcode's $(...) macros, which Ninja leaves unexpanded -> broken `open`).
    MACOSX_BUNDLE_BUNDLE_NAME "${EXECUTABLE_NAME}"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.cairns.gfx"
    XCODE_GENERATE_SCHEME TRUE
    XCODE_ATTRIBUTE_BUNDLE_IDENTIFIER "com.ravbug.sdl3-sample"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.ravbug.sdl3-sample"
    XCODE_ATTRIBUTE_CURRENTYEAR "${CURRENTYEAR}"
    RESOURCE "${RESOURCE_FILES}"
)

# on Visual Studio, set our app as the default project
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT "${EXECUTABLE_NAME}")

# On macOS, ensure that the bundle is valid for distribution by calling fixup_bundle.
if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    install(TARGETS ${EXECUTABLE_NAME}
        BUNDLE DESTINATION ./install COMPONENT Runtime
        RUNTIME DESTINATION ./install/bin COMPONENT Runtime
    )

    set(DEP_DIR "${CMAKE_BINARY_DIR}")
    INSTALL(CODE
        "include(BundleUtilities)
        fixup_bundle(\"$<TARGET_BUNDLE_DIR:${EXECUTABLE_NAME}>\" \"\" \"${DEP_DIR}\")
        "
    )
    set(CPACK_GENERATOR "DragNDrop")
    include(CPack)
endif()

# ---- Tests -----------------------------------------------------------------
# Off by default so device + Release-app builds aren't burdened with Catch2.
# `cairns_spec_tests` is pure CPU (no engine link, no GPU); `cairns_golden_tests`
# (under tests/CMakeLists.txt) links cairns_core and is opt-in via
# -DCAIRNS_GFX_BUILD_GOLDEN_TESTS=ON.
option(CAIRNS_GFX_BUILD_TESTS "Build the cairns-gfx test suite" OFF)
if(CAIRNS_GFX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
