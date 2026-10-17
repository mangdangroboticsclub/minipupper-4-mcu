#include "network/marketplace_proxy.h"
#include "network/wifi_sta.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <cerrno>
#include <fcntl.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

static const char *TAG = "marketplace_proxy";

#ifndef LWIP_SOCKET_OFFSET
#define LWIP_SOCKET_OFFSET 0
#endif

namespace network {

// ── Socket census — logs every open socket fd and its peer ─────────
// Called when socket() fails so we can see EXACTLY what is holding the pool
// (PWA clients on :80, proxy/upstream links to the gateway:8080, etc.).
static void log_open_sockets(const char *why)
{
    int count = 0;
    ESP_LOGE(TAG, "==== SOCKET CENSUS (%s) ====", why);
    for (int fd = LWIP_SOCKET_OFFSET;
         fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
        int type = 0;
        socklen_t tlen = sizeof(type);
        if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tlen) != 0) {
            continue;   // fd not an open socket
        }
        count++;
        struct sockaddr_in local {};
        struct sockaddr_in peer  {};
        socklen_t llen = sizeof(local), plen = sizeof(peer);
        char lbuf[16] = "-", pbuf[16] = "-";
        int lport = 0, pport = 0;
        if (getsockname(fd, (struct sockaddr *)&local, &llen) == 0) {
            inet_ntoa_r(local.sin_addr, lbuf, sizeof(lbuf));
            lport = ntohs(local.sin_port);
        }
        if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0) {
            inet_ntoa_r(peer.sin_addr, pbuf, sizeof(pbuf));
            pport = ntohs(peer.sin_port);
        }
        ESP_LOGE(TAG, "  fd=%d local=%s:%d peer=%s:%d", fd, lbuf, lport, pbuf, pport);
    }
    ESP_LOGE(TAG, "==== OPEN SOCKETS = %d / %d ====", count, CONFIG_LWIP_MAX_SOCKETS);
}

/* ── Configuration from Kconfig ────────────────────────────── */
static constexpr const char *GATEWAY_HOST = CONFIG_APP_CHAT_SERVER_IP;
static constexpr int         GATEWAY_PORT = CONFIG_APP_CHAT_SERVER_PORT;

/* ── Buffer sizes ───────────────────────────────────────────── */
static constexpr size_t HTTP_HEADER_BUF = 2048;
static constexpr size_t HTTP_BODY_BUF   = 16384;

/* ── Concurrency cap ────────────────────────────────────────────
 * The PWA can fire several marketplace requests at once (skills list +
 * robot skills + details).  Without a limit, each grabs a socket at the
 * same instant and — combined with the WS clients and upstream link —
 * exhausts the 16-socket LWIP pool ("Failed to create socket").  Cap the
 * number of simultaneous gateway requests; excess callers wait (or shed).
 */
static constexpr int MAX_INFLIGHT_GATEWAY = 3;

static SemaphoreHandle_t inflight_sem()
{
    // C++11 guarantees thread-safe one-time init of function-local statics.
    static SemaphoreHandle_t s =
        xSemaphoreCreateCounting(MAX_INFLIGHT_GATEWAY, MAX_INFLIGHT_GATEWAY);
    return s;
}

std::string gateway_request(const char *method,
                            const char *path,
                            const std::string &body,
                            bool &ok)
{
    ok = false;

    // ── Echo mode check ───────────────────────────────────────
    if (std::strcmp(GATEWAY_HOST, "echo") == 0) {
        ESP_LOGW(TAG, "Gateway is in echo mode — cannot proxy request");
        return {};
    }

    // ── Check STA connectivity ────────────────────────────────
    if (wifi_sta_get_state() != StaState::Connected) {
        ESP_LOGW(TAG, "STA not connected — cannot reach gateway");
        return {};
    }

    // ── Limit concurrent gateway requests (socket-pool guard) ──
    SemaphoreHandle_t sem = inflight_sem();
    if (!sem || xSemaphoreTake(sem, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "Too many in-flight gateway requests — shedding %s %s",
                 method, path);
        return {};
    }
    // RAII: release the slot on every return path below.
    struct SemGuard {
        SemaphoreHandle_t s;
        ~SemGuard() { if (s) xSemaphoreGive(s); }
    } sem_guard{ sem };

    // ── Resolve hostname ──────────────────────────────────────
    struct addrinfo hints = {};
    struct addrinfo *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%d", GATEWAY_PORT);

    int err = getaddrinfo(GATEWAY_HOST, port_str, &hints, &res);
    if (err != 0 || !res) {
        ESP_LOGE(TAG, "DNS resolution failed for %s: %d", GATEWAY_HOST, err);
        return {};
    }

    // ── Create TCP socket ─────────────────────────────────────
    int sock = socket(res->ai_family, res->ai_socktype, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket (errno=%d)", errno);
        log_open_sockets("socket() failed");
        freeaddrinfo(res);
        return {};
    }

    // ── Set timeouts ──────────────────────────────────────────
    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    // ── Force immediate close (no TIME_WAIT) ──────────────────
    // These proxy connections are short-lived and very frequent (the PWA
    // polls the marketplace).  Without this, each closed socket sits in
    // TIME_WAIT and the pool drains.  SO_LINGER{1,0} makes close() send a
    // RST and free the socket at once.
    struct linger lg = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));

    // ── Connect (bounded, non-blocking) ───────────────────────
    // Use a non-blocking connect with a 4 s select() timeout so a dead or
    // unreachable gateway fails fast instead of tying up a socket for the
    // full TCP connect timeout (tens of seconds).  Under repeated polling
    // that hang was a major contributor to socket-pool exhaustion.
    ESP_LOGI(TAG, "Connecting to gateway %s:%d ...", GATEWAY_HOST, GATEWAY_PORT);
    int fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl | O_NONBLOCK);

    int cres = connect(sock, res->ai_addr, res->ai_addrlen);
    bool connected = (cres == 0);
    if (cres != 0 && errno == EINPROGRESS) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        struct timeval ctv = { .tv_sec = 4, .tv_usec = 0 };
        if (select(sock + 1, nullptr, &wset, nullptr, &ctv) > 0) {
            int soerr = 0;
            socklen_t slen = sizeof(soerr);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &slen);
            connected = (soerr == 0);
        }
    }
    fcntl(sock, F_SETFL, fl);   // restore blocking mode for send/recv

    if (!connected) {
        ESP_LOGE(TAG, "TCP connect to gateway failed (timeout/unreachable)");
        close(sock);
        freeaddrinfo(res);
        return {};
    }
    freeaddrinfo(res);

    // ── Build HTTP request ────────────────────────────────────
    std::string request;
    request.reserve(512 + body.size());

    bool has_body = !body.empty() &&
                    (std::strcmp(method, "POST") == 0 ||
                     std::strcmp(method, "PATCH") == 0 ||
                     std::strcmp(method, "PUT") == 0);

    request += std::string(method) + " " + path + " HTTP/1.1\r\n";
    request += "Host: " + std::string(GATEWAY_HOST) + ":" + port_str + "\r\n";
    request += "Connection: close\r\n";
    request += "Accept: application/json\r\n";
    if (has_body) {
        request += "Content-Type: application/json\r\n";
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    if (has_body) {
        request += body;
    }

    // ── Send request ──────────────────────────────────────────
    size_t total_sent = 0;
    while (total_sent < request.size()) {
        int n = send(sock, request.data() + total_sent,
                     request.size() - total_sent, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "Send failed");
            close(sock);
            return {};
        }
        total_sent += n;
    }

    // ── Read response ─────────────────────────────────────────
    std::string response;
    response.reserve(HTTP_BODY_BUF);
    char buf[512];
    int n;

    while ((n = recv(sock, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = 0;
        response.append(buf, n);
    }

    close(sock);

    if (response.empty()) {
        ESP_LOGW(TAG, "Empty response from gateway");
        return {};
    }

    // ── Parse HTTP status line ────────────────────────────────
    // Look for "HTTP/1.1 200" or similar
    if (response.size() >= 12) {
        // Find the status code after the first space
        const char *start = response.data();
        const char *space1 = std::strchr(start, ' ');
        if (space1) {
            int status_code = std::atoi(space1 + 1);
            ok = (status_code >= 200 && status_code < 300);
            ESP_LOGI(TAG, "Gateway HTTP status: %d for %s %s",
                     status_code, method, path);
        } else {
            ESP_LOGW(TAG, "Could not find HTTP status in response: %.*s",
                     (int)std::min(response.size(), size_t(128)),
                     response.c_str());
        }
    } else {
        ESP_LOGW(TAG, "Response too short for HTTP status: %zu bytes",
                 response.size());
    }

    // ── Extract body (after "\r\n\r\n") ───────────────────────
    const char *header_end = std::strstr(response.data(), "\r\n\r\n");
    if (header_end) {
        std::string body_str(header_end + 4);
        ESP_LOGI(TAG, "Gateway response: %zu bytes (ok=%d)", body_str.size(), ok);
        return body_str;
    }

    // No header/body split — return whole response
    ESP_LOGI(TAG, "Gateway response: %zu bytes (no header split, ok=%d)",
             response.size(), ok);
    return response;
}

}  // namespace network
