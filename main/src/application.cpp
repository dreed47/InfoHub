#include "infohub/application.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_littlefs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "infohub/time_sync.hpp"

#if defined(INFOHUB_HW_VARIANT_AMOLED_1_75)
#include "bsp/esp32_s3_touch_amoled_1_75.h"
#elif defined(INFOHUB_HW_VARIANT_LCD_2_8C)
#include "bsp/esp32_s3_touch_lcd_2_8c.h"
#else
#error "Unknown InfoHub hardware variant"
#endif

namespace infohub {

namespace {
constexpr char kTag[] = "infohub.app";
constexpr TickType_t kScreenOffTouchWakePollSlice = pdMS_TO_TICKS(25);
constexpr TickType_t kUiCommandWakePollSlice = pdMS_TO_TICKS(50);

esp_err_t configure_power_management() {
#if CONFIG_PM_ENABLE
  esp_pm_config_t pm_config = {};
  pm_config.max_freq_mhz = 240;
  pm_config.min_freq_mhz = 80;
  pm_config.light_sleep_enable = false;
  ESP_RETURN_ON_ERROR(esp_pm_configure(&pm_config), kTag, "esp_pm_configure failed");
  ESP_LOGI(kTag, "Power management enabled: DFS 80-240 MHz, light sleep off");
#else
  ESP_LOGI(kTag, "Power management disabled in sdkconfig (CONFIG_PM_ENABLE=n)");
#endif
  return ESP_OK;
}

void wait_for_next_iteration(Ui& ui, TickType_t delay) {
  TickType_t remaining = delay;
  while (remaining > 0) {
    if (ui.has_chamber_light_toggle_request() || ui.has_print_command_request()) {
      break;
    }
    const bool touch_wake_poll_active = ui.screen_power_mode() == ScreenPowerMode::kOff;
    TickType_t slice = remaining;
    if (touch_wake_poll_active && slice > kScreenOffTouchWakePollSlice) {
      slice = kScreenOffTouchWakePollSlice;
    } else if (slice > kUiCommandWakePollSlice) {
      slice = kUiCommandWakePollSlice;
    }
    vTaskDelay(slice);
    remaining -= slice;

    if (ui.has_chamber_light_toggle_request() || ui.has_print_command_request()) {
      break;
    }
    if (touch_wake_poll_active && gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
      // The LVGL worker is paused while the screen is off, so a short tap can
      // be missed if the main loop sleeps for the full low-power interval.
      // Poll the raw touch IRQ in short slices so wake feels immediate.
      ui.request_wake_display();
      break;
    }
  }
}
}  // namespace

Application::Application()
    : setup_portal_(config_store_, wifi_manager_, printer_plugin_, ui_, pmu_manager_,
                    audio_notifier_),
      serial_provisioner_(config_store_, wifi_manager_) {
  printer_plugin_.cloud_client().set_config_store(&config_store_);
  // Route printer online/offline events from the Bambu Cloud MQTT feed to the
  // local PrinterClient so it can collapse its reconnect backoff the moment the
  // printer is known to be reachable again. Avoids blind TCP-probe cycles while
  // the printer is powered off or roaming on the LAN.
  printer_plugin_.cloud_client().set_printer_presence_callback([this](bool online) {
    printer_plugin_.printer_client().notify_cloud_presence(online);
  });
  plugins_[0] = &printer_plugin_;
#if CONFIG_INFOHUB_PLUGIN_WEATHER
  plugins_[1] = &weather_plugin_;
#endif
}

void Application::run() {
  esp_log_level_set("mbedtls", ESP_LOG_WARN);
  ESP_LOGI(kTag, "Bootstrapping native InfoHub project");

  ESP_ERROR_CHECK(config_store_.initialize());
  // Apply persisted timezone before any localtime_r() consumer (UI ETA,
  // logs, etc.). SNTP itself is started later when an IP is acquired.
  time_sync::set_timezone_iana(config_store_.load_timezone_iana());
  ESP_ERROR_CHECK(configure_power_management());
  ESP_ERROR_CHECK(wifi_manager_.initialize_network_stack());
  ESP_ERROR_CHECK(wifi_manager_.start_setup_access_point(config_store_.load_device_name()));

  const WifiCredentials wifi_credentials = config_store_.load_wifi_credentials();
  if (wifi_credentials.is_configured()) {
    const esp_err_t wifi_err = wifi_manager_.connect_station(wifi_credentials);
    if (wifi_err != ESP_OK) {
      ESP_LOGW(kTag, "Stored Wi-Fi connect failed: %s", esp_err_to_name(wifi_err));
    }
  }

  ESP_ERROR_CHECK(setup_portal_.start(plugins_));
  if (serial_provisioner_.start() != ESP_OK) {
    ESP_LOGW(kTag, "USB Wi-Fi setup unavailable; use the fallback setup access point");
  }
  ESP_ERROR_CHECK(pmu_manager_.initialize());
  ESP_LOGI(kTag, "Heap status: internal=%u bytes psram=%u bytes",
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  ui_.set_arc_color_scheme(config_store_.load_arc_color_scheme());
  ui_.set_display_rotation(config_store_.load_display_rotation());
  ui_.set_display_tilt_deci_deg(config_store_.load_display_tilt_deci_deg());
  ui_.set_battery_display_policy(config_store_.load_battery_display_policy());
  audio_notifier_.set_enabled(config_store_.load_audio_enabled());
  audio_notifier_.set_volume_percent(config_store_.load_audio_volume_percent());

  // Mount the LittleFS partition that holds custom sound files.
  // Must be done before loading PCM blobs below.
  {
    const esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path = "/sounds",
        .partition_label = "sounds",
        .partition = nullptr,
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    const esp_err_t lfs_err = esp_vfs_littlefs_register(&lfs_conf);
    if (lfs_err != ESP_OK) {
      ESP_LOGW(kTag, "LittleFS mount failed (%s) - custom sounds unavailable this boot",
               esp_err_to_name(lfs_err));
    } else {
      size_t total = 0, used = 0;
      esp_littlefs_info("sounds", &total, &used);
      ESP_LOGI(kTag, "LittleFS sounds: %u KB total, %u KB used",
               static_cast<unsigned>(total / 1024), static_cast<unsigned>(used / 1024));
    }
  }

  // Per-event enable flags and optional custom PCM blobs.
  for (uint8_t i = 0; i < AudioNotifier::kEventCount; ++i) {
    audio_notifier_.set_event_enabled(
        static_cast<AudioNotifier::Event>(i),
        config_store_.load_audio_event_enabled(i));
    const std::vector<uint8_t> pcm_bytes = config_store_.load_audio_event_pcm(i);
    if (!pcm_bytes.empty() && (pcm_bytes.size() % sizeof(int16_t)) == 0) {
      std::vector<int16_t> samples(pcm_bytes.size() / sizeof(int16_t));
      std::memcpy(samples.data(), pcm_bytes.data(), pcm_bytes.size());
      audio_notifier_.set_event_pcm(static_cast<AudioNotifier::Event>(i), std::move(samples));
    }
  }
  if (audio_notifier_.initialize() != ESP_OK) {
    ESP_LOGW(kTag, "Audio notifier init failed - sound disabled this boot");
  }

  // Every compiled-in plugin's generic-page-pool size must be known before
  // ui_.initialize() builds the pager, so the pool's LVGL containers can be
  // created up front alongside every other page. PrinterPlugin doesn't
  // participate (page_count() stays 0 — its pages predate this pool and stay
  // Ui-hardcoded), so this only sums weather/stocks/future-plugin counts.
  uint16_t plugin_page_total = 0;
  for (Plugin* plugin : plugins_) {
    if (plugin != nullptr) {
      plugin_page_total = static_cast<uint16_t>(plugin_page_total + plugin->page_count());
    }
  }
  ui_.reserve_plugin_page_pool(plugin_page_total);

  ESP_ERROR_CHECK(ui_.initialize());

  PluginContext plugin_ctx{config_store_, wifi_manager_,   ui_,
                           setup_portal_,  pmu_manager_,    audio_notifier_};
  for (Plugin* plugin : plugins_) {
    if (plugin == nullptr) {
      continue;
    }
    ESP_ERROR_CHECK(plugin->init(plugin_ctx));
  }

  // build_screen() runs after init() so plugins that need their own
  // core-service refs (e.g. PrinterPlugin::ui_) have them set first.
  printer_plugin_.build_screen(ui_.printer_select_page_container(), 0);
  for (Plugin* plugin : plugins_) {
    if (plugin == nullptr) {
      continue;
    }
    const uint8_t page_count = plugin->page_count();
    if (page_count == 0) {
      continue;
    }
    const int base = ui_.register_plugin_pages(plugin->id(), page_count);
    for (uint8_t i = 0; i < page_count; ++i) {
      plugin->build_screen(ui_.plugin_page_container(base + i), i);
    }
  }

  ESP_LOGI(kTag, "Bootstrap complete");

  while (true) {
    const uint64_t now_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
    if (ui_.consume_portal_unlock_request()) {
      setup_portal_.request_unlock_pin();
    }

    // Portal PIN/hint push is core, independent of any plugin's enabled
    // state — a device running weather-only (printer disabled) still needs
    // the PIN overlay to work. See Ui::update_portal_access_visuals().
    const PortalAccessSnapshot portal_access = setup_portal_.access_snapshot();
    ui_.set_portal_access_state(portal_access.lock_enabled, portal_access.request_authorized,
                                portal_access.session_active, portal_access.pin_active,
                                portal_access.pin_code, portal_access.pin_remaining_s,
                                portal_access.session_remaining_s);
    ui_.set_core_wifi_state(wifi_manager_.is_station_connected(),
                            wifi_manager_.is_setup_access_point_active(),
                            wifi_manager_.station_ip());
    ui_.update_portal_access_visuals();

    for (Plugin* plugin : plugins_) {
      if (plugin == nullptr || !plugin->enabled()) {
        continue;
      }
      plugin->tick(now_ms);
    }
    for (Plugin* plugin : plugins_) {
      if (plugin == nullptr || !plugin->enabled()) {
        continue;
      }
      plugin->update_ui();
    }

    uint32_t min_poll_interval_ms = 1500;
    for (Plugin* plugin : plugins_) {
      if (plugin == nullptr || !plugin->enabled()) {
        continue;
      }
      min_poll_interval_ms = std::min(min_poll_interval_ms, plugin->desired_poll_interval_ms());
    }
    wait_for_next_iteration(ui_, pdMS_TO_TICKS(min_poll_interval_ms));
  }
}

}  // namespace infohub
