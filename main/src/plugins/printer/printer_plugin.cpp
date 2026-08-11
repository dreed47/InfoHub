#include "infohub/plugins/printer/printer_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "infohub/audio_notifier.hpp"
#include "infohub/config_store.hpp"
#include "infohub/pmu.hpp"
#include "infohub/setup_portal.hpp"
#include "infohub/plugins/printer/error_lookup.hpp"
#include "infohub/plugins/printer/status_resolver.hpp"
#include "infohub/portal_shared.hpp"
#include "infohub/ui.hpp"
#include "infohub/ui_toolkit.hpp"
#include "infohub/wifi_manager.hpp"

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#else
#error "Unknown InfoHub hardware variant"
#endif

// Font global, declared the same way ui.cpp/weather_plugin.cpp do: extern
// "C", at file scope (a namespaced extern here would create a mismatched
// mangled symbol against the plain-C font object).
extern "C" {
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_32;
}

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.printer";
constexpr TickType_t kStopBannerDuration = pdMS_TO_TICKS(12000);
constexpr TickType_t kHybridCameraCloudCooldown = pdMS_TO_TICKS(8000);
constexpr TickType_t kLocalMqttHandoffCooldown = pdMS_TO_TICKS(30000);
constexpr uint64_t kChamberLightOverrideMs = 6000;
// Pause/resume/stop optimistic-state window. Most printers reflect the new
// lifecycle in their next status report (sub-second). 5 s is a safety net so
// the button doesn't lock forever if the command is silently dropped.
constexpr uint64_t kPrintCommandOverrideMs = 5000;

bool local_print_is_live(const PrinterSnapshot& snapshot) {
  return snapshot.print_active || snapshot.lifecycle == PrintLifecycleState::kPreparing ||
         snapshot.lifecycle == PrintLifecycleState::kPrinting ||
         snapshot.lifecycle == PrintLifecycleState::kPaused;
}

bool cloud_print_is_live(const BambuCloudSnapshot& snapshot) {
  return snapshot.lifecycle == PrintLifecycleState::kPreparing ||
         snapshot.lifecycle == PrintLifecycleState::kPrinting ||
         snapshot.lifecycle == PrintLifecycleState::kPaused;
}

bool tick_deadline_active(TickType_t deadline, TickType_t now) {
  return deadline != 0 && static_cast<int32_t>(deadline - now) > 0;
}

PrinterModel preferred_model_for_routing(const PrinterSnapshot& local_snapshot,
                                         const BambuCloudSnapshot& cloud_snapshot) {
  if (cloud_snapshot.model != PrinterModel::kUnknown) {
    return cloud_snapshot.model;
  }
  return local_snapshot.local_model;
}

bool hybrid_prefers_cloud_status(const PrinterSnapshot& local_snapshot,
                                 const BambuCloudSnapshot& cloud_snapshot) {
  return printer_model_prefers_cloud_status(
      preferred_model_for_routing(local_snapshot, cloud_snapshot));
}

bool hybrid_local_status_supported(const PrinterSnapshot& local_snapshot,
                                   const BambuCloudSnapshot& cloud_snapshot) {
  return printer_model_supports_local_status(
      preferred_model_for_routing(local_snapshot, cloud_snapshot));
}

bool route_allows_local_jpeg_camera(SourceMode source_mode,
                                    const PrinterSnapshot& local_snapshot,
                                    const BambuCloudSnapshot& cloud_snapshot) {
  if (source_mode == SourceMode::kCloudOnly) {
    return false;
  }

  const PrinterModel model = preferred_model_for_routing(local_snapshot, cloud_snapshot);
  if (printer_model_has_jpeg_camera(model)) {
    return true;
  }
  if (model != PrinterModel::kUnknown) {
    return false;
  }

  if (source_mode == SourceMode::kLocalOnly) {
    return true;
  }
  return source_mode == SourceMode::kHybrid && local_snapshot.local_connected;
}

struct ChamberLightCommandPlan {
  bool try_local = false;
  bool try_cloud = false;
};

ChamberLightCommandPlan chamber_light_command_plan(SourceMode source_mode,
                                                   bool hybrid_prefers_cloud,
                                                   bool hybrid_local_status_supported_now,
                                                   bool local_network_ready,
                                                   bool local_printer_enabled,
                                                   bool cloud_network_ready,
                                                   const PrinterSnapshot& local_snapshot,
                                                   const BambuCloudSnapshot& cloud_snapshot) {
  ChamberLightCommandPlan plan;
  switch (source_mode) {
    case SourceMode::kLocalOnly:
      plan.try_local = true;
      break;
    case SourceMode::kCloudOnly:
      plan.try_cloud = true;
      break;
    case SourceMode::kHybrid:
    default:
      plan.try_local =
          !hybrid_prefers_cloud && hybrid_local_status_supported_now && local_network_ready &&
          local_printer_enabled &&
          (local_snapshot.local_connected ||
           printer_model_has_chamber_light(local_snapshot.local_model));
      plan.try_cloud =
          cloud_network_ready &&
          (cloud_snapshot.connected || printer_model_has_chamber_light(cloud_snapshot.model));
      break;
  }
  return plan;
}

void mark_chamber_light_state(PrinterSnapshot& snapshot, bool on) {
  snapshot.chamber_light_supported = true;
  snapshot.chamber_light_state_known = true;
  snapshot.chamber_light_on = on;
}

void mark_chamber_light_state(BambuCloudSnapshot& snapshot, bool on) {
  snapshot.chamber_light_supported = true;
  snapshot.chamber_light_state_known = true;
  snapshot.chamber_light_on = on;
}

// Decide whether a print-control command should be issued via the local broker,
// the cloud broker, or both. Mirrors the chamber-light routing but does not
// gate on chamber-light capability — pause/resume/stop is universal.
struct PrintCommandPlan {
  bool try_local = false;
  bool try_cloud = false;
};

PrintCommandPlan print_command_plan(SourceMode source_mode, bool hybrid_prefers_cloud,
                                    bool hybrid_local_status_supported_now,
                                    bool local_network_ready, bool local_printer_enabled,
                                    bool cloud_network_ready,
                                    const PrinterSnapshot& local_snapshot,
                                    const BambuCloudSnapshot& cloud_snapshot) {
  PrintCommandPlan plan;
  switch (source_mode) {
    case SourceMode::kLocalOnly:
      plan.try_local = true;
      break;
    case SourceMode::kCloudOnly:
      plan.try_cloud = true;
      break;
    case SourceMode::kHybrid:
    default:
      plan.try_local = !hybrid_prefers_cloud && hybrid_local_status_supported_now &&
                       local_network_ready && local_printer_enabled &&
                       local_snapshot.local_connected;
      plan.try_cloud = cloud_network_ready && cloud_snapshot.connected;
      break;
  }
  return plan;
}

// Resolve the printer lifecycle state implied by a freshly issued print
// command. Pause -> Paused, Resume -> Printing, Stop -> Idle. Used to drive
// the optimistic UI override until the next status report arrives.
PrintLifecycleState lifecycle_after_print_command(PrintCommand cmd) {
  switch (cmd) {
    case PrintCommand::kPause:
      return PrintLifecycleState::kPaused;
    case PrintCommand::kResume:
      return PrintLifecycleState::kPrinting;
    case PrintCommand::kStop:
      return PrintLifecycleState::kIdle;
    case PrintCommand::kNone:
    default:
      return PrintLifecycleState::kUnknown;
  }
}

// Status-ring color/animation decision logic, moved here from ui.cpp so Ui
// no longer needs to know about PrinterSnapshot/print-lifecycle at all for
// this path — see PrinterPlugin::apply_ring_visual(). Pure functions, no
// LVGL/Ui dependency; Ui::RingVisual/RingAnimKind are the shared, printer-
// agnostic output type any plugin's own ring-driving logic would produce.
constexpr uint32_t kRingBaseDark = 0x101010;

RingVisual lifecycle_ring_visual(const PrinterSnapshot& snapshot, const ArcColorScheme& colors) {
  const int progress = std::clamp(static_cast<int>(std::lround(snapshot.progress_percent)), 0, 100);
  RingVisual visual = {};

  // Filament load/unload animation — direction derived from resolver's ui_status.
  if (snapshot.ui_status == "loading" || snapshot.ui_status == "unloading") {
    visual.main_hex = kRingBaseDark;
    visual.indicator_hex = colors.filament;
    visual.anim_kind = (snapshot.ui_status == "loading") ? RingAnimKind::kFilamentLoad
                                                         : RingAnimKind::kFilamentUnload;
    return visual;
  }

  // Connection-level states (independent of print status).
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    visual.main_hex = colors.setup;
    visual.indicator_hex = colors.setup;
    return visual;
  }
  if (snapshot.connection == PrinterConnectionState::kError || snapshot.has_error ||
      snapshot.lifecycle == PrintLifecycleState::kError) {
    visual.main_hex = colors.error;
    visual.indicator_hex = colors.error;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.error;
    visual.pulse_period_ms = 1600U;
    return visual;
  }
  if (!snapshot.wifi_connected) {
    visual.main_hex = colors.offline;
    visual.indicator_hex = colors.offline;
    return visual;
  }

  // All remaining classifications come from the resolver (ui_status / lifecycle).
  if (snapshot.ui_status == "done" || snapshot.lifecycle == PrintLifecycleState::kFinished) {
    visual.main_hex = colors.done;
    visual.indicator_hex = colors.done;
    return visual;
  }

  if (snapshot.ui_status == "downloading") {
    visual.main_hex = kRingBaseDark;
    visual.indicator_hex = colors.preheat;
    visual.value_override = progress;
    return visual;
  }

  if (snapshot.ui_status == "preheating" || snapshot.ui_status == "preparing" ||
      snapshot.lifecycle == PrintLifecycleState::kPreparing) {
    visual.main_hex = colors.preheat;
    visual.indicator_hex = colors.preheat;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.preheat;
    visual.pulse_period_ms = 1400U;
    return visual;
  }

  if (snapshot.ui_status == "clean nozzle") {
    visual.main_hex = colors.clean;
    visual.indicator_hex = colors.clean;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.clean;
    visual.pulse_period_ms = 1200U;
    return visual;
  }

  if (snapshot.ui_status == "bed level") {
    visual.main_hex = colors.level;
    visual.indicator_hex = colors.level;
    visual.anim_kind = RingAnimKind::kPulseBoth;
    visual.pulse_base_hex = colors.level;
    visual.pulse_period_ms = 1400U;
    return visual;
  }

  if (snapshot.ui_status == "cooling") {
    visual.main_hex = colors.cool;
    visual.indicator_hex = colors.cool;
    return visual;
  }

  if (snapshot.ui_status == "offline") {
    visual.main_hex = colors.offline;
    visual.indicator_hex = colors.offline;
    return visual;
  }

  // Lifecycle-based fallback for states where ui_status is a free-form string.
  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPaused:
      visual.main_hex = kRingBaseDark;
      visual.indicator_hex = colors.printing;
      return visual;
    case PrintLifecycleState::kFinished:
      visual.main_hex = colors.done;
      visual.indicator_hex = colors.done;
      return visual;
    case PrintLifecycleState::kIdle:
      if (snapshot.print_active) {
        visual.main_hex = kRingBaseDark;
        visual.indicator_hex = colors.idle_active;
      } else {
        visual.main_hex = colors.idle;
        visual.indicator_hex = colors.idle;
      }
      return visual;
    case PrintLifecycleState::kUnknown:
    default:
      break;
  }

  if (snapshot.connection == PrinterConnectionState::kBooting ||
      snapshot.connection == PrinterConnectionState::kConnecting ||
      snapshot.connection == PrinterConnectionState::kReadyForLanConnect) {
    visual.main_hex = colors.setup;
    visual.indicator_hex = colors.setup;
    return visual;
  }

  visual.main_hex = colors.unknown;
  visual.indicator_hex = colors.unknown;
  return visual;
}

uint32_t stable_status_text_hex(const PrinterSnapshot& snapshot, const ArcColorScheme& colors) {
  // Filament
  if (snapshot.ui_status == "loading" || snapshot.ui_status == "unloading") {
    return colors.filament;
  }

  // Connection-level states
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return colors.setup;
  }
  if (snapshot.connection == PrinterConnectionState::kError || snapshot.has_error ||
      snapshot.lifecycle == PrintLifecycleState::kError) {
    return colors.error;
  }
  if (!snapshot.wifi_connected) {
    return colors.offline;
  }

  // Resolver-classified states
  if (snapshot.ui_status == "done" || snapshot.lifecycle == PrintLifecycleState::kFinished) {
    return colors.done;
  }
  if (snapshot.ui_status == "downloading") {
    return colors.preheat;
  }
  if (snapshot.ui_status == "preheating" || snapshot.ui_status == "preparing" ||
      snapshot.lifecycle == PrintLifecycleState::kPreparing) {
    return colors.preheat;
  }
  if (snapshot.ui_status == "clean nozzle") {
    return colors.clean;
  }
  if (snapshot.ui_status == "bed level") {
    return colors.level;
  }
  if (snapshot.ui_status == "cooling") {
    return colors.cool;
  }
  if (snapshot.ui_status == "offline") {
    return colors.offline;
  }

  // Lifecycle-based fallback
  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPaused:
      return colors.printing;
    case PrintLifecycleState::kPreparing:
      return colors.preheat;
    case PrintLifecycleState::kFinished:
      return colors.done;
    case PrintLifecycleState::kIdle:
      return snapshot.print_active ? colors.idle_active : colors.idle;
    case PrintLifecycleState::kUnknown:
    default:
      break;
  }

  if (snapshot.connection == PrinterConnectionState::kBooting ||
      snapshot.connection == PrinterConnectionState::kConnecting ||
      snapshot.connection == PrinterConnectionState::kReadyForLanConnect) {
    return colors.setup;
  }

  return colors.unknown;
}

// Page0 (printer-selector) card widgets, moved here from ui.cpp — see
// PrinterPlugin::build_screen()/update_printer_cards(). Pure LVGL helpers,
// zero adaptation needed beyond the move itself.
constexpr uint32_t kCardRevealDurationMs = 300U;
constexpr int32_t kCardRevealYStart = 28;
constexpr uint32_t kCardRevealStaggerMs = 55U;

// Micro-interaction: uniform scale on card tap (256 = 100% in LVGL 9)
void card_scale_exec_cb(void* obj, int32_t val) {
  lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(obj), val, 0);
}

// Cascading reveal: vertical slide-in per card
void card_reveal_y_exec_cb(void* obj, int32_t val) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(obj), val, 0);
}

// Cascading reveal: fade-in per card
void card_reveal_opa_exec_cb(void* obj, int32_t val) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(val), 0);
}
}  // namespace

esp_err_t PrinterPlugin::init(PluginContext& ctx) {
  config_store_ = &ctx.config_store;
  wifi_manager_ = &ctx.wifi_manager;
  ui_ = &ctx.ui;
  setup_portal_ = &ctx.setup_portal;
  pmu_manager_ = &ctx.pmu_manager;
  audio_notifier_ = &ctx.audio_notifier;

  const BambuCloudCredentials cloud_credentials = ctx.config_store.load_cloud_credentials();
  source_mode_ = ctx.config_store.load_source_mode();
  const PrinterConnection printer_connection =
      ctx.config_store.load_active_printer_profile().to_connection();
  cloud_client_.configure(cloud_credentials, printer_connection.serial);
  ESP_ERROR_CHECK(cloud_client_.start());

  printer_client_.configure(printer_connection);
  ESP_ERROR_CHECK(printer_client_.start());
  camera_client_.configure(printer_connection);
  ESP_ERROR_CHECK(camera_client_.start());

  filament_wake_enabled_ = ctx.config_store.load_filament_wake_enabled();
  filament_anim_enabled_ = ctx.config_store.load_filament_anim_enabled();

  if (!initialize_error_lookup_storage()) {
    ESP_LOGW(kTag, "Embedded error lookup unavailable; falling back to generic error text");
  }

  set_enabled(ctx.config_store.load_plugin_string(id(), "enabled") != "0");
  // Longer timeout than the default 200ms: this runs immediately after
  // Ui::initialize()'s own build_dashboard() call, which can still be
  // holding the LVGL lock for ~300ms — losing that race at boot would
  // silently leave the pager showing printer pages regardless of the
  // persisted disabled state (no retry exists once init() moves on).
  ui_->set_printer_plugin_enabled(enabled(), /*lock_timeout_ms=*/2000);
  return ESP_OK;
}

void PrinterPlugin::build_screen(lv_obj_t* parent) {
  title_ = lv_label_create(parent);
  set_label_text_if_changed(title_, "Printers");
  lv_obj_set_width(title_, 320);
  lv_label_set_long_mode(title_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(title_, &dosis_32, 0);
  lv_obj_set_style_text_color(title_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 60);

  card_list_ = lv_obj_create(parent);
  lv_obj_set_size(card_list_, 380, 300);
  lv_obj_align(card_list_, LV_ALIGN_CENTER, 0, 20);
  make_transparent(card_list_);
  lv_obj_set_flex_flow(card_list_, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card_list_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(card_list_, 10, 0);
  lv_obj_set_style_pad_all(card_list_, 0, 0);
  lv_obj_set_scroll_dir(card_list_, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(card_list_, LV_SCROLLBAR_MODE_OFF);

  empty_note_ = lv_label_create(parent);
  set_label_text_if_changed(empty_note_, "No printers configured.\nUse the web portal to add printers.");
  lv_obj_set_width(empty_note_, 320);
  lv_label_set_long_mode(empty_note_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(empty_note_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(empty_note_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(empty_note_, lv_color_hex(0x666666), 0);
  lv_obj_align(empty_note_, LV_ALIGN_CENTER, 0, 20);
  lv_obj_add_flag(empty_note_, LV_OBJ_FLAG_HIDDEN);

  ui_->register_page0_fade_targets(title_, card_list_, empty_note_);
  ui_->register_page0_reentry_callback(&PrinterPlugin::replay_card_animations_trampoline, this);
}

void PrinterPlugin::replay_card_animations_trampoline(void* user_data) {
  static_cast<PrinterPlugin*>(user_data)->replay_card_animations_locked();
}

void PrinterPlugin::printer_card_click_cb(lv_event_t* event) {
  auto* self = static_cast<PrinterPlugin*>(lv_event_get_user_data(event));
  lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(event));
  for (const auto& cw : self->page0_cards_) {
    if (cw.card == target) {
      self->pending_printer_switch_ = cw.profile_index;
      self->ui_->request_wake_display();

      // Micro-interaction: quick scale bounce (100% → 91% → 100%)
      lv_anim_t sa;
      lv_anim_init(&sa);
      lv_anim_set_var(&sa, target);
      lv_anim_set_exec_cb(&sa, card_scale_exec_cb);
      lv_anim_set_values(&sa, 256, 233);
      lv_anim_set_duration(&sa, 75);
      lv_anim_set_reverse_duration(&sa, 110);
      lv_anim_set_path_cb(&sa, lv_anim_path_ease_out);
      lv_anim_start(&sa);
      break;
    }
  }
}

int PrinterPlugin::consume_printer_switch_request() {
  int val = pending_printer_switch_;
  pending_printer_switch_ = -1;
  return val;
}

void PrinterPlugin::update_printer_cards(const std::vector<PrinterCardInfo>& cards) {
  // Only rebuild (and trigger the reveal animation) when the card data has
  // actually changed — the caller runs every main-loop tick, so without this
  // guard the animation restarts hundreds of times per second.
  const auto cards_equal = [](const std::vector<PrinterCardInfo>& a,
                               const std::vector<PrinterCardInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (a[i].index != b[i].index ||
          a[i].active != b[i].active ||
          a[i].connected != b[i].connected ||
          a[i].name != b[i].name ||
          a[i].model != b[i].model ||
          a[i].host != b[i].host) {
        return false;
      }
    }
    return true;
  };
  if (cards_equal(cards, last_printer_cards_)) {
    return;
  }
  last_printer_cards_ = cards;

  if (bsp_display_lock(200) != ESP_OK) {
    return;
  }
  rebuild_printer_cards_locked(cards);
  bsp_display_unlock();
}

void PrinterPlugin::rebuild_printer_cards_locked(const std::vector<PrinterCardInfo>& cards) {
  // Remove old card widgets
  for (auto& cw : page0_cards_) {
    if (cw.card != nullptr) {
      lv_obj_delete(cw.card);
    }
  }
  page0_cards_.clear();

  const bool empty = cards.empty();
  set_hidden(card_list_, empty);
  set_hidden(empty_note_, !empty);
  if (empty) {
    return;
  }

  const lv_font_t* font_name = &dosis_20;
  const lv_font_t* font_detail = &lv_font_montserrat_14;

  int card_idx = 0;
  for (const auto& info : cards) {
    // Card container — glasmorphism-lite: semi-transparent bg + shadow elevation
    lv_obj_t* card = lv_obj_create(card_list_);
    lv_obj_set_size(card, 340, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(card, 72, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1E1E1E), 0);
    lv_obj_set_style_bg_opa(card, 195, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    // Shadow for 3D depth on OLED
    lv_obj_set_style_shadow_width(card, 20, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_offset_y(card, 6, 0);

    if (info.active) {
      lv_obj_set_style_border_color(card, lv_color_hex(0x00CC66), 0);
      lv_obj_set_style_border_width(card, 2, 0);
      lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    } else {
      // Subtle border to define card edges on dark background
      lv_obj_set_style_border_color(card, lv_color_hex(0x303030), 0);
      lv_obj_set_style_border_width(card, 1, 0);
      lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    }

    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, &PrinterPlugin::printer_card_click_cb, LV_EVENT_CLICKED, this);

    // Status dot — small colored circle in top-right
    lv_obj_t* dot = lv_obj_create(card);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, info.connected ? lv_color_hex(0x00CC66) : lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    // Printer name (bold, larger)
    lv_obj_t* name_lbl = lv_label_create(card);
    const std::string display_name = info.name.empty() ? info.model : info.name;
    set_label_text_if_changed(name_lbl, display_name);
    lv_obj_set_width(name_lbl, 300);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name_lbl, font_name, 0);
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    // Model
    lv_obj_t* model_lbl = lv_label_create(card);
    set_label_text_if_changed(model_lbl, info.model.empty() ? "Unknown" : info.model);
    lv_obj_set_width(model_lbl, 300);
    lv_label_set_long_mode(model_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(model_lbl, font_detail, 0);
    lv_obj_set_style_text_color(model_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align_to(model_lbl, name_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    // Host IP
    lv_obj_t* host_lbl = lv_label_create(card);
    set_label_text_if_changed(host_lbl, info.host.empty() ? "No local IP" : info.host);
    lv_obj_set_width(host_lbl, 300);
    lv_label_set_long_mode(host_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(host_lbl, font_detail, 0);
    lv_obj_set_style_text_color(host_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align_to(host_lbl, model_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    PrinterCardWidgets cw;
    cw.card = card;
    cw.name_label = name_lbl;
    cw.model_label = model_lbl;
    cw.host_label = host_lbl;
    cw.status_dot = dot;
    cw.profile_index = info.index;
    page0_cards_.push_back(cw);

    // Cascading reveal: start hidden below, fade + slide in with staggered delay
    lv_obj_set_style_opa(card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(card, kCardRevealYStart, 0);
    const uint32_t reveal_delay = static_cast<uint32_t>(card_idx) * kCardRevealStaggerMs;

    lv_anim_t ry;
    lv_anim_init(&ry);
    lv_anim_set_var(&ry, card);
    lv_anim_set_exec_cb(&ry, card_reveal_y_exec_cb);
    lv_anim_set_values(&ry, kCardRevealYStart, 0);
    lv_anim_set_duration(&ry, kCardRevealDurationMs);
    lv_anim_set_delay(&ry, reveal_delay);
    LV_ANIM_SET_EASE_OUT_BACK(&ry);
    lv_anim_set_path_cb(&ry, lv_anim_path_custom_bezier3);
    lv_anim_start(&ry);

    lv_anim_t ro;
    lv_anim_init(&ro);
    lv_anim_set_var(&ro, card);
    lv_anim_set_exec_cb(&ro, card_reveal_opa_exec_cb);
    lv_anim_set_values(&ro, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ro, kCardRevealDurationMs);
    lv_anim_set_delay(&ro, reveal_delay);
    lv_anim_set_path_cb(&ro, lv_anim_path_ease_out);
    lv_anim_start(&ro);

    ++card_idx;
  }
}

void PrinterPlugin::replay_card_animations_locked() {
  int card_idx = 0;
  for (const PrinterCardWidgets& cw : page0_cards_) {
    lv_obj_set_style_opa(cw.card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(cw.card, kCardRevealYStart, 0);
    const uint32_t reveal_delay = static_cast<uint32_t>(card_idx) * kCardRevealStaggerMs;

    lv_anim_t ry;
    lv_anim_init(&ry);
    lv_anim_set_var(&ry, cw.card);
    lv_anim_set_exec_cb(&ry, card_reveal_y_exec_cb);
    lv_anim_set_values(&ry, kCardRevealYStart, 0);
    lv_anim_set_duration(&ry, kCardRevealDurationMs);
    lv_anim_set_delay(&ry, reveal_delay);
    LV_ANIM_SET_EASE_OUT_BACK(&ry);
    lv_anim_set_path_cb(&ry, lv_anim_path_custom_bezier3);
    lv_anim_start(&ry);

    lv_anim_t ro;
    lv_anim_init(&ro);
    lv_anim_set_var(&ro, cw.card);
    lv_anim_set_exec_cb(&ro, card_reveal_opa_exec_cb);
    lv_anim_set_values(&ro, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&ro, kCardRevealDurationMs);
    lv_anim_set_delay(&ro, reveal_delay);
    lv_anim_set_path_cb(&ro, lv_anim_path_ease_out);
    lv_anim_start(&ro);

    ++card_idx;
  }
}

bool PrinterPlugin::is_configured() const {
  // No enabled() check here — SetupPortal's generic is_provisioning_complete()
  // already skips disabled plugins before ever calling this.
  const SourceMode source_mode = config_store_->load_source_mode();
  const bool local_connected =
      printer_client_.snapshot().connection == PrinterConnectionState::kOnline;
  const bool cloud_connected = cloud_portal_ready(cloud_client_.snapshot());

  switch (source_mode) {
    case SourceMode::kCloudOnly:
      return cloud_connected;
    case SourceMode::kLocalOnly:
      return local_connected;
    case SourceMode::kHybrid:
    default:
      return cloud_connected || local_connected;
  }
}

void PrinterPlugin::tick(uint64_t now_ms) {
  const TickType_t now_tick = xTaskGetTickCount();
  const int switch_idx = consume_printer_switch_request();
  if (switch_idx >= 0 &&
      static_cast<uint8_t>(switch_idx) != config_store_->load_active_printer_index()) {
    config_store_->save_active_printer_index(static_cast<uint8_t>(switch_idx));
    const PrinterConnection new_conn = config_store_->load_active_printer_profile().to_connection();
    printer_client_.configure(new_conn);
    camera_client_.configure(new_conn);
    cloud_client_.configure(config_store_->load_cloud_credentials(), new_conn.serial);
    ESP_LOGI(kTag, "Switched active printer to profile %d", switch_idx);
  }
  if (ui_->is_config_page_active()) {
    const auto profiles = config_store_->load_printer_profiles();
    const uint8_t active_idx = config_store_->load_active_printer_index();
    const bool local_connected = printer_client_.snapshot().local_connected;
    std::vector<PrinterCardInfo> cards;
    cards.reserve(profiles.size());
    for (const auto& p : profiles) {
      PrinterCardInfo ci;
      ci.index = p.index;
      ci.name = p.display_name;
      ci.model = p.model;
      ci.host = p.host;
      ci.active = (p.index == active_idx);
      ci.connected = ci.active && local_connected;
      cards.push_back(std::move(ci));
    }
    update_printer_cards(cards);
  }
  const bool wifi_connected = wifi_manager_->is_station_connected();
  const std::string wifi_ip = wifi_manager_->station_ip();
  const bool page_transition_active = ui_->is_page_transition_active();
  const bool preview_page_active = ui_->is_page2_active();
  const bool camera_page_active = ui_->is_camera_page_active();
  source_mode_ = config_store_->load_source_mode();
  const bool source_mode_changed = source_mode_ != last_source_mode_;
  const bool wifi_lost = !wifi_connected && last_wifi_connected_;
  local_printer_enabled_ = printer_client_.is_configured();
  PrinterSnapshot local_snapshot = printer_client_.snapshot();
  if (local_snapshot.local_connected && local_mqtt_handoff_until_tick_.load() != 0) {
    local_mqtt_handoff_until_tick_ = 0;
    ESP_LOGI(kTag, "Local MQTT handoff complete: local MQTT connected");
  }
  const bool camera_page_visible = ui_->is_camera_page_visible();

  if (source_mode_ == SourceMode::kHybrid && last_camera_page_active_ && !camera_page_visible &&
      wifi_connected) {
    hybrid_camera_cooldown_deadline_ = now_tick + kHybridCameraCloudCooldown;
    ESP_LOGD(kTag, "Hybrid mode: delaying cloud path briefly after camera activity");
  }
  if (source_mode_changed || wifi_lost || source_mode_ != SourceMode::kHybrid) {
    hybrid_local_gate_open_ = false;
    hybrid_camera_cooldown_deadline_ = 0;
    // The handoff window is a hybrid-only concept; drop any stale deadline
    // on mode switch / Wi-Fi loss so it cannot block cloud MQTT later.
    local_mqtt_handoff_until_tick_ = 0;
  }

  BambuCloudSnapshot cloud_snapshot = cloud_client_.snapshot();
  const bool hybrid_prefers_cloud =
      source_mode_ == SourceMode::kHybrid &&
      hybrid_prefers_cloud_status(local_snapshot, cloud_snapshot);
  const bool hybrid_local_status_supported_now =
      source_mode_ != SourceMode::kCloudOnly &&
      hybrid_local_status_supported(local_snapshot, cloud_snapshot);
  const PrinterModel routing_model = preferred_model_for_routing(local_snapshot, cloud_snapshot);
  const bool routing_model_has_jpeg_camera = printer_model_has_jpeg_camera(routing_model);
  const bool camera_model_has_jpeg =
      route_allows_local_jpeg_camera(source_mode_, local_snapshot, cloud_snapshot);
  const bool hybrid_camera_cooldown_active =
      source_mode_ == SourceMode::kHybrid &&
      tick_deadline_active(hybrid_camera_cooldown_deadline_, now_tick);
  const bool hybrid_cloud_allows_warm_local =
      !cloud_snapshot.configured ||
      (cloud_snapshot.session_connected && cloud_snapshot.printer_online) ||
      local_snapshot.local_connected;
  const bool hybrid_local_camera_demand =
      source_mode_ == SourceMode::kHybrid && routing_model_has_jpeg_camera &&
      hybrid_cloud_allows_warm_local;
  // Local MQTT is an independent status/command transport. It must not be
  // gated by camera type or cloud presence; only the heavier JPEG camera path
  // remains demand-driven below.
  if (source_mode_ == SourceMode::kHybrid) {
    if (!wifi_connected || !local_printer_enabled_) {
      if (hybrid_local_gate_open_) {
        ESP_LOGI(kTag, "Hybrid mode: disabling local MQTT path");
      }
      hybrid_local_gate_open_ = false;
    } else {
      if (!hybrid_local_gate_open_) {
        ESP_LOGI(kTag,
                 "Hybrid mode: local printer configured, enabling local MQTT "
                 "(model=%s camera_demand=%d)",
                 to_string(routing_model), hybrid_local_camera_demand ? 1 : 0);
        if (!local_snapshot.local_connected) {
          // Serialize the connect phase: give the local MQTT TLS handshake a
          // quiet window without concurrent cloud HTTPS fetches / cloud MQTT
          // (re)connects. TLS handshakes are the heap spikes — steady-state
          // traffic can overlap. Cleared early once local MQTT connects.
          local_mqtt_handoff_until_tick_ = now_tick + kLocalMqttHandoffCooldown;
          ESP_LOGI(kTag, "Hybrid mode: pausing cloud traffic for local MQTT handoff");
        }
      }
      hybrid_local_gate_open_ = true;
    }
  }
  // Evaluated after the gate block so a handoff window opened above pauses
  // cloud traffic in this very iteration (not one loop later).
  const bool local_mqtt_handoff_active =
      tick_deadline_active(local_mqtt_handoff_until_tick_.load(), now_tick);

  const bool local_network_ready =
      wifi_connected && local_printer_enabled_ &&
      (source_mode_ == SourceMode::kLocalOnly ||
       (source_mode_ == SourceMode::kHybrid && hybrid_local_gate_open_));
  const bool local_camera_network_ready =
      local_network_ready &&
      (source_mode_ == SourceMode::kLocalOnly || hybrid_local_camera_demand);
  printer_client_.set_network_ready(local_network_ready);
  camera_client_.set_network_ready(local_camera_network_ready);

  local_snapshot.wifi_connected = wifi_connected;
  local_snapshot.wifi_ip = wifi_ip;
  local_snapshot.setup_ap_active = wifi_manager_->is_setup_access_point_active();
  local_snapshot.setup_ap_ssid = wifi_manager_->setup_access_point_ssid();
  local_snapshot.setup_ap_password = wifi_manager_->setup_access_point_password();
  local_snapshot.setup_ap_ip = wifi_manager_->setup_access_point_ip();
  camera_client_.observe_printer_snapshot(local_snapshot);
  if (last_local_print_live_ && local_snapshot.non_error_stop) {
    stop_banner_until_tick_ = now_tick + kStopBannerDuration;
  } else if (source_mode_ != SourceMode::kCloudOnly && !local_snapshot.non_error_stop) {
    stop_banner_until_tick_ = 0;
  }
  local_snapshot.show_stop_banner =
      local_snapshot.non_error_stop && tick_deadline_active(stop_banner_until_tick_, now_tick);
  resolve_ui_state(local_snapshot);

  const bool camera_enabled =
      source_mode_ != SourceMode::kCloudOnly && local_printer_enabled_ &&
      local_camera_network_ready &&
      camera_model_has_jpeg && local_snapshot.local_connected && !local_mqtt_handoff_active &&
      camera_page_active &&
      ui_->screen_power_mode() != ScreenPowerMode::kOff;
  camera_client_.set_enabled(camera_enabled);
  if (ui_->consume_camera_refresh_request()) {
    camera_client_.request_refresh();
  }

  const bool hybrid_local_path_healthy =
      source_mode_ == SourceMode::kHybrid && local_network_ready && local_printer_enabled_ &&
      local_snapshot.local_connected && hybrid_local_status_supported_now && !hybrid_prefers_cloud;
  // Keep the cloud session warm in hybrid mode even while the local path is
  // the active status source. Tearing the whole cloud path down (previous
  // behaviour: cloud_network_ready=false) parked the cloud task in the
  // "Waiting for Wi-Fi" loop (visible session flapping) and forced a fresh
  // HTTPS login + bindings + preview burst — several TLS handshakes right
  // next to the local MQTT/camera connections — whenever the user swiped to
  // the preview page. Instead the session/token stays alive and only the
  // heavy traffic sources (live MQTT, HTTPS fetches) are gated below.
  const bool cloud_network_ready = wifi_connected && source_mode_ != SourceMode::kLocalOnly;
  const bool hybrid_cloud_idle =
      source_mode_ == SourceMode::kHybrid && hybrid_local_path_healthy && !preview_page_active;
  const bool cloud_live_mqtt_enabled =
      cloud_network_ready &&
      !local_mqtt_handoff_active &&
      (source_mode_ == SourceMode::kCloudOnly ||
       (source_mode_ == SourceMode::kHybrid &&
        (hybrid_prefers_cloud || !hybrid_local_path_healthy)));
  const bool pause_cloud_fetches =
      source_mode_ == SourceMode::kHybrid &&
      (hybrid_cloud_idle ||
       (hybrid_local_gate_open_ &&
        (camera_page_active || page_transition_active || hybrid_camera_cooldown_active)) ||
       local_mqtt_handoff_active || !cloud_network_ready);
  cloud_client_.set_network_ready(cloud_network_ready);
  cloud_client_.set_live_mqtt_enabled(cloud_live_mqtt_enabled);
  cloud_client_.set_fetch_paused(pause_cloud_fetches);

  cloud_snapshot = cloud_client_.snapshot();
  if (source_mode_ == SourceMode::kCloudOnly) {
    if (last_cloud_print_live_ && cloud_snapshot.non_error_stop) {
      stop_banner_until_tick_ = now_tick + kStopBannerDuration;
    } else if (!cloud_snapshot.non_error_stop) {
      stop_banner_until_tick_ = 0;
    }
  }
  auto build_merged_snapshot = [&](const PrinterSnapshot& current_local_snapshot,
                                   const BambuCloudSnapshot& current_cloud_snapshot) {
    PrinterSnapshot merged =
        merge_status_sources(current_local_snapshot, local_printer_enabled_, current_cloud_snapshot,
                             source_mode_, now_ms, wifi_connected, wifi_ip);
    merged.setup_ap_active = current_local_snapshot.setup_ap_active;
    merged.setup_ap_ssid = current_local_snapshot.setup_ap_ssid;
    merged.setup_ap_password = current_local_snapshot.setup_ap_password;
    merged.setup_ap_ip = current_local_snapshot.setup_ap_ip;
    merged.show_stop_banner =
        merged.non_error_stop && tick_deadline_active(stop_banner_until_tick_, now_tick);
    merged.preview_page_available = source_mode_ != SourceMode::kLocalOnly;
    merged.camera_page_available =
        route_allows_local_jpeg_camera(source_mode_, current_local_snapshot,
                                       current_cloud_snapshot);
    return merged;
  };
  auto apply_chamber_light_override = [&](PrinterSnapshot* target_snapshot) {
    if (target_snapshot == nullptr) {
      return;
    }
    if (!chamber_light_override_active_) {
      return;
    }
    if (now_ms >= chamber_light_override_until_ms_) {
      chamber_light_override_active_ = false;
      chamber_light_override_until_ms_ = 0;
      return;
    }
    target_snapshot->chamber_light_supported = true;
    target_snapshot->chamber_light_state_known = true;
    target_snapshot->chamber_light_on = chamber_light_override_on_;
  };
  auto apply_print_command_override = [&](PrinterSnapshot* target_snapshot) {
    if (target_snapshot == nullptr ||
        print_command_override_kind_ == PrintCommand::kNone) {
      return;
    }
    const PrintLifecycleState desired =
        lifecycle_after_print_command(print_command_override_kind_);
    // Clear early once the printer's actual lifecycle confirms the command.
    if (target_snapshot->lifecycle == desired || now_ms >= print_command_override_until_ms_) {
      print_command_override_kind_ = PrintCommand::kNone;
      print_command_override_until_ms_ = 0;
      target_snapshot->print_command_pending_kind = PrintCommand::kNone;
      return;
    }
    target_snapshot->lifecycle = desired;
    target_snapshot->print_command_pending_kind = print_command_override_kind_;
  };
  PrinterSnapshot snapshot = build_merged_snapshot(local_snapshot, cloud_snapshot);
  apply_chamber_light_override(&snapshot);
  apply_print_command_override(&snapshot);

  if (ui_->consume_chamber_light_toggle_request()) {
    const bool requested_on =
        !snapshot.chamber_light_state_known || !snapshot.chamber_light_on;
    bool command_sent = false;
    const ChamberLightCommandPlan light_plan =
        chamber_light_command_plan(source_mode_, hybrid_prefers_cloud,
                                   hybrid_local_status_supported_now, local_network_ready,
                                   local_printer_enabled_, cloud_network_ready,
                                   local_snapshot, cloud_snapshot);

    if (light_plan.try_local) {
      command_sent = printer_client_.set_chamber_light(requested_on);
      if (command_sent) {
        mark_chamber_light_state(local_snapshot, requested_on);
      }
    }
    if (!command_sent && light_plan.try_cloud) {
      command_sent = cloud_client_.set_chamber_light(requested_on);
      if (command_sent) {
        mark_chamber_light_state(cloud_snapshot, requested_on);
      }
    }

    if (!command_sent) {
      ESP_LOGW(kTag, "Chamber light toggle failed in %s mode", to_string(source_mode_));
    } else {
      chamber_light_override_active_ = true;
      chamber_light_override_on_ = requested_on;
      chamber_light_override_until_ms_ = now_ms + kChamberLightOverrideMs;
      snapshot = build_merged_snapshot(local_snapshot, cloud_snapshot);
      apply_chamber_light_override(&snapshot);
    }
  }

  if (const PrintCommand requested_print_cmd = ui_->consume_print_command_request();
      requested_print_cmd != PrintCommand::kNone) {
    bool command_sent = false;
    const PrintCommandPlan plan = print_command_plan(
        source_mode_, hybrid_prefers_cloud, hybrid_local_status_supported_now,
        local_network_ready, local_printer_enabled_, cloud_network_ready, local_snapshot,
        cloud_snapshot);
    if (plan.try_local) {
      command_sent = printer_client_.set_print_command(requested_print_cmd);
    }
    if (!command_sent && plan.try_cloud) {
      command_sent = cloud_client_.set_print_command(requested_print_cmd);
    }

    if (!command_sent) {
      ESP_LOGW(kTag, "Print command %s failed in %s mode", to_string(requested_print_cmd),
               to_string(source_mode_));
    } else {
      ESP_LOGI(kTag, "Print command %s issued (%s)", to_string(requested_print_cmd),
               to_string(source_mode_));
      print_command_override_kind_ = requested_print_cmd;
      print_command_override_until_ms_ = now_ms + kPrintCommandOverrideMs;
      snapshot = build_merged_snapshot(local_snapshot, cloud_snapshot);
      apply_chamber_light_override(&snapshot);
      apply_print_command_override(&snapshot);
    }
  }

  const PowerSnapshot power = pmu_manager_->sample();
  if (power.available) {
    snapshot.battery_percent = power.battery_percent;
    snapshot.battery_present = power.battery_present;
    snapshot.charging = power.charging;
    snapshot.usb_present = power.usb_present;
    snapshot.pmu_temp_c = power.temperature_c;
  }

  const P1sCameraSnapshot camera_snapshot = camera_client_.snapshot();
  if (source_mode_ == SourceMode::kCloudOnly || !local_printer_enabled_ ||
      !snapshot.camera_page_available) {
    snapshot.camera_connected = false;
    if (source_mode_ == SourceMode::kCloudOnly) {
      snapshot.camera_detail = "Camera unavailable in cloud-only mode";
    } else if (!local_printer_enabled_) {
      snapshot.camera_detail = "Local camera not configured";
    } else {
      snapshot.camera_detail = "Camera unavailable on this model";
    }
    snapshot.camera_blob.reset();
    snapshot.camera_width = 0;
    snapshot.camera_height = 0;
    snapshot.camera_source = FieldSource::kNone;
  } else {
    snapshot.camera_connected = camera_snapshot.connected;
    snapshot.camera_detail = camera_snapshot.detail;
    snapshot.camera_blob = camera_snapshot.frame_blob;
    snapshot.camera_width = camera_snapshot.width;
    snapshot.camera_height = camera_snapshot.height;
    if (!camera_page_active) {
      snapshot.camera_blob.reset();
      snapshot.camera_width = 0;
      snapshot.camera_height = 0;
    }
  }

  // Detect filament stage before resolve_ui_state for animation suppression and wake logic.
  const bool is_filament = is_filament_stage(snapshot.stage);
  const bool is_external_spool = snapshot.tray_tar == 254;

  // When filament animation is disabled, suppress the loading/unloading stage for AMS auto
  // changes so resolve_ui_state treats it as normal printing (no arc animation).
  if (!filament_anim_enabled_ && is_filament && !is_external_spool) {
    snapshot.stage.clear();
    snapshot.raw_stage.clear();
  }

  resolve_ui_state(snapshot);
  // Audio-notification edge detection. Runs strictly off the merged
  // PrinterSnapshot so it sees the same view that the UI does - no double
  // beeps when cloud and local report the same transition.
  {
    const PrintLifecycleState lc = snapshot.lifecycle;
    const bool has_err = snapshot.has_error;
    const int err_code = snapshot.print_error_code;
    const size_t hms_count = snapshot.hms_codes.size();
    if (audio_state_primed_) {
      if (lc != audio_last_lifecycle_) {
        if (lc == PrintLifecycleState::kFinished &&
            audio_last_lifecycle_ == PrintLifecycleState::kPrinting) {
          audio_notifier_->play(AudioNotifier::Event::kPrintFinished);
        } else if (lc == PrintLifecycleState::kPrinting &&
                   (audio_last_lifecycle_ == PrintLifecycleState::kIdle ||
                    audio_last_lifecycle_ == PrintLifecycleState::kPreparing ||
                    audio_last_lifecycle_ == PrintLifecycleState::kUnknown)) {
          audio_notifier_->play(AudioNotifier::Event::kPrintStarted);
        } else if (lc == PrintLifecycleState::kPaused &&
                   audio_last_lifecycle_ == PrintLifecycleState::kPrinting) {
          audio_notifier_->play(AudioNotifier::Event::kPrintPaused);
        } else if (lc == PrintLifecycleState::kError &&
                   audio_last_lifecycle_ != PrintLifecycleState::kError) {
          audio_notifier_->play(AudioNotifier::Event::kPrintError);
        }
      } else if ((has_err && !audio_last_has_error_) ||
                 (err_code != 0 && err_code != audio_last_print_error_code_)) {
        audio_notifier_->play(AudioNotifier::Event::kPrintError);
      } else if (hms_count > audio_last_hms_count_) {
        audio_notifier_->play(AudioNotifier::Event::kHmsAlert);
      }
    }
    audio_last_lifecycle_ = lc;
    audio_last_has_error_ = has_err;
    audio_last_print_error_code_ = err_code;
    audio_last_hms_count_ = hms_count;
    audio_state_primed_ = true;
  }
  last_local_print_live_ = local_print_is_live(local_snapshot);
  last_cloud_print_live_ = cloud_print_is_live(cloud_snapshot);

  const bool on_battery = power.available && power.battery_present && !power.usb_present;
  const bool preview_pipeline_enabled =
      source_mode_ == SourceMode::kCloudOnly || preview_page_active;
  cloud_client_.set_preview_fetch_enabled(source_mode_ != SourceMode::kLocalOnly &&
                                          preview_pipeline_enabled);
  const bool provisioning_active =
      snapshot.setup_ap_active ||
      snapshot.connection == PrinterConnectionState::kWaitingForCredentials;
  // Hard wake-lock only for provisioning / camera page / page transitions.
  // An active print no longer forces the screen awake — it just switches the
  // energy policy to the "during print" timeouts (see update_power_save).
  const bool keep_screen_awake =
      provisioning_active || camera_page_active || page_transition_active;
  if (filament_wake_enabled_ && is_filament && is_external_spool) {
    // External-spool filament change needs the user at the printer: wake once.
    ui_->request_wake_display();
  }
  ui_->update_power_save(on_battery, keep_screen_awake, snapshot.print_active);

  cloud_client_.set_low_power_mode(camera_page_active || page_transition_active ||
                                   (on_battery && ui_->is_low_power_mode_active() &&
                                    !snapshot.print_active));

  desired_poll_interval_ms_ =
      (snapshot.print_active || camera_page_active || page_transition_active ||
       !ui_->is_low_power_mode_active())
          ? (page_transition_active ? 100 : 500)
          : 1500;
  keep_screen_awake_ = keep_screen_awake;
  last_source_mode_ = source_mode_;
  last_wifi_connected_ = wifi_connected;
  last_camera_page_active_ = camera_page_visible;

  latest_snapshot_ = std::move(snapshot);
}

void PrinterPlugin::apply_ring_visual() {
  if (ui_ == nullptr) {
    return;
  }
  // Nothing real to show yet — same guard Ui::set_arc_color_scheme used to
  // apply before this logic moved here.
  if (latest_snapshot_.ui_status.empty() && latest_snapshot_.stage.empty() &&
      latest_snapshot_.detail.empty()) {
    return;
  }
  const ArcColorScheme& colors = ui_->arc_color_scheme();
  const RingVisual ring = lifecycle_ring_visual(latest_snapshot_, colors);
  const uint32_t text_hex = stable_status_text_hex(latest_snapshot_, colors);
  const int progress =
      std::clamp(static_cast<int>(latest_snapshot_.progress_percent + 0.5f), 0, 100);
  ui_->apply_ring_visual(ring, progress, text_hex);
}

void PrinterPlugin::update_ui() {
  // Portal PIN/hint push is now core (Application calls it directly every
  // loop iteration, independent of any plugin) — see
  // Ui::update_portal_access_visuals().
  ui_->apply_snapshot(latest_snapshot_);
  apply_ring_visual();
}

bool PrinterPlugin::wants_network() const {
  // Best-effort hint; NetworkArbiter is not wired into the connect call sites
  // yet (Phase 2 follow-up), so nothing consumes this today.
  return local_printer_enabled_;
}

bool PrinterPlugin::wants_awake() const { return keep_screen_awake_; }

uint32_t PrinterPlugin::desired_poll_interval_ms() const { return desired_poll_interval_ms_; }

}  // namespace infohub
