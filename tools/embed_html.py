#!/usr/bin/env python3
"""Generate web_index.inc from web/index.html.

The generated file is a single C++ raw-string literal declaring
`WEB_UI_HTML`, included by fsuae_rpc.cpp.
"""
import sys
from pathlib import Path

here = Path(__file__).resolve().parent.parent
html = (here / "web" / "index.html").read_text()
out = here / "web_index.inc"

# Defensive: if the HTML happens to contain our delimiter string, fail.
DELIM = "FSUAEWEBUI"
if f")" + DELIM in html:
    print(f"ERROR: HTML contains delimiter ')'+ '{DELIM}'; pick a new one", file=sys.stderr)
    sys.exit(1)

with out.open("w") as f:
    f.write("/* Auto-generated from web/index.html.  Do not edit by hand.\n")
    f.write(" * Regenerate via: tools/embed_html.py                       */\n")
    f.write(f'static const char WEB_UI_HTML[] = R"{DELIM}(\n')
    f.write(html)
    f.write(f'){DELIM}";\n')

print(f"wrote {out} ({out.stat().st_size} bytes)")
