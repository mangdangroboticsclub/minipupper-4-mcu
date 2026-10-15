/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The robot's access point answers every DNS name with its own address, so
 * a phone that joins it pops up the setup page (a "captive portal").
 *
 * This replaces ESP-Claw's captive_dns (submodule, not edited), which made
 * the first visit painfully slow. It built every answer by copying the whole
 * query and appending an A record, which goes wrong in two ways phones hit
 * on every lookup:
 *
 *   - Modern resolvers add an EDNS0 OPT record to each query (Android, iOS,
 *     Windows all do). Copied verbatim it stays where it was -- in the
 *     additional section -- and the A record lands AFTER it, while the
 *     header claims one answer and one additional. The reply does not
 *     parse; the phone waits out its timeout and retries, sometimes over
 *     TCP, which nothing here answers.
 *   - Every question got an A record, including AAAA and HTTPS (type 65)
 *     questions that phones send alongside each A lookup. An answer of the
 *     wrong type is ignored as bogus, so those wait for a timeout too.
 *
 * Seconds per name, several names per page, and the portal took ages to
 * appear. Here an A (or ANY) question gets one A record; any other type gets
 * an empty, valid "no such record" (NOERROR, no answers) at once; EDNS is
 * dropped from the reply, which is allowed.
 */

#include "mpx_captive_dns.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "mpx_captive_dns";

#define DNS_PORT       53
#define DNS_BUF        512
#define DNS_TTL_S      30
#define DNS_TYPE_A     1
#define DNS_TYPE_ANY   255
#define DNS_CLASS_IN   1

static TaskHandle_t       s_task;
static esp_netif_t       *s_ap_netif;

/* The redirect target: the AP's address, read per query so a moved AP
 * (main.c puts it on 192.168.2.1 after start) is followed. Network order. */
static uint32_t ap_ip(void)
{
    esp_netif_ip_info_t info = {0};
    if (s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
        return info.ip.addr;
    }
    return 0;
}

/* Returns the reply length, or 0 to send nothing. */
static int build_reply(const uint8_t *q, int len, uint8_t *r, int rmax, uint32_t ip_net)
{
    int pos = 12;
    uint16_t qtype, qclass;
    const uint8_t opcode = (q[2] >> 3) & 0x0F;

    if (len < 12 || (q[2] & 0x80)) {            /* too short, or a response */
        return 0;
    }
    if (((q[4] << 8) | q[5]) < 1) {
        return 0;
    }
    /* QNAME: labels up to a zero byte; a query never compresses its name. */
    while (pos < len && q[pos] != 0) {
        if ((q[pos] & 0xC0) != 0) {
            return 0;
        }
        pos += 1 + q[pos];
    }
    pos += 1;                                    /* the zero byte */
    if (pos + 4 > len || pos + 4 + 16 > rmax) {
        return 0;
    }
    qtype = (uint16_t)((q[pos] << 8) | q[pos + 1]);
    qclass = (uint16_t)((q[pos + 2] << 8) | q[pos + 3]);
    pos += 4;

    memcpy(r, q, pos);                           /* header + the one question */
    r[2] = (uint8_t)(0x80 | (q[2] & 0x79) | 0x04);   /* QR, opcode, AA, RD */
    r[3] = 0x80;                                 /* RA, NOERROR */
    r[4] = 0; r[5] = 1;                          /* QDCOUNT */
    r[6] = 0; r[7] = 0;                          /* ANCOUNT */
    r[8] = 0; r[9] = 0;                          /* NSCOUNT */
    r[10] = 0; r[11] = 0;                        /* ARCOUNT: EDNS dropped */

    if (opcode != 0) {
        r[3] = 0x80 | 4;                         /* NOTIMP */
        return pos;
    }
    if ((qtype == DNS_TYPE_A || qtype == DNS_TYPE_ANY) && qclass == DNS_CLASS_IN && ip_net) {
        const uint8_t *ip = (const uint8_t *)&ip_net;   /* already network order */
        r[7] = 1;
        r[pos++] = 0xC0; r[pos++] = 0x0C;        /* name: the question's */
        r[pos++] = 0x00; r[pos++] = DNS_TYPE_A;
        r[pos++] = 0x00; r[pos++] = DNS_CLASS_IN;
        r[pos++] = 0; r[pos++] = 0; r[pos++] = 0; r[pos++] = DNS_TTL_S;
        r[pos++] = 0x00; r[pos++] = 0x04;
        r[pos++] = ip[0]; r[pos++] = ip[1]; r[pos++] = ip[2]; r[pos++] = ip[3];
    }
    /* Anything else: NOERROR with no answer -- "no such record", at once. */
    return pos;
}

static void dns_task(void *arg)
{
    (void)arg;
    static uint8_t q[DNS_BUF];
    static uint8_t r[DNS_BUF + 16];
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cannot listen on UDP %d", DNS_PORT);
        if (sock >= 0) {
            close(sock);
        }
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "answering every name with the robot's address");
    for (;;) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        const int len = recvfrom(sock, q, sizeof(q), 0, (struct sockaddr *)&src, &src_len);
        if (len <= 0) {
            continue;
        }
        const int n = build_reply(q, len, r, sizeof(r), ap_ip());
        if (n > 0) {
            sendto(sock, r, n, 0, (struct sockaddr *)&src, src_len);
        }
    }
}

esp_err_t mpx_captive_dns_start(esp_netif_t *ap_netif)
{
    esp_netif_ip_info_t info = {0};
    esp_netif_dns_info_t dns = {0};
    esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
    uint8_t offer_dns = 0x02;   /* OFFER_DNS */
    esp_err_t err;

    if (!ap_netif) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task) {
        return ESP_OK;
    }
    s_ap_netif = ap_netif;

    /* DHCP hands phones this robot as their DNS server. */
    if (esp_netif_get_ip_info(ap_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = info.ip.addr;
    esp_netif_dhcps_get_status(ap_netif, &status);
    if (status == ESP_NETIF_DHCP_STARTED) {
        esp_netif_dhcps_stop(ap_netif);
    }
    err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer_dns, sizeof(offer_dns));
    if (err == ESP_OK) {
        err = esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (status == ESP_NETIF_DHCP_STARTED) {
        esp_netif_dhcps_start(ap_netif);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP will not offer the robot as DNS: %s", esp_err_to_name(err));
    }

    /* Priority above the web server's (5): a lookup is a few microseconds of
     * work, and it is what every page load waits on first. */
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 6, &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
