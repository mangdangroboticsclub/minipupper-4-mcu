/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See include/mpx_mcp_ws.h for what this is and why it exists.
 *
 * The shape of it: our own esp_mcp_t engine (esp_mcp_create) for the
 * session-level methods, tools/list and tools/call answered directly from
 * claw_cap's registry (see "tools, from claw_cap" below for why not through
 * the engine), and an esp_mcp_transport_t vtable that does nothing
 * for start/stop/register_endpoint -- the xiaozhi socket's lifecycle belongs
 * to mpx_voice_link, not to us -- and does real work for exactly one thing:
 * emit_message(), which wraps a JSON-RPC string in
 * {"session_id":...,"type":"mcp","payload":...} and hands it to
 * mpx_voice_send_json().
 *
 * Dispatch runs on its own task with a PSRAM stack, same discipline as the
 * Opus codec task and for the same reason: esp_mcp_engine.c is a large
 * cJSON-heavy file, and finding that out the hard way once this session
 * (Opus, on the console task's stack) was enough.
 */

#include "mpx_mcp_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "claw_agent_mgr.h"
#include "claw_core.h"
#include "claw_cap.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mcp_data.h"
#include "esp_mcp_engine.h"
#include "esp_mcp_mgr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mpx_voice_link.h"
#include "sdkconfig.h"

static const char *TAG = "mpx_mcp_ws";

#define MPX_MCP_ENDPOINT     "mcp"
/* PSRAM stack (see mpx_mcp_ws_init). Bigger than the first cut's 16 KB now
 * that any capability can run on this task -- a Lua script or a file read
 * goes deeper than a display call did. */
#define MPX_MCP_TASK_STACK   (8192 * 4)
#define MPX_MCP_INBOX_DEPTH  4

static esp_mcp_t *s_mcp;
static esp_mcp_mgr_handle_t s_mgr;
static QueueHandle_t s_inbox;
static unsigned s_tool_count;

/* ── tools, from claw_cap ────────────────────────────────────────────────
 *
 * Every capability the ESP-Claw agent itself can call -- Lua, files, the
 * scheduler, skills, system, web search, HTTP, the robot, the display -- is
 * offered to the cloud as an MCP tool, straight from claw_cap's registry.
 * The list is whatever claw_cap_build_llm_tools_json() gives the root agent:
 * CALLABLE_BY_LLM, the visible-group filter, the same descriptions and JSON
 * schemas the on-device LLM sees. Nothing is hand-written per tool any more,
 * so a capability added to ESP-Claw shows up here without anyone touching
 * this file.
 *
 * The first cut of this bridge registered four display tools against the
 * esp_mcp engine and let it parse arguments. That does not scale to the
 * registry, for a reason worth recording: the engine's tools/call treats
 * EVERY declared property as required ("Missing argument") -- there is no
 * optional-with-default -- and a real capability schema is mostly optional
 * fields. So tools/list and tools/call are answered here, directly from
 * claw_cap, with the arguments object handed to the capability verbatim,
 * and only the session-level methods (initialize, ping, notifications) still
 * go to the engine. The reply shapes are MCP's:
 *
 *   tools/list  -> {"tools":[{"name","description","inputSchema"}], "nextCursor"?}
 *   tools/call  -> {"content":[{"type":"text","text":...}], "isError":bool}
 *
 * tools/list is paged the way xiaozhi's own firmware pages it: as many tools
 * as fit in MPX_MCP_LIST_PAGE_BYTES of JSON, then a nextCursor naming the
 * first tool of the next page. The server walks the pages itself. */

/* Smaller than xiaozhi's own 8000: a tools/list page is the largest single
 * message this device ever sends, and it goes out on a freshly opened TLS
 * socket. 4000 keeps each page to two websocket fragments, so a slow radio
 * moment costs a page, not the session (see VOICE_WS_SEND_JSON_MS). */
#define MPX_MCP_LIST_PAGE_BYTES  4000
#define MPX_MCP_CALL_OUTPUT_MAX  (16 * 1024)

/* Names on the wire are the capability names, with one exception table.
 *
 * The xiaozhi server merges the device's tools with its own server-side
 * functions into one list for the model, and refuses the whole device set
 * on any name clash:
 *
 *   {"type":"alert","status":"ERROR","message":"Duplicate tool names: get_current_time"}
 *
 * -- ESP-Claw's cap_system has get_current_time, and so does the server.
 * Prefixing everything with "self." (xiaozhi's convention for its own
 * device tools) fixed that, but the model stopped calling any tool at all
 * that session, so the prefix is gone again: names stay as the on-device
 * agent knows them, and only the known clash is renamed. Add to the table
 * when the server reports another one. */
static const struct { const char *cap; const char *wire; } s_renames[] = {
    { "get_current_time", "robot_get_current_time" },
};

static const char *wire_name(const char *cap)
{
    for (size_t i = 0; i < sizeof(s_renames) / sizeof(s_renames[0]); i++) {
        if (strcmp(cap, s_renames[i].cap) == 0) {
            return s_renames[i].wire;
        }
    }
    return cap;
}

static const char *cap_name(const char *wire)
{
    for (size_t i = 0; i < sizeof(s_renames) / sizeof(s_renames[0]); i++) {
        if (strcmp(wire, s_renames[i].wire) == 0) {
            return s_renames[i].cap;
        }
    }
    /* Tolerate the "self." form too, in case the server remembered it. */
    return strncmp(wire, "self.", 5) == 0 ? wire + 5 : wire;
}

/* Which capability groups the cloud sees: CONFIG_MP4_MCP_GROUPS, a
 * space-separated list of claw_cap group ids, empty meaning every group.
 *
 * The default leaves out ESP-Claw's own plumbing -- agent/session/router
 * management, LLM config, IM channels, skill registration, the MCP client.
 * Those are for the on-device agent administering itself; offered to the
 * cloud they are 30 more tools the model has to read on every turn and
 * never wants, and the first session with all 61 exposed is the one where
 * the model stopped calling tools altogether. */
static bool group_exposed(const char *group_id)
{
    const char *list = CONFIG_MP4_MCP_GROUPS;
    if (list[0] == '\0' || group_id == NULL) {
        return list[0] == '\0';
    }
    const size_t n = strlen(group_id);
    for (const char *p = list; *p; ) {
        while (*p == ' ' || *p == ',') p++;
        const char *end = p;
        while (*end && *end != ' ' && *end != ',') end++;
        if ((size_t)(end - p) == n && strncmp(p, group_id, n) == 0) {
            return true;
        }
        p = end;
    }
    return false;
}

static bool cap_exposed(const char *cap)
{
    claw_cap_descriptor_info_t info = {0};
    if (claw_cap_get_descriptor_state(cap, &info) != ESP_OK) {
        return false;
    }
    return group_exposed(info.group_id);
}

/* The capability registry's view for the cloud: the root agent's. */
static void claw_ctx_init(claw_cap_call_context_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->caller     = CLAW_CAP_CALLER_AGENT;
    ctx->channel    = "xiaozhi";
    ctx->session_id = mpx_voice_session_id();
}

/* Defined with the other JSON-RPC helpers further down. */
static cJSON *jsonrpc_reply(const cJSON *id);
static cJSON *jsonrpc_error(const cJSON *id, int code, const char *message);

/* ── ask_robot_agent: hand a task to the onboard ESP-Claw agent ───────────
 *
 * The capabilities above are the agent's hands; this is the agent itself.
 * The cloud model sees a one-line description of lua_run_script and nothing
 * about the Lua modules, the display API or the skills on the flash, so
 * "draw a crab on the screen" is beyond it. The on-device ESP-Claw agent has
 * all of that context (its skills, its module docs, its memory) and its own
 * LLM. So this tool is the delegation path: the user's request goes in as a
 * chat message on the same channel the PWA's chat uses, ESP-Claw does
 * whatever rounds of tool calls it needs, and its final answer comes back
 * as the tool result for the cloud model to speak.
 *
 * Plumbing: claw_agent_mgr_submit_root() straight into the root agent, as
 * a message on channel "web", chat id MPX_MCP_AGENT_CHAT -- the same
 * request the event router would build for a PWA chat message, minus the
 * router. The first cut went through cap_im_local_emit_user_message() and
 * the router; the router task has an 8 KB stack set in app_claw.c (in the
 * submodule, not ours to change) and creating the new "voice" session
 * inside it overflowed it on the very first request. Submitting from this
 * task, which has a 32 KB stack in PSRAM, sidesteps that entirely.
 * Replies still come out of cap_im_local's single outbound callback, which
 * http_server_webim_api.c owns; it mirrors messages for this chat id to
 * mpx_mcp_ws_on_agent_reply(). Stage notes (the lobster-prefixed
 * "Round 2: ..." narration) are progress, not the answer.
 *
 * One request at a time, synchronous with a timeout: the xiaozhi server is
 * waiting on the tools/call. A reply that arrives after the timeout is not
 * lost -- it is put on the screen, since there is no longer a voice turn to
 * speak it in. Deliberately NOT a claw_cap capability: the on-device agent
 * must not see a tool that asks itself. */

#define MPX_MCP_AGENT_TOOL       "ask_robot_agent"
#define MPX_MCP_AGENT_CHANNEL    "web"
#define MPX_MCP_AGENT_CHAT       "voice"
#define MPX_MCP_AGENT_SENDER     "xiaozhi"
#define MPX_MCP_AGENT_WAIT_MS    30000
#define MPX_MCP_AGENT_REPLY_MAX  1500
#define MPX_MCP_AGENT_STAGE      "\xF0\x9F\xA6\x9E"   /* U+1F99E, claw_core's stage prefix */

static SemaphoreHandle_t s_agent_reply_sem;
static SemaphoreHandle_t s_agent_busy;
static char             *s_agent_reply;     /* heap, set by the callback */
static volatile bool     s_agent_waiting;

void mpx_mcp_ws_on_agent_reply(const char *chat_id, const char *text)
{
    if (!chat_id || strcmp(chat_id, MPX_MCP_AGENT_CHAT) != 0 || !text || !text[0]) {
        return;
    }
    if (strncmp(text, MPX_MCP_AGENT_STAGE, sizeof(MPX_MCP_AGENT_STAGE) - 1) == 0) {
        ESP_LOGI(TAG, "agent: %.120s", text);
        return;
    }
    if (!s_agent_waiting) {
        /* Came in after the tool call gave up. Say it somewhere. */
        ESP_LOGW(TAG, "agent replied after the voice turn ended: %.200s", text);
        char json[192];
        snprintf(json, sizeof(json), "{\"text\":\"%.120s\"}", text);
        char out[64];
        claw_cap_call_context_t ctx;
        claw_ctx_init(&ctx);
        (void)claw_cap_call("display_show_text", json, &ctx, out, sizeof(out));
        return;
    }
    char *copy = strndup(text, MPX_MCP_AGENT_REPLY_MAX);
    if (!copy) {
        return;
    }
    char *old = s_agent_reply;
    s_agent_reply = copy;
    free(old);
    xSemaphoreGive(s_agent_reply_sem);
}

static cJSON *agent_tool_descriptor(void)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", MPX_MCP_AGENT_TOOL);
    cJSON_AddStringToObject(tool, "description",
        "Hand a task to the robot's onboard agent (ESP-Claw), which has its own AI, "
        "skills, a Lua runtime and full control of the robot and its screen. ALWAYS "
        "use this for anything the other tools cannot do directly -- drawing a "
        "picture, shape, animal or animation on the screen, running Lua, multi-step "
        "jobs, schedules, files, skills. Never say the robot cannot do something "
        "before trying this. Pass the user's request in their own words and "
        "language. Takes a few seconds; tell the user the agent's reply.");
    cJSON *schema = cJSON_AddObjectToObject(tool, "inputSchema");
    cJSON_AddStringToObject(schema, "type", "object");
    cJSON *props = cJSON_AddObjectToObject(schema, "properties");
    cJSON *req = cJSON_AddObjectToObject(props, "request");
    cJSON_AddStringToObject(req, "type", "string");
    cJSON_AddStringToObject(req, "description", "what the user wants done, verbatim");
    cJSON *required = cJSON_AddArrayToObject(schema, "required");
    cJSON_AddItemToArray(required, cJSON_CreateString("request"));
    return tool;
}

/* Hand `request` to the onboard agent and wait up to `wait_ms` for its
 * answer. On success *reply_owned is a heap string the caller frees; on
 * failure *message says why, in words fit to show or speak. Used by the
 * ask_robot_agent tool and by the device-brain voice turn below. */
static bool agent_ask(const char *request, uint32_t wait_ms, char **reply_owned,
                      const char **message)
{
    *reply_owned = NULL;
    *message = NULL;
    if (!s_agent_reply_sem) {
        s_agent_reply_sem = xSemaphoreCreateBinary();
        s_agent_busy = xSemaphoreCreateMutex();
        if (!s_agent_reply_sem || !s_agent_busy) {
            *message = "out of memory";
            return false;
        }
    }

    const char *text = NULL;
    char *owned = NULL;

    if (xSemaphoreTake(s_agent_busy, 0) != pdTRUE) {
        text = "The robot's agent is still busy with the previous request. Ask again in a moment.";
    } else {
        /* Clear anything stale from an earlier late reply. */
        (void)xSemaphoreTake(s_agent_reply_sem, 0);
        free(s_agent_reply);
        s_agent_reply = NULL;
        s_agent_waiting = true;

        ESP_LOGI(TAG, "asking the onboard agent: %.120s", request);
        const claw_agent_mgr_root_input_t input = {
            .session_policy    = CLAW_SESSION_POLICY_CHAT,
            .user_text         = request,
            .source_cap        = "mpx_mcp_ws",
            .event_type        = "message",
            .source_channel    = MPX_MCP_AGENT_CHANNEL,
            .source_chat_id    = MPX_MCP_AGENT_CHAT,
            .source_sender_id  = MPX_MCP_AGENT_SENDER,
            .source_message_id = NULL,
            .event_id          = NULL,
            .target_channel    = MPX_MCP_AGENT_CHANNEL,
            .target_chat_id    = MPX_MCP_AGENT_CHAT,
            /* Same set the event router uses for a chat message: publish
             * the answer and the stage notes as out-messages (that is how
             * they reach the callback), and interrupt anything the agent
             * was idly doing. */
            .flags = CLAW_CORE_REQUEST_FLAG_PUBLISH_OUT_MESSAGE |
                     CLAW_CORE_REQUEST_FLAG_PUBLISH_STAGE_MESSAGE |
                     CLAW_CORE_REQUEST_FLAG_SKIP_RESPONSE_QUEUE |
                     CLAW_CORE_REQUEST_FLAG_USER_INTERRUPT,
            .request_id = 0,   /* 0 = let the manager number it */
        };
        const esp_err_t err = claw_agent_mgr_submit_root(&input, 2000);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "agent submit failed: %s", esp_err_to_name(err));
            s_agent_waiting = false;
            text = "The robot's onboard agent is not running (check its AI settings in the web app).";
        } else if (xSemaphoreTake(s_agent_reply_sem, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            s_agent_waiting = false;
            owned = s_agent_reply;
            s_agent_reply = NULL;
            text = owned ? NULL : "(empty reply)";
        } else {
            s_agent_waiting = false;
            text = "The robot's agent is still working on it. Its answer will appear on the "
                   "robot's screen when it is done.";
        }
        xSemaphoreGive(s_agent_busy);
    }

    if (owned) {
        *reply_owned = owned;
        return true;
    }
    *message = text;
    return false;
}

static cJSON *handle_agent_call(const cJSON *id, const cJSON *arguments)
{
    const cJSON *request = arguments ? cJSON_GetObjectItem(arguments, "request") : NULL;
    if (!cJSON_IsString(request) || !request->valuestring[0]) {
        return jsonrpc_error(id, -32602, "'request' is required");
    }
    char *owned = NULL;
    const char *failure = NULL;
    const bool ok = agent_ask(request->valuestring, MPX_MCP_AGENT_WAIT_MS, &owned, &failure);
    const char *text = ok ? owned : (failure ? failure : "(empty reply)");
    const bool is_error = !ok;

    cJSON *resp = jsonrpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text);
    cJSON_AddItemToArray(content, item);
    cJSON_AddBoolToObject(result, "isError", is_error);
    free(owned);
    return resp;
}

/* claw_cap's list, already filtered for the LLM. Caller owns the cJSON. */
static cJSON *claw_tools(void)
{
    claw_cap_call_context_t ctx;
    claw_ctx_init(&ctx);
    char *json = claw_cap_build_llm_tools_json(&ctx, false);
    if (!json) {
        return NULL;
    }
    cJSON *arr = cJSON_Parse(json);
    cJSON_free(json);
    if (!arr) {
        return NULL;
    }
    /* Drop the groups the config does not expose. Walking by index because
     * cJSON_ArrayForEach cannot survive a deletion. */
    for (int i = cJSON_GetArraySize(arr) - 1; i >= 0; i--) {
        const cJSON *name = cJSON_GetObjectItem(cJSON_GetArrayItem(arr, i), "name");
        if (!cJSON_IsString(name) || !cap_exposed(name->valuestring)) {
            cJSON_DeleteItemFromArray(arr, i);
        }
    }
    return arr;
}

static unsigned claw_tool_count(void)
{
    cJSON *arr = claw_tools();
    const unsigned n = arr ? (unsigned)cJSON_GetArraySize(arr) : 0;
    cJSON_Delete(arr);
    return n;
}

static cJSON *jsonrpc_reply(const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return NULL;
    }
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    return resp;
}

static cJSON *jsonrpc_error(const cJSON *id, int code, const char *message)
{
    cJSON *resp = jsonrpc_reply(id);
    if (!resp) {
        return NULL;
    }
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return resp;
}

static cJSON *handle_tools_list(const cJSON *id, const cJSON *params)
{
    cJSON *all = claw_tools();
    if (!all) {
        return jsonrpc_error(id, -32603, "capability registry not available");
    }

    const cJSON *cursor = params ? cJSON_GetObjectItem(params, "cursor") : NULL;
    const char *start = (cJSON_IsString(cursor) && cursor->valuestring[0])
                            ? cap_name(cursor->valuestring) : NULL;

    cJSON *resp = jsonrpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");

    size_t bytes = 0;
    bool started = (start == NULL);

    /* The delegation tool goes first, on the first page: the model reads
     * the list top-down and the tool that can do everything else should
     * be the first thing it sees, not the last. */
    if (start == NULL) {
        cJSON *agent = agent_tool_descriptor();
        char *one = cJSON_PrintUnformatted(agent);
        bytes += one ? strlen(one) + 1 : 0;
        cJSON_free(one);
        cJSON_AddItemToArray(tools, agent);
    }
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, all) {
        const cJSON *name = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(name)) {
            continue;
        }
        if (!started) {
            started = (strcmp(name->valuestring, start) == 0);
            if (!started) {
                continue;
            }
        }

        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", wire_name(name->valuestring));
        const cJSON *desc = cJSON_GetObjectItem(item, "description");
        cJSON_AddStringToObject(tool, "description", cJSON_IsString(desc) ? desc->valuestring : "");
        const cJSON *schema = cJSON_GetObjectItem(item, "input_schema");
        cJSON_AddItemToObject(tool, "inputSchema",
                              schema ? cJSON_Duplicate(schema, true)
                                     : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));

        char *one = cJSON_PrintUnformatted(tool);
        const size_t len = one ? strlen(one) + 1 : 0;
        cJSON_free(one);

        /* Always at least one tool per page, or a giant schema would stall
         * the walk forever on the same cursor. */
        if (bytes > 0 && bytes + len > MPX_MCP_LIST_PAGE_BYTES) {
            cJSON_Delete(tool);
            cJSON_AddStringToObject(result, "nextCursor", wire_name(name->valuestring));
            break;
        }
        cJSON_AddItemToArray(tools, tool);
        bytes += len;
    }
    cJSON_Delete(all);

    ESP_LOGI(TAG, "tools/list: %d tool(s)%s%s", cJSON_GetArraySize(tools),
             start ? " from " : "", start ? start : "");
    return resp;
}

static cJSON *handle_tools_call(const cJSON *id, const cJSON *params)
{
    const cJSON *name = params ? cJSON_GetObjectItem(params, "name") : NULL;
    if (!cJSON_IsString(name) || !name->valuestring[0]) {
        return jsonrpc_error(id, -32602, "params.name is required");
    }
    const char *cap = cap_name(name->valuestring);
    if (strcmp(cap, MPX_MCP_AGENT_TOOL) == 0) {
        return handle_agent_call(id, cJSON_GetObjectItem(params, "arguments"));
    }
    if (!claw_cap_find(cap) || !cap_exposed(cap)) {
        return jsonrpc_error(id, -32602, "unknown tool");
    }

    /* The arguments object goes to the capability as-is: it validates its
     * own schema, with its own idea of what is optional. */
    const cJSON *arguments = cJSON_GetObjectItem(params, "arguments");
    char *input = (arguments && cJSON_IsObject(arguments)) ? cJSON_PrintUnformatted(arguments) : NULL;

    char *output = heap_caps_malloc(MPX_MCP_CALL_OUTPUT_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) {
        output = malloc(MPX_MCP_CALL_OUTPUT_MAX);
    }
    if (!output) {
        cJSON_free(input);
        return jsonrpc_error(id, -32603, "out of memory");
    }
    output[0] = '\0';

    claw_cap_call_context_t ctx;
    claw_ctx_init(&ctx);
    ESP_LOGI(TAG, "tools/call %s %s", cap, input ? input : "{}");
    const esp_err_t err = claw_cap_call(cap, input ? input : "{}", &ctx,
                                        output, MPX_MCP_CALL_OUTPUT_MAX);
    cJSON_free(input);

    if (err != ESP_OK && output[0] == '\0') {
        snprintf(output, MPX_MCP_CALL_OUTPUT_MAX, "Error: %s", esp_err_to_name(err));
    }

    cJSON *resp = jsonrpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", output);
    cJSON_AddItemToArray(content, text);
    cJSON_AddBoolToObject(result, "isError", err != ESP_OK);
    free(output);
    return resp;
}

/* tools/list and tools/call are answered here; anything else returns NULL
 * and goes to the engine. The returned string is the JSON-RPC reply (or
 * NULL for a notification), heap-allocated for the caller to free. */
static bool claw_dispatch(const char *msg, char **reply)
{
    *reply = NULL;
    cJSON *root = cJSON_Parse(msg);
    if (!root) {
        return false;
    }
    const cJSON *method = cJSON_GetObjectItem(root, "method");
    if (!cJSON_IsString(method)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *resp = NULL;

    if (strcmp(method->valuestring, "tools/list") == 0) {
        resp = handle_tools_list(id, params);
    } else if (strcmp(method->valuestring, "tools/call") == 0) {
        resp = handle_tools_call(id, params);
    } else {
        cJSON_Delete(root);
        return false;
    }

    if (resp) {
        *reply = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
    }
    cJSON_Delete(root);
    return true;
}

/* ── outbound: wrap and hand to the socket ──────────────────────────────── */

static esp_err_t send_envelope(const char *session_id, const char *jsonrpc_message)
{
    ESP_RETURN_ON_FALSE(jsonrpc_message, ESP_ERR_INVALID_ARG, TAG, "no message");

    const char *sid = (session_id && session_id[0]) ? session_id : mpx_voice_session_id();
    const size_t need = strlen(jsonrpc_message) + (sid ? strlen(sid) : 0) + 48;
    char *buf = malloc(need);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "out of memory");

    snprintf(buf, need, "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
             sid ? sid : "", jsonrpc_message);
    const esp_err_t err = mpx_voice_send_json(buf);
    free(buf);
    return err;
}

/* ── the transport vtable ───────────────────────────────────────────────
 *
 * init/deinit/start/stop/register_endpoint/unregister_endpoint are required
 * -- esp_mcp_mgr_init() rejects a NULL entry outright -- but have nothing to
 * do here: the socket they would start or route through already exists and
 * is owned by mpx_voice_link. Only emit_message does real work. request and
 * open_stream stay NULL: they are for a client-mode transport making
 * outbound calls, which is the wrong direction for a device exposing tools
 * to the cloud. */

static esp_err_t transport_init(esp_mcp_mgr_handle_t mgr, esp_mcp_transport_handle_t *out)
{
    (void)mgr;
    *out = (esp_mcp_transport_handle_t)1;
    return ESP_OK;
}

static esp_err_t transport_deinit(esp_mcp_transport_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

static esp_err_t transport_create_config(const void *in, void **out)
{
    (void)in;
    /* start()/stop() never look at this -- but esp_mcp_mgr_init() treats a
     * NULL result as allocation failure, so hand back one real byte rather
     * than a magic non-NULL pointer delete_config would be wrong to free(). */
    *out = malloc(1);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t transport_delete_config(void *config)
{
    free(config);
    return ESP_OK;
}

static esp_err_t transport_start(esp_mcp_transport_handle_t handle, void *config)
{
    (void)handle;
    (void)config;
    return ESP_OK;
}

static esp_err_t transport_stop(esp_mcp_transport_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

static esp_err_t transport_register_endpoint(esp_mcp_transport_handle_t handle,
                                              const char *ep_name, void *priv_data)
{
    (void)handle;
    (void)ep_name;
    (void)priv_data;
    return ESP_OK;
}

static esp_err_t transport_unregister_endpoint(esp_mcp_transport_handle_t handle,
                                                const char *ep_name)
{
    (void)handle;
    (void)ep_name;
    return ESP_OK;
}

static esp_err_t transport_emit_message(esp_mcp_transport_handle_t handle,
                                        const char *session_id, const char *jsonrpc_message)
{
    (void)handle;
    return send_envelope(session_id, jsonrpc_message);
}

static const esp_mcp_transport_t s_transport = {
    .init = transport_init,
    .deinit = transport_deinit,
    .start = transport_start,
    .stop = transport_stop,
    .create_config = transport_create_config,
    .delete_config = transport_delete_config,
    .register_endpoint = transport_register_endpoint,
    .unregister_endpoint = transport_unregister_endpoint,
    .request = NULL,
    .open_stream = NULL,
    .emit_message = transport_emit_message,
    .terminate = NULL,
};

/* ── inbound: xiaozhi socket -> engine -> reply ─────────────────────────── */

/* esp_mcp_engine.c refuses any method but "initialize" as the first message
 * a session sends (ESP_ERR_INVALID_STATE) -- correct MCP behaviour, and the
 * real xiaozhi cloud already does this itself (it is what that early "mcp
 * message received (319 bytes)" log line was, back before this bridge
 * existed to answer it). `mcp call`/`mcp send`, though, build one synthetic
 * message and have no handshake to send first. Rather than push that
 * two-step requirement onto whoever is testing from the console, handle it
 * here: if the engine reports "not initialized yet", send a synthetic
 * initialize on its behalf and retry the original message once. A real cloud
 * session never hits this branch -- its own initialize already satisfied the
 * engine by the time anything else arrives. */
static esp_err_t dispatch_one(const char *msg, uint8_t **outbuf, uint16_t *outlen)
{
    return esp_mcp_mgr_req_handle(s_mgr, MPX_MCP_ENDPOINT,
                                  (const uint8_t *)msg, (uint16_t)strlen(msg),
                                  outbuf, outlen);
}

/* ── Device brain: voice answered by the onboard agent ("own" AI mode) ────
 *
 * The server transcribed what was said and is waiting for this robot to say
 * what to speak (see mpx_voice_set_device_brain). The transcript arrives on
 * the websocket task, is queued here with a marker byte in front -- a
 * JSON-RPC message never starts with 0x01 -- and is answered on this task's
 * big PSRAM stack, the same way ask_robot_agent is. */

#define MPX_VOICE_TURN_MARK     '\x01'
#define MPX_VOICE_TURN_WAIT_MS  60000
#define MPX_VOICE_PROMPT \
    "(Said out loud to the robot; your reply is spoken by its speaker. " \
    "Answer in one or two short sentences in plain words -- no markdown, " \
    "no lists, no code.) "

static void device_brain_sink(char *text_owned)
{
    if (!text_owned) {
        return;
    }
    const size_t n = strlen(text_owned);
    char *msg = s_inbox ? malloc(n + 2) : NULL;
    if (msg) {
        msg[0] = MPX_VOICE_TURN_MARK;
        memcpy(msg + 1, text_owned, n + 1);
        if (xQueueSend(s_inbox, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "voice turn dropped: inbox full");
            free(msg);
        }
    }
    free(text_owned);
}

static void handle_voice_turn(const char *heard)
{
    const size_t n = strlen(MPX_VOICE_PROMPT) + strlen(heard) + 1;
    char *request = malloc(n);
    char *reply = NULL;
    const char *failure = "Sorry, I can't answer right now.";

    if (request) {
        snprintf(request, n, "%s%s", MPX_VOICE_PROMPT, heard);
        const char *why = NULL;
        if (!agent_ask(request, MPX_VOICE_TURN_WAIT_MS, &reply, &why) && why) {
            ESP_LOGW(TAG, "voice turn: %s", why);
            failure = (strstr(why, "busy") != NULL)
                ? "I'm still busy with the last thing. Ask me again in a moment."
                : (strstr(why, "not running") != NULL)
                ? "My brain isn't set up. Check the AI settings in the app."
                : "I'm still thinking about that. I'll show the answer on my screen.";
        }
        free(request);
    }
    /* Always end the turn: the server is holding the conversation until it
     * hears "last". */
    if (mpx_voice_send_speak(reply ? reply : failure, true) != ESP_OK) {
        ESP_LOGW(TAG, "voice turn: could not send the reply -- socket down?");
    }
    free(reply);
}

esp_err_t mpx_mcp_ws_enable_device_brain(void)
{
    ESP_RETURN_ON_FALSE(s_inbox, ESP_ERR_INVALID_STATE, TAG, "bridge not started");
    mpx_voice_set_device_brain(true, device_brain_sink);
    ESP_LOGI(TAG, "voice: device brain -- the server transcribes and speaks, "
                  "this robot's agent answers");
    return ESP_OK;
}

static void mcp_task(void *arg)
{
    (void)arg;
    char *msg = NULL;

    for (;;) {
        if (xQueueReceive(s_inbox, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg && msg[0] == MPX_VOICE_TURN_MARK) {
            handle_voice_turn(msg + 1);
            free(msg);
            msg = NULL;
            continue;
        }

        /* The tool methods never reach the engine -- see "tools, from
         * claw_cap" above. */
        char *reply = NULL;
        if (claw_dispatch(msg, &reply)) {
            if (reply) {
                if (send_envelope(mpx_voice_session_id(), reply) != ESP_OK) {
                    ESP_LOGW(TAG, "could not send MCP reply -- socket down?");
                }
                cJSON_free(reply);
            }
            free(msg);
            msg = NULL;
            continue;
        }

        uint8_t *outbuf = NULL;
        uint16_t outlen = 0;
        esp_err_t err = dispatch_one(msg, &outbuf, &outlen);

        if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGI(TAG, "session not initialized yet -- sending a synthetic "
                          "initialize on this message's behalf");
            static const char *kInit =
                "{\"jsonrpc\":\"2.0\",\"id\":0,\"method\":\"initialize\","
                "\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
                "\"clientInfo\":{\"name\":\"mpx-mcp-ws\",\"version\":\"0.1.0\"}}}";
            uint8_t *init_out = NULL;
            uint16_t init_outlen = 0;
            const esp_err_t init_err = dispatch_one(kInit, &init_out, &init_outlen);
            if (init_out) {
                esp_mcp_free_response(s_mcp, init_out);
            }
            if (init_err != ESP_OK) {
                ESP_LOGW(TAG, "synthetic initialize failed too: %s -- giving up on "
                              "the original message", esp_err_to_name(init_err));
            } else {
                err = dispatch_one(msg, &outbuf, &outlen);
            }
        }

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "dispatch failed: %s -- message was: %.200s", esp_err_to_name(err), msg);
        }

        if (outbuf && outlen > 0) {
            /* esp_mcp_handle_message's outlen is authoritative; outbuf is not
             * documented NUL-terminated, so bound the copy explicitly rather
             * than assume it. */
            char *resp = malloc((size_t)outlen + 1);
            if (resp) {
                memcpy(resp, outbuf, outlen);
                resp[outlen] = '\0';
                if (send_envelope(mpx_voice_session_id(), resp) != ESP_OK) {
                    ESP_LOGW(TAG, "could not send MCP reply -- socket down?");
                }
                free(resp);
            }
            esp_mcp_free_response(s_mcp, outbuf);
        }

        free(msg);
        msg = NULL;
    }
}

static void mcp_incoming(const char *jsonrpc_owned)
{
    if (!jsonrpc_owned) {
        return;
    }
    if (!s_inbox) {
        ESP_LOGW(TAG, "MCP bridge not started yet -- dropping message");
        free((void *)jsonrpc_owned);
        return;
    }
    /* Ownership of jsonrpc_owned passes to the queue on success, to us on
     * failure -- either way this function is responsible for freeing it
     * exactly once. */
    if (xQueueSend(s_inbox, &jsonrpc_owned, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MCP inbox full -- dropping message");
        free((void *)jsonrpc_owned);
    }
}

/* ── setup ───────────────────────────────────────────────────────────────*/

esp_err_t mpx_mcp_ws_init(void)
{
    if (s_mcp) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_mcp_create(&s_mcp), TAG, "engine create failed");

    const esp_mcp_mgr_config_t cfg = {
        .transport = s_transport,
        .config = NULL,
        .instance = s_mcp,
    };
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_init(cfg, &s_mgr), TAG, "manager init failed");
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_start(s_mgr), TAG, "manager start failed");
    ESP_RETURN_ON_ERROR(esp_mcp_mgr_register_endpoint(s_mgr, MPX_MCP_ENDPOINT, NULL),
                        TAG, "endpoint register failed");

    s_inbox = xQueueCreate(MPX_MCP_INBOX_DEPTH, sizeof(char *));
    ESP_RETURN_ON_FALSE(s_inbox, ESP_ERR_NO_MEM, TAG, "queue alloc failed");

    /* Same PSRAM-stack discipline as the Opus codec task -- see
     * mpx_voice_codec.cc. esp_mcp_engine.c is large and cJSON-heavy; running
     * it on whatever small stack called us in is how the earlier Opus crash
     * happened, and there is no reason to repeat that lesson here. */
    BaseType_t ok = xTaskCreateWithCaps(mcp_task, "mcp_ws", MPX_MCP_TASK_STACK, NULL, 5,
                                        NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreateWithCaps(mcp_task, "mcp_ws", MPX_MCP_TASK_STACK, NULL, 5,
                                 NULL, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");

    mpx_voice_set_mcp_sink(mcp_incoming);

    s_tool_count = claw_tool_count();
    ESP_LOGI(TAG, "MCP bridge ready: %u ESP-Claw capabilit%s offered as tools over the xiaozhi socket",
             s_tool_count, s_tool_count == 1 ? "y" : "ies");
    return ESP_OK;
}

/* ── console command ─────────────────────────────────────────────────────
 *
 * `mcp call <tool> [json-args]` and `mcp send <raw-jsonrpc>` both go through
 * the exact same path a real cloud message would: mcp_incoming() -> the
 * queue -> mcp_task() -> esp_mcp_mgr_req_handle(). That makes this the gate
 * for Phase 3 that does not need STT working first -- `mcp call
 * display_show_emotion {"emotion":"sad"}` should change the face the same
 * way the cloud calling it over the socket will, once the cloud can. */

static void mcp_usage(void)
{
    printf("\n"
           "  mcp call <tool> [json-args]   synthesize a tools/call and dispatch it\n"
           "  mcp send <raw-jsonrpc>        dispatch one JSON-RPC message verbatim\n"
           "  mcp info                      tool count, endpoint name\n"
           "\n"
           "Both `call` and `send` go through the same queue and task a real\n"
           "message from the cloud would, so this is the Phase 3 gate that does\n"
           "not need STT working yet:\n"
           "\n"
           "  mcp call display_show_emotion {\"emotion\":\"sad\"}\n"
           "\n"
           "should change the face. If a socket is connected (`voice connect`),\n"
           "the reply also goes out over it, same as a real cloud call. The\n"
           "engine requires an \"initialize\" as the first message of a session --\n"
           "a real MCP client always sends one, this test command does not, so\n"
           "the bridge sends one on its behalf automatically the first time it\n"
           "sees ESP_ERR_INVALID_STATE. You will not see that error anymore.\n"
           "\n");
}

static int mcp_cmd(int argc, char **argv)
{
    if (argc < 2) {
        mcp_usage();
        return 0;
    }

    if (strcmp(argv[1], "info") == 0) {
        printf("  engine   : %s\n", s_mcp ? "up" : "not started");
        printf("  endpoint : %s\n", MPX_MCP_ENDPOINT);
        cJSON *all = claw_tools();
        printf("  groups   : %s\n", CONFIG_MP4_MCP_GROUPS[0] ? CONFIG_MP4_MCP_GROUPS : "(all)");
        printf("  tools    : %d\n", all ? cJSON_GetArraySize(all) : 0);
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, all) {
            const cJSON *name = cJSON_GetObjectItem(item, "name");
            const cJSON *desc = cJSON_GetObjectItem(item, "description");
            printf("    %-28s %.70s\n", cJSON_IsString(name) ? wire_name(name->valuestring) : "?",
                   cJSON_IsString(desc) ? desc->valuestring : "");
        }
        cJSON_Delete(all);
        printf("    %-28s %.70s\n", MPX_MCP_AGENT_TOOL, "delegate a task to the onboard ESP-Claw agent (built in)");
        return 0;
    }

    if (strcmp(argv[1], "send") == 0) {
        if (argc < 3) {
            printf("  usage: mcp send <raw-jsonrpc>\n");
            return 1;
        }
        char *copy = strdup(argv[2]);
        if (!copy) {
            ESP_LOGE(TAG, "out of memory");
            return 1;
        }
        mcp_incoming(copy);
        return 0;
    }

    if (strcmp(argv[1], "call") == 0) {
        if (argc < 3) {
            printf("  usage: mcp call <tool> [json-args]\n");
            return 1;
        }
        const char *args_json = (argc >= 4) ? argv[3] : "{}";
        const size_t need = strlen(argv[2]) + strlen(args_json) + 96;
        char *msg = malloc(need);
        if (!msg) {
            ESP_LOGE(TAG, "out of memory");
            return 1;
        }
        snprintf(msg, need,
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
                 "\"params\":{\"name\":\"%s\",\"arguments\":%s}}",
                 argv[2], args_json);
        mcp_incoming(msg);
        return 0;
    }

    mcp_usage();
    return 0;
}

void register_mcp_command(void)
{
    const esp_console_cmd_t cmd = {
        .command = "mcp",
        .help = "MCP bridge test (see `mcp` with no arguments)",
        .hint = NULL,
        .func = mcp_cmd,
    };
    if (esp_console_cmd_register(&cmd) == ESP_OK) {
        ESP_LOGI(TAG, "'mcp' console command registered");
    }
}
