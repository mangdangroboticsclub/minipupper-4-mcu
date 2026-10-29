#include "mpx_identity.h"

#include "mpx_derive.h"
#include "mpx_efuse.h"
#include "mpx_id.h"

#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "sdkconfig.h"

/* A Secure Boot image is signed against a key that only the eFuse source can
 * authenticate, so a Kconfig identity there is either a mistake or a downgrade
 * attack. Fail the build rather than boot it. */
#if CONFIG_SECURE_BOOT && !CONFIG_APP_IDENTITY_SOURCE_EFUSE
#error "Secure Boot builds must use the eFuse identity source"
#endif

/* With Secure Boot v2 the eFuse blocks are read-protected before ours can be
 * burned, so the HMAC_UP block needs the release path that keeps it writable. */
#if CONFIG_APP_IDENTITY_SOURCE_EFUSE && CONFIG_SECURE_BOOT && \
    !CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS
#error "HMAC_UP is burned after Secure Boot: enable SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS"
#endif

static const char *TAG = "mpx_identity";

namespace {

mpx_id_state_t s_state = MPX_ID_UNPROVISIONED;
char s_robot_id[MPX_ID_LEN + 1];          /* "" or 1..16 characters */
uint8_t s_key[MPX_KEY_LEN];
bool s_key_loaded = false;
uint32_t s_kv = 1;
char s_key_fp[MPX_KEY_FP_LEN + 1] = {};
uint16_t s_sec_flags = 0;

/* NVS records the key version a unit is on, so a rotation can be applied
 * without touching the eFuse record (§6.9 C3, D13). The namespace is created by
 * the provisioning station; a dev board simply has none. */
const char *const kNvsNamespace = "mpx_id";
const char *const kNvsKeyVersion = "kv";

/* The value main/Kconfig.projbuild ships as APP_CHAT_AES_KEY_HEX. It is in
 * every firmware image and every clone of this repo, which is why it may never
 * reach a robot that talks to a real gateway: the log line below is the only
 * thing standing between a bench build and that mistake. */
const uint8_t kPublicDevKey[MPX_KEY_LEN] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
};

const char *state_name(mpx_id_state_t state)
{
    switch (state) {
    case MPX_ID_KCONFIG:      return "kconfig";
    case MPX_ID_EFUSE:        return "efuse";
    case MPX_ID_INCONSISTENT: return "inconsistent";
    case MPX_ID_UNPROVISIONED:
    default:                  return "unprovisioned";
    }
}

int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parse_hex_key(const char *hex, uint8_t out[MPX_KEY_LEN])
{
    if (hex == nullptr || std::strlen(hex) != 2 * (size_t)MPX_KEY_LEN) {
        return false;
    }
    for (size_t i = 0; i < (size_t)MPX_KEY_LEN; i++) {
        const int hi = hex_nibble(hex[2 * i]);
        const int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

bool all_zero(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (buf[i] != 0) {
            return false;
        }
    }
    return true;
}

/* HMAC-SHA256 in software. On a provisioned unit the equivalent call is
 * `esp_hmac_calculate`, which runs the same HMAC inside the peripheral so the
 * secret never enters RAM; here the key is already in RAM, coming from Kconfig
 * or from a host test vector. */
int hmac_sha256_sw(const uint8_t *key, size_t key_len,
                   const uint8_t *msg, size_t msg_len,
                   uint8_t out[MPX_KEY_LEN])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    if (md == nullptr || key_len > 32) {
        return -1;
    }
    /* mbedtls takes the key length as an unsigned char and truncates silently
     * above 255, so bound the only call we ever make: a 32-byte secret. */
    if (mbedtls_md_hmac(md, key, key_len, msg, msg_len, out) != 0) {
        return -1;
    }
    return 0;
}

bool read_nvs_kv(uint32_t *out)
{
    nvs_handle_t handle;
    esp_err_t err;
    uint32_t value = 0;

    if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    err = nvs_get_u32(handle, kNvsKeyVersion, &value);
    nvs_close(handle);
    if (err != ESP_OK) {
        return false;
    }
    *out = value;
    return true;
}

void log_identity()
{
    /* id and key_fp are safe to log: the fingerprint is a one-way HMAC over the
     * key, and support only ever needs "is this the key I think it is". */
    ESP_LOGI(TAG, "identity: state=%s id=\"%s\" kv=%u key_fp=%s",
             state_name(s_state), s_robot_id, (unsigned)s_kv, s_key_fp);
}

#if CONFIG_APP_IDENTITY_SOURCE_EFUSE

esp_err_t load_efuse_identity()
{
    mpx_efuse_record_t rec;
    uint8_t block = 0;
    esp_err_t err;

    err = mpx_efuse_read_record(&rec);
    if (err != ESP_OK) {
        /* A blank block is the normal state of a board that has not been to the
         * bench yet: boot, walk, stay off the cloud. It is not an error worth
         * failing startup for. */
        ESP_LOGW(TAG, "identity: no eFuse record -- this unit is not provisioned");
        s_state = MPX_ID_UNPROVISIONED;
        return ESP_ERR_INVALID_STATE;
    }
    if (!mpx_efuse_find_hmac_key(&block)) {
        /* The ID is burned but S is not, so there is no key and no way to make
         * one: the unit needs a `provision` run, not a different key version.
         * Reported separately because it is a bench mistake, not a field one. */
        ESP_LOGE(TAG, "identity: record \"%s\" but no HMAC_UP key -- "
                      "re-run provisioning", rec.robot_id);
        s_state = MPX_ID_INCONSISTENT;
        return ESP_ERR_INVALID_STATE;
    }

    std::snprintf(s_robot_id, sizeof(s_robot_id), "%s", rec.robot_id);
    s_kv = rec.kv_base;
    s_sec_flags = mpx_efuse_sec_flags();

    /* A rotation is applied by raising the version in NVS, never by rewriting
     * the record: the record's WR_DIS bit is set at provisioning time. The
     * higher of the two wins, so a rolled-back NVS cannot un-rotate a key. */
    uint32_t stored = 0;
    if (read_nvs_kv(&stored) && stored > s_kv) {
        s_kv = stored;
    }

    /* K_kv comes out of the HMAC peripheral: `S` is in an RD_DIS block that no
     * code can read, and the HMAC adapter ignores the secret argument entirely
     * for that reason. */
    if (mpx_derive_key(mpx_efuse_hmac_sha256, nullptr, 0, s_robot_id, s_kv,
                       s_key) != 0) {
        ESP_LOGE(TAG, "identity: deriving K_%u failed", (unsigned)s_kv);
        std::memset(s_key, 0, sizeof(s_key));
        s_state = MPX_ID_INCONSISTENT;
        return ESP_ERR_INVALID_STATE;
    }
    /* The fingerprint is an HMAC *over* K_kv, so it needs the software
     * primitive: routing it through the eFuse adapter would HMAC with S again
     * and produce a value that does not match the station's. */
    if (mpx_derive_key_fp(hmac_sha256_sw, s_key, s_key_fp) != 0) {
        ESP_LOGE(TAG, "identity: could not compute the key fingerprint");
        std::memset(s_key, 0, sizeof(s_key));
        s_state = MPX_ID_INCONSISTENT;
        return ESP_ERR_INVALID_STATE;
    }

    s_key_loaded = true;
    s_state = MPX_ID_EFUSE;
    log_identity();
    return ESP_OK;
}

#else

esp_err_t load_kconfig_identity()
{
    const char *id = CONFIG_APP_ROBOT_UUID;

    if (!mpx_id_valid_or_legacy(id)) {
        ESP_LOGE(TAG, "identity: APP_ROBOT_UUID \"%s\" is not a robot ID "
                      "(1..16 printable ASCII bytes)", id);
        s_state = MPX_ID_UNPROVISIONED;
        return ESP_ERR_INVALID_STATE;
    }
    /* Not an error: main/Kconfig.projbuild has defaulted to this since before
     * the format existed, and a dev board that stops booting because its ID is
     * short would be a worse trade. */
    if (!mpx_id_valid(id)) {
        ESP_LOGW(TAG, "identity: legacy development ID \"%s\" -- it travels "
                      "zero-padded in the frame header", id);
    }
    std::snprintf(s_robot_id, sizeof(s_robot_id), "%s", id);

    if (!parse_hex_key(CONFIG_APP_CHAT_AES_KEY_HEX, s_key)) {
        ESP_LOGE(TAG, "identity: APP_CHAT_AES_KEY_HEX is not 64 hex characters");
        s_state = MPX_ID_UNPROVISIONED;
        return ESP_ERR_INVALID_STATE;
    }
    /* A zero key is what an unprovisioned NVS read would produce, and it is
     * never a usable secret. Treat it as no identity rather than as a key that
     * happens to decrypt nothing. */
    if (all_zero(s_key, sizeof(s_key))) {
        ESP_LOGE(TAG, "identity: APP_CHAT_AES_KEY_HEX is all zeros -- provision a "
                      "key or set it in menuconfig");
        s_state = MPX_ID_UNPROVISIONED;
        return ESP_ERR_INVALID_STATE;
    }
    if (std::memcmp(s_key, kPublicDevKey, sizeof(s_key)) == 0) {
        ESP_LOGE(TAG, "identity: this is the PUBLIC development key from the "
                      "repository -- fine on a bench, never against a real gateway");
    }

    /* Dev boards start at kv 1, but honour a taller value if a station ever
     * recorded one, so rotating a key on a bench unit does not need a reflash. */
    uint32_t stored = 0;
    if (read_nvs_kv(&stored) && stored > s_kv) {
        s_kv = stored;
    }

    if (mpx_derive_key_fp(hmac_sha256_sw, s_key, s_key_fp) != 0) {
        ESP_LOGE(TAG, "identity: could not compute the key fingerprint");
        std::memset(s_key, 0, sizeof(s_key));
        s_state = MPX_ID_UNPROVISIONED;
        return ESP_ERR_INVALID_STATE;
    }

    s_key_loaded = true;
    s_state = MPX_ID_KCONFIG;
    log_identity();
    return ESP_OK;
}

#endif /* CONFIG_APP_IDENTITY_SOURCE_EFUSE */

}  // namespace

esp_err_t mpx_identity_init(void)
{
    std::memset(s_robot_id, 0, sizeof(s_robot_id));
    std::memset(s_key, 0, sizeof(s_key));
    std::memset(s_key_fp, 0, sizeof(s_key_fp));
    s_key_loaded = false;
    s_kv = 1;
    s_sec_flags = 0;
    s_state = MPX_ID_UNPROVISIONED;

#if CONFIG_APP_IDENTITY_SOURCE_EFUSE
    return load_efuse_identity();
#else
    return load_kconfig_identity();
#endif
}

mpx_id_state_t mpx_identity_state(void)
{
    return s_state;
}

const char *mpx_identity_robot_id(void)
{
    return s_robot_id;
}

bool mpx_identity_robot_key(uint8_t out[MPX_KEY_LEN])
{
    if (out == nullptr || !s_key_loaded) {
        return false;
    }
    std::memcpy(out, s_key, MPX_KEY_LEN);
    return true;
}

bool mpx_identity_frame_id(uint8_t out[MPX_ID_LEN])
{
    if (out == nullptr || !s_key_loaded) {
        return false;
    }
    /* s_robot_id was validated at init, so this cannot fail on a loaded
     * identity; the return value is passed on rather than asserted because a
     * caller that ignores it would send an unauthenticated frame. */
    return mpx_id_to_frame(out, s_robot_id);
}

uint32_t mpx_identity_key_version(void)
{
    return s_kv;
}

const char *mpx_identity_key_fp(void)
{
    return s_key_fp;
}

uint16_t mpx_identity_sec_flags(void)
{
    return s_sec_flags;
}
