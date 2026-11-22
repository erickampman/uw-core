#pragma once

// CLAP note effects (MIDI in, MIDI out: Retune, NoteGran, EDOSpread12) in
// series in front of an instrument, each one's output the next one's input
// and the last one's the instrument's, all in the same block at the same
// offsets. Built from StageHand's row chain (its docs/midi-effects.md, "The
// chain in the engine") for Umpire's plugin (its MILESTONES.md, NE); StageHand
// moves onto it later.
//
// - MIDI keeps its protocol where the next note port takes it, and is
//   translated where it doesn't: down to MIDI 1.0 for a port that takes MIDI
//   1.0 and not 2.0 (losing per-note pitch), up to MIDI 2.0 for one that
//   takes only MIDI 2.0, with an upconverter state per stream. A message no
//   form of which the port takes (a CLAP-only port, SysEx) is dropped and
//   counted (takeUnsupported()).
// - An effect's output out of time order is sorted, and an offset past the
//   block is clamped into it.
// - A bypassed effect still runs, with its own parameter changes and no MIDI,
//   and its output is dropped: the MIDI goes around it.
// - No audio is heard from an effect: one with an audio output gets a
//   scratch buffer that's never mixed. An audio input gets nothing.
// - The chain doesn't own, activate or stop its effects; the host does.
//
// Threads: prepare() on the main thread (it allocates and reads each effect's
// ports); run() on the audio thread, without allocating or locking.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "uw/base/Event.h"
#include "uw/base/TimeInfo.h"
#include "uw/plugin/HostedPlugin.h"
#include "uw/plugin/PortLayout.h"
#include "uw/ump/Ump.h"

namespace uw {

// Can a note port with `dialects` take `m` unchanged?
bool dialectAccepts(NoteDialectMask dialects, const MidiMessage& m) noexcept;

// `m` as a note port with `dialects` takes it, into `out` (at least
// ump::kMaxTranslatedMessages): unchanged if it takes it, down to MIDI 1.0 if
// it takes that and not MIDI 2.0, up to MIDI 2.0 if it takes only that (with
// `upconvert`, the stream's state). Returns the count: 0 for a message the
// upconverter absorbs (a bank select, an RPN's selection), and for one the
// port takes in no form, when `*unsupported` (if given) is set.
std::size_t translateFor(NoteDialectMask dialects, const MidiMessage& m, ump::UpconvertState& upconvert,
                         MidiMessage* out, bool* unsupported = nullptr) noexcept;

// A note effect's output that isn't MIDI (its parameter changes, from its
// own editor or modulation), for the host to follow. Audio thread.
class NoteEffectFeedback {
public:
    virtual ~NoteEffectFeedback() = default;
    virtual void effectEvent(HostedPlugin* effect, const Event& event) noexcept = 0;
};

class NoteEffectChain {
public:
    struct Stage {
        HostedPlugin* plugin   = nullptr;   // not owned; never null
        bool          bypassed = false;
    };

    // Lays out the chain for `stages`, in order, before a destination whose
    // note port `destinationPort` takes `destinationDialects`, for blocks of
    // up to `maxFrames` frames and `maxEvents` MIDI events at each stage. Main
    // thread; reads each effect's ports (portLayout()).
    void prepare(std::vector<Stage> stages, NoteDialectMask destinationDialects, std::uint16_t destinationPort,
                 std::uint32_t maxFrames, std::uint32_t maxEvents);

    bool        empty() const noexcept { return stages.empty(); }
    std::size_t size() const noexcept { return stages.size(); }
    const Stage& stage(std::size_t i) const noexcept { return stages[i]; }
    bool        contains(const HostedPlugin* plugin) const noexcept;
    // Whether every effect is bypassed (or there are none): run() then passes
    // its input on unchanged.
    bool        passesThrough() const noexcept;

    // One block: `input`, time-sorted MIDI events (any protocol; their port
    // is ignored), through the effects. `params`, if not null, holds one
    // time-sorted block of parameter changes per stage. Returns the chain's
    // output for the destination: time-sorted, translated for its dialects,
    // addressed to its port. With no effect running, that's `input` itself.
    // The result stays valid until the next run(). Audio thread.
    EventBlock run(std::uint32_t frames, const TimeInfo& time, EventBlock input,
                   const EventBlock* params = nullptr, NoteEffectFeedback* feedback = nullptr) noexcept;

    // Events dropped since the last call because a stage's buffer was full.
    std::uint64_t takeDropped() noexcept { return dropped.exchange(0, std::memory_order_relaxed); }
    // Messages dropped since the last call because the next note port takes
    // them in no form.
    std::uint64_t takeUnsupported() noexcept { return unsupported.exchange(0, std::memory_order_relaxed); }

private:
    struct StageState {
        NoteDialectMask     dialects = 0;    // of its note port 0 (0: it takes no MIDI)
        std::uint32_t       audioChannels = 0;
        std::vector<float>  audio;           // audioChannels * maxFrames, never heard
        std::vector<float*> channels;
        ump::UpconvertState upconvert;       // for what reaches it
    };

    // Collects a stage's output, translated for the next stage.
    class Sink final : public EventSink {
    public:
        NoteEffectChain*    chain     = nullptr;
        HostedPlugin*       plugin    = nullptr;
        NoteEffectFeedback* feedback  = nullptr;
        Event*              events    = nullptr;
        std::uint32_t       capacity  = 0, count = 0, frames = 0;
        NoteDialectMask     dialects  = 0;
        std::uint16_t       port      = 0;
        ump::UpconvertState* upconvert = nullptr;
        void push(const Event& event) noexcept override;
        void add(const MidiMessage& message, std::uint32_t offset) noexcept;
    };

    EventBlock translateInput(EventBlock input, std::size_t first) noexcept;
    EventBlock merge(EventBlock midi, EventBlock params, std::vector<Event>& into) noexcept;

    std::vector<Stage>      stages;
    std::vector<StageState> states;
    NoteDialectMask         destinationDialects = 0;
    std::uint16_t           destinationPort     = 0;
    ump::UpconvertState     destinationUpconvert;
    std::uint32_t           maxFrames = 0;
    std::vector<Event>      chain[2];    // the stages' outputs, in turn
    std::vector<Event>      head;        // the input, translated for the first stage
    std::vector<Event>      merged;      // a stage's input: MIDI and its parameter changes
    Sink                    sink;
    std::atomic<std::uint64_t> dropped{0}, unsupported{0};
};

} // namespace uw
