/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mpx_rings.h"

#include <cstdlib>
#include <cstring>
#include <string>

#include "log_ring.h"
#include "trace_ring.h"

namespace {

char *dup_to_c(const std::string &s)
{
    char *out = (char *)malloc(s.size() + 1);
    if (out == nullptr) {
        return nullptr;
    }
    memcpy(out, s.data(), s.size());
    out[s.size()] = '\0';
    return out;
}

}  // namespace

extern "C" {

void mpx_log_ring_init(void)
{
    util::log_ring_init();
}

char *mpx_log_ring_json_alloc(uint32_t since, size_t max_lines, uint32_t *out_next)
{
    uint32_t next = since;
    const std::string json = util::log_ring_json(since, max_lines, next);
    if (out_next != nullptr) {
        *out_next = next;
    }
    return dup_to_c(json);
}

char *mpx_trace_ring_json_alloc(uint32_t since, size_t max_samples, uint32_t *out_next)
{
    uint32_t next = since;
    const std::string json = util::trace_ring_json(since, max_samples, next);
    if (out_next != nullptr) {
        *out_next = next;
    }
    return dup_to_c(json);
}

void mpx_ring_json_free(char *json)
{
    free(json);
}

}  // extern "C"
