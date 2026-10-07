#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "uw/ump/MidiMessage.h"

namespace uw {

// Describes an available MIDI input port.
struct MidiSourceInfo {
    std::string  id;                            // stable identifier used to open it
    std::string  name;                          // human-readable, for UI
    bool         isVirtual  = false;            // provided by an app, not the kernel
    bool         isHardware = false;            // a physical device (not e.g. Midi Through)
    MidiProtocol protocol   = MidiProtocol::Midi1;   // the source's native protocol
};

// Implemented by the engine. Called from the MIDI thread when a message
// arrives. Implementations MUST be non-blocking (push to a lock-free queue and
// return; no locks, no allocation).
class MidiSink {
public:
    virtual ~MidiSink() = default;
    virtual void onMidi(MidiSourceHandle source,
                        const TimedMidiMessage& msg) noexcept = 0;
};

// The MIDI-input seam. The only component that knows ALSA (AlsaSeqInput: the
// sequencer in UMP mode, with one MIDI 1.0 client and one MIDI 2.0 client —
// StageHand's docs/architecture.md section 1) or CoreMIDI (CoreMidiInput: two input ports —
// StageHand's docs/macos.md). Messages come on a non-RT MIDI thread (its own, or CoreMIDI's),
// timestamped at capture, and go to the sink.
//
// It WRAPS BUT NEVER TRANSLATES: MIDI 1.0 sources arrive as MT 0x2/0x1/0x3 (a
// lossless wrap), MIDI 2.0 sources as MT 0x4. Routing and protocol translation
// are the engine's job. The one exception is the global protocol preference set
// to Midi1, which deliberately receives every source as MIDI 1.0.
//
// Lifecycle: setProtocolPreference() / setSink() -> open() sources -> [events
// on the MIDI thread] -> close(). Everything except sink delivery is
// main-thread.
class MidiInput {
public:
    virtual ~MidiInput() = default;

    // App-wide (Logic-style) protocol preference. Midi2 (default): UMP-native
    // sources are received as MIDI 2.0, legacy sources as MIDI 1.0. Midi1: every
    // source is received as MIDI 1.0 (a compatibility safety valve). Applies to
    // sources opened afterwards; reopen existing sources to change them.
    virtual void setProtocolPreference(MidiProtocol preference) = 0;

    // Where captured messages are delivered. Set before opening any source.
    virtual void setSink(MidiSink* sink) = 0;

    // List currently available input sources.
    virtual std::vector<MidiSourceInfo> enumerate() = 0;

    // Open a source by its MidiSourceInfo::id. Returns kInvalidMidiSource on
    // failure. Multiple sources may be open at once.
    virtual MidiSourceHandle open(const std::string& sourceId) = 0;

    // Create a named input port that other apps can connect to (e.g. a KeyEDO
    // port or a MIDI 2.0 test tool), receiving in `protocol`. Returns
    // kInvalidMidiSource on failure. Unlike open(), the connecting app's protocol
    // isn't known here: an app of the other protocol is converted by the
    // platform (on ALSA, a MIDI 1.0 app on a MIDI 2.0 port loses MSB-only
    // RPN/NRPN data entry — StageHand's docs/architecture.md section 1). Apps should connect
    // to the port matching their own protocol.
    virtual MidiSourceHandle openVirtual(const std::string& name,
                                         MidiProtocol protocol) = 0;

    virtual void close(MidiSourceHandle source) = 0;

    // Devices come and go (hotplug, StageHand's docs/command-line.md "MIDI devices coming
    // and going"). changes() goes up whenever the system announces a source
    // appearing or leaving; the main thread compares it with the value it saw
    // last, then calls sync() for each source it opened. Thread-safe.
    virtual std::uint64_t changes() const { return 0; }
    // An opened device: true if it's connected now. One that left and came
    // back (the same id) is connected again, under the same handle, so
    // whatever uses the handle hears it again. Virtual inputs: always true.
    virtual bool sync(MidiSourceHandle source) {
        (void)source;
        return true;
    }

    // Human-readable backend name, e.g. "ALSA sequencer". For UI and logging.
    virtual const char* name() const = 0;
};

} // namespace uw
