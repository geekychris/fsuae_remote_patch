#!/usr/bin/env bash
# run.sh — launch the patched fs-uae and (optionally) open the web UI.
#
# This is the "one command to get a debugger" entry point.
#
# Usage:
#   ./run.sh                                  # launch with no config
#   ./run.sh <config.fs-uae>                  # launch a specific config
#   ./run.sh --no-browser <config.fs-uae>     # don't auto-open the UI
#
# Env vars:
#   FSUAE_BINARY               path to fs-uae (default: /tmp/fsuae-src/fs-uae)
#   FSUAE_RPC_PORT             port for the debugger API (default: 8765)
#   FSUAE_GDB_PORT             port for the in-process GDB stub
#                              (unset = disabled; suggest 2331)
#   FSUAE_RPC_PAUSE_AT_BOOT    set to 1 to pause at boot
#   FSUAE_RPC_LOG              path to redirect fs-uae stdout/stderr
#                              (default: /tmp/fsuae-rpc.log)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
FSUAE_BINARY="${FSUAE_BINARY:-/tmp/fsuae-src/fs-uae}"
FSUAE_RPC_PORT="${FSUAE_RPC_PORT:-8765}"
FSUAE_RPC_LOG="${FSUAE_RPC_LOG:-/tmp/fsuae-rpc.log}"
OPEN_BROWSER=1
CONFIG=""

# Argv parse
for arg in "$@"; do
    case "$arg" in
        --no-browser) OPEN_BROWSER=0 ;;
        -h|--help)
            sed -n '2,16p' "$0"
            exit 0
            ;;
        *) CONFIG="$arg" ;;
    esac
done

if [[ ! -x "$FSUAE_BINARY" ]]; then
    echo "fs-uae binary not found at: $FSUAE_BINARY" >&2
    echo "Build it first:  $HERE/build.sh" >&2
    exit 1
fi

# Kill any existing instance bound to our port — keeps the workflow
# idempotent (re-run this script and it brings up a fresh emulator).
if pgrep -f "$FSUAE_BINARY" >/dev/null; then
    echo "==> stopping existing fs-uae instance"
    pkill -f "$FSUAE_BINARY" || true
    sleep 1
fi

# Build the launch command.  Stdin from /dev/null is critical: the
# patched console_get() relies on it returning EOF so the in-process
# debugger blocks instead of accepting "commands" from a tty.
ARGS=()
[[ -n "$CONFIG" ]] && ARGS=("$CONFIG")

echo "==> launching fs-uae on port $FSUAE_RPC_PORT"
echo "    log:    $FSUAE_RPC_LOG"
[[ -n "$CONFIG" ]] && echo "    config: $CONFIG"

FSUAE_RPC_PORT="$FSUAE_RPC_PORT" \
FSUAE_GDB_PORT="${FSUAE_GDB_PORT:-}" \
FSUAE_RPC_PAUSE_AT_BOOT="${FSUAE_RPC_PAUSE_AT_BOOT:-}" \
    "$FSUAE_BINARY" "${ARGS[@]}" </dev/null >"$FSUAE_RPC_LOG" 2>&1 &
PID=$!
disown

# Wait for the RPC to come up.
for i in $(seq 1 20); do
    if curl -s "http://127.0.0.1:$FSUAE_RPC_PORT/v1/ping" >/dev/null 2>&1; then
        echo "==> fs-uae up (PID $PID)"
        break
    fi
    sleep 0.2
    if [[ $i -eq 20 ]]; then
        echo "fs-uae failed to start RPC.  Check $FSUAE_RPC_LOG" >&2
        exit 1
    fi
done

echo
echo "  Web UI:      http://127.0.0.1:$FSUAE_RPC_PORT/"
echo "  JSON-RPC:    http://127.0.0.1:$FSUAE_RPC_PORT/v1/*"
echo "  WebSocket:   ws://127.0.0.1:$FSUAE_RPC_PORT/v1/events"
if [[ -n "${FSUAE_GDB_PORT:-}" ]]; then
    echo "  GDB stub:    target remote 127.0.0.1:$FSUAE_GDB_PORT"
    echo "               gdb -tui -x $HERE/tools/fsuae.gdbinit"
fi
echo
echo "  Stop with:   pkill -f $FSUAE_BINARY"
echo "  Log tail:    tail -f $FSUAE_RPC_LOG"
echo

if [[ $OPEN_BROWSER -eq 1 ]] && command -v open >/dev/null; then
    open "http://127.0.0.1:$FSUAE_RPC_PORT/"
elif [[ $OPEN_BROWSER -eq 1 ]] && command -v xdg-open >/dev/null; then
    xdg-open "http://127.0.0.1:$FSUAE_RPC_PORT/"
fi
