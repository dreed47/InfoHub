#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "printsphere/plugins/printer/bambu_cloud_client.hpp"
#include "printsphere/plugins/printer/p1s_camera_client.hpp"
#include "printsphere/plugin.hpp"
#include "printsphere/plugins/printer/printer_client.hpp"
#include "printsphere/plugins/printer/printer_state.hpp"

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

  // Computes the status ring's visual (color/animation) from the latest
  // snapshot and pushes it to Ui::apply_ring_visual(). Called from
  // update_ui() every tick, and also from the arc-color live-preview portal
  // handler for instant feedback when tuning colors in Web Config.
  void apply_ring_visual();

  bool wants_network() const override;
  bool wants_awake() const override;
  uint32_t desired_poll_interval_ms() const override;

  void build_tile(lv_obj_t*) override {}
  // Builds the printer-selector page0 content (title/card list/empty note)
  // into the container Ui hands out via printer_select_page_container().
  void build_screen(lv_obj_t* parent) override;
  void register_portal_routes(httpd_handle_t server) override;
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

  // Phase 7: thin forwarding accessors so SetupPortal's read-only status/
  // telemetry display code can go through PrinterPlugin instead of holding
  // its own BambuCloudClient&/PrinterClient& refs. Same data, just fetched
  // through the plugin that actually owns the clients.
  PrinterSnapshot local_snapshot() const { return printer_client_.snapshot(); }
  bool local_configured() const { return printer_client_.is_configured(); }
  BambuCloudSnapshot cloud_snapshot() const { return cloud_client_.snapshot(); }
  BambuCloudSnapshot cloud_refreshed_snapshot() { return cloud_client_.refreshed_snapshot(); }
  std::vector<CloudDeviceInfo> cloud_devices() const { return cloud_client_.get_cloud_devices(); }
  MqttTelemetry local_mqtt_telemetry() const { return printer_client_.mqtt_telemetry(); }
  MqttTelemetry cloud_mqtt_telemetry() const { return cloud_client_.mqtt_telemetry(); }
  // Source-mode-aware "is the printer side of provisioning satisfied" check,
  // moved here from SetupPortal::is_provisioning_complete() (which still
  // handles the Wi-Fi-level checks itself — those are genuinely core).
  bool is_provisioning_satisfied() const;

 private:
  // Phase 8d: portal routes moved here from SetupPortal (printer_plugin_portal.cpp).
  // Same handler shape as SetupPortal's — static + httpd_req_t::user_ctx — just
  // registered against this plugin's own httpd_uri_t table.
  static esp_err_t handle_arc_preview(httpd_req_t* request);
  static esp_err_t handle_arc_commit(httpd_req_t* request);
  static esp_err_t handle_arc_update(httpd_req_t* request, bool persist);
  static esp_err_t handle_source_mode_post(httpd_req_t* request);
  static esp_err_t handle_ams_display_post(httpd_req_t* request);
  static esp_err_t handle_cloud_connect(httpd_req_t* request);
  static esp_err_t handle_cloud_verify(httpd_req_t* request);
  static esp_err_t handle_local_connect(httpd_req_t* request);
  static esp_err_t handle_printers_get(httpd_req_t* request);
  static esp_err_t handle_printers_select(httpd_req_t* request);
  static esp_err_t handle_printers_save(httpd_req_t* request);
  static esp_err_t handle_printers_delete(httpd_req_t* request);
  static esp_err_t handle_printers_clear_local(httpd_req_t* request);
  // Phase 7: was left behind in SetupPortal during 8d despite already being
  // registered at the plugin-owned /api/plugins/printer/config path.
  static esp_err_t handle_plugin_printer_config_get(httpd_req_t* request);

  // Phase 6a: page0 (printer-selector) card management, moved here from Ui
  // (see CLAUDE.md's "Ui changes sketch" / the Phase 6a plan). Ui only owns
  // the bare page0 container + the generic scroll-fade machinery; everything
  // about what a "printer card" is lives here.
  struct PrinterCardInfo {
    uint8_t index = 0;
    std::string name;
    std::string model;
    std::string host;
    bool active = false;
    bool connected = false;
  };
  struct PrinterCardWidgets {
    lv_obj_t* card = nullptr;
    lv_obj_t* name_label = nullptr;
    lv_obj_t* model_label = nullptr;
    lv_obj_t* host_label = nullptr;
    lv_obj_t* status_dot = nullptr;
    uint8_t profile_index = 0;
  };
  void update_printer_cards(const std::vector<PrinterCardInfo>& cards);
  int consume_printer_switch_request();
  void rebuild_printer_cards_locked(const std::vector<PrinterCardInfo>& cards);
  void replay_card_animations_locked();
  static void replay_card_animations_trampoline(void* user_data);
  static void printer_card_click_cb(lv_event_t* event);

  lv_obj_t* title_ = nullptr;
  lv_obj_t* card_list_ = nullptr;
  lv_obj_t* empty_note_ = nullptr;
  std::vector<PrinterCardWidgets> page0_cards_;
  std::vector<PrinterCardInfo> last_printer_cards_;  // change-detection cache
  int pending_printer_switch_ = -1;

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
