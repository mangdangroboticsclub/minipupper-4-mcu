#pragma once
/* captive.h -- keep phones on the robot's own Wi-Fi (MPX-Dog, 192.168.2.1).
 *
 * A phone that joins a Wi-Fi asks two things within a few seconds: a name (DNS) and a known page
 * ("is there internet?": Android's /generate_204, Apple's /hotspot-detect.html, Windows'
 * /connecttest.txt). The AP hands out its own address as the DNS server, and nothing answered it,
 * so phones judged the network broken and left (21 Oct: "leave ... reason = 3", 6 s after joining)
 * for the home Wi-Fi or mobile data, and the app lost the robot.
 *
 *   captive_dns_start()   a DNS server on the AP's address, port 53: every A question is answered
 *                         with 192.168.2.1, anything else with an empty answer. Only clients of the
 *                         AP ask it; the robot's own lookups go to the router as before.
 *   captive_http_handler  the catch-all GET route ("/" + "*"), registered LAST: the connectivity checks get the
 *                         answers their systems expect, any other unknown path a 404 as before. */
#include "esp_http_server.h"

namespace network {

void captive_dns_start();
esp_err_t captive_http_handler(httpd_req_t *req);

}  // namespace network
