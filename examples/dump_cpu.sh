#!/usr/bin/env bash
# Pause the emulator, dump CPU registers + 64 bytes at the PC, resume.
set -euo pipefail

PORT="${FSUAE_RPC_PORT:-8765}"
BASE="http://127.0.0.1:$PORT"

curl -sX POST "$BASE/v1/pause" >/dev/null
sleep 0.05  # let in-flight cycles settle

echo "--- CPU ---"
curl -s "$BASE/v1/cpu" | python3 -m json.tool

# Extract PC and dump 64 bytes around it
PC=$(curl -s "$BASE/v1/cpu" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["pc"])')
echo
echo "--- $PC: 64 bytes ---"
curl -s "$BASE/v1/mem?addr=$PC&len=64" | python3 -m json.tool

curl -sX POST "$BASE/v1/resume" >/dev/null
echo
echo "(resumed)"
