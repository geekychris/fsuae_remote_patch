# Architecture

How `fsuae_remote_patch` actually works inside FS-UAE.

## Threads

```
fs-uae process
├── main thread          ──┐
│   • runs SDL event loop │  emulation thread:
│   • drives graphics     │  m68k_run, custom chipset
│                         │  (these are the same thread —
│   ┌─── m68k_run ◀─────  │   FS-UAE is single-threaded
│   │                     │   for emulation)
│   │   (paused here when │
│   │    debugger_active  │
│   │    is non-zero)     │
│                         │
│   debug() ←─────────────┤
│   • spcflag_brk handler │
│   • calls debug_1()     │
│   • calls console_get() │
│     (patched to block   │
│      until /v1/resume)  │
│                         │
├── rpc_worker (pthread)──┤  HTTP/JSON-RPC + WS upgrade
│   • accept() loop       │  socket server on port 8765
│   • handle_request()    │
│   • reads regs/mem      │
│     directly (safe      │
│     while paused)       │
│                         │
└── ws_pulse_thread       ──┘  WebSocket push
    • 50 ms poll loop
    • broadcasts state
      changes to ws clients
```

The emulation thread (`m68k_run`) and the RPC worker run concurrently.
This is safe because **the RPC reads are non-destructive memory snoops**
and the writes are intended to be issued while the emulator is paused.

## Pause semantics

FS-UAE's `debugger_active` global gates the in-process debugger.  When
set, the CPU loop ends up in `debug()` → `debug_1()` → `console_get()`,
which reads from stdin to accept commands.

With `stdin = /dev/null` (the common case for an RPC-driven launch),
`fgets` returns NULL on EOF — vanilla FS-UAE treats this as "exit
debugger", so it would just resume immediately.  The patch changes
`console_get()` to:

  - return -1 (the normal "exit" signal) if `debugger_active == 0`
  - sleep 20 ms and return 0 (empty line, loop continues) otherwise

This means **pause is sticky**: the in-process debugger loop spins on a
20 ms sleep until an RPC client clears `debugger_active` (via
`/v1/resume`).  The RPC worker can read CPU/memory state through the
whole window.

## Watchpoint bank-rewrapping

FS-UAE implements watchpoints by *replacing* the live `mem_banks[]`
entries for affected banks with debug-wrapped versions (debug_lput,
debug_wput, etc.) that call `memwatch_func()` before forwarding to the
real bank.

After `uae_reset()`, `memory_hardreset()` rebuilds `mem_banks[]` with
the *native* handlers, which clobbers the wrap.  The patch adds a
post-reset hook in `newcpu.cpp` that calls `memwatch_setup()` once the
reset processing completes — so any active watchpoints survive a hard
reset transparently.

## Single-step

The trace mechanism mirrors FS-UAE's existing `t` debugger command:

  1. Set `skipaddr_doskip = N` (instructions to run)
  2. Set `no_trace_exceptions = 1`, `exception_debugging = 1`
  3. Set `debugging = 1` (so the CPU re-enters `debug()` next time
     it sees `SPCFLAG_BRK`)
  4. Clear `debugger_active` so `debug_1()` returns
  5. Set `SPCFLAG_BRK` so the CPU breaks at the next instruction
     boundary

The CPU runs N instructions, each one triggering `debug()` which
decrements `skipaddr_doskip` and re-arms `SPCFLAG_BRK` until the count
hits zero.

## Web UI delivery

`web/index.html` is a single-file SPA — vanilla JS, no framework, no
build step.  At fs-uae build time, `tools/embed_html.py` converts it
into `web_index.inc`:

```c++
static const char WEB_UI_HTML[] = R"FSUAEWEBUI(
<!doctype html>
...
)FSUAEWEBUI";
```

`fsuae_rpc.cpp` `#include`s this, and `ep_ui()` serves it on `GET /`
and `GET /v1/ui`.

So the *entire* UI is **inside the fs-uae binary** — no external file
dependency at runtime.

## WebSocket handshake

`/v1/events` is a regular HTTP request that gets upgraded.  The handler:

  1. Reads the request, looks for `Upgrade: websocket`
  2. Extracts the `Sec-WebSocket-Key` header
  3. Concatenates with the RFC magic GUID, runs SHA-1, base64-encodes
  4. Sends the 101 Switching Protocols response
  5. Registers the fd in the broadcast list

The pulse thread polls `debugger_active` and `memwatch_triggered`
every 50 ms, and on change broadcasts a JSON frame to all registered
clients.

SHA-1 and base64 are both implemented inline in `fsuae_rpc.cpp` — no
OpenSSL dependency.  SIGPIPE is ignored process-wide so disconnected
clients don't crash the emulator.

## Symbol resolution

Two complementary tables:

  - **Address symbols** (140 entries): static map of well-known Amiga
    addresses to names.  Includes 68k exception vectors,
    `$DFF000..$DFF1FE` chipset registers, CIA-A and CIA-B registers.
    Used by `GET /v1/symbols/*`.

  - **Function descriptors** (~80 entries for exec): negative offsets
    from `A6` to library function names.  Used by `GET /v1/fd/*` and
    by the `/v1/disasm?annotate=1` post-processor that scans the
    disassembly for `(A6, -$XX)` patterns and inlines
    `; exec.FuncName()` annotations.

## Concurrency safety

The RPC worker accesses FS-UAE globals (`regs`, `dmacon`, `bpnodes`,
`mwnodes`) without locking.  This is safe in practice because:

  - The expected workflow is `/v1/pause` → read state → modify →
    `/v1/resume`.  Reads/writes happen while the emulation thread is
    blocked in `debug_1()`.
  - Reads while running may see torn 16/32-bit values, but won't
    crash — `get_byte_debug()` does single-byte reads through the
    debug bank's check function.
  - Writes while running are racy and may not stick until the
    instruction pipeline drains.  The docs explicitly say "pause
    first" for writes.

For tighter safety we'd need a `pause_emulation()` primitive in FS-UAE
that synchronously fences the emulation thread.  Not currently exposed.
