/*
 * Minimal recursive-descent JSON parser.
 * Zero dependencies. Allocates into arena.
 */

#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ── Parser state ─────────────────────────────────────────────── */

typedef struct {
    const char *src;
    size_t      len;
    size_t      pos;
    nc_arena   *arena;
} jp;

static void jp_skip_ws(jp *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            p->pos++;
        else break;
    }
}

static char jp_peek(jp *p) {
    jp_skip_ws(p);
    return p->pos < p->len ? p->src[p->pos] : '\0';
}

static bool jp_consume(jp *p, char c) {
    jp_skip_ws(p);
    if (p->pos < p->len && p->src[p->pos] == c) {
        p->pos++;
        return true;
    }
    return false;
}

static nc_json *jp_alloc(jp *p) {
    nc_json *v = (nc_json *)nc_arena_alloc(p->arena, sizeof(nc_json));
    if (v) {
        memset(v, 0, sizeof(*v));
        v->src = p->src + p->pos;
        v->src_len = 0;
    }
    return v;
}

/* Forward declaration */
static nc_json *jp_value(jp *p);

static nc_json *jp_string_val(jp *p) {
    size_t value_start;

    jp_skip_ws(p);
    value_start = p->pos;
    if (!jp_consume(p, '"')) return NULL;

    size_t start = p->pos;
    /* Scan for closing quote, handle \\ and \" */
    while (p->pos < p->len) {
        if (p->src[p->pos] == '\\') {
            if (p->pos + 1 < p->len) p->pos += 2; else p->pos++;
            continue;
        }
        if (p->src[p->pos] == '"') break;
        p->pos++;
    }
    size_t end = p->pos;
    if (p->pos < p->len) p->pos++; /* skip closing " (if found) */

    nc_json *v = jp_alloc(p);
    if (!v) return NULL;
    v->type = NC_JSON_STRING;
    v->src = p->src + value_start;

    /* Unescape into arena (worst case: same length; \uXXXX expands to <=3 UTF-8 bytes) */
    char *buf = (char *)nc_arena_alloc(p->arena, (end - start) * 3 + 1);
    if (!buf) return NULL;
    size_t di = 0;
    for (size_t si = start; si < end; si++) {
        if (p->src[si] == '\\' && si + 1 < end) {
            si++;
            switch (p->src[si]) {
                case '"':  buf[di++] = '"';  break;
                case '\\': buf[di++] = '\\'; break;
                case '/':  buf[di++] = '/';  break;
                case 'n':  buf[di++] = '\n'; break;
                case 'r':  buf[di++] = '\r'; break;
                case 't':  buf[di++] = '\t'; break;
                case 'b':  buf[di++] = '\b'; break;
                case 'f':  buf[di++] = '\f'; break;
                case 'u': {
                    /* \uXXXX — decode to UTF-8 */
                    if (si + 4 < end) {
                        char hex[5] = { p->src[si+1], p->src[si+2], p->src[si+3], p->src[si+4], 0 };
                        unsigned long cp = strtoul(hex, NULL, 16);
                        si += 4;

                        /* Handle surrogate pairs: \uD800-\uDBFF followed by \uDC00-\uDFFF */
                        if (cp >= 0xD800 && cp <= 0xDBFF && si + 2 < end &&
                            p->src[si+1] == '\\' && p->src[si+2] == 'u' && si + 6 < end) {
                            char hex2[5] = { p->src[si+3], p->src[si+4], p->src[si+5], p->src[si+6], 0 };
                            unsigned long lo = strtoul(hex2, NULL, 16);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                si += 6;
                            }
                        }

                        /* Encode as UTF-8 */
                        if (cp < 0x80) {
                            buf[di++] = (char)cp;
                        } else if (cp < 0x800) {
                            buf[di++] = (char)(0xC0 | (cp >> 6));
                            buf[di++] = (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            buf[di++] = (char)(0xE0 | (cp >> 12));
                            buf[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            buf[di++] = (char)(0x80 | (cp & 0x3F));
                        } else if (cp < 0x110000) {
                            buf[di++] = (char)(0xF0 | (cp >> 18));
                            buf[di++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                            buf[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            buf[di++] = (char)(0x80 | (cp & 0x3F));
                        }
                    } else {
                        buf[di++] = 'u';  /* malformed: keep literal */
                    }
                    break;
                }
                default:   buf[di++] = p->src[si]; break;
            }
        } else {
            buf[di++] = p->src[si];
        }
    }
    buf[di] = '\0';

    v->string.ptr = buf;
    v->string.len = di;
    v->src_len = p->pos - value_start;
    return v;
}

static nc_json *jp_number(jp *p) {
    jp_skip_ws(p);
    size_t start = p->pos;
    if (p->pos < p->len && p->src[p->pos] == '-') p->pos++;
    while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    if (p->pos < p->len && p->src[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }
    if (p->pos < p->len && (p->src[p->pos] == 'e' || p->src[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->src[p->pos] == '+' || p->src[p->pos] == '-')) p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->src[p->pos])) p->pos++;
    }

    char tmp[64];
    size_t nlen = p->pos - start;
    if (nlen >= sizeof(tmp)) nlen = sizeof(tmp) - 1;
    memcpy(tmp, p->src + start, nlen);
    tmp[nlen] = '\0';

    nc_json *v = jp_alloc(p);
    if (!v) return NULL;
    v->type = NC_JSON_NUMBER;
    v->src = p->src + start;
    v->src_len = p->pos - start;
    v->number = strtod(tmp, NULL);
    return v;
}

static nc_json *jp_array(jp *p) {
    size_t start;

    jp_skip_ws(p);
    start = p->pos;
    if (!jp_consume(p, '[')) return NULL;

    /* Count-then-parse: first pass count, second parse */
    /* Simpler: dynamic with arena, max 1024 items */
    nc_json **items = (nc_json **)nc_arena_alloc(p->arena, 1024 * sizeof(nc_json *));
    if (!items) return NULL;
    int count = 0;

    if (jp_peek(p) != ']') {
        do {
            nc_json *item = jp_value(p);
            if (item && count < 1024)
                items[count++] = item;
        } while (jp_consume(p, ','));
    }
    jp_consume(p, ']');

    nc_json *v = jp_alloc(p);
    if (!v) return NULL;
    v->type = NC_JSON_ARRAY;
    v->src = p->src + start;
    v->src_len = p->pos - start;
    v->array.count = count;
    v->array.items = (nc_json *)nc_arena_alloc(p->arena, count * sizeof(nc_json));
    for (int i = 0; i < count; i++)
        v->array.items[i] = *items[i];
    return v;
}

static nc_json *jp_object(jp *p) {
    size_t start;

    jp_skip_ws(p);
    start = p->pos;
    if (!jp_consume(p, '{')) return NULL;

    nc_str  *keys = (nc_str *)nc_arena_alloc(p->arena, 512 * sizeof(nc_str));
    nc_json **vals = (nc_json **)nc_arena_alloc(p->arena, 512 * sizeof(nc_json *));
    if (!keys || !vals) return NULL;
    int count = 0;

    if (jp_peek(p) != '}') {
        do {
            nc_json *k = jp_string_val(p);
            if (!k) break;
            if (!jp_consume(p, ':')) break;
            nc_json *val = jp_value(p);
            if (val && count < 512) {
                keys[count] = k->string;
                vals[count] = val;
                count++;
            }
        } while (jp_consume(p, ','));
    }
    jp_consume(p, '}');

    nc_json *v = jp_alloc(p);
    if (!v) return NULL;
    v->type = NC_JSON_OBJECT;
    v->src = p->src + start;
    v->src_len = p->pos - start;
    v->object.count = count;
    v->object.keys = (nc_str *)nc_arena_alloc(p->arena, count * sizeof(nc_str));
    v->object.vals = (nc_json *)nc_arena_alloc(p->arena, count * sizeof(nc_json));
    for (int i = 0; i < count; i++) {
        v->object.keys[i] = keys[i];
        v->object.vals[i] = *vals[i];
    }
    return v;
}

static nc_json *jp_value(jp *p) {
    jp_skip_ws(p);
    if (p->pos >= p->len) return NULL;

    char c = p->src[p->pos];

    if (c == '"') return jp_string_val(p);
    if (c == '{') return jp_object(p);
    if (c == '[') return jp_array(p);
    if (c == '-' || isdigit((unsigned char)c)) return jp_number(p);

    /* true / false / null */
    if (p->len - p->pos >= 4 && memcmp(p->src + p->pos, "true", 4) == 0) {
        size_t start = p->pos;
        p->pos += 4;
        nc_json *v = jp_alloc(p);
        v->type = NC_JSON_BOOL;
        v->src = p->src + start;
        v->src_len = 4;
        v->boolean = true;
        return v;
    }
    if (p->len - p->pos >= 5 && memcmp(p->src + p->pos, "false", 5) == 0) {
        size_t start = p->pos;
        p->pos += 5;
        nc_json *v = jp_alloc(p);
        v->type = NC_JSON_BOOL;
        v->src = p->src + start;
        v->src_len = 5;
        v->boolean = false;
        return v;
    }
    if (p->len - p->pos >= 4 && memcmp(p->src + p->pos, "null", 4) == 0) {
        size_t start = p->pos;
        p->pos += 4;
        nc_json *v = jp_alloc(p);
        v->type = NC_JSON_NULL;
        v->src = p->src + start;
        v->src_len = 4;
        return v;
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

nc_json *nc_json_parse(nc_arena *a, const char *src, size_t len) {
    if (!src || len == 0) return NULL;
    jp p = { .src = src, .len = len, .pos = 0, .arena = a };
    return jp_value(&p);
}

nc_json *nc_json_get(nc_json *obj, const char *key) {
    if (!obj || obj->type != NC_JSON_OBJECT) return NULL;
    size_t klen = strlen(key);
    for (int i = 0; i < obj->object.count; i++) {
        if (obj->object.keys[i].len == klen &&
            memcmp(obj->object.keys[i].ptr, key, klen) == 0)
            return &obj->object.vals[i];
    }
    return NULL;
}


nc_str nc_json_get_slice(nc_json *root, nc_json *node) {
    (void)root;
    if (!node || !node->src || node->src_len == 0) return NC_STR_NULL;
    return (nc_str){ .ptr = node->src, .len = node->src_len };
}

nc_str nc_json_str(nc_json *v, const char *fallback) {
    if (v && v->type == NC_JSON_STRING) return v->string;
    return nc_str_from(fallback);
}

double nc_json_num(nc_json *v, double fallback) {
    if (!v) return fallback;
    if (v->type == NC_JSON_NUMBER) return v->number;
    /* Coerce integer-looking values */
    return fallback;
}

bool nc_json_bool(nc_json *v, bool fallback) {
    if (v && v->type == NC_JSON_BOOL) return v->boolean;
    return fallback;
}

/* ── JSON writer ──────────────────────────────────────────────── */

static void jw_append(nc_jw *w, const char *s, size_t n) {
    if (w->len + n < w->cap) {
        memcpy(w->buf + w->len, s, n);
        w->len += n;
    }
    /* Always maintain NUL termination so callers can safely use strlen */
    if (w->len < w->cap) w->buf[w->len] = '\0';
}

static void jw_appendz(nc_jw *w, const char *s) {
    jw_append(w, s, strlen(s));
}

static void jw_comma(nc_jw *w) {
    if (w->needs_comma) jw_appendz(w, ",");
    jw_appendz(w, "\n");
    for (int i = 0; i < w->depth; i++) jw_appendz(w, "  ");
    w->needs_comma = true;
}

void nc_jw_init(nc_jw *w, char *buf, size_t cap) {
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->depth = 0;
    w->needs_comma = false;
}

void nc_jw_obj_open(nc_jw *w) {
    if (w->depth > 0) jw_comma(w);
    jw_appendz(w, "{");
    w->depth++;
    w->needs_comma = false;
}

void nc_jw_obj_close(nc_jw *w) {
    w->depth--;
    jw_appendz(w, "\n");
    for (int i = 0; i < w->depth; i++) jw_appendz(w, "  ");
    jw_appendz(w, "}");
    w->needs_comma = true;
    if (w->len < w->cap) w->buf[w->len] = '\0';
}

void nc_jw_arr_open(nc_jw *w, const char *key) {
    jw_comma(w);
    jw_appendz(w, "\"");
    jw_appendz(w, key);
    jw_appendz(w, "\": [");
    w->depth++;
    w->needs_comma = false;
}

void nc_jw_arr_close(nc_jw *w) {
    w->depth--;
    jw_appendz(w, "\n");
    for (int i = 0; i < w->depth; i++) jw_appendz(w, "  ");
    jw_appendz(w, "]");
    w->needs_comma = true;
}

void nc_jw_str(nc_jw *w, const char *key, const char *val) {
    jw_comma(w);
    jw_appendz(w, "\"");
    jw_appendz(w, key);
    jw_appendz(w, "\": \"");
    /* Minimal escaping */
    for (const char *p = val; *p; p++) {
        switch (*p) {
            case '"':  jw_appendz(w, "\\\""); break;
            case '\\': jw_appendz(w, "\\\\"); break;
            case '\n': jw_appendz(w, "\\n"); break;
            case '\r': jw_appendz(w, "\\r"); break;
            case '\t': jw_appendz(w, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned char)*p);
                    jw_append(w, esc, 6);
                } else {
                    jw_append(w, p, 1);
                }
                break;
        }
    }
    jw_appendz(w, "\"");
}

void nc_jw_num(nc_jw *w, const char *key, double val) {
    jw_comma(w);
    jw_appendz(w, "\"");
    jw_appendz(w, key);
    jw_appendz(w, "\": ");
    char tmp[32];
    if (val >= -2147483648.0 && val <= 2147483647.0 && val == (int)val)
        snprintf(tmp, sizeof(tmp), "%d", (int)val);
    else
        snprintf(tmp, sizeof(tmp), "%.2f", val);
    jw_appendz(w, tmp);
}

void nc_jw_bool(nc_jw *w, const char *key, bool val) {
    jw_comma(w);
    jw_appendz(w, "\"");
    jw_appendz(w, key);
    jw_appendz(w, "\": ");
    jw_appendz(w, val ? "true" : "false");
}

void nc_jw_raw(nc_jw *w, const char *key, const char *raw) {
    jw_comma(w);
    jw_appendz(w, "\"");
    jw_appendz(w, key);
    jw_appendz(w, "\": ");
    jw_appendz(w, raw);
}

/* ── Tests ──────────────────────────────────────────────────────── */

#ifdef NC_TEST
void nc_test_json(void) {
    nc_arena a;
    nc_arena_init(&a, 4096);

    /* Parse object */
    const char *j1 = "{\"name\": \"noclaw\", \"version\": 1, \"fast\": true}";
    nc_json *r1 = nc_json_parse(&a, j1, strlen(j1));
    NC_ASSERT(r1 != NULL, "json parse object");
    NC_ASSERT(r1->type == NC_JSON_OBJECT, "json type object");

    nc_str name = nc_json_str(nc_json_get(r1, "name"), "");
    NC_ASSERT(nc_str_eql(name, "noclaw"), "json get string");

    double ver = nc_json_num(nc_json_get(r1, "version"), 0);
    NC_ASSERT(ver == 1.0, "json get number");

    bool fast = nc_json_bool(nc_json_get(r1, "fast"), false);
    NC_ASSERT(fast == true, "json get bool");

    NC_ASSERT(nc_json_get(r1, "missing") == NULL, "json get missing returns NULL");

    /* Parse array */
    const char *j2 = "[1, 2, 3]";
    nc_json *r2 = nc_json_parse(&a, j2, strlen(j2));
    NC_ASSERT(r2 != NULL && r2->type == NC_JSON_ARRAY, "json parse array");
    NC_ASSERT(r2->array.count == 3, "json array count");

    /* Parse nested */
    const char *j3 = "{\"gateway\": {\"port\": 3000, \"host\": \"127.0.0.1\"}}";
    nc_json *r3 = nc_json_parse(&a, j3, strlen(j3));
    NC_ASSERT(r3 != NULL, "json parse nested");
    nc_json *gw = nc_json_get(r3, "gateway");
    NC_ASSERT(gw != NULL && gw->type == NC_JSON_OBJECT, "json nested object");
    NC_ASSERT(nc_json_num(nc_json_get(gw, "port"), 0) == 3000, "json nested number");

    /* Parse string with escapes */
    const char *j4 = "{\"msg\": \"hello\\nworld\"}";
    nc_json *r4 = nc_json_parse(&a, j4, strlen(j4));
    nc_str msg = nc_json_str(nc_json_get(r4, "msg"), "");
    NC_ASSERT(msg.len == 11, "json escape string length");

    /* Parse null */
    const char *j5 = "null";
    nc_json *r5 = nc_json_parse(&a, j5, strlen(j5));
    NC_ASSERT(r5 != NULL && r5->type == NC_JSON_NULL, "json parse null");

    /* Slice tracking */
    const char *j6 = "{ \"message\": {\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"x\"}]}, \"n\": 1 }";
    nc_json *r6 = nc_json_parse(&a, j6, strlen(j6));
    nc_json *m6 = nc_json_get(r6, "message");
    nc_str s6 = nc_json_get_slice(r6, m6);
    NC_ASSERT(s6.ptr != NULL && s6.len > 0, "json slice exists");
    NC_ASSERT(memcmp(s6.ptr, "{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"x\"}]}", s6.len) == 0,
              "json slice content");

    nc_arena_free(&a);
}

void nc_test_jwriter(void) {
    char buf[512];
    nc_jw w;
    nc_jw_init(&w, buf, sizeof(buf));

    nc_jw_obj_open(&w);
    nc_jw_str(&w, "name", "noclaw");
    nc_jw_num(&w, "port", 3000);
    nc_jw_bool(&w, "fast", true);
    nc_jw_obj_close(&w);

    NC_ASSERT(w.len > 0, "jwriter produced output");
    NC_ASSERT(strstr(buf, "\"noclaw\"") != NULL, "jwriter has string");
    NC_ASSERT(strstr(buf, "3000") != NULL, "jwriter has number");
    NC_ASSERT(strstr(buf, "true") != NULL, "jwriter has bool");
}
#endif
