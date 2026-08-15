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

constexpr uint8_t kGeoHourlyForecastCount = 4;

// One hour's entry from Open-Meteo's /v1/forecast `hourly` block. Unlike
// WeatherFlow's forecast.hourly[] (array of objects), Open-Meteo's hourly
// response is columnar (parallel arrays indexed together) -- see
// GeoWeatherClient::parse_forecast_response() for how this gets flattened
// into one entry per hour. `weather_code` is a WMO weather interpretation
// code (0-99), a completely different vocabulary from WeatherFlow's icon
// strings -- see condition_color_for_wmo_code()/wmo_code_text() in
// geoweather_plugin.cpp.
struct GeoHourlyForecastEntry {
  uint64_t time_unix = 0;
  double temperature_c = 0.0;
  int weather_code = -1;
  double precip_probability = 0.0;
  bool has_data = false;
};

// Latest fetched conditions for one geocoded location. `configured` is true
// once a non-empty location string has been saved (regardless of whether it
// has resolved yet); `location_resolved` is true once geocoding has
// succeeded at least once for the *current* location text -- a stale
// resolution from a previous location string is never shown as current.
// Coordinates are intentionally not persisted anywhere outside this
// in-memory snapshot; see geo_weather_client.cpp's task_loop() comment.
struct GeoWeatherSnapshot {
  bool configured = false;
  bool location_resolved = false;
  std::string resolved_display_name;
  double latitude = 0.0;
  double longitude = 0.0;

  bool last_fetch_ok = false;
  std::string last_error;
  uint64_t last_fetch_ms = 0;

  bool has_current = false;
  double current_temperature_c = 0.0;
  double current_humidity_pct = 0.0;
  double current_pressure_hpa = 0.0;
  int current_weather_code = -1;

  bool has_forecast = false;
  std::array<GeoHourlyForecastEntry, kGeoHourlyForecastCount> hourly_forecast{};
};

// Resolves a free-text location (e.g. "Austin, TX") to lat/lon via
// Open-Meteo's free geocoding API, then polls Open-Meteo's free forecast API
// for current conditions + an hourly forecast at that location -- no API key
// or account required for either endpoint. Own FreeRTOS task, same
// own-task-for-blocking-HTTPS pattern as WeatherFlowClient/StockClient (see
// weather_flow_client.hpp) so the blocking HTTPS calls never run on the
// shared Application loop.
class GeoWeatherClient {
 public:
  GeoWeatherClient() = default;
  GeoWeatherClient(const GeoWeatherClient&) = delete;
  GeoWeatherClient& operator=(const GeoWeatherClient&) = delete;

  // Starts the polling task. Safe to call multiple times — subsequent calls
  // are no-ops (same contract as WeatherFlowClient::start()).
  esp_err_t start();

  // Must be called before start() (or at least before Wi-Fi connects) — same
  // rationale as WeatherFlowClient::set_wifi_manager().
  void set_wifi_manager(const WifiManager* wifi_manager) { wifi_manager_ = wifi_manager; }
  // Shared handshake-serialization slot -- see NetworkArbiter. May be null
  // (falls back to unconditional connect, same as before this existed).
  void set_network_arbiter(NetworkArbiter* arbiter) { network_arbiter_ = arbiter; }

  // Sets the location text + poll interval and wakes the task. Passing an
  // empty location_text marks the client unconfigured (task idles,
  // snapshot().configured stays false). A location_text that differs from
  // whatever was last successfully geocoded triggers a fresh geocode on the
  // task's own next loop iteration, followed immediately by a forecast fetch
  // once that succeeds -- geocoding never happens on every poll, only when
  // the location text actually changes (see task_loop()).
  void configure(const std::string& location_text, uint32_t poll_interval_s);

  GeoWeatherSnapshot snapshot() const;

 private:
  static void task_entry(void* context);
  void task_loop();
  bool geocode_once(const std::string& location_text);
  bool fetch_forecast_once();
  bool perform_json_request(const std::string& url, const char* arbiter_owner_id,
                            int* status_code, std::string* response_body,
                            size_t max_response_bytes);
  bool parse_geocode_response(const std::string& body, double* lat, double* lon,
                              std::string* display_name);
  void parse_forecast_response(const std::string& body, GeoWeatherSnapshot* out);

  TaskHandle_t task_handle_ = nullptr;
  const WifiManager* wifi_manager_ = nullptr;
  NetworkArbiter* network_arbiter_ = nullptr;

  mutable std::mutex config_mutex_;
  std::string location_text_;
  uint32_t poll_interval_s_ = 600;
  std::atomic<bool> reconfigure_requested_{false};

  mutable std::mutex snapshot_mutex_;
  GeoWeatherSnapshot snapshot_{};
};

}  // namespace infohub
