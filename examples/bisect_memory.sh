#!/usr/bin/env bash
# bisect_memory.sh — find when a target memory region transitions away
# from a known baseline value.
#
# Usage:
#   bisect_memory.sh <addr> <len> <max_wait_secs> [<poll_interval>]
#
# Example: watch chip RAM $C0-$FF for any non-zero byte over up to 30 s,
# polling every 0.5 s:
#
#   examples/bisect_memory.sh 0xC0 64 30 0.5
#
# When the region first changes, the script pauses the emulator and
# reports the CPU state at that moment.  Useful for finding where a
# particular ROM routine writes to a particular target address.

set -euo pipefail

ADDR="${1:?usage: $0 <addr> <len> <max_wait_secs> [<poll_interval>]}"
LEN="${2:?usage: $0 <addr> <len> <max_wait_secs> [<poll_interval>]}"
MAX="${3:?usage: $0 <addr> <len> <max_wait_secs> [<poll_interval>]}"
POLL="${4:-0.5}"

PORT="${FSUAE_RPC_PORT:-8765}"
BASE="http://127.0.0.1:$PORT"

read_hex() {
    curl -s "$BASE/v1/mem?addr=$ADDR&len=$LEN" \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["hex"])'
}

# Snapshot the baseline
curl -sX POST "$BASE/v1/pause" >/dev/null
sleep 0.05
BASELINE="$(read_hex)"
echo "baseline @ $ADDR (len $LEN): $BASELINE"
curl -sX POST "$BASE/v1/resume" >/dev/null

# Poll until it changes or we hit max
END=$(($(date +%s) + MAX))
while (( $(date +%s) < END )); do
    sleep "$POLL"
    CUR="$(read_hex)"
    if [[ "$CUR" != "$BASELINE" ]]; then
        echo
        echo "*** changed @ $(date +%H:%M:%S)"
        echo "now:  $CUR"
        echo
        curl -sX POST "$BASE/v1/pause" >/dev/null
        sleep 0.05
        echo "--- CPU at change ---"
        curl -s "$BASE/v1/cpu" | python3 -m json.tool
        echo
        echo "(emulator left paused — use POST /v1/resume to continue)"
        exit 0
    fi
done

echo "no change within ${MAX} s"
exit 1
