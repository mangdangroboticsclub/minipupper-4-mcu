#include "registry.h"

#include <cstring>

#include "esp_log.h"
#include "cJSON.h"

#include "mpx_skill_fs.h"

static const char *TAG = "registry";

namespace skills {
namespace {

std::vector<Entry> s_entries;

/* ── WebAssembly section walking ──────────────────────────────────────────
 *
 * The format is simple enough that pulling in a parser would cost more than
 * it saves: a module is an 8-byte header followed by (id, LEB128 length,
 * body). Custom sections have id 0 and begin with their own LEB128-prefixed
 * name.
 *
 * Every length is bounds-checked against the buffer. This parses a file that
 * may have arrived from a marketplace, so it is untrusted input and a
 * truncated or hostile module must produce "no metadata", never a read past
 * the end.
 */
bool read_leb128(const uint8_t *buf, std::size_t len, std::size_t &i,
                 std::uint32_t &out)
{
	std::uint32_t value = 0;
	int shift = 0;
	while (i < len) {
		const uint8_t byte = buf[i++];
		value |= static_cast<std::uint32_t>(byte & 0x7F) << shift;
		if (!(byte & 0x80)) {
			out = value;
			return true;
		}
		shift += 7;
		if (shift > 28) return false;      // > 32 bits: malformed
	}
	return false;
}

bool find_mpx_section(const uint8_t *wasm, std::size_t len,
                      const uint8_t *&body, std::uint32_t &body_len)
{
	if (len < 8 || std::memcmp(wasm, "\0asm", 4) != 0) return false;

	std::size_t i = 8;
	while (i < len) {
		const uint8_t id = wasm[i++];
		std::uint32_t size = 0;
		if (!read_leb128(wasm, len, i, size)) return false;
		if (size > len - i) return false;              // truncated

		const std::size_t section_end = i + size;

		if (id == 0) {
			std::size_t j = i;
			std::uint32_t name_len = 0;
			if (read_leb128(wasm, section_end, j, name_len)
			    && name_len <= section_end - j
			    && name_len == 3
			    && std::memcmp(wasm + j, "mpx", 3) == 0) {
				body     = wasm + j + 3;
				body_len = static_cast<std::uint32_t>(section_end - (j + 3));
				return true;
			}
		}
		i = section_end;
	}
	return false;
}

std::string json_str(const cJSON *obj, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
	return (cJSON_IsString(v) && v->valuestring) ? std::string(v->valuestring)
	                                             : std::string();
}

}  // namespace

bool parse_module(const uint8_t *wasm, std::size_t len, Entry &out)
{
	const uint8_t *body = nullptr;
	std::uint32_t  body_len = 0;
	if (!find_mpx_section(wasm, len, body, body_len) || body_len == 0) return false;

	cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(body),
	                                    body_len);
	if (!root) return false;

	out.slug          = json_str(root, "slug");
	out.provides_gait = json_str(root, "provides_gait");

	const cJSON *abi = cJSON_GetObjectItemCaseSensitive(root, "abi");
	if (cJSON_IsNumber(abi)) out.abi = abi->valueint;

	out.autorun   = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "autorun"));
	out.behaviour = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "behaviour"));

	out.on.clear();
	const cJSON *on = cJSON_GetObjectItemCaseSensitive(root, "on");
	if (cJSON_IsArray(on)) {
		const cJSON *ev = nullptr;
		cJSON_ArrayForEach(ev, on) {
			if (cJSON_IsString(ev) && ev->valuestring) out.on.push_back(ev->valuestring);
		}
	}

	cJSON_Delete(root);
	return true;
}

void rescan()
{
	s_entries.clear();

	for (const std::string &name : fs::list_files("/")) {
		if (name.size() < 6) continue;
		const std::string ext = name.substr(name.size() - 5);
		if (ext != ".wasm" && !(name.size() >= 5 && name.substr(name.size() - 5) == ".mpxe"))
			continue;

		const std::string path = (name[0] == '/') ? name : "/" + name;
		auto bytes = fs::read_file(path.c_str());
		if (bytes.empty()) continue;

		Entry e;
		e.file = path;
		if (!parse_module(bytes.data(), bytes.size(), e)) {
			// A skill with no embedded manifest is perfectly valid — it just
			// cannot register a gait or a trigger. Built before this existed,
			// or built by a toolchain that does not add the section.
			continue;
		}
		if (e.slug.empty()) {
			e.slug = path.substr(1, path.size() - 6);
		}
		s_entries.push_back(std::move(e));
	}

	ESP_LOGI(TAG, "%d skill(s) with embedded metadata", (int)s_entries.size());
	for (const Entry &e : s_entries) {
		ESP_LOGI(TAG, "  %-20s gait=%-12s autorun=%d behaviour=%d events=%d",
		         e.slug.c_str(),
		         e.provides_gait.empty() ? "-" : e.provides_gait.c_str(),
		         (int)e.autorun, (int)e.behaviour, (int)e.on.size());
	}
}

const std::vector<Entry> &all() { return s_entries; }

const Entry *by_gait(const char *gait_name)
{
	if (!gait_name || !*gait_name) return nullptr;
	for (const Entry &e : s_entries)
		if (!e.provides_gait.empty() && e.provides_gait == gait_name) return &e;
	return nullptr;
}

const Entry *by_slug(const char *slug)
{
	if (!slug || !*slug) return nullptr;
	for (const Entry &e : s_entries)
		if (e.slug == slug) return &e;
	return nullptr;
}

std::vector<const Entry *> by_event(const char *event)
{
	std::vector<const Entry *> out;
	if (!event || !*event) return out;
	for (const Entry &e : s_entries)
		for (const std::string &ev : e.on)
			if (ev == event) { out.push_back(&e); break; }
	return out;
}

const Entry *autorun_entry()
{
	const Entry *found = nullptr;
	for (const Entry &e : s_entries) {
		if (!e.autorun) continue;
		if (found) {
			ESP_LOGW(TAG, "'%s' also claims autorun; ignoring (autorun is one "
			              "slot, not a queue) — '%s' has it",
			         e.slug.c_str(), found->slug.c_str());
			continue;
		}
		found = &e;
	}
	return found;
}

std::string to_json()
{
	std::string out = "{\"skills\":[";
	bool first = true;
	for (const Entry &e : s_entries) {
		if (!first) out += ',';
		first = false;
		out += "{\"slug\":\"" + e.slug + "\",\"file\":\"" + e.file + "\",\"abi\":"
		     + std::to_string(e.abi)
		     + ",\"provides_gait\":\"" + e.provides_gait + "\""
		     + ",\"autorun\":"   + (e.autorun   ? "true" : "false")
		     + ",\"behaviour\":" + (e.behaviour ? "true" : "false")
		     + ",\"on\":[";
		for (std::size_t i = 0; i < e.on.size(); ++i) {
			if (i) out += ',';
			out += "\"" + e.on[i] + "\"";
		}
		out += "]}";
	}
	return out + "]}";
}

}  // namespace skills
