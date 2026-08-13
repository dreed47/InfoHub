#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "infohub/plugins/printer/bambu_cloud_client.hpp"
#include "infohub/plugins/printer/p1s_camera_client.hpp"
#include "infohub/plugin.hpp"
#include "infohub/plugins/printer/printer_client.hpp"
#include "infohub/plugins/printer/printer_state.hpp"

namespace infohub {

// Phase 8a (plugin-architecture extraction, see CLAUDE.md): PrinterPlugin
// now genuinely owns the printer-domain clients and all the hybrid
// arbitration / chamber-light / print-command / audio-edge-detection state
// that used to live on Application as private members, delegated to via
// friend access (Phase 5). The method bodies are a straight code-motion of
// Application::printer_plugin_tick()/init()/update_ui() — Phase 5 already
// consolidated all of this logic into those three methods, so this phase is
// "move the method," not "redesign the logic."
//
// Phase 4b/4c: PrinterPlugin now also owns every AMS/main-dashboard/preview/
// camera widget and its rendering logic (moved out of Ui — see
// main/src/plugins/printer/printer_plugin_ui.cpp). Ui knows nothing about
// PrinterSnapshot or any printer-specific widget; the connective tissue is a
// small set of generic hooks on Ui (register_page_visibility_callback(),
// register_page_settle_callback(), register_page_tap_callback(),
// set_plugin_page_available(), set_battery_overlay_text(),
// set_active_page_by_plugin(), request_chamber_light_toggle(),
// request_print_command()) that any content-owning plugin could use, not
// just this one.
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
  // 1 (printer-selector) + kMaxAmsUnits (AMS unit pages) + 3 (main/preview/
  // camera) = 8. Flows through Application's generic per-plugin
  // build_screen()/register_plugin_pages() loop exactly like weather/stocks
  // (Phase 4b/4c retired the Phase 4a transitional pool special-case).
  uint8_t page_count() const override {
    return static_cast<uint8_t>(1 + kMaxAmsUnits + 3);
  }
  // Builds all 8 local pages: 0=printer-selector, 1..kMaxAmsUnits=AMS units,
  // kMaxAmsUnits+1=main dashboard, +2=preview, +3=camera.
  void build_screen(lv_obj_t* parent, uint8_t page_index) override;
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
  // Source-mode-aware "is a printer actually connected" check — Plugin's
  // generic is_configured() hook, consulted by SetupPortal's
  // plugin-agnostic is_provisioning_complete() (which still handles the
  // Wi-Fi-level check itself — that's genuinely core).
  bool is_configured() const override;

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
  // Separate from the config GET/the other printer POST routes above (those
  // all trigger a reboot on change) — this one applies live, same shape as
  // WeatherPlugin's enabled toggle.
  static esp_err_t handle_enabled_post(httpd_req_t* request);

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

  // --- Phase 4b/4c: moved from Ui (main/src/plugins/printer/printer_plugin_ui.cpp) ---
  void build_main_dashboard_page(lv_obj_t* parent);
  void build_preview_page(lv_obj_t* parent);
  void build_camera_page(lv_obj_t* parent);
  void build_ams_page(int unit_idx);
  void render_ams_unit(int unit_idx, const PrinterSnapshot& snapshot, bool show_unit_label);
  void compute_ams_tray_errors(const PrinterSnapshot& snapshot);
  static void ams_error_pulse_timer_cb(lv_timer_t* timer);
  void apply_ams_error_pulse_locked();
  bool ensure_preview_image_loaded_locked(
      bool force_reload,
      std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw = nullptr,
      const lv_image_dsc_t* pre_decoded_dsc = nullptr);
  void release_preview_image_locked();
  void apply_snapshot_locked(const PrinterSnapshot& snapshot, bool force_ring_refresh,
                             std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw = nullptr,
                             const lv_image_dsc_t* pre_decoded_dsc = nullptr);
  void update_page_availability_locked(const PrinterSnapshot& snapshot);
  // Shared by update_page_availability_locked() (snapshot-driven) and
  // the enabled-toggle path (immediate, no snapshot available) — hides
  // ams_pages_/page2_/page3_, releases preview/camera image resources.
  void hide_printer_content_pages_locked();
  // Umbrella enable/disable — mirrors the old Ui::set_printer_plugin_enabled().
  // Disabling resets this plugin's own AMS/preview/camera availability state
  // and hides its widgets before telling Ui to hide the whole page range;
  // enabling just tells Ui to show the range (this plugin's own update_ui()
  // repopulates AMS/preview/camera availability from the next snapshot).
  void apply_enabled_state_to_ui(bool enabled_now, uint32_t lock_timeout_ms);
  void update_print_buttons_locked(const PrinterSnapshot& snapshot);
  static void pause_button_event_cb(lv_event_t* event);
  static void stop_button_event_cb(lv_event_t* event);
  void handle_pause_button_event(lv_event_t* event);
  void handle_stop_button_event(lv_event_t* event);
  static void remaining_row_event_cb(lv_event_t* event);
  void handle_remaining_row_click();
  static void logo_event_cb(lv_event_t* event);
  void handle_logo_event(lv_event_t* event);
  // Visibility-only refresh (no snapshot re-render) — registered with Ui via
  // register_page_visibility_callback() so chrome-triggered refreshes (page
  // scroll begin, availability toggles) still show/hide this plugin's
  // widgets instantly without waiting for the next update_ui() tick.
  void refresh_page_visibility();
  static void refresh_page_visibility_trampoline(void* user_data);
  // Full re-render on page settle (registered via register_page_settle_callback()) —
  // replays the deferred/last snapshot, mirroring the pre-4b/4c
  // Ui::set_active_page() tail.
  void on_page_settled();
  static void on_page_settled_trampoline(void* user_data);
  // Tap-to-refresh-camera (registered via register_page_tap_callback()).
  void on_page_tapped();
  static void on_page_tapped_trampoline(void* user_data);

  lv_obj_t* title_ = nullptr;
  lv_obj_t* card_list_ = nullptr;
  lv_obj_t* empty_note_ = nullptr;
  std::vector<PrinterCardWidgets> page0_cards_;
  std::vector<PrinterCardInfo> last_printer_cards_;  // change-detection cache
  int pending_printer_switch_ = -1;

  // --- AMS pages (local indices 1..kMaxAmsUnits) ---
  // One page per AMS unit. ams_pages_[0] additionally hosts the external-spool
  // widgets (which dynamically shrink the AMS visualization). Pages 1..3 do
  // not host the external spool.
  lv_obj_t* ams_pages_[kMaxAmsUnits] = {};
  lv_obj_t* ams_unit_label_[kMaxAmsUnits] = {};   // "AMS 1/2/3/4" header (only when count>1)
  lv_obj_t* ams_tray_row_[kMaxAmsUnits] = {};
  lv_obj_t* ams_tray_col_[kMaxAmsUnits][kMaxAmsTrays] = {};
  lv_obj_t* ams_tray_rect_[kMaxAmsUnits][kMaxAmsTrays] = {};
  lv_obj_t* ams_tray_fill_[kMaxAmsUnits][kMaxAmsTrays] = {};   // dark overlay for empty portion
  lv_obj_t* ams_tray_pct_[kMaxAmsUnits][kMaxAmsTrays] = {};    // percentage label inside rect
  lv_obj_t* ams_tray_type_[kMaxAmsUnits][kMaxAmsTrays] = {};
  lv_obj_t* ams_tray_arrow_[kMaxAmsUnits][kMaxAmsTrays] = {};  // triangle indicator below pill
  lv_obj_t* ams_shelf_[kMaxAmsUnits] = {};                     // gray shelf behind upper pills
  lv_obj_t* ams_base_[kMaxAmsUnits] = {};                      // dark base behind lower pills
  lv_obj_t* ams_humidity_drop_[kMaxAmsUnits] = {};
  lv_obj_t* ams_humidity_label_[kMaxAmsUnits] = {};
  lv_obj_t* ams_temp_label_[kMaxAmsUnits] = {};
  lv_obj_t* ams_note_[kMaxAmsUnits] = {};
  // Per-tray HMS/Error indicator state (true → pill gets diamond overlay,
  // arrow shows pulsating red triangle).
  bool ams_tray_error_[kMaxAmsUnits][kMaxAmsTrays] = {};
  // External spool widgets (only on ams_pages_[0]).
  lv_obj_t* ams_ext_col_ = nullptr;
  lv_obj_t* ams_ext_rect_ = nullptr;
  lv_obj_t* ams_ext_type_ = nullptr;
  lv_obj_t* ams_ext_mat_ = nullptr;
  lv_obj_t* ams_ext_arrow_ = nullptr;
  bool ams_ext_spool_shown_ = false;
  // Per-page availability (true if this AMS unit is present on the printer).
  bool ams_unit_present_[kMaxAmsUnits] = {};
  // Pulse animation state for error indicators (single shared timer).
  lv_timer_t* ams_error_pulse_timer_ = nullptr;
  uint32_t ams_error_pulse_phase_ = 0;

  // --- Main dashboard (local index kMaxAmsUnits+1) ---
  lv_obj_t* badge_slot_ = nullptr;
  lv_obj_t* logo_badge_ = nullptr;
  lv_obj_t* logo_image_ = nullptr;
  lv_obj_t* status_label_ = nullptr;
  lv_obj_t* detail_label_ = nullptr;
  lv_obj_t* layer_label_ = nullptr;
  lv_obj_t* layer_row_ = nullptr;
  lv_obj_t* filament_icon_label_ = nullptr;
  lv_obj_t* filament_value_label_ = nullptr;
  lv_obj_t* nozzle_prefix_label_ = nullptr;
  lv_obj_t* nozzle_value_label_ = nullptr;
  lv_obj_t* nozzle_aux_label_ = nullptr;
  lv_obj_t* bed_prefix_label_ = nullptr;
  lv_obj_t* bed_value_label_ = nullptr;
  lv_obj_t* bed_aux_label_ = nullptr;
  lv_obj_t* remaining_prefix_label_ = nullptr;
  lv_obj_t* remaining_label_ = nullptr;
  lv_obj_t* remaining_row_ = nullptr;

  // --- Preview page (local index kMaxAmsUnits+2) ---
  lv_obj_t* page2_image_ = nullptr;
  lv_obj_t* page2_note_ = nullptr;
  lv_obj_t* page2_subnote_ = nullptr;
  // Print-control buttons on the preview page. Visible while a job is in
  // Printing/Paused/Preparing state. The pause button toggles between
  // pause/resume based on lifecycle. The stop button requires LV_EVENT_LONG_PRESSED
  // (~1.5s hold) so a stray tap can't kill a print.
  lv_obj_t* page2_pause_button_ = nullptr;
  lv_obj_t* page2_pause_button_label_ = nullptr;
  lv_obj_t* page2_stop_button_ = nullptr;
  lv_obj_t* page2_stop_button_label_ = nullptr;

  // --- Camera page (local index kMaxAmsUnits+3) ---
  lv_obj_t* page3_image_ = nullptr;
  lv_obj_t* page3_note_ = nullptr;
  lv_obj_t* page3_subnote_ = nullptr;

  bool detail_visible_ = true;
  bool show_logo_ = false;
  bool preview_page_available_ = true;
  bool preview_image_visible_ = false;
  bool preview_text_image_mode_ = false;
  bool camera_page_available_ = true;
  bool camera_image_visible_ = false;
  bool camera_text_image_mode_ = false;
  bool nozzle_aux_visible_ = false;
  bool bed_aux_visible_ = false;
  // Toggled by tapping the remaining-time row on page1: when true the row
  // shows the predicted finish wall-clock time instead of the remaining
  // duration. The clock-icon prefix is hidden in ETA mode to make room.
  bool show_eta_ = false;
  bool logo_clickable_ = false;
  bool logo_recolor_enabled_ = false;
  uint32_t logo_recolor_hex_ = 0;
  uint32_t last_rendered_ams_signature_ = UINT32_MAX;
  std::string last_ui_status_;
  bool last_print_active_ = false;
  std::string last_diag_status_;
  std::string last_diag_detail_;
  std::string last_diag_stage_;
  lv_image_dsc_t preview_image_dsc_{};
  std::shared_ptr<std::vector<uint8_t>> last_preview_blob_{};
  std::shared_ptr<std::vector<uint8_t>> last_preview_raw_{};
  lv_image_dsc_t camera_image_dscs_[2]{};
  std::shared_ptr<std::vector<uint8_t>> camera_blobs_[2]{};
  uint8_t active_camera_slot_ = 0;
  bool camera_slot_initialized_ = false;
  uint16_t last_camera_width_ = 0;
  uint16_t last_camera_height_ = 0;
  bool deferred_snapshot_pending_ = false;
  PrinterSnapshot deferred_snapshot_{};
  PrinterSnapshot last_snapshot_{};
  mutable std::mutex camera_refresh_mutex_{};
  bool camera_refresh_requested_ = false;
  bool consume_camera_refresh_request();

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

}  // namespace infohub
