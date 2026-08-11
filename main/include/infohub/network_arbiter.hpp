#pragma once

#include <atomic>
#include <cstdint>

namespace infohub {

// Coordinates TLS/MQTT/HTTPS handshake starts across independently-authored
// network clients (PrinterClient, BambuCloudClient, P1sCameraClient, and any
// future plugin's own client) so at most kMaxConcurrentHandshakes run at
// once. Concurrent mbedTLS handshakes are the actual heap-pressure risk on
// this target: printer_client.cpp documents a ~13 KB internal-RAM leak per
// handshake destroyed mid-flight, and every network client here logs heap
// stats around its own connect path. This exists to eventually replace
// ad-hoc, hardcoded two-party gating (e.g. Application's local-MQTT handoff
// window) with a primitive any number of callers can use without knowing
// about each other — see NOT YET WIRED IN below.
//
// NOT YET WIRED IN: nothing calls this class yet. Wiring it into the actual
// connect call sites in PrinterClient/BambuCloudClient/P1sCameraClient, and
// removing Application's existing hybrid-mode gating in favor of it, is a
// separate follow-up that touches hot reconnect paths and needs on-device
// stress testing (Wi-Fi drop/reconnect, hybrid source-mode switching) before
// it can be trusted on a shipping product. See the "Phased extraction
// sequencing plan" in CLAUDE.md for the full rationale.
//
// Non-blocking by design: try_acquire_handshake_slot() never waits. Callers
// must be prepared to back off and retry on their own next tick()/loop
// iteration, matching the existing PrinterClient/BambuCloudClient worker-task
// pattern of polling state rather than blocking.
class NetworkArbiter {
 public:
  // Attempts to reserve a handshake slot for `owner_id` (a short, stable,
  // caller-owned string literal, e.g. "printer.mqtt" or "cloud.https", used
  // only for logging — not a lookup key, ownership isn't tracked per-id).
  // Returns true if the caller may proceed to open its TLS/MQTT/HTTPS
  // connection now; false if another handshake is already in flight. On
  // true, the caller MUST call release_handshake_slot() exactly once when
  // the connection attempt finishes (success OR failure) before it may
  // acquire again.
  bool try_acquire_handshake_slot(const char* owner_id);
  void release_handshake_slot(const char* owner_id);

  // Suggested backoff before the caller should retry acquiring, in ms.
  // Fixed for now (only one slot exists); becomes meaningful once multiple
  // concurrent slots / priority are introduced.
  uint32_t requested_yield_ms() const { return kDefaultYieldMs; }

  // Diagnostic use only (e.g. debug log / setup-portal telemetry) — not for
  // making acquire decisions, callers must go through
  // try_acquire_handshake_slot() for that.
  bool handshake_in_flight() const;

 private:
  static constexpr uint8_t kMaxConcurrentHandshakes = 1;
  static constexpr uint32_t kDefaultYieldMs = 250;

  std::atomic<uint8_t> active_handshakes_{0};
};

}  // namespace infohub
