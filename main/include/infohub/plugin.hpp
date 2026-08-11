#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"
#include "lvgl.h"
#include "infohub/config_store.hpp"
#include "infohub/wifi_manager.hpp"

namespace infohub {

class Ui;
class SetupPortal;
class PmuManager;
class AudioNotifier;

// Maximum number of plugins compiled into one firmware image. Each plugin is
// a compile-time unit (own Kconfig toggle + source files), not a
// dynamically-loaded module — ESP32-S3 has no OS-level dynamic loading. See
// the "Plugin interface sketch" in CLAUDE.md for the full rationale.
static constexpr uint8_t kMaxPlugins = 8;

// Core services a plugin may need. Grown from the original 3-field sketch
// once PrinterPlugin (Phase 8a) needed real access to portal PIN state,
// battery sampling and audio playback — SetupPortal/PmuManager/AudioNotifier
// stay core (owned by Application), plugins only borrow references.
struct PluginContext {
  ConfigStore& config_store;
  WifiManager& wifi_manager;  // shared net stack; a plugin does not own its own Wi-Fi
  Ui& ui;
  SetupPortal& setup_portal;
  PmuManager& pmu_manager;
  AudioNotifier& audio_notifier;
};

// Base class every plugin implements. Phase 8a: PrinterPlugin is now the
// first real implementation (owns its clients, drives its own tick/update_ui
// logic) — see the "Phased extraction sequencing plan" in CLAUDE.md.
class Plugin {
 public:
  virtual ~Plugin() = default;

  // e.g. "printer", "weather" — also doubles as the ConfigStore NVS
  // namespace for this plugin's own settings, so must be <= 15 chars (see
  // ConfigStore::load_plugin_string).
  virtual const char* id() const = 0;
  virtual const char* display_name() const = 0;

  virtual esp_err_t init(PluginContext& ctx) = 0;

  // Called once per Application main-loop iteration. Must not block — any
  // network I/O belongs on the plugin's own worker task, polled from here via
  // atomics/snapshots, the same discipline PrinterClient/BambuCloudClient
  // already follow. One slow tick() stalls every other plugin's UI update for
  // that iteration.
  virtual void tick(uint64_t now_ms) = 0;

  // Arbitration/power-save hints Application aggregates across all plugins.
  virtual bool wants_network() const = 0;                  // hint for handshake scheduling
                                                             // (see NetworkArbiter)
  virtual bool wants_awake() const = 0;                     // true suppresses screen dimming/off
  virtual uint32_t desired_poll_interval_ms() const = 0;    // Application takes the min
                                                             // across all plugins

  // UI: home-screen tile + full screen(s), LVGL objects built lazily.
  virtual void build_tile(lv_obj_t* parent) = 0;
  virtual void build_screen(lv_obj_t* parent) = 0;
  virtual void update_ui() = 0;  // pull latest state, push to widgets

  // Portal: settings page fragment + API handlers, namespaced under
  // /api/plugins/<id>/*. Handlers must call SetupPortal's
  // is_request_authorized() themselves to honor the PIN/session lock.
  virtual void register_portal_routes(httpd_handle_t server) = 0;
  virtual std::string portal_settings_html() const = 0;

  // Config: plugin owns its own NVS namespace via ConfigStore's per-plugin
  // accessors (load_plugin_string/save_plugin_indexed/etc.), not a shared
  // struct declared in ConfigStore itself.
  virtual void load_config() = 0;
  virtual void save_config() = 0;

  // Runtime enable/disable (Phase 7), independent of the Kconfig toggle that
  // controls whether a plugin is compiled in at all. Concrete, not virtual —
  // every plugin gets this for free. Application's tick()/update_ui()/
  // poll-interval loops skip a plugin while disabled; init() and
  // build_screen() still run unconditionally (objects/tasks built eagerly
  // either way, same pattern as AMS pages). Defaults to true so a plugin with
  // no portal toggle (e.g. PrinterPlugin today) behaves exactly as before.
  bool enabled() const { return enabled_; }
  void set_enabled(bool enabled) { enabled_ = enabled; }

  // True once this plugin's own data source is set up enough that Web
  // Config's PIN lock is allowed to engage — e.g. printer: a printer is
  // actually connected; weather: station ID + token are saved. Default true
  // (a plugin with no "needs setup" concept never blocks provisioning).
  // SetupPortal only consults this for enabled plugins — a disabled plugin
  // never blocks provisioning regardless of its own configured state.
  virtual bool is_configured() const { return true; }

 protected:
  bool enabled_ = true;
};

}  // namespace infohub
