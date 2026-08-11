#include "infohub/plugins/weather/weather_flow_client.hpp"

#include <cerrno>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "infohub/wifi_manager.hpp"

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.weather";
constexpr size_t kMaxJsonResponseBytes = 16U * 1024U;
constexpr char kUrlPrefix[] = "https://swd.weatherflow.com/swd/rest/observations/station/";

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
  const bool request_ok = perform_json_request(url, &status_code, &response_body);

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

bool WeatherFlowClient::perform_json_request(const std::string& url, int* status_code,
                                             std::string* response_body) {
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

  const esp_err_t open_err = esp_http_client_open(client, 0);
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
