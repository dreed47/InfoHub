#include "infohub/plugins/stocks/stock_client.hpp"

#include <cerrno>
#include <cstdlib>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
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
constexpr size_t kMaxJsonResponseBytes = 8U * 1024U;
constexpr char kUrlPrefix[] = "https://www.alphavantage.co/query?function=GLOBAL_QUOTE&symbol=";

uint64_t now_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
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
                            const std::string& api_key, uint32_t poll_interval_s) {
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    symbols_ = symbols;
    api_key_ = api_key;
    poll_interval_s_ = poll_interval_s > 0 ? poll_interval_s : 21600;
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

void StockClient::task_loop() {
  TickType_t last_fetch_tick = 0;
  while (true) {
    reconfigure_requested_.store(false);

    std::array<std::string, kStockSymbolCount> symbols;
    std::string api_key;
    uint32_t poll_interval_s;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      symbols = symbols_;
      api_key = api_key_;
      poll_interval_s = poll_interval_s_;
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

    const TickType_t now_tick = xTaskGetTickCount();
    const TickType_t elapsed_ticks = now_tick - last_fetch_tick;
    const bool due = last_fetch_tick == 0 ||
                     elapsed_ticks >= pdMS_TO_TICKS(poll_interval_s * 1000ULL);

    if (configured && due && wifi_ready) {
      last_fetch_tick = now_tick;
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

    // Poll cycles are hours apart (see header comment re: 25 req/day free
    // tier) -- no need for weather's ~1s recheck cadence once Wi-Fi is up
    // and a fetch isn't due; a slower wake keeps this task mostly idle.
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
  bool any_ok = false;
  bool any_attempted = false;

  for (uint8_t i = 0; i < kStockSymbolCount; ++i) {
    if (symbols[i].empty()) {
      updated.quotes[i] = StockQuote{};
      continue;
    }
    any_attempted = true;

    const std::string url =
        std::string(kUrlPrefix) + symbols[i] + "&apikey=" + api_key;
    int status_code = 0;
    std::string response_body;
    const bool request_ok = perform_json_request(url, &status_code, &response_body);

    StockQuote quote;
    quote.symbol = symbols[i];
    if (!request_ok) {
      ESP_LOGW(kTag, "Fetch failed for symbol %s", symbols[i].c_str());
    } else if (status_code < 200 || status_code >= 300) {
      ESP_LOGW(kTag, "Fetch rejected for symbol %s: status=%d", symbols[i].c_str(), status_code);
    } else if (parse_quote_response(response_body, &quote)) {
      any_ok = true;
    } else {
      ESP_LOGW(kTag, "Symbol %s: response parsed but no quote fields found -- "
                     "rate limit likely hit (25 req/day free tier) or symbol invalid",
               symbols[i].c_str());
    }
    updated.quotes[i] = quote;
  }

  updated.last_fetch_ok = any_ok;
  updated.last_error = any_ok ? "" : (any_attempted ? "No symbols returned data" : "");

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
