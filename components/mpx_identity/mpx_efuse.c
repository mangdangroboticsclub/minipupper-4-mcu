#include "mpx_efuse.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_efuse.h"
/* The built-in field descriptors (WR_DIS_*, SECURE_BOOT_EN, SPI_BOOT_CRYPT_CNT,
 * DIS_PAD_JTAG, ...) live in the chip table header, which esp_efuse.h does not
 * pull in. */
#include "esp_efuse_table.h"
#include "esp_hmac.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mpx_identity.h"
#include "mpx_seal.h"

/* Generated from main/esp_efuse_custom_table.csv by the efuse component into
 * main/include/esp_efuse_custom_table.h and compiled into the app there. The
 * declarations are repeated here rather than including that header because an
 * ESP-IDF component does not get another component's generated include dir; a
 * mismatch is a link error, not a silent one. */
extern const esp_efuse_desc_t *ESP_EFUSE_USER_DATA_MPX_ROBOT_ID[];
extern const esp_efuse_desc_t *ESP_EFUSE_USER_DATA_MPX_ID_FORMAT[];
extern const esp_efuse_desc_t *ESP_EFUSE_USER_DATA_MPX_ID_FLAGS[];
extern const esp_efuse_desc_t *ESP_EFUSE_USER_DATA_MPX_KV_BASE[];

static const char *TAG = "mpx_efuse";

/* The record's 20 bytes live in four eFuse fields, not one: the field table in
 * main/esp_efuse_custom_table.csv splits them so `idf.py show-efuse-table`
 * prints something readable and so the reserved bytes stay zero. The packing
 * itself is mpx_efuse_record_encode()'s job -- this file only moves blobs. */
static const esp_efuse_desc_t **const kIdField    = ESP_EFUSE_USER_DATA_MPX_ROBOT_ID;
static const esp_efuse_desc_t **const kFormatField = ESP_EFUSE_USER_DATA_MPX_ID_FORMAT;
static const esp_efuse_desc_t **const kFlagsField  = ESP_EFUSE_USER_DATA_MPX_ID_FLAGS;
static const esp_efuse_desc_t **const kKvField     = ESP_EFUSE_USER_DATA_MPX_KV_BASE;

esp_err_t mpx_efuse_read_record(mpx_efuse_record_t *rec)
{
    uint8_t raw[MPX_EFUSE_RECORD_LEN];
    esp_err_t err;

    if (rec == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(raw, 0, sizeof raw);

    err = esp_efuse_read_field_blob(kIdField, raw + MPX_EFUSE_ID_OFF,
                                    MPX_ID_LEN * 8);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_efuse_read_field_blob(kFormatField, raw + MPX_EFUSE_FORMAT_OFF, 8);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_efuse_read_field_blob(kFlagsField, raw + MPX_EFUSE_FLAGS_OFF, 8);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_efuse_read_field_blob(kKvField, raw + MPX_EFUSE_KVBASE_OFF, 16);
    if (err != ESP_OK) {
        return err;
    }

    /* A blank block reads as zeros, which decode() rejects -- so "not
     * provisioned" arrives as ESP_ERR_NOT_FOUND instead of as a record with an
     * empty ID that would later derive a key from an empty string. */
    if (mpx_efuse_record_decode(rec, raw) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

bool mpx_efuse_find_hmac_key(uint8_t *block_out)
{
    esp_efuse_block_t block = EFUSE_BLK_KEY0;
    hmac_key_id_t id = HMAC_KEY0;
    bool found = false;

    /* Find the key by purpose rather than assuming BLOCK_KEY0: the station
     * picks the first unused block, and a board that already burned a DS key or
     * a device key will not have the HMAC key in slot 0. */
    for (esp_efuse_block_t b = EFUSE_BLK_KEY0; b < EFUSE_BLK_KEY_MAX; b++) {
        if (esp_efuse_get_key_purpose(b) == ESP_EFUSE_KEY_PURPOSE_HMAC_UP) {
            block = b;
            id = (hmac_key_id_t)(HMAC_KEY0 + (int)(b - EFUSE_BLK_KEY0));
            found = true;
            break;
        }
    }

    if (found && block_out != NULL) {
        *block_out = (uint8_t)id;
    }
    return found;
}

esp_err_t mpx_efuse_burn_record(const mpx_efuse_record_t *rec,
                                const uint8_t secret[32],
                                uint8_t *key_block_out)
{
    uint8_t raw[MPX_EFUSE_RECORD_LEN];
    esp_efuse_block_t key_block = EFUSE_BLK_KEY0;
    esp_err_t err;

    if (rec == NULL || secret == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (mpx_efuse_record_encode(raw, rec) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Refuse to touch a board that already has a record. Re-burning the ID would
     * leave the old HMAC key in place, producing a robot whose ID and key
     * disagree -- exactly the state the cloud reports as a corrupt frame. */
    mpx_efuse_record_t existing;
    if (mpx_efuse_read_record(&existing) == ESP_OK) {
        ESP_LOGE(TAG, "burn refused: board already carries %s", existing.robot_id);
        return ESP_ERR_INVALID_STATE;
    }

    /* The HMAC key has to go into an unused block whose purpose field is still
     * writable; anything else would silently reuse a DS or device key slot. */
    bool have_slot = false;

    for (esp_efuse_block_t candidate = EFUSE_BLK_KEY0;
         candidate < EFUSE_BLK_KEY_MAX; candidate++) {
        if (esp_efuse_key_block_unused(candidate) &&
            !esp_efuse_get_keypurpose_dis_write(candidate)) {
            key_block = candidate;
            have_slot = true;
            break;
        }
    }
    if (!have_slot) {
        ESP_LOGE(TAG, "burn refused: no unused HMAC key block left");
        return ESP_ERR_NO_MEM;
    }

    /* One batch: a reset between the ID and the key would leave a board that
     * reads as provisioned but cannot derive its own key. */
    err = esp_efuse_batch_write_begin();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_efuse_write_field_blob(kIdField, raw + MPX_EFUSE_ID_OFF,
                                     MPX_ID_LEN * 8);
    if (err == ESP_OK) {
        err = esp_efuse_write_field_blob(kFormatField,
                                         raw + MPX_EFUSE_FORMAT_OFF, 8);
    }
    if (err == ESP_OK) {
        err = esp_efuse_write_field_blob(kFlagsField,
                                         raw + MPX_EFUSE_FLAGS_OFF, 8);
    }
    if (err == ESP_OK) {
        err = esp_efuse_write_field_blob(kKvField, raw + MPX_EFUSE_KVBASE_OFF, 16);
    }
    if (err != ESP_OK) {
        esp_efuse_batch_write_cancel();
        ESP_LOGE(TAG, "burn failed writing the record: %s", esp_err_to_name(err));
        return err;
    }

    /* write_key sets the purpose in the same batch, so the key is never
     * readable-as-a-key without also being marked HMAC_UP. */
    err = esp_efuse_write_key(key_block, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, secret, 32);
    if (err != ESP_OK) {
        esp_efuse_batch_write_cancel();
        ESP_LOGE(TAG, "burn failed writing the HMAC key: %s", esp_err_to_name(err));
        return err;
    }

    /* Protect what was just written. Order does not matter inside the batch,
     * but all three must be set: WR_DIS stops a second record, RD_DIS stops the
     * key being read back (which is what makes HMAC_UP worth using), and the
     * purpose lock stops a later boot re-pointing the block at something else.
     */
    esp_efuse_write_field_bit(ESP_EFUSE_WR_DIS_BLOCK_USR_DATA);
    esp_efuse_set_key_dis_read(key_block);
    esp_efuse_set_key_dis_write(key_block);
    esp_efuse_set_keypurpose_dis_write(key_block);

    err = esp_efuse_batch_write_commit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "burn failed committing the batch: %s", esp_err_to_name(err));
        return err;
    }

    if (key_block_out != NULL) {
        *key_block_out = (uint8_t)(key_block - EFUSE_BLK_KEY0);
    }
    ESP_LOGW(TAG, "burned %s into USR_DATA and S into key block %d (irreversible)",
             rec->robot_id, (int)(key_block - EFUSE_BLK_KEY0));
    return ESP_OK;
}

int mpx_efuse_hmac_sha256(const uint8_t *key, size_t key_len,
                          const uint8_t *msg, size_t msg_len,
                          uint8_t out[32])
{
    uint8_t block = 0;

    (void)key;
    (void)key_len;

    if (msg == NULL || out == NULL) {
        return -1;
    }
    if (!mpx_efuse_find_hmac_key(&block)) {
        return -1;
    }
    const esp_err_t err = esp_hmac_calculate((hmac_key_id_t)block, msg, msg_len, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_hmac_calculate(block %u) failed: %s", (unsigned)block,
                 esp_err_to_name(err));
        return -1;
    }
    return 0;
}

esp_err_t mpx_efuse_mac(uint8_t out[6])
{
    uint8_t mac[6];

    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        err = esp_read_mac(mac, ESP_MAC_BASE);
    }
    if (err != ESP_OK) {
        return err;
    }
    memcpy(out, mac, sizeof mac);
    return ESP_OK;
}

uint16_t mpx_efuse_sec_flags(void)
{
    uint16_t flags = 0;
    size_t crypt_cnt = 0;
    uint8_t block = 0;

    if (esp_efuse_read_field_bit(ESP_EFUSE_SECURE_BOOT_EN)) {
        flags |= MPX_SEC_FLAG_SECURE_BOOT;
    }
    /* SPI_BOOT_CRYPT_CNT saturates at 0b111 when the count can no longer be
     * changed, which is the "release" state: encryption on and no way to turn
     * it off. One or three bits set is "enabled but revocable", which a
     * customer unit must not report as released. */
    if (esp_efuse_read_field_cnt(ESP_EFUSE_SPI_BOOT_CRYPT_CNT, &crypt_cnt) == ESP_OK &&
        crypt_cnt == 7) {
        flags |= MPX_SEC_FLAG_FLASH_ENC_RELEASE;
    }
    if (esp_efuse_read_field_bit(ESP_EFUSE_DIS_PAD_JTAG)) {
        flags |= MPX_SEC_FLAG_JTAG_DISABLED;
    }
    if (esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG)) {
        flags |= MPX_SEC_FLAG_USB_JTAG_DISABLED;
    }
    /* Either bit closes the ROM download path: DIS_DOWNLOAD_MODE removes the
     * mode entirely, ENABLE_SECURITY_DOWNLOAD leaves it encrypted-and-signed
     * only. Both are stronger than stock, so both count. */
    if (esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE) ||
        esp_efuse_read_field_bit(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD)) {
        flags |= MPX_SEC_FLAG_DL_MODE_SECURE;
    }
    if (mpx_efuse_find_hmac_key(&block)) {
        esp_efuse_block_t blk = (esp_efuse_block_t)(EFUSE_BLK_KEY0 + block);
        if (esp_efuse_get_key_dis_read(blk)) {
            flags |= MPX_SEC_FLAG_HMAC_UP_RD_DIS;
        }
    }
    return flags;
}

uint32_t mpx_efuse_secure_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    return (desc != NULL) ? desc->secure_version : 0;
}

esp_err_t mpx_efuse_lock_all(void)
{
    uint8_t block = 0;
    esp_err_t err;

    /* `lock` is the step pilot units skip (D18). It is idempotent and it never
     * writes a key or an ID: it only takes away the ability to change either.
     * WR_DIS_RD_DIS is set last because it freezes RD_DIS, so doing it first
     * would stop the key's own read protection from being applied. */
    err = esp_efuse_batch_write_begin();
    if (err != ESP_OK) {
        return err;
    }

    esp_efuse_write_field_bit(ESP_EFUSE_WR_DIS_BLOCK_USR_DATA);
    if (mpx_efuse_find_hmac_key(&block)) {
        esp_efuse_block_t blk = (esp_efuse_block_t)(EFUSE_BLK_KEY0 + block);
        esp_efuse_set_key_dis_read(blk);
        esp_efuse_set_key_dis_write(blk);
        esp_efuse_set_keypurpose_dis_write(blk);
    }
    esp_efuse_write_field_bit(ESP_EFUSE_WR_DIS_RD_DIS);

    err = esp_efuse_batch_write_commit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lock failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "identity locked: record and key are now read-only");
    return ESP_OK;
}
