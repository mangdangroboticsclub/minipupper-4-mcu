/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See agent_lua.h.
 */

#include "agent/agent_lua.h"

#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "lua/lua_vm.h"

namespace agent_lua {

namespace {

const char *TAG = "agent_lua";

constexpr const char *kGroupLua = "cap_mpx_lua";

/* The agent's turn waits for the script; the app's Run allows a minute. */
constexpr int kDefaultTimeoutMs = 30000;
constexpr int kMinTimeoutMs     = 1000;
constexpr int kMaxTimeoutMs     = 60000;
/* What print() can return to the model; the rest is cut. */
constexpr size_t kOutputMax = 2048;

constexpr const char *kLuaExt = ".lua";
constexpr const char *kAppLuaDir = "/lua/";            /* the app's scripts, under /fs */
constexpr const char *kSystemSkillsDir = "/system/skills/";

bool starts_with(const std::string &s, const char *prefix)
{
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

bool ends_with(const std::string &s, const char *suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/* Scripts may run from the agent's data root (its skills), the firmware's
 * skills, or the app's /lua folder; nothing else on the filesystem. */
bool path_allowed(const std::string &path)
{
    const std::string data_root = std::string(CONFIG_APP_AGENT_DATA_DIR) + "/";
    return starts_with(path, data_root.c_str()) ||
           starts_with(path, kSystemSkillsDir) ||
           starts_with(path, (std::string("/fs") + kAppLuaDir).c_str());
}

std::string print_and_free(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(obj);
    return out;
}

const char *error_text(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:     return "script not found or does not compile";
    case ESP_ERR_TIMEOUT:       return "script timed out";
    case ESP_ERR_INVALID_STATE: return "Lua is busy with another script (or not running); try again shortly";
    default:                    return "script failed";
    }
}

robot_tools::Reply lua_run_script(const cJSON *args)
{
    const cJSON *path_item = args ? cJSON_GetObjectItem(args, "path") : nullptr;
    if (!cJSON_IsString(path_item) || !path_item->valuestring[0]) {
        return { "Error: 'path' is required: the absolute path of a .lua file.", true };
    }
    std::string path = path_item->valuestring;
    if (starts_with(path, kAppLuaDir)) path = "/fs" + path;

    if (!ends_with(path, kLuaExt) || path.find("..") != std::string::npos || !path_allowed(path)) {
        return { "Error: 'path' must be a .lua file under " CONFIG_APP_AGENT_DATA_DIR
                 "/ (e.g. a skill's scripts/ folder), /system/skills/ or /fs/lua/.", true };
    }

    const cJSON *script_args = args ? cJSON_GetObjectItem(args, "args") : nullptr;
    if (script_args && !cJSON_IsObject(script_args)) {
        return { "Error: 'args' must be an object.", true };
    }
    char *args_json = script_args ? cJSON_PrintUnformatted(script_args) : nullptr;

    int timeout_ms = kDefaultTimeoutMs;
    const cJSON *t = args ? cJSON_GetObjectItem(args, "timeout_ms") : nullptr;
    if (cJSON_IsNumber(t)) {
        timeout_ms = (int)t->valuedouble;
        if (timeout_ms < kMinTimeoutMs) timeout_ms = kMinTimeoutMs;
        if (timeout_ms > kMaxTimeoutMs) timeout_ms = kMaxTimeoutMs;
    }

    std::string output(kOutputMax, '\0');
    ESP_LOGI(TAG, "run %s (%d ms)", path.c_str(), timeout_ms);
    const esp_err_t err = lua_run_file_args(path.c_str(), args_json, &output[0], output.size(),
                                            (uint32_t)timeout_ms);
    cJSON_free(args_json);
    output.resize(std::strlen(output.c_str()));

    /* A script error is information for the model (fix the script, tell the
     * user), not a failed tool call. */
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", err == ESP_OK);
    cJSON_AddStringToObject(o, "path", path.c_str());
    if (err != ESP_OK) {
        cJSON_AddStringToObject(o, "error", error_text(err));
    }
    cJSON_AddStringToObject(o, "output", output.c_str());
    return { print_and_free(o), false };
}

const robot_tools::Tool kTools[] = {
    { "lua_run_script", kGroupLua,
      "Run a Lua script on the robot's own Lua VM and return what it printed. "
      "The script sees a global `args` table and can call every robot tool "
      "with tools.call(name, args). Read skill mpx_skill_creator before writing one.",
      nullptr,
      "{\"type\":\"object\",\"properties\":{"
      "\"path\":{\"type\":\"string\",\"description\":\"Absolute path of the .lua file, "
      "e.g. a skill's scripts/<name>.lua\"},"
      "\"args\":{\"type\":\"object\",\"description\":\"Becomes the script's `args` table.\"},"
      "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":1000,\"maximum\":60000}},"
      "\"required\":[\"path\"]}",
      lua_run_script },
};

}  // namespace

const robot_tools::Tool *tools(size_t *count)
{
    if (count) *count = sizeof(kTools) / sizeof(kTools[0]);
    return kTools;
}

}  // namespace agent_lua
