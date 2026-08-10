#pragma once

#include "printsphere/plugin.hpp"

namespace printsphere {

class Application;

// Phase 5 of the plugin-architecture extraction (see "Phased extraction
// sequencing plan" in CLAUDE.md): thin adapter proving Application::run()
// can drive init()/tick()/update_ui() through the Plugin interface with one
// plugin. All printer logic still lives on Application as private methods
// (printer_plugin_init/tick/update_ui) — this class only forwards. No files
// move and no behavior changes until Phase 8.
class PrinterPlugin : public Plugin {
 public:
  explicit PrinterPlugin(Application& app) : app_(app) {}

  const char* id() const override { return "printer"; }
  const char* display_name() const override { return "Printer"; }

  esp_err_t init(PluginContext& ctx) override;
  void tick(uint64_t now_ms) override;
  void update_ui() override;

  bool wants_network() const override;
  bool wants_awake() const override;
  uint32_t desired_poll_interval_ms() const override;

  // UI/portal/config wiring is Phase 6/7/1-follow-up territory — no-ops for
  // now, Ui/SetupPortal don't call through the Plugin interface yet.
  void build_tile(lv_obj_t*) override {}
  void build_screen(lv_obj_t*) override {}
  void register_portal_routes(httpd_handle_t) override {}
  std::string portal_settings_html() const override { return {}; }
  void load_config() override {}
  void save_config() override {}

 private:
  Application& app_;
};

}  // namespace printsphere
