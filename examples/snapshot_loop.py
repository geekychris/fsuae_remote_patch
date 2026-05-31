#!/usr/bin/env python3
"""snapshot_loop.py — save FS-UAE state every N seconds.

Each tick: POST /v1/pause, POST /v1/state/save?path=..., POST /v1/resume.

The output is a series of timestamped .uss files you can later replay or
diff. Useful for bisection workflows where you want to walk back through
a long boot looking for the cycle at which some condition first held.

Usage:
    python3 snapshot_loop.py <out_dir> [--interval 5] [--count 12]
"""
import argparse
import json
import os
import sys
import time
import urllib.request

def call(method, path, base):
    req = urllib.request.Request(base + path, method=method)
    with urllib.request.urlopen(req) as r:
        return json.load(r)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out_dir", help="directory to write .uss snapshots into")
    ap.add_argument("--interval", type=float, default=5.0,
                    help="seconds between snapshots (default: 5)")
    ap.add_argument("--count", type=int, default=12,
                    help="number of snapshots to take (default: 12)")
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("FSUAE_RPC_PORT", "8765")),
                    help="RPC port (default: $FSUAE_RPC_PORT or 8765)")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    base = f"http://127.0.0.1:{args.port}"

    print(f"snapshot_loop: every {args.interval}s × {args.count} -> {args.out_dir}")
    for i in range(args.count):
        if i > 0:
            time.sleep(args.interval)
        # pause + small settle delay
        call("POST", "/v1/pause", base)
        time.sleep(0.05)

        # read PC for the filename
        cpu = call("GET", "/v1/cpu", base)
        pc = cpu["pc"].lower()

        # save state
        path = os.path.abspath(
            os.path.join(args.out_dir, f"snap_{i:03d}_pc_{pc}.uss"))
        r = call("POST", f"/v1/state/save?path={path}", base)
        if not r.get("ok"):
            print(f"[{i:03d}] FAIL: {r}", file=sys.stderr)
        else:
            sz = os.path.getsize(path)
            print(f"[{i:03d}] pc={pc} -> {path} ({sz} bytes)")

        # resume
        call("POST", "/v1/resume", base)

    print("done")

if __name__ == "__main__":
    main()
