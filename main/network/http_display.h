#pragma once

#include "esp_http_server.h"

namespace network {

/**
 * @brief Register the /v1/display routes (the app's Display page).
 *
 * @param server  The running httpd.
 * @param reg     http_server.cc's register_uri(): registers one handler and
 *                logs by name when the table is full. Routes stay in one
 *                table so the max_uri_handlers count has one owner.
 * @return number of routes registered (0 with the display compiled out).
 */
int register_display_routes(httpd_handle_t server,
                            bool (*reg)(httpd_handle_t, const httpd_uri_t *, const char *));

}  // namespace network
