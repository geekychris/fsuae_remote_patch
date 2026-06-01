#!/usr/bin/env python3
"""mcp_fsuae.py — Model Context Protocol stdio server that exposes
fs-uae-rpc as a tool surface for LLM agents.

This is a minimal MCP server implementation (JSON-RPC 2.0 over stdio)
following the MCP 2024-11-05 schema.  No external dependencies — uses
only the Python standard library plus urllib for HTTP.

Run it like:
    FSUAE_RPC_PORT=8765 python3 mcp_fsuae.py

Then point an MCP-aware client (Claude Desktop, Claude Code, etc.) at
the script.  The server exposes one tool per fsuae_rpc endpoint:
    fsuae_ping, fsuae_state, fsuae_cpu, fsuae_mem_read, fsuae_mem_write,
    fsuae_pause, fsuae_resume, fsuae_step, fsuae_reset, fsuae_disasm,
    fsuae_custom, fsuae_memmap, fsuae_stack, fsuae_breakpoint_add,
    fsuae_breakpoint_list, fsuae_breakpoint_clear, fsuae_watchpoint_add,
    fsuae_watchpoint_list, fsuae_watchpoint_clear, fsuae_watchpoint_last,
    fsuae_state_save, fsuae_state_load, fsuae_symbol_lookup,
    fsuae_fd_lookup, fsuae_fd_load, fsuae_fd_libraries.

Each tool's input schema mirrors the matching HTTP query params; outputs
are the raw fs-uae-rpc JSON returned as text content.
"""
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request


PORT = int(os.environ.get("FSUAE_RPC_PORT", "8765"))
BASE = f"http://127.0.0.1:{PORT}"


def http(method, path, params=None):
    """Make one fsuae_rpc HTTP request and return the parsed JSON body."""
    qs = ("?" + urllib.parse.urlencode(params)) if params else ""
    req = urllib.request.Request(BASE + path + qs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except Exception:
            return {"ok": False, "err": f"HTTP {e.code}: {e.reason}"}
    except urllib.error.URLError as e:
        return {"ok": False, "err": f"connection failed: {e.reason}"}


# Tool table: each entry is (name, http_method, path, [param_name, ...], description).
# Param schemas are all string/number with sensible types; MCP clients render the
# defaults from the description.  Keeping the schema shallow keeps the server simple.
TOOLS = [
    ("fsuae_ping",            "GET",  "/v1/ping",              [], "Smoke test — confirms the fs-uae-rpc backend is reachable."),
    ("fsuae_state",           "GET",  "/v1/state",             [], "Returns whether the emulator is currently paused or running."),
    ("fsuae_cpu",             "GET",  "/v1/cpu",               [], "Full 68k register snapshot (D0-D7, A0-A7, PC, SR, USP, ISP)."),
    ("fsuae_mem_read",        "GET",  "/v1/mem",               ["addr", "len"], "Read up to 65536 bytes from emulated memory. Returns hex string."),
    ("fsuae_mem_write",       "POST", "/v1/mem",               ["addr", "hex"], "Write bytes to memory. `hex` is a contiguous hex string ('CAFEBABE')."),
    ("fsuae_pause",           "POST", "/v1/pause",             [], "Pause the emulator (sticky)."),
    ("fsuae_resume",          "POST", "/v1/resume",            [], "Resume from pause; auto-rearms any installed watchpoints."),
    ("fsuae_step",            "POST", "/v1/step",              ["n", "mode"], "Step one or N instructions, or `mode=over` / `mode=out` for source-style navigation."),
    ("fsuae_reset",           "POST", "/v1/reset",             ["hard"], "Trigger a power-on (hard=1) or soft (hard=0) reset."),
    ("fsuae_disasm",          "GET",  "/v1/disasm",            ["addr", "count", "annotate", "library"], "Disassemble N (1..256) instructions. annotate=1 adds library function names for JSR/JMP -nn(A6)."),
    ("fsuae_custom",          "GET",  "/v1/custom",            [], "Amiga chipset register snapshot — DMACON, INTENA/INTREQ, BPLCON0, copper/bitplane pointers, ..."),
    ("fsuae_memmap",          "GET",  "/v1/memmap",            [], "Memory region descriptors (chip/slow/fast/ROM/IO/unmapped) by walking mem_banks."),
    ("fsuae_stack",           "GET",  "/v1/stack",             ["depth"], "Read N (1..1024) longwords from (A7) with code/data heuristic tagging."),
    ("fsuae_breakpoint_add",  "POST", "/v1/breakpoints",       ["addr", "skip", "oneshot"], "Install a PC breakpoint. skip=N silently ignores first N hits; oneshot=1 auto-clears after first fire."),
    ("fsuae_breakpoint_list", "GET",  "/v1/breakpoints",       [], "List active breakpoints with hit counts."),
    ("fsuae_breakpoint_clear","POST", "/v1/breakpoints/clear", [], "Remove all breakpoints."),
    ("fsuae_watchpoint_add",  "POST", "/v1/watchpoints",       ["addr", "size", "rwi", "mustchange", "val", "valmask"], "Install a memory watchpoint."),
    ("fsuae_watchpoint_list", "GET",  "/v1/watchpoints",       [], "List active watchpoints."),
    ("fsuae_watchpoint_clear","POST", "/v1/watchpoints/clear", [], "Remove all watchpoints."),
    ("fsuae_watchpoint_last", "GET",  "/v1/watchpoints/last",  [], "Details (addr, PC, value) of the most recent watchpoint hit."),
    ("fsuae_state_save",      "POST", "/v1/state/save",        ["path"], "Save emulator state to absolute path (.uss)."),
    ("fsuae_state_load",      "POST", "/v1/state/load",        ["path"], "Restore emulator state from absolute path."),
    ("fsuae_symbol_lookup",   "GET",  "/v1/symbols/lookup",    ["addr"], "Look up a well-known Amiga address (DFF000 chipset, BFExxx CIA, exception vectors)."),
    ("fsuae_fd_lookup",       "GET",  "/v1/fd/lookup",         ["offset", "library"], "Look up an AmigaOS library function by negative offset."),
    ("fsuae_fd_load",         "POST", "/v1/fd/load",           ["path", "library"], "Load an .fd file and register its function table under the given library name."),
    ("fsuae_fd_libraries",    "GET",  "/v1/fd/libraries",      [], "List loaded FD libraries with function counts."),
]


def tool_schema(name, params, description):
    """Build an MCP tool descriptor (inputSchema is JSON-Schema-ish)."""
    properties = {}
    for p in params:
        # Crude type inference: numeric-looking params are numbers, the rest strings.
        if p in ("n", "len", "count", "size", "depth", "skip"):
            properties[p] = {"type": "integer"}
        elif p in ("hard", "oneshot", "mustchange", "annotate"):
            properties[p] = {"type": "integer", "enum": [0, 1]}
        else:
            properties[p] = {"type": "string"}
    return {
        "name": name,
        "description": description,
        "inputSchema": {
            "type": "object",
            "properties": properties,
            "required": [],
        },
    }


def dispatch_tool(name, args):
    """Look up a tool by name, call the matching HTTP endpoint, return JSON."""
    for tname, method, path, params, _desc in TOOLS:
        if tname == name:
            qp = {k: str(v) for k, v in (args or {}).items() if k in params}
            return http(method, path, qp)
    return {"ok": False, "err": f"unknown tool {name!r}"}


# ---- MCP framing helpers -----------------------------------------------------

def send(obj):
    """Write a JSON-RPC message to stdout, one line, then flush.

    The MCP stdio transport historically used Content-Length framing, but
    in 2024-11-05 it switched to one JSON object per line.  We use the
    line-delimited form for simplicity.
    """
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main():
    """JSON-RPC stdin loop: read one request per line, write one response."""
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError as e:
            send({"jsonrpc": "2.0", "id": None,
                  "error": {"code": -32700, "message": f"parse error: {e}"}})
            continue
        rid = req.get("id")
        method = req.get("method")
        params = req.get("params") or {}

        if method == "initialize":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": "2024-11-05",
                "capabilities": {"tools": {}},
                "serverInfo": {"name": "fsuae-rpc", "version": "1.0.0"},
            }})
        elif method == "tools/list":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "tools": [tool_schema(n, p, d) for n, _, _, p, d in TOOLS],
            }})
        elif method == "tools/call":
            name = params.get("name")
            args = params.get("arguments") or {}
            body = dispatch_tool(name, args)
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": json.dumps(body, indent=2)}],
                "isError": not bool(body.get("ok")),
            }})
        elif method in ("notifications/initialized", "$/cancelRequest"):
            # Notification — no response.
            pass
        elif rid is not None:
            send({"jsonrpc": "2.0", "id": rid,
                  "error": {"code": -32601, "message": f"unknown method: {method}"}})


if __name__ == "__main__":
    main()
