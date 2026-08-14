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
#include "infohub/pmu.hpp"
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

// PrintCommand lives in printer_state.hpp today (printer-domain type), but
// Ui's request_print_command()/consume_print_command_request() only pass it
// through opaquely — declared here as a forward-friendly alias so ui.hpp
// doesn't need to include printer_state.hpp. (Defined as the same enum via
// the plugin.hpp chain that already pulls in wifi_manager.hpp etc.)
enum class PrintCommand : uint8_t;

class Ui {
 public:
  void set_display_rotation(DisplayRotation rotation);
  void set_display_tilt_deci_deg(int deci_deg);
  esp_err_t initialize();
  void set_arc_color_scheme(const ArcColorScheme& colors);
  const ArcColorScheme& arc_color_scheme() const { return arc_colors_; }
  void apply_ring_visual(const RingVisual& ring, int progress_value, uint32_t text_hex);
  // keep_awake: hard wake-lock (provisioning / camera page / page transition) —
  //             blocks both dimming and screen-off.
  // print_active: selects the "during print" timeouts instead of the idle
  //               timeouts; dimming/screen-off stay allowed per policy.
  void update_power_save(bool on_battery, bool keep_awake, bool print_active);
  void set_battery_display_policy(const BatteryDisplayPolicy& policy);
  bool is_low_power_mode_active() const;
  ScreenPowerMode screen_power_mode() const { return ui_shell_.screen_power_mode(); }
  // Generic query API: works for any plugin+local-page-index pair via
  // UiShell's plugin_page_range() registry. is_plugin_page_active()
  // additionally requires the pager isn't mid-scroll; is_plugin_page_visible()
  // doesn't (matches the old "visible" vs "active" distinction).
  bool is_plugin_page_active(const char* plugin_id, int local_idx) const {
    int base = 0;
    uint8_t count = 0;
    if (!ui_shell_.plugin_page_range(plugin_id, &base, &count) || local_idx < 0 ||
        local_idx >= static_cast<int>(count)) {
      return false;
    }
    return !page_scrolling_snapshot_.load(std::memory_order_relaxed) &&
           active_page_snapshot_.load(std::memory_order_relaxed) == (base + local_idx);
  }
  bool is_plugin_page_visible(const char* plugin_id, int local_idx) const {
    int base = 0;
    uint8_t count = 0;
    if (!ui_shell_.plugin_page_range(plugin_id, &base, &count) || local_idx < 0 ||
        local_idx >= static_cast<int>(count)) {
      return false;
    }
    return active_page_snapshot_.load(std::memory_order_relaxed) == (base + local_idx);
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
  // True while the portal PIN hint/overlay currently owns the screen real
  // estate a content plugin might also want (e.g. a status detail label) —
  // lets a plugin defer to the portal hint without Ui needing to know
  // anything about that plugin's own widgets.
  bool is_portal_hint_visible() const { return portal_hint_currently_shown_; }
  bool consume_chamber_light_toggle_request();
  bool has_chamber_light_toggle_request() const { return chamber_light_toggle_requested_.load(); }
  void request_chamber_light_toggle() { chamber_light_toggle_requested_.store(true); }
  // Pause / resume / stop buttons on the preview page set this request.
  // Application::loop polls it every iteration and dispatches via the LAN /
  // Cloud client. Returns kNone when no command pending. Consuming clears
  // the request atomically.
  PrintCommand consume_print_command_request();
  void request_print_command(PrintCommand command) {
    print_command_request_.store(static_cast<uint8_t>(command));
  }
  bool has_print_command_request() const {
    // 0 == PrintCommand::kNone (first enumerator, printer_state.hpp) — kept
    // as a bare literal here rather than PrintCommand::kNone so this header
    // only needs an opaque forward declaration of the enum, not its full
    // definition (which is printer-domain, in printer_state.hpp).
    return print_command_request_.load() != 0;
  }
  bool consume_portal_unlock_request();

  void request_wake_display();
  // Delegate to ui_shell_ — public so plugin-owned content code (moved out
  // of Ui, e.g. PrinterPlugin's) can note user activity the same way Ui's
  // own chrome does, without needing its own copy of the dimming state
  // machine.
  void note_activity(bool wake_display);

  // Generic plugin-page pool: every plugin (including the printer plugin,
  // as of Phase 4b/4c) reserves N pages for its own on-device screens.
  // Application calls reserve_plugin_page_pool() once at boot (before
  // initialize()) with the sum of every compiled-in plugin's
  // Plugin::page_count(), then register_plugin_pages() once per plugin
  // (after initialize()) to get that plugin's base index — pass
  // plugin_page_container(base + i) to Plugin::build_screen() for each page
  // i in [0, count).
  void reserve_plugin_page_pool(uint16_t total_pages);
  int register_plugin_pages(const char* plugin_id, uint8_t count);
  lv_obj_t* plugin_page_container(int page_index) const;
  // Same as plugin_page_container(), but resolved by plugin id + local page
  // index instead of an absolute pool index a caller would otherwise have
  // to remember from register_plugin_pages()'s return value.
  lv_obj_t* plugin_page_container_for(const char* plugin_id, int local_idx) const;
  // Overrides one page's pager-skip availability flag (whether swiping past
  // a disabled page should land on it) with a plugin-owned bool, instead of
  // the generic per-plugin-range default from register_plugin_pages(). For
  // a plugin whose page count is fixed but individual pages can be
  // dynamically present/absent (e.g. AMS unit pages, appear/disappear as
  // units connect). `enabled_flag` must outlive the plugin.
  void set_plugin_page_available(const char* plugin_id, int local_idx, const bool* enabled_flag);
  // Umbrella on/off switch for one plugin's entire reserved page range (all
  // pages it registered via register_plugin_pages()). Always re-runs the
  // pager's hide/parallax pass internally so a plugin can never forget to
  // (this was previously a recurring bug: a page-enable toggle that skipped
  // the resettle pass left stale printer-overlay text visible on top of the
  // newly-active page).
  // lock_timeout_ms defaults to 200 for live in-loop calls (e.g. a plugin's
  // own update_ui() re-asserting its state every tick, where a lost race
  // just gets retried next tick). A one-shot caller with no next tick to
  // retry on (e.g. a portal enable/disable toggle handler, which stops this
  // plugin's update_ui() calls the moment it disables) should pass a longer
  // timeout.
  void set_plugin_pages_enabled(const char* plugin_id, bool enabled, uint32_t lock_timeout_ms = 200);
  // Re-clamps/rescrolls/republishes/re-parallaxes the pager after a content
  // plugin changes one or more of its own pages' availability flags (see
  // set_plugin_page_available()) outside of the umbrella
  // set_plugin_pages_enabled() path, which already does this internally.
  // Pure chrome — no knowledge of which plugin or why.
  void resettle_pager_after_availability_change();
  // Jump directly to one plugin's page — used once at boot by the first
  // content plugin that wants a specific starting page (e.g. printer's main
  // dashboard rather than its page 0 printer-selector). A build with that
  // plugin disabled/absent just keeps Ui's own default starting page.
  void set_active_page_by_plugin(const char* plugin_id, int local_idx);

  // PrinterPlugin calls this once after building its page0 widgets, so Ui's
  // existing page0<->page1 scroll fade (apply_page0_parallax) has something
  // to animate without needing to know what a "printer card" is.
  void register_page0_fade_targets(lv_obj_t* title, lv_obj_t* card_list, lv_obj_t* empty_note);
  // Same static-fn/user_data idiom as every other LVGL callback in this file.
  // Invoked from set_active_page() when the pager returns to page0, so
  // PrinterPlugin can replay its card reveal animation.
  using Page0ReentryCallback = void (*)(void* user_data);
  void register_page0_reentry_callback(Page0ReentryCallback callback, void* user_data);
  // Generic single-slot callback registrations any content-owning plugin can
  // use (only PrinterPlugin does today) instead of Ui hardcoding a specific
  // plugin's widget-visibility/re-render logic:
  //  - visibility callback: invoked whenever chrome needs this plugin to
  //    re-show/hide its own widgets without necessarily re-rendering new
  //    data (scroll begin, availability toggles).
  //  - settle callback: invoked when the pager finishes landing on a page
  //    (mirrors the old direct apply_snapshot_locked() replay on
  //    set_active_page()).
  //  - tap callback: invoked on a plain screen tap that wasn't a swipe or a
  //    brightness drag (mirrors the old camera-refresh-on-tap behavior).
  using PluginPageCallback = void (*)(void* user_data);
  void register_page_visibility_callback(PluginPageCallback callback, void* user_data);
  void register_page_settle_callback(PluginPageCallback callback, void* user_data);
  void register_page_tap_callback(PluginPageCallback callback, void* user_data);
  // Battery overlay text/visibility — battery_icon_label_/battery_pct_label_
  // stay Ui-owned (genuinely shared/core: driven by PmuManager, not any
  // plugin's own data), but which page(s) they're visible on is currently
  // still special-cased to the printer plugin's pages (see CLAUDE.md's
  // "Battery routed through PrinterSnapshot" note — generalizing this is
  // explicitly future work, not this phase's job).
  void set_battery_overlay_text(const std::string& icon_text, const std::string& pct_text);
  void set_battery_overlay_visible(bool visible);
  // Core battery refresh: computes icon/pct text + a default visibility
  // (present || charging, no page-dependency) directly from PmuManager's
  // sample and pushes via the two methods above. Called every Application
  // loop iteration, independent of any plugin, so the overlay works even
  // with the printer plugin (or every plugin) disabled. A plugin may still
  // refine visibility further afterward via set_battery_overlay_visible()
  // (e.g. PrinterPlugin hides it on pages where it doesn't belong) — this
  // just guarantees a sane default exists first.
  void update_battery_overlay(const PowerSnapshot& power);

 private:
  // Phase 6/9 (plugin-architecture extraction, see CLAUDE.md "Ui changes
  // sketch"): UiShell owns page-index bookkeeping (page_enabled()/
  // page_object() below just delegate to it) and the dimming/brightness/
  // power-save state machine.
  UiShell ui_shell_{};

  void register_page_slots();

  esp_err_t build_dashboard();
  void apply_ring_visual_locked(const RingVisual& ring, int progress_value, uint32_t text_hex);
  void apply_page_visibility();
  // Shows/hides no_plugins_overlay_ based on whether any pager page is
  // currently enabled. Called after any page-enabled state changes
  // (set_plugin_pages_enabled(), resettle_pager_after_availability_change()).
  void update_no_plugins_overlay_locked();
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
  static void pulse_anim_exec_cb(void* var, int32_t scale);
  static void pager_event_cb(lv_event_t* event);
  static void screen_event_cb(lv_event_t* event);
  static void logo_event_cb(lv_event_t* event);

  bool initialized_ = false;
  lv_display_t* display_ = nullptr;
  lv_obj_t* screen_ = nullptr;
  lv_obj_t* fixed_overlay_ = nullptr;
  // Non-owning pointers registered by PrinterPlugin via
  // register_page0_fade_targets() — see that method's comment.
  lv_obj_t* page0_fade_title_ = nullptr;
  lv_obj_t* page0_fade_card_list_ = nullptr;
  lv_obj_t* page0_fade_empty_note_ = nullptr;
  Page0ReentryCallback page0_reentry_cb_ = nullptr;
  void* page0_reentry_user_data_ = nullptr;
  PluginPageCallback page_visibility_cb_ = nullptr;
  void* page_visibility_cb_user_data_ = nullptr;
  PluginPageCallback page_settle_cb_ = nullptr;
  void* page_settle_cb_user_data_ = nullptr;
  PluginPageCallback page_tap_cb_ = nullptr;
  void* page_tap_cb_user_data_ = nullptr;

  void apply_page0_parallax(bool force = false);

  // Generic plugin-page pool storage — sized once by reserve_plugin_page_pool()
  // (before initialize() creates the LVGL objects), never resized after, so
  // the raw pointers register_plugin_pages() hands to UiShell::register_page_slot()
  // stay valid for the lifetime of Ui. unique_ptr<T[]> instead of std::vector<bool>
  // specifically for plugin_pages_enabled_: vector<bool> is bit-packed and can't
  // hand out a stable bool* to an individual element.
  uint16_t plugin_pool_size_ = 0;
  std::unique_ptr<lv_obj_t*[]> plugin_pages_;
  std::unique_ptr<bool[]> plugin_pages_enabled_;
  lv_obj_t* no_plugins_overlay_ = nullptr;
  lv_obj_t* no_plugins_overlay_label_ = nullptr;
  lv_obj_t* status_arc_ = nullptr;
  lv_obj_t* progress_label_ = nullptr;
  lv_obj_t* battery_icon_label_ = nullptr;
  lv_obj_t* battery_pct_label_ = nullptr;
  lv_obj_t* brightness_overlay_ = nullptr;
  lv_obj_t* portal_hint_label_ = nullptr;
  lv_obj_t* portal_overlay_card_ = nullptr;
  lv_obj_t* portal_overlay_title_ = nullptr;
  lv_obj_t* portal_overlay_value_ = nullptr;
  lv_obj_t* portal_overlay_detail_ = nullptr;
  bool portal_hint_currently_shown_ = false;
  lv_timer_t* ring_anim_timer_ = nullptr;  // unused, ambient sweep timer removed
  bool gesture_active_ = false;
  bool overlay_visible_ = false;
  bool scrolling_ = false;
  bool accent_initialized_ = false;
  bool ring_animation_active_ = false;
  bool swipe_switched_ = false;
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
  // True from a real finger-press SCROLL_BEGIN until that same gesture
  // finalizes in handle_pager_event()'s SCROLL_END handling (covering its
  // ANIM_ON snap-animation follow-through, whose own SCROLL_BEGIN fires with
  // no pressed indev since the finger's already lifted by then). False the
  // rest of the time, including during any fully programmatic pager scroll
  // (set_active_page()'s own ANIM_OFF call, boot redirect, resettle, etc.),
  // so that scroll's SCROLL_END doesn't get reinterpreted as a gesture
  // release using stale scroll_origin_page_/scroll-position state.
  bool pager_gesture_active_ = false;
  std::atomic<int> active_page_snapshot_{0};
  std::atomic<bool> page_scrolling_snapshot_{false};
  int last_parallax_clamped_ = -1;
  ArcColorScheme arc_colors_{};
  uint32_t last_accent_hex_ = 0;
  uint32_t last_ring_main_hex_ = UINT32_MAX;
  uint32_t last_ring_indicator_hex_ = UINT32_MAX;
  uint32_t last_ring_text_hex_ = UINT32_MAX;
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
  std::atomic<bool> chamber_light_toggle_requested_{false};
  std::atomic<uint8_t> print_command_request_{0};
  std::atomic<bool> portal_unlock_requested_{false};
  // Core Wi-Fi state, pushed via set_core_wifi_state() — see that method's
  // comment. compute_portal_texts_locked()'s only non-printer data source.
  bool core_wifi_connected_ = false;
  bool core_setup_ap_active_ = false;
  std::string core_wifi_ip_;
  DisplayRotation display_rotation_ = DisplayRotation::k0;
  int display_tilt_deci_deg_ = 0;
};

}  // namespace infohub
