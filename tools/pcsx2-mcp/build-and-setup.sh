#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo_dir=$(cd -- "$script_dir/../.." && pwd)
build_dir=${PCSX2_BUILD_DIR:-"$repo_dir/build/pcsx2-mcp"}
deps=${PCSX2_DEPS_PREFIX:-${CMAKE_PREFIX_PATH:-}}

if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    cat <<'HELP'
Usage: build-and-setup.sh [-- PCSX2 arguments...]
Builds PCSX2 and its MCP companion on Linux and registers the server with Codex.

Optional environment variables
  PCSX2_BUILD_DIR    Out-of-tree build directory (default: build/pcsx2-mcp)
  PCSX2_DEPS_PREFIX  CMake prefix for existing PCSX2 build dependencies
  PCSX2_BUILD_JOBS   Parallel build jobs (default: 6)
HELP
    exit 0
fi
if [[ $# -gt 0 ]]; then
    if [[ $1 != -- ]]; then
        printf 'Pass PCSX2 arguments after --. See %s --help.\n' "$0" >&2
        exit 2
    fi
    shift
fi

for command in cmake ninja clang clang++ ld.lld go codex; do
    if ! command -v "$command" >/dev/null; then
        printf 'Required command missing from PATH: %s\n' "$command" >&2
        exit 1
    fi
done

if [[ -z $deps && -d $repo_dir/deps ]]; then
    deps=$repo_dir/deps
elif [[ -z $deps && -d /tmp/pcsx2-mcp-deps ]]; then
    deps=/tmp/pcsx2-mcp-deps
fi
mkdir -p -- "$build_dir"
build_dir=$(cd -- "$build_dir" && pwd)
cmake_args=()
if [[ -n $deps ]]; then
    cmake_args+=("-DCMAKE_PREFIX_PATH=$deps")
fi
cmake --preset clang-devel -S "$repo_dir" -B "$build_dir" -DUSE_BACKTRACE=OFF "${cmake_args[@]}"
cmake --build "$build_dir" --target pcsx2-qt --parallel "${PCSX2_BUILD_JOBS:-6}"

bin_dir=$build_dir/bin
emulator=$bin_dir/pcsx2-qt
companion=$bin_dir/pcsx2-mcp
if [[ ! -x $emulator || ! -d $bin_dir/resources || ! -s $bin_dir/translations/pcsx2-qt_en-US.qm ]]; then
    printf 'Incomplete build output under %s. Expected pcsx2-qt, resources/, and generated translations/.\n' "$bin_dir" >&2
    exit 1
fi

(
    cd -- "$script_dir"
    CGO_ENABLED=0 go build -o "$companion" .
)

codex_env=()
for name in DISPLAY WAYLAND_DISPLAY XAUTHORITY XDG_RUNTIME_DIR XDG_CONFIG_HOME XDG_DATA_HOME DBUS_SESSION_BUS_ADDRESS; do
    if [[ -n ${!name:-} ]]; then
        codex_env+=(--env "$name=${!name}")
    fi
done
emulator_args=()
if [[ $# -gt 0 ]]; then
    emulator_args+=(-- "$@")
fi
codex mcp add pcsx2 "${codex_env[@]}" -- "$companion" --emulator "$emulator" "${emulator_args[@]}"
codex mcp get pcsx2 >/dev/null
printf '\nRegistered pcsx2 with Codex. Restart the local Codex client, reopen this conversation, and ask it to call pcsx2_connect.\n'
