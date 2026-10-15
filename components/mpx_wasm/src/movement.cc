#include "movement.h"

#include "esp_log.h"

#include "robot.h"
#include "registry.h"
#include "runner.h"
#include "wasm_sandbox.h"

static const char *TAG = "movement";

namespace skills {

MovementResult run(const char *name, bool from_skill)
{
	if (!name || !*name) return MovementResult::Unknown;

	// Built-in gaits win. A skill cannot shadow "advance" by claiming the
	// name: the firmware's own movements have to stay predictable, and a
	// downloaded skill silently replacing the walk is not a feature.
	robot::GaitCmd cmd;
	if (robot::gait_from_name(name, cmd)) {
		robot::send_gait_cmd(cmd);
		return MovementResult::Started;
	}

	const Entry *e = by_gait(name);
	if (!e) return MovementResult::Unknown;

	// One skill at a time, so a skill asking for a skill-provided movement
	// would be asking to be replaced by another while it is still running.
	// Refused rather than queued or nested -- nesting cannot work, and
	// queueing means the robot performing something long after it was asked.
	if (from_skill) {
		ESP_LOGW(TAG, "'%s' is provided by skill '%s'; a skill cannot start "
		              "another skill", name, e->slug.c_str());
		return MovementResult::NotPermitted;
	}

	if (!start(e->file.c_str(), e->slug.c_str(), "",
	           e->behaviour ? Mode::Behaviour : Mode::OneShot, "gait")) {
		return MovementResult::Busy;
	}
	return MovementResult::Started;
}

const char *result_text(MovementResult r)
{
	switch (r) {
		case MovementResult::Started:      return "started";
		case MovementResult::Unknown:      return "no such movement";
		case MovementResult::Busy:         return "a skill is already running";
		case MovementResult::NotPermitted: return "a skill cannot start another skill";
	}
	return "unknown";
}

std::string list_json()
{
	std::string out = "{\"movements\":[";
	bool first = true;

	for (int i = 0; i < robot::gait_name_count(); ++i) {
		const char *n = robot::gait_name_at(i);
		if (!n) continue;
		if (!first) out += ',';
		first = false;
		out += "{\"name\":\"";
		out += n;
		out += "\",\"source\":\"builtin\"}";
	}

	for (const Entry &e : all()) {
		if (e.provides_gait.empty()) continue;
		if (!first) out += ',';
		first = false;
		out += "{\"name\":\"" + e.provides_gait + "\",\"source\":\"skill\""
		     + ",\"skill\":\"" + e.slug + "\""
		     + ",\"behaviour\":" + (e.behaviour ? "true" : "false") + "}";
	}

	return out + "]}";
}

}  // namespace skills
