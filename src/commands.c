/*
 * CLI command implementations: agent, gateway, status, onboard, doctor.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


/* ── Agent command ────────────────────────────────────────────── */

typedef struct nc_agent_opts {
    const char *single_msg;
    const char *message_file;
    const char *channel_name;
} nc_agent_opts;

static bool parse_agent_opts(int argc, char **argv, nc_config *cfg, nc_agent_opts *opts) {
    memset(opts, 0, sizeof(*opts));
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--message") == 0) && i + 1 < argc)
            opts->single_msg = argv[++i];
        else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) && i + 1 < argc)
            opts->message_file = argv[++i];
        else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--workspace") == 0) && i + 1 < argc)
            nc_strlcpy(cfg->workspace_dir, argv[++i], sizeof(cfg->workspace_dir));
        else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--instructions") == 0) && i + 1 < argc)
            nc_strlcpy(cfg->instructions_file, argv[++i], sizeof(cfg->instructions_file));
        else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc)
            opts->channel_name = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("noclaw agent [options]\n\n");
            printf("  -m, --message TEXT        Send a single message\n");
            printf("  -f, --file PATH           Send instructions/prompt from a file\n");
            printf("  -w, --workspace PATH      Use a separate workspace for this agent\n");
            printf("  -i, --instructions PATH   Use PATH as system instructions\n");
            printf("      --channel NAME        cli, telegram, discord, slack\n");
            return false;
        }
    }
    return true;
}

static const char *provider_is_local_name = "ollama";

static nc_provider make_provider(const nc_config *cfg) {
    if (strcmp(cfg->default_provider, "anthropic") == 0)
        return nc_provider_anthropic(cfg->api_key, cfg->api_url);
    if (strcmp(cfg->default_provider, "gemini") == 0)
        return nc_provider_gemini(cfg->api_key, cfg->api_url);
    return nc_provider_openai(cfg->api_key, cfg->api_url);
}

static void make_agent_stack(nc_config *cfg, nc_provider *prov, nc_memory *mem,
                             nc_tool *tools, int *tool_count, nc_agent *agent) {
    char mem_path[1024];
    nc_mkdir_p(cfg->workspace_dir);
    nc_path_join(mem_path, sizeof(mem_path), cfg->workspace_dir, "memories.tsv");
    *mem = nc_memory_flat(mem_path);

    *tool_count = 0;
    tools[(*tool_count)++] = nc_tool_shell(cfg);
    tools[(*tool_count)++] = nc_tool_apply_patch(cfg);
    tools[(*tool_count)++] = nc_tool_file_read(cfg);
    tools[(*tool_count)++] = nc_tool_file_write(cfg);
    tools[(*tool_count)++] = nc_tool_memory_store(mem);
    tools[(*tool_count)++] = nc_tool_memory_recall(mem);

    nc_agent_init(agent, cfg, prov, tools, *tool_count, mem);
}

int nc_cmd_agent(int argc, char **argv) {
    nc_config cfg;
    if (!nc_config_load(&cfg)) { fprintf(stderr, "No config found -- run `noclaw onboard` first\n"); return 1; }
    nc_agent_opts opts;
    if (!parse_agent_opts(argc, argv, &cfg, &opts)) return 0;
    bool is_local = strcmp(cfg.default_provider, provider_is_local_name) == 0;
    if (!cfg.api_key[0] && !is_local) { fprintf(stderr, "No API key configured. Run `noclaw onboard` first\n"); return 1; }

    nc_provider prov = make_provider(&cfg);
    nc_http_set_timeout(cfg.http_timeout_seconds);
    nc_memory mem;
    nc_tool tools[NC_MAX_TOOLS];
    int tool_count;
    nc_agent agent;
    make_agent_stack(&cfg, &prov, &mem, tools, &tool_count, &agent);

    char *file_msg = NULL;
    const char *single_msg = opts.single_msg;
    if (opts.message_file) {
        size_t file_len = 0;
        file_msg = nc_read_file(opts.message_file, &file_len);
        if (!file_msg) { fprintf(stderr, "Cannot read message file: %s\n", opts.message_file); nc_agent_free(&agent); mem.free(&mem); if (prov.free) prov.free(&prov); return 1; }
        single_msg = file_msg;
    }

    if (single_msg) {
        const char *reply = nc_agent_chat(&agent, single_msg);
        if (reply) printf("%s\n", reply);
    } else {
        nc_channel ch;
        if (opts.channel_name && strcmp(opts.channel_name, "telegram") == 0) {
            const char *tok = cfg.telegram_token[0] ? cfg.telegram_token : getenv("NOCLAW_TELEGRAM_TOKEN");
            if (!tok || !tok[0]) { fprintf(stderr, "No Telegram token. Set NOCLAW_TELEGRAM_TOKEN or config.\n"); return 1; }
            ch = nc_channel_telegram(tok); printf("noclaw v" NC_VERSION " -- telegram mode\n");
        } else if (opts.channel_name && strcmp(opts.channel_name, "discord") == 0) {
            const char *tok = cfg.discord_token[0] ? cfg.discord_token : getenv("NOCLAW_DISCORD_TOKEN");
            if (!tok || !tok[0]) { fprintf(stderr, "No Discord token. Set NOCLAW_DISCORD_TOKEN or config.\n"); return 1; }
            ch = nc_channel_discord(tok); printf("noclaw v" NC_VERSION " -- discord mode\n");
        } else if (opts.channel_name && strcmp(opts.channel_name, "slack") == 0) {
            const char *tok = cfg.slack_token[0] ? cfg.slack_token : getenv("NOCLAW_SLACK_TOKEN");
            if (!tok || !tok[0]) { fprintf(stderr, "No Slack token. Set NOCLAW_SLACK_TOKEN or config.\n"); return 1; }
            ch = nc_channel_slack(tok); printf("noclaw v" NC_VERSION " -- slack mode\n");
        } else { ch = nc_channel_cli(); printf("noclaw v" NC_VERSION " -- interactive mode (type /quit to exit, /new to reset)\n"); }
        printf("  Provider:  %s\n", cfg.default_provider);
        printf("  Model:     %s\n", cfg.default_model);
        printf("  Workspace: %s\n", cfg.workspace_dir);
        if (cfg.instructions_file[0]) printf("  Instr:     %s\n", cfg.instructions_file);
        printf("  Tools:     %d loaded\n\n", tool_count);
        nc_incoming_msg msg;
        while (ch.poll(&ch, &msg)) {
            if (strcmp(msg.content, "/quit") == 0 || strcmp(msg.content, "/exit") == 0) break;
            if (strcmp(msg.content, "/new") == 0 || strcmp(msg.content, "/reset") == 0) { nc_agent_reset(&agent); ch.send(&ch, msg.sender, "Session reset."); continue; }
            if (strcmp(msg.content, "/status") == 0) { char status_buf[1536]; snprintf(status_buf, sizeof(status_buf), "Model: %s | Messages: %d | Provider: %s | Workspace: %s", cfg.default_model, agent.message_count, cfg.default_provider, cfg.workspace_dir); ch.send(&ch, msg.sender, status_buf); continue; }
            if (strcmp(msg.content, "/help") == 0) { ch.send(&ch, msg.sender, "Commands: /new /status /quit /help"); continue; }
            const char *reply = nc_agent_chat(&agent, msg.content); if (reply) ch.send(&ch, msg.sender, reply);
        }
        ch.free(&ch);
    }
    nc_agent_free(&agent); mem.free(&mem); if (prov.free) prov.free(&prov); free(file_msg); return 0;
}

/* ── Gateway command ──────────────────────────────────────────── */

int nc_cmd_gateway(int argc, char **argv) {
    nc_config cfg;
    if (!nc_config_load(&cfg)) {
        fprintf(stderr, "No config found -- run `noclaw onboard` first\n");
        return 1;
    }

    /* Parse args */
    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i], "--port") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            cfg.gateway_port = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc)
            nc_strlcpy(cfg.gateway_host, argv[++i], sizeof(cfg.gateway_host));
        else if ((strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--workspace") == 0) && i + 1 < argc)
            nc_strlcpy(cfg.workspace_dir, argv[++i], sizeof(cfg.workspace_dir));
        else if ((strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--instructions") == 0) && i + 1 < argc)
            nc_strlcpy(cfg.instructions_file, argv[++i], sizeof(cfg.instructions_file));
    }

    nc_provider prov = make_provider(&cfg);
    nc_http_set_timeout(cfg.http_timeout_seconds);
    nc_memory mem;
    nc_tool tools[NC_MAX_TOOLS];
    int tool_count;
    nc_agent agent;
    make_agent_stack(&cfg, &prov, &mem, tools, &tool_count, &agent);

    nc_gateway gw;
    nc_gateway_init(&gw, &cfg, &agent);
    nc_gateway_run(&gw);

    nc_agent_free(&agent);
    mem.free(&mem);
    if (prov.free) prov.free(&prov);
    return 0;
}

/* ── Status command ───────────────────────────────────────────── */

int nc_cmd_status(int argc, char **argv) {
    (void)argc; (void)argv;
    nc_config cfg;
    bool loaded = nc_config_load(&cfg);

    printf("noclaw v" NC_VERSION "\n");
    printf("─────────────────────────────\n");
    printf("  Config:     %s\n", loaded ? cfg.config_path : "(not found)");
    printf("  Workspace:  %s\n", loaded ? cfg.workspace_dir : "(not configured)");
    printf("  Instr file: %s\n", (loaded && cfg.instructions_file[0]) ? cfg.instructions_file : "-");
    printf("  Provider:   %s\n", loaded ? cfg.default_provider : "-");
    printf("  Model:      %s\n", loaded ? cfg.default_model : "-");
    printf("  Gateway:    %s:%d\n", loaded ? cfg.gateway_host : "-", loaded ? cfg.gateway_port : 0);
    printf("  Memory:     %s\n", loaded ? cfg.memory_backend : "-");
    printf("  Runtime:    %s\n", loaded ? cfg.runtime_kind : "-");
    printf("  API Key:    %s\n", (loaded && cfg.api_key[0]) ? "configured" : "not set");
    printf("  Heartbeat:  %s\n", (loaded && cfg.heartbeat_enabled) ? "enabled" : "disabled");
    printf("  Sandbox:    %s\n", loaded ? cfg.sandbox_backend : "-");
    printf("  HTTP timeout: %ds\n", loaded ? cfg.http_timeout_seconds : 0);
    return 0;
}

/* ── Onboard command ──────────────────────────────────────────── */

/* Returns true if this provider does not require an API key. */
static bool provider_is_local(const char *name) {
    return strcmp(name, "ollama") == 0;
}

int nc_cmd_onboard(int argc, char **argv) {
    nc_config cfg;
    nc_config_defaults(&cfg);

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("noclaw onboard -- quick setup\n\n");
            printf("USAGE:\n");
            printf("  noclaw onboard [--provider PROVIDER] [--api-key KEY] [--base-url URL] [--model MODEL] [--max-tokens N] [--http-timeout N] [--workspace PATH] [--instructions PATH]\n\n");
            printf("OPTIONS:\n");
            printf("  --provider PROVIDER  openrouter, anthropic, openai, gemini, ollama\n");
            printf("  --api-key KEY        API key for remote providers\n");
            printf("  --base-url URL       OpenAI-compatible base URL\n");
            printf("  --model MODEL        Default model to use\n");
            printf("  --max-tokens N       Maximum completion tokens per request\n");
            printf("  --http-timeout N     HTTP socket timeout in seconds\n");
            printf("  --workspace PATH     Default workspace directory\n");
            printf("  --instructions PATH  Default system instructions file\n");
            printf("  -h, --help           Show this help\n\n");
            printf("If no provider/api key is passed, onboard runs interactively and asks for setup values.\n");
            return 0;
        }
    }

    /* Parse flags */
    const char *api_key = NULL;
    const char *provider = NULL;
    const char *base_url = NULL;
    const char *model = NULL;
    const char *max_tokens = NULL;
    const char *http_timeout = NULL;
    const char *workspace = NULL;
    const char *instructions = NULL;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--api-key") == 0 && i + 1 < argc)
            api_key = argv[++i];
        else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
            provider = argv[++i];
        else if (strcmp(argv[i], "--base-url") == 0 && i + 1 < argc)
            base_url = argv[++i];
        else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
            model = argv[++i];
        else if (strcmp(argv[i], "--max-tokens") == 0 && i + 1 < argc)
            max_tokens = argv[++i];
        else if (strcmp(argv[i], "--http-timeout") == 0 && i + 1 < argc)
            http_timeout = argv[++i];
        else if (strcmp(argv[i], "--workspace") == 0 && i + 1 < argc)
            workspace = argv[++i];
        else if (strcmp(argv[i], "--instructions") == 0 && i + 1 < argc)
            instructions = argv[++i];
    }

    if (provider)
        nc_strlcpy(cfg.default_provider, provider, sizeof(cfg.default_provider));
    if (api_key)
        nc_strlcpy(cfg.api_key, api_key, sizeof(cfg.api_key));
    if (base_url)
        nc_strlcpy(cfg.api_url, base_url, sizeof(cfg.api_url));
    if (model)
        nc_strlcpy(cfg.default_model, model, sizeof(cfg.default_model));
    if (max_tokens) {
        long mt = strtol(max_tokens, NULL, 10);
        if (mt > 0 && mt <= 10000000L)
            cfg.max_tokens = (int)mt;
    }
    if (http_timeout) {
        long t = strtol(http_timeout, NULL, 10);
        if (t >= 1 && t <= 3600L)
            cfg.http_timeout_seconds = (int)t;
    }
    if (workspace)
        nc_strlcpy(cfg.workspace_dir, workspace, sizeof(cfg.workspace_dir));
    if (instructions)
        nc_strlcpy(cfg.instructions_file, instructions, sizeof(cfg.instructions_file));

    /* Interactive mode when no provider-related flags were given on the CLI */
    if (!provider && !api_key) {
        printf("noclaw onboard -- quick setup\n\n");

        printf("Provider (openrouter/anthropic/openai/gemini/ollama) [openrouter]: ");
        fflush(stdout);
        char prov_buf[64];
        if (fgets(prov_buf, sizeof(prov_buf), stdin)) {
            size_t len = strlen(prov_buf);
            if (len > 0 && prov_buf[len - 1] == '\n') prov_buf[len - 1] = '\0';
            if (prov_buf[0])
                nc_strlcpy(cfg.default_provider, prov_buf, sizeof(cfg.default_provider));
        }

        if (!provider_is_local(cfg.default_provider)) {
            printf("API key: ");
            fflush(stdout);
            char key_buf[256];
            if (fgets(key_buf, sizeof(key_buf), stdin)) {
                size_t len = strlen(key_buf);
                if (len > 0 && key_buf[len - 1] == '\n') key_buf[len - 1] = '\0';
                nc_strlcpy(cfg.api_key, key_buf, sizeof(cfg.api_key));
            }
        }

        printf("Base URL (optional): ");
        fflush(stdout);
        char url_buf[256];
        if (fgets(url_buf, sizeof(url_buf), stdin)) {
            size_t len = strlen(url_buf);
            if (len > 0 && url_buf[len - 1] == '\n') url_buf[len - 1] = '\0';
            if (url_buf[0])
                nc_strlcpy(cfg.api_url, url_buf, sizeof(cfg.api_url));
        }

        printf("Model [%s]: ", cfg.default_model);
        fflush(stdout);
        char model_buf[128];
        if (fgets(model_buf, sizeof(model_buf), stdin)) {
            size_t len = strlen(model_buf);
            if (len > 0 && model_buf[len - 1] == '\n') model_buf[len - 1] = '\0';
            if (model_buf[0])
                nc_strlcpy(cfg.default_model, model_buf, sizeof(cfg.default_model));
        }


        printf("Max tokens [%d]: ", cfg.max_tokens);
        fflush(stdout);
        char max_tokens_buf[32];
        if (fgets(max_tokens_buf, sizeof(max_tokens_buf), stdin)) {
            size_t len = strlen(max_tokens_buf);
            if (len > 0 && max_tokens_buf[len - 1] == '\n') max_tokens_buf[len - 1] = '\0';
            if (max_tokens_buf[0]) {
                long mt = strtol(max_tokens_buf, NULL, 10);
                if (mt > 0 && mt <= 10000000L)
                    cfg.max_tokens = (int)mt;
            }
        }
    }

    /* Ollama defaults */
    if (strcmp(cfg.default_provider, "ollama") == 0) {
        if (!cfg.api_url[0])
            nc_strlcpy(cfg.api_url, "http://localhost:11434/v1", sizeof(cfg.api_url));
        if (!model)  /* --model not given on CLI; set an ollama-appropriate default */
            nc_strlcpy(cfg.default_model, "llama3.2", sizeof(cfg.default_model));
    }

    /* Ensure directories */
    nc_mkdir_p(cfg.config_dir);
    nc_mkdir_p(cfg.workspace_dir);

    /* Save config */
    if (nc_config_save(&cfg)) {
        printf("\nConfig saved to: %s\n", cfg.config_path);
        printf("Workspace:       %s\n", cfg.workspace_dir);
        printf("Provider:        %s\n", cfg.default_provider);
        printf("Model:           %s\n", cfg.default_model);
        printf("Max tokens:      %d\n", cfg.max_tokens);
        printf("HTTP timeout:    %ds\n", cfg.http_timeout_seconds);
        if (cfg.instructions_file[0])
            printf("Instructions:    %s\n", cfg.instructions_file);
        if (cfg.api_url[0])
            printf("Base URL:        %s\n", cfg.api_url);
        printf("\nRun `noclaw agent` to start chatting!\n");
        return 0;
    }

    fprintf(stderr, "Failed to save configuration.\n");
    return 1;
}

/* ── Doctor command ───────────────────────────────────────────── */

int nc_cmd_doctor(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("noclaw doctor -- system diagnostics\n");
    printf("────────────────────────────────────\n");

    nc_config cfg;
    bool loaded = nc_config_load(&cfg);
    int issues = 0;

    /* Config check */
    printf("  Config file:    ");
    if (loaded) {
        printf("OK (%s)\n", cfg.config_path);
    } else {
        printf("MISSING -- run `noclaw onboard`\n");
        issues++;
    }

    /* Workspace */
    printf("  Workspace dir:  ");
    if (loaded && nc_file_exists(cfg.workspace_dir)) {
        printf("OK (%s)\n", cfg.workspace_dir);
    } else {
        printf("MISSING\n");
        issues++;
    }

    /* API key */
    printf("  API key:        ");
    if (loaded && cfg.api_key[0]) {
        printf("configured (%.4s...)\n", cfg.api_key);
    } else if (loaded && provider_is_local(cfg.default_provider)) {
        printf("not required (%s is local)\n", cfg.default_provider);
    } else {
        printf("NOT SET\n");
        issues++;
    }

    /* TLS */
    printf("  TLS backend:    ");
#ifdef __APPLE__
    printf("SecureTransport (built-in)\n");
#else
    printf("BearSSL\n");
#endif

    /* Memory */
    printf("  Memory:         flat-file (keyword search)\n");

    /* Summary */
    printf("\n");
    if (issues == 0) {
        printf("All checks passed.\n");
    } else {
        printf("%d issue(s) found.\n", issues);
    }

    return issues > 0 ? 1 : 0;
}
