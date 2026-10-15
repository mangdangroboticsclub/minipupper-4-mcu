#include "events.h"

#include <cstddef>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "imu.h"
#include "registry.h"
#include "runner.h"

static const char *TAG = "events";

namespace skills {
namespace {

struct Cooldown { std::string event; std::int64_t until_us; };
std::vector<Cooldown> s_cooldowns;

bool in_cooldown(const char *event)
{
	const std::int64_t now = esp_timer_get_time();
	for (auto &c : s_cooldowns) {
		if (c.event == event) {
			if (now < c.until_us) return true;
			c.until_us = now + (std::int64_t)EVENT_COOLDOWN_MS * 1000;
			return false;
		}
	}
	s_cooldowns.push_back({ event, now + (std::int64_t)EVENT_COOLDOWN_MS * 1000 });
	return false;
}

/* ── The IMU watcher ──────────────────────────────────────────────────────
 *
 * Thresholds over a short moving state, not instantaneous values. A quadruped
 * walking produces plenty of momentary readings that look like being picked
 * up; requiring the condition to hold for several consecutive samples is what
 * separates "lifted" from "took a step".
 *
 * Deliberately crude. This is a trigger, not an estimator -- a skill that
 * cares about attitude should read the IMU itself at whatever rate it likes.
 */
constexpr int   SAMPLE_MS       = 100;
constexpr int   HOLD_SAMPLES    = 4;     /* 400 ms                          */
constexpr float LIFTED_G        = 0.55f; /* sustained low vertical load     */
constexpr float FALLEN_TILT_G   = 0.60f; /* gravity mostly off the z axis   */
constexpr float SHAKEN_DPS      = 260.0f;

void watcher_task(void *)
{
	int lifted = 0, fallen = 0, shaken = 0;

	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));

		// Nothing to trigger while a skill runs, so do not even sample.
		if (running()) { lifted = fallen = shaken = 0; continue; }

		const robot::ImuData d = robot::imu_read();

		const float az = d.az < 0 ? -d.az : d.az;
		const float gmag = (d.gx < 0 ? -d.gx : d.gx)
		                 + (d.gy < 0 ? -d.gy : d.gy)
		                 + (d.gz < 0 ? -d.gz : d.gz);

		lifted = (az < LIFTED_G)      ? lifted + 1 : 0;
		fallen = (az < FALLEN_TILT_G) ? fallen + 1 : 0;
		shaken = (gmag > SHAKEN_DPS)  ? shaken + 1 : 0;

		// Fallen is the more specific claim, so it is tested first: a robot on
		// its side also reads as unloaded, and reporting both would fire two
		// skills for one event.
		if (fallen >= HOLD_SAMPLES)      { fallen = lifted = 0; fire(EVENT_FALLEN); }
		else if (lifted >= HOLD_SAMPLES) { lifted = 0;          fire(EVENT_LIFTED); }
		if (shaken >= 2)                 { shaken = 0;          fire(EVENT_SHAKEN); }
	}
}

}  // namespace

bool fire(const char *event)
{
	if (!event || !*event) return false;

	auto matches = by_event(event);
	if (matches.empty()) return false;

	if (running()) {
		ESP_LOGI(TAG, "%s ignored: '%s' is running", event, current().c_str());
		return false;
	}
	if (in_cooldown(event)) {
		ESP_LOGD(TAG, "%s ignored: in cooldown", event);
		return false;
	}

	// If two skills both want an event, the first in the registry wins and the
	// rest are logged. Running several at once is impossible, and picking one
	// arbitrarily without saying so would look like the others were broken.
	const Entry *e = matches.front();
	for (std::size_t i = 1; i < matches.size(); ++i)
		ESP_LOGW(TAG, "'%s' also wants %s; only one skill runs at a time",
		         matches[i]->slug.c_str(), event);

	const std::string why = std::string("event:") + event;
	return start(e->file.c_str(), e->slug.c_str(), "",
	             e->behaviour ? Mode::Behaviour : Mode::OneShot, why.c_str());
}

bool fire_chat(const char *message)
{
	if (!message) return false;

	// Match the first word, lowercased. "Dance!" and "dance" are the same
	// request, and anything cleverer belongs in the chat layer, not here.
	std::string word;
	for (const char *p = message; *p && word.size() < 24; ++p) {
		const unsigned char c = static_cast<unsigned char>(*p);
		if (std::isalnum(c)) word += static_cast<char>(std::tolower(c));
		else if (!word.empty()) break;
	}
	if (word.empty()) return false;

	return fire((std::string(EVENT_CHAT_PREFIX) + word).c_str());
}

void events_start()
{
	if (xTaskCreate(watcher_task, "imu_events", 3072, nullptr, 3, nullptr) != pdPASS) {
		ESP_LOGE(TAG, "could not start the IMU watcher");
		return;
	}
	ESP_LOGI(TAG, "IMU event watcher running (%d ms sampling)", SAMPLE_MS);
}

}  // namespace skills
