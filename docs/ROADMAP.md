# Roadmap

Things explicitly out of v1, in rough priority order.

## v2 candidates

### Single-step

`POST /v1/step` — execute one 68k instruction and re-pause.

Implementation: wrap `debugger_step()` / similar in `src/debug.cpp`. Some
care needed because FS-UAE's debugger drives stepping by re-entering the
in-process command-prompt loop; we'd need a non-interactive entry point.

### PC breakpoints with auto-pause

```
POST /v1/breakpoints?addr=HEX     install breakpoint at addr
GET  /v1/breakpoints              list active breakpoints
DELETE /v1/breakpoints?addr=HEX   remove breakpoint
```

Implementation: FS-UAE already has breakpoint state in `debug.cpp` —
expose it. The trickier part is notifying the client when a breakpoint
hits (see "event stream" below) — for a pure-polling v2 we can just
expose a `/v1/state` endpoint that says `paused` / `running` and let
clients poll.

### Memory write

`POST /v1/mem?addr=HEX&hex=DEADBEEF` — write bytes from a hex string.

Implementation: wrap `put_byte_debug()`. Need length bounds and a
sanity check that we're not stomping ROM (or allow it explicitly with
`?force=1`).

### Register write

```
POST /v1/cpu/d0?value=HEX
POST /v1/cpu/pc?value=HEX
```

Implementation: write into `regs.regs[i]` and update `m68k_setpc()`.
Sanity-check by reading back. Should only be safe while paused.

### State load

`POST /v1/state/load?path=ABS_PATH`

Implementation: wrap `restore_state()`. Mirror image of `state/save`.

### Disassemble

`GET /v1/disasm?addr=HEX&n=N` — disassemble N instructions at addr.

Implementation: FS-UAE has `m68k_disasm()` (output is normally to a
`FILE*` for the debugger console — would need a capture variant).

### Chipset register snapshot

`GET /v1/custom` — DMACON, INTENA, INTREQ, COPxLC, BPLxPT, etc.

Implementation: read out of `custom_storage[]` / live `dmacon`/`intena`
globals. The set of fields is a design call; the most useful is probably
"everything that shows up in the FS-UAE debugger's `od` (output dma)
command".

## v3 / larger features

### WebSocket event stream

Connect once, get notifications:

```json
{"event":"breakpoint","addr":"0x00FC1234","pc":"0x00FC1234"}
{"event":"paused","reason":"user"}
{"event":"crash","vector":"0x00000010","pc":"0x00BADC0D"}
```

Implementation: significant. Needs proper WebSocket framing + a way for
the emulation thread to signal the worker thread without deadlocks.
Probably worth doing as a separate `/v1/events` endpoint that upgrades
to WebSocket on `GET`.

### Windows support

`fsuae_rpc.cpp` currently has an `#ifdef _WIN32 → no-op stub` guard.
A real Win32 implementation would use `_beginthreadex` + WinSock2 instead
of `pthread` + BSD sockets. The HTTP parsing / dispatch / endpoint
implementations are platform-neutral and would carry over unchanged.

### MCP server wrapper

A Node or Python MCP server that translates Model Context Protocol tool
calls into HTTP requests, so an LLM agent (Claude Desktop, Codex CLI, …)
can natively drive FS-UAE. The HTTP surface is intentionally simple
enough that the MCP wrapper would be a few hundred lines.

### Upstream PR

Eventually submit a polished version to
[FrodeSolheim/fs-uae](https://github.com/FrodeSolheim/fs-uae) so this is
just a configure-time option (`./configure --enable-rpc`) instead of a
separate patch. Probably needs:

- Configure-script integration (`AC_ARG_ENABLE([rpc])`)
- An option to make port + bind-addr configurable via FS-UAE's config
  file rather than only the env var
- Documentation in FS-UAE's own docs/

## Not planned

- **Authentication.** Localhost-only by design. If you need remote
  access, put it behind SSH or a reverse proxy you control.
- **HTTPS.** Same reason.
- **Multi-client concurrency.** The emulator state is a single
  shared resource; serialising requests at the HTTP layer is the
  right model.
- **Browser UI.** That's a job for a separate frontend that calls this
  HTTP API.
