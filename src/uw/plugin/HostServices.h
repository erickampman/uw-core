#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "uw/Types.h"

namespace uw {

using TimerId = std::uint32_t;
inline constexpr TimerId kInvalidTimer = 0;

// File-descriptor event bits. Values match CLAP_POSIX_FD_READ / _WRITE / _ERROR.
namespace FdEvent {
inline constexpr std::uint32_t Read  = 1u << 0;
inline constexpr std::uint32_t Write = 1u << 1;
inline constexpr std::uint32_t Error = 1u << 2;
} // namespace FdEvent

// Who the host is, as a plugin is told (CLAP's clap_host_t). Each app gives
// its own through HostServices::hostInfo().
struct HostInfo {
    std::string name    = "uw-core host";
    std::string vendor  = "Unlikelyware";
    std::string url;
    std::string version = "0.0.0";
};

// Callbacks a HostedPlugin uses to talk back to the host, passed in at creation.
// Per-editor requests (resize, show) go to the EditorHost given to
// HostedPlugin::openEditor() instead, since they concern one window.
class HostServices {
public:
    virtual ~HostServices() = default;

    // Ask the host to deactivate and reactivate the plugin (e.g. latency change).
    // Thread: main; the host schedules it, not necessarily synchronously.
    virtual void requestRestart() = 0;

    // Ask the host to schedule a process() call (e.g. to flush a tail).
    // Thread: any.
    virtual void requestProcess() = 0;

    // Ask the host to call flushParams() while the plugin is not processing.
    // Thread: any.
    virtual void requestParamFlush() = 0;

    // Host-side logging. Thread: any.
    virtual void log(LogLevel level, const char* message) = 0;

    // The host's name, vendor, URL and version, read when a plugin is created.
    // Thread: main.
    virtual HostInfo hostInfo() const { return {}; }

    // The plugin's state changed in a way a save would capture (e.g. an edit in
    // its editor; CLAP state.mark_dirty), so the rack has unsaved changes.
    // Thread: main.
    virtual void stateChanged() {}

    // --- main-loop services (main thread; callbacks also run on the main thread)
    // Plugin editors on Linux have no event loop of their own; they rely on the
    // host's (CLAP timer-support and posix-fd-support). The defaults mean "not
    // available", which a plugin must tolerate.

    // A periodic callback, roughly every `periodMs`. Returns kInvalidTimer if
    // unavailable.
    virtual TimerId addTimer(std::uint32_t /*periodMs*/, std::function<void()> /*callback*/) {
        return kInvalidTimer;
    }
    // Stop a timer. Safe to call from inside its own callback.
    virtual void removeTimer(TimerId /*id*/) {}

    // Call `callback` with the FdEvent bits that fired whenever `fd` is ready for
    // `events`. Returns false if unavailable or `fd` is already watched.
    virtual bool watchFd(int /*fd*/, std::uint32_t /*events*/,
                         std::function<void(std::uint32_t)> /*callback*/) {
        return false;
    }
    virtual bool modifyFd(int /*fd*/, std::uint32_t /*events*/) { return false; }
    // Stop watching. Safe to call from inside its own callback.
    virtual void unwatchFd(int /*fd*/) {}
};

} // namespace uw
