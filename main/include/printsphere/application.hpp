#pragma once

#include <array>

#include "printsphere/audio_notifier.hpp"
#include "printsphere/config_store.hpp"
#include "printsphere/plugin.hpp"
#include "printsphere/pmu.hpp"
#include "printsphere/printer_plugin.hpp"
#include "printsphere/setup_portal.hpp"
#include "printsphere/serial_provisioner.hpp"
#include "printsphere/ui.hpp"
#include "printsphere/wifi_manager.hpp"
#include "freertos/FreeRTOS.h"

namespace printsphere {

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
  // Declared before setup_portal_ so its clients exist first — SetupPortal's
  // constructor still takes direct client references (Phase 8d, deferred).
  PrinterPlugin printer_plugin_{};
  SetupPortal setup_portal_;
  SerialProvisioner serial_provisioner_;
  std::array<Plugin*, kMaxPlugins> plugins_{};
};

}  // namespace printsphere
