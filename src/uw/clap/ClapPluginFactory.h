#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "uw/plugin/PluginFactory.h"

namespace uw {

class ClapModule;

// Finds and instantiates CLAP plugins. Searches CLAP's standard locations
// (Linux: ~/.clap, /usr/lib/clap; macOS: ~/Library/Audio/Plug-Ins/CLAP,
// /Library/Audio/Plug-Ins/CLAP) plus $CLAP_PATH (colon-separated), recursively,
// for .clap files (Linux) or bundles (macOS; one built for Intel only is named
// as such, not loaded). A loaded .clap module is shared by every plugin
// created from it.
class ClapPluginFactory final : public PluginFactory {
public:
    ClapPluginFactory();

    PluginFormat                  format() const override { return PluginFormat::Clap; }
    std::vector<PluginDescriptor> scan() override;
    std::unique_ptr<HostedPlugin> create(const PluginDescriptor& desc, HostServices& host) override;

    // The plugins in one .clap file.
    std::vector<PluginDescriptor> scanFile(const std::string& path);

    const std::vector<std::string>& searchPaths() const noexcept { return paths; }

private:
    std::shared_ptr<ClapModule> module(const std::string& path);

    std::vector<std::string>                          paths;
    std::map<std::string, std::weak_ptr<ClapModule>> modules;
};

} // namespace uw
