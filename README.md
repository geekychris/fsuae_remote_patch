# fsuae_remote_patch

**An automation surface for [FS-UAE](https://fs-uae.net/) — drive the Amiga
emulator from scripts, browsers, `gdb -tui`, or LLM agents instead of
clicking around its GUI.**

## The problem

FS-UAE has a full 68000 debugger built in — breakpoints, watchpoints,
single-step, register inspection, all of it. But it's only reachable
through the emulator window: you pause with a hotkey, press `` ` `` to
drop into a text prompt, and type commands one at a time.

That works fine for someone debugging by hand. It's a non-starter for
anything automated: regression scripts, CI harnesses, bisection loops,
MCP-driven agents, or any tool that wants to inspect emulator state
without a human at the keyboard.

## What this gives you

A drop-in C++ source file plus a tiny patch that adds, inside the
existing FS-UAE binary:

- **HTTP/JSON-RPC API** — pause, resume, step, read/write memory and
  registers, install breakpoints and watchpoints, save state, all over
  `curl` on `localhost`.
- **WebSocket event stream** — push notifications when the emulator
  pauses, a breakpoint fires, or a watchpoint triggers. No polling.
- **Embedded web UI** — single-page HTML/JS, served from the binary.
  Live registers, annotated disassembly, hex memory view,
  point-and-click breakpoints.
- **In-process GDB Remote Serial Protocol stub** —
  `gdb -tui -x tools/fsuae.gdbinit` attaches like the emulator were
  hardware. M68k target description served automatically.
- **Amiga-aware annotation** — disassembly auto-labels `JSR -nn(A6)`
  with the matching `exec.OpenLibrary()` etc. via embedded `.fd`
  tables, with runtime-load for other libraries.
- **Optional MCP server** — `tools/mcp_fsuae.py` exposes 26 of the
  endpoints as MCP tools so Claude (or any MCP client) can drive the
  emulator directly.

The patch is **off by default**. Without `FSUAE_RPC_PORT` /
`FSUAE_GDB_PORT` set, the patched binary is byte-for-byte
behaviourally identical to upstream FS-UAE.

## Quick start

```sh
git clone git@github.com:geekychris/fsuae_remote_patch.git
cd fsuae_remote_patch
./build.sh                                    # one-time, ~12 s on Apple Silicon
./run.sh ~/Documents/FS-UAE/Configurations/MyAmiga.fs-uae
```

`./run.sh` launches the patched fs-uae with the HTTP API on `:8765`
and opens the web UI in your browser.

To enable the GDB stub too:

```sh
FSUAE_GDB_PORT=2331 ./run.sh ~/.../MyAmiga.fs-uae
gdb -tui -x tools/fsuae.gdbinit
```

## A taste

**Pause, peek, step, resume via curl:**

```sh
$ curl -sX POST localhost:8765/v1/pause
{"ok":true,"state":"paused"}

$ curl -s localhost:8765/v1/cpu
{"ok":true,"pc":"0x00fc00d2","sr":"0x2700",
 "d0":"0x00000000",...,"a7":"0x11114ef9"}

$ curl -s 'localhost:8765/v1/disasm?addr=pc&count=3&annotate=1'
{"ok":true,"lines":[
  "00FC00D2 4FF80004    LEA       $00040000,A7",
  "00FC00D8 203C00020000 MOVE.L   #$00020000,D0",
  "00FC00DE 5380        SUBQ.L    #1,D0"
]}

$ curl -sX POST 'localhost:8765/v1/watchpoints?addr=0xC0&size=4&rwi=W&mustchange=1'
$ curl -sX POST localhost:8765/v1/resume
# … wait for the WP to fire …
$ curl -s localhost:8765/v1/watchpoints/last
{"ok":true,"hit":true,"addr":"0x000000c0","pc":"0xfc02a8",
 "rwi":"W","size":4,"value":"0x00c096dc"}
```

**Or via gdb:**

```
(gdb) target remote :2331
(gdb) break *0xfc00e6
Breakpoint 1 at 0xfc00e6
(gdb) continue
Continuing.
Breakpoint 1, 0x00fc00e6 in ?? ()
PC=00fc00e6  A6=00000000  A7=00040000
(gdb) x/4i $pc
=> 0xfc00e6:   lea 0xf00000,%a1
   0xfc00ec:   cmpal %a1,%a0
   0xfc00ee:   beq.s 0xfc00fc
   0xfc00f0:   moveq #0,%d7
```

## Documentation

Pick by intent:

| If you want to… | Read |
|---|---|
| **Use the debugger to find bugs** in Amiga code | [`docs/DEBUGGING.md`](docs/DEBUGGING.md) — task-oriented guide covering Web UI / HTTP / GDB / MCP |
| **Look up a specific endpoint or RSP packet** | [`docs/PROTOCOL.md`](docs/PROTOCOL.md) — full reference |
| **Quick HTTP recipes** for scripting | [`docs/USAGE.md`](docs/USAGE.md) — cookbook |
| **Understand how the patch works** inside FS-UAE | [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — threads, sticky pause, the memwatch rewrap dance |
| **See what's planned** | [`docs/ROADMAP.md`](docs/ROADMAP.md) |
| **Drive the emulator from runnable scripts** | [`examples/`](examples/) |

## What's in this repo

```
fsuae_remote_patch/
├── build.sh                            clone FS-UAE + apply patch + build
├── run.sh                              launcher: start fs-uae + open web UI
├── fsuae_rpc.cpp                       the drop-in C++ source (~3000 lines, no external deps)
├── web/index.html                      source for the embedded web UI
├── web_index.inc                       generated header (embedded into binary at build time)
├── patches/0001-fsuae-rpc-hook.patch   ~10 lines into Makefile.am + main.cpp + a few un-statics
├── docs/                               see the table above
├── tools/
│   ├── fsuae.gdbinit                   one-line attach for gdb -tui
│   ├── mcp_fsuae.py                    stdio MCP server (26 tools)
│   ├── uss_diff.py                     chunk-aware .uss savestate differ
│   └── embed_html.py                   build step: web/index.html → web_index.inc
└── examples/                           runnable shell + Python scripts
```

## Configuration

| Env var | Meaning |
|---|---|
| `FSUAE_RPC_PORT` | Port for the HTTP/WS/web UI (e.g. `8765`). Unset → disabled. |
| `FSUAE_GDB_PORT` | Port for the in-process GDB stub (e.g. `2331`). Unset → disabled. Independent of `FSUAE_RPC_PORT`. |
| `FSUAE_RPC_PAUSE_AT_BOOT` | `1` → start paused, so you can install BPs / WPs before the CPU executes anything. |
| `FSUAE_SRC` | Where `build.sh` clones FS-UAE source (default `/tmp/fsuae-src`). |
| `FSUAE_TAG` | Which FS-UAE tag to build (default `v3.2.35`). |

## Build dependencies

`build.sh` installs them automatically on macOS via Homebrew. On Linux,
install manually first (Debian/Ubuntu names shown):

```
sudo apt install build-essential autoconf automake libtool pkg-config \
                 gettext libglib2.0-dev libpng-dev libmpeg2-4-dev \
                 libopenal-dev libsdl2-dev zlib1g-dev
```

## Status

| Platform | Status |
|---|---|
| macOS arm64 / x86_64 | ✅ built + runtime-tested against FS-UAE `v3.2.35` |
| Linux | ✅ builds; the build script auto-detects and uses apt-style deps |
| Windows (MSYS2 / MinGW) | ⚠️ compiles via a portability shim in `fsuae_rpc.cpp`, not yet runtime-tested |

The patch should apply with little change to other FS-UAE 3.x releases.
The FS-UAE 5 dev branch has a meaningfully different build system; the
patch will need to be adapted.

## Safety / concurrency

- **127.0.0.1 only.** There is no authentication. Don't expose the
  port externally; if you must, SSH-tunnel it.
- **Reads while running may see torn 16/32-bit values** as the
  emulation thread races with the worker. Pause first for stable
  snapshots; the docs spell out the "pause before read" contract.
- **One request at a time** — the HTTP server is single-threaded.
  Fine for scripting; not designed for high concurrency.
- The GDB stub uses one worker thread per connection (up to 4
  concurrent gdb clients).

## Origin

This patch was written for the
[fast68k](https://github.com/) project — a custom Verilog 68000 + Amiga
chipset emulator that needed to compare its state cycle-by-cycle
against a known-good reference (FS-UAE) during a long boot-bisection
campaign. Manually pausing the FS-UAE GUI every N million cycles wasn't
practical, so we added an automation surface.

It works well enough for that use case and seems generally useful,
hence this standalone release.

## Contributing / upstreaming

For now this is a separate project — easier to iterate without an
upstream review cycle. We may eventually submit a polished version to
[FrodeSolheim/fs-uae](https://github.com/FrodeSolheim/fs-uae) as a PR.
PRs welcome here in the meantime — especially Windows runtime
verification, additional `.fd` libraries, or new endpoints.

## License

GPL-2.0 — same as FS-UAE itself, since the patched binary links this
code into a GPL-2.0 work.
