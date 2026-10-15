/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http_server_priv.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static const char *TAG = "http_server";

static http_server_ctx_t s_ctx;

http_server_ctx_t *http_server_ctx(void)
{
    return &s_ctx;
}

esp_err_t http_server_captive_404_handler(httpd_req_t *req, httpd_err_code_t error)
{
    http_server_wifi_status_t status = {0};
    esp_err_t err = s_ctx.services.get_wifi_status ? s_ctx.services.get_wifi_status(&status) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK || !status.ap_active) {
        return httpd_resp_send_err(req, error, NULL);
    }

    const char *ap_ip = (status.ap_ip && status.ap_ip[0]) ? status.ap_ip : "192.168.4.1";
    char location[40];
    snprintf(location, sizeof(location), "http://%s/", ap_ip);

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t http_server_init(const http_server_config_t *config)
{
    if (!config || !config->storage_base_path || config->storage_base_path[0] != '/') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!config->services.load_config || !config->services.save_config || !config->services.get_wifi_status) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    strlcpy(s_ctx.storage_base_path, config->storage_base_path, sizeof(s_ctx.storage_base_path));
    s_ctx.services = config->services;
    return ESP_OK;
}

esp_err_t http_server_register_uri_table(httpd_handle_t server,
                                         const httpd_uri_t *handlers,
                                         size_t count,
                                         const char *group_name)
{
    esp_err_t first_error = ESP_OK;

    for (size_t i = 0; i < count; ++i) {
        const esp_err_t err = httpd_register_uri_handler(server, &handlers[i]);
        if (err != ESP_OK) {
            /* Keep going rather than bailing: one full table should not hide
             * how many other routes are also missing. */
            ESP_LOGE(TAG, "UNREGISTERED: %s (%s) - this endpoint will 404",
                     handlers[i].uri, group_name ? group_name : "?");
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }
    return first_error;
}

static void http_server_close_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    http_server_webim_ws_fd_remove(sockfd);
#if CONFIG_MP4_ROBOT_ENABLE
    http_server_mpx_chat_ws_fd_remove(sockfd);
#endif
    close(sockfd);
}

esp_err_t http_server_start(void)
{
    if (s_ctx.server) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = HTTP_SERVER_CTRL_PORT;
    /* ESP-Claw registers 21; the MPX-Dog /v1 API adds roughly 30 more.
     * httpd refuses registrations past this limit *silently*, so the
     * headroom is deliberate and http_server_register_uri_table() shouts if
     * one is ever refused anyway. */
    config.max_uri_handlers = 104;
    config.stack_size = 8192;
    config.max_open_sockets = 12;
    config.lru_purge_enable = true;

    /* The httpd task stack MUST stay in internal RAM. Do not move it to PSRAM
     * to save a few KB -- that was tried, and this is what happens.
     *
     * Every request that serves a file reaches the FAT partition on flash.
     * esp_flash_read() disables the instruction cache while the SPI flash is
     * busy, and with the cache off, PSRAM is unreachable -- including a task
     * stack that lives there. IDF catches it with an assert rather than
     * letting it fault:
     *
     *   assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
     *   cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())
     *
     * The crash lands in stat() under web_send_file(), which reads like a
     * filesystem bug and is not one.
     *
     * This is inherited code, and it was safe upstream: ESP-Claw serves its
     * web assets out of ramfs, so its httpd task never touches flash. Moving
     * both bundles to the system partition -- which is what let the app image
     * shrink by ~70 KB while gaining a whole UI -- is what made a PSRAM stack
     * unsafe here.
     *
     * CONFIG_SPIRAM_XIP_FROM_PSRAM hides it, because with code running from
     * PSRAM the cache is never disabled for a flash read. That is why the MP4
     * board would not have shown this and a plain devkit does. It is not a
     * fix; it is one config option away from returning. */
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

    config.close_fn = http_server_close_fn;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_ctx.server, &config), TAG, "Failed to start HTTP server");
#if !CONFIG_MP4_ROBOT_ENABLE
    ESP_RETURN_ON_ERROR(http_server_register_assets_routes(s_ctx.server), TAG, "Failed to register assets routes");
#endif
    ESP_RETURN_ON_ERROR(http_server_register_capabilities_routes(s_ctx.server), TAG, "Failed to register capability routes");
    ESP_RETURN_ON_ERROR(http_server_register_lua_modules_routes(s_ctx.server), TAG, "Failed to register Lua module routes");
    ESP_RETURN_ON_ERROR(http_server_register_config_routes(s_ctx.server), TAG, "Failed to register config routes");
    ESP_RETURN_ON_ERROR(http_server_register_status_routes(s_ctx.server), TAG, "Failed to register status routes");
    ESP_RETURN_ON_ERROR(http_server_register_files_routes(s_ctx.server), TAG, "Failed to register files routes");
#if CONFIG_APP_CLAW_LUA_MODULE_HTTP_SERVER
    ESP_RETURN_ON_ERROR(http_server_register_lua_app_routes(s_ctx.server), TAG, "Failed to register Lua app routes");
#endif
    ESP_RETURN_ON_ERROR(http_server_register_wechat_routes(s_ctx.server), TAG, "Failed to register WeChat routes");
    ESP_RETURN_ON_ERROR(http_server_register_webim_routes(s_ctx.server), TAG, "Failed to register Web IM routes");
#if CONFIG_MP4_ROBOT_ENABLE
    /* Registered after ESP-Claw's own routes so that if a path ever collides,
     * the framework keeps it and the robot API is the one that complains. */
    ESP_RETURN_ON_ERROR(http_server_register_mpx_robot_routes(s_ctx.server), TAG, "Failed to register robot routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_skills_routes(s_ctx.server), TAG, "Failed to register robot skill routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_fs_routes(s_ctx.server), TAG, "Failed to register robot fs routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_studio_routes(s_ctx.server), TAG, "Failed to register servo studio routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_display_routes(s_ctx.server), TAG, "Failed to register display routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_wifi_routes(s_ctx.server), TAG, "Failed to register wifi routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_ai_routes(s_ctx.server), TAG, "Failed to register AI routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_market_routes(s_ctx.server), TAG, "Failed to register marketplace routes");
    ESP_RETURN_ON_ERROR(http_server_register_mpx_chat_routes(s_ctx.server), TAG, "Failed to register chat routes");
#if CONFIG_APP_CLAW_CAP_LUA
    ESP_RETURN_ON_ERROR(http_server_register_mpx_lua_routes(s_ctx.server), TAG, "Failed to register lua routes");
#endif
    /* ── ADD NOTHING BELOW THIS LINE ──────────────────────────────────────
     *
     * http_server_register_mpx_web_routes() registers a catch-all whose URI is
     * a slash followed by an asterisk -- not written literally anywhere in a
     * block comment in this file, because that pair ends the comment and
     * -Werror=comment rejects it. This server is configured with
     * httpd_uri_match_wildcard (see config.uri_match_fn above), and that
     * match function is used for TWO things: dispatching a request, and the
     * duplicate check inside httpd_register_uri_handler().
     *
     * So once the catch-all is registered, registering ANY later GET route
     * asks httpd "does an existing entry match this URI?", the catch-all
     * answers yes, and the registration is refused with
     * ESP_ERR_HTTPD_HANDLER_EXISTS -- reported as `handler /v1/... with
     * method 1 already registered`, which reads like a genuine duplicate and
     * is not one. http_server_start() then returns an error, and app_main
     * aborts if the caller checks it.
     *
     * POST routes survive, because the catch-all is GET-only. That is why the
     * symptom was two dead endpoints (/v1/lua/list and /v1/lua/read) out of
     * six rather than all of them, which made it look like a collision with
     * something in ESP-Claw. Nothing collided; the order was wrong.
     *
     * Registration order still decides dispatch, so the catch-all also has to
     * stay last for the routes above to be reachable at all. Both constraints
     * point the same way: everything else first, this last, nothing after. */
    ESP_RETURN_ON_ERROR(http_server_register_mpx_web_routes(s_ctx.server), TAG, "Failed to register web routes");
#endif
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(s_ctx.server, HTTPD_404_NOT_FOUND, http_server_captive_404_handler),
                        TAG, "Failed to register captive 404 handler");

    return ESP_OK;
}

esp_err_t http_server_stop(void)
{
    if (!s_ctx.server) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_ctx.server);
    if (err == ESP_OK) {
        s_ctx.server = NULL;
    }
    return err;
}
