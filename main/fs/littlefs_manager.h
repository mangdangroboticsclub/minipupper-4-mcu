#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fs {

/**
 * @brief Initialize and mount the LittleFS partition.
 *
 * If the partition contains valid LittleFS data it is mounted directly;
 * otherwise it is formatted first then mounted.
 *
 * @return true on success, false on failure.
 */
bool init_littlefs();

/**
 * @brief Unmount and release LittleFS resources.
 */
void deinit_littlefs();

/**
 * @brief Write data to a file, creating/overwriting it.
 *
 * @param path  Absolute path within the LittleFS mount (e.g. "/skill.wasm").
 * @param data  Pointer to the data buffer.
 * @param len   Number of bytes to write.
 * @return true on success.
 */
bool write_file(const char *path, const void *data, std::size_t len);

/**
 * @brief Read an entire file into memory.
 *
 * @param path  Absolute path within the LittleFS mount.
 * @return Vector of bytes, empty on failure.
 */
std::vector<uint8_t> read_file(const char *path);

/**
 * @brief Delete a file.
 *
 * @param path  Absolute path within the LittleFS mount.
 * @return true on success or if the file did not exist.
 */
bool delete_file(const char *path);

/**
 * @brief Check whether a file exists.
 */
bool file_exists(const char *path);

/**
 * @brief Return the size of a file in bytes, or 0 if it doesn't exist.
 */
std::size_t file_size(const char *path);

/**
 * @brief List all files under a given prefix path.
 *
 * Returns a vector of full paths (e.g. "/skill.wasm").
 */
std::vector<std::string> list_files(const char *dir_path);

/**
 * @brief Return total and used bytes on the LittleFS partition.
 */
bool stats(std::size_t &total_bytes, std::size_t &used_bytes);

}  // namespace fs
