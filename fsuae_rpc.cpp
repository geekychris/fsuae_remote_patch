/*
 * FS-UAE — external HTTP/JSON-RPC control surface.
 *
 * Drop-in source file that adds a minimal remote-control API to FS-UAE,
 * which otherwise only exposes its debugger interactively via the GUI
 * (`console_debugger = 1` + Pause+backtick to enter the prompt).
 *
 * Lets external tools — scripts, CI harnesses, MCP servers, custom
 * frontends — pause/resume the emulator, read CPU registers and memory,
 * and snapshot state, without keyboard interaction.
 *
 * Activation:
 *     FSUAE_RPC_PORT=8765 fs-uae <config>
 *
 * If the env var is not set, this module is a no-op (fsuae_rpc_init
 * returns immediately).  The HTTP server runs in its own worker thread
 * and binds 127.0.0.1 only (no remote network access).
 *
 * Concurrency notes:
 *     - Memory and register reads are SAFE while the emulator is PAUSED
 *       (i.e. after a /v1/pause call).  Reading while the emulator is
 *       running may race with the emulation thread; clients should pause
 *       first for stable snapshots.
 *     - /v1/pause and /v1/resume just toggle the in-process debugger.
 *
 * v1 endpoints — execution control:
 *     GET  /v1/ping                        smoke test — returns {"ok":true}
 *     GET  /v1/state                       running/paused (poll for BP/WP hits)
 *     POST /v1/pause                       stop the emulator
 *     POST /v1/resume                      resume emulation
 *     POST /v1/step?n=N                    execute N instructions then pause
 *     POST /v1/reset?hard=1                trigger reset (hard=1 default, 0=soft)
 *
 * v1 endpoints — inspection:
 *     GET  /v1/cpu                         CPU registers + PC + SR
 *     GET  /v1/mem?addr=HEX&len=N          read N bytes from addr, hex string
 *     GET  /v1/disasm?addr=HEX&count=N     disassemble N insns from addr
 *     GET  /v1/custom                      Amiga chipset register snapshot
 *
 * v1 endpoints — mutation (paused-state recommended):
 *     POST /v1/mem?addr=HEX&hex=BYTES      write bytes to memory
 *     POST /v1/cpu?reg=NAME&value=HEX      write a CPU register
 *                                          (d0..d7, a0..a7, pc, sr, usp, isp)
 *
 * v1 endpoints — state snapshots:
 *     POST /v1/state/save?path=ABS_PATH    save state to absolute path
 *     POST /v1/state/load?path=ABS_PATH    restore state from path
 *
 * v1 endpoints — breakpoints & watchpoints:
 *     POST /v1/breakpoints?addr=HEX        install PC breakpoint, auto-pause on hit
 *     GET  /v1/breakpoints                 list active breakpoints
 *     POST /v1/breakpoints/clear           remove all breakpoints
 *     POST /v1/watchpoints?addr=HEX&size=N&rwi=W&mustchange=0|1
 *                                          install memory watchpoint
 *     GET  /v1/watchpoints                 list active watchpoints
 *     POST /v1/watchpoints/clear           remove all watchpoints
 *     POST /v1/watchpoints/rearm           re-wrap mem banks (use after /v1/reset)
 *
 * Conventions:
 *     - addr / len / value query params accept decimal, 0x-prefixed hex,
 *       or $-prefixed hex
 *     - all responses are JSON: {"ok":true,...} or {"ok":false,"err":"msg"}
 *     - "Connection: close" — one request per TCP connection (simple v1)
 *
 * Not in v1 (deferred — see ROADMAP.md):
 *     - WebSocket event stream (notify on breakpoint hit)
 *     - Symbol lookup / source line mapping
 *     - Memory map (region descriptors)
 *     - Stack walker
 *     - Step-over / step-out
 *     - Windows support
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "debug.h"
#include "savestate.h"
#include "uae.h"

/* External chipset state — declared here because we read them by name
 * rather than via the chipset register memory window at $DFF000.
 * Reading register memory at $DFF000 only returns the *readable*
 * sub-set (DMACONR, INTREQR, INTENAR, VPOSR, VHPOSR, etc.); the live
 * shadow state we care about (DMACON write mask, full INTENA bits, etc.)
 * is held in these globals. */
extern uae_u16 dmacon;
extern uae_u16 intena, intreq;
extern unsigned int bplcon0;

/* The patch un-statics these so we can drive memwatch installation
 * and single-step without re-entering the in-process debugger command parser.
 * Defined in debug.cpp (C++ linkage); declared here to match. */
void memwatch_setup(void);
void initialize_memwatch(int mode);
extern int skipaddr_doskip;
extern int no_trace_exceptions;

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifndef _WIN32
# include <pthread.h>
# include <unistd.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

extern "C" void fsuae_rpc_init(void);

#ifdef _WIN32
/* v1 is POSIX-only.  Windows port deferred (would use _beginthreadex +
 * winsock2 instead of pthread/BSD sockets).  Compile to a no-op stub. */
extern "C" void fsuae_rpc_init(void) {}
#else

/* ----- Tiny JSON / HTTP helpers (no external dependency) ----- */

static void send_str(int fd, const char *s) {
    size_t n = strlen(s);
    while (n > 0) {
        ssize_t w = send(fd, s, n, 0);
        if (w <= 0) return;
        s += w;
        n -= (size_t)w;
    }
}

static void send_response(int fd, int code, const char *body) {
    char hdr[256];
    snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        code,
        code == 200 ? "OK" :
        code == 400 ? "Bad Request" :
        code == 404 ? "Not Found" :
        code == 500 ? "Internal Server Error" : "Other",
        strlen(body));
    send_str(fd, hdr);
    send_str(fd, body);
}

static void err_response(int fd, int code, const char *msg) {
    char body[512];
    snprintf(body, sizeof body, "{\"ok\":false,\"err\":\"%s\"}\n", msg);
    send_response(fd, code, body);
}

static uae_u32 parse_uint(const char *s, int *ok) {
    if (!s || !*s) { if (ok) *ok = 0; return 0; }
    char *end = NULL;
    unsigned long v;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        v = strtoul(s + 2, &end, 16);
    else if (s[0] == '$')
        v = strtoul(s + 1, &end, 16);
    else
        v = strtoul(s, &end, 0); /* auto-detect 0x prefix or base 10 */
    if (ok) *ok = (end && *end != '\0' && *end != '&' && *end != ' ') ? 0 : 1;
    return (uae_u32)v;
}

/* Extract a query-string param value (URL-decoded for our minimal
 * subset — we don't expect %-encoded chars in addr/len/path values).
 * Returns pointer into a static buffer, or NULL if not found.
 * Caller MUST copy before another call. */
static char *get_query_param(const char *qs, const char *name) {
    static char buf[1024];
    if (!qs) return NULL;
    size_t nlen = strlen(name);
    const char *p = qs;
    while (p && *p) {
        if (strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            const char *e = strchr(v, '&');
            size_t vlen = e ? (size_t)(e - v) : strlen(v);
            if (vlen >= sizeof buf) vlen = sizeof buf - 1;
            memcpy(buf, v, vlen);
            buf[vlen] = '\0';
            return buf;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return NULL;
}

/* ----- Endpoint impls ----- */

static void ep_ping(int fd) {
    send_response(fd, 200, "{\"ok\":true,\"service\":\"fs-uae-rpc v1\"}\n");
}

static void ep_cpu(int fd) {
    /* regs[0..7] = D0-D7, regs[8..15] = A0-A7.  m68k_getpc() gives PC.
     * regs.sr is the user-visible SR (after MakeSR() refresh).        */
    char body[2048];
    int n = 0;
    n += snprintf(body + n, sizeof body - n,
        "{\"ok\":true,\"pc\":\"0x%08x\",\"sr\":\"0x%04x\"",
        (unsigned)m68k_getpc(), (unsigned)(regs.sr & 0xFFFF));
    for (int i = 0; i < 8; i++)
        n += snprintf(body + n, sizeof body - n,
            ",\"d%d\":\"0x%08x\"", i, (unsigned)regs.regs[i]);
    for (int i = 0; i < 8; i++)
        n += snprintf(body + n, sizeof body - n,
            ",\"a%d\":\"0x%08x\"", i, (unsigned)regs.regs[8 + i]);
    n += snprintf(body + n, sizeof body - n,
        ",\"usp\":\"0x%08x\",\"isp\":\"0x%08x\"}\n",
        (unsigned)regs.usp, (unsigned)regs.isp);
    send_response(fd, 200, body);
}

static void ep_mem(int fd, const char *qs) {
    char *as = get_query_param(qs, "addr");
    if (!as) { err_response(fd, 400, "missing addr"); return; }
    char addr_buf[64];
    snprintf(addr_buf, sizeof addr_buf, "%s", as);
    char *ls = get_query_param(qs, "len");
    int ok_a, ok_l;
    uae_u32 addr = parse_uint(addr_buf, &ok_a);
    uae_u32 len  = ls ? parse_uint(ls, &ok_l) : 64;
    if (!ok_a) { err_response(fd, 400, "bad addr"); return; }
    if (ls && !ok_l) { err_response(fd, 400, "bad len"); return; }
    if (len == 0 || len > 0x10000) {
        err_response(fd, 400, "len out of range (1..65536)");
        return;
    }
    /* Body header. */
    static char body[256 + 0x10000 * 2 + 64];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"addr\":\"0x%08x\",\"len\":%u,\"hex\":\"",
        (unsigned)addr, (unsigned)len);
    /* Read byte-at-a-time via the debugger-safe read path. */
    for (uae_u32 i = 0; i < len && n < (int)sizeof body - 4; i++) {
        uae_u8 v = (uae_u8)(get_byte_debug(addr + i) & 0xFF);
        n += snprintf(body + n, sizeof body - n, "%02x", v);
    }
    n += snprintf(body + n, sizeof body - n, "\"}\n");
    send_response(fd, 200, body);
}

static void ep_pause(int fd) {
    activate_debugger();
    send_response(fd, 200, "{\"ok\":true,\"state\":\"paused\"}\n");
}

/* If any watchpoints exist, re-arm the memwatch bank-wrapping.  This is
 * a no-op if the wrap is already in place, but it's needed after every
 * /v1/reset because the reset path re-initializes mem_banks back to the
 * native (non-wrapped) implementations. */
static void rearm_watchpoints_if_any(void) {
    for (int i = 0; i < MEMWATCH_TOTAL; i++) {
        if (mwnodes[i].size != 0) {
            memwatch_setup();
            return;
        }
    }
}

static void ep_resume(int fd) {
    rearm_watchpoints_if_any();
    deactivate_debugger();
    send_response(fd, 200, "{\"ok\":true,\"state\":\"running\"}\n");
}

/* External — defined in debug.cpp.  These globals control single-stepping. */
extern int debugging;

static void ep_step(int fd, const char *qs) {
    /* Execute N instructions then re-pause.  Maps to FS-UAE's 't' trace
     * command internals: set skipaddr_doskip to step count, set
     * exception_debugging, set SPCFLAG_BRK.
     *
     * We must clear debugger_active (so the debugger's inner stdin-read
     * loop returns and the CPU resumes) WITHOUT clearing `debugging` —
     * the CPU loop only re-enters debug() when `debugging` is true, and
     * that re-entry is what makes single-stepping work. */
    char *ns = get_query_param(qs, "n");
    int n = 1;
    if (ns) {
        char nbuf[16];
        snprintf(nbuf, sizeof nbuf, "%s", ns);
        int ok = 0;
        n = (int)parse_uint(nbuf, &ok);
        if (!ok || n < 1 || n > 10000) {
            err_response(fd, 400, "bad n (1..10000)");
            return;
        }
    }
    rearm_watchpoints_if_any();
    skipaddr_doskip = n;
    no_trace_exceptions = 1;
    exception_debugging = 1;
    debugging = 1;            /* keep CPU loop checking SPCFLAG_BRK -> debug() */
    debugger_active = 0;      /* let debug_1's inner stdin-read loop return */
    set_special(SPCFLAG_BRK);
    char body[128];
    snprintf(body, sizeof body, "{\"ok\":true,\"stepped\":%d}\n", n);
    send_response(fd, 200, body);
}

static void ep_state_save(int fd, const char *qs) {
    char *path = get_query_param(qs, "path");
    if (!path || !*path) { err_response(fd, 400, "missing path"); return; }
    char path_buf[1024];
    snprintf(path_buf, sizeof path_buf, "%s", path);
    /* save_state() expects TCHAR (= char on POSIX builds with !UNICODE). */
    int rc = save_state(path_buf, "fsuae-rpc save");
    if (rc) {
        char body[1280];
        snprintf(body, sizeof body,
            "{\"ok\":true,\"path\":\"%s\"}\n", path_buf);
        send_response(fd, 200, body);
    } else {
        err_response(fd, 500, "save_state failed");
    }
}

/* ----- Run-state + breakpoint/watchpoint endpoints ----- */

static void ep_run_state(int fd) {
    /* debugger_active = 1 when paused (either by /v1/pause or because a
     * BP/WP fired and FS-UAE auto-paused).  Poll this after resume to
     * detect a BP/WP hit. */
    char body[128];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"state\":\"%s\"}\n",
        debugger_active ? "paused" : "running");
    send_response(fd, 200, body);
}

static void ep_bp_list(int fd) {
    char body[4096];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"breakpoints\":[");
    int first = 1;
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (!bpnodes[i].enabled) continue;
        n += snprintf(body + n, sizeof body - n,
            "%s{\"slot\":%d,\"addr\":\"0x%08x\"}",
            first ? "" : ",", i, (unsigned)bpnodes[i].addr);
        first = 0;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    send_response(fd, 200, body);
}

static void ep_bp_add(int fd, const char *qs) {
    char *as = get_query_param(qs, "addr");
    if (!as) { err_response(fd, 400, "missing addr"); return; }
    char addr_buf[64];
    snprintf(addr_buf, sizeof addr_buf, "%s", as);
    int ok = 0;
    uae_u32 addr = parse_uint(addr_buf, &ok);
    if (!ok) { err_response(fd, 400, "bad addr"); return; }
    /* find first free slot */
    int slot = -1;
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (!bpnodes[i].enabled) { slot = i; break; }
    }
    if (slot < 0) {
        err_response(fd, 500, "no free breakpoint slot");
        return;
    }
    bpnodes[slot].addr = addr;
    bpnodes[slot].enabled = 1;
    char body[256];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"slot\":%d,\"addr\":\"0x%08x\"}\n",
        slot, (unsigned)addr);
    send_response(fd, 200, body);
}

static void ep_bp_clear(int fd) {
    int cleared = 0;
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (bpnodes[i].enabled) {
            bpnodes[i].enabled = 0;
            bpnodes[i].addr = 0;
            cleared++;
        }
    }
    char body[128];
    snprintf(body, sizeof body, "{\"ok\":true,\"cleared\":%d}\n", cleared);
    send_response(fd, 200, body);
}

static void ep_wp_list(int fd) {
    char body[4096];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"watchpoints\":[");
    int first = 1;
    for (int i = 0; i < MEMWATCH_TOTAL; i++) {
        if (mwnodes[i].size == 0) continue;
        n += snprintf(body + n, sizeof body - n,
            "%s{\"slot\":%d,\"addr\":\"0x%08x\",\"size\":%d,\"rwi\":%d}",
            first ? "" : ",", i,
            (unsigned)mwnodes[i].addr, mwnodes[i].size, mwnodes[i].rwi);
        first = 0;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    send_response(fd, 200, body);
}

static int parse_rwi(const char *s) {
    /* FS-UAE memwatch rwi bits (from debug.cpp parser): R=1, W=2, I=4. */
    int v = 0;
    if (!s) return 3;  /* default: R+W */
    for (; *s; s++) {
        char c = (char)toupper((unsigned char)*s);
        if      (c == 'R') v |= 1;
        else if (c == 'W') v |= 2;
        else if (c == 'I') v |= 4;
        else if (c == '+' || c == ',' || c == ' ') continue;
        else return -1;
    }
    return v == 0 ? 3 : v;
}

static void ep_wp_add(int fd, const char *qs) {
    char *as = get_query_param(qs, "addr");
    if (!as) { err_response(fd, 400, "missing addr"); return; }
    char addr_buf[64];
    snprintf(addr_buf, sizeof addr_buf, "%s", as);
    char *ss = get_query_param(qs, "size");
    char size_buf[16] = "1";
    if (ss) snprintf(size_buf, sizeof size_buf, "%s", ss);
    char *rs = get_query_param(qs, "rwi");
    char rwi_buf[16] = "W";
    if (rs) snprintf(rwi_buf, sizeof rwi_buf, "%s", rs);
    int ok_a = 0, ok_s = 0;
    uae_u32 addr = parse_uint(addr_buf, &ok_a);
    uae_u32 size = parse_uint(size_buf, &ok_s);
    if (!ok_a) { err_response(fd, 400, "bad addr"); return; }
    if (!ok_s) { err_response(fd, 400, "bad size"); return; }
    int rwi = parse_rwi(rwi_buf);
    if (rwi < 0) { err_response(fd, 400, "bad rwi (use R, W, I, or combos)"); return; }
    if (size == 0 || size > 0x1000000) {
        err_response(fd, 400, "size out of range");
        return;
    }
    /* Lazy-init memwatch subsystem on first add. */
    initialize_memwatch(0);
    /* find first free slot */
    int slot = -1;
    for (int i = 0; i < MEMWATCH_TOTAL; i++) {
        if (mwnodes[i].size == 0) { slot = i; break; }
    }
    if (slot < 0) { err_response(fd, 500, "no free watchpoint slot"); return; }
    struct memwatch_node *mwn = &mwnodes[slot];
    mwn->addr = addr;
    mwn->size = (int)size;
    mwn->rwi = rwi;
    mwn->val_enabled = 0;
    mwn->val_mask = 0xFFFFFFFF;
    mwn->val = 0;
    /* MW_MASK_ALL covers every access source — CPU read/write/instr fetch,
     * blitter A/B/C/D, copper, disk, audio, bitplane, sprite.  Required —
     * a watchpoint with access_mask=0 never fires. */
    mwn->access_mask = MW_MASK_ALL;
    mwn->reg = 0xFFFFFFFF;
    mwn->frozen = 0;
    /* mustchange=1 → only fire if the write actually changes memory
     * (skips writes-of-same-value, useful for finding the FIRST modifying
     * write to an address after a memset-style init). */
    char *mc = get_query_param(qs, "mustchange");
    mwn->mustchange = (mc && (mc[0]=='1' || mc[0]=='y' || mc[0]=='Y')) ? 1 : 0;
    mwn->modval = 0;
    mwn->modval_written = 0;
    mwn->pc = 0xFFFFFFFF;
    memwatch_setup();
    char body[256];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"slot\":%d,\"addr\":\"0x%08x\",\"size\":%u,\"rwi\":%d}\n",
        slot, (unsigned)addr, (unsigned)size, rwi);
    send_response(fd, 200, body);
}

static void ep_wp_clear(int fd) {
    int cleared = 0;
    for (int i = 0; i < MEMWATCH_TOTAL; i++) {
        if (mwnodes[i].size != 0) {
            mwnodes[i].size = 0;
            cleared++;
        }
    }
    if (cleared > 0) memwatch_setup();
    char body[128];
    snprintf(body, sizeof body, "{\"ok\":true,\"cleared\":%d}\n", cleared);
    send_response(fd, 200, body);
}

static void ep_reset(int fd, const char *qs) {
    /* hard=1 (default) is a power-on-style reset, hard=0 is a soft reset. */
    char *hs = get_query_param(qs, "hard");
    int hard = 1;
    if (hs && (hs[0] == '0' || hs[0] == 'f' || hs[0] == 'F'))
        hard = 0;
    uae_reset(hard, 1);
    char body[128];
    snprintf(body, sizeof body, "{\"ok\":true,\"reset\":\"%s\"}\n",
        hard ? "hard" : "soft");
    send_response(fd, 200, body);
}

/* ----- Mutation endpoints ----- */

/* Decode a hex string ("DEADBEEF" or "deadbeef") into bytes.  Returns
 * the number of bytes decoded, or -1 on parse failure. */
static int hex_decode(const char *s, uae_u8 *out, int max) {
    int n = 0;
    while (*s && n < max) {
        char c1 = *s++;
        if (!*s) return -1;  /* odd hex length */
        char c2 = *s++;
        int v1 = (c1 >= '0' && c1 <= '9') ? c1 - '0' :
                 (c1 >= 'a' && c1 <= 'f') ? c1 - 'a' + 10 :
                 (c1 >= 'A' && c1 <= 'F') ? c1 - 'A' + 10 : -1;
        int v2 = (c2 >= '0' && c2 <= '9') ? c2 - '0' :
                 (c2 >= 'a' && c2 <= 'f') ? c2 - 'a' + 10 :
                 (c2 >= 'A' && c2 <= 'F') ? c2 - 'A' + 10 : -1;
        if (v1 < 0 || v2 < 0) return -1;
        out[n++] = (uae_u8)((v1 << 4) | v2);
    }
    return *s ? -1 : n;
}

static void ep_mem_write(int fd, const char *qs) {
    char *as = get_query_param(qs, "addr");
    if (!as) { err_response(fd, 400, "missing addr"); return; }
    char addr_buf[64];
    snprintf(addr_buf, sizeof addr_buf, "%s", as);
    char *hs = get_query_param(qs, "hex");
    if (!hs) { err_response(fd, 400, "missing hex"); return; }
    char hex_buf[8192];
    snprintf(hex_buf, sizeof hex_buf, "%s", hs);
    int ok = 0;
    uae_u32 addr = parse_uint(addr_buf, &ok);
    if (!ok) { err_response(fd, 400, "bad addr"); return; }
    static uae_u8 bytes[4096];
    int n = hex_decode(hex_buf, bytes, sizeof bytes);
    if (n < 0) { err_response(fd, 400, "bad hex"); return; }
    if (n == 0) { err_response(fd, 400, "empty hex"); return; }
    /* debug_write_memory_8 returns 1 on success, -1 on no bank.
     * Loop one byte at a time — slower than a packed write but lets us
     * report partial failures and works for any address range. */
    int wrote = 0;
    for (int i = 0; i < n; i++) {
        if (debug_write_memory_8(addr + i, bytes[i]) == 1)
            wrote++;
    }
    char body[256];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"addr\":\"0x%08x\",\"requested\":%d,\"written\":%d}\n",
        (unsigned)addr, n, wrote);
    send_response(fd, 200, body);
}

/* Map a register name to a pointer into the regs struct.  Returns NULL
 * if the name doesn't match a writable register. */
static uae_u32 *reg_ptr_by_name(const char *name) {
    if (!name) return NULL;
    if (strlen(name) == 2 &&
        (name[0] == 'd' || name[0] == 'D') &&
         name[1] >= '0' && name[1] <= '7')
        return &regs.regs[name[1] - '0'];
    if (strlen(name) == 2 &&
        (name[0] == 'a' || name[0] == 'A') &&
         name[1] >= '0' && name[1] <= '7')
        return &regs.regs[8 + name[1] - '0'];
    if (!strcasecmp(name, "usp")) return &regs.usp;
    if (!strcasecmp(name, "isp")) return &regs.isp;
    /* PC and SR need special setters — not raw uae_u32 writes — see below */
    return NULL;
}

static void ep_cpu_write(int fd, const char *qs) {
    char *reg = get_query_param(qs, "reg");
    if (!reg) { err_response(fd, 400, "missing reg"); return; }
    char reg_buf[16];
    snprintf(reg_buf, sizeof reg_buf, "%s", reg);
    char *vs = get_query_param(qs, "value");
    if (!vs) { err_response(fd, 400, "missing value"); return; }
    char val_buf[64];
    snprintf(val_buf, sizeof val_buf, "%s", vs);
    int ok = 0;
    uae_u32 value = parse_uint(val_buf, &ok);
    if (!ok) { err_response(fd, 400, "bad value"); return; }

    if (!strcasecmp(reg_buf, "pc")) {
        m68k_setpc(value);
        char body[160];
        snprintf(body, sizeof body,
            "{\"ok\":true,\"reg\":\"pc\",\"value\":\"0x%08x\"}\n",
            (unsigned)value);
        send_response(fd, 200, body);
        return;
    }
    if (!strcasecmp(reg_buf, "sr")) {
        regs.sr = (uae_u16)(value & 0xFFFF);
        MakeFromSR();   /* propagate the SR bits into the CCR / mode state */
        char body[160];
        snprintf(body, sizeof body,
            "{\"ok\":true,\"reg\":\"sr\",\"value\":\"0x%04x\"}\n",
            (unsigned)(value & 0xFFFF));
        send_response(fd, 200, body);
        return;
    }
    uae_u32 *p = reg_ptr_by_name(reg_buf);
    if (!p) { err_response(fd, 400, "unknown reg"); return; }
    *p = value;
    char body[160];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"reg\":\"%s\",\"value\":\"0x%08x\"}\n",
        reg_buf, (unsigned)value);
    send_response(fd, 200, body);
}

/* ----- Disassembly ----- */

static void ep_disasm(int fd, const char *qs) {
    char *as = get_query_param(qs, "addr");
    char addr_buf[64];
    if (as) snprintf(addr_buf, sizeof addr_buf, "%s", as);
    char *cs = get_query_param(qs, "count");
    char count_buf[16];
    if (cs) snprintf(count_buf, sizeof count_buf, "%s", cs);

    uae_u32 addr;
    int count = 1;
    if (as) {
        int ok = 0;
        addr = parse_uint(addr_buf, &ok);
        if (!ok) { err_response(fd, 400, "bad addr"); return; }
    } else {
        addr = (uae_u32)m68k_getpc();
    }
    if (cs) {
        int ok = 0;
        count = (int)parse_uint(count_buf, &ok);
        if (!ok || count < 1 || count > 256) {
            err_response(fd, 400, "bad count (1..256)");
            return;
        }
    }
    /* m68k_disasm_2 writes formatted text into the buffer.  Each instr is
     * up to MAX_LINEWIDTH chars + newline.  We then split on lines and
     * emit them as a JSON array. */
    int bufsz = (MAX_LINEWIDTH + 4) * count;
    TCHAR *dbuf = (TCHAR *)malloc(bufsz);
    if (!dbuf) { err_response(fd, 500, "alloc failed"); return; }
    uaecptr nextpc = 0;
    m68k_disasm_2(dbuf, bufsz, addr, &nextpc, count, NULL, NULL, 1);

    /* JSON-escape any backslash or quote in the disasm text.  The 68k
     * disassembler doesn't produce control chars or non-ASCII so this is
     * a simple pass. */
    static char body[131072];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"addr\":\"0x%08x\",\"nextpc\":\"0x%08x\",\"lines\":[",
        (unsigned)addr, (unsigned)nextpc);
    int first = 1;
    char *p = dbuf;
    while (*p && n < (int)sizeof body - 128) {
        char *eol = p;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        if (eol == p) { p++; continue; }  /* skip blank line */
        n += snprintf(body + n, sizeof body - n,
            "%s\"", first ? "" : ",");
        first = 0;
        for (char *c = p; c < eol && n < (int)sizeof body - 8; c++) {
            if (*c == '"' || *c == '\\') {
                body[n++] = '\\';
                body[n++] = *c;
            } else if ((unsigned char)*c < 0x20) {
                /* skip control characters (tabs get rendered as spaces) */
                body[n++] = ' ';
            } else {
                body[n++] = *c;
            }
        }
        n += snprintf(body + n, sizeof body - n, "\"");
        p = eol;
        while (*p == '\n' || *p == '\r') p++;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    free(dbuf);
    send_response(fd, 200, body);
}

/* ----- Chipset register snapshot ----- */

static void ep_custom(int fd) {
    /* Snapshot of the most-useful Amiga chipset registers.  We mix:
     *   - direct reads from globals (dmacon, intena, intreq, bplcon0)
     *   - get_word_debug() reads of the chipset register window at $DFF000
     *     for ones with public read addresses (VPOSR, VHPOSR, ADKCON,
     *     COPxLC, BPLxPT, etc.)
     *
     * This gives a frontend debugger enough state to render the Amiga's
     * current display configuration, IRQ state, and DMA channel status. */
    char body[4096];
    int n = 0;
    n += snprintf(body + n, sizeof body - n,
        "{\"ok\":true,"
        "\"dmacon\":\"0x%04x\","
        "\"intena\":\"0x%04x\","
        "\"intreq\":\"0x%04x\","
        "\"bplcon0\":\"0x%04x\","
        "\"vposr\":\"0x%04x\","
        "\"vhposr\":\"0x%04x\","
        "\"adkconr\":\"0x%04x\","
        "\"diwstrt\":\"0x%04x\","
        "\"diwstop\":\"0x%04x\","
        "\"ddfstrt\":\"0x%04x\","
        "\"ddfstop\":\"0x%04x\"",
        (unsigned)dmacon,
        (unsigned)intena,
        (unsigned)intreq,
        (unsigned)bplcon0,
        (unsigned)(get_word_debug(0xDFF004) & 0xFFFF),
        (unsigned)(get_word_debug(0xDFF006) & 0xFFFF),
        (unsigned)(get_word_debug(0xDFF010) & 0xFFFF),
        (unsigned)(get_word_debug(0xDFF08E) & 0xFFFF),
        (unsigned)(get_word_debug(0xDFF090) & 0xFFFF),
        (unsigned)(get_word_debug(0xDFF092) & 0xFFFF),
        (unsigned)(get_word_debug(0xDFF094) & 0xFFFF));

    /* Copper pointers — COP1LC at $080/$082, COP2LC at $084/$086.
     * These are write-only on real hw, so reading the chipset window
     * returns garbage; but FS-UAE's debug-read path reflects the live
     * shadow.  In practice these come back 0xFFFF; we expose them
     * anyway for completeness. */
    uae_u32 cop1lc = ((uae_u32)(get_word_debug(0xDFF080) & 0xFFFF) << 16)
                   |  (uae_u32)(get_word_debug(0xDFF082) & 0xFFFF);
    uae_u32 cop2lc = ((uae_u32)(get_word_debug(0xDFF084) & 0xFFFF) << 16)
                   |  (uae_u32)(get_word_debug(0xDFF086) & 0xFFFF);
    n += snprintf(body + n, sizeof body - n,
        ",\"cop1lc\":\"0x%08x\",\"cop2lc\":\"0x%08x\"",
        (unsigned)cop1lc, (unsigned)cop2lc);

    /* Bitplane pointers — BPLxPT at $0E0..$0F6.  Same caveat as copper. */
    n += snprintf(body + n, sizeof body - n, ",\"bplpt\":[");
    for (int i = 0; i < 8; i++) {
        uae_u32 base = 0xDFF0E0 + i * 4;
        uae_u32 ptr = ((uae_u32)(get_word_debug(base) & 0xFFFF) << 16)
                    |  (uae_u32)(get_word_debug(base + 2) & 0xFFFF);
        n += snprintf(body + n, sizeof body - n,
            "%s\"0x%08x\"", i ? "," : "", (unsigned)ptr);
    }
    n += snprintf(body + n, sizeof body - n, "]");

    n += snprintf(body + n, sizeof body - n, "}\n");
    send_response(fd, 200, body);
}

/* ----- State load ----- */

static void ep_state_load(int fd, const char *qs) {
    char *path = get_query_param(qs, "path");
    if (!path || !*path) { err_response(fd, 400, "missing path"); return; }
    char path_buf[1024];
    snprintf(path_buf, sizeof path_buf, "%s", path);
    /* restore_state() returns void in this FS-UAE version; success is
     * best inferred from a subsequent /v1/cpu showing the expected PC.
     * We still surface a 200 here on the assumption that the path is
     * valid — caller should verify. */
    restore_state(path_buf);
    char body[1280];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"path\":\"%s\"}\n", path_buf);
    send_response(fd, 200, body);
}

/* ----- Request parser + dispatcher ----- */

static void handle_request(int fd) {
    char buf[2048];
    ssize_t rd = recv(fd, buf, sizeof buf - 1, 0);
    if (rd <= 0) { close(fd); return; }
    buf[rd] = '\0';
    /* Parse the request line: "METHOD PATH HTTP/1.1\r\n..." */
    char method[8] = {0}, path[512] = {0};
    if (sscanf(buf, "%7s %511s", method, path) != 2) {
        err_response(fd, 400, "malformed");
        close(fd);
        return;
    }
    /* Split path / query. */
    char *qs = strchr(path, '?');
    if (qs) { *qs = '\0'; qs++; }

    /* Dispatch. */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/ping") == 0) {
        ep_ping(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/state") == 0) {
        ep_run_state(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/cpu") == 0) {
        ep_cpu(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/mem") == 0) {
        ep_mem(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/pause") == 0) {
        ep_pause(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/resume") == 0) {
        ep_resume(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/step") == 0) {
        ep_step(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/watchpoints/rearm") == 0) {
        memwatch_setup();
        send_response(fd, 200, "{\"ok\":true}\n");
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/state/save") == 0) {
        ep_state_save(fd, qs);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/breakpoints") == 0) {
        ep_bp_list(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/breakpoints") == 0) {
        ep_bp_add(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/breakpoints/clear") == 0) {
        ep_bp_clear(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/watchpoints") == 0) {
        ep_wp_list(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/watchpoints") == 0) {
        ep_wp_add(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/watchpoints/clear") == 0) {
        ep_wp_clear(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/reset") == 0) {
        ep_reset(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/mem") == 0) {
        ep_mem_write(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/cpu") == 0) {
        ep_cpu_write(fd, qs);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/disasm") == 0) {
        ep_disasm(fd, qs);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/custom") == 0) {
        ep_custom(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/state/load") == 0) {
        ep_state_load(fd, qs);
    } else {
        err_response(fd, 404, "no such endpoint");
    }
    close(fd);
}

/* ----- Worker thread: accept loop ----- */

static void *rpc_worker(void *arg) {
    int port = (int)(intptr_t)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "[fsuae-rpc] socket() failed: %s\n", strerror(errno));
        return NULL;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost only */
    sa.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "[fsuae-rpc] bind(127.0.0.1:%d) failed: %s\n",
            port, strerror(errno));
        close(srv);
        return NULL;
    }
    if (listen(srv, 8) < 0) {
        fprintf(stderr, "[fsuae-rpc] listen() failed: %s\n", strerror(errno));
        close(srv);
        return NULL;
    }
    fprintf(stderr, "[fsuae-rpc] listening on http://127.0.0.1:%d/\n", port);
    for (;;) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[fsuae-rpc] accept() failed: %s\n", strerror(errno));
            break;
        }
        handle_request(cfd);
    }
    close(srv);
    return NULL;
}

/* ----- Public init (called from FS-UAE startup) ----- */

extern "C" void fsuae_rpc_init(void) {
    const char *env = getenv("FSUAE_RPC_PORT");
    if (!env || !*env) return; /* disabled */
    int port = atoi(env);
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[fsuae-rpc] invalid FSUAE_RPC_PORT='%s'\n", env);
        return;
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, rpc_worker, (void *)(intptr_t)port) != 0) {
        fprintf(stderr, "[fsuae-rpc] pthread_create failed: %s\n",
            strerror(errno));
        return;
    }
    pthread_detach(tid);

    /* Optional: start paused so the client can install breakpoints /
     * watchpoints BEFORE the emulator executes its first instruction.
     * Useful for catching very-early-boot ROM writes (chip-RAM init,
     * IRQ vector install, etc.). */
    const char *pause = getenv("FSUAE_RPC_PAUSE_AT_BOOT");
    if (pause && (pause[0] == '1' || pause[0] == 'y' || pause[0] == 'Y')) {
        fprintf(stderr, "[fsuae-rpc] starting paused (FSUAE_RPC_PAUSE_AT_BOOT=1)\n");
        activate_debugger();
    }
}

#endif /* !_WIN32 */
