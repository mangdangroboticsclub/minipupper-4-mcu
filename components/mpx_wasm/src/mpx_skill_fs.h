/*
 * SPDX-FileCopyrightText: 2026 MangDang
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The skill filesystem.
 *
 * The MPX-Dog firmware kept skills at the root of a 13.4 MB LittleFS partition
 * and reached them through namespace fs in fs/littlefs_manager.h. ESP-Claw has
 * no LittleFS: it has FATFS mounted at a root that may be flash or an SD card
 * depending on the board, and the only correct way to find it is
 * claw_paths_join(CLAW_PATH_DATA, ...).
 *
 * So this keeps the same two functions and the same namespace -- registry.cc
 * and wasm_sandbox.cc call them unchanged -- but reinterprets what a path
 * means. Every path here is relative to <DATA>/mpx_skills, and "/" is that
 * directory rather than the root of a partition.
 *
 * That is not just a convenience. It makes the skill root a chroot: a path
 * arriving from a marketplace cannot climb out of it, because it is never
 * resolved against anything else. Paths containing ".." are refused outright.
 *
 * A side benefit of living under the DATA root: skills show up in ESP-Claw's
 * /api/files browser and to cap_files for free, so the agent can list and
 * delete them without any of this being taught to it.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fs {

/**
 * @brief Absolute path of the skills directory, e.g. "/fatfs/mpx_skills".
 *
 * Created on first use. Returns an empty string if the DATA root is not
 * mounted yet -- callers should treat that as "no skills", not as an error.
 */
const std::string &skills_root();

/**
 * @brief Read a whole file. `path` is relative to the skills root; a leading
 *        '/' is optional and means the same thing.
 *
 * @return the bytes, or an empty vector if the file is missing, unreadable,
 *         or the path tries to escape the skills root.
 */
std::vector<std::uint8_t> read_file(const char *path);

/**
 * @brief List regular files directly under `dir_path` (relative to the skills
 *        root; "/" means the root itself).
 *
 * @return names with a leading '/', e.g. "/moonwalk.wasm". Subdirectories are
 *         not descended into -- the layout is deliberately flat.
 */
std::vector<std::string> list_files(const char *dir_path);

/**
 * @brief Write a whole file. Used by the /v1/skills/upload handler.
 *
 * @return false on a bad path or any write error. A partial write is removed
 *         rather than left behind, so a failed upload cannot leave a truncated
 *         module that the registry would then try to parse.
 */
bool write_file(const char *path, const std::uint8_t *data, std::size_t len);

/** @brief Delete a file. Returns false if it was not there or not removable. */
bool delete_file(const char *path);

/** @brief Whether a file exists. */
bool exists(const char *path);

}  // namespace fs
