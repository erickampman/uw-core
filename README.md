# uw-core

Code shared by unlikelyware's MIDI apps, [StageHand](https://github.com/erickampman/StageHand)
and Umpire, and (its UMP layer) by the synth and effect plugins.

Status: empty. The code moves here from StageHand in its own milestone
(Umpire's `docs/decisions.md`, "Shared code").

## Planned layers

Separate CMake targets, so each user builds only what it needs. C++
namespace `uw`.

| Target | What | Used by |
|---|---|---|
| `uw::ump` | UMP message types, decoding by message type and status, Pitch 7.9, later per-note data. No dependencies, largely header-only; `-fPIC`, hidden symbols inside plugins. | the apps, the plugins |
| `uw::clap-host` | hosting CLAP plugins | the apps |
| `uw::audio`, `uw::midi` | audio and MIDI backends behind interfaces: JACK, ALSA sequencer UMP; CoreAudio, CoreMIDI | the apps |

DPF/DGL is not here: it's planned as its own fork, beside this repo
(Umpire's `docs/decisions.md`, "DPF fork").

## Use

Each user adds this repo as a git submodule and pins a commit.
