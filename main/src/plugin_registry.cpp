#include "printsphere/plugin_registry.hpp"

namespace printsphere {

// No plugins registered yet — see plugin_registry.hpp.
std::array<Plugin*, kMaxPlugins> registered_plugins{};

}  // namespace printsphere
