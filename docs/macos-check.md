# Checking uw-core on the Mac

uw-core was made on the Linux PC (2026-10-07). Its macOS code was copied
from StageHand, where it built and ran, but **uw-core has never been built on
macOS**:
- the CoreAudio and CoreMIDI backends;
- CLAP bundle loading;
- the Apple branch of `CMakeLists.txt`;
- StageHand's macOS build over uw-core (StageHand's branch `uw-core-switch`).

This page is the checklist for the first Mac session, for the author or for
Claude. Record what happens at the end of this file, and in Umpire's
`docs/roadmap.md` ("Outside Umpire's roadmap").

## Before starting

- macOS 15 or later, Apple Silicon (StageHand's macos.md, decisions 9 and 13).
- Xcode's command-line tools, and `brew install cmake ninja`.
- Access to the private repos: `erickampman/uw-core` (and StageHand) over
  HTTPS or SSH, as for StageHand's own clone.
- Paths on the Mac: StageHand is `~/Dev-MIDI/StageHand`, and the plugins are
  in `~/Dev-Audio`. uw-core has no Mac home yet. `~/Dev-Common/uw-core`
  matches the Linux PC's `~/dev/dev-common`. Put it there unless the author
  says otherwise, and note the path in this file.
- In each Mac clone with submodules (StageHand now; Umpire and the
  plugins as they get them), run once:
  `git config push.recurseSubmodules check`. Or push with
  `git push --recurse-submodules=check`. Either way, git refuses to push a
  pin whose uw-core commit isn't on GitHub yet (README.md, "Pushing:
  uw-core first").
- Everything below is silent except where marked **audible**. The author
  runs the audible steps; warn before any of them.

## 1. uw-core on its own

```bash
git clone --recurse-submodules https://github.com/erickampman/uw-core.git ~/Dev-Common/uw-core
cd ~/Dev-Common/uw-core
cmake -S . -B build -G Ninja
ninja -C build
ctest --test-dir build --output-on-failure
```

Expect it to build without warnings, and 5 suites to pass: `test-ump`,
`test-spsc`, `test-clock`, `test-denormals` and `test-clap-host`.
`test-backends` is Linux-only; nothing tests CoreAudio and CoreMIDI
directly, so StageHand (step 3) is their test.

`test-clap-host` loads the first CLAP instrument it finds in
`~/Library/Audio/Plug-Ins/CLAP` or `/Library/Audio/Plug-Ins/CLAP` and
processes a few blocks into memory. It says which plugin, or that it skipped.
An Intel-only bundle is listed as such, not loaded.

What may go wrong, and where to look:
- **Link errors for `CF...`, `Audio...` or `MIDI...` symbols:** the Apple
  frameworks in `CMakeLists.txt` (`uw-clap-host` needs CoreFoundation;
  `uw-audio` needs CoreAudio and CoreFoundation; `uw-midi` needs CoreMIDI and
  CoreFoundation). StageHand linked all of them into one library; uw-core
  splits them by target. If CoreAudio needs AudioToolbox or another
  framework, add it to `uw-audio`.
- **Warnings or errors in `CoreAudioBackend` or `CoreMidiInput`:** compare
  with StageHand's last copies (StageHand `0c7edc5`, `src/Audio` and
  `src/Midi`). uw-core changed only names (README.md, "Changes from
  StageHand's copy"), plus `setAggregateIdentity()` in `CoreAudioBackend`.
- **Deprecation warnings:** uw-core doesn't set
  `CMAKE_OSX_DEPLOYMENT_TARGET` (StageHand sets 15.0). If the SDK's default
  causes warnings, set it the way StageHand does.

## 2. uw-core as a plugin would use it

```bash
cmake -S . -B build-ump -G Ninja -DUW_CORE_UMP_ONLY=ON -DCMAKE_CXX_STANDARD=17
ninja -C build-ump && ctest --test-dir build-ump
```

Expect only `test-ump`, passing, with no CLAP or framework linking.

## 3. StageHand over uw-core

```bash
cd ~/Dev-MIDI/StageHand
git fetch
git checkout uw-core-switch
git submodule update --init --recursive
cmake -S . -B build-uw -G Ninja
ninja -C build-uw
ctest --test-dir build-uw --output-on-failure
```

Use a fresh build directory: the old one remembers the removed
`external/clap`. Expect 11 suites to pass (the other 4 moved to uw-core),
and the app to build.

Then the app, silent first:
- `build-uw/stagehand --list-midi`: MIDI sources through uw-core's CoreMIDI.
- `build-uw/stagehand --list-audio`: output devices through uw-core's
  CoreAudio.
- `build-uw/stagehand --list-plugins`: CLAP bundles through uw-core's host.
- In another app (e.g. Audio MIDI Setup's MIDI Studio, or a DAW's MIDI
  inputs), with StageHand playing: its virtual inputs should still be named
  "StageHand" and "StageHand MIDI 2.0". uw-core now gets that name from
  StageHand's `kAppName`.

Then the **audible** checks, by the author:
- `build-uw/stagehand --tone`: the test sine.
- Play a rack with an instrument and a MIDI keyboard, with the rack window
  and a plugin editor open.
- The click on a second device (the private aggregate device, now named by
  `setAggregateIdentity("StageHand", "com.unlikelyware.stagehand")`).

If something fails here but step 1 passed, look at StageHand's Apple
branch in its `CMakeLists.txt` (the `stagehand-lib` interface target) and at
`src/App/main.cpp`'s `setAggregateIdentity` calls.

## 4. Afterwards

- Fix what broke in uw-core first. Commit and push it, then move
  StageHand's pin (`external/uw-core`) to that commit and commit on
  `uw-core-switch`. Push uw-core before StageHand: with
  `push.recurseSubmodules check` set (or `git push
  --recurse-submodules=check`), git refuses StageHand's push otherwise.
- Record the result below, and in Umpire's `docs/roadmap.md` ("Outside
  Umpire's roadmap": the StageHand line). Remove the "never been built on
  macOS" note in README.md once it's built.
- The author merges `uw-core-switch` when both platforms are good.

## Results

(none yet)
