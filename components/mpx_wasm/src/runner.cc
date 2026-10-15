#include "runner.h"

#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wasm_sandbox.h"

static const char *TAG = "runner";

namespace skills {
namespace {

struct Args {
	std::string path;
	std::string name;
	std::string params;
	Mode        mode;
	std::string why;
};

std::atomic<bool> s_running{false};
std::string   s_current, s_started_by, s_last_name;
Mode          s_mode        = Mode::OneShot;
int           s_last_result = 0;
std::uint32_t s_last_ms     = 0;
std::int64_t  s_started_us  = 0;

// One-shot skills keep the watchdog they have always had. A behaviour passes 0,
// which the sandbox reads as "no total limit" -- safe only because the tick
// loop bounds each on_tick() and stop() can always end it.
constexpr std::uint32_t ONESHOT_TIMEOUT_MS = 60000;

void run_task(void *arg)
{
	Args *a = static_cast<Args *>(arg);

	ESP_LOGI(TAG, "start '%s' (%s, %s)", a->name.c_str(),
	         a->mode == Mode::Behaviour ? "behaviour" : "one-shot",
	         a->why.c_str());

	wasm::set_params(a->params.empty() ? nullptr : a->params.c_str());

	const auto result = wasm::load_and_run(
		a->path.c_str(), "on_start",
		a->mode == Mode::Behaviour ? 0 : ONESHOT_TIMEOUT_MS);

	ESP_LOGI(TAG, "end   '%s' result=%d", a->name.c_str(), (int)result);

	// Publish the outcome BEFORE clearing `running`, so a status poll that
	// sees running=false is guaranteed to already see the matching result.
	s_last_name   = a->name;
	s_last_result = static_cast<int>(result);
	s_last_ms     = static_cast<std::uint32_t>((esp_timer_get_time() - s_started_us) / 1000);
	s_current.clear();
	s_started_by.clear();

	delete a;
	s_running = false;
	vTaskDelete(nullptr);
}

}  // namespace

bool start(const char *path, const char *name, const char *params,
           Mode mode, const char *why)
{
	if (!path || !*path) return false;

	// Exactly one at a time. Refused, never queued: a queue would mean the
	// robot performing a movement someone asked for long enough ago that they
	// have stopped expecting it.
	bool expected = false;
	if (!s_running.compare_exchange_strong(expected, true)) {
		ESP_LOGW(TAG, "refused '%s' (%s): '%s' is already running",
		         name ? name : "?", why ? why : "?", s_current.c_str());
		return false;
	}

	s_current    = name ? name : path;
	s_started_by = why ? why : "";
	s_mode       = mode;
	s_started_us = esp_timer_get_time();

	auto *args = new Args{ path, s_current, params ? params : "", mode, s_started_by };

	// Priority 4 and no core affinity, matching what the HTTP path used: the
	// scheduler keeps httpd responsive while the skill yields in its delays.
	if (xTaskCreate(run_task, "skill_run", 8192, args, 4, nullptr) != pdPASS) {
		ESP_LOGE(TAG, "could not create the skill task");
		delete args;
		s_current.clear();
		s_started_by.clear();
		s_running = false;
		return false;
	}
	return true;
}

void stop()
{
	if (!s_running.load()) return;
	ESP_LOGI(TAG, "stop requested for '%s'", s_current.c_str());
	wasm::request_stop();
}

bool running()                     { return s_running.load(); }
const std::string &current()       { return s_current; }
const std::string &started_by()    { return s_started_by; }
Mode current_mode()                { return s_mode; }
const std::string &last_name()     { return s_last_name; }
int last_result()                  { return s_last_result; }
std::uint32_t last_ms()            { return s_last_ms; }

std::uint32_t running_ms()
{
	if (!s_running.load()) return 0;
	return static_cast<std::uint32_t>((esp_timer_get_time() - s_started_us) / 1000);
}

std::string status_json()
{
	std::string out = "{\"running\":";
	out += s_running.load() ? "true" : "false";
	if (s_running.load()) {
		out += ",\"current\":\"" + s_current + "\"";
		out += ",\"mode\":\"";
		out += (s_mode == Mode::Behaviour ? "behaviour" : "oneshot");
		out += "\",\"started_by\":\"" + s_started_by + "\"";
		out += ",\"running_ms\":" + std::to_string(running_ms());
	}
	if (!s_last_name.empty()) {
		out += ",\"last\":{\"name\":\"" + s_last_name + "\",\"result\":"
		     + std::to_string(s_last_result) + ",\"ms\":"
		     + std::to_string(s_last_ms) + "}";
	}
	return out + "}";
}

}  // namespace skills
