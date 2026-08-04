cmake_minimum_required(VERSION 3.25)

project(AudioLens
    VERSION 0.1.1
    DESCRIPTION "Preset-based audio assistance for Windows playback"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "AudioLens は Windows 専用です。")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Targets pick these up via the audiolens::options interface library rather than
# global flags, so a future third-party dependency is not compiled with them.
add_library(audiolens_options INTERFACE)
add_library(audiolens::options ALIAS audiolens_options)

target_compile_definitions(audiolens_options INTERFACE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00      # Windows 10
    # Single source of truth: the version lives in project() above and reaches
    # the code from there, so a release cannot ship a binary claiming one
    # version while the build claims another.
    AUDIOLENS_VERSION="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(audiolens_options INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /EHsc
        /MP
        # C4324: padding inserted for alignas. Cache-line alignment of the
        # ring buffer's atomics is deliberate, so the padding is the point.
        /wd4324
    )
    # Release builds keep symbols so a crash report from a user is actionable.
    target_link_options(audiolens_options INTERFACE $<$<CONFIG:Release>:/DEBUG>)
    target_compile_options(audiolens_options INTERFACE $<$<CONFIG:Release>:/Zi>)
endif()

add_subdirectory(src/common)
add_subdirectory(src/engine)
add_subdirectory(src/dsp)
add_subdirectory(src/core)
add_subdirectory(src/audiofile)
add_subdirectory(src/analysis)
add_subdirectory(src/tools/passthrough)
add_subdirectory(src/tools/process)
add_subdirectory(src/tools/tone)
add_subdirectory(src/tools/procloop)

# The GUI is the only part that needs Qt. Everything else — the engine, the DSP
# and the tests — stays buildable without it, so a Qt-less checkout still gets a
# working build rather than a configure error.
option(AUDIOLENS_BUILD_GUI "Build the Qt GUI application" ON)
if(AUDIOLENS_BUILD_GUI)
    find_package(Qt6 6.5 COMPONENTS Widgets QUIET)
    if(Qt6_FOUND)
        qt_standard_project_setup()
        add_subdirectory(src/app)
        message(STATUS "GUI: Qt ${Qt6_VERSION} を使用します")
    else()
        message(STATUS "GUI: Qt6 が見つからないため GUI をビルドしません "
                       "(CMAKE_PREFIX_PATH に Qt のキットを指定してください)")
    endif()
endif()

enable_testing()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
