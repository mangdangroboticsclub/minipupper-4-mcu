/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>

#include "app_config.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool wifi_connected;
    const char *ip;
    bool ap_active;
    const char *ap_ssid;
    const char *ap_ip;
    const char *wifi_mode;
    /* The PWA's setup screen reports three states, not one flag: a robot that
     * has credentials but has not joined yet is "connecting", which is the
     * state the user spends the most time looking at. */
    bool sta_configured;
    const char *sta_ssid;
    /* Why the last station attempt ended, in words, or "" if it has not failed
     * since boot. The radio associates happily with a wrong password and only
     * fails at the 4-way handshake three seconds later, so "connecting" can
     * persist forever with nothing on screen to say the password is wrong.
     * This is what the setup screen needs to stop the user waiting. */
    const char *sta_error;
} http_server_wifi_status_t;

typedef struct {
    bool active;
    bool configured;
    bool completed;
    bool persisted;
    char session_key[64];
    char status[32];
    char message[160];
    char qr_data_url[256];
    char account_id[64];
    char user_id[96];
    char token[256];
    char base_url[160];
} http_server_wechat_login_status_t;

typedef struct {
    esp_err_t (*load_config)(app_config_t *config);
    esp_err_t (*save_config)(const app_config_t *config);
    esp_err_t (*get_wifi_status)(http_server_wifi_status_t *status);
    esp_err_t (*restart_device)(void);
    /* Applied live rather than on the next boot -- see
     * http_server_mpx_wifi_api.c for why the PWA needs that. */
    esp_err_t (*wifi_connect)(const char *ssid, const char *password);
    esp_err_t (*wifi_disconnect)(void);
    esp_err_t (*wifi_forget)(void);
    esp_err_t (*wechat_login_start)(const char *account_id, bool force);
    esp_err_t (*wechat_login_get_status)(http_server_wechat_login_status_t *status);
    esp_err_t (*wechat_login_cancel)(void);
    esp_err_t (*wechat_login_mark_persisted)(void);
} http_server_services_t;

typedef struct {
    const char *storage_base_path;
    http_server_services_t services;
} http_server_config_t;

esp_err_t http_server_init(const http_server_config_t *config);
esp_err_t http_server_start(void);
esp_err_t http_server_stop(void);
esp_err_t http_server_webim_bind_im(void);

#ifdef __cplusplus
}
#endif
