# fsuae_remote_patch

A patch + drop-in source file that turns [FS-UAE](https://fs-uae.net/) into a
full cross-platform Amiga debugger backend, with an HTTP/JSON-RPC API, a
WebSocket event stream, a built-in web UI, and Amiga-aware symbol resolution.

Out of the box, FS-UAE only exposes its debugger interactively in the GUI
window (you press `Pause` then `` ` `` to drop into the prompt). That makes it
hard to drive from scripts, MCP servers, CI harnesses, or any sort of
automated bisection / inspection workflow.

This project adds a minimal HTTP server inside FS-UAE itself, so external
tools can:

- pause / resume the emulator
- read the 68k CPU registers
- read emulated memory at any address
- save state snapshots to disk

…all over plain HTTP+JSON on `localhost`, with no GUI interaction.

The patch is **off by default** — set `FSUAE_RPC_PORT=8765` (or any port)
to enable it. Without that env var, the patched binary is byte-for-byte
behaviourally identical to upstream FS-UAE.

## Quick start (macOS / Linux)

```sh
git clone git@github.com:geekychris/fsuae_remote_patch.git
cd fsuae_remote_patch

# Build the patched fs-uae binary (one-time, ~12 s on Apple Silicon)
./build.sh

# Launch with the web UI auto-opened in your browser
./run.sh ~/Documents/FS-UAE/Configurations/MyAmiga.fs-uae
```

That's it — you now have a full Amiga debugger in your browser at
`http://127.0.0.1:8765/`.  Live CPU registers, disassembly with
exec.library function-name annotations, memory hex dump, chipset
state, point-and-click breakpoints and watchpoints.

For scripting / automation:

```sh
BASE=http://127.0.0.1:8765
curl -s $BASE/v1/ping
# {"ok":true,"service":"fs-uae-rpc v1"}

curl -sX POST $BASE/v1/pause
curl -s $BASE/v1/cpu
# {"ok":true,"pc":"0x00fc0fdc","sr":"0x2000",...}

curl -s "$BASE/v1/mem?addr=0xC0&len=64"
curl -sX POST $BASE/v1/resume
```

See:
- [`docs/USAGE.md`](docs/USAGE.md) — cookbook of common workflows
- [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — full endpoint reference
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how the patch works inside FS-UAE
- [`docs/ROADMAP.md`](docs/ROADMAP.md) — what's next
- [`examples/`](examples/) — runnable scripts

## What's in this repo

```
fsuae_remote_patch/
├── README.md                         this file
├── LICENSE                           GPL-2.0 (matches FS-UAE)
├── build.sh                          one-shot clone + patch + build
├── fsuae_rpc.cpp                     drop-in source file (~325 lines, no deps)
├── patches/
│   └── 0001-fsuae-rpc-hook.patch     2-file unified diff (Makefile.am + main.cpp)
├── docs/
│   ├── PROTOCOL.md                   endpoint reference + JSON schemas
│   └── ROADMAP.md                    v2 features (breakpoints, single-step, write)
└── examples/
    ├── ping.sh                       smoke test
    ├── dump_cpu.sh                   pause + dump regs
    ├── bisect_memory.sh              find when a memory location changes
    └── snapshot_loop.py              save state every N seconds
```

## How does it work?

`fsuae_rpc.cpp` registers an HTTP server in a dedicated `pthread` worker,
created from `fsuae_rpc_init()` which the patch hooks into `real_main2()`
just before `start_program()`. The server binds **127.0.0.1 only** — there
is no remote-network exposure.

Each endpoint is a thin wrapper around an existing internal FS-UAE function:

| Endpoint | Wraps |
|---|---|
**Web UI**

| Endpoint | Returns |
|---|---|
| `GET /` or `GET /v1/ui` | self-contained single-page debugger UI (HTML/JS/CSS embedded in the binary) |

Open `http://127.0.0.1:8765/` in any browser — live CPU regs, disasm,
memory hex dump, chipset state, breakpoints, watchpoints, all driven by
the JSON-RPC API and the WebSocket event stream.

**WebSocket event stream**

| Endpoint | Returns |
|---|---|
| `GET /v1/events` (with `Upgrade: websocket`) | push channel for `paused`, `running`, `wp_hit` events |

Frames are plain JSON, one per WebSocket message.  Eliminates polling
`/v1/state` in tight loops.

**Execution control**

| Endpoint | Wraps |
|---|---|
| `GET /v1/ping` | — |
| `GET /v1/state` | `debugger_active` (poll for BP/WP hits after resume) |
| `POST /v1/pause` | `activate_debugger()` |
| `POST /v1/resume` | `deactivate_debugger()` + auto-rearm watchpoints |
| `POST /v1/step?n=N` | trace-step N instructions then re-pause |
| `POST /v1/reset?hard=0\|1` | `uae_reset()` (memwatch survives via post-reset hook) |

**Inspection**

| Endpoint | Wraps |
|---|---|
| `GET /v1/cpu` | `m68k_getpc()`, `regs.regs[0..15]`, `regs.sr`, `regs.usp`, `regs.isp` |
| `GET /v1/mem?addr&len` | `get_byte_debug()` — same byte-read as the in-process debugger |
| `GET /v1/disasm?addr&count` | `m68k_disasm_2()` into JSON lines |
| `GET /v1/custom` | DMACON, INTENA/REQ, BPLCON0, COPxLC, BPLxPT, beam pos, … |

**Mutation** (pause first for safety)

| Endpoint | Wraps |
|---|---|
| `POST /v1/mem?addr&hex` | `debug_write_memory_8()` per byte |
| `POST /v1/cpu?reg&value` | `regs.regs[]`, `m68k_setpc()`, `MakeFromSR()` |

**State snapshots**

| Endpoint | Wraps |
|---|---|
| `POST /v1/state/save?path` | `save_state()` |
| `POST /v1/state/load?path` | `restore_state()` |

**Breakpoints & watchpoints**

| Endpoint | Wraps |
|---|---|
| `POST /v1/breakpoints?addr` | writes `bpnodes[]` directly |
| `GET /v1/breakpoints` | reads `bpnodes[]` |
| `POST /v1/breakpoints/clear` | clears all `bpnodes[]` |
| `POST /v1/watchpoints?addr&size&rwi&mustchange&val&valmask` | writes `mwnodes[]` + `memwatch_setup()` |
| `GET /v1/watchpoints` | reads `mwnodes[]` |
| `GET /v1/watchpoints/last` | last triggered WP — addr, PC, value (real trigger PC, not post-IRQ PC) |
| `POST /v1/watchpoints/clear` | clears all `mwnodes[]` |
| `POST /v1/watchpoints/rearm` | force re-call of `memwatch_setup()` |

**Symbol resolution** (built-in Amiga register names + 68k vectors)

| Endpoint | Returns |
|---|---|
| `GET /v1/symbols` | full table (~140 entries) — chipset regs, CIA-A/B, 68k vectors |
| `GET /v1/symbols/lookup?addr=X` | name + description for that address (`null` if unknown) |

The patch also:

- Un-`static`s `memwatch_setup()`, `initialize_memwatch()`,
  `skipaddr_doskip`, `mwhit`, and `memwatch_triggered` in `debug.cpp`
  so the RPC can install watchpoints + drive single-step + read trigger
  context without re-entering FS-UAE's interactive console command
  parser.
- Patches `console_get()` in `od-fs/uaemisc.cpp` to *block on stdin
  EOF* instead of treating it as "resume". This makes `/v1/pause` and
  watchpoint/breakpoint pauses *actually stick* when FS-UAE is launched
  with stdin redirected from `/dev/null` (the common case for a
  background-process driven by RPC).
- Adds a post-reset hook in `newcpu.cpp` that calls `memwatch_setup()`
  automatically after every `uae_reset()` — so watchpoints survive
  hard-resets without manual re-arm dances.

Total added: one new C++ file (~1100 lines), one `extern "C"` decl,
one function call, six `static`→non-`static` changes, ~20 lines in
`console_get()`, and 5 lines in the reset path. No new external
dependencies (the WebSocket handshake includes a tiny inline SHA-1 +
base64 — no OpenSSL).

## Build dependencies

The script installs these automatically on macOS via Homebrew. On Linux,
install them manually first (Debian/Ubuntu names shown):

| macOS (brew) | Debian/Ubuntu (apt) |
|---|---|
| autoconf, automake, libtool, pkg-config | build-essential, autoconf, automake, libtool, pkg-config |
| gettext | gettext |
| glib | libglib2.0-dev |
| libpng | libpng-dev |
| libmpeg2 | libmpeg2-4-dev |
| openal-soft | libopenal-dev |
| sdl2 | libsdl2-dev |
| zlib | zlib1g-dev |

(macOS provides OpenGL as a system framework; no brew package needed.)

## Configuration

| Env var | Default | Meaning |
|---|---|---|
| `FSUAE_RPC_PORT` | *(unset)* | Port to listen on. Unset → RPC disabled. |
| `FSUAE_RPC_PAUSE_AT_BOOT` | *(unset)* | `1` → emulator starts paused (lets you install breakpoints/watchpoints before any instruction runs). |
| `FSUAE_SRC` | `/tmp/fsuae-src` | Where `build.sh` clones FS-UAE source. |
| `FSUAE_TAG` | `v3.2.35` | Which FS-UAE tag to build. |
| `FSUAE_URL` | `https://github.com/FrodeSolheim/fs-uae.git` | Source repo to clone. |
| `JOBS` | `nproc` | Parallelism for `make`. |

## Versions supported

Verified against:

- **FS-UAE `v3.2.35`** (stable, the default). Built and tested on macOS arm64.

The patch should apply with little change to other FS-UAE 3.x point
releases. The main branch (FS-UAE 5 dev) has a meaningfully different
build system; the patch will need to be adapted.

## Concurrency / safety

- Memory and register reads while the emulator is running may race with
  the emulation thread (you may see torn 16/32-bit values). **Pause first**
  for stable snapshots.
- `/v1/pause` is non-blocking; the request returns once the debugger flag
  is set, but in-flight emulation cycles may complete first. Allow a few
  ms after pausing before reading.
- The server is single-threaded — one request at a time. Fine for
  scripting; not designed for high concurrency.
- 127.0.0.1 only. There is no auth. Don't expose the port externally; if
  you must, put it behind an SSH tunnel.

## Roadmap

Landed in this release: pause/resume, single-step, reset,
breakpoints, watchpoints (with `mustchange` filter), memory R/W,
register R/W, disassembly, chipset register snapshot, state save/load,
sticky-pause via patched `console_get`, pause-at-boot.

See [docs/ROADMAP.md](docs/ROADMAP.md) for what's next:

- Symbol lookup / source line mapping (Kickstart `.fd` files, `.sym` files)
- Memory map endpoint (region descriptors: chip / slow / fast / ROM / IO)
- Step-over (skip JSR/BSR) and step-out (run until RTS)
- WebSocket event stream (notify on breakpoint hit instead of polling)
- Debugger command pass-through (`POST /v1/debug?cmd=...`)
- Frontend debugger (cross-platform UI built on this API)
- MCP server wrapper (so LLM agents can drive FS-UAE natively)
- Windows port (currently compiles to a no-op stub on Win32)

## Background

This patch was written for the
[fast68k](https://github.com/) project — a custom Verilog 68000 / Amiga
chipset emulator that needed to compare its state cycle-by-cycle against
a known-good Amiga reference (FS-UAE) during a long boot-bisection
campaign. Manually pausing the FS-UAE GUI every N million cycles wasn't
practical, so we added an automation surface.

It works well enough for that use case and seems generally useful, hence
this standalone release.

## Contributing / upstreaming

For now this is a separate project — easier to iterate without a slow
upstream review cycle. Eventually we may submit a polished version to
[FrodeSolheim/fs-uae](https://github.com/FrodeSolheim/fs-uae) as a PR.
If you have improvements (especially: Windows support, more endpoints,
better safety), PRs welcome.

## License

GPL-2.0 — same as FS-UAE itself, since the patched binary links this
code into a GPL-2.0 work.
