#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace infohub {

class WifiManager;
class NetworkArbiter;

constexpr uint8_t kHourlyForecastCount = 4;

// One hour's entry from WeatherFlow's better_forecast endpoint
// (forecast.hourly[]). `has_data` is set only if the entry parsed with at
// least a temperature reading. `icon` is WeatherFlow's small fixed enum
// (e.g. "clear-day", "rainy", "possibly-thunderstorm-night" -- see
// condition_color_for_icon() in tempest_plugin.cpp for the full set), not
// the free-text `conditions` string, which has too many phrasings ("Partly
// Cloudy", "Rain Likely", ...) to map to a color reliably.
struct HourlyForecastEntry {
  uint64_t time_unix = 0;
  double air_temperature_c = 0.0;
  std::string conditions;
  std::string icon;
  double precip_probability = 0.0;
  bool has_data = false;
};

// Latest fetched conditions for one WeatherFlow Tempest station. `configured`
// is true once a station_id + api_token have been set; `last_fetch_ok` and
// `last_error` report the outcome of the most recent poll. Optional fields
// (everything past the three confirmed-by-docs core readings) carry their own
// presence flag because the exact field-name set returned by WeatherFlow's
// named-field observation endpoint wasn't fully confirmed against a live
// response while building this — see this file's own .cpp parse comment.
struct WeatherFlowSnapshot {
  bool configured = false;
  bool last_fetch_ok = false;
  std::string last_error;
  uint64_t last_fetch_ms = 0;

  // Confirmed field names (WeatherFlow REST API docs).
  bool has_core_reading = false;
  double air_temperature_c = 0.0;
  double relative_humidity_pct = 0.0;
  double barometric_pressure_mb = 0.0;

  // Best-effort field names — presence flag set only if the key was actually
  // present and numeric in the response.
  bool has_wind = false;
  double wind_avg_mps = 0.0;
  double wind_gust_mps = 0.0;
  double wind_direction_deg = 0.0;
  bool has_uv = false;
  double uv_index = 0.0;
  bool has_precip = false;
  double precip_accumulated_mm = 0.0;
  bool has_battery = false;
  double battery_volts = 0.0;

  // Next kHourlyForecastCount hours from the better_forecast endpoint --
  // fetched on its own, slower cadence (see task_loop()), independent of
  // the observation poll interval above.
  bool has_forecast = false;
  std::array<HourlyForecastEntry, kHourlyForecastCount> hourly_forecast{};

  // better_forecast's top-level current_conditions object -- fetched on the
  // same request/cadence as hourly_forecast above (no extra API call).
  // Drives the main page's condition-color ring (see tempest_plugin.cpp).
  bool has_current_conditions = false;
  std::string current_icon;
  std::string current_conditions_text;
};

// Polls WeatherFlow's cloud REST API (https://swd.weatherflow.com/swd/rest/
// observations/station/{station_id}?api_key=...) for one station's latest
// observation, on its own FreeRTOS task — mirrors BambuCloudClient/
// PrinterClient's own-task pattern so the blocking HTTPS call never runs on
// the shared Application loop (TempestPlugin::tick() must stay non-blocking).
class WeatherFlowClient {
 public:
  WeatherFlowClient() = default;
  WeatherFlowClient(const WeatherFlowClient&) = delete;
  WeatherFlowClient& operator=(const WeatherFlowClient&) = delete;

  // Starts the polling task. Safe to call multiple times — subsequent calls
  // are no-ops (same contract as BambuCloudClient::start()).
  esp_err_t start();

  // Must be called before start() (or at least before Wi-Fi connects) — the
  // task checks this every ~1s and won't attempt a fetch (or start counting
  // down the poll interval) until the station is actually connected. Without
  // this, the very first fetch fires at boot before Wi-Fi has associated,
  // fails outright (DNS has no route yet), and then waits a full
  // poll_interval_s before trying again.
  void set_wifi_manager(const WifiManager* wifi_manager) { wifi_manager_ = wifi_manager; }
  // Shared handshake-serialization slot -- see NetworkArbiter. May be null
  // (falls back to unconditional connect, same as before this existed).
  void set_network_arbiter(NetworkArbiter* arbiter) { network_arbiter_ = arbiter; }

  // Sets station_id/api_token/poll interval and wakes the task for an
  // immediate fetch. Passing an empty station_id or api_token marks the
  // client unconfigured (task idles, snapshot().configured stays false).
  void configure(const std::string& station_id, const std::string& api_token,
                 uint32_t poll_interval_s);

  WeatherFlowSnapshot snapshot() const;

 private:
  static void task_entry(void* context);
  void task_loop();
  bool fetch_once();
  bool fetch_forecast_once();
  bool perform_json_request(const std::string& url, int* status_code, std::string* response_body,
                            size_t max_response_bytes);
  void parse_observation_response(const std::string& body, WeatherFlowSnapshot* out);
  void parse_forecast_response(const std::string& body, WeatherFlowSnapshot* out);

  TaskHandle_t task_handle_ = nullptr;
  const WifiManager* wifi_manager_ = nullptr;
  NetworkArbiter* network_arbiter_ = nullptr;

  mutable std::mutex config_mutex_;
  std::string station_id_;
  std::string api_token_;
  uint32_t poll_interval_s_ = 300;
  std::atomic<bool> reconfigure_requested_{false};

  mutable std::mutex snapshot_mutex_;
  WeatherFlowSnapshot snapshot_{};
};

}  // namespace infohub
