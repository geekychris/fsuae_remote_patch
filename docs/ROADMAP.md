# Roadmap

What's in vs. what's not, and concrete next steps.

## Done

- HTTP/JSON-RPC backend (pause, resume, mem, cpu, state, reset, step)
- Watchpoints (with mustchange + value-match filters, post-reset survival)
- Breakpoints (with **skip-count** and **one-shot** modes)
- Disassembly (with multi-library FD annotation)
- Chipset register snapshot (`/v1/custom`)
- State save / load
- Memory write, CPU register write
- Symbol resolution: ~140 static addresses + exec.library built-in FD
- WebSocket event stream (pause/resume/wp_hit/bp broadcast)
- Embedded web UI (single-file vanilla JS, no build tooling)
- Sticky pause via patched `console_get` (works with `stdin=/dev/null`)
- Cross-platform build script (macOS + Linux deps auto-installed)
- **Memory map endpoint** (`/v1/memmap`) — walks `mem_banks[]` and emits
  chip/slow/fast/ROM/IO/CIA/unmapped region descriptors
- **Stack walker** (`/v1/stack?depth=N`) — reads `(A7)`+, tags each
  word as `code` / `data` so callers can manual-walk frames
- **Step over / step out** — `/v1/step?mode=over|out` installs a
  one-shot BP at PC+insn_len or `(A7)` then resumes
- **Multi-library FD load** — `/v1/fd/load?path=&library=` parses
  `.fd` files at runtime; disasm annotator searches all loaded libs
- **MCP server wrapper** — `tools/mcp_fsuae.py` exposes 26 fsuae_rpc
  endpoints as MCP tools (stdio JSON-RPC, no extra deps)
- **Snapshot diff tooling** — `tools/uss_diff.py` parses the
  IFF-style `.uss` format and reports per-chunk byte-level deltas
- **GDB stub (in-process, C++)** — second TCP listener built into the
  patch itself (`FSUAE_GDB_PORT`).  Speaks GDB Remote Serial Protocol
  with an embedded m68k target description; stop events pushed via the
  pulse thread (no polling).  Reads regs / memory / installs BPs and
  WPs directly through FS-UAE internals — no HTTP round-trip.

## Next (small, high-impact)

These should each be 1–2 hour additions:

- **Conditional breakpoints** — `POST /v1/breakpoints?addr=X&cond=...`
  with a tiny expression language (`a0 == 0xC094D4`, `*$4 != 0`, etc.).
  Currently you have to break unconditionally and filter client-side.

- **`POST /v1/exec`** — run an arbitrary 68k instruction by writing
  opcode + operands + setting PC.  Useful for "run this function"
  workflows without modifying memory.

- **`/v1/breakpoints/<slot>/clear`** — clear a single BP by slot id.
  The gdb_bridge currently has to rebuild the whole list on every
  `z0` packet because there's no per-BP clear endpoint.

## Medium (each ~1 day)

- **Hunk-format executable loader** — parse Amiga hunk binaries to
  extract debug data (HUNK_DEBUG, source line tables).  Map PCs to
  source files + lines for code compiled with `vasm -dwarf` or
  `m68k-amigaos-gcc -g`.

- **Live memory viewer in the UI** — current UI fetches once per
  user request.  Auto-refresh on emulator pause; highlight changed
  bytes since last snapshot.

- **A6 tracking for the disasm annotator** — currently the annotator
  searches every loaded library when it sees `JSR -nn(A6)`.  Track
  the last `MOVEA.L X(Ai),A6` and prefer the corresponding library
  to disambiguate when multiple libs share the same offset.

## Larger (each ~1 week)

- **Source-level debugging via DWARF** — parse DWARF debug info from
  m68k-amigaos-gcc binaries, map every PC to (file, line, column).
  Show source side-by-side in the web UI when debugging a `-g` build.

- **Time-travel debugging** — leverage FS-UAE's existing
  `savestate_capture` to record state every N frames; expose
  `POST /v1/state/rewind?frames=N` and `POST /v1/state/replay` to
  navigate the timeline.

- **Windows port** — *compiles*, not yet runtime-verified.  The
  cross-platform shim at the top of `fsuae_rpc.cpp` maps pthread →
  Win32 SRWLOCK + CONDITION_VARIABLE + `_beginthreadex`, BSD sockets
  → Winsock2, `usleep` → `Sleep`.  `WSAStartup()` runs in
  `fsuae_rpc_init` on `_WIN32`.  Needs someone with an MSYS2/MinGW
  Windows environment to build + smoke-test the full HTTP and GDB
  paths.

- **Upstream PR** — submit a polished version to
  [FrodeSolheim/fs-uae](https://github.com/FrodeSolheim/fs-uae) so this
  becomes a configure-time option (`./configure --enable-rpc`) rather
  than a patch.

## Explicitly out of scope

- Authentication / HTTPS — localhost-only by design.  If you need
  remote access, SSH-tunnel the port.
- Multi-emulator support — one fs-uae per port.  Multiple instances
  trivially supported by varying `FSUAE_RPC_PORT`.
- Browser-based emulator UI — that's a different project.  The web
  UI here is for *debugger* operations, not gameplay.

## Wishlist (no immediate plans)

- Plugin API for custom annotators (e.g. Workbench struct decoders,
  graphics.library BitMap visualisers).
- Performance profiling — sample PC at intervals, build a flamegraph.
- Disk image hex editor through the API.
- AROS / Kickstart 3.x symbol files imported automatically.
