// Portal routes for GeoWeatherPlugin, split from geoweather_plugin.cpp the
// same way tempest_plugin_portal.cpp splits off TempestPlugin's routes.

#include "infohub/plugins/geoweather/geoweather_plugin.hpp"

#include <cstdlib>

#include "cJSON.h"
#include "infohub/config_store.hpp"
#include "infohub/portal_shared.hpp"
#include "infohub/setup_portal.hpp"
#include "infohub/ui.hpp"

namespace infohub {

namespace {
constexpr char kPluginNs[] = "geoweather";
}  // namespace

esp_err_t GeoWeatherPlugin::handle_config_get(httpd_req_t* request) {
  auto* plugin = static_cast<GeoWeatherPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  const std::string location = plugin->config_store_->load_plugin_string(kPluginNs, "location");
  const std::string poll_s_str = plugin->config_store_->load_plugin_string(kPluginNs, "poll_s");
  const GeoWeatherSnapshot snapshot = plugin->client_.snapshot();

  std::string body = "{\"location\":\"";
  body += json_escape(location);
  body += "\",\"poll_s\":";
  body += poll_s_str.empty() ? "600" : poll_s_str;
  body += ",\"units\":\"";
  body += plugin->fahrenheit() ? "f" : "c";
  body += "\",\"enabled\":";
  body += plugin->enabled() ? "true" : "false";
  body += ",\"configured\":";
  body += snapshot.configured ? "true" : "false";
  body += ",\"location_resolved\":";
  body += snapshot.location_resolved ? "true" : "false";
  body += ",\"resolved_name\":\"" + json_escape(snapshot.resolved_display_name) + "\"";
  body += ",\"last_fetch_ok\":";
  body += snapshot.last_fetch_ok ? "true" : "false";
  body += ",\"last_error\":\"" + json_escape(snapshot.last_error) + "\"";
  body += ",\"has_current\":";
  body += snapshot.has_current ? "true" : "false";
  if (snapshot.has_current) {
    body += ",\"current_temperature_c\":" + std::to_string(snapshot.current_temperature_c);
    body += ",\"current_humidity_pct\":" + std::to_string(snapshot.current_humidity_pct);
    body += ",\"current_pressure_hpa\":" + std::to_string(snapshot.current_pressure_hpa);
    body += ",\"current_weather_code\":" + std::to_string(snapshot.current_weather_code);
  }
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t GeoWeatherPlugin::handle_config_post(httpd_req_t* request) {
  auto* plugin = static_cast<GeoWeatherPlugin*>(request->user_ctx);
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

  const std::string submitted_location = trim_copy(read_string_field(root, "location"));
  const std::string poll_s_field = trim_copy(read_string_field(root, "poll_s"));
  const std::string units_field = trim_copy(read_string_field(root, "units"));
  cJSON_Delete(root);

  uint32_t poll_interval_s = 600;
  if (!poll_s_field.empty()) {
    poll_interval_s = static_cast<uint32_t>(std::strtoul(poll_s_field.c_str(), nullptr, 10));
    if (poll_interval_s < 300) {
      poll_interval_s = 300;  // don't allow hammering Open-Meteo's free API faster than 1/5min
    }
  }

  const bool fahrenheit = units_field == "f";

  plugin->config_store_->save_plugin_string(kPluginNs, "location", submitted_location);
  plugin->config_store_->save_plugin_string(kPluginNs, "poll_s", std::to_string(poll_interval_s));
  plugin->config_store_->save_plugin_string(kPluginNs, "units", fahrenheit ? "f" : "c");

  // Only push the new location to the client (and let it start
  // geocoding/fetching) if the plugin is currently enabled -- same
  // enabled-gating rule StocksPlugin's handle_config_post uses, avoiding a
  // save while disabled firing a fetch anyway. Geocoding itself never
  // happens synchronously in this handler (would block the portal's httpd
  // worker on a second external HTTPS round-trip) -- client_.configure()
  // just sets its reconfigure flag; the client's own task geocodes within
  // ~1s, and the 5s Web Config status poll surfaces the result shortly
  // after.
  if (plugin->enabled()) {
    plugin->client_.configure(submitted_location, poll_interval_s);
  }
  plugin->set_fahrenheit(fahrenheit);

  send_json(request, "{\"status\":\"saved\"}");
  return ESP_OK;
}

// Separate from handle_config_post so toggling the checkbox can apply
// instantly without risking a partial payload wiping the location -- same
// split TempestPlugin/StocksPlugin use for their own enabled toggles.
esp_err_t GeoWeatherPlugin::handle_enabled_post(httpd_req_t* request) {
  auto* plugin = static_cast<GeoWeatherPlugin*>(request->user_ctx);
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

  // GeoWeatherClient has no concept of Plugin::enabled_ -- push/clear its
  // location here so the toggle actually stops (or resumes) polling
  // immediately, same rule StocksPlugin's equivalent handler follows (and
  // that TempestPlugin's own handler currently does not -- see this
  // plugin's design notes).
  if (enabled) {
    const std::string location = plugin->config_store_->load_plugin_string(kPluginNs, "location");
    const std::string poll_s_str = plugin->config_store_->load_plugin_string(kPluginNs, "poll_s");
    uint32_t poll_interval_s = 600;
    if (!poll_s_str.empty()) {
      poll_interval_s = static_cast<uint32_t>(std::strtoul(poll_s_str.c_str(), nullptr, 10));
    }
    plugin->client_.configure(location, poll_interval_s);
  } else {
    plugin->client_.configure("", 600);
  }

  if (!enabled && plugin->ui_ != nullptr) {
    // Same lost-race-on-LVGL-lock rationale as TempestPlugin's/StocksPlugin's
    // equivalent handlers (see set_plugin_pages_enabled()'s lock_timeout_ms
    // doc).
    plugin->ui_->set_plugin_pages_enabled(plugin->id(), false, /*lock_timeout_ms=*/2000);
  }

  send_json(request, "{\"status\":\"saved\"}");
  return ESP_OK;
}

void GeoWeatherPlugin::register_portal_routes(httpd_handle_t server) {
  httpd_uri_t get_uri = {};
  get_uri.uri = "/api/plugins/geoweather/config";
  get_uri.method = HTTP_GET;
  get_uri.handler = &GeoWeatherPlugin::handle_config_get;
  get_uri.user_ctx = this;
  httpd_register_uri_handler(server, &get_uri);

  httpd_uri_t post_uri = {};
  post_uri.uri = "/api/plugins/geoweather/config";
  post_uri.method = HTTP_POST;
  post_uri.handler = &GeoWeatherPlugin::handle_config_post;
  post_uri.user_ctx = this;
  httpd_register_uri_handler(server, &post_uri);

  httpd_uri_t enabled_uri = {};
  enabled_uri.uri = "/api/plugins/geoweather/enabled";
  enabled_uri.method = HTTP_POST;
  enabled_uri.handler = &GeoWeatherPlugin::handle_enabled_post;
  enabled_uri.user_ctx = this;
  httpd_register_uri_handler(server, &enabled_uri);
}

}  // namespace infohub
