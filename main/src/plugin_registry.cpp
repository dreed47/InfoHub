#include "infohub/plugin_registry.hpp"

namespace infohub {

// No plugins registered yet — see plugin_registry.hpp.
std::array<Plugin*, kMaxPlugins> registered_plugins{};

}  // namespace infohub
