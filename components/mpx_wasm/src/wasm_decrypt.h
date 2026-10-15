#pragma once

#include <cstddef>
#include <cstdint>

namespace wasm {

/**
 * @brief Result of MPXE decryption.
 */
enum class DecryptResult {
    Success,              // Decrypted successfully, plaintext in out_buf
    NotEncrypted,         // Data does not start with "MPXE" magic
    Truncated,            // Blob too small to contain a valid header
    UnsupportedVersion,   // Version byte != 0x01
    KeyNotLoaded,         // Robot AES key could not be loaded
    UnwrapFailed,         // GCM auth failure during key unwrap (wrong robot)
    DecryptFailed,        // GCM auth failure during WASM decryption (tampered)
    OutOfMemory,          // malloc failed for output buffer
    CryptoError,          // Internal mbedTLS error
};

/**
 * @brief Decrypt an MPXE-encrypted WASM blob.
 *
 * The caller is responsible for determining that the input is an MPXE
 * blob (e.g., by file extension .mpxe, or by checking the "MPXE" magic
 * bytes for raw byte buffers).  This function does NOT check for the
 * magic header — it assumes the input is valid MPXE and proceeds
 * directly to header parsing and decryption.
 *
 * For raw-byte callers that need automatic detection (e.g. Lua
 * wasm.run_bytes), use decrypt_mpxe_autodetect() which checks the
 * magic header and returns NotEncrypted for plain data.
 *
 * All sensitive key material is zeroed before returning.
 *
 * @param data           Pointer to the MPXE blob.
 * @param data_size      Size of the blob in bytes.
 * @param out_plaintext  [out] Set to malloc'd plaintext on Success.
 *                       Caller must free() this buffer.
 * @param out_size       [out] Size of the plaintext in bytes.
 * @return DecryptResult indicating the outcome.
 */
DecryptResult decrypt_mpxe(const uint8_t *data, size_t data_size,
                           uint8_t **out_plaintext, size_t *out_size);

/**
 * @brief Auto-detect and decrypt — checks "MPXE" magic first.
 *
 * Use this for raw byte buffers where the format is unknown.
 * If the data starts with "MPXE", decrypts it.  Otherwise returns
 * NotEncrypted (caller should treat data as plain .wasm).
 *
 * This is used by wasm.run_bytes() in Lua bindings where there is
 * no file extension to indicate the format.
 */
DecryptResult decrypt_mpxe_autodetect(const uint8_t *data, size_t data_size,
                                      uint8_t **out_plaintext, size_t *out_size);

/**
 * @brief Can this blob be decrypted? Runs the full pipeline, discards the
 *        plaintext, and reports the verdict.
 *
 * For checking a blob at the moment it ARRIVES rather than the moment it is
 * run. A GCM tag failure is not transient: the bytes on flash are wrong, and
 * every future run fails identically. Verifying at install turns "press Run,
 * get 'corrupted or tampered', try again, same result" into one failed
 * download that says what happened.
 *
 * Costs a full decrypt — one malloc the size of the module — paid once per
 * install instead of once per run.
 *
 * @param out_accepted_size  Optional. On Success, the blob length that
 *        actually authenticated — which may be one less than @p data_size if
 *        the payload arrived with a stray trailing byte. Store THAT many
 *        bytes and the copy on flash is canonical, so nothing downstream has
 *        to know this happened.
 */
DecryptResult verify_mpxe(const uint8_t *data, size_t data_size,
                          size_t *out_accepted_size = nullptr);

/** Human-readable name for a DecryptResult, for logs and error strings. */
const char *decrypt_result_name(DecryptResult r);

}  // namespace wasm
