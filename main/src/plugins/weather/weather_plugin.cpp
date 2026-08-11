#include "infohub/plugins/weather/weather_plugin.hpp"

#include <cstdio>
#include <cstdlib>

#include "esp_log.h"
#include "infohub/ui.hpp"

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
extern const lv_font_t dosis_40;
}

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.weather";
constexpr char kPluginNs[] = "weather";
}  // namespace

esp_err_t WeatherPlugin::init(PluginContext& ctx) {
  config_store_ = &ctx.config_store;
  wifi_manager_ = &ctx.wifi_manager;
  setup_portal_ = &ctx.setup_portal;
  ui_ = &ctx.ui;

  client_.set_wifi_manager(wifi_manager_);
  load_config();

  const esp_err_t err = client_.start();
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "WeatherFlowClient task start failed: %s", esp_err_to_name(err));
  }
  return err;
}

void WeatherPlugin::load_config() {
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

  set_enabled(config_store_->load_plugin_string(kPluginNs, "enabled") != "0");
}

void WeatherPlugin::tick(uint64_t) {
  // No per-tick work — polling happens entirely on WeatherFlowClient's own
  // task; update_ui() (called every Application loop iteration regardless of
  // this) is where the fetched snapshot gets pushed to the screen.
}

bool WeatherPlugin::wants_network() const {
  return client_.snapshot().configured;
}

void WeatherPlugin::build_screen(lv_obj_t* parent) {
  if (parent == nullptr) {
    return;
  }

  if (bsp_display_lock(3000) != ESP_OK) {
    ESP_LOGW(kTag, "LVGL lock failed building weather screen");
    return;
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
  lv_obj_set_style_text_font(temp_label_, &dosis_40, 0);
  lv_obj_set_style_text_color(temp_label_, lv_color_hex(0xFFFFFF), 0);

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

  bsp_display_unlock();
}

void WeatherPlugin::update_ui() {
  if (ui_ == nullptr || temp_label_ == nullptr) {
    return;
  }

  const WeatherFlowSnapshot snapshot = client_.snapshot();
  ui_->set_plugin_page_enabled(snapshot.configured);
  if (!snapshot.configured) {
    return;
  }

  if (bsp_display_lock(200) != ESP_OK) {
    return;
  }

  if (snapshot.has_core_reading) {
    char temp_buf[16];
    std::snprintf(temp_buf, sizeof(temp_buf), "%.1f\xC2\xB0" "C", snapshot.air_temperature_c);
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

  bsp_display_unlock();
}

}  // namespace infohub
