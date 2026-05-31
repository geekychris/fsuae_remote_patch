# fsuae_remote_patch

A small patch + drop-in source file that adds an HTTP/JSON-RPC remote-control
surface to [FS-UAE](https://fs-uae.net/).

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
./build.sh
# ... ~12 s on Apple Silicon ...

# Launch FS-UAE with the RPC enabled
FSUAE_RPC_PORT=8765 /tmp/fsuae-src/fs-uae ~/Documents/FS-UAE/Configurations/MyAmiga.fs-uae &

# In another shell:
curl -s http://127.0.0.1:8765/v1/ping
# {"ok":true,"service":"fs-uae-rpc v1"}

curl -sX POST http://127.0.0.1:8765/v1/pause
curl -s http://127.0.0.1:8765/v1/cpu
# {"ok":true,"pc":"0x00fc0fdc","sr":"0x2000","d0":"...",...,"a7":"0x00c80000",...}

curl -s "http://127.0.0.1:8765/v1/mem?addr=0xC0&len=64"
# {"ok":true,"addr":"0x000000c0","len":64,"hex":"0000000000000000..."}

curl -sX POST http://127.0.0.1:8765/v1/resume
```

See [`docs/PROTOCOL.md`](docs/PROTOCOL.md) for the full endpoint reference
and [`examples/`](examples/) for runnable scripts.

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
| `GET /v1/ping` | — |
| `GET /v1/state` | `debugger_active` (poll for BP/WP hits after resume) |
| `GET /v1/cpu` | `m68k_getpc()`, `regs.regs[0..15]`, `regs.sr`, `regs.usp`, `regs.isp` |
| `GET /v1/mem?addr&len` | `get_byte_debug()` — the same byte-read the in-process debugger uses |
| `POST /v1/pause` | `activate_debugger()` |
| `POST /v1/resume` | `deactivate_debugger()` |
| `POST /v1/state/save?path` | `save_state()` |
| `POST /v1/reset?hard=0\|1` | `uae_reset()` |
| `POST /v1/breakpoints?addr` | writes `bpnodes[]` directly |
| `GET /v1/breakpoints` | reads `bpnodes[]` |
| `POST /v1/breakpoints/clear` | clears all `bpnodes[]` |
| `POST /v1/watchpoints?addr&size&rwi` | writes `mwnodes[]` + `memwatch_setup()` |
| `GET /v1/watchpoints` | reads `mwnodes[]` |
| `POST /v1/watchpoints/clear` | clears all `mwnodes[]` |

The patch also un-`static`s `memwatch_setup()` and `initialize_memwatch()`
in `debug.cpp` so the RPC can install watchpoints without going through
the in-process console command parser.

Total added: one C++ file (~450 lines), one `extern "C"` declaration,
one function call, and two `static`→non-`static` changes. No new
external dependencies.

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

Already landed in this release: breakpoints, watchpoints, reset,
state polling, pause-at-boot. See [docs/ROADMAP.md](docs/ROADMAP.md)
for what's next:

- Single-step (`/v1/step`)
- Memory write (`POST /v1/mem`)
- Register write (`POST /v1/cpu/d0` etc.)
- State load (`POST /v1/state/load`)
- Debugger command pass-through (`POST /v1/debug?cmd=`)
- WebSocket event stream (notify on breakpoint hit)
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
