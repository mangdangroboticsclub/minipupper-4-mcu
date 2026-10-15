#include "autorun.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "events.h"
#include "registry.h"
#include "runner.h"

static const char *TAG = "autorun";

namespace skills {
namespace {

constexpr const char *NVS_NS  = "mpx";
constexpr const char *NVS_KEY = "arun_fail";

bool s_safe_mode = false;

int read_attempts()
{
	nvs_handle_t h;
	if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
	int32_t v = 0;
	nvs_get_i32(h, NVS_KEY, &v);
	nvs_close(h);
	return static_cast<int>(v);
}

void write_attempts(int v)
{
	nvs_handle_t h;
	if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
	nvs_set_i32(h, NVS_KEY, v);
	nvs_commit(h);
	nvs_close(h);
}

/* Clears the counter once the robot has been up long enough to call this boot
 * a success. Deliberately time-based rather than "the skill returned": a
 * behaviour is supposed to keep running, so waiting for it to finish would
 * never clear the counter, and a skill that reboots the robot never gets here
 * either way. */
void prove_task(void *)
{
	vTaskDelay(pdMS_TO_TICKS(AUTORUN_PROVEN_MS));
	if (read_attempts() != 0) {
		write_attempts(0);
		ESP_LOGI(TAG, "boot proven after %d ms; autorun counter cleared",
		         AUTORUN_PROVEN_MS);
	}
	vTaskDelete(nullptr);
}

}  // namespace

void autorun_boot()
{
	const Entry *e = autorun_entry();

	// Fire the boot event either way: "on":["boot"] and "autorun":true are
	// different requests, and a robot with neither should still tick over.
	const bool fired_event = fire(EVENT_BOOT);

	if (!e) {
		if (!fired_event) ESP_LOGI(TAG, "no autorun skill");
		return;
	}

	const int attempts = read_attempts();
	if (attempts >= AUTORUN_MAX_ATTEMPTS) {
		s_safe_mode = true;
		ESP_LOGW(TAG, "SAFE MODE: '%s' is marked autorun but the last %d boots "
		              "did not stay up. Not starting it.", e->slug.c_str(), attempts);
		ESP_LOGW(TAG, "  The robot is running normally and the web UI works. "
		              "Uninstall the skill, or clear safe mode, to try again.");
		return;
	}

	write_attempts(attempts + 1);
	xTaskCreate(prove_task, "arun_prove", 2048, nullptr, 2, nullptr);

	if (fired_event) {
		ESP_LOGI(TAG, "'%s' is marked autorun but a boot-event skill already "
		              "started; skipping", e->slug.c_str());
		return;
	}

	ESP_LOGI(TAG, "autorun '%s' (attempt %d of %d)",
	         e->slug.c_str(), attempts + 1, AUTORUN_MAX_ATTEMPTS);

	start(e->file.c_str(), e->slug.c_str(), "",
	      e->behaviour ? Mode::Behaviour : Mode::OneShot, "boot");
}

bool safe_mode() { return s_safe_mode; }

void clear_safe_mode()
{
	write_attempts(0);
	s_safe_mode = false;
	ESP_LOGI(TAG, "safe mode cleared; autorun will run on the next boot");
}

}  // namespace skills
