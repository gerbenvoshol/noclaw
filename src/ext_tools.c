/*
 * External tools: executables in the tools directory, described by a JSON
 * manifest, registered as first-class tools at startup.
 *
 *   <tools_dir>/<name>/tool.json  {"name", "description", "parameters", "timeout_seconds"}
 *   <tools_dir>/<name>/run        executable; tool arguments (JSON) on stdin,
 *                                 result on stdout, exit status 0 = success
 *
 * Tools run with the workspace as working directory and get NOCLAW_WORKSPACE
 * and NOCLAW_TOOL_DIR in their environment. They are trusted code installed
 * by the user: the shell tool's command filter does not apply to them.
 * Tools are only loaded from the tools directory (outside the workspace by
 * default), so the agent cannot install new tools for itself with file_write.
 */

#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NC_EXT_TOOLS_SCAN_MAX 128

typedef struct {
    char  name[65];
    char  dir[PATH_MAX];
    char  run[PATH_MAX];
    char *description;
    char *parameters;
    int   timeout_seconds;
    const nc_config *cfg;
} ext_tool_ctx;

static bool ext_name_valid(const char *name, size_t len) {
    if (len == 0 || len > 64)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static bool ext_tool_execute(nc_tool *self, const char *args_json, char *out, size_t out_cap) {
    ext_tool_ctx *ctx = (ext_tool_ctx *)self->ctx;
    char workspace[PATH_MAX];
    char *env_ws = NULL;
    char *env_dir = NULL;
    bool ok = false;

    if (out_cap == 0)
        return false;
    if (!realpath(ctx->cfg->workspace_dir, workspace)) {
        nc_strlcpy(out, "error: invalid workspace path", out_cap);
        return false;
    }

    env_ws = nc_format("NOCLAW_WORKSPACE=%s", workspace);
    env_dir = nc_format("NOCLAW_TOOL_DIR=%s", ctx->dir);
    if (!env_ws || !env_dir) {
        nc_strlcpy(out, "error: out of memory", out_cap);
        goto done;
    }

    {
        const char *argv[] = { ctx->run, NULL };
        const char *env[] = { env_ws, env_dir, NULL };
        const char *input = args_json && args_json[0] ? args_json : "{}";
        nc_proc_opts po = {
            .argv = argv,
            .cwd = workspace,
            .stdin_data = input,
            .stdin_len = strlen(input),
            .env = env,
            .timeout_seconds = ctx->timeout_seconds,
        };
        nc_proc_result pr;

        nc_log(NC_LOG_INFO, "[tool:%s] running %s", ctx->name, ctx->run);
        if (!nc_run_process(&po, out, out_cap, &pr))
            goto done;
        ok = nc_proc_annotate(&pr, po.timeout_seconds, out, out_cap);
        if (!ok && WIFEXITED(pr.status) && WEXITSTATUS(pr.status) == 127 && !pr.timed_out && !out[0])
            nc_strlcpy(out, "error: failed to execute tool (check the shebang line)", out_cap);
        if (ok && !out[0])
            nc_strlcpy(out, "(no output)", out_cap);
    }

done:
    free(env_ws);
    free(env_dir);
    return ok;
}

static void ext_tool_free(nc_tool *self) {
    ext_tool_ctx *ctx = (ext_tool_ctx *)self->ctx;
    if (ctx) {
        free(ctx->description);
        free(ctx->parameters);
        free(ctx);
    }
    self->ctx = NULL;
}

/* Load one tool from dir. Returns NULL and fills `why` on failure. */
static ext_tool_ctx *ext_tool_load_one(const nc_config *cfg, const char *dir,
                                       char *why, size_t why_cap) {
    char manifest_path[PATH_MAX];
    size_t mlen = 0;
    char *manifest = NULL;
    nc_arena a;
    ext_tool_ctx *ctx = NULL;
    struct stat st;

    if (!nc_path_join(manifest_path, sizeof(manifest_path), dir, "tool.json")) {
        nc_strlcpy(why, "path too long", why_cap);
        return NULL;
    }
    manifest = nc_read_file(manifest_path, &mlen);
    if (!manifest) {
        nc_strlcpy(why, "missing tool.json", why_cap);
        return NULL;
    }

    nc_arena_init(&a, mlen * 2 + 512);
    nc_json *root = nc_json_parse(&a, manifest, mlen);
    if (!root || root->type != NC_JSON_OBJECT) {
        nc_strlcpy(why, "tool.json is not a valid JSON object", why_cap);
        goto fail;
    }

    nc_json *name = nc_json_get(root, "name");
    nc_json *desc = nc_json_get(root, "description");
    nc_json *params = nc_json_get(root, "parameters");
    nc_json *timeout = nc_json_get(root, "timeout_seconds");

    if (!name || name->type != NC_JSON_STRING ||
        !ext_name_valid(name->string.ptr, name->string.len)) {
        nc_strlcpy(why, "\"name\" must be 1-64 characters of [A-Za-z0-9_-]", why_cap);
        goto fail;
    }
    if (!desc || desc->type != NC_JSON_STRING || desc->string.len == 0) {
        nc_strlcpy(why, "\"description\" is required", why_cap);
        goto fail;
    }
    if (params && params->type != NC_JSON_OBJECT) {
        nc_strlcpy(why, "\"parameters\" must be a JSON Schema object", why_cap);
        goto fail;
    }

    ctx = (ext_tool_ctx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        nc_strlcpy(why, "out of memory", why_cap);
        goto fail;
    }
    ctx->cfg = cfg;
    memcpy(ctx->name, name->string.ptr, name->string.len);
    ctx->name[name->string.len] = '\0';
    ctx->description = nc_strdup_n(desc->string.ptr, desc->string.len);
    if (params) {
        nc_str raw = nc_json_get_slice(root, params);
        ctx->parameters = nc_strdup_n(raw.ptr, raw.len);
    } else {
        ctx->parameters = nc_strdup("{\"type\":\"object\",\"properties\":{}}");
    }
    if (!ctx->description || !ctx->parameters) {
        nc_strlcpy(why, "out of memory", why_cap);
        goto fail;
    }

    ctx->timeout_seconds = cfg->shell_timeout_seconds > 0 ? cfg->shell_timeout_seconds : 60;
    if (timeout) {
        double t = nc_json_num(timeout, 0);
        if (t < 1 || t > 86400) {
            nc_strlcpy(why, "\"timeout_seconds\" must be between 1 and 86400", why_cap);
            goto fail;
        }
        ctx->timeout_seconds = (int)t;
    }

    if (!realpath(dir, ctx->dir) ||
        !nc_path_join(ctx->run, sizeof(ctx->run), ctx->dir, "run")) {
        nc_strlcpy(why, "cannot resolve tool directory", why_cap);
        goto fail;
    }
    if (stat(ctx->run, &st) != 0 || !S_ISREG(st.st_mode)) {
        nc_strlcpy(why, "missing executable 'run'", why_cap);
        goto fail;
    }
    if (access(ctx->run, X_OK) != 0) {
        nc_strlcpy(why, "'run' is not executable (chmod +x)", why_cap);
        goto fail;
    }

    nc_arena_free(&a);
    free(manifest);
    return ctx;

fail:
    if (ctx) {
        free(ctx->description);
        free(ctx->parameters);
        free(ctx);
    }
    nc_arena_free(&a);
    free(manifest);
    return NULL;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int nc_ext_tools_load(const nc_config *cfg, nc_tool *tools, int count, int max,
                      FILE *report) {
    char *names[NC_EXT_TOOLS_SCAN_MAX];
    int n = 0;
    DIR *d;
    struct dirent *e;

    if (!cfg->tools_dir[0])
        return count;
    d = opendir(cfg->tools_dir);
    if (!d) {
        if (report)
            fprintf(report, "  (no tools directory at %s)\n", cfg->tools_dir);
        return count;
    }
    while ((e = readdir(d)) && n < NC_EXT_TOOLS_SCAN_MAX) {
        if (e->d_name[0] == '.')
            continue;
        if (!(names[n] = nc_strdup(e->d_name)))
            break;
        n++;
    }
    closedir(d);

    /* Stable order keeps the tool list (and prompt caches) deterministic. */
    qsort(names, (size_t)n, sizeof(names[0]), cmp_str);

    for (int i = 0; i < n; i++) {
        char dir[PATH_MAX];
        char why[160] = "";
        struct stat st;
        ext_tool_ctx *ctx = NULL;

        if (!nc_path_join(dir, sizeof(dir), cfg->tools_dir, names[i]) ||
            stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
            goto next;

        if (count >= max) {
            nc_strlcpy(why, "tool limit reached (NC_MAX_TOOLS)", sizeof(why));
        } else if ((ctx = ext_tool_load_one(cfg, dir, why, sizeof(why)))) {
            for (int j = 0; j < count; j++) {
                if (strcmp(tools[j].def.name, ctx->name) == 0) {
                    snprintf(why, sizeof(why), "name '%s' is already used by another tool", ctx->name);
                    nc_tool tmp = { .ctx = ctx };
                    ext_tool_free(&tmp);
                    ctx = NULL;
                    break;
                }
            }
        }

        if (ctx) {
            tools[count++] = (nc_tool){
                .def = {
                    .name = ctx->name,
                    .description = ctx->description,
                    .parameters_json = ctx->parameters,
                },
                .ctx = ctx,
                .execute = ext_tool_execute,
                .free = ext_tool_free,
            };
            nc_log(NC_LOG_INFO, "External tool: %s (%s)", ctx->name, ctx->dir);
            if (report)
                fprintf(report, "  %-20s OK (%s)\n", ctx->name, ctx->dir);
        } else {
            nc_log(NC_LOG_WARN, "Skipping external tool %s: %s", dir, why);
            if (report)
                fprintf(report, "  %-20s SKIPPED: %s\n", names[i], why);
        }
next:
        free(names[i]);
    }
    return count;
}
