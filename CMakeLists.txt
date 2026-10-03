cmake_minimum_required(VERSION 3.15)

project(Colourunder
    VERSION 0.1.0
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Universal by default so one bundle loads in both the Apple Silicon and the
# Intel build of Resolume. Override with -DCMAKE_OSX_ARCHITECTURES=arm64 for a
# faster dev build.
#
# This must sit immediately after project() and before any target exists. CMake
# latches the architecture list when the first target is created; setting it
# later is silently ignored and you get an arm64-only binary that the build log
# calls a success. Verify with `lipo`, never with the log.
# ---------------------------------------------------------------------------
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Apple deprecated the whole OpenGL API in 10.14 and FFGL's plugin interface is
# still OpenGL, so every SDK call warns. There is no alternative API to move to
# here, so silencing it is not ignoring a problem.
if(APPLE)
    add_compile_definitions(GL_SILENCE_DEPRECATION)
endif()

if(MSVC)
    add_compile_options(/utf-8)
endif()

# windows.h defines min and max as macros, which turns every std::min into
# `std::(...)` and fails with an error naming neither min nor windows.h. Set
# globally rather than per-file: the next file to include windows.h hits it too.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

option(COLOURUNDER_BUILD_TOOLS "Build the offline render and measurement harness" ON)

# ---------------------------------------------------------------------------
# The parts with no GL in them at all: the deck's arithmetic, the controls, the
# clock and the frame plan (the CPU half of ProcessOpenGL).
#
# A separate library from the FFGL core because a CPU render of the same deck
# needs it and has no GL context. Splitting it is what lets that build link the
# SAME arithmetic rather than a second transcription of it.
# ---------------------------------------------------------------------------
add_library(colourunder_dsp OBJECT
    source/Model.h
    source/Model.cpp
    source/Controls.h
    source/Controls.cpp
    source/Clock.h
    source/Clock.cpp
    source/Frame.h
    source/Frame.cpp
)
target_include_directories(colourunder_dsp PUBLIC source)
set_target_properties(colourunder_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# The Resolume FFGL SDK, as a git submodule pinned to b1afaf9 -- the rev the
# rest of the fleet pins.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/ffgl/CMakeLists.txt")
    message(FATAL_ERROR
        "FFGL SDK not found. Run: git submodule update --init --recursive")
endif()

set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(external/ffgl EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Everything the plugin is, as an OBJECT library so both the bundle and the
# offline harness are built from exactly the same code.
#
# OBJECT and not STATIC on purpose. `CFFGLPluginInfo` registers itself from a
# file-scope constructor in Colourunder.cpp and nothing ever references that object
# by name, so in an archive the linker is entitled to drop the whole
# translation unit -- and the result is a bundle that loads, exports plugMain,
# and reports that it contains no plugins.
# ---------------------------------------------------------------------------
add_library(colourunder_core OBJECT
    source/StoatworksAbout.h
    source/StoatworksAboutLinks.h
    source/StoatworksAboutParams.h
    source/Diag.h
    source/Diag.cpp
    source/PassBuffer.h
    source/PassBuffer.cpp
    source/Shaders.h
    source/Shaders.cpp
    source/Colourunder.h
    source/Colourunder.cpp
)

target_include_directories(colourunder_core PUBLIC source)
target_link_libraries(colourunder_core PUBLIC ffgl::sdk colourunder_dsp)
target_compile_definitions(colourunder_core PUBLIC COLOURUNDER_VERSION="${PROJECT_VERSION}")
set_target_properties(colourunder_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)
target_link_libraries(colourunder_core PUBLIC Threads::Threads)

# On Windows/Linux the SDK headers pull in <GL/glew.h>. The SDK links GLEW
# privately, so consumers need it too -- for the include path and for the
# GLEW_STATIC define the static vcpkg triplet propagates. macOS uses the system
# OpenGL framework and needs none of this. GLEW arrives through vcpkg.json,
# which nothing in this file points at: lose it and only Windows breaks, and
# only at configure time.
if(NOT APPLE)
    find_package(GLEW REQUIRED)
    target_link_libraries(colourunder_core PUBLIC GLEW::GLEW)
endif()

# ---------------------------------------------------------------------------
# The plugin. FFGL resolves one plugMain per binary.
# ---------------------------------------------------------------------------
function(colourunder_add_plugin target entry bundle_name identifier)
    add_library(${target} MODULE ${entry})
    # colourunder_dsp is named again here, and on every other final target,
    # because an OBJECT library's objects do NOT travel transitively through a
    # second OBJECT library: colourunder_core linking it gives this target the
    # include paths and nothing to link, and the failure is a pile of undefined
    # symbols at the very last step of an otherwise clean build.
    target_link_libraries(${target} PRIVATE colourunder_core colourunder_dsp)

    if(APPLE)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "bundle"
            OUTPUT_NAME "${bundle_name}"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Info.plist.in"
            MACOSX_BUNDLE_BUNDLE_NAME "${bundle_name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${identifier}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        # FFGL hosts resolve plugMain by symbol name, so keep default visibility.
        target_link_libraries(${target} PRIVATE
            "-framework OpenGL"
            "-framework Carbon"
            "-framework AppKit"
        )
    else()
        set_target_properties(${target} PROPERTIES OUTPUT_NAME "${bundle_name}")
    endif()
endfunction()

colourunder_add_plugin(Colourunder source/PluginEntry.cpp
    "Colourunder" "com.stoatworks.ffgl.colourunder")

# `cmake --install build` drops the bundle where Arena reads it. Avenue reads
# ~/Documents/Resolume Avenue/Extra Effects instead; pass --prefix for that.
if(APPLE AND CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "$ENV{HOME}/Documents/Resolume Arena/Extra Effects"
        CACHE PATH "" FORCE)
endif()
install(TARGETS Colourunder
    LIBRARY DESTINATION "."
    BUNDLE  DESTINATION "."
)

# ---------------------------------------------------------------------------
# The harness: the real plugin class in a real GL context, with no host.
#
# macOS only, because it stands up a CGL context directly. The chroma band,
# the group delay, the head switch, PAL against NTSC, the compensator, the
# generations, the tracking bar and the resize are all measured out of the
# plugin's own render, so this is where the deck is verified rather than
# previewed. See tools/cutest. zlib ships with the OS, which is
# why the PNG writer is fifty lines here rather than a vendored dependency.
# ---------------------------------------------------------------------------
if(COLOURUNDER_BUILD_TOOLS AND APPLE)
    add_executable(cutest tools/cutest/main.cpp)
    target_link_libraries(cutest PRIVATE colourunder_core colourunder_dsp z
        "-framework OpenGL"
        "-framework AppKit"
    )
endif()
