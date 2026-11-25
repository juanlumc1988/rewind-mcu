# SPDX-License-Identifier: Apache-2.0
# Part of rewind -- deterministic record & replay for bare-metal firmware.

cmake_minimum_required(VERSION 3.16)
project(rewind VERSION 1.0.0 LANGUAGES CXX)

option(REWIND_BUILD_TESTS "Build the unit tests" ON)
option(REWIND_WERROR      "Treat warnings as errors" OFF)
option(REWIND_BUILD_GUI   "Build rewind-gui, the Qt viewer (needs Qt 6 Widgets)" OFF)
option(REWIND_BUILD_EXAMPLES "Build the example that uses rewind with a firmware of its own" OFF)
option(REWIND_INSTALL     "Generate the install rules and the CMake package" ON)

# ---------------------------------------------------------------------------
# Two worlds, deliberately kept apart.
#
#   Target side -- core/, target/, sim/, firmware/
#       Strict C++98. No heap, no exceptions, no RTTI, no standard library
#       beyond what a freestanding implementation must provide. This is the
#       code that cross-compiles, so the standard is enforced by the compiler
#       rather than by discipline: -pedantic-errors turns "someone slipped an
#       auto in" from a code review argument into a build failure.
#
#       -Wno-long-long is the one concession. Sixty-four-bit timestamps need
#       long long, which C++98 does not have; every real cross-toolchain
#       provides it as an extension, and rw_types.h static-asserts the width.
#
#   Host side -- host/, tests/
#       C++17. Runs on a workstation, so it gets a heap and a standard
#       library. Nothing here is ever cross-compiled.
# ---------------------------------------------------------------------------

set(REWIND_WARNINGS "")
set(REWIND_TARGET_FLAGS "")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(REWIND_WARNINGS -Wall -Wextra)
    set(REWIND_TARGET_FLAGS -pedantic-errors -Wno-long-long)
    if(REWIND_WERROR)
        list(APPEND REWIND_WARNINGS -Werror)
    endif()
elseif(MSVC)
    set(REWIND_WARNINGS /W4)
    if(REWIND_WERROR)
        list(APPEND REWIND_WARNINGS /WX)
    endif()
endif()

# Pins a library to the target-side rules.
function(rewind_target_side tgt)
    set_target_properties(${tgt} PROPERTIES
        CXX_STANDARD 98
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(${tgt} PRIVATE ${REWIND_WARNINGS} ${REWIND_TARGET_FLAGS})
endfunction()

function(rewind_host_side tgt)
    set_target_properties(${tgt} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_compile_options(${tgt} PRIVATE ${REWIND_WARNINGS})
endfunction()

# ---------------------------------------------------------------------------
# Target side
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

# The version, for the tools to report and for code that has to check which
# rewind it was built against. Generated, so there is one place to change it.
configure_file(cmake/version.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/rewind/version.h @ONLY)

add_library(rewind_core STATIC
    core/src/varint.cpp
    core/src/crc32.cpp
    core/src/trace_writer.cpp
    core/src/trace_reader.cpp
    core/src/ring_buffer.cpp)
target_include_directories(rewind_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
rewind_target_side(rewind_core)

add_library(rewind_target STATIC
    target/src/hal.cpp
    target/src/clock.cpp
    target/src/recorder.cpp
    target/src/cortex_m.cpp)
target_include_directories(rewind_target PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/target/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rewind_target PUBLIC rewind_core)
rewind_target_side(rewind_target)

add_library(rewind_sim STATIC sim/src/mcu.cpp)
target_include_directories(rewind_sim PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sim/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rewind_sim PUBLIC rewind_target)
rewind_target_side(rewind_sim)

add_library(rewind_fw STATIC firmware/src/rx_pump.cpp)
target_include_directories(rewind_fw PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/firmware/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rewind_fw PUBLIC rewind_target rewind_sim)
rewind_target_side(rewind_fw)

# ---------------------------------------------------------------------------
# Host side
# ---------------------------------------------------------------------------

add_library(rewind_host STATIC
    host/src/firmware.cpp
    host/src/example_firmware.cpp
    host/src/replay.cpp
    host/src/session.cpp
    host/src/timeline.cpp
    host/src/landings.cpp
    host/src/locate.cpp
    host/src/tracediff.cpp
    host/src/describe.cpp
    host/src/lanes.cpp
    host/src/tracedoc.cpp)
target_include_directories(rewind_host PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/host/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# The host headers use the C++17 library; a program built against them gets it.
target_compile_features(rewind_host PUBLIC cxx_std_17)
target_link_libraries(rewind_host PUBLIC rewind_fw rewind_sim rewind_target rewind_core)
rewind_host_side(rewind_host)

# The names a program that uses rewind links against, the same whether rewind
# is built alongside it (add_subdirectory, FetchContent) or installed
# (find_package(rewind)).
foreach(lib core target sim fw host)
    add_library(rewind::${lib} ALIAS rewind_${lib})
    set_target_properties(rewind_${lib} PROPERTIES EXPORT_NAME ${lib})
endforeach()

add_executable(rewind host/src/main.cpp)
target_link_libraries(rewind PRIVATE rewind_host)
rewind_host_side(rewind)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(REWIND_BUILD_TESTS)
    enable_testing()

    add_executable(rewind_tests
        tests/main.cpp
        tests/test_varint.cpp
        tests/test_crc32.cpp
        tests/test_trace.cpp
        tests/test_ring_buffer.cpp
        tests/test_hal.cpp
        tests/test_clock.cpp
        tests/test_cortex_m.cpp
        tests/test_sites.cpp
        tests/test_unguarded.cpp
        tests/test_landings.cpp
        tests/test_locate.cpp
        tests/test_dma.cpp
        tests/test_nested.cpp
        tests/test_firmware.cpp
        tests/test_ram.cpp
        tests/test_tracediff.cpp
        tests/test_view.cpp
        tests/test_version.cpp
        tests/test_recorder.cpp
        tests/test_timeline.cpp
        tests/test_determinism.cpp)
    target_include_directories(rewind_tests PRIVATE tests/third_party)
    target_link_libraries(rewind_tests PRIVATE rewind_host)
    rewind_host_side(rewind_tests)

    add_test(NAME unit COMMAND rewind_tests --force-colors=off)
endif()

# ---------------------------------------------------------------------------
# The viewer. Off by default: it is the only part that needs Qt, and everything
# it shows comes from the Qt-free host library, which the command line uses too.
# ---------------------------------------------------------------------------

if(REWIND_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)

    set(REWIND_GUI_SOURCES
        gui/src/timelinewidget.cpp
        gui/src/eventmodel.cpp
        gui/src/statepanel.cpp
        gui/src/mainwindow.cpp)

    # Everything but main(), so the tests can build a window without a process.
    add_library(rewind_gui_lib STATIC ${REWIND_GUI_SOURCES})
    target_include_directories(rewind_gui_lib PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/gui/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/rewind-gui>)
    add_library(rewind::gui ALIAS rewind_gui_lib)
    set_target_properties(rewind_gui_lib PROPERTIES EXPORT_NAME gui)
    target_link_libraries(rewind_gui_lib PUBLIC rewind_host Qt6::Widgets)
    # Qt's `emit`, `signals` and `slots` are macros, and `emit` is also a method
    # of the trace writer. Keep the macros out and spell them Q_EMIT, Q_SIGNALS.
    target_compile_definitions(rewind_gui_lib PUBLIC QT_NO_KEYWORDS)
    set_target_properties(rewind_gui_lib PROPERTIES AUTOMOC ON)
    rewind_host_side(rewind_gui_lib)

    add_executable(rewind-gui gui/src/main.cpp)
    target_link_libraries(rewind-gui PRIVATE rewind_gui_lib)
    rewind_host_side(rewind-gui)

    if(REWIND_BUILD_TESTS)
        find_package(Qt6 REQUIRED COMPONENTS Test)
        add_executable(rewind_gui_tests gui/tests/gui_test.cpp)
        target_link_libraries(rewind_gui_tests PRIVATE rewind_gui_lib Qt6::Test)
        target_include_directories(rewind_gui_tests PRIVATE tests)
        set_target_properties(rewind_gui_tests PROPERTIES AUTOMOC ON)
        rewind_host_side(rewind_gui_tests)
        add_test(NAME gui COMMAND rewind_gui_tests)
        set_tests_properties(gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

# ---------------------------------------------------------------------------
# An example that is not the example firmware
# ---------------------------------------------------------------------------

if(REWIND_BUILD_EXAMPLES)
    enable_testing()
    add_subdirectory(examples/custom-firmware)
endif()

# ---------------------------------------------------------------------------
# Install, and the package other projects find
#
#   find_package(rewind 1 REQUIRED)
#   target_link_libraries(my_tool PRIVATE rewind::host)      # replay, timeline, diff
#   target_link_libraries(my_device_code PRIVATE rewind::target)   # the shim and recorder
#
# Libraries are installed for whatever the build targeted, so an install from
# the cross-compiled build (cmake/arm-none-eabi.cmake) is the one to link
# device code against, and one from the host build is the one for host tools.
# ---------------------------------------------------------------------------

if(REWIND_INSTALL)
    include(CMakePackageConfigHelpers)

    # A cross-compiled build installs only what runs on the device: host/ is
    # workstation code and is not built for the part.
    set(REWIND_INSTALL_LIBS rewind_core rewind_target rewind_sim rewind_fw)
    set(REWIND_INSTALL_HEADERS core/include/ target/include/ sim/include/ firmware/include/)
    if(NOT CMAKE_CROSSCOMPILING)
        list(APPEND REWIND_INSTALL_LIBS rewind_host)
        list(APPEND REWIND_INSTALL_HEADERS host/include/)
    endif()

    install(TARGETS ${REWIND_INSTALL_LIBS}
            EXPORT rewindTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY ${REWIND_INSTALL_HEADERS}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h")
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/generated/rewind/version.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rewind)

    if(NOT CMAKE_CROSSCOMPILING)
        install(TARGETS rewind RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(EXPORT rewindTargets
            NAMESPACE rewind::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rewind)

    set(REWIND_HAS_GUI ${REWIND_BUILD_GUI})
    if(REWIND_BUILD_GUI)
        install(TARGETS rewind_gui_lib
                EXPORT rewindGuiTargets
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
        install(TARGETS rewind-gui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY gui/src/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rewind-gui
                FILES_MATCHING PATTERN "*.h")
        install(EXPORT rewindGuiTargets
                NAMESPACE rewind::
                DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rewind)
    endif()

    configure_package_config_file(cmake/rewindConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/rewindConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rewind)
    # Same major version only: 1.x is compatible with 1.y, and nothing else.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/rewindConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/rewindConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/rewindConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rewind)
endif()
