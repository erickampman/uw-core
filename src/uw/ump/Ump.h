#pragma once

#include <cstddef>
#include <cstdint>

#include "uw/ump/MidiMessage.h"

// UMP helpers shared by the MIDI seam and the engine's router/translator:
//   - field accessors (inline, header-only)
//   - value scaling, velocity16, transposeNote, toMidi1Bytes
//   - MIDI 1.0 <-> 2.0 protocol translation (MIDI 2.0 spec translation rules;
//     expected values cross-checked against the ALSA kernel's output in M2)
//   - the MIDI 1.0 byte-stream parser for non-UMP sources
// (Core/Ump.cpp; tests in tests/UmpTests.cpp)
// See StageHand's docs/domain-model.md, "MIDI protocol handling & per-plugin routing".
namespace uw::ump {

// ---------------------------------------------------------------------------
// Message classification
// ---------------------------------------------------------------------------

// UMP message type: the top nibble of word 0.
enum class MessageType : std::uint8_t {
    Utility           = 0x0,
    System            = 0x1,  // clock, start/stop, song position (both protocols)
    Midi1ChannelVoice = 0x2,  // MIDI 1.0 protocol
    Data64            = 0x3,  // SysEx7 (both protocols)
    Midi2ChannelVoice = 0x4,  // MIDI 2.0 protocol
    Data128           = 0x5,  // SysEx8 / mixed data set (MIDI 2.0)
    FlexData          = 0xD,
    Stream            = 0xF,
};

// Channel-voice status nibble. 0x8-0xE are shared by MT 0x2 and MT 0x4; the
// rest exist only in MIDI 2.0 (MT 0x4).
enum class Status : std::uint8_t {
    RegisteredPerNoteController  = 0x0,  // MIDI 2.0 only
    AssignablePerNoteController  = 0x1,  // MIDI 2.0 only
    RegisteredController         = 0x2,  // MIDI 2.0 only (RPN as one message)
    AssignableController         = 0x3,  // MIDI 2.0 only (NRPN as one message)
    RelativeRegisteredController = 0x4,  // MIDI 2.0 only
    RelativeAssignableController = 0x5,  // MIDI 2.0 only
    PerNotePitchBend             = 0x6,  // MIDI 2.0 only
    NoteOff                      = 0x8,
    NoteOn                       = 0x9,
    PolyPressure                 = 0xA,
    ControlChange                = 0xB,
    ProgramChange                = 0xC,
    ChannelPressure              = 0xD,
    PitchBend                    = 0xE,
    PerNoteManagement            = 0xF,  // MIDI 2.0 only
};

// MIDI 2.0 note-on/off attribute type (word 0, low byte, of an MT 0x4 note).
enum class NoteAttribute : std::uint8_t {
    None                 = 0x0,
    ManufacturerSpecific = 0x1,
    ProfileSpecific      = 0x2,
    Pitch7_9             = 0x3,   // absolute pitch: 7-bit note + 9-bit fraction
};

// Words in a packet of message type `mt` (0x0-0xF), fixed by the UMP spec.
constexpr std::uint8_t wordCountForType(std::uint8_t mt) noexcept {
    switch (mt & 0x0F) {
        case 0x0: case 0x1: case 0x2: case 0x6: case 0x7:           return 1;
        case 0x3: case 0x4: case 0x8: case 0x9: case 0xA:           return 2;
        case 0xB: case 0xC:                                          return 3;
        default:                                                     return 4;
    }
}

constexpr MessageType messageType(const MidiMessage& m) noexcept {
    return static_cast<MessageType>(m.words[0] >> 28);
}

constexpr std::uint8_t group(const MidiMessage& m) noexcept {
    return static_cast<std::uint8_t>((m.words[0] >> 24) & 0x0F);
}

constexpr bool isChannelVoice(const MidiMessage& m) noexcept {
    const MessageType t = messageType(m);
    return t == MessageType::Midi1ChannelVoice || t == MessageType::Midi2ChannelVoice;
}

// Protocol of a channel-voice message. Other types are protocol-neutral (see
// isProtocolNeutral) and every plugin dialect can receive them.
constexpr MidiProtocol protocolOf(const MidiMessage& m) noexcept {
    const MessageType t = messageType(m);
    return (t == MessageType::Midi2ChannelVoice || t == MessageType::Data128)
               ? MidiProtocol::Midi2
               : MidiProtocol::Midi1;
}

// System and SysEx7 messages mean the same thing under either protocol.
constexpr bool isProtocolNeutral(const MidiMessage& m) noexcept {
    const MessageType t = messageType(m);
    return t == MessageType::System || t == MessageType::Data64 ||
           t == MessageType::Utility;
}

// --- channel-voice fields (valid only when isChannelVoice(m)) --------------

constexpr Status status(const MidiMessage& m) noexcept {
    return static_cast<Status>((m.words[0] >> 20) & 0x0F);
}

constexpr std::uint8_t channel(const MidiMessage& m) noexcept {
    return static_cast<std::uint8_t>((m.words[0] >> 16) & 0x0F);
}

// Note number for note on/off and poly pressure (same bit position in MT 0x2
// and MT 0x4).
constexpr std::uint8_t noteNumber(const MidiMessage& m) noexcept {
    return static_cast<std::uint8_t>((m.words[0] >> 8) & 0x7F);
}

// A sounding note-on. In MIDI 1.0 a note-on with velocity 0 is a note-off; in
// MIDI 2.0 velocity 0 is a valid note-on.
constexpr bool isNoteOn(const MidiMessage& m) noexcept {
    if (!isChannelVoice(m) || status(m) != Status::NoteOn) return false;
    if (messageType(m) == MessageType::Midi1ChannelVoice)
        return (m.words[0] & 0x7F) != 0;
    return true;
}

constexpr bool isNoteOff(const MidiMessage& m) noexcept {
    if (!isChannelVoice(m)) return false;
    if (status(m) == Status::NoteOff) return true;
    return messageType(m) == MessageType::Midi1ChannelVoice &&
           status(m) == Status::NoteOn && (m.words[0] & 0x7F) == 0;
}

// ---------------------------------------------------------------------------
// Value scaling (MIDI 2.0 min-center-max rules)
// ---------------------------------------------------------------------------

// Scale between resolutions per the MIDI 2.0 translation rules. Upscaling then
// downscaling returns the original value for anything that started at the lower
// resolution.
std::uint32_t scaleUp(std::uint32_t value, unsigned srcBits, unsigned dstBits) noexcept;
std::uint32_t scaleDown(std::uint32_t value, unsigned srcBits, unsigned dstBits) noexcept;

// Note velocity at 16-bit resolution, for either protocol (MIDI 1.0 upscaled).
// Used by MidiFilter velocity ranges.
std::uint16_t velocity16(const MidiMessage& m) noexcept;

// ---------------------------------------------------------------------------
// Protocol translation (only at a plugin boundary)
// ---------------------------------------------------------------------------

// Most messages a single translate call can produce (a MIDI 2.0 registered
// controller downconverts to CC 101/100/6/38).
inline constexpr std::size_t kMaxTranslatedMessages = 4;

// State for MIDI 1.0 -> 2.0 upconversion: RPN/NRPN selection and bank select,
// per group/channel. Fixed size, no allocation. Owned PER SOURCE: the state
// belongs to the incoming stream, not to a destination plugin.
struct UpconvertState {
    struct Channel {
        std::uint8_t paramMsb    = 0x7F;   // 0x7F/0x7F = no RPN/NRPN selected
        std::uint8_t paramLsb    = 0x7F;
        bool         paramIsNrpn = false;
        std::uint8_t dataMsb     = 0;
        bool         dataMsbPending = false;
        std::uint8_t bankMsb     = 0;
        std::uint8_t bankLsb     = 0;
        bool         bankValid   = false;
    };
    Channel channels[16][16];   // [group][channel]
};

// MIDI 1.0 (MT 0x2) -> MIDI 2.0 (MT 0x4). Stateful: CC 101/100/99/98/6/38 and
// bank select are absorbed into `state` and emitted as single MIDI 2.0 messages,
// so a call may write 0 messages. Data Entry MSB (CC 6) emits immediately with
// LSB 0, and a following CC 38 re-emits: unlike the ALSA kernel's upconverter,
// which drops MSB-only data entry (measured in M2). Non-MT-0x2 input passes
// through unchanged.
// Writes at most `capacity` messages to `out`; returns the count written.
std::size_t upconvert(const MidiMessage& in, UpconvertState& state,
                      MidiMessage* out, std::size_t capacity) noexcept;

// MIDI 2.0 (MT 0x4) -> MIDI 1.0 (MT 0x2). Stateless; one message may expand to
// several. Data with no MIDI 1.0 equivalent (per-note controllers, per-note
// pitch bend, note attributes) is dropped, and a velocity-0 note-on becomes
// velocity 1. Non-MT-0x4 input passes through unchanged. Writes at most
// `capacity` messages to `out`; returns the count written.
std::size_t downconvert(const MidiMessage& in, MidiMessage* out,
                        std::size_t capacity) noexcept;

// ---------------------------------------------------------------------------
// Protocol-aware transforms
// ---------------------------------------------------------------------------

// Transpose a note message by `semitones`. For a MIDI 2.0 note carrying a
// Pitch7_9 attribute, the attribute moves by the same amount so microtonal pitch
// stays correct, and so does the value of a Pitch 7.25 (registered per-note
// controller 3), which is absolute too. Returns false (message unchanged) if the result would leave
// 0..127.
bool transposeNote(MidiMessage& m, int semitones) noexcept;

// Absolute pitch of a note on/off in 7.9 (512 per semitone): the Pitch7_9
// attribute of a MIDI 2.0 note carrying one, otherwise the note number << 9.
std::uint16_t notePitch79(const MidiMessage& m) noexcept;

// Transpose a MIDI 2.0 (MT 0x4) note on/off by `delta` in 1/512 semitone, for
// microtonal shifts. The result is written as a Pitch7_9 attribute and the note
// number becomes its integer part, so a receiver that ignores the attribute
// plays the nearest note below. Any other attribute (manufacturer or profile
// specific) is replaced. Returns false (message unchanged) for anything but a
// MIDI 2.0 note on/off, or if the pitch would leave 0..127 + 511/512.
bool transposeNote79(MidiMessage& m, int delta) noexcept;

// Move the value of a Pitch 7.25 (MIDI 2.0 registered per-note controller 3) by
// `delta` in 1/512 semitone, leaving its note number (the note it addresses)
// alone. The partner of transposeNote79 for a note that was shifted by a
// fraction. Returns false (message unchanged) for any other message, or if the
// pitch would leave the 7.25 range.
bool transposePitch725(MidiMessage& m, int delta) noexcept;

// ---------------------------------------------------------------------------
// MIDI 1.0 bytes <-> UMP (for non-UMP sources, and for MIDI-dialect plugins)
// ---------------------------------------------------------------------------

// Parser state for a raw MIDI 1.0 byte stream (JACK MIDI, legacy rawmidi,
// files, network). One per source. Handles running status, real-time bytes
// interleaved anywhere, and SysEx chunking into MT 0x3 packets.
struct Midi1ParserState {
    std::uint8_t group          = 0;    // UMP group stamped on output
    std::uint8_t runningStatus  = 0;
    std::uint8_t data[2]        = {0, 0};
    std::uint8_t dataCount      = 0;
    bool         inSysex        = false;
    bool         sysexStarted   = false; // a Start packet has been emitted
    std::uint8_t sysexBytes[6]  = {};
    std::uint8_t sysexCount     = 0;
};

// Feed one byte. Returns true when a complete packet has been written to `out`.
// Channel voice → MT 0x2, system → MT 0x1, SysEx → MT 0x3: a lossless wrap that
// never upconverts to MIDI 2.0 (a note-on with velocity 0 stays one). One packet
// per byte: in the odd case of a Tune Request (F6) ending an unterminated
// SysEx, the SysEx end is emitted and the Tune Request is lost.
bool parseMidi1Byte(std::uint8_t byte, Midi1ParserState& state,
                    MidiMessage& out) noexcept;

// MT 0x2 channel voice or MT 0x1 system → the original 1-3 MIDI 1.0 bytes (e.g.
// for CLAP's clap_event_midi). Returns the byte count, or 0 if not representable.
std::size_t toMidi1Bytes(const MidiMessage& m, std::uint8_t out[3]) noexcept;

// The reverse, for one complete message with its status byte (no running
// status), such as CLAP's clap_event_midi: channel voice → MT 0x2, system
// common and real-time → MT 0x1, in `group`. Bytes past the message's length
// are ignored. Returns false for SysEx, a stray data byte, or too few bytes.
bool fromMidi1Bytes(const std::uint8_t* bytes, std::size_t size, std::uint8_t group,
                    MidiMessage& out) noexcept;

} // namespace uw::ump
