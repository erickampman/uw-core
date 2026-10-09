#pragma once

// The MIDI output this platform's app uses. Only CoreMIDI so far: the ALSA
// sequencer's is to come (KeyboardEDO's roadmap, milestone 3). Elsewhere
// UW_HAVE_MIDI_OUTPUT is 0 and there is no PlatformMidiOutput.
#ifdef __APPLE__
#include "uw/midi/CoreMidiOutput.h"
#define UW_HAVE_MIDI_OUTPUT 1
namespace uw {
using PlatformMidiOutput = CoreMidiOutput;
}
#else
#define UW_HAVE_MIDI_OUTPUT 0
#endif
