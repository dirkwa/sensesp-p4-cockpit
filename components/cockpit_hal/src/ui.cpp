/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "cockpit_hal/ui.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char* TAG = "ui";

namespace cockpit_hal {
namespace ui {

namespace {

DisplayDriver* s_display = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_lvgl_lock = nullptr;   // recursive: LVGL calls from other tasks
SemaphoreHandle_t s_started = nullptr;
std::mutex s_queue_mutex;
std::deque<std::function<void()>> s_queue;
std::atomic<uint32_t> s_heartbeat{0};
uint32_t s_next_handle = 1;
std::map<uint32_t, lv_timer_t*> s_timers;

void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
  if (s_display) {
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;
    int disp_w = s_display->width();
    int disp_h = s_display->height();
    // The panel is mounted upside down: rotate the strip 180° in place.
    uint16_t* px = reinterpret_cast<uint16_t*>(px_map);
    int total = w * h;
    for (int i = 0; i < total / 2; i++) {
      uint16_t t = px[i];
      px[i] = px[total - 1 - i];
      px[total - 1 - i] = t;
    }
    s_display->flush(disp_w - (area->x1 + w), disp_h - (area->y1 + h), w, h, px_map);
    // draw_bitmap is asynchronous: hold the buffer until the DMA copy is done.
    s_display->wait_flush_done();
  }
  lv_display_flush_ready(disp);
}

void indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
  auto* touch = static_cast<TouchDriver*>(lv_indev_get_user_data(indev));
  auto pt = touch->read();
  data->point.x = pt.x;
  data->point.y = pt.y;
  data->state = pt.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void tick_cb(void*) { lv_tick_inc(1); }

void drain_queue() {
  for (;;) {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> g(s_queue_mutex);
      if (s_queue.empty()) return;
      fn = std::move(s_queue.front());
      s_queue.pop_front();
    }
    fn();
  }
}

void ui_task(void*) {
  uint32_t last_hb = 0;
  xSemaphoreGive(s_started);
  for (;;) {
    xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
    drain_queue();
    uint32_t wait = lv_timer_handler();
    xSemaphoreGiveRecursive(s_lvgl_lock);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (now - last_hb >= 250) {
      s_heartbeat.fetch_add(1);
      last_hb = now;
    }
    if (wait > 20) wait = 20;   // stay responsive to post()
    if (wait == 0) wait = 1;
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

}  // namespace

void start(DisplayDriver* display, TouchDriver* touch) {
  s_display = display;
  s_lvgl_lock = xSemaphoreCreateRecursiveMutex();
  s_started = xSemaphoreCreateBinary();
  display->init();
  touch->init();
  lv_init();
  ESP_LOGI(TAG, "LVGL %d.%d.%d", lv_version_major(), lv_version_minor(), lv_version_patch());

  lv_display_t* d = lv_display_create(display->width(), display->height());
  lv_display_set_flush_cb(d, flush_cb);
  size_t buf_size = display->width() * (display->height() / 10) * sizeof(uint16_t);
  void* b1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
  void* b2 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
  lv_display_set_buffers(d, b1, b2, buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

  lv_indev_t* in = lv_indev_create();
  lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(in, indev_read_cb);
  lv_indev_set_user_data(in, touch);

  esp_timer_handle_t tick;
  esp_timer_create_args_t ta = {};
  ta.callback = tick_cb;
  ta.dispatch_method = ESP_TIMER_TASK;
  ta.name = "lv_tick";
  ESP_ERROR_CHECK(esp_timer_create(&ta, &tick));
  ESP_ERROR_CHECK(esp_timer_start_periodic(tick, 1000));

  // Big stack: widget_factory builds whole layouts on this thread; PSRAM-safe
  // stacks are not (flash ops from LVGL fonts), so internal RAM.
  xTaskCreatePinnedToCore(ui_task, "ui", 16384, nullptr, 5, &s_task, 1);
  xSemaphoreTake(s_started, portMAX_DELAY);
  ESP_LOGI(TAG, "ui task running (%dx%d)", display->width(), display->height());
}

void post(std::function<void()> fn) {
  if (on_ui_thread()) {
    fn();
    return;
  }
  std::lock_guard<std::mutex> g(s_queue_mutex);
  s_queue.push_back(std::move(fn));
}

namespace {
struct TimerCtx {
  std::function<void()> fn;
  uint32_t handle;
  bool once;
};
void timer_cb(lv_timer_t* t) {
  auto* ctx = static_cast<TimerCtx*>(lv_timer_get_user_data(t));
  const uint32_t handle = ctx->handle;
  const bool once = ctx->once;
  ctx->fn();
  // fn may have called cancel(handle) — then ctx is gone; check the map.
  auto it = s_timers.find(handle);
  if (it == s_timers.end()) return;
  if (once) {
    s_timers.erase(it);
    lv_timer_delete(t);
    delete ctx;
  }
}
uint32_t make_timer(uint32_t ms, std::function<void()> fn, bool once) {
  auto* ctx = new TimerCtx{std::move(fn), 0, once};
  uint32_t h = 0;
  auto create = [&] {
    // 0 is the callers' "no timer" sentinel, and after a counter wrap a
    // recycled handle could still be live in the map — take the next free
    // nonzero one so cancel() can never target the wrong timer.
    do {
      if (s_next_handle == 0) s_next_handle = 1;
      ctx->handle = s_next_handle++;
    } while (s_timers.count(ctx->handle) != 0);
    lv_timer_t* t = lv_timer_create(timer_cb, ms ? ms : 1, ctx);
    if (once) lv_timer_set_repeat_count(t, 1);
    s_timers[ctx->handle] = t;
    h = ctx->handle;
  };
  if (on_ui_thread()) {
    create();
  } else {
    // creating LVGL timers must happen on the UI thread; hand it over and
    // reserve the handle synchronously so callers can cancel later
    ctx->handle = 0;
    xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
    create();
    xSemaphoreGiveRecursive(s_lvgl_lock);
  }
  return h;
}
}  // namespace

void after(uint32_t ms, std::function<void()> fn) { make_timer(ms, std::move(fn), true); }
uint32_t every(uint32_t ms, std::function<void()> fn) { return make_timer(ms, std::move(fn), false); }

void cancel(uint32_t handle) {
  xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY);
  auto it = s_timers.find(handle);
  if (it != s_timers.end()) {
    auto* ctx = static_cast<TimerCtx*>(lv_timer_get_user_data(it->second));
    lv_timer_delete(it->second);
    delete ctx;
    s_timers.erase(it);
  }
  xSemaphoreGiveRecursive(s_lvgl_lock);
}

bool on_ui_thread() { return xTaskGetCurrentTaskHandle() == s_task; }
void lock() { xSemaphoreTakeRecursive(s_lvgl_lock, portMAX_DELAY); }
void unlock() { xSemaphoreGiveRecursive(s_lvgl_lock); }
DisplayDriver* display() { return s_display; }
uint32_t heartbeat() { return s_heartbeat.load(); }

}  // namespace ui
}  // namespace cockpit_hal
