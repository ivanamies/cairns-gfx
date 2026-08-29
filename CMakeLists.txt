cmake_minimum_required(VERSION 3.16)

# set the output directory for built objects.
# This makes sure that the dynamic library goes into the build directory automatically.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIGURATION>")

# prevent installing to system directories. 
set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}" CACHE INTERNAL "")

# Declare the project
project(sdl-min)

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

# GPU backend is a true compile-time feature: pick exactly one.
set(CAIRNS_GFX_BACKEND "metal" CACHE STRING "GPU backend: metal or vulkan")
set_property(CACHE CAIRNS_GFX_BACKEND PROPERTY STRINGS metal vulkan)

if(CAIRNS_GFX_BACKEND STREQUAL "metal")
  set(CAIRNS_GFX_BACKEND_METAL 1)
  set(CAIRNS_GFX_BACKEND_VULKAN 0)
elseif(CAIRNS_GFX_BACKEND STREQUAL "vulkan")
  set(CAIRNS_GFX_BACKEND_METAL 0)
  set(CAIRNS_GFX_BACKEND_VULKAN 1)
else()
  message(FATAL_ERROR "CAIRNS_GFX_BACKEND must be 'metal' or 'vulkan', got '${CAIRNS_GFX_BACKEND}'")
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util/gfx_config.hpp.in"
  "${CMAKE_BINARY_DIR}/generated/util/gfx_config.hpp"
  @ONLY)

# Set the name of the executable
set(EXECUTABLE_NAME ${PROJECT_NAME})

# Create an executable or a shared library based on the platform and add our sources to it
if (ANDROID)
	# The SDL java code is hardcoded to load libmain.so on android, so we need to change EXECUTABLE_NAME
	set(EXECUTABLE_NAME main)
	add_library(${EXECUTABLE_NAME} SHARED)
else()
	add_executable(${EXECUTABLE_NAME})
endif()

# GCC and Clang (Linux, macOS, Android, Emscripten)
target_compile_options(${EXECUTABLE_NAME} PRIVATE 
  -Wall 
  -Wextra 
  -Werror
  -fno-exceptions # no crutches to hide behind
  -fno-rtti # ditto
  # -O2
)


# What is iosLaunchScreen.storyboard? This file describes what Apple's mobile platforms
# should show the user while the application is starting up. If you don't include one,
# then you get placed in a compatibility mode that does not allow HighDPI.
# This file is referenced inside Info.plist.in, where it is marked as the launch screen file.
# It is also ignored on non-Apple platforms.

# To get an app icon on Apple platforms, add it to your executable.
# Afterward, the image file in Info.plist.in.
if(APPLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/logo.png")
    target_sources("${EXECUTABLE_NAME}" PRIVATE "assets/logo.png")
endif()

if(CAIRNS_GFX_BACKEND_METAL)
  message("tagiamies using metal")
    # --- START METAL-CPP CONFIGURATION ---
    # Point to the metal-cpp directory in the root of your project
    target_include_directories(${EXECUTABLE_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/metal-cpp")
    
    # Find system frameworks required by Metal
    find_library(METAL_LIB Metal REQUIRED)
    find_library(FOUNDATION_LIB Foundation REQUIRED)
    find_library(QUARTZ_LIB QuartzCore REQUIRED)
    
    # Link the frameworks
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE ${METAL_LIB} ${FOUNDATION_LIB} ${QUARTZ_LIB})
    # --- END METAL-CPP CONFIGURATION ---    
endif()
if(CAIRNS_GFX_BACKEND_VULKAN)
  if (APPLE)
  message("tagiamies using vulkan too!!")
    find_package(Vulkan REQUIRED)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE Vulkan::Vulkan)
  elseif (ANDROID)
    # the vulkan lib sorta just shows up on android...
    target_link_libraries(${EXECUTABLE_NAME} PUBLIC vulkan android log)
  endif()
endif()

# Add your sources to the target
file(GLOB_RECURSE ALL_SOURCES
  "src/*.cpp"
  "src/*.hpp"
  "src/iosLaunchScreen.storyboard"
)

list(REMOVE_ITEM ALL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/render/render_thread.cpp")
target_sources(${EXECUTABLE_NAME} PRIVATE ${ALL_SOURCES})

# Taskflow needs exceptions + has size_t->int narrowing in its headers, so the
# one TU that includes it lives in its own static library with relaxed flags.
add_library(cairns_render_thread STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/render/render_thread.cpp")
target_include_directories(cairns_render_thread PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/taskflow"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm")
target_compile_features(cairns_render_thread PUBLIC cxx_std_20)
target_compile_options(cairns_render_thread PRIVATE -Wall -Wextra)
target_compile_definitions(cairns_render_thread PRIVATE
    $<$<BOOL:${CAIRNS_GFX_BACKEND_METAL}>:CAIRNS_METAL=1>
    $<$<BOOL:${CAIRNS_GFX_BACKEND_VULKAN}>:CAIRNS_VULKAN=1>)
if (CAIRNS_GFX_BACKEND_VULKAN AND APPLE)
    target_link_libraries(cairns_render_thread PRIVATE Vulkan::Vulkan)
endif()
target_link_libraries(${EXECUTABLE_NAME} PRIVATE cairns_render_thread)

# Set C++ version
target_compile_features(${EXECUTABLE_NAME} PUBLIC cxx_std_20)

target_include_directories(${EXECUTABLE_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_BINARY_DIR}/generated"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/taskflow"
)

# on Web targets, we need CMake to generate a HTML webpage. 
if(EMSCRIPTEN)
	set(CMAKE_EXECUTABLE_SUFFIX ".html" CACHE INTERNAL "")
endif()

# import fastgltf
set(FASTGLTF_ENABLE_ASSET_DEBUGGER OFF CACHE BOOL "" FORCE)
set(FASTGLTF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FASTGLTF_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory(third_party/fastgltf EXCLUDE_FROM_ALL)

# Configure SDL by calling its CMake file.
# we use EXCLUDE_FROM_ALL so that its install targets and configs don't
# pollute upwards into our configuration.
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)

# If you don't want SDL_ttf, then remove this section.
set(SDLTTF_VENDORED ON) # tell SDL_ttf to build its own dependencies
add_subdirectory(third_party/SDL_ttf EXCLUDE_FROM_ALL)	

# SDL_mixer (used for playing audio)
set(SDLMIXER_MIDI_NATIVE OFF)     # disable formats we don't use to make the build faster and smaller. Also some of these don't work on all platforms so you'll need to do some experimentation.
set(SDLMIXER_GME OFF)
set(SDLMIXER_WAVPACK OFF)     
set(SDLMIXER_MOD OFF)
set(SDLMIXER_MOD_XMP OFF)
set(SDLMIXER_OPUS OFF)
set(SDLMIXER_MP3_MPG123 OFF)
set(SDLMIXER_VORBIS_VORBISFILE OFF)
set(SDLMIXER_FLAC_LIBFLAC OFF)
set(SDLMIXER_VENDORED ON)   # tell SDL_mixer to build its own dependencies
add_subdirectory(third_party/SDL_mixer EXCLUDE_FROM_ALL)

# SDL_image (used for loading various image formats)
set(SDLIMAGE_VENDORED ON)
set(SDLIMAGE_AVIF OFF)	# disable formats we don't use to make the build faster and smaller.
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
    third_party/imgui/backends/imgui_impl_sdl3.cpp
)
target_include_directories(imgui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends"
)
target_compile_features(imgui PUBLIC cxx_std_20)
target_compile_options(imgui PRIVATE -fno-exceptions -fno-rtti)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

# Link SDL to our executable. This also makes its include directory available to us.
target_link_libraries(${EXECUTABLE_NAME} PUBLIC
	SDL3_ttf::SDL3_ttf      # remove if you are not using SDL_ttf
	SDL3_mixer::SDL3_mixer  # remove if you are not using SDL_mixer
	SDL3_image::SDL3_image	# remove if you are not using SDL_image
        SDL3::SDL3              # If using satelite libraries, SDL must be the last item in the list.
        fastgltf::fastgltf
        imgui
)

# SDL_Image bug: https://github.com/libsdl-org/SDL_image/issues/506
if (APPLE AND NOT BUILD_SHARED_LIBS)
    find_library(IO_LIB ImageIO REQUIRED)
    find_library(CS_LIB CoreServices REQUIRED)
    find_library(CT_LIB CoreText REQUIRED)
    find_library(CG_LIB CoreGraphics REQUIRED)
    find_library(CF_LIB CoreFoundation REQUIRED)
    target_link_libraries(${EXECUTABLE_NAME} PUBLIC ${CF_LIB} ${CT_LIB} ${IO_LIB} ${CS_LIB} ${CG_LIB})
endif()

# Dealing with assets
# We have some non-code resources that our application needs in order to work. How we deal with those differs per platform.
if (APPLE)
    # Define the base directory for assets
    set(input_root "${CMAKE_CURRENT_SOURCE_DIR}/assets")

    # This macro ensures files end up in the "Resources" folder of the .app bundle
    macro(add_resource FILE)
        if(EXISTS "${FILE}")
            # We target_sources so Xcode sees the file in the project tree
            target_sources(${EXECUTABLE_NAME} PRIVATE "${FILE}")

            # We strip the path to get just the filename
            get_filename_component(FILENAME "${FILE}" NAME)

            # We force the file into the "Resources" root.
            # On iOS and macOS, this is where the app looks by default.
            set_property(SOURCE "${FILE}" PROPERTY MACOSX_PACKAGE_LOCATION "Resources")
        else()
            message(WARNING "add_resource: missing, skipping ${FILE}")
        endif()
    endmacro()

    # 1. Add specific known files
    add_resource("${input_root}/Inter-VariableFont.ttf")
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
    # Using GLOB is fine as long as add_resource flattens the path
    file(GLOB GLB_FILES "${input_root}/*.glb")
    foreach(GLB_FILE ${GLB_FILES})
        add_resource("${GLB_FILE}")
    endforeach()

    # 4. Hack in copying all of my league of legends assets
    set(EXTERNAL_ASSETS_DIR "/Users/ivanamies/dev/gfx/assets/riot/lol_champions/staging")
    file(GLOB EXTERNAL_GLB_FILES "${EXTERNAL_ASSETS_DIR}/*.glb")
    foreach(EXT_GLB ${EXTERNAL_GLB_FILES})
        add_resource("${EXT_GLB}")
    endforeach()

elseif(EMSCRIPTEN)
    # on the web, we have to put the files inside of the webassembly
    # somewhat unintuitively, this is done via a linker argument.
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE
    )
else()
    if (ANDROID)
        if (NOT MOBILE_ASSETS_DIR)
            message(FATAL_ERROR "Building for Android, but MOBILE_ASSETS_DIR is not set")
        endif()
        file(MAKE_DIRECTORY "${MOBILE_ASSETS_DIR}")    # we need to create the project Assets dir otherwise CMake won't copy our assets.
    endif()

    
    macro(copy_helper filename)
        if (ANDROID)
            # MOBILE_ASSETS_DIR is set in the gradle file via the cmake command line and points to the Android Studio Assets folder. 
            # when we copy assets there, the Android build pipeline knows to add them to the apk. 
            set(outname "${MOBILE_ASSETS_DIR}/${filename}")    
        else()
            # for platforms that do not have a good packaging format, all we can do is copy the assets to the process working directory.
            set(outname "${CMAKE_BINARY_DIR}/$<CONFIGURATION>/${filename}")
        endif()
        add_custom_command(POST_BUILD
            TARGET "${EXECUTABLE_NAME}"
		    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_LIST_DIR}/assets/${filename}" "${outname}"
	    )
    endmacro()
    copy_helper("unlit.vert.spv")
    copy_helper("unlit.frag.spv")
    copy_helper("particle.vert.spv")
    copy_helper("particle.frag.spv")
    copy_helper("particle.comp.spv")
    copy_helper("imgui.vert.spv")
    copy_helper("imgui.frag.spv")
endif()

# set some extra configs for each platform
set(RESOURCE_FILES "assets/logo.png")
if(IOS)
    set(RESOURCE_FILES "assets/iosLaunchScreen.storyboard;assets/logo.png") # make the splash screen show up on iOS
endif()

set_target_properties(${EXECUTABLE_NAME} PROPERTIES 
    # On macOS, make a proper .app bundle instead of a bare executable
    MACOSX_BUNDLE TRUE
    # Set the Info.plist file for Apple Mobile platforms. Without this file, your app
    # will not launch. 
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/assets/Info.plist.in"

    # in Xcode, create a Scheme in the schemes dropdown for the app.
    XCODE_GENERATE_SCHEME TRUE
    # Identification for Xcode
    XCODE_ATTRIBUTE_BUNDLE_IDENTIFIER "com.ravbug.sdl3-sample"
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.ravbug.sdl3-sample"
	XCODE_ATTRIBUTE_CURRENTYEAR "${CURRENTYEAR}"
    RESOURCE "${RESOURCE_FILES}"
)

# on Visual Studio, set our app as the default project
set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT "${EXECUTABLE_NAME}")

# On macOS Platforms, ensure that the bundle is valid for distribution by calling fixup_bundle.
# note that fixup_bundle does not work on iOS, so you will want to use static libraries 
# or manually copy dylibs and set rpaths
if(CMAKE_SYSTEM_NAME MATCHES "Darwin")
    # tell Install about the target, otherwise fixup won't know about the transitive dependencies
    install(TARGETS ${EXECUTABLE_NAME}
    	BUNDLE DESTINATION ./install COMPONENT Runtime
   	    RUNTIME DESTINATION ./install/bin COMPONENT Runtime
    )
	
    set(DEP_DIR "${CMAKE_BINARY_DIR}")  # where to look for dependencies when fixing up
    INSTALL(CODE 
        "include(BundleUtilities)
        fixup_bundle(\"$<TARGET_BUNDLE_DIR:${EXECUTABLE_NAME}>\" \"\" \"${DEP_DIR}\")
        " 
    )
    set(CPACK_GENERATOR "DragNDrop")
    include(CPack)
endif()
