#pragma once

#include <cstddef>
#include <string>

namespace network {

/**
 * @brief Make an HTTP request to the marketplace Gateway and return the
 *        response body as a string.
 *
 * Opens a TCP connection to the Gateway host (from Kconfig) on the STA
 * interface, sends an HTTP request with the given method, path, and
 * optional body, then reads and returns the full response body.
 *
 * @param method  HTTP method: "GET", "POST", "PATCH", "DELETE"
 * @param path    URL path on the Gateway, e.g. "/v1/skills" or
 *                "/v1/robots/MPX-DOG-01/skills"
 * @param body    Request body (empty for GET/DELETE)
 * @param ok      [out] set to true if the server returned 2xx
 * @return        Response body string, or empty on failure
 */
std::string gateway_request(const char *method,
                            const char *path,
                            const std::string &body,
                            bool &ok);

}  // namespace network
