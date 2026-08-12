#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace infohub {

class WifiManager;

constexpr uint8_t kStockSymbolCount = 4;

// Latest fetched quote for one ticker symbol. `has_data` is true once a
// successful fetch has populated price/change; `configured` on the owning
// StockSnapshot gates whether any of this is meaningful at all.
struct StockQuote {
  std::string symbol;
  bool has_data = false;
  double price = 0.0;
  double change = 0.0;
  double change_percent = 0.0;
};

// Latest fetched quotes for up to kStockSymbolCount tracked symbols.
// `configured` is true once an API key + at least one symbol are set.
// `last_fetch_ok`/`last_error` report the outcome of the most recent poll
// cycle as a whole (a cycle fetches every configured symbol in sequence;
// a single symbol's failure doesn't fail the whole cycle, see
// StockClient::fetch_once()).
struct StockSnapshot {
  bool configured = false;
  bool last_fetch_ok = false;
  std::string last_error;
  uint64_t last_fetch_ms = 0;
  std::array<StockQuote, kStockSymbolCount> quotes{};
};

// Polls Alpha Vantage's GLOBAL_QUOTE endpoint (https://www.alphavantage.co/
// query?function=GLOBAL_QUOTE&symbol=...&apikey=...) for up to
// kStockSymbolCount symbols, one HTTP request per symbol per poll cycle, on
// its own FreeRTOS task — same own-task-for-blocking-HTTPS pattern as
// WeatherFlowClient (see weather_flow_client.hpp).
//
// Free-tier Alpha Vantage accounts are capped at 25 requests/day; with 4
// symbols that's ~6 poll cycles/day, so poll_interval_s should default to
// several hours, not weather's 5-minute cadence. Free-tier quote data itself
// also only refreshes once per trading day (end-of-day), independent of how
// often this polls — see stocks_plugin.hpp for where that's surfaced to the
// user.
class StockClient {
 public:
  StockClient() = default;
  StockClient(const StockClient&) = delete;
  StockClient& operator=(const StockClient&) = delete;

  // Starts the polling task. Safe to call multiple times — subsequent calls
  // are no-ops (same contract as WeatherFlowClient::start()).
  esp_err_t start();

  // Must be called before start() (or at least before Wi-Fi connects) — same
  // rationale as WeatherFlowClient::set_wifi_manager().
  void set_wifi_manager(const WifiManager* wifi_manager) { wifi_manager_ = wifi_manager; }

  // Sets symbols/api_key/poll interval and wakes the task for an immediate
  // fetch. Empty entries in `symbols` are skipped when fetching. Passing an
  // empty api_key, or all-empty symbols, marks the client unconfigured (task
  // idles, snapshot().configured stays false).
  void configure(const std::array<std::string, kStockSymbolCount>& symbols,
                 const std::string& api_key, uint32_t poll_interval_s);

  StockSnapshot snapshot() const;

 private:
  static void task_entry(void* context);
  void task_loop();
  bool fetch_once();
  bool perform_json_request(const std::string& url, int* status_code,
                            std::string* response_body);
  bool parse_quote_response(const std::string& body, StockQuote* out);

  TaskHandle_t task_handle_ = nullptr;
  const WifiManager* wifi_manager_ = nullptr;

  mutable std::mutex config_mutex_;
  std::array<std::string, kStockSymbolCount> symbols_{};
  std::string api_key_;
  uint32_t poll_interval_s_ = 21600;  // 6h default — see class comment re: 25 req/day free tier
  std::atomic<bool> reconfigure_requested_{false};

  mutable std::mutex snapshot_mutex_;
  StockSnapshot snapshot_{};
};

}  // namespace infohub
