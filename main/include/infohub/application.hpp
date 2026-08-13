#pragma once

#include <array>

#include "infohub/audio_notifier.hpp"
#include "infohub/config_store.hpp"
#include "infohub/plugin.hpp"
#include "infohub/pmu.hpp"
#include "infohub/setup_portal.hpp"
#include "infohub/serial_provisioner.hpp"
#include "infohub/ui.hpp"
#include "infohub/wifi_manager.hpp"
#include "freertos/FreeRTOS.h"
#if CONFIG_INFOHUB_PLUGIN_PRINTER
#include "infohub/plugins/printer/printer_plugin.hpp"
#endif
#if CONFIG_INFOHUB_PLUGIN_WEATHER
#include "infohub/plugins/weather/weather_plugin.hpp"
#endif
#if CONFIG_INFOHUB_PLUGIN_STOCKS
#include "infohub/plugins/stocks/stocks_plugin.hpp"
#endif

namespace infohub {

class Application {
 public:
  Application();
  void run();

 private:
  // Phase 8a: printer-domain state/clients/logic all live on PrinterPlugin
  // now (see printer_plugin.hpp) — Application only owns core services and
  // drives the plugin array generically. No more friend access, no more
  // printer_plugin_* delegate methods.
  ConfigStore config_store_{};
  WifiManager wifi_manager_{};
  Ui ui_{};
  PmuManager pmu_manager_{};
  AudioNotifier audio_notifier_{};
#if CONFIG_INFOHUB_PLUGIN_PRINTER
  PrinterPlugin printer_plugin_{};
#endif
#if CONFIG_INFOHUB_PLUGIN_WEATHER
  WeatherPlugin weather_plugin_{};
#endif
#if CONFIG_INFOHUB_PLUGIN_STOCKS
  StocksPlugin stocks_plugin_{};
#endif
  SetupPortal setup_portal_;
  SerialProvisioner serial_provisioner_;
  std::array<Plugin*, kMaxPlugins> plugins_{};
};

}  // namespace infohub
