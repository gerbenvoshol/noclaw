/*
 * Channel implementations: CLI, Telegram, Discord, Slack.
 * Each channel is a vtable instance matching the nc_channel interface.
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

/* ── CLI channel ──────────────────────────────────────────────── */

typedef struct {
    char history[32][4096];
    int history_count;
    int history_pos;
} cli_ctx;

static size_t cli_term_columns(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return 80;
}

static void cli_refresh_line(const char *buf, size_t len, size_t cursor) {
    size_t prompt_len = 5; /* "you> " */
    size_t cols = cli_term_columns();
    size_t total = prompt_len + len;
    size_t lines = cols ? (total / cols) + 1 : 1;

    printf("\r");
    if (lines > 1)
        printf("\x1b[%zuA", lines - 1);
    printf("you> %.*s\x1b[J", (int)len, buf);

    if (len > cursor)
        printf("\x1b[%zuD", len - cursor);
    fflush(stdout);
}

static bool cli_readline(cli_ctx *ctx, char *out, size_t out_cap) {
    if (!isatty(STDIN_FILENO)) {
        if (!fgets(out, out_cap, stdin))
            return false;
        size_t len = strlen(out);
        if (len > 0 && out[len - 1] == '\n')
            out[len - 1] = '\0';
        return out[0] != '\0';
    }

    struct termios oldt, raw;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0)
        return false;
    raw = oldt;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return false;

    size_t len = 0;
    size_t cursor = 0;
    out[0] = '\0';
    ctx->history_pos = ctx->history_count;

    printf("you> ");
    fflush(stdout);

    for (;;) {
        unsigned char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
            printf("\n");
            return false;
        }

        if (ch == '\r' || ch == '\n') {
            out[len] = '\0';
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
            printf("\n");
            return out[0] != '\0';
        }

        if (ch == 4) {
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
            printf("\n");
            return false;
        }

        if (ch == 127 || ch == 8) {
            if (cursor > 0 && len > 0) {
                memmove(out + cursor - 1, out + cursor, len - cursor + 1);
                cursor--;
                len--;
                cli_refresh_line(out, len, cursor);
            }
            continue;
        }

        if (ch == 27) {
            unsigned char seq1 = 0, seq2 = 0, seq3 = 0;
            if (read(STDIN_FILENO, &seq1, 1) <= 0)
                continue;
            if (seq1 != '[')
                continue;
            if (read(STDIN_FILENO, &seq2, 1) <= 0)
                continue;

            if (seq2 == 'A') {
                if (ctx->history_count > 0 && ctx->history_pos > 0) {
                    ctx->history_pos--;
                    nc_strlcpy(out, ctx->history[ctx->history_pos], out_cap);
                    len = strlen(out);
                    cursor = len;
                    cli_refresh_line(out, len, cursor);
                }
            } else if (seq2 == 'B') {
                if (ctx->history_pos < ctx->history_count) {
                    ctx->history_pos++;
                    if (ctx->history_pos == ctx->history_count) {
                        out[0] = '\0';
                        len = 0;
                    } else {
                        nc_strlcpy(out, ctx->history[ctx->history_pos], out_cap);
                        len = strlen(out);
                    }
                    cursor = len;
                    cli_refresh_line(out, len, cursor);
                }
            } else if (seq2 == 'C') {
                if (cursor < len) {
                    cursor++;
                    cli_refresh_line(out, len, cursor);
                }
            } else if (seq2 == 'D') {
                if (cursor > 0) {
                    cursor--;
                    cli_refresh_line(out, len, cursor);
                }
            } else if (seq2 == '3') {
                if (read(STDIN_FILENO, &seq3, 1) > 0 && seq3 == '~' && cursor < len) {
                    memmove(out + cursor, out + cursor + 1, len - cursor);
                    len--;
                    cli_refresh_line(out, len, cursor);
                }
            }
            continue;
        }

        if (ch >= 32 && ch < 127) {
            if (len + 1 < out_cap) {
                memmove(out + cursor + 1, out + cursor, len - cursor + 1);
                out[cursor] = (char)ch;
                cursor++;
                len++;
                cli_refresh_line(out, len, cursor);
            }
        }
    }
}

static void cli_history_add(cli_ctx *ctx, const char *line) {
    if (!line[0])
        return;
    if (ctx->history_count > 0 && strcmp(ctx->history[ctx->history_count - 1], line) == 0)
        return;
    if (ctx->history_count < (int)(sizeof(ctx->history) / sizeof(ctx->history[0]))) {
        nc_strlcpy(ctx->history[ctx->history_count++], line, sizeof(ctx->history[0]));
        return;
    }
    memmove(ctx->history, ctx->history + 1, sizeof(ctx->history) - sizeof(ctx->history[0]));
    nc_strlcpy(ctx->history[ctx->history_count - 1], line, sizeof(ctx->history[0]));
}

static bool cli_poll(nc_channel *self, nc_incoming_msg *out) {
    cli_ctx *ctx = (cli_ctx *)self->ctx;
    memset(out, 0, sizeof(*out));

    if (!cli_readline(ctx, out->content, sizeof(out->content)))
        return false;

    cli_history_add(ctx, out->content);
    nc_strlcpy(out->sender, "cli", sizeof(out->sender));
    nc_strlcpy(out->channel_name, "cli", sizeof(out->channel_name));
    return true;
}

static bool cli_send(nc_channel *self, const char *to, const char *text) {
    (void)self;
    (void)to;
    printf("noclaw> %s\n", text);
    return true;
}

static void cli_free(nc_channel *self) {
    free(self->ctx);
    self->ctx = NULL;
}

nc_channel nc_channel_cli(void) {
    cli_ctx *ctx = (cli_ctx *)calloc(1, sizeof(cli_ctx));
    return (nc_channel){
        .name = "cli",
        .ctx  = ctx,
        .poll = cli_poll,
        .send = cli_send,
        .free = cli_free,
    };
}

/* ── Telegram channel (Bot API, long polling) ─────────────────── */

typedef struct {
    char token[1024];
    long long last_update_id;
} tg_ctx;

static bool tg_poll(nc_channel *self, nc_incoming_msg *out) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;
    memset(out, 0, sizeof(*out));

    /* GET /getUpdates?offset=<last+1>&timeout=30 */
    char url[1536];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/getUpdates?offset=%lld&timeout=30&allowed_updates=[\"message\"]",
        ctx->token, ctx->last_update_id + 1);

    nc_http_response resp;
    if (!nc_http_get(url, NULL, 0, &resp) || resp.status != 200) {
        nc_http_response_free(&resp);
        return false;
    }

    /* Parse response: {"ok":true,"result":[{"update_id":...,"message":{"chat":{"id":...},"text":"..."}}]} */
    nc_arena a;
    nc_arena_init(&a, resp.body_len * 2 + 512);
    nc_json *root = nc_json_parse(&a, resp.body, resp.body_len);
    nc_http_response_free(&resp);

    if (!root || !nc_json_bool(nc_json_get(root, "ok"), false)) {
        nc_arena_free(&a);
        return false;
    }

    nc_json *result = nc_json_get(root, "result");
    if (!result || result->type != NC_JSON_ARRAY || result->array.count == 0) {
        nc_arena_free(&a);
        return false;  /* no new messages, will long-poll again */
    }

    /* Take first update */
    nc_json *update = &result->array.items[0];
    long long uid = (long long)nc_json_num(nc_json_get(update, "update_id"), 0);
    if (uid > 0) ctx->last_update_id = uid;

    nc_json *message = nc_json_get(update, "message");
    if (!message) {
        nc_arena_free(&a);
        return false;
    }

    /* Extract chat_id and text */
    nc_json *chat = nc_json_get(message, "chat");
    long long chat_id = (long long)nc_json_num(nc_json_get(chat, "id"), 0);
    nc_str text = nc_json_str(nc_json_get(message, "text"), "");

    if (text.len == 0) {
        nc_arena_free(&a);
        return false;
    }

    snprintf(out->sender, sizeof(out->sender), "%lld", chat_id);
    size_t cplen = text.len < sizeof(out->content) - 1 ? text.len : sizeof(out->content) - 1;
    memcpy(out->content, text.ptr, cplen);
    out->content[cplen] = '\0';
    nc_strlcpy(out->channel_name, "telegram", sizeof(out->channel_name));

    nc_arena_free(&a);
    return true;
}

static bool tg_send(nc_channel *self, const char *to, const char *text) {
    tg_ctx *ctx = (tg_ctx *)self->ctx;

    char url[1536];
    snprintf(url, sizeof(url),
        "https://api.telegram.org/bot%s/sendMessage", ctx->token);

    /* Build JSON body */
    char body[65536];
    nc_jw w;
    nc_jw_init(&w, body, sizeof(body));
    nc_jw_obj_open(&w);
    nc_jw_raw(&w, "chat_id", to);
    nc_jw_str(&w, "text", text);
    nc_jw_obj_close(&w);

    const char *headers[] = { "Content-Type: application/json" };
    nc_http_response resp;
    bool ok = nc_http_post(url, body, w.len, headers, 1, &resp);
    int status = resp.status;
    if (!ok || status != 200)
        nc_log(NC_LOG_WARN, "Telegram sendMessage failed: HTTP %d", status);
    nc_http_response_free(&resp);
    return ok && status == 200;
}

static void tg_free(nc_channel *self) {
    free(self->ctx);
    self->ctx = NULL;
}

nc_channel nc_channel_telegram(const char *bot_token) {
    tg_ctx *ctx = (tg_ctx *)calloc(1, sizeof(tg_ctx));
    nc_strlcpy(ctx->token, bot_token, sizeof(ctx->token));
    return (nc_channel){
        .name = "telegram",
        .ctx  = ctx,
        .poll = tg_poll,
        .send = tg_send,
        .free = tg_free,
    };
}

/* ── Discord channel (Bot API, HTTP polling) ──────────────────── */

/*
 * Minimal Discord integration via REST API.
 * Uses GET /channels/{id}/messages to poll, POST to send.
 * A full implementation would use the WebSocket gateway for real-time events.
 * This is sufficient for low-traffic bot usage without the WS complexity.
 */

typedef struct {
    char token[1024];
    char channel_id[32];
    char last_message_id[32];
} discord_ctx;

static bool discord_poll(nc_channel *self, nc_incoming_msg *out) {
    discord_ctx *ctx = (discord_ctx *)self->ctx;
    memset(out, 0, sizeof(*out));

    if (!ctx->channel_id[0]) {
        nc_log(NC_LOG_WARN, "Discord channel_id not set (NOCLAW_DISCORD_CHANNEL)");
        usleep(5000000);  /* 5s */
        return false;
    }

    /* GET /channels/{id}/messages?after={last}&limit=1 */
    char url[1536];
    if (ctx->last_message_id[0]) {
        snprintf(url, sizeof(url),
            "https://discord.com/api/v10/channels/%s/messages?after=%s&limit=1",
            ctx->channel_id, ctx->last_message_id);
    } else {
        snprintf(url, sizeof(url),
            "https://discord.com/api/v10/channels/%s/messages?limit=1",
            ctx->channel_id);
    }

    char auth[1200];
    snprintf(auth, sizeof(auth), "Authorization: Bot %s", ctx->token);
    const char *headers[] = { auth, "User-Agent: noclaw/0.1" };

    nc_http_response resp;
    if (!nc_http_get(url, headers, 2, &resp) || resp.status != 200) {
        nc_http_response_free(&resp);
        /* Rate limited or no messages; back off */
        usleep(2000000);  /* 2s */
        return false;
    }

    /* Response is a JSON array of messages */
    nc_arena a;
    nc_arena_init(&a, resp.body_len * 2 + 512);
    nc_json *arr = nc_json_parse(&a, resp.body, resp.body_len);
    nc_http_response_free(&resp);

    if (!arr || arr->type != NC_JSON_ARRAY || arr->array.count == 0) {
        nc_arena_free(&a);
        usleep(2000000);  /* 2s, no new messages */
        return false;
    }

    /* Take latest message */
    nc_json *msg = &arr->array.items[0];
    nc_str msg_id = nc_json_str(nc_json_get(msg, "id"), "");
    nc_str content = nc_json_str(nc_json_get(msg, "content"), "");

    /* Skip if it's the same message we already saw */
    if (msg_id.len > 0 && strcmp(ctx->last_message_id, "") != 0) {
        /* Check if this is actually new */
        char id_buf[32];
        size_t il = msg_id.len < sizeof(id_buf) - 1 ? msg_id.len : sizeof(id_buf) - 1;
        memcpy(id_buf, msg_id.ptr, il);
        id_buf[il] = '\0';
        if (strcmp(id_buf, ctx->last_message_id) == 0) {
            nc_arena_free(&a);
            usleep(2000000);
            return false;
        }
    }

    /* Skip bot's own messages */
    nc_json *author = nc_json_get(msg, "author");
    if (author && nc_json_bool(nc_json_get(author, "bot"), false)) {
        if (msg_id.len > 0 && msg_id.len < sizeof(ctx->last_message_id))  {
            memcpy(ctx->last_message_id, msg_id.ptr, msg_id.len);
            ctx->last_message_id[msg_id.len] = '\0';
        }
        nc_arena_free(&a);
        usleep(2000000);
        return false;
    }

    /* Store message ID */
    if (msg_id.len > 0 && msg_id.len < sizeof(ctx->last_message_id)) {
        memcpy(ctx->last_message_id, msg_id.ptr, msg_id.len);
        ctx->last_message_id[msg_id.len] = '\0';
    }

    /* Extract content */
    if (content.len == 0) {
        nc_arena_free(&a);
        return false;
    }

    nc_strlcpy(out->sender, ctx->channel_id, sizeof(out->sender));
    size_t cplen = content.len < sizeof(out->content) - 1 ? content.len : sizeof(out->content) - 1;
    memcpy(out->content, content.ptr, cplen);
    out->content[cplen] = '\0';
    nc_strlcpy(out->channel_name, "discord", sizeof(out->channel_name));

    nc_arena_free(&a);
    return true;
}

static bool discord_send(nc_channel *self, const char *to, const char *text) {
    discord_ctx *ctx = (discord_ctx *)self->ctx;
    (void)to;

    char url[1536];
    snprintf(url, sizeof(url),
        "https://discord.com/api/v10/channels/%s/messages", ctx->channel_id);

    char body[65536];
    nc_jw w;
    nc_jw_init(&w, body, sizeof(body));
    nc_jw_obj_open(&w);
    nc_jw_str(&w, "content", text);
    nc_jw_obj_close(&w);

    char auth[1200];
    snprintf(auth, sizeof(auth), "Authorization: Bot %s", ctx->token);
    const char *headers[] = {
        "Content-Type: application/json",
        auth,
        "User-Agent: noclaw/0.1",
    };

    nc_http_response resp;
    bool ok = nc_http_post(url, body, w.len, headers, 3, &resp);
    int status = resp.status;
    if (!ok || status != 200)
        nc_log(NC_LOG_WARN, "Discord send failed: HTTP %d", status);
    nc_http_response_free(&resp);
    return ok && status == 200;
}

static void discord_free(nc_channel *self) {
    free(self->ctx);
    self->ctx = NULL;
}

nc_channel nc_channel_discord(const char *bot_token) {
    discord_ctx *ctx = (discord_ctx *)calloc(1, sizeof(discord_ctx));
    nc_strlcpy(ctx->token, bot_token, sizeof(ctx->token));

    /* Channel ID from env or will be set from first incoming message */
    const char *ch = getenv("NOCLAW_DISCORD_CHANNEL");
    if (ch) nc_strlcpy(ctx->channel_id, ch, sizeof(ctx->channel_id));

    return (nc_channel){
        .name = "discord",
        .ctx  = ctx,
        .poll = discord_poll,
        .send = discord_send,
        .free = discord_free,
    };
}

/* ── Slack channel (Web API + Events via gateway webhook) ─────── */

/*
 * Slack integration using the Web API.
 * Sending: POST to chat.postMessage.
 * Receiving: conversations.history polling.
 * For real-time: use the noclaw gateway with Slack Events API webhook.
 */

typedef struct {
    char token[1024];      /* xoxb-... bot token */
    char channel_id[32];  /* channel to monitor */
    char last_ts[32];     /* timestamp of last seen message */
} slack_ctx;

static bool slack_poll(nc_channel *self, nc_incoming_msg *out) {
    slack_ctx *ctx = (slack_ctx *)self->ctx;
    memset(out, 0, sizeof(*out));

    if (!ctx->channel_id[0]) {
        usleep(5000000);
        return false;
    }

    /* GET conversations.history?channel={id}&oldest={ts}&limit=1 */
    char url[1536];
    if (ctx->last_ts[0]) {
        snprintf(url, sizeof(url),
            "https://slack.com/api/conversations.history?channel=%s&oldest=%s&limit=1",
            ctx->channel_id, ctx->last_ts);
    } else {
        snprintf(url, sizeof(url),
            "https://slack.com/api/conversations.history?channel=%s&limit=1",
            ctx->channel_id);
    }

    char auth[1200];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->token);
    const char *headers[] = { auth };

    nc_http_response resp;
    if (!nc_http_get(url, headers, 1, &resp) || resp.status != 200) {
        nc_http_response_free(&resp);
        usleep(2000000);
        return false;
    }

    nc_arena a;
    nc_arena_init(&a, resp.body_len * 2 + 512);
    nc_json *root = nc_json_parse(&a, resp.body, resp.body_len);
    nc_http_response_free(&resp);

    if (!root || !nc_json_bool(nc_json_get(root, "ok"), false)) {
        nc_arena_free(&a);
        usleep(2000000);
        return false;
    }

    nc_json *messages = nc_json_get(root, "messages");
    if (!messages || messages->type != NC_JSON_ARRAY || messages->array.count == 0) {
        nc_arena_free(&a);
        usleep(2000000);
        return false;
    }

    /* Slack returns newest first */
    nc_json *msg = &messages->array.items[0];
    nc_str ts = nc_json_str(nc_json_get(msg, "ts"), "");
    nc_str text = nc_json_str(nc_json_get(msg, "text"), "");

    /* Skip if same timestamp */
    if (ts.len > 0) {
        char ts_buf[32];
        size_t tl = ts.len < sizeof(ts_buf) - 1 ? ts.len : sizeof(ts_buf) - 1;
        memcpy(ts_buf, ts.ptr, tl);
        ts_buf[tl] = '\0';
        if (strcmp(ts_buf, ctx->last_ts) == 0) {
            nc_arena_free(&a);
            usleep(2000000);
            return false;
        }
        nc_strlcpy(ctx->last_ts, ts_buf, sizeof(ctx->last_ts));
    }

    /* Skip bot messages (bot_id is a string like "B01234", not a boolean) */
    nc_str subtype = nc_json_str(nc_json_get(msg, "subtype"), "");
    nc_json *bot_id_node = nc_json_get(msg, "bot_id");
    if (subtype.len > 0 || (bot_id_node && bot_id_node->type == NC_JSON_STRING && bot_id_node->string.len > 0)) {
        nc_arena_free(&a);
        usleep(2000000);
        return false;
    }

    if (text.len == 0) {
        nc_arena_free(&a);
        return false;
    }

    nc_strlcpy(out->sender, ctx->channel_id, sizeof(out->sender));
    size_t cplen = text.len < sizeof(out->content) - 1 ? text.len : sizeof(out->content) - 1;
    memcpy(out->content, text.ptr, cplen);
    out->content[cplen] = '\0';
    nc_strlcpy(out->channel_name, "slack", sizeof(out->channel_name));

    nc_arena_free(&a);
    return true;
}

static bool slack_send(nc_channel *self, const char *to, const char *text) {
    slack_ctx *ctx = (slack_ctx *)self->ctx;
    const char *channel = (to && to[0]) ? to : ctx->channel_id;

    char body[65536];
    nc_jw w;
    nc_jw_init(&w, body, sizeof(body));
    nc_jw_obj_open(&w);
    nc_jw_str(&w, "channel", channel);
    nc_jw_str(&w, "text", text);
    nc_jw_obj_close(&w);

    char auth[1200];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", ctx->token);
    const char *headers[] = {
        "Content-Type: application/json; charset=utf-8",
        auth,
    };

    nc_http_response resp;
    bool ok = nc_http_post("https://slack.com/api/chat.postMessage",
                           body, w.len, headers, 2, &resp);
    int status = resp.status;
    if (!ok || status != 200)
        nc_log(NC_LOG_WARN, "Slack send failed: HTTP %d", status);
    nc_http_response_free(&resp);
    return ok && status == 200;
}

static void slack_free(nc_channel *self) {
    free(self->ctx);
    self->ctx = NULL;
}

nc_channel nc_channel_slack(const char *bot_token) {
    slack_ctx *ctx = (slack_ctx *)calloc(1, sizeof(slack_ctx));
    nc_strlcpy(ctx->token, bot_token, sizeof(ctx->token));

    const char *ch = getenv("NOCLAW_SLACK_CHANNEL");
    if (ch) nc_strlcpy(ctx->channel_id, ch, sizeof(ctx->channel_id));

    return (nc_channel){
        .name = "slack",
        .ctx  = ctx,
        .poll = slack_poll,
        .send = slack_send,
        .free = slack_free,
    };
}
