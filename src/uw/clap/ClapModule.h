#pragma once

#include <memory>
#include <string>

struct clap_plugin_entry;
struct clap_plugin_factory;

namespace uw {

// One loaded .clap: a file dlopen'd on Linux, a bundle loaded through CFBundle
// on macOS. Its clap_entry is initialized once (with the file's or bundle's
// path) and the module shared (via shared_ptr) by every plugin instance created
// from it. The last user's release runs clap_entry.deinit() (and on Linux
// dlclose()).
class ClapModule {
public:
    // Load and initialize. Returns null (after printing why) on failure.
    static std::shared_ptr<ClapModule> load(const std::string& path);

    // Can this process load it at all? macOS: false, with `problem` saying why,
    // for a bundle with no arm64 binary ("Intel only", decision 13), checked
    // without loading it. Linux: true (loading says why, if not).
    static bool isLoadableHere(const std::string& path, std::string& problem);
    ~ClapModule();

    ClapModule(const ClapModule&)            = delete;
    ClapModule& operator=(const ClapModule&) = delete;

    const clap_plugin_factory* factory() const noexcept { return pluginFactory; }
    const std::string&         path() const noexcept { return modulePath; }

private:
    ClapModule() = default;

    std::string                modulePath;
    void*                      handle        = nullptr;   // dlopen's, or a CFBundleRef
    const clap_plugin_entry*   entry         = nullptr;
    const clap_plugin_factory* pluginFactory = nullptr;
    bool                       initialized   = false;
};

} // namespace uw
