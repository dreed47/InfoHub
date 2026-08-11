#pragma once

#include "printsphere/plugin.hpp"
#include "printsphere/plugins/weather/weather_flow_client.hpp"

namespace printsphere {

// First real second Plugin implementation (weather, via WeatherFlow's cloud
// REST API for a Tempest station) — see CLAUDE.md's "Phased extraction
// sequencing plan" Phase 9/10. v1 is Web Config only: build_tile()/
// build_screen() stay no-ops, same as PrinterPlugin's today, since there's no
// generic UiShell page-registration API yet for a second plugin to hook a
// screen into (see the parked printer-selector-page investigation).
class WeatherPlugin : public Plugin {
 public:
  WeatherPlugin() = default;

  const char* id() const override { return "weather"; }
  const char* display_name() const override { return "Weather"; }

  esp_err_t init(PluginContext& ctx) override;
  void tick(uint64_t now_ms) override;
  void update_ui() override {}

  bool wants_network() const override;
  bool wants_awake() const override { return false; }
  uint32_t desired_poll_interval_ms() const override { return 5000; }

  void build_tile(lv_obj_t*) override {}
  void build_screen(lv_obj_t*) override {}
  void register_portal_routes(httpd_handle_t server) override;
  std::string portal_settings_html() const override { return {}; }
  void load_config() override;
  void save_config() override {}

  WeatherFlowClient& client() { return client_; }

 private:
  static esp_err_t handle_config_get(httpd_req_t* request);
  static esp_err_t handle_config_post(httpd_req_t* request);

  ConfigStore* config_store_ = nullptr;
  WifiManager* wifi_manager_ = nullptr;
  SetupPortal* setup_portal_ = nullptr;

  WeatherFlowClient client_{};
};

}  // namespace printsphere
