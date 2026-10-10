#include <cstdint>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "audio/mpx_audio.h"
#include "display/display_glue.h"
#include "fs/littlefs_manager.h"
#include "lua/lua_vm.h"
#include "network/chat_ws.h"
#include "network/http_server.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"
#include "network/www_assets.h"
#include "pi/pi_link.h"
#include "robot/mpx_robot.h"
#include "robot/robot.h"
#include "sdk/wasm_host_functions.h"
#include "voice/voice_glue.h"
#include "wasm/registry.h"
#include "wasm/wasm_sandbox.h"

static const char *TAG = "main";

/* ── The robot's view of the WASM skill runtime ───────────────
 * robot.cc (mpv2's) does not include the sandbox; it asks these three
 * questions through mpx_robot_set_skill_hooks() so the gait loop knows when
 * a skill owns the servos and so the Pi link can stop one when it takes
 * control. The struct must outlive the registration: it is static. */
static const mpx_robot_skill_hooks_t s_skill_hooks = {
	.skill_is_running = [] { return wasm::is_running(); },
	.skill_owns_pose  = [] { return sdk::control_owner_is_pose(); },
	.request_stop     = [] { wasm::request_stop(); },
};

/* The Raspberry Pi took (true) or released (false) the robot. Runs on the
 * Pi link task: nothing here may block. The screen's "ROS" badge goes
 * through display_glue's queue; voice and music off (and hands-free back
 * when the Pi lets go) through voice_glue's. */
static void on_master_changed(bool active)
{
	ESP_LOGW(TAG, "Raspberry Pi %s control of the robot",
	         active ? "took" : "released");
	display_glue::on_master_changed(active);
	voice_glue::on_master_changed(active);
}

/* Internal RAM after each boot stage. The Wi-Fi driver needs internal RAM
 * for every packet; when this runs out the AP stays up but phones time out. */
static void log_internal_heap(const char *stage)
{
	ESP_LOGI(TAG, "internal RAM after %s: %u free, %u largest", stage,
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void init_nvs()
{
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
}

extern "C" void app_main(void)
{
	init_nvs();
	log_internal_heap("boot");

	// ── Robot HAL (servo bus, gait task on Core 1) ─────────────
	mpx_robot_set_skill_hooks(&s_skill_hooks);
	robot::set_master_observer(on_master_changed);
	if (!robot::init()) {
		ESP_LOGE(TAG, "Robot HAL init failed — continuing without servo control");
	}

	// ── Screen: ST7789 + LVGL, the face ───────────────────────
	// Straight after the robot (the eyes follow the IMU) and before
	// anything slow: the face is the first thing on the panel, and the
	// backlight only comes on once it is drawn (no white flash).
	display_glue::start_face();
	log_internal_heap("robot + screen");

	// ── Audio: microphones + speaker ──────────────────────────
	// After the robot (the IMU and the ES7210 share the I2C bus; the IMU
	// claims it first) and before the Pi link, which plays the Pi's music.
	if (mpx_audio_init() != ESP_OK) {
		ESP_LOGW(TAG, "Audio init failed — no microphone, no speaker");
	}

	// ── Raspberry Pi (ROS 2) link on J-PI ─────────────────────
	// Straight after the robot: a Pi that takes control must always be
	// able to lose it, so the link must not wait on the network.
#if CONFIG_APP_PI_LINK_ENABLE
	if (pi_link::init() != ESP_OK) {
		ESP_LOGW(TAG, "Pi link init failed — continuing standalone");
	}
#endif
	log_internal_heap("audio + Pi link");

	// Mount LittleFS (for .wasm storage and PWA assets)
	if (!fs::init_littlefs()) {
		ESP_LOGE(TAG, "LittleFS mount failed — wasm sandbox unavailable");
		return;
	}

	// ── Screen: the Display page's GIFs live on LittleFS ──────
	display_glue::start_animations();

	// ── Network: start Wi-Fi AP (Core 0, Priority 6) ──────────
	if (!network::init_wifi_ap()) {
		ESP_LOGE(TAG, "Wi-Fi AP init failed");
		return;
	}

	// ── Network: initialise Wi-Fi STA (auto-connects if saved) ─
	if (!network::init_wifi_sta()) {
		ESP_LOGW(TAG, "Wi-Fi STA init failed — continuing in AP-only mode");
	}

	// ── Screen: network line under the eyes, Pi address, ROS badge ─
	display_glue::start_events();
	log_internal_heap("Wi-Fi");

	// ── Deploy embedded PWA assets to LittleFS ────────────────
	if (!network::deploy_www_assets()) {
		ESP_LOGE(TAG, "PWA asset deployment failed");
		return;
	}

	// ── Skill registry: what the installed .wasm files declare ─
	// Before the HTTP server, so /v1/robot/moves and /v1/skills/registry
	// are complete from the first request. Rescanned on upload and delete.
	skills::rescan();

	// ── Start HTTP + WebSocket server (Core 0, Priority 6) ───
	if (!network::start_http_server()) {
		ESP_LOGE(TAG, "HTTP server init failed");
		return;
	}

	// ── Chat pipeline (echo / encrypted cloud proxy) ─────────
	if (!network::init_chat_pipeline()) {
		ESP_LOGW(TAG, "Chat pipeline init failed — chat UI unavailable");
	}

	// ── Wasm sandbox ──────────────────────────────────────────
	if (!wasm::init_sandbox()) {
		ESP_LOGE(TAG, "WAMR sandbox init failed");
		return;
	}

	// ── Lua scripting VM ──────────────────────────────────────
	if (lua_init() != ESP_OK) {
		ESP_LOGE(TAG, "Lua VM init failed — continuing without Lua scripting");
	} else {
		ESP_LOGI(TAG, "Lua VM ready — robot bindings registered");
	}
	log_internal_heap("HTTP + chat + WASM + Lua");

	// ── Voice: wake word, the MangDang voice gateway, music ───
	// Last, as in mpv2-claw: esp-sr's front end is the biggest consumer of
	// internal RAM to come up, and the voice tools start skills through the
	// HTTP server's helpers and the sandbox, which both exist by now. Nothing
	// touches the network until the first wake word.
	voice_glue::start();
}