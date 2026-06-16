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
    static const char sys_prompt[] =
        "You are noclaw, the smallest AI assistant. "
        "Pure C. Under 100KB. You are helpful, concise, and fast. "
        "Use available tools when needed to complete tasks.";

    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, "system", 6),
        .content = nc_arena_dup(&agent->arena, sys_prompt, sizeof(sys_prompt) - 1),
    };
    agent->message_count = 1;
}

/* ── Build tools JSON for the provider ────────────────────────── */

static const char *build_tools_json(nc_agent *agent) {
    if (agent->tool_count == 0) return NULL;

    /* 32KB — enough for NC_MAX_TOOLS=32 tool definitions */
    static const size_t bufsz = 32768;
    char *buf = (char *)nc_arena_alloc(&agent->arena, bufsz);
    if (!buf) return NULL;
    int off = 0;
    off += snprintf(buf + off, bufsz - (size_t)off, "[");

    for (int i = 0; i < agent->tool_count; i++) {
        if (i > 0) off += snprintf(buf + off, bufsz - (size_t)off, ",");
        off += snprintf(buf + off, bufsz - (size_t)off,
            "{\"type\":\"function\",\"function\":{\"name\":\"%s\",\"description\":\"%s\",\"parameters\":%s}}",
            agent->tools[i].def.name,
            agent->tools[i].def.description,
            agent->tools[i].def.parameters_json);
    }

    off += snprintf(buf + off, bufsz - (size_t)off, "]");
    return buf;
}

/* ── Add message to history ───────────────────────────────────── */

static void agent_push_msg(nc_agent *agent, const char *role, const char *content,
                           const char *tool_call_id,
                           const nc_tool_call *tool_calls, int tool_call_count) {
    if (agent->message_count >= NC_MAX_MESSAGES) {
        /* Compact: keep system + last half of messages */
        int keep = NC_MAX_MESSAGES / 2;
        memmove(&agent->messages[1], &agent->messages[agent->message_count - keep],
                (size_t)keep * sizeof(nc_message));
        agent->message_count = 1 + keep;
    }

    nc_message *msg = &agent->messages[agent->message_count++];
    memset(msg, 0, sizeof(*msg));
    msg->role = nc_arena_dup(&agent->arena, role, strlen(role));
    msg->content = content ? nc_arena_dup(&agent->arena, content, strlen(content)) : NULL;
    msg->tool_call_id = tool_call_id
        ? nc_arena_dup(&agent->arena, tool_call_id, strlen(tool_call_id))
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

/* ── Chat: single turn (loops for tool calls) ─────────────────── */

const char *nc_agent_chat(nc_agent *agent, const char *user_input) {
    agent_push_msg(agent, "user", user_input, NULL, NULL, 0);

    const char *tools_json = build_tools_json(agent);
    int max_iterations = agent->config->max_iterations > 0 ? agent->config->max_iterations : 100;

    for (int iter = 0; iter < max_iterations; iter++) {
        nc_chat_request req = {
            .messages = agent->messages,
            .message_count = agent->message_count,
            .model = agent->config->default_model,
            .temperature = agent->config->default_temperature,
            .tools_json = tools_json,
            .max_tokens = agent->config->max_tokens,
        };

        nc_chat_response resp;
        if (!agent->provider->chat(agent->provider, &req, &resp)) {
            static const char err_msg[] = "Sorry, I encountered an error communicating with the provider.";
            return nc_arena_dup(&agent->arena, err_msg, sizeof(err_msg) - 1);
        }

        if (!resp.has_tool_calls) {
            /* Final response — no tool calls */
            const char *reply = nc_arena_dup(&agent->arena, resp.content, strlen(resp.content));
            agent_push_msg(agent, "assistant", resp.content, NULL, NULL, 0);
            return reply;
        }

        /* ── Tool call dispatch ───────────────────────────────── */

        nc_log(NC_LOG_INFO, "Tool calls: %d (iteration %d/%d)",
               resp.tool_call_count, iter + 1, max_iterations);

        /* Push assistant message with tool_calls attached */
        agent_push_msg(agent, "assistant",
                       resp.content[0] ? resp.content : NULL,
                       NULL,
                       resp.tool_calls, resp.tool_call_count);

        /* Execute each tool call and push results */
        for (int i = 0; i < resp.tool_call_count; i++) {
            nc_tool_call *tc = &resp.tool_calls[i];

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
            char result_buf[NC_TOOL_RESULT_MAX];

            if (tc->arguments_truncated) {
                snprintf(result_buf, sizeof(result_buf),
                         "error: tool arguments for '%s' exceeded limit (%zu bytes > %zu bytes) and were not executed",
                         tc->name,
                         tc->arguments_len,
                         sizeof(tc->arguments) - 1);
                nc_log(NC_LOG_WARN,
                       "  <- tool %s blocked: arguments truncated (%zu bytes > %zu)",
                       tc->name, tc->arguments_len, sizeof(tc->arguments) - 1);
            } else if (tool) {
                bool ok = tool->execute(tool, tc->arguments, result_buf, sizeof(result_buf));
                if (!ok) {
                    nc_log(NC_LOG_WARN, "  <- tool %s returned error", tc->name);
                    /* Still push the error as the tool result — the LLM needs to see it */
                }
            } else {
                snprintf(result_buf, sizeof(result_buf),
                         "error: unknown tool '%s'", tc->name);
                nc_log(NC_LOG_WARN, "  <- unknown tool: %s", tc->name);
            }

            /* Push tool result message */
            agent_push_msg(agent, "tool", result_buf, tc->id, NULL, 0);
        }

        /* Loop back to provider with the tool results */
    }

    static const char max_iter_msg[] = "I reached the maximum number of tool iterations. Here's what I have so far.";
    return nc_arena_dup(&agent->arena, max_iter_msg, sizeof(max_iter_msg) - 1);
}

/* ── Reset conversation ───────────────────────────────────────── */

void nc_agent_reset(nc_agent *agent) {
    /* Copy system prompt to stack BEFORE resetting arena (avoids use-after-free) */
    char role_buf[32];
    char content_buf[512];
    nc_strlcpy(role_buf, agent->messages[0].role, sizeof(role_buf));
    nc_strlcpy(content_buf, agent->messages[0].content, sizeof(content_buf));

    nc_arena_reset(&agent->arena);

    /* Re-duplicate system prompt in fresh arena from stack copies */
    agent->messages[0] = (nc_message){
        .role = nc_arena_dup(&agent->arena, role_buf, strlen(role_buf)),
        .content = nc_arena_dup(&agent->arena, content_buf, strlen(content_buf)),
    };
    agent->message_count = 1;
}

void nc_agent_free(nc_agent *agent) {
    nc_arena_free(&agent->arena);
    agent->message_count = 0;
}
