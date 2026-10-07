#pragma once

#include <string>
#include <vector>

namespace uw {

// A playback device the audio server offers: its name as the server shows it
// (PipeWire's description, e.g. "Built-in Audio Analog Stereo") and its
// playback ports in order (FL, FR, ...).
struct PlaybackDevice {
    std::string              name;
    std::vector<std::string> ports;
};

// Where the auxiliary outputs go: the click's own pair (AudioConfig::
// auxOutputChannels), which is never mixed into the main outputs or connected
// automatically (StageHand's docs/sequences.md, section 7). Implemented by the backend; no
// audio-server types. Main thread.
class AuxRouting {
public:
    virtual ~AuxRouting() = default;

    virtual std::vector<PlaybackDevice> playbackDevices() = 0;
    // Connect the aux outputs to `device` (aux 1 -> its first port, aux 2 ->
    // its second, or its first again if it has one), replacing any earlier
    // connection. False if it couldn't.
    virtual bool connectAux(const PlaybackDevice& device) = 0;
    virtual void disconnectAux() = 0;
    // True once after devices came or went (hot-plug), so connections can be
    // tried again.
    virtual bool takeDevicesChanged() = 0;
    // The device the main outputs are connected to ("" if none): the click
    // never goes there unless chosen.
    virtual std::string mainDevice() = 0;
};

} // namespace uw
