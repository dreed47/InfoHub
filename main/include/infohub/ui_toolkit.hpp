#pragma once

#include <cstdint>
#include <string>

#include "lvgl.h"

namespace infohub {

// Small generic LVGL widget-building helpers shared between Ui and plugin
// content code (e.g. PrinterPlugin's own screens). Kept deliberately tiny —
// only what has more than one real consumer today.
void make_transparent(lv_obj_t* obj);
void enable_touch_bubble(lv_obj_t* obj);
void set_hidden(lv_obj_t* obj, bool hidden);
void set_label_text_if_changed(lv_obj_t* label, const char* text);
void set_label_text_if_changed(lv_obj_t* label, const std::string& text);

// RAII guard around the LVGL display lock (bsp_display_lock/unlock). Moved
// here from ui.cpp (Phase 4a, plugin-architecture extraction, see
// CLAUDE.md) so plugin content code that touches LVGL objects directly
// (e.g. PrinterPlugin's own screens, once Phase 4b/4c moves them out of Ui)
// can use the same primitive instead of reimplementing it — losing the
// wait/hold-time diagnostics and consecutive-failure tracking that guard
// against silent LVGL-lock contention would be a real regression, not a
// simplification. mark_phase()/note_phase() let a long widget-build
// sequence annotate which part was slow when the hold-time log fires.
class LvglLockGuard {
 public:
  explicit LvglLockGuard(uint32_t timeout_ms, const char* caller = "?");
  ~LvglLockGuard();

  void mark_phase(const char* phase);
  // Annotate the currently active guard from anywhere underneath it.
  static void note_phase(const char* phase);

  bool locked() const { return locked_; }

  static uint32_t lock_fail_count_;
  static uint32_t total_acquires_;

 private:
  const char* caller_ = "?";
  const char* phase_ = nullptr;
  uint32_t acquired_ts_ = 0;
  uint32_t phase_ts_ = 0;
  uint32_t acquire_seq_ = 0;
  bool locked_ = false;
  LvglLockGuard* previous_active_ = nullptr;

  static thread_local LvglLockGuard* active_;
};

}  // namespace infohub
