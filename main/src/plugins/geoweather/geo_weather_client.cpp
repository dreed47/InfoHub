#include "infohub/plugins/geoweather/geo_weather_client.hpp"

#include <cerrno>
#include <cstdio>
#include <ctime>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "infohub/network_arbiter.hpp"
#include "infohub/wifi_manager.hpp"

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.geoweather";
constexpr size_t kMaxGeocodeJsonBytes = 4U * 1024U;
constexpr size_t kMaxForecastJsonBytes = 8U * 1024U;
constexpr char kGeocodeUrlPrefix[] = "https://geocoding-api.open-meteo.com/v1/search?name=";
constexpr char kGeocodeUrlSuffix[] = "&count=1&language=en&format=json";
constexpr char kForecastUrlPrefix[] = "https://api.open-meteo.com/v1/forecast?";
// Fallback retry cadence for a persistently bad/misspelled location, once the
// fast-retry budget (below) is exhausted -- avoids hammering Open-Meteo's
// free geocoding endpoint indefinitely for a location that will never
// resolve.
constexpr uint32_t kGeoSlowRetryIntervalS = 1800;
// Absorbs transient just-after-Wi-Fi-associates DNS hiccups without waiting
// a full slow-retry interval -- reset any time the location text changes.
constexpr uint8_t kGeocodeFastRetryBudget = 5;

uint64_t now_ms() {
  return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

bool read_number(const cJSON* object, const char* key, double* out) {
  if (object == nullptr || out == nullptr) {
    return false;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsNumber(item)) {
    return false;
  }
  *out = item->valuedouble;
  return true;
}

bool read_string(const cJSON* object, const char* key, std::string* out) {
  if (object == nullptr || out == nullptr) {
    return false;
  }
  const cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  *out = item->valuestring;
  return true;
}

// No existing helper in this codebase does percent-encoding -- needed here
// for the geocoding endpoint's `name=` query param (a free-text location
// like "Austin, TX" contains a comma and a space).
std::string url_encode(const std::string& value) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(value.size() * 3);
  for (unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += kHex[(c >> 4) & 0x0F];
      encoded += kHex[c & 0x0F];
    }
  }
  return encoded;
}
}  // namespace

esp_err_t GeoWeatherClient::start() {
  if (task_handle_ != nullptr) {
    return ESP_OK;
  }
  const BaseType_t result =
      xTaskCreate(&GeoWeatherClient::task_entry, "geoweather", 6144, this, 3, &task_handle_);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}

void GeoWeatherClient::configure(const std::string& location_text, uint32_t poll_interval_s) {
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    location_text_ = location_text;
    poll_interval_s_ = poll_interval_s > 0 ? poll_interval_s : 600;
  }
  reconfigure_requested_.store(true);
  if (task_handle_ != nullptr) {
    xTaskNotifyGive(task_handle_);
  }
}

GeoWeatherSnapshot GeoWeatherClient::snapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

void GeoWeatherClient::task_entry(void* context) {
  static_cast<GeoWeatherClient*>(context)->task_loop();
}

void GeoWeatherClient::task_loop() {
  // Task-local geocode/retry state, same style as StockClient::task_loop()'s
  // own locals -- not member state, since it's purely about this task's own
  // retry bookkeeping and no other code needs to observe it.
  std::string last_geocode_attempt_text;
  bool resolved_for_current_text = false;
  uint8_t geocode_fast_retries_remaining = 0;
  TickType_t last_geocode_attempt_tick = 0;
  TickType_t last_forecast_fetch_tick = 0;

  while (true) {
    reconfigure_requested_.store(false);

    std::string location_text;
    uint32_t poll_interval_s;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      location_text = location_text_;
      poll_interval_s = poll_interval_s_;
    }
    const bool configured = !location_text.empty();

    if (location_text != last_geocode_attempt_text) {
      // Location text changed (including the very first configure()) --
      // whatever resolution/retry state existed belonged to a different
      // location, forget it.
      resolved_for_current_text = false;
      geocode_fast_retries_remaining = kGeocodeFastRetryBudget;
      last_geocode_attempt_tick = 0;
      last_forecast_fetch_tick = 0;
    }

    {
      std::lock_guard<std::mutex> lock(snapshot_mutex_);
      snapshot_.configured = configured;
      if (!configured) {
        snapshot_.location_resolved = false;
      }
    }

    if (!configured) {
      last_geocode_attempt_text.clear();
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    const bool wifi_ready = wifi_manager_ == nullptr || wifi_manager_->is_station_connected();
    if (!wifi_ready) {
      // Recheck every ~1s instead of burning a retry/poll slot waiting on
      // Wi-Fi, same pattern as WeatherFlowClient/StockClient.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }

    if (!resolved_for_current_text) {
      const TickType_t now_tick = xTaskGetTickCount();
      const bool fast_budget_left = geocode_fast_retries_remaining > 0;
      const TickType_t elapsed_ticks = now_tick - last_geocode_attempt_tick;
      const bool slow_retry_due =
          elapsed_ticks >= pdMS_TO_TICKS(kGeoSlowRetryIntervalS * 1000ULL);
      const bool should_attempt =
          last_geocode_attempt_tick == 0 || fast_budget_left || slow_retry_due;

      if (should_attempt) {
        last_geocode_attempt_tick = now_tick;
        last_geocode_attempt_text = location_text;
        if (geocode_fast_retries_remaining > 0) {
          --geocode_fast_retries_remaining;
        }
        if (geocode_once(location_text)) {
          resolved_for_current_text = true;
          // Don't wait for the next poll tick -- fetch a forecast right away
          // now that we have coordinates.
          last_forecast_fetch_tick = now_tick;
          fetch_forecast_once();
        }
      }

      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }

    const TickType_t now_tick = xTaskGetTickCount();
    const TickType_t elapsed_ticks = now_tick - last_forecast_fetch_tick;
    const bool due =
        last_forecast_fetch_tick == 0 || elapsed_ticks >= pdMS_TO_TICKS(poll_interval_s * 1000ULL);
    if (due) {
      last_forecast_fetch_tick = now_tick;
      fetch_forecast_once();
    }

    if (reconfigure_requested_.load()) {
      continue;
    }

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}

bool GeoWeatherClient::geocode_once(const std::string& location_text) {
  const std::string url = std::string(kGeocodeUrlPrefix) + url_encode(location_text) + kGeocodeUrlSuffix;

  int status_code = 0;
  std::string response_body;
  const bool request_ok = perform_json_request(url, "geoweather.geocode", &status_code,
                                                &response_body, kMaxGeocodeJsonBytes);

  GeoWeatherSnapshot updated = snapshot();
  bool resolved = false;

  if (!request_ok) {
    updated.location_resolved = false;
    updated.last_error = "Geocoding request failed";
    ESP_LOGW(kTag, "Geocode failed for \"%s\"", location_text.c_str());
  } else if (status_code < 200 || status_code >= 300) {
    updated.location_resolved = false;
    updated.last_error = "Geocoding HTTP status " + std::to_string(status_code);
    ESP_LOGW(kTag, "Geocode rejected for \"%s\": status=%d", location_text.c_str(), status_code);
  } else {
    double lat = 0.0;
    double lon = 0.0;
    std::string display_name;
    if (parse_geocode_response(response_body, &lat, &lon, &display_name)) {
      updated.location_resolved = true;
      updated.latitude = lat;
      updated.longitude = lon;
      updated.resolved_display_name = display_name;
      updated.last_error.clear();
      resolved = true;
    } else {
      updated.location_resolved = false;
      updated.last_error = "Location not found";
      ESP_LOGW(kTag, "Geocode: no match for \"%s\"", location_text.c_str());
    }
  }

  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = updated;
  }
  return resolved;
}

bool GeoWeatherClient::parse_geocode_response(const std::string& body, double* lat, double* lon,
                                              std::string* display_name) {
  cJSON* root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    ESP_LOGW(kTag, "Geocode response was not valid JSON");
    return false;
  }

  // Open-Meteo omits the "results" key entirely when there's no match --
  // absent and present-but-empty are both "not found".
  const cJSON* results = cJSON_GetObjectItemCaseSensitive(root, "results");
  if (!cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
    cJSON_Delete(root);
    return false;
  }

  const cJSON* first = cJSON_GetArrayItem(results, 0);
  double parsed_lat = 0.0;
  double parsed_lon = 0.0;
  const bool has_lat = read_number(first, "latitude", &parsed_lat);
  const bool has_lon = read_number(first, "longitude", &parsed_lon);
  if (!has_lat || !has_lon) {
    cJSON_Delete(root);
    return false;
  }

  std::string name;
  std::string admin1;
  std::string country_code;
  read_string(first, "name", &name);
  read_string(first, "admin1", &admin1);
  read_string(first, "country_code", &country_code);

  std::string built = name;
  if (!admin1.empty()) {
    built += ", " + admin1;
  }
  if (!country_code.empty()) {
    built += ", " + country_code;
  }

  *lat = parsed_lat;
  *lon = parsed_lon;
  *display_name = built.empty() ? "Resolved" : built;

  cJSON_Delete(root);
  return true;
}

bool GeoWeatherClient::fetch_forecast_once() {
  double lat = 0.0;
  double lon = 0.0;
  {
    const GeoWeatherSnapshot current = snapshot();
    if (!current.location_resolved) {
      return false;
    }
    lat = current.latitude;
    lon = current.longitude;
  }

  char coord_buf[80];
  std::snprintf(coord_buf, sizeof(coord_buf), "latitude=%.6f&longitude=%.6f", lat, lon);
  const std::string url = std::string(kForecastUrlPrefix) + coord_buf +
                          "&current=temperature_2m,relative_humidity_2m,surface_pressure,weather_code"
                          "&hourly=temperature_2m,weather_code,precipitation_probability"
                          "&temperature_unit=celsius&timezone=UTC&timeformat=unixtime&forecast_days=2";

  int status_code = 0;
  std::string response_body;
  const bool request_ok = perform_json_request(url, "geoweather.http", &status_code,
                                                &response_body, kMaxForecastJsonBytes);

  GeoWeatherSnapshot updated = snapshot();
  updated.last_fetch_ms = now_ms();

  if (!request_ok) {
    updated.last_fetch_ok = false;
    updated.last_error = "Forecast request failed";
    ESP_LOGW(kTag, "Forecast fetch failed");
  } else if (status_code < 200 || status_code >= 300) {
    updated.last_fetch_ok = false;
    updated.last_error = "Forecast HTTP status " + std::to_string(status_code);
    ESP_LOGW(kTag, "Forecast fetch rejected: status=%d", status_code);
  } else {
    parse_forecast_response(response_body, &updated);
    if (updated.has_current) {
      updated.last_fetch_ok = true;
      updated.last_error.clear();
    } else {
      updated.last_fetch_ok = false;
      updated.last_error = "Forecast response missing current conditions";
      ESP_LOGW(kTag, "Forecast response parsed but no current conditions found");
    }
  }

  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = updated;
  }
  return updated.last_fetch_ok;
}

void GeoWeatherClient::parse_forecast_response(const std::string& body, GeoWeatherSnapshot* out) {
  cJSON* root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    ESP_LOGW(kTag, "Forecast response was not valid JSON");
    return;
  }

  const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
  if (current != nullptr) {
    double temp = 0.0;
    double humidity = 0.0;
    double pressure = 0.0;
    double code = 0.0;
    const bool has_temp = read_number(current, "temperature_2m", &temp);
    const bool has_humidity = read_number(current, "relative_humidity_2m", &humidity);
    const bool has_pressure = read_number(current, "surface_pressure", &pressure);
    const bool has_code = read_number(current, "weather_code", &code);
    if (has_temp && has_code) {
      out->has_current = true;
      out->current_temperature_c = temp;
      out->current_humidity_pct = has_humidity ? humidity : 0.0;
      out->current_pressure_hpa = has_pressure ? pressure : 0.0;
      out->current_weather_code = static_cast<int>(code);
    }
  }

  // Open-Meteo's hourly block is columnar -- parallel arrays indexed
  // together, NOT an array of objects like WeatherFlow's forecast.hourly[].
  const cJSON* hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
  const cJSON* time_array = hourly != nullptr ? cJSON_GetObjectItemCaseSensitive(hourly, "time") : nullptr;
  const cJSON* temp_array =
      hourly != nullptr ? cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m") : nullptr;
  const cJSON* code_array =
      hourly != nullptr ? cJSON_GetObjectItemCaseSensitive(hourly, "weather_code") : nullptr;
  const cJSON* precip_array =
      hourly != nullptr ? cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability")
                        : nullptr;
  if (!cJSON_IsArray(time_array) || !cJSON_IsArray(temp_array) || !cJSON_IsArray(code_array)) {
    ESP_LOGW(kTag, "Forecast response had no usable hourly arrays");
    cJSON_Delete(root);
    return;
  }

  const int array_len = cJSON_GetArraySize(time_array);
  const uint64_t now = static_cast<uint64_t>(std::time(nullptr));

  // Open-Meteo's hourly array starts at midnight of the request day, not
  // "next hour" like WeatherFlow's -- find the first entry at or after now.
  int start_index = -1;
  for (int i = 0; i < array_len; ++i) {
    const cJSON* time_item = cJSON_GetArrayItem(time_array, i);
    if (!cJSON_IsNumber(time_item)) {
      continue;
    }
    if (static_cast<uint64_t>(time_item->valuedouble) >= now) {
      start_index = i;
      break;
    }
  }
  if (start_index < 0) {
    ESP_LOGW(kTag, "Forecast hourly array had no future entries (array_len=%d)", array_len);
    cJSON_Delete(root);
    return;
  }

  const int code_array_len = cJSON_GetArraySize(code_array);
  const int precip_array_len = precip_array != nullptr ? cJSON_GetArraySize(precip_array) : 0;

  bool any_entry = false;
  for (uint8_t i = 0; i < kGeoHourlyForecastCount; ++i) {
    const int idx = start_index + i;
    GeoHourlyForecastEntry parsed;
    if (idx < array_len) {
      const cJSON* time_item = cJSON_GetArrayItem(time_array, idx);
      const cJSON* temp_item = cJSON_GetArrayItem(temp_array, idx);
      const cJSON* code_item = idx < code_array_len ? cJSON_GetArrayItem(code_array, idx) : nullptr;
      const cJSON* precip_item =
          idx < precip_array_len ? cJSON_GetArrayItem(precip_array, idx) : nullptr;

      const bool has_time = cJSON_IsNumber(time_item);
      const bool has_temp = cJSON_IsNumber(temp_item);
      if (has_time) {
        parsed.time_unix = static_cast<uint64_t>(time_item->valuedouble);
      }
      if (has_temp) {
        parsed.temperature_c = temp_item->valuedouble;
      }
      if (cJSON_IsNumber(code_item)) {
        parsed.weather_code = static_cast<int>(code_item->valuedouble);
      }
      if (cJSON_IsNumber(precip_item)) {
        parsed.precip_probability = precip_item->valuedouble;
      }
      parsed.has_data = has_time && has_temp;
      if (parsed.has_data) {
        any_entry = true;
      }
    }
    out->hourly_forecast[i] = parsed;
  }
  out->has_forecast = any_entry;

  cJSON_Delete(root);
}

bool GeoWeatherClient::perform_json_request(const std::string& url, const char* arbiter_owner_id,
                                            int* status_code, std::string* response_body,
                                            size_t max_response_bytes) {
  if (status_code == nullptr || response_body == nullptr) {
    return false;
  }
  response_body->clear();
  *status_code = 0;

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.method = HTTP_METHOD_GET;
  config.keep_alive_enable = false;
  config.buffer_size = 2048;
  config.buffer_size_tx = 1024;

  // No secret in these URLs (Open-Meteo needs no API key) -- safe to log in
  // full, unlike WeatherFlowClient/StockClient's masked-key logging.
  ESP_LOGI(kTag, "Fetching %s", url.c_str());

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(kTag, "esp_http_client_init failed (out of memory or bad config)");
    return false;
  }
  esp_http_client_set_header(client, "Accept", "application/json");

  // See WeatherFlowClient::perform_json_request() for why a synchronous
  // acquire/release around just this one blocking call is correct here.
  if (network_arbiter_ != nullptr &&
      !network_arbiter_->try_acquire_handshake_slot(arbiter_owner_id)) {
    esp_http_client_cleanup(client);
    return false;
  }
  const esp_err_t open_err = esp_http_client_open(client, 0);
  if (network_arbiter_ != nullptr) {
    network_arbiter_->release_handshake_slot(arbiter_owner_id);
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
  if (content_length > static_cast<int64_t>(max_response_bytes)) {
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
    if (response_body->size() + static_cast<size_t>(read) > max_response_bytes) {
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
