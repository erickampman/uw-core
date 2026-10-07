#pragma once

#include <cstdint>

#include "uw/base/AudioBuffer.h"
#include "uw/base/TimeInfo.h"
#include "uw/Types.h"

namespace uw {

// Requested and negotiated audio stream configuration. A 0 for `sampleRate` or
// `bufferSize` means "device default"; the backend reports what the hardware
// actually granted through AudioBackend::open()'s `negotiated` out-parameter.
struct AudioConfig {
    double        sampleRate     = 0.0;
    std::uint32_t bufferSize     = 0;   // frames per block; may change while running (see config())
    std::uint32_t inputChannels  = 0;
    std::uint32_t outputChannels = 2;
    // Outputs of their own (the click), never mixed into the main outputs and
    // never connected automatically: see AuxRouting.
    std::uint32_t auxOutputChannels = 0;
};

// Everything handed to the real-time callback for one block. `blockStartTime`
// is the time this processing cycle began, on nowNanoseconds()'s clock
// (Core/Clock.h), the one MIDI is timestamped by. The engine maps MIDI
// captured during the previous cycle onto this block, one period behind, so
// relative timing stays sample-exact (domain-model.md, sample-accurate
// alignment). Every backend keeps this convention (StageHand's docs/macos.md, decision 4;
// testCaptureToBlock): JACK's cycle start; on CoreAudio the IOProc's "now",
// not the output's time, which is later by the buffer and the device's
// latency and would put every message a block late.
struct AudioCallbackContext {
    std::uint32_t    frameCount     = 0;
    Nanoseconds      blockStartTime = 0;
    TimeInfo         time;
    ConstAudioBuffer input;    // hardware inputs; channelCount may be 0
    AudioBuffer      output;   // hardware outputs to fill
    AudioBuffer      aux;      // auxiliary outputs to fill (channelCount may be 0)
};

// Implemented by the engine; invoked by the backend on the real-time audio
// thread once per block.
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    // Render one block. Runs on the real-time audio thread: no locks, no
    // allocation, no syscalls, no blocking (see domain-model.md, threading).
    virtual void process(const AudioCallbackContext& ctx) noexcept = 0;
};

// The audio seam. The ONLY component that knows about JACK / PipeWire /
// CoreAudio / ALSA. It owns the real-time thread, drives the AudioProcessor,
// and is the clock source for the whole app. Initial impl: JackBackend (via
// pipewire-jack). An exclusive low-latency backend can be added later purely as
// a new implementation, no engine change (StageHand's docs/architecture.md 3b).
//
// Lifecycle: open() -> setProcessor() -> start() -> [RT callbacks] -> stop() ->
// close(). Every method is called on the main/control thread; only
// AudioProcessor::process() runs on the RT thread.
//
// Errors are reported as a bool for now; richer status/error reporting is
// deferred until a concrete backend needs it.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    // Open the device. `requested` may contain 0s meaning "default"; the config
    // actually granted is written to `negotiated`. Returns false on failure.
    virtual bool open(const AudioConfig& requested, AudioConfig& negotiated) = 0;

    // Register the processor invoked each block. Must be set before start().
    virtual void setProcessor(AudioProcessor* processor) = 0;

    // Begin / end real-time callbacks.
    virtual bool start() = 0;
    virtual void stop()  = 0;

    // Release the device. Safe after stop(); close() implies stop().
    virtual void close() = 0;

    // The negotiated configuration (valid between open() and close()).
    virtual AudioConfig config() const = 0;

    // From a block's start (its period) to its sound reaching the outputs,
    // in seconds: the device's and the server's buffering. 0 if unknown. For
    // drawing the beat with the sound (StageHand's docs/perform.md, decision 14).
    virtual double outputLatencySeconds() const { return 0.0; }

    // Human-readable backend name, e.g. "JACK". For UI and logging.
    virtual const char* name() const = 0;
};

} // namespace uw
