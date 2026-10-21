#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "uw/base/AudioBuffer.h"
#include "uw/base/Event.h"
#include "uw/base/TimeInfo.h"
#include "uw/plugin/Editor.h"
#include "uw/plugin/Parameter.h"
#include "uw/plugin/PluginDescriptor.h"
#include "uw/plugin/PortLayout.h"

namespace uw {

// Everything a plugin needs to process one block. By the time this is built the
// host has already mapped MIDI to sample offsets and merged param events into
// `eventsIn` (time-sorted). `eventsOut` may be null if the host is not
// collecting feedback this block.
struct ProcessContext {
    std::uint32_t    frameCount = 0;
    TimeInfo         time;
    ConstAudioBuffer audioIn;              // may be empty (channelCount == 0)
    AudioBuffer      audioOut;
    EventBlock       eventsIn;             // time-sorted MIDI + param events
    EventSink*       eventsOut = nullptr;  // plugin pushes param feedback here
};

// The plugin seam. The only place that knows CLAP (initial) or VST3 (later);
// each format's quirks live in its impl (StageHand's docs/architecture.md 3a). One
// HostedPlugin wraps one plugin instance, shared by every output format the host
// exposes.
//
// Lifecycle: create (via PluginFactory) -> loadState (optional, BEFORE activate)
// -> activate -> [process on the audio thread] -> stopProcessing (audio thread)
// -> deactivate -> destroy. Destroying an active plugin deactivates it first.
class HostedPlugin {
public:
    virtual ~HostedPlugin() = default;

    // --- identity / introspection (main thread) ---
    virtual const PluginDescriptor&    descriptor() const = 0;
    virtual PortLayout                 portLayout() const = 0;
    virtual std::vector<ParameterInfo> parameters() const = 0;

    // --- lifecycle (main thread) ---
    // Prepare to process at `sampleRate` with at most `maxBlockFrames` per block.
    // Returns false on failure.
    virtual bool activate(double sampleRate, std::uint32_t maxBlockFrames) = 0;
    virtual void deactivate() = 0;

    // --- real-time processing (audio thread) ---
    // Consume ctx.eventsIn, render ctx.audioOut, emit feedback via ctx.eventsOut.
    // The first call after activate() (or after stopProcessing()) starts
    // processing implicitly (CLAP start_processing); if that fails, the plugin
    // outputs silence. No locks, allocation, syscalls, or blocking.
    virtual void process(const ProcessContext& ctx) noexcept = 0;

    // End processing (CLAP stop_processing): the engine calls this on the audio
    // thread when the plugin leaves the running graph, before the main thread
    // may deactivate it. No-op if not processing.
    virtual void stopProcessing() noexcept = 0;

    // Clear the plugin's audio memory (a reverb's tail, a delay's buffer) and
    // sounding voices: a Panic's second press (StageHand's docs/panic.md). Audio thread,
    // while active; CLAP's reset(). No-op by default.
    virtual void reset() noexcept {}

    // --- parameters ---
    // Current plain value of a parameter (main thread, for UI). NOTE: to change a
    // parameter audibly, the engine sends a ParamValue Event into process();
    // there is deliberately no direct RT setter here.
    virtual double paramValue(std::uint32_t id) const = 0;
    // A value of parameter `id` as the plugin writes it ("440 Hz", "Saw"),
    // for a host's own controls (main thread). False: the plugin has no text
    // for it, and the host shows the number.
    virtual bool paramValueText(std::uint32_t /*id*/, double /*value*/, std::string& /*text*/) const { return false; }

    // Apply parameter changes / collect feedback while NOT processing (main
    // thread) — the stopped-transport path (CLAP params.flush).
    virtual void flushParams(const EventBlock& in, EventSink* out) = 0;

    // --- state (main thread) ---
    // Serialize opaque plugin state into `out` (appends). For VST3 this wraps
    // both component and controller state. Returns false on failure.
    virtual bool saveState(std::vector<std::byte>& out) const = 0;
    // Restore from a blob previously produced by saveState(). Call BEFORE the
    // plugin joins the live graph (StageHand's docs/domain-model.md, save/restore).
    virtual bool loadState(const std::byte* data, std::size_t size) = 0;

    // --- main-thread servicing ---
    // Called regularly from the main loop: runs work the plugin asked to have
    // done on the main thread (CLAP request_callback -> on_main_thread, and a
    // requested params flush while inactive).
    virtual void idle() = 0;

    // --- editor (main thread; optional) ---
    // An editor that embeds in a host window. Floating editors aren't supported.
    // The defaults mean "no editor". Close the editor before the plugin is
    // destroyed.

    // True if the plugin has an editor this platform's window system can embed.
    virtual bool hasEditor() const { return false; }

    // Create the editor inside `parent` and show it; `size` receives its initial
    // size. `host` receives the plugin's requests until closeEditor(). Returns
    // false, with nothing left open, on failure.
    virtual bool openEditor(NativeWindow /*parent*/, EditorHost& /*host*/, EditorSize& /*size*/) {
        return false;
    }

    // Hide and destroy the editor. No-op if none is open.
    virtual void closeEditor() {}

    // Whether the user may resize the editor.
    virtual bool editorResizable() const { return false; }

    // The host window changed size. The plugin may adjust `size` to one it
    // supports; the host then fits its window to that. Returns false if the
    // editor can't be resized.
    virtual bool setEditorSize(EditorSize& /*size*/) { return false; }
};

} // namespace uw
