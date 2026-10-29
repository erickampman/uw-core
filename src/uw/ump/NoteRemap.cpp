#include "uw/ump/NoteRemap.h"

namespace uw::ump {

namespace {

// num / den rounded to nearest, halves away from zero (den > 0).
int roundDiv(int num, int den) noexcept {
    return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den);
}

// num / 512 rounded down.
int floor512(int num) noexcept {
    return num >= 0 ? num / 512 : -((-num + 511) / 512);
}

// Messages whose note number names a sounding note, apart from Note On/Off.
bool isPerNote(const MidiMessage& m) noexcept {
    const Status s = status(m);
    if (s == Status::PolyPressure) return true;
    return messageType(m) == MessageType::Midi2ChannelVoice &&
           (s == Status::RegisteredPerNoteController || s == Status::AssignablePerNoteController ||
            s == Status::PerNotePitchBend || s == Status::PerNoteManagement);
}

// A Pitch 7.25: registered per-note controller 3, an absolute pitch.
bool isPitch725(const MidiMessage& m) noexcept {
    return messageType(m) == MessageType::Midi2ChannelVoice &&
           status(m) == Status::RegisteredPerNoteController && (m.words[0] & 0xFF) == 3;
}

// All Sound Off (CC 120) or All Notes Off (CC 123); the controller index sits in the same bits in
// MT 0x2 and MT 0x4.
bool isAllNotesOff(const MidiMessage& m) noexcept {
    if (status(m) != Status::ControlChange) return false;
    const auto index = (m.words[0] >> 8) & 0x7F;
    return index == 120 || index == 123;
}

// A promoted note's MIDI 1.0 Note Off or poly pressure, as MIDI 2.0 on `outKey`. Returns false
// for anything else, or a key outside 0..127.
bool promote(const MidiMessage& in, int outKey, MidiMessage& out) noexcept {
    if (outKey < 0 || outKey > 127) return false;
    const auto key   = static_cast<std::uint8_t>(outKey);
    const auto value = in.words[0] & 0x7F;
    if (isNoteOff(in)) {
        out = midi2NoteOff(group(in), channel(in), key, static_cast<std::uint16_t>(scaleUp(value, 7, 16)));
        return true;
    }
    if (status(in) == Status::PolyPressure) {
        out = MidiMessage{};
        out.words[0] = 0x40000000u | std::uint32_t(group(in)) << 24 | 0xAu << 20 |
                       std::uint32_t(channel(in)) << 16 | std::uint32_t(key) << 8;
        out.words[1] = scaleUp(value, 7, 32);
        out.wordCount = 2;
        return true;
    }
    return false;
}

} // namespace

void NoteRemap::setShifts(const int (&shift79)[128]) noexcept {
    for (int k = 0; k < 128; ++k) shift79_[k] = shift79[k];
}

void NoteRemap::setShift(int shift79) noexcept {
    for (int& s : shift79_) s = shift79;
}

void NoteRemap::reset() noexcept {
    for (auto& g : latch_)
        for (auto& c : g)
            for (auto& e : c) e = Entry{};
}

NoteRemap::Entry& NoteRemap::entry(const MidiMessage& m) noexcept {
    return latch_[group(m)][channel(m)][noteNumber(m)];
}

NoteRemap::Plan NoteRemap::plan(const MidiMessage& m) const noexcept {
    const int s = shift79_[noteNumber(m)];
    if (messageType(m) == MessageType::Midi2ChannelVoice)
        return {false, floor512(s), s};   // the key is the integer part of the shifted pitch
    if (promoteMidi1_ && s % 512 != 0) return {true, floor512(s), s};
    const int keyShift = roundDiv(s, 512);
    return {false, keyShift, keyShift * 512};
}

bool NoteRemap::moveToKey(MidiMessage& m, int keyShift, int shift79) const noexcept {
    if (!transposeNote(m, keyShift)) return false;
    // transposeNote moved a Pitch 7.25 value by whole semitones with its key; add the fraction so
    // the value moves by the note's full shift.
    if (isPitch725(m)) return transposePitch725(m, shift79 - keyShift * 512);
    return true;
}

std::size_t NoteRemap::process(const MidiMessage& in, MidiMessage* out) noexcept {
    if (!isChannelVoice(in)) {
        out[0] = in;
        return 1;
    }

    if (isNoteOn(in)) {
        Entry&      e = entry(in);
        const Plan  p = plan(in);
        std::size_t n = 0;
        MidiMessage m = in;
        bool inRange;
        if (p.promoted) {
            const int pitch = (noteNumber(in) << 9) + p.shift79;
            inRange = pitch >= 0 && pitch <= (127 << 9) + 511;
            if (inRange)
                m = midi2NoteOn(group(in), channel(in), static_cast<std::uint8_t>(pitch >> 9),
                                static_cast<std::uint16_t>(scaleUp(in.words[0] & 0x7F, 7, 16)),
                                static_cast<std::uint16_t>(pitch));
        } else if (messageType(in) == MessageType::Midi2ChannelVoice) {
            inRange = transposeNote79(m, p.shift79);
        } else {
            inRange = transposeNote(m, p.keyShift);
        }
        if (e.state == Latch::Sounding &&
            (!inRange || e.outKey != noteNumber(m) || e.promoted != p.promoted)) {
            // Retriggered while sounding, and the new note won't land on the old one: end the old
            // one first so it can't hang.
            MidiMessage off;
            if (e.promoted) {
                off = midi2NoteOff(group(in), channel(in), e.outKey);
            } else {
                off = in;
                off.words[0] = (off.words[0] & 0xFF0F00FFu) |
                               (static_cast<std::uint32_t>(Status::NoteOff) << 20) |
                               (static_cast<std::uint32_t>(e.outKey) << 8);
                off.words[0] &= ~0xFFu;   // MIDI 1.0: velocity 0; MIDI 2.0: no attribute
                off.words[1] = 0;
            }
            out[n++] = off;
        }
        if (!inRange) {
            e = {Latch::Dropped, false, 0, 0};
            return n;
        }
        e = {Latch::Sounding, p.promoted, noteNumber(m), static_cast<std::int16_t>(p.shift79)};
        out[n++] = m;
        return n;
    }

    if (isNoteOff(in) || isPerNote(in)) {
        Entry&      e = entry(in);
        MidiMessage m = in;
        bool keep = true;
        switch (e.state) {
            case Latch::Sounding:
                keep = e.promoted && messageType(in) == MessageType::Midi1ChannelVoice
                           ? promote(in, e.outKey, m)
                           : moveToKey(m, e.outKey - noteNumber(in), e.shift79);
                break;
            case Latch::Dropped:
                keep = false;
                break;
            case Latch::None: {
                // No Note On seen (it came before a reset, or before this started): guess the key
                // the current shift would have given it.
                const Plan p = plan(in);
                keep = p.promoted ? promote(in, noteNumber(in) + p.keyShift, m)
                                  : moveToKey(m, p.keyShift, p.shift79);
                break;
            }
        }
        if (isNoteOff(in)) e = Entry{};
        if (!keep) return 0;
        out[0] = m;
        return 1;
    }

    if (isAllNotesOff(in))
        for (auto& e : latch_[group(in)][channel(in)]) e = Entry{};
    out[0] = in;
    return 1;
}

} // namespace uw::ump
