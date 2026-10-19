# uw-core milestones

## Tests
- all: ninja -C build && ctest --test-dir build --output-on-failure

## M1: All layers
- [x] `uw::ump`: MidiMessage, UMP fields, MIDI 1.0↔2.0 translation, value scaling, transposition
- [x] `uw::base`: clock, SPSC queue, denormals, audio buffer, transport time, events
- [x] `uw::plugin`: plugin seam — HostedPlugin, PluginFactory, HostServices, editors, ports (headers)
- [x] `uw::clap-host`: CLAP scan, load, process, state, editors
- [x] `uw::audio`: JackBackend (Linux), CoreAudioBackend (macOS)
- [x] `uw::midi` input: AlsaSeqInput (Linux, two UMP clients), CoreMidiInput (macOS, two ports)
- [x] `uw::midi` output: AlsaSeqOutput (Linux), CoreMidiOutput (macOS); persistent virtual source id
- [x] Tests: ump, spsc-queue, clock, denormals, clap-host, backends, alsa-output, coremidi-output

## M2: UMP endpoints (KeyboardEDO's milestone 11a)
- [x] `uw::ump` UMP Stream messages: discovery, endpoint and function block notifications, answers (`UmpStream.h`)
- [x] `MidiOutput::openEndpoint`, `setFunctionBlocks`, `inquiries`; CoreMIDI's in `CoreMidiEndpoint.mm` (macOS 15+)
- [x] The ALSA sequencer's: `AlsaSeqEndpoint`, a client of its own with the kernel's endpoint and block info, changed in place

## M3: Later
- [ ] Plugin editor windows moved here from StageHand (Umpire roadmap M3, milestone 10)
