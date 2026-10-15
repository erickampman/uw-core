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
#include "uw/ump/UmpStream.h"

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

// The stream messages among `got`, in order.
std::vector<MidiMessage> streamOnly(const std::vector<MidiMessage>& got) {
    std::vector<MidiMessage> out;
    for (const auto& m : got)
        if (ump::isStream(m)) out.push_back(m);
    return out;
}

void testEndpoint(CoreMidiOutput& out, CoreMidiInput& in, Collector& sink) {
    // A UMP endpoint of our own, as KeyboardEDO publishes: its source plays,
    // keeps the persistent id, and its destination answers discovery.
    constexpr std::int32_t kId   = 0x75774345;   // "uwCE"
    const char*            kName = "uw-core test endpoint";
    ump::EndpointInfo      e;
    e.name              = kName;
    e.productInstanceId = "uw-core-test-1";
    e.blocks            = {{"Keyboard", 0, 1, ump::FunctionBlock::Direction::Output, true}};
    const auto source   = out.openEndpoint(e, MidiProtocol::Midi2, kId);
    CHECK(source != kInvalidMidiDestination);
    CHECK(out.protocol(source) == MidiProtocol::Midi2);
    CHECK(out.inquiries(source) == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(uniqueIdOf(kName) == kId);

    const auto listening = in.open(kName);
    CHECK(listening != kInvalidMidiSource);
    CHECK(out.send(source, &kNoteOn2, 1));
    auto got = sink.wait(1);
    CHECK(got.size() == 1 && same(got[0], kNoteOn2));

    // A peer's discovery, sent to the endpoint's destination, is answered on
    // its source (and counted).
    const auto peer = out.open(kName);
    CHECK(peer != kInvalidMidiDestination);
    const MidiMessage ask[] = {ump::endpointDiscovery(0x0D), ump::functionBlockDiscovery()};
    CHECK(out.send(peer, ask, 2));
    got = streamOnly(sink.wait(6));   // info, name (two), Product Instance ID; block info and name
    CHECK(got.size() == 6);
    CHECK(out.inquiries(source) == 2);
    if (got.size() == 6) {
        CHECK(ump::streamStatus(got[0]) == ump::StreamStatus::EndpointInfo);
        CHECK((got[0].words[1] >> 24 & 0x7F) == 1);
        ump::StreamText name, id;
        name.add(got[1], 2);
        name.add(got[2], 2);
        id.add(got[3], 2);
        CHECK(name.complete() && name.text == kName);
        CHECK(id.complete() && id.text == "uw-core-test-1");
        CHECK(ump::streamStatus(got[4]) == ump::StreamStatus::FunctionBlockInfo);
        CHECK(got[4].words[1] == 0x00010000);   // group 1, one group
    }
    out.close(peer);

    // New blocks: a new endpoint, the same name and id, answering with them.
    CHECK(out.setFunctionBlocks(source, {{"Keyboard", 0, 5, ump::FunctionBlock::Direction::Output, true}}));
    CHECK(out.setFunctionBlocks(source, {{"Keyboard", 0, 5, ump::FunctionBlock::Direction::Output, true}}));   // unchanged
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(uniqueIdOf(kName) == kId);
    in.close(listening);
    const auto again = in.open(kName);   // as a host reconnects
    CHECK(again != kInvalidMidiSource);
    const auto peer2 = out.open(kName);
    CHECK(peer2 != kInvalidMidiDestination);
    const MidiMessage askBlocks = ump::functionBlockDiscovery(0xFF, 0x01);
    CHECK(out.send(peer2, &askBlocks, 1));
    got = streamOnly(sink.wait(1));
    CHECK(got.size() == 1 && got[0].words[1] == 0x00050000);   // groups 1-5
    CHECK(out.inquiries(source) == 3);
    CHECK(out.send(source, &kNoteOn2, 1));
    got = sink.wait(1);
    CHECK(got.size() == 1 && same(got[0], kNoteOn2));
    out.close(peer2);
    in.close(again);

    out.close(source);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(uniqueIdOf(kName) == 0);   // gone
    CHECK(!out.setFunctionBlocks(source, {}));
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
    testEndpoint(*out, *in, sink);

    CHECK(out->open("no such destination") == kInvalidMidiDestination);
    in->setSink(nullptr);
    return test::finish("coremidi-output");
}
