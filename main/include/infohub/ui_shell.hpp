#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "lvgl.h"
#include "infohub/config_store.hpp"
#include "infohub/plugins/printer/printer_state.hpp"

namespace infohub {

enum class ScreenPowerMode : uint8_t {
  kAwake,
  kDimmed,
  kOff,
};

// Phase 6/9 continuation of the plugin-architecture extraction (see CLAUDE.md
// "Ui changes sketch"). UiShell owns the chrome mechanics that don't need
// PrinterSnapshot: page-index bookkeeping (register_page_slot() stands in for
// the sketch's eventual multi-plugin register_pages()) and the dimming/
// brightness/power-save state machine. Ui still owns all LVGL object
// construction and every printer-content concern; it calls into UiShell
// through thin delegating methods so this is a state/logic relocation, not
// an API change — Application's calls to Ui are completely unaffected.
//
// NOT moved here (deliberately, see CLAUDE.md's phased sequencing notes):
// active_page_/scrolling_/scroll_origin_page_ and the portal PIN overlay.
// Both are read by dozens of printer-content call sites (apply_snapshot_locked
// alone touches active_page_/scrolling_ ~15 times) or coupled to
// PrinterSnapshot fields (compute_portal_texts_locked reads
// last_snapshot_.setup_ap_active/connection) — moving them needs a real
// design decision (a content-facing query API, or resolving the "does page 0
// become a plugin carousel" open question first), not a rename.
class UiShell {
 public:
  static constexpr int kPageIdxPrinterSelect = 0;
  static constexpr int kPageIdxAmsFirst = 1;
  static constexpr int kPageIdxAmsLast = kPageIdxAmsFirst + kMaxAmsUnits - 1;
  static constexpr int kPageIdxMain = kPageIdxAmsLast + 1;
  static constexpr int kPageIdxPreview = kPageIdxMain + 1;
  static constexpr int kPageIdxCamera = kPageIdxMain + 2;
  // Fixed core pages end at kPageIdxCamera. Beyond that is a generic pool of
  // plugin-owned pages, sized exactly to the sum of every compiled-in
  // plugin's declared Plugin::page_count() — no plugin gets a hardcoded slot
  // here by name. See configure_generic_page_pool()/reserve_plugin_pages().
  static constexpr int kPageIdxGenericFirst = kPageIdxCamera + 1;

  // --- Page registry: page_enabled()/page_object() table, see Phase 6 ---
  void register_page_slot(int index, lv_obj_t* const* object, const bool* enabled_flag);
  bool page_enabled(int page) const;
  lv_obj_t* page_object(int page) const;
  int next_enabled_page(int page, int direction) const;
  int clamp_enabled_page(int page) const;
  int nearest_enabled_page_for_scroll(int active_page) const;
  // True if at least one registered page slot is currently enabled — used to
  // decide whether the pager has anything to show at all, vs. every plugin
  // having been disabled in Web Config.
  bool any_page_enabled() const;
  // Highest valid kPageIdx-space index currently registered (core pages +
  // however many generic pages configure_generic_page_pool() reserved).
  int last_page_index() const { return static_cast<int>(page_slots_.size()) - 1; }

  // Resizes the page-slot table to fit `total_pages` generic plugin pages
  // beyond the fixed core set. Call once, before any reserve_plugin_pages()
  // call — Ui does this right after construction, before initialize().
  void configure_generic_page_pool(uint16_t total_pages);
  // Assigns `count` consecutive generic-pool indices to `plugin_id`, in call
  // order (first plugin to call this gets kPageIdxGenericFirst, etc).
  // Returns the base kPageIdx-space index (>= kPageIdxGenericFirst).
  int reserve_plugin_pages(const char* plugin_id, uint8_t count);
  // Looks up a previously-reserved range by plugin id — e.g. so an
  // enable/disable toggle can find which slots are "mine" without the
  // caller having to remember its own base index. False if this id never
  // called reserve_plugin_pages().
  bool plugin_page_range(const char* plugin_id, int* base, uint8_t* count) const;

  // --- Pager object + scroll lock ---
  void bind_pager(lv_obj_t* pager) { pager_ = pager; }
  lv_obj_t* pager() const { return pager_; }
  void set_pager_scroll_locked(bool locked);
  bool pager_scroll_locked() const { return pager_scroll_locked_; }

  // --- Dimming / brightness / power-save state machine ---
  void set_battery_display_policy(const BatteryDisplayPolicy& policy) {
    battery_display_policy_ = policy;
  }
  // Forces the next set_brightness_percent() call through (bypasses its
  // no-op-if-unchanged guard) — used once at boot so bsp_display_brightness_set()
  // actually runs instead of getting skipped because the default already
  // matches the requested value.
  void reset_brightness_state();
  void set_brightness_percent(int brightness_percent);
  int brightness_percent() const { return user_brightness_percent_; }
  void note_activity(bool wake_display_now);
  void wake_display();
  void update_power_save(bool on_battery, bool keep_awake, bool print_active);
  bool is_low_power_mode_active() const { return screen_power_mode_ != ScreenPowerMode::kAwake; }
  ScreenPowerMode screen_power_mode() const { return screen_power_mode_; }

 private:
  void apply_brightness_policy();

  struct PageSlot {
    lv_obj_t* const* object = nullptr;
    const bool* enabled_flag = nullptr;
  };
  // Pre-sized to the fixed core pages; configure_generic_page_pool() extends
  // it once with however many generic plugin pages are actually needed.
  std::vector<PageSlot> page_slots_ = std::vector<PageSlot>(kPageIdxGenericFirst);
  lv_obj_t* pager_ = nullptr;
  bool pager_scroll_locked_ = false;

  uint16_t next_generic_offset_ = 0;
  static constexpr uint8_t kMaxPluginPageRanges = 8;
  struct PluginPageRange {
    const char* id = nullptr;
    int base = 0;
    uint8_t count = 0;
  };
  std::array<PluginPageRange, kMaxPluginPageRanges> plugin_page_ranges_{};
  uint8_t plugin_page_range_count_ = 0;

  ScreenPowerMode screen_power_mode_ = ScreenPowerMode::kAwake;
  int user_brightness_percent_ = 80;
  int applied_brightness_percent_ = -1;
  std::atomic<uint32_t> last_activity_tick_ms_{0};
  BatteryDisplayPolicy battery_display_policy_{};
};

}  // namespace infohub
