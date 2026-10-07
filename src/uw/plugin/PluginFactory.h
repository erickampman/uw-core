#pragma once

#include <memory>
#include <vector>

#include "uw/plugin/HostedPlugin.h"
#include "uw/plugin/PluginDescriptor.h"

namespace uw {

class HostServices;

// Discovers and instantiates plugins of one format. One impl per format
// (ClapPluginFactory initial; Vst3PluginFactory later). The only component that
// knows a format's on-disk layout and entry point.
class PluginFactory {
public:
    virtual ~PluginFactory() = default;

    virtual PluginFormat format() const = 0;

    // Scan the standard locations (and any configured extra paths) for plugins
    // of this format.
    virtual std::vector<PluginDescriptor> scan() = 0;

    // Instantiate a plugin. `host` provides the callbacks the plugin may use for
    // its lifetime. Returns null on failure.
    virtual std::unique_ptr<HostedPlugin> create(const PluginDescriptor& desc,
                                                 HostServices& host) = 0;
};

} // namespace uw
