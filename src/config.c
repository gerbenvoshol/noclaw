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

    /* Provider */
    nc_strlcpy(cfg->default_provider, "openrouter", sizeof(cfg->default_provider));
    nc_strlcpy(cfg->default_model, "anthropic/claude-sonnet-4", sizeof(cfg->default_model));
    cfg->default_temperature = 0.7;

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

    if ((v = nc_json_get(root, "api_key")))
        str_to_buf(cfg->api_key, sizeof(cfg->api_key), nc_json_str(v, ""));
    if ((v = nc_json_get(root, "api_url")))
        str_to_buf(cfg->api_url, sizeof(cfg->api_url), nc_json_str(v, ""));
    if ((v = nc_json_get(root, "default_provider")))
        str_to_buf(cfg->default_provider, sizeof(cfg->default_provider), nc_json_str(v, "openrouter"));
    if ((v = nc_json_get(root, "default_model")))
        str_to_buf(cfg->default_model, sizeof(cfg->default_model), nc_json_str(v, ""));
    if ((v = nc_json_get(root, "default_temperature")))
        cfg->default_temperature = nc_json_num(v, 0.7);

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

    char buf[4096];
    nc_jw w;
    nc_jw_init(&w, buf, sizeof(buf));

    nc_jw_obj_open(&w);

    if (cfg->api_key[0])
        nc_jw_str(&w, "api_key", cfg->api_key);
    nc_jw_str(&w, "default_provider", cfg->default_provider);
    nc_jw_str(&w, "default_model", cfg->default_model);
    nc_jw_num(&w, "default_temperature", cfg->default_temperature);

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
        /* Write raw suffix */
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
            "    \"max_actions_per_hour\": %d\n  }",
            cfg->autonomy_level,
            cfg->workspace_only ? "true" : "false",
            cfg->max_actions_per_hour);
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
    if ((v = getenv("NOCLAW_GATEWAY_PORT"))) {
        long p = strtol(v, NULL, 10);
        if (p > 0 && p <= 65535) cfg->gateway_port = (uint16_t)p;
    }
    if ((v = getenv("NOCLAW_GATEWAY_HOST")))
        nc_strlcpy(cfg->gateway_host, v, sizeof(cfg->gateway_host));
    if ((v = getenv("NOCLAW_WORKSPACE")))
        nc_strlcpy(cfg->workspace_dir, v, sizeof(cfg->workspace_dir));
    if ((v = getenv("NOCLAW_BASE_URL")))
        nc_strlcpy(cfg->api_url, v, sizeof(cfg->api_url));

    /* Apply sensible defaults for local providers */
    if (strcmp(cfg->default_provider, "ollama") == 0) {
        if (!cfg->api_url[0])
            nc_strlcpy(cfg->api_url, "http://localhost:11434/v1", sizeof(cfg->api_url));
    }
}

/* ── Tests ──────────────────────────────────────────────────────── */

#ifdef NC_TEST
void nc_test_config(void) {
    nc_config cfg;
    nc_config_defaults(&cfg);

    NC_ASSERT(strcmp(cfg.default_provider, "openrouter") == 0, "config default provider");
    NC_ASSERT(cfg.default_temperature == 0.7, "config default temp");
    NC_ASSERT(cfg.gateway_port == 3000, "config default port");
    NC_ASSERT(cfg.gateway_require_pairing == true, "config default pairing");
    NC_ASSERT(cfg.gateway_allow_public_bind == false, "config default no public bind");
    NC_ASSERT(cfg.workspace_only == true, "config default workspace_only");
    NC_ASSERT(cfg.secrets_encrypt == true, "config default secrets encrypt");
    NC_ASSERT(strcmp(cfg.memory_backend, "flat") == 0, "config default memory backend");
    NC_ASSERT(strcmp(cfg.runtime_kind, "native") == 0, "config default runtime");
}
#endif
