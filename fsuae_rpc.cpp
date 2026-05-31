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
 * v1 endpoints:
 *     GET  /v1/ping                        smoke test — returns {"ok":true}
 *     GET  /v1/cpu                         CPU registers + PC + SR
 *     GET  /v1/mem?addr=HEX&len=N          read N bytes from addr, hex string
 *     POST /v1/pause                       activate_debugger (stop the emu)
 *     POST /v1/resume                      deactivate_debugger (resume)
 *     POST /v1/state/save?path=ABS_PATH    save state to absolute path
 *
 * Conventions:
 *     - addr/len query params accept decimal or 0x-prefixed hex (or $-hex)
 *     - all responses are JSON: {"ok":true,...} or {"ok":false,"err":"msg"}
 *     - "Connection: close" — one request per TCP connection (simple v1)
 *
 * Not in v1 (deferred — see ROADMAP.md):
 *     - WebSocket event stream (notify on breakpoint hit)
 *     - PC breakpoints with auto-pause
 *     - Single-step
 *     - Memory write
 *     - Register write
 *     - State load
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

static void ep_resume(int fd) {
    deactivate_debugger();
    send_response(fd, 200, "{\"ok\":true,\"state\":\"running\"}\n");
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
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/cpu") == 0) {
        ep_cpu(fd);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/v1/mem") == 0) {
        ep_mem(fd, qs);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/pause") == 0) {
        ep_pause(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/resume") == 0) {
        ep_resume(fd);
    } else if (strcmp(method, "POST") == 0 && strcmp(path, "/v1/state/save") == 0) {
        ep_state_save(fd, qs);
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
}

#endif /* !_WIN32 */
