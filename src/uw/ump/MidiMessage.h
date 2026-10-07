#pragma once

#include <cstdint>

#include "uw/Types.h"

namespace uw {

// The two MIDI protocols. UMP is a *format* that carries either; a MidiMessage
// keeps whichever protocol it arrived in (StageHand's docs/domain-model.md, "MIDI protocol
// handling & per-plugin routing").
enum class MidiProtocol { Midi1, Midi2 };

// Opaque handle to an opened MIDI source. kInvalidMidiSource (0) means none.
using MidiSourceHandle = std::uint32_t;
inline constexpr MidiSourceHandle kInvalidMidiSource = 0;

// A single Universal MIDI Packet (UMP): one to four 32-bit words, `wordCount`
// of them used. The canonical MIDI unit for BOTH protocols. MIDI 1.0 travels as
// MT 0x2 (channel voice), 0x1 (system) or 0x3 (SysEx7), a lossless wrap of the
// original bytes. MIDI 2.0 travels as MT 0x4 (channel voice) or 0x5. Input never
// converts between protocols; translation happens only at a plugin boundary,
// when that plugin can't accept the message's own protocol. Helpers are in
// Core/Ump.h.
struct MidiMessage {
    std::uint32_t words[4]  = {0, 0, 0, 0};
    std::uint8_t  wordCount = 0;   // 1..4
};

// A MidiMessage tagged with the monotonic time it was captured, as delivered
// from the MIDI thread to the engine. The engine maps `captureTime` to a sample
// offset within the current block (StageHand's docs/domain-model.md, sample-accurate
// alignment).
struct TimedMidiMessage {
    Nanoseconds captureTime = 0;
    MidiMessage message;
};

} // namespace uw
