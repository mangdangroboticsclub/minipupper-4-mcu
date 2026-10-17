#pragma once

namespace network {

/**
 * @brief Copy the embedded PWA assets from firmware binary to LittleFS.
 *
 * This is called once on first boot to populate /fs/www/ with the
 * compiled Svelte frontend and its gzipped variants. Subsequent boots
 * skip the copy if the files already exist.
 *
 * @return true on success (or if already deployed).
 */
bool deploy_www_assets();

}  // namespace network
