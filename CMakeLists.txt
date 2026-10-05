# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20)
project(supergbamidi VERSION 1.2.0 LANGUAGES CXX)

# The name of the macOS droplet app.
set(SUPERGBAMIDI_APP_NAME Supergbamidi)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Link MSVC's runtime statically, so the released .exe runs without the runtime DLLs installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(SUPERGBAMIDI_BUILD_TESTS "Build and register the tests" ON)

add_library(supergbamidi_core STATIC
    src/inflate.cpp
    src/midi.cpp
    src/music.cpp
    src/rom.cpp
    src/sf2.cpp
    src/konami/convert.cpp
    src/konami/driver.cpp
    src/konami/music.cpp
    src/konami/pitch.cpp
    src/konami/seqformat.cpp
    src/konami/sequencer.cpp
    src/konami/soundfont.cpp
    src/mp2k/convert.cpp
    src/mp2k/driver.cpp
    src/mp2k/music.cpp
    src/mp2k/sequencer.cpp
    src/mp2k/song.cpp
    src/mp2k/soundfont.cpp
    src/rare/convert.cpp
    src/rare/driver.cpp
    src/rare/music.cpp
    src/rare/sequencer.cpp
    src/rare/song.cpp
    src/rare/soundfont.cpp
    src/quintet/convert.cpp
    src/quintet/driver.cpp
    src/quintet/music.cpp
    src/quintet/sequencer.cpp
    src/quintet/song.cpp
    src/quintet/soundfont.cpp
    src/rd2/convert.cpp
    src/rd2/driver.cpp
    src/rd2/music.cpp
    src/rd2/sequencer.cpp
    src/rd2/song.cpp
    src/rd2/soundfont.cpp
    src/thumb.cpp
)
target_include_directories(supergbamidi_core PUBLIC src)

# Disable fused multiply-add so output stays byte-identical across platforms. GCC and Clang can fuse a * b + c on
# CPUs such as ARM64, changing the rounding. MSVC's default /fp:precise disables this in Visual Studio 2022 and later;
# clang-cl needs an explicit option.
if(MSVC)
    target_compile_options(supergbamidi_core PUBLIC /W4 /utf-8)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(supergbamidi_core PUBLIC /clang:-ffp-contract=off)
    endif()
else()
    target_compile_options(supergbamidi_core PUBLIC -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()

add_executable(supergbamidi src/main.cpp)
target_link_libraries(supergbamidi PRIVATE supergbamidi_core)
if(WIN32)
    target_link_libraries(supergbamidi PRIVATE shell32)  # CommandLineToArgvW
    # Embed the version and description shown in Explorer's file properties.
    configure_file(platform/windows/version.rc.in version.rc @ONLY)
    target_sources(supergbamidi PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
endif()
target_compile_definitions(supergbamidi PRIVATE SUPERGBAMIDI_VERSION="${PROJECT_VERSION}")
include(GNUInstallDirs)
install(TARGETS supergbamidi RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE THIRD_PARTY_NOTICES DESTINATION "${CMAKE_INSTALL_DATADIR}/supergbamidi")

# macOS: a droplet app, so files can be dropped on supergbamidi in the Finder.
if(APPLE)
    find_program(OSACOMPILE osacompile)
    find_program(OSASCRIPT osascript)
    find_program(CODESIGN codesign)
    if(OSACOMPILE AND CODESIGN)
        set(SUPERGBAMIDI_APP "${CMAKE_CURRENT_BINARY_DIR}/${SUPERGBAMIDI_APP_NAME}.app")
        set(SUPERGBAMIDI_APP_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/platform/macos/${SUPERGBAMIDI_APP_NAME}.applescript")
        add_custom_command(
            OUTPUT "${SUPERGBAMIDI_APP}/Contents/Resources/supergbamidi"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SUPERGBAMIDI_APP}"
            COMMAND "${OSACOMPILE}" -o "${SUPERGBAMIDI_APP}" "${SUPERGBAMIDI_APP_SCRIPT}"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:supergbamidi>"
                    "${SUPERGBAMIDI_APP}/Contents/Resources/supergbamidi"
            COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                    "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES" "${SUPERGBAMIDI_APP}/Contents/Resources/"
            COMMAND "${CODESIGN}" --force --deep --sign - "${SUPERGBAMIDI_APP}"
            DEPENDS supergbamidi "${SUPERGBAMIDI_APP_SCRIPT}" LICENSE THIRD_PARTY_NOTICES
            COMMENT "Building ${SUPERGBAMIDI_APP_NAME}.app"
            VERBATIM)
        add_custom_target(supergbamidi_app ALL DEPENDS "${SUPERGBAMIDI_APP}/Contents/Resources/supergbamidi")
    endif()
endif()

if(SUPERGBAMIDI_BUILD_TESTS)
    enable_testing()
    add_executable(supergbamidi_tests tests/test_main.cpp tests/test_util.cpp tests/konami_test.cpp tests/rare_test.cpp
                                      tests/quintet_test.cpp tests/mp2k_test.cpp tests/rd2_test.cpp)
    target_link_libraries(supergbamidi_tests PRIVATE supergbamidi_core)
    add_test(NAME supergbamidi_tests COMMAND supergbamidi_tests)

    # Test the droplet app's conversion report using a stand-in for the program.
    if(TARGET supergbamidi_app AND OSASCRIPT)
        add_test(NAME droplet
                 COMMAND "${OSASCRIPT}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/droplet_test.applescript"
                         "${SUPERGBAMIDI_APP}/Contents/Resources/Scripts/main.scpt"
                         "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_tool.sh")
    endif()

    # Load local_tests.cmake from each immediate subdirectory to register tests that use locally supplied files.
    file(GLOB local_tests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*/local_tests.cmake")
    foreach(file IN LISTS local_tests)
        include("${file}")
    endforeach()
endif()
