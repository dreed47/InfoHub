#include "infohub/plugins/weather/weather_flow_client.hpp"

#include <algorithm>
#include <cerrno>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "infohub/network_arbiter.hpp"
#include "infohub/wifi_manager.hpp"

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.weather";
constexpr size_t kMaxObservationJsonBytes = 16U * 1024U;
// better_forecast returns many more fields/entries (forecast.hourly[] alone
// is typically ~24-200+ hours, plus forecast.daily[] and a full current-
// conditions block) than the single-observation endpoint -- 16KB isn't
// enough (confirmed on-device: "JSON response exceeded cap while reading").
// Generous headroom since this is a once-an-hour fetch on a board with
// several MB of free PSRAM.
constexpr size_t kMaxForecastJsonBytes = 96U * 1024U;
constexpr char kUrlPrefix[] = "https://swd.weatherflow.com/swd/rest/observations/station/";
constexpr char kForecastUrlPrefix[] = "https://swd.weatherflow.com/swd/rest/better_forecast";
// Forecast doesn't need observation's 5-minute freshness -- refetch hourly,
// independent of the (usually much shorter) poll_interval_s used for
// observations. See task_loop()'s second `forecast_due` check.
constexpr uint32_t kForecastIntervalS = 3600;

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

}  // namespace

esp_err_t WeatherFlowClient::start() {
  if (task_handle_ != nullptr) {
    return ESP_OK;
  }
  const BaseType_t result =
      xTaskCreate(&WeatherFlowClient::task_entry, "weatherflow", 6144, this, 3, &task_handle_);
  return result == pdPASS ? ESP_OK : ESP_FAIL;
}

void WeatherFlowClient::configure(const std::string& station_id, const std::string& api_token,
                                  uint32_t poll_interval_s) {
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    station_id_ = station_id;
    api_token_ = api_token;
    poll_interval_s_ = poll_interval_s > 0 ? poll_interval_s : 300;
  }
  reconfigure_requested_.store(true);
  if (task_handle_ != nullptr) {
    xTaskNotifyGive(task_handle_);
  }
}

WeatherFlowSnapshot WeatherFlowClient::snapshot() const {
  std::lock_guard<std::mutex> lock(snapshot_mutex_);
  return snapshot_;
}

void WeatherFlowClient::task_entry(void* context) {
  static_cast<WeatherFlowClient*>(context)->task_loop();
}

void WeatherFlowClient::task_loop() {
  TickType_t last_fetch_tick = 0;
  TickType_t last_forecast_fetch_tick = 0;
  while (true) {
    reconfigure_requested_.store(false);

    std::string station_id;
    std::string api_token;
    uint32_t poll_interval_s;
    {
      std::lock_guard<std::mutex> lock(config_mutex_);
      station_id = station_id_;
      api_token = api_token_;
      poll_interval_s = poll_interval_s_;
    }
    const bool configured = !station_id.empty() && !api_token.empty();

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

    const TickType_t forecast_elapsed_ticks = now_tick - last_forecast_fetch_tick;
    const bool forecast_due = last_forecast_fetch_tick == 0 ||
                              forecast_elapsed_ticks >= pdMS_TO_TICKS(kForecastIntervalS * 1000ULL);
    if (configured && forecast_due && wifi_ready) {
      last_forecast_fetch_tick = now_tick;
      fetch_forecast_once();
    }

    if (!configured) {
      // Idle until reconfigured.
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    if (reconfigure_requested_.load()) {
      continue;
    }

    if (!wifi_ready) {
      // Wi-Fi not up yet (or dropped) — don't burn the poll interval waiting
      // on it, recheck every ~1s instead so the first successful fetch
      // happens promptly once the station connects.
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      continue;
    }

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}

bool WeatherFlowClient::fetch_once() {
  std::string station_id;
  std::string api_token;
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    station_id = station_id_;
    api_token = api_token_;
  }
  if (station_id.empty() || api_token.empty()) {
    return false;
  }

  const std::string url = std::string(kUrlPrefix) + station_id + "?api_key=" + api_token;

  int status_code = 0;
  std::string response_body;
  const bool request_ok =
      perform_json_request(url, &status_code, &response_body, kMaxObservationJsonBytes);

  WeatherFlowSnapshot updated = snapshot();
  updated.configured = true;
  updated.last_fetch_ms = now_ms();

  if (!request_ok) {
    updated.last_fetch_ok = false;
    updated.last_error = "HTTP request failed";
    ESP_LOGW(kTag, "Fetch failed for station %s", station_id.c_str());
  } else if (status_code < 200 || status_code >= 300) {
    updated.last_fetch_ok = false;
    updated.last_error = "HTTP status " + std::to_string(status_code);
    ESP_LOGW(kTag, "Fetch rejected for station %s: status=%d", station_id.c_str(), status_code);
  } else {
    parse_observation_response(response_body, &updated);
    if (updated.has_core_reading) {
      updated.last_fetch_ok = true;
      updated.last_error.clear();
    } else {
      updated.last_fetch_ok = false;
      updated.last_error = "Response missing expected observation fields";
      ESP_LOGW(kTag, "Station %s: response parsed but no core reading found — "
                     "field names may not match what this build expects",
               station_id.c_str());
    }
  }

  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = updated;
  }
  return updated.last_fetch_ok;
}

bool WeatherFlowClient::fetch_forecast_once() {
  std::string station_id;
  std::string api_token;
  {
    std::lock_guard<std::mutex> lock(config_mutex_);
    station_id = station_id_;
    api_token = api_token_;
  }
  if (station_id.empty() || api_token.empty()) {
    return false;
  }

  const std::string url = std::string(kForecastUrlPrefix) + "?station_id=" + station_id +
                          "&api_key=" + api_token +
                          "&units_temp=c&units_wind=mps&units_pressure=mb"
                          "&units_precip=mm&units_distance=km";

  int status_code = 0;
  std::string response_body;
  const bool request_ok =
      perform_json_request(url, &status_code, &response_body, kMaxForecastJsonBytes);

  WeatherFlowSnapshot updated = snapshot();

  if (!request_ok) {
    ESP_LOGW(kTag, "Forecast fetch failed for station %s", station_id.c_str());
    return false;
  }
  if (status_code < 200 || status_code >= 300) {
    ESP_LOGW(kTag, "Forecast fetch rejected for station %s: status=%d", station_id.c_str(),
             status_code);
    return false;
  }

  parse_forecast_response(response_body, &updated);
  if (!updated.has_forecast) {
    ESP_LOGW(kTag, "Station %s: forecast response parsed but no hourly entries found",
             station_id.c_str());
  }

  {
    std::lock_guard<std::mutex> lock(snapshot_mutex_);
    snapshot_ = updated;
  }
  return updated.has_forecast;
}

void WeatherFlowClient::parse_observation_response(const std::string& body,
                                                    WeatherFlowSnapshot* out) {
  cJSON* root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    ESP_LOGW(kTag, "Observation response was not valid JSON");
    return;
  }

  const cJSON* obs_array = cJSON_GetObjectItemCaseSensitive(root, "obs");
  const cJSON* obs = (cJSON_IsArray(obs_array) && cJSON_GetArraySize(obs_array) > 0)
                          ? cJSON_GetArrayItem(obs_array, 0)
                          : nullptr;
  if (obs == nullptr) {
    ESP_LOGW(kTag, "Observation response had no obs[0] entry");
    cJSON_Delete(root);
    return;
  }

  // Confirmed field names.
  const bool has_temp = read_number(obs, "air_temperature", &out->air_temperature_c);
  const bool has_rh = read_number(obs, "relative_humidity", &out->relative_humidity_pct);
  const bool has_pressure = read_number(obs, "barometric_pressure", &out->barometric_pressure_mb);
  out->has_core_reading = has_temp && has_rh && has_pressure;
  if (!out->has_core_reading) {
    ESP_LOGW(kTag, "Observation missing expected field(s): temp=%d rh=%d pressure=%d",
             has_temp, has_rh, has_pressure);
  }

  // Best-effort fields — names not confirmed against a live response; log
  // once per fetch if entirely absent so a field-name mismatch is visible
  // on-device rather than silently showing stale/zero values.
  const bool wind_avg_ok = read_number(obs, "wind_avg", &out->wind_avg_mps);
  const bool wind_gust_ok = read_number(obs, "wind_gust", &out->wind_gust_mps);
  const bool wind_dir_ok = read_number(obs, "wind_direction", &out->wind_direction_deg);
  out->has_wind = wind_avg_ok || wind_gust_ok || wind_dir_ok;

  out->has_uv = read_number(obs, "uv", &out->uv_index);
  out->has_precip = read_number(obs, "precip_accum_local_day", &out->precip_accumulated_mm);
  out->has_battery = read_number(obs, "battery", &out->battery_volts);

  cJSON_Delete(root);
}

void WeatherFlowClient::parse_forecast_response(const std::string& body,
                                                WeatherFlowSnapshot* out) {
  cJSON* root = cJSON_Parse(body.c_str());
  if (root == nullptr) {
    ESP_LOGW(kTag, "Forecast response was not valid JSON");
    return;
  }

  const cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current_conditions");
  if (current != nullptr) {
    std::string icon;
    std::string conditions;
    const bool has_icon = read_string(current, "icon", &icon);
    read_string(current, "conditions", &conditions);
    if (has_icon) {
      out->has_current_conditions = true;
      out->current_icon = icon;
      out->current_conditions_text = conditions;
    }
  }

  const cJSON* forecast_obj = cJSON_GetObjectItemCaseSensitive(root, "forecast");
  const cJSON* hourly_array =
      forecast_obj != nullptr ? cJSON_GetObjectItemCaseSensitive(forecast_obj, "hourly") : nullptr;
  if (!cJSON_IsArray(hourly_array)) {
    ESP_LOGW(kTag, "Forecast response had no forecast.hourly array");
    cJSON_Delete(root);
    return;
  }

  const int count = std::min(cJSON_GetArraySize(hourly_array),
                             static_cast<int>(kHourlyForecastCount));
  bool any_entry = false;
  for (int i = 0; i < count; ++i) {
    const cJSON* entry = cJSON_GetArrayItem(hourly_array, i);
    HourlyForecastEntry parsed;
    double time_raw = 0.0;
    const bool has_time = read_number(entry, "time", &time_raw);
    if (has_time) {
      parsed.time_unix = static_cast<uint64_t>(time_raw);
    }
    const bool has_temp = read_number(entry, "air_temperature", &parsed.air_temperature_c);
    read_string(entry, "conditions", &parsed.conditions);
    read_string(entry, "icon", &parsed.icon);
    read_number(entry, "precip_probability", &parsed.precip_probability);
    parsed.has_data = has_time && has_temp;
    if (parsed.has_data) {
      any_entry = true;
    }
    out->hourly_forecast[i] = parsed;
  }
  out->has_forecast = any_entry;

  cJSON_Delete(root);
}

bool WeatherFlowClient::perform_json_request(const std::string& url, int* status_code,
                                             std::string* response_body,
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

  // Log with the api_key value redacted — this log line ends up in the
  // serial console and gets pasted into bug reports.
  const size_t key_pos = url.find("api_key=");
  const std::string masked_url =
      key_pos == std::string::npos ? url : url.substr(0, key_pos) + "api_key=***";
  ESP_LOGI(kTag, "Fetching %s", masked_url.c_str());

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(kTag, "esp_http_client_init failed (out of memory or bad config)");
    return false;
  }
  esp_http_client_set_header(client, "Accept", "application/json");

  // esp_http_client_open() is the blocking TCP+TLS connect -- the actual
  // handshake NetworkArbiter exists to serialize across plugins. It fully
  // resolves (success or failure) by the time this call returns, so a
  // synchronous acquire/release around just this one call is correct (no
  // async event-handler bookkeeping needed, unlike an MQTT client's
  // esp_mqtt_client_start()).
  if (network_arbiter_ != nullptr && !network_arbiter_->try_acquire_handshake_slot("weather.http")) {
    esp_http_client_cleanup(client);
    return false;
  }
  const esp_err_t open_err = esp_http_client_open(client, 0);
  if (network_arbiter_ != nullptr) {
    network_arbiter_->release_handshake_slot("weather.http");
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
