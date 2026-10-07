#pragma once

#include <cstdint>

#include "uw/ump/MidiMessage.h"

namespace uw {

// Identifies a parameter within a plugin. `cookie` is the plugin's opaque
// fast-lookup handle (CLAP cookie); may be null.
struct ParamRef {
    std::uint32_t id     = 0;
    void*         cookie = nullptr;
};

// The kinds of event that share the per-block, time-sorted stream. Mirrors
// CLAP's single interleaved input event list; the VST3 seam demultiplexes these
// into IEventList + IParameterChanges (StageHand's docs/architecture.md 3a).
enum class EventType {
    Midi,               // a UMP MidiMessage
    ParamValue,         // set a parameter to `value`
    ParamMod,           // add modulation `value` to a parameter
    ParamGestureBegin,  // user began touching a parameter (automation recording)
    ParamGestureEnd,    // user released a parameter
};

// One event with a sample offset into the current block. A tagged record rather
// than a variant, to stay trivially copyable and RT-friendly: `midi` and
// `portIndex` are valid when type == Midi, the parameter fields for the Param*
// kinds.
//
// By the time a Midi event reaches a plugin, the engine has already routed it
// and translated it into a protocol that note port accepts. The plugin seam only
// maps by type: MT 0x2/0x1 → MIDI 1.0 bytes, MT 0x4 → MIDI 2.0.
struct Event {
    std::uint32_t sampleOffset = 0;   // in [0, frameCount)
    EventType     type         = EventType::Midi;

    MidiMessage   midi;               // valid when type == Midi
    std::uint16_t portIndex = 0;      // plugin note-input port it's addressed to

    ParamRef param;             // valid when type is a Param* kind
    double   value = 0.0;       // new value (ParamValue) / mod amount (ParamMod)
};

// A non-owning, time-sorted (ascending sampleOffset) view of the events for one
// plugin for one block. Matches CLAP's sorted input event list.
struct EventBlock {
    const Event*  events = nullptr;
    std::uint32_t count  = 0;
};

// Where a plugin pushes its OUTPUT events during process() — parameter changes
// driven by the plugin's own GUI or internal modulation, fed back to the host
// (the RT->Main path in StageHand's docs/domain-model.md). Called on the audio thread;
// implementations must be non-blocking.
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void push(const Event& event) noexcept = 0;
};

} // namespace uw
