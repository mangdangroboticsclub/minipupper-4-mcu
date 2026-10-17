#pragma once

#include "esp_http_server.h"

namespace network {

/**
 * @brief Register the /v1/uros routes (the RL Walk page's ROS 2 card).
 *
 * @param server  The running httpd.
 * @param reg     http_server.cc's register_uri(), as register_display_routes().
 * @return number of routes registered (0 with micro-ROS compiled out).
 */
int register_uros_routes(httpd_handle_t server,
                         bool (*reg)(httpd_handle_t, const httpd_uri_t *, const char *));

}  // namespace network
