#include "agent/agent_runtime.h"

#include "sdkconfig.h"

#if CONFIG_APP_AGENT_ENABLE

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "esp_log.h"

#include "agent/agent_caps.h"
#include "agent/agent_config.h"
#include "agent/agent_fs.h"
#include "app_claw.h"
#include "cap_im_local.h"

static const char *TAG = "agent";

namespace {

/* The PWA's chat is ESP-Claw's "web" channel (app_claw binds it to
 * local_send_message), one chat per conversation uuid. */
constexpr const char *kChannel  = "web";
constexpr const char *kSenderId = "pwa_user";

/* ESP-Claw marks progress notes ("Round 2: robot_move(...)") and the
 * router's working reply with the lobster, U+1F99E. */
constexpr const char kStepMark[] = "\xF0\x9F\xA6\x9E";

/* mpv2-claw's app_config defaults. */
constexpr const char *kTimeoutMs       = "120000";
constexpr const char *kMaxTokens       = "8192";
constexpr const char *kImageMaxBytes   = "524288";
constexpr const char *kGatewayBackend  = "openai_compatible";
constexpr const char *kGatewayModel    = "mangdang";
constexpr const char *kGatewayAuth     = "bearer";
constexpr const char *kGatewayTokField = "max_tokens";

bool s_running = false;
agent::OutboundFn s_outbound = nullptr;

void copy_field(char *dst, size_t dst_size, const char *src)
{
	snprintf(dst, dst_size, "%s", src ? src : "");
}

/* Project agent_config into ESP-Claw's config. Nothing the gateway owns is
 * written to NVS: the token is derived every time. */
void fill_config(app_claw_config_t *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	copy_field(cfg->llm_timeout_ms, sizeof(cfg->llm_timeout_ms), kTimeoutMs);
	copy_field(cfg->llm_max_tokens, sizeof(cfg->llm_max_tokens), kMaxTokens);
	copy_field(cfg->llm_default_image_max_bytes, sizeof(cfg->llm_default_image_max_bytes), kImageMaxBytes);
	copy_field(cfg->llm_supports_tools, sizeof(cfg->llm_supports_tools), "true");
	copy_field(cfg->llm_supports_vision, sizeof(cfg->llm_supports_vision), "false");
	copy_field(cfg->llm_image_remote_url_only, sizeof(cfg->llm_image_remote_url_only), "false");
	/* Empty = every compiled capability group enabled and visible. */

	switch (agent_config::mode()) {
	case agent_config::Mode::MangDang: {
		const std::string token = agent_config::robot_token();
		if (token.empty() || !CONFIG_APP_AGENT_GATEWAY_LLM_URL[0]) {
			ESP_LOGW(TAG, "no gateway token or URL: the agent has no model");
			break;
		}
		copy_field(cfg->llm_api_key, sizeof(cfg->llm_api_key), token.c_str());
		copy_field(cfg->llm_backend_type, sizeof(cfg->llm_backend_type), kGatewayBackend);
		copy_field(cfg->llm_base_url, sizeof(cfg->llm_base_url), CONFIG_APP_AGENT_GATEWAY_LLM_URL);
		copy_field(cfg->llm_model, sizeof(cfg->llm_model), kGatewayModel);
		copy_field(cfg->llm_auth_type, sizeof(cfg->llm_auth_type), kGatewayAuth);
		copy_field(cfg->llm_max_tokens_field, sizeof(cfg->llm_max_tokens_field), kGatewayTokField);
		ESP_LOGI(TAG, "AI mode mangdang: the model is the MangDang gateway %s",
		         CONFIG_APP_AGENT_GATEWAY_LLM_URL);
		break;
	}
	case agent_config::Mode::Own: {
		const agent_config::OwnLlm own = agent_config::own();
		if (!agent_config::own_complete(own)) {
			ESP_LOGW(TAG, "AI mode own, but the account is incomplete: the agent has no model");
			break;
		}
		copy_field(cfg->llm_api_key, sizeof(cfg->llm_api_key), own.api_key.c_str());
		copy_field(cfg->llm_backend_type, sizeof(cfg->llm_backend_type), own.backend.c_str());
		copy_field(cfg->llm_base_url, sizeof(cfg->llm_base_url), own.base_url.c_str());
		copy_field(cfg->llm_model, sizeof(cfg->llm_model), own.model.c_str());
		copy_field(cfg->llm_auth_type, sizeof(cfg->llm_auth_type), own.auth_type.c_str());
		copy_field(cfg->llm_max_tokens_field, sizeof(cfg->llm_max_tokens_field), own.max_tokens_field.c_str());
		ESP_LOGI(TAG, "AI mode own: %s %s (%s)", own.backend.c_str(), own.model.c_str(),
		         own.base_url.c_str());
		break;
	}
	case agent_config::Mode::Off:
	default:
		/* Nothing the agent could send a request with. submit() refuses
		 * before it gets that far. */
		ESP_LOGI(TAG, "AI mode off: the agent has no model");
		break;
	}
}

/* ESP-Claw's own `/llm set ...` chat command and llm_config_command cap end
 * in app_claw_apply_config(), which calls this. What the owner typed there
 * is their own account: keep it as "own". The gateway's values are never
 * stored (a `/llm show` on the mangdang mode followed by apply would
 * otherwise freeze today's token into NVS). */
esp_err_t save_config(const app_claw_config_t *config, void *)
{
	if (!config) {
		return ESP_ERR_INVALID_ARG;
	}
	if (strcmp(config->llm_base_url, CONFIG_APP_AGENT_GATEWAY_LLM_URL) == 0) {
		return ESP_OK;
	}
	if (!config->llm_base_url[0] || !config->llm_api_key[0]) {
		return ESP_OK;
	}
	agent_config::OwnLlm own;
	own.backend          = config->llm_backend_type;
	own.base_url         = config->llm_base_url;
	own.model            = config->llm_model;
	own.auth_type        = config->llm_auth_type;
	own.max_tokens_field = config->llm_max_tokens_field;
	own.api_key          = config->llm_api_key;
	esp_err_t err = agent_config::save_own(own);
	if (err == ESP_OK && agent_config::mode() != agent_config::Mode::Own) {
		err = agent_config::save_mode(agent_config::Mode::Own);
	}
	return err;
}

/* The one outbound slot of cap_im_local: every message the agent (or a
 * router rule) sends to any "web" chat lands here. */
esp_err_t on_outbound(const cap_im_local_message_t *msg, void *)
{
	if (!msg || !msg->text || !msg->chat_id) {
		return ESP_ERR_INVALID_ARG;
	}
	if (msg->channel && strcmp(msg->channel, kChannel) != 0) {
		return ESP_OK;
	}
	const char *text = msg->text;
	bool is_step = false;
	if (strncmp(text, kStepMark, sizeof(kStepMark) - 1) == 0) {
		is_step = true;
		text += sizeof(kStepMark) - 1;
		while (*text == ' ') {
			text++;
		}
	}
	if (s_outbound) {
		s_outbound(msg->chat_id, text, is_step);
	} else {
		ESP_LOGI(TAG, "[%s] %s%s", msg->chat_id, is_step ? "step: " : "", text);
	}
	return ESP_OK;
}

}  // namespace

namespace agent {

bool available()
{
	return true;
}

bool running()
{
	return s_running;
}

esp_err_t start()
{
	if (s_running) {
		return ESP_OK;
	}
	esp_err_t err = agent_fs::init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "filesystem roots not ready (%s): the agent is not started", esp_err_to_name(err));
		return err;
	}

	/* Before app_claw_start(): the groups register inside app_capabilities_init(). */
	(void)agent_caps::register_groups();

	app_claw_set_save_config_callback(save_config, nullptr);

	/* 4 KB of config: not on the main task's stack. */
	app_claw_config_t *cfg = static_cast<app_claw_config_t *>(calloc(1, sizeof(app_claw_config_t)));
	if (!cfg) {
		return ESP_ERR_NO_MEM;
	}
	fill_config(cfg);
	err = app_claw_start(cfg);
	free(cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "app_claw_start: %s", esp_err_to_name(err));
		return err;
	}

	err = cap_im_local_set_outbound_callback(on_outbound, nullptr);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "web channel outbound not bound: %s -- replies will only be logged",
		         esp_err_to_name(err));
	}
	s_running = true;
	ESP_LOGI(TAG, "on-device agent running (mode %s)", agent_config::mode_name(agent_config::mode()));
	return ESP_OK;
}

esp_err_t apply_config()
{
	if (!s_running) {
		return ESP_ERR_INVALID_STATE;
	}
	app_claw_config_t *cfg = static_cast<app_claw_config_t *>(calloc(1, sizeof(app_claw_config_t)));
	if (!cfg) {
		return ESP_ERR_NO_MEM;
	}
	fill_config(cfg);
	const esp_err_t err = app_claw_update_config(cfg);
	free(cfg);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "app_claw_update_config: %s", esp_err_to_name(err));
	}
	return err;
}

esp_err_t submit(const char *session_id, const char *text)
{
	if (!session_id || !session_id[0] || !text || !text[0]) {
		return ESP_ERR_INVALID_ARG;
	}
	if (!s_running || agent_config::mode() == agent_config::Mode::Off) {
		return ESP_ERR_INVALID_STATE;
	}
	return cap_im_local_emit_user_message(kChannel, session_id, kSenderId, nullptr, text, nullptr, 0);
}

void set_outbound(OutboundFn fn)
{
	s_outbound = fn;
}

}  // namespace agent

#else  /* !CONFIG_APP_AGENT_ENABLE */

namespace agent {

bool available() { return false; }
bool running() { return false; }
esp_err_t start() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t apply_config() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t submit(const char *, const char *) { return ESP_ERR_INVALID_STATE; }
void set_outbound(OutboundFn) {}

}  // namespace agent

#endif  /* CONFIG_APP_AGENT_ENABLE */
