#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "uw/ump/MidiMessage.h"

namespace uw {

// Opaque handle to an opened MIDI destination. kInvalidMidiDestination (0)
// means none.
using MidiDestinationHandle = std::uint32_t;
inline constexpr MidiDestinationHandle kInvalidMidiDestination = 0;

// Describes an available MIDI output port: somewhere messages can be sent.
struct MidiDestinationInfo {
    std::string  id;                            // stable identifier used to open it
    std::string  name;                          // human-readable, for UI
    bool         isVirtual  = false;            // provided by an app, not a device
    bool         isHardware = false;            // a physical device
    MidiProtocol protocol   = MidiProtocol::Midi1;   // the destination's native protocol
};

// The MIDI-output seam, beside MidiInput: the only component that knows
// CoreMIDI (CoreMidiOutput) or the ALSA sequencer (to come) for sending. Used
// by controllers such as KeyboardEDO; the apps will use it to play to hardware.
//
// It SENDS BUT NEVER TRANSLATES, as MidiInput wraps but never translates: the
// caller makes messages in the protocol protocol(handle) reports (MT 0x4 for
// MIDI 2.0, MT 0x2 for MIDI 1.0; uw::ump::downconvert can help), because only
// the caller knows how to fall back well (a microtonal note, for one, needs a
// different note number, not a dropped attribute). Protocol-neutral messages
// (utility, system, SysEx7) go to either.
//
// Lifecycle: open() destinations or openVirtual() sources -> send() ->
// close(). Everything, send() included, is called on one thread (the app's
// main thread); send() doesn't block.
class MidiOutput {
public:
    virtual ~MidiOutput() = default;

    // List currently available destinations, not including this output's own
    // virtual sources.
    virtual std::vector<MidiDestinationInfo> enumerate() = 0;

    // Open a destination by its MidiDestinationInfo::id. Returns
    // kInvalidMidiDestination on failure.
    virtual MidiDestinationHandle open(const std::string& destinationId) = 0;

    // Publish a named source that other apps connect to and receive from (e.g.
    // KeyEDO's "Publish to CoreMIDI"), sending `protocol`. The platform converts
    // for a receiver of the other protocol. Returns kInvalidMidiDestination on
    // failure.
    //
    // `persistentId`, if not 0, keeps the source the same device from one run
    // to the next where the platform has such an id (CoreMIDI's unique ID), so
    // hosts such as REAPER remember it instead of seeing a new device each
    // launch. Taken already (another instance has it): the platform's own id.
    virtual MidiDestinationHandle openVirtual(const std::string& name, MidiProtocol protocol,
                                              std::int32_t persistentId = 0) = 0;

    virtual void close(MidiDestinationHandle destination) = 0;

    // The protocol to send to `destination`: the destination's own, or a
    // virtual source's as opened. Midi1 for an unknown handle.
    virtual MidiProtocol protocol(MidiDestinationHandle destination) const = 0;

    // Send `count` messages, in order, now. Returns false if the handle isn't
    // open or the platform refused them.
    virtual bool send(MidiDestinationHandle destination, const MidiMessage* messages,
                      std::size_t count) = 0;

    // Hotplug, as MidiInput: changes() goes up when the system announces a
    // destination appearing or leaving, and sync() reconnects an opened one if
    // it came back. Virtual sources: always true.
    virtual std::uint64_t changes() const { return 0; }
    virtual bool sync(MidiDestinationHandle destination) {
        (void)destination;
        return true;
    }

    // Human-readable backend name, e.g. "CoreMIDI". For UI and logging.
    virtual const char* name() const = 0;
};

} // namespace uw
