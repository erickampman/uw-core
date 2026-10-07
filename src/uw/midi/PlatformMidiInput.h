#pragma once

// The MIDI input this platform's app uses. Both have the same create(),
// describe(), threadDescription() and overrunCount() for main.cpp, beyond the
// MidiInput seam.
#ifdef __APPLE__
#include "uw/midi/CoreMidiInput.h"
namespace uw {
using PlatformMidiInput = CoreMidiInput;
}
#else
#include "uw/midi/AlsaSeqInput.h"
namespace uw {
using PlatformMidiInput = AlsaSeqInput;
}
#endif
