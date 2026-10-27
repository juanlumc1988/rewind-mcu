#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Part of rewind -- deterministic record & replay for bare-metal firmware.
#
# One-shot setup for Ubuntu 24.04 on WSL2. Installs the host toolchain, the
# arm-none-eabi cross toolchain and a debugger, clones the repository, writes
# the CMake presets and the VS Code configuration, then proves the result by
# running the builds CI runs through those same presets: host/GCC, host/Clang
# and Cortex-M4.
#
#   scripts/setup-rewindmcu.sh                from inside a clone: uses that clone
#   bash setup-rewindmcu.sh                   anywhere else: clones into ~/rewind-mcu
#   bash setup-rewindmcu.sh --dir ~/src/rw    ... or into another directory
#
# Run it as your normal user. It calls sudo for apt and for nothing else.
#
# It leaves the clone's tracked files alone. CMakeUserPresets.json is the file
# CMake reserves for local presets; it is hidden from git through
# .git/info/exclude, which is local to the clone. .vscode/ is already in the
# project's .gitignore.
#
# Safe to re-run: apt skips what is installed, an existing clone is left
# untouched, existing config files are kept, and the build directories are
# reconfigured in place.

set -Eeuo pipefail

readonly REPO_URL=https://github.com/juanlumc1988/rewind-mcu.git
# Installed into the WSL side of VS Code, not the Windows side.
readonly VSCODE_EXTENSIONS=(ms-vscode.cpptools ms-vscode.cmake-tools)

dest=$HOME/rewind-mcu
want_install=1
want_clang=1
want_arm=1
want_verify=1
want_vscode=1
force_config=0

log()  { printf '\n==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }
trap 'die "failed at line $LINENO: $BASH_COMMAND"' ERR

usage() {
    cat <<'EOF'
Usage: setup-rewindmcu.sh [options]

  --dir PATH      where to clone (default: ~/rewind-mcu; ignored when the
                  script is saved inside a clone)
  --no-install    do not touch apt or sudo; the tools must already be there
  --no-clang      skip Clang (CI builds with both compilers)
  --no-arm        skip the arm-none-eabi cross toolchain
  --no-verify     skip the build and test run
  --no-vscode     skip .vscode/ and the VS Code extensions
  --force-config  replace CMakeUserPresets.json and .vscode/* that already
                  exist; the old files are saved next to them as .bak
  -h, --help      show this help
EOF
}

while (($#)); do
    case $1 in
        --dir)          [[ $# -ge 2 ]] || die "--dir needs a path"; dest=$2; shift ;;
        --no-install)   want_install=0 ;;
        --no-clang)     want_clang=0 ;;
        --no-arm)       want_arm=0 ;;
        --no-verify)    want_verify=0 ;;
        --no-vscode)    want_vscode=0 ;;
        --force-config) force_config=1 ;;
        -h|--help)      usage; exit 0 ;;
        *)              usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------

[[ $EUID -ne 0 ]] || die "run as your normal user, not root (a root-owned clone is a headache in VS Code)"

# shellcheck source=/dev/null
. /etc/os-release
if [[ ${ID:-} != ubuntu || ${VERSION_ID:-} != 24.04 ]]; then
    warn "written for Ubuntu 24.04, found ${PRETTY_NAME:-an unknown distro}; continuing"
fi
if ! grep -qi microsoft /proc/sys/kernel/osrelease; then
    warn "this does not look like WSL; continuing anyway"
elif ! grep -qi wsl2 /proc/sys/kernel/osrelease; then
    warn "WSL1 detected: builds are much slower there (wsl --set-version <distro> 2)"
fi

# ---------------------------------------------------------------------------
# Packages
# ---------------------------------------------------------------------------

if ((want_install)); then
    command -v apt-get >/dev/null || die "apt-get not found: this script targets Ubuntu/Debian"
    sudo -v || die "sudo is required to install packages"

    # build-essential brings g++ and make; CMake 3.28 and Clang 18 are what
    # Ubuntu 24.04 ships, comfortably past the 3.16 the project asks for (the
    # presets below need 3.21).
    pkgs=(git ca-certificates build-essential cmake ninja-build gdb)
    if ((want_clang)); then
        pkgs+=(clang)
    fi

    # The ARM toolchain goes in without recommends, on purpose. Its recommends
    # chain ends at libstdc++-arm-none-eabi-newlib: ~460 MB to download and
    # ~3 GB on disk, for a library the target side never links (no heap, no
    # exceptions, no RTTI, no standard library). The libraries cross-compile
    # without any newlib, and CI's no-heap/no-RTTI check passes on this minimal
    # set. Add libnewlib-arm-none-eabi yourself when you start linking real
    # firmware.
    arm_pkgs=(gcc-arm-none-eabi binutils-arm-none-eabi)

    apt_install() {
        sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y "$@"
    }

    log "installing: ${pkgs[*]}"
    sudo apt-get update
    apt_install "${pkgs[@]}"
    if ((want_arm)); then
        log "installing: ${arm_pkgs[*]} (without recommends)"
        apt_install --no-install-recommends "${arm_pkgs[@]}"
    fi
fi

need_tools() {   # also the whole check under --no-install
    local missing=() t
    for t in git cmake ninja g++ gdb "$@"; do
        command -v "$t" >/dev/null || missing+=("$t")
    done
    ((${#missing[@]} == 0)) || die "missing tools: ${missing[*]} (re-run without --no-install)"
}

extra_tools=()
if ((want_clang)); then extra_tools+=(clang++); fi
if ((want_arm));   then extra_tools+=(arm-none-eabi-gcc arm-none-eabi-size); fi
need_tools "${extra_tools[@]}"

show_version() {   # first line of "<tool> --version", without a pipe to trip pipefail
    local out
    out=$("$@" --version 2>&1)
    printf '  %s\n' "${out%%$'\n'*}"
}

log "toolchain"
show_version cmake
printf '  ninja %s\n' "$(ninja --version)"
show_version g++
show_version gdb
if ((want_clang)); then show_version clang++; fi
if ((want_arm));   then show_version arm-none-eabi-gcc; fi

# ---------------------------------------------------------------------------
# The repository
# ---------------------------------------------------------------------------

is_rewind_tree() { [[ -f $1/CMakeLists.txt ]] && grep -q '^project(rewind ' "$1/CMakeLists.txt"; }

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]:-$0}")" && pwd)
repo=
for d in "$script_dir" "$script_dir/.."; do   # the clone's root, or its scripts/
    if is_rewind_tree "$d"; then
        repo=$(cd -- "$d" && pwd)
        break
    fi
done
if [[ -n $repo ]]; then
    log "using the clone this script lives in: $repo"
else
    repo=$(realpath -m -- "$dest")
    if [[ -e $repo/.git ]]; then
        is_rewind_tree "$repo" || die "$repo is a git clone, but not of rewind-mcu"
        log "using the existing clone: $repo"
    elif [[ -e $repo ]]; then
        die "$repo exists and is not a git clone"
    else
        log "cloning into $repo"
        git clone "$REPO_URL" "$repo"
    fi
fi

case $repo in
    /mnt/*) warn "$repo is on a Windows drive: file access through 9P is slow and file modes/line endings get mangled. Clone under \$HOME instead." ;;
esac

cd -- "$repo"

# ---------------------------------------------------------------------------
# Local configuration: CMake presets and VS Code
# ---------------------------------------------------------------------------

# write_config <path>: the file's content comes on stdin. An existing file is
# left alone unless --force-config, which saves it as <path>.bak first.
write_config() {
    local path=$1
    if [[ -e $path ]]; then
        if ((!force_config)); then
            printf '  kept   %s (already there)\n' "${path#"$repo"/}"
            return 0
        fi
        cp -- "$path" "$path.bak"
    fi
    mkdir -p -- "$(dirname -- "$path")"
    cat >"$path"
    printf '  wrote  %s\n' "${path#"$repo"/}"
}

# Build directories are build-<preset>/, which the project's .gitignore
# already covers (build-*/). ARM builds only the four libraries that ship on
# the device: host/ is workstation code and does not cross-compile, so the
# default "all" target would fail there.
log "CMake presets"
write_config "$repo/CMakeUserPresets.json" <<'EOF'
{
  "version": 3,
  "cmakeMinimumRequired": { "major": 3, "minor": 21, "patch": 0 },
  "configurePresets": [
    {
      "name": "base",
      "hidden": true,
      "generator": "Ninja",
      "binaryDir": "${sourceDir}/build-${presetName}",
      "cacheVariables": { "CMAKE_EXPORT_COMPILE_COMMANDS": "ON" }
    },
    {
      "name": "gcc-debug",
      "displayName": "Host / GCC / Debug",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_CXX_COMPILER": "g++"
      }
    },
    {
      "name": "clang-debug",
      "displayName": "Host / Clang / Debug",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_CXX_COMPILER": "clang++"
      }
    },
    {
      "name": "gcc-asan-ubsan",
      "displayName": "Host / GCC / Debug + ASan + UBSan",
      "inherits": "base",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "Debug",
        "CMAKE_CXX_COMPILER": "g++",
        "CMAKE_CXX_FLAGS": "-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
      }
    },
    {
      "name": "arm-cortex-m4",
      "displayName": "Target / arm-none-eabi / Cortex-M4 / MinSizeRel (libraries only)",
      "inherits": "base",
      "toolchainFile": "${sourceDir}/cmake/arm-none-eabi.cmake",
      "cacheVariables": {
        "CMAKE_BUILD_TYPE": "MinSizeRel",
        "REWIND_BUILD_TESTS": "OFF"
      }
    }
  ],
  "buildPresets": [
    { "name": "gcc-debug", "configurePreset": "gcc-debug" },
    { "name": "clang-debug", "configurePreset": "clang-debug" },
    { "name": "gcc-asan-ubsan", "configurePreset": "gcc-asan-ubsan" },
    {
      "name": "arm-cortex-m4",
      "configurePreset": "arm-cortex-m4",
      "targets": ["rewind_core", "rewind_target", "rewind_sim", "rewind_fw"]
    }
  ],
  "testPresets": [
    {
      "name": "gcc-debug",
      "configurePreset": "gcc-debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "clang-debug",
      "configurePreset": "clang-debug",
      "output": { "outputOnFailure": true }
    },
    {
      "name": "gcc-asan-ubsan",
      "configurePreset": "gcc-asan-ubsan",
      "output": { "outputOnFailure": true },
      "environment": {
        "ASAN_OPTIONS": "detect_leaks=1",
        "UBSAN_OPTIONS": "print_stacktrace=1"
      }
    }
  ]
}
EOF

# Untracked, so git status would list it. Hide it locally, without touching
# any tracked file.
exclude=$(git rev-parse --path-format=absolute --git-path info/exclude)
mkdir -p -- "$(dirname -- "$exclude")"
if ! grep -qxF '/CMakeUserPresets.json' "$exclude" 2>/dev/null; then
    printf '/CMakeUserPresets.json\n' >>"$exclude"
    printf '  hid    CMakeUserPresets.json from git (%s)\n' "${exclude#"$repo"/}"
fi

# Tasks and launch configurations call cmake and the built binaries directly,
# not through the CMake Tools extension, so F5 and Ctrl+Shift+B work whatever
# the extension's state. The extension supplies IntelliSense and the status-bar
# preset picker.
if ((want_vscode)); then
    log "VS Code: .vscode/"

    write_config "$repo/.vscode/extensions.json" <<'EOF'
{
  "recommendations": [
    "ms-vscode.cpptools",
    "ms-vscode.cmake-tools"
  ]
}
EOF

    write_config "$repo/.vscode/settings.json" <<'EOF'
{
  "cmake.useCMakePresets": "always",
  "cmake.configureOnOpen": true,
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools",
  "files.associations": { "*.h": "cpp" },
  "files.eol": "\n",
  "files.watcherExclude": { "**/build-*/**": true },
  "search.exclude": { "**/build-*": true }
}
EOF

    write_config "$repo/.vscode/tasks.json" <<'EOF'
{
  "version": "2.0.0",
  "tasks": [
    {
      "label": "build: gcc-debug",
      "type": "shell",
      "command": "cmake --preset gcc-debug && cmake --build --preset gcc-debug",
      "options": { "cwd": "${workspaceFolder}" },
      "group": { "kind": "build", "isDefault": true },
      "problemMatcher": ["$gcc"]
    },
    {
      "label": "test: gcc-debug",
      "type": "shell",
      "command": "ctest --preset gcc-debug",
      "dependsOn": "build: gcc-debug",
      "options": { "cwd": "${workspaceFolder}" },
      "group": { "kind": "test", "isDefault": true },
      "problemMatcher": []
    },
    {
      "label": "build: clang-debug",
      "type": "shell",
      "command": "cmake --preset clang-debug && cmake --build --preset clang-debug",
      "options": { "cwd": "${workspaceFolder}" },
      "group": "build",
      "problemMatcher": ["$gcc"]
    },
    {
      "label": "build + test: gcc-asan-ubsan",
      "type": "shell",
      "command": "cmake --preset gcc-asan-ubsan && cmake --build --preset gcc-asan-ubsan && ctest --preset gcc-asan-ubsan",
      "options": { "cwd": "${workspaceFolder}" },
      "group": "test",
      "problemMatcher": ["$gcc"]
    },
    {
      "label": "build: arm-cortex-m4",
      "type": "shell",
      "command": "cmake --preset arm-cortex-m4 && cmake --build --preset arm-cortex-m4 && arm-none-eabi-size -t build-arm-cortex-m4/librewind_core.a build-arm-cortex-m4/librewind_target.a",
      "options": { "cwd": "${workspaceFolder}" },
      "group": "build",
      "problemMatcher": ["$gcc"]
    },
    {
      "label": "rewind: save failing trace",
      "type": "shell",
      "command": "build-gcc-debug/rewind hunt --from 1 --to 30 --save build-gcc-debug/bug.rwd",
      "dependsOn": "build: gcc-debug",
      "options": { "cwd": "${workspaceFolder}" },
      "problemMatcher": []
    }
  ]
}
EOF

    write_config "$repo/.vscode/launch.json" <<'EOF'
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "rewind hunt 1..30",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build-gcc-debug/rewind",
      "args": ["hunt", "--from", "1", "--to", "30"],
      "cwd": "${workspaceFolder}",
      "stopAtEntry": false,
      "MIMode": "gdb",
      "setupCommands": [
        { "description": "Enable gdb pretty-printing", "text": "-enable-pretty-printing", "ignoreFailures": true }
      ],
      "preLaunchTask": "build: gcc-debug"
    },
    {
      "name": "rewind replay (saved failing trace)",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build-gcc-debug/rewind",
      "args": ["replay", "${workspaceFolder}/build-gcc-debug/bug.rwd"],
      "cwd": "${workspaceFolder}",
      "stopAtEntry": false,
      "MIMode": "gdb",
      "setupCommands": [
        { "description": "Enable gdb pretty-printing", "text": "-enable-pretty-printing", "ignoreFailures": true }
      ],
      "preLaunchTask": "rewind: save failing trace"
    },
    {
      "name": "rewind inspect (saved failing trace)",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build-gcc-debug/rewind",
      "args": ["inspect", "${workspaceFolder}/build-gcc-debug/bug.rwd", "--at", "200", "--walk", "5"],
      "cwd": "${workspaceFolder}",
      "stopAtEntry": false,
      "MIMode": "gdb",
      "setupCommands": [
        { "description": "Enable gdb pretty-printing", "text": "-enable-pretty-printing", "ignoreFailures": true }
      ],
      "preLaunchTask": "rewind: save failing trace"
    },
    {
      "name": "rewind_tests (all)",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build-gcc-debug/rewind_tests",
      "args": ["--force-colors=off"],
      "cwd": "${workspaceFolder}",
      "stopAtEntry": false,
      "MIMode": "gdb",
      "setupCommands": [
        { "description": "Enable gdb pretty-printing", "text": "-enable-pretty-printing", "ignoreFailures": true }
      ],
      "preLaunchTask": "build: gcc-debug"
    },
    {
      "name": "rewind_tests (test-case filter)",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build-gcc-debug/rewind_tests",
      "args": ["--force-colors=off", "--test-case=${input:testCase}"],
      "cwd": "${workspaceFolder}",
      "stopAtEntry": false,
      "MIMode": "gdb",
      "setupCommands": [
        { "description": "Enable gdb pretty-printing", "text": "-enable-pretty-printing", "ignoreFailures": true }
      ],
      "preLaunchTask": "build: gcc-debug"
    }
  ],
  "inputs": [
    {
      "id": "testCase",
      "type": "promptString",
      "description": "doctest test-case filter; * is a wildcard, e.g. *varint*",
      "default": "*"
    }
  ]
}
EOF
fi

# ---------------------------------------------------------------------------
# Prove it works
# ---------------------------------------------------------------------------

jobs=$(nproc)

# Verification goes through the presets above, so what VS Code is about to use
# is what has just been shown to work. An existing CMakeUserPresets.json is
# never overwritten, so it may lack ours: say so, with the fix, rather than
# letting cmake fail with a bare "no such preset".
need_preset() {
    local listed
    listed=$(cmake --list-presets=configure)
    [[ $listed == *"\"$1\""* ]] ||
        die "no configure preset '$1' in $repo/CMakeUserPresets.json (an existing file was kept); re-run with --force-config to replace it"
}

host_check() {   # <preset>: configure, build and run the unit tests
    log "host build and tests: preset $1"
    need_preset "$1"
    cmake --preset "$1"
    cmake --build --preset "$1" -j "$jobs"
    ctest --preset "$1"
}

# What the CI "Exercise the command line" step does, minus the files it writes:
# unit tests never touch argument parsing.
cli_smoke() {
    log "command-line smoke test"
    "$repo/build-gcc-debug/rewind" hunt --from 1 --to 30
}

arm_check() {
    log "cross build for Cortex-M4: preset arm-cortex-m4"
    need_preset arm-cortex-m4
    cmake --preset arm-cortex-m4
    cmake --build --preset arm-cortex-m4 -j "$jobs"
    arm-none-eabi-size -t build-arm-cortex-m4/librewind_core.a build-arm-cortex-m4/librewind_target.a
}

if ((want_verify)); then
    host_check gcc-debug
    cli_smoke
    if ((want_clang)); then host_check clang-debug; fi
    if ((want_arm));   then arm_check; fi
fi

# ---------------------------------------------------------------------------
# VS Code extensions
# ---------------------------------------------------------------------------

# `code` inside WSL is the wrapper that Windows VS Code puts on the PATH. Run
# from here it installs into the WSL-side server, which is where the C++
# extensions have to live to see this toolchain.
if ((want_vscode)); then
    if command -v code >/dev/null; then
        log "installing VS Code extensions into WSL: ${VSCODE_EXTENSIONS[*]}"
        for ext in "${VSCODE_EXTENSIONS[@]}"; do
            code --install-extension "$ext" || warn "could not install $ext; install it from VS Code instead"
        done
    else
        warn "'code' is not on the PATH, so no VS Code extensions were installed (see below)"
    fi
fi

log "done"
cat <<EOF
Open the project from Windows through WSL:

    cd "$repo" && code .

Needs VS Code on Windows with the "WSL" extension (ms-vscode-remote.remote-wsl).
The first 'code .' downloads the VS Code server into WSL. Accept the prompt to
install the recommended extensions in "WSL: <distro>" if they are not there yet:
${VSCODE_EXTENSIONS[*]}

In VS Code:
    status bar        pick the configure preset: gcc-debug
    Ctrl+Shift+B      build (gcc-debug)
    F5                debug in gdb; pick a launch configuration in
                      Run and Debug: hunt, replay, inspect, unit tests
    Testing view      ctest results

The same from a terminal, in $repo:

    cmake --preset gcc-debug && cmake --build --preset gcc-debug && ctest --preset gcc-debug
    cmake --preset arm-cortex-m4 && cmake --build --preset arm-cortex-m4

CMakeUserPresets.json and .vscode/ are local: git status stays clean. To
regenerate them, re-run with --force-config; the old files are kept as .bak.
EOF
