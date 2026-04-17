/*
 * flowto.c — "Command flow" remote-execution mode for dumbterm.
 *
 * Two roles in this file:
 *   1. AGENT (--agent PORT):  listens for RPC requests, executes them locally.
 *   2. DRIVER (--flowto ADDR): spawns child + JS shim, forwards shim's RPC calls
 *                              to an agent over TCP, multiplexes local "host" too.
 *
 * RPC protocol: newline-delimited JSON. Each line is a request or response.
 *   Request:  {"id":N,"op":"exec","cmd":"ls","cwd":"/tmp"}
 *   Response: {"id":N,"ok":true,"exit":0,"out":"<base64>","err":"<base64>"}
 *
 * Shim talks to driver over a local TCP socket on 127.0.0.1:<gateway_port>,
 * which the driver tells the child about via env DUMBTERM_GATEWAY.
 *
 * Phase 1 scope: exec + ping only. Phase 2 adds fs.*. Phase 3 adds path/env.
 */

#ifndef FLOWTO_C
#define FLOWTO_C

#include <sys/stat.h>
#include <dirent.h>
#ifndef _WIN32
#include <sys/types.h>
#endif
/* S_ISDIR missing on some MinGW headers */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif

/* ── minimal JSON parse + base64 ─────────────────────────────── */

/* base64 encode (caller frees) */
static char *b64_encode(const unsigned char *data, int len) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int olen = 4 * ((len + 2) / 3);
    char *out = (char*)malloc(olen + 1);
    int i, j = 0;
    for (i = 0; i < len; i += 3) {
        unsigned int v = ((unsigned)data[i]) << 16;
        if (i + 1 < len) v |= ((unsigned)data[i+1]) << 8;
        if (i + 2 < len) v |= (unsigned)data[i+2];
        out[j++] = T[(v >> 18) & 0x3F];
        out[j++] = T[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? T[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < len) ? T[v & 0x3F] : '=';
    }
    out[j] = 0;
    return out;
}

/* base64 decode (caller frees). *out_len gets output size. Returns NULL on error. */
static unsigned char *b64_decode(const char *s, int *out_len) {
    static const unsigned char DT[256] = {
        /* initialize with 0xFF then set valid chars inline at runtime — but for
           simplicity: compute on demand */
        0
    };
    static int inited = 0;
    static unsigned char DT2[256];
    if (!inited) {
        int i; for (i = 0; i < 256; i++) DT2[i] = 0xFF;
        const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (i = 0; T[i]; i++) DT2[(unsigned char)T[i]] = i;
        inited = 1;
    }
    int slen = strlen(s);
    /* strip trailing '=' */
    while (slen > 0 && s[slen-1] == '=') slen--;
    int olen = (slen * 3) / 4;
    unsigned char *out = (unsigned char*)malloc(olen + 4);
    int oi = 0, i;
    unsigned int v = 0; int bits = 0;
    for (i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (DT2[c] == 0xFF) { if (c == '\r' || c == '\n' || c == ' ') continue; free(out); return NULL; }
        v = (v << 6) | DT2[c];
        bits += 6;
        if (bits >= 8) { bits -= 8; out[oi++] = (v >> bits) & 0xFF; }
    }
    *out_len = oi;
    return out;
}

/* find a string field: json must contain "key" : "value" (whitespace tolerant). Returns strdup'd value or NULL. */
static char *json_str(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const char *end = p;
    /* find unescaped closing quote */
    while (*end) {
        if (*end == '\\' && end[1]) { end += 2; continue; }
        if (*end == '"') break;
        end++;
    }
    if (*end != '"') return NULL;
    int n = end - p;
    char *out = (char*)malloc(n + 1);
    /* minimal unescape: only \\ \" \n \t */
    int oi = 0;
    const char *s = p;
    while (s < end) {
        if (*s == '\\' && s[1]) {
            char c = s[1];
            if (c == 'n') out[oi++] = '\n';
            else if (c == 't') out[oi++] = '\t';
            else if (c == 'r') out[oi++] = '\r';
            else out[oi++] = c;
            s += 2;
        } else out[oi++] = *s++;
    }
    out[oi] = 0;
    return out;
}

/* find an integer field: json must contain "key" : 123 (whitespace tolerant). */
static int json_int(const char *json, const char *key, int dflt) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return dflt;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

/* JSON-escape a string into dst (caller ensures dst is big enough: 6*len + 1). */
static void json_escape(char *dst, const char *src) {
    char *d = dst;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"') { *d++='\\'; *d++='"'; }
        else if (c == '\\') { *d++='\\'; *d++='\\'; }
        else if (c == '\n') { *d++='\\'; *d++='n'; }
        else if (c == '\r') { *d++='\\'; *d++='r'; }
        else if (c == '\t') { *d++='\\'; *d++='t'; }
        else if (c < 0x20) { d += sprintf(d, "\\u%04x", c); }
        else *d++ = c;
    }
    *d = 0;
}

/* ── line-buffered JSON reader over a socket ─────────────────── */
/* Dynamic growing buffer. In-place null-terminate; caller uses immediately. */
typedef struct {
    sock_t s;
    char *buf;
    int cap;
    int used;
    int last_line_len; /* bytes returned last, to consume on next call */
} RpcConn;

static void rpc_conn_init(RpcConn *c, sock_t s) {
    c->s = s; c->cap = 65536; c->buf = (char*)malloc(c->cap); c->used = 0; c->last_line_len = 0;
}
static void rpc_conn_free(RpcConn *c) {
    if (c->buf) free(c->buf);
    c->buf = NULL; c->cap = c->used = c->last_line_len = 0;
}

/* Read one newline-terminated line. On success: null-terminate in place,
   return pointer valid until next rpc_read_line call on the same conn. */
static char *rpc_read_line(RpcConn *c, int *out_len) {
    /* consume previous line: shift bytes after "last_line_len + 1 (\n)" to start */
    if (c->last_line_len > 0) {
        int consumed = c->last_line_len + 1;
        if (c->used > consumed) {
            memmove(c->buf, c->buf + consumed, c->used - consumed);
            c->used -= consumed;
        } else c->used = 0;
        c->last_line_len = 0;
    }
    for (;;) {
        int i;
        for (i = 0; i < c->used; i++) {
            if (c->buf[i] == '\n') {
                c->buf[i] = 0;
                *out_len = i;
                c->last_line_len = i;
                return c->buf;
            }
        }
        if (c->used >= c->cap) {
            c->cap *= 2;
            c->buf = (char*)realloc(c->buf, c->cap);
        }
        int room = c->cap - c->used;
        int n = recv(c->s, c->buf + c->used, room, 0);
        if (n <= 0) { *out_len = n; return NULL; }
        c->used += n;
    }
}

/* send one JSON line (appends \n) */
static int rpc_write_line(sock_t s, const char *json) {
    int n = (int)strlen(json);
    if (send(s, json, n, 0) != n) return -1;
    if (send(s, "\n", 1, 0) != 1) return -1;
    return 0;
}

/* ── AGENT: execute requests ─────────────────────────────────── */

/* Execute cmd via /bin/sh -c (unix) or cmd.exe /c (Windows), capture stdout+stderr+exit. */
static void agent_exec(const char *cmd, const char *cwd,
                       char **out, int *out_len, char **err, int *err_len, int *exit_code) {
    *out = NULL; *out_len = 0; *err = NULL; *err_len = 0; *exit_code = -1;
#ifdef _WIN32
    /* Windows: CreateProcess with stdout+stderr redirected to pipes. TODO: merge. */
    HANDLE out_rd, out_wr, err_rd, err_wr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    CreatePipe(&out_rd, &out_wr, &sa, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&err_rd, &err_wr, &sa, 0);
    SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_wr; si.hStdError = err_wr; si.hStdInput = NULL;
    PROCESS_INFORMATION pi = {0};
    char cmdline[8192];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", cmd);
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
                             (cwd && *cwd) ? cwd : NULL, &si, &pi);
    if (!ok && cwd && *cwd) {
        /* cwd may be a foreign-platform path (e.g. Mac sending "/Users/..."
           to a Windows agent). Retry without cwd — use agent's default. */
        ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
                            NULL, &si, &pi);
    }
    CloseHandle(out_wr); CloseHandle(err_wr);
    if (!ok) { CloseHandle(out_rd); CloseHandle(err_rd); return; }
    /* slurp pipes */
    int out_cap = 4096; *out = (char*)malloc(out_cap);
    int err_cap = 4096; *err = (char*)malloc(err_cap);
    DWORD nr;
    char tmp[4096];
    while (ReadFile(out_rd, tmp, sizeof(tmp), &nr, NULL) && nr > 0) {
        if (*out_len + (int)nr > out_cap) { out_cap = (*out_len + nr) * 2; *out = (char*)realloc(*out, out_cap); }
        memcpy(*out + *out_len, tmp, nr); *out_len += nr;
    }
    while (ReadFile(err_rd, tmp, sizeof(tmp), &nr, NULL) && nr > 0) {
        if (*err_len + (int)nr > err_cap) { err_cap = (*err_len + nr) * 2; *err = (char*)realloc(*err, err_cap); }
        memcpy(*err + *err_len, tmp, nr); *err_len += nr;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
    *exit_code = (int)ec;
    CloseHandle(out_rd); CloseHandle(err_rd);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
#else
    /* POSIX: pipe + fork + exec sh -c */
    int outpipe[2], errpipe[2];
    if (pipe(outpipe) < 0) return;
    if (pipe(errpipe) < 0) { close(outpipe[0]); close(outpipe[1]); return; }
    pid_t pid = fork();
    if (pid < 0) {
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        return;
    }
    if (pid == 0) {
        /* child */
        if (cwd && *cwd) { if (chdir(cwd) != 0) { /* ignore */ } }
        dup2(outpipe[1], 1); dup2(errpipe[1], 2);
        close(outpipe[0]); close(outpipe[1]);
        close(errpipe[0]); close(errpipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    /* parent */
    close(outpipe[1]); close(errpipe[1]);
    int out_cap = 4096; *out = (char*)malloc(out_cap);
    int err_cap = 4096; *err = (char*)malloc(err_cap);
    char tmp[4096];
    int nr;
    /* drain both pipes — naive blocking read serially (fine for small outputs) */
    while ((nr = read(outpipe[0], tmp, sizeof(tmp))) > 0) {
        if (*out_len + nr > out_cap) { out_cap = (*out_len + nr) * 2; *out = (char*)realloc(*out, out_cap); }
        memcpy(*out + *out_len, tmp, nr); *out_len += nr;
    }
    while ((nr = read(errpipe[0], tmp, sizeof(tmp))) > 0) {
        if (*err_len + nr > err_cap) { err_cap = (*err_len + nr) * 2; *err = (char*)realloc(*err, err_cap); }
        memcpy(*err + *err_len, tmp, nr); *err_len += nr;
    }
    close(outpipe[0]); close(errpipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

/* Stream one message to the socket ({id, stream:"out|err", data:base64}).
   We use send() direct; line-terminated JSON so shim's reader can split. */
static void agent_stream(sock_t s, int id, const char *stream_name,
                         const unsigned char *data, int len) {
    if (len <= 0) return;
    char *b64 = b64_encode(data, len);
    int cap = strlen(b64) + 128;
    char *msg = (char*)malloc(cap);
    snprintf(msg, cap, "{\"id\":%d,\"stream\":\"%s\",\"data\":\"%s\"}", id, stream_name, b64);
    rpc_write_line(s, msg);
    free(msg); free(b64);
}

/* spawn a child and stream its stdout/stderr as JSON-line messages.
   Blocks until the child exits, then sends {id, exit:N}.
   During this call the main loop doesn't pump other requests. */
static void agent_spawn_streaming(sock_t s, int id, const char *cmd,
                                  const char *args_json, const char *cwd) {
#ifdef _WIN32
    /* Build cmdline: "cmd.exe /c <cmd> <args>". args_json is a JSON array; we
       parse minimally — for Phase 5 just pass cmd through as-is (user can
       include args in cmd string). */
    (void)args_json;
    HANDLE out_rd, out_wr, err_rd, err_wr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    CreatePipe(&out_rd, &out_wr, &sa, 0);
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    CreatePipe(&err_rd, &err_wr, &sa, 0);
    SetHandleInformation(err_rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_wr; si.hStdError = err_wr; si.hStdInput = NULL;
    PROCESS_INFORMATION pi = {0};
    char cmdline[8192];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", cmd);
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL,
                             (cwd && *cwd) ? cwd : NULL, &si, &pi);
    if (!ok && cwd && *cwd) {
        ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    CloseHandle(out_wr); CloseHandle(err_wr);
    if (!ok) {
        CloseHandle(out_rd); CloseHandle(err_rd);
        char msg[256]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"exit\":-1,\"err\":\"spawn failed\"}", id);
        rpc_write_line(s, msg); return;
    }
    /* announce pid */
    { char msg[128]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":true,\"pid\":%u}", id, (unsigned)pi.dwProcessId);
      rpc_write_line(s, msg); }
    /* poll loop: peek pipes, read + send; check if process exited */
    unsigned char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(out_rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD n = 0; if (ReadFile(out_rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &n, NULL) && n > 0)
                agent_stream(s, id, "out", buf, (int)n);
        }
        if (PeekNamedPipe(err_rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD n = 0; if (ReadFile(err_rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &n, NULL) && n > 0)
                agent_stream(s, id, "err", buf, (int)n);
        }
        DWORD wr = WaitForSingleObject(pi.hProcess, 30); /* 30ms poll interval */
        if (wr == WAIT_OBJECT_0) {
            /* drain remaining */
            for (;;) {
                avail = 0; if (!PeekNamedPipe(out_rd, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
                DWORD n = 0; if (!ReadFile(out_rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &n, NULL) || n == 0) break;
                agent_stream(s, id, "out", buf, (int)n);
            }
            for (;;) {
                avail = 0; if (!PeekNamedPipe(err_rd, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
                DWORD n = 0; if (!ReadFile(err_rd, buf, avail < sizeof(buf) ? avail : sizeof(buf), &n, NULL) || n == 0) break;
                agent_stream(s, id, "err", buf, (int)n);
            }
            break;
        }
    }
    DWORD ec = 0; GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(out_rd); CloseHandle(err_rd); CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    char msg[128]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"exit\":%d}", id, (int)ec);
    rpc_write_line(s, msg);
#else
    /* POSIX: pipes + fork + exec sh -c, then poll using select() */
    (void)args_json;
    int outpipe[2], errpipe[2];
    if (pipe(outpipe) < 0 || pipe(errpipe) < 0) {
        char msg[256]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"exit\":-1,\"err\":\"pipe failed\"}", id);
        rpc_write_line(s, msg); return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        char msg[256]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"exit\":-1,\"err\":\"fork failed\"}", id);
        rpc_write_line(s, msg); return;
    }
    if (pid == 0) {
        if (cwd && *cwd) { (void)chdir(cwd); }
        dup2(outpipe[1], 1); dup2(errpipe[1], 2);
        close(outpipe[0]); close(outpipe[1]); close(errpipe[0]); close(errpipe[1]);
        execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    close(outpipe[1]); close(errpipe[1]);
    int outfd = outpipe[0], errfd = errpipe[0];
    /* non-blocking reads */
    int fl = fcntl(outfd, F_GETFL); fcntl(outfd, F_SETFL, fl | O_NONBLOCK);
    fl = fcntl(errfd, F_GETFL); fcntl(errfd, F_SETFL, fl | O_NONBLOCK);
    { char msg[128]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":true,\"pid\":%d}", id, (int)pid);
      rpc_write_line(s, msg); }
    unsigned char buf[4096];
    int out_open = 1, err_open = 1, exited = 0, exit_code = -1;
    while (out_open || err_open || !exited) {
        fd_set rfds; FD_ZERO(&rfds);
        int maxfd = -1;
        if (out_open) { FD_SET(outfd, &rfds); if (outfd > maxfd) maxfd = outfd; }
        if (err_open) { FD_SET(errfd, &rfds); if (errfd > maxfd) maxfd = errfd; }
        struct timeval tv = {0, 50000}; /* 50ms */
        if (maxfd >= 0) select(maxfd + 1, &rfds, NULL, NULL, &tv);
        else usleep(50000);
        if (out_open && FD_ISSET(outfd, &rfds)) {
            int n = read(outfd, buf, sizeof(buf));
            if (n > 0) agent_stream(s, id, "out", buf, n);
            else if (n == 0) out_open = 0;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) out_open = 0;
        }
        if (err_open && FD_ISSET(errfd, &rfds)) {
            int n = read(errfd, buf, sizeof(buf));
            if (n > 0) agent_stream(s, id, "err", buf, n);
            else if (n == 0) err_open = 0;
            else if (errno != EAGAIN && errno != EWOULDBLOCK) err_open = 0;
        }
        if (!exited) {
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid) {
                exited = 1;
                exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
        }
    }
    close(outfd); close(errfd);
    char msg[128]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"exit\":%d}", id, exit_code);
    rpc_write_line(s, msg);
#endif
}

/* handle one RPC request on the given connection */
static void agent_handle_request(sock_t s, const char *req) {
    char *op = json_str(req, "op");
    int id = json_int(req, "id", 0);
    if (!op) { rpc_write_line(s, "{\"ok\":false,\"err\":\"no op\"}"); return; }

    if (strcmp(op, "ping") == 0) {
        char host[256] = "unknown";
        gethostname(host, sizeof(host));
        char msg[512];
        snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":true,\"host\":\"%s\"}", id, host);
        rpc_write_line(s, msg);
    } else if (strcmp(op, "host_info") == 0) {
        char hostname[256] = "unknown";
        gethostname(hostname, sizeof(hostname));
        char cwd[2048]; cwd[0] = 0;
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, ".");
#ifdef _WIN32
        const char *plat = "win32";
        const char *sep = "\\\\";
#else
        const char *plat = "darwin";
        const char *sep = "/";
#endif
        /* Build env JSON object */
        int env_cap = 16384; char *env_json = (char*)malloc(env_cap);
        int env_pos = 0;
        env_pos += snprintf(env_json + env_pos, env_cap - env_pos, "{");
#ifdef _WIN32
        LPCH env_block = GetEnvironmentStringsA();
        if (env_block) {
            int first = 1;
            for (LPCH p = env_block; *p; ) {
                if (*p != '=') {
                    const char *eq = strchr(p, '=');
                    if (eq) {
                        int kl = eq - p;
                        char key[512], val_esc[8192], key_esc[1024];
                        if (kl < (int)sizeof(key) - 1) {
                            memcpy(key, p, kl); key[kl] = 0;
                            json_escape(key_esc, key);
                            json_escape(val_esc, eq + 1);
                            int need = strlen(key_esc) + strlen(val_esc) + 16;
                            if (env_pos + need > env_cap) {
                                env_cap = (env_pos + need) * 2;
                                env_json = (char*)realloc(env_json, env_cap);
                            }
                            env_pos += snprintf(env_json + env_pos, env_cap - env_pos,
                                "%s\"%s\":\"%s\"", first ? "" : ",", key_esc, val_esc);
                            first = 0;
                        }
                    }
                }
                p += strlen(p) + 1;
            }
            FreeEnvironmentStringsA(env_block);
        }
#else
        extern char **environ;
        int first = 1;
        for (char **e = environ; *e; e++) {
            const char *eq = strchr(*e, '=');
            if (!eq) continue;
            int kl = eq - *e;
            char key[512], val_esc[8192], key_esc[1024];
            if (kl >= (int)sizeof(key)) continue;
            memcpy(key, *e, kl); key[kl] = 0;
            json_escape(key_esc, key);
            json_escape(val_esc, eq + 1);
            int need = strlen(key_esc) + strlen(val_esc) + 16;
            if (env_pos + need > env_cap) {
                env_cap = (env_pos + need) * 2;
                env_json = (char*)realloc(env_json, env_cap);
            }
            env_pos += snprintf(env_json + env_pos, env_cap - env_pos,
                "%s\"%s\":\"%s\"", first ? "" : ",", key_esc, val_esc);
            first = 0;
        }
#endif
        env_pos += snprintf(env_json + env_pos, env_cap - env_pos, "}");

        char cwd_esc[4096]; json_escape(cwd_esc, cwd);
        char host_esc[512]; json_escape(host_esc, hostname);
        int msg_cap = env_pos + 1024;
        char *msg = (char*)malloc(msg_cap);
        snprintf(msg, msg_cap,
            "{\"id\":%d,\"ok\":true,\"hostname\":\"%s\",\"cwd\":\"%s\",\"platform\":\"%s\",\"sep\":\"%s\",\"env\":%s}",
            id, host_esc, cwd_esc, plat, sep, env_json);
        rpc_write_line(s, msg);
        free(msg); free(env_json);
    } else if (strcmp(op, "spawn") == 0) {
        char *cmd = json_str(req, "cmd");
        char *cwd = json_str(req, "cwd");
        if (cmd) agent_spawn_streaming(s, id, cmd, NULL, cwd);
        else {
            char msg[128]; snprintf(msg, sizeof(msg), "{\"id\":%d,\"exit\":-1,\"err\":\"no cmd\"}", id);
            rpc_write_line(s, msg);
        }
        free(cmd); free(cwd);
    } else if (strcmp(op, "exec") == 0) {
        char *cmd = json_str(req, "cmd");
        char *cwd = json_str(req, "cwd");
        char *out = NULL, *err = NULL;
        int out_len = 0, err_len = 0, exit_code = -1;
        if (cmd) agent_exec(cmd, cwd, &out, &out_len, &err, &err_len, &exit_code);
        char *out_b64 = b64_encode((unsigned char*)(out ? out : ""), out_len);
        char *err_b64 = b64_encode((unsigned char*)(err ? err : ""), err_len);
        int msg_cap = strlen(out_b64) + strlen(err_b64) + 256;
        char *msg = (char*)malloc(msg_cap);
        snprintf(msg, msg_cap, "{\"id\":%d,\"ok\":true,\"exit\":%d,\"out\":\"%s\",\"err\":\"%s\"}",
                 id, exit_code, out_b64, err_b64);
        rpc_write_line(s, msg);
        free(msg); free(out_b64); free(err_b64);
        free(cmd); free(cwd); free(out); free(err);
    } else if (strcmp(op, "read") == 0) {
        char *path = json_str(req, "path");
        FILE *f = path ? fopen(path, "rb") : NULL;
        if (!f) {
            char msg[512];
            snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"errno\":\"%s\",\"err\":\"%s\"}",
                     id, (errno == ENOENT ? "ENOENT" : errno == EACCES ? "EACCES" : "EIO"), strerror(errno));
            rpc_write_line(s, msg);
        } else {
            fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
            unsigned char *buf = (unsigned char*)malloc(sz > 0 ? sz : 1);
            int n = sz > 0 ? (int)fread(buf, 1, sz, f) : 0;
            fclose(f);
            char *b64 = b64_encode(buf, n);
            int cap = strlen(b64) + 128; char *msg = (char*)malloc(cap);
            snprintf(msg, cap, "{\"id\":%d,\"ok\":true,\"data\":\"%s\"}", id, b64);
            rpc_write_line(s, msg);
            free(msg); free(b64); free(buf);
        }
        free(path);
    } else if (strcmp(op, "write") == 0) {
        char *path = json_str(req, "path");
        char *data = json_str(req, "data");
        int data_len = 0;
        unsigned char *bytes = data ? b64_decode(data, &data_len) : NULL;
        int ok_w = 0;
        if (path && bytes) {
            FILE *f = fopen(path, "wb");
            if (f) {
                if (fwrite(bytes, 1, data_len, f) == (size_t)data_len) ok_w = 1;
                fclose(f);
            }
        }
        char msg[512];
        if (ok_w) snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":true}", id);
        else      snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"errno\":\"%s\",\"err\":\"%s\"}",
                           id, (errno == ENOENT ? "ENOENT" : errno == EACCES ? "EACCES" : "EIO"),
                           strerror(errno));
        rpc_write_line(s, msg);
        free(path); free(data); free(bytes);
    } else if (strcmp(op, "stat") == 0) {
        char *path = json_str(req, "path");
        struct stat st;
        if (path && stat(path, &st) == 0) {
            int is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
            char msg[512];
            snprintf(msg, sizeof(msg),
                "{\"id\":%d,\"ok\":true,\"size\":%lld,\"mtime\":%lld,\"isDir\":%s,\"mode\":%u}",
                id, (long long)st.st_size, (long long)st.st_mtime,
                is_dir ? "true" : "false", (unsigned)st.st_mode);
            rpc_write_line(s, msg);
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"errno\":\"ENOENT\"}", id);
            rpc_write_line(s, msg);
        }
        free(path);
    } else if (strcmp(op, "readdir") == 0) {
        char *path = json_str(req, "path");
        /* portable: use opendir/readdir — works on POSIX + MinGW */
        DIR *d = path ? opendir(path) : NULL;
        if (!d) {
            char msg[256];
            snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"errno\":\"ENOENT\"}", id);
            rpc_write_line(s, msg);
        } else {
            /* build JSON array of names */
            int cap = 4096; char *out = (char*)malloc(cap); int pos = 0;
            pos += snprintf(out + pos, cap - pos, "{\"id\":%d,\"ok\":true,\"entries\":[", id);
            struct dirent *ent; int first = 1;
            while ((ent = readdir(d))) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                /* JSON-escape the name */
                char esc[1024]; json_escape(esc, ent->d_name);
                /* grow buffer if needed */
                int need = strlen(esc) + 8;
                if (pos + need > cap) { cap = (pos + need) * 2; out = (char*)realloc(out, cap); }
                pos += snprintf(out + pos, cap - pos, "%s\"%s\"", first ? "" : ",", esc);
                first = 0;
            }
            closedir(d);
            pos += snprintf(out + pos, cap - pos, "]}");
            rpc_write_line(s, out);
            free(out);
        }
        free(path);
    } else if (strcmp(op, "unlink") == 0) {
        char *path = json_str(req, "path");
        int rc = path ? unlink(path) : -1;
        char msg[256];
        if (rc == 0) snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":true}", id);
        else snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"errno\":\"%s\"}",
                      id, (errno == ENOENT ? "ENOENT" : "EIO"));
        rpc_write_line(s, msg);
        free(path);
    } else if (strcmp(op, "mkdir") == 0) {
        char *path = json_str(req, "path");
#ifdef _WIN32
        int rc = path ? mkdir(path) : -1;
#else
        int rc = path ? mkdir(path, 0755) : -1;
#endif
        char msg[256];
        if (rc == 0) snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":true}", id);
        else snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"errno\":\"%s\"}",
                      id, strerror(errno));
        rpc_write_line(s, msg);
        free(path);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"err\":\"unknown op: %s\"}", id, op);
        rpc_write_line(s, msg);
    }
    free(op);
}

/* Run as --agent: listen, accept one client, serve RPC forever. */
static int flowto_run_agent(const char *addr) {
    sock_init();
    int port = atoi(addr);
    char host[64] = "0.0.0.0";
    const char *colon = strchr(addr, ':');
    if (colon) {
        memcpy(host, addr, colon - addr); host[colon - addr] = 0;
        port = atoi(colon + 1);
    }
    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr(host);
    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("agent bind"); return 1; }
    listen(srv, 1);
    fprintf(stderr, "flowto agent: listening on %s:%d\n", host, port);
    for (;;) {
        struct sockaddr_in ca; int calen = sizeof(ca);
        sock_t c = accept(srv, (struct sockaddr*)&ca, (void*)&calen);
        if (c == SOCK_INVALID) continue;
        fprintf(stderr, "flowto agent: client connected\n");
        RpcConn conn; rpc_conn_init(&conn, c);
        for (;;) {
            int n = 0;
            char *req = rpc_read_line(&conn, &n);
            if (!req || n <= 0) break;
            agent_handle_request(c, req);
        }
        rpc_conn_free(&conn);
        sock_close(c);
        fprintf(stderr, "flowto agent: client disconnected\n");
    }
}

/* ── DRIVER: gateway for child's JS shim ─────────────────────
   The driver opens a local TCP gateway. It tells the spawned child via
   DUMBTERM_GATEWAY env var. The shim (--require _shim.js) opens a client
   socket to the gateway. Shim sends JSON-line RPC requests; driver routes
   them:
     - host=="local" → agent_handle_request locally
     - host=="<remote>" → forward to the configured agent via a second socket.
   One gateway accept, then serve forever. */

static sock_t g_agent_sock = SOCK_INVALID; /* driver → agent connection */

/* Connect to remote agent; strdup'd errors to stderr. Returns sock or SOCK_INVALID. */
static sock_t flowto_connect_agent(const char *addr) {
    sock_init();
    char host[256] = "127.0.0.1";
    int port = 0;
    const char *colon = strrchr(addr, ':');
    if (colon) {
        memcpy(host, addr, colon - addr); host[colon - addr] = 0;
        port = atoi(colon + 1);
    } else {
        port = atoi(addr);
    }
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET; sa.sin_port = htons(port);
    struct hostent *he = gethostbyname(host);
    if (he) memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
    else sa.sin_addr.s_addr = inet_addr(host);
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "flowto: can't connect to agent at %s:%d: %s\n", host, port, strerror(errno));
        return SOCK_INVALID;
    }
    fprintf(stderr, "flowto: connected to agent at %s:%d\n", host, port);
    return s;
}

/* Forward an exec request to the remote agent, return response to shim. */
static void flowto_forward_to_agent(sock_t shim, const char *req) {
    if (g_agent_sock == SOCK_INVALID) {
        int id = json_int(req, "id", 0);
        char msg[256];
        snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"err\":\"no agent connected\"}", id);
        rpc_write_line(shim, msg);
        return;
    }
    rpc_write_line(g_agent_sock, req);
    /* Persistent RpcConn to the agent. Read response lines until we see a
       terminal message. For exec/fs/ping/host_info: one response with "ok".
       For spawn: many stream messages + a final one with "exit". */
    static RpcConn ac = { SOCK_INVALID, NULL, 0, 0, 0 };
    if (ac.s != g_agent_sock) {
        if (ac.buf) rpc_conn_free(&ac);
        rpc_conn_init(&ac, g_agent_sock);
    }
    /* Is this a spawn request? If so, loop until we see "exit". */
    char *op = json_str(req, "op");
    int is_spawn = op && strcmp(op, "spawn") == 0;
    free(op);
    for (;;) {
        int n = 0;
        char *resp = rpc_read_line(&ac, &n);
        if (!resp || n <= 0) {
            int id = json_int(req, "id", 0);
            char msg[256];
            snprintf(msg, sizeof(msg), "{\"id\":%d,\"ok\":false,\"err\":\"agent read failed\"}", id);
            rpc_write_line(shim, msg);
            sock_close(g_agent_sock); g_agent_sock = SOCK_INVALID;
            return;
        }
        rpc_write_line(shim, resp);
        if (!is_spawn) return;
        /* spawn: consume stream messages + first one with "exit" ends */
        if (strstr(resp, "\"exit\":")) return;
    }
}

/* Handle one request from the shim: dispatch by host field. */
static void flowto_dispatch(sock_t shim, const char *req) {
    char *host = json_str(req, "host");
    /* default to remote if --flowto given, else local */
    int to_remote = g_flowto_addr && (!host || strcmp(host, "local") != 0);
    if (to_remote) flowto_forward_to_agent(shim, req);
    else           agent_handle_request(shim, req);
    free(host);
}

/* Run the shim gateway loop: accept one client, serve requests forever.
   gateway_port is the bound TCP port. Returns when shim disconnects. */
static void flowto_run_gateway(sock_t gateway) {
    struct sockaddr_in ca; int calen = sizeof(ca);
    sock_t shim = accept(gateway, (struct sockaddr*)&ca, (void*)&calen);
    if (shim == SOCK_INVALID) return;
    fprintf(stderr, "flowto: shim connected\n");
    RpcConn conn; rpc_conn_init(&conn, shim);
    for (;;) {
        int n = 0;
        char *req = rpc_read_line(&conn, &n);
        if (!req || n <= 0) break;
        flowto_dispatch(shim, req);
    }
    rpc_conn_free(&conn);
    sock_close(shim);
    fprintf(stderr, "flowto: shim disconnected\n");
}

/* Bind a free gateway port on 127.0.0.1. Returns (sock, port) or (-1, 0). */
static sock_t flowto_bind_gateway(int *out_port) {
    sock_init();
    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = 0; /* any free port */
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(srv, (struct sockaddr*)&sa, sizeof(sa)) < 0) { *out_port = 0; return SOCK_INVALID; }
    int alen = sizeof(sa);
    if (getsockname(srv, (struct sockaddr*)&sa, (void*)&alen) < 0) { *out_port = 0; sock_close(srv); return SOCK_INVALID; }
    listen(srv, 1);
    *out_port = ntohs(sa.sin_port);
    return srv;
}

#endif /* FLOWTO_C */
