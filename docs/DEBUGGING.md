# Debugging guide

How to actually use the debugger to find bugs in Amiga code — organized
by task, not by API endpoint.

## Pick your interface

There are four ways to drive the debugger.  All four hit the same
underlying primitives, so you can mix them (e.g. set a breakpoint via
curl, watch it fire in the Web UI, inspect the trigger in gdb).

| Interface | Best for | Start with |
|---|---|---|
| **Web UI** | Exploration, ad-hoc poking, watching values live | `./run.sh <config>` → browser opens automatically |
| **HTTP / curl** | Shell scripts, CI, one-shot inspections | `curl http://127.0.0.1:8765/v1/cpu` |
| **GDB** | Anyone already fluent in gdb; TUI register view | `gdb -tui -x tools/fsuae.gdbinit` |
| **MCP** | LLM-agent driven debugging (Claude Code, Claude Desktop) | configure `tools/mcp_fsuae.py` as an MCP server |

The Web UI and HTTP API share port `FSUAE_RPC_PORT` (default 8765); the
GDB stub uses a separate port `FSUAE_GDB_PORT` (suggest 2331); MCP
launches the wrapper as a stdio subprocess.

## The five-minute tour

```sh
./build.sh                              # one-time, builds fs-uae
FSUAE_GDB_PORT=2331 FSUAE_RPC_PAUSE_AT_BOOT=1 \
    ./run.sh ~/Documents/FS-UAE/Configurations/MyAmiga.fs-uae
```

You now have:

- A paused emulator (because `FSUAE_RPC_PAUSE_AT_BOOT=1`)
- Web UI auto-opened at `http://127.0.0.1:8765/`
- HTTP/JSON-RPC at the same port
- GDB stub listening on `127.0.0.1:2331`

The Web UI shows you live CPU registers, disassembly at PC (annotated
with `exec.OpenLibrary()` etc. when the routine matches a known FD
entry), a memory hex view, and a chipset register snapshot.  Click
**Resume** to start the boot.  Add a breakpoint by typing an address in
the BP box and hitting `+ BP`.

## Debugging cookbook

### Inspect what's at PC right now

```sh
# HTTP
curl -sX POST http://127.0.0.1:8765/v1/pause
curl -s http://127.0.0.1:8765/v1/cpu
curl -s 'http://127.0.0.1:8765/v1/disasm?addr=pc&count=16&annotate=1'
```

```
(gdb) info registers
(gdb) x/16i $pc
```

Web UI: registers are always live; disassembly auto-refreshes around PC
on every pause.

### Step through code

```sh
# HTTP — one insn, then 100 insns
curl -sX POST 'http://127.0.0.1:8765/v1/step?n=1'
curl -sX POST 'http://127.0.0.1:8765/v1/step?n=100'

# Step OVER a JSR/BSR (one-shot BP at PC+insn_len, then resume)
curl -sX POST 'http://127.0.0.1:8765/v1/step?mode=over'

# Step OUT of the current function (one-shot BP at *(A7) then resume)
curl -sX POST 'http://127.0.0.1:8765/v1/step?mode=out'
```

```
(gdb) stepi
(gdb) si 100
(gdb) finish      # → maps to vCont;c with a one-shot BP at return PC
```

Web UI: the **Step** button is one instruction; **×100** does 100.

### Set a breakpoint

```sh
# Plain BP
curl -sX POST 'http://127.0.0.1:8765/v1/breakpoints?addr=0xFC0234'

# BP that fires only on the 47th hit (silently skips the first 46)
curl -sX POST 'http://127.0.0.1:8765/v1/breakpoints?addr=0xFC0234&skip=46'

# One-shot BP (auto-clears after first fire)
curl -sX POST 'http://127.0.0.1:8765/v1/breakpoints?addr=0xFC0234&oneshot=1'

# List
curl -s http://127.0.0.1:8765/v1/breakpoints
# {"breakpoints":[{"slot":0,"addr":"0x00fc0234","skip_remaining":46,"hit_count":0,...}]}
```

```
(gdb) break *0xfc0234
(gdb) continue
```

Skip-count is useful for routines like `CopyMem` called hundreds of
times during boot — set `skip=N` to land on the call that interests you
without stopping at every other call.

### Find what writes to a memory location

The classic Amiga reverse-engineering task: "who is clobbering my
sprite pointer?".

```sh
# Pause on any WRITE to chip $C0..$C3
curl -sX POST 'http://127.0.0.1:8765/v1/watchpoints?addr=0xC0&size=4&rwi=W&mustchange=1'
curl -sX POST http://127.0.0.1:8765/v1/resume

# When it fires, get the exact triggering PC + value
curl -s http://127.0.0.1:8765/v1/watchpoints/last
# {"hit":true,"addr":"0x000000c0","pc":"0xfc02a8","rwi":"W","size":4,"value":"0x00c096dc"}
```

`mustchange=1` skips writes-of-same-value (common during init); without
it you'd stop on every `clr.l $C0` even though the value didn't change.
Add `val=HEX&valmask=HEX` to fire only on a specific value being
written — useful for catching the moment a recognisable signature lands
at an address.

```
(gdb) awatch *(int*)0xc0          # access (R or W)
(gdb) watch  *(int*)0xc0          # write
(gdb) rwatch *(int*)0xc0          # read
(gdb) continue
```

After the stop, `p/x $pc` shows the triggering instruction.  Note that
GDB watchpoints don't have `mustchange` — use the HTTP API if you need
that filter.

### Catch the Nth call to a routine

```sh
# Stop on the 1000th call to CopyMem (exec.library offset -624)
curl -sX POST 'http://127.0.0.1:8765/v1/breakpoints?addr=0xFC...&skip=999'
```

To find the address of an exec.library function:

```sh
# Look up the routine's negative offset → use ExecBase + neg_offset
curl -s 'http://127.0.0.1:8765/v1/fd/lookup?offset=-624'
# {"library":"exec","offset":-624,"name":"CopyMem"}

# ExecBase pointer is at $4
curl -s 'http://127.0.0.1:8765/v1/mem?addr=0x4&len=4'
# {"hex":"00c00276"}    → ExecBase = 0x00c00276

# So CopyMem entry is at 0x00c00276 - 624 = 0x00c0001e
```

### Decode an AmigaOS library call

The disassembler annotates `JSR -nn(A6)` with the library function
name automatically:

```sh
curl -s 'http://127.0.0.1:8765/v1/disasm?addr=0xFC2000&count=8&annotate=1'
# ...
# "FC2010  4EAEFE38     JSR    -$1c8(A6)         ; exec.OpenLibrary()"
```

For non-exec libraries, load the `.fd` file first:

```sh
curl -sX POST 'http://127.0.0.1:8765/v1/fd/load?path=/path/to/graphics.fd&library=graphics'
# Now the annotator searches graphics.fd too.
curl -s 'http://127.0.0.1:8765/v1/disasm?addr=0xFC2000&count=8&annotate=1&library=graphics'
# When multiple libraries share an offset, the `library=` hint wins.
```

You can load arbitrarily many libraries (`intuition`, `dos`,
`mathieeesingbas`, etc.); they all live in a single registry.

### Walk the stack

```sh
curl -s 'http://127.0.0.1:8765/v1/stack?depth=32'
# {"sp":"0x40000","words":[
#   {"offset":0,"addr":"0x00040000","value":"0x00fc16e2","kind":"code"},
#   {"offset":4,"addr":"0x00040004","value":"0x00000001","kind":"data"},
#   ...
# ]}
```

There's no enforced frame format on m68k, so the response heuristically
tags each long as `code` (even, in a RAM/ROM bank, ≥ 0x400) or `data`.
Walk the `kind:"code"` entries as likely return addresses.

### Render a memory map

```sh
curl -s http://127.0.0.1:8765/v1/memmap
# {"regions":[
#   {"start":"0x00000000","end":"0x001fffff","kind":"chipram",...},
#   {"start":"0x00200000","end":"0x009fffff","kind":"unmapped",...},
#   {"start":"0x00a00000","end":"0x00bfffff","kind":"cia",...},
#   {"start":"0x00c00000","end":"0x00c7ffff","kind":"ram","label":"bog"...},
#   {"start":"0x00dc0000","end":"0x00dcffff","kind":"io",...},
#   {"start":"0x00fc0000","end":"0x00ffffff","kind":"rom",...}
# ]}
```

Useful before issuing a memory write: writing to an `unmapped` or `rom`
region will silently no-op.

### Save / restore state for bisection

```sh
# Snapshot now
curl -sX POST 'http://127.0.0.1:8765/v1/state/save?path=/tmp/before.uss'

# ...let it run, hit a bug...
curl -sX POST 'http://127.0.0.1:8765/v1/state/save?path=/tmp/after.uss'

# Diff the two snapshots — see which chunks changed
python3 tools/uss_diff.py /tmp/before.uss /tmp/after.uss --bytes
```

Watchpoints **survive** state load and reset — the patch hooks the
reset path to re-rearm memwatch.  So a typical bisection workflow is:

```
load earlier snapshot → resume → wait for WP fire → inspect → repeat
```

### Modify CPU state mid-execution

```sh
# Force D0 = 0
curl -sX POST 'http://127.0.0.1:8765/v1/cpu?reg=d0&value=0'

# Jump somewhere
curl -sX POST 'http://127.0.0.1:8765/v1/cpu?reg=pc&value=0xFC0000'

# Write some bytes
curl -sX POST 'http://127.0.0.1:8765/v1/mem?addr=0xC0&hex=DEADBEEF'
```

```
(gdb) set $d0 = 0
(gdb) set $pc = 0xfc0000
(gdb) set {int}0xC0 = 0xdeadbeef
```

## The GDB workflow

GDB gives you the standard reverse-engineering experience: TUI register
pane, scriptable `display`, conditional breakpoints (gdb-side, see
caveats below), commands stored in `.gdbinit`.

### Launch

```sh
# Terminal 1
FSUAE_GDB_PORT=2331 FSUAE_RPC_PAUSE_AT_BOOT=1 ./run.sh <config>

# Terminal 2
gdb -tui -x tools/fsuae.gdbinit
```

`tools/fsuae.gdbinit` sets architecture, endianness, connects to the
stub, and installs a `hook-stop` that prints `PC` / `A6` / `A7` after
every step.

### TUI layout

```
(gdb) layout regs       # split: register pane on top, cmd below
(gdb) layout asm        # split: disassembly pane (note: needs symbols
                        #        for the source pane to populate;
                        #        without symbols you'll see
                        #        "[ No Source Available ]")
(gdb) focus cmd         # focus the command pane (Ctrl-x o cycles)
```

For naked ROM debugging the register pane is the most useful one.  Use
`x/16i $pc` in the cmd pane for disassembly.

### Useful gdb commands

```
break *0xfc0234         # absolute-address BP (no symbols needed)
rbreak ...              # NOT supported — no symbol table
awatch *(int*)0xc0      # access watchpoint (read or write)
watch  *(int*)0xc0      # write watchpoint
rwatch *(int*)0xc0      # read watchpoint
display/i $pc           # auto-show next insn at every stop
display/4i $pc          # ...or four insns
si                      # stepi — one instruction
si 100                  # 100 instructions
c                       # continue
finish                  # step out (one-shot BP at return addr)
x/16xw 0x4              # 16 longs from address 4 (ExecBase pointer)
info registers          # full register dump
```

### Caveats specific to the GDB stub

- **No symbol file.** The Kickstart ROM has no DWARF or ELF symbols.
  Every breakpoint is `break *0xADDR`.  GDB will print `?? ()` for
  function names everywhere.
- **No conditional breakpoints (gdb-side).** GDB *does* support
  conditions, but it evaluates them by stopping the target every hit
  and re-checking — which means a `break *0xfc0234 if $d0 == 5` will
  pause the emulator on every hit and is slow.  Use the HTTP API's
  `skip=N` for high-frequency breakpoints instead.
- **No source-line layout.** `layout src` shows "No Source Available".
  Use `layout regs` + `x/16i $pc`.
- **Endianness** must be forced (`set endian big`) — gdb 17.x doesn't
  infer it from the target description.  `fsuae.gdbinit` does this for
  you.

## The MCP workflow (LLM agents)

`tools/mcp_fsuae.py` is a stdio MCP server that exposes 26 fsuae_rpc
endpoints as tools.  Configure it in your MCP-aware client (Claude
Desktop, Claude Code) like this:

```json
{
  "mcpServers": {
    "fsuae": {
      "command": "python3",
      "args": ["/abs/path/to/tools/mcp_fsuae.py"],
      "env": { "FSUAE_RPC_PORT": "8765" }
    }
  }
}
```

The agent can then call tools like `fsuae_pause`, `fsuae_disasm`,
`fsuae_breakpoint_add`, `fsuae_memmap`, etc.  Each tool's output is the
raw fsuae_rpc JSON, surfaced as text content.

This is the right interface when you want an LLM to:

- bisect a long boot to find when a value changes
- decode a chipset register state into a human explanation
- propose what a fragment of disassembly is probably doing
- drive a multi-step exploration ("set a BP at OpenLibrary, continue,
  inspect a1, follow the string pointer")

## Tips & gotchas

### The OVL bit (early-boot memory writes)

The Amiga's CIA-A `OVL` bit, asserted at power-on, mirrors the ROM into
low memory (`0x00000000–0x000FFFFF`).  Writes to addresses in this
range silently no-op until the boot code clears OVL (a write to
`0xBFE001`).  If your `set {int}0x80000 = ...` doesn't stick during
early boot, that's why.  Run past the OVL-clear (typically the first
few hundred instructions) before writing to low memory.

### Watchpoint installation cost

Installing the first watchpoint triggers `memwatch_setup()` which
rewraps every memory bank with a debug shim.  This adds a small
per-access overhead — maybe 10-20% slower while watchpoints are active.
Subsequent installs are fast.  Clear watchpoints when you're done
exploring.

### `mustchange=1` is usually what you want

A typical boot clears chip RAM with `clr.l (a0)+`.  Without
`mustchange=1`, your watchpoint will fire on every one of those clears.
With it, you only fire on the FIRST write that actually changes the
target value — perfect for "find the code that finally sets this".

### Pause before reading for stable snapshots

Reads while the emulator is running can see torn 16/32-bit values.  Not
crashes — just inconsistent.  `POST /v1/pause` first (or just connect
gdb — gdb sends `?` which forces pause).

### Watchpoints survive resets; BPs survive too

The patch's reset hook re-runs `memwatch_setup()` after every
`uae_reset()` so watchpoints don't get clobbered.  Breakpoints are just
entries in `bpnodes[]` — also unaffected by reset.  So a typical
"reset and try again" cycle Just Works.

### Where to put a "find this" breakpoint

If you're looking for *where* something happens but don't know the
address, install a watchpoint instead of a breakpoint — it tells you
the triggering PC.  Then promote it to a breakpoint at that PC for
targeted stops.

## Worked example: catch the moment Workbench loads

```sh
# Start paused
FSUAE_RPC_PAUSE_AT_BOOT=1 FSUAE_RPC_PORT=8765 ./run.sh kick13.fs-uae

# Watchpoint on workbench.library's open: it's set up via OpenLibrary,
# which writes to a struct field.  Easier: watch the screen-base
# pointer in $DFF0E0 (BPL1PT) for the first non-zero value.
curl -sX POST 'http://127.0.0.1:8765/v1/watchpoints?addr=0xDFF0E0&size=4&rwi=W&mustchange=1'
curl -sX POST http://127.0.0.1:8765/v1/resume

# Watch the WebSocket for the fire event
python3 -c '
import websocket, json
ws = websocket.create_connection("ws://127.0.0.1:8765/v1/events")
for msg in iter(ws.recv, ""):
    m = json.loads(msg)
    print(m)
    if m.get("event") == "wp_hit": break
'

# Inspect when it fires
curl -s http://127.0.0.1:8765/v1/watchpoints/last
curl -s 'http://127.0.0.1:8765/v1/disasm?addr=pc&count=8&annotate=1'
```

The same flow in gdb:

```
(gdb) watch *(int*)0xdff0e0
(gdb) continue
... stops at the write ...
(gdb) x/8i $pc
(gdb) bt          # stack trace — heuristic, see /v1/stack caveats
```

## Where to go next

- [`PROTOCOL.md`](PROTOCOL.md) — full endpoint and packet reference
- [`ARCHITECTURE.md`](ARCHITECTURE.md) — how the patch works inside fs-uae
- [`USAGE.md`](USAGE.md) — quick-reference recipes (mostly subset of this doc)
- [`ROADMAP.md`](ROADMAP.md) — what's done, what's planned
- [`../examples/`](../examples/) — runnable shell + Python scripts
