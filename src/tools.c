/*
 * Built-in tool implementations: shell, file_read, file_write, memory.
 * Each tool is a vtable instance matching the nc_tool interface.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <poll.h>
#include <signal.h>
#include <time.h>

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>
#include <ctype.h>
#include <strings.h>

static bool path_is_safe_relative(const char *path);

/* ── Helper: extract string from tool args JSON ───────────────── */

static size_t json_escape(const char *src, size_t len,
                          char *dst, size_t cap)
{
    size_t j = 0;

    for (size_t i = 0; i < len && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];

        switch (c) {
        case '"':
            if (j + 2 >= cap) goto done;
            dst[j++] = '\\';
            dst[j++] = '"';
            break;

        case '\\':
            if (j + 2 >= cap) goto done;
            dst[j++] = '\\';
            dst[j++] = '\\';
            break;

        case '\n':
            if (j + 2 >= cap) goto done;
            dst[j++] = '\\';
            dst[j++] = 'n';
            break;

        case '\r':
            if (j + 2 >= cap) goto done;
            dst[j++] = '\\';
            dst[j++] = 'r';
            break;

        case '\t':
            if (j + 2 >= cap) goto done;
            dst[j++] = '\\';
            dst[j++] = 't';
            break;

        default:
            if (c < 0x20) {
                if (j + 6 >= cap) goto done;
                snprintf(dst + j, cap - j, "\\u%04x", c);
                j += 6;
            } else {
                dst[j++] = (char)c;
            }
        }
    }

done:
    dst[j] = '\0';
    return j;
}

typedef enum {
    NC_EXTRACT_OK = 0,
    NC_EXTRACT_MISSING,
    NC_EXTRACT_EMPTY,
    NC_EXTRACT_TRUNCATED,
    NC_EXTRACT_INVALID_JSON
} nc_extract_status;

static nc_extract_status extract_json_string2(const char *json,
                                              const char *key,
                                              char *out,
                                              size_t out_cap,
                                              size_t *full_len) {
    nc_arena a;
    nc_arena_init(&a, strlen(json) * 2 + 256);
    nc_json *root = nc_json_parse(&a, json, strlen(json));
    if (!root) {
        nc_arena_free(&a);
        return NC_EXTRACT_INVALID_JSON;
    }

    nc_json *val = nc_json_get(root, key);
    nc_str s = nc_json_str(val, "");
    if (full_len)
        *full_len = s.len;
    if (!val || val->type != NC_JSON_STRING) {
        nc_arena_free(&a);
        return NC_EXTRACT_MISSING;
    }

    if (s.len == 0) {
        if (out_cap > 0)
            out[0] = '\0';
        nc_arena_free(&a);
        return NC_EXTRACT_OK;
    }

    if (out_cap == 0) {
        nc_arena_free(&a);
        return NC_EXTRACT_TRUNCATED;
    }

    size_t max_bytes = s.len < out_cap - 1 ? s.len : out_cap - 1;
    size_t cplen = nc_utf8_safe_prefix(s.ptr, s.len, max_bytes);
    memcpy(out, s.ptr, cplen);
    out[cplen] = '\0';
    nc_arena_free(&a);
    return s.len > cplen ? NC_EXTRACT_TRUNCATED : NC_EXTRACT_OK;
}

static char *extract_json_string_dup(const char *json, const char *key) {
    size_t full_len = 0;
    nc_extract_status st = extract_json_string2(json, key, NULL, 0, &full_len);
    if (st == NC_EXTRACT_MISSING || st == NC_EXTRACT_INVALID_JSON)
        return NULL;
    char *out = (char *)malloc(full_len + 1);
    if (!out)
        return NULL;
    st = extract_json_string2(json, key, out, full_len + 1, NULL);
    if (st != NC_EXTRACT_OK) {
        free(out);
        return NULL;
    }
    return out;
}

static bool append_suffix(char *out, size_t out_cap, const char *suffix)
{
    size_t total = strlen(out);
    size_t sl = strlen(suffix);
    size_t full_sl = sl;

    if (total >= out_cap)
        return false;

    if (sl > out_cap - total - 1)
        sl = out_cap - total - 1;

    memcpy(out + total, suffix, sl);
    out[total + sl] = '\0';
    return sl == full_sl;
}

static void tool_debug(const char *tool, const char *fmt, ...)
{
    char msg[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    nc_log(NC_LOG_INFO, "[tool:%s] %s", tool, msg);
}

static void tool_debug_preview(const char *tool,
                               const char *label,
                               const char *data,
                               size_t len)
{
    char preview[NC_LOG_PREVIEW_MAX + 32];
    size_t shown;

    if (!data)
        data = "";

    shown = len < NC_LOG_PREVIEW_MAX ? len : NC_LOG_PREVIEW_MAX;
    memcpy(preview, data, shown);
    preview[shown] = '\0';

    if (shown < len)
        nc_log(NC_LOG_INFO,
               "[tool:%s] %s (%zu bytes, preview %zu): %s...[truncated for log]",
               tool, label, len, shown, preview);
    else
        nc_log(NC_LOG_INFO,
               "[tool:%s] %s (%zu bytes): %s",
               tool, label, len, preview);
}

/* ── Shell tool ───────────────────────────────────────────────── */

static bool shell_command_is_allowed(const char *command, char *reason, size_t reason_cap)
{
    const char *blocked[] = {
        "rm -rf /", "rm -rf /*", "mkfs", "shutdown", "reboot", "halt",
        "poweroff", "init 0", "init 6", ":(){", "dd if=", "chmod -R /", NULL
    };

    if (!command || !command[0]) {
        nc_strlcpy(reason, "empty command", reason_cap);
        return false;
    }

    for (int i = 0; blocked[i]; i++) {
        if (strstr(command, blocked[i])) {
            snprintf(reason, reason_cap, "blocked pattern: %s", blocked[i]);
            return false;
        }
    }

    for (const unsigned char *p = (const unsigned char *)command; *p; p++) {
        if ((*p < 0x20 && *p != '\n' && *p != '\r' && *p != '\t') || *p == 0x7f) {
            nc_strlcpy(reason, "command contains disallowed control characters", reason_cap);
            return false;
        }
    }

    return true;
}

static bool shell_quote_single(char *dst, size_t dst_cap, const char *src)
{
    size_t di = 0;

    if (dst_cap < 3)
        return false;

    dst[di++] = '\'';
    for (; *src; src++) {
        if (*src == '\'') {
            if (di + 4 >= dst_cap)
                return false;
            memcpy(dst + di, "'\\''", 4);
            di += 4;
        } else {
            if (di + 1 >= dst_cap)
                return false;
            dst[di++] = *src;
        }
    }

    if (di + 2 > dst_cap)
        return false;
    dst[di++] = '\'';
    dst[di] = '\0';
    return true;
}

static long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Run argv in its own process group, feeding stdin_data (or /dev/null) and
 * capturing stdout+stderr into `out`. Output beyond out_cap is drained and
 * discarded so the child never blocks on a full pipe; stdin is written
 * non-blocking alongside so large inputs cannot deadlock against output.
 * If the process is still running after timeout_seconds, the whole process
 * group (pipelines, background jobs) is killed. */
bool nc_run_process(const nc_proc_opts *o, char *out, size_t out_cap,
                    nc_proc_result *r)
{
    int out_pipe[2];
    int in_pipe[2] = { -1, -1 };
    pid_t pid;
    size_t total = 0;
    size_t in_off = 0;
    int timeout_s = o->timeout_seconds > 0 ? o->timeout_seconds : 60;
    /* Leave room after captured output for truncation/exit/timeout notes. */
    size_t capture_cap = out_cap > 1024 ? out_cap - 256 : out_cap;
    long long deadline = monotonic_ms() + (long long)timeout_s * 1000;
    struct sigaction ign, old_pipe;

    memset(r, 0, sizeof(*r));
    if (out_cap == 0)
        return false;
    out[0] = '\0';

    if (pipe(out_pipe) != 0) {
        snprintf(out, out_cap, "error: failed to create pipe: %s", strerror(errno));
        return false;
    }
    if (o->stdin_data && pipe(in_pipe) != 0) {
        snprintf(out, out_cap, "error: failed to create pipe: %s", strerror(errno));
        close(out_pipe[0]);
        close(out_pipe[1]);
        return false;
    }

    pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (in_pipe[0] >= 0) { close(in_pipe[0]); close(in_pipe[1]); }
        snprintf(out, out_cap, "error: failed to execute command: %s", strerror(saved));
        return false;
    }

    if (pid == 0) {
        setpgid(0, 0);
        if (in_pipe[0] >= 0) {
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
            close(in_pipe[1]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        if (o->cwd && chdir(o->cwd) != 0) {
            static const char msg[] = "error: cannot enter working directory\n";
            ssize_t ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
            (void)ignored;
            _exit(126);
        }
        for (const char *const *e = o->env; e && *e; e++) {
            const char *eq = strchr(*e, '=');
            char key[128];
            if (!eq || (size_t)(eq - *e) >= sizeof(key))
                continue;
            memcpy(key, *e, (size_t)(eq - *e));
            key[eq - *e] = '\0';
            setenv(key, eq + 1, 1);
        }
        execv(o->argv[0], (char *const *)o->argv);
        _exit(127);
    }

    /* Also set in the parent so kill(-pid) works even if the child has not
     * run setpgid yet. */
    setpgid(pid, pid);
    close(out_pipe[1]);

    /* A child that exits without reading its stdin must not SIGPIPE us. */
    memset(&ign, 0, sizeof(ign));
    ign.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &ign, &old_pipe);

    if (in_pipe[0] >= 0) {
        close(in_pipe[0]);
        fcntl(in_pipe[1], F_SETFL, fcntl(in_pipe[1], F_GETFL) | O_NONBLOCK);
        if (o->stdin_len == 0) {
            close(in_pipe[1]);
            in_pipe[1] = -1;
        }
    }

    for (;;) {
        long long remaining = deadline - monotonic_ms();
        struct pollfd pfd[2];
        int nfds = 1;
        char buf[4096];
        ssize_t n;
        int pr;

        if (remaining <= 0) {
            r->timed_out = true;
            break;
        }
        pfd[0] = (struct pollfd){ .fd = out_pipe[0], .events = POLLIN };
        if (in_pipe[1] >= 0)
            pfd[nfds++] = (struct pollfd){ .fd = in_pipe[1], .events = POLLOUT };

        pr = poll(pfd, (nfds_t)nfds, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; /* deadline re-checked at top of loop */

        if (nfds == 2 && pfd[1].revents) {
            n = write(in_pipe[1], o->stdin_data + in_off, o->stdin_len - in_off);
            if (n > 0)
                in_off += (size_t)n;
            if ((n < 0 && errno != EAGAIN && errno != EINTR) ||
                in_off >= o->stdin_len || (pfd[1].revents & (POLLERR | POLLHUP))) {
                close(in_pipe[1]);
                in_pipe[1] = -1;
            }
        }

        if (!pfd[0].revents)
            continue;
        n = read(out_pipe[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (n == 0)
            break; /* EOF: every writer has closed the pipe */

        if (total < capture_cap - 1) {
            size_t cp = (size_t)n;
            if (cp > capture_cap - 1 - total)
                cp = capture_cap - 1 - total;
            memcpy(out + total, buf, cp);
            total += cp;
            if (cp < (size_t)n)
                r->truncated = true;
        } else {
            r->truncated = true;
        }
    }
    close(out_pipe[0]);
    if (in_pipe[1] >= 0)
        close(in_pipe[1]);
    sigaction(SIGPIPE, &old_pipe, NULL);
    out[total] = '\0';

    if (r->timed_out)
        kill(-pid, SIGKILL);

    /* The process may outlive its stdout (e.g. `exec >&-; sleep 100`), so
     * keep honouring the deadline while waiting for it to exit. */
    for (;;) {
        pid_t w = waitpid(pid, &r->status, r->timed_out ? 0 : WNOHANG);
        if (w == pid)
            break;
        if (w < 0) {
            if (errno == EINTR)
                continue;
            snprintf(out, out_cap, "error: failed to wait for process: %s", strerror(errno));
            kill(-pid, SIGKILL);
            return false;
        }
        if (r->timed_out)
            continue;
        if (monotonic_ms() >= deadline) {
            r->timed_out = true;
            kill(-pid, SIGKILL);
            continue;
        }
        {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000 };
            nanosleep(&ts, NULL);
        }
    }

    /* Background jobs that detached from the pipe (e.g. `cmd > log &`) are
     * left running on success; only a timeout kills the group. */
    return true;
}

/* Append the standard notes (truncation, timeout, signal, exit code) for a
 * finished process. Returns true only for a clean exit with status 0. */
bool nc_proc_annotate(const nc_proc_result *r, int timeout_seconds,
                      char *out, size_t out_cap)
{
    char suffix[96];

    if (r->truncated)
        append_suffix(out, out_cap, "\n[output truncated]");

    if (r->timed_out) {
        snprintf(suffix, sizeof(suffix),
                 "\n[timed out after %d seconds; process killed]", timeout_seconds);
        append_suffix(out, out_cap, suffix);
        return false;
    }
    if (!WIFEXITED(r->status)) {
        if (WIFSIGNALED(r->status)) {
            snprintf(suffix, sizeof(suffix), "\n[terminated by signal: %d]",
                     WTERMSIG(r->status));
            append_suffix(out, out_cap, suffix);
        } else {
            append_suffix(out, out_cap, "\n[process terminated abnormally]");
        }
        return false;
    }
    if (WEXITSTATUS(r->status) != 0) {
        snprintf(suffix, sizeof(suffix), "\n[exit code: %d]", WEXITSTATUS(r->status));
        append_suffix(out, out_cap, suffix);
        return false;
    }
    return true;
}

static bool shell_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char *command = NULL;
    char *shell_cmd = NULL;
    char block_reason[128];
    size_t full_len = 0;
    nc_extract_status est;
    bool ok = false;

    if (out_cap == 0)
        return false;

    out[0] = '\0';

    est = extract_json_string2(args_json, "command", NULL, 0, &full_len);
    if (est == NC_EXTRACT_INVALID_JSON) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        goto cleanup;
    }
    if (est == NC_EXTRACT_MISSING) {
        nc_strlcpy(out, "error: missing 'command' argument", out_cap);
        goto cleanup;
    }
    if (full_len >= NC_SHELL_COMMAND_MAX) {
        snprintf(out, out_cap,
                 "error: 'command' argument too long (%zu bytes, max %zu)",
                 full_len, (size_t)NC_SHELL_COMMAND_MAX - 1);
        goto cleanup;
    }
    command = (char *)malloc(full_len + 1);
    if (!command) {
        nc_strlcpy(out, "error: out of memory", out_cap);
        goto cleanup;
    }
    est = extract_json_string2(args_json, "command", command, full_len + 1, NULL);
    if (est != NC_EXTRACT_OK) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        goto cleanup;
    }

    if (command[0] == '\0') {
        nc_strlcpy(out, "error: empty command", out_cap);
        goto cleanup;
    }

    if (!shell_command_is_allowed(command, block_reason, sizeof(block_reason))) {
        snprintf(out, out_cap, "error: unsafe command rejected (%s)", block_reason);
        goto cleanup;
    }

    tool_debug_preview("shell", "command", command, strlen(command));

    if (cfg->workspace_only) {
        char workspace[PATH_MAX];
        char quoted_workspace[PATH_MAX * 4 + 8];
        int n;
        size_t shell_cmd_cap;

        if (!realpath(cfg->workspace_dir, workspace)) {
            nc_strlcpy(out, "error: invalid workspace path", out_cap);
            goto cleanup;
        }

        if (!shell_quote_single(quoted_workspace, sizeof(quoted_workspace), workspace)) {
            nc_strlcpy(out, "error: workspace path too long", out_cap);
            goto cleanup;
        }

        shell_cmd_cap = strlen(command) + strlen(quoted_workspace) + 32;
        shell_cmd = (char *)malloc(shell_cmd_cap);
        if (!shell_cmd) {
            nc_strlcpy(out, "error: out of memory", out_cap);
            goto cleanup;
        }
        n = snprintf(shell_cmd, shell_cmd_cap,
                     "cd %s && ( %s\n) 2>&1",
                     quoted_workspace, command);
        if (n < 0 || (size_t)n >= shell_cmd_cap) {
            nc_strlcpy(out, "error: command too long", out_cap);
            goto cleanup;
        }
    } else {
        size_t shell_cmd_cap = strlen(command) + 16;
        int n;
        shell_cmd = (char *)malloc(shell_cmd_cap);
        if (!shell_cmd) {
            nc_strlcpy(out, "error: out of memory", out_cap);
            goto cleanup;
        }
        n = snprintf(shell_cmd, shell_cmd_cap, "( %s\n) 2>&1", command);
        if (n < 0 || (size_t)n >= shell_cmd_cap) {
            nc_strlcpy(out, "error: command too long", out_cap);
            goto cleanup;
        }
    }

    {
        int timeout_s = cfg->shell_timeout_seconds > 0 ? cfg->shell_timeout_seconds : 60;
        const char *argv[] = { "/bin/sh", "-c", shell_cmd, NULL };
        nc_proc_opts po = { .argv = argv, .timeout_seconds = timeout_s };
        nc_proc_result pr;

        if (!nc_run_process(&po, out, out_cap, &pr))
            goto cleanup;
        if (pr.timed_out)
            tool_debug("shell", "command timed out after %d seconds", timeout_s);
        if (!nc_proc_annotate(&pr, timeout_s, out, out_cap))
            goto cleanup;
    }

    ok = true;

cleanup:
    free(shell_cmd);
    free(command);
    return ok;
}

nc_tool nc_tool_shell(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "shell",
            .description = "Execute a shell command and return its output.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\",\"description\":\"The shell command to execute\"}},\"required\":[\"command\"]}",
        },
        .ctx = (void *)cfg,
        .execute = shell_execute,
        .free = NULL,
    };
}

/* ── File read tool ───────────────────────────────────────────── */

static bool path_within_workspace(const char *path,
                                  const char *workspace)
{
    size_t n = strlen(workspace);

    return strncmp(path, workspace, n) == 0 &&
           (path[n] == '\0' || path[n] == '/');
}

/* Resolve the path of an existing file for reading or editing. In
 * workspace mode the fully resolved path (after symlinks) must stay inside
 * the workspace. On failure writes an error message to err and returns
 * false. */
static bool resolve_existing_path(const nc_config *cfg, const char *path,
                                  char *resolved_out, size_t resolved_cap,
                                  char *err, size_t err_cap)
{
    if (cfg->workspace_only) {
        char candidate[PATH_MAX];
        char resolved[PATH_MAX];
        char workspace[PATH_MAX];

        if (path[0] == '/') {
            if (nc_strlcpy(candidate, path, sizeof(candidate)) >= sizeof(candidate)) {
                nc_strlcpy(err, "error: path too long", err_cap);
                return false;
            }
        } else {
            if (!path_is_safe_relative(path)) {
                nc_strlcpy(err, "error: path traversal not allowed", err_cap);
                return false;
            }
            if (!nc_path_join(candidate, sizeof(candidate), cfg->workspace_dir, path)) {
                nc_strlcpy(err, "error: path too long", err_cap);
                return false;
            }
        }

        if (!realpath(candidate, resolved)) {
            snprintf(err, err_cap, "error: cannot resolve file '%s'", path);
            return false;
        }
        if (!realpath(cfg->workspace_dir, workspace)) {
            nc_strlcpy(err, "error: invalid workspace path", err_cap);
            return false;
        }
        if (!path_within_workspace(resolved, workspace)) {
            nc_strlcpy(err, "error: path escapes workspace", err_cap);
            return false;
        }
        if (nc_strlcpy(resolved_out, resolved, resolved_cap) >= resolved_cap) {
            nc_strlcpy(err, "error: path too long", err_cap);
            return false;
        }
        return true;
    }

    if (path[0] != '/' && !path_is_safe_relative(path)) {
        nc_strlcpy(err, "error: path traversal not allowed", err_cap);
        return false;
    }
    if (nc_strlcpy(resolved_out, path, resolved_cap) >= resolved_cap) {
        nc_strlcpy(err, "error: path too long", err_cap);
        return false;
    }
    return true;
}

static bool file_read_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024];
    char read_path[PATH_MAX];
    size_t full_len = 0;
    nc_extract_status est = extract_json_string2(args_json, "path", path, sizeof(path), &full_len);

    if (est == NC_EXTRACT_INVALID_JSON) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        return false;
    }
    if (est == NC_EXTRACT_MISSING) {
        nc_strlcpy(out, "error: missing 'path' argument", out_cap);
        return false;
    }
    if (est == NC_EXTRACT_TRUNCATED) {
        snprintf(out, out_cap, "error: 'path' argument too long (%zu bytes, max %zu)",
                 full_len, sizeof(path) - 1);
        return false;
    }

    if (!resolve_existing_path(cfg, path, read_path, sizeof(read_path), out, out_cap))
        return false;

    size_t len;
    char *content = nc_read_file(read_path, &len);
    if (!content) {
        snprintf(out, out_cap, "error: cannot read file '%s'", path);
        return false;
    }

    json_escape(content, len, out, out_cap);
    free(content);
    return true;
}

nc_tool nc_tool_file_read(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "file_read",
            .description = "Read the contents of a file.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path to read\"}},\"required\":[\"path\"]}",
        },
        .ctx = (void *)cfg,
        .execute = file_read_execute,
        .free = NULL,
    };
}

/* ── File write tool ──────────────────────────────────────────── */

static bool file_write_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024];
    char *content = NULL;
    char write_path[PATH_MAX];
    size_t content_len;
    size_t full_len = 0;
    nc_extract_status est;
    bool ok = false;

    est = extract_json_string2(args_json, "path", path, sizeof(path), &full_len);
    if (est == NC_EXTRACT_INVALID_JSON) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        goto cleanup;
    }
    if (est == NC_EXTRACT_MISSING) {
        nc_strlcpy(out, "error: missing 'path' argument", out_cap);
        goto cleanup;
    }
    if (est == NC_EXTRACT_TRUNCATED) {
        snprintf(out, out_cap, "error: 'path' argument too long (%zu bytes, max %zu)",
                 full_len, sizeof(path) - 1);
        goto cleanup;
    }
    est = extract_json_string2(args_json, "content", NULL, 0, &full_len);
    if (est == NC_EXTRACT_MISSING) {
        nc_strlcpy(out, "error: missing 'content' argument", out_cap);
        goto cleanup;
    }
    if (est == NC_EXTRACT_INVALID_JSON) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        goto cleanup;
    }
    if (full_len >= NC_FILE_CONTENT_MAX) {
        snprintf(out, out_cap, "error: 'content' argument too long (%zu bytes, max %zu)",
                 full_len, (size_t)NC_FILE_CONTENT_MAX - 1);
        goto cleanup;
    }
    content = (char *)malloc(full_len + 1);
    if (!content) {
        nc_strlcpy(out, "error: out of memory", out_cap);
        goto cleanup;
    }
    est = extract_json_string2(args_json, "content", content, full_len + 1, NULL);
    if (est != NC_EXTRACT_OK) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        goto cleanup;
    }

    content_len = strlen(content);

    /* Security: workspace scoping */
    if (cfg->workspace_only) {
        if (path[0] == '/') {
            nc_strlcpy(out, "error: absolute paths not allowed in workspace mode", out_cap);
            goto cleanup;
        }

        if (!path_is_safe_relative(path)) {
            nc_strlcpy(out, "error: path traversal not allowed", out_cap);
            goto cleanup;
        }

        if (!nc_path_join(write_path, sizeof(write_path), cfg->workspace_dir, path)) {
            nc_strlcpy(out, "error: path too long", out_cap);
            goto cleanup;
        }
    } else {
        if (path[0] == '/') {
            if (nc_strlcpy(write_path, path, sizeof(write_path)) >= sizeof(write_path)) {
                nc_strlcpy(out, "error: path too long", out_cap);
                goto cleanup;
            }
        } else {
            if (!path_is_safe_relative(path)) {
                nc_strlcpy(out, "error: path traversal not allowed", out_cap);
                goto cleanup;
            }
            if (nc_strlcpy(write_path, path, sizeof(write_path)) >= sizeof(write_path)) {
                nc_strlcpy(out, "error: path too long", out_cap);
                goto cleanup;
            }
        }
    }

    {
        char dirbuf[PATH_MAX];
        char *slash;

        if (nc_strlcpy(dirbuf, write_path, sizeof(dirbuf)) >= sizeof(dirbuf)) {
            nc_strlcpy(out, "error: path too long", out_cap);
            goto cleanup;
        }

        slash = strrchr(dirbuf, '/');
        if (slash) {
            *slash = '\0';
            if (dirbuf[0] != '\0' && !nc_mkdir_p(dirbuf)) {
                snprintf(out, out_cap, "error: failed to create parent directories for '%s'", path);
                goto cleanup;
            }
        }
    }

    if (nc_write_file(write_path, content, content_len)) {
        snprintf(out, out_cap, "Written %zu bytes to %s", content_len, write_path);
        ok = true;
    } else {
        snprintf(out, out_cap, "error: failed to write '%s'", path);
    }

cleanup:
    free(content);
    return ok;
}

nc_tool nc_tool_file_write(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "file_write",
            .description = "Write content to a file.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"File path\"},\"content\":{\"type\":\"string\",\"description\":\"Content to write\"}},\"required\":[\"path\",\"content\"]}",
        },
        .ctx = (void *)cfg,
        .execute = file_write_execute,
        .free = NULL,
    };
}

/* ── File edit tool ───────────────────────────────────────────── */

static size_t line_of_offset(const char *s, size_t off) {
    size_t line = 1;
    for (size_t i = 0; i < off; i++)
        if (s[i] == '\n')
            line++;
    return line;
}

static size_t count_occurrences(const char *hay, const char *needle, size_t nlen,
                                size_t *first_off) {
    size_t count = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + nlen, needle)) {
        if (count == 0 && first_off)
            *first_off = (size_t)(p - hay);
        count++;
    }
    return count;
}

/* Explain a failed match: CRLF mismatch, or where the first line of
 * old_string does occur (so the model can re-read that region). */
static void edit_not_found_hint(const char *content, const char *old_s,
                                char *out, size_t out_cap) {
    const char *nl;
    char first[256];
    size_t fl;
    const char *start = old_s;

    if (strstr(content, "\r\n") && strchr(old_s, '\n') && !strstr(old_s, "\r\n")) {
        append_suffix(out, out_cap,
                      " The file uses CRLF (\\r\\n) line endings; include \\r in old_string.");
        return;
    }

    while (*start == ' ' || *start == '\t' || *start == '\n')
        start++;
    nl = strchr(start, '\n');
    fl = nl ? (size_t)(nl - start) : strlen(start);
    while (fl > 0 && (start[fl - 1] == ' ' || start[fl - 1] == '\t' || start[fl - 1] == '\r'))
        fl--;
    if (fl < 4 || fl >= sizeof(first) || !nl)
        goto generic;
    memcpy(first, start, fl);
    first[fl] = '\0';
    {
        const char *hit = strstr(content, first);
        if (hit) {
            char hint[200];
            snprintf(hint, sizeof(hint),
                     " Its first line occurs at line %zu, but the following lines differ;"
                     " re-read the file around that line.",
                     line_of_offset(content, (size_t)(hit - content)));
            append_suffix(out, out_cap, hint);
            return;
        }
    }
generic:
    append_suffix(out, out_cap,
                  " Re-read the file and copy the exact text, including whitespace and indentation.");
}

/* Append lines [from, to] of s, numbered, to out. */
static void append_numbered_lines(const char *s, size_t from, size_t to,
                                  char *out, size_t out_cap) {
    size_t line = 1;
    const char *p = s;
    while (*p && line <= to) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (line >= from) {
            char buf[64];
            snprintf(buf, sizeof(buf), "\n%6zu  ", line);
            append_suffix(out, out_cap, buf);
            if (len > 300) {
                char *tmp = nc_strdup_n(p, 300);
                if (tmp) {
                    append_suffix(out, out_cap, tmp);
                    free(tmp);
                }
                append_suffix(out, out_cap, "...");
            } else {
                char *tmp = nc_strdup_n(p, len);
                if (tmp) {
                    append_suffix(out, out_cap, tmp);
                    free(tmp);
                }
            }
        }
        if (!eol)
            break;
        p = eol + 1;
        line++;
    }
}

/* Write data to path atomically (temp file in the same directory, then
 * rename), keeping the original file mode. */
static bool write_file_atomic(const char *path, const char *data, size_t len, mode_t mode) {
    char tmp[PATH_MAX];
    const char *slash = strrchr(path, '/');
    int fd;
    size_t off = 0;

    if (slash)
        snprintf(tmp, sizeof(tmp), "%.*s/.noclaw_edit_XXXXXX", (int)(slash - path), path);
    else
        nc_strlcpy(tmp, ".noclaw_edit_XXXXXX", sizeof(tmp));

    fd = mkstemp(tmp);
    if (fd < 0)
        return false;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            unlink(tmp);
            return false;
        }
        off += (size_t)n;
    }
    if (fchmod(fd, mode & 07777) != 0 || close(fd) != 0) {
        unlink(tmp);
        return false;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return false;
    }
    return true;
}

static bool file_edit_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char path[1024];
    char real[PATH_MAX];
    char *old_s = NULL, *new_s = NULL, *content = NULL, *updated = NULL;
    bool replace_all = false;
    bool ok = false;
    size_t len = 0, old_len, new_len, first_off = 0, count;
    struct stat st;
    nc_arena a;

    if (out_cap == 0)
        return false;
    out[0] = '\0';

    nc_arena_init(&a, strlen(args_json) * 2 + 256);
    {
        nc_json *root = nc_json_parse(&a, args_json, strlen(args_json));
        nc_json *pv = root ? nc_json_get(root, "path") : NULL;
        nc_json *ov = root ? nc_json_get(root, "old_string") : NULL;
        nc_json *nv = root ? nc_json_get(root, "new_string") : NULL;

        if (!root) {
            nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
            goto cleanup;
        }
        if (!pv || pv->type != NC_JSON_STRING || pv->string.len == 0) {
            nc_strlcpy(out, "error: missing 'path' argument", out_cap);
            goto cleanup;
        }
        if (!ov || ov->type != NC_JSON_STRING) {
            nc_strlcpy(out, "error: missing 'old_string' argument", out_cap);
            goto cleanup;
        }
        if (!nv || nv->type != NC_JSON_STRING) {
            nc_strlcpy(out, "error: missing 'new_string' argument", out_cap);
            goto cleanup;
        }
        if (pv->string.len >= sizeof(path)) {
            nc_strlcpy(out, "error: 'path' argument too long", out_cap);
            goto cleanup;
        }
        memcpy(path, pv->string.ptr, pv->string.len);
        path[pv->string.len] = '\0';
        old_s = nc_strdup_n(ov->string.ptr, ov->string.len);
        new_s = nc_strdup_n(nv->string.ptr, nv->string.len);
        replace_all = nc_json_bool(nc_json_get(root, "replace_all"), false);
        if (!old_s || !new_s) {
            nc_strlcpy(out, "error: out of memory", out_cap);
            goto cleanup;
        }
    }

    old_len = strlen(old_s);
    new_len = strlen(new_s);
    if (old_len == 0) {
        nc_strlcpy(out, "error: 'old_string' must not be empty (use file_write to create files)", out_cap);
        goto cleanup;
    }
    if (strcmp(old_s, new_s) == 0) {
        nc_strlcpy(out, "error: 'old_string' and 'new_string' are identical; nothing to change", out_cap);
        goto cleanup;
    }

    if (!resolve_existing_path(cfg, path, real, sizeof(real), out, out_cap))
        goto cleanup;
    if (stat(real, &st) != 0 || !S_ISREG(st.st_mode)) {
        snprintf(out, out_cap, "error: '%s' is not a regular file", path);
        goto cleanup;
    }
    content = nc_read_file(real, &len);
    if (!content) {
        snprintf(out, out_cap, "error: cannot read '%s' (unreadable or larger than 10 MB)", path);
        goto cleanup;
    }
    if (memchr(content, '\0', len)) {
        snprintf(out, out_cap, "error: '%s' looks like a binary file; file_edit only edits text", path);
        goto cleanup;
    }

    count = count_occurrences(content, old_s, old_len, &first_off);
    if (count == 0) {
        snprintf(out, out_cap, "error: old_string not found in '%s'.", path);
        edit_not_found_hint(content, old_s, out, out_cap);
        goto cleanup;
    }
    if (count > 1 && !replace_all) {
        size_t shown = 0;
        snprintf(out, out_cap,
                 "error: old_string occurs %zu times in '%s' (lines", count, path);
        for (const char *p = strstr(content, old_s); p && shown < 10;
             p = strstr(p + old_len, old_s), shown++) {
            char num[32];
            snprintf(num, sizeof(num), "%s %zu", shown ? "," : "",
                     line_of_offset(content, (size_t)(p - content)));
            append_suffix(out, out_cap, num);
        }
        append_suffix(out, out_cap, count > 10 ? ", ...)." : ").");
        append_suffix(out, out_cap,
                      " Include more surrounding lines to make it unique, or set replace_all to true.");
        goto cleanup;
    }

    {
        size_t new_total = len - count * old_len + count * new_len;
        char *w;
        const char *r = content;

        updated = (char *)malloc(new_total + 1);
        if (!updated) {
            nc_strlcpy(out, "error: out of memory", out_cap);
            goto cleanup;
        }
        w = updated;
        for (size_t i = 0; i < count; i++) {
            const char *hit = strstr(r, old_s);
            memcpy(w, r, (size_t)(hit - r));
            w += hit - r;
            memcpy(w, new_s, new_len);
            w += new_len;
            r = hit + old_len;
        }
        memcpy(w, r, len - (size_t)(r - content));
        w += len - (size_t)(r - content);
        *w = '\0';

        if (!write_file_atomic(real, updated, new_total, st.st_mode)) {
            snprintf(out, out_cap, "error: failed to write '%s': %s", path, strerror(errno));
            goto cleanup;
        }
    }

    {
        size_t first_line = line_of_offset(content, first_off);
        size_t new_lines = 1;
        for (const char *p = new_s; *p; p++)
            if (*p == '\n')
                new_lines++;
        snprintf(out, out_cap, "Edited %s: replaced %zu occurrence%s",
                 path, count, count == 1 ? "" : "s");
        if (count == 1) {
            size_t from = first_line > 2 ? first_line - 2 : 1;
            size_t to = first_line + new_lines + 1;
            if (to > from + 30)
                to = from + 30;
            append_suffix(out, out_cap, ". Result:");
            append_numbered_lines(updated, from, to, out, out_cap);
        } else {
            char at[64];
            snprintf(at, sizeof(at), " (first at line %zu)", first_line);
            append_suffix(out, out_cap, at);
        }
    }
    tool_debug("file_edit", "%s: %zu replacement(s)", path, count);
    ok = true;

cleanup:
    nc_arena_free(&a);
    free(old_s);
    free(new_s);
    free(content);
    free(updated);
    return ok;
}

nc_tool nc_tool_file_edit(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "file_edit",
            .description = "Edit an existing text file by replacing an exact string. old_string must match the file exactly (including whitespace and indentation) and be unique unless replace_all is true. Prefer this over file_write for changing existing files.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{"
                "\"path\":{\"type\":\"string\",\"description\":\"File to edit\"},"
                "\"old_string\":{\"type\":\"string\",\"description\":\"Exact text to replace; include enough surrounding lines to be unique\"},"
                "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text (empty string deletes old_string)\"},"
                "\"replace_all\":{\"type\":\"boolean\",\"description\":\"Replace every occurrence instead of requiring a unique match (default false)\"}},"
                "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        },
        .ctx = (void *)cfg,
        .execute = file_edit_execute,
        .free = NULL,
    };
}

/* ── Apply patch tool ───────────────────────────────────────────── */

typedef struct {
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];
} patch_target;

static bool patch_text_has_hunk(const char *patch);

static bool path_is_safe_relative(const char *path) {
    if (!path || !*path)
        return false;

    if (path[0] == '/')
        return false;

    if (strstr(path, "../") ||
        strstr(path, "/..") ||
        strcmp(path, "..") == 0)
        return false;

    return true;
}

static bool unquote_patch_path(const char *src, char *dst, size_t dst_cap)
{
    size_t src_len;
    size_t di = 0;
    size_t i;

    if (!src || !dst || dst_cap == 0)
        return false;

    src_len = strlen(src);
    if (src_len >= 2 && src[0] == '"' && src[src_len - 1] == '"') {
        src++;
        src_len -= 2;
    }

    for (i = 0; i < src_len; i++) {
        char c = src[i];

        if (c == '\\' && i + 1 < src_len) {
            i++;
            c = src[i];
        }

        if (di + 1 >= dst_cap)
            return false;
        dst[di++] = c;
    }

    dst[di] = '\0';
    return true;
}

static bool parse_diff_git_paths(const char *line,
                                 char *old_path,
                                 size_t old_cap,
                                 char *new_path,
                                 size_t new_cap)
{
    const char *p = line;
    const char *start;
    char tok1[PATH_MAX * 2];
    char tok2[PATH_MAX * 2];
    size_t len = 0;
    bool quoted;

    if (strncmp(p, "diff --git ", 11) != 0)
        return false;
    p += 11;

    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return false;

    quoted = (*p == '"');
    start = p;
    if (quoted) {
        p++;
        while (*p) {
            if (*p == '"' && p[-1] != '\\') {
                p++;
                break;
            }
            p++;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }

    len = (size_t)(p - start);
    if (len == 0 || len >= sizeof(tok1))
        return false;
    memcpy(tok1, start, len);
    tok1[len] = '\0';

    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return false;

    quoted = (*p == '"');
    start = p;
    if (quoted) {
        p++;
        while (*p) {
            if (*p == '"' && p[-1] != '\\') {
                p++;
                break;
            }
            p++;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t')
            p++;
    }

    len = (size_t)(p - start);
    if (len == 0 || len >= sizeof(tok2))
        return false;
    memcpy(tok2, start, len);
    tok2[len] = '\0';

    if (!unquote_patch_path(tok1, tok1, sizeof(tok1)) ||
        !unquote_patch_path(tok2, tok2, sizeof(tok2)))
        return false;

    if (strcmp(tok1, "/dev/null") == 0) {
        old_path[0] = '\0';
    } else {
        const char *old_src = tok1;
        if (strncmp(old_src, "a/", 2) == 0)
            old_src += 2;
        if (!*old_src || !nc_strlcpy(old_path, old_src, old_cap))
            return false;
    }

    if (strcmp(tok2, "/dev/null") == 0) {
        new_path[0] = '\0';
    } else {
        const char *new_src = tok2;
        if (strncmp(new_src, "b/", 2) == 0)
            new_src += 2;
        if (!*new_src || !nc_strlcpy(new_path, new_src, new_cap))
            return false;
    }

    return true;
}

static bool normalize_patch_header_path(const char *src,
                                        char *dst,
                                        size_t dst_cap)
{
    const char *path = src;

    while (*path == ' ' || *path == '\t')
        path++;

    if (!*path)
        return false;

    if (strcmp(path, "/dev/null") == 0) {
        if (dst_cap == 0)
            return false;
        dst[0] = '\0';
        return true;
    }

    if ((path[0] == 'a' || path[0] == 'b') && path[1] == '/')
        path += 2;

    if (!*path)
        return false;

    return nc_strlcpy(dst, path, dst_cap) < dst_cap;
}

static bool parse_unified_header_pair(const char *old_line,
                                      const char *new_line,
                                      char *old_path,
                                      size_t old_cap,
                                      char *new_path,
                                      size_t new_cap)
{
    if (strncmp(old_line, "--- ", 4) != 0 ||
        strncmp(new_line, "+++ ", 4) != 0)
        return false;

    if (!normalize_patch_header_path(old_line + 4, old_path, old_cap))
        return false;
    if (!normalize_patch_header_path(new_line + 4, new_path, new_cap))
        return false;

    return true;
}

static bool validate_patch_path(const char *workspace_root,
                                const char *relpath)
{
    char joined[PATH_MAX];
    char real_ws[PATH_MAX];
    char current[PATH_MAX];
    const char *seg = relpath;

    if (!path_is_safe_relative(relpath))
        return false;

    if (!realpath(workspace_root, real_ws))
        return false;

    if (nc_strlcpy(current, real_ws, sizeof(current)) >= sizeof(current))
        return false;

    while (*seg) {
        const char *slash = strchr(seg, '/');
        size_t seg_len = slash ? (size_t)(slash - seg) : strlen(seg);
        bool is_last = (slash == NULL);
        char component[NAME_MAX + 1];
        char resolved[PATH_MAX];

        if (seg_len == 0 || seg_len > NAME_MAX)
            return false;

        memcpy(component, seg, seg_len);
        component[seg_len] = '\0';

        if (!strcmp(component, ".") || !strcmp(component, ".."))
            return false;

        if (!nc_path_join(joined, sizeof(joined), current, component))
            return false;

        if (access(joined, F_OK) == 0) {
            if (!realpath(joined, resolved))
                return false;
            if (!path_within_workspace(resolved, real_ws))
                return false;
            if (nc_strlcpy(current, resolved, sizeof(current)) >= sizeof(current))
                return false;
        } else {
            if (!is_last && errno != ENOENT)
                return false;
            if (!is_last)
                return false;
        }

        if (!slash)
            break;
        seg = slash + 1;
    }

    return true;
}

static bool add_patch_target(patch_target *targets,
                             size_t *count,
                             size_t max_targets,
                             const char *old_path,
                             const char *new_path)
{
    if (*count >= max_targets)
        return false;

    if (old_path) {
        if (nc_strlcpy(targets[*count].old_path,
                       old_path,
                       sizeof(targets[*count].old_path)) >= sizeof(targets[*count].old_path))
            return false;
    } else {
        targets[*count].old_path[0] = '\0';
    }

    if (new_path) {
        if (nc_strlcpy(targets[*count].new_path,
                       new_path,
                       sizeof(targets[*count].new_path)) >= sizeof(targets[*count].new_path))
            return false;
    } else {
        targets[*count].new_path[0] = '\0';
    }

    (*count)++;
    return true;
}

static bool convert_begin_patch_to_unified(const char *patch,
                                           char *out,
                                           size_t out_cap)
{
    const char *p = patch;
    bool saw_begin = false;
    bool saw_end = false;
    bool changed = false;
    size_t out_len = 0;

    if (!patch || !out || out_cap == 0)
        return false;

    out[0] = '\0';

    while (*p) {
        size_t len = 0;
        char line[8192];

        while (p[len] && p[len] != '\n' && len < sizeof(line) - 1)
            len++;

        if (p[len] != '\0' && p[len] != '\n')
            return false;

        memcpy(line, p, len);
        line[len] = '\0';

        if (strcmp(line, "*** Begin Patch") == 0) {
            saw_begin = true;
            changed = true;
        } else if (strcmp(line, "*** End Patch") == 0) {
            saw_end = true;
            changed = true;
        } else if (strncmp(line, "*** Update File: ", 17) == 0) {
            const char *path = line + 17;
            int n = snprintf(out + out_len,
                             out_cap - out_len,
                             "--- a/%s\n+++ b/%s\n",
                             path,
                             path);
            if (n < 0 || (size_t)n >= out_cap - out_len)
                return false;
            out_len += (size_t)n;
            changed = true;
        } else if (strncmp(line, "*** Add File: ", 14) == 0) {
            const char *path = line + 14;
            int n = snprintf(out + out_len,
                             out_cap - out_len,
                             "--- /dev/null\n+++ b/%s\n",
                             path);
            if (n < 0 || (size_t)n >= out_cap - out_len)
                return false;
            out_len += (size_t)n;
            changed = true;
        } else if (strncmp(line, "*** Delete File: ", 17) == 0) {
            const char *path = line + 17;
            int n = snprintf(out + out_len,
                             out_cap - out_len,
                             "--- a/%s\n+++ /dev/null\n",
                             path);
            if (n < 0 || (size_t)n >= out_cap - out_len)
                return false;
            out_len += (size_t)n;
            changed = true;
        } else {
            if (out_len + len + 2 > out_cap)
                return false;
            memcpy(out + out_len, line, len);
            out_len += len;
            out[out_len++] = '\n';
        }

        if (!p[len])
            break;
        p += len + 1;
    }

    if (!changed)
        return false;

    if (saw_begin && !saw_end)
        return false;

    out[out_len] = '\0';
    return true;
}

static bool patch_text_has_hunk(const char *patch)
{
    const char *p = patch;

    while (p && *p) {
        const char *line_end = strchr(p, '\n');
        size_t len = line_end ? (size_t)(line_end - p) : strlen(p);

        if (len >= 2 && p[0] == '@' && p[1] == '@')
            return true;

        if (!line_end)
            break;
        p = line_end + 1;
    }

    return false;
}

static bool collect_patch_targets(const char *patch,
                                  patch_target *targets,
                                  size_t *count,
                                  size_t max_targets)
{
    char line[8192];
    char prev_line[8192];
    bool have_prev_line = false;
    const char *p = patch;

    *count = 0;
    prev_line[0] = '\0';

    while (*p) {
        size_t len = 0;
        char old_path[PATH_MAX];
        char new_path[PATH_MAX];

        while (p[len] && p[len] != '\n' && len < sizeof(line) - 1)
            len++;

        if (p[len] != '\0' && p[len] != '\n')
            return false;

        memcpy(line, p, len);
        line[len] = '\0';

        old_path[0] = '\0';
        new_path[0] = '\0';

        if (parse_diff_git_paths(line,
                                 old_path,
                                 sizeof(old_path),
                                 new_path,
                                 sizeof(new_path))) {
            if (!add_patch_target(targets,
                                  count,
                                  max_targets,
                                  old_path[0] ? old_path : NULL,
                                  new_path[0] ? new_path : NULL))
                return false;
        } else if (strncmp(line, "*** Update File: ", 17) == 0) {
            if (!add_patch_target(targets,
                                  count,
                                  max_targets,
                                  line + 17,
                                  line + 17))
                return false;
        } else if (strncmp(line, "*** Add File: ", 14) == 0) {
            if (!add_patch_target(targets,
                                  count,
                                  max_targets,
                                  NULL,
                                  line + 14))
                return false;
        } else if (strncmp(line, "*** Delete File: ", 17) == 0) {
            if (!add_patch_target(targets,
                                  count,
                                  max_targets,
                                  line + 17,
                                  NULL))
                return false;
        } else if (have_prev_line &&
                   parse_unified_header_pair(prev_line,
                                             line,
                                             old_path,
                                             sizeof(old_path),
                                             new_path,
                                             sizeof(new_path))) {
            if (!add_patch_target(targets,
                                  count,
                                  max_targets,
                                  old_path[0] ? old_path : NULL,
                                  new_path[0] ? new_path : NULL))
                return false;
        }

        if (nc_strlcpy(prev_line, line, sizeof(prev_line)) >= sizeof(prev_line))
            return false;
        have_prev_line = true;

        if (!p[len])
            break;

        p += len + 1;
    }

    return true;
}

static bool validate_patch_targets(const char *workspace_root,
                                   const char *patch_text,
                                   char *err,
                                   size_t err_cap)
{
    patch_target *targets = NULL;
    size_t count = 0;

    targets = (patch_target *)calloc(512, sizeof(*targets));
    if (!targets) {
        nc_strlcpy(err, "error: out of memory validating patch", err_cap);
        return false;
    }

    if (!collect_patch_targets(patch_text, targets, &count, 512)) {
        nc_strlcpy(err,
                   "error: invalid patch metadata or too many patch targets",
                   err_cap);
        free(targets);
        return false;
    }

    if (count == 0) {
        nc_strlcpy(err, "error: patch contains no file targets", err_cap);
        free(targets);
        return false;
    }

    if (!patch_text_has_hunk(patch_text)) {
        nc_strlcpy(err, "error: patch contains no hunks", err_cap);
        free(targets);
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        if (targets[i].old_path[0] &&
            !validate_patch_path(workspace_root, targets[i].old_path)) {
            snprintf(err,
                     err_cap,
                     "error: invalid patch path '%s'",
                     targets[i].old_path);
            free(targets);
            return false;
        }

        if (targets[i].new_path[0] &&
            !validate_patch_path(workspace_root, targets[i].new_path)) {
            snprintf(err,
                     err_cap,
                     "error: invalid patch path '%s'",
                     targets[i].new_path);
            free(targets);
            return false;
        }
    }

    free(targets);
    return true;
}

static bool run_patch_command(const char *workspace,
                              const char *patch_file,
                              char *out,
                              size_t out_cap,
                              int *status_out)
{
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) != 0) {
        snprintf(out, out_cap, "error: failed to create pipe: %s", strerror(errno));
        return false;
    }

    pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        snprintf(out, out_cap, "error: failed to fork patch process: %s", strerror(saved));
        return false;
    }

    if (pid == 0) {
        int patch_fd = open(patch_file, O_RDONLY);
        if (patch_fd < 0)
            _exit(127);

        if (dup2(patch_fd, STDIN_FILENO) < 0)
            _exit(127);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0)
            _exit(127);
        if (dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);

        close(patch_fd);
        close(pipefd[0]);
        close(pipefd[1]);

        execlp("patch",
               "patch",
               "-d", workspace,
               "--batch",
               "--forward",
               "--reject-file=-",
               "--posix",
               "-p1",
               (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);

    {
        size_t total = 0;
        char buf[512];
        ssize_t nread;

        out[0] = '\0';
        while ((nread = read(pipefd[0], buf, sizeof(buf))) > 0) {
            size_t n = (size_t)nread;
            if (total + n >= out_cap - 1)
                n = out_cap - 1 - total;
            memcpy(out + total, buf, n);
            total += n;
            if (total >= out_cap - 1) {
                tool_debug("apply_patch", "patch output truncated to %zu bytes", total);
                break;
            }
        }
        out[total] = '\0';

        if (nread < 0) {
            int saved = errno;
            close(pipefd[0]);
            waitpid(pid, status_out, 0);
            snprintf(out, out_cap, "error: failed to read patch output: %s", strerror(saved));
            return false;
        }

        if (nread > 0)
            append_suffix(out, out_cap, "\n[output truncated]");
    }

    close(pipefd[0]);

    while (waitpid(pid, status_out, 0) < 0) {
        if (errno != EINTR) {
            snprintf(out, out_cap, "error: failed to wait for patch: %s", strerror(errno));
            return false;
        }
    }

    return true;
}

static bool apply_patch_execute(nc_tool *self,
                                const char *args_json,
                                char *out,
                                size_t out_cap)
{
    const nc_config *cfg = (const nc_config *)self->ctx;
    char *patch_input = NULL;
    size_t patch_full_len = 0;
    char *normalized_patch = NULL;
    nc_extract_status patch_st;
    char workspace[PATH_MAX];
    char tmp_template[PATH_MAX];
    int fd = -1;
    bool ok = false;

    tool_debug("apply_patch", "request received");

    if (out_cap == 0)
        return false;
    out[0] = '\0';
    tmp_template[0] = '\0';

    patch_st = extract_json_string2(args_json, "patch", NULL, 0, &patch_full_len);
    if (patch_st == NC_EXTRACT_INVALID_JSON) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        goto cleanup;
    }
    if (patch_st == NC_EXTRACT_MISSING) {
        tool_debug("apply_patch", "missing 'patch' argument");
        nc_strlcpy(out, "error: missing 'patch' argument", out_cap);
        goto cleanup;
    }
    if (patch_full_len >= NC_PATCH_INPUT_MAX) {
        snprintf(out, out_cap,
                 "error: 'patch' argument too long (%zu bytes, max %zu)",
                 patch_full_len, (size_t)NC_PATCH_INPUT_MAX - 1);
        goto cleanup;
    }
    patch_input = (char *)malloc(patch_full_len + 1);
    if (!patch_input) {
        nc_strlcpy(out, "error: out of memory", out_cap);
        goto cleanup;
    }
    patch_st = extract_json_string2(args_json,
                                    "patch",
                                    patch_input,
                                    patch_full_len + 1,
                                    NULL);
    if (patch_st != NC_EXTRACT_OK) {
        nc_strlcpy(out, "error: invalid JSON arguments", out_cap);
        goto cleanup;
    }

    tool_debug("apply_patch", "patch payload size: %zu bytes", strlen(patch_input));
    tool_debug_preview("apply_patch", "patch preview", patch_input, strlen(patch_input));

    if (patch_input[0] == '\0') {
        nc_strlcpy(out, "error: empty patch", out_cap);
        goto cleanup;
    }

    if (strncmp(patch_input, "*** Begin Patch", 15) == 0) {
        normalized_patch = (char *)malloc(NC_PATCH_INPUT_MAX);
        if (!normalized_patch) {
            nc_strlcpy(out, "error: out of memory", out_cap);
            goto cleanup;
        }
        if (!convert_begin_patch_to_unified(patch_input,
                                            normalized_patch,
                                            NC_PATCH_INPUT_MAX)) {
            nc_strlcpy(out, "error: invalid Begin Patch format", out_cap);
            goto cleanup;
        }
        free(patch_input);
        patch_input = normalized_patch;
        normalized_patch = NULL;
        tool_debug("apply_patch", "converted Begin Patch format to unified diff");
        tool_debug_preview("apply_patch", "normalized patch preview", patch_input, strlen(patch_input));
    }

    if (!cfg->workspace_only) {
        nc_strlcpy(out,
                   "error: apply_patch requires workspace mode",
                   out_cap);
        goto cleanup;
    }

    if (!realpath(cfg->workspace_dir, workspace)) {
        tool_debug("apply_patch",
                   "invalid workspace path '%s': %s",
                   cfg->workspace_dir,
                   strerror(errno));
        nc_strlcpy(out, "error: invalid workspace path", out_cap);
        goto cleanup;
    }

    tool_debug("apply_patch", "resolved workspace: %s", workspace);

    if (!validate_patch_targets(workspace, patch_input, out, out_cap)) {
        tool_debug("apply_patch", "patch target validation failed: %s", out);
        goto cleanup;
    }

    if (snprintf(tmp_template,
                 sizeof(tmp_template),
                 "%s/.noclaw_patch_XXXXXX",
                 workspace) >= (int)sizeof(tmp_template)) {
        nc_strlcpy(out, "error: workspace path too long", out_cap);
        goto cleanup;
    }

    fd = mkstemp(tmp_template);
    if (fd < 0) {
        tool_debug("apply_patch",
                   "failed to create temp patch file '%s': %s",
                   tmp_template,
                   strerror(errno));
        nc_strlcpy(out, "error: failed to create temp patch file", out_cap);
        goto cleanup;
    }
    {
        size_t patch_len = strlen(patch_input);
        size_t written = 0;

        while (written < patch_len) {
            ssize_t n = write(fd, patch_input + written, patch_len - written);
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                nc_strlcpy(out, "error: failed to write patch", out_cap);
                goto cleanup;
            }
            written += (size_t)n;
        }

        if (patch_len > 0 && patch_input[patch_len - 1] != '\n') {
            static const char newline = '\n';
            if (write(fd, &newline, 1) != 1) {
                nc_strlcpy(out, "error: failed to finalize patch", out_cap);
                goto cleanup;
            }
        }
    }

    if (close(fd) != 0) {
        nc_strlcpy(out, "error: failed to finalize patch", out_cap);
        goto cleanup;
    }
    fd = -1;

    {
        int status;
        if (!run_patch_command(workspace, tmp_template, out, out_cap, &status)) {
            goto cleanup;
        }

        if (unlink(tmp_template) != 0)
            tool_debug("apply_patch", "failed to remove temp patch file '%s': %s", tmp_template, strerror(errno));
        tmp_template[0] = '\0';

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (!WIFEXITED(status)) {
                if (WIFSIGNALED(status)) {
                    char suffix[64];
                    snprintf(suffix,
                             sizeof(suffix),
                             "\n[terminated by signal: %d]",
                             WTERMSIG(status));
                    append_suffix(out, out_cap, suffix);
                } else {
                    append_suffix(out, out_cap, "\n[process terminated abnormally]");
                }
            } else if (WEXITSTATUS(status) == 127 && !out[0]) {
                nc_strlcpy(out, "error: patch utility not found", out_cap);
            }

            if (!out[0])
                nc_strlcpy(out, "error: patch failed", out_cap);
            goto cleanup;
        }
    }

    if (!out[0])
        nc_strlcpy(out, "Patch applied successfully", out_cap);

    tool_debug("apply_patch", "patch applied successfully, output size: %zu bytes", strlen(out));
    ok = true;

cleanup:
    if (fd >= 0)
        close(fd);
    if (tmp_template[0])
        unlink(tmp_template);
    free(normalized_patch);
    free(patch_input);
    return ok;
}

nc_tool nc_tool_apply_patch(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "apply_patch",
            .description = "Apply a unified diff patch.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"patch\":{\"type\":\"string\",\"description\":\"Unified diff patch text\"}},\"required\":[\"patch\"]}",
        },
        .ctx = (void *)cfg,
        .execute = apply_patch_execute,
        .free = NULL,
    };
}

/* ── Web fetch tool ───────────────────────────────────────────── */

#define NC_WEB_FETCH_MAX_BODY      (2 * 1024 * 1024)
#define NC_WEB_FETCH_DEFAULT_CHARS 20000
#define NC_WEB_FETCH_MAX_REDIRECTS 5

/* Bounded text writer with HTML-style whitespace collapsing. */
typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    int    pending_nl;    /* newlines owed before the next visible char */
    bool   pending_space;
    bool   pre;           /* inside <pre>: keep whitespace */
} text_out;

static void to_raw(text_out *t, const char *s, size_t n) {
    if (t->len + n >= t->cap)
        return;
    memcpy(t->buf + t->len, s, n);
    t->len += n;
    t->buf[t->len] = '\0';
}

static void to_flush(text_out *t) {
    if (t->len > 0 && t->pending_nl > 0) {
        to_raw(t, "\n\n", (size_t)(t->pending_nl > 2 ? 2 : t->pending_nl));
    } else if (t->len > 0 && t->pending_space && t->buf[t->len - 1] != '\n') {
        to_raw(t, " ", 1);
    }
    t->pending_nl = 0;
    t->pending_space = false;
}

static void to_text(text_out *t, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') {
            if (t->pre && c == '\n') {
                t->pending_space = false;
                to_raw(t, "\n", 1);
            } else if (t->pre && c != '\r') {
                to_raw(t, " ", 1);
            } else {
                t->pending_space = true;
            }
            continue;
        }
        to_flush(t);
        to_raw(t, &c, 1);
    }
}

static void to_break(text_out *t, int lines) {
    if (t->pending_nl < lines)
        t->pending_nl = lines;
}

static size_t utf8_encode(unsigned long cp, char *out) {
    if (cp == 0 || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        cp = 0xfffd;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

/* Decode an entity starting at s[0] == '&'. Writes UTF-8 to out and returns
 * the number of input bytes consumed, or 0 if it is not an entity. */
static size_t decode_entity(const char *s, size_t len, char *out, size_t *out_len) {
    static const struct { const char *name; unsigned long cp; } named[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026},
        {"copy", 0xa9}, {"reg", 0xae}, {"lsquo", 0x2018}, {"rsquo", 0x2019},
        {"ldquo", 0x201c}, {"rdquo", 0x201d}, {"middot", 0xb7}, {"bull", 0x2022},
    };
    size_t semi = 1;
    while (semi < len && semi < 12 && s[semi] != ';')
        semi++;
    if (semi >= len || s[semi] != ';' || semi < 2)
        return 0;

    if (s[1] == '#') {
        char *end;
        unsigned long cp = (s[2] == 'x' || s[2] == 'X')
            ? strtoul(s + 3, &end, 16) : strtoul(s + 2, &end, 10);
        if (end != s + semi)
            return 0;
        *out_len = utf8_encode(cp, out);
        return semi + 1;
    }
    for (size_t i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        size_t nl = strlen(named[i].name);
        if (nl == semi - 1 && strncmp(s + 1, named[i].name, nl) == 0) {
            *out_len = utf8_encode(named[i].cp, out);
            return semi + 1;
        }
    }
    return 0;
}

/* Decode entities in s[0..n) and feed the result to the writer. */
static void to_html_text(text_out *t, const char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (s[i] == '&') {
            char enc[4];
            size_t enc_len = 0;
            size_t used = decode_entity(s + i, n - i, enc, &enc_len);
            if (used) {
                if (enc_len == 1 && enc[0] == ' ')
                    to_text(t, " ", 1);
                else {
                    to_flush(t);
                    to_raw(t, enc, enc_len);
                }
                i += used;
                continue;
            }
        }
        size_t j = i + 1;
        while (j < n && s[j] != '&')
            j++;
        to_text(t, s + i, j - i);
        i = j;
    }
}

static const char *find_ci(const char *s, const char *end, const char *needle) {
    size_t nl = strlen(needle);
    for (const char *p = s; p + nl <= end; p++) {
        if (strncasecmp(p, needle, nl) == 0)
            return p;
    }
    return NULL;
}

/* Find the '>' that closes a tag, skipping quoted attribute values. */
static const char *tag_end(const char *p, const char *end) {
    char quote = 0;
    for (; p < end; p++) {
        if (quote) {
            if (*p == quote) quote = 0;
        } else if (*p == '"' || *p == '\'') {
            quote = *p;
        } else if (*p == '>') {
            return p;
        }
    }
    return NULL;
}

/* Copy an attribute value (e.g. href) from a tag's text into out. */
static void tag_attr(const char *tag, const char *end, const char *attr,
                     char *out, size_t out_cap) {
    size_t al = strlen(attr);
    out[0] = '\0';
    for (const char *p = tag; p + al < end; p++) {
        if (strncasecmp(p, attr, al) != 0 || (p > tag && !isspace((unsigned char)p[-1])))
            continue;
        const char *v = p + al;
        while (v < end && isspace((unsigned char)*v)) v++;
        if (v >= end || *v != '=') continue;
        v++;
        while (v < end && isspace((unsigned char)*v)) v++;
        char quote = (v < end && (*v == '"' || *v == '\'')) ? *v++ : 0;
        size_t n = 0;
        while (v + n < end && (quote ? v[n] != quote : !isspace((unsigned char)v[n])))
            n++;
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        return;
    }
}

static bool tag_in(const char *name, const char *const *list) {
    for (; *list; list++) {
        if (strcmp(name, *list) == 0)
            return true;
    }
    return false;
}

/* Convert HTML to readable plain text: drops scripts/styles/comments, turns
 * block elements into line breaks, headings into "# ", list items into
 * "- ", appends link targets as " (url)", decodes entities and collapses
 * whitespace. Returns a malloc'd string, or NULL on allocation failure. */
static char *html_to_text(const char *html, size_t len) {
    static const char *const skip_tags[] = {
        "script", "style", "noscript", "template", "svg", "iframe", "object", NULL
    };
    static const char *const para_tags[] = {
        "p", "table", "blockquote", "pre", "section", "article", "header",
        "footer", "main", "nav", "aside", "form", "figure", "details", "ul", "ol", "dl", NULL
    };
    static const char *const line_tags[] = {
        "br", "div", "tr", "hr", "dt", "dd", "figcaption", "summary", "li",
        "h1", "h2", "h3", "h4", "h5", "h6", NULL
    };
    const char *p = html;
    const char *end = html + len;
    char title[512] = "";
    char href[512] = "";
    size_t link_text_start = 0;
    text_out t = { 0 };

    t.cap = len + 1024;
    t.buf = (char *)malloc(t.cap);
    if (!t.buf)
        return NULL;
    t.buf[0] = '\0';

    while (p < end) {
        if (*p != '<') {
            const char *lt = memchr(p, '<', (size_t)(end - p));
            const char *stop = lt ? lt : end;
            to_html_text(&t, p, (size_t)(stop - p));
            p = stop;
            continue;
        }

        if (end - p >= 4 && strncmp(p, "<!--", 4) == 0) {
            const char *c = find_ci(p + 4, end, "-->");
            p = c ? c + 3 : end;
            continue;
        }
        if (p + 1 < end && (p[1] == '!' || p[1] == '?')) {
            const char *gt = memchr(p, '>', (size_t)(end - p));
            p = gt ? gt + 1 : end;
            continue;
        }

        bool closing = (p + 1 < end && p[1] == '/');
        const char *n = p + (closing ? 2 : 1);
        char name[16];
        size_t nl = 0;
        while (n + nl < end && isalnum((unsigned char)n[nl]) && nl < sizeof(name) - 1) {
            name[nl] = (char)tolower((unsigned char)n[nl]);
            nl++;
        }
        name[nl] = '\0';
        if (nl == 0) {
            /* A bare '<' in text */
            to_text(&t, "<", 1);
            p++;
            continue;
        }
        const char *gt = tag_end(n + nl, end);
        if (!gt)
            break;

        if (!closing && (tag_in(name, skip_tags) || strcmp(name, "title") == 0)) {
            char close_tag[24];
            snprintf(close_tag, sizeof(close_tag), "</%s", name);
            const char *c = find_ci(gt + 1, end, close_tag);
            if (strcmp(name, "title") == 0 && c && !title[0]) {
                text_out tt = { .buf = title, .cap = sizeof(title) };
                to_html_text(&tt, gt + 1, (size_t)(c - (gt + 1)));
            }
            if (!c) {
                p = end;
                continue;
            }
            const char *cgt = memchr(c, '>', (size_t)(end - c));
            p = cgt ? cgt + 1 : end;
            continue;
        }

        if (tag_in(name, para_tags))
            to_break(&t, 2);
        else if (tag_in(name, line_tags))
            to_break(&t, 1);

        if (!closing && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
            to_break(&t, 2);
            to_flush(&t);
            for (int i = 0; i < name[1] - '0'; i++)
                to_raw(&t, "#", 1);
            to_raw(&t, " ", 1);
        } else if (closing && name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
            to_break(&t, 2);
        } else if (!closing && strcmp(name, "li") == 0) {
            to_flush(&t);
            to_raw(&t, "- ", 2);
        } else if (strcmp(name, "pre") == 0) {
            t.pre = !closing;
        } else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
            t.pending_space = true;
        } else if (strcmp(name, "a") == 0) {
            if (!closing) {
                char raw_href[512];
                text_out ht = { .buf = href, .cap = sizeof(href) };
                tag_attr(n + nl, gt, "href", raw_href, sizeof(raw_href));
                href[0] = '\0';
                to_html_text(&ht, raw_href, strlen(raw_href)); /* decode &amp; etc. */
                link_text_start = t.len;
            } else if (href[0] && href[0] != '#' &&
                       strncasecmp(href, "javascript:", 11) != 0 &&
                       strncasecmp(href, "mailto:", 7) != 0) {
                /* Skip when the link text already is the URL. */
                size_t text_len = t.len - link_text_start;
                if (!(text_len == strlen(href) &&
                      memcmp(t.buf + link_text_start, href, text_len) == 0)) {
                    to_raw(&t, " (", 2);
                    to_raw(&t, href, strlen(href));
                    to_raw(&t, ")", 1);
                }
                href[0] = '\0';
            }
        } else if (strcmp(name, "img") == 0 && !closing) {
            char alt[256];
            tag_attr(n + nl, gt, "alt", alt, sizeof(alt));
            if (alt[0]) {
                to_text(&t, "[image: ", 8);
                to_html_text(&t, alt, strlen(alt));
                to_text(&t, "]", 1);
            }
        }

        p = gt + 1;
    }

    if (title[0]) {
        size_t tl = strlen(title);
        char *withtitle = (char *)malloc(t.len + tl + 16);
        if (withtitle) {
            int w = snprintf(withtitle, t.len + tl + 16, "Title: %s\n\n%s", title, t.buf);
            (void)w;
            free(t.buf);
            return withtitle;
        }
    }
    return t.buf;
}

/* Resolve a redirect Location against the URL that produced it. */
static bool resolve_url(const char *base, const char *loc, char *out, size_t out_cap) {
    const char *scheme_end = strstr(base, "://");
    if (!scheme_end)
        return false;

    if (strncasecmp(loc, "http://", 7) == 0 || strncasecmp(loc, "https://", 8) == 0)
        return nc_strlcpy(out, loc, out_cap) < out_cap;

    if (loc[0] == '/' && loc[1] == '/')
        return snprintf(out, out_cap, "%.*s:%s",
                        (int)(scheme_end - base), base, loc) < (int)out_cap;

    const char *host = scheme_end + 3;
    const char *path = strchr(host, '/');
    size_t origin_len = path ? (size_t)(path - base) : strlen(base);

    if (loc[0] == '/')
        return snprintf(out, out_cap, "%.*s%s", (int)origin_len, base, loc) < (int)out_cap;

    /* Relative path: replace everything after the last '/' of the base path
     * (ignoring any query string). */
    size_t dir_len = origin_len;
    if (path) {
        const char *q = strpbrk(path, "?#");
        const char *path_end = q ? q : path + strlen(path);
        for (const char *s = path; s < path_end; s++) {
            if (*s == '/')
                dir_len = (size_t)(s - base) + 1;
        }
    }
    return snprintf(out, out_cap, "%.*s%s%s", (int)dir_len, base,
                    path ? "" : "/", loc) < (int)out_cap;
}

static bool content_type_is(const char *ct, const char *what) {
    char lower[128];
    size_t i;
    for (i = 0; ct[i] && i < sizeof(lower) - 1; i++)
        lower[i] = (char)tolower((unsigned char)ct[i]);
    lower[i] = '\0';
    return strstr(lower, what) != NULL;
}

static bool looks_like_html(const char *body, size_t len) {
    size_t n = len < 1024 ? len : 1024;
    return find_ci(body, body + n, "<!doctype html") || find_ci(body, body + n, "<html");
}

static bool looks_binary(const char *body, size_t len) {
    size_t n = len < 4096 ? len : 4096;
    return memchr(body, '\0', n) != NULL;
}

static bool web_fetch_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char url[2048];
    char next[2048];
    long max_chars = NC_WEB_FETCH_DEFAULT_CHARS;
    nc_http_response resp;
    nc_arena a;
    bool ok = false;
    char *text = NULL;

    if (out_cap < 256)
        return false;
    out[0] = '\0';
    memset(&resp, 0, sizeof(resp));

    nc_arena_init(&a, strlen(args_json) * 2 + 256);
    nc_json *root = nc_json_parse(&a, args_json, strlen(args_json));
    nc_json *uv = root ? nc_json_get(root, "url") : NULL;
    if (!uv || uv->type != NC_JSON_STRING || uv->string.len == 0) {
        nc_strlcpy(out, root ? "error: missing 'url' argument" : "error: invalid JSON arguments",
                   out_cap);
        nc_arena_free(&a);
        return false;
    }
    if (uv->string.len >= sizeof(url)) {
        nc_strlcpy(out, "error: 'url' argument too long", out_cap);
        nc_arena_free(&a);
        return false;
    }
    memcpy(url, uv->string.ptr, uv->string.len);
    url[uv->string.len] = '\0';
    max_chars = (long)nc_json_num(nc_json_get(root, "max_chars"), (double)max_chars);
    nc_arena_free(&a);

    if (max_chars < 100)
        max_chars = 100;
    if ((size_t)max_chars > out_cap - 512)
        max_chars = (long)(out_cap - 512);

    const char *headers[] = {
        "User-Agent: noclaw/" NC_VERSION " (+https://github.com/noclaw/noclaw)",
        "Accept: text/html,text/plain,application/json,application/xml;q=0.9,*/*;q=0.5",
        "Accept-Encoding: identity",
    };
    nc_http_get_opts opts = {
        .max_body = NC_WEB_FETCH_MAX_BODY,
        .block_private = !cfg->web_fetch_allow_private,
    };

    for (int hop = 0; ; hop++) {
        char *frag = strchr(url, '#');
        if (frag)
            *frag = '\0';
        if (strncasecmp(url, "http://", 7) != 0 && strncasecmp(url, "https://", 8) != 0) {
            snprintf(out, out_cap, "error: only http:// and https:// URLs are supported: %s", url);
            return false;
        }

        tool_debug("web_fetch", "GET %s", url);
        if (!nc_http_get_ex(url, headers, 3, &opts, &resp)) {
            if (resp.blocked)
                snprintf(out, out_cap,
                         "error: refusing to fetch %s: it resolves to a private or local address "
                         "(set autonomy.web_fetch_allow_private to allow)", url);
            else
                snprintf(out, out_cap, "error: request to %s failed (DNS, connection, TLS or timeout)", url);
            nc_http_response_free(&resp);
            return false;
        }

        bool redirect = (resp.status == 301 || resp.status == 302 || resp.status == 303 ||
                         resp.status == 307 || resp.status == 308) && resp.location[0];
        if (!redirect)
            break;
        if (hop >= NC_WEB_FETCH_MAX_REDIRECTS) {
            snprintf(out, out_cap, "error: too many redirects (last: %s)", url);
            nc_http_response_free(&resp);
            return false;
        }
        if (!resolve_url(url, resp.location, next, sizeof(next))) {
            snprintf(out, out_cap, "error: invalid redirect location: %s", resp.location);
            nc_http_response_free(&resp);
            return false;
        }
        nc_http_response_free(&resp);
        nc_strlcpy(url, next, sizeof(url));
    }

    const char *body = resp.body ? resp.body : "";
    size_t body_len = resp.body ? resp.body_len : 0;
    bool is_html = content_type_is(resp.content_type, "html") ||
                   (!resp.content_type[0] && looks_like_html(body, body_len));
    bool is_text = is_html ||
                   content_type_is(resp.content_type, "text/") ||
                   content_type_is(resp.content_type, "json") ||
                   content_type_is(resp.content_type, "xml") ||
                   content_type_is(resp.content_type, "javascript") ||
                   (!resp.content_type[0] && !looks_binary(body, body_len));

    int hdr = snprintf(out, out_cap, "URL: %s\nStatus: %d\nContent-Type: %s\n\n",
                       url, resp.status, resp.content_type[0] ? resp.content_type : "unknown");
    if (hdr < 0 || (size_t)hdr >= out_cap)
        hdr = 0;

    if (!is_text) {
        snprintf(out + hdr, out_cap - (size_t)hdr,
                 "error: unsupported content type (binary, %zu bytes%s); use shell tools to download it",
                 body_len, resp.truncated ? "+" : "");
        goto done;
    }

    if (is_html) {
        text = html_to_text(body, body_len);
        if (!text) {
            nc_strlcpy(out, "error: out of memory", out_cap);
            goto done;
        }
        body = text;
        body_len = strlen(text);
    }

    {
        size_t room = out_cap - (size_t)hdr - 128;
        size_t limit = (size_t)max_chars < room ? (size_t)max_chars : room;
        size_t n = nc_utf8_safe_prefix(body, body_len, limit);
        if (n == 0 && body_len > 0 && limit > 0) {
            /* Not valid UTF-8 at the start: copy raw bytes; the provider
             * replaces malformed sequences when serializing. */
            n = body_len < limit ? body_len : limit;
        }
        memcpy(out + hdr, body, n);
        out[hdr + n] = '\0';
        if (n < body_len || resp.truncated) {
            char note[96];
            snprintf(note, sizeof(note),
                     "\n[content truncated: showing %zu of %zu%s characters; raise max_chars for more]",
                     n, body_len, resp.truncated ? "+" : "");
            append_suffix(out, out_cap, note);
        }
    }
    ok = resp.status >= 200 && resp.status < 400;

done:
    free(text);
    nc_http_response_free(&resp);
    return ok;
}

nc_tool nc_tool_web_fetch(const nc_config *cfg) {
    return (nc_tool){
        .def = {
            .name = "web_fetch",
            .description = "Fetch a web page or text resource over HTTP(S) and return it as readable text. HTML is converted to plain text with link targets in parentheses.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"url\":{\"type\":\"string\",\"description\":\"http:// or https:// URL to fetch\"},\"max_chars\":{\"type\":\"integer\",\"description\":\"Maximum characters of content to return (default 20000)\"}},\"required\":[\"url\"]}",
        },
        .ctx = (void *)cfg,
        .execute = web_fetch_execute,
        .free = NULL,
    };
}

/* ── Memory store tool ────────────────────────────────────────── */

static bool memory_store_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    nc_memory *mem = (nc_memory *)self->ctx;

    tool_debug("memory_store", "request received");

    if (!mem) {
        tool_debug("memory_store", "memory backend not configured");
        nc_strlcpy(out, "error: memory not configured", out_cap);
        return false;
    }

    char *key = extract_json_string_dup(args_json, "key");
    char *content = extract_json_string_dup(args_json, "content");
    if (!key) {
        tool_debug("memory_store", "missing 'key' argument");
        nc_strlcpy(out, "error: missing 'key' argument", out_cap);
        free(content);
        return false;
    }
    if (!content) {
        tool_debug("memory_store", "missing 'content' argument for key '%s'", key);
        nc_strlcpy(out, "error: missing 'content' argument", out_cap);
        free(key);
        return false;
    }

    tool_debug("memory_store", "storing key '%s' with %zu bytes", key, strlen(content));

    if (mem->store(mem, key, content)) {
        tool_debug("memory_store", "stored key '%s'", key);
        snprintf(out, out_cap, "Stored memory: %s", key);
        free(key);
        free(content);
        return true;
    }
    tool_debug("memory_store", "failed to store key '%s'", key);
    nc_strlcpy(out, "error: failed to store memory", out_cap);
    free(key);
    free(content);
    return false;
}

nc_tool nc_tool_memory_store(void *mem_ctx) {
    return (nc_tool){
        .def = {
            .name = "memory_store",
            .description = "Store a piece of information in long-term memory.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\",\"description\":\"Memory key\"},\"content\":{\"type\":\"string\",\"description\":\"Content to remember\"}},\"required\":[\"key\",\"content\"]}",
        },
        .ctx = mem_ctx,
        .execute = memory_store_execute,
        .free = NULL,
    };
}

/* ── Memory recall tool ───────────────────────────────────────── */

static bool memory_recall_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    nc_memory *mem = (nc_memory *)self->ctx;

    tool_debug("memory_recall", "request received");

    if (!mem) {
        tool_debug("memory_recall", "memory backend not configured");
        nc_strlcpy(out, "error: memory not configured", out_cap);
        return false;
    }

    char *query = extract_json_string_dup(args_json, "query");
    if (!query) {
        tool_debug("memory_recall", "missing 'query' argument");
        nc_strlcpy(out, "error: missing 'query' argument", out_cap);
        return false;
    }

    tool_debug("memory_recall", "recalling with query: %s", query);

    if (mem->recall(mem, query, out, out_cap)) {
        tool_debug("memory_recall", "recall returned success");
        free(query);
        return true;
    }
    tool_debug("memory_recall", "no matches found for query: %s", query);
    nc_strlcpy(out, "No matching memories found.", out_cap);
    free(query);
    return true;
}

nc_tool nc_tool_memory_recall(void *mem_ctx) {
    return (nc_tool){
        .def = {
            .name = "memory_recall",
            .description = "Search long-term memory for relevant information.",
            .parameters_json =
                "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search query\"}},\"required\":[\"query\"]}",
        },
        .ctx = mem_ctx,
        .execute = memory_recall_execute,
        .free = NULL,
    };
}

#ifdef NC_TEST
static bool html_case(const char *html, const char *expect) {
    char *t = html_to_text(html, strlen(html));
    bool ok = t && strcmp(t, expect) == 0;
    if (!ok)
        fprintf(stderr, "  html_to_text(%s)\n    got:    [%s]\n    expect: [%s]\n",
                html, t ? t : "(null)", expect);
    free(t);
    return ok;
}

static bool resolve_case(const char *base, const char *loc, const char *expect) {
    char out[512];
    return resolve_url(base, loc, out, sizeof(out)) && strcmp(out, expect) == 0;
}

void nc_test_web_fetch(void) {
    NC_ASSERT(html_case("<p>Hello <b>world</b></p><p>Second</p>", "Hello world\n\nSecond"),
              "html: paragraphs and inline tags");
    NC_ASSERT(html_case("<html><head><title>T &amp; U</title><style>p{x:1}</style>"
                        "<script>var a='<p>';</script></head><body>Body</body></html>",
                        "Title: T & U\n\nBody"),
              "html: title extracted, script/style dropped");
    NC_ASSERT(html_case("<h2>Intro</h2><ul><li>one</li><li>two</li></ul>",
                        "## Intro\n\n- one\n- two"),
              "html: headings and list items");
    NC_ASSERT(html_case("See <a href=\"/docs?a=1&amp;b=2\">the docs</a>.",
                        "See the docs (/docs?a=1&b=2)."),
              "html: link targets appended");
    NC_ASSERT(html_case("<a href=\"https://x.org\">https://x.org</a> <a href=\"#top\">top</a>",
                        "https://x.org top"),
              "html: redundant and fragment links not repeated");
    NC_ASSERT(html_case("a &lt;b&gt; &#233; &#x1F44B; &nbsp;c &bogus; &", "a <b> \xc3\xa9 \xf0\x9f\x91\x8b c &bogus; &"),
              "html: entities decoded");
    NC_ASSERT(html_case("<!-- hidden --><pre>a  b\n  c</pre>x", "a  b\n  c\n\nx"),
              "html: comments dropped, pre keeps whitespace");
    NC_ASSERT(html_case("1 < 2 and <img src=x alt=\"a cat\"> ok", "1 < 2 and [image: a cat] ok"),
              "html: bare '<' and image alt text");
    NC_ASSERT(html_case("<div title=\"a>b\">x</div>", "x"),
              "html: '>' inside quoted attribute");

    NC_ASSERT(resolve_case("https://a.com/x/y?q=1", "https://b.com/z", "https://b.com/z"),
              "redirect: absolute URL");
    NC_ASSERT(resolve_case("https://a.com/x/y", "//c.com/p", "https://c.com/p"),
              "redirect: protocol-relative");
    NC_ASSERT(resolve_case("https://a.com/x/y?q=1", "/root", "https://a.com/root"),
              "redirect: absolute path");
    NC_ASSERT(resolve_case("https://a.com/x/y?q=/no", "z", "https://a.com/x/z"),
              "redirect: relative path ignores query");
    NC_ASSERT(resolve_case("https://a.com", "z", "https://a.com/z"),
              "redirect: relative path from bare host");
    NC_ASSERT(resolve_case("http://a.com:8080/docs", "/docs/", "http://a.com:8080/docs/"),
              "redirect: keeps port");
}

void nc_test_tools_utf8(void) {
    const char *json = "{\"content\":\"\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x91\x8b\"}";
    char full[16];
    char truncated[5];

    NC_ASSERT(extract_json_string2(json, "content", full, sizeof(full), NULL) == NC_EXTRACT_OK,
              "tool args extract UTF-8");
    NC_ASSERT(strcmp(full, "\xe4\xbd\xa0\xe5\xa5\xbd\xf0\x9f\x91\x8b") == 0,
              "tool args preserve UTF-8");
    NC_ASSERT(extract_json_string2(json, "content", truncated, sizeof(truncated), NULL) == NC_EXTRACT_TRUNCATED,
              "tool args truncate UTF-8");
    NC_ASSERT(strcmp(truncated, "\xe4\xbd\xa0") == 0,
              "tool args truncate at UTF-8 boundary");
}
#endif
