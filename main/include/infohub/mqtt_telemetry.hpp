#pragma once

#include <cstdint>

namespace infohub {

// Lightweight reconnect-storm telemetry exported by the MQTT clients
// (PrinterClient / BambuCloudClient) for the setup-portal status page.
// Populated from the MQTT event handler and read from arbitrary tasks; all
// fields are snapshots of `std::atomic<>` counters so values may be slightly
// inconsistent with each other but never torn.
struct MqttTelemetry {
  uint32_t consecutive_failures = 0;  // resets on every MQTT_EVENT_CONNECTED
  uint32_t total_failures = 0;        // since boot
  uint32_t total_successes = 0;       // since boot
  uint64_t last_attempt_ms = 0;       // BEFORE_CONNECT seen
  uint64_t last_success_ms = 0;       // CONNECTED fired
  uint64_t last_failure_ms = 0;       // ERROR / DISCONNECTED fired
  uint32_t current_backoff_ms = 0;    // 0 when no rebuild is pending
  bool connected = false;             // current MQTT_CONNECTED state
};

}  // namespace infohub
