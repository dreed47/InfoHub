#pragma once

#include <cstdint>
#include <string>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "infohub/plugins/printer/bambu_cloud_client.hpp"
#include "infohub/config_store.hpp"
#include "infohub/mqtt_telemetry.hpp"

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

// Bambu Cloud/local status presentation helpers — printer-domain, but also
// consumed by SetupPortal's still-core handle_root/handle_health/
// handle_config_get, so they stay physically in setup_portal.cpp rather than
// moving to the printer plugin outright.
bool cloud_portal_ready(const BambuCloudSnapshot& snapshot);
bool cloud_detail_is_transitional(const std::string& detail);
void append_cloud_status_fields(std::string* body, const BambuCloudSnapshot& cloud);
void append_local_status_fields(std::string* body, const PrinterSnapshot& local,
                                 bool local_configured);
void append_mqtt_telemetry_fields(std::string* body, const MqttTelemetry& local,
                                   const MqttTelemetry& cloud);

// Millisecond monotonic clock, shared between SetupPortal's own handlers and
// plugin-owned ones (e.g. append_mqtt_telemetry_fields' seconds-ago math).
uint64_t now_ms();

}  // namespace infohub
