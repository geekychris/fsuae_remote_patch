# fsuae.gdbinit — attach gdb to the in-process GDB stub built into fs-uae.
#
# Prerequisite: fs-uae must have been launched with FSUAE_GDB_PORT set,
# e.g.
#   FSUAE_GDB_PORT=2331 FSUAE_RPC_PAUSE_AT_BOOT=1 ./run.sh <config>
#
# Then:
#   gdb -x tools/fsuae.gdbinit                  # text mode
#   gdb -tui -x tools/fsuae.gdbinit             # TUI (split asm + regs)
#
# Why `set endian big` is required:
#   The stub advertises an m68k target description via qXfer:features,
#   but gdb 17.x doesn't pin endianness purely from <architecture>m68k
#   — it defaults to the host's byte order.  We force big-endian here.
#   With this, register values display correctly on the first connect.

set architecture m68k
set endian big

# Disable the --Type RET-- pager: keeps TUI sessions from getting stuck
# on first connect when the banner text fills more than a screen.
set pagination off

# No symbol file means breakpoints are always absolute addresses;
# silence the "make pending?" prompt.
set breakpoint pending on

# Print PC / fp (a6) / sp (a7) at every stop.  Replaces the source-line
# context gdb would normally show when symbols are available.
define hook-stop
  printf "PC=%08x  A6=%08x  A7=%08x\n", $pc, $fp, $sp
end

# One-shot connect.  Adjust port if FSUAE_GDB_PORT is set to something
# other than the default 2331.
target remote 127.0.0.1:2331

# Optional convenience: switch into a split TUI layout (regs pane on top,
# cmd pane below).  Trigger with `fsuae-tui` from the gdb prompt; or pass
# `-tui` on the gdb command line to start in TUI mode and `layout regs`.
define fsuae-tui
  layout regs
end

echo \n
echo fs-uae gdb stub connected. useful commands:\n
echo   x/16i $pc       — disassemble around PC\n
echo   x/16xw 0x4      — peek raw memory (here: ExecBase pointer)\n
echo   info registers  — full CPU state\n
echo   stepi           — single-step one instruction\n
echo   continue        — resume until next BP / WP / signal\n
echo   break *0xfc0100 — install absolute-address breakpoint\n
echo   rwatch *(int*)0xc0  — install hardware read-watchpoint\n
echo   fsuae-tui       — switch to TUI register layout\n
echo \n
