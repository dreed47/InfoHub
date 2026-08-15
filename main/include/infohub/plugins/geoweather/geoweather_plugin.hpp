#pragma once

#include <array>
#include <atomic>

#include "infohub/plugin.hpp"
#include "infohub/plugins/geoweather/geo_weather_client.hpp"

namespace infohub {

class Ui;

// Generic, no-hardware-required weather plugin: free-text location (e.g.
// "Austin, TX") resolved via Open-Meteo's free geocoding API, then polled
// via Open-Meteo's free forecast API -- no API key or account needed. This
// is the plugin most users actually want; see TempestPlugin for the
// specialty variant that requires owning a WeatherFlow Tempest station.
// Structurally mirrors TempestPlugin closely (same page layout, same widget
// shapes) -- see that plugin's build_screen()/update_ui() for the design
// this one reuses.
class GeoWeatherPlugin : public Plugin {
 public:
  GeoWeatherPlugin() = default;

  const char* id() const override { return "geoweather"; }
  const char* display_name() const override { return "Weather"; }

  esp_err_t init(PluginContext& ctx) override;
  void tick(uint64_t now_ms) override;
  void update_ui() override;

  bool wants_network() const override;
  bool wants_awake() const override { return false; }
  uint32_t desired_poll_interval_ms() const override { return 5000; }

  // page_index 0: current conditions summary. page_index 1: next-4-hours
  // forecast strip (Open-Meteo's hourly forecast).
  uint8_t page_count() const override { return 2; }
  void build_tile(lv_obj_t*) override {}
  void build_screen(lv_obj_t* parent, uint8_t page_index) override;
  void register_portal_routes(httpd_handle_t server) override;
  std::string portal_settings_html() const override { return {}; }
  void load_config() override;
  void save_config() override {}

  // Has a non-empty location string saved and a successful (or
  // at-least-tried) fetch — same "configured" flag update_ui() already gates
  // its own page visibility on.
  bool is_configured() const override { return client_.snapshot().configured; }

  GeoWeatherClient& client() { return client_; }

  // Display-only unit preference -- GeoWeatherClient always fetches/stores
  // Celsius internally, convert only at render time (same discipline as
  // TempestPlugin/WeatherFlowClient).
  bool fahrenheit() const { return fahrenheit_.load(); }
  void set_fahrenheit(bool fahrenheit) { fahrenheit_.store(fahrenheit); }

 private:
  static esp_err_t handle_config_get(httpd_req_t* request);
  static esp_err_t handle_config_post(httpd_req_t* request);
  static esp_err_t handle_enabled_post(httpd_req_t* request);

  ConfigStore* config_store_ = nullptr;
  WifiManager* wifi_manager_ = nullptr;
  SetupPortal* setup_portal_ = nullptr;
  Ui* ui_ = nullptr;

  std::atomic<bool> fahrenheit_{false};

  GeoWeatherClient client_{};

  // Page 0 widgets, built once in build_screen(), updated in update_ui().
  // condition_arc_ is a full-circle ring (built first, so it draws behind
  // the text column) colored per current_weather_code -- see
  // condition_color_for_wmo_code() in geoweather_plugin.cpp.
  lv_obj_t* condition_arc_ = nullptr;
  lv_obj_t* temp_label_ = nullptr;
  lv_obj_t* condition_text_label_ = nullptr;
  lv_obj_t* detail_label_ = nullptr;
  lv_obj_t* status_label_ = nullptr;

  // Page 1 widgets — one card per forecast hour: time, temp, conditions.
  // card is the colored background tile, recolored per entry.weather_code
  // in update_ui() since the code isn't known yet at build_screen() time.
  struct ForecastColumn {
    lv_obj_t* card = nullptr;
    lv_obj_t* time_label = nullptr;
    lv_obj_t* temp_label = nullptr;
    lv_obj_t* conditions_label = nullptr;
  };
  std::array<ForecastColumn, kGeoHourlyForecastCount> forecast_columns_{};
};

}  // namespace infohub
