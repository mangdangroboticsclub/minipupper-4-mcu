/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See voice_tools.h. Wire format and paging follow mpv2-claw's mpx_mcp_ws.c.
 * The tools themselves (bodies, descriptions, schemas) are in
 * main/agent/robot_tools.cc since Phase 5, shared with the on-device agent.
 */

#include "voice_tools.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "agent/robot_tools.h"
#include "mpx_voice_link.h"

static const char *TAG = "voice_tools";

namespace {

using robot_tools::Reply;
using robot_tools::Tool;

/* Smaller than xiaozhi's own 8000 -- see mpx_mcp_ws.c: a tools/list page is
 * the largest message this device sends, on a freshly opened TLS socket. */
constexpr size_t kListPageBytes = 4000;
constexpr int    kInboxDepth    = 8;
constexpr int    kTaskStack     = 8192;    /* internal RAM, see the header */
constexpr int    kTaskPrio      = 4;
constexpr const char *kProtocolVersion = "2024-11-05";

const char *str(const cJSON *obj, const char *key)
{
    const cJSON *v = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
    return (cJSON_IsString(v) && v->valuestring[0]) ? v->valuestring : nullptr;
}

std::string print_and_free(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(obj);
    return out;
}

/* ── JSON-RPC ────────────────────────────────────────────────────────── */

cJSON *rpc_reply(const cJSON *id)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) {
        cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, true));
    } else {
        cJSON_AddNullToObject(resp, "id");
    }
    return resp;
}

cJSON *rpc_error(const cJSON *id, int code, const char *message)
{
    cJSON *resp = rpc_reply(id);
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", message);
    return resp;
}

cJSON *handle_initialize(const cJSON *id, const cJSON *params)
{
    /* Echo the client's protocol version when it gives one -- the reference
     * engine does, and a mismatch here is what makes some servers drop the
     * MCP session silently. */
    const char *proto = str(params, "protocolVersion");
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *resp = rpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON_AddStringToObject(result, "protocolVersion", proto ? proto : kProtocolVersion);
    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    cJSON_AddStringToObject(info, "name", "mpx-dog");
    cJSON_AddStringToObject(info, "version", app ? app->version : "0");
    return resp;
}

cJSON *handle_tools_list(const cJSON *id, const cJSON *params)
{
    const char *start = str(params, "cursor");
    if (start && std::strncmp(start, "self.", 5) == 0) start += 5;

    cJSON *resp = rpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON *tools = cJSON_AddArrayToObject(result, "tools");

    size_t count = 0;
    const Tool *table = robot_tools::all(&count);
    size_t bytes = 0;
    bool started = (start == nullptr);
    for (size_t k = 0; k < count; k++) {
        const Tool &t = table[k];
        if (!started) {
            started = std::strcmp(t.name, start) == 0;
            if (!started) continue;
        }
        cJSON *tool = robot_tools::descriptor(t);
        char *one = cJSON_PrintUnformatted(tool);
        const size_t len = one ? std::strlen(one) + 1 : 0;
        cJSON_free(one);
        /* Always at least one tool per page, or a giant schema would stall
         * the server's walk forever on the same cursor. */
        if (bytes > 0 && bytes + len > kListPageBytes) {
            cJSON_Delete(tool);
            cJSON_AddStringToObject(result, "nextCursor", t.name);
            break;
        }
        cJSON_AddItemToArray(tools, tool);
        bytes += len;
    }
    ESP_LOGI(TAG, "tools/list: %d tool(s)%s%s", cJSON_GetArraySize(tools),
             start ? " from " : "", start ? start : "");
    return resp;
}

cJSON *handle_tools_call(const cJSON *id, const cJSON *params)
{
    const char *name = str(params, "name");
    if (!name) return rpc_error(id, -32602, "params.name is required");
    const Tool *tool = robot_tools::find(name);
    if (!tool) return rpc_error(id, -32602, "unknown tool");

    const cJSON *arguments = params ? cJSON_GetObjectItem(params, "arguments") : nullptr;
    ESP_LOGI(TAG, "tools/call %s", tool->name);
    const Reply r = robot_tools::run(*tool, arguments);

    cJSON *resp = rpc_reply(id);
    cJSON *result = cJSON_AddObjectToObject(resp, "result");
    cJSON *content = cJSON_AddArrayToObject(result, "content");
    cJSON *text = cJSON_CreateObject();
    cJSON_AddStringToObject(text, "type", "text");
    cJSON_AddStringToObject(text, "text", r.text.c_str());
    cJSON_AddItemToArray(content, text);
    cJSON_AddBoolToObject(result, "isError", r.error);
    return resp;
}

/* One JSON-RPC message in, the reply (heap, caller frees) or nullptr out --
 * nullptr for notifications, which get no answer. */
char *dispatch(const char *msg)
{
    cJSON *root = cJSON_Parse(msg);
    if (!root) {
        ESP_LOGW(TAG, "unparseable JSON-RPC (%u bytes)", (unsigned)std::strlen(msg));
        return nullptr;
    }
    const cJSON *method = cJSON_GetObjectItem(root, "method");
    const cJSON *id = cJSON_GetObjectItem(root, "id");
    const cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *resp = nullptr;

    if (!cJSON_IsString(method)) {
        /* A response to something we sent? We send nothing, so: ignore. */
        if (!id) resp = rpc_error(nullptr, -32600, "invalid request");
    } else if (std::strncmp(method->valuestring, "notifications/", 14) == 0) {
        resp = nullptr;
    } else if (std::strcmp(method->valuestring, "initialize") == 0) {
        resp = handle_initialize(id, params);
    } else if (std::strcmp(method->valuestring, "ping") == 0) {
        resp = rpc_reply(id);
        cJSON_AddObjectToObject(resp, "result");
    } else if (std::strcmp(method->valuestring, "tools/list") == 0) {
        resp = handle_tools_list(id, params);
    } else if (std::strcmp(method->valuestring, "tools/call") == 0) {
        resp = handle_tools_call(id, params);
    } else {
        ESP_LOGW(TAG, "method not supported: %s", method->valuestring);
        resp = rpc_error(id, -32601, "method not found");
    }
    cJSON_Delete(root);

    char *out = resp ? cJSON_PrintUnformatted(resp) : nullptr;
    cJSON_Delete(resp);
    return out;
}

esp_err_t send_envelope(const char *jsonrpc)
{
    const char *sid = mpx_voice_session_id();
    const size_t need = std::strlen(jsonrpc) + (sid ? std::strlen(sid) : 0) + 48;
    char *buf = (char *)malloc(need);
    if (!buf) return ESP_ERR_NO_MEM;
    std::snprintf(buf, need, "{\"session_id\":\"%s\",\"type\":\"mcp\",\"payload\":%s}",
                  sid ? sid : "", jsonrpc);
    const esp_err_t err = mpx_voice_send_json(buf);
    free(buf);
    return err;
}

/* ── the task ────────────────────────────────────────────────────────── */

QueueHandle_t s_inbox;

void tools_task(void *)
{
    for (;;) {
        char *msg = nullptr;
        if (xQueueReceive(s_inbox, &msg, portMAX_DELAY) != pdTRUE || !msg) continue;
        char *reply = dispatch(msg);
        if (reply) {
            if (send_envelope(reply) != ESP_OK) {
                ESP_LOGW(TAG, "could not send MCP reply -- socket down?");
            }
            cJSON_free(reply);
        }
        free(msg);
    }
}

/* mpx_voice_link's sink: runs on the websocket task, so only queue. The
 * string is ours to free -- by the task, or here if the queue is full. */
void mcp_incoming(const char *jsonrpc_owned)
{
    if (!jsonrpc_owned) return;
    if (!s_inbox || xQueueSend(s_inbox, &jsonrpc_owned, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MCP inbox full -- dropping message");
        free((void *)jsonrpc_owned);
    }
}

}  // namespace

namespace voice_tools {

esp_err_t start()
{
    if (s_inbox) return ESP_OK;
    s_inbox = xQueueCreate(kInboxDepth, sizeof(char *));
    if (!s_inbox) return ESP_ERR_NO_MEM;
    if (xTaskCreate(tools_task, "voice_tools", kTaskStack, nullptr, kTaskPrio, nullptr) != pdPASS) {
        vQueueDelete(s_inbox);
        s_inbox = nullptr;
        return ESP_ERR_NO_MEM;
    }
    mpx_voice_set_mcp_sink(mcp_incoming);
    size_t count = 0;
    (void)robot_tools::all(&count);
    ESP_LOGI(TAG, "%u tools offered to the voice server", (unsigned)count);
    return ESP_OK;
}

std::string list_json()
{
    cJSON *arr = cJSON_CreateArray();
    size_t count = 0;
    const Tool *table = robot_tools::all(&count);
    for (size_t k = 0; k < count; k++) {
        cJSON_AddItemToArray(arr, robot_tools::descriptor(table[k]));
    }
    return print_and_free(arr);
}

std::string call(const char *name, const char *arguments_json, bool *is_error)
{
    const Reply r = robot_tools::call(name, arguments_json);
    if (is_error) *is_error = r.error;
    return r.text;
}

}  // namespace voice_tools
