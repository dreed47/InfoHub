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
class ConfigStore;

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
  // esp_timer_get_time()-based (boot-relative, not wall-clock -- same units
  // as last_fetch_ms above), set only when at least one symbol's quote
  // actually refreshed this cycle. Drives the "Updated Xm ago" display --
  // unlike last_fetch_ms (every attempt), this never regresses on a failed
  // or budget-skipped cycle, so it always reflects how old the data
  // currently on screen really is.
  uint64_t last_success_ms = 0;
  // True if the daily API-request budget (see StockClient::configure()'s
  // daily_limit param) stopped at least one symbol's fetch this cycle.
  // Recomputed fresh every fetch_once() call -- does not stick once the
  // budget resets the next day.
  bool budget_exhausted = false;
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
// On top of the schedule, configure()'s daily_limit param enforces an
// actual NVS-persisted request counter (reset at US-Eastern midnight) so
// reconfigures/reboots can't silently exceed a user-set cap either -- see
// consume_budget_slot_if_available_locked()/record_budget_consumed_locked().
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
  // Persists the daily API-request budget counter across reboots (keys
  // "req_count"/"req_day" under this plugin's "stocks" NVS namespace) --
  // same const-ConfigStore*-for-a-client's-own-persistence pattern
  // BambuCloudClient::set_config_store() already uses. May be null (budget
  // tracking then stays in-memory only for that boot -- degrades gracefully
  // rather than crashing).
  void set_config_store(const ConfigStore* config_store) { config_store_ = config_store; }

  // Sets symbols/api_key/daily_limit and wakes the task for an immediate
  // first fetch. Empty entries in `symbols` are skipped when fetching.
  // Passing an empty api_key, or all-empty symbols, marks the client
  // unconfigured (task idles, snapshot().configured stays false).
  // Subsequent fetches follow the fixed weekday schedule (see class
  // comment), not this call. `daily_limit`: 0 = unlimited (no budget
  // check). Non-zero caps the number of requests that actually reach
  // Alpha Vantage's server per US-Eastern day (see fetch_once()) --
  // covers every request this client makes, including the immediate
  // fetch this call itself may trigger.
  void configure(const std::array<std::string, kStockSymbolCount>& symbols,
                 const std::string& api_key, uint32_t daily_limit);

  StockSnapshot snapshot() const;

 private:
  static void task_entry(void* context);
  void task_loop();
  bool fetch_once();
  bool perform_json_request(const std::string& url, int* status_code,
                            std::string* response_body);
  bool parse_quote_response(const std::string& body, StockQuote* out);
  // Both assume config_mutex_ is already held by the caller. Returns true
  // if a request is allowed to proceed (and, as a side effect, rolls the
  // counter over to a fresh US-Eastern day if one has started since the
  // last check). daily_limit_ == 0 always returns true without touching
  // the counter at all -- no NVS writes for an unlimited account.
  bool consume_budget_slot_if_available_locked();
  // Called only after a request that actually reached Alpha Vantage's
  // server (got any HTTP response back) -- see class comment on why a
  // rate-limit/bad-key response still counts.
  void record_budget_consumed_locked();

  TaskHandle_t task_handle_ = nullptr;
  const WifiManager* wifi_manager_ = nullptr;
  NetworkArbiter* network_arbiter_ = nullptr;
  const ConfigStore* config_store_ = nullptr;

  mutable std::mutex config_mutex_;
  std::array<std::string, kStockSymbolCount> symbols_{};
  std::string api_key_;
  uint32_t daily_limit_ = 0;
  uint32_t req_count_today_ = 0;
  int req_day_key_ = 0;          // 0 = not yet loaded from NVS
  bool budget_state_loaded_ = false;
  std::atomic<bool> reconfigure_requested_{false};

  mutable std::mutex snapshot_mutex_;
  StockSnapshot snapshot_{};
};

}  // namespace infohub
