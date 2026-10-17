/*
 * claw_overrides.c -- link-time overrides for the vendored ESP-Claw tree.
 *
 * third-party/esp-claw is never edited. Where one of its defaults does not
 * fit this robot, the fix lives here: the linker's --wrap (main/CMakeLists.txt)
 * hands the call to __wrap_<fn>(), which adjusts the arguments and continues
 * into the real function as __real_<fn>(). Lifted from mpv2-claw's
 * main/claw_overrides.c.
 *
 * claw_event_router_init: app_claw.c starts the event_router task with an
 * 8 KB stack and offers no Kconfig for it. That task runs every routing rule
 * -- run_agent included, which formats JSON and walks the session store --
 * and on mpv2-claw a single chat message fanning out to the web channel and
 * the PWA bridge overflowed it ("A stack overflow in task event_router has
 * been detected"). The stack is only ever raised, never lowered, and
 * claw_task_create() puts it in PSRAM, so the bigger value costs no internal
 * RAM.
 */
#include "sdkconfig.h"

#if CONFIG_APP_AGENT_ENABLE

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
	if (cfg.task_stack_size < CONFIG_APP_AGENT_EVENT_ROUTER_STACK) {
		ESP_LOGI(TAG, "event_router stack %u -> %u bytes",
		         (unsigned)cfg.task_stack_size,
		         (unsigned)CONFIG_APP_AGENT_EVENT_ROUTER_STACK);
		cfg.task_stack_size = CONFIG_APP_AGENT_EVENT_ROUTER_STACK;
	}
	return __real_claw_event_router_init(&cfg);
}

#endif /* CONFIG_APP_AGENT_ENABLE */
