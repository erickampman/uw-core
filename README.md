# uw-core

Code shared by unlikelyware's MIDI apps, [StageHand](https://github.com/erickampman/StageHand)
and Umpire, and (its UMP layer) by the synth and effect plugins. MIT
licensed; see [LICENSE](LICENSE).

The decisions about it are in Umpire's `docs/decisions.md` ("Shared code",
"DPF fork") and its plan in Umpire's `docs/roadmap.md` (milestone 2). DPF/DGL
is not here: it gets its own fork, beside this repo.

## Layers

Separate CMake targets, so each user builds only what it needs. C++
namespace `uw`; headers under `src/uw/`, included as `"uw/<layer>/File.h"`.

| Target | What | Needs | Used by |
|---|---|---|---|
| `uw::ump` | `MidiMessage` (one UMP), message classification and fields, MIDI 1.0 ↔ 2.0 translation, value scaling, transposition (Pitch 7.9 aware), the MIDI 1.0 byte parser, UMP Stream messages (endpoint and function block discovery and its answers: `UmpStream.h`) | nothing; C++17; position-independent | the apps, the plugins |
| `uw::base` | the shared clock, SPSC queue, denormal control, audio buffers, transport time, events | `uw::ump`; C++20 | the apps |
| `uw::plugin` | the plugin seam: `HostedPlugin`, `PluginFactory`, `HostServices`, editors, parameters, ports (headers only) | `uw::base` | the apps |
| `uw::clap-host` | the seam's CLAP implementation: scanning, loading, processing, state, editors | `uw::plugin`, the CLAP SDK (submodule, 1.2.10) | the apps |
| `uw::audio` | `AudioBackend`: JACK (Linux), CoreAudio (macOS) | `uw::base`; libjack | the apps |
| `uw::midi` | `MidiInput`: ALSA sequencer UMP (Linux, alsa-lib 1.2.10+), CoreMIDI (macOS). `MidiOutput`: ALSA sequencer UMP (Linux), CoreMIDI (macOS); a published source can be a UMP endpoint with function blocks (`openEndpoint`: CoreMIDI, macOS 15+, in `CoreMidiEndpoint.mm`, uw-core's one Objective-C++ file; the ALSA sequencer, alsa-lib 1.2.13+, a client of its own, in `AlsaSeqEndpoint`) | `uw::base`; alsa-lib | the apps, KeyboardEDO |

Every target is built with hidden symbols, so a copy linked into a plugin
can't bind to another plugin's copy in the same host.

## Using it

Add it as a git submodule and pin a commit:

```cmake
add_subdirectory(external/uw-core)
target_link_libraries(my-app PRIVATE uw::clap-host uw::audio uw::midi)
```

A plugin that needs only the UMP layer, without CLAP, JACK or ALSA:

```cmake
set(UW_CORE_UMP_ONLY ON CACHE BOOL "" FORCE)
add_subdirectory(external/uw-core)
target_link_libraries(my-plugin PRIVATE uw::ump)
```

Without the backends (no JACK or ALSA needed: libraries and tests only),
set `UW_CORE_BACKENDS` to `OFF`.

### Pushing: uw-core first

A change in uw-core that a user needs goes in two steps:
1. Commit and push uw-core.
2. Move the user's pin (`external/uw-core`), then commit and push the user.

Push the submodule's commit before the pin that points at it. Otherwise the
other machine can't fetch the pin. To have git refuse such a push:

```bash
git push --recurse-submodules=check             # this push only
git config push.recurseSubmodules check         # the default for this clone
```

The `git config` line is per clone: run it in every clone that has
submodules (StageHand, Umpire, each plugin), on the Linux PC and on the Mac.

## Building and testing

```bash
git submodule update --init
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Linux needs `libjack-jackd2-dev` (or PipeWire's JACK) and `libasound2-dev`
1.2.10 or later. The tests are silent:
- `test-clap-host` loads an installed CLAP instrument and processes a few
  blocks into memory. It skips if none is installed.
- `test-backends` checks for a JACK server without starting one, and opens
  and closes an ALSA sequencer client.
- `test-coremidi-output` (macOS) sends UMP through `CoreMidiOutput` to a
  `CoreMidiInput` in the same process, over a virtual source and virtual
  destinations of both protocols, and checks the words arrive untouched.
  It also publishes a UMP endpoint, sends discovery to its destination and
  checks the answers on its source, before and after its function block
  changes, and that its unique ID stays. Its endpoints exist only while it
  runs.
- `test-alsa-output` (Linux) does the same through `AlsaSeqOutput` and
  `AlsaSeqInput`, and also checks a destination coming and going, a burst of
  300 messages arriving in order without an overrun, and a legacy (pre-UMP)
  client receiving MIDI 1.0 as ordinary sequencer events. Its endpoint test
  reads the endpoint and block info back from the kernel, plays through a
  group's port, has a peer client subscribe and send discovery, and changes
  the blocks with both listeners still subscribed. Its clients and ports
  exist only while it runs.

The macOS sources (CoreAudio, CoreMIDI, CLAP bundles) build without
warnings in StageHand's CI (macOS 15, Apple Silicon). Nothing has run them on
a Mac yet; [docs/macos-check.md](docs/macos-check.md) is the checklist for
that.

## Where the code came from

Copied on 2026-10-07 from StageHand at `0c7edc5` (branch
`macos-milestone-6`): `src/Core` (the UMP, clock, queue, denormal, buffer,
time and event headers), `src/Plugin`, `src/Audio`, `src/Midi`, and the tests
`UmpTests`, `SpscQueueTests`, `ClockTests`, `DenormalTests`. StageHand's own
model (`Sequence`, `MidiFilter`, `MidiSourceRef`), engine, session and UI
stayed in StageHand. Its plugin editor windows (`App/EditorWindow*`) come
later (Umpire's roadmap, milestone 10).

Comments that cite `docs/...` mean StageHand's docs; milestone numbers in
comments ("measured in M2") are StageHand's.

### Changes from StageHand's copy

For StageHand's switch to uw-core:

- Namespace `stagehand` → `uw` (`stagehand::ump` → `uw::ump`, and the tests'
  `stagehand::test` → `uw::test`).
- Includes: `Core/Types.h` → `uw/Types.h`; `Core/MidiMessage.h` and
  `Core/Ump.h` → `uw/ump/`; the other `Core/` files → `uw/base/`; `Plugin/`
  → `uw/plugin/`; `Plugin/Clap/` → `uw/clap/`; `Audio/` → `uw/audio/`;
  `Midi/` → `uw/midi/`.
- The app names the host, instead of "StageHand" being built in:
  - `HostServices::hostInfo()` (new, returns a `HostInfo` with name, vendor,
    URL and version) is what a CLAP plugin is told. The default is
    "uw-core host", so StageHand overrides it.
  - `JackBackend(jackClientName, ...)`, `AlsaSeqInput::create(clientName)`
    and `CoreMidiInput::create(clientName)` no longer default to
    "StageHand", so the name must be passed.
  - `CoreAudioBackend::setAggregateIdentity(name, uidPrefix)` (new) names
    the private aggregate device. It defaults to "uw-core" and
    `com.unlikelyware.uw-core`, so StageHand calls it with "StageHand" and
    `com.unlikelyware.stagehand`.
  - JACK's server probe client is "uw-core-probe".
- New tests: `ClapHostTests`, `BackendTests`.

### Since the copy

- `MidiOutput` (2026-10-09, for KeyboardEDO): the output seam beside
  `MidiInput`, sending UMP to a destination or from a virtual source, never
  translating. `CoreMidiOutput` implements it; the ALSA sequencer's is next,
  on the Linux PC. `PlatformMidiOutput.h` defines `UW_HAVE_MIDI_OUTPUT` (0
  where there's none yet).
- `MidiOutput::openVirtual(name, protocol, persistentId)` (2026-10-09): a
  published source can keep one CoreMIDI unique ID across launches, so
  hosts such as REAPER remember it, as KeyEDO's fixed "KEDO" id does.
- `AlsaSeqOutput` (2026-10-12, KeyboardEDO's milestone 3b): `MidiOutput` on
  the ALSA sequencer, with `AlsaSeqInput`'s two UMP clients ("<name>" for
  MIDI 1.0, "<name> MIDI 2.0"), so a published source shows the protocol it
  sends. Destinations are sent to directly, without a subscription;
  published sources send to their subscribers. ALSA has no persistent id, so
  `persistentId` is ignored there. Hotplug announcements are read when
  `changes()` is called, with no thread. Each client's output pool is the
  kernel's most (2000), and a send waits at most 10 ms for a slow receiver
  to make room. `UW_HAVE_MIDI_OUTPUT` is now 1 on Linux too.
- `AlsaSeqInput`'s clients take the kernel's largest input pool (2000
  events, from ALSA's default 200), so a burst from a fast sender doesn't
  overrun and lose what was queued.
- UMP endpoints (2026-10-15 to 10-19, KeyboardEDO's milestone 11a):
  `uw::ump` Stream messages (`UmpStream.h`) and `MidiOutput::openEndpoint`,
  `setFunctionBlocks` and `inquiries`. CoreMIDI's is `CoreMidiEndpoint.mm`
  (macOS 15+), remade when the blocks change. The ALSA sequencer's is
  `AlsaSeqEndpoint`: a client of its own named after the endpoint (the
  kernel keeps one endpoint per client, on port 0), with a port for each
  group, shown in the directions of the blocks that hold it. It sets the
  kernel's endpoint and block info itself rather than with alsa-lib's
  `snd_seq_create_ump_endpoint`/`_block`, which can't change a block once
  made, so new blocks change it in place and receivers stay subscribed; the
  listeners get the new Endpoint and Function Block Info in band. A thread
  of its own answers discovery and counts it, and receivers subscribing, in
  `inquiries()`. It needs alsa-lib 1.2.13 (the info's setters); with an
  older one, such as Ubuntu 24.04's 1.2.11, `openEndpoint` publishes a plain
  source. So does `AlsaSeqOutput`'s check for a MIDI 2.0 device's MIDI 1.0
  port (`snd_seq_port_info_get_ump_is_midi1`): before 1.2.13 such a port is
  taken as MIDI 2.0. That call broke StageHand's Linux CI from 2026-10-12;
  uw-core's ALSA code now builds against 1.2.11's headers.
