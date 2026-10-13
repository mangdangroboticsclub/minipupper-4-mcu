#include "util/sys_monitor.h"

#include <cstdio>
#include <cstring>

#include "sdkconfig.h"

#if CONFIG_APP_SYSMON_ENABLE

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
#error "APP_SYSMON_ENABLE needs FREERTOS_GENERATE_RUN_TIME_STATS (its Kconfig selects it)"
#endif

static const char *TAG = "sysmon";

namespace util {
namespace {

static_assert(configNUMBER_OF_CORES <= SYSMON_MAX_CORES,
              "SysStats::cpu_pct has room for SYSMON_MAX_CORES cores");

constexpr std::uint32_t INTERNAL_CAPS      = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
constexpr std::uint32_t TABLE_CAPS         = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr std::uint32_t STACK_WARN_BYTES   = 512;
/* Tasks can be created between uxTaskGetNumberOfTasks() and the snapshot. */
constexpr UBaseType_t   TABLE_SLACK        = 8;
constexpr std::uint32_t MONITOR_STACK      = 4096;
constexpr UBaseType_t   MONITOR_PRIORITY   = 1;
constexpr int64_t       US_PER_MS          = 1000;
constexpr int64_t       US_PER_S           = 1000 * 1000;
constexpr std::uint32_t BYTES_PER_KB       = 1024;

using RunTime = configRUN_TIME_COUNTER_TYPE;

/* The task table at the start of a measuring window. CPU load over the
 * window is each task's run-time counter now minus its counter here. */
struct Window {
    TaskStatus_t *tasks = nullptr;
    UBaseType_t   count = 0;
    RunTime       total = 0;
    int64_t       at_us = 0;
    bool          valid = false;
};

constexpr std::size_t LOG_TOP_LEN = SYSMON_TOP_TASKS * (SYSMON_TASK_NAME_MAX + 8);

struct Monitor {
    TaskStatus_t     *cur = nullptr;
    UBaseType_t       cap = 0;
    Window            sample;       /* since the previous sample: the PWA's view */
    Window            log;          /* since the previous serial log line */
    SysStats          work = {};    /* being built; only the monitor task touches it */
    char              log_top[LOG_TOP_LEN] = {};
    SemaphoreHandle_t lock = nullptr;   /* guards latest and has_latest */
    SysStats          latest = {};
    bool              has_latest = false;
};

Monitor s_mon;

bool grow_table(TaskStatus_t *&table, UBaseType_t cap)
{
    void *p = heap_caps_realloc(table, cap * sizeof(TaskStatus_t), TABLE_CAPS);
    if (!p) return false;
    table = static_cast<TaskStatus_t *>(p);
    return true;
}

bool ensure_capacity(Monitor &m, UBaseType_t want)
{
    if (want <= m.cap) return true;
    if (!grow_table(m.cur, want) || !grow_table(m.sample.tasks, want) ||
        !grow_table(m.log.tasks, want)) {
        return false;
    }
    m.cap = want;
    return true;
}

SysMemStats mem_stats(std::uint32_t caps)
{
    return {
        static_cast<std::uint32_t>(heap_caps_get_total_size(caps)),
        static_cast<std::uint32_t>(heap_caps_get_free_size(caps)),
        static_cast<std::uint32_t>(heap_caps_get_minimum_free_size(caps)),
        static_cast<std::uint32_t>(heap_caps_get_largest_free_block(caps)),
    };
}

void fill_task(SysTaskLoad &out, const TaskStatus_t &t, std::uint8_t cpu_pct)
{
    std::snprintf(out.name, sizeof(out.name), "%s", t.pcTaskName);
    out.core       = taskVALID_CORE_ID(t.xCoreID) ? static_cast<std::int8_t>(t.xCoreID) : -1;
    out.cpu_pct    = cpu_pct;
    out.stack_free = t.usStackHighWaterMark;
}

/* A task that did not exist at the window's start has run only inside it. */
RunTime run_since(const Window &w, const TaskStatus_t &t)
{
    for (UBaseType_t i = 0; i < w.count; i++) {
        if (w.tasks[i].xHandle == t.xHandle) return t.ulRunTimeCounter - w.tasks[i].ulRunTimeCounter;
    }
    return t.ulRunTimeCounter;
}

std::uint8_t percent(RunTime part, RunTime whole)
{
    const std::uint64_t pct = static_cast<std::uint64_t>(part) * 100 / whole;
    return static_cast<std::uint8_t>(pct > 100 ? 100 : pct);
}

bool is_idle_task(TaskHandle_t h)
{
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
        if (xTaskGetIdleTaskHandleForCore(core) == h) return true;
    }
    return false;
}

void insert_top(SysStats &s, const TaskStatus_t &t, std::uint8_t pct)
{
    int pos = s.top_count;
    while (pos > 0 && s.top[pos - 1].cpu_pct < pct) pos--;
    if (pos >= SYSMON_TOP_TASKS) return;

    const int last = s.top_count < SYSMON_TOP_TASKS ? s.top_count : SYSMON_TOP_TASKS - 1;
    for (int i = last; i > pos; i--) s.top[i] = s.top[i - 1];
    fill_task(s.top[pos], t, pct);
    if (s.top_count < SYSMON_TOP_TASKS) s.top_count++;
}

/* CPU figures in @p s over window @p w, ending at the table @p cur. */
void compute_load(const Window &w, const TaskStatus_t *cur, UBaseType_t n,
                  RunTime total, int64_t now_us, SysStats &s)
{
    s.cpu_valid = false;
    s.top_count = 0;
    s.window_ms = 0;
    const RunTime elapsed = total - w.total;
    if (!w.valid || elapsed == 0) return;

    s.window_ms = static_cast<std::uint32_t>((now_us - w.at_us) / US_PER_MS);
    for (BaseType_t core = 0; core < configNUMBER_OF_CORES; core++) {
        const TaskHandle_t idle = xTaskGetIdleTaskHandleForCore(core);
        s.cpu_pct[core] = 100;
        for (UBaseType_t i = 0; i < n; i++) {
            if (cur[i].xHandle == idle) {
                s.cpu_pct[core] = 100 - percent(run_since(w, cur[i]), elapsed);
                break;
            }
        }
    }
    for (UBaseType_t i = 0; i < n; i++) {
        if (is_idle_task(cur[i].xHandle)) continue;
        insert_top(s, cur[i], percent(run_since(w, cur[i]), elapsed));
    }
    s.cpu_valid = true;
}

void remember(Window &w, const TaskStatus_t *cur, UBaseType_t n, RunTime total, int64_t now_us)
{
    std::memcpy(w.tasks, cur, n * sizeof(TaskStatus_t));
    w.count = n;
    w.total = total;
    w.at_us = now_us;
    w.valid = true;
}

void find_low_stack(const TaskStatus_t *cur, UBaseType_t n, SysTaskLoad &out)
{
    std::memset(&out, 0, sizeof(out));
    const TaskStatus_t *low = nullptr;
    for (UBaseType_t i = 0; i < n; i++) {
        if (!low || cur[i].usStackHighWaterMark < low->usStackHighWaterMark) low = &cur[i];
    }
    if (low) fill_task(out, *low, 0);
}

void log_stats(Monitor &m, const SysStats &s)
{
    char cpu[24] = "n/a";
    if (s.cpu_valid) {
        int len = 0;
        for (int c = 0; c < s.cores && len < (int)sizeof(cpu); c++) {
            len += std::snprintf(cpu + len, sizeof(cpu) - len, "%s%u%%", c ? " " : "",
                                 (unsigned)s.cpu_pct[c]);
        }
    }
    ESP_LOGI(TAG, "up %lus | cpu %s over %lums | internal %luK free (min %luK, largest %luK) of %luK"
                  " | psram %luK free (min %luK) of %luK | %u tasks",
             (unsigned long)s.uptime_s, cpu, (unsigned long)s.window_ms,
             (unsigned long)(s.internal.free / BYTES_PER_KB),
             (unsigned long)(s.internal.min_free / BYTES_PER_KB),
             (unsigned long)(s.internal.largest / BYTES_PER_KB),
             (unsigned long)(s.internal.total / BYTES_PER_KB),
             (unsigned long)(s.psram.free / BYTES_PER_KB),
             (unsigned long)(s.psram.min_free / BYTES_PER_KB),
             (unsigned long)(s.psram.total / BYTES_PER_KB),
             (unsigned)s.task_count);

    if (s.top_count) {
        int len = 0;
        for (int i = 0; i < s.top_count && len < (int)sizeof(m.log_top); i++) {
            len += std::snprintf(m.log_top + len, sizeof(m.log_top) - len, "%s%s %u%%",
                                 i ? ", " : "", s.top[i].name, (unsigned)s.top[i].cpu_pct);
        }
        ESP_LOGI(TAG, "busiest (%% of one core): %s", m.log_top);
    }

    if (s.low_stack.stack_free < STACK_WARN_BYTES) {
        ESP_LOGW(TAG, "task '%s' has only %lu bytes of stack left at its deepest",
                 s.low_stack.name, (unsigned long)s.low_stack.stack_free);
    }
}

void sample_once(Monitor &m)
{
    if (!ensure_capacity(m, uxTaskGetNumberOfTasks() + TABLE_SLACK)) {
        ESP_LOGW(TAG, "no memory for the task table");
        return;
    }
    RunTime total = 0;
    const UBaseType_t n = uxTaskGetSystemState(m.cur, m.cap, &total);
    if (n == 0) return;   /* the table filled up between counting and copying */
    const int64_t now_us = esp_timer_get_time();

    SysStats &s = m.work;
    s = {};
    s.uptime_s   = static_cast<std::uint32_t>(now_us / US_PER_S);
    s.cores      = configNUMBER_OF_CORES;
    s.internal   = mem_stats(INTERNAL_CAPS);
    s.psram      = mem_stats(MALLOC_CAP_SPIRAM);
    s.task_count = static_cast<std::uint16_t>(n);
    find_low_stack(m.cur, n, s.low_stack);
    compute_load(m.sample, m.cur, n, total, now_us, s);
    remember(m.sample, m.cur, n, total, now_us);

    xSemaphoreTake(m.lock, portMAX_DELAY);
    m.latest = s;
    m.has_latest = true;
    xSemaphoreGive(m.lock);

    constexpr int64_t log_every_us = CONFIG_APP_SYSMON_LOG_INTERVAL_S * US_PER_S;
    if (log_every_us > 0 && (!m.log.valid || now_us - m.log.at_us >= log_every_us)) {
        compute_load(m.log, m.cur, n, total, now_us, s);
        log_stats(m, s);
        remember(m.log, m.cur, n, total, now_us);
    }
}

void monitor_task(void *)
{
    for (;;) {
        sample_once(s_mon);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_SYSMON_SAMPLE_MS));
    }
}

void append_json_string(std::string &out, const char *s)
{
    out += '"';
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') out += '\\';
        if (static_cast<unsigned char>(*s) >= 0x20) out += *s;
    }
    out += '"';
}

void append_mem(std::string &out, const char *key, const SysMemStats &m)
{
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  R"(,"%s":{"total":%lu,"free":%lu,"min_free":%lu,"largest":%lu})", key,
                  (unsigned long)m.total, (unsigned long)m.free,
                  (unsigned long)m.min_free, (unsigned long)m.largest);
    out += buf;
}

void append_task(std::string &out, const SysTaskLoad &t, bool with_cpu)
{
    char buf[64];
    out += R"({"name":)";
    append_json_string(out, t.name);
    if (with_cpu) {
        std::snprintf(buf, sizeof(buf), R"(,"core":%d,"cpu":%u,"stack_free":%lu})",
                      t.core, (unsigned)t.cpu_pct, (unsigned long)t.stack_free);
    } else {
        std::snprintf(buf, sizeof(buf), R"(,"core":%d,"stack_free":%lu})",
                      t.core, (unsigned long)t.stack_free);
    }
    out += buf;
}

}  // anonymous namespace

bool sys_monitor_start()
{
    if (s_mon.lock) return true;
    s_mon.lock = xSemaphoreCreateMutex();
    if (!s_mon.lock) return false;

    if (xTaskCreateWithCaps(monitor_task, "sysmon", MONITOR_STACK, nullptr,
                            MONITOR_PRIORITY, nullptr, TABLE_CAPS) != pdPASS) {
        vSemaphoreDelete(s_mon.lock);
        s_mon.lock = nullptr;
        ESP_LOGE(TAG, "could not start the monitor task");
        return false;
    }
    ESP_LOGI(TAG, "sampling every %d ms, logging every %d s",
             CONFIG_APP_SYSMON_SAMPLE_MS, CONFIG_APP_SYSMON_LOG_INTERVAL_S);
    return true;
}

bool sys_monitor_get(SysStats &out)
{
    if (!s_mon.lock) return false;
    xSemaphoreTake(s_mon.lock, portMAX_DELAY);
    const bool ok = s_mon.has_latest;
    if (ok) out = s_mon.latest;
    xSemaphoreGive(s_mon.lock);
    return ok;
}

namespace {

void append_stats_json(std::string &out, const SysStats &s)
{
    out.reserve(768);
    char buf[96];
    std::snprintf(buf, sizeof(buf), R"({"ok":true,"uptime_s":%lu,"window_ms":%lu,"cpu":)",
                  (unsigned long)s.uptime_s, (unsigned long)s.window_ms);
    out += buf;
    if (s.cpu_valid) {
        out += '[';
        for (int c = 0; c < s.cores; c++) {
            std::snprintf(buf, sizeof(buf), "%s%u", c ? "," : "", (unsigned)s.cpu_pct[c]);
            out += buf;
        }
        out += ']';
    } else {
        out += "null";
    }
    append_mem(out, "internal", s.internal);
    append_mem(out, "psram", s.psram);

    std::snprintf(buf, sizeof(buf), R"(,"tasks":{"count":%u,"top":[)", (unsigned)s.task_count);
    out += buf;
    for (int i = 0; i < s.top_count; i++) {
        if (i) out += ',';
        append_task(out, s.top[i], true);
    }
    out += R"(],"low_stack":)";
    append_task(out, s.low_stack, false);
    out += "}}";
}

}  // anonymous namespace

std::string sys_monitor_json()
{
    if (!s_mon.lock) return R"({"ok":false,"error":"monitor not started"})";

    std::string out;
    xSemaphoreTake(s_mon.lock, portMAX_DELAY);
    if (s_mon.has_latest) append_stats_json(out, s_mon.latest);
    xSemaphoreGive(s_mon.lock);
    return out.empty() ? R"({"ok":false,"error":"no sample yet"})" : out;
}

}  // namespace util

#else  // !CONFIG_APP_SYSMON_ENABLE

namespace util {

bool sys_monitor_start() { return false; }
bool sys_monitor_get(SysStats &) { return false; }
std::string sys_monitor_json() { return R"({"ok":false,"error":"system monitor disabled"})"; }

}  // namespace util

#endif  // CONFIG_APP_SYSMON_ENABLE
