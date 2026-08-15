#include "infohub/ui.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <string>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "infohub/board_config.hpp"
#include "infohub/pmu.hpp"
#include "infohub/ui_toolkit.hpp"

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#else
#error "Unknown InfoHub hardware variant"
#endif

extern "C" {
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_40;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t mdi_30;
}

namespace infohub {

namespace {

constexpr char kTag[] = "infohub.ui";
constexpr int kDefaultBrightnessPercent = 80;
constexpr int kRingStrokeWidth = 22;
// Was 24 — raised after confirming via on-device debug logging that natural
// hand tremor during a ~1s hold-for-PIN long-press was crossing this
// threshold, flipping the gesture into brightness-drag mode
// (overlay_visible_=true), which silently vetoes the long-press unlock
// request. This threshold gates both horizontal-swipe and vertical-
// brightness-drag commitment, so a real, deliberate swipe/drag still
// resolves quickly — only accidental jitter during a stationary hold is now
// tolerated.
constexpr int kSwipeThresholdPx = 40;
constexpr int kGestureAxisLockMarginPx = 16;
constexpr int kBrightnessHorizontalTolerancePx = 18;
constexpr int kRotatedVisualOffsetX = 0;
constexpr int kRotatedVisualOffsetY = 0;
constexpr int kManualMinBrightnessPercent = 4;
constexpr uint8_t kRingPulseDepthPercent = 55U;
constexpr int32_t kParallaxTitleMaxY = -18;
constexpr int32_t kParallaxCardsMaxY = -8;
constexpr uint32_t kBatteryDimTimeoutIdleMs = 20000U;
constexpr uint32_t kBatteryOffTimeoutIdleMs = 60000U;
constexpr uint32_t kBatteryDimTimeoutActiveMs = 30000U;
constexpr uint32_t kBatteryOffTimeoutActiveMs = 120000U;
constexpr uint64_t kPortalHintIntroMs = 5ULL * 60ULL * 1000ULL;
constexpr uint32_t kRingBaseDark = 0x101010;
constexpr uint32_t kRingIdleSolid = 0x404040;

// Battery overlay glyphs (MDI font codepoints) + text formatting — core
// (PmuManager-driven, not printer-specific), moved back here from
// printer_plugin_ui.cpp so Application can refresh the overlay every loop
// iteration independent of any plugin. See update_battery_overlay() below.
constexpr char kMdiBatteryCharging0[] = "\xF3\xB0\xA0\x92";
constexpr char kMdiBatteryCharging10[] = "\xF3\xB0\xA0\x88";
constexpr char kMdiBatteryCharging20[] = "\xF3\xB0\xA0\x89";
constexpr char kMdiBatteryCharging30[] = "\xF3\xB0\xA0\x8A";
constexpr char kMdiBatteryCharging40[] = "\xF3\xB0\xA0\x8B";
constexpr char kMdiBatteryCharging50[] = "\xF3\xB0\xA0\x8C";
constexpr char kMdiBatteryCharging60[] = "\xF3\xB0\xA0\x8D";
constexpr char kMdiBatteryCharging70[] = "\xF3\xB0\xA0\x8E";
constexpr char kMdiBatteryCharging80[] = "\xF3\xB0\xA0\x8F";
constexpr char kMdiBatteryCharging90[] = "\xF3\xB0\xA0\x90";
constexpr char kMdiBatteryCharging100[] = "\xF3\xB0\xA0\x87";
constexpr char kMdiBatteryAlert[] = "\xF3\xB1\x83\x8D";
constexpr char kMdiBattery10[] = "\xF3\xB0\x81\xBA";
constexpr char kMdiBattery20[] = "\xF3\xB0\x81\xBB";
constexpr char kMdiBattery30[] = "\xF3\xB0\x81\xBC";
constexpr char kMdiBattery40[] = "\xF3\xB0\x81\xBD";
constexpr char kMdiBattery50[] = "\xF3\xB0\x81\xBE";
constexpr char kMdiBattery60[] = "\xF3\xB0\x81\xBF";
constexpr char kMdiBattery70[] = "\xF3\xB0\x82\x80";
constexpr char kMdiBattery80[] = "\xF3\xB0\x82\x81";
constexpr char kMdiBattery90[] = "\xF3\xB0\x82\x82";
constexpr char kMdiBattery100[] = "\xF3\xB0\x81\xB9";

const char* mdi_battery_symbol(const PowerSnapshot& power) {
  if (!power.usb_present && power.battery_percent == 0U && !power.charging) {
    return kMdiBatteryAlert;
  }

  if (power.charging) {
    if (power.battery_percent >= 95U) return kMdiBatteryCharging100;
    if (power.battery_percent >= 85U) return kMdiBatteryCharging90;
    if (power.battery_percent >= 75U) return kMdiBatteryCharging80;
    if (power.battery_percent >= 65U) return kMdiBatteryCharging70;
    if (power.battery_percent >= 55U) return kMdiBatteryCharging60;
    if (power.battery_percent >= 45U) return kMdiBatteryCharging50;
    if (power.battery_percent >= 35U) return kMdiBatteryCharging40;
    if (power.battery_percent >= 25U) return kMdiBatteryCharging30;
    if (power.battery_percent >= 15U) return kMdiBatteryCharging20;
    if (power.battery_percent >= 5U) return kMdiBatteryCharging10;
    return kMdiBatteryCharging0;
  }

  if (power.battery_percent >= 95U) return kMdiBattery100;
  if (power.battery_percent >= 85U) return kMdiBattery90;
  if (power.battery_percent >= 75U) return kMdiBattery80;
  if (power.battery_percent >= 65U) return kMdiBattery70;
  if (power.battery_percent >= 55U) return kMdiBattery60;
  if (power.battery_percent >= 45U) return kMdiBattery50;
  if (power.battery_percent >= 35U) return kMdiBattery40;
  if (power.battery_percent >= 25U) return kMdiBattery30;
  if (power.battery_percent >= 15U) return kMdiBattery20;
  return kMdiBattery10;
}

std::string battery_icon_text(const PowerSnapshot& power) {
  return mdi_battery_symbol(power);
}

std::string battery_pct_text(const PowerSnapshot& power) {
  if (!power.usb_present && power.battery_percent == 0U && !power.charging) {
    return "--%";
  }
  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "%u%%", power.battery_percent);
  return buffer;
}

bsp_display_rotation_t bsp_rotation_for(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
      return BSP_DISPLAY_ROTATE_90;
    case DisplayRotation::k180:
      return BSP_DISPLAY_ROTATE_180;
    case DisplayRotation::k270:
      return BSP_DISPLAY_ROTATE_270;
    case DisplayRotation::k0:
    default:
      return BSP_DISPLAY_ROTATE_0;
  }
}

void apply_touch_rotation_flags(DisplayRotation rotation, bsp_display_cfg_t* cfg) {
  if (cfg == nullptr) {
    return;
  }

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
    switch (rotation) {
      case DisplayRotation::k90:
        cfg->touch_flags.swap_xy = 1;
        cfg->touch_flags.mirror_x = 0;
        cfg->touch_flags.mirror_y = 1;
        break;
      case DisplayRotation::k180:
        cfg->touch_flags.swap_xy = 0;
        cfg->touch_flags.mirror_x = 0;
        cfg->touch_flags.mirror_y = 0;
        break;
      case DisplayRotation::k270:
        cfg->touch_flags.swap_xy = 1;
        cfg->touch_flags.mirror_x = 1;
        cfg->touch_flags.mirror_y = 0;
        break;
      case DisplayRotation::k0:
      default:
        cfg->touch_flags.swap_xy = 0;
        cfg->touch_flags.mirror_x = 1;
        cfg->touch_flags.mirror_y = 1;
        break;
    }
#else
    switch (rotation) {
      case DisplayRotation::k90:
        cfg->touch_flags.swap_xy = 1;
        cfg->touch_flags.mirror_x = 0;
        cfg->touch_flags.mirror_y = 1;
        break;
      case DisplayRotation::k180:
        cfg->touch_flags.swap_xy = 0;
        cfg->touch_flags.mirror_x = 1;
        cfg->touch_flags.mirror_y = 1;
        break;
      case DisplayRotation::k270:
        cfg->touch_flags.swap_xy = 1;
        cfg->touch_flags.mirror_x = 1;
        cfg->touch_flags.mirror_y = 0;
        break;
      case DisplayRotation::k0:
      default:
        cfg->touch_flags.swap_xy = 0;
        cfg->touch_flags.mirror_x = 0;
        cfg->touch_flags.mirror_y = 0;
        break;
    }
#endif
}

int display_rotation_visual_offset_x(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
    case DisplayRotation::k270:
      return kRotatedVisualOffsetX;
    case DisplayRotation::k0:
    case DisplayRotation::k180:
    default:
      return 0;
  }
}

int display_rotation_visual_offset_y(DisplayRotation rotation) {
  switch (rotation) {
    case DisplayRotation::k90:
    case DisplayRotation::k270:
      return kRotatedVisualOffsetY;
    case DisplayRotation::k0:
    case DisplayRotation::k180:
    default:
      return 0;
  }
}

void apply_display_rotation_visual_offset(lv_obj_t* obj, DisplayRotation rotation) {
  if (obj == nullptr) {
    return;
  }

  lv_obj_set_style_translate_x(obj, display_rotation_visual_offset_x(rotation), 0);
  lv_obj_set_style_translate_y(obj, display_rotation_visual_offset_y(rotation), 0);
}

// base_deci_deg is the user-tunable "mounting tilt" offset, in tenths of a
// degree, set from the web setup portal's Screen Rotation panel. k270 gets
// the mirrored sign since it's the same panel viewed from the opposite edge.
int32_t display_rotation_tilt_correction_decideg(DisplayRotation rotation, int32_t base_deci_deg) {
  switch (rotation) {
    case DisplayRotation::k90:
      return base_deci_deg;
    case DisplayRotation::k270:
      return -base_deci_deg;
    case DisplayRotation::k0:
    case DisplayRotation::k180:
    default:
      return 0;
  }
}

void apply_display_rotation_tilt_correction(lv_obj_t* screen, DisplayRotation rotation,
                                             int32_t base_deci_deg) {
  if (screen == nullptr) {
    return;
  }

  lv_obj_set_style_transform_pivot_x(screen, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(screen, lv_pct(50), 0);
  lv_obj_set_style_transform_rotation(screen, display_rotation_tilt_correction_decideg(rotation, base_deci_deg), 0);
}

uint32_t scale_color(uint32_t color, uint16_t scale_0_to_255) {
  const uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
  const uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFF);
  const uint8_t b = static_cast<uint8_t>(color & 0xFF);
  const uint8_t sr =
      static_cast<uint8_t>((static_cast<uint32_t>(r) * scale_0_to_255 + 127U) / 255U);
  const uint8_t sg =
      static_cast<uint8_t>((static_cast<uint32_t>(g) * scale_0_to_255 + 127U) / 255U);
  const uint8_t sb =
      static_cast<uint8_t>((static_cast<uint32_t>(b) * scale_0_to_255 + 127U) / 255U);
  return (static_cast<uint32_t>(sr) << 16) | (static_cast<uint32_t>(sg) << 8) | sb;
}

std::string short_duration_text(uint32_t total_seconds) {
  const uint32_t minutes = total_seconds / 60U;
  const uint32_t seconds = total_seconds % 60U;
  char buffer[24] = {};
  if (minutes > 0U) {
    std::snprintf(buffer, sizeof(buffer), "%um %us", static_cast<unsigned int>(minutes),
                  static_cast<unsigned int>(seconds));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%us", static_cast<unsigned int>(seconds));
  }
  return buffer;
}

}  // namespace

void Ui::set_display_rotation(DisplayRotation rotation) {
  if (initialized_) {
    return;
  }
  display_rotation_ = rotation;
}

void Ui::set_display_tilt_deci_deg(int deci_deg) {
  if (initialized_) {
    return;
  }
  display_tilt_deci_deg_ = deci_deg;
}

esp_err_t Ui::initialize() {
  if (initialized_) {
    return ESP_OK;
  }

  portal_hint_boot_ms_ = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);

  bsp_display_cfg_t display_cfg = {
      .lv_adapter_cfg = []() {
        // Same defaults as ESP_LV_ADAPTER_DEFAULT_CONFIG(), with callbacks
        // explicitly zeroed to avoid -Wmissing-field-initializers in 0.5.3.
        return esp_lv_adapter_config_t{
            .task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE,
            .task_priority = ESP_LV_ADAPTER_DEFAULT_TASK_PRIORITY,
            // Pin LVGL render task to core 1 so WiFi/BT on core 0 can't interrupt rendering.
            .task_core_id = 1,
            .tick_period_ms = ESP_LV_ADAPTER_DEFAULT_TICK_PERIOD_MS,
            .task_min_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MIN_DELAY_MS,
            .task_max_delay_ms = ESP_LV_ADAPTER_DEFAULT_TASK_MAX_DELAY_MS,
            .stack_in_psram = false,
            .auto_sleep = {
                .enable = false,
                .mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_DISABLED,
                .idle_timeout_ms = ESP_LV_ADAPTER_DEFAULT_AUTO_SLEEP_TIMEOUT_MS,
                .callbacks = {},
            },
        };
      }(),
      .rotation = ESP_LV_ADAPTER_ROTATE_0,
#if defined(INFOHUB_HW_VARIANT_LCD_2_8C)
      // The 2.8C board is an RGB panel without a TE signal. Use double full
      // buffering so the RGB driver can switch complete framebuffers instead of
      // showing LVGL's in-progress updates on screen.
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_FULL,
#else
      // AMOLED 1.75 (CO5300 QSPI) has a TE line: TE_SYNC + single PSRAM buffer
      // is the validated combo (see April 2026 notes). DOUBLE_FULL is rejected
      // by this panel path and would fall back to NONE + double buffer, which
      // starved internal DRAM and broke mbedTLS handshakes. The adapter's TE
      // wait is deadline-bounded, so a missed TE pulse can no longer deadlock
      // the LVGL worker.
      .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC,
#endif
      .touch_flags = {
          .swap_xy = 0,
          .mirror_x = 1,
          .mirror_y = 1,
      },
  };
  apply_touch_rotation_flags(display_rotation_, &display_cfg);

  display_ = bsp_display_start_with_config(&display_cfg);
  if (display_ == nullptr) {
    ESP_LOGE(kTag, "bsp_display_start failed");
    return ESP_FAIL;
  }

  ESP_RETURN_ON_ERROR(bsp_display_rotation_set(bsp_rotation_for(display_rotation_)), kTag,
                      "apply display rotation failed");

  ui_shell_.reset_brightness_state();
  ui_shell_.set_brightness_percent(kDefaultBrightnessPercent);
  ESP_RETURN_ON_ERROR(build_dashboard(), kTag, "build_dashboard failed");

  // With LV_SCROLL_SNAP_NONE the pager decelerates freely after finger release.
  // scroll_throw shrinks velocity per tick; higher values kill momentum faster
  // so SCROLL_END fires sooner and handle_pager_event() launches a single
  // ease-out snap animation to the nearest page with less risk of a competing
  // native LVGL snap animation still being in flight. 90 was tuned to
  // eliminate that race but leaves ~zero fling glide, making a quick flick
  // read as a short static drag. Trying 30 for more forgiving glide —
  // watch for double-snap/jank at page landings if this reintroduces it.
  {
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev != nullptr) {
      if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
        lv_indev_set_scroll_throw(indev, 30);
        break;
      }
      indev = lv_indev_get_next(indev);
    }
  }

  // Ambient sweep timer removed — only pulse/filament animations remain (driven by lv_anim_t).

  initialized_ = true;
  ESP_LOGI(kTag, "UI ready with YAML-style pager layout (rotation=%s)",
           to_string(display_rotation_));
  return ESP_OK;
}

void Ui::set_arc_color_scheme(const ArcColorScheme& colors) {
  if (!initialized_) {
    arc_colors_ = colors;
    return;
  }

  LvglLockGuard lock(200, "set_arc_color");
  if (!lock.locked()) {
    return;
  }

  arc_colors_ = colors;
}

bool Ui::consume_chamber_light_toggle_request() {
  return chamber_light_toggle_requested_.exchange(false);
}

PrintCommand Ui::consume_print_command_request() {
  // 0 == PrintCommand::kNone — see has_print_command_request()'s comment on
  // why this stays a bare literal instead of the enumerator name.
  const uint8_t value = print_command_request_.exchange(0);
  return static_cast<PrintCommand>(value);
}

bool Ui::consume_portal_unlock_request() {
  return portal_unlock_requested_.exchange(false);
}

void Ui::set_portal_access_state(bool lock_enabled, bool request_authorized,
                                 bool session_active, bool pin_active,
                                 const std::string& pin_code, uint32_t pin_remaining_s,
                                 uint32_t session_remaining_s) {
  if (!initialized_) {
    return;
  }

  // Store portal data without LVGL lock — Application calls
  // update_portal_access_visuals() (which does its own locking) right after
  // pushing this, so text computation and the visual update stay a single
  // logical step without holding the lock across two separate calls.
  portal_lock_enabled_ = lock_enabled;
  portal_request_authorized_ = request_authorized;
  portal_session_active_ = session_active;
  portal_pin_active_ = pin_active;
  portal_pin_code_ = pin_code;
  portal_pin_remaining_s_ = pin_remaining_s;
  portal_session_remaining_s_ = session_remaining_s;
}

void Ui::set_core_wifi_state(bool station_connected, bool setup_ap_active,
                             const std::string& station_ip) {
  core_wifi_connected_ = station_connected;
  core_setup_ap_active_ = setup_ap_active;
  core_wifi_ip_ = station_ip;
}

void Ui::update_portal_access_visuals() {
  if (!initialized_) {
    return;
  }
  LvglLockGuard lock(200, "update_portal_access_visuals");
  if (!lock.locked()) {
    return;
  }
  compute_portal_texts_locked();
}

void Ui::compute_portal_texts_locked() {
  // Plugin-agnostic on purpose — see set_core_wifi_state(). No printer-only
  // Bambu-cloud-login sub-state here anymore (that used to also suppress the
  // hint via PrinterSnapshot::connection == kWaitingForCredentials); losing
  // that narrow nicety is the deliberate tradeoff for zero PrinterSnapshot
  // dependency in core portal chrome.
  const bool provisioning_context = core_setup_ap_active_ || !core_wifi_connected_;
  const bool station_portal_available =
      !core_setup_ap_active_ && core_wifi_connected_ && !core_wifi_ip_.empty();

  if (portal_pin_active_) {
    portal_hint_text_ = portal_request_authorized_ ? "Web Config PIN active on the display"
                                                   : "Enter the PIN shown on the display";
    portal_overlay_title_text_ = "WEB CONFIG PIN";
    portal_overlay_value_text_ = portal_pin_code_;
    portal_overlay_detail_text_ =
        "Valid for " + short_duration_text(std::max<uint32_t>(portal_pin_remaining_s_, 1U)) +
        ". Enter it in the browser.";
  } else {
    portal_overlay_title_text_.clear();
    portal_overlay_value_text_.clear();
    portal_overlay_detail_text_.clear();
    if (portal_session_active_ && portal_request_authorized_) {
      portal_hint_text_ =
          "Web Config unlocked for " +
          short_duration_text(std::max<uint32_t>(portal_session_remaining_s_, 1U));
    } else if (provisioning_context) {
      portal_hint_text_.clear();
    } else if (!portal_lock_enabled_) {
      portal_hint_text_ =
          station_portal_available ? ("Open " + core_wifi_ip_) : "Web Config open";
    } else if (portal_hint_boot_ms_ != 0 &&
               static_cast<uint64_t>(esp_timer_get_time() / 1000ULL) <
                   portal_hint_boot_ms_ + kPortalHintIntroMs) {
      portal_hint_text_ = station_portal_available
                              ? ("Open " + core_wifi_ip_ + " | Hold for PIN")
                              : "Hold for PIN";
    } else {
      portal_hint_text_.clear();
    }
  }

  update_portal_access_visuals_locked();
}

// --- Page 0: printer-selector scroll chrome ---
// The page0 widgets themselves (title/card list/empty note) are owned and
// built by PrinterPlugin (see PrinterPlugin::build_screen()) — Ui only knows
// about whichever objects were registered via register_page0_fade_targets(),
// and the callback registered via register_page0_reentry_callback().

void Ui::register_page0_fade_targets(lv_obj_t* title, lv_obj_t* card_list,
                                      lv_obj_t* empty_note) {
  page0_fade_title_ = title;
  page0_fade_card_list_ = card_list;
  page0_fade_empty_note_ = empty_note;
}

void Ui::register_page0_reentry_callback(Page0ReentryCallback callback, void* user_data) {
  page0_reentry_cb_ = callback;
  page0_reentry_user_data_ = user_data;
}

void Ui::apply_page0_parallax(bool force) {
  if (page0_fade_title_ == nullptr || page0_fade_card_list_ == nullptr ||
      ui_shell_.pager() == nullptr) {
    // No printer page0 -- either the printer plugin is compiled out
    // entirely, or build_dashboard() hasn't reached page0 construction yet.
    // There's nothing to parallax-fade, but the lv_layer_top() overlay
    // (arc/progress label) has no other mechanism keeping it hidden -- it
    // defaults to fully opaque at creation (see build_dashboard()) and
    // every other path to hiding it runs through the code below, which we
    // can't reach without page0. Without this, a printer-disabled build
    // shows a permanently stuck "--%" at the top of every screen. Battery
    // labels aren't touched here -- they have their own independent
    // hidden-flag gate (set_battery_overlay_visible()) that already works
    // regardless of this function.
    if (fixed_overlay_ != nullptr) {
      set_hidden(fixed_overlay_, true);
    }
    if (progress_label_ != nullptr) {
      lv_obj_set_style_opa(progress_label_, LV_OPA_TRANSP, 0);
    }
    return;
  }

  int scroll_x = lv_obj_get_scroll_x(ui_shell_.pager());
  if (scroll_x < 0) scroll_x = 0;

  const int page_w = board::kDisplayWidth;
  const int clamped = (scroll_x > page_w) ? page_w : scroll_x;
  if (!force && clamped == last_parallax_clamped_) {
    return;
  }
  last_parallax_clamped_ = clamped;

  // Page 0 content: fade out + shift up as we scroll away
  const int32_t title_y = static_cast<int32_t>(kParallaxTitleMaxY * clamped / page_w);
  const int32_t cards_y = static_cast<int32_t>(kParallaxCardsMaxY * clamped / page_w);
  const lv_opa_t page0_opa = static_cast<lv_opa_t>(255 - 255 * clamped / page_w);

  lv_obj_set_style_translate_y(page0_fade_title_, title_y, 0);
  lv_obj_set_style_translate_y(page0_fade_card_list_, cards_y, 0);
  // NOTE: lv_obj_set_style_opa() on large containers forces LVGL to render the entire
  // widget tree into a temporary offscreen layer before blending it at the given opacity.
  // With LV_DRAW_LAYER_SIMPLE_BUF_SIZE=24KB on a 466px-wide display that means ~14 layer
  // passes for the card list alone — triggered every LV_EVENT_SCROLL. This reduces scroll
  // fps to ~4. Use only translate_y for the parallax shift; no opa on large containers.
  if (page0_fade_empty_note_ != nullptr) {
    // empty_note is a single small label — opa is cheap here
    lv_obj_set_style_opa(page0_fade_empty_note_, page0_opa, 0);
  }

  // Overlay (arc, progress, battery): fade in as we leave page 0
  const lv_opa_t overlay_opa = static_cast<lv_opa_t>(255 * clamped / page_w);

  // fixed_overlay_/status_arc_ and progress_label_ live on screen_/
  // lv_layer_top() respectively — not inside any pager page — so they're not
  // naturally clipped per-page like page content is. This parallax fade only
  // knows about the page0<->page1 boundary within the printer plugin's own
  // pages; any OTHER plugin's page (weather/stocks/...) should force both
  // hidden while settled there, on top of the normal scroll-based fade.
  // Battery-overlay-tied-to-printer-pages is a known, documented special
  // case (see CLAUDE.md's "Battery routed through PrinterSnapshot" note —
  // generalizing this is explicitly future work, not this phase's job).
  bool on_printer_page = false;
  {
    int printer_base = 0;
    uint8_t printer_count = 0;
    if (ui_shell_.plugin_page_range("printer", &printer_base, &printer_count)) {
      on_printer_page = active_page_ >= printer_base &&
                        active_page_ < printer_base + static_cast<int>(printer_count);
    }
  }
  const bool on_plugin_page = !scrolling_ && !on_printer_page;

  // fixed_overlay_ contains the full-screen arc (466×466 px).  Setting opa to any
  // fractional value forces LVGL to render the entire widget tree into a 24 KB
  // offscreen-layer buffer → ~18 layer-passes per scroll event → measurable stutter.
  // Instead we snap to binary visibility: hidden while page 0 dominates, fully
  // opaque once we cross the midpoint toward page 1.  Exactly 0 and LV_OPA_COVER
  // are both "fully decided" → LVGL skips the layer path entirely.
  if (clamped < page_w / 2 || on_plugin_page) {
    set_hidden(fixed_overlay_, true);
  } else {
    set_hidden(fixed_overlay_, false);
    lv_obj_set_style_opa(fixed_overlay_, LV_OPA_COVER, 0);
  }

  // Small text labels: opa fade is cheap (entire label fits in the 24 KB layer buffer
  // in a single pass), so the visual cross-fade is worth keeping.
  const lv_opa_t progress_opa =
      on_plugin_page ? static_cast<lv_opa_t>(LV_OPA_TRANSP) : overlay_opa;
  lv_obj_set_style_opa(progress_label_, progress_opa, 0);
  lv_obj_set_style_opa(battery_icon_label_, overlay_opa, 0);
  lv_obj_set_style_opa(battery_pct_label_, overlay_opa, 0);
}

void Ui::apply_ring_visual(const RingVisual& ring, int progress_value, uint32_t text_hex) {
  LvglLockGuard lock(200, "apply_ring_visual");
  if (!lock.locked()) {
    return;
  }
  apply_ring_visual_locked(ring, progress_value, text_hex);
}

void Ui::apply_ring_visual_locked(const RingVisual& ring, int progress, uint32_t text_hex) {
  // --- Manage lv_anim transitions ---
  const auto anim_kind_u8 = static_cast<uint8_t>(ring.anim_kind);
  if (anim_kind_u8 != active_ring_anim_kind_) {
    stop_ring_animations_locked();
    active_ring_anim_kind_ = anim_kind_u8;

    switch (ring.anim_kind) {
      case RingAnimKind::kFilamentLoad:
      case RingAnimKind::kFilamentUnload: {
        const bool loading = (ring.anim_kind == RingAnimKind::kFilamentLoad);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, status_arc_);
        lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
          lv_arc_set_value(static_cast<lv_obj_t*>(obj), v);
        });
        lv_anim_set_values(&a, loading ? 0 : 100, loading ? 100 : 0);
        lv_anim_set_duration(&a, 2000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_repeat_delay(&a, 300);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        break;
      }
      case RingAnimKind::kPulseBoth:
      case RingAnimKind::kPulseIndicator: {
        const uint16_t depth = static_cast<uint16_t>(
            std::min<uint32_t>(kRingPulseDepthPercent, 100U) * 255U / 100U);
        const int32_t min_scale = static_cast<int32_t>(255 > depth ? 255 - depth : 0);
        pulse_base_hex_ = ring.pulse_base_hex;
        pulse_both_parts_ = (ring.anim_kind == RingAnimKind::kPulseBoth);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, this);
        lv_anim_set_exec_cb(&a, pulse_anim_exec_cb);
        lv_anim_set_values(&a, min_scale, 255);
        lv_anim_set_duration(&a, ring.pulse_period_ms / 2);
        lv_anim_set_reverse_duration(&a, ring.pulse_period_ms / 2);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_linear);
        lv_anim_start(&a);
        break;
      }
      case RingAnimKind::kNone:
        break;
    }
  } else if (ring.anim_kind == RingAnimKind::kPulseBoth ||
             ring.anim_kind == RingAnimKind::kPulseIndicator) {
    pulse_base_hex_ = ring.pulse_base_hex;
    pulse_both_parts_ = (ring.anim_kind == RingAnimKind::kPulseBoth);
  }

  // --- Apply static properties ---
  if (ring.anim_kind != RingAnimKind::kFilamentLoad &&
      ring.anim_kind != RingAnimKind::kFilamentUnload) {
    const int displayed_value = (ring.value_override >= 0) ? ring.value_override : progress;
    if (lv_arc_get_value(status_arc_) != displayed_value) {
      lv_arc_set_value(status_arc_, displayed_value);
    }
  }

  if (ring.anim_kind != RingAnimKind::kPulseBoth &&
      ring.anim_kind != RingAnimKind::kPulseIndicator) {
    if (last_ring_main_hex_ != ring.main_hex) {
      lv_obj_set_style_arc_color(status_arc_, lv_color_hex(ring.main_hex), LV_PART_MAIN);
      last_ring_main_hex_ = ring.main_hex;
    }
    if (last_ring_indicator_hex_ != ring.indicator_hex) {
      lv_obj_set_style_arc_color(status_arc_, lv_color_hex(ring.indicator_hex), LV_PART_INDICATOR);
      last_ring_indicator_hex_ = ring.indicator_hex;
    }
  }

  if (last_ring_text_hex_ != text_hex) {
    lv_obj_set_style_text_color(progress_label_, lv_color_hex(text_hex), 0);
    last_ring_text_hex_ = text_hex;
  }

  char progress_buffer[8] = {};
  std::snprintf(progress_buffer, sizeof(progress_buffer), "%d%%", progress);
  set_label_text_if_changed(progress_label_, progress_buffer);
}

void Ui::stop_ring_animations_locked() {
  lv_anim_delete(status_arc_, nullptr);
  lv_anim_delete(this, pulse_anim_exec_cb);
  active_ring_anim_kind_ = static_cast<uint8_t>(RingAnimKind::kNone);
}

void Ui::pulse_anim_exec_cb(void* var, int32_t scale) {
  auto* ui = static_cast<Ui*>(var);
  if (ui == nullptr || ui->status_arc_ == nullptr) return;
  const uint32_t hex = scale_color(ui->pulse_base_hex_, static_cast<uint16_t>(scale));
  if (ui->pulse_both_parts_) {
    if (ui->last_ring_main_hex_ != hex) {
      lv_obj_set_style_arc_color(ui->status_arc_, lv_color_hex(hex), LV_PART_MAIN);
      ui->last_ring_main_hex_ = hex;
    }
  }
  if (ui->last_ring_indicator_hex_ != hex) {
    lv_obj_set_style_arc_color(ui->status_arc_, lv_color_hex(hex), LV_PART_INDICATOR);
    ui->last_ring_indicator_hex_ = hex;
  }
}

// Ambient sweep timer (ring_timer_cb, handle_ring_timer) removed — the rotating
// arc during printing caused excessive LVGL redraws on single-buffered display.

// --- Multi-AMS helpers --------------------------------------------------------


esp_err_t Ui::build_dashboard() {
  LvglLockGuard lock(3000, "build_dashboard");
  if (!lock.locked()) {
    return ESP_ERR_TIMEOUT;
  }

  screen_ = lv_screen_active();
  lv_obj_set_style_bg_color(screen_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
  apply_display_rotation_tilt_correction(screen_, display_rotation_, display_tilt_deci_deg_);

  const lv_font_t* dosis20 = &dosis_20;
  const lv_font_t* dosis40 = &dosis_40;
  const lv_font_t* mdi30 = &mdi_30;

  lv_obj_t* pager = lv_obj_create(screen_);
  lv_obj_set_size(pager, board::kDisplayWidth, board::kDisplayHeight);
  lv_obj_center(pager);
  make_transparent(pager);
  apply_display_rotation_visual_offset(pager, display_rotation_);
  lv_obj_set_style_pad_column(pager, 0, 0);
  lv_obj_set_style_pad_row(pager, 0, 0);
  enable_touch_bubble(pager);
  lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
  lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
  // LV_SCROLL_SNAP_NONE: we handle page snapping ourselves in handle_pager_event.
  // LV_SCROLL_SNAP_CENTER would make LVGL launch its own snap animation that conflicts
  // with our set_active_page() call, causing double-animation jitter and page-skip bugs.
  lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_NONE);
  lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(pager, &Ui::pager_event_cb, LV_EVENT_ALL, this);
  ui_shell_.bind_pager(pager);

  auto create_page = [](lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_set_size(page, board::kDisplayWidth, board::kDisplayHeight);
    make_transparent(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
  };

  // Generic plugin-page pool — every compiled-in plugin's pages (printer's
  // included, as of Phase 4b/4c) live here uniformly, sized once by
  // reserve_plugin_page_pool() (Application, before initialize() runs).
  // Allocate at least 1 slot so the unique_ptr is never null even when no
  // plugin wants a page.
  plugin_pages_ = std::make_unique<lv_obj_t*[]>(plugin_pool_size_ > 0 ? plugin_pool_size_ : 1);
  plugin_pages_enabled_ = std::make_unique<bool[]>(plugin_pool_size_ > 0 ? plugin_pool_size_ : 1);
  for (uint16_t i = 0; i < plugin_pool_size_; ++i) {
    plugin_pages_[i] = create_page(pager);
    plugin_pages_enabled_[i] = false;
    // Must match plugin_pages_enabled_[i]'s false default -- set_hidden()
    // otherwise only ever runs inside set_plugin_pages_enabled(), which a
    // plugin that starts (and stays) disabled never calls (Application only
    // calls update_ui() for enabled plugins). Without this, the *tracking*
    // flag correctly says "disabled" (so pager-settle logic in
    // clamp_enabled_page()/next_enabled_page() correctly skips it when
    // deciding where to land), but the widget itself is still visually
    // rendered at its normal position in the pager's horizontal layout --
    // a raw finger drag isn't gated by that tracking flag at all, so it
    // physically scrolls over real, visible content before SCROLL_END's
    // snap-back logic redirects away. Confirmed on-device 2026-08-14: a
    // disabled plugin's pages were draggable-into and fully rendered, even
    // though every settled (SCROLL_END) navigation decision correctly
    // avoided them.
    set_hidden(plugin_pages_[i], true);
    enable_touch_bubble(plugin_pages_[i]);
  }

  // --- Shared overlay: status ring, progress %, battery — genuinely core,
  // rendered on lv_layer_top()/fixed_overlay_, not any single plugin's page.
  fixed_overlay_ = lv_obj_create(screen_);
  lv_obj_set_size(fixed_overlay_, board::kDisplayWidth, board::kDisplayHeight);
  lv_obj_center(fixed_overlay_);
  make_transparent(fixed_overlay_);
  apply_display_rotation_visual_offset(fixed_overlay_, display_rotation_);
  lv_obj_clear_flag(fixed_overlay_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(fixed_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(fixed_overlay_);
  lv_obj_move_foreground(fixed_overlay_);

  status_arc_ = lv_arc_create(fixed_overlay_);
  lv_obj_set_size(status_arc_, board::kDisplayWidth, board::kDisplayHeight);
  lv_obj_remove_flag(status_arc_, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_rotation(status_arc_, 270);
  lv_arc_set_bg_angles(status_arc_, 0, 360);
  lv_arc_set_range(status_arc_, 0, 100);
  lv_arc_set_value(status_arc_, 0);
  lv_obj_center(status_arc_);
  lv_obj_set_style_arc_width(status_arc_, kRingStrokeWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_width(status_arc_, kRingStrokeWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(status_arc_, lv_color_hex(0x101010), LV_PART_MAIN);
  lv_obj_set_style_arc_color(status_arc_, lv_color_hex(arc_colors_.printing), LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(status_arc_, true, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(status_arc_, true, LV_PART_INDICATOR);
  lv_obj_set_style_opa(status_arc_, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_clear_flag(status_arc_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(status_arc_);

  progress_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(progress_label_, "--%");
  lv_obj_set_style_text_font(progress_label_, dosis40, 0);
  lv_obj_set_style_text_color(progress_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(progress_label_, LV_ALIGN_CENTER, 0, -178);
  apply_display_rotation_visual_offset(progress_label_, display_rotation_);
  apply_display_rotation_tilt_correction(progress_label_, display_rotation_, display_tilt_deci_deg_);
  lv_obj_move_foreground(progress_label_);

  battery_icon_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(battery_icon_label_, "");
  lv_obj_set_style_text_font(battery_icon_label_, mdi30, 0);
  lv_obj_set_style_text_color(battery_icon_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(battery_icon_label_, LV_ALIGN_CENTER, -20, -140);
  apply_display_rotation_visual_offset(battery_icon_label_, display_rotation_);
  apply_display_rotation_tilt_correction(battery_icon_label_, display_rotation_, display_tilt_deci_deg_);
  lv_obj_move_foreground(battery_icon_label_);
  lv_obj_add_flag(battery_icon_label_, LV_OBJ_FLAG_HIDDEN);

  battery_pct_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(battery_pct_label_, "--%");
  lv_obj_set_style_text_font(battery_pct_label_, dosis20, 0);
  lv_obj_set_style_text_color(battery_pct_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(battery_pct_label_, LV_ALIGN_CENTER, 20, -140);
  apply_display_rotation_visual_offset(battery_pct_label_, display_rotation_);
  apply_display_rotation_tilt_correction(battery_pct_label_, display_rotation_, display_tilt_deci_deg_);
  lv_obj_move_foreground(battery_pct_label_);
  lv_obj_add_flag(battery_pct_label_, LV_OBJ_FLAG_HIDDEN);

  // Portal hint — was parented to the printer plugin's main-dashboard page;
  // now on lv_layer_top() like the rest of this shared overlay so it works
  // regardless of which plugin (if any) owns the currently-settled page
  // (Phase 4b/4c — see CLAUDE.md). Shown on any settled (non-scrolling) page
  // now instead of only the printer's main dashboard.
  portal_hint_label_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(portal_hint_label_, "");
  lv_obj_set_width(portal_hint_label_, 320);
  lv_label_set_long_mode(portal_hint_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(portal_hint_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(portal_hint_label_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(portal_hint_label_, lv_color_hex(0x64748B), 0);
  lv_obj_align(portal_hint_label_, LV_ALIGN_CENTER, 0, 114);
  lv_obj_add_flag(portal_hint_label_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(portal_hint_label_);

  brightness_overlay_ = lv_label_create(lv_layer_top());
  set_label_text_if_changed(brightness_overlay_, "80%");
  lv_obj_set_style_text_font(brightness_overlay_, dosis40, 0);
  lv_obj_set_style_text_color(brightness_overlay_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_color(brightness_overlay_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(brightness_overlay_, LV_OPA_60, 0);
  lv_obj_set_style_pad_hor(brightness_overlay_, 20, 0);
  lv_obj_set_style_pad_ver(brightness_overlay_, 10, 0);
  lv_obj_set_style_radius(brightness_overlay_, 16, 0);
  lv_obj_align(brightness_overlay_, LV_ALIGN_CENTER, 0, 0);
  apply_display_rotation_visual_offset(brightness_overlay_, display_rotation_);
  apply_display_rotation_tilt_correction(brightness_overlay_, display_rotation_, display_tilt_deci_deg_);
  lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(brightness_overlay_);

  // Shown full-screen whenever every plugin's page is disabled (nothing left
  // for the pager to show) — see update_no_plugins_overlay_locked(). Created
  // before portal_overlay_card_ so the PIN unlock overlay always draws on
  // top of this one if both ever coincide (portal_overlay_card_ also calls
  // lv_obj_move_foreground() on itself below).
  no_plugins_overlay_ = lv_obj_create(lv_layer_top());
  lv_obj_set_size(no_plugins_overlay_, board::kDisplayWidth, board::kDisplayHeight);
  make_transparent(no_plugins_overlay_);
  lv_obj_set_style_bg_color(no_plugins_overlay_, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(no_plugins_overlay_, LV_OPA_COVER, 0);
  lv_obj_clear_flag(no_plugins_overlay_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(no_plugins_overlay_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(no_plugins_overlay_, LV_OBJ_FLAG_HIDDEN);

  no_plugins_overlay_label_ = lv_label_create(no_plugins_overlay_);
  set_label_text_if_changed(no_plugins_overlay_label_,
                             "No plugins enabled.\nEnable one or more in Web Config.");
  lv_obj_set_width(no_plugins_overlay_label_, 340);
  lv_label_set_long_mode(no_plugins_overlay_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(no_plugins_overlay_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(no_plugins_overlay_label_, dosis20, 0);
  lv_obj_set_style_text_color(no_plugins_overlay_label_, lv_color_hex(0x999999), 0);
  lv_obj_center(no_plugins_overlay_label_);

  portal_overlay_card_ = lv_obj_create(lv_layer_top());
  lv_obj_set_size(portal_overlay_card_, 280, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(portal_overlay_card_, 22, 0);
  lv_obj_set_style_bg_color(portal_overlay_card_, lv_color_hex(0x071018), 0);
  lv_obj_set_style_bg_opa(portal_overlay_card_, LV_OPA_90, 0);
  lv_obj_set_style_border_color(portal_overlay_card_, lv_color_hex(0xF0A64B), 0);
  lv_obj_set_style_border_width(portal_overlay_card_, 2, 0);
  lv_obj_set_style_pad_hor(portal_overlay_card_, 22, 0);
  lv_obj_set_style_pad_ver(portal_overlay_card_, 18, 0);
  lv_obj_set_style_pad_row(portal_overlay_card_, 8, 0);
  lv_obj_set_layout(portal_overlay_card_, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(portal_overlay_card_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(portal_overlay_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_center(portal_overlay_card_);
  apply_display_rotation_visual_offset(portal_overlay_card_, display_rotation_);
  apply_display_rotation_tilt_correction(portal_overlay_card_, display_rotation_, display_tilt_deci_deg_);
  lv_obj_clear_flag(portal_overlay_card_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(portal_overlay_card_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(portal_overlay_card_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(portal_overlay_card_);

  portal_overlay_title_ = lv_label_create(portal_overlay_card_);
  set_label_text_if_changed(portal_overlay_title_, "WEB CONFIG PIN");
  lv_obj_set_style_text_font(portal_overlay_title_, dosis20, 0);
  lv_obj_set_style_text_color(portal_overlay_title_, lv_color_hex(0xF8FAFC), 0);

  portal_overlay_value_ = lv_label_create(portal_overlay_card_);
  set_label_text_if_changed(portal_overlay_value_, "000000");
  lv_obj_set_style_text_font(portal_overlay_value_, dosis40, 0);
  lv_obj_set_style_text_color(portal_overlay_value_, lv_color_hex(0xF0A64B), 0);

  portal_overlay_detail_ = lv_label_create(portal_overlay_card_);
  set_label_text_if_changed(portal_overlay_detail_, "");
  lv_obj_set_width(portal_overlay_detail_, 236);
  lv_label_set_long_mode(portal_overlay_detail_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(portal_overlay_detail_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(portal_overlay_detail_, dosis20, 0);
  lv_obj_set_style_text_color(portal_overlay_detail_, lv_color_hex(0xCBD5E1), 0);

  lv_obj_add_event_cb(screen_, &Ui::screen_event_cb, LV_EVENT_ALL, this);
  lv_obj_update_layout(ui_shell_.pager());

  // Default starting page is the pool's first page (index 0) — normally the
  // first-registered plugin's first page. A content plugin that wants a
  // different starting page (e.g. the printer plugin's main dashboard rather
  // than its own page 0) calls set_active_page_by_plugin() once it finishes
  // building, overriding this.
  active_page_ = 0;
  scrolling_ = false;
  publish_page_state_snapshot();
  apply_page_visibility();
  update_portal_access_visuals_locked();

  // Establish the initial no-plugins-overlay state right away. Every other
  // call site is set_plugin_pages_enabled()'s resettle path, which only
  // fires on a *change* — but plugin_pages_enabled_[] already defaults to
  // false for every page (see the loop above), so a plugin that starts (and
  // stays) unconfigured never causes a false->false "change" and this would
  // otherwise never run at all, leaving a genuinely blank screen instead of
  // the "No plugins enabled" message when nothing is configured yet.
  update_no_plugins_overlay_locked();

  return ESP_OK;
}

void Ui::apply_page_visibility() {
  LvglLockGuard::note_phase("page_visibility");
  // Content-plugin widget visibility (which page's labels/images show) is
  // entirely the registered plugin's own responsibility now (Phase 4b/4c —
  // see CLAUDE.md) — Ui just fires the hook on every trigger that used to
  // directly manipulate printer widgets (scroll begin/end, availability
  // toggles). Overlay/parallax/portal below stay genuinely core.
  if (page_visibility_cb_ != nullptr) {
    page_visibility_cb_(page_visibility_cb_user_data_);
  }

  // Overlay + page0 opacity are driven by apply_page0_parallax.
  // When not scrolling, snap to final state.
  if (!scrolling_) {
    apply_page0_parallax(true);
  }

  update_portal_access_visuals_locked();
}

void Ui::update_no_plugins_overlay_locked() {
  set_hidden(no_plugins_overlay_, ui_shell_.any_page_enabled());
}

void Ui::reserve_plugin_page_pool(uint16_t total_pages) {
  plugin_pool_size_ = total_pages;
  ui_shell_.configure_generic_page_pool(total_pages);
}

int Ui::register_plugin_pages(const char* plugin_id, uint8_t count) {
  if (count == 0) {
    return -1;
  }
  const int base = ui_shell_.reserve_plugin_pages(plugin_id, count);
  if (base < 0 || static_cast<uint16_t>(base + count) > plugin_pool_size_) {
    return base;
  }
  for (uint8_t i = 0; i < count; ++i) {
    ui_shell_.register_page_slot(base + i, &plugin_pages_[base + i], &plugin_pages_enabled_[base + i]);
  }
  return base;
}

lv_obj_t* Ui::plugin_page_container(int page_index) const {
  if (page_index < 0 || static_cast<uint16_t>(page_index) >= plugin_pool_size_) {
    return nullptr;
  }
  return plugin_pages_[page_index];
}

lv_obj_t* Ui::plugin_page_container_for(const char* plugin_id, int local_idx) const {
  int base = 0;
  uint8_t count = 0;
  if (!ui_shell_.plugin_page_range(plugin_id, &base, &count) || local_idx < 0 ||
      local_idx >= static_cast<int>(count)) {
    return nullptr;
  }
  return plugin_page_container(base + local_idx);
}

void Ui::set_plugin_page_available(const char* plugin_id, int local_idx, const bool* enabled_flag) {
  int base = 0;
  uint8_t count = 0;
  if (!ui_shell_.plugin_page_range(plugin_id, &base, &count) || local_idx < 0 ||
      local_idx >= static_cast<int>(count) || enabled_flag == nullptr) {
    return;
  }
  const int abs_idx = base + local_idx;
  if (abs_idx < 0 || static_cast<uint16_t>(abs_idx) >= plugin_pool_size_) {
    return;
  }
  ui_shell_.register_page_slot(abs_idx, &plugin_pages_[abs_idx], enabled_flag);
}

void Ui::resettle_pager_after_availability_change() {
  active_page_ = clamp_enabled_page(active_page_);
  lv_obj_update_layout(ui_shell_.pager());
  if (lv_obj_t* target_page = page_object(active_page_); target_page != nullptr) {
    lv_obj_scroll_to_view(target_page, LV_ANIM_OFF);
  }
  scrolling_ = false;
  publish_page_state_snapshot();
  apply_page0_parallax(true);
  apply_page_visibility();
  update_no_plugins_overlay_locked();
}

void Ui::set_active_page_by_plugin(const char* plugin_id, int local_idx) {
  int base = 0;
  uint8_t count = 0;
  if (!ui_shell_.plugin_page_range(plugin_id, &base, &count) || local_idx < 0 ||
      local_idx >= static_cast<int>(count)) {
    return;
  }
  set_active_page(base + local_idx);
}

void Ui::set_plugin_pages_enabled(const char* plugin_id, bool enabled, uint32_t lock_timeout_ms) {
  LvglLockGuard lock(lock_timeout_ms, "set_plugin_pages_enabled");
  if (!lock.locked()) {
    return;
  }
  int base = 0;
  uint8_t count = 0;
  if (!ui_shell_.plugin_page_range(plugin_id, &base, &count) || count == 0) {
    return;
  }
  if (base < 0 || static_cast<uint16_t>(base + count) > plugin_pool_size_) {
    return;
  }
  bool changed = false;
  for (uint8_t i = 0; i < count; ++i) {
    if (plugin_pages_enabled_[base + i] != enabled) {
      plugin_pages_enabled_[base + i] = enabled;
      changed = true;
    }
    set_hidden(plugin_pages_[base + i], !enabled);
  }
  if (!changed) {
    return;
  }
  resettle_pager_after_availability_change();
}

void Ui::register_page_visibility_callback(PluginPageCallback callback, void* user_data) {
  page_visibility_cb_ = callback;
  page_visibility_cb_user_data_ = user_data;
}

void Ui::register_page_settle_callback(PluginPageCallback callback, void* user_data) {
  page_settle_cb_ = callback;
  page_settle_cb_user_data_ = user_data;
}

void Ui::register_page_tap_callback(PluginPageCallback callback, void* user_data) {
  page_tap_cb_ = callback;
  page_tap_cb_user_data_ = user_data;
}

void Ui::set_battery_overlay_text(const std::string& icon_text, const std::string& pct_text) {
  set_label_text_if_changed(battery_icon_label_, icon_text);
  set_label_text_if_changed(battery_pct_label_, pct_text);
}

void Ui::set_battery_overlay_visible(bool visible) {
  set_hidden(battery_icon_label_, !visible);
  set_hidden(battery_pct_label_, !visible);
}

void Ui::update_battery_overlay(const PowerSnapshot& power) {
  if (!power.available) {
    return;
  }
  set_battery_overlay_text(battery_icon_text(power), battery_pct_text(power));
  set_battery_overlay_visible(power.battery_present || power.charging);
}

lv_obj_t* Ui::page_object(int page) const { return ui_shell_.page_object(page); }

int Ui::next_enabled_page(int page, int direction) const {
  return ui_shell_.next_enabled_page(page, direction);
}

int Ui::clamp_enabled_page(int page) const { return ui_shell_.clamp_enabled_page(page); }

int Ui::nearest_enabled_page_for_scroll() const {
  return ui_shell_.nearest_enabled_page_for_scroll(active_page_);
}

void Ui::set_active_page(int page) {
  const int clamped_page = clamp_enabled_page(page);
  const int previous_page = active_page_;
  lv_obj_update_layout(ui_shell_.pager());
  if (lv_obj_t* target_page = page_object(clamped_page); target_page != nullptr) {
    // lv_obj_scroll_to_view can leave a small residual offset depending on
    // scroll direction.  Use the page's exact x-position for a pixel-perfect snap.
    const int target_x = lv_obj_get_x(target_page);
    lv_obj_scroll_to_x(ui_shell_.pager(), target_x, LV_ANIM_OFF);
  }
  active_page_ = clamped_page;
  if (clamped_page == 0 && previous_page != 0 && page0_reentry_cb_ != nullptr) {
    page0_reentry_cb_(page0_reentry_user_data_);
  }
  scrolling_ = false;
  publish_page_state_snapshot();
  apply_page0_parallax(true);
  apply_page_visibility();
  // Content-plugin full re-render on settle (replays deferred/last snapshot,
  // preview-image-load-on-entry, camera-refresh-on-entry — all now the
  // registered plugin's own responsibility, see printer_plugin_ui.cpp's
  // on_page_settled()). Fired unconditionally, even landing back on the same
  // page — a snapshot may have arrived mid-scroll and be waiting to apply.
  if (page_settle_cb_ != nullptr) {
    page_settle_cb_(page_settle_cb_user_data_);
  }
}

void Ui::set_pager_scroll_locked(bool locked) {
  const bool was_locked = ui_shell_.pager_scroll_locked();
  ui_shell_.set_pager_scroll_locked(locked);

  if (ui_shell_.pager() == nullptr || !locked || was_locked) {
    return;
  }

  // Snap back to the currently active page as soon as brightness control wins
  // the gesture, so a slightly diagonal drag doesn't leave the pager half-way
  // between pages.
  lv_obj_update_layout(ui_shell_.pager());
  if (lv_obj_t* target_page = page_object(active_page_); target_page != nullptr) {
    const int target_x = lv_obj_get_x(target_page);
    lv_obj_scroll_to_x(ui_shell_.pager(), target_x, LV_ANIM_OFF);
  }
  scrolling_ = false;
  publish_page_state_snapshot();
  apply_page0_parallax(true);
  apply_page_visibility();
}

void Ui::publish_page_state_snapshot() {
  active_page_snapshot_.store(active_page_, std::memory_order_relaxed);
  page_scrolling_snapshot_.store(scrolling_, std::memory_order_relaxed);
}

void Ui::handle_pager_event(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);

  if (ui_shell_.pager_scroll_locked()) {
    return;
  }

  if (code == LV_EVENT_SCROLL) {
    apply_page0_parallax();
    return;
  }

  if (code != LV_EVENT_SCROLL_BEGIN && code != LV_EVENT_SCROLL_END) {
    return;
  }

  if (code == LV_EVENT_SCROLL_BEGIN) {
    scrolling_ = true;
    // Capture the gesture's origin page for the release-threshold decision
    // below, and mark a real gesture as in progress. Only finger-initiated
    // scrolls count; the snap animation's own SCROLL_BEGIN (no pressed
    // indev) must not overwrite scroll_origin_page_, and must not clear
    // pager_gesture_active_ either — by the time that follow-through fires,
    // the finger that started the gesture has already been released, so
    // "not pressed" doesn't mean "not part of a real gesture" here.
    if (lv_indev_t* begin_indev = lv_indev_active();
        begin_indev != nullptr && lv_indev_get_state(begin_indev) == LV_INDEV_STATE_PRESSED) {
      scroll_origin_page_ = nearest_enabled_page_for_scroll();
      pager_gesture_active_ = true;
    }
    publish_page_state_snapshot();

    apply_page_visibility();
    return;
  }

  int scroll_x = lv_obj_get_scroll_x(ui_shell_.pager());
  if (scroll_x < 0) {
    scroll_x = -scroll_x;
  }

  // If the user grabbed the pager while a snap animation was in flight, LVGL
  // deletes the animation and fires SCROLL_END mid-press. Don't fight the
  // finger: their own gesture will deliver a fresh SCROLL_END on release.
  if (lv_indev_t* active_indev = lv_indev_active();
      active_indev != nullptr && lv_indev_get_state(active_indev) == LV_INDEV_STATE_PRESSED) {
    return;
  }

  // set_active_page() (boot redirect, resettle-after-availability-change,
  // etc.) scrolls the pager itself via lv_obj_scroll_to_x(), which fires this
  // same SCROLL_BEGIN/SCROLL_END pair with no pressed indev. Without this
  // guard that synthetic SCROLL_END falls into the gesture-interpretation
  // logic below and re-derives a snap target from stale
  // scroll_origin_page_/scroll position, silently overriding the
  // programmatic jump (e.g. landing on an AMS page instead of the intended
  // dashboard on boot). pager_gesture_active_ is only set true by a real
  // press (above) and only cleared once a real gesture actually finalizes
  // (below), so it correctly stays true across a real gesture's own
  // snap-animation follow-through while staying false for a fully
  // programmatic scroll.
  if (!pager_gesture_active_) {
    scrolling_ = false;
    publish_page_state_snapshot();
    apply_page0_parallax(true);
    apply_page_visibility();
    return;
  }

  // Smartphone-style snap: instead of hard-jumping to the nearest page
  // (LV_ANIM_OFF), glide there with LVGL's built-in ease-out scroll animation
  // (200-400 ms depending on distance). While the animation runs we keep
  // scrolling_ == true so both pages stay visible; the animation fires a final
  // SCROLL_END when it lands, which re-enters this handler with a zero delta
  // and finalizes the page switch via set_active_page().
  //
  // Page-advance threshold: the nearest-page rule alone would snap BACK unless
  // more than half the screen was dragged. Smartphone pagers advance once the
  // drag passes ~20% of the width, so if the nearest page is still the gesture's
  // origin page but the drag went past the threshold, advance one page in the
  // drag direction instead.
  int snap_page = nearest_enabled_page_for_scroll();
  if (lv_obj_t* origin_obj = page_object(scroll_origin_page_); origin_obj != nullptr) {
    const int delta = scroll_x - lv_obj_get_x(origin_obj);
    if (snap_page == scroll_origin_page_) {
      constexpr int kAdvanceThresholdPx = board::kDisplayWidth / 5;
      if (std::abs(delta) >= kAdvanceThresholdPx) {
        snap_page = next_enabled_page(scroll_origin_page_, delta > 0 ? 1 : -1);
      }
    } else {
      // With real fling momentum (scroll_throw), a fast flick can carry
      // scroll_x more than one page-width past the origin before SCROLL_END
      // fires, making the nearest-center rule above jump 2+ pages in a single
      // gesture. Cap every gesture to at most one page of travel from its
      // origin, matching normal single-swipe-single-page pager UX.
      snap_page = next_enabled_page(scroll_origin_page_, delta > 0 ? 1 : -1);
    }
  }
  if (lv_obj_t* snap_target = page_object(snap_page); snap_target != nullptr) {
    const int target_x = lv_obj_get_x(snap_target);
    if (std::abs(scroll_x - target_x) > 1) {
      lv_obj_scroll_to_x(ui_shell_.pager(), target_x, LV_ANIM_ON);
      return;
    }
  }
  // Real gesture is finalizing now — clear before set_active_page() runs its
  // own (programmatic, ANIM_OFF) scroll, so that scroll's own SCROLL_END
  // doesn't get mistaken for a further real gesture.
  pager_gesture_active_ = false;
  set_active_page(snap_page);
}

void Ui::handle_screen_event(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
      code != LV_EVENT_PRESS_LOST && code != LV_EVENT_LONG_PRESSED) {
    return;
  }

  lv_indev_t* indev = lv_indev_get_act();
  if (indev == nullptr) {
    return;
  }

  lv_point_t point = {};
  lv_indev_get_point(indev, &point);

  if (code == LV_EVENT_PRESSED) {
    set_pager_scroll_locked(false);
    if (ui_shell_.screen_power_mode() == ScreenPowerMode::kOff) {
      // First touch wakes the screen; a second touch performs UI actions.
      note_activity(true);
      gesture_active_ = false;
      swipe_switched_ = false;
      overlay_visible_ = false;
      return;
    }

    note_activity(false);
    gesture_active_ = true;
    swipe_switched_ = false;
    overlay_visible_ = false;
    gesture_start_x_ = point.x;
    gesture_start_y_ = point.y;
    gesture_start_brightness_ = ui_shell_.brightness_percent();
    return;
  }

  if (code == LV_EVENT_PRESSING && gesture_active_) {
    note_activity(false);
    const int dx = static_cast<int>(point.x - gesture_start_x_);
    const int dy = static_cast<int>(gesture_start_y_ - point.y);
    const int abs_dx = std::abs(dx);
    const int abs_dy = std::abs(dy);

    if (swipe_switched_) {
      return;
    }

    if (!overlay_visible_) {
      // Resolve the gesture once: either a horizontal page swipe or a mostly
      // vertical brightness drag. This avoids accidental brightness changes
      // while the finger is moving diagonally during page navigation.
      const bool horizontal_swipe =
          abs_dx >= kSwipeThresholdPx &&
          abs_dx >= (abs_dy - kGestureAxisLockMarginPx);
      const bool vertical_brightness =
          abs_dy >= kSwipeThresholdPx &&
          abs_dx <= kBrightnessHorizontalTolerancePx &&
          abs_dy >= (abs_dx + kGestureAxisLockMarginPx);

      if (horizontal_swipe) {
        swipe_switched_ = true;
        return;
      }
      if (!vertical_brightness) {
        return;
      }

      set_pager_scroll_locked(true);
    }

    const float delta = static_cast<float>(dy) * (100.0f / 250.0f);
    const int new_brightness =
        std::clamp(gesture_start_brightness_ + static_cast<int>(std::lround(delta)),
                   kManualMinBrightnessPercent, 100);
    ui_shell_.set_brightness_percent(new_brightness);

    char buffer[8] = {};
    std::snprintf(buffer, sizeof(buffer), "%d%%", ui_shell_.brightness_percent());
    set_label_text_if_changed(brightness_overlay_, buffer);
    lv_obj_clear_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
    overlay_visible_ = true;
    return;
  }

  if (code == LV_EVENT_LONG_PRESSED && gesture_active_) {
    note_activity(false);
    if (!overlay_visible_ && !scrolling_ && !swipe_switched_) {
      portal_unlock_requested_.store(true);
    }
    return;
  }

  if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && gesture_active_) {
    note_activity(false);
    const int dx = static_cast<int>(point.x - gesture_start_x_);
    const int dy = static_cast<int>(gesture_start_y_ - point.y);
    const int abs_dx = std::abs(dx);
    const int abs_dy = std::abs(dy);
    const bool swipe_locked = swipe_switched_;

    gesture_active_ = false;
    swipe_switched_ = false;
    set_pager_scroll_locked(false);
    if (overlay_visible_) {
      lv_obj_add_flag(brightness_overlay_, LV_OBJ_FLAG_HIDDEN);
      overlay_visible_ = false;
      return;
    }
    if (swipe_locked) {
      return;
    }

    // Horizontal page swiping is handled by the LVGL pager (flex-row +
    // LV_SCROLL_SNAP_NONE → handle_pager_event snaps to nearest page on SCROLL_END).
    // Only handle plain taps here — content-plugin-specific (e.g. camera
    // refresh on the printer plugin's camera page) via the registered tap
    // callback, generic here.
    if (abs_dx < 12 && abs_dy < 12 && page_tap_cb_ != nullptr) {
      page_tap_cb_(page_tap_cb_user_data_);
    }
  }
}

void Ui::update_portal_access_visuals_locked() {
  // Simplified (Phase 4b/4c — see CLAUDE.md): no longer restricted to "the
  // printer plugin's main dashboard is the settled page," and no longer
  // yields to a content plugin's own detail label (that coupling required
  // Ui to reach into printer-owned widget state directly, which is exactly
  // what this phase removes) — shows on any settled page whenever the PIN
  // lock or an active session actually needs the user's attention.
  const bool portal_hint_has_priority = portal_pin_active_ || portal_session_active_;
  const bool show_hint = portal_hint_label_ != nullptr && !scrolling_ &&
                         !portal_hint_text_.empty() && portal_hint_has_priority;
  portal_hint_currently_shown_ = show_hint;
  set_hidden(portal_hint_label_, !show_hint);
  if (show_hint) {
    set_label_text_if_changed(portal_hint_label_, portal_hint_text_);
  }

  const bool show_overlay = portal_overlay_card_ != nullptr && portal_pin_active_;
  set_hidden(portal_overlay_card_, !show_overlay);
  if (!show_overlay) {
    return;
  }

  set_label_text_if_changed(portal_overlay_title_, portal_overlay_title_text_);
  set_label_text_if_changed(portal_overlay_value_, portal_overlay_value_text_);
  set_label_text_if_changed(portal_overlay_detail_, portal_overlay_detail_text_);
}

void Ui::note_activity(bool wake_display_now) { ui_shell_.note_activity(wake_display_now); }

void Ui::request_wake_display() { ui_shell_.note_activity(true); }

void Ui::set_battery_display_policy(const BatteryDisplayPolicy& policy) {
  ui_shell_.set_battery_display_policy(policy);
}

void Ui::update_power_save(bool on_battery, bool keep_awake, bool print_active) {
  ui_shell_.update_power_save(on_battery, keep_awake, print_active);
}

bool Ui::is_low_power_mode_active() const { return ui_shell_.is_low_power_mode_active(); }

void Ui::pager_event_cb(lv_event_t* event) {
  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_pager_event(event);
  }
}

void Ui::screen_event_cb(lv_event_t* event) {
  auto* ui = static_cast<Ui*>(lv_event_get_user_data(event));
  if (ui != nullptr) {
    ui->handle_screen_event(event);
  }
}

}  // namespace infohub
