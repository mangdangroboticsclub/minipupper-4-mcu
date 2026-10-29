/* The sealed blob the robot sends to the cloud (docs/ROBOT_IDENTITY_AND_KEYS.md
 * §6.4). Two layers, and no RSA: the encryption itself is device code in
 * mpx_provision.c, because it needs mbedTLS. What lives here is the byte
 * layout, which is the part the cloud parses and the part a mistake would break
 * silently -- so it is ESP-free and covered by host tests.
 *
 *     blob     offset size  field
 *              0      4     magic "MPXS"
 *              4      1     format 0x01
 *              5      1     seal_key_id   (index into the firmware's KMS keys)
 *              6      2     reserved
 *              8      384   RSA-OAEP-3072-SHA256(ciphertext)
 *
 *     plain    offset size  field
 *              0      4     magic "MPXK"
 *              4      1     format 0x01
 *              5      4     key_version (BE)
 *              9      16    robot_id (ASCII, zero-padded)
 *              25     16    nonce (from the reservation)
 *              41     32    K_kv
 *              73     6     base MAC
 *              79     2     sec_flags
 *              81     4     esp_app_desc.secure_version
 *              85     32    app version, NUL-padded
 *              117    32    sha256(DS public key DER), zeros if none
 */
#ifndef MPX_SEAL_H
#define MPX_SEAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mpx_id.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPX_SEAL_FORMAT_V1      0x01
#define MPX_SEAL_BLOB_MAGIC     "MPXS"
#define MPX_SEAL_PLAIN_MAGIC    "MPXK"
#define MPX_SEAL_RSA_BYTES      384   /* RSA-3072 */
#define MPX_SEAL_BLOB_HEADER    8
#define MPX_SEAL_BLOB_LEN       (MPX_SEAL_BLOB_HEADER + MPX_SEAL_RSA_BYTES)

/* What the robot reports about its own security state, §6.4. The cloud refuses
 * a prod blob that does not meet the `secure` bar (D12), so these bits are the
 * difference between "pilot unit that skipped lock" and "customer unit". */
#define MPX_SEC_FLAG_SECURE_BOOT          0x0001
#define MPX_SEC_FLAG_FLASH_ENC_RELEASE    0x0002
#define MPX_SEC_FLAG_JTAG_DISABLED        0x0004
#define MPX_SEC_FLAG_USB_JTAG_DISABLED    0x0008
#define MPX_SEC_FLAG_DL_MODE_SECURE       0x0010
#define MPX_SEC_FLAG_HMAC_UP_RD_DIS       0x0020

#define MPX_SEAL_NONCE_LEN 16
#define MPX_SEAL_MAC_LEN   6
#define MPX_SEAL_APPVER_LEN 32
#define MPX_SEAL_DSHASH_LEN 32
#define MPX_SEAL_KEY_LEN   32
#define MPX_SEAL_PLAIN_LEN 149

#define MPX_SEAL_PLAIN_KV_OFF      5
#define MPX_SEAL_PLAIN_ID_OFF      9
#define MPX_SEAL_PLAIN_NONCE_OFF   25
#define MPX_SEAL_PLAIN_KEY_OFF     41
#define MPX_SEAL_PLAIN_MAC_OFF     73
#define MPX_SEAL_PLAIN_FLAGS_OFF   79
#define MPX_SEAL_PLAIN_SVER_OFF    81
#define MPX_SEAL_PLAIN_APPVER_OFF  85
#define MPX_SEAL_PLAIN_DSHASH_OFF  117

typedef struct {
    uint32_t key_version;
    char     robot_id[MPX_ID_STR_LEN + 1];
    uint8_t  nonce[MPX_SEAL_NONCE_LEN];
    uint8_t  key[MPX_SEAL_KEY_LEN];
    uint8_t  mac[MPX_SEAL_MAC_LEN];
    uint16_t sec_flags;
    uint32_t secure_version;
    char     app_version[MPX_SEAL_APPVER_LEN + 1];
    uint8_t  ds_pub_sha256[MPX_SEAL_DSHASH_LEN]; /* zeros when no DS key */
} mpx_seal_plain_t;

/* Serialises the OAEP plaintext. Returns 0, or -1 for an invalid robot ID,
 * key_version 0 or an app version that does not fit. */
int mpx_seal_plain_encode(uint8_t out[MPX_SEAL_PLAIN_LEN],
                          const mpx_seal_plain_t *p);

/* Parses the OAEP plaintext, for tests and for the bench tool that inspects a
 * blob before it is uploaded. Returns 0 on success. */
int mpx_seal_plain_decode(mpx_seal_plain_t *p,
                          const uint8_t in[MPX_SEAL_PLAIN_LEN]);

/* Wraps the RSA ciphertext into the blob. The ciphertext is *not* checked for
 * content -- it is opaque here -- only its length. */
int mpx_seal_blob_encode(uint8_t out[MPX_SEAL_BLOB_LEN], uint8_t seal_key_id,
                         const uint8_t rsa_ciphertext[MPX_SEAL_RSA_BYTES]);

/* True when the first bytes are the blob magic and the format is known, i.e.
 * "this is a sealed blob this firmware's stations can upload". */
bool mpx_seal_blob_looks_valid(const uint8_t *blob, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* MPX_SEAL_H */
