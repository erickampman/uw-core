# Building uw-core

uw-core is a library: the projects that use it build it as a submodule, and you don't need to build
it on its own to use them. Building it here runs its tests. CMake + Ninja, C++20, the same on Linux
and macOS.

## What you need

**macOS:** macOS 26 or later on Apple Silicon (arm64). Intel Macs aren't supported.

- The Xcode Command Line Tools: `xcode-select --install`. They bring clang, the macOS SDK, git and
  Python 3.
- [Homebrew](https://brew.sh), then `brew install cmake ninja`.
- Full Xcode isn't needed.

**Linux:** a C++20 compiler (GCC 11+ or Clang 14+), CMake 3.21+, Ninja, Python 3 and git. On Debian
or Ubuntu this installs them and every library the build needs:

```bash
sudo apt install build-essential cmake ninja-build pkg-config python3 git \
  libgl1-mesa-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxinerama-dev \
  libxcb1-dev libx11-xcb-dev libxcb-util-dev libxkbcommon-dev libxkbcommon-x11-dev \
  libfontconfig1-dev libasound2-dev libjack-jackd2-dev \
  libxfixes-dev libxi-dev libxss-dev libxtst-dev libwayland-dev wayland-protocols \
  libegl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev libdecor-0-dev libudev-dev \
  libdbus-1-dev libibus-1.0-dev
```

It's one list for every Unlikelyware project (the same one CI installs), so it covers more than
uw-core needs. On other distros, install the same libraries under their own names.

Of those libraries uw-core uses JACK's headers (`libjack-jackd2-dev`, or PipeWire's JACK) and ALSA's
(`libasound2-dev`). alsa-lib must be 1.2.10 or later (Ubuntu 24.04, Debian 13 or newer) for UMP in
the ALSA sequencer.

## Get the source

```bash
git clone https://github.com/erickampman/uw-core.git
cd uw-core
tools/fetch_deps.sh
```

`tools/fetch_deps.sh` checks out the git submodules at their pinned commits (the CLAP SDK). Run it
again after a `git pull` that moves, adds or re-points a submodule: a plain pull leaves them where
they were.

## Build and test

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The same three commands on both platforms. Everything goes in `build/` (ignored by git).

The tests are silent:
- `test-help-server` serves a page over loopback, fetches it and its image,
  and checks what it refuses: no token or a wrong one, `..`, a foreign Host
  (DNS rebinding), other methods, oversized and garbled requests, and that it
  listens on 127.0.0.1 only, survives a client that says nothing, and stops.
- `test-clap-host` loads an installed CLAP instrument and processes a few
  blocks into memory. It skips if none is installed.
- `test-backends` checks for a JACK server without starting one, and opens
  and closes an ALSA sequencer client. It skips if there's no ALSA sequencer
  (`/dev/snd/seq`), as does `test-alsa-output`.
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
warnings in StageHand's CI (macOS 26, Apple Silicon), and were first built and
tested on a Mac on 2026-10-08.

## Options

Pass these to the first command (`cmake -B build ... -DNAME=VALUE`).

| Option | Default | Effect |
|---|---|---|
| `UW_CORE_UMP_ONLY` | `OFF` | Build only `uw::ump` (what the plugins use): no CLAP host, JACK or ALSA. |
| `UW_CORE_BACKENDS` | `ON` | Build `uw::audio` and `uw::midi` (on Linux, needs JACK and ALSA). |
| `UW_CORE_TESTS` | `ON` when built on its own | Build the tests. The projects that use uw-core turn it off. |

## How this differs from the other projects

Every Unlikelyware project builds the same way: `git clone`, `tools/fetch_deps.sh`, the three
commands under "Build and test", and for a plugin `cmake --build build --target install-clap`. The
same package list, the same `build/` folder, the same option names (`<PROJECT>_BUILD_VST3`,
`<PROJECT>_UW_CORE_DIR` and so on). What's different here, and why:

- A library, not a product: nothing to install or run. StageHand, KeyboardEDO, StruckKit, Retune and NoteGran build it from their `external/uw-core` submodule; each has a `<PROJECT>_UW_CORE_DIR` option for building against this clone instead.
- The option prefix is `UW_CORE_`, and its options are set by the projects that use it rather than by you.
- `test-clap-host` needs an installed CLAP instrument, and skips without one.
- CI (`.github/workflows/ci.yml`) runs on `ubuntu-24.04`, the oldest Ubuntu supported, not `ubuntu-latest`, and on `macos-26`. GitHub's Linux runners have no ALSA sequencer (`/dev/snd/seq`), so `test-backends` and `test-alsa-output` are skipped there (exit code 77); they run wherever the `snd-seq` module is loaded.
