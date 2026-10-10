#pragma once

/*
 * agent_config -- the owner's "AI & Voice" choice and the LLM behind it.
 *
 * mpv2-claw keeps this in app_config (NVS, projected into ESP-Claw's
 * app_claw_config_t at boot, restart to apply). Here it is one small module
 * that both the voice stack (voice_glue) and the on-device agent
 * (agent_runtime) read, applied live:
 *
 *   mangdang   zero setup. Voice goes to the MangDang voice gateway; the
 *              agent's model is the gateway's OpenAI-compatible endpoint
 *              (CONFIG_APP_AGENT_GATEWAY_LLM_URL), authenticated with a
 *              token derived from this robot's AES key -- no provider key
 *              on the robot, nobody types one.
 *   own        the owner's own LLM account (backend, URL, model, key),
 *              entered in Settings > AI & Voice (POST /v1/ai) and stored in
 *              NVS. The agent uses it; voice still goes through the gateway
 *              for now (the voice -> local agent handoff is later work).
 *   off        no AI: voice off, the microphone never opens, the agent
 *              refuses chat.
 *
 * The mode lives under the NVS key voice_glue used before Phase 5
 * ("voice"/"ai_mode"), so a robot that was set to "off" stays off.
 */

#include <string>

#include "esp_err.h"

namespace agent_config {

enum class Mode { MangDang, Own, Off };

const char *mode_name(Mode m);                      /*!< "mangdang" | "own" | "off" */
bool        parse_mode(const char *name, Mode *out);

/** The saved mode (read from NVS once, then cached). */
Mode mode();

/** Save a new mode. Does not apply it: /v1/ai tells voice_glue and the
 *  agent afterwards, in that order. */
esp_err_t save_mode(Mode m);

/** The owner's own LLM account, as stored. api_key is the real key: never
 *  echo it over HTTP (use key_set/key_hint). */
struct OwnLlm {
	std::string backend;            /*!< "openai_compatible" | "anthropic_compatible" */
	std::string base_url;           /*!< "https://api.example.com/v1" */
	std::string model;
	std::string auth_type;          /*!< "bearer" | "none" (Anthropic's x-api-key) */
	std::string max_tokens_field;   /*!< "max_tokens" | "max_completion_tokens" */
	std::string api_key;
};

OwnLlm own();

/** Everything "own" needs to send a request: backend, URL, model, key. */
bool own_complete(const OwnLlm &o);

/** Store the owner's account. An empty api_key keeps the stored one;
 *  empty auth_type / max_tokens_field take the usual defaults. */
esp_err_t save_own(const OwnLlm &o);

/** The last four characters of the stored key, for the settings page. */
std::string own_key_hint();

/** "<robot uuid>.<first 32 hex of HMAC-SHA256(aes_key, "mpx-ai-v1:" + uuid)>",
 *  the gateway's robot token (CONFIG_APP_ROBOT_UUID, CONFIG_APP_CHAT_AES_KEY_HEX).
 *  Empty when the key is not 64 hex digits. */
std::string robot_token();

}  // namespace agent_config
