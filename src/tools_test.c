#include "nc.h"

#ifdef NC_TEST

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

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

    unlink("/tmp/escape.txt");
}

#endif
