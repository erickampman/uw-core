#pragma once

#include <string>

namespace uw {

enum class PluginFormat { Clap, Vst3, Auv2 };

// What a plugin is, from its format's own tags (CLAP's features): it decides
// where it's offered. Instruments are rack rows; audio effects belong on the
// rig's buses (StageHand's docs/rig.md); Other is anything else (a note effect, say).
// Plugins that don't say are taken for instruments, so they stay usable.
enum class PluginKind { Instrument, Effect, Other };

// Format-neutral identity of a plugin: enough to list it and to re-instantiate
// the exact plugin later from a saved Rack. `id` is an opaque, format-tagged
// string (a CLAP string id, or a VST3 128-bit TUID rendered as hex).
struct PluginDescriptor {
    PluginFormat format = PluginFormat::Clap;
    std::string  id;
    std::string  name;
    std::string  vendor;
    std::string  version;
    std::string  path;    // file/bundle to load
    PluginKind   kind = PluginKind::Instrument;   // from the scan; not saved in a rack
};

} // namespace uw
