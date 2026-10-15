#include "log_ring.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace util {
namespace {

/* A fixed ring of fixed-width slots rather than a byte ring with framing.
 * Log lines are short and bounded, slots make "give me everything after
 * sequence N" O(1), and there is no allocation on the logging path — which
 * matters because this runs inside ESP_LOGx, including the ESP_LOGE that
 * fires when the heap is exhausted.
 */
// NOT `LINE_MAX`: that is a POSIX macro from <limits.h>, which FreeRTOS.h
// pulls in, so the name expands to a number before the compiler ever sees
// a declaration.
constexpr std::size_t LOG_LINE_MAX = 160;   // longer lines are truncated
constexpr std::size_t SLOTS     = 104;   // ~16 KB total

struct Slot {
    uint32_t seq;
    uint16_t len;
    char     text[LOG_LINE_MAX];
};

Slot              s_ring[SLOTS];
uint32_t          s_next_seq  = 0;       // sequence of the next line written
SemaphoreHandle_t s_lock      = nullptr;
vprintf_like_t    s_chain     = nullptr; // the original sink (UART)
bool             s_installed = false;

/* Partial-line accumulator. ESP_LOGx emits a whole line per call in practice,
 * but printf() from application code does not, so buffer until a newline. */
char        s_pending[LOG_LINE_MAX];
std::size_t s_pending_len = 0;

void push_line(const char *text, std::size_t len)
{
    if (len == 0) return;
    if (len > LOG_LINE_MAX) len = LOG_LINE_MAX;

    Slot &s = s_ring[s_next_seq % SLOTS];
    s.seq = s_next_seq;
    s.len = static_cast<uint16_t>(len);
    std::memcpy(s.text, text, len);
    ++s_next_seq;
}

int capture_vprintf(const char *fmt, va_list args)
{
    // Format once into a stack buffer. va_list can only be walked once, so
    // the chained sink gets the formatted result rather than the original
    // args — which is why we write to it with fputs, not vprintf.
    char line[LOG_LINE_MAX + 1];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);

    if (n < 0) return n;
    std::size_t got = (static_cast<std::size_t>(n) < sizeof(line))
                          ? static_cast<std::size_t>(n) : sizeof(line) - 1;

    // Keep the serial console working exactly as before.
    if (s_chain) {
        va_list c2;
        va_copy(c2, args);
        s_chain(fmt, c2);
        va_end(c2);
    }

    // The lock is only taken for the ring, never around the chained sink, so
    // a slow UART cannot serialise every logging task in the system.
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) return n;

    for (std::size_t i = 0; i < got; ++i) {
        const char c = line[i];
        if (c == '\n' || s_pending_len == LOG_LINE_MAX) {
            push_line(s_pending, s_pending_len);
            s_pending_len = 0;
            if (c == '\n') continue;
        }
        if (c == '\r') continue;
        s_pending[s_pending_len++] = c;
    }

    // A line longer than the slot loses its newline to vsnprintf's truncation,
    // so the loop above never sees the terminator that would flush it. Without
    // this it would sit in the pending buffer until some unrelated later line
    // pushed it out — which is exactly the case where you most want the log:
    // a long message is usually a stack dump or an error detail.
    if (s_pending_len == LOG_LINE_MAX) {
        push_line(s_pending, s_pending_len);
        s_pending_len = 0;
    }

    xSemaphoreGive(s_lock);
    return n;
}

/* Local copy rather than reusing http_server.cc's json_escape: this module
 * must not depend on the web server, and the escaping needed here is small. */
void append_escaped(std::string &out, const char *p, std::size_t len)
{
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(p[i]);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\t': out += "\\t";  break;
            case '\r': out += "\\r";  break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", c);
                    out += b;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

}  // namespace

void log_ring_init()
{
    if (s_installed) return;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;                       // no lock, no capture — but boot on
    s_chain = esp_log_set_vprintf(capture_vprintf);
    s_installed = true;
    ESP_LOGI("log_ring", "Log capture active (%u slots x %u B) — GET /v1/logs",
             static_cast<unsigned>(SLOTS), static_cast<unsigned>(LOG_LINE_MAX));
}

uint32_t log_ring_next_seq()
{
    return s_next_seq;
}

std::string log_ring_json(uint32_t since, std::size_t max_lines,
                          uint32_t &out_next)
{
    std::string out = "[";
    if (!s_lock) { out_next = 0; return out + "]"; }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        out_next = since;
        return out + "]";
    }

    const uint32_t newest = s_next_seq;
    // Oldest sequence still held. Unsigned arithmetic, so guard the
    // under-SLOTS case explicitly rather than relying on wraparound.
    const uint32_t oldest = (newest > SLOTS) ? (newest - SLOTS) : 0;

    // A caller that fell behind gets a gap, not an error.
    uint32_t seq = (since < oldest) ? oldest : since;
    if (seq > newest) seq = newest;              // clock went backwards; resync

    std::size_t emitted = 0;
    bool first = true;
    for (; seq < newest && emitted < max_lines; ++seq, ++emitted) {
        const Slot &s = s_ring[seq % SLOTS];
        if (s.seq != seq) continue;              // overwritten mid-read
        if (!first) out += ',';
        first = false;
        out += '"';
        append_escaped(out, s.text, s.len);
        out += '"';
    }
    out_next = seq;

    xSemaphoreGive(s_lock);
    return out + "]";
}

}  // namespace util
