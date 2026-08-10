#pragma once

#include <array>
#include <atomic>

#include "printsphere/audio_notifier.hpp"
#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/p1s_camera_client.hpp"
#include "printsphere/plugin.hpp"
#include "printsphere/pmu.hpp"
#include "printsphere/printer_client.hpp"
#include "printsphere/printer_plugin.hpp"
#include "printsphere/setup_portal.hpp"
#include "printsphere/serial_provisioner.hpp"
#include "printsphere/ui.hpp"
#include "printsphere/wifi_manager.hpp"
#include "freertos/FreeRTOS.h"

namespace printsphere {

class Application {
 public:
  Application();
  void run();

 private:
  friend class PrinterPlugin;

  // Phase 5 adapter methods: same logic that used to live inline in run()'s
  // loop, now called through the Plugin interface via printer_plugin_. See
  // "Phased extraction sequencing plan" in CLAUDE.md. Everything these touch
  // (cloud_client_, printer_client_, camera_client_, and all the hybrid/
  // audio/override state below) stays an Application member until Phase 8 —
  // only the call shape changed, not where the state lives.
  esp_err_t printer_plugin_init(PluginContext& ctx);
  void printer_plugin_tick(uint64_t now_ms);
  void printer_plugin_update_ui();
  bool printer_plugin_wants_network() const;
  bool printer_plugin_wants_awake() const;
  uint32_t printer_plugin_desired_poll_interval_ms() const;

  ConfigStore config_store_{};
  WifiManager wifi_manager_{};
  BambuCloudClient cloud_client_{};
  PrinterClient printer_client_{};
  P1sCameraClient camera_client_{};
  Ui ui_{};
  SetupPortal setup_portal_;
  SerialProvisioner serial_provisioner_;
  PmuManager pmu_manager_{};
  AudioNotifier audio_notifier_{};
  bool local_printer_enabled_ = false;
  bool last_local_print_live_ = false;
  bool last_cloud_print_live_ = false;
  TickType_t stop_banner_until_tick_ = 0;
  SourceMode source_mode_ = SourceMode::kHybrid;
  SourceMode last_source_mode_ = SourceMode::kHybrid;
  bool last_wifi_connected_ = false;
  bool last_camera_page_active_ = false;
  bool hybrid_local_gate_open_ = false;
  TickType_t hybrid_camera_cooldown_deadline_ = 0;
  std::atomic<TickType_t> local_mqtt_handoff_until_tick_{0};
  bool filament_wake_enabled_ = false;
  bool filament_anim_enabled_ = true;
  bool chamber_light_override_active_ = false;
  bool chamber_light_override_on_ = false;
  uint64_t chamber_light_override_until_ms_ = 0;
  // Optimistic UI override for the most recently issued pause/resume/stop
  // command. Active until the printer reports the corresponding lifecycle
  // transition or the timeout expires (whichever comes first).
  PrintCommand print_command_override_kind_ = PrintCommand::kNone;
  uint64_t print_command_override_until_ms_ = 0;
  // Edge-detection state for AudioNotifier — captured from the previous
  // PrinterSnapshot every loop iteration so we only beep on real transitions.
  PrintLifecycleState audio_last_lifecycle_ = PrintLifecycleState::kUnknown;
  bool audio_last_has_error_ = false;
  int audio_last_print_error_code_ = 0;
  size_t audio_last_hms_count_ = 0;
  bool audio_state_primed_ = false;

  // Phase 5: split of the old inline apply_snapshot() call — tick() computes
  // and stores this, update_ui() pushes it to the widgets.
  PrinterSnapshot latest_snapshot_{};
  // Cached so desired_poll_interval_ms()/wants_awake() can answer without
  // recomputing; set at the end of printer_plugin_tick() each iteration.
  uint32_t printer_desired_poll_interval_ms_ = 1500;
  bool printer_keep_screen_awake_ = false;

  PrinterPlugin printer_plugin_;
  std::array<Plugin*, kMaxPlugins> plugins_{};
};

}  // namespace printsphere
