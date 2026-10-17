#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/* ── System monitor: what the robot is spending its CPU and RAM on ──────────
 *
 * A low-priority task samples the FreeRTOS task table every
 * CONFIG_APP_SYSMON_SAMPLE_MS and keeps the latest result: per-core CPU load
 * (from the idle tasks' run time), internal RAM and PSRAM, the busiest tasks
 * and the task closest to overflowing its stack.
 *
 * Two readers:
 *  - GET /v1/system/stats serves sys_monitor_json() (the PWA's widget);
 *  - every CONFIG_APP_SYSMON_LOG_INTERVAL_S a summary goes to the serial log,
 *    with CPU averaged over that whole interval rather than the last sample.
 *
 * Internal RAM is the figure to watch: the Wi-Fi driver, I2S DMA and esp-sr
 * all need it, and when it runs out the symptoms (phones timing out on the
 * AP, the microphone failing to open) do not point back at memory.
 *
 * With CONFIG_APP_SYSMON_ENABLE off every call answers "not running".
 */
namespace util {

constexpr int         SYSMON_MAX_CORES     = 2;
constexpr int         SYSMON_TOP_TASKS     = 5;
constexpr std::size_t SYSMON_TASK_NAME_MAX = 16;   /* including the terminator */

struct SysMemStats {
    std::uint32_t total;
    std::uint32_t free;
    std::uint32_t min_free;   /* low-water mark since boot */
    std::uint32_t largest;    /* largest single allocation that would succeed */
};

struct SysTaskLoad {
    char          name[SYSMON_TASK_NAME_MAX];
    std::int8_t   core;         /* pinned core, or -1 for either */
    std::uint8_t  cpu_pct;      /* of one core, over the window */
    std::uint32_t stack_free;   /* bytes; the least it has ever had */
};

struct SysStats {
    std::uint32_t uptime_s;
    std::uint32_t window_ms;    /* what the CPU figures cover; 0 before the second sample */
    bool          cpu_valid;
    std::uint8_t  cores;
    std::uint8_t  cpu_pct[SYSMON_MAX_CORES];
    SysMemStats   internal;
    SysMemStats   psram;        /* all zero on a board without PSRAM */
    std::uint16_t task_count;
    std::uint8_t  top_count;
    SysTaskLoad   top[SYSMON_TOP_TASKS];   /* busiest first, idle tasks excluded */
    SysTaskLoad   low_stack;               /* least stack headroom of any task */
};

/** Start the sampling task. Safe to call once, as early as you like. */
bool sys_monitor_start();

/** Copy out the latest sample. False until the first one is taken. */
bool sys_monitor_get(SysStats &out);

/**
 * @brief The latest sample as JSON, for GET /v1/system/stats.
 *
 * `{"ok":true,"uptime_s":..,"window_ms":..,"cpu":[34,12] | null,
 *   "internal":{"total","free","min_free","largest"},"psram":{...},
 *   "tasks":{"count":..,"top":[{"name","core","cpu","stack_free"}],
 *            "low_stack":{"name","core","stack_free"}}}`
 * or `{"ok":false,"error":"..."}` when the monitor is off or not started.
 */
std::string sys_monitor_json();

}  // namespace util
