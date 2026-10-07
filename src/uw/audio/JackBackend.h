#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "uw/audio/AudioBackend.h"
#include "uw/audio/AuxRouting.h"

typedef struct _jack_client jack_client_t;
typedef struct _jack_port   jack_port_t;

namespace uw {

// AudioBackend over the JACK API. On this system the JACK API is served by
// PipeWire (pipewire-jack): run the app under `pw-jack` so calls reach PipeWire
// instead of looking for a jackd server.
//
// The server owns the sample rate: open() reports it rather than imposing the
// request. A requested buffer size (AudioConfig::bufferSize != 0) is FORCED on
// our graph while our client is active, via PipeWire's node.force-quantum, and
// released when the client goes away (StageHand's docs/architecture.md section 2). The
// buffer size can change while running, in both directions (a forced quantum
// applies at activation; other clients can change the graph); config() always
// reports the current value, and the engine never assumes a fixed block size.
class JackBackend final : public AudioBackend, public AuxRouting {
public:
    // `connectToSystem`: on start(), connect our ports to the system's physical
    // playback/capture ports. JACK-specific, so it lives here rather than in
    // AudioConfig (other backends output to their device directly).
    // `jackClientName`: the app's name, as JACK and PipeWire show it.
    explicit JackBackend(std::string jackClientName, bool connectToSystem = true);
    ~JackBackend() override;

    JackBackend(const JackBackend&)            = delete;
    JackBackend& operator=(const JackBackend&) = delete;

    // Is a JACK server reachable from this process (without starting one)?
    // Opens and closes a throwaway client, with libjack's own messages silenced.
    static bool serverAvailable();

    bool        open(const AudioConfig& requested, AudioConfig& negotiated) override;
    void        setProcessor(AudioProcessor* audioProcessor) override;
    bool        start() override;
    void        stop() override;
    void        close() override;
    AudioConfig config() const override;
    double      outputLatencySeconds() const override;   // the main outputs' playback latency
    const char* name() const override { return "JACK"; }

    // --- AuxRouting: physical playback ports, grouped by device (client name).
    std::vector<PlaybackDevice> playbackDevices() override;
    bool                        connectAux(const PlaybackDevice& device) override;
    void                        disconnectAux() override;
    bool                        takeDevicesChanged() override;
    std::string                 mainDevice() override;

    // --- diagnostics (any thread) ---
    std::uint64_t xrunCount() const noexcept { return xruns.load(std::memory_order_relaxed); }
    bool          serverShutDown() const noexcept { return shutDown.load(std::memory_order_relaxed); }
    std::int64_t  framesProcessed() const noexcept { return frameCounter.load(std::memory_order_relaxed); }

    // The quantum being forced while our client is active, or 0 if none (not
    // requested, or a user-set PIPEWIRE_PROPS took precedence).
    std::uint32_t forcedQuantum() const noexcept { return appliedQuantum; }

    // JACK clock (jack_get_time) minus CLOCK_MONOTONIC, measured at open(). ~0
    // means JACK cycle times and our MIDI capture timestamps share a clock,
    // which sample-accurate MIDI depends on. blockStartTime is corrected by
    // this offset either way.
    Nanoseconds clockOffset() const noexcept { return jackClockOffset; }

private:
    static int  processCallback(std::uint32_t nframes, void* arg);
    static int  bufferSizeCallback(std::uint32_t nframes, void* arg);
    static int  xrunCallback(void* arg);
    static void shutdownCallback(void* arg);
    static void portRegistrationCallback(std::uint32_t port, int registered, void* arg);

    void connectToSystemPorts();

    std::string clientName;
    bool        autoConnect;

    jack_client_t*            client = nullptr;
    std::vector<jack_port_t*> inputPorts;
    std::vector<jack_port_t*> outputPorts;
    std::vector<jack_port_t*> auxPorts;

    // Sized at open() so the RT callback never allocates.
    std::vector<const float*> inputPointers;
    std::vector<float*>       outputPointers;
    std::vector<float*>       auxPointers;

    AudioProcessor* processor = nullptr;
    AudioConfig     negotiatedConfig;
    Nanoseconds     jackClockOffset = 0;
    std::uint32_t   appliedQuantum  = 0;
    bool            running         = false;

    std::atomic<std::uint32_t> currentBufferSize{0};
    std::atomic<std::int64_t>  frameCounter{0};     // written only by the RT thread
    std::atomic<std::uint64_t> xruns{0};
    std::atomic<bool>          shutDown{false};
    std::atomic<bool>          devicesChanged{false};   // set by JACK's notification thread
};

} // namespace uw
