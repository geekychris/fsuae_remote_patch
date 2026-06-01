#!/usr/bin/env python3
"""uss_diff.py — diff two FS-UAE .uss savestate files.

Walks the IFF-style chunk structure (chunk-id + big-endian length + flags
+ payload, 4-byte aligned) and reports which chunks differ.  For memory
chunks (CRAM/FRAM/BRAM/etc.) the diff also prints a byte-level summary —
total changed bytes, longest changed run, and the first few changed
ranges.

Usage:
    python3 uss_diff.py before.uss after.uss [--bytes]

Without --bytes, only chunk-level changes are printed; --bytes adds a
range-level breakdown for each differing memory chunk.

.uss chunk format (from fs-uae/src/savestate.cpp):
    char     name[4]      e.g. "CPU "
    uint32   total_len    big-endian, including the 12-byte header
    uint32   flags        bit 0 = zlib-compressed payload
    [uint32  orig_len]    only when compressed: pre-zlib payload size
    bytes    payload      length = total_len - 12 (or after decompression)
    bytes    pad          0..3 bytes to align to 4
"""
import argparse
import os
import struct
import sys
import zlib

MEM_CHUNKS = {
    "CRAM", "BRAM", "FRAM", "ZRAM", "ZCRM", "PRAM", "A3K1", "A3K2",
    "CHIP", "BORO", "FAST", "Z3 1", "Z3 2", "Z3CH",
}


def read_chunks(path):
    """Yield (name, flags, payload_bytes) for each chunk in the file."""
    with open(path, "rb") as fh:
        data = fh.read()
    pos = 0
    n = len(data)
    while pos + 12 <= n:
        name = data[pos:pos + 4].decode("latin-1")
        total_len = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        flags = struct.unpack(">I", data[pos + 8:pos + 12])[0]
        # payload may be compressed
        payload_start = pos + 12
        payload_end = pos + total_len
        if payload_end > n or total_len < 12:
            # malformed tail — bail out
            return
        body = data[payload_start:payload_end]
        if flags & 1:
            # first u32 of body is the uncompressed length
            if len(body) < 4:
                yield name, flags, b""
            else:
                try:
                    body = zlib.decompress(body[4:])
                except zlib.error:
                    body = b""
        yield name, flags, body
        # FS-UAE's save_chunk() ALWAYS appends 4-(payload_len & 3) padding
        # bytes after each chunk — even when the payload is already aligned
        # (i.e. len2=4, not 0).  See savestate.cpp:375.  So the next chunk
        # starts at total_len + padding, where padding is in [1..4].
        payload_len = total_len - 12
        if payload_len < 0:
            return
        padding = 4 - (payload_len & 3)  # always 1..4 (never 0)
        pos += total_len + padding


def summarise_byte_diff(a, b, limit=8):
    """Compare two byte buffers and return a one-line summary + up to
    `limit` differing-range descriptors.

    A "range" is a maximal contiguous run of differing bytes.  The
    summary line names total changed bytes, total runs, and the largest
    single run.
    """
    if a == b:
        return "identical", []
    na, nb = len(a), len(b)
    if na != nb:
        # If sizes differ, just report counts — byte-by-byte diff would
        # mismatch on every byte from the resize point onwards.
        return f"size changed {na} → {nb}", []
    ranges = []
    i = 0
    total_changed = 0
    while i < na:
        if a[i] != b[i]:
            j = i
            while j < na and a[j] != b[j]:
                j += 1
            ranges.append((i, j - i))
            total_changed += (j - i)
            i = j
        else:
            i += 1
    largest = max((r[1] for r in ranges), default=0)
    summary = (f"{total_changed} bytes across {len(ranges)} runs "
               f"(largest run = {largest} bytes)")
    return summary, ranges[:limit]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before", help="reference .uss file")
    ap.add_argument("after",  help=".uss file to compare against `before`")
    ap.add_argument("--bytes", action="store_true",
                    help="print first few byte ranges for differing memory chunks")
    args = ap.parse_args()

    if not os.path.exists(args.before): sys.exit(f"no such file: {args.before}")
    if not os.path.exists(args.after):  sys.exit(f"no such file: {args.after}")

    a_chunks = list(read_chunks(args.before))
    b_chunks = list(read_chunks(args.after))

    a_by_name = {}
    for name, flags, body in a_chunks:
        a_by_name.setdefault(name, []).append((flags, body))
    b_by_name = {}
    for name, flags, body in b_chunks:
        b_by_name.setdefault(name, []).append((flags, body))

    all_names = sorted(set(a_by_name) | set(b_by_name))
    print(f"before: {args.before}  ({len(a_chunks)} chunks)")
    print(f"after:  {args.after}   ({len(b_chunks)} chunks)")
    print()
    print(f"{'chunk':6}  {'status':12}  detail")
    print("-" * 70)

    same = added = removed = differ = 0
    for name in all_names:
        a_list = a_by_name.get(name, [])
        b_list = b_by_name.get(name, [])
        if not a_list:
            added += 1
            sizes = ", ".join(str(len(b)) for _, b in b_list)
            print(f"{name:6}  added         {len(b_list)} chunk(s) ({sizes} bytes)")
            continue
        if not b_list:
            removed += 1
            sizes = ", ".join(str(len(b)) for _, b in a_list)
            print(f"{name:6}  removed       {len(a_list)} chunk(s) ({sizes} bytes)")
            continue
        # Pair up by index — most chunks appear once.  For repeats
        # (e.g. multiple FRAM banks) compare in order.
        any_diff = False
        for i in range(max(len(a_list), len(b_list))):
            if i >= len(a_list) or i >= len(b_list):
                any_diff = True
                continue
            (_, a_body) = a_list[i]
            (_, b_body) = b_list[i]
            if a_body == b_body:
                continue
            any_diff = True
            summary, ranges = summarise_byte_diff(a_body, b_body)
            tag = f"#{i}" if len(a_list) > 1 or len(b_list) > 1 else ""
            print(f"{name:6}  differ {tag:5}  {summary}")
            if args.bytes and ranges:
                for off, n in ranges:
                    print(f"            run: +0x{off:08x} len={n}")
        if any_diff:
            differ += 1
        else:
            same += 1

    print()
    print(f"summary: {same} same, {differ} differ, {added} added, {removed} removed")


if __name__ == "__main__":
    main()
