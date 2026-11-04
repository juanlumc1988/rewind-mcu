#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Part of rewind -- deterministic record & replay for bare-metal firmware.
#
# Builds and runs the Cortex-M interrupt trampoline check under QEMU.
#
#   cmake -S . -B build-arm -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
#         -DREWIND_BUILD_TESTS=OFF
#   cmake --build build-arm --target rewind_core rewind_target
#   tests/qemu/run.sh build-arm
#
# Needs arm-none-eabi-g++ and qemu-system-arm (apt: gcc-arm-none-eabi
# qemu-system-arm). Two programs are run:
#
#   the real trampoline   must pass, with the frame on MSP and on PSP;
#   a broken one          (always reads MSP) must FAIL the PSP case.
#
# The second is what shows the PSP check can fail. A check that cannot fail
# says nothing, and "the interrupt was recorded" would pass for a trampoline
# that read garbage and happened to land somewhere plausible.

set -Eeuo pipefail

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
trap 'die "failed at line $LINENO: $BASH_COMMAND"' ERR

[[ $# -eq 1 ]] || die "usage: $0 <build dir holding librewind_core.a and librewind_target.a>"
build=$(cd -- "$1" && pwd)
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)

cxx=${ARM_CXX:-arm-none-eabi-g++}
qemu=${QEMU:-qemu-system-arm}
for tool in "$cxx" "$qemu"; do
    command -v "$tool" >/dev/null || die "$tool not found"
done
for lib in librewind_core.a librewind_target.a; do
    [[ -f $build/$lib ]] || die "$build/$lib not found: build rewind_core and rewind_target for the arm toolchain first"
done

tmp=$(mktemp -d)
trap 'rm -rf -- "${tmp:?}"' EXIT

arch=(-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard)
common=("${arch[@]}" -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections
        -O1 -g -I"$root/core/include" -I"$root/target/include")

# The test program may use GNU extensions; the trampoline translation unit is
# held to the target side's rules, since that is who the macro is for.
# -fno-tree-loop-distribute-patterns keeps main.cpp's own memset from being
# compiled into a call to itself.
"$cxx" "${common[@]}" -std=gnu++14 -Wall -Wextra -Werror \
       -fno-tree-loop-distribute-patterns -c "$here/main.cpp" -o "$tmp/main.o"
"$cxx" "${common[@]}" -std=c++98 -pedantic-errors -Wall -Wextra -Werror -Wno-long-long \
       -c "$here/isr_tu.cpp" -o "$tmp/isr_tu.o"
"$cxx" "${common[@]}" -std=c++98 -c "$here/isr_mutant.cpp" -o "$tmp/isr_mutant.o"

link() {   # <trampoline object> <elf>
    "$cxx" "${arch[@]}" -nostdlib -nostartfiles -Wl,--gc-sections -T "$here/link.ld" \
           "$tmp/main.o" "$1" "$build/librewind_target.a" "$build/librewind_core.a" \
           -lgcc -o "$2"
}

# Runs one program, leaving its output in $out and its exit status in $status.
# QEMU's exit status is the program's: it exits through semihosting. The output
# goes through a file and run is called in this shell, not in $(...), so that
# $status survives.
status=0
out=
run() {
    status=0
    timeout 60 "$qemu" -M mps2-an386 -nographic \
            -semihosting-config enable=on,target=native -kernel "$1" \
            >"$tmp/out.txt" 2>&1 </dev/null || status=$?
    out=$(<"$tmp/out.txt")
    printf '%s\n' "$out"
}

link "$tmp/isr_tu.o" "$tmp/real.elf"
link "$tmp/isr_mutant.o" "$tmp/mutant.elf"

printf '== real trampoline\n'
run "$tmp/real.elf"
[[ $status -eq 0 ]] || die "real trampoline: exit status $status (expected 0)"
[[ $out == *"MSP:"*"OK"* && $out == *"PSP:"*"OK"* ]] || die "real trampoline: MSP and PSP cases did not both report OK"

printf '\n== broken trampoline (must fail the PSP case)\n'
run "$tmp/mutant.elf"
[[ $status -ne 0 ]] || die "broken trampoline passed: the PSP check cannot fail"
[[ $out == *"MSP:"*"OK"* && $out == *"PSP:"*"FAIL"* ]] || die "broken trampoline did not fail where expected"

printf '\nok: trampoline correct on MSP and PSP, and the check can fail\n'
