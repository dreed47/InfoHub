#include "infohub/plugins/tempest/tempest_plugin.hpp"

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "esp_log.h"
#include "infohub/board_config.hpp"
#include "infohub/ui.hpp"
#include "infohub/ui_toolkit.hpp"

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#else
#error "Unknown InfoHub hardware variant"
#endif

// Same fonts ui.cpp uses for its own pages (include/font/dosis_*.c) — plain C
// objects, declared the same way ui.cpp does: extern "C", at file scope (not
// inside namespace infohub, which would give them C++ mangled names that
// don't match the real symbols).
extern "C" {
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_32;
extern const lv_font_t dosis_40;
extern const lv_font_t dosis_64;
}

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.tempest";
constexpr char kPluginNs[] = "tempest";
constexpr int kConditionArcDiameter = board::kDisplayWidth - 60;
constexpr int kConditionArcWidth = 18;
constexpr uint32_t kConditionArcNeutralHex = 0x444444;

// WeatherFlow's `icon` field on both current_conditions and each hourly
// forecast entry is a small fixed enum (DarkSky-derived, confirmed against
// WeatherFlow's REST API docs) -- unlike the free-text `conditions` string
// ("Partly Cloudy", "Rain Likely", ...), it's stable enough to map to a
// color category. Substring match (not exact), so e.g.
// "possibly-thunderstorm-night" still hits "thunderstorm".
uint32_t condition_color_for_icon(const std::string& icon) {
  if (icon.find("thunderstorm") != std::string::npos) {
    return 0x8E44AD;  // storm -- deep purple
  }
  if (icon.find("snow") != std::string::npos || icon.find("sleet") != std::string::npos) {
    return 0x7FD8F5;  // snow/sleet -- icy cyan
  }
  if (icon.find("rain") != std::string::npos) {
    return 0x3B82F6;  // rain -- blue
  }
  if (icon.find("foggy") != std::string::npos) {
    return 0x9AA5B1;  // fog -- muted grey
  }
  if (icon.find("windy") != std::string::npos) {
    return 0x2FBF9F;  // windy -- teal
  }
  if (icon.find("partly-cloudy") != std::string::npos) {
    return 0xB0BEC5;  // partly cloudy -- light grey-blue
  }
  if (icon.find("cloudy") != std::string::npos) {
    return 0x78909C;  // overcast -- darker grey-blue
  }
  if (icon.find("clear") != std::string::npos) {
    return 0xFFC940;  // clear/sunny -- gold
  }
  return kConditionArcNeutralHex;  // unrecognized icon -- neutral, not a guess
}

// "3pm", "11am" — respects TZ since time_sync.cpp calls tzset() via
// set_timezone_iana() at boot (see main/src/time_sync.cpp). Same
// localtime_r-based pattern as ui.cpp's eta_text().
std::string hour_label_text(uint64_t time_unix) {
  const std::time_t t = static_cast<std::time_t>(time_unix);
  std::tm local{};
  if (localtime_r(&t, &local) == nullptr) {
    return "--";
  }
  int hour12 = local.tm_hour % 12;
  if (hour12 == 0) {
    hour12 = 12;
  }
  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "%d%s", hour12, local.tm_hour < 12 ? "am" : "pm");
  return buffer;
}

// WeatherFlowClient always stores Celsius internally (see fahrenheit_'s
// declaration comment) -- this is the one place the display conversion
// happens, applied wherever a temperature gets rendered.
double celsius_to_display(double celsius_value, bool fahrenheit) {
  return fahrenheit ? (celsius_value * 9.0 / 5.0 + 32.0) : celsius_value;
}
}  // namespace

esp_err_t TempestPlugin::init(PluginContext& ctx) {
  config_store_ = &ctx.config_store;
  wifi_manager_ = &ctx.wifi_manager;
  setup_portal_ = &ctx.setup_portal;
  ui_ = &ctx.ui;

  client_.set_wifi_manager(wifi_manager_);
  client_.set_network_arbiter(&ctx.network_arbiter);
  load_config();

  const esp_err_t err = client_.start();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "WeatherFlowClient task start failed: %s", esp_err_to_name(err));
  }
  return err;
}

void TempestPlugin::load_config() {
  if (config_store_ == nullptr) {
    return;
  }
  const std::string station_id = config_store_->load_plugin_string(kPluginNs, "station_id");
  const std::string api_token = config_store_->load_plugin_string(kPluginNs, "api_token");
  const std::string poll_s_str = config_store_->load_plugin_string(kPluginNs, "poll_s");
  uint32_t poll_interval_s = 300;
  if (!poll_s_str.empty()) {
    poll_interval_s = static_cast<uint32_t>(std::strtoul(poll_s_str.c_str(), nullptr, 10));
  }
  client_.configure(station_id, api_token, poll_interval_s);

  set_fahrenheit(config_store_->load_plugin_string(kPluginNs, "units") == "f");
  set_enabled(config_store_->load_plugin_string(kPluginNs, "enabled") != "0");
}

void TempestPlugin::tick(uint64_t) {
  // No per-tick work — polling happens entirely on WeatherFlowClient's own
  // task; update_ui() (called every Application loop iteration regardless of
  // this) is where the fetched snapshot gets pushed to the screen.
}

bool TempestPlugin::wants_network() const {
  return client_.snapshot().configured;
}

void TempestPlugin::build_screen(lv_obj_t* parent, uint8_t page_index) {
  if (parent == nullptr) {
    return;
  }

  LvglLockGuard lock(3000, "TempestPlugin::build_screen");
  if (!lock.locked()) {
    return;
  }

  if (page_index == 1) {
    // Row of kHourlyForecastCount fixed cards (no scroll -- with only 4
    // entries they all fit, so a static row fills the round page evenly
    // instead of leaving a half-scrolled strip). Each card is a rounded
    // tile tinted by that hour's condition color (set in update_ui(), since
    // entry.icon isn't known yet here) so the row reads as a color-coded
    // strip at a glance, not just a wall of text.
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, board::kDisplayWidth - 24, 300);
    lv_obj_center(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    for (uint8_t i = 0; i < kHourlyForecastCount; ++i) {
      ForecastColumn& fc = forecast_columns_[i];

      fc.card = lv_obj_create(row);
      lv_obj_set_size(fc.card, 96, 280);
      lv_obj_set_style_radius(fc.card, 18, 0);
      lv_obj_set_style_bg_color(fc.card, lv_color_hex(kConditionArcNeutralHex), 0);
      lv_obj_set_style_bg_opa(fc.card, LV_OPA_30, 0);
      lv_obj_set_style_border_width(fc.card, 0, 0);
      lv_obj_clear_flag(fc.card, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_flex_flow(fc.card, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_flex_align(fc.card, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
      lv_obj_set_style_pad_row(fc.card, 4, 0);
      lv_obj_set_style_pad_top(fc.card, 14, 0);
      lv_obj_set_style_pad_bottom(fc.card, 14, 0);

      fc.time_label = lv_label_create(fc.card);
      lv_label_set_text(fc.time_label, "--");
      lv_obj_set_style_text_font(fc.time_label, &dosis_32, 0);
      lv_obj_set_style_text_color(fc.time_label, lv_color_hex(0xCCCCCC), 0);

      fc.temp_label = lv_label_create(fc.card);
      lv_label_set_text(fc.temp_label, "--");
      lv_obj_set_style_text_font(fc.temp_label, &dosis_40, 0);
      lv_obj_set_style_text_color(fc.temp_label, lv_color_hex(0xFFFFFF), 0);

      fc.conditions_label = lv_label_create(fc.card);
      lv_label_set_text(fc.conditions_label, "");
      lv_obj_set_width(fc.conditions_label, 88);
      lv_label_set_long_mode(fc.conditions_label, LV_LABEL_LONG_WRAP);
      lv_obj_set_style_text_align(fc.conditions_label, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_style_text_font(fc.conditions_label, &dosis_20, 0);
      lv_obj_set_style_text_color(fc.conditions_label, lv_color_hex(0xEEEEEE), 0);
    }

    return;
  }

  if (page_index == 0) {
    // Created before `column` below so it draws behind the text -- a full
    // ring (bg_angles 0-360, value pinned to the range max) recolored per
    // current_conditions.icon in update_ui(), not a progress metric.
    condition_arc_ = lv_arc_create(parent);
    lv_obj_set_size(condition_arc_, kConditionArcDiameter, kConditionArcDiameter);
    lv_obj_center(condition_arc_);
    lv_arc_set_rotation(condition_arc_, 270);
    lv_arc_set_bg_angles(condition_arc_, 0, 360);
    lv_arc_set_range(condition_arc_, 0, 100);
    lv_arc_set_value(condition_arc_, 100);
    lv_obj_remove_style(condition_arc_, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(condition_arc_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(condition_arc_, kConditionArcWidth, LV_PART_MAIN);
    lv_obj_set_style_arc_width(condition_arc_, kConditionArcWidth, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(condition_arc_, lv_color_hex(kConditionArcNeutralHex), LV_PART_MAIN);
    lv_obj_set_style_arc_color(condition_arc_, lv_color_hex(kConditionArcNeutralHex), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(condition_arc_, LV_OPA_TRANSP, 0);
  }

  lv_obj_t* column = lv_obj_create(parent);
  lv_obj_set_size(column, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(column, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(column, 0, 0);
  lv_obj_clear_flag(column, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(column, 8, 0);
  lv_obj_center(column);

  temp_label_ = lv_label_create(column);
  lv_label_set_text(temp_label_, "--");
  lv_obj_set_style_text_font(temp_label_, &dosis_64, 0);
  lv_obj_set_style_text_color(temp_label_, lv_color_hex(0xFFFFFF), 0);

  condition_text_label_ = lv_label_create(column);
  lv_label_set_text(condition_text_label_, "");
  lv_obj_set_style_text_font(condition_text_label_, &dosis_32, 0);
  lv_obj_set_style_text_color(condition_text_label_, lv_color_hex(0xEEEEEE), 0);

  detail_label_ = lv_label_create(column);
  lv_label_set_text(detail_label_, "");
  lv_obj_set_style_text_font(detail_label_, &dosis_20, 0);
  lv_obj_set_style_text_color(detail_label_, lv_color_hex(0xCCCCCC), 0);

  status_label_ = lv_label_create(column);
  lv_label_set_text(status_label_, "Not configured");
  lv_obj_set_width(status_label_, 320);
  lv_label_set_long_mode(status_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(status_label_, &dosis_20, 0);
  lv_obj_set_style_text_color(status_label_, lv_color_hex(0x999999), 0);
}

void TempestPlugin::update_ui() {
  if (ui_ == nullptr || temp_label_ == nullptr) {
    return;
  }

  const WeatherFlowSnapshot snapshot = client_.snapshot();
  ui_->set_plugin_pages_enabled(id(), snapshot.configured);
  if (!snapshot.configured) {
    return;
  }

  LvglLockGuard lock(200, "TempestPlugin::update_ui");
  if (!lock.locked()) {
    return;
  }

  const bool fahrenheit = fahrenheit_.load();

  if (snapshot.has_core_reading) {
    char temp_buf[16];
    std::snprintf(temp_buf, sizeof(temp_buf), "%.1f\xC2\xB0%s",
                  celsius_to_display(snapshot.air_temperature_c, fahrenheit),
                  fahrenheit ? "F" : "C");
    lv_label_set_text(temp_label_, temp_buf);

    char detail_buf[64];
    std::snprintf(detail_buf, sizeof(detail_buf), "%.0f%% RH  \xC2\xB7  %.1f mb",
                  snapshot.relative_humidity_pct, snapshot.barometric_pressure_mb);
    lv_label_set_text(detail_label_, detail_buf);

    lv_label_set_text(status_label_, snapshot.last_fetch_ok ? "" : "Last update failed - showing stale data");
  } else {
    lv_label_set_text(temp_label_, "--");
    lv_label_set_text(detail_label_, "");
    lv_label_set_text(status_label_,
                      snapshot.last_error.empty() ? "Waiting for first fetch..."
                                                   : snapshot.last_error.c_str());
  }

  if (condition_arc_ != nullptr) {
    // Prefer better_forecast's current_conditions; fall back to the nearest
    // hourly entry (same request, just a different part of the payload) if
    // current_conditions wasn't present in the response.
    std::string icon;
    std::string condition_text;
    if (snapshot.has_current_conditions) {
      icon = snapshot.current_icon;
      condition_text = snapshot.current_conditions_text;
    } else if (snapshot.has_forecast && snapshot.hourly_forecast[0].has_data) {
      icon = snapshot.hourly_forecast[0].icon;
      condition_text = snapshot.hourly_forecast[0].conditions;
    }
    const uint32_t arc_color = icon.empty() ? kConditionArcNeutralHex : condition_color_for_icon(icon);
    lv_obj_set_style_arc_color(condition_arc_, lv_color_hex(arc_color), LV_PART_INDICATOR);
    if (condition_text_label_ != nullptr) {
      lv_label_set_text(condition_text_label_, condition_text.c_str());
    }
  }

  if (snapshot.has_forecast) {
    for (uint8_t i = 0; i < kHourlyForecastCount; ++i) {
      const HourlyForecastEntry& entry = snapshot.hourly_forecast[i];
      const ForecastColumn& fc = forecast_columns_[i];
      if (fc.time_label == nullptr) {
        continue;
      }
      if (!entry.has_data) {
        lv_label_set_text(fc.time_label, "--");
        lv_label_set_text(fc.temp_label, "--");
        lv_label_set_text(fc.conditions_label, "");
        if (fc.card != nullptr) {
          lv_obj_set_style_bg_color(fc.card, lv_color_hex(kConditionArcNeutralHex), 0);
        }
        continue;
      }
      lv_label_set_text(fc.time_label, hour_label_text(entry.time_unix).c_str());
      char temp_buf[16];
      std::snprintf(temp_buf, sizeof(temp_buf), "%.0f\xC2\xB0",
                    celsius_to_display(entry.air_temperature_c, fahrenheit));
      lv_label_set_text(fc.temp_label, temp_buf);
      std::string conditions_text = entry.conditions;
      if (entry.precip_probability > 0.0) {
        char precip_buf[24];
        std::snprintf(precip_buf, sizeof(precip_buf), "\n%.0f%%", entry.precip_probability);
        conditions_text += precip_buf;
      }
      lv_label_set_text(fc.conditions_label, conditions_text.c_str());

      const uint32_t card_color =
          entry.icon.empty() ? kConditionArcNeutralHex : condition_color_for_icon(entry.icon);
      if (fc.card != nullptr) {
        lv_obj_set_style_bg_color(fc.card, lv_color_hex(card_color), 0);
      }
      lv_obj_set_style_text_color(fc.temp_label, lv_color_hex(card_color), 0);
    }
  }
}

}  // namespace infohub
