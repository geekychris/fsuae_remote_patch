# Protocol reference

All endpoints are HTTP/1.1 over a plain TCP socket on `127.0.0.1:$FSUAE_RPC_PORT`.
Responses are JSON, `Content-Type: application/json`, `Connection: close`.

Conventions:

- Numeric query params (`addr`, `len`, etc.) accept three forms:
  decimal (`192`), `0x`-prefixed hex (`0xC0`), or `$`-prefixed hex (`$C0`).
- Every response is either `{"ok":true, ...}` or `{"ok":false, "err":"..."}`.
- Status codes: 200 OK, 400 Bad Request, 404 Not Found, 500 Internal Error.
- Server is single-threaded and processes one request per TCP connection.
  Pipelining is not supported; reconnect for each call.

## `GET /v1/ping`

Smoke test.

```json
{"ok":true,"service":"fs-uae-rpc v1"}
```

## `GET /v1/state`

Returns whether the emulator is currently paused or running. Use this
to poll for breakpoint / watchpoint hits after `/v1/resume` — when the
WP fires, FS-UAE auto-pauses and this flips to `"paused"`.

```json
{"ok":true,"state":"running"}
```

Possible values: `"running"`, `"paused"`.

## `GET /v1/cpu`

68k CPU register snapshot. See README for field list. For stable reads,
`POST /v1/pause` first.

## `GET /v1/mem?addr=HEX&len=N`

Read N bytes (1..65536) of emulated memory. Returns hex string.

## `POST /v1/pause`

Stops the emulator by calling `activate_debugger()`. Returns
`{"ok":true,"state":"paused"}`.

## `POST /v1/resume`

Resumes emulation by calling `deactivate_debugger()`. Returns
`{"ok":true,"state":"running"}`.

If any watchpoints are installed, this automatically calls
`memwatch_setup()` first to re-wrap the memory banks. This is needed
because `/v1/reset` re-initializes the bank dispatch table and would
otherwise leave watchpoints dead.

## `POST /v1/step?n=N`

Execute N instructions then re-pause. Default `n=1`. Maps to FS-UAE's
internal `t` trace command.

```json
{"ok":true,"stepped":1}
```

After this returns, poll `/v1/state` until `"paused"` (usually
sub-millisecond for small N). Then read `/v1/cpu` for the new PC.

**Note:** for `n>1`, watchpoints and breakpoints are still honored
mid-step — if a WP/BP fires before the step count is reached, the
emulator pauses early. Check `/v1/state` and `/v1/cpu` after each step
to determine why it paused.

## `POST /v1/watchpoints/rearm`

Force a re-call of `memwatch_setup()` without changing any watchpoint
state. Use after `/v1/reset` if you don't immediately call `/v1/resume`
(which auto-rearms).

```json
{"ok":true}
```

## `POST /v1/state/save?path=ABS_PATH`

Save state snapshot to disk. `path` must be absolute. Produces a
standard `.uss` file (same format as F11 → save in the GUI).

## `POST /v1/reset?hard=1|0`

Trigger an emulator reset.

- `hard=1` (default): power-on-style reset (RAM cleared)
- `hard=0`: soft reset (keyboard CTRL+A+A equivalent)

Note: this calls `uae_reset()` which is **async** — it sets a flag the
main loop picks up at its next tick. After this returns, the actual
reset takes one or more emulator frames to complete. If you're chaining
this with watchpoints, allow ~100 ms of wallclock before polling state.

## `POST /v1/breakpoints?addr=HEX`

Install a PC breakpoint. The emulator pauses just before executing the
instruction at `addr`. Up to 20 breakpoints can be active simultaneously
(FS-UAE limit).

```json
{"ok":true,"slot":0,"addr":"0x00fc0234"}
```

After resuming, poll `/v1/state` until `"paused"` to detect a hit.

## `GET /v1/breakpoints`

List currently-active breakpoints.

```json
{"ok":true,"breakpoints":[{"slot":0,"addr":"0x00fc0234"}]}
```

## `POST /v1/breakpoints/clear`

Remove all breakpoints.

```json
{"ok":true,"cleared":1}
```

## `POST /v1/watchpoints?addr=HEX&size=N&rwi=R|W|I|RW|RWI&mustchange=0|1`

Install a memory watchpoint. The emulator pauses when any access in
range `[addr, addr+size)` matches the requested rwi (Read / Write /
Instruction-fetch). Up to 20 watchpoints can be active.

Query params:

| Name | Required | Default | Notes |
|---|---|---|---|
| `addr` | yes | — | start of watched range |
| `size` | no | 1 | bytes covered (`64` for 16 longwords) |
| `rwi` | no | `RW` | any combination of R, W, I (e.g. `W`, `RW`, `RWI`) |
| `mustchange` | no | `0` | `1` → only fire if write actually changes memory (skip writes-of-same-value) |

```json
{"ok":true,"slot":0,"addr":"0x000000c0","size":64,"rwi":2}
```

The `rwi` field in responses is the FS-UAE internal bitmask: R=1, W=2, I=4.

**Behavioural note:** FS-UAE's memwatch implementation rewrites the
memory bank dispatch table to intercept accesses. The first time you
install any watchpoint, the system initializes itself — there may be a
small one-time delay (~1 ms). Subsequent installs are fast.

**Behavioural note 2:** A reset (`POST /v1/reset`) re-initializes the
memory banks, which clobbers the watchpoint interception. After a reset
you must re-issue your watchpoint installs.

## `GET /v1/watchpoints`

List currently-active watchpoints.

## `POST /v1/watchpoints/clear`

Remove all watchpoints.

## Error responses

All non-success responses look like `{"ok":false,"err":"<short message>"}`.
Common messages:

| Message | Status | Cause |
|---|---|---|
| `missing addr` | 400 | required query param absent |
| `bad addr` / `bad len` / `bad size` | 400 | numeric parse failed |
| `bad rwi (use R, W, I, or combos)` | 400 | unknown rwi character |
| `len out of range (1..65536)` | 400 | `/v1/mem` len bound |
| `size out of range` | 400 | watchpoint size bound (1..16777216) |
| `no free breakpoint slot` | 500 | 20 breakpoints already active |
| `no free watchpoint slot` | 500 | 20 watchpoints already active |
| `missing path` | 400 | `/v1/state/save` without `path` |
| `save_state failed` | 500 | disk full, bad path, etc. |
| `no such endpoint` | 404 | unknown method/path combo |
| `malformed` | 400 | could not parse HTTP request line |

## Versioning

The URL prefix `/v1/` is the protocol version. Any breaking change to
request/response shape will land under `/v2/` rather than mutating `/v1/`.
New endpoints may be added to `/v1/` without breaking existing clients.
