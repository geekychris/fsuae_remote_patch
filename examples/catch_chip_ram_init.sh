#!/usr/bin/env bash
# catch_chip_ram_init.sh — find where the Amiga Kickstart populates the
# CPU vector table at chip RAM $C0-$FF.
#
# Demonstrates the full bisection workflow:
#   1. Launch FS-UAE paused at boot (FSUAE_RPC_PAUSE_AT_BOOT=1)
#   2. Install a write watchpoint on the target address
#   3. Resume
#   4. Watchpoint auto-pauses the emulator on the first matching write
#   5. Read CPU regs → that's the install routine's PC
#
# Usage:
#   FSUAE_RPC_PORT=8765 FSUAE_RPC_PAUSE_AT_BOOT=1 fs-uae <config> &
#   ./catch_chip_ram_init.sh

set -euo pipefail

PORT="${FSUAE_RPC_PORT:-8765}"
BASE="http://127.0.0.1:$PORT"
TARGET_ADDR="${TARGET_ADDR:-0xC0}"
TARGET_LEN="${TARGET_LEN:-64}"

echo "Watching writes to $TARGET_ADDR (len $TARGET_LEN) on $BASE"

# Verify FS-UAE is up and paused
state=$(curl -s "$BASE/v1/state" | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])')
echo "Initial state: $state"
if [[ "$state" != "paused" ]]; then
    echo "!! emulator is not paused. Relaunch with FSUAE_RPC_PAUSE_AT_BOOT=1."
    exit 1
fi

# Make sure no leftover watchpoints from a prior run
curl -sX POST "$BASE/v1/watchpoints/clear" >/dev/null

# Install the watchpoint
echo "Installing watchpoint..."
curl -sX POST "$BASE/v1/watchpoints?addr=$TARGET_ADDR&size=$TARGET_LEN&rwi=W" \
    | python3 -m json.tool

# Resume
echo "Resuming..."
curl -sX POST "$BASE/v1/resume" >/dev/null

# Poll for hit
echo "Polling for watchpoint hit (up to 10 s)..."
for i in $(seq 1 200); do
    sleep 0.05
    s=$(curl -s "$BASE/v1/state" | python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])')
    if [[ "$s" == "paused" ]]; then
        echo "  HIT at poll #$i (~${i}*50ms wallclock)"
        echo
        echo "=== CPU at hit ==="
        curl -s "$BASE/v1/cpu" | python3 -m json.tool
        echo
        echo "=== Memory at $TARGET_ADDR (len $TARGET_LEN) ==="
        curl -s "$BASE/v1/mem?addr=$TARGET_ADDR&len=$TARGET_LEN" | python3 -m json.tool
        exit 0
    fi
done

echo "  no watchpoint hit in 10 s"
exit 1
