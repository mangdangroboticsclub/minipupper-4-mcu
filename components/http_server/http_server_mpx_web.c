/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Static web serving for the MPX-Dog PWA, out of the read-only system
 * partition rather than out of the firmware image.
 *
 * ESP-Claw embeds its SolidJS bundle with EMBED_FILES, which turns it into
 * .rodata inside the app partition. That is the right default for one small
 * UI. It stops being right once there are two: the PWA's bundle is 126 KB
 * gzipped and ESP-Claw's is 73 KB, and 199 KB of the 5 MB app slot spent on
 * files that never execute is 199 KB not available to the agent, the skills or
 * an OTA image of the same size sitting in the other slot.
 *
 * So both bundles are staged into the system partition at build time (see the
 * top-level CMakeLists.txt) and streamed from there. The app image gets
 * *smaller* than it was before the PWA existed, and re-flashing a UI change no
 * longer means re-flashing firmware.
 *
 * What it costs: a 4 KB read buffer per in-flight request instead of sending
 * straight from memory-mapped flash. http_server_alloc_scratch_buffer() takes
 * that from PSRAM by preference, so it does not touch the internal DIRAM that
 * is actually scarce on this board.
 *
 * ── Routing ───────────────────────────────────────────────────────────────
 *
 *   /favicon.ico     embedded (one file, always present, answers instantly)
 *   /settings[/...]  ESP-Claw's SolidJS UI, from <SYSTEM>/settings/
 *   everything else  the PWA, from <SYSTEM>/www/
 *
 * The catch-all is a single wildcard handler registered after every /v1 route,
 * because esp_http_server returns the first matching entry in registration
 * order. One handler covers the whole PWA -- its assets and its client-side
 * routes both -- which matters when the handler table is a fixed-size array
 * that silently refuses registrations once full.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"

#include "claw_paths.h"
#include "esp_log.h"

#include "http_server_priv.h"

static const char *TAG = "http_mpx_web";

#define MPX_WEB_PWA_DIR         "www"
#define MPX_WEB_SETTINGS_DIR    "settings"
#define MPX_WEB_INDEX           "index.html"
#define MPX_WEB_ETAG_MAX        48

extern const uint8_t favicon_ico_start[] asm("_binary_favicon_ico_start");
extern const uint8_t favicon_ico_end[]   asm("_binary_favicon_ico_end");

/* Shown only when the system partition has no UI in it -- a robot flashed with
 * firmware but not with its data partitions. Without this the browser gets a
 * bare 404 and the cause is invisible; the fix is one command, so say it. */
static const char s_no_assets_page[] =
    "<!doctype html><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>MPX-Dog</title>"
    "<style>body{font:15px/1.6 system-ui,sans-serif;margin:0;padding:2rem;"
    "background:#111;color:#eee}code{background:#222;padding:.15em .4em;"
    "border-radius:4px}</style>"
    "<h1>Web UI not installed</h1>"
    "<p>The firmware is running, but the system partition holds no web assets. "
    "The robot's API is up and answering on <code>/v1/...</code>.</p>"
    "<p>Flash the data partitions:<br><code>idf.py flash</code></p>";

/* ── Path plumbing ─────────────────────────────────────────────────────── */

/* Copy the path part of a URI (query string dropped) into out. Rejects
 * anything that is not an absolute path or that contains "..", because the
 * result is handed straight to fopen() under the system root. */
static bool web_request_path(const httpd_req_t *req, char *out, size_t out_size)
{
    const char *uri = req->uri;
    const char *query = strchr(uri, '?');
    size_t len = query ? (size_t)(query - uri) : strlen(uri);

    if (len == 0 || len >= out_size) {
        return false;
    }
    memcpy(out, uri, len);
    out[len] = '\0';
    http_server_url_decode_inplace(out);
    return http_server_path_is_safe(out);
}

static esp_err_t web_system_path(const char *dir, const char *rel,
                                 const char *suffix, char *out, size_t out_size)
{
    char joined[HTTP_SERVER_PATH_MAX];
    int written = snprintf(joined, sizeof(joined), "%s%s%s", dir, rel, suffix ? suffix : "");

    if (written < 0 || (size_t)written >= sizeof(joined)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return claw_paths_join(CLAW_PATH_SYSTEM, joined, out, out_size);
}

static const char *web_content_type(const char *path)
{
    const char *dot = strrchr(path, '.');

    if (!dot) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(dot, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(dot, ".json") == 0) {
        /* Both manifest.json and any data file: the manifest spec wants
         * application/manifest+json but every browser accepts this. */
        return "application/json; charset=utf-8";
    }
    if (strcmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(dot, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcmp(dot, ".woff2") == 0) {
        return "font/woff2";
    }
    if (strcmp(dot, ".ttf") == 0) {
        return "font/ttf";
    }
    return "application/octet-stream";
}

/* ── Sending ───────────────────────────────────────────────────────────────
 *
 * Assets are stored pre-gzipped, so the robot never compresses anything at
 * runtime -- it sets Content-Encoding and streams the bytes as they sit on
 * flash. The ETag is size and mtime, which is enough to answer a repeat visit
 * with a 304 and no body at all. That is the cheapest possible response for
 * the case that dominates: a phone reloading a 101 KB bundle it already has.
 */

static void web_make_etag(const struct stat *st, char *out, size_t out_size)
{
    snprintf(out, out_size, "\"%llx-%llx\"",
             (unsigned long long)st->st_size,
             (unsigned long long)st->st_mtime);
}

static bool web_etag_matches(httpd_req_t *req, const char *etag)
{
    char header[MPX_WEB_ETAG_MAX];

    if (httpd_req_get_hdr_value_str(req, "If-None-Match", header, sizeof(header)) != ESP_OK) {
        return false;
    }
    return strcmp(header, etag) == 0;
}

static esp_err_t web_send_file(httpd_req_t *req, const char *full_path,
                               const char *content_type, bool gzipped)
{
    struct stat st;
    char etag[MPX_WEB_ETAG_MAX];
    FILE *file;
    char *scratch;

    if (stat(full_path, &st) != 0 || S_ISDIR(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }

    web_make_etag(&st, etag, sizeof(etag));
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "ETag", etag);
    /* no-cache, not no-store: the browser may keep the bytes, it just has to
     * ask first. Asset names here are not content-hashed (m.js is always
     * m.js), so a cached copy with no revalidation would survive a firmware
     * update and show the old UI against the new API. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (gzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    }

    if (web_etag_matches(req, etag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }

    file = fopen(full_path, "rb");
    if (!file) {
        return ESP_ERR_NOT_FOUND;
    }
    scratch = http_server_alloc_scratch_buffer();
    if (!scratch) {
        fclose(file);
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    while (!feof(file)) {
        size_t read_bytes = fread(scratch, 1, HTTP_SERVER_SCRATCH_SIZE, file);

        if (read_bytes == 0) {
            /* Same reasoning as http_server_files_api.c: a hard read error
             * leaves feof() false, so without this the loop spins forever
             * holding a worker and its buffer. Abort the connection rather
             * than closing the chunked stream cleanly, so the client sees a
             * broken transfer instead of a silently truncated file. */
            if (ferror(file)) {
                free(scratch);
                fclose(file);
                return ESP_FAIL;
            }
            break;
        }
        if (httpd_resp_send_chunk(req, scratch, read_bytes) != ESP_OK) {
            free(scratch);
            fclose(file);
            return ESP_FAIL;
        }
    }

    free(scratch);
    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* Try "<dir><rel>.gz" first, then "<dir><rel>". Everything the build stages is
 * gzipped, but a file dropped in by hand later need not be. */
static esp_err_t web_send_asset(httpd_req_t *req, const char *dir, const char *rel)
{
    char full[HTTP_SERVER_PATH_MAX];
    const char *type = web_content_type(rel);

    if (web_system_path(dir, rel, ".gz", full, sizeof(full)) == ESP_OK) {
        esp_err_t err = web_send_file(req, full, type, true);
        if (err != ESP_ERR_NOT_FOUND) {
            return err;
        }
    }
    if (web_system_path(dir, rel, NULL, full, sizeof(full)) == ESP_OK) {
        return web_send_file(req, full, type, false);
    }
    return ESP_ERR_NOT_FOUND;
}

/* ── Handlers ──────────────────────────────────────────────────────────── */

static esp_err_t web_favicon_handler(httpd_req_t *req)
{
    return http_server_send_embedded_file(req, favicon_ico_start, favicon_ico_end,
                                          "image/x-icon");
}

static bool web_path_looks_like_a_file(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;

    return strchr(name, '.') != NULL;
}

static esp_err_t web_catch_all_handler(httpd_req_t *req)
{
    char path[HTTP_SERVER_PATH_MAX];
    esp_err_t err;

    if (!web_request_path(req, path, sizeof(path))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    /* ESP-Claw's own UI. It is a single self-contained page, so every path
     * under /settings resolves to it and the SolidJS router takes it from
     * there -- the same deal the PWA gets below. */
    if (strncmp(path, "/settings", 9) == 0 &&
            (path[9] == '\0' || path[9] == '/')) {
        err = web_send_asset(req, MPX_WEB_SETTINGS_DIR, "/" MPX_WEB_INDEX);
        if (err == ESP_ERR_NOT_FOUND) {
            httpd_resp_set_type(req, "text/html; charset=utf-8");
            return httpd_resp_send(req, s_no_assets_page, HTTPD_RESP_USE_STRLEN);
        }
        return err;
    }

    if (strcmp(path, "/") == 0) {
        strlcpy(path, "/" MPX_WEB_INDEX, sizeof(path));
    }

    err = web_send_asset(req, MPX_WEB_PWA_DIR, path);
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    /* An API path that no handler took is a missing endpoint, not a page:
     * answer 404 in JSON. Serving the PWA shell here (200, text/html) is what
     * made GET /v1/robot/look look "supported" to a client that probes it,
     * and turns every typo'd API call into a confusing JSON parse error. */
    if (strncmp(path, "/v1/", 4) == 0 || strcmp(path, "/v1") == 0 ||
        strncmp(path, "/api/", 5) == 0) {
        httpd_resp_set_status(req, HTTPD_404);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no such endpoint\"}");
    }

    /* A miss on something with a file extension is a genuine 404 -- answering
     * a request for a missing .js with a page of HTML is how you get a syntax
     * error in the console instead of a 404 in the network tab. A miss on an
     * extensionless path is a client-side route, and gets the shell. */
    if (web_path_looks_like_a_file(path)) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    err = web_send_asset(req, MPX_WEB_PWA_DIR, "/" MPX_WEB_INDEX);
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, s_no_assets_page, HTTPD_RESP_USE_STRLEN);
    }
    return err;
}

/* ── Dead-socket reaper ────────────────────────────────────────────────────
 *
 * Ported from the MPX-Dog firmware, where a socket census showed the LWIP pool
 * being exhausted not by load but by dead browser connections on port 80 that
 * httpd never freed. lru_purge_enable alone does not fix that: it evicts the
 * oldest IDLE session when a new one arrives, and the oldest idle session is
 * very often a live keep-alive socket the browser is about to reuse -- so the
 * reused socket dies on send and fetch() rejects before anything reaches the
 * robot. The screens that made it happen are still here: Servo Studio polls at
 * 140 ms, the chat holds a WebSocket, and the marketplace proxy takes an
 * outbound socket per request.
 *
 * This peeks at every port-80 socket and asks httpd to close the ones whose
 * peer has already gone. httpd_sess_trigger_close (not a raw close) means
 * httpd owns the close and ignores any fd that is not one of its sessions.
 */

#define MPX_WEB_REAP_INTERVAL_MS 4000

static void web_socket_reaper_task(void *arg)
{
    httpd_handle_t server = arg;

    for (;;) {
        int reaped = 0;

        vTaskDelay(pdMS_TO_TICKS(MPX_WEB_REAP_INTERVAL_MS));

        for (int fd = LWIP_SOCKET_OFFSET;
                fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; fd++) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            int type = 0;
            socklen_t type_len = sizeof(type);
            char probe;
            int peeked;

            if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0) {
                continue;   /* not an open socket */
            }
            if (getsockname(fd, (struct sockaddr *)&local, &len) != 0 ||
                    ntohs(local.sin_port) != 80) {
                continue;   /* not ours: the listener's peers, or the proxy's */
            }
            if (getpeername(fd, (struct sockaddr *)&local, &len) != 0) {
                continue;   /* the listening socket itself */
            }

            /* 0 from a peek means the peer sent FIN. -1 with EAGAIN means the
             * connection is alive with nothing to say, which is exactly what a
             * healthy keep-alive or WebSocket looks like. */
            peeked = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (peeked == 0) {
                httpd_sess_trigger_close(server, fd);
                reaped++;
            }
        }

        if (reaped > 0) {
            ESP_LOGI(TAG, "reaped %d dead socket(s)", reaped);
        }
    }
}

/* ── Registration ──────────────────────────────────────────────────────────
 *
 * Registered LAST, after every /v1 route: esp_http_server hands a request to
 * the first entry that matches, and the catch-all matches every GET. */

esp_err_t http_server_register_mpx_web_routes(httpd_handle_t server)
{
    static const httpd_uri_t handlers[] = {
        { .uri = "/favicon.ico", .method = HTTP_GET, .handler = web_favicon_handler },
        { .uri = "/*",           .method = HTTP_GET, .handler = web_catch_all_handler },
    };
    esp_err_t err = http_server_register_uri_table(server, handlers,
                                                   sizeof(handlers) / sizeof(handlers[0]),
                                                   "mpx web");

    if (xTaskCreate(web_socket_reaper_task, "sock_reap", 2560, server, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "socket reaper not started; dead sockets will linger");
    }
    return err;
}
