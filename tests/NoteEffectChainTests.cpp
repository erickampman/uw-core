// NoteEffectChain: note effects in series before an instrument
// (src/uw/plugin/NoteEffectChain.h).

#include <algorithm>
#include <vector>

#include "Check.h"
#include "uw/plugin/NoteEffectChain.h"

using namespace uw;

namespace {

// A note effect that moves each note by `semitones`, keeps everything else,
// and can answer out of order, add an offset past the block, send a
// parameter change, or take only one protocol.
class FakeEffect final : public HostedPlugin {
public:
    explicit FakeEffect(int semitones, NoteDialectMask dialects = NoteDialect::Midi | NoteDialect::Midi2)
        : shift(semitones), takes(dialects) {}

    int                 shift;
    NoteDialectMask     takes;
    bool                reverse = false, late = false, sendParam = false;
    std::uint32_t       audioChannels = 0;
    std::vector<Event>  got;        // the last block's input
    int                 calls = 0;

    const PluginDescriptor& descriptor() const override { return desc; }
    PortLayout portLayout() const override {
        PortLayout l;
        l.noteInputs.push_back({"in", takes, NoteDialect::Midi2});
        l.noteOutputs.push_back({"out", takes, NoteDialect::Midi2});
        if (audioChannels > 0) l.audioOutputs.push_back({"out", audioChannels, true});
        return l;
    }
    std::vector<ParameterInfo> parameters() const override { return {}; }
    bool activate(double, std::uint32_t) override { return true; }
    void deactivate() override {}
    void process(const ProcessContext& ctx) noexcept override {
        ++calls;
        got.assign(ctx.eventsIn.events, ctx.eventsIn.events + ctx.eventsIn.count);
        for (std::uint32_t c = 0; c < ctx.audioOut.channelCount; ++c)
            for (std::uint32_t f = 0; f < ctx.frameCount; ++f) ctx.audioOut.channels[c][f] = 1.0f;
        if (ctx.eventsOut == nullptr) return;
        std::vector<Event> out;
        for (const Event& e : got) {
            if (e.type != EventType::Midi) continue;
            Event o = e;
            if (ump::isNoteOn(o.midi) || ump::isNoteOff(o.midi)) ump::transposeNote(o.midi, shift);
            out.push_back(o);
        }
        if (reverse) std::reverse(out.begin(), out.end());
        if (late && !out.empty()) out.back().sampleOffset = ctx.frameCount + 50;
        for (const Event& e : out) ctx.eventsOut->push(e);
        if (sendParam) {
            Event p;
            p.type     = EventType::ParamValue;
            p.param.id = 7;
            p.value    = 0.5;
            ctx.eventsOut->push(p);
        }
    }
    void   stopProcessing() noexcept override {}
    double paramValue(std::uint32_t) const override { return 0; }
    void   flushParams(const EventBlock&, EventSink*) override {}
    bool   saveState(std::vector<std::byte>&) const override { return true; }
    bool   loadState(const std::byte*, std::size_t) override { return true; }
    void   idle() override {}

private:
    PluginDescriptor desc;
};

struct Recorder final : NoteEffectFeedback {
    std::vector<std::pair<HostedPlugin*, Event>> events;
    void effectEvent(HostedPlugin* effect, const Event& e) noexcept override { events.emplace_back(effect, e); }
};

Event midi(std::uint32_t offset, const MidiMessage& m, std::uint16_t port = 3) {
    Event e;
    e.sampleOffset = offset;
    e.type         = EventType::Midi;
    e.midi         = m;
    e.portIndex    = port;
    return e;
}

constexpr NoteDialectMask kBoth = NoteDialect::Midi | NoteDialect::Midi2;

void testEmptyAndBypassed() {
    NoteEffectChain chain;
    chain.prepare({}, kBoth, 0, 64, 32);
    const Event      in[] = {midi(5, ump::midi2NoteOn(0, 0, 60, 0x8000))};
    const EventBlock out  = chain.run(64, TimeInfo{}, EventBlock{in, 1});
    CHECK(chain.empty() && chain.passesThrough());
    CHECK(out.events == in && out.count == 1);   // nothing to do: the input itself

    FakeEffect a(12);
    chain.prepare({{&a, true}}, kBoth, 0, 64, 32);
    const EventBlock around = chain.run(64, TimeInfo{}, EventBlock{in, 1});
    CHECK(chain.passesThrough() && around.events == in);
    CHECK(a.calls == 1 && a.got.empty());   // it runs, without the MIDI
}

void testSeries() {
    FakeEffect a(12), b(-5), c(100);
    c.audioChannels = 2;
    NoteEffectChain chain;
    // b bypassed: a's output goes on to c.
    chain.prepare({{&a, false}, {&b, true}, {&c, false}}, kBoth, 2, 64, 32);
    CHECK(chain.contains(&b) && !chain.passesThrough() && chain.size() == 3);
    c.shift = 1;
    const Event in[] = {midi(0, ump::midi2NoteOn(0, 0, 60, 0x8000)), midi(10, ump::midi2NoteOff(0, 0, 60))};
    const EventBlock out = chain.run(64, TimeInfo{}, EventBlock{in, 2});
    CHECK(out.count == 2);
    CHECK(ump::noteNumber(out.events[0].midi) == 73 && ump::isNoteOn(out.events[0].midi));
    CHECK(ump::noteNumber(out.events[1].midi) == 73 && ump::isNoteOff(out.events[1].midi));
    CHECK(out.events[0].portIndex == 2 && out.events[1].sampleOffset == 10);
    CHECK(a.got.size() == 2 && a.got[0].portIndex == 0);   // a's own port 0, not the input's
    CHECK(b.got.empty() && c.got.size() == 2 && ump::noteNumber(c.got[0].midi) == 72);
}

void testTranslation() {
    // A MIDI 2.0 note with a Pitch 7.9 attribute: kept through an effect that
    // takes MIDI 2.0 to a destination that does; down to MIDI 1.0 for one
    // that doesn't.
    FakeEffect      a(0);
    NoteEffectChain chain;
    const std::uint16_t pitch = (60u << 9) + 198u;
    const Event         in[]  = {midi(0, ump::midi2NoteOn(0, 0, 60, 0x8000, pitch))};
    chain.prepare({{&a, false}}, NoteDialect::Midi2, 0, 64, 32);
    EventBlock out = chain.run(64, TimeInfo{}, EventBlock{in, 1});
    CHECK(out.count == 1 && ump::notePitch79(out.events[0].midi) == pitch);

    chain.prepare({{&a, false}}, NoteDialect::Midi, 0, 64, 32);
    out = chain.run(64, TimeInfo{}, EventBlock{in, 1});
    CHECK(out.count == 1 && ump::messageType(out.events[0].midi) == ump::MessageType::Midi1ChannelVoice);
    CHECK(ump::noteNumber(out.events[0].midi) == 60);

    // An effect that takes only MIDI 1.0 gets the note downconverted; one
    // after it that takes only MIDI 2.0 gets its output upconverted.
    FakeEffect one(0, NoteDialect::Midi), two(0, NoteDialect::Midi2);
    chain.prepare({{&one, false}, {&two, false}}, kBoth, 0, 64, 32);
    out = chain.run(64, TimeInfo{}, EventBlock{in, 1});
    CHECK(one.got.size() == 1 && ump::messageType(one.got[0].midi) == ump::MessageType::Midi1ChannelVoice);
    CHECK(two.got.size() == 1 && ump::messageType(two.got[0].midi) == ump::MessageType::Midi2ChannelVoice);
    CHECK(out.count == 1 && ump::isNoteOn(out.events[0].midi));

    // A destination that takes no MIDI (a CLAP-only port): dropped, counted.
    chain.prepare({{&a, false}}, NoteDialect::Clap, 0, 64, 32);
    out = chain.run(64, TimeInfo{}, EventBlock{in, 1});
    CHECK(out.count == 0 && chain.takeUnsupported() == 1 && chain.takeUnsupported() == 0);
    // SysEx (MT 3) to a MIDI 1.0 effect: no form it takes.
    MidiMessage sysex;
    sysex.words[0]  = 0x30010000u;
    sysex.wordCount = 2;
    const Event sx[] = {midi(0, sysex)};
    chain.prepare({{&one, false}}, kBoth, 0, 64, 32);
    out = chain.run(64, TimeInfo{}, EventBlock{sx, 1});
    CHECK(one.got.empty() && chain.takeUnsupported() == 1);
}

void testOrderClampAndFeedback() {
    FakeEffect a(0);
    a.reverse   = true;
    a.late      = true;
    a.sendParam = true;
    Recorder        rec;
    NoteEffectChain chain;
    chain.prepare({{&a, false}}, kBoth, 0, 64, 32);
    const Event in[] = {midi(1, ump::midi2NoteOn(0, 0, 60, 0x8000)), midi(20, ump::midi2NoteOn(0, 0, 62, 0x8000)),
                        midi(40, ump::midi2NoteOn(0, 0, 64, 0x8000))};
    const EventBlock out = chain.run(64, TimeInfo{}, EventBlock{in, 3}, nullptr, &rec);
    CHECK(out.count == 3);
    // Reversed and the last pushed (key 60) moved past the block: sorted,
    // and clamped to its last frame.
    CHECK(out.events[0].sampleOffset == 20 && out.events[1].sampleOffset == 40 && out.events[2].sampleOffset == 63);
    CHECK(ump::noteNumber(out.events[2].midi) == 60);
    CHECK(rec.events.size() == 1 && rec.events[0].first == &a && rec.events[0].second.param.id == 7);
}

void testParamsAndCapacity() {
    FakeEffect      a(0), b(0);
    NoteEffectChain chain;
    chain.prepare({{&a, false}, {&b, true}}, kBoth, 0, 64, 2);
    Event p;
    p.type         = EventType::ParamValue;
    p.sampleOffset = 4;
    p.param.id     = 1;
    const EventBlock params[] = {EventBlock{&p, 1}, EventBlock{&p, 1}};
    const Event in[] = {midi(0, ump::midi2NoteOn(0, 0, 60, 0x8000)), midi(8, ump::midi2NoteOn(0, 0, 61, 0x8000)),
                        midi(9, ump::midi2NoteOn(0, 0, 62, 0x8000))};
    const EventBlock out = chain.run(64, TimeInfo{}, EventBlock{in, 3}, params);
    // Two fit at each stage: the third is dropped and counted.
    CHECK(out.count == 2 && chain.takeDropped() == 1 && chain.takeDropped() == 0);
    // a got the MIDI and its parameter change in time order; bypassed b, its change only.
    CHECK(a.got.size() == 3 && a.got[1].type == EventType::ParamValue);
    CHECK(b.got.size() == 1 && b.got[0].type == EventType::ParamValue);
}

} // namespace

int main() {
    testEmptyAndBypassed();
    testSeries();
    testTranslation();
    testOrderClampAndFeedback();
    testParamsAndCapacity();
    return uw::test::finish("note-effect-chain");
}
