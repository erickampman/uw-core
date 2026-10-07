#include "uw/ump/Ump.h"

namespace uw::ump {

// MIDI 2.0 min-center-max upscaling: values at or below the source center shift
// up; values above it fill the new low bits by repeating the source's low bits,
// so the maximum maps to the maximum. Matches the ALSA kernel's output (M2).
std::uint32_t scaleUp(std::uint32_t value, unsigned srcBits, unsigned dstBits) noexcept {
    if (srcBits == 0 || dstBits <= srcBits) return scaleDown(value, srcBits, dstBits);
    const unsigned      scaleBits = dstBits - srcBits;
    std::uint32_t       shifted   = value << scaleBits;
    const std::uint32_t srcCenter = 1u << (srcBits - 1);
    if (value <= srcCenter) return shifted;

    const unsigned repeatBits  = srcBits - 1;
    std::uint32_t  repeatValue = value & ((1u << repeatBits) - 1);
    if (scaleBits > repeatBits)
        repeatValue <<= scaleBits - repeatBits;
    else
        repeatValue >>= repeatBits - scaleBits;
    while (repeatValue != 0) {
        shifted |= repeatValue;
        repeatValue >>= repeatBits;
    }
    return shifted;
}

std::uint32_t scaleDown(std::uint32_t value, unsigned srcBits, unsigned dstBits) noexcept {
    if (dstBits >= srcBits) return value;
    return value >> (srcBits - dstBits);
}

std::uint16_t velocity16(const MidiMessage& m) noexcept {
    if (!isChannelVoice(m)) return 0;
    if (status(m) != Status::NoteOn && status(m) != Status::NoteOff) return 0;
    if (messageType(m) == MessageType::Midi2ChannelVoice)
        return static_cast<std::uint16_t>(m.words[1] >> 16);
    return static_cast<std::uint16_t>(scaleUp(m.words[0] & 0x7F, 7, 16));
}

bool transposeNote(MidiMessage& m, int semitones) noexcept {
    if (semitones == 0 || !isChannelVoice(m)) return true;

    const Status s     = status(m);
    const bool   midi2 = messageType(m) == MessageType::Midi2ChannelVoice;
    const bool   carriesNote =
        s == Status::NoteOn || s == Status::NoteOff || s == Status::PolyPressure ||
        (midi2 && (s == Status::RegisteredPerNoteController ||
                   s == Status::AssignablePerNoteController || s == Status::PerNotePitchBend ||
                   s == Status::PerNoteManagement));
    if (!carriesNote) return true;

    const int note = noteNumber(m) + semitones;
    if (note < 0 || note > 127) return false;

    std::uint32_t w0 = (m.words[0] & ~0x7F00u) | (static_cast<std::uint32_t>(note) << 8);
    std::uint32_t w1 = m.words[1];
    if (midi2 && (s == Status::NoteOn || s == Status::NoteOff) &&
        static_cast<NoteAttribute>(m.words[0] & 0xFF) == NoteAttribute::Pitch7_9) {
        const int pitch = static_cast<int>(w1 & 0xFFFF) + semitones * 512;   // 7.9: 512 per semitone
        if (pitch < 0 || pitch > 0xFFFF) return false;
        w1 = (w1 & 0xFFFF0000u) | static_cast<std::uint32_t>(pitch);
    }
    m.words[0] = w0;
    m.words[1] = w1;
    return true;
}

std::size_t toMidi1Bytes(const MidiMessage& m, std::uint8_t out[3]) noexcept {
    const std::uint32_t w0         = m.words[0];
    const auto          statusByte = static_cast<std::uint8_t>((w0 >> 16) & 0xFF);
    out[0] = statusByte;
    out[1] = static_cast<std::uint8_t>((w0 >> 8) & 0x7F);
    out[2] = static_cast<std::uint8_t>(w0 & 0x7F);

    switch (messageType(m)) {
        case MessageType::Midi1ChannelVoice:
            return (status(m) == Status::ProgramChange || status(m) == Status::ChannelPressure) ? 2 : 3;
        case MessageType::System:
            switch (statusByte) {
                case 0xF1:
                case 0xF3: return 2;
                case 0xF2: return 3;
                default:   return statusByte >= 0xF6 ? 1 : 0;
            }
        default:
            return 0;
    }
}

// --- translation ------------------------------------------------------------------

namespace {

MidiMessage midi2Message(std::uint8_t group, Status s, std::uint8_t channel, std::uint8_t byte2,
                         std::uint8_t byte1, std::uint32_t data) noexcept {
    MidiMessage m;
    m.words[0] = (0x4u << 28) | (std::uint32_t{group} << 24) | (static_cast<std::uint32_t>(s) << 20) |
                 (std::uint32_t{channel} << 16) | (std::uint32_t{byte2} << 8) | byte1;
    m.words[1]  = data;
    m.wordCount = 2;
    return m;
}

MidiMessage midi1Message(std::uint8_t group, std::uint8_t statusNibble, std::uint8_t channel,
                         std::uint8_t d1, std::uint8_t d2) noexcept {
    MidiMessage m;
    m.words[0] = (0x2u << 28) | (std::uint32_t{group} << 24) | (std::uint32_t{statusNibble} << 20) |
                 (std::uint32_t{channel} << 16) | (std::uint32_t{d1} << 8) | d2;
    m.wordCount = 1;
    return m;
}

MidiMessage systemMessage(std::uint8_t group, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) noexcept {
    MidiMessage m;
    m.words[0]  = (0x1u << 28) | (std::uint32_t{group} << 24) | (std::uint32_t{status} << 16) |
                  (std::uint32_t{d1} << 8) | d2;
    m.wordCount = 1;
    return m;
}

// kind: 0 complete, 1 start, 2 continue, 3 end.
MidiMessage sysex7Packet(std::uint8_t group, std::uint8_t kind, const std::uint8_t* bytes,
                         std::uint8_t count) noexcept {
    std::uint8_t b[6] = {};
    for (std::uint8_t i = 0; i < count && i < 6; ++i) b[i] = bytes[i];
    MidiMessage m;
    m.words[0] = (0x3u << 28) | (std::uint32_t{group} << 24) | (std::uint32_t{kind} << 20) |
                 (std::uint32_t{count} << 16) | (std::uint32_t{b[0]} << 8) | b[1];
    m.words[1] = (std::uint32_t{b[2]} << 24) | (std::uint32_t{b[3]} << 16) | (std::uint32_t{b[4]} << 8) | b[5];
    m.wordCount = 2;
    return m;
}

// MIDI 1.0 control change -> MIDI 2.0. Bank select and RPN/NRPN selection are
// absorbed into state; Data Entry becomes one Registered/Assignable Controller
// message, sent at the MSB (with LSB 0) and again if an LSB follows.
std::size_t upconvertControl(std::uint8_t group, std::uint8_t channel, std::uint8_t cc,
                             std::uint8_t value, UpconvertState::Channel& c, MidiMessage* out) noexcept {
    switch (cc) {
        case 0:   c.bankMsb = value; c.bankValid = true; return 0;
        case 32:  c.bankLsb = value; c.bankValid = true; return 0;
        case 101: c.paramMsb = value; c.paramIsNrpn = false; c.dataMsbPending = false; return 0;
        case 100: c.paramLsb = value; c.paramIsNrpn = false; c.dataMsbPending = false; return 0;
        case 99:  c.paramMsb = value; c.paramIsNrpn = true;  c.dataMsbPending = false; return 0;
        case 98:  c.paramLsb = value; c.paramIsNrpn = true;  c.dataMsbPending = false; return 0;
        case 6:
        case 38: {
            const bool selected = !(c.paramMsb == 0x7F && c.paramLsb == 0x7F);   // 127/127 = null
            if (!selected) break;   // no parameter selected: an ordinary controller
            if (cc == 6) {
                c.dataMsb        = value;
                c.dataMsbPending = true;
            }
            const std::uint32_t data14 = (std::uint32_t{c.dataMsb} << 7) | (cc == 38 ? value : 0u);
            out[0] = midi2Message(group, c.paramIsNrpn ? Status::AssignableController : Status::RegisteredController,
                                  channel, c.paramMsb, c.paramLsb, scaleUp(data14, 14, 32));
            return 1;
        }
        default:
            break;
    }
    out[0] = midi2Message(group, Status::ControlChange, channel, cc, 0, scaleUp(value, 7, 32));
    return 1;
}

std::uint8_t dataBytesFor(std::uint8_t status) noexcept {
    switch (status & 0xF0) {
        case 0xC0:
        case 0xD0: return 1;
        case 0xF0: break;
        default:   return 2;
    }
    switch (status) {
        case 0xF1:
        case 0xF3: return 1;
        case 0xF2: return 2;
        default:   return 0;
    }
}

} // namespace

std::size_t upconvert(const MidiMessage& in, UpconvertState& state, MidiMessage* out,
                      std::size_t capacity) noexcept {
    if (capacity == 0) return 0;
    if (messageType(in) != MessageType::Midi1ChannelVoice) {
        out[0] = in;
        return 1;
    }
    const std::uint8_t      g  = group(in);
    const std::uint8_t      ch = channel(in);
    const auto              d1 = static_cast<std::uint8_t>((in.words[0] >> 8) & 0x7F);
    const auto              d2 = static_cast<std::uint8_t>(in.words[0] & 0x7F);
    UpconvertState::Channel& c = state.channels[g][ch];

    switch (status(in)) {
        case Status::NoteOff:
            out[0] = midi2Message(g, Status::NoteOff, ch, d1, 0, scaleUp(d2, 7, 16) << 16);
            return 1;
        case Status::NoteOn:
            if (d2 == 0) {   // a note-off in MIDI 1.0; velocity 0, as the ALSA kernel does
                out[0] = midi2Message(g, Status::NoteOff, ch, d1, 0, 0);
                return 1;
            }
            out[0] = midi2Message(g, Status::NoteOn, ch, d1, 0, scaleUp(d2, 7, 16) << 16);
            return 1;
        case Status::PolyPressure:
            out[0] = midi2Message(g, Status::PolyPressure, ch, d1, 0, scaleUp(d2, 7, 32));
            return 1;
        case Status::ControlChange:
            return upconvertControl(g, ch, d1, d2, c, out);
        case Status::ProgramChange: {
            const std::uint32_t bank = c.bankValid ? ((std::uint32_t{c.bankMsb} << 8) | c.bankLsb) : 0u;
            out[0] = midi2Message(g, Status::ProgramChange, ch, 0, c.bankValid ? 1 : 0,
                                  (std::uint32_t{d1} << 24) | bank);
            return 1;
        }
        case Status::ChannelPressure:
            out[0] = midi2Message(g, Status::ChannelPressure, ch, 0, 0, scaleUp(d1, 7, 32));
            return 1;
        case Status::PitchBend:
            out[0] = midi2Message(g, Status::PitchBend, ch, 0, 0,
                                  scaleUp((std::uint32_t{d2} << 7) | d1, 14, 32));
            return 1;
        default:
            out[0] = in;
            return 1;
    }
}

std::size_t downconvert(const MidiMessage& in, MidiMessage* out, std::size_t capacity) noexcept {
    if (capacity == 0) return 0;
    if (messageType(in) != MessageType::Midi2ChannelVoice) {
        out[0] = in;
        return 1;
    }
    const std::uint8_t  g     = group(in);
    const std::uint8_t  ch    = channel(in);
    const std::uint32_t w0    = in.words[0];
    const std::uint32_t w1    = in.words[1];
    const auto          byte2 = static_cast<std::uint8_t>((w0 >> 8) & 0x7F);
    const auto          byte1 = static_cast<std::uint8_t>(w0 & 0x7F);

    std::size_t n    = 0;
    auto        push = [&](std::uint8_t statusNibble, std::uint32_t d1, std::uint32_t d2) {
        if (n < capacity)
            out[n++] = midi1Message(g, statusNibble, ch, static_cast<std::uint8_t>(d1 & 0x7F),
                                    static_cast<std::uint8_t>(d2 & 0x7F));
    };

    switch (status(in)) {
        case Status::NoteOff:
            push(0x8, byte2, scaleDown(w1 >> 16, 16, 7));
            break;
        case Status::NoteOn: {
            const std::uint32_t velocity = scaleDown(w1 >> 16, 16, 7);
            push(0x9, byte2, velocity == 0 ? 1u : velocity);   // 0 would mean note-off in MIDI 1.0
            break;
        }
        case Status::PolyPressure:
            push(0xA, byte2, scaleDown(w1, 32, 7));
            break;
        case Status::ControlChange:
            push(0xB, byte2, scaleDown(w1, 32, 7));
            break;
        case Status::RegisteredController:
        case Status::AssignableController: {
            const bool          nrpn   = status(in) == Status::AssignableController;
            const std::uint32_t data14 = scaleDown(w1, 32, 14);
            push(0xB, nrpn ? 99 : 101, byte2);
            push(0xB, nrpn ? 98 : 100, byte1);
            push(0xB, 6, data14 >> 7);
            push(0xB, 38, data14 & 0x7F);
            break;
        }
        case Status::ProgramChange:
            if (w0 & 1u) {   // bank valid
                push(0xB, 0, (w1 >> 8) & 0x7F);
                push(0xB, 32, w1 & 0x7F);
            }
            push(0xC, (w1 >> 24) & 0x7F, 0);
            break;
        case Status::ChannelPressure:
            push(0xD, scaleDown(w1, 32, 7), 0);
            break;
        case Status::PitchBend: {
            const std::uint32_t data14 = scaleDown(w1, 32, 14);
            push(0xE, data14 & 0x7F, data14 >> 7);
            break;
        }
        default:   // per-note controllers / pitch bend / management, relative controllers
            break;
    }
    return n;
}

bool parseMidi1Byte(std::uint8_t byte, Midi1ParserState& s, MidiMessage& out) noexcept {
    const auto g = static_cast<std::uint8_t>(s.group & 0x0F);

    if (byte >= 0xF8) {   // real-time: allowed anywhere, disturbs nothing
        out = systemMessage(g, byte, 0, 0);
        return true;
    }
    if (byte == 0xF0) {
        s.inSysex       = true;
        s.sysexStarted  = false;
        s.sysexCount    = 0;
        s.runningStatus = 0;
        return false;
    }
    if (byte == 0xF7) {
        if (!s.inSysex) return false;
        s.inSysex = false;
        out       = sysex7Packet(g, s.sysexStarted ? 3 : 0, s.sysexBytes, s.sysexCount);
        return true;
    }
    if (byte & 0x80) {   // any other status byte
        bool emitted = false;
        if (s.inSysex) {   // ends an unterminated SysEx
            s.inSysex = false;
            out       = sysex7Packet(g, s.sysexStarted ? 3 : 0, s.sysexBytes, s.sysexCount);
            emitted   = true;
        }
        s.dataCount = 0;
        if (byte == 0xF6) {   // tune request: complete by itself
            s.runningStatus = 0;
            if (emitted) return true;   // one packet per byte: a tune request ending a SysEx is lost
            out = systemMessage(g, 0xF6, 0, 0);
            return true;
        }
        s.runningStatus = (byte == 0xF4 || byte == 0xF5) ? 0 : byte;   // undefined: ignore
        return emitted;
    }

    // Data byte.
    if (s.inSysex) {
        if (s.sysexCount == 6) {
            out            = sysex7Packet(g, s.sysexStarted ? 2 : 1, s.sysexBytes, 6);
            s.sysexStarted = true;
            s.sysexBytes[0] = byte;
            s.sysexCount   = 1;
            return true;
        }
        s.sysexBytes[s.sysexCount++] = byte;
        return false;
    }
    if (s.runningStatus == 0) return false;   // stray data byte
    s.data[s.dataCount++] = byte;
    const std::uint8_t needed = dataBytesFor(s.runningStatus);
    if (s.dataCount < needed) return false;
    s.dataCount = 0;

    const std::uint8_t d2 = needed > 1 ? s.data[1] : 0;
    if (s.runningStatus >= 0xF0) {   // system common: complete, and cancels running status
        out             = systemMessage(g, s.runningStatus, s.data[0], d2);
        s.runningStatus = 0;
        return true;
    }
    out = midi1Message(g, static_cast<std::uint8_t>(s.runningStatus >> 4),
                       static_cast<std::uint8_t>(s.runningStatus & 0x0F), s.data[0], d2);
    return true;
}

} // namespace uw::ump
