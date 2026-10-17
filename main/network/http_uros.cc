/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/uros routes -- the RL Walk page's ROS 2 card: the robot as a
 * micro-ROS node on Wi-Fi (main/ros/), talking to a micro-ROS agent on a PC.
 *
 *   GET  /v1/uros   settings + status (state, session, counters)
 *   POST /v1/uros   {"enabled":true,"agent_ip":"192.168.1.50","agent_port":8888,
 *                    "domain_id":0,"imu_hz":100,"joint_hz":50}   (any subset)
 *
 * A POST stores the settings in NVS and the node reconnects with them. This
 * replaces mpv2-uros's `uros` console command: mpx-dog has no console.
 * Nothing is registered when micro-ROS is compiled out.
 */

#include "network/http_uros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "http_uros";

#if CONFIG_APP_UROS_ENABLE

#include "ros/mpx_uros.h"
#if CONFIG_APP_UROS_LIDAR_LD06
#include "ros/uros_lidar.h"
#endif

namespace {

esp_err_t reply(httpd_req_t *req, const char *status, const char *error)
{
    mpx_uros_settings_t c;
    mpx_uros_status_t st;
    mpx_uros_get_settings(&c);
    mpx_uros_get_status(&st);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    cJSON_AddBoolToObject(o, "available", true);
    cJSON_AddBoolToObject(o, "ok", error == nullptr);
    if (error) cJSON_AddStringToObject(o, "error", error);
    cJSON_AddBoolToObject(o, "enabled", c.enabled);
    cJSON_AddStringToObject(o, "agent_ip", c.agent_ip);
    cJSON_AddNumberToObject(o, "agent_port", c.agent_port);
    cJSON_AddNumberToObject(o, "domain_id", c.domain_id);
    cJSON_AddNumberToObject(o, "imu_hz", c.imu_hz);
    cJSON_AddNumberToObject(o, "joint_hz", c.joint_hz);
    cJSON_AddStringToObject(o, "state", mpx_uros_state_name(st.state));
    cJSON_AddBoolToObject(o, "connected", st.state == MPX_UROS_CONNECTED);
    cJSON_AddBoolToObject(o, "time_synced", st.time_synced);
    cJSON_AddNumberToObject(o, "connected_s", st.connected_s);
    cJSON_AddNumberToObject(o, "sessions", st.sessions);
    cJSON_AddNumberToObject(o, "agent_lost", st.agent_lost);
    cJSON_AddNumberToObject(o, "ping_ms", st.last_ping_ms);
    cJSON_AddNumberToObject(o, "imu_sent", st.imu_sent);
    cJSON_AddNumberToObject(o, "imu_fail", st.imu_fail);
    cJSON_AddNumberToObject(o, "joint_sent", st.joint_sent);
    cJSON_AddNumberToObject(o, "joint_fail", st.joint_fail);
    cJSON_AddNumberToObject(o, "pings", st.pings_echoed);
    cJSON_AddNumberToObject(o, "late_ticks", st.tick_overruns);
    cJSON_AddNumberToObject(o, "cmd_vel", st.cmd_vel_rx);
    cJSON_AddNumberToObject(o, "cmd_vel_refused", st.cmd_vel_refused);
    cJSON_AddNumberToObject(o, "odom_sent", st.odom_sent);
    cJSON_AddNumberToObject(o, "odom_fail", st.odom_fail);
    cJSON_AddBoolToObject(o, "in_control", st.in_control);
    cJSON_AddNumberToObject(o, "scan_sent", st.scan_sent);
    cJSON_AddNumberToObject(o, "scan_fail", st.scan_fail);
#if CONFIG_APP_UROS_LIDAR_LD06
    uros_lidar_stats_t ls;
    uros_lidar_get_stats(&ls);
    cJSON *ld = cJSON_AddObjectToObject(o, "lidar");
    if (ld) {
        cJSON_AddNumberToObject(ld, "hz", ls.hz);
        cJSON_AddNumberToObject(ld, "points", ls.points);
        cJSON_AddNumberToObject(ld, "turns", ls.turns);
        cJSON_AddNumberToObject(ld, "packets", ls.packets);
        cJSON_AddNumberToObject(ld, "crc_errors", ls.crc_errors);
    }
#endif
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

esp_err_t uros_get_handler(httpd_req_t *req)
{
    return reply(req, "200 OK", nullptr);
}

esp_err_t uros_post_handler(httpd_req_t *req)
{
    const int len = req->content_len;
    if (len <= 0 || len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
    }
    char buf[513];
    int total = 0;
    while (total < len) {
        const int got = httpd_req_recv(req, buf + total, static_cast<size_t>(len - total));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
        total += got;
    }
    buf[total] = '\0';
    cJSON *body = cJSON_Parse(buf);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
    }
    mpx_uros_settings_t c;
    mpx_uros_get_settings(&c);
    const cJSON *v;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(body, "enabled"))) c.enabled = cJSON_IsTrue(v);
    if (cJSON_IsString(v = cJSON_GetObjectItem(body, "agent_ip"))) {
        snprintf(c.agent_ip, sizeof(c.agent_ip), "%s", v->valuestring);
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "agent_port"))) {
        c.agent_port = (v->valuedouble >= 1 && v->valuedouble <= 65535) ? static_cast<uint16_t>(v->valuedouble) : 0;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "domain_id"))) {
        c.domain_id = (v->valuedouble >= 0 && v->valuedouble <= 255) ? static_cast<uint8_t>(v->valuedouble) : 255;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "imu_hz"))) {
        c.imu_hz = (v->valuedouble >= 0 && v->valuedouble <= 65535) ? static_cast<uint16_t>(v->valuedouble) : 65535;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "joint_hz"))) {
        c.joint_hz = (v->valuedouble >= 0 && v->valuedouble <= 65535) ? static_cast<uint16_t>(v->valuedouble) : 65535;
    }
    cJSON_Delete(body);
    char why[96] = "";
    const esp_err_t err = mpx_uros_configure(&c, why, sizeof(why));
    if (err == ESP_ERR_INVALID_ARG) {
        return reply(req, "400 Bad Request", why);
    }
    return reply(req, "200 OK", err == ESP_OK ? nullptr : why);
}

}  // namespace

int network::register_uros_routes(httpd_handle_t server,
                                  bool (*reg)(httpd_handle_t, const httpd_uri_t *, const char *))
{
    const struct { const char *uri; httpd_method_t method; const char *name;
                   esp_err_t (*handler)(httpd_req_t *); } table[] = {
        { "/v1/uros", HTTP_GET,  "GET",  uros_get_handler  },
        { "/v1/uros", HTTP_POST, "POST", uros_post_handler },
    };
    int ok = 0;
    for (const auto &t : table) {
        httpd_uri_t h = {};
        h.uri = t.uri;
        h.method = t.method;
        h.handler = t.handler;
        if (reg(server, &h, t.name)) {
            ESP_LOGI(TAG, "  API: %s %s", t.name, t.uri);
            ok++;
        }
    }
    return ok;
}

#else /* !CONFIG_APP_UROS_ENABLE */

int network::register_uros_routes(httpd_handle_t,
                                  bool (*)(httpd_handle_t, const httpd_uri_t *, const char *))
{
    ESP_LOGI(TAG, "micro-ROS compiled out; /v1/uros not registered");
    return 0;
}

#endif /* CONFIG_APP_UROS_ENABLE */
