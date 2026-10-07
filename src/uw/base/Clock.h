#pragma once

#include <cstdint>

#include "uw/Types.h"

namespace uw {

// The one clock MIDI timestamps and audio block times share (StageHand's docs/macos.md,
// decision 4): CLOCK_MONOTONIC on Linux; on macOS the host time
// (mach_absolute_time, which CoreMIDI and CoreAudio report), in nanoseconds.
// Real-time safe: no allocation, no lock, no system call on either.
Nanoseconds nowNanoseconds() noexcept;

#ifdef __APPLE__
// A host time (mach_absolute_time's units, as in a MIDIEventPacket or an
// AudioTimeStamp) on nowNanoseconds()'s scale.
Nanoseconds hostTimeToNanoseconds(std::uint64_t hostTime) noexcept;
#endif

} // namespace uw
