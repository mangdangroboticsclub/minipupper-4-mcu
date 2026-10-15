/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "app_claw.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CONFIG_STR_LEN        320
#define APP_CONFIG_TIMEZONE_LEN   32

#define APP_WIFI_SSID             CONFIG_APP_WIFI_SSID
#define APP_WIFI_PASSWORD         CONFIG_APP_WIFI_PASSWORD
#define APP_SEARCH_HTTP_ALLOWLIST CONFIG_APP_SEARCH_HTTP_ALLOWLIST

typedef struct {
    char wifi_ssid[APP_CONFIG_STR_LEN];
    char wifi_password[APP_CONFIG_STR_LEN];
    char ap_ssid[APP_CONFIG_STR_LEN];
    char ap_password[APP_CONFIG_STR_LEN];
    char ap_behavior[16];
    char llm_api_key[APP_CONFIG_STR_LEN];
    char llm_backend_type[32];
    char llm_model[64];
    char llm_base_url[APP_CONFIG_STR_LEN];
    char llm_auth_type[32];
    char llm_timeout_ms[16];
    char llm_max_tokens[16];
    char llm_default_image_max_bytes[16];
    char llm_max_tokens_field[32];
    char llm_supports_tools[8];
    char llm_supports_vision[8];
    char llm_image_remote_url_only[8];
    char qq_app_id[32];
    char qq_app_secret[APP_CONFIG_STR_LEN];
    char qq_msg_type[8];
    char feishu_app_id[64];
    char feishu_app_secret[APP_CONFIG_STR_LEN];
    char tg_bot_token[APP_CONFIG_STR_LEN];
    char wechat_token[APP_CONFIG_STR_LEN];
    char wechat_base_url[APP_CONFIG_STR_LEN];
    char wechat_cdn_base_url[APP_CONFIG_STR_LEN];
    char wechat_account_id[32];
    char search_brave_key[APP_CONFIG_STR_LEN];
    char search_tavily_key[APP_CONFIG_STR_LEN];
    char search_http_allowlist[APP_CONFIG_STR_LEN];
    char enabled_cap_groups[APP_CONFIG_STR_LEN];
    char llm_visible_cap_groups[APP_CONFIG_STR_LEN];
    char enabled_lua_modules[APP_CONFIG_STR_LEN];
    char time_timezone[APP_CONFIG_TIMEZONE_LEN];
    char ai_mode[12];     /* "mangdang" (default), "own" or "off" -- see below */
} app_config_t;

/* Where the robot's AI comes from. One setting, three answers:
 *
 *   APP_AI_MANGDANG  the default. Chat goes to the MangDang gateway with this
 *                    robot's own token; voice uses the MangDang voice server
 *                    end to end. Nothing to set up.
 *   APP_AI_OWN       the owner's own LLM account (Anthropic, OpenAI, ...),
 *                    entered on the robot. Chat calls it directly; voice still
 *                    uses the MangDang server, but ONLY for speech-to-text and
 *                    text-to-speech -- the answer comes from this robot's agent
 *                    with the owner's key, which never leaves the robot.
 *   APP_AI_OFF       no AI at all: no LLM settings in effect, no voice, the
 *                    microphone is never opened. The robot still walks, the
 *                    web app and the skill store still work. */
typedef enum {
    APP_AI_MANGDANG = 0,
    APP_AI_OWN,
    APP_AI_OFF,
} app_ai_mode_t;

app_ai_mode_t app_config_ai_mode(const app_config_t *config);
const char *app_config_ai_mode_name(app_ai_mode_t mode);
/** Parse "mangdang" / "own" / "off"; false for anything else. */
bool app_config_parse_ai_mode(const char *text, app_ai_mode_t *out);
/** The mode this boot started with (voice and the device brain are decided
 *  at start-up; a new mode takes effect after a restart). */
app_ai_mode_t app_config_running_ai_mode(void);
/** Load what is stored, WITHOUT the mode's overrides -- i.e. the owner's own
 *  LLM settings even while the gateway is in effect. For the AI settings
 *  screen; everything else wants app_config_load(). */
esp_err_t app_config_load_stored(app_config_t *config);

esp_err_t app_config_init(void);
void app_config_load_defaults(app_config_t *config);
esp_err_t app_config_load(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
esp_err_t app_config_validate_wifi(const app_config_t *config, const char **message);
void app_config_to_claw(const app_config_t *config, app_claw_config_t *out);
const char *app_config_get_timezone(const app_config_t *config);

/** This robot's token for the MangDang gateway ("<uuid>.<32 hex>"), derived
 *  from CONFIG_MP4_SKILL_AES_KEY_HEX. False if the key is not configured. */
bool app_config_robot_token(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
