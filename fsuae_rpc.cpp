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
 *     GET  /v1/disasm?addr=HEX&count=N&annotate=1
 *                                          disassemble N insns from addr
 *                                          (annotate=1 adds exec.fn() to
 *                                           JSR/JMP -nn(A6) lines)
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
 * v1 endpoints — symbol resolution:
 *     GET  /v1/symbols                     full known-address table
 *     GET  /v1/symbols/lookup?addr=HEX     lookup name for an address
 *     GET  /v1/fd/exec                     exec.library function table
 *     GET  /v1/fd/lookup?offset=N          lookup exec function by -offset
 *
 * v1 endpoints — breakpoints & watchpoints:
 *     POST /v1/breakpoints?addr=HEX        install PC breakpoint, auto-pause on hit
 *     GET  /v1/breakpoints                 list active breakpoints
 *     POST /v1/breakpoints/clear           remove all breakpoints
 *     POST /v1/watchpoints?addr=HEX&size=N&rwi=W&mustchange=0|1
 *                              &val=HEX&valmask=HEX
 *                                          install memory watchpoint
 *                                          (val/valmask filter — fire only on
 *                                           matching value)
 *     GET  /v1/watchpoints                 list active watchpoints
 *     POST /v1/watchpoints/clear           remove all watchpoints
 *     POST /v1/watchpoints/rearm           re-wrap mem banks (use after /v1/reset)
 *     GET  /v1/watchpoints/last            details of last triggered watchpoint
 *                                          (PC at hit, addr, value, etc.)
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

/* Embedded single-page web debugger.  Generated from web/index.html.   */
#include "web_index.inc"

/* The patch un-statics these so we can drive memwatch installation
 * and single-step without re-entering the in-process debugger command parser.
 * Defined in debug.cpp (C++ linkage); declared here to match. */
void memwatch_setup(void);
void initialize_memwatch(int mode);
extern int skipaddr_doskip;
extern int no_trace_exceptions;

/* Forward decl — defined later in this file; used by the disasm
 * annotator before its full definition. */
static const char *exec_fd_lookup(int neg_offset);

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

#ifndef _WIN32
# include <pthread.h>
# include <unistd.h>
# include <signal.h>
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

static void send_response_ct(int fd, int code, const char *body,
                              size_t body_len, const char *content_type) {
    char hdr[512];
    snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "\r\n",
        code,
        code == 200 ? "OK" :
        code == 400 ? "Bad Request" :
        code == 404 ? "Not Found" :
        code == 500 ? "Internal Server Error" : "Other",
        content_type, body_len);
    send_str(fd, hdr);
    /* Write the body raw — may contain NULs in binary cases, but for
     * our JSON / HTML responses it's always C-string-safe.            */
    size_t off = 0;
    while (off < body_len) {
        ssize_t w = send(fd, body + off, body_len - off, 0);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

static void send_response(int fd, int code, const char *body) {
    send_response_ct(fd, code, body, strlen(body), "application/json");
}

static void ep_ui(int fd) {
    send_response_ct(fd, 200, WEB_UI_HTML, sizeof(WEB_UI_HTML) - 1,
                     "text/html; charset=utf-8");
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
    /* Optional value-match: val=HEX (must match) + valmask=HEX
     * (defaults to 0xFFFFFFFF = full width).  Useful for catching a
     * specific signature, e.g. val=0x20430C68 to catch the moment a
     * relocatable image header lands at an address. */
    char *vs = get_query_param(qs, "val");
    char *vms = get_query_param(qs, "valmask");
    mwn->val_enabled = 0;
    mwn->val_mask = 0xFFFFFFFF;
    mwn->val = 0;
    if (vs) {
        char vbuf[64];
        snprintf(vbuf, sizeof vbuf, "%s", vs);
        int ok = 0;
        mwn->val = parse_uint(vbuf, &ok);
        if (!ok) { err_response(fd, 400, "bad val"); return; }
        mwn->val_enabled = 1;
        /* val_size is used by memwatch_func's match window; set to the
         * watchpoint's size, max 4 bytes (longword) per the rwi check. */
        mwn->val_size = (int)size;
        if (mwn->val_size > 4) mwn->val_size = 4;
        if (vms) {
            char mbuf[64];
            snprintf(mbuf, sizeof mbuf, "%s", vms);
            ok = 0;
            mwn->val_mask = parse_uint(mbuf, &ok);
            if (!ok) { err_response(fd, 400, "bad valmask"); return; }
        }
    }
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

    /* Optional annotation: if annotate=1 (or ?annot=1), look at each
     * line for `JSR -nn(A6)` and `JMP -nn(A6)` and append the matching
     * exec-library function name (if known).  This is heuristic — it
     * blindly assumes A6 points to ExecBase, which holds for most
     * Kickstart code but not for app code that swaps A6 to other libs. */
    int annotate = 0;
    char *anns = get_query_param(qs, "annotate");
    if (!anns) anns = get_query_param(qs, "annot");
    if (anns && (anns[0] == '1' || anns[0] == 'y' || anns[0] == 'Y'))
        annotate = 1;

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
                body[n++] = ' ';
            } else {
                body[n++] = *c;
            }
        }
        /* Try to annotate JSR / JMP with -nn(A6).  m68k_disasm_2 prints
         * the form `JSR -nn(A6)` or `JSR (A6, -$nn)` depending on the
         * branch — we match both. */
        if (annotate) {
            char line[1024];
            int llen = (eol - p) < (int)sizeof line - 1 ? (int)(eol - p) : (int)sizeof line - 1;
            memcpy(line, p, llen);
            line[llen] = 0;
            const char *fn = NULL;
            /* Look for "(A6, -$xxxx)" or "$ffffXXXX,A6" style references */
            char *paren = strstr(line, "(A6,");
            if (!paren) paren = strstr(line, "(a6,");
            if (paren) {
                /* Find the displacement.  Format: " -$XX,A6) == $.." */
                char *dollar = strchr(paren, '$');
                if (dollar) {
                    int neg = (dollar > line && dollar[-1] == '-') ||
                              strstr(paren, "-$") != NULL;
                    /* Parse hex displacement until ')' or ',' */
                    int v = 0;
                    char *d = dollar + 1;
                    while ((*d >= '0' && *d <= '9') ||
                           (*d >= 'a' && *d <= 'f') ||
                           (*d >= 'A' && *d <= 'F')) {
                        v = (v << 4) | (*d >= 'a' ? *d - 'a' + 10 :
                                        *d >= 'A' ? *d - 'A' + 10 : *d - '0');
                        d++;
                    }
                    /* If parsed as positive hex with leading 0xFFFF, it's the
                     * 16-bit signed displacement in unsigned form. */
                    if (v > 0x7FFF) v -= 0x10000;
                    if (neg && v > 0) v = -v;
                    if (v < 0) fn = exec_fd_lookup(v);
                }
            }
            if (fn) {
                n += snprintf(body + n, sizeof body - n,
                    "       ; exec.%s()", fn);
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

/* memwatch_func records the trigger context here: PC, addr, value, rwi,
 * size, access_mask, and reg.  Reading these is the only reliable way to
 * find the exact PC that triggered a watchpoint — by the time the
 * debugger pauses, an IRQ may have already redirected the CPU.        */
extern struct memwatch_node mwhit;
extern int memwatch_triggered;

static void ep_wp_last(int fd) {
    char body[1024];
    /* memwatch_triggered = slot_index + 1, or 0 if nothing fired since
     * last clear.  Surface both the trigger context and "no hit". */
    if (memwatch_triggered == 0) {
        snprintf(body, sizeof body, "{\"ok\":true,\"hit\":false}\n");
        send_response(fd, 200, body);
        return;
    }
    /* Decode rwi back into the user-visible RWI flags. */
    char rwi_str[8] = "";
    int p = 0;
    if (mwhit.rwi & 1) rwi_str[p++] = 'R';
    if (mwhit.rwi & 2) rwi_str[p++] = 'W';
    if (mwhit.rwi & 4) rwi_str[p++] = 'I';
    rwi_str[p] = 0;
    snprintf(body, sizeof body,
        "{\"ok\":true,\"hit\":true,"
        "\"slot\":%d,"
        "\"addr\":\"0x%08x\","
        "\"pc\":\"0x%08x\","
        "\"rwi\":\"%s\","
        "\"size\":%d,"
        "\"value\":\"0x%08x\","
        "\"access_mask\":\"0x%08x\"}\n",
        memwatch_triggered - 1,
        (unsigned)mwhit.addr,
        (unsigned)mwhit.pc,
        rwi_str,
        mwhit.size,
        (unsigned)mwhit.val,
        (unsigned)mwhit.access_mask);
    send_response(fd, 200, body);
}

/* ----- Symbol resolution -----
 *
 * Static table of well-known Amiga addresses.  Covers:
 *   - 68000 CPU exception vectors (0x000..0x0FC)
 *   - Custom chipset registers ($DFF000..$DFF1FE)
 *   - CIA-A / CIA-B registers ($BFE001..$BFEF01 / $BFD000..$BFDF00)
 *   - Common Kickstart entry points + ExecBase offsets
 *
 * No external symbol file load yet (no .fd parsing, no symbol table
 * upload).  This is the minimum useful baseline for navigating an
 * Amiga debugger session. */

struct sym_entry { uae_u32 addr; const char *name; const char *desc; };

static const struct sym_entry SYMBOLS[] = {
    /* 68000 vectors */
    {0x000, "ResetSSP",   "reset supervisor stack pointer"},
    {0x004, "ResetPC",    "reset program counter"},
    {0x008, "BusError",   "bus error vector"},
    {0x00C, "AddrError",  "address error vector"},
    {0x010, "IllegalOp",  "illegal instruction vector"},
    {0x014, "ZeroDiv",    "divide by zero vector"},
    {0x018, "CHK",        "CHK instruction vector"},
    {0x01C, "TRAPV",      "TRAPV instruction vector"},
    {0x020, "Privilege",  "privilege violation vector"},
    {0x024, "Trace",      "trace vector"},
    {0x028, "LineA",      "LINE A unimplemented vector"},
    {0x02C, "LineF",      "LINE F unimplemented vector"},
    {0x060, "Spurious",   "spurious interrupt vector"},
    {0x064, "Autovec1",   "autovec L1 (TBE/DSKBLK/SOFT)"},
    {0x068, "Autovec2",   "autovec L2 (CIA-A / PORTS)"},
    {0x06C, "Autovec3",   "autovec L3 (COPER/VERTB/BLIT)"},
    {0x070, "Autovec4",   "autovec L4 (AUD0-3)"},
    {0x074, "Autovec5",   "autovec L5 (RBF/DSKSYNC)"},
    {0x078, "Autovec6",   "autovec L6 (CIA-B / EXTER)"},
    {0x07C, "Autovec7",   "autovec L7 (NMI)"},
    {0x080, "TRAP0",      "TRAP #0"},
    {0x084, "TRAP1",      "TRAP #1"},
    {0x088, "TRAP2",      "TRAP #2"},
    {0x08C, "TRAP3",      "TRAP #3"},
    {0x090, "TRAP4",      "TRAP #4"},
    {0x094, "TRAP5",      "TRAP #5"},
    {0x098, "TRAP6",      "TRAP #6"},
    {0x09C, "TRAP7",      "TRAP #7"},
    {0x0A0, "TRAP8",      "TRAP #8"},
    {0x0A4, "TRAP9",      "TRAP #9"},
    {0x0A8, "TRAP10",     "TRAP #10"},
    {0x0AC, "TRAP11",     "TRAP #11"},
    {0x0B0, "TRAP12",     "TRAP #12"},
    {0x0B4, "TRAP13",     "TRAP #13"},
    {0x0B8, "TRAP14",     "TRAP #14"},
    {0x0BC, "TRAP15",     "TRAP #15"},

    /* Exec lowmem */
    {0x0004, "ExecBase*", "pointer to ExecBase"},

    /* Custom chipset — read registers */
    {0xDFF000, "BLTDDAT",   "blitter dest early read"},
    {0xDFF002, "DMACONR",   "DMA control read"},
    {0xDFF004, "VPOSR",     "raster pos high"},
    {0xDFF006, "VHPOSR",    "raster pos low"},
    {0xDFF008, "DSKDATR",   "disk data early read"},
    {0xDFF00A, "JOY0DAT",   "joystick 0 read"},
    {0xDFF00C, "JOY1DAT",   "joystick 1 read"},
    {0xDFF00E, "CLXDAT",    "collision data read"},
    {0xDFF010, "ADKCONR",   "audio/disk control read"},
    {0xDFF012, "POT0DAT",   "pot 0 read"},
    {0xDFF014, "POT1DAT",   "pot 1 read"},
    {0xDFF016, "POTGOR",    "pot port read"},
    {0xDFF018, "SERDATR",   "serial data read"},
    {0xDFF01A, "DSKBYTR",   "disk byte read"},
    {0xDFF01C, "INTENAR",   "interrupt enable read"},
    {0xDFF01E, "INTREQR",   "interrupt request read"},
    /* Custom chipset — write registers */
    {0xDFF020, "DSKPT",     "disk DMA pointer (high)"},
    {0xDFF024, "DSKLEN",    "disk length / DMA enable"},
    {0xDFF026, "DSKDAT",    "disk data write"},
    {0xDFF028, "REFPTR",    "refresh pointer"},
    {0xDFF02A, "VPOSW",     "raster pos high write"},
    {0xDFF02C, "VHPOSW",    "raster pos low write"},
    {0xDFF02E, "COPCON",    "copper control"},
    {0xDFF030, "SERDAT",    "serial data write"},
    {0xDFF032, "SERPER",    "serial period"},
    {0xDFF034, "POTGO",     "pot start"},
    {0xDFF036, "JOYTEST",   "joystick test"},
    {0xDFF080, "COP1LCH",   "copper list 1 high"},
    {0xDFF082, "COP1LCL",   "copper list 1 low"},
    {0xDFF084, "COP2LCH",   "copper list 2 high"},
    {0xDFF086, "COP2LCL",   "copper list 2 low"},
    {0xDFF088, "COPJMP1",   "trigger copper list 1"},
    {0xDFF08A, "COPJMP2",   "trigger copper list 2"},
    {0xDFF08E, "DIWSTRT",   "display window start"},
    {0xDFF090, "DIWSTOP",   "display window stop"},
    {0xDFF092, "DDFSTRT",   "data fetch start"},
    {0xDFF094, "DDFSTOP",   "data fetch stop"},
    {0xDFF096, "DMACON",    "DMA control write"},
    {0xDFF098, "CLXCON",    "collision control"},
    {0xDFF09A, "INTENA",    "interrupt enable write"},
    {0xDFF09C, "INTREQ",    "interrupt request write"},
    {0xDFF09E, "ADKCON",    "audio/disk control write"},
    /* Audio channels */
    {0xDFF0A0, "AUD0LCH",   "audio 0 location high"},
    {0xDFF0A2, "AUD0LCL",   "audio 0 location low"},
    {0xDFF0A4, "AUD0LEN",   "audio 0 length"},
    {0xDFF0A6, "AUD0PER",   "audio 0 period"},
    {0xDFF0A8, "AUD0VOL",   "audio 0 volume"},
    /* Bitplane pointers */
    {0xDFF0E0, "BPL1PTH",   "bitplane 1 pointer high"},
    {0xDFF0E2, "BPL1PTL",   "bitplane 1 pointer low"},
    {0xDFF0E4, "BPL2PTH",   "bitplane 2 pointer high"},
    {0xDFF0E8, "BPL3PTH",   "bitplane 3 pointer high"},
    {0xDFF0EC, "BPL4PTH",   "bitplane 4 pointer high"},
    {0xDFF0F0, "BPL5PTH",   "bitplane 5 pointer high"},
    {0xDFF0F4, "BPL6PTH",   "bitplane 6 pointer high"},
    {0xDFF100, "BPLCON0",   "bitplane control 0"},
    {0xDFF102, "BPLCON1",   "bitplane control 1"},
    {0xDFF104, "BPLCON2",   "bitplane control 2"},
    {0xDFF108, "BPL1MOD",   "bitplane 1 modulo"},
    {0xDFF10A, "BPL2MOD",   "bitplane 2 modulo"},
    /* Blitter */
    {0xDFF040, "BLTCON0",   "blitter control 0"},
    {0xDFF042, "BLTCON1",   "blitter control 1"},
    {0xDFF044, "BLTAFWM",   "blitter A first-word mask"},
    {0xDFF046, "BLTALWM",   "blitter A last-word mask"},
    {0xDFF048, "BLTCPTH",   "blitter C pointer high"},
    {0xDFF04A, "BLTCPTL",   "blitter C pointer low"},
    {0xDFF04C, "BLTBPTH",   "blitter B pointer high"},
    {0xDFF050, "BLTAPTH",   "blitter A pointer high"},
    {0xDFF054, "BLTDPTH",   "blitter D pointer high"},
    {0xDFF058, "BLTSIZE",   "blitter size / start"},
    {0xDFF060, "BLTCMOD",   "blitter C modulo"},
    {0xDFF062, "BLTBMOD",   "blitter B modulo"},
    {0xDFF064, "BLTAMOD",   "blitter A modulo"},
    {0xDFF066, "BLTDMOD",   "blitter D modulo"},
    {0xDFF070, "BLTCDAT",   "blitter C data"},
    {0xDFF072, "BLTBDAT",   "blitter B data"},
    {0xDFF074, "BLTADAT",   "blitter A data"},
    /* Color registers */
    {0xDFF180, "COLOR00",   "color 0 (background)"},
    {0xDFF182, "COLOR01",   "color 1"},
    {0xDFF184, "COLOR02",   "color 2"},
    {0xDFF186, "COLOR03",   "color 3"},
    /* CIA-A */
    {0xBFE001, "ciaa_pra",  "CIA-A port A (LED, OVL, disk control)"},
    {0xBFE101, "ciaa_prb",  "CIA-A port B (parallel)"},
    {0xBFE201, "ciaa_ddra", "CIA-A DDR port A"},
    {0xBFE301, "ciaa_ddrb", "CIA-A DDR port B"},
    {0xBFE401, "ciaa_ta_lo","CIA-A timer A lo"},
    {0xBFE501, "ciaa_ta_hi","CIA-A timer A hi"},
    {0xBFE601, "ciaa_tb_lo","CIA-A timer B lo"},
    {0xBFE701, "ciaa_tb_hi","CIA-A timer B hi"},
    {0xBFE801, "ciaa_e_lo", "CIA-A TOD event lo"},
    {0xBFE901, "ciaa_e_mid","CIA-A TOD event mid"},
    {0xBFEA01, "ciaa_e_hi", "CIA-A TOD event hi"},
    {0xBFEC01, "ciaa_sdr",  "CIA-A serial data (keyboard)"},
    {0xBFED01, "ciaa_icr",  "CIA-A IRQ control / read"},
    {0xBFEE01, "ciaa_cra",  "CIA-A control reg A"},
    {0xBFEF01, "ciaa_crb",  "CIA-A control reg B"},
    /* CIA-B */
    {0xBFD000, "ciab_pra",  "CIA-B port A (serial RTS/DTR/CD)"},
    {0xBFD100, "ciab_prb",  "CIA-B port B (motor/sel/side/step/dir)"},
    {0xBFD200, "ciab_ddra", "CIA-B DDR port A"},
    {0xBFD300, "ciab_ddrb", "CIA-B DDR port B"},
    {0xBFD400, "ciab_ta_lo","CIA-B timer A lo"},
    {0xBFD500, "ciab_ta_hi","CIA-B timer A hi"},
    {0xBFDC00, "ciab_sdr",  "CIA-B serial data"},
    {0xBFDD00, "ciab_icr",  "CIA-B IRQ control / read"},
    {0xBFDE00, "ciab_cra",  "CIA-B control reg A"},
    {0xBFDF00, "ciab_crb",  "CIA-B control reg B"},
};

static const char *symbol_for_addr(uae_u32 addr) {
    for (size_t i = 0; i < sizeof SYMBOLS / sizeof SYMBOLS[0]; i++) {
        if (SYMBOLS[i].addr == addr) return SYMBOLS[i].name;
    }
    return NULL;
}

static void ep_sym_lookup(int fd, const char *qs) {
    char *as = get_query_param(qs, "addr");
    if (!as) { err_response(fd, 400, "missing addr"); return; }
    char addr_buf[64];
    snprintf(addr_buf, sizeof addr_buf, "%s", as);
    int ok = 0;
    uae_u32 addr = parse_uint(addr_buf, &ok);
    if (!ok) { err_response(fd, 400, "bad addr"); return; }
    const char *name = NULL;
    const char *desc = NULL;
    for (size_t i = 0; i < sizeof SYMBOLS / sizeof SYMBOLS[0]; i++) {
        if (SYMBOLS[i].addr == addr) {
            name = SYMBOLS[i].name;
            desc = SYMBOLS[i].desc;
            break;
        }
    }
    char body[512];
    if (name) {
        snprintf(body, sizeof body,
            "{\"ok\":true,\"addr\":\"0x%08x\",\"name\":\"%s\",\"desc\":\"%s\"}\n",
            (unsigned)addr, name, desc ? desc : "");
    } else {
        snprintf(body, sizeof body,
            "{\"ok\":true,\"addr\":\"0x%08x\",\"name\":null}\n",
            (unsigned)addr);
    }
    send_response(fd, 200, body);
}

/* ----- AmigaOS Function Descriptor (.fd) lookup -----
 *
 * AmigaOS libraries are called via a fixed offset from the library base
 * register (conventionally A6 for exec, A4 in some BCPL code, etc.):
 *
 *   MOVEA.L  $4.W, A6        ; A6 = ExecBase
 *   JSR      -132(A6)        ; FindResident()
 *
 * The .fd file format maps these negative offsets to function names.
 * We embed the exec.library FD table here as a static array (the most
 * commonly referenced library on the Amiga); other libraries can be
 * loaded at runtime via POST /v1/fd/load.
 *
 * Each function's offset is its NEGATIVE distance from the library
 * base.  In exec, Supervisor is at -30, ExitIntr at -36, ... — the
 * bias starts at 30 and the offset increments by 6 per function.       */

struct fd_entry { int offset; const char *name; const char *args; };

/* exec.library FD (Kickstart 1.3, exec V34).  ~80 most-used functions.
 * Offsets are NEGATIVE (i.e. JSR -30(A6) is Supervisor). */
static const struct fd_entry EXEC_FD[] = {
    {-30,  "Supervisor",        "userFunction"},
    {-36,  "ExitIntr",          ""},
    {-42,  "Schedule",          ""},
    {-48,  "Reschedule",        ""},
    {-54,  "Switch",            ""},
    {-60,  "Dispatch",          ""},
    {-66,  "Exception",         ""},
    {-72,  "InitCode",          "startClass, version"},
    {-78,  "InitStruct",        "initTable, memory, size"},
    {-84,  "MakeLibrary",       "funcInit, structInit, libInit, dataSize, segList"},
    {-90,  "MakeFunctions",     "target, functionArray, funcDispBase"},
    {-96,  "FindResident",      "name"},
    {-102, "InitResident",      "resident, segList"},
    {-108, "Alert",             "alertNum"},
    {-114, "Debug",             "flags"},
    {-120, "Disable",           ""},
    {-126, "Enable",            ""},
    {-132, "Forbid",            ""},
    {-138, "Permit",            ""},
    {-144, "SetSR",             "newSR, mask"},
    {-150, "SuperState",        ""},
    {-156, "UserState",         "sysStack"},
    {-162, "SetIntVector",      "intNumber, interrupt"},
    {-168, "AddIntServer",      "intNumber, interrupt"},
    {-174, "RemIntServer",      "intNumber, interrupt"},
    {-180, "Cause",             "interrupt"},
    {-186, "Allocate",          "freeList, byteSize"},
    {-192, "Deallocate",        "freeList, memoryBlock, byteSize"},
    {-198, "AllocMem",          "byteSize, requirements"},
    {-204, "AllocAbs",          "byteSize, location"},
    {-210, "FreeMem",           "memoryBlock, byteSize"},
    {-216, "AvailMem",          "requirements"},
    {-222, "AllocEntry",        "entry"},
    {-228, "FreeEntry",         "entry"},
    {-234, "Insert",            "list, node, pred"},
    {-240, "AddHead",           "list, node"},
    {-246, "AddTail",           "list, node"},
    {-252, "Remove",            "node"},
    {-258, "RemHead",           "list"},
    {-264, "RemTail",           "list"},
    {-270, "Enqueue",           "list, node"},
    {-276, "FindName",          "list, name"},
    {-282, "AddTask",           "task, initialPC, finalPC"},
    {-288, "RemTask",           "task"},
    {-294, "FindTask",          "name"},
    {-300, "SetTaskPri",        "task, priority"},
    {-306, "SetSignal",         "newSignals, signalSet"},
    {-312, "SetExcept",         "newSignals, signalSet"},
    {-318, "Wait",              "signalSet"},
    {-324, "Signal",            "task, signalSet"},
    {-330, "AllocSignal",       "signalNum"},
    {-336, "FreeSignal",        "signalNum"},
    {-342, "AllocTrap",         "trapNum"},
    {-348, "FreeTrap",          "trapNum"},
    {-354, "AddPort",           "port"},
    {-360, "RemPort",           "port"},
    {-366, "PutMsg",            "port, message"},
    {-372, "GetMsg",            "port"},
    {-378, "ReplyMsg",          "message"},
    {-384, "WaitPort",          "port"},
    {-390, "FindPort",          "name"},
    {-396, "AddLibrary",        "library"},
    {-402, "RemLibrary",        "library"},
    {-408, "OldOpenLibrary",    "libName"},
    {-414, "CloseLibrary",      "library"},
    {-420, "SetFunction",       "library, funcOffset, newFunction"},
    {-426, "SumLibrary",        "library"},
    {-432, "AddDevice",         "device"},
    {-438, "RemDevice",         "device"},
    {-444, "OpenDevice",        "devName, unit, ioRequest, flags"},
    {-450, "CloseDevice",       "ioRequest"},
    {-456, "DoIO",              "ioRequest"},
    {-462, "SendIO",            "ioRequest"},
    {-468, "CheckIO",           "ioRequest"},
    {-474, "WaitIO",            "ioRequest"},
    {-480, "AbortIO",           "ioRequest"},
    {-486, "AddResource",       "resource"},
    {-492, "RemResource",       "resource"},
    {-498, "OpenResource",      "resName"},
    {-552, "RawDoFmt",          "formatString, dataStream, putChProc, putChData"},
    {-558, "GetCC",             ""},
    {-564, "TypeOfMem",         "address"},
    {-570, "Procure",           "semaport, bidMsg"},
    {-576, "Vacate",            "semaport"},
    {-582, "OpenLibrary",       "libName, version"},
    /* V36+ semaphore functions land at -588 onward in newer kickstarts */
    {-588, "InitSemaphore",     "sigSem"},
    {-594, "ObtainSemaphore",   "sigSem"},
    {-600, "ReleaseSemaphore",  "sigSem"},
    {-606, "AttemptSemaphore",  "sigSem"},
    {-624, "FindSemaphore",     "name"},
    {-630, "AddSemaphore",      "sigSem"},
    {-636, "RemSemaphore",      "sigSem"},
    {-648, "AddMemList",        "size, attributes, pri, base, name"},
    {-624, "CopyMem",           "source, dest, size"},
    {-630, "CopyMemQuick",      "source, dest, size"},
};

static const char *exec_fd_lookup(int neg_offset) {
    for (size_t i = 0; i < sizeof EXEC_FD / sizeof EXEC_FD[0]; i++) {
        if (EXEC_FD[i].offset == neg_offset) return EXEC_FD[i].name;
    }
    return NULL;
}

static void ep_fd_exec(int fd) {
    static char body[8192];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"library\":\"exec\",\"functions\":[");
    for (size_t i = 0; i < sizeof EXEC_FD / sizeof EXEC_FD[0]; i++) {
        n += snprintf(body + n, sizeof body - n,
            "%s{\"offset\":%d,\"name\":\"%s\",\"args\":\"%s\"}",
            i ? "," : "",
            EXEC_FD[i].offset,
            EXEC_FD[i].name,
            EXEC_FD[i].args);
        if (n >= (int)sizeof body - 256) break;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    send_response(fd, 200, body);
}

static void ep_fd_lookup(int fd, const char *qs) {
    char *os = get_query_param(qs, "offset");
    if (!os) { err_response(fd, 400, "missing offset"); return; }
    char obuf[32];
    snprintf(obuf, sizeof obuf, "%s", os);
    /* Offsets are typically given as negative; accept both -132 and 132. */
    int off = atoi(obuf);
    if (off > 0) off = -off;
    const char *name = exec_fd_lookup(off);
    char body[256];
    if (name) {
        snprintf(body, sizeof body,
            "{\"ok\":true,\"library\":\"exec\",\"offset\":%d,\"name\":\"%s\"}\n",
            off, name);
    } else {
        snprintf(body, sizeof body,
            "{\"ok\":true,\"library\":\"exec\",\"offset\":%d,\"name\":null}\n",
            off);
    }
    send_response(fd, 200, body);
}

static void ep_sym_list(int fd) {
    static char body[16384];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"symbols\":[");
    for (size_t i = 0; i < sizeof SYMBOLS / sizeof SYMBOLS[0]; i++) {
        n += snprintf(body + n, sizeof body - n,
            "%s{\"addr\":\"0x%08x\",\"name\":\"%s\",\"desc\":\"%s\"}",
            i ? "," : "",
            (unsigned)SYMBOLS[i].addr,
            SYMBOLS[i].name,
            SYMBOLS[i].desc);
        if (n >= (int)sizeof body - 256) break;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    send_response(fd, 200, body);
}

/* ===== WebSocket event stream =====
 *
 * On WS upgrade the connection becomes a push channel.  A background
 * pulse thread polls debugger_active + memwatch_triggered every 50 ms;
 * when state changes, a JSON frame is pushed to all connected clients.
 *
 * Frame shape:
 *   {"event":"paused"  , "pc":"0x..", "reason":"user|wp|bp|step"}
 *   {"event":"running" }
 *   {"event":"wp_hit"  , "addr":"0x..", "pc":"0x..", "value":"0x.."}
 *
 * We keep a tiny array of connected client fds (max 8) protected by a
 * mutex.  No backpressure handling — slow clients get their frames
 * dropped silently. */

#include <pthread.h>

#define WS_MAX_CLIENTS 8
static int ws_clients[WS_MAX_CLIENTS];
static pthread_mutex_t ws_lock = PTHREAD_MUTEX_INITIALIZER;
static int ws_pulse_started = 0;

/* ----- Base64 + SHA-1 for the WebSocket handshake key ----- */

static const char *B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const uint8_t *in, int n, char *out) {
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t b = ((uint32_t)in[i] << 16);
        if (i + 1 < n) b |= ((uint32_t)in[i + 1] << 8);
        if (i + 2 < n) b |= ((uint32_t)in[i + 2]);
        out[o++] = B64[(b >> 18) & 0x3F];
        out[o++] = B64[(b >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? B64[(b >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? B64[b & 0x3F]       : '=';
    }
    out[o] = 0;
}

/* Minimal SHA-1.  Tiny self-contained — only used for the WebSocket
 * accept-key computation, never in any hot path. */
static void sha1_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}
static uint32_t sha1_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1(const uint8_t *msg, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t padded = ((len + 9 + 63) / 64) * 64;
    uint8_t *buf = (uint8_t *)calloc(1, padded);
    memcpy(buf, msg, len);
    buf[len] = 0x80;
    uint64_t bitlen = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) buf[padded - 1 - i] = (uint8_t)(bitlen >> (i * 8));
    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)buf[off + i*4] << 24)
                 | ((uint32_t)buf[off + i*4 + 1] << 16)
                 | ((uint32_t)buf[off + i*4 + 2] << 8)
                 | ((uint32_t)buf[off + i*4 + 3]);
        }
        for (int i = 16; i < 80; i++)
            w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;             k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;             k = 0xCA62C1D6; }
            uint32_t t = sha1_rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = sha1_rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(buf);
    for (int i = 0; i < 5; i++) sha1_be32(out + i*4, h[i]);
}

/* Returns -1 if the send hits a broken pipe (so callers can evict the
 * client).  We use MSG_NOSIGNAL on Linux; macOS sets SO_NOSIGPIPE per
 * socket and treats EPIPE as a normal errno.  Either way SIGPIPE never
 * kills the process. */
#ifndef MSG_NOSIGNAL
# define MSG_NOSIGNAL 0
#endif

static int ws_send_frame(int fd, const char *msg) {
    size_t n = strlen(msg);
    uint8_t hdr[10];
    int hlen;
    hdr[0] = 0x81;  /* FIN + text */
    if (n < 126) {
        hdr[1] = (uint8_t)n;
        hlen = 2;
    } else if (n < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(n >> 8);
        hdr[3] = (uint8_t)n;
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++) hdr[2 + i] = (uint8_t)(n >> ((7 - i) * 8));
        hlen = 10;
    }
    if (send(fd, hdr, hlen, MSG_NOSIGNAL) <= 0) return -1;
    if (send(fd, msg, n, MSG_NOSIGNAL) <= 0) return -1;
    return 0;
}

static void ws_broadcast(const char *msg) {
    pthread_mutex_lock(&ws_lock);
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (ws_clients[i] >= 0) {
            if (ws_send_frame(ws_clients[i], msg) < 0) {
                close(ws_clients[i]);
                ws_clients[i] = -1;
            }
        }
    }
    pthread_mutex_unlock(&ws_lock);
}

static void *ws_pulse_thread(void *unused) {
    (void)unused;
    int last_active = -1;
    int last_trig = -1;
    for (;;) {
        usleep(50000);  /* 50 ms */
        if (debugger_active != last_active) {
            char buf[256];
            if (debugger_active) {
                const char *reason = "user";
                if (memwatch_triggered) reason = "wp";
                else if (skipaddr_doskip > 0) reason = "step";
                snprintf(buf, sizeof buf,
                    "{\"event\":\"paused\",\"pc\":\"0x%08x\",\"reason\":\"%s\"}\n",
                    (unsigned)m68k_getpc(), reason);
            } else {
                snprintf(buf, sizeof buf, "{\"event\":\"running\"}\n");
            }
            ws_broadcast(buf);
            last_active = debugger_active;
        }
        if (memwatch_triggered != last_trig && memwatch_triggered > 0) {
            char buf[512];
            snprintf(buf, sizeof buf,
                "{\"event\":\"wp_hit\",\"slot\":%d,\"addr\":\"0x%08x\","
                "\"pc\":\"0x%08x\",\"value\":\"0x%08x\"}\n",
                memwatch_triggered - 1,
                (unsigned)mwhit.addr, (unsigned)mwhit.pc, (unsigned)mwhit.val);
            ws_broadcast(buf);
            last_trig = memwatch_triggered;
        }
    }
    return NULL;
}

static void ws_add_client(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    pthread_mutex_lock(&ws_lock);
    int slot = -1;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (ws_clients[i] < 0) { ws_clients[i] = fd; slot = i; break; }
    }
    pthread_mutex_unlock(&ws_lock);
    if (slot < 0) close(fd);
    /* Start the pulse thread on first client.  Idempotent. */
    if (!ws_pulse_started) {
        ws_pulse_started = 1;
        pthread_t t;
        pthread_create(&t, NULL, ws_pulse_thread, NULL);
        pthread_detach(t);
    }
}

/* ----- /v1/events upgrade handler -----
 * Looks for Sec-WebSocket-Key in the raw request, computes the magic
 * SHA-1 + base64 of (key + WS magic GUID), sends the upgrade response,
 * then registers the fd as a push client. */

static void ep_events_upgrade(int fd, const char *raw) {
    const char *k = strstr(raw, "Sec-WebSocket-Key:");
    if (!k) { err_response(fd, 400, "missing ws key"); return; }
    k += 18;
    while (*k == ' ' || *k == '\t') k++;
    char key[128];
    int kn = 0;
    while (*k && *k != '\r' && *k != '\n' && kn < (int)sizeof key - 1)
        key[kn++] = *k++;
    key[kn] = 0;
    /* Concatenate with the WebSocket GUID, SHA-1, base64. */
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char concat[256];
    snprintf(concat, sizeof concat, "%s%s", key, GUID);
    uint8_t hash[20];
    sha1((const uint8_t *)concat, strlen(concat), hash);
    char accept_b64[40];
    b64_encode(hash, 20, accept_b64);
    char hdr[512];
    snprintf(hdr, sizeof hdr,
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "\r\n", accept_b64);
    send_str(fd, hdr);
    ws_add_client(fd);
    /* Send a hello frame immediately so the client knows the channel is live. */
    char hello[128];
    snprintf(hello, sizeof hello,
        "{\"event\":\"hello\",\"service\":\"fs-uae-rpc v1\",\"state\":\"%s\"}\n",
        debugger_active ? "paused" : "running");
    ws_send_frame(fd, hello);
}

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
    /* Bigger buffer than original — needed for WebSocket upgrade headers
     * which include a Sec-WebSocket-Key plus various browser baggage. */
    char buf[4096];
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
    /* WebSocket upgrade: GET /v1/events with Upgrade: websocket */
    if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/events") == 0 &&
        strstr(buf, "Upgrade: websocket")) {
        ep_events_upgrade(fd, buf);
        /* Don't close fd — it's the push channel now. */
        return;
    }
    /* Split path / query. */
    char *qs = strchr(path, '?');
    if (qs) { *qs = '\0'; qs++; }

    /* CORS preflight handling — any OPTIONS request returns 200 with
     * the CORS headers already in send_response_ct.                    */
    if (strcmp(method, "OPTIONS") == 0) {
        send_response_ct(fd, 200, "", 0, "text/plain");
        close(fd);
        return;
    }

    /* Dispatch. */
    if ((strcmp(method, "GET") == 0) &&
        (strcmp(path, "/") == 0 || strcmp(path, "/v1/ui") == 0)) {
        ep_ui(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/ping") == 0) {
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
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/watchpoints/last") == 0) {
        ep_wp_last(fd);
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
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/symbols") == 0) {
        ep_sym_list(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/symbols/lookup") == 0) {
        ep_sym_lookup(fd, qs);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/fd/exec") == 0) {
        ep_fd_exec(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/fd/lookup") == 0) {
        ep_fd_lookup(fd, qs);
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
    /* Ignore SIGPIPE — broken client connections would otherwise kill
     * the entire emulator process.  We rely on send() returning EPIPE. */
    signal(SIGPIPE, SIG_IGN);

    /* Init the websocket-client slot array — -1 means "free". */
    for (int i = 0; i < WS_MAX_CLIENTS; i++) ws_clients[i] = -1;

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
