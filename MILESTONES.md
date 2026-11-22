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

## M3: Microtonal transpose (Retune's M1)
- [x] `uw::ump`: `notePitch79`, `transposeNote79` (shift a MIDI 2.0 note by 1/512 semitone, written as Pitch 7.9), `transposePitch725` (its Pitch 7.25 partner)

## M4: Plugin output events (StageHand's note effects)
- [x] `uw::ump::fromMidi1Bytes`: one MIDI 1.0 message's bytes → MT 0x2/0x1 (the reverse of `toMidi1Bytes`)
- [x] `ClapPlugin`: a plugin's note, MIDI and MIDI 2.0 output events reach the `EventSink` as `Midi` events with their offsets and port

## M5: Parameters for a host's own controls (StageHand's generic editor)
- [x] `ParameterInfo`: hidden, read-only and module from CLAP; `HostedPlugin::paramValueText` (CLAP `value_to_text`)

## M6: Scheduled output (Umpire's milestone 7, playback)
- [x] `MidiOutput::sendAt`, `schedules`, `cancelScheduled`; `AlsaSeqOutput`'s on a sequencer queue, tagged per handle
- [ ] `CoreMidiOutput`'s: timestamps on its event lists, `MIDIFlushOutput` to cancel (destinations; a virtual source's receivers get the timestamps and may ignore them)

## M7: Plugin editor windows (Umpire's milestone 11)
- [x] `EditorWindow` moved here from StageHand (`App/EditorWindow*`): a top-level
  window hosting one plugin's editor, X11 on Linux, Cocoa on macOS. Decoupled
  from `stagehand::MainLoop`: takes a `HostServices&` for the X11 connection's
  fd watch (Linux only; Cocoa drives its own), and a `requestQuit` callback
  for Ctrl/Cmd-Q, instead of a concrete app's main loop.

## M8: Network MIDI 2.0 transport (KeyboardEDO's M10, StageHand, Umpire)
- [ ] Stream Configuration Request read and Notification answered (`UmpStream.h`); CoreMIDI and ALSA endpoints reply with the current protocol, even to a refused request (KeyboardEDO's M9)
- [ ] netmidi2 (github.com/erickampman/netmidi2, MIT, C17) as a submodule, linked as `netmidi2::netmidi2`
- [ ] A network `MidiOutput` / `MidiInput` behind the same seam as CoreMIDI and ALSA: open a session as host or client, send and receive UMP
- [ ] Discovery and Stream Configuration answered over the session (no OS converts there, so a MIDI 1.0 peer asks in-band)
- [ ] Finding peers: a typed address and port first, then mDNS / DNS-SD `_midi2._udp` (Bonjour on macOS, Avahi on Linux)
- [ ] In netmidi2: in-order delivery after retransmit, authentication; interop against Apple's network MIDI 2.0 on a real LAN

## M9: Note effects in series (Umpire's NE, then StageHand)
- [x] `uw::note-effects`: `NoteEffectChain`, CLAP note effects in series before an instrument, from StageHand's row chain (`Engine::runMidiEffects`, `ChainSink`): each effect's input in the protocol its note port takes, its output sorted and clamped into the block, a bypassed one run without MIDI, parameter changes in and out; `dialectAccepts`, `translateFor` (`test-note-effect-chain`)
- [x] `EditorWindow::setTitle` (X11, Cocoa): an effect's editor retitled when it's bypassed
- [x] StageHand's rows on `NoteEffectChain`, in place of their own chain (2026-11-22); `translateFor` now follows StageHand's rule: down to MIDI 1.0 only for a port that takes it, and a message no form of which the port takes is dropped and counted (`takeUnsupported()`), not passed on
