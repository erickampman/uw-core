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
| `uw::ump` | `MidiMessage` (one UMP), message classification and fields, MIDI 1.0 ↔ 2.0 translation, value scaling, transposition (Pitch 7.9 aware), the MIDI 1.0 byte parser | nothing; C++17; position-independent | the apps, the plugins |
| `uw::base` | the shared clock, SPSC queue, denormal control, audio buffers, transport time, events | `uw::ump`; C++20 | the apps |
| `uw::plugin` | the plugin seam: `HostedPlugin`, `PluginFactory`, `HostServices`, editors, parameters, ports (headers only) | `uw::base` | the apps |
| `uw::clap-host` | the seam's CLAP implementation: scanning, loading, processing, state, editors | `uw::plugin`, the CLAP SDK (submodule, 1.2.10) | the apps |
| `uw::audio` | `AudioBackend`: JACK (Linux), CoreAudio (macOS) | `uw::base`; libjack | the apps |
| `uw::midi` | `MidiInput`: ALSA sequencer UMP (Linux, alsa-lib 1.2.10+), CoreMIDI (macOS) | `uw::base`; alsa-lib | the apps |

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

The macOS sources (CoreAudio, CoreMIDI, CLAP bundles) were copied with the
rest but haven't been built in uw-core yet. The first Mac build follows
[docs/macos-check.md](docs/macos-check.md).

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
