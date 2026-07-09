/*
 * Memory backend: flat-file with keyword search.
 * File format: one record per line, tab-separated: key\tcontent\ttimestamp
 * Search: tokenize query, scan entries, count matching words, return top N.
 * No SQLite, no FTS5 — the LLM is the ranker.
 */

#include "nc.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>
#include <strings.h>
#include <stdint.h>

/* ── Flat-file context ───────────────────────────────────────── */

typedef struct {
    char path[512];
} flat_mem;

#define NC_RECALL_LINE_MAX 9216

/* ── Flat-file escape/unescape (tab and newline break the format) ── */

static void flat_escape(const char *in, char *out, size_t out_cap) {
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 2 < out_cap; i++) {
        if (in[i] == '\t')      { out[j++] = '\\'; out[j++] = 't'; }
        else if (in[i] == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
        else if (in[i] == '\\') { out[j++] = '\\'; out[j++] = '\\'; }
        else                    { out[j++] = in[i]; }
    }
    out[j] = '\0';
}

static char *flat_escape_dup(const char *in) {
    if (!in) return NULL;
    size_t len = strlen(in);
    size_t cap = len * 2 + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    flat_escape(in, out, cap);
    return out;
}

static void flat_unescape(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            if (*r == 't')      *w++ = '\t';
            else if (*r == 'n') *w++ = '\n';
            else if (*r == '\\') *w++ = '\\';
            else { *w++ = '\\'; *w++ = *r; }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

/* ── Helpers ─────────────────────────────────────────────────── */

/* Case-insensitive substring check */
static bool contains_word(const char *haystack, const char *word, size_t wlen) {
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, word, wlen) == 0) {
            /* Check word boundary: start of string or non-alnum before */
            if (p == haystack || !isalnum((unsigned char)p[-1])) {
                char after = p[wlen];
                if (after == '\0' || !isalnum((unsigned char)after))
                    return true;
            }
        }
    }
    return false;
}

/* Count how many query tokens appear in text (key + content) */
static int score_entry(const char *key, const char *content,
                       const char *tokens[], int token_lens[], int ntokens) {
    int score = 0;
    for (int i = 0; i < ntokens; i++) {
        if (contains_word(key, tokens[i], (size_t)token_lens[i]))
            score += 2;  /* key match worth more */
        if (contains_word(content, tokens[i], (size_t)token_lens[i]))
            score += 1;
    }
    return score;
}

/* Tokenize query into words (pointers into original string) */
static int tokenize(const char *query, const char *tokens[], int lens[], int max) {
    int n = 0;
    const char *p = query;
    while (*p && n < max) {
        while (*p && !isalnum((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p)) p++;
        tokens[n] = start;
        lens[n] = (int)(p - start);
        n++;
    }
    return n;
}

/* ── File I/O helpers ────────────────────────────────────────── */

/* Read entire file into malloc'd buffer. Returns NULL if file doesn't exist. */
static char *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "r");
    if (!f) { *out_len = 0; return NULL; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); *out_len = 0; return NULL; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); *out_len = 0; return NULL; }

    *out_len = fread(buf, 1, (size_t)sz, f);
    buf[*out_len] = '\0';
    fclose(f);
    return buf;
}

/* Write buffer to file atomically (write to .tmp, rename) */
static bool write_all(const char *path, const char *data, size_t len) {
    char *tmp = nc_format("%s.tmp", path);
    if (!tmp) return false;

    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(tmp);
        return false;
    }

    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        remove(tmp);
        free(tmp);
        return false;
    }
    fclose(f);
    bool ok = rename(tmp, path) == 0;
    free(tmp);
    return ok;
}

/* ── Store ───────────────────────────────────────────────────── */

static bool flat_store(nc_memory *self, const char *key, const char *content) {
    flat_mem *ctx = (flat_mem *)self->ctx;
    if (!ctx) return false;

    time_t now = time(NULL);

    /* Escape key and content so tabs/newlines don't break the format */
    char *esc_key = flat_escape_dup(key);
    char *esc_content = flat_escape_dup(content);
    if (!esc_key || !esc_content) {
        free(esc_key);
        free(esc_content);
        return false;
    }
    size_t eklen = strlen(esc_key);
    size_t eclen = strlen(esc_content);

    /* Read existing file */
    size_t flen = 0;
    char *data = read_all(ctx->path, &flen);

    /* Build new file: replace line with matching key, or append */
    if (flen > SIZE_MAX - eklen - eclen - 128) {
        nc_log(NC_LOG_ERROR, "memory_store failed: entry too large");
        free(data);
        free(esc_key);
        free(esc_content);
        return false;
    }
    size_t new_cap = flen + eklen + eclen + 128;
    char *out = (char *)malloc(new_cap);
    if (!out) {
        free(data);
        free(esc_key);
        free(esc_content);
        return false;
    }

    size_t out_len = 0;

    /* Copy existing lines, skipping the one with matching key */
    if (data) {
        char *line = data;
        while (*line) {
            char *eol = strchr(line, '\n');
            size_t llen = eol ? (size_t)(eol - line) : strlen(line);

            /* Check if this line's key matches (stored escaped) */
            bool skip = false;
            if (llen > eklen) {
                skip = (memcmp(line, esc_key, eklen) == 0 && line[eklen] == '\t');
            }

            if (!skip && llen > 0) {
                if (out_len > new_cap || llen > new_cap - out_len - 1) {
                    nc_log(NC_LOG_ERROR, "memory_store failed: rebuilt memory data exceeded buffer");
                    free(out);
                    free(data);
                    free(esc_key);
                    free(esc_content);
                    return false;
                }
                memcpy(out + out_len, line, llen);
                out_len += llen;
                out[out_len++] = '\n';
            }

            line += llen;
            if (*line == '\n') line++;
        }
    }

    /* Append the new/updated entry (escaped) */
    int n = snprintf(out + out_len, new_cap - out_len, "%s\t%s\t%ld\n",
                     esc_key, esc_content, (long)now);
    if (n < 0 || (size_t)n >= new_cap - out_len) {
        nc_log(NC_LOG_ERROR, "memory_store failed: formatted entry exceeded buffer");
        free(out);
        free(data);
        free(esc_key);
        free(esc_content);
        return false;
    }
    out_len += (size_t)n;

    bool ok = write_all(ctx->path, out, out_len);

    free(out);
    free(data);
    free(esc_key);
    free(esc_content);
    return ok;
}

/* ── Recall ──────────────────────────────────────────────────── */

typedef struct { int score; const char *key; size_t klen; const char *content; size_t clen; } match_t;

static bool flat_recall(nc_memory *self, const char *query, char *out, size_t out_cap) {
    flat_mem *ctx = (flat_mem *)self->ctx;
    if (!ctx) return false;
    if (out_cap == 0) return false;

    size_t flen = 0;
    char *data = read_all(ctx->path, &flen);
    if (!data) {
        nc_strlcpy(out, "No matching memories found.", out_cap);
        return false;
    }

    /* Tokenize query */
    const char *tokens[32];
    int lens[32];
    int ntokens = tokenize(query, tokens, lens, 32);
    if (ntokens == 0) {
        free(data);
        nc_strlcpy(out, "No matching memories found.", out_cap);
        return false;
    }

    /* Score all entries, keep top 5 */
    match_t top[5] = {{0}};

    char *line = data;
    while (*line) {
        char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        if (llen == 0) { line++; continue; }

        /* Temporarily null-terminate line */
        char saved = line[llen];
        line[llen] = '\0';

        /* Parse: key\tcontent\ttimestamp */
        char *tab1 = strchr(line, '\t');
        if (tab1) {
            *tab1 = '\0';
            char *val = tab1 + 1;
            char *tab2 = strchr(val, '\t');
            if (tab2) *tab2 = '\0';

            int sc = score_entry(line, val, tokens, lens, ntokens);
            if (sc > 0) {
                /* Insert into top-5 (sorted descending) */
                for (int i = 0; i < 5; i++) {
                    if (sc > top[i].score) {
                        /* Shift down */
                        for (int j = 4; j > i; j--) top[j] = top[j-1];
                        top[i] = (match_t){ .score = sc, .key = line,
                                            .klen = (size_t)(tab1 - line),
                                            .content = val,
                                            .clen = tab2 ? (size_t)(tab2 - val) : strlen(val) };
                        break;
                    }
                }
            }

            /* Restore tabs for continued parsing */
            *tab1 = '\t';
            if (tab2) *tab2 = '\t';
        }

        line[llen] = saved;
        line += llen;
        if (*line == '\n') line++;
    }

    /* Format output (unescape stored content for display) */
    size_t off = 0;
    int count = 0;
    for (int i = 0; i < 5 && top[i].score > 0; i++) {
        char *key_buf = nc_strdup_n(top[i].key, top[i].klen);
        char *content_buf = nc_strdup_n(top[i].content, top[i].clen);
        if (!key_buf || !content_buf) {
            free(key_buf);
            free(content_buf);
            break;
        }
        flat_unescape(key_buf);

        flat_unescape(content_buf);

        char line_buf[NC_RECALL_LINE_MAX];
        int n = snprintf(line_buf, sizeof(line_buf), "[%s] %s\n", key_buf, content_buf);
        if (n < 0) {
            free(key_buf);
            free(content_buf);
            continue;
        }
        size_t line_len = (size_t)n < sizeof(line_buf) ? (size_t)n : sizeof(line_buf) - 1;
        size_t avail;

        if (off >= out_cap - 1)
            break;

        avail = out_cap - off - 1;
        if (line_len > avail)
            line_len = avail;

        memcpy(out + off, line_buf, line_len);
        off += line_len;
        out[off] = '\0';
        count++;
        free(key_buf);
        free(content_buf);

        if (avail == line_len)
            break;
    }

    free(data);

    if (count == 0) {
        nc_strlcpy(out, "No matching memories found.", out_cap);
        return false;
    }
    return true;
}

/* ── Forget ──────────────────────────────────────────────────── */

static bool flat_forget(nc_memory *self, const char *key) {
    flat_mem *ctx = (flat_mem *)self->ctx;
    if (!ctx) return false;

    size_t flen = 0;
    char *data = read_all(ctx->path, &flen);
    if (!data) return true;  /* nothing to forget */

    /* Escape key to match stored format */
    char *esc_key = flat_escape_dup(key);
    if (!esc_key) {
        free(data);
        return false;
    }
    size_t eklen = strlen(esc_key);

    char *out = (char *)malloc(flen + 1);
    if (!out) {
        free(data);
        free(esc_key);
        return false;
    }

    size_t out_len = 0;
    char *line = data;
    while (*line) {
        char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);

        bool skip = (llen > eklen && memcmp(line, esc_key, eklen) == 0 && line[eklen] == '\t');

        if (!skip && llen > 0) {
            memcpy(out + out_len, line, llen);
            out_len += llen;
            out[out_len++] = '\n';
        }

        line += llen;
        if (*line == '\n') line++;
    }

    bool ok = write_all(ctx->path, out, out_len);
    free(out);
    free(data);
    free(esc_key);
    return ok;
}

/* ── Free ────────────────────────────────────────────────────── */

static void flat_free(nc_memory *self) {
    flat_mem *ctx = (flat_mem *)self->ctx;
    if (ctx) free(ctx);
    self->ctx = NULL;
}

/* ── Constructor ─────────────────────────────────────────────── */

nc_memory nc_memory_flat(const char *path) {
    flat_mem *ctx = (flat_mem *)calloc(1, sizeof(flat_mem));
    if (!ctx) return nc_memory_noop();

    nc_strlcpy(ctx->path, path, sizeof(ctx->path));
    nc_log(NC_LOG_INFO, "Memory: flat-file at %s", path);

    return (nc_memory){
        .backend_name = "flat",
        .ctx     = ctx,
        .store   = flat_store,
        .recall  = flat_recall,
        .forget  = flat_forget,
        .free    = flat_free,
    };
}

/* ── Noop fallback ───────────────────────────────────────────── */

static bool noop_store(nc_memory *self, const char *key, const char *content) {
    (void)self; (void)key; (void)content;
    return true;
}

static bool noop_recall(nc_memory *self, const char *query, char *out, size_t out_cap) {
    (void)self; (void)query;
    nc_strlcpy(out, "Memory not available.", out_cap);
    return false;
}

static bool noop_forget(nc_memory *self, const char *key) {
    (void)self; (void)key;
    return true;
}

static void noop_free(nc_memory *self) {
    (void)self;
}

nc_memory nc_memory_noop(void) {
    return (nc_memory){
        .backend_name = "noop",
        .ctx = NULL,
        .store  = noop_store,
        .recall = noop_recall,
        .forget = noop_forget,
        .free   = noop_free,
    };
}

/* ── Tests ───────────────────────────────────────────────────── */

#ifdef NC_TEST
#include <unistd.h>

void nc_test_memory(void) {
    /* Use a temp file for testing */
    char tmppath[] = "/tmp/noclaw_test_mem_XXXXXX";
    int fd = mkstemp(tmppath);
    NC_ASSERT(fd >= 0, "create temp file for memory test");
    close(fd);

    nc_memory mem = nc_memory_flat(tmppath);
    NC_ASSERT(strcmp(mem.backend_name, "flat") == 0, "flat backend name");

    /* Store */
    bool ok = mem.store(&mem, "greeting", "Hello, world!");
    NC_ASSERT(ok, "memory store greeting");

    ok = mem.store(&mem, "project", "noclaw is written in C");
    NC_ASSERT(ok, "memory store project");

    ok = mem.store(&mem, "language", "C is fast and small");
    NC_ASSERT(ok, "memory store language");

    /* Recall by keyword search */
    char buf[4096];
    ok = mem.recall(&mem, "noclaw", buf, sizeof(buf));
    NC_ASSERT(ok, "memory recall noclaw");
    NC_ASSERT(strstr(buf, "noclaw") != NULL, "recall result contains noclaw");

    /* Recall with no match */
    ok = mem.recall(&mem, "xyznonexistent", buf, sizeof(buf));
    NC_ASSERT(!ok, "recall returns false for no match");

    /* Upsert (store with existing key) */
    ok = mem.store(&mem, "greeting", "Updated greeting!");
    NC_ASSERT(ok, "memory upsert greeting");

    ok = mem.recall(&mem, "Updated", buf, sizeof(buf));
    NC_ASSERT(ok, "recall finds updated content");
    NC_ASSERT(strstr(buf, "Updated greeting") != NULL, "upsert overwrote content");

    /* Forget */
    ok = mem.forget(&mem, "greeting");
    NC_ASSERT(ok, "memory forget greeting");

    ok = mem.recall(&mem, "Updated greeting", buf, sizeof(buf));
    NC_ASSERT(!ok, "recall returns false after forget");

    /* Multiple results ranking */
    mem.store(&mem, "c_info_1", "C language was created in 1972");
    mem.store(&mem, "c_info_2", "C is used for systems programming");
    mem.store(&mem, "c_info_3", "C compilers include gcc and clang");

    ok = mem.recall(&mem, "C language", buf, sizeof(buf));
    NC_ASSERT(ok, "recall multiple C results");

    {
        char small[32];
        memset(small, 'x', sizeof(small));
        small[sizeof(small) - 1] = '\0';
        ok = mem.recall(&mem, "C", small, sizeof(small));
        NC_ASSERT(ok, "recall works with small output buffers");
        NC_ASSERT(small[sizeof(small) - 1] == '\0', "small recall buffer remains null-terminated");
    }

    /* Free */
    mem.free(&mem);
    NC_ASSERT(mem.ctx == NULL, "memory free nulls ctx");

    /* Cleanup */
    unlink(tmppath);

    /* Test noop fallback */
    nc_memory noop = nc_memory_noop();
    NC_ASSERT(strcmp(noop.backend_name, "noop") == 0, "noop backend name");
    ok = noop.store(&noop, "key", "val");
    NC_ASSERT(ok, "noop store returns true");
    ok = noop.recall(&noop, "anything", buf, sizeof(buf));
    NC_ASSERT(!ok, "noop recall returns false");
    noop.free(&noop);
}
#endif
