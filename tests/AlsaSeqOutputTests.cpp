// AlsaSeqOutput, looped back through AlsaSeqInput in the same process.
// Silent: only ports of our own, which exist while the test runs (other apps
// may list them for that moment), and nothing is sent anywhere else.

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include <alsa/asoundlib.h>

#include "Check.h"
#include "uw/midi/AlsaSeqInput.h"
#include "uw/midi/AlsaSeqOutput.h"

using namespace uw;

namespace {

struct Collector final : MidiSink {
    std::mutex               lock;
    std::vector<MidiMessage> received;

    void onMidi(MidiSourceHandle, const TimedMidiMessage& m) noexcept override {
        std::lock_guard<std::mutex> g(lock);
        received.push_back(m.message);
    }
    // Waits up to two seconds for `count` messages.
    std::vector<MidiMessage> wait(std::size_t count) {
        for (int i = 0; i < 200; ++i) {
            {
                std::lock_guard<std::mutex> g(lock);
                if (received.size() >= count) {
                    auto out = received;
                    received.clear();
                    return out;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::lock_guard<std::mutex> g(lock);
        auto out = received;
        received.clear();
        return out;
    }
};

MidiMessage message(std::uint32_t w0, std::uint32_t w1 = 0, std::uint8_t words = 1) {
    MidiMessage m;
    m.words[0]  = w0;
    m.words[1]  = w1;
    m.wordCount = words;
    return m;
}

bool same(const MidiMessage& a, const MidiMessage& b) {
    if (a.wordCount != b.wordCount) return false;
    for (int i = 0; i < a.wordCount; ++i)
        if (a.words[i] != b.words[i]) return false;
    return true;
}

// MIDI 2.0: a note-on with a Pitch 7.9 attribute and a per-note pitch bend, as
// KeyboardEDO sends them. MIDI 1.0: a note-on.
const MidiMessage kNoteOn2 = message(0x40911103, 0xFFFF5A00, 2);
const MidiMessage kBend2   = message(0x40611100, 0x90000000, 2);
const MidiMessage kNoteOn1 = message(0x20913C64);

void testVirtualSource(AlsaSeqOutput& out, AlsaSeqInput& in, Collector& sink, MidiProtocol protocol,
                       const char* name, const MidiMessage* sent, std::size_t count) {
    // KeyEDO's "Publish": our source, which another app (here, our input) opens.
    const auto source = out.openVirtual(name, protocol);
    CHECK(source != kInvalidMidiDestination);
    CHECK(out.protocol(source) == protocol);
    std::printf("  %s\n", out.describe(source).c_str());

    const std::string client = protocol == MidiProtocol::Midi2 ? "uw-core output tests MIDI 2.0"
                                                               : "uw-core output tests";
    bool listed = false;
    for (const auto& s : in.enumerate())
        if (s.id == client + ":" + name) {
            listed = true;
            CHECK(s.protocol == protocol);   // others see the protocol it sends
        }
    CHECK(listed);

    const auto handle = in.open(client + ":" + name);
    CHECK(handle != kInvalidMidiSource);
    CHECK(out.send(source, sent, count));
    const auto got = sink.wait(count);
    CHECK(got.size() == count);
    if (got.size() == count)
        for (std::size_t i = 0; i < count; ++i) CHECK(same(got[i], sent[i]));   // untouched
    in.close(handle);
    out.close(source);
    CHECK(!out.send(source, sent, count));   // closed
}

void testDestination(AlsaSeqOutput& out, AlsaSeqInput& in, Collector& sink, MidiProtocol protocol,
                     const char* name, const MidiMessage& m) {
    // Another app's destination (here, our input's virtual input).
    const auto input = in.openVirtual(name, protocol);
    CHECK(input != kInvalidMidiSource);

    const std::string client = protocol == MidiProtocol::Midi2 ? "uw-core input tests MIDI 2.0"
                                                               : "uw-core input tests";
    const std::string id     = client + ":" + name;
    bool listed = false;
    for (const auto& d : out.enumerate())
        if (d.id == id) {
            listed = true;
            CHECK(d.isVirtual);
            CHECK(!d.isHardware);
            CHECK(d.protocol == protocol);
        }
    CHECK(listed);

    const auto handle = out.open(id);
    CHECK(handle != kInvalidMidiDestination);
    CHECK(out.protocol(handle) == protocol);
    std::printf("  %s\n", out.describe(handle).c_str());
    CHECK(out.send(handle, &m, 1));
    const auto got = sink.wait(1);
    CHECK(got.size() == 1 && same(got[0], m));
    CHECK(out.sync(handle));

    // Gone: sync says so, send refuses, and nothing is left queued.
    const std::uint64_t before = out.changes();
    in.close(input);
    for (int i = 0; i < 100 && out.changes() == before; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(out.changes() != before);
    CHECK(!out.sync(handle));
    CHECK(!out.send(handle, &m, 1));

    // Back (at a new port): sync finds it by its id, and it hears us again.
    const auto again = in.openVirtual(name, protocol);
    CHECK(out.sync(handle));
    CHECK(out.send(handle, &m, 1));
    const auto heard = sink.wait(1);
    CHECK(heard.size() == 1 && same(heard[0], m));
    out.close(handle);
    in.close(again);
}

void testManyMessages(AlsaSeqOutput& out, AlsaSeqInput& in, Collector& sink) {
    // More than one write's worth, in order.
    const auto input = in.openVirtual("uw-core test burst", MidiProtocol::Midi2);
    const auto handle = out.open("uw-core input tests MIDI 2.0:uw-core test burst");
    CHECK(handle != kInvalidMidiDestination);
    std::vector<MidiMessage> sent;
    for (std::uint32_t i = 0; i < 300; ++i) sent.push_back(message(0x40911100 | (i & 0x7F) << 8, i << 16, 2));
    CHECK(out.send(handle, sent.data(), sent.size()));
    const auto got = sink.wait(sent.size());
    CHECK(got.size() == sent.size());
    bool inOrder = got.size() == sent.size();
    for (std::size_t i = 0; inOrder && i < got.size(); ++i) inOrder = same(got[i], sent[i]);
    CHECK(inOrder);
    CHECK(in.overrunCount() == 0);   // AlsaSeqInput's input pool holds a burst
    out.close(handle);
    in.close(input);
}

void testLegacyDestination(AlsaSeqOutput& out) {
    // A client that predates UMP (FluidSynth, most Linux synths): MIDI 1.0,
    // and the kernel turns MT 0x2 into its ordinary events.
    snd_seq_t* legacy = nullptr;
    CHECK(snd_seq_open(&legacy, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) == 0);
    if (legacy == nullptr) return;
    snd_seq_set_client_name(legacy, "uw-core legacy tests");
    const int port = snd_seq_create_simple_port(legacy, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                                SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    CHECK(port >= 0);

    bool listed = false;
    for (const auto& d : out.enumerate())
        if (d.id == "uw-core legacy tests:in") {
            listed = true;
            CHECK(d.protocol == MidiProtocol::Midi1);
        }
    CHECK(listed);

    const auto handle = out.open("uw-core legacy tests:in");
    CHECK(out.protocol(handle) == MidiProtocol::Midi1);
    CHECK(out.send(handle, &kNoteOn1, 1));   // channel 2, note 60, velocity 100
    snd_seq_event_t* event = nullptr;
    int result = -EAGAIN;
    for (int i = 0; i < 200 && result == -EAGAIN; ++i) {
        result = snd_seq_event_input(legacy, &event);
        if (result == -EAGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(result >= 0 && event != nullptr);
    if (result >= 0 && event != nullptr) {
        CHECK(event->type == SND_SEQ_EVENT_NOTEON);
        CHECK(event->data.note.channel == 1 && event->data.note.note == 60 && event->data.note.velocity == 100);
    }
    out.close(handle);
    snd_seq_close(legacy);
}

} // namespace

int main() {
    auto out = AlsaSeqOutput::create("uw-core output tests");
    auto in  = AlsaSeqInput::create("uw-core input tests");
    CHECK(out != nullptr);   // needs /dev/snd/seq (the snd-seq module) and UMP support
    CHECK(in != nullptr);
    if (!out || !in) return test::finish("alsa-output");

    Collector sink;
    in->setSink(&sink);
    const MidiMessage sent2[] = {kNoteOn2, kBend2};
    testVirtualSource(*out, *in, sink, MidiProtocol::Midi2, "uw-core test source 2.0", sent2, 2);
    testVirtualSource(*out, *in, sink, MidiProtocol::Midi1, "uw-core test source 1.0", &kNoteOn1, 1);
    testDestination(*out, *in, sink, MidiProtocol::Midi2, "uw-core test input 2.0", kNoteOn2);
    testDestination(*out, *in, sink, MidiProtocol::Midi1, "uw-core test input 1.0", kNoteOn1);
    testManyMessages(*out, *in, sink);
    testLegacyDestination(*out);

    CHECK(out->open("no such destination") == kInvalidMidiDestination);
    in->setSink(nullptr);
    return test::finish("alsa-output");
}
