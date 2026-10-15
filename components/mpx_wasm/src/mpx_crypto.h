#pragma once

#include <cstddef>
#include <cstdint>

/* Ported from the MPX-Dog firmware, where this had two consumers: the
 * AES-GCM frames of the cloud chat pipeline, and the MPXE encrypted-skill
 * envelope. The chat pipeline is gone -- ESP-Claw's agent runs on the device
 * now -- so wasm_decrypt.cc is the only caller left, which is why this lives
 * inside mpx_wasm rather than in a shared component. */
namespace crypto {

// ── AES-256-GCM constants ────────────────────────────────────
constexpr size_t KEY_SIZE = 32;    // 256 bits
constexpr size_t IV_SIZE  = 12;    // 96-bit nonce (recommended for GCM)
constexpr size_t TAG_SIZE = 16;    // 128-bit authentication tag

/**
 * @brief Encrypt and authenticate a buffer using AES-256-GCM.
 *
 * Uses the ESP32-S3 hardware-accelerated AES engine via mbedtls.
 *
 * @param key         32-byte secret key.
 * @param iv          12-byte initialization vector (must be unique per msg).
 * @param aad         Additional Authenticated Data (may be nullptr).
 * @param aad_len     Length of AAD in bytes.
 * @param plaintext   Input plaintext.
 * @param pt_len      Plaintext length in bytes.
 * @param ciphertext  Output buffer (must be >= pt_len bytes).
 * @param tag         Output 16-byte authentication tag.
 * @return true on success.
 */
bool gcm_encrypt(const uint8_t key[KEY_SIZE],
                 const uint8_t iv[IV_SIZE],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *plaintext, size_t pt_len,
                 uint8_t *ciphertext,
                 uint8_t tag[TAG_SIZE]);

/**
 * @brief Decrypt and verify a buffer using AES-256-GCM.
 *
 * @param key         32-byte secret key.
 * @param iv          12-byte initialization vector.
 * @param aad         Additional Authenticated Data (may be nullptr).
 * @param aad_len     Length of AAD in bytes.
 * @param ciphertext  Input ciphertext.
 * @param ct_len      Ciphertext length in bytes.
 * @param tag         16-byte authentication tag to verify.
 * @param plaintext   Output buffer (must be >= ct_len bytes).
 * @return true on success (authentication passed).
 */
bool gcm_decrypt(const uint8_t key[KEY_SIZE],
                 const uint8_t iv[IV_SIZE],
                 const uint8_t *aad, size_t aad_len,
                 const uint8_t *ciphertext, size_t ct_len,
                 const uint8_t tag[TAG_SIZE],
                 uint8_t *plaintext);

/**
 * @brief Generate cryptographically secure random bytes.
 *
 * Uses the ESP32-S3 hardware random number generator (TRNG)
 * via esp_fill_random().
 *
 * @param buf  Output buffer.
 * @param len  Number of bytes to generate.
 */
void random_bytes(uint8_t *buf, size_t len);

/**
 * @brief Parse a hex string into a byte array.
 *
 * @param hex   Null-terminated hex string (e.g. "a1b2c3...").
 * @param buf   Output byte buffer.
 * @param buflen  Size of output buffer.
 * @return true on success.
 */
bool hex_to_bytes(const char *hex, uint8_t *buf, size_t buflen);

}  // namespace crypto
