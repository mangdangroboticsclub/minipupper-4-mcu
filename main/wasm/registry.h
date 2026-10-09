#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/* ── What the skills on this robot say about themselves ───────────────────
 *
 * Ported from mpv2-claw (components/mpx_wasm/src/registry.{h,cc}); same
 * section format, same JSON, so skills built by `mpx-cli build` for either
 * firmware describe themselves the same way here.
 *
 * A skill used to be an anonymous blob of bytecode: the firmware knew its
 * filename and nothing else. Everything a skill might want to declare -- that
 * it provides a movement called "moonwalk", that it should start at boot, that
 * it wants running when the robot is picked up -- had nowhere to live.
 *
 * `mpx-cli build` embeds the manifest's declarative fields in the .wasm as a
 * custom section named "mpx". The metadata travels inside the artifact, so
 * whatever moves the module moves the metadata with it. A runtime that does
 * not know the section ignores it.
 *
 * This module scans the filesystem, reads those sections, and caches the
 * result. Rescan on upload and delete; it is not free.
 *
 * Only `provides_gait` is acted on in this firmware so far (POST
 * /v1/robot/gait starts the skill). `autorun` and `on` are parsed and
 * reported but not yet wired to anything -- see .cursor/PORT_NOTES.md.
 */
namespace skills {

struct Entry {
    std::string file;          /**< "/moonwalk.wasm"                       */
    std::string slug;          /**< "moonwalk"                             */
    std::string provides_gait; /**< movement name it registers, or empty   */
    std::vector<std::string> on;  /**< event names that should run it      */
    int  abi       = 0;
    bool autorun   = false;    /**< start at boot                          */
    bool behaviour = false;    /**< run without a total watchdog           */
};

/** Re-read every .wasm on the filesystem. Call after upload, delete, install. */
void rescan();

/** Everything currently known. Cheap; rescan() does the work. */
const std::vector<Entry> &all();

/** The skill providing this movement name, or nullptr. Case-sensitive. */
const Entry *by_gait(const char *gait_name);

/** The skill with this slug, or nullptr. */
const Entry *by_slug(const char *slug);

/** Every skill that asked to be run on `event`. */
std::vector<const Entry *> by_event(const char *event);

/** The one skill marked autorun, or nullptr. Later ones are ignored and
 *  logged: two skills both claiming the boot slot is a mistake, not a queue. */
const Entry *autorun_entry();

/**
 * @brief Read the embedded manifest out of a module in memory.
 *
 * Exposed for the upload path, which already holds the bytes and can validate
 * before writing. Returns false if there is no "mpx" section.
 */
bool parse_module(const uint8_t *wasm, std::size_t len, Entry &out);

/** The registry as JSON, for GET /v1/skills/registry and the web UI. */
std::string to_json();

}  // namespace skills
