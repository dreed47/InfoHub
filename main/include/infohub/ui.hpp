#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lvgl.h"
#include "esp_err.h"
#include "infohub/config_store.hpp"
#include "infohub/plugins/printer/printer_state.hpp"
#include "infohub/ui_shell.hpp"

namespace infohub {

// Generic description of what the shared status ring should show — printer-
// agnostic on purpose. Ui::apply_ring_visual() just renders whatever it's
// given; the decision of *what* to show (derived from PrinterSnapshot /
// print lifecycle) lives in PrinterPlugin, not here. Any plugin whose page
// is active could drive the ring the same way, reusing Ui's existing
// lv_anim_t bookkeeping instead of reimplementing it.
enum class RingAnimKind : uint8_t {
  kNone,            // Static — no animation
  kFilamentLoad,    // Arc value sweeps 0->100, repeat
  kFilamentUnload,  // Arc value sweeps 100->0, repeat
  kPulseBoth,       // Both MAIN & INDICATOR color pulse
  kPulseIndicator,  // Only INDICATOR color pulses
};

struct RingVisual {
  uint32_t main_hex = 0;
  uint32_t indicator_hex = 0xFFFFFF;
  int value_override = -1;
  RingAnimKind anim_kind = RingAnimKind::kNone;
  uint32_t pulse_base_hex = 0;
  uint32_t pulse_period_ms = 0;
  bool animated() const { return anim_kind != RingAnimKind::kNone; }
};

class Ui {
 public:
  // Page layout (left → right):
  //   0:                            printer-selector
  //   1 .. kMaxAmsUnits:            AMS unit pages (only present units enabled)
  //   kPageIdxMain:                 main dashboard
  //   kPageIdxPreview:              print preview
  //   kPageIdxCamera:               camera feed
  static constexpr int kPageIdxPrinterSelect = 0;
  static constexpr int kPageIdxAmsFirst = 1;
  static constexpr int kPageIdxAmsLast = kPageIdxAmsFirst + kMaxAmsUnits - 1;
  static constexpr int kPageIdxMain = kPageIdxAmsLast + 1;
  static constexpr int kPageIdxPreview = kPageIdxMain + 1;
  static constexpr int kPageIdxCamera = kPageIdxMain + 2;
  static constexpr int kPageIdxPluginPage = kPageIdxCamera + 1;
  static constexpr int kPageIdxLast = kPageIdxPluginPage;

  void set_display_rotation(DisplayRotation rotation);
  void set_display_tilt_deci_deg(int deci_deg);
  esp_err_t initialize();
  void set_arc_color_scheme(const ArcColorScheme& colors);
  const ArcColorScheme& arc_color_scheme() const { return arc_colors_; }
  void apply_ring_visual(const RingVisual& ring, int progress_value, uint32_t text_hex);
  void apply_snapshot(const PrinterSnapshot& snapshot);
  // keep_awake: hard wake-lock (provisioning / camera page / page transition) —
  //             blocks both dimming and screen-off.
  // print_active: selects the "during print" timeouts instead of the idle
  //               timeouts; dimming/screen-off stay allowed per policy.
  void update_power_save(bool on_battery, bool keep_awake, bool print_active);
  void set_battery_display_policy(const BatteryDisplayPolicy& policy);
  bool is_low_power_mode_active() const;
  ScreenPowerMode screen_power_mode() const { return ui_shell_.screen_power_mode(); }
  bool is_config_page_active() const {
    return !page_scrolling_snapshot_.load(std::memory_order_relaxed) &&
           active_page_snapshot_.load(std::memory_order_relaxed) == kPageIdxPrinterSelect;
  }
  bool is_page2_active() const {
    return !page_scrolling_snapshot_.load(std::memory_order_relaxed) &&
           active_page_snapshot_.load(std::memory_order_relaxed) == kPageIdxPreview;
  }
  bool is_camera_page_active() const {
    return !page_scrolling_snapshot_.load(std::memory_order_relaxed) &&
           active_page_snapshot_.load(std::memory_order_relaxed) == kPageIdxCamera;
  }
  bool is_camera_page_visible() const {
    return active_page_snapshot_.load(std::memory_order_relaxed) == kPageIdxCamera;
  }
  bool is_page_transition_active() const {
    return page_scrolling_snapshot_.load(std::memory_order_relaxed);
  }
  void set_portal_access_state(bool lock_enabled, bool request_authorized, bool session_active,
                               bool pin_active, const std::string& pin_code,
                               uint32_t pin_remaining_s, uint32_t session_remaining_s);
  // Core Wi-Fi state, pushed every Application loop iteration — independent
  // of any plugin. compute_portal_texts_locked() uses this instead of a
  // plugin's own snapshot fields, so the portal hint/PIN overlay works
  // regardless of which plugins are enabled.
  void set_core_wifi_state(bool station_connected, bool setup_ap_active,
                           const std::string& station_ip);
  // Recomputes and re-renders the portal hint/PIN overlay. Called directly
  // by Application every loop iteration (core, not routed through any
  // plugin's update_ui()) so it keeps working even with every plugin
  // disabled.
  void update_portal_access_visuals();
  bool consume_camera_refresh_request();
  bool consume_chamber_light_toggle_request();
  bool has_chamber_light_toggle_request() const { return chamber_light_toggle_requested_.load(); }
  // Pause / resume / stop buttons on the preview page set this request.
  // Application::loop polls it every iteration and dispatches via the LAN /
  // Cloud client. Returns kNone when no command pending. Consuming clears
  // the request atomically.
  PrintCommand consume_print_command_request();
  bool has_print_command_request() const {
    return print_command_request_.load() != static_cast<uint8_t>(PrintCommand::kNone);
  }
  bool consume_portal_unlock_request();

  void request_wake_display();

  // The one reserved generic plugin-page slot (kPageIdxPluginPage) — see the
  // comment on that constant. A plugin calls build_screen() once with this
  // container during Application startup, then drives visibility in the
  // pager via set_plugin_page_enabled() from its own update_ui().
  lv_obj_t* plugin_page_container() const { return plugin_page_; }
  void set_plugin_page_enabled(bool enabled);

  // Page 0 (printer-selector) is a fixed pager slot (kPageIdxPrinterSelect,
  // always present — PrinterPlugin is not Kconfig-optional like weather is)
  // but its content is owned by PrinterPlugin, same "Ui hands out a bare
  // container" pattern as plugin_page_container() above.
  lv_obj_t* printer_select_page_container() const { return page0_; }
  // PrinterPlugin calls this once after building its page0 widgets, so Ui's
  // existing page0<->page1 scroll fade (apply_page0_parallax) has something
  // to animate without needing to know what a "printer card" is.
  void register_page0_fade_targets(lv_obj_t* title, lv_obj_t* card_list, lv_obj_t* empty_note);
  // Same static-fn/user_data idiom as every other LVGL callback in this file.
  // Invoked from set_active_page() when the pager returns to page0, so
  // PrinterPlugin can replay its card reveal animation.
  using Page0ReentryCallback = void (*)(void* user_data);
  void register_page0_reentry_callback(Page0ReentryCallback callback, void* user_data);

  // Umbrella on/off switch for all of PrinterPlugin's page groups (page0,
  // page1, AMS units, preview, camera) — see PrinterPlugin::init()/its portal
  // enabled toggle. Disabling forces every one of those pages hidden
  // immediately (they'd otherwise freeze at their last state, since
  // Application stops calling PrinterPlugin::update_ui() once disabled).
  // lock_timeout_ms defaults to the usual snappy 200ms for live portal-toggle
  // calls; PrinterPlugin::init() passes a longer timeout since it runs right
  // after build_dashboard()'s own heavy LVGL construction, which can still be
  // holding the lock at that exact moment (~300ms) — silently losing that
  // race at boot means the disabled state never reaches the pager at all.
  void set_printer_plugin_enabled(bool enabled, uint32_t lock_timeout_ms = 200);

 private:
  // Phase 6/9 (plugin-architecture extraction, see CLAUDE.md "Ui changes
  // sketch"): UiShell owns page-index bookkeeping (page_enabled()/
  // page_object() below just delegate to it — register_page_slots() still
  // orchestrates building the table since it needs addresses of Ui's own
  // content-owned page objects) and the dimming/brightness/power-save state
  // machine. Everything here still encodes exactly the same single printer
  // page set, same order, same behavior — a relocation, not a redesign.
  // active_page_/scrolling_ deliberately were NOT moved: they're read by
  // ~15 printer-content call sites (apply_snapshot_locked alone) that would
  // need touching for no functional benefit this pass.
  UiShell ui_shell_{};
  void register_page_slots();

  esp_err_t build_dashboard();
  void apply_ring_visual_locked(const RingVisual& ring, int progress_value, uint32_t text_hex);
  void apply_snapshot_locked(const PrinterSnapshot& snapshot, bool force_ring_refresh,
                             std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw = nullptr,
                             const lv_image_dsc_t* pre_decoded_dsc = nullptr);
  bool ensure_preview_image_loaded_locked(
      bool force_reload,
      std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw = nullptr,
      const lv_image_dsc_t* pre_decoded_dsc = nullptr);
  void release_preview_image_locked();
  void apply_page_visibility();
  void apply_logo_visibility();
  void update_page_availability_locked(const PrinterSnapshot& snapshot);
  // Shared by update_page_availability_locked() (snapshot-driven) and
  // set_printer_plugin_enabled(false) (immediate, no snapshot available) —
  // hides ams_pages_/page2_/page3_, releases preview/camera image resources,
  // reclamps active_page_, and republishes page state.
  void hide_printer_content_pages_locked();
  // Shows/hides no_plugins_overlay_ based on whether any pager page is
  // currently enabled. Called after any page-enabled state changes
  // (set_printer_plugin_enabled(), set_plugin_page_enabled()).
  void update_no_plugins_overlay_locked();
  // Delegate to ui_shell_ — kept as same-named/same-signature Ui methods so
  // the many printer-content call sites below don't need touching.
  void note_activity(bool wake_display);
  void set_pager_scroll_locked(bool locked);
  void set_active_page(int page);
  void publish_page_state_snapshot();
  int clamp_enabled_page(int page) const;
  int next_enabled_page(int page, int direction) const;
  int nearest_enabled_page_for_scroll() const;
  lv_obj_t* page_object(int page) const;
  void handle_pager_event(lv_event_t* event);
  void handle_screen_event(lv_event_t* event);
  void handle_logo_event(lv_event_t* event);
  void update_portal_access_visuals_locked();
  void compute_portal_texts_locked();
  void stop_ring_animations_locked();
  // Build a single AMS-unit page (widgets attached to ams_pages_[unit_idx]).
  // unit_idx 0 also receives the external-spool widgets.
  void build_ams_page(int unit_idx);
  // Apply AMS rendering for a single unit. Called once per visible unit.
  void render_ams_unit(int unit_idx, const PrinterSnapshot& snapshot,
                      bool show_unit_label);
  // Compute per-tray HMS error flags from snapshot.hms_codes.
  // Sets ams_tray_error_[unit][slot] for AMS-class HMS codes.
  void compute_ams_tray_errors(const PrinterSnapshot& snapshot);
  static void ams_error_pulse_timer_cb(lv_timer_t* timer);
  void apply_ams_error_pulse_locked();
  static void pulse_anim_exec_cb(void* var, int32_t scale);
  static void pager_event_cb(lv_event_t* event);
  static void screen_event_cb(lv_event_t* event);
  static void logo_event_cb(lv_event_t* event);
  static void pause_button_event_cb(lv_event_t* event);
  static void stop_button_event_cb(lv_event_t* event);
  void handle_pause_button_event(lv_event_t* event);
  void handle_stop_button_event(lv_event_t* event);
  void update_print_buttons_locked(const PrinterSnapshot& snapshot);
  static void remaining_row_event_cb(lv_event_t* event);
  void handle_remaining_row_click();

  bool initialized_ = false;
  lv_display_t* display_ = nullptr;
  lv_obj_t* screen_ = nullptr;
  lv_obj_t* fixed_overlay_ = nullptr;
  lv_obj_t* page0_ = nullptr;
  // Non-owning pointers registered by PrinterPlugin via
  // register_page0_fade_targets() — see that method's comment.
  lv_obj_t* page0_fade_title_ = nullptr;
  lv_obj_t* page0_fade_card_list_ = nullptr;
  lv_obj_t* page0_fade_empty_note_ = nullptr;
  Page0ReentryCallback page0_reentry_cb_ = nullptr;
  void* page0_reentry_user_data_ = nullptr;

  void apply_page0_parallax(bool force = false);

  // --- AMS pages (page indices 1..kMaxAmsUnits) ---
  // One page per AMS unit. ams_pages_[0] additionally hosts the external-spool
  // widgets (which dynamically shrink the AMS visualization). Pages 1..3 do not
  // host the external spool.
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

  lv_obj_t* page1_ = nullptr;
  lv_obj_t* page2_ = nullptr;
  lv_obj_t* page3_ = nullptr;
  lv_obj_t* plugin_page_ = nullptr;
  bool plugin_page_enabled_ = false;  // starts disabled — pager skips it until a plugin turns it on
  bool printer_plugin_enabled_ = true;  // registered against page0/page1's slots
  lv_obj_t* no_plugins_overlay_ = nullptr;
  lv_obj_t* no_plugins_overlay_label_ = nullptr;
  lv_obj_t* status_arc_ = nullptr;
  lv_obj_t* progress_label_ = nullptr;
  lv_obj_t* battery_icon_label_ = nullptr;
  lv_obj_t* battery_pct_label_ = nullptr;
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
  lv_obj_t* brightness_overlay_ = nullptr;
  lv_obj_t* page2_shell_ = nullptr;
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
  lv_obj_t* page3_image_ = nullptr;
  lv_obj_t* page3_note_ = nullptr;
  lv_obj_t* page3_subnote_ = nullptr;
  lv_obj_t* portal_hint_label_ = nullptr;
  lv_obj_t* portal_overlay_card_ = nullptr;
  lv_obj_t* portal_overlay_title_ = nullptr;
  lv_obj_t* portal_overlay_value_ = nullptr;
  lv_obj_t* portal_overlay_detail_ = nullptr;
  lv_timer_t* ring_anim_timer_ = nullptr;  // unused, ambient sweep timer removed
  bool gesture_active_ = false;
  bool overlay_visible_ = false;
  bool scrolling_ = false;
  bool deferred_snapshot_pending_ = false;
  bool detail_visible_ = true;
  bool show_logo_ = false;
  bool accent_initialized_ = false;
  bool preview_page_available_ = true;
  bool preview_image_visible_ = false;
  bool preview_text_image_mode_ = false;
  bool camera_page_available_ = true;
  bool camera_image_visible_ = false;
  bool camera_text_image_mode_ = false;
  bool nozzle_aux_visible_ = false;
  bool bed_aux_visible_ = false;
  bool ring_animation_active_ = false;
  bool swipe_switched_ = false;
  // Toggled by tapping the remaining-time row on page1: when true the row
  // shows the predicted finish wall-clock time instead of the remaining
  // duration. The clock-icon prefix is hidden in ETA mode to make room.
  bool show_eta_ = false;
  uint8_t active_ring_anim_kind_ = 0;
  uint32_t pulse_base_hex_ = 0;
  bool pulse_both_parts_ = false;
  lv_coord_t gesture_start_x_ = 0;
  lv_coord_t gesture_start_y_ = 0;
  int gesture_start_brightness_ = 80;
  int active_page_ = 0;
  // Page the current swipe gesture started on; used for the page-advance
  // threshold decision when the finger is released (handle_pager_event).
  int scroll_origin_page_ = 0;
  std::atomic<int> active_page_snapshot_{0};
  std::atomic<bool> page_scrolling_snapshot_{false};
  int last_parallax_clamped_ = -1;
  ArcColorScheme arc_colors_{};
  uint32_t last_accent_hex_ = 0;
  uint32_t last_ring_main_hex_ = UINT32_MAX;
  uint32_t last_ring_indicator_hex_ = UINT32_MAX;
  uint32_t last_ring_text_hex_ = UINT32_MAX;
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
  bool logo_clickable_ = false;
  bool logo_recolor_enabled_ = false;
  uint32_t logo_recolor_hex_ = 0;
  bool portal_lock_enabled_ = true;
  bool portal_request_authorized_ = false;
  bool portal_session_active_ = false;
  bool portal_pin_active_ = false;
  uint64_t portal_hint_boot_ms_ = 0;
  uint32_t portal_pin_remaining_s_ = 0;
  uint32_t portal_session_remaining_s_ = 0;
  std::string portal_pin_code_;
  std::string portal_hint_text_;
  std::string portal_overlay_title_text_;
  std::string portal_overlay_value_text_;
  std::string portal_overlay_detail_text_;
  mutable std::mutex camera_refresh_mutex_{};
  bool camera_refresh_requested_ = false;
  std::atomic<bool> chamber_light_toggle_requested_{false};
  std::atomic<uint8_t> print_command_request_{static_cast<uint8_t>(PrintCommand::kNone)};
  std::atomic<bool> portal_unlock_requested_{false};
  PrinterSnapshot deferred_snapshot_{};
  PrinterSnapshot last_snapshot_{};
  // Core Wi-Fi state, pushed via set_core_wifi_state() — see that method's
  // comment. compute_portal_texts_locked()'s only non-printer data source.
  bool core_wifi_connected_ = false;
  bool core_setup_ap_active_ = false;
  std::string core_wifi_ip_;
  DisplayRotation display_rotation_ = DisplayRotation::k0;
  int display_tilt_deci_deg_ = 0;
};

}  // namespace infohub
