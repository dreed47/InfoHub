#include "infohub/plugins/stocks/stock_client.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <ctime>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "infohub/config_store.hpp"
#include "infohub/network_arbiter.hpp"
#include "infohub/wifi_manager.hpp"

// Alpha Vantage's cert chain (alphavantage.co -> Google Trust Services WE1 ->
// GTS Root R4) isn't covered by this project's pruned "common" mbedTLS cert
// bundle (esp_crt_bundle_attach fails with "No matching trusted root
// certificate found" -- confirmed on-device). Pin the root directly instead,
// same EMBED_TXTFILES pattern as the printer plugin's Bambu certs (see
// printer_client.cpp) -- gts_root_r4.cert is embedded via main/CMakeLists.txt.
extern const uint8_t gts_root_r4_cert_start[] asm("_binary_gts_root_r4_cert_start");
extern const uint8_t gts_root_r4_cert_end[] asm("_binary_gts_root_r4_cert_end");

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.stocks";
// Same NVS namespace StocksPlugin/its portal handlers use for "symbol1"
// etc. (kPluginNs in stocks_plugin.cpp/stocks_plugin_portal.cpp) -- the
// budget counter is client-owned persistence, same shape as
// BambuCloudClient's own config_store_ usage, but lives in the plugin's
// existing namespace rather than inventing a new one.
constexpr char kPluginNs[] = "stocks";
constexpr size_t kMaxJsonResponseBytes = 8U * 1024U;
constexpr char kUrlPrefix[] = "https://www.alphavantage.co/query?function=GLOBAL_QUOTE&symbol=";

uint64_t now_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

struct ScheduleSlot {
  int hour;
  int minute;
};
// US Eastern time -- these are Alpha Vantage/NYSE trading-hours checkpoints,
// not tied to the user's own display timezone. Weekdays only (tm_wday 1-5)
// -- see class comment in stock_client.hpp re: Alpha Vantage's 25 req/day
// free-tier cap and end-of-day-only data.
constexpr ScheduleSlot kScheduleSlots[] = {{10, 0}, {12, 0}, {14, 30}, {17, 0}};

// Returns the UTC timestamp of the Nth (1-based) given weekday (0=Sunday) of
// `month` in `year`, at midnight UTC.
std::time_t nth_weekday_utc(int year, int month, int weekday, int nth) {
  struct tm t {};
  t.tm_year = year - 1900;
  t.tm_mon = month - 1;
  t.tm_mday = 1;
  const std::time_t first_of_month = timegm(&t);
  struct tm first_tm {};
  gmtime_r(&first_of_month, &first_tm);
  const int day_of_first_match = 1 + ((weekday - first_tm.tm_wday + 7) % 7);
  t.tm_mday = day_of_first_match + (nth - 1) * 7;
  return timegm(&t);
}

// US Eastern DST rule (unchanged since 2007): EDT (UTC-4) from the 2nd
// Sunday of March at 2:00am local (07:00 UTC) through the 1st Sunday of
// November at 2:00am local (06:00 UTC); EST (UTC-5) otherwise. Computed
// directly from UTC arithmetic (not via America/New_York's POSIX rule in
// time_sync.cpp / the process-wide TZ env var) because this must stay
// correct at true Eastern time even when the device's own display timezone
// (Web Config's tz_iana) is set to something else -- mutating the global TZ
// from this task would also race any other task calling localtime_r()
// concurrently (e.g. printer ETA text).
int eastern_utc_offset_seconds(std::time_t utc_now) {
  struct tm utc_tm {};
  gmtime_r(&utc_now, &utc_tm);
  const int year = utc_tm.tm_year + 1900;
  const std::time_t spring_utc = nth_weekday_utc(year, 3, 0, 2) + 7 * 3600;   // 2am EST -> EDT
  const std::time_t fall_utc = nth_weekday_utc(year, 11, 0, 1) + 6 * 3600;    // 2am EDT -> EST
  const bool is_dst = utc_now >= spring_utc && utc_now < fall_utc;
  return is_dst ? -4 * 3600 : -5 * 3600;
}

// Returns the UTC timestamp of the most recent scheduled slot (Eastern time,
// see kScheduleSlots) at or before `now`, or 0 if none has occurred yet
// today (Eastern) or today (Eastern) is a weekend. A slot only counts as
// "due" if it's newer than `last_fetch_slot_ts` (the slot that triggered the
// previous fetch) -- callers compare the two so each slot fires exactly
// once.
std::time_t latest_due_slot(std::time_t now) {
  const int offset_s = eastern_utc_offset_seconds(now);
  // Adding the Eastern UTC offset to a UTC epoch and decomposing it with
  // gmtime_r (not localtime_r) yields Eastern wall-clock fields without
  // touching the global TZ state -- gmtime_r never consults TZ.
  const std::time_t eastern_shifted = now + offset_s;
  struct tm eastern_tm {};
  if (gmtime_r(&eastern_shifted, &eastern_tm) == nullptr) {
    return 0;
  }
  if (eastern_tm.tm_wday == 0 || eastern_tm.tm_wday == 6) {
    return 0;  // Sunday / Saturday Eastern -- markets closed, nothing to fetch
  }
  std::time_t best = 0;
  for (const ScheduleSlot& slot : kScheduleSlots) {
    struct tm slot_tm = eastern_tm;
    slot_tm.tm_hour = slot.hour;
    slot_tm.tm_min = slot.minute;
    slot_tm.tm_sec = 0;
    const std::time_t slot_shifted = timegm(&slot_tm);
    const std::time_t slot_utc = slot_shifted - offset_s;
    if (slot_utc <= now && slot_utc > best) {
      best = slot_utc;
    }
  }
  return best;
}

// YYYYMMDD in US-Eastern wall-clock terms (see eastern_utc_offset_seconds
// above) -- used purely to detect a day rollover for the daily
// request-budget counter. Alpha Vantage's quota resets at US Eastern
// midnight, not UTC midnight, so this deliberately doesn't just use
// std::time(nullptr)'s own UTC date fields.
int eastern_date_key(std::time_t now) {
  const int offset_s = eastern_utc_offset_seconds(now);
  const std::time_t eastern_shifted = now + offset_s;
  struct tm eastern_tm {};
  if (gmtime_r(&eastern_shifted, &eastern_tm) == nullptr) {
    return 0;
  }
  return (eastern_tm.tm_year + 1900) * 10000 + (eastern_tm.tm_mon + 1) * 100 + eastern_tm.tm_mday;
}

// Alpha Vantage returns quote fields as JSON strings (e.g. "05. price":
// "172.4500"), not numbers -- strtod, not cJSON_IsNumber/valuedouble.
bool read_string_number(const cJSON* object, const char* key, double* out) {
  if (object == nullptr || out == nullptr) {
    return false;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr || item->valuestring[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const double value = std::strtod(item->valuestring, &end);
  if (end == item->valuestring) {
    return false;
  }
  *out = value;
  return true;
}

}  // namespace

esp_err_t StockClient::start() {
  if (task_handle_ != nullptr) {
    return ESP_OK;
  }
  const BaseType_t result =
      xTaskCreate(&StockClient::task_entry, "stockquote", 6144, this, 3, &task_handle_);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}

void StockClient::configure(const std::array<std::string, kStockSymbolCount>& symbols,
                            const std::string& api_key, uint32_t daily_limit) {
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    symbols_ = symbols;
    api_key_ = api_key;
    daily_limit_ = daily_limit;
  }
  reconfigure_requested_.store(true);
  if (task_handle_ != nullptr) {
    xTaskNotifyGive(task_handle_);
  }
}

StockSnapshot StockClient::snapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

void StockClient::task_entry(void* context) {
  static_cast<StockClient*>(context)->task_loop();
}

bool StockClient::consume_budget_slot_if_available_locked() {
  if (!budget_state_loaded_) {
    if (config_store_ != nullptr) {
      const std::string count_str = config_store_->load_plugin_string(kPluginNs, "req_count");
      const std::string day_str = config_store_->load_plugin_string(kPluginNs, "req_day");
      if (!count_str.empty()) {
        req_count_today_ = static_cast<uint32_t>(std::strtoul(count_str.c_str(), nullptr, 10));
      }
      if (!day_str.empty()) {
        req_day_key_ = std::atoi(day_str.c_str());
      }
    }
    budget_state_loaded_ = true;
  }

  const int today_key = eastern_date_key(std::time(nullptr));
  if (today_key != req_day_key_) {
    req_count_today_ = 0;
    req_day_key_ = today_key;
    if (config_store_ != nullptr) {
      config_store_->save_plugin_string(kPluginNs, "req_count", "0");
      config_store_->save_plugin_string(kPluginNs, "req_day", std::to_string(req_day_key_));
    }
  }

  if (daily_limit_ == 0) {
    return true;  // unlimited -- counter tracked above for day-rollover only, never gates
  }
  return req_count_today_ < daily_limit_;
}

void StockClient::record_budget_consumed_locked() {
  if (daily_limit_ == 0) {
    return;
  }
  ++req_count_today_;
  if (config_store_ != nullptr) {
    config_store_->save_plugin_string(kPluginNs, "req_count", std::to_string(req_count_today_));
  }
}

void StockClient::task_loop() {
  bool has_fetched_once = false;
  std::time_t last_fetch_slot_ts = 0;
  while (true) {
    // exchange(), not store(false) -- need to know whether *this* iteration
    // is the direct result of a configure() call (new symbols/API key), so a
    // save always fetches immediately rather than silently waiting for the
    // next schedule slot.
    const bool just_reconfigured = reconfigure_requested_.exchange(false);

    std::array<std::string, kStockSymbolCount> symbols;
    std::string api_key;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      symbols = symbols_;
      api_key = api_key_;
    }
    bool any_symbol = false;
    for (const auto& symbol : symbols) {
      if (!symbol.empty()) {
        any_symbol = true;
        break;
      }
    }
    const bool configured = !api_key.empty() && any_symbol;

    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      snapshot_.configured = configured;
    }

    const bool wifi_ready = wifi_manager_ == nullptr || wifi_manager_->is_station_connected();

    // The very first fetch after boot, and every explicit configure() (i.e.
    // every "Save Stocks Settings" in the portal), happens immediately
    // regardless of clock/schedule -- otherwise a symbol/API-key change
    // would silently sit unfetched until the next weekday slot. Passive,
    // non-reconfigure fetches follow the fixed schedule (kScheduleSlots) so
    // a slot only fires once and a reconfigure never "steals" a slot it
    // didn't earn (last_fetch_slot_ts is only advanced when a real slot was
    // actually due).
    const std::time_t now = std::time(nullptr);
    const std::time_t due_slot_ts = latest_due_slot(now);
    const bool schedule_due = due_slot_ts != 0 && due_slot_ts > last_fetch_slot_ts;
    const bool due = !has_fetched_once || just_reconfigured || schedule_due;

    if (configured && due && wifi_ready) {
      has_fetched_once = true;
      if (due_slot_ts > last_fetch_slot_ts) {
        last_fetch_slot_ts = due_slot_ts;
      }
      fetch_once();
    }

    if (!configured) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    if (reconfigure_requested_.load()) {
      continue;
    }

    if (!wifi_ready) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }

    // Schedule slots are hours apart -- no need for weather's ~1s recheck
    // cadence once Wi-Fi is up and no slot is due; a slower wake keeps this
    // task mostly idle. 30s keeps slot-boundary latency low without busy-
    // polling.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
  }
}

bool StockClient::fetch_once() {
  std::array<std::string, kStockSymbolCount> symbols;
  std::string api_key;
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    symbols = symbols_;
    api_key = api_key_;
  }
  if (api_key.empty()) {
    return false;
  }

  StockSnapshot updated = snapshot();
  updated.configured = true;
  updated.last_fetch_ms = now_ms();
  updated.budget_exhausted = false;
  bool any_ok = false;
  bool any_attempted = false;

  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    if (symbols[i].empty()) {
      updated.quotes[i] = StockQuote{};
      continue;
    }
    any_attempted = true;

    // `updated.quotes[i]` currently still holds whatever the previous cycle
    // last fetched (it was cloned from snapshot() above). Only overwrite it
    // below on a successful parse, or when the tracked symbol itself
    // changed -- a failed fetch of the *same* symbol should leave the last
    // known-good price/color on screen rather than blanking it.
    const bool same_symbol_as_before = updated.quotes[i].symbol == symbols[i];

    bool budget_available = true;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      budget_available = consume_budget_slot_if_available_locked();
    }
    if (!budget_available) {
      ESP_LOGW(kTag, "Daily API budget exhausted (%u/%u) -- skipping %s", req_count_today_,
               daily_limit_, symbols[i].c_str());
      updated.budget_exhausted = true;
      // updated.quotes[i] already holds the prior cycle's data (cloned from
      // snapshot() above) -- same preserve-old-data behavior the
      // same_symbol_as_before path below relies on, just reached without
      // ever attempting the request.
      continue;
    }

    const std::string url =
        std::string(kUrlPrefix) + symbols[i] + "&apikey=" + api_key;
    int status_code = 0;
    std::string response_body;
    const bool request_ok = perform_json_request(url, &status_code, &response_body);
    if (request_ok) {
      // Reached Alpha Vantage's server and got a response back -- billed
      // against the daily quota the instant it arrived, whether or not the
      // body turned out to contain real quote data (a rate-limit/bad-key/
      // bad-symbol response below is itself request N, not a free retry).
      std::lock_guard<std::mutex> lock(config_mutex_);
      record_budget_consumed_locked();
    }

    StockQuote quote;
    quote.symbol = symbols[i];
    bool parsed_ok = false;
    if (!request_ok) {
      ESP_LOGW(kTag, "Fetch failed for symbol %s", symbols[i].c_str());
    } else if (status_code < 200 || status_code >= 300) {
      ESP_LOGW(kTag, "Fetch rejected for symbol %s: status=%d", symbols[i].c_str(), status_code);
    } else if (parse_quote_response(response_body, &quote)) {
      any_ok = true;
      parsed_ok = true;
    } else {
      // Alpha Vantage reports rate-limit/bad-key/bad-symbol conditions as a
      // 200 with an "Information"/"Note"/"Error Message" field instead of a
      // real "Global Quote" object, so the body itself is the only way to
      // tell those apart -- capped to keep one bad response from flooding
      // the log. Alpha Vantage's rate-limit message echoes the API key back
      // verbatim (confirmed on-device), so mask it the same way the request
      // URL log above already does before printing.
      std::string masked_body = response_body;
      if (!api_key.empty()) {
        size_t pos = 0;
        while ((pos = masked_body.find(api_key, pos)) != std::string::npos) {
          masked_body.replace(pos, api_key.size(), "***");
          pos += 3;
        }
      }
      constexpr size_t kLogBodyPreviewChars = 300;
      ESP_LOGW(kTag, "Symbol %s: response parsed but no quote fields found -- "
                     "rate limit likely hit (25 req/day free tier), bad API key, or "
                     "invalid symbol. Response body: %.*s",
               symbols[i].c_str(), static_cast<int>(std::min(masked_body.size(), kLogBodyPreviewChars)),
               masked_body.c_str());
    }

    if (parsed_ok) {
      updated.quotes[i] = quote;
    } else if (!same_symbol_as_before) {
      // Symbol was just changed (or this is the first fetch for it) --
      // there's no prior data worth keeping, so show "--" rather than a
      // stale quote for a different ticker.
      updated.quotes[i] = quote;
    }
    // else: leave updated.quotes[i] (the previous cycle's data) untouched.
  }

  updated.last_fetch_ok = any_ok;
  updated.last_error = any_ok ? "" : (any_attempted ? "No symbols returned data" : "");
  if (any_ok) {
    // Never regresses on a failed or budget-skipped cycle -- always
    // reflects how old the data actually on screen currently is (see
    // StockSnapshot::last_success_ms's doc comment).
    updated.last_success_ms = updated.last_fetch_ms;
  }

  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = updated;
  }
  return any_ok;
}

bool StockClient::parse_quote_response(const std::string& body, StockQuote* out) {
  cJSON* root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    ESP_LOGW(kTag, "Quote response was not valid JSON");
    return false;
  }

  const cJSON* quote_obj = cJSON_GetObjectItemCaseSensitive(root, "Global Quote");
  if (quote_obj == nullptr) {
    cJSON_Delete(root);
    return false;
  }

  const bool has_price = read_string_number(quote_obj, "05. price", &out->price);
  const bool has_change = read_string_number(quote_obj, "09. change", &out->change);
  double change_percent_raw = 0.0;
  bool has_change_percent = false;
  const cJSON* pct_item = cJSON_GetObjectItemCaseSensitive(quote_obj, "10. change percent");
  if (cJSON_IsString(pct_item) && pct_item->valuestring != nullptr) {
    // Field is like "1.2345%" -- strtod stops at the '%', which is fine.
    char* end = nullptr;
    change_percent_raw = std::strtod(pct_item->valuestring, &end);
    has_change_percent = end != pct_item->valuestring;
  }
  out->change_percent = change_percent_raw;

  out->has_data = has_price && has_change;
  cJSON_Delete(root);
  return out->has_data || has_change_percent;
}

bool StockClient::perform_json_request(const std::string& url, int* status_code,
                                       std::string* response_body) {
  if (status_code == nullptr || response_body == nullptr) {
    return false;
  }
  response_body->clear();
  *status_code = 0;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 10000;
  config.cert_pem = reinterpret_cast<const char*>(gts_root_r4_cert_start);
  config.cert_len = static_cast<size_t>(gts_root_r4_cert_end - gts_root_r4_cert_start);
  config.method = HTTP_METHOD_GET;
  config.keep_alive_enable = false;
  config.buffer_size = 2048;
  config.buffer_size_tx = 1024;

  // Log with the apikey value redacted -- this log line ends up in the
  // serial console and gets pasted into bug reports.
  const size_t key_pos = url.find("apikey=");
  const std::string masked_url =
      key_pos == std::string::npos ? url : url.substr(0, key_pos) + "apikey=***";
  ESP_LOGI(kTag, "Fetching %s", masked_url.c_str());

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(kTag, "esp_http_client_init failed (out of memory or bad config)");
    return false;
  }
  esp_http_client_set_header(client, "Accept", "application/json");

  // See WeatherFlowClient::perform_json_request() for why a synchronous
  // acquire/release around just this one blocking call is correct here.
  if (network_arbiter_ != nullptr && !network_arbiter_->try_acquire_handshake_slot("stocks.http")) {
    esp_http_client_cleanup(client);
    return false;
  }
  const esp_err_t open_err = esp_http_client_open(client, 0);
  if (network_arbiter_ != nullptr) {
    network_arbiter_->release_handshake_slot("stocks.http");
  }
  if (open_err != ESP_OK) {
    ESP_LOGW(kTag, "HTTP open failed: %s", esp_err_to_name(open_err));
    esp_http_client_cleanup(client);
    return false;
  }

  const int fetch_result = esp_http_client_fetch_headers(client);
  if (fetch_result < 0) {
    ESP_LOGW(kTag, "HTTP fetch headers failed (result=%d)", fetch_result);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  *status_code = esp_http_client_get_status_code(client);
  const int64_t content_length = esp_http_client_get_content_length(client);
  if (content_length > static_cast<int64_t>(kMaxJsonResponseBytes)) {
    ESP_LOGW(kTag, "JSON response too large: %lld bytes", content_length);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return false;
  }

  char buffer[1024];
  while (true) {
    const int read = esp_http_client_read(client, buffer, sizeof(buffer));
    if (read < 0) {
      ESP_LOGW(kTag, "HTTP read failed (errno=%d)", errno);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    if (read == 0) {
      break;
    }
    if (response_body->size() + static_cast<size_t>(read) > kMaxJsonResponseBytes) {
      ESP_LOGW(kTag, "JSON response exceeded cap while reading");
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return false;
    }
    response_body->append(buffer, read);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return true;
}

}  // namespace infohub
