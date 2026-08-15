// Portal routes for StocksPlugin, split from stocks_plugin.cpp the same way
// tempest_plugin_portal.cpp splits off TempestPlugin's routes.

#include "infohub/plugins/stocks/stocks_plugin.hpp"

#include <cstdlib>

#include "cJSON.h"
#include "infohub/config_store.hpp"
#include "infohub/portal_shared.hpp"
#include "infohub/setup_portal.hpp"
#include "infohub/ui.hpp"

namespace infohub {

namespace {
constexpr char kPluginNs[] = "stocks";

// Same empty-submission-reuses-stored-value convention as TempestPlugin's
// merge_secret -- the settings card never echoes the saved key back, so a
// blank field on submit means "keep the current one".
std::string merge_secret(const std::string& submitted, const std::string& stored) {
  return submitted.empty() ? stored : submitted;
}
}  // namespace

esp_err_t StocksPlugin::handle_config_get(httpd_req_t* request) {
  auto* plugin = static_cast<StocksPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  const StockSnapshot snapshot = plugin->client_.snapshot();

  std::string body = "{\"symbols\":[";
  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    const std::string key = "symbol" + std::to_string(i + 1);
    const std::string symbol = plugin->config_store_->load_plugin_string(kPluginNs, key.c_str());
    if (i > 0) {
      body += ",";
    }
    body += "\"" + json_escape(symbol) + "\"";
  }
  body += "],\"enabled\":";
  body += plugin->enabled() ? "true" : "false";
  body += ",\"configured\":";
  body += snapshot.configured ? "true" : "false";
  body += ",\"last_fetch_ok\":";
  body += snapshot.last_fetch_ok ? "true" : "false";
  body += ",\"last_error\":\"" + json_escape(snapshot.last_error) + "\"";
  {
    // daily_limit/req_count_today are read directly from ConfigStore (not
    // threaded through StockSnapshot) -- StockClient already persists both
    // there for its own budget tracking, no need to duplicate them on the
    // hot-path snapshot struct just for this portal display.
    const std::string daily_limit_str =
        plugin->config_store_->load_plugin_string(kPluginNs, "daily_limit");
    body += ",\"daily_limit\":";
    body += daily_limit_str.empty() ? "null" : daily_limit_str;
    const std::string req_count_str =
        plugin->config_store_->load_plugin_string(kPluginNs, "req_count");
    body += ",\"req_count_today\":" + (req_count_str.empty() ? std::string("0") : req_count_str);
  }
  body += ",\"budget_exhausted\":";
  body += snapshot.budget_exhausted ? "true" : "false";
  body += ",\"last_success_ago_s\":";
  body += snapshot.last_success_ms == 0
              ? "null"
              : std::to_string((now_ms() - snapshot.last_success_ms) / 1000);
  body += ",\"quotes\":[";
  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    if (i > 0) {
      body += ",";
    }
    const StockQuote& quote = snapshot.quotes[i];
    body += "{\"symbol\":\"" + json_escape(quote.symbol) + "\",\"has_data\":";
    body += quote.has_data ? "true" : "false";
    if (quote.has_data) {
      body += ",\"price\":" + std::to_string(quote.price);
      body += ",\"change\":" + std::to_string(quote.change);
      body += ",\"change_percent\":" + std::to_string(quote.change_percent);
    }
    body += "}";
  }
  body += "]}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t StocksPlugin::handle_config_post(httpd_req_t* request) {
  auto* plugin = static_cast<StocksPlugin*>(request->user_ctx);
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

  std::array<std::string, kStockSymbolCount> symbols{};
  if (root != nullptr) {
    const cJSON* symbols_field = cJSON_GetObjectItemCaseSensitive(root, "symbols");
    if (cJSON_IsArray(symbols_field)) {
      const int count = cJSON_GetArraySize(symbols_field);
      for (int i = 0; i < count && i < kStockSymbolCount; ++i) {
        const cJSON* item = cJSON_GetArrayItem(symbols_field, i);
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
          symbols[i] = trim_copy(std::string(item->valuestring));
        }
      }
    }
  }
  const std::string submitted_api_token = read_string_field(root, "api_token");
  const std::string daily_limit_field = trim_copy(read_string_field(root, "daily_limit"));
  cJSON_Delete(root);

  const std::string stored_api_token =
      plugin->config_store_->load_plugin_string(kPluginNs, "api_token");
  const std::string api_token = merge_secret(submitted_api_token, stored_api_token);

  // Garbage/zero/negative input is treated as "unlimited" rather than
  // rejected outright -- lenient-parse-with-sane-fallback, consistent with
  // how this codebase's other numeric fields (e.g. poll_s) handle bad
  // input elsewhere. A blank field is the expected "paid account, no cap"
  // case, not an error.
  uint32_t daily_limit = 0;
  if (!daily_limit_field.empty()) {
    const long parsed = std::strtol(daily_limit_field.c_str(), nullptr, 10);
    if (parsed > 0) {
      daily_limit = static_cast<uint32_t>(parsed);
    }
  }
  const std::string daily_limit_to_store = daily_limit == 0 ? "" : std::to_string(daily_limit);

  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    const std::string key = "symbol" + std::to_string(i + 1);
    plugin->config_store_->save_plugin_string(kPluginNs, key.c_str(), symbols[i]);
  }
  plugin->config_store_->save_plugin_string(kPluginNs, "api_token", api_token);
  plugin->config_store_->save_plugin_string(kPluginNs, "daily_limit", daily_limit_to_store);

  // Only actually push credentials/symbols to the client (and let it start
  // fetching) if the plugin is currently enabled -- otherwise a save while
  // disabled would fire an immediate fetch despite the toggle, same bug
  // load_config() had. handle_enabled_post() pushes the up-to-date stored
  // values in when the user later flips it back on.
  if (plugin->enabled()) {
    plugin->client_.configure(symbols, api_token, daily_limit);
  }

  send_json(request, "{\"status\":\"saved\"}");
  return ESP_OK;
}

// Separate from handle_config_post so toggling the checkbox can apply
// instantly without risking a partial payload wiping symbols/key -- same
// split TempestPlugin/PrinterPlugin use for their own enabled toggles.
esp_err_t StocksPlugin::handle_enabled_post(httpd_req_t* request) {
  auto* plugin = static_cast<StocksPlugin*>(request->user_ctx);
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

  // StockClient has no concept of Plugin::enabled_ -- push/clear its
  // credentials here so the toggle takes effect immediately (matches
  // load_config()/handle_config_post()'s "disabled == unconfigured, don't
  // hand the client real symbols/key" rule) rather than waiting for the
  // next scheduled slot to notice nothing changed, or worse, having a
  // stale re-enable never actually start fetching because init() ran while
  // still disabled.
  if (enabled) {
    std::array<std::string, kStockSymbolCount> symbols{};
    for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
      const std::string key = "symbol" + std::to_string(i + 1);
      symbols[i] = plugin->config_store_->load_plugin_string(kPluginNs, key.c_str());
    }
    const std::string api_token = plugin->config_store_->load_plugin_string(kPluginNs, "api_token");
    const std::string daily_limit_str =
        plugin->config_store_->load_plugin_string(kPluginNs, "daily_limit");
    uint32_t daily_limit = 0;
    if (!daily_limit_str.empty()) {
      daily_limit = static_cast<uint32_t>(std::strtoul(daily_limit_str.c_str(), nullptr, 10));
    }
    plugin->client_.configure(symbols, api_token, daily_limit);
  } else {
    plugin->client_.configure({}, "", 0);
  }

  if (!enabled && plugin->ui_ != nullptr) {
    // Application's update_ui() loop skips disabled plugins, so the pager
    // wouldn't otherwise notice the page should stop being offered -- hide
    // it immediately. This runs on SetupPortal's HTTP task, racing the main
    // task for the LVGL lock, with no next update_ui() tick to retry a lost
    // race on -- same fix as TempestPlugin's/PrinterPlugin's equivalent
    // portal handlers (see set_plugin_pages_enabled()'s lock_timeout_ms doc).
    plugin->ui_->set_plugin_pages_enabled(plugin->id(), false, /*lock_timeout_ms=*/2000);
  }

  send_json(request, "{\"status\":\"saved\"}");
  return ESP_OK;
}

void StocksPlugin::register_portal_routes(httpd_handle_t server) {
  httpd_uri_t get_uri = {};
  get_uri.uri = "/api/plugins/stocks/config";
  get_uri.method = HTTP_GET;
  get_uri.handler = &StocksPlugin::handle_config_get;
  get_uri.user_ctx = this;
  httpd_register_uri_handler(server, &get_uri);

  httpd_uri_t post_uri = {};
  post_uri.uri = "/api/plugins/stocks/config";
  post_uri.method = HTTP_POST;
  post_uri.handler = &StocksPlugin::handle_config_post;
  post_uri.user_ctx = this;
  httpd_register_uri_handler(server, &post_uri);

  httpd_uri_t enabled_uri = {};
  enabled_uri.uri = "/api/plugins/stocks/enabled";
  enabled_uri.method = HTTP_POST;
  enabled_uri.handler = &StocksPlugin::handle_enabled_post;
  enabled_uri.user_ctx = this;
  httpd_register_uri_handler(server, &enabled_uri);
}

}  // namespace infohub
