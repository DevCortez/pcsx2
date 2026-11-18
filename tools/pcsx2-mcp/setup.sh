#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
companion=${1:-"$script_dir/pcsx2-mcp"}
if [[ ! -f "$companion" || ! -x "$companion" ]]; then
    printf 'Companion executable not found: %s\nUsage: %s [path-to-pcsx2-mcp]\n' "$companion" "$0" >&2
    exit 1
fi
companion=$(realpath -- "$companion")
args=()

printf 'PCSX2 MCP setup\n\n'
printf 'PCSX2 executable (blank to use the executable beside the companion): '
IFS= read -r emulator || exit 0
if [[ -n "$emulator" ]]; then
    if [[ ! -f "$emulator" || ! -x "$emulator" ]]; then
        printf 'PCSX2 executable not found: %s\n' "$emulator" >&2
        exit 1
    fi
    args+=(--emulator "$(realpath -- "$emulator")")
fi

PS3='Configure which client? '
select client in 'Antigravity (agy)' 'Codex' 'Both' 'Quit'; do
    case "$REPLY" in
        1) clients=(agy) ;;
        2) clients=(codex) ;;
        3) clients=(agy codex) ;;
        4) exit 0 ;;
        *) printf 'Select a number from 1 to 4.\n' >&2; continue ;;
    esac
    for cli in "${clients[@]}"; do
        if ! command -v "$cli" >/dev/null; then
            printf '%s is missing from PATH.\n' "$cli" >&2
            exit 1
        fi
    done
    for cli in "${clients[@]}"; do
        if [[ "$cli" == agy ]]; then
            agy mcp add --type stdio pcsx2 -- "$companion" "${args[@]}"
        else
            codex mcp add pcsx2 -- "$companion" "${args[@]}"
        fi
    done
    printf 'Registered pcsx2. Reopen your client to load the configuration.\n'
    exit 0
done
