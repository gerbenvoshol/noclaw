/*
 * Minimal HTTP gateway server using POSIX sockets.
 * Endpoints: /health, /pair, /webhook
 * Matches nullclaw/zeroclaw gateway interface.
 */

#include "nc.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>

/* ── HTTP response helpers ────────────────────────────────────── */

static void send_response(int fd, int status, const char *content_type, const char *body) {
    const char *status_text = status == 200 ? "OK" :
                              status == 401 ? "Unauthorized" :
                              status == 404 ? "Not Found" :
                              status == 405 ? "Method Not Allowed" : "Error";

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text, content_type, strlen(body));

    ssize_t ignored = write(fd, header, (size_t)hlen);
    (void)ignored;
    ignored = write(fd, body, strlen(body));
    (void)ignored;
}

static void send_json(int fd, int status, const char *json) {
    send_response(fd, status, "application/json", json);
}

/* ── Parse HTTP request (minimal) ─────────────────────────────── */

typedef struct {
    char method[8];
    char path[256];
    char *body;
    size_t body_len;
    char auth_header[1200];   /* Authorization: Bearer ... */
    char pairing_header[32];  /* X-Pairing-Code */
} http_request;

static bool parse_request(int fd, http_request *req) {
    char *buf = NULL;
    size_t buf_cap = 131072;
    bool ok = false;

    memset(req, 0, sizeof(*req));

    buf = (char *)malloc(buf_cap);
    if (!buf)
        return false;
    req->body = (char *)malloc(1);
    if (!req->body) {
        free(buf);
        return false;
    }
    req->body[0] = '\0';

    size_t total = 0;
    while (total < buf_cap - 1) {
        ssize_t n = read(fd, buf + total, buf_cap - 1 - total);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    if (total == 0) goto cleanup;

    /* Parse request line */
    sscanf(buf, "%7s %255s", req->method, req->path);

    /* Parse headers */
    size_t content_length = 0;
    const char *line = buf;
    while ((line = strstr(line, "\r\n")) != NULL) {
        line += 2;
        if (line[0] == '\r') break; /* end of headers */

        if (strncasecmp(line, "Authorization:", 14) == 0) {
            const char *val = line + 14;
            while (*val == ' ') val++;
            size_t vlen = 0;
            while (val[vlen] && val[vlen] != '\r') vlen++;
            if (vlen >= sizeof(req->auth_header))
                vlen = sizeof(req->auth_header) - 1;
            memcpy(req->auth_header, val, vlen);
            req->auth_header[vlen] = '\0';
        }
        if (strncasecmp(line, "X-Pairing-Code:", 15) == 0) {
            const char *val = line + 15;
            while (*val == ' ') val++;
            size_t vlen = 0;
            while (val[vlen] && val[vlen] != '\r') vlen++;
            if (vlen >= sizeof(req->pairing_header))
                vlen = sizeof(req->pairing_header) - 1;
            memcpy(req->pairing_header, val, vlen);
            req->pairing_header[vlen] = '\0';
        }
        if (strncasecmp(line, "Content-Length:", 15) == 0) {
            content_length = (size_t)atol(line + 15);
        }
    }

    /* Find body (after \r\n\r\n) — continue reading until Content-Length is satisfied */
    const char *body_start = strstr(buf, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        size_t body_so_far = (size_t)(buf + total - body_start);

        while (content_length > 0 && body_so_far < content_length
               && total < buf_cap - 1) {
            ssize_t n = read(fd, buf + total, buf_cap - 1 - total);
            if (n <= 0) break;
            total += (size_t)n;
            buf[total] = '\0';
            body_so_far = (size_t)(buf + total - body_start);
        }

        req->body_len = body_so_far;
        char *body = (char *)malloc(req->body_len + 1);
        if (!body) {
            req->body_len = 0;
            goto cleanup;
        }
        free(req->body);
        req->body = body;
        memcpy(req->body, body_start, req->body_len);
        req->body[req->body_len] = '\0';
    }

    ok = true;

cleanup:
    if (!ok) {
        free(req->body);
        req->body = NULL;
        req->body_len = 0;
    }
    free(buf);
    return ok;
}

/* ── Constant-time string comparison (timing side-channel prevention) ── */

static bool ct_str_eq(const char *a, const char *b) {
    size_t alen = strlen(a);
    size_t blen = strlen(b);
    size_t maxlen = alen > blen ? alen : blen;
    /* Compare all bytes of the longer string to avoid early-exit leak */
    volatile unsigned char diff = (unsigned char)(alen != blen);
    for (size_t i = 0; i < maxlen; i++) {
        unsigned char ca = (i < alen) ? (unsigned char)a[i] : 0;
        unsigned char cb = (i < blen) ? (unsigned char)b[i] : 0;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

/* ── Gateway init ─────────────────────────────────────────────── */

void nc_gateway_init(nc_gateway *gw, nc_config *cfg, nc_agent *agent) {
    memset(gw, 0, sizeof(*gw));
    gw->config = cfg;
    gw->agent = agent;
    gw->paired = !cfg->gateway_require_pairing;
    gw->server_fd = -1;

    /* Generate pairing code (6 digits) */
    if (cfg->gateway_require_pairing) {
        nc_random_hex(gw->pairing_code, 6);
    }

    /* Generate bearer token */
    nc_random_hex(gw->bearer_token, 64);
}

/* ── Handle single request ────────────────────────────────────── */

static void handle_request(nc_gateway *gw, int client_fd) {
    http_request req;
    memset(&req, 0, sizeof(req));
    if (!parse_request(client_fd, &req)) {
        close(client_fd);
        return;
    }

    /* GET /health — always public */
    if (strcmp(req.path, "/health") == 0 && strcmp(req.method, "GET") == 0) {
        send_json(client_fd, 200, "{\"status\":\"ok\",\"version\":\"" NC_VERSION "\"}");
        free(req.body);
        close(client_fd);
        return;
    }

    /* POST /pair */
    if (strcmp(req.path, "/pair") == 0 && strcmp(req.method, "POST") == 0) {
        if (gw->config->gateway_require_pairing) {
            if (ct_str_eq(req.pairing_header, gw->pairing_code)) {
                gw->paired = true;
                char resp_body[256];
                snprintf(resp_body, sizeof(resp_body),
                    "{\"token\":\"%s\"}", gw->bearer_token);
                send_json(client_fd, 200, resp_body);
            } else {
                send_json(client_fd, 401, "{\"error\":\"invalid pairing code\"}");
            }
        } else {
            char resp_body[256];
            snprintf(resp_body, sizeof(resp_body),
                "{\"token\":\"%s\"}", gw->bearer_token);
            send_json(client_fd, 200, resp_body);
        }
        free(req.body);
        close(client_fd);
        return;
    }

    /* POST /webhook — requires bearer token */
    if (strcmp(req.path, "/webhook") == 0 && strcmp(req.method, "POST") == 0) {
        /* Check auth */
        char expected[256];
        snprintf(expected, sizeof(expected), "Bearer %s", gw->bearer_token);
        if (!gw->paired || !ct_str_eq(req.auth_header, expected)) {
            send_json(client_fd, 401, "{\"error\":\"unauthorized\"}");
            free(req.body);
            close(client_fd);
            return;
        }

        /* Extract message from body */
        nc_arena a;
        nc_arena_init(&a, 4096);
        nc_json *body = nc_json_parse(&a, req.body, req.body_len);
        nc_str message = nc_json_str(nc_json_get(body, "message"), "");

        if (message.len > 0) {
            char *msg_buf = (char *)malloc(message.len + 1);
            char *resp_buf = NULL;
            size_t reply_len;
            size_t resp_buf_cap;
            size_t cplen;
            if (!msg_buf) {
                send_json(client_fd, 500, "{\"error\":\"out of memory\"}");
                nc_arena_free(&a);
                free(req.body);
                close(client_fd);
                return;
            }
            cplen = message.len;
            memcpy(msg_buf, message.ptr, cplen);
            msg_buf[cplen] = '\0';

            const char *reply = nc_agent_chat(gw->agent, msg_buf);

            /* Build JSON response */
            reply_len = reply ? strlen(reply) : 0;
            if (reply_len > (SIZE_MAX - 64) / 6) {
                free(msg_buf);
                send_json(client_fd, 500, "{\"error\":\"response too large\"}");
                nc_arena_free(&a);
                free(req.body);
                close(client_fd);
                return;
            }
            resp_buf_cap = reply_len * 6 + 64;
            resp_buf = (char *)malloc(resp_buf_cap);
            if (!resp_buf) {
                free(msg_buf);
                send_json(client_fd, 500, "{\"error\":\"out of memory\"}");
                nc_arena_free(&a);
                free(req.body);
                close(client_fd);
                return;
            }
            nc_jw w;
            nc_jw_init(&w, resp_buf, resp_buf_cap);
            nc_jw_obj_open(&w);
            nc_jw_str(&w, "response", reply ? reply : "");
            nc_jw_obj_close(&w);
            send_json(client_fd, 200, resp_buf);
            free(resp_buf);
            free(msg_buf);
        } else {
            send_json(client_fd, 400, "{\"error\":\"missing 'message' field\"}");
        }

        nc_arena_free(&a);
        free(req.body);
        close(client_fd);
        return;
    }

    /* 404 */
    send_json(client_fd, 404, "{\"error\":\"not found\"}");
    free(req.body);
    close(client_fd);
}

/* ── Run server loop ──────────────────────────────────────────── */

bool nc_gateway_run(nc_gateway *gw) {
    /* Security: refuse public bind without explicit opt-in */
    if (strcmp(gw->config->gateway_host, "0.0.0.0") == 0 &&
        !gw->config->gateway_allow_public_bind) {
        nc_log(NC_LOG_ERROR, "Refusing to bind 0.0.0.0 without allow_public_bind=true");
        return false;
    }

    signal(SIGPIPE, SIG_IGN);

    gw->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (gw->server_fd < 0) {
        nc_log(NC_LOG_ERROR, "socket(): %s", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(gw->server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(gw->config->gateway_port);
    inet_pton(AF_INET, gw->config->gateway_host, &addr.sin_addr);

    if (bind(gw->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        nc_log(NC_LOG_ERROR, "bind(): %s", strerror(errno));
        close(gw->server_fd);
        return false;
    }

    if (listen(gw->server_fd, 16) < 0) {
        nc_log(NC_LOG_ERROR, "listen(): %s", strerror(errno));
        close(gw->server_fd);
        return false;
    }

    nc_log(NC_LOG_INFO, "noclaw gateway listening on %s:%d",
           gw->config->gateway_host, gw->config->gateway_port);

    if (gw->config->gateway_require_pairing && !gw->paired) {
        nc_log(NC_LOG_INFO, "Pairing code: %s", gw->pairing_code);
        nc_log(NC_LOG_INFO, "POST /pair with X-Pairing-Code header to authenticate");
    }

    /* Accept loop */
    while (true) {
        int client_fd = accept(gw->server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            nc_log(NC_LOG_ERROR, "accept(): %s", strerror(errno));
            break;
        }
        /* Timeout on client socket to prevent slowloris-style stalling */
        struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        handle_request(gw, client_fd);
    }

    close(gw->server_fd);
    return true;
}
