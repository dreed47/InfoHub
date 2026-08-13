#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include "lvgl.h"
#include "infohub/config_store.hpp"

namespace infohub {

enum class ScreenPowerMode : uint8_t {
  kAwake,
  kDimmed,
  kOff,
};

// Phase 6/9 continuation of the plugin-architecture extraction (see CLAUDE.md
// "Ui changes sketch"). UiShell owns the chrome mechanics: page-index
// bookkeeping (register_page_slot()/reserve_plugin_pages(), a real generic
// multi-plugin page pool as of Phase 4b/4c — every compiled-in plugin,
// printer included, is a uniform pool entry, no fixed/hardcoded slots) and
// the dimming/brightness/power-save state machine. Ui owns all LVGL object
// construction; content-owning plugins (only PrinterPlugin today) own their
// own widgets entirely — UiShell/Ui hold zero PrinterSnapshot or other
// plugin-content type references.
//
// NOT moved here (deliberately): active_page_/scrolling_/scroll_origin_page_
// and the portal PIN overlay stay on Ui — they're read by Ui's own chrome
// (gesture handling, parallax, portal hint) and by the generic
// is_plugin_page_active()/is_plugin_page_visible() query API content plugins
// use, not by anything UiShell itself needs to compute.
class UiShell {
 public:
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
  // Highest valid page-pool index currently registered.
  int last_page_index() const { return static_cast<int>(page_slots_.size()) - 1; }

  // Resizes the page-slot table to fit `total_pages` — the sum of every
  // compiled-in plugin's Plugin::page_count(). Call once, before any
  // reserve_plugin_pages() call — Ui does this right after construction,
  // before initialize().
  void configure_generic_page_pool(uint16_t total_pages);
  // Assigns `count` consecutive pool indices to `plugin_id`, in call order
  // (first plugin to call this gets base index 0, etc). Returns the base
  // index.
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
  // Empty until configure_generic_page_pool() sizes it to the sum of every
  // compiled-in plugin's page_count() — no fixed/hardcoded slots (Phase
  // 4b/4c, see CLAUDE.md).
  std::vector<PageSlot> page_slots_{};
  lv_obj_t* pager_ = nullptr;
  bool pager_scroll_locked_ = false;

  uint16_t next_pool_offset_ = 0;
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
