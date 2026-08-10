# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

PrintSphere: ESP32-S3 firmware (C17/C++17, ESP-IDF v5.5.4, LVGL v9.5.0) for a round/square touch display that shows Bambu Lab 3D printer status. Talks to printers via Bambu Cloud, local MQTT, or both ("hybrid" mode). Two hardware variants share one codebase, selected at CMake configure time.

## Direction: generic plugin platform (goal, not yet built)

Long-term goal: turn PrintSphere from single-purpose Bambu display into generic info-display platform. Printer status becomes one "plugin" among many (weather, stocks, other data sources), each plugged into shared display/config/network/portal scaffolding.

Current codebase (architecture below) is Bambu-specific and monolithic: `Application` hardcodes printer/cloud/camera clients as fixed members, `PrinterSnapshot`/`PrinterStateStore` is the one-and-only shared state contract, `Ui` renders printer screens directly, `SetupPortal` hardcodes printer/Wi-Fi/cloud config pages. None of this is decoupled into a plugin interface yet — treat the architecture section as "what exists today," not "what a plugin author targets."

When asked to work toward this goal: no code changes or design decisions without explicit direction from user first — this is a large refactor (state model, UI screen registration, config schema, portal pages, network client lifecycle all need a generic seam). Surface tradeoffs, don't just start restructuring.

### Plugin interface sketch (design only, not implemented)

ESP32-S3 has no OS-level dynamic loading, so "plugin" means compile-time unit (own `main/src/plugins/*.cpp` + Kconfig toggle), not a runtime-loaded `.so`. Interface = abstract base class + static registration table.

```cpp
// plugin.hpp
namespace printsphere {

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

}  // namespace printsphere
```

Registration is compile-time, one file, list grows as plugins are added:

```cpp
// plugin_registry.cpp
std::array<Plugin*, kMaxPlugins> registered_plugins = {
#if CONFIG_PRINTSPHERE_PLUGIN_PRINTER
  &g_printer_plugin,
#endif
#if CONFIG_PRINTSPHERE_PLUGIN_WEATHER
  &g_weather_plugin,
#endif
};
```

Each plugin gated by its own Kconfig bool → controls flash/RAM budget per build, same pattern as `PRINTSPHERE_EXPERIMENTAL_PRINT_CONTROL` today.

**What has to change to support this:**

- **`PrinterSnapshot`/`PrinterStateStore`** — currently the one shared state contract. Becomes printer-plugin-private; each plugin owns its own state struct + mutex-guarded store, same shape as `PrinterStateStore` but per plugin.
- **`Application`** — stops hardcoding members (`printer_client_`, `cloud_client_`, `camera_client_`...), instead owns `std::array<Plugin*, N> plugins_` and drives `init()`/`tick()` uniformly. Printer/cloud/camera/audio-notifier logic moves inside the printer plugin.
- **`Ui`** — home screen becomes a tile grid/carousel over `plugin->build_tile()`, not fixed printer pages. Navigation (`page1`/`page2`/camera page today) becomes per-plugin screen(s) registered generically.
- **`SetupPortal`** — root config page becomes a shell that iterates `register_portal_routes()` / `portal_settings_html()` per plugin instead of one big handler file. Wi-Fi/cloud-auth/PIN/OTA stay core (every plugin needs network+security), not plugin-owned.
- **`ConfigStore`** — move from fixed typed methods (`load_source_mode()` etc.) toward a namespaced key-value API plugins call directly (`config_store.load_string("weather.city")`), so adding a plugin doesn't mean editing the core header.
- **Audio/error-lookup** — arguably core services any plugin can call (`audio_notifier.play(event)`), not printer-specific, if kept general enough.

**Open questions before any of this gets built:**

- Memory: LVGL objects + N plugins' state all resident — is lazy screen construction (build on nav, destroy on leave) required, or can multiple plugins' screens coexist?
- Does every plugin get its own network task, or do they share one poll loop with per-plugin timers (favors flash/RAM, matches current single-app-task model)?
- Tile/home-screen layout: fixed slots vs scrollable list — affects `Ui` more than `Plugin` interface itself.
- Migration path: printer becomes "just a plugin" — does existing `ConfigStore` NVS layout need a migration shim so existing devices don't lose settings on upgrade?

## Build commands

Requires ESP-IDF v5.5.4 environment activated (`idf.py` on PATH). Every build must pick a hardware variant explicitly and use a variant-specific build dir — never share a build dir between variants.

```bash
# AMOLED 1.75 variant
idf.py -B build-amoled_1_75 -DPRINTSPHERE_HW_VARIANT=amoled_1_75 reconfigure build
idf.py -B build-amoled_1_75 -DPRINTSPHERE_HW_VARIANT=amoled_1_75 -p <port> build flash monitor

# LCD 2.8C variant
idf.py -B build-lcd_2_8c -DPRINTSPHERE_HW_VARIANT=lcd_2_8c reconfigure build
idf.py -B build-lcd_2_8c -DPRINTSPHERE_HW_VARIANT=lcd_2_8c -p <port> build flash monitor
```

Run `reconfigure` (not `fullclean`) after any CMake/Kconfig/dependency change. There is no unit test suite — verification is build + flash + on-device monitor.

Packaging release images (after building both variants):

```bash
powershell -ExecutionPolicy Bypass -File tools/package_release.ps1 -Version vX.Y.Z
# or per-variant:
python tools/package_initial_flash.py --build-dir build-amoled_1_75 --release-root release --version vX.Y.Z
python tools/package_initial_flash.py --build-dir build-lcd_2_8c --release-root release/2.8c --version vX.Y.Z-2.8c
```

`printsphere_full.bin` = merged bootloader+partitions+app, for initial/empty-device flash at `0x0`. `printsphere_ota.bin` = app-only, installed through the running device's Web Config so ESP-IDF writes it to the inactive OTA slot and preserves NVS config. Never flash an OTA image as a full image. Full details: `docs/Build/README.md`.

## Hardware variant mechanism

`PRINTSPHERE_HW_VARIANT` (CMake cache var, `lcd_2_8c` default or `amoled_1_75`) drives, in root `CMakeLists.txt`:
- which BSP component builds (`esp32_s3_touch_lcd_2_8c` vs `esp32_s3_touch_amoled_1_75`, under `components/`)
- a compile define (`PRINTSPHERE_HW_VARIANT_LCD_2_8C=1` / `PRINTSPHERE_HW_VARIANT_AMOLED_1_75=1`) that gates hardware-specific code, e.g. `main/include/printsphere/board_config.hpp` (GPIO pins, display size) and the AXP2101 PMU code path (AMOLED only, via `components/XPowersLib`)
- which release directory packaging writes to

`main/CMakeLists.txt` conditionally adds `PRIV_REQUIRES` per variant (XPowersLib+BSP for AMOLED, esp_adc+BSP for LCD). When adding hardware-specific code, gate it with `#if defined(PRINTSPHERE_HW_VARIANT_AMOLED_1_75)` / `#elif defined(PRINTSPHERE_HW_VARIANT_LCD_2_8C)`, following `board_config.hpp`'s pattern.

## Architecture

Single FreeRTOS task (`printsphere_app`, `app_main.cpp`) runs `printsphere::Application::run()` (`main/src/application.cpp`), which owns every subsystem and drives them from one loop — most components are not independently threaded except where noted:

- **`ConfigStore`** (`config_store.{hpp,cpp}`) — NVS + LittleFS-backed settings: Wi-Fi/cloud credentials, printer profiles (up to `kMaxPrinterProfiles`), display/battery/audio/color config. All reads/writes go through this; printer-profile mutation is mutex-serialized because both the main loop and the setup-portal HTTP task can touch it.
- **`WifiManager`** — station Wi-Fi connect/reconnect.
- **`BambuCloudClient`** (`bambu_cloud_client.{hpp,cpp}`, largest file at ~5.4k lines) — Bambu Cloud auth (incl. 2FA/email-code) and cloud MQTT status/preview feed.
- **`PrinterClient`** (`printer_client.{hpp,cpp}`, ~3k lines) — local MQTT client talking directly to the printer (`device/<serial>/report` / `.../request`) over TLS, on its own task (`task_loop`/`task_entry`) synchronized via atomics + a runtime-state mutex, not the FreeRTOS caller's context. Parses Bambu's JSON status reports into `PrinterClient::LocalPrinterRuntimeState`, then republishes as a `PrinterSnapshot`. Also handles chamber-light and print pause/resume/stop commands (see `PRINTSPHERE_EXPERIMENTAL_PRINT_CONTROL` Kconfig — Bambu printers reject unsigned print-control MQTT commands from third-party firmware; this is a deliberately experimental/off-by-default feature with legal-exposure notes in `main/Kconfig.projbuild`).
- **`status_resolver`** (`status_resolver.{hpp,cpp}`) — `merge_status_sources()` combines the local `PrinterSnapshot` and cloud `BambuCloudSnapshot` per `SourceMode` (`kCloudOnly`/`kLocalOnly`/`kHybrid`) into the single `PrinterSnapshot` the UI renders; `resolve_ui_state()` derives UI-facing status/stage strings. This is where hybrid-mode source arbitration logic lives — read it before changing anything about which source "wins" for a given field.
- **`PrinterStateStore`** (in `printer_state.hpp`) — mutex-guarded holder of the current `PrinterSnapshot`, the shared data contract between backend and UI (connection/lifecycle state, temps, progress, AMS/filament info, camera/preview blobs, HMS error codes, battery, Wi-Fi, per-field `FieldSource` provenance).
- **`Ui`** (`ui.{hpp,cpp}`, ~4k lines) — LVGL screens/widgets, polls `PrinterSnapshot` and renders it; also owns display rotation/tilt and dimming/screen-off policy.
- **`P1sCameraClient`** — local JPEG snapshot fetching (only for models with `camera_jpeg_socket` capability: A1/A1 Mini/A2L/P1P/P1S). RTSP-only models (P2S, H2 family, X1 family, X2D) cannot be decoded on-device; see `printer_model_has_rtsp_camera`.
- **`SetupPortal`** (`setup_portal.{hpp,cpp}`, ~5.1k lines) — on-device HTTP server ("Web Config"): Wi-Fi scan/config, cloud+local printer connection setup, all display/battery/audio settings, firmware OTA (file upload or URL), and PIN-gated access (`PortalAccessSnapshot`/`request_unlock_pin`). This is the primary user-facing config surface — most new settings need a `ConfigStore` field + a portal HTTP handler + (if runtime-visible) an `Application`/`Ui` wire-up.
- **`SerialProvisioner`** — USB-serial Wi-Fi provisioning path used by the web installer (`flash/index.html`) as an alternative to the `PrintSphere-Setup` fallback AP.
- **`PmuManager`** — battery/USB power state (AXP2101 on AMOLED; ADC-based on LCD 2.8C).
- **`AudioNotifier`** — per-event (print started/finished/error/HMS/paused/filament-change/reconnect/click) WAV-backed sound notifications, PCM stored on LittleFS via `ConfigStore`.
- **`error_lookup`** — HMS/print-error code → human string, backed by `main/include/error_lookup/error_lookup.tsv` embedded into the binary via `EMBED_TXTFILES` (not a filesystem partition).

`PrinterModel` (`printer_state.hpp`) drives per-model capability queries (`printer_model_has_*`, `default_local_capabilities_for_model`) used throughout `status_resolver` and the clients to decide what a given printer can report.

`Application` also owns optimistic-UI state for pause/resume/stop and chamber-light toggles (see the `*_override_*` members in `application.hpp`) — a UI action is shown immediately and reverted/confirmed once the printer's real status catches up or a timeout expires.

## Conventions

- New source files go in `main/src/`, headers in `main/include/printsphere/`; every new `.cpp` must be added to the `SRCS` list in `main/CMakeLists.txt`.
- Warnings are treated as significant: build uses `-Wall -Wextra`.
- Debug-only logging: gate with `#ifdef PRINTSPHERE_DEBUG_BUILD` (auto-set when `PRINTSPHERE_RELEASE_VERSION` contains `-debug`/`_debug`); `debug_log_buffer.{hpp,cpp}` is the in-RAM ring buffer this feeds.
- `managed_components/` and `components/*` (BSPs, XPowersLib) are vendored/generated dependencies — don't hand-edit unless intentionally patching a vendored bug (see `tools/patches/`).
