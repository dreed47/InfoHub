#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "esp_err.h"

namespace infohub {

// Lightweight buzzer-style audio notifier driving the on-board ES8311 codec via
// `bsp_audio_codec_speaker_init()`. Renders short square-wave tones / melodies
// from a dedicated FreeRTOS task; never blocks the caller.
//
// Triggering events is fire-and-forget: when the worker is busy or the
// notifier is muted/disabled the request is dropped silently.
class AudioNotifier {
 public:
  enum class Event : uint8_t {
    kPrintStarted,    // idle/preparing -> printing
    kPrintFinished,   // printing -> finished (Tada melody)
    kPrintError,      // has_error edge / new print_error_code
    kHmsAlert,        // new HMS warning code observed
    kPrintPaused,     // printing -> paused
    kFilamentChange,  // filament change / runout stage
    kReconnect,       // optional: cloud/local MQTT recovered after failures
    kClick,           // optional UI click feedback
  };

  // Number of built-in events above. Fixed — do not renumber, existing NVS
  // storage is keyed by this ordinal (see ConfigStore::load/save_audio_event_*).
  static constexpr uint8_t kEventCount = 8;

  // Total event-slot capacity, built-in + dynamically registered (see
  // register_event() below). Raising this only costs a few bytes per slot
  // (one atomic<bool> + one empty std::vector) until slots are actually used.
  static constexpr uint8_t kMaxEvents = 32;

  AudioNotifier();
  AudioNotifier(const AudioNotifier&) = delete;
  AudioNotifier& operator=(const AudioNotifier&) = delete;

  // Initialises the codec, allocates DMA buffers and spawns the worker task.
  // Safe to call multiple times — subsequent calls are no-ops.
  esp_err_t initialize();

  // Enqueue a notification. Drops silently if disabled, muted, or the queue
  // is full.
  void play(Event event);
  void play(uint8_t event_id);

  // Master enable/disable (persisted by caller via NVS). When disabled all
  // play() requests are dropped immediately.
  void set_enabled(bool enabled);
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

  // Volume in 0..100. Applied as linear amplitude scaling on top of the
  // codec hardware gain.
  void set_volume_percent(int volume);
  int volume_percent() const { return volume_pct_.load(std::memory_order_relaxed); }

  // Plays a short test tone — used by the web portal "Test" button.
  void play_test();

  // Plays a specific event, bypassing the master and per-event enable gates.
  // Used by the web portal per-event test buttons.
  void play_test_event(Event event);
  void play_test_event(uint8_t event_id);

  // Per-event enable/disable (finer gate below the master toggle).
  // Defaults: first 5 events (Print Started/Finished/Error/HMS/Paused) and
  // Click on; Filament Change and Reconnect default to off.
  void set_event_enabled(Event event, bool enabled);
  bool event_enabled(Event event) const;
  void set_event_enabled(uint8_t event_id, bool enabled);
  bool event_enabled(uint8_t event_id) const;

  // Replace the built-in melody for one event with a custom PCM blob
  // (16 kHz, 16-bit signed mono samples). An empty vector reverts to the
  // built-in tone.
  void set_event_pcm(Event event, std::vector<int16_t> samples);
  void clear_event_pcm(Event event);
  bool has_event_pcm(Event event) const;
  void set_event_pcm(uint8_t event_id, std::vector<int16_t> samples);
  void clear_event_pcm(uint8_t event_id);
  bool has_event_pcm(uint8_t event_id) const;

  // Claims a new event slot beyond the built-in kEventCount for a plugin's
  // own notification (e.g. a future weather plugin's "severe alert"). Returns
  // the assigned id (>= kEventCount) to drive via the uint8_t-taking overloads
  // above, or 0xFF if capacity (kMaxEvents) is exhausted. `stable_key` is
  // retained only for diagnostics/logging, not used as a lookup key here —
  // persisting the enabled-state/PCM for a registered event is the caller's
  // responsibility via ConfigStore's per-plugin accessors, the same way
  // Application loads/restores the built-in 8 at boot today. Not thread-safe
  // against concurrent registration — call only during single-threaded
  // plugin init, before any worker task references the assigned id.
  uint8_t register_event(const char* stable_key, bool default_enabled);

 private:
  std::atomic<bool> enabled_{true};
  std::atomic<int> volume_pct_{60};
  std::atomic<bool> initialized_{false};
  std::atomic<bool> event_enabled_[kMaxEvents];
  std::mutex pcm_mutex_;
  std::vector<int16_t> custom_pcm_[kMaxEvents];
  std::array<const char*, kMaxEvents> event_keys_{};
  std::atomic<uint8_t> next_dynamic_slot_{kEventCount};
};

}  // namespace infohub
