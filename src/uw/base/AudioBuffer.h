#pragma once

#include <cstdint>

namespace uw {

// A non-owning view of one block of non-interleaved 32-bit float audio:
// `channelCount` channels, each `frameCount` samples long. Backend-neutral —
// JACK per-port pointers and CoreAudio's AudioBufferList both adapt into this.
// The pointer array is const; the samples it points at are writable.
//
// N-channel by construction: nothing here assumes stereo, and nothing
// downstream may either (see StageHand's docs/architecture.md 3b, pro-hardware scaling).
struct AudioBuffer {
    float* const* channels     = nullptr;  // channels[c][f], c < channelCount
    std::uint32_t channelCount = 0;
    std::uint32_t frameCount   = 0;
};

// Read-only counterpart, for hardware capture and plugin audio inputs.
struct ConstAudioBuffer {
    const float* const* channels     = nullptr;
    std::uint32_t       channelCount = 0;
    std::uint32_t       frameCount   = 0;
};

} // namespace uw
