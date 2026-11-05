// uw::clap-host against the CLAP plugins installed here (~/.clap, $CLAP_PATH):
// create an instrument, activate it, and process one block with a MIDI 2.0
// note-on into memory; and a note effect, whose notes must come out of
// process() as MIDI events. Nothing reaches an audio device. Each part skips if
// no such plugin is found.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Check.h"
#include "uw/clap/ClapPluginFactory.h"
#include "uw/plugin/HostServices.h"
#include "uw/plugin/HostedPlugin.h"

namespace {

struct TestHost final : uw::HostServices {
    void         requestRestart() override {}
    void         requestProcess() override {}
    void         requestParamFlush() override {}
    void         log(uw::LogLevel, const char*) override {}
    uw::HostInfo hostInfo() const override { return {"uw-core tests", "Unlikelyware", "", "0.1.0"}; }
};

uw::MidiMessage midi2NoteOn(std::uint8_t note, std::uint16_t velocity) {
    uw::MidiMessage m;
    m.words[0]  = (0x4u << 28) | (0x9u << 20) | (static_cast<std::uint32_t>(note) << 8);
    m.words[1]  = static_cast<std::uint32_t>(velocity) << 16;
    m.wordCount = 2;
    return m;
}

void unknownFile() {
    uw::ClapPluginFactory factory;
    CHECK(factory.scanFile("/nonexistent/none.clap").empty());
}

void instrument() {
    uw::ClapPluginFactory factory;
    const auto            found = factory.scan();
    const auto it = std::find_if(found.begin(), found.end(),
                                 [](const uw::PluginDescriptor& d) { return d.kind == uw::PluginKind::Instrument; });
    if (it == found.end()) {
        std::printf("clap-host: no CLAP instrument installed; skipped\n");
        return;
    }
    std::printf("clap-host: %zu plugin(s) found; trying %s\n", found.size(), it->name.c_str());

    TestHost   host;
    const auto plugin = factory.create(*it, host);
    CHECK(plugin != nullptr);
    if (!plugin) return;
    CHECK(plugin->descriptor().id == it->id);

    constexpr std::uint32_t kFrames = 256;
    CHECK(plugin->activate(48000.0, kFrames));

    std::vector<std::vector<float>> channels(2, std::vector<float>(kFrames, 0.0f));
    float*                          pointers[2] = {channels[0].data(), channels[1].data()};
    uw::Event                       note;
    note.type = uw::EventType::Midi;
    note.midi = midi2NoteOn(60, 0xC000);

    uw::ProcessContext ctx;
    ctx.frameCount         = kFrames;
    ctx.time.sampleRate    = 48000.0;
    ctx.audioOut           = uw::AudioBuffer{pointers, 2, kFrames};
    ctx.eventsIn           = uw::EventBlock{&note, 1};
    for (int block = 0; block < 4; ++block) {
        plugin->process(ctx);
        ctx.eventsIn = {};
    }
    bool finite = true;
    for (const auto& ch : channels)
        for (float v : ch) finite = finite && std::isfinite(v);
    CHECK(finite);

    plugin->stopProcessing();
    plugin->deactivate();
}

struct Collect final : uw::EventSink {
    std::vector<uw::Event> events;
    void push(const uw::Event& e) noexcept override { events.push_back(e); }   // allocates: fine in a test
};

// A note effect (Retune, if it's installed: a known pass-through at its
// defaults) hands its output notes to the sink, in their own protocol.
void noteEffect() {
    uw::ClapPluginFactory factory;
    const auto            found = factory.scan();
    auto it = std::find_if(found.begin(), found.end(), [](const uw::PluginDescriptor& d) { return d.name == "Retune"; });
    if (it == found.end())
        it = std::find_if(found.begin(), found.end(), [](const uw::PluginDescriptor& d) { return d.kind == uw::PluginKind::Other; });
    if (it == found.end()) {
        std::printf("clap-host: no CLAP note effect installed; skipped\n");
        return;
    }
    std::printf("clap-host: note effect %s\n", it->name.c_str());

    TestHost   host;
    const auto plugin = factory.create(*it, host);
    CHECK(plugin != nullptr);
    if (!plugin) return;
    const uw::PortLayout layout = plugin->portLayout();
    CHECK(!layout.noteInputs.empty() && !layout.noteOutputs.empty());
    constexpr std::uint32_t kFrames = 256;
    CHECK(plugin->activate(48000.0, kFrames));

    uw::Event note;
    note.type         = uw::EventType::Midi;
    note.sampleOffset = 17;
    note.midi         = midi2NoteOn(60, 0xC000);
    Collect            out;
    uw::ProcessContext ctx;
    ctx.frameCount      = kFrames;
    ctx.time.sampleRate = 48000.0;
    ctx.eventsIn        = uw::EventBlock{&note, 1};
    ctx.eventsOut       = &out;
    plugin->process(ctx);
    const auto midi = std::count_if(out.events.begin(), out.events.end(),
                                    [](const uw::Event& e) { return e.type == uw::EventType::Midi; });
    CHECK(midi >= 1);
    if (it->name == "Retune") {   // its parameters, with the plugin's own value text
        // Retune appends parameters and never moves them (Dry came fourth), so only the first
        // three are fixed.
        const auto params = plugin->parameters();
        CHECK(params.size() >= 3 && params[0].name == "Divisions" && params[1].name == "Octaves" &&
              params[2].name == "Steps" && params[0].isStepped && !params[0].isHidden && !params[0].isReadOnly);
        std::string text;
        CHECK(plugin->paramValueText(0, 31.0, text) && text == "31");
    }
    if (it->name == "Retune") {   // at its defaults, the same note comes out, as MIDI 2.0, at the same time
        CHECK(midi == 1);
        const auto on = std::find_if(out.events.begin(), out.events.end(),
                                     [](const uw::Event& e) { return e.type == uw::EventType::Midi; });
        CHECK(on != out.events.end() && on->midi.wordCount == 2 && on->sampleOffset == 17 &&
              (on->midi.words[0] >> 28) == 0x4 && ((on->midi.words[0] >> 8) & 0x7F) == 60 &&
              (on->midi.words[1] >> 16) == 0xC000);
    }

    plugin->stopProcessing();
    plugin->deactivate();
}

} // namespace

int main() {
    unknownFile();
    instrument();
    noteEffect();
    return uw::test::finish("clap-host");
}
