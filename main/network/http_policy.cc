/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/policy routes -- the app's RL Walk page: an RL walk policy running
 * on the ESP32-S3 itself (main/rl/), no Raspberry Pi.
 *
 *   GET  /v1/policy       state, the built-in policy, the last run's numbers
 *   GET  /v1/policy/log   the last logged run, one CSV line per 20 ms step
 *   GET  /v1/policy/bench the size test: how long bigger networks take here
 *   POST /v1/policy       {"action":"run","seconds":10,"vx":0.05,"vy":0,"wz":0,
 *                          "hold":2,"dry":false,"force":false,"log":true,
 *                          "own_calibration":true,"getup_auto":true}
 *                         {"action":"getup","force":false}
 *                         {"action":"cmd","vx":..,"vy":..,"wz":..}   (also a keepalive)
 *                         {"action":"keepalive"} | {"action":"stop"} | {"action":"test"}
 *                         {"action":"bench","bits":16}   (starts the size test; read it on /v1/policy/bench)
 *                         {"action":"weights","bits":16}  (32, 16 or 8: how the robot runs the network)
 *                         {"action":"policy","name":"terrain"}  (switch built-in walk: "walk" or "terrain")
 *
 * A run started here has a dead-man (DEADMAN_MS): the app sends "cmd" every
 * quarter second while it runs; without it the robot ramps to its stand and
 * stops, so a locked phone or lost Wi-Fi cannot leave it walking.
 *
 * Ported from mpv2-uros (components/http_server/http_server_mpx_robot_api.c);
 * same JSON, so the same page works on both. Nothing is registered when the
 * policy is compiled out: the app treats a 404 on GET /v1/policy as "no RL".
 */

#include "network/http_policy.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "http_policy";

#if CONFIG_APP_POLICY_ENABLE

#include "rl/mpx_policy.h"

namespace {

constexpr uint32_t DEADMAN_MS = 2000;

void num_or_null(cJSON *o, const char *key, float v)
{
    if (std::isfinite(v)) {
        cJSON_AddNumberToObject(o, key, v);
    } else {
        cJSON_AddNullToObject(o, key);
    }
}

const char *phase_name(mpx_policy_state_t s)
{
    switch (s) {
    case MPX_POLICY_IDLE:     return "idle";
    case MPX_POLICY_PREP:     return "prep";
    case MPX_POLICY_RAMP_IN:  return "ramp_in";
    case MPX_POLICY_HOLD_IN:  return "hold_in";
    case MPX_POLICY_RUN:      return "run";
    case MPX_POLICY_RAMP_OUT: return "ramp_out";
    case MPX_POLICY_HOLD_OUT: return "hold_out";
    case MPX_POLICY_GETUP:    return "getup";
    }
    return "idle";
}

esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *o)
{
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

esp_err_t status_reply(httpd_req_t *req, const char *status, const char *error)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    mpx_policy_status_t st;
    float m[3] = { 0, 0, 0 };
    mpx_policy_get_status(&st);
    const bool loaded = mpx_policy_cmd_max(m);
    cJSON_AddBoolToObject(o, "available", true);
    cJSON_AddBoolToObject(o, "ok", error == nullptr);
    if (error) cJSON_AddStringToObject(o, "error", error);
    cJSON_AddStringToObject(o, "state", mpx_policy_state_name(st.state));
    cJSON_AddStringToObject(o, "phase", phase_name(st.state));
    cJSON_AddBoolToObject(o, "active", st.state != MPX_POLICY_IDLE);
    cJSON_AddBoolToObject(o, "loaded", loaded);
    cJSON_AddStringToObject(o, "name", st.name);
    cJSON_AddStringToObject(o, "robot", st.robot);
    cJSON_AddStringToObject(o, "this_robot", CONFIG_APP_POLICY_ROBOT);
    cJSON_AddBoolToObject(o, "omni", st.omni);
    cJSON_AddBoolToObject(o, "own_calibration", st.robot_offsets);
    cJSON_AddBoolToObject(o, "dry", st.dry);
    cJSON *cmd = cJSON_AddArrayToObject(o, "cmd");
    cJSON *max = cJSON_AddArrayToObject(o, "cmd_max");
    for (int i = 0; i < 3; ++i) {
        if (cmd) cJSON_AddItemToArray(cmd, cJSON_CreateNumber(st.cmd[i]));
        if (max) cJSON_AddItemToArray(max, cJSON_CreateNumber(m[i]));
    }
    cJSON_AddNumberToObject(o, "steps", st.steps);
    cJSON_AddNumberToObject(o, "late_steps", st.late_steps);
    cJSON_AddNumberToObject(o, "forward_us_max", st.forward_us_max);
    cJSON_AddNumberToObject(o, "tick_us_max", st.tick_us_max);
    cJSON_AddNumberToObject(o, "exchange_us_max", st.exchange_us_max);
    num_or_null(o, "tilt_deg", st.state != MPX_POLICY_IDLE ? st.tilt_deg : NAN);
    num_or_null(o, "temp_max_c", st.state != MPX_POLICY_IDLE ? st.temp_max_c : NAN);
    cJSON_AddStringToObject(o, "last_stop", st.last_stop);
    cJSON_AddBoolToObject(o, "getup_ready", st.getup_ready);
    cJSON_AddStringToObject(o, "getup_name", st.getup_name);
    cJSON_AddBoolToObject(o, "getup_auto", st.auto_getup);
    cJSON_AddNumberToObject(o, "getups", st.getups);
    cJSON_AddNumberToObject(o, "getups_ok", st.getups_ok);
    cJSON_AddStringToObject(o, "builtin", mpx_policy_builtin_current());
    cJSON *bl = cJSON_AddArrayToObject(o, "builtins");
    for (int i = 0; bl && i < mpx_policy_builtin_count(); ++i)
        cJSON_AddItemToArray(bl, cJSON_CreateString(mpx_policy_builtin_id(i)));
    cJSON_AddNumberToObject(o, "weight_bits", st.weight_bits);
    cJSON_AddNumberToObject(o, "weights_bytes", st.weights_bytes);
    cJSON_AddNumberToObject(o, "weights_internal", st.weights_internal);
    cJSON_AddNumberToObject(o, "weights_worst_deg", st.weights_worst_deg);
    cJSON_AddNumberToObject(o, "weights_mean_deg", st.weights_mean_deg);
    cJSON_AddNumberToObject(o, "ram_free", st.ram_free);
    cJSON_AddNumberToObject(o, "ram_min_free", st.ram_min_free);
    cJSON_AddNumberToObject(o, "ram_largest", st.ram_largest);
    size_t dropped = 0;
    cJSON_AddNumberToObject(o, "log_steps", static_cast<double>(mpx_policy_log_count(&dropped)));
    cJSON_AddNumberToObject(o, "log_dropped", static_cast<double>(dropped));
    return send_json(req, status, o);
}

/* The whole body, NUL-terminated, malloc'd; nullptr if empty or too big. */
char *read_body(httpd_req_t *req, int max_len)
{
    const int len = req->content_len;
    if (len <= 0 || len > max_len) {
        return nullptr;
    }
    char *buf = static_cast<char *>(malloc(static_cast<size_t>(len) + 1));
    if (!buf) {
        return nullptr;
    }
    int total = 0;
    while (total < len) {
        const int got = httpd_req_recv(req, buf + total, static_cast<size_t>(len - total));
        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            free(buf);
            return nullptr;
        }
        total += got;
    }
    buf[total] = '\0';
    return buf;
}

float num(const cJSON *o, const char *key, float dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    return cJSON_IsNumber(v) ? static_cast<float>(v->valuedouble) : dflt;
}

esp_err_t policy_get_handler(httpd_req_t *req)
{
    return status_reply(req, "200 OK", nullptr);
}

esp_err_t policy_log_handler(httpd_req_t *req)
{
    if (mpx_policy_active()) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "the run is still on");
    }
    size_t dropped = 0;
    const size_t n = mpx_policy_log_count(&dropped);
    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"rl_walk_log.csv\"");
    if (httpd_resp_sendstr_chunk(req, mpx_policy_log_header()) != ESP_OK) return ESP_FAIL;
    if (httpd_resp_sendstr_chunk(req, "\n") != ESP_OK) return ESP_FAIL;
    /* About 80 bytes a line, sent in 2 KB pieces. */
    static char chunk[2048];
    char line[200];
    size_t used = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!mpx_policy_log_row(i, line, sizeof(line))) break;
        const size_t l = strlen(line);
        if (used + l + 2 > sizeof(chunk)) {
            if (httpd_resp_send_chunk(req, chunk, used) != ESP_OK) return ESP_FAIL;
            used = 0;
        }
        memcpy(chunk + used, line, l);
        used += l;
        chunk[used++] = '\n';
    }
    if (used && httpd_resp_send_chunk(req, chunk, used) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t policy_bench_handler(httpd_req_t *req)
{
    mpx_policy_bench_t b;
    mpx_policy_bench_get(&b);
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    mpx_policy_status_t st;
    mpx_policy_get_status(&st);
    cJSON_AddBoolToObject(o, "running", b.running);
    cJSON_AddBoolToObject(o, "done", b.done);
    cJSON_AddNumberToObject(o, "bits", b.bits);
    cJSON_AddNumberToObject(o, "n_rows", b.n_rows);
    cJSON_AddNumberToObject(o, "n_total", b.n_total);
    cJSON_AddNumberToObject(o, "internal_free", b.internal_free);
    cJSON_AddNumberToObject(o, "internal_reserve", b.internal_reserve);
    cJSON_AddNumberToObject(o, "internal_largest", b.internal_largest);
    cJSON_AddNumberToObject(o, "psram_free", b.psram_free);
    cJSON_AddNumberToObject(o, "psram_largest", b.psram_largest);
    /* The servo exchange shares the tick with the network: the last run's worst (0 before any run). */
    cJSON_AddNumberToObject(o, "exchange_us_max", st.exchange_us_max);
    if (b.error[0]) cJSON_AddStringToObject(o, "error", b.error);
    cJSON *rows = cJSON_AddArrayToObject(o, "rows");
    for (int i = 0; rows && i < b.n_rows && i < MPX_POLICY_BENCH_MAX; ++i) {
        const mpx_policy_bench_row_t &r = b.rows[i];
        cJSON *x = cJSON_CreateObject();
        if (!x) break;
        cJSON_AddNumberToObject(x, "hidden", r.hidden);
        cJSON_AddNumberToObject(x, "params", r.params);
        cJSON_AddNumberToObject(x, "bytes", r.bytes);
        cJSON_AddNumberToObject(x, "bytes_internal", r.bytes_internal);
        cJSON_AddBoolToObject(x, "ok", r.ok);
        if (r.ok) {
            cJSON_AddNumberToObject(x, "us_avg", r.us_avg);
            cJSON_AddNumberToObject(x, "us_max", r.us_max);
            cJSON_AddBoolToObject(x, "flushed", r.flushed);
        } else {
            cJSON_AddNumberToObject(x, "fail_bytes", r.fail_bytes);
        }
        cJSON_AddItemToArray(rows, x);
    }
    return send_json(req, "200 OK", o);
}

esp_err_t policy_post_handler(httpd_req_t *req)
{
    char *raw = read_body(req, 1024);
    cJSON *body = raw ? cJSON_Parse(raw) : nullptr;
    free(raw);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid JSON body");
    }
    char action[16] = "";
    const cJSON *a = cJSON_GetObjectItem(body, "action");
    if (cJSON_IsString(a)) snprintf(action, sizeof(action), "%s", a->valuestring);
    char pname[16] = "";
    const cJSON *pn = cJSON_GetObjectItem(body, "name");
    if (cJSON_IsString(pn)) snprintf(pname, sizeof(pname), "%s", pn->valuestring);
    const float vx = num(body, "vx", 0.0f);
    const float vy = num(body, "vy", 0.0f);
    const float wz = num(body, "wz", 0.0f);
    const float seconds = num(body, "seconds", 10.0f);
    const float hold = num(body, "hold", 2.0f);
    const int bits = static_cast<int>(num(body, "bits", 0.0f));
    const bool dry = cJSON_IsTrue(cJSON_GetObjectItem(body, "dry"));
    const bool force = cJSON_IsTrue(cJSON_GetObjectItem(body, "force"));
    const bool log = cJSON_IsTrue(cJSON_GetObjectItem(body, "log"));
    const cJSON *own = cJSON_GetObjectItem(body, "own_calibration");
    const bool set_own = cJSON_IsBool(own);
    const bool own_cal = cJSON_IsTrue(own);
    const cJSON *gauto = cJSON_GetObjectItem(body, "getup_auto");
    const bool set_gauto = cJSON_IsBool(gauto);
    const bool getup_auto = cJSON_IsTrue(gauto);
    cJSON_Delete(body);

    if (strcmp(action, "run") == 0) {
        char why[96] = "";
        if (set_own && !mpx_policy_active()) mpx_policy_set_own_calibration(own_cal);
        mpx_policy_set_log(log);
        if (set_gauto && !mpx_policy_active()) (void)mpx_policy_set_auto_getup(getup_auto);
        mpx_policy_set_deadman_ms(DEADMAN_MS);
        if (mpx_policy_start(seconds, hold, force, dry, why, sizeof(why)) != ESP_OK) {
            ESP_LOGW(TAG, "run refused: %s", why);
            return status_reply(req, "409 Conflict", why);
        }
        mpx_policy_set_cmd(vx, vy, wz);   /* only once accepted */
        ESP_LOGI(TAG, "run %.1f s, cmd %.3f %.3f %.2f%s", seconds, vx, vy, wz, dry ? " (dry)" : "");
        return status_reply(req, "200 OK", nullptr);
    }
    if (strcmp(action, "getup") == 0) {
        char why[96] = "";
        mpx_policy_set_log(log);
        mpx_policy_set_deadman_ms(DEADMAN_MS);
        if (mpx_policy_getup(force, why, sizeof(why)) != ESP_OK) {
            ESP_LOGW(TAG, "getup refused: %s", why);
            return status_reply(req, "409 Conflict", why);
        }
        ESP_LOGI(TAG, "getup");
        return status_reply(req, "200 OK", nullptr);
    }
    if (strcmp(action, "cmd") == 0) {
        mpx_policy_keepalive();
        if (mpx_policy_active()) mpx_policy_set_cmd(vx, vy, wz);
        return status_reply(req, "200 OK", nullptr);
    }
    if (strcmp(action, "keepalive") == 0) {
        mpx_policy_keepalive();
        return status_reply(req, "200 OK", nullptr);
    }
    if (strcmp(action, "stop") == 0) {
        mpx_policy_stop("stopped from the app");
        return status_reply(req, "200 OK", nullptr);
    }
    if (strcmp(action, "test") == 0) {
        float e = 0, avg = 0, worst = 0;
        if (!mpx_policy_selftest(&e, &avg, &worst)) {
            return status_reply(req, "409 Conflict", "not while a run is on (or no policy loaded)");
        }
        cJSON *o = cJSON_CreateObject();
        if (!o) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        cJSON_AddBoolToObject(o, "ok", e < 1e-4f);
        cJSON_AddNumberToObject(o, "max_err", e);
        cJSON_AddNumberToObject(o, "us_avg", avg);
        cJSON_AddNumberToObject(o, "us_max", worst);
        return send_json(req, "200 OK", o);
    }
    if (strcmp(action, "policy") == 0) {
        char why[96] = "";
        if (mpx_policy_select(pname, why, sizeof(why)) != ESP_OK) {
            return status_reply(req, "409 Conflict", why);
        }
        return status_reply(req, "200 OK", nullptr);
    }
    if (strcmp(action, "weights") == 0) {
        char why[96] = "";
        if (mpx_policy_set_weight_bits(bits, why, sizeof(why)) != ESP_OK) {
            return status_reply(req, "409 Conflict", why);
        }
        return status_reply(req, "200 OK", nullptr);
    }
    if (strcmp(action, "bench") == 0) {
        char why[64] = "";
        if (mpx_policy_bench_start(bits ? bits : 32, why, sizeof(why)) != ESP_OK) {
            return status_reply(req, "409 Conflict", why);
        }
        return status_reply(req, "200 OK", nullptr);
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "action: run | getup | cmd | keepalive | stop | test | bench | weights | policy");
}

}  // namespace

int network::register_policy_routes(httpd_handle_t server,
                                    bool (*reg)(httpd_handle_t, const httpd_uri_t *, const char *))
{
    const struct { const char *uri; httpd_method_t method; const char *name;
                   esp_err_t (*handler)(httpd_req_t *); } table[] = {
        { "/v1/policy",     HTTP_GET,  "GET",  policy_get_handler  },
        { "/v1/policy",     HTTP_POST, "POST", policy_post_handler },
        { "/v1/policy/log", HTTP_GET,  "GET",  policy_log_handler  },
        { "/v1/policy/bench", HTTP_GET, "GET", policy_bench_handler },
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

#else /* !CONFIG_APP_POLICY_ENABLE */

int network::register_policy_routes(httpd_handle_t,
                                    bool (*)(httpd_handle_t, const httpd_uri_t *, const char *))
{
    ESP_LOGI(TAG, "RL policy compiled out; /v1/policy not registered");
    return 0;
}

#endif /* CONFIG_APP_POLICY_ENABLE */
