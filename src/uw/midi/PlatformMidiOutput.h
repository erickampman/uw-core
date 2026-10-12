#pragma once

// The MIDI output this platform's app uses. Both have the same create() and
// describe(), beyond the MidiOutput seam. UW_HAVE_MIDI_OUTPUT is 1 on both
// (it was 0 on Linux until AlsaSeqOutput).
#ifdef __APPLE__
#include "uw/midi/CoreMidiOutput.h"
namespace uw {
using PlatformMidiOutput = CoreMidiOutput;
}
#else
#include "uw/midi/AlsaSeqOutput.h"
namespace uw {
using PlatformMidiOutput = AlsaSeqOutput;
}
#endif
#define UW_HAVE_MIDI_OUTPUT 1
