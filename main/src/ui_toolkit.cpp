#include "infohub/ui_toolkit.hpp"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#else
#error "Unknown InfoHub hardware variant"
#endif

namespace infohub {

namespace {
// Same tag as before the move (ui.cpp's "infohub.ui") — deliberately kept
// identical so on-device log filtering/grep for these lock diagnostics
// doesn't change behavior just because the code moved files.
constexpr char kTag[] = "infohub.ui";
}  // namespace

void make_transparent(lv_obj_t* obj) {
  lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
}

void enable_touch_bubble(lv_obj_t* obj) {
  if (obj == nullptr) {
    return;
  }
  lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

void set_hidden(lv_obj_t* obj, bool hidden) {
  if (obj == nullptr) {
    return;
  }

  const bool currently_hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
  if (currently_hidden == hidden) {
    return;
  }

  if (hidden) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

void set_label_text_if_changed(lv_obj_t* label, const char* text) {
  if (label == nullptr || text == nullptr) {
    return;
  }

  const char* current = lv_label_get_text(label);
  if (current != nullptr && std::strcmp(current, text) == 0) {
    return;
  }

  lv_label_set_text(label, text);
}

void set_label_text_if_changed(lv_obj_t* label, const std::string& text) {
  set_label_text_if_changed(label, text.c_str());
}

LvglLockGuard::LvglLockGuard(uint32_t timeout_ms, const char* caller) : caller_(caller) {
  const uint32_t before = esp_log_timestamp();
  locked_ = bsp_display_lock(timeout_ms) == ESP_OK;
  const uint32_t wait_ms = esp_log_timestamp() - before;
  if (!locked_) {
    ESP_LOGW(kTag, "LVGL lock FAILED after %lums (timeout=%lu, caller=%s) "
             "heap_int=%lu heap_dma=%lu",
             (unsigned long)wait_ms, (unsigned long)timeout_ms, caller_,
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DMA));
    lock_fail_count_++;
    if (lock_fail_count_ >= 5 && (lock_fail_count_ % 10) == 0) {
      ESP_LOGE(kTag, "LVGL lock failed %lu times consecutively — worker task may be stuck",
               (unsigned long)lock_fail_count_);
    }
  } else {
    acquire_seq_ = ++total_acquires_;
    previous_active_ = active_;
    active_ = this;
    if (lock_fail_count_ > 0) {
      ESP_LOGI(kTag, "LVGL lock recovered after %lu failures (wait=%lums, caller=%s)",
               (unsigned long)lock_fail_count_, (unsigned long)wait_ms, caller_);
    }
    lock_fail_count_ = 0;
    if (wait_ms > 150) {
      ESP_LOGW(kTag, "LVGL lock waited %lums (caller=%s)",
               (unsigned long)wait_ms, caller_);
    }
  }
  acquired_ts_ = esp_log_timestamp();
}

LvglLockGuard::~LvglLockGuard() {
  if (locked_) {
    const uint32_t held_ms = esp_log_timestamp() - acquired_ts_;
    const char* last_phase = phase_;
    const uint32_t since_phase = phase_ts_ != 0 ? (esp_log_timestamp() - phase_ts_) : 0;
    active_ = previous_active_;
    bsp_display_unlock();
    if (held_ms > 80) {
      ESP_LOGW(kTag,
               "LVGL lock held %lums (caller=%s seq=%lu phase=%s phase_ms=%lu)",
               (unsigned long)held_ms, caller_, (unsigned long)acquire_seq_,
               last_phase != nullptr ? last_phase : "-",
               (unsigned long)since_phase);
    }
  }
}

void LvglLockGuard::mark_phase(const char* phase) {
  phase_ = phase;
  phase_ts_ = esp_log_timestamp();
}

void LvglLockGuard::note_phase(const char* phase) {
  if (active_ != nullptr) {
    active_->mark_phase(phase);
  }
}

uint32_t LvglLockGuard::lock_fail_count_ = 0;
uint32_t LvglLockGuard::total_acquires_ = 0;
thread_local LvglLockGuard* LvglLockGuard::active_ = nullptr;

}  // namespace infohub
