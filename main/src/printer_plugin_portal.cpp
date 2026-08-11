// Phase 8d (plugin-architecture extraction, see CLAUDE.md): the printer-domain
// portal routes formerly on SetupPortal now live here, registered through
// PrinterPlugin::register_portal_routes(). Split into its own TU (rather than
// printer_plugin.cpp) to keep tick()/init()/update_ui() separate from HTTP
// handler code. Generic JSON/HTTP helpers and the Bambu Cloud status-field
// helpers still shared with SetupPortal's own handlers (handle_root/
// handle_health/handle_config_get) come from portal_shared.hpp — see that
// header for why they stay physically defined in setup_portal.cpp.

#include "printsphere/printer_plugin.hpp"

#include <cctype>
#include <string>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "printsphere/config_store.hpp"
#include "printsphere/portal_shared.hpp"
#include "printsphere/setup_portal.hpp"
#include "printsphere/ui.hpp"

namespace printsphere {

namespace {

constexpr char kTag[] = "printsphere.printer";

BambuCloudCredentials merge_cloud_credentials(BambuCloudCredentials submitted,
                                              const BambuCloudCredentials& stored) {
  if (submitted.password.empty() && !submitted.email.empty() && submitted.email == stored.email) {
    submitted.password = stored.password;
  }
  return submitted;
}

PrinterConnection merge_printer_connection(PrinterConnection submitted,
                                           const PrinterConnection& stored) {
  if (submitted.access_code.empty() && !submitted.serial.empty() &&
      submitted.serial == stored.serial) {
    submitted.access_code = stored.access_code;
  }
  return submitted;
}

bool can_reuse_cloud_session(const BambuCloudCredentials& submitted,
                             const BambuCloudCredentials& stored,
                             const std::string& access_token) {
  return !access_token.empty() && !submitted.email.empty() && submitted.email == stored.email &&
         submitted.region == stored.region;
}

bool is_valid_ipv4(const std::string& host) {
  int dots = 0;
  int octet_value = 0;
  int octet_digits = 0;

  for (size_t i = 0; i < host.size(); ++i) {
    const char ch = host[i];
    if (ch == '.') {
      if (octet_digits == 0 || octet_value > 255) {
        return false;
      }
      ++dots;
      octet_value = 0;
      octet_digits = 0;
      continue;
    }

    if (ch < '0' || ch > '9') {
      return false;
    }

    octet_value = (octet_value * 10) + (ch - '0');
    ++octet_digits;
    if (octet_digits > 3) {
      return false;
    }
  }

  return dots == 3 && octet_digits > 0 && octet_value <= 255;
}

bool is_valid_hostname(const std::string& host) {
  if (host.empty() || host.size() > 253) {
    return false;
  }

  bool saw_alpha = false;
  size_t label_len = 0;
  char prev = '\0';
  for (const char ch : host) {
    const bool is_alpha = std::isalpha(static_cast<unsigned char>(ch)) != 0;
    const bool is_digit = std::isdigit(static_cast<unsigned char>(ch)) != 0;
    if (is_alpha) {
      saw_alpha = true;
    }

    if (is_alpha || is_digit || ch == '-') {
      ++label_len;
      if (label_len > 63) {
        return false;
      }
    } else if (ch == '.') {
      if (label_len == 0 || prev == '-') {
        return false;
      }
      label_len = 0;
    } else {
      return false;
    }

    prev = ch;
  }

  return label_len > 0 && prev != '-' && saw_alpha;
}

bool is_valid_printer_host(const std::string& host) {
  return is_valid_ipv4(host) || is_valid_hostname(host);
}

SourceMode parse_source_mode_field(const cJSON* root) {
  if (root == nullptr) {
    return SourceMode::kHybrid;
  }

  std::string value = trim_copy(read_string_field(root, "source_mode"));
  if (value.empty()) {
    value = trim_copy(read_string_field(root, "state_source"));
  }
  return parse_source_mode(value);
}

bool cloud_stage_is_user_visible_progress(CloudSetupStage stage) {
  switch (stage) {
    case CloudSetupStage::kEmailCodeRequired:
    case CloudSetupStage::kTfaRequired:
    case CloudSetupStage::kBindingPrinter:
    case CloudSetupStage::kConnectingMqtt:
    case CloudSetupStage::kConnected:
    case CloudSetupStage::kFailed:
      return true;
    case CloudSetupStage::kIdle:
    case CloudSetupStage::kLoggingIn:
    case CloudSetupStage::kCodeSubmitted:
    default:
      return false;
  }
}

bool cloud_connect_result_ready(const BambuCloudSnapshot& before,
                                const BambuCloudSnapshot& current) {
  if (!cloud_portal_ready(before) && cloud_portal_ready(current)) {
    return true;
  }
  if ((!before.verification_required && current.verification_required) ||
      (!before.tfa_required && current.tfa_required)) {
    return true;
  }
  if (cloud_stage_is_user_visible_progress(current.setup_stage) &&
      current.setup_stage != before.setup_stage) {
    return true;
  }

  if (current.configured != before.configured ||
      current.resolved_serial != before.resolved_serial) {
    return true;
  }

  if (current.detail == before.detail) {
    return false;
  }

  return !cloud_detail_is_transitional(current.detail);
}

bool cloud_verify_result_ready(const BambuCloudSnapshot& before,
                               const BambuCloudSnapshot& current) {
  if (!cloud_portal_ready(before) && cloud_portal_ready(current)) {
    return true;
  }
  if ((!before.verification_required && current.verification_required) ||
      (!before.tfa_required && current.tfa_required)) {
    return true;
  }
  if (cloud_stage_is_user_visible_progress(current.setup_stage) &&
      current.setup_stage != before.setup_stage) {
    return true;
  }
  if (current.resolved_serial != before.resolved_serial) {
    return true;
  }
  if (current.detail == before.detail) {
    return false;
  }
  if (current.setup_stage == CloudSetupStage::kFailed) {
    return true;
  }
  return !cloud_detail_is_transitional(current.detail) &&
         current.setup_stage != CloudSetupStage::kCodeSubmitted &&
         current.setup_stage != CloudSetupStage::kLoggingIn;
}

bool cloud_login_still_pending(const BambuCloudSnapshot& snapshot) {
  if (!snapshot.configured || cloud_portal_ready(snapshot) || snapshot.verification_required ||
      snapshot.tfa_required) {
    return false;
  }

  return snapshot.setup_stage != CloudSetupStage::kFailed;
}

}  // namespace

esp_err_t PrinterPlugin::handle_arc_preview(httpd_req_t* request) {
  return handle_arc_update(request, false);
}

esp_err_t PrinterPlugin::handle_arc_commit(httpd_req_t* request) {
  return handle_arc_update(request, true);
}

esp_err_t PrinterPlugin::handle_arc_update(httpd_req_t* request, bool persist) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  ArcColorScheme arc_colors = plugin->config_store_->load_arc_color_scheme();
  const bool colors_valid = parse_arc_colors_from_json(root, &arc_colors);
  cJSON_Delete(root);

  if (!colors_valid) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid arc color value");
  }

  plugin->ui_->set_arc_color_scheme(arc_colors);
  plugin->apply_ring_visual();
  if (persist) {
    const esp_err_t save_err = plugin->config_store_->save_arc_color_scheme(arc_colors);
    if (save_err != ESP_OK) {
      ESP_LOGE(kTag, "save live arc colors failed: %s", esp_err_to_name(save_err));
      return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "save live arc colors failed");
    }
  }

  std::string body = "{\"status\":\"";
  body += persist ? "saved" : "previewed";
  body += "\",\"persisted\":";
  body += persist ? "true" : "false";
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_source_mode_post(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving source mode only: %s", to_string(source_mode));
  ESP_RETURN_ON_ERROR(plugin->config_store_->save_source_mode(source_mode), kTag,
                      "save source mode failed");

  plugin->setup_portal_->request_reboot();

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_ams_display_post(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const bool filament_wake = read_bool_field(root, "filament_wake",
      plugin->config_store_->load_filament_wake_enabled());
  const bool filament_anim = read_bool_field(root, "filament_anim",
      plugin->config_store_->load_filament_anim_enabled());
  cJSON_Delete(root);

  ESP_LOGI(kTag, "Saving AMS display: wake=%d anim=%d",
           filament_wake, filament_anim);
  ESP_RETURN_ON_ERROR(plugin->config_store_->save_filament_wake_enabled(filament_wake), kTag,
                      "save filament wake failed");
  ESP_RETURN_ON_ERROR(plugin->config_store_->save_filament_anim_enabled(filament_anim), kTag,
                      "save filament anim failed");

  plugin->setup_portal_->request_reboot();

  send_json(request, "{\"status\":\"saved\",\"rebooting\":true}");
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_cloud_connect(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const BambuCloudCredentials stored_cloud = plugin->config_store_->load_cloud_credentials();
  const std::string stored_cloud_access_token = plugin->config_store_->load_cloud_access_token();
  const BambuCloudCredentials cloud = merge_cloud_credentials({
      .email = trim_copy(read_string_field(root, "cloud_email")),
      .password = read_string_field(root, "cloud_password"),
      .region = parse_cloud_region(trim_copy(read_string_field(root, "cloud_region"))),
  }, stored_cloud);
  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  if (!cloud.is_configured() &&
      !can_reuse_cloud_session(cloud, stored_cloud, stored_cloud_access_token)) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Cloud credentials incomplete\",\"detail\":\"Enter both Bambu email and password first.\"}");
    return ESP_OK;
  }

  if (!plugin->wifi_manager_->is_station_connected()) {
    httpd_resp_set_status(request, "409 Conflict");
    send_json(request,
              "{\"error\":\"Wi-Fi not ready\",\"detail\":\"Save Wi-Fi and reboot first. Cloud login starts after the ESP is on your home network.\"}");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(plugin->config_store_->save_cloud_credentials(cloud), kTag, "save cloud failed");
  ESP_RETURN_ON_ERROR(plugin->config_store_->save_source_mode(source_mode), kTag,
                      "save source mode failed");
  if (source_mode == SourceMode::kLocalOnly) {
    send_json(
        request,
        "{\"status\":\"saved\",\"detail\":\"Cloud credentials saved. Switch source mode to Hybrid or Cloud only to connect.\",\"cloud_connected\":false,\"cloud_verification_required\":false,\"cloud_tfa_required\":false,\"cloud_configured\":true,\"cloud_detail\":\"Cloud credentials saved. Switch source mode to Hybrid or Cloud only to connect.\",\"cloud_resolved_serial\":\"\"}");
    return ESP_OK;
  }
  plugin->cloud_client_.request_reload_from_store();

  const BambuCloudSnapshot before = plugin->cloud_client_.snapshot();
  BambuCloudSnapshot current = before;
  for (int attempt = 0; attempt < 60; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = plugin->cloud_client_.refreshed_snapshot();
    if (cloud_connect_result_ready(before, current)) {
      break;
    }
  }

  const bool login_still_pending = cloud_login_still_pending(current);
  if (current.setup_stage == CloudSetupStage::kFailed) {
    httpd_resp_set_status(request, "502 Bad Gateway");
  }

  std::string body = "{\"status\":\"";
  if (cloud_portal_ready(current)) {
    body += "connected";
  } else if (current.verification_required) {
    body += "verification_required";
  } else if (current.setup_stage == CloudSetupStage::kFailed) {
    body += "failed";
  } else if (login_still_pending) {
    body += "queued";
  } else {
    body += "saved";
  }
  body += "\",\"detail\":\"";
  body += json_escape(current.detail);
  body += "\"";
  append_cloud_status_fields(&body, current);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_cloud_verify(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const std::string code = trim_copy(read_string_field(root, "code"));
  cJSON_Delete(root);
  if (code.empty()) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "verification code missing");
  }

  BambuCloudSnapshot snapshot = plugin->cloud_client_.snapshot();
  const std::string previous_detail = snapshot.detail;
  const std::string previous_resolved_serial = snapshot.resolved_serial;
  const bool previous_connected = snapshot.connected;
  const bool previous_session_connected = snapshot.session_connected;
  const bool previous_verification_required = snapshot.verification_required;
  const bool previous_configured = snapshot.configured;
  const CloudSetupStage previous_setup_stage = snapshot.setup_stage;
  BambuCloudSnapshot previous_snapshot;
  previous_snapshot.configured = previous_configured;
  previous_snapshot.connected = previous_connected;
  previous_snapshot.session_connected = previous_session_connected;
  previous_snapshot.setup_stage = previous_setup_stage;
  previous_snapshot.detail = previous_detail;
  previous_snapshot.resolved_serial = previous_resolved_serial;
  previous_snapshot.verification_required = previous_verification_required;

  plugin->cloud_client_.submit_verification_code(code);
  for (int attempt = 0; attempt < 80; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    snapshot = plugin->cloud_client_.refreshed_snapshot();
    if (cloud_verify_result_ready(previous_snapshot, snapshot)) {
      break;
    }
  }

  const bool login_still_pending = cloud_login_still_pending(snapshot);
  if (snapshot.verification_required) {
    httpd_resp_set_status(request, "409 Conflict");
  } else if (snapshot.setup_stage == CloudSetupStage::kFailed ||
             (!cloud_portal_ready(snapshot) && !login_still_pending)) {
    httpd_resp_set_status(request, "502 Bad Gateway");
  }

  std::string body = "{\"status\":\"";
  if (cloud_portal_ready(snapshot)) {
    body += "connected";
  } else if (snapshot.verification_required) {
    body += "verification_required";
  } else if (login_still_pending) {
    body += "queued";
  } else {
    body += "failed";
  }
  body += "\",\"detail\":\"";
  body += json_escape(snapshot.detail);
  body += "\"";
  append_cloud_status_fields(&body, snapshot);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_local_connect(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) {
    return parse_err;
  }

  const PrinterConnection stored_printer = plugin->config_store_->load_active_printer_profile().to_connection();
  const PrinterConnection printer = merge_printer_connection({
      .host = trim_copy(read_string_field(root, "printer_host")),
      .serial = trim_copy(read_string_field(root, "printer_serial")),
      .access_code = trim_copy(read_string_field(root, "printer_access_code")),
  }, stored_printer);
  const SourceMode source_mode = parse_source_mode_field(root);
  cJSON_Delete(root);

  if (!printer.is_ready()) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Local printer fields incomplete\",\"detail\":\"Enter printer host, serial and access code first.\"}");
    return ESP_OK;
  }
  if (!is_valid_printer_host(printer.host)) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request,
              "{\"error\":\"Invalid printer host\",\"detail\":\"Printer host must be a full IPv4 address or hostname.\"}");
    return ESP_OK;
  }
  if (!plugin->wifi_manager_->is_station_connected()) {
    httpd_resp_set_status(request, "409 Conflict");
    send_json(request,
              "{\"error\":\"Wi-Fi not ready\",\"detail\":\"Save Wi-Fi and reboot first. The local path starts after the ESP is on your home network.\"}");
    return ESP_OK;
  }

  ESP_RETURN_ON_ERROR(plugin->config_store_->save_source_mode(source_mode), kTag,
                      "save source mode failed");

  // Upsert into multi-profile system
  {
    auto profiles = plugin->config_store_->load_printer_profiles();
    PrinterProfile profile;
    bool found = false;
    for (const auto& p : profiles) {
      if (p.serial == printer.serial) { profile = p; found = true; break; }
    }
    if (!found) {
      profile.index = static_cast<uint8_t>(profiles.size());
    }
    profile.serial = printer.serial;
    profile.host = printer.host;
    profile.access_code = printer.access_code;
    if (profile.display_name.empty() || profile.model.empty() || !profile.cloud_bound) {
      // Try to resolve model name from cloud device list
      const auto cloud_devs = plugin->cloud_client_.get_cloud_devices();
      for (const auto& cd : cloud_devs) {
        if (cd.serial == printer.serial) {
          if (profile.display_name.empty()) {
            profile.display_name = !cd.display_name.empty() ? cd.display_name : to_string(cd.model);
          }
          if (profile.model.empty()) {
            profile.model = to_string(cd.model);
          }
          profile.cloud_bound = true;
          break;
        }
      }
    }
    if (profile.index < kMaxPrinterProfiles) {
      plugin->config_store_->save_printer_profile(profile);
      plugin->config_store_->save_active_printer_index(profile.index);
    }
  }
  if (source_mode == SourceMode::kCloudOnly) {
    send_json(
        request,
        "{\"status\":\"saved\",\"detail\":\"Local printer credentials saved. Switch source mode to Hybrid or Local only to connect.\",\"local_error\":false,\"local_connected\":false,\"local_configured\":true,\"local_detail\":\"Local printer credentials saved. Switch source mode to Hybrid or Local only to connect.\"}");
    return ESP_OK;
  }

  const PrinterSnapshot before = plugin->printer_client_.snapshot();
  plugin->printer_client_.configure(printer);
  plugin->camera_client_.configure(printer);

  PrinterSnapshot current = before;
  for (int attempt = 0; attempt < 80; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(100));
    current = plugin->printer_client_.snapshot();
    if (current.connection == PrinterConnectionState::kOnline ||
        current.connection == PrinterConnectionState::kError ||
        current.local_configured != before.local_configured ||
        current.detail != before.detail || current.stage != before.stage ||
        current.resolved_serial != before.resolved_serial) {
      break;
    }
  }

  const bool local_pending = current.local_configured &&
                             current.connection != PrinterConnectionState::kOnline &&
                             current.connection != PrinterConnectionState::kError;
  if (current.connection == PrinterConnectionState::kError) {
    httpd_resp_set_status(request, "502 Bad Gateway");
  }

  std::string body = "{\"status\":\"";
  if (current.connection == PrinterConnectionState::kOnline) {
    body += "connected";
  } else if (current.connection == PrinterConnectionState::kError) {
    body += "error";
  } else if (local_pending) {
    body += "queued";
  } else {
    body += "saved";
  }
  body += "\",\"detail\":\"";
  body += json_escape(current.detail);
  body += "\"";
  append_local_status_fields(&body, current, true);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Printer profile REST endpoints
// ---------------------------------------------------------------------------

esp_err_t PrinterPlugin::handle_printers_get(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) return ESP_FAIL;
  if (!plugin->setup_portal_->is_request_authorized(request)) return plugin->setup_portal_->send_locked_response(request);

  const auto profiles = plugin->config_store_->load_printer_profiles();
  const uint8_t active_idx = plugin->config_store_->load_active_printer_index();
  const auto cloud_devices = plugin->cloud_client_.get_cloud_devices();

  std::string body = "{\"active\":";
  body += std::to_string(active_idx);
  body += ",\"profiles\":[";
  for (size_t i = 0; i < profiles.size(); ++i) {
    if (i > 0) body += ",";
    const auto& p = profiles[i];
    body += "{\"index\":";
    body += std::to_string(p.index);
    body += ",\"serial\":\"";
    body += json_escape(p.serial);
    body += "\",\"host\":\"";
    body += json_escape(p.host);
    body += "\",\"display_name\":\"";
    body += json_escape(p.display_name);
    body += "\",\"model\":\"";
    body += json_escape(p.model);
    body += "\",\"has_local\":";
    body += p.has_local_config() ? "true" : "false";
    body += ",\"cloud_bound\":";
    body += p.cloud_bound ? "true" : "false";
    body += "}";
  }
  body += "],\"cloud_devices\":[";
  for (size_t i = 0; i < cloud_devices.size(); ++i) {
    if (i > 0) body += ",";
    const auto& cd = cloud_devices[i];
    body += "{\"serial\":\"";
    body += json_escape(cd.serial);
    body += "\",\"display_name\":\"";
    body += json_escape(cd.display_name);
    body += "\",\"model\":\"";
    body += to_string(cd.model);
    body += "\",\"online\":";
    body += cd.online ? "true" : "false";
    body += "}";
  }
  body += "]}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_printers_select(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) return ESP_FAIL;
  if (!plugin->setup_portal_->is_request_authorized(request)) return plugin->setup_portal_->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const cJSON* index_item = cJSON_GetObjectItem(root, "index");
  if (!cJSON_IsNumber(index_item)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "index field required");
  }
  const uint8_t new_index = static_cast<uint8_t>(cJSON_GetNumberValue(index_item));
  cJSON_Delete(root);

  const auto profiles = plugin->config_store_->load_printer_profiles();
  const PrinterProfile* selected = nullptr;
  for (const auto& p : profiles) {
    if (p.index == new_index) { selected = &p; break; }
  }
  if (selected == nullptr) {
    httpd_resp_set_status(request, "404 Not Found");
    send_json(request, "{\"error\":\"Profile not found\"}");
    return ESP_OK;
  }

  plugin->config_store_->save_active_printer_index(new_index);

  // Live-reconnect all clients
  const PrinterConnection conn = selected->to_connection();
  if (conn.is_ready()) {
    plugin->printer_client_.configure(conn);
    plugin->camera_client_.configure(conn);
  }
  const BambuCloudCredentials cloud_creds = plugin->config_store_->load_cloud_credentials();
  plugin->cloud_client_.configure(cloud_creds, selected->serial);

  std::string body = "{\"status\":\"ok\",\"printer\":\"";
  body += json_escape(selected->display_name.empty() ? selected->serial : selected->display_name);
  body += "\"}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_printers_save(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) return ESP_FAIL;
  if (!plugin->setup_portal_->is_request_authorized(request)) return plugin->setup_portal_->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const std::string serial = trim_copy(read_string_field(root, "serial"));
  const std::string host = trim_copy(read_string_field(root, "host"));
  const std::string access_code = trim_copy(read_string_field(root, "access_code"));
  const std::string display_name = trim_copy(read_string_field(root, "display_name"));
  const std::string model = trim_copy(read_string_field(root, "model"));
  const cJSON* cloud_bound_item = cJSON_GetObjectItem(root, "cloud_bound");
  const bool cloud_bound_explicit = cJSON_IsBool(cloud_bound_item) && cJSON_IsTrue(cloud_bound_item);
  cJSON_Delete(root);

  if (serial.empty()) {
    httpd_resp_set_status(request, "400 Bad Request");
    send_json(request, "{\"error\":\"Serial number is required\"}");
    return ESP_OK;
  }

  // Check if a profile with this serial already exists — update it
  auto profiles = plugin->config_store_->load_printer_profiles();
  PrinterProfile profile;
  bool found = false;
  for (const auto& p : profiles) {
    if (p.serial == serial) {
      profile = p;
      found = true;
      break;
    }
  }
  if (!found) {
    profile.index = static_cast<uint8_t>(profiles.size());
    if (profile.index >= kMaxPrinterProfiles) {
      httpd_resp_set_status(request, "507 Insufficient Storage");
      send_json(request, "{\"error\":\"Maximum number of printer profiles reached\"}");
      return ESP_OK;
    }
  }

  profile.serial = serial;
  if (!host.empty()) profile.host = host;
  if (!access_code.empty()) profile.access_code = access_code;
  if (!display_name.empty()) profile.display_name = display_name;
  if (!model.empty()) profile.model = model;
  if (cloud_bound_explicit) profile.cloud_bound = true;
  // Update cloud_bound from live cloud device list
  if (!profile.cloud_bound) {
    const auto cloud_devs = plugin->cloud_client_.get_cloud_devices();
    for (const auto& cd : cloud_devs) {
      if (cd.serial == serial) { profile.cloud_bound = true; break; }
    }
  }
  plugin->config_store_->save_printer_profile(profile);

  std::string body = "{\"status\":\"saved\",\"index\":";
  body += std::to_string(profile.index);
  body += "}";
  send_json(request, body);
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_printers_delete(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) return ESP_FAIL;
  if (!plugin->setup_portal_->is_request_authorized(request)) return plugin->setup_portal_->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const cJSON* index_item = cJSON_GetObjectItem(root, "index");
  if (!cJSON_IsNumber(index_item)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "index field required");
  }
  const uint8_t del_index = static_cast<uint8_t>(cJSON_GetNumberValue(index_item));
  cJSON_Delete(root);

  const uint8_t active_idx = plugin->config_store_->load_active_printer_index();
  const esp_err_t err = plugin->config_store_->delete_printer_profile(del_index);
  if (err != ESP_OK) {
    httpd_resp_set_status(request, "404 Not Found");
    send_json(request, "{\"error\":\"Profile not found\"}");
    return ESP_OK;
  }

  // If the deleted profile was the active one, clear legacy config and disconnect clients
  if (del_index == active_idx) {
    const PrinterConnection empty_conn;
    plugin->printer_client_.configure(empty_conn);
    plugin->camera_client_.configure(empty_conn);
    // Reconfigure cloud to drop the serial binding
    const BambuCloudCredentials cloud_creds = plugin->config_store_->load_cloud_credentials();
    plugin->cloud_client_.configure(cloud_creds, "");
    // Switch active to first remaining profile if any
    const auto remaining = plugin->config_store_->load_printer_profiles();
    if (!remaining.empty()) {
      plugin->config_store_->save_active_printer_index(remaining.front().index);
      const PrinterConnection new_conn = remaining.front().to_connection();
      if (new_conn.is_ready()) {
        plugin->printer_client_.configure(new_conn);
        plugin->camera_client_.configure(new_conn);
      }
      plugin->cloud_client_.configure(cloud_creds, remaining.front().serial);
    }
  }

  send_json(request, "{\"status\":\"deleted\"}");
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_printers_clear_local(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) return ESP_FAIL;
  if (!plugin->setup_portal_->is_request_authorized(request)) return plugin->setup_portal_->send_locked_response(request);

  cJSON* root = nullptr;
  esp_err_t parse_err = receive_json_body(request, &root);
  if (parse_err != ESP_OK) return parse_err;

  const cJSON* index_item = cJSON_GetObjectItem(root, "index");
  if (!cJSON_IsNumber(index_item)) {
    cJSON_Delete(root);
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "index field required");
  }
  const uint8_t idx = static_cast<uint8_t>(cJSON_GetNumberValue(index_item));
  cJSON_Delete(root);

  auto profiles = plugin->config_store_->load_printer_profiles();
  PrinterProfile* target = nullptr;
  for (auto& p : profiles) {
    if (p.index == idx) { target = &p; break; }
  }
  if (target == nullptr) {
    httpd_resp_set_status(request, "404 Not Found");
    send_json(request, "{\"error\":\"Profile not found\"}");
    return ESP_OK;
  }

  target->host.clear();
  target->access_code.clear();
  plugin->config_store_->save_printer_profile(*target);

  // If this is the active profile, disconnect local clients
  const uint8_t active_idx = plugin->config_store_->load_active_printer_index();
  if (idx == active_idx) {
    const PrinterConnection empty_conn;
    plugin->printer_client_.configure(empty_conn);
    plugin->camera_client_.configure(empty_conn);
  }

  send_json(request, "{\"status\":\"local_cleared\"}");
  return ESP_OK;
}

esp_err_t PrinterPlugin::handle_plugin_printer_config_get(httpd_req_t* request) {
  auto* plugin = static_cast<PrinterPlugin*>(request->user_ctx);
  if (plugin == nullptr) {
    return ESP_FAIL;
  }
  if (!plugin->setup_portal_->is_request_authorized(request)) {
    return plugin->setup_portal_->send_locked_response(request);
  }

  const BambuCloudCredentials cloud = plugin->config_store_->load_cloud_credentials();
  const SourceMode source_mode = plugin->config_store_->load_source_mode();
  const PrinterConnection printer =
      plugin->config_store_->load_active_printer_profile().to_connection();
  const BambuCloudSnapshot cloud_snapshot = plugin->cloud_snapshot();
  const std::string effective_printer_serial = [&]() -> std::string {
    if (!printer.serial.empty()) return printer.serial;
    const auto cloud_devs = plugin->cloud_devices();
    const auto profiles = plugin->config_store_->load_printer_profiles();
    for (const auto& cd : cloud_devs) {
      bool has_local = false;
      for (const auto& p : profiles) {
        if (p.serial == cd.serial && p.has_local_config()) { has_local = true; break; }
      }
      if (!has_local) return cd.serial;
    }
    return cloud_snapshot.resolved_serial;
  }();

  std::string body = "{";
  body += "\"cloud_email\":\"" + json_escape(cloud.email) + "\",";
  body += "\"cloud_region\":\"" + json_escape(to_string(cloud.region)) + "\",";
  body += "\"printer_host\":\"" + json_escape(printer.host) + "\",";
  body += "\"printer_serial\":\"" + json_escape(effective_printer_serial) + "\",";
  body += "\"source_mode\":\"";
  body += to_string(source_mode);
  body += "\",";
  body += "\"state_source\":\"";
  body += to_string(source_mode);
  body += "\"";
  const PrinterSnapshot local_snapshot = plugin->local_snapshot();
  append_cloud_status_fields(&body, cloud_snapshot);
  append_local_status_fields(&body, local_snapshot, plugin->local_configured());
  append_mqtt_telemetry_fields(&body, plugin->local_mqtt_telemetry(), plugin->cloud_mqtt_telemetry());
  body += "}";

  send_json(request, body);
  return ESP_OK;
}

void PrinterPlugin::register_portal_routes(httpd_handle_t server) {
  struct RouteEntry {
    const char* uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t*);
  };
  static constexpr RouteEntry kRoutes[] = {
      {"/api/arc/preview", HTTP_POST, &PrinterPlugin::handle_arc_preview},
      {"/api/arc/commit", HTTP_POST, &PrinterPlugin::handle_arc_commit},
      {"/api/source-mode", HTTP_POST, &PrinterPlugin::handle_source_mode_post},
      {"/api/ams-display", HTTP_POST, &PrinterPlugin::handle_ams_display_post},
      {"/api/cloud/connect", HTTP_POST, &PrinterPlugin::handle_cloud_connect},
      {"/api/cloud/verify", HTTP_POST, &PrinterPlugin::handle_cloud_verify},
      {"/api/local/connect", HTTP_POST, &PrinterPlugin::handle_local_connect},
      {"/api/printers", HTTP_GET, &PrinterPlugin::handle_printers_get},
      {"/api/printers/select", HTTP_POST, &PrinterPlugin::handle_printers_select},
      {"/api/printers/save", HTTP_POST, &PrinterPlugin::handle_printers_save},
      {"/api/printers/delete", HTTP_POST, &PrinterPlugin::handle_printers_delete},
      {"/api/printers/clear-local", HTTP_POST, &PrinterPlugin::handle_printers_clear_local},
      {"/api/plugins/printer/config", HTTP_GET, &PrinterPlugin::handle_plugin_printer_config_get},
  };

  for (const RouteEntry& route : kRoutes) {
    httpd_uri_t uri = {};
    uri.uri = route.uri;
    uri.method = route.method;
    uri.handler = route.handler;
    uri.user_ctx = this;
    const esp_err_t err = httpd_register_uri_handler(server, &uri);
    if (err != ESP_OK) {
      ESP_LOGE(kTag, "route '%s' handler failed: %s", route.uri, esp_err_to_name(err));
    }
  }
}

}  // namespace printsphere
