// NoteRemap tests: a shift per key, the latch, and MIDI 1.0 promotion. Retune's own tests cover a
// single shift for every key in depth.

#include <vector>

#include "Check.h"
#include "uw/ump/NoteRemap.h"
#include "uw/ump/Ump.h"

using namespace uw;
using namespace uw::ump;

namespace {

std::vector<MidiMessage> run(NoteRemap& r, const MidiMessage& in) {
    MidiMessage out[NoteRemap::kMaxOut];
    const std::size_t n = r.process(in, out);
    return {out, out + n};
}

MidiMessage polyPressure1(std::uint8_t key, std::uint8_t value) {
    MidiMessage m;
    m.words[0]  = 0x20A00000u | std::uint32_t(key) << 8 | value;
    m.wordCount = 1;
    return m;
}

void testPerKeyShifts() {
    NoteRemap r;
    int shifts[128] = {};
    shifts[60] = 512 * 2;         // C4 up a whole tone
    shifts[62] = -512;            // D4 down a semitone
    shifts[64] = 512 * 3 / 2;     // E4 up 1.5 semitones
    r.setShifts(shifts);

    auto o = run(r, midi1NoteOn(0, 0, 60, 100));
    CHECK(o.size() == 1 && noteNumber(o[0]) == 62);
    o = run(r, midi1NoteOn(0, 0, 62, 100));
    CHECK(o.size() == 1 && noteNumber(o[0]) == 61);
    o = run(r, midi1NoteOn(0, 0, 61, 100));   // no shift
    CHECK(o.size() == 1 && noteNumber(o[0]) == 61);

    // MIDI 2.0 keeps the fraction; MIDI 1.0 rounds it (halves away from zero).
    o = run(r, midi2NoteOn(0, 1, 64, 0x8000));
    CHECK(o.size() == 1 && noteNumber(o[0]) == 65 && notePitch79(o[0]) == (65 << 9) + 256);
    o = run(r, midi1NoteOn(0, 2, 64, 100));
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi1ChannelVoice && noteNumber(o[0]) == 66);

    // A MIDI 2.0 note's own Pitch 7.9 keeps its offset from its key.
    o = run(r, midi2NoteOn(0, 3, 60, 0x8000, (60 << 9) + 100));
    CHECK(o.size() == 1 && notePitch79(o[0]) == (62 << 9) + 100);
}

void testLatchSurvivesNewShifts() {
    NoteRemap r;
    int shifts[128] = {};
    shifts[60] = 512 * 4;
    r.setShifts(shifts);
    CHECK(noteNumber(run(r, midi1NoteOn(0, 0, 60, 100))[0]) == 64);

    shifts[60] = -512 * 3;
    r.setShifts(shifts);
    auto o = run(r, polyPressure1(60, 50));
    CHECK(o.size() == 1 && noteNumber(o[0]) == 64);
    o = run(r, midi1NoteOff(0, 0, 60));
    CHECK(o.size() == 1 && isNoteOff(o[0]) && noteNumber(o[0]) == 64);
    CHECK(noteNumber(run(r, midi1NoteOn(0, 0, 60, 100))[0]) == 57);
}

void testRetriggerAfterNewShift() {
    NoteRemap r;
    r.setShift(512);
    run(r, midi1NoteOn(0, 0, 60, 100));
    r.setShift(512 * 2);
    auto o = run(r, midi1NoteOn(0, 0, 60, 100));
    CHECK(o.size() == 2 && isNoteOff(o[0]) && noteNumber(o[0]) == 61 && isNoteOn(o[1]) &&
          noteNumber(o[1]) == 62);
}

void testOutOfRangeDropped() {
    NoteRemap r;
    int shifts[128] = {};
    shifts[120] = 512 * 12;
    r.setShifts(shifts);
    CHECK(run(r, midi1NoteOn(0, 0, 120, 100)).empty());
    CHECK(run(r, polyPressure1(120, 9)).empty());
    CHECK(run(r, midi1NoteOff(0, 0, 120)).empty());
}

void testPromotion() {
    NoteRemap r;
    int shifts[128] = {};
    shifts[60] = 512 + 128;   // up 1.25 semitones
    shifts[62] = 512;         // whole: stays MIDI 1.0
    r.setShifts(shifts);

    // Off: rounded, MIDI 1.0.
    auto o = run(r, midi1NoteOn(0, 0, 60, 127));
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi1ChannelVoice && noteNumber(o[0]) == 61);
    run(r, midi1NoteOff(0, 0, 60));

    r.setPromoteMidi1(true);
    o = run(r, midi1NoteOn(0, 0, 60, 127));
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi2ChannelVoice && isNoteOn(o[0]) &&
          noteNumber(o[0]) == 61 && notePitch79(o[0]) == (61 << 9) + 128 && velocity16(o[0]) == 0xFFFF);
    o = run(r, polyPressure1(60, 127));
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi2ChannelVoice &&
          status(o[0]) == Status::PolyPressure && noteNumber(o[0]) == 61 && o[0].words[1] == 0xFFFFFFFFu);
    o = run(r, midi1NoteOff(0, 0, 60));
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi2ChannelVoice && isNoteOff(o[0]) &&
          noteNumber(o[0]) == 61);

    // A whole shift stays MIDI 1.0.
    o = run(r, midi1NoteOn(0, 0, 62, 100));
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi1ChannelVoice && noteNumber(o[0]) == 63);

    // MIDI 1.0 Note On velocity 0 is a Note Off, and follows the promoted note.
    run(r, midi1NoteOn(0, 1, 60, 64));
    MidiMessage off = midi1NoteOn(0, 1, 60, 1);
    off.words[0] &= ~0x7Fu;
    o = run(r, off);
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi2ChannelVoice && isNoteOff(o[0]) &&
          noteNumber(o[0]) == 61);

    // Promoted, then the shift becomes whole: the retrigger ends the MIDI 2.0 note in MIDI 2.0.
    run(r, midi1NoteOn(0, 2, 60, 64));
    shifts[60] = 512;
    r.setShifts(shifts);
    o = run(r, midi1NoteOn(0, 2, 60, 64));
    CHECK(o.size() == 2 && messageType(o[0]) == MessageType::Midi2ChannelVoice && isNoteOff(o[0]) &&
          noteNumber(o[0]) == 61 && messageType(o[1]) == MessageType::Midi1ChannelVoice &&
          noteNumber(o[1]) == 61);
}

void testUnlatchedFollowsCurrentShift() {
    NoteRemap r;
    int shifts[128] = {};
    shifts[60] = 512 + 128;
    r.setShifts(shifts);
    r.setPromoteMidi1(true);
    auto o = run(r, midi1NoteOff(0, 0, 60));
    CHECK(o.size() == 1 && messageType(o[0]) == MessageType::Midi2ChannelVoice && noteNumber(o[0]) == 61);
}

void testPassThrough() {
    NoteRemap r;
    r.setShift(512 * 5);
    MidiMessage cc;
    cc.words[0]  = 0x20B00740u;   // CC 7
    cc.wordCount = 1;
    auto o = run(r, cc);
    CHECK(o.size() == 1 && o[0].words[0] == cc.words[0]);
}

} // namespace

int main() {
    testPerKeyShifts();
    testLatchSurvivesNewShifts();
    testRetriggerAfterNewShift();
    testOutOfRangeDropped();
    testPromotion();
    testUnlatchedFollowsCurrentShift();
    testPassThrough();
    return uw::test::finish("note-remap");
}
