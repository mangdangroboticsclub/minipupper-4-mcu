#include "network/captive.h"

#include <cstring>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "network/wifi_ap.h"

namespace network {

namespace {

const char *TAG = "captive";
constexpr int DNS_PORT = 53;
constexpr int DNS_MAX = 512;
constexpr uint32_t DNS_TTL_S = 60;
bool s_started = false;

/* Only the phones' and laptops' "is there internet?" checks get the robot's address. Every other name gets a
 * fast "no such name": when every name was the robot, every app on the phone (and every other browser tab)
 * sent its traffic to the robot's small web server, which has 5 connections for the app. */
const char *const CHECK_HOSTS[] = {
    "connectivitycheck.gstatic.com", "www.gstatic.com", "connectivitycheck.android.com",
    "clients1.google.com", "clients3.google.com", "www.google.com", "play.googleapis.com",
    "captive.apple.com", "www.apple.com", "www.appleiphonecell.com", "www.itools.info", "www.ibook.info",
    "www.airport.us", "www.thinkdifferent.us",
    "www.msftconnecttest.com", "ipv6.msftconnecttest.com", "www.msftncsi.com", "dns.msftncsi.com",
    "detectportal.firefox.com", "nmcheck.gnome.org", "connectivity-check.ubuntu.com",
    "connect.rom.miui.com", "connectivitycheck.platform.hicloud.com", "connectivitycheck.cbg-app.huawei.com",
    "wifi.vivo.com.cn", "conn1.oppomobile.com", "conn2.oppomobile.com", "www.qualcomm.cn",
    "connectivitycheck.samsung.com", "captiveportal.samsung.com",
};

bool is_check_host(const char *name)
{
    for (const char *h : CHECK_HOSTS)
        if (strcasecmp(name, h) == 0) return true;
    return false;
}

/* One reply for one query, in place in `buf` (DNS_MAX bytes); returns its length, 0 to stay silent. */
int dns_answer(uint8_t *buf, int len, uint32_t ip_be)
{
    if (len < 12) return 0;
    const uint16_t flags = (uint16_t)(buf[2] << 8 | buf[3]);
    if (flags & 0x8000) return 0;                       // a response, not a question
    if (((flags >> 11) & 0xF) != 0) return 0;           // only standard queries
    const uint16_t qd = (uint16_t)(buf[4] << 8 | buf[5]);
    if (qd < 1) return 0;

    // the first question: its name (kept, dotted, for the check-host test), type and class
    char name[128];
    int nl = 0;
    int p = 12;
    while (p < len && buf[p] != 0) {
        if ((buf[p] & 0xC0) != 0) return 0;             // no compression in a question
        const int l = buf[p];
        if (p + 1 + l > len) return 0;
        if (nl + l + 1 < (int)sizeof(name)) {
            if (nl) name[nl++] = '.';
            std::memcpy(name + nl, buf + p + 1, l);
            nl += l;
        }
        p += l + 1;
    }
    name[nl] = '\0';
    p += 1;                                             // the root label
    if (p + 4 > len) return 0;
    const uint16_t qtype  = (uint16_t)(buf[p] << 8 | buf[p + 1]);
    const uint16_t qclass = (uint16_t)(buf[p + 2] << 8 | buf[p + 3]);
    p += 4;                                             // end of the first question

    const bool known = is_check_host(name);
    const bool a = known && (qtype == 1 || qtype == 255) && (qclass & 0x7FFF) == 1;
    if (a && p + 16 > DNS_MAX) return 0;

    // header: same id; QR, AA, RD as asked, RA; NOERROR (a check host) or NXDOMAIN (anything else);
    // one question, one answer or none
    buf[2] = (uint8_t)(0x84 | (flags >> 8 & 0x01));
    buf[3] = known ? 0x80 : 0x83;
    buf[4] = 0; buf[5] = 1;
    buf[6] = 0; buf[7] = a ? 1 : 0;
    buf[8] = 0; buf[9] = 0;
    buf[10] = 0; buf[11] = 0;                           // EDNS and the rest dropped
    if (!a) return p;                                   // not a check host: NXDOMAIN; AAAA, HTTPS, ...: no record

    uint8_t *o = buf + p;
    *o++ = 0xC0; *o++ = 0x0C;                           // the name: the question's
    *o++ = 0; *o++ = 1;                                 // A
    *o++ = 0; *o++ = 1;                                 // IN
    *o++ = (uint8_t)(DNS_TTL_S >> 24); *o++ = (uint8_t)(DNS_TTL_S >> 16);
    *o++ = (uint8_t)(DNS_TTL_S >> 8);  *o++ = (uint8_t)DNS_TTL_S;
    *o++ = 0; *o++ = 4;
    std::memcpy(o, &ip_be, 4);
    return p + 16;
}

void dns_task(void *)
{
    const uint32_t ip_be = inet_addr(AP_IP_ADDR);
    int s = -1;
    for (;;) {
        if (s < 0) {
            s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (s >= 0) {
                sockaddr_in a = {};
                a.sin_family = AF_INET;
                a.sin_port = htons(DNS_PORT);
                a.sin_addr.s_addr = ip_be;              // the AP only: home-network clients never see it
                if (bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0) {
                    ESP_LOGW(TAG, "DNS: bind %s:53 failed (errno %d), retrying", AP_IP_ADDR, errno);
                    close(s);
                    s = -1;
                } else {
                    ESP_LOGI(TAG, "DNS on %s:53: the internet checks are the robot, other names do not exist", AP_IP_ADDR);
                }
            }
            if (s < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
        }
        uint8_t buf[DNS_MAX];
        sockaddr_in from = {};
        socklen_t fl = sizeof(from);
        const int n = recvfrom(s, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &fl);
        if (n < 0) {
            ESP_LOGW(TAG, "DNS: recv failed (errno %d), reopening", errno);
            close(s);
            s = -1;
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        const int m = dns_answer(buf, n, ip_be);
        if (m > 0) sendto(s, buf, m, 0, reinterpret_cast<sockaddr *>(&from), fl);
    }
}

bool path_is(const char *uri, const char *path)
{
    const size_t n = std::strlen(path);
    return std::strncmp(uri, path, n) == 0 && (uri[n] == '\0' || uri[n] == '?');
}

}  // namespace

void captive_dns_start()
{
    if (s_started) return;
    s_started = true;
    // Small and rare: one task, one socket, a 512-byte buffer on its stack.
    if (xTaskCreatePinnedToCore(dns_task, "captive_dns", 3072, nullptr, 3, nullptr, 0) != pdPASS) {
        ESP_LOGE(TAG, "DNS task not started");
        s_started = false;
    }
}

esp_err_t captive_http_handler(httpd_req_t *req)
{
    const char *u = req->uri;
    if (path_is(u, "/generate_204") || path_is(u, "/gen_204")) {           // Android, Chrome
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, nullptr, 0);
    }
    const char *body = nullptr, *type = "text/plain";
    if (path_is(u, "/hotspot-detect.html") || path_is(u, "/library/test/success.html")) {   // Apple
        body = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
        type = "text/html";
    } else if (path_is(u, "/connecttest.txt")) {                             // Windows 10/11
        body = "Microsoft Connect Test";
    } else if (path_is(u, "/ncsi.txt")) {                                    // Windows, older
        body = "Microsoft NCSI";
    } else if (path_is(u, "/success.txt")) {                                 // Firefox
        body = "success\n";
    }
    if (body) {
        httpd_resp_set_type(req, type);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Nothing matches the given URI");
}

}  // namespace network
