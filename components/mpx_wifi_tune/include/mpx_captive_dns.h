/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Captive-portal DNS for the robot's access point: every A lookup answers
 * with the AP's address, every other type answers "no record" at once.
 * Replaces ESP-Claw's captive_dns (see mpx_captive_dns.c for why).
 */
#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the AP's DHCP to offer the robot as DNS server and start the
 * responder on UDP 53. Call after the AP has its final IP. Idempotent. */
esp_err_t mpx_captive_dns_start(esp_netif_t *ap_netif);

#ifdef __cplusplus
}
#endif
