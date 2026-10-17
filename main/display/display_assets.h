#pragma once

namespace display {

/**
 * @brief Write the firmware's built-in GIFs (blink, smile, sleepy) into
 *        `dir` unless they are already there at the same size.
 *
 * Call after LittleFS is mounted and before the face restores a saved
 * animation. Returns false if any file could not be written.
 */
bool deploy_builtin_gifs(const char *dir);

}  // namespace display
