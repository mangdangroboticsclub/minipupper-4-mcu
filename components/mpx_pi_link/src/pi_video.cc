/* pi_video.cc - see pi_video.h. Ported from mpv2; the display is a sink. */
#include "pi_video.h"

#include <atomic>
#include <cstring>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "pi_protocol.h"

namespace pi_video {
namespace {

const char *TAG = "pi_video";

uint8_t *s_asm = nullptr;          // frame being assembled (Pi link task only)
uint8_t *s_ready = nullptr;        // complete frame for the decode task
size_t   s_asm_len = 0;
size_t   s_ready_len = 0;
uint16_t s_frame_id = 0;
int      s_next = -1;              // next chunk index expected, -1 = waiting for index 0
std::atomic<bool> s_busy{false};   // the decode task owns s_ready
TaskHandle_t s_task = nullptr;
bool     s_failed = false;
esp_err_t (*volatile s_sink)(const uint8_t *, size_t) = nullptr;

std::atomic<uint32_t> s_shown{0}, s_dropped{0}, s_errors{0};

void decode_task(void *)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!s_busy.load()) continue;
        esp_err_t (*sink)(const uint8_t *, size_t) = s_sink;
        if (sink && sink(s_ready, s_ready_len) == ESP_OK) s_shown++;
        else s_errors++;
        s_busy = false;
    }
}

bool ensure()
{
    if (s_task) return true;
    if (s_failed) return false;
    s_asm = static_cast<uint8_t *>(heap_caps_malloc(PI_VIDEO_MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_ready = static_cast<uint8_t *>(heap_caps_malloc(PI_VIDEO_MAX_FRAME, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // PSRAM stack: JPEG decode + LVGL, no flash access.
    if (!s_asm || !s_ready ||
        xTaskCreatePinnedToCoreWithCaps(decode_task, "pi_video", 8192, nullptr, 3, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the camera picture");
        s_task = nullptr;
        s_failed = true;
        return false;
    }
    return true;
}

}  // namespace

void chunk(const uint8_t *payload, uint8_t len)
{
    if (len <= sizeof(pi_video_hdr_t) || !s_sink || !ensure()) return;
    pi_video_hdr_t h;
    std::memcpy(&h, payload, sizeof(h));
    const uint8_t *data = payload + sizeof(h);
    const size_t n = len - sizeof(h);
    if (h.count == 0 || h.index >= h.count) return;

    if (h.index == 0) {                     // a new frame starts
        if (s_next > 0) s_dropped++;        // the previous one never finished
        s_frame_id = h.frame_id;
        s_asm_len = 0;
        s_next = 0;
    }
    if (s_next != h.index || h.frame_id != s_frame_id || s_asm_len + n > PI_VIDEO_MAX_FRAME) {
        if (s_next >= 0) s_dropped++;       // a chunk went missing: skip to the next frame
        s_next = -1;
        return;
    }
    std::memcpy(s_asm + s_asm_len, data, n);
    s_asm_len += n;
    s_next++;
    if (h.index + 1 < h.count) return;

    s_next = -1;                            // frame complete
    if (s_busy.load()) { s_dropped++; return; }
    std::swap(s_asm, s_ready);
    s_ready_len = s_asm_len;
    s_busy = true;
    xTaskNotifyGive(s_task);
}

void set_sink(esp_err_t (*sink)(const uint8_t *jpeg, size_t len))
{
    s_sink = sink;
}

Stats stats()
{
    return Stats{ s_shown.load(), s_dropped.load(), s_errors.load() };
}

}  // namespace pi_video
