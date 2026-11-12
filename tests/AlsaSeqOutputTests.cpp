// AlsaSeqOutput, looped back through AlsaSeqInput in the same process.
// Silent: only ports of our own, which exist while the test runs (other apps
// may list them for that moment), and nothing is sent anywhere else.

#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <alsa/asoundlib.h>

#include "Check.h"
#include "uw/midi/AlsaSeqInput.h"
#include "uw/base/Clock.h"
#include "uw/midi/AlsaSeqOutput.h"
#include "uw/ump/UmpStream.h"

using namespace uw;

namespace {

struct Collector final : MidiSink {
    std::mutex               lock;
    std::vector<MidiMessage> received;
    std::vector<Nanoseconds> times;   // each one's capture time, until clear()

    void onMidi(MidiSourceHandle, const TimedMidiMessage& m) noexcept override {
        std::lock_guard<std::mutex> g(lock);
        received.push_back(m.message);
        times.push_back(m.captureTime);
    }
    std::vector<Nanoseconds> takeTimes() {
        std::lock_guard<std::mutex> g(lock);
        auto out = times;
        times.clear();
        return out;
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

void testScheduled(AlsaSeqOutput& out, AlsaSeqInput& in, Collector& sink) {
    // sendAt(): held by the kernel's queue until its time, in order; a time
    // past is now; cancelScheduled() drops only its own handle's.
    const auto input = in.openVirtual("uw-core test scheduled", MidiProtocol::Midi2);
    const std::string id = "uw-core input tests MIDI 2.0:uw-core test scheduled";
    const auto handle = out.open(id);
    const auto other  = out.open(id);   // the same place, cancelled on its own
    CHECK(handle != kInvalidMidiDestination && other != kInvalidMidiDestination);
    CHECK(out.schedules(handle));
    sink.wait(0);
    sink.takeTimes();

    const MidiMessage later = message(0x40911100 | 61 << 8, 0x80000000, 2);
    const MidiMessage first = message(0x40911100 | 62 << 8, 0x80000000, 2);
    const Nanoseconds now   = nowNanoseconds();
    CHECK(out.sendAt(handle, &later, 1, now + 150'000'000));
    CHECK(out.sendAt(handle, &first, 1, now + 50'000'000));
    CHECK(out.sendAt(handle, &kNoteOn2, 1, now - 1'000'000));   // past: now
    auto got   = sink.wait(3);
    auto times = sink.takeTimes();
    CHECK(got.size() == 3 && times.size() == 3);
    if (got.size() == 3 && times.size() == 3) {
        CHECK(same(got[0], kNoteOn2) && same(got[1], first) && same(got[2], later));
        const auto off = [&](std::size_t i, Nanoseconds want) { return (times[i] - now - want) / 1'000'000; };
        std::printf("  scheduled: arrived %+lld ms and %+lld ms from their times\n",
                    static_cast<long long>(off(1, 50'000'000)), static_cast<long long>(off(2, 150'000'000)));
        CHECK(times[0] - now < 40'000'000);
        CHECK(off(1, 50'000'000) >= -1 && off(1, 50'000'000) < 15);   // the capture thread's wake-up, on a busy machine
        CHECK(off(2, 150'000'000) >= -1 && off(2, 150'000'000) < 15);
    }

    // Cancelled: nothing of `handle`'s arrives; `other`'s does.
    const Nanoseconds then = nowNanoseconds();
    CHECK(out.sendAt(handle, &later, 1, then + 100'000'000));
    CHECK(out.sendAt(other, &first, 1, then + 150'000'000));
    out.cancelScheduled(handle);
    out.cancelScheduled(handle);   // twice: nothing left
    got = sink.wait(2);
    CHECK(got.size() == 1 && same(got[0], first));
    sink.takeTimes();

    // Closing cancels too.
    CHECK(out.sendAt(other, &later, 1, nowNanoseconds() + 100'000'000));
    out.close(other);
    CHECK(sink.wait(1).empty());
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

    // Senders subscribed to the port: open() subscribes, close() undoes it,
    // as VirMIDI takes only what subscribed senders send.
    auto writers = [&] {
        snd_seq_query_subscribe_t* query = nullptr;
        snd_seq_query_subscribe_alloca(&query);
        snd_seq_addr_t root{static_cast<unsigned char>(snd_seq_client_id(legacy)), static_cast<unsigned char>(port)};
        snd_seq_query_subscribe_set_root(query, &root);
        snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_WRITE);
        int count = 0;
        for (snd_seq_query_subscribe_set_index(query, 0); snd_seq_query_port_subscribers(legacy, query) >= 0;
             snd_seq_query_subscribe_set_index(query, snd_seq_query_subscribe_get_index(query) + 1))
            ++count;
        return count;
    };
    CHECK(writers() == 0);

    const auto handle = out.open("uw-core legacy tests:in");
    CHECK(writers() == 1);
    const auto second = out.open("uw-core legacy tests:in");   // the same pair: one subscription
    CHECK(writers() == 1);
    out.close(second);
    CHECK(writers() == 1);   // still the first handle's
    CHECK(out.protocol(handle) == MidiProtocol::Midi1);
    CHECK(out.send(handle, &kNoteOn1, 1));   // channel 2, note 60, velocity 100
    // Wait up to two seconds for the note-on, skipping anything else that
    // reaches the port first (another client's subscription notice, say).
    snd_seq_event_t* event = nullptr;
    for (int i = 0; i < 200 && event == nullptr; ++i) {
        snd_seq_event_t* e = nullptr;
        const int result = snd_seq_event_input(legacy, &e);
        if (result >= 0 && e != nullptr && e->type == SND_SEQ_EVENT_NOTEON) event = e;
        else if (result == -EAGAIN) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(event != nullptr);
    if (event != nullptr)
        CHECK(event->data.note.channel == 1 && event->data.note.note == 60 && event->data.note.velocity == 100);
    out.close(handle);
    CHECK(writers() == 0);
    snd_seq_close(legacy);
}

#if SND_LIB_VERSION >= SND_LIB_VER(1, 2, 13)   // AlsaSeqEndpoint's
// Other UMP clients on the sequencer (PipeWire's bridge, a running MIDI 2.0
// app) may subscribe to the test endpoint or ask about it too, which adds to
// its inquiries. With any of them around, counts are checked as "at least";
// alone, as exact.
bool othersListening = false;

bool otherUmpClients() {
    snd_seq_t* seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) != 0) return false;
    snd_seq_client_info_t* info = nullptr;
    snd_seq_client_info_alloca(&info);
    snd_seq_client_info_set_client(info, -1);
    bool found = false;
    while (snd_seq_query_next_client(seq, info) >= 0)
        if (snd_seq_client_info_get_midi_version(info) != SND_SEQ_CLIENT_LEGACY_MIDI &&
            std::string(snd_seq_client_info_get_name(info)).rfind("uw-core", 0) != 0)
            found = true;
    snd_seq_close(seq);
    return found;
}

bool inquiriesAre(const AlsaSeqOutput& out, MidiDestinationHandle h, std::uint64_t count) {
    return othersListening ? out.inquiries(h) >= count : out.inquiries(h) == count;
}

// Waits up to two seconds for `inquiries` to reach `count`.
bool inquiriesReach(const AlsaSeqOutput& out, MidiDestinationHandle h, std::uint64_t count) {
    for (int i = 0; i < 200 && out.inquiries(h) < count; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return inquiriesAre(out, h, count);
}

// A peer of the endpoint, as another app would be: a UMP MIDI 2.0 client
// with a port that sends to it and hears it.
struct Peer {
    snd_seq_t* seq  = nullptr;
    int        port = -1;

    Peer() {
        if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) < 0) return;
        snd_seq_set_client_midi_version(seq, SND_SEQ_CLIENT_UMP_MIDI_2_0);
        snd_seq_set_client_name(seq, "uw-core peer tests");
        port = snd_seq_create_simple_port(seq, "peer", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE,
                                          SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    }
    ~Peer() {
        if (seq != nullptr) snd_seq_close(seq);
    }
    void send(int client, int toPort, const MidiMessage& m) {
        snd_seq_ump_event_t event;
        snd_seq_ump_ev_clear(&event);
        snd_seq_ev_set_ump_data(&event, const_cast<std::uint32_t*>(m.words), m.wordCount * sizeof m.words[0]);
        snd_seq_ev_set_source(&event, port);
        snd_seq_ev_set_dest(&event, client, toPort);
        snd_seq_ev_set_direct(&event);
        CHECK(snd_seq_ump_event_output_direct(seq, &event) >= 0);
    }
    // Waits up to two seconds for `count` UMP Stream messages.
    std::vector<MidiMessage> wait(std::size_t count) {
        std::vector<MidiMessage> got;
        for (int i = 0; i < 200 && got.size() < count;) {
            snd_seq_ump_event_t* event = nullptr;
            if (snd_seq_ump_event_input(seq, &event) < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                ++i;
                continue;
            }
            if (!snd_seq_ev_is_ump(event) || (event->ump[0] >> 28) != 0xF) continue;
            got.push_back(message(event->ump[0], event->ump[1], 4));
            got.back().words[2] = event->ump[2];
            got.back().words[3] = event->ump[3];
        }
        return got;
    }
};

bool listed(AlsaSeqInput& in, const std::string& id) {
    for (const auto& s : in.enumerate())
        if (s.id == id) return true;
    return false;
}

void testEndpoint(AlsaSeqOutput& out, AlsaSeqInput& in, Collector& sink) {
    // A UMP endpoint of our own, as KeyboardEDO publishes: a client of its own
    // whose info the kernel holds, a port per group of its blocks, sending to
    // their subscribers and answering a peer's discovery.
    othersListening = otherUmpClients();
    if (othersListening)
        std::fprintf(stderr, "  other UMP clients present: endpoint inquiries checked as at least\n");
    const std::string kName = "uw-core test endpoint";
    ump::EndpointInfo e;
    e.name              = kName;
    e.productInstanceId = "uw-core-test-1";
    e.manufacturer      = {0x7D, 0, 0};
    e.family            = 0x0102;
    e.model             = 0x0003;
    e.revision          = {0, 1, 2, 3};
    e.blocks            = {{"Keyboard", 0, 1, ump::FunctionBlock::Direction::Output, true}};
    const auto source   = out.openEndpoint(e, MidiProtocol::Midi2);
    CHECK(source != kInvalidMidiDestination);
    CHECK(out.protocol(source) == MidiProtocol::Midi2);
    CHECK(inquiriesAre(out, source, 0));
    std::printf("  %s\n", out.describe(source).c_str());
    for (const auto& d : out.enumerate()) CHECK(d.id.rfind(kName + ":", 0) != 0);   // not our own

    // What other apps read from the kernel.
    Peer peer;
    CHECK(peer.port >= 0);
    snd_seq_client_info_t* client = nullptr;
    snd_seq_client_info_alloca(&client);
    int endpoint = -1;
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(peer.seq, client) >= 0)
        if (kName == snd_seq_client_info_get_name(client)) endpoint = snd_seq_client_info_get_client(client);
    CHECK(endpoint >= 0);
    snd_ump_endpoint_info_t* ep = nullptr;
    snd_ump_endpoint_info_alloca(&ep);
    CHECK(snd_seq_get_ump_endpoint_info(peer.seq, endpoint, ep) == 0);
    CHECK(kName == snd_ump_endpoint_info_get_name(ep));
    CHECK(std::string("uw-core-test-1") == snd_ump_endpoint_info_get_product_id(ep));
    CHECK(snd_ump_endpoint_info_get_num_blocks(ep) == 1);
    CHECK(snd_ump_endpoint_info_get_protocol(ep) == SND_UMP_EP_INFO_PROTO_MIDI2);
    CHECK(snd_ump_endpoint_info_get_manufacturer_id(ep) == 0x7D0000);
    CHECK(snd_ump_endpoint_info_get_family_id(ep) == 0x0202);   // 0x0102: 7-bit bytes 0x02, 0x02
    CHECK(snd_ump_endpoint_info_get_model_id(ep) == 0x0003);
    snd_ump_block_info_t* block = nullptr;
    snd_ump_block_info_alloca(&block);
    CHECK(snd_seq_get_ump_block_info(peer.seq, endpoint, 0, block) == 0);
    CHECK(std::string("Keyboard") == snd_ump_block_info_get_name(block));
    CHECK(snd_ump_block_info_get_first_group(block) == 0 && snd_ump_block_info_get_num_groups(block) == 1);
    CHECK(snd_ump_block_info_get_direction(block) == SND_UMP_DIR_OUTPUT);
    CHECK(listed(in, kName + ":MIDI 2.0"));
    CHECK(listed(in, kName + ":Group 1 (Keyboard)"));
    CHECK(!listed(in, kName + ":Group 2"));   // no block holds it

    // A receiver subscribing to group 1's port is counted, and hears what's
    // sent on group 1 (the kernel passes it on from port 0).
    const auto group1 = in.open(kName + ":Group 1 (Keyboard)");
    CHECK(group1 != kInvalidMidiSource);
    CHECK(inquiriesReach(out, source, 1));
    CHECK(out.send(source, &kNoteOn2, 1));
    auto got = sink.wait(1);
    CHECK(got.size() == 1 && same(got[0], kNoteOn2));

    // A peer listening on port 0 (counted) asks; the answers come back to it.
    CHECK(snd_seq_connect_from(peer.seq, peer.port, endpoint, 0) == 0);
    CHECK(inquiriesReach(out, source, 2));
    peer.send(endpoint, 0, ump::endpointDiscovery(0x0D));
    peer.send(endpoint, 0, ump::functionBlockDiscovery());
    auto answers = peer.wait(6);   // info, name (two), Product Instance ID; block info and name
    CHECK(answers.size() == 6);
    CHECK(inquiriesReach(out, source, 4));
    if (answers.size() == 6) {
        CHECK(ump::streamStatus(answers[0]) == ump::StreamStatus::EndpointInfo);
        ump::StreamText name, id;
        name.add(answers[1], 2);
        name.add(answers[2], 2);
        id.add(answers[3], 2);
        CHECK(name.complete() && name.text == kName);
        CHECK(id.complete() && id.text == "uw-core-test-1");
        CHECK(ump::streamStatus(answers[4]) == ump::StreamStatus::FunctionBlockInfo);
        CHECK(answers[4].words[1] == 0x00010000);   // group 1, one group
    }

    // New blocks, in place: the kernel's info and the ports change, both
    // listeners stay subscribed, and the peer is told.
    CHECK(out.setFunctionBlocks(source, {{"Keyboard", 0, 5, ump::FunctionBlock::Direction::Output, true}}));
    answers = peer.wait(3);   // endpoint info; block info and name
    CHECK(answers.size() == 3 && ump::streamStatus(answers[1]) == ump::StreamStatus::FunctionBlockInfo &&
          answers[1].words[1] == 0x00050000);   // groups 1-5
    CHECK(out.setFunctionBlocks(source, {{"Keyboard", 0, 5, ump::FunctionBlock::Direction::Output, true}}));   // unchanged
    CHECK(peer.wait(1).empty());
    CHECK(snd_seq_get_ump_block_info(peer.seq, endpoint, 0, block) == 0);
    CHECK(snd_ump_block_info_get_num_groups(block) == 5);
    CHECK(listed(in, kName + ":Group 5 (Keyboard)"));
    CHECK(!listed(in, kName + ":Group 6"));
    CHECK(out.send(source, &kNoteOn2, 1));
    got = sink.wait(1);
    CHECK(got.size() == 1 && same(got[0], kNoteOn2));
    CHECK(inquiriesAre(out, source, 4));

    in.close(group1);
    out.close(source);
    CHECK(snd_seq_get_ump_endpoint_info(peer.seq, endpoint, ep) < 0);   // gone
    CHECK(!out.setFunctionBlocks(source, {}));
    CHECK(out.inquiries(source) == 0);
}
#else
void testEndpoint(AlsaSeqOutput& out, AlsaSeqInput&, Collector&) {
    // Before alsa-lib 1.2.13: a plain source, which has no blocks to change.
    ump::EndpointInfo e;
    e.name            = "uw-core test endpoint";
    const auto source = out.openEndpoint(e, MidiProtocol::Midi2);
    CHECK(source != kInvalidMidiDestination);
    CHECK(out.describe(source).rfind("virtual source", 0) == 0);
    CHECK(!out.setFunctionBlocks(source, {}));
    out.close(source);
}
#endif

} // namespace

int main() {
    if (access("/dev/snd/seq", F_OK) != 0) {   // no snd-seq module (CI runners): skip, don't fail
        std::printf("alsa-output: skipped, no ALSA sequencer (/dev/snd/seq)\n");
        return 77;                              // SKIP_RETURN_CODE in CMakeLists.txt
    }
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
    testScheduled(*out, *in, sink);
    testLegacyDestination(*out);
    testEndpoint(*out, *in, sink);

    CHECK(out->open("no such destination") == kInvalidMidiDestination);
    in->setSink(nullptr);
    return test::finish("alsa-output");
}
