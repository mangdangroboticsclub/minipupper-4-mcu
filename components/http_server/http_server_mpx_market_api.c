/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/marketplace proxy and /v1/gateway/config.
 *
 * The PWA's Store and Skills screens talk to a skill marketplace that lives
 * off-device. They cannot talk to it directly: the phone is usually joined to
 * the robot's own AP with no route to the internet, and the gateway answers to
 * the robot's UUID rather than to the browser. So the robot forwards.
 *
 *   /v1/marketplace/robot/{rest}  ->  /v1/robots/{uuid}/{rest}
 *   /v1/marketplace/{rest}        ->  /v1/{rest}
 *
 * The second rule is a deliberate passthrough rather than a list of known
 * endpoints: without it, every new gateway endpoint needed a firmware change
 * and a reflash before the app could reach it. It is not an open relay -- the
 * path is always rooted at /v1/ and the host is the one compiled in, so the
 * browser can only reach the gateway the robot already talks to.
 *
 * Two limits here are not tuning knobs, they are the fix for a specific
 * failure. Each proxied request costs an inbound socket and an outbound one,
 * and LWIP has sixteen. The PWA opens the Store screen and fires several
 * requests at once, on top of the chat WebSocket and the browser's keep-alive
 * sockets, and the pool runs dry -- which surfaces as the whole UI dying, not
 * as a slow marketplace. So GETs are served from a short-lived cache, and at
 * most three requests are in flight upstream at a time.
 *
 * Where the old firmware hand-rolled this over raw LWIP sockets, this uses
 * esp_http_client, which is already in the image for cap_http_request.
 */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_http_client.h"
#include "esp_log.h"

#include "http_server_priv.h"

static const char *TAG = "http_mpx_mkt";

#define MPX_GATEWAY_HOST        CONFIG_MP4_GATEWAY_HOST
#define MPX_GATEWAY_PORT        CONFIG_MP4_GATEWAY_PORT
#define MPX_ROBOT_UUID          CONFIG_MP4_ROBOT_UUID

#define MPX_PROXY_REQ_MAX       4096            /* largest body we forward     */
#define MPX_PROXY_RESP_MAX      (16 * 1024)     /* largest body we read back   */
#define MPX_PROXY_TIMEOUT_MS    8000
#define MPX_PROXY_MAX_INFLIGHT  3
#define MPX_PROXY_URL_MAX       320

#define MPX_CACHE_SLOTS         4
#define MPX_CACHE_TTL_MS        15000
#define MPX_CACHE_BODY_MAX      (8 * 1024)      /* bigger answers are not kept */

static bool gateway_configured(void)
{
    return MPX_GATEWAY_HOST[0] != '\0' && strcmp(MPX_GATEWAY_HOST, "echo") != 0;
}

/* ── The GET cache ─────────────────────────────────────────────────────────
 *
 * Four slots, keyed on the upstream path (query string included -- a filtered
 * storefront listing is a different answer from an unfiltered one). Any write
 * drops every entry, because a purchase changes what the listings say. */

typedef struct {
    char       *path;
    char       *body;
    size_t      len;
    TickType_t  expires;
} mpx_cache_entry_t;

static mpx_cache_entry_t s_cache[MPX_CACHE_SLOTS];
static SemaphoreHandle_t s_cache_lock;

static SemaphoreHandle_t cache_lock(void)
{
    /* Created on the httpd task before any handler can run concurrently with
     * another, so no double-create race exists in practice. */
    if (!s_cache_lock) {
        s_cache_lock = xSemaphoreCreateMutex();
    }
    return s_cache_lock;
}

static void cache_clear_entry(mpx_cache_entry_t *e)
{
    free(e->path);
    free(e->body);
    e->path = NULL;
    e->body = NULL;
    e->len = 0;
    e->expires = 0;
}

/* Returns a malloc'd copy of the cached body, or NULL. The copy exists so the
 * lock is not held across the send. */
static char *cache_get(const char *path, size_t *out_len)
{
    SemaphoreHandle_t lock = cache_lock();
    TickType_t now = xTaskGetTickCount();
    char *copy = NULL;

    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return NULL;
    }
    for (int i = 0; i < MPX_CACHE_SLOTS; i++) {
        if (s_cache[i].path && strcmp(s_cache[i].path, path) == 0) {
            if ((int32_t)(s_cache[i].expires - now) > 0) {
                copy = malloc(s_cache[i].len + 1);
                if (copy) {
                    memcpy(copy, s_cache[i].body, s_cache[i].len);
                    copy[s_cache[i].len] = '\0';
                    *out_len = s_cache[i].len;
                }
            } else {
                cache_clear_entry(&s_cache[i]);
            }
            break;
        }
    }
    xSemaphoreGive(lock);
    return copy;
}

static void cache_put(const char *path, const char *body, size_t len)
{
    SemaphoreHandle_t lock = cache_lock();
    TickType_t now = xTaskGetTickCount();
    int slot = -1;

    if (len == 0 || len > MPX_CACHE_BODY_MAX) {
        return;
    }
    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < MPX_CACHE_SLOTS; i++) {
        if (!s_cache[i].path ||
                strcmp(s_cache[i].path, path) == 0 ||
                (int32_t)(s_cache[i].expires - now) <= 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = 0;
    }
    cache_clear_entry(&s_cache[slot]);

    s_cache[slot].path = strdup(path);
    s_cache[slot].body = malloc(len);
    if (!s_cache[slot].path || !s_cache[slot].body) {
        cache_clear_entry(&s_cache[slot]);
    } else {
        memcpy(s_cache[slot].body, body, len);
        s_cache[slot].len = len;
        s_cache[slot].expires = now + pdMS_TO_TICKS(MPX_CACHE_TTL_MS);
    }
    xSemaphoreGive(lock);
}

static void cache_invalidate_all(void)
{
    SemaphoreHandle_t lock = cache_lock();

    if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    for (int i = 0; i < MPX_CACHE_SLOTS; i++) {
        cache_clear_entry(&s_cache[i]);
    }
    xSemaphoreGive(lock);
}

/* ── The in-flight cap ─────────────────────────────────────────────────── */

static SemaphoreHandle_t s_inflight;

static bool inflight_take(void)
{
    if (!s_inflight) {
        s_inflight = xSemaphoreCreateCounting(MPX_PROXY_MAX_INFLIGHT, MPX_PROXY_MAX_INFLIGHT);
        if (!s_inflight) {
            return false;
        }
    }
    return xSemaphoreTake(s_inflight, pdMS_TO_TICKS(MPX_PROXY_TIMEOUT_MS)) == pdTRUE;
}

static void inflight_give(void)
{
    if (s_inflight) {
        xSemaphoreGive(s_inflight);
    }
}

/* ── The upstream call ─────────────────────────────────────────────────────
 *
 * Returns a malloc'd body (always NUL-terminated) and the upstream status, or
 * NULL with *out_status == 0 when the gateway could not be reached at all.
 * That distinction is the whole reason the status travels back: collapsing
 * everything into 502 made "you do not own this skill" and "no such skill"
 * both read to the user as a broken robot. */

static char *gateway_request(esp_http_client_method_t method,
                             const char *path,
                             const char *body,
                             size_t body_len,
                             int *out_status,
                             size_t *out_len)
{
    char url[MPX_PROXY_URL_MAX];
    esp_http_client_handle_t client;
    esp_http_client_config_t cfg = {0};
    char *buf = NULL;
    int written;
    int64_t content_len;
    int read_len;

    *out_status = 0;
    *out_len = 0;

    written = snprintf(url, sizeof(url), "http://%s:%d%s",
                       MPX_GATEWAY_HOST, MPX_GATEWAY_PORT, path);
    if (written < 0 || (size_t)written >= sizeof(url)) {
        ESP_LOGW(TAG, "gateway URL too long for %s", path);
        return NULL;
    }

    cfg.url = url;
    cfg.method = method;
    cfg.timeout_ms = MPX_PROXY_TIMEOUT_MS;
    cfg.disable_auto_redirect = true;

    client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (esp_http_client_open(client, (int)body_len) != ESP_OK) {
        ESP_LOGW(TAG, "gateway unreachable: %s", url);
        esp_http_client_cleanup(client);
        return NULL;
    }
    if (body_len > 0 &&
            esp_http_client_write(client, body, body_len) != (int)body_len) {
        ESP_LOGW(TAG, "short write to gateway for %s", path);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    content_len = esp_http_client_fetch_headers(client);
    *out_status = esp_http_client_get_status_code(client);

    /* A chunked reply reports -1 rather than a length, so the buffer is sized
     * by the cap in that case and the read loop is what actually bounds it. */
    size_t cap = (content_len > 0 && content_len < MPX_PROXY_RESP_MAX)
                 ? (size_t)content_len : MPX_PROXY_RESP_MAX;
    buf = malloc(cap + 1);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        *out_status = 0;
        return NULL;
    }

    read_len = esp_http_client_read_response(client, buf, (int)cap);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len < 0) {
        free(buf);
        *out_status = 0;
        return NULL;
    }
    buf[read_len] = '\0';
    *out_len = (size_t)read_len;
    return buf;
}

/* ── GET /v1/gateway/config ────────────────────────────────────────────────
 *
 * What the PWA needs to describe this robot's marketplace identity. The UUID
 * is also the AAD for encrypted-skill key unwrap, which is why it is a
 * compile-time constant rather than something settable from here. */

static esp_err_t gateway_config_handler(httpd_req_t *req)
{
    cJSON *resp = cJSON_CreateObject();

    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    http_server_json_add_string(resp, "host", MPX_GATEWAY_HOST);
    cJSON_AddNumberToObject(resp, "port", MPX_GATEWAY_PORT);
    http_server_json_add_string(resp, "robot_uuid", MPX_ROBOT_UUID);
    cJSON_AddBoolToObject(resp, "configured", gateway_configured());
    return http_server_send_json_response(req, resp);
}

/* ── The proxy ─────────────────────────────────────────────────────────── */

static esp_err_t proxy_send_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }
    http_server_json_add_string(resp, "error", message);
    return http_server_send_json_response(req, resp);
}

/* /v1/marketplace/robot/x -> /v1/robots/{uuid}/x, everything else -> /v1/x */
static bool proxy_target_path(const char *uri, char *out, size_t out_size)
{
    static const char robot_prefix[] = "/v1/marketplace/robot/";
    static const char mkt_prefix[]   = "/v1/marketplace/";
    int written;

    if (strncmp(uri, robot_prefix, sizeof(robot_prefix) - 1) == 0) {
        written = snprintf(out, out_size, "/v1/robots/%s/%s",
                           MPX_ROBOT_UUID, uri + sizeof(robot_prefix) - 1);
    } else if (strncmp(uri, mkt_prefix, sizeof(mkt_prefix) - 1) == 0) {
        written = snprintf(out, out_size, "/v1/%s", uri + sizeof(mkt_prefix) - 1);
    } else {
        return false;
    }
    return written > 0 && (size_t)written < out_size;
}

static esp_err_t marketplace_proxy_handler(httpd_req_t *req)
{
    char path[MPX_PROXY_URL_MAX];
    char *body = NULL;
    char *resp_body = NULL;
    size_t resp_len = 0;
    int status = 0;
    esp_http_client_method_t method;
    const char *method_name;
    bool is_get = (req->method == HTTP_GET);
    esp_err_t result;

    if (!gateway_configured()) {
        return proxy_send_error(req, "503 Service Unavailable",
                                "no marketplace gateway is configured on this robot");
    }
    if (!proxy_target_path(req->uri, path, sizeof(path))) {
        return proxy_send_error(req, "404 Not Found", "unknown marketplace path");
    }

    switch (req->method) {
    case HTTP_POST:   method = HTTP_METHOD_POST;   method_name = "POST";   break;
    case HTTP_PATCH:  method = HTTP_METHOD_PATCH;  method_name = "PATCH";  break;
    case HTTP_DELETE: method = HTTP_METHOD_DELETE; method_name = "DELETE"; break;
    default:          method = HTTP_METHOD_GET;    method_name = "GET";    break;
    }

    /* An over-size body used to be dropped and the request forwarded empty, so
     * the gateway rejected something the robot had quietly mangled. Refuse it
     * here instead, where the reason can be said out loud. */
    if (req->content_len >= MPX_PROXY_REQ_MAX) {
        return proxy_send_error(req, "413 Payload Too Large",
                                "request body too large for the proxy");
    }
    if (req->content_len > 0) {
        int total = 0;

        body = malloc(req->content_len + 1);
        if (!body) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        }
        while (total < (int)req->content_len) {
            int got = httpd_req_recv(req, body + total, req->content_len - total);
            if (got <= 0) {
                free(body);
                return proxy_send_error(req, "400 Bad Request", "could not read request body");
            }
            total += got;
        }
        body[total] = '\0';
    }

    if (is_get) {
        resp_body = cache_get(path, &resp_len);
        if (resp_body) {
            httpd_resp_set_type(req, "application/json");
            result = httpd_resp_send(req, resp_body, resp_len);
            free(resp_body);
            free(body);
            return result;
        }
    } else {
        cache_invalidate_all();
    }

    if (!inflight_take()) {
        ESP_LOGW(TAG, "shedding %s %s -- too many gateway requests in flight",
                 method_name, path);
        free(body);
        return proxy_send_error(req, "503 Service Unavailable",
                                "robot is busy talking to the marketplace, try again");
    }
    resp_body = gateway_request(method, path, body, body ? strlen(body) : 0,
                                &status, &resp_len);
    inflight_give();
    free(body);

    ESP_LOGI(TAG, "%s %s -> %s (status=%d, %u bytes)",
             method_name, req->uri, path, status, (unsigned)resp_len);

    if (status == 0) {
        free(resp_body);
        return proxy_send_error(req, "502 Bad Gateway", "gateway unreachable");
    }

    if (is_get && status >= 200 && status < 300 && resp_body) {
        cache_put(path, resp_body, resp_len);
    }

    if (status < 200 || status >= 300) {
        /* Pass the gateway's own status through: 403 and 404 are answers, not
         * failures, and the UI has something useful to say about each. */
        char line[32];
        snprintf(line, sizeof(line), "%d Upstream", status);
        httpd_resp_set_status(req, line);
    }
    httpd_resp_set_type(req, "application/json");
    result = httpd_resp_send(req, resp_body ? resp_body : "{}", resp_body ? resp_len : 2);
    free(resp_body);
    return result;
}

/* ── Registration ──────────────────────────────────────────────────────────
 *
 * Four entries for one URI because esp_http_server matches one method per
 * entry. Losing the POST and DELETE while keeping the GET is the worst
 * possible partial failure -- the marketplace lists everything and nothing you
 * press does anything -- which is exactly what happened once when the handler
 * table filled up. http_server_register_uri_table names anything that fails. */

esp_err_t http_server_register_mpx_market_routes(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] = {
        { .uri = "/v1/gateway/config",  .method = HTTP_GET,    .handler = gateway_config_handler     },
        { .uri = "/v1/marketplace/*",   .method = HTTP_GET,    .handler = marketplace_proxy_handler  },
        { .uri = "/v1/marketplace/*",   .method = HTTP_POST,   .handler = marketplace_proxy_handler  },
        { .uri = "/v1/marketplace/*",   .method = HTTP_PATCH,  .handler = marketplace_proxy_handler  },
        { .uri = "/v1/marketplace/*",   .method = HTTP_DELETE, .handler = marketplace_proxy_handler  },
    };

    return http_server_register_uri_table(server, handlers,
                                          sizeof(handlers) / sizeof(handlers[0]),
                                          "mpx marketplace");
}
