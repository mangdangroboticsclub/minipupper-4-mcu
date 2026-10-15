#include "wasm_decrypt.h"
#include "mpx_crypto.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "wasm_decrypt";

// ── MPXE blob format constants ──────────────────────────────
static constexpr uint32_t MPXE_MAGIC      = 0x4558504D;  // "MPXE" (little-endian)
static constexpr uint8_t  MPXE_VERSION_1  = 0x01;
static constexpr uint8_t  KEY_ALGO_DIRECT = 0x00;

// Offsets within the MPXE header
static constexpr size_t OFF_MAGIC            = 0;
static constexpr size_t OFF_VERSION          = 4;
static constexpr size_t OFF_WRAP_IV          = 5;
static constexpr size_t OFF_WRAPPED_KEY      = 17;   // 32 bytes ct + 16 bytes tag
static constexpr size_t OFF_KEY_ALGO         = 65;
static constexpr size_t OFF_SKILL_ID_HASH    = 66;   // SHA-256 first 8 bytes
static constexpr size_t OFF_WASM_IV          = 74;
static constexpr size_t OFF_ENCRYPTED_WASM   = 86;

static constexpr size_t HEADER_SIZE          = 86;   // total fixed header
static constexpr size_t GCM_TAG_SIZE         = 16;
static constexpr size_t MIN_MPXE_SIZE        = HEADER_SIZE + 1 + GCM_TAG_SIZE;  // 103

// ── Secure zeroing (compiler-proof) ─────────────────────────
// Plain memset() on a buffer that is about to be freed may be
// optimised away by the compiler as a dead store.  Using a
// volatile pointer forces the compiler to emit the stores.
static void secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *p = static_cast<volatile uint8_t *>(ptr);
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
    }
}

// ── Load robot AES key from Kconfig ─────────────────────────
// Returns false if the key is all zeros or invalid hex.
static bool load_robot_key(uint8_t key_out[crypto::KEY_SIZE])
{
    if (!crypto::hex_to_bytes(CONFIG_MP4_SKILL_AES_KEY_HEX, key_out,
                               crypto::KEY_SIZE)) {
        ESP_LOGE(TAG, "Failed to parse robot AES key from Kconfig");
        return false;
    }

    // Refuse the all-zeros default key in production builds.
    // In debug builds we allow it so encrypted skills work
    // during development without provisioning real keys.
#ifdef NDEBUG
    bool all_zero = true;
    for (size_t i = 0; i < crypto::KEY_SIZE; i++) {
        if (key_out[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) {
        ESP_LOGE(TAG, "Robot AES key is all zeros — robot not provisioned");
        return false;
    }
#endif

    return true;
}

namespace wasm {

// ── One decryption attempt at a given blob length ──────────────────────────
// Split out from decrypt_mpxe_core so the caller can retry at a different
// length. Logs nothing about a tag mismatch: whether that is worth an error
// depends on whether the retry succeeds, and only the caller knows.
static DecryptResult try_decrypt(const uint8_t *data, size_t data_size,
                                 uint8_t **out_plaintext, size_t *out_size)
{
    *out_plaintext = nullptr;
    *out_size = 0;

    // ── Step 1: Validate header ──────────────────────────────
    if (data_size < MIN_MPXE_SIZE) {
        ESP_LOGE(TAG, "MPXE blob too small: %zu bytes (min %zu)",
                 data_size, MIN_MPXE_SIZE);
        return DecryptResult::Truncated;
    }

    uint8_t version = data[OFF_VERSION];
    if (version != MPXE_VERSION_1) {
        ESP_LOGE(TAG, "Unsupported MPXE version: 0x%02x", version);
        return DecryptResult::UnsupportedVersion;
    }

    uint8_t key_algo = data[OFF_KEY_ALGO];
    if (key_algo != KEY_ALGO_DIRECT) {
        ESP_LOGW(TAG, "Unexpected key_algo 0x%02x, continuing anyway", key_algo);
    }

    // ── Parse header fields ──────────────────────────────────
    const uint8_t *wrap_iv         = data + OFF_WRAP_IV;
    const uint8_t *wrapped_key_ct  = data + OFF_WRAPPED_KEY;       // 48 bytes (32 ct + 16 tag)
    const uint8_t *blob_skill_hash = data + OFF_SKILL_ID_HASH;     // 8 bytes
    const uint8_t *wasm_iv         = data + OFF_WASM_IV;
    const uint8_t *encrypted_wasm  = data + OFF_ENCRYPTED_WASM;

    size_t wasm_ct_len = data_size - OFF_ENCRYPTED_WASM - GCM_TAG_SIZE;
    const uint8_t *wasm_tag = data + OFF_ENCRYPTED_WASM + wasm_ct_len;

    // ── Step 2: Load robot key ───────────────────────────────
    uint8_t robot_key[crypto::KEY_SIZE];
    if (!load_robot_key(robot_key)) {
        return DecryptResult::KeyNotLoaded;
    }

    // ── Step 3: Unwrap the per-skill key ─────────────────────
    // AAD = "wasm-wrap:v1:" + robot_uuid
    char wrap_aad_buf[128];
    int wrap_aad_len = snprintf(wrap_aad_buf, sizeof(wrap_aad_buf),
                                "wasm-wrap:v1:%s", CONFIG_MP4_ROBOT_UUID);

    uint8_t skill_key[crypto::KEY_SIZE];
    bool unwrap_ok = crypto::gcm_decrypt(
        robot_key,
        wrap_iv,
        reinterpret_cast<const uint8_t *>(wrap_aad_buf),
        static_cast<size_t>(wrap_aad_len),
        wrapped_key_ct, crypto::KEY_SIZE,
        wrapped_key_ct + crypto::KEY_SIZE,  // tag follows ciphertext
        skill_key);

    secure_zero(robot_key, sizeof(robot_key));

    if (!unwrap_ok) {
        ESP_LOGE(TAG, "Key unwrap failed — blob was encrypted for a "
                 "different robot or tampered in transit");
        return DecryptResult::UnwrapFailed;
    }

    ESP_LOGI(TAG, "Per-skill key unwrapped successfully");

    // ── Step 4: Decrypt the WASM binary ──────────────────────
    // AAD = "wasm-skill:v1:" + hex(blob_skill_hash[0:8])
    char wasm_aad_buf[128];
    char hash_hex[17];
    for (int i = 0; i < 8; i++) {
        snprintf(&hash_hex[i * 2], 3, "%02x", blob_skill_hash[i]);
    }
    hash_hex[16] = '\0';
    int wasm_aad_len = snprintf(wasm_aad_buf, sizeof(wasm_aad_buf),
                                "wasm-skill:v1:%s", hash_hex);

    uint8_t *plaintext = static_cast<uint8_t *>(std::malloc(wasm_ct_len));
    if (!plaintext) {
        ESP_LOGE(TAG, "OOM allocating %zu bytes for decrypted WASM", wasm_ct_len);
        secure_zero(skill_key, sizeof(skill_key));
        return DecryptResult::OutOfMemory;
    }

    bool decrypt_ok = crypto::gcm_decrypt(
        skill_key,
        wasm_iv,
        reinterpret_cast<const uint8_t *>(wasm_aad_buf),
        static_cast<size_t>(wasm_aad_len),
        encrypted_wasm, wasm_ct_len,
        wasm_tag,
        plaintext);

    secure_zero(skill_key, sizeof(skill_key));

    if (!decrypt_ok) {
        secure_zero(plaintext, wasm_ct_len);
        std::free(plaintext);
        return DecryptResult::DecryptFailed;
    }

    ESP_LOGI(TAG, "WASM decrypted successfully (%zu bytes plaintext)", wasm_ct_len);

    *out_plaintext = plaintext;
    *out_size = wasm_ct_len;
    return DecryptResult::Success;
}

/* ── Core decryption, with a one-byte tail probe ────────────────────────────
 *
 * WHAT THE ROBOT'S OWN LOGS ESTABLISHED, before this existed:
 *
 *   Per-skill key unwrapped successfully        <- bytes 0..85 are exact
 *   gcm_auth_decrypt failed (integrity check)   <- the ciphertext is not
 *   blob=26996 B ... ✓ gaits deployed (26995b)  <- and it is 1 byte long
 *
 * The key unwrap reads only the first 86 bytes. It succeeding proves the
 * header arrived byte-for-byte, which rules out a shifted or mangled stream
 * and leaves exactly one shape of fault: something appended a byte to the
 * END. The tag is located by counting back from the end of the file, so one
 * trailing byte moves the tag by one and fails the check every single time,
 * identically, forever — which is precisely what pressing Run kept showing.
 *
 * So try again one byte shorter. This is not a guess dressed up as a fix:
 * GCM either authenticates or it does not. If the shortened blob
 * authenticates, the bytes ARE the publisher's and the extra byte was
 * padding that rode along; if it does not, nothing is accepted and the
 * original failure is reported. Exactly one byte is probed, because exactly
 * one byte is what a successful unwrap plus a wrong tag can mean — anything
 * more would be fishing.
 *
 * It is logged loudly rather than silently absorbed. A robot that quietly
 * repairs its downloads is a robot whose publishing pipeline stays broken.
 */
static DecryptResult decrypt_mpxe_core(const uint8_t *data, size_t data_size,
                                       uint8_t **out_plaintext, size_t *out_size,
                                       size_t *out_blob_len = nullptr)
{
    if (out_blob_len) *out_blob_len = data_size;

    DecryptResult r = try_decrypt(data, data_size, out_plaintext, out_size);
    if (r != DecryptResult::DecryptFailed) return r;

    if (data_size > MIN_MPXE_SIZE) {
        const DecryptResult retry =
            try_decrypt(data, data_size - 1, out_plaintext, out_size);
        if (retry == DecryptResult::Success) {
            if (out_blob_len) *out_blob_len = data_size - 1;
            ESP_LOGW(TAG, "Blob carried ONE TRAILING BYTE too many (0x%02x). "
                          "The %zu-byte payload authenticates; the %zu-byte "
                          "one does not. Accepting the %zu-byte payload.",
                     data[data_size - 1], data_size - 1, data_size,
                     data_size - 1);
            ESP_LOGW(TAG, "This is a PUBLISHING/TRANSPORT bug, not a robot "
                          "one — the blob left its encoder a byte longer than "
                          "it was encrypted. Worth fixing upstream.");
            return retry;
        }
    }

    /* Neither length authenticates. The per-skill key unwrapped a moment ago,
     * which already rules out the wrong robot, the wrong root key and a
     * mangled header, so the ciphertext itself does not match its tag. */
    const size_t ct_len = data_size - OFF_ENCRYPTED_WASM - GCM_TAG_SIZE;
    ESP_LOGE(TAG, "WASM decryption failed — ciphertext does not match its GCM "
                  "tag (key unwrap already succeeded, so this is NOT a "
                  "wrong-robot or wrong-key problem)");
    ESP_LOGE(TAG, "  blob=%zu B = header %zu + ciphertext %zu + tag %zu; tag "
                  "read at offset %zu. Trailing bytes: %02x %02x %02x %02x. "
                  "One byte shorter does not authenticate either, so this is "
                  "not a stray trailing byte — the payload differs from what "
                  "was encrypted.",
             data_size, OFF_ENCRYPTED_WASM, ct_len, GCM_TAG_SIZE,
             OFF_ENCRYPTED_WASM + ct_len,
             data[data_size - 4], data[data_size - 3],
             data[data_size - 2], data[data_size - 1]);
    return DecryptResult::DecryptFailed;
}

DecryptResult decrypt_mpxe(const uint8_t *data, size_t data_size,
                           uint8_t **out_plaintext, size_t *out_size)
{
    // No magic check — caller already knows this is MPXE
    // (e.g., by file extension .mpxe).
    return decrypt_mpxe_core(data, data_size, out_plaintext, out_size);
}

DecryptResult decrypt_mpxe_autodetect(const uint8_t *data, size_t data_size,
                                      uint8_t **out_plaintext, size_t *out_size)
{
    *out_plaintext = nullptr;
    *out_size = 0;

    // Check magic bytes — if not "MPXE", it's plain data
    if (data_size < 4) {
        return DecryptResult::NotEncrypted;
    }

    uint32_t magic;
    std::memcpy(&magic, data + OFF_MAGIC, 4);
    if (magic != MPXE_MAGIC) {
        return DecryptResult::NotEncrypted;
    }

    ESP_LOGI(TAG, "MPXE magic detected, entering decryption pipeline");
    return decrypt_mpxe_core(data, data_size, out_plaintext, out_size);
}

DecryptResult verify_mpxe(const uint8_t *data, size_t data_size,
                          size_t *out_accepted_size)
{
    uint8_t *plaintext = nullptr;
    size_t   plain_len = 0;
    const DecryptResult r = decrypt_mpxe_core(data, data_size, &plaintext,
                                              &plain_len, out_accepted_size);
    if (plaintext) {
        secure_zero(plaintext, plain_len);
        std::free(plaintext);
    }
    return r;
}

const char *decrypt_result_name(DecryptResult r)
{
    switch (r) {
    case DecryptResult::Success:            return "ok";
    case DecryptResult::NotEncrypted:       return "not encrypted";
    case DecryptResult::Truncated:          return "truncated";
    case DecryptResult::UnsupportedVersion: return "unsupported MPXE version";
    case DecryptResult::KeyNotLoaded:       return "robot key not provisioned";
    case DecryptResult::UnwrapFailed:       return "encrypted for a different robot";
    case DecryptResult::DecryptFailed:      return "damaged in transit (GCM tag mismatch)";
    case DecryptResult::OutOfMemory:        return "out of memory";
    case DecryptResult::CryptoError:        return "crypto error";
    }
    return "unknown";
}

}  // namespace wasm
