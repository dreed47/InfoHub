# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

InfoHub: ESP32-S3 firmware (C17/C++17, ESP-IDF v5.5.4, LVGL v9.5.0) for a round/square touch display, built as a generic info-display platform — Bambu Lab 3D printer status (Bambu Cloud, local MQTT, or "hybrid" mode), a WeatherFlow Tempest station ("Tempest"), generic city/location weather ("GeoWeather", via Open-Meteo, no API key), and stocks are each an independently Kconfig-toggleable plugin, not a fixed feature set. Two hardware variants share one codebase, selected at CMake configure time.

## Generic plugin platform

This used to be a Bambu-only monolith; the extraction described below is done, not aspirational — treat this section as "what exists today," not a design sketch.

ESP32-S3 has no OS-level dynamic loading, so "plugin" means a compile-time unit (own `main/src/plugins/<id>/*.cpp` + Kconfig toggle: `INFOHUB_PLUGIN_PRINTER` / `INFOHUB_PLUGIN_TEMPEST` / `INFOHUB_PLUGIN_STOCKS` / `INFOHUB_PLUGIN_GEOWEATHER`, all in `main/Kconfig.projbuild`), not a runtime-loaded `.so`. Printer defaults **on** (it's the flagship feature); tempest/stocks/geoweather default off. Disabling a plugin drops its source files from the build (`main/CMakeLists.txt`'s per-plugin `if(CONFIG_INFOHUB_PLUGIN_*)` blocks feeding `INFOHUB_PLUGIN_SRCS`/`INFOHUB_PLUGIN_EMBED_TXTFILES`) and the linker strips its now-unreferenced library code — disabling printer alone saves roughly a third of the flash image (MQTT client, JPEG decoder, Bambu TLS certs, error-lookup table all drop out).

Small caveat worth knowing if you ever touch `main/CMakeLists.txt`: `REQUIRES`/`PRIV_REQUIRES` **cannot** be gated behind `CONFIG_INFOHUB_PLUGIN_*` — ESP-IDF evaluates each component's `CMakeLists.txt` twice, and `CONFIG_*` Kconfig variables aren't populated during the first (dependency-graph) pass, so a gated `REQUIRES` breaks the build regardless of the option's value (confirmed empirically, not a guess). `mqtt`/`espressif__esp_new_jpeg` stay in the unconditional `REQUIRES` list; only `SRCS`/`EMBED_TXTFILES` gating works, and that's sufficient since the linker still strips the dead code.

When asked to work on plugin architecture: no code changes or design decisions without explicit direction from user first for anything not already covered below — the remaining open items (network handshake arbitration, audio event registration) touch hot reconnect/heap paths on a shipping product with no unit tests. Surface tradeoffs, don't just start restructuring.

### Plugin interface (implemented)

`main/include/infohub/plugin.hpp`:

```cpp
namespace infohub {

static constexpr uint8_t kMaxPlugins = 8;

// Core services a plugin may need. SetupPortal/PmuManager/AudioNotifier stay
// core (owned by Application), plugins only borrow references.
struct PluginContext {
  ConfigStore& config_store;
  WifiManager& wifi_manager;  // shared net stack; a plugin does not own its own Wi-Fi
  Ui& ui;
  SetupPortal& setup_portal;
  PmuManager& pmu_manager;
  AudioNotifier& audio_notifier;
  NetworkArbiter& network_arbiter;  // shared handshake-serialization slot — see NetworkArbiter section
};

class Plugin {
 public:
  virtual ~Plugin() = default;

  // e.g. "printer", "tempest" — also doubles as the ConfigStore NVS
  // namespace for this plugin's own settings, so must be <= 15 chars.
  virtual const char* id() const = 0;
  virtual const char* display_name() const = 0;

  virtual esp_err_t init(PluginContext& ctx) = 0;

  // Called once per Application main-loop iteration. Must not block — any
  // network I/O belongs on the plugin's own worker task, polled from here via
  // atomics/snapshots (same discipline PrinterClient/BambuCloudClient
  // follow). One slow tick() stalls every other plugin's UI update.
  virtual void tick(uint64_t now_ms) = 0;

  // Arbitration/power-save hints Application aggregates across all plugins.
  virtual bool wants_network() const = 0;
  virtual bool wants_awake() const = 0;                    // true suppresses screen dimming/off
  virtual uint32_t desired_poll_interval_ms() const = 0;   // Application takes the min across all

  // Number of on-device screens (beyond the home tile) this plugin wants
  // reserved in Ui's generic plugin-page pool. 0 (default) means tile-only.
  // Fixed per plugin type at compile time. Printer reports 8 (1 select +
  // kMaxAmsUnits + main/preview/camera) and is a real, uniform pool entry —
  // no special-cased slot, same mechanism tempest/stocks/geoweather use.
  virtual uint8_t page_count() const { return 0; }

  virtual void build_tile(lv_obj_t* parent) = 0;
  // Called once per reserved page (page_index in [0, page_count())), after
  // init(). `parent` is that page's bare LVGL container
  // (Ui::plugin_page_container()).
  virtual void build_screen(lv_obj_t* parent, uint8_t page_index) = 0;
  virtual void update_ui() = 0;

  // Portal: API handlers namespaced under /api/plugins/<id>/*, registered
  // generically by SetupPortal (for (Plugin* p : plugins) p->register_portal_routes(...)).
  // Handlers must call SetupPortal::is_request_authorized() themselves to
  // honor the PIN/session lock. portal_settings_html() exists on the
  // interface but is currently dead code — see the SetupPortal section below.
  virtual void register_portal_routes(httpd_handle_t server) = 0;
  virtual std::string portal_settings_html() const = 0;

  virtual void load_config() = 0;
  virtual void save_config() = 0;

  // Runtime enable/disable, independent of the Kconfig toggle. Concrete, not
  // virtual. Application's tick()/update_ui()/poll-interval loops skip a
  // disabled plugin; init()/build_screen() still run unconditionally.
  bool enabled() const { return enabled_; }
  void set_enabled(bool enabled) { enabled_ = enabled; }

  // True once this plugin's own data source is set up enough that Web
  // Config's PIN lock is allowed to engage. Default true.
  virtual bool is_configured() const { return true; }

 protected:
  bool enabled_ = true;
};

}  // namespace infohub
```

Registration is a flat array built directly in `Application`'s constructor (`main/src/application.cpp`), each entry `#if CONFIG_INFOHUB_PLUGIN_*`-guarded:

```cpp
#if CONFIG_INFOHUB_PLUGIN_PRINTER
  plugins_[0] = &printer_plugin_;
#endif
#if CONFIG_INFOHUB_PLUGIN_TEMPEST
  plugins_[1] = &tempest_plugin_;
#endif
#if CONFIG_INFOHUB_PLUGIN_STOCKS
  plugins_[2] = &stocks_plugin_;
#endif
#if CONFIG_INFOHUB_PLUGIN_GEOWEATHER
  plugins_[3] = &geoweather_plugin_;
#endif
```

(`main/include/infohub/plugin_registry.hpp`/`.cpp` also exist with a `registered_plugins` array of the same shape — that's dead scaffolding nothing reads; the array above in `application.cpp` is the real mechanism. Don't be misled by it.)

### ConfigStore (implemented)

Core settings (Wi-Fi, cloud credentials, display/battery/audio/color, printer profiles) keep their original fixed typed methods and the `"infohub"` NVS namespace — untouched, for migration safety on existing devices. Plugin-owned settings go through a generic per-plugin-namespace accessor (`main/include/infohub/config_store.hpp`):

```cpp
std::string load_plugin_string(const char* plugin_ns, const char* key) const;
esp_err_t   save_plugin_string(const char* plugin_ns, const char* key,
                                const std::string& value) const;

// Indexed-record helpers, generalized from the prn_%u_%s / prn_count
// pattern PrinterProfile already used.
std::string load_plugin_indexed(const char* plugin_ns, uint8_t index,
                                 const char* suffix) const;
esp_err_t   save_plugin_indexed(const char* plugin_ns, uint8_t index,
                                 const char* suffix, const std::string& value) const;
uint8_t     load_plugin_record_count(const char* plugin_ns) const;
esp_err_t   save_plugin_record_count(const char* plugin_ns, uint8_t count) const;
```

`plugin_ns` (== `Plugin::id()`) must be ≤ 15 chars — ESP-IDF NVS's hard cap on namespace/key strings, which is exactly why this is a per-plugin *namespace* rather than a dotted-key scheme like `"plugin.geoweather.city"` (that would blow the 15-char key limit immediately). Tempest/stocks/geoweather/printer's own `enabled` flag and settings all go through this already (`config_store_->load_plugin_string(id(), "enabled")` etc.) — this isn't a sketch, it's load-bearing for all four shipped plugins.

### Ui / UiShell (implemented)

`Ui` (`ui.{hpp,cpp}`) is chrome only: pager/gesture mechanics, brightness/power-save policy, portal PIN overlay, rotation/tilt, the shared status-ring/battery overlay (`lv_layer_top()`-parented, generic — driven by `apply_ring_visual()`/`update_battery_overlay()`, not any one plugin's data). `UiShell` (`ui_shell.{hpp,cpp}`) is the actual page registry underneath it. Neither references `PrinterSnapshot` or `PrinterPlugin` — verified zero hits for either symbol in `ui.hpp`/`ui.cpp`/`ui_shell.hpp`/`ui_shell.cpp`.

Every plugin's screens live in one uniform generic pool, no exceptions:

```cpp
// Application, once at boot, before ui_.initialize():
uint16_t plugin_page_total = 0;
for (Plugin* plugin : plugins_) {
  if (plugin == nullptr) continue;
  plugin_page_total += plugin->page_count();
}
ui_.reserve_plugin_page_pool(plugin_page_total);   // creates that many bare LVGL pages up front
ui_.initialize();

// ... after every plugin's init():
for (Plugin* plugin : plugins_) {
  if (plugin == nullptr) continue;
  const uint8_t page_count = plugin->page_count();
  if (page_count == 0) continue;
  const int base = ui_.register_plugin_pages(plugin->id(), page_count);
  for (uint8_t i = 0; i < page_count; ++i) {
    plugin->build_screen(ui_.plugin_page_container(base + i), i);
  }
}
```

Page-active/visible queries are one generic method, not printer-named ones: `bool Ui::is_plugin_page_active(const char* plugin_id, int local_idx) const` / `is_plugin_page_visible(...)`, resolved through `UiShell::plugin_page_range(plugin_id, &base, &count)`. Printer's own `tick()`/`update_ui()` call these with `"printer"` + its own local indices — Ui has no idea what "the camera page" or "the preview page" means.

`LvglLockGuard` (moved to `ui_toolkit.{hpp,cpp}` as a shared primitive) wraps every LVGL API touch made from a non-LVGL-task context — **any plugin's `build_screen()`/`update_ui()` implementation must acquire this before touching LVGL objects.** This bit InfoHub for real during printer's own extraction: the widget-construction code used to get the lock for free from `Ui::build_dashboard()`'s own wrapper; moving it into `PrinterPlugin::build_screen()` without bringing the lock along let it race LVGL's render task and hang boot (LVGL's own `"invalidate area is not allowed during rendering"` assert, silently continuing into a corrupted-state infinite loop rather than aborting). Root-caused via on-device serial debugging — `ESP_LOGI`'s buffered output was getting lost the instant the hang started, needed raw `esp_rom_printf` to actually see the crash site. Fixed now, but tempest/stocks/geoweather's own `build_screen()` implementations have the same *missing*-lock pattern today — they just haven't hit the race in practice (smaller/simpler widget trees). Known latent risk, not yet hardened.

**Still-parked, no live bug forcing it:** the home screen is still a flat pager (first-registered plugin's page 0 is what you land on, not a tile/carousel home screen) — `Plugin::build_tile()` exists on the interface but nothing calls it yet. Revisit only if a future plugin actually needs a tile-grid home screen.

### SetupPortal (implemented)

`SetupPortal`'s constructor no longer takes any plugin-specific reference — `SetupPortal(ConfigStore&, const WifiManager&, Ui&, const PmuManager&, AudioNotifier&)`, plus `start(const std::array<Plugin*, kMaxPlugins>& plugins)`. Where it needs printer-specific status (two read-only display handlers, `handle_root`/`handle_health`), it looks the plugin up by id from the array it already has (`Plugin* printer_plugin() const` — linear scan for `id() == "printer"`), same pattern `is_provisioning_complete()` already used for the generic "is any plugin still unconfigured" check. No plugin gets a hardcoded reference-typed member.

Route registration is fully generic — `for (Plugin* plugin : plugins) plugin->register_portal_routes(server_);` — every plugin (printer included) registers its own routes under `/api/plugins/<id>/*` via its own handler, in its own translation unit (`printer_plugin_portal.cpp`, `tempest_plugin_portal.cpp`, `stocks_plugin_portal.cpp`, `geoweather_plugin_portal.cpp`). The old `/api/config` god-endpoint that used to string-concatenate every subsystem's settings into one blob is gone — `handle_config_get`/`handle_config_post` are core-only now (wifi/display/battery/audio/arc-color/filament/timezone/portal-lock), no plugin fields.

**Not done, no live need for it:** `handle_root`'s settings-page HTML is still one big hand-written C++ string with an `#if CONFIG_INFOHUB_PLUGIN_*`-guarded block per plugin (mirrors tempest/stocks, which were built this way from the start) — it does *not* generically loop `plugin->portal_settings_html()`. That virtual method exists on the interface and every plugin overrides it to return `{}`; it's dead code. Building a real generic HTML-fragment mechanism was considered and explicitly deferred (smaller diff, consistent with how tempest/stocks already work) — geoweather followed the same pattern rather than revisiting it.

### Application (implemented)

`Application` (`main/include/infohub/application.hpp`, `main/src/application.cpp`) owns only core services plus the plugin array — `config_store_`, `wifi_manager_`, `ui_`, `pmu_manager_`, `audio_notifier_`, `setup_portal_`, `serial_provisioner_`, `std::array<Plugin*, kMaxPlugins> plugins_`. `printer_plugin_`/`tempest_plugin_`/`stocks_plugin_`/`geoweather_plugin_` members are each `#if CONFIG_INFOHUB_PLUGIN_*`-guarded; nothing printer-specific exists unconditionally on this class.

`run()`'s core `while (true)` loop, every iteration:

```cpp
ui_.set_portal_access_state(...);           // core: PIN/session overlay
ui_.set_core_wifi_state(...);               // core: Wi-Fi badge state
ui_.update_portal_access_visuals();
ui_.update_battery_overlay(pmu_manager_.sample());  // core: PmuManager-driven, no plugin involved

for (Plugin* plugin : plugins_) { if (plugin && plugin->enabled()) plugin->tick(now_ms); }
for (Plugin* plugin : plugins_) { if (plugin && plugin->enabled()) plugin->update_ui(); }

uint32_t min_poll_interval_ms = 1500;
for (Plugin* plugin : plugins_) {
  if (!plugin || !plugin->enabled()) continue;
  min_poll_interval_ms = std::min(min_poll_interval_ms, plugin->desired_poll_interval_ms());
}
wait_for_next_iteration(ui_, pdMS_TO_TICKS(min_poll_interval_ms));
```

Battery being pushed here directly (not laundered through any plugin's snapshot type) is deliberate — it's physically tied to the board and must work with every plugin disabled. All the hybrid source-mode arbitration, chamber-light/print-command optimistic-UI dispatch, and audio edge-detection that used to live in `Application`'s loop body are gone from this file entirely — they're `PrinterPlugin::tick()`'s problem now, invisible to `Application`.

### AudioNotifier and error_lookup (partially generalized)

`AudioNotifier`'s original 8-slot `Event` enum (`kPrintStarted`/`kPrintFinished`/.../`kReconnect`/`kClick`) is unchanged and still positionally NVS-indexed — explicitly "do not renumber" per its own header comment. On top of that, `register_event(const char* stable_key, bool default_enabled)` now exists for *new* dynamically-registered events beyond the fixed 8 (`kMaxEvents = 32` cap), persisted by `stable_key` rather than ordinal, plus a `play(uint8_t event_id)` overload alongside the original `play(Event)`. So: the printer plugin's original 8 events are still enum-ordinal-based (untouched, for NVS migration safety), but a plugin wanting its *own* new events (Tempest rain-start/rain-stop or lightning-detected tones, say — future work, deliberately not wired up yet, and deliberately Tempest-only, not GeoWeather's concern) has a real, working path that doesn't require touching the closed enum. Not the full "replace the enum" sketch once written here — a coexistence model instead, and it's shipped/load-bearing, not aspirational.

`error_lookup` (HMS/print-error code → human string, Bambu-specific top to bottom) now lives under `plugins/printer/error_lookup.{hpp,cpp}`, and its `EMBED_TXTFILES` entry (plus the 4 Bambu TLS certs) is conditional on `CONFIG_INFOHUB_PLUGIN_PRINTER` in `main/CMakeLists.txt` — this was the literal "not yet gated" comment in this file that prompted the whole plugin-optionality effort; it's resolved now.

### NetworkArbiter (scaffolding built, not wired in)

`main/include/infohub/network_arbiter.hpp` / `main/src/network_arbiter.cpp` exist — a small, non-blocking `try_acquire_handshake_slot(owner_id)`/`release_handshake_slot(owner_id)` primitive meant to replace `Application`'s ad-hoc, hardcoded two-party gating (`hybrid_local_gate_open_`, `local_mqtt_handoff_until_tick_`) for TLS/MQTT handshake serialization across independently-authored plugin network clients. The class itself is real, but **nothing calls it yet** (the header says so explicitly: "NOT YET WIRED IN"). `PrinterClient`/`BambuCloudClient`/`P1sCameraClient` still use the original two-party hardcoded gating internally. Wiring this in touches hot reconnect paths tied to a documented ~13KB-internal-RAM-leak-per-failed-handshake issue — needs real on-device Wi-Fi drop/reconnect + heap-watch stress testing before it's trustworthy on a shipping product. No code changes here without explicit user direction first.

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

## Plugin selection

`idf.py -B <build-dir> menuconfig` → "InfoHub" submenu → `INFOHUB_PLUGIN_PRINTER` / `INFOHUB_PLUGIN_TEMPEST` / `INFOHUB_PLUGIN_STOCKS` / `INFOHUB_PLUGIN_GEOWEATHER` (printer defaults on, the other three default off). Each toggle is independent of `INFOHUB_HW_VARIANT` — any combination builds on either hardware variant. To flip a toggle non-interactively (e.g. scripting a second test build without touching the main `sdkconfig`), point a separate build at its own sdkconfig file:

```bash
idf.py -B build-test -DSDKCONFIG=build-test/sdkconfig -DINFOHUB_HW_VARIANT=amoled_1_75 reconfigure
# edit build-test/sdkconfig directly (e.g. sed the CONFIG_INFOHUB_PLUGIN_* line), then:
idf.py -B build-test -DSDKCONFIG=build-test/sdkconfig -DINFOHUB_HW_VARIANT=amoled_1_75 build
```

Without `-DSDKCONFIG=...`, every build dir shares the one `sdkconfig` at the project root — editing it for a throwaway test build affects the "real" one too.

## Architecture

Single FreeRTOS task (`infohub_app`, `app_main.cpp`) runs `infohub::Application::run()` (`main/src/application.cpp`), which owns core services plus the compiled-in `Plugin*` array and drives everything from one loop — most components are not independently threaded except where noted. See "Generic plugin platform" above for the `Plugin` interface itself; this section covers what's core vs. what's now printer-plugin-owned.

**Core (`Application`-owned, exists regardless of which plugins are compiled in):**

- **`ConfigStore`** (`config_store.{hpp,cpp}`) — NVS + LittleFS-backed settings. Core typed methods (Wi-Fi/cloud credentials, printer profiles, display/battery/audio/color) unchanged for migration safety; generic `load_plugin_string`/`load_plugin_indexed`/etc. for plugin-owned settings (see above). Printer-profile mutation stays mutex-serialized (main loop + setup-portal HTTP task both touch it).
- **`WifiManager`** — station Wi-Fi connect/reconnect. No plugin touches it directly; plugins read `wifi_manager.is_station_connected()` themselves from `PluginContext`.
- **`Ui`/`UiShell`** (`ui.{hpp,cpp}`, `ui_shell.{hpp,cpp}`) — chrome, generic page pool, shared status-ring/battery overlay. Zero plugin-specific type knowledge (see above).
- **`SetupPortal`** (`setup_portal.{hpp,cpp}`) — on-device HTTP server ("Web Config"): Wi-Fi scan/config, core display/battery/audio settings, firmware OTA, PIN-gated access (`PortalAccessSnapshot`/`request_unlock_pin`), and the generic per-plugin route-registration loop. No plugin-specific client references.
- **`SerialProvisioner`** — USB-serial Wi-Fi provisioning path (`flash/index.html`) as an alternative to the `InfoHub-Setup` fallback AP.
- **`PmuManager`** — battery/USB power state (AXP2101 on AMOLED; ADC-based on LCD 2.8C), pushed to `Ui` every loop iteration independent of any plugin.
- **`AudioNotifier`** — codec/task/tone-rendering engine is generic; `Reconnect`/`Click` are the only two truly core events, everything else is plugin-registered (see "AudioNotifier and error_lookup" above).
- **`NetworkArbiter`** — handshake-serialization scaffolding, not wired into any client yet (see above).

**Printer plugin** (`main/include/infohub/plugins/printer/`, `main/src/plugins/printer/`, all `#if CONFIG_INFOHUB_PLUGIN_PRINTER`-gated in `CMakeLists.txt`) — everything below is `PrinterPlugin`-owned, not `Application`- or `Ui`-owned:

- **`PrinterPlugin`** (`printer_plugin.{hpp,cpp}` for lifecycle/tick/config, `printer_plugin_ui.cpp` for all AMS/dashboard/preview/camera widget construction and updates, `printer_plugin_portal.cpp` for its `/api/plugins/printer/*` routes) — owns `BambuCloudClient cloud_client_`, `PrinterClient printer_client_`, `P1sCameraClient camera_client_` as direct members (not `Application`), and the optimistic-UI override state for pause/resume/stop and chamber-light toggles (`*_override_*` members, now on `PrinterPlugin`, not `Application`). `build_screen()` builds all 8 of its pages (printer-selector + AMS units + main dashboard + preview + camera) directly into pool-provided containers, under an `LvglLockGuard` for the whole call.
- **`BambuCloudClient`** (`bambu_cloud_client.{hpp,cpp}`, largest file at ~5.4k lines) — Bambu Cloud auth (incl. 2FA/email-code) and cloud MQTT status/preview feed.
- **`PrinterClient`** (`printer_client.{hpp,cpp}`, ~3k lines) — local MQTT client talking directly to the printer (`device/<serial>/report` / `.../request`) over TLS, on its own task synchronized via atomics + a runtime-state mutex. Parses Bambu's JSON status reports into a `PrinterSnapshot`. Also handles chamber-light and print pause/resume/stop commands (see `INFOHUB_EXPERIMENTAL_PRINT_CONTROL` Kconfig, `depends on INFOHUB_PLUGIN_PRINTER` — Bambu printers reject unsigned print-control MQTT commands from third-party firmware; deliberately experimental/off-by-default with legal-exposure notes in `main/Kconfig.projbuild`).
- **`status_resolver`** — `merge_status_sources()` combines local `PrinterSnapshot` and cloud `BambuCloudSnapshot` per `SourceMode` (`kCloudOnly`/`kLocalOnly`/`kHybrid`) into the `PrinterSnapshot` `PrinterPlugin` renders; `resolve_ui_state()` derives UI-facing status/stage strings. Hybrid-mode source arbitration lives here — read before changing which source "wins" for a given field.
- **`PrinterStateStore`** (in `printer_state.hpp`) — mutex-guarded holder of the current `PrinterSnapshot` (connection/lifecycle state, temps, progress, AMS/filament info, camera/preview blobs, HMS error codes, per-field `FieldSource` provenance). Plugin-private now, not a shared cross-plugin contract.
- **`P1sCameraClient`** — local JPEG snapshot fetching (only for models with `camera_jpeg_socket` capability: A1/A1 Mini/A2L/P1P/P1S). RTSP-only models (P2S, H2 family, X1 family, X2D) can't be decoded on-device; see `printer_model_has_rtsp_camera`.
- **`error_lookup`** — HMS/print-error code → human string, `error_lookup.tsv` conditionally `EMBED_TXTFILES`'d (see above).

`PrinterModel` (`printer_state.hpp`) drives per-model capability queries (`printer_model_has_*`, `default_local_capabilities_for_model`) used throughout `status_resolver` and the clients.

**Tempest/stocks/geoweather plugins** (`main/include/infohub/plugins/tempest/`, `main/src/plugins/tempest/`; same shape under `plugins/stocks/` and `plugins/geoweather/`) — much smaller, each roughly: one HTTP-polling client class, `<id>_plugin.{hpp,cpp}` for lifecycle/tick/widgets, `<id>_plugin_portal.cpp` for its own routes. Good reference for a new plugin's shape — closer to what a plugin "should" look like than printer's much larger, historically-grown footprint. Tempest talks to WeatherFlow's cloud API for one physical Tempest station (station ID + API token); GeoWeather is the generic, no-hardware, no-API-key alternative — free-text location resolved via Open-Meteo's geocoding API, then polled via Open-Meteo's forecast API. Both currently ship the same 2-page layout (current conditions + 4-hour forecast strip), but Tempest is the one slated for future hardware-specific sound events (rain start/stop, lightning detected) that don't make sense for a generic location.

## Conventions

- New core source files go in `main/src/`, headers in `main/include/infohub/`; every new `.cpp` must be added to the `SRCS` list in `main/CMakeLists.txt`. New plugin-owned files go in `main/src/plugins/<id>/` / `main/include/infohub/plugins/<id>/` and get added to that plugin's `if(CONFIG_INFOHUB_PLUGIN_<ID>)` block feeding `INFOHUB_PLUGIN_SRCS`/`INFOHUB_PLUGIN_EMBED_TXTFILES` (see "Generic plugin platform"), not the unconditional `SRCS` list.
- Warnings are treated as significant: build uses `-Wall -Wextra`.
- Debug-only logging: gate with `#ifdef INFOHUB_DEBUG_BUILD` (auto-set when `INFOHUB_RELEASE_VERSION` contains `-debug`/`_debug`); `debug_log_buffer.{hpp,cpp}` is the in-RAM ring buffer this feeds.
- `managed_components/` and `components/*` (BSPs, XPowersLib) are vendored/generated dependencies — don't hand-edit unless intentionally patching a vendored bug (see `tools/patches/`).
