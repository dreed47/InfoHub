// Portal routes for WeatherPlugin, split from weather_plugin.cpp the same
// way printer_plugin_portal.cpp splits off PrinterPlugin's routes — keeps
// HTTP handler code separate from tick()/init() lifecycle code.

#include "infohub/plugins/weather/weather_plugin.hpp"

#include <cstdlib>

#include "cJSON.h"
#include "infohub/config_store.hpp"
#include "infohub/portal_shared.hpp"
#include "infohub/setup_portal.hpp"
#include "infohub/ui.hpp"

namespace infohub {

namespace {
constexpr char kPluginNs[] = "weather";

// Same empty-submission-reuses-stored-value convention as PrinterPlugin's
// merge_cloud_credentials — the settings card never echoes the saved token
// back, so a blank field on submit means "keep the current one".
std::string merge_secret(const std::string& submitted, const std::string& stored) {
  return submitted.empty() ? stored : submitted;
}
}  // namespace

esp_err_t WeatherPlugin::handle_config_get(httpd_req_t* request) {
  auto* plugin = static_cast<WeatherPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  const std::string station_id = plugin->config_store_->load_plugin_string(kPluginNs, "station_id");
  const std::string poll_s_str = plugin->config_store_->load_plugin_string(kPluginNs, "poll_s");
  const WeatherFlowSnapshot snapshot = plugin->client_.snapshot();

  std::string body = "{\"station_id\":\"";
  body += json_escape(station_id);
  body += "\",\"poll_s\":";
  body += poll_s_str.empty() ? "300" : poll_s_str;
  body += ",\"enabled\":";
  body += plugin->enabled() ? "true" : "false";
  body += ",\"configured\":";
  body += snapshot.configured ? "true" : "false";
  body += ",\"last_fetch_ok\":";
  body += snapshot.last_fetch_ok ? "true" : "false";
  body += ",\"last_error\":\"" + json_escape(snapshot.last_error) + "\"";
  body += ",\"has_core_reading\":";
  body += snapshot.has_core_reading ? "true" : "false";
  if (snapshot.has_core_reading) {
    body += ",\"air_temperature_c\":" + std::to_string(snapshot.air_temperature_c);
    body += ",\"relative_humidity_pct\":" + std::to_string(snapshot.relative_humidity_pct);
    body += ",\"barometric_pressure_mb\":" + std::to_string(snapshot.barometric_pressure_mb);
  }
  if (snapshot.has_wind) {
    body += ",\"wind_avg_mps\":" + std::to_string(snapshot.wind_avg_mps);
    body += ",\"wind_gust_mps\":" + std::to_string(snapshot.wind_gust_mps);
  }
  if (snapshot.has_uv) {
    body += ",\"uv_index\":" + std::to_string(snapshot.uv_index);
  }
  if (snapshot.has_battery) {
    body += ",\"battery_volts\":" + std::to_string(snapshot.battery_volts);
  }
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t WeatherPlugin::handle_config_post(httpd_req_t* request) {
  auto* plugin = static_cast<WeatherPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  const esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const std::string submitted_station_id = trim_copy(read_string_field(root, "station_id"));
  const std::string submitted_api_token = read_string_field(root, "api_token");
  const std::string poll_s_field = trim_copy(read_string_field(root, "poll_s"));
  cJSON_Delete(root);

  const std::string stored_api_token =
      plugin->config_store_->load_plugin_string(kPluginNs, "api_token");
  const std::string api_token = merge_secret(submitted_api_token, stored_api_token);

  uint32_t poll_interval_s = 300;
  if (!poll_s_field.empty()) {
    poll_interval_s = static_cast<uint32_t>(std::strtoul(poll_s_field.c_str(), nullptr, 10));
    if (poll_interval_s < 60) {
      poll_interval_s = 60;  // don't allow hammering WeatherFlow's API faster than 1/min
    }
  }

  plugin->config_store_->save_plugin_string(kPluginNs, "station_id", submitted_station_id);
  plugin->config_store_->save_plugin_string(kPluginNs, "api_token", api_token);
  plugin->config_store_->save_plugin_string(kPluginNs, "poll_s", std::to_string(poll_interval_s));

  plugin->client_.configure(submitted_station_id, api_token, poll_interval_s);

  send_json(request, "{\"status\":\"saved\"}");
  return ESP_OK;
}

// Separate from handle_config_post (which resaves station_id/api_token/poll_s
// unconditionally on every submit) so toggling the checkbox can apply
// instantly without risking a partial payload wiping those fields — same
// split PrinterPlugin uses for its own enabled toggle.
esp_err_t WeatherPlugin::handle_enabled_post(httpd_req_t* request) {
  auto* plugin = static_cast<WeatherPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  const esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }
  const bool enabled = read_bool_field(root, "enabled", plugin->enabled());
  cJSON_Delete(root);

  plugin->config_store_->save_plugin_string(kPluginNs, "enabled", enabled ? "1" : "0");
  plugin->set_enabled(enabled);
  if (!enabled && plugin->ui_ != nullptr) {
    // Application's update_ui() loop skips disabled plugins, so the pager
    // wouldn't otherwise notice the page should stop being offered — hide it
    // immediately rather than leaving it enabled at its last-known state.
    // This runs on SetupPortal's HTTP task, racing the main task for the
    // LVGL lock, with no next update_ui() tick to retry a lost race on (see
    // set_plugin_pages_enabled()'s lock_timeout_ms doc) — same fix as
    // PrinterPlugin's equivalent portal handler.
    plugin->ui_->set_plugin_pages_enabled(plugin->id(), false, /*lock_timeout_ms=*/2000);
  }

  send_json(request, "{\"status\":\"saved\"}");
  return ESP_OK;
}

void WeatherPlugin::register_portal_routes(httpd_handle_t server) {
  httpd_uri_t get_uri = {};
  get_uri.uri = "/api/plugins/weather/config";
  get_uri.method = HTTP_GET;
  get_uri.handler = &WeatherPlugin::handle_config_get;
  get_uri.user_ctx = this;
  httpd_register_uri_handler(server, &get_uri);

  httpd_uri_t post_uri = {};
  post_uri.uri = "/api/plugins/weather/config";
  post_uri.method = HTTP_POST;
  post_uri.handler = &WeatherPlugin::handle_config_post;
  post_uri.user_ctx = this;
  httpd_register_uri_handler(server, &post_uri);

  httpd_uri_t enabled_uri = {};
  enabled_uri.uri = "/api/plugins/weather/enabled";
  enabled_uri.method = HTTP_POST;
  enabled_uri.handler = &WeatherPlugin::handle_enabled_post;
  enabled_uri.user_ctx = this;
  httpd_register_uri_handler(server, &enabled_uri);
}

}  // namespace infohub
