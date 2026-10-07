// uw::clap-host against the CLAP plugins installed here (~/.clap, $CLAP_PATH):
// create an instrument, activate it, and process one block with a MIDI 2.0
// note-on into memory. Nothing reaches an audio device. Skips if none is found.

#include <algorithm>
#include <cmath>
#include <cstdio>
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

} // namespace

int main() {
    unknownFile();
    instrument();
    return uw::test::finish("clap-host");
}
