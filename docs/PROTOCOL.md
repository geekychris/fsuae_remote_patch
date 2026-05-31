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

## `GET /v1/cpu`

68k CPU register snapshot.

Response fields (all values are zero-padded 32-bit hex strings, SR is 16-bit):

| Field | Source |
|---|---|
| `pc` | `m68k_getpc()` |
| `sr` | `regs.sr & 0xFFFF` |
| `d0`..`d7` | `regs.regs[0..7]` |
| `a0`..`a7` | `regs.regs[8..15]` |
| `usp` | `regs.usp` |
| `isp` | `regs.isp` |

Example:

```json
{
  "ok":true,
  "pc":"0x00fc0fdc","sr":"0x2000",
  "d0":"0x00c00400","d1":"0x00000000", "...": "...",
  "a0":"0x00c0040c","a1":"0x00c00410", "...": "...",
  "a7":"0x00c80000",
  "usp":"0x00c06248","isp":"0x00c80000"
}
```

**Caveat:** A7 is the *active* stack pointer for the current mode; `usp`/`isp`
are the saved alternates. While in supervisor mode, `a7 == isp` and `usp`
holds the user stack you'll swap back to.

For stable reads, `POST /v1/pause` first.

## `GET /v1/mem?addr=HEX&len=N`

Read N bytes of emulated memory starting at `addr`.

Query params:

| Name | Required | Default | Range |
|---|---|---|---|
| `addr` | yes | — | full 32-bit address space |
| `len` | no | 64 | 1..65536 |

Reads use `get_byte_debug()` — the same byte-read path the FS-UAE in-process
debugger uses (no side effects, no I/O strobes for chipset register
reads, no exceptions on bad addresses).

Response:

```json
{
  "ok":true,
  "addr":"0x000000c0",
  "len":64,
  "hex":"00000000000000000000000000000000..."
}
```

The `hex` field is `len * 2` lowercase hex chars (no spaces, no `0x`,
big-endian byte order = same order as the bytes in emulated RAM).

Decoding examples:

```python
import json, urllib.request
r = json.loads(urllib.request.urlopen(
    f"http://127.0.0.1:8765/v1/mem?addr=0xC0&len=64").read())
data = bytes.fromhex(r["hex"])
# data is now a 64-byte bytes object

# As 16 big-endian 32-bit vectors (the 68k vector table format):
import struct
vectors = struct.unpack(">16I", data)
```

For stable reads, `POST /v1/pause` first.

## `POST /v1/pause`

Stops the emulator by calling `activate_debugger()` internally. The emulator
will halt at the next instruction boundary.

```json
{"ok":true,"state":"paused"}
```

The response returns immediately (the debugger flag is set synchronously),
but a few in-flight emulation cycles may complete after the response is
sent. For tight timing, pause then wait ~10 ms before reading state.

## `POST /v1/resume`

Resumes emulation by calling `deactivate_debugger()`.

```json
{"ok":true,"state":"running"}
```

## `POST /v1/state/save?path=ABS_PATH`

Save state snapshot to disk. `path` must be an absolute path the FS-UAE
process can write to. Produces a standard `.uss` file (the same format
FS-UAE writes when you press F11 → save).

```json
{"ok":true,"path":"/tmp/snap.uss"}
```

On failure:

```json
{"ok":false,"err":"save_state failed"}
```

The file is typically 0.5–2 MB for a 512 KB chip + 512 KB slow A500
configuration.

## Error responses

All non-success responses look like:

```json
{"ok":false,"err":"<short message>"}
```

Common messages:

| Message | Status | Cause |
|---|---|---|
| `missing addr` | 400 | `/v1/mem` without `addr` query param |
| `bad addr` | 400 | `addr` failed to parse as integer |
| `bad len` | 400 | `len` failed to parse as integer |
| `len out of range (1..65536)` | 400 | `len` outside allowed window |
| `missing path` | 400 | `/v1/state/save` without `path` param |
| `save_state failed` | 500 | `save_state()` returned 0 (disk full, bad path, …) |
| `no such endpoint` | 404 | Unknown method/path combination |
| `malformed` | 400 | Could not parse HTTP request line |

## Versioning

The URL prefix `/v1/` is the protocol version. Any breaking change to
request/response shape will land under `/v2/` rather than mutating `/v1/`.
New endpoints may be added to `/v1/` without breaking existing clients.
