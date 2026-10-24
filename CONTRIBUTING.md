# Contributing to uw-core

uw-core is the code shared by StageHand, Umpire, KeyboardEDO, Retune, NoteGran and StruckKit: UMP
(MIDI 2.0) types, the clock and queues, the CLAP host, and the MIDI and audio backends. The README
covers the layers, building and testing.

## Before each commit

```sh
ctest --test-dir build --output-on-failure
```

The tests are silent (no audible output); keep new ones that way.

## No UI code

uw-core has no user-interface code. Each consumer chooses its own toolkit.

## Changes reach the consumers through pins

Each consumer pins uw-core as a submodule (`external/uw-core`). A change is made and committed here,
in this repo, never inside a consumer's submodule. Push it first, then move each consumer's pin to the
new commit. With `git config push.recurseSubmodules check` set once in every consumer clone, git
refuses to push a pin whose uw-core commit isn't on GitHub yet.
