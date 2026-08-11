#include "infohub/network_arbiter.hpp"

#include "esp_log.h"

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.netarb";
}

bool NetworkArbiter::try_acquire_handshake_slot(const char* owner_id) {
  uint8_t expected = active_handshakes_.load(std::memory_order_acquire);
  while (expected < kMaxConcurrentHandshakes) {
    if (active_handshakes_.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
      ESP_LOGD(kTag, "Handshake slot acquired by %s", owner_id ? owner_id : "?");
      return true;
    }
  }
  return false;
}

void NetworkArbiter::release_handshake_slot(const char* owner_id) {
  const uint8_t prev = active_handshakes_.fetch_sub(1, std::memory_order_acq_rel);
  if (prev == 0) {
    // Defensive: a release without a matching acquire would underflow back
    // to 255. Clamp back to 0 and log — this indicates a caller bug.
    active_handshakes_.store(0, std::memory_order_release);
    ESP_LOGW(kTag, "release_handshake_slot() called without a held slot (owner=%s)",
             owner_id ? owner_id : "?");
    return;
  }
  ESP_LOGD(kTag, "Handshake slot released by %s", owner_id ? owner_id : "?");
}

bool NetworkArbiter::handshake_in_flight() const {
  return active_handshakes_.load(std::memory_order_acquire) > 0;
}

}  // namespace infohub
