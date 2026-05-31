# Roadmap

What's in vs. what's not, and concrete next steps.

## Done

- HTTP/JSON-RPC backend (pause, resume, mem, cpu, state, reset, step)
- Watchpoints (with mustchange + value-match filters, post-reset survival)
- Breakpoints
- Disassembly (with optional FD annotation)
- Chipset register snapshot (`/v1/custom`)
- State save / load
- Memory write, CPU register write
- Symbol resolution: ~140 static addresses + ~80 exec.library FD entries
- WebSocket event stream (pause/resume/wp_hit broadcast)
- Embedded web UI (single-file vanilla JS, no build tooling)
- Sticky pause via patched `console_get` (works with `stdin=/dev/null`)
- Cross-platform build script (macOS + Linux deps auto-installed)

## Next (small, high-impact)

These should each be 1–2 hour additions:

- **`POST /v1/fd/load?path=ABS&library=NAME`** — load any `.fd` file at
  runtime, not just the built-in exec.fd.  Would enable annotation of
  graphics.library, intuition.library, dos.library, etc.

- **Conditional breakpoints** — `POST /v1/breakpoints?addr=X&cond=...`
  with a tiny expression language (`a0 == 0xC094D4`, `*$4 != 0`, etc.).
  Currently you have to break unconditionally and filter client-side.

- **`/v1/breakpoints/skip?count=N`** — break only after the Nth hit.
  For heavily-used routines like `CopyMem` where you want the 47th
  call, not the first.

- **`POST /v1/exec`** — run an arbitrary 68k instruction by writing
  opcode + operands + setting PC.  Useful for "run this function"
  workflows without modifying memory.

- **Memory map endpoint** — `GET /v1/memmap` returns region descriptors
  (chip / slow / fast / ROM / IO / unmapped) so frontends can render a
  memory map at a glance.

- **Step over / step out** — extend `/v1/step` with `mode=over` or
  `mode=out` for source-level navigation.  Implementation: set a one-shot
  BP at `PC + insn_len` (over) or stack-top (out), then resume.

## Medium (each ~1 day)

- **Multiple library FD support** — extend the static table to a
  dictionary keyed by library name.  Annotator uses A6 history
  heuristics or explicit library hint param to pick the right table.
  Would need `MOVEA.L X(A4), A6` tracking ideally.

- **Hunk-format executable loader** — parse Amiga hunk binaries to
  extract debug data (HUNK_DEBUG, source line tables).  Map PCs to
  source files + lines for code compiled with `vasm -dwarf` or
  `m68k-amigaos-gcc -g`.

- **Live memory viewer in the UI** — current UI fetches once per
  user request.  Auto-refresh on emulator pause; highlight changed
  bytes since last snapshot.

- **Stack walker** — `GET /v1/stack?depth=N` returns frames by
  following the A7 / A5 chain through saved-PC + saved-A5 conventions.
  Tricky on m68k because there's no enforced frame format, but works
  well enough for code compiled with GCC -fno-omit-frame-pointer.

- **Snapshot diff tooling** — given two `.uss` files, summarise which
  memory regions and registers changed.  Useful for bisection
  workflows.

- **MCP server wrapper** — Node.js or Python MCP that exposes
  `/v1/*` as MCP tools (`fsuae.pause`, `fsuae.read_mem`, etc.) so
  LLM agents can drive fs-uae directly.

## Larger (each ~1 week)

- **GDB stub** — translate GDB remote protocol to `/v1/*` so existing
  GDB-aware tools (Eclipse, VS Code's cortex-debug, etc.) can attach.
  Would need careful packetisation but the underlying primitives are
  all there.

- **Source-level debugging via DWARF** — parse DWARF debug info from
  m68k-amigaos-gcc binaries, map every PC to (file, line, column).
  Show source side-by-side in the web UI when debugging a `-g` build.

- **Time-travel debugging** — leverage FS-UAE's existing
  `savestate_capture` to record state every N frames; expose
  `POST /v1/state/rewind?frames=N` and `POST /v1/state/replay` to
  navigate the timeline.

- **Windows port** — `claude_rpc.cpp` currently compiles to a no-op
  stub on `_WIN32`.  Port using `_beginthreadex` + Winsock2 instead
  of pthread + BSD sockets; the HTTP parser / endpoint logic carries
  over unchanged.

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
