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
 *     FSUAE_RPC_PORT=8765 fs-uae <config>            (HTTP + WS + web UI)
 *     FSUAE_GDB_PORT=2331 fs-uae <config>            (GDB remote stub)
 *
 * The two ports are independent — enable either, both, or neither.  If
 * neither env var is set, this module is a no-op.  All listeners bind
 * 127.0.0.1 only (no remote network access).
 *
 * The GDB stub speaks the Remote Serial Protocol and advertises itself
 * as an m68k big-endian target via qXfer:features:read:target.xml, so:
 *
 *     (gdb) target remote :2331
 *
 * "Just works" with no `set architecture` or `set endian` needed.  Stop
 * events are pushed by the pulse thread the moment debugger_active flips
 * — no client-side polling.
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
 *     GET  /v1/fd/lookup?offset=N&library=NAME
 *                                          lookup function by -offset (default lib=exec)
 *     GET  /v1/fd/libraries                list loaded FD libraries
 *     GET  /v1/fd/list?library=NAME        full function table for a loaded library
 *     POST /v1/fd/load?path=ABS&library=NAME
 *                                          parse an .fd file at runtime and
 *                                          register its function table under
 *                                          the given library name
 *
 * v1 endpoints — memory map + stack:
 *     GET  /v1/memmap                      region descriptors (chip/slow/fast/
 *                                          ROM/IO/unmapped) by walking mem_banks
 *     GET  /v1/stack?depth=N               return N words from A7 with heuristic
 *                                          classification (likely-code / data)
 *
 * v1 endpoints — breakpoints & watchpoints:
 *     POST /v1/breakpoints?addr=HEX&skip=N&oneshot=0|1&trace=0|1&out=PATH
 *                                          install PC breakpoint, auto-pause on hit
 *                                          skip=N → silently ignore the first N hits
 *                                          oneshot=1 → auto-clear after first fire
 *                                          trace=1 → silent capture-and-resume (NO
 *                                            auto-pause; logs {pc, D0, A0, SP,
 *                                            mem[SP]} per hit to the trace file).
 *                                            Use for high-throughput in-emulator
 *                                            instrumentation like "trace every
 *                                            exec.Allocate call".  Zero cost when
 *                                            no trace BPs are installed.
 *                                          out=PATH → open trace output file
 *     GET  /v1/breakpoints                 list active breakpoints with hit counts
 *     POST /v1/breakpoints/clear           remove all breakpoints
 *     GET  /v1/trace                       trace output state (path, open?, count)
 *     POST /v1/trace?path=PATH             open/replace trace output file
 *     POST /v1/trace                       close trace output file
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
 * v1 endpoints — stepping (extended):
 *     POST /v1/step?n=N                    execute N instructions then pause
 *     POST /v1/step?mode=over              run until PC reaches the instruction
 *                                          AFTER the current one (steps over JSR/BSR
 *                                          by installing a one-shot BP at PC+insn_len)
 *     POST /v1/step?mode=out               run until PC returns to caller (reads
 *                                          long at (A7), installs one-shot BP there)
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

/* For the memory-map endpoint we walk the live bank table.  mem_banks[]
 * is a hardware-mapped array (size MEMORY_BANKS) of addrbank* — each
 * entry covers 64KB of address space.  dummy_bank is the unmapped marker.
 * currprefs.address_space_24 tells us whether the chipset is configured
 * for the original 24-bit 68000 address space (256 banks) or the full
 * 32-bit space (65536 banks).
 *
 * Note: addrbank and uae_prefs are typedefs (no `struct` keyword needed). */
extern addrbank *mem_banks[];
extern addrbank dummy_bank;
extern struct uae_prefs currprefs;

/* Forward-declared sizes so the disasm annotator (defined long before the
 * FD library section) can declare a fixed-size library-name buffer. */
#define FD_LIB_NAME_LEN     32

/* Embedded single-page web debugger.  Generated from web/index.html.   */
#include "web_index.inc"

/* The patch un-statics these so we can drive memwatch installation
 * and single-step without re-entering the in-process debugger command parser.
 * Defined in debug.cpp (C++ linkage); declared here to match. */
void memwatch_setup(void);
void initialize_memwatch(int mode);
extern int skipaddr_doskip;
extern int no_trace_exceptions;

/* Forward decls — defined later in this file; used by the disasm
 * annotator before their full definitions. */
static const char *exec_fd_lookup(int neg_offset);
static const char *fd_lookup_any(int neg_offset, const char *prefer,
                                  const char **lib_out);
/* Defined in the GDB stub section; called from ws_pulse_thread to wake
 * gdb clients blocked on continue/step. */
static void gdb_signal_event(void);

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>

/* ===== Platform compatibility shim =====
 *
 * The rest of this file is written against POSIX (pthreads + BSD
 * sockets).  On Windows we map the small surface area we use onto
 * the native equivalents: Winsock2, _beginthreadex, SRWLOCK,
 * CONDITION_VARIABLE, Sleep().  The code below is then identical
 * across platforms.
 *
 * Build-time requirement on Windows: link against ws2_32.lib.
 */
#ifdef _WIN32
# define WIN32_LEAN_AND_MEAN
# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>
# include <process.h>

/* Winsock declares SSIZE_T (signed size_t); alias to ssize_t for the
 * rest of the file. */
typedef SSIZE_T ssize_t;
typedef int     socklen_t;

# define close(fd)         closesocket(fd)
# define usleep(us)        Sleep((DWORD)((us) / 1000))
# define strcasecmp        _stricmp
# define strncasecmp       _strnicmp
# define MSG_NOSIGNAL      0
# ifndef SHUT_RDWR
#  define SHUT_RDWR        SD_BOTH
# endif

/* ---- pthread-style shims using Win32 primitives ----
 *
 * SRWLOCK + CONDITION_VARIABLE both have static initializers, so the
 * existing `PTHREAD_MUTEX_INITIALIZER` / `PTHREAD_COND_INITIALIZER`
 * patterns translate one-to-one without runtime init calls.
 */

typedef HANDLE              pthread_t;
typedef SRWLOCK             pthread_mutex_t;
typedef CONDITION_VARIABLE  pthread_cond_t;

# define PTHREAD_MUTEX_INITIALIZER   SRWLOCK_INIT
# define PTHREAD_COND_INITIALIZER    CONDITION_VARIABLE_INIT

static inline int pthread_mutex_lock(pthread_mutex_t *m) {
    AcquireSRWLockExclusive(m); return 0;
}
static inline int pthread_mutex_unlock(pthread_mutex_t *m) {
    ReleaseSRWLockExclusive(m); return 0;
}
static inline int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    SleepConditionVariableSRW(c, m, INFINITE, 0); return 0;
}
static inline int pthread_cond_broadcast(pthread_cond_t *c) {
    WakeAllConditionVariable(c); return 0;
}

/* _beginthreadex expects `unsigned __stdcall(void*)` while pthread fns
 * are `void*(void*)`.  Trampoline through a heap struct to bridge the
 * calling conventions. */
struct fsuae_thread_arg {
    void *(*fn)(void *);
    void *arg;
};
static unsigned __stdcall fsuae_thread_trampoline(void *p) {
    struct fsuae_thread_arg *t = (struct fsuae_thread_arg *)p;
    void *(*fn)(void *) = t->fn;
    void *arg = t->arg;
    free(t);
    fn(arg);
    return 0;
}
static inline int pthread_create(pthread_t *th, void *attr,
                                  void *(*fn)(void *), void *arg) {
    (void)attr;
    struct fsuae_thread_arg *t =
        (struct fsuae_thread_arg *)malloc(sizeof *t);
    if (!t) return -1;
    t->fn = fn; t->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, fsuae_thread_trampoline,
                                  t, 0, NULL);
    if (!h) { free(t); return -1; }
    *th = (HANDLE)h;
    return 0;
}
static inline int pthread_detach(pthread_t th) {
    CloseHandle(th); return 0;
}

#else  /* POSIX */
# include <pthread.h>
# include <unistd.h>
# include <signal.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <arpa/inet.h>
#endif

extern "C" void fsuae_rpc_init(void);

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

/* External — defined in debug.cpp.  These globals control single-stepping. */
extern int debugging;

/* Arm SPCFLAG_BRK + debugging + do_skip so the CPU loop starts calling
 * debug() and checking BPs.  See debug.cpp for the helper definition.
 * Without re-arming after /v1/resume, BPs sit dormant because
 * deactivate_debugger() clears `debugging`. */
extern "C" void fsuae_rpc_arm_bp_skip (void);

static void ep_resume(int fd) {
    rearm_watchpoints_if_any();
    deactivate_debugger();
    /* deactivate_debugger() clears `debugging`, which gates the CPU
     * loop's debug() invocation.  If any BPs are installed (especially
     * silent trace BPs), re-arm so SPCFLAG_BRK -> debug() -> BP-check
     * keeps firing.  Without this, BPs sit dormant after resume. */
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (bpnodes[i].enabled) { fsuae_rpc_arm_bp_skip(); break; }
    }
    send_response(fd, 200, "{\"ok\":true,\"state\":\"running\"}\n");
}

/* ===== Per-breakpoint metadata (skip + oneshot) =====
 *
 * FS-UAE's `struct breakpoint_node` only has {addr, enabled} — no hit count,
 * no skip behaviour.  We maintain parallel arrays for the extra state.
 *
 *   bp_skip[i]      — number of remaining hits to silently ignore
 *   bp_hit_count[i] — cumulative times this BP has fired
 *   bp_oneshot[i]   — if 1, clear this BP after first fire
 *
 * The pulse thread reads these on every paused-state transition and decides
 * whether to silently auto-resume (skip>0 or transitional) or surface the
 * paused event to clients. */
static int bp_skip[BREAKPOINT_TOTAL];
static int bp_hit_count[BREAKPOINT_TOTAL];
static int bp_oneshot[BREAKPOINT_TOTAL];

/* ===== Trace-BP state =====
 *
 * A trace BP fires inside the CPU loop (in debug.cpp's BP-match block),
 * captures a fixed register snapshot to a file, and silently auto-resumes
 * — no debugger_active set, no client poll, no 50 ms ws_pulse_thread
 * latency.  Used for high-throughput in-emulator instrumentation like
 * "trace every exec.Allocate call".
 *
 *   bp_trace[i]            non-zero if slot i is a trace BP
 *   bp_trace_count_total   total trace-BPs installed (for the fast-exit
 *                          check in fsuae_rpc_trace_bp_hook)
 *   bp_trace_fp            single shared output file (line-buffered).
 *                          NULL when no trace BPs configured.
 *
 * Zero-cost-when-disabled invariant: when bp_trace_count_total == 0 the
 * hook returns 0 after a single load+compare.  No file I/O, no register
 * reads, no allocations. */
static int bp_trace[BREAKPOINT_TOTAL];
static int bp_trace_count_total = 0;
static FILE *bp_trace_fp = NULL;
static char bp_trace_path[1024] = {0};
static pthread_mutex_t bp_trace_lock = PTHREAD_MUTEX_INITIALIZER;

/* Called from debug.cpp's BP-match block on every breakpoint hit.
 *
 * Returns 1 if the BP was handled as a silent trace capture (the caller
 * should treat the BP as not-fired and resume immediately).  Returns 0
 * to let the normal BP machinery take over (auto-pause, surface to
 * clients, possibly step-over).
 *
 * Zero-cost-when-disabled: a single load+compare on `bp_trace_count_total`
 * is all that runs when no trace BPs are installed.  The mutex is only
 * taken on actual trace-hits. */
extern "C" int fsuae_rpc_trace_bp_hook (int slot, uae_u32 pc) {
    if (bp_trace_count_total == 0) return 0;  /* fast exit */
    if (slot < 0 || slot >= BREAKPOINT_TOTAL) return 0;
    if (!bp_trace[slot]) return 0;
    pthread_mutex_lock(&bp_trace_lock);
    if (!bp_trace_fp) {
        pthread_mutex_unlock(&bp_trace_lock);
        return 0;  /* trace requested but no output file open */
    }
    bp_hit_count[slot]++;
    int hit = bp_hit_count[slot];
    /* Capture D0 (size), A0/A2/A3/A6 (memheader + commonly-used
     * pointers in OS routines), A7 (caller SP), and a small stack
     * window so the caller PC chain is visible.  exec.Allocate's
     * calling convention: D0 = byte count, A0 = MemHeader.  When the
     * BP is at Allocate's entry ($FC16D8), MOVEM hasn't run yet so
     * stack[0] = direct caller (typically AllocMem at $FC17FC).
     * stack[1] is one word further, often the higher-level caller. */
    uae_u32 d0  = (uae_u32)regs.regs[0];
    uae_u32 a0  = (uae_u32)regs.regs[8];
    uae_u32 a2  = (uae_u32)regs.regs[10];
    uae_u32 a3  = (uae_u32)regs.regs[11];
    uae_u32 a6  = (uae_u32)regs.regs[14];
    uae_u32 a7  = (uae_u32)regs.regs[15];
    uae_u32 ret0 = (uae_u32)get_long_debug(a7);
    uae_u32 ret1 = (uae_u32)get_long_debug(a7 + 4);
    uae_u32 ret2 = (uae_u32)get_long_debug(a7 + 8);
    uae_u32 ret3 = (uae_u32)get_long_debug(a7 + 12);
    fprintf(bp_trace_fp,
        "[BP-TRACE] slot=%d hit=%d pc=$%08X D0=%d $%08X A0=$%08X "
        "A2=$%08X A3=$%08X A6=$%08X SP=$%08X "
        "ret=$%08X s1=$%08X s2=$%08X s3=$%08X\n",
        slot, hit, (unsigned)pc, (int)(int32_t)d0, (unsigned)d0,
        (unsigned)a0, (unsigned)a2, (unsigned)a3, (unsigned)a6,
        (unsigned)a7,
        (unsigned)ret0, (unsigned)ret1, (unsigned)ret2, (unsigned)ret3);
    /* line-buffered fopen("w") => fflush after each '\n' already; no
     * explicit fflush needed.  Mutex held to serialise multi-thread
     * writers (only the CPU thread should write today, but keep it
     * safe). */
    pthread_mutex_unlock(&bp_trace_lock);
    return 1;
}

/* Open or replace the trace output file.  path == NULL => close & clear. */
static int set_trace_path(const char *path) {
    pthread_mutex_lock(&bp_trace_lock);
    if (bp_trace_fp) { fclose(bp_trace_fp); bp_trace_fp = NULL; }
    bp_trace_path[0] = 0;
    if (path && *path) {
        bp_trace_fp = fopen(path, "w");
        if (!bp_trace_fp) { pthread_mutex_unlock(&bp_trace_lock); return -1; }
        setvbuf(bp_trace_fp, NULL, _IOLBF, 0);  /* line buffered */
        snprintf(bp_trace_path, sizeof bp_trace_path, "%s", path);
    }
    pthread_mutex_unlock(&bp_trace_lock);
    return 0;
}

/* Install a one-shot BP at `addr` and resume.  Returns slot on success,
 * -1 on no-free-slot.  Used by step-over / step-out. */
static int install_oneshot_bp(uae_u32 addr) {
    int slot = -1;
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (!bpnodes[i].enabled) { slot = i; break; }
    }
    if (slot < 0) return -1;
    bpnodes[slot].addr = addr;
    bpnodes[slot].enabled = 1;
    bp_skip[slot] = 0;
    bp_hit_count[slot] = 0;
    bp_oneshot[slot] = 1;
    return slot;
}

static void ep_step(int fd, const char *qs) {
    /* Execute N instructions then re-pause.  Maps to FS-UAE's 't' trace
     * command internals: set skipaddr_doskip to step count, set
     * exception_debugging, set SPCFLAG_BRK.
     *
     * We must clear debugger_active (so the debugger's inner stdin-read
     * loop returns and the CPU resumes) WITHOUT clearing `debugging` —
     * the CPU loop only re-enters debug() when `debugging` is true, and
     * that re-entry is what makes single-stepping work.
     *
     * Modes (mode=over / mode=out) bypass the step counter entirely and
     * install a one-shot BP at the expected next-PC, then resume. */

    char *mode = get_query_param(qs, "mode");
    char mode_buf[16] = "";
    if (mode) snprintf(mode_buf, sizeof mode_buf, "%s", mode);

    if (mode_buf[0] && !strcmp(mode_buf, "over")) {
        /* Step over: install one-shot BP at PC + current insn length.
         * Use m68k_disasm_2 with count=1 to get the next-PC for free. */
        uae_u32 pc = (uae_u32)m68k_getpc();
        TCHAR tmp[MAX_LINEWIDTH + 4];
        uaecptr nextpc = 0;
        m68k_disasm_2(tmp, sizeof tmp, pc, &nextpc, 1, NULL, NULL, 1);
        if (nextpc == 0 || nextpc == pc) {
            err_response(fd, 500, "disasm could not advance PC");
            return;
        }
        int slot = install_oneshot_bp((uae_u32)nextpc);
        if (slot < 0) { err_response(fd, 500, "no free bp slot for step-over"); return; }
        rearm_watchpoints_if_any();
        deactivate_debugger();
        char body[256];
        snprintf(body, sizeof body,
            "{\"ok\":true,\"mode\":\"over\",\"oneshot_at\":\"0x%08x\",\"slot\":%d}\n",
            (unsigned)nextpc, slot);
        send_response(fd, 200, body);
        return;
    }

    if (mode_buf[0] && !strcmp(mode_buf, "out")) {
        /* Step out: read the long at (A7) — for ordinary JSR/BSR call
         * conventions this is the return address.  Install a one-shot BP
         * there and resume.  Caveats: doesn't work for RTE, doesn't work
         * if the callee has manipulated the stack, doesn't work for
         * tail-called code.  Best-effort. */
        uae_u32 sp = (uae_u32)regs.regs[8 + 7];  /* A7 */
        uae_u32 ret = (uae_u32)get_long_debug(sp);
        if (ret == 0 || (ret & 1)) {
            char body[256];
            snprintf(body, sizeof body,
                "{\"ok\":false,\"err\":\"unlikely return PC at (A7)\","
                "\"sp\":\"0x%08x\",\"candidate\":\"0x%08x\"}\n",
                (unsigned)sp, (unsigned)ret);
            send_response(fd, 400, body);
            return;
        }
        int slot = install_oneshot_bp(ret);
        if (slot < 0) { err_response(fd, 500, "no free bp slot for step-out"); return; }
        rearm_watchpoints_if_any();
        deactivate_debugger();
        char body[256];
        snprintf(body, sizeof body,
            "{\"ok\":true,\"mode\":\"out\",\"oneshot_at\":\"0x%08x\",\"sp\":\"0x%08x\",\"slot\":%d}\n",
            (unsigned)ret, (unsigned)sp, slot);
        send_response(fd, 200, body);
        return;
    }

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
            "%s{\"slot\":%d,\"addr\":\"0x%08x\","
             "\"skip_remaining\":%d,\"hit_count\":%d,\"oneshot\":%d,\"trace\":%d}",
            first ? "" : ",", i, (unsigned)bpnodes[i].addr,
            bp_skip[i], bp_hit_count[i], bp_oneshot[i], bp_trace[i]);
        first = 0;
    }
    n += snprintf(body + n, sizeof body - n,
        "],\"trace_path\":\"%s\",\"trace_open\":%s}\n",
        bp_trace_path, bp_trace_fp ? "true" : "false");
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
    /* Optional: skip=N silently ignores the first N hits.  Useful for
     * heavily-used routines like CopyMem where you want the Nth call. */
    int skip = 0;
    char *ss = get_query_param(qs, "skip");
    if (ss) {
        char sbuf[16];
        snprintf(sbuf, sizeof sbuf, "%s", ss);
        int sok = 0;
        skip = (int)parse_uint(sbuf, &sok);
        if (!sok || skip < 0) { err_response(fd, 400, "bad skip"); return; }
    }
    /* Optional: oneshot=1 — auto-clear this BP the first time it fires.
     * Used internally by step-over and step-out. */
    int oneshot = 0;
    char *os = get_query_param(qs, "oneshot");
    if (os && (os[0]=='1' || os[0]=='y' || os[0]=='Y')) oneshot = 1;
    /* Optional: trace=1 — silent capture-and-resume mode.  When set, BP
     * hits don't pause the emulator; instead, a snapshot {D0, A0, SP,
     * mem[SP]} is logged to the trace file and the CPU continues.  See
     * fsuae_rpc_trace_bp_hook above.  Requires an output file via the
     * `out=PATH` query param OR a prior POST /v1/trace?path=PATH. */
    int trace = 0;
    char *ts = get_query_param(qs, "trace");
    if (ts && (ts[0]=='1' || ts[0]=='y' || ts[0]=='Y')) trace = 1;
    char *out = get_query_param(qs, "out");
    if (out && *out) {
        char out_buf[1024];
        snprintf(out_buf, sizeof out_buf, "%s", out);
        if (set_trace_path(out_buf) < 0) {
            err_response(fd, 500, "could not open trace output file");
            return;
        }
    }
    if (trace && !bp_trace_fp) {
        err_response(fd, 400,
            "trace=1 requires an open trace file (pass out=PATH on this "
            "request or call POST /v1/trace?path=PATH first)");
        return;
    }
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
    bp_skip[slot] = skip;
    bp_hit_count[slot] = 0;
    bp_oneshot[slot] = oneshot;
    if (trace && !bp_trace[slot]) bp_trace_count_total++;
    bp_trace[slot] = trace;
    /* Arm the CPU's debug() loop so SPCFLAG_BRK fires and BPs get
     * checked.  The in-process debugger handles this when the user
     * types `g`; via RPC we set it directly. */
    fsuae_rpc_arm_bp_skip();
    char body[512];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"slot\":%d,\"addr\":\"0x%08x\","
        "\"skip\":%d,\"oneshot\":%d,\"trace\":%d,\"trace_path\":\"%s\"}\n",
        slot, (unsigned)addr, skip, oneshot, trace, bp_trace_path);
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
        bp_skip[i] = 0;
        bp_hit_count[i] = 0;
        bp_oneshot[i] = 0;
        bp_trace[i] = 0;
    }
    bp_trace_count_total = 0;
    char body[128];
    snprintf(body, sizeof body, "{\"ok\":true,\"cleared\":%d}\n", cleared);
    send_response(fd, 200, body);
}

/* POST /v1/trace?path=ABS    open/replace the trace output file
 * POST /v1/trace             close it (passing no path)
 * GET  /v1/trace             return current state (path, count of trace BPs) */
static void ep_trace(int fd, const char *method, const char *qs) {
    if (strcmp(method, "GET") == 0) {
        char body[1280];
        snprintf(body, sizeof body,
            "{\"ok\":true,\"path\":\"%s\",\"open\":%s,"
            "\"trace_bp_count\":%d}\n",
            bp_trace_path, bp_trace_fp ? "true" : "false",
            bp_trace_count_total);
        send_response(fd, 200, body);
        return;
    }
    char *p = get_query_param(qs, "path");
    char path_buf[1024] = {0};
    if (p) snprintf(path_buf, sizeof path_buf, "%s", p);
    if (set_trace_path(p ? path_buf : NULL) < 0) {
        err_response(fd, 500, "could not open path");
        return;
    }
    char body[1280];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"path\":\"%s\",\"open\":%s}\n",
        bp_trace_path, bp_trace_fp ? "true" : "false");
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
     * library function name (if known).  This is heuristic — it blindly
     * assumes A6 points at a library base; without A6-tracking we can't
     * know which library so we scan every loaded FD table for a match.
     *
     * Caller can hint via &library=NAME — that library is checked first,
     * so when multiple libs share an offset (rare) the hint wins. */
    int annotate = 0;
    char *anns = get_query_param(qs, "annotate");
    if (!anns) anns = get_query_param(qs, "annot");
    if (anns && (anns[0] == '1' || anns[0] == 'y' || anns[0] == 'Y'))
        annotate = 1;
    char *libs = get_query_param(qs, "library");
    char prefer_lib[FD_LIB_NAME_LEN] = "exec";
    if (libs && *libs) snprintf(prefer_lib, sizeof prefer_lib, "%s", libs);

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
                    if (v < 0) {
                        const char *src_lib = prefer_lib;
                        fn = fd_lookup_any(v, prefer_lib, &src_lib);
                        if (fn) {
                            n += snprintf(body + n, sizeof body - n,
                                "       ; %s.%s()", src_lib, fn);
                            fn = NULL;  /* already emitted */
                        }
                    }
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

/* ----- Memory map -----
 *
 * Walks the live FS-UAE bank table (mem_banks[]) and emits one JSON record
 * per contiguous run of identical-bank pointers.  Each bank in mem_banks[]
 * covers 64KB; consecutive entries pointing at the same addrbank are coalesced.
 *
 * For each region we surface:
 *   start, end           — inclusive byte addresses
 *   name, label          — bank's human-readable id (from FS-UAE addrbank def)
 *   kind                 — derived from addrbank.flags (chip-ram / rom / io / etc.)
 *   size                 — bytes covered
 *
 * The mapping is interpreted, not a raw dump.  ABFLAG_NONE banks (unmapped)
 * are emitted with kind="unmapped" so frontends can render the gaps.  */

static const char *bank_kind(int flags) {
    /* From uae/memory.h:
     *   ABFLAG_RAM=1 ABFLAG_ROM=2 ABFLAG_ROMIN=4 ABFLAG_IO=8 ABFLAG_NONE=16
     *   ABFLAG_CHIPRAM=2048 ABFLAG_CIA=4096
     * Priority order matches what frontends usually want to render. */
    if (flags & 16)         return "unmapped";   /* ABFLAG_NONE */
    if (flags & 4096)       return "cia";        /* ABFLAG_CIA */
    if (flags & 2048)       return "chipram";    /* ABFLAG_CHIPRAM */
    if (flags & 8)          return "io";         /* ABFLAG_IO */
    if (flags & 2)          return "rom";        /* ABFLAG_ROM */
    if (flags & 4)          return "romin";      /* ABFLAG_ROMIN (writable shadow) */
    if (flags & 1)          return "ram";        /* ABFLAG_RAM */
    return "unknown";
}

static void ep_memmap(int fd) {
    /* Use the cpu config to decide how many banks to iterate.  24-bit
     * mode mirrors $00xxxxxx into the upper banks, so iterating 256
     * entries is enough; 32-bit mode covers the full 65536. */
    int total = currprefs.address_space_24 ? 256 : 65536;

    static char body[131072];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"address_space_bits\":%d,\"regions\":[",
                     currprefs.address_space_24 ? 24 : 32);
    int first = 1;
    int i = 0;
    while (i < total && n < (int)sizeof body - 512) {
        addrbank *b = mem_banks[i];
        int j = i + 1;
        while (j < total && mem_banks[j] == b) j++;
        /* Avoid spamming output with the long unmapped tails — collapsed
         * but still surfaced so callers see the gaps. */
        const char *name = "(null)";
        const char *label = "(null)";
        int flags = 0;
        if (b) {
            /* TCHAR ≡ char on POSIX builds; safe to treat as C-string. */
            if (b->name)  name  = (const char *)b->name;
            if (b->label) label = (const char *)b->label;
            flags = b->flags;
        }
        const char *kind = bank_kind(flags);
        uae_u32 start = (uae_u32)i << 16;
        uae_u32 end   = (((uae_u32)j << 16) - 1);
        n += snprintf(body + n, sizeof body - n,
            "%s{\"start\":\"0x%08x\",\"end\":\"0x%08x\","
            "\"size\":%u,\"name\":\"%s\",\"label\":\"%s\","
            "\"kind\":\"%s\",\"flags\":\"0x%x\"}",
            first ? "" : ",",
            (unsigned)start, (unsigned)end,
            (unsigned)((j - i) << 16),
            name, label, kind, (unsigned)flags);
        first = 0;
        i = j;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    send_response(fd, 200, body);
}

/* ----- Stack walker -----
 *
 * No frame format is enforced on m68k, so reliable stack walking would
 * require either DWARF unwind tables or strict frame-pointer conventions.
 * Instead we surface the raw stack words and heuristically tag each one
 * as "code" or "data":
 *
 *   - Even, in a bank with flags & (ABFLAG_RAM|ABFLAG_ROM|ABFLAG_CHIPRAM)
 *     → likely a saved PC (return address pushed by JSR/BSR).
 *   - Other → likely data (saved register, local, etc.).
 *
 * The caller can then take the "likely code" addresses, disassemble
 * backwards, and assemble a call chain. */

static int addr_looks_like_code(uae_u32 a) {
    if (a & 1) return 0;
    if (a < 0x400) return 0;  /* exception vectors / zero page */
    int bank = (a >> 16) & 0xFFFF;
    int total = currprefs.address_space_24 ? 256 : 65536;
    if (bank >= total) return 0;
    addrbank *b = mem_banks[bank];
    if (!b) return 0;
    int f = b->flags;
    if (f & 16) return 0;        /* ABFLAG_NONE */
    /* Allow RAM (1), ROM (2), ROMIN (4), CHIPRAM (2048) */
    return (f & (1 | 2 | 4 | 2048)) != 0;
}

static void ep_stack(int fd, const char *qs) {
    int depth = 32;
    char *ds = get_query_param(qs, "depth");
    if (ds) {
        char dbuf[16];
        snprintf(dbuf, sizeof dbuf, "%s", ds);
        int ok = 0;
        depth = (int)parse_uint(dbuf, &ok);
        if (!ok || depth < 1 || depth > 1024) {
            err_response(fd, 400, "bad depth (1..1024)");
            return;
        }
    }
    uae_u32 sp = (uae_u32)regs.regs[8 + 7];  /* A7 */

    static char body[65536];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"sp\":\"0x%08x\",\"depth\":%d,\"words\":[", (unsigned)sp, depth);
    int first = 1;
    for (int i = 0; i < depth && n < (int)sizeof body - 256; i++) {
        uae_u32 a = sp + (uae_u32)(i * 4);
        uae_u32 v = (uae_u32)get_long_debug(a);
        int code = addr_looks_like_code(v);
        n += snprintf(body + n, sizeof body - n,
            "%s{\"offset\":%d,\"addr\":\"0x%08x\","
            "\"value\":\"0x%08x\",\"kind\":\"%s\"}",
            first ? "" : ",", i * 4,
            (unsigned)a, (unsigned)v,
            code ? "code" : "data");
        first = 0;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
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

/* ----- Multi-library FD registry -----
 *
 * The built-in EXEC_FD[] above is registered as library "exec" at startup.
 * Additional .fd files (graphics.fd, intuition.fd, dos.fd, ...) can be
 * parsed at runtime via POST /v1/fd/load and added to fd_libs[].
 *
 * Parser handles the standard .fd format used by Commodore + the modern
 * AmigaOS NDK distribution:
 *
 *     * comment line
 *     ##base _GfxBase
 *     ##bias 30
 *     ##public
 *     BltClear(memBlock,byteCount,flags)(a1,d0,d1)
 *     ##private
 *     ...
 *     ##end
 *
 * Bias starts positive (typically 30) and increments by 6 per function.
 * We store offsets as NEGATIVE (so -30, -36, ...) to match the convention
 * used by the disasm annotator. */

#define FD_MAX_LIBS         16
/* FD_LIB_NAME_LEN is defined near the top of the file (forward decl). */

struct fd_library {
    char name[FD_LIB_NAME_LEN];
    const struct fd_entry *entries;   /* points at static array OR malloc'd */
    int count;
    int builtin;                      /* 1 = built-in (don't free entries / names) */
};

static struct fd_library fd_libs[FD_MAX_LIBS];
static int fd_lib_count = 0;

static struct fd_library *fd_lib_find(const char *name) {
    if (!name || !*name) return NULL;
    for (int i = 0; i < fd_lib_count; i++) {
        if (strcasecmp(fd_libs[i].name, name) == 0) return &fd_libs[i];
    }
    return NULL;
}

static void fd_register_builtin(const char *name, const struct fd_entry *entries, int count) {
    if (fd_lib_count >= FD_MAX_LIBS) return;
    struct fd_library *lib = &fd_libs[fd_lib_count++];
    snprintf(lib->name, sizeof lib->name, "%s", name);
    lib->entries = entries;
    lib->count = count;
    lib->builtin = 1;
}

static const char *fd_lookup_in(const char *lib_name, int neg_offset) {
    struct fd_library *lib = fd_lib_find(lib_name);
    if (!lib) return NULL;
    for (int i = 0; i < lib->count; i++) {
        if (lib->entries[i].offset == neg_offset) return lib->entries[i].name;
    }
    return NULL;
}

/* Annotator helper: scan all loaded libraries for an offset match, with
 * `prefer` as a tiebreaker.  Returns "libname.FuncName" in a static buf,
 * or NULL on no match.  Called by the disasm post-processor — at most
 * once per disasm line, so the static-buffer pattern is safe. */
static const char *fd_lookup_any(int neg_offset, const char *prefer,
                                  const char **lib_out) {
    static char qual[96];
    /* Preferred library first */
    if (prefer && *prefer) {
        const char *nm = fd_lookup_in(prefer, neg_offset);
        if (nm) {
            snprintf(qual, sizeof qual, "%s", nm);
            if (lib_out) *lib_out = prefer;
            return qual;
        }
    }
    for (int i = 0; i < fd_lib_count; i++) {
        for (int j = 0; j < fd_libs[i].count; j++) {
            if (fd_libs[i].entries[j].offset == neg_offset) {
                snprintf(qual, sizeof qual, "%s", fd_libs[i].entries[j].name);
                if (lib_out) *lib_out = fd_libs[i].name;
                return qual;
            }
        }
    }
    return NULL;
}

/* Back-compat: the original disasm annotator called exec_fd_lookup.
 * Keep that signature working by routing through the new registry. */
static const char *exec_fd_lookup(int neg_offset) {
    return fd_lookup_in("exec", neg_offset);
}

/* Parse a .fd file from disk and register it as a new library.  Returns
 * the number of functions parsed (>=0) or a negative error code:
 *   -1 = couldn't open file
 *   -2 = parse error / no functions found
 *   -3 = registry full */
static int fd_parse_file(const char *path, const char *lib_name) {
    if (fd_lib_count >= FD_MAX_LIBS) return -3;
    /* Replace existing entry with the same name (so /v1/fd/load can be
     * used to refresh a table during development). */
    struct fd_library *existing = fd_lib_find(lib_name);
    if (existing && !existing->builtin) {
        /* Free per-entry name buffers (each was malloc'd) then the array. */
        for (int i = 0; i < existing->count; i++) {
            free((char *)existing->entries[i].name);
        }
        free((struct fd_entry *)existing->entries);
        existing->entries = NULL;
        existing->count = 0;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    int bias = 30;
    int cap = 64, n = 0;
    struct fd_entry *arr = (struct fd_entry *)calloc(cap, sizeof *arr);
    if (!arr) { fclose(fp); return -2; }
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        /* Strip CR/LF and leading WS */
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strlen(s);
        while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (!*s) continue;
        if (s[0] == '*') continue;            /* comment */
        if (strncmp(s, "##end", 5) == 0) break;
        if (strncmp(s, "##bias", 6) == 0) {
            bias = atoi(s + 6);
            if (bias < 0) bias = -bias;
            continue;
        }
        if (s[0] == '#' && s[1] == '#') continue;  /* ##base / ##public / ##private */
        /* A function line: "Name(args)(regs)" — we only need the name. */
        char *paren = strchr(s, '(');
        if (!paren) continue;
        if (paren == s) continue;             /* malformed */
        if (n >= cap) {
            cap *= 2;
            struct fd_entry *bigger = (struct fd_entry *)realloc(arr, cap * sizeof *arr);
            if (!bigger) break;
            arr = bigger;
        }
        int nlen = (int)(paren - s);
        if (nlen > 63) nlen = 63;
        char *namebuf = (char *)malloc((size_t)nlen + 1);
        if (!namebuf) break;
        memcpy(namebuf, s, (size_t)nlen);
        namebuf[nlen] = 0;
        arr[n].offset = -bias;
        arr[n].name   = namebuf;
        arr[n].args   = "";   /* not preserved — keeps the parser simple */
        n++;
        bias += 6;
    }
    fclose(fp);
    if (n == 0) { free(arr); return -2; }
    if (existing) {
        existing->entries = arr;
        existing->count = n;
        existing->builtin = 0;
        return n;
    }
    if (fd_lib_count >= FD_MAX_LIBS) { free(arr); return -3; }
    struct fd_library *lib = &fd_libs[fd_lib_count++];
    snprintf(lib->name, sizeof lib->name, "%s", lib_name);
    lib->entries = arr;
    lib->count = n;
    lib->builtin = 0;
    return n;
}

static void ep_fd_load(int fd, const char *qs) {
    char *ps = get_query_param(qs, "path");
    if (!ps) { err_response(fd, 400, "missing path"); return; }
    char path_buf[1024];
    snprintf(path_buf, sizeof path_buf, "%s", ps);
    char *ls = get_query_param(qs, "library");
    if (!ls || !*ls) { err_response(fd, 400, "missing library"); return; }
    char lib_buf[FD_LIB_NAME_LEN];
    snprintf(lib_buf, sizeof lib_buf, "%s", ls);
    int rc = fd_parse_file(path_buf, lib_buf);
    if (rc == -1) { err_response(fd, 400, "cannot open path"); return; }
    if (rc == -2) { err_response(fd, 400, "fd parse failed"); return; }
    if (rc == -3) { err_response(fd, 500, "fd registry full"); return; }
    char body[256];
    snprintf(body, sizeof body,
        "{\"ok\":true,\"library\":\"%s\",\"path\":\"%s\",\"functions\":%d}\n",
        lib_buf, path_buf, rc);
    send_response(fd, 200, body);
}

static void ep_fd_libraries(int fd) {
    char body[2048];
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"libraries\":[");
    for (int i = 0; i < fd_lib_count; i++) {
        n += snprintf(body + n, sizeof body - n,
            "%s{\"name\":\"%s\",\"functions\":%d,\"builtin\":%s}",
            i ? "," : "", fd_libs[i].name, fd_libs[i].count,
            fd_libs[i].builtin ? "true" : "false");
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    send_response(fd, 200, body);
}

static void ep_fd_list(int fd, const char *qs) {
    char *ls = get_query_param(qs, "library");
    const char *lib_name = ls && *ls ? ls : "exec";
    char lib_buf[FD_LIB_NAME_LEN];
    snprintf(lib_buf, sizeof lib_buf, "%s", lib_name);
    struct fd_library *lib = fd_lib_find(lib_buf);
    if (!lib) { err_response(fd, 404, "unknown library"); return; }
    static char body[65536];
    int n = snprintf(body, sizeof body,
        "{\"ok\":true,\"library\":\"%s\",\"functions\":[", lib->name);
    for (int i = 0; i < lib->count; i++) {
        n += snprintf(body + n, sizeof body - n,
            "%s{\"offset\":%d,\"name\":\"%s\",\"args\":\"%s\"}",
            i ? "," : "", lib->entries[i].offset,
            lib->entries[i].name, lib->entries[i].args ? lib->entries[i].args : "");
        if (n >= (int)sizeof body - 256) break;
    }
    n += snprintf(body + n, sizeof body - n, "]}\n");
    send_response(fd, 200, body);
}

/* Kept for compat: same as ep_fd_list with library=exec. */
static void ep_fd_exec(int fd) {
    static char body[8192];
    struct fd_library *lib = fd_lib_find("exec");
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"library\":\"exec\",\"functions\":[");
    if (lib) {
        for (int i = 0; i < lib->count; i++) {
            n += snprintf(body + n, sizeof body - n,
                "%s{\"offset\":%d,\"name\":\"%s\",\"args\":\"%s\"}",
                i ? "," : "",
                lib->entries[i].offset, lib->entries[i].name,
                lib->entries[i].args ? lib->entries[i].args : "");
            if (n >= (int)sizeof body - 256) break;
        }
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
    char *ls = get_query_param(qs, "library");
    const char *lib_name = "exec";
    char lib_buf[FD_LIB_NAME_LEN];
    if (ls && *ls) {
        snprintf(lib_buf, sizeof lib_buf, "%s", ls);
        lib_name = lib_buf;
    }
    const char *src_lib = lib_name;
    const char *name = fd_lookup_any(off, lib_name, &src_lib);
    char body[256];
    if (name) {
        snprintf(body, sizeof body,
            "{\"ok\":true,\"library\":\"%s\",\"offset\":%d,\"name\":\"%s\"}\n",
            src_lib, off, name);
    } else {
        snprintf(body, sizeof body,
            "{\"ok\":true,\"library\":\"%s\",\"offset\":%d,\"name\":null}\n",
            lib_name, off);
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

/* pthread / Win32 shim is set up at the top of the file. */

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

/* On a paused-transition where the cause was a PC breakpoint, check our
 * skip / oneshot bookkeeping.  Returns:
 *   1 — handled silently (auto-resumed; caller should NOT broadcast paused)
 *   0 — no BP match or BP fired for real (caller broadcasts paused as usual)
 *
 * Called from ws_pulse_thread while debugger_active is non-zero. */
static int handle_bp_pause(uae_u32 pc, int *bp_slot_out, int *bp_hits_out) {
    int slot = -1;
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (bpnodes[i].enabled && bpnodes[i].addr == pc) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return 0;     /* no BP hit — must have been step/wp/user */
    bp_hit_count[slot]++;
    if (bp_slot_out) *bp_slot_out = slot;
    if (bp_hits_out) *bp_hits_out = bp_hit_count[slot];
    if (bp_skip[slot] > 0) {
        /* Not the target hit yet — silently resume. */
        bp_skip[slot]--;
        rearm_watchpoints_if_any();
        deactivate_debugger();
        return 1;
    }
    if (bp_oneshot[slot]) {
        /* Surface a normal paused event but disable the BP so it won't
         * fire again.  Useful for step-over / step-out which want exactly
         * one stop. */
        bpnodes[slot].enabled = 0;
        bpnodes[slot].addr = 0;
        bp_oneshot[slot] = 0;
    }
    return 0;
}

static void *ws_pulse_thread(void *unused) {
    (void)unused;
    int last_active = -1;
    int last_trig = -1;
    for (;;) {
        usleep(50000);  /* 50 ms */
        if (debugger_active != last_active) {
            char buf[512];
            if (debugger_active) {
                uae_u32 pc = (uae_u32)m68k_getpc();
                int slot = -1, hits = 0;
                if (handle_bp_pause(pc, &slot, &hits)) {
                    /* Skipped silently; debugger_active was cleared.
                     * Re-snapshot for the next loop iteration. */
                    last_active = debugger_active;
                    continue;
                }
                const char *reason = "user";
                if (memwatch_triggered) reason = "wp";
                else if (slot >= 0) reason = "bp";
                else if (skipaddr_doskip > 0) reason = "step";
                if (slot >= 0) {
                    snprintf(buf, sizeof buf,
                        "{\"event\":\"paused\",\"pc\":\"0x%08x\","
                        "\"reason\":\"%s\",\"bp_slot\":%d,\"bp_hits\":%d}\n",
                        (unsigned)pc, reason, slot, hits);
                } else {
                    snprintf(buf, sizeof buf,
                        "{\"event\":\"paused\",\"pc\":\"0x%08x\",\"reason\":\"%s\"}\n",
                        (unsigned)pc, reason);
                }
            } else {
                snprintf(buf, sizeof buf, "{\"event\":\"running\"}\n");
            }
            ws_broadcast(buf);
            /* Wake any gdb worker blocked in continue/step.  Cheap even
             * when no gdb client is connected. */
            gdb_signal_event();
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

/* ===== GDB Remote Serial Protocol stub =====
 *
 * Exposes the emulator as a GDB remote target on a separate TCP port
 * (FSUAE_GDB_PORT).  The target description is autoconfigured (m68k +
 * big-endian) so the user just types `target remote :PORT` — no manual
 * `set architecture` / `set endian` required.
 *
 * Implemented packets:
 *
 *   ?                       — last stop reason (returns T05 SIGTRAP)
 *   g  / G                  — read / write all 18 m68k registers
 *   p NN / P NN=V           — read / write one register
 *   m A,L / M A,L:D         — read / write memory
 *   c / s                   — continue / step
 *   vCont? / vCont;c|s      — same, in verbose form
 *   Z0..Z4 / z0..z4         — install / remove BP (0,1) or WP (2,3,4)
 *   qSupported              — capabilities
 *   qXfer:features:read:target.xml — target description (architecture + regs)
 *   qAttached / qC          — already-attached / current thread
 *   qfThreadInfo / qsThreadInfo — single-thread list ("m1" then "l")
 *   qSymbol / qTStatus / vMustReplyEmpty — minimal stubs
 *   H g0 / H c -1           — set thread for ops (no-op, returns OK)
 *   D / k                   — detach / kill (closes the session)
 *
 * Threading:
 *   gdb_listener_thread     — accept() loop, spawns one worker per client
 *   gdb_client_thread       — per-connection RSP packet loop
 *   ws_pulse_thread         — already monitors debugger_active; we hook in
 *                             gdb_signal_event() so blocked continue/step
 *                             calls wake up the instant the emulator pauses
 *
 * Concurrency model matches the rest of this file: per-client thread reads
 * regs / memory / bpnodes directly, relying on the emulator being paused
 * for stable reads — which it always is between gdb packets.
 */

#define GDB_MAX_CLIENTS  4
#define GDB_RX_BUF       8192
#define GDB_TX_BUF       8192

static int gdb_clients[GDB_MAX_CLIENTS];
static pthread_mutex_t gdb_clients_lock = PTHREAD_MUTEX_INITIALIZER;

/* Stop-event cond — signalled by the pulse thread whenever
 * debugger_active changes.  gdb_wait_for_pause() blocks on this so a
 * worker thread that just issued `c` / `s` parks until the emulator
 * actually re-pauses (BP, WP, step complete, user pause, etc.). */
static pthread_mutex_t gdb_event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  gdb_event_cond = PTHREAD_COND_INITIALIZER;
static int gdb_event_seq = 0;

/* Embedded target description.  GDB requests this on connect via
 * qXfer:features:read:target.xml — once it has this, both architecture
 * and endianness are autoconfigured.  Without it, the user has to type
 * `set architecture m68k` + `set endian big` before `target remote`. */
static const char GDB_TARGET_XML[] = R"GDBXML(<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
  <architecture>m68k:68000</architecture>
  <feature name="org.gnu.gdb.m68k.core">
    <reg name="d0"  bitsize="32" type="int32"/>
    <reg name="d1"  bitsize="32" type="int32"/>
    <reg name="d2"  bitsize="32" type="int32"/>
    <reg name="d3"  bitsize="32" type="int32"/>
    <reg name="d4"  bitsize="32" type="int32"/>
    <reg name="d5"  bitsize="32" type="int32"/>
    <reg name="d6"  bitsize="32" type="int32"/>
    <reg name="d7"  bitsize="32" type="int32"/>
    <reg name="a0"  bitsize="32" type="data_ptr"/>
    <reg name="a1"  bitsize="32" type="data_ptr"/>
    <reg name="a2"  bitsize="32" type="data_ptr"/>
    <reg name="a3"  bitsize="32" type="data_ptr"/>
    <reg name="a4"  bitsize="32" type="data_ptr"/>
    <reg name="a5"  bitsize="32" type="data_ptr"/>
    <reg name="fp"  bitsize="32" type="data_ptr"/>
    <reg name="sp"  bitsize="32" type="data_ptr"/>
    <reg name="ps"  bitsize="32" type="int32"/>
    <reg name="pc"  bitsize="32" type="code_ptr"/>
  </feature>
</target>
)GDBXML";

/* ----- RSP framing ----- */

static uint8_t rsp_checksum(const char *p, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += (uint8_t)p[i];
    return (uint8_t)(s & 0xFF);
}

static int rsp_sendall(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_NOSIGNAL);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

/* Send a $payload#cs packet and wait for an ack.  On NAK we resend up
 * to twice; otherwise we assume the link is OK and continue. */
static int rsp_send(int fd, const char *payload) {
    size_t n = strlen(payload);
    char tail[8];
    snprintf(tail, sizeof tail, "#%02x", rsp_checksum(payload, n));
    for (int attempt = 0; attempt < 3; attempt++) {
        if (rsp_sendall(fd, "$", 1) < 0) return -1;
        if (n && rsp_sendall(fd, payload, n) < 0) return -1;
        if (rsp_sendall(fd, tail, strlen(tail)) < 0) return -1;
        char ack;
        ssize_t r = recv(fd, &ack, 1, 0);
        if (r <= 0) return -1;
        if (ack == '+') return 0;
        if (ack == '-') continue;
        /* Some clients (older gdbs) skip the ack entirely.  Treat the
         * non-ack byte as the start of the next packet. */
        return 0;
    }
    return -1;
}

/* Receive one packet payload.  Skips noise between packets, sends the
 * "+" ack after a valid frame.  Returns payload length or -1 on EOF. */
static int rsp_recv(int fd, char *out, size_t cap) {
    char c;
    /* Find a $ */
    while (1) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '$') break;
    }
    size_t n = 0;
    while (1) {
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) return -1;
        if (c == '#') break;
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    /* Skip the two-char checksum.  We trust TCP for integrity. */
    char cs[2];
    if (recv(fd, cs, 2, 0) != 2) return -1;
    if (rsp_sendall(fd, "+", 1) < 0) return -1;
    return (int)n;
}

/* ----- Hex helpers ----- */

static char gdb_hex_digit(int v) {
    return (char)((v < 10) ? ('0' + v) : ('a' + v - 10));
}

static int gdb_hex_val(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void gdb_hex_u32_be(uae_u32 v, char *out) {
    for (int i = 0; i < 4; i++) {
        uae_u8 b = (uae_u8)(v >> ((3 - i) * 8));
        out[i*2 + 0] = gdb_hex_digit((b >> 4) & 0xF);
        out[i*2 + 1] = gdb_hex_digit(b & 0xF);
    }
}

/* Parse up to 8 hex chars; stops at first non-hex.  Returns count consumed. */
static int gdb_parse_hex(const char *s, uae_u32 *v) {
    uae_u32 r = 0;
    int n = 0;
    while (n < 8) {
        int d = gdb_hex_val((unsigned char)s[n]);
        if (d < 0) break;
        r = (r << 4) | (uae_u32)d;
        n++;
    }
    *v = r;
    return n;
}

/* ----- Register get/set bridges to FS-UAE internals ----- */

/* GDB register layout, matching GDB_TARGET_XML above:
 *   0..7   D0..D7
 *   8..15  A0..A7   (a6 = fp, a7 = sp)
 *   16     PS (sr)
 *   17     PC                                                                  */
static void gdb_read_all_regs(char *out144) {
    for (int i = 0; i < 8; i++)
        gdb_hex_u32_be((uae_u32)regs.regs[i], out144 + i * 8);
    for (int i = 0; i < 8; i++)
        gdb_hex_u32_be((uae_u32)regs.regs[8 + i], out144 + (8 + i) * 8);
    MakeSR();
    gdb_hex_u32_be((uae_u32)(regs.sr & 0xFFFF), out144 + 16 * 8);
    gdb_hex_u32_be((uae_u32)m68k_getpc(),       out144 + 17 * 8);
}

static void gdb_write_all_regs(const char *in144) {
    uae_u32 v;
    for (int i = 0; i < 8; i++) {
        gdb_parse_hex(in144 + i * 8, &v);
        regs.regs[i] = v;
    }
    for (int i = 0; i < 8; i++) {
        gdb_parse_hex(in144 + (8 + i) * 8, &v);
        regs.regs[8 + i] = v;
    }
    gdb_parse_hex(in144 + 16 * 8, &v);
    regs.sr = (uae_u16)(v & 0xFFFF);
    MakeFromSR();
    gdb_parse_hex(in144 + 17 * 8, &v);
    m68k_setpc(v);
}

static uae_u32 gdb_read_one_reg(int idx) {
    if (idx >= 0 && idx <= 15) return (uae_u32)regs.regs[idx];
    if (idx == 16) { MakeSR(); return (uae_u32)(regs.sr & 0xFFFF); }
    if (idx == 17) return (uae_u32)m68k_getpc();
    return 0;
}

static void gdb_write_one_reg(int idx, uae_u32 v) {
    if (idx >= 0 && idx <= 15) regs.regs[idx] = v;
    else if (idx == 16) { regs.sr = (uae_u16)(v & 0xFFFF); MakeFromSR(); }
    else if (idx == 17) m68k_setpc(v);
}

/* ----- BP / WP packet handlers ----- */

static void gdb_install_bp(uaecptr addr) {
    /* If an identical-addr BP already exists, leave it alone. */
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (bpnodes[i].enabled && bpnodes[i].addr == addr) return;
    }
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (!bpnodes[i].enabled) {
            bpnodes[i].addr = addr;
            bpnodes[i].enabled = 1;
            bp_skip[i] = 0;
            bp_hit_count[i] = 0;
            bp_oneshot[i] = 0;
            return;
        }
    }
}

static void gdb_remove_bp(uaecptr addr) {
    for (int i = 0; i < BREAKPOINT_TOTAL; i++) {
        if (bpnodes[i].enabled && bpnodes[i].addr == addr) {
            bpnodes[i].enabled = 0;
            bpnodes[i].addr = 0;
            bp_skip[i] = 0;
            bp_hit_count[i] = 0;
            bp_oneshot[i] = 0;
        }
    }
}

static void gdb_install_wp(int rwi, uaecptr addr, int size) {
    if (size <= 0) size = 1;
    if (size > 0x10000) size = 0x10000;
    initialize_memwatch(0);
    for (int i = 0; i < MEMWATCH_TOTAL; i++) {
        if (mwnodes[i].size == 0) {
            struct memwatch_node *mwn = &mwnodes[i];
            mwn->addr = addr;
            mwn->size = size;
            mwn->rwi = rwi;
            mwn->val_enabled = 0;
            mwn->val_mask = 0xFFFFFFFF;
            mwn->val = 0;
            mwn->access_mask = MW_MASK_ALL;
            mwn->reg = 0xFFFFFFFF;
            mwn->frozen = 0;
            mwn->mustchange = 0;
            mwn->modval = 0;
            mwn->modval_written = 0;
            mwn->pc = 0xFFFFFFFF;
            memwatch_setup();
            return;
        }
    }
}

static void gdb_remove_wp(int rwi, uaecptr addr, int size) {
    int any = 0;
    for (int i = 0; i < MEMWATCH_TOTAL; i++) {
        if (mwnodes[i].size != 0 && mwnodes[i].addr == addr &&
            mwnodes[i].size == size && mwnodes[i].rwi == rwi) {
            mwnodes[i].size = 0;
            any = 1;
        }
    }
    if (any) memwatch_setup();
}

/* ----- Pause-event sync ----- */

/* Called by ws_pulse_thread on every state transition.  Wakes any
 * gdb worker blocked in gdb_wait_for_pause(). */
static void gdb_signal_event(void) {
    pthread_mutex_lock(&gdb_event_lock);
    gdb_event_seq++;
    pthread_cond_broadcast(&gdb_event_cond);
    pthread_mutex_unlock(&gdb_event_lock);
}

/* Block until the emulator is paused.  Returns immediately if already
 * paused; otherwise polls `debugger_active` at 1 ms granularity.
 *
 * Why polling instead of the cond-var: a single-step cycle can complete
 * in well under the pulse-thread's 50 ms tick, leaving the pulse thread
 * with last_active=1 on both samples and no transition to broadcast.
 * A cond_var signalled from the pulse thread is therefore unreliable
 * for tight stepi loops.  Direct polling keeps the worst-case wakeup
 * latency at 1 ms and burns one CPU only while a continue/step is in
 * flight. */
static void gdb_wait_for_pause(void) {
    while (debugger_active == 0) {
        usleep(1000);
    }
}

/* Build a T05 (SIGTRAP) stop reply with SP + PC pre-cached so GDB can
 * skip an immediate follow-up `g`. */
static void gdb_make_stop_reply(char *out, size_t cap) {
    char sp_hex[9], pc_hex[9];
    gdb_hex_u32_be((uae_u32)regs.regs[8 + 7], sp_hex); sp_hex[8] = 0;
    gdb_hex_u32_be((uae_u32)m68k_getpc(),     pc_hex); pc_hex[8] = 0;
    /* Reg IDs in hex: 0f = a7/SP, 11 = PC.  Critical not to use decimal! */
    snprintf(out, cap, "T05thread:1;0f:%s;11:%s;", sp_hex, pc_hex);
}

/* ----- Packet dispatch ----- */

/* Returns 0 to keep the session alive, -1 to terminate (after sending
 * the reply).  Reply payload written into `out` (NUL-terminated). */
static int gdb_handle_packet(const char *pkt, int n, char *out, size_t cap) {
    out[0] = 0;
    if (n == 0) return 0;
    char c = pkt[0];

    /* Stop reason — used right after `target remote`. */
    if (n == 1 && c == '?') {
        if (!debugger_active) {
            activate_debugger();
            /* Give the CPU loop a moment to park in debug_1() so the
             * regs we snapshot below are stable. */
            usleep(20000);
        }
        gdb_make_stop_reply(out, cap);
        return 0;
    }
    /* Read all regs */
    if (n == 1 && c == 'g') {
        char tmp[145];
        gdb_read_all_regs(tmp);
        tmp[144] = 0;
        snprintf(out, cap, "%s", tmp);
        return 0;
    }
    /* Write all regs */
    if (c == 'G' && n >= 1 + 144) {
        gdb_write_all_regs(pkt + 1);
        snprintf(out, cap, "OK");
        return 0;
    }
    /* Read single reg: p NN */
    if (c == 'p') {
        uae_u32 idx;
        if (gdb_parse_hex(pkt + 1, &idx) > 0 && idx < 18) {
            char tmp[9];
            gdb_hex_u32_be(gdb_read_one_reg((int)idx), tmp); tmp[8] = 0;
            snprintf(out, cap, "%s", tmp);
        } else {
            snprintf(out, cap, "xxxxxxxx");
        }
        return 0;
    }
    /* Write single reg: P NN=VVVVVVVV */
    if (c == 'P') {
        uae_u32 idx, val;
        int k = gdb_parse_hex(pkt + 1, &idx);
        if (k > 0 && pkt[1 + k] == '=' &&
            gdb_parse_hex(pkt + 1 + k + 1, &val) > 0 && idx < 18) {
            gdb_write_one_reg((int)idx, val);
            snprintf(out, cap, "OK");
        } else {
            snprintf(out, cap, "E01");
        }
        return 0;
    }
    /* Read memory: m ADDR,LEN */
    if (c == 'm') {
        const char *comma = strchr(pkt + 1, ',');
        if (!comma) { snprintf(out, cap, "E01"); return 0; }
        uae_u32 addr, len;
        gdb_parse_hex(pkt + 1, &addr);
        gdb_parse_hex(comma + 1, &len);
        /* Cap to fit in the response buffer (2 hex chars per byte). */
        size_t max_bytes = (cap - 4) / 2;
        if (len > max_bytes) len = (uae_u32)max_bytes;
        size_t off = 0;
        for (uae_u32 i = 0; i < len; i++) {
            uae_u8 b = (uae_u8)(get_byte_debug(addr + i) & 0xFF);
            out[off++] = gdb_hex_digit((b >> 4) & 0xF);
            out[off++] = gdb_hex_digit(b & 0xF);
        }
        out[off] = 0;
        return 0;
    }
    /* Write memory: M ADDR,LEN:DATA */
    if (c == 'M') {
        const char *comma = strchr(pkt + 1, ',');
        const char *colon = strchr(pkt + 1, ':');
        if (!comma || !colon || colon < comma) {
            snprintf(out, cap, "E01"); return 0;
        }
        uae_u32 addr, len;
        gdb_parse_hex(pkt + 1, &addr);
        gdb_parse_hex(comma + 1, &len);
        const char *data = colon + 1;
        for (uae_u32 i = 0; i < len; i++) {
            int hi = gdb_hex_val((unsigned char)data[i * 2]);
            int lo = gdb_hex_val((unsigned char)data[i * 2 + 1]);
            if (hi < 0 || lo < 0) break;
            debug_write_memory_8(addr + i, (uae_u8)((hi << 4) | lo));
        }
        snprintf(out, cap, "OK");
        return 0;
    }
    /* Continue */
    if (n == 1 && c == 'c') {
        rearm_watchpoints_if_any();
        deactivate_debugger();
        gdb_wait_for_pause();
        gdb_make_stop_reply(out, cap);
        return 0;
    }
    /* Step one instruction */
    if (n == 1 && c == 's') {
        rearm_watchpoints_if_any();
        skipaddr_doskip = 1;
        no_trace_exceptions = 1;
        exception_debugging = 1;
        debugging = 1;
        debugger_active = 0;
        set_special(SPCFLAG_BRK);
        gdb_wait_for_pause();
        gdb_make_stop_reply(out, cap);
        return 0;
    }
    /* Breakpoints + watchpoints: Z/z TYPE,ADDR,KIND */
    if ((c == 'Z' || c == 'z') && n >= 5) {
        char type = pkt[1];
        if (pkt[2] != ',') { snprintf(out, cap, "E01"); return 0; }
        uae_u32 addr;
        int k = gdb_parse_hex(pkt + 3, &addr);
        uae_u32 kind = 1;
        if (pkt[3 + k] == ',') gdb_parse_hex(pkt + 3 + k + 1, &kind);
        int is_install = (c == 'Z');
        if (type == '0' || type == '1') {
            if (is_install) gdb_install_bp((uaecptr)addr);
            else            gdb_remove_bp((uaecptr)addr);
            snprintf(out, cap, "OK");
            return 0;
        }
        /* rwi mapping: Z2 = write (W=2), Z3 = read (R=1), Z4 = access (RW=3) */
        if (type == '2' || type == '3' || type == '4') {
            int rwi = (type == '2') ? 2 : (type == '3') ? 1 : 3;
            if (is_install) gdb_install_wp(rwi, (uaecptr)addr, (int)kind);
            else            gdb_remove_wp(rwi, (uaecptr)addr, (int)kind);
            snprintf(out, cap, "OK");
            return 0;
        }
        /* Unknown breakpoint type — empty reply tells GDB it's unsupported. */
        return 0;
    }
    /* Detach / kill */
    if (n == 1 && (c == 'D' || c == 'k')) {
        snprintf(out, cap, "OK");
        rearm_watchpoints_if_any();
        deactivate_debugger();
        return -1;
    }
    /* Query packets */
    if (c == 'q') {
        if (!strncmp(pkt, "qSupported", 10)) {
            snprintf(out, cap,
                "PacketSize=2000;swbreak+;hwbreak+;qXfer:features:read+");
            return 0;
        }
        if (!strncmp(pkt, "qXfer:features:read:target.xml:", 31)) {
            /* Args: OFFSET,LENGTH (hex).  Reply: 'm' (more) or 'l' (last)
             * followed by chunk bytes.  Our XML fits in one chunk, but
             * GDB may still issue paginated reads. */
            const char *args = pkt + 31;
            uae_u32 off, len;
            int k = gdb_parse_hex(args, &off);
            if (args[k] != ',') { snprintf(out, cap, "E00"); return 0; }
            gdb_parse_hex(args + k + 1, &len);
            size_t xml_len = sizeof(GDB_TARGET_XML) - 1;
            if (off >= xml_len) { snprintf(out, cap, "l"); return 0; }
            size_t remaining = xml_len - off;
            int more = (len < remaining);
            size_t take = more ? len : remaining;
            if (take > cap - 4) take = cap - 4;
            out[0] = more ? 'm' : 'l';
            memcpy(out + 1, GDB_TARGET_XML + off, take);
            out[1 + take] = 0;
            return 0;
        }
        if (!strncmp(pkt, "qXfer:", 6))         { snprintf(out, cap, "l");  return 0; }
        if (!strcmp(pkt, "qAttached"))          { snprintf(out, cap, "1");  return 0; }
        if (!strcmp(pkt, "qC"))                 { snprintf(out, cap, "QC1"); return 0; }
        if (!strcmp(pkt, "qfThreadInfo"))       { snprintf(out, cap, "m1"); return 0; }
        if (!strcmp(pkt, "qsThreadInfo"))       { snprintf(out, cap, "l");  return 0; }
        if (!strncmp(pkt, "qSymbol", 7))        { snprintf(out, cap, "OK"); return 0; }
        /* Unknown q* — empty = unsupported. */
        return 0;
    }
    /* v packets */
    if (c == 'v') {
        if (!strcmp(pkt, "vMustReplyEmpty")) return 0;
        if (!strcmp(pkt, "vCont?")) { snprintf(out, cap, "vCont;c;s"); return 0; }
        if (!strncmp(pkt, "vCont;c", 7)) {
            rearm_watchpoints_if_any();
            deactivate_debugger();
            gdb_wait_for_pause();
            gdb_make_stop_reply(out, cap);
            return 0;
        }
        if (!strncmp(pkt, "vCont;s", 7)) {
            rearm_watchpoints_if_any();
            skipaddr_doskip = 1;
            no_trace_exceptions = 1;
            exception_debugging = 1;
            debugging = 1;
            debugger_active = 0;
            set_special(SPCFLAG_BRK);
            gdb_wait_for_pause();
            gdb_make_stop_reply(out, cap);
            return 0;
        }
        return 0;
    }
    /* H packets — set thread for ops (we have one thread; accept all). */
    if (c == 'H') { snprintf(out, cap, "OK"); return 0; }
    /* Default: empty reply = "feature not implemented". */
    return 0;
}

/* ----- Client + listener threads ----- */

static void gdb_register_client(int fd) {
    pthread_mutex_lock(&gdb_clients_lock);
    for (int i = 0; i < GDB_MAX_CLIENTS; i++) {
        if (gdb_clients[i] < 0) { gdb_clients[i] = fd; break; }
    }
    pthread_mutex_unlock(&gdb_clients_lock);
}

static void gdb_unregister_client(int fd) {
    pthread_mutex_lock(&gdb_clients_lock);
    for (int i = 0; i < GDB_MAX_CLIENTS; i++) {
        if (gdb_clients[i] == fd) { gdb_clients[i] = -1; break; }
    }
    pthread_mutex_unlock(&gdb_clients_lock);
}

static void *gdb_client_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    gdb_register_client(fd);
    /* Pause on connect — gdb assumes a stopped target. */
    activate_debugger();
    /* Per-connection buffers on the stack — no contention between
     * concurrent clients. */
    char rx[GDB_RX_BUF];
    char tx[GDB_TX_BUF];
    for (;;) {
        int n = rsp_recv(fd, rx, sizeof rx);
        if (n < 0) break;
        int rc = gdb_handle_packet(rx, n, tx, sizeof tx);
        if (rsp_send(fd, tx) < 0) break;
        if (rc < 0) break;
    }
    close(fd);
    gdb_unregister_client(fd);
    return NULL;
}

static void *gdb_listener_thread(void *arg) {
    int port = (int)(intptr_t)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        fprintf(stderr, "[fsuae-rpc] gdb socket() failed: %s\n", strerror(errno));
        return NULL;
    }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&sa, sizeof sa) < 0) {
        fprintf(stderr, "[fsuae-rpc] gdb bind(127.0.0.1:%d) failed: %s\n",
            port, strerror(errno));
        close(srv);
        return NULL;
    }
    if (listen(srv, 4) < 0) {
        fprintf(stderr, "[fsuae-rpc] gdb listen() failed: %s\n", strerror(errno));
        close(srv);
        return NULL;
    }
    fprintf(stderr, "[fsuae-rpc] gdb stub listening on 127.0.0.1:%d (m68k be)\n",
        port);
    for (;;) {
        int cfd = accept(srv, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        pthread_t t;
        if (pthread_create(&t, NULL, gdb_client_thread,
                           (void *)(intptr_t)cfd) == 0)
            pthread_detach(t);
        else
            close(cfd);
    }
    close(srv);
    return NULL;
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
    } else if (strcmp(path, "/v1/trace") == 0 &&
               (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0)) {
        ep_trace(fd, method, qs);
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
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/fd/libraries") == 0) {
        ep_fd_libraries(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/fd/list") == 0) {
        ep_fd_list(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/fd/load") == 0) {
        ep_fd_load(fd, qs);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/memmap") == 0) {
        ep_memmap(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/stack") == 0) {
        ep_stack(fd, qs);
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
#ifdef _WIN32
    /* Winsock requires per-process init before any socket() call.
     * Idempotent across multiple WSAStartup calls (refcounted). */
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    /* Ignore SIGPIPE — broken client connections would otherwise kill
     * the entire emulator process.  We rely on send() returning EPIPE.
     * Windows has no SIGPIPE; broken sends just return SOCKET_ERROR. */
    signal(SIGPIPE, SIG_IGN);
#endif

    /* Init the websocket-client slot array — -1 means "free". */
    for (int i = 0; i < WS_MAX_CLIENTS; i++) ws_clients[i] = -1;
    /* Same for gdb-stub client slots. */
    for (int i = 0; i < GDB_MAX_CLIENTS; i++) gdb_clients[i] = -1;

    /* Register the built-in exec.library FD table as the first entry in
     * the FD library registry.  Additional libraries can be loaded at
     * runtime via POST /v1/fd/load. */
    fd_register_builtin("exec", EXEC_FD, (int)(sizeof EXEC_FD / sizeof EXEC_FD[0]));

    /* GDB stub — independent of FSUAE_RPC_PORT.  Enables the RSP
     * listener on a separate TCP port so `gdb -ex "target remote :PORT"`
     * works without an external bridge process. */
    const char *gdb_env = getenv("FSUAE_GDB_PORT");
    if (gdb_env && *gdb_env) {
        int gdb_port = atoi(gdb_env);
        if (gdb_port > 0 && gdb_port <= 65535) {
            pthread_t gtid;
            if (pthread_create(&gtid, NULL, gdb_listener_thread,
                               (void *)(intptr_t)gdb_port) == 0)
                pthread_detach(gtid);
            else
                fprintf(stderr, "[fsuae-rpc] gdb pthread_create failed: %s\n",
                    strerror(errno));
        } else {
            fprintf(stderr, "[fsuae-rpc] invalid FSUAE_GDB_PORT='%s'\n", gdb_env);
        }
    }

    const char *env = getenv("FSUAE_RPC_PORT");
    if (env && *env) {
        int port = atoi(env);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "[fsuae-rpc] invalid FSUAE_RPC_PORT='%s'\n", env);
        } else {
            pthread_t tid;
            if (pthread_create(&tid, NULL, rpc_worker,
                               (void *)(intptr_t)port) != 0) {
                fprintf(stderr, "[fsuae-rpc] pthread_create failed: %s\n",
                    strerror(errno));
            } else {
                pthread_detach(tid);
            }
        }
    }

    /* Optional: start paused so the client can install breakpoints /
     * watchpoints BEFORE the emulator executes its first instruction.
     * Useful for catching very-early-boot ROM writes (chip-RAM init,
     * IRQ vector install, etc.).  Applies to either backend (HTTP or
     * GDB) — useful for gdb users who want to set BPs before reset. */
    const char *pause = getenv("FSUAE_RPC_PAUSE_AT_BOOT");
    if (pause && (pause[0] == '1' || pause[0] == 'y' || pause[0] == 'Y')) {
        fprintf(stderr, "[fsuae-rpc] starting paused (FSUAE_RPC_PAUSE_AT_BOOT=1)\n");
        activate_debugger();
    }
}
