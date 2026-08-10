#include "printsphere/printer_plugin.hpp"

#include "printsphere/application.hpp"

namespace printsphere {

esp_err_t PrinterPlugin::init(PluginContext& ctx) { return app_.printer_plugin_init(ctx); }

void PrinterPlugin::tick(uint64_t now_ms) { app_.printer_plugin_tick(now_ms); }

void PrinterPlugin::update_ui() { app_.printer_plugin_update_ui(); }

bool PrinterPlugin::wants_network() const { return app_.printer_plugin_wants_network(); }

bool PrinterPlugin::wants_awake() const { return app_.printer_plugin_wants_awake(); }

uint32_t PrinterPlugin::desired_poll_interval_ms() const {
  return app_.printer_plugin_desired_poll_interval_ms();
}

}  // namespace printsphere
