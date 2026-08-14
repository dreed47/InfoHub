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
class NetworkArbiter;

constexpr uint8_t kStockSymbolCount = 4;

// Latest fetched quote for one ticker symbol. `has_data` is true once a
// successful fetch has populated price/change; `configured` on the owning
// StockSnapshot gates whether any of this is meaningful at all. A quote
// that fails to fetch keeps the previous cycle's price/change/has_data as-is
// (see StockClient::fetch_once()) -- the screen always shows the last known
// good quote rather than blanking out on a transient failure.
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
// symbols that's 4 requests/cycle, so polling is pinned to a fixed
// weekday schedule rather than a free-running interval -- see
// kScheduleSlots in stock_client.cpp (10:00, 12:00, 14:30, 17:00 US
// Eastern time, Mon-Fri only; markets/quote data don't move on weekends).
// The schedule is pinned to true Eastern time via its own UTC-based DST
// arithmetic (see eastern_utc_offset_seconds() in stock_client.cpp) --
// independent of the device's own display timezone (Web Config's
// tz_iana/time_sync.hpp), which may be set to something else entirely.
// Requires SNTP (see time_sync.hpp) for a correct UTC clock; until that's
// synced the schedule simply never fires, but the very first configure()
// after boot always fetches once immediately regardless of the clock, so
// the screen isn't left empty.
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
  // Shared handshake-serialization slot -- see NetworkArbiter. May be null
  // (falls back to unconditional connect, same as before this existed).
  void set_network_arbiter(NetworkArbiter* arbiter) { network_arbiter_ = arbiter; }

  // Sets symbols/api_key and wakes the task for an immediate first fetch.
  // Empty entries in `symbols` are skipped when fetching. Passing an empty
  // api_key, or all-empty symbols, marks the client unconfigured (task
  // idles, snapshot().configured stays false). Subsequent fetches follow
  // the fixed weekday schedule (see class comment), not this call.
  void configure(const std::array<std::string, kStockSymbolCount>& symbols,
                 const std::string& api_key);

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
  NetworkArbiter* network_arbiter_ = nullptr;

  mutable std::mutex config_mutex_;
  std::array<std::string, kStockSymbolCount> symbols_{};
  std::string api_key_;
  std::atomic<bool> reconfigure_requested_{false};

  mutable std::mutex snapshot_mutex_;
  StockSnapshot snapshot_{};
};

}  // namespace infohub
