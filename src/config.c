/*
 * Configuration loader: ~/.noclaw/config.json
 * Mirrors nullclaw's config structure with C fixed-size buffers.
 */

#include "nc.h"
#include <string.h>
#include <stdlib.h>

/* ── Defaults ─────────────────────────────────────────────────── */

void nc_config_defaults(nc_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    /* Paths */
    const char *home = nc_home_dir();
    nc_path_join(cfg->config_dir, sizeof(cfg->config_dir), home, NC_CONFIG_DIR);
    nc_path_join(cfg->config_path, sizeof(cfg->config_path), cfg->config_dir, NC_CONFIG_FILE);
    nc_path_join(cfg->workspace_dir, sizeof(cfg->workspace_dir), cfg->config_dir, NC_WORKSPACE_DIR);
    cfg->instructions_file[0] = '\0';

    /* Provider */
    nc_strlcpy(cfg->default_provider, "openrouter", sizeof(cfg->default_provider));
    nc_strlcpy(cfg->default_model, "anthropic/claude-sonnet-4", sizeof(cfg->default_model));
    cfg->default_temperature = -1.0;  /* -1 = not set; provider uses its own default */
    cfg->max_tokens = 4096;

    /* Gateway */
    nc_strlcpy(cfg->gateway_host, "127.0.0.1", sizeof(cfg->gateway_host));
    cfg->gateway_port = 3000;
    cfg->gateway_require_pairing = true;
    cfg->gateway_allow_public_bind = false;

    /* Memory */
    nc_strlcpy(cfg->memory_backend, "flat", sizeof(cfg->memory_backend));
    cfg->memory_auto_save = true;

    /* Autonomy */
    nc_strlcpy(cfg->autonomy_level, "supervised", sizeof(cfg->autonomy_level));
    cfg->workspace_only = true;
    cfg->max_actions_per_hour = 20;
    cfg->max_iterations = 50;

    /* Heartbeat */
    cfg->heartbeat_enabled = false;
    cfg->heartbeat_interval_minutes = 30;

    /* Security */
    cfg->secrets_encrypt = true;
    nc_strlcpy(cfg->sandbox_backend, "auto", sizeof(cfg->sandbox_backend));

    /* Identity */
    nc_strlcpy(cfg->identity_format, "openclaw", sizeof(cfg->identity_format));

    /* Runtime */
    nc_strlcpy(cfg->runtime_kind, "native", sizeof(cfg->runtime_kind));

    /* Cost */
    cfg->cost_enabled = false;
    cfg->cost_daily_limit_usd = 10.0;
    cfg->cost_monthly_limit_usd = 100.0;
}

/* Helper: copy nc_str to fixed buffer */
static void str_to_buf(char *buf, size_t bufsz, nc_str s) {
    size_t cplen = s.len < bufsz - 1 ? s.len : bufsz - 1;
    if (s.ptr && cplen > 0) memcpy(buf, s.ptr, cplen);
    buf[cplen] = '\0';
}

/* ── Load ─────────────────────────────────────────────────────── */

bool nc_config_load(nc_config *cfg) {
    nc_config_defaults(cfg);

    size_t len;
    char *data = nc_read_file(cfg->config_path, &len);
    if (!data) return false;

    nc_arena a;
    nc_arena_init(&a, len * 4 + 1024);

    nc_json *root = nc_json_parse(&a, data, len);
    free(data);

    if (!root || root->type != NC_JSON_OBJECT) {
        nc_arena_free(&a);
        return false;
    }

    /* Top-level fields */
    nc_json *v;
    if ((v = nc_json_get(root, "workspace_dir")))
        str_to_buf(cfg->workspace_dir, sizeof(cfg->workspace_dir), nc_json_str(v, ""));

    if ((v = nc_json_get(root, "api_key")))
        str_to_buf(cfg->api_key, sizeof(cfg->api_key), nc_json_str(v, ""));
    if ((v = nc_json_get(root, "api_url")))
        str_to_buf(cfg->api_url, sizeof(cfg->api_url), nc_json_str(v, ""));
    if ((v = nc_json_get(root, "default_provider")))
        str_to_buf(cfg->default_provider, sizeof(cfg->default_provider), nc_json_str(v, "openrouter"));
    if ((v = nc_json_get(root, "default_model")))
        str_to_buf(cfg->default_model, sizeof(cfg->default_model), nc_json_str(v, ""));
    if ((v = nc_json_get(root, "default_temperature")))
        cfg->default_temperature = nc_json_num(v, -1.0);
    if ((v = nc_json_get(root, "max_tokens")))
        cfg->max_tokens = (int)nc_json_num(v, 4096);
    if ((v = nc_json_get(root, "instructions_file")))
        str_to_buf(cfg->instructions_file, sizeof(cfg->instructions_file), nc_json_str(v, ""));

    /* Gateway section */
    nc_json *gw = nc_json_get(root, "gateway");
    if (gw && gw->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(gw, "port")))
            cfg->gateway_port = (uint16_t)nc_json_num(v, 3000);
        if ((v = nc_json_get(gw, "host")))
            str_to_buf(cfg->gateway_host, sizeof(cfg->gateway_host), nc_json_str(v, "127.0.0.1"));
        if ((v = nc_json_get(gw, "require_pairing")))
            cfg->gateway_require_pairing = nc_json_bool(v, true);
        if ((v = nc_json_get(gw, "allow_public_bind")))
            cfg->gateway_allow_public_bind = nc_json_bool(v, false);
    }

    /* Memory section */
    nc_json *mem = nc_json_get(root, "memory");
    if (mem && mem->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(mem, "backend")))
            str_to_buf(cfg->memory_backend, sizeof(cfg->memory_backend), nc_json_str(v, "flat"));
        if ((v = nc_json_get(mem, "auto_save")))
            cfg->memory_auto_save = nc_json_bool(v, true);
    }

    /* Autonomy section */
    nc_json *aut = nc_json_get(root, "autonomy");
    if (aut && aut->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(aut, "level")))
            str_to_buf(cfg->autonomy_level, sizeof(cfg->autonomy_level), nc_json_str(v, "supervised"));
        if ((v = nc_json_get(aut, "workspace_only")))
            cfg->workspace_only = nc_json_bool(v, true);
        if ((v = nc_json_get(aut, "max_actions_per_hour")))
            cfg->max_actions_per_hour = (int)nc_json_num(v, 20);
        if ((v = nc_json_get(aut, "max_iterations")))
            cfg->max_iterations = (int)nc_json_num(v, 50);
    }

    /* Heartbeat section */
    nc_json *hb = nc_json_get(root, "heartbeat");
    if (hb && hb->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(hb, "enabled")))
            cfg->heartbeat_enabled = nc_json_bool(v, false);
        if ((v = nc_json_get(hb, "interval_minutes")))
            cfg->heartbeat_interval_minutes = (int)nc_json_num(v, 30);
    }

    /* Security section */
    nc_json *sec = nc_json_get(root, "security");
    if (sec && sec->type == NC_JSON_OBJECT) {
        nc_json *sb = nc_json_get(sec, "sandbox");
        if (sb && sb->type == NC_JSON_OBJECT) {
            if ((v = nc_json_get(sb, "backend")))
                str_to_buf(cfg->sandbox_backend, sizeof(cfg->sandbox_backend), nc_json_str(v, "auto"));
        }
    }

    /* Secrets section */
    nc_json *secrets = nc_json_get(root, "secrets");
    if (secrets && secrets->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(secrets, "encrypt")))
            cfg->secrets_encrypt = nc_json_bool(v, true);
    }

    /* Channels */
    nc_json *channels = nc_json_get(root, "channels");
    if (channels && channels->type == NC_JSON_OBJECT) {
        nc_json *telegram = nc_json_get(channels, "telegram");
        if (telegram && telegram->type == NC_JSON_OBJECT) {
            if ((v = nc_json_get(telegram, "token")))
                str_to_buf(cfg->telegram_token, sizeof(cfg->telegram_token), nc_json_str(v, ""));
        }
        nc_json *discord = nc_json_get(channels, "discord");
        if (discord && discord->type == NC_JSON_OBJECT) {
            if ((v = nc_json_get(discord, "token")))
                str_to_buf(cfg->discord_token, sizeof(cfg->discord_token), nc_json_str(v, ""));
        }
        nc_json *slack = nc_json_get(channels, "slack");
        if (slack && slack->type == NC_JSON_OBJECT) {
            if ((v = nc_json_get(slack, "token")))
                str_to_buf(cfg->slack_token, sizeof(cfg->slack_token), nc_json_str(v, ""));
        }
    }

    /* Identity */
    nc_json *id = nc_json_get(root, "identity");
    if (id && id->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(id, "format")))
            str_to_buf(cfg->identity_format, sizeof(cfg->identity_format), nc_json_str(v, "openclaw"));
    }

    /* Runtime */
    nc_json *rt = nc_json_get(root, "runtime");
    if (rt && rt->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(rt, "kind")))
            str_to_buf(cfg->runtime_kind, sizeof(cfg->runtime_kind), nc_json_str(v, "native"));
    }

    /* Cost */
    nc_json *cost = nc_json_get(root, "cost");
    if (cost && cost->type == NC_JSON_OBJECT) {
        if ((v = nc_json_get(cost, "enabled")))
            cfg->cost_enabled = nc_json_bool(v, false);
        if ((v = nc_json_get(cost, "daily_limit_usd")))
            cfg->cost_daily_limit_usd = nc_json_num(v, 10.0);
        if ((v = nc_json_get(cost, "monthly_limit_usd")))
            cfg->cost_monthly_limit_usd = nc_json_num(v, 100.0);
    }

    nc_arena_free(&a);
    nc_config_apply_env(cfg);
    return true;
}

/* ── Save ─────────────────────────────────────────────────────── */

bool nc_config_save(const nc_config *cfg) {
    nc_mkdir_p(cfg->config_dir);

    char buf[8192];
    nc_jw w;
    nc_jw_init(&w, buf, sizeof(buf));

    nc_jw_obj_open(&w);

    if (cfg->api_key[0])
        nc_jw_str(&w, "api_key", cfg->api_key);
    if (cfg->api_url[0])
        nc_jw_str(&w, "api_url", cfg->api_url);
    nc_jw_str(&w, "workspace_dir", cfg->workspace_dir);
    if (cfg->instructions_file[0])
        nc_jw_str(&w, "instructions_file", cfg->instructions_file);
    nc_jw_str(&w, "default_provider", cfg->default_provider);
    nc_jw_str(&w, "default_model", cfg->default_model);
    if (cfg->default_temperature >= 0.0)
        nc_jw_num(&w, "default_temperature", cfg->default_temperature);
    nc_jw_num(&w, "max_tokens", cfg->max_tokens);

    /* Gateway */
    nc_jw_raw(&w, "gateway", "{");
    {
        char tmp[256];
        snprintf(tmp, sizeof(tmp),
            "\n    \"port\": %d,\n    \"host\": \"%s\",\n"
            "    \"require_pairing\": %s,\n    \"allow_public_bind\": %s\n  }",
            cfg->gateway_port, cfg->gateway_host,
            cfg->gateway_require_pairing ? "true" : "false",
            cfg->gateway_allow_public_bind ? "true" : "false");
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Memory */
    nc_jw_raw(&w, "memory", "{");
    {
        char tmp[256];
        snprintf(tmp, sizeof(tmp),
            "\n    \"backend\": \"%s\",\n    \"auto_save\": %s\n  }",
            cfg->memory_backend,
            cfg->memory_auto_save ? "true" : "false");
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Autonomy */
    nc_jw_raw(&w, "autonomy", "{");
    {
        char tmp[256];
        snprintf(tmp, sizeof(tmp),
            "\n    \"level\": \"%s\",\n    \"workspace_only\": %s,\n"
            "    \"max_actions_per_hour\": %d,\n    \"max_iterations\": %d\n  }",
            cfg->autonomy_level,
            cfg->workspace_only ? "true" : "false",
            cfg->max_actions_per_hour,
            cfg->max_iterations);
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Heartbeat */
    nc_jw_raw(&w, "heartbeat", "{");
    {
        char tmp[128];
        snprintf(tmp, sizeof(tmp),
            "\n    \"enabled\": %s,\n    \"interval_minutes\": %d\n  }",
            cfg->heartbeat_enabled ? "true" : "false",
            cfg->heartbeat_interval_minutes);
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Security */
    nc_jw_raw(&w, "security", "{");
    {
        char tmp[128];
        snprintf(tmp, sizeof(tmp),
            "\n    \"sandbox\": {\n      \"backend\": \"%s\"\n    }\n  }",
            cfg->sandbox_backend);
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Secrets */
    nc_jw_raw(&w, "secrets", "{");
    {
        char tmp[96];
        snprintf(tmp, sizeof(tmp),
            "\n    \"encrypt\": %s\n  }",
            cfg->secrets_encrypt ? "true" : "false");
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Channels */
    nc_jw_raw(&w, "channels", "{");
    {
        char tmp[1024];
        int off = snprintf(tmp, sizeof(tmp), "\n");
        bool first = true;
        if (cfg->telegram_token[0]) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            "    \"telegram\": { \"token\": \"%s\" }",
                            cfg->telegram_token);
            first = false;
        }
        if (cfg->discord_token[0] && off < (int)sizeof(tmp)) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            "%s    \"discord\": { \"token\": \"%s\" }",
                            first ? "" : ",\n",
                            cfg->discord_token);
            first = false;
        }
        if (cfg->slack_token[0] && off < (int)sizeof(tmp)) {
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off,
                            "%s    \"slack\": { \"token\": \"%s\" }",
                            first ? "" : ",\n",
                            cfg->slack_token);
            first = false;
        }
        if (first)
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "  }");
        else
            off += snprintf(tmp + off, sizeof(tmp) - (size_t)off, "\n  }");
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Identity */
    nc_jw_raw(&w, "identity", "{");
    {
        char tmp[96];
        snprintf(tmp, sizeof(tmp),
            "\n    \"format\": \"%s\"\n  }",
            cfg->identity_format);
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Runtime */
    nc_jw_raw(&w, "runtime", "{");
    {
        char tmp[96];
        snprintf(tmp, sizeof(tmp),
            "\n    \"kind\": \"%s\"\n  }",
            cfg->runtime_kind);
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    /* Cost */
    nc_jw_raw(&w, "cost", "{");
    {
        char tmp[192];
        snprintf(tmp, sizeof(tmp),
            "\n    \"enabled\": %s,\n    \"daily_limit_usd\": %.2f,\n    \"monthly_limit_usd\": %.2f\n  }",
            cfg->cost_enabled ? "true" : "false",
            cfg->cost_daily_limit_usd,
            cfg->cost_monthly_limit_usd);
        size_t tl = strlen(tmp);
        if (w.len + tl < w.cap) { memcpy(w.buf + w.len, tmp, tl); w.len += tl; }
    }

    nc_jw_obj_close(&w);

    if (w.len < w.cap) w.buf[w.len] = '\0';
    return nc_write_file(cfg->config_path, w.buf, w.len);
}

/* ── Env overrides ────────────────────────────────────────────── */

void nc_config_apply_env(nc_config *cfg) {
    const char *v;
    if ((v = getenv("NOCLAW_API_KEY")))
        nc_strlcpy(cfg->api_key, v, sizeof(cfg->api_key));
    if ((v = getenv("NOCLAW_PROVIDER")))
        nc_strlcpy(cfg->default_provider, v, sizeof(cfg->default_provider));
    if ((v = getenv("NOCLAW_MODEL")))
        nc_strlcpy(cfg->default_model, v, sizeof(cfg->default_model));
    if ((v = getenv("NOCLAW_TEMPERATURE"))) {
        char *endp;
        double t = strtod(v, &endp);
        if (endp != v && t >= 0.0 && t <= 2.0) cfg->default_temperature = t;
    }
    if ((v = getenv("NOCLAW_MAX_TOKENS"))) {
        long mt = strtol(v, NULL, 10);
        if (mt > 0 && mt <= 10000000L) cfg->max_tokens = (int)mt;
    }
    if ((v = getenv("NOCLAW_GATEWAY_PORT"))) {
        long p = strtol(v, NULL, 10);
        if (p > 0 && p <= 65535) cfg->gateway_port = (uint16_t)p;
    }
    if ((v = getenv("NOCLAW_GATEWAY_HOST")))
        nc_strlcpy(cfg->gateway_host, v, sizeof(cfg->gateway_host));
    if ((v = getenv("NOCLAW_WORKSPACE")))
        nc_strlcpy(cfg->workspace_dir, v, sizeof(cfg->workspace_dir));
    if ((v = getenv("NOCLAW_INSTRUCTIONS_FILE")))
        nc_strlcpy(cfg->instructions_file, v, sizeof(cfg->instructions_file));
    if ((v = getenv("NOCLAW_BASE_URL")))
        nc_strlcpy(cfg->api_url, v, sizeof(cfg->api_url));

    /* Apply sensible defaults for local providers */
    if (strcmp(cfg->default_provider, "ollama") == 0) {
        if (!cfg->api_url[0])
            nc_strlcpy(cfg->api_url, "http://localhost:11434/v1", sizeof(cfg->api_url));
    }
    else if (strcmp(cfg->default_provider, "openai") == 0) {
        if (!cfg->api_url[0])
            nc_strlcpy(cfg->api_url, "https://api.openai.com/v1", sizeof(cfg->api_url));
    }
    else if (strcmp(cfg->default_provider, "gemini") == 0) {
        if (!cfg->api_url[0])
            nc_strlcpy(cfg->api_url, "https://generativelanguage.googleapis.com/v1beta/openai", sizeof(cfg->api_url));
    }
}

/* ── Tests ──────────────────────────────────────────────────────── */

#ifdef NC_TEST
void nc_test_config(void) {
    nc_config cfg;
    nc_config_defaults(&cfg);

    NC_ASSERT(strcmp(cfg.default_provider, "openrouter") == 0, "config default provider");
    NC_ASSERT(cfg.default_temperature == -1.0, "config default temp (unset)");
    NC_ASSERT(cfg.max_tokens == 4096, "config default max_tokens");
    NC_ASSERT(cfg.gateway_port == 3000, "config default port");
    NC_ASSERT(cfg.gateway_require_pairing == true, "config default pairing");
    NC_ASSERT(cfg.gateway_allow_public_bind == false, "config default no public bind");
    NC_ASSERT(cfg.workspace_only == true, "config default workspace_only");
    NC_ASSERT(cfg.max_iterations == 50, "config default max_iterations");
    NC_ASSERT(cfg.secrets_encrypt == true, "config default secrets encrypt");
    NC_ASSERT(strcmp(cfg.memory_backend, "flat") == 0, "config default memory backend");
    NC_ASSERT(strcmp(cfg.runtime_kind, "native") == 0, "config default runtime");
}
#endif
