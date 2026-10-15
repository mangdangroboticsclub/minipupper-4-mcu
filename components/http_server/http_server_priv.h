/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "http_server.h"

#define HTTP_SERVER_CTRL_PORT         32769
#define HTTP_SERVER_SCRATCH_SIZE      4096
#define HTTP_SERVER_PATH_MAX          256
#define HTTP_SERVER_UPLOAD_MAX_SIZE   (512 * 1024)

typedef struct {
    httpd_handle_t server;
    char storage_base_path[HTTP_SERVER_PATH_MAX];
    http_server_services_t services;
} http_server_ctx_t;

http_server_ctx_t *http_server_ctx(void);

char *http_server_alloc_scratch_buffer(void);
bool http_server_path_is_safe(const char *path);
void http_server_url_decode_inplace(char *value);
esp_err_t http_server_query_get(httpd_req_t *req, const char *key, char *value, size_t value_size);
esp_err_t http_server_send_embedded_file(httpd_req_t *req,
                                         const uint8_t *start,
                                         const uint8_t *end,
                                         const char *content_type);
void http_server_json_add_string(cJSON *root, const char *key, const char *value);
esp_err_t http_server_send_json_response(httpd_req_t *req, cJSON *root);
esp_err_t http_server_parse_json_body(httpd_req_t *req, cJSON **out_root);
void http_server_json_read_string(cJSON *root, const char *key, char *buffer, size_t buffer_size);
esp_err_t http_server_resolve_storage_path(const char *relative_path, char *full_path, size_t full_path_size);
bool http_server_build_child_relative_path(const char *base_path,
                                           const char *entry_name,
                                           char *out_path,
                                           size_t out_path_size);

esp_err_t http_server_register_assets_routes(httpd_handle_t server);
esp_err_t http_server_register_capabilities_routes(httpd_handle_t server);
esp_err_t http_server_register_lua_modules_routes(httpd_handle_t server);
esp_err_t http_server_register_config_routes(httpd_handle_t server);
esp_err_t http_server_register_status_routes(httpd_handle_t server);
esp_err_t http_server_register_files_routes(httpd_handle_t server);
#if CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
esp_err_t http_server_register_lua_app_routes(httpd_handle_t server);
#endif
esp_err_t http_server_register_wechat_routes(httpd_handle_t server);
esp_err_t http_server_register_webim_routes(httpd_handle_t server);
void http_server_webim_ws_fd_remove(int fd);
esp_err_t http_server_captive_404_handler(httpd_req_t *req, httpd_err_code_t error);

/* ── Shared registration helper ───────────────────────────────────────────
 *
 * Registers a table of handlers and LOGS the URI of anything that fails.
 *
 * That logging is the whole point. httpd silently refuses registrations once
 * max_uri_handlers is full, and the MPX-Dog firmware lost a day to exactly
 * that: the marketplace POST/PATCH/DELETE routes stopped registering when the
 * table filled at 64 entries, and the only symptom was 404s from endpoints
 * whose code was plainly right there. A failed registration must say so. */
esp_err_t http_server_register_uri_table(httpd_handle_t server,
                                         const httpd_uri_t *handlers,
                                         size_t count,
                                         const char *group_name);

#if CONFIG_MP4_ROBOT_ENABLE
#include "cap_im_local.h"

/* The /v1 API the MPX-Dog PWA speaks. */
esp_err_t http_server_register_mpx_robot_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_skills_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_fs_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_studio_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_display_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_wifi_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_ai_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_market_routes(httpd_handle_t server);
esp_err_t http_server_register_mpx_chat_routes(httpd_handle_t server);
/* Serves the PWA and the settings UI from the system partition, and owns
 * "/" in place of http_server_register_assets_routes(). Register it LAST:
 * its catch-all matches every GET. */
esp_err_t http_server_register_mpx_web_routes(httpd_handle_t server);
void http_server_mpx_chat_ws_fd_remove(int fd);
/* Mirror of every outbound "web" message into the PWA chat socket.
 * cap_im_local has one outbound callback slot and webim owns it, so the
 * fan-out happens there rather than by fighting over the slot. */
void http_server_mpx_chat_on_outbound(const cap_im_local_message_t *message);
#if CONFIG_APP_CLAW_CAP_LUA
esp_err_t http_server_register_mpx_lua_routes(httpd_handle_t server);
#endif
#endif
