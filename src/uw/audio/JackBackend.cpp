#include "uw/audio/JackBackend.h"

#include <jack/jack.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <utility>

#include "uw/base/Clock.h"

namespace uw {

namespace {

void silentJackMessage(const char*) {}
void printJackMessage(const char* message) {   // what libjack does by default
    std::fprintf(stderr, "%s\n", message);
}

// The PIPEWIRE_PROPS value this process set to force a quantum, if any. It stays
// set after jack_client_open: clearing it while PipeWire's threads are running
// could race with their getenv(). The next open() — when no client is running —
// replaces or clears it.
std::string& ownPipewireProps() {
    static std::string value;
    return value;
}

// Force (quantum > 0) or stop forcing (quantum == 0) the PipeWire quantum for
// the client about to be opened. Sets only node.force-quantum; PIPEWIRE_QUANTUM
// would force the sample rate as well. A PIPEWIRE_PROPS the user set in the
// environment wins. Returns false if a requested force could not be applied.
bool prepareQuantumForce(std::uint32_t quantum) {
    const char* existing = std::getenv("PIPEWIRE_PROPS");
    const bool  ours     = existing != nullptr && ownPipewireProps() == existing;
    if (existing != nullptr && !ours) return quantum == 0;

    if (quantum == 0) {
        if (ours) unsetenv("PIPEWIRE_PROPS");
        ownPipewireProps().clear();
        return true;
    }
    ownPipewireProps() = "{ node.force-quantum = " + std::to_string(quantum) + " }";
    setenv("PIPEWIRE_PROPS", ownPipewireProps().c_str(), 1);
    return true;
}

} // namespace

JackBackend::JackBackend(std::string jackClientName, bool connectToSystem)
    : clientName(std::move(jackClientName)), autoConnect(connectToSystem) {}

JackBackend::~JackBackend() {
    close();
}

bool JackBackend::serverAvailable() {
    jack_set_error_function(&silentJackMessage);
    jack_set_info_function(&silentJackMessage);
    jack_status_t        status{};
    jack_client_t* const probe = jack_client_open("uw-core-probe", JackNoStartServer, &status);
    if (probe != nullptr) jack_client_close(probe);
    jack_set_error_function(&printJackMessage);   // as before (there's no getter for the old ones)
    jack_set_info_function(&printJackMessage);
    return probe != nullptr;
}

bool JackBackend::open(const AudioConfig& requested, AudioConfig& negotiated) {
    if (client != nullptr) return false;   // already open

    // A requested buffer size is forced on our graph for as long as our client
    // is active (PipeWire node.force-quantum; StageHand's docs/architecture.md section 2).
    // PipeWire releases it when the client goes away, crash included. It takes
    // effect at activation, so the size reported below is still the old one;
    // the buffer-size callback reports the change. A real jackd ignores it.
    if (prepareQuantumForce(requested.bufferSize)) {
        appliedQuantum = requested.bufferSize;
    } else {
        appliedQuantum = 0;
        std::fprintf(stderr,
                     "JackBackend: PIPEWIRE_PROPS is already set in the environment; "
                     "not forcing a %u-frame quantum\n",
                     requested.bufferSize);
    }

    jack_status_t status{};
    client = jack_client_open(clientName.c_str(), JackNoStartServer, &status);
    if (client == nullptr) {
        std::fprintf(stderr,
                     "JackBackend: jack_client_open failed (status 0x%x). "
                     "Is it running under PipeWire's JACK layer (pw-jack)?\n",
                     static_cast<unsigned>(status));
        return false;
    }

    jack_set_process_callback(client, &JackBackend::processCallback, this);
    jack_set_buffer_size_callback(client, &JackBackend::bufferSizeCallback, this);
    jack_set_xrun_callback(client, &JackBackend::xrunCallback, this);
    jack_on_shutdown(client, &JackBackend::shutdownCallback, this);
    jack_set_port_registration_callback(client, &JackBackend::portRegistrationCallback, this);

    auto registerPorts = [this](std::uint32_t count, const char* prefix,
                                unsigned long flags, std::vector<jack_port_t*>& ports) {
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::string portName = prefix + std::to_string(i + 1);
            jack_port_t* port = jack_port_register(client, portName.c_str(),
                                                   JACK_DEFAULT_AUDIO_TYPE, flags, 0);
            if (port == nullptr) return false;
            ports.push_back(port);
        }
        return true;
    };
    if (!registerPorts(requested.inputChannels, "in_", JackPortIsInput, inputPorts) ||
        !registerPorts(requested.outputChannels, "out_", JackPortIsOutput, outputPorts)) {
        std::fprintf(stderr, "JackBackend: port registration failed\n");
        close();
        return false;
    }
    // Aux (the click): "click_L"/"click_R" for a pair.
    for (std::uint32_t i = 0; i < requested.auxOutputChannels; ++i) {
        const std::string portName = requested.auxOutputChannels == 2 ? (i == 0 ? "click_L" : "click_R")
                                                                      : "click_" + std::to_string(i + 1);
        jack_port_t* port = jack_port_register(client, portName.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (port == nullptr) break;
        auxPorts.push_back(port);
    }
    if (auxPorts.size() != requested.auxOutputChannels) {
        std::fprintf(stderr, "JackBackend: port registration failed\n");
        close();
        return false;
    }
    inputPointers.assign(inputPorts.size(), nullptr);
    outputPointers.assign(outputPorts.size(), nullptr);
    auxPointers.assign(auxPorts.size(), nullptr);

    // The server owns rate and buffer size: report what it runs, not the request.
    negotiatedConfig.sampleRate     = jack_get_sample_rate(client);
    negotiatedConfig.bufferSize     = jack_get_buffer_size(client);
    negotiatedConfig.inputChannels  = static_cast<std::uint32_t>(inputPorts.size());
    negotiatedConfig.outputChannels = static_cast<std::uint32_t>(outputPorts.size());
    negotiatedConfig.auxOutputChannels = static_cast<std::uint32_t>(auxPorts.size());
    currentBufferSize.store(negotiatedConfig.bufferSize, std::memory_order_relaxed);

    if (requested.sampleRate != 0.0 && requested.sampleRate != negotiatedConfig.sampleRate)
        std::fprintf(stderr, "JackBackend: requested %.0f Hz, server runs %.0f Hz\n",
                     requested.sampleRate, negotiatedConfig.sampleRate);

    // Measure JACK's clock against nowNanoseconds() (CLOCK_MONOTONIC), bracketing the read.
    const Nanoseconds before    = nowNanoseconds();
    const jack_time_t jackUsecs = jack_get_time();
    const Nanoseconds after     = nowNanoseconds();
    jackClockOffset = static_cast<Nanoseconds>(jackUsecs) * 1000 - (before + after) / 2;

    negotiated = negotiatedConfig;
    return true;
}

void JackBackend::setProcessor(AudioProcessor* audioProcessor) {
    if (running) {
        std::fprintf(stderr, "JackBackend: setProcessor() ignored while running\n");
        return;
    }
    processor = audioProcessor;
}

// JACK's playback latency of our first output: from a period's start to its
// frames reaching the hardware (the later of its range).
double JackBackend::outputLatencySeconds() const {
    if (client == nullptr || outputPorts.empty() || negotiatedConfig.sampleRate <= 0.0) return 0.0;
    jack_latency_range_t range{};
    jack_port_get_latency_range(outputPorts.front(), JackPlaybackLatency, &range);
    return static_cast<double>(range.max) / negotiatedConfig.sampleRate;
}

bool JackBackend::start() {
    if (client == nullptr || running) return false;
    if (jack_activate(client) != 0) {
        std::fprintf(stderr, "JackBackend: jack_activate failed\n");
        return false;
    }
    running = true;
    if (autoConnect) connectToSystemPorts();
    return true;
}

void JackBackend::stop() {
    if (!running) return;
    jack_deactivate(client);
    running = false;
}

void JackBackend::close() {
    stop();
    if (client != nullptr) {
        jack_client_close(client);   // also unregisters our ports
        client = nullptr;
    }
    inputPorts.clear();
    outputPorts.clear();
    auxPorts.clear();
    inputPointers.clear();
    outputPointers.clear();
    auxPointers.clear();
}

AudioConfig JackBackend::config() const {
    AudioConfig current = negotiatedConfig;
    current.bufferSize  = currentBufferSize.load(std::memory_order_relaxed);
    return current;
}

// --- AuxRouting ------------------------------------------------------------------

std::vector<PlaybackDevice> JackBackend::playbackDevices() {
    std::vector<PlaybackDevice> devices;
    if (client == nullptr) return devices;
    const char** ports = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, JackPortIsPhysical | JackPortIsInput);
    if (ports == nullptr) return devices;
    for (std::size_t i = 0; ports[i] != nullptr; ++i) {
        const std::string full  = ports[i];
        const std::size_t colon = full.find(':');
        const std::string name  = full.substr(0, colon);
        auto it = std::find_if(devices.begin(), devices.end(), [&](const PlaybackDevice& d) { return d.name == name; });
        if (it == devices.end()) it = devices.insert(devices.end(), PlaybackDevice{name, {}});
        it->ports.push_back(full);
    }
    jack_free(ports);
    return devices;
}

bool JackBackend::connectAux(const PlaybackDevice& device) {
    if (client == nullptr || !running || device.ports.empty()) return false;
    disconnectAux();
    bool ok = true;
    for (std::size_t i = 0; i < auxPorts.size(); ++i) {
        const std::string& to = device.ports[std::min(i, device.ports.size() - 1)];
        ok = jack_connect(client, jack_port_name(auxPorts[i]), to.c_str()) == 0 && ok;
    }
    return ok;
}

void JackBackend::disconnectAux() {
    if (client == nullptr) return;
    for (jack_port_t* port : auxPorts) jack_port_disconnect(client, port);
}

bool JackBackend::takeDevicesChanged() {
    return devicesChanged.exchange(false, std::memory_order_relaxed);
}

std::string JackBackend::mainDevice() {
    if (client == nullptr || outputPorts.empty()) return {};
    std::string device;
    if (const char** to = jack_port_get_all_connections(client, outputPorts[0])) {
        if (to[0] != nullptr) device = std::string(to[0]).substr(0, std::string(to[0]).find(':'));
        jack_free(to);
    }
    return device;
}

void JackBackend::portRegistrationCallback(std::uint32_t, int, void* arg) {
    static_cast<JackBackend*>(arg)->devicesChanged.store(true, std::memory_order_relaxed);
}

void JackBackend::connectToSystemPorts() {
    // Our out_N -> the Nth physical playback port.
    if (const char** playback = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                               JackPortIsPhysical | JackPortIsInput)) {
        for (std::size_t i = 0; i < outputPorts.size() && playback[i] != nullptr; ++i) {
            const char* ours = jack_port_name(outputPorts[i]);
            if (jack_connect(client, ours, playback[i]) == 0)
                std::printf("  %s -> %s\n", ours, playback[i]);
            else
                std::fprintf(stderr, "JackBackend: could not connect %s -> %s\n", ours, playback[i]);
        }
        jack_free(playback);
    }
    // The Nth physical capture port -> our in_N.
    if (const char** capture = jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
                                              JackPortIsPhysical | JackPortIsOutput)) {
        for (std::size_t i = 0; i < inputPorts.size() && capture[i] != nullptr; ++i) {
            const char* ours = jack_port_name(inputPorts[i]);
            if (jack_connect(client, capture[i], ours) == 0)
                std::printf("  %s -> %s\n", capture[i], ours);
            else
                std::fprintf(stderr, "JackBackend: could not connect %s -> %s\n", capture[i], ours);
        }
        jack_free(capture);
    }
}

// --- JACK callbacks ------------------------------------------------------------

// RT thread. No allocation, locks, or syscalls beyond JACK's own RT-safe calls.
int JackBackend::processCallback(std::uint32_t nframes, void* arg) {
    auto* self = static_cast<JackBackend*>(arg);

    for (std::size_t i = 0; i < self->inputPorts.size(); ++i)
        self->inputPointers[i] =
            static_cast<const float*>(jack_port_get_buffer(self->inputPorts[i], nframes));
    for (std::size_t i = 0; i < self->outputPorts.size(); ++i)
        self->outputPointers[i] =
            static_cast<float*>(jack_port_get_buffer(self->outputPorts[i], nframes));
    for (std::size_t i = 0; i < self->auxPorts.size(); ++i)
        self->auxPointers[i] = static_cast<float*>(jack_port_get_buffer(self->auxPorts[i], nframes));

    const std::int64_t position = self->frameCounter.load(std::memory_order_relaxed);

    AudioCallbackContext ctx;
    ctx.frameCount = nframes;

    jack_nframes_t cycleFrames = 0;
    jack_time_t    cycleUsecs  = 0;
    jack_time_t    nextUsecs   = 0;
    float          periodUsecs = 0.0f;
    if (jack_get_cycle_times(self->client, &cycleFrames, &cycleUsecs, &nextUsecs,
                             &periodUsecs) == 0)
        ctx.blockStartTime = static_cast<Nanoseconds>(cycleUsecs) * 1000 - self->jackClockOffset;
    else
        ctx.blockStartTime = nowNanoseconds();   // vDSO clock read: no syscall, RT-safe

    ctx.time.sampleRate     = self->negotiatedConfig.sampleRate;
    ctx.time.samplePosition = position;

    ctx.input  = ConstAudioBuffer{self->inputPointers.data(),
                                  static_cast<std::uint32_t>(self->inputPointers.size()), nframes};
    ctx.output = AudioBuffer{self->outputPointers.data(),
                             static_cast<std::uint32_t>(self->outputPointers.size()), nframes};
    ctx.aux    = AudioBuffer{self->auxPointers.data(), static_cast<std::uint32_t>(self->auxPointers.size()), nframes};

    if (self->processor != nullptr) {
        self->processor->process(ctx);
    } else {
        for (float* channel : self->outputPointers)
            std::fill_n(channel, nframes, 0.0f);
        for (float* channel : self->auxPointers)
            std::fill_n(channel, nframes, 0.0f);
    }

    self->frameCounter.store(position + nframes, std::memory_order_relaxed);
    return 0;
}

int JackBackend::bufferSizeCallback(std::uint32_t nframes, void* arg) {
    static_cast<JackBackend*>(arg)->currentBufferSize.store(nframes, std::memory_order_relaxed);
    return 0;
}

int JackBackend::xrunCallback(void* arg) {
    static_cast<JackBackend*>(arg)->xruns.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

void JackBackend::shutdownCallback(void* arg) {
    static_cast<JackBackend*>(arg)->shutDown.store(true, std::memory_order_relaxed);
}

} // namespace uw
