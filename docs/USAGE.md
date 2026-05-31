# Usage cookbook

Recipes for common Amiga-debugging tasks using the fsuae_rpc API.

All examples assume `FSUAE_RPC_PORT=8765` and `BASE=http://127.0.0.1:8765`.

## Get started

```sh
# One-time: build the patched fs-uae binary
./build.sh

# Launch the debugger
./run.sh ~/Documents/FS-UAE/Configurations/MyAmiga.fs-uae
# → opens http://127.0.0.1:8765/ in your browser
```

The UI is fully functional from there.  The recipes below are for
scripting / automation use cases.

## Pause and inspect

```sh
curl -sX POST $BASE/v1/pause
curl -s $BASE/v1/cpu              # registers
curl -s $BASE/v1/custom           # chipset state
curl -s "$BASE/v1/mem?addr=0xC0&len=64"          # raw memory
curl -s "$BASE/v1/disasm?addr=pc&count=20&annotate=1"
```

## Step one instruction

```sh
curl -sX POST "$BASE/v1/step?n=1"
# Step 1000 in a single call:
curl -sX POST "$BASE/v1/step?n=1000"
```

## Set a breakpoint and run until it hits

```sh
curl -sX POST "$BASE/v1/breakpoints?addr=0xFC0234"
curl -sX POST $BASE/v1/resume

# Poll until paused:
while [[ "$(curl -s $BASE/v1/state | jq -r .state)" != "paused" ]]; do
    sleep 0.1
done
echo "BP hit at $(curl -s $BASE/v1/cpu | jq -r .pc)"
```

Or use the WebSocket event stream to avoid polling — see PROTOCOL.md.

## Watch a memory location

```sh
# Pause on any write to chip $C0..$C3
curl -sX POST "$BASE/v1/watchpoints?addr=0xC0&size=4&rwi=W&mustchange=1"
curl -sX POST $BASE/v1/resume

# When the WP fires, get the actual triggering PC:
curl -s $BASE/v1/watchpoints/last | jq
# {"ok":true,"hit":true,"addr":"0x000000c0","pc":"0xfc02a8",
#  "rwi":"W","size":4,"value":"0x00c096dc",...}
```

The `mustchange=1` filter is important: writes-of-same-value don't
fire.  Combine with `val=HEX&valmask=HEX` to fire only on a specific
value being written.

## Look up addresses + library functions

```sh
# Symbolic table — chipset regs, 68k vectors, CIA regs
curl -s "$BASE/v1/symbols/lookup?addr=0xDFF096"
# {"ok":true,"addr":"0x00dff096","name":"DMACON",...}

# exec.library function names (for JSR -nn(A6))
curl -s "$BASE/v1/fd/lookup?offset=-132"
# {"ok":true,"library":"exec","offset":-132,"name":"Forbid"}
```

## Snapshot and restore

```sh
# Save current state
curl -sX POST "$BASE/v1/state/save?path=/tmp/snap.uss"

# Later: restore it
curl -sX POST "$BASE/v1/state/load?path=/tmp/snap.uss"
```

## Modify CPU registers and memory

```sh
# Pause first
curl -sX POST $BASE/v1/pause

# Force D0 to a specific value
curl -sX POST "$BASE/v1/cpu?reg=d0&value=0x12345678"

# Force PC (jump somewhere)
curl -sX POST "$BASE/v1/cpu?reg=pc&value=0xFC0000"

# Write bytes to memory (zero out chip $C0..$FF)
curl -sX POST "$BASE/v1/mem?addr=0xC0&hex=00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"

# Read it back
curl -s "$BASE/v1/mem?addr=0xC0&len=64"
```

## Reset the emulator

```sh
curl -sX POST "$BASE/v1/reset?hard=1"     # power-on reset (memory wiped)
curl -sX POST "$BASE/v1/reset?hard=0"     # soft reset (keeps memory)
```

After a reset, watchpoints survive automatically — the patch hooks the
reset path in `newcpu.cpp` to re-call `memwatch_setup()` once banks are
re-initialized.

## Drive everything from Python

```python
import json, urllib.request

BASE = "http://127.0.0.1:8765"

def call(method, path):
    req = urllib.request.Request(BASE + path, method=method)
    return json.load(urllib.request.urlopen(req))

call("POST", "/v1/pause")
pc = call("GET", "/v1/cpu")["pc"]
print(f"paused at {pc}")
call("POST", "/v1/step?n=100")
print("stepped 100 instructions")
call("POST", "/v1/resume")
```

The full sequence of working examples is in `examples/`.

## Event-driven (WebSocket)

```python
import websocket, json

ws = websocket.create_connection("ws://127.0.0.1:8765/v1/events")
while True:
    frame = json.loads(ws.recv())
    print(frame)
    # {"event":"paused","pc":"0xfc0234","reason":"wp"}
    # {"event":"running"}
    # {"event":"wp_hit","slot":0,"addr":"0xc0","pc":"0xfc02a8",...}
```

The pulse thread inside fs-uae broadcasts these events at ~20 Hz
(50 ms granularity).  Use this instead of polling `/v1/state` for
tight bisection loops.
