# SPDX-License-Identifier: Apache-2.0
# Part of rewind -- deterministic record & replay for bare-metal firmware.
#
# A project that uses rewind with a firmware of its own. Builds two ways:
#
#   * inside rewind's own build  (-DREWIND_BUILD_EXAMPLES=ON), against its targets
#   * on its own, against an installed rewind:
#       cmake --install build --prefix /some/prefix        (in rewind)
#       cmake -S examples/custom-firmware -B ex -DCMAKE_PREFIX_PATH=/some/prefix

cmake_minimum_required(VERSION 3.16)

if(NOT TARGET rewind::host)
    project(rewind_custom_firmware LANGUAGES CXX)
    find_package(rewind 1 REQUIRED)
endif()

# The firmware: the part that runs on the device. Linked against the shim only.
# It is device code, so it is held to the device's rules -- C++98 -- and a slip is
# a build failure, as in rewind's own device-side libraries.
add_library(thermostat STATIC thermostat.cpp)
target_link_libraries(thermostat PUBLIC rewind::target)
set_target_properties(thermostat PROPERTIES
    CXX_STANDARD 98 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(thermostat PRIVATE -Wall -Wextra -pedantic-errors -Wno-long-long)
endif()
target_include_directories(thermostat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The host side: record against a model of the hardware, replay, walk the run.
add_executable(thermostat_replay main.cpp)
target_link_libraries(thermostat_replay PRIVATE thermostat rewind::host)

enable_testing()
add_test(NAME custom_firmware_example COMMAND thermostat_replay
         ${CMAKE_CURRENT_BINARY_DIR}/thermostat-test.rwd)

# A viewer for it, if rewind came with one.
if(TARGET rewind::gui)
    add_executable(thermostat_viewer viewer.cpp)
    target_link_libraries(thermostat_viewer PRIVATE thermostat rewind::gui)
    set_target_properties(thermostat_viewer PROPERTIES AUTOMOC ON)
endif()
