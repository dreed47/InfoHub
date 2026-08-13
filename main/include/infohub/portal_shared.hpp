#pragma once

#include <cstdint>
#include <string>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "infohub/config_store.hpp"
// Unconditional: BambuCloudSnapshot is a plain data struct (no MQTT/client
// behavior pulled in beyond what the mqtt component's headers already cost,
// which stay linkable regardless of CONFIG_INFOHUB_PLUGIN_PRINTER per
// CMakeLists.txt's REQUIRES comment) — needed below so CloudPortalPresentation
// type-checks even when the printer plugin is compiled out, since
// SetupPortal's handle_root always declares a CloudPortalPresentation (real
// or safe-default) regardless of Kconfig.
#include "infohub/plugins/printer/bambu_cloud_client.hpp"
#if CONFIG_INFOHUB_PLUGIN_PRINTER
#include "infohub/mqtt_telemetry.hpp"
#endif

namespace infohub {

// Generic JSON/HTTP helpers used by both SetupPortal's own handlers and
// plugin-owned portal routes (e.g. PrinterPlugin's, in
// printer_plugin_portal.cpp). Definitions stay in setup_portal.cpp — this
// header just gives them external linkage instead of anonymous-namespace
// (TU-private) linkage.
std::string json_escape(const std::string& input);
std::string read_string_field(const cJSON* object, const char* key);
bool read_bool_field(const cJSON* object, const char* key, bool fallback);
std::string trim_copy(const std::string& input);
void send_json(httpd_req_t* request, const std::string& body);
esp_err_t receive_json_body(httpd_req_t* request, cJSON** root_out);
bool parse_arc_colors_from_json(const cJSON* root, ArcColorScheme* colors);

// Plain data holder — ungated (unlike the functions below) so
// SetupPortal::handle_root can declare a safe-default CloudPortalPresentation
// in its `#else` (printer-disabled) branch without needing the printer-domain
// presentation logic that only makes sense when a real snapshot exists.
struct CloudPortalPresentation {
  BambuCloudSnapshot snapshot{};
  bool ready = false;
  std::string badge_value = "Not configured";
  const char* badge_class = "idle";
  std::string status_line = "Connect Bambu Cloud";
  std::string status_detail = "No cloud response yet";
};

#if CONFIG_INFOHUB_PLUGIN_PRINTER
// Bambu Cloud/local status presentation helpers — printer-domain. Defined in
// printer_plugin_portal.cpp; declared here (rather than in printer_plugin.hpp)
// because SetupPortal's own handle_root/handle_health still call these when
// the printer plugin is enabled, same shared-header convention as the
// generic JSON/HTTP helpers above.
CloudPortalPresentation cloud_portal_presentation(const BambuCloudSnapshot& cloud);
bool cloud_portal_ready(const BambuCloudSnapshot& snapshot);
bool cloud_detail_is_transitional(const std::string& detail);
void append_cloud_status_fields(std::string* body, const BambuCloudSnapshot& cloud);
void append_local_status_fields(std::string* body, const PrinterSnapshot& local,
                                 bool local_configured);
void append_mqtt_telemetry_fields(std::string* body, const MqttTelemetry& local,
                                   const MqttTelemetry& cloud);
#endif

// Millisecond monotonic clock, shared between SetupPortal's own handlers and
// plugin-owned ones (e.g. append_mqtt_telemetry_fields' seconds-ago math).
uint64_t now_ms();

}  // namespace infohub
