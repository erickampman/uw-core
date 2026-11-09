#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "uw/audio/AudioBackend.h"
#include "uw/audio/AuxRouting.h"

typedef unsigned int AudioObjectID;   // CoreAudio's (UInt32), so callers needn't include it

namespace uw {

// An output device CoreAudio offers, for --list-audio and --device.
struct AudioDeviceInfo {
    AudioObjectID id = 0;
    std::string   name;            // "MacBook Pro Speakers"
    std::string   uid;             // persistent across reboots: "BuiltInSpeakerDevice"
    std::uint32_t outputChannels = 0;
    double        sampleRate     = 0.0;   // nominal, now
    bool          isDefault      = false;   // the system's default output, now
};

// AudioBackend over a CoreAudio HAL IOProc (StageHand's docs/macos.md, "CoreAudio
// backend"). Outputs only (decision 2).
//
// The main device is chosen at construction (decision 12): a name, a UID, or a
// unique part of a name; "" for the system's default output as it is at open(),
// not followed afterwards. With `fallBackToDefault` (a device remembered from
// an earlier run), a choice that isn't there opens the default instead, and
// service() moves to the chosen one when it appears.
//
// The device keeps its own sample rate unless one is asked for; a requested
// buffer size is this process's IO buffer on it (other apps keep theirs), put
// back at close(). A block's time is the IOProc's "now", the period's start
// (decision 4).
//
// The engine renders into planar buffers of ours (always the outputs asked
// for); the IOProc interleaves them onto the device's first channels, across its
// streams in order (Float32, the HAL's virtual format). A channel the device
// lacks is dropped.
//
// The click (the aux outputs, AuxRouting): on the main device, onto its first
// two channels with the main outputs; on another device, through a private
// aggregate of the main device and that one (decision 3: the main device is the
// clock, the other is set to its rate and drift-compensated), which the IOProc
// then runs on. Moving the click to or from another device rebuilds it: a short
// gap, a setup action.
//
// Devices going away: the main one (unplugged, or the Mac asleep) leaves the
// engine without blocks until it's back, then everything resumes; the click's
// leaves the click unrouted until the controller routes it again
// (takeDevicesChanged()). Main-thread work happens in service().
class CoreAudioBackend final : public AudioBackend, public AuxRouting {
public:
    explicit CoreAudioBackend(std::string deviceChoice = {}, bool fallBackToDefault = false);
    ~CoreAudioBackend() override;

    // The private aggregate device's name, and the start of its UID (the
    // process id is added), e.g. "StageHand" and "com.unlikelyware.stagehand".
    // Call before open().
    void setAggregateIdentity(std::string name, std::string uidPrefix) {
        aggregateName      = std::move(name);
        aggregateUidPrefix = std::move(uidPrefix);
    }

    CoreAudioBackend(const CoreAudioBackend&)            = delete;
    CoreAudioBackend& operator=(const CoreAudioBackend&) = delete;

    // Every device with outputs, in CoreAudio's order.
    static std::vector<AudioDeviceInfo> outputDevices();
    // The device `choice` names (see the class comment), or false with
    // `problem` saying why (none, or several match).
    static bool findDevice(const std::string& choice, AudioDeviceInfo& found, std::string& problem);

    bool        open(const AudioConfig& requested, AudioConfig& negotiated) override;
    void        setProcessor(AudioProcessor* audioProcessor) override;
    bool        start() override;
    void        stop() override;
    void        close() override;
    AudioConfig config() const override;
    double      outputLatencySeconds() const override;
    const char* name() const override { return "CoreAudio"; }

    // --- AuxRouting: the system's output devices (ports "1", "2", ...).
    std::vector<PlaybackDevice> playbackDevices() override;
    bool                        connectAux(const PlaybackDevice& device) override;
    void                        disconnectAux() override;
    bool                        takeDevicesChanged() override;
    std::string                 mainDevice() override;

    // Main thread, often (the app's housekeeping timer): acts on devices going
    // and coming back, and on a chosen device appearing. Returns what to tell
    // the user.
    std::vector<std::string> service();

    // The main device (valid after open()).
    const AudioDeviceInfo& device() const noexcept { return main; }

    // The rates the main device offers (ascending; a continuous range gives
    // the common rates inside it), and the buffer sizes this process may ask
    // for on it, in frames. Main thread, after open().
    std::vector<double> sampleRates() const;
    bool                bufferSizeRange(std::uint32_t& minimum, std::uint32_t& maximum) const;

    // This process's buffer on the device, now, while playing (clamped to
    // bufferSizeRange()); close() puts back what was there before. The
    // plugins need nothing: a block is never longer than the buffer, and the
    // engine takes any length up to its maximum. Returns the frames it runs
    // at, or 0 if it isn't open. Main thread.
    std::uint32_t setBufferSize(std::uint32_t frames);
    // open() fell back to the default: the choice wasn't there (and is waited for).
    bool usingFallback() const noexcept { return !pendingChoice.empty(); }

    // --- diagnostics (any thread) ---
    std::uint64_t xrunCount() const noexcept { return overloads.load(std::memory_order_relaxed); }
    // The main device is gone: no blocks until it's back (service()).
    bool          deviceLost() const noexcept { return lost.load(std::memory_order_relaxed); }
    // Its nominal sample rate changed while open (another app, Audio MIDI
    // Setup): config() reports the new rate.
    bool          sampleRateChanged() const noexcept { return rateChanged.load(std::memory_order_relaxed); }
    std::int64_t  framesProcessed() const noexcept { return frameCounter.load(std::memory_order_relaxed); }

    // Called from CoreAudio's threads (the IOProc, the property listeners), by
    // functions in the .cpp. Not for other callers.
    void render(const void* now, void* outputBufferList) noexcept;   // AudioTimeStamp, AudioBufferList
    void onPropertyChanged(AudioObjectID object, std::uint32_t selector) noexcept;

private:
    bool setUp();      // the IOProc on main (or the aggregate with the click's device), at `wanted`
    void tearDown();   // stops and removes it (and the aggregate)
    bool restart();    // tearDown(), setUp(), and start again if it was running
    bool makeAggregate(const AudioDeviceInfo& other);
    void addListeners(AudioObjectID object);
    void removeListeners();

    std::string choice;
    bool        fallBack = false;
    std::string aggregateName      = "uw-core";
    std::string aggregateUidPrefix = "com.unlikelyware.uw-core";
    std::string pendingChoice;   // the chosen device, while the default stands in

    AudioDeviceInfo main;               // the main device
    std::string     clickUid;           // the click's other device, or "" (none, or the main one)
    bool            clickOnMain  = false;
    AudioObjectID   aggregate    = 0;   // ours, while the click is on another device
    AudioObjectID   ioDevice     = 0;   // what the IOProc runs on: main.id or aggregate
    std::uint32_t   mainChannels = 0;   // the main device's outputs (the aggregate's first)
    AudioObjectID   listenedTo   = 0;

    void*           procId    = nullptr;   // AudioDeviceIOProcID
    AudioProcessor* processor = nullptr;
    AudioConfig     wanted;               // what open() was asked for; its rate, what we run at once open
    AudioConfig     negotiatedConfig;
    std::uint32_t   restoreBufferSize = 0;   // this process's buffer on main before open() set one
    bool            isOpen            = false;
    bool            running           = false;   // start() asked for (the IOProc may be down: lost)
    bool            lostReported      = false;
    int                                   rateRestores = 0;   // the rate set back, lately (service())
    std::chrono::steady_clock::time_point firstRateRestore;

    // Planar buffers the engine renders into, sized for the largest buffer the
    // device allows, so the IOProc never allocates. Aux channel c goes onto
    // ioDevice channel auxChannels[c], mixed with what's there.
    std::uint32_t              capacityFrames = 0;
    std::vector<float>         planar;
    std::vector<float*>        outputPointers;
    std::vector<float*>        auxPointers;
    std::vector<std::uint32_t> auxChannels;   // empty: the click goes nowhere

    std::atomic<std::uint32_t> currentBufferSize{0};
    std::atomic<double>        currentSampleRate{0.0};
    std::atomic<std::int64_t>  frameCounter{0};   // written only by the IOProc
    std::atomic<std::uint64_t> overloads{0};
    std::atomic<bool>          lost{false};
    std::atomic<bool>          rateChanged{false};
    std::atomic<bool>          devicesChanged{false};       // the system's device list, for AuxRouting
    std::atomic<bool>          devicesChangedHere{false};   // the same, for service()
};

} // namespace uw
