#include "uw/clap/ClapPluginFactory.h"

#include <clap/clap.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string_view>

#include "uw/clap/ClapModule.h"
#include "uw/clap/ClapPlugin.h"

namespace uw {

namespace fs = std::filesystem;

ClapPluginFactory::ClapPluginFactory() {
#ifdef __APPLE__
    if (const char* home = std::getenv("HOME")) paths.push_back(std::string(home) + "/Library/Audio/Plug-Ins/CLAP");
    paths.emplace_back("/Library/Audio/Plug-Ins/CLAP");
#else
    if (const char* home = std::getenv("HOME")) paths.push_back(std::string(home) + "/.clap");
    paths.emplace_back("/usr/lib/clap");
#endif
    if (const char* extra = std::getenv("CLAP_PATH")) {
        std::stringstream list(extra);
        std::string       dir;
        while (std::getline(list, dir, ':'))
            if (!dir.empty()) paths.push_back(dir);
    }
}

std::vector<PluginDescriptor> ClapPluginFactory::scan() {
    std::vector<PluginDescriptor> found;
    for (const std::string& dir : paths) {
        std::error_code ec;
        fs::recursive_directory_iterator it(
            dir, fs::directory_options::follow_directory_symlink | fs::directory_options::skip_permission_denied,
            ec);
        for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
            std::error_code fileError;
            if (it->path().extension() != ".clap") continue;
#ifdef __APPLE__
            // A bundle: a folder, the plugin itself; nothing to find inside it.
            if (!fs::is_directory(it->path(), fileError)) continue;
            it.disable_recursion_pending();
            if (std::string problem; !ClapModule::isLoadableHere(it->path().string(), problem)) {
                std::fprintf(stderr, "CLAP: %s: %s\n", it->path().filename().string().c_str(), problem.c_str());
                continue;
            }
#else
            if (!fs::is_regular_file(it->path(), fileError)) continue;
#endif
            std::vector<PluginDescriptor> more = scanFile(it->path().string());
            found.insert(found.end(), more.begin(), more.end());
        }
    }
    return found;
}

namespace {

// CLAP's features: an instrument says "instrument"; an audio effect says
// "audio-effect" (or "analyzer"); a note effect says "note-effect". No
// features at all: taken for an instrument.
PluginKind kindOf(const char* const* features) {
    if (features == nullptr || features[0] == nullptr) return PluginKind::Instrument;
    bool effect = false, note = false;
    for (const char* const* f = features; *f != nullptr; ++f) {
        const std::string_view feature = *f;
        if (feature == CLAP_PLUGIN_FEATURE_INSTRUMENT) return PluginKind::Instrument;
        effect = effect || feature == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT || feature == CLAP_PLUGIN_FEATURE_ANALYZER;
        note   = note || feature == CLAP_PLUGIN_FEATURE_NOTE_EFFECT;
    }
    return effect ? PluginKind::Effect : note ? PluginKind::Other : PluginKind::Instrument;
}

} // namespace

std::vector<PluginDescriptor> ClapPluginFactory::scanFile(const std::string& path) {
    std::vector<PluginDescriptor> found;
    std::error_code   ec;
    const std::string absolute = fs::absolute(path, ec).string();
    const auto        loaded   = module(absolute);
    if (!loaded) return found;

    const clap_plugin_factory_t* factory = loaded->factory();
    const std::uint32_t          count   = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory, i);
        if (d == nullptr || !clap_version_is_compatible(d->clap_version)) continue;
        PluginDescriptor desc;
        desc.format  = PluginFormat::Clap;
        desc.id      = d->id != nullptr ? d->id : "";
        desc.name    = d->name != nullptr ? d->name : "";
        desc.vendor  = d->vendor != nullptr ? d->vendor : "";
        desc.version = d->version != nullptr ? d->version : "";
        desc.path    = absolute;
        desc.kind    = kindOf(d->features);
        found.push_back(desc);
    }
    return found;
}

std::unique_ptr<HostedPlugin> ClapPluginFactory::create(const PluginDescriptor& desc, HostServices& host) {
    if (desc.format != PluginFormat::Clap) return nullptr;
    std::shared_ptr<ClapModule> loaded = module(desc.path);
    if (!loaded) return nullptr;
    return ClapPlugin::create(std::move(loaded), desc, host);
}

std::shared_ptr<ClapModule> ClapPluginFactory::module(const std::string& path) {
    if (std::shared_ptr<ClapModule> existing = modules[path].lock()) return existing;
    std::shared_ptr<ClapModule> loaded = ClapModule::load(path);
    modules[path] = loaded;
    return loaded;
}

} // namespace uw
