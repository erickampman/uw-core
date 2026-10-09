// CoreMidiOutput, looped back through CoreMidiInput in the same process.
// Silent: only virtual endpoints of our own, which exist while the test runs
// (other apps may list them for that moment), and no device is opened.

#include <CoreMIDI/CoreMIDI.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "Check.h"
#include "uw/midi/CoreMidiInput.h"
#include "uw/midi/CoreMidiOutput.h"

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

void testVirtualSource(CoreMidiOutput& out, CoreMidiInput& in, Collector& sink) {
    // KeyEDO's "Publish": our source, which another app (here, our input) opens.
    const auto source = out.openVirtual("uw-core test source", MidiProtocol::Midi2);
    CHECK(source != kInvalidMidiDestination);
    CHECK(out.protocol(source) == MidiProtocol::Midi2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // let CoreMIDI list it

    const auto handle = in.open("uw-core test source");
    CHECK(handle != kInvalidMidiSource);
    const MidiMessage sent[] = {kNoteOn2, kBend2};
    CHECK(out.send(source, sent, 2));
    const auto got = sink.wait(2);
    CHECK(got.size() == 2);
    if (got.size() == 2) {
        CHECK(same(got[0], kNoteOn2));   // untouched: attribute and all
        CHECK(same(got[1], kBend2));
    }
    in.close(handle);
    out.close(source);
    CHECK(!out.send(source, sent, 2));   // closed
}

void testDestination(CoreMidiOutput& out, CoreMidiInput& in, Collector& sink, MidiProtocol protocol,
                     const char* name, const MidiMessage& m) {
    // Another app's destination (here, our input's virtual input).
    const auto input = in.openVirtual(name, protocol);
    CHECK(input != kInvalidMidiSource);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    bool listed = false;
    for (const auto& d : out.enumerate())
        if (d.id == name) {
            listed = true;
            CHECK(d.isVirtual);
            CHECK(d.protocol == protocol);
        }
    CHECK(listed);

    const auto handle = out.open(name);
    CHECK(handle != kInvalidMidiDestination);
    CHECK(out.protocol(handle) == protocol);
    CHECK(out.send(handle, &m, 1));
    const auto got = sink.wait(1);
    CHECK(got.size() == 1 && same(got[0], m));
    CHECK(out.sync(handle));
    out.close(handle);
    in.close(input);
}

// The unique ID CoreMIDI has for our source named `name`, or 0.
std::int32_t uniqueIdOf(const char* name) {
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        const MIDIEndpointRef e = MIDIGetSource(i);
        CFStringRef n = nullptr;
        if (MIDIObjectGetStringProperty(e, kMIDIPropertyName, &n) != noErr || !n) continue;
        char buf[128] = {};
        CFStringGetCString(n, buf, sizeof buf, kCFStringEncodingUTF8);
        CFRelease(n);
        SInt32 id = 0;
        if (std::string(buf) == name && MIDIObjectGetIntegerProperty(e, kMIDIPropertyUniqueID, &id) == noErr)
            return id;
    }
    return 0;
}

void testPersistentId(CoreMidiOutput& out) {
    // The same id every time it's published, so hosts remember the device.
    constexpr std::int32_t kId = 0x75774354;   // "uwCT"
    for (int run = 0; run < 2; ++run) {
        const auto source = out.openVirtual("uw-core test persistent", MidiProtocol::Midi2, kId);
        CHECK(source != kInvalidMidiDestination);
        CHECK(uniqueIdOf("uw-core test persistent") == kId);
        out.close(source);
    }
    // Without one, CoreMIDI picks.
    const auto source = out.openVirtual("uw-core test persistent", MidiProtocol::Midi2);
    CHECK(uniqueIdOf("uw-core test persistent") != 0);
    out.close(source);
}

} // namespace

int main() {
    auto out = CoreMidiOutput::create("uw-core output tests");
    auto in  = CoreMidiInput::create("uw-core input tests");
    CHECK(out != nullptr);
    CHECK(in != nullptr);
    if (!out || !in) return test::finish("coremidi-output");

    Collector sink;
    in->setSink(&sink);
    testVirtualSource(*out, *in, sink);
    testDestination(*out, *in, sink, MidiProtocol::Midi2, "uw-core test input 2.0", kNoteOn2);
    testDestination(*out, *in, sink, MidiProtocol::Midi1, "uw-core test input 1.0", kNoteOn1);
    testPersistentId(*out);

    CHECK(out->open("no such destination") == kInvalidMidiDestination);
    in->setSink(nullptr);
    return test::finish("coremidi-output");
}
