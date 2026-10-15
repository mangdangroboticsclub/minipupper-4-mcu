#include "trace_ring.h"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace util {
namespace {

constexpr std::size_t CAPACITY = 256;

struct Sample {
	std::uint32_t seq;
	std::uint32_t t_ms;
	float         value;
	char          name[TRACE_NAME_MAX];
};

Sample        s_ring[CAPACITY] = {};
std::uint32_t s_next_seq       = 0;

// The WASM thread writes and an httpd thread reads, so the ring needs a lock.
// A mutex rather than atomics because a Sample is 24 bytes and a reader must
// not see half of one. Created on first use: this is reached from a skill,
// which cannot happen before app_main() has finished starting things.
SemaphoreHandle_t s_lock = nullptr;

SemaphoreHandle_t lock()
{
	if (s_lock == nullptr) {
		s_lock = xSemaphoreCreateMutex();
	}
	return s_lock;
}

/* Append a float with two decimals and no locale, no exponent, no NaN spelled
 * in a way JSON cannot parse. cJSON is not used here because this runs on the
 * hot path of a control loop's trace call. */
void append_float(std::string &out, float v)
{
	// JSON has no NaN or Infinity. Emitting either produces a response the
	// CLI cannot parse, which reads as "the robot is broken" rather than "your
	// control loop diverged" -- so say so explicitly instead.
	if (v != v) { out += "null"; return; }
	if (v >  3.0e38f) { out += "1e38";  return; }
	if (v < -3.0e38f) { out += "-1e38"; return; }

	char buf[32];
	const int n = std::snprintf(buf, sizeof(buf), "%.4g", static_cast<double>(v));
	out.append(buf, (n > 0 && n < static_cast<int>(sizeof(buf))) ? n : 0);
}

}  // namespace

void trace_ring_reset()
{
	SemaphoreHandle_t m = lock();
	if (m && xSemaphoreTake(m, pdMS_TO_TICKS(20)) == pdTRUE) {
		s_next_seq = 0;
		std::memset(s_ring, 0, sizeof(s_ring));
		xSemaphoreGive(m);
	}
}

void trace_ring_put(const char *name, float value, std::uint32_t t_ms)
{
	if (!name) return;

	SemaphoreHandle_t m = lock();
	// A trace must never stall a control loop. If the lock is busy the sample
	// is dropped, and the sequence gap tells the reader it happened.
	if (!m || xSemaphoreTake(m, 0) != pdTRUE) return;

	Sample &s = s_ring[s_next_seq % CAPACITY];
	s.seq   = s_next_seq;
	s.t_ms  = t_ms;
	s.value = value;

	// The name comes from inside the sandbox, so it is untrusted input that
	// ends up inside a JSON string. Rather than escape it on the way out --
	// on a path read by a chart -- it is restricted on the way in to the
	// characters a signal name plausibly needs. A quote or a backslash here
	// would otherwise produce a response the CLI cannot parse.
	std::size_t n = 0;
	for (; name[n] != '\0' && n < TRACE_NAME_MAX - 1; ++n) {
		const char c = name[n];
		const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		               || (c >= '0' && c <= '9') || c == '_' || c == '.'
		               || c == '-';
		s.name[n] = safe ? c : '_';
	}
	s.name[n] = '\0';
	++s_next_seq;

	xSemaphoreGive(m);
}

std::uint32_t trace_ring_next_seq()
{
	return s_next_seq;
}

std::string trace_ring_json(std::uint32_t since, std::size_t max_samples,
                            std::uint32_t &out_next)
{
	std::string out = "[";

	SemaphoreHandle_t m = lock();
	if (!m || xSemaphoreTake(m, pdMS_TO_TICKS(50)) != pdTRUE) {
		out_next = since;
		return "[]";
	}

	const std::uint32_t newest = s_next_seq;
	const std::uint32_t oldest = (newest > CAPACITY) ? newest - CAPACITY : 0;

	std::uint32_t seq = (since < oldest) ? oldest : since;
	std::size_t   n   = 0;

	for (; seq < newest && n < max_samples; ++seq, ++n) {
		const Sample &s = s_ring[seq % CAPACITY];
		if (s.seq != seq) continue;          // overwritten mid-read

		if (n) out += ',';
		out += "{\"t\":";
		out += std::to_string(s.t_ms);
		out += ",\"n\":\"";
		out += s.name;                        // sanitised at write; safe to inline
		out += "\",\"v\":";
		append_float(out, s.value);
		out += '}';
	}

	out_next = seq;
	xSemaphoreGive(m);

	out += ']';
	return out;
}

}  // namespace util
