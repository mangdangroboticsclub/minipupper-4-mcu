/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * WASM skills, as tools.
 *
 * Note this is a different thing from ESP-Claw's own cap_skill / claw_skill,
 * which manages SKILL.md documents. These are compiled WebAssembly modules
 * that drive the robot through a 74-function ABI. Both are called "skills" by
 * their respective systems and they do not interact; the tool names here are
 * prefixed mpx_ so the model has some chance of keeping them apart.
 */

#include "cap_mpx_skill.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "claw_cap.h"
#include "esp_log.h"

#include "mpx_wasm.h"

static const char *TAG = "cap_mpx_skill";

static esp_err_t execute_list(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    size_t at = 0;
    const int n = mpx_wasm_skill_count();

    if (!output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    at += (size_t)snprintf(output + at, output_size - at,
                           "{\"ok\":true,\"safe_mode\":%s,\"running\":",
                           mpx_wasm_safe_mode() ? "true" : "false");

    {
        const char *running = mpx_wasm_running_skill_name();
        at += (size_t)snprintf(output + at, output_size - at,
                               running ? "\"%s\"" : "null", running ? running : "");
    }

    at += (size_t)snprintf(output + at, output_size - at, ",\"skills\":[");
    for (int i = 0; i < n && at < output_size; i++) {
        const char *file = NULL, *slug = NULL, *gait = NULL;
        int abi = 0;
        bool autorun = false, behaviour = false;
        if (!mpx_wasm_skill_at(i, &file, &slug, &gait, &abi, &autorun, &behaviour)) {
            continue;
        }
        at += (size_t)snprintf(output + at, output_size - at,
                               "%s{\"file\":\"%s\",\"name\":\"%s\",\"abi\":%d,"
                               "\"autorun\":%s,\"behaviour\":%s",
                               i ? "," : "",
                               file ? file : "", slug ? slug : "", abi,
                               autorun ? "true" : "false",
                               behaviour ? "true" : "false");
        if (gait && gait[0] && at < output_size) {
            at += (size_t)snprintf(output + at, output_size - at,
                                   ",\"provides_movement\":\"%s\"", gait);
        }
        if (at < output_size) {
            at += (size_t)snprintf(output + at, output_size - at, "}");
        }
    }
    if (at < output_size) {
        snprintf(output + at, output_size - at, "]}");
    }
    return ESP_OK;
}

static esp_err_t execute_run(const char *input_json,
                             const claw_cap_call_context_t *ctx,
                             char *output, size_t output_size)
{
    (void)ctx;
    cJSON *root = NULL;
    const cJSON *skill = NULL;
    const cJSON *params = NULL;

    if (!input_json || !output || output_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: input must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    skill = cJSON_GetObjectItem(root, "skill");
    if (!cJSON_IsString(skill) || !skill->valuestring[0]) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: 'skill' is required. Call mpx_list_skills for the "
                 "installed file names.");
        return ESP_ERR_INVALID_ARG;
    }

    if (mpx_wasm_skill_running()) {
        const char *running = mpx_wasm_running_skill_name();
        cJSON_Delete(root);
        /* One skill at a time, and a request that arrives during one is
         * refused rather than queued -- a queue means the robot performing a
         * movement someone asked for long enough ago that they have stopped
         * expecting it. */
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"a skill is already running\","
                 "\"running\":\"%s\"}", running ? running : "");
        return ESP_OK;
    }

    params = cJSON_GetObjectItem(root, "params");
    {
        const char *p = cJSON_IsString(params) ? params->valuestring : "";
        const bool ok = mpx_wasm_run_skill(skill->valuestring, p, "api");
        snprintf(output, output_size,
                 ok ? "{\"ok\":true,\"started\":\"%s\"}"
                    : "{\"ok\":false,\"skill\":\"%s\",\"error\":\"could not start; "
                      "check the file exists with mpx_list_skills\"}",
                 skill->valuestring);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t execute_stop(const char *input_json,
                              const claw_cap_call_context_t *ctx,
                              char *output, size_t output_size)
{
    (void)input_json;
    (void)ctx;
    const bool was = mpx_wasm_skill_running();
    mpx_wasm_stop_skill();
    snprintf(output, output_size, "{\"ok\":true,\"was_running\":%s}",
             was ? "true" : "false");
    return ESP_OK;
}

static const claw_cap_descriptor_t s_descriptors[] = {
    {
        .id = "mpx_list_skills",
        .name = "mpx_list_skills",
        .family = "robot",
        .description =
            "List the WebAssembly robot skills installed on this device, which "
            "one is running, and any extra movement names they add for "
            "robot_move. **Do not guess skill names.**",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = execute_list,
    },
    {
        .id = "mpx_run_skill",
        .name = "mpx_run_skill",
        .family = "robot",
        .description =
            "Run an installed robot skill by file name. Only one runs at a time. "
            "Get names from mpx_list_skills first.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json =
            "{\"type\":\"object\",\"properties\":{"
            "\"skill\":{\"type\":\"string\",\"description\":"
            "\"File name, e.g. moonwalk.wasm\"},"
            "\"params\":{\"type\":\"string\",\"description\":"
            "\"Optional name=value;name=value string the skill can read.\"}},"
            "\"required\":[\"skill\"]}",
        .execute = execute_run,
    },
    {
        .id = "mpx_stop_skill",
        .name = "mpx_stop_skill",
        .family = "robot",
        .description =
            "Stop the running robot skill. Cooperative: the skill gets to clean "
            "up, so the robot settles rather than dropping.",
        .kind = CLAW_CAP_KIND_CALLABLE,
        .cap_flags = CLAW_CAP_FLAG_CALLABLE_BY_LLM,
        .input_schema_json = "{\"type\":\"object\",\"properties\":{}}",
        .execute = execute_stop,
    },
};

static const claw_cap_group_t s_group = {
    .group_id = "cap_mpx_skill",
    .plugin_name = "Robot skills",
    .version = "1",
    .descriptors = s_descriptors,
    .descriptor_count = sizeof(s_descriptors) / sizeof(s_descriptors[0]),
};

esp_err_t cap_mpx_skill_register_group(void)
{
    if (claw_cap_group_exists(s_group.group_id)) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "registering %u skill tools", (unsigned)s_group.descriptor_count);
    return claw_cap_register_group(&s_group);
}
