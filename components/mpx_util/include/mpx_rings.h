/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * C access to the log and trace rings.
 *
 * log_ring.h and trace_ring.h are C++ and return std::string. Their producer
 * (the WASM runtime, also C++) is happy with that; their consumer is the HTTP
 * layer, which is C. Rather than make that file C++ just to read a ring, this
 * hands back a malloc'd buffer the caller frees.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install the log ring's vprintf hook.
 *
 * Call this FIRST in app_main, before anything logs, so the ring captures the
 * boot sequence too -- that is exactly the part you want when a device will
 * not come up. It chains to the original sink, so serial output is unchanged.
 */
void mpx_log_ring_init(void);

/**
 * @brief Render log lines newer than `since` as the JSON GET /v1/logs returns.
 *
 * @param out_next  receives the sequence number to pass as `since` next time.
 * @return malloc'd NUL-terminated JSON, or NULL. Free with mpx_ring_json_free().
 */
char *mpx_log_ring_json_alloc(uint32_t since, size_t max_lines, uint32_t *out_next);

/** @brief The same, for the trace ring and GET /v1/trace. */
char *mpx_trace_ring_json_alloc(uint32_t since, size_t max_samples, uint32_t *out_next);

void mpx_ring_json_free(char *json);

#ifdef __cplusplus
}
#endif
