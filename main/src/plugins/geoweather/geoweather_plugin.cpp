#include "infohub/plugins/geoweather/geoweather_plugin.hpp"

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

extern "C" {
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_32;
extern const lv_font_t dosis_40;
extern const lv_font_t dosis_64;
}

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.geoweather";
constexpr char kPluginNs[] = "geoweather";
constexpr int kConditionArcDiameter = board::kDisplayWidth - 60;
constexpr int kConditionArcWidth = 18;
constexpr uint32_t kConditionArcNeutralHex = 0x444444;

// WMO weather interpretation codes (Open-Meteo's `weather_code` field,
// 0-99) -- a completely different vocabulary from WeatherFlow's icon
// strings, so this can't reuse TempestPlugin's condition_color_for_icon().
// No "windy" category -- WMO codes have no wind-specific value.
uint32_t condition_color_for_wmo_code(int code) {
  if (code == 0) {
    return 0xFFC940;  // clear -- gold
  }
  if (code == 1 || code == 2) {
    return 0xB0BEC5;  // mostly clear / partly cloudy -- light grey-blue
  }
  if (code == 3) {
    return 0x78909C;  // overcast -- darker grey-blue
  }
  if (code == 45 || code == 48) {
    return 0x9AA5B1;  // fog -- muted grey
  }
  if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
    return 0x3B82F6;  // drizzle/rain (incl. freezing) and rain showers -- blue
  }
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
    return 0x7FD8F5;  // snow/snow showers -- icy cyan
  }
  if (code >= 95 && code <= 99) {
    return 0x8E44AD;  // thunderstorm (incl. hail) -- deep purple
  }
  return kConditionArcNeutralHex;  // unrecognized/absent -- neutral, not a guess
}

// Open-Meteo gives no free-text condition string like WeatherFlow does --
// the plugin renders its own short label from the numeric code.
const char* wmo_code_text(int code) {
  switch (code) {
    case 0:
      return "Clear";
    case 1:
      return "Mostly Clear";
    case 2:
      return "Partly Cloudy";
    case 3:
      return "Overcast";
    case 45:
    case 48:
      return "Fog";
    case 51:
    case 53:
    case 55:
      return "Drizzle";
    case 56:
    case 57:
      return "Freezing Drizzle";
    case 61:
    case 63:
    case 65:
      return "Rain";
    case 66:
    case 67:
      return "Freezing Rain";
    case 71:
    case 73:
    case 75:
    case 77:
      return "Snow";
    case 80:
    case 81:
    case 82:
      return "Rain Showers";
    case 85:
    case 86:
      return "Snow Showers";
    case 95:
      return "Thunderstorm";
    case 96:
    case 99:
      return "Thunderstorm w/ Hail";
    default:
      return "Unknown";
  }
}

// "3pm", "11am" — same localtime_r-based pattern as TempestPlugin's own copy
// (not worth a shared header for 2 call sites).
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

// GeoWeatherClient always stores Celsius internally -- this is the one place
// the display conversion happens, applied wherever a temperature gets
// rendered.
double celsius_to_display(double celsius_value, bool fahrenheit) {
  return fahrenheit ? (celsius_value * 9.0 / 5.0 + 32.0) : celsius_value;
}
}  // namespace

esp_err_t GeoWeatherPlugin::init(PluginContext& ctx) {
  config_store_ = &ctx.config_store;
  wifi_manager_ = &ctx.wifi_manager;
  setup_portal_ = &ctx.setup_portal;
  ui_ = &ctx.ui;

  client_.set_wifi_manager(wifi_manager_);
  client_.set_network_arbiter(&ctx.network_arbiter);
  load_config();

  const esp_err_t err = client_.start();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "GeoWeatherClient task start failed: %s", esp_err_to_name(err));
  }
  return err;
}

void GeoWeatherPlugin::load_config() {
  if (config_store_ == nullptr) {
    return;
  }
  const std::string location = config_store_->load_plugin_string(kPluginNs, "location");
  const std::string poll_s_str = config_store_->load_plugin_string(kPluginNs, "poll_s");
  uint32_t poll_interval_s = 600;
  if (!poll_s_str.empty()) {
    poll_interval_s = static_cast<uint32_t>(std::strtoul(poll_s_str.c_str(), nullptr, 10));
  }
  client_.configure(location, poll_interval_s);

  set_fahrenheit(config_store_->load_plugin_string(kPluginNs, "units") == "f");
  set_enabled(config_store_->load_plugin_string(kPluginNs, "enabled") != "0");
}

void GeoWeatherPlugin::tick(uint64_t) {
  // No per-tick work — polling happens entirely on GeoWeatherClient's own
  // task; update_ui() (called every Application loop iteration regardless of
  // this) is where the fetched snapshot gets pushed to the screen.
}

bool GeoWeatherPlugin::wants_network() const {
  return client_.snapshot().configured;
}

void GeoWeatherPlugin::build_screen(lv_obj_t* parent, uint8_t page_index) {
  if (parent == nullptr) {
    return;
  }

  LvglLockGuard lock(3000, "GeoWeatherPlugin::build_screen");
  if (!lock.locked()) {
    return;
  }

  if (page_index == 1) {
    // Same fixed-4-card-row layout as TempestPlugin's page 1 -- see that
    // file's build_screen() for the design rationale.
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, board::kDisplayWidth - 24, 300);
    lv_obj_center(row);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);

    for (uint8_t i = 0; i < kGeoHourlyForecastCount; ++i) {
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

void GeoWeatherPlugin::update_ui() {
  if (ui_ == nullptr || temp_label_ == nullptr) {
    return;
  }

  const GeoWeatherSnapshot snapshot = client_.snapshot();
  ui_->set_plugin_pages_enabled(id(), snapshot.configured);
  if (!snapshot.configured) {
    return;
  }

  LvglLockGuard lock(200, "GeoWeatherPlugin::update_ui");
  if (!lock.locked()) {
    return;
  }

  const bool fahrenheit = fahrenheit_.load();

  if (snapshot.has_current) {
    char temp_buf[16];
    std::snprintf(temp_buf, sizeof(temp_buf), "%.1f\xC2\xB0%s",
                  celsius_to_display(snapshot.current_temperature_c, fahrenheit),
                  fahrenheit ? "F" : "C");
    lv_label_set_text(temp_label_, temp_buf);

    char detail_buf[64];
    std::snprintf(detail_buf, sizeof(detail_buf), "%.0f%% RH  \xC2\xB7  %.1f hPa",
                  snapshot.current_humidity_pct, snapshot.current_pressure_hpa);
    lv_label_set_text(detail_label_, detail_buf);
  } else {
    lv_label_set_text(temp_label_, "--");
    lv_label_set_text(detail_label_, "");
  }

  // Three-way status: not-resolved (show error/"resolving"), resolved but no
  // fetch yet, or a failed fetch -- unlike TempestPlugin's two-way status,
  // since geocoding is an extra async step this plugin has that tempest
  // doesn't.
  if (!snapshot.location_resolved) {
    lv_label_set_text(status_label_,
                      snapshot.last_error.empty() ? "Resolving location..." : snapshot.last_error.c_str());
  } else if (!snapshot.has_current) {
    lv_label_set_text(status_label_, "Waiting for first fetch...");
  } else if (!snapshot.last_fetch_ok) {
    lv_label_set_text(status_label_, "Last update failed - showing stale data");
  } else {
    lv_label_set_text(status_label_, "");
  }

  if (condition_arc_ != nullptr) {
    const uint32_t arc_color = snapshot.has_current
                                    ? condition_color_for_wmo_code(snapshot.current_weather_code)
                                    : kConditionArcNeutralHex;
    lv_obj_set_style_arc_color(condition_arc_, lv_color_hex(arc_color), LV_PART_INDICATOR);
    if (condition_text_label_ != nullptr) {
      lv_label_set_text(condition_text_label_,
                        snapshot.has_current ? wmo_code_text(snapshot.current_weather_code) : "");
    }
  }

  if (snapshot.has_forecast) {
    for (uint8_t i = 0; i < kGeoHourlyForecastCount; ++i) {
      const GeoHourlyForecastEntry& entry = snapshot.hourly_forecast[i];
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
                    celsius_to_display(entry.temperature_c, fahrenheit));
      lv_label_set_text(fc.temp_label, temp_buf);

      std::string conditions_text = wmo_code_text(entry.weather_code);
      if (entry.precip_probability > 0.0) {
        char precip_buf[24];
        std::snprintf(precip_buf, sizeof(precip_buf), "\n%.0f%%", entry.precip_probability);
        conditions_text += precip_buf;
      }
      lv_label_set_text(fc.conditions_label, conditions_text.c_str());

      const uint32_t card_color = condition_color_for_wmo_code(entry.weather_code);
      if (fc.card != nullptr) {
        lv_obj_set_style_bg_color(fc.card, lv_color_hex(card_color), 0);
      }
      lv_obj_set_style_text_color(fc.temp_label, lv_color_hex(card_color), 0);
    }
  }
}

}  // namespace infohub
