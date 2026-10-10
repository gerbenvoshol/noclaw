/*
 * Agent loop: manages conversation history, dispatches tool calls,
 * and drives the provider in a loop until the LLM produces a final response.
 *
 * Tool dispatch:
 *   1. Send messages (with tool definitions) to provider
 *   2. If response has tool_calls: record assistant message, execute each tool,
 *      push tool results, and loop back to step 1
 *   3. If response has no tool_calls: return the text content
 */

#include "nc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#define NC_FNV1A64_OFFSET_BASIS 1469598103934665603ULL
#define NC_FNV1A64_PRIME        1099511628211ULL

/* ── Init / Free ──────────────────────────────────────────────── */

void nc_agent_init(nc_agent *agent, nc_config *cfg, nc_provider *prov,
                   nc_tool *tools, int tool_count, nc_memory *mem) {
    memset(agent, 0, sizeof(*agent));
    agent->config = cfg;
    agent->provider = prov;
    agent->tools = tools;
    agent->tool_count = tool_count;
    agent->memory = mem;

    nc_arena_init(&agent->arena, 64 * 1024);

    /* System prompt */
    static const char default_sys_prompt[] =
        "You are noclaw, the smallest AI assistant. "
        "Pure C. Under 100KB. You are helpful, concise, and fast. "
        "Use available tools when needed to complete tasks.";
    const char *sys_prompt = default_sys_prompt;
    size_t sys_prompt_len = sizeof(default_sys_prompt) - 1;
    char *file_prompt = NULL;

    if (cfg->instructions_file[0]) {
        size_t len = 0;
        file_prompt = nc_read_file(cfg->instructions_file, &len);
        if (file_prompt) {
            sys_prompt = file_prompt;
            sys_prompt_len = len;
        } else {
            nc_log(NC_LOG_WARN, "Could not read instructions file: %s", cfg->instructions_file);
        }
    }

    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, "system", 6),
        .content = nc_arena_dup(&agent->arena, sys_prompt, sys_prompt_len),
    };
    agent->message_count = 1;
    free(file_prompt);

}

/* Safe snprintf append */
static int append_snprintf(char *buf, size_t bufsz, int off, const char *fmt, ...) {
    if (bufsz == 0 || (size_t)off >= bufsz - 1) return off;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + off, bufsz - (size_t)off, fmt, args);
    va_end(args);
    if (written > 0) {
        off += written;
        if ((size_t)off >= bufsz) off = (int)(bufsz - 1);
    }
    return off;
}

/* ── Build tools JSON for the provider ────────────────────────── */


static size_t estimate_tools_json_size(nc_agent *agent) {
    size_t sz = 32;
    for (int i = 0; i < agent->tool_count; i++) {
        sz += 128;
        if (agent->tools[i].def.name)
            sz += strlen(agent->tools[i].def.name) * 2;
        if (agent->tools[i].def.description)
            sz += strlen(agent->tools[i].def.description) * 2;
        if (agent->tools[i].def.parameters_json)
            sz += strlen(agent->tools[i].def.parameters_json);
    }
    return sz;
}

static bool json_array_complete(const char *s) {
    size_t n;
    if (!s) return false;
    n = strlen(s);
    return n >= 2 && s[0] == '[' && s[n - 1] == ']';
}
static const char *build_tools_json(nc_agent *agent) {
    size_t bufsz;
    char *buf;
    int off = 0;

    if (agent->tool_count == 0) return NULL;

    bufsz = estimate_tools_json_size(agent);
    buf = (char *)nc_arena_alloc(&agent->arena, bufsz);
    if (!buf) return NULL;
    off = append_snprintf(buf, bufsz, off, "[");

    for (int i = 0; i < agent->tool_count; i++) {
        if (i > 0) off = append_snprintf(buf, bufsz, off, ",");
        off = append_snprintf(buf, bufsz, off,
            "{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":%s}}",
            agent->tools[i].def.name,
            agent->tools[i].def.description,
            agent->tools[i].def.parameters_json);
    }

    off = append_snprintf(buf, bufsz, off, "]");
    if (!json_array_complete(buf)) {
        nc_log(NC_LOG_ERROR, "Tools JSON exceeded allocated buffer");
        return NULL;
    }
    return buf;
}

/* Forward declaration — agent_push_msg is defined below agent_compact_messages */
static void agent_push_msg(nc_agent *agent, const char *role, const char *content,
                           const char *tool_call_id,
                           const nc_tool_call *tool_calls, int tool_call_count,
                           const char *raw_json);

/* ── Compact message history, resetting the arena ────────────── */

/* A tool-call round is one assistant message followed by at most
 * NC_MAX_TOOL_CALLS tool results, so a window of NC_MAX_MESSAGES/2 messages
 * always contains at least one non-tool message to start from. */
_Static_assert(NC_MAX_TOOL_CALLS < NC_MAX_MESSAGES / 2,
               "compaction window must be larger than one tool-call round");

static const char compaction_note[] =
    "[Earlier conversation was truncated to fit the context window.]";

static bool msg_role_is(const nc_message *m, const char *role) {
    return m->role && strcmp(m->role, role) == 0;
}

/* Pick the first message to keep, at or after `start`. Kept history must not
 * begin with a tool result whose assistant tool_calls message was dropped —
 * providers reject such history — so skip forward past tool messages. */
static int compaction_start(const nc_agent *agent, int start) {
    for (int i = start; i < agent->message_count; i++) {
        if (!msg_role_is(&agent->messages[i], "tool"))
            return i;
    }
    return agent->message_count;
}

/* Scratch copy of a message for arena-safe compaction. */
typedef struct {
    char  role[32];
    char *content;       /* malloc'd, or NULL */
    char *tool_call_id;  /* malloc'd, or NULL */
    char *raw_json;      /* malloc'd, or NULL */
    nc_tool_call *tool_calls; /* malloc'd array, or NULL */
    int   tool_call_count;
} nc_msg_scratch;

static void scratch_save(nc_msg_scratch *s, const nc_message *m, const char *default_role) {
    nc_strlcpy(s->role, m->role ? m->role : default_role, sizeof(s->role));
    if (m->content && !(s->content = strdup(m->content)))
        nc_log(NC_LOG_WARN, "Session compaction: strdup failed for message content");
    if (m->tool_call_id && !(s->tool_call_id = strdup(m->tool_call_id)))
        nc_log(NC_LOG_WARN, "Session compaction: strdup failed for tool_call_id");
    if (m->raw_json && !(s->raw_json = strdup(m->raw_json)))
        nc_log(NC_LOG_WARN, "Session compaction: strdup failed for raw_json");
    if (m->tool_calls && m->tool_call_count > 0) {
        s->tool_calls = (nc_tool_call *)malloc(
            (size_t)m->tool_call_count * sizeof(nc_tool_call));
        if (s->tool_calls) {
            memcpy(s->tool_calls, m->tool_calls,
                   (size_t)m->tool_call_count * sizeof(nc_tool_call));
            s->tool_call_count = m->tool_call_count;
        } else {
            nc_log(NC_LOG_WARN, "Session compaction: malloc failed for tool_calls");
        }
    }
}

static void agent_compact_messages(nc_agent *agent) {
    int total = agent->message_count;
    int start = total - NC_MAX_MESSAGES / 2;
    if (start < 1) start = 1; /* never drop system message */
    start = compaction_start(agent, start);

    int keep = total - start;
    /* Most providers (Anthropic in particular) require the first non-system
     * message to come from the user; insert a note when the cut lands on an
     * assistant message. */
    bool add_note = keep > 0 && !msg_role_is(&agent->messages[start], "user");
    int note_slots = add_note ? 1 : 0;

    /* scratch[0] = system message, scratch[1..keep] = the kept messages. */
    nc_msg_scratch *scratch = (nc_msg_scratch *)calloc(
        (size_t)keep + 1, sizeof(nc_msg_scratch));
    if (!scratch) {
        /* Fallback: memmove without arena reset (no crash, but arena leaks) */
        memmove(&agent->messages[1 + note_slots], &agent->messages[start],
                (size_t)keep * sizeof(nc_message));
        if (add_note) {
            agent->messages[1] = (nc_message){
                .role = nc_arena_dup(&agent->arena, "user", 4),
                .content = nc_arena_dup(&agent->arena, compaction_note,
                                        sizeof(compaction_note) - 1),
            };
        }
        agent->message_count = 1 + note_slots + keep;
        nc_log(NC_LOG_WARN,
               "Session compaction: alloc failed, arena not reset (%d messages kept)",
               keep);
        return;
    }

    if (agent->messages[0].role == NULL)
        nc_log(NC_LOG_WARN, "Session compaction: system message has NULL role");
    scratch_save(&scratch[0], &agent->messages[0], "system");
    for (int i = 0; i < keep; i++)
        scratch_save(&scratch[i + 1], &agent->messages[start + i], "user");

    /* Reset the arena — all arena-allocated string pointers are now invalid */
    nc_arena_reset(&agent->arena);
    agent->message_count = 0;

    for (int i = 0; i <= keep; i++) {
        if (i == 1 && add_note)
            agent_push_msg(agent, "user", compaction_note, NULL, NULL, 0, NULL);
        agent_push_msg(agent,
                       scratch[i].role,
                       scratch[i].content,
                       scratch[i].tool_call_id,
                       scratch[i].tool_calls,
                       scratch[i].tool_call_count,
                       scratch[i].raw_json);
        free(scratch[i].content);
        free(scratch[i].tool_call_id);
        free(scratch[i].raw_json);
        free(scratch[i].tool_calls);
    }
    free(scratch);

    nc_log(NC_LOG_INFO,
           "Session compacted: kept %d/%d messages%s, arena reset",
           keep, total, add_note ? " (+truncation note)" : "");
}


static void agent_push_msg(nc_agent *agent, const char *role, const char *content,
                           const char *tool_call_id,
                           const nc_tool_call *tool_calls, int tool_call_count,
                           const char *raw_json) {
    if (agent->message_count >= NC_MAX_MESSAGES) {
        agent_compact_messages(agent);
    }

    nc_message *msg = &agent->messages[agent->message_count++];
    memset(msg, 0, sizeof(*msg));
    msg->role = nc_arena_dup(&agent->arena, role, strlen(role));
    msg->content = content ? nc_arena_dup(&agent->arena, content, strlen(content)) : NULL;
    msg->tool_call_id = tool_call_id
        ? nc_arena_dup(&agent->arena, tool_call_id, strlen(tool_call_id))
        : NULL;
    msg->raw_json = raw_json
        ? nc_arena_dup(&agent->arena, raw_json, strlen(raw_json))
        : NULL;

    /* Copy tool_calls into arena */
    if (tool_calls && tool_call_count > 0) {
        msg->tool_calls = (nc_tool_call *)nc_arena_alloc(
            &agent->arena, (size_t)tool_call_count * sizeof(nc_tool_call));
        if (msg->tool_calls) {
            memcpy(msg->tool_calls, tool_calls, (size_t)tool_call_count * sizeof(nc_tool_call));
            msg->tool_call_count = tool_call_count;
        }
    }
}

/* ── Find a tool by name ──────────────────────────────────────── */

static nc_tool *find_tool(nc_agent *agent, const char *name) {
    for (int i = 0; i < agent->tool_count; i++) {
        if (strcmp(agent->tools[i].def.name, name) == 0)
            return &agent->tools[i];
    }
    return NULL;
}

/* Tool-loop guard hash: we hash each tool-call round and stop if identical
 * rounds repeat several times, which prevents infinite tool-call cycles. */
static uint64_t fnv1a64_update(uint64_t h, const char *s) {
    if (!s) return h;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= NC_FNV1A64_PRIME;
    }
    return h;
}

static uint64_t tool_round_hash(const nc_tool_call *calls, int count) {
    /* Standard FNV-1a 64-bit offset basis. */
    uint64_t h = NC_FNV1A64_OFFSET_BASIS;
    for (int i = 0; i < count; i++) {
        h = fnv1a64_update(h, calls[i].id);
        h = fnv1a64_update(h, calls[i].name);
        h = fnv1a64_update(h, calls[i].arguments);
        h ^= (uint64_t)calls[i].arguments_len;
        h *= NC_FNV1A64_PRIME;
        h ^= (uint64_t)calls[i].arguments_truncated;
        h *= NC_FNV1A64_PRIME;
    }
    return h;
}

/* ── Chat: single turn (loops for tool calls) ─────────────────── */

const char *nc_agent_chat(nc_agent *agent, const char *user_input) {
    agent_push_msg(agent, "user", user_input, NULL, NULL, 0, NULL);

    const char *tools_json = build_tools_json(agent);
    int max_iterations = agent->config->max_iterations > 0 ? agent->config->max_iterations : 100;
    uint64_t prev_tool_hash = 0;
    int repeat_tool_rounds = 0;

    /* nc_chat_response (~17 MB) and result_buf (256 KB) are too large for the
     * stack (default 8 MB on Linux).  Allocate both on the heap once and reuse
     * them across iterations to avoid repeated malloc/free overhead. */
    const char oom_msg[] = "Sorry, I ran out of memory.";

    nc_chat_response *resp = malloc(sizeof(*resp));
    if (!resp)
        return nc_arena_dup(&agent->arena, oom_msg, sizeof(oom_msg) - 1);

    char *result_buf = malloc(NC_TOOL_RESULT_MAX);
    if (!result_buf) {
        free(resp);
        return nc_arena_dup(&agent->arena, oom_msg, sizeof(oom_msg) - 1);
    }

    const char *final_reply = NULL;

    for (int iter = 0; iter < max_iterations; iter++) {
        nc_chat_request req = {
            .messages = agent->messages,
            .message_count = agent->message_count,
            .model = agent->config->default_model,
            .temperature = agent->config->default_temperature,
            .tools_json = tools_json,
            .max_tokens = agent->config->max_tokens,
        };

        if (!agent->provider->chat(agent->provider, &req, resp)) {
            static const char err_msg[] = "Sorry, I encountered an error communicating with the provider.";
            final_reply = nc_arena_dup(&agent->arena, err_msg, sizeof(err_msg) - 1);
            goto done;
        }

        if (!resp->has_tool_calls) {
            /* Final response — no tool calls */
            final_reply = nc_arena_dup(&agent->arena, resp->content, strlen(resp->content));
            agent_push_msg(agent, "assistant", resp->content, NULL, NULL, 0, NULL);
            goto done;
        }

        if (resp->tool_call_count <= 0) {
            nc_log(NC_LOG_WARN, "Provider indicated tool calls but returned none; ending loop");
            static const char loop_guard_msg[] =
                "I couldn't continue because tool-call metadata was incomplete.";
            final_reply = nc_arena_dup(&agent->arena, loop_guard_msg, sizeof(loop_guard_msg) - 1);
            goto done;
        }

        {
            uint64_t h = tool_round_hash(resp->tool_calls, resp->tool_call_count);
            if (iter > 0 && h == prev_tool_hash)
                repeat_tool_rounds++;
            else
                repeat_tool_rounds = 0;
            prev_tool_hash = h;

            if (repeat_tool_rounds >= 2) {
                nc_log(NC_LOG_WARN, "Detected repeated tool-call loop; ending after %d repeated rounds",
                       repeat_tool_rounds + 1);
                static const char repeat_msg[] =
                    "I stopped because the model repeated the same tool calls without progress.";
                final_reply = nc_arena_dup(&agent->arena, repeat_msg, sizeof(repeat_msg) - 1);
                goto done;
            }
        }

        /* ── Tool call dispatch ───────────────────────────────── */

        nc_log(NC_LOG_INFO, "Tool calls: %d (iteration %d/%d)",
               resp->tool_call_count, iter + 1, max_iterations);

        /* Push assistant message with tool_calls attached */
        agent_push_msg(agent, "assistant",
                       resp->content[0] ? resp->content : NULL,
                       NULL,
                       resp->tool_calls, resp->tool_call_count,
                       resp->raw_message_json[0] ? resp->raw_message_json : NULL);

        /* Execute each tool call and push results */
        for (int i = 0; i < resp->tool_call_count; i++) {
            nc_tool_call *tc = &resp->tool_calls[i];
            char fallback_tool_call_id[64];
            const char *tool_call_id = tc->id;

            if (!tool_call_id || !tool_call_id[0]) {
                /* 1-based IDs are easier to read in user-facing logs. */
                snprintf(fallback_tool_call_id, sizeof(fallback_tool_call_id),
                         "call_iter%d_%d", iter + 1, i + 1);
                tool_call_id = fallback_tool_call_id;
                nc_log(NC_LOG_WARN, "  -> tool call '%s' missing id; using fallback id '%s'",
                       tc->name, tool_call_id);
            }

            size_t arg_preview = strlen(tc->arguments);
            if (arg_preview > NC_LOG_PREVIEW_MAX)
                arg_preview = NC_LOG_PREVIEW_MAX;
            nc_log(NC_LOG_INFO, "  -> %s(args=%zu bytes%s): %.*s%s",
                   tc->name,
                   tc->arguments_len ? tc->arguments_len : strlen(tc->arguments),
                   tc->arguments_truncated ? ", truncated" : "",
                   (int)arg_preview,
                   tc->arguments,
                   strlen(tc->arguments) > arg_preview ? "...[truncated for log]" : "");

            nc_tool *tool = find_tool(agent, tc->name);

            if (tc->arguments_truncated) {
                snprintf(result_buf, NC_TOOL_RESULT_MAX,
                         "error: tool arguments for '%s' exceeded limit (%zu bytes > %zu bytes) and were not executed",
                         tc->name,
                         tc->arguments_len,
                         sizeof(tc->arguments) - 1);
                nc_log(NC_LOG_WARN,
                       "  <- tool %s blocked: arguments truncated (%zu bytes > %zu)",
                       tc->name, tc->arguments_len, sizeof(tc->arguments) - 1);
            } else if (tool) {
                bool ok = tool->execute(tool, tc->arguments, result_buf, NC_TOOL_RESULT_MAX);
                if (!ok) {
                    nc_log(NC_LOG_WARN, "  <- tool %s returned error", tc->name);
                    /* Still push the error as the tool result — the LLM needs to see it */
                }
            } else {
                snprintf(result_buf, NC_TOOL_RESULT_MAX,
                         "error: unknown tool '%s'", tc->name);
                nc_log(NC_LOG_WARN, "  <- unknown tool: %s", tc->name);
            }

            /* Push tool result message */
            agent_push_msg(agent, "tool", result_buf, tool_call_id, NULL, 0, NULL);
        }

        /* Loop back to provider with the tool results */
    }

    static const char max_iter_msg[] = "I reached the maximum number of tool iterations. Here's what I have so far.";
    final_reply = nc_arena_dup(&agent->arena, max_iter_msg, sizeof(max_iter_msg) - 1);

done:
    free(resp);
    free(result_buf);
    return final_reply;
}

/* ── Reset conversation ───────────────────────────────────────── */

void nc_agent_reset(nc_agent *agent) {
    /* Copy system prompt off-arena BEFORE resetting arena (avoids use-after-free). */
    const char *role_src = agent->messages[0].role ? agent->messages[0].role : "system";
    const char *content_src = agent->messages[0].content ? agent->messages[0].content : "";
    char *role_copy = strdup(role_src);
    char *content_copy = strdup(content_src);

    if (!role_copy || !content_copy) {
        free(role_copy);
        free(content_copy);
        role_copy = strdup("system");
        content_copy = strdup("You are noclaw, the smallest AI assistant.");
        if (!role_copy || !content_copy) {
            free(role_copy);
            free(content_copy);
            return;
        }
    }

    nc_arena_reset(&agent->arena);

    /* Re-duplicate system prompt in fresh arena from heap copies. */
    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, role_copy, strlen(role_copy)),
        .content = nc_arena_dup(&agent->arena, content_copy, strlen(content_copy)),
    };
    agent->message_count = 1;

    free(role_copy);
    free(content_copy);
}

void nc_agent_free(nc_agent *agent) {
    nc_arena_free(&agent->arena);
    agent->message_count = 0;
}

/* ── Tests ───────────────────────────────────────────────────── */

#ifdef NC_TEST

/* Kept history must start with a user message, and every tool result must
 * follow (through other tool results) an assistant message with tool_calls. */
static bool history_is_valid(const nc_agent *agent) {
    if (agent->message_count < 1 || !msg_role_is(&agent->messages[0], "system"))
        return false;
    if (agent->message_count > 1 && !msg_role_is(&agent->messages[1], "user"))
        return false;
    for (int i = 1; i < agent->message_count; i++) {
        if (!msg_role_is(&agent->messages[i], "tool"))
            continue;
        int j = i - 1;
        while (j > 0 && msg_role_is(&agent->messages[j], "tool")) j--;
        if (!msg_role_is(&agent->messages[j], "assistant") ||
            agent->messages[j].tool_call_count == 0)
            return false;
    }
    return true;
}

static void test_compaction_rounds(int calls_per_round, int rounds) {
    static nc_config cfg;
    static nc_agent agent;
    static nc_tool_call calls[NC_MAX_TOOL_CALLS];
    bool valid = true;
    int compactions = 0;

    memset(&cfg, 0, sizeof(cfg));
    nc_agent_init(&agent, &cfg, NULL, NULL, 0, NULL);
    for (int i = 0; i < calls_per_round; i++) {
        memset(&calls[i], 0, sizeof(calls[i]));
        snprintf(calls[i].id, sizeof(calls[i].id), "call_%d", i);
        nc_strlcpy(calls[i].name, "shell", sizeof(calls[i].name));
    }

    agent_push_msg(&agent, "user", "start a long task", NULL, NULL, 0, NULL);
    for (int r = 0; r < rounds && valid; r++) {
        int before = agent.message_count;
        agent_push_msg(&agent, "assistant", NULL, NULL, calls, calls_per_round, NULL);
        if (agent.message_count < before) compactions++;
        valid = history_is_valid(&agent);
        for (int i = 0; i < calls_per_round && valid; i++) {
            before = agent.message_count;
            agent_push_msg(&agent, "tool", "ok", calls[i].id, NULL, 0, NULL);
            if (agent.message_count < before) compactions++;
            valid = history_is_valid(&agent);
        }
    }

    NC_ASSERT(compactions > 0, "compaction test triggers compaction");
    NC_ASSERT(valid, "compacted history never starts with orphaned tool results");
    NC_ASSERT(agent.message_count <= NC_MAX_MESSAGES, "compacted history fits");
    nc_agent_free(&agent);
}

void nc_test_agent(void) {
    test_compaction_rounds(1, 400);
    test_compaction_rounds(3, 200);
    test_compaction_rounds(7, 100);
    test_compaction_rounds(NC_MAX_TOOL_CALLS, 20);

    /* Plain chat: whichever role the cut lands on, history stays valid. */
    {
        static nc_config cfg;
        static nc_agent agent;
        memset(&cfg, 0, sizeof(cfg));
        nc_agent_init(&agent, &cfg, NULL, NULL, 0, NULL);
        for (int i = 0; i < NC_MAX_MESSAGES + 10; i++)
            agent_push_msg(&agent, (i % 2) ? "assistant" : "user", "hi", NULL, NULL, 0, NULL);
        NC_ASSERT(history_is_valid(&agent), "plain chat history stays valid after compaction");
        nc_agent_free(&agent);
    }
}

#endif
