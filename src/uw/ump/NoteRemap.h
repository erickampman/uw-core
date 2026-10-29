#pragma once

// NoteRemap: moves each note by a pitch shift chosen per input key, and keeps every note it moved
// latched so its Note Off and per-note messages follow it. The engine of Retune (one shift for
// every key) and EDOSpread12 (a shift per key), with no CLAP in it: ClapNoteRemap
// (uw/clap/ClapNoteRemap.h) is its partner for CLAP note events.
//
// One UMP message in, zero to two out:
// - A MIDI 2.0 Note On (MT 0x4) moves by its key's shift in 1/512 semitone and leaves with a Pitch
//   7.9 attribute (transposeNote79): its note number is the integer part, so a receiver that
//   ignores the attribute plays the nearest key below. A Pitch 7.9 it arrived with keeps its offset
//   from its key.
// - A MIDI 1.0 Note On (MT 0x2) moves by its key's shift rounded to the nearest semitone (halves
//   away from zero). With promotion on (setPromoteMidi1), a MIDI 1.0 note whose shift has a
//   fraction leaves as MIDI 2.0 instead, with the exact pitch, and so do its Note Off and poly
//   pressure.
// - Note Off, poly pressure and every MIDI 2.0 per-note message follow the Note On's latched key,
//   and a Pitch 7.25 value moves by the Note On's full shift, fraction included. So a shift change
//   while notes are held can't leave a note stuck: only new notes take it.
// - A Note On whose result leaves 0..127 is dropped, with everything that follows it.
// - A Note On for a key that's already sounding, whose result lands elsewhere (or is dropped),
//   first ends the old note.
// - Two input keys landing on one output key (a shift per key can do that): the second Note On
//   first ends the note the first one is sounding there, and the first key's Note Off and per-note
//   messages are then dropped, since its note is gone. The second key's Note Off ends the sound.
// - All Sound Off (CC 120) and All Notes Off (CC 123) clear that channel's latches; everything else
//   passes through unchanged.

#include <cstddef>
#include <cstdint>

#include "uw/ump/MidiMessage.h"
#include "uw/ump/Ump.h"

namespace uw::ump {

class NoteRemap {
public:
    // Most messages one process() call can write: a retriggered note whose output key moved
    // writes a Note Off for the old key, and one for another key's note on the new key, before
    // the new Note On.
    static constexpr std::size_t kMaxOut = 3;

    NoteRemap() noexcept { reset(); }

    // The shift for each input key, in 1/512 semitone. Takes effect for notes that start after
    // the call; sounding notes keep their latch.
    void setShifts(const int (&shift79)[128]) noexcept;
    // The same shift for every key.
    void setShift(int shift79) noexcept;

    // Off by default (Retune). On: a MIDI 1.0 note whose shift has a fraction leaves as MIDI 2.0.
    void setPromoteMidi1(bool on) noexcept { promoteMidi1_ = on; }

    // Writes at most kMaxOut messages to `out`; returns the count.
    std::size_t process(const MidiMessage& in, MidiMessage* out) noexcept;

    // Forget every sounding note (host reset).
    void reset() noexcept;

private:
    enum class Latch : std::uint8_t { None, Sounding, Dropped };
    struct Entry {
        Latch        state    = Latch::None;
        bool         promoted = false;   // a MIDI 1.0 note sent as MIDI 2.0
        bool         midi2    = false;   // sent as MIDI 2.0 (promoted or not)
        std::uint8_t outKey   = 0;
        std::int16_t shift79  = 0;       // the pitch shift the Note On got, for a Pitch 7.25 value
    };

    // How a Note On for this message's key would leave now.
    struct Plan {
        bool promoted = false;
        int  keyShift = 0;   // whole semitones to the output key
        int  shift79  = 0;   // the full pitch shift
    };

    Entry& entry(const MidiMessage& m) noexcept;
    Plan   plan(const MidiMessage& m) const noexcept;
    bool   moveToKey(MidiMessage& m, int keyShift, int shift79) const noexcept;

    int         shift79_[128] = {};
    bool        promoteMidi1_ = false;
    Entry       latch_[16][16][128];   // [group][channel][input key]
    std::int8_t owner_[16][16][128];   // [group][channel][output key]: the input key sounding there, or -1
};

} // namespace uw::ump
