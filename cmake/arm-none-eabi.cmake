# SPDX-License-Identifier: Apache-2.0
# Part of rewind -- deterministic record & replay for bare-metal firmware.
#
# Cross-compilation toolchain for the target-side libraries. Defaults to a
# Cortex-M4F; override REWIND_ARM_CPU / REWIND_ARM_FPU for another part.
#
#   cmake -S . -B build-arm \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
#         -DREWIND_BUILD_TESTS=OFF
#   cmake --build build-arm --target rewind_core rewind_target
#
# Only rewind_core and rewind_target cross-compile, and that is the point:
# they are the two libraries that ship on the device. Everything under host/
# and tests/ is workstation code and is not expected to build here.

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(REWIND_ARM_PREFIX "arm-none-eabi-" CACHE STRING "Cross-toolchain prefix")
set(REWIND_ARM_CPU    "cortex-m4"      CACHE STRING "-mcpu value")
set(REWIND_ARM_FPU    "fpv4-sp-d16"    CACHE STRING "-mfpu value")

set(CMAKE_C_COMPILER   ${REWIND_ARM_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${REWIND_ARM_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${REWIND_ARM_PREFIX}gcc)
set(CMAKE_AR           ${REWIND_ARM_PREFIX}ar     CACHE FILEPATH "" FORCE)
set(CMAKE_OBJCOPY      ${REWIND_ARM_PREFIX}objcopy CACHE FILEPATH "" FORCE)
set(CMAKE_SIZE         ${REWIND_ARM_PREFIX}size    CACHE FILEPATH "" FORCE)

# There is no C runtime to link a test executable against, so probe with a
# static library instead of the default try-run.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(REWIND_ARM_ARCH_FLAGS
    "-mcpu=${REWIND_ARM_CPU} -mthumb -mfpu=${REWIND_ARM_FPU} -mfloat-abi=hard")

# The target-side code allocates nothing and throws nothing, so say so and
# get the flash back: -fno-exceptions and -fno-rtti between them drop the
# unwind tables and the typeinfo that would otherwise come along for the ride.
# -ffunction-sections with --gc-sections lets the linker discard whatever the
# firmware does not call.
set(CMAKE_CXX_FLAGS_INIT
    "${REWIND_ARM_ARCH_FLAGS} -fno-exceptions -fno-rtti -fno-unwind-tables \
-ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT "${REWIND_ARM_ARCH_FLAGS} -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${REWIND_ARM_ARCH_FLAGS} -Wl,--gc-sections --specs=nano.specs")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
