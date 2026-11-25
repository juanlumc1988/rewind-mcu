# SPDX-License-Identifier: Apache-2.0
# Part of rewind -- deterministic record & replay for bare-metal firmware.

cmake_minimum_required(VERSION 3.16)
project(rewind VERSION 1.0.0 LANGUAGES CXX)

option(REWIND_BUILD_TESTS "Build the unit tests" ON)
option(REWIND_WERROR      "Treat warnings as errors" OFF)
option(REWIND_BUILD_GUI   "Build rewind-gui, the Qt viewer (needs Qt 6 Widgets)" OFF)

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
target_include_directories(rewind_core PUBLIC core/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated)
rewind_target_side(rewind_core)

add_library(rewind_target STATIC
    target/src/hal.cpp
    target/src/clock.cpp
    target/src/recorder.cpp
    target/src/cortex_m.cpp)
target_include_directories(rewind_target PUBLIC target/include)
target_link_libraries(rewind_target PUBLIC rewind_core)
rewind_target_side(rewind_target)

add_library(rewind_sim STATIC sim/src/mcu.cpp)
target_include_directories(rewind_sim PUBLIC sim/include)
target_link_libraries(rewind_sim PUBLIC rewind_target)
rewind_target_side(rewind_sim)

add_library(rewind_fw STATIC firmware/src/rx_pump.cpp)
target_include_directories(rewind_fw PUBLIC firmware/include)
target_link_libraries(rewind_fw PUBLIC rewind_target rewind_sim)
rewind_target_side(rewind_fw)

# ---------------------------------------------------------------------------
# Host side
# ---------------------------------------------------------------------------

add_library(rewind_host STATIC
    host/src/replay.cpp
    host/src/session.cpp
    host/src/timeline.cpp
    host/src/landings.cpp
    host/src/locate.cpp
    host/src/tracediff.cpp
    host/src/describe.cpp
    host/src/lanes.cpp
    host/src/tracedoc.cpp)
target_include_directories(rewind_host PUBLIC host/include)
target_link_libraries(rewind_host PUBLIC rewind_fw rewind_sim rewind_target rewind_core)
rewind_host_side(rewind_host)

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
    target_include_directories(rewind_gui_lib PUBLIC gui/src)
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
        set_target_properties(rewind_gui_tests PROPERTIES AUTOMOC ON)
        rewind_host_side(rewind_gui_tests)
        add_test(NAME gui COMMAND rewind_gui_tests)
        set_tests_properties(gui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()
