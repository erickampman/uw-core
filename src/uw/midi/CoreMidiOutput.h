#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "uw/midi/MidiOutput.h"

typedef unsigned int MIDIObjectRef;   // CoreMIDI's handles (UInt32), so callers needn't include it
typedef struct __CFRunLoop* CFRunLoopRef;

namespace uw {

// MidiOutput over CoreMIDI's UMP API (MIDISendEventList, MIDIReceivedEventList).
//
// One output port sends to every opened destination. A destination's protocol
// is its kMIDIPropertyProtocolID; each run of messages goes out in an event
// list of its own protocol, so CoreMIDI passes it through untouched when the
// caller has matched the destination, as the seam asks. A virtual source is
// made with MIDISourceCreateWithProtocol, and CoreMIDI converts for receivers
// of the other protocol.
//
// Ids have CoreMidiInput's shape, "<device>:<endpoint>", or just "<endpoint>"
// for another app's virtual destination.
//
// Hotplug: as CoreMidiInput, the client is made on a small thread of ours that
// runs a CFRunLoop and only counts notifications (changes()).
class CoreMidiOutput final : public MidiOutput {
public:
    // Makes the client, its output port and the notification thread. Returns
    // null (after printing why) if CoreMIDI can't be reached.
    static std::unique_ptr<CoreMidiOutput> create(const std::string& clientName);
    ~CoreMidiOutput() override;

    CoreMidiOutput(const CoreMidiOutput&)            = delete;
    CoreMidiOutput& operator=(const CoreMidiOutput&) = delete;

    std::vector<MidiDestinationInfo> enumerate() override;
    MidiDestinationHandle            open(const std::string& destinationId) override;
    MidiDestinationHandle            openVirtual(const std::string& name, MidiProtocol protocol) override;
    void                             close(MidiDestinationHandle destination) override;
    MidiProtocol                     protocol(MidiDestinationHandle destination) const override;
    bool                             send(MidiDestinationHandle destination, const MidiMessage* messages,
                                          std::size_t count) override;
    std::uint64_t                    changes() const override { return announced.load(std::memory_order_acquire); }
    bool                             sync(MidiDestinationHandle destination) override;
    const char*                      name() const override { return "CoreMIDI"; }

    // Where a handle's messages go, e.g. "destination 'IAC Driver:Bus 1' (MIDI 1.0)".
    std::string describe(MidiDestinationHandle destination) const;

private:
    struct Destination {
        MidiDestinationInfo info;
        MIDIObjectRef       endpoint = 0;
        std::int32_t        uniqueId = 0;
    };
    struct Opened {
        bool          isVirtual = false;
        std::string   id;                  // what open() was given, or the virtual source's name
        std::int32_t  uniqueId  = 0;       // the destination's, to find it first
        MIDIObjectRef endpoint  = 0;       // the destination now, or the virtual source; 0 if gone
        MidiProtocol  protocol  = MidiProtocol::Midi1;
    };

    CoreMidiOutput() = default;

    std::vector<Destination> listDestinations() const;
    bool findDestination(const std::string& id, std::int32_t uniqueId, Destination& found) const;

    MIDIObjectRef client = 0;
    MIDIObjectRef port   = 0;

    std::map<MidiDestinationHandle, Opened> opened;   // main thread only
    MidiDestinationHandle                   nextHandle = 1;

    std::atomic<std::uint64_t> announced{0};   // CoreMIDI notifications, counted: changes()
    std::thread                notifyThread;
    std::atomic<CFRunLoopRef>  notifyLoop{nullptr};
    std::atomic<bool>          quit{false};
};

} // namespace uw
