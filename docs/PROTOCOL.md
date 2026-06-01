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

## `POST /v1/step?mode=over`

Step *over* a subroutine call. Implementation: disassemble the current
instruction, install a one-shot breakpoint at `PC + instruction_length`,
then resume.

For a JSR/BSR this means "run the called function to completion and
re-pause at the next instruction." For non-call instructions this
behaves the same as `n=1`.

```json
{"ok":true,"mode":"over","oneshot_at":"0x00fc0238","slot":3}
```

## `POST /v1/step?mode=out`

Step *out* of the current function. Reads the long at `(A7)` and treats
that as the return PC; installs a one-shot breakpoint there and resumes.

Works for the common case where the current function was entered via
`JSR` / `BSR` and hasn't yet manipulated `A7` beyond the standard prologue.
Returns `400 Bad Request` if the candidate return address looks unsafe
(odd or zero).

```json
{"ok":true,"mode":"out","oneshot_at":"0x00fc0238","sp":"0x000ff000","slot":3}
```

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

## `POST /v1/breakpoints?addr=HEX&skip=N&oneshot=0|1`

Install a PC breakpoint. The emulator pauses just before executing the
instruction at `addr`. Up to 20 breakpoints can be active simultaneously
(FS-UAE limit).

Optional params:

| Name | Default | Meaning |
|---|---|---|
| `skip` | 0 | Silently ignore the first N hits. The pulse thread auto-resumes for each one. |
| `oneshot` | 0 | Clear this breakpoint automatically the first time it fires. |

```json
{"ok":true,"slot":0,"addr":"0x00fc0234","skip":0,"oneshot":0}
```

After resuming, poll `/v1/state` until `"paused"` to detect a hit.

## `GET /v1/breakpoints`

List currently-active breakpoints with their hit counts and remaining
skip counts.

```json
{"ok":true,"breakpoints":[
  {"slot":0,"addr":"0x00fc0234","skip_remaining":2,"hit_count":3,"oneshot":0}
]}
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

## `GET /v1/memmap`

Walk the FS-UAE bank table and emit one record per contiguous run of
identical 64KB bank pointers. The result is a coarse memory map covering
the full 24-bit or 32-bit address space (depending on the emulated CPU's
`address_space_24` setting).

```json
{"ok":true,"address_space_bits":24,"regions":[
  {"start":"0x00000000","end":"0x001fffff","size":2097152,
   "name":"Chip memory","label":"chip","kind":"chipram","flags":"0x801"},
  {"start":"0x00200000","end":"0x009fffff","size":8388608,
   "name":"<none>","label":"<none>","kind":"unmapped","flags":"0x10"},
  ...
]}
```

`kind` is one of: `chipram`, `ram`, `rom`, `romin`, `io`, `cia`,
`unmapped`, or `unknown`. `flags` is the raw ABFLAG bitmask from
FS-UAE's `addrbank.flags`.

## `GET /v1/stack?depth=N`

Read N longwords from `(A7)` upward and tag each one as `code` or `data`.
Heuristic classification: a word is `code` if it's even, ≥ `0x400`, and
falls inside a bank with `flags` set to RAM / ROM / CHIPRAM.

```json
{"ok":true,"sp":"0x000ff000","depth":8,"words":[
  {"offset":0,"addr":"0x000ff000","value":"0x00fc16e2","kind":"code"},
  {"offset":4,"addr":"0x000ff004","value":"0x00000001","kind":"data"},
  ...
]}
```

`depth` defaults to 32. Range 1..1024.

Use the `kind:"code"` entries as candidate return addresses for a
manual stack walk — there's no enforced frame layout on m68k, so this
is the best generic heuristic.

## `GET /v1/fd/libraries`

List loaded FD libraries.

```json
{"ok":true,"libraries":[
  {"name":"exec","functions":85,"builtin":true},
  {"name":"graphics","functions":216,"builtin":false}
]}
```

## `GET /v1/fd/list?library=NAME`

Full function table for a loaded library (default `library=exec`).

## `POST /v1/fd/load?path=ABS&library=NAME`

Parse an `.fd` file from disk and register it under `library=NAME`.
The path must be absolute. Supported `.fd` directives:

- `##base _LibBase` — informational, recorded but not currently surfaced
- `##bias N` — starting offset (positive; stored as negative)
- `##public` / `##private` — section markers
- `Name(args)(regs)` — one function per line; bias increments by 6
- `##end` — terminator
- Lines beginning `*` — comments

Loading a name that's already registered replaces the previous table
(useful for iterating on a `.fd` during development).

```json
{"ok":true,"library":"graphics","path":"/Users/me/graphics.fd","functions":216}
```

## `GET /v1/fd/lookup?offset=N&library=NAME`

Look up a function by negative offset. `library` defaults to `exec` but
the lookup falls back to *every* registered library if no match in the
preferred one — the response's `library` field tells you which table
won.

```json
{"ok":true,"library":"intuition","offset":-90,"name":"OpenWindow"}
```

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

## GDB Remote Serial Protocol (separate listener)

Enabled by `FSUAE_GDB_PORT=<port>` at fs-uae launch time.  Independent
of `FSUAE_RPC_PORT` — you can run the GDB stub alone, the HTTP API
alone, or both.

The stub speaks RSP over TCP and advertises an m68k target description
via `qXfer:features:read:target.xml`.  Stop events are pushed by the
emulator's pulse path immediately on pause — no client-side polling.

### Quick start

```bash
FSUAE_GDB_PORT=2331 FSUAE_RPC_PAUSE_AT_BOOT=1 ./run.sh
gdb -tui -x tools/fsuae.gdbinit
```

`fsuae.gdbinit` sets `architecture m68k` + `endian big` then connects.
(Endian must be set manually — gdb 17.x does not infer it from the
target description's `<architecture>m68k</architecture>` element alone.)

### Supported packets

| Packet | Meaning |
|---|---|
| `?` | Last stop reason (returns `T05` SIGTRAP + cached SP/PC) |
| `g` / `G` | Read / write all 18 registers (D0–D7, A0–A7, PS, PC) |
| `p NN` / `P NN=V` | Read / write one register by hex index |
| `m A,L` / `M A,L:D` | Read / write up to ~1KB of memory |
| `c` / `s` | Continue / single-step |
| `vCont?` / `vCont;c\|s` | Verbose continue/step |
| `Z0`,`Z1` / `z0`,`z1` | Install / remove software or hardware breakpoint |
| `Z2`,`Z3`,`Z4` / `z2`,`z3`,`z4` | Install / remove write, read, access watchpoint |
| `D` / `k` | Detach / kill (closes the session) |
| `qSupported` | Capabilities (`PacketSize`, `swbreak+`, `hwbreak+`, `qXfer:features:read+`) |
| `qXfer:features:read:target.xml` | Embedded m68k feature description |
| `qAttached`, `qC`, `qfThreadInfo`, `qsThreadInfo`, `qSymbol` | Minimal stubs (single-thread, no symbols) |
| `H g0`, `H c -1` | Set ops/continue thread (no-op, returns OK) |

### Latency

The continue / step path is non-polling: when the emulator pauses
(BP hit, WP hit, step complete, user pause), the worker thread parked
in `gdb_wait_for_pause()` wakes within 1 ms and sends the `T05` stop
reply.

### Why not just `set architecture m68k`?

The stub's target description sets the architecture but gdb 17.x leaves
endianness on host default unless explicitly told.  `tools/fsuae.gdbinit`
issues `set endian big` for this reason.  Without it, register values
display byte-swapped (a sign that endian is wrong, not that the stub
is misbehaving).

## Versioning

The URL prefix `/v1/` is the protocol version. Any breaking change to
request/response shape will land under `/v2/` rather than mutating `/v1/`.
New endpoints may be added to `/v1/` without breaking existing clients.
