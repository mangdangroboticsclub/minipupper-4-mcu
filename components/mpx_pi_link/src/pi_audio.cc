/* pi_audio.cc - see pi_audio.h. Ported from mpv2; only the includes and the volume calls differ. */
#include "pi_audio.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "mpx_audio.h"

namespace pi_audio {
namespace {

const char *TAG = "pi_audio";

constexpr uint32_t RATE          = 16000;
constexpr size_t   RING_BYTES    = RATE * 2 * 2;          // 2 s of s16 mono
constexpr size_t   PRIME_BYTES   = RATE * 2 * 15 / 100;   // start after 150 ms
constexpr size_t   CHUNK_FRAMES  = 320;                   // 20 ms per write
constexpr int      GIVE_UP_CHUNKS = 25;                   // 0.5 s of underrun -> stop

enum class St : uint8_t { Idle, Priming, Playing };

uint8_t          *s_ring = nullptr;
size_t            s_rd = 0, s_wr = 0, s_fill = 0;
SemaphoreHandle_t s_lock = nullptr;
TaskHandle_t      s_task = nullptr;

std::atomic<St>   s_state{St::Idle};
std::atomic<bool> s_req_stop{false};
std::atomic<bool> s_req_end{false};
std::atomic<int>  s_volume{80};
std::atomic<uint32_t> s_underruns{0};
std::atomic<uint32_t> s_dropped{0};
int               s_saved_out_volume = -1;

size_t ring_take(uint8_t *dst, size_t want)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = want < s_fill ? want : s_fill;
    for (size_t done = 0; done < n;) {
        const size_t run = std::min(n - done, RING_BYTES - s_rd);
        std::memcpy(dst + done, s_ring + s_rd, run);
        s_rd = (s_rd + run) % RING_BYTES;
        done += run;
    }
    s_fill -= n;
    xSemaphoreGive(s_lock);
    return n;
}

void ring_clear()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rd = s_wr = s_fill = 0;
    xSemaphoreGive(s_lock);
}

size_t ring_fill()
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t f = s_fill;
    xSemaphoreGive(s_lock);
    return f;
}

void output_open()
{
    // Music volume is absolute: run the shared output at 100 % and scale here.
    // _transient: this task's stack is in PSRAM, and the user's saved volume
    // must not change because a song played (mpv2's set_volume did not save;
    // this firmware's does).
    s_saved_out_volume = mpx_audio_get_volume();
    mpx_audio_set_volume_transient(100);
    if (mpx_audio_output_start(RATE) != ESP_OK) ESP_LOGW(TAG, "speaker would not open");
}

void output_close()
{
    mpx_audio_output_stop();
    if (s_saved_out_volume >= 0) mpx_audio_set_volume_transient(s_saved_out_volume);
    s_saved_out_volume = -1;
}

void player_task(void *)
{
    static int16_t pcm[CHUNK_FRAMES];
    int starved = 0;
    for (;;) {
        const St st = s_state.load();
        if (s_req_stop.exchange(false)) {
            if (st == St::Playing) output_close();
            ring_clear();
            s_req_end = false;
            s_state = St::Idle;
            ESP_LOGI(TAG, "stopped");
            continue;
        }
        if (st == St::Idle) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            continue;
        }
        if (st == St::Priming) {
            if (ring_fill() >= PRIME_BYTES || (s_req_end && ring_fill() > 0)) {
                output_open();
                starved = 0;
                s_state = St::Playing;
                ESP_LOGI(TAG, "playing");
            } else {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            }
            continue;
        }
        // Playing: the I2S write blocks, which paces this loop.
        size_t got = ring_take(reinterpret_cast<uint8_t *>(pcm), sizeof(pcm)) / 2;
        if (got == 0) {
            if (s_req_end) {                       // song finished
                output_close();
                s_req_end = false;
                s_state = St::Idle;
                ESP_LOGI(TAG, "finished");
                continue;
            }
            if (++starved > GIVE_UP_CHUNKS) {      // the Pi went quiet
                output_close();
                s_state = St::Idle;
                ESP_LOGW(TAG, "no audio from the Pi for 0.5 s - speaker off");
                continue;
            }
            s_underruns++;
            std::memset(pcm, 0, sizeof(pcm));
            got = CHUNK_FRAMES;
        } else {
            starved = 0;
            const int vol = s_volume.load();
            for (size_t i = 0; i < got; ++i) pcm[i] = static_cast<int16_t>(pcm[i] * vol / 100);
        }
        mpx_audio_output_write(pcm, got);
    }
}

bool ensure_task()
{
    if (s_task) return true;
    if (!mpx_audio_have_speaker()) return false;
    s_ring = static_cast<uint8_t *>(heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_lock = xSemaphoreCreateMutex();
    if (!s_ring || !s_lock) return false;
    // PSRAM stack: only I2S writes and the ring, no flash access.
    if (xTaskCreatePinnedToCoreWithCaps(player_task, "pi_audio", 4096, nullptr, 6, &s_task, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = nullptr;
        return false;
    }
    return true;
}

}  // namespace

bool start(uint16_t sample_rate, uint8_t volume)
{
    if (sample_rate != RATE) return false;
    if (!ensure_task()) return false;
    if (s_state.load() != St::Idle) {               // a new song replaces the old one
        s_req_stop = true;
        xTaskNotifyGive(s_task);
        for (int i = 0; i < 50 && s_state.load() != St::Idle; ++i) vTaskDelay(pdMS_TO_TICKS(2));
    }
    ring_clear();
    s_req_end = false;
    if (volume <= 100) s_volume = volume;
    s_state = St::Priming;
    xTaskNotifyGive(s_task);
    return true;
}

void push(const uint8_t *pcm, size_t len)
{
    if (!s_task || s_state.load() == St::Idle || s_req_end) return;
    len &= ~static_cast<size_t>(1);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t space = RING_BYTES - s_fill;
    const size_t n = len < space ? len : space;
    for (size_t done = 0; done < n;) {
        const size_t run = std::min(n - done, RING_BYTES - s_wr);
        std::memcpy(s_ring + s_wr, pcm + done, run);
        s_wr = (s_wr + run) % RING_BYTES;
        done += run;
    }
    s_fill += n;
    xSemaphoreGive(s_lock);
    if (n < len) s_dropped += static_cast<uint32_t>(len - n);
    xTaskNotifyGive(s_task);
}

void end()
{
    if (!s_task) return;
    s_req_end = true;
    xTaskNotifyGive(s_task);
}

void stop()
{
    if (!s_task) return;
    s_req_stop = true;
    xTaskNotifyGive(s_task);
}

void set_volume(uint8_t volume)
{
    if (volume <= 100) s_volume = volume;
}

Stats stats()
{
    Stats s{};
    s.playing = s_state.load() == St::Playing;
    s.buffered_ms = s_task ? static_cast<uint32_t>(ring_fill() * 1000 / (RATE * 2)) : 0;
    s.underruns = s_underruns.load();
    s.dropped_bytes = s_dropped.load();
    return s;
}

}  // namespace pi_audio
