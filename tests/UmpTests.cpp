// UMP helper tests. The translators and byte parser join these when
// Core/Ump.cpp lands (M5).

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "Check.h"
#include "uw/ump/Ump.h"

using namespace uw;

namespace {

MidiMessage packet(std::uint32_t w0, std::uint32_t w1 = 0) {
    MidiMessage m;
    m.words[0]  = w0;
    m.words[1]  = w1;
    m.wordCount = ump::wordCountForType(static_cast<std::uint8_t>(w0 >> 28));
    return m;
}

void testFieldAccess() {
    // MIDI 1.0 CV in UMP: group 3, note-on, channel 5, note 60, velocity 100.
    const MidiMessage m1 = packet(0x23953C64);
    CHECK(ump::messageType(m1) == ump::MessageType::Midi1ChannelVoice);
    CHECK(ump::group(m1) == 3);
    CHECK(ump::status(m1) == ump::Status::NoteOn);
    CHECK(ump::channel(m1) == 5);
    CHECK(ump::noteNumber(m1) == 60);
    CHECK(m1.wordCount == 1);

    // MIDI 2.0 CV: group 0, note-on, channel 0, note 61, pitch 7.9 attribute.
    const MidiMessage m2 = packet(0x40903D03, 0x8000'7A00);
    CHECK(ump::messageType(m2) == ump::MessageType::Midi2ChannelVoice);
    CHECK(ump::noteNumber(m2) == 61);
    CHECK(static_cast<ump::NoteAttribute>(m2.words[0] & 0xFF) == ump::NoteAttribute::Pitch7_9);
    CHECK(m2.wordCount == 2);
}

void testNoteOnOffSemantics() {
    CHECK(ump::isNoteOn(packet(0x20903C64)));                      // 1.0 note-on
    CHECK(!ump::isNoteOn(packet(0x20903C00)));                     // 1.0 vel 0 ...
    CHECK(ump::isNoteOff(packet(0x20903C00)));                     // ... is a note-off
    CHECK(ump::isNoteOff(packet(0x20803C40)));                     // 1.0 note-off
    CHECK(ump::isNoteOn(packet(0x40903C00, 0x0000'0000)));         // 2.0 vel 0 is still ON
    CHECK(!ump::isNoteOff(packet(0x40903C00, 0x0000'0000)));
    CHECK(ump::isNoteOff(packet(0x40803C00, 0x8000'0000)));        // 2.0 note-off
    CHECK(!ump::isNoteOn(packet(0x10F80000)));                     // clock: not a note
}

void testProtocolClassification() {
    CHECK(ump::protocolOf(packet(0x20903C64)) == MidiProtocol::Midi1);
    CHECK(ump::protocolOf(packet(0x40903C00)) == MidiProtocol::Midi2);
    CHECK(ump::isProtocolNeutral(packet(0x10F80000)));             // timing clock
    CHECK(ump::isProtocolNeutral(packet(0x30160000)));             // SysEx7 complete
    CHECK(!ump::isProtocolNeutral(packet(0x20903C64)));
    CHECK(!ump::isChannelVoice(packet(0x10F80000)));
}

void testScaling() {
    // Expected values are what the ALSA kernel produced in M2.
    CHECK(ump::scaleUp(100, 7, 16) == 0xC924);
    CHECK(ump::scaleUp(100, 7, 32) == 0xC9249249);
    CHECK(ump::scaleUp(12288, 14, 32) == 0xC0020010);
    CHECK(ump::scaleUp(0, 7, 16) == 0);
    CHECK(ump::scaleUp(64, 7, 16) == 0x8000);     // center maps to center
    CHECK(ump::scaleUp(127, 7, 16) == 0xFFFF);    // max maps to max
    for (std::uint32_t v = 0; v < 128; ++v)       // round trip for 7-bit values
        CHECK(ump::scaleDown(ump::scaleUp(v, 7, 16), 16, 7) == v);
    CHECK(ump::velocity16(packet(0x20903C64)) == 0xC924);
    CHECK(ump::velocity16(packet(0x40903C00, 0x1234'0000)) == 0x1234);
}

void testToMidi1Bytes() {
    std::uint8_t bytes[3] = {};
    CHECK(ump::toMidi1Bytes(packet(0x20913C64), bytes) == 3);
    CHECK(bytes[0] == 0x91 && bytes[1] == 0x3C && bytes[2] == 0x64);
    CHECK(ump::toMidi1Bytes(packet(0x20C00500), bytes) == 2);          // program change
    CHECK(bytes[0] == 0xC0 && bytes[1] == 0x05);
    CHECK(ump::toMidi1Bytes(packet(0x10F80000), bytes) == 1);          // clock
    CHECK(bytes[0] == 0xF8);
    CHECK(ump::toMidi1Bytes(packet(0x10F20304), bytes) == 3);          // song position
    CHECK(bytes[0] == 0xF2 && bytes[1] == 0x03 && bytes[2] == 0x04);
    CHECK(ump::toMidi1Bytes(packet(0x40903C00, 0x8000'0000), bytes) == 0);   // MIDI 2.0: no
}

void testTranspose() {
    MidiMessage m1 = packet(0x20903C64);
    CHECK(ump::transposeNote(m1, 2) && ump::noteNumber(m1) == 62);
    CHECK((m1.words[0] & 0x7F) == 0x64);                               // velocity kept

    MidiMessage m2 = packet(0x40903D03, 0x8000'7B00);                   // 61 + pitch 61.5
    CHECK(ump::transposeNote(m2, -1) && ump::noteNumber(m2) == 60);
    CHECK((m2.words[1] & 0xFFFF) == ((60u << 9) | 256u));              // pitch moved too: 60.5
    CHECK((m2.words[1] >> 16) == 0x8000);                              // velocity kept

    MidiMessage top = packet(0x20907F64);
    CHECK(!ump::transposeNote(top, 1));                                // out of range...
    CHECK(ump::noteNumber(top) == 127);                                // ...and unchanged
    MidiMessage cc = packet(0x20B00764);
    CHECK(ump::transposeNote(cc, 5) && cc.words[0] == 0x20B00764);     // no note: untouched
}

// Feed a MIDI 1.0 CV packet through upconvert(); returns the outputs.
std::vector<MidiMessage> up(ump::UpconvertState& state, std::uint32_t w0) {
    MidiMessage out[ump::kMaxTranslatedMessages];
    const std::size_t n = ump::upconvert(packet(w0), state, out, ump::kMaxTranslatedMessages);
    return {out, out + n};
}

std::vector<MidiMessage> down(std::uint32_t w0, std::uint32_t w1) {
    MidiMessage out[ump::kMaxTranslatedMessages];
    const std::size_t n = ump::downconvert(packet(w0, w1), out, ump::kMaxTranslatedMessages);
    return {out, out + n};
}

void testUpconvert() {
    ump::UpconvertState state;

    auto m = up(state, 0x20903C64);                         // note-on vel 100
    CHECK(m.size() == 1 && m[0].words[0] == 0x40903C00 && m[0].words[1] == 0xC9240000);
    m = up(state, 0x20903C00);                              // vel 0 -> note-off, vel 0 (as the kernel)
    CHECK(m.size() == 1 && m[0].words[0] == 0x40803C00 && m[0].words[1] == 0);
    m = up(state, 0x20B00764);                              // CC 7 = 100
    CHECK(m.size() == 1 && m[0].words[0] == 0x40B00700 && m[0].words[1] == 0xC9249249);
    m = up(state, 0x20E00060);                              // pitch bend 12288
    CHECK(m.size() == 1 && m[0].words[0] == 0x40E00000 && m[0].words[1] == 0xC0020010);

    // RPN 0:0 with MSB-only Data Entry: emitted at the MSB (the kernel drops it)...
    CHECK(up(state, 0x20B06500).empty());                   // CC 101 = 0
    CHECK(up(state, 0x20B06400).empty());                   // CC 100 = 0
    m = up(state, 0x20B00602);                              // CC 6 = 2
    CHECK(m.size() == 1 && m[0].words[0] == 0x40200000 && m[0].words[1] == 0x04000000);
    m = up(state, 0x20B02605);                              // ...and again when an LSB follows
    CHECK(m.size() == 1 && m[0].words[1] == ump::scaleUp((2u << 7) | 5u, 14, 32));

    // NRPN 1:8 on channel 2, MSB + LSB.
    CHECK(up(state, 0x20B16301).empty());
    CHECK(up(state, 0x20B16208).empty());
    m = up(state, 0x20B10640);
    CHECK(m.size() == 1 && m[0].words[0] == 0x40310108 && m[0].words[1] == 0x80000000);

    // Null RPN (127/127): Data Entry is an ordinary controller again.
    up(state, 0x20B0657F);
    up(state, 0x20B0647F);
    m = up(state, 0x20B00640);
    CHECK(m.size() == 1 && m[0].words[0] == 0x40B00600);

    // Bank select is absorbed and rides on the next program change.
    CHECK(up(state, 0x20B00001).empty());                   // bank MSB 1
    CHECK(up(state, 0x20B02002).empty());                   // bank LSB 2
    m = up(state, 0x20C00500);                              // program 5
    CHECK(m.size() == 1 && m[0].words[0] == 0x40C00001 && m[0].words[1] == 0x05000102);

    // State is per group and channel: channel 3 has no RPN selected.
    m = up(state, 0x20B20603);
    CHECK(m.size() == 1 && m[0].words[0] == 0x40B20600);

    // Not MIDI 1.0 channel voice: passed through unchanged.
    m = up(state, 0x10F80000);
    CHECK(m.size() == 1 && m[0].words[0] == 0x10F80000);
}

void testDownconvert() {
    auto m = down(0x40903D03, 0x80007B00);                  // note-on vel 0x8000, pitch attribute
    CHECK(m.size() == 1 && m[0].words[0] == 0x20903D40);    // vel 64, attribute gone
    m = down(0x40903C00, 0x00000000);                       // vel 0 is a real note-on in 2.0...
    CHECK(m.size() == 1 && m[0].words[0] == 0x20903C01);    // ...so it becomes vel 1, not a note-off
    m = down(0x40B00700, 0xC9249249);
    CHECK(m.size() == 1 && m[0].words[0] == 0x20B00764);
    m = down(0x40E00000, 0xC0020010);
    CHECK(m.size() == 1 && m[0].words[0] == 0x20E00060);

    m = down(0x40200000, 0x04000000);                       // RPN 0:0 = 2 semitones
    CHECK(m.size() == 4);
    if (m.size() == 4) {
        CHECK(m[0].words[0] == 0x20B06500 && m[1].words[0] == 0x20B06400);
        CHECK(m[2].words[0] == 0x20B00602 && m[3].words[0] == 0x20B02600);
    }
    m = down(0x40C00001, 0x05000102);                       // program 5, bank 1:2
    CHECK(m.size() == 3);
    if (m.size() == 3)
        CHECK(m[0].words[0] == 0x20B00001 && m[1].words[0] == 0x20B02002 && m[2].words[0] == 0x20C00500);

    CHECK(down(0x40603C00, 0x80000000).empty());            // per-note pitch bend: no MIDI 1.0 form
}

void testRoundTrip() {
    // MIDI 1.0 -> 2.0 -> 1.0 returns the original for ordinary channel voice.
    const std::uint32_t originals[] = {0x20903C64, 0x20803C40, 0x20A33C12, 0x20B50741,
                                       0x20D47F00, 0x20E02A55, 0x20917F7F, 0x20900001};
    for (const std::uint32_t w0 : originals) {
        ump::UpconvertState state;
        const auto twoPointOh = up(state, w0);
        CHECK(twoPointOh.size() == 1);
        if (twoPointOh.size() != 1) continue;
        const auto back = down(twoPointOh[0].words[0], twoPointOh[0].words[1]);
        CHECK(back.size() == 1 && back[0].words[0] == w0);
    }
}

std::vector<MidiMessage> parse(std::initializer_list<std::uint8_t> bytes) {
    ump::Midi1ParserState    state;
    std::vector<MidiMessage> out;
    for (const std::uint8_t b : bytes) {
        MidiMessage m;
        if (ump::parseMidi1Byte(b, state, m)) out.push_back(m);
    }
    return out;
}

void testParser() {
    auto m = parse({0x90, 0x3C, 0x64});
    CHECK(m.size() == 1 && m[0].words[0] == 0x20903C64 && m[0].wordCount == 1);

    m = parse({0x90, 0x3C, 0x64, 0x3E, 0x00});             // running status; vel 0 kept as-is
    CHECK(m.size() == 2 && m[1].words[0] == 0x20903E00);

    m = parse({0x90, 0xF8, 0x3C, 0xFE, 0x64});             // real-time interleaved mid-message
    CHECK(m.size() == 3);
    if (m.size() == 3)
        CHECK(m[0].words[0] == 0x10F80000 && m[1].words[0] == 0x10FE0000 && m[2].words[0] == 0x20903C64);

    m = parse({0xC3, 0x05, 0x06});                          // program change: one data byte, running
    CHECK(m.size() == 2 && m[0].words[0] == 0x20C30500 && m[1].words[0] == 0x20C30600);

    m = parse({0xF2, 0x03, 0x04, 0xF6});                    // song position, tune request
    CHECK(m.size() == 2 && m[0].words[0] == 0x10F20304 && m[1].words[0] == 0x10F60000);

    m = parse({0xF3, 0x01, 0x02});                          // system common cancels running status
    CHECK(m.size() == 1 && m[0].words[0] == 0x10F30100);

    m = parse({0xF0, 0x7E, 0x7F, 0x06, 0xF7});              // short SysEx: one complete packet
    CHECK(m.size() == 1 && m[0].words[0] == 0x30037E7F && m[0].words[1] == 0x06000000);

    m = parse({0xF0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0xF7});   // 14 bytes
    CHECK(m.size() == 3);
    if (m.size() == 3) {
        CHECK(m[0].words[0] == 0x30160102 && m[0].words[1] == 0x03040506);   // start, 6
        CHECK(m[1].words[0] == 0x30260708 && m[1].words[1] == 0x090A0B0C);   // continue, 6
        CHECK(m[2].words[0] == 0x30320D0E && m[2].words[1] == 0);            // end, 2
    }

    m = parse({0x3C, 0x64});                                // stray data, no status: ignored
    CHECK(m.empty());
}

void testWordCounts() {
    // UMP spec packet sizes by message type 0x0..0xF.
    const std::uint8_t expected[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    for (std::uint8_t mt = 0; mt < 16; ++mt)
        CHECK(ump::wordCountForType(mt) == expected[mt]);
}

} // namespace

int main() {
    testFieldAccess();
    testNoteOnOffSemantics();
    testProtocolClassification();
    testWordCounts();
    testScaling();
    testToMidi1Bytes();
    testTranspose();
    testUpconvert();
    testDownconvert();
    testRoundTrip();
    testParser();
    return test::finish("UMP");
}
