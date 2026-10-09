#include <cstdint>

#include "esp_log.h"
#include "nvs_flash.h"

#include "fs/littlefs_manager.h"
#include "lua/lua_vm.h"
#include "network/chat_ws.h"
#include "network/http_server.h"
#include "network/wifi_ap.h"
#include "network/wifi_sta.h"
#include "network/www_assets.h"
#include "robot/mpx_robot.h"
#include "robot/robot.h"
#include "sdk/wasm_host_functions.h"
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

	// ── Robot HAL (servo bus, gait task on Core 1) ─────────────
	mpx_robot_set_skill_hooks(&s_skill_hooks);
	if (!robot::init()) {
		ESP_LOGE(TAG, "Robot HAL init failed — continuing without servo control");
	}

	// Mount LittleFS (for .wasm storage and PWA assets)
	if (!fs::init_littlefs()) {
		ESP_LOGE(TAG, "LittleFS mount failed — wasm sandbox unavailable");
		return;
	}

	// ── Network: start Wi-Fi AP (Core 0, Priority 6) ──────────
	if (!network::init_wifi_ap()) {
		ESP_LOGE(TAG, "Wi-Fi AP init failed");
		return;
	}

	// ── Network: initialise Wi-Fi STA (auto-connects if saved) ─
	if (!network::init_wifi_sta()) {
		ESP_LOGW(TAG, "Wi-Fi STA init failed — continuing in AP-only mode");
	}

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
}