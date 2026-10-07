#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "uw/midi/MidiInput.h"

typedef unsigned int MIDIObjectRef;   // CoreMIDI's handles (UInt32), so callers needn't include it
typedef struct __CFRunLoop* CFRunLoopRef;

namespace uw {

// MidiInput over CoreMIDI's UMP API (StageHand's docs/macos.md, "CoreMIDI input"). Like
// AlsaSeqInput's two clients, it has two input ports, because CoreMIDI fixes
// the protocol per port, not per connection, and would otherwise convert:
//   "MIDI 1.0" port   MIDI 1.0 sources connect here and arrive as MT 0x2/0x1/0x3
//                     (CoreMIDI wraps the bytes, losslessly).
//   "MIDI 2.0" port   MIDI 2.0 sources connect here and arrive as MT 0x4, untouched.
// open() picks the port from the source's own protocol (kMIDIPropertyProtocolID)
// and the global preference; each connection's refCon names its handle.
//
// Ids have the ALSA shape, "<device>:<endpoint>" ("KeyLab 88:KeyLab 88 MIDI"), so
// a rack made on Linux mostly finds its devices here (decision 5). A source with
// no device (another app's virtual source, e.g. KeyEDO) is just "<endpoint>".
//
// Messages arrive on CoreMIDI's own high-priority thread: no thread of ours for
// MIDI. Each is timestamped with nowNanoseconds() as it reaches us, as ALSA's
// are when read: never earlier than it reaches the engine's queue, which the
// engine's one-period placement needs (the packet's own host time is earlier,
// by CoreMIDI's delivery, and made messages "late").
//
// Hotplug: CoreMIDI posts its notifications on the run loop of the thread that
// made the client. So that they come whatever loop the app runs (a sleep loop,
// DGL's, none), the client is made on a small thread of ours that runs a
// CFRunLoop and only counts notifications (changes()).
class CoreMidiInput final : public MidiInput {
public:
    // Makes the client, its two ports and the notification thread. Returns null
    // (after printing why) if CoreMIDI can't be reached.
    static std::unique_ptr<CoreMidiInput> create(const std::string& clientName);   // the app's name, as CoreMIDI shows it
    ~CoreMidiInput() override;

    CoreMidiInput(const CoreMidiInput&)            = delete;
    CoreMidiInput& operator=(const CoreMidiInput&) = delete;

    void                        setProtocolPreference(MidiProtocol pref) override;
    void                        setSink(MidiSink* midiSink) override;
    std::vector<MidiSourceInfo> enumerate() override;
    MidiSourceHandle            open(const std::string& sourceId) override;
    MidiSourceHandle            openVirtual(const std::string& name,
                                            MidiProtocol protocol) override;
    void                        close(MidiSourceHandle source) override;
    std::uint64_t               changes() const override { return announced.load(std::memory_order_acquire); }
    bool                        sync(MidiSourceHandle source) override;
    const char*                 name() const override { return "CoreMIDI"; }

    // --- diagnostics (the same as AlsaSeqInput's, for main.cpp) ---
    const char*   threadDescription() const noexcept { return "CoreMIDI's (real-time)"; }
    std::uint64_t overrunCount() const noexcept { return 0; }   // CoreMIDI doesn't report any
    // Where a handle's messages come in, e.g. "port '<clientName> MIDI 2.0' (MIDI 2.0)".
    // Main thread.
    std::string describe(MidiSourceHandle source) const;

    // The message receiver for one handle: the refCon CoreMIDI hands back with
    // each event list. Never freed before the input, so a late callback for a
    // closed handle finds `handle` cleared rather than freed memory.
    struct Receiver {
        std::atomic<MidiSourceHandle> handle{kInvalidMidiSource};
    };
    // Delivers one MIDIEventList to the sink. Called on CoreMIDI's thread, from
    // the receive blocks; not for other callers.
    void receive(const void* eventList, Receiver* receiver) noexcept;

private:
    enum { kMidi1Port = 0, kMidi2Port = 1, kPortCount = 2 };

    struct Source {
        MidiSourceInfo info;
        MIDIObjectRef  endpoint = 0;
        std::int32_t   uniqueId = 0;
    };
    struct OpenPort {
        int           portIndex   = 0;
        bool          isVirtual   = false;
        std::string   sourceId;            // what open() was given, to find it again
        std::int32_t  uniqueId    = 0;     // the source's, to find it first
        MIDIObjectRef connected   = 0;     // the source endpoint connected now, or 0
        MIDIObjectRef destination = 0;     // a virtual input's own endpoint
        std::string   portName;            // for describe()
        Receiver*     receiver    = nullptr;
    };

    CoreMidiInput() = default;

    std::vector<Source> listSources() const;
    bool                findSource(const std::string& sourceId, std::int32_t uniqueId, Source& found) const;
    int                 portFor(MidiProtocol protocol) const;
    Receiver*           newReceiver();
    MidiSourceHandle    registerPort(OpenPort open);

    MIDIObjectRef client = 0;
    MIDIObjectRef ports[kPortCount] = {0, 0};
    std::string   portNames[kPortCount];

    std::vector<std::unique_ptr<Receiver>> receivers;    // main thread; see Receiver
    std::map<MidiSourceHandle, OpenPort>   openPorts;    // main thread only
    MidiSourceHandle                       nextHandle = 1;
    MidiProtocol                           preference = MidiProtocol::Midi2;

    std::atomic<MidiSink*>     sink{nullptr};
    std::atomic<std::uint64_t> announced{0};   // CoreMIDI notifications, counted: changes()

    std::thread               notifyThread;
    std::atomic<CFRunLoopRef> notifyLoop{nullptr};
    std::atomic<bool>         quit{false};
};

} // namespace uw
