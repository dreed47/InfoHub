# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

InfoHub: ESP32-S3 firmware (C17/C++17, ESP-IDF v5.5.4, LVGL v9.5.0) for a round/square touch display that shows Bambu Lab 3D printer status. Talks to printers via Bambu Cloud, local MQTT, or both ("hybrid" mode). Two hardware variants share one codebase, selected at CMake configure time.

## Direction: generic plugin platform (goal, not yet built)

Long-term goal: turn InfoHub from single-purpose Bambu display into generic info-display platform. Printer status becomes one "plugin" among many (weather, stocks, other data sources), each plugged into shared display/config/network/portal scaffolding.

Current codebase (architecture below) is Bambu-specific and monolithic: `Application` hardcodes printer/cloud/camera clients as fixed members, `PrinterSnapshot`/`PrinterStateStore` is the one-and-only shared state contract, `Ui` renders printer screens directly, `SetupPortal` hardcodes printer/Wi-Fi/cloud config pages. None of this is decoupled into a plugin interface yet — treat the architecture section as "what exists today," not "what a plugin author targets."

When asked to work toward this goal: no code changes or design decisions without explicit direction from user first — this is a large refactor (state model, UI screen registration, config schema, portal pages, network client lifecycle all need a generic seam). Surface tradeoffs, don't just start restructuring.

### Plugin interface sketch (design only, not implemented)

ESP32-S3 has no OS-level dynamic loading, so "plugin" means compile-time unit (own `main/src/plugins/*.cpp` + Kconfig toggle), not a runtime-loaded `.so`. Interface = abstract base class + static registration table.

```cpp
// plugin.hpp
namespace infohub {

struct PluginContext {
  ConfigStore& config_store;
  WifiManager& wifi_manager;   // shared net, plugin doesn't own its own Wi-Fi
  Ui& ui;
};

class Plugin {
 public:
  virtual ~Plugin() = default;

  virtual const char* id() const = 0;          // "weather", "stocks", "printer"
  virtual const char* display_name() const = 0;

  virtual esp_err_t init(PluginContext& ctx) = 0;
  virtual void tick(uint64_t now_ms) = 0;       // called from Application loop, non-blocking
  virtual bool wants_network() const = 0;       // arbitration hint, like PrinterClient today

  // UI: home-screen tile + full screen, LVGL objects built lazily
  virtual void build_tile(lv_obj_t* parent) = 0;
  virtual void build_screen(lv_obj_t* parent) = 0;
  virtual void update_ui() = 0;                 // pull latest snapshot, push to widgets

  // Portal: settings page fragment + API handlers, namespaced under /api/plugins/<id>/*
  virtual void register_portal_routes(httpd_handle_t server) = 0;
  virtual std::string portal_settings_html() const = 0;

  // Config: plugin owns its own NVS/LittleFS namespace, no shared struct sprawl
  virtual void load_config() = 0;
  virtual void save_config() = 0;
};

}  // namespace infohub
```

Registration is compile-time, one file, list grows as plugins are added:

```cpp
// plugin_registry.cpp
std::array<Plugin*, kMaxPlugins> registered_plugins = {
#if CONFIG_INFOHUB_PLUGIN_PRINTER
  &g_printer_plugin,
#endif
#if CONFIG_INFOHUB_PLUGIN_WEATHER
  &g_weather_plugin,
#endif
};
```

Each plugin gated by its own Kconfig bool → controls flash/RAM budget per build, same pattern as `INFOHUB_EXPERIMENTAL_PRINT_CONTROL` today.

**What has to change to support this:**

- **`PrinterSnapshot`/`PrinterStateStore`** — currently the one shared state contract. Becomes printer-plugin-private; each plugin owns its own state struct + mutex-guarded store, same shape as `PrinterStateStore` but per plugin.
- **`Application`** — stops hardcoding members (`printer_client_`, `cloud_client_`, `camera_client_`...), instead owns `std::array<Plugin*, N> plugins_` and drives `init()`/`tick()` uniformly. Printer/cloud/camera/audio-notifier logic moves inside the printer plugin.
- **`Ui`** — home screen becomes a tile grid/carousel over `plugin->build_tile()`, not fixed printer pages. Navigation (`page1`/`page2`/camera page today) becomes per-plugin screen(s) registered generically.
- **`SetupPortal`** — root config page becomes a shell that iterates `register_portal_routes()` / `portal_settings_html()` per plugin instead of one big handler file. Wi-Fi/cloud-auth/PIN/OTA stay core (every plugin needs network+security), not plugin-owned.
- **`ConfigStore`** — move from fixed typed methods (`load_source_mode()` etc.) toward a per-plugin namespaced key-value API, so adding a plugin doesn't mean editing the core header. See sketch below — NVS's 15-char namespace/key limit rules out a naive dotted-key scheme.
- **Audio/error-lookup** — arguably core services any plugin can call (`audio_notifier.play(event)`), not printer-specific, if kept general enough.

**Open questions before any of this gets built:**

- Memory: LVGL objects + N plugins' state all resident — is lazy screen construction (build on nav, destroy on leave) required, or can multiple plugins' screens coexist?
- Does every plugin get its own network task, or do they share one poll loop with per-plugin timers (favors flash/RAM, matches current single-app-task model)?
- Tile/home-screen layout: fixed slots vs scrollable list — affects `Ui` more than `Plugin` interface itself.
- Migration path: printer becomes "just a plugin" — does existing `ConfigStore` NVS layout need a migration shim so existing devices don't lose settings on upgrade?

### ConfigStore changes sketch (design only, not implemented)

Grounded in current code: single NVS namespace `"infohub"`, flat abbreviated keys, `save_string`/`load_string` are already generic primitives underneath every typed getter. Indexed keys (`prn_%u_%s` + `prn_count`) already exist for multi-instance printer profiles. Audio blobs live on LittleFS at `/sounds`, not NVS.

**Hard constraint:** ESP-IDF NVS caps both namespace and key strings at 15 chars. A dotted-key scheme like `"plugin.weather.city"` breaks immediately. Fix: per-plugin NVS namespace (`nvs_open("psp_weather", ...)`) instead of key-prefixing — plugin id must fit the budget, enforced at plugin registration.

```cpp
class ConfigStore {
 public:
  // existing typed methods unchanged: wifi/cloud/display/battery/audio/color —
  // these are core, not plugin-owned, stay exactly as today.

  // NEW: generic scoped accessor for plugin-owned settings.
  // `plugin_ns` must be ≤ 15 chars (enforced at plugin registration, not here).
  std::string load_plugin_string(const char* plugin_ns, const char* key) const;
  esp_err_t   save_plugin_string(const char* plugin_ns, const char* key,
                                  const std::string& value) const;

  // NEW: generic indexed-record helpers, generalized from the
  // prn_%u_%s / prn_count pattern already used for PrinterProfile.
  // Plugin passes its own count/get/set of one string field at a time —
  // same shape as profile_key(), just not printer-specific.
  std::string load_plugin_indexed(const char* plugin_ns, uint8_t index,
                                   const char* suffix) const;
  esp_err_t   save_plugin_indexed(const char* plugin_ns, uint8_t index,
                                   const char* suffix, const std::string& value) const;
  uint8_t     load_plugin_record_count(const char* plugin_ns) const;
  esp_err_t   save_plugin_record_count(const char* plugin_ns, uint8_t count) const;

 private:
  // load_string/save_string become namespace-parameterized internally
  // instead of hardcoded to kNamespace — this is the actual internal change,
  // everything above is a thin wrapper over it.
  esp_err_t save_string_ns(const char* ns, const char* key, const std::string& value) const;
  std::string load_string_ns(const char* ns, const char* key) const;

  mutable std::mutex plugin_config_mutex_{};  // one lock for all plugin writes,
                                               // same role as printer_profile_mutex_
                                               // today but generalized instead of
                                               // per-feature
};
```

Plugins compose typed structs on top of this themselves (mirroring how `PrinterProfile`/`ArcColorScheme` are just structs built from flat string keys today) — `ConfigStore` doesn't need to know about `WeatherSettings` or `StockTicker`, same as it doesn't hardcode `PrinterProfile` parsing logic beyond the flat keys.

**Other pieces:**

- **`parse_bool_or_default`/`parse_color_or_default`** — currently anonymous-namespace private to `config_store.cpp`. Move to a small shared header (`config_parse.hpp`) so plugin code can reuse them instead of reimplementing bool/color/int parsing per plugin.
- **Blob storage** — audio PCM's `/sounds` LittleFS convention generalizes to a per-plugin subdirectory (e.g. `/data/<plugin_ns>/...`) if a plugin needs cached binary data (icon, forecast cache). Reuse the existing `fopen`/path-building pattern, just parameterize the directory instead of hardcoding `"/sounds"`.
- **Migration safety** — none of the existing keys (`wifi_ssid`, `cloud_email`, `prn_*`, `arc_*`, `bat_*`...) move. They stay exactly where they are in the core namespace so existing devices don't lose settings on upgrade. Only *new* plugin settings go through the generic API.
- **Concurrency** — today only printer profiles get a dedicated mutex because only they're written from both the HTTP portal task and the main loop. Once plugins can each register portal routes that write their own config, that race becomes general — one shared `plugin_config_mutex_` covering all `save_plugin_*` calls is simplest, rather than one mutex per plugin.

### Ui changes sketch (design only, not implemented)

`Ui` today is one monolithic class: fixed compile-time page sequence (`kPageIdxPrinterSelect`, `kPageIdxAmsFirst..Last`, `kPageIdxMain`, `kPageIdxPreview`, `kPageIdxCamera`, `kPageIdxCredits`), one `apply_snapshot(PrinterSnapshot)` entry point, and ~4k lines mixing two things that need to separate: **chrome** (pager/gesture mechanics, brightness/power-save policy, portal PIN overlay, rotation/tilt, logo) and **printer-specific content** (AMS pages, status arc, camera/preview pages, print buttons, printer-selector).

**Split `Ui` into a shell + per-plugin content, not one class:**

```cpp
// ui_shell.hpp — chrome only, replaces most of current ui.hpp
class UiShell {
 public:
  esp_err_t initialize(std::array<Plugin*, N>& plugins);  // builds pager + one
                                                            // page-group per plugin
  // Registered by each plugin at init, replaces fixed kPageIdx* constants:
  int register_pages(const char* plugin_id, int page_count);  // returns base index

  // Chrome that stays exactly as today, unrelated to any plugin's content:
  void set_display_rotation(DisplayRotation rotation);
  void set_display_tilt_deci_deg(int deci_deg);
  void update_power_save(bool on_battery, bool keep_awake, bool any_plugin_busy);
  void set_battery_display_policy(const BatteryDisplayPolicy& policy);
  ScreenPowerMode screen_power_mode() const;
  void request_wake_display();
  // portal PIN overlay, gesture/swipe, brightness dimming, logo — unchanged internals

  // Generic replacement for is_page2_active()/is_camera_page_active():
  bool is_plugin_page_active(const char* plugin_id, int local_page_idx) const;
};
```

Each plugin owns its own LVGL object members and widget-building code (today's `ams_pages_[4]`, `page2_pause_button_`, `camera_image_dscs_[2]`, etc. move out of `Ui` entirely into the printer plugin). `build_screen()`/`update_ui()` from the plugin interface become the plugin's replacement for `build_dashboard()`/`apply_snapshot()` — printer plugin's `update_ui()` does what `apply_snapshot(PrinterSnapshot)` does today, just scoped to its own pages.

**Specific couplings to break:**

- **Page indices** — `kPageIdxPrinterSelect..kPageIdxLast` are compile-time constants sized for exactly one printer-domain page set. Replace with `register_pages()` returning a base index per plugin at `UiShell::initialize()`, computed from whichever plugins are Kconfig-enabled. Printer plugin registers 1(select) + up to `kMaxAmsUnits` + main + preview + camera; a weather plugin might register 1.
- **`apply_snapshot(PrinterSnapshot)`** — today the single global update entry called from `Application`. Becomes: `Application` calls `plugin->update_ui()` per plugin per loop tick (per the plugin interface sketch above); `UiShell` no longer knows about `PrinterSnapshot` at all.
- **`print_active` driving power-save** — `update_power_save(on_battery, keep_awake, print_active)` bakes a printer concept into core dimming policy. Replace with `plugin->wants_awake() const` on the `Plugin` interface, queried across all plugins by `Application` and OR'd into the `any_plugin_busy` param.
- **`is_page2_active()` / `is_camera_page_active()`** — printer-specific query methods on `Ui`'s public API, consumed by `Application`/`PrinterClient` for MQTT-handoff and camera-fetch gating. Generalize to `UiShell::is_plugin_page_active(plugin_id, local_idx)`; printer plugin calls it with its own id instead of `Ui` hardcoding the concept.
- **Printer-selector (`page0_`, `PrinterCardInfo`, `update_printer_cards()`)** — this is printer-profile multi-instance UI, not core chrome. Becomes the printer plugin's own first page, built via its `build_screen()`, not a permanent fixture of every InfoHub build (a build with printer plugin disabled shouldn't show it).
- **Shared widget primitives** — AMS tray pills, status arc, labels use a consistent style/font set built once today inside `Ui`. Pull that into a small shared `ui_toolkit.hpp` (fonts, colors, common widget constructors) plugins call from `build_screen()`, so each plugin isn't reimplementing LVGL styling from scratch.

**Open questions:**

- Object lifecycle: build every enabled plugin's pages eagerly at `UiShell::initialize()` (matches current AMS-pages-always-built-but-hidden pattern) vs. lazy build/destroy on nav — matters more as plugin count grows past what fits in RAM alongside LVGL's own buffers.
- Home/tile screen from the plugin-interface sketch (`build_tile()`) vs. today's flat pager where page 0 is *the* printer selector — does page 0 become a plugin carousel (tiles), with swiping into a tile entering that plugin's own page group, i.e. two navigation levels instead of one flat pager?
- Gesture/swipe thresholds and page-count assumptions in `handle_pager_event`/`next_enabled_page` are written against a small fixed page count — needs checking they still behave with a variable, potentially larger, page count.

### SetupPortal changes sketch (design only, not implemented)

`SetupPortal` today is ~30 hand-registered `httpd_uri_t` routes (5-6 lines of boilerplate each), a single monolithic `handle_config_get`/`handle_config_post` pair that string-concatenates *every* subsystem's settings into one JSON blob (wifi, cloud, printer, display/tilt, arc colors, battery, filament, audio, timezone — `setup_portal.cpp:3414-3510+`), and a constructor that hardcodes references to every printer-domain client (`BambuCloudClient&`, `PrinterClient&`, `P1sCameraClient&`). `handle_root` builds the entire settings-page HTML/JS as one generated C++ string.

```cpp
class SetupPortal {
 public:
  // Constructor drops printer-specific refs (BambuCloudClient&, PrinterClient&,
  // P1sCameraClient&) — those move into the printer plugin. Keeps only what's
  // actually core: config, network, display, power, audio-as-core-service.
  SetupPortal(ConfigStore& config_store, const WifiManager& wifi_manager,
              Ui& ui, const PmuManager& pmu_manager, AudioNotifier& audio_notifier,
              std::array<Plugin*, N>& plugins);

  esp_err_t start();  // registers core routes, then loops:
                       //   for (plugin : plugins) plugin->register_portal_routes(server_);
  void request_unlock_pin();
  PortalAccessSnapshot access_snapshot(bool request_authorized = false);

  // NEW: exposed so a plugin's own handler can honor the PIN/session lock
  // without reimplementing it — plugin handlers call this first line 1.
  bool is_request_authorized(httpd_req_t* request);

 private:
  // Core routes unchanged: root shell, favicon, health, unlock, wifi scan,
  // session extend, OTA upload/url/status, debug log.
  static esp_err_t handle_root(httpd_req_t* request);
  static esp_err_t handle_core_config_get(httpd_req_t* request);   // wifi/display/
  static esp_err_t handle_core_config_post(httpd_req_t* request);  // battery/audio/
                                                                    // portal-lock only —
                                                                    // printer/cloud fields
                                                                    // removed, moved to
                                                                    // printer plugin's own
                                                                    // routes
  // handle_arc_*, handle_ams_display_*, handle_cloud_*, handle_local_connect,
  // handle_printers_* all DELETED from here — they move into the printer
  // plugin's register_portal_routes(), using the Plugin's own ConfigStore
  // namespace from the ConfigStore sketch.
  ...
};
```

**Specific couplings to break:**

- **`/api/config` god-endpoint** — `handle_config_get`/`handle_config_post` mix every subsystem into one flat JSON object built with raw string concat. This is the actual blocker for adding a plugin today: a new setting means editing this ~200-line pair by hand. Split into `handle_core_config_get/post` (wifi/display/battery/audio/portal-lock — genuinely core) and a generic `/api/plugins/<id>/config` GET/POST that each plugin serves via its own handler in `register_portal_routes()`, reading/writing its own `ConfigStore` namespace from the ConfigStore sketch above.
- **Route registration boilerplate** — every route today is hand-written `httpd_uri_t` + `httpd_register_uri_handler` in `start()`. Plugin routes register the same way, just called from inside `plugin->register_portal_routes(server_)` instead of being enumerated in `SetupPortal::start()`.
- **Constructor deps** — `BambuCloudClient&`, `PrinterClient&`, `P1sCameraClient&` are printer-plugin-specific and currently baked into `SetupPortal`'s constructor signature. These move out; `SetupPortal` takes the plugin array instead and never touches those types directly.
- **Printer-specific handlers** — `handle_arc_preview/commit`, `handle_source_mode_post`, `handle_ams_display_post`, `handle_cloud_connect/verify`, `handle_local_connect`, `handle_printers_get/select/save/delete/clear_local` are all printer-domain and move into the printer plugin wholesale (roughly a third of this 5.1k-line file).
- **`is_request_authorized`/PIN lock** — genuinely core, must keep gating *every* route including plugin ones. Exposed as a public method so plugin handlers call `portal.is_request_authorized(request)` as their first line, same pattern every existing handler already follows — no new mechanism needed, just wider visibility.
- **`handle_root`'s generated HTML** — currently one big hand-written string with a card/section per feature. Needs either (a) each plugin contributing a `portal_settings_html()` fragment concatenated into the shell at boot, or (b) the shell shipping a fixed layout that JS-fetches `/api/plugins` (list of registered ids) and lazy-loads each plugin's settings fragment. (a) is simpler and probably fine since disabled plugins already aren't compiled in.

**Open questions:**

- Should the printer plugin's config truly move to `/api/plugins/printer/config`, or does it stay at legacy `/api/config`-adjacent paths for backward compat with anything scraping the API?
- OTA (`handle_ota_upload/url/status`) is core today — stays core, but does OTA need to become plugin-version-aware if plugins ship as separately versioned components eventually, or is "one firmware blob, one version" fine for the foreseeable future?
- `access_mutex_`/`ota_url_mutex_` stay `SetupPortal`-private; combined with the `plugin_config_mutex_` from the ConfigStore sketch, is there a risk of a plugin handler holding one and blocking on the other — needs a straight-line locking order documented once real plugins land, not just conventions.

### Application changes sketch (design only, not implemented)

`Application::run()` is ~790 lines and maybe 80 of them are genuinely core bootstrap (config/power-mgmt/Wi-Fi bring-up, `setup_portal_.start()`, `serial_provisioner_.start()`, `pmu_manager_.initialize()`, LittleFS mount for sounds, `ui_.initialize()`). Everything else in the main `while(true)` loop — hybrid source-mode arbitration, chamber-light/print-command dispatch, camera gating, stop-banner, filament-wake, snapshot merging, audio edge-detection — is printer-domain logic with zero separation from the loop itself. `application.hpp`'s member list (`cloud_client_`, `printer_client_`, `camera_client_`, `source_mode_`, all the `hybrid_*`/`*_override_*`/`audio_last_*` fields) is ~15 of ~20 members, all printer-plugin state sitting directly on `Application`.

```cpp
class Application {
 public:
  Application();
  void run();

 private:
  // Core only — everything printer-specific is gone from this class.
  ConfigStore config_store_{};
  WifiManager wifi_manager_{};
  Ui ui_{};
  UiShell ui_shell_{};              // from the Ui sketch
  SetupPortal setup_portal_;
  SerialProvisioner serial_provisioner_;
  PmuManager pmu_manager_{};
  AudioNotifier audio_notifier_{};  // core service, plugins call audio_notifier_.play(event)
  std::array<Plugin*, kMaxPlugins> plugins_{};
};
```

```cpp
void Application::run() {
  // ... bootstrap unchanged: config_store_.initialize(), power mgmt, wifi bring-up,
  //     setup_portal_.start(), serial_provisioner_.start(), pmu_manager_.initialize(),
  //     LittleFS mount, audio_notifier_.initialize(), ui_.initialize() ...

  for (auto* plugin : plugins_) plugin->init(PluginContext{config_store_, wifi_manager_, ui_});

  while (true) {
    const uint64_t now_ms = ...;
    if (ui_.consume_portal_unlock_request()) setup_portal_.request_unlock_pin();

    for (auto* plugin : plugins_) plugin->tick(now_ms);
    for (auto* plugin : plugins_) plugin->update_ui();

    bool any_awake = false;
    uint32_t min_poll_interval_ms = kIdlePollIntervalMs;
    for (auto* plugin : plugins_) {
      any_awake |= plugin->wants_awake();
      min_poll_interval_ms = std::min(min_poll_interval_ms, plugin->desired_poll_interval_ms());
    }

    const PowerSnapshot power = pmu_manager_.sample();  // battery stays core
    ui_shell_.update_power_save(on_battery(power), keep_screen_awake(), any_awake);
    wait_for_next_iteration(ui_, pdMS_TO_TICKS(min_poll_interval_ms));
  }
}
```

**Specific couplings to break:**

- **Hybrid source-mode arbitration** — `hybrid_prefers_cloud_status`, `hybrid_local_status_supported`, `route_allows_local_jpeg_camera`, `chamber_light_command_plan`, `print_command_plan`, and the entire local/cloud gate block (`hybrid_local_gate_open_`, `local_mqtt_handoff_until_tick_`, `hybrid_camera_cooldown_deadline_`) are pure printer-plugin internals — this is the bulk of `application.cpp`'s anonymous namespace and the loop body. Moves wholesale into the printer plugin's `tick()`.
- **`wait_for_next_iteration` polling printer-specific UI flags** — the loop-pacing helper (genuinely core: touch-IRQ short-poll while screen is off) currently calls `ui.has_chamber_light_toggle_request()`/`ui.has_print_command_request()` directly to decide whether to shorten its sleep slice. That's a core helper reaching into printer-plugin state. Needs a generic `ui.has_pending_plugin_request()` flag any plugin can raise, replacing the two hardcoded booleans.
- **Audio edge-detection** — the ~35-line block translating `PrintLifecycleState` transitions / `hms_codes` / `print_error_code` into `audio_notifier_.play(...)` calls is printer-domain (uses `PrinterSnapshot` fields directly). Moves into the printer plugin's `tick()`, calling the still-core `audio_notifier_` the same way.
- **Chamber-light / print-command dispatch** — `ui_.consume_chamber_light_toggle_request()`/`consume_print_command_request()` and their local/cloud dispatch plans are printer-only concepts baked into the core loop. Moves into the printer plugin; `Ui`'s request-consuming API for these becomes plugin-scoped (plugin registers its own request queue rather than `Ui` hardcoding chamber-light and print-command as first-class request types).
- **Printer-switch / printer-card handling** — `consume_printer_switch_request()`, building `Ui::PrinterCardInfo` from `config_store_.load_printer_profiles()` — ties directly to the printer-selector page discussed in the Ui sketch; moves into the printer plugin along with that page.
- **Battery routed through `PrinterSnapshot`** — today `PowerSnapshot` from `pmu_manager_.sample()` gets copied into `snapshot.battery_percent`/`charging`/etc. (printer-domain struct) so the UI can read it. Battery is genuinely core (physical to the board, shown regardless of which plugins are active) — should live in a small core `DeviceStatus` struct the UI shell reads directly, not laundered through a plugin's snapshot type.
- **Loop pacing** (`loop_delay` computed from `snapshot.print_active || camera_page_active || page_transition_active`) — becomes `min` of each plugin's `desired_poll_interval_ms()`, so a busy plugin (mid-print, camera streaming) can request tighter polling without every other plugin needing to know about it.
- **Constructor wiring** (`cloud_client_.set_printer_presence_callback(...)` routing cloud MQTT presence into `printer_client_`'s reconnect backoff) — purely internal to the printer plugin, moves entirely into its own constructor/`init()`, `Application`'s constructor no longer references either client.

**Open questions:**

- `Plugin::tick()` must stay non-blocking (network I/O lives on each plugin's own worker task, same discipline `PrinterClient`/`BambuCloudClient` already follow) — worth stating as a hard rule for plugin authors, not just an implicit convention, since one slow `tick()` would stall every other plugin's UI update for that loop iteration.
- Does battery/power stay permanently core, or could power source itself become pluggable later (e.g. a mains-only variant with no `PmuManager`)? Leaning toward "stays core" — it's physically tied to the board, not a swappable data source like weather/stocks.
- `wait_for_next_iteration`'s touch-IRQ short-poll logic is BSP/core, but is screen-off wake behavior something a plugin should ever influence (e.g. a plugin wanting to wake the screen on an alert, like today's filament-wake) — if so `request_wake_display()` needs to stay plugin-callable, which it already is on `Ui`'s public API.

### AudioNotifier and error_lookup changes sketch (design only, not implemented)

`AudioNotifier`'s core engine (codec init, worker task, tone/melody rendering, volume/mute, PCM playback) is already generic — nothing printer-specific there. The coupling is narrower than `Ui`/`Application`: it's the closed `Event` enum (8 fixed slots, half printer-named: `kPrintStarted`/`kPrintFinished`/`kPrintError`/`kHmsAlert`/`kPrintPaused`/`kFilamentChange`, half generic: `kReconnect`/`kClick`) plus `ConfigStore`'s positional indexing against that enum's ordinal (`config_store.hpp`: *"event_index matches AudioNotifier::Event order"*).

`error_lookup` is the opposite case — it's Bambu-specific top to bottom (`ErrorLookupDomain::kPrintError/kDeviceHms`, `PrinterModel` baked into the lookup signature, HMS/print-error TSV format) but currently lives as a "core" module and gets unconditionally `EMBED_TXTFILES`'d in `main/CMakeLists.txt` regardless of what's enabled.

**AudioNotifier sketch:**

```cpp
class AudioNotifier {
 public:
  // Replaces the closed Event enum. Any plugin registers its own events at
  // init; core (Reconnect/Click) registers first so they're always slots 0-1.
  uint8_t register_event(const char* stable_key, bool default_enabled);  // stable_key
                                                                          // e.g. "print_started",
                                                                          // ≤ 15 chars — doubles as
                                                                          // the ConfigStore key
  void play(uint8_t event_id);            // was play(Event)
  void set_event_enabled(uint8_t event_id, bool enabled);
  bool event_enabled(uint8_t event_id) const;
  void set_event_pcm(uint8_t event_id, std::vector<int16_t> samples);
  // ... rest unchanged (engine internals don't care what an event "means")

 private:
  static constexpr uint8_t kMaxEvents = 32;  // was kEventCount = 8, now a cap not a fixed count
  // storage keyed by event_id same as today, just no longer tied to a
  // hardcoded enum's meaning
};
```

Couplings to break:

- **`Event` enum → `ConfigStore` ordinal** — today `load_audio_event_enabled(uint8_t event_index)`/PCM storage is positionally indexed against enum order, called out explicitly in the header comment. That's fragile even for one plugin (reordering the enum silently remaps saved settings) and breaks outright once a second plugin wants its own events. Fix: persist by the `stable_key` string (via the per-plugin `ConfigStore` namespace from the ConfigStore sketch), not by ordinal — `register_event()`'s `stable_key` becomes the actual NVS key, `event_id` is just a runtime handle.
- **Portal's per-event UI** — `handle_audio_event_post`/`play_test_event` currently enumerate a hardcoded 8-row table. Becomes: each plugin lists its registered events (label + `event_id`) in its own `portal_settings_html()` fragment, posting to a generic `/api/audio/event/<id>` endpoint instead of a printer-specific hardcoded set.
- **Which events are "core"** — `kReconnect`/`kClick` are the only two truly generic ones (any plugin's network client could fire Reconnect; any UI tap could fire Click). Those two register first, always available; `kPrintStarted` etc. move to be registered by the printer plugin at its own `init()`, alongside a weather plugin's own future `severe_alert` event, etc.

**error_lookup sketch:**

This module doesn't get generalized — it gets relocated. `ErrorLookupDomain`, the TSV format (domain marker + hex code + model-list + message), and the `PrinterModel` parameter are Bambu HMS/print-error concepts specifically; there's no generic "error lookup" concept to extract beyond the mechanism.

- **Move wholesale into the printer plugin** — `error_lookup.{hpp,cpp}` becomes plugin-private (e.g. `plugins/printer/error_lookup.{hpp,cpp}`), same as `status_resolver` and the rest of the printer-domain code from the earlier sketches.
- **`EMBED_TXTFILES` entry moves with it** — `main/CMakeLists.txt` embeds `error_lookup.tsv` unconditionally today, same list as the TLS certs. Once printer becomes optional, this entry (and the certs, which are also printer/Bambu-cloud-specific) should be conditional on the printer plugin being enabled, following the same `EXCLUDE_COMPONENTS`-per-variant pattern already used for hardware selection.
- **The mechanism is reusable, even though the content isn't** — the embedded-TSV-with-linker-symbols pattern (`_binary_error_lookup_tsv_start/_end`, single-entry cache in `lookup_error_text`, the TSV line parser) is a decent general shape for "compact code→text table baked into flash" that a future plugin might want for its own codes (e.g. a weather-condition code table). Worth factoring the parsing/caching mechanics into a small generic helper (`embedded_tsv_lookup.hpp`) the printer plugin instantiates with its own domain enum — but only once a second real consumer exists; premature to build it for one caller.
- **`format_resolved_error_detail`/`is_hms_suppressed`** — pure printer-domain helpers, move unchanged into the printer plugin alongside the rest of `error_lookup`.

**Open questions:**

- How big is `error_lookup.tsv` compiled-in — worth checking before deciding conditional-embed is actually worth the build-system complexity for a printer-optional build, versus just always including it since InfoHub without any printer plugin is a fairly odd configuration anyway.
- Does `AudioNotifier::register_event()` need collision detection if two plugins pick the same `stable_key` by accident, or is "plugin ids + event key" (e.g. `printer.print_started`) the actual namespaced key to avoid relying on authors picking unique names?

### WifiManager and network layer changes sketch (design only, not implemented)

`WifiManager` itself needs no structural change — it's already clean: pure STA+AP lifecycle (`connect_station`, `scan_visible_networks`, setup-AP fallback), depends only on `WifiCredentials` from `config_store.hpp` (core), and none of `PrinterClient`/`BambuCloudClient`/`P1sCameraClient` touch it directly. The real network-layer problem is one level up: each of those three clients independently opens its own TLS/MQTT/raw-socket connection, and the only coordination between them is ad-hoc, hardcoded, two-party logic sitting in `Application` — `hybrid_local_gate_open_`, `local_mqtt_handoff_until_tick_` (a 30s cooldown gating cloud traffic specifically while local MQTT connects), `pause_cloud_fetches`, plus a `PrinterClient::set_pre_local_mqtt_callback()` hook that exists but has no caller today. `printer_client.cpp` documents why this matters: destroying a client mid-TLS-handshake leaks ~13KB of internal RAM per failed attempt, and comments throughout (`p1s_camera_client.cpp`, `bambu_cloud_client.cpp`) show constant heap-pressure awareness (`heap_caps_get_free_size(MALLOC_CAP_INTERNAL/DMA/SPIRAM)` logged around every connect).

That serialization is currently written for exactly two known clients. It doesn't scale to N independently-authored plugins each wanting network access — a naive multi-plugin build could fire several concurrent TLS handshakes at boot and blow the internal-RAM budget on this heap-constrained target.

```cpp
// network_arbiter.hpp — new, small, core
class NetworkArbiter {
 public:
  // Non-blocking, matches the tick()-must-not-block discipline from the
  // Application sketch — caller checks, backs off, and retries next tick()
  // rather than waiting.
  bool try_acquire_handshake_slot(const char* owner_id);
  void release_handshake_slot(const char* owner_id);

  // Generalizes today's unused set_pre_local_mqtt_callback() hook and the
  // hardcoded local_mqtt_handoff_until_tick_ window — "give the caller a
  // hint for how long to back off" instead of Application manually tracking
  // per-client cooldown deadlines.
  uint32_t requested_yield_ms(const char* owner_id) const;

 private:
  static constexpr uint8_t kMaxConcurrentHandshakes = 1;  // matches today's
                                                            // de-facto serialization
  std::atomic<uint8_t> active_handshakes_{0};
};
```

**Couplings to break:**

- **Two-party hardcoded gating → shared arbiter** — `hybrid_local_gate_open_`/`local_mqtt_handoff_until_tick_`/`pause_cloud_fetches` in `Application` exist because it's the only place that knows about both `printer_client_` and `cloud_client_` simultaneously. Each plugin's network worker task instead calls `arbiter.try_acquire_handshake_slot(plugin_id)` immediately before its `esp_tls_conn_new_sync`/`esp_mqtt_client_start`/HTTPS call, and releases on connect-complete-or-fail. `Application` no longer needs to know the internals of any specific client pair.
- **`set_network_ready()` pattern generalizes cleanly as-is** — today `Application` computes `local_network_ready`/`cloud_network_ready` from Wi-Fi state + source-mode logic and pushes it into each client via `set_network_ready(bool)`. Since `WifiManager&` is already exposed on `PluginContext` in the plugin-interface sketch, plugins can just read `wifi_manager.is_station_connected()` themselves inside their own `tick()` instead of `Application` manually pushing state into two hardcoded clients — the printer plugin keeps its own hybrid-mode logic (from the Application sketch) to decide when it wants to connect, then asks the arbiter for a slot.
- **`P1sCameraClient`'s raw `esp_tls` socket** — bypasses `esp_http_client`/`esp_mqtt_client` entirely for memory reasons (JPEG streaming protocol specific to Bambu cameras). Stays entirely inside the printer plugin; just needs to go through the same arbiter for its handshake, no other generalization needed.
- **Bambu TLS trust anchors** — the 4 cert files in `main/CMakeLists.txt`'s `EMBED_TXTFILES` (`bambu.cert`, `bambu_p2s_250626.cert`, `bambu_h2c_251122.cert`, `bambu_x2c_260425.cert`) are printer-plugin trust material, not core network layer. Same conditional-embed treatment as `error_lookup.tsv` from the previous sketch — move with the printer plugin, gated on it being enabled.
- **Wi-Fi scan/config portal routes** — already noted as core in the SetupPortal sketch; unaffected here, `WifiManager` stays the single shared owner of the Wi-Fi stack regardless of plugin count.

**Open questions:**

- Is `kMaxConcurrentHandshakes = 1` too conservative once actually measured, or does the ~13KB-per-mbedTLS-session figure mean even 2 simultaneous HTTPS/MQTT handshakes risk OOM on this target? Needs real heap measurement with a second plugin's TLS client added before picking a number instead of guessing.
- Does the arbiter need any priority (e.g. the currently-visible plugin's screen gets its handshake slot first) or is simple first-come/cheap-retry sufficient given plugins are expected to back off and retry on failure anyway?
- Should the arbiter distinguish handshake type (MQTT keep-alive vs. one-shot HTTPS fetch vs. raw TLS camera socket) for slot accounting, or is "one handshake in flight system-wide, regardless of protocol" sufficient given the existing code treats all three the same way today?

### Phased extraction sequencing plan (design only, not implemented)

Sequencing principle: every phase must be additive-first — printer behavior stays externally identical after each phase, verified by build+flash+monitor on both hardware variants (no unit test suite exists here — verification is build/flash/on-device). Don't move to the next phase until the current one is proven stable, since this is a shipping product with a real install base.

| Phase | What | Depends on | Size/risk | Validation gate |
| --- | --- | --- | --- | --- |
| **0** | Baseline: tag current `v1.6.2` behavior as the regression reference (feature matrix: hybrid mode, chamber light, print control, camera, audio, OTA). No code change. | — | trivial | Have a checklist to test against after every later phase. |
| **1** | `ConfigStore` generic accessor (`load_plugin_string`/`save_plugin_indexed`/etc., per-plugin NVS namespace) added alongside existing typed methods — nothing calls it yet. | ConfigStore sketch | small, low risk | Builds clean, existing settings unaffected (new methods are dead code until used). |
| **2** | `NetworkArbiter` added as a new class; wire it into `PrinterClient`/`BambuCloudClient`/`P1sCameraClient`'s actual TLS/MQTT connect call sites, replacing the ad-hoc `local_mqtt_handoff_until_tick_`/`pause_cloud_fetches` gating in `Application`. | WifiManager/network sketch | medium, real risk (touches hot reconnect paths tied to the documented ~13KB-leak issue) | Stress-test Wi-Fi drop/reconnect and hybrid-mode source switching on-device; heap-watch (`heap_caps_get_free_size`) must not regress vs. baseline. |
| **3** | `AudioNotifier::register_event(stable_key, ...)` replaces the closed `Event` enum; printer's 6 events + `Reconnect`/`Click` register at boot as before, but persistence moves from ordinal index to `stable_key` via Phase 1's accessor. | AudioNotifier sketch, Phase 1 | medium — needs a one-time NVS migration so existing users' saved audio-enable/volume/custom-PCM settings survive the ordinal→key switch | Flash over an existing device's saved config (not a fresh device); confirm audio settings didn't reset. |
| **4** | Define the `Plugin` base class + `PluginContext` + `plugin_registry` scaffolding — new files only, nothing implements it yet, nothing calls it yet. | Plugin interface sketch, Phases 1–3 (interface references them) | small, no runtime risk | Compiles; dead code path. |
| **5** | Wrap existing printer logic behind a single `PrinterPlugin` adapter that still calls the same functions internally (no file-moving yet) — just proves `Application::run()` can drive `init()`/`tick()`/`update_ui()` through the array-of-plugins shape with exactly one plugin. | Application sketch, Phase 4 | medium — changes `Application`'s control flow, but logic underneath is untouched | Full feature-matrix retest from Phase 0 checklist; behavior must be bit-for-bit identical. |
| **6** | `UiShell` chrome/content split: pull pager mechanics, gesture/swipe, brightness/power-save, portal PIN overlay out of `Ui` into `UiShell`; `register_pages()` replaces the fixed `kPageIdx*` constants (still computing the same single printer page set for now). | Ui sketch, Phase 5 | large, highest visual-regression risk — `ui.cpp` is the biggest, most stateful file | On-device visual check of every page/transition on both variants: AMS pages, preview, camera, printer-selector, dimming/screen-off timing, gesture thresholds. |
| **7** | `SetupPortal` split: `/api/config` god-endpoint separated into core config + generic `/api/plugins/<id>/config`; route registration loop replaces hand-enumerated list; constructor drops printer-specific client refs in favor of the plugin array. | SetupPortal sketch, Phases 1, 4–6 | large — ~1/3 of `setup_portal.cpp` moves | Full Web Config walkthrough: every settings page, OTA upload/URL, PIN lock/unlock, printer profile CRUD. |
| **8** | Finish the `PrinterPlugin` extraction for real: move `status_resolver`, `error_lookup` (+ its `EMBED_TXTFILES` entries and Bambu certs made conditional), hybrid arbitration, chamber-light/print-command dispatch, printer-selector page, audio edge-detection wholesale into plugin-owned files. `Application`/`Ui`/`SetupPortal` no longer reference printer types directly. | All prior phases | large, but each piece was already isolated/validated in place by earlier phases — lowest-risk of the "big" phases precisely because it's mostly moving already-proven code | Full feature-matrix retest; diff flash size/heap against baseline. |
| **9** | Build one trivial reference plugin (e.g. a static test-pattern/clock page, no network) — proves the interface supports a second plugin before committing to a real integration's complexity. | Phase 8 | small | Confirm both plugins coexist: tile/page navigation, independent config, independent audio events, no interference with printer plugin's network/UI timing. |
| **10** | First real second plugin (weather or stocks) — only after Phase 9 proves the seam holds. | Phase 9 | scoped by that plugin's own needs | New feature validation, not a regression gate. |

Phases 1–4 can land as small, low-drama PRs in parallel-ish sequence. Phases 5–8 are the real work and should each be its own branch, fully soak-tested before the next starts — that's where a shipping OTA product actually gets hurt if rushed.

## Build commands

Requires ESP-IDF v5.5.4 environment activated (`idf.py` on PATH). Every build must pick a hardware variant explicitly and use a variant-specific build dir — never share a build dir between variants.

```bash
# AMOLED 1.75 variant
idf.py -B build-amoled_1_75 -DINFOHUB_HW_VARIANT=amoled_1_75 reconfigure build
idf.py -B build-amoled_1_75 -DINFOHUB_HW_VARIANT=amoled_1_75 -p <port> build flash monitor

# LCD 2.8C variant
idf.py -B build-lcd_2_8c -DINFOHUB_HW_VARIANT=lcd_2_8c reconfigure build
idf.py -B build-lcd_2_8c -DINFOHUB_HW_VARIANT=lcd_2_8c -p <port> build flash monitor
```

Run `reconfigure` (not `fullclean`) after any CMake/Kconfig/dependency change. There is no unit test suite — verification is build + flash + on-device monitor.

Packaging release images (after building both variants):

```bash
powershell -ExecutionPolicy Bypass -File tools/package_release.ps1 -Version vX.Y.Z
# or per-variant:
python tools/package_initial_flash.py --build-dir build-amoled_1_75 --release-root release --version vX.Y.Z
python tools/package_initial_flash.py --build-dir build-lcd_2_8c --release-root release/2.8c --version vX.Y.Z-2.8c
```

`infohub_full.bin` = merged bootloader+partitions+app, for initial/empty-device flash at `0x0`. `infohub_ota.bin` = app-only, installed through the running device's Web Config so ESP-IDF writes it to the inactive OTA slot and preserves NVS config. Never flash an OTA image as a full image. Full details: `docs/Build/README.md`.

## Hardware variant mechanism

`INFOHUB_HW_VARIANT` (CMake cache var, `lcd_2_8c` default or `amoled_1_75`) drives, in root `CMakeLists.txt`:

- which BSP component builds (`esp32_s3_touch_lcd_2_8c` vs `esp32_s3_touch_amoled_1_75`, under `components/`)
- a compile define (`INFOHUB_HW_VARIANT_LCD_2_8C=1` / `INFOHUB_HW_VARIANT_AMOLED_1_75=1`) that gates hardware-specific code, e.g. `main/include/infohub/board_config.hpp` (GPIO pins, display size) and the AXP2101 PMU code path (AMOLED only, via `components/XPowersLib`)
- which release directory packaging writes to

`main/CMakeLists.txt` conditionally adds `PRIV_REQUIRES` per variant (XPowersLib+BSP for AMOLED, esp_adc+BSP for LCD). When adding hardware-specific code, gate it with `#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)` / `#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)`, following `board_config.hpp`'s pattern.

## Architecture

Single FreeRTOS task (`infohub_app`, `app_main.cpp`) runs `infohub::Application::run()` (`main/src/application.cpp`), which owns every subsystem and drives them from one loop — most components are not independently threaded except where noted:

- **`ConfigStore`** (`config_store.{hpp,cpp}`) — NVS + LittleFS-backed settings: Wi-Fi/cloud credentials, printer profiles (up to `kMaxPrinterProfiles`), display/battery/audio/color config. All reads/writes go through this; printer-profile mutation is mutex-serialized because both the main loop and the setup-portal HTTP task can touch it.
- **`WifiManager`** — station Wi-Fi connect/reconnect.
- **`BambuCloudClient`** (`bambu_cloud_client.{hpp,cpp}`, largest file at ~5.4k lines) — Bambu Cloud auth (incl. 2FA/email-code) and cloud MQTT status/preview feed.
- **`PrinterClient`** (`printer_client.{hpp,cpp}`, ~3k lines) — local MQTT client talking directly to the printer (`device/<serial>/report` / `.../request`) over TLS, on its own task (`task_loop`/`task_entry`) synchronized via atomics + a runtime-state mutex, not the FreeRTOS caller's context. Parses Bambu's JSON status reports into `PrinterClient::LocalPrinterRuntimeState`, then republishes as a `PrinterSnapshot`. Also handles chamber-light and print pause/resume/stop commands (see `INFOHUB_EXPERIMENTAL_PRINT_CONTROL` Kconfig — Bambu printers reject unsigned print-control MQTT commands from third-party firmware; this is a deliberately experimental/off-by-default feature with legal-exposure notes in `main/Kconfig.projbuild`).
- **`status_resolver`** (`status_resolver.{hpp,cpp}`) — `merge_status_sources()` combines the local `PrinterSnapshot` and cloud `BambuCloudSnapshot` per `SourceMode` (`kCloudOnly`/`kLocalOnly`/`kHybrid`) into the single `PrinterSnapshot` the UI renders; `resolve_ui_state()` derives UI-facing status/stage strings. This is where hybrid-mode source arbitration logic lives — read it before changing anything about which source "wins" for a given field.
- **`PrinterStateStore`** (in `printer_state.hpp`) — mutex-guarded holder of the current `PrinterSnapshot`, the shared data contract between backend and UI (connection/lifecycle state, temps, progress, AMS/filament info, camera/preview blobs, HMS error codes, battery, Wi-Fi, per-field `FieldSource` provenance).
- **`Ui`** (`ui.{hpp,cpp}`, ~4k lines) — LVGL screens/widgets, polls `PrinterSnapshot` and renders it; also owns display rotation/tilt and dimming/screen-off policy.
- **`P1sCameraClient`** — local JPEG snapshot fetching (only for models with `camera_jpeg_socket` capability: A1/A1 Mini/A2L/P1P/P1S). RTSP-only models (P2S, H2 family, X1 family, X2D) cannot be decoded on-device; see `printer_model_has_rtsp_camera`.
- **`SetupPortal`** (`setup_portal.{hpp,cpp}`, ~5.1k lines) — on-device HTTP server ("Web Config"): Wi-Fi scan/config, cloud+local printer connection setup, all display/battery/audio settings, firmware OTA (file upload or URL), and PIN-gated access (`PortalAccessSnapshot`/`request_unlock_pin`). This is the primary user-facing config surface — most new settings need a `ConfigStore` field + a portal HTTP handler + (if runtime-visible) an `Application`/`Ui` wire-up.
- **`SerialProvisioner`** — USB-serial Wi-Fi provisioning path used by the web installer (`flash/index.html`) as an alternative to the `InfoHub-Setup` fallback AP.
- **`PmuManager`** — battery/USB power state (AXP2101 on AMOLED; ADC-based on LCD 2.8C).
- **`AudioNotifier`** — per-event (print started/finished/error/HMS/paused/filament-change/reconnect/click) WAV-backed sound notifications, PCM stored on LittleFS via `ConfigStore`.
- **`error_lookup`** — HMS/print-error code → human string, backed by `main/include/error_lookup/error_lookup.tsv` embedded into the binary via `EMBED_TXTFILES` (not a filesystem partition).

`PrinterModel` (`printer_state.hpp`) drives per-model capability queries (`printer_model_has_*`, `default_local_capabilities_for_model`) used throughout `status_resolver` and the clients to decide what a given printer can report.

`Application` also owns optimistic-UI state for pause/resume/stop and chamber-light toggles (see the `*_override_*` members in `application.hpp`) — a UI action is shown immediately and reverted/confirmed once the printer's real status catches up or a timeout expires.

## Conventions

- New source files go in `main/src/`, headers in `main/include/infohub/`; every new `.cpp` must be added to the `SRCS` list in `main/CMakeLists.txt`.
- Warnings are treated as significant: build uses `-Wall -Wextra`.
- Debug-only logging: gate with `#ifdef INFOHUB_DEBUG_BUILD` (auto-set when `INFOHUB_RELEASE_VERSION` contains `-debug`/`_debug`); `debug_log_buffer.{hpp,cpp}` is the in-RAM ring buffer this feeds.
- `managed_components/` and `components/*` (BSPs, XPowersLib) are vendored/generated dependencies — don't hand-edit unless intentionally patching a vendored bug (see `tools/patches/`).
