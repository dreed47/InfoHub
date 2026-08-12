#pragma once

#include <array>

#include "infohub/plugin.hpp"
#include "infohub/plugins/stocks/stock_client.hpp"

namespace infohub {

class Ui;

// Third Plugin implementation (stocks, via Alpha Vantage's GLOBAL_QUOTE REST
// API), built directly on WeatherPlugin's established shape: own client on
// its own FreeRTOS task, ConfigStore per-plugin namespace, portal routes
// split into .../config + .../enabled, one page from the generic plugin-page
// pool (page_count()/build_screen(parent, page_index)).
class StocksPlugin : public Plugin {
 public:
  StocksPlugin() = default;

  const char* id() const override { return "stocks"; }
  const char* display_name() const override { return "Stocks"; }

  esp_err_t init(PluginContext& ctx) override;
  void tick(uint64_t now_ms) override;
  void update_ui() override;

  bool wants_network() const override;
  bool wants_awake() const override { return false; }
  uint32_t desired_poll_interval_ms() const override { return 30000; }

  uint8_t page_count() const override { return 1; }
  void build_tile(lv_obj_t*) override {}
  void build_screen(lv_obj_t* parent, uint8_t page_index) override;
  void register_portal_routes(httpd_handle_t server) override;
  std::string portal_settings_html() const override { return {}; }
  void load_config() override;
  void save_config() override {}

  // Has an API key + at least one symbol saved and a successful (or
  // at-least-tried) fetch -- same "configured" flag update_ui() already
  // gates its own page visibility on, mirrors WeatherPlugin::is_configured().
  bool is_configured() const override { return client_.snapshot().configured; }

  StockClient& client() { return client_; }

 private:
  static esp_err_t handle_config_get(httpd_req_t* request);
  static esp_err_t handle_config_post(httpd_req_t* request);
  static esp_err_t handle_enabled_post(httpd_req_t* request);

  ConfigStore* config_store_ = nullptr;
  WifiManager* wifi_manager_ = nullptr;
  SetupPortal* setup_portal_ = nullptr;
  Ui* ui_ = nullptr;

  StockClient client_{};

  // Screen widgets, built once in build_screen(), updated in update_ui().
  // One row per tracked symbol: symbol label, price label, colored dot.
  struct QuoteRow {
    lv_obj_t* symbol_label = nullptr;
    lv_obj_t* price_label = nullptr;
    lv_obj_t* dot = nullptr;
  };
  std::array<QuoteRow, kStockSymbolCount> rows_{};
  lv_obj_t* status_label_ = nullptr;
};

}  // namespace infohub
