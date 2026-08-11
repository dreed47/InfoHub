#pragma once

#include <array>

#include "infohub/plugin.hpp"

namespace infohub {

// Compile-time plugin registration table. Empty for now — this is scaffolding
// only (see the "Phased extraction sequencing plan" in CLAUDE.md). Each entry
// will be gated by its own Kconfig bool once real plugins land, e.g.:
//
//   #if CONFIG_INFOHUB_PLUGIN_PRINTER
//     &g_printer_plugin,
//   #endif
//
// NOT YET WIRED IN: Application does not iterate this array yet.
extern std::array<Plugin*, kMaxPlugins> registered_plugins;

}  // namespace infohub
