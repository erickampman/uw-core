#include "uw/clap/ClapModule.h"

#include <clap/clap.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#else
#include <dlfcn.h>
#endif

#include <cstdio>

namespace uw {

#ifdef __APPLE__

// A .clap on macOS is a bundle (a folder); CFBundle finds and loads the binary
// inside it (StageHand's docs/macos.md, "Plugins").
static CFBundleRef openBundle(const std::string& path) {
    const CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), true);
    if (url == nullptr) return nullptr;
    const CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
    CFRelease(url);
    return bundle;
}

bool ClapModule::isLoadableHere(const std::string& path, std::string& problem) {
    const CFBundleRef bundle = openBundle(path);
    if (bundle == nullptr) {
        problem = "not a bundle";
        return false;
    }
    bool             arm64 = false, intel = false;
    const CFArrayRef archs = CFBundleCopyExecutableArchitectures(bundle);
    if (archs != nullptr) {
        for (CFIndex i = 0; i < CFArrayGetCount(archs); ++i) {
            int arch = 0;
            CFNumberGetValue(static_cast<CFNumberRef>(CFArrayGetValueAtIndex(archs, i)), kCFNumberIntType, &arch);
            arm64 = arm64 || arch == kCFBundleExecutableArchitectureARM64;
            intel = intel || arch == kCFBundleExecutableArchitectureX86_64;
        }
        CFRelease(archs);
    }
    CFRelease(bundle);
    if (arm64) return true;
    // An arm64 process can't load an x86_64 binary (decision 13): say so, not "broken".
    problem = intel ? "Intel only: not loadable on Apple Silicon" : "no binary for this Mac";
    return false;
}

#else

bool ClapModule::isLoadableHere(const std::string&, std::string&) {
    return true;   // dlopen() says why, if not
}

#endif

std::shared_ptr<ClapModule> ClapModule::load(const std::string& path) {
    std::shared_ptr<ClapModule> module(new ClapModule());
    module->modulePath = path;

#ifdef __APPLE__
    if (std::string problem; !isLoadableHere(path, problem)) {
        std::fprintf(stderr, "CLAP: %s: %s\n", path.c_str(), problem.c_str());
        return nullptr;
    }
    const CFBundleRef bundle = openBundle(path);
    module->handle           = bundle;
    CFErrorRef error         = nullptr;
    if (bundle == nullptr || !CFBundleLoadExecutableAndReturnError(bundle, &error)) {
        std::fprintf(stderr, "CLAP: cannot load %s\n", path.c_str());
        if (error != nullptr) {
            CFShow(error);   // the loader's reason, to stderr
            CFRelease(error);
        }
        return nullptr;
    }
    module->entry = static_cast<const clap_plugin_entry_t*>(CFBundleGetDataPointerForName(bundle, CFSTR("clap_entry")));
#else
    module->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module->handle == nullptr) {
        std::fprintf(stderr, "CLAP: cannot load %s: %s\n", path.c_str(), dlerror());
        return nullptr;
    }
    module->entry = static_cast<const clap_plugin_entry_t*>(dlsym(module->handle, "clap_entry"));
#endif
    if (module->entry == nullptr) {
        std::fprintf(stderr, "CLAP: %s has no clap_entry\n", path.c_str());
        return nullptr;
    }
    if (!clap_version_is_compatible(module->entry->clap_version)) {
        std::fprintf(stderr, "CLAP: %s uses an incompatible CLAP version\n", path.c_str());
        return nullptr;
    }
    if (!module->entry->init(path.c_str())) {
        std::fprintf(stderr, "CLAP: %s failed to initialize\n", path.c_str());
        return nullptr;
    }
    module->initialized   = true;
    module->pluginFactory = static_cast<const clap_plugin_factory_t*>(
        module->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (module->pluginFactory == nullptr) {
        std::fprintf(stderr, "CLAP: %s has no plugin factory\n", path.c_str());
        return nullptr;
    }
    return module;
}

ClapModule::~ClapModule() {
    if (initialized) entry->deinit();
#ifdef __APPLE__
    // The binary stays loaded (as most hosts leave it): unloading code a plugin
    // may still have threads or callbacks in is the riskier choice.
    if (handle != nullptr) CFRelease(static_cast<CFBundleRef>(handle));
#else
    if (handle != nullptr) dlclose(handle);
#endif
}

} // namespace uw
