#include "nc.h"

#ifdef NC_TEST

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>

static void mkdtemp_or_die(char *tmpl)
{
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        exit(1);
    }
}

static void setup_cfg(nc_config *cfg, const char *workspace, bool workspace_only)
{
    memset(cfg, 0, sizeof(*cfg));
    nc_strlcpy(cfg->workspace_dir, workspace, sizeof(cfg->workspace_dir));
    cfg->workspace_only = workspace_only;
}

static void write_tool(const char *tools_dir, const char *dir, const char *manifest,
                       const char *script, bool executable)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", tools_dir, dir);
    mkdir(path, 0755);
    if (manifest) {
        snprintf(path, sizeof(path), "%s/%s/tool.json", tools_dir, dir);
        nc_write_file(path, manifest, strlen(manifest));
    }
    if (script) {
        snprintf(path, sizeof(path), "%s/%s/run", tools_dir, dir);
        nc_write_file(path, script, strlen(script));
        chmod(path, executable ? 0755 : 0644);
    }
}

static nc_tool *find_loaded(nc_tool *tools, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(tools[i].def.name, name) == 0)
            return &tools[i];
    return NULL;
}

static void test_external_tools(char *out, size_t out_cap)
{
    char ws[] = "/tmp/noclaw_extws_XXXXXX";
    char td[] = "/tmp/noclaw_exttools_XXXXXX";
    char real_ws[1024];
    nc_config cfg;
    nc_tool tools[NC_MAX_TOOLS];
    int n;

    mkdtemp_or_die(ws);
    mkdtemp_or_die(td);
    setup_cfg(&cfg, ws, true);
    nc_strlcpy(cfg.tools_dir, td, sizeof(cfg.tools_dir));
    NC_ASSERT(realpath(ws, real_ws) != NULL, "resolve ext test workspace");

    write_tool(td, "echo", "{\"name\":\"echo_args\",\"description\":\"Echo \\\"args\\\"\\nback\","
               "\"parameters\":{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"a\",\"b\"]}}}}",
               "#!/bin/sh\ncat\necho\necho \"ws=$NOCLAW_WORKSPACE\"\necho \"pwd=$(pwd -P)\"\n"
               "echo \"dir=$(basename \"$NOCLAW_TOOL_DIR\")\"\n", true);
    write_tool(td, "fail", "{\"name\":\"fails\",\"description\":\"Always fails\"}",
               "#!/bin/sh\necho broken >&2\nexit 3\n", true);
    write_tool(td, "slow", "{\"name\":\"slow\",\"description\":\"Sleeps\",\"timeout_seconds\":1}",
               "#!/bin/sh\nsleep 30\n", true);
    write_tool(td, "big", "{\"name\":\"roundtrip\",\"description\":\"Copies stdin to stdout\"}",
               "#!/bin/sh\nexec cat\n", true);
    write_tool(td, "badjson", "{not json", "#!/bin/sh\n", true);
    write_tool(td, "norun", "{\"name\":\"norun\",\"description\":\"x\"}", NULL, false);
    write_tool(td, "noexec", "{\"name\":\"noexec\",\"description\":\"x\"}", "#!/bin/sh\n", false);
    write_tool(td, "clash", "{\"name\":\"shell\",\"description\":\"x\"}", "#!/bin/sh\n", true);
    write_tool(td, "badname", "{\"name\":\"bad name\",\"description\":\"x\"}", "#!/bin/sh\n", true);
    write_tool(td, "nodesc", "{\"name\":\"nodesc\"}", "#!/bin/sh\n", true);
    write_tool(td, "badtimeout", "{\"name\":\"badtimeout\",\"description\":\"x\",\"timeout_seconds\":0}",
               "#!/bin/sh\n", true);

    tools[0] = nc_tool_shell(&cfg);
    n = nc_ext_tools_load(&cfg, tools, 1, NC_MAX_TOOLS, NULL);
    NC_ASSERT(n == 5, "external tools: 4 valid tools loaded next to shell");
    NC_ASSERT(strcmp(tools[0].def.name, "shell") == 0, "built-in shell keeps its slot");
    NC_ASSERT(!find_loaded(tools, n, "norun") && !find_loaded(tools, n, "noexec") &&
              !find_loaded(tools, n, "nodesc") && !find_loaded(tools, n, "badtimeout"),
              "external tools: invalid manifests are skipped");

    nc_tool *echo = find_loaded(tools, n, "echo_args");
    NC_ASSERT(echo != NULL, "external tool registered under manifest name");
    if (echo) {
        NC_ASSERT(strstr(echo->def.description, "\"args\"\nback") != NULL,
                  "external tool description is decoded");
        NC_ASSERT(strstr(echo->def.parameters_json, "\"enum\":[\"a\",\"b\"]") != NULL,
                  "external tool keeps its full parameter schema");
        NC_ASSERT(echo->execute(echo, "{\"mode\":\"a\"}", out, out_cap),
                  "external tool executes");
        NC_ASSERT(strstr(out, "{\"mode\":\"a\"}") != NULL, "external tool receives args on stdin");
        {
            char expect[1200];
            snprintf(expect, sizeof(expect), "ws=%s", real_ws);
            NC_ASSERT(strstr(out, expect) != NULL, "external tool gets NOCLAW_WORKSPACE");
            snprintf(expect, sizeof(expect), "pwd=%s", real_ws);
            NC_ASSERT(strstr(out, expect) != NULL, "external tool runs in the workspace");
        }
        NC_ASSERT(strstr(out, "dir=echo") != NULL, "external tool gets NOCLAW_TOOL_DIR");
    }

    nc_tool *fails = find_loaded(tools, n, "fails");
    NC_ASSERT(fails && !fails->execute(fails, "{}", out, out_cap), "failing external tool returns error");
    NC_ASSERT(strstr(out, "broken") && strstr(out, "[exit code: 3]"),
              "failing external tool reports stderr and exit code");

    nc_tool *slow = find_loaded(tools, n, "slow");
    {
        time_t t0 = time(NULL);
        NC_ASSERT(slow && !slow->execute(slow, "{}", out, out_cap), "slow external tool times out");
        NC_ASSERT(strstr(out, "timed out after 1 seconds") != NULL,
                  "external tool honours manifest timeout");
        NC_ASSERT(time(NULL) - t0 < 5, "external tool timeout is prompt");
    }

    nc_tool *rt = find_loaded(tools, n, "roundtrip");
    if (rt) {
        size_t big = 200 * 1024;
        char *args = (char *)malloc(big + 32);
        memcpy(args, "{\"data\":\"", 9);
        memset(args + 9, 'z', big);
        memcpy(args + 9 + big, "\"}", 3);
        NC_ASSERT(rt->execute(rt, args, out, out_cap), "external tool streams large stdin");
        NC_ASSERT(strlen(out) == big + 11, "large stdin round-trips intact");
        free(args);
    }

    {
        /* doctor-style report lists every tool with its status */
        char *rep = NULL;
        size_t rep_len = 0;
        FILE *f = open_memstream(&rep, &rep_len);
        nc_tool more[NC_MAX_TOOLS];
        more[0] = nc_tool_shell(&cfg);
        int m = nc_ext_tools_load(&cfg, more, 1, NC_MAX_TOOLS, f);
        fclose(f);
        NC_ASSERT(rep && strstr(rep, "already used") && strstr(rep, "not executable") &&
                  strstr(rep, "missing executable") && strstr(rep, "\"description\" is required"),
                  "external tool report explains skipped tools");
        for (int i = 1; i < m; i++) more[i].free(&more[i]);
        free(rep);
    }

    {
        nc_tool few[2];
        few[0] = nc_tool_shell(&cfg);
        NC_ASSERT(nc_ext_tools_load(&cfg, few, 1, 2, NULL) == 2,
                  "external tools respect the tool limit");
        few[1].free(&few[1]);
    }

    for (int i = 1; i < n; i++)
        tools[i].free(&tools[i]);

    {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s' '%s'", ws, td);
        NC_ASSERT(system(cmd) == 0, "clean up external tool test dirs");
    }
}

static void test_web_fetch_offline(char *out, size_t out_cap)
{
    nc_config cfg;
    setup_cfg(&cfg, "/tmp", true);
    nc_tool wf = nc_tool_web_fetch(&cfg);

    NC_ASSERT(!wf.execute(&wf, "{}", out, out_cap) && strstr(out, "missing 'url'"),
              "web_fetch requires url");
    NC_ASSERT(!wf.execute(&wf, "{\"url\":\"file:///etc/passwd\"}", out, out_cap) &&
              strstr(out, "only http"), "web_fetch rejects non-http schemes");
    NC_ASSERT(!wf.execute(&wf, "{\"url\":\"http://127.0.0.1:9/\"}", out, out_cap) &&
              strstr(out, "private or local"), "web_fetch blocks loopback by default");
    NC_ASSERT(!wf.execute(&wf, "{\"url\":\"http://localhost:9/\"}", out, out_cap) &&
              strstr(out, "private or local"), "web_fetch blocks names resolving to loopback");
    NC_ASSERT(!wf.execute(&wf, "{\"url\":\"http://169.254.169.254/latest/meta-data/\"}", out, out_cap) &&
              strstr(out, "private or local"), "web_fetch blocks cloud metadata address");

    cfg.web_fetch_allow_private = true;
    NC_ASSERT(!wf.execute(&wf, "{\"url\":\"http://127.0.0.1:9/\"}", out, out_cap) &&
              strstr(out, "request to") && !strstr(out, "private"),
              "web_fetch_allow_private lifts the block");
}

void nc_test_tools(void)
{
    char tmpl[] = "/tmp/noclaw_tools_XXXXXX";
    char out[NC_TOOL_RESULT_MAX];
    nc_config cfg;
    mkdtemp_or_die(tmpl);
    setup_cfg(&cfg, tmpl, true);

    nc_tool fr = nc_tool_file_read(&cfg);
    nc_tool fw = nc_tool_file_write(&cfg);
    nc_tool sh = nc_tool_shell(&cfg);
    nc_tool ap = nc_tool_apply_patch(&cfg);

    NC_ASSERT(fw.execute(&fw,
        "{\"path\":\"sub/a.txt\",\"content\":\"hello\\nworld\\n\"}",
        out, sizeof(out)),
        "file_write writes within workspace");

    NC_ASSERT(fw.execute(&fw,
        "{\"path\":\"sub/empty.txt\",\"content\":\"\"}",
        out, sizeof(out)),
        "file_write allows empty content");

    NC_ASSERT(access("/tmp", F_OK) == 0, "sanity");

    NC_ASSERT(fr.execute(&fr,
        "{\"path\":\"sub/a.txt\"}",
        out, sizeof(out)),
        "file_read reads within workspace");
    NC_ASSERT(strstr(out, "hello\\nworld\\n") != NULL,
        "file_read returns escaped content");

    NC_ASSERT(!fw.execute(&fw,
        "{\"path\":\"../escape.txt\",\"content\":\"x\"}",
        out, sizeof(out)),
        "file_write rejects traversal");

    NC_ASSERT(!fr.execute(&fr,
        "{\"path\":\"../escape.txt\"}",
        out, sizeof(out)),
        "file_read rejects traversal");

    NC_ASSERT(!fw.execute(&fw,
        "{\"path\":\"/tmp/escape.txt\",\"content\":\"x\"}",
        out, sizeof(out)),
        "file_write rejects absolute paths in workspace mode");

    NC_ASSERT(sh.execute(&sh,
        "{\"command\":\"pwd\"}",
        out, sizeof(out)),
        "shell executes command");
    NC_ASSERT(strstr(out, tmpl) != NULL,
        "shell runs inside workspace");

    NC_ASSERT(!sh.execute(&sh,
        "{\"command\":\"false\"}",
        out, sizeof(out)),
        "shell reports nonzero exit");
    NC_ASSERT(strstr(out, "exit code") != NULL,
        "shell includes exit code");

    NC_ASSERT(!sh.execute(&sh,
        "{\"command\":\"\"}",
        out, sizeof(out)),
        "shell rejects empty command");
    NC_ASSERT(strstr(out, "empty command") != NULL,
        "shell reports empty command explicitly");

    NC_ASSERT(sh.execute(&sh,
        "{\"command\":\"cat <<'EOF'\nhello\nEOF\"}",
        out, sizeof(out)),
        "shell supports heredoc commands");
    NC_ASSERT(strstr(out, "hello") != NULL,
        "shell captures heredoc output");

    NC_ASSERT(!sh.execute(&sh,
        "{\"command\":\"rm -rf /\"}",
        out, sizeof(out)),
        "shell rejects obviously dangerous command");
    NC_ASSERT(strstr(out, "unsafe command rejected") != NULL,
        "shell explains unsafe command rejection");

    {
        char bad_json[64];
        snprintf(bad_json, sizeof(bad_json), "{\"command\":\"bad%c\"}", 1);
        NC_ASSERT(!sh.execute(&sh, bad_json, out, sizeof(out)),
            "shell rejects command with control characters");
    }

    NC_ASSERT(ap.execute(&ap,
        "{\"patch\":\"diff --git a/sub/a.txt b/sub/a.txt\\n--- a/sub/a.txt\\n+++ b/sub/a.txt\\n@@ -1,2 +1,2 @@\\n hello\\n-world\\n+WORLD\\n\"}",
        out, sizeof(out)),
        "apply_patch modifies tracked file");

    NC_ASSERT(fr.execute(&fr,
        "{\"path\":\"sub/a.txt\"}",
        out, sizeof(out)),
        "file_read reads patched file");
    NC_ASSERT(strstr(out, "WORLD") != NULL,
        "patch updated content");

    NC_ASSERT(!ap.execute(&ap,
        "{\"patch\":\"diff --git a/../x b/../x\\n--- a/../x\\n+++ b/../x\\n@@ -0,0 +1 @@\\n+bad\\n\"}",
        out, sizeof(out)),
        "apply_patch rejects traversal target");


    NC_ASSERT(!ap.execute(&ap,
        "{\"patch\":\"diff --git a/sub/nohunk.txt b/sub/nohunk.txt\\n--- a/sub/nohunk.txt\\n+++ b/sub/nohunk.txt\\n\"}",
        out, sizeof(out)),
        "apply_patch rejects patch with no hunks");
    NC_ASSERT(strstr(out, "no hunks") != NULL,
        "apply_patch reports missing hunks");

    NC_ASSERT(!ap.execute(&ap,
        "{\"patch\":\"*** Begin Patch\\n*** Update File: sub/a.txt\\n@@ -1 +1 @@\\n-hello\\n+hi\\n\"}",
        out, sizeof(out)),
        "apply_patch rejects unterminated Begin Patch format");

    NC_ASSERT(ap.execute(&ap,
        "{\"patch\":\"*** Begin Patch\\n*** Update File: sub/a.txt\\n@@ -1,2 +1,2 @@\\n hello\\n-WORLD\\n+world-again\\n*** End Patch\\n\"}",
        out, sizeof(out)),
        "apply_patch accepts Begin Patch format");

    NC_ASSERT(fr.execute(&fr,
        "{\"path\":\"sub/a.txt\"}",
        out, sizeof(out)),
        "file_read reads Begin Patch update");
    NC_ASSERT(strstr(out, "world-again") != NULL,
        "Begin Patch conversion updated content");


    {
        size_t cmd_len = NC_SHELL_COMMAND_MAX + 32;
        char *json = (char *)malloc(cmd_len + 32);
        char *cmd = (char *)malloc(cmd_len + 1);
        memset(cmd, 'a', cmd_len);
        cmd[cmd_len] = '\0';
        snprintf(json, cmd_len + 32, "{\"command\":\"%s\"}", cmd);
        NC_ASSERT(!sh.execute(&sh, json, out, sizeof(out)),
            "shell rejects oversized command argument");
        NC_ASSERT(strstr(out, "too long") != NULL,
            "shell reports oversized command explicitly");
        free(json);
        free(cmd);
    }

    {
        size_t patch_len = NC_PATCH_INPUT_MAX + 32;
        char *json = (char *)malloc(patch_len + 32);
        char *patch = (char *)malloc(patch_len + 1);
        memset(patch, 'b', patch_len);
        patch[patch_len] = '\0';
        snprintf(json, patch_len + 32, "{\"patch\":\"%s\"}", patch);
        NC_ASSERT(!ap.execute(&ap, json, out, sizeof(out)),
            "apply_patch rejects oversized patch argument");
        NC_ASSERT(strstr(out, "too long") != NULL,
            "apply_patch reports oversized patch explicitly");
        free(json);
        free(patch);
    }

    {
        time_t t0 = time(NULL);
        cfg.shell_timeout_seconds = 1;
        NC_ASSERT(!sh.execute(&sh, "{\"command\":\"echo started; sleep 30\"}", out, sizeof(out)),
            "shell fails on timeout");
        NC_ASSERT(strstr(out, "started") != NULL,
            "shell keeps output produced before timeout");
        NC_ASSERT(strstr(out, "timed out after 1 seconds") != NULL,
            "shell reports timeout");
        NC_ASSERT(time(NULL) - t0 < 5, "shell timeout kills the command promptly");

        t0 = time(NULL);
        NC_ASSERT(!sh.execute(&sh, "{\"command\":\"sleep 30 | cat\"}", out, sizeof(out)),
            "shell times out on pipelines");
        NC_ASSERT(time(NULL) - t0 < 5, "shell timeout kills the whole pipeline");
        cfg.shell_timeout_seconds = 0;
    }

    NC_ASSERT(sh.execute(&sh, "{\"command\":\"cat; echo done\"}", out, sizeof(out)),
        "shell commands get empty stdin instead of blocking");
    NC_ASSERT(strstr(out, "done") != NULL, "shell stdin test output");

    NC_ASSERT(sh.execute(&sh,
        "{\"command\":\"head -c 1000000 /dev/zero | tr '\\\\0' a\"}",
        out, sizeof(out)),
        "shell handles output larger than the buffer");
    NC_ASSERT(strstr(out, "[output truncated]") != NULL,
        "shell marks oversized output as truncated");

    NC_ASSERT(sh.execute(&sh, "{\"command\":\"echo err >&2\"}", out, sizeof(out)) &&
              strstr(out, "err") != NULL,
        "shell captures stderr");

    test_external_tools(out, sizeof(out));
    test_web_fetch_offline(out, sizeof(out));

    unlink("/tmp/escape.txt");
}

#endif
