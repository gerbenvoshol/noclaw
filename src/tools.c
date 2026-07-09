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

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <limits.h>

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

    size_t cplen = s.len < out_cap - 1 ? s.len : out_cap - 1;
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

static bool shell_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    const nc_config *cfg = (const nc_config *)self->ctx;
    char *command = NULL;
    char *shell_cmd = NULL;
    char block_reason[128];
    size_t full_len = 0;
    nc_extract_status est;
    FILE *fp = NULL;
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

    fp = popen(shell_cmd, "r");
    if (!fp) {
        snprintf(out, out_cap,
                 "error: failed to execute command: %s",
                 strerror(errno));
        goto cleanup;
    }

    size_t total = 0;
    char buf[1024];
    while (fgets(buf, sizeof(buf), fp) && total < out_cap - 1) {
        size_t n = strlen(buf);
        if (total + n >= out_cap - 1)
            n = out_cap - 1 - total;
        memcpy(out + total, buf, n);
        total += n;
    }
    out[total] = '\0';
    if (!feof(fp))
        append_suffix(out, out_cap, "\n[output truncated]");

    int status = pclose(fp);
    fp = NULL;

    if (status == -1) {
        snprintf(out, out_cap,
                 "error: failed to close shell process: %s",
                 strerror(errno));
        goto cleanup;
    }

    if (!WIFEXITED(status)) {
        if (WIFSIGNALED(status)) {
            char suffix[64];
            int n = snprintf(suffix, sizeof(suffix),
                             "\n[terminated by signal: %d]",
                             WTERMSIG(status));
            if (n >= 0)
                append_suffix(out, out_cap, suffix);
        } else {
            append_suffix(out, out_cap, "\n[process terminated abnormally]");
        }
        goto cleanup;
    }

    if (WEXITSTATUS(status) != 0) {
        char suffix[64];
        int n = snprintf(suffix, sizeof(suffix), "\n[exit code: %d]", WEXITSTATUS(status));
        if (n >= 0)
            append_suffix(out, out_cap, suffix);
        goto cleanup;
    }

    ok = true;

cleanup:
    if (fp)
        pclose(fp);
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

    /* Security: workspace scoping */
    if (cfg->workspace_only) {
        char candidate[PATH_MAX];
        char resolved[PATH_MAX];
        char workspace[PATH_MAX];

        if (path[0] == '/') {
            if (nc_strlcpy(candidate, path, sizeof(candidate)) >= sizeof(candidate)) {
                nc_strlcpy(out, "error: path too long", out_cap);
                return false;
            }
        } else {
            if (!path_is_safe_relative(path)) {
                nc_strlcpy(out, "error: path traversal not allowed", out_cap);
                return false;
            }
            if (!nc_path_join(candidate, sizeof(candidate),
                              cfg->workspace_dir, path)) {
                nc_strlcpy(out, "error: path too long", out_cap);
                return false;
            }
        }

        if (!realpath(candidate, resolved)) {
            snprintf(out, out_cap,
                     "error: cannot resolve file '%s'", path);
            return false;
        }

        if (!realpath(cfg->workspace_dir, workspace)) {
            nc_strlcpy(out,
                       "error: invalid workspace path",
                       out_cap);
            return false;
        }

        if (!path_within_workspace(resolved, workspace)) {
            nc_strlcpy(out,
                       "error: path escapes workspace",
                       out_cap);
            return false;
        }

        if (nc_strlcpy(read_path, resolved, sizeof(read_path)) >= sizeof(read_path)) {
            nc_strlcpy(out, "error: path too long", out_cap);
            return false;
        }
    } else {
        if (path[0] == '/') {
            if (nc_strlcpy(read_path, path, sizeof(read_path)) >= sizeof(read_path)) {
                nc_strlcpy(out, "error: path too long", out_cap);
                return false;
            }
        } else {
            if (!path_is_safe_relative(path)) {
                nc_strlcpy(out, "error: path traversal not allowed", out_cap);
                return false;
            }
            if (nc_strlcpy(read_path, path, sizeof(read_path)) >= sizeof(read_path)) {
                nc_strlcpy(out, "error: path too long", out_cap);
                return false;
            }
        }
    }

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
