// Phase 4b/4c (plugin-architecture extraction, see CLAUDE.md "Ui changes
// sketch"): AMS/main-dashboard/preview/camera widget construction and
// rendering, moved wholesale out of Ui (main/src/ui.cpp) into PrinterPlugin.
// Mirrors the existing printer_plugin_portal.cpp split-by-responsibility
// convention — this TU owns everything printer-content/LVGL-widget related;
// printer_plugin.cpp keeps client lifecycle/tick/network logic;
// printer_plugin_portal.cpp keeps Web Config routes.
//
// This is a straight code-motion from ui.cpp with mechanical adaptation
// (Ui's private members become PrinterPlugin's own; chrome-only concerns —
// active page, pager scroll, dimming — are reached through the small set of
// generic hooks Ui now exposes: is_plugin_page_active()/is_plugin_page_visible(),
// register_page_visibility_callback()/register_page_settle_callback()/
// register_page_tap_callback(), set_plugin_page_available(),
// set_battery_overlay_text(), set_active_page_by_plugin(),
// request_chamber_light_toggle(), request_print_command()). Logic itself is
// unchanged from what Ui did before this phase.

#include "infohub/plugins/printer/printer_plugin.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "png.h"
#include "sdkconfig.h"
#include "infohub/board_config.hpp"
#include "infohub/ui.hpp"
#include "infohub/ui_toolkit.hpp"

extern "C" {
extern const lv_image_dsc_t bambuicon_small;
extern const lv_font_t dosis_20;
extern const lv_font_t dosis_32;
extern const lv_font_t dosis_40;
extern const lv_font_t lv_font_montserrat_20;
extern const lv_font_t mdi_30;
extern const lv_font_t mdi_40;
}

namespace infohub {

namespace {

constexpr char kTag[] = "infohub.printer.ui";
constexpr size_t kImagePersistentReserveBytes = 20U * 1024U;
// Page-2 preview cover layout. The compact layout (240 px cover, lifted up
// to make room for two print-control buttons below) is only used when the
// experimental print-control buttons are compiled in. The default build keeps
// the original full-size cover layout (320 px, centered) to preserve the
// look-and-feel of releases prior to v1.6-rc1.
#if CONFIG_INFOHUB_EXPERIMENTAL_PRINT_CONTROL
constexpr int kPage2PreviewSize = 240;
constexpr int kPage2PreviewYOffset = -90;
constexpr int kPage2NoteWithImageY = 56;
constexpr int kPage2SubnoteWithImageY = 60;
#else
constexpr int kPage2PreviewSize = 320;
constexpr int kPage2PreviewYOffset = -12;
constexpr int kPage2NoteWithImageY = 138;
constexpr int kPage2SubnoteWithImageY = 188;
#endif
constexpr int kPage3CameraWidth = 400;
constexpr int kPage3CameraHeight = 224;
constexpr int kPage3CameraYOffset = 0;
constexpr int kPage3NoteWithImageY = 150;
constexpr int kPage3SubnoteWithImageY = 182;
// Status text shown above the camera JPEG when an image is loaded.
// Image is 224 high and centered at y=0, so its top is ~y=-112; the
// status sits above with comfortable breathing room.
constexpr int kPage3StatusAboveImageY = -138;
constexpr int kAuxTempRowY = 28;
constexpr int kRemainingRowY = 172;
constexpr char kDegreeC[] = "\xC2\xB0""C";
constexpr char kMdiClock[] = "\xF3\xB1\x91\x8E";
constexpr char kMdiNozzle[] = "\xF3\xB0\xB9\x9B";
constexpr char kMdiBed[] = "\xF3\xB1\xA1\x9B";
constexpr char kMdiSpool[] = "\xF3\xB1\x82\x97";  // U+F1097 mdi-spool
// Battery MDI glyphs/text-formatting moved to ui.cpp (core, PmuManager-driven
// now — see Ui::update_battery_overlay()), not printer-specific.

const char* ram_region(const void* ptr) {
  if (ptr == nullptr) {
    return "null";
  }
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  return esp_ptr_external_ram(ptr) ? "psram" : "internal";
#else
  return "internal";
#endif
}

size_t allocated_size(const void* ptr) {
  return ptr == nullptr ? 0U : heap_caps_get_allocated_size(const_cast<void*>(ptr));
}

void log_heap_diag(const char* context) {
  ESP_LOGV(kTag,
           "[RAM] %s: int_free=%u int_largest=%u dma_free=%u dma_largest=%u "
           "psram_free=%u psram_largest=%u",
           context != nullptr ? context : "-",
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void log_blob_diag(const char* context, const std::shared_ptr<std::vector<uint8_t>>& blob) {
  const void* data = blob && !blob->empty() ? blob->data() : nullptr;
  ESP_LOGV(kTag,
           "[RAM] %s: size=%u capacity=%u alloc=%u ram=%s data=%p",
           context != nullptr ? context : "-",
           static_cast<unsigned>(blob ? blob->size() : 0U),
           static_cast<unsigned>(blob ? blob->capacity() : 0U),
           static_cast<unsigned>(allocated_size(data)),
           ram_region(data), data);
  log_heap_diag(context);
}

// Draw callback for the green/red triangle slot indicator.
// The triangle is an upward-pointing isosceles triangle centered in the object.
// The bg_color style controls the triangle color; the bg_opa style controls
// the triangle opacity (used for the pulse animation when an HMS error is
// flagged on the slot).
void ams_arrow_draw_cb(lv_event_t* e) {
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  lv_layer_t* layer = lv_event_get_layer(e);
  if (obj == nullptr || layer == nullptr) return;

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t w = coords.x2 - coords.x1;
  const int32_t cx = coords.x1 + w / 2;

  const lv_color_t color = lv_obj_get_style_bg_color(obj, LV_PART_MAIN);

  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);
  dsc.p[0] = {cx, coords.y1};               // top center (tip)
  dsc.p[1] = {coords.x1, coords.y2};        // bottom left
  dsc.p[2] = {coords.x1 + w, coords.y2};   // bottom right
  dsc.color = color;
  dsc.opa = LV_OPA_COVER;
  lv_draw_triangle(layer, &dsc);
}

// Draw callback that overlays a rhombus (diamond stripe) pattern on the
// AMS pill rect when an HMS/Error code is mapped to that slot. Activated by
// setting the user-data flag pointer to a `bool*` that evaluates to true.
void ams_pill_error_overlay_cb(lv_event_t* e) {
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  lv_layer_t* layer = lv_event_get_layer(e);
  if (obj == nullptr || layer == nullptr) return;
  const bool* flag = static_cast<const bool*>(lv_event_get_user_data(e));
  if (flag == nullptr || !*flag) return;

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t x1 = coords.x1;
  const int32_t y1 = coords.y1;
  const int32_t x2 = coords.x2;
  const int32_t y2 = coords.y2;
  const int32_t w = x2 - x1;
  const int32_t h = y2 - y1;

  // Draw a diagonal stripe pattern (two crossing line sets = rhombus grid).
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = lv_color_hex(0xFFFFFF);
  line.opa = LV_OPA_50;
  line.width = 2;
  const int32_t spacing = 12;
  // Forward diagonals
  for (int32_t k = -h; k < w; k += spacing) {
    int32_t a_x = x1 + k;
    int32_t a_y = y1;
    int32_t b_x = x1 + k + h;
    int32_t b_y = y2;
    if (a_x < x1) { a_y += (x1 - a_x); a_x = x1; }
    if (b_x > x2) { b_y -= (b_x - x2); b_x = x2; }
    line.p1 = {a_x, a_y};
    line.p2 = {b_x, b_y};
    lv_draw_line(layer, &line);
  }
  // Backward diagonals
  for (int32_t k = 0; k < w + h; k += spacing) {
    int32_t a_x = x1 + k;
    int32_t a_y = y1;
    int32_t b_x = x1 + k - h;
    int32_t b_y = y2;
    if (a_x > x2) { a_y += (a_x - x2); a_x = x2; }
    if (b_x < x1) { b_y -= (x1 - b_x); b_x = x1; }
    line.p1 = {a_x, a_y};
    line.p2 = {b_x, b_y};
    lv_draw_line(layer, &line);
  }
}

void set_clickable(lv_obj_t* obj, bool clickable) {
  if (obj == nullptr) {
    return;
  }

  const bool currently_clickable = lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  if (currently_clickable == clickable) {
    return;
  }

  if (clickable) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  }
}

std::string optional_temperature_text(const char* label, float temperature_c, bool known = false) {
  if (label == nullptr || (!known && temperature_c <= 0.0f)) {
    return {};
  }

  char buffer[40] = {};
  std::snprintf(buffer, sizeof(buffer), "%s %.0f%s", label, temperature_c, kDegreeC);
  return buffer;
}

bool decode_preview_png(const std::shared_ptr<std::vector<uint8_t>>& encoded_blob,
                        std::shared_ptr<std::vector<uint8_t>>* decoded_blob,
                        lv_image_dsc_t* image_dsc) {
  if (!encoded_blob || encoded_blob->empty() || decoded_blob == nullptr || image_dsc == nullptr) {
    return false;
  }

  png_image image;
  std::memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;

  if (!png_image_begin_read_from_memory(&image, encoded_blob->data(), encoded_blob->size())) {
    ESP_LOGW(kTag, "Preview PNG header decode failed");
    return false;
  }

  image.format = PNG_FORMAT_BGRA;
  const size_t row_stride = static_cast<size_t>(image.width) * 4U;
  const size_t decoded_size = PNG_IMAGE_SIZE(image);
  auto raw = std::make_shared<std::vector<uint8_t>>();
  raw->reserve(std::max(decoded_size, kImagePersistentReserveBytes));
  raw->resize(decoded_size);

  const bool ok = png_image_finish_read(&image, nullptr, raw->data(),
                                        static_cast<png_int_32>(row_stride), nullptr) != 0;
  if (!ok) {
    ESP_LOGW(kTag, "Preview PNG decode failed: %s", image.message);
    png_image_free(&image);
    return false;
  }

  png_image_free(&image);

  std::memset(image_dsc, 0, sizeof(*image_dsc));
  image_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  image_dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
  image_dsc->header.flags = 0;
  image_dsc->header.w = static_cast<uint16_t>(image.width);
  image_dsc->header.h = static_cast<uint16_t>(image.height);
  image_dsc->header.stride = static_cast<uint16_t>(row_stride);
  image_dsc->data_size = static_cast<uint32_t>(raw->size());
  image_dsc->data = raw->data();
  log_blob_diag("ui preview encoded png", encoded_blob);
  log_blob_diag("ui preview decoded raw", raw);
  *decoded_blob = std::move(raw);
  return true;
}

bool configure_camera_rgb565(const std::shared_ptr<std::vector<uint8_t>>& decoded_blob,
                             uint16_t width, uint16_t height, lv_image_dsc_t* image_dsc) {
  if (!decoded_blob || decoded_blob->empty() || image_dsc == nullptr || width == 0U || height == 0U) {
    return false;
  }

  std::memset(image_dsc, 0, sizeof(*image_dsc));
  image_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
  image_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
  image_dsc->header.flags = 0;
  image_dsc->header.w = width;
  image_dsc->header.h = height;
  image_dsc->header.stride = static_cast<uint16_t>(width * sizeof(uint16_t));
  image_dsc->data_size = static_cast<uint32_t>(decoded_blob->size());
  image_dsc->data = decoded_blob->data();
  return true;
}

uint32_t hash_mix(uint32_t hash, uint32_t value) {
  hash ^= value + 0x9E3779B9U + (hash << 6) + (hash >> 2);
  return hash;
}

uint32_t hash_string(uint32_t hash, const std::string& value) {
  for (char ch : value) {
    hash = hash_mix(hash, static_cast<uint8_t>(ch));
  }
  return hash_mix(hash, static_cast<uint32_t>(value.size()));
}

uint32_t ams_ui_signature(const PrinterSnapshot& snapshot) {
  uint32_t hash = 2166136261U;
  hash = hash_mix(hash, snapshot.tray_now < 0 ? 0xFFFFFFFFU : static_cast<uint32_t>(snapshot.tray_now));
  hash = hash_mix(hash, snapshot.tray_tar < 0 ? 0xFFFFFFFFU : static_cast<uint32_t>(snapshot.tray_tar));
  if (!snapshot.ams) {
    return hash_mix(hash, 0);
  }

  hash = hash_mix(hash, snapshot.ams->count);
  if (snapshot.ams->count == 0) {
    return hash;
  }

  for (uint8_t u = 0; u < snapshot.ams->count && u < kMaxAmsUnits; ++u) {
    const AmsUnitInfo& unit = snapshot.ams->units[u];
    hash = hash_mix(hash, unit.humidity_pct < 0 ? 0xFFFFFFFFU : static_cast<uint32_t>(unit.humidity_pct));
    hash = hash_mix(hash, static_cast<uint32_t>(std::lround(unit.temperature_c * 10.0f)));
    for (int i = 0; i < kMaxAmsTrays; ++i) {
      const AmsTrayInfo& tray = unit.trays[i];
      hash = hash_mix(hash, tray.present ? 1U : 0U);
      hash = hash_mix(hash, tray.active ? 1U : 0U);
      hash = hash_mix(hash, tray.color_rgba);
      hash = hash_mix(hash, tray.remain_pct < 0 ? 0xFFFFFFFFU : static_cast<uint32_t>(tray.remain_pct));
      hash = hash_string(hash, tray.material_type);
    }
  }

  const AmsTrayInfo& ext = snapshot.ams->external_spool;
  hash = hash_mix(hash, ext.present ? 1U : 0U);
  hash = hash_mix(hash, ext.active ? 1U : 0U);
  hash = hash_mix(hash, ext.color_rgba);
  hash = hash_mix(hash, ext.remain_pct < 0 ? 0xFFFFFFFFU : static_cast<uint32_t>(ext.remain_pct));
  hash = hash_string(hash, ext.material_type);

  // Fold HMS codes (subset relevant to AMS) into the signature so the error
  // overlay updates when codes appear/disappear.
  for (uint64_t code : snapshot.hms_codes) {
    const uint32_t attr = static_cast<uint32_t>(code >> 32);
    if (((attr >> 24) & 0xFFU) == 0x07U) {
      hash = hash_mix(hash, static_cast<uint32_t>(code & 0xFFFFFFFFU));
      hash = hash_mix(hash, attr);
    }
  }
  return hash;
}

std::string lifecycle_label(const PrinterSnapshot& snapshot) {
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return "setup";
  }
  if (snapshot.connection == PrinterConnectionState::kConnecting && !snapshot.wifi_connected) {
    return "syncing";
  }
  // Let filament ui_status (loading/unloading) take priority over has_error,
  // because Bambu sends user-prompt error codes during filament changes.
  if (snapshot.ui_status == "loading" || snapshot.ui_status == "unloading") {
    return snapshot.ui_status;
  }
  if (snapshot.connection == PrinterConnectionState::kError || snapshot.has_error ||
      snapshot.lifecycle == PrintLifecycleState::kError) {
    return "failed";
  }
  if (snapshot.connection == PrinterConnectionState::kBooting) {
    return "booting";
  }
  if (snapshot.connection == PrinterConnectionState::kReadyForLanConnect) {
    return "ready";
  }
  if (!snapshot.ui_status.empty()) {
    return snapshot.ui_status;
  }

  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPreparing:
      return "preparing";
    case PrintLifecycleState::kPrinting:
      return "printing";
    case PrintLifecycleState::kPaused:
      return "paused";
    case PrintLifecycleState::kFinished:
      return "done";
    case PrintLifecycleState::kIdle:
      return "idle";
    case PrintLifecycleState::kUnknown:
    default:
      return snapshot.wifi_connected ? "waiting..." : "offline";
  }
}

std::string setup_access_text(const PrinterSnapshot& snapshot) {
  std::string text;
  if (!snapshot.setup_ap_ssid.empty()) {
    text = "AP: " + snapshot.setup_ap_ssid;
  }
  if (!snapshot.setup_ap_password.empty()) {
    if (!text.empty()) {
      text += "\n";
    }
    text += "PW: " + snapshot.setup_ap_password;
  }

  const std::string ip = snapshot.setup_ap_ip.empty() ? "192.168.4.1" : snapshot.setup_ap_ip;
  if (!ip.empty()) {
    if (!text.empty()) {
      text += "\n";
    }
    text += "Open " + ip;
  }

  return text;
}

std::string station_portal_text(const PrinterSnapshot& snapshot) {
  if (!snapshot.wifi_ip.empty()) {
    return "Open " + snapshot.wifi_ip;
  }
  return "Open the portal on your Wi-Fi IP";
}

std::string detail_text(const PrinterSnapshot& snapshot) {
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    if (snapshot.setup_ap_active) {
      return setup_access_text(snapshot);
    }
    if (snapshot.wifi_connected) {
      return station_portal_text(snapshot);
    }
    return {};
  }
  if (!snapshot.wifi_connected) {
    if (snapshot.setup_ap_active) {
      return setup_access_text(snapshot);
    }
    return {};
  }
  if (snapshot.has_error && !snapshot.detail.empty()) {
    return snapshot.detail;
  }
  // HMS warnings (codes present but `has_error` not flagged) should also win
  // over the job name so the user sees the full TSV-resolved message — e.g.
  // "MQTT Command verification failed..." after using a print-control button —
  // instead of just the filename of the running job.
  if ((!snapshot.hms_codes.empty() || snapshot.hms_alert_count > 0) &&
      !snapshot.detail.empty() && snapshot.detail != snapshot.stage) {
    return snapshot.detail;
  }
  if (!snapshot.job_name.empty() &&
      (snapshot.lifecycle == PrintLifecycleState::kPreparing ||
       snapshot.lifecycle == PrintLifecycleState::kPrinting ||
       snapshot.lifecycle == PrintLifecycleState::kPaused)) {
    return snapshot.job_name;
  }
  if (!snapshot.detail.empty() && snapshot.detail != snapshot.stage &&
      snapshot.detail != "Connected to local Bambu MQTT" &&
      snapshot.detail != "Printer version info received" &&
      snapshot.detail != "Status payload received") {
    return snapshot.detail;
  }
  return {};
}

std::string layer_text(const PrinterSnapshot& snapshot) {
  char buffer[32] = {};
  if (snapshot.total_layers > 0) {
    std::snprintf(buffer, sizeof(buffer), "Layer: %u / %u", snapshot.current_layer,
                  snapshot.total_layers);
  } else if (snapshot.current_layer > 0) {
    std::snprintf(buffer, sizeof(buffer), "Layer: %u / --", snapshot.current_layer);
  } else {
    std::snprintf(buffer, sizeof(buffer), "Layer: -- / --");
  }
  return std::string(buffer);
}

// Format the slicer filament weight estimate as a compact "14g" / "1.45kg"
// string. Returned empty when no estimate is available so the caller can
// hide the dedicated icon + value labels in the layer row.
// NOTE: The tilde '~' character is intentionally omitted — the embedded
// dosis_32 font does not include U+007E (tilde) so it would render as a
// blank tofu rectangle.
std::string filament_estimate_text(const PrinterSnapshot& snapshot) {
  if (snapshot.estimated_filament_weight_g <= 0.0f) {
    return {};
  }
  char fila[24] = {};
  if (snapshot.estimated_filament_weight_g >= 1000.0f) {
    std::snprintf(fila, sizeof(fila), "%.2fkg",
                  snapshot.estimated_filament_weight_g / 1000.0f);
  } else {
    std::snprintf(fila, sizeof(fila), "%.0fg",
                  snapshot.estimated_filament_weight_g);
  }
  return std::string(fila);
}

std::string remaining_text(const PrinterSnapshot& snapshot) {
  if (snapshot.ui_status == "done" || snapshot.lifecycle == PrintLifecycleState::kFinished) {
    return "Done";
  }
  if (snapshot.remaining_seconds == 0) {
    return "--m";
  }

  const uint32_t minutes_total = snapshot.remaining_seconds / 60U;
  const uint32_t hours = minutes_total / 60U;
  const uint32_t minutes = minutes_total % 60U;
  char buffer[24] = {};
  if (hours > 0U) {
    std::snprintf(buffer, sizeof(buffer), "%uh %um", static_cast<unsigned int>(hours),
                  static_cast<unsigned int>(minutes));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%um", static_cast<unsigned int>(minutes));
  }
  return buffer;
}

// Wall-clock predicted finish time as "HH:MM". Falls back to the regular
// remaining-duration text when SNTP has not synced yet (year < 2024) or
// when no remaining time is reported.
std::string eta_text(const PrinterSnapshot& snapshot) {
  if (snapshot.ui_status == "done" || snapshot.lifecycle == PrintLifecycleState::kFinished) {
    return "Done";
  }
  if (snapshot.remaining_seconds == 0) {
    return "--:--";
  }
  const std::time_t now = std::time(nullptr);
  if (now < 1700000000) {
    // Wall clock not yet synced — fall back to duration so the row stays useful.
    return remaining_text(snapshot);
  }
  const std::time_t finish = now + static_cast<std::time_t>(snapshot.remaining_seconds);
  std::tm local{};
  if (localtime_r(&finish, &local) == nullptr) {
    return remaining_text(snapshot);
  }
  char buffer[8] = {};
  std::snprintf(buffer, sizeof(buffer), "%02d:%02d", local.tm_hour, local.tm_min);
  return buffer;
}

std::string preview_note_text(const PrinterSnapshot& snapshot) {
  if (snapshot.preview_blob && !snapshot.preview_blob->empty()) {
    return {};  // note hidden when cover is loaded — title takes over
  }
  if (!snapshot.preview_url.empty()) {
    return "Loading cloud cover";
  }
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return "Set up printer";
  }
  if (!snapshot.wifi_connected) {
    return "Printer offline";
  }
  if (!snapshot.cloud_detail.empty() && !snapshot.cloud_connected) {
    return "Connecting to cloud";
  }

  switch (snapshot.lifecycle) {
    case PrintLifecycleState::kPreparing:
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPaused:
      return "Preparing cover";
    case PrintLifecycleState::kFinished:
      return "Last print done";
    case PrintLifecycleState::kError:
      return "Cover unavailable";
    case PrintLifecycleState::kIdle:
    case PrintLifecycleState::kUnknown:
    default:
      return "No active print";
  }
}

std::string preview_subnote_text(const PrinterSnapshot& snapshot) {
  if (snapshot.print_active && !snapshot.job_name.empty()) {
    return snapshot.job_name;
  }
  if (!snapshot.preview_title.empty()) {
    return snapshot.preview_title;
  }
  if (!snapshot.job_name.empty()) {
    return snapshot.job_name;
  }
  if (!snapshot.cloud_detail.empty()) {
    return snapshot.cloud_detail;
  }
  return snapshot.preview_hint;
}

std::string camera_note_text(const PrinterSnapshot& snapshot) {
  if (snapshot.camera_blob && !snapshot.camera_blob->empty()) {
    // The camera header shares its position with the global battery overlay.
    // Prefer the battery state whenever a battery is installed (including
    // charging); on USB-only power the header instead shows the print status.
    return (snapshot.battery_present || snapshot.charging) ? std::string{} : lifecycle_label(snapshot);
  }
  if (snapshot.connection == PrinterConnectionState::kWaitingForCredentials) {
    return "Set up printer";
  }
  if (!snapshot.wifi_connected) {
    return "Camera offline";
  }
  if (!snapshot.camera_detail.empty()) {
    return snapshot.camera_detail;
  }
  return "Tap for new image";
}

std::string camera_subnote_text(const PrinterSnapshot& snapshot) {
  if (snapshot.camera_blob && !snapshot.camera_blob->empty()) {
    // Image is loaded: show layer progress below the snapshot so the cam page
    // carries the print-in-progress info that the dashboard usually owns.
    if (snapshot.total_layers > 0 || snapshot.current_layer > 0) {
      return layer_text(snapshot);
    }
    if (!snapshot.job_name.empty()) {
      return snapshot.job_name;
    }
    return {};
  }
  if (!snapshot.job_name.empty()) {
    return snapshot.job_name;
  }
  return "Auto-refresh every 2s";
}

bool should_show_logo(const PrinterSnapshot& snapshot) {
  if (!snapshot.wifi_connected) {
    return false;
  }

  switch (snapshot.connection) {
    case PrinterConnectionState::kBooting:
    case PrinterConnectionState::kWaitingForCredentials:
    case PrinterConnectionState::kConnecting:
      return false;
    case PrinterConnectionState::kReadyForLanConnect:
    case PrinterConnectionState::kOnline:
    case PrinterConnectionState::kError:
    default:
      return true;
  }
}

}  // namespace

// --- Page construction -------------------------------------------------

void PrinterPlugin::build_main_dashboard_page(lv_obj_t* parent) {
  badge_slot_ = lv_obj_create(parent);
  make_transparent(badge_slot_);
  lv_obj_set_size(badge_slot_, 86, 86);
  lv_obj_align(badge_slot_, LV_ALIGN_CENTER, 0, -7);
  lv_obj_clear_flag(badge_slot_, LV_OBJ_FLAG_SCROLLABLE);

  logo_badge_ = lv_obj_create(badge_slot_);
  lv_obj_set_size(logo_badge_, 120, 120);
  lv_obj_set_style_radius(logo_badge_, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(logo_badge_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_opa(logo_badge_, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(logo_badge_, 0, 0);
  lv_obj_center(logo_badge_);
  lv_obj_clear_flag(logo_badge_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(logo_badge_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(logo_badge_);
  lv_obj_add_event_cb(logo_badge_, &PrinterPlugin::logo_event_cb, LV_EVENT_CLICKED, this);

  logo_image_ = lv_image_create(logo_badge_);
  lv_image_set_src(logo_image_, &bambuicon_small);
  lv_image_set_scale(logo_image_, 183);
  lv_image_set_antialias(logo_image_, true);
  lv_obj_set_style_image_recolor_opa(logo_image_, LV_OPA_TRANSP, 0);
  lv_obj_center(logo_image_);
  lv_obj_clear_flag(logo_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(logo_image_, LV_OBJ_FLAG_SCROLLABLE);

  status_label_ = lv_label_create(parent);
  set_label_text_if_changed(status_label_, "waiting...");
  lv_obj_set_style_text_font(status_label_, &dosis_32, 0);
  lv_obj_set_style_text_color(status_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, -86);

  detail_label_ = lv_label_create(parent);
  set_label_text_if_changed(detail_label_, "Waiting for printer data");
  lv_label_set_long_mode(detail_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(detail_label_, 320);
  lv_obj_set_style_text_align(detail_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(detail_label_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(detail_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(detail_label_, LV_ALIGN_CENTER, 0, 114);

  layer_row_ = lv_obj_create(parent);
  make_transparent(layer_row_);
  lv_obj_set_size(layer_row_, 360, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(layer_row_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(layer_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(layer_row_, LV_ALIGN_CENTER, 0, 70);
  lv_obj_clear_flag(layer_row_, LV_OBJ_FLAG_SCROLLABLE);

  layer_label_ = lv_label_create(layer_row_);
  set_label_text_if_changed(layer_label_, "Layer: -- / --");
  lv_obj_set_style_text_font(layer_label_, &dosis_32, 0);
  lv_obj_set_style_text_color(layer_label_, lv_color_hex(0xDDDDDD), 0);

  // Filament estimate (icon + grams) appears on the right of the layer row
  // when the cloud history exposes a slicer weight estimate. Both children
  // are hidden by default and only shown by apply_snapshot_locked() when an
  // estimate is available so the row stays cleanly centered around the
  // layer counter when no estimate is known.
  filament_icon_label_ = lv_label_create(layer_row_);
  set_label_text_if_changed(filament_icon_label_, kMdiSpool);
  lv_obj_set_style_text_font(filament_icon_label_, &mdi_40, 0);
  lv_obj_set_style_text_color(filament_icon_label_, lv_color_hex(0xC9A227), 0);
  lv_obj_set_style_pad_left(filament_icon_label_, 14, 0);
  lv_obj_set_style_pad_right(filament_icon_label_, 4, 0);
  lv_obj_add_flag(filament_icon_label_, LV_OBJ_FLAG_HIDDEN);

  filament_value_label_ = lv_label_create(layer_row_);
  set_label_text_if_changed(filament_value_label_, "");
  lv_obj_set_style_text_font(filament_value_label_, &dosis_32, 0);
  lv_obj_set_style_text_color(filament_value_label_, lv_color_hex(0xDDDDDD), 0);
  // Add a left pad so the value sits a sensible distance from the layer
  // counter now that the spool icon is hidden.
  lv_obj_set_style_pad_left(filament_value_label_, 14, 0);
  lv_obj_add_flag(filament_value_label_, LV_OBJ_FLAG_HIDDEN);

  nozzle_prefix_label_ = lv_label_create(parent);
  set_label_text_if_changed(nozzle_prefix_label_, kMdiNozzle);
  lv_obj_set_style_text_font(nozzle_prefix_label_, &mdi_40, 0);
  lv_obj_set_style_text_color(nozzle_prefix_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(nozzle_prefix_label_, LV_ALIGN_CENTER, -182, -10);

  nozzle_value_label_ = lv_label_create(parent);
  set_label_text_if_changed(nozzle_value_label_, "--°C");
  lv_obj_set_style_text_font(nozzle_value_label_, &dosis_32, 0);
  lv_obj_set_style_text_color(nozzle_value_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(nozzle_value_label_, LV_ALIGN_CENTER, -132, -10);
  set_label_text_if_changed(nozzle_value_label_, std::string("--") + kDegreeC);

  nozzle_aux_label_ = lv_label_create(parent);
  set_label_text_if_changed(nozzle_aux_label_, "");
  lv_obj_set_width(nozzle_aux_label_, 170);
  lv_label_set_long_mode(nozzle_aux_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(nozzle_aux_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(nozzle_aux_label_, &dosis_20, 0);
  lv_obj_set_style_text_color(nozzle_aux_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(nozzle_aux_label_, LV_ALIGN_CENTER, -132, kAuxTempRowY);
  lv_obj_add_flag(nozzle_aux_label_, LV_OBJ_FLAG_HIDDEN);

  bed_prefix_label_ = lv_label_create(parent);
  set_label_text_if_changed(bed_prefix_label_, kMdiBed);
  lv_obj_set_style_text_font(bed_prefix_label_, &mdi_40, 0);
  lv_obj_set_style_text_color(bed_prefix_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(bed_prefix_label_, LV_ALIGN_CENTER, 182, -10);

  bed_value_label_ = lv_label_create(parent);
  set_label_text_if_changed(bed_value_label_, "--°C");
  lv_obj_set_style_text_font(bed_value_label_, &dosis_32, 0);
  lv_obj_set_style_text_color(bed_value_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_width(bed_value_label_, 96);
  lv_obj_set_style_text_align(bed_value_label_, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(bed_value_label_, LV_ALIGN_CENTER, 108, -10);
  set_label_text_if_changed(bed_value_label_, std::string("--") + kDegreeC);

  bed_aux_label_ = lv_label_create(parent);
  set_label_text_if_changed(bed_aux_label_, "");
  lv_obj_set_width(bed_aux_label_, 170);
  lv_label_set_long_mode(bed_aux_label_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(bed_aux_label_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(bed_aux_label_, &dosis_20, 0);
  lv_obj_set_style_text_color(bed_aux_label_, lv_color_hex(0x94A3B8), 0);
  lv_obj_align(bed_aux_label_, LV_ALIGN_CENTER, 132, kAuxTempRowY);
  lv_obj_add_flag(bed_aux_label_, LV_OBJ_FLAG_HIDDEN);

  remaining_row_ = lv_obj_create(parent);
  make_transparent(remaining_row_);
  lv_obj_set_size(remaining_row_, 280, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(remaining_row_, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(remaining_row_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_align(remaining_row_, LV_ALIGN_CENTER, 0, kRemainingRowY);
  lv_obj_clear_flag(remaining_row_, LV_OBJ_FLAG_SCROLLABLE);
  // Tap to toggle between remaining duration and predicted finish time.
  lv_obj_add_flag(remaining_row_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(remaining_row_, &PrinterPlugin::remaining_row_event_cb, LV_EVENT_CLICKED, this);

  remaining_prefix_label_ = lv_label_create(remaining_row_);
  set_label_text_if_changed(remaining_prefix_label_, kMdiClock);
  lv_obj_set_style_text_font(remaining_prefix_label_, &mdi_40, 0);
  lv_obj_set_style_text_color(remaining_prefix_label_, lv_color_hex(0x87CEEB), 0);
  lv_obj_set_style_pad_right(remaining_prefix_label_, 8, 0);

  remaining_label_ = lv_label_create(remaining_row_);
  set_label_text_if_changed(remaining_label_, "--m");
  lv_obj_set_style_text_font(remaining_label_, &dosis_40, 0);
  lv_obj_set_style_text_color(remaining_label_, lv_color_hex(0x87CEEB), 0);
}

void PrinterPlugin::build_preview_page(lv_obj_t* parent) {
  page2_image_ = lv_image_create(parent);
  lv_obj_set_size(page2_image_, kPage2PreviewSize, kPage2PreviewSize);
  lv_image_set_inner_align(page2_image_, LV_IMAGE_ALIGN_CONTAIN);
  lv_obj_align(page2_image_, LV_ALIGN_CENTER, 0, kPage2PreviewYOffset);
  lv_obj_add_flag(page2_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(page2_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(page2_image_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(page2_image_);

  page2_note_ = lv_label_create(parent);
  set_label_text_if_changed(page2_note_, "No cover image yet");
  lv_obj_set_width(page2_note_, 280);
  lv_label_set_long_mode(page2_note_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page2_note_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page2_note_, &dosis_20, 0);
  lv_obj_set_style_text_color(page2_note_, lv_color_hex(0x888888), 0);
  lv_obj_align(page2_note_, LV_ALIGN_CENTER, 0, -14);
  enable_touch_bubble(page2_note_);

  page2_subnote_ = lv_label_create(parent);
  set_label_text_if_changed(page2_subnote_, "");
  lv_obj_set_width(page2_subnote_, 320);
  lv_label_set_long_mode(page2_subnote_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page2_subnote_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page2_subnote_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(page2_subnote_, lv_color_hex(0x888888), 0);
  lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, 18);
  lv_obj_add_flag(page2_subnote_, LV_OBJ_FLAG_HIDDEN);
  enable_touch_bubble(page2_subnote_);

  // Print-control buttons (pause/resume + stop). Only compiled in when the
  // experimental print-control feature is enabled via Kconfig. See the help
  // text of CONFIG_INFOHUB_EXPERIMENTAL_PRINT_CONTROL for background on
  // why this is opt-in. When disabled, page2_pause_button_/page2_stop_button_
  // stay nullptr and update_print_buttons_locked() becomes a no-op.
#if CONFIG_INFOHUB_EXPERIMENTAL_PRINT_CONTROL
  auto style_print_button = [](lv_obj_t* button, lv_color_t bg) {
    lv_obj_set_size(button, 96, 64);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_set_style_bg_color(button, bg, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_90, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(button, LV_OPA_30, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  };

  page2_pause_button_ = lv_obj_create(parent);
  style_print_button(page2_pause_button_, lv_color_hex(0x1F6F4A));
  lv_obj_align(page2_pause_button_, LV_ALIGN_CENTER, -64, 110);
  lv_obj_add_event_cb(page2_pause_button_, &PrinterPlugin::pause_button_event_cb, LV_EVENT_CLICKED, this);
  page2_pause_button_label_ = lv_label_create(page2_pause_button_);
  lv_obj_center(page2_pause_button_label_);
  lv_obj_set_style_text_color(page2_pause_button_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(page2_pause_button_label_, &lv_font_montserrat_20, 0);
  set_label_text_if_changed(page2_pause_button_label_, LV_SYMBOL_PAUSE);

  page2_stop_button_ = lv_obj_create(parent);
  style_print_button(page2_stop_button_, lv_color_hex(0x8E2A2A));
  lv_obj_align(page2_stop_button_, LV_ALIGN_CENTER, 64, 110);
  // Stop requires a long press (~1.5 s default) to commit — guards against
  // accidental cancellation. A short tap is intentionally a no-op.
  lv_obj_add_event_cb(page2_stop_button_, &PrinterPlugin::stop_button_event_cb, LV_EVENT_LONG_PRESSED, this);
  page2_stop_button_label_ = lv_label_create(page2_stop_button_);
  lv_obj_center(page2_stop_button_label_);
  lv_obj_set_style_text_color(page2_stop_button_label_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(page2_stop_button_label_, &lv_font_montserrat_20, 0);
  set_label_text_if_changed(page2_stop_button_label_, LV_SYMBOL_STOP);
#endif  // CONFIG_INFOHUB_EXPERIMENTAL_PRINT_CONTROL
}

void PrinterPlugin::build_camera_page(lv_obj_t* parent) {
  page3_image_ = lv_image_create(parent);
  lv_obj_set_size(page3_image_, board::kDisplayWidth, kPage3CameraHeight);
  lv_image_set_inner_align(page3_image_, LV_IMAGE_ALIGN_CENTER);
  lv_obj_align(page3_image_, LV_ALIGN_CENTER, 0, kPage3CameraYOffset);
  lv_obj_add_flag(page3_image_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(page3_image_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(page3_image_, LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(page3_image_);

  page3_note_ = lv_label_create(parent);
  set_label_text_if_changed(page3_note_, "Tap for new image");
  lv_obj_set_width(page3_note_, 320);
  lv_label_set_long_mode(page3_note_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page3_note_, LV_TEXT_ALIGN_CENTER, 0);
  // Match the main-page status label (dosis32 / white) so the lifecycle status
  // line on the camera page reads consistently across pages.
  lv_obj_set_style_text_font(page3_note_, &dosis_32, 0);
  lv_obj_set_style_text_color(page3_note_, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(page3_note_, LV_ALIGN_CENTER, 0, 0);
  enable_touch_bubble(page3_note_);

  page3_subnote_ = lv_label_create(parent);
  set_label_text_if_changed(page3_subnote_, "Auto-refresh every 2s");
  lv_obj_set_width(page3_subnote_, 320);
  lv_label_set_long_mode(page3_subnote_, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(page3_subnote_, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(page3_subnote_, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(page3_subnote_, lv_color_hex(0x888888), 0);
  lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, 28);
  lv_obj_add_flag(page3_subnote_, LV_OBJ_FLAG_HIDDEN);
  enable_touch_bubble(page3_subnote_);
}

void PrinterPlugin::build_ams_page(int unit_idx) {
  if (unit_idx < 0 || unit_idx >= kMaxAmsUnits) return;
  lv_obj_t* page = ams_pages_[unit_idx];
  if (page == nullptr) return;

  // Unit header label ("AMS 1..4") — created always, hidden by default.
  ams_unit_label_[unit_idx] = lv_label_create(page);
  char unit_label_buf[16];
  std::snprintf(unit_label_buf, sizeof(unit_label_buf), "AMS %d", unit_idx + 1);
  set_label_text_if_changed(ams_unit_label_[unit_idx], unit_label_buf);
  lv_obj_set_style_text_font(ams_unit_label_[unit_idx], &dosis_32, 0);
  lv_obj_set_style_text_color(ams_unit_label_[unit_idx], lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_align(ams_unit_label_[unit_idx], LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ams_unit_label_[unit_idx], LV_ALIGN_TOP_MID, 0, 60);
  lv_obj_add_flag(ams_unit_label_[unit_idx], LV_OBJ_FLAG_HIDDEN);

  // Gray shelf background (behind upper half of pills)
  ams_shelf_[unit_idx] = lv_obj_create(page);
  lv_obj_set_size(ams_shelf_[unit_idx], 359, 110);
  lv_obj_set_style_radius(ams_shelf_[unit_idx], 20, 0);
  lv_obj_set_style_bg_color(ams_shelf_[unit_idx], lv_color_hex(0x565656), 0);
  lv_obj_set_style_bg_opa(ams_shelf_[unit_idx], LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ams_shelf_[unit_idx], 2, 0);
  lv_obj_set_style_border_color(ams_shelf_[unit_idx], lv_color_hex(0x888888), 0);
  lv_obj_set_style_border_opa(ams_shelf_[unit_idx], LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(ams_shelf_[unit_idx],
      static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
  lv_obj_align(ams_shelf_[unit_idx], LV_ALIGN_CENTER, 0, -50);
  lv_obj_clear_flag(ams_shelf_[unit_idx], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ams_shelf_[unit_idx], LV_OBJ_FLAG_CLICKABLE);
  enable_touch_bubble(ams_shelf_[unit_idx]);

  // Dark base behind lower half of pills
  ams_base_[unit_idx] = lv_obj_create(page);
  lv_obj_set_size(ams_base_[unit_idx], 385, 103);
  lv_obj_set_style_radius(ams_base_[unit_idx], 0, 0);
  lv_obj_set_style_bg_color(ams_base_[unit_idx], lv_color_hex(0x1F1F1F), 0);
  lv_obj_set_style_bg_opa(ams_base_[unit_idx], LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ams_base_[unit_idx], 2, 0);
  lv_obj_set_style_border_color(ams_base_[unit_idx], lv_color_hex(0x888888), 0);
  lv_obj_set_style_border_opa(ams_base_[unit_idx], LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(ams_base_[unit_idx], LV_BORDER_SIDE_FULL, 0);
  lv_obj_align(ams_base_[unit_idx], LV_ALIGN_CENTER, 0, 35);
  lv_obj_clear_flag(ams_base_[unit_idx], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ams_base_[unit_idx], LV_OBJ_FLAG_CLICKABLE);
  enable_touch_bubble(ams_base_[unit_idx]);

  ams_tray_row_[unit_idx] = lv_obj_create(page);
  lv_obj_set_size(ams_tray_row_[unit_idx], 420, LV_SIZE_CONTENT);
  make_transparent(ams_tray_row_[unit_idx]);
  lv_obj_set_flex_flow(ams_tray_row_[unit_idx], LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(ams_tray_row_[unit_idx], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(ams_tray_row_[unit_idx], 6, 0);
  lv_obj_align(ams_tray_row_[unit_idx], LV_ALIGN_CENTER, 0, -13);
  lv_obj_clear_flag(ams_tray_row_[unit_idx], LV_OBJ_FLAG_SCROLLABLE);
  enable_touch_bubble(ams_tray_row_[unit_idx]);

  for (int i = 0; i < kMaxAmsTrays; ++i) {
    ams_tray_col_[unit_idx][i] = lv_obj_create(ams_tray_row_[unit_idx]);
    lv_obj_set_size(ams_tray_col_[unit_idx][i], 76, LV_SIZE_CONTENT);
    make_transparent(ams_tray_col_[unit_idx][i]);
    lv_obj_set_flex_flow(ams_tray_col_[unit_idx][i], LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ams_tray_col_[unit_idx][i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ams_tray_col_[unit_idx][i], 4, 0);
    lv_obj_set_style_pad_all(ams_tray_col_[unit_idx][i], 0, 0);
    lv_obj_clear_flag(ams_tray_col_[unit_idx][i], LV_OBJ_FLAG_SCROLLABLE);

    ams_tray_rect_[unit_idx][i] = lv_obj_create(ams_tray_col_[unit_idx][i]);
    lv_obj_set_size(ams_tray_rect_[unit_idx][i], 72, 140);
    lv_obj_set_style_radius(ams_tray_rect_[unit_idx][i], 40, 0);
    lv_obj_set_style_bg_color(ams_tray_rect_[unit_idx][i], lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_opa(ams_tray_rect_[unit_idx][i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ams_tray_rect_[unit_idx][i], 1, 0);
    lv_obj_set_style_border_color(ams_tray_rect_[unit_idx][i], lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(ams_tray_rect_[unit_idx][i], LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(ams_tray_rect_[unit_idx][i], 0, 0);
    lv_obj_set_style_pad_all(ams_tray_rect_[unit_idx][i], 0, 0);
    lv_obj_set_style_clip_corner(ams_tray_rect_[unit_idx][i], true, 0);
    lv_obj_clear_flag(ams_tray_rect_[unit_idx][i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ams_tray_rect_[unit_idx][i], LV_OBJ_FLAG_CLICKABLE);
    // Diamond/rhombus pattern overlay drawn when this slot has an HMS error.
    lv_obj_add_event_cb(ams_tray_rect_[unit_idx][i], ams_pill_error_overlay_cb,
                        LV_EVENT_DRAW_POST, &ams_tray_error_[unit_idx][i]);

    ams_tray_type_[unit_idx][i] = lv_label_create(ams_tray_rect_[unit_idx][i]);
    set_label_text_if_changed(ams_tray_type_[unit_idx][i], "--");
    lv_obj_set_width(ams_tray_type_[unit_idx][i], 68);
    lv_label_set_long_mode(ams_tray_type_[unit_idx][i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ams_tray_type_[unit_idx][i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ams_tray_type_[unit_idx][i], &dosis_20, 0);
    lv_obj_set_style_text_color(ams_tray_type_[unit_idx][i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ams_tray_type_[unit_idx][i], LV_ALIGN_TOP_MID, 0, 10);

    ams_tray_fill_[unit_idx][i] = lv_obj_create(ams_tray_rect_[unit_idx][i]);
    lv_obj_set_size(ams_tray_fill_[unit_idx][i], 72, 0);
    lv_obj_set_style_radius(ams_tray_fill_[unit_idx][i], 0, 0);
    lv_obj_set_style_bg_color(ams_tray_fill_[unit_idx][i], lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(ams_tray_fill_[unit_idx][i], LV_OPA_40, 0);
    lv_obj_set_style_border_width(ams_tray_fill_[unit_idx][i], 0, 0);
    lv_obj_align(ams_tray_fill_[unit_idx][i], LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_clear_flag(ams_tray_fill_[unit_idx][i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ams_tray_fill_[unit_idx][i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ams_tray_fill_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);

    ams_tray_arrow_[unit_idx][i] = lv_obj_create(ams_tray_col_[unit_idx][i]);
    lv_obj_set_size(ams_tray_arrow_[unit_idx][i], 40, 25);
    make_transparent(ams_tray_arrow_[unit_idx][i]);
    // bg_color carries the triangle color; bg_opa stays 0 so the OBJ background
    // is invisible and only the triangle drawn in ams_arrow_draw_cb is visible.
    lv_obj_set_style_bg_color(ams_tray_arrow_[unit_idx][i], lv_color_hex(0x1F1F1F), 0);
    lv_obj_set_style_pad_all(ams_tray_arrow_[unit_idx][i], 0, 0);
    lv_obj_clear_flag(ams_tray_arrow_[unit_idx][i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ams_tray_arrow_[unit_idx][i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ams_tray_arrow_[unit_idx][i], ams_arrow_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  }

  // Floating percentage labels above pills (ignore layout)
  for (int i = 0; i < kMaxAmsTrays; ++i) {
    ams_tray_pct_[unit_idx][i] = lv_label_create(page);
    set_label_text_if_changed(ams_tray_pct_[unit_idx][i], "");
    lv_obj_set_style_text_font(ams_tray_pct_[unit_idx][i], &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ams_tray_pct_[unit_idx][i], lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_flag(ams_tray_pct_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ams_tray_pct_[unit_idx][i], LV_OBJ_FLAG_IGNORE_LAYOUT);
  }

  // Humidity pill
  lv_obj_t* hum_pill = lv_obj_create(page);
  lv_obj_set_size(hum_pill, 139, 50);
  lv_obj_set_style_radius(hum_pill, 25, 0);
  lv_obj_set_style_bg_opa(hum_pill, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(hum_pill, lv_color_hex(0x9B9B9B), 0);
  lv_obj_set_style_border_opa(hum_pill, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(hum_pill, 1, 0);
  lv_obj_set_style_pad_all(hum_pill, 0, 0);
  lv_obj_align(hum_pill, LV_ALIGN_CENTER, 0, 135);
  lv_obj_clear_flag(hum_pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(hum_pill, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(hum_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(hum_pill, 8, 0);

  ams_humidity_drop_[unit_idx] = lv_obj_create(hum_pill);
  lv_obj_set_size(ams_humidity_drop_[unit_idx], 14, 14);
  lv_obj_set_style_radius(ams_humidity_drop_[unit_idx], LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ams_humidity_drop_[unit_idx], lv_color_hex(0x4A90D9), 0);
  lv_obj_set_style_bg_opa(ams_humidity_drop_[unit_idx], LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ams_humidity_drop_[unit_idx], 0, 0);
  lv_obj_clear_flag(ams_humidity_drop_[unit_idx], LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(ams_humidity_drop_[unit_idx], LV_OBJ_FLAG_CLICKABLE);

  ams_humidity_label_[unit_idx] = lv_label_create(hum_pill);
  set_label_text_if_changed(ams_humidity_label_[unit_idx], "--");
  lv_obj_set_style_text_font(ams_humidity_label_[unit_idx], &dosis_20, 0);
  lv_obj_set_style_text_color(ams_humidity_label_[unit_idx], lv_color_hex(0x94A3B8), 0);
  lv_obj_set_style_text_align(ams_humidity_label_[unit_idx], LV_TEXT_ALIGN_CENTER, 0);

  ams_temp_label_[unit_idx] = lv_label_create(hum_pill);
  set_label_text_if_changed(ams_temp_label_[unit_idx], "--\xC2\xB0""C");
  lv_obj_set_style_text_font(ams_temp_label_[unit_idx], &dosis_20, 0);
  lv_obj_set_style_text_color(ams_temp_label_[unit_idx], lv_color_hex(0x94A3B8), 0);
  lv_obj_set_style_text_align(ams_temp_label_[unit_idx], LV_TEXT_ALIGN_CENTER, 0);

  // External-spool widgets only on the first AMS page.
  if (unit_idx == 0) {
    ams_ext_col_ = lv_obj_create(page);
    lv_obj_set_size(ams_ext_col_, 56, LV_SIZE_CONTENT);
    make_transparent(ams_ext_col_);
    lv_obj_set_flex_flow(ams_ext_col_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ams_ext_col_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(ams_ext_col_, 4, 0);
    lv_obj_set_style_pad_all(ams_ext_col_, 0, 0);
    lv_obj_add_flag(ams_ext_col_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_clear_flag(ams_ext_col_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ams_ext_col_, LV_OBJ_FLAG_HIDDEN);

    ams_ext_rect_ = lv_obj_create(ams_ext_col_);
    lv_obj_set_size(ams_ext_rect_, 52, 108);
    lv_obj_set_style_radius(ams_ext_rect_, 32, 0);
    lv_obj_set_style_bg_color(ams_ext_rect_, lv_color_hex(0x444444), 0);
    lv_obj_set_style_bg_opa(ams_ext_rect_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ams_ext_rect_, 1, 0);
    lv_obj_set_style_border_color(ams_ext_rect_, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_opa(ams_ext_rect_, LV_OPA_COVER, 0);
    lv_obj_set_style_outline_width(ams_ext_rect_, 0, 0);
    lv_obj_set_style_pad_all(ams_ext_rect_, 0, 0);
    lv_obj_set_style_clip_corner(ams_ext_rect_, true, 0);
    lv_obj_clear_flag(ams_ext_rect_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ams_ext_rect_, LV_OBJ_FLAG_CLICKABLE);

    ams_ext_type_ = lv_label_create(ams_ext_rect_);
    set_label_text_if_changed(ams_ext_type_, "EXT");
    lv_obj_set_width(ams_ext_type_, 48);
    lv_label_set_long_mode(ams_ext_type_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ams_ext_type_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ams_ext_type_, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ams_ext_type_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ams_ext_type_, LV_ALIGN_CENTER, 0, 8);

    ams_ext_mat_ = lv_label_create(ams_ext_rect_);
    set_label_text_if_changed(ams_ext_mat_, "");
    lv_obj_set_width(ams_ext_mat_, 48);
    lv_label_set_long_mode(ams_ext_mat_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ams_ext_mat_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ams_ext_mat_, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ams_ext_mat_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(ams_ext_mat_, LV_ALIGN_TOP_MID, 0, 12);

    ams_ext_arrow_ = lv_obj_create(ams_ext_col_);
    lv_obj_set_size(ams_ext_arrow_, 35, 23);
    make_transparent(ams_ext_arrow_);
    // bg_color carries the triangle color; bg_opa stays 0 (see ams_arrow_draw_cb).
    lv_obj_set_style_bg_color(ams_ext_arrow_, lv_color_hex(0x1F1F1F), 0);
    lv_obj_set_style_pad_all(ams_ext_arrow_, 0, 0);
    lv_obj_clear_flag(ams_ext_arrow_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(ams_ext_arrow_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ams_ext_arrow_, ams_arrow_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  }

  ams_note_[unit_idx] = lv_label_create(page);
  set_label_text_if_changed(ams_note_[unit_idx], "No AMS connected");
  lv_obj_set_width(ams_note_[unit_idx], 280);
  lv_label_set_long_mode(ams_note_[unit_idx], LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(ams_note_[unit_idx], LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_font(ams_note_[unit_idx], &dosis_20, 0);
  lv_obj_set_style_text_color(ams_note_[unit_idx], lv_color_hex(0x666666), 0);
  lv_obj_align(ams_note_[unit_idx], LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(ams_note_[unit_idx], LV_OBJ_FLAG_HIDDEN);
}

void PrinterPlugin::compute_ams_tray_errors(const PrinterSnapshot& snapshot) {
  for (int u = 0; u < kMaxAmsUnits; ++u) {
    for (int s = 0; s < kMaxAmsTrays; ++s) {
      ams_tray_error_[u][s] = false;
    }
  }
  for (uint64_t code : snapshot.hms_codes) {
    const uint32_t attr = static_cast<uint32_t>(code >> 32);
    const uint8_t module_id = (attr >> 24) & 0xFFU;
    if (module_id != 0x07U) continue;  // not an AMS-class HMS code
    const uint8_t module_num = (attr >> 16) & 0xFFU;
    int unit_idx = (module_num >= 128) ? (module_num - 128) : module_num;
    if (unit_idx < 0 || unit_idx >= kMaxAmsUnits) continue;
    const uint8_t part_id = (attr >> 8) & 0xFFU;
    // Slot mapping (BambuStudio convention): part_id values 0x20..0x23 map to
    // slots 0..3. Other part_id values describe unit-level errors and are
    // ignored for per-slot overlay (a unit-level indicator could be added
    // later via a shared marker on ams_unit_label_).
    if (part_id >= 0x20U && part_id <= 0x20U + (kMaxAmsTrays - 1)) {
      const int slot_idx = part_id - 0x20U;
      ams_tray_error_[unit_idx][slot_idx] = true;
    }
  }
  apply_ams_error_pulse_locked();
}

void PrinterPlugin::ams_error_pulse_timer_cb(lv_timer_t* timer) {
  auto* plugin = static_cast<PrinterPlugin*>(lv_timer_get_user_data(timer));
  if (plugin == nullptr) return;
  plugin->ams_error_pulse_phase_ = (plugin->ams_error_pulse_phase_ + 1U) & 0x3FU;
  // Triangle wave 0..32 → blend factor between dark and red. The arrow stays
  // fully opaque (LV_OPA_COVER); only the color changes, so it never bleeds
  // through the pill background like opacity-based pulsing did.
  const int32_t phase = static_cast<int32_t>(plugin->ams_error_pulse_phase_);
  const int32_t tri = phase < 32 ? phase : (64 - phase);  // 0..32
  // Blend factor 0..255 (with a small floor so the dim end stays visible).
  const int32_t mix = 60 + tri * 6;  // 60..252
  // Lerp 0x1F1F1F → 0xEF4444
  const int32_t r = (0x1F * (255 - mix) + 0xEF * mix) / 255;
  const int32_t g = (0x1F * (255 - mix) + 0x44 * mix) / 255;
  const int32_t b = (0x1F * (255 - mix) + 0x44 * mix) / 255;
  const lv_color_t col = lv_color_hex(
      (static_cast<uint32_t>(r) << 16) |
      (static_cast<uint32_t>(g) << 8) |
      static_cast<uint32_t>(b));
  for (int u = 0; u < kMaxAmsUnits; ++u) {
    for (int s = 0; s < kMaxAmsTrays; ++s) {
      if (!plugin->ams_tray_error_[u][s]) continue;
      lv_obj_t* arrow = plugin->ams_tray_arrow_[u][s];
      if (arrow == nullptr) continue;
      lv_obj_set_style_bg_color(arrow, col, 0);
      lv_obj_invalidate(arrow);
    }
  }
}

void PrinterPlugin::apply_ams_error_pulse_locked() {
  bool any_error = false;
  for (int u = 0; u < kMaxAmsUnits && !any_error; ++u) {
    for (int s = 0; s < kMaxAmsTrays && !any_error; ++s) {
      if (ams_tray_error_[u][s]) any_error = true;
    }
  }

  // Force pill redraw so the rhombus overlay event-cb re-runs with the new
  // flag value. Arrow color/opacity is owned by render_ams_unit() (non-error)
  // and the pulse timer (error) — we must not override it here, otherwise we
  // would clobber the green "active" indicator on non-error slots.
  for (int u = 0; u < kMaxAmsUnits; ++u) {
    for (int s = 0; s < kMaxAmsTrays; ++s) {
      if (ams_tray_rect_[u][s] != nullptr) {
        lv_obj_invalidate(ams_tray_rect_[u][s]);
      }
    }
  }

  if (any_error && ams_error_pulse_timer_ == nullptr) {
    ams_error_pulse_timer_ = lv_timer_create(&PrinterPlugin::ams_error_pulse_timer_cb, 60, this);
  } else if (!any_error && ams_error_pulse_timer_ != nullptr) {
    lv_timer_delete(ams_error_pulse_timer_);
    ams_error_pulse_timer_ = nullptr;
    ams_error_pulse_phase_ = 0;
  }
}

void PrinterPlugin::render_ams_unit(int unit_idx, const PrinterSnapshot& snapshot, bool show_unit_label) {
  if (unit_idx < 0 || unit_idx >= kMaxAmsUnits) return;
  if (snapshot.ams == nullptr) return;
  const AmsUnitInfo& unit = snapshot.ams->units[unit_idx];

  // Unit header label
  if (ams_unit_label_[unit_idx] != nullptr) {
    set_hidden(ams_unit_label_[unit_idx], !show_unit_label);
  }

  const bool ext_spool_active =
      unit_idx == 0 && (snapshot.tray_now == 254 || snapshot.tray_tar == 254);

  // Dynamic ext-spool layout shrink — only on AMS page 0.
  if (unit_idx == 0 && ext_spool_active != ams_ext_spool_shown_) {
    ams_ext_spool_shown_ = ext_spool_active;
    if (ext_spool_active) {
      for (int i = 0; i < kMaxAmsTrays; ++i) {
        lv_obj_set_size(ams_tray_col_[0][i], 58, LV_SIZE_CONTENT);
        lv_obj_set_size(ams_tray_rect_[0][i], 54, 108);
        lv_obj_set_style_radius(ams_tray_rect_[0][i], 30, 0);
        lv_obj_set_width(ams_tray_type_[0][i], 50);
        lv_obj_set_size(ams_tray_fill_[0][i], 54, 0);
      }
      lv_obj_set_size(ams_shelf_[0], 275, 85);
      lv_obj_align(ams_shelf_[0], LV_ALIGN_CENTER, 38, -55);
      lv_obj_set_size(ams_base_[0], 300, 80);
      lv_obj_align(ams_base_[0], LV_ALIGN_CENTER, 38, 19);
      lv_obj_set_size(ams_tray_row_[0], 310, LV_SIZE_CONTENT);
      lv_obj_align(ams_tray_row_[0], LV_ALIGN_CENTER, 38, -30);
      lv_obj_align(ams_ext_col_, LV_ALIGN_CENTER, -155, -30);
      lv_obj_clear_flag(ams_ext_col_, LV_OBJ_FLAG_HIDDEN);
    } else {
      for (int i = 0; i < kMaxAmsTrays; ++i) {
        lv_obj_set_size(ams_tray_col_[0][i], 76, LV_SIZE_CONTENT);
        lv_obj_set_size(ams_tray_rect_[0][i], 72, 140);
        lv_obj_set_style_radius(ams_tray_rect_[0][i], 40, 0);
        lv_obj_set_width(ams_tray_type_[0][i], 68);
        lv_obj_set_size(ams_tray_fill_[0][i], 72, 0);
      }
      lv_obj_set_size(ams_shelf_[0], 359, 110);
      lv_obj_align(ams_shelf_[0], LV_ALIGN_CENTER, 0, -50);
      lv_obj_set_size(ams_base_[0], 385, 103);
      lv_obj_align(ams_base_[0], LV_ALIGN_CENTER, 0, 35);
      lv_obj_set_size(ams_tray_row_[0], 420, LV_SIZE_CONTENT);
      lv_obj_align(ams_tray_row_[0], LV_ALIGN_CENTER, 0, -13);
      lv_obj_add_flag(ams_ext_col_, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // External spool styling (only on unit 0 when active).
  if (unit_idx == 0 && ext_spool_active) {
    const AmsTrayInfo& ext = snapshot.ams->external_spool;
    if (ext.color_rgba != 0) {
      const uint32_t ext_rgb = (ext.color_rgba >> 8) & 0x00FFFFFF;
      lv_obj_set_style_bg_color(ams_ext_rect_, lv_color_hex(ext_rgb), 0);
      const bool ext_dark = ((ext_rgb >> 16) & 0xFF) * 299 +
                            ((ext_rgb >> 8) & 0xFF) * 587 +
                            (ext_rgb & 0xFF) * 114 < 128000;
      const lv_color_t txt_col = lv_color_hex(ext_dark ? 0xFFFFFF : 0x000000);
      lv_obj_set_style_text_color(ams_ext_type_, txt_col, 0);
      lv_obj_set_style_text_color(ams_ext_mat_, txt_col, 0);
    } else {
      lv_obj_set_style_bg_color(ams_ext_rect_, lv_color_hex(0x444444), 0);
      lv_obj_set_style_text_color(ams_ext_type_, lv_color_hex(0xFFFFFF), 0);
      lv_obj_set_style_text_color(ams_ext_mat_, lv_color_hex(0xFFFFFF), 0);
    }
    lv_obj_set_style_bg_opa(ams_ext_rect_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ams_ext_rect_, 1, 0);
    lv_obj_set_style_border_color(ams_ext_rect_, lv_color_hex(0x555555), 0);
    lv_obj_set_style_outline_width(ams_ext_rect_, 0, 0);
    lv_obj_set_style_bg_color(ams_ext_arrow_, lv_color_hex(0x4ADE80), 0);
    const char* mat_label = !ext.material_type.empty() ? ext.material_type.c_str() : "";
    set_label_text_if_changed(ams_ext_mat_, mat_label);
  }

  const int pill_h = (unit_idx == 0 && ext_spool_active) ? 108 : 140;

  for (int i = 0; i < kMaxAmsTrays; ++i) {
    const AmsTrayInfo& tray = unit.trays[i];
    lv_obj_t* rect = ams_tray_rect_[unit_idx][i];
    lv_obj_t* arrow = ams_tray_arrow_[unit_idx][i];
    const bool has_error = ams_tray_error_[unit_idx][i];
    if (tray.present) {
      const uint32_t rgba = tray.color_rgba;
      const uint32_t rgb = (rgba >> 8) & 0x00FFFFFF;
      lv_obj_set_style_bg_color(rect, lv_color_hex(rgb), 0);
      lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(rect, 1, 0);
      lv_obj_set_style_border_color(rect, lv_color_hex(0x555555), 0);
      lv_obj_set_style_outline_width(rect, 0, 0);
      if (has_error) {
        // Red triangle on error (color pulses via timer).
        lv_obj_set_style_bg_color(arrow, lv_color_hex(0xEF4444), 0);
      } else if (tray.active) {
        lv_obj_set_style_bg_color(arrow, lv_color_hex(0x4ADE80), 0);
      } else {
        lv_obj_set_style_bg_color(arrow, lv_color_hex(0x1F1F1F), 0);
      }
      set_label_text_if_changed(ams_tray_type_[unit_idx][i],
                                tray.material_type.empty() ? "--" : tray.material_type);
      const bool is_dark = ((rgb >> 16) & 0xFF) * 299 +
                           ((rgb >> 8) & 0xFF) * 587 +
                           (rgb & 0xFF) * 114 < 128000;
      lv_obj_set_style_text_color(ams_tray_type_[unit_idx][i],
          lv_color_hex(is_dark ? 0xFFFFFF : 0x000000), 0);

      if (tray.remain_pct >= 0) {
        const int empty_h = pill_h - (pill_h * tray.remain_pct / 100);
        lv_obj_set_height(ams_tray_fill_[unit_idx][i], empty_h);
        lv_obj_align(ams_tray_fill_[unit_idx][i], LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_clear_flag(ams_tray_fill_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);
        char pct_buf[8];
        std::snprintf(pct_buf, sizeof(pct_buf), "%d%%", tray.remain_pct);
        set_label_text_if_changed(ams_tray_pct_[unit_idx][i], pct_buf);
        lv_obj_set_style_text_color(ams_tray_pct_[unit_idx][i],
            lv_color_hex(is_dark ? 0xFFFFFF : 0x000000), 0);
        lv_obj_clear_flag(ams_tray_pct_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_align_to(ams_tray_pct_[unit_idx][i], rect,
                        LV_ALIGN_BOTTOM_MID, 0, -10);
      } else {
        lv_obj_add_flag(ams_tray_fill_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ams_tray_pct_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);
      }
    } else {
      lv_obj_set_style_bg_color(rect, lv_color_hex(0x333333), 0);
      lv_obj_set_style_bg_opa(rect, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(rect, 1, 0);
      lv_obj_set_style_border_color(rect, lv_color_hex(0x555555), 0);
      lv_obj_set_style_outline_width(rect, 0, 0);
      set_label_text_if_changed(ams_tray_type_[unit_idx][i], "Empty");
      lv_obj_add_flag(ams_tray_fill_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(ams_tray_pct_[unit_idx][i], LV_OBJ_FLAG_HIDDEN);
      if (has_error) {
        lv_obj_set_style_bg_color(arrow, lv_color_hex(0xEF4444), 0);
      } else {
        lv_obj_set_style_bg_color(arrow, lv_color_hex(0x1F1F1F), 0);
      }
    }
  }

  // Humidity + temperature
  char hum_buf[16] = {};
  if (unit.humidity_pct >= 0) {
    std::snprintf(hum_buf, sizeof(hum_buf), "%d%%", unit.humidity_pct);
  } else {
    std::snprintf(hum_buf, sizeof(hum_buf), "--%% ");
  }
  set_label_text_if_changed(ams_humidity_label_[unit_idx], hum_buf);

  char temp_buf[24] = {};
  if (unit.temperature_c > 0.0f) {
    std::snprintf(temp_buf, sizeof(temp_buf), "%.0f%s", unit.temperature_c, kDegreeC);
  } else {
    std::snprintf(temp_buf, sizeof(temp_buf), "--%s", kDegreeC);
  }
  set_label_text_if_changed(ams_temp_label_[unit_idx], temp_buf);

  set_hidden(ams_tray_row_[unit_idx], false);
  set_hidden(ams_note_[unit_idx], true);
}

// --- Preview/camera image lifecycle ------------------------------------

bool PrinterPlugin::ensure_preview_image_loaded_locked(
    bool force_reload,
    std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw,
    const lv_image_dsc_t* pre_decoded_dsc) {
  if (force_reload) {
    release_preview_image_locked();
  }

  if (last_preview_raw_ && !last_preview_raw_->empty()) {
    // Source is already installed for this decoded blob. Re-setting it on
    // every status tick invalidates the full 320px image and can starve other
    // tasks trying to acquire the LVGL lock.
    return true;
  }

  // Use pre-decoded data when available (decoded outside LVGL lock).
  if (pre_decoded_raw && !pre_decoded_raw->empty() && pre_decoded_dsc != nullptr) {
    last_preview_raw_ = std::move(pre_decoded_raw);
    preview_image_dsc_ = *pre_decoded_dsc;
    lv_image_set_src(page2_image_, &preview_image_dsc_);
    log_blob_diag("ui preview set_src raw", last_preview_raw_);
    return true;
  }

  // No fallback decode under lock — pre-decode happens outside the LVGL
  // lock in update_ui() (see apply_snapshot_locked's caller). If we reach
  // here without decoded data the image will appear on the next snapshot
  // tick (~500 ms).
  return false;
}

void PrinterPlugin::release_preview_image_locked() {
  if (page2_image_ != nullptr) {
    lv_image_set_src(page2_image_, nullptr);
  }
  lv_image_cache_drop(&preview_image_dsc_);
  last_preview_raw_.reset();
  std::memset(&preview_image_dsc_, 0, sizeof(preview_image_dsc_));
}

// --- Snapshot rendering --------------------------------------------------

void PrinterPlugin::update_page_availability_locked(const PrinterSnapshot& snapshot) {
  // While the printer plugin is disabled, nothing should re-derive AMS/
  // preview/camera visibility from a snapshot. This is called not just from
  // update_ui() (already gated at the Application level by Plugin::enabled())
  // but also from the page-settle callback on every page swipe, which always
  // re-applies last_snapshot_ regardless of which plugin's page is now
  // active. Without this guard, a swipe while disabled resurrects
  // camera_page_available_/preview_page_available_ from whatever
  // last_snapshot_ held before disabling, unhiding pages that the enabled
  // toggle already hid.
  if (!enabled()) {
    return;
  }
  const bool preview_available = snapshot.preview_page_available;
  const bool camera_available = snapshot.camera_page_available;
  const uint8_t ams_count = snapshot.ams ? snapshot.ams->count : 0;
  bool ams_changed = false;
  for (int u = 0; u < kMaxAmsUnits; ++u) {
    const bool present = u < ams_count;
    if (ams_unit_present_[u] != present) {
      ams_unit_present_[u] = present;
      ams_changed = true;
    }
  }
  const bool availability_changed =
      preview_page_available_ != preview_available || camera_page_available_ != camera_available ||
      ams_changed;

  preview_page_available_ = preview_available;
  camera_page_available_ = camera_available;

  if (!availability_changed) {
    return;
  }

  hide_printer_content_pages_locked();
}

void PrinterPlugin::hide_printer_content_pages_locked() {
  for (int u = 0; u < kMaxAmsUnits; ++u) {
    if (ams_pages_[u] != nullptr) {
      set_hidden(ams_pages_[u], !ams_unit_present_[u]);
    }
  }
  lv_obj_t* page2 = ui_->plugin_page_container_for("printer", kMaxAmsUnits + 2);
  lv_obj_t* page3 = ui_->plugin_page_container_for("printer", kMaxAmsUnits + 3);
  set_hidden(page2, !preview_page_available_);
  set_hidden(page3, !camera_page_available_);

  if (!preview_page_available_) {
    release_preview_image_locked();
    preview_image_visible_ = false;
  }

  if (!camera_page_available_) {
    if (camera_slot_initialized_ && page3_image_ != nullptr) {
      lv_image_set_src(page3_image_, nullptr);
    }
    lv_image_cache_drop(&camera_image_dscs_[0]);
    lv_image_cache_drop(&camera_image_dscs_[1]);
    camera_blobs_[0].reset();
    camera_blobs_[1].reset();
    camera_slot_initialized_ = false;
    active_camera_slot_ = 0;
    last_camera_width_ = 0;
    last_camera_height_ = 0;
    std::memset(&camera_image_dscs_[0], 0, sizeof(camera_image_dscs_[0]));
    std::memset(&camera_image_dscs_[1], 0, sizeof(camera_image_dscs_[1]));
    camera_image_visible_ = false;
  }

  // Per-page (AMS unit)/preview/camera availability just changed — tell Ui's
  // pager to re-clamp/rescroll/republish/re-parallax (pure chrome, no
  // printer knowledge needed there).
  ui_->resettle_pager_after_availability_change();
}

void PrinterPlugin::apply_enabled_state_to_ui(bool enabled_now, uint32_t lock_timeout_ms) {
  if (!enabled_now) {
    LvglLockGuard lock(lock_timeout_ms, "printer_apply_enabled_state");
    if (lock.locked()) {
      for (int u = 0; u < kMaxAmsUnits; ++u) {
        ams_unit_present_[u] = false;
      }
      preview_page_available_ = false;
      camera_page_available_ = false;
      hide_printer_content_pages_locked();
    }
  }
  ui_->set_plugin_pages_enabled(id(), enabled_now, lock_timeout_ms);
}

void PrinterPlugin::apply_snapshot_locked(const PrinterSnapshot& snapshot, bool force_ring_refresh,
                                          std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw,
                                          const lv_image_dsc_t* pre_decoded_dsc) {
  (void)force_ring_refresh;  // ring visual is driven separately by apply_ring_visual()
  deferred_snapshot_pending_ = false;
  update_page_availability_locked(snapshot);

  bool wake_due_to_state_change = false;
  if (!snapshot.ui_status.empty() && snapshot.ui_status != last_ui_status_) {
    wake_due_to_state_change = true;
    last_ui_status_ = snapshot.ui_status;
  }
  if (snapshot.print_active != last_print_active_) {
    wake_due_to_state_change = true;
    last_print_active_ = snapshot.print_active;
  }
  if (wake_due_to_state_change) {
    ui_->note_activity(true);
  }

  const std::string status_text = lifecycle_label(snapshot);
  set_label_text_if_changed(status_label_, status_text);

  const std::string detail = detail_text(snapshot);

  // [DIAG] Log what the display is actually showing — on change only.
  if (status_text != last_diag_status_ || detail != last_diag_detail_ ||
      snapshot.stage != last_diag_stage_) {
    last_diag_status_ = status_text;
    last_diag_detail_ = detail;
    last_diag_stage_ = snapshot.stage;
    ESP_LOGI(kTag, "[DIAG] display: status=%s stage=%s detail=%.60s lifecycle=%s",
             status_text.c_str(), snapshot.stage.c_str(),
             detail.empty() ? "(-)" : detail.c_str(),
             to_string(snapshot.lifecycle));
  }
  detail_visible_ = !detail.empty();
  if (detail_visible_) {
    // Scroll long error / HMS messages so the full TSV-resolved text is
    // readable on the narrow label.  Warnings (HMS codes without `has_error`)
    // also scroll: the user otherwise only sees a wrapped/truncated bare
    // code where the lookup text would not fit on a single line.
    const bool has_hms_or_error = snapshot.has_error ||
                                  snapshot.print_error_code != 0 ||
                                  !snapshot.hms_codes.empty() ||
                                  snapshot.hms_alert_count > 0;
    // LVGL's circular-scroll long mode triggers continuous widget invalidation
    // for the running marquee animation. That work happens regardless of the
    // detail label being on screen, so on the AMS / preview pages it just
    // burns LVGL-lock time. Only enable scrolling when the main page is
    // actually settled in front of the user — every other state falls back
    // to the cheaper LV_LABEL_LONG_WRAP, which renders once and is silent.
    const bool main_page_visible = ui_->is_plugin_page_active("printer", kMaxAmsUnits + 1);
    const lv_label_long_mode_t desired_mode =
        (has_hms_or_error && main_page_visible) ? LV_LABEL_LONG_SCROLL_CIRCULAR
                                                 : LV_LABEL_LONG_WRAP;
    if (lv_label_get_long_mode(detail_label_) != desired_mode) {
      lv_label_set_long_mode(detail_label_, desired_mode);
    }
    set_label_text_if_changed(detail_label_, detail);
  }

  const std::string layer = layer_text(snapshot);
  set_label_text_if_changed(layer_label_, layer);

  // Drive the optional spool icon + grams text alongside the layer counter
  // (siblings inside layer_row_, hidden when no slicer estimate is known).
  const std::string filament = filament_estimate_text(snapshot);
  const bool has_filament_estimate = !filament.empty();
  // The spool icon (U+F1097) maps to the wrong glyph in the embedded MDI font
  // (renders as "human-male-board" instead of a spool). Keep it hidden until
  // the font is regenerated with the correct codepoint mapping.
  set_hidden(filament_icon_label_, true);
  set_hidden(filament_value_label_, !has_filament_estimate);
  if (has_filament_estimate) {
    set_label_text_if_changed(filament_value_label_, filament);
  }

  const std::string remaining = show_eta_ ? eta_text(snapshot) : remaining_text(snapshot);
  set_label_text_if_changed(remaining_label_, remaining);
  // Hide the clock-icon prefix in ETA mode — leaves more room for "HH:MM"
  // and avoids the redundant clock-glyph + clock-time stutter.
  set_hidden(remaining_prefix_label_, show_eta_);

  char temp_buffer[24] = {};
  const bool is_dual_nozzle = snapshot.active_nozzle_index >= 0;
  const char* active_prefix = is_dual_nozzle ? (snapshot.active_nozzle_index == 1 ? "L " : "R ") : "";
  const char* secondary_prefix = is_dual_nozzle ? (snapshot.active_nozzle_index == 1 ? "R " : "L ") : "";
  if (snapshot.nozzle_temp_known || snapshot.nozzle_temp_c > 0.0f) {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "%s%.0f%s", active_prefix, snapshot.nozzle_temp_c, kDegreeC);
  } else {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "%s--%s", active_prefix, kDegreeC);
  }
  set_label_text_if_changed(nozzle_value_label_, temp_buffer);

  if (snapshot.bed_temp_known || snapshot.bed_temp_c > 0.0f) {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "%.0f%s", snapshot.bed_temp_c, kDegreeC);
  } else {
    std::snprintf(temp_buffer, sizeof(temp_buffer), "--%s", kDegreeC);
  }
  set_label_text_if_changed(bed_value_label_, temp_buffer);

  char nozzle_aux_buf[40] = {};
  if (is_dual_nozzle &&
      (snapshot.secondary_nozzle_temp_known || snapshot.secondary_nozzle_temp_c > 0.0f)) {
    std::snprintf(nozzle_aux_buf, sizeof(nozzle_aux_buf), "%s%.0f%s",
                  secondary_prefix, snapshot.secondary_nozzle_temp_c, kDegreeC);
  } else if (!is_dual_nozzle) {
    const std::string tmp =
        optional_temperature_text("Other nozzle", snapshot.secondary_nozzle_temp_c,
                                  snapshot.secondary_nozzle_temp_known);
    std::snprintf(nozzle_aux_buf, sizeof(nozzle_aux_buf), "%s", tmp.c_str());
  }
  const std::string nozzle_aux(nozzle_aux_buf);
  nozzle_aux_visible_ = !nozzle_aux.empty();
  if (nozzle_aux_visible_) {
    set_label_text_if_changed(nozzle_aux_label_, nozzle_aux);
  }

  const std::string bed_aux =
      optional_temperature_text("Chamber", snapshot.chamber_temp_c, snapshot.chamber_temp_known);
  bed_aux_visible_ = !bed_aux.empty();
  if (bed_aux_visible_) {
    set_label_text_if_changed(bed_aux_label_, bed_aux);
  }

  // Battery overlay text is refreshed by Application's core loop now
  // (Ui::update_battery_overlay(), driven directly by PmuManager) — this
  // plugin only refines *visibility* further below, in refresh_page_visibility().

  // --- AMS page rendering ---
  const uint8_t ams_count = snapshot.ams ? snapshot.ams->count : 0;
  const uint32_t ams_signature = ams_ui_signature(snapshot);
  const bool ams_visible = ui_->is_page_transition_active() ||
      (ui_->is_plugin_page_active("printer", 1) || ui_->is_plugin_page_active("printer", 2) ||
       ui_->is_plugin_page_active("printer", 3) || ui_->is_plugin_page_active("printer", 4));
  const bool render_ams =
      ams_visible && ams_signature != last_rendered_ams_signature_;
  if (ams_count > 0 && render_ams) {
    last_rendered_ams_signature_ = ams_signature;
    compute_ams_tray_errors(snapshot);
    const bool show_unit_labels = ams_count > 1;
    for (int u = 0; u < kMaxAmsUnits; ++u) {
      if (u < ams_count) {
        render_ams_unit(u, snapshot, show_unit_labels);
      }
    }
  } else if (render_ams) {
    last_rendered_ams_signature_ = ams_signature;
    // No AMS — show "No AMS connected" note on the first AMS page only.
    for (int u = 0; u < kMaxAmsUnits; ++u) {
      if (ams_tray_row_[u] != nullptr) {
        set_hidden(ams_tray_row_[u], true);
      }
      if (ams_note_[u] != nullptr) {
        set_hidden(ams_note_[u], u != 0);
      }
      for (int s = 0; s < kMaxAmsTrays; ++s) {
        ams_tray_error_[u][s] = false;
      }
    }
    if (ams_ext_col_ != nullptr) {
      set_hidden(ams_ext_col_, true);
    }
    apply_ams_error_pulse_locked();
  }

  const std::string preview_note = preview_note_text(snapshot);
  const std::string preview_subnote = preview_subnote_text(snapshot);
  const std::string camera_note = camera_note_text(snapshot);
  const std::string camera_subnote = camera_subnote_text(snapshot);
  bool has_preview_image = false;
  const bool preview_page_active = ui_->is_plugin_page_active("printer", kMaxAmsUnits + 2);
  if (snapshot.preview_blob && !snapshot.preview_blob->empty()) {
    const bool preview_blob_changed = last_preview_blob_.get() != snapshot.preview_blob.get();
    last_preview_blob_ = snapshot.preview_blob;
    if (preview_page_active) {
      has_preview_image = ensure_preview_image_loaded_locked(
          preview_blob_changed, std::move(pre_decoded_raw), pre_decoded_dsc);
    } else if (preview_blob_changed) {
      release_preview_image_locked();
    }
  } else {
    if (last_preview_blob_ || last_preview_raw_) {
      release_preview_image_locked();
    }
    last_preview_blob_.reset();
  }
  const bool has_page2_image = has_preview_image;
  preview_image_visible_ = has_page2_image;

  if (preview_text_image_mode_ != has_page2_image) {
    if (has_page2_image) {
      // Note is hidden when cover is loaded; subnote (title) moves to former note position.
      lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, kPage2NoteWithImageY);
    } else {
      lv_obj_align(page2_note_, LV_ALIGN_CENTER, 0, -14);
      lv_obj_align(page2_subnote_, LV_ALIGN_CENTER, 0, 18);
    }
    preview_text_image_mode_ = has_page2_image;
  }
  set_hidden(page2_note_, preview_note.empty());
  if (!preview_note.empty()) {
    set_label_text_if_changed(page2_note_, preview_note);
  }
  set_hidden(page2_subnote_, preview_subnote.empty());
  if (!preview_subnote.empty()) {
    set_label_text_if_changed(page2_subnote_, preview_subnote);
  }
  // Avoid LV_LABEL_LONG_SCROLL_CIRCULAR here. It is a permanent LVGL animation
  // and on the preview page it competes with large image redraws under the
  // display lock. Use a static dotted title instead.
  {
    const bool image_title = preview_page_active && has_page2_image;
    const lv_label_long_mode_t desired = image_title ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP;
    if (lv_label_get_long_mode(page2_subnote_) != desired) {
      lv_label_set_long_mode(page2_subnote_, desired);
    }
  }

  update_print_buttons_locked(snapshot);

  bool has_camera_image =
      camera_slot_initialized_ && camera_blobs_[active_camera_slot_] &&
      !camera_blobs_[active_camera_slot_]->empty();
  if (snapshot.camera_blob && !snapshot.camera_blob->empty() && snapshot.camera_width > 0U &&
      snapshot.camera_height > 0U) {
    const bool camera_blob_changed =
        !camera_slot_initialized_ || camera_blobs_[active_camera_slot_].get() != snapshot.camera_blob.get();
    if (camera_blob_changed ||
        last_camera_width_ != snapshot.camera_width || last_camera_height_ != snapshot.camera_height) {
      const uint8_t next_slot =
          camera_slot_initialized_ ? static_cast<uint8_t>((active_camera_slot_ + 1U) % 2U) : 0U;
      log_blob_diag("ui camera incoming rgb565", snapshot.camera_blob);
      lv_image_cache_drop(&camera_image_dscs_[next_slot]);
      log_heap_diag("ui camera after cache drop");
      lv_image_dsc_t next_dsc = {};
      if (configure_camera_rgb565(snapshot.camera_blob, snapshot.camera_width, snapshot.camera_height,
                                  &next_dsc)) {
        camera_blobs_[next_slot] = snapshot.camera_blob;
        camera_image_dscs_[next_slot] = next_dsc;
        active_camera_slot_ = next_slot;
        camera_slot_initialized_ = true;
        last_camera_width_ = snapshot.camera_width;
        last_camera_height_ = snapshot.camera_height;
        lv_image_set_src(page3_image_, &camera_image_dscs_[active_camera_slot_]);
        log_heap_diag("ui camera after lv_image_set_src");
      } else {
        camera_blobs_[next_slot].reset();
        std::memset(&camera_image_dscs_[next_slot], 0, sizeof(camera_image_dscs_[next_slot]));
        if (!camera_slot_initialized_) {
          active_camera_slot_ = 0;
        }
        last_camera_width_ = 0;
        last_camera_height_ = 0;
      }
    }
    has_camera_image =
        camera_slot_initialized_ && camera_blobs_[active_camera_slot_] &&
        !camera_blobs_[active_camera_slot_]->empty();
  } else {
    if (camera_slot_initialized_ && page3_image_ != nullptr) {
      lv_image_set_src(page3_image_, nullptr);
    }
    lv_image_cache_drop(&camera_image_dscs_[0]);
    lv_image_cache_drop(&camera_image_dscs_[1]);
    camera_blobs_[0].reset();
    camera_blobs_[1].reset();
    camera_slot_initialized_ = false;
    active_camera_slot_ = 0;
    last_camera_width_ = 0;
    last_camera_height_ = 0;
    std::memset(&camera_image_dscs_[0], 0, sizeof(camera_image_dscs_[0]));
    std::memset(&camera_image_dscs_[1], 0, sizeof(camera_image_dscs_[1]));
    has_camera_image = false;
  }
  camera_image_visible_ = has_camera_image;

  if (camera_text_image_mode_ != has_camera_image) {
    if (has_camera_image) {
      // Status note moves above the image; layer/subnote sits below.
      lv_obj_align(page3_note_, LV_ALIGN_CENTER, 0, kPage3StatusAboveImageY);
      lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, kPage3NoteWithImageY);
    } else {
      lv_obj_align(page3_note_, LV_ALIGN_CENTER, 0, 0);
      lv_obj_align(page3_subnote_, LV_ALIGN_CENTER, 0, 28);
    }
    camera_text_image_mode_ = has_camera_image;
  }
  set_hidden(page3_note_, camera_note.empty());
  if (!camera_note.empty()) {
    set_label_text_if_changed(page3_note_, camera_note);
  }
  set_hidden(page3_subnote_, camera_subnote.empty());
  if (!camera_subnote.empty()) {
    // Use the same large font as the main-page layer label when showing
    // "Layer: X / Y", fall back to the regular small font otherwise.
    const bool subnote_is_layer =
        has_camera_image && (snapshot.total_layers > 0 || snapshot.current_layer > 0);
    lv_obj_set_style_text_font(page3_subnote_,
                               subnote_is_layer ? &dosis_32 : &lv_font_montserrat_20, 0);
    set_label_text_if_changed(page3_subnote_, camera_subnote);
  }

  show_logo_ = should_show_logo(snapshot);
  const bool chamber_light_clickable = snapshot.chamber_light_supported;
  if (logo_clickable_ != chamber_light_clickable) {
    set_clickable(logo_badge_, chamber_light_clickable);
    logo_clickable_ = chamber_light_clickable;
  }

  const bool logo_recolor_enabled =
      snapshot.chamber_light_supported && snapshot.chamber_light_state_known &&
      !snapshot.chamber_light_on;
  const uint32_t logo_recolor_hex = logo_recolor_enabled ? 0x7A7A7A : 0U;
  if (logo_recolor_enabled != logo_recolor_enabled_ ||
      (logo_recolor_enabled && logo_recolor_hex != logo_recolor_hex_)) {
    if (logo_recolor_enabled) {
      lv_obj_set_style_image_recolor(logo_image_, lv_color_hex(logo_recolor_hex), 0);
      lv_obj_set_style_image_recolor_opa(logo_image_, LV_OPA_COVER, 0);
    } else {
      lv_obj_set_style_image_recolor_opa(logo_image_, LV_OPA_TRANSP, 0);
    }
    logo_recolor_enabled_ = logo_recolor_enabled;
    logo_recolor_hex_ = logo_recolor_hex;
  }
  // Portal hint/PIN overlay is driven by Application calling
  // update_portal_access_visuals() directly every loop iteration (core, not
  // routed through any plugin's snapshot tick) — see that method.
  refresh_page_visibility();
}

void PrinterPlugin::update_ui() {
  // Application only calls update_ui() for enabled plugins, so reaching here
  // means this plugin is enabled — ensure its page range is visible (cheap
  // no-op once already so, mirrors WeatherPlugin's own update_ui() pattern).
  // The disable transition itself is handled by apply_enabled_state_to_ui(),
  // called from the portal's enabled-toggle handler.
  ui_->set_plugin_pages_enabled(id(), true, 200);

  // Moved from the old Ui::apply_snapshot() (pre-Phase-4b/4c) — same
  // pre-decode-outside-the-lock optimization and mid-scroll deferral, just
  // now living on PrinterPlugin instead of Ui.
  if (ui_->is_page_transition_active()) {
    last_snapshot_ = latest_snapshot_;
    deferred_snapshot_ = latest_snapshot_;
    deferred_snapshot_pending_ = true;
    apply_ring_visual();
    return;
  }

  // Pre-decode preview PNG outside LVGL lock, but only when the preview page is
  // actually active. The decoded cover is ~1 MB, lives in PSRAM, and is kept
  // cached across page changes so navigating away doesn't create a decode storm.
  std::shared_ptr<std::vector<uint8_t>> pre_decoded_raw;
  lv_image_dsc_t pre_decoded_dsc{};
  const bool preview_page_active = ui_->is_plugin_page_active("printer", kMaxAmsUnits + 2);
  const bool preview_blob_changed =
      latest_snapshot_.preview_blob && !latest_snapshot_.preview_blob->empty() &&
      last_preview_blob_.get() != latest_snapshot_.preview_blob.get();
  // Also pre-decode when the blob exists but hasn't been decoded yet (e.g.
  // blob arrived while another page was active, then user scrolled to preview).
  const bool needs_first_decode =
      !preview_blob_changed && latest_snapshot_.preview_blob &&
      !latest_snapshot_.preview_blob->empty() &&
      (!last_preview_raw_ || last_preview_raw_->empty());
  if (preview_page_active && (preview_blob_changed || needs_first_decode)) {
    decode_preview_png(latest_snapshot_.preview_blob, &pre_decoded_raw, &pre_decoded_dsc);
  }

  LvglLockGuard lock(500, "printer_plugin_update_ui");
  if (!lock.locked()) {
    return;
  }

  last_snapshot_ = latest_snapshot_;
  if (ui_->is_page_transition_active()) {
    deferred_snapshot_ = latest_snapshot_;
    deferred_snapshot_pending_ = true;
    apply_ring_visual();
    return;
  }

  apply_snapshot_locked(latest_snapshot_, false, std::move(pre_decoded_raw), &pre_decoded_dsc);
  apply_ring_visual();

  // Default to the main dashboard on first boot, not this plugin's own page 0
  // (printer-selector), which Ui's own default (page pool index 0) would
  // otherwise land on since printer registers first. Must happen here, after
  // set_plugin_pages_enabled() above and update_page_availability_locked()
  // (inside apply_snapshot_locked) have both run at least once — page-enabled
  // state isn't populated yet during build_screen(), so doing this jump from
  // there instead landed on whatever page happened to read as "enabled by
  // default" (see initial_page_set_'s declaration).
  //
  // set_plugin_pages_enabled() above can itself silently no-op if it fails to
  // grab the LVGL lock (200ms timeout, common on a boot-contended tick) —
  // when that happens the dashboard page isn't enabled yet, so jumping to it
  // here would just clamp to whatever else reads enabled (e.g. the preview
  // page) and get stuck there forever once latched. Only latch once the jump
  // actually landed on the dashboard; otherwise retry next tick.
  if (!initial_page_set_) {
    ui_->set_active_page_by_plugin(id(), kMaxAmsUnits + 1);
    initial_page_set_ = ui_->is_plugin_page_active(id(), kMaxAmsUnits + 1);
  }
}

// --- Visibility / page-settle / tap callbacks --------------------------

void PrinterPlugin::refresh_page_visibility() {
  const bool scrolling = ui_->is_page_transition_active();
  const bool on_page1 = ui_->is_plugin_page_active("printer", kMaxAmsUnits + 1) || scrolling;
  const bool on_page2 = ui_->is_plugin_page_active("printer", kMaxAmsUnits + 2) || scrolling;
  const bool on_page3 = ui_->is_plugin_page_active("printer", kMaxAmsUnits + 3) || scrolling;
  const bool settled_page1 = ui_->is_plugin_page_active("printer", kMaxAmsUnits + 1);

  for (int u = 0; u < kMaxAmsUnits; ++u) {
    if (ams_pages_[u] != nullptr) {
      set_hidden(ams_pages_[u], !ams_unit_present_[u]);
    }
  }
  lv_obj_t* page2 = ui_->plugin_page_container_for("printer", kMaxAmsUnits + 2);
  lv_obj_t* page3 = ui_->plugin_page_container_for("printer", kMaxAmsUnits + 3);
  set_hidden(page2, !preview_page_available_);
  set_hidden(page3, !camera_page_available_);
  set_hidden(status_label_, !on_page1);
  set_hidden(detail_label_, !on_page1 || !detail_visible_ || ui_->is_portal_hint_visible());
  set_hidden(layer_row_, !on_page1);
  set_hidden(nozzle_prefix_label_, !on_page1);
  set_hidden(nozzle_value_label_, !on_page1);
  set_hidden(nozzle_aux_label_, !on_page1 || !nozzle_aux_visible_);
  set_hidden(bed_prefix_label_, !on_page1);
  set_hidden(bed_value_label_, !on_page1);
  set_hidden(bed_aux_label_, !on_page1 || !bed_aux_visible_);
  set_hidden(remaining_row_, !on_page1);
  set_hidden(badge_slot_, !on_page1);
  set_hidden(page2_image_, !on_page2 || !preview_image_visible_);
  set_hidden(page3_image_, !on_page3 || !camera_image_visible_);

  // badge_slot_ is a child of the main-dashboard page so it clips naturally
  // during scroll.
  const bool show_badge_slot = scrolling || settled_page1;
  if (!show_badge_slot) {
    set_hidden(logo_badge_, true);
  } else {
    set_hidden(logo_badge_, !show_logo_);
  }

  // Battery overlay + portal hint visibility depend on printer's own page
  // state (which page is settled) — Ui resolves the rest (text/portal state)
  // itself via set_battery_overlay_text()/update_portal_access_visuals().
  ui_->set_battery_overlay_visible(
      (last_snapshot_.battery_present || last_snapshot_.charging) &&
      (settled_page1 || ui_->is_plugin_page_active("printer", kMaxAmsUnits + 3)));
}

void PrinterPlugin::refresh_page_visibility_trampoline(void* user_data) {
  static_cast<PrinterPlugin*>(user_data)->refresh_page_visibility();
}

void PrinterPlugin::on_page_settled() {
  if (deferred_snapshot_pending_) {
    apply_snapshot_locked(deferred_snapshot_, true);
  } else {
    apply_snapshot_locked(last_snapshot_, true);
  }
  if (ui_->is_plugin_page_active("printer", kMaxAmsUnits + 2)) {
    preview_image_visible_ = ensure_preview_image_loaded_locked(false);
  }
  if (ui_->is_plugin_page_active("printer", kMaxAmsUnits + 3)) {
    std::lock_guard<std::mutex> lock(camera_refresh_mutex_);
    camera_refresh_requested_ = true;
  }
}

void PrinterPlugin::on_page_settled_trampoline(void* user_data) {
  static_cast<PrinterPlugin*>(user_data)->on_page_settled();
}

void PrinterPlugin::on_page_tapped() {
  if (ui_->is_plugin_page_active("printer", kMaxAmsUnits + 3)) {
    std::lock_guard<std::mutex> lock(camera_refresh_mutex_);
    camera_refresh_requested_ = true;
  }
}

void PrinterPlugin::on_page_tapped_trampoline(void* user_data) {
  static_cast<PrinterPlugin*>(user_data)->on_page_tapped();
}

bool PrinterPlugin::consume_camera_refresh_request() {
  std::lock_guard<std::mutex> lock(camera_refresh_mutex_);
  const bool requested = camera_refresh_requested_;
  camera_refresh_requested_ = false;
  return requested;
}

// --- Print buttons / remaining-row / logo click -------------------------

void PrinterPlugin::update_print_buttons_locked(const PrinterSnapshot& snapshot) {
  if (page2_pause_button_ == nullptr || page2_stop_button_ == nullptr) {
    return;
  }
  // Only show controls on the cloud/hybrid Preview page when a job is active.
  const bool job_active = snapshot.lifecycle == PrintLifecycleState::kPrinting ||
                          snapshot.lifecycle == PrintLifecycleState::kPaused ||
                          snapshot.lifecycle == PrintLifecycleState::kPreparing;
  const bool show = snapshot.preview_page_available && job_active;
  set_hidden(page2_pause_button_, !show);
  set_hidden(page2_stop_button_, !show);
  if (!show) {
    return;
  }

  // Pause button glyph follows lifecycle: paused -> play (resume), else pause.
  const bool show_play_glyph = snapshot.lifecycle == PrintLifecycleState::kPaused;
  set_label_text_if_changed(page2_pause_button_label_,
                            show_play_glyph ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);

  // Visually disable both buttons while a command is in flight (avoid double-fire
  // until the printer reflects the new lifecycle in its next status report).
  const bool pending = snapshot.print_command_pending_kind != PrintCommand::kNone;
  const lv_opa_t pause_opa = pending ? LV_OPA_40 : LV_OPA_90;
  lv_obj_set_style_bg_opa(page2_pause_button_, pause_opa, 0);
  lv_obj_set_style_bg_opa(page2_stop_button_, pause_opa, 0);
}

void PrinterPlugin::pause_button_event_cb(lv_event_t* event) {
  auto* plugin = static_cast<PrinterPlugin*>(lv_event_get_user_data(event));
  if (plugin != nullptr) {
    plugin->handle_pause_button_event(event);
  }
}

void PrinterPlugin::stop_button_event_cb(lv_event_t* event) {
  auto* plugin = static_cast<PrinterPlugin*>(lv_event_get_user_data(event));
  if (plugin != nullptr) {
    plugin->handle_stop_button_event(event);
  }
}

void PrinterPlugin::handle_pause_button_event(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }
  if (ui_->is_page_transition_active()) {
    return;
  }
  // Translate the tap based on the current lifecycle: Printing -> pause,
  // Paused -> resume. Preparing/Idle/Finished/Error don't expose the button.
  PrintCommand command = PrintCommand::kNone;
  switch (last_snapshot_.lifecycle) {
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPreparing:
      command = PrintCommand::kPause;
      break;
    case PrintLifecycleState::kPaused:
      command = PrintCommand::kResume;
      break;
    default:
      return;
  }
  ui_->note_activity(false);
  ui_->request_print_command(command);
}

void PrinterPlugin::handle_stop_button_event(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_LONG_PRESSED) {
    return;
  }
  if (ui_->is_page_transition_active()) {
    return;
  }
  // Stop is only meaningful while a job exists (printing / paused / preparing).
  switch (last_snapshot_.lifecycle) {
    case PrintLifecycleState::kPrinting:
    case PrintLifecycleState::kPaused:
    case PrintLifecycleState::kPreparing:
      break;
    default:
      return;
  }
  ui_->note_activity(false);
  ui_->request_print_command(PrintCommand::kStop);
}

void PrinterPlugin::remaining_row_event_cb(lv_event_t* event) {
  auto* plugin = static_cast<PrinterPlugin*>(lv_event_get_user_data(event));
  if (plugin != nullptr) {
    plugin->handle_remaining_row_click();
  }
}

void PrinterPlugin::handle_remaining_row_click() {
  if (ui_->is_page_transition_active()) {
    return;
  }
  // We are inside an LVGL event — the lvgl_port task already holds the
  // display lock, so no extra locking is needed before mutating widgets.
  show_eta_ = !show_eta_;
  // Re-apply the cached snapshot so the row updates immediately.
  apply_snapshot_locked(last_snapshot_, false);
}

void PrinterPlugin::logo_event_cb(lv_event_t* event) {
  auto* plugin = static_cast<PrinterPlugin*>(lv_event_get_user_data(event));
  if (plugin != nullptr) {
    plugin->handle_logo_event(event);
  }
}

void PrinterPlugin::handle_logo_event(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }

  if (ui_->is_page_transition_active() || !show_logo_ || !last_snapshot_.chamber_light_supported) {
    return;
  }

  ui_->note_activity(false);
  ui_->request_chamber_light_toggle();
}

}  // namespace infohub
