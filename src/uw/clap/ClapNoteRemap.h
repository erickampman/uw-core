#pragma once

// ClapNoteRemap: NoteRemap's rules (uw/ump/NoteRemap.h) for the CLAP note dialect: CLAP_EVENT_NOTE_*
// and note expressions, which UMP can't carry. Hosts such as Bitwig send plugins CLAP notes and no
// MIDI 2.0, so this is how a fractional shift survives there. Used by Retune and EDOSpread12.
//
// A Note On moves to the nearest whole key (its key's shift rounded, the same key as NoteRemap's
// MIDI 1.0 path) and, when the shift has a fraction, is followed at the same time by a
// CLAP_NOTE_EXPRESSION_TUNING with that fraction (same note_id, port, channel and new key). A
// synth that ignores tuning expressions still plays the rounded note.
//
// Each sounding note is latched by channel + key, with its note_id: Note Off, choke and every note
// expression follow the latched key, found by note_id when the event has one. A host's own tuning
// expression on a moved note gets the note's fraction added. Events with no latched note follow
// the current shift. A Note On whose key leaves 0..127 is dropped, with everything that follows it.
// Two input keys landing on one output key: the second Note On first ends the first one's note,
// whose Note Off and expressions are then dropped (as NoteRemap).
// A wildcard event (key and note_id both -1) passes through, since it matches every output key
// anyway.
//
// Needs the CLAP headers: the uw::clap-notes target, apart from uw::ump.

#include <clap/clap.h>

#include <cstdint>

namespace uw {

class ClapNoteRemap {
public:
    ClapNoteRemap() noexcept { reset(); }

    // The shift for each input key, in 1/512 semitone. Takes effect for notes that start after the
    // call; sounding notes keep their latch.
    void setShifts(const int (&shift79)[128]) noexcept;
    // The same shift for every key.
    void setShift(int shift79) noexcept;

    // One CLAP note event (NOTE_ON, NOTE_OFF, NOTE_CHOKE or NOTE_EXPRESSION) in; the results are
    // pushed to `out`. Anything else is pushed unchanged.
    void process(const clap_event_header_t* in, const clap_output_events_t* out) noexcept;

    // Forget every sounding note (host reset).
    void reset() noexcept;

private:
    enum class Latch : std::uint8_t { None, Sounding, Dropped };
    struct Entry {
        Latch        state      = Latch::None;
        std::uint8_t outKey     = 0;
        std::int16_t fraction79 = 0;    // the tuning the Note On got, in 1/512 semitone
        std::int32_t noteId     = -1;
    };

    void noteOn(const clap_event_note_t& ev, const clap_output_events_t* out) noexcept;
    void noteEnd(const clap_event_note_t& ev, const clap_output_events_t* out) noexcept;
    void expression(const clap_event_note_expression_t& ev, const clap_output_events_t* out) noexcept;

    // Calls f(channel, entry) for each latched note the event's address names; returns how many.
    template <typename F>
    int forEachMatch(int channel, int key, std::int32_t noteId, F&& f) noexcept;

    int         keyShift_[128]     = {};   // each key's shift, rounded to whole semitones
    int         fraction79_[128]   = {};   // what's left of it, in 1/512 semitone
    int         uniformFraction79_ = 0;    // the fraction every key shares, or 0, for a wildcard expression
    Entry       latch_[16][128];   // [channel][input key]
    std::int8_t owner_[16][128];   // [channel][output key]: the input key last sounding there, or -1
};

} // namespace uw
