#include "infohub/ui_shell.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "driver/gpio.h"
#include "esp_lv_adapter.h"
#include "esp_log.h"
#include "infohub/board_config.hpp"

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#else
#error "Unknown InfoHub hardware variant"
#endif

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.ui";
}  // namespace

void UiShell::register_page_slot(int index, lv_obj_t* const* object, const bool* enabled_flag) {
  if (index < 0 || index >= static_cast<int>(page_slots_.size())) {
    return;
  }
  page_slots_[index] = {object, enabled_flag};
}

bool UiShell::page_enabled(int page) const {
  if (page < 0 || page >= static_cast<int>(page_slots_.size())) {
    return false;
  }
  const PageSlot& slot = page_slots_[page];
  return slot.enabled_flag == nullptr || *slot.enabled_flag;
}

lv_obj_t* UiShell::page_object(int page) const {
  if (page < 0 || page >= static_cast<int>(page_slots_.size())) {
    return nullptr;
  }
  const PageSlot& slot = page_slots_[page];
  return slot.object != nullptr ? *slot.object : nullptr;
}

int UiShell::next_enabled_page(int page, int direction) const {
  int candidate = page + direction;
  while (candidate >= 0 && candidate <= last_page_index()) {
    if (page_enabled(candidate)) {
      return candidate;
    }
    candidate += direction;
  }
  return page;
}

bool UiShell::any_page_enabled() const {
  for (int page = 0; page <= last_page_index(); ++page) {
    if (page_enabled(page)) {
      return true;
    }
  }
  return false;
}

int UiShell::clamp_enabled_page(int page) const {
  if (page_enabled(page)) {
    return page;
  }

  for (int candidate = 0; candidate <= last_page_index(); ++candidate) {
    if (page_enabled(candidate)) {
      return candidate;
    }
  }

  return 0;
}

void UiShell::configure_generic_page_pool(uint16_t total_pages) {
  page_slots_.resize(static_cast<size_t>(kPageIdxGenericFirst) + total_pages);
}

int UiShell::reserve_plugin_pages(const char* plugin_id, uint8_t count) {
  const int base = kPageIdxGenericFirst + next_generic_offset_;
  next_generic_offset_ = static_cast<uint16_t>(next_generic_offset_ + count);
  if (plugin_page_range_count_ < plugin_page_ranges_.size()) {
    plugin_page_ranges_[plugin_page_range_count_++] = {plugin_id, base, count};
  }
  return base;
}

bool UiShell::plugin_page_range(const char* plugin_id, int* base, uint8_t* count) const {
  for (uint8_t i = 0; i < plugin_page_range_count_; ++i) {
    if (std::strcmp(plugin_page_ranges_[i].id, plugin_id) == 0) {
      if (base != nullptr) {
        *base = plugin_page_ranges_[i].base;
      }
      if (count != nullptr) {
        *count = plugin_page_ranges_[i].count;
      }
      return true;
    }
  }
  return false;
}

int UiShell::nearest_enabled_page_for_scroll(int active_page) const {
  if (pager_ == nullptr) {
    return clamp_enabled_page(active_page);
  }
  lv_obj_update_layout(pager_);
  int scroll_x = lv_obj_get_scroll_x(pager_);
  if (scroll_x < 0) {
    scroll_x = -scroll_x;
  }

  // With LV_SCROLL_SNAP_NONE + scroll_throw=90, the throw decays in ~3 ticks so
  // scroll_x already reflects the post-throw resting position when SCROLL_END fires.
  // Simply pick whichever page center is closest to the viewport center.
  const int viewport_center = scroll_x + (board::kDisplayWidth / 2);
  int best_page = clamp_enabled_page(active_page);
  int best_distance = INT32_MAX;

  for (int page = 0; page <= last_page_index(); ++page) {
    if (!page_enabled(page)) {
      continue;
    }

    lv_obj_t* object = page_object(page);
    if (object == nullptr) {
      continue;
    }

    const int page_center = lv_obj_get_x(object) + (board::kDisplayWidth / 2);
    const int distance = std::abs(page_center - viewport_center);
    if (distance < best_distance) {
      best_distance = distance;
      best_page = page;
    }
  }

  return best_page;
}

void UiShell::set_pager_scroll_locked(bool locked) {
  if (pager_ == nullptr || pager_scroll_locked_ == locked) {
    return;
  }

  pager_scroll_locked_ = locked;
  lv_obj_set_scroll_dir(pager_, locked ? LV_DIR_NONE : LV_DIR_HOR);
}

void UiShell::reset_brightness_state() {
  user_brightness_percent_ = -1;
  applied_brightness_percent_ = -1;
  screen_power_mode_ = ScreenPowerMode::kAwake;
  last_activity_tick_ms_.store(lv_tick_get());
}

void UiShell::set_brightness_percent(int brightness_percent) {
  const int clamped = std::clamp(brightness_percent, 0, 100);
  if (user_brightness_percent_ == clamped) {
    return;
  }

  user_brightness_percent_ = clamped;
  apply_brightness_policy();
}

void UiShell::note_activity(bool wake_display_now) {
  last_activity_tick_ms_.store(lv_tick_get());
  if (wake_display_now) {
    wake_display();
  }
}

void UiShell::wake_display() {
  if (screen_power_mode_ == ScreenPowerMode::kAwake) {
    return;
  }

  const bool was_off = screen_power_mode_ == ScreenPowerMode::kOff;
  screen_power_mode_ = ScreenPowerMode::kAwake;
  apply_brightness_policy();
  if (was_off) {
    esp_lv_adapter_resume();
  }
}

void UiShell::apply_brightness_policy() {
  int target_brightness = user_brightness_percent_;
  if (screen_power_mode_ == ScreenPowerMode::kDimmed) {
    if (battery_display_policy_.dim_brightness_percent > 0) {
      target_brightness = std::clamp(battery_display_policy_.dim_brightness_percent, 1, 100);
    } else {
      target_brightness = std::max(8, std::min(18, std::max(1, user_brightness_percent_ / 3)));
    }
  } else if (screen_power_mode_ == ScreenPowerMode::kOff) {
    target_brightness = 0;
  }

  if (applied_brightness_percent_ == target_brightness) {
    return;
  }

  applied_brightness_percent_ = target_brightness;
  bsp_display_brightness_set(target_brightness);
}

void UiShell::update_power_save(bool on_battery, bool keep_awake, bool print_active) {
  const uint32_t now = lv_tick_get();
  const uint32_t idle_ms = now - last_activity_tick_ms_.load();

  // While the LVGL worker is paused (screen off), LVGL touch events are not
  // processed.  Poll the raw touch-interrupt GPIO so a finger press can still
  // wake the display.  CST9217 pulls INT (GPIO 11) low on contact.
  if (screen_power_mode_ == ScreenPowerMode::kOff &&
      gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
    note_activity(true);  // updates last_activity_tick_ms_ + calls wake_display()
    return;               // re-evaluate on next call with fresh idle_ms
  }

  // print_active only selects the "during print" timeouts — it must NOT
  // suppress dimming/screen-off (v1.6 regression: print_active was folded
  // into keep_awake, which made the *_active_s policy timeouts dead code).
  const uint32_t dim_timeout = (print_active
      ? battery_display_policy_.dim_timeout_active_s
      : battery_display_policy_.dim_timeout_idle_s) * 1000U;
  const uint32_t off_timeout = (print_active
      ? battery_display_policy_.off_timeout_active_s
      : battery_display_policy_.off_timeout_idle_s) * 1000U;

  ScreenPowerMode target_mode = ScreenPowerMode::kAwake;
  if (!keep_awake && (on_battery || battery_display_policy_.usb_power_save_enabled)) {
    if (battery_display_policy_.screen_off_enabled && idle_ms >= off_timeout) {
      target_mode = ScreenPowerMode::kOff;
    } else if (battery_display_policy_.dim_enabled && idle_ms >= dim_timeout) {
      target_mode = ScreenPowerMode::kDimmed;
    }
  }

  if (screen_power_mode_ != target_mode) {
    const bool was_off = screen_power_mode_ == ScreenPowerMode::kOff;
    const bool going_off = target_mode == ScreenPowerMode::kOff;
    screen_power_mode_ = target_mode;

    // IMPORTANT: When turning the screen off, pause the LVGL worker BEFORE
    // setting brightness to 0.  The AMOLED panel stops generating the TE
    // signal on GPIO13 at brightness 0.  If lv_timer_handler() is in the
    // flush path it will block forever on xSemaphoreTake(te_vsync_sem,
    // portMAX_DELAY).  Pausing first ensures the worker finishes its current
    // render cycle (while TE is still active) and then idles.
    //
    // If pause times out (worker stuck in flush), we must NOT kill the TE
    // signal — that would permanently deadlock the worker.  Instead, abort
    // the screen-off transition and stay in the current power mode.
    if (going_off && !was_off) {
      esp_err_t pause_ret = esp_lv_adapter_pause(1000);
      if (pause_ret != ESP_OK) {
        ESP_LOGW(kTag, "LVGL worker pause timeout — aborting screen-off to avoid TE deadlock (%s)",
                 esp_err_to_name(pause_ret));
        // Treat a failed screen-off attempt like fresh activity so we don't
        // immediately hammer pause() again on the next main-loop iteration.
        last_activity_tick_ms_.store(now);
        screen_power_mode_ = was_off ? ScreenPowerMode::kOff : ScreenPowerMode::kAwake;
        return;
      }
    }

    apply_brightness_policy();

    if (was_off && !going_off) {
      esp_lv_adapter_resume();
    }
  }
}

}  // namespace infohub
