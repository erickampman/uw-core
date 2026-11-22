#include "uw/plugin/NoteEffectChain.h"

#include <algorithm>
#include <utility>

namespace uw {

namespace {

bool takesMidi1(NoteDialectMask dialects) noexcept {
    return (dialects & (NoteDialect::Midi | NoteDialect::MidiMpe)) != 0;
}

// An insertion sort by offset: stable, nothing to do when it's in order, and
// no allocation.
void sortByOffset(Event* events, std::uint32_t count) noexcept {
    for (std::uint32_t k = 1; k < count; ++k)
        for (std::uint32_t j = k; j > 0 && events[j].sampleOffset < events[j - 1].sampleOffset; --j)
            std::swap(events[j], events[j - 1]);
}

} // namespace

bool dialectAccepts(NoteDialectMask dialects, const MidiMessage& m) noexcept {
    switch (ump::messageType(m)) {
        case ump::MessageType::Midi1ChannelVoice: return takesMidi1(dialects);
        case ump::MessageType::Midi2ChannelVoice: return (dialects & NoteDialect::Midi2) != 0;
        case ump::MessageType::System:
            return (dialects & (NoteDialect::Midi | NoteDialect::MidiMpe | NoteDialect::Midi2)) != 0;
        default: return false;
    }
}

std::size_t translateFor(NoteDialectMask dialects, const MidiMessage& m, ump::UpconvertState& upconvert,
                         MidiMessage* out) noexcept {
    const ump::MessageType type = ump::messageType(m);
    if (dialectAccepts(dialects, m)) {
        out[0] = m;
        return 1;
    }
    if (type == ump::MessageType::Midi2ChannelVoice && (dialects & NoteDialect::Midi2) == 0)
        return ump::downconvert(m, out, ump::kMaxTranslatedMessages);
    if (type == ump::MessageType::Midi1ChannelVoice && (dialects & NoteDialect::Midi2) != 0)
        return ump::upconvert(m, upconvert, out, ump::kMaxTranslatedMessages);
    out[0] = m;
    return 1;
}

void NoteEffectChain::prepare(std::vector<Stage> stageList, NoteDialectMask dialects, std::uint16_t port,
                              std::uint32_t frames, std::uint32_t maxEvents) {
    stages              = std::move(stageList);
    destinationDialects = dialects;
    destinationPort     = port;
    destinationUpconvert = {};
    maxFrames           = frames;
    states.assign(stages.size(), StageState{});
    for (std::size_t i = 0; i < stages.size(); ++i) {
        StageState&      s      = states[i];
        const PortLayout layout = stages[i].plugin->portLayout();
        s.dialects              = layout.noteInputs.empty() ? 0 : layout.noteInputs[0].supportedDialects;
        const int main          = mainAudioOutput(layout);
        s.audioChannels = main >= 0 ? layout.audioOutputs[static_cast<std::size_t>(main)].channelCount : 0;
        s.audio.assign(static_cast<std::size_t>(s.audioChannels) * frames, 0.0f);
        s.channels.resize(s.audioChannels);
        for (std::uint32_t c = 0; c < s.audioChannels; ++c) s.channels[c] = s.audio.data() + c * frames;
    }
    for (auto& c : chain) c.assign(maxEvents, Event{});
    head.assign(maxEvents, Event{});
    merged.assign(maxEvents * 2, Event{});
}

bool NoteEffectChain::contains(const HostedPlugin* plugin) const noexcept {
    return std::any_of(stages.begin(), stages.end(), [plugin](const Stage& s) { return s.plugin == plugin; });
}

bool NoteEffectChain::passesThrough() const noexcept {
    return std::all_of(stages.begin(), stages.end(), [](const Stage& s) { return s.bypassed; });
}

// The chain's input as the first running stage's note port takes it.
EventBlock NoteEffectChain::translateInput(EventBlock input, std::size_t first) noexcept {
    StageState&         s   = states[first];
    const std::uint32_t cap = static_cast<std::uint32_t>(head.size());
    std::uint32_t       n   = 0;
    MidiMessage         translated[ump::kMaxTranslatedMessages];
    for (std::uint32_t i = 0; i < input.count; ++i) {
        const Event& e = input.events[i];
        if (e.type != EventType::Midi || s.dialects == 0) continue;
        const std::size_t count = translateFor(s.dialects, e.midi, s.upconvert, translated);
        for (std::size_t k = 0; k < count; ++k) {
            if (n == cap) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            Event& out       = head[n++];
            out              = e;
            out.midi         = translated[k];
            out.portIndex    = 0;
            out.sampleOffset = std::min(e.sampleOffset, maxFrames > 0 ? maxFrames - 1 : 0u);
        }
    }
    return EventBlock{head.data(), n};
}

// Two time-sorted lists into `into`, the MIDI first at the same offset.
EventBlock NoteEffectChain::merge(EventBlock midi, EventBlock params, std::vector<Event>& into) noexcept {
    if (params.count == 0) return midi;
    if (midi.count == 0) return params;
    Event* const        out = into.data();
    const std::uint32_t cap = static_cast<std::uint32_t>(into.size());
    std::uint32_t       n = 0, i = 0, j = 0;
    while (i < midi.count || j < params.count) {
        const bool fromMidi = j == params.count || (i < midi.count && midi.events[i].sampleOffset <= params.events[j].sampleOffset);
        const Event& e      = fromMidi ? midi.events[i++] : params.events[j++];
        if (n < cap) out[n++] = e;
        else dropped.fetch_add(1, std::memory_order_relaxed);
    }
    return EventBlock{out, n};
}

EventBlock NoteEffectChain::run(std::uint32_t frames, const TimeInfo& time, EventBlock input,
                                const EventBlock* params, NoteEffectFeedback* feedback) noexcept {
    const std::size_t count = stages.size();
    if (count == 0) return input;

    std::size_t first = 0;
    while (first < count && stages[first].bypassed) ++first;

    EventBlock  previous = first < count ? translateInput(input, first) : input;
    std::size_t written  = 0;   // chain buffers used: the next stage's output goes in the other
    for (std::size_t e = 0; e < count; ++e) {
        StageState&      s = states[e];
        const EventBlock own = params != nullptr ? params[e] : EventBlock{};

        ProcessContext pc;
        pc.frameCount = frames;
        pc.time       = time;
        pc.audioOut   = AudioBuffer{s.channels.data(), s.audioChannels, frames};

        sink.chain     = this;
        sink.plugin    = stages[e].plugin;
        sink.feedback  = feedback;
        sink.count     = 0;
        sink.frames    = frames;
        sink.upconvert = nullptr;
        if (stages[e].bypassed) {   // its own parameter changes only; the MIDI goes around it
            sink.events   = nullptr;
            sink.capacity = 0;
            sink.dialects = 0;
            pc.eventsIn   = own;
            pc.eventsOut  = &sink;
            stages[e].plugin->process(pc);
            continue;
        }

        std::size_t next = e + 1;   // where its output goes: the next stage running, or the destination
        while (next < count && stages[next].bypassed) ++next;
        const bool          last = next == count;
        std::vector<Event>& out  = chain[written++ % 2];
        sink.events    = out.data();
        sink.capacity  = static_cast<std::uint32_t>(out.size());
        sink.dialects  = last ? destinationDialects : states[next].dialects;
        sink.port      = last ? destinationPort : 0;
        sink.upconvert = last ? &destinationUpconvert : &states[next].upconvert;
        pc.eventsIn    = s.dialects != 0 ? merge(previous, own, merged) : own;
        pc.eventsOut   = &sink;
        stages[e].plugin->process(pc);

        // CLAP asks a plugin for its output in time order; keep it so if one doesn't.
        sortByOffset(out.data(), sink.count);
        previous = EventBlock{out.data(), sink.count};
    }
    sink.plugin = nullptr;
    return previous;
}

void NoteEffectChain::Sink::push(const Event& event) noexcept {
    if (event.type != EventType::Midi) {
        if (feedback != nullptr) feedback->effectEvent(plugin, event);
        return;
    }
    if (dialects == 0 || upconvert == nullptr) return;
    const std::uint32_t offset = std::min(event.sampleOffset, frames > 0 ? frames - 1 : 0u);
    MidiMessage         translated[ump::kMaxTranslatedMessages];
    const std::size_t   n = translateFor(dialects, event.midi, *upconvert, translated);
    for (std::size_t k = 0; k < n; ++k) add(translated[k], offset);
}

void NoteEffectChain::Sink::add(const MidiMessage& message, std::uint32_t offset) noexcept {
    if (count == capacity) {
        chain->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Event& e       = events[count++];
    e              = Event{};
    e.sampleOffset = offset;
    e.type         = EventType::Midi;
    e.midi         = message;
    e.portIndex    = port;
}

} // namespace uw
