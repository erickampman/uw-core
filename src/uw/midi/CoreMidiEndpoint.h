#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

#include "uw/ump/MidiMessage.h"
#include "uw/ump/UmpStream.h"

typedef unsigned int MIDIObjectRef;

namespace uw {

// A CoreMIDI UMP endpoint of our own (MIDIUMPMutableEndpoint, macOS 15 and
// later), for CoreMidiOutput::openEndpoint. Plain C++ outside: the
// Objective-C is in CoreMidiEndpoint.mm, uw-core's only Objective-C++, as
// CoreMIDI's endpoint API exists only for Objective-C.
//
// It's made enabled, with its function blocks registered, and lasts until
// destroyed: CoreMIDI shows other apps no change to an enabled endpoint's
// blocks (reconfigured, renamed, or registered again while disabled: checked
// on macOS 26.6), so new blocks need a new endpoint. Its destination answers
// a peer's discovery on its source, from a copy of `info` made at creation,
// and counts them in `inquiries`.
class CoreMidiEndpoint {
public:
    // Null (after printing why) before macOS 15, or if CoreMIDI refuses.
    static std::unique_ptr<CoreMidiEndpoint> create(const ump::EndpointInfo& info, MidiProtocol protocol,
                                                    std::shared_ptr<std::atomic<std::uint64_t>> inquiries);
    ~CoreMidiEndpoint();

    CoreMidiEndpoint(const CoreMidiEndpoint&)            = delete;
    CoreMidiEndpoint& operator=(const CoreMidiEndpoint&) = delete;

    MIDIObjectRef source() const { return source_; }
    MIDIObjectRef destination() const { return destination_; }

private:
    CoreMidiEndpoint() = default;

    void*         object_      = nullptr;   // the MIDIUMPMutableEndpoint, retained
    MIDIObjectRef source_      = 0;
    MIDIObjectRef destination_ = 0;
};

} // namespace uw
