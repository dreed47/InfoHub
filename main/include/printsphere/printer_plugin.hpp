#pragma once

#include <atomic>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "printsphere/bambu_cloud_client.hpp"
#include "printsphere/p1s_camera_client.hpp"
#include "printsphere/plugin.hpp"
#include "printsphere/printer_client.hpp"
#include "printsphere/printer_state.hpp"

namespace printsphere {

// Phase 8a (plugin-architecture extraction, see CLAUDE.md): PrinterPlugin
// now genuinely owns the printer-domain clients and all the hybrid
// arbitration / chamber-light / print-command / audio-edge-detection state
// that used to live on Application as private members, delegated to via
// friend access (Phase 5). The method bodies are a straight code-motion of
// Application::printer_plugin_tick()/init()/update_ui() — Phase 5 already
// consolidated all of this logic into those three methods, so this phase is
// "move the method," not "redesign the logic."
//
// register_portal_routes()/portal_settings_html()/build_tile()/
// build_screen() stay no-ops — SetupPortal's printer-domain routes
// (handle_cloud_connect, handle_local_connect, handle_printers_*, etc.) and
// Ui's printer-content code (AMS/arc/camera/preview pages) are still owned
// directly by SetupPortal/Ui. Moving those is Phase 8c/8d, deferred: both
// need UiShell's register_pages() to become a real multi-plugin API first,
// which isn't designed yet.
class PrinterPlugin : public Plugin {
 public:
  PrinterPlugin() = default;

  const char* id() const override { return "printer"; }
  const char* display_name() const override { return "Printer"; }

  esp_err_t init(PluginContext& ctx) override;
  void tick(uint64_t now_ms) override;
  void update_ui() override;

  bool wants_network() const override;
  bool wants_awake() const override;
  uint32_t desired_poll_interval_ms() const override;

  void build_tile(lv_obj_t*) override {}
  void build_screen(lv_obj_t*) override {}
  void register_portal_routes(httpd_handle_t) override {}
  std::string portal_settings_html() const override { return {}; }
  void load_config() override {}
  void save_config() override {}

  // Phase 8a bridge: SetupPortal's constructor still takes direct client
  // references (its printer-domain handlers — cloud/local connect, printer
  // profile CRUD — aren't plugin-routed yet, that's Phase 8d). Application's
  // constructor uses these purely to wire SetupPortal; nothing else should
  // reach through a Plugin* to grab a concrete client.
  BambuCloudClient& cloud_client() { return cloud_client_; }
  PrinterClient& printer_client() { return printer_client_; }
  P1sCameraClient& camera_client() { return camera_client_; }

 private:
  // Core service references, captured during init(). Plugin is default-
  // constructed as an Application member before init() runs, so these start
  // null rather than being reference members.
  ConfigStore* config_store_ = nullptr;
  WifiManager* wifi_manager_ = nullptr;
  Ui* ui_ = nullptr;
  SetupPortal* setup_portal_ = nullptr;
  PmuManager* pmu_manager_ = nullptr;
  AudioNotifier* audio_notifier_ = nullptr;

  BambuCloudClient cloud_client_{};
  PrinterClient printer_client_{};
  P1sCameraClient camera_client_{};

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

  // Split of the old inline apply_snapshot() call — tick() computes and
  // stores this, update_ui() pushes it to the widgets.
  PrinterSnapshot latest_snapshot_{};
  // Cached so desired_poll_interval_ms()/wants_awake() can answer without
  // recomputing; set at the end of tick() each iteration.
  uint32_t desired_poll_interval_ms_ = 1500;
  bool keep_screen_awake_ = false;
};

}  // namespace printsphere
