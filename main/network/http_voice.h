/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The /v1/ai and /v1/voice routes -- the app's "AI & Voice" settings and a
 * status/bench view of the voice link. See http_voice.cc for the shapes.
 */
#pragma once

#include "esp_http_server.h"

namespace network {

/** Register the routes. `reg` is http_server.cc's registration helper (it
 *  logs and counts); returns how many handlers were registered. Always
 *  registers /v1/ai (it offers only "off" when voice is compiled out). */
int register_voice_routes(httpd_handle_t server,
                          bool (*reg)(httpd_handle_t, const httpd_uri_t *, const char *));

}  // namespace network
