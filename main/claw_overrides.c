/*
 * claw_overrides.c -- link-time overrides for the esp-claw submodule.
 *
 * The submodule under third-party/esp-claw is never edited. Where one of its
 * defaults does not fit this robot, the fix lives here: the linker's --wrap
 * (see main/CMakeLists.txt) hands the call to __wrap_<fn>(), which adjusts
 * the arguments and continues into the real function as __real_<fn>().
 *
 * claw_event_router_init: app_claw.c starts the event_router task with an
 * 8 KB stack. That task runs every routing rule, and on this robot a single
 * chat message fans out from it into the local IM channel, the ESP-Claw web
 * socket, the PWA chat socket and the MCP bridge mirror. 8 KB overflows
 * ("A stack overflow in task event_router has been detected") and the robot
 * reboots. The stack is only ever raised, never lowered, and it is allocated
 * in PSRAM by claw_task_create(), so the bigger value costs no internal RAM.
 */
#include "sdkconfig.h"

#if CONFIG_MP4_ROBOT_ENABLE

#include "esp_err.h"
#include "esp_log.h"
#include "claw_event_router.h"

static const char *TAG = "claw_overrides";

esp_err_t __real_claw_event_router_init(const claw_event_router_config_t *config);

esp_err_t __wrap_claw_event_router_init(const claw_event_router_config_t *config)
{
    claw_event_router_config_t cfg;

    if (!config) {
        return __real_claw_event_router_init(config);
    }
    cfg = *config;
    if (cfg.task_stack_size < CONFIG_MP4_EVENT_ROUTER_STACK) {
        ESP_LOGI(TAG, "event_router stack %u -> %u bytes",
                 (unsigned)cfg.task_stack_size, (unsigned)CONFIG_MP4_EVENT_ROUTER_STACK);
        cfg.task_stack_size = CONFIG_MP4_EVENT_ROUTER_STACK;
    }
    return __real_claw_event_router_init(&cfg);
}

#endif /* CONFIG_MP4_ROBOT_ENABLE */
