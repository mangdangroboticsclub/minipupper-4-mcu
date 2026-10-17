#include "agent/agent_config.h"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "sdkconfig.h"

namespace agent_config {

namespace {

const char *TAG = "agent_config";

/* The mode stays where voice_glue kept it before Phase 5. */
constexpr const char *kModeNamespace = "voice";
constexpr const char *kModeKey       = "ai_mode";

/* The owner's own account. NVS keys are at most 15 characters. */
constexpr const char *kOwnNamespace  = "agent_llm";
constexpr const char *kKeyBackend    = "backend";
constexpr const char *kKeyBaseUrl    = "base_url";
constexpr const char *kKeyModel      = "model";
constexpr const char *kKeyAuthType   = "auth_type";
constexpr const char *kKeyMaxTokensF = "max_toks_f";
constexpr const char *kKeyApiKey     = "api_key";

constexpr const char *kDefaultAuthType       = "bearer";
constexpr const char *kDefaultMaxTokensField = "max_tokens";

/* Same limits as ESP-Claw's app_claw_config_t, so a value that fits here
 * fits there (APP_CLAW_STR_LEN 320, APP_CLAW_MODEL_LEN 64, SHORT 32). */
constexpr size_t kMaxStr   = 320;
constexpr size_t kMaxModel = 64;
constexpr size_t kMaxShort = 32;

bool s_mode_loaded = false;
Mode s_mode = Mode::MangDang;    /* zero setup is the default */

std::string nvs_read(nvs_handle_t h, const char *key, size_t max_len)
{
	size_t len = 0;
	if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0 || len > max_len) {
		return std::string();
	}
	std::string out(len, '\0');
	if (nvs_get_str(h, key, &out[0], &len) != ESP_OK) {
		return std::string();
	}
	out.resize(strnlen(out.c_str(), len));
	return out;
}

esp_err_t nvs_write(nvs_handle_t h, const char *key, const std::string &value, size_t max_len)
{
	if (value.size() >= max_len) {
		return ESP_ERR_INVALID_SIZE;
	}
	return nvs_set_str(h, key, value.c_str());
}

Mode load_mode()
{
	nvs_handle_t h;
	Mode m = Mode::MangDang;
	if (nvs_open(kModeNamespace, NVS_READONLY, &h) == ESP_OK) {
		const std::string text = nvs_read(h, kModeKey, 16);
		(void)parse_mode(text.c_str(), &m);
		nvs_close(h);
	}
	return m;
}

bool hex_to_key(const char *hex, uint8_t out[32])
{
	if (!hex || strlen(hex) != 64) {
		return false;
	}
	for (int i = 0; i < 32; ++i) {
		unsigned v;
		if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
			return false;
		}
		out[i] = static_cast<uint8_t>(v);
	}
	return true;
}

}  // namespace

const char *mode_name(Mode m)
{
	switch (m) {
	case Mode::Own: return "own";
	case Mode::Off: return "off";
	default:        return "mangdang";
	}
}

bool parse_mode(const char *name, Mode *out)
{
	if (!name || !out) {
		return false;
	}
	if (strcmp(name, "mangdang") == 0 || name[0] == '\0') {
		*out = Mode::MangDang;
	} else if (strcmp(name, "own") == 0) {
		*out = Mode::Own;
	} else if (strcmp(name, "off") == 0) {
		*out = Mode::Off;
	} else {
		return false;
	}
	return true;
}

Mode mode()
{
	if (!s_mode_loaded) {
		s_mode = load_mode();
		s_mode_loaded = true;
	}
	return s_mode;
}

esp_err_t save_mode(Mode m)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(kModeNamespace, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}
	err = nvs_set_str(h, kModeKey, mode_name(m));
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	nvs_close(h);
	if (err == ESP_OK) {
		s_mode = m;
		s_mode_loaded = true;
		ESP_LOGW(TAG, "AI mode -> %s", mode_name(m));
	}
	return err;
}

OwnLlm own()
{
	OwnLlm o;
	nvs_handle_t h;
	if (nvs_open(kOwnNamespace, NVS_READONLY, &h) == ESP_OK) {
		o.backend          = nvs_read(h, kKeyBackend, kMaxShort);
		o.base_url         = nvs_read(h, kKeyBaseUrl, kMaxStr);
		o.model            = nvs_read(h, kKeyModel, kMaxModel);
		o.auth_type        = nvs_read(h, kKeyAuthType, kMaxShort);
		o.max_tokens_field = nvs_read(h, kKeyMaxTokensF, kMaxShort);
		o.api_key          = nvs_read(h, kKeyApiKey, kMaxStr);
		nvs_close(h);
	}
	if (o.auth_type.empty()) {
		o.auth_type = kDefaultAuthType;
	}
	if (o.max_tokens_field.empty()) {
		o.max_tokens_field = kDefaultMaxTokensField;
	}
	return o;
}

bool own_complete(const OwnLlm &o)
{
	return !o.backend.empty() && !o.base_url.empty() && !o.model.empty() && !o.api_key.empty();
}

esp_err_t save_own(const OwnLlm &o)
{
	nvs_handle_t h;
	esp_err_t err = nvs_open(kOwnNamespace, NVS_READWRITE, &h);
	if (err != ESP_OK) {
		return err;
	}
	err = nvs_write(h, kKeyBackend, o.backend, kMaxShort);
	if (err == ESP_OK) err = nvs_write(h, kKeyBaseUrl, o.base_url, kMaxStr);
	if (err == ESP_OK) err = nvs_write(h, kKeyModel, o.model, kMaxModel);
	if (err == ESP_OK) {
		err = nvs_write(h, kKeyAuthType,
		                o.auth_type.empty() ? std::string(kDefaultAuthType) : o.auth_type, kMaxShort);
	}
	if (err == ESP_OK) {
		err = nvs_write(h, kKeyMaxTokensF,
		                o.max_tokens_field.empty() ? std::string(kDefaultMaxTokensField)
		                                           : o.max_tokens_field, kMaxShort);
	}
	if (err == ESP_OK && !o.api_key.empty()) {
		err = nvs_write(h, kKeyApiKey, o.api_key, kMaxStr);
	}
	if (err == ESP_OK) {
		err = nvs_commit(h);
	}
	nvs_close(h);
	if (err == ESP_OK) {
		ESP_LOGI(TAG, "own LLM saved: %s %s %s", o.backend.c_str(), o.base_url.c_str(), o.model.c_str());
	}
	return err;
}

std::string own_key_hint()
{
	const std::string key = own().api_key;
	if (key.size() < 4) {
		return std::string();
	}
	return std::string("...") + key.substr(key.size() - 4);
}

std::string robot_token()
{
	uint8_t key[32];
	uint8_t mac[32];
	if (!hex_to_key(CONFIG_APP_CHAT_AES_KEY_HEX, key)) {
		ESP_LOGW(TAG, "APP_CHAT_AES_KEY_HEX is not 64 hex digits: no gateway token");
		return std::string();
	}
	const std::string msg = std::string("mpx-ai-v1:") + CONFIG_APP_ROBOT_UUID;
	const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
	if (!md || mbedtls_md_hmac(md, key, sizeof(key),
	                           reinterpret_cast<const unsigned char *>(msg.data()), msg.size(),
	                           mac) != 0) {
		return std::string();
	}
	char hex[33];
	for (int i = 0; i < 16; ++i) {
		snprintf(hex + 2 * i, sizeof(hex) - 2 * i, "%02x", mac[i]);
	}
	return std::string(CONFIG_APP_ROBOT_UUID) + "." + hex;
}

}  // namespace agent_config
