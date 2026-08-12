#pragma once

#include <array>
#include <atomic>

#include "infohub/plugin.hpp"
#include "infohub/plugins/weather/weather_flow_client.hpp"

namespace infohub {

class Ui;

// First real second Plugin implementation (weather, via WeatherFlow's cloud
// REST API for a Tempest station) — see CLAUDE.md's "Phased extraction
// sequencing plan" Phase 9/10. build_screen() renders into a page reserved
// from Ui's generic plugin-page pool (Ui::plugin_page_container()) via
// page_count()/register_plugin_pages() — the same mechanism any other
// non-printer plugin (e.g. a future stocks plugin) uses.
class WeatherPlugin : public Plugin {
 public:
  WeatherPlugin() = default;

  const char* id() const override { return "weather"; }
  const char* display_name() const override { return "Weather"; }

  esp_err_t init(PluginContext& ctx) override;
  void tick(uint64_t now_ms) override;
  void update_ui() override;

  bool wants_network() const override;
  bool wants_awake() const override { return false; }
  uint32_t desired_poll_interval_ms() const override { return 5000; }

  // page_index 0: temp/humidity/pressure summary. page_index 1: wind/UV/
  // precip/station-battery. page_index 2: next-4-hours forecast strip
  // (WeatherFlowClient's better_forecast fetch).
  uint8_t page_count() const override { return 3; }
  void build_tile(lv_obj_t*) override {}
  void build_screen(lv_obj_t* parent, uint8_t page_index) override;
  void register_portal_routes(httpd_handle_t server) override;
  std::string portal_settings_html() const override { return {}; }
  void load_config() override;
  void save_config() override {}

  // Has a station ID + API token saved and a successful (or at-least-tried)
  // fetch — same "configured" flag update_ui() already gates its own page
  // visibility on.
  bool is_configured() const override { return client_.snapshot().configured; }

  WeatherFlowClient& client() { return client_; }

  // Display-only unit preference -- WeatherFlowClient always fetches/stores
  // Celsius internally (confirmed: the plain observations endpoint has no
  // units param at all, unlike better_forecast which does but is fetched
  // hardcoded to units_temp=c for the same reason -- one internal unit,
  // convert only at render time). Saved/applied alongside station_id/poll_s
  // in handle_config_post, same instant-apply shape as
  // WeatherFlowClient::configure() -- no reboot/re-fetch needed, the next
  // update_ui() tick just renders in the new unit.
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

  WeatherFlowClient client_{};

  // Page 0 widgets, built once in build_screen(), updated in update_ui().
  lv_obj_t* temp_label_ = nullptr;
  lv_obj_t* detail_label_ = nullptr;
  lv_obj_t* status_label_ = nullptr;

  // Page 1 widgets — wind/UV/precip/station-battery.
  lv_obj_t* extra_title_label_ = nullptr;
  lv_obj_t* extra_detail_label_ = nullptr;

  // Page 2 widgets — one column per forecast hour: time, temp, conditions
  // (text, not icons — see the plan doc for why).
  struct ForecastColumn {
    lv_obj_t* time_label = nullptr;
    lv_obj_t* temp_label = nullptr;
    lv_obj_t* conditions_label = nullptr;
  };
  std::array<ForecastColumn, kHourlyForecastCount> forecast_columns_{};
};

}  // namespace infohub
