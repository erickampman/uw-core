#pragma once

#include <cstdint>

namespace uw {

// Musical transport state for one process block (or chunk of one). The backend
// fills the sample fields; the engine fills the musical ones from its beat clock
// (StageHand's docs/sequences.md, section 1). Maps cleanly onto CLAP's transport event and
// VST3's ProcessContext (see StageHand's docs/architecture.md section 3a — this is one of the
// clean matches). Beats are quarter notes: BPM counts them.
struct TimeInfo {
    double       sampleRate     = 0.0;    // frames per second for this block
    std::int64_t samplePosition = 0;      // frames elapsed since transport start

    double tempoBpm            = 120.0;   // beats per minute
    int    timeSigNumerator    = 4;
    int    timeSigDenominator  = 4;
    bool   playing             = false;   // is the transport running?

    double       beatPosition  = 0.0;     // beats since the transport started, at the block's first frame
    double       barStartBeats = 0.0;     // where the bar containing beatPosition began
    std::int32_t barNumber     = 0;       // that bar, counting from 0

    // Loop region in frames; meaningful only when `looping` is true.
    bool         looping   = false;
    std::int64_t loopStart = 0;
    std::int64_t loopEnd   = 0;
};

// What the beat lights and the click count in a bar (StageHand's docs/sequences.md,
// section 7). Counted beats: a compound meter (6/8, 9/8, 12/8, and the same
// over 16) counts dotted beats of three notes; any other meter counts its
// notes (7/8: seven). Every note: always the numerator (6/8: six).
struct MetronomePulse {
    int    perBar = 4;     // pulses in a bar
    double beats  = 1.0;   // one pulse's length, in quarter notes
};
constexpr MetronomePulse metronomePulse(int numerator, int denominator, bool everyNote) noexcept {
    if (numerator < 1 || denominator < 1) return {};
    const bool compound = !everyNote && denominator >= 8 && numerator > 3 && numerator % 3 == 0;
    const int  notes    = compound ? 3 : 1;
    return MetronomePulse{numerator / notes, notes * 4.0 / denominator};
}

} // namespace uw
