#include "printsphere/plugins/weather/weather_plugin.hpp"

#include <cstdlib>

#include "esp_log.h"

namespace printsphere {

namespace {
constexpr char kTag[] = "printsphere.weather";
constexpr char kPluginNs[] = "weather";
}  // namespace

esp_err_t WeatherPlugin::init(PluginContext& ctx) {
  config_store_ = &ctx.config_store;
  wifi_manager_ = &ctx.wifi_manager;
  setup_portal_ = &ctx.setup_portal;

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
}

void WeatherPlugin::tick(uint64_t) {
  // No per-tick work — polling happens entirely on WeatherFlowClient's own
  // task; there's no on-device UI to refresh in v1.
}

bool WeatherPlugin::wants_network() const {
  return client_.snapshot().configured;
}

}  // namespace printsphere
